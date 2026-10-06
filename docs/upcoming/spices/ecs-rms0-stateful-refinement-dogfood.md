# RM-S0 -- Stateful-refinement dogfooding in `tur-ecs`

**Date:** 2026-07-26
**Phase:** RM-S0 of
[`refine-stateful-measures-plan.md`](../v1/refine-stateful-measures-plan.md)
(gap C2 of the ECS refinement plan).
**Purpose:** RM-S0 is explicitly *not* an implementation phase. It writes the
RE1 accessor call sites twice -- once with **epoch arguments** (Candidate A),
once inside a hypothetical **frozen region** (Candidate B) -- runs the epoch
version, and reads both to answer one question: *is the annotation burden
tolerable, and does the epoch surface read acceptably?* That answer decides
A-with-a-lint vs. building B.

This is written against `tur` at v0.31.0 **after C1/RM-B1 landed** (boolean-sorted
measures), which materially changes what the epoch surface can discharge -- so
the plan's "the epoch version partly runs today" is now testable rather than
hypothetical.

---

## Candidate A -- Epoch arguments (runs today, with a catch)

The measure is made a function of a monotonically-increasing **epoch** that
every mutation bumps. Two occurrences of `(alive-at? (world-epoch w) e)` are
congruent exactly when the epoch has not moved.

### A.1 -- Epoch as a pure struct field: **discharges**

```turmeric
#lang turmeric refined
(import ecs/entity :refer [Entity entity-new slot-new generation-new])

(defstruct GameWorld [epoch : int])            ;; epoch is a PLAIN int field
(defn world-epoch [^borrow w : GameWorld] #fx{} : int (.epoch w))
(defn alive-at?   [ep : int e : Entity]     #fx{} : bool (> ep 0))

(defn get-Pos! [^borrow w : GameWorld
                e : #refine{ x : Entity | (alive-at? (world-epoch w) x) }] : int
  0)

(defn case-clean [^borrow w : GameWorld e : Entity] : int
  (if (alive-at? (world-epoch w) e)
    (get-Pos! w e)            ;; guard establishes alive-at?(epoch,e); read discharges
    -1))
```

`TUR_REFINE_STATS=1 tur --enable=refined run ...` reports
**`1 obligation(s): 1 proven, 0 refuted, 0 unknown`.**

Why it works now: `world-epoch` is a `^borrow` field read, which the purity
walk accepts (ECS plan probe 5 -- a `^borrow` field read is congruent), and C1
lets `alive-at?` be a `bool`-returning predicate atom. Before C1 this same
program was `0 proven, 1 unknown` regardless.

### A.2 -- Direct `set!` between guard and read: **soundly invalidates**

```turmeric
(defn case-setbang [e : Entity] : int
  (let [^mut w (GameWorld 1)]
    (if (alive-at? (world-epoch w) e)
      (do (set! w (GameWorld 2))       ;; mutation the caller can see
          (get-Pos! w e))
      -1)))
```

Reports **`0 proven, 1 unknown`.** A `set!` in the caller's body abandons the
crossing, so a *visible* mutation correctly kills the proof. This is the one
sound behavior the epoch surface gets for free.

### A.3 -- Epoch behind the real `tur-ecs` handle: **hits the wall**

The catch. In `tur-ecs` the world's mutable state is a `malloc`'d control block
behind an `:int` (now `WorldState`) handle -- fields are handles into *shared*
heap, and `sized-defsystem-scheduled` documents that "mutations applied through
cap-gated `set-<Comp>!` flow through to every other loaded copy of the same
boxed world." So the epoch cannot be a by-value struct field that tracks
mutations; it must be read out of the shared block:

```turmeric
(defn world-epoch [state : int] #fx{} : int
  ```c
  return (int64_t)state;   /* read the epoch out of the shared control block */
  ```)
```

Reports **`0 proven, 1 unknown`.** Reading through inline C is impure, so the
measure gets a fresh symbol per occurrence and never discharges. This is
Candidate A's "bad point 2" in the plan, now empirically confirmed.

### What A.1 vs A.3 means

The epoch surface discharges **only** when the world's state is a by-value
struct field, and `tur-ecs`'s state is shared heap behind a handle. Bridging
that gap is not a call-site annotation -- it is moving `sized-world.tur`'s
control block into real struct fields, *and* giving up the shared-world
semantics that let a system's writes flow across loaded world copies. That is
the opposite of how the sized scheduler is built.

And even with the state restructured, **nothing enforces the bump** (bad point
1): a mutator that forgets to increment `epoch` makes the compiler prove a dead
entity alive, with no declaration to point at. The runtime cost of a wrong
epoch discipline is an *elided* aliveness check -- a use-after-despawn read.

---

## Candidate B -- Scoped congruence window (a sketch that does not compile)

Do not make the state a value; make the *absence of mutation* checkable, and
let the measure be congruent inside a region where mutation is impossible.
`tur-ecs` already ships the mechanism (`ecs/cap`'s linear `WriteCap`/`ReadCap`,
and `defsystem`'s linearity-gated writes); a despawn capability is the same
shape.

```turmeric
;; SKETCH -- with-frozen-entities and the #despawns row do not exist yet.
(defn world-despawn! [^borrow w : GameWorld e : Entity]
                     #despawns w                 ;; declared mutation row
                     : bool ...)

(with-frozen-entities w [tok]            ;; borrows w's despawn capability
  (if (alive? w e)
    (get-Pos! cap w e)                   ;; congruent HERE: no despawn is callable
    (handle-dead)))
;; outside the region, (world-despawn! w e) is callable again and alive? is not
;; congruent.
```

Inside the region the despawn capability is borrowed away, so `world-despawn!`
(which `#despawns w`) is not callable, so `alive?` genuinely *is* a function of
its arguments there -- no epoch, no bump to forget. The enforcement is the type
system's, not a library's.

What B costs (all *work*, not *risk*):

1. A **declared** relation between a function and the state it mutates (a
   `#despawns`/`#writes` row -- a real language addition, and per the parent
   plan it must be *declared and checked*, never inferred from `set!`/inline-C
   the way `#fx{}` is not).
2. A **region form** whose entry borrows the capability and whose body is a
   congruence window.
3. **Hypothesis invalidation at region boundaries** -- a third invalidation
   site alongside `set!` and the `do`-split rule (the plan asks these three be
   one shared predicate).

B's failure mode is "the region is smaller than it could be" -> loses proofs,
keeps checks -> **sound**. A's failure mode is "a check was elided that should
not have been" -> **unsound**.

---

## Reading the two surfaces -- the RM-S0 judgment

The plan's rule: *if the epoch version reads acceptably, B is hard to justify
and the answer is A-with-a-lint; if not, B is the only one worth building.*

**The epoch version does not read acceptably for `tur-ecs`** -- and not because
of call-site verbosity (which is real but survivable). It fails on two
structural points that a lint cannot fix:

- It **does not discharge against the shipped world** (A.3): the state lives in
  shared heap behind a handle, so `world-epoch` is inline C and impure. Making
  it discharge means restructuring `sized-world.tur` into by-value struct-field
  state and abandoning the shared-world write-propagation the scheduler relies
  on.
- Its soundness rests on an **unenforced** discipline (bad point 1), whose
  failure is an elided use-after-despawn check -- the exact miscompile class
  this feature has fixed three times.

So RM-S0's evidence points to **Candidate B**, consistent with the plan's own
"B is the right shape." The honest counterweight is size: B is a genuine
language addition (mutation rows + region form + a third invalidation site),
whereas A-with-a-lint is small. But A-with-a-lint does not actually buy a
working ECS aliveness proof here -- it buys a verbose, unenforced surface that
still does not discharge against the real world. That is not a cheaper win; it
is not a win.

### Recommendation

1. **Do not ship Candidate A for `tur-ecs`.** It neither discharges against the
   shipped world nor enforces its own premise.
2. **Treat Candidate B as the target**, but stage it so each piece is
   independently useful and the risky congruence-window step comes last and
   behind an experiment flag:
   - **B1:** declared mutation rows (`#despawns`/`#writes <thing>`), *checked*
     (a call to a mutator inside a region that froze that thing is a compile
     error) but with **no** refinement interaction yet. This is a
     substructural-types feature that stands on its own and is testable without
     touching the VC.
   - **B2:** the region form + capability borrow, still no refinement
     interaction.
   - **B3:** the congruence-window link -- inside a region that froze `w`, a
     measure over `w` is congruent -- as the single new hypothesis-invalidation
     site, gated behind `--enable=refined` and fuzzed with the `stateful`
     sabotage the plan's acceptance requires.
3. **Write `errors/refine-stateful-mutation-invalidates` first**, before any B
   code -- the plan is explicit that it is the fixture that catches B being
   implemented as an escape hatch.

This is a large, multi-slice compiler project, not a spice change. RM-S0's
output is this recommendation; RM-S1/RM-S2 do not start without a decision on
whether to commit to B's size.

---

## Reproduction

Each snippet above runs from inside `spices/ecs/` (so `ecs/entity` resolves):

```sh
TUR=/path/to/turmeric/build/tur
TUR_REFINE_STATS=1 "$TUR" --enable=refined run <file>.tur
```

Observed: A.1 -> `1 proven`; A.2 -> `1 unknown`; A.3 -> `1 unknown`.

---

## Update -- B1 landed (Candidate B, first slice)

Decision: build Candidate B. **B1 -- the declared, checked mutation gate --
ships as `ecs/freeze`, and needs no new compiler feature.** Probing the shipped
substructural machinery showed it already expresses the frozen-region property:

- `DespawnCap<W>` is a `:linear` capability (same shape as `ecs/cap`'s
  `WriteCap`). A despawn is gated on consuming one.
- `with-frozen` borrows the cap for a region body. While borrowed the cap is
  not consumable, so a cap-gated despawn inside the region **fails to
  elaborate** -- `TUR-E0101: linear value used after being consumed`. A compile
  error, not a runtime check, and it is the type system's, not a library's.

Tests: `tests/freeze-region.tur` (positive: freeze, read, despawn allowed
after) and `tests/errors/freeze-despawn-in-region.tur` (negative: despawn in
region rejected). This is the plan's RM-S2 item 1 realized as a linear cap
rather than an effect row -- lower-risk, and it hands B3 a concrete type-level
fact (cap frozen here) to key congruence off.

Two caveats carried into B2:

1. **Non-forgeability is a usage discipline at B1.** The freeze is only as tight
   as the cap is scarce: mint `DespawnCap<W>` once at world construction and
   thread it. B2's region *form* makes this structural (its entry owns the
   world's unique cap).
2. **The ergonomic region HOF hit a codegen bug.** The natural polymorphic
   `with-frozen [W R] ... body : (fn [] R) : R` SIGBUSes when the region body is
   a *capturing* closure (the common case), because a HOF with a **type-variable
   result** miscompiles captured closures -- filed at
   `docs/reported/poly-result-hof-capturing-closure-sigbus.md` (now archived at
   `docs/archive/poly-result-hof-capturing-closure-sigbus.md`). Worked
   around by fixing the body result to `int`. B2 should make the region a
   first-class language form (not a polymorphic HOF), or that bug must be fixed
   first.

## Update -- B2 landed (the region form)

`ecs/freeze` now exports a `with-frozen` **macro** -- the region *form*:

```turmeric
(let [sum (with-frozen dc
            (let [x (get-a w) y (get-b w)]
              (+ x y)))]          ;; despawn in here => compile error
  ...)                            ;; dc is usable again: with-frozen borrows,
                                  ;; it does not consume
```

It is sugar over the `with-frozen-fn` borrow combinator (wrapping `body...` in
the `int`-returning thunk it wants), so the call site reads as a region rather
than a higher-order call. Entry borrows the cap -- not consumes -- so the cap
survives the region for a later real despawn; `tests/freeze-region-reuse.tur`
reads across a multi-statement region, returns a computed int, and despawns
after.

Why a macro and not yet a first-class special form: the `^borrow` HOF is the
right mechanism (a *consume* cannot restore the linear cap -- `set!` on a
consumed linear binding is itself `TUR-E0101`), but its polymorphic form hits
the codegen crash above, so the region result is pinned to `int` (end a
side-effect region with a trailing `0`). A `(frozen w ...)` special form -- which
B3 could key congruence off directly -- is the eventual shape, deferred behind
the codegen fix or B3's elaborator work rather than built twice. B3 will
recognize the region at its `^borrow (DespawnCap W)` entry.

## Update -- the sound `frozen` region (cap retired)

The cap-uniqueness investigation (see the turmeric plan) established that the
`DespawnCap` freeze has an **unclosable aliasing hole**: a fresh cap can be
minted inside a region and used to despawn. The sound replacement needs **no
capability and no compiler change**:

```turmeric
(frozen w                     ;; (let [_ (& w)] ...) -- immutable borrow of w
  (let [x (get-a w) y (get-b w)]
    (+ x y)))                 ;; despawn! w here => TUR-E0200
```

`(frozen w body...)` (an `ecs/freeze` macro) lowers to `(let [_ (& w)]
body...)`. It holds an immutable borrow of the **owned** world `w` across the
body, so a despawn declared `[^unique ^mut w : World ...]` is statically
rejected inside (`TUR-E0200: cannot pass 'w' as ^unique ^mut -- active borrow
exists`). This is **sound**: there is no cap to mint or alias, and exclusive
mutable access cannot be forged from a shared borrow. Read-only accessors
(`[^borrow w]`) coexist with the region borrow and stay callable. The body is a
plain `let` body -- any result type (no HOF, no `int` limitation, no poly-result
codegen crash) and *inline*, so the refinement encoder can see it (what B3
needs).

Requirements: `w` is an owned `^mut` local (a `^borrow` parameter does not
register a UT2-visible borrow -- verified), and despawn is `[^unique ^mut w]`.
`ecs/freeze` now exports just `frozen`; `DespawnCap` / `mint-despawn-cap` /
`with-frozen` are retired. Tests: `tests/frozen-region.tur`,
`tests/errors/frozen-despawn-in-region.tur`.

This closes B3 blocker 3 (cap uniqueness) and blocker 1's structural obstacle
(the region body is now inline). What remains for B3: a declared measure-over-`W`
relation, the encoder logic to recognize the `(& w)` borrow and grant
congruence, and the pre-existing refine runtime-check fix.
