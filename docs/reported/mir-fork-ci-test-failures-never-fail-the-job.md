# The MIR fork's CI goes green whatever the C tests say

**Severity: medium (test coverage).** In `turmeric-lang/mir`'s `make test`, a
failing c-test or a failed c2mir bootstrap prints `FAIL` and the job still
passes. Since the fork's first CI run, the macOS arm64 leg has printed **six
bootstrap `FAIL`s and one failing c-test per pass** on every run, all shown as
green. turmeric ships a macOS arm64 JIT built from this code. Filed 2026-10-16
while reworking the fork's CI.

## Repro

Read it off any macOS run of the fork, e.g. the apple-aarch64 job on
`cbf0f524` ([run 38850570612](https://github.com/turmeric-lang/mir/actions/runs/38850570612/job/116810636759)),
conclusion **success**:

```text
./c-tests/new/va-struct-args.c:FAIL (code = 3)          (gen -O1 and -O3 passes)
Tests 1093, Success tests 2184                          (a pass counts 2 per test: one failed)
+++++++ C2MIR Bootstrap lazy func test with default optimize level ... ./1o2.bmir ./2o2.bmir differ: char 880978, line 7679
FAIL
...                                                     (all six bootstrap targets the same)
```

The same output was already there on upstream `a8ab7c31`, the fork's first
run on 2026-07-29 ([run 30430571497](https://github.com/turmeric-lang/mir/actions/runs/30430571497/job/90506613355)):
`Tests 1084, Success tests 2166`, six bootstrap `differ ... FAIL`. So the
fork's patches did not cause this. The ubuntu x86-64 leg is clean
(`Tests 1095, Success tests 2190`, six `Passed`).

The reworked CI's first run (mir#11, `d93d2bd7`,
[run 38852158065](https://github.com/turmeric-lang/mir/actions/runs/38852158065)),
all four legs green:

| Leg | c-tests | Bootstraps |
| --- | --- | --- |
| ubuntu x86-64 | all pass | 6 of 6 pass |
| ubuntu arm64 (first run ever) | all pass except in lazy-bb mode: `c2mir-bb-gen-test` reads `Tests 1098, Success tests 2185`, with four `long double` tests failing (below) | 5 of 6 pass; the lazy-bb one (`-p4 -eb`) differs at byte 652165 |
| macOS 15 arm64 (`macos-latest`) | one fails per pass | 0 of 6; all differ at `char 8, line 1` |

On macos-14 the six differed about 880 KB in; on macOS 15 they differ at
byte 8. The failure moves with the OS image.

## Root cause

- `c-tests/runtests.sh` counts results and prints `Tests N, Success tests M`,
  but has no `exit` anywhere. It returns the status of its last `rm -f`.
- Every `c2mir-bootstrap*` recipe in `GNUmakefile` ends in
  `cmp 1.bmir 2.bmir && echo Passed || echo FAIL`, which exits 0 either way.

So `make test` fails only if a build breaks or one of the three
self-checking programs (`readme-example-test`, `mir-bin-run-test`,
`c2mir-simple-test`) crashes. The Windows leg runs `ctest` (42 tests, none of
them c-tests or bootstraps), and that one does gate.

## What the hidden failures mean

A bootstrap compiles c2mir with the clang-built `c2m` (stage 1), then runs
that output under the JIT to compile the same sources again (stage 2), and
the two `.bmir` files must match. On macOS arm64 they do not. So c2mir built
by c2mir behaves differently from c2mir built by clang -- a miscompile or
nondeterminism that no test points at. It is Apple-specific: Linux arm64
passes the five lazy-func bootstraps and every c-test outside lazy-bb mode.
Unverified lead: Apple arm64 passes variadic arguments differently from
AAPCS64, and the other failure, `va-struct-args.c`, is a variadic test.

Linux arm64's failures are all in lazy-bb mode (`-eb`), and are a separate
signal. Four c-tests fail there, every one of them a `long double` test, in
the job log of run 38852158065:
- `gcc/20020413-1.c`, `gcc/20030914-1.c` and `gcc/regstack-1.c` abort
  (`FAIL (code = 134)`);
- `new/issue186-2.c` prints `FAIL!` (NaN comparisons on float, double and
  long double).

On aarch64 Linux `long double` is a 128-bit quad, so lazy BB generation's
handling of it is the first suspect. The lazy-bb bootstrap mismatch may be
the same cause, since c2mir's own sources use `long double`. It is also one
run of a parallel (`-p4`) test, so it may be nondeterministic; re-run it
before reading anything into it.

That run includes mir#10 (`fcdaec59`, which inlines small struct copies).
That change is not what fails the c-tests:
- three of the four contain no struct or union, so the code mir#10 changed
  never runs for them, and their MIR is the same with or without it;
- the fourth, `20030914-1.c`, does copy structs, but it fails the same way as
  the other three.

There is no pre-mir#10 arm64 run to compare the bootstrap with, so the
bootstrap is not settled. A `workflow_dispatch` run of `test.yml` on a branch
at `456c2e2b` (before mir#10) would settle it.

## Fix directions

1. Make the failures count. Either make `runtests.sh` exit non-zero when
   `ok != 2 * all`, and turn each bootstrap's `|| echo FAIL` into
   `|| { echo FAIL; exit 1; }` (a fork-local diff in upstream files), or leave
   the scripts alone and have `test.yml` tee the log and fail on `^FAIL` /
   `:FAIL`.
2. Either way the macOS leg then goes red on every PR until (3) is done, so
   the order is a choice: gate Linux and Windows first, or carry a known-failure
   list for macOS.
3. Find the macOS arm64 bootstrap mismatch. `cmp -l` the two `.bmir` files,
   map the first differing byte back to the function, and compare that
   function's stage-1 and stage-2 MIR.
