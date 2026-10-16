# The MIR fork's CI goes green whatever the C tests say

**RESOLVED 2026-10-16** by turmeric-lang/mir#12 (merged as `fa8a5d69`) and
re-synced into `external/mir/` the same day -- see *Resolution* at the end.
The fork's CI now fails on every failing c-test and bootstrap. The hidden
failures had three causes: a harness bug (the macOS bootstraps) and two real
miscompiles, one of which reached turmeric's linux-aarch64 JIT.

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
| ubuntu arm64 (first run ever) | all pass except in lazy-bb mode: `c2mir-bb-gen-test` reads `Tests 1098, Success tests 2185`, with seven 128-bit (`long double`, `__int128`) tests failing (below) | 5 of 6 pass; the lazy-bb one (`-p4 -eb`) differs at byte 652165 |
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
signal. Seven c-tests fail there, every one of them a 128-bit value, in the
job log of run 38852158065:

| test | result | what it uses |
| --- | --- | --- |
| `gcc/20020413-1.c` | aborts (`FAIL (code = 134)`) | `long double` |
| `gcc/20030914-1.c` | aborts | `long double` |
| `gcc/regstack-1.c` | aborts | `long double` |
| `lacc/long-double-compare.c` | `code = 141, expected code = 142` | `long double` |
| `new/issue186-2.c` | output mismatch (`FAIL!` for `OK!`) | NaN comparisons, `long double` among them |
| `new/int128-misc.c` | output mismatch | `__int128` (mir#7) |
| `new/int128-ops.c` | output mismatch | `__int128` (mir#7) |

On aarch64 Linux `long double` is a 128-bit quad, and `__int128` travels as
two 64-bit halves. So lazy BB generation's handling of 128-bit values is the
first suspect. The lazy-bb bootstrap mismatch may be the same cause, since
c2mir's own sources use `long double`.

**mir#10 is not the cause of any of this.** Run 38852158065 includes mir#10
(`fcdaec59`, which inlines small struct copies). A baseline without it gives
the same result: [run 38861952644](https://github.com/turmeric-lang/mir/actions/runs/38861952644),
a `workflow_dispatch` of `test.yml` on `1302114e`, which is `456c2e2b` (master
before mir#10) with only the workflows directory taken from mir#11. On its
arm64 leg:
- the same seven c-tests fail in lazy-bb mode, the same way
  (`Tests 1097, Success tests 2183`, the same 11 results short, one test fewer
  because `block-move-small.c` is not there yet);
- the lazy-bb bootstrap differs too (byte 644759, line 6105; the offset moves
  because `c2mir.c` is a different size).

The lazy-bb bootstrap failed in both runs, so it is not a one-off flake of
the `-p4` test. The scratch branch, `claude/ci-baseline-pre-mir10`, is not for
merging.

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

## Resolution

turmeric-lang/mir#12, merged as `fa8a5d69`:

- **The gate (fork only, not vendored).** `runtests.sh` records each failing
  test and exits 1; the bootstrap recipes exit 1 on a mismatch and keep the
  `.bmir` files; `test.yml` runs `make -k test`, and a final "List failures"
  step repeats the failing lines at the end of the log.
- **macOS bootstraps: a harness bug, not a miscompile.** On Darwin the
  clang-built `c2m` is compiled with
  `-DADDITIONAL_INCLUDE_PATH=<xcrun SDK>/usr/include`, but
  `C2M_BOOTSTRAP_FLAGS` lacked it. So the stage-1 c2mir read the Command Line
  Tools SDK instead, and stage 2 parsed different headers. Fixed in
  `GNUmakefile`; reproduced on Linux by giving stage 1 an extra header
  directory (`differ: char 8, line 1`, as on macOS 15).
- **Linux arm64 lazy-bb: 128-bit long doubles lost their upper half.** AAPCS64
  preserves only the low 64 bits of v8-v15 across a call. The register
  allocator consults `call_used_hard_regs[MIR_T_UNDEF]` only, so it kept long
  doubles in v8-v15 across calls, and native code in between -- the lazy
  generation hooks, any C callee -- clobbered the upper half. That is all
  seven 128-bit c-test failures and the bootstrap mismatch (the self-compiled
  c2mir wrote 2^64 as `0.0L`; reproduced under qemu-user, deterministic, and
  present before mir#10 as found above). `target_hard_reg_type_ok_p` now keeps
  a 16-byte `MIR_T_LD` out of v8-v15. This reached turmeric:
  `TUR_JIT_GEN=lazy`, the default, sends every function's first call through
  the C hook. Covered by `c-tests/new/ldouble-live-across-calls.c`.
- **macOS `va-struct-args.c`: `va_arg` returned a named argument.** On Apple
  every vararg is on the stack after the named args that did not fit in
  registers, and `va_start` pointed at the first of those, in the generator
  (`mir-gen-aarch64.c`) and the interpreter (`mir-interp.c`,
  `mir-aarch64.c`) alike. The variadic lead above was right for this test,
  not for the bootstraps.

mir#12's own run was green on all four legs with the gate in place.
