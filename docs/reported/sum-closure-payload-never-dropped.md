# A capturing closure held in a by-value Option/Result local is released only in its simplest shapes

**Severity: low-medium (24 B per construction, in ordinary programs).**
`(let [cb (some (fn [x] (+ x k)))] ...)` mallocs the closure's env and hands
it to the sum. Before 2026-10-08 nothing released it at scope exit, unlike
the same closure let-bound on its own (`TUR_CLOSURE_DROP` at the let's end) or
stored in a by-value struct's fn field (`drop_fnfields_<T>`). An optional
callback built per call leaked its env every call.

**Narrowed 2026-10-08: the direct shapes are fixed** (`emit_expr.c`,
`let_binding_sum_closure_freeable`). Such a local now drops its live arm's
closure at scope exit, through the closure's own header drop, when it provably
holds the env's only reference:

- the init wraps a fresh capturing closure literal whose env drop is shallow
  (`closure_env_drop_is_shallow`: scalar captures, no inline C) directly in a
  one-field constructor (`(Some <closure>)`, `(Ok <closure>)`), or through a
  function whose whole body is that constructor over its one parameter
  (stdlib `some` / `ok` / `err` -- asked structurally, so a user function
  named `some` that keeps its argument is not trusted);
- every use of the local is a `match` whose non-scalar arm binders are only
  invoked (`closure_binding_escapes`), or an argument to a tag predicate whose
  whole body is a `match` returning literals (`some?` / `none?` / `ok?` /
  `err?`).

It works on both the trailing-drop path (`emit_let_value`) and the tail path
(`let_binding_push_scope_frees`), where the call's value is put in a temp
before the drop runs.

Pinned by `tests/fixtures/sum-closure-payload-dropped` (`requires.leak-check`:
`(some ...)`, `(Some ...)` written directly, an `Ok` arm next to a returned
`Err` int, `some?` then `match`, and 1000 constructions from a `while`) and
`tests/fixtures/sum-closure-payload-kept` (shapes that must keep the closure,
because something else can still reach it).

## Measured

`(defn run [k] (let [bump k ff (some (fn [x] (+ x bump)))] (match ff (Some f)
(f 41) (None) 0)))`, under ASan: 24 B leaked per call before, 0 now.

One shape was a use-after-free during development and is the reason for the
structural predicate check. `keep-opt` matches the closure out and
`vec-push!`es it, returning an `int`, yet its inferred
`nonretain_sum_param_mask` bit is set. That mask is about keeping the sum's
BOX (the RM1 / value-struct drops it serves free the box, not a closure
inside it), so it says nothing about a closure taken out of an arm.
`sum-closure-payload-kept`'s `kept-by-callee` pins it: it segfaults without
ASan if the drop ever trusts that mask again.

## What is left

Each of these still leaks the env, as before. None is freed early.

1. **The local is passed to any other function** -- `(ap ff fa)`,
   `(fmap ...)`, a user `(call-opt ff 10)`. A callee that only matches and
   calls the closure would be safe, but nothing infers that today. It needs a
   per-param "does not keep a closure taken out of this sum" mask. This is the
   shape of the 24 B env rows in
   [carrier-sum-option-boxes-have-no-owner](carrier-sum-option-boxes-have-no-owner.md)
   (`conv-defstruct-option-hkt-instance-bodies`,
   `hkt-stdlib-option-result-instances`: `(opt-val (ap ff fa))`).
2. **`unwrap` / `unwrap-or`**: the read hands the closure out as a value.
   Freeing would need the result to be only invoked, the same question as
   the arm binder, asked through the call.
3. **Lets the CPS backend emits.** A function that calls through the payload
   is CPS-colored; when its body takes the native CPS path (a `while` loop
   driving the call, as in a first draft of the fixture), its lets are lowered
   by `emit_cps_ir.c`, which has no such drop.
4. **A non-shallow closure** -- one capturing a `^mut` cell, an `rc`, a fat
   closure, or with inline C -- would need its deep drop glue and the
   ownership questions that come with it.
5. **Owning non-closure payloads** -- an `rc` in an Option. The
   `option-rc-payload-turmeric-construction` fixture says outright that
   "nothing releases an Option's payload at scope exit". Not measured here:
   rc blocks sit in a global registry, so LeakSanitizer always reads them as
   reachable.

## Fix directions

- For (1), infer a closure-aware sibling of `nonretain_sum_param_mask`: bit
  i set when the callee's arm binders of param i are only invoked and the
  param itself does not escape. `ap` / `fmap` over Option would qualify.
- For (3), give `emit_cps_ir.c`'s let lowering the same scope-exit hook the
  closure env and `any` drops already have there.
