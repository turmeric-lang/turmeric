---
title: Polymorphic defn returning a typed closure shares one inner body across monomorphizations
category: Bug Report
description: A polymorphic defn that returns an inner `(fn ...)` whose result type depends on the type parameter A emits *one* shared inner C function. When the outer defn is specialised at multiple Ts the inner function is dispatched through differently-typed function pointers (e.g. `double (*)(void*, double)` vs `int64_t (*)(void*, double)`) but the actual body only has one C-level return type. The mismatch silently yields garbage for the non-default specialisation.
---

# Polymorphic defn shares one inner closure body across monomorphizations

> **Status:** RESOLVED for the **dispatch-free** inner-body shape (2026-06-05)
>   via fix directions 1 + 2 (per-monomorphization inner-closure specialization);
>   the **dispatching** shape remains a hard error (see below). For a
>   dispatch-free inner body (one that returns a captured value, e.g.
>   `(fn [t] : A val)`), the emit phase now clones the lifted inner closure body
>   per concrete result type, so a closure-returning generic `defn` specialized at
>   a **floating-point** type is register-class-correct end to end (xmm0 dispatch
>   + double-typed env field) instead of being rejected. The machinery lives in
>   `src/compiler/emit_module.c` (`emit_inner_closure_needs_float_spec` interns an
>   inner-body spec alongside the outer spec;
>   `EmitAbiSpecialization.env_name_override` / `inner_closure_spec_idx` link them
>   and give the float layout its own env struct), with the per-spec type
>   resolution threaded through `emit_resolve_type` in `emit_fns.c` (inner clone
>   signature/env) and `emit_expr.c` (outer `EX_CLOSURE` thunk + fat-dispatch
>   typedef). `cstr`/`ptr`/`int` specializations still round-trip through the
>   shared integer carrier with byte-identical codegen (the clone fires only for
>   float-class tyvars). The former error fixture is now the passing
>   codegen+stdout fixture `tests/fixtures/poly-closure-result-tyvar-float`
>   (prints `440\n440\n1`). Implemented per
>   `docs/upcoming/poly-closure-result-specialization-plan.md` (Stages B+C+D).
>
>   **Still a hard error (TUR-E0705), now narrowed:** a closure-returning generic
>   whose inner body **fat-dispatches a captured closure** (e.g.
>   `(fn [x] (gv (fv x)))`, the `>>>`/`cmp` shape) cannot be float-specialized --
>   its intermediate result types are erased to the int64 carrier in the
>   elaborated body. The guard now fires for **only** that case (keyed off
>   `Binding.closure_return_dispatches`), not the dispatch-free case above. Root
>   cause + fix directions: `docs/reported/poly-closure-inner-dispatch-result-erased.md`.
>   `bash tests/run.sh`: 0 FAIL (1524 passed).
> **Found:** surfaced by spike G2 of `language-readiness-for-typed-signal-plan`.
> **Severity:** silent miscompile (no diagnostic, wrong runtime result); now
>   fixed for the dispatch-free float case and a hard error for the dispatching
>   float case.

## Summary

Severity: **silent miscompile** (no diagnostic, wrong runtime result).

When a polymorphic defn returns an `(fn ...)` whose declared result type is a
type parameter `A`, the compiler emits a *single* C function for the inner
closure body whose C-level return type is fixed (currently `int64_t`). The
outer defn is monomorphised correctly (one per concrete `A`) and each
monomorphisation hands out a fat-closure box whose dispatch pointer types
the call as `A (*)(void*, ...)`. For `A = :int` the dispatch type matches the
body, the call works. For `A = :float`, the call site reads the return from
the xmm0 register while the body wrote to rax -- the float reader sees the
*parameter* (which also lives in xmm0) instead of the captured value.

This was surfaced by spike G2 of
`docs/upcoming/language-readiness-for-typed-signal-plan.md`.

## Minimal repro

```turmeric
;; tests/fixtures/signal-constant-poly/input.tur (now removed; repro lives here)

(defn constant [A] [val : A] : ptr<void>
  (fn [t : float] : A val))

(defn call-float [^fat f : (fn [:float] #{} : float) t : float] : float (f t))
(defn call-int   [^fat f : (fn [:float] #{} : int)   t : float] : int   (f t))

(defn main [] : int
  (let [k440  (constant 440.0)
        kflag (constant 1)]
    (println (call-float k440  0.0))     ;; expected 440, got 0
    (println (call-float k440  0.5))     ;; expected 440, got 0.5
    (println (call-int   kflag 0.0)))    ;; expected 1,   got 1 (matches by ABI accident)
  0)
```

Observed output:

```
0
0.5
1
```

Expected output:

```
440
440
1
```

## Root cause sketch

`tur emit-c` on the repro produces:

```c
struct __env_860 { int64_t __fn; int64_t val; };

/* one shared body -- returns int64_t */
static int64_t __fn_858(void * __env_p_861, double t) {
    struct __env_860 *__env___env_860 = (struct __env_860 *)__env_p_861;
    return __env___env_860->val;
}

/* int specialisation */
static void * constant(int64_t val) {
    struct __env_860 *__t25 = malloc(sizeof(*__t25));
    __t25->__fn = (int64_t)(intptr_t)__fn_858;   /* <-- same fn pointer */
    __t25->val = val;
    return __t25;
}

/* float specialisation */
static void * constant__spec__void___double(double val) {
    struct __env_860 *__t28 = malloc(sizeof(*__t28));
    __t28->__fn = (int64_t)(intptr_t)__fn_858;   /* <-- same fn pointer */
    __t28->val = val;                            /* <-- env field is int64_t! */
    return __t28;
}

/* float dispatcher */
static double call_float(int64_t f, double t) {
    return (*(tur_thunk_double_double_t *)((void *)f))((void *)f, t);
}
```

Two compounding issues:

1. **One inner body for both specialisations.** The compiler reuses
   `__fn_858` (declared returning `int64_t`) for both `constant`'s int and
   float specialisations. The float caller invokes it through a function
   pointer typed `double (*)(void*, double)`, which is UB on the calling
   convention: rax was set, xmm0 was not. The reader gets xmm0 (the
   parameter `t`), explaining why both float calls return their argument.

2. **`val` env field typed `int64_t`.** Even with a per-A specialisation of
   the body, `__env_860::val` is `int64_t` so the float `val=440.0` is
   bit-reinterpreted on store (`__t28->val = val;` is an implicit
   `double`->`int64_t` conversion -- here the value is small enough that the
   int conversion of `440.0` is `440`, which would have *happened to work*
   if the body had matched).

This is the same family as the original
`docs/archive/history/cstr-returning-closure-thunk-int64-return.md` and
`docs/archive/history/bare-fat-param-non-int-result-miscompiles.md`
fixes, generalised to the polymorphic-return case.

## Proposed fix directions

1. **Per-A specialisation of the inner body.** When the outer defn is
   monomorphised, also clone its captured `(fn ...)` bodies so each
   specialisation has a C function whose return type matches `A`.
2. **Type the env field by A.** The `val` field on the closure environment
   should be `A`, not `int64_t`. This is the same change as the field-typing
   pass that landed for closure-returning instance methods in #205, but
   extended to env fields whose type comes from a generic outer parameter.
3. **Reject the construct at elaboration if (1) is too invasive.** A clear
   error -- "polymorphic closure-returning defn: result type depends on a
   type parameter; not yet supported, work around by writing one defn per
   A" -- is strictly better than the current silent miscompile.

## Validation

A fix is valid when the repro above prints `440\n440\n1\n` under both
Debug and Release builds. The fixture directory
`tests/fixtures/signal-constant-poly/` is currently a stub; once a fix
lands, re-add the input.tur and an `expected.stdout` of `440\n440\n1\n`.

## Cross-references

- Surfaced by [[language-readiness-for-typed-signal-plan]] gap G2.
- Related fixes: closure-returning-instance-method-codegen-plan (archived),
  bare-fat-result-type-inference-plan (archived).
