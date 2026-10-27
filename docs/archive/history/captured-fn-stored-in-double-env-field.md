---
title: Captured Function Value Stored in a `double` Closure-Env Field
category: Reported Bug
description: A closure that captures another function value (a fat closure or bare fn pointer) emitted the env field with the captured fn's RESULT type's C name. For a :float-returning callback that is "double", so the captured closure POINTER was stored in a floating-point field and reinterpreted via a double on read -- a latent miscompile that only round-trips because valid pointers fit in a double's 53-bit integer range. Fixed by pinning captured-fn env fields to the int64_t fn-ABI carrier.
---

# Captured Function Value Stored in a `double` Env Field -- Reported Bug

> **Status:** Fixed (2026-06-03, same session as discovery)
> **Found:** while writing the smallest standalone test of a `:float`-returning
>   fat closure passing through composition.
> **Severity:** Medium -- a silent miscompile. It round-trips by luck on every
>   current target (valid userspace pointers are <= 48 bits, which a `double`
>   represents exactly within its 53-bit integer range), so it does not segfault
>   today, but the captured closure pointer travels through a floating-point
>   field and the env field's C type disagrees with the `int64_t` fn-ABI carrier
>   the rest of the pipeline uses. This is precisely the "works by luck because
>   the register/representation happens to survive" hazard the typed-closure ABI
>   work is retiring.
> **Related:**
> - [closure-typed-invocation-abi-plan.md](../upcoming/closure-typed-invocation-abi-plan.md)
> - [cstr-returning-closure-thunk-int64-return.md](cstr-returning-closure-thunk-int64-return.md)

---

## Summary

When a closure captures a **function value** (another closure / fat pointer, or
a bare fn pointer), the capture-env struct field was emitted with
`type_c_name(captured->type)`. For a `TY_FN`, `type_c_name` returns the
function's **result** type's C name (see `src/compiler/types.c:1845-1853`):

```c
case TY_FN: {
    if (t.as.fn.boxed) return "void *";
    /* For bare function references, return the result type's C name. */
    return type_c_name(type_from_kind(t.as.fn.result_kind));
}
```

So a captured `:(fn [:float] #{} :float)` value lowered to a **`double`** env
field. The captured value is actually the closure *pointer* (an `int64_t`
fn-ABI carrier -- the same C type fn-typed parameters use, see
`src/compiler/emit_fns.c` TY_FN param branch). The producer therefore stored a
pointer into a `double` field (`__env->f = f;`, int64 -> double conversion) and
every read reinterpreted it as a pointer (`(void *)(intptr_t)(__env->f)`,
double -> int conversion).

## Repro

```turmeric
(defn call-f [^fat f : (fn [:float] #{} : float) x : float] : float
  (f x))

(defn compose [^fat f : (fn [:float] #{} : float)
               ^fat g : (fn [:float] #{} : float)] : ptr<void>
  (fn [x : float] : float (f (g x))))   ;; captures f and g -> fat, :float result

(defn make-scale [k : float] : ptr<void> (fn [x : float] : float (* x k)))
(defn make-add   [n : float] : ptr<void> (fn [x : float] : float (+ x n)))

(defn main [] : int
  (let [h (compose (make-scale 2.0) (make-add 1.0))]
    (println (call-f h 3.0)))   ;; (3 + 1) * 2 = 8
  0)
```

### Observed (before fix)

```c
struct __env_864 { tur_thunk_double_double_t __fn; double f; double g; };
                                                /*  ^^^^^^      ^^^^^^  captured fat-closure POINTERS */

static void * compose(int64_t f, int64_t g) {
    struct __env_864 *__t25 = (struct __env_864 *)malloc(sizeof(struct __env_864));
    __t25->__fn = (tur_thunk_double_double_t)__fn_862;
    __t25->f = f;   /* int64_t pointer -> double (numeric conversion) */
    __t25->g = g;
    ...
}

static double __fn_862(void * __env_p_865, double x) {
    struct __env_864 *__env___env_864 = (struct __env_864 *)__env_p_865;
    return (*(tur_thunk_double_double_t *)((void *)(intptr_t)(__env___env_864->f)))(   /* double -> intptr_t */
              (void *)(intptr_t)(__env___env_864->f),
              (*(tur_thunk_double_double_t *)((void *)(intptr_t)(__env___env_864->g)))(
                  (void *)(intptr_t)(__env___env_864->g), x));
}
```

The program prints `8` and exits 0 -- but only because the heap pointers are
below 2^53 and so survive the int -> double -> int round trip exactly.

### Expected (after fix)

```c
struct __env_864 { tur_thunk_double_double_t __fn; int64_t f; int64_t g; };
```

The captured fn carrier is stored and read as `int64_t`, agreeing with the fn
parameter ABI; no value ever travels through a floating-point field.

## Why it "works" today (and why that is a trap)

`double` exactly represents every integer in `[-2^53, 2^53]`. Userspace
pointers on x86-64/AArch64 are <= 48 bits, so `int64 ptr -> double -> intptr_t`
is currently lossless. The defect therefore hides behind a numeric coincidence:
the moment an address exceeds 2^53 (5-level paging / 57-bit VAs), or anything
relies on the env field's declared C type matching the `int64_t` carrier (the
typed-thunk dispatch contract), the pointer is corrupted. Storing a pointer in a
`double` is also needless FP traffic and is UB-adjacent.

## Root cause and fix

`type_c_name(TY_FN)` answering with the *result* type's C name is the wrong
thing for a captured fn **value** -- a function value is carried as the
`int64_t` fn-ABI carrier, never as its result type. Every capture-env emission
site repeated the same `type_c_name(captured->type)` call:

- `src/compiler/emit_fns.c` -- closure-thunk env layout (the site that emits the
  struct in the repro)
- `src/compiler/emit_expr.c` -- the secondary closure-env emission path and the
  `try`/`recover` thunk env
- `src/compiler/emit_effects.c` -- the cloneable-continuation capture env

Each now pins a captured `TY_FN` field to `int64_t` (mirroring the fn-param ABI)
instead of calling `type_c_name`. Poly-fn captures continue to use
`tur_poly_fn_t`.

## Validation

- `tests/fixtures/fat-closure-float-compose` -- the repro above, with an
  `expected.c` snapshot asserting `int64_t f; int64_t g;` and stdout `8`.
- Full suite stays green: `bash tests/run.sh` -> `0 failed`. No other fixture's
  snapshot drifted, confirming no existing fixture captured a fn value in a
  closure env before this test.
