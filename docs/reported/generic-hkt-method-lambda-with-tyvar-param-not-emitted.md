# A lambda with a type-variable parameter, passed to an HKT method in a generic defn, is never emitted

**Severity: medium (cc failure on valid code).** Filed 2026-10-11 while
executing [parsec-guide-plan](../archive/parsec-guide-plan.md). Reproduces on
`main` at v0.64.0.

## Repro

```turmeric
(defn w3 [A] [o : (Option A)] : (Option A)
  (bind o (fn [v : A] : (Option A) (some v))))

(defn main [] : int (println (unwrap (w3 (some 5)))) 0)
```

```
nl4_tur.c: In function 'w3__spec__tur_adt_Option__int_tur_adt_Option__int':
error: '__poly_8' undeclared (first use in this function)
```

The same shape with the `Parser` monad (`(bind p (fn [v : A] : (Parser A)
(pure v)))` inside `(defn w1 [A] ...)`) fails at link time with `undefined
reference to '__poly_802'`, and a nested continuation capturing such a
parameter (`(do-m v p vs (many q) (pure (list-cons v vs)))` with `v : A`)
fails with `'__fn_N' undeclared`.

A lambda whose parameter is typed by the enclosing signature's type variable,
handed to a dictionary-dispatched method (`bind`, `fmap`) inside a generic
`defn`, is referenced from the per-instantiation clone but its body is never
emitted. The same lambda passed to an ordinary higher-order `defn` compiles.

## Effect on parsec

`parsec-guide-plan` P8 types an un-annotated `do-m` binder from its
receiver -- `(do-m c (item) ...)` binds `c : Char`. Inside a generic `defn`
that would type binders with the enclosing tyvar (`v : A`) and walk straight
into this defect, so the inference is restricted to GROUND element types
(`elab_method_call`, src/compiler/elab_typeclasses.c): a generic parser's
binders keep the old int-carrier default. Lifting that restriction is the
test for a fix here.

## Fix direction

Find where the poly-wrapped lambda (`__poly_N` / `__fn_N`) of a generic
`defn`'s body is lifted: it is emitted for the carrier base or skipped, but the
spec clone re-elaborates the call and references a fresh name. Either emit the
lifted lambda per spec, or have the clone reuse the base's lifted lambda.
