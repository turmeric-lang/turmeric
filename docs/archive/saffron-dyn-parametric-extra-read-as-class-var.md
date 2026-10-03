# An `int` extra on a parametric-head instance is cast as the receiver type

**RESOLVED 2026-10-03** by fix direction 2, started at the rewrite as the
narrowing below said it had to be.  In a dynamic dialect, a bare EXTRA
parameter (not the receiver) of an instance on a parametric head is typed
`any` by `elab_definstance` instead of being retyped to the head -- the
dialect's own default for a bare parameter, and exactly what spelling
`n : any` in the instance already did.  The impl narrows it where it is used:
`(vec-get v n)` and `(vec-len y)` both pass it through the D5 seam, so
`Nth [Vec]`'s index and a `same-len?` whose `y` really is another vector both
work, on both dispatch paths.  The witness needs no change: an `any` impl
parameter is neither the tyvar nor the head, so `saffron_extra_is_class_var`
says no and the extra is passed through bare.  The receiver keeps the
rewrite, and so does a kind-* head, whose class variable is a concrete type.
Non-dynamic instances (the stdlib's `Eq [Vec]`) are untouched.  Pinned by
`tests/fixtures/saffron-dyn-bare-extra` (the report's repro, a vector extra,
a cstr/float extra; dynamic and static dispatch; matches `--interpret`).

**Narrowed 2026-09-29: a SPELLED `n : int` is fixed; a bare `n` remains.**
Fix direction 1: `saffron_extra_is_class_var` returns false for a parameter
the class annotated (`param_explicit_type`), and the witness's concrete-type
cast arm -- which only ran for a kind-* head -- now also runs for a
parametric head's extra that is not the class variable, so the `any` extra is
cast to `int` rather than passed through (without the second half the spelled
case became a cc error, `incompatible type for argument 2`).  Pinned by
`tests/fixtures/saffron-dyn-spelled-int-extra` (both spellings from the
report).

**What remains: the bare `(nth-of [x n] : any)`.**  Re-checked 2026-09-29, it
is more than a guess in the witness: the instance side's Prereq-4 rewrite
(`elab_definstance`, "untyped params default to TY_INT") retypes a bare `int`
class parameter to the instance head, so the `Nth [Vec]` impl itself declares
`n : (Vec ...)` -- exactly as `Eq [Vec]`'s `y`.  A body-scan that tells the
two apart (`Eq`'s body ascribes `y` to `(Vec A)`; `Nth`'s never touches `n`
that way) was tried and reverted: it cannot help while the impl's own
signature says `Vec`.  The fix has to start at that rewrite (fix direction 2).


**Severity: low-medium.** Compiled only, and a clean panic at a checked cast,
never a wrong answer; `--interpret` answers. It is not limited to
unannotated parameters: an explicitly spelled `n : int` panics the same way.
Split out of [saffron-lang-plan](../archive/saffron-lang-plan.md) S9's
remaining limits (the "unannotated extra on a parametric head" item) when the
plan was archived 2026-09-28.

When a Saffron program dispatches a method on an `any` whose instance has a
parametric head (`Nth [Vec]`), the compiled witness decides per extra
argument whether it is "another value of the receiver's type". An `int` in
both the class and the impl counts as yes, because that is how `Eq`'s
unannotated `(eq? [x y])` records -- so an index argument is cast to
`(Vec any)` and panics.

## Repro

```turmeric
#lang saffron
(defclass Nth [a]
  (nth-of [x n] : any))
(definstance Nth [Vec]
  (nth-of [v n] (vec-get v n)))
(defn pick [x i] (.nth-of x i))
(defn main []
  (println (pick [7.25 1 "hi"] 2))
  0)
```

Measured 2026-09-28 with `./build/tur` (v0.56.2):

- `tur --interpret`: prints `hi` (expected).
- `tur run`: `panic at ...: cast: any holds int, not Vec`, exit 134.

Identical results with the class spelled `(nth-of [x n : int] : any)` and
`(nth-of [x : a n : int] : any)`.

## Root cause

`saffron_extra_is_class_var` (`src/compiler/elab_typeclasses.c:6245`): its
last arm, line 6253, returns true when the impl's parameter and the class's
parameter are both `TY_INT`. The caller (the parametric-head arm at line
6433) then casts the extra to the all-`any` instantiation `(Vec any)`. The
arm exists so `(.eq? v w)` on two `(Vec any)` values works, since `Eq [Vec]`
records its unannotated `y` as `int` on both sides; a genuine `int` is
indistinguishable at that point.

The class DOES record the difference for a spelled annotation:
`TypeClassMethod.param_explicit_type` (`src/compiler/typeclass.h:40`, set by
`parse_typeclass_method`) is true for `n : int` and false for a bare `n`. The
helper does not consult it.

## Fix directions

1. Gate the `TY_INT` arm on `!tc->methods[slot].param_explicit_type[j]`, so a
   spelled `: int` is an `int` and only a bare parameter is guessed to be the
   class variable. That fixes the spelled cases above outright.
2. For a bare parameter the guess stays ambiguous (`Eq`'s `y` and this `n`
   record identically). Consult the impl: a parameter used as the receiver's
   type in the body (passed where a `(Vec A)` is expected) versus as an index,
   or pass a bare extra through as `any` and let the impl narrow it -- which
   first needs the method-dispatch path to run the D5 argument seam (the
   kind-`*` witness arm just below notes that it does not).
3. Add a fixture: the repro plus the `n : int` variant, on both back ends.
