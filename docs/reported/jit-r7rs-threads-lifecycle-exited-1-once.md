# `r7rs-threads-lifecycle` exited 1 once under the JIT, after the fork fix

**Severity:** low -- one occurrence in 26 Linux JIT runs, unreproduced, no
product impact known. Filed so a second occurrence is recognised, not to
claim a bug.

**What was seen.** `JIT engine (ubuntu-latest)`, run `37149022036`
(`dde72b1fd`, 2026-10-03 20:21Z), `tests/run-jit.sh`:

```
FAIL r7rs-threads-lifecycle -- exited 1 (expected 0)
jit fixture summary: 3415 passed, 1 failed, 61 skipped
```

That commit already carries #1050's fork skip
([jit-fork-child-inherits-asan-allocator-lock](../archive/jit-fork-child-inherits-asan-allocator-lock.md)),
and the symptom is not that one: a failed fork check prints
`(fork-failures 1)` and still exits 0, which `run-jit.sh` reports as a `stdout
mismatch`. Exit status 1 from a sanitized `tur jit` with
`ASAN_OPTIONS=detect_leaks=0` is most likely an ASan/UBSan error report in the
JIT'd program (ASan's default `exitcode` is 1) -- for example in the detached
rounds, which retire thread records concurrently -- but that is inference. The
console log is grep-filtered to the FAIL line.

**Repro.** Not reproduced: 40 runs of `./build/tur jit input.tur` from the
fixture dir on a Linux `-DCMAKE_BUILD_TYPE=Debug -DTUR_JIT=ON` build (GCC
13.3), `ASAN_OPTIONS=detect_leaks=0`, 4 concurrent, all printed the expected
three lines and exited 0 (2026-10-04).

**Next step.** The full output is in that run's `jit-ctest-log-ubuntu-latest`
artifact (ID 11283599190, 90-day retention): look for the
`r7rs-threads-lifecycle` block and any `ERROR: AddressSanitizer` /
`runtime error:` lines. If it is a sanitizer report, it names the bug; if it
is empty, the harness's exit-status capture is the next suspect.
