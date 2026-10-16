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
nondeterminism in the aarch64 path that no test points at. Unverified lead:
Apple arm64 passes variadic arguments differently from AAPCS64, and the other
failure, `va-struct-args.c`, is a variadic test. It is unknown whether Linux
aarch64 shares it, because the fork's `aarch64-test` workflow never ran (it
asked for upstream's self-hosted runner). The `ubuntu-24.04-arm` leg added in
the CI rework will show it in its log on its first run.

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
