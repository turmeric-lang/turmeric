---
title: Solver Extension Plan (SX)
category: Planning
description: Extending the refinement solver into an incremental, backtracking decision procedure -- a trail with per-cell opt-out, a capture/restore cost curve in the benchmark harness, an offline lazy-SMT stepping stone, an untrusted-search/trusted-checker seam, and a query surface for interrogating the solver from outside the compiler.
---

# Solver Extension (SX)

> **Archived 2026-09-05.** Every live phase has landed (SX0, SX1, SX2, SX3,
> SX8a-c, SX9 including the `"schema": 1` stamp); what is not built -- SX4,
> SX5's remainder, SX6, SX6b -- is parked on evidence gates that have been
> measured shut, with the re-measurement recipe recorded under each.  A later
> integer-side slice of SX7 lives in
> [solver-integer-tail-plan.md](solver-integer-tail-plan.md),
> which is where the solver's open work is tracked now.

**Status:** proposal. SX0 (both instruments), SX8a (the interrogation surface),
SX1 (the trail primitive, minus its effect row), SX2 in full (measurement,
combinators, and the depth-first driver), and SX3 (incremental EUF) have
landed; everything still open is parked on evidence. SX4 and SX6 are parked on SX0(b)'s
evidence; SX2's gate came back the other way -- the trail pays for itself, so
SX3/SX4 should build on it. Nothing here is on the critical path to v1; the whole of SX is
*additive* to a solver that already ships and is already sound. Read section 5
for what to do first if only one phase gets built.

**What the measurements changed.** Both SX0 instruments have now run.

SX0(b) exists to gate the two most expensive phases, and it gates them both
**shut**: neither the arithmetic cap (SX4) nor the cube caps (SX6) fire on any
of the 411 units swept, and no cap hit anywhere cost a real proof. That removes
roughly the plan's whole right-hand side from consideration for now.

SX0(a) answers the review's second question and moves a number the design was
assuming: the DK chain and a ucontext fiber cross at **F ~ 20 frames on
restore**, not at the hundreds the "small constant beats a real slope" framing
implied -- and there is no one-shot fast path to fall into, so a search layer
resuming over more than a shallow slice is on the wrong side of that line
today. It also found the plan's own `a + b*F + c*E` cost model mis-specified
for a chain (the env term is per-frame, `c*F*E`).

SX8a has since landed too, so the solver is now answerable from outside the
compile pipeline (`tur smt`, `--dump-refine=json`) -- which is what every later
phase's acceptance tests were going to want a door for.

**SX2's driver has now landed too** (`stdlib/backtrack-dfs.tur`; see the
phase's landing note for what moved and the two compiler bugs the work
surfaced).  **SX3 has landed** -- one EUF state with mark/undo per cube,
verdict-identical to the rebuild path, `TUR_REFINE_EUF=rebuild` kept as the
replay seam.  **SX8b has landed** (2026-09-03) -- `tur smt` runs a script as a
session with a real assertion stack, plus `--interactive`; its own premise
turned out to be wrong (push/pop do NOT map onto the trail, and could not) and
the phase records the correction and the differential acceptance test that
replaced it.  With those down, every live phase of this plan is done:
what remains is parked on evidence (SX4, SX6, and their dependents) or is
follow-up filed in its own right (`#fx{Bt}` for precision, the self-recursion
bug below).  Details under
SX0(a), SX0(b) and SX8a; reproduce the two measurements with
`benchmarks/run-capture-curve.sh` and `benchmarks/run-cap-sweep.sh`.

## 0. Provenance

This plan is the write-up of an outside design review of the refinement
solver. Two excerpts from that review are quoted verbatim in section 1; the
originating conversation
(`https://claude.ai/share/bb3cf410-6151-44f4-a2f3-fed54a946731`) sits behind a
Cloudflare interstitial and could not be fetched from the build container, but
a structured summary of the full discussion was supplied afterwards and is
folded in throughout. Beyond the two headline questions, that summary covers:
why the cube design is what it is and when it stops being tolerable; the real
cost of moving to DPLL(T)/CDCL(T) (the theory seam and the testing burden, not
the SAT core); whether continuations help the theory seam (mostly no, with
specific exceptions); the consequences of `tur` having a live compile-time
runtime; multi-shot versus backtrackable; and a recommended layer split. Each
of those has a home below: sections 2, 3.5-3.7, 4, and the phase structure of
section 5.

The rest of this document is (a) what the codebase actually does today,
checked against the source, (b) the literature each point leans on, and (c) a
phased plan.

One item is *not* from the review: the external interrogation surface (SX8)
was requested in follow-up -- expose the solver so tools, tests, and users
outside the compile pipeline can put queries to it directly. It slots in
cleanly because every later phase makes the same surface strictly more
informative (marks make `push`/`pop` real, certificates make `get-proof`
real), so it is specified here rather than as a separate plan.

## 1. The two review questions

### 1.1 Backtrackable state as a runtime primitive

> "If it means something closer to **backtrackable state** -- a trail that
> records mutations and undoes them on backtrack, a la Prolog's WAM or a CLP
> engine -- that's a much better match. That's precisely what EUF's undo trail
> and simplex's bound stack are hand-rolling in C. Having it as a runtime
> primitive would be real leverage, provided you can mark specific cells as
> *not* backtrackable (learned clauses, activity scores, the simplex basis).
> Every constraint system that does this well has that escape hatch; Prolog
> spells it `assert`, ASP solvers spell it 'global.'
>
> So the question I'd put to your own runtime: **can I opt a cell out of the
> trail?** If yes, the theory-seam story gets substantially better than what I
> said earlier. If the trail is all-or-nothing, you're back to threading state
> around it."

Short answer, and the reason section 3 exists: **there is no trail in the
runtime today, so the honest answer is "not yet -- and when it lands, opt-out
is the default, not an escape hatch."** See 3.3 for the spelling.

One correction to the premise, recorded because it changes the shape of the
work: EUF's undo trail and simplex's bound stack are **not** hand-rolled in
`tur` today -- they do not exist. Today's solver backtracks by *discarding*
theory state and rebuilding it from scratch for the next cube
(`refine_solver_euf.c:199`, `refine_solver_no.c:44` -- a fresh `euf_new` /
`la_new` per cube out of an arena). The reviewer is describing the shape the
extension has to take, and is right that it is the shape; SX3/SX4 are where
those two structures get built, and section 3.4 is the table of what they trail
and what they deliberately do not.

### 1.2 The number to get out of the benchmarks

> "On the benchmarks -- the number I'd want out of them isn't 'how far off
> native,' it's **cost per capture/restore as a function of live state under
> the prompt**. That single curve decides more of this design than the
> interpreter-vs-JIT ratio does. Worth making sure it's in the harness while
> you're building it."

Taken as written, and it sets the phase order: the harness change (SX0) lands
**before** the primitive it is meant to measure (SX1). Section 4 designs the
curve.

## 2. Where the solver is today

Facts, checked against the source. The user-facing feature is documented in
[refinement-types-guide.md](../guides/refinement-types-guide.md); the internals
in
[refinement-solver-internals-guide.md](../guides/refinement-solver-internals-guide.md);
the design of record is
[refinement-types-plan.md](refinement-types-plan.md).

| Stage | File | What it decides | Incremental? |
|---|---|---|---|
| S0 | `refine_solver_s0.c` | trivial / syntactic | n/a (stateless) |
| S1 | `refine_solver_euf.c` | congruence closure (EUF), naive O(n^2) fixpoint | **no** -- `euf_new` per cube |
| S2 | `refine_solver_arith.c` | linear arithmetic, Fourier-Motzkin over exact int64 rationals | **no** -- `la_new` per cube |
| S3 | `refine_solver_no.c` | Nelson-Oppen combination of S1+S2 | **no** -- both rebuilt per cube |
| (S4) | -- | never built; boolean structure is handled by small-DNF cube expansion | -- |

The invariants that constrain every line of this plan:

- **One-directional soundness.** A stage may never answer `RT_VALID` for
  something not entailed. `RT_UNKNOWN` is always safe and always available,
  because every obligation still has its runtime contract check to fall back
  on. Adding a stage moves obligations left in the chain; it never changes an
  answer.
- **Everything is capped** (`refine_solver.h:29-41`): 64 cubes, 64 literals per
  cube, 32 LA variables, 512 LA constraints, 512 EUF terms. A cap hit is
  `RT_UNKNOWN`, i.e. a kept runtime check. Today, *the caps are the reason
  there is no incrementality*: a rebuild per cube is affordable precisely
  because the cube count is bounded at 64.
- **No solver dependency.** No SMT library in any default or release build; the
  Z3 dev oracle was retired in 0.32.5. The replacement is data in the repo --
  `tests/corpus/smtlib/` (125 labelled `.smt2` benchmarks) replayed by the
  `tur_refine_corpus` ctest target -- plus the source-level differential fuzzer
  `tests/refine-fuzz-src.py`. Both of those are the acceptance instruments for
  every phase below.

### 2.1 Why the cube design is what it is, and when it stops being tolerable

The review walked through this and it is worth pinning down, because it is the
premise behind SX6:

- Theory procedures decide *conjunctions*. Congruence closure takes a set of
  (dis)equalities; Fourier-Motzkin takes a set of linear inequalities. Neither
  handles disjunction. The DNF cube expansion exists to strip boolean structure
  so each cube can be handed to S1/S2/S3 whole.
- DNF is worst-case exponential, which is why no production solver does it this
  way: real solvers run DPLL(T)/CDCL(T), where a SAT engine lazily enumerates
  boolean assignments (each one *is* a cube) with clause learning and theory
  propagation. Turmeric's caps make the naive expansion tolerable, and the
  runtime fallback makes the cap-hit answer sound.
- The disequality wrinkle compounds it: `a != b` over a total order is
  `a < b OR b < a`, so a *negated equality* creates a disjunction in NNF --
  even the most natural postcondition shape, `(= r <expr>)`, needs cube
  expansion. The same integer non-convexity is what S3 declines to case-split
  on.
- The diagnosis for when small-DNF actually fails: **when the propositional
  structure of VCs grows** -- path-sensitive obligations over `match`/`if`, or
  loop unrolling. If RT1 does not generate such VCs yet, a SAT engine buys very
  little. That is a measurable question, and SX0(b) exists to answer it before
  anyone writes one.

The existing seams the extension slots behind (`refine_solver.h:70-105`):

```c
EufState *euf_new(RefineVC *, Arena *);
bool      euf_assert_cube(EufState *, const VCCube *);
bool      euf_assert_eq(EufState *, VCTerm *, VCTerm *);
bool      euf_equal(EufState *, VCTerm *, VCTerm *);

LaState  *la_new(RefineVC *, Arena *);
bool      la_assert_cube(LaState *, const VCCube *);
bool      la_assert_le(LaState *, VCTerm *lhs, VCTerm *rhs, bool strict);
bool      la_unsat(LaState *);
bool      la_entails_eq(LaState *, VCTerm *, VCTerm *);
```

These are already the right shape. What they are missing is exactly three
operations -- a mark, an undo, and (for SX6b) an *explanation*: the ability to
return the inconsistent subset of asserted literals that caused a conflict, so
the caller can learn a clause from it. The first two are the trail; the third
is the proof machinery of 3.7.

## 3. Answer to Q1 -- the trail

### 3.1 What every system that does this well actually does

The reviewer's claim that the escape hatch is universal holds up. The research,
system by system, with what each one keeps *out* of the undo trail:

**Prolog / the WAM.** On backtracking to a choice point, every binding made
since that choice point is undone; the trail is the record of which. The WAM
does not trail every binding -- it uses **conditional trailing**: compare the
address of the variable being bound against the heap/stack pointers saved in
the topmost choice point, and trail only if the variable is *older* than the
choice point. A variable created after the choice point will be discarded
wholesale on backtrack, so recording it is pure overhead. This is the single
most important optimization in the design, and it generalizes (see 3.2, "the
stamp"). The escape hatch is `assert`/`asserta`: a clause added to the database
is not undone on backtracking, which is exactly how Prolog programs accumulate
knowledge across a search.

**CLP engines.** Constraint engines extend the WAM's binding trail into a
**value trail**: to allow destructive update of a location (a domain bitmap, a
bound), the trail entry records the location *and its previous value*, so undo
restores an arbitrary earlier state rather than just "unbound". The literature
flags the cost this introduces -- the same location written repeatedly inside
one choice-point level gets trailed repeatedly unless the engine timestamps
cells. Handle it the same way conditional trailing handles the WAM case.

**CDCL SAT (MiniSat and descendants).** The assignment trail plus `trail_lim`
(the index in the trail where each decision level began) *is* the mechanism:
backjumping to level `d` pops the trail down to `trail_lim[d]`. What is
explicitly **not** popped: learned clauses (a clause derived by resolution is
implied by the original formula, so it is valid at every level, not just the
one that produced it), VSIDS activity scores and their decay, phase saving, and
the restart/luby state. Those are the reviewer's "learned clauses, activity
scores" -- and their whole value comes from surviving the backtrack that
produced them. The canonical backtrack is *decrementing a trail index*, and
CDCL does it millions of times per second -- a number that matters in 3.6.

**ASP solvers** spell the same idea "global" -- nogoods/lemmas recorded at
level 0 are permanent.

**Simplex in DPLL(T)** (Dutertre and de Moura, CAV 2006 -- the exact reference
S2b targets). Backtracking saves the value of each **bound** on a stack before
updating it and restores from that stack; the **tableau and the basis are not
restored at all**. Any basis is a valid starting point for the next check, so
restoring one costs pivots and buys nothing. This is the cleanest example in
the literature of "the escape hatch is not a hack, it is where the performance
comes from."

The pattern across all five: the trail covers *semantic* state whose value is
level-dependent, and skips *derived, monotone, or heuristic* state. A design
that cannot express that distinction is not a trail, it is a checkpoint.

### 3.2 The proposed runtime primitive

A per-thread trail in the runtime, with an explicit, allocation-time choice per
cell. Sketch of the C side (`src/runtime/trail.c`, new):

```c
typedef struct TurTrailEntry {
    int64_t     *slot;       /* the cell's payload word */
    int64_t      old;        /* value to restore */
    uint32_t     stamp;      /* level at which this cell was last trailed */
    TurDropFn    drop;       /* rc/aggregate teardown for the discarded value */
    TurCloneFn   clone;      /* rc/aggregate re-acquire for the restored value */
} TurTrailEntry;

uint32_t tur_trail_mark(void);              /* push a level, return its id     */
void     tur_trail_undo_to(uint32_t mark);  /* pop to that level, running undo */
void     tur_trail_commit_to(uint32_t mark);/* drop the level, keep the writes */
void     tur_trail_record(int64_t *slot, int64_t old, TurDropFn, TurCloneFn);
```

Four decisions that are not obvious and should be settled in SX1:

1. **Two cell flavors, not one.** A *value cell* (`BtCell`) trails the previous
   payload on every write -- the CLP shape, needed for union-find parents,
   simplex bounds, domain bitmaps. A *write-once cell* (`LVar`) is the WAM
   shape: it can go from unbound to bound once per level, so the trail entry
   only has to say "reset to unbound", and conditional trailing applies
   verbatim. `stdlib/logic.tur`'s `TVar`/`Subst` pair is exactly the second
   one, today encoded as a persistent association list.
2. **The stamp is what makes it cheap.** Every cell carries the level at which
   it was last trailed. `bt-set!` records to the trail only when
   `cell->stamp < current_level`, then bumps the stamp. A cell written a
   thousand times inside one level costs one trail entry. This subsumes the
   WAM's address comparison (a cell allocated inside the current level starts
   with the current stamp, so its first write is not trailed either) and fixes
   the redundant-trailing problem the CLP literature reports.
3. **Undo must be ownership-correct.** Turmeric values can be rc handles.
   Restoring an old value means re-acquiring it and dropping the value being
   discarded, which is why the entry carries a clone/drop pair. This is the
   same mechanism the continuation machine already uses for owning frame
   environments (`DKEnvClone`/`DKEnvDrop`, `src/runtime/cps_prompt.h`), and
   SX1 should reuse those function types rather than mint parallel ones. A
   `:copy` cell gets both fields NULL and the fast path.
4. **Marks are generational.** A mark is a level id plus a generation counter,
   so `bt-undo-to!` on a mark that has already been undone or committed is a
   detected runtime error rather than memory corruption. This matters because
   multi-shot continuations make stale marks reachable (see 3.5).

Turmeric surface (`stdlib/trail.tur`, new). Types are spelled out per the
project's no-`:int` rule -- a mark is an opaque newtype, not a bare integer:

```turmeric
(defopaque BtCell [A] :ptr<void>)   ; kind (* -> *), like (Goal A) in logic.tur
(defopaque GCell  [A] :ptr<void>)   ; never trailed -- the opt-out flavor
(defopaque Mark   :int)             ; opaque level handle

(defn bt-cell-new  [A] [init : A] : (BtCell A) ...)
(defn g-cell-new   [A] [init : A] : (GCell A)  ...)   ; opts out at allocation
(defn bt-get  [A] [c : (BtCell A)] : A ...)
(defn bt-set!  [A] [c : (BtCell A) v : A] #fx{Bt} : void ...)
(defn bt-mark [] #fx{Bt} : Mark ...)
(defn bt-undo-to!  [m : Mark] #fx{Bt} : void ...)
(defn bt-commit-to! [m : Mark] #fx{Bt} : void ...)
(defn bt-scope [A] [body : (fn [] #fx{Bt} A)] #fx{Bt} : A ...)  ; mark/undo bracket
(defn with-untrailed [A] [body : (fn [] #fx{Bt} A)] #fx{Bt} : A ...)
```

`#fx{Bt}` is a new leaf effect row (name to settle in SX1 against
[effects-system-guide.md](../guides/effects-system-guide.md)). It has to be a
real row for one specific reason: the refinement solver's own purity whitelist
decides whether two occurrences of a call may be treated as the same value
(`refine_collect.h:53`). A function that mutates a trailed cell is not pure and
must not be congruence-collapsed. Adding the primitive without adding the row
would put a hole in the thing this plan is extending.

### 3.3 So: can you opt a cell out?

**Yes, three ways, in ascending scope.** The design deliberately makes the
answer "yes" at every granularity the reviewer's examples need:

| Granularity | Spelling | Motivating case |
|---|---|---|
| Per cell, forever | `g-cell-new` instead of `bt-cell-new` | activity scores, learned-clause database, the simplex basis and tableau |
| Per write | `with-untrailed` around the mutation | a counter bumped inside a search that must survive it (node count, statistics) |
| Per level | `bt-commit-to!` instead of `bt-undo-to!` | a level's writes are kept -- Prolog's `assert`, ASP's "global", promoting a lemma to level 0 |

`GCell` and `BtCell` are *distinct types*, so opting out is visible in the
signature of anything that touches the cell rather than a flag someone forgets.
A `GCell` write is not an undo-able effect, which is precisely the property the
learned-clause database wants.

The trail is therefore not all-or-nothing, and the "threading state around it"
fallback the reviewer worried about does not apply.

### 3.4 What the extended solver trails, and what it does not

This table is the actual answer to "does the theory-seam story get better", and
it is worth writing down before any code because it is the specification for
SX3 and SX4:

| Solver state | Trailed | Why |
|---|---|---|
| EUF union-find parent / rank arrays | yes (value cells) | class membership is level-dependent |
| EUF congruence lookup table entries | yes | derived from the partition |
| EUF disequality list | yes | asserted per level |
| Simplex bounds `lo[x]`, `hi[x]` | yes -- *this is the bound stack* | Dutertre--de Moura; the only thing that must be restored |
| Simplex tableau rows and basis | **no** | any basis is a valid start; restoring costs pivots, buys nothing |
| Simplex assignment `beta(x)` | **no** | repaired lazily on the next `check()` |
| Learned lemmas / blocking clauses (SX6) | **no** | implied by the original VC, hence valid at every level |
| Branching heuristic scores (SX6) | **no** | heuristic; the whole point is that it accumulates |
| Hash-consed `VCTerm` table, the arena | **no** | monotone within a query; terms are immortal by construction |
| RT7 discharge memo (fingerprint -> verdict) | **no** | process-lifetime, already keyed on VC identity |
| RT6 hint-generation probes | **no** (each probe is its own query) | speculative, reports nothing |

Five of the eleven rows are opt-outs. That ratio is the reason a trail without
an escape hatch would not have been usable here.

### 3.5 Trail meets continuations -- and why multi-shot is the wrong strength

Turmeric already ships `shift`/`reset`, `shift0`/`reset0`, `call/cc`,
`call/cc*` (multi-shot) and `escape`
([delimited-control-operators-guide.md](../guides/delimited-control-operators-guide.md)).
A trail is a *stack* discipline; a multi-shot continuation is not.

First, the framing point from the review, which the rest of this section
serves: **backtracking in a solver is asymmetric, and a continuation is
symmetric.** A continuation restores *everything*; a CDCL backjump discards the
trail above the backjump level but **keeps the learned clause** -- the only
thing preventing re-exploration -- and simplex keeps its basis. Threading the
kept state around the restore leaves the continuation carrying nothing. So "use
continuations as the theory-seam backtracking mechanism" dissolves into "use
backtrackable *state*" -- this section's trail -- with control expressed
however is convenient. If "backtrackable continuation" is read as "capture a
resumption point and re-enter", that is one-shot capture by another name and
the same asymmetry objection stands.

Second: **multi-shot is specifically the wrong strength for solver-internal
search.** Reusability is what makes capture expensive -- the copying and the
immutability discipline exist so a second entry does not see the first entry's
mutations. CDCL-style search is *entirely* mutation over a shared trail, watch
lists, and activity scores, and each node of its search tree is entered exactly
once. A multi-shot default pays for reusability, on the most frequent operation,
for an algorithm that never uses it. The runtime already distinguishes the
paths (`call/cc` one-shot vs `call/cc*` multi-shot); the one-shot path is the
relevant one for search, and SX0's `R` axis measures the one-shot/multi-shot
gap instead of assuming it.

With that placed, the interaction between the trail and the existing operators
has one sharp edge and one design decision:

- **Unwinding is fine.** `escape`, an abortive `shift0`, a panic: the runtime
  unwinds past a `bt-scope`, which runs the undo. Same as any scope-exit
  action.
- **Re-entering is the sharp edge.** Invoke a captured `k` twice and the second
  invocation resumes a computation whose trail levels were already undone. The
  writes are not replayed by the capture -- the continuation captured *control*,
  not *state*. Options: (a) make it a checked runtime error via the generation
  counter of 3.2(4) -- cheap, honest, and the recommendation for SX1; (b) have
  `call/cc*` snapshot the live trail segment, making state genuinely multi-shot
  at a cost proportional to the trail depth -- which is the second axis of the
  benchmark in section 4 and should not be built before that curve exists.

**DECIDED 2026-08-29: (a) is permanent. (b) is declined, not deferred.** This
was the last open question holding `backtrackable-state` at
`XF_LIFECYCLE_PROTOTYPE`; it is what SX9's graduation clause was waiting on.
Three things settle it, in increasing order of weight.

**1. The curve exists now, and (b) is not cheap.** A trail entry is 32 bytes and
a record-plus-undo cycle costs **5.0 ns** per write (`capture-curve-results.md`,
the T axis, mean over T >= 8). Snapshotting that same entry costs a 32-byte copy
-- which the `dk` fit prices at 0.14-0.18 ns per env byte, so **4.6-5.8 ns**
before anything else -- plus a `clone` hook call (~2 ns, the `closure-call`
baseline) for every entry whose payload owns something. So a snapshot of depth
T is at best at parity with simply replaying T writes, and usually worse. The
asymmetry in *when* it is paid is what decides it: the undo is paid only on the
branch actually taken, while a snapshot is paid at **every capture**, including
the captures that are never re-entered. For search, that is the wrong end.

**2. (b) does not give a solver what it needs even if it were free.** This
section's own opening framing: backtracking in a solver is asymmetric, and a
snapshot is symmetric. A CDCL backjump discards the trail above the backjump
level but **keeps the learned clause**; simplex keeps its basis. A snapshot that
faithfully restores the live trail segment restores the learned clause away
too -- it reproduces exactly the problem that made "use continuations as the
theory-seam backtracking mechanism" dissolve into "use backtrackable state" in
the first place. The operation that *does* serve the asymmetric case is
`bt-commit-to!`, which is shipped. (b) would be a faithful implementation of the
wrong shape.

**3. Nothing wants it.** The one candidate genuine multi-shot consumer named
anywhere in this plan is RT6 hint generation (below), which is unmeasured and
filed as open question 3 rather than as a phase. Building a mechanism with a
measurable cost at every capture, for zero present callers and one hypothetical
one, on the strength of symmetry that the actual consumers do not want, is not
a trade this plan should make.

**4. Found while writing the fixture: the typed surface already refuses the
shape.** `Mark`, `BtCell` and `GCell` are all `defopaque` with no `Clone`
instance, and `cloneable-shift` requires `Clone` on every captured binding --
so a cloneable (multi-shot) continuation **cannot close over a trail handle at
all**. It is `TUR-E0014` at compile time, not a runtime hazard. The re-entry
edge is therefore only reachable by laundering a mark through a plain `:int`
(or through inline-C), which is what `sx1-trail-reentry-stale-mark` does to
exercise the backstop. This was not known when 3.5 was written and it inverts
the emphasis: the static refusal is the primary defense and the generation
counter is defense in depth. It also means (b) would have had to *add* a `Clone`
instance for `Mark` before it could do anything -- deliberately widening the
hole the type checker currently closes. Pinned by
`errors/sx1-trail-mark-not-cloneable`, precisely so that adding such an
instance cannot happen silently.

**Revisit trigger, stated so it is falsifiable:** measure RT6 hint generation
(the SX0 curve x `TUR_REFINE_STATS` probe counts, per open question 3). If it
both beats the RT7 memo *and* wants state to be multi-shot rather than just
control, reopen (b) then -- with a caller in hand. Absent that, the checked
error is the shipped semantics and `bt-undo-to!` returning false on a stale mark
is a documented part of the contract, not a placeholder.

- **Serialization. LANDED 2026-08-29.** `serial.c` / the serializable-continuation
  path must either serialize the live trail segment alongside the continuation or
  refuse to serialize inside a `bt-scope`. Refusing is the SX1 answer; the
  diagnostic says which scope. Both halves now do: `serial_cont_to_bytes` (the
  host codec) returns NULL, and the emitted `tur_serial_cont_serialize` aborts,
  each reporting the trail depth and the count of outstanding trailed writes.

  Two things this turned up. The guard in the emitted prelude is emitted **only
  when `stdlib/trail.tur` was autoloaded into that compile** (`g_trail_autoloaded`),
  because that autoload is also what puts trail.tur's `__tur_autolink__` marker in
  the output and so pulls `src/runtime/trail.c` into the link -- gating on the
  experiment bit instead would emit a call to `tur_trail_level` that `cc` cannot
  resolve under `--no-auto-stdlib`. And writing the guard exposed a latent ABI
  bug: `tur_trail_level` / `tur_trail_depth` return `uint32_t`, while trail.tur
  declared them `:int` (`int64_t`), so the result's upper half was unspecified and
  a level could in principle read as 4294967297. Both now go through
  `tur_trail_level_i64` / `tur_trail_depth_i64`, matching how the mark is already
  packed across that boundary. Fixture: `sx1-serial-in-trail-scope-refused`.

**One candidate *genuine* multi-shot use: RT6 hint generation.** Today each
candidate hypothesis re-runs the whole chain from scratch (`run_chain`,
`refine_discharge.c:139`), which is why the RT7 memo exists. The multi-shot
shape fits exactly: capture a continuation after the hypotheses are asserted
but before the theory work, then re-enter it once per candidate -- same prefix,
many futures. Whether it beats the memo depends on how much work precedes the
branch point, which is measurable and should be measured (SX0 gives the
capture cost; `TUR_REFINE_STATS` gives the probe count) before anyone builds
it. Filed as open question 3 in section 8, not as a phase.

### 3.6 Honest scoping -- who benefits, and the layer split

Three consumers, and they are not the same. An earlier draft of the review
dismissed compiler-side use on the grounds that the solver is compile-time C
with no runtime available -- that objection **dissolved on inspection**:
full-Turmeric macros run pre-type-resolution, so there *is* a live Turmeric
runtime inside `tur` at compile time. What survives is a cost argument, and it
produces a split rather than a verdict:

| Layer | Frequency | Where it lives | Trail / continuations? |
|---|---|---|---|
| BCP / unit propagation, watch lists, simplex pivoting, union-find find/union | millions of ops/sec | C | **no** -- the canonical backtrack here is "decrement a trail index"; anything heavier is overhead on the hottest loop. WASM stack-switching is also awkward, and the zero-dependency playground runs this code |
| branch-and-bound, splitting-on-demand, strategy selection, RT6 candidate enumeration | thousands of ops/sec | Turmeric (compile-time runtime), eventually | **yes** -- nondeterminism expressed directly with `bt-scope` and one-shot capture |

The seam between the layers is the point: **the Turmeric side proposes, the C
side validates** (see 3.7). The interpreter being ~100-1000x off native for
tight mutation loops is a *category gap* for the top row -- not a deferred
optimization -- and is perfectly adequate for the bottom row.

So, the three consumers:

1. **User-level search in Turmeric** -- the direct beneficiary.
   `stdlib/logic.tur` currently threads a persistent association-list `Subst`
   with a fresh-variable counter riding in its base; `stdlib/backtrack.tur` is
   a list monad over eagerly built alternative lists. Both are the "thread
   state around it" design the reviewer named. With a trail, miniKanren's
   substitution becomes a destructive union-find with undo -- the actual WAM
   design -- and the backtracking monad can become a `bt-scope`-bracketed
   depth-first search that does not materialize alternatives. Elsewhere in the
   compiler's own problem domain, the elaborator's searches (unification with
   backtracking, typeclass instance resolution, RT4 template inference) have
   the same shape and are eventual candidates.
2. **The solver's hot layer** -- the top row of the table. It is C and stays
   C. What SX3/SX4 take from SX1 is the *design* and a small shared C utility
   (`src/compiler/trail_c.h`), so the two implementations agree on stamps,
   opt-out, and undo ordering.
3. **The solver's search layer** -- the bottom row, the genuinely new
   possibility the compile-time runtime opens. Branch-and-bound for the
   integer tail (SX7) can be *prototyped in Turmeric* against
   `tests/corpus/smtlib/` and ported to C only if the stats move. This is
   cheap to try and cheap to discard, which is the right risk profile for the
   most speculative part of the plan.

### 3.7 TCB discipline -- untrusted search, trusted checking

A `RT_VALID` verdict elides a runtime check, so everything that can produce
one is trusted compute. Today that is a few kLOC of in-house C. A
Turmeric-hosted search stage (3.6, consumer 3) would add the macro expander,
the compile-time runtime, and its dependencies to that trusted base --
potentially including Turmeric code that itself carries refinements decided by
the very solver being bootstrapped. That is not a reason to forbid it; it is a
reason to adopt the architecture every serious proof-producing solver adopts:

> **Search is untrusted; checking is trusted.** No verdict proposed by a
> search layer -- Turmeric-hosted or C -- is accepted unless its certificate
> passes an independent checker. An unverified certificate is `RT_UNKNOWN`.

The certificates are small and known:

- **Simplex:** Farkas coefficients from the infeasible row -- a linear
  combination of asserted bounds summing to a contradiction; checking is a dot
  product in exact rationals.
- **EUF:** the path through the proof forest (Nieuwenhuis-Oliveras
  proof-producing congruence closure); checking is replaying a list of merges.
- **Propositional:** the resolution chain behind a learned clause; DRAT is the
  standard format for exactly this.

The checker is a few hundred lines of C that never has to be smart, and it
collapses the TCB back to something auditable regardless of how clever the
search above it becomes. It also converts the soundness invariant from a
review discipline into a mechanical property -- which section 5 leans on hard,
because the classic CDCL(T) bug class (a subtly wrong explanation clause that
prunes the search into a silent, input-dependent wrong answer) is exactly the
kind of thing a 125-case corpus does not catch. Prior art: veriT and the
Alethe proof format; DRAT-trim for the propositional half.

Bootstrap ordering note, so nobody trips on it later: macros run
pre-type-resolution and discharge runs during elaboration, so a solver stage
written in Turmeric must be fully expanded and executable before the first
obligation of the unit being compiled exists. Practically: the stage ships
pre-compiled with `tur` (like the stdlib), it is not compiled by the unit that
uses it.

## 4. Answer to Q2 -- the capture/restore curve

### 4.1 What the literature measures

The reviewer's metric is the standard one for this design space, and the
implementation strategies are distinguished precisely by their shape on that
curve:

- **Stack copying** -- capture copies the live stack segment. Cost is linear in
  bytes of live state under the prompt; restore likewise. Simple, and the
  baseline every other strategy is measured against.
- **Segmented / chunked stacks** (Hieb, Dybvig and Bruggeman; then Bruggeman,
  Waddell and Dybvig for one-shot continuations). Capture splits the stack at
  the top frame and hands the bottom segment to the continuation -- **constant
  time**, with the copying deferred to (and paid only by) an actual re-entry.
  One-shot continuations then eliminate the copy entirely, because there is no
  second entry to protect against.
- **Heap-allocated frames / CPS** -- capture is O(1) pointer copy because
  frames are already heap objects and immutable; the cost moves to every call
  (allocation) rather than to capture. Farvardin's "Weighing Continuations for
  Concurrency" is the modern head-to-head of these three.
- Contemporary systems land in the same taxonomy: OCaml 5's effect handlers use
  segmented fibers; WasmFX's stack switching is the same idea at the Wasm
  level.

The single most useful thing the curve reveals is **the intercept versus the
slope**: a strategy with a large constant and zero slope beats a strategy with
a small constant and a real slope at some crossover point, and where that
crossover sits relative to real workloads is the whole decision.

### 4.2 Turmeric's three capture paths and their predicted shapes

This is what the harness has to separate, because they are genuinely different
curves and today nothing distinguishes them:

| Path | Where | Predicted capture cost | Predicted restore cost |
|---|---|---|---|
| DK chain slice (compiled `shift`/`reset`, effect handlers) | `cps_prompt.c`, mirrored in `tur_rt_split.c:1134+` | O(frames under the prompt) -- one `dk_copy_node` per frame, plus one `env_clone` per *owning* frame | O(frames) again on each `dk_invoke`, because invoke replays a fresh copy |
| Fiber / ucontext | `FiberBlock`, `tur_rt_split.c:1857+` | O(1) to suspend, but 1 MiB default stack per fiber allocated up front | O(1) switch |
| Cloneable cont snapshot | `tur_cloneable_cont_clone` = `tur_continuation_snapshot`, `tur_rt_split.c:1113` | O(env), one `clone_env` call | per *resume*, not per capture -- `emit_effects.c:1113` snapshots before every `(k v)` |

The DK chain is documented as "Capture is O(depth-of-slice) and unbounded -- no
16-frame ceiling" (`tur_rt_split.c:1137`). That is a slope, and the multi-shot
path pays it on *every resume*. Whether the constant is small enough that the
slope does not matter until 100 frames or 10 000 frames is exactly the
unmeasured number, and it is the number that decides 3.5(b) -- and, per 3.5,
whether a one-shot fast path needs to exist before any search layer uses
capture at all.

**Measured (SX0(a)):** the answer is neither 100 nor 10 000 -- it is **about
20**. Restore costs roughly `36 + 22*F` ns against a fiber's flat 429 ns, so
the slope starts mattering almost immediately, and the per-resume slope at
R = 8 is within 1% of R = 1, meaning there is no one-shot fast path today.
See `benchmarks/capture-curve-results.md`.

### 4.3 The benchmark, specified

**Independent variables** (sweep each, hold the others fixed):

1. `F` -- frames under the prompt: 1, 2, 4, ... 4096 (log sweep).
2. `E` -- owning-env bytes per frame: 0 (all `:copy`, `env_clone == NULL`),
   1 rc handle, 8, 64. This separates the per-node cost from the per-owner
   clone cost, which is the difference between a memcpy slope and a refcount
   slope.
3. `R` -- resumes per capture: 1 (one-shot) and 8 (multi-shot). The gap between
   these two *is* the one-shot optimization the literature reports, measured
   rather than assumed.
4. `T` -- trailed writes live under the prompt: 0, 10, 10^3, 10^5 (added by
   SX1; the axis exists in the harness from SX0 with `T = 0` so the file does
   not need reshaping later).

**Dependent variables:** ns per capture, ns per restore, bytes allocated per
capture, peak RSS. Report the fitted `a + b*F + c*E` per path, not just the
totals -- the intercept and the slope are separately actionable.

**Baselines to include in the same table**, so the curve has meaning rather
than just a shape:
- a plain function call and a plain closure invocation (the floor);
- `hamt-bench-snapshot.tur`, the existing persistent-structure snapshot cost
  (this is the "thread state around it" alternative the trail competes with,
  and the crossover between them is a real design output);
- once SX1 lands, `bt-mark`/`bt-undo-to!` over `T` writes.

**Deliverables:** `benchmarks/bench-capture-restore.tur`,
`benchmarks/bench-trail-undo.tur` (SX1), `benchmarks/run-capture-curve.sh`
emitting CSV plus a markdown table appended to
`benchmarks/benchmark-results.md`. Run compiled and `--interpret`, and under
the JIT where available -- but note that the interpreter/JIT ratio is
explicitly *not* the headline number; the per-path slope is. The
interpreter's row matters for one decision only: confirming (or refuting) the
3.6 claim that the tree-walker is adequate for a thousands-per-second search
layer.

**Method discipline**, because this repo has been bitten by it: never run the
sweep concurrently with a build or another suite. `tests/run.sh` stamps the
binary for exactly this reason; the benchmark script should do the same, and
refuse to publish a row if `./build/tur` changed mid-run.

## 5. The plan

Phases are ordered so that each one is independently landable and independently
valuable. SX0 must be first: both of its instruments produce the decision data
the later phases are gated on, and the reviewer asked for one of them "while
you're building it".

### SX0 -- measure first

Two instruments, no language or runtime change:

- **(a) The capture/restore curve** -- **LANDED.**
  `benchmarks/bench-capture-restore.tur` sweeps `F`, `E`, `R` (and carries the
  `T` column at 0) across all three paths of 4.2;
  `benchmarks/run-capture-curve.sh` fits each and writes
  `benchmarks/capture-curve-results.md` plus the raw CSV.

  **Result -- the three paths are the three shapes the literature predicts, and
  the crossover is much lower than the design assumed.** Measured on a Release
  build (a sanitized one measures ASan, and the script refuses to publish from
  one), best of three timed rounds after an untimed warm-up:

  | path | capture | restore |
  |---|---|---|
  | DK chain slice | `~23 + 17*F` ns | `~36 + 22*F` ns |
  | fiber (ucontext) | `~2060` ns, flat | `~429` ns, flat |
  | cloneable snapshot | `~8 + 0.15*E` ns | `~2` ns |

  Figures are rounded and the results file is the source of truth; across
  reruns the DK slope moves within about +/-15% and the fiber restore constant
  within about 1%. The restore crossover below is stable to the frame; the
  capture crossover is not, because fiber creation is the noisiest thing here.

  1. **The restore crossover is F ~ 20 frames.** A fiber switch costs a flat
     432 ns; the DK chain costs 41 + 17.2 per frame, so past about twenty
     frames under the prompt the fiber is cheaper *per resume* -- and the DK
     path pays that slope on **every** resume, because `dk_invoke` re-copies
     the chain (`tur_rt_split.c:1461`). The capture crossover is much further
     out -- F ~ 140-155, and it wanders across reruns -- because a fiber pays
     for a stack up front. Twenty frames is not a deep stack. Any search layer
     that resumes repeatedly over more than a shallow slice is on the wrong
     side of this line today.
  2. **`R` shows no one-shot optimization exists.** The per-resume *slope* is
     the same whether a capture is resumed once or eight times -- 22.36 versus
     22.56 ns/frame on the DK path in the checked-in run, under 1% apart, and
     the fiber constant likewise (428.8 against 425.2 ns). That is the
     measurement behind 3.5(b): there is
     no one-shot fast path to fall into, so if a search layer wants one it has
     to be built, and the curve says what it would be worth.
  3. **The plan's own cost model was mis-specified.** Section 4.3 asks for a fit
     of `a + b*F + c*E`. On a chain that is wrong: `dk_copy_node` fires
     `env_clone` once per *owning frame*, so the env term is `c*F*E`. Fitting
     the literal `c*E` produced an intercept of **-1876 ns** -- a constant no
     mechanism can have -- because the fit had to absorb 4096 frames' worth of
     64-byte copies into `b`. The results file fits `a + b*F + c*(F*E)` for the
     chain and `a + c*E` for the (chainless) cloneable path, which is the same
     formula wherever the two agree.
  4. **Per-frame cost is flat from F=32 to F=2048 and then steps up ~65% at
     F=4096** -- a chain that size stops fitting in cache. The fitted `b` is
     the flat band; past ~2048 frames the real cost is worse than the model.

  Two method notes worth carrying into SX1's `bench-trail-undo`, both of which
  produced wrong numbers before they were caught: the first pass through any of
  these paths pays page-fault and allocator-warmup costs that no later pass
  repeats (an untimed warm-up round moved the first fiber row from 425,346 ns
  to 3,172 ns), and a baseline whose result is unused is deleted outright -- the
  closure baseline read 0.33 ns per invocation, *below an empty loop iteration*,
  until the callee was made opaque to devirtualization.

  **The interpreter row the phase asks for is not obtainable from this
  benchmark**, and that is a real answer rather than a gap: every timed region
  is inline C by design, so `tur --interpret` declines the file outright
  ("inline-C not supported in interpreter mode"). The 3.6 question the
  interpreter row was meant to settle -- is the tree-walker adequate for a
  thousands-per-second search layer? -- needs a *Turmeric-level* search
  benchmark, which is exactly what SX2's `bench-logic-query` and
  `bench-backtrack-n-queens` are. That row belongs there, not here.
- **(b) Cap-hit telemetry** -- **LANDED.** `TUR_REFINE_STATS=1` now reports
  *which* cap bit and how often, plus the high-water mark of the quantity each
  cap bounds, so a cap that never fires still reports its headroom.
  `benchmarks/run-cap-sweep.sh` sweeps all three populations the phase asks for
  and writes `benchmarks/cap-sweep-results.md`.

  **Result -- both gates come back NO.** Across 411 units (124 corpus
  benchmarks, 87 in-tree refinement fixtures, 200 fuzzer-generated VCs):

  | cap | corpus peak | in-tree peak | fuzzer peak | hits |
  |---|---:|---:|---:|---|
  | `REFINE_MAX_LA_CONSTR` (512) | 42 | 12 | 9 | none, anywhere |
  | FM blow-up (same 512) | -- | -- | -- | none, anywhere |
  | `REFINE_MAX_CUBES` (64) | 40 | 4 | 8 | none, anywhere |
  | `REFINE_MAX_CUBE_LITS` (64) | 13 | 10 | 5 | none, anywhere |
  | `REFINE_MAX_EXPAND_DEPTH` (256) | 8 | 5 | 6 | none, anywhere |
  | `REFINE_MAX_LA_VARS` (32) | 9 | 9 | 7 | none, anywhere |
  | `REFINE_MAX_EUF_TERMS` (512) | 512 | 25 | 23 | 982, all on one unit |
  | `NO_MAX_SHARED` (8) | 9 | 9 | 7 | 4 units, always by exactly 1 |
  | `NO_MAX_ROUNDS` (4) | -- | -- | -- | none, anywhere |

  Three readings, in descending order of consequence:

  1. **SX4 does not start.** `REFINE_MAX_LA_CONSTR` peaks at 42 of 512 -- 92%
     headroom on the widest thing in the corpus, 98% on real code -- and the FM
     elimination never once backed off. The archived plan reserved simplex for
     "if the cap ever bites"; measured, it does not come close.
  2. **SX6 does not start.** The cube caps never fire either. The corpus's
     40-of-64 peak is `gen_mixed_sat_00003`, a generated benchmark; in-tree
     refinement code peaks at 4 cubes of 64. This is 2.1's question answered
     directly: path-sensitive VCs with real propositional structure **do not
     exist in this codebase yet**.
  3. **Not one cap hit cost a proof.** Every capped unit was one that must
     answer `UNKNOWN` regardless -- four corpus benchmarks labelled `sat`, and
     `refine-match-field-wrong`, a soundness fixture whose obligation is a
     genuine violation. The EUF column's 982 hits are all
     `qf_lra_deep_arith_chain_sat`, a 1000-deep `(+ 1 (+ 1 ...))` nesting that
     exists to regress a stack overflow -- an artifact of the regression, not a
     shape real code produces.

  The one cap with a live signal is `NO_MAX_SHARED`, and it is a small one: it
  turned away exactly one eligible term on each of four units, and the fuzzer's
  own peak sits at 7 of 8. That is SX5's "raise on evidence rather than
  caution" note becoming actionable -- see SX5. **Acted on: raised to 16 on
  2026-08-25**, so the table above is the pre-raise record; the current
  telemetry is whatever `benchmarks/run-cap-sweep.sh` last wrote.

  **Re-run 2026-09-03 at v0.43.0** (the gate re-run SX4 and SX6 each ask for
  before reconsidering): **byte-identical to the 2026-08-26 baseline** apart
  from the generated-at header -- same 124 / 85 / 200 units, same peaks, the
  same single capped unit (`qf_lra_deep_arith_chain_sat`, the 1000-deep
  regression artifact). Both gates stay shut, now on current evidence rather
  than on a nine-day-old file. Re-running it is cheap and is the right first
  move any time this plan's right-hand side comes up again.

**Size:** small. **Gate:** none (benchmarks and stats are not compiler
features).

### SX1 -- the trail primitive

**LANDED (partially -- see "what is not here" below).**

`src/runtime/trail.{c,h}` implements all four decisions of 3.2; `stdlib/trail.tur`
is the surface, behind the `backtrackable-state` experiment; `tests/unit/trail.c`
is 58 checks at the C level and `tests/fixtures/sx1-trail-basics` is the same
acceptance list through the language.

**The measurement, which is the phase's real output.** `bench-capture-restore.tur`
grew the T axis it had been reserving since SX0, so the trail now sits in the
same table as the three capture paths:

| mechanism | cost per unit of live state |
|---|---|
| trail: `bt-mark` + write + `bt-undo-to!` | **~5 ns per write** (flat, T = 8..4096) |
| DK chain slice: restore | ~23 ns per frame |
| fiber: restore | 486 ns flat, any depth |

Undoing recorded state is roughly **5x cheaper per unit of live state** than
replaying captured control -- and unlike a continuation it can be *asymmetric*,
which is the whole argument of 3.5. A continuation restores everything; a search
needs to discard what it assumed while keeping what it learned. The constant is
a nice-to-have; the asymmetry is the reason.

**Design notes worth carrying into SX3/SX4**, all of which cost a bug first:

1. **Ownership is a TRANSFER, not a copy.** Recording moves the cell's reference
   into the trail entry; undoing moves it back. The first draft cloned at both
   ends, which leaks exactly one payload per undone write -- invisible until
   something real is on the other end of the drop hook. The unit test checks a
   fake refcount to zero rather than leaving it to LeakSanitizer.
2. **Commit must roll the stamp back, and the first version did not.** This was
   filed as "the comparison must be `<` not `!=`" and that was wrong -- `<` was
   a band-aid over a real bug that shipped. A commit left the cell stamped at a
   level that had been popped; a later mark reuses that index, the stamp already
   equals the new level, so the write is not trailed and its undo **silently
   does nothing**. The worst shape a backtracking bug can have: no crash, no
   error, just a value that quietly fails to come back.

   It surfaced from mutation testing -- flipping `<` to `!=` did not fail the
   suite, which meant the invariant was not actually pinned, which meant looking
   at why. The fix is one line in `tur_trail_commit_to` (restore each entry's
   `old_stamp`), and with stamps kept in range `<` and `!=` become equivalent.
   `test_commit_then_reused_level` pins it, and is itself mutation-verified to
   fail without the fix.

   **Method note for SX3/SX4:** the original `test_commit` passed either way,
   because it undid to the OUTER mark, whose entry restored the right value
   regardless. A test that exercises a mechanism is not the same as a test that
   would notice the mechanism breaking.
3. **`g-set!` is the trail paused, not a second cell representation.** One struct,
   one set of semantics; the opt-out is the TYPE that reaches it.
4. **A mark is packed into an int64 at the language boundary**, because
   `extern-c` can express pointers, ints and bools but not a two-word struct
   returned by value. The C API keeps the struct.

**What is NOT here, and why:**

- **The `#fx{Bt}` effect row.** Not added -- and **checked 2026-08-26, it is not
  a soundness prerequisite after all.** The reason it was thought to matter is
  real in shape: the refinement solver's purity whitelist decides whether two
  occurrences of a call may be congruence-collapsed, and a function that mutates
  a trailed cell must not be. This note used to say the hole was merely
  unreachable "today" for want of callers. Made reachable and measured, the hole
  **does not exist**:

  ```turmeric
  (defn tick [c : BtCell] : int
    (let [v (bt-get c)] (do (bt-set! c (+ v 1)) v)))
  (defn probe [c : BtCell] : #refine{ r : int | (>= r 0) }
    (- (tick c) (tick c)))          ; really -1
  ```

  The solver answers **unknown** and keeps the runtime check, which then fires
  on the real value. A pure measure in the same unit is still proven, so this is
  discrimination rather than the solver giving up: `1 proven, 1 unknown`.

  Why: `rt_classify_binding_top` (`elab_fns.c:517`) is a **default-deny** walk,
  and `bt-set!` is an `extern-c` call it cannot prove pure. So any function
  touching the trail is at best UNKNOWN by construction, with no annotation.
  The declared-row veto in the same function is a second line that is never
  reached here. And `refine_purity` is a memo of that walk, not a user-settable
  claim, so there is no override to lie with.

  Pinned by `tests/fixtures/sx2-trail-measure-not-congruent`, which asserts the
  W0372 rather than stdout -- mutation-verified by making the measure pure and
  watching the fixture fail.

  **What the row would still buy** is precision and documentation, not safety:
  a way to say "this touches the trail" that is checked, instead of relying on
  a conservative walk to notice. That is worth having, but it does not gate
  SX2's engine work, and this phase should stop listing it as a prerequisite.

  **Revisited 2026-08-29, and the "checked" half does not hold up.** The trail's
  eight mutators in `stdlib/trail.tur` now carry `#fx{Bt}` -- `bt-set!`,
  `g-set!`, `bt-mark`, `bt-undo-to!`, `bt-commit-to!`, `untrailed-begin`,
  `untrailed-end`, `trail-reset!` -- but this is **declared intent, not a
  checked guarantee**, and the entry above should not be read as promising more:

  - **No compiler change was needed or made.** `#fx{...}` labels are free-form;
    `#fx{Bt}` parses and type-checks today with no registry row, no elaboration
    hook, and no new diagnostic. There was never a feature here to build.
  - **It does not ripple.** Annotating the stdlib mutators required no change to
    any caller -- not `bt-scope`, not `stdlib/backtrack-dfs.tur`, not the
    fixtures. The row is carried, not propagated as an obligation.
  - **No verdict anywhere moves.** The intended demonstration was a body the
    purity walk *can* prove pure, vetoed only by its declared row. It could not
    be produced: on every shape tried, `#fx{Bt}`, `#fx{IO}` and even
    `#fx{Unsafe}` all left the solver's answer unchanged. So the
    `row_veto` line in `rt_classify_binding_top` is real code that reads
    `effect_row`, but nothing in reach exercises it, and **no fixture can
    currently pin the row doing work.** One was not written rather than writing
    one that asserts something else and reads as if it pinned this.

  Net: the annotations are accurate and free, and they mean a later widening of
  the purity walk would find a declared row already in place rather than
  silently promoting `bt-set!` to pure. That is the whole of the value. Anyone
  picking this up should treat "make the row checked" as the actual open work --
  it is unstarted, and the annotations do not represent progress on it.

  **DONE 2026-09-05: the row is checked.** Every finding of the 2026-08-29
  revisit had one cause, and it was not in the purity walk. `effect_row_resolve`
  **silently drops any uppercase name that no `defeffect` declares**, and
  nothing declared `Bt` -- `stdlib/effects.tur` is not autoloaded, and the
  tag was never added to it anyway -- so `#fx{Bt}` resolved to `#fx{}` on every
  mutator. `--dump-effects` showed `bt-set! : #{}` all along. "No compiler
  change was needed" was true because the annotation was not reaching the
  compiler. The `#fx{IO}` control in the same experiment failed for the same
  reason (not autoloaded either), which is why every shape tried "left the
  answer unchanged".

  Three changes close it:

  1. **`(defeffect Bt [] :nil ^capability)` in `stdlib/trail.tur`** -- in the
     autoloaded module, deliberately, so it is declared wherever the row is
     written. As a capability it propagates from the declared row (the
     `#fx{FS}` mechanism, `effect_check.c`), so a `#fx{}` caller of a trail
     mutator is **TUR-E0009**, and it is exempt from the over-annotation
     warning. The discipline stays opt-in: an unannotated caller is unchecked.
  2. **`bt-scope`, `with-untrailed`, `dfs-solve` and `dfs-choose-go` carry
     the row** -- the bracket forms this note said "land with it", and the
     driver's two entry points that actually write and unwind. The goal
     constructors do not: they build a closure, and the trail is touched when
     `dfs-solve` runs it. Their thunk parameters are left without a declared
     row so a body that also prints is not rejected at the bracket.
  3. **The purity veto moved into `rt_classify_binding`** (elab_fns.c), where
     the verdict is memoized. At the top only, it was skipped on every memo
     hit -- and the memo was written first by the predicate-impurity walk, so
     it never ran at all; that is the second half of "no verdict moves", and
     it would have held for a resolved `#fx{Unsafe}` too. In the binding it is
     memo-stable and **transitive**: an unannotated wrapper around a `#fx{Bt}`
     function is UNKNOWN, not PURE. An unresolved row also vetoes (rows resolve
     in the effect pass, after this walk), which is the conservative direction.

  Pinned by `tests/fixtures/sx1-bt-row-checked` (the W0372 on a `#fx{Bt}`
  identity and on its unannotated caller, with an unannotated control that
  still proves; plus `#fx{Bt}` callers of both brackets running) and
  `tests/fixtures/errors/sx1-bt-row-pure-caller-rejected` (TUR-E0009 through
  a direct write, `bt-scope`, and `dfs-solve`). The guide has a section.

  The one residual worth knowing: the `#reads`-frame grant
  (`enc_reads_args_frozen`) *can* hand congruence to an otherwise-impure
  measure when its named arguments are frozen at the site. That is a
  user-facing trusted claim and it is the general
  [trusted-refinement-claims](trusted-refinement-claims-plan.md)
  problem rather than a trail-specific one, but a trailed cell is exactly the
  kind of state where a wrong `#reads` frame would cost a check.
- **The serialization refusal of 3.5.** Not added. Same reasoning: no serializable
  continuation currently spans a `bt-scope`, because nothing spans one yet.
- **`bt-scope` / `with-untrailed` as higher-order forms.** The surface ships the
  bracket halves (`bt-mark` / `bt-undo-to!`, `untrailed-begin` / `untrailed-end`)
  rather than combinators taking a thunk. The combinators want the effect row to
  type honestly, so they land with it.

The gate is `EXPERIMENTS[]` row `backtrackable-state`, prototype lifecycle,
introduced 0.39.0, expires 0.42.0, `opt_global` `g_opt_backtrackable_state`. It
gates the AUTOLOAD of `stdlib/trail.tur`: with the experiment off the module is
simply not there and `bt-set!` is an unknown function, which is a more honest
report than names that exist and refuse to work. `experiment_warn_if_used` fires
once per compile from the same place.

One bug fixed on the way: `tur_stdlib_prepend_forms` iterated the raw autoload
array while project mode went through the accessor, so a gated entry was honoured
in one build mode and silently ignored in the other -- the same program compiling
two different ways. Both now go through the accessor.

Original specification follows.

- **Do:** `src/runtime/trail.{c,h}` per 3.2, `stdlib/trail.tur` per the surface
  in 3.2, the `#fx{Bt}` effect row, the generational stale-mark error, the
  serialization refusal of 3.5.
- **Accept:** fixtures for value cells, write-once cells, stamp suppression of
  redundant trailing, `GCell` never appearing on the trail, `with-untrailed`,
  `bt-commit-to!`, ownership-correct undo of an rc payload under ASan/LSan, and
  a stale-mark error that is a diagnostic rather than a crash. `bench-trail-undo`
  joins the SX0 table.
- **Gate:** `EXPERIMENTS[]` row `backtrackable-state`, `plan_path` pointing
  here, `introduced` 0.36.0, `expires_at` 0.39.0, lifecycle prototype,
  `opt_global` `g_opt_backtrackable_state`, and
  `experiment_warn_if_used("backtrackable-state")` at the elaboration entry
  point -- per the project's experimental-features rule. This is exactly the
  "semantics in flux" case the gate exists for: 3.5's re-entry question is open
  on purpose.
- **Size:** medium. The runtime is a few hundred lines; the effect row and the
  ownership glue are where the work is.

### SX2 -- make the stdlib search paths use it

**The gate is answered: the trail pays for itself, decisively.** The measurement
is `benchmarks/bench-logic-subst.tur`, results in
`benchmarks/logic-subst-results.md`.

| bindings | persistent ns/op | trailed ns/op | speedup |
|---:|---:|---:|---:|
| 1 | 209.5 | 18.7 | 11.2x |
| 8 | 194.1 | 11.5 | 16.8x |
| 64 | 369.9 | 20.7 | 17.9x |
| 512 | 782.2 | 22.7 | 34.4x |

**There is no crossover.** The expectation going in was that a persistent list
would win at small n -- it is O(1) to extend and free to backtrack -- and lose
only once its O(n) lookup dominated. It never wins. So the conditional in "why
it is in this plan at all" resolves the other way: SX3/SX4 **should** build on
the shared trail rather than proceeding as plain C.

**Re-measured 2026-09-03 at v0.43.0, after the whole SR programme landed.** The
table above predates SR1, SR2a/b, SR3 and the SR4 default flip -- every
representation change that touches `logic.tur`'s `Term`/`Subst`/`Stream` -- and
the benchmark had bit-rotted into a build error in the meantime (the trail
natives are compiler-predeclared now, so its own `extern-c` rows collided).
Fixed and re-run; full table in
[benchmarks/logic-subst-results.md](../../benchmarks/logic-subst-results.md).

**The gate verdict is unchanged and the margin widened by an order of
magnitude: 11x at n=1 to 215x at n=512.** Nothing here reopens SX2.

The second finding above -- "a large per-operation CONSTANT, not the linear
scan, is what dominates" -- **no longer holds on the default.** The persistent
curve is flat only to n=4 and cleanly linear from n=8 (2.0x per doubling from
n=64). The constant was the allocator and the SR programme took most of it
away. But the new slope is mostly not asymptotics either: an A/B against
`TUR_SR4_RECURSIVE_CARRIER=1` on one box puts **6.8x of the n=512 cost on the
by-value recursive-sum lowering**, which copies 120 bytes per chain link where
the carrier moved one word, two thirds of it redundantly. Filed as
[sr4-byvalue-recursive-sum-walk-copies-per-link.md](sr4-byvalue-recursive-sum-walk-copies-per-link.md).
`TUR_SR1_SUM_BYVALUE=0` on top moves nothing, which is the control: these types
are all self-recursive, so SR4 is the entire effect.

**Nothing on the compiler side of this plan is affected.** SX3's EUF, SX4's
simplex and SX0(b)'s cap sweep live in `src/compiler/refine_solver_*.c` -- C
compiled by CMake, not Turmeric lowered by `tur` -- so no representation change
reaches them, and their gates stand exactly as recorded. The SR programme moves
the SX2/stdlib half of this plan only.

The shape carries a second finding. The persistent path's cost is roughly FLAT
at ~190-230 ns/op from n=1 to n=16 and only climbs past n=128, so what dominates
at every size a real query reaches is a large per-operation CONSTANT, not the
linear scan. That constant is the thing worth attacking in the persistent path,
and it is not diagnosed here: a microbenchmark that tried to isolate it was
optimized away entirely -- the same trap as SX0(a)'s closure baseline -- and a
number from a folded loop is worse than no number. Filed under
[solver-hot-structures-linear-scans.md](solver-hot-structures-linear-scans.md) (resolved 2026-09-28).

**The two benchmarks this phase names do not measure what it needs.** Both
`bench-logic-query.tur` and `bench-backtrack-n-queens.tur` are self-contained
inline-C simulations of a backtracking monad; neither loads `stdlib/logic.tur`
or `stdlib/backtrack.tur`, so "run both paths" on them would have measured a
hand-written C list against a trail and answered a much kinder question than the
one asked. `bench-logic-subst.tur` measures `logic.tur`'s REAL `Subst` --
`SBind`, `logic-walk`, `subst-lookup` -- which is what would actually change.

**What landed:** `tur_uf_*` in `src/runtime/trail.c` -- an indexed, trailed
variable->term map built on write-once cells, which is also the shape SX3's EUF
union-find wants -- plus the head-to-head benchmark and its results.

**What did NOT land:** the engine swap itself. Putting the trailed substitution
behind `logic.tur`'s `Goal`/`Subst` API is not a drop-in: `Subst` is a persistent
value threaded through a `Stream` monad that FORKS it, and a trail is a stack
discipline that cannot be forked. That is exactly why the phase also calls for a
`bt-scope`-bracketed depth-first driver in `stdlib/backtrack.tur` -- the driver
has to change with the representation. Sequencing it after the measurement was
deliberate: the plan says not to build it if the trail loses, and until now
nobody knew. It does not lose, so the driver is the next increment, and it is
where the `#fx{Bt}` effect row belongs too (see SX1) -- that is the first caller
the refinement solver's purity whitelist can actually see.

**LANDED 2026-08-26: the driver.** `stdlib/backtrack-dfs.tur` -- CPS goal
combinators over trailed cells (`dfs-succeed` / `dfs-fail` / `dfs-and` /
`dfs-or` / `dfs-guard` / `dfs-set` / `dfs-choose-int` / `dfs-solve`), typed
end to end as `(fn [(fn [] bool)] bool)` per this phase's no-`:int` note.
Reify-at-success is the API: `dfs-solve`'s `on-solution` runs with bindings
live and must copy out what it keeps; the whole run is bracketed in
`bt-scope`, so the fixture pins cells reading their PRE-search values
afterwards.  It is a sibling file rather than an addition to `backtrack.tur`
as originally specced, deliberately: the driver references `bt-scope` /
`bt-set!`, so folding it in would break every `(load "stdlib/backtrack.tur")`
under the default (experiment-off) configuration.  Fixture: `sx2-dfs-driver`
(reify+undo, or-ordering, cut, lvar refusal-as-failure).

Two spellings inside the driver are dictated by checkers, worth knowing
before extending it: the combinators use the `bt-mark`/`bt-undo-to!` HALVES,
not the `bt-scope` bracket, because wrapping `(g1 k)` in a scope thunk
CAPTURES `k` and capturing a fat closure moves it (TUR-E0005) -- calling is
non-consuming, capture is not.  And recursion is written pass-`k`-through
(`dfs-choose-go`) rather than goal-returning, because of the second bug
below.

**The head-to-head the phase asked for**
(`benchmarks/bench-dfs-queens.tur`, results in
`benchmarks/dfs-queens-results.md`): N-queens on both shipped surfaces.
**No crossover in range -- the list monad is 4-11% ahead at N=4..7.**  Both
prior results stand together: the trail's 11-34x was against persistent-state
LOOKUP; enumeration against the monad's inline-C cell walk is a different
fight, and the driver pays a fat-dispatch per combinator layer per node.
What the driver buys is scale and reification: the list path's packed-int
board is structurally capped at N=7, while the driver runs N=8/9/10 (92 /
352 / 724 solutions, all matching the known sequence) and reads any
solution out of live cells.

**Two compiler bugs surfaced by this work, filed:**

- `mbind` called DIRECTLY with a non-capturing lambda segfaults (its untyped
  `:int` param takes a thin code pointer, its body reads `fat[0]`); the
  typeclass `bind` path fat-boxes and works, which is why the in-tree corpus
  never hit it.  Recorded in the benchmark's comments; the benchmark uses
  `bind`.
- A SELF-recursive call whose fn-typed result feeds a `^fat` parameter
  segfaults; the same call through a one-line forwarder works --
  [self-recursive-fn-returning-call-into-fat-sink](../reported/self-recursive-fn-returning-call-into-fat-sink.md).
  The natural recursive-combinator shape, so anyone composing goals
  recursively hits it; the benchmark carries the forwarder workaround.

**A measurement trap, re-learned:** the first Release numbers came from a
STALE `build-release/tur` predating two landed fixes -- it silently dropped
the `with-untrailed` call (the statement-position bug) and mis-emitted the
slab preamble, producing wrong answers fast.  Rebuild Release before
believing any number from it; the results file carries the warning.

**Corrected 2026-08-26: the row is NOT a prerequisite.** A trail-mutating
measure is already refused congruence by the default-deny purity walk, with no
annotation -- measured, and pinned by
`tests/fixtures/sx2-trail-measure-not-congruent`. See SX1's note for the probe
and the mechanism. The row remains worth having for precision, but the driver
no longer waits on it, which removes one of this phase's two coupled pieces.
**The row landed checked on 2026-09-05** (SX1's note has the record);
`dfs-solve` and `dfs-choose-go` carry it.

**Re-checked 2026-08-26, after lazy streams landed in `logic.tur`.** Two of the
statements above have moved, one helpfully and one not.

**The fork got a shape a trail can use.** The blocker as written is that `Subst`
is threaded through a `Stream` monad that FORKS it while a trail is a stack
discipline. `logic.tur` now has `disjoined-dfs` / `st-append-dfs`: depth-first
*and* lazy, so the left branch is fully exhausted before the right one is
touched. That is exactly mark / explore / undo-to-mark -- the discipline a trail
wants -- and it did not exist when this phase was written. The default
`disjoined` interleaves, which is the *worst* case for a trail (it alternates
branches, so every alternation would need an undo and a redo), so a trailed
engine would be built against the dfs pair, not against the default.

**But there is a second obstacle the phase does not name, and it is the harder
one: solutions ESCAPE the search.** `run-logic` returns a `Stream` of `Subst`
and the caller reads it afterwards -- `reify-walk` walks a term through
`(first-state results)` once `run-logic` has already returned. Today that is
fine: a `Subst` is a persistent `SBind` chain, so it outlives the search that
built it. Under a trailed union-find the bindings live in cells that backtracking
UNDOES, so every answer would be empty by the time the caller looked at it.

The standard fix is to reify at success time inside the driver -- copy each
answer out of the trailed store before backtracking past it -- which changes
what `run-logic` returns. That is an API change to a shipped module, and it is
the real reason "put the trailed substitution behind `logic.tur`'s existing
`Goal`/`Subst` API" is not a drop-in. The phase's own instinct to build the
driver in `stdlib/backtrack.tur` instead sidesteps it, and on this evidence that
is the right call.

**Two more things worth knowing before starting:**

- `tur_uf_*` (the piece that DID land) has exactly one caller in the tree:
  `benchmarks/bench-logic-subst.tur`, via `extern-c`. Nothing in `stdlib/` uses
  it, so the engine work starts from an unexercised primitive.
- `stdlib/backtrack.tur` -- where the driver goes -- is written entirely in
  `:int`: `mplus`, `mbind`, `bt-cons` and `run-backtrack` all take and return
  `:int` for what are really stream handles. Per CLAUDE.md's no-lazy-`:int`
  rule the new driver should not extend that, which means the phase carries a
  typing decision for its own surface that the original spec does not mention.

Original specification follows.

- **Do:** a trailed union-find substitution behind `stdlib/logic.tur`'s existing
  `Goal`/`Subst` API (the persistent path stays as the default until the
  benchmark says otherwise); a `bt-scope`-bracketed depth-first driver in
  `stdlib/backtrack.tur` alongside the list monad.
- **Accept:** `benchmarks/bench-logic-query.tur` and
  `bench-backtrack-n-queens.tur` run both paths and the crossover point is
  recorded. Existing fixtures keep passing on the persistent path.
- **Why it is in this plan at all:** it is the honest test of 3.6(1). If a
  trailed union-find does not beat a persistent assoc list on
  `bench-logic-query`, the primitive is not paying for itself and SX3/SX4
  should proceed as plain C with no shared utility.

### SX3 -- incremental EUF (S1i)

**LANDED 2026-08-26.** `euf_mark` / `euf_undo_to` over one state, backed by
`src/compiler/trail_c.h` -- a header-only value trail carrying two lessons
from the runtime trail rather than relearning them: entries record slot
INDICES (the arrays are arena-grown, so a trailed pointer can dangle into the
superseded allocation), and the level counter is MONOTONIC (undo does not roll
it back; a rolled-back level plus stale-high stamps silently suppresses
trailing -- the exact bug the runtime trail's commit path shipped).  Every
`parent[]` write is trailed, path compression included, so the merge history a
Nieuwenhuis-Oliveras proof forest needs stays recoverable -- the
representation question this phase was told to keep open.

Both rebuild sites converted: `refine_s1_decide` and `refine_s3_decide` hold
one state and bracket each cube with mark/undo; each cube still starts from
the empty partition, so verdicts and cap telemetry are identical to the
rebuild path by construction.  `LaState` stays per-cube -- making it
incremental is SX4, which is parked.

**Accepted as specified:** the corpus output is BIT-IDENTICAL between
`TUR_REFINE_EUF=rebuild` and the incremental default (full-output diff, 138
lines), all 93 `refine-*` fixtures agree under both modes including
`TUR_REFINE_STATS` counts, and the full suite is green.  Compile time on the
widest fixture (`refine-crossing-path-conditions`) is a wash -- 184 vs 189
ms/check -- which is what the phase predicted: the payoff is the deleted
per-cube allocation churn, not speed.  Unit tests: `test_euf_mark_undo`,
mutation-verified against both failure modes (a silently-skipped trail entry,
and the stale-stamp level reuse), each caught independently.  The first draft
of the stale-stamp check did NOT catch its mutation -- undo truncated the
re-registered term, refreshing its stamp -- so the landed test registers its
probe vars below the marks, which is the only shape the trap actually bites.

Original specification follows; its second-payoff note stands.

**A second reason to want this, measured 2026-08-25.** `no_cube_unsat` calls
`euf_new` and `la_new` per cube, up to 64 cubes per obligation, all into the
unit arena and never reclaimed -- ~267 KB of theory state per compile on a
generated 400-obligation file, and ~613 KB on the heaviest in-tree refinement
fixture. Replacing the rebuild with mark/undo over one state deletes most of
that as a side effect. It is not a reason to start SX3 on its own (the number is
a flat constant, not a growth curve), but it is a real second payoff once the
phase is taken for its cost argument. A per-entry arena was measured as the
alternative way to reclaim it and came back NO --
[per-entry-arena-gate.md](history/per-entry-arena-gate.md).

- **Do:** extend the `euf_*` seam with `euf_mark` / `euf_undo_to`, backed by
  `src/compiler/trail_c.h` (the C sibling of SX1, same stamp discipline).
  Replace the per-cube `euf_new` in `refine_s1_decide` and `no_cube_unsat` with
  assert/mark/undo over one state. Pick the union-find representation with the
  Nieuwenhuis-Oliveras proof forest in mind -- not built here, but the merge
  history it needs must be recoverable, so SX6b's `euf_explain` is an addition
  rather than a second rewrite.
- **Accept:** identical verdicts on all 125 corpus benchmarks and all
  `refine-*` fixtures -- this phase changes *cost*, not answers, and a verdict
  diff is a bug. `TUR_REFINE_STATS=1` shows the same proven/refuted/unknown
  counts. Measure compile time on the widest fixtures.
- **Gate:** none needed if verdicts are bit-identical; keep the old path behind
  `TUR_REFINE_EUF=rebuild|incremental` as a test seam (env-only, like
  `TUR_REFINE_NO_DISCHARGE`) so the corpus can be replayed against both.

### SX4 -- incremental simplex (S2b)

- **Do:** the Dutertre--de Moura solver behind the existing `la_*` seam, with a
  bound stack and *no* basis restoration (3.4). Add `la_mark` / `la_backtrack`,
  and produce **Farkas coefficients** on infeasibility from day one -- they are
  the explanation SX6b needs and the certificate 3.7's checker verifies, and
  retrofitting them is harder than emitting them.
  Note the naming collision: `la_push` is already taken by the static
  constraint-adder at `refine_solver_arith.c:249` -- do not overload it.
- **Numerics:** the existing exact int64 rational layer is partly reusable, but
  pivoting grows coefficients. Decision to make up front: bignum rationals, or
  keep int64 with the current overflow checks and a disciplined
  overflow -> `RT_UNKNOWN` degrade. The second is in keeping with everything
  else in this solver and is the recommendation; revisit only if telemetry
  shows real obligations degrading.
- **Accept:** every obligation S2 proves today is still proven (superset
  property; a regression here is a real regression), the `REFINE_MAX_LA_CONSTR`
  cap stops biting on the corpus's wide benchmarks, strict inequalities via the
  paper's delta-rational treatment, and every unsat verdict carries a
  Farkas certificate that an independent check accepts.
- **Size:** the design of record already estimates 2--4 weeks for this, and that
  estimate looks right. This is the single largest phase.
- **Gate on starting at all:** SX0(b). The archived plan chose FM over simplex
  deliberately, with simplex reserved for "if the cap ever bites". If the
  telemetry says it does not bite, SX4 is a performance project with no
  user-visible payoff and waits behind SX6a.
  **Measured: it does not bite.** `REFINE_MAX_LA_CONSTR` peaked at 42 of 512
  across all three populations and fired zero times; the FM blow-up path never
  triggered. SX4 is **parked** on this evidence. Re-run
  `benchmarks/run-cap-sweep.sh` before reconsidering -- the number to watch is
  the `la_constr` peak, and the thing that would move it is wider arithmetic
  obligations than anything in the tree today.

### SX5 -- incremental Nelson-Oppen (S3i)

- **Do:** run the equality exchange over the marked/undoable S1i and S2b states
  instead of rebuilding both per cube. The exchange loop itself is unchanged.
- **Accept:** as SX3 -- identical verdicts, lower cost. `NO_MAX_SHARED` and
  `NO_MAX_ROUNDS` can then be raised on evidence rather than caution.
- **The `NO_MAX_SHARED` raise is DONE** -- 8 -> 16, landed 2026-08-25, ahead of
  the rest of SX5 and independent of it. SX0(b) found it turning away eligible
  terms on four units, every time by exactly one (9 eligible against a cap of
  8), while `NO_MAX_ROUNDS` never ran out anywhere; it was the only cap in the
  solver with a live signal.

  It was taken with the measurement this bullet asked for rather than as a
  drive-by, because the cost is real: the exchange is quadratic in the shared
  set and each `la_entails_eq` runs Fourier-Motzkin twice, so 8 -> 16 is 4x the
  pair work on every S3 cube, paid by every obligation that reaches S3 rather
  than only by the ones near the cap.

  **Measured, Release build, before landing:** verdicts identical on all 125
  corpus benchmarks (per-benchmark diff, not just the totals) and all 89
  in-tree refinement fixtures; `bash tests/run.sh` unchanged at 2694/0; corpus
  replay 0.0926s -> 0.0923s and the fixture population 1.566s -> 1.552s, both
  inside noise. The round budget was not traded into -- the unit that hit the
  cap now reaches its fixpoint with `NO rounds out 0 (of 4)`. The regenerated
  `benchmarks/cap-sweep-results.md` shows `no_shared` at **0 hits across all
  three populations**, 44-56% headroom.

  **It bought headroom, not capability:** all four capped units had to answer
  `UNKNOWN` regardless, so no verdict moved. Full numbers and method in
  [history/no-max-shared-raise.md](history/no-max-shared-raise.md).

  This leaves S3 with **no cap carrying a live signal**, which removes the one
  standalone reason to start SX5 early. The rest of SX5 (running the exchange
  over marked/undoable S1i/S2b states) still trades cost for cost and still
  waits on SX3/SX4.

### SX6 -- boolean structure beyond small DNF (S4)

The line the design of record explicitly does not cross ("no DPLL(T) engine").
The review reframed the cost usefully: the SAT core is the cheap part
(Tseitin CNF from the already-existing NNF is days; a MiniSat-grade CDCL core
is ~1.5-2 kLOC of C with paper and source as an effective spec); the
*incremental, backtrackable, explaining theory seam* is the real cost, and the
*testing burden* -- a subtly wrong explanation clause prunes the search into a
silent, input-dependent false verdict -- dominates even that. Ballpark from
the review: 2-4 person-months to make CDCL(T) work, 6+ to trust it with
eliding checks. So this phase is two steps with a stepping stone, and the
stepping stone may be where it permanently stops:

**SX6a -- lazy cubes, then offline lazy SMT.**

- ~~Step zero, available today with no new machinery: *stream* cubes off an
  explicit stack instead of materializing up to 64, so a time budget replaces
  the `overflow` flag. No semantic change; retires the cube-count cap as a
  cliff.~~ **RETIRED 2026-09-03 on measurement: there is nothing to stream.**
  The premise is "materializing up to 64", and nothing in any measured
  population comes near it. Counting cube-set width directly (a temporary
  probe in `refine_cubes_build`, over `tur smt` on the corpus and `tur check`
  in-tree), the widest SINGLE cube set anywhere is **16** -- on three corpus
  benchmarks (`qf_lra_ite_int_numerals_{sat,unsat}`, `qf_lia_ite_nested_sat`)
  -- and the heaviest in-tree refinement fixture
  (`refine-crossing-path-conditions`) peaks at **4**, producing 71 cubes total
  across 48 builds in 117 expand frames. An explicit stack would save neither
  memory nor time over an array that never exceeds sixteen entries, and the
  cap it would retire as a cliff is the one SX0(b) already found never fires.
  This item is not deferred; it is answered.

  **The same probe found the thing actually worth changing here, and it is not
  a boolean-structure phase at all:** the chain expands the IDENTICAL DNF once
  per stage, because S0/S1/S2/S3 each open with their own
  `refine_cubes_build` on an unchanged VC. The first row above reads `builds=4
  cubes=64 peak=16` -- one 16-cube DNF, expanded four times, three of them
  discarded. Filed with a fix direction (build once in the chain driver and
  pass it down; do NOT cache on the `RefineVC`, which SX8b's `pop` makes
  unsound) as
  [../reported/refine-chain-expands-the-same-dnf-four-times.md](../reported/refine-chain-expands-the-same-dnf-four-times.md).
  It is a simplification rather than a speedup -- the solver is compile-time
  noise either way -- so it waits for someone touching this code.
- Then offline lazy SMT: Tseitin-encode the (already-NNF) refutation formula,
  run a plain CDCL SAT core as a **black box**, hand each full boolean model --
  which *is* a cube -- to the existing **non-incremental** S1-S3, and on
  theory conflict add a blocking clause and re-solve. This removes the
  exponential DNF blowup with **none** of the explanation machinery, no
  incremental theories, and no changes to the stages at all. It is the honest
  stepping stone: every piece of it survives into SX6b if SX6b ever happens.
- **Accept:** verdicts never flip against the DNF path where both decide;
  corpus/telemetry `UNKNOWN`s attributable to cube caps shrink; the
  source-level fuzzer stays clean across at least six seeds; the whole thing
  remains dependency-free (the SAT core is in-tree C).
- **Gate:** `EXPERIMENTS[]` row `solver-lazy-smt` while in flux; graduation
  deletes the row per the project rule. Gate on *starting*: SX0(b) shows the
  cube caps biting on real obligations.
  **Measured: they do not.** Zero cube-cap hits across all three populations;
  in-tree refinement code peaks at 4 cubes of 64. SX6 is **parked**, and SX6b
  is parked behind it. ~~The step-zero item above (streaming cubes off an
  explicit stack) is still a fine cleanup on its own merits, but it is now a
  cleanup rather than a cap-driven necessity.~~ **And step zero is retired
  too, 2026-09-03** -- see the item above: the widest cube set in any measured
  population is 16, so there is no materialization to stream. With that gone,
  nothing under SX6 is available to build; the whole phase waits on its gate
  reopening.

**SX6b -- CDCL(T) proper.** Only if SX6a's blocking-clause loop measurably
thrashes (many iterations rediscovering the same theory conflict) on real
obligations.

- **Needs:** the theories incremental (SX3/SX4/SX5), backtrackable (the
  trail), and explaining -- `euf_explain` via the SX3 proof-forest hooks,
  `la_explain` via SX4's Farkas certificates. Learned clauses and activity
  scores are the never-undone rows of 3.4. Splitting-on-demand for S3's
  non-convex integer case becomes "emit the split as a lemma clause", which is
  the modern move precisely because the learning then works across the split.
- **Precondition, not afterthought:** the 3.7 certificate checker, wired so a
  bad explanation is *rejected at solve time* (degrading to `RT_UNKNOWN`)
  rather than trusted. This is what makes the dominant bug class survivable.
- **Accept:** obligations `UNKNOWN` for propositional reasons shrink to none
  on the corpus; no verdict flips; fuzzer clean; every conflict clause checks.
- **Do not:** attempt a competitive SAT solver. The stopping condition is "no
  real obligation is `UNKNOWN` for propositional reasons", not benchmark
  parity with a real SMT solver.

### SX7 -- integer completeness, the long tail (S2c)

> **First slice LANDED 2026-09-05, in C, not via the Turmeric prototype this
> phase prescribes** -- see
> [solver-integer-tail-plan.md](solver-integer-tail-plan.md). The Omega test's
> two exact steps (gcd normalization of inequalities; the divisibility test
> and unit-coefficient substitution on equations) went straight into
> `refine_solver_arith.c` behind the unchanged `la_*` seam, because at ~150
> lines of exact int64 arithmetic they were cheaper to write than to
> prototype. Alongside: the encoder axiomatizes `(/ a k)` / `(mod a k)` by a
> literal and asserts squares non-negative, and the counterexample search is
> budgeted by evaluations rather than capped at three variables. The trigger
> was not a cap hit -- every cap still sits on its 2026-09-03 headroom -- but
> nine ordinary probes, four of which the solver could not decide, all
> outside every shape the three swept populations contain. That blind spot
> in the instruments is that plan's Phase 4. What remains of SX7 proper --
> Pugh's sigma-substitution, and branch-and-bound / the dark shadow on the
> inequality side -- keeps the prescription below.

- **Do:** branch-and-bound over the SX4 simplex, depth-limited, `UNKNOWN` past
  the limit. Optionally Gomory cuts. Closes the integer non-convexity hole that
  makes S3 incomplete on disequality case-splits today.
- **How:** per 3.6(3), *prototype the search in Turmeric first* -- it is the
  bottom-row, thousands-per-second layer -- and run it against
  `tests/corpus/smtlib/` with verdicts checked through the 3.7 seam. Port to C
  only if the stats move. This is deliberately the first consumer of the
  untrusted-search/trusted-checker architecture, on the smallest search
  problem in the plan.
- **Gate:** depth cap is a constant, not a flag. Deferrable indefinitely.

### SX8 -- external interrogation surface

Expose the solver to callers *outside* the compile pipeline. The review kept
asking "measure it", "replay it", "check it against an external solver by
hand" -- every one of those is easier when the solver answers queries
directly, and the pieces already exist in-tree: `refine_smtlib.c` serializes a
VC *out* to SMT-LIB2, and `tests/unit/refine_corpus.c` already contains a
complete SMT-LIB2 reader *in* (s-expr parser, term builder onto `RefineVC`,
`:status` handling) that today only the ctest harness can invoke. SX8 is
mostly relocation and plumbing, in three tiers that ride the other phases:

**SX8a -- `tur smt` (batch CLI) and the JSON obligation dump. LANDED.**

Both halves shipped, and the loop between them closes: a compile emits each
obligation's VC as SMT-LIB2 text, and that text feeds straight back into
`tur smt` and reaches the same verdict via the same stage. That round trip is
pinned by a fixture, and it is the two-way differential-testing door the tier
existed for -- an external harness can now drive any solver against `tur` in
both directions without `tur` linking one.

What landed:

- **The reader moved.** The s-expr parser, term builder and `:status` handling
  are out of `tests/unit/refine_corpus.c` and into
  `src/compiler/refine_smtlib.c`, which is now the whole SMT-LIB2 seam in both
  directions (`refine_smtlib_read` / `refine_smtlib_emit`). The corpus harness
  shrank to what it always was underneath -- fork-per-benchmark isolation, the
  label check, the time budget, the tally -- and replays bit-identically: 125
  benchmarks, 68 proved, 56 sat-correct, 1 skipped, 0 soundness failures.
- **`tur smt <file.smt2>`** runs the standard S0-S3 chain and prints
  `sat` / `unsat` / `unknown`, the deciding stage, and the model when the
  bounded search finds one. Exit codes mirror the answer (0 unsat, 1 sat,
  2 unknown, 3 error) so a shell harness branches on `$?` without parsing
  stdout -- deliberately not the 0-is-success convention, because `unsat` is an
  answer rather than a success.
- **`--dump-refine=json`** emits one record per obligation: location,
  predicate as written, verdict, deciding stage, memo hit, counterexample,
  the replayable `vc_smtlib`, and the caps that bit *for that obligation*.
  It is stripped in the global argv pre-pass, so it works on `check` as well
  as `emit-c` -- the records come from the elaboration both share, and forcing
  a codegen run to read a report would be the wrong shape.

**Acceptance, checked:** the corpus replay expressed as 125 `tur smt`
invocations agrees with the ctest harness -- 124 agree, 1 refused as outside
the fragment (the same one the harness skips), 0 soundness failures, 0
disagreements. The JSON round-trips through `python3 -m json.tool`. Both are
exercised by fixtures (`sx8a-tur-smt`, `sx8a-refine-json-dump`).

**Two things the tier turned up that the plan should carry forward:**

1. **The per-obligation cap deltas are new machinery, not a re-read.** SX0(b)'s
   counters are a per-compile summary, so "which obligation hit the cube cap"
   is not recoverable from them -- each obligation now snapshots and subtracts
   around its own decision. That is what lets a record say `caps_hit: {cubes: 4}`
   next to `verdict: unknown`, which is the single most useful pairing in the
   dump and exactly what SX6's gate would want per-site evidence from.
2. **`decided_by` had to be recorded, not derived.** Nothing on the obligation
   said which stage answered; the chain loop now names it. `tur smt` reports
   the same string, which is why a replayed VC can be checked for reaching the
   same verdict *the same way* rather than merely the same answer.

**Deferred within the tier, deliberately:** the record carries the predicate as
written and the VC as SMT-LIB2, but not a separate rendering of the hypotheses
-- the SMT-LIB text already states them precisely and a second, prettier copy
would be a second thing to keep true. `help:` hints are likewise not in the
record: they are generated during diagnostic emission for obligations that
report, and plumbing them into every record is SX8b-shaped work with no
consumer yet.

Original specification follows.

Independent of everything else; can land right after SX0.

- `tur smt <file.smt2>`: lift the corpus harness's reader out of `tests/unit/`
  into `src/compiler/refine_smtlib.c` (making that file the reader *and*
  writer, which also lets the corpus harness shrink to a driver), run the
  standard chain, print `sat` / `unsat` / `unknown` plus which stage decided,
  the model when the bounded search found one, and -- once SX4/SX6b exist --
  the certificate. Exit codes mirror the answer so shell harnesses can branch.
  This closes the loop the internals guide already gestures at: today you can
  dump a VC *to* "whatever solver you like"; after SX8a an external harness
  can differentially test any solver against `tur` in both directions without
  `tur` ever linking one.
- `tur emit-c --dump-refine=json <file.tur>` (or a `tur refine-report`
  spelling, settle in-phase): one JSON record per obligation -- location,
  predicate source, hypotheses, the VC as SMT-LIB2 text, verdict, deciding
  stage, counterexample, `help:` hints, memo hit, and which caps (if any) were
  hit. This is deliberately the same stream SX0(b)'s telemetry wants: one
  emitter, two consumers (a human/tool reading a build, and the cap-hit
  sweep). Editors and CI read it; the LSP can surface "show me the VC / why
  UNKNOWN" as a hover or code action from the same records later, without a
  new protocol.
- **Gating:** none. Per the experimental-features rule, diagnostic and dump
  surfaces (`--dump-*`) are explicitly outside the `EXPERIMENTS[]` regime.
  Mark the JSON schema explicitly unstable (a `"schema": 0` field) until SX9.
- **Accept:** the corpus replay can be expressed as `tur smt` invocations and
  agrees with the ctest harness on all 125 labels; the JSON dump round-trips
  through `python3 -m json.tool`; both are exercised by fixtures.

**SX8b -- incremental queries. LANDED 2026-09-03.**

`tur smt` now runs a script as a SESSION: `(push [n])` / `(pop [n])` scope the
assertions, each `(check-sat)` is answered where it appears, `(get-model)`
reprints the last witness, `(exit)` ends the session, and
`tur smt --interactive` reads the same commands from stdin and answers as they
arrive. The exit code is the last answer.

**The premise below is wrong, and this is the correction.** The tier was
specced as "after SX3/SX4 land marks... `(push)` / `(pop)` map onto `euf_mark`
+ `la_mark` / undo", making it "the *external witness* that the trail works".
They do not map onto the trail, and no amount of SX4 would change that: each
`check-sat` runs the chain from the current assertion set, which rebuilds the
DNF cubes, and **adding one hypothesis changes the cube set wholesale** -- so
there is no solver-side mark that spans two checks. `euf_mark`/`euf_undo_to`
bracket cubes *within* one check, which is SX3 and is already exercised by
every compile. What is incremental here is the ASSERTION SET: a `pop` restores
exactly the hypotheses in scope at the matching `push`, with nothing re-read or
re-translated. That is what the protocol means by incremental, and it is worth
having on its own; it is just not a witness for the trail.

**So the acceptance criterion was replaced with one that bites.** The property
that makes an incremental interface worth having is that it answers the SAME as
the non-incremental one, so `tests/fixtures/sx8b-smt-push-pop` is a
DIFFERENTIAL check: every answer a stacked script gives is compared against a
flat script with the same assertions in scope, run as a separate process with
no stack at all. A `pop` that kept a popped assertion, or dropped a live one,
fails there and would pass any single-script test.

**What landed, in one sentence of design:** one command executor serves both
doors, and they differ in a single `stack_ok` bit -- batch
(`refine_smtlib_read`, unchanged for the corpus) says `push`/`pop` are outside
the fragment because it folds a script into one assertion set; sessions say
they are protocol. Keeping one executor is what makes the two doors agree about
the accepted fragment by construction rather than by parallel maintenance.

**Two limits, both deliberate and both documented in the guide:** only
hypotheses are scoped (a `declare-fun` survives its `pop` -- sound in this
direction, since an unconstrained variable cannot turn `sat` into `unsat`, and
truncating `n_vars` would leave hash-consed terms pointing at reusable slots);
and a script with no `(check-sat)` is still decided once at the end, which is
SX8a's contract and what every corpus benchmark relies on.

**Validation:** the corpus replay is unmoved (125 benchmarks, 68 proved, 56
sat-correct, 1 skipped, 0 soundness failures -- the SX8a figures exactly), the
`sx8a-tur-smt` fixture passes byte-identically, `bash tests/run.sh` is 2780/0,
and `ctest -R refine` is 4/4. The new fixture agrees byte-for-byte between the
Release and the ASan Debug build.

**Still not built, and still needing what they always needed:**
`(get-unsat-core)` wants the 3.7 conflict explanation and `(get-proof)` wants
the certificate; both are SX6b-shaped and parked behind it.

**SX8c -- in-language and playground access. HALF LANDED 2026-09-04.**

- ~~A compile-time API for the 3.6 search layer: expose the discharge seam to
  Turmeric compile-time code as `(solver/check hyps goal)` returning
  valid/invalid/unknown plus model.~~ **NOT BUILT, and deliberately: it has no
  consumer and it carries an unmade design decision.** Its own justification
  here is that it "*is* the seam SX7's Turmeric-side prototype needs" -- and
  SX7 is item 6 of the recommended order, marked "deferrable indefinitely",
  with nobody having started it. Building a seam to fit a prototype that does
  not exist is how a surface gets shaped wrong and then has to be kept.

  The undecided part is the signature, not the plumbing. `hyps` and `goal` as
  written imply TURMERIC-LEVEL terms, and there is no Turmeric representation
  of a `VCTerm` -- the only translation into one that exists is the SMT-LIB2
  reader, from text. So the API is either a new term-building surface (real
  design work, and SX7 is the only thing that could say what shape it wants)
  or it is `(solver/check "<smtlib>")`, which is the string door below bound as
  an interpreter native -- cheap, but not what this bullet asks for, and
  calling it done would be overclaiming. Left for SX7 to drive.

- **A WASM export -- `turi_smt_check` in `src/web/wasm_glue.c`, next to
  `turi_doc_lookup`. LANDED.** The premise checked out: `compiler/refine_*.c`
  are already in `TUR_CORE_SOURCES` (src/CMakeLists.txt:199-208), so every
  browser session has been shipping S0..S3 all along with no way to see it.

  The semantics are `tur smt`'s to the letter -- the same SX8b session reader
  (so `(push)`/`(pop)` scope assertions and each `(check-sat)` is answered
  where it appears), the same chain in the same order, the same bounded model
  search, and the same "a script with no `(check-sat)` is decided once at the
  end". Two doors onto one solver that disagreed would be a second solver
  wearing the first one's name, and the browser is exactly where nobody would
  notice. Returns JSON (`{"schema":0,"results":[...]}`, `schema` 0 while
  unstable, matching `--dump-refine=json`); a script outside the fragment gets
  an `error` key and an EMPTY `results` array -- refused whole, never partially
  parsed. Read-only as the spec requires: nothing reachable from it touches
  elaboration or discharge, so no answer can elide a runtime check.

  **Tested natively, which is the point.** `tests/wasm_glue_smt_unit.c` (ctest
  `tur_wasm_glue_smt_unit`) links `libturi_wasm` and calls the real function
  against the real chain -- the `tur_wasm_glue_lang_unit` precedent, and the
  reason a defect here does not have to be found by a person with an
  Emscripten toolchain in the way of every diagnosis. Every case is one whose
  CLI answer is pinned by `sx8a-tur-smt` or `sx8b-smt-push-pop`, so the two
  doors are checked against each other rather than only against themselves.
  It earned its keep immediately: the first version returned `Buf.data`
  straight back as a C string, and `Buf` carries a length and does NOT
  NUL-terminate -- valid JSON with heap garbage trailing the closing brace,
  which in a browser panel would have read as a rendering bug.

  **Not verified end to end, and this is the honest limit:** `emcc` is not
  installed on the box this landed from, so the Emscripten link was never run.
  The symbol is confirmed present (`nm libturi_wasm.a` shows
  `T turi_smt_check`) and `_turi_smt_check` is added to `EXPORTED_FUNCTIONS`,
  but the first person with a toolchain should run `just wasm` once. A wrong
  name in that list fails the link loudly rather than silently, which is the
  failure mode to expect if it is wrong at all. The `tur_refine_wasm` ctest
  (which compiles the refine TUs under emcc) skips on this box for the same
  reason.

**Scope honesty, so the surface never overpromises:** `tur smt` accepts the
corpus subset of SMT-LIB2 over `QF_UFLIA`/`QF_UFLRA`, answers `unknown`
freely, and parity with a production SMT solver is a non-goal (SX6b's
stopping condition applies here too). The reader must keep the corpus
harness's discipline of erroring on anything it would otherwise silently
drop: a partially parsed assertion set weakens `unsat` claims, which is the
one dishonest failure mode a query surface can have.

### SX9 -- docs and graduation

- Update
  [refinement-solver-internals-guide.md](../guides/refinement-solver-internals-guide.md)
  (the stage table, the caps table, the new seam operations, the incrementality
  story), write a `docs/guides/backtrackable-state-guide.md` for the SX1
  surface, and cross-link from
  [backtracking-guide.md](../guides/backtracking-guide.md) and
  [logic-programming-guide.md](../guides/logic-programming-guide.md). Document
  the `tur smt` subcommand and the JSON obligation schema (stamping it
  `"schema": 1`) in a `docs/guides/solver-query-guide.md`.
- ~~Graduate `backtrackable-state` (delete the row, behavior unconditional) once
  3.5's re-entry question is settled and SX2 has a benchmark answer.~~
  **DONE 2026-08-29.** Both conditions were met: SX2's benchmark answered
  decisively (no crossover, 11-34x on `Subst` lookup) and 3.5's re-entry
  question is DECIDED above. The row is gone from `EXPERIMENTS[]`,
  `g_opt_backtrackable_state` is retired, and `stdlib/trail.tur` is an ordinary
  member of `autoload_files_`.

  Two things the graduation turned up that were not on this list. Making the
  autoload unconditional put `trail.tur` in front of the **turi native-parity
  ratchet**, which requires every compiled-path auto-load to be in the
  interpreter prelude or carry a written rationale. The trail had no turi
  natives at all, so it was carved out and the gap filed -- then **closed the
  same day**: `wk_register_trail_natives` shims all 18 bindings as direct calls
  into the already-linked `src/runtime/trail.c`, so the interpreter and the
  compiled path share one trail rather than two that can drift. Five fixtures
  dropped `requires.compiled` and now run under both harnesses with
  byte-identical output, including the `bt-depth` counts that pin the stamp
  discipline. Record:
  [trail-tur-has-no-turi-natives](https://github.com/turmeric-lang/turmeric/blob/main/docs/archive/trail-tur-has-no-turi-natives.md).
  And prepending trail.tur to every compile moved **148 codegen snapshots**,
  regenerated in the same change per the fixture rule.

  ~~Still open in this phase: graduate `solver-lazy-smt` on the SX6a corpus
  criterion.~~ **Moot, corrected 2026-09-03:** there is no `solver-lazy-smt`
  row to graduate. SX6a never started (its gate came back shut), so the row it
  would have registered was never written -- `grep solver-lazy-smt src/` is
  empty. This line becomes live again only if SX6a's gate reopens.

  **Still open in this phase, and actually open:** the internals guide's
  incrementality story was stale for a release cycle after SX3 landed -- the
  S1 section still described the rebuild-per-cube state, `TUR_REFINE_EUF` was
  documented nowhere but here, and the caps table still read `NO_MAX_SHARED`
  8 against the source's 16. Fixed 2026-09-03. The `"schema": 0` -> `1` stamp
  on the `--dump-refine=json` record is the remaining SX9 item; `tur smt` and
  the JSON dump are documented in the internals guide rather than a separate
  `solver-query-guide.md`, which is where they read better and is treated as
  closing that deliverable.

  **The `"schema": 1` stamp landed 2026-09-05**: `--dump-refine=json` and
  `turi_smt_check` both write 1, the note reads "stable since SX9", and the
  rule is that an additive change keeps the number while a removed or
  retyped key bumps it.  Schema 0 was the same shape flagged unstable.

  ~~Move this plan to `docs/archive/` when the last phase lands, per the
  archiving rule.~~  Moved 2026-09-05.

### Recommended order

This mirrors the review's own next-steps list, with the interrogation
surface slotted where it is cheapest:

1. ~~**SX0**, both instruments~~ -- **both done.** (b) reshaped items 4 and 5
   below; (a) supplied the crossover SX1's design turns on, and its two method
   traps (untimed warm-up, and a baseline the optimizer deletes) carry straight
   into SX1's `bench-trail-undo`.
2. ~~**SX8a alongside or immediately after SX0**~~ -- **done.** Every phase
   after it now has a public query door for its acceptance tests, plus two-way
   differential testing against external solvers. SX3/SX4's "identical
   verdicts" criteria can be checked through `tur smt` rather than only through
   the ctest harness, and SX6's gate can read per-obligation cap attribution
   out of the JSON dump.
3. ~~**SX1**~~, **then SX2** (re-checked 2026-08-26: the fork now has a
   trail-shaped variant in `disjoined-dfs`, but answers escaping the search is a
   second obstacle the phase never named -- see SX2) -- SX1 answered the opt-out question with a shipped
   design instead of a promise: three granularities, and a measurement showing
   record-and-undo is ~5x cheaper per unit of state than capture-and-restore.
   **SX2 is now the live phase**, and it is the honest test -- if a trailed
   union-find does not beat a persistent assoc list on `bench-logic-query`, the
   primitive is not paying for itself. SX2 is also what should carry the
   `#fx{Bt}` effect row, since it brings the first caller the solver's purity
   whitelist can actually see.
4. ~~**If SX0(b) says the cube caps bite: SX6a**~~ -- **it says they do not.**
   Parked; revisit only if a later sweep moves the cube peaks off the floor.
5. ~~**SX3-SX5**~~ -- SX4 is **parked** on its own gate (the LA cap does not
   bite). SX3/SX5 were never cap-gated -- they trade cost, not answers -- so
   they stay available, but with SX6b parked behind SX4 there is no longer a
   consumer pulling them forward; take them only if measured compile time asks.
   The one exception worth doing on its own was the `NO_MAX_SHARED` raise
   SX0(b) turned up, and it is now **done** (8 -> 16, no verdict moved, no
   measurable cost; see SX5). No cap in the solver carries a live signal any
   more.
6. **SX7 prototype in Turmeric early**, via the SX8c seam (it needs only
   SX4's seam or even today's S2 for a first cut against the corpus), C port
   last or never. **SX6b** last of all, and only if SX6a thrashes -- which now
   requires SX6a to exist first, which requires its own gate to reopen.

## 6. Explicitly not doing

- **Not** linking an external SMT solver, in any build. The Z3 oracle was
  retired for reasons that still hold, and the corpus plus the source-level
  fuzzer replaced it with something that runs everywhere, including WASM. The
  SX6a SAT core, if built, is in-tree C.
- **Not** building a competitive SAT solver (see SX6b's stopping condition).
- **Not** making captured continuations capture *state*. A continuation
  captures control; the trail is a separate mechanism with a separate scope
  discipline. Conflating them is how this design gets slow in a way that cannot
  be undone later.
- **Not** putting the hot layer in Turmeric. BCP, watch lists, pivoting, and
  union-find operations run millions of times a second; per 3.6 they are C on
  every target, WASM included.
- **Not** removing the caps. Every cap stays; incrementality changes what a cap
  costs, not whether it exists. An `UNKNOWN` is always available and always
  sound.
- **Not** trailing by default in the compiler's arena-allocated VC structures.
  Terms are immortal within a query by construction; that is a feature.
- **Not** shipping a general-purpose SMT solver CLI. `tur smt` is an
  interrogation window onto the solver `tur` already contains -- the corpus
  subset of SMT-LIB2, `unknown` as a first-class answer, no parity ambitions.
- **Not** trusting any Turmeric-hosted search stage to decide. Per 3.7 it may
  only propose; the C checker decides, and an unverified certificate is
  `RT_UNKNOWN`.

## 7. Risks

| Risk | Mitigation |
|---|---|
| Incremental EUF/simplex changes a verdict | Corpus + fixtures are verdict-identical by acceptance criterion; keep the rebuild path behind an env test seam for differential replay |
| A subtly wrong explanation clause prunes the search into a silent, input-dependent false verdict (the classic CDCL(T) bug class) | The 3.7 certificate checker validates every conflict at solve time -- a bad explanation degrades to `RT_UNKNOWN` instead of being trusted; differential replay against the DNF and lazy-SMT paths; the corpus is necessary but known-insufficient here and is not the safety argument |
| TCB growth from a Turmeric-hosted search stage | Untrusted-search/trusted-checker split (3.7); a Tur stage proposes, never decides |
| Trail undo mis-handles rc ownership -> leak or double-free | ASan/LSan is on for the compiled fixture path; add a fixture with an rc payload written and undone in a loop |
| Multi-shot resume across a stale mark corrupts state | Generational marks, checked at `bt-undo-to!` (3.2(4)); the alternative is a silent wrong answer |
| SX4 / SX6 are weeks-to-months of work with no user-visible payoff | Both are gated on SX0(b) cap-hit telemetry before starting; SX6a's blocking-clause loop must measurably thrash before SX6b starts |
| Simplex coefficient growth overflows int64 rationals | Disciplined overflow -> `RT_UNKNOWN` (today's policy, kept); bignums only if telemetry shows real degrades |
| Fixture snapshot churn | Regenerate in the same PR as the codegen change, per the project rule; SX1 is the only phase that touches codegen at all |
| The benchmark measures the wrong thing under load | Serialize the sweep, stamp the binary, refuse to publish rows if it changed mid-run |
| The query surface (JSON schema, SMT-LIB subset) hardens into a compatibility contract before the solver settles | Schema versioned and explicitly unstable through SX8a-b; stabilized (`"schema": 1`) only at SX9, alongside the guide |

## 8. Open questions to take back to the reviewer

1. ~~Multi-shot re-entry across a trail scope (3.5): checked error, or
   state-snapshotting `call/cc*`?~~ **ANSWERED 2026-08-29: the checked error,
   permanently.** The SX0 curve settled the cost half -- a snapshot is at best
   at parity with replaying the writes, and is paid at every capture rather
   than only on the branch taken -- and the semantics half went the same way:
   a symmetric snapshot restores the learned clause away, which is the shape a
   solver specifically does not want. Declined rather than deferred, with a
   falsifiable revisit trigger (measure RT6, question 3). Full record in 3.5.
2. ~~Is a *write-once* cell flavor (`LVar`) worth its own type, or should
   everything be a value cell with the stamp doing the optimization? The WAM
   says yes; a smaller API says no.~~ **ANSWERED by SX1 as shipped, recorded
   2026-09-05:** the WAM's way, but as a *flavor* rather than a type.
   `bt-lvar-new` allocates a write-once cell that is still a `BtCell` --
   `bt-set!` refuses a second binding and returns `false`, undo returns it to
   unbound, `bt-bound?` reads the state. One cell struct, one trail, one set
   of accessors; the write-once discipline is a per-cell mode, not a second
   representation. Pinned by `sx1-trail-basics` and the driver's
   "lvar refusal-as-failure" case in `sx2-dfs-driver`. This question was
   still listed as open because nobody struck it, not because it was.
3. RT6 hint generation as the one genuine multi-shot consumer: capture after
   hypotheses are asserted, re-enter once per candidate. Does it beat the RT7
   memo? Depends on the work ahead of the branch point -- measure (SX0 curve
   x `TUR_REFINE_STATS` probe counts) before building.
4. For SX6b: is theory propagation worth it at this scale, or is
   explain-and-learn enough given obligations of a few dozen atoms? Related:
   the review notes the modern treatment of non-convex splits is to emit them
   as SAT lemmas so learning works across them -- adopted in SX6b's design,
   worth confirming it holds at this problem size.

## 9. References

**In-tree**

- [refinement-solver-internals-guide.md](../guides/refinement-solver-internals-guide.md) -- the current pipeline, caps, seams, diagnostics.
- [refinement-types-guide.md](../guides/refinement-types-guide.md) -- the user-facing feature.
- [refinement-types-plan.md](refinement-types-plan.md) -- design of record; S0--S4 and RT1--RT7 phase names, the S2b/S2c/S4 tail, the Z3 retirement criteria.
- [delimited-control-operators-guide.md](../guides/delimited-control-operators-guide.md), [effects-system-guide.md](../guides/effects-system-guide.md), [backtracking-guide.md](../guides/backtracking-guide.md), [logic-programming-guide.md](../guides/logic-programming-guide.md), [experimental-flags-guide.md](../guides/experimental-flags-guide.md).
- Source: `src/compiler/refine_*.{c,h}`, `src/runtime/cps_prompt.{c,h}`, `src/runtime/generated/tur_rt_split.c`, `stdlib/{logic,backtrack,stm,ref}.tur`.

**External -- backtrackable state**

- Warren Abstract Machine trailing and conditional trailing; see the ECLiPSe and BinProlog architecture papers for the CLP value-trail extension and its redundant-trailing problem: <https://ar5iv.labs.arxiv.org/html/1012.4240>, <https://arxiv.org/pdf/1102.1178>, <http://eclipseclp.org/reports/ECRC-95-11.pdf>.
- B. Dutertre and L. de Moura, "A Fast Linear-Arithmetic Solver for DPLL(T)", CAV 2006 -- the bound stack, and the decision not to restore the basis: <https://yices.csl.sri.com/papers/cav06.pdf>.
- MiniSat's trail / `trail_lim` / backjumping, and what survives a backjump: N. Een and N. Sorensson, "An Extensible SAT-solver", SAT 2003; <https://github.com/niklasso/minisat/blob/master/minisat/core/Solver.cc>, <https://www.cs.cmu.edu/~mheule/publications/JSAT7_11_vanderTak.pdf>.
- G. Nelson and D. Oppen, "Simplification by Cooperating Decision Procedures", ACM TOPLAS 1(2), 1979.
- J. Harrison, *Handbook of Practical Logic and Automated Reasoning*, CUP 2009, ch. 2-5 -- NNF/DNF, congruence closure, Nelson-Oppen, with source.

**External -- proofs and certificates (3.7, SX6b)**

- R. Nieuwenhuis and A. Oliveras, "Proof-Producing Congruence Closure", RTA 2005 -- the proof forest behind `euf_explain`.
- G. Tseitin, "On the Complexity of Derivation in Propositional Calculus", 1968 -- the CNF encoding SX6a uses.
- The veriT solver and the Alethe proof format; N. Wetzler, M. Heule, W. Hunt, "DRAT-trim: Efficient Checking and Trimming Using Expressive Clausal Proofs", SAT 2014 -- prior art for untrusted-search/trusted-checker.

**External -- capture/restore cost**

- R. Hieb, R. K. Dybvig, C. Bruggeman, "Representing Control in the Presence of First-Class Continuations" -- stack segments, constant-time capture: <https://www.cs.tufts.edu/~nr/cs257/archive/kent-dybvig/stack.pdf>.
- C. Bruggeman, O. Waddell, R. K. Dybvig, "Representing Control in the Presence of One-Shot Continuations" -- eliminating the copy for one-shot capture: <https://www.cs.tufts.edu/comp/150VM/modules/archive/kent-dybvig/one-shot-continuations.pdf>.
- W. Clinger, A. Hartheimer, E. Ost, "Implementation Strategies for First-Class Continuations" -- the taxonomy the three paths in 4.2 fall into: <https://link.springer.com/article/10.1023/A:1010016816429>.
- K. Farvardin, "Weighing Continuations for Concurrency" -- modern head-to-head measurement of the strategies: <http://manticore.cs.uchicago.edu/papers/farvardin-masters.pdf>.
- "Continuing WebAssembly with Effect Handlers" (WasmFX) -- segmented-stack switching in a contemporary runtime: <https://arxiv.org/pdf/2308.08347>.
