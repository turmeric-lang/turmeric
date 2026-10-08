---
title: ECS Storage Guide -- Dense, Sparse, Tag
category: Data Structures and Libraries
description: How `tur-ecs`'s three component-storage backends -- `Dense`, `Sparse`, `Tag` -- differ in layout, cost, and ergonomics; when to pick each; how `StorageOps` unifies them; and how the `Sized*` variants thread a capacity through the type.
---

# ECS Storage Guide -- Dense, Sparse, Tag

The `tur-ecs` spice ships three component-storage backends. They share
an API shape so a system written against one mostly ports to another,
but their runtime layouts and cost profiles are different enough that
the choice matters per component, not per world. This guide covers what
each backend is, when to reach for it, how they compose under the
`StorageOps` typeclass, and how the sized counterparts (`SizedDense`,
`SizedSparse`, `SizedTag`) thread a capacity `n` through the type.

For the broader ECS surface (worlds, queries, systems, the row-typed
`Query` value, the raylib loop), see
[`ecs-guide.md`](ecs-guide.md). For the long-form design rationale, see
[`docs/archive/ecs-spice-plan.md`](https://github.com/turmeric-lang/turmeric/blob/main/docs/archive/ecs-spice-plan.md).

## The three backends at a glance

| Backend | Module | Handle | Per-entity cost | Per-populated cost | Best for |
|---|---|---|---|---|---|
| Dense  | `ecs/storage` | `(Dense A)`  | `sizeof(A) + 1` byte | -- | Components most entities carry (`Pos`, `Vel`) |
| Sparse | `ecs/sparse`  | `(Sparse A)` | 0 | ~`sizeof(A) + 17` bytes | Components few entities carry (`Hp` on a world of props) |
| Tag    | `ecs/tag`     | `Tag`        | 1 bit | -- | Zero-payload markers (`Dead`, `Frozen`, `Player`) |

All three are `defopaque` over an `:int` carrier (a pointer to a C
control block). `Dense` and `Sparse` are phantom-typed over the element
type `A`, so `(Dense Pos)` and `(Dense Vel)` are distinct at the type
level even though both are `int64` at runtime. `Tag` has no element
type because tags carry no payload.

## Dense -- the default

```turmeric
(import ecs/storage :refer [dense-new dense-set! dense-get dense-has?
                             dense-len dense-free])

(let [pos : (Dense Pos) (dense-new)]
  (dense-set! pos 0 (pos2-make 1.0 2.0))
  (dense-set! pos 7 (pos2-make 3.0 4.0))
  (dense-len  pos)          ; => 8 (one past the largest written index)
  (dense-has? pos 0)        ; => true
  (dense-has? pos 1)        ; => false
  (let [p : Pos (dense-get pos 7)]
    ...)
  (dense-free pos))
```

Internally a `(Dense A)` is a parallel array indexed by the entity's
slot index, plus a `uint8_t present[]` bitmap:

```
data:    [ A | A | _ | _ | _ | _ | _ | A | ... ]
present: [ 1 | 1 | 0 | 0 | 0 | 0 | 0 | 1 | ... ]
```

* **Lookup** is one array index plus one bitmap read -- O(1), cache-friendly.
* **Iteration** is contiguous: walk `len` slots, skip those where
  `present[i] == 0`.
* **Cost** is one element-sized slot per entity *in the world's index
  range*, whether or not that entity carries the component. A world of
  100k entities where only 50 carry `Hp` still pays `100000 * sizeof(Hp)`
  bytes for a `(Dense Hp)`.

The element type `A` is reflected into the inline-C body via the
`__TUR_TY_A__` template marker, so per-instantiation monomorphization
sizes the data array correctly for both int-carried opaques and
by-value structs. When reading a struct component, annotate the binding
to drive the generic into a struct-specialized clone:

```turmeric
(let [p : Pos (dense-get (.Pos w) i)]   ; drives the (Dense Pos) instantiation
  ...)
```

(The annotation is often inferable from the handle's own `(Dense A)`
type, so you can usually omit it. Keep it when reading through an
`:int` you haven't yet typed.)

**Pick `Dense` when** most entities carry the component. This is the
v1 default; `defworld` field handles default to dense and the standard
`for-each` iteration walks the dense-storage union.

## Sparse -- few-of-many

```turmeric
(import ecs/sparse :refer [sparse-new sparse-set! sparse-get sparse-has?
                            sparse-del! sparse-len sparse-free])

(let [hp : (Sparse Hp) (sparse-new)]
  (sparse-set! hp 0    (hp-make 100))
  (sparse-set! hp 9999 (hp-make 5))
  (sparse-len  hp)         ; => 2  (populated count, not max index)
  (sparse-has? hp 0)       ; => true
  (sparse-has? hp 7)       ; => false
  (sparse-del! hp 0)       ; => true if the key was present
  (sparse-free hp))

```

`(Sparse A)` is an **open-addressed Robin Hood** hash table from
entity-index (`u32`) to a value of type `A`, with a parallel
`uint8_t probe_dist[]` array that doubles as occupancy and distance:

```
probe_dist[i] == 0  ->  slot i is empty
probe_dist[i] == d  ->  the entry at slot i sits at displacement (d - 1)
```

Insert does the Robin Hood swap: a newer entry with a higher probe
distance swaps past a resident with a lower one. Delete does
**backward-shift** guided by `probe_dist` -- no rehash math in the
shift loop. Lookup short-circuits when its own probe distance exceeds
the resident's, because the RH invariant guarantees the key cannot
appear later in the chain. The implementation follows
martinus/robin-hood-hashing v3.11.5.

Cost is ~`sizeof(A) + 17` bytes per **populated** entry, regardless of
the world's entity range. The previous example -- 100k entities, 50
with `Hp` -- pays ~`50 * (sizeof(Hp) + 17)` bytes for a `(Sparse Hp)`
instead of `100000 * sizeof(Hp)`.

**Pick `Sparse` when** populated count is much smaller than the world's
entity-index range. The crossover with `Dense` is roughly
`populated / range < sizeof(A) / (sizeof(A) + 17)`; in practice, if
"most entities don't have this", use `Sparse`.

### One thing `Sparse` has that `Dense` doesn't

`sparse-del!` removes an entry and returns `bool` (true if it was
present). `Dense` has no `dense-del!`: the bitmap-cleared model would
work, but it isn't part of v1 because dense storage's natural
"absence" model is "entity hasn't been spawned into that slot yet." If
you need per-entity removal, that component wants sparse storage.

## Tag -- zero-payload markers

```turmeric
(import ecs/tag :refer [tag-new tag-set! tag-clear! tag-has?
                         tag-count tag-cap tag-free])

(let [dead : Tag (tag-new)]
  (tag-set!   dead 7)
  (tag-set!   dead 42)
  (tag-has?   dead 7)        ; => true
  (tag-count  dead)          ; => 2  (popcount over the bitset)
  (tag-clear! dead 7)
  (tag-free   dead))
```

`Tag` is a plain bitset, one bit per entity slot. `Set`/`clear`/`test`
are O(1) on the index; `tag-count` is a popcount via
`__builtin_popcountll` over the bitset's words. There is no element
type because there is no payload.

**Pick `Tag` for** boolean markers: `Dead`, `Frozen`, `Player`,
`Selected`, `OnFire`. Anything where the answer is "is this entity in
this set?" and there's no data riding along.

Tags compose with dense/sparse storages through the `with` / `without`
filters in `ecs/query`: `with [Dead]` walks the `Dead` tag bitset;
`without [Dead]` skips entities whose `Dead` bit is set. Until the
sparse-primary iteration variant lands, tags participate in `for-each`
only as filters inside the body, not as primary iteration drivers.

## A mixed world

`defworld` accepts a mix of dense, sparse, and tag fields -- they all
hold an `:int` handle, and the same `(.Comp w)` syntax works regardless
of backend:

```turmeric
(defcomponent Pos)      ; dense (default)
(defcomponent Vel)      ; dense
(defcomponent Hp)       ; sparse: most entities are scenery, not combatants
(defcomponent Dead)     ; tag

(defworld Game [Pos Vel Hp Dead])

(let [w (make-struct Game
          (vec-new)
          (dense-new)   ; Pos
          (dense-new)   ; Vel
          (sparse-new)  ; Hp
          (tag-new))]   ; Dead
  ...)
```

The constructor positional arguments must match `defworld`'s component
order; pass `(dense-new)`, `(sparse-new)`, or `(tag-new)` per field as
appropriate. (`defcomponent :storage :sparse` is a reserved future
surface; today the backend is determined by which constructor you pass.)

## The `StorageOps` typeclass

`Dense` and `Sparse` share the same insert / get / has? shape, so
`ecs/storage-ops` lifts that common surface into a typeclass:

```turmeric
(defclass StorageOps [S]
  (type Elem : Type)
  (storage-insert! [s : S idx : int v : Elem] : nil)
  (storage-get     [s : S idx : int] : Elem)
  (storage-has?    [s : S idx : int] : bool))

(definstance StorageOps [(Dense A)]
  (type Elem = A)
  (storage-insert! [s idx v] (dense-set! s idx v))
  (storage-get     [s idx]   (dense-get  s idx))
  (storage-has?    [s idx]   (dense-has? s idx)))

(definstance StorageOps [(Sparse A)]
  (type Elem = A)
  (storage-insert! [s idx v] (sparse-set! s idx v))
  (storage-get     [s idx]   (sparse-get  s idx))
  (storage-has?    [s idx]   (sparse-has? s idx)))
```

A system written against `storage-insert!` / `storage-get` /
`storage-has?` runs against either backend; the instance is selected by
the storage handle's type. This is what lets the same system code run
against `(Dense Pos)` in one world and `(Sparse Pos)` in another --
the dispatch is static, no runtime branch.

`Tag` is intentionally *not* a `StorageOps` instance: it has no element
type and its operations don't share the insert/get/has? shape (there is
no value to insert or get). Tags compose through the query-filter
surface, not the storage-class surface.

`sparse-del!` is also not in the class -- the class is the **common**
surface across backends, not the union of every backend's ops. If your
system needs deletion, take a `(Sparse A)` directly and call
`sparse-del!`.

## Sized counterparts -- `SizedDense`, `SizedSparse`, `SizedTag`

For the bounded-capacity ("sized") world surface, each backend has a
sized counterpart that carries the capacity `n` as a phantom type-level
index:

```turmeric
(defopaque SizedDense  [n A] :int)
(defopaque SizedSparse [n A] :int)
(defopaque SizedTag    [n]   :int)
```

The same `n` unifies across a mixed-representation world, so a
`(GameWorld n)` declared with `sized-defworld` has one
`(SizedDense n Comp)` (or `SizedSparse`, `SizedTag`) per component plus
a state cell, and iteration is **statically rectangular** -- the
`sized-for-each` loop bound comes from the world's type, not a runtime
min-capacity probe.

The trade is that you commit to the capacity at construction:

```turmeric
(sized-defworld      GameWorld          [Pos Vel Hp Dead])  ; polymorphic n
(sized-defworld-mono GameWorld (Static 64) [Pos Vel Hp Dead])  ; baked-in n
```

`sized-spawn!` aborts on a full world; `sized-spawn` reports capacity
exhaustion as a typed `(err world-full)` -- the
`(Result int WorldFull)` shape so you can branch on it. `sized-despawn`
frees a slot and bumps its generation; `sized-alive?` answers the
use-after-despawn question (a stale handle whose generation no longer
matches the slot reads as dead).

To grow a sized world at runtime, `sized-defworld-world-resize` emits a
`world-resize-<World>` that allocates a fresh larger destination,
copies (handles preserved across the resize via
`sized-defworld-copy-into`), and packs the result inside the
size-hiding existential `(exists [n'] (<World> n'))`. Callers `open`
it to recover an abstract `n'` and continue with `sized-for-each` and
the cap-gated accessors.

Use sized worlds when:

* You know an upper bound at design time (a level with at most 64 NPCs,
  a UI scene with at most 256 widgets) and want the type system to
  enforce it.
* You want `sized-for-each` to skip the runtime min-capacity probe.
* You want spawn-failure to be a typed `Result` rather than an abort.

Use the unsized backends when capacity is genuinely unbounded or you
don't want to thread `n` through your system signatures.

## Decision quick reference

```
Component carries a payload?
+- No  -> Tag
+- Yes
   |
   How many entities carry it, relative to the world's index range?
   +- Most  -> Dense
   +- Few   -> Sparse
   |
   Need per-entity removal?
   +- Yes -> Sparse (only Sparse has -del!)
   +- No  -> backend-by-density still applies
   |
   Known capacity bound you want enforced at the type level?
   +- Yes -> SizedDense / SizedSparse / SizedTag
   +- No  -> Dense / Sparse / Tag
```

## Known limitations

* **`for-eachN` iterates dense storages only.** Listing a sparse or
  tag component on the primary loop would need a different primary
  (smallest-set iteration) or a dense-then-test fallback. Today, use
  sparse and tag components only via filters inside the body.
* **`defcomponent :storage :sparse` is reserved surface, not yet
  active.** Today the backend is chosen by which constructor you pass
  to `make-struct` for the world's field, not by a declaration on the
  component.
* **No `dense-del!`.** If a component needs per-entity removal,
  it wants sparse storage.

## See also

* [`ecs-guide.md`](ecs-guide.md) -- the broader ECS surface (worlds,
  queries, systems, the row-typed `Query` value, raylib loop).
* [`ecs-vs-haskell-ecs.md`](ecs-vs-haskell-ecs.md) -- comparison with a
  Haskell-style ECS.
* [`docs/archive/ecs-spice-plan.md`](https://github.com/turmeric-lang/turmeric/blob/main/docs/archive/ecs-spice-plan.md) --
  long-form plan and per-slice rationale.
* [`docs/archive/ecs-sized-world-plan.md`](https://github.com/turmeric-lang/turmeric/blob/main/docs/archive/ecs-sized-world-plan.md)
  -- the sized-world design plan.
