---
title: cstr-returning Closure Thunk Emitted with int64_t Return Type
category: Reported Bug
description: A capturing (fn [...] :cstr ...) closure's thunk function is emitted with an int64_t return type instead of const char *, producing a -Wint-conversion warning and a latent mismatch with the typed-thunk typedef the call site computes. Pointer-returning (:ptr<void>) and float-returning (:float) closures lower correctly, so the defect is specific to the :cstr (TY_CSTR) result.
---

# cstr-returning Closure Thunk Emitted with int64_t Return -- Reported Bug

> **Status:** RESOLVED (2026-06-03). Root cause was upstream as predicted:
>   the `fn`-literal return-type parser in `elab_fn` (`elab_fns.c`) handled
>   `int`/`float`/`bool`/`void`/`ptr<void>`/`nil`/`ptr`/`rc`/`weak`/`!` but had
>   **no `cstr` case** (unlike `elab_defn`, which handles it), so a `(fn [...]
>   :cstr ...)` literal's `:cstr` return fell into the tyvar fallback and was
>   carried as a named type variable that lowers to the int64 carrier. Added the
>   missing `cstr` keyword branch so the closure's fn-type result is `TY_CSTR`;
>   the thunk now emits as `const char * (*)(void *, ...)` with no
>   `-Wint-conversion` warning. Regression fixture:
>   `tests/fixtures/cstr-returning-closure-thunk` (a `:cstr`-*returning*
>   capturing closure round-tripped through `TUR_APPLY1_T`). `bash tests/run.sh`:
>   0 FAIL.
> **Found:** 2026-06-03, during Phase 1 of the Typed Closure Invocation ABI work
> **Severity:** Low-Medium -- compiles and runs correctly today (a pointer
>   fits in the int64 register), but emits a `-Wint-conversion` warning and
>   leaves the thunk's real C signature inconsistent with the typed-thunk
>   typedef, which is exactly the erasure-by-luck the ABI work is retiring.
> **Related:**
> - [closure-typed-invocation-abi-plan.md](../upcoming/closure-typed-invocation-abi-plan.md)

---

## Summary

A capturing closure whose declared return type is `:cstr`, e.g.
`(fn [s :cstr] :cstr ...)`, is lowered to a C thunk whose **return type is
`int64_t`** rather than `const char *`. The thunk's *parameter* is typed
correctly (`const char *`), and the closure body's internal temporaries
are typed correctly; only the function's return type is wrong.

`:ptr<void>`-returning and `:float`-returning closures lower correctly
(`void *` and `double` respectively), so the defect is specific to the
`:cstr` / `TY_CSTR` result type.

## Repro

```turmeric
(defn call-s [f : ptr<void> s : cstr] : cstr
  ```c return TUR_APPLY1_T(char *, char *, f, s); ```)

(defn make-pick [n :int] :ptr<void>
  (fn [s :cstr] :cstr (if (> n 0) s "other")))   ;; captures n -> fat closure

(defn main [] :int
  (println (call-s (make-pick 1) "hello"))
  0)
```

`tur build` emits:

```
warning: returning 'const char *' from a function with return type
'int64_t' {aka 'long int'} makes integer from pointer without a cast
[-Wint-conversion]
```

and the generated thunk is:

```c
static int64_t __fn_859(void * __env_p_862, const char * s) {   /* <- int64_t, should be const char * */
    struct __env_861 *__env___env_861 = (struct __env_861 *)__env_p_862;
    const char * __t2;                                          /* body temps are correct */
    if ((__env___env_861->n) > (INT64_C(0))) { __t2 = s; }
    else { __t2 = "other"; }
    return __t2;                                                /* const char * returned through int64_t */
}
```

Contrast (both correct):

```c
static void * __fn_863(void * __env_p_866, void * p)  { ... }   /* :ptr<void> -> void *   */
static double __fn_859(void * __env_p_862, double x)  { ... }   /* :float     -> double    */
```

## Why it works anyway (and why that is a trap)

`const char *` and `int64_t` share the same return register on every
mainstream ABI, so the pointer survives the round-trip and the program
prints `hello`. That is the same "works by luck because the register
class happens to match" hazard the parent ABI plan documents for pointer
returns. The moment something depends on the thunk's *declared* C return
type matching the typed-thunk typedef the call site computes
(`tur_thunk_const_char_..._t = const char * (*)(void *, const char *)`),
the int64-return thunk is a type-mismatched function pointer. It also
means a `:cstr`-returning closure cannot be invoked cleanly through the
compiler's own typed-thunk dispatch path without the warning.

## Likely cause

The closure-thunk header is emitted in `emit_fns.c` (~lines 365-385). The
return type is taken from the closure's `e->type` (its `TY_FN`):

```c
} else if (e->type.as.fn.result_full_type) {
    ... emit_type_c_name(ctx, *e->type.as.fn.result_full_type) ...
} else {
    buf_puts(file, emit_type_c_name(ctx, emit_type_from_kind(result)));  // result = result_kind
}
```

For `:ptr<void>` and `:float` the recorded `result_kind` /
`result_full_type` is correct, so this prints `void *` / `double`. For
`:cstr` the emitted type is `int64_t`, which means the closure's recorded
fn-type result for a `:cstr` return is being carried as `TY_INT` (or a
full type that lowers to `int64_t`) rather than `TY_CSTR`. The erasure is
therefore upstream, at the point the `(fn [...] :cstr ...)` literal's
`TY_FN` type is built during elaboration -- the `:cstr` return annotation
is not surviving onto `result_kind` / `result_full_type` the way `:float`
and `:ptr<void>` do. The fix belongs there (preserve `TY_CSTR` on the
closure's fn type), not at the emit site.

## Validation when fixed

- The repro above compiles with no `-Wint-conversion` warning and the
  thunk is emitted as `static const char * __fn_...(void *, const char *)`.
- A `:cstr`-returning capturing closure invoked through the compiler's
  typed-thunk dispatch (not just hand-written `TUR_APPLY1_T`) round-trips
  the string.
- Add a fixture mirroring `tests/fixtures/tur-apply-t-cstr-arg` but with
  a `:cstr` *return* (it was deliberately written as a `:cstr` *argument*
  to dodge this bug at the time).
