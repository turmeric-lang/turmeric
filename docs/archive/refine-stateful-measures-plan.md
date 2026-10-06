---
title: Refinements Over Mutable State
category: Planning
description: A measure must be provably pure to be congruent, which makes every predicate about a mutable world unprovable. Two candidate designs that recover it without reopening the congruence hole.
---

# Refinements Over Mutable State (`RM-S`)

**Status: LANDED (Candidate B).** `#reads` plus the borrow-based `frozen`
region ship in the compiler; every acceptance fixture below is marked DONE and
the guide is out of spec-mode. RM-S1 (Candidate A, epochs) was **not pursued**.
This plan states the problem and the two candidate answers, and keeps the
dogfooding evidence and the design spikes that chose between them -- read the
dated blocks below as a record, not as open work.
**Depends on:** [refinement-types-plan.md](refinement-types-plan.md).
**Feeds:** [ecs-refinement-typed-apis-plan.md](../upcoming/v1/ecs-refinement-typed-apis-plan.md)
gap C2 -- **satisfied**; that plan's RE1 is complete.

> **Reconciled 2026-08-01.** The header used to say the A-vs-B decision was
> "awaiting sign-off". It was decided on 2026-07-26 (Candidate B) and built the
> same day, but the top banner and several "Remaining"/"pending sign-off"
> paragraphs further down were never updated, so this file read as open work
> for a week. Where prose below still says something is remaining, trust the
> shipped code and the DONE-marked acceptance list instead:
> `enc_reads_arg_frozen` (`src/compiler/refine_collect.c:234`, granted at
> `:470`) and `rt_pred_reads_measure` (`src/compiler/elab_fns.c:794`).

> **Status 2026-07-26 -- RM-S0 done, recommending Candidate B; decision open.**
> Both surfaces were written and read in `tur-ecs` (full artifact:
> `docs/upcoming/spices/ecs-rms0-stateful-refinement-dogfood.md`). RM-S0 was run
> *after* C1/RM-B1 landed, so the epoch surface is now testable rather than
> hypothetical. Empirical results (`TUR_REFINE_STATS=1 --enable=refined`):
>
> | case | result |
> |---|---|
> | A.1 epoch as a **pure struct field**, clean guard->read | **1 proven** |
> | A.2 direct `set!` between guard and read | 1 unknown (sound invalidation) |
> | A.3 epoch behind the **real `tur-ecs` inline-C handle** | 1 unknown (the wall) |
>
> **Reading:** the epoch surface does **not** read acceptably for `tur-ecs`, and
> not because of call-site verbosity. It fails on two structural points a lint
> cannot fix: (a) it does not discharge against the *shipped* world (A.3) -- the
> state is shared heap behind a handle, so `world-epoch` is inline C and impure;
> making it discharge means restructuring `sized-world.tur` into by-value
> struct-field state and giving up the shared-world write-propagation the
> scheduler relies on; and (b) its soundness rests on an **unenforced** bump
> discipline (bad point 1), whose failure is an elided use-after-despawn check.
>
> Per this plan's own decision rule, that points to **Candidate B** (scoped
> congruence windows via the linear caps `ecs/cap` already ships). B's failure
> mode is "region smaller than it could be" (loses proofs, keeps checks --
> sound); A's is "check elided that shouldn't be" (unsound). The counterweight
> is size: B is a real language addition (declared mutation rows + region form +
> a third hypothesis-invalidation site), staged as B1 (checked mutation rows,
> no VC interaction) -> B2 (region form + cap borrow) -> B3 (the congruence-window
> link, behind `--enable=refined`, fuzzed with the `stateful` sabotage). A-with-
> a-lint is small but buys no working ECS aliveness proof here, so it is not a
> cheaper win -- it is not a win.
>
> **Decision (2026-07-26): build Candidate B, staged.** RM-S1 (epoch + lint)
> is not pursued.
>
> **B1 done 2026-07-26 -- and it needs no new compiler row.** RM-S0's follow-up
> probing established that the "a mutator is uncallable inside a frozen region"
> property is *already* expressible with the shipped substructural machinery --
> a `:linear` capability, the same mechanism `defsystem` uses. So B1 ships
> spice-side as `ecs/freeze` (`turmeric-spices/spices/ecs/src/ecs/freeze.tur`):
> a `:linear` `DespawnCap<W>` gates a despawn, and `with-frozen` borrows the cap
> for a region so any cap-gated despawn inside the body fails to elaborate
> (`TUR-E0101`, a compile error). Verified: `tests/freeze-region.tur` (positive)
> and `tests/errors/freeze-despawn-in-region.tur` (negative). This is a
> *refinement of this plan's RM-S2 item 1*: the mutation gate is "declared and
> checked" as the plan demands, but realized as a linear cap rather than a new
> effect row -- lower risk, reuses proven machinery, and gives B3 a concrete
> type-level fact (cap frozen in this scope) to key congruence off. Two caveats,
> both carried into B2: (a) non-forgeability is a usage discipline at B1 (mint
> the cap once at world construction; do not re-mint mid-frame) -- B2's region
> *form* makes it structural; (b) the ergonomic region HOF hit a codegen bug
> (`docs/archive/poly-result-hof-capturing-closure-sigbus.md`: a capturing
> closure through a HOF with a *type-variable result* SIGBUSes), worked around
> by fixing `with-frozen`'s body result to `int`. B2 should make the region a
> first-class form (not a polymorphic HOF), or that bug must be fixed first.
>
> **B2 done 2026-07-26 -- the region FORM.** `ecs/freeze` now exports a
> `with-frozen` *macro*: `(with-frozen cap body...)` runs `body` in a scope that
> has borrowed the world's despawn cap, so no despawn is reachable inside it (a
> compile error). Its entry borrows the capability -- not consumes it -- so the
> cap is usable again after the region (verified: `tests/freeze-region-reuse.tur`
> reads a world across a multi-statement region, returns a computed int, then
> despawns after). The macro is sugar over the `with-frozen-fn` borrow
> combinator; a region body evaluates to `int` (end a side-effect pass with a
> trailing `0`).
>
> Why a macro over a HOF and not a first-class special form: the borrow-based
> HOF is the right mechanism (borrow, not consume -- a consume cannot restore
> the linear cap in place; `set!` on a consumed linear binding is itself a
> `TUR-E0101`), but its *polymorphic* form crashes on a capturing body
> (`docs/archive/poly-result-hof-capturing-closure-sigbus.md`), so the region
> result is pinned to `int`. A first-class `(frozen w ...)` special form -- which
> B3 could also key congruence off directly -- is the eventual shape; it is
> deferred behind either that codegen fix or B3's elaborator work, whichever
> lands first, rather than built twice.
>
> **B3 spiked 2026-07-26 -- NOT shippable as one change; gated on prerequisites.**
> The VC congruence-window link would, inside a region, give a world-measure over
> the frozen world a stable (congruent) symbol. But three probes (see "B3 design
> spike" below) show it would be *unsound today*: (1) the region is structurally
> invisible to the encoder -- `with-frozen` is a macro over a HOF, so the body
> elaborates as a separate function and the crossing never sees the region;
> (2) nothing declares a measure to be a function of a world's despawn history;
> (3) the aliasing hole is live -- a *second* `DespawnCap` can be minted and used
> to despawn inside a frozen region (verified rc=0), so a region does not
> actually guarantee "no despawn here". (An earlier "foundation caveat" that the
> refine runtime-check emission was broken turned out to be a Release-`tur`
> artifact -- contracts are stripped under NDEBUG; a Debug `tur` fires them and
> the suite is 2360/1. No foundation fix needed.)
> The one-line encoder hook is understood; it is the prerequisites that make it
> sound, and they are the work. **Recommendation: do not ship the override; build
> the prerequisites first (spike lists the sequence).** RE1 stays blocked on C2
> until then.
>
> **Cap-uniqueness investigation done 2026-07-26 -> sound `frozen` region LANDED
> (started per user request).** Blocker 3 cannot be closed with a library value
> (minting a fresh `DespawnCap` inside a region is unstoppable, verified rc=0),
> and it is the *same problem* as blocker 1. The investigation converged on a
> mechanism that needs **no compiler change and no capability at all**:
> `(frozen w body...)` (an `ecs/freeze` macro) lowers to `(let [_ (& w)]
> body...)` -- it holds an immutable borrow of the *owned* world `w` across the
> body, so a despawn declared `[^unique ^mut w]` is statically rejected inside
> (`TUR-E0200`, verified). This is **sound** (no cap to mint or alias; exclusive
> access cannot be forged from a shared borrow) and the body is elaborated
> **inline** (a `let` body, not a closure). So **blocker 3 is closed and blocker
> 1's structural obstacle is gone** (the crossing now sits in the same scope as
> the region borrow). B1/B2's `DespawnCap`/`with-frozen` are retired. Remaining
> for B3: (2) a declared measure-over-`W` relation; and the encoder logic to
> *recognize* the region's `(& w)` borrow and grant congruence + region-exit
> invalidation. (The "foundation runtime-check fix" is off the list -- it was a
> Release-`tur` artifact; a Debug `tur` fires the checks, suite 2360/1.) See
> "Cap-uniqueness investigation" in the spike.
>
> **Both landed 2026-07-26 -- nothing remains for B3.** The declared relation
> shipped as the trusted `#reads w` annotation and the encoder grant shipped as
> `enc_reads_arg_frozen` (`refine_collect.c:234`, granted at `:470`), with the
> entry-contract suppression in `rt_pred_reads_measure` (`elab_fns.c:794`).

## The problem

Refinement predicates are useful about immutable things and useless about
mutable ones. Not partly useless -- entirely, and by construction.

The rule is in the guide: *"A measure must be provably pure."* Two occurrences
of `(size-of v)` denote one value only if `size-of` is a mathematical function
of `v`. The purity walk is default-deny, and it correctly refuses inline C, a
read of a `^mut` binding, and a field read behind `rc<T>` / `ref<T>`. So:

```turmeric
(defn sized-alive? [s : WorldState e : Entity] : bool
  ;; reads gens[idx] out of a malloc'd control block through inline C
  ...)

(if (sized-alive? s e)
  (get-Pos! cap w e)     ;; the crossing does NOT discharge
  ...)
```

Verified: `0 proven, 1 unknown`. The guard is recovered as a path condition,
the argument is the same expression, and the two occurrences of
`sized-alive?` still get distinct symbols -- so the hypothesis and the goal
are about unrelated opaque values.

**And that is the correct answer today.** `(- (tick) (tick))` is `-1`, not `0`;
the same reasoning applies to a world that is despawned between the guard and
the read. Three separate soundness bugs in this feature came from assuming a
call was congruent when it was not, and the fix each time was to assume less.
Loosening the rule for the ECS's convenience would be the fourth.

### What is NOT wrong

Probes worth recording so nobody re-runs them looking for a hole:

- A `set!` in the caller's body **abandons** the crossing (verified: Unknown).
- A `^mut` parameter is **by value** in Turmeric -- a callee's `set!` is not
  observable by the caller (verified by print). So an intervening
  `(clobber! w)` cannot stale a caller's hypothesis about a struct it passed,
  and the fact that such a crossing reports *proven* is correct rather than a
  bug.
- A `^borrow` parameter's field read **is** congruent -- the decline keys off
  the receiver's type kind (`TY_REF` / `TY_RC` / `TY_PTR_VOID`), and `^borrow`
  is an annotation, not a reference type. This is sound for the same reason
  by-value is: the callee holds no second handle to mutate through.

The mutation channels are closed. The task is to open a *narrow, checked* one,
not to widen the purity walk.

## What "congruent" has to mean instead

The unstated assumption in "a measure must be pure" is that the only thing a
predicate can be a function of is its arguments. For a stateful predicate that
is false: `alive?` is a function of its arguments **and of the world's despawn
history**, and the history is not a value anywhere in the program.

So there are exactly two honest moves: make the history a value, or bound the
window over which it cannot change.

---

## Candidate A -- Epoch arguments (make the state a value)

Give the mutable thing a monotonically increasing **epoch**, bumped by every
mutation, and make the measure a function of it:

```turmeric
(defn world-epoch [^borrow w : GameWorld] #fx{} : int (.epoch w))

(defn get-Pos! [^borrow cap : (ReadCap Pos)
                ^borrow w   : GameWorld
                e : #refine{ x : Entity | (alive-at? (world-epoch w) x) }]
             : Pos ...)
```

`alive-at?` is now a function of two values, and two occurrences are congruent
exactly when the epoch has not moved. A despawn bumps `.epoch`, so the guard's
fact and the read's goal mention different epochs and the proof correctly
fails.

**What is good about it:** it needs *no new compiler feature at all*. The
epoch is an ordinary immutable field read, which probe 5 of the ECS plan shows
is already congruent through a `^borrow`. It is entirely a library discipline.

**What is bad about it:**

1. **Nothing enforces the bump.** A mutator that forgets to increment the epoch
   makes the compiler prove a false thing. The whole soundness argument moves
   into a library's hands, silently, which is the shape of every escape hatch
   this feature has refused so far. It is materially worse than `#fx{}`-lying,
   because there is not even a declaration to point at.
2. **The epoch has to live somewhere the compiler can read purely.** In
   `tur-ecs` the world's mutable state is behind an `:int` handle to a malloc'd
   block, so `world-epoch` would be inline C and therefore impure -- the exact
   wall this is trying to climb. It works only after the state moves into
   struct fields (a large change to `sized-world.tur`).
3. **It is coarse.** Any mutation invalidates every fact, so an ECS that
   spawns during iteration loses all its proofs, including ones about entities
   the spawn cannot have touched.

Point 1 is the one that decides it. This is the "trusted purity annotation"
idea wearing a different hat, and the parent plan's history says trusted
purity is how the miscompiles got in.

---

## Candidate B -- Scoped congruence windows (bound the mutation)

Do not make the state a value; make the *absence of mutation* checkable, and
let a measure be congruent inside a region where mutation is impossible.

Turmeric already has the mechanism: **substructural types**. `ecs/cap.tur`
ships linear `WriteCap<T>` / `ReadCap<T>`, and `defsystem` already uses
linearity to make "writing a component you did not declare" a compile error.
The same shape gives a despawn capability:

```turmeric
;; despawn! consumes the linear cap, so no despawn can happen inside a
;; region that borrowed it away.
(with-frozen-entities w [tok]
  (if (alive? w e)
    (get-Pos! cap w e)     ;; congruent: the region owns the despawn cap
    ...))
```

Inside `with-frozen-entities`, the despawn capability is borrowed by the
region, so `world-despawn!` is not callable, so `alive?` genuinely *is* a
function of its arguments there.

**What is good about it:** the enforcement is the type system's, not a
library's. There is no bump to forget; if you can call the mutator, you are
not in the region.

**What is bad about it:** the compiler does not currently connect "this
capability is unavailable in this scope" to "this measure is congruent in this
scope". That connection is the actual work, and it is not small:

- The purity walk answers a question about a *function's body*, statelessly.
  This asks a question about a *program point*. `rt_binding_is_pure` has no
  notion of where the call is.
- The link between a cap type and the set of functions it gates is a library
  fact today (`get-Pos` takes a `ReadCap Pos` because the macro emits it), not
  something the elaborator knows. Teaching the encoder "these calls mutate
  this thing" needs a declared relation -- something like a `#reads`/`#writes`
  row on the function, which is a real language addition.
- Region entry/exit is a third place refinement hypotheses would have to be
  invalidated, alongside `set!` and the `do`-split rule.

**What is honest about it:** the bad list is all *work*, not *risk*. Every
failure mode is "the region is smaller than it could be", which loses proofs
and keeps checks. Candidate A's failure mode is "a check was elided that should
not have been".

---

## The recommendation, and the phase that tests it

Candidate B is the right shape and Candidate A is the right size, and the
decision should not be made on either of those. It should be made on **whether
anyone writes the annotations**.

So RM-S0 is not an implementation phase.

### RM-S0 -- Write both surfaces by hand, in `tur-ecs`, and read them  [DONE 2026-07-26 -- recommends B; see status block + docs/upcoming/spices/ecs-rms0-stateful-refinement-dogfood.md]

Take the accessor family from
[the ECS plan's RE1](../upcoming/v1/ecs-refinement-typed-apis-plan.md#re1----strict-aliveness-needs-c2-wants-c1)
and write its call sites twice -- once with epoch arguments threaded, once
inside a hypothetical frozen region. No compiler changes; the epoch version
partly runs today, the region version is a sketch that does not compile.

The question RM-S0 answers is the dogfooding plan's "distant third": *is the
annotation burden tolerable?* For this feature it is not third at all, it is
first, because both candidates cost the user something at every call site and
only one of them buys enforcement for it.

If the epoch version reads acceptably, Candidate B's cost is hard to justify
and the answer is A-with-a-lint. If it does not, B is the only one worth
building.

### RM-S1 -- (A) Epoch discipline + a lint that makes forgetting loud  [NOT PURSUED -- RM-S0 chose B]

Only if RM-S0 says so. The minimum viable version is not the epoch itself --
that is library code today -- but a diagnostic: a function that mutates a
struct carrying a field the codebase treats as an epoch, and does not bump it,
should warn. Which requires the epoch to be *declared* as one, which is a
small language addition (`^epoch` on a struct field) and turns "silently
trusted" into "declared and checked".

Without that, do not do A. A trusted purity claim with no declaration to point
at is the thing this feature has spent its whole history refusing.

### RM-S2 -- (B) Declared mutation rows + scope-bounded congruence

Only if RM-S0 says so. Three pieces, in order, each independently useful:

1. **A declared relation between a function and the state it mutates.**
   **[B1 DONE 2026-07-26 -- realized as a linear capability, not an effect
   row.]** Likely spelled as an extension of the effect row -- **but B1 found
   the shipped `:linear` capability machinery already delivers a "declared and
   checked" mutation gate** (`ecs/freeze`'s `DespawnCap<W>` + `with-frozen`;
   despawn inside a frozen region is `TUR-E0101` at compile time). That
   satisfies this item without a new row, and keeps the parent plan's rule --
   the gate is *declared* (a `^linear` cap in the signature) and *checked* (the
   linearity checker), never inferred from `set!`/inline C. A new effect row
   remains an option if B2/B3 need mutation facts the cap cannot carry, but is
   not the starting point. **[Update 2026-07-26: that option is now planned --
   `docs/upcoming/checked-write-frames-plan.md` (`#writes` + a checked tier +
   frame-aware hypothesis invalidation), motivated by two post-RE1 demand
   signals rather than by B2/B3.]**
2. **A region form** whose entry borrows the capability and whose body is a
   congruence window. **[DONE 2026-07-26 -- reworked to the sound `frozen`
   region: `(frozen w body...)` -> `(let [_ (& w)] body...)` holds an immutable
   borrow of the owned world, so a `[^unique ^mut w]` despawn is `TUR-E0200`
   inside. No capability, no minting/aliasing hole, no HOF (any result type),
   body inline. Retires B1/B2's `DespawnCap`/`with-frozen`. This closed blocker
   3 and blocker 1's structural obstacle together -- see the cap-uniqueness
   investigation.]**
3. **Hypothesis invalidation at region boundaries**, joining `set!` and the
   `do`-split rule as the third invalidation site. These three should be one
   shared predicate with one comment naming the failure mode -- `rt_peel_
   contract` had to be reached independently three times before it was
   centralised, and this is the same pattern arriving early enough to avoid
   that. **[B3 SPIKED 2026-07-26 -- not shippable yet; three soundness blockers
   (region invisible to encoder, no declared world-measure relation, live
   aliasing hole) + a broken-runtime-check foundation caveat. See the B3 design
   spike below for the sound sequence.]** **[SHIPPED 2026-07-26 -- all three
   blockers closed; the foundation caveat was a Release-`tur` artifact, not a
   defect. The shared invalidation predicate this item asks for is
   `enc_reads_arg_frozen`.]**

### B3 design spike (2026-07-26) -- what a sound implementation requires, and why it is not one change

Three probes against `build/tur` v0.31.0, on top of the shipped B1/B2, plus a
foundation caveat. All are reasons B3's congruence override **must not ship
yet** -- each would elide a real check on trust, which is the miscompile this
whole plan exists to prevent.

**Blocker 1 -- the region is invisible to the encoder, structurally.** A
stateful `alive?` (impure: reads state) guarding a `get-Pos!` crossing *inside*
a `with-frozen` region still reports `0 proven, 1 unknown`. Not because the
encoder ignores the region -- because it *cannot see it*. `with-frozen` is a
macro over a HOF (B2), so its body is a **closure elaborated as a separate
function**; the crossing's obligation is collected with no knowledge that the
caller froze the world, and a guard in that closure cannot discharge an impure
measure. B3 therefore needs the region body elaborated **inline, with the region
fact in scope** -- a first-class `(frozen cap body...)` special form (the piece
B2 deferred), not a HOF/closure. The existing `do`-split invalidation already
works this way (`rt_form_mentions_set` scans inline statements, `elab_fns.c`);
B3's congruence grant + region-exit invalidation belong on that same inline
path. **The hard part is non-obvious:** giving an *inline* body a borrow SCOPE
for the cap -- the HOF got that for free from its call frame.

**Blocker 2 -- no declared "measure M is a function of world W's despawn
history".** Inside a region that froze W, the encoder would give a world-measure
over W a stable (congruent) symbol -- but it must *know* `alive?` is a
world-measure over W. Nothing declares that today; `alive?` merely reads state.
This is the read-side analogue of the write-side gate B1 built, and it must be
declared and checked, never inferred (the parent plan's standing rule).

**Blocker 3 -- cap uniqueness is a discipline, not a guarantee (the aliasing
hole is live).** Verified: inside a `with-frozen dc` region you can `mint-despawn-cap`
a **second** `DespawnCap<W>` and despawn with it -- rc=0, no error. The freeze
only borrows the *one* cap it was handed; nothing makes despawn authority
unique per world. So a region does **not** actually guarantee "no despawn
here", and a B3 congruence grant on top of it would elide a use-after-despawn
check. This is `errors/refine-stateful-aliased-mutation`, live. Until minting is
structurally once-per-world (e.g. the cap is created only by the world
constructor and never re-mintable), B3 cannot soundly elide.

**Foundation caveat -- RESOLVED 2026-07-26, it was not real.** An earlier
revision flagged "the kept-runtime-check emission looks broken" because ~10
refine fixtures (impure-predicate, let-shadow, match-*, typeclass-*) ran clean
when they should abort. Root cause: those runs used a **Release-built `tur`**,
and `rt_contracts_emitted()` (`elab_fns.c`) strips every contract check under
`#ifdef NDEBUG` unless `--keep-contracts`. Built Debug (CLAUDE.md's bootstrap
default), the suite is **2360 passed, 1 failed** (only `gc-heap-struct-rc`,
unrelated), and every `:pre`/`:post`/`#refine` runtime check fires. So B3's
safety premise -- "the kept runtime check fires when a static proof is absent"
-- **holds today**; there is no foundation to fix. (Lesson: validate
contract/refine-*runtime* behaviour with a Debug `tur`; a Release `tur` is only
valid for compile-time checks -- `--strict-refine` proving, TUR-E02xx, codegen.)

**The one-line hook, once 1-3 hold:** in a recognized frozen region,
`enc_measure` gives a world-measure over the frozen W a stable symbol instead of
the impure fresh-per-occurrence one; region exit joins `set!` and `do`-split as
the third invalidation site (one shared predicate, one comment, per item 3).
That line turns `0 proven` into `1 proven` -- and, if 1-3 are wrong, silently
elides a real check.

**Recommendation.** B3 is a multi-slice compiler project. Updated sequence after
the cap-uniqueness investigation (below) and the foundation re-check:
**~~(ii) inline region form~~ DONE** (the sound `frozen` region, no compiler
change, closed blockers 1 and 3 together) and **~~(i) foundation runtime-check
fix~~ NOT NEEDED** (it was a Release-`tur` artifact; the runtime fallback fires
under a Debug `tur`). ~~Remaining: (iii) add the declared world-measure
relation; (iv) then, and only then, the scoped congruence grant + region-exit
invalidation in `enc_measure` -- keyed off the region's live `(& w)` borrow --
behind `--enable=refined`, not landable until the `stateful` differential fuzzer
sabotage is green (validate it with a **Debug** `tur`, per the foundation
note).~~ **(iii) and (iv) both landed 2026-07-26**, in that order and with that
gate honoured: the sabotage was run (making the frozen check always-true made
`errors/refine-stateful-shadow-despawn` and `errors/refine-stateful-no-region`
wrongly prove, and the hook was removed before commit), and the `stateful`
fuzzer shape is green. The `--enable=refined` qualifier is moot -- `refined`
graduated 2026-08-01 and discharge is unconditional.

### Cap-uniqueness investigation (2026-07-26) -- blockers 1 and 3 are one problem, and the cap should be retired

Probing blocker 3 (make despawn authority unique) established, empirically, that
it **cannot be enforced spice-side**, and *why* -- and the why is the same root
cause as blocker 1:

- **The minting regress is unclosable with a library value.** A fresh
  `DespawnCap<W>` can be minted *inside* a `with-frozen` region and used to
  despawn (verified: rc=0, no error). Minting is a function call; a plain
  exported function cannot be made un-callable in a scope. Gating the minter on
  a consumed token only moves the regress up a level (the token's source is
  itself callable). Linearity forbids *duplicating* a value, not *creating* one,
  so no linear cap is unique-by-construction.
- **Substructural facts do not cross the HOF/closure region boundary.** B1/B2's
  `with-frozen` is a HOF; its `^borrow` of the cap is not seen as active inside
  the body closure. B1/B2's despawn-in-region rejection worked only because the
  closure *captured the same linear value* and re-consuming it double-uses it --
  not because the region was understood. A borrow held by the region is invisible
  to the body, exactly as the crossing's obligation is (blocker 1).
- **`^unique ^mut` is the real exclusive-access gate** (`elab_call.c:5312`
  rejects a `^unique ^mut` argument "when active borrows exist") -- but bare
  `^unique` does not reject a `^borrow` argument (verified rc=0), a call-scoped
  borrow that has already ended does not count, and **there is no inline-borrow
  mechanism**: a `^borrow` `let` binding is unbound (verified). Borrows come only
  from `^borrow` function parameters, so a borrow cannot be held live across
  inline statements today.

**Consequence -- the design simplifies, and it needed no compiler change.**
Blockers 1 and 3 collapse into a single mechanism: an **inline** `(frozen w ...)`
region form that (a) elaborates its body in-scope (fixes blocker 1) and (b) holds
the world's borrow *live across the body*. And the existing borrow forms already
give both: `(frozen w body...)` lowers to `(let [_ (& w)] body...)`. The `(& w)`
registers an immutable scope borrow (`scope_add_borrow`) that stays active for
the `let` body, and passing `w` as `^unique ^mut` while that borrow is live is
`TUR-E0200` (`elab_call.c` UT2). So the separate `DespawnCap` is **unnecessary**:
despawn declared `[^unique ^mut w : World ...]` is structurally uncallable inside
the region, with nothing to mint and no regress. Sounder and simpler than B1/B2's
cap -- the authority is the world's own exclusive access, which cannot be forged
from a shared borrow.

**LANDED 2026-07-26.** `ecs/freeze` now exports just `frozen`; the `DespawnCap`
capability sketch is retired. Verified: `tests/frozen-region.tur` (multi-read
region returns a value, despawn allowed after) and
`tests/errors/frozen-despawn-in-region.tur` (despawn inside -> `TUR-E0200`). The
gotcha: `w` must be an OWNED `^mut` local -- a `^borrow` parameter does *not*
register a UT2-visible borrow (verified), so this works for a world the caller
owns, not one borrowed from elsewhere. That is the correct shape anyway (the
frame that despawns owns the world).

**What remains for B3:** (2) the declared measure-over-`W` relation -- **designed
2026-07-26** as a *trusted* `#reads w` annotation, sound because B3 elides only
the crossing check, never the accessor's kept entry check (see "Blocker 2
design"); ~~pending sign-off~~ **signed off and shipped 2026-07-26**, as it
departs from the literal "no trusted attribute"
line. And the encoder logic to *recognize* the region's `(& w)` borrow and grant
a `#reads w` measure a stable symbol inside + region-exit invalidation -- which
lands *with* blocker 2 (the annotation is inert without it). The region form is
no longer a blocker -- it is a shipped, sound, greppable construct (`(& w)` in a
`let`) the encoder keys off. (The foundation runtime-check fix that previously
appeared here is dropped: it was a Release-`tur` artifact.)

### Blocker 2 design (2026-07-26) -- the declared measure-over-`W` relation, and why a *trusted* one is sound here

Grounding (verified): even inside the *inline* `frozen` region -- body visible
to the encoder, `w` borrowed live -- an impure `alive?` guard still reports
`0 proven, 1 unknown`. The region alone does nothing; `alive?` reads state
through inline C, so the encoder gives it a fresh symbol per occurrence
(impure), and the guard's `alive?(w,e)` and the crossing's are unrelated terms.
To discharge, the encoder must give `alive?` a **stable** symbol inside the
region -- and it may do so soundly *iff* `alive?` is a function of `w`'s (now
frozen) despawn-state and its pure arguments, nothing else. That "nothing else"
is the declaration blocker 2 supplies.

**The obstacle that made this look impossible.** For `tur-ecs`'s real world,
`alive?` reads a `malloc`'d control block through inline C -- opaque. The
compiler cannot *check* that it reads only `w`'s despawn-state (it cannot see
into inline C), and this plan's stated rule forbids a **trusted** purity/measure
attribute: "the cost of a wrong purity claim is an elided check." A pure-Turmeric
world-measure (reading struct fields) is already congruent (RM-S0 case A.1) and
needs none of this; the whole difficulty is the *impure-handle* measure, and it
seemed to force either an uncheckable trusted claim or the world-state
rearchitecture RM-S0 flagged.

**The resolution -- the plan's own objection does not apply if B3 keeps the
safety check.** The plan forbids a trusted attribute *because it elides a check*.
But B3 need not elide the check that matters. There are **two** checks in an
RE1-style guarded read:

- the **crossing** check at `(get-Pos! w e)` -- verifies `(alive? w e)` at the
  call site; this is what a proof elides;
- `get-Pos!`'s **own entry check** -- the `gens[idx]` aliveness compare inside
  the accessor, which RE1 states is *always emitted* ("the refinement's value
  here is the compile error, not the elision"; whole-program entry-check elision
  measured at zero benefit).

So B3's congruence proof elides only the crossing check; the accessor's entry
check is never elided. A **wrong** `#reads w` declaration therefore causes a
*missed compile-time lint* (an unguarded read compiles clean when it should warn
`TUR-W0372`), never a use-after-despawn: the kept entry check aborts at runtime
on a dead handle. That is exactly the asymmetry the plan wants -- the cost of a
wrong claim is a *kept* check, not an elided one -- reached by not eliding the
safety check rather than by refusing the declaration.

> **Correction (2026-07-26) -- which "entry check" is the backstop.** Building
> the codegen path made the two-checks story above precise, and one word of it
> was loose. The backstop is **the accessor's own internal defensive check**
> (the hand-written `gens[idx]` aliveness compare in `get-Pos!`'s body -- plain
> code that always runs), **not** an auto-generated runtime contract for the
> `#refine{ x | (alive? w x) }` param. That refinement *contract* is impure by
> construction (`alive?` is impure -- that is the whole premise), and an impure
> runtime contract is `TUR-E0375` ("predicate has side effects"): it is
> **unemittable**, so the entry-check injector must and now does **suppress** it
> for a `#reads`-measure predicate (`rt_pred_reads_measure` in `elab_fns.c`).
> The soundness argument is unchanged -- the accessor's internal check is the
> kept safety guard -- but the mechanism is "the accessor writes its own check",
> not "the refinement layer emits one for it". A minimal accessor with no
> internal check (like the `refine-stateful-guard-discharges` fixture's `(.n w)`)
> is a congruence proof, **not** a safety demonstration; a real `tur-ecs`
> accessor carries its own bounds/aliveness check as the backstop.

**Proposed shape.**

- A measure that reads mutable state declares the world argument it reads:
  `(defn alive? [^borrow w : World e : Entity] #reads w : bool ...)`. `#reads w`
  names a `^borrow` parameter; it is a promise, not a checked fact, and it is
  sound only in the non-eliding regime above.
- The encoder's congruence rule gains one arm: a call `(m ... w ...)` whose
  callee declares `#reads w` is given a **stable** symbol (congruent) when a
  live borrow of `w` is in scope (the `(frozen w ...)` region's `(& w)`), and a
  fresh symbol otherwise. Region exit / a `set!` of `w` / the `do`-split rule
  invalidate, as the third shared invalidation site.
- Pure-Turmeric measures need no `#reads` and are unaffected -- they are already
  congruent.

**Why not checked-structural instead.** A `#reads w` that the purity walk could
verify would require the measure to read `w` only through the borrowed argument
and touch no other mutable state -- unverifiable for an inline-C body, which is
every real ECS measure. Checked-structural works only once world state is
Turmeric-visible (the RM-S0 rearchitecture). The trusted-but-non-eliding route
delivers the feature against the *shipped* `tur-ecs` today, at the cost of a
weaker guarantee (a wrong declaration loses a lint, not memory safety).

**Decision this needs.** This is a deliberate, documented *departure* from the
plan's literal "no trusted-purity attribute" line -- justified by keeping the
safety check -- and it should be signed off before building. It is also
inseparable from the congruence grant: `#reads` with no encoder arm is inert, so
blocker 2 and the grant land together (behind `--enable=refined`, gated on the
`stateful` differential fuzzer -- run on a Debug `tur`, per the foundation note).

## Acceptance (whichever candidate)

The fixtures are the same either way, and the **negative** ones are the point.
All the ones named below now exist and are green under a Debug `tur` (their
diagnostics shifted with the move to the sound borrow-based `frozen` design --
noted per fixture):

- `refine-stateful-guard-discharges`: guard, then read, no mutation between.
  **Proves** (and now codegens/runs -> `42`; the entry-contract-suppression fix
  is what let a `#reads`-refined accessor build at all). **[DONE]**
- `errors/refine-stateful-mutation-invalidates`: guard, **mutate** (`set!` of
  the frozen world's state), then read. Must NOT prove -- it is the fixture that
  catches the design being implemented as an escape hatch. The mutation is the
  third shared invalidation site (region exit / `set!` of `w` / do-split), so
  the crossing goes unknown and `--strict-refine` makes it a hard error
  (`TUR-W0372`). **[DONE 2026-07-26]**
- `errors/refine-stateful-aliased-mutation`: the mutation reaches the world
  through a *second* handle. This is the aliasing question the `rc<T>` /
  `ref<T>` decline answers for field reads -- and the borrow-based region
  **inherits** the answer rather than re-deriving it: while `(& w)` is live no
  *mutating* handle to the frozen world (aliased or direct) can be acquired, so
  the attempt is a structural `TUR-E0200` (borrow conflict), not a runtime hole.
  Uniqueness is the aliasing answer. **[DONE 2026-07-26]**
- Two supporting negatives added alongside: `errors/refine-stateful-no-region`
  (no `(& w)` -> impure, never congruent) and `errors/refine-stateful-shadow-despawn`
  (a shadowed inner `w` is not the frozen one), plus the positive
  `refine-stateful-nonstrict-warns` (non-strict surfaces an unproven crossing as
  a `TUR-W0372` warning rather than silently trusting it). **[DONE]**
- Source-level differential fuzzing, with a `stateful` generator shape and a
  sabotage: make the invalidation a no-op and confirm the fuzzer reports
  soundness bugs where the shipped build reports zero. Per the parent plan's
  standing lesson -- both historical soundness bugs lived in the encoder, below
  the VC fuzzer's reach -- this is the only harness that covers this work.

  **Status 2026-07-26 -- source fuzzer UNBLOCKED (double-load fixed); `stateful`
  shape SHIPPED; a codegen blocker found and fixed along the way.** The fuzzer
  was vacuous because `tur` double-loaded its stdlib when spawned via a
  subprocess from a checkout root; fixed in `load_path_key` (resolve a
  `stdlib/X` load via `stdlib_dir` first). Report archived at
  [`refine-fuzzer-subprocess-stdlib-double-load.md`](history/refine-fuzzer-subprocess-stdlib-double-load.md).
  **Run it with a Debug `tur` and `TUR_STDLIB_DIR` unset** (Release strips
  contracts so gate-off never aborts; a stale env stdlib is the wrong one).

  Writing the `stateful` shape surfaced a **codegen blocker that had to be fixed
  first**: a `#refine{ x | (m w x) }` param whose measure `m` is a `#reads`
  measure is *impure by construction*, and `rt_inject_param_checks`
  (`elab_fns.c`) unconditionally injected a runtime **entry contract** for every
  refined param -- an impure predicate there is `TUR-E0375` ("predicate has side
  effects"). So *no* `#reads`-refined function could `emit-c`/`build`/`run` at
  all: the whole feature was `check`-only, and `refine-stateful-guard-discharges`
  (which asserts `stdout: 42`) was silently red under the harness's `emit-c`
  path. Fixed by teaching the entry-check injector to **suppress** the contract
  for a `#reads`-measure predicate (`rt_pred_reads_measure`), since that check is
  unemittable and, per Blocker 2, is not the safety backstop anyway (the accessor
  keeps its *own* internal check). Non-`#reads` impure predicates still get
  E0375 (protection intact); the `errors/` negatives still reject under
  `--strict-refine`; `guard-discharges` now runs and prints `42` on both gates.

  The `stateful` shape (a `FzWorld`, an impure `#reads w` measure, a reader with
  a `#refine{ x | (fz-alive? w x) }` crossing, and a `frozen`-region guarded
  read) now lands in `tests/refine-fuzz-src.py` and classifies `agree_clean`
  (self-test PASS; n=150 both-mode and n=400 int-mode runs: **0 soundness bugs,
  0 other BUG classes**).

  **What the runtime fuzzer does and does NOT gate for `#reads`.** The shape is
  *correctness/crash* coverage (a codegen divergence between gates, or a crash on
  one gate, is caught). It is **not** a soundness gate, and cannot be: a `#reads`
  measure is impure, so gate-off carries **no** runtime contract for the crossing
  (the entry contract is suppressed as above) -- there is no gate-off abort for a
  gate-on elision to contradict. `#reads` soundness is a **compile-time** property
  (a wrong crossing proof is a missed static error, never a runtime miscompile),
  gated by the `errors/refine-stateful-*` fixtures under `--strict-refine` plus
  the targeted frozen-check sabotage below. This is a structural property of a
  *trusted, impure* measure, not a fuzzer limitation to be engineered around.

  **Interim gate met by targeted sabotage.** The plan's *core* requirement --
  "make the frozen-check a no-op and confirm the negatives are caught" -- was
  run directly: env-gating `enc_reads_arg_frozen` to always-true made
  `errors/refine-stateful-shadow-despawn` and `errors/refine-stateful-no-region`
  **wrongly prove** (compile clean under `--strict-refine`, no `TUR-W0372`),
  i.e. those `errors/` fixtures FAIL under the sabotaged build and PASS under the
  shipped one -- exactly "soundness bugs reported where the shipped build reports
  zero". The sabotage hook was removed before commit (a soundness backdoor must
  not ship). This is narrower than the fuzzer (it exercises the shapes I thought
  to write, not random ones), so the source-fuzzer `stateful` shape is still
  owed once the double-load is fixed.

### Documentation (landing task -- do NOT skip)

The guide [`stateful-refinements-guide.md`](../guides/stateful-refinements-guide.md)
was written *ahead* of the implementation and is explicitly marked **in-flight /
spec**. When `#reads` + the congruence grant land, **update that guide in the
same PR**:

- flip its status banner from "in-flight / not yet implemented" to shipped, with
  the graduation state of the `refined` experiment;
- replace "specified here and not yet implemented" / "verified: still `0 proven,
  1 unknown`" hedges with the real behaviour and the passing fixture names;
- confirm every `#reads` code sample compiles and every claimed diagnostic
  (`TUR-E0200`, `TUR-W0372`) matches what the build emits;
- move it out of the "trusted now (step 1)" framing only if step 2/3 also
  landed -- otherwise keep the trajectory section honest;
- add it to the shipped-guides index `docs/guides/README.md` (it is deliberately
  left out while in-flight).

Treat a green `stateful` fuzzer and this guide update as jointly gating the
feature's landing. A shipped `#reads` with a stale spec-mode guide is a
regression in its own right.

## What this plan will not do

- **No trusted-purity attribute that elides a check.** No `#[pure]`, no
  `:measure` that removes a runtime guard on trust. The cost of a wrong purity
  claim must never be an *elided* check. **Reconciled 2026-07-26:** `#reads w`
  (blocker 2) *is* a trusted attribute, but it does not violate this line,
  because B3 elides only the call-site crossing check, never the callee's kept
  entry check -- a wrong `#reads` costs a missed lint, not an elided safety
  check. The invariant is "no trusted claim elides a *safety* check", and
  `#reads` honours it. See "Blocker 2 design".
- **No heap model.** Nothing here adds a memory theory to the VC. Both
  candidates work by making the state question disappear before encoding --
  A by turning it into an argument, B by proving it cannot change.
- **No inference.** Neither an epoch's bump sites nor a region's extent is
  inferred. Checking, not inference, is the founding decision of the
  refinement design and this plan does not reopen it.

## References

- [refinement-types-plan.md](refinement-types-plan.md) -- particularly
  "The purity gap was a real miscompile", "Purity, take two: the effect row was
  never evidence", and "The third congruence door"
- [refinement-types-guide.md](../guides/refinement-types-guide.md) -- the
  purity rules as shipped
- [substructural-types-guide.md](../guides/substructural-types-guide.md) --
  the linear/borrow machinery Candidate B leans on
- [ecs-refinement-typed-apis-plan.md](../upcoming/v1/ecs-refinement-typed-apis-plan.md) -- the
  consumer
- [refined-dogfooding-plan.md](refined-dogfooding-plan.md) -- RM-S0 is
  a slice of it
