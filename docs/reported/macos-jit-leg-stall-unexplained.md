# `JIT engine (macos-latest)` stalls for the whole job budget, cause unknown

**Severity: medium.** It gates: `JIT engine (macos-latest)` is the one JIT leg
that is not `continue-on-error`, so a stall fails the run. Four occurrences to
date, the most recent
[run 36385448273](https://github.com/turmeric-lang/turmeric/actions/runs/36385448273)
on 2026-09-28 (rjungemann/turmeric#953): ~48 minutes in `Run JIT suites`
against a 13-19 min baseline on the two preceding `main` runs.

Filed 2026-09-28, split out of
[docs/archive/macos-jit-hang-loses-both-diagnostics.md](https://github.com/turmeric-lang/turmeric/blob/main/docs/archive/macos-jit-hang-loses-both-diagnostics.md)
when that report's *instrumentation* defect was fixed. This is the half that
remains, and it is deliberately thin, because the honest state is that **there
is no evidence to reason from.**

## Why there is nothing here yet

The 2026-09-28 occurrence destroyed its own diagnostics: a `timeout-minutes`
kill cancels the job and leaves every remaining step `pending`, so the
`if: always()` artifact upload never ran, and the run's log blob 404s. That is
now fixed -- see the archived report -- but it cannot be applied backwards.

Two earlier root causes are known and were genuinely fixed, so neither is a
live lead: a missing `timeout(1)` turning `httpd-async-limit`'s listen-fd
deadlock into a job kill
([docs/archive/macos-jit-leg-intermittent-45min-hang.md](https://github.com/turmeric-lang/turmeric/blob/main/docs/archive/macos-jit-leg-intermittent-45min-hang.md)),
and the `brew install coreutils` containment that followed it -- verified still
in place on the hung run, whose `Install dependencies (macOS)` step took 4
seconds and poured `coreutils`.

## What the next occurrence will hand you

All three now exist, so this should be a short investigation rather than
another blind one:

- **A `jit-ctest-log-macos-latest` artifact.** The step now alarms at 2100s and
  fails, which is a failing step rather than a killed job, so the upload runs.
- **The test's name.** `tur_jit_fixture_tests`, `tur_repl_spice_jit` and
  `tur_flags_tests` carry ctest `TIMEOUT` properties (1500 / 600 / 900), so a
  single hang is killed and named well inside the alarm.
- **A per-case bound inside `run-flags.sh`.** Every `$TUR` invocation there runs
  under `timeout -k 5 180`, so a hung `jit-ffi-*` case -- dynamic FFI,
  callbacks, threads, the shapes that hang -- fails as a case with its name.

## Where to look when it does

Ranked by what the fixes above cannot bound:

1. **An untimed phase around a fixture, not the fixture.** `run-jit.sh`'s
   `_run_timed` wraps the `"$TUR" ... jit "$input"` invocations; harness setup
   and any untimed compile/link is outside it. This was the archived report's
   own point 3 and is still the likeliest home for a 45-minute stall.
2. **A `jit-ffi-*` case in `run-flags.sh`.** It had no timeout of any kind until
   2026-09-28, and CI runs it in this leg precisely because it is the only
   harness whose `jit-ffi-*` cases are not skipped.
3. **Runner size.** `macos-latest` hands out 3-core and 5-core machines and CI
   draws the 3-core one ~97% of the time; `tur_jit_fixture_tests` alone is
   ~730s median / ~940s p90 there. That explains 19 minutes, not 48, so it is
   context rather than cause -- but a genuine stall plus a slow runner is what
   sets the per-test `TIMEOUT` values, and those may need revisiting if a
   legitimate run ever trips one.

## 2026-09-29: the first instrumented occurrence was slowness, not a stall

[Run 36608484061](https://github.com/turmeric-lang/turmeric/actions/runs/36608484061)
(rjungemann/turmeric#970) failed the leg with the instrumentation working as
designed: `tur_jit_fixture_tests ***Timeout 1500.55 sec`, the step failing
rather than the job being killed. The console lines the step's filter lets
through (FAILs and fallback PASSes) arrived at a steady ~1.9 fixtures/s the
whole way. Every gap between them scales with the number of fixtures it covers.
The last one, 1408 s in, was `workstealing-steal`, #2660 of 2663 top-level
fixtures. What was cut off was the tail of the positive pass, the nested
fixture dirs and the whole `errors/` pass (~650 dirs) -- work, not a hang.
`main`'s run of the same hour passed the leg, in 27 minutes for the whole
job.

The cause of *that* was a fixed per-program cost: c2mir compiled the whole
auto-loaded prelude and its system headers for every program. It is fixed in
[jit-suite-pays-for-the-whole-prelude](../archive/jit-suite-pays-for-the-whole-prelude.md)
(sum of per-fixture time 1843 s -> 1355 s locally, median 508 -> 350 ms). The
ctest `TIMEOUT` was deliberately left at 1500.

This does not explain the earlier 45-48 minute occurrences, which had no
per-test timeout and left no evidence, so the report stays open on its own
closure condition below. It does mean a `***Timeout` on this test is first a
question about throughput: check the fixture rate in the console before
looking for a stuck fixture.

## 2026-10-01: the second instrumented occurrence, same shape -- and a misreport

[Run 36780743533](https://github.com/turmeric-lang/turmeric/actions/runs/36780743533)
(rjungemann/turmeric#1002, a dependabot `setup-emsdk` v14 -> v16 bump) failed
the leg with `tur_jit_fixture_tests ***Timeout 1500.38 sec`. The PR is not
implicated: `setup-emsdk` is used only by the `test` job's `tur_refine_wasm`
step and the web job, and all three checks that run it passed. This leg never
touches emsdk.

Again throughput, not a stall, by this report's own test -- and this time the
comparison is clean, because the prelude fix above was already in the tested
tree (it landed 2026-09-29, the PR base is 2026-09-30):

| | fixture dirs done | wall | rate |
| --- | --- | --- | --- |
| #1002 (killed) | 2393 | 1500 s | **1.6/s** |
| `main` 36901587136, same day, green | 3413 | 1102 s | **3.1/s** |

The ratio holds at every milestone the step's filter lets through
(`promise-linear` 814 s vs 477 s, `thread-local-basic` 1497 s vs 936 s), so the
runner was uniformly ~1.9x slower for the whole run -- the 3-core vs 5-core
`macos-latest` split, which point 3 above already named as context. What is new
is that it is no longer only context: at 3413 dirs the FAST draw now takes
1102 s, above the 940 s p90 the 1500 s bound was sized against, so the bound had
quietly become a throughput assertion a slow draw must fail. That discharges
point 3's "those may need revisiting if a legitimate run ever trips one":
`TIMEOUT` is now 2400, the workflow's whole-ctest alarm 2550 s and the job's
`timeout-minutes` 60, keeping the per-test bound the first to fire so it still
names the target.

**The occurrence also cost a triage pass to a misreport**, which is fixed in
the same change. The run's other line was

```
117: FAIL r7rs-tail-calls -- stdout mismatch
```

which reads as a tail-call regression. It was not. The uploaded
`jit-ctest-log-macos-latest` artifact shows the actual stdout was zero bytes
(`@@ -1,4 +0,0 @@`), and the FAIL printed exactly 60 s after `jit.stdout` was
created -- that fixture's own `expected.timeout`. It was killed, not wrong.
`tests/run-jit.sh` captured the child's `rc` and then diffed `expected.stdout`
without ever testing it, and `124` appeared nowhere in the file, so EVERY
per-fixture timeout in this harness reported as a stdout mismatch. `run.sh` and
`run-turi.sh` each grew that check after
[docs/archive/ci-cps-tramp-turi-timeouts-under-load.md](https://github.com/turmeric-lang/turmeric/blob/main/docs/archive/ci-cps-tramp-turi-timeouts-under-load.md);
this harness was missed. It now reports `timed out (>Ns under the JIT engine)`.

Note the two symptoms had one cause. A slow draw produces both a per-FIXTURE
timeout and the per-TEST one, so on the next occurrence expect stray fixture
FAILs alongside the `***Timeout` and do not triage them as separate bugs.

## 2026-10-03: the third instrumented occurrence -- the bound moved down

[Run 37107254052](https://github.com/turmeric-lang/turmeric/actions/runs/37107254052)
failed the leg in the shape the section above predicted -- "expect stray
fixture FAILs alongside the `***Timeout`" -- with one part conspicuously
missing: **there was no `***Timeout`.**

| | this run | macOS p50 / p90 / max |
| --- | --- | --- |
| `tur_jit_fixture_tests` | `***Failed 1054.57 sec` | 850 / 1125 / 1648 s |

1055 s is *below* the p90. The suite ran at an entirely ordinary pace, 3407
fixtures passed, 61 skipped, and the only failure was

```
121: FAIL r7rs-tail-calls -- timed out (>60s under the JIT engine)
```

-- so the per-fixture timeout reporting added on 2026-10-01 worked and named
the target immediately, with no log archaeology.

**What this adds: the 2026-10-01 fix raised the per-TEST bound and left the
per-FIXTURE one alone, so the failure migrated down into it.** `TIMEOUT` went
1500 -> 2400 to absorb the 1.9x slow draw; `r7rs-tail-calls`'s own
`expected.timeout` stayed at 60. That fixture is four 1e7-iteration tail-call
loops at -O0 (self, mutual `ev?`/`od?`, indirect through a value, and a named
`let` -- roughly 40M trampolined calls), so on a slow draw it is the suite's
longest single fixture. Being killed at 60 s implies ~32 s on a fast draw:
60 was almost exactly the slow-draw time, leaving no margin at all. It is
**120** as of #1044 -- ~3.75x the fast-draw estimate and ~2x the observed
kill, still far inside the 2400 s suite bound, so a genuine hang is still
caught by the per-fixture bound first and still named.

This is the second time this fixture has been killed by a slow draw; the
2026-10-01 occurrence is the other, where it reported as a stdout mismatch.

**2026-10-04 (stress-fixture-tiering-plan):** the cause is now removed rather
than budgeted around. `r7rs-tail-calls` runs each loop 1e6 deep (4 s under the
Debug JIT locally, against 23-25 s at 1e7) with a 30 s budget; the 1e7 version
moved to `r7rs-tail-calls-stress` (`requires.stress`), which only the nightly
arm64 workflow runs. 1e6 still asserts: the same procedures without the tail
position overflow an 8 MB stack between 250,000 and 300,000 frames.

Measured frequency, one entry per commit from `suite-timings-2026.jsonl` on
the `ci-metrics` branch:

| env | commits | fail rate | episodes |
| --- | --- | --- | --- |
| macOS AppleClang-21, 3 cores (**gates**) | 289 | 2 = **1%** | 2, both isolated |
| Linux GNU-13.3.0, 4 cores (non-gating) | 302 | 55 = **18%** | 37 (29 isolated, longest 10) |

Two things follow. The macOS failures are isolated, never consecutive, which
is what a resource-variance story predicts and a standing defect does not. And
the Linux rate is 18x higher with `continue-on-error` absorbing all of it --
its own finding, and resolved the same day as
[jit-linux-leg-failures-absorbed](../archive/jit-linux-leg-failures-absorbed.md):
18 of 18 sampled Linux failures are two fixtures that each already had a
report -- `r7rs-threads-lifecycle` (the open ASan fork/allocator-lock
flake) and `fn-field-carrier-shim-read-typed` (resolved).

`run-jit.sh` now prints the fixtures closest to their own budget in its
summary, so the next bound to go marginal is visible while it still passes
rather than when a slow draw kills it.

## What would close this

A named stall with a cause, or a long enough quiet period on a leg that now
cannot hide one. Do not close it on the instrumentation fix alone: that made
the next occurrence legible, it did not make it stop.
