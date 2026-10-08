---
title: Diagnosing the Compiler
category: Contributor Notes
description: How Turmeric contributors narrow a suspected compiler bug -- ruling out the environment, finding the failing stage, the dump flags, three-way back-end comparison, the float and leak checkers, the source fuzzers, and the test-harness traps that produce false failures
---

# Diagnosing the Compiler

This guide is for contributors chasing a bug in **Turmeric itself**: the
compiler accepts a program and then the C does not compile, the program
crashes, or it prints the wrong answer. If the bug is more likely in your own
program, start with [Debugging Turmeric Programs](debugging-guide.md); the
lldb, `tur debug` and sanitizer recipes there apply here too.

The order of the sections is the order to work in: rule out the environment,
reproduce small, find the stage, then look inside it.

## 1. Rule out the environment first

Most "the compiler is wrong" reports that do not reproduce come from one of
these. Each takes seconds to check.

- **Run the binary you just built.** Use `./build/tur`, not the `tur` on
  `PATH`. A package-manager or tvm install is often older than your tree, and a
  version mismatch explains most impossible-looking errors. `./build/tur
  --version` and `command -v tur` tell you which is which.
- **Check `TUR_STDLIB_DIR`.** It overrides where the standard library is
  loaded from. mise's `python3` shim exports it, so a Python-driven harness
  (the fuzzers, the float checker) can silently build against an old installed
  stdlib. That shows up as `Result`/`Option` "codegen" bugs that do not
  reproduce by hand. `tests/run.sh` unsets it; when in doubt, `unset
  TUR_STDLIB_DIR` and re-run.
- **Use a Debug build for contract and refinement work.** A Release build of
  `tur` defines `NDEBUG`, which strips runtime contracts, so contract and
  refinement fixtures fail spuriously against it.
- **Make sure nothing else is building or testing.** See
  [section 6](#6-false-failures-from-the-harness).

## 2. Reproduce it small

Get the bug into a single short `.tur` file before anything else. Two habits
save a round trip:

- **Probe floats with a fractional value.** Use `7.1` or `3.25`, never `7` or
  `7.0`: an integer-valued float cannot show truncation, rounding, or an
  int/float bit confusion, so it reports "looks fine" on exactly the bugs you
  are looking for.
- **Make inputs opaque to the C optimizer.** A literal argument at `-O2` is
  constant-folded, and a small call cycle is inlined into a loop, so you end up
  measuring clang rather than Turmeric. Read the input from `*args*` (or a
  value the optimizer cannot see), and build at `-O0` with
  `TUR_CC_FLAGS="-O0 -std=c99 -fno-strict-aliasing"` when the question is
  about what Turmeric emitted.

## 3. Find the failing stage

Each command stops at a later point in the pipeline, so the first one that
fails tells you where the bug is:

| Command | Runs | A failure here means |
|---|---|---|
| `tur parse-check a.tur b.tur.sweet` | the reader, on both files | two spellings read to different ASTs (a reader bug) |
| `tur check file.tur` | reader, macro expansion, elaboration, type/kind/effect/borrow checks | a checker bug, or a genuinely wrong program |
| `tur expand file.tur` | as `check`, printing each macro expansion | a macro expands wrongly |
| `tur emit-c file.tur` | as `check`, plus C emission | the emitter crashed or produced nothing |
| `tur build file.tur -o out` | as `emit-c`, plus the C compiler and linker | **`check` accepted something the C compiler rejects** -- an emitter bug |
| `./out` | the program | a crash or wrong output: emitter or runtime |

The last two rows are where most compiler bugs land. The source fuzzers
([section 7](#7-fuzzers)) classify failures the same way: `BUG_invalid_c` (cc
rejected the emitted C), `BUG_link`, `BUG_crash`, `BUG_wrong_output`.

## 4. Look inside a stage

### The generated C

`tur emit-c file.tur > out.c` gives you the C. Add `--debug` to get `#line`
directives, so a compile error or a debugger frame names `.tur` lines. Turmeric
names are mangled into C identifiers; `tur demangle` reverses them, and
[the name mangling guide](name-mangling-guide.md) has the scheme.

`TUR_CC_FLAGS` replaces the flags passed to the C compiler (the default is
`-O2 -std=c99 -Wall -fno-strict-aliasing`, or `-g -Og ...` under `--debug`).
Use it to drop to `-O0`, to add `-fsanitize=...`, or to make warnings fatal.

### Dump flags

These print an intermediate result and work with `tur check` (some write to
stderr, so capture with `2>&1`):

| Flag | Shows |
|---|---|
| `--dump-kinds` | the kind of every type constructor and class parameter |
| `--dump-effects` | the inferred effect row of every function |
| `--dump-sizes` | sized-type information |
| `--dump-cps` | the CPS IR, function by function |
| `--dump-mono-specs` | the by-value monomorphization specializations |
| `--dump-clone-plan`, `--dump-cps-coloring`, `--dump-reflect` | narrower internals of the same passes |
| `--emit-abi-trace` | the ABI decisions made for each function |

The documented ones are described in the [compiler flags guide](compiler-flags-guide.md).

### Float/int conversions

`python3 tests/check-emitted-float-conversions.py FILE.tur` reports every
float-to-int or int-to-float **value** conversion in the emitted C that the
program did not ask for. The corpus is at zero findings, so any finding is new
and is almost certainly a bug. A conversion the emitter makes on purpose is
spelled `TUR_AS(T, x)` and is not reported. The
[value representations guide](value-representations-guide.md#detecting-the-mechanism-not-the-shape)
explains why this checks the mechanism rather than particular program shapes.

## 5. Compare the back ends

Turmeric has three ways to run a program, and they share the front end but not
the back end:

```sh
./build/tur run file.tur         # compile through C, run
./build/tur interpret file.tur   # tree-walking interpreter (turi)
./build/tur jit file.tur         # in-process MIR JIT
```

- **Compiled and JIT disagree, interpreter agrees with one:** the JIT sees
  different C than `tur build` (a split runtime preamble and pruned
  definitions). `TUR_JIT_NO_SPLIT=1 TUR_JIT_NO_PRUNE=1` removes most of that
  difference; the [JIT guide](jit-guide.md) has the rest.
- **Compiled and interpreted disagree:** check the
  [interpreter parity guide](turi-parity-guide.md) first. Inline C, for one, is
  a permanent interpreter carve-out, not a bug.
- **All three agree on the wrong answer:** the bug is upstream of every back
  end -- the reader, elaboration, or the type checker.

## 6. False failures from the harness

The suite can produce failures that look exactly like compiler regressions.
Before believing a red result, check for these.

- **A rebuild during a run.** Fixtures run `./build/tur` straight out of the
  build tree. A `cmake --build` that lands mid-run replaces the binary, and
  every fixture started while the file is being linked fails with `Permission
  denied`, which `tests/run.sh` reports as `build failed`. `tests/run.sh`
  stamps the binary at the start, re-checks it at the end, and prints a
  `WARNING: ... changed while this run was in progress` (exiting 2) when that
  happened. Other harnesses do not check, so the rule is: **if an assertion
  passes when you run it by hand, the suite probably never really ran it.**
- **Oversubscription.** `tests/run.sh` and `tests/run-turi.sh` already run
  one job per core. Running them in parallel with each other, or under `ctest
  -jN`, multiplies that, and per-fixture timeouts start expiring on work that
  would finish. Run one suite at a time.
- **Interpreter memory.** The interpreter keeps its closures and continuations
  for the life of the process, about 4 KiB per step of a trampolined loop. A
  fixture with a million steps peaks at gigabytes under `--interpret`, and two
  of them at once is memory pressure. When an interpreted fixture times out,
  check its peak memory before raising its timeout.
- **Timeouts are labeled.** A killed fixture reports `timed out (>Ns)`, not a
  stdout mismatch.

### Running the suite usefully

```sh
TUR_TEST_FILTER='^hello$' bash tests/run.sh 2>&1    # one fixture (a regex)
TUR_TEST_SUITE=errors      bash tests/run.sh 2>&1    # happy | errors | snapshots
TUR_FORCE=1                bash tests/run.sh 2>&1    # ignore the stamp cache
```

- `tests/run.sh` writes its `PASS`/`FAIL` lines and summary to stdout. Every
  variable that narrows or changes a run is in
  [running-tests-guide.md](running-tests-guide.md).
- Run every suite with a 12-minute timeout. A clean full run takes about five
  minutes on four cores.
- A passing fixture is skipped on the next run unless something it depends on
  changed. The stamp covers the fixture's files, the `tur` binary, every file
  under `stdlib/`, and the build configuration (`CC`, `TUR_CC_FLAGS` and
  similar). It does not cover a file the fixture `load`s from elsewhere, or a C
  compiler upgraded in place; use `TUR_FORCE=1` after changing either, or
  whenever a green run looks too fast.
- A fixture's `expected.c` is a codegen snapshot. A mismatch there means the
  emitted C moved, not necessarily that it is wrong; regenerate with the loop
  in [CLAUDE.md](https://github.com/turmeric-lang/turmeric/blob/main/CLAUDE.md#fixture-snapshots).

`tests/run-turi.sh` is the interpreter suite and `tests/run-jit.sh` the JIT
suite. The `requires.*` skip markers and the `expected.xfail` convention are
listed in
[CLAUDE.md](https://github.com/turmeric-lang/turmeric/blob/main/CLAUDE.md#requires-skip-markers).

## 7. Fuzzers

The source fuzzers generate whole programs whose correct output is known by
construction, run them through the real pipeline, and classify every
disagreement. They find the combinations nobody wrote a fixture for.

| Fuzzer | Bug family |
|---|---|
| `tests/type-fuzz-src.py` | values crossing a typed boundary (return, `fn` parameter, method result, container slot) |
| `tests/refine-fuzz-src.py` | refinement types and the solver |
| `tests/regions-fuzz-src.py` | region escapes |
| `tests/saffron-fuzz-src.py` | the Saffron dialect |

All take `--n` (cases), `--seed`, `--jobs`, `--tur` and `--save-dir` (keep
the generated programs). A small run is a reasonable smoke test after a change
in the relevant area:

```sh
python3 tests/type-fuzz-src.py --n 50 --seed 1 --tur build/tur --save-dir /tmp/fuzz
```

`tests/replay-fuzz-seeds.sh` replays the seeds recorded in
`tests/fuzz-seed-corpus.txt`, which is how a seed that once found a bug stays
covered.

## 8. Sanitizers and leaks

What is and is not checked surprises people:

- **`tur` itself** is built with `-fsanitize=address,undefined` in a Debug
  build. On Linux that includes LeakSanitizer, so a leak in the compiler fails
  `tests/run.sh`.
- **Programs `tur` emits are not sanitized.** Fixtures compile without
  sanitizers and run with leak detection off, so a leak or use-after-free in
  emitted code passes `run.sh` silently. Use `tests/run-leak-check.sh`, and opt
  a fixture in with a `requires.leak-check` marker file. The
  [test suite portability guide](test-suite-portability-guide.md#7a-leak-checking-what-is-covered-and-what-is-not)
  has the full coverage map and the two traps that have produced wrong answers.
- **Indirect-call type errors** (calling a function pointer through the wrong
  signature) are found by clang's `-fsanitize=function`, run by
  `tests/run-fnsan.sh`. Apple's clang does not support that sanitizer, so on
  macOS point `CC` at a Homebrew LLVM clang; a clean run with Apple clang
  proves nothing.

### macOS: `tur` hangs at startup

If **every** `tur` command hangs, including `tur --version`, the cause may be
the ASan runtime deadlocking before `main` when the Command Line Tools are
older than the OS. Confirm it without `tur`:

```sh
printf 'int main(void){return 0;}\n' > bare.c
cc -fsanitize=address -g -o bare-asan bare.c
perl -e 'alarm 15; exec @ARGV' ./bare-asan     # hangs only when live
```

If that hangs, update the Command Line Tools (`softwareupdate --list`), or build
with Homebrew LLVM. If it does not hang, the problem is something else. The
remedies, and why sanitizers stay on by default, are in
[CLAUDE.md](https://github.com/turmeric-lang/turmeric/blob/main/CLAUDE.md#macos-startup-hang----outdated-asan-runtime).

## 9. Writing it down

A bug you are not fixing right now goes in `docs/reported/<slug>.md`: a
one-line summary with a severity, a minimal repro, the root cause with
`file:line` when you know it, and fix directions. Add a row to
`docs/reported/README.md`; `bash tests/check-reported-index.sh` fails if the
index and the directory disagree. When a report is fixed, move it to
`docs/archive/`. The full convention is in
[CLAUDE.md](https://github.com/turmeric-lang/turmeric/blob/main/CLAUDE.md#reporting-bugs).

Before investigating a red result at all, search `docs/reported/`. A failure
you hit is often already filed, with a repro and a root cause.

## See also

- [Compiler internals](compiler-internals.md) -- the pipeline and the `src/`
  layout, file by file
- [Value representations](value-representations-guide.md) -- how every kind of
  value is laid out in emitted C, and the boundaries where they meet
- [Test suite portability](test-suite-portability-guide.md) -- harness
  portability, sanitizers and leak checking in depth
- [JIT guide](jit-guide.md) -- the in-process engine and its fallback rules
- [Debugging Turmeric Programs](debugging-guide.md) -- lldb, `tur debug`,
  sanitizers for inline C
