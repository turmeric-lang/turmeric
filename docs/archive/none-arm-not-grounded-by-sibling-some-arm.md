# A `(None)` arm is not grounded by its sibling `(Some x)` arm, so a value-position literal match builds two representations

**Resolved 2026-10-14: the report is archived.** The repro, its `if` twin
and the `(Some ...)`-first ordering all build and print `1`, under gcc, clang
and the interpreter.

**Root cause.** A nullary constructor of a parametric ADT has no argument
to infer its type parameter from. Unless an expected type pins it
(`elab_call.c`'s `result_pinned`), it is typed as the **bare** ADT,
`Option`, rather than as an application. From there the two forms failed
differently:

- **`if`**: the branch check compares types strictly, so it was a type
  error: "then=Option else=(Option T)".
- **`match`**: the arm unifier joins a bare ADT with an application of it
  by keeping the application, which is the right type. But the `(None)`
  node kept the bare type, so codegen built it with the erased base
  constructor (`ctor_Option_None()`, the int64 carrier) beside the
  `(Some x)` monomorph (a by-value aggregate).
- **The literal-pattern path** also kept the *first* arm's type, so with
  `(None)` first the whole match was bare.

**The gap was wider than filed.** On the previous commit even a
non-generic `(let [o (if c (None) (Some 3))] ...)` was a type error
("then=Option else=(Option int)"), and so was any `cond` with a `(None)`
branch, through its `if` expansion. Only a declared result type or another
expected type made these shapes work.

**Fix (`elab_structs.c`, `elab_forms.c`).** `elab_retype_nullary_ctor_arm`
gives a bare-typed nullary constructor the joined application, which is the
type an expected type would have pinned. A nullary constructor carries no
payload, so its representation follows from its type alone. GADTs are
excluded, and any other bare-typed expression keeps its type. It is applied
in three places:

- at the end of the ADT `match` join;
- at the end of the literal-pattern join, which now also prefers an
  application over a bare first-arm ADT, as the ADT path already did;
- in the `if` join, which accepts a bare-vs-application pair *only* when
  the bare side is such a constructor (any other bare branch still
  mismatches).

Pinned by `tests/fixtures/none-arm-joined-with-some-arm`, under `run.sh`
(gcc 13, clang 18) and `run-turi.sh`. It covers int, 7.1, cstr and a
user-defined ADT, with the `None` path both taken and not taken, across:

- a literal match, in both arm orders
- an ADT-pattern match
- an `if`
- a `cond` with two `(None)` branches
- the non-generic `let`

Full `run.sh` was 3802 passed, 0 failed, with no `expected.c` moved. Full
`run-turi.sh` was 2775 passed, 0 failed.

The fixture carries no `requires.leak-check`. Its leaked blocks are `Cons`
nodes, plus one erased `(Some v)` box in the uncloned base of a generic.
That box belongs to the open
[carrier-sum-option-boxes-have-no-owner](../reported/carrier-sum-option-boxes-have-no-owner.md),
and this change does not touch it, since it retypes only nullary
constructors.

The original filing follows. Its guess at the cause, that the arm join does
not push the `Some` arm's element type into `(None)`, was right. The fix
does that after the join, not by unification.

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
