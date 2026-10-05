---
title: Running the Tests
category: Contributor Notes
description: Which harness runs what, how to narrow a run to the tests you care about, the fixture marker files, and how to reproduce a red CI job locally
---

# Running the Tests

This guide answers "how do I run the tests I care about, right now". It is
task-oriented. The harness internals and the traps that have burned us live
elsewhere:

- [test-suite-portability-guide.md](test-suite-portability-guide.md) --
  portability and performance pitfalls (Bash 3.2, ASan, leak-check coverage,
  sharding parity, timeout budgets).
- [test-runner-contract.md](test-runner-contract.md) -- the contract for
  writing a test framework or harness.
- [diagnosing-the-compiler-guide.md](diagnosing-the-compiler-guide.md) --
  debugging the compiler once a test has told you something is wrong.

Two rules hold for every command below:

- **Wrap every suite run in a 12-minute timeout** (`timeout 720 ...`, or
  `timeout: 720000` in the Bash tool). See CLAUDE.md.
- **Run one suite at a time.** Two suites, or a suite and a build, on the same
  machine produce failures that look exactly like product bugs. See
  CLAUDE.md, "Overlapping runs cause false FAILURES".

## 1. Which harness runs what

| Harness | What it runs | Typical use |
| --- | --- | --- |
| `tests/run.sh` | Every fixture under `tests/fixtures/`, compiled to a native binary through `cc` | The main suite |
| `tests/run-turi.sh` | The same fixtures under `tur --interpret` | Interpreter changes |
| `tests/run-jit.sh` | The same fixtures through `tur jit` (MIR) | JIT changes |
| `tests/check-*.sh` | One invariant each (a lint, a sync check, a ratchet) | Usually as named by a CI failure |
| `tests/run-*.sh` (others) | One focused harness each (r7rs conformance, leak gates, debugger, ...) | Usually as named by a CI failure |
| `ctest --test-dir build` | Every harness above, each registered as one ctest target | What CI actually runs |

Each ctest target is one script. To see which script a target runs:

```sh
ctest --test-dir build -N -V -R '^tur_r7rs_gc$' | grep 'Test command'
# Test command: /usr/bin/bash "tests/run-r7rs-gc.sh"
```

Then run that script directly. It is faster than going through ctest, and
its output is not truncated.

## 2. Narrowing a run

### `tests/run.sh`

| Variable | Meaning | Example |
| --- | --- | --- |
| `TUR_TEST_FILTER` | Regex; run only fixtures whose name (path under `tests/fixtures/`) matches | `TUR_TEST_FILTER='^(arith|r7rs-srfi-1)$'` |
| `TUR_TEST_EXCLUDE` | Regex; leave out matching fixtures (applied after the filter) | `TUR_TEST_EXCLUDE='^r7rs-'` |
| `TUR_TEST_SUITE` | `happy`, `errors` (negative fixtures under `errors/`), or `snapshots` (fixtures with `expected.c`) | `TUR_TEST_SUITE=errors` |
| `TUR_TEST_SHARD` | `i/N`: run the i-th of N disjoint slices | `TUR_TEST_SHARD=2/3` |

They compose. One fixture, from scratch:

```sh
TUR_FORCE=1 TUR_TEST_FILTER='^arith$' timeout 720 bash tests/run.sh
```

The output goes to **stdout**: a `PASS`/`FAIL` line per fixture, then
`summary: P passed, F failed`. A failing fixture's `actual.stdout` and
`actual.stderr` are left in its directory.

There is no `TUR_FILTER`. An unknown variable is silently ignored, and you
get the full suite.

### `tests/run-turi.sh`

| Variable | Meaning |
| --- | --- |
| `TURI_FILTER` | `grep -E` pattern on the fixture name. `TUR_TEST_FILTER` is accepted as an alias; `TURI_FILTER` wins when both are set |
| `TUR_TURI_SHARD` | `i/N`, parsed like `TUR_TEST_SHARD`. A separate name on purpose: the interpreter suite runs inside CI's `aux` ctest part, where `TUR_TEST_SHARD` would mislabel the other suites' timings |
| `TURI_TEST_LIST` | `1`: print the fixtures that would run, and run nothing |

```sh
TURI_FILTER='r7rs-srfi' timeout 720 bash tests/run-turi.sh
```

The summary reads `P passed, F failed, S skipped of D discovered`. The run
fails if those do not add up.

### `tests/run-jit.sh`

`TUR_TEST_FILTER` (or `JIT_FILTER`, which wins), `TUR_TEST_SHARD`, and
`TUR_TEST_LIST=1` (list only), as in `tests/run.sh`.

### ctest

```sh
timeout 720 ctest --test-dir build -R 'r7rs' -E 'conformance'
```

`-R` and `-E` are regexes on the target name. CI's parts (below) are exactly
such patterns.

## 3. Caching, parallelism and binaries

| Variable | Harnesses | Meaning |
| --- | --- | --- |
| `TUR_FORCE` | all three | `1`: ignore the pass cache and run everything. A passing fixture is skipped next time unless its files, the `tur` binary, `stdlib/` or the build configuration changed. Force it after changing a file the fixture `load`s from elsewhere, or after a C compiler upgrade |
| `TUR_STAMP_CACHE` | `run.sh` | Where the cache lives (default `tests/.stamp-cache`); empty disables it |
| `TUR_TEST_JOBS` | all three | Parallel workers. `run.sh` defaults to physical cores capped at 8, the other two to the CPU count; an explicit value is not capped |
| `TUR` | all three | The compiler under test: default `./build/tur`, or `./build-turjit/tur` for `run-jit.sh`. E.g. `TUR=./build-release/tur` |
| `TUR_CC_FLAGS` | `run.sh`, `run-jit.sh` | Flags for compiling fixture programs |
| `TUR_USE_CCACHE` | `run.sh` | `0` to stop using ccache for fixture builds (default `1` when ccache is installed) |
| `TUR_EMIT_C_MODE` | `run.sh` | `always` to run `emit-c` for every fixture, not only those with `expected.c` |
| `TUR_TEST_TMPDIR` | `run.sh` | The scratch directory fixtures write to (default: a fresh temp dir) |

## 4. Variants and gates

| Variable | Harnesses | Meaning |
| --- | --- | --- |
| `TUR_STRESS` | all three | `1`: also run the `requires.stress` fixtures (the full-size twins of per-PR fixtures; the nightly sets it) |
| `TUR_TSAN` | all three | `1`: build and run under ThreadSanitizer, and run the `requires.tsan` fixtures |
| `TUR_SANITIZER_GATE` | `run.sh` | `1`: sanitizer findings in `tur` fail the run (by default they are collected and reported) |
| `TUR_SANITIZER_LOG_OUT` | `run.sh` | Where to copy the sanitizer findings log (default: next to the compiler) |
| `TUR_SKIP_PARITY_CHECK` | `run.sh` | `1`: skip the interpreter-parity ratchet at startup (for a half-finished `eval.c` change) |
| `TUR_SKIP_CC_WARN_CHECK` | `run.sh` | `1`: skip the emitted-C warning ratchet |
| `TUR_SKIP_CROSSING_CHECK` | `run.sh` | `1`: skip the carrier-crossing audit |
| `TUR_JIT_FALLBACK_UPDATE` | `run-jit.sh` | `1`: rewrite the cc-fallback baseline (refused in a sharded run) |
| `TUR_HEADROOM_TOP` | `run-jit.sh` | How many of the slowest fixtures to report (default 8) |

Leak checking of *emitted* programs is a separate harness,
`tests/run-leak-check.sh`, for fixtures carrying `requires.leak-check`.
`run.sh` checks `tur` itself for leaks, not the programs it compiles. See
[test-suite-portability-guide.md](test-suite-portability-guide.md#7a-leak-checking----what-is-covered-and-what-is-not).

## 5. Fixture files and markers

A fixture is a directory under `tests/fixtures/` (negative fixtures under
`tests/fixtures/errors/`). These are the files the three fixture harnesses
read. A file not in this table is ignored, so a misspelled marker silently
removes a check.

**What the fixture is**

| File | Meaning |
| --- | --- |
| `input.tur` (or `<dirname>.tur`) | The program |
| `hook.sh` | The fixture drives its own build and run instead (`run.sh`, `run-jit.sh`) |
| `flags` | Extra compiler flags, one line |
| `run.args` | Arguments for the program, one line |
| `input.stdin` | Fed to the program's stdin |

**What it must do**

| File | Meaning |
| --- | --- |
| `expected.stdout` | The program's exact output. Without it the output is not checked at all |
| `expected.exit` | The expected exit status (default 0) |
| `expected.stderr` | Substrings that must appear in stderr |
| `expected.diag` | Negative fixtures: substrings the compiler's diagnostics must contain, one per line |
| `expected.c` | Codegen snapshot, compared on every run. `TUR_TEST_SUITE=snapshots` runs only these; regenerate with `tur run regen-snapshots` |
| `expected.timeout` | Seconds for each of the build and the run (default 10 compiled, 15 interpreted; `0` means unlimited). r7rs fixtures that compile a library or a large program usually need `60` |
| `expected.xfail` | A named failing test: the stdout mismatch passes as `(xfail)`, and a match fails with "delete expected.xfail" |

**When it runs**

| Marker | Effect |
| --- | --- |
| `requires.compiled` | Skipped by `run-turi.sh` (compiled-only behaviour, or too heavy to interpret) |
| `requires.interp-only` | Skipped by `run.sh`; `run-turi.sh` owns it |
| `requires.interp` | `run.sh` runs it through the interpreter instead of compiling |
| `requires.tur-only` | Skipped by `run-turi.sh`: a feature the interpreter deliberately omits |
| `requires.cc` | Skipped by `run-jit.sh`: genuinely cc-only |
| `requires.dedicated-runner` | Skipped by the fixture harnesses; its own ctest target runs it |
| `requires.stress` | Skipped unless `TUR_STRESS=1` |
| `requires.tsan` | Skipped unless `TUR_TSAN=1` |
| `requires.posix-apis` | Skipped when producing Windows binaries |
| `requires.spices` | Skipped when `../turmeric-spices/` is absent |
| `requires.musttail` | Skipped when the fixture compiler cannot honour `TUR_MUSTTAIL` |
| `requires.no-leak-check` | The program runs with leak detection off |
| `requires.leak-check` | Opted in to `tests/run-leak-check.sh` |
| `known.fnsan` | A known `-fsanitize=function` trap, handled by `tests/run-fnsan.sh` |

CLAUDE.md, "Fixture Files", has the reasoning behind several of these.

## 6. Reproducing a red CI job

Every check in `.github/workflows/ci.yml`, and the local command that runs
the same thing:

| CI job | Local command |
| --- | --- |
| Docs lint | `bash tests/check-docs-lint.sh` |
| Test (ubuntu/macos) | `bash tests/run.sh` (the `fixtures` part is the one ctest target `tur_tests`, which runs it) |
| Auxiliary suites | `ctest --test-dir build -E "$(python3 tests/ctest-parts.py pattern aux --leg linux)"` |
| R7RS suites | `ctest --test-dir build -R "$(python3 tests/ctest-parts.py pattern r7rs --leg linux)"` |
| Float conversions | `ctest --test-dir build -R "$(python3 tests/ctest-parts.py pattern fconv --leg linux)"` |
| Generic spec matrix 1/2, 2/2 | the `gsm` part: `ctest --test-dir build -R "$(python3 tests/ctest-parts.py pattern gsm --leg linux)"` |
| Interpreter without JIT | `bash tests/run-turi.sh`, on a Debug build configured with `-DTUR_JIT=OFF` |
| JIT engine | `ctest --test-dir build` on a JIT build; the fixture part is `bash tests/run-jit.sh` |
| Whole runtime preamble (cc path) | `TUR_PREAMBLE_SPLIT=0 bash tests/run.sh` |
| Indirect-call type check | `CC=clang TUR_FNSAN_LIB_DIR=build-nosan/src bash tests/run-fnsan.sh` |
| Check codegen snapshots | `./build/tur run regen-snapshots -- --check` |
| Windows build + suite i/3 | `TUR_TEST_SHARD=i/3 bash tests/run.sh` (on Windows: `TUR=./build-win/tur.exe`) |

Use `--leg macos` for a macOS job's pattern. `python3 tests/ctest-parts.py`
with no arguments checks that the parts between them cover every registered
target.

Inside a failed job's log, look for these lines:

- **`FAIL <name> -- <reason>`** is one fixture. Rerun it alone:
  `TUR_FORCE=1 TUR_TEST_FILTER='^<name>$' timeout 720 bash tests/run.sh`.
- **`<N> - <target> (Failed)`** in ctest's summary is one target. Find its
  script with `ctest -N -V -R '^<target>$'`, then run the script.
- **`tur build timed out (>10s)`** is the build budget, not a hang. It is
  common on the Windows runners for r7rs fixtures; give the fixture an
  `expected.timeout`.
- **`timed out (>15s under --interpret)`** is the interpreter's run budget.
  Check the fixture's peak memory first: the interpreter keeps about 2-4 KiB
  per step, and fixtures run side by side.
- **`passes now; delete expected.xfail`** means the gap the marker recorded
  has closed. Delete the marker.

A failure on Windows or macOS that you cannot reproduce on Linux is often
timing. Before calling it a product bug, check whether the failing fixture
passed on that platform in an earlier run of the same shard.

## 7. Keeping this guide true

`tests/check-running-tests-guide.sh` (part of Docs lint) fails when
`tests/run.sh`, `tests/run-turi.sh` or `tests/run-jit.sh` reads a
`TUR_*`/`TURI_*` variable with a default that this guide does not mention.
Adding a variable to a harness means adding a row here.
