# A `(None)` arm is not grounded by its sibling `(Some x)` arm, so a value-position literal match builds two representations

**Severity: low-medium.** The compiled program fails to build. With `if`
the same program is a type error instead. One ascription works around both
forms.

**Discovered:** 2026-10-14, while resolving
[instance-method-match-fallthrough-return-unbridged](../archive/instance-method-match-fallthrough-return-unbridged.md).

## Repro

```turmeric
(defn first-flag [T] [o : (Cons T)] : int
  (match (match (tnil? o) true (None) false (Some (thead o)))
    (None) 0
    (Some x) 1))
(defn main [] : int
  (println (first-flag (tcons-of 3 (tnil))))
  0)
```

`tur run` (gcc or clang):

```
error: incompatible types when assigning to type 'int64_t' from type 'tur_adt_Option__int'
```

The inner literal match's arms disagree on representation. `(None)` builds
the erased base, `ctor_Option_None()`, which is the int64 carrier. `(Some
(thead o))` builds the monomorph `ctor_Option_Some__int`, a by-value
aggregate. Nothing grounds the match's own `(Option T)` (the enclosing
function returns `int`), so its result temp stays the carrier, and the
aggregate arm is stored into it unbridged.

The same shape as an `if`, `(if (tnil? o) (None) (Some (thead o)))`, is
rejected earlier by elaboration: "if branches have mismatched types". So
`(None)`'s element type is not unified with the sibling arm's in either
form; the `match` form just gets further before failing.

As a function's *result*, the same literal match works, because the spec's
result type grounds it (`first-lit` in
`tests/fixtures/match-result-temp-typed-return`).

## Root cause

Not located. It is elaboration-level: the join of the arms (`match` arms
or `if` branches) does not push the `Some` arm's element type into the
nullary `(None)`. That is the same family as the generic-zipper plan's I1
and I4, where `(tnil)` does not take `T` from its context.

## Workaround

Ascribe the nullary arm: `(:: (None) (Option T))`. Both the `match` and
the `if` form then build and print the right answer.

## Fix directions

- Unify a nullary constructor arm's type parameters with the joined result
  of the other arms (the `match` arm unifier, and the `if` branch check).
- Pin it: the repro, its `if` twin, and a `(Some ...)`-first ordering.
