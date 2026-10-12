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
  invoked (`closure_binding_escapes`), an argument to a tag predicate whose
  whole body is a `match` returning literals (`some?` / `none?` / `ok?` /
  `err?`), or an argument to a statically known callee whose body does the
  same with its parameter (`fn_param_keeps_no_payload_closure`, memoized per
  FnDef, recursion read as "keeps"; the callee must have no inline C and a
  runtime-pure row with no `await`, so a continuation captured inside it
  cannot call the closure after the caller's `let` has freed it). Option's
  `ap` instance qualifies, and so do user helpers that match and call.
- the payload closure itself cannot suspend either (same test).

It works on both the trailing-drop path (`emit_let_value`) and the tail path
(`let_binding_push_scope_frees`), where the call's value is put in a temp
before the drop runs.

Pinned by `tests/fixtures/sum-closure-payload-dropped` (`requires.leak-check`:
`(some ...)`, `(Some ...)` written directly, an `Ok` arm next to a returned
`Err` int, `some?` then `match`, and 1000 constructions from a `while`) and
`tests/fixtures/sum-closure-payload-kept` (shapes that must keep the closure,
because something else can still reach it: returned, an arm returning it,
callees that store it, return the Option, recurse, or are inline C, and an
alias).

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

1. ~~**A dictionary-dispatched callee inside a generic.**~~ **Fixed
   2026-10-12.** The call's `fn_binding` names a representative instance,
   not the one that runs, so only a statically dispatched call was asked.
   The payload walk (`sum_closure_payload_escapes`, `src/compiler/emit_expr.c`)
   now re-resolves such a call under the active spec
   (`emit_reresolve_method_fndef`, as the call itself is emitted) and asks
   the instance method that runs; inside a callee's own memoized question it
   does not (the active spec is the caller's there), so that answer stays
   conservative. 2400 B in 100 calls before, 0 now. Pinned by
   `sum-closure-payload-dict-dispatch` (leak-checked) and
   `sum-closure-payload-dict-dispatch-kept` (one instance keeps the closure
   in a global vector; called after the generic returned). Found on the way,
   pre-existing on `main`: that program did not LINK -- its two instances
   are `defopaque :int`, so the generic's specs are Gap H siblings with one
   C signature, and a CPS-emitted caller's cps->direct call tied between them
   and fell back to the never-emitted base (`find_mono_clone_for_call`,
   `src/compiler/emit_cps_ir.c`, now breaks the tie with the clone the
   emitter recorded for that call). (Before 2026-10-08 this item also
   covered every statically known callee; a first version of this report
   pointed at the `:int`-reader rows of carrier-sum-option-boxes-have-no-owner
   as this shape. They are not: those fixtures erase the closure to `int`
   themselves.)
2. ~~**`unwrap` / `unwrap-or`**: the read hands the closure out as a value.~~
   **Fixed 2026-10-12.** A callee that only PROJECTS the payload keeps nothing
   (`fn_projects_payload_of`, `src/compiler/emit_expr.c`). Its whole body is a
   `match` on that parameter. Each arm answers either its own binder or
   something that names neither the parameter nor any binder. The callee
   cannot suspend, and its inline C never spells the parameter's name.
   `unwrap`, `unwrap-or` and `ok-val` qualify; `unwrap`'s `(default-of A)` arm
   is inline C that names nothing of the Option. The question then moves to
   the call's result. If it is invoked on the spot (`((unwrap ff) 41)`), or
   bound by a `let` (sibling or nested) to a local that is itself only invoked
   (`closure_binding_escapes`), the closure goes nowhere and the local still
   drops it. 2400 B in 100 calls before, 0 now. Pinned by
   `sum-closure-payload-unwrap-dropped` (leak-checked: on the spot, sibling
   let, nested let, `unwrap-or`, `ok-val`, 100 turns). `sum-closure-payload-kept`
   gained the must-keep half: the projected closure returned, pushed into a
   vector (directly, through a nested let, from `unwrap-or`), each called after
   the heap has been reused. Found on the way, pre-existing on `main`: the
   named `plus1` in `(unwrap-or ff plus1)` leaked a 24 B box per call, as does
   every named defn passed into ANY generic's type-variable slot (`(pick plus1
   plus1)`: two). The box is a link-time constant, so that slot now asks for
   the shared static one (`static_ok`, `src/compiler/elab_call.c`), except
   for a `^construct` callee, whose sum payload is what this report's drops
   free (GCC flags a static address reaching `free` there). Its no-op header
   makes a later drop harmless. Pinned by `fn-value-at-tyvar-slot-static-box`
   (leak-checked: called on the spot, let-bound, in a struct field, as a
   `some` payload; 9696 B in 404 allocations before, 0 now).
3. ~~**Lets the CPS backend emits.**~~ **Fixed 2026-10-09.** In a function
   the CPS backend lowers (its body performs), the let's `(some <closure>)` is
   a cps->direct call (`CT_LETCALL`); the IR builder names the `let` on it
   (`sum_let`, `cps_bind_let_init`) and the emitter asks the direct emitter's
   question (`emit_let_binding_sum_closure_freeable`) with one relaxation --
   a `perform` in the let is not an escape of the arm binder
   (`closure_binding_escapes_reaped`), since the free waits for the outermost
   DK entry's exit, after every resume the entry sees -- and registers the
   live arm's closure with the entry boundary's reap under a tag switch
   (`emit_letcall_sum_closure_reap`, `__dk_reap_closure`), as a non-escaping
   closure env or a struct's fn fields are there. 2400 B in 100 allocations
   before, 0 now. Pinned by `tests/fixtures/sum-closure-payload-dropped-cps`
   (leak-checked: performed in the arm, in a sibling binding, an `Ok` arm, a
   `some?` test, 200 times from a performing loop). Found on the way, not
   this report's: `(let [ff (Some (fn ...))] (match ff (Some f) (f (perform
   ...))))` -- the constructor written directly -- evicts the function from
   the CPS backend ("indirect call (non-atomic args)"), where `(some ...)`
   does not. **Fixed 2026-10-12:** a constructor has no FnDef, so the call
   took the fn-VALUE route (`delegate_call_atomized`, `src/passes/cps_ir.c`),
   which refused any non-scalar argument it had to atomize, and a capturing
   closure is never safe to delegate. A constructor's `fn`-typed argument is
   now atomized as one word, as the arguments of `(some ...)`'s cps->direct
   call are; the delegated raw let names its `let` too (`letraw.sum_let`), so
   the live arm's closure gets the same reap (`emit_sum_let_closure_reap`),
   and the constructor spelling leaked 2376 B in 99 constructions before
   that, 0 after. `sum-closure-payload-dropped-cps` gained `(Some ...)`, an
   `(Ok ...)` arm and a 200-turn loop of the constructor spelling.
4. **A closure whose drop releases something** -- one capturing an `rc`, a
   fat closure or a Drop instance, or with inline C -- would need its deep
   drop glue and the ownership questions that come with it. (**Narrowed
   2026-10-09:** a closure whose env drop frees the box alone is dropped now,
   `closure_env_drop_frees_box_only` -- a struct capture is a copy in the box,
   and the glue releases only rc, owned fat and Drop-instance captures.
   Pinned by `sum-closure-payload-dropped`'s `struct-cap`. A closure capturing
   a `^mut` cell drops its env box too, and the 8 B cell is freed at the same
   exit: `mut_cell_escapes` and `let_binding_mut_cell_freeable` vouch for a
   closure wrapped in a fresh sum the let drops (`let_init_sum_payload_closure`)
   as they do for one bound directly -- `mut-cap`, 800 B in 100 allocations
   before, 0 now.)
5. **Owning non-closure payloads** -- an `rc` in an Option. The
   `option-rc-payload-turmeric-construction` fixture says outright that
   "nothing releases an Option's payload at scope exit". Not measured here:
   rc blocks sit in a global registry, so LeakSanitizer always reads them as
   reachable.

## Fix directions

- For (3), give `emit_cps_ir.c`'s let lowering the same scope-exit hook the
  closure env and `any` drops already have there.
