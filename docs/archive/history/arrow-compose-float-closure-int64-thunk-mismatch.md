---
title: stdlib `>>>` (and untyped closure-composition) dispatches float closures through int64 thunk pointers
category: Bug Report
description: The untyped `>>>` combinator in stdlib/arrow.tur emits an int64-only inner closure body. When its argument closures are `:float -> :float`, the generated C calls `double (*)(void*, double)` bodies through `int64_t (*)(void*, int64_t)` pointers -- undefined behavior that currently produces correct results only by SysV register-class accident (floats ride xmm0 untouched).
---

# `>>>` dispatches float closures through int64 thunk pointers

> **Status:** MITIGATED (fix direction A landed); `>>>` itself still affected
> (direction B tracked separately). Surfaced while evaluating "generalize `>>>`
> now" (the G7 amber edge of `language-readiness-for-typed-signal-plan`). The
> `tests/fixtures/sf-compose-typed/` fixture deliberately avoids stdlib `>>>`
> and uses a local monomorphic `compose-f` for exactly this reason; this report
> pins down *why* `>>>` cannot be swapped in.
>
> **Direction A shipped:** `stdlib/arrow.tur` now exports `compose-float`, a
> register-class-correct `:float -> :float` sequential-composition combinator
> (fixture `tests/fixtures/arrow-compose-float/`). Use it for float pipelines.
> Generalizing `>>>` *itself* (direction B) needs the compiler change tracked
> in `docs/upcoming/poly-closure-result-specialization-plan.md`; until that
> lands, `>>>` must not be applied to `:float`-returning closures.
>
> **Direction B, Stage A landed (2026-06-05):** the elaboration-acceptance
> widening that lets a *generic typed* closure-returning combinator
> (`(defn cmp [A B C] [^fat f :(fn [A] B) ^fat g :(fn [B] C)] : ptr<void> ...)`)
> type-check is implemented (`src/compiler/elab_call.c`) and verified at `:int`
> (fixture `tests/fixtures/generic-compose-int/`). The remaining emit-side
> inner-body cloning (Stages B+C) plus two newly-discovered prerequisites --
> specialization only triggers on fn-typed (not `ptr<void>`) arguments, and
> producing fn-typed closures from a `defn` is itself broken
> (`docs/reported/boxed-fn-typed-closure-return-miscompiles.md`) -- are tracked
> in the plan. `>>>` is **not** yet retyped.
>
> **Severity:** latent silent miscompile (UB function-pointer call). Produces
> correct output today for pure-`:float` chains by register-class luck; not
> guaranteed under optimization, a different ABI, or mixed int/float chains.

## Summary

`stdlib/arrow.tur`'s `>>>` is untyped:

```turmeric
(defn >>> [^fat f ^fat g] (let [fv f gv g] (fn [x] (gv (fv x)))))
```

Because `f`/`g`/`x` carry no declared signature, codegen commits the inner
closure to the int64 thunk ABI. When `>>>` is applied to two
`(fn [:float] :float)` closures, the emitted C invokes their
`double (*)(void*, double)` bodies through `int64_t (*)(void*, int64_t)`
function pointers. That is an indirect call through an incompatible
function-pointer type -- undefined behavior in C.

It happens to compute the right answer on x86-64 SysV today because integer
and floating-point arguments use disjoint register files: the int64-typed
inner body never touches `xmm0`, so the `double` argument/result flow through
`xmm0` undisturbed while the int64 plumbing shuffles garbage that is never
actually consumed. This is "works by luck because the register classes happen
to match," which the project treats as a bug, not a non-issue.

## Minimal repro

```turmeric
;; repro.tur
(load "stdlib/arrow.tur")
(defn make-scale [k : float] : ptr<void> (fn [x : float] : float (* x k)))
(defn make-add   [n : float] : ptr<void> (fn [x : float] : float (+ x n)))
(defn call-f [^fat f : (fn [:float] #{} : float) x : float] : float (f x))
(defn main [] : int
  ;; (3.0 + 0.0) * 0.5 = 1.5
  (println (call-f (>>> (make-add 0.0) (make-scale 0.5)) 3.0))
  0)
```

- **Observed:** `1.5` (correct -- but see root cause; it is correct only by
  register-class accident).
- **Expected (semantically):** `1.5`, computed through a type-correct dispatch.

## Root cause (file:line + emitted C)

`stdlib/arrow.tur:45` -- the inner `(fn [x] (gv (fv x)))` is untyped, so
`emit-c` lowers it to the int64 thunk ABI:

```c
struct __env_912 { tur_thunk_int64_t_int64_t_t __fn; void * gv; void * fv; };
static int64_t __fn_910(void * __env_p_913, int64_t x) {
    struct __env_912 *e = (struct __env_912 *)__env_p_913;
    return (*( tur_thunk_int64_t_int64_t_t *)(e->gv))(e->gv,
             (*( tur_thunk_int64_t_int64_t_t *)(e->fv))(e->fv, x));
}
```

But the argument closures are emitted as `double`-returning bodies:

```c
static double __fn_980(void * env, double x) { ... return x * k; }   /* make-scale */
static double __fn_987(void * env, double x) { ... return x + n; }   /* make-add   */
```

And the caller (`call-f`, whose parameter is typed `(fn [:float] :float)`)
dispatches the composed box through the *double* thunk type:

```c
static double call_f(int64_t f, double x) {
    return (*( tur_thunk_double_double_t *)f)((void *)f, x);   /* calls __fn_910 as double(void*,double) */
}
```

So three layers disagree on the C signature of the same code pointer:
`call_f` calls `__fn_910` as `double(void*,double)`; `__fn_910` is defined
`int64_t(void*,int64_t)`; and `__fn_910` calls `__fn_980`/`__fn_987`
(`double(void*,double)`) as `int64_t(void*,int64_t)`. Every one of these is a
function-pointer-type-mismatch indirect call (UBSan `-fsanitize=function`
would flag it; the runtime lib was unavailable in this environment to capture
the trap, but the type mismatch is plain in the source).

## Why a single generalized `>>>` cannot fix it in stdlib alone

Three spellings were tried against `./build/tur` (Debug):

1. **Polymorphic typed** `(defn >>> [A B C] [^fat f :(fn [:A] :B) ^fat g :(fn [:B] :C)] ...)`
   -- does not type-check: `^fat` shimming + tyvar function-typed parameters
   collide (`TUR-E0001: function 'fv' arg 1: expected <struct>, got tyvar`),
   and even if it did, returning a closure whose result is the bare tyvar `C`
   bound to a float is the `TUR-E0705` case (the resolved G2 report,
   `poly-defn-shares-inner-closure-body-across-monomorphizations`).
2. **Monomorphic `:float`** `compose-f` -- correct (verified, matches the
   int64 path's lucky output), but it cannot share the `>>>` name with the
   int-class definition (`>>>` mangles to a single C identifier `___`).
3. **Untyped (status quo)** -- the latent miscompile above.

So `>>>` is type-erased over its argument closures' signatures; at the C level
the inner body must commit to one concrete thunk signature, and untyped
defaults to int64. Float support requires either a separately-named typed
combinator or compiler support to specialize a closure-returning `defn` per
the closure types flowing through it.

## Proposed fix directions

- **A (stdlib, available now):** add a correctly-typed `:float` composition
  combinator (promote `tests/fixtures/sf-compose-typed/`'s `compose-f` into
  `stdlib/arrow.tur` under a distinct name). `>>>` stays int-register-class.
  Cheap, no codegen change, unblocks the signal rebuild's central combinator.
- **B (compiler):** specialize closure-returning `defn`s per the concrete
  closure signatures of their `^fat` arguments (and clone the inner closure
  body per specialization with the right C return type) -- the same
  "fix direction 1/2" the G2 report defers. This makes a polymorphic typed
  `>>>` work for `:float` and is the end-state. Larger; touches codegen and
  requires fixture-snapshot regeneration.

## Validation for a fix

- Direction A: a new fixture composes two `:float -> :float` closures via the
  stdlib combinator and asserts fractional output; emitted C shows
  `tur_thunk_double_double_t` end-to-end with no int64 thunk on the float path.
- Direction B: the existing `tests/fixtures/sf-compose-typed/` is rewritten to
  call stdlib `>>>` directly on `:float` SFs; emitted C contains no
  `int64_t (*)(void*, int64_t)` cast over a `double`-bodied closure;
  `bash tests/run.sh` green with snapshots regenerated.
</content>
