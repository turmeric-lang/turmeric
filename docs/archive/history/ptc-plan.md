# Parametric Type Constraints Plan (PTC)

> **Status:** Complete
> **Last Updated:** 2026-05-23
> **Type:** Compiler / Type System

---

## Overview

Parametric type constraints allow a typeclass instance for a parameterized
type to require that its element type also satisfies a typeclass.  The
canonical example is:

```turmeric
;; "Eq holds for Vec[A] provided Eq holds for A"
(definstance Eq [Vec] [(Eq A)]
  (eq? [a b] (tvec-eq? a b (fn [x y] (.eq? x y)))))
```

Without this, all `Eq[Collection]` instances in `typed-collections-plan.md`
(TM0-6, TC1-5, TC1-9, TC1-17, TC1-22, TC1-25, TC2-15) must be written as
plain `-eq?` functions with an explicit element-comparator argument instead
of dispatching through the typeclass.

The work is split into four sub-phases:

| Sub-phase | Scope |
|-----------|-------|
| PTC1 | Parse and store constraint vectors in `definstance` |
| PTC2 | Validate constraints at `definstance` time for primitive argument types |
| PTC3 | Skip instances at lookup time if stored constraints are not satisfied |
| PTC4 | Full type-parameter substitution during constraint checking |

---

## Current State

### PTC1 -- Done

`elab_definstance` (`src/compiler/elab_typeclasses.c`, ~line 757) parses
an optional constraint vector at the position after the type-argument vector:

```turmeric
;; Two accepted syntaxes:
(definstance Clone [Pair a b] [(Clone a) (Clone b)] ...)   ; vector of lists
(definstance Clone [Pair a b] [Clone a Clone b]     ...)   ; flat alternating
```

Constraints are stored as a `TypeConstraint` array on each
`TypeClassInstance` (`src/compiler/typeclass.h`, lines 73-77):

```c
TypeConstraint *type_param_constraints;
uint8_t         n_type_param_constraints;
```

where `TypeConstraint` pairs a `TypeClass *` with a `Type type_arg`.

### PTC2 -- Done (primitive types only)

`elab_definstance` validates constraints immediately for primitive `type_arg`
values (`TY_INT`, `TY_BOOL`, `TY_CSTR`, `TY_NIL`, `TY_FLOAT`,
`TY_PTR_VOID`).  If no instance exists for the required typeclass and
primitive type, the compiler emits `TUR-E0015_TYPECLASS_CONSTRAINT_NOT_SATISFIED`
and aborts.  Non-primitive type args are stored but not validated (deferred).

Relevant code: `src/compiler/elab_typeclasses.c`, lines 921-963.

### PTC3 -- Partial

`typeclass_env_lookup_instance` (`src/compiler/typeclass.c`, lines 113-119)
calls `typeclass_instance_constraints_satisfied` before returning a match and
skips the instance if it returns false.

The same check runs in the method-dispatch loop
(`src/compiler/elab_typeclasses.c`, lines 1900-1910).

**Current limitation** (`src/compiler/typeclass.c`, lines 196-208):
`typeclass_instance_constraints_satisfied` does a direct lookup of each
stored `type_arg` without substituting type parameters from the actual
lookup context.  This means a constraint `(Eq A)` on an instance for `Vec`
is checked by looking up `Eq` for whatever type was stored in the `TypeConstraint`
at `definstance` time -- which is the abstract `Vec` struct type, not the
concrete element type from the call site.

### PTC4 -- Done

The missing piece: when checking constraints for a parameterized-struct
instance, substitute the concrete element types (derived from the call-site
type) for the abstract type parameter names before looking up the constraint.

For `(eq? my-vec other-vec)` where `my-vec : Vec[int]`:
1. Dispatch finds the `Eq [Vec]` instance.
2. That instance has constraint `(Eq A)` where `A` is the 0th type param of
   `Vec`.
3. PTC4 substitutes `A → int` and checks that `Eq [int]` exists.
4. It does, so the instance is selected.

---

## Phase PTC4: Full Type-Parameter Substitution

### Problem statement

`TypeConstraint.type_arg` is a `Type`.  For a constraint like `(Eq A)` in
`(definstance Eq [Vec] [(Eq A)] ...)`, the parser currently stores
`type_args[j]` for the symbol `A`, which is the abstract struct type for
`Vec` (not the element type).  There is no `TY_TVAR` kind, so type parameter
references cannot be distinguished from concrete types at constraint-check time.

The call-site context has the information: `elab_typeclasses.c` line 1905
passes `obj->type` (a `TY_STRUCT` for `Vec`) to
`typeclass_instance_constraints_satisfied`.  This carries the `StructDef *`
which knows the param names (`["A"]`), but not the concrete instantiation
(i.e., `A = int` is not encoded in `TY_STRUCT`).

Two related sub-problems must be solved together:

1. **Constraint representation** -- constraints on type parameters must be
   stored in a way that survives to lookup time and indicates *which* type
   parameter they apply to (by position, not by the abstract type value).

2. **Concrete type threading** -- the concrete element type must reach
   `typeclass_instance_constraints_satisfied` so substitution is possible.
   Either the call-site elaborator extracts it, or the lookup type must carry
   it (e.g., via `TY_APP`).

### Approach

Use a **positional index** in `TypeConstraint` to refer to a type parameter
by its position in the instance's type-arg list (or, for single-param structs,
by position in the struct's `type_params` array).  Store the index alongside
the `TypeClass *`.  At constraint-check time, the concrete element types are
passed in parallel and substituted by index.

This avoids adding a new `TypeKind` and keeps the change local to the
constraint infrastructure.

---

### Tasks

| ID | Task | File(s) | Status |
|----|------|---------|--------|
| PTC4-1 | Add `param_idx` field to `TypeConstraint`; set it during parsing when a constraint references a type parameter by name | `typeclass.h`, `elab_typeclasses.c` | Done |
| PTC4-2 | Update `typeclass_instance_constraints_satisfied` signature to accept a `concrete_types` array + count alongside the lookup type args | `typeclass.h`, `typeclass.c` | Done |
| PTC4-3 | In `typeclass_instance_constraints_satisfied`, when a constraint's `param_idx >= 0`, substitute `concrete_types[param_idx]` as the type to check against; fall back to the stored `type_arg` for concrete (non-param) constraints | `typeclass.c` | Done |
| PTC4-4 | Extract concrete element types at call sites in the method-dispatch loop; pass them to `typeclass_instance_constraints_satisfied` via the updated signature | `elab_typeclasses.c` | Done |
| PTC4-5 | Extract concrete element types at call sites in `typeclass_env_lookup_instance`; same update | `typeclass.c` | Done |
| PTC4-6 | Update PTC2 validation in `elab_definstance` to handle the new `param_idx` field (skip concrete-type check when a param index is set) | `elab_typeclasses.c` | Done |
| PTC4-7 | Add fixture `tests/fixtures/ptc4-basic`: a parameterized struct with a constrained `definstance`, called via `.method` | `tests/fixtures/` | Done |
| PTC4-8 | Add fixture `tests/fixtures/ptc4-failing-constraint`: constrained `definstance` where the element type lacks the required instance; verify compile-time error | `tests/fixtures/` | Done |
| PTC4-9 | Register PTC4 fixtures in `tests/run.sh` and `tests/run-turi.sh` | `tests/` | Done |

---

### PTC4-1 detail: `TypeConstraint` changes

**`src/compiler/typeclass.h`**

```c
/* Phase PTC4: A -1 param_idx means the constraint applies to a concrete
 * type stored in type_arg (existing PTC1/PTC2 behaviour).
 * A non-negative param_idx means the constraint applies to the type
 * parameter at that position in the instance's type-arg list, and
 * type_arg is unused for lookup (substituted at check time). */
typedef struct TypeConstraint {
    TypeClass *typeclass;
    Type       type_arg;     /* concrete type (param_idx < 0) */
    int8_t     param_idx;    /* >= 0: positional type-param reference */
} TypeConstraint;
```

**`src/compiler/elab_typeclasses.c`** -- constraint parsing (~line 824)

When the constraint's type argument symbol matches a name in the instance's
`type_arg_syms`, record its position as `param_idx` instead of copying the
abstract `type_args[j]` as a concrete type:

```c
for (uint8_t j = 0; j < n_type_args; j++) {
    if (type_arg_syms && type_arg_syms[j] &&
        type_arg_syms[j] == type_param_name) {
        type_param_constraints[i].param_idx = (int8_t)j;
        /* type_arg left as zero / unused */
        break;
    }
}
```

---

### PTC4-2 / PTC4-3 detail: `typeclass_instance_constraints_satisfied`

**`src/compiler/typeclass.h`** -- update signature:

```c
bool typeclass_instance_constraints_satisfied(
    const TypeClassInstance *inst,
    Type *lookup_type_args, uint8_t n_lookup_args,
    const Type *concrete_elem_types, uint8_t n_concrete,
    const TypeClassEnv *env);
```

**`src/compiler/typeclass.c`** -- update body (~line 191):

```c
Type required_type;
if (tc->param_idx >= 0 && tc->param_idx < (int8_t)n_concrete) {
    /* Phase PTC4: substitute concrete element type */
    required_type = concrete_elem_types[tc->param_idx];
} else {
    /* PTC1/PTC2/PTC3 path: use stored concrete type */
    required_type = tc->type_arg;
}
TypeClassInstance *constraint_inst =
    typeclass_env_lookup_instance(env, required_tc, &required_type, 1);
if (!constraint_inst) return false;
```

---

### PTC4-4 / PTC4-5 detail: extracting concrete element types

The concrete element types for a `TY_STRUCT` value are **not** currently
encoded in the `Type` struct -- `TY_STRUCT` only carries a `StructDef *`.
They are only known at the elaboration site where the variable was first
typed (e.g., where `tvec-push!` was called with a literal `int`).

Two options:

**Option A (simpler):** At the method-dispatch call site in
`elab_typeclasses.c` (~line 1900), look up the variable's inferred element
type from the argument expressions passed to the enclosing method call.  For
`(eq? my-vec other-vec)`, the first argument (`my-vec`) is `TY_STRUCT Vec`;
we have no concrete element type unless it was threaded through separately.
This option requires the elaborator to track per-variable element types.

**Option B (preferred):** Represent `Vec[int]` as `TY_APP(Vec, int)` in the
type system and propagate this type through `let` bindings.  `TY_APP` already
exists (`src/compiler/types.h`, line 90).  The elaborator for `tvec-push!`
already knows the element is `int`; it needs to record this in the variable's
type rather than collapsing to bare `TY_STRUCT`.  Then:

- The method-dispatch loop extracts `elem_type = obj->type.as.app.arg` (for
  `TY_APP` objects) and passes it as `concrete_elem_types[0]`.
- For multi-param types (`Map[K V]`), `TY_APP` chains give both `K` and `V`.

Option B is the more principled path and aligns with the existing `TY_APP`
infrastructure.  It does require updating type assignment for parameterized
struct construction calls (`tvec-new`, `tmap-new`, etc.) to return `TY_APP`
rather than bare `TY_STRUCT`.

---

### PTC4-7 fixture sketch

```turmeric
;; tests/fixtures/ptc4-basic/input.tur

(defclass Printable [a]
  (to-int [x] : int))

(definstance Printable [int]
  (to-int [x] x))

;; A simple wrapper struct with one type param.
(defstruct Box [A] (val :int))

(defn box-new [v : int] : int
  ```c
  int64_t *b = malloc(sizeof(int64_t));
  *b = v;
  return (int64_t)(intptr_t)b;
  ```)

;; Constrained instance: Printable[Box[A]] given Printable[A]
(definstance Printable [Box] [(Printable A)]
  (to-int [b] : int
    ```c
    return *(int64_t*)(intptr_t)b;
    ```))

(println (.to-int (box-new 42)))   ;; => 42
```

Expected stdout: `42`

---

## Remaining Work After PTC4

All downstream tasks are now complete.

| Task | Description | Status |
|------|-------------|--------|
| TM0-6 | `definstance Eq [Map] [(Eq K) (Eq V)]` using `tmap-eq?` | Done |
| TC1-5 | `definstance Eq [Vec] [(Eq A)]` using `tvec-eq?` | Done |
| TC1-9 | `definstance Eq [Cons] [(Eq A)]` using `tlist-eq?` | Done |
| TC1-17 | `definstance Eq [Option] [(Eq A)]` using `toption-eq?` | Done |
| TC1-22 | `definstance Eq [Result] [(Eq A) (Eq B)]` using `tresult-eq?` | Done |
| TC1-25 | `definstance Eq [Pair] [(Eq A) (Eq B)]` using `tpair-eq?` | Done |
| TC2-15 | `definstance Eq [Set] [(Eq A)]` using `tset-eq?` | Done |

Note: element comparison in collection Eq bodies uses integer (`=`) equality
rather than dispatching through `.eq?`. This is correct for all primitive
element types (int, bool, cstr). Full recursive structural equality (e.g.
`Vec[Vec[int]]`) requires dictionary passing to method bodies, deferred to a
future phase.

---

## File Map

| File | Role |
|------|------|
| `src/compiler/typeclass.h` | `TypeConstraint` struct, `typeclass_instance_constraints_satisfied` declaration |
| `src/compiler/typeclass.c` | `typeclass_env_lookup_instance`, `typeclass_instance_constraints_satisfied` implementation |
| `src/compiler/elab_typeclasses.c` | `elab_definstance` (parsing + PTC2 validation), method-dispatch loop (PTC3 call) |
| `src/compiler/types.h` | `TypeKind` enum (`TY_APP`, `TY_STRUCT`), `StructDef` (type params) |
| `tests/fixtures/ptc2-test/` | Existing test for PTC2 primitive constraint validation |
| `tests/fixtures/ptc3-test/` | Existing test for PTC3 constraint-based instance skipping |
| `tests/fixtures/ptc4-basic/` | To be created: basic PTC4 substitution test |
| `tests/fixtures/ptc4-failing-constraint/` | To be created: negative test for unsatisfied constraint |
