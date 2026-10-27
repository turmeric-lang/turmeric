# Plan: Generic substrate for typed slots (`defstruct` + `defn`)

> **Status:** In progress — GS1 landed, GS2 landed, GS3 landed, GS4 landed, GS5 in progress
> **Last Updated:** 2026-05-25
> **Type:** Compiler / Language
> **Depends on:** [route-b-typed-slots-plan.md](route-b-typed-slots-plan.md) TS1-TS2
> **Companion for remaining GS5 compiler work:** [typed-slots-gs5-compiler-support-plan.md](typed-slots-gs5-compiler-support-plan.md)

---

## Overview

TS2 landed the compiler-only `EX_REINTERPRET` node, but TS3 is blocked
on missing generic substrate rather than missing container code.

Two gaps are now explicit:

1. **`defstruct` cannot declare fields in terms of its own type
   parameters.**
   `src/compiler/elab_structs.c` records `StructDef.type_params`, but a
   field like `(value A)` is still rejected as an unrecognized type.
   The parser only routes compound `F_LIST` forms through
   `type_expr_from_form(...)`; bare-symbol field annotations still go
   through `parse_struct_field_type(...)` and user-type lookup.

2. **`defn` support is inconsistent across annotation positions and still
   lacks an explicit binder surface.**
   `src/compiler/elab_fns.c` already supports generic parameter
   annotations in the runtime parameter vector (for example, `[a x :a]`
   works, and return `: a` works), but the fused return form `:a` still
   fails and there is no explicit function-level type-parameter surface
   comparable to `defstruct Box [A]`. That makes typed-slot APIs like
   `tcons`, `thead`, `some`, `unwrap`, `pair-fst`, and `vec-get`
   awkward or impossible to express directly as clean generic
   functions.

This plan adds the missing substrate in two coordinated pieces:

- **Parameterized struct fields** so `defstruct Cons [A] (head A) ...`
  becomes legal and retains the full type.
- **Consistent generic annotation syntax** so both fused (`:A`) and
  spaced (`: A`) forms work everywhere this plan touches.
- **Parameterized function signatures** so `defn` can bind `A` once and
  use it consistently in parameter and return annotations.

With both in place, TS3 can specialize concrete container layouts
without relying on ad hoc `:int` carrier APIs.

---

## Current blockers

### `defstruct`

Current behavior:

- `StructDef.type_params` and `StructDef.n_type_params` already exist in
  `src/compiler/types.h`.
- `type_expr_from_form(...)` already knows how to resolve bare type
  parameters when it is given a `type_params` environment.
- `elab_structs.c` does **not** pass that environment when parsing field
  types, so:

  ```turmeric
  (defstruct Box [A]
    (value A))
  ```

  fails with:

  ```text
  defstruct field 'value' has unrecognized type :A
  ```

Downstream consequences:

- `StructField.full_type` cannot carry `TY_TVAR` / type-parameter
  structure for container payload fields.
- `EX_MAKE_STRUCT` currently returns `type_struct(def)`, not a concrete
  `TY_APP` instantiation.
- `EX_GET_FIELD` can return `full_type`, but it has no substitution
  step from `A` to the concrete type argument at the use site.
- `emit_module.c` emits exactly one C typedef per `StructDef`, so there
  is no place to materialize distinct layouts like `Cons__float`.
- The failure reproduces for both the bare-symbol field form and the
  explicit annotation spelling:

  ```turmeric
  (defstruct Box [A]
    (value A))

  (defstruct Box [A]
    (value : A))
  ```

### `defn`

Current behavior:

- `elab_fns.c` can parse complex parameter types and already carries
  full types for `forall`, `exists`, `TY_APP`, unions, intersections,
  and function types.
- It also has `return_app_type` plumbing for concrete type threading.
- But there is no explicit function-level type-parameter binder
  analogous to `defstruct [A]`.

Observed syntax matrix:

```turmeric
(defn id [a x : a] : a x)  ; works
(defn id [a x : a] : a x)  ; works
(defn id [a x : a] : a x)  ; fails today
(defn id [A] [x : A] : A x); fails today
```

That leaves three problems:

1. Generic container helpers cannot declare their element type once and
   reuse it across the full signature.
2. Fused and spaced generic annotation spellings are not accepted
   uniformly across parameters, returns, and struct fields.
3. Parameter annotations like `:A` currently behave more like implicit
   "unknown type variable" placeholders than lexically bound generics.
4. There is no obvious anonymous-function analogue for typed generic
   callbacks once TS3/TS5 start depending on them.

---

## Goals

### Primary goals

1. Allow parameterized structs to use their own type parameters in field
   annotations.
2. Preserve instantiated container types through `make-struct`, field
   access, let-binding, and call boundaries.
3. Accept both fused (`:A`) and spaced (`: A`) generic annotation syntax
   in every surface this plan touches.
4. Add analogous explicit type-parameter binding for `defn` (and likely
   `fn`) so generic container APIs are expressible without carrier-era
   `:int` signatures.
5. Enable per-instantiation C layout specialization for concrete
   primitive arguments.

### Non-goals

- Full whole-program monomorphization of arbitrary generic functions.
- Retiring implicit `:a` / `TY_TYVAR` behavior everywhere in one step.
- Solving TS4/TS5 in this plan; they should consume this substrate.

---

## Recommended surface syntax

### `defstruct`

Keep the existing syntax and make both spellings work:

```turmeric
(defstruct Cons [A]
  (head A)
  (tail :(Option (Cons A))))

(defstruct Cons [A]
  (head : A)
  (tail : (Option (Cons A))))
```

Both should normalize to the same internal type form before elaboration.

### `defn`

Add an explicit type-parameter vector immediately after the function
name:

```turmeric
(defn tcons [A] [h : A t : (Option (Cons A))] : (Cons A)
  ...)

(defn thead [A] [xs : (Cons A)] : A
  ...)
```

Anonymous `fn` should get the analogous form:

```turmeric
(fn [A] [x : A] : A x)
```

Compatibility / normalization rule:

- Wherever generic annotations are accepted, both fused and spaced
  spellings should work:

  ```turmeric
  (defn id [A] [x : A] : A x)
  (defn id [A] [x : A] : A x)
  ```

- Existing implicit generic `defn` forms should also accept both:

  ```turmeric
  (defn id [a x : a] : a x)
  (defn id [a x : a] : a x)
  (defn id [a x : a] : a x)
  ```

Rationale:

- Matches `defstruct` / `defdata` / `defgadt` visually.
- Avoids overloading the runtime parameter vector.
- Lets `type_expr_from_form(...)` reuse the same `type_params` path
  already used by type-level forms.
- Removes an unnecessary parser/elaborator distinction between `F_KEYWORD`
  and `F_TYPE_ANN` spellings for the same generic type variable.

---

## Phase plan

## Progress checklist

- [x] GS1 — Parameterized `defstruct` field types
- [x] GS2 — Instantiated struct types survive elaboration
- [x] GS3 — Monomorphized struct layout emission
- [x] GS4 — Explicit type parameters for `defn` / `fn`
- [ ] GS5 — Migrate typed-slot stdlib APIs onto the substrate

**Worktree update (2026-05-26):** GS5 is now underway on the stdlib side.
`Option`, `Pair`, and `Result` `defstruct` payload fields now use their
type parameters directly, and `Cons` now specializes its `head` payload
field while intentionally leaving the `tail` link on the legacy carrier.
Focused typed-slots fixtures now emit concrete layouts such as
`Option__float`, `Pair__int__float`, `Result__float__cstr`, and
`Cons__float` (with `double head; int64_t tail;`). The legacy helper
functions are still carrier-era APIs, so GS5 remains open until those
surfaces are migrated too. The missing compiler support needed for that
next step is tracked in
[`typed-slots-gs5-compiler-support-plan.md`](typed-slots-gs5-compiler-support-plan.md).

### Phase GS1 -- Parameterized `defstruct` field types

**Goal.** Make `defstruct Box [A] (value A)` legal and preserve the full
field type as a type-variable reference instead of collapsing it to an
opaque `:int`.

**Compiler changes.**

- `src/compiler/elab_structs.c`
  - When parsing field types, route bare symbol forms through
    `type_expr_from_form(...)` when the struct has type params.
  - Also route the explicit annotation spelling (`: A`) through the same
    path instead of leaving it in the keyword-only field parser.
  - Pass `def->type_params` / `def->n_type_params` into
    `type_expr_from_form(...)`.
  - Store the resulting full type in `StructField.full_type`.
  - Derive the C-level storage kind from that full type:
    - `TY_TVAR` / `TY_APP` / `TY_EXISTS` / `TY_FORALL` still lower to
      opaque carrier storage in the unspecialized case.
    - Concrete primitives keep their concrete storage kind.

**Acceptance test.**

- `tur check` succeeds for:

  ```turmeric
  (defstruct Box [A]
    (value A))

  (defstruct Box [A]
    (value : A))
  ```

### Phase GS2 -- Instantiated struct types survive elaboration

**Goal.** When a struct is constructed or accessed under a concrete
instantiation like `(Box float)`, the IR keeps that instantiated type
instead of degrading back to `type_struct(def)`.

**Compiler changes.**

- `src/compiler/elab_structs.c`
  - `elab_make_struct(...)` should be able to produce a `TY_APP`
    result type when the target struct instantiation is known.
- `src/compiler/elab_typeclasses.c`
  - `EX_GET_FIELD` elaboration should recover the receiver's concrete
    type arguments from a `TY_APP` chain and substitute them into the
    field's `full_type`.
- Add a small reusable helper in `src/compiler/elab_types.c` or
  `types.c` to substitute struct type parameters inside a `Type`.

**Acceptance tests.**

1. A value ascribed `:(Box float)` yields field-access type `:float`.
2. A value ascribed `:(Pair int float)` yields `.fst :int` and
   `.snd :float`.

**Worktree update (2026-05-26).**

- `elab_defstruct(...)` now parses both bare (`A`) and spaced (`: A`)
  field annotations against the struct's type-parameter environment and
  stores named type variables in `StructField.full_type`.
- `elab_make_struct(...)`, `EX_GET_FIELD`, and `set! (.field ...)`
  now preserve/infer `TY_APP` struct instantiations and substitute those
  concrete type arguments back into field types during elaboration.
- Call boundaries now preserve full applied parameter types too, so a
  function expecting `:(Box float)` rejects `:(Box int)` instead of
  collapsing both to a bare `TY_APP` kind match.
- GS2 is now complete as an **elaboration** milestone, and GS3 has now
  landed the matching codegen path.
- Concrete `TY_APP` struct instantiations are registered during codegen,
  given deterministic mangled C names such as `Box__float` and
  `Duo2__int__float`, and emitted as standalone typedefs before any
  forward declarations or function bodies use them.
- `EX_MAKE_STRUCT` and typed function signatures now lower concrete
  applied struct types through those emitted typedefs, so direct runtime
  layouts like `double value;` and `int64_t fst; double snd;` appear in
  the generated C for concrete instantiations.

### Phase GS3 -- Monomorphized struct layout emission

**Goal.** Emit distinct C layouts for concrete primitive instantiations
instead of one erased layout per `StructDef`.

**Compiler changes.**

- `src/compiler/emit_module.c`
  - Add an instantiation table for concrete struct applications used by
    the compilation unit.
  - Emit one concrete C typedef per used instantiation, e.g.
    `Cons__float`, `Pair__int__float`.
- `src/compiler/emit_expr.c`
  - `EX_MAKE_STRUCT` should emit the concrete instantiated C type name.
  - `EX_GET_FIELD` should use the concrete instantiated receiver type.
- `src/compiler/types.c` or a new helper
  - Add deterministic mangling for `TY_APP` struct instantiations.

**Boundary rule.**

- If a concrete instantiation flows into a polymorphic slot, insert
  TS2 `EX_REINTERPRET` exactly at the generic/concrete boundary rather
  than inside user stdlib code.

**Acceptance tests.**

1. `Cons<float>` emits a `double head;` field.
2. `Option<float>` emits a `double value;` field.
3. `Pair<int, float>` emits `int64_t fst; double snd;`.

### Phase GS4 -- Explicit type parameters for `defn` / `fn`

**Goal.** Add function-level generic binders so APIs can express and
preserve the same type parameters as parameterized structs.

**Surface syntax.**

```turmeric
(defn id [A] [x : A] : A x)
(fn [A] [x : A] : A x)
```

**Compiler changes.**

- `src/compiler/elab_fns.c`
  - Parse an optional type-parameter vector between the function name
    and runtime parameter vector.
  - Normalize fused and spaced generic annotation spellings in both
    parameter and return positions before type elaboration.
  - Pass those type parameters into parameter and return annotation
    parsing via `type_expr_from_form(...)`.
  - Preserve explicit `TY_TVAR` structure instead of treating unknown
    annotations as ad hoc implicit variables.
- `src/compiler/types.h`
  - If needed, extend function-type metadata to record declared type
    parameters separately from rank-2 `forall`.
- `src/compiler/elab_call.c`
  - Ensure call-site instantiation can thread concrete types from
    arguments into the annotated return type for generic container
    helpers.

**Compatibility rule.**

- Existing implicit `:a` behavior can remain temporarily, but explicit
  `[A]` binders become the preferred and better-scoped form.
- As part of that compatibility path, the compiler should accept both
  `:a` and `: a` in parameter and return positions, rather than only the
  currently working subset.

**Acceptance tests.**

1. `(defn id [A] [x :A] :A x)` type-checks and works for `:int` and
   `:float`.
2. `(defn id [A] [x : A] : A x)` also type-checks.
3. `(defn id [a x :a] :a x)` keeps working for the implicit-generic
   compatibility path.
4. `(defn tcons [A] [h :A t :(Option (Cons A))] :(Cons A) ...)`
   type-checks.
5. `(fn [A] [x :A] :A x)` type-checks in a let-bound position.

**Worktree update (2026-05-25).**

- `defn` and `fn` now accept an explicit type-parameter vector between the
  name and runtime parameter vector, so forms like `(defn id [A] [x :A] :A x)`
  and `(fn [A] [x : A] : A x)` elaborate directly.
- Both fused and spaced generic annotations now preserve named `TY_TYVAR`
  structure through parameter and return parsing instead of collapsing bare
  binder uses into opaque placeholder structs.
- The existing implicit compatibility form now works too: leading binder-style
  names such as `(defn id [a x :a] :a x)` are recognized as generic binders
  when later annotations reference them.
- Call elaboration now binds named type variables from actual arguments,
  instantiates generic scalar results, and inserts TS2 reinterpret nodes at
  scalar generic/carrier boundaries so `:float` generic identity-style calls
  round-trip correctly at runtime.

### Phase GS5 -- TS3 container migration

**Goal.** Once GS1-GS4 exist, migrate the typed-slot stdlib containers
onto the new substrate.

**Stdlib changes.**

- `stdlib/list.tur`
  - Replace carrier-era signatures like `tcons [h :int t :int] :int`
    with explicit generic forms.
- `stdlib/option.tur`
  - Change `value :int` storage to `value A`.
- `stdlib/result.tur`
  - Change `ok-val :int`, `err-val :int` to `A` / `B`.
- `stdlib/pair.tur`
  - Change `fst :int`, `snd :int` to `A` / `B`.
- `stdlib/vec.tur`
  - Special-case `data` buffer element type for concrete primitive
    instantiations; keep pointer-erased fallback for unspecialized or
    non-primitive cases.

**Acceptance tests.**

- `tests/fixtures/typed-slots/cons-float.tur`
- `tests/fixtures/typed-slots/option-float.tur`
- `tests/fixtures/typed-slots/pair-int-float.tur`
- `tests/fixtures/typed-slots/polymorphic-cons-boundary.tur`

---

## File touch points

| Area | Files | Why |
|---|---|---|
| Type parsing | `src/compiler/elab_types.c`, `src/compiler/elab_internal.h` | Reuse `type_expr_from_form(...)` with explicit type-param environments. |
| Struct elaboration | `src/compiler/elab_structs.c`, `src/compiler/types.h` | Accept `A` in field types and retain `full_type`. |
| Function elaboration | `src/compiler/elab_fns.c`, `src/compiler/elab_call.c` | Add explicit `defn` / `fn` type params and thread instantiations. |
| Type substitution | `src/compiler/types.c`, `src/compiler/types.h` | Substitute `A -> float` inside field and return types. |
| IR / field access | `src/compiler/expr.h`, `src/compiler/elab_typeclasses.c` | Preserve instantiated types on `EX_MAKE_STRUCT` / `EX_GET_FIELD`. |
| C emission | `src/compiler/emit_module.c`, `src/compiler/emit_expr.c` | Emit concrete instantiated struct layouts and accesses. |
| Tests | `tests/fixtures/typed-slots/*`, `tests/run-flags.sh` | Add typed-slot regression coverage. |

---

## Suggested implementation order

1. **GS1** -- legalize `A` in `defstruct` fields.
2. **GS2** -- preserve instantiated types through IR.
3. **GS4** -- add explicit `defn` / `fn` type params.
4. **GS3** -- emit concrete instantiated layouts.
5. **GS5** -- migrate stdlib containers and add TS3 fixtures.

Why this order:

- GS1 + GS2 make the type information exist.
- GS4 makes the stdlib API expressible cleanly.
- GS3 turns that type information into concrete layouts.
- GS5 is the first large user of all three pieces.

---

## Test plan

### Compiler substrate

1. `defstruct` accepts bare type parameters in field positions.
2. Field access on an ascribed instantiated struct yields substituted
   concrete types.
3. Generic `defn` signatures with explicit `[A]` binders parse and
   type-check.

### Codegen

1. `emit-c` for `Cons<float>` shows `double head;`.
2. `emit-c` for `Option<float>` shows `double value;`.
3. Generic/concrete boundary sites use `EX_REINTERPRET`, not user code
   unions.

### End-to-end

1. `thead` over a `Cons<float>` returns `1.5`.
2. `pair-snd` over `Pair<int, float>` returns the correct `float`.
3. `option-map` over `Option<float>` keeps the payload concrete through
   the mapped function.

---

## Open questions

1. **Should explicit function type params be required, or should `:A`
   continue to auto-bind implicitly?**
   Recommendation: keep implicit behavior for back-compat, but prefer
   explicit `[A]` for new code.

2. **Should fused and spaced generic annotation spellings be normalized
   everywhere, even if one path already works today?**
   Recommendation: yes. This plan should treat `:A` and `: A` as
   equivalent in `defstruct`, `defn`, and `fn` surfaces.

3. **Do we need `fn` support in the same phase as `defn`?**
   Recommendation: yes. TS5/HKT work will likely want anonymous generic
   helpers too.

4. **How broad should layout specialization be initially?**
   Recommendation: concrete primitive instantiations only
   (`bool`, `int*`, `uint*`, `float*`, `cstr`, `ptr<void>`), with
   erased fallback for non-primitives.

5. **Where should struct instantiations be recorded?**
   Recommendation: compilation-unit emission context, not `StructDef`
   itself, so repeated uses dedupe naturally and cross-module naming
   stays deterministic.

6. **Should `Vec<A>` element buffers specialize in the same phase?**
   Recommendation: probably after `Cons` / `Option` / `Pair` / `Result`,
   because `Vec` changes allocation and growth code rather than only
   field layout.
