---
title: Closure-returning generic with an inner fat-dispatch cannot be float-specialized (inner result types erased)
category: Bug Report
description: Stage B+C of poly-closure-result-specialization clones a generic defn's inner closure body per monomorphization so a float result is register-class-correct. It works for a DISPATCH-FREE inner body (e.g. `(fn [t] : A val)`), but NOT when the inner body fat-dispatches a captured closure (e.g. `>>>`'s `(fn [x] (g (f x)))`): the intermediate result types are already lowered to the int64 carrier in the elaborated body, so emit_resolve_type cannot recover their float register class. This blocks Stage E (generalizing stdlib `>>>`).
---

# Closure-returning generic + inner fat-dispatch: inner result types erased

> **Status:** RESOLVED (2026-06-06). Stage E of
>   `docs/upcoming/poly-closure-result-specialization-plan.md` landed: `>>>`
>   rewritten to the polymorphic typed form; Direction 3 recovery in
>   `emit_expr.c` (e->type carries TY_TYVAR("C") which resolves to float through
>   the spec bindings) makes the inner fat-dispatch register-class-correct. Full
>   suite green. Archived to `docs/archive/history/`.
> **Severity was:** expressiveness gap, guarded against miscompile.

## Summary

`poly-closure-result-specialization` (Stages B+C) makes a polymorphic
closure-returning `defn` specialize its lifted inner closure body per
monomorphization, so a **float** result type is register-class-correct (xmm0)
instead of dispatched through the shared int64 thunk (rax). This is implemented
in emit (`emit_module.c`: `emit_inner_closure_needs_float_spec`, the inner-spec
interning, `EmitAbiSpecialization.env_name_override` / `inner_closure_spec_idx`),
with per-spec type resolution threaded through `emit_resolve_type` in
`emit_fns.c` (inner clone signature + env struct) and `emit_expr.c` (outer
`EX_CLOSURE` thunk + fat-dispatch typedef).

It works end-to-end for a **dispatch-free** inner body -- one that returns a
captured value directly:

```turmeric
(defn constant [A] [val : A] : ptr<void>
  (fn [t : float] : A val))      ; inner body is just `val`
```

Here every float-relevant type is recoverable: the inner fn's declared result
(`A`), its params, and its captured env fields all resolve through the spec
bindings. Fixture `tests/fixtures/poly-closure-result-tyvar-float` exercises
this and is register-class-correct (verified with fractional probes, e.g.
`3.675`, not just integer-valued doubles).

It does **not** work when the inner body **fat-dispatches a captured closure**:

```turmeric
(defn >>> [A B C]
  [^fat f : (fn [A] B) ^fat g : (fn [B] C)] : ptr<void>
  (let [fv f gv g] (fn [x : A] : C (gv (fv x)))))   ; inner body dispatches fv, gv
```

## Root cause

By the time the inner closure body `(gv (fv x))` reaches emit, its
**intermediate** result types are already lowered to the int64 carrier in the
elaborated AST. The inner `EX_CALL` for `(fv x)` has `e->type == TY_INT` (the
carrier), **not** a `TY_TYVAR B`. The captured `fv` / `gv` bindings are typed
`ptr<void>` (a `^fat` carrier box), carrying no result-type structure either.

The Stage B/C machinery resolves float types via `emit_resolve_type`, which can
only substitute a **named** `TY_TYVAR`. With the intermediate node already a
bare `TY_INT`, there is nothing to resolve:

```c
/* emit_expr.c fat-dispatch (legacy branch) */
Type _disp_result = emit_resolve_type(ctx, e->type);  /* e->type == TY_INT -> stays int64 */
```

So the inner clone `__fn_N__spec__double` still emits
`int64_t (*)(void*, int64_t)` dispatch casts even though the closures it calls
are `double(void*,double)` -- a register-class miscompile (observed: garbage
like `9.39866e+13`, a pointer reinterpreted as a double).

The float info exists only in (a) the spec bindings (`A=B=C=float`) and (b) the
inner closure's **declared** signature (`(fn [x:A] : C)`); the **per-call-site
intermediate** `B` is genuinely gone from the body. Recovering it needs more
than a type substitution.

## Minimal repro

```turmeric
(defn cmp [A B C] [^fat f : (fn [A] #{} B) ^fat g : (fn [B] #{} C)] : ptr<void>
  (let [fv f gv g] (fn [x : A] : C (gv (fv x)))))
(defn call-float [^fat f : (fn [float] #{} float) x : float] : float (f x))
(defn fadd  [x : float] : float (+ x 0.25))
(defn fhalf [x : float] : float (* x 0.5))
(defn main [] : int
  (println (call-float (cmp fadd fhalf) 7.1))  ; want (7.1+0.25)*0.5 = 3.675
  0)
```

Note: as written this passes today only because `cmp`'s `^fat` fn-typed params
do not propagate `A/B/C` bindings to the call site (see "Interaction" below), so
no spec is interned and the call routes through the int64 base body, which
"works by SysV register-class accident" (the original
`arrow-compose-float-closure-int64-thunk-mismatch.md`). The moment the bindings
*are* propagated and a spec is interned, the inner dispatch miscompiles.

## Observed vs expected

- **Observed (if specialized):** inner clone dispatches `(fv x)` / `(gv ...)`
  through `int64_t(*)(void*,int64_t)` while the targets are `double(...)`;
  runtime garbage.
- **Expected:** the inner clone dispatches through `double(*)(void*,double)`
  (xmm0), composing the float closures correctly.

## Current guard (no silent miscompile)

`Binding.closure_return_dispatches` is set during elaboration
(`expr_closure_return_dispatches` in `elab_core.c`, computed where
`returns_closure_fn_binding` is assigned in `elab_fns.c`). It walks the returned
closure's body for an `EX_CALL` through a non-global `ptr<void>`/`TY_FN`
binding (a captured-closure fat dispatch).

- **emit** (`emit_module.c`): the inner float-spec trigger is gated on
  `!fn_binding->closure_return_dispatches`, so a dispatching inner body is never
  given a (broken) clone.
- **elab** (`elab_call.c`): the retained `TUR-E0705` fires for a float binding
  of a closure-returning generic **only** when
  `closure_return_dispatches` is true -- a hard error directing the user to a
  monomorphic per-type defn, instead of a silent miscompile.

So: dispatch-free float closures work; dispatching float closures are a hard
error; the int/cstr/ptr cases are unaffected (they share the integer carrier).

## Interaction: fn-typed `^fat` params do not bind the result tyvar

A separate, smaller gap compounds this for `>>>`/`cmp`-shaped callees: a
`^fat f :(fn [A] B)` parameter loses its nested tyvars. A bare type-param name
inside a `(fn ...)` type resolves to a *nameless* `TY_STRUCT(def==NULL)`
placeholder (`elab_types.c`, the type-param branch), so `(fn [A] B)` is stored
with `arg_full_types == NULL` / `result_full_type == NULL`. Consequently
`call_collect_type_bindings` records **no** bindings for such a param, the call
carries no `abi_bindings`, and neither the spec nor the E0705 guard engages --
the call silently routes through the int64 base body (lucky on SysV).

A spike fix (re-stamping those placeholders as named tyvars in
`type_expr_from_form`, plus a `TY_FN` case in `call_type_has_named_tyvar` and a
fn-typed branch in the call binding-collection) *does* propagate the bindings
and trigger the spec -- which then immediately hits the inner-dispatch erasure
above. That spike was reverted to avoid shipping the miscompile; it is the
natural first half of the real fix once the body-level erasure is solved.

## Proposed fix directions

1. **Per-spec re-elaboration of the inner body.** Re-run elaboration (or a
   type-refinement pass) on the lifted inner closure body under the concrete
   spec bindings so each intermediate `EX_CALL`'s `e->type` carries the concrete
   float instead of the erased carrier. Heaviest, but fully general.
2. **Retain tyvar types on inner-body sub-expressions.** Stop lowering
   intermediate result types of a generic closure body to the int64 carrier
   during the first elaboration; keep the `TY_TYVAR` so `emit_resolve_type` can
   substitute per spec. Risk: broad codegen churn wherever generic bodies rely
   on the carrier lowering.
3. **Derive the dispatch result type from the captured closure's resolved
   type.** When the dispatched value is a captured binding whose *declared*
   full type is `(fn [..] R)`, resolve `R` through the spec bindings instead of
   reading the erased `e->type`. Handles the common `^fat`-typed-closure case
   (`fv:(fn [A] B)`) but requires the param's nested tyvars to survive (the
   "Interaction" fix above) so `R` is a named tyvar.

Direction 3 + the fn-typed-param tyvar preservation is the most targeted path to
unblocking `>>>` specifically.

## How to validate a fix

- `tests/fixtures/poly-closure-result-tyvar-float` must stay green (dispatch-free
  case).
- A new fixture composing two `:float -> :float` closures through the
  generalized `>>>` must print exact fractional results (e.g. `3.675`, `3.8`)
  and its `expected.c` must show the inner clone dispatching through
  `tur_thunk_double_double_t` / `double(*)(void*,double)`, not `int64_t`.
- `bash tests/run.sh`: 0 FAIL, with all `expected.c` snapshots regenerated.
- The retained TUR-E0705 guard can then be removed for the now-handled shape.
