# The macOS JIT leg's 45-minute hang recurred, and both diagnostics were lost

**RESOLVED 2026-09-28** -- the instrumentation defect, which is what this report
was filed for. All three fix directions landed: the ctest invocation is bounded
inside the job budget so a hang FAILS the step instead of getting the job
killed (which is what skipped the `if: always()` upload), the leg's three ctest
targets carry `TIMEOUT` properties so a per-test kill names the culprit, and
`tests/run-flags.sh` has a per-invocation timeout for the first time. See "The
fix" below.

**The stall itself is still unexplained**, and cannot be investigated from this
occurrence -- its evidence is gone, which was the report's whole point. It is
tracked as its own open finding:
[docs/reported/macos-jit-leg-stall-unexplained.md](https://github.com/turmeric-lang/turmeric/blob/main/docs/reported/macos-jit-leg-stall-unexplained.md).
The next occurrence will produce a log.

**Severity: medium.** The hang itself gates -- `JIT engine (macos-latest)` is
the one JIT leg that is not `continue-on-error`, so it fails the run. What
makes this worth its own report rather than a line on the archived one is the
second half: the instrumentation added in 2026-08-02 specifically so the next
occurrence would be diagnosable **produced nothing**, so occurrence #5 will be
as blind as #1 unless it is fixed.

Filed 2026-09-28, from rjungemann/turmeric#953.

## The occurrence

| | |
|---|---|
| Run | [36385448273](https://github.com/turmeric-lang/turmeric/actions/runs/36385448273), job 108809755220 |
| Step | `Run JIT suites`, started 06:14:54Z |
| End | job `cancelled` 07:09:17Z by `timeout-minutes: 45` |
| Wall | ~54 min job, ~48 min in the step |

Baseline for that step on the two immediately preceding `main` runs:
**13m and 19m** (runs 36379252305, 36381500505). So this is 2.5-3.5x the
recent worst case, not a slow runner.

It is not the PR's doing. #953 touches `web/`, `docs/`, `src/web/wasm_glue.c`
and `tests/wasm_glue_lang_unit.c`; `wasm_glue.c` links into `libturi_wasm` and
the two `tur_wasm_glue_*_unit` tests, and the step runs only
`tur_jit_fixture_tests`, `tur_repl_spice_jit` and `tur_flags_tests`. Nothing
in the diff executes there. The other 17 checks on that run passed, including
`Test (macos-latest)`.

## Both diagnostics were lost

[docs/archive/macos-jit-leg-intermittent-45min-hang.md](https://github.com/turmeric-lang/turmeric/blob/main/docs/archive/macos-jit-leg-intermittent-45min-hang.md)
rebuilt this step in 2026-08-02 so a hang could not be silent again: `tee
jit-ctest.log | grep --line-buffered` to stream progress to the console, and
an `Upload JIT ctest log` step with `if: always()` -- "which covers the
cancellation a timeout kill produces, so the partial log survives".

Neither survived:

- **The artifact upload never ran.** After the kill the job's steps read
  `10 Run JIT suites -- in_progress`, then `11 Upload JIT ctest log -- pending`,
  `12`, `13`, `14` all `pending`. A `timeout-minutes` kill does not run the
  remaining `if: always()` steps. The run's artifact list has
  `jit-ctest-log-ubuntu-latest` and no macOS counterpart.
- **The console log is gone too.** `GET /actions/jobs/108809755220/logs` is
  `BlobNotFound`, `gh run view --job ... --log` is `log not found`, and the
  run-level log archive contains `JIT engine (ubuntu-latest)/` and the Windows
  legs but no `JIT engine (macos-latest)` step files at all.

The archived report's "Confirmed working on run 30767617172" was a run that
**succeeded** -- so the `if: always()` claim was never exercised under the
condition it was written for. That is the defect.

## The 2026-08-18 containment is still in place

Worth stating, because it is the obvious first suspect and it is not the
cause. `ci.yml:384` still reads `brew install libedit ccache coreutils || true`,
and the hung run's `Install dependencies (macOS)` step took **4 seconds**
against **~3.5 seconds** for the successful install on run 36381500505
(`Pouring coreutils--9.11.arm64_tahoe.bottle`). So `gtimeout` was present and
per-fixture timeouts were live. `httpd-async-limit`'s own deadlock was
root-caused and fixed in the same archived report and is not implicated.

That leaves a stall somewhere a per-fixture `gtimeout` does not reach.

## Where a stall can still reach the job wall

Three structural gaps, all verified against the tree at `30ca2df47`:

1. **`tests/run-flags.sh` has no timeout wrapper at all.** `run-jit.sh` and
   `run.sh` carry the `_tur_timeout_bin` probe and `_run_timed`; `run-flags.sh`
   contains neither string. `tur_flags_tests` is in this leg deliberately (the
   CI comment: it is "the only harness carrying the `jit-ffi-*` cases", which
   are gated on the engine and skip entirely in the `test` job), and those
   cases exercise dynamic FFI, callbacks and threads -- exactly the shapes that
   hang.
2. **None of the three targets carries a ctest `TIMEOUT` property.** Verified
   with `ctest --show-only=json-v1`: `tur_flags_tests` reports `NO TIMEOUT
   PROPERTY`, and `tur_jit_fixture_tests` / `tur_repl_spice_jit` are declared
   at CMakeLists.txt:1264-1282 with `RUN_SERIAL TRUE` and nothing else. The
   same file sets explicit `TIMEOUT` on many other targets (300, 600, 720,
   900), so this is an omission rather than a policy.
3. **`_run_timed` covers the fixture invocations, not the phases around
   them.** `run-jit.sh` wraps `"$TUR" ... jit "$input"` at lines 320/324/413.
   A stall in harness setup, or in an untimed compile/link, is outside it --
   which is the archived report's own point 3 about where a 45-minute stall is
   likeliest to live.

## The fix (landed 2026-09-28)

Three bounds, outermost first. Each one alone would have salvaged this
occurrence; together they narrow it from "the leg hung" to "this test hung, and
here is its log".

- **`ci.yml`, `Run JIT suites`: `perl -e 'alarm 2100; exec @ARGV'` heads the
  ctest pipeline.** 35 minutes against a 13-19 min baseline for the step, ~10
  minutes inside the job's `timeout-minutes: 45`. `perl -e alarm` rather than
  `timeout`, because stock macOS ships no coreutils `timeout` -- the same idiom
  as the engine-present probe at the top of the job. It heads the pipeline so
  `PIPESTATUS[0]` carries its 142 (128+SIGALRM) and the step FAILS; a failing
  step runs the remaining `if: always()` steps, which is the property a
  `timeout-minutes` kill does not have and the reason the artifact was lost.
  The step also prints a `::error::` line saying what happened, so a reader of
  the summary is not left inferring it from an exit code. The upload step's
  comment no longer claims `always()` covers a timeout kill -- it does not, and
  that wrong claim is what made the gap invisible for two months.
- **`CMakeLists.txt`: `TIMEOUT` on all three targets.**
  `tur_jit_fixture_tests` 1500 (~1.6x its ~940s p90 on the 3-core runner CI
  draws ~97% of the time), `tur_repl_spice_jit` 600, `tur_flags_tests` 900.
  These are hang bounds, not performance assertions; the values are documented
  as such next to them. The per-test kill is what names the test, which is most
  of the diagnosis. Their sum exceeds the 2100s alarm deliberately: the alarm is
  the backstop for several targets running long at once, while these catch the
  single hang -- which is what has actually happened, four times.
- **`tests/run-flags.sh`: a per-invocation timeout, the harness's first.** Every
  case invokes the compiler as `"$TUR" ...` and nothing else uses `$TUR`, so
  `$TUR` becomes a generated wrapper that runs the real binary under
  `timeout -k 5 ${TUR_CASE_TIMEOUT:-180}`. One indirection over ~100 call sites
  -- and over the cases added later, which is exactly what a hand-edited call
  site gets wrong. `-k` because `tur` spawns a C compiler and a SIGTERM to the
  parent alone can leave that child running. The real binary stays argv[0]
  (`resolve_stdlib_root`'s last-resort path hint), and with neither `timeout`
  nor `gtimeout` present the wrapper is skipped -- the same tradeoff `run.sh`
  makes. Its `trap ... EXIT` is now one trap for the whole file: the jit-ffi
  block's own trap would have replaced it, bash keeping only the last.

## What is NOT fixed

The stall. This occurrence's evidence no longer exists, so there is nothing to
diagnose from -- see
[docs/reported/macos-jit-leg-stall-unexplained.md](https://github.com/turmeric-lang/turmeric/blob/main/docs/reported/macos-jit-leg-stall-unexplained.md).
The three structural gaps above are now closed as *gaps*; whether one of them
was also the *cause* (a `run-flags.sh` jit-ffi case deadlocking, say) is exactly
what the next occurrence will say and this one cannot.

## Note on the archive

The archived report's root cause (a missing `timeout(1)` turning
`httpd-async-limit`'s listen-fd deadlock into a job kill) was genuinely fixed,
and its fixes are still in the tree -- so it stays archived rather than
returning here. This report is the different defect wearing the same symptom,
and the archive carries a forward pointer to it.
