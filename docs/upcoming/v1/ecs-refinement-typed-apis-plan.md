---
title: ECS Refinement-Typed APIs Plan
category: Planning
description: Rewritten after refinement types landed. What the shipped `#refine{...}` surface can and cannot do for `tur-ecs`, measured against the real compiler, and the three compiler gaps that stand between here and a strict-aliveness ECS API.
---

# ECS Refinement-Typed APIs -- Plan

> **Status 2026-07-26 -- rewritten.** The previous revision of this file was
> written while refinement types were still unbuilt, and it is wrong in ways
> that matter: it assumed a `(refine T P)` type former that never shipped, it
> described the ECS accessors as `option`-returning when they are not, and it
> proposed a `/has` world bound that is not a refinement type at all.
>
> RT0--RT7 and S0--S4 landed 2026-07-24/25 behind `--enable=refined` /
> `#lang turmeric refined` (see
> [`refinement-types-plan.md`](../../archive/refinement-types-plan.md) and
> [the guide](../../guides/refinement-types-guide.md)). So this plan can stop
> speculating and start measuring. **Everything asserted below about compiler
> behaviour was checked against `build-release/tur` at VERSION 0.30.8**; each
> claim carries the probe that establishes it. **Later probe updates carry
> their own versions** -- the RE2 phase was re-measured at v0.37.0
> (2026-08-20 profile), `023013c8` (2026-09-05) and v0.59.0 (2026-10-02);
> read the dated block quotes in a phase before relying on a 0.30.8 claim
> about it.

## What changed, in one paragraph

The gating question is no longer "do refinement types exist". It is "can a
refinement predicate say anything about a **mutable world**". The answer today
is no, and that is not an oversight -- it is the mechanism that keeps the
feature sound. A measure is congruent (two occurrences denote one value) only
when the compiler can prove the callee pure, and `sized-alive?` reads a
generation counter out of a malloc'd control block through inline C. Making it
congruent anyway is precisely the miscompile shape the refinement work found
and fixed three separate times. So the ECS strict-aliveness API is blocked on a
*sound* way to talk about state, not on a flag.

---

## Ground truth: what the shipped compiler does

Six probes, all run against a Release build of the current tree.

### 1. A boolean-valued measure cannot be a predicate atom

```turmeric
(defn alive? [w : int e : int] #fx{} : bool (= w e))
(defn use-it [w : int e : #refine{ x : int | (alive? w x) }] : int e)
```

`refine: 1 obligation(s): 0 proven, 0 refuted, 1 unknown`, and
`TUR_REFINE_DUMP=1` prints **no VC at all** -- the obligation never reaches the
solver.

The cause is in the encoder: `enc_measure` (`refine_collect.c`) declares every
measure with `vc_declare_ufunc(..., VS_INT, ...)`, and `refine_vc_build`
rejects a goal whose sort is not `VS_BOOL` with *"predicate does not denote a
proposition"*. There is no way to write `(alive? w e)` as a predicate. This is
the single most load-bearing gap for ECS, because *every* domain predicate an
ECS wants -- alive, has-component, in-bounds-for-this-world -- is naturally a
`bool`-returning function.

Tracked in [`refine-predicate-measures-plan.md`](../../archive/refine-predicate-measures-plan.md).

### 2. The int-valued workaround **works today**

Spell the same predicate as an `int` comparison and the obligation discharges:

```turmeric
(defn alive-i [w : int e : int] #fx{} : int (if (= w e) 1 0))
(defn use-it [w : int e : #refine{ x : int | (= (alive-i w x) 1) }] : int e)

(defn caller [w : int e : int] : int
  (if (= (alive-i w e) 1)      ;; guard
    (use-it w e)               ;; crossing
    0))
```

`refine: 1 obligation(s): 1 proven, 0 refuted, 0 unknown`.

This is the important positive result, and it reshapes the whole plan. The
crossing's path-condition recovery (landed 2026-07-25) already does the work
the old plan wanted a bespoke `entity-alive!` promotion form for: a call
guarded by `(if (alive? ...) ...)` discharges the callee's aliveness
refinement, with **no new type former, no flow-typing, and no `-alive`
accessor family**. The old RE0 design is obsolete; what is left is choosing the
encoding.

### 3. ...but only because `alive-i` is pure

Rewrite `alive-i` to read a generation out of a handle through inline C -- i.e.
write the function the ECS actually has -- and the same program reports
`0 proven, 1 unknown`. The purity walk is default-deny; inline C is impure;
each occurrence of the call gets a distinct symbol; the guard says nothing
about the argument.

Note it is **not** an error: `TUR-E0375` fires only on *proven* impurity, and
an `(unsafe ...)`-wrapped inline-C call classifies `RT_P_UNKNOWN`, which the
diagnostic reads as pure and congruence reads as impure. So the failure mode
here is silent loss of the proof, not a rejection.

This is the wall. Tracked in
[`refine-stateful-measures-plan.md`](../../archive/refine-stateful-measures-plan.md).

### 4. A `#refine{...}` on a `defopaque` parameter compiles

```turmeric
(defopaque Entity :int)
(defn use-it [e : #refine{ x : Entity | (>= (idx-of x) 0) }] : int 0)
```

Compiles clean. `rt_sort_of_kind` (`elab_fns.c:46`) maps every non-float kind to
`VS_INT`, so an opaque newtype over `:int` is a first-class refinement subject.
Nothing about the ECS needing real handle types is blocked by the solver.

### 5. A `^borrow` struct parameter's field read stays congruent

```turmeric
(defn cap-borrowed [^borrow w : W] #fx{} : int (.n w))
(defn use-b [^borrow w : W i : #refine{ x : int | (< x (cap-borrowed w)) }] : int i)
```

Proven, identically to the by-value spelling. The guide's "behind a reference it
is declined" rule keys off the receiver's *type kind* (`TY_REF`, `TY_RC`,
`TY_PTR_VOID`), and `^borrow` is a parameter annotation rather than a reference
type. This matters because every ECS accessor takes `^borrow w : World`, and it
means a world whose state lives in **struct fields** is reasonable-about while a
world whose state lives behind a `:int` handle is not.

### 6. A refined index inside a `while` loop is Unknown

```turmeric
(while (< acc n)
  (do (get-at s acc) (set! acc (+ acc 1))))
```

`0 proven, 1 unknown`, exactly as the guide's `[deferred]` limit says. This is
the one that costs real performance rather than real safety: `for-each` lowers
to a `while` over slot indices, and the bounds facts that would let dense
storage drop its per-access check live in the loop condition. Waiting on
[`loop-invariants-plan.md`](../../archive/loop-invariants-plan.md), which was on hold
for want of a demand signal -- **this plan was that signal.**

> **C3 landed (experimental), 2026-09-30.** `(while c :invariant p ...)` ships
> behind `--enable=loop-invariants`.  With `:invariant (>= i 0)` on the loop
> above, a callee demanding `(and (>= j 0) (< j n))` discharges BOTH bounds
> under `--strict-refine`: the upper from the condition, the lower from the
> proved invariant (`tests/fixtures/refine-loop-invariant-re2`).  A macro that
> expands to an annotated `while` gets it with no extra work -- that fixture's
> `for-slots` is the prototype of the `for-each` lowering, which only has to
> emit `:invariant (>= i 0)` in its expansion.  RE2 still does not start
> without a profile; what changed is that the `while` lowering is no longer
> the blocker.

### Not found: a soundness hole

Probes 3--6 were also run adversarially, to see whether a hypothesis could be
staled by an intervening mutation and then used to elide a check. It cannot,
and the reasons are worth recording so nobody re-runs this:

- a `set!` in the caller's body abandons the crossing (verified: Unknown);
- a `^mut` parameter is **by value** in Turmeric -- a callee's `set!` is not
  visible to the caller (verified by print), so a mutating call cannot stale a
  caller's hypothesis about a struct it passed;
- a read of a `^mut` binding classifies `RT_P_UNKNOWN`, and inline C classifies
  impure, so the two remaining mutation channels both decline congruence.

The mutation channel is closed. It is closed *by the same rule* that blocks
probe 3, which is the tension the supplemental plans have to resolve without
opening it.

---

## Corrections owed to the shipped docs

Both statements below are in guides today and both are false against the spice
as it stands. Landed alongside this rewrite.

| Where | Says | Actually |
|---|---|---|
| `ecs-vs-haskell-ecs.md:37` | "`option`-returning reads (`(none)` on dead-handle)" | `defcomponent-accessors` emits `get-<Comp>` returning `~comp-name` **directly**. There is no aliveness check on the read path at all. |
| `ecs-guide.md:299` | "Aliveness: runtime, via generation comparison on every ..." | True for `sized-*` worlds (`sized-alive?`). The unsized `ecs/world` exposes **no aliveness predicate whatsoever** -- only `world-despawn!`, which bumps `gens[idx]`. |

This is a bigger finding than a doc typo. The old plan's pitch was "trade an
`option` unwrap for a type-level proof". The real trade is "**introduce** an
aliveness check where today there is none, and then prove it away". That is a
better story -- it is a correctness win first and a performance win second --
but it means RE1 is not a pure surface addition over the substrate, as the old
plan claimed. It adds an API that does not exist.

---

## Goal

Give `tur-ecs` an opt-in surface where **use-after-despawn is a compile-time
error**, and where the sized worlds' slot-index bounds are discharged statically
rather than re-checked per access. Both are opt-in; the existing accessor family
keeps its shape and its call sites stay correct.

Explicitly **not** a goal: making refinement types the default way to use
`tur-ecs`. The refined accessors stay a surface you opt into by importing
`ecs/refined-world` or calling `sized-defworld-refined`; the forgiving
`get-<Comp>` family keeps its shape either way.

> **Correction 2026-08-01 -- the flag-gating rationale is gone.** This
> paragraph used to read "The experiment expires at `0.34.0` and its graduation
> is a separate decision; an ECS that requires it is an ECS that cannot ship on
> the near side of that decision." That constraint shaped this plan's whole
> sequencing, and it no longer exists: `refined` **graduated 2026-08-01**
> (`bb7cbef61`, shipped v0.33.0). Static `#refine{...}` discharge is
> unconditional, the `EXPERIMENTS[]` and `LANG_LAYERS[]` rows are deleted, and
> a lingering `--enable=refined` / `#lang turmeric refined` is a no-op
> (TUR-W0063 / TUR-W0064) whose shim ages out one minor line later. So the
> refined surface no longer costs its consumers a flag, and "opt-in" here means
> an API choice rather than a compiler gate. See
> [`refined-graduation-plan.md`](../../archive/refined-graduation-plan.md).

---

## Prerequisites

Three compiler gaps and one spice-side prerequisite. The compiler gaps have
their own plans; this section states what ECS needs from each, not how to build
it.

| # | Gap | Needed for | Plan |
|---|---|---|---|
| C1 | Boolean-sorted measures -- `(alive? w e)` usable as a predicate atom | ergonomics of every ECS predicate | [`refine-predicate-measures-plan.md`](../../archive/refine-predicate-measures-plan.md) -- **RM-B1 LANDED 2026-07-26** |
| C2 | A sound route for a measure over mutable world state | RE1 at all | [`refine-stateful-measures-plan.md`](../../archive/refine-stateful-measures-plan.md) -- **LANDED 2026-07-26** (`#reads` + the `frozen` region) |
| C3 | User-written `while` invariants | RE2's bounds elimination | [`loop-invariants-plan.md`](../../archive/loop-invariants-plan.md) -- **LANDED 2026-09-30** behind `--enable=loop-invariants` |

**C1 is not strictly blocking** -- probe 2 shows the `(= (alive-i w x) 1)`
encoding proves today. It is blocking on *whether anyone would write it*. An
ECS whose accessor signatures read
`#refine{ x : Entity | (= (alive-i w x) 1) }` is an ECS nobody adopts.

> **C1 landed (RM-B1), and a purity caveat surfaced for RE1.** A `bool`-returning
> function is now a first-class predicate atom, so `#refine{ x : Entity |
> (alive? w x) }` type-checks and, when `alive?` is pure, discharges through
> an `if`-guard. **But RE0's handle-unwrap helpers are inline-C** (`slot->int`,
> `entity-index`, `entity-generation`, ...), and the purity walk is default-deny
> on inline C -- so any predicate that unpacks a `Slot`/`Entity` (e.g.
> `(in-bounds? n (slot->int x))`) is classified *impure*, gets a fresh symbol
> per occurrence, and does **not** discharge as a congruent measure (verified:
> `0 proven, 1 unknown`). This is the same wall as C2, reached one step earlier:
> for RE1's accessor predicates to be congruent, the predicate must be a pure
> function of values, which today means either (a) predicates that compare
> handles/newtypes without unwrapping through inline C, or (b) making the RE0
> unwrappers pure primitives the purity walk accepts. Fold this into the C2
> design rather than treating C1 as sufficient on its own.

~~**C2 is hard-blocking for RE1.** There is no encoding of a mutable-state
predicate that discharges today, and there should not be one until the design
question is answered.~~

> **Resolved 2026-07-26 -- C2 landed; RE1 is not blocked.** The struck text
> above was true when written and is kept for the record. The design question
> was answered by `#reads` plus the borrow-based `frozen` region: an impure
> measure declares the state it reads, and inside a region holding `(& w)` it
> is congruent, because a mutator declared `^unique ^mut w` cannot be called
> there (`TUR-E0200`). That is in the shipped compiler --
> `enc_reads_arg_frozen` (`src/compiler/refine_collect.c:234`, granted at
> `:470`) and `rt_pred_reads_measure` (`src/compiler/elab_fns.c:794`) -- and
> every acceptance fixture in the C2 plan is marked DONE. RE1 is complete and
> promoted to the real sized-world stack as `ecs/sized-refined`; see the RE1
> phase banners below.

> **Trigger note 2026-08-17 -- the `#reads` trust tier now has its own plan,
> and ONE ECS DECISION IS ITS TRIGGER.**
> [`trusted-refinement-claims-plan.md`](../../archive/trusted-refinement-claims-plan.md)
> covers the promise C2's answer rests on. Two pieces are already live and
> cost the ECS nothing today: `TUR-W0383` warns when a `#reads` measure's
> body *demonstrably* reads a mutable global, and the same evidence refuses
> the congruence grant (gated as `--enable=checked-reads` when this note was
> written; unconditional since it graduated in 0.37.0) -- every ECS measure
> is inline-C, which yields no evidence, so both stay silent for the spice as
> it stands, graduation included.
>
> The piece that waits on this plan is **R4, the general checked `#reads`**:
> it is blocked on the measure layer holding its state in Turmeric-visible
> structs instead of a malloc'd block behind inline C -- which is *exactly*
> option "(b) making the RE0 unwrappers pure primitives" in the C1 purity
> caveat above, reached from the other direction. **If that rewrite is ever
> undertaken** (RE0 unwrappers become pure, the `gens` block becomes a
> struct field or vec), do it with R4 in hand: the same change that makes
> the accessor predicates congruent-as-pure is the one that makes `#reads`
> *checkable*, and the two should land as one design rather than
> rediscover each other. Conversely, a new pure-Turmeric ECS measure that
> reads a `^mut` global will draw `TUR-W0383` immediately -- that is the
> trust boundary being reported, not a lint to silence.

**C3 was blocking for RE2 only** (RE1 never needed it), and it landed
2026-09-30. RE2 is no longer gated on a compiler feature; it is unstarted
by decision -- see the 2026-10-02 probe update in the RE2 phase, which
measures the mechanism working and finds the performance justification
inverted rather than merely unsupported.

### Spice-side prerequisite: real types at the API surface

`ecs/entity` declares `(defopaque Entity :int)` and then types every function
`[e : int] : int`. `defcomponent-accessors` emits `[e : int]`. `sized-alive?`
is `[s : int e : int] : bool`. The world state cell is `:int`.

This is the `:int` stand-in pattern `CLAUDE.md` forbids, and it is not merely
stylistic here -- it is what makes the old plan's negative fixture
unwritable. "Calling `get-Pos-alive` on an un-refined `Entity` fails to
elaborate" has no meaning when `Entity`, slot index, generation counter, state
cell and component id are all the same type. A refinement on `:int` is a
refinement on everything.

Probe 4 establishes there is no solver-side reason to keep the erasure: a
`defopaque` over `:int` refines exactly as well as a bare `:int` does. This is
phase RE0.

---

## Phasing

### RE0 -- Real handle types at the ECS API surface (spice-side, LANDED)

> **Status 2026-07-25 -- landed** on the `turmeric-spices` branch
> `claude/ecs-refinement-re0`. `ecs/entity` now carries `Slot` and
> `Generation` newtypes beside `Entity` (with `slot->int` /
> `generation->int` escape hatches and `slot-new` / `generation-new`
> constructors); `ecs/sized-world` carries a `WorldState` newtype for the
> control block and threads `Entity`/`Slot`/`Generation`/`WorldState`
> through spawn/despawn/alive and the `sized-defworld` `state` field;
> `ecs/world`'s `world-alloc-entity!` / `world-despawn!` are `Entity`-typed.
> The negative fixture is `spices/ecs/tests/errors/slot-not-entity.tur`
> (a `Slot` where an `Entity` is required -> `TUR-E0001`). No regressions
> against the pre-existing v0.31.0 suite baseline (which carries ~23
> unrelated failures from a `(Storage T)` associated-type regression --
> the accessor/for-each validation surface -- so those were validated
> against the passing `sized-world-*` tests instead).
>
> **Two sub-items intentionally deferred** (noted, not done): the
> `defcomponent-accessors` / `sized-defcomponent-accessors` slot parameter
> stays a bare `:int` storage index, and `for-each`'s slot binding stays
> `:int` -- both because lifting them to `Slot` requires lifting the
> int-keyed storage layer (`ecs/storage`, `ecs/sized-storage`) to `Slot`
> too, which is out of RE0's three-module scope, and because every one of
> their consumers is currently in the `(Storage T)`-broken set (so the
> change is neither validatable nor "passes unchanged" today). Fold this
> into the storage-side follow-up.
>
> **Update 2026-07-26 -- the `(Storage T)` skew is FIXED and the deferral's
> stated blocker is gone.** `struct_field_type_from_form` was missing the
> assoc-type-projection dispatch (`(Storage Pos)` -> spurious TUR-E0012) and
> the SZ8 Size-literal placeholder (`(SizedDense (Static 8) Pos)`); both now
> mirror `type_expr_from_form` (fixture: `defstruct-assoc-sized-fields`).
> With the spice tests' legacy by-value box triples also retired
> (`defworld-box-helpers`), the ecs suite is **66/66 green** -- so the
> accessor/for-each consumers are validatable again, and the two deferred
> `Slot`-typing sub-items are unblocked whenever the storage-side follow-up
> is picked up.
>
> **Update 2026-07-26 (later) -- both deferred sub-items DONE; RE0 fully
> complete.** Every public storage index (`dense-*`, `sparse-*`, `tag-*`,
> and the sized trios), the `StorageOps` class methods, the accessor
> emitters' slot parameter, and `for-each`/`sized-for-each`'s binder are
> `Slot`-typed; `defmirror` and `sized-defworld-copy-into`'s generated loops
> follow. Inline-C bodies were unchanged (same int64 carrier) -- the lift is
> signatures + `(slot-new ...)` at the honest int-to-slot boundaries. A raw
> int where a `Slot` is expected is `TUR-E0001`
> (`tests/errors/int-not-slot.tur`); the guide's canonical for-each body
> pattern (binder straight into storage/accessors) is now correct BY TYPE.
> Suite 66/66 before and after (turmeric-spices `830e911`). RE1's refined
> signatures were already `Entity`-typed, so nothing there needed rewriting
> -- the sequencing (types before more refinements) held.

Retire the `:int` stand-ins on the public surface of `ecs/entity`,
`ecs/world`, and `ecs/sized-world`:

- `Entity` (already a `defopaque`) used as the parameter and return type of
  `entity-new` / `entity-index` / `entity-generation` / `entity=?` /
  `world-despawn!` / `sized-spawn!` / `sized-despawn` / `sized-alive?`.
- A `defopaque Slot :int` for the raw slot index, distinct from `Entity` --
  `for-each` currently binds a slot index to a name the user reads as an
  entity, which is a live confusion independent of refinements.
- A `defopaque Generation :int`.
- A `defopaque WorldState :int` for the sized world's control block, or a
  `:ptr<...>` if the struct can be named.

Inline-C bodies keep taking the carrier; only the signatures change. The
existing `unsafe` raw helpers (`__sized-state-*-raw`) stay `:int` internally.

**Why first:** it is the only phase with no compiler prerequisite, it is
independently correct under `CLAUDE.md`, and every later phase writes
refinements *on these types*. Doing it after RE1 means rewriting RE1's
signatures.

**Acceptance:** the existing spice test suite passes unchanged; a new negative
fixture under `spices/ecs/tests/errors/` passes a `Slot` where an `Entity` is
expected and fails to elaborate.

### RE1 -- Strict aliveness (needs C2, wants C1)

> **Status 2026-07-26 -- pattern PROVEN end-to-end; two compiler deps found +
> fixed; harness/module integration remains.** C2 landed (`#reads` + the sound
> `frozen` region), so RE1 is unblocked. The aliveness-refined accessor works
> against the real `ecs/freeze` region: `alive?` reads liveness through an opaque
> handle (a malloc'd `gens` array -- `sized-alive?`'s shape, so genuinely impure
> and `#reads`-carrying), `despawn!` is `^unique ^mut` (locked out in the region,
> `TUR-E0200`), and a guarded `(frozen w (if (alive? w e) (get-x! w e) ...))`
> reports **refine: 1 proven** and runs; the same read with NO region is
> **1 unknown -> TUR-W0372** (so `#reads`+`frozen` is load-bearing). Fixtures:
> `turmeric-spices/spices/ecs/tests/refined/alive-frozen.tur` (positive) and
> `.../tests/errors/refined-alive-no-region.tur` (negative); dogfood write-up
> `docs/upcoming/spices/ecs-re1-refined-aliveness.md`.
>
> Getting here required two compiler fixes (both landed, validated, suite 2367/0):
> (1) a `#reads`-refined param could not codegen -- the impure entry contract was
> `TUR-E0375`; now suppressed (the accessor keeps its own internal check as the
> backstop). (2) the `frozen` *macro* did not compose with guard-discharge --
> macro expansion copies the body, so the crossing path walk missed it under
> pointer identity; fixed with a source-span crossing match (`rt_form_ident`).
>
> **Update 2026-07-26 -- (b) shipped, with a characterized encapsulation limit.**
> The self-contained facade is now a real module, `ecs/refined-world`
> (turmeric-spices, in `build.tur :exports`), and the refined accessor discharges
> **cross-module**: an importer guards in its own `frozen` region and calls the
> module's `rgworld-get-x!` -- **1 proven, runs -> 42**; the same read with no
> region is `TUR-W0372`. `RGWorld` is an opaque affine handle, so a `frozen`
> borrow locks out its `^unique ^mut` mutators (`TUR-E0200`). The soundness
> caveat is real and documented: neither a `defstruct` field nor a `defopaque`
> encapsulates against the `::` coercing cast -- `(:: w :int)` unwraps the handle
> and `(:: int RGWorld)` reconstructs an alias, so a deliberate `::`/inline-C
> bypass can despawn inside the region. That is the same trust boundary `#reads`
> already carries (sound for ordinary code, not adversarial code); a hard
> guarantee needs a language feature (module-private construction / a `::`-sealed
> newtype). Filed as `docs/archive/frozen-region-aliasing-via-coercing-cast.md`.
>
> **Update 2026-07-26 -- (a) tooling built, but auto-running BLOCKED by a
> compiler bug; (c) pattern proven.**
>
> **(a)** `tur test` gained two leading-comment directives --
> `;; tur-test-flags: --strict-refine` (per-test strict compile, so an unproven
> crossing is a hard error, which ENFORCES the proof) and
> `;; tur-test-expect-error: TUR-W0372` (must fail to compile and name the
> diagnostic; run phase skipped). The directive feature works and is general (a
> reusable `tur test` improvement). BUT auto-running the *refined* tests through
> it surfaced a serious pre-existing compiler bug: **compiling multiple refined
> files in one process (`tur test <dir>`, LSP/worker) corrupts memory and
> segfaults nondeterministically** (6-8/8 crashes; ASan-silent -> arena/stack, not
> heap). A partial fix landed (`cmd_build` now calls `refine_discharge_reset()` --
> the memo held stale per-compile-arena VC pointers), but a second channel
> remains. Filed as `docs/archive/history/refined-multi-compile-memory-corruption.md`.
> So the RE1 refined tests are kept in `spices/ecs/tests/refined/` (a subdir
> `tur test tests` does not descend into) and verified **individually** (each
> passes on its own `tur run`/`tur check`), NOT auto-run. Auto-running is blocked
> until the corruption is fixed (then: run each via its own invocation, or move
> them flat).
>
> **Update 2026-07-26 (later) -- corruption FIXED; (a) fully unblocked.** The
> second channel was root-caused via the executed
> `docs/archive/history/arena-debug-poisoning-plan.md` (the AP4 guard mode's clean run
> disproved the UAF theory): `parse_typeclass_method` left the RT1 memo field
> `TypeClassMethod.refine_class_binding` UNINITIALIZED in non-zeroed arena
> memory, so the second in-process compile read recycled-slab junk as a
> `Binding*`. Fixed by zeroing the struct. The `tur test tests/refined` repro
> went 8/8 SIGSEGV -> 0/20 failures, so the refined tests now auto-run via
> `tur test` (moved flat into `spices/ecs/tests/`). Resolved report:
> `docs/archive/history/refined-multi-compile-memory-corruption.md`.
>
> **(c)** The `for-each` aliveness refinement is PROVEN. A refined LOOP whose
> body's `rgworld-get-x!` discharges per-entity works today
> (`tests/refined/refined-loop-alive.tur`: 1 proven, runs -> 40, correctly skips
> a despawned entity). The mechanism: the `while`+`set!` form is blocked -- the
> loop counter's `set!` trips the whole-body `mentions_set` decline in the
> crossing path-cond collector (the C3-adjacent gap) -- so it uses TAIL RECURSION
> (TCO'd; verified 5M calls) + a re-borrow of `w` inside the recursive helper (so
> `alive?` is congruent there) + the `alive?` guard.
>
> **Correction (2026-07-27):** an earlier note here claimed "turmeric has no
> `loop`/`recur` or self-recursive `let`-`fn`, and a macro cannot emit a
> top-level recursive helper." That was WRONG (bad probing -- I used a plain
> `let`, not `letrec`). Verified: **named-let (`(let go [...] ...)`) and `letrec`
> both work** (local recursion exists; a hand-written named-let refined loop
> discharges, `1 proven`), and **a macro CAN emit a top-level `defn`**. Only
> `(loop [...] (recur ...))` is genuinely absent, and named-let covers it. So the
> recursive form does NOT need the order-aware-`set!` fix at all. What actually
> blocks an ergonomic `for-each-alive` MACRO is a different bug: a macro that
> *generates* a refined guard/crossing (via the quasiquote template, not `~@body`
> splicing the user's forms) does not discharge -- spurious `TUR-W0372`. Filed as
> `docs/archive/history/macro-generated-refined-crossings-do-not-discharge.md`.
>
> **Remaining for RE1:** fix the refined-multi-compile corruption (unblocks
> auto-running all refined tests); fix macro-generated refined-crossing discharge
> (unblocks an ergonomic `for-each-alive` macro). The while-based `for-each`
> order-aware-`set!` fix is optional -- the recursive form already works.
>
> **Update 2026-07-26 (later) -- BOTH remaining blockers fixed; RE1 complete.**
> The corruption was an uninitialized `refine_class_binding` memo field (see the
> earlier update); the macro-generated-crossing bug is fixed by recording each
> macro call's expansion (`refine_note_macro_expansion`) and letting the
> crossing path walk traverse INTO expansions -- `rt_form_occurrences` /
> `rt_collect_path_conds` / `rt_form_mentions_set` walk a macro call AS its
> expansion (resolved report:
> `docs/archive/history/macro-generated-refined-crossings-do-not-discharge.md`). The
> set!-scan depth also rose 12 -> 24 (an expansion is legitimately deeper than
> the source spelling it; the old limit's conservative "too deep, assume
> assignment" answer spuriously declined clean for-each expansions). The
> ergonomic **`for-each-alive` macro now proves**: one macro generates the
> recursive loop + frozen re-borrow + aliveness guard, the user's refined read
> is spliced as the body, and the crossing discharges per-entity
> (`tests/fixtures/refine-macrogen-foreach`; the three report shapes + nesting
> in `tests/fixtures/refine-macrogen-crossings`; adversarial negatives in
> `tests/fixtures/errors/refine-macrogen-*`). Shipped to the ecs spice as
> `ecs/refined-world`'s `for-each-alive!`.

> **Update 2026-07-26 -- the promotion is SHIPPED: `ecs/sized-refined`.** The
> accessor family below now exists against the REAL sized-world stack, emitted
> per world/component: `(sized-defworld-refined W)` -> `<W>-alive?` (`#reads`)
> + `<W>-despawn!` (`^unique ^mut`); `(sized-defcomponent-accessor-refined W
> C)` -> the cap-gated `get-<C>!` with the refined entity parameter -- the
> exact signature in the code block below; `(for-each-alive W w n e body)` for
> the per-entity-proven iteration. Getting there took one more compiler fix:
> macro TEMPLATES could not emit `#reads` (fx_prov dropped by both template
> copiers) or substitute into a `#refine{...}` predicate (F_CONTRACT_TYPE
> returned as-is) -- fixed in `elab_macros.c`, pinned by
> `tests/fixtures/refine-template-emitters`. Acceptance: spawn/despawn/read
> proves + runs, no-region is `TUR-W0372`, in-region despawn is `TUR-E0200`,
> for-each proves per-entity (spices `tests/refined-stack-*`, suite 70/70).

Add an opt-in accessor family that will not compile against a handle whose
aliveness has not been established:

```turmeric
;; today's surface -- unchanged, no aliveness check anywhere
(get-Pos read-cap w e)

;; the refined surface
(defn get-Pos! [^borrow cap : (ReadCap Pos)
                ^borrow w   : GameWorld
                e           : #refine{ x : Entity | (alive? w x) }]
             : Pos
  ...)

(if (alive? w e)
  (get-Pos! read-cap w e)     ;; discharged from the guard
  (handle-dead-entity))
```

Three things to decide at elaboration time, listed because they are the
decisions and not the typing:

1. **What `alive?` is a function of.** The honest answer is "the world's
   despawn history", which is not a value. C2's job is to supply a spelling
   that is a function of values -- see that plan for the two candidates (a
   version/epoch argument, and a scope-bounded congruence window backed by the
   linear caps `ecs/cap` already ships).
2. **Whether `for-each` bodies get it.** `for-each` splices its body inline and
   binds a raw slot, never checking generations. Making the loop's binding
   carry an aliveness refinement is the highest-value version of this feature
   and the one most exposed to C3.
3. **Whether the entry check is acceptable.** It always is emitted (guide,
   `[by design]`). For `get-Pos!` that is one integer compare against
   `gens[idx]` -- which is *cheaper* than what the sized worlds do today and
   strictly more than the unsized worlds do today (nothing). The refinement's
   value here is the compile error, not the elision. Measured evidence backs
   this reading: the parent plan's "whole-program entry-check elision:
   measured, declined" section found **zero** runtime and code-size benefit
   from eliding a parameter check.

**Acceptance:**
- `spices/ecs/tests/` fixture: spawn N, despawn half, read `Pos` through
  `get-Pos!` inside an `(if (alive? ...))` guard, `refine: N proven`.
- `spices/ecs/tests/errors/`: the same call *without* the guard fails under
  `--strict-refine`, and warns (`TUR-W0372`) without it.
- A fixture pinning the **negative** direction: a `despawn!` between the guard
  and the call must NOT discharge. This is the fixture that would catch C2
  being implemented as an escape hatch, and it should be written before the
  feature, not after.

### RE2 -- Bounded slot indices on sized worlds (C3 landed; UNSTARTED by decision)

A sized world knows its capacity at the type level. `sized-dense-get` re-checks
`0 <= i < cap` on every access; the check is provable from the loop condition
and the world's `n`.

```turmeric
(defn sized-get-at [^borrow w : (GameWorld n)
                    i : #refine{ x : Slot | (and (>= x 0) (< x n)) }] : Pos
  ...)
```

Probe 6 says this is Unknown inside a `while` today, which is where every real
call site lives. With a written `:invariant` on the `for-each` expansion's loop
it becomes an ordinary path-splitting obligation.

> **Probe update 2026-07-26 -- the RECURSION shape discharges bounds TODAY,
> no C3.** A bounds-refined accessor (`#refine{ x | (and (>= x 0) (< x 8)) }`)
> called from a tail-recursive loop proves under `--strict-refine`: the upper
> bound comes from the loop guard `(< i 8)` as an ordinary path condition, and
> the lower bound rides a refined parameter (`i : #refine{ x | (>= x 0) }`)
> inductively -- the recursive crossing proves `i+1 >= 0` from `i >= 0`, the
> canonical decreasing-argument shape path conditions already handle. Negative
> controls both reject (an off-by-one guard `(< i 9)`; a dropped lower-bound
> refinement). A sized capacity is a type-level constant, so the whole proof
> lives in the PURE fragment -- no `#reads`, no trust. So C3 gates only the
> `while` lowering: RE1 (c)'s `for-each-alive!` pattern (a macro generating
> the named-let loop) carries over directly, and RE2's remaining gate is the
> PROFILE alone. The `#reads`/`#writes` trajectory still matters here for two
> follow-ons: checked write-frames would replace the coarse whole-body `set!`
> decline (unblocking the `while` form), and `frozen` + `#reads` extends
> bounds elimination to RESIZABLE storage, where `(in-bounds? buf i)` reads
> mutable capacity (see stateful-refinements-guide.md "Where this
> generalizes"). The trajectory's trusted-tier half now has its own plan --
> [`trusted-refinement-claims-plan.md`](../../archive/trusted-refinement-claims-plan.md);
> see the trigger note under C2 above before touching how measures hold
> state.

> **Probe update 2026-09-05 -- the probe above measured a CONSTANT bound, and
> the signature this section writes down does not currently parse.** Both
> halves re-measured against `023013c8` (Debug build):
>
> - **Constant capacity proves, confirming the 2026-07-26 result.**
>   `#refine{ x : int | (and (>= x 0) (< x 8)) }` called from a tail-recursive
>   loop guarded by `(< i 8)` passes `tur check --strict-refine` clean, and the
>   off-by-one negative control (`(< i 9)`) rejects with `TUR-E0371` plus the
>   note "the predicate (and (>= x 0) (< x 8)) does not hold for every input
>   here". So the proof is real, not vacuous.
> - **Parametric in the sized index does not.** RE2's own signature above spells
>   the bound as `(< x n)`, where `n` is the world's type-level size index. That
>   is not a value in refinement scope:
>
>   ```
>   probe.tur:13:61: error: unbound symbol 'n'
>      13 |    i : #refine{ x : int | (and (>= x 0) (< x n)) }] : int
>   probe.tur:13:61: help: Did you mean 'b'?
>   ```
>
>   Reproduced on a `defopaque SBuf [n]` carrier with the index pinned by the
>   callee's return type (`(defn mk8 [] : (SBuf (Static 8)) ...)`), so the size
>   is as statically known as it ever gets. A refinement predicate ranges over
>   runtime values in scope; a size index is a type-level name lowered to a
>   `TY_INT` placeholder. There is no bridge between the two scopes.
>
> **What this changes.** Nothing about RE2's sequencing -- the profile note
> below still governs whether it starts at all. It does mean the phase is not
> "discharge works, wire it up": a capacity that is a *literal at the accessor's
> definition site* works today, but the parametric `(GameWorld n)` shape RE2 is
> actually for needs type-level size indices visible to the refinement
> elaborator, which is unbuilt and unscoped. Cost that in before quoting the
> 2026-07-26 probe as evidence the mechanism is ready. The narrower move, if
> RE2 is built for its correctness payoff, is a monomorphic accessor per
> capacity (bound as a literal), which needs no compiler work at all.
>
> One prerequisite for the parametric shape did land the same day: a
> declared or ascribed size index is now reconciled with the value it
> describes (`docs/archive/declared-size-index-never-checked-against-value.md`),
> so if size indices ever become visible to the refinement elaborator, a
> proof of `i < n` is a proof against a checked claim rather than a bare
> annotation. The bridge itself is still unbuilt.

Deliberately sequenced last: it is the only phase whose payoff is measured in
nanoseconds, and the parent plan's benchmarking section is a standing warning
that this class of win tends to evaporate under `cc -O2`, which already proves
locally-derived bounds for free. **RE2 does not start without a profile.**

> **THE PROFILE EXISTS AS OF 2026-08-20, and it reframes RE2 rather than
> greenlighting it.** `turmeric-spices/spices/ecs/bench/` -- 100k entities x
> 100 frames of dense float `Pos`/`Vel` integration, against a hand-rolled C
> baseline. Best of 5 on an M2, `tur` v0.37.0:
>
> | variant | ms | vs C |
> |---|---|---|
> | `c-baseline` (flat arrays, hand-rolled C) | 4.4 | 1.00x |
> | `manual` (raw Turmeric, flat buffers, no ECS) | 4.6 | 1.04x |
> | `ecs-unsized` (`defworld` + `for-each2`) | 36.9 | 8.42x |
> | `ecs-sized` (`sized-defworld` + `sized-for-each`) | 15.3 | 3.50x |
>
> Three findings bear on RE2, in decreasing order of comfort:
>
> 1. **The archived parent plan's within-2x target is missed by a wide
>    margin** -- 8.42x unsized, 3.50x sized. This is the first measurement
>    and it is worse than the plan assumed. Worth recording plainly.
> 2. **The cost is not where RE2 is aiming.** It is not codegen (`manual`
>    runs 1.04x C) and not the query macro (a hand-written loop with no
>    `for-each` anywhere ties it, despite `for-each2` doing strictly more work
>    per slot). Reading `ecs/storage.tur`, the unsized `dense-set!`
>    emits per write: an `elem_sz` init test, a capacity test guarding a
>    `realloc` auto-grow path, the store, a second array write to
>    `present[idx]`, and a `len` update branch. RE2 refines a slot index to
>    discharge `0 <= i < cap`; that maps onto the **auto-grow guard**, not a
>    pure bounds check, and removing it leaves the `present[]` write and the
>    `len` update untouched.
> 3. **`ecs-sized` already banks most of the available win, with no
>    refinement types at all.** A sized world's capacity is static, so its
>    grow branch is already dead -- and it still runs 3.50x. So the
>    36.9 -> 15.3 part of the gap is had today, and the remaining
>    15.3 -> 4.4 is not something a refined index addresses.
>
> **What this changes.** RE2's *performance* justification does not survive
> this profile: the cheaper first move, if speed is the goal, is a
> `dense-reserve!` / non-growing write path on the unsized storage, which is
> ordinary spice work and needs no compiler feature. RE2's *correctness*
> justification -- compile-time rejection of an out-of-range slot -- is
> untouched and stands on its own; it just should not be sold on this
> benchmark. The probe update above (bounds discharge works today in the
> recursion shape, no C3 needed) still holds, so if RE2 is built for the
> type-level property it remains cheap to build.
>
> Not separately measured: the per-cost attribution *inside* `dense-set!`.
> An ablation that removes the grow branch and the `present[]` write
> independently would settle how much of the 36.9 -> 15.3 each accounts for,
> and is the obvious next probe. (`ecs-sized` still does a `present[]` write
> and a struct copy per store, so its 3.50x is not the grow branch alone.)
> See `spices/ecs/bench/README.md`.

> **Probe update 2026-10-02 -- C3 LANDED, the mechanism works, and the
> performance case is INVERTED rather than merely unsupported. RE2 stays
> unstarted.** C3 (`loop-invariants`) landed 2026-09-30 behind
> `--enable=loop-invariants`, so probe 6 was re-run for the first time since.
> All measurements against a v0.59.0 Debug build.
>
> **1. The `while` shape now discharges, and the invariant is load-bearing.**
> Probe 6 measured `0 proven, 1 unknown`. With a written `:invariant` it is an
> ordinary path-splitting obligation, exactly as this section predicted. Four
> variants under `--strict-refine`:
>
> | variant | obligations | result |
> |---|---|---|
> | gate on, guard `(< i cap)`, `:invariant (>= i 0)` | 3 | **3 proven, 0 refuted** |
> | gate **off** (probe 6's own condition) | 1 | refuted |
> | `:invariant` removed, gate on | 1 | refuted |
> | off-by-one guard `(< i (+ cap 1))` | 3 | 2 proven, **1 refuted** |
>
> The lower bound rides the invariant and the upper bound rides the loop guard;
> removing either refutes, and an off-by-one guard refutes, so the proof is not
> vacuous and C3 is precisely what unblocks it.
>
> **2. The bound does not need to be a type-level index -- a preceding VALUE
> parameter works.** The 2026-09-05 probe above is re-confirmed at 0.59.0:
> `(< x n)` over a size index is still `unbound symbol 'n'` (identical error and
> identical "Did you mean 'b'?" help), and the type-level -> refinement-scope
> bridge is still unbuilt. But it is not needed. A refinement may reference a
> parameter declared before it:
>
> ```turmeric
> (defn get-at [cap : int
>               i : #refine{ x : int | (and (>= x 0) (< x cap)) }] : int ...)
> ```
>
> and that is already the shape `sized-for-each` expands to -- `ecs/sized-query`
> binds `__cap` to `(sized-dense-cap ...)` in a `let` *before* the `while`, so
> the capacity is a value in scope at every call site in the body. So RE2 needs
> neither the unbuilt bridge nor the "monomorphic accessor per capacity"
> fallback proposed above. That part of the 2026-09-05 note is superseded: the
> parametric shape is reachable today, just parametric in a value rather than in
> a type index.
>
> **3. The decisive new finding: proving the call site removes NO runtime
> check.** This is what changes the phase's standing, and the 2026-08-20
> profile did not reach it.
>
> | program | contract checks in emitted C |
> |---|---|
> | refined accessor, call site fully proved | **6** |
> | identical code, plain `i : int` | **5** |
>
> and the emitted C is **byte-identical with the gate on and off**. The reason
> is structural: the bound is checked in the **callee's prologue**, not at the
> call site -- in the probe, inside `get_hyat` itself. A caller-side proof
> cannot elide a check the callee emits on behalf of every possible caller,
> and an exported spice accessor has callers that do not exist yet. So adding
> the refinement *costs* one check per access and the proof buys nothing back.
>
> **This is documented design, not a defect**, which is worth stating plainly
> since it sets the ceiling on RE2 rather than being something to fix on the
> way. [refinement-types-guide.md](../../guides/refinement-types-guide.md)
> divides a function's refinements into two roles, and only one of them elides:
>
> - a **goal** -- a return refinement or `:post` -- "when proved, **no runtime
>   check is emitted for it**";
> - a **hypothesis** -- a parameter's refinement or `:pre` -- which the body may
>   assume, and which "is still checked at runtime, since it constrains the
>   caller rather than the body".
>
> A parameter refinement is in the second role, so a crossing proof buys
> *safety* (a provably out-of-range call is `TUR-E0371`, a compile failure
> rather than a runtime panic) and never *speed*. RE2's payoff is entirely in
> the first of those. The third mode -- caller owes the proof AND the callee
> emits no check -- is the one that does not exist, and is exactly what the
> unchecked-variant sketch below is about.
>
> **What this changes.** The 2026-08-20 profile concluded RE2's performance
> justification does not survive because the cost is the auto-grow guard and
> the `present[]` write rather than a bounds check. Finding 3 is stronger and
> more structural: **the bounds check RE2 targets is not eliminable by proving
> it at all** under the current scheme. RE2's *correctness* justification --
> compile-time rejection of an out-of-range slot -- is untouched, now verified
> to work, and cheap to build; it is the only justification left, and it comes
> at +1 runtime check per access rather than -1.
>
> **Decision 2026-10-02: record and leave unstarted.** Nothing here is a
> blocker; the mechanism is ready. What is absent is a reason to spend the
> coupling: building RE2 would make `tur-ecs` depend on `--enable=loop-invariants`,
> a `prototype` row whose surface is expected to move, in exchange for a
> compile-time guarantee that costs a runtime check. Revisit if the
> out-of-range-slot guarantee is wanted for its own sake, or if an unchecked
> accessor variant (below) makes the proof pay.
>
> **Consequence for C3's own graduation.** `loop-invariants`' trigger 1 is
> "RE2's profile shows the per-access bounds re-check is a real cost and the
> `while` lowering is where the call sites live." The profile shows it is not a
> real cost, and finding 3 shows proving it changes no code. **RE2 will not be
> the consumer that fires that trigger**, even if built -- it would consume the
> correctness path only. The loop-invariants row should not be held open in
> expectation of RE2.
>
> **The unchecked variant: option (c) is the direction.** To make a
> caller-side proof buy anything at runtime, the callee needs an entry point
> whose precondition is *assumed* rather than checked -- the third mode the
> guide's goal/hypothesis split does not currently have. Three shapes were
> considered; **(c) is the one to pursue**, and (a) and (b) are recorded as
> rejected so they are not re-proposed.
>
> - **(a) Two functions, one `#fx{Unsafe}` -- rejected as a destination.**
>   Ship `sized-dense-get` (checked) and `sized-dense-get-unchecked`, and have
>   the macro expand to the latter. Zero compiler work, and `#fx{Unsafe}` is
>   genuinely enforced -- only `(unsafe ...)` or an already-`Unsafe` caller
>   discharges it -- so every such call site is marked and the effect system
>   can enumerate them. What it does not do is connect the proof to the call:
>   the macro asserts the bound and nothing verifies the assertion. Fine as a
>   stopgap while a macro is the sole caller; not a place to stop, because the
>   guarantee degrades silently the moment anyone calls the unchecked form by
>   hand.
> - **(b) Elide the callee check when every caller is visible -- rejected as
>   insufficient.** Sound and needs no new syntax, but it dies exactly where
>   this is needed: an exported spice accessor's callers are not all visible,
>   and an indirect call never is. Worth having for its own sake someday;
>   useless for this.
> - **(c) Make the proof the authorization -- the direction.** An entry point
>   declaring a precondition that is **never** checked at runtime and that
>   every call site must discharge statically or fail to compile. Most of the
>   machinery exists: the crossing obligation, the solver, `TUR-E0371`, and
>   `--strict-refine`'s promotion. What is new is a declaration meaning
>   "refuse to compile rather than fall back to a runtime check".
>
> **What (c) has to answer.** Written down now so the first attempt does not
> rediscover them:
>
> 1. **Always-strict, by construction.** The whole point is that there is no
>    runtime fallback, so this flavour cannot soften under a non-strict build
>    the way `TUR-W0372` does. An unproved call site is an error at every
>    strictness level -- which means the declaration is also a promise to the
>    *caller* that its build will fail, not merely warn.
> 2. **Virality is the real cost.** The assumed-ness must reach a call site
>    that can actually prove the bound. An intermediate wrapper
>    (`(defn helper [cap : int i : int] (get-assumed cap i))`) cannot discharge
>    it and must declare the precondition itself -- at which point, under
>    today's rules, the wrapper emits its own runtime check and the cost moves
>    rather than disappears. Every wrapper in the chain has to be in the
>    assumed mode. This is the `#reads` propagation problem in a new coat.
>    *Mitigating observation:* the win concentrates in macro expansion, where
>    the loop and the access are generated together at the proving site with no
>    wrapper in between -- which is exactly the ECS `for-each` shape. So (c) is
>    cheap in the macro case and expensive in the general library case, and a
>    first cut could legitimately support only the former.
> 3. **It moves refinement proofs into the memory-safety TCB.** This is the
>    heaviest consideration and the reason (c) should not be built casually.
>    Today a solver bug in a *goal* elision means a missing check on a value
>    the body computed -- bad, contained. A solver bug in an *assumed
>    precondition* is an unchecked out-of-range index into a buffer: memory
>    unsafety produced by a prover mistake. Both historical refinement
>    soundness bugs lived in the encoder, and as of 2026-10-02 the encoder's
>    newest work (`^reflect`'s RF3/RF4) had no differential fuzz coverage at
>    all -- see
>    [reflect-fuzz-never-reaches-the-rf3-rf4-encoder](../../archive/reflect-fuzz-never-reaches-the-rf3-rf4-encoder.md).
>    (Closed 2026-10-03 by `shape_reflect`, whose first run found a third
>    encoder soundness bug -- a float field's selector declared Int -- fixed
>    the same day.)
>    **Closing that gap is a prerequisite, not a nicety**, if proofs are going
>    to be load-bearing for memory safety. A bisection hatch
>    (`TUR_<NAME>=0` re-enabling every suppressed check) and a harness kept on
>    the off path are the other half -- per the INVERTS-not-retires rule in
>    [experimental-flags-guide](../../guides/experimental-flags-guide.md).
> 4. **Keep the checked entry point.** A caller that cannot prove the bound
>    needs somewhere to go that is not `unsafe`. Two entry points over one body
>    is the shape, which makes (a) the fallback *inside* (c) rather than a
>    rival to it.
> 5. **A candidate framing worth trying first:** make *defining* such an entry
>    point `#fx{Unsafe}` while *calling* it is safe exactly when the obligation
>    discharges. That puts the feature inside machinery that already exists,
>    makes the audit surface enumerable by the effect system, and states the
>    asymmetry honestly -- the body does unchecked indexing; the call site has
>    earned it. It also means a call site that cannot prove the bound falls
>    back to the ordinary `Unsafe` discharge rules rather than to silence.
> 6. **Where it lives.** Not in this plan. It is a compiler feature, so per
>    [CLAUDE.md](../../../CLAUDE.md) it needs its own `docs/upcoming/` plan and
>    an `EXPERIMENTS[]` row behind `--enable=`, and it belongs beside
>    [`trusted-refinement-claims-plan.md`](../../archive/trusted-refinement-claims-plan.md),
>    which already owns "what promise does a refinement rest on, and is the
>    promise checkable" -- an assumed precondition is a new entry in that
>    taxonomy rather than a variation on `#reads`.
>
> **Sequencing, stated plainly:** (c) only pays off where the eliminated check
> is a measurable cost, and per the 2026-08-20 profile the ECS bounds check is
> not one. **So (c) should not be motivated by RE2.** It wants a case where the
> per-access check demonstrably dominates -- a tight numeric kernel over a
> refined index is the likely candidate -- plus prerequisite 3 above. RE2
> remains a consumer that would benefit, not the reason to build it.

### RE3 -- Documentation (DONE 2026-07-26)

- Fix the two false statements in the table above. *(Landed with the plan
  rewrite.)*
- `ecs-guide.md`: replace the "gated on the refinement-types work" pointer with
  what actually ships. *(Done: the Entities section and the runtime-checks
  bullet now describe `ecs/refined-world` + `for-each-alive!` under
  `--enable=refined`, with the facade-vs-full-stack scope note.)*
- `ecs-vs-haskell-ecs.md`: the aliveness row gains a compile-time entry
  alongside the runtime default; the polymorphism row is **not** touched (see
  below). *(Done: row, "still runtime-checked" section -- the "open design
  question" text replaced with the shipped `#reads` + `frozen` answer and its
  trust-boundary caveat -- and the honest-scorecard bullet.)*
- Also landed in the same pass: `stateful-refinements-guide.md` gained the
  "Macros compose" section (splicing vs generating macros both discharge;
  `for-each-alive!` as the shipped consumer), and
  `refinement-types-guide.md`'s pointer to it no longer says "in-flight".

---

## Dropped from the previous revision

### `/has` world bounds are not refinement types

The old RE1 proposed `(refine W (/has Pos /has Vel))` as a third polymorphism
encoding. This should not be built as written, for a reason more basic than
"the elaborator does not support it":

A refinement type constrains a **value**. `/has Pos` constrains a **type** --
it asks whether the type `W` has a field named `Pos`. Those are different
judgements over different objects, and the shipped refinement machinery has no
representation for the second: the guide lists "no refinements on type
parameters" as a `[prototype]` limit, and the VC term language has integers,
reals, booleans and uninterpreted functions -- no types.

Encoding a structural type predicate through the SMT layer would mean teaching
the solver about the type system, which is a great deal of machinery to
reimplement a question `defworld` can answer by field lookup at elaboration
time. The right home for it is row/presence constraints, re-filed as
[`ecs-component-set-bounds-plan.md`](../../archive/ecs-component-set-bounds-plan.md).

Meanwhile the `(HasPos W)` typeclass encoding **ships and works**, so nothing
is lost by the deferral except one dictionary indirection per polymorphic call.

### The `-alive` accessor family and `entity-alive!`

Superseded by probe 2. Crossing path-condition recovery gives the promotion for
free from an ordinary `if`; a bespoke promotion form would be a second, weaker
mechanism for something the general one already does. RE1 keeps only the
refined *accessors*, not the promotion form.

### Sized `-alive` analogues

Same reasoning; the sized accessors take the same refinement on their `Entity`
parameter.

---

## Relationship to the graduation decision

[`refined-dogfooding-plan.md`](../../archive/refined-dogfooding-plan.md) is on hold
"waiting on a program to exist, not on effort", and feeds graduation
precondition 2 (cost on something that is not a fixture).

**`tur-ecs` is that program**, and the fit is close enough to be worth stating
against the dogfooding plan's own tier list:

| Dogfooding item | What ECS supplies |
|---|---|
| Calls guarded by `if` / `let` / `match` | RE1 is nothing but this, at every accessor call site |
| A typeclass with a refined method parameter | `StorageOps` / `Component`, whose methods take the slot index |
| `match` on an ADT with refined arms | `sized-spawn`'s `(Result Entity WorldFull)` |
| Recursion, incl. a mutually-recursive pair | the `for-each` / `defworld` macro expansions are recursive at expansion time, not runtime -- **ECS does not supply this** |
| Floats with non-zero fractional parts | `ecs-raylib`'s `Pos`/`Vel` are float components |
| Compile-time cost on a real program | `spices/ecs` is ~5400 lines across 22 modules |

So the ordering that falls out is: **RE0 now** (no prerequisite), then C1
(small, verified, unblocks readable signatures), then C2 (the real design
work), then RE1 as the dogfooding vehicle, then C3/RE2 only against a profile.

---

## Out of scope

- Anything not gated on refinement types -- shipped in the archived parent plan.
- Cross-world systems / `World-Mirror` -- `xworld.tur` / `xstage.tur`, tracked
  separately.
- Routing `defcomponent-accessors` through `StorageOps` -- shipped (E2d-P6).
- Refinements in function types (`TUR-E0378`). `for-each` splices inline and
  `defsystem` resolves statically, so the ECS does not need them; a stage
  scheduler holding system *values* would, and that is a reason not to build
  the scheduler that way.
- Parameterized refinement aliases (`(deftype (Alive w) ...)`). Would let RE1
  write `[e : (Alive w)]` instead of inlining the predicate at every accessor.
  Real ergonomics, no semantics -- worth doing if C1 lands and the signatures
  are still unpleasant, not worth its own plan before then.

## References

- Parent plan (archived): [`ecs-spice-plan`](../../archive/ecs-spice-plan.md)
- [`refinement-types-plan.md`](../../archive/refinement-types-plan.md) -- what landed
- [`refinement-types-guide.md`](../../guides/refinement-types-guide.md) -- the surface
- [`refined-graduation-plan.md`](../../archive/refined-graduation-plan.md)
- [`refined-dogfooding-plan.md`](../../archive/refined-dogfooding-plan.md)
- [`loop-invariants-plan.md`](../../archive/loop-invariants-plan.md)
- `docs/guides/ecs-guide.md`, `docs/guides/ecs-vs-haskell-ecs.md`,
  `docs/guides/ecs-storage-guide.md`
- `docs/guides/substructural-types-guide.md` -- the linear caps C2 leans on
