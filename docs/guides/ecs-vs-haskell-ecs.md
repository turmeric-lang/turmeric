---
title: ECS -- tur-ecs vs apecs vs aztecs
category: Data Structures and Libraries
description: A side-by-side walk through a small game in apecs (Haskell), aztecs (Haskell), and tur-ecs (Turmeric), tabulating what each system catches at compile time vs. runtime, what each gives up to get there, and where the trade-offs actually land.
---

# tur-ecs vs apecs vs aztecs -- what moved to compile time

`tur-ecs`'s pitch is "take aztecs's structural clarity and apecs's
ergonomic queries, but lean on Turmeric's type system (HKTs, coherent
typeclasses, substructural types, sized types) to push more invariants
to compile time than Haskell's typeclass-soup approach can."

This guide tests the pitch against working code. It walks a small
bouncing-balls simulation in apecs, aztecs, and tur-ecs side by side,
then tabulates what each system rejects at compile time, what it
catches at runtime, and what it can't catch at all. The goal is to be
honest -- the rows where tur-ecs is unambiguously better are real, but
so are the rows where Haskell's ad-hoc polymorphism still buys you
something we don't have.

For the introductory tutorial see
[`ecs-guide.md`](ecs-guide.md). For the long-form plan and its
load-bearing prereqs see
[`../archive/ecs-spice-plan.md`](https://github.com/turmeric-lang/turmeric/blob/main/docs/archive/ecs-spice-plan.md).

## The bottom line

| Property | apecs | aztecs | tur-ecs |
|---|---|---|---|
| **Component membership** ("does this world have `Pos`?") | Runtime hashmap lookup keyed by `TypeRep` | Runtime hashmap lookup keyed by `TypeRep` | Struct-field access; *unbound name* at compile time if missing |
| **Write-set enforcement** (system declares it only writes `Pos`, body actually writes `Vel`) | Trust the programmer | Trust the programmer | **Compile-time error** (cap-gated `set-<Comp>!`) |
| **Read-set enforcement** | Trust the programmer | Trust the programmer | Same -- read caps in scope iff comp is in `:reads` |
| **Storage choice** (dense vs sparse vs tag) | Type-family `Storage c` -- resolved at compile time, but the user can lie via orphan instances | Associated type, same caveat | Per-component; macro registers, accessor type bakes it in. No orphan-instance surface |
| **Typeclass coherence** | Open instances; orphans are a maintenance hazard | Open instances | Coherent -- one `Component T` instance per `T`, enforced by the elaborator |
| **Polymorphic-system bound** ("any world with `Pos` and `Vel`") | `Has w Pos, Has w Vel => ...` constraint, solved by GHC | Same | `(HasPos W) (HasVel W) => ...` Turmeric class constraint -- shipped via `defcomponent-class` / `definstance` |
| **Entity aliveness** | `Maybe`-returning reads | `Maybe`-returning reads | Generational handles, checked where you ask: `sized-alive?` on sized worlds; the unsized `defcomponent-accessors` reads return `T` **unchecked** (a stale handle reads stale bits). Opt-in **compile-time** strict aliveness by importing the `ecs/refined-world` facade: a read whose entity is not proven alive is a compile error |
| **Query arity** | Tuples up to 8-ish via type-class hackery; degrades past that | `Query` arrow combinators -- no cap, but composition cost is real | Truly variadic via row-kinded `for-each`; row type is the kind-`[*]` of components |
| **Dense-storage length matching** | Runtime check on zip | Runtime check on zip | Runtime check on the unsized world; compile-time (statically rectangular, `TUR-E0260` on mismatch) with the opt-in sized worlds (`sized-defworld` / `sized-for-each`) |
| **Cross-world systems** | Out of scope | Out of scope | Shipped -- `ecs/xsystem` / `defmirror` / `XStage` (see [`ecs-guide.md`](ecs-guide.md#cross-world-systems)) |

The single largest delta is **write-set enforcement**. Both Haskell
libraries trust the programmer not to write to a component they didn't
declare; tur-ecs gates writes through per-component `WriteCap<T>`
linear capabilities that are only in scope when the comp is in
`:writes`. The plan calls this "the single biggest delta vs. Haskell
ECSes"; the rest of this guide shows it in real code.

## The example: bouncing balls

A canvas of `N` balls with position, velocity, and color. Each frame:
integrate position, bounce off the walls, render. Spawn 1000 of them.
Everyone uses the same surface, so the only thing that varies is the
ECS framework.

### apecs

```haskell
{-# LANGUAGE TemplateHaskell, ScopedTypeVariables, TypeFamilies, FlexibleContexts #-}
module BouncingBalls where

import Apecs
import Apecs.Stores (Map)
import Linear

newtype Pos   = Pos   (V2 Float) deriving (Show)
newtype Vel   = Vel   (V2 Float) deriving (Show)
newtype Color = Color (V4 Float) deriving (Show)

instance Component Pos   where type Storage Pos   = Map Pos
instance Component Vel   where type Storage Vel   = Map Vel
instance Component Color where type Storage Color = Map Color

makeWorld "World" [''Pos, ''Vel, ''Color]

integrate :: Float -> System World ()
integrate dt = cmap $ \(Pos p, Vel v) -> Pos (p + dt *^ v)

bounce :: System World ()
bounce = cmap $ \(Pos (V2 x y), Vel (V2 vx vy)) ->
  let vx' = if x < 0 || x > 800 then -vx else vx
      vy' = if y < 0 || y > 600 then -vy else vy
  in  Vel (V2 vx' vy')

frame :: Float -> System World ()
frame dt = integrate dt >> bounce
```

The `cmap` is the workhorse: it picks up the row by matching
constructors in the lambda. `integrate` says "I touch `Pos` and `Vel`"
*by writing the lambda's pattern*. Nothing prevents the body from also
touching `Color`; the type system doesn't know.

The `Has` constraints (`Has World IO Pos`, `Has World IO Vel`) appear
in inferred types but rarely in user code; GHC threads them. The
`Storage Pos = Map Pos` line is the actual compile-time choice -- if
you write `Map` twice you have two stores for `Pos`, which is an error
GHC will catch via overlap rejection (or fail to catch if you let it
in via orphan instances and `-fno-warn-orphans`).

### aztecs

```haskell
{-# LANGUAGE TypeApplications #-}
module BouncingBalls where

import Aztecs.ECS as ECS
import Linear

newtype Pos   = Pos   (V2 Float)
newtype Vel   = Vel   (V2 Float)
newtype Color = Color (V4 Float)

integrate :: Float -> System ()
integrate dt = ECS.map_ $ proc () -> do
  Pos p <- ECS.read @Pos -< ()
  Vel v <- ECS.read @Vel -< ()
  ECS.write @Pos -< Pos (p + dt *^ v)

bounce :: System ()
bounce = ECS.map_ $ proc () -> do
  Pos (V2 x y)   <- ECS.read @Pos -< ()
  Vel (V2 vx vy) <- ECS.read @Vel -< ()
  let vx' = if x < 0 || x > 800 then -vx else vx
      vy' = if y < 0 || y > 600 then -vy else vy
  ECS.write @Vel -< Vel (V2 vx' vy')
```

aztecs makes the read/write explicit via arrow notation. That's an
improvement over `cmap`: you can read the system's declared effects
off the arrow body. But the *enforcement* is the same as apecs --
nothing makes `integrate` actually use only `Pos` and `Vel`. A line
that says `ECS.write @Color -< ...` would type-check fine.

The query value here is `proc ... do`, which composes via arrow
combinators. Past three or four reads/writes this gets hard to read,
which is the cost aztecs pays for its compile-time row shape.

### tur-ecs

```turmeric
(defmodule bouncing-balls (export)

(import ecs/system  :refer [System make-system system-run! defsystem])
(import ecs/storage :refer [dense-new dense-set!])
(import ecs/world   :refer [defworld defcomponent-accessors world-alloc-entity!])
(import ecs/cap     :refer [WriteCap ReadCap make-write-cap make-read-cap use-cap!])

(defstruct Pos   [x : float y : float])
(defstruct Vel   [x : float y : float])
(defstruct Color [r : float g : float b : float a : float])

(defworld World [Pos Vel Color])
(defcomponent-accessors World Pos)
(defcomponent-accessors World Vel)
(defcomponent-accessors World Color)

(def Pos-cid   0)
(def Vel-cid   1)
(def Color-cid 2)

;; integrate declares :reads [Pos Vel], :writes [Pos].  The body has
;; Pos-write-cap in scope but no Color-write-cap; writing Color is a
;; compile-time error.
(defsystem integrate
  [Pos Vel]
  [Pos]
  (let [w' : World (:: w World)]
    ;; ... read Pos, read Vel, set-Pos! Pos-write-cap w' e new-pos ...
    nil))

(defsystem bounce
  [Pos]
  [Vel]
  (let [w' : World (:: w World)]
    ;; ... read Pos, set-Vel! Vel-write-cap w' e new-vel ...
    nil))
)
```

The line you cannot write here:

```turmeric
(defsystem integrate
  [Pos Vel]                          ;; :reads
  [Pos]                              ;; :writes -- only Pos!
  (let [w' : World (:: w World)]
    (set-Color! Color-write-cap w' 0 (make-struct Color 1.0 0.0 0.0 1.0))))
;;             ^^^^^^^^^^^^^^^
;; error [TUR-E0003]: unbound symbol 'Color-write-cap'
```

`Color` is not in `:writes`, so the `defsystem` macro never binds
`Color-write-cap` in body scope, so the `set-Color!` call cannot
resolve its required cap argument. The body refuses to elaborate.
This is the compile-time enforcement neither apecs nor aztecs
deliver.

The corresponding *positive* case is just as direct:

```turmeric
(defsystem integrate
  [Pos Vel]
  [Pos]
  (let [w' : World (:: w World)
        rp (get-Pos Pos-read-cap w' 0)
        rv (get-Vel Vel-read-cap w' 0)]
    (set-Pos! Pos-write-cap w' 0 (make-struct Pos
                                              (+ (.x rp) (.x rv))
                                              (+ (.y rp) (.y rv))))))
```

`Pos-read-cap`, `Vel-read-cap`, and `Pos-write-cap` are bound by the
`defsystem` macro for each entry in the `:reads` / `:writes` vectors.
The cap names are predictable; the user does not mint them by hand.

## What moved to compile time -- the four wins

### 1. Component membership

In apecs and aztecs, `World` is a heterogeneous container keyed by
`TypeRep`. Asking "does this world have `Pos`?" is a runtime hash
lookup. If you query a component you didn't register, the lookup
returns `Nothing` and you find out at runtime.

In tur-ecs, `World` is a `defstruct` with one storage field per
declared component. Querying a component the world doesn't declare is
an *unbound-name* error from the elaborator:

```turmeric
(defworld Game [Pos Vel])

(defsystem render
  [Sprite]                                  ;; Sprite is not on Game
  []
  ...)
;; error: defworld Game has no field 'Sprite'
```

This is the structural-membership lever. We can claim it not because
of any clever type-system feature but because we made the world a
named, closed type instead of a `TypeRep`-keyed map.

### 2. Write-set enforcement

The headline. `defsystem`'s `:writes [Pos]` binds a linear
`Pos-write-cap : (WriteCap Pos)` in body scope. The accessors emitted
by `defcomponent-accessors` require the cap as their first argument:

```turmeric
(defn set-Pos! [^borrow cap : (WriteCap Pos)
                ^borrow w   : World
                e           : int
                v           : Pos]
              : nil
  (dense-set! (.Pos w) e v))
```

A body that did not declare `:writes [Vel]` has no `Vel-write-cap`;
the `set-Vel!` call name-resolves, but its first argument is unbound.

The Phase I report
([`docs/archive/history/ecs-defsystem-write-caps-not-enforced.md`](https://github.com/turmeric-lang/turmeric/blob/main/docs/archive/history/ecs-defsystem-write-caps-not-enforced.md))
walks the implementation. The load-bearing prereq was the parametric
`:linear` propagation fix
([`docs/archive/history/parametric-linear-opaque-not-enforced.md`](https://github.com/turmeric-lang/turmeric/blob/main/docs/archive/history/parametric-linear-opaque-not-enforced.md));
without it, `WriteCap<T>` would compile-check fine but its
single-use discipline would silently drop on every application.

apecs and aztecs cannot offer this. Haskell's substructural-types
story (`linear-base`, `LinearTypes`) exists but is not what either
library uses; both inherit Haskell's "the system says it writes `Pos`
but the body can do whatever" trust model.

### 3. Storage choice baked into the accessor

apecs's `type Storage Pos = Map Pos` and aztecs's analog are
compile-time choices. If a downstream consumer wants `Pos` in a sparse
set, they cannot override the choice without an orphan instance --
which Haskell allows but discourages.

tur-ecs's `defcomponent` registers a storage choice per (world,
component) pair. There is no orphan-instance surface because there are
no orphan instances of `Component T` -- the elaborator rejects them.
A world that uses `Pos` in `:dense` and another that uses `Pos` in
`:sparse` are different worlds; both compile.

### 4. Coherent typeclass dispatch

`Component T` has exactly one instance per `T`. The elaborator
enforces this. apecs and aztecs both ride on Haskell's open-world
typeclass model, which is *fine* in practice but leaves a known
maintenance hazard: an orphan instance imported from a library can
silently change behaviour, and `-fno-warn-orphans` is depressingly
common.

The cost: less ad-hoc polymorphism. apecs lets you swap storages by
swapping instances at import time; tur-ecs makes that an explicit
choice at the world's declaration site.

## What's still runtime-checked

Being honest matters. The "compile-time everything" pitch is wrong;
here's where tur-ecs still trusts runtime checks.

### Entity aliveness

```turmeric
(defopaque Entity :int)   ;; low 32 = slot index, high 32 = generation
```

A dead-entity handle's generation mismatches the slot's current
generation, and `sized-alive?` is the runtime u32 compare that says so.
apecs and aztecs do the same thing the same way -- neither ships
compile-time alive-set proofs, and tur-ecs's *default* path doesn't
either.

Two honesty notes. The reads are **not** `option`-returning:
`defcomponent-accessors` emits a `get-<Comp>` that returns the
component directly. And on the unsized world the comparison is not
performed anywhere on the read path -- a stale handle reads stale
bits. Sized worlds have `sized-alive?`; unsized worlds leave the
check to you.

A compile-time version now **ships as an opt-in module** -- you opt in
by importing it, not by setting a flag. Refinement types are checked
statically on every compile
([`refinement-types-guide.md`](refinement-types-guide.md)), and the
impure-measure question -- `alive?` reads mutable world state through
inline C, which is exactly what congruence must refuse in general -- was
answered by `#reads` + `frozen` regions
([`docs/archive/refine-stateful-measures-plan.md`](https://github.com/turmeric-lang/turmeric/blob/main/docs/archive/refine-stateful-measures-plan.md)):
a `#reads w` measure is congruent while `w` is immutably borrowed, and
the borrow makes `^unique ^mut` despawn a compile error inside the
region, which is what makes trusting the guard sound. The
`ecs/refined-world` module puts that to work: `rgworld-get-x!` refines
its entity with `(rgworld-alive? w x)`, a guarded read in a `frozen`
region proves, and the `for-each-alive!` macro generates loop + borrow +
guard so a per-entity body read discharges. Under `--strict-refine` an
unproven read is a hard error. The same family ships for the real sized
stack via `ecs/sized-refined` (`sized-defworld-refined` /
`sized-defcomponent-accessor-refined` / `for-each-alive`), emitted per
world/component beside the unchanged forgiving accessors. Honest scope
notes: the guarantee is against ordinary code, not a deliberate
`::`-cast / inline-C bypass (the same trust boundary `#reads` itself
carries); and the forgiving default API stays -- that remains the sound
choice when
the prover can't.

### Dense-storage length matching

On the unsized world, `for-each [Pos Vel]` zips two dense storages
with a runtime check that both storages cover the loop range (the
`__fe-min-cap` probe). The opt-in sized worlds move this to compile
time: `sized-for-each` over a `(GameWorld (Static n))` is statically
rectangular, and a mixed-capacity invocation is a `TUR-E0260`
elaboration error (see [`ecs-storage-guide.md`](ecs-storage-guide.md)
and the sized-world section of [`ecs-guide.md`](ecs-guide.md)). The
runtime check remains only on the unsized path.

### The world handle is `:int` at the system body's signature

`(defsystem foo [Pos Vel] [Pos] body)` lowers to `(defn foo-impl [w :
int] : nil ...)`. The body receives `w` as an int and recovers the
typed world with `(:: w World)`. This was the cheapest way to ship a
working scheduler without rewriting the system trampoline contract.
The downstream effect is that the world *type* is recovered inside
the body, not enforced at the system's external signature. Nothing
unsound happens -- the cap surface still gates writes -- but the
narrowing happens one layer in.

A future revision will likely type the world parameter directly; it
isn't load-bearing for the v1 wins above.

## What we gave up vs. apecs

| Loss | What it costs in practice |
|---|---|
| **Ad-hoc polymorphism over component sets.** `cmap` lets a system body decide *at the lambda* which tuple of components it touches. tur-ecs makes you declare `:reads` / `:writes` up front. | Declaration ceremony per system. Pays off the moment your codebase has more than one system writing the same component. |
| **Orphan-instance flexibility.** A downstream consumer cannot swap `Storage Pos = Map` for `Storage Pos = Cache (Map Pos)` from outside the component-declaration site. | Largely a non-cost in practice; orphan instances are a maintenance hazard apecs codebases regret. |
| **Open-world typeclass extension.** A new library can register itself as a `Component` for an existing type without source access to the original module. | We force the registration into the world declaration. For a library author this means the world author has to opt in -- which is what you'd want anyway for cap-gated writes. |
| **GHC's constraint inference.** `(Has w Pos, Has w Vel) => ...` is solved silently; the user often doesn't see it. tur-ecs makes typeclass-bounded systems explicit via `(HasPos W) (HasVel W)`. | Slightly more verbose at the declaration site. Identical runtime cost (one dictionary lookup per polymorphic call). |

The recurring theme: tur-ecs trades flexibility-at-the-import-site for
locality-of-reasoning-at-the-declaration-site. In a small project this
feels like ceremony; in a large project this is the whole point.

## What we gave up vs. aztecs

aztecs's arrow-based queries are genuinely more *compositional* than
tur-ecs's `for-each`. A `Query a b` arrow can be passed around,
combined with `>>>` and `***`, and run against multiple worlds. tur-ecs's
`for-each` is a macro that splices code inline. The `defquery` /
`run-query!` surface is the closest analog and gets you the
named-query reuse, but not the arrow-algebra composition.

This is a real loss for code that wants to express queries as
*data*. For code that wants to express them as *loops*, tur-ecs is
direct and aztecs is indirect -- the trade-off cuts both ways.

## Scheduling: where the cap surface earns its keep

```turmeric
(defsystem physics [Pos Vel] [Pos]   body)
(defsystem render  [Pos Sprite] []   body)
```

These two systems' write sets are `[Pos]` and `[]`. Their read sets
share `Pos`. By the v1 rule "no write/read overlap on any component
bit," `render` cannot run in parallel with `physics`. The scheduler
proves this *statically* from the declared sets.

```turmeric
(defsystem physics [Pos Vel] [Pos]   body)
(defsystem ai      [Brain]   [Brain] body)
```

These don't share any component. Disjoint, parallelisable, scheduler
runs them concurrently. The proof is type-level set disjointness on
the declared `:reads` / `:writes` vectors, computed at macro-expansion
time.

apecs and aztecs both ship parallel schedulers; both rely on the
programmer to declare disjoint sets accurately. tur-ecs rejects
inaccurate declarations at the body's elaboration step, *before* the
scheduler ever runs.

## Where to look next

- [`ecs-guide.md`](ecs-guide.md) -- the introductory tutorial.
- [`../archive/ecs-spice-plan.md`](https://github.com/turmeric-lang/turmeric/blob/main/docs/archive/ecs-spice-plan.md)
  -- the long-form plan, status, and what's still queued for v2.
- [`../archive/history/ecs-defsystem-write-caps-not-enforced.md`](https://github.com/turmeric-lang/turmeric/blob/main/docs/archive/history/ecs-defsystem-write-caps-not-enforced.md)
  -- the Phase I implementation log for the cap-gating surface that
  delivered the headline compile-time-write-set claim.
- [`../archive/history/ecs-cross-world-systems-plan.md`](https://github.com/turmeric-lang/turmeric/blob/main/docs/archive/history/ecs-cross-world-systems-plan.md)
  -- the shipped extension of the cap surface to multi-world
  render-extract / client-prediction patterns (`ecs/xsystem`,
  `defmirror`, `XStage`).
- [`substructural-types-guide.md`](substructural-types-guide.md) --
  the `^linear` / `^borrow` machinery the cap surface is built on.

## Honest scorecard

If you take only one thing from this guide, take this:

- **Component membership and write-set enforcement** are the rows
  where tur-ecs is unambiguously better than apecs and aztecs in v1.
  The cap-gating has shipped; this is not a future claim.
- **Aliveness** is the row where the *defaults* are the same and
  runtime-checked everywhere (Haskell's `liquid-haskell` is the analog;
  nobody ships it as default). tur-ecs now has an opt-in compile-time
  surface here -- the `ecs/refined-world` facade, which you opt into by
  importing it instead of the forgiving module, and where an
  unproven read fails to compile -- which neither Haskell library
  offers. On the default path tur-ecs is still arguably a step
  *behind*: the unsized read skips the generation compare entirely,
  where apecs and aztecs return a `Maybe`.
- **Query composition** is the row where aztecs is genuinely ahead of
  tur-ecs for code that treats queries as first-class data.
- **Convenience for tiny one-off games** is the row where apecs's
  `cmap` is still the lowest-ceremony surface; tur-ecs's declaration
  vector is a real cost for code that doesn't need the cap surface.

The pitch holds. The honest scorecard says we won the rows we claimed
to and didn't win the ones we didn't.
