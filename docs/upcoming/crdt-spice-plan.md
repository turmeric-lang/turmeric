# `spices/crdt`: conflict-free replicated data types

> **Status:** C1-C3 **landed** 2026-09-11/12 as `spices/crdt` 0.1.0
> (turmeric-spices #67, #68): counters, causal core, `GSet` / `TwoPSet` /
> `ORSet`, HLC, both registers and the constrained `ORMap`, with eight test
> suites including the 400-seed convergence fuzzer. **C4 and C5 landed**
> 2026-09-29 as 0.2.0 (turmeric-spices #76): `DeltaCRDT` with delta mutators
> for all eight C1-C3 types, and `crdt/rga`, the sequence. Eleven suites, all
> green on both CI platforms. The two typeclass defects below, and every
> defect the C1-C3 build filed, are fixed and archived.
> **Open:** C6 only (the separate `crdt-sync` spice) -- not started. Spice
> follow-ups also open: `ReplicaId` is still a `defalias` for `Sym` although
> the `defopaque`-over-`Sym` defect was fixed 2026-09-16 (C1); `crdt/set`
> still uses the explicit-hash macros although the typed adder was fixed
> 2026-09-16 (C2); no HAMT-join benchmark yet (section 7); the spice has no
> `README.md`, so it does not appear on the docs site (44 of 48 spices have
> one). **Track:** post-v1 -- nothing on the v1
> line depends on this; it is written down so the design survives.
> **Type:** spice (in `../turmeric-spices/`), plus one candidate stdlib
> addition (`map-merge-with`) that the plan deliberately declines to make.
> **Sequencing:** last of three.
> [type-confusion-detection-plan.md](../archive/type-confusion-detection-plan.md) ->
> [../archive/lattice-vocabulary-plan.md](../archive/lattice-vocabulary-plan.md) -> this. C1's
> `JoinSemilattice` comes from the lattice plan rather than being redeclared
> here. Both were gated on two typeclass defects filed from this plan's own
> probes; both are fixed.

## 0. Summary

Turmeric has **no lattice vocabulary at all.** Greps for `Semigroup`,
`Monoid`, `semilattice`, `CRDT`, and `LWW` over `stdlib/`, `src/`,
`docs/guides/`, and all 45 spices return nothing. There is no `join`, no
`bottom`, and no class that expresses "these two values can be combined in
any order, any number of times, and you get the same answer."

That is a larger hole than it looks, because Turmeric already has the two
things a CRDT library normally has to build for itself:

- **Persistent state with snapshot semantics.** `stdlib/hamt.tur` is an
  immutable, structurally-sharing HAMT -- "all update operations return a
  new root; the old root remains valid". That is precisely the CvRDT state
  model, and `stdlib/set.tur` already exposes `set-union` / `set-intersect`
  / `set-diff` over it. A grow-only set is `set-union` with a name.
- **A type system that can state the laws.** Associative, commutative,
  idempotent is the entire correctness argument for a state-based CRDT, and
  Turmeric can put those three laws in a typeclass, attach them to
  `#refine{...}` contracts, and check them with a seeded fuzzer -- rather
  than leaving them in a doc comment, which is where nearly every CRDT
  library leaves them.

This plan proposes `spices/crdt`: a lattice core, the standard primitive
set (counters, sets, registers, maps), delta-state mutators carried on an
**associated type**, and a law-checking harness that is the reason to write
this in Turmeric rather than port it. Sequences (RGA) and the network sync
layer are explicitly separate, later phases.

## 1. What exists today, and the three real gaps

### 1.1 Substrate that already works

| Need | What we have | File |
| --- | --- | --- |
| Immutable state, cheap joins | persistent HAMT, structural sharing | `stdlib/hamt.tur` |
| Grow-only set join | `set-union`, `set-intersect`, `set-diff` | `stdlib/set.tur:225,262,301` |
| Keyed state | `Map K V` over the same HAMT | `stdlib/map.tur` |
| Replica identity | interned `Sym`, `sym=?` | `stdlib/sym.tur` |
| Per-CRDT delta type | associated types, incl. method return position | `docs/guides/typeclass-guide.md` |
| Deterministic clock for tests | `Mock-Time`, `mock-time-set`, `mock-time-get` | `stdlib/time.tur:168,201,219` |
| Seeded generation for the fuzzer | `Seeded-Random`, `seeded-next-int!` | `stdlib/random.tur:138,153` |
| Wire format | `derive-json` (defstruct), `derive-json-sum` (defdata) | `spices/json` |
| Equality / hashing for keys | `Eq`, `Hash`, `MapKey` | `stdlib/typeclass-eq.tur`, `typeclass-hash.tur`, `map.tur:370` |

Two facts about associated types are load-bearing below and are already
covered by fixtures, so the design is not speculating:

- A class method may **return** its own associated type, referenced bare
  inside the class body (`(sop-get [...] : Elem)`) --
  `tests/fixtures/typeclass-assoc-type-method-return`.
- A projection resolves per instance at annotation sites, including over
  multiple class parameters --
  `tests/fixtures/assoc-type-multiparam-projection`.

### 1.2 Gap 1 -- `map-merge` is right-biased, not value-combining

`stdlib/map.tur:681` documents its second argument as "wins on collision".
That is the wrong merge for every CRDT map: joining two `ORMap`s must join
the *values* under a shared key, not pick one. There is no `map-merge-with`
anywhere in stdlib.

This is the single most reusable thing the plan needs and does not have.
The resolution is in section 5: the spice ships it, over `map-iter`, rather
than growing stdlib.

The smaller half of the same gap: there was no `max` or `min` usable over an
abstract element type, so even a G-Counter's pointwise join had to define its
own. **Closed 2026-09-11** by lattice-vocabulary-plan L1: `max` and `min` are
constrained generics over `Ord` in `stdlib/typeclass.tur`, and `Ord` gained
`float` and `cstr` instances. What this section originally recorded -- that the
join of two integers under the natural order did not have a name -- was the
clearest measure of how absent the lattice vocabulary was.

### 1.3 Gap 2 -- no monotonic clock, and `get-time-ms` is wall time

`stdlib/time.tur:237` exposes `get-time-ms`. Wall time is not an ordering:
it goes backwards under NTP, and two replicas disagree by more than the
message latency that would have ordered them causally. An LWW register
keyed on it silently loses writes.

The spice must ship a **hybrid logical clock** (HLC) rather than reach for
the wall clock, and the LWW types must take the stamp as a parameter so the
tests can drive them from `Mock-Time` deterministically.

### 1.4 Gap 3 -- replica IDs need real entropy, and `random.tur` is `rand()`

`stdlib/random.tur` is libc `rand()` seeded with `time(NULL)` (this is
already documented as the motivating defect in
[../archive/secret-spice-plan.md](../archive/secret-spice-plan.md) section 2.3). Two replicas
started in the same second can draw the same ID, and a duplicated replica
ID breaks a G-Counter's *arithmetic*, not just its tidiness -- two replicas
sharing a slot overwrite each other's counts under a pointwise `max`.

Resolution: `ReplicaId` is caller-supplied, and the doc page says so
plainly. A convenience constructor draws from `crypto-random-bytes!` **if**
the secret spice exists; otherwise there is no default and the caller
passes a name. Never silently `rand()`.

## 2. Design

### 2.1 The lattice core

```turmeric
;;; A join-semilattice: `join` is associative, commutative, and idempotent.
(defclass JoinSemilattice [a]
  (join [x : a y : a] : a))

;;; The identity element for `join`.
(defclass BoundedJoinSemilattice [a]
  (bottom [] : a))
```

Two flat classes, not one with a superclass: `defclass` has no `extends`
(no superclass support exists in `src/compiler/`, and the typeclass guide
has no section for it), so a function that needs both lists both. That is
the whole cost, and it is visible:

```turmeric
;;; The partial order induced by the join: x <= y iff (join x y) = y.
(defn lattice-leq? [^JoinSemilattice A ^Eq A] [x : A y : A] : bool
  (eq? (join x y) y))

;;; Fold a batch of states into one. Order-independent by the class laws.
(defn join-all [^JoinSemilattice A ^BoundedJoinSemilattice A]
               [xs : (Vec A)] : A
  ...)
```

`join-all`'s signature is the argument for the whole class: it is correct
for *every* instance, and the only reason it is correct is the three laws.
Section 3 is about making that claim checkable instead of aspirational.

### 2.2 Identity and causality

```turmeric
;; An interned replica name. Caller-supplied -- see 1.4.
(defopaque ReplicaId :Sym)

;; One replica's contribution: (replica, counter). The unit of causality.
(defstruct Dot [replica : ReplicaId  counter : int])

;; A version vector: the causal history a replica has observed.
(defstruct DotContext [seen : (Map ReplicaId int)])

;; A hybrid logical clock stamp. Total order, causally consistent, and
;; bounded-drift against wall time.
(defstruct Hlc [wall : int  logical : int  replica : ReplicaId])
```

`Dot` and `Hlc` carrying a `Sym`-backed field is newly possible: `Sym`
became a legal `defstruct`/`defdata` field type in **v0.46.1** (`defstruct
/defdata: `Sym` is a field type`, commit `4ff9ad724`). Before that release
`(defstruct Dot [replica : Sym ...])` was `defdata: field has unrecognized
type :Sym` and this record could not be written at all. `defopaque` **over**
`Sym` is the one shape here that is not yet exercised by a fixture; see
section 7.

Per the CLAUDE.md strict rule, none of these are `:int`. A `Dot` is not "a
packed 64-bit thing"; a `ReplicaId` is not "an index". The counter inside
`Dot` is a genuine machine integer and stays `:int`.

### 2.3 The primitive set

| Type | State | Join | Phase |
| --- | --- | --- | --- |
| `GCounter` | `(Map ReplicaId int)` | pointwise `max` | C1 |
| `PNCounter` | two `GCounter`s | pointwise, per side | C1 |
| `GSet A` | `(Set A)` | `set-union` | C2 |
| `TwoPSet A` | `(Set A)` adds + `(Set A)` tombstones | per side | C2 |
| `ORSet A` | `(Map A (Set Dot))` + `DotContext` | add-wins, dot-aware | C2 |
| `LwwRegister A` | `A` + `Hlc` | larger stamp, `ReplicaId` breaks ties | C3 |
| `MvRegister A` | `(Set A)` + `DotContext` | keep concurrent values | C3 |
| `ORMap K V` | `(Map K V)` + `DotContext` | recursive join on `V` | C3 |

```turmeric
(defstruct GCounter [entries : (Map ReplicaId int)])
(defstruct PNCounter [pos : GCounter  neg : GCounter])
(defstruct LwwRegister [A] [value : A  stamp : Hlc])

;; `max` comes from stdlib now -- a constrained generic over Ord, added by
;; lattice-vocabulary-plan L1 (2026-09-11).  This plan used to define its own
;; `ord-max` here because stdlib had no `max` usable over an abstract element
;; type; it does now, for every Ord instance including float and cstr.
(definstance JoinSemilattice [GCounter]
  (join [x y]
    (GCounter (map-merge-with max (.entries x) (.entries y)))))

;;; The counter's reading. NOT part of the lattice -- it is the query.
(defn gcounter-value [c : GCounter] : int ...)
```

`ORMap`'s instance is the one that needs the whole apparatus at once: its
join is `map-merge-with` where the combining function is `join` at the
*value's* instance, so `(ORMap K V)` is a `JoinSemilattice` exactly when `V`
is. That is a constrained instance
(`docs/guides/typeclass-guide.md` section "Parametric Instances and
Constraints"), and it is what makes nesting work -- an `ORMap` of
`PNCounter`s merges correctly with no code specific to that pairing.

### 2.4 Delta-state, carried on an associated type

Shipping full state on every change is what makes naive CvRDT unusable over
a network. The delta-state formulation (Almeida, Shoker, Baquero) has each
mutator return a small value from the *same* lattice, which the receiver
joins. Turmeric can type that channel instead of erasing it:

```turmeric
(defclass DeltaCRDT [a]
  (type Delta : Type)
  (delta-bottom [] : Delta)
  (delta-join   [d : Delta e : Delta] : Delta)
  (apply-delta  [x : a d : Delta] : a))
```

`Delta` is referenced bare in method position inside the class body, which
is the shape `typeclass-assoc-type-method-return` already pins, and projects
as `(Delta GCounter)` at annotation sites outside it.

For a `GCounter` the delta is another `GCounter` holding one entry, so
`(type Delta = GCounter)`. For an `ORSet` it is a dot-store fragment plus
the causal context those dots were observed in -- a genuinely different type
from the set itself, which is exactly why the delta type must be per
instance rather than `a` itself. Most implementations either force
`Delta = a` (and lose the compression for causal types) or erase the
delta to bytes at the boundary. The associated type gives both.

Delta buffering (what to keep, when a peer's acknowledgment lets you drop
it) is a sync-layer concern and lands in C6, not here.

### 2.5 Purity, ownership, and regions

Every `join` is a pure function of two states: `#fx{}`, no I/O, no globals.
That is worth stating because it buys three things at once -- the law
fuzzer can call joins in any order, `with-region` can wrap a merge burst,
and the whole core stays runnable under `tur --interpret`.

The interpreter point has a concrete design consequence. `tests/run-turi.sh`
PASS-skips any fixture whose program contains a user inline-C block, so
**every inline-C body in the core costs interpreter coverage.** The lattice
core, the counters, the sets, and the registers should be written in plain
Turmeric over the existing HAMT natives, and inline-C reserved for
`map-merge-with`'s iterator walk if the Turmeric-level version measures
badly. This is the opposite of the secret spice, where the guarantees *are*
the inline C.

On regions: the spice introduces no new store primitive, so it adds no
`TUR_REGION_NOTE` obligations -- it writes through `map-assoc`, `set-add`
and the HAMT setters, which are already on the hooked list in CLAUDE.md. If
a later phase adds a mutable dot-store for performance, that changes and the
region-hook rule applies in the same commit.

Ownership follows the existing HAMT convention: joins return a new root and
leave both inputs valid. `map-free` / `map-free-all`'s split (wrapper vs.
contents) is the trap to document per type, since a `GCounter`'s entries are
`int` values but an `ORMap`'s are themselves CRDTs that own HAMTs.

No `:linear`. It is tempting -- the sqlite and secret spices both use it --
but a CRDT state is *meant* to be copied, shipped, and joined from many
places at once. Linearity would fight the data type. A `Replica` handle
owning a transport socket in C6 is a different story and may well want it.

### 2.6 Serialization

`derive-json` (defstruct) and `derive-json-sum` (defdata) from `spices/json`
cover the wire format with no new encoder, and give a readable format for
debugging a divergence, which is most of what you want early. The `:spices`
dep is `:optional true` in the manifest, matching how `spices/json` itself
depends on `spices/test` -- the core must build without a transport.

Binary framing belongs to
[../archive/msgpack-spice-plan.md](../archive/msgpack-spice-plan.md) when that lands;
CRDT deltas are a good forcing case for it and a bad reason to block on it.

## 3. Law checking -- the part that is not a port

A CRDT is correct if and only if its join is associative, commutative, and
idempotent, and its query is monotone in the lattice order. Every CRDT
library asserts this in prose. Turmeric can do better, in three layers of
increasing cost:

**Layer 1 -- the laws as contracts.** `#refine{...}` contract types are
always-on (the experiments registry is empty; see
`docs/guides/contract-types-guide.md`), so an instance can carry a checked
post-condition on its join in debug builds. Note the Release caveat from the
project's own experience: a Release-built `tur` compiles contracts out under
`NDEBUG`, so law checking is a Debug-build activity and the suite must say so
(this is the same trap recorded for the refine fixtures).

**Layer 2 -- a law harness.** One function per law, generic over the class:

```turmeric
(defn law-associative? [^JoinSemilattice A ^Eq A] [x : A y : A z : A] : bool
  (eq? (join (join x y) z) (join x (join y z))))

(defn law-commutative? [^JoinSemilattice A ^Eq A] [x : A y : A] : bool
  (eq? (join x y) (join y x)))

(defn law-idempotent? [^JoinSemilattice A ^Eq A] [x : A] : bool
  (eq? (join x x) x))
```

These are written once and instantiate for every type in the table. That is
the payoff for the typeclass: a new CRDT gets its correctness suite by
declaring an instance.

**Layer 3 -- a seeded differential fuzzer.** `spices/test` is `describe`/`it`
with no generators and no shrinking, so the harness ships its own generator
over `Seeded-Random`. The model is `tests/saffron-fuzz-src.py`, the in-repo
precedent for randomized differential testing: generate a random set of
per-replica operation sequences, apply them in N random delivery orders
(including duplicates and reorderings, which is the entire point), and
assert every replica reaches the same state. A failing seed is the repro.

Convergence under arbitrary delivery order is the property users actually
care about, and it is not implied by the three laws alone for the causal
types -- `ORSet` needs its dot context to be correct too. Layer 3 is what
catches that; layers 1 and 2 will not.

## 4. Back-end and dialect caveats

- **Interpreter:** the core is designed to run there (section 2.5). This is
  a real advantage over the secret spice and should be protected by keeping
  inline-C out of the primitives, not merely hoped for.
- **JIT:** nothing here is JIT-specific, but the corpus is a good stress
  case for the engine -- deeply nested generic joins over parametric records
  are exactly the shape that surfaced `jit-ffi-interp-refuses-parametric-
  record-field` and the `any` -> scalar miscompile fixed in v0.46.1. Expect
  to find at least one, and file it rather than working around it.
- **Saffron:** CRDT values through `any` are out of scope. The types are the
  point.

## 5. Why a spice, and why `map-merge-with` does not go in stdlib

The stdlib-growth gate is that additions are justified by tur-signal's
actual call surface. tur-signal does not merge replicated state, so
`JoinSemilattice`, `Dot`, and the primitives are spice material without
argument.

`map-merge-with` is the genuinely arguable one. It is a general-purpose
combinator, it is the obvious companion to an existing right-biased
`map-merge`, and every caller who has ever wanted "merge, but combine the
values" has had to walk the iterators by hand. The plan still says **spice**,
for one reason: putting it in stdlib means committing to its ownership
semantics for owned value types (who retains the combined box) at a moment
when no stdlib caller exists to pin them down. Ship it in `crdt/lattice`,
let the CRDT primitives be its first real users, and promote it to stdlib
once the ownership question has been answered by something other than a
guess. If it does get promoted, `map-merge`'s docstring should gain a
cross-reference, since discovering the right-biased one first and assuming
it combines is a live foot-gun today.

One stdlib-adjacent fix is worth doing regardless of this plan's fate:
`get-time-ms`'s docstring should say it is wall time and is not an ordering
(section 1.3). One comment block, no behavior change.

## 6. Phases

- **C1 -- DONE 2026-09-11.** `spices/crdt/` exists: manifest, `crdt/lattice`
  (`map-merge-with`, `join-all`), `crdt/counter` (`GCounter`, `PNCounter`,
  their `JoinSemilattice` / `BoundedJoin` / `Eq` instances), and
  `tests/crdt/test_counter.tur`, which passes. The classes themselves come
  from `stdlib/typeclass-lattice.tur` (lattice-vocabulary-plan L3) rather than
  being redeclared here, so this phase is smaller than written.

  The test checks convergence, not just arithmetic: two replicas that have seen
  different subsets of the updates merge to the same reading in either order
  (commutativity), re-delivering a state changes nothing (idempotence -- the
  law duplicate delivery actually leans on), and `join-all` over an empty `Vec`
  answers `bottom`.

  **Zero inline C**, as section 2.5 argued for.

  Four things the build taught us, all recorded at their sites:

  - **`(defopaque ReplicaId :Sym)` did not compile** (fixed 2026-09-16).
    Section 2.2 flagged it as the one unverified shape and was right to: the
    opaque path stored the interned symbol POINTER into an `int64_t` slot and
    cc rejected it (`-Wint-conversion`, an error on clang >= 21). Filed as
    [defopaque-over-sym-skips-the-ptr-bridge](../archive/defopaque-over-sym-skips-the-ptr-bridge.md);
    an opaque over `:Sym` or `:cstr` now takes the pointer spelling, so the
    newtype spelling is open to the spice again.
  - **The `defstruct` fallback does not work either, for a different reason.**
    It compiles, but then the map key is a by-value struct, which needs a
    `MapKey` instance -- whose `mk-cmp` returns a comparator function pointer
    and can only be written in inline C. That would cost the spice its
    interpreter coverage for the sake of a newtype. `ReplicaId` is a
    transparent `defalias` for `Sym` for now: it names the concept without
    buying type safety, and `Sym` is a real type (an interned name), not the
    `:int` stand-in CLAUDE.md forbids. One line to change when the defect
    lands.
  - **A `defalias` is not an exportable name**, so `ReplicaId` cannot appear in
    the module's `(export ...)` list. Callers spell `Sym` or use `replica`.
    Another reason to want the newtype.
  - **Counter states are affine.** `GCounter` holds a Map handle, so `join`
    CONSUMES its arguments and the readers take `^borrow`. The test rebuilds
    divergent states per assertion rather than reusing them -- which is honest
    anyway: a replica does not hand its state away and keep using it.

  `map-merge-with` is written over the boxed HAMT iterator
  (`hamt/iter-alloc` / `-advance!` / `-cur-key` / `-cur-val` / `-destroy!`) in
  plain Turmeric, so section 1.2's gap is closed without inline C and without
  touching stdlib.
- **C2 -- DONE 2026-09-11.** `crdt/causal`
  (`Dot`, `DotContext`), `crdt/set` (`GSet`, `TwoPSet`) and `crdt/orset`
  (`ORSet`) are all in and tested, and **law layer 3 landed** as
  `tests/crdt/test_converge.tur`: a seeded fuzzer over a pure-Turmeric LCG, 200
  causally-independent seeds plus 200 with a mid-stream sync, checking four
  delivery orders (including a duplicate delivery) plus a model oracle.
  Replicas are rebuilt by replaying the seed rather than copied -- which is
  also what the affine states require.

  **The most useful thing it produced is a limit on itself**, measured by
  breaking the OR-Set merge two ways:

  | merge broken how | convergence fuzzer | test_orset scenarios |
  | --- | --- | --- |
  | context test inverted | **caught** | caught (scenario 2) |
  | context test removed | **NOT caught** -- 400 seeds green | caught (1 and 3) |

  Keeping every dot is perfectly order-independent; it just means removes never
  propagate. A convergence check cannot see that, because what it checks is
  still true -- every order agrees, on the wrong answer. So section 3's claim
  that "layer 3 is what catches that; layers 1 and 2 will not" is **half
  right**: layer 3 catches order-dependence over a state space nobody would
  enumerate, and the hand-written semantic scenarios catch a merge that keeps
  too much. Neither subsumes the other, and a project that ships only the
  fuzzer would have a green suite over an OR-Set whose removes do nothing.

  **The OR-Set is the reason that layer exists, and its test already shows
  why.** Its three scenarios were validated by breaking the merge's "has the
  other side's context seen this dot" test two ways, and measuring:

  | merge broken how | scenarios that fail |
  | --- | --- |
  | test removed (keep every dot) | 1 and 3 -- **2 passes** |
  | test inverted (keep only seen) | 1, 2 and 3 |

  So a suite with only the concurrent-add scenario would certify an
  implementation where removes never propagate, and one without it would
  certify a remove-wins set. Three hand-written scenarios were enough to
  separate those two failures -- but only because someone thought to write all
  three. That is exactly the argument for generating delivery orders instead of
  enumerating them.

  Still zero inline C across all four modules.

  Three things worth carrying forward:

  - **`dot` is not an available name.** `stdlib/macros.tur` -- auto-loaded --
    already defines a `dot` macro for method-call sugar, and macro expansion
    precedes everything, so a function of that name is simply unreachable. The
    constructor is `mk-dot`. Same collision shape that retired the `max`/`min`
    macros in lattice-vocabulary-plan L1; **check a name against `macros.tur`
    before exporting it.**
  - **Affine state dominates how the tests read.** Every CRDT state here holds
    a Map or Set handle, so `join` and every mutator CONSUME their argument.
    An assertion that joins and one that reads the same state need two states,
    so the tests rebuild divergent replicas per assertion. That is more honest
    than it first looks -- a replica does not hand its state away and keep
    using it -- but it is the single biggest difference from how this code
    would read in a GC'd language, and C5's sequence types will feel it more.
  - **`Eq [GSet]` is size-only**, and the file says so. An element-wise
    comparison needs a set iterator the stdlib `Set` API does not expose the
    way the HAMT one does, so the law functions are correspondingly weaker for
    `GSet` than for the counters. This is exactly the "Eq must be structural"
    risk the lattice plan's section 5 named, hitting for a different reason
    than pointer identity.

  Also filed while building it:
  [set-add-elem-hash-disagrees-with-set-member](../archive/set-add-elem-hash-disagrees-with-set-member.md)
  -- the typed `set-add-elem__` added elements that `set-member?` could not
  find, so `crdt/set` uses the explicit-hash macro pair throughout. Fixed
  2026-09-16 (two emitter defects, neither in set.tur); the typed adder is
  usable again and the spice's spelling can be simplified when it is next
  touched.
- **C3 -- registers + maps. DONE 2026-09-12.** `crdt/hlc`, `crdt/register`
  (`LwwRegister`, `MvRegister`), `crdt/ormap`. Seven test suites green.

  Two notes on how C3 differs from the sketch above:

  1. **`ORMap` carries BOTH merges. DONE 2026-09-12.** Section 2.3's constrained
     instance now ships: `(ORMap V)` is a `JoinSemilattice` exactly when `V` is,
     so `join`, `join-all` and the lattice laws work on a map of any CRDT value
     and it nests. `ormap-merge-with` stays beside it -- strictly more general,
     since two ORMaps over one value type can merge differently.

     `test_ormap_join` asserts the point that matters: the same map code gives
     DIFFERENT answers for two value lattices (max vs min). A map that ignored
     its element's instance would give one answer and pass any convergence-only
     test -- which is exactly what every earlier attempt did, silently.

     Getting there took **five** compiler fixes (all landed):

     | Defect | What it did |
     | --- | --- |
     | [phantom type param does not drive monomorphization](../archive/phantom-type-param-does-not-drive-monomorphization.md) | a value read out of a carrier and ascribed back to the class var was not seen as a dispatch |
     | [constrained relay dispatch collapses](../archive/typeclass-constrained-relay-dispatch.md) | a constrained generic calling another was never specialized -- the fold-helper shape exactly |
     | [base body picks an aggregate instance](../archive/phantom-constrained-generic-base-body-picks-aggregate-instance.md) | the representative search had no tier for a `defopaque` newtype, so it chose an aggregate and the base body would not compile |
     | [generic defn forward reference](../archive/generic-defn-forward-reference-wrong-arity.md) | both pre-passes read the TYPE-param vector as the value params, so a generic could not be called from above |
     | [bare parametric `:heap` base repr](../archive/bare-parametric-heap-base-repr-disagreement.md) | `repr_of` said heap-ptr where the emitter said carrier, ICEing on the merge temp |

     Two design points worth keeping:

     - **`ORMap` is `:heap`**, like stdlib's `Map`/`Vec`/`Set`. A non-heap
       parametric struct becomes a distinct by-value C aggregate per
       instantiation, and that path types the result temp at the carrier while
       the constructor returns the aggregate.
     - **Both merges share ONE fold.** The constrained path passes `__vjoin`
       -- a constrained generic -- as the `f` of the same fold the explicit
       path uses. That collapsed onto a single instance until
       [the fn-value defect](../archive/constrained-generic-as-fn-value-collapses.md)
       was fixed, which is why an earlier revision carried a second copy of the
       loop differing in one line.
     - **`V` is phantom**, but `ormap-new` no longer needs an ascription: a
       phantom-only parametric ADT has one layout for every instantiation, so
       its base ctor is real (it used to be an abort trap), and `V` is pinned by
       a sibling argument as in `(ormap-put (ormap-new) r k v)`. `DotContext` is
       `:heap` for the same reason -- a by-value aggregate field would have made
       ORMap's base ctor need the generic boxing convention.

  2. **Deterministic tests come from parameter-passed wall time, not
     `Mock-Time`.** Every `crdt/hlc` operation takes `now` as an argument, so
     the tests are exact without `stdlib/time.tur` -- which would also have
     cost the spice its inline-C-free property (2.5).

  **Nine compiler defects** were found building C3 -- **all nine fixed**:

  | Defect | What it did |
  | --- | --- |
  | [`clone_struct_app_type` SEGV](../archive/clone-struct-app-type-segv-on-null-arg.md) | crashed on a parametric instance head whose method recurses into the type parameter |
  | [fn-typed param forwarded to a later defn](../archive/fn-typed-param-forwarded-to-a-later-defn-miscasts.md) | emitted a carrier cast onto a `tur_poly_fn_t`; trigger was source order |
  | [phantom type param does not drive monomorphization](../archive/phantom-type-param-does-not-drive-monomorphization.md) | a dispatch reached through an ascription was not detected |
  | [constrained relay dispatch collapses](../archive/typeclass-constrained-relay-dispatch.md) | a constrained generic calling another was never specialized |
  | [base body picks an aggregate instance](../archive/phantom-constrained-generic-base-body-picks-aggregate-instance.md) | no representative tier for a `defopaque` newtype, so the base body would not compile |
  | [generic defn forward reference](../archive/generic-defn-forward-reference-wrong-arity.md) | both pre-passes read the TYPE-param vector as the value params |
  | [bare parametric `:heap` base repr](../archive/bare-parametric-heap-base-repr-disagreement.md) | `repr_of` and the merge-temp emitter disagreed; ICE, plus a duplicate-typedef guard bug |
  | [phantom-only parametric base ctor was a trap](../archive/generic-defn-forward-reference-wrong-arity.md) | an empty container needed a hand-written ascription; `(ormap-new)` aborted |
  | [constrained generic as a fn value collapses](../archive/constrained-generic-as-fn-value-collapses.md) | forced two near-identical folds; also surfaced a latent use-after-free in spec interning |

  Five of the nine were **silent wrong answers**, every one a constrained
  generic quietly running the wrong instance. That is the argument for building
  these CRDTs against convergence tests whose value types have DIFFERENT joins,
  rather than only type-checking them: a merge that ignores its element's
  instance still converges -- on the wrong answer.

  The HLC ships a drift bound (`hlc-max-drift`) so a remote replica with a
  wrong clock cannot drag this one forward permanently -- 1.3's requirement.

- **C4 -- deltas. DONE 2026-09-29.** `DeltaCRDT` lands in `crdt/lattice` (a
  `defclass` is not an exportable name, and neither are its methods, so a
  `crdt/delta` module of its own would have had nothing real to export; the
  methods are ambient once the defining module is in the build). Delta
  mutators for all eight C1-C3 types, and `tests/crdt/test_delta.tur` asserts
  the equivalence this phase was named for -- applying a sequence of deltas
  equals joining the full states -- plus size, idempotence under duplicate
  delivery, and buffering, per type.

  **Section 2.4 was right about the ORSet, and the reason is sharper than it
  wrote.** The associated type is load-bearing, and the split falls exactly
  along whether a type carries a causal context:

  | | `Delta` |
  | --- | --- |
  | GCounter, PNCounter, GSet, TwoPSet, LwwRegister, MvRegister, Rga | the state type |
  | **ORSet, ORMap** | **a distinct delta type** |

  A `DotContext` is a compacted VERSION VECTOR -- `alice -> 3` means alice's
  dots 1, 2 and 3 -- which is exact for a state and wrong for a delta.
  Measured on the first attempt, which shipped `Delta = ORSet`: an add-delta
  from alice@3 carrying `{alice: 3}` claims alice@1 and alice@2 as observed,
  so the receiver reads its own untouched elements as deliberately removed
  and drops them. Delivering ONE delta to a replica holding two elements left
  it holding one, silently. So `crdt/causal` gained an exact `DotSet` and the
  two causal types carry `OrsetDelta` / `OrmapDelta` instead.

  **Two shapes in 2.4's sketch do not compile**, both filed:
  [associated-type-unusable-nullary-and-generic](../archive/associated-type-unusable-nullary-and-generic.md).

  1. `(delta-bottom [] : Delta)` -- a nullary method whose only mention of the
     class variable is through the associated type is not registered at all.
     Dispatch reads the first parameter; there is nothing to dispatch on. A
     two-parameter class with a fundep (`[a d] | (a -> d)`) was probed as the
     alternative and fails the same way, because dispatch reads only the FIRST
     class variable. Hence the `^borrow x : a` WITNESS parameter on
     `delta-bottom` and `delta-join` -- they take a state they do not read.
  2. `(Delta A)` projected at a type VARIABLE is rejected; at a concrete type
     it works. So the class cannot be used generically, and C4 ships no
     generic delta helpers: every `apply-delta` caller names a concrete state
     type. That is the biggest limitation of C4 as shipped.

  **Both now compile (report archived 2026-10-12):** the projection at a type
  variable since 2026-10-03, and the nullary method -- `(:: (delta-bottom)
  (Delta A))` inside a generic, `(:: (delta-bottom) (Delta GCounter))` at a
  concrete one -- since 2026-10-09 / 2026-10-12. Dropping the witness
  parameter and adding generic delta helpers is open spice work.

  Deltas here require CAUSAL DELIVERY, which is the assumption `crdt/causal`
  already documents for its version-vector compaction -- applying a delta
  folds its exact dots into the receiver's version vector, and that is only
  lossless while a replica's dots arrive without gaps. C6 owes this module
  that; an exception-cloud `DotContext` is the alternative and costs every
  state operation.

- **C5 -- sequences. DONE 2026-09-29.** `crdt/rga`: a replicated growable
  array over LAMPORT ids -- not the per-replica counters `crdt/causal` mints,
  because RGA's linear integration scan is correct only while a node's id
  exceeds its origin's -- with tombstones and an O(n) index-to-position walk.
  `test_rga` pins six hand-written scenarios; `test_rga_converge` runs 300
  seeded cases in three sweeps.

  **The scenario worth the file is `test-no-interleave`**: concurrent runs of
  "abc" and "xyz" must merge to one run then the other, never "axbycz". A
  fuzzer cannot see that -- interleaving converges perfectly -- which is the
  same complementarity C2 measured between `test_orset` and `test_converge`,
  arriving for a third time.

  **Every assertion was mutation-tested, and three mutations survived the
  first draft.** Each got a test written for it:

  | mutation | caught by, after | why it slipped |
  | --- | --- | --- |
  | retract by "counter at or below", not the exact dot | `test-orset-remove-is-exact` | every other test retracts the LOWEST dot its replica minted, so there is nothing below to over-reach onto |
  | drop ORMap's already-seen guard | `test-ormap-duplicates` | nothing else re-delivered anything |
  | drop the clock max in `rga-merge` | `test-clock-monotone` | **invisible to the fuzzer**: every replica corrupts the id tree identically and they still agree |

  The third is the one to remember. It is C2's finding again in its strongest
  form: a convergence check cannot see a bug that converges, and the only
  thing that caught it was stating the invariant directly as a white-box
  assertion.

  Two compiler defects were filed building it, neither blocking:
  [vec-push-byvalue-struct-param-emits-unbridged-pointer](../reported/vec-push-byvalue-struct-param-emits-unbridged-pointer.md)
  (a `defstruct` wider than two words cannot be pushed into a `(Vec T)` from a
  parameter -- hence the eight-parameter `__ins-at!`) and
  [phantom-parametric-heap-let-binding-repr-ice](../reported/phantom-parametric-heap-let-binding-repr-ice.md)
  (which also records the pre-existing macOS-only red in `test_ormap.tur`,
  fixed in the same PR).
- **C6 -- sync.** A separate `crdt-sync` spice over `valkey` pubsub or the
  `ws-*` spices: delta buffering, acknowledgment, anti-entropy. Separate
  because a transport dependency has no business in the core, and because
  the core is testable without one.

C1-C3 are the useful unit; someone can build with the spice at the end of
C3. C4 is what makes it usable over a network, C5 is a research-grade
follow-on, C6 is plumbing. C1-C5 are in; only C6 is left.

## 7. Risks and open questions

- **`defopaque` over `Sym` is unverified.** `(defopaque ReplicaId :Sym)` is
  the natural spelling, but the fixture coverage that landed in v0.46.1 is
  for `Sym` as a `defstruct`/`defdata` **field**, not as a `defopaque`
  representation. Probe it first; the fallback is a one-field
  `(defstruct ReplicaId [name : Sym])`, which is verified. Do not fall back
  to `:int`.
- **Tombstone growth is the standing CRDT complaint.** `TwoPSet` and `ORSet`
  grow without bound under churn. The honest answer is a documented
  compaction protocol (stable-causal-cut based) in C6, not a claim that it
  does not happen. The doc page should carry a "when not to use this" section
  naming it.
- **Constraint lists get long without superclasses.** `[^JoinSemilattice A
  ^BoundedJoinSemilattice A ^Eq A ^Hash A ^MapKey A]` is a realistic
  signature for an `ORMap` helper. Functional dependencies
  (`docs/guides/typeclass-guide.md`) may reduce some of it. If the
  ergonomics turn out to be the thing that kills adoption, that is a real
  data point for superclass support as a compiler feature -- file it as a
  report rather than fighting it in the spice.
- **`Eq` for law checking must be structural, not pointer identity.** The
  law functions are all `eq?` calls, and the existing helpers differ in
  whether they take one: `set-eq?` (`stdlib/set.tur:344`) takes no comparator
  while `set-eq-cmp?` (`:382`) does, and both `map-eq?` (`stdlib/map.tur:773`)
  and `map-eq-k?` (`:813`) require a value comparator. A law suite wired to a
  comparator-free variant would compare pointers and pass on everything. Pin
  this with a deliberately-failing instance in the C1 tests.
- **HLC needs a bounded-drift check.** An HLC whose wall component runs away
  from real time is a correctness problem that looks like nothing until
  restart. Ship the max-drift rejection with the clock, not after.
- **Nothing here is measured.** No benchmark exists for HAMT join cost at
  the sizes a real document hits. C1 should land a small benchmark alongside
  the correctness suite, so C5's design (which is where performance stops
  being free) has a baseline to argue against.

## 8. References

- Shapiro, Preguica, Baquero, Zawirski, *A comprehensive study of
  Convergent and Commutative Replicated Data Types* (INRIA RR-7506, 2011)
  -- the primitive catalog in section 2.3.
- Almeida, Shoker, Baquero, *Delta State Replicated Data Types* (2016) --
  the delta formulation in section 2.4.
- Kulkarni et al., *Logical Physical Clocks* (2014) -- the HLC in 2.2.
- Weiss, Urso, Molli, *Logoot*; Roh et al., *RGA* -- candidates for C5.
- In-tree: [../archive/secret-spice-plan.md](../archive/secret-spice-plan.md) (CSPRNG, and the
  template this plan follows), `docs/guides/typeclass-guide.md`
  (associated types, constrained instances),
  `docs/guides/contract-types-guide.md` (`#refine{...}`).
