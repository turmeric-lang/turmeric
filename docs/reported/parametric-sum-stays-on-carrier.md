# A parametric sum stays on the malloc'd carrier where its non-parametric twin goes by value

**Severity: low (performance; no wrong results).** Every constructor call
`malloc`s a box and the value rides the int64 carrier, where the same type
written without a type parameter is a by-value struct. Since the ADT/GADT
unification, an unannotated `defdata` and an annotated-but-unrefined one share
the same representation, so this is not GADT-specific any more. It is the
reason that slice 3 of
[adt-gadt-unification-plan](../upcoming/adt-gadt-unification-plan.md) moved
only the parameterless stdlib types (`StaticInt`, `Nat`, `Size`). `GVec`
(`stdlib/gadt-vec.tur:17`) and `SizedVec` (`stdlib/sized.tur:174`) gained
nothing. The gap was written down in that plan's *As built* notes only, and
this report is the record of it.

Found 2026-10-25 re-checking the unification's representation change on
`main` (`45c5fc32`).

## Repro

```turmeric
(defdata Bnd :copy [A] (Incl int) (Excl int) (Unb))
(defn mk  [x : int] : (Bnd int) (Incl x))   ; applied
(defn mkb [x : int] : Bnd       (Incl x))   ; bare
(defn main [] : int
  (match (mkb 4)
    (Incl x) (println x)
    (Excl x) (println x)
    (Unb) (println 0))
  0)
```

`tur emit-c` gives `static tur_adt_Bnd__int mk(...)`, which is by value, but
`static int64_t mkb(...)` over `ctor_Bnd_Incl`, which `malloc`s a
`tur_adt_Bnd`. Drop `[A]` and `mkb` returns a by-value `tur_adt_Bnd`.

The recursive case:

```turmeric
(defdata L [a] (N) (C a (L a)))
(defn len [v : (L int)] : int
  (match v
    (N) 0
    (C _ t) (+ 1 (len t))))
(defn main [] : int (println (len (C 1 (C 2 (N))))) 0)
```

A `ctor_L_C__int` monomorph is generated, but it `malloc`s a
`tur_adt_L__int` and returns `int64_t`, and `len` takes `int64_t v`. The
non-parametric `(defdata GVn (GNil) (GCons int GVn))` gives
`static tur_adt_GVn ctor_GVn_GCons(...)` and `len(const tur_adt_GVn * v)`.

| Shape | Representation on `main` |
| --- | --- |
| non-parametric sum, recursive or not | by value |
| parametric, bare name (`Bnd`, `GVec`, `SizedVec`) | carrier (`malloc` + `int64_t`) |
| parametric, applied, non-recursive (`(Bnd int)`) | by value (`tur_adt_Bnd__int`) |
| parametric, applied, self-recursive (`(L int)`, `(GVec int)`) | `__int` monomorph emitted, still carrier |

All of the programs above run and print the right answer. Only the
representation differs.

## Root cause

Two gates in `src/compiler/types.c`:

1. **Bare name.** `adt_sr1_sum_candidate` (`types.c:4023`) refuses any
   `n_type_params != 0` (`:4026`). That is correct while a type variable is
   free, because an `(L a)` has no single layout. It is also applied to a bare
   use of a parametric type whose constructors never mention the parameter (a
   phantom, like `Bnd`'s `[A]`). It is applied as well where every
   constructor's declared result pins the parameters to one closed
   instantiation (`GVec`: every constructor returns `(GVec int)`). Both have
   exactly one layout.
2. **Applied and self-recursive.** `adt_app_is_byvalue_product_inner`
   (`types.c:4597`) admits a multi-variant monomorph only when
   `!def->is_self_recursive` (`:4602`). That exclusion dates from SR2, when
   SR4 had "measured and declined" recursive sums. RM4 (2026-09-02) then
   flipped the **non-parametric** side to by value by default (`:4056-4065`,
   with `TUR_SR4_RECURSIVE_CARRIER=1` as the A/B escape). The app side was
   never flipped, so `(L int)` stays on the carrier and `GVn` does not.

## Fix directions

1. **App side first (gate 2).** Mirror RM4 in
   `adt_app_is_byvalue_product_inner`: admit a self-recursive monomorph, keep
   the fixpoint-partnership exclusion (`(ExprF Expr)` and friends) and the
   inline-cycle soundness gate, and read the same
   `TUR_SR4_RECURSIVE_CARRIER` escape. Re-measure the RM4 workloads at the
   applied type, and run `tests/run-sr4-seam.sh` and
   `region-escape-via-store` (a new by-value shape for
   `TUR_REGION_NOTE_WORDS`).
2. **Bare name of a single-layout parametric type (gate 1).** Let
   `adt_sr1_sum_candidate` accept a parametric def when no constructor field
   mentions a type parameter (a phantom). Also accept it when `is_refined` is
   false and every constructor's result form is the same closed
   instantiation. Treat the bare name as that instantiation's monomorph, so
   `Bnd` and `(Bnd int)` share one C type. Check `sized-vec-basic` first:
   `sized-vec-from-list`'s inline body relies on `ctor_SVNil()` /
   `ctor_SVCons(...)` returning `int64_t` (unification plan, slice 3).
3. Workaround meanwhile: drop an unused type parameter, as `range.tur`'s
   `Bound` did, or annotate uses with the applied type.
