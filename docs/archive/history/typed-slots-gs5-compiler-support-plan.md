# Plan: GS5 compiler support for typed container helpers

> **Status:** Proposed
> **Last Updated:** 2026-05-25
> **Type:** Compiler / Codegen / Macro expansion
> **Depends on:** [route-b-typed-slots-plan.md](route-b-typed-slots-plan.md), [typed-slots-generic-substrate-plan.md](typed-slots-generic-substrate-plan.md)

---

## Overview

GS1-GS4 landed the generic substrate, and GS5 has already moved stdlib
container **definitions** onto typed payload slots:

- `Option`, `Pair`, and `Result` payload fields now use their type
  parameters directly.
- `Cons` now specializes its `head` payload slot while intentionally
  leaving the `tail` link on the legacy carrier.
- Direct `make-struct` use sites now emit concrete layouts such as
  `Option__float`, `Pair__int__float`, `Result__float__cstr`, and
  `Cons__float`.

What is still missing is the compiler support needed to migrate the
**public helper APIs** off the `int64_t` carrier path. Today, both of the
obvious GS5 implementation strategies still hit compiler limits:

1. **Function-based helpers** still lower through the carrier ABI for
   aggregate generic results/parameters.
2. **Macro-based helpers** cannot reliably synthesize `.field` access
   forms that survive elaboration as struct field access rather than
   typeclass-method lookup.

This plan isolates that missing support so GS5 can continue without
overloading the already-landed substrate plan.

---

## Current failures

### 1. Generic helper functions still erase aggregate ABI

The substrate is now strong enough to express signatures like:

```turmeric
(defn mk-box [A] [x : A] : (Box A)
  (make-struct Box x))

(defn get-box [A] [b : (Box A)] : A
  (.value b))
```

but these still fail in codegen once `A` is instantiated to a concrete
struct-app or other non-scalar layout-changing type. The generated C still
treats the helper boundary as `int64_t`, producing errors like:

```text
return (int64_t){.value = x};
return (b).value;
initializing 'Box__float' with an expression of incompatible type 'int64_t'
```

Relevant surfaces:

- `src/compiler/emit_fns.c` — function signatures use
  `e->type.as.fn.result_full_type` / `arg_full_types` when available, but
  they ultimately rely on `type_c_name(...)`.
- `src/compiler/types.c` — `type_c_name(TY_APP)` only emits a concrete
  C name when `type_has_concrete_codegen_layout(...)` can prove the full
  application is concrete; otherwise it falls back to `int64_t`.
- `src/compiler/elab_call.c` — named type variables are collected and
  instantiated for call results, but the instantiated type does not yet
  imply a specialized function ABI or clone selection.

### 2. Macro-generated field access still fails in important elaboration paths

The other obvious GS5 route is to make helpers like `pair-fst`,
`pair-snd`, `thead`, `unwrap`, and `ok-val` into macros that expand to
direct field access at the concrete use site:

```turmeric
(defmacro pair-fst [p]
  ...)
```

In practice this currently runs into a macro/elaboration seam:

- compile-time helpers in `src/compiler/elab_macros.c` can build
  `.field`-shaped symbols via `dot-sym`,
- but macro-expanded `.fst` / `.snd` forms still get diagnosed as
  typeclass-method lookup (`no typeclass method found for 'fst'`) before
  they reliably settle into the `EX_GET_FIELD` path in
  `src/compiler/elab_typeclasses.c`.

Scoping note from the current GS5 investigation: plain quasiquote is **not**
the whole problem. A minimal macro like `` `(.fst ~p) `` can round-trip to
direct field access in simple cases. The failure reliably reproduces once the
same field-access form appears in a `definstance` method body for a
parameterized struct helper path, which points at a deeper receiver-typing gap
in addition to the macro constructor gap.

This blocks the "expand helper APIs to `make-struct` / `.field`" strategy
even for simple containers like `Pair` that do not need inactive-payload
defaults.

### 2b. `definstance` method parameters still erase applied-struct receivers

While testing the macro route, an additional compiler limit showed up:
`definstance` method parameters for parameterized struct constructors are still
collapsed to the carrier ABI too early. Inside an instance body, a receiver
that should still look like `:(Pair A B)` or `:(GPair A B)` instead behaves
like `:int`, so `(.fst x)` falls through to typeclass method lookup rather than
the struct-field path.

Today this comes from `src/compiler/elab_typeclasses.c`, where method
parameter typing rewrites parameterized struct-constructor arguments back to
`TYPE_INT` for ABI compatibility before the body is elaborated. That is enough
for codegen, but too early for elaboration paths that need the receiver's full
applied-struct type.

This matters for GS5 even if CS2 lands, because the first stdlib migration
targets (`Pair` helpers and later `Option` / `Result` accessors) want to be
usable both at ordinary call sites and inside constrained instance bodies.

### 3. Applied-struct return instantiation is still incomplete downstream

GS4 fixed scalar generic result crossings, but aggregate returns still have
holes. A helper like:

```turmeric
(defn id-box [A] [b : (Box2 A)] : (Box2 A)
  b)
```

can type-check, but downstream uses still observe incomplete
instantiation in some paths. The known symptom is that a call returning
`:(Box2 float)` can still surface as `(type-app Box2 tyvar)` to later
consumers instead of preserving the concrete `float` argument all the way
through.

That incompleteness matters for both strategies above:

- macro-based helpers eventually expand into ordinary calls and field
  access,
- function-based helpers need the return type to be concretely known if
  the ABI is going to change.

---

## Goals

1. Let generic stdlib helper APIs return and accept concrete applied-struct
   values without collapsing back to `int64_t`.
2. Make macro-generated `.field` access elaborate through the same
   `EX_GET_FIELD` path as handwritten field access.
3. Preserve the current carrier fallback for genuinely polymorphic call
   sites.
4. Keep the scope targeted to GS5 helpers rather than introducing full
   arbitrary whole-program monomorphization.

## Non-goals

- Replacing the existing carrier ABI for all polymorphic functions.
- Reworking `Option`/`Result` runtime representation in this plan.
- Solving TS4 or TS5 completely here; this plan only provides the missing
  compiler support they need.

---

## Recommended approach

### Phase CS1 — Finish aggregate return instantiation in elaboration

**Goal.** If a call to a generic function can infer concrete type arguments
for an applied-struct result, preserve that concrete result type all the
way to downstream consumers.

**Compiler changes.**

- `src/compiler/elab_call.c`
  - extend `call_collect_type_bindings(...)` /
    `call_instantiate_type(...)` validation for aggregate `TY_APP`
    results, not just scalar `TY_TYVAR` crossings.
  - ensure instantiated aggregate result types propagate into the
    `Expr->type` seen by later `EX_GET_FIELD`, ascription, and call
    compatibility checks.
- Audit any helper that still reconstructs result types from erased
  `TypeKind` metadata instead of the fully instantiated type tree.

**Acceptance tests.**

1. `id-box [A] [b :(Box2 A)] :(Box2 A)` called with `(Box2 float)` yields
   downstream field access type `:float`.
2. Nested returns like `:(Wrap2 (Box2 A))` preserve concrete inner args
   through the call site.

### Phase CS1b — Preserve applied-struct receiver types in `definstance` bodies

**Goal.** Elaborate instance method bodies against the receiver's full
applied-struct type even when the emitted ABI still uses the carrier path.

**Compiler changes.**

- `src/compiler/elab_typeclasses.c`
  - keep both views of each instance-method parameter:
    - the **full elaboration type** (`TY_APP` / parameterized `TY_STRUCT`) used
      for scope bindings and body elaboration,
    - the **C ABI type** used when building the emitted function signature.
  - stop collapsing parameterized struct-constructor receivers to `TYPE_INT`
    before elaborating the method body.
  - preserve enough type information on the method parameter binding for
    `elab_method_call(...)` / `EX_GET_FIELD` to recognize `(.field x)` as
    struct field access inside instance bodies.
- Audit any later pass that assumes `binding->type` and the emitted ABI type are
  identical for instance methods, and split those responsibilities if needed.

**Acceptance tests.**

1. A local `GPair[A B]` with an `Eq [GPair]` instance can use a macro-expanded
   `(.fst x)` / `(.snd x)` accessor inside the instance body without triggering
   `no typeclass method found for 'fst'`.
2. Existing collection-instance dispatch behavior remains unchanged for methods
   that genuinely operate on carrier-only values.

### Phase CS2 — Add a macro-safe field-access constructor

**Goal.** Macro authors can synthesize field access without depending on
ad hoc quoted `.field` symbols surviving elaboration.

**Compiler changes.**

- `src/compiler/elab_macros.c`
  - add a dedicated compile-time builtin that constructs a field-access
    form directly, instead of overloading `dot-sym` (which is documented
    as producing method-call symbols).
  - document that this constructor is for struct-field access, not typeclass
    method dispatch, so stdlib helper macros do not need to rely on ad hoc
    quoted `.field` symbols.
  - alternatively, teach macro expansion to mark `.field` symbols created
    at compile time so elaboration defers method lookup until the receiver
    type has been checked against the struct-field path.
- `src/compiler/elab_typeclasses.c`
  - ensure macro-generated field-access forms enter the same
    `EX_GET_FIELD` branch as handwritten `(.field x)` forms.
- `stdlib/macros.tur`
   - either repair `dot` so it can be used as the public helper it claims to
    be, or explicitly keep CS2's new field-access constructor separate from
    `dot` until the method-call macro is fixed. The current `dot` expansion is
    not a sufficient workaround for GS5 by itself.

**Acceptance tests.**

1. A macro-defined `pair-fst` that expands to field access works on
   `(Pair int float)` values.
2. The same macro-defined accessor also works when expanded inside a
   `definstance` method body whose receiver is a parameterized struct value.
3. Existing method-style macro uses like `.show` continue to work.

### Phase CS3 — Selective concrete ABI specialization for generic helpers

**Goal.** When a generic `defn` is called at a concrete instantiation whose
parameter/result layout is not representable as `int64_t`, emit a concrete
specialized entry point instead of forcing the call through the carrier.

**Why selective specialization?**

Full arbitrary monomorphization is still out of scope, but GS5 helper
functions only need a narrower rule:

- if the inferred concrete instantiation changes the ABI
  (`type_c_name(TY_APP)` would otherwise want a concrete struct typedef),
  emit a specialized clone for that instantiation;
- otherwise keep using the existing generic carrier ABI.

**Compiler changes.**

- `src/compiler/types.c`
  - reuse the existing concrete struct-app registry / mangling machinery as
    part of a function-specialization key.
- `src/compiler/emit_module.c`
  - add a per-module registry for specialized generic helper clones.
- `src/compiler/emit_fns.c`
  - emit specialized function signatures using the fully concrete
    `arg_full_types` / `result_full_type`.
- `src/compiler/emit_expr.c`
  - select the specialized clone at concrete call sites.
- `src/compiler/elab_call.c`
  - attach enough instantiated type information to the call node for
    codegen to choose between the generic carrier ABI and the specialized
    concrete ABI.

**Acceptance tests.**

1. `mk-box [A] [x :A] :(Box A)` called at `A=float` emits and calls a
   concrete helper returning `Box__float`.
2. `pair-second [A B] [p :(Pair A B)] :B` called at `(Pair int float)`
   emits a concrete helper that accepts `Pair__int__float`.
3. A genuinely polymorphic call site still uses the old `int64_t` path.

### Phase CS4 — Re-express GS5 helper APIs on top of the new support

**Goal.** Migrate the public container helpers in the smallest-risk order.

**Suggested order.**

1. `Pair` accessors/constructors — simplest aggregate surface.
2. `List` accessors over `Cons[A]` once `head` and recursive `tail`
   boundaries are stable.
3. `Option` / `Result` accessors.
4. `Option` / `Result` constructors once the representation/default-value
   story is settled.
5. `Vec` APIs after the aggregate helper path is proven on smaller
   containers.

**Acceptance tests.**

- `tpair`, `pair-fst`, `pair-snd`
- `thead`, `ttail`
- `unwrap`, `ok-val`, `err-val`
- a typed-slots fixture that exercises helper composition, not just direct
  `make-struct`

---

## Open design choices

### Macro route vs specialized-function route

The compiler should support **both**, but they solve different pieces:

- macros are attractive for pure accessors/constructors because they move
  work to the use site and avoid ABI questions,
- specialized generic helpers are still needed for reusable APIs that are
  awkward as macros, for nested helper composition, and for keeping the
  stdlib surface uniform.

Recommendation: land **CS2 first** if it is small, then **CS1 + CS3**
immediately after so GS5 does not get stuck on a macro-only path.

### Existing `dot` macro vs a dedicated field-access constructor

The stdlib already has a `dot` macro, so there is an understandable temptation
to route GS5 helper rewrites through it. The current investigation suggests
that should be treated as a separate compatibility question, not as the GS5
field-access solution itself:

- the existing `dot` helper is a **method-call surface** (`(. obj msg ...)` ->
  `(.msg obj ...)`), not a dedicated struct-field constructor,
- the current implementation is not a proven workaround for GS5 field helpers,
  and in local probing it still errors during macro expansion,
- even a repaired `dot` would not address the `definstance` receiver-type
  erasure described in CS1b.

Recommendation: keep CS2 free to add a purpose-built field-access constructor
even if `dot` is later repaired and reused on top.

### Representation of inactive `Option` / `Result` payloads

This plan intentionally does not pick the final representation. Typed
payload slots make the old "set inactive field to 0" constructor pattern
invalid for some types, but that is a container/runtime design question on
top of the compiler work here.

---

## Suggested validation matrix

1. **Elaboration**
   - generic aggregate return instantiation
   - macro-generated field access
   - instance-body field access with applied-struct receivers preserved
2. **Codegen**
   - specialized helper signature emission
   - specialized helper call selection
3. **Stdlib**
   - `Pair` helper migration
   - `List` helper migration
4. **Regression**
   - existing GS1-GS4 typed-slots fixtures
   - existing `tpair` / `tlist` / `option` / `result` fixtures

---

## Exit criteria

This plan is complete when:

- a generic helper can accept and return concrete applied-struct values
  without collapsing back to `int64_t`,
- macros can synthesize field access reliably, including from inside
  `definstance` bodies,
- at least one real stdlib helper family is migrated onto the typed-slot
  substrate using the new support,
- Route B / GS5 can move from "typed container defs landed" to "typed
  helper APIs landing" without relying on handwritten carrier-era inline C.
