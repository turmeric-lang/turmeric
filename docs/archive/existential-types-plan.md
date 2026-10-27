# Existential Types Plan

> **Status:** Draft Plan -- Option B Implementation Tasks
> **Last Updated:** 2026-05-23
> **Type:** Language Design

---

## Overview

Existential types let you hide a type variable behind an opaque boundary:
`exists a. F a` means "there is some type `a` satisfying `F`, but I am not
telling you which one." The outside world can only use the type through an
agreed-upon interface (a typeclass constraint or an explicit operation set).

This plan covers the motivation, design options, and complexity of adding
existential types to Turmeric. The implementation is oriented toward
**Option B -- `pack`/`open` with typeclass constraints**, executed in phases.

---

## Motivation

### 1. Hiding size indices (`Vec[A]` vs. `SizedVec[n A]`)

`docs/upcoming/typed-collections-plan.md` defines `Vec[A]` and `SizedVec[n A]`
as completely separate types (Phase TC2 decision). With existential types, a
third option becomes viable:

```turmeric
;; Option B from typed-collections-plan.md:
(deftype Vec [A] = (exists n. SizedVec[n A]))
```

A `Vec[A]` would be a `SizedVec` whose length index is sealed away. You could
convert in both directions:

```turmeric
;; Reveal the size (existential open):
(open-vec [v :Vec[A]] ([n] [sv :SizedVec[n A]])
  ;; n is a fresh abstract Nat; sv is the underlying SizedVec
  ...)

;; Pack any SizedVec back into a Vec (existential introduction):
(defn vec-of-sized [sv : SizedVec[n A]] : Vec[A]
  (pack sv as Vec[A]))
```

This would make `Vec[A]` a first-class abstraction over length-indexed
vectors, enabling code that works with both.

### 2. Type-erased heterogeneous collections

Without existentials, a list of "things that can be shown" requires a concrete
shared type. With existentials:

```turmeric
(deftype Showable = (exists a. (Show a) => a))

(let [items (list (pack 1    as Showable)
                  (pack "hi" as Showable)
                  (pack 3.14 as Showable))]
  (list-map show items))
```

### 3. Abstract handles / capabilities

Existentials are a natural model for opaque handles where the caller must not
inspect internals:

```turmeric
(deftype Handle[Op] = (exists s. (Op s) => s))
```

This overlaps with the existing `capability.tur` and `:linear` work, and could
provide a cleaner foundation for both.

---

## Implementation Options

Three approaches are ordered from least to most complex.

---

### Option A -- Struct-encoded existentials (no new type-checker machinery)

Encode existentials as structs carrying a value plus a vtable of operations.
No `exists` keyword. The programmer writes the encoding by hand; stdlib
provides helper macros.

```turmeric
;; Manual encoding of (exists a. (Show a) => a):
(defstruct Showable
  (value  :int)                    ; the boxed value (erased type)
  (show-f :ptr<void>))             ; fn [x :int] :cstr

(defmacro pack-showable [x]
  `(Showable ~x (fn [v] (show (unbox v)))))
```

**Complexity:** Low. Requires no changes to the type-checker or parser.
Already partially expressible with existing `defstruct` and function pointers.

**Limitations:**
- Every existential type needs its own hand-written struct.
- No compiler verification that the vtable is correctly typed.
- Cannot express `exists n. SizedVec[n A]` this way -- the hidden variable is
  a type-level Nat, not a value, so there is no vtable to store.

**Verdict:** Covers use cases 2 and 3, but not use case 1 (the
`Vec[A]`/`SizedVec[n A]` unification).

---

### Option B -- `pack`/`open` with typeclass constraints (medium complexity)

Add `exists` as a first-class type constructor, with `pack` (introduction) and
`open` (elimination). The hidden type variable must carry at least one
typeclass constraint.

```turmeric
;; Type syntax:
(exists a. (Constraint a) => T)

;; Introduction -- pack:
(pack value as (exists a. (Show a) => a))

;; Elimination -- open (binding form):
(open expr ([a] [x : T])
  body)   ; a and x are bound here; a must not escape body's type
```

The "no escape" rule is the key invariant: inside the `open` block you have a
fresh abstract type `a` and a value `x : T[a]`, but `a` cannot appear in the
type of any value that flows *out* of the block. The type-checker must enforce
this with a scope check on the existential variable.

**Type-checker changes required:**
- Extend the type grammar with `(exists a. ...)`.
- Extend unification to treat existential variables as skolem constants during
  elimination (they unify with nothing except themselves within their scope).
- Add a scope escape check: after `open`, no reference to `a` may appear in
  the inferred type of `body`.
- Extend `defclass` resolution to thread the hidden constraint witness through
  `pack`/`open` automatically.

**Parser changes:** `pack`, `open`, and `exists` as new keywords.

**Runtime:** No runtime cost beyond what boxing already provides. `pack` is a
no-op at runtime (the value is already boxed as `int64_t`); the constraint
witness is a pointer to a vtable allocated at `pack` time.

**Does this cover use case 1?** Partially. `(exists n. SizedVec[n A])` hides a
*type-level Nat* (`n`), not a value. If `n` is a phantom index (as in the
current `SizedVec` GADT), the hidden variable carries no runtime representation
and there is no vtable. The `open` form would still need to bind `n` as an
abstract phantom type. This is expressible in Option B if the type-checker
treats phantom type variables as valid existential variables -- but it requires
care to avoid the escape of `n` through the length of any array that comes out
of the `open` block.

**Complexity:** Medium. Scope checking for existential variables is the hardest
part; it is well-understood (standard in System F and GHC's implementation) but
requires a non-trivial addition to the Turmeric type-checker.

---

### Option C -- First-class existential types with type functions (high complexity)

Full `exists a. F a` where `F` can be any type function, including ones not
constrained by a typeclass. Subsumes Options A and B.

Adds:
- Type-level lambda / type functions (if not already present).
- Higher-kinded existentials: `exists (f :: * -> *). f A`.
- Interaction with GADTs: GADT constructors already carry implicit existentials
  (e.g. `SVCons` hides the predecessor Nat). Full existentials would make these
  explicit and usable at the type level.

**Complexity:** High. Requires:
- A kind system (to type-check type functions).
- Unification in the presence of higher-kinded variables.
- Careful interaction with the existing GADT elaboration.
- Likely incompatible with full type inference -- annotations required at all
  pack/open sites (same caveat as GHC's `ExistentialQuantification`).

**Verdict:** Deferred. This is the right long-term direction but should follow
dependent types work, not precede it.

---

## Recommended Path

| Phase | Approach | Prerequisite |
|-------|----------|--------------|
| EX0 | Option A -- struct-encoded existentials + macro helpers | None |
| EX1 | Option B -- `pack`/`open` with typeclass constraints | Typeclass system complete |
| EX2 | Revisit Option B for phantom type variables (Vec/SizedVec use case) | TC2 (typed collections) shipped |
| EX3 | Option C -- full existential types | Dependent types phase |

EX0 can ship without any language changes and closes use cases 2 and 3.
EX1 closes them more cleanly and enables the constrained-existential pattern
used widely in Haskell and OCaml. EX2 is what enables Option B from
`typed-collections-plan.md`.

---

## Complexity Summary

| Concern | Option A | Option B | Option C |
|---------|----------|----------|----------|
| Parser changes | None | `pack`, `open`, `exists` keywords | Type-level lambda syntax |
| Type-checker changes | None | Skolemization + scope escape check | Kind system + HK unification |
| Runtime changes | None | Vtable allocation at `pack` | None beyond Option B |
| Inference impact | None | Annotations required at pack/open | Annotations required everywhere |
| Covers Vec/SizedVec unification | No | Partially (EX2) | Yes |
| Risk | Low | Medium | High |

The main implementation risk in Option B is the **scope escape check** --
ensuring that the hidden type variable does not leak out of an `open` block.
This is conceptually simple (a free-variable check on the inferred type of the
body) but interacts subtly with let-generalization, GADTs, and any future
dependent-type elaboration.

---

## Relation to Other Plans

- `typed-collections-plan.md` -- TC2 `Vec[A]` and `SizedVec[n A]` are kept
  separate until EX2 lands. EX2 enables Option B: `Vec[A] = exists n. SizedVec[n A]`.
- `refinement-types-plan.md` -- refinements and existentials are orthogonal but
  interact at the boundary where a refinement narrows the hidden type.
- `currying-plan.md` -- curried functions as existentials (`exists a b. a -> b`)
  is a known encoding; keep compatible.

---

## Detailed Implementation Tasks (Option B)

Tasks are grouped by phase. Each phase builds on the previous. All parser and
type-checker work targets the C interpreter core (`src/`).

---

### Phase EX0 -- Struct-encoded existentials (stdlib only)  (SUPERSEDED)

> **Status: superseded.**  EX1 shipped native `pack`/`open` /
> `exists` syntax, so the struct-encoded fallback EX0 envisioned was
> never needed.  Entries below are left for historical context and
> intentionally remain unchecked.

No interpreter changes. Adds stdlib infrastructure and documents the manual
encoding pattern so code written today can migrate to EX1 later.

- [ ] **EX0-1** Add `stdlib/existential.tur` with:
  - `defmacro make-existential-struct` -- generates a `defstruct` with a boxed
    `value :int` field and one function-pointer field per constraint method.
  - `defmacro pack-existential` -- fills the struct fields from the concrete
    type's typeclass instance.
  - Document that each macro-generated type must be migrated to `pack`/`open`
    once EX1 lands.

- [ ] **EX0-2** Add `Showable` as the canonical worked example in
  `stdlib/existential.tur`, including:
  - `defstruct Showable`
  - `pack-showable` macro
  - `showable-show` accessor
  - A `;;; Since: Phase EX0` docstring block on each exported name.

- [ ] **EX0-3** Add fixture tests in `tests/existential/`:
  - `tests/existential/showable_test.tur` -- pack integers, strings, and floats
    into `Showable`; call `showable-show`; verify output.
  - `tests/existential/handle_test.tur` -- demonstrate an opaque handle using
    the struct encoding.

- [ ] **EX0-4** Update `docs/guides/README.md` with a brief entry for
  existential types pointing at the new stdlib file.

---

### Phase EX1a -- Parser: `exists`, `pack`, `open` syntax

Adds the three new syntactic forms. No type-checking yet; newly parsed nodes
are left as unresolved AST nodes.

- [x] **EX1a-1** Extend the lexer (`src/lex.c` or equivalent) to recognize
  `exists`, `pack`, and `open` as reserved keywords. Add token types
  `TK_EXISTS`, `TK_PACK`, `TK_OPEN`.

- [x] **EX1a-2** Extend the type-expression parser to handle:
  ```
  (exists <var>. (<Constraint> <var>) => <type>)
  (exists <var>. (<C1> <var>) (<C2> <var>) => <type>)
  ```
  Produce an `AST_EXISTS_TYPE` node carrying: bound variable name, constraint
  list, and body type.

- [x] **EX1a-3** Extend the expression parser to handle:
  ```
  (pack <expr> as <exists-type>)
  ```
  Produce an `AST_PACK` node carrying: expression, target existential type.
  Require the `as` keyword between expression and type.

- [x] **EX1a-4** Extend the expression parser to handle the `open` elimination
  form:
  ```
  (open <expr> ([<type-var>] [<val-var> : <body-type>])
    <body>)
  ```
  Produce an `AST_OPEN` node carrying: scrutinee expression, bound type
  variable name, bound value variable name, expected body type annotation, and
  body expression.

- [x] **EX1a-5** Add round-trip pretty-printer support for `AST_EXISTS_TYPE`,
  `AST_PACK`, and `AST_OPEN` so that `--print-ast` output is readable.

- [x] **EX1a-6** Add parse-error tests (fixture files that must fail to parse):
  - `pack` missing `as` keyword
  - `exists` without a constraint
  - `open` with wrong binding arity

---

### Phase EX1b -- Type representation: existential types in the type IR

Extends the internal type representation before wiring up type-checking.

- [x] **EX1b-1** Add a new type kind `TY_EXISTS` to the type IR (e.g.
  `src/types.h`). Fields:
  - `bound_var`: unique type variable ID (a fresh skolem level slot)
  - `constraints`: list of `(typeclass_id, type_arg)` pairs
  - `body`: the body type (a `Type*` that may reference `bound_var`)

- [x] **EX1b-2** Extend the type printer / unparser to emit
  `(exists a. (C a) => T)` syntax for `TY_EXISTS` nodes.

- [x] **EX1b-3** Extend the type comparison and structural equality functions
  to handle `TY_EXISTS` -- alpha-rename bound variables before comparing (bound
  variable name is irrelevant; the structure must match).

- [x] **EX1b-4** Extend the type substitution function to correctly skip
  substitution of the bound variable inside a `TY_EXISTS` body (it is locally
  bound and must not be replaced by an outer unification).

---

### Phase EX1c -- Type-checker: `pack` introduction

Implement type-checking for `pack` expressions. At a `pack` site the concrete
type is known from context; the type-checker must verify that the concrete type
satisfies all constraints named in the target existential.

- [x] **EX1c-1** In the type-checker's expression handler, add a case for
  `AST_PACK`:
  1. Infer the type `T_conc` of the inner expression.
  2. Parse the target `(exists a. (C a) => U)`.
  3. Unify `T_conc` with `U[a := T_conc]` (substitute `a` with `T_conc` in the
     body).
  4. For each constraint `(C a)`, look up the typeclass instance for `C T_conc`.
     Emit a type error if no instance exists.
  5. Return type `(exists a. (C a) => U)` for the overall `pack` expression.

- [x] **EX1c-2** At runtime, `pack` must bundle:
  - The boxed value (already `int64_t`).
  - A pointer to the vtable for each constraint instance discovered in step 4.
  Store these in a small heap-allocated record; the runtime type of the `pack`
  result is a pointer to this record.

- [x] **EX1c-3** Add type-checker tests:
  - `pack 42 as (exists a. (Show a) => a)` -- should type-check.
  - `pack (fn [] 0) as (exists a. (Show a) => a)` -- should fail (no `Show`
    instance for functions).
  - `pack "hello" as (exists a. (Show a) (Eq a) => a)` -- multi-constraint,
    should succeed if both instances exist.

---

### Phase EX1d -- Type-checker: `open` elimination and scope escape check

Implement type-checking for `open` expressions. This is the hardest part of
Option B.

- [x] **EX1d-1** In the type-checker's expression handler, add a case for
  `AST_OPEN`:
  1. Infer the type of the scrutinee. It must be `(exists a. constraints => U)`.
  2. Generate a fresh **skolem constant** `s` (a rigid type variable that
     unifies only with itself). Record `s` at the current scope level.
  3. Bind the type variable name from the `open` binder to `s` in the local
     type environment.
  4. Substitute `a := s` throughout `U` to get `U_s`. Bind the value variable
     to `U_s` in the local term environment.
  5. Resolve all constraints `(C a)` against `s` and add the resulting
     witnesses to the local instance environment (so that methods of `C` are
     callable on the value variable inside the body).
  6. Type-check the body expression, yielding type `T_body`.
  7. **Scope escape check**: verify that `s` does not appear free in `T_body`.
     If it does, emit an error: "existential type variable `a` escapes its
     scope."
  8. Remove `s` from the local type environment. Return `T_body` as the type
     of the `open` expression.

- [x] **EX1d-2** Implement the free-variable scanner used in step 7. Walk the
  `T_body` type tree and collect all skolem constants. Check membership against
  the set of skolems introduced by the current `open` (nested `open` forms
  introduce their own skolems at a deeper level).

- [x] **EX1d-3** Implement skolem level tracking so that nested `open` forms
  do not confuse each other's escape checks. Each `open` increments a skolem
  depth counter; skolems are tagged with their depth; the escape check at depth
  `d` only looks for skolems tagged `d`.

- [x] **EX1d-4** Add type-checker tests:
  - Basic roundtrip: `pack` then `open`, call `show` on the inner value,
    return a `:cstr`. Should type-check.
  - Escape attempt: open an existential, try to return the inner value directly
    (its type contains the skolem). Should fail with the escape error.
  - Nested opens: two independent existentials opened in sequence. Should
    type-check.
  - Nested opens with cross-reference: try to use the skolem from the outer
    `open` as the annotation in the inner `open`'s body type. Should fail.

---

### Phase EX1e -- Runtime: vtable layout and constraint dispatch

Wire up the runtime representation so that constraint methods can be called
inside an `open` block.

- [x] **EX1e-1** Define the heap layout for a packed existential value. Proposed
  layout (similar to the existing typeclass witness records):
  ```c
  struct tur_existential {
    int64_t  value;        /* boxed payload */
    int32_t  n_witnesses;  /* number of constraint vtables */
    void**   witnesses;    /* vtable pointers, one per constraint */
  };
  ```

- [x] **EX1e-2** Emit allocation and initialization of `tur_existential` records
  at each `pack` site. The vtable pointers are resolved at compile time (they
  are static for a given concrete type and typeclass pair).

- [x] **EX1e-3** Inside an `open` block, method calls on the bound value
  variable must be dispatched through the stored vtable pointer rather than
  through the normal typeclass resolution path. Implement this by inserting an
  "existential dispatch" indirection in the code generator for method calls
  whose receiver type is a skolem constant.

- [x] **EX1e-4** Add a GC root for the `tur_existential` record so the GC does
  not collect the vtables while the value is live inside an `open` block.
  *(EX1e-4 broke out as a separate phase: see
  [`existential-gc-plan.md`](existential-gc-plan.md). EX1e shipped the
  allocation; that plan covers the freeing, borrow-check integration,
  cycle-collector visibility, and an optional `:linear` variant.)*

- [x] **EX1e-5** Add runtime tests (integration-level, run through the
  interpreter):
  - Build a `list` of `Showable` existentials, map `show` over them, verify
    the output strings.
  - Verify that the vtable pointer for a multi-constraint existential correctly
    dispatches both methods.

---

### Phase EX1f -- Stdlib integration and documentation

- [x] **EX1f-1** Rewrite `stdlib/existential.tur` to use native `pack`/`open`
  instead of the struct encoding from EX0. Keep the same exported names
  (`Showable`, etc.) so existing code does not break.

- [x] **EX1f-2** Add `exists`/`pack`/`open` to the language reference in
  `docs/guides/README.md` with a short worked example (the `Showable` list from
  motivation section 2).

- [x] **EX1f-3** Add docstrings to any stdlib helpers introduced in EX1f-1.
  Follow the `;;; Since: Phase EX1` convention.

- [x] **EX1f-4** Run `just docs` and verify that the generated API docs include
  the new entries without errors.

- [x] **EX1f-5** Ensure `just test` passes with all EX0 and EX1 fixture tests.

---

### Phase EX2 -- Phantom type variables (`Vec[A]` / `SizedVec[n A]`)

Depends on TC2 (typed collections) being shipped. Extends Option B to support
existentials over phantom type-level variables (indices with no runtime
representation).

- [x] **EX2-1** Audit the type-checker change from EX1b-4 to verify that
  substitution correctly handles phantom type variables (variables that appear
  only in index positions, not in value-carrying fields). *Audit complete:
  `type_expr_from_form` (`src/compiler/elab_types.c`) extends `type_params`
  with each `exists` binder before parsing the body, so a phantom variable
  appearing nested in a TY_APP resolves to a fresh local tyvar (TY_STRUCT
  with `def = NULL`) and does not leak into outer unification scope.
  Verified by `tests/fixtures/ex2-1-phantom-substitution`.*

- [x] **EX2-2** Extend `AST_OPEN` to allow the bound type variable annotation
  to be a phantom (no runtime binding). The value variable still has a concrete
  runtime type; only the index is hidden. *Implemented in `elab_open` via
  `ex2_peel_phantom_app`: when the existential body is a `TY_APP` chain, the
  value variable's type is derived by peeling to the head carrier, so callers
  expecting the bare carrier (e.g. `:SizedVec`) accept the opened value while
  the phantom binder remains unbound at runtime. Verified by
  `tests/fixtures/ex2-2-phantom-open`.*

- [x] **EX2-3** Extend the escape check (EX1d-2) to handle phantom skolem
  variables. A phantom skolem escaping through an array length is as dangerous
  as a value-carrying skolem escaping through a return type; both must be
  rejected. *Implemented two complementary protections in `elab_open`
  (`src/compiler/elab_types.c`): (a) the binder symbol is intentionally
  not added to the body's type-elaboration scope, so a phantom variable
  cannot be referenced in any type annotation inside the body (negative
  fixture `errors/ex2-3-phantom-escape-type`); (b) the new
  `ex2_3_type_has_phantom_residue` walker fires on phantom-indexed
  bodies and rejects any tail type that carries a `TY_APP` with an
  unresolved tyvar argument -- a defensive net for paths that could
  surface a phantom binder through inferred types. Positive fixture
  `ex2-3-phantom-escape-ok` verifies the check is not overly broad.*

- [x] **EX2-4** Implement `Vec[A]` as `(exists n. SizedVec[n A])` in
  `stdlib/vec.tur` (or the typed-collections module from TC2). Provide:
  - `vec-of-sized` -- wraps a `SizedVec[n A]` into a `Vec[A]` via `pack`.
  - `open-vec` -- macro that eliminates a `Vec[A]` into a fresh abstract `n`
    and a `SizedVec[n A]` via `open`.
  *Shipped as `stdlib/vec-existential.tur` (opt-in, like
  `stdlib/existential.tur`).  `vec-of-sized` expands to a native `pack`
  against `(exists [n] (SizedVec n int))`; `open-vec` expands to a
  native `open` with a `[n sv]` binder vector, riding the EX2-2 peel so
  `sv` has the bare carrier type and the EX2-3 escape checks still
  apply.  Verified end-to-end by `tests/fixtures/ex2-4-vec-existential`.*

- [x] **EX2-5** Add fixture tests for `Vec[A]`:
  - Wrap a `SizedVec[3 :int]` into a `Vec[:int]`; open it; verify the length.
  - Attempt to return the `SizedVec` directly from the `open` block; verify
    that the escape check rejects this.
  - Pass a `Vec[:int]` to a function that does not know its length; verify that
    the function can still call `sv-length` inside an `open` block.
  *Shipped three fixtures covering the bullets:
  `tests/fixtures/ex2-5-vec-roundtrip` packs a length-3 `SizedVec` via
  `vec-of-sized` and reports `(sized-vec-len s)` inside `open-vec`;
  `tests/fixtures/errors/ex2-5-vec-escape` returns the bound `SizedVec`
  carrier directly from the body and confirms the EX1d/EX2-3 escape
  check fires (`open: existential type variable 'n' escapes its scope`);
  `tests/fixtures/ex2-5-vec-passthrough` exercises the function-boundary
  case -- the callee receives the size-erased Vec as `:ptr<void>`,
  re-ascribes it to `(exists [n] (SizedVec n int))`, and calls
  `sized-vec-len` inside `open-vec` against three different runtime
  lengths. All three pass under `-Xgadt`.*

---

## Task Summary

| ID | Phase | Description |
|----|-------|-------------|
| EX0-1 | EX0 | `make-existential-struct` / `pack-existential` macros |
| EX0-2 | EX0 | `Showable` worked example in stdlib |
| EX0-3 | EX0 | Fixture tests for struct-encoded existentials |
| EX0-4 | EX0 | Update `docs/guides/README.md` |
| EX1a-1 | EX1a | Lexer: `exists`, `pack`, `open` tokens |
| EX1a-2 | EX1a | Parser: `exists` type expression |
| EX1a-3 | EX1a | Parser: `pack` expression |
| EX1a-4 | EX1a | Parser: `open` binding form |
| EX1a-5 | EX1a | Pretty-printer for new AST nodes |
| EX1a-6 | EX1a | Parse-error fixture tests |
| EX1b-1 | EX1b | `TY_EXISTS` type kind in type IR |
| EX1b-2 | EX1b | Type printer for `TY_EXISTS` |
| EX1b-3 | EX1b | Type equality / alpha-rename for `TY_EXISTS` |
| EX1b-4 | EX1b | Type substitution respects bound variable |
| EX1c-1 | EX1c | Type-check `pack` introduction |
| EX1c-2 | EX1c | Runtime vtable bundling at `pack` site |
| EX1c-3 | EX1c | Type-checker tests for `pack` |
| EX1d-1 | EX1d | Type-check `open` elimination + skolemization |
| EX1d-2 | EX1d | Free-variable scanner for escape check |
| EX1d-3 | EX1d | Skolem depth tracking for nested opens |
| EX1d-4 | EX1d | Type-checker tests for `open` |
| EX1e-1 | EX1e | `tur_existential` heap layout |
| EX1e-2 | EX1e | Emit `tur_existential` allocation at `pack` |
| EX1e-3 | EX1e | Existential dispatch in code generator |
| EX1e-4 | EX1e | GC root for `tur_existential` records |
| EX1e-5 | EX1e | Runtime integration tests |
| EX1f-1 | EX1f | Rewrite stdlib to use native `pack`/`open` |
| EX1f-2 | EX1f | Language reference docs |
| EX1f-3 | EX1f | Docstrings for new stdlib helpers |
| EX1f-4 | EX1f | Verify `just docs` |
| EX1f-5 | EX1f | Full test suite pass |
| EX2-1 | EX2 | Audit phantom variable handling in substitution |
| EX2-2 | EX2 | Phantom-variable support in `open` binder |
| EX2-3 | EX2 | Escape check for phantom skolems |
| EX2-4 | EX2 | `Vec[A]` as `exists n. SizedVec[n A]` |
| EX2-5 | EX2 | Fixture tests for `Vec[A]` |
