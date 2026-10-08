---
title: ECS Guide
category: Data Structures and Libraries
description: Building games and simulations with `tur-ecs` and the `tur-ecs-raylib` companion -- components, queries, systems, the row-typed Query value, and the standard 2D rendering loop.
---

# ECS Guide

`tur-ecs` is Turmeric's Entity-Component-System library; `tur-ecs-raylib`
is its standard 2D rendering companion. Both ship as spices in the
`turmeric-spices` repo. This guide walks through the surface that's
load-bearing in practice: declaring components, building worlds,
iterating with `for-each`, naming queries with `defquery`, and wiring a
raylib game loop with the standard `integrate-2d` / `render-circles`
systems.

For the long-form plan, prerequisites, and where each piece of the
surface came from, see
[`docs/archive/ecs-spice-plan.md`](https://github.com/turmeric-lang/turmeric/blob/main/docs/archive/ecs-spice-plan.md).

## TL;DR

```turmeric
(defmodule bouncing-balls (export)

(import ecs/entity   :refer [entity-index])
(import ecs/storage  :refer [dense-new dense-set! dense-get])
(import ecs/world    :refer [defcomponent defworld world-alloc-entity!])
(import ecs/query    :refer [for-each])
(import ecs-raylib/components :refer [Pos2 Vel2 Radius Color
                                       pos2-make vel2-make radius-make])
(import ecs-raylib/systems    :refer [integrate-2d])
(import ecs-raylib/render     :refer [render-circles])
(import ecs-raylib/loop       :refer [with-game-loop])
(import raylib/color          :refer [red])

(defcomponent Pos2)
(defcomponent Vel2)
(defcomponent Radius)
(defcomponent Color)
(defworld Scene [Pos2 Vel2 Radius Color])

(integrate-2d  integrate Scene)
(render-circles render   Scene)

(defn main [] : int
  (let [w (make-struct Scene (vec-new)
            (dense-new) (dense-new) (dense-new) (dense-new))]
    (let [e (world-alloc-entity! (.gens w))
          i (entity-index e)]
      (dense-set! (.Pos2   w) i (pos2-make 400.0 300.0))
      (dense-set! (.Vel2   w) i (vel2-make  120.0  80.0))
      (dense-set! (.Radius w) i (radius-make 30.0))
      (dense-set! (.Color  w) i (red)))
    (with-game-loop w "balls" 800 600 60
      (do (integrate w dt) (render w)))
    0))

) ;; end defmodule
```

Run with `tur run bouncing-balls.tur` from a project that has
`tur-ecs-raylib` on its `:spices` list.

## The model

| Concept | What it is | Where it lives |
|---|---|---|
| **Entity** | An opaque handle: low 32 bits = slot index, high 32 = generation. | `ecs/entity` |
| **Component** | A typed value stored per entity in a dense / sparse / tag storage. | `ecs/storage`, `ecs/sparse`, `ecs/tag` |
| **World** | A `defstruct` with one storage field per declared component plus a `gens` vec. | `ecs/world:defworld` |
| **Query** | Either a `for-each` iteration (imperative) or a row-typed `Query` value (typed). | `ecs/query` |
| **System** | A `defn` (often emitted by `defquery` or one of the standard system macros) that walks a query and writes results. | user code + `ecs-raylib/systems` |
| **Scheduler / Stage** | A sequenced or wave-parallelised invocation of systems. | `ecs/stage` |

## Components

A component is **any int-carried Turmeric value** registered with
`defcomponent`. The simplest cases are `defstruct`s used as field
types on the world; the spice ships standard components in
`ecs-raylib/components`:

```turmeric
(defopaque Pos2 :int)   ; packed (x << 32) | y, both fixed-point /1000
(defopaque Vel2 :int)   ; same layout
(defopaque Rot  :int)   ; angle * 1000
(defopaque Radius :int) ; radius * 1000
(defopaque Color  :int) ; raylib heap-allocated Color* cast to int
```

Helpers (`pos2-make`, `pos2-x`, `pos2-y`, `vel2-make`, ...) pack and
unpack the int carrier. The int-carrier choice matches `tur-ecs`'s
dense storage convention; a future colored revision could swap to
`defstruct` + `dense-get-w` (witness variant) without changing the
ECS spice itself.

User-defined components are equally simple:

```turmeric
(defcomponent Health)   ; marker: there is a Health component in the system
(defworld Game [Pos2 Vel2 Health])
```

`defcomponent` is a no-op marker today (E0 scope); the macro slot is
reserved so a future `defcomponent :storage :sparse` surface lands
without churn.

## Worlds

`defworld` lowers to a `defstruct` with one storage field per
component plus a `gens` vec for generational entity bookkeeping:

```turmeric
(defworld Game [Pos2 Vel2 Health])
;; lowers to:
(defstruct Game
  [gens   : int
   Pos2   : int   ; dense storage handle
   Vel2   : int
   Health : int])
```

Construction passes a `(vec-new)` for `gens` and a `(dense-new)` /
`(sparse-new)` / `(tag-new)` for each component slot, in declaration
order:

```turmeric
(let [w (make-struct Game (vec-new) (dense-new) (dense-new) (dense-new))]
  ...)
```

The current `defworld` cap is **5 components**. Beyond that, declare
the struct by hand -- everything else (entities, queries, systems)
keeps working since they only consume the `.Comp w` field-access
convention.

## Entities

Entities are 64-bit handles packing (generation, index):

```turmeric
(let [e (world-alloc-entity! (.gens w))]   ; allocate next slot
  (dense-set! (.Pos2 w) (entity-index e) (pos2-make 0.0 0.0)))

(world-despawn! (.gens w) e)   ; bumps the generation; dense data not cleared
```

Aliveness is a runtime check (`gens[index] == handle's generation`) --
but note **who runs it**. `sized-alive?` performs the comparison for
sized worlds; the unsized world exposes no aliveness predicate at all,
and the `defcomponent-accessors` reads do not consult `gens`. A handle
whose slot has been despawned reads the old bits. Compare generations
yourself when a handle may have outlived its entity.

A strict-aliveness API that makes use-after-despawn a *compile* error
**ships as an opt-in module** -- you opt in by importing it, not by
setting a flag; refinement checking itself is unconditional in every
build. The `ecs/refined-world` module's accessor
`rgworld-get-x!` refines its entity parameter with the impure `#reads`
measure `rgworld-alive?`, and the read compiles only where aliveness is
proven -- a guard inside a `frozen` region (which locks out
`^unique ^mut` despawn, `TUR-E0200`), or the `for-each-alive!` macro,
which generates the loop, the frozen borrow, and the guard so the body's
read discharges per-entity. Under `--strict-refine` an unproven read is
a hard error (`TUR-W0372`). The same surface ships for the REAL sized
stack via `ecs/sized-refined`: `(sized-defworld-refined GameWorld)`
emits `GameWorld-alive?` (`#reads`) + `GameWorld-despawn!`
(`^unique ^mut`), `(sized-defcomponent-accessor-refined GameWorld Pos)`
emits the cap-gated `get-Pos!` with the refined entity parameter, and
`(for-each-alive GameWorld w n e body)` iterates with per-entity proofs
-- all opt-in beside the unchanged forgiving family. See
[docs/upcoming/v1/ecs-refinement-typed-apis-plan.md](https://github.com/turmeric-lang/turmeric/blob/main/docs/upcoming/v1/ecs-refinement-typed-apis-plan.md).

## Queries: `for-each` (imperative)

The headline iteration surface is **truly variadic** -- no arity cap:

```turmeric
(for-each w [Pos2 Vel2] [e p v]
  (dense-set! (.Pos2 w) e
    (pos2-add! (:: p Pos2) (:: v Vel2) dt)))
```

The first vec lists component types; the second vec binds the entity
slot index followed by one value per component. The body is spliced
inline (no closure allocation) and runs for every slot where **all
listed components are present**. `dense-get` returns the int carrier;
ascribe with `(:: v Vel2)` if you want the typed wrapper.

Filters are stacked inside the body with `when` / `unless` and the
tag helpers:

```turmeric
(for-each w [Pos2 Vel2] [e p v]
  (when (world-tagged? w Player e)
    (unless (world-tagged? w Frozen e)
      ...)))
```

The explicit `for-each1`..`for-each3` ladder is kept as thin shims
over the variadic form for back-compat with E1' code.

## Queries: the row-typed `Query` value (typed)

For systems whose signature wants to name the precise component set
at the type level, the spice exposes a row-typed `Query` value
(using the `#row{...}` reader form):

```turmeric
(defstruct Query [^&in ^&out] (world :int))   ; from ecs/query

(defn integrate [q : (Query #row{Pos2 Vel2} #row{Pos2})] : nil
  (let [w (query-world q)]
    ...))
```

Row arguments are *phantom* -- the variadic-HKT-rows work erases them
at codegen, so a `Query` carries only the world handle at runtime,
but two `Query`s with different `(in, out)` rows are distinguished at
the type level. Strict element resolution catches typo'd component
names at the type level:
`(Query #row{Pos2 Velocityy} #row{Pos2})` errors because `Velocityy`
isn't declared.

## Systems with `defquery` + `run-query!`

`defquery` bundles a `for-each` iteration into a named `defn` over a
specific world type; `run-query!` is sugar for invoking it:

```turmeric
(defquery integrate w Game [Pos2 Vel2] [e p v]
  (dense-set! (.Pos2 w) e
    (pos2-add! (:: p Pos2) (:: v Vel2) dt)))

;; later:
(run-query! integrate game)   ; => (integrate game)
```

The world is borrowed (`^borrow`), so callers can reuse it after
running the system.

## Standard systems (raylib companion)

`ecs-raylib/systems` ships **`integrate-2d`** as a macro that emits
a typed `(defn ...)` for a concrete world type:

```turmeric
(integrate-2d integrate GameWorld)
;; emits: (defn integrate [^borrow w : GameWorld dt : float] : nil
;;          (for-each w [Pos2 Vel2] [e p v] ...))
```

`ecs-raylib/render` ships **`render-circles`** with the same shape
for the (Pos2, Radius, Color) drawing path. The two modules are
**split** so headless unit tests of integration math don't drag in
`<raylib.h>`; importing `ecs-raylib/render` is what pulls raylib in.

The pattern -- "macro that emits a typed system for the user's world
type" -- is the v1 "monomorphic systems against a concrete world"
path the plan calls out. Typeclass-bounded polymorphism over worlds
is a follow-up; `ecs-raylib/systems` stays usable today.

## The game loop

`ecs-raylib/loop:with-game-loop` wraps the raylib boilerplate around
a per-frame body:

```turmeric
(with-game-loop w "Demo" 800 600 60
  (do
    (integrate w dt)
    (render    w)))
```

The body sees `dt` (the frame's delta-time) in scope, and is free to
call ECS systems plus any raylib primitives. The expansion is:

```turmeric
(do
  (init-window 800 600 "Demo")
  (set-target-fps 60)
  (while (not (window-should-close))
    (let [dt (get-frame-time)]
      (begin-drawing)
      (clear-background (raywhite))
      <body>
      (end-drawing)))
  (close-window))
```

Override the clear color by drawing your own background rectangle at
the top of the body.

## The bouncing-balls demo

`spices/ecs-raylib/tests/demo-bouncing-balls.tur` is the canonical
worked example -- five circles bouncing in an 800x600 window. It
spawns the entities, defines a hand-written `bounce-walls` system
(velocity flip at the window edges), and threads
`(integrate w dt)` / `(bounce-walls w 800.0 600.0)` / `(render w)`
through `with-game-loop`. Run with
`tur run tests/demo-bouncing-balls.tur` from the spice root once
raylib is on the cmake-deps path.

## How ECS systems run today

- **Schedule**: explicit. The user writes the per-frame `(do ...)`
  block inside `with-game-loop` and decides the order. `ecs/stage`
  ships a `Stage` value that bundles a sequence of systems and a
  wave-parallel runner; consult `spices/ecs/src/ecs/stage.tur` for
  that surface.
- **Read/write capabilities**: the `defsystem` form in `ecs/system`
  collects `:reads`/`:writes` vectors and **enforces them at
  elaboration time** as substructural capabilities. A body that
  writes a component not listed in `:writes` fails to elaborate with
  `unbound symbol '<Comp>-write-cap'`. See
  [docs/guides/substructural-types-guide.md](substructural-types-guide.md)
  for the underlying cap machinery.
- **Aliveness**: runtime by default, via generation comparison --
  performed by `sized-alive?` on sized worlds, and **not** performed by
  the unsized `defcomponent-accessors` read path. Compile-time strict
  aliveness ships on the `ecs/refined-world` facade -- opting in means
  importing that module rather than the forgiving one; no flag is
  involved (see "Entities" above).

## Cross-world systems

Some workloads do not fit inside a single world. Render extraction wants
the simulation's components projected into a separate render world so
the renderer never sees fields it is not supposed to read. Client-side
prediction reconciles an authoritative server world against a locally
predicted one. Save/snapshot pipelines copy live state into a frozen
world before serialising. The `ecs/xsystem`, `ecs/xstage`, and
`ecs/xmirror` modules ship the v1 surface for these two-world shapes:
`defxsystem` declares a system that touches *two* worlds with paired
`:reads-from` / `:writes-to` clauses, `defmirror` is the one-line
shorthand for "copy one component verbatim between matching slots", and
`XStage` schedules them with a (world, component) conflict key so
non-conflicting cross-world systems coalesce into one parallel wave.

### `defxsystem` -- two-world systems

`defxsystem` takes two world bindings, then a sequence of
`:reads-from <w> [...]` / `:writes-to <w> [...]` clauses in any order.
A world that only reads (or only writes) simply omits the empty side;
every declared world must appear in at least one clause (a binding
listed but never read or written is rejected at compile time):

```turmeric
(defxsystem extract-renderables
  [sim SimWorld  ren RenderWorld]
  :reads-from sim [Pos]
  :writes-to  ren [RenderPos]
  (do
    (set-RenderPos! ren-RenderPos-write-cap ren 0
      (:: (:: (get-Pos sim-Pos-read-cap sim 0) :int) :RenderPos))
    (set-RenderPos! ren-RenderPos-write-cap ren 1
      (:: (:: (get-Pos sim-Pos-read-cap sim 1) :int) :RenderPos))))
```

The macro expands to three top-level definitions: an `extract-renderables-impl`
that takes the two typed worlds `^borrow`, an `extract-renderables-fn`
that unboxes two `:int` carriers via `load-SimWorld` / `load-RenderWorld`
and calls `-impl`, and an `extract-renderables` value of type `XSystem`
that bundles the four per-world read/write masks with `-fn`. The
`load-<WType>` helpers are user-written (same convention
`sized-defsystem-scheduled` uses), and a matching `box-<WType>` is what
the caller invokes to lift a world into the `:int` slot the scheduler
threads through.

Inside the body, every `:reads-from <w> [C ...]` puts a binding
`<w>-<C>-read-cap : (XReadCap <WType> C)` in scope, and every
`:writes-to <w> [C ...]` puts a `<w>-<C>-write-cap : (XWriteCap <WType> C)`
in scope. The cap types pin the world: a `set-RenderPos!` accessor minted
for `RenderWorld` rejects a `SimWorld`-keyed write-cap at elaboration
time, so the body cannot write into the world it only declared as
`:reads-from`. Write-caps are linear and the macro auto-consumes them at
body end, so user code never calls `use-cap!` by hand.

Each component `C` referenced still needs its world-local `C-cid`
binding in scope at the call site, exactly as single-world `defsystem`
requires. The same `C-cid` value can be reused across worlds because the
conflict key is `(world-id, cid)` -- the world identity is what
discriminates the lock target.

### Three or more worlds

`defxsystem` is not limited to two worlds. List as many world bindings
as the pipeline needs; the per-`(world, component)` lock model is
identical, only the bookkeeping arity grows. The motivating shape is a
snapshot -> predicted -> render pipeline: read the authoritative state,
write a predicted view and a render view in one pass.

```turmeric
(import ecs/xsystem :refer [defxsystem])
(import ecs/xstage  :refer [bind-xsystem-n xstage-add-n!
                            xstage-new xstage-run!])

(defxsystem project
  [auth AuthWorld  pred PredWorld  ren RenderWorld]
  :reads-from auth [Pos]
  :writes-to  pred [PredPos]
  :writes-to  ren  [RenderPos]
  (do
    (set-PredPos!   pred-PredPos-write-cap   pred 0
      (:: (:: (get-Pos auth-Pos-read-cap auth 0) :int) :PredPos))
    (set-RenderPos! ren-RenderPos-write-cap  ren  0
      (:: (:: (get-Pos auth-Pos-read-cap auth 0) :int) :RenderPos))))
```

A three-or-more-world system is bound with **`bind-xsystem-n`** -- one
`world-id handle` pair per slot, in declaration order -- and added with
**`xstage-add-n!`**:

```turmeric
(let [xs (xstage-new)
      bs (bind-xsystem-n project  0 auth-ptr  1 pred-ptr  2 render-ptr)]
  (xstage-add-n! xs bs)
  (xstage-run! xs))
```

The same conflict, wave, and cycle logic applies across every slot:
two `project` bindings that share the `pred` world both write
`(pred, PredPos)`, so they serialise into two waves; bound to disjoint
worlds they coalesce into one. The two-world surface (`bind-xsystem` /
`xstage-add!`) is unchanged and remains the right tool for exactly two
worlds; `bind-xsystem-n` / `xstage-add-n!` are the N-world
generalisation (`N >= 3`).

### `defmirror` -- cross-world copy shorthand

`defmirror` is the render-extract one-liner. It lowers to a single
`defxsystem` whose body loops `i` over `0..count-1`, copying each
declared source component into its target component through an `:int`
round-trip.

**Single component (asymmetric names):**

```turmeric
(defmirror mirror-pos
  [sim SimWorld  ren RenderWorld]
  :count 2
  :from  Pos
  :to    RenderPos)
```

**Multiple components in one pass (MULTI-MIR-V0):**

```turmeric
(defmirror extract-renderables
  [sim SimWorld  ren RenderWorld]
  :count 1024
  :components [[Pos RPos] [Sprite RSprite] [Color RColor]])
```

Each `:components` entry is a `[SrcC DstC]` pair (`:component <entry>`
is sugar for the one-element list). The generated system declares
`:reads-from sim [Pos Sprite Color]` and
`:writes-to ren [RPos RSprite RColor]` -- one read cap and one write
cap per component -- so the cap discipline still applies and the
scheduler sees the multi-component mirror as a single system holding
the full bundle of read/write targets. Users who want independent
scheduling per component should keep separate `defmirror`s.

Source and destination component names must be **distinct** because
`sized-defcomponent-accessors-xmono` mints `set-<C>!` / `get-<C>` at
the global function-name layer. Use distinct names per world (`Pos` on
`sim`, `RPos` on `ren`) so the accessors do not collide. For a
non-trivial projection or a body that does more than copy, write the
`defxsystem` directly.

### `XStage` -- parallel scheduling across worlds

A `BoundXSystem` is an `XSystem` resolved against concrete world
identities (`w0-id`, `w1-id`) and boxed world handles. `bind-xsystem`
threads both pairs in; a single-world system passes `w1-id = -1` and
`w1 = 0` to leave slot 1 empty:

```turmeric
(let [sp (box-SimWorld sim)
      rp (box-RenderWorld ren)
      xs (xstage-new)
      ws (bind-xsystem extract-renderables 0 sp 1 rp)]
  (xstage-add! xs ws)
  (xstage-run! xs)
  ...
  (xstage-free! xs))
```

`xstage-run!` recomputes a conflict-free wave partition on first run
(and after any `xstage-add!`), then runs waves sequentially with the
systems inside each wave running concurrently on pthreads. The
**conflict key is `(world-id, component-bit)`**, which is the v1
single-world rule lifted point-wise over the set of worlds a system
touches. Two systems conflict iff there is *some* world they both touch
where one's writes overlap the other's reads-or-writes. Worlds touched
by only one of them are independent lock targets and never conflict.

The practical payoff is that two systems writing the *same* component
in *different* worlds coalesce into a single wave, while two writing
the *same* `(world, component)` are forced into separate waves:

```turmeric
;; writeSim writes Pos in world 0, writeRen writes Pos in world 1.
(xstage-add! xs writeSim)
(xstage-add! xs writeRen)
(xstage-run! xs)
(xstage-n-waves xs)   ;; => 1  (different worlds, no conflict)

;; writeRen and writeRen2 both write Pos in world 1.
(xstage-add! ys writeRen)
(xstage-add! ys writeRen2)
(xstage-n-waves ys)   ;; => 2  (same world+component, forced apart)
```

`xstage-has-cycle?` is the static well-formedness check the cross-world
plan calls for. It builds the producer-to-consumer edge set
(`S -> T` when `T` reads a `(world, component)` `S` writes) and reports
whether the result contains a cycle. A cyclic cross-world stage cannot
be linearised by any wave order; split it with a sequencing barrier
(run the offending systems in separate, sequenced stages) instead. Run
the check once at startup before entering the simulation loop.

### Capability typing across worlds

`XReadCap` and `XWriteCap` (from `ecs/xcap`) are world-keyed analogues
of the single-world caps. The two-parameter type `(XWriteCap W C)` is
what lets the elaborator distinguish a `SimWorld` Pos-write from a
`RenderWorld` Pos-write even though both occupy the same component bit.
The single-world `defsystem` discipline still holds inside each world's
slot: `:writes-to` is linear and the cap is auto-consumed at body end,
so an undeclared write fails with the same
`unbound symbol '<W>-<C>-write-cap'` shape called out in
[substructural-types-guide](substructural-types-guide.md).

### Plumbing: world boxes and component-id bindings

A cross-world setup needs three pieces of glue per world type. A
single macro emits the trio:

```turmeric
(import ecs/xworld :refer [defworld-box-helpers])

(sized-defworld-mono SimWorld (Static 8) [Pos])
(defworld-box-helpers SimWorld)
;; emits:
;;   (defn box-SimWorld      [w  : SimWorld] : int      ...)
;;   (defn load-SimWorld     [wp : int]      : SimWorld ...)
;;   (defn free-SimWorld-box [wp : int]      : nil      ...)
```

Each helper is a thin wrapper over the polymorphic `box-world` /
`load-world` / `free-world-box` defined in `ecs/xworld`; the inline-C
bodies reflect the concrete world's C struct name via the
`__TUR_TY_W__` template marker, so no per-world inline-C block is
needed.

Plus a `(def <C>-cid <n>)` per component, per world. The numbered
constants are also macro-emitted:

```turmeric
(import ecs/world :refer [defcomponent-cids])

(defcomponent-cids [Pos])           ;; emits (def Pos-cid 0)
(defcomponent-cids [RenderPos])     ;; emits (def RenderPos-cid 0)
```

Cids are per-world, so two distinct worlds may legitimately assign `0`
to different components -- the scheduler keys conflict on `(world, cid)`
rather than `cid` alone. Numbering is stable per `:components` order;
reordering an existing list is a breaking change to any external
consumer of cid numbers. See `tests/defcomponent-cids.tur` and
`tests/xworld-defbox.tur` in the ecs spice for the canonical shape;
`tests/xworld-extract.tur` shows the legacy hand-rolled trio.

### Worked example: minimal extract pipeline

```turmeric
(sized-defworld-mono SimWorld    (Static 8) [Pos])
(sized-defcomponent-accessors-xmono SimWorld Pos)

(sized-defworld-mono RenderWorld (Static 8) [RenderPos])
(sized-defcomponent-accessors-xmono RenderWorld RenderPos)

(defcomponent-cids [Pos])
(defcomponent-cids [RenderPos])

;; box-/load-/free- helpers for each world type elided -- see plumbing above.

(defmirror mirror-pos
  [sim SimWorld  ren RenderWorld]
  :count 8
  :from  Pos
  :to    RenderPos)

(defn run-frame [sim : SimWorld ren : RenderWorld] : nil
  (let [sp (box-SimWorld sim)
        rp (box-RenderWorld ren)
        xs (xstage-new)]
    (xstage-add! xs (bind-xsystem mirror-pos 0 sp 1 rp))
    (when (xstage-has-cycle? xs)
      (panic "cross-world stage has a cycle -- split with a barrier"))
    (xstage-run! xs)
    (xstage-free! xs)
    (free-SimWorld-box   sp)
    (free-RenderWorld-box rp)))
```

In a real simulation loop the `XStage` is built once at startup, the
`xstage-has-cycle?` check runs once, and `xstage-run!` is called per
frame; only the world boxes flip per frame as the sim/render state
advances.

For the shipped plan and motivation, see
[docs/archive/history/ecs-cross-world-systems-plan.md](https://github.com/turmeric-lang/turmeric/blob/main/docs/archive/history/ecs-cross-world-systems-plan.md).

## Sized worlds -- compile-time rectangular iteration

The default `defworld` produces an *unsized* world: every storage
allocates its own capacity at construction time, and `for-each` walks
the intersection of the relevant storages by taking a runtime min over
their capacities (the `__fe-min-cap` probe). It works, but two queries
against the same world over differently-sized storages can't be
statically proven rectangular.

The **sized** form moves the capacity into the type. A
`(GameWorld (Static 1024))` declares once that every dense storage
inside is sized to 1024, and the SZ8 cross-parameter unifier proves
rectangularity across them without a runtime probe. Two worlds with
different capacities are distinct types; the elaborator rejects a
mixed-capacity `for-each` at compile time with TUR-E0260.

### Declaring a sized world

```turmeric
;; Monomorphic capacity baked in at the declaration site:
(defworld GameWorld (Static 1024) [Pos Vel Hp])

;; Polymorphic capacity -- a library shape callers pick a capacity for:
(defworld [n] GameWorld n [Pos Vel Hp])
```

The monomorphic form is the ergonomic default for application code
with a fixed entity budget. The polymorphic form is what reusable
library worlds use; callers ascribe a concrete `n` at construction.
The shipped macro is also available as `sized-defworld-mono` (the
explicit name); plain `defworld` with a `(Static k)` slot resolves to
the same thing.

Every component storage in a sized world shares the same `n`:

- `(SizedDense n A)` -- `n` slots, slot-indexed. O(n) memory.
- `(SizedSparse n A)` -- hashmap with key domain `[0, n)`. Memory
  is data-proportional; `n` is an **id-space bound**, not a
  storage-cost bound. (This is the one Sparse subtlety to remember.)
- `(SizedTag n)` -- n-bit bitset. O(n) bits.

### `sized-for-each` -- the load-bearing payoff

```turmeric
(sized-for-each [p (.pos w) v (.vel w)]
  (update-position! p v))
```

Lowers to a loop indexed `0..n` where the bound is the first
storage's type-level capacity, not a runtime min. Inside the loop body
the elaborator already knows `i in [0, n)` structurally, so the
generated accessor codegen elides bounds checks. A mixed-capacity
invocation fails at elaboration:

```turmeric
(sized-for-each [p (.pos w-256) v (.vel w-512)]
  ...)  ;; TUR-E0260: (Static 256) /= (Static 512)
```

`sized-world-tagged?` / `sized-world-untagged?` are the sized
analogues of `with` / `without` filters, composing with
`sized-for-each` via `when` / `unless` in the body.

### Spawn / despawn / generational handles

```turmeric
(defn sized-spawn  [n] [w : (GameWorld n)]         : (Result Entity WorldFull) ...)
(defn sized-spawn! [n] [w : (GameWorld n)]         : Entity ...)
(defn sized-despawn [n] [w : (GameWorld n) e : Entity] : nil ...)
```

`sized-spawn` is the typed-fallible form -- it returns
`(err world-full)` when the live-count would exceed `n`. Use it in
application paths where running out of slots is recoverable.
`sized-spawn!` panics on full and is meant for benchmark/demo paths
where the budget is known to fit.

Both return a packed generational `Entity`. Slot reuse is
invariant-preserving: despawn frees the id, generation bumps on next
reuse, and a stale handle's generation mismatch makes
`sized-alive?` return false.

### Cap-gated accessors and systems

`sized-defcomponent-accessors` and `sized-defsystem` mirror their
unsized counterparts and carry the same substructural cap
machinery -- `:writes [Pos]` binds a `Pos-write-cap` in body scope and
the elaborator only exposes `sized-set-Pos!` to that body. Negative
fixture: `tests/errors/sized-defsystem-undeclared-write.tur`.

A companion macro `sized-defsystem-scheduled` lowers a
monomorphic-world body to a `System` value runnable on the parallel
`Stage`, threading the world as a heap-boxed pointer through the
scheduler's int-carrier interface. Per-world `box-<W>` / `load-<W>`
helpers stay user-written (a macro cannot splice identifiers into
inline-C text).

### Resizing a sized world

A sized world cannot grow in place -- the capacity is in the type. To
grow, allocate a fresh world at the new capacity, copy slots through
`sized-defworld-copy-into` (generated per world; polymorphic in both
source and destination capacity), and the existential
`sized-defworld-world-resize` wrapper packages the
`(exists [n'] (GameWorld n'))` so callers see a single typed result:

```turmeric
(let [w' (world-resize-GameWorld w (Static 2048))]
  ...)  ;; w' : (exists [n] (GameWorld n))
```

Growing resizes work directly; shrinking aborts before any partial
state is observable.

### When to reach for sized vs unsized

| Use sized when... | Stick with unsized when... |
|---|---|
| The entity budget is known up front (level-fixed, demo-fixed). | The world's size genuinely varies at runtime in ways callers cannot predict. |
| Multiple `for-each` queries iterate the same storages and you want the runtime probe gone. | You only do one or two queries per frame and the probe cost is invisible. |
| You want compile-time rejection of cross-world iteration mistakes. | You're prototyping and the type ergonomics get in the way. |

The unsized form remains a fully-supported first-class shape; sized
is opt-in. Mixed-world projects where most worlds are sized and one
is bounded-but-mutable (chunk loaders, streaming open worlds) is the
intended sweet spot.

## Where to look next

- `docs/upcoming/v1/ecs-refinement-typed-apis-plan.md` -- the
  refinement-typed roadmap: strict aliveness (RE1, shipped as
  `ecs/refined-world` + `for-each-alive!`), bounded slot indices
  (RE2, gated on loop invariants and a profile).
- `docs/archive/ecs-component-set-bounds-plan.md` -- the structural
  "any world with `Pos` and `Vel`" bound, split out of the above.
- `../guides/hkt-guide.md` -- the variadic-HKT-rows mechanism behind
  the row-typed `Query` value.
- `../guides/substructural-types-guide.md` -- the substructural
  capability machinery that backs the shipped `:writes` enforcement
  and the `frozen` region's borrow semantics.
- `../../turmeric-spices/spices/ecs/README.md` -- the ECS spice
  release notes and known limitations.
- `../../turmeric-spices/spices/ecs-raylib/README.md` -- the raylib
  companion spice's setup and demo.
