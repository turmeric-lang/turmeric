# Stress-fixture tiering -- small counts per PR, full counts nightly

> **Status: IMPLEMENTED 2026-10-04 -- archive once the first nightly confirms
> T4** (section 8). The expensive set turned out to be ONE fixture,
> `r7rs-tail-calls`; it now runs 1e6 per PR and its 1e7 twin runs nightly
> under a new `requires.stress` marker (mechanism B). Section 3's suggested
> per-PR value of ~1e5 is WRONG for r7rs and section 8.2 says why.
>
> Originally written in answer to "do we
> need such a high order of magnitude for testing stack depth in tests, and is
> there a nightly test we can run that is more intensive while keeping feature
> branch runs quicker?"
> **Type:** CI / test infrastructure
> **Touches:** ~19 fixtures under `tests/fixtures/*-deep*`, `*-stress*`,
> `*unoptimized*`; `tests/run-jit.sh` and `tests/run.sh` (the knob);
> [`.github/workflows/nightly-arm64.yml`](../../.github/workflows/nightly-arm64.yml)
> (the full tier).
>
> **Three things measurement settled before any design:**
> 1. **Cost is not proportional to the iteration count.** Six fixtures run 1e7
>    iterations with no `expected.timeout` at all and are nowhere near their
>    budget; one runs 1e6 and sits at 60%. A blanket "divide every bound by
>    100" would slow nothing down and would throw away margin for free
>    (section 2).
> 2. **The latency prize is modest: ~7% of one leg**, not the big win the
>    framing suggests. The honest justification is fewer slow-draw timeout
>    failures, not a faster suite (section 4).
> 3. **The mechanism already exists and has precedent.** `TUR_GSM_SHARD` is
>    exactly this pattern -- a quarter per PR, all of it nightly -- and
>    `nightly-arm64.yml` is already the home for "the expensive suites in
>    FULL, off the critical path" (section 5).

## 1. The inventory

Fixtures whose name says depth or stress and which carry an explicit loop
bound, with the budget each has today:

| fixture | bound | `expected.timeout` |
| --- | --- | --- |
| `tailcall-musttail-deep` | 1e7 | -- |
| `tailcall-mutual-deep` | 1e7 | -- |
| `tco-self-tail-deep` | 1e7 | -- |
| `tailcall-match-arm-deep` | 1e7 | -- |
| `tailcall-annot-self-deep` | 1e7 | -- |
| `tailcall-and-or-deep` | 1e7 | -- |
| `r7rs-tail-calls` | 1e7 x4 | 120 |
| `tco-named-let-nocapture-deep` | 5e6 | -- |
| `tco-named-let-capture-deep` | 5e6 | -- |
| `cps-tramp-resume-deep-1m` | 3e6 | 60 |
| `cps-tramp-resume-multicase-500k` | 1.5e6 | 60 |
| `tailcall-dyn-deep` | 1e6 | 60 |
| `r7rs-cps-mutual-tail-unoptimized` | 1e6 | 60 |
| `void-self-tail-call-loop` | 1e6 | 60 |
| `tailcall-drop-glue-deep` | 1e6 | -- |
| `r7rs-cps-loops-unoptimized` | 1e6 | 60 |
| `stackless-catch-unwind-deep` | 5e5 | -- |
| `cps-tramp-resume-deep` | 3e5 | 30 |
| `rc-free-queue-deep-cascade` | 1e5 | -- |

Note what that table does NOT show: any relationship between the bound and
whether anyone felt the need to give the fixture a budget.

## 2. Why a blanket reduction is the wrong change

The six 1e7 fixtures with no budget are not near failing. `tailcall-dyn-deep`
at **1e6** is the one at 60% of budget. The cost tracks the MECHANISM, not the
count:

| what the tail call is | what it lowers to | cost |
| --- | --- | --- |
| self, annotated self, and-or, match-arm, musttail | a **backedge** -- a real loop | ~1 ns an iteration |
| mutual (2-8 members) | one fused function with a dispatch loop (T5) | ~ns |
| **dynamic, through a function value** | trampoline bounce + registry lookup | **47 ns compiled at -O2** |

So 1e7 self tail calls is a 1e7-iteration loop -- tens of milliseconds -- while
1e6 dynamic calls is 1e6 trampoline round trips. That is why
`tailcall-dyn-deep` is the expensive one at a hundredth of the count, and why
dividing every bound by 100 would buy nothing on six of the seven 1e7
fixtures while deleting the margin that makes them meaningful.

**Corollary: target the fixtures the headroom block names, not the ones with
big numbers in them.** `tests/run-jit.sh` now prints the fixtures closest to
their own `expected.timeout` in its summary, which is the list to work from.
Its first run on `main` named `tailcall-dyn-deep` at 9s/15s on Linux and
6s/15s on macOS, which is how this question arose at all.

## 3. How small can a bound get and still assert anything

The bound exists to exceed the stack-overflow threshold, so that a regression
in tail-call lowering crashes instead of passing.
[proper-tail-calls-plan](../archive/proper-tail-calls-plan.md) measures that
threshold: before T6 each dynamic tail call was a nested C call through the
callee's direct-entry wrapper, about **285 bytes of stack per level**, which
"overflowed an 8 MB stack by 20,000 deep" and reached "~30,000 deep at -O2,
and far less at -O0".

So **1e5 is a 3-5x margin over the -O2 threshold and more at -O0** -- the
regression still crashes, loudly, at a tenth of the current 1e6. 1e6 is a 33x
margin and 1e7 a 333x margin: both are insurance against a threshold nobody
expects to move by two orders of magnitude.

Keep the margin where it is free (section 2) and spend it where it is not.

## 4. What this is actually worth -- and the stronger argument

From the first `main` run carrying the headroom block, the Linux JIT leg's
whole suite is **733 s**, and the named heavy tail-call fixtures account for
roughly **58 s** of it (`r7rs-tail-calls` 45 s, `tailcall-dyn-deep` 9 s,
`tailcall-drop-glue-deep` 4 s; the six budget-less 1e7 fixtures are all under
the 8th-place 4 s and mostly far under). A 10x reduction on the expensive ones
saves perhaps 50 s of 733 -- about **7% of one leg**, one to two minutes off
the JIT leg's 13-19 min.

That is not nothing, but it is not the headline, and the plan should not be
sold on it. **The stronger argument is flake, not latency.** A fixture sitting
at 60% of its budget on a leg whose suite wall clock spans 2.8x run to run is
a fixture that fails on a slow draw -- which is exactly what happened to
`r7rs-tail-calls` twice
([macos-jit-leg-stall-unexplained](../reported/macos-jit-leg-stall-unexplained.md),
2026-10-01 and 2026-10-03), each time costing a triage pass and once reading
as a tail-call regression it was not. Cutting the count on the expensive
fixtures removes that failure mode at the source rather than papering it with
a bigger timeout, which is all the two budget bumps so far have done.

## 5. The mechanism

`nightly-arm64.yml` already exists for "the two expensive representation
suites, in FULL, on arm64", and `TUR_GSM_SHARD` is already the knob pattern:
the PR leg runs `TUR_GSM_SHARD=1/4` of the generic-spec matrix and the nightly
runs all of it. This plan is the same shape for iteration counts.

Three ways to express a per-tier count, in descending preference:

**A. An env var the fixture reads, defaulting to the small count.** The
fixture asks for its bound instead of hardcoding it; `run.sh` / `run-jit.sh`
pass `TUR_STRESS_SCALE` (1 by default, 100 nightly). This is the
`TUR_GSM_SHARD` shape, keeps one fixture per behaviour, and makes the nightly
genuinely the same test at full size.

The cost is that a fixture must read the environment, and
[CLAUDE.md](../../CLAUDE.md) restricts argument reading to `*args*` /
`stdlib/args.tur`. An env read is not an argument read, but it is new fixture
machinery and wants a single shared helper rather than 19 copies of an
inline-C `getenv`.

**B. A `requires.stress` marker and two fixtures per behaviour** -- a small
one that always runs and a 1e7 sibling skipped outside nightly. No new
runtime machinery at all, and it reuses the `requires.*` convention the
harnesses already honour (`requires.tsan` is precisely this: skip unless the
tier is active). The cost is duplicated fixtures, which drift.

**C. Leave the counts alone and raise the budgets.** What the last two changes
did. Cheapest, and it does stop the flake, but the suite keeps paying the time
and every future slow draw re-opens the question.

**Recommended: B for the 1e7 fixtures that are cheap** (nothing to gain by
scaling them, and a `requires.stress` sibling costs nothing), **A for the
expensive dynamic-call ones** where the count is the cost. If A's env plumbing
proves contentious, B alone is sufficient and strictly better than C.

## 6. Steps

- **T1.** Widen the headroom block from its top 8 to the full list (or dump
  the `.time` files as an artifact) for one run, to size the problem against
  measurement rather than against this table's guesses. Cheap, and it is the
  only step whose output changes the rest.
- **T2.** Pick the mechanism on T1's evidence. If the expensive set really is
  3-4 fixtures, A's plumbing may not be worth it and B covers everything.
- **T3.** Convert the expensive fixtures, keeping the current bound as the
  nightly value and ~1e5 as the PR value -- 3-5x over the overflow threshold
  per section 3, never less.
- **T4.** Wire the full tier into `nightly-arm64.yml` and **verify it actually
  runs at full size**, by reading the nightly's own headroom output rather
  than assuming the env var arrived. A nightly that silently runs the small
  counts is worse than no nightly, because it looks like coverage.
- **T5.** Drop the budget bumps that T3 makes unnecessary, so the timeouts go
  back to describing a hang rather than absorbing a slow draw.

## 7. What is deliberately not in scope

- **Reducing the six cheap 1e7 fixtures.** Section 2: they cost nothing and
  the margin is free.
- **Raising the optimization level to make them faster.** The `-O0` in these
  fixtures' `flags` files is the assertion: at `-O2` the optimizer collapses
  the loops and the test stops proving constant stack. See the note in
  `tests/fixtures/tailcall-dyn-deep/input.tur`.
- **The JIT's per-call cost.** Measuring `tur jit` against compiled on one
  machine is its own task -- the published 47 ns baseline is x86_64 Linux and
  the only JIT figures so far came off a contended laptop, so the ratio is not
  established. It does not block any step here: the tiering decision rests on
  the measured per-fixture wall clock, whatever the per-call breakdown is.

## 8. As implemented (2026-10-04)

### 8.1 T1 -- the measurement

`tests/run-jit.sh` grew `TUR_HEADROOM_TOP` (default 8) so the headroom block
can list everything. Run over all 147 candidates -- every fixture with an
`expected.timeout`, plus every `*deep*` / `*stress*` / `*unoptimized*` one --
on a 4-core Linux Debug + `TUR_JIT` build, otherwise idle:

| fixture | elapsed / budget |
| --- | --- |
| `r7rs-tail-calls` | **25 s / 120 s** |
| `tailcall-carrier-let-deep` | 3 s / 15 s |
| `tailcall-mutual-deep`, `tailcall-drop-glue-deep`, `panic-catch-unwind-nested-deep` | 2 s / 15 s |
| `cps-tramp-resume-deep-1m`, `cps-tramp-resume-multicase-500k`, `r7rs-threads-dynamic-env` | 7 s / 60 s |
| `tailcall-dyn-deep` | 4 s / 60 s |
| every other candidate | <= 7% of budget |

That agrees with section 4's CI numbers (`r7rs-tail-calls` 45 s, the next
9 s and falling): **one** fixture is expensive, and it is 40M trampolined
dynamic calls. Everything else, including `tailcall-dyn-deep`, has margin and
was left alone (section 7's reasoning applies to it too).

### 8.2 T3 -- how low the count can go, measured for r7rs

Section 3 took its threshold from the Turmeric dynamic-call path (~285 bytes
a level, overflow near 20-30k). r7rs frames are far smaller. The same
procedures as the fixture WITHOUT the tail position,
`(define (f n) (if (= n 0) 0 (+ 1 (f (- n 1)))))`, with the fixture's
`--debug` flag on an 8 MB stack:

| depth | compiled | `tur jit` |
| --- | --- | --- |
| 1e5 - 2.5e5 | ok | ok |
| 3e5, 1e6 | crash | crash |

So **1e5 would not assert anything** -- it fits on the stack, and a tail-call
regression would pass. **1e6 is the floor**, ~3.5x past the overflow, and is
what `r7rs-tail-calls` now runs; its header carries the measurement and a "do
not lower" note. Measured cost at 1e6: 4 s under `tur jit` (mostly compile;
23-25 s at 1e7) and 0.29 s compiled (2.7 s at 1e7).

### 8.3 T2 / T4 / T5 -- mechanism, nightly, budgets

- **Mechanism B.** `requires.stress` is honoured by `run.sh`, `run-jit.sh` and
  `run-turi.sh` (skipped unless `TUR_STRESS=1`). `run.sh` prints the skip as
  `PASS <name> (stress-skipped)` rather than a bare PASS, so a log can tell a
  skip from a run. `r7rs-tail-calls-stress` is the 1e7 twin. Mechanism A's env
  plumbing was not needed for one fixture.
- **Nightly.** `nightly-arm64.yml` runs every `requires.stress` fixture through
  both `run.sh` and `run-jit.sh` with `TUR_STRESS=1 TUR_FORCE=1`. The set comes
  from the markers on disk; an empty set is an error; each harness must report
  exactly that many passes, with nothing skipped and no `(stress-skipped)`
  line. Dry-run locally both ways: passes with `TUR_STRESS=1`, and fails with
  it withheld from either harness.
- **Budget.** `r7rs-tail-calls` goes 120 -> **30 s** (4 s measured; ~2x a
  pessimistic slow-macOS estimate), so the budget describes a hang again. The
  twin keeps 120.

**Found on the way:** `run-jit.sh`'s headroom block hung forever when no
fixture wrote a `.time` file (all skipped, or all stamp-cache hits): with
`nullglob` on, the `ls` guard got no operand and succeeded, and `cat` then
read stdin. Fixed with an array guard.

**Still to observe:** the first scheduled nightly after merge -- its
`stress.log` should show `r7rs-tail-calls-stress` passing under both
harnesses. Archive this plan then.

