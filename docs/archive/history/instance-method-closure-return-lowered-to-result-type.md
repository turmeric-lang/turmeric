---
title: Closure-Returning Typeclass Method Mis-Lowered to its Result Type (Dict Field + Impl)
category: Reported Bug
description: A `definstance` method whose declared return type is a thin function type `(fn [...] T)` lowers both its dictionary field type and its C impl signature to the function's *result* type `T` instead of a function-pointer type. The impl body returns a bare `T (*)(...)` function pointer, so the emitted C is type-mismatched. For non-`int64_t` result kinds (`:float`, `:bool`, small ints) this is a hard `cc` error; for `int64_t`-compatible results it "works by luck" with a `-Wint-conversion` warning. This is the instance-method mirror of the (now-resolved) plain-`defn` producer bug.
---

# Closure-Returning Typeclass Method Mis-Lowered to its Result Type -- Reported Bug

> **Status:** RESOLVED (2026-06-04). A `definstance` method whose declared
>   return type is a concrete (non-boxed) function value now carries it as the
>   matching fn-ptr typedef (thin, non-capturing body) or the `int64_t` closure
>   carrier (fat box) on **both** the dictionary field type and the impl
>   signature/forward-decl -- never the function's *result* type. The `:float`
>   repro compiles and runs; the `:int` thin case no longer emits
>   `-Wint-conversion`. Regression fixtures:
>   `tests/fixtures/instance-closure-return-float/` (`(fn [:float] :float)`)
>   and `tests/fixtures/instance-closure-return-bool/` (`(fn [:int] :bool)`).
>   See the "Resolution" section at the end.
>
> **Found:** 2026-06-04, while fixing
>   [fn-typed-return-lowered-to-result-type](history/fn-typed-return-lowered-to-result-type.md)
>   (the plain-`defn` producer-side mirror of this bug).
> **Severity:** Medium-High. For a closure result type whose C lowering is not
>   `int64_t` (e.g. `(fn [:float] :float)`), the emitted C is a **hard `cc`
>   error** -- the method cannot be compiled at all. For an `int64_t`-compatible
>   result (e.g. `(fn [:int] :int)`) it compiles but "works by luck": a bare
>   function pointer is returned through an `int64_t` slot with a
>   `-Wint-conversion` warning, and is read back correctly only because a valid
>   pointer fits in `int64_t`. The latter is the
>   [instance-closure-return-simple](../../tests/fixtures/instance-closure-return-simple/)
>   family of fixtures, which pass today only because their closures return
>   `:int`.

---

## Summary

A typeclass instance method whose declared return type is a thin
(non-capturing) function value lowers **both** its dictionary field type
**and** its C impl signature to the function's *result* type, not a
function-pointer type. The impl body produces a bare `T (*)(...)` function
pointer, so:

- `(fn [:int] :int)` -> dict field `int64_t (*m)(...)`, impl returns
  `int64_t`, body returns `int64_t (*)(int64_t)` -> compiles with
  `-Wint-conversion` (works by luck).
- `(fn [:float] :float)` -> dict field `double (*m)(...)`, impl returns
  `double`, body returns `double (*)(double)` -> **hard error**:
  `incompatible types when returning type 'double (*)(double)' but 'double'
  was expected`.

## Repro

Minimal (`:float`-returning closure -> hard error):

```turmeric
(defclass HasArr [a]
  (arr-of [self] : (fn [:float] : float)))

(defstruct ArrW [a]
  (raw :int))

(definstance HasArr [ArrW]
  (arr-of [self]
    (fn [x : float] : float x)))

(defn main [] : int
  (let [w (:: (make-struct ArrW 0) (ArrW int))
        g (.arr-of w)]
    0))
```

Emitted (excerpt from `tur emit-c`):

```c
static double __inst_HasArr_arr_of_ArrW(int64_t);   /* expected: a fn-ptr type */

typedef struct dict_HasArr_ArrW {
    double (*arr_of)(int64_t);                       /* expected: fn-ptr return */
} dict_HasArr_ArrW;

static double __inst_HasArr_arr_of_ArrW(int64_t self) {
        return __fn_857;                             /* __fn_857 is double(*)(double) */
}
```

`tur build` errors out:

```
error: incompatible types when returning type 'double (*)(double)' but 'double' was expected
        return __fn_857;
               ^~~~~~~~~
```

The `:int` variant (the existing `instance-closure-return-simple` fixture)
instead compiles with:

```
warning: returning 'int64_t (*)(int64_t)' from a function with return type
         'int64_t' makes integer from pointer without a cast [-Wint-conversion]
```

## Root cause

Two sites lower the method's fn-typed return via `type_c_name(rft)`, which for
a non-boxed `TY_FN` returns its *result* type's C name (the "bare function
reference returns its result" convention) -- correct for inline-C raw function
pointers, wrong for a turmeric-level closure value:

1. **Dict field type** -- `src/compiler/emit_stmt.c:468-482`: `ret_type` is
   taken from `method_impl->binding->type.as.fn.result_full_type` (the
   `(fn ...)` type), then `ret_c_name = type_c_name(ret_type)` collapses it to
   the result type. The field becomes `double (*arr_of)(int64_t)`.
2. **Impl signature** -- `src/compiler/emit_fns.c` (definition) and
   `src/compiler/emit_module.c` (forward decl): the
   `emit_fn_return_typedef(fd, rft)` helper added for the plain-`defn` fix
   **deliberately excludes `__inst_*` method impls** (so the impl matches the
   dict field), so the impl also lowers via `type_c_name(rft)` -> `double`.

The impl body, a thin non-capturing closure, emits a bare function pointer
(`__fn_857` of type `double (*)(double)`). Returning it through a `double`
(or `int64_t`) slot is the mismatch.

This is the exact dual of
[fn-typed-return-lowered-to-result-type](history/fn-typed-return-lowered-to-result-type.md):
that report fixed the plain-`defn` producer; this one is the dictionary /
method-impl path, which was intentionally left on the `int64_t`-by-luck
carrier when scoping that fix.

The comment at `src/compiler/elab_typeclasses.c:2172-2186` claims attaching
`result_full_type` "makes `type_c_name` lower the fn carrier to `int64_t`" --
but that is only true when the closure's result kind is itself `int64_t`. For
`:float`/`:bool`/small-int results `type_c_name` returns `double`/`bool`/etc.,
so the claim (and the codegen) is wrong for those.

## Implications

- A typeclass that returns a closure with a non-`int64_t` result type cannot
  be instantiated -- the program fails to compile.
- Even the `:int` case is a latent miscompile waiting to bite: the
  `-Wint-conversion` warning is the only thing standing between "works" and a
  register-class mismatch should the warning be downgraded or the closure
  flow through a path that reinterprets the slot (cf. the `^fat`
  bare-param/non-int-result miscompile resolved on the consumer side).

## Proposed fix

The dict field type and the impl signature must agree, and both must match the
thin-fn-pointer representation the body produces. Two consistent directions:

1. **Thin fn-ptr typedef on both sites (preferred).** Make the dict field
   (`emit_stmt.c`) and the method impl (`emit_fns.c` / `emit_module.c`) use the
   same `emit_fn_return_typedef`-style fn-ptr typedef when the method's return
   is a concrete, non-boxed `TY_FN` and the impl body statically yields a thin
   function pointer (`body_yields_thin_fn`). Drop the `__inst_*` exclusion in
   `emit_fn_return_typedef` once the dict field is updated in lockstep. The
   dispatch call site (`emit_expr.c`, `((ret_t (*)(...))(intptr_t)(dict.m))(args)`)
   already casts through `intptr_t`, so it is agnostic to the field's exact
   pointer type -- but verify it does not assume `int64_t` width anywhere.
   Capturing-closure-returning methods (fat boxes) must keep the
   `int64_t`/`void*` carrier, exactly as the plain-`defn` fix does, so the box
   is not mistyped.

2. **`int64_t` carrier on both sites.** Force the dict field and impl return to
   `int64_t` for any fn-typed return (matching params/captures), with explicit
   `(int64_t)(intptr_t)` casts on the impl `return`. This unifies thin and fat
   under one carrier and removes the `-Wint-conversion` warning, at the cost of
   the call site always casting back. This is the more uniform option and least
   likely to surprise the fat path.

Whichever is chosen, the dict field (`emit_stmt.c:468-482`) and the impl
signature (`emit_fns.c` / `emit_module.c`) must be changed **together**, and
the `instance-closure-return-*` snapshots regenerated.

## Validation when fixed

1. The `:float` repro above compiles cleanly; `(.arr-of w)` applied to `1.5`
   yields `1.5`.
2. Add a regression fixture mirroring `instance-closure-return-simple` but with
   a `(fn [:float] :float)` (and a `(fn [:int] :bool)`) method return, applied
   at the consumer.
3. The existing `instance-closure-return-*` fixtures still pass (snapshots
   regenerated), and the `:int` cases no longer emit `-Wint-conversion`.
4. Full `bash tests/run.sh` stays green.

## Resolution (2026-06-04)

Implemented **direction 1** (thin fn-ptr typedef), extended to keep fat boxes
on the `int64_t` carrier explicitly so non-`int` *result kinds* are correct on
both the thin and the fat path.

- The `__inst_*` exclusion was removed from `emit_fn_return_typedef`
  (`src/compiler/emit_core.c`): a thin (non-capturing) method body yields a
  bare C function pointer, so a typeclass-method impl uses the same
  `tur_fnptr_<ret>_<args>_t` typedef as a plain `defn` producer.
- A new arbiter `emit_inst_fn_return_carrier(fd, rft)` (also in `emit_core.c`)
  is the single source of truth for an `__inst_*` method's fn-typed return:
  it returns the fn-ptr typedef for a thin body (via `emit_fn_return_typedef`),
  otherwise the `int64_t` closure carrier (a `void*` fat box returned through
  an `int64_t` slot). It returns NULL for non-method / non-fn returns so the
  ordinary lowering is untouched.
- All three emission sites consult it so they agree:
  - dict field type -- `src/compiler/emit_stmt.c` (the dict-struct emission);
  - impl definition signature -- `src/compiler/emit_fns.c`;
  - impl forward declaration -- `src/compiler/emit_module.c`.

  This replaces the previous `type_c_name(rft)` lowering, which collapsed the
  fn type to its *result* type's C name (`double`/`bool`/...), mismatching the
  function-pointer / box the body produces.

Why this also covers the latent fat path: `type_c_name(TY_FN)` returned the
*result* type's C name, which equals the `int64_t` carrier only when the
result kind is itself `int`. A capturing method returning e.g.
`(fn [:float] :float)` would have mis-lowered to `double` too; routing every
non-thin fn-typed `__inst_*` return through the explicit `int64_t` carrier
fixes that as well (the by-luck `-Wint-conversion` on the fat path remains, as
the box is intentionally a `void*` returned through `int64_t`, exactly as the
plain-`defn` fix leaves it).

Validation:

1. The `:float` repro compiles cleanly (no warning) and `(.arr-of w)` applied
   to `1.5` yields `3.0`.
2. New regression fixtures
   [instance-closure-return-float](../../tests/fixtures/instance-closure-return-float/)
   (`(fn [:float] :float)`) and
   [instance-closure-return-bool](../../tests/fixtures/instance-closure-return-bool/)
   (`(fn [:int] :bool)`) build, run, and pass.
3. The existing `instance-closure-return-*` fixtures still pass; the thin
   `:int` `instance-closure-return-simple` snapshot was regenerated and now
   emits `tur_fnptr_int64_t_int64_t_t` (no more `-Wint-conversion`).
4. Full `bash tests/run.sh` stays green.

## Cross-references

- Dual of (resolved):
  [fn-typed-return-lowered-to-result-type](history/fn-typed-return-lowered-to-result-type.md)
  (plain-`defn` producer side).
- Same register-class family as the `^fat` bare-param / non-int-result
  miscompile (consumer side, resolved).
- Originating design:
  [closure-returning-instance-method-codegen-plan](../upcoming/closure-returning-instance-method-codegen-plan.md)
  -- whose `int64_t`-carrier claim holds only for `int64_t`-result closures.
