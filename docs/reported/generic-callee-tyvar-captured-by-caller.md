# A generic callee's type variable is bound to the caller's when both are named `A`

**Severity: medium (mistyped C; gcc 14 and clang 15+ reject it).** Found
2026-10-28 building xml-html-followups-plan follow-up D1b. The same family
as [generic-result-tyvar-captured-under-projection-expected](../archive/generic-result-tyvar-captured-under-projection-expected.md),
which fixed a projection-typed expected type; this one needs no projection.

## Repro

```turmeric
(defn lens [A] [xs : (Cons A)] : (Cons int)
  (if (tnil? xs) (tnil) (tcons-of 1 (lens (ttail xs)))))
(defn main [] : int
  (let [r (lens (tcons-of "a" (tcons-of "b" (tnil))))]
    (println (thead r)))
  0)
```

It prints `1`, but the specialization of `lens` at `A = cstr` returns the
`(tnil)` branch as a `(Cons cstr)`:

```
warning: returning 'tur_adt_Cons__cstr *' from a function with incompatible return type 'tur_adt_Cons__int *' [-Wincompatible-pointer-types]
```

`tnil` is declared `(defn tnil [A] [] : (Cons A) ...)`; its `A` is bound to
the caller's `A` (`cstr`) instead of being solved from the expected type
`(Cons int)`. An explicit `(:: (tnil) (Cons int))` does not help. Renaming
the caller's variable (`[E] [xs : (Cons E)]`) does.

## Where it bit

tur-dom-core's `dom/holes` `__children` and tur-html's `__to-html-list` --
`(defn f [^C A] [xs : (Cons A)] : (Cons Child) (if (tnil? xs) (tnil) ...))`
-- emitted this for every program with a list in an `#html{...}` or hiccup
hole. Both now name their variable `E`.

## Fix directions

Freshen a callee's type variables before unifying its signature at a call
inside a generic body (the archived fix did this for one expected-type
shape); the specializer should never see the caller's `A` in `tnil`'s
instantiation.
