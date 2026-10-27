---
title: Drop Leading Colons on Types Inside `(fn ...)` Type Expressions
category: Planning
description: Inside a `(fn [...] ...)` type expression, the param-type and result-type positions already imply "this is a type". The leading `:` on each type is redundant and visually noisy. Migrate `(fn [:float] :float)` → `(fn [float] float)` across the codebase, matching the convention already established by the spaced-type-annotation-migration-plan for outer `defn` / `let` annotations.
---

# Drop Leading Colons on Types Inside `(fn ...)` Type Expressions -- Plan

## Motivation

The [[spaced-type-annotation-migration-plan]] (complete) moved Turmeric
*outer* type annotations from fused `name:type` to spaced `name : type`.
The colon there is **structural** -- it separates the binder from the
type, so it has to stay.

Inside a `(fn [...] ...)` **type expression**, the situation is
different. Position alone tells the elaborator that the brackets hold
parameter types and that the form after the optional `#{...}` effect
set is the result type. The leading `:` on each inner type is redundant,
and it makes already-dense fat-closure type signatures harder to read:

```turmeric
;; current
(defn compose
  [^fat f : (fn [:float] #{} : float)
   ^fat g : (fn [:float] #{} : float)] : ptr<void>
  ...)

;; target
(defn compose
  [^fat f : (fn [float] #{} float)
   ^fat g : (fn [float] #{} float)] : ptr<void>
  ...)
```

The structural colon between `^fat f` and the closure type stays; only
the *inner* `:float`s lose theirs.

This is the same line of reasoning that moved keyword-prefixed numeric
literals to bare numerics: when the position already disambiguates,
sigils only add noise.

## Non-goals

- Touching outer `defn` / `let` / `defstruct` field annotations -- those
  already use the structural-spaced form and stay.
- Changing the *value-position* `:keyword` syntax. Keywords like
  `:vertex` / `:refer` remain keywords; this plan changes only how
  **types** appear inside `(fn ...)` type expressions.
- Touching `defstruct Pair [A B] (fst A) (snd B)` -- the field-name /
  type-name pairing there is already colon-free in the bare-identifier
  position (the field-type form).
- Removing the colon from the `result-type` of a `defn` (e.g.
  `(defn f [...] : (fn [float] float) ...)`). That outer `:` is the
  structural separator between the param vector and the return type and
  is in scope for the prior plan, not this one.
- Cleaning up the `&rest :type` variadic spelling.

## Current state

### Reader / parser

The reader emits `F_KEYWORD(":float")` (fused) for `:float` and
`F_TYPE_ANN(F_SYMBOL("float"))` (spaced) for `: float` (see
`src/compiler/reader.c`'s `read_keyword` path, ~lines 456–509). Both
forms already survive elaboration in **outer** annotation positions.

Inside a `(fn [...] ...)` type expression, elaboration currently
expects every type slot to arrive as `F_KEYWORD` (fused colon form) --
both inside the `[...]` param list and at the result-type position.
The path is in `src/compiler/elab_types.c` (the type-form unwrap that
calls `type_from_keyword`); the bare-identifier branch is not wired up
for these positions.

### Quantitative scope

`grep -rE '\(fn \[:' stdlib/ tests/fixtures/` finds about **615**
occurrences across `.tur` sources. Add `../turmeric-spices/` and the
total roughly doubles. Snapshots (`expected.c`) are codegen output and
are unaffected by source-level syntax changes once the parser accepts
both forms.

### Existing partial precedents

A handful of fixtures already use bare-identifier types *inside*
`(fn ...)` expressions in *value* position:

```turmeric
;; stdlib/parsec.tur:345
(defn pfail [] ^fat :ptr<void> (fn [inp] (mzero)))
```

But that is a `(fn [params] body)` value expression, not a
`(fn [types] result)` *type* expression -- different parser path.
The type-expression path is the one this plan targets.

## Design

### Surface syntax

A `(fn ...)` form in type position accepts a parameter list of
**bare-identifier types**, an **optional** `#{...}` effect set, and a
**bare-identifier result type**:

```turmeric
(fn [float] float)           ;; unary
(fn [float] #{} float)       ;; unary, empty effect set
(fn [int float] cstr)        ;; binary
(fn [] int)                  ;; nullary
(fn [int] (fn [int] int))    ;; curried -- inner type is itself a (fn ...)
```

Parenthesised inner types (`(fn ...)`, `(Pair int int)`, `ptr<int>`,
`(list float)`) keep their parens; only bare-identifier types lose
their leading `:`.

### Backwards-compatible transition

The parser accepts **both** forms during the transition:

```turmeric
(fn [:float] : float)        ;; legacy -- accepted
(fn [float]  float)          ;; new    -- accepted
(fn [:float]  float)         ;; mixed  -- accepted (lenient)
```

A deprecation warning fires on the legacy form after the codemod
sweep lands, and the legacy branch is removed two minor versions
later. Same shape as the spaced-type-annotation migration.

### Disambiguation

Inside `(fn [...] result)`, position is unambiguous:

- The first form is always the param-vector `[...]`.
- A `#{...}` set, if present, is always the effect set.
- The remaining form is always the result type.

No syntactic ambiguity is introduced. The only thing the elaborator
needs to add is: "in type position inside `(fn ...)`, accept an
`F_SYMBOL` and treat it as a type identifier" -- which is the same
lookup it already does for `F_KEYWORD` minus the `:` strip.

### Effect-set position stays the same

`#{}` (empty) and `#{IO Alloc}` (non-empty) remain colon-free already;
this plan does not touch them.

### Nested function types

```turmeric
;; before
(fn [:int] (fn [:int] : int))

;; after
(fn [int] (fn [int] int))
```

The inner `(fn ...)` is parsed recursively under the same rules.

### `:: ` ascription expressions

`(:: expr :ptr<void>)` and `(:: expr :float)` are **value-position**
ascriptions, not `(fn ...)` types. They are out of scope for this
plan. A follow-up could extend the rule to `::` ascriptions if the
ergonomics carry over, but that is a separate decision.

## Phasing

### Phase 1 -- Parser / elaborator: accept bare-identifier inside `(fn ...)`

1. **Locate the type-expression entrypoint.** `src/compiler/elab_types.c`,
   the branch that handles `head == fn-type` (or similar). Confirm it
   currently calls `type_from_keyword` on every slot.
2. **Add a bare-identifier branch.** For each param slot and the result
   slot: if the form is `F_SYMBOL`, look the symbol up in the type
   environment exactly as the keyword branch already does for the
   stripped name. If unknown → hard error with the same diagnostic the
   keyword branch emits.
3. **Land a regression fixture** at
   `tests/fixtures/fn-type-bare-identifier/input.tur` covering:
   - unary `(fn [float] float)`
   - binary `(fn [int float] cstr)`
   - nullary `(fn [] int)`
   - curried `(fn [int] (fn [int] int))`
   - mixed legacy + new in one signature
   - parenthesised inner type `(fn [(Pair int int)] int)`
   - declared user type `(fn [Sample] Sample)` (i.e. `defalias Sample : float`)
4. **No deprecation warning yet** -- both forms compile cleanly.

**Exit criteria**: all new fixtures pass; no existing fixture regresses
(`bash tests/run.sh` shows zero new FAILs).

### Phase 2 -- Codemod for in-repo sources

Mechanical rewrite, file at a time:

1. **Author a small `tools/rewrite_fn_type_colons.py`** that walks `.tur`
   files, finds the head `(fn` followed by `[`, and strips the leading
   `:` from every bare-identifier token between the `[` and the matching
   `]` plus the next non-`#{...}` form (the result type). Skip
   already-bare tokens. Skip parenthesised inner forms. Skip value
   `(fn [args] body)` forms (head is followed by `[name` not `[:type`
   or `[ ` + type identifier; require all bracket contents to look like
   types before rewriting). Default to dry-run; `--write` to commit.
2. **Run over `stdlib/`** first (smaller, faster feedback). Manual diff
   review. Build + tests must stay green.
3. **Run over `tests/fixtures/`** (sources only, never touch
   `expected.c` / `actual.*`). Many fixtures have `(fn [:int] :int)`
   in *comments* -- the codemod must restrict itself to type
   expressions in code positions, not comment bodies. Easiest is a
   token-level walk against the reader's tokens rather than a regex.
4. **Run over `docs/`** -- guide examples and plan snippets get the
   new spelling. Skip `docs/archive/`.
5. **Run over `../turmeric-spices/`** in a sibling PR. That repo lives
   on its own branch / PR cadence.

**Exit criteria**: all in-repo `.tur` sources use the new form; legacy
form survives only in:

- `docs/archive/**` (frozen),
- intentionally-mixed fixture `tests/fixtures/fn-type-bare-identifier/`
  (proves the lenient path still works).

### Phase 3 -- Deprecation warning on the legacy form

1. In `elab_types.c`, when an `F_KEYWORD` is consumed in a
   `(fn [...] ...)` type position, emit a `TUR-D000x` deprecation
   warning pointing at the source location. Warning is suppressible
   via the existing diagnostic-class machinery.
2. Add the warning's wording and migration hint to
   `docs/guides/diagnostics-guide.md` (if such a file exists; if not,
   skip).

**Exit criteria**: warning fires on the lenient-path fixture, doesn't
fire on the modern sources, doesn't fire on the value-position
`(fn [name] ...)` lambda form.

### Phase 4 -- Remove the legacy branch

After at least one minor release with the deprecation warning live, and
once `../turmeric-spices/` has been migrated and released:

1. Remove the `F_KEYWORD` branch from the `(fn ...)` type-expression
   handler. The keyword path elsewhere (outer annotations, ascriptions,
   `:keyword` value literals) is unaffected.
2. Update the lenient-path fixture
   (`tests/fixtures/fn-type-bare-identifier/`) to drop the mixed-form
   case; replace with a fixture under `tests/fixtures/errors/` that
   expects a hard diagnostic for `(fn [:float] :float)`.

**Exit criteria**: legacy form is a hard error with a clear "use bare
identifiers inside (fn ...) types" diagnostic.

## Risks and open questions

### Codegen snapshot churn

None expected -- this is a source-syntax change, not a codegen change.
The `expected.c` snapshots should not move. Phase 1 should verify this
by running the snapshot suite after the parser change but before any
codemod.

### Codemod over-reach

The biggest risk is rewriting `(fn [args] body)` lambda *values*
instead of type expressions. The value form's bracket holds binders
(symbols, possibly with `: type` annotations), not type identifiers.
Distinguish by inspecting bracket contents:

- If every bracket entry is a bare type identifier (or `(...)`
  type form), it's a type expression -- rewrite.
- If any entry is a binder shape (symbol followed by `: type` or
  `^fat` marker), it's a value lambda -- skip.

Add a unit test for the codemod that exercises both shapes before
running it on the tree.

### Spaced inner colon: `: float` vs `: int`

A user could write `(fn [: float] : float)` mixing the spaced outer
convention into the inner position. The parser would already see this
as `F_TYPE_ANN(float)`. The new elaborator branch should accept
`F_TYPE_ANN` in inner type slots identically to `F_SYMBOL` (strip the
wrapper, look up the type). This means three forms are all accepted in
Phase 1+2:

```turmeric
(fn [:float] : float)        ;; legacy
(fn [: float] : float)       ;; spaced-but-still-redundant -- accepted, but discouraged
(fn [float] float)           ;; target
```

Phase 3's deprecation warning should fire on **both** the legacy
keyword form *and* the spaced-but-redundant `F_TYPE_ANN` form -- the
goal is "no colon, in any flavour, inside a `(fn ...)` type".

### Interaction with sweet-exp

Sweet-exp users write `(fn [float] float)` in `.tur.sweet` files via
the same reader path, so no separate plumbing needed. Verify with one
`.tur.sweet` fixture.

### Macros that synthesise `(fn [...] ...)` types

`stdlib/contract.tur`, `stdlib/macros.tur`, etc. emit `(fn ...)` type
forms inside macro expansions. The codemod skips macro bodies (the
output is generated, not source). After Phase 2 they will still emit
the legacy form, which is accepted by Phase 1 and warned by Phase 3.
Sweep them in Phase 4's removal as a one-time fix.

## Acceptance

The plan is complete when:

1. The parser accepts bare-identifier types inside `(fn ...)` type
   expressions in every position (Phase 1).
2. All `.tur` sources under `stdlib/`, `tests/fixtures/` (inputs only),
   `docs/upcoming/`, `docs/guides/`, and `../turmeric-spices/` use the
   bare-identifier form (Phase 2).
3. The legacy form fires a deprecation warning (Phase 3) and is
   eventually rejected (Phase 4).
4. No codegen snapshot moves.

## Cross-references

- Builds on [[spaced-type-annotation-migration-plan]] (the outer-
  annotation analogue, complete).
- The two new bug reports filed under
  [[language-readiness-for-typed-signal-plan]] (G2, G4) and any other
  `(fn ...)` type signatures in the typed-signal rebuild
  ([[tur-signal-rebuild-plan]]) should be written in the new form
  once Phase 1 is in -- saves a churn pass later.
