# Four ctest suites have never run in full on a hosted runner

**Summary:** the `/ci` skip ledger, read over its first two weeks of data
(89 `main` pushes, 2026-08-26 to 2026-09-09), shows four suites that skip
all or part of themselves on **every** run of the `test` job, on one or both
operating systems. Each one exits 0, so ctest records a PASS and nothing in
the job output says the coverage is missing. The timing-trends plan's open
question predicted exactly this shape and said it deserves a report rather
than a dashboard row.

**Status: RESOLVED 2026-09-09.** The report's own closure condition --
"Close this report when the `/ci` skip ledger shows all four as `pass` on
Linux" -- is met, on both operating systems rather than just Linux.

Read straight off the `ci-metrics` orphan branch
(`suite-timings-2026.jsonl`, 25,962 rows), which is the artifact this report
was written from:

| suite | 5204f4c83 | b58c91e67 | a42097764 | bf7713246 | d43cc42ef | 70f079975 |
| --- | --- | --- | --- | --- | --- | --- |
| `tur_phase4_gdb` | pass / pass | pass / pass | pass / pass | pass / pass | pass / pass | pass / pass |
| `tur_phase5_gdb` | pass / pass | pass / pass | pass / pass | pass / pass | pass / pass | pass / pass |
| `tur_tutorial_steps` | pass / **skip** | pass / pass | pass / pass | pass / pass | pass / pass | pass / pass |
| `tur_refine_wasm` | **skip / skip** | pass / pass | pass / pass | pass / pass | pass / pass | pass / pass |
| `tur_scscm_compile` | **skip / skip** | pass / pass | pass / pass | pass / pass | pass / pass | pass / pass |

(Linux / macOS. `5204f4c83` is the push immediately before the `ci.yml`
changes landed; the surviving skip reasons on that row are verbatim
`emcc not on PATH`, `sibling ../turmeric-spices checkout absent` and
`pyyaml unavailable`.)

That last column of the pre-fix row matters more than the five green ones
after it. This report exists because a skip and a pass are indistinguishable
in a job log -- both exit 0 -- so "they are green now" would be exactly the
claim the report warns against. The `5204f4c83` row is the positive control:
the ledger *does* record these suites as `skip` when they skip, it recorded
them that way right up to the fix, and it has recorded `pass` on every push
since. Five consecutive pushes, both OSes.

Two predictions in the fix directions did not hold, both in the favourable
direction:

- **macOS gdb.** Direction 1 said macOS "is harder (Homebrew's gdb needs
  code-signing to attach) and can stay partial as long as Linux runs the full
  suite." It did not stay partial -- `tur_phase4_gdb` and `tur_phase5_gdb`
  are `pass`, not `partial`, on macOS too.
- **"Expect to find something the first time they run."** The gdb halves had
  never executed on a hosted runner in the project's history. They passed on
  their first real run, and on every one since.

The `tur_refine_wasm` wrinkle the report records mid-flight -- Emscripten's
one-time `shared:INFO: (Emscripten: Running sanity checks)` line, which the
harness counted as compiler output and therefore a failure -- is fixed and
stayed fixed: the harness warms `emcc` once before the loop, and the suite has
been `pass` on both OSes for five pushes.

All five `ci.yml` dependency changes are present on `main` at `70f079975`:
`gdb` in the Linux `apt-get` line (:78), `pyyaml` on macOS (:97), the
`Set up Emscripten` step in the `test` job (:124), and the sibling
`turmeric-spices` clone (:136).

**Severity:** medium. None of these is a product defect, but two of them
guard the debugger's native source maps (Phase 4 and Phase 5 of the
debugger work), which have therefore only ever been checked by hand.

**Status:** all four fix directions below are applied to `ci.yml` on the
same branch as this report (2026-09-09). Before pushing, each suite was run
locally with its tool present on a Debug/ASan build: both gdb suites pass
(5 and 6 assertions), `tur_scscm_compile` passes, and the three
`requires.spices` fixtures pass under `tests/run.sh` (under
`tests/run-turi.sh` two of them are `requires.dedicated-runner` skips and
the third is an `errors/` fixture, so the checkout changes nothing there).
`tur_refine_wasm` could not be run here at first (no `emcc` in the
container) and failed on its first CI run
([#846](https://github.com/turmeric-lang/turmeric/pull/846)): Emscripten prints
a one-time `shared:INFO: (Emscripten: Running sanity checks)` line on a
fresh install, and the harness treats any compiler output as a failure, so
the first source compiled "failed" on that line alone while the other nine
and the solver checks passed. Reproduced locally on a fresh emsdk with the
sanity cache cleared; the harness now warms `emcc` once before the loop.
The first run also surfaced that enabling the spices checkout put
`errors/ecs-defsystem-writes-unauthorized` in front of `tur --interpret` for
the first time, which cannot resolve a manifest's `:spices :path` deps (only
`check` / `emit-c` / `run <file>` and the REPL do that walk), so it now
carries `requires.compiled`. Close this report when the `/ci` skip ledger
shows all four as `pass` on Linux.

## What the ledger shows

Every row below is "skipped on every one of N runs" in the canonical
environments (`Test (ubuntu-latest)` 88 runs, `Test (macos-latest)` 89
runs). The marker text is what the harness prints and
`tools/ci/collect-suite-timings.py` records.

| suite | Linux | macOS | marker |
|---|---|---|---|
| `tur_phase4_gdb` | partial, 88/88 | partial, 89/89 | `TUR_SKIP_PARTIAL: gdb unavailable (native backtrace check)` |
| `tur_phase5_gdb` | partial, 88/88 | partial, 89/89 | `TUR_SKIP_PARTIAL: gdb unavailable (DWARF + pretty-printer checks)` |
| `tur_tutorial_steps` | runs | skip, 89/89 | `TUR_SKIP: pyyaml unavailable` |
| `tur_refine_wasm` | skip, 88/88 | skip, 89/89 | `TUR_SKIP: emcc not on PATH` |
| `tur_scscm_compile` | skip, 88/88 | skip, 89/89 | `TUR_SKIP: sibling ../turmeric-spices checkout absent` |

"Partial" means the emit-c half of the gdb suites does run (586 ms and
1392 ms measured in the plan's baseline); it is the gdb half -- the part
that actually asserts a native backtrace names Turmeric source lines, and
that the DWARF and pretty-printers work -- that has never executed in CI.

## Why it went unnoticed

All five harnesses exit 0 on the skip, which is the right behaviour for a
developer box without the tool. Before the `TUR_SKIP` markers landed
(2026-08-25) three of them printed `PASS` on the skip path, so even a person
reading the log saw a pass. The ledger is the first artifact that
distinguishes "passed" from "did not run" across runs, and it took the full
two weeks of rows for "every run" to be a claim rather than a guess.

## Fix directions

Each is one line in the job's dependency step of `.github/workflows/ci.yml`;
none changes a harness.

1. **gdb on Linux:** add `gdb` to the `apt-get install` line of the `test`
   job. The ubuntu-24.04 image does not put a `gdb` on `PATH` (the harness
   checks `command -v gdb`). This turns the Phase 4/5 native checks into
   real CI coverage for the first time; expect to find something the first
   time they run. macOS is harder (Homebrew's gdb needs code-signing to
   attach) and can stay partial as long as Linux runs the full suite.
2. **pyyaml on macOS:** `python3 -m pip install pyyaml` in the macOS
   dependency step, matching whatever gives Linux its copy, so the tutorial
   steps are asserted on both OSes rather than one.
3. **`tur_refine_wasm`:** either install Emscripten in the `test` job (the
   browser job already has a `Set up Emscripten` step to copy) or move the
   suite to the browser job, which is the only place `emcc` exists today.
4. **`tur_scscm_compile`:** clone `rjungemann/turmeric-spices` next to the
   checkout in the `test` job, the way CLAUDE.md's "Optional dependencies"
   section describes, so the `requires.spices` fixtures and this suite run.
   That is also the precondition for the spices side of several other
   suites' `requires.spices` skips, which are per-fixture and not counted
   here.

Once fixed, the rows flip from `skip` to `pass` on the next `main` push and
the ledger is the check that they stay that way.

## Related

- `docs/archive/suite-timing-trends-plan.md` -- the two-week readout this
  report comes from.
- `docs/archive/turi-suite-accounting-and-reporting-gaps.md` -- the same
  "green while not running" shape inside one suite.
