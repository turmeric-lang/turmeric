# macOS CI fails both fixture suites while Linux passes on the same commit

**RESOLVED 2026-08-01.** Both halves are fixed and neither was a macOS-platform
defect. The JIT half was a test-harness portability bug (fixed in PR #753); the
AOT half was a platform-independent codegen bug that only Linux's
warn-instead-of-error posture hid, now fixed in
[`macos-int-conversion-carrier-pointer-straddles`](macos-int-conversion-carrier-pointer-straddles.md).

The report's original framing -- an undiagnosed macOS-only mystery needing
someone with a Mac -- was wrong on both counts, and no macOS box was needed to
diagnose either. (The GC/Rc report it spawned was resolved on one the same day; see Related.)

Found 2026-07-31 while triaging CI on PR #752.

---

## What failed

Run 2197, head `c091889a4`
(https://github.com/turmeric-lang/turmeric/actions/runs/30621915696):

| job | ctest target | result |
| --- | --- | --- |
| Test (macos-latest) | `tur_tests` | **FAILED** (0% of 1) |
| JIT engine (macos-latest) | `tur_jit_fixture_tests` | **FAILED** (50% of 2) |
| Test (ubuntu-latest) | `tur_tests` | passed |
| Test (ubuntu-latest) | `turi_fixture_tests` | failed -- unrelated, filed as [`ci-cps-tramp-turi-timeouts-under-load.md`](ci-cps-tramp-turi-timeouts-under-load.md) (resolved 2026-08-01) |
| JIT engine (ubuntu-latest) | `tur_jit_fixture_tests` | **passed** |

Not caused by the PR's commits: the previous run (2196, head `dbfecd0d2`) shows
the same two macOS jobs already failing. `Check codegen snapshots` failed in
2196 and passed in 2197 -- unrelated, fixed by the snapshot regen in
`c091889a4`.

## `Test (macos-latest)` -- the straddle report

The AOT suite's tail on run 2202 (`main`, job 91324836416, head `8b1ea4380`):

```
summary: 2495 passed, 4 failed
  - conv-defstruct-option-fn-element (build failed)
  - defalias-composite (build failed)
  - fn-value-matrix-ok-rows (build failed)
  - hkt-ap-fn-in-container (build failed)
```

Exactly the four fixtures of
[`macos-int-conversion-carrier-pointer-straddles`](macos-int-conversion-carrier-pointer-straddles.md),
with exactly its two error shapes. Three corrections followed, and they retired
most of this report:

1. **There was no local-vs-CI disagreement.** The macOS agent's commit message
   on `dbfecd0d2` reported `AOT: 2495 passed, 4 failed`; CI reported
   `summary: 2495 passed, 4 failed`. Same numbers. The "most interesting part of
   this report" was a misreading of its own evidence.
2. **The sanitizer/toolchain hypothesis was dead for this job.** The failures
   were `cc` rejecting emitted C at `-Wint-conversion` -- nothing to do with
   ASan, Homebrew LLVM, or the startup deadlock. It reproduced at any sanitizer
   setting.
3. **The six-hour hang in run 2196 was not this.** `Test (macos-latest)`
   completed in 501s in run 2202. Whatever stalled 2196 was a one-off or a
   different phase; there was no standing hang to hunt.

The failing set was stable across PRs (corroborated on PR #753, job 91329206395:
same four names, `summary: 2496 passed, 4 failed` -- +1 for that PR's one added
fixture), which made the job usable as a regression signal despite being red.

**Closed by the straddle fix**, which takes the corpus to `2500 passed, 0 failed`
on Apple clang 21 / macOS 27 arm64.

## `JIT engine (macos-latest)` -- a harness bug, unrelated to the above

From PR #753, job 91329206401:

```
jit fixture summary: 2008 passed, 407 failed, 47 skipped
```

**All 407 failures were `errors/*` negative fixtures, every one reporting
`jit diagnostic mismatch`.** No positive fixture failed. That shape was the
whole diagnosis: negative fixtures are the only ones that go through
`run_jit_error_fixture`.

### Root cause -- `tests/run-jit.sh:308` called bare `timeout`

The harness already knows stock macOS ships no `timeout(1)` and probes for it at
`tests/run-jit.sh:109-118`, setting `_tur_timeout_bin` to `timeout`, else
`gtimeout`, else empty. The positive-fixture path used the resulting `_run_timed`
wrapper (`:235`, `:239`). **The negative-fixture path was missed and still
called bare `timeout 15`.**

On a runner without GNU coreutils that fails silently and totally:

```
$ ( PATH=/nonexistent timeout 15 sh -c 'echo DIAGNOSTIC >&2' ) 2>err || true
$ cat err
bash: timeout: command not found          <- not the diagnostic
```

`timeout` is not found, the command fails, the trailing `|| true` swallows it,
`jit.stderr` gets a shell error instead of a compiler diagnostic, and every
`expected.diag` needle misses.

This is the one place a local-vs-CI disagreement was *real*: the macOS JIT leg
was made blocking on a hand-measured local baseline
(`.github/workflows/ci.yml:195-212`), and a developer Mac with Homebrew
coreutils on `PATH` has `timeout` and is green while the GitHub macOS runner
does not and is not. Linux is green because `timeout` exists there.

**Fixed in PR #753** (`:308` now uses `_run_timed`). On Linux it is a
byte-for-byte no-op, so it could only change the macOS result.

### Confirmed: 407 -> 6

First macOS JIT run carrying the fix (run 30685603734, job 91330616390, head
`03ec0d4a`):

```
jit fixture summary: 2409 passed, 6 failed, 47 skipped
```

Every `errors/*` negative fixture passes. 401 of the 407 were the harness bug.
The **6 residuals were real findings this bug was masking** -- all GC / `Rc` /
weak-reference fixtures, all macOS-only, and failing on `main` too, buried in the
401. Filed separately as
[`jit-macos-gc-rc-weak-fixtures-fail`](jit-macos-gc-rc-weak-fixtures-fail.md);
this report never waited on them. (They too are now resolved.)

## Still worth doing

**Upload the harness stdout (or `tests/**/actual.*`) as a CI artifact on
failure.** Both halves of this report cost a log-window fight to diagnose, and
the next one will too. Filed here rather than carried forward because it is an
infrastructure improvement, not a defect.

## Related

- [`macos-int-conversion-carrier-pointer-straddles`](macos-int-conversion-carrier-pointer-straddles.md)
  -- the AOT half. Resolved.
- [`jit-macos-gc-rc-weak-fixtures-fail`](jit-macos-gc-rc-weak-fixtures-fail.md)
  -- the 6 JIT residuals the harness fix exposed. Also closed: they asserted on
  a process-wide live-heap probe, which under one-process `tur jit` measures the
  compiler's allocator. They now read the CG6 collector counter and run under
  both paths. Never a GC/Rc/JIT defect.
- [`ci-cps-tramp-turi-timeouts-under-load.md`](ci-cps-tramp-turi-timeouts-under-load.md)
  -- the unrelated ubuntu `turi_fixture_tests` failure noted in the table
  above; resolved 2026-08-01 (peak RSS, not CPU contention) and archived.
