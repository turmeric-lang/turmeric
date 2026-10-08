# `JIT engine (ubuntu-latest)` fails 18% of commits, and `continue-on-error` absorbs all of it

**Severity: medium when filed; RESOLVED 2026-10-03 as "already known".** No
product defect and, as it turns out, no unknown hiding place either: the leg's
redness decomposes entirely into two fixtures that each already had a report.

Filed 2026-10-03 while diagnosing the macOS leg's intermittent red
([macos-jit-leg-stall-unexplained](../reported/macos-jit-leg-stall-unexplained.md)).
The macOS rate turned out to be 1%; measuring the Linux one to compare is how
this surfaced. Filed deliberately as a measurement only, with "which fixtures
fail is NOT established" stated as its own limit -- and then established the
same day, which is what closes it.

## The measurement

One entry per commit, from `suite-timings-2026.jsonl` on the `ci-metrics`
branch, suite `tur_jit_fixture_tests`:

| env | commits | pass | fail | fail rate | episodes |
| --- | --- | --- | --- | --- | --- |
| Linux GNU-13.3.0, 4 cores | 306 | 251 | 55 | **18%** | 37 (29 isolated, longest 10) |
| macOS AppleClang-21, 3 cores | 289 | 287 | 2 | 1% | 2, both isolated |

Duration is not the story the way it is on macOS: Linux runs
min 267 / p50 501 / p90 630 / max 738 s, a far tighter spread than macOS's
500-1648 s, and well inside the suite bound. **46 of the 55 failing runs
report exactly `failed=1`**, two report 2, and seven predate the field -- so
one fixture at a time, not a scatter.

## What fails: two fixtures, 18 of 18 sampled

`jit-ctest-log-ubuntu-latest` from the 18 newest failing Linux commits, every
one of which still had its artifact:

| fixture | runs | kind | owner |
| --- | --- | --- | --- |
| `r7rs-threads-lifecycle` | 9 | `(fork-failures 0)` -> `(fork-failures 1)` | [jit-fork-child-inherits-asan-allocator-lock](jit-fork-child-inherits-asan-allocator-lock.md) (resolved 2026-10-04) |
| `fn-field-carrier-shim-read-typed` | 9 | zero-byte stdout (`@@ -1,7 +0,0 @@`) | [fn-field-carrier-shim-read-typed](fn-field-carrier-shim-read-typed.md) (**resolved**) |

Ordered by recency they interleave as threads (6 oldest), carrier-shim (9),
threads (3 newest) -- consistent with a persistent ~1-in-3 flake running
underneath a deterministic standing bug, and the two runs reporting
`failed=2` are where they overlapped.

**`r7rs-threads-lifecycle` is the persistent component and is fully
root-caused already:** a child forked under the sanitized `tur jit` inherits
ASan's allocator lock, hangs in `FutexWait` inside
`SizeClassAllocator64::GetFromAllocator`, and dies of its own `alarm(5)`. Only
Debug + `-fsanitize=address` + `tur jit` is affected -- which is exactly the
CI JIT legs -- and the compiled and Release paths use glibc `malloc`, which is
fork-safe. Severity there is **low, explicitly "no product impact"**, and that
report measures ~1 run in 3 locally against the 18% seen here.

**`fn-field-carrier-shim-read-typed` is already fixed**, which is why it stops
appearing in the three newest failures. Its zero-byte stdout is worth noting
for shape: that is the same signature the macOS report records for a killed
fixture misreported as a stdout mismatch, so a zero-byte "mismatch" on this
harness is a prompt to check the clock before reading it as a wrong answer.

## What this corrects in the filing

The original text warned that "a real JIT regression landing on Linux would
look exactly like the existing noise". The structural point stands -- a leg
that cannot fail the run is a leg nobody reads -- but the alarming reading
does not: the noise is one known, root-caused, product-impact-free sanitizer
artifact, not an unexamined 18%.

It also assumed the asymmetry in `ci.yml` (`continue-on-error: ${{ matrix.os
!= 'macos-latest' }}`, "BLOCKING on macOS, non-blocking on Linux") rested on a
premise the data contradicts. That is half right. Linux does fail 18x more
often, but for a reason that has nothing to do with the JIT being less sound
there: the Linux leg runs the ASan fork path that the macOS leg's toolchain
does not trip the same way. The asymmetry is defensible; it is just not
defensible for the reason the comment gives.

## What would actually improve the signal

Not a gating change. **Make `r7rs-threads-lifecycle` stop failing**, per the
first fix direction in its own report -- skip the fork check when the program
runs on a sanitizer allocator, which narrows a skip that existed before the
`g_gen_lock` fix rather than restoring it. That alone should take this leg from
18% to near zero, at which point its failure count means "something new broke"
and the `continue-on-error` question becomes worth asking. This is the same
order the browser suites established: clean baseline first, then gate.

## How this was measured

`git show origin/ci-metrics:suite-timings-2026.jsonl`, filtered to
`suite == "tur_jit_fixture_tests"`, deduplicated by `sha`, grouped by the
`(os, cc, nproc)` tuple, consecutive same-status runs collapsed to separate
isolated failures from episodes. Fixture names and diffs come from
`gh run download <run> -n jit-ctest-log-ubuntu-latest` for the 18 newest
failing commits, using the `run_id` each timings row already carries. No
fixture was run locally: the JIT harness needs a `-DTUR_JIT=ON` build.
