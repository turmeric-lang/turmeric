# Constrained-generic function as a value bakes the carrier representative instance

> **RESOLVED 2026-06-15.** All three reachability forms now dispatch the
> receiver's real instance on **both** backends (`7` for a `Box`, `-1` for the
> `int` representative), with **zero** snapshot churn beyond the single benign
> regen noted below. Each fix *reuses the already-correct direct-call GDE path*
> rather than adding machinery to the delicate monomorphization emitter:
>
> 1. **`let`-bound alias** -- `(let [g count-it] (g box))`. `elab_call.c` follows
>    `Binding.source_binding` when resolving a call head, so an immutable alias of
>    a global function elaborates to a **direct** call and the existing
>    per-call-site specialization fires. Fixture:
>    `tests/fixtures/gde6-generic-dict-alias-call/`. (One snapshot,
>    `macro-quasiquote-unquote`, regenerated -- an alias call there is now a
>    direct call, behaviour unchanged.)
>
> 2. **Bidirectional inference** -- `(apply-fn (fn [b] (count-it b)) box)`. The
>    call site pushes the parameter's concrete fn type onto the expected-type
>    channel while elaborating a lambda arg, and `elab_fn` types an UN-annotated
>    param from that expected type (gated to non-primitive struct/adt/app types,
>    so primitive lambdas are untouched -- zero churn). `b : Box`, so the inner
>    direct `(count-it b)` pins `Size [Box]`. (Previously `-1` even *with* a
>    lambda unless the param was explicitly annotated `: Box`.)
>
> 3. **Eta-expansion of a bare coercion** -- `(apply-fn count-it box)`.
>    `try_eta_expand_generic_fn_arg` (`elab_call.c`) rewrites a bare
>    constrained-generic global fn passed where a concrete fn type is expected
>    into `(fn [g] (count-it g))`; fix (2) types `g`, and the body is a direct
>    call. Gated to the dispatch-relevant case (a tyvar param pinned to a
>    non-primitive concrete type) and to simple fns (no substructural/variadic/
>    fat/rank-2 params), so nothing else is rewritten -- zero churn.
>
> Combined regression fixture for (2) and (3):
> `tests/fixtures/gde7-generic-dict-coerced-fn-arg/`. Validation: compiled suite
> **1649 passed / 0 failed**; turi harness **1208 passed / 2 failed** (the 2 --
> `eq-carrier-capturing-comparator`, `mutmap-eq` -- are pre-existing). The
> `let`-alias resolution shipped first (commit history); fixes (2) and (3) closed
> the coerced-parameter case and the related inference gap together.
>
> Original report follows.

**Summary:** When a class-constrained generic function (e.g.
`(defn count-it [^Size A] [x :A] :int (size x))`) is reached **indirectly** --
bound to a local (`(let [g count-it] (g box))`) or coerced to a concrete
function type and passed to a higher-order function -- the emit-side
generic-dict specialization (GDE1/GDE2) does **not** fire, so the *base clone*
bakes the carrier representative instance (`Size [int]`) and the call silently
returns the wrong instance's answer. `(g (make-struct Box 0))` returns `-1`
(the `Size [int]` constant) instead of `7` (`Size [Box]`), at **rc=0** on the
**compiled** path. The direct call `(count-it box)` is correct (`7`) on both
paths.

**Severity:** Medium-High. A **silent wrong-value miscompile** (rc=0, no
diagnostic) on the **compiled** path -- the most dangerous class -- for ordinary
code (binding a constrained generic to a `let` and applying it). Unlike
[../archive/turi-generic-dict-dispatch-bakes-representative-instance.md](../archive/turi-generic-dict-dispatch-bakes-representative-instance.md)
(which was `--interpret`-only), this one is **compiled-path** and, in the
`let`-bound case, the **interpreter is now the correct side** -- so the two
backends also *disagree*.

## Repro matrix

```turmeric
(defclass Size [a] (size [x] : int))
(definstance Size [int] (size [x] -1))          ; carrier representative
(defstruct Box [n : int])
(definstance Size [Box] (size [x : Box] 7))     ; pure-Turmeric, distinct answer
(defn count-it [^Size A] [x : A] : int (size x))  ; generic-dict driver
```

Status as of the 2026-06-15 partial fix (was: both indirect rows `-1` on
compiled; the `let`-bound row also diverged interp-vs-compiled):

| Application form | `--interpret` | `tur run` (compiled) | correct | status |
| --- | --- | --- | --- | --- |
| `(count-it (make-struct Box 0))` -- direct | `7` | `7` | `7` | OK |
| `(let [g count-it] (g (make-struct Box 0)))` -- local binding | `7` | `7` | `7` | **fixed** |
| `(apply-fn count-it (make-struct Box 0))` -- coerced + indirect | `-1` | `-1` | `7` | **open** |
| `(let [g count-it] (g 42))` -- local binding, primitive | `-1` | `-1` | `-1` | OK |

Original (pre-fix) matrix:

| Application form | `--interpret` | `tur run` (compiled) | correct |
| --- | --- | --- | --- |
| `(count-it (make-struct Box 0))` -- direct | `7` | `7` | `7` |
| `(let [g count-it] (g (make-struct Box 0)))` -- local binding | `7` | **`-1`** | `7` |
| `(apply-fn count-it (make-struct Box 0))` where `apply-fn : (fn [(fn [Box] int) Box] int)` -- coerced + indirect | **`-1`** | **`-1`** | `7` |
| `(let [g count-it] (g 42))` -- local binding, primitive | `-1` | `-1` | `-1` |

(The primitive `int` argument always correctly resolves to the representative,
which *is* `Size [int]` -- so the negative path is fine on both backends.)

## Observed vs. expected

- **Observed (compiled, `let`-bound, Box):** `-1` -- `count_hyit`'s base clone
  emits `return __inst_Size_size_int(x);` (the representative), and the
  indirect call through `g` never reaches a GDE-specialized callee.
- **Observed (both backends, coerced + indirect):** `-1` -- same base clone via
  the function-typed parameter.
- **Expected:** `7` -- coercing/binding `count-it` at a site where `A = Box` is
  statically known should pin the `Size [Box]` dictionary, exactly as the direct
  `(count-it box)` call does.

## Root cause (direction)

The compiled generic-dict path specializes **per direct call site**:
`emit_module.c` GDE1 (the `:spices`-free scan around `emit_module.c:846` /
`:1368-1371`) interns a specialization "when ... the base clone bakes the
representative (int-carrier) instance," and `emit_core.c`'s
`emit_reresolve_method_call` (GDE2, `emit_core.c:971-1054`) re-resolves a
`TY_APP` receiver to the head-constructor instance. Both are keyed on a
**direct** `(count-it <arg>)` call where the elaborator knows the receiver type.

When `count-it` is instead **captured as a value** (`(let [g count-it] ...)`,
passed to a HOF, stored in a struct/vec), the application site is `(g <arg>)` /
an indirect `tur_poly_fn_t` apply -- there is no named-callee call for GDE to
specialize, so the **base clone** is used, and the base clone bakes the
representative (confirmed in emitted C: `count_hyit` -> `__inst_Size_size_int`).
The fix direction is to resolve the dictionary at the **coercion/capture site**
(where `A` is pinned -- `g`'s declared type is `(fn [Box] int)`), either by
specializing the captured function or by capturing the concrete dictionary, so
the value carries `Size [Box]` rather than the representative. Reject-or-resolve:
if neither is feasible, binding a still-polymorphic constrained generic to a
monomorphic-typed slot should be a hard error, not a silent representative bake.

## Interpreter interaction (why the `let`-bound row diverges)

The interpreter's runtime generic-dict re-resolution
(`frame_record_abi`/`gde_reresolve_method`, added in
[../archive/turi-generic-dict-dispatch-bakes-representative-instance.md](../archive/turi-generic-dict-dispatch-bakes-representative-instance.md))
fires off the call's `abi_bindings`. The elaborator still attaches
`{A -> Box}` to `(g (make-struct Box 0))` (it resolves `g` to `count-it`), so
the interpreter re-resolves to `Size [Box]` and returns the **correct** `7`.
The compiled path discards that and bakes the representative, so the two
backends now disagree on this form. The **coerced + indirect** row (`apply-fn`)
strips the binding behind an opaque `(fn [Box] int)` parameter, so the
interpreter call `(f b)` carries no `abi_bindings` either -- and both backends
fall back to the representative.

This means a fixture over the `let`-bound form **cannot** be added to
`tests/run-turi.sh` as-is: the interpreter would emit `7` while the
compiled-path `expected.stdout` is the (wrong) `-1`, so the harness would flag a
mismatch. The regression fixture for the resolved interpreter bug
(`tests/fixtures/gde5-generic-dict-reresolve/`) deliberately uses the **direct**
call form, where both backends agree on `7`.

## Validation (after a fix)

- `(let [g count-it] (g (make-struct Box 0)))` returns `7` under **`tur run`**
  (currently `-1`), matching `--interpret`.
- `(apply-fn count-it (make-struct Box 0))` returns `7` under **both** backends
  (currently `-1`).
- The primitive negative path (`(g 42)` -> `-1`) is unchanged on both backends.
- Add a `tests/fixtures/` snapshot of the emitted C for the `let`-bound form
  asserting the captured callee resolves to `__inst_Size_size_Box` (not
  `__inst_Size_size_int`) at the application site, and a run fixture once both
  backends agree on `7` so it can join the turi harness.

## Status

Found while validating the fix for
[../archive/turi-generic-dict-dispatch-bakes-representative-instance.md](../archive/turi-generic-dict-dispatch-bakes-representative-instance.md)
(the `--interpret` generic-dict non-specialisation, now resolved). That fix
covers **direct** generic calls under the interpreter; this report captures the
**indirect / value-captured** case, which is a **compiled-path** GDE gap (and a
both-backends gap for the concrete-fn-type coercion) rather than an
interpreter-only defect. Filed separately because the fix lives in the emitter
(`emit_module.c` / `emit_core.c`), not in `src/turi/eval.c`.
