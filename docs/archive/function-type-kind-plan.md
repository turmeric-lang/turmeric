# Plan: Function-Type Kind in Spaced Annotations

> **Status:** Draft Plan
> **Last Updated:** 2026-05-27
> **Type:** Compiler / Elaboration
> **Tracks:** KB-008 (see `docs/archive/history/known-bugs.md`)

---

## Overview

`(defn apply1 [f : (-> int int)] :int ...)` fails to elaborate with:

```
error [TUR-E0012]: kind mismatch: cannot apply a type of kind '*' as a
type constructor; type must have kind '* -> *' or '* -> * -> *'
```

The `->` symbol in a *spaced* parameter annotation (`f : (-> int int)`)
is being looked up in the type-symbol table and finding a concrete
`TY_FN` of kind `*`, then the elaborator tries to apply it as a
constructor over `int int` and rejects the apply because `*` is not
applicable.

The non-spaced shorthand `f :(-> int int)` and the colonless form used
in lambda annotations both work, because they hit a different elaboration
path that special-cases `->`.

---

## Root cause

There are two parallel paths that resolve a type-position form into a
`Type *`:

1. **`type_expr_from_form`** in `src/compiler/elab_types.c` -- handles
   the colon-less / colon-prefix shorthand parameter list.  This path
   recognises `->` as a *type constructor* of kind `* -> * -> *` and
   expands `(-> A B)` into `TY_FN(A, B)`.
2. **`elab_spaced_annotation_type`** (the path triggered by the spaced
   `f : ...` form) -- shares most of the resolver with
   `type_expr_from_form` but routes `->` through the generic
   "look up symbol, then apply" code path.  The lookup returns the
   concrete `TY_FN` registered for `->` as a value-level operator,
   which has kind `*`, and the apply check rejects it.

The fix is small but needs care: the type-name resolver must
distinguish between the *value-level* `->` (an operator that
participates in expression elaboration) and the *type-level* `->`
(a constructor of kind `* -> * -> *`).

---

## Plan

### Phase 1 -- Reproduce in a fixture

Add `tests/fixtures/errors/defn-spaced-compound/` (or under a non-error
path if the spaced form should succeed):

```turmeric
(defn apply1 [f : (-> int int) x : int] : int
  (f x))

(defn main [] : int
  (apply1 (fn [x : int] : int (+ x 1)) 41))
```

`expected.stdout` is `42\n`.  This is the canonical reproducer for
KB-008 and the regression test once the fix lands.

### Phase 2 -- Locate the divergent resolver path

1. Search `src/compiler/` for `"->"` symbol-table lookups.  Likely
   suspects:
   - `elab_types.c` -- `type_expr_from_form`, `resolve_type_ctor`
   - `elab_core.c` -- spaced-annotation path; probably calls into a
     `resolve_type_name` helper that misses the `->` special case.
2. Diff the two paths against the colon-shorthand path that works.
   The shorthand path should already have a `match name ... case "->"`
   branch -- the spaced path is missing it.

### Phase 3 -- Centralise `->` resolution

Add a helper:

```c
// Returns a TY_FN constructor (kind * -> * -> *) when `name` is the
// type-level arrow, or NULL otherwise.  Centralises the special-case
// so spaced and colon-shorthand paths agree.
const Type *type_name_arrow_ctor(const char *name);
```

Both type-resolver paths call this before falling back to the generic
symbol-table lookup.  When it returns non-NULL, the form
`(-> A B)` is rewritten to `TY_APP(arrow, [A, B])` (or, equivalently,
constructed directly as `TY_FN(A, B)`).

### Phase 4 -- Audit related arrow-spelling cases

While fixing `->`, audit:

- `(=> A B)` -- typeclass-constraint arrow.  Is it consistent across
  paths?
- `(-> A B C)` -- multi-arg function type.  Should curry into
  `TY_FN(A, TY_FN(B, C))`.  Confirm both paths agree.
- Sweet-exp neoteric form: `f : ->(int int)`.  This goes through the
  neoteric reader before reaching the elaborator; verify it normalises
  to the same internal form.

### Phase 5 -- Regression coverage

Add fixtures covering each spelling:

| Fixture | Form | Notes |
|---|---|---|
| `fn-type-spaced/` | `f : (-> int int)` | Direct KB-008 repro |
| `fn-type-shorthand/` | `f :(-> int int)` | Already passes; lock in |
| `fn-type-curried/` | `f : (-> int int int)` | Multi-arg |
| `fn-type-neoteric/` | `f : ->(int int)` (in a `.tur.sweet` file) | Sweet-exp form |
| `fn-type-constraint/` | `[A] [^Eq a] (-> a a a)` | With typeclass constraint |

---

## Out of scope

- Higher-kinded `->` (an arrow over kinds, e.g. `* -> *`).  This plan
  only addresses the term-level arrow type constructor.
- Polymorphic `->` (rank-N function types).  The fix here is purely a
  parser/resolver alignment; rank-N support is a separate effort.

---

## Risks

- **Symbol-table conflict.**  The value-level `->` (operator) and the
  type-level `->` (constructor) currently share a name.  If the
  resolver helper accidentally short-circuits a value-level lookup
  in expression position, normal `->` calls break.  Mitigation: the
  helper only fires when the caller is a type-position resolver
  (`type_expr_from_form`, `elab_spaced_annotation_type`); value-side
  callers go through their own path unchanged.

---

## Verification

- KB-008 repro fixture passes.
- All four arrow-spelling fixtures from Phase 5 pass.
- `tests/run.sh` and `tests/run-turi.sh` both green.
- No regressions in `tests/fixtures/errors/` arrow-related cases
  (they should continue to fail with the same error messages).
