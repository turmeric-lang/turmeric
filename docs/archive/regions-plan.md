---
title: Regions (RM3) -- a declared lifetime for values with no unique owner
category: Plan
description: A scope form over the arena that already ships, so a persistent structure whose nodes have no unique owner is reclaimed by generation instead of per node. The answer RM2 needs and cannot supply itself; conservative by construction, so a shape the escape walk cannot prove means "no saving", never "use-after-reset".
---

# Regions (RM3)

> **Archived 2026-09-05.** Graduated and on by default; every increment
> (R1-R5) and every graduation item below has landed, so this is a record.
> The one open thread it leaves is item 2's list of result shapes the static
> escape walk still refuses, which is a standing one-shape-at-a-time widening
> rather than a phase; the latest batch (parametric monomorphs by argument
> substitution, and the malloc-backed collections by element type --
> `region-scope-parametric`) is recorded under item 2.  The reclamation plan
> (`reclamation-plan.md`, RM2) owns what is left of the spine
> residue.

**Status: GRADUATED 2026-09-05 -- ON BY DEFAULT.** `--enable=regions` is
retired (a lingering enable is a `TUR-W0063` no-op for one minor line);
`TUR_REGIONS=0` is the bisection hatch that restores the pre-graduation build,
and `tests/run-regions-seam.sh` -- inverted at graduation, per the flags guide
-- keeps that off path green. R1-R5 landed 2026-09-04 as a prototype; R5 held
it there on one blocker, and the four graduation requirements below were then
closed in order (1, 2, 3 by work; 4 by the flip itself). The R5 decision text
is kept as the record of why it waited.

**Where the runtime comes from, since the flip.** The first CI run of the
default-on build failed everywhere the emitted C is compiled without
`libturt_runtime.a` -- a project build, a `--shared` library, the REPL's spice
cache, a bare `cc` of `tur emit-c` output -- with `undefined reference to
tur_region_shutdown`, because region.c lived only in the archive. The rc<T>/GC
runtime had already solved that exact problem with the DEDUP-4b archive
posture, and regions now follow it: `emit_closure_fat_runtime` pastes
`src/runtime/region.h` verbatim (declarations, every TU), and
`emit_region_runtime_bodies` pastes `arena.h`, `arena.c` and `region.c`
verbatim into the owner TU (`#ifdef TUR_RT_OWNER` in shared mode; a `.so`
takes `TUR_RT_API = TUR_RT_LOCAL`, hidden visibility) unless
`rt_global_from_archive()` says the archive is on the link line. The sources
are embedded into the compiler at build time by
`cmake/embed_region_runtime.cmake` (hex byte arrays, `region_rt_embed.h`), so
there is one implementation, not a replica that drifts. The S2 split forces
the archive posture and never carries the bodies. The multi-module executable
link, which chose the archive posture and then never named the archive, now
adds `-lturt_runtime` like the single-file path.

RM3 of [reclamation-plan.md](reclamation-plan.md). Read that plan's RM2
section first -- this phase exists because RM2's question has no answer at RM2.

## The problem this solves

A persistent recursive structure has **no unique owner by construction**.
`(SBind v t rest)` shares `rest` with every older chain, and backtracking
depends on that sharing, so "is this the last reference to this node?" is a
runtime fact. RM1's scope-exit rule cannot reach it (the nodes escape their
constructor -- that is what a spine is), and per-node free needs ownership the
emitter does not have.

Measured cost of leaving it (2026-09-04):

- Per-node spine boxes are **~990 of the 1790 bytes** remaining in the RM1
  leak sweep, the largest real category once one fixture's hand-written test
  scaffold is out of it.
- It **grows**: a 64-link `Subst` chain leaks 64 boxes, and 100 rounds of an
  8-link chain leak 800, not 8. A program that builds and discards recursive
  values in a loop grows without bound.

## The shape

Do not ask who owns a node. Ask **when the whole generation dies.**

For a solver that is one query. For a parser it is one parse. The boundary is
not something this phase invents -- `bt-scope` (stdlib/trail.tur) already
brackets exactly it, and the reclamation plan already said so: "not region
inference; it is one `arena_reset()` at a call site that already exists."

## What already ships, and is why this is the tractable phase

- **`arena_reset`** (src/runtime/arena.c) -- O(slabs) rewind that keeps the
  backing memory. In a Debug build it **poisons** the reclaimed bytes, so a
  value that outlives its region crashes loudly under ASan instead of reading
  stale-but-mapped data. That is this phase's worst risk, already
  instrumented, before a line of it is written.
- **`arena_owns`** -- the guard the reclamation plan requires on every free
  path, so a pointer into region memory is never handed to `free()`.
- **A precedent with the same shape, already running.** turi's value-pool
  scratch/permanent split (`src/turi/eval.c`,
  `turi-value-pool-scratch-promotion-plan`) does reset-with-promotion: walk
  the escapees out, then rewind. Its conservatism rule is the one this phase
  adopts verbatim:

  > Correctness never depends on catching every shape: a missed shape means
  > "this eval does not shrink", never "use-after-reset".

  The code is the interpreter's and does not transfer; the discipline does,
  and it has been load-bearing in this tree before.

## The safety rule, stated once

**A region that cannot prove every escaping value safe to relocate does not
rewind.** No partial rewinds, no best-effort. That turns the failure mode from
"silent wrong answer" into "no saving", which is the only trade that makes a
lifetime mechanism shippable behind a flag.

Corollaries:

- Every free path consults `arena_owns` before `free()`.
- The Debug poison stays on. It is the backstop that makes a missed shape
  loud in the suite rather than latent in a user's program.
- A fixture asserts the **value** read back across the boundary, never merely
  that the program builds. That is the SR4 lesson and the reclamation plan
  already says it applies here with more force.

## Increments

**R1 -- the gate and the plumbing (this commit).** `EXPERIMENTS[]` row with
every descriptor field, `g_opt_regions`, `experiment_warn_if_used` at the
elaboration entry point. No behaviour change: with the flag off nothing moves,
and with it on the entry point warns and does nothing yet. Landing the gate
first is what CLAUDE.md requires for a user-visible feature, and it means every
later increment is measurable against a flag rather than a rebuild.

**R2 -- an allocation generation. LANDED 2026-09-04.** Spine-node allocation
routed to the innermost open generation, and to `malloc` outside one. No
escape analysis and no rewind: pure plumbing.

`tur_region_alloc_or_malloc` is the routing point, so the emitted constructor
needs ONE call site rather than a branch -- it allocates by generation inside a
region and exactly as it does today outside one. The consequence a caller must
carry is that the result is not necessarily region memory, which is why
`tur_region_owns` has to gate every free path rather than the allocation being
assumed.

**Two ctor emitters, and only finding one is how this nearly shipped wrong.**
A `:heap` ADT's node is emitted by the base ctor (`emit_module.c`,
`emit_adt_typedef_and_ctors`) AND by the monomorph ctor (`types.c`). The first
pass changed only the base emitter, and `ctor_Cons_Cons__int` -- the ctor the
leak sweep actually blames -- kept its plain `malloc`. Caught because the
verification asked the emitted C rather than trusting the edit. This is
verbatim the standing habit RM1 wrote down after the null-None mirror ("when a
representation change lands in a ctor or temp emitter, grep for the other
emitter before calling it done"), and it cost a round anyway; the two sites now
cross-reference each other by name.

`region.c` and `arena.c` also had to join `TURT_RUNTIME_SOURCES`, not just the
compiler's own object list: an emitted program LINKS the routing helper, so a
definition only the compiler can see is an undefined reference at the fixture's
link step. With the flag off nothing references it and the linker never
extracts the member -- the same reasoning the rc/gc members carry.

*Validation:* `tests/run-regions-seam.sh` (ctest `tur_regions_seam`) asserts
that `--enable=regions` with no region open changes nothing OBSERVABLE across
six spine-carrying fixtures -- output equality, not that both arms build, since
a routing bug returning uninitialised memory would still link. That is the
property R2 is built to have and the one a later increment is most likely to
break by accident.

**R3 -- the escape check and the rewind. LANDED 2026-09-04 (runtime half).**

`tur_region_note_escape(p)` records a value crossing out of the innermost
generation; `tur_region_pop_checked(depth)` reclaims only when no noted escape
pointed INTO it, and retires otherwise. It returns which it did, so a caller
can report whether a region paid for itself.

**Reclaiming rewinds rather than releases.** `arena_reset` keeps the slabs and
the arena is pooled for the next push, so a per-query region inside a loop pays
for its slabs once -- and the reset is what gets the Debug poison, which
`arena_free` would not. The unit test pins the reuse (`second == first`), not
just that reclaim happened.

### What the runtime check does and does not establish

This is the part to read before extending it. The check proves the escaping
pointer ITSELF does not point into the generation. **It proves nothing about
what that pointer transitively reaches** -- a malloc'd struct whose field
points at a region node passes and would dangle after a rewind.

So it is the SECOND lock. The first is static, and lands with R4 where there
is a form to attach it to: a region reclaims only when its result TYPE cannot
transitively reach a region-allocated node, which is a compile-time question
with a decidable conservative answer. The runtime check then catches the direct
case cheaply and makes a static mistake loud rather than silent. Neither alone
is the safety argument, and neither should be removed on the strength of the
other.

Erring the right way is what the tests assert: a region-owned escape must BLOCK
the rewind, and a heap escape must NOT -- one direction is unsafe, the other
makes the check useless.

### The backstop is verified, not asserted

The claim this phase rests on is that a value outliving its generation crashes
loudly rather than reading stale-but-mapped data. Checked directly: a canary
that reads reclaimed region memory reports

```
ERROR: AddressSanitizer: use-after-poison on address 0x531000000818
READ of size 1 ... #0 in main canary.c:10
```

`tests/check-region-poison.sh` (ctest `tur_region_poison`) is that canary, kept
because a backstop that silently stops firing -- an ASan flag dropped from the
Debug build, `TUR_DEBUG_ARENA_POISON` turned off, `arena_reset` changed to skip
the poison -- looks exactly like a program with no stragglers. Same reasoning
as `check-cc-warn-ratchet.sh`, and the same failure it was written for.

**R4 -- wire `bt-scope`. LANDED 2026-09-04.** Not a new surface form: the
solver's existing bracket became the region boundary, so a caller opts in by
using the bracket it would use anyway. `emit_value` opens a generation around a
call to `bt-scope` and closes it after the call's hoist temp, and the CPS
emitter's `CT_TAILCALL` cps->direct arm does the same.

**Two emit paths, and the second was found by measuring.** A `bt-scope` inside a
CPS-lowered function never reaches `emit_value`, so the direct-path hook alone
left the NATURAL spelling -- `(defn one-round [n] (bt-scope (fn [] ...)))` --
with no region at all: the fixture read 909 live blocks with the flag on and 909
with it off. Both sites share one predicate pair
(`emit_binding_is_region_scope` / `emit_region_scope_reclaims`) rather than two
copies of the static walk. The CPS `CT_LETCALL` arm deliberately has NO copy: the
bracket was probed in tail position, as an arithmetic operand, and bound twice in
a `let`, and every one lowered to a tailcall, so a transcription there would be
untested safety code -- worse than a missed saving, which is all it would cost.

### The static lock, which is the half R3 could not have

`region_type_reaches_node` (emit_expr.c) walks the call's RESULT type and
refuses unless it can prove the type cannot transitively reach a
region-allocated node. Scalars and `cstr` pass; a `:heap` ADT fails on sight; a
non-heap ADT is walked through its type arguments and every constructor field,
with a **self-recursive field's NULL `full_type` read as "reaches"** -- which is
exactly the spine, so a recursive result is refused. Pointers, refs, closures,
structs, containers and type variables are refused outright: R4 needs none of
them to say no, and refusing costs a saving rather than correctness.

An unproven result emits `tur_region_pop` (retire), which is byte-for-byte what
a flag-off build does.

### Both locks are load-bearing, and that was checked rather than argued

R3's header claims the runtime note is a second lock, not decoration. R4 has the
case that proves it: a carrier-ERASED `:heap` pointer is spelled `:int`, so the
static walk sees a scalar and clears the rewind. Only
`tur_region_note_escape` catches it. Deleting the note from
`tests/fixtures/region-scope-escape-refused`'s emitted C turns it into

```
ERROR: AddressSanitizer: use-after-poison ... READ of size 8 ... #0 in chain_hysum
```

so the fixture is a live test of the lock and not a shape that happens to work.

### region-lock-hardening (2026-09-06): the result is not the only way out

The two locks above answer one question -- can the RESULT reach the
generation? -- and a day after graduation three programs answered a different
one. Each was a segfault on the default build and correct under
`TUR_REGIONS=0`:

1. **A store.** `(with-region (fn [] (do (vec-push! v (build n 0)) 1)))` --
   the result is an `:int`, both locks clear it, the generation rewinds, and
   the outer vec holds a dangling node.
2. **An erased field.** `(with-region (fn [] (HI n (build n 0))))` with
   `(defdata HoldsInt (HI :int :int))` -- the static walk admits a record of
   scalars, the runtime note only ever saw the top-level word (and a by-value
   aggregate is not a word), and the second field was a node.
3. **An erased element.** A `(Vec int)` result whose elements are
   `(:: (Link ..) :int)`. Same shape as 2, one container deeper.

The section above says "it proves nothing about what that pointer
transitively reaches", and every one of these is that sentence coming due.
The fix keeps both locks and widens the runtime one from "the result word"
to "every word that leaves the generation":

- **The store-side lock.** Every primitive that writes a caller's word into
  memory that can outlive a bracket notes it through one emitted macro,
  `TUR_REGION_NOTE(word)`: stdlib inline-C (`vec-push!`, `vec-set-o!`,
  `mutmap-set!`, `bt-set!`, `g-set!`, `atomic-store!/swap!/cas!`,
  `gen-arr-push!`, `grid-set!`, `rcvec-push!`), the HAMT setters (host
  runtime, so `tur_hamt_set` calls `tur_region_note_escape` directly), the
  emitter's `set!` on a global or shared cell, `set-field!`, `set-deref!`,
  the closure-env fill, the two malloc'd ctor-box emitters and the types.c
  monomorph twin, the element-box helpers, and `rc/of`. The macro is
  `((void)0)` under `TUR_REGIONS=0`, so the seam canary still sees no region
  symbol on the off arm.
- **The erasure note.** An ascription whose source type reaches a node and
  whose target type does not (`:int`, `Any`, `ptr<void>`) is where the
  static walk goes blind, so it is treated as the escape: the value is noted
  there. The typed style never takes this path; the erased style retires
  where it used to rewind unsoundly.
- **Aggregate results by their words.** `tur_region_note_escape_words` reads
  every aligned word of a by-value result; a scalar compared as an address
  is not region memory, an erased pointer is.
- **Owner flagging.** The note flags whichever LIVE generation owns the word,
  not just the innermost: a store fired inside a nested bracket for a value
  the OUTER generation allocated must block the outer rewind.
- **Per-thread stack, process-wide ownership.** The generation stack is
  `__thread` (as the trail and the panic state already were), so a worker's
  nodes stop landing in a generation the main thread has open; a registry
  under a spinlock keeps `tur_region_owns` true across threads, consulted
  only when another thread has ever opened a region.
- **Skipped pops retire.** A pop shallower than the stack retires the
  generations above it (a panic unwound through their brackets) instead of
  refusing and leaving the stack jammed on a stale innermost. Latent today
  -- both catch paths pop before the propagation check -- and pinned in
  `tests/region_unit.c`.

`tests/fixtures/region-escape-via-store` (nine stores, including the nested
owner case and a captured closure) and `region-escape-via-erasure` (the three
result shapes) read every value back after the pop and run on both arms
through `tests/run-regions-seam.sh`. `bench-regions-subst`, the typed
workload R4 was priced on, keeps its rewinds.

**And then fuzzed.** The three shapes above were found by hand, one at a
time, which is the wrong rate for a lock with this many doors.
`tests/regions-fuzz-src.py` (ctest smoke `tur_regions_fuzz_src`) generates
programs of random brackets -- every result shape the walk admits or
refuses, every hooked store, the erasure, nested brackets with and without
the owner case, `with-region` and `bt-scope` alike -- reads each value back
after the pop on the ASan-compiled default arm and under `TUR_REGIONS=0`
against a predicted stdout, and checks the runtime's `TUR_REGION_STATS=1`
rewind / retire counts against the generator's model of which brackets may
rewind. Two properties, because the second is the one a safety fuzzer never
asks: a bracket with no escape must still rewind. The first sessions (seeds
1 and 2, 180 programs, 1,080 brackets) found no region defect; what they
found was the model learning that `bt-scope` undoes a `bt-set!` at its pop,
which is the trail doing its job.

What this did not close, at first, was a store the emitter never sees: a user
inline-C body writing a node into its own `malloc`'d cell. That was filed as
`docs/archive/region-escape-through-unhooked-stores.md` with a documented
contract as the proposed fix -- and both halves of that were wrong.

**The inline-C parameter note (2026-09-06, same day).** The report called the
inline-C class `#fx{Unsafe}` territory needing a contract. It is neither
exotic nor unfixable. A typed node handed to a hand-written body --

```turmeric
(defn cell-set! [c : ptr<void> v : Link] : nil ```c *(int64_t *)c = (int64_t)(intptr_t)v; ```)
(defn stash [c : ptr<void> n : int] : int
  (with-region (fn [] (do (cell-set! c (Link n 0)) 1))))
```

-- has a scalar result, clears both locks, **rewinds**, and the read after the
pop printed garbage and exited 0. A silent wrong answer on the default build,
with no ascription trickery in it, and the poison did not trap because the
pooled arena had already been reused.

The fix is at the CALLEE, not the call site: a call site cannot note an
argument without re-emitting it, but in an emitted inline-C function every
parameter is already a plain C identifier. So the note goes once at body
entry, and one rule covers stdlib and user inline-C alike.

The filter is the whole engineering. `emit_region_word_can_be_node` asks "can
this word BE region memory", which is much narrower than the result lock's
"cannot prove it reaches nothing" -- that also refuses a `ptr<void>`, a
`cstr`, a bare tyvar and a `(Map K V)` handle, none of which can ever be a
node. Only a `:heap` ADT or a by-value aggregate holding one qualifies;
opaque newtypes (`defopaque BtCell :ptr` is a C-made handle) and the
malloc-backed collections are excluded. Two wrong versions came first and the
snapshots priced them: the result lock as the filter emitted 12,373 notes
across the fixtures, an intermediate one 1,369 (every trail-cell accessor).
The right filter emits **zero** in this tree -- nothing in it hands a node to
inline-C -- so the fix costs no existing saving at all. One more trap on the
way: the type to ask about is the BINDING's, since `fd->param_types[]` carries
a def-less `TY_ADT` for a `:heap` parameter, which read as "erased" and made
the note silently never fire.

`tests/fixtures/region-escape-via-inline-c` pins both directions on both arms
(the node store retires; a bracket whose inline-C sees only scalars still
rewinds), and the fuzzer gained `store-inline-c` / `inline-c-scalar`, which
fail 2 of 30 programs on the pre-fix compiler by stdout AND by the savings
model. What is left in the report is `extern-c` (no emitted body to note in)
and primitives taking an already-erased `:int`, neither with a repro.

### R2's "spine-node allocation is routed" did not cover the workload

The single most valuable thing R4 did was measure the saving instead of
re-reading the claim. Two allocation sites had to be found by looking at emitted
C and at valgrind block counts:

- **A THIRD ctor emitter.** `emit_program` emits base ctors at its own site
  (emit_module.c, near the SR3 slice-A note that records this exact miss one
  representation change earlier), so a NON-parametric `:heap` ADT -- `(defdata
  Link :heap ...)`, the plainest spine there is -- still called `malloc` after
  R2. Symptom: valgrind reported 913 live blocks with the flag on against 909
  with it off. The region was pushed, popped, and never allocated into.
- **The SR4 recursive-field box, which is not a ctor at all.** `Subst` is
  `:copy`, not `:heap`, so its per-link box is the `tur_adt_Subst *__t =
  malloc(...)` that emit_expr.c writes when a by-value aggregate flows into a
  recursive constructor field. That is the allocation the RM1 leak sweep
  actually blames, and none of the three ctor emitters reach it. Until it was
  routed, R4 on `logic.tur` saved nothing at all.

Four allocation sites now, then, and they do not resemble each other. The habit
this keeps re-teaching is not "grep for the other emitter" -- that was already
written down twice and still missed both of these. It is: **a routing change is
verified by measuring the saving, never by reading the diff.**

### The free side had to move with it

From R2 onward a node allocated inside a region is arena memory while the same
emitted drop glue still ended in `free(ptr)`. That is an allocator mismatch --
glibc aborts -- not a leak. `tur_region_free` (free unless `tur_region_owns`) is
the mirror of `tur_region_alloc_or_malloc`, spelled by `region_free_fn()` at the
node free paths and resolving to plain `free` in a default build.

### What it is worth

`benchmarks/bench-regions-subst.tur`, same source both arms, flag as the only
variable (full numbers in `benchmarks/regions-subst-results.md`):

| | flag off | flag on |
|---|---|---|
| leaked at exit | 2,668,800 B in 55,600 blocks | **0** |
| allocations | 82,785 | 27,190 |
| ns/op, 2 <= n <= 32 | -- | **0.77-0.92x** |
| ns/op, n >= 64 | -- | ~parity |
| peak RSS | 7,340 kB | **3,892 kB** |

Checksums identical in every row, which is the column that would catch a rewind
of something still live.

**The peak-RSS row was reported backwards once.** An earlier revision of this
plan and of the results doc said footprint went UP (2,076 -> 2,360 kB) and told
R5 to weigh that. It was a measurement artifact: a shell poller reading `VmHWM`
every 50 ms from a program that runs in 21 ms, which returned 240, 1584, 2424,
2484 and 2500 kB for the same binary on repeat. `wait4`'s `ru_maxrss` gives the
kernel's own high-water mark, is bit-stable across five runs of each arm, and
says the flag CUTS peak footprint by 47%. The lesson is the cheap one: a spread
that wide is the measurement failing, and it was visible before the number was
believed.

### Known gaps R5 has to price

- ~~**A `:void` bracket is not wrapped at all.**~~ **Filed here in error and
  withdrawn.** `emit_value` does skip a TY_NIL/TY_NEVER call -- there is no
  hoist temp and so no statement seam -- but that guard never fires for this
  callee: `bt-scope` emits as `int64_t bt_hyscope(int64_t)`, the erased carrier,
  so `(bt-scope (fn [] (println ...)))` types as a word and IS bracketed on both
  paths. Checked, after writing it down as a cost without checking.
  `tests/fixtures/region-scope-void-body` pins it so the claim cannot be
  re-derived. The guard stays as a guard.
- **The pooled slab is never returned.** 64 KiB stays reachable at exit because
  no emitted program calls `tur_region_shutdown`. O(1), reported as reachable
  rather than lost, and worth closing before graduation.
- **Argument-position allocation lands in the generation**, because the push has
  to precede the argument emissions -- the call text embeds their temps. Sound
  for this form's thunk; the shapes it is not sound for are the transitive ones
  the static walk already refuses.
- **The CPS `CT_LETCALL` arm carries no bracket** (see above). Probing did not
  reach it with a `bt-scope` callee; if a shape does, it is a missed saving.

The report R4 filed against a `bt-scope` in a non-main defn is fixed and
archived (`docs/archive/cps-direct-bt-scope-closure-temp-undeclared.md`); both
fixtures and the benchmark are on the natural spelling now.

**R5 -- decide. DECIDED 2026-09-04: keep it, gated, at `prototype`.**

Shelving was a real outcome and R4 retired the reason it was most likely: the
solver's region DOES rewind, takes the whole per-link spine with it (2,668,800
leaked bytes to zero), and cuts peak RSS 47% on the workload RM0(b) named. So
the phase is not shelved.

Nor is it graduated, and the reason is a SURFACE question rather than any of the
measurements:

> **`bt-scope` is the trail bracket, and a region is not a trail level.**

R4 chose it because the reclamation plan did, and the choice is right for a
solver: one query is one generation. But it conflates two things a user might
want separately. A caller who wants a region and no trail level has no spelling;
a caller who wants a trail level and no region -- a `bt-scope` around code whose
allocations must outlive it -- gets a region anyway and pays the escape check to
find out it cannot rewind. Graduating would make that second case everyone's
default, silently, behind a form they wrote for backtracking. `prototype`
(TUR-W0060, "breaking changes likely") is the accurate label while the boundary
form is still the wrong shape, and promoting to `beta` would advertise a frozen
surface plus a graduation date this does not have.

`expires_at` stays 0.47.0 against a 0.43.0 tree -- no pressure either way, and
per the standing rule it would not block a release if there were.

### What graduation requires, in the order that matters

1. ~~**A boundary form of its own.** Either a `with-region` bracket that means
   only the lifetime, or an explicit statement that `bt-scope` means both and a
   documented way to opt one out. This is the blocker; everything below is
   ordinary work.~~ **DONE 2026-09-05 -- `with-region` (A1).** The blocker
   is a public-surface decision, and it was decided by the reclamation plan's
   own measured residue: RM2's category 1 (496 B, `constrained-defn-cons-
   return-monomorphize` and `refined-nonempty`) is in programs with NO
   backtracking that build a throwaway spine, so the lifetime needs a spelling
   that is not a search primitive. Of the three designs weighed --

   - **A1, additive** (taken): `with-region` is the region-only bracket;
     `bt-scope` KEEPS opening its region automatically, so R4's measured
     solver win is untouched and the change is purely additive.
   - **A2, orthogonal**: `with-region` becomes the only region opener and
     `bt-scope` reverts to trail-only, "both" being one wrapped in the other.
     Cleanest model, but it takes the automatic region off the solver path,
     which would have to re-add it in `dfs-solve` -- a change to the exact
     path R4 measured, deferred as its own separately-measured step if ever
     wanted.
   - **B, documented coupling**: no new form; declare `bt-scope` means both
     and tell non-backtracking programs to call it and not use the trail.
     Zero surface, but it leaves the conflation R5 flagged in place, merely
     documented -- a parser with no backtracking still calls a backtracking
     primitive to get a lifetime.

   -- A1 fills the one corner of the 2x2 that had no spelling, and each corner
   now has one:

   | | region | no region |
   |---|---|---|
   | **trail level** | `bt-scope` | `bt-mark` / `bt-undo-to!` halves |
   | **no trail level** | `with-region` | plain code |

   `stdlib/region.tur` (autoloaded, its own file: the region is its own
   concept, not the trail's). The body is a bare forwarder `(body)`; the
   boundary is a CODEGEN decision, and `emit_binding_is_region_scope` now
   recognizes both names, so the static walk, both emit paths and both
   runtime locks are shared unchanged -- the only difference between the two
   brackets is in the stdlib body this pass never reads. With the flag off
   `with-region` is an identity call. It carries NO `#fx{Bt}`, which is the
   checkable difference: `region-with-region` has a `#fx{}`-declared caller
   reach it, the shape that is TUR-E0009 for `bt-scope`. Measured on that
   fixture: live blocks at exit 908 -> 5 (the five are the pooled slab, R5
   item 3), allocations 1,424 -> 521, values identical both arms. Seam 13/0.
   Autoloading a new module moved all 148 codegen snapshots (binding-id
   renumbering plus the one new defn), regenerated in the same commit.
2. **Widen the static walk deliberately, with a fixture per shape admitted.**
   It accepts scalars, `cstr`, and non-heap ADTs whose every field it can walk.
   Structs, containers, refs and closures are refused wholesale -- sound, and it
   means a bracket returning a `Vec` of scalars never rewinds.

   **First widening LANDED 2026-09-05:** a non-heap ADT result whose every
   ctor field is a bare scalar primitive now REWINDS, where the walk used to
   refuse it on the field's NULL `full_type` (which is also NULL for a genuine
   `:int`). The disambiguator is the declared FORM, not the field kind -- a
   bare scalar keyword (`:int`, `:cstr`, ...) reaches nothing; an ADT name,
   `ptr`, a type variable, or any compound form stays refused, so the widening
   can only turn a refuse into a pass for a provable scalar. This admits
   `(RxIP :int :int)` -- `re.tur`'s `re-find-from` result, the 312 B of RM2
   category 2 -- and keeps the mutual-recursion result (`(MAcons :int :MB)`)
   refused. `region_field_form_is_scalar` in emit_expr.c; `region-scope-adt-
   result` now reads `retire=1 rewind=2` with the value asserted across the
   pop, and it carries the mutual-recursion case as the negative. Closes
   [region-walk-refuses-every-adt-result](https://github.com/turmeric-lang/turmeric/blob/main/docs/archive/region-walk-refuses-every-adt-result.md).
   **Second batch, PINNED 2026-09-05 (`region-scope-shapes`):** three more
   result shapes REWIND, and it turned out none needed a new walk change --
   the scalar-form widening plus the existing `full_type` walk already admit
   them, so the work was to prove it and pin each with its value read after
   the pop, per the one-shape-one-fixture rule:

   - a **`defstruct` with scalar fields** -- every defstruct is a record ADT
     (structdef-retirement DS-D), so it reaches the same TY_ADT arm and its
     `[x : int y : float]` fields are scalar keywords;
   - a variant holding a **`:cstr`** -- never region memory, on the list;
   - a variant holding a **field-less by-value enum** (`(T :int :Color)`) --
     admitted the OTHER way: `Color` is not a scalar keyword, so it only got
     through because the field's `full_type` is recorded (`record_full`) and
     the walk descends into `Color`, whose every ctor is field-less. That is
     an inference from the emitted verdict, and the negatives make it a safe
     one.

   And the negatives held: a variant HOLDING a `:heap` node retires, a
   variant holding a self-recursive spine retires, and `region-scope-adt-
   result`'s mutual-recursion case still retires and prints 42. The
   `:heap`-holding shape was at first kept DEFINED in the fixture but not
   RUN -- writing it surfaced an unrelated pre-existing miscompile (a
   record holding a heap node returned through the bracket read its int
   field back as an address, flag off too), and running it would have
   baked a nondeterministic pointer into the expected output. **Fixed the
   same day and the shape now runs with its 10 asserted**: the cause was
   the direct emitter's by-args spec matcher handing the call the clone
   minted for a SIBLING by-value record result, since its result guard
   only told two primitive kinds apart --
   [capturing-thunk-returning-heap-field-record-garbles-int](capturing-thunk-returning-heap-field-record-garbles-int.md).
   Worth knowing for this plan because every bracket call site is that
   shape: a `[A]` generic whose type variable reaches only the result.

   **Third batch, PINNED 2026-09-05 (also `region-scope-shapes`):** a
   variant holding a **non-recursive multi-variant sum of scalars**
   (`(Circle :float) (Sq :float :float)`) REWINDS, and this closes the
   "carrier-erased inner ADT" question rather than opening it: the inner
   was expected to get no `full_type` (not a product, not field-less) and
   need name-to-def resolution -- but `adt_is_byvalue_product` (types.c),
   despite its name, admits an SR1 by-value sum candidate and walks every
   variant's fields, so `Shape` gets `record_full`, a recorded `full_type`,
   and the walk descends into it.  The same free route as the enum.  What
   is left with NO recorded `full_type` is exactly what should stay
   refused: an inner with drop glue (an rc can point at a region node), a
   self-recursive inner, and a mutual-recursion partner.  Negatives pinned:
   a sum one of whose arms holds a `:heap` node retires (a union is only
   as safe as its widest arm), a self-recursive sum retires.  So the
   "name-to-def resolution" widening the previous paragraph priced is
   NOT needed; struck.

   **Fourth batch, LANDED 2026-09-05 -- the one real walk change of the
   later batches (`region-scope-vec-scalar`):** a **`Vec` of scalars**
   REWINDS.  It was refused for a reason the plan's own example ("a
   bracket returning a `Vec` of scalars never rewinds") never stated: a
   parametric monomorph `(Vec int)` is a TY_APP, so it never reached the
   walk's TY_ADT arm at all and fell to `default:`.  A new TY_APP arm
   admits exactly this shape, one fact per lock: (1) Vec's own storage --
   handle and element buffer -- is plain inline-C `malloc` in vec.tur,
   never the region router, which is emitted only at the four ADT-ctor
   sites; so the escaping handle is never region memory and the runtime
   lock is satisfied.  A compiler-warranted name check, the option-niche
   plan's warrant for Vec/Map/Set.  (2) A scalar element is an int64 word
   in that malloc'd buffer and reaches nothing.  Anything else refuses:
   a `Vec` of `:heap` nodes (pinned -- its buffer holds region pointers),
   a by-value aggregate element (heap-boxed at push by the escaping
   bridge, a later shape once that is pinned as never-routed), any other
   parametric def (its tyvar fields would refuse without the argument
   substitution the walk does not yet do).  Measured: the spines that
   produce the elements are reclaimed and the Vec survives with both
   elements and its length read back correctly after the pop.

   ~~Still refused, deliberately: the **recursive spine** (the result IS the
   node); a **Vec of by-value aggregates** and **Map / Set** (the same
   warrant would apply, each its own increment with its own fixture);
   **other parametric monomorphs** like `(Pair int int)` (need tyvar
   substitution in the field walk).~~

   **Fifth batch, LANDED 2026-09-05 (`region-scope-parametric`)** -- the
   whole of that list but the spine.  Two walk changes: (1) the ADT field
   walk SUBSTITUTES a monomorph's type arguments into its field types
   (`substitute_adt_app_type_owned`) before asking whether a field reaches,
   so `(Pair int int)` and `(Option int)` REWIND while `(Pair Link int)` and
   `(Option Link)` RETIRE -- the substituted field IS the node; a def that is
   its own type argument (`(Pair (Pair int int) int)`) is nesting, not a
   cycle, so the arguments are walked before the def goes on the path.  (2)
   The four malloc-backed collections (`Vec`, `Map`, `Set`, `MutableMap` --
   the same compiler-warranted name list as the option-niche plan) reach
   exactly what their ELEMENT types reach: handle and storage are inline-C or
   runtime `malloc`, and the container-insert bridge boxes a by-value element
   with plain `malloc`, so `(Vec (Pair int int))`, `(Map int int)` and
   `(Set int)` REWIND while `(Map int Link)` RETIRES.  Nine shapes, each with
   its value read after the pop; both arms identical.  What the walk still
   refuses is the recursive spine (by design), pointers, refs, closures and
   type variables -- and a `:heap` parametric def of the user's own.

   Writing it surfaced an unrelated pre-existing compile failure:
   `vec-get-byval` on a `Vec` of a by-value struct
   (`vec-get-byval-struct-element-returns-carrier.md`); the
   fixture reads its elements through `(:: (vec-get v i) T)`.
3. ~~**Close or price the residue** named under R4: the unbracketed CPS
   `CT_LETCALL` arm, argument-position allocation landing in the generation, and
   the pooled slab that is never returned (reachable at exit, not lost -- a pool,
   not a leak, and freeing it needs care about `atexit` ordering against module
   defers that may still read retired generations).~~ **DONE 2026-09-05 --
   all three closed, two by proof and one by code.**

   - **The `CT_LETCALL` arm stays bracket-free, by proof.** The IR builder
     (`cps_ir.c`) emits `CT_LETCALL` only for an UNCOLORED callee; a colored
     callee always becomes `CT_TAILCALL`, with a join when not in tail
     position. Both region scopes are colored (each invokes a fat thunk), so a
     region bracket can never land on the LETCALL arm. R4 knew every probed
     shape became a tailcall but only probed `bt-scope`; `region-scope-
     nontail-cps` pins it for `with-region` in the two non-tail positions
     that would produce a LETCALL for an uncolored callee (let-bound, and as
     an operand) inside a CPS-lowered function: both calls bracketed, `--dump-
     cps` shows tailcalls, no LETCALL names the bracket, 46 survives. No
     transcription -- which is what R4 wanted.
   - **Argument-position allocation is sound, verified in emitted C.** The
     only thing allocated in argument position of a region-scope call is the
     thunk's closure env, and it is `void *__t = malloc(...)` -- never the
     router, which is emitted only at the four ADT-ctor sites. So it lands
     after the push but not in the generation. (The second routed site this
     was once feared to be, emit_expr.c ~8273, is the same SR4 recursive-field
     box as ~8223; the line moved.)
   - **The pooled slab is returned: `atexit(tur_region_shutdown)`.** Flag-on
     exit residue 65,728 B in 5 blocks -> **0 bytes in 0 blocks**, valgrind
     0 errors, flag-off build references no region symbol. **The first
     attempt was wrong exactly the way this item warned**, and a probe caught
     it before it shipped: registered in a `main` prologue (all four of them),
     it ran BEFORE a module `defer` that read a value escaped into a retired
     generation -- `0` where `42` was right, `Invalid read`. Cause: a
     `__attribute__((constructor))` runs `__tur_static_init` -- and so
     registers the defers' `atexit`s -- before `main`, so anything `main`
     registers is later and, LIFO, runs first. The only always-earlier place
     is the first statement of `__tur_static_init` itself, behind its
     idempotent guard: once, from whichever path enters first, before any
     band. One call site (`static_init_emit`, emit_core.c), not four.
     `region-shutdown-order` pins the placement (after the guard, before
     `__module_defers_init`, not in `main`), the gating, and the value: the
     defer prints `42` from the retired generation before shutdown frees it.
4. ~~**Soak the existing `bt-scope` callers.** `dfs-solve` and the sx2 fixtures
   become region users the moment this is on by default; the seam test covers
   twelve fixtures today, which is a floor, not a soak.~~ **This is the flip
   (2026-09-05).** The soak is not a prerequisite the flip waits on; it is
   what the flip *is*: from this commit every `bt-scope` and `with-region`
   caller in the tree -- `dfs-solve`, the sx2 fixtures, every spine-carrying
   program -- runs with a region, on every `bash tests/run.sh`, on every CI
   run. What the seam covers is now the OTHER path (the thirteen-fixture
   population under `TUR_REGIONS=0`, plus a canary that the hatch bites), so
   a bisection stays a one-variable switch. Full suite green at the flip with
   all 148 codegen snapshots regenerated (every program now carries the
   region externs, the routed ctor allocations, the atexit shutdown, and a
   bracket at each boundary).

## What this does NOT do

- It does not free a value whose lifetime is not a scope. Those stay RM2's,
  and RM2 gets smaller for it -- what remains is "spines that escape their
  region", which is also the set R3's walk has to identify anyway.
- ~~It does not change any default. The flag is off; `logic.tur` and every other
  program allocate exactly as they do today until R4 opts a call site in.~~
  **Superseded 2026-09-05: it is the default.** `TUR_REGIONS=0` restores the
  old allocation exactly.
- It is not region *inference*. Every region is declared.

## Guides to update when this graduates

- ~~`docs/guides/gc-guide.md` -- it documents the arena and the rc/cycle paths;
  a third reclamation mode belongs beside them.~~ **Done 2026-09-05** -- a
  "Region-allocated values" entry beside the arena paragraph, with the two
  locks, the two brackets and the hatch.
- ~~`docs/guides/experimental-flags-guide.md` -- the row while it is gated.~~
  **Struck 2026-09-05: there is no such row to add.** That guide deliberately
  does not restate the experiment list -- "the registry is the single source
  of truth; this guide does not restate the list -- the command does" -- and
  defers to `tur experiments`. The `EXPERIMENTS[]` row in
  `src/runtime/experiments.c` IS the gated-feature documentation. Nothing to
  do there while gated, and nothing to remove at graduation either.
