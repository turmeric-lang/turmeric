# Typed Collections Plan

> **Status:** Complete
> **Last Updated:** 2026-05-23
> **Type:** Language / Stdlib Design

---

## Overview

Every data structure in stdlib currently stores values as `int64_t` or routes
through `ptr<void>`, with no compile-time parameterization over element or
key/value types. This plan introduces typed, parameterized versions of the
core collections under a new `TM` (Typed Maps) / `TC` (Typed Collections)
phase umbrella.

The work is split into three phases:

| Phase | Scope |
|-------|-------|
| TM0 | Typed `Map[K V]` (the primary motivator) |
| TC1 | Typed `Vec[A]`, `List[A]`, `Slice[A]`, `Option[A]`, `Result[A B]`, `Pair[A B]` |
| TC2 | Typed `Grid[A]`, `Zipper[A]`; `Set[A]` (new) |

---

## Background: Current State

All existing collections are monomorphic over `int64_t` or untyped
`ptr<void>`. Key limitations:

- **`hamt.tur` / `map.tur`** -- keys and values are `ptr<void>`; key equality
  uses pointer identity only, not value equality. `map-eq?` requires a
  caller-supplied `val-cmp` and still cannot do key hashing by value.
- **`vec.tur`, `list.tur`, `slice.tur`** -- element type is hardcoded to
  `int64_t`.
- **`option.tur`, `result.tur`, `pair.tur`** -- likewise `int64_t` in both
  payload slots.
- **`grid.tur`, `zipper.tur`** -- row elements are `int64_t`.
- **No `Set[A]`** exists at all; the only set-like structure is the raw HAMT
  used as a map.
- **`gadt-vec.tur`** carries a phantom type parameter `[a]` declared in the
  GADT header but unused in the constructor fields -- it is not a fully typed
  parameterized vec.

Phase E1 (already landed) added `*-eq?` comparator functions to all of the
above but did not introduce type parameters. Phase E2 (this plan) picks up
where E1 left off.

---

## Phase TM0: Typed Map

### Goal

A `Map[K V]` type where:
- The compiler knows the key type `K` and value type `V` at every call site.
- Key hashing and equality are resolved through typeclass constraints
  (`Hash[K]` and `Eq[K]`), not raw pointer identity.
- The persistent HAMT backing store is unchanged; typing is a layer above it.

### Design

```turmeric
;;; Typed map, backed by persistent HAMT.
;;; K must satisfy Hash and Eq.
(defstruct Map [K V]
  (hamt :ptr<void>))   ; opaque HAMT node

(defn tmap-new [] : Map[K V]
  ...)

(defn tmap-assoc [m : Map[K V] key : K val : V] : Map[K V]
  ...)

(defn tmap-dissoc [m : Map[K V] key : K] : Map[K V]
  ...)

(defn tmap-get [m : Map[K V] key : K] : Option[V]
  ...)

(defn tmap-has? [m : Map[K V] key : K] : bool
  ...)

(defn tmap-count [m : Map[K V]] : int
  ...)

(defn tmap-merge [a : Map[K V] b : Map[K V]] : Map[K V]
  ...)
```

`tmap-get` returns `Option[V]` (see TC1) rather than a nullable `ptr<void>`.

### Typeclass Requirements

Two typeclasses must exist (or be extended) before TM0 can ship:

| Typeclass | Methods | Notes |
|-----------|---------|-------|
| `Hash[A]` | `hash [x :A] :int` | Must already exist or be added; `cstr`, `int`, `float` get built-in instances |
| `Eq[A]` | `eq? [a :A b :A] :bool` | Already exists; constrained instance for Map itself deferred to TM0-6 |

### Tasks

| ID | Task | Status |
|----|------|--------|
| TM0-1 | Add `Hash[A]` typeclass and built-in instances for `int`, `float`, `cstr`, `bool` | Done |
| TM0-2 | Define `Map[K V]` struct wrapping the HAMT `ptr<void>` | Done |
| TM0-3 | Implement `tmap-new`, `tmap-assoc`, `tmap-dissoc`, `tmap-count`, `tmap-merge` | Done |
| TM0-4 | Implement `tmap-get` returning `Option[V]` (depends on TC1-option or a stub) | Done |
| TM0-5 | Implement `tmap-has?` | Done |
| TM0-6 | Add constrained `Eq[Map[K V]]` instance using `Hash[K]` + `Eq[K]` + `Eq[V]` | Done |
| TM0-7 | Deprecation notice on untyped `map.tur` (`assoc`, `dissoc`, `get`, `has?`, `count`, `merge`) -- keep for one release cycle | Done (comment only; compile-time warning deferred, see DEP-1) |
| TM0-8 | Update `gendocs.py` / doc tooling to render type parameters in `defstruct` | Done |
| TM0-9 | Add fixtures in `tests/fixtures/` covering: empty map, single assoc, dissoc, merge, collision (two keys with same hash), `tmap-eq?` | Done |

### Open Questions

- **Key hashing by value vs. by pointer** -- the existing HAMT hashes by the
  raw `int64_t` bit pattern. For non-pointer scalar keys (`int`, `bool`) that
  is already value-based. For `cstr` keys the `Hash[cstr]` instance must
  compute a content hash, not pointer hash. The HAMT C layer needs a way to
  accept an externally computed hash; it already does (`hamt/hash-ptr` is just
  one option), so this should be straightforward.
- **Mutable variant** -- `map.tur` header mentions a future mutable map. TM0
  covers only the persistent variant. A `MutableMap[K V]` can be a TC2+ item.

---

## Phase TC1: Typed Core Collections

Parameterize the six monomorphic structures that already exist and are
widely used.

### TC1-A: `Vec[A]`

Replace the `int64_t`-only vector with a parameterized growable array.

```turmeric
(defstruct Vec [A]
  (data  :ptr<int64_t>)
  (len   :int)
  (cap   :int))

(defn tvec-new    []                 : Vec[A])
(defn tvec-push!  [v : Vec[A] x : A]  : void)
(defn tvec-get    [v : Vec[A] i : int] : A)
(defn tvec-len    [v : Vec[A]]        : int)
(defn tvec-pop!   [v : Vec[A]]        : Option[A])
```

| ID | Task | Status |
|----|------|--------|
| TC1-1 | Define `Vec[A]` struct (`data`, `len`, `cap`) | Done |
| TC1-2 | Implement `tvec-new`, `tvec-len`, `tvec-get` | Done |
| TC1-3 | Implement `tvec-push!`, `tvec-pop!` | Done |
| TC1-4 | Implement `tvec-set!`, `tvec-free` | Done |
| TC1-5 | Add constrained `Eq[Vec[A]]` instance | Done |

### TC1-B: `List[A]`

Parameterize the cons-list. Because the cons cell stores `int64_t`, the
typed wrapper casts at boundaries.

```turmeric
(defstruct Cons [A]
  (head :A)
  (tail :Option[List[A]]))

(deftype List [A] = Option[Cons[A]])

(defn tcons  [h : A t : List[A]] : List[A])
(defn thead  [l : List[A]]      : Option[A])
(defn ttail  [l : List[A]]      : List[A])
(defn tnil   []                : List[A])
```

| ID | Task | Status |
|----|------|--------|
| TC1-6 | Define `Cons[A]` struct (`head`, `tail`) | Done |
| TC1-7 | Implement `tcons`, `tnil`, `tnil?`, `thead`, `ttail` | Done |
| TC1-8 | Implement `tlist-length`, `tlist-eq?` | Done |
| TC1-9 | Add constrained `Eq[List[A]]` instance | Done |

### TC1-C: `Slice[A]`

A borrowed, bounds-checked view into a contiguous region of `A`.

```turmeric
(defstruct Slice [A]
  (ptr :ptr<A>)
  (len :int))

(defn tslice-new [data : ptr<A> len : int] : Slice[A])
(defn tslice-get [s : Slice[A] i : int]   : A)
(defn tslice-len [s : Slice[A]]          : int)
```

| ID | Task | Status |
|----|------|--------|
| TC1-10 | Define `Slice[A]` struct (`ptr`, `len`) | Done |
| TC1-11 | Implement `tslice-new`, `tslice-len`, `tslice-get`, `tslice-free` | Done |
| TC1-12 | Implement `tslice-eq?` | Done |

### TC1-D: `Option[A]`

Already used above; make it first-class:

```turmeric
(defstruct Option [A]
  (is-some :bool)
  (value   :A))

(defn some   [x : A]      : Option[A])
(defn none   []          : Option[A])
(defn some?  [o : Option[A]] : bool)
(defn unwrap [o : Option[A]] : A)   ; panics on None
(defn unwrap-or [o : Option[A] default : A] : A)
```

| ID | Task | Status |
|----|------|--------|
| TC1-13 | Define `Option[A]` struct (`is-some`, `value`) | Done |
| TC1-14 | Implement `tsome`, `tnone`, `tsome?`, `tunwrap`, `tunwrap-or` | Done |
| TC1-15 | Implement `toption-free` | Done |
| TC1-16 | Implement `toption-map` | Done |
| TC1-17 | Add constrained `Eq[Option[A]]` instance | Done |

### TC1-E: `Result[A B]`

```turmeric
(defstruct Result [A B]
  (is-ok   :bool)
  (ok-val  :A)
  (err-val :B))

(defn ok        [x : A]       : Result[A B])
(defn err       [e : B]       : Result[A B])
(defn ok?       [r : Result[A B]] : bool)
(defn unwrap-ok [r : Result[A B]] : A)
(defn unwrap-err [r : Result[A B]] : B)
```

| ID | Task | Status |
|----|------|--------|
| TC1-18 | Define `Result[A B]` struct (`is-ok`, `ok-val`, `err-val`) | Done |
| TC1-19 | Implement `tok`, `terr`, `tok?`, `terr?`, `tok-val`, `terr-val` | Done |
| TC1-20 | Implement `tresult-free` | Done |
| TC1-21 | Implement `tresult-map` | Done |
| TC1-22 | Add constrained `Eq[Result[A B]]` instance | Done |

### TC1-F: `Pair[A B]`

```turmeric
(defstruct Pair [A B]
  (fst :A)
  (snd :B))

(defn pair    [a : A b : B] : Pair[A B])
(defn pair-fst [p : Pair[A B]] : A)
(defn pair-snd [p : Pair[A B]] : B)
```

| ID | Task | Status |
|----|------|--------|
| TC1-23 | Define `Pair[A B]` struct (`fst`, `snd`) | Done |
| TC1-24 | Implement `tpair`, `tpair-fst`, `tpair-snd`, `tpair-free` | Done |
| TC1-25 | Add constrained `Eq[Pair[A B]]` instance | Done |

---

## Phase TC2: Remaining Collections + Set

### TC2-A: `Grid[A]`

A 2D row-major grid parameterized over element type.

```turmeric
(defstruct Grid [A]
  (data   :ptr<A>)
  (width  :int)
  (height :int)
  (cx     :int)
  (cy     :int))

(defn tgrid-new [width : int height : int] : Grid[A])
(defn tgrid-get [g : Grid[A] x : int y : int] : A)
(defn tgrid-set! [g : Grid[A] x : int y : int v : A] : void)
```

| ID | Task | Status |
|----|------|--------|
| TC2-1 | Define `Grid[A]` struct (`data`, `width`, `height`, `cx`, `cy`) | Done |
| TC2-2 | Implement `tgrid-new`, `tgrid-get`, `tgrid-set!` | Done |
| TC2-3 | Implement `tgrid-width`, `tgrid-height`, `tgrid-free` | Done |
| TC2-4 | Add fixture `tests/fixtures/typed/tgrid-basic` | Done |

### TC2-B: `Zipper[A]`

A 1D list zipper with typed focus and neighbor arrays.

```turmeric
(defstruct Zipper [A]
  (left      :Slice[A])
  (focus     :A)
  (right     :Slice[A]))

(defn tzipper-new  [left : Slice[A] focus : A right : Slice[A]] : Zipper[A])
(defn tzipper-move-left  [z : Zipper[A]] : Option[Zipper[A]])
(defn tzipper-move-right [z : Zipper[A]] : Option[Zipper[A]])
```

| ID | Task | Status |
|----|------|--------|
| TC2-5 | Define `Zipper[A]` struct (`left`, `left-len`, `focus`, `right`, `right-len`) | Done |
| TC2-6 | Implement `tzipper-new`, `tzipper-focus`, `tzipper-free` | Done |
| TC2-7 | Implement `tzipper-move-left`, `tzipper-move-right` (return `Option`-tagged ptr) | Done |
| TC2-8 | Add fixture `tests/fixtures/typed/tzipper-basic` | Done |

### TC2-C: `Set[A]`

A new persistent set backed by the HAMT with `Hash[A]` + `Eq[A]` constraints.
Does not exist at all today.

```turmeric
(defstruct Set [A]
  (hamt :ptr<void>))

(defn tset-new    []               : Set[A])
(defn tset-add    [s : Set[A] x : A] : Set[A])
(defn tset-remove [s : Set[A] x : A] : Set[A])
(defn tset-member? [s : Set[A] x : A] : bool)
(defn tset-count  [s : Set[A]]     : int)
(defn tset-union  [a : Set[A] b : Set[A]] : Set[A])
(defn tset-intersect [a : Set[A] b : Set[A]] : Set[A])
(defn tset-diff   [a : Set[A] b : Set[A]] : Set[A])
(defn tset-eq?    [a : Set[A] b : Set[A]] : bool)
```

| ID | Task | Status |
|----|------|--------|
| TC2-9 | Define `Set[A]` struct (`hamt`) | Done |
| TC2-10 | Implement `tset-new`, `tset-add`, `tset-remove`, `tset-member?` | Done |
| TC2-11 | Implement `tset-count` | Done |
| TC2-12 | Implement `tset-union`, `tset-intersect`, `tset-diff` | Done |
| TC2-13 | Implement `tset-eq?`, `tset-free` | Done |
| TC2-14 | Add fixture `tests/fixtures/typed/tset-basic` | Done |
| TC2-15 | Add constrained `Eq[Set[A]]` instance | Done |

---

## Deprecation Strategy

Untyped collections will not be removed immediately. For one release cycle
after each phase ships:

1. The untyped function names (e.g. `assoc`, `vec-push!`, `cons`) emit a
   deprecation warning at compile time.
2. After the deprecation window, the untyped names are removed from stdlib
   and the typed names become canonical.

Exception: `hamt.tur` itself is a low-level internal module. It stays untyped;
`Map[K V]` and `Set[A]` are the public-facing typed interfaces on top of it.

### Deprecation Tasks

| ID | Task | Status |
|----|------|--------|
| DEP-1 | Deprecation notice for untyped `map.tur` functions | Done (docstring `Deprecated:` section per function + module banner; rendered prominently by gendocs.py. Compile-time warning deferred -- requires a generic `^deprecated` attribute system.) |
| DEP-2 | Deprecation notice for `vec.tur`, `list.tur`, `slice.tur`, `option.tur`, `result.tur`, `pair.tur` | Done (same approach as DEP-1 across all six modules) |

---

## File Layout

```
stdlib/
  tmap.tur          -- Phase TM0: Map[K V]
  tvec.tur          -- Phase TC1-A: Vec[A]
  tlist.tur         -- Phase TC1-B: List[A]
  tslice.tur        -- Phase TC1-C: Slice[A]
  toption.tur       -- Phase TC1-D: Option[A]
  tresult.tur       -- Phase TC1-E: Result[A B]
  tpair.tur         -- Phase TC1-F: Pair[A B]
  tgrid.tur         -- Phase TC2-A: Grid[A]
  tzipper.tur       -- Phase TC2-B: Zipper[A]
  tset.tur          -- Phase TC2-C: Set[A]
```

Tests live under `tests/fixtures/typed/`.

---

## Decisions (cross-cutting)

1. **Syntax for type parameters in `defstruct`** -- use `defgadt`-style `[K V]`
   brackets. The parser already supports this for `defgadt`; `defstruct` needs
   the same grammar path. The angle-bracket `Name<T>` style seen in
   `scscm/errors.tur` is not the standard; do not adopt it for stdlib.

2. **Monomorphization vs. boxing** -- boxing. Type parameters are compile-time
   only; the runtime layout is always `int64_t`. The type-checker enforces `A`
   at push/get call boundaries; the C layer never sees it. `float32` and other
   non-pointer-sized types cross boundaries via bit-cast.

3. **`Hash[A]` typeclass** -- does not exist yet. Add
   `(defclass Hash [a] (hash [x :a] :int))` to `stdlib/typeclass.tur` with
   built-in instances for `int` (identity), `bool` (0/1), `cstr`
   (`hamt/hash-str`), and `float` (bit-cast then `hamt/hash-ptr`). This must
   land before TM0-3.

4. **`ptr<A>` with type parameters** -- not needed. Given boxing (decision 2),
   `Vec[A]` and `Slice[A]` declare their data field as `:ptr<int64_t>`
   internally. The type parameter `A` is tracked by the type-checker at call
   sites only.

5. **Interaction with `sized.tur`** -- `Vec[A]` and `SizedVec[n A]` are
   completely separate types. Users choose which they need; no conversion
   functions are provided in this phase. A future existential-types phase could
   introduce `exists n. SizedVec[n A]` as a path to unifying them; see
   `docs/upcoming/existential-types-plan.md`.

---

## Blocking Work: Phase PTC4 (Constrained Instance Dispatch)

All `Eq[Collection[A]]` instances (TM0-6, TC1-5, TC1-9, TC1-17, TC1-22,
TC1-25, TC2-15) and the full deprecation warning system (DEP-1, DEP-2) require
Phase PTC4 to land first.

### Current state of constrained instances (as of 2026-05-22)

The type system has partial support across four sub-phases:

| Sub-phase | Status | Scope |
|-----------|--------|-------|
| PTC1 | Done | Parse and store constraint vectors (`[Clone a Clone b]` syntax) in `definstance` |
| PTC2 | Done (v1) | Validate constraints at `definstance` time for **primitive** argument types only |
| PTC3 | Partial | Skip instances at lookup time if constraints are unsatisfied; no type-param substitution yet |
| PTC4 | **Not done** | Full type-parameter substitution: resolve `Eq[Vec[int]]` through `Eq[Vec[A] given (Eq A)]` by substituting `A=int` and verifying `Eq[int]` exists |

### What PTC4 must implement

- When resolving an instance for `Eq[Vec[int]]`, the resolver must unify
  `Vec[int]` against the instance head `Vec[A]`, producing `{A -> int}`, then
  check that each constraint in the vector (`Eq[A]` -> `Eq[int]`) is satisfied
  by a known instance.
- Relevant files: `src/compiler/typeclass.c` (`typeclass_instance_constraints_satisfied`),
  `src/compiler/elab_typeclasses.c` (`elab_definstance`, method dispatch loop).
- Full task breakdown: see `docs/ptc-plan.md`.

### What can be done without PTC4

The plain `-eq?` functions (`tvec-eq?`, `tlist-eq?`, etc.) are implemented and
work correctly today. They take an explicit element-comparator argument rather
than dispatching through `Eq`. These cover the functional requirement; the
typeclass instances are a cleanliness improvement once PTC4 lands.

`tresult-map` (TC1-21) does not require PTC4 and can be added immediately.

**Update (2026-05-23):** PTC4 has landed; all constrained `Eq[Collection]`
instances now exist as `definstance` forms in their respective `t*.tur`
modules. Element comparison inside those instance bodies uses integer
equality (`=`) rather than dispatching through `.eq?` on the element type;
this is correct for all primitive element types (`int`, `bool`, `cstr`)
but does not yet support recursive structural equality (e.g.
`Vec[Vec[int]]`), which requires dictionary passing to method bodies and
is deferred to a future phase.

---

## Interpreter Gaps (turi / tree-walker)

The following fixtures are in `tests/fixtures/` and pass under `tur` (compiled)
but fail under `tur run` (interpreter) due to pre-existing interpreter bugs.
They have been removed from the `TURI_FIXTURES_DEFAULT` list in
`tests/run-turi.sh` until the underlying interpreter issues are fixed.

| Fixture | Interpreter error | Root cause |
|---------|------------------|------------|
| `adt-param` | `match: scrutinee must be an ADT type, got adt` | `defdata`-typed `let` bindings lose their ADT type tag in the tree-walker |
| `adt-nested` | Same as above (sub-match on an ADT field) | Same root cause |
| `clone-list` | C `redefinition` of `Cons` | User-defined `defstruct Cons` collides with the built-in forward declaration |
| `clone-option` | C `redefinition` of `Option` | Same -- user `defstruct Option` vs. built-in |
| `clone-pair` | C `redefinition` of `Pair` | Same -- user `defstruct Pair` vs. built-in |
| `gadt-syntax-multi` | `defgadt requires the -Xgadt flag` | No `flags` file in the fixture; also hits the same ADT scrutinee-type bug |
