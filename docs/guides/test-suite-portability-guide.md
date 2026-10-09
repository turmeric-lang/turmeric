---
title: Test Suite Portability & Performance Guide
category: Contributor Notes
description: macOS / Bash 3.2 gotchas, background-terminal SIGTTOU guards, inline-C math wrapper shadowing, and stamp-cache patterns for the test harnesses
---

# Test Suite Portability & Performance Guide

The Turmeric test harnesses (`tests/run.sh`, `tests/run-turi.sh`,
`tools/run-doctests.sh`) run on both Linux CI and macOS developer boxes.
macOS ships Bash 3.2 by default, which trips a handful of portability
pitfalls that are easy to reintroduce. This guide records the gotchas that
have burned us, the patterns that fix them, and the stamp-cache design that
keeps parallel fixture runs cheap.

If you are editing anything under `tests/` or `tools/run-doctests.sh`,
skim this before you push.

---

## 1. macOS Bash 3.2 -- no `mapfile`

Bash 3.2 does not have `mapfile` (a.k.a. `readarray`). Any harness that
uses it silently produces empty arrays and turns real diagnostics into
false failures.

Use a portable `while read` loop instead:

```bash
expected_arr=()
while IFS= read -r line || [ -n "$line" ]; do
  expected_arr+=("$line")
done < "$expected_file"
```

The `|| [ -n "$line" ]` clause is what handles a final line without a
trailing newline -- do not drop it.

Rule: **no `mapfile` in any file under `tests/` or `tools/`.** Grep for it
before landing a patch.

---

## 2. Background jobs and `tcsetattr` -- guard against `SIGTTOU`

When a fixture runs under `xargs` (or any background subshell) and its
program touches terminal state -- `tcsetattr`, `stty`, raw mode -- the
kernel sends `SIGTTOU` to the background process group and stops it. From
the harness's point of view the fixture just hangs forever with no output.

Any inline-C that calls `tcsetattr` must first check whether the caller
actually owns the controlling terminal, and no-op otherwise:

```c
#include <unistd.h>
pid_t fg = tcgetpgrp((int)fd);
if (fg == -1 || fg != getpgrp()) {
  /* Backgrounded or not a tty -- skip; avoids SIGTTOU stop. */
  return -1;
}
/* ... tcsetattr(fd, ...) ... */
```

The `stdlib/term.tur` helpers (`term/set-raw`, `term/set-cooked`) already
carry this guard -- in `term/set-mode`, the single inline-C body both of
them delegate to; keep it in place, and copy the pattern into any new
terminal-state code.

The guard is also why terminal-state code is hard to *test*: it makes both
helpers no-op under the harness, which always redirects stdout.
`tests/fixtures/term-raw-cooked-roundtrip` gets past it by building a
terminal rather than borrowing one -- `posix_openpt`, then `fork` +
`setsid` + `ioctl(TIOCSCTTY)` in the child so the pty slave is a
controlling terminal whose foreground process group is the child's. Reuse
that shape for anything else that has to exercise a real tty.

---

## 3. Inline-C name shadowing -- do not call `sqrt` from a `defn sqrt`

A Turmeric `defn` compiles to a `static` C function of the same name.
Inside that function's inline-C body, an unqualified call to `sqrt(x)`
resolves to the local static function -- not `libm` -- and recurses until
the stack overflows. The fixture appears to hang.

The fix is to route math wrappers through the compiler's `__builtin_*`
intrinsics, which resolve to the target math op directly and cannot be
shadowed by a local static:

```turmeric
(defn sqrt [x : float] : float
  ```c
  return __builtin_sqrt(x);
  ```)

(defn fabs [x : float] : float
  ```c
  return __builtin_fabs(x);
  ```)
```

Both GCC and Clang support `__builtin_sqrt`, `__builtin_fabs`,
`__builtin_floor`, `__builtin_ceil`, `__builtin_pow`, etc. This is the
canonical pattern for any Turmeric wrapper whose name collides with a
libm symbol.

If you must call libm by its real name, rename the Turmeric-side wrapper
(e.g. `float/sqrt`) so the C symbol does not shadow libm.

---

## 4. Stamp cache -- cache `$TUR` mtime **once**, export to workers

`tests/run.sh` and `tests/run-turi.sh` compute a stamp key per fixture to
skip already-built outputs. The stamp key mixes the fixture hash with the
compiler binary's mtime, so a recompile of `tur` invalidates every cached
fixture -- correct, but naive implementations `stat` the binary once per
fixture (~1500 spawns) and once per parallel worker.

Cache the mtime in the parent shell and export it so `xargs`-spawned
workers inherit it:

```bash
_tur_mtime() {
  stat -f '%m' "$1" 2>/dev/null || stat -c '%Y' "$1" 2>/dev/null || echo "0"
}

export TUR_MTIME="$(_tur_mtime "$TUR")"

stamp_key() {
  local input="$1"
  local dir; dir="$(dirname "$input")"
  local ec_hash=""
  [ -f "$dir/expected.c" ] && ec_hash="$(_tur_hash_file "$dir/expected.c")"
  echo "$(_tur_hash_file "$input")-${ec_hash}-${TUR_MTIME}"
}
```

Two things to preserve when editing:

1. **Dual-flavour `stat`** -- BSD (`-f '%m'`) first, GNU (`-c '%Y'`)
   second, `0` fallback last. Do not collapse to one flavour.
2. **`export` the cached value.** Without `export`, `xargs` workers do not
   inherit `TUR_MTIME` and silently re-stat per fixture.

The same pattern applies to `tools/run-doctests.sh`; the doctest runner
does not need the fixture's `expected.c` hash but must still cache
`TUR_MTIME` once.

---

## 5. Parallel `ctest`

Root-level `ctest` invocations must pass `-j` so the ~108 registered
targets run across cores rather than sequentially. The Justfile recipe
looks like:

```justfile
test: build doctest
    timeout 720 ctest -j "$(getconf _NPROCESSORS_ONLN)" --output-on-failure --progress --test-dir build
```

**The job count must be explicit.** A bare `ctest -j` looks like it asks for
"parallel, pick a number" -- it does not. Through CMake 3.28 the option is
documented as `-j <jobs>`; a value-less `--parallel` only arrived in 3.29.
On 3.28 a bare `-j` is accepted **in silence and does nothing**: measured on a
4-core box, five targets took 11.2s serial, 11.5s with bare `-j`, and 5.8s
with an explicit count. So a bare `-j` is the worst of both worlds -- it reads
in review as the parallel recipe while behaving exactly like the serial one,
which is how this section's own snippet came to document a no-op. Use
`getconf _NPROCESSORS_ONLN`: `nproc` is GNU coreutils only, and macOS has
neither it nor a value-less `-j` on the CMake it ships.

The cap is 12 minutes, not 5: `tests/run.sh` alone is ~265s on a 4-core box
and is `RUN_SERIAL`, so a 300s cap kills the whole suite on any machine
slower than the one it was tuned on -- and the kill looks like a hang, not a
timeout. 12 minutes is the repo-wide suite timeout; see CLAUDE.md.

Under parallel `ctest`, end-to-end wall time is bounded by the slowest
single target (usually `tests/run.sh` cold), not the sum. Removing the
`-j` and shipping is a soft regression that will not show up in any test's
own timing -- watch for it in code review.

`-j` is safe here only because the heavy targets are marked `RUN_SERIAL`
(`tur_tests`, `turi_fixture_tests`, `tur_jit_fixture_tests`): each already
fans out across `nproc` internally, so letting ctest run two of them at once
oversubscribes the box and expires their per-fixture timeouts. Keep the
marking when you add a fan-out harness -- see "Failures That Are Not Product
Bugs" in [test-runner-contract.md](test-runner-contract.md).

Note the marking is per-target, not per-family: only the harnesses that
themselves fan out are `RUN_SERIAL`. Among the REPL-spice targets, for
instance, `reload`/`watch`/`jit` are and `call`/`load`/`linklibs` are not --
those three drive a single REPL rather than `nproc` workers, so they are fine
in parallel, but do not assume a whole prefix shares the marking.

### `test` depends on `doctest`, and a doctest failure means the suite never runs

The recipe is `test: build doctest`, so a non-zero exit from
`tools/run-doctests.sh` stops the chain **before** the `ctest` line. The
failure mode is quiet in the worst way: the recipe exits 1 in roughly 24
seconds, which reads like a fast suite rather than no suite at all. If
`tur run test` comes back in well under a minute, it did not reach ctest --
check the doctest output above it. The runner now says so explicitly on
failure, and points at a direct `ctest` invocation to use meanwhile.

**The runner must keep stdout and stderr separate.** Expected/actual pairing
is positional -- the Nth line of program stdout is compared against the Nth
line of `<module>.expected` -- so a single diagnostic line on stderr shifts
every later case by one and turns one real problem into a module-wide
cascade. Capturing with `2>&1` did exactly that: an `unsafe block has 29
expressions` lint desynchronised all 34 `rational` cases and a gcc
`-Wreturn-type` warning did the same to `panic`, producing failures like
``got "1 | ; AUTO-GENERATED by tools/doctest.py"`` -- the runner comparing a
diagnostic's own source echo against an expected value. 40 of 51 failures
were that one bug wearing different masks.

**An example whose output is environment-dependent gets an explicit
opt-out**, a trailing `; doctest: <reason>` on the `; =>` line:

```turmeric
;;;   (term/bold "hello")  ; => "\x1b[1mhello\x1b[0m"  ; doctest: stdout must be a tty (plain copy otherwise)
```

Such a case is skipped and *reported* by `tools/doctest.py`. Without the
annotation it is skipped anyway -- `TESTABLE_RE` is anchored, so any trailing
text stops it matching -- but silently and by accident, which is how three
non-deterministic `random` examples came to be "passing" while a fourth,
`rand-bool`, had a bare `; => 1` and flaked on every other run.

---

## 6. A check that enumerated nothing passes vacuously

`head -z` / `--zero-terminated` is a GNU coreutils extension; BSD/macOS
`head` does not have it. In `tests/run-fmt.sh` it errored, the
NUL-separated `read` loop never ran, the failure counter stayed at zero,
and `fmt-idempotence-stdlib` reported PASS having checked **zero files**.
Formatter idempotence had no macOS coverage at all and the summary line
said everything was fine.

The general shape: **any check whose `pass` is guarded only by "no
failures accumulated" is greenest when its enumeration breaks.** Every
file-walking check needs an explicit "checked at least one file" guard:

```bash
SEEN=0
while IFS= read -r -d '' f; do
    SEEN=$((SEEN + 1))
    ...
done < <(find stdlib -name '*.tur' -print0)

if [ "$SEEN" -eq 0 ]; then
    fail "$NAME" "no files checked -- stdlib enumeration produced nothing"
elif [ "$FAILED" -eq 0 ]; then
    pass "$NAME"
fi
```

Bound a sample inside the loop (`[ "$SEEN" -ge 20 ] && break`) rather than
with a `head` in the pipeline. `tests/run-fmt.sh` carries both guards
(`fmt-bootstrap-stdlib`, `fmt-idempotence-stdlib`) -- copy them.

---

## 6a. Which browser suites gate, and reading the one that does not

The browser tests are not their own checks. They are **steps inside one job**,
`Try Turmeric smoke test (browser)`, and they do not all gate:

| Step | Gates? |
| --- | --- |
| `Run deploy-gate smoke test` (`deploy-gate.spec.js`) | **yes** |
| `Run broader smoke suite (desktop) -- BLOCKING` | **yes**, since 2026-10-02 |
| `Run mobile smoke suite (non-blocking)` | no -- `continue-on-error: true` |

So **mobile** can exit 1 while the job reports success, and `gh run view`
renders that step with a green check. Nothing in `gh pr checks` distinguishes
"mobile ran clean" from "mobile ran red, ignored".

Mobile is not non-blocking because its tests are worse -- measured over 266
commits its isolated-flake rate is lower than desktop's. It is non-blocking
because of its **precondition**: the `Install Playwright WebKit` step is itself
`continue-on-error`, and when that download fails the whole project dies at
`browserType.launch: Executable doesn't exist` before any test body runs.
Gating there would convert a third-party download failure into a blocked PR,
which is infrastructure flake rather than a test signal.

Three places carry mobile's real answer, cheapest first:

1. **The run page's annotations.** The `Report browser suite outcomes` step
   writes a pass/fail line per suite to the job summary. A failed mobile suite
   raises a `::warning::`; desktop raises an `::error::`, though a red job is
   the louder signal there anyway.
2. **The `playwright-report` artifact**, uploaded `if: ${{ !cancelled() }}`
   rather than `if: failure()`. That condition predates desktop gating and is
   still the right one: a `failure()` upload would now catch a desktop failure
   but would still miss mobile behind `continue-on-error`. It carries the
   per-test `error-context.md` files.
3. **`web_desktop` / `web_mobile` on [/ci](https://turmeric-lang.com/ci)**, which
   is the trend rather than the single run.
   `tools/ci/collect-playwright-timings.py` publishes `status: "fail"` when any
   test failed even though the job stayed green, and `status: "skip"` when a
   suite produced no JUnit at all (the Playwright-install failure mode), so a
   suite that never ran reaches the skip ledger instead of vanishing.

**A standing failure teaches readers to ignore the annotation**, and that is
what made gating impossible for as long as it was. When the desktop suite sat
at `1 failed / 141 passed` on every run, a real new regression would have read
as `2 failed` -- indistinguishable at a glance. Known-broken browser tests
therefore get marked `test.fixme` with a pointer to their report (see
`web/tests/docs-offline.spec.js`), so the count stays meaningful. Note
`test.fixme` does not self-close the way a fixture's `expected.xfail` does: a
marked test that starts passing stays silently skipped, so deleting the marker
is a manual obligation carried by the report.

**The raw fail rate is not the flakiness rate**, and conflating them is what
keeps a suite non-blocking longer than it needs to be. `web_desktop` had failed
on 42 of 266 commits (16%) when it was promoted -- a number that reads as far
too flaky to gate on. But collapsing *consecutive* failures gives only 10
episodes, two of them 21 and 7 commits long: standing bugs, which fail every
run until fixed and then stop. Isolated single-commit failures, the actual
flake signal, were 6 of 266 (2.3%). Before arguing a suite is too flaky to
gate, separate the two -- the query is one pass over
`suite-timings-<year>.jsonl` on the `ci-metrics` branch. Residual flake at that
level is then damped by `retries` in `web/playwright.config.js` (2 on CI, 0
locally so a freshly written flaky test fails in front of its author).

For the equivalent trap on the `ctest` side -- suites that `bash tests/run.sh`
does not run at all -- see section 7d and the See-also list.

---

## 7. Heap probes and sanitizers do not mix

A malloc-probe assertion means nothing under ASan, and it means nothing
*differently* per platform:

- **glibc** -- ASan replaces the allocator, so `mallinfo2().uordblks`
  reads 0 and the check is vacuously green.
- **Darwin** -- the probe reads ASan's own zone and the free quarantine
  inflates it (measured: 160 bytes per iteration of quarantined frees).

The two platforms disagree, which is worse than both being wrong: the
glibc leg looks like a working control for the Darwin leg.

Separately, `malloc_zone_statistics(malloc_default_zone(), ...)` on Darwin
measures the **whole default zone**, including stdlib bucket/page
bookkeeping, and moves in 16-32 KB steps. Absolute heap-delta assertions
are noise there; glibc's `mallinfo2().uordblks` is the narrow equivalent.

Two rules fall out:

1. The diagnostic that settles "leak or noise" is **scaling**: a real leak
   grows with iteration count, noise does not. Probe at two sizes before
   believing a number.
2. `mallinfo2` is useless anywhere in this tree that links an
   ASan-instrumented `libturi`. Use RSS from `/proc/self/statm`, which is
   allocator-independent.

---

## 7c. ASan inflates the stack ~40x -- deep nesting dies before it computes

The Debug build's `-fsanitize=address` costs stack as well as heap, and by a
much larger factor than people expect. Measured on the emitter's expression
walk: **~170 KB of stack per recursion level under ASan, versus ~4 KB
without** -- so the sanitized build reaches a `stack-overflow` at roughly 1/40th
the depth of an unsanitized one.

Concretely, before the depth bound went in, `tur emit-c` on a nested
expression died at:

| Build | Stack limit | Crashes at |
| --- | --- | --- |
| Debug + ASan (**the documented bootstrap build**) | 8 MB (default) | 47 (macOS/clang), 60-80 (Linux/gcc) |
| Debug + ASan | 32 MB | 150-200 |
| `-DTUR_DEBUG_SANITIZE=OFF` | 8 MB (default) | 2000-5000 |

Three things follow, and the first two are the traps:

1. **A depth-related crash on a Debug build is not evidence of a runaway
   recursion.** It may be an entirely bounded walk that an unsanitized build
   completes without noticing. Re-run with `-DTUR_DEBUG_SANITIZE=OFF` before
   concluding anything about termination.
2. **Raising the stack limit only buys a linear factor.** `ulimit -s
   unlimited` (which `turmeric-spices` CI uses) moves the cliff; it does not
   remove it, and it does nothing for a developer running the documented
   bootstrap build locally.
3. **Do not bound a recursive compiler walk with a depth constant.** It was
   tried, and it failed in both directions at once. `EMIT_MAX_EXPR_DEPTH` was
   40, sized under the measured cliffs in the table above -- then the
   `emit_value` frame grew a 256-byte region-walk array, the macOS cliff moved
   *below* 40, and the guard lost the race it existed to win: an ASan
   stack-overflow abort where TUR-E0712 should have printed. In the other
   direction, 40 was checked against the deepest **hand-written** nesting in
   the tree (20) -- but macro expansion is not hand-written, and a
   12-component `for-each` in the `ecs` spice expanded past it and could not be
   compiled at all.

   Depth follows the input, so the compiler sizes the stack for the job instead
   of rationing it. `tur` trampolines its whole driver onto a stack sized by
   `TUR_STACK_MB` (default 256 MiB, `tur_run_on_big_stack` in
   `src/compiler/stack_guard.h`), exactly as `jit_engine.c` has always done for
   a JIT'd program's entry stack via `TUR_JIT_STACK_MB`, and as rustc does with
   `RUST_MIN_STACK`. There is no depth cap any more.

   What remains at each recursive walk -- `emit_value`, `elab_call`, and macro
   expansion -- is a **backstop**, not a bound: `tur_stack_nearly_exhausted()`
   measures the calling thread's real remaining stack, so reaching it means a
   walk that is genuinely unbounded rather than merely deep. Measuring the
   actual resource is also the only bound that cannot rot the way the constant
   did -- it is correct at any frame size, on any host, under any sanitizer.

   Note the numbers in the table above are now *historical*. They describe
   where an 8 MiB stack ran out, which is no longer the stack the compiler
   runs on.

   Three things to know before adding a fourth such walk. Under ASan **the
   address of a local does not approximate the stack pointer** (address-taken
   locals live on the sanitizer's fake stack), so a hand-rolled probe silently
   degrades to no probe at all; use the shared helper, which reads the SP
   register. `TUR_DEBUG_STACK_GUARD=1` traces what the guards see. And an
   unguarded walk fails *worse* than a guarded one: `elab_call -> elab_form`
   had no guard, so deep nesting aborted the compiler with no diagnostic
   whatsoever until one was added. See TUR-E0712,
   [docs/archive/emit-value-dispatch-unbounded-recursion.md](https://github.com/turmeric-lang/turmeric/blob/main/docs/archive/emit-value-dispatch-unbounded-recursion.md)
   and
   [docs/archive/emit-depth-guard-loses-race-with-asan-stack.md](https://github.com/turmeric-lang/turmeric/blob/main/docs/archive/emit-depth-guard-loses-race-with-asan-stack.md).

A regression fixture cannot usefully assert the backstop: on the real stack the
nesting needed to trip it is impractically large, and a fixture tuned to sit
just under a cliff is precisely the stack-size canary that
`errors/expr-nesting-depth-limit` became before it was retired. Shrink the
stack instead and keep the program small -- that is what
`tests/run-compiler-stack-guard.sh` (ctest `tur_compiler_stack_guard`) does
with `TUR_STACK_MB`, asserting both directions: the same source compiles on a
normal stack and is refused, with a diagnostic rather than an abort, on a tiny
one. The positive half lives in the ordinary suite as
`tests/fixtures/expr-nesting-deep`, whose 120 levels are 3x the retired cap and
entirely unremarkable now.

---

## 7a. Leak checking -- what is covered and what is not

The single most misread thing about this suite. `CLAUDE.md` says
`bash tests/run.sh` runs "with leak detection ON", and that is true -- **of the
compiler process**. It is not true of the programs the compiler produces, and
the complement is what catches people out:

| what runs | sanitized? | leak-checked? |
|---|---|---|
| `tur` itself (`build`, `emit-c`, `check`) in a Debug build | yes, ASan+UBSan | **yes** for LEAKS -- but see 7b for UBSan, which is a different story |
| the fixture PROGRAM `tur` produced | **no** | **no** |
| turi/eval interpreter harnesses (`run-turi.sh`, `run-flags.sh`) | yes | no -- `detect_leaks=0` by design (process-lifetime closures) |

Emitted programs are built at `tests/run.sh:169` with
`-O2 -std=c99 -Wall -fno-strict-aliasing` -- no `-fsanitize` -- link the lean
non-ASan runtime by default, and run with `detect_leaks=0` regardless. A fixture
built the normal way has **zero `__asan_init` symbols**. A program that leaks a
megabyte passes the suite silently, because the suite only compares printed
output.

So `requires.no-leak-check` is a no-op in the default configuration. It only
means anything in a build where fixtures do link an instrumented runtime.

## 7b. UBSan findings in the compiler are collected, not fatal

The row above says the compiler is ASan+UBSan instrumented, and it is -- but
those two behave differently, and the difference hid a bug for as long as it
existed.

ASan aborts. **UBSan does not**: the Debug build uses
`-fsanitize=address,undefined` *without* `-fno-sanitize-recover`, so a UBSan
finding prints one line to stderr and execution continues. This suite compares
stdout. So a UBSan line was, until 2026-08-25, completely invisible --
`fat_captures_borrowed` was read out of uninitialized arena memory on 60
fixtures, on every single run, and nothing ever failed
([history](https://github.com/turmeric-lang/turmeric/blob/main/docs/archive/history/fat-captures-borrowed-read-uninitialized.md)).

`tests/run.sh` now scans each phase's captured stderr for `: runtime error:`
and reports what it finds after the summary:

```
SANITIZER: 63 finding(s) from `tur` across 59 fixture(s).
  These are UBSan/ASan diagnostics from the COMPILER, not from emitted programs.
  They do not fail the run (set TUR_SANITIZER_GATE=1 to make them fatal).
         59 build   .../emit_fns.c:3131:66: runtime error: load of value N, ...
          4 emit-c  .../emit_fns.c:3131:66: runtime error: load of value N, ...
```

**Findings are reported but do not fail the run by default.** That is deliberate
rather than timid: arming it on discovery would have turned 60 silent findings
into 60 red fixtures in one step, which is how a gate gets switched off instead
of fixed. `TUR_SANITIZER_GATE=1` makes any finding fail the run.

**CI arms it on both legs.** `.github/workflows/ci.yml`'s "Run fixture suite"
step -- the only step that runs `tur_tests` -- sets `TUR_SANITIZER_GATE: "1"`
unconditionally. Linux and macOS both measure zero findings, so a finding in CI
is a regression rather than backlog. Run the suite armed locally before pushing
anything that touches the compiler's C if you would rather find out first.

One asymmetry to keep in mind: the Linux zero was measured on the CI toolchain,
while the macOS zero was measured on a local Apple-silicon box rather than the
`macos-latest` runner image. Same libubsan lineage and architecture, so a first
macOS finding is more likely a genuine version-specific one than a broken gate.

**On a finding, read the log file, not the console.** The summary prints only
the ten most common findings; the full per-fixture attribution is copied to
`$(dirname "$TUR")/sanitizer-findings.log` (override with
`TUR_SANITIZER_LOG_OUT`), because the harness's own results directory is a
`mktemp -d` that the `EXIT` trap deletes. CI uploads that file as the
`sanitizer-findings-<os>` artifact.

Two things to know if you touch this:

- **The scan runs inside the xargs workers**, which are separate `bash -c`
  processes. `note_sanitizer` must stay in the `export -f` list. It was left out
  of the first version, and the result was a gate that reported a clean tree
  while the bug it was written for was present -- a mechanism that exercises
  nothing looks exactly like a mechanism that found nothing.
- **Verify it by putting a finding back**, not by watching it stay quiet. The
  check that matters is deleting the three `fat_captures_borrowed = false`
  initializers, rebuilding, and confirming the count goes from 0 to ~63.

### If you are chasing a leak in emitted code

Do not reach for `bash tests/run.sh` -- it cannot see one. Four harnesses can
(and a fifth, below, sees memory errors but not leaks):

- **`tests/run-leak-check.sh`** -- the general one, and the one ctest runs
  (`ctest -R tur_leak_check`, `RUN_SERIAL` because it fans out across `nproc`
  itself). Drop a `requires.leak-check` marker in any fixture directory and it
  is rebuilt with ASan and run with `detect_leaks=1`, asserting both its
  `expected.stdout` and no leak. Start here. 54 fixtures currently opt in --
  the `rc-*`, `affine-*`, `weak-*` and `defer-*` families, where reclamation is
  the point of the test.
- `tests/run-gc-leak-gate.sh` -- cycle-collector fixtures, ASan, with a
  collector-off control run.
- `tests/run-r7rs-sanitize.sh` (ctest `tur_r7rs_sanitize`) -- every
  `#lang r7rs` fixture compiled with ASan and UBSan (UB fatal) and run with
  leak detection OFF: it gates memory errors, not leaks, because a Scheme
  program's data is never freed by design
  (`docs/reported/r7rs-heap-data-never-reclaimed.md`).
- `tests/run-closure-env-leak.sh`, `tests/run-fat-shim-leak.sh` -- one
  regression each; they emit C and compile it by hand with ASan.
- `tests/run-leak-gate.sh` -- despite the name, this is the COMPILER's error
  path, not emitted code.

A fixture with a real but not-yet-fixed leak carries a `known-leak` file naming
its open report: `run-leak-check.sh` then reports it as `KNOWN` rather than
failing, and fails if it ever runs *clean*, so the marker cannot outlive the
bug.

### Four traps, each of which has produced wrong answers here

1. **Confirm the instrument before believing a clean result.** ASan silently
   reports nothing when it is not linked -- which is the default for fixtures.
   Plant a deliberate `malloc(1234)` that is never freed and check the tool
   reports it before concluding that anything is leak-free.
2. **LSan reports what is UNREACHABLE, not what is dead.** Anything held in a
   global registry is reachable by construction and will be called live however
   dead it is; section 7's rc-block case is exactly this. A clean run means
   "nothing was orphaned", not "nothing was retained". For retention, count it.
3. **A live LOCAL hides a leak just as well as a global does, so a small
   program is the worst place to test one.** This is the same rule as trap 2
   but it bites differently: in a `main` that ends right after the allocation,
   the binding still holds the pointer at exit, LSan calls the block reachable,
   and the run is CLEAN. Add one trailing statement and the same program
   reports the leak -- with byte-identical emitted C for the leaking block.
   `ref/from-rc` in
   [rc-ref-conversion-and-weak-upgrade-leak](https://github.com/turmeric-lang/turmeric/blob/main/docs/archive/rc-ref-conversion-and-weak-upgrade-leak.md)
   behaves exactly this way. When probing a suspected leak, always put work
   after it.
4. **The leak harness links no host runtime, and the default build does.**
   `run-leak-check.sh` forces `TUR_RUNTIME=source` (every runtime function
   compiled into the fixture's own TU), while the default Linux cc path and
   `tur jit` are HOSTED builds: the program half is compiled against the
   split runtime (`src/runtime/generated/`, `libturi`) and the two halves
   share per-thread state only through the host accessors in
   `src/runtime/tur_tls.c`. A thread-local the generator's table
   (`tools/gen-runtime-split.py`, `TLS`) does not list gets two slots -- the
   program writes one, the runtime reads the other -- and a drop keyed on it
   never happens. `tur_async_owns_env` was such a slot for a day: every
   leak-checked async fixture ran clean while the default build dropped no
   async env at all
   ([async-capturing-body-env-never-freed](../reported/async-capturing-body-env-never-freed.md),
   the fifth narrowing). When the leak you are chasing is in code that
   crosses the runtime boundary (a spawn, a handler, a fiber), run the
   harness once more as `TUR_RUNTIME=split bash tests/run-leak-check.sh`, or
   check the suspect fixture under `tur jit` with `ASAN_OPTIONS=detect_leaks=1`,
   and treat a thread-local added to the preamble as unfinished until it is
   in that table.

---

## 7d. What `run-turi.sh` does not run -- and how it says so

`tests/run-turi.sh` (ctest `turi_fixture_tests`) walks the same
`tests/fixtures/` tree as `run.sh` but interprets instead of compiling, and it
deliberately does not run about a quarter of what it discovers. Two kinds of
skip, both counted:

- **The inline-C carve-out.** A fixture whose program contains a user ` ```c `
  block (in its own file or one `(load ...)` deep) cannot run under the
  tree-walking interpreter (TI7). It is detected, printed as
  `SKIP <name> (inline-c carve-out)`, and counted. The small allowlist
  `TURI_INLINEC_RUN` names the inline-C fixtures that *do* interpret (native
  overrides / the simple inline-C evaluator).
- **Marker skips.** `requires.compiled`, `requires.tur-only`,
  `requires.dedicated-runner`, `requires.spices` (when `../turmeric-spices` is
  absent) and `requires.tsan` (when `TUR_TSAN` is not `1`) -- the same set
  `run.sh` honours -- applied by one helper (`marker_skip`) to **both** the
  positive pass and the `errors/` diag pass.

The `errors/` pass asserts `expected.diag`, or `run.sh`'s other substring
file `expected.stderr`; an `errors/` fixture with neither is a **FAIL**
("asserts nothing"), the negative-pass equivalent of the loose-`.tur`-files
rule in section 6.

Every discovered fixture lands in exactly one bucket, and the summary says so:

```
turi fixture summary: 1898 passed, 0 failed, 843 skipped of 2741 discovered
  (of which 743 inline-c carve-outs -- TI7, never run under turi)
TUR_SKIP_PARTIAL: inline-c carve-out (743 fixtures)
  (and 100 requires.* marker skips)
```

`passed + failed + skipped` must equal `discovered`, or the run fails with
`FAIL run-turi accounting: ...` -- a fixture that printed a line and fell
into no bucket is the failure mode this exists to catch (it happened to 99
fixtures at once; see
`docs/archive/turi-suite-accounting-and-reporting-gaps.md`). The
`TUR_SKIP_PARTIAL:` line is what `tools/ci/collect-suite-timings.py` reads,
and it now also parses the census counts into the suite's row
(`passed` / `failed` / `skipped` / `discovered`), so a suite that silently
starts skipping shows up as a trend. A `TURI_ERRORS_DENY` entry that names no
fixture is a startup error, not a no-op. The seven `tests/turi/eval-async-*.sh`
scripts are their own ctest targets and are no longer folded into this
suite's counts.

## 8. Harness environment parity

`tests/run.sh` exports `TUR_BIND_LOOPBACK=1`; `stdlib/httpd.tur` and
`stdlib/async_socket.tur` read it at run time and bind `INADDR_LOOPBACK`.
`stdlib/async_socket.tur` otherwise binds `INADDR_ANY`, and `stdlib/httpd.tur`
binds loopback by default but `INADDR_ANY` once a program calls
`httpd-set-bind-any!` -- so a sibling harness that forgets the export still
makes server fixtures bind all interfaces.

That is not a cosmetic difference. It surfaced as an apparent macOS-only
JIT defect: BSD permits a wildcard bind while a specific address holds the
port, so the second server silently succeeded and stole nothing on Linux
(which refuses the wildcard-over-specific bind) but did on macOS. The
mechanism was BSD socket semantics; the cause was one missing `export` in
`tests/run-jit.sh`.

Rule: **any env var `tests/run.sh` exports, every sibling harness must
export.** Diff them before landing a new runner:

```sh
grep -n TUR_BIND_LOOPBACK tests/run.sh tests/run-jit.sh
```

### `TUR_TEST_SHARD` -- one spelling, two harnesses

`tests/run.sh` and `tests/run-jit.sh` both honour `TUR_TEST_SHARD="i/N"`:
run only the i-th of N disjoint slices, so N runners can share one corpus.
The two parsers are deliberately identical, down to the clamping of a
nonsense index (`0/3` and `abc/3` both mean `1/3`; `4/3` means `3/3`;
`1/0`, `1/1` and a value with no slash all mean "not sharded"). CI sets the
variable once per job and both harnesses may read it, so a second dialect
would silently partition the two corpora differently.

Three properties the partition is required to have, and which
`tests/run-shard-partition.sh` asserts against the live corpus:

- **Disjoint and complete.** The union of `1/N`..`N/N` is exactly the
  unsharded run, with no fixture in two shards.
- **Stable under filtering.** Ordinals advance over the FULL corpus, not
  over admitted fixtures only, so `TUR_TEST_FILTER` never shifts shard
  membership -- which is what lets you re-run one shard's failure by name.
- **Balanced per class.** Happy and error fixtures round-robin on their own
  ordinal counters, so each shard holds within one fixture of an equal share
  of *both* -- which is what makes the shards equal-cost. A contiguous slice
  would be just as disjoint and just as complete while costing twice as much
  on one runner as another.

**A shard is not a full run, and anything that ratchets must know it.**
`run-jit.sh`'s cc-fallback baseline is the live example. Its NEW-fallback
half works per shard and still holds across the job -- every name is in
exactly one shard, so the shards' union checks each name exactly once. Its
*reclaimed* half cannot: a shard sees 1/N of the corpus, so N-1/N of the
baseline is simply unexercised and would be reported as reclaimed by the
engine. That half is suppressed under a shard exactly as it already was
under a filter. Regenerating the baseline from a shard is refused outright
(exit 2) rather than warned about, because the file is rewritten whole: a
shard would delete every name it did not run, and nothing about the result
would look wrong.

---

## 9. String literals are not reliably merged

C11 6.4.5p7 leaves it **unspecified** whether identical string literals in
one translation unit share an address. gcc and clang merge them; c2mir
(the JIT backend) does not.

A fixture that probes a map with the *same* literal it inserted therefore
tests pointer identity on one engine and content equality on another:

```c
/* gcc: MERGED (a == b)          c2mir: DISTINCT (a != b) */
```

So a JIT-only failure on a `cstr`-keyed container can be a **fixture**
defect, not an engine defect. Build the probe key separately from the
insert key -- or compare by content -- before filing anything against the
backend.

---

## 10. Timeout budget

`bash tests/run.sh` is expected to complete in ~4-5 minutes end-to-end and
**must** always be invoked with a 12-minute (`timeout: 720000`) budget --
see the top-level `CLAUDE.md` "Test Suite Timeout" rule. If a run stretches
to 15-20 minutes, suspect CPU contention (overlapping suite runs), not a
hang: per-fixture *run* timeouts already cap at 10s, so a genuine runtime
loop surfaces as `FAIL`, not an indefinite stall.

### Where the per-case bounds are, harness by harness

`timeout` is preferred, `gtimeout` is the fallback, and with neither present
every harness below runs untimed -- stock macOS ships neither name, so CI
installs `coreutils`. Nothing here bounds an **untimed phase around** a
fixture (harness setup, a compile or link the harness does not wrap); that is
where a stall long enough to reach a CI job's wall clock is likeliest to
live.

| Harness | Per-case bound |
| --- | --- |
| `run.sh` | 10s per fixture *run*, `expected.timeout` overrides |
| `run-turi.sh` | 15s per fixture, interpreted |
| `run-jit.sh` | `_run_timed` around each `tur ... jit`, incl. the `errors/` pass |
| `run-flags.sh` | `TUR_CASE_TIMEOUT` (180s) per `$TUR` invocation, via a generated wrapper |

`run-flags.sh` is the odd one: its cases are inline shell, not fixture
directories, so instead of a `_run_timed` at each of ~100 call sites it points
`$TUR` at a small wrapper that runs the real binary under `timeout -k 5`. That
bounds cases added later too. It had **no** bound at all until 2026-09-28,
which is how a stall in its `jit-ffi-*` cases -- dynamic FFI, callbacks,
threads -- could reach the CI job's `timeout-minutes`
([docs/archive/macos-jit-hang-loses-both-diagnostics.md](https://github.com/turmeric-lang/turmeric/blob/main/docs/archive/macos-jit-hang-loses-both-diagnostics.md)).

The CI legs add two outer bounds of their own, because a `timeout-minutes`
kill **cancels the job** and leaves every remaining `if: always()` step
`pending` -- so it destroys the log artifact it was supposed to preserve. The
JIT leg alarms the whole `ctest` call at 2100s, and its three targets carry
ctest `TIMEOUT` properties so a single hang is killed and *named* first.

---

## See also

- [running-tests-guide.md](running-tests-guide.md) -- running the tests:
  narrowing a run, every harness variable, the fixture markers, and the local
  command for each CI job.
- [test-runner-contract.md](test-runner-contract.md) -- stdlib test
  framework contract (assertions, discovery, exit semantics), plus
  "Failures that are not product bugs" (sanitizer-laundered crashes,
  overlapping runs, contract fixtures under Release).
- [performance-guide.md](performance-guide.md) -- user-facing performance
  guidance for Turmeric programs (not the test harness itself).
