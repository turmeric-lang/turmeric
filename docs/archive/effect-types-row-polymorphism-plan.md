# Effect Types (Row Polymorphism) -- Implementation Plan (ET0--ET4)

> **Status:** Draft -- Not Started
>
> **Target:** v3
>
> **Prerequisites:**
> - Phase 19 (Algebraic Effects) complete -- shift/reset substrate, `EffectRow`
>   types, `effect_check.c` inference pass, row-variable substitution.
> - Effect Rows enforcement (ER0--ER6) complete or substantially complete --
>   especially ER2 (row-variable unification) and ER4 (effect row subtyping).
> - HRT Phase HRT1 (Rank-2 types) from the v2 roadmap -- required for
>   `forall [e]` effect polymorphism (ET2).
>
> **Related:**
> - [../guides/advanced-type-system-rationale.md](../guides/advanced-type-system-rationale.md) (§8 Effect Types / Row Polymorphism)
> - [effect-rows-plan.md](effect-rows-plan.md) (ER0--ER6 enforcement phases)
> - [guides/hrt-guide.md](guides/hrt-guide.md) (Higher-Ranked Types prerequisite)
>
> **Last updated:** 2026-05-15

---

## Motivation

Phase 19 shipped algebraic effects with a runtime shift/reset substrate and a
fixed-point inference pass (`effect_check.c`). The ER0--ER6 enforcement phases
add strict checking, row-variable unification, typeclass method rows, subtyping,
and module visibility.

What those phases do **not** provide is **first-class effect polymorphism**:
functions that are generic over their effect rows in the full Koka/Eff/OCaml 5
sense. Specifically, they leave open:

| Gap | Symptom | Addressed in |
|---|---|---|
| No `forall [e]` quantification | `map` cannot say "I propagate `f`'s effects" generically | ET2 |
| Handler types are untyped | `handle` expressions have no static type for the handler | ET3 |
| Effect row not part of `fn` type signature | Type aliases and structs cannot abstract over effect rows | ET0 |
| Handler composition has no static model | Stacking handlers silently widens rows | ET3 |
| No stdlib effect hierarchy | Each effect is a flat name with no subtyping structure | ET4 |

ET0--ET4 address these gaps in a layered way that is forward-compatible with
ER2 row-variable work and the HRT Rank-2 type system.

---

## Architecture Overview

The relevant source files:

```
src/reader.c        -- parses #{...} and (forall [e] ...) syntax
src/elab.c          -- elaborates defn, defeffect, defclass, definstance
src/effect.h/.c     -- EffectRow algebra; EffectRowSubst; will gain PolyRow
src/effect_check.c  -- fixed-point inference + declared-vs-inferred check
src/effect_lower.c  -- perform -> shift; handle -> reset; populates EffectEnv
src/types.h/.c      -- Type struct; fn effect_row field; subtype relation
src/typeclass.c     -- dict passing; method effect rows (ER3)
```

After ER0--ER6 the inference pass understands row variables (`#{e}`) at
call sites, enforces subtyping, and propagates rows through typeclasses.
ET0--ET4 extend the **type representation** so that row variables can be
**universally quantified** and so that handler expressions have a **typed
signature**.

---

## Phase ET0 -- Effect Row Syntax in Function Types

**Goal:** Effect rows become a syntactically first-class part of function types,
not just an annotation on `defn`. Type aliases, struct fields, and anonymous
function types all support explicit effect-row positions.

### Background

After ER0 a `defn` can carry `#{Write Log}` or `#{e}`. But the underlying
`Type` struct for a function (`TY_FN`) stores the effect row in an
`effect_row` field added in Phase 19. The elaborator does not yet propagate
this field when constructing derived types (e.g. when a `let`-bound variable
is inferred to have a function type from a lambda, or when a struct field is
typed `(fn [...] :T)`).

### Tasks

#### ET0-A: `(fn [...] #{...} :T)` in type positions

- [ ] Update `src/reader.c`: `fn` in a **type** context (inside `:` annotations,
  `deftype` bodies, struct field types) already parses `(fn [arg-types] ret-type)`.
  Extend to accept an optional `#{...}` between the arg list and return type:
  `(fn [arg-types] #{effect-row} ret-type)`.
- [ ] Update `elab_type_expr` in `src/elab.c` to parse the `#{...}` component
  from the `fn` type AST node and store it in the resulting `TY_FN`'s
  `effect_row` field.
- [ ] Type equality (`type_equal`) and subtyping (`type_is_subtype`) already
  have a hook for `effect_row`; confirm they work for the annotated function
  type case.

#### ET0-B: Effect row in type aliases

- [ ] `(deftype PureFunc [a b] (fn [a] #{} b))` -- type alias captures the
  empty row. Verify the row propagates when the alias is expanded in
  `elab_type_expr`.
- [ ] `(deftype IofuncOf [a b] (fn [a] #{Io} b))` -- alias for an Io function
  type. Expanding the alias must produce a `TY_FN` with `effect_row = #{Io}`.

#### ET0-C: Effect row in struct fields

- [ ] A struct field typed `(fn [] #{Write} :nil)` stores the effect row in
  the field's `Type`. When the field is accessed and called, the caller's
  inferred row picks up `#{Write}`.
- [ ] This is a prerequisite for capability-field effect polymorphism
  (completed in ER4/ER6 for simpler cases; ET0 extends it to struct fields in
  general positions).
- [ ] Verify that codegen ignores effect rows (they are erased before
  `effect_lower.c` emits C).

#### ET0-D: Fixtures

- [ ] `effect-fn-type-annot.tur` -- `(fn [int] #{Write} int)` in a type
  annotation accepted; calling it propagates `#{Write}` to the caller.
- [ ] `effect-type-alias.tur` -- type aliases with explicit effect rows expand
  correctly.
- [ ] `effect-struct-field-row.tur` -- struct with an effectful function field;
  calling the field propagates the declared row.
- [ ] Negative: `errors/effect-fn-type-mismatch.tur` -- assigning a `#{Write}`
  function to a `#{}` field or variable emits `TUR-E0009`.

**Exit criterion:** Effect rows are first-class in all type positions; the
elaborator propagates them consistently from type annotations to inferred types.

---

## Phase ET1 -- Effect Row Type Checking

**Goal:** The elaborator enforces effect rows at every expression boundary,
not just at `defn`-level. Subsumption, widening, and narrowing are checked
consistently. `handle` expressions reduce the row of their body by the effects
they handle.

### Background

ER4 adds subtyping for `TY_FN` types with effect rows. ET1 extends this to:
1. Inline subsumption checks at `let`, `if`, `match` branches, and `do`
   sequences.
2. Proper row reduction at `handle` / `try-with` sites (the handled effects
   are removed from the body's inferred row).

### Tasks

#### ET1-A: Subsumption at let-bindings

- [ ] When `(let [f expr] ...)` binds `f` to a function, and `f` has a declared
  type annotation on the binding site, check that `inferred_row(expr) ⊆
  declared_row(annotation)`.
- [ ] Emit `TUR-E0009` on failure. Include the binding name in the error message:
  "binding `f` has declared row `#{}` but expression performs `#{Write}`".

#### ET1-B: Subsumption at if/match branches

- [ ] The inferred row of an `if` expression is the **union** of the rows of
  both branches. If one branch performs `#{Write}` and the other performs
  `#{Log}`, the `if` as a whole performs `#{Write Log}`.
- [ ] Same rule for `match` -- union of all arm rows.
- [ ] This is a soundness fix: currently branches are flattened by
  `collect_effects_in_expr`; verify correctness and add a test.

#### ET1-C: Row reduction at `handle` sites

- [ ] When `(handle body (Effect [...] k) arm ...)` is elaborated:
  1. The inferred row of `body` is computed.
  2. Each handled effect is **subtracted** from that row.
  3. The resulting row is the inferred row of the entire `handle` expression.
- [ ] `effect_row_subtract` (to be added in `src/effect.h`): given two
  `EffectRow`s `r` and `handled`, returns `r` with all effects in `handled`
  removed.
- [ ] If `body` does not actually perform a handled effect, emit `TUR-W0033`
  ("handler clause for `Foo` is unreachable -- `Foo` is not in the body's
  effect row").

#### ET1-D: Row reduction at `try-with` sites

- [ ] `try-with` (introduced in ER6) desugars to `handle`; ET1's row reduction
  applies automatically once the desugaring is in place.

#### ET1-E: Do-sequence row accumulation

- [ ] In `(do expr1 expr2 ...)`, the inferred row is the union of all
  sub-expression rows. Verify `collect_effects_in_expr` already unions across
  `EX_DO` sequences and add a fixture.

#### ET1-F: Fixtures

- [ ] `effect-let-subsumption.tur` -- `let`-binding with explicit effect annotation
  is enforced.
- [ ] `effect-if-union.tur` -- `if` with mixed-effect branches reports union row.
- [ ] `effect-handle-reduce.tur` -- `handle` correctly removes handled effects
  from the enclosing row.
- [ ] `effect-do-union.tur` -- `do` sequences accumulate all sub-expression rows.
- [ ] Negative: `errors/effect-handle-unreachable.tur` -- `TUR-W0033` for handler
  clause that cannot be reached.
- [ ] Negative: `errors/effect-let-row-mismatch.tur` -- `TUR-E0009` for
  over-constrained `let` binding.

**Exit criterion:** Effect rows are enforced at every expression boundary;
`handle` reduces the body's row by the handled effects; subsumption errors
are reported with accurate source locations.

---

## Phase ET2 -- Effect Polymorphism (`forall [e]`)

**Goal:** Functions can quantify over their effect rows using `forall [e]`,
enabling reusable higher-order combinators that are transparent to effects.

### Background

ER2 introduces per-call-site row-variable binding for `#{e}` in `defn`
signatures. This covers the common case -- a function like `run-twice` that
delegates effects to a callback. However, `#{e}` is an **open** row variable:
it is not universally quantified, so it cannot be used in type aliases or as a
return type.

Full effect polymorphism requires rank-2 (or higher) types: `forall [e] (fn []
#{e} T)`. This requires **HRT Phase HRT1** (Rank-2 types) as a prerequisite.
ET2 must not start until HRT1 is landed.

### Syntax

```turmeric
;; A function polymorphic over its effect row
(defn run-twice [f : (forall [e] (fn [] #{e} : int))] #{e} : int
  (+ (f) (f)))

;; Effect-polymorphic map
(defn fmap [f : (forall [e] (fn [a] #{e} b)), xs : (list a)] #{e} (list b)
  ...)

;; Effect-polymorphic bracket (acquire / use / release)
(defn with-resource
  [acquire : (fn [] #{e} : R)
   use     : (forall [e2] (fn [R] #{e e2} : A))
   release : (fn [R] #{e} : unit)]
  #{e} : A
  ...)
```

### Tasks

#### ET2-A: `forall [e]` in type expressions

- [ ] `src/reader.c`: parse `(forall [e1 e2 ...] type-expr)` as a new AST
  node `EX_FORALL_ROW` (or reuse the existing rank-N forall from HRT with a
  `TK_EFFECT_ROW` kind marker).
- [ ] `elab_type_expr`: elaborate `(forall [e] ...)` into a new type kind
  `TY_POLY_ROW` (or extend `TY_FORALL` to carry row-kinded binders).
- [ ] Row-kinded binders have kind `Row` (not `*`); extend the kind system to
  track this.

#### ET2-B: `EffectRowVar` with universal scope

- [ ] Extend `src/effect.h`: add `ERK_POLY_VAR` (or reuse `ERK_VAR` with a
  universally-quantified flag) to distinguish per-call-site row variables
  (ER2) from universally quantified ones (ET2).
- [ ] During `forall [e]`-type instantiation, fresh `ERK_VAR` variables are
  generated (same as HRT's type-variable instantiation).
- [ ] Instantiation and generalisation follow the standard Hindley-Milner scheme
  applied to row kinds.

#### ET2-C: Row-variable unification during instantiation

- [ ] When a `(forall [e] ...)` type is applied, instantiate `e` with a fresh
  row variable and pass it to `effect_row_unify` (already exists from ER2).
- [ ] After unification, apply the substitution to both the function body and
  the call site's inferred row.
- [ ] Row variables that remain free after the function returns are generalised
  (become universally quantified in the inferred type).

#### ET2-D: Effect polymorphism in typeclasses

- [ ] `defclass` method signatures may include `forall [e]` row quantifiers:
  ```turmeric
  (defclass Traversable [t]
    (traverse [f : (forall [e] (fn [a] #{e} b)), xs : (t a)] #{e} (t b)))
  ```
- [ ] `definstance` bodies are checked against the instantiated row for the
  particular instance's concrete effects.
- [ ] This builds directly on ER3 (typeclass method effect rows).

#### ET2-E: Inference without explicit `forall`

- [ ] When a `defn` has a row variable `#{e}` in its **inferred** row (not
  just declared), and `e` is not bound at the call site, generalise `e` to a
  universally quantified row variable automatically (Hindley-Milner style).
- [ ] This means simple higher-order functions do not need explicit `forall`
  annotations; the elaborator infers them.
- [ ] Under `--strict-effects` (ER1), emit `TUR-W0034` ("row variable `e`
  was generalised; consider adding explicit `forall [e]` annotation").

#### ET2-F: Fixtures

- [ ] `effect-poly-map.tur` -- effect-polymorphic `map` function accepted and
  caller's inferred row matches the mapped function's row.
- [ ] `effect-poly-bracket.tur` -- `with-resource` pattern with
  `forall [e]` callback; row propagates through.
- [ ] `effect-poly-typeclass.tur` -- `Traversable` instance with polymorphic
  `traverse`; row propagates to the caller.
- [ ] `effect-poly-infer.tur` -- `forall [e]` inferred without explicit
  annotation.
- [ ] Negative: `errors/effect-poly-escape.tur` -- row variable that escapes
  its scope is rejected.

**Exit criterion:** `forall [e]` is parsed, elaborated, instantiated, and
inferred; effect-polymorphic functions compose correctly; typeclasses support
row-quantified method signatures.

---

## Phase ET3 -- Handler Typing

**Goal:** `handle` expressions and handler definitions have explicit, checkable
types. Handlers are first-class values that can be passed, composed, and typed.

### Background

Currently, `handle` is a special form with no associated type beyond the
inferred row of its body. A handler for `Write` has no type that says "I
accept a `Write` effect and produce `unit`". This makes handler composition
ad-hoc and prevents static verification of handler stacks.

### Handler Type Representation

A handler for effect `E` with continuation type `K` and result type `R` has
type:

```
(handler E K R)
```

This is analogous to a function type but with three components:
- `E` -- the effect being handled
- `K` -- the type of the value passed to `resume` (continuation argument)
- `R` -- the type returned by the handler clause body

```turmeric
;; A handler for the Write effect
(deftype WriteHandler []
  (handler Write string unit))

;; A handler that can be composed
(defn log-handler [] : (handler Write string unit)
  (handle-clause (Write [msg] k) (resume k (println msg))))
```

### Tasks

#### ET3-A: `TY_HANDLER` type kind

- [ ] Add `TY_HANDLER` to `src/types.h` with fields:
  - `effect_name : Symbol*` -- the effect being handled.
  - `value_type : Type*` -- type of the value passed with `perform`.
  - `cont_arg_type : Type*` -- type of the value passed to `resume`.
  - `result_type : Type*` -- type returned by the handler body.
- [ ] Add constructor `type_make_handler(...)` in `src/types.c`.
- [ ] `TY_HANDLER` is a non-function, non-ADT type; codegen emits it as a
  C struct (or function pointer bundle).

#### ET3-B: Handler clauses as first-class values

- [ ] Introduce `(handler-clause (Effect [params] k) body)` syntax (or a macro
  over `handle`) that evaluates to a value of type `(handler Effect ...)`.
- [ ] Alternatively, extend `handle` to accept named handler definitions that
  can be extracted as first-class values.
- [ ] First-class handlers are represented as C function pointers with a closure
  for any captured values.

#### ET3-C: Handler typing in `handle` expressions

- [ ] When elaborating `(handle body clause1 clause2 ...)`:
  1. Elaborate each clause to get its `TY_HANDLER` type.
  2. Check that the handled effects in clauses exactly cover some subset of
     the body's inferred row.
  3. The residual row (body row minus handled effects) becomes the inferred
     row of the `handle` expression.
  4. The `result_type` of each handler clause must match the declared return
     type of the `handle` expression (or be unified with it).

#### ET3-D: Handler subtyping

- [ ] A handler for effect `E` with value type `V` and result type `R` is a
  subtype of a handler for `E` with value type `V'` and result type `R'` if
  `V' ⊆ V` (contravariant) and `R ⊆ R'` (covariant), analogous to function
  subtyping.
- [ ] Implement via `type_is_subtype` extension for `TY_HANDLER`.

#### ET3-E: Handler composition

- [ ] `(compose-handlers h1 h2)` stacks two handlers; the composed handler
  handles the union of their effects and returns the result type of the
  outermost handler.
- [ ] Implement as a library function (or macro) in `stdlib/effects.tur` using
  the `TY_HANDLER` type.
- [ ] Type-check that the effect sets of `h1` and `h2` are disjoint (emit
  `TUR-E0251` on overlap) or that the overlap is explicitly allowed.

#### ET3-F: Fixtures

- [ ] `effect-handler-type.tur` -- `(handler Write string unit)` type is
  well-formed; value of this type assigned to a variable.
- [ ] `effect-handler-compose.tur` -- two handlers for different effects
  composed; caller's row is reduced by the union.
- [ ] `effect-handler-subtype.tur` -- handler subtyping accepted for widened
  result type.
- [ ] Negative: `errors/effect-handler-overlap.tur` -- `TUR-E0251` for
  composed handlers with overlapping effects.
- [ ] Negative: `errors/effect-handler-result-mismatch.tur` -- handler clause
  return type does not match `handle` expression type.

**Exit criterion:** Handlers have first-class types; `handle` expressions are
fully typed; handler composition is statically verified.

---

## Phase ET4 -- Integration, Stdlib, and Effect Hierarchies

**Goal:** Round out the effect type system with stdlib annotations, a standard
effect hierarchy, tooling, and error messages. This phase makes ET0--ET3
production-ready.

### Effect Hierarchy in Stdlib

Design a standard effect lattice for Turmeric's stdlib effects:

```
Total (no effects)
  |
Pure (deterministic, no I/O)
  |
IO (file, network, stdio)
  |-- Read
  |-- Write
  |-- Network
  |-- Filesystem
  |
Async
  |-- Spawn
  |-- Await
  |
Fail
  |-- Panic
  |-- Throw
  |
Log
  |-- Debug
  |-- Info
  |-- Warn
  |-- Error
  |
Unsafe (raw pointers, casts)
```

- [ ] Define the hierarchy using `defeffect` declarations with a `^extends`
  annotation (or similar syntax TBD).
- [ ] Update `effect_row_is_subset` to respect the hierarchy so that a function
  performing `Write` satisfies a context requiring `IO`.
- [ ] Document the hierarchy in `docs/guides/effects-system-guide.md` and
  `docs/guides/custom-effects-tutorial.md`.

### Stdlib Annotation

- [ ] Annotate all public stdlib `defn`s with explicit effect rows (building on
  the ER6 stdlib annotation task, which covered priority files; ET4 covers the
  full stdlib).
- [ ] Key files: `stdlib/list.tur`, `stdlib/vec.tur`, `stdlib/map.tur`,
  `stdlib/hamt.tur`, `stdlib/result.tur`, `stdlib/option.tur`,
  `stdlib/free.tur`, `stdlib/fix.tur`, `stdlib/async.tur`,
  `stdlib/thread.tur`, `stdlib/stm.tur`.
- [ ] Pure functions in the stdlib are annotated `#{}` and enforced.
- [ ] I/O functions carry explicit `#{Write}` / `#{Read}` / `#{Io}` rows.

### Error Messages

Introduce new error codes in the `TUR_E0250`--`TUR_E0299` range (reserved in
the advanced type system plan):

| Code | Condition |
|---|---|
| `TUR-E0250` | `forall [e]` row variable escapes its quantifier scope |
| `TUR-E0251` | Composed handlers have overlapping effect sets |
| `TUR-E0252` | Handler clause result type mismatch |
| `TUR-E0253` | Effect not in scope at perform site (private or undeclared) |
| `TUR-E0254` | `forall [e]` instantiation produces an infinite row (occurs check) |

- [ ] Implement each code in `effect_check.c` / `elab.c` with source-location
  information, a one-line diagnostic, and a longer hint message.
- [ ] Add `tur explain TUR-E0250` entries (if the `tur explain` tool is available).

### Feature Flag

- [ ] Introduce `-Xeffect-types` compiler flag in `src/main.c` /
  `src/compiler_options.h`.
- [ ] When `-Xeffect-types` is active:
  - ET0--ET4 checks are enforced.
  - `--strict-effects` (ER1) is automatically implied.
  - `forall [e]` syntax is accepted.
  - `TY_HANDLER` types are available.
- [ ] When `-Xeffect-types` is absent, the system falls back to ER0--ER6
  behaviour (no `forall [e]`, no `TY_HANDLER`).

### Tooling

- [ ] `--dump-effects` (ER6): update output to show quantified rows, e.g.
  `run-twice : forall [e]. (fn [(fn [] #{e} int)] #{e} int)`.
- [ ] Language server hover (if LSP support is available): show full
  effect-polymorphic type including quantified rows.
- [ ] `--check` mode includes ET0--ET4 errors when `-Xeffect-types` is set.

### Fixtures

- [ ] `effect-stdlib-pure.tur` -- verifies that `list/map`, `option/map`, etc.
  are inferred as `#{}` (pure).
- [ ] `effect-stdlib-io.tur` -- verifies that `println`, `file/read`, etc. carry
  explicit `#{Write}` / `#{Read}` rows.
- [ ] `effect-hierarchy.tur` -- a function performing `Write` satisfies a
  context requiring `#{IO}` via the effect hierarchy.
- [ ] `effect-flag-off.tur` -- without `-Xeffect-types`, `forall [e]` syntax is
  rejected gracefully.
- [ ] `effect-error-codes.tur` -- golden-output tests for each new error code.

**Exit criterion:** All stdlib public functions are annotated; the effect
hierarchy is defined and respected; `-Xeffect-types` enables the full feature;
error codes are in place with helpful messages.

---

## Complexity Assessment

| Aspect | Complexity | Notes |
|---|---|---|
| Type system changes | High | `TY_POLY_ROW`, `TY_HANDLER`, row-kind binders |
| Elaborator changes | Very High | Row unification, `forall` instantiation, handler typing |
| Codegen changes | Low | Effect rows are erased; `TY_HANDLER` maps to C function pointers |
| C emission | Low | No new C constructs; handlers are function pointers |
| Error messages | Medium | New error codes, good source locations |
| Stdlib work | Medium | Annotation pass over ~15 files |

---

## Relationship to ER0--ER6

The ER and ET phase series are complementary:

| ER Phase | Purpose | ET Counterpart |
|---|---|---|
| ER1 (`--strict-effects`) | Opt-in purity enforcement | Subsumed by `-Xeffect-types` in ET4 |
| ER2 (row-var unification) | Per-call-site `#{e}` binding | Foundation for ET2 `forall [e]` |
| ER3 (typeclass rows) | Method body vs. declared row | ET2-D extends to row-quantified methods |
| ER4 (row subtyping) | `fn` type subtyping | ET1 formalises subsumption everywhere |
| ER5 (module visibility) | Private effect enforcement | ET4 integrates with the effect hierarchy |
| ER6 (polish) | `try-with`, static one-shot | ET3 adds typed handler composition |

ER2 is intentionally conservative: it supports `#{e}` without universal
quantification, allowing incremental adoption without requiring HRT. ET2
upgrades `#{e}` to full `forall [e]` once HRT1 lands.

---

## Dependencies

```
ER0--ER6 (enforcement layer, prerequisite)
HRT Phase HRT1 (Rank-2 types, prerequisite for ET2)
  |
  ET0 (effect row syntax in type positions)
    |
    ET1 (expression-level row checking; handle reduction)
      |
      [GATE: effect_row_occurs check + TUR-E0254 fixture must pass]
        |
        ET2 (forall [e] polymorphism)  <-- requires HRT1 + occurs check gate
          |
          ET3 (handler typing; TY_HANDLER; typed handler structs)
            |
            ET4 (stdlib, hierarchy, tooling, -Xeffect-types flag)
```

ET0 and ET1 can proceed in parallel with HRT development (they do not require
rank-N types). ET2 must wait for HRT1. ET3 and ET4 follow ET2.

---

## Summary Roadmap

| Phase | Goal | Key Files | Status |
|---|---|---|---|
| ET0 | Effect rows in all type positions | `reader.c`, `elab.c`, `types.h` | Planned |
| ET1 | Expression-level row checking; `handle` reduction | `elab.c`, `effect_check.c`, `effect.h` | Planned |
| ET2 | `forall [e]` polymorphism | `reader.c`, `elab.c`, `effect.h`, `types.h` | Planned (needs HRT1) |
| ET3 | Handler typing; first-class handlers | `types.h`, `elab.c`, `effect_check.c` | Planned |
| ET4 | Stdlib, hierarchy, tooling, `-Xeffect-types` | Multiple; `stdlib/`, `main.c` | Planned |

---

## Open Questions

1. **Effect hierarchy syntax:** ~~Should `^extends` be a `defeffect` attribute, or should the hierarchy be declared in a separate form?~~
   **Decision:** `^extends` attribute on `defeffect`. The parent relationship lives adjacent to the effect definition:
   ```turmeric
   (defeffect Write ^extends IO
     (write [msg : cstr] : unit))
   ```
   Consistent with other `^`-prefixed annotations; keeps the hierarchy discoverable at the definition site. No standalone `effect-hierarchy` form.

2. **`forall [e]` vs. implicit generalisation:** ~~Should row polymorphism always require explicit `forall [e]`, or should the elaborator generalise row variables implicitly?~~
   **Decision:** Implicit generalisation by default; explicit `forall [e]` always permitted. Under `--strict-effects` (implied by `-Xeffect-types`) the elaborator emits `TUR-W0034` on any inferred `forall [e]` that was not written, nudging library authors toward explicit annotations without forcing it on all users.

3. **First-class handlers vs. `handle` special form:** ~~Making handlers first-class (`handler-clause` syntax) adds expressiveness but increases elaborator complexity. An intermediate option is typed handler records (structs with `TY_HANDLER`-typed fields) without a new syntactic form.~~
   **Decision:** Typed handler structs (Option B). No new `handler-clause` syntax. A handler is a regular Turmeric struct with a `TY_HANDLER`-typed field holding a function pointer matching the handler signature. Handlers can be named, bound, and passed as struct values. Composition is manual (call the struct's field). Reuses existing struct and function pointer machinery; defers full first-class handler syntax to a later phase if demand warrants it.

4. **Interaction with Linear Types (LT0--LT4):** ~~The ER6 plan notes that static one-shot enforcement of `k` (the continuation in a handler) is a stepping stone toward linear continuations. ET3 should coordinate with the linear types plan to ensure `TY_HANDLER` and `CK_LINEAR` continuations compose cleanly.~~
   **Decision:** Defer. Ship ET3 with untracked continuation kind; ER6's static one-shot warning provides safety in the interim. Continuation linearity is revisited once linear types (LT0--LT4) are stable and real handler usage patterns are established. See [linear-continuations-plan.md](linear-continuations-plan.md) for the full design of the deferred Option B implementation (`CK_UNIQUE` by default, `^linear k` opt-in).

5. **Effect row occurs check:** ~~Row unification can loop if a row variable is unified with a row that contains it. An occurs check must be added to `effect_row_unify` in `src/effect.h` before ET2 lands. Error code `TUR-E0254` is reserved for this.~~
   **Decision:** The occurs check is a **gating prerequisite for ET2** -- a separate, explicitly completed task before ET2 work begins. Add `effect_row_occurs(EffectRowVar*, EffectRow*)` to `src/effect.h`; call it inside `effect_row_unify` before committing any substitution; emit `TUR-E0254` on failure. ET2 is not started until this task has a passing fixture (`errors/effect-row-occurs.tur`).

---

## References

- Bauer, A. & Pretnar, M. -- [Programming with Algebraic Effects and Handlers](https://doi.org/10.1016/j.jlamp.2014.02.001) (Eff language)
- Leijen, D. -- [Koka: Programming with Row-Polymorphic Effect Types](https://doi.org/10.4204/EPTCS.153.8)
- Sivaramakrishnan, K.C. et al. -- [Retrofitting Effect Handlers onto OCaml](https://doi.org/10.1145/3453483.3454039) (OCaml 5)
- Hillerström, D. & Lindley, S. -- [Liberating Effects with Rows and Handlers](https://doi.org/10.1145/3007263.3007271)
- [effect-rows-plan.md](effect-rows-plan.md) -- ER0--ER6 near-term enforcement plan
- [../guides/advanced-type-system-rationale.md](../guides/advanced-type-system-rationale.md) -- Feature decision matrix and roadmap

---

*Last updated: 2026-05-15*
