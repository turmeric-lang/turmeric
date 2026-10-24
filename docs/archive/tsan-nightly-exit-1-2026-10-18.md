# Nightly TSan run failed with exit 1 (Sentry `turmeric-ci` 7784962154)

**Resolved 2026-10-23: TSan was never told about the fiber switches.** The
run's own log (run 39115563799, job "TSan concurrency fixtures") named the
fixture: `FAIL fiber-scheduler-mt-migration -- stdout mismatch`, expected
`360`, got nothing, 76 of 77 passing. It is one failure in 23 nights of the
workflow, and it reproduces locally about once in 150 runs of that fixture's
TSan binary (Release `tur`, `-O2 -fsanitize=thread`): `ThreadSanitizer: SEGV
... WRITE memory access`, then "nested bug in the same thread, aborting" --
exit 66 from the program, inside TSan's own runtime, before anything is
printed.

The fixture is nine fibers on a three-thread `tur_scheduler_mt` that migrate
between workers at every yield. The fiber runtime switches with
`swapcontext`, and ThreadSanitizer keeps a shadow stack per thread: a switch
it is not told about leaves that state describing the wrong stack, which is
unsupported and, here, occasionally fatal. The emitted runtime now announces
every switch when it is compiled under `-fsanitize=thread`
(`TUR_TSAN_FIBERS`, `src/compiler/emit_module.c`): each `FiberBlock` gets a
TSan fiber (`__tsan_create_fiber`, destroyed with the block), and
`tur_fiber_block_resume` / `_yield` and the shim's final switch call
`__tsan_switch_to_fiber` first. Without TSan the macros are empty. With them,
450 runs of the fixture's binary are clean (and 600 of a hand-patched one),
and `tests/run-tsan.sh` passes all 80 fixtures.

## As filed

**Severity: unknown until triaged.** Filed from the Sentry issue alone; the
run log was not available when this was written, so the cause is open.

## Report

| Field | Value |
| --- | --- |
| Sentry issue | `7784962154`, project `turmeric-ci` |
| Title | `unknown: TSan: nightly run failed (exit 1)` |
| Date | 2026-10-18 09:31:41 UTC |
| Release (head) | `7af0e323f991b62e3291954361f3f6d474fab294` (merge of PR #1282, `claude/extras-plan-status`) |
| Tags | `kind=tsan`, `target=tsan`, `transaction=tsan`, `exit_code=1`, `ci=github-actions`, `level=error` |
| Trace | `da5900d88bd7439b80279abf52999639` / span `da5900d88bd7439b` |

Raised by the "Report the findings to Sentry" step of
`.github/workflows/tsan.yml` (`rc != 0` from `tests/run-tsan.sh`).

## What the exit code says

The workflow's own note: "An exit 66 is a TSan report." This run exited **1**,
so it was most likely **not** a ThreadSanitizer race report. Exit 1 is the
harness's generic failure: a fixture that failed to build, a stdout mismatch, a
`timed out` line, or a fixture-selection error. A race would have come back as
66 with the race in the fixture's stderr.

The Sentry event carries no fixture names, so which fixture failed is not
recorded here. The grouping fingerprint is `tsan` plus a hash of the sorted
`FAIL ` lines (`tsan-failures.txt`), and the event's `--log` and `--attach`
payloads (`tsan-failures.txt`, `tsan.log`) are where they are.

## To triage

1. Open the Sentry issue and read the attached `tsan.log`; the `FAIL <name> --`
   lines name the fixture(s) and the reason.
2. Check the run for 2026-10-18 on the TSan workflow: did `Build` or the smoke
   check pass, or did the run die before `tests/run-tsan.sh`? (The Sentry step
   runs only after the harness step, which swallows its own exit code.)
3. Reproduce locally with a non-ASan `tur` (the Release tree the workflow
   builds), as the workflow's note says:
   `TUR=./build-tsan/tur timeout 720 bash tests/run-tsan.sh`.
   Run it alone: an overlapping build or suite produces false failures (see
   CLAUDE.md, "Overlapping runs cause false FAILURES").
4. Compare against a night where it passed, by the release SHA, to bound the
   change. Recent TSan work: the release/tsan build-tree split is the one the
   sibling report [tsan-cmake-config-fails-to-link](../archive/tsan-cmake-config-fails-to-link.md)
   came from (since fixed), but that one affected only `-DCMAKE_BUILD_TYPE=TSan`, which the
   workflow does not use, so it is unlikely to be the cause.

## Fix directions

Unknown until step 1. If the log shows a fixture that is simply slow under
TSan, give it `expected.timeout` or an `EXCLUDE` entry in `tests/run-tsan.sh`
with the reason beside it; if it is a real race, reduce it and file it on its
own; if the log shows a build failure, it is an ordinary regression on that head.
