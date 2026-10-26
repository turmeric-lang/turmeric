---
title: Test Runner Contract
category: CLI Tools
description: Test framework API and contract
tags:
  - testing
  - test-runner
  - assertions
  - fixtures
  - contract
---

# Test Runner Contract

This document defines the expected behavior for stdlib test running.

## Scope

Applies to:

- stdlib test helpers in `stdlib/test.tur`
- CLI test entry behavior (the `tur test` command path)
- fixture and output conventions for pass/fail reporting

## Discovery Model

1. Directory mode scans `tests/fixtures/**` for test-bearing fixture directories.
2. A fixture is considered runnable if it has `input.tur`.
3. Error fixtures are identified by `tests/fixtures/errors/**` and validated against `expected.diag`.
4. Non-error fixtures are validated against `expected.stdout` where provided.

## Registration Model

1. `deftest` registers test definitions in a global registry in declaration order.
2. `run-tests!` executes registered tests in deterministic order (declaration order).
3. Duplicate test names are rejected with a diagnostic.

## Assertion Semantics

- `assert expected actual`: pass iff values are equal.
- `assert-true x`: pass iff `x` is boolean true.
- `assert-false x`: pass iff `x` is boolean false.
- `assert-nil x`: pass iff `x` is nil.
- `assert-error body`: pass iff evaluating `body` emits an error per test harness policy.

## Callback Callability Contract (v1)

- Test callbacks are passed through function parameters typed as `:ptr<void>`.
- Passing a function value where `:ptr<void>` is expected is supported.
- Passing `nil` where `:ptr<void>` is expected is supported as a null-callback convention.
- In v1, callback invocation through this bridge is arity-0 only.
- Non-zero-arity invocation through `:ptr<void>` is a compile-time arity mismatch.
- Callback calls lower to typed function-pointer casts in emitted C.

## Process Exit Semantics

- Exit code `0`: all tests pass.
- Exit code `1`: at least one test fails.
- Exit code `2`: harness/runtime/configuration error prevented reliable execution.

## Output Contract

Streaming output:

- `.` per passing test
- `F` per failing test

Summary output at end:

- total tests
- passed
- failed
- elapsed (optional)

Failure detail block includes:

- test name
- assertion message / diagnostic snippet
- source location if available

## Stdout/Stderr Split

- Progress markers and summary go to stdout.
- Diagnostics, harness errors, and failure details may go to stderr.
- Contract tests should match whichever stream is explicitly specified for a fixture.

## Determinism Requirements

- Test order is stable.
- Output order is stable.
- Summary counts are reproducible.

## Multi-Threaded Fixture Support (T19)

### Timeout (`expected.timeout`)

A fixture directory may contain an `expected.timeout` file whose content is an
integer number of seconds.  The test runner kills the compiled binary and marks
the fixture as failed if it runs longer than this timeout.  The default when the
file is absent is **10 seconds**.  Set the value to `0` to disable the timeout
for a fixture.

The runner uses `timeout(1)` (GNU coreutils), `gtimeout` (Homebrew coreutils on
macOS), or a `perl -e 'alarm N'` fallback.

### ThreadSanitizer (`requires.tsan` / `TUR_TSAN=1`)

A fixture directory may contain an empty `requires.tsan` marker file.

- When `TUR_TSAN` is **not** set (default): fixtures with `requires.tsan` are
  **skipped** (counted as PASS with a `(tsan-skipped)` detail).
- When `TUR_TSAN=1`: all fixtures run normally, including those with
  `requires.tsan`.  The compiler flags gain `-fsanitize=thread -g`.

Run the concurrency fixtures under TSan with the dedicated recipe (Justfile,
via `tur run` or `just`):

```sh
tur run test-tsan
```

It builds a Release (non-ASan) `tur` into `build-tsan/`, then runs
`tests/run-tsan.sh` against it: every `requires.tsan` fixture plus the thread, STM, channel and scheduler
families, the same selection the nightly TSan workflow runs. For every fixture,
point `TUR` at that build:

```sh
TUR=./build-tsan/tur TUR_TSAN=1 bash tests/run.sh
```

Not at the default `build/tur`: that one is ASan-instrumented, fixture
programs link the runtime archives next to it, and ASan and TSan cannot share
a process.

## Failures That Are Not Product Bugs

Three failure shapes in this tree look exactly like product regressions and
are not. Recognize them before you start bisecting. See
[test-suite-portability-guide.md](test-suite-portability-guide.md) for the
platform-divergence counterparts (vacuous enumerations, heap probes under
ASan, harness env parity, unspecified string-literal merging).

### Sanitizers launder crashes into passes

ASan and UBSan abort with **exit code 1** on a deadly signal. Any
fork-and-classify harness whose outcome enum uses small exit codes will
therefore tally a sanitizer-killed child as whatever category owns code
`1` -- the crash disappears into a legitimate-looking bucket and the
summary stays green.

Two requirements for any such harness:

- Use **distinctive** exit codes for the outcome enum. The SMT-LIB corpus
  runner moved its enum to `40..46` for exactly this reason.
- Classify any *unexpected* exit status as a **crash**, never as a
  category. A default arm that maps unknown codes onto a real outcome is
  the bug.

### Overlapping runs produce failures that read as product bugs

Two distinct causes, both observed:

- **`ctest -jN` oversubscribes.** `tests/run.sh` and `tests/run-turi.sh`
  each already fan out across `nproc` internally, so `-j4` is `4 x nproc`
  processes on the box, and the per-fixture timeouts (10s compiled, 15s
  interpreted) expire on work that would otherwise finish comfortably.
  Both targets are marked `RUN_SERIAL` in `CMakeLists.txt` so ctest gives
  them the machine, which is what they already assumed.
- **A concurrent `cmake --build` relinks the compiler mid-run.** Fixtures
  exec `./build/tur` straight out of the build tree. During the link
  window the file exists but is not yet executable, so everything
  dispatched in that window dies with `Permission denied`, which the
  harness reports as `build failed`. A batch of those reads as a compiler
  regression.

`tests/run.sh` stamps the binary at startup, re-checks at the end, and
exits `2` with a `WARNING: ... changed while this run was in progress` if
it moved. Other harnesses do not, so learn the shape instead:
**an assertion that passes when you run it by hand was probably never
really run.** Re-run alone before believing a failure, and never launch a
build and a suite concurrently.

### Contract fixtures must pin `--keep-contracts` themselves

A fixture that asserts contract or refinement **runtime** behavior must
put `--keep-contracts` in its own `flags` file. It must never inherit that
behavior from how `tur` happened to be built: a Debug `tur` checks
contracts, a Release `tur` strips them under `NDEBUG`.

Nine `refine-*` fixtures inherited Debug-ness, passed under
`tests/run.sh`, and failed under the Release-built `tests/run-jit.sh`,
where the check never fired. The same applies to diagnostics that depend
on CT1 obligation injection, such as `TUR-E0375` -- with contracts
stripped the obligation is never injected and the diagnostic is never
reported.

If the fixture asserts a runtime abort, `--keep-contracts` is part of what
it is testing. Declare it.

## Follow-up Work Hooks

- Integrate this contract with `stdlib/test.tur` implementation work.
- Integrate with `tur test` CLI command behavior and fixture validations.
