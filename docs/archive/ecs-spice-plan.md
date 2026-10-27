---
title: Turmeric ECS Spice Plan
category: Planning
description: An Entity-Component-System library for Turmeric, inspired by Haskell's `aztecs` and `apecs`, paired with `tur-raylib`, designed to push compile-time type safety further than Haskell's holistic constraint-based approach.
---

# `tur-ecs` -- Plan

> **Status 2026-06-18.** Phases E0, E1', E1 (variadic), E2 (incl.
> compile-time write-cap enforcement via I1-I6), E2d (associated-type
> storage projection, P1-P6 + P5b variadic-arity collapse), E2c
> (sized-rectangular iteration, slices 1-12 against the resolved
> [`ecs-sized-world-plan.md`](../archive/ecs-sized-world-plan.md)), E3 (raylib
> companion), and E4 (comparison writeup --
> [`docs/guides/ecs-guide.md`](../guides/ecs-guide.md) and
> [`docs/guides/ecs-vs-haskell-ecs.md`](../guides/ecs-vs-haskell-ecs.md))
> all shipped, plus the sized parallel-scheduler wiring (direction 1)
> via the new `sized-defsystem-scheduled` macro and the `world-resize`
> existential wrapper via `sized-defworld-world-resize` -- see the E2c
> sized-scheduler section below. Residual follow-ups (none
> design-blocking):
>
> 1. **Routing `defcomponent-accessors` through `StorageOps`** -- still
>    waiting on struct-element projection (independent of PR #420).
> 2. **Sized-scheduler direction 2** (cross-world / heterogeneous
>    scheduling -- a `System` / `Stage` polymorphic in the world
>    type). The compiler-side gap-H blocker (typeclass-bounded
>    `[S] [(StorageOps S)]` wrappers failing to monomorphise at multiple
>    carrier backends) is now closed -- see
>    [`docs/archive/bounded-storageops-wrapper-heterogeneous-monomorphisation-gap.md`](../archive/bounded-storageops-wrapper-heterogeneous-monomorphisation-gap.md).
>    The remaining work is spice-side (building the world-type-polymorphic
>    `System` / `Stage` surface on top of the now-available wrapper).
>    Direction 1 (monomorphic single-world scheduling) shipped via
>    `sized-defsystem-scheduled`.
>
> E2b (refinement-typed APIs) remains gated on refinement types, which
> are still in plan. The original prerequisite-tracking plan has been
> archived at
> [`docs/archive/history/ecs-prereq-plan.md`](../archive/history/ecs-prereq-plan.md).

## Goal

Build an ECS (Entity-Component-System) spice that lives at
`../turmeric-spices/spices/ecs/` and pairs cleanly with `tur-raylib` for
real-time games and simulations. The core idea: **take aztecs's structural
clarity and apecs's ergonomic queries, but lean on Turmeric's
type-system features (HKTs, typeclasses with coherent dispatch,
substructural/refinement types, sized types) to push more invariants to
compile time** than Haskell's typeclass-soup approach can.

This is partly a library, partly an empirical comparison: how far can a
language with strict, dispatch-coherent typeclasses and refinement types
get with the ECS pattern before the compiler has to "give up" and start
trusting the programmer?

## Inspiration: aztecs vs. apecs vs. us

| Concern | apecs (Haskell) | aztecs (Haskell) | tur-ecs (proposed) |
|---|---|---|---|
| Component declaration | `instance Component Pos where type Storage Pos = Map Pos` | `Component` typeclass + `derive`d storages | `defcomponent` macro that lowers to a `definstance Component` + chosen storage |
| Storage selection | Type family `Storage c` | Associated type / `Storage c` | Associated *type member* on `Component` typeclass; default = dense `vec`, override per component |
| Query language | `cmap`, `cfold` over tuples `(Pos, Vel)` | Arrow-like `Query` ADT, runnable on a `World` | First-class `(query [Pos Vel])` macro that elaborates to a typed iterator with HKT'd row type |
| World | Monolithic `World` record type | Heterogeneous map keyed by `TypeRep` | Generated `World` struct (one storage field per registered component), known at compile time |
| Systems | `System w a` reader-monad style | Arrow / `runSystem` | Plain `defn`s over a `World` handle; scheduler is a separate module |
| Parallelism | Implicit / careful | `aztecs-stm` / scheduler awareness | Explicit "read-set / write-set" types on `defsystem`; scheduler can statically prove non-conflict |
| Error mode | Runtime: missing storage = crash | Runtime: missing component = `Nothing` | Compile-time: missing storage is unbound name; missing component is a type error |

Where Haskell uses one big `Has w c => System w c -> ...` constraint and
lets GHC unify, we instead **bake the component set into the world type**
and let elaboration check membership directly. This is what "pushing
holistic to compile-time" means here.

## Concrete shape

### Entities

```turmeric
;; Opaque handle. Generationally-versioned so dangling references are
;; caught cheaply at runtime, not as use-after-free memory corruption.
(defopaque Entity :int)
```

Internally an `Entity` packs `index : u32` + `generation : u32`. We
expose only `entity=?`, `entity-alive?`, and constructors via the
`World` API. **Dead-entity reads return `(none)`; this is the only API
surface in v1** -- a "strict-aliveness" surface that turns
use-after-despawn into a *type* error would require refinement types,
which are not shipping (see
[docs/archive/refinement-types-plan.md](refinement-types-plan.md)).
The `option`-returning forgiving surface is what every shipping ECS in
every language exposes anyway; we are not giving anything up in
practice.

### Components

```turmeric
#lang sweet-exp

defstruct Pos [x : float y : float]
defstruct Vel [x : float y : float]
defstruct Hp  [cur : int  max : int]

;; defcomponent picks a storage and registers the component name into
;; a compile-time component registry consulted by defworld.
defcomponent Pos :storage :dense
defcomponent Vel :storage :dense
defcomponent Hp  :storage :sparse        ;; few entities have HP
```

`defcomponent` lowers to:

1. A `definstance Component <T>` that ties `T` to its storage's
   associated type member.
2. A registration hook so `defworld` can see `T` was declared.

Storage backends in v1: `:dense` (parallel vector keyed by entity
index), `:sparse` (HAMT from `Entity` -> `T`), `:tag` (zero-payload
"has component X" set, useful for `Dead`, `Frozen`, etc.).

### World

```turmeric
defworld GameWorld
  [Pos Vel Hp Sprite Player Enemy]
```

`defworld` is a macro that:

- Generates a `defstruct GameWorld` with one field per component
  (typed to that component's storage).
- Generates `world-new`, `spawn`, `despawn`, and per-component
  `get-Pos`, `set-Pos`, `remove-Pos`, ... accessors. (These are not the
  user-facing API; queries are -- but they exist so queries elaborate
  to plain field access.)
- Records the component set on the type so the elaborator can reject
  a `(query [Foo])` if `Foo` was not listed.

Two worlds with overlapping but distinct component sets are
*different types*. This is the central typing trick: we sacrifice
"polymorphic system over any world that has `Pos`" (which is what
apecs's `Has w c` constraint buys) in exchange for "the world is a
named, closed type and the compiler knows every cell of it."

Two recovery paths for systems that should be polymorphic over any
world containing a given component set:

**(a) Monomorphic systems against a concrete world (v1 default).** Every
`defsystem` names its concrete world type. For a single-game project
this is fine -- the world is declared once and reused. The raylib demo
in E3 uses this path.

**(b) Typeclass-bounded systems (v1 optional, lands with E2).** Each
component generates a `HasComponent` class (`HasPos`, `HasVel`, ...).
`defworld` emits the corresponding instances automatically. A system
that wants polymorphism states it via class constraints:

```turmeric
defn integrate [W] [(HasPos W) (HasVel W)] [w : W dt : float] : void
  ...
```

This is the apecs `Has w c` encoding, lifted into Turmeric's coherent
class system. We *don't* need refinement types for this -- it falls out
of the typeclass machinery we already have. The only thing we lose
relative to a refinement-typed `/has` is that the class dispatch is
resolved at call sites by dictionary lookup rather than by literal
field-access elaboration; that is the same trade-off Haskell makes and
the runtime cost is one indirection per call.

Refinement-typed world bounds (`/has Pos /has Vel` as a predicate on
the world type) remain a v2 ambition; see
[docs/archive/refinement-types-plan.md](refinement-types-plan.md).

### Queries

The ergonomic centerpiece. Two surfaces:

**Imperative** (apecs `cmap` style):

```turmeric
for-each [(p :Pos) (v :Vel)] world
  fn [e p v]
    set-Pos! world e
      Pos {p.x + {v.x * dt}}
          {p.y + {v.y * dt}}
```

**Functional** (aztecs `Query` style, arrows-flavored):

```turmeric
def integrate-q
  query
    in  [Pos Vel]
    out [Pos]
    fn [p v]
      Pos {p.x + {v.x * dt}}
          {p.y + {v.y * dt}}

run-query! integrate-q world
```

Both lower to the same primitive: a typed iterator over the
intersection of the relevant storages. `in` and `out` are declared
because they feed system scheduling (below).

A query's row type is an HKT'd tuple -- this is where Turmeric's
existing HKT machinery (see `docs/guides/hkt-guide.md`) earns its
keep. `Query in out` is a functor over `out`, an applicative when
joining queries, and (per the comonad story) extendable via focus on a
single entity.

### Filters

```turmeric
query
  in   [Pos]
  with [Player]            ;; tag filter: only entities with Player
  without [Dead]           ;; tag filter: exclude Dead
  fn [p] ...
```

`with`/`without` are not in the row -- they constrain the iterator,
not its yielded tuple. Statically, both must be registered components
on the world.

### Systems and scheduling

A `defsystem` is a `defn` plus declared read/write sets, expressed
as component-name vectors (the actually-shipped I3 surface; the
original plan-time bitmask-int form was a transitional shape):

```turmeric
(defsystem physics
  [Pos Vel]     ;; :reads
  [Pos]         ;; :writes
  body)

(defsystem render
  [Pos Sprite]  ;; :reads
  []            ;; :writes
  body)
```

The scheduler (`stage`) takes a list of systems and a world and runs
them in dependency order. **Two systems with disjoint write sets and
no write/read overlap can run in parallel**, and the scheduler can
*prove* that statically from the declared sets -- no runtime conflict
detection, no STM. Mis-declaring (writing to a component you didn't
list) is a compile-time error because the elaborator only exposes
`set-X!` capabilities for `X` in `:writes`.

This is the substructural-types angle: `:reads` and `:writes` define
linear capabilities the system body is allowed to use. `defsystem`
binds `<Comp>-read-cap : (ReadCap Comp)` and `<Comp>-write-cap :
(WriteCap Comp)` in body scope for each entry in the vectors;
`defcomponent-accessors` emits `set-<Comp>!` / `get-<Comp>` that
require the corresponding cap as their first argument. A body that
calls `set-Vel!` without `Vel` in `:writes` has no `Vel-write-cap`
in scope and fails to elaborate. Shipped 2026-06-11 via Phases I1-I6
(see [`docs/archive/history/ecs-defsystem-write-caps-not-enforced.md`](../archive/history/ecs-defsystem-write-caps-not-enforced.md)).
For the underlying machinery see `docs/guides/substructural-types-guide.md`.

### Raylib integration

`tur-ecs-raylib` is a thin companion spice that provides:

- Standard components: `Pos2 {x y :float}`, `Vel2`, `Rot`, `Sprite
  {tex :Texture w h :float}`, `Color`, `Collider2`.
- Standard systems: `integrate-2d`, `render-sprites`, `tick-input`.
- A `with-game-loop` macro:

```turmeric
with-game-loop world :title "Demo" :size [800 600] :fps 60
  stage
    tick-input
    physics
    render-sprites
```

`with-game-loop` wraps `init-window` / `window-should-close` /
`begin-drawing` / `end-drawing` / `close-window`, and the body runs the
scheduler each frame.

## Where Turmeric's types pull ahead

These are the v1 wins -- claims that hold against the type-system
machinery actually shipping today (no refinement types required).

1. **Closed world type.** `defworld` produces a single named struct.
   "Does this world have `Hp`?" is field lookup, not constraint
   solving. Adding a component is an explicit change to the world
   declaration; you cannot accidentally add one via instance import.
   *Querying a component the world doesn't declare is an
   unbound-name error from the elaborator -- compile-time membership
   checking without needing refinements.*
2. **Coherent typeclass dispatch.** `Component T` has exactly one
   instance per `T`. No orphan-instance footguns; storage choice is a
   property of the component, not the import graph.
3. **Static read/write effects.** `:reads`/`:writes` become first-class
   substructural capabilities (we have `-Xsubstructural` shipping
   today). apecs's `cmap` does not have this; it trusts the programmer.
   The scheduler proves non-conflict statically and the elaborator only
   exposes `set-X!` for `X` in `:writes`. **This is the single biggest
   delta vs. Haskell ECSes and it needs zero new type-system work.**
4. **Generational entity safety.** Use-after-despawn cannot corrupt
   memory: a stale handle's generation counter mismatches the slot's
   current generation, so `get-Pos` returns `(none)`. This is a
   *runtime* check (one u32 compare per access), not a type error --
   but every shipping ECS in every language does it this way. The
   compile-time version (refinement-typed alive-set) is a v2 ambition,
   not a v1 advantage.
5. **No runtime `TypeRep` lookup.** aztecs's heterogeneous map is fast
   in practice but ultimately a hash-by-typerep. Our equivalent is a
   struct field access resolved by the elaborator -- zero overhead, and
   a typo on a component name fails to compile rather than returning
   `Nothing`.
6. **Typeclass-bounded polymorphism, coherently dispatched.** Systems
   that want to be world-polymorphic state it via `(HasPos W)
   (HasVel W)` constraints (encoding (b) under "Concrete shape > World"
   above). Closer to Rust trait bounds than Haskell free-form
   constraints; checked at the call site, not by global instance search.

The cost: less ad-hoc polymorphism than Haskell, and no compile-time
"this handle is alive" / "this world has component X" *refinement*
guarantees. We get the latter via struct-field membership (which is
arguably stronger -- it's structural, not predicate-based) and accept
the former as runtime-checked.

### Deferred (refinement-typed surfaces)

Still gated on refinement types
([`docs/archive/refinement-types-plan.md`](refinement-types-plan.md)),
which have not yet shipped:

- **Refinement-typed entities** (`entity-alive!` strict API). The
  forgiving `option`-returning API is the only v1 surface.
- **Refinement-typed world bounds** (`/has Pos` as a predicate on `W`).
  Use the typeclass encoding in v1.

### Shipped (E2c + E2d wiring)

- **Statically-rectangular sized iteration.** Sized-types SZ6-SZ8
  landed 2026-06-10 (see
  [`docs/archive/history/sized-types-phantom-index.md`](../archive/history/sized-types-phantom-index.md));
  the bounded-capacity world API designed in
  [`ecs-sized-world-plan.md`](../archive/ecs-sized-world-plan.md) (Q1-Q4
  settled) and the spice-side `sized-defworld` / `sized-for-each`
  / cap-gated sized accessors / `sized-defsystem` /
  `sized-defworld-copy-into` / fallible `sized-spawn` family
  shipped as E2c slices 1-12. See the E2c section below for
  per-slice detail and residual follow-ups.
- **Associated-type storage projection.** Associated type members
  on typeclasses landed in turmeric 0.20.0 (see
  [`docs/archive/history/typeclass-associated-types-missing.md`](../archive/history/typeclass-associated-types-missing.md));
  `defcomponent` now emits a real `Component` instance with an
  associated `Storage` type, and `defworld` projects through it.
  Shipped as E2d slices P1-P6 + P5b variadic collapse; see the
  E2d section below.

## Out of scope (v1)

- **Reactive / arrow-style systems** as the *only* surface. We provide
  the functional `Query` form because it composes, but the imperative
  `for-each` is first-class. No FRP layer in v1.
- **Hierarchies / parent-child transforms.** Add as a follow-up spice
  (`tur-ecs-scene`) once raylib usage shows what shape is needed.
- **Serialization / save-state.** Likely a follow-up using the
  `application-image-dumps` work.
- **STM-style optimistic concurrency.** The static scheduler is the
  story; if it does not suffice, we will revisit.

## Phasing

The original phasing was split into a **v1 track** (everything that
landed against the type system as it shipped at the time) and a **v2
track** (gated on elaborator prerequisites). The v1 phases and all
three v2 prereqs (variadic HKT rows, associated types, sized cross-
parameter unification + bounded-capacity world) landed across the
2026-06-11 and 2026-06-17 ship windows, plus the spice-side E2d
(P1-P6 + P5b) and E2c (slices 1-12) wiring. The residual work is one
deferred surface (refinement-typed APIs, E2b) plus three follow-ups
whose prereqs are already in: sized-world parallel-scheduler
generalisation, a `world-resize` existential wrapper, and routing
`defcomponent-accessors` through `StorageOps`.

### Shipped (2026-06-11)

**E0 -- skeleton.** `defcomponent`, `defworld`, dense storage,
manual `spawn`/`get`/`set`. Verified by `tests/spawn1k.tur` in the
spice.

**E1' -- fixed-arity queries.** Originally scoped as a `(query1
[...])` ... `(query5 [...])` macro family. Subsumed by E1 (below):
when the variadic-HKT-rows prereq landed, the spice rewrote queries
as a single truly-variadic `for-each` macro driven by recursive
helpers, with no arity cap. The originally-planned `queryN` shims
were never built; `for-each1` / `for-each2` / `for-each3` remain
exported as ergonomic shorthands but reduce to the variadic
`for-each` internally.

**E1 -- variadic queries.** Originally scoped as v2-track, gated on
variadic HKT rows. The HKT-rows prereq shipped in turmeric 0.20.0
(six layers landed; see
[`docs/archive/history/variadic-hkt-rows-missing.md`](../archive/history/variadic-hkt-rows-missing.md)),
and the spice's `for-each` macro consumes that surface directly.
`Query #row{...} #row{...}` carries the kind-`[*]` row type the plan
called for. Sparse + tag storages, `with`/`without` filters,
`defquery` and `run-query!` all ship against this.

**E2 -- systems and scheduler.** `defsystem` with `:reads`/`:writes`,
sequential scheduler, parallel scheduler proving non-conflict and
running disjoint systems on the fiber pool. Substructural
capabilities back the write access -- the load-bearing v1 win.
Auto-generated `HasComponent` classes (the typeclass-bounded
polymorphism path) ship via `defcomponent-class` /
`defcomponent-class-instance`.

Compile-time write-cap enforcement landed via Phases I1-I6 (see
[`docs/archive/history/ecs-defsystem-write-caps-not-enforced.md`](../archive/history/ecs-defsystem-write-caps-not-enforced.md)
for the original gap report and the full implementation log).
`defsystem`'s `:reads`/`:writes` are component-name vectors (breaking
change from the prior bitmask-int form); `defcomponent-accessors`
emits `set-<Comp>!` / `get-<Comp>` that require a `WriteCap<Comp>` /
`ReadCap<Comp>` first arg; the macro binds those caps in body scope
iff the comp is in `:reads` / `:writes`. A body that writes a
component it did not declare fails to elaborate with `unbound symbol
'<Comp>-write-cap'`. Regression-tested by
`tests/fixtures/errors/ecs-defsystem-writes-unauthorized/`.

**E3 -- raylib companion.** `tur-ecs-raylib` ships at
`../turmeric-spices/spices/ecs-raylib/` with standard 2D components,
integration / rendering systems, and a `with-game-loop` macro.
Aliveness is `option`-returning throughout; the demos use
typeclass-bounded systems where useful and monomorphic systems
otherwise.

### Shipped (continued)

**E4 -- comparison writeup.** Both deliverables shipped:
`docs/guides/ecs-guide.md` (the in-tree introduction) and
[`docs/guides/ecs-vs-haskell-ecs.md`](../guides/ecs-vs-haskell-ecs.md)
(the apecs / aztecs / tur-ecs side-by-side walking through a small
bouncing-balls simulation in all three frameworks and tabulating
what moved from runtime to compile time). The headline-table row is
write-set enforcement -- the cap-gating that shipped in Phase I lets
the tur-ecs column claim compile-time `:writes` enforcement honestly
against apecs's and aztecs's trust-the-programmer baseline.

### Shipped (E2d -- 2026-06-17)

**E2d -- associated-type storage projection.** A `Component`
typeclass in `ecs/world` carries an associated type member
`(type Storage : Type)`; `(defcomponent T)` lowers to a
`(definstance Component [T] (type Storage = (Dense T)))`
registration and `defworld` projects every component field type
through `(Storage T)` instead of the prior hard-coded `: int`.
Storage backend choice is now a property of the component, visible
on the world's type to every downstream consumer. The critical
path landed in order:

- **P1 -- Typed storage opaques per backend.** `ecs/storage`
  exports `(Dense A)`, `ecs/sparse` exports `(Sparse A)`,
  `ecs/tag` exports `Tag`. Carrier still `:int`; a one-token `::`
  ascription crosses the boundary for hand-rolled handles.
  **Breaking:** bare-`int` "marker" components no longer compile
  -- a `(defopaque Marker :int)` (or real struct) plus a
  `(defcomponent ...)` registration is required.
- **P2 -- `Component` class with associated `Storage`.**
- **P3 -- `defstruct` accepts `(Storage T)` in field position**
  (verification fixture green; no elaborator work needed).
- **P4 -- `defcomponent` macro emits the instance.**
- **P5 -- `defworld` projects field types through `(Storage T)`.**
  Removes `defworld`'s dependency on the macro-time storage
  registry.
- **P5b -- variadic `defworld` (uncapped arity).** The former
  `defworld--0..5` per-arity cascade is collapsed into one
  variadic body via a recursive `world-fields` helper macro,
  riding the 2026-06-17 turmeric fixes for `~@`-splice into a
  vector literal and nested user-macro calls from inside a `~@`
  splice. `tests/spawn1k-wide.tur` is the eight-component
  regression.
- **P6 (stretch) -- Polymorphic storage ops via a single-param
  class.** `ecs/storage-ops` ships `(defclass StorageOps [S]
  (type Elem : Type) (storage-insert! ...) (storage-get ...)
  (storage-has? ...))` with instances for `(Dense A)` and
  `(Sparse A)`. `Tag` is deliberately out (payload-less). One
  follow-up remains: routing `defcomponent-accessors` through
  `StorageOps` is **deferred** until struct-element projection
  lands -- those accessors carry struct components that
  monomorphise `Elem` to the int64 carrier and mismatch the C
  ABI, and the cap-gated passing-test surface is too large to
  regress for no behavioural gain today. Bounded-polymorphic
  wrappers (`[S] [(StorageOps S)]`) also remain off the shipped
  surface (the gap-H typeclass-bounded-wrapper limitation);
  monomorphic dispatch works end to end.

### Shipped (E2c -- 2026-06-17)

**E2c -- sized-rectangular dense iteration.** The 2026-06-12
reclassification correctly observed that SZ6-SZ8 cross-parameter
unification needs the size index to ride a constructor chain, and
that a bounded-capacity world API was the right landing. That
design plan ([`ecs-sized-world-plan.md`](../archive/ecs-sized-world-plan.md))
settled Q1-Q4 and the spice-side slices then landed in sequence
against the resolved surface:

- **Slices 1-3 -- sized storage shapes + `sized-defworld`.**
  `(SizedDense n A)`, `(SizedSparse n A)`, `(SizedTag n)` ship
  with `n` threaded structurally; `sized-defworld` emits an
  `n`-polymorphic world `defstruct` whose dense fields all share
  `n`, so SZ8 proves rectangularity across storages without a
  runtime probe.
- **Slice 3b -- variadic `sized-defworld` (uncapped arity).**
  Mirrors P5b; bounded-capacity worlds may carry any number of
  components. `tests/sized-world-wide.tur` is the five-component
  regression.
- **Slices 4 / 4b / 4c -- spawn / despawn / generational handles.**
  `sized-spawn!` allocates from a slot free-list (slice 4b reuse)
  and returns a packed generational `Entity` (slice 4c). The
  matching `sized-despawn`, `sized-alive?`,
  `sized-slot-generation` surface lifts the sized world to the
  same use-after-despawn safety the unsized world has had since
  E0. **Breaking:** `sized-spawn!` returns `Entity` and
  `sized-despawn` takes `Entity` (was bare slot `int` in slice 4b).
- **Slice 5 -- `sized-for-each` payoff macro.** New
  `ecs/sized-query` module: every storage access goes through the
  typed `(SizedDense n Comp)` surface and the loop bound is the
  first storage's static `sized-dense-cap`; the runtime
  `__fe-min-cap` probe is gone for sized worlds. This is the
  load-bearing E2c win.
- **Slice 6 -- `sized-world-tagged?` / `sized-world-untagged?`.**
  Sized analogues of the unsized `with`/`without` filter pair,
  composing with `sized-for-each` via `when`/`unless`.
- **Slice 7 -- `sized-defcomponent-accessors`.** Cap-gated
  `get-<Comp>` / `set-<Comp>!` / `has-<Comp>?` for sized worlds,
  `n`-polymorphic at the accessor signature so one family
  elaborates against every `(GameWorld (Static k))` shape. Cap
  surface unchanged: caps pair with component type, not world
  shape.
- **Slice 8 -- `sized-defsystem`.** Single `n`-polymorphic
  `(defn name [n] [^borrow w : (WorldName n)] : nil ...)` with
  the same cap-binding rules and auto-consume as the unsized
  `defsystem`. Negative regression
  (`tests/errors/sized-defsystem-undeclared-write.tur`) confirms
  the "writes to a component not in `:writes` is a compile-time
  error" guarantee carries over. **Companion macro shipped:**
  `sized-defsystem-scheduled` (sized-scheduler wiring, direction 1
  -- see
  [`docs/archive/sized-scheduler-system-stage-world-carrier.md`](../archive/sized-scheduler-system-stage-world-carrier.md))
  lowers a monomorphic-world body to a typed impl plus an
  int-carrier wrapper plus a `System` value runnable on the
  parallel `Stage`. The wrapper passes the world as a heap-boxed
  `ptr<void>` reinterpreted as `:int`; per-world `box-<W>` /
  `load-<W>` / `free-<W>-box` helpers stay user-written (macros
  cannot splice identifiers into inline-C text -- see
  [`docs/archive/history/macro-cannot-emit-inline-c-block.md`](../archive/history/macro-cannot-emit-inline-c-block.md)).
  Direction 2 (cross-world / heterogeneous scheduling) remains
  queued behind gap-H world-type polymorphism, itself behind the
  Track A monomorphization phases. New regression:
  `tests/sized-stage.tur`.
- **Slice 9 -- mixed-shape sparse lookup**
  (`sized-world-sparse-has?` / `sized-world-sparse-get`), the
  sized counterpart of slice 6's tag pair. Hand-rolled-world
  caveat: `sized-defworld` emits dense fields only, so worlds
  mixing in `SizedSparse` / `SizedTag` spell out their
  `defstruct` by hand (the same constraint the unsized `defworld`
  has today).
- **Slice 10 -- monomorphic `sized-defworld-mono` +
  `sized-defcomponent-accessors-mono`.** The ergonomic-default
  for application code with a fixed budget: capacity baked in at
  declaration, no `[n]` ascription at call sites. The polymorphic
  forms stay for libraries shipping reusable world shapes.
- **Slice 11 -- `sized-defworld-copy-into` for slot-preserving
  resize.** Emits a per-world `copy-into-<Name>` function
  polymorphic in both source and destination capacity, threading
  the `gens` array so `Entity` handles packed against the source
  remain `(sized-alive? dst e)` after the copy. Growing resizes
  work directly; shrinking aborts before any partial state is
  observable. The `world-resize` existential
  wrapper shipped as `sized-defworld-world-resize` via
  turmeric-spices PR #17 (2026-06-19): a thin client layer over
  `copy-into-<W>` plus the `(exists [n'] ...)` packaging the
  archived [`ecs-sized-world-plan`](../archive/ecs-sized-world-plan.md)
  calls out, riding PR #420's existential pack/open heap-boxing fix.
- **Slice 12 -- fallible `sized-spawn`** returning
  `(Result int WorldFull)`, the typed counterpart of the
  panicking `sized-spawn!`. Q3's result-returning spawn; the
  slot-allocation path is shared verbatim with `sized-spawn!`
  so generational correctness is preserved. The carrier-bridge
  workaround (per-arm helper functions whose body is a bare tail
  `(ok ...)` / `(err ...)`) is documented in CHANGELOG and is the
  spice-side idiom for the if-branched-form carrier-bridge
  tail-position limitation until that bridges directly upstream.

### Still deferred (refinement types not shipping)

**E2b -- refinement-typed APIs.** The `entity-alive!` strict-aliveness
surface and `/has Pos` refinement on world bounds both require
refinement types, which are still in plan
([`docs/archive/refinement-types-plan.md`](refinement-types-plan.md)).
Both are pure surface additions over the shipped substrate; nothing
in the current spice has to change to accommodate them when they
do land.

## Validation -- what shipped

- **Spice-side regression tests** under
  `../turmeric-spices/spices/ecs/tests/` covering: world declaration
  + spawn/despawn + generational safety
  (`spawn1k.tur`, `spawn1k-pos.tur`, `spawn1k-wide.tur` for the
  eight-component variadic `defworld`); variadic queries up to
  arity 12 with intersection and tag filters
  (`for-each-arity-*.tur`, `defquery-integrate.tur`,
  `query-typed.tur`); system scheduling order and parallel
  non-conflict (`stage-pair.tur`, `stage-wave.tur`); Phase I cap
  surface (`cap-linear-single-use.tur`,
  `cap-mint-per-instance.tur`, `defsystem-caps-bound.tur`,
  `defcomponent-accessors.tur`); E2d storage-ops polymorphism
  (`storage-ops-poly.tur`); E2c sized-world surface
  (`sized-world-wide.tur`, `sized-world-reuse.tur`,
  `sized-world-generation.tur`, `sized-for-each.tur`,
  `sized-filter-with-without.tur`, `sized-sparse-lookup.tur`,
  `sized-defcomponent-accessors.tur`, `sized-defsystem.tur`,
  `sized-defworld-mono.tur`, `sized-world-copy-into.tur`,
  `sized-world-spawn-result.tur`); negative cases under
  `tests/errors/` rejecting double-use caps, wrong-component cap
  shapes, `:writes`-undeclared writes in both `defsystem` and
  `sized-defsystem`.
- **Main-repo regression fixture**
  `tests/fixtures/errors/ecs-defsystem-writes-unauthorized/` --
  declaring `:writes [Pos]` and trying to write `Vel` via the typed
  accessor fails to elaborate.
- **`tur-ecs-raylib` demo** runnable via `tur run` from
  `../turmeric-spices/spices/ecs-raylib/`.
- **Bench** -- ~~still TODO~~ **WRITTEN 2026-08-20, and the target is
  MISSED.** `turmeric-spices/spices/ecs/bench/` implements the
  100k-entity dense `Pos`/`Vel` integration vs. hand-rolled C
  (100 frames, float components, checksum-verified across every
  variant). Best of 9 on an M2, `tur` v0.37.0: hand-rolled C 4.4ms;
  raw Turmeric over flat buffers 4.6ms (**1.04x** -- the backend is
  not the problem); `defworld` + `for-each2` 36.9ms (**8.42x**);
  `sized-defworld` + `sized-for-each` 15.3ms (**3.50x**).
  Within-2x-of-hand-rolled was the target and neither ECS path
  reaches it.
  The cost is not codegen and not the query macro (a hand-written
  loop with no `for-each` ties `for-each2`, which does strictly more
  work per slot); it concentrates in the unsized `dense-set!` write
  path, which carries an auto-grow capacity branch, a `present[]`
  byte write, and a `len` update per store. Dictionary dispatch on
  the typeclass-polymorphic path is a non-issue: 20M lookups are
  unmeasurable (37.4ms vs 37.3ms hoisted -- `cc -O2` hoists it
  itself). Full write-up and the
  consequences for the two deferred plans (ECB declined, RE2
  reframed) in `spices/ecs/bench/README.md`.

## Resolved design decisions

These were the v1 open questions; all four are now committed. Precedent
comparison notes live in [docs/guides/ecs-vs-haskell-ecs.md](../guides/ecs-vs-haskell-ecs.md)
once that lands in E4.

### D1 -- Query rows use true variadic HKT

`Query in out` is parameterised by *type-level cons-lists of types*, not by
fixed-arity tuples. This requires extending the elaborator to support
type-level lists at kind level (a `Row :: List Type` kind) and a `Row`
HKT that queries are functorial over.

Precedent rejected: macro-generated tuple arities 1..N (Bevy/Specs) and
arrow composition with nested pairs (aztecs). Both work; both would have
shipped sooner. We are paying the implementation cost now because:

- Variadic rows generalise to records, relations, and the future
  data-frame work in `stats-formula-plan` -- not paying once means
  paying repeatedly.
- The whole pitch of the project is "more invariants at the type level
  than Haskell." Capping query arity at 5 via macro is exactly the
  retreat-to-runtime move we are trying to avoid.

**Implication: D1 is a hard prerequisite for the variadic E1, not for
shipping queries at all.** The v1 track ships an arity-capped
`queryN` macro family (E1') against today's HKT machinery; the
variadic single-`query` form lands as E1 once D1 does. The arity cap
is acknowledged as the retreat-to-runtime move we wanted to avoid --
we are taking it explicitly and time-boxed, with a documented
migration path, rather than indefinitely.

### D2 -- Per-component storage, small Storage trait

`Storage` typeclass exposes `insert`, `remove`, `get`, `iter`, plus an
associated type `Elem`. Three v1 backends: `:dense` (vec), `:sparse`
(HAMT), `:tag` (bitset). Component picks its storage at `defcomponent`.

Precedent rejected:
- Archetype storage (Flecs, modern Bevy). Faster iteration, but
  archetypes are themselves type-level sets of components -- so
  archetype storage and variadic HKT rows interact in nontrivial
  ways. We do not want both elaborator projects in the same release.
  Designing the `Storage` trait so an archetype backend *could* be
  swapped in later (E5+) without breaking the user-facing API is a
  soft goal; we will not contort v1 to guarantee it.
- Single sparse-set (EnTT). Removes the "storage choice is visible in
  the type" lever, which is a feature we want to keep.

`iter` for `:dense` returns a sized iterator (`Vec<n, (Entity, T)>`);
for `:sparse` and `:tag`, an unsized iterator. Sized witnesses
propagate through query joins where both sides are dense.

### D3 -- Tags are an explicit storage choice

`defcomponent Player :storage :tag` lowers to a bitset over entity
index space. A zero-field struct without `:storage :tag` still gets
dense storage -- no auto-detection, no silent perf cliffs from adding
a field. Tags participate in the same `Storage` trait as everything
else; `iter` on a tag yields `Entity` only (the associated `Elem` is
the unit-like singleton, optimised away).

Precedent rejected:
- EnTT-style auto-detection. Too magical; adding one field to a tag
  silently moves it from O(1) bit to O(n) words. We might add a lint
  in a later phase.
- Flecs-style `deftag` as a separate kind. Cleaner semantically but
  doubles the registry surface. We can revisit if real usage shows
  that tag-vs-component confusion is a footgun in practice.

### D4 -- Single-world v1

A `defsystem` reads/writes one world. Multiple worlds in one process
are values and may coexist; cross-world systems are not a thing in v1.

Precedent rejected, deferred to a post-v1 follow-up plan:
- World-Mirror primitive for render-extract / client-prediction
  patterns. Real use cases exist (Bevy SubApp, Unity DOTS) but none of
  them are blockers for the raylib demo. We will revisit when the
  first concrete user shows up.
- Full cross-world `:reads-from`/`:writes-to`. Significant elaborator
  work; not justified until D4-revisit happens.

## References

- aztecs: <https://hackage-content.haskell.org/package/aztecs-0.17.1/docs/Aztecs-ECS.html>
- apecs: <https://github.com/jonascarpay/apecs>
- `docs/guides/hkt-guide.md`
- `docs/guides/substructural-types-guide.md`
- `docs/guides/sized-types-guide.md`
- `../turmeric-spices/spices/raylib/`
