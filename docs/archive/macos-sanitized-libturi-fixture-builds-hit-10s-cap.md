# macOS: the first fixtures that link the sanitized `libturi` time out building under `run.sh`'s 10 s cap

**RESOLVED 2026-10-03** by fix direction 1.  `tests/run.sh` now builds a
three-line program that imports `arc` once, untimed, with the fixtures' `$TUR`
and `$BUILD_CC`, before dispatching the happy-path fixtures -- the same shape
as the r7rs prelude warm-up above it.  It runs only when the run reaches a
fixture whose sources import an autolinking module (`arc`, `httpd`, `reactor`,
`turi/eval`, `r7rs/eval`, or `(scheme eval)`; 47 fixtures today), so a
filtered run that touches none of them pays nothing.  Whichever of those
fixtures comes first -- under any shard, filter, or new fixture that sorts
before `arc-` -- now builds warm, so the two `expected.timeout` overrides
(`arc-basic`, `arc-weak-upgrade`) are deleted.

Not measured on macOS (fix direction 2): the warm-up covers the cold cost
whatever its split between compile, link and first exec.  If an `arc-`,
`httpd-` or `reactor-` build times out on the macOS `Test` leg after this,
the warm-up did not cover it -- reopen this report rather than restoring
the overrides.

**Filed 2026-10-01**, investigating CI #3081's red `Test (macos-latest)`.

**Severity:** low-medium (intermittent CI red on `main`; no product defect).
The build succeeds and the program passes. It just takes longer than the suite
allows on the macOS runner, some of the time.

## Summary

`tests/run.sh` gives every fixture 10 s per phase, and the **build** phase is
timed too (`_run_timed "$fixture_timeout" "$TUR" build ...`). A fixture whose
program imports a stdlib module that autolinks `-lturi` (`arc`, `httpd`,
`reactor`, `turi/eval`, `r7rs/eval`) links `libturi.a`. Under the Debug build
that archive is ASan-instrumented, so `resolve_autolink_flags` (src/main.c)
compiles the whole program with `-fsanitize=address,undefined` at `-O2` as
well. Those builds are several times dearer than a plain fixture's. On the
macOS `Test` leg, the first two of them in run order have now timed out twice
in the last 11 `main` runs:

| `main` commit | run | failed |
| --- | --- | --- |
| `82ccdc55` (09-30 18:59) | CI #3052 | `arc-basic` (build failed) |
| `53319bc6` (10-01 03:08) | CI #3081 | `arc-basic`, `arc-weak-upgrade` -- `tur build timed out (>10s)` |

Neither commit touched the compiler or these fixtures. Linux, Windows, and
the other macOS runs pass them.

**Worked around** in the change that filed this report:
`tests/fixtures/arc-basic/expected.timeout` and
`tests/fixtures/arc-weak-upgrade/expected.timeout` set 30 s. `run.sh` applies
that override to the build and run phases alike. The cause is not fixed, so
this report stays open.

## Why these two

- **The compile is sanitized.** In the Debug build, the `cc` line for
  `arc-basic` carries `-O2 -fsanitize=address,undefined ... -lturi`, while
  `adt-basic` gets plain `-O2`. Both emit about 8,500 lines of C, so the
  difference is the flags, not the program.
- **They come first.** 44 happy-path fixtures link `libturi`: 35 `httpd-*`,
  6 `reactor-*`, the 2 `arc-*`, and `eval-import`. None had a timeout
  override. `eval-import` is `requires.dedicated-runner`, so it
  is skipped under `run.sh`. Alphabetically, that makes `arc-basic` and
  `arc-weak-upgrade` the **first** sanitized-libturi builds a run reaches,
  and they are dispatched next to each other.
- **The first such build pays a cold cost.** Measured on a 4-core Linux
  container with the Debug `tur` and an emptied `/tmp/tur-build`:

  | build, in this order | time |
  | --- | --- |
  | `arc-basic` (first sanitized build) | 1.98 s |
  | `arc-basic` again | 0.87 s |
  | `arc-weak-upgrade` | 0.70 s |
  | `httpd-h1-basic` | 1.66 s |
  | `adt-basic` (unsanitized) | 0.35 s |

  What the 1.1 s goes to was not measured. Candidates include the linker
  reading the 110 MB Debug `libturi.a` and the sanitizer runtimes into cache.
  The driver's own `nm libturi.a | grep -q __asan_init` probe is not it: it
  stops at the first match and takes about 20 ms. On a 3-core macOS runner, while
  `run.sh` keeps `nproc` builds in flight and fresh binaries go through the
  OS's first-run scanning, the same cold step is the likeliest place for
  10 s to go.

The macOS part of that is an inference from the pattern: same two
fixtures, the first in order, and intermittent. It was not observed on a
macOS box.

## Repro

Not deterministic. The CI shape is the full fixture suite on
`macos-latest` (`Test (macos-latest)`, step "Run fixture suite"). The cost
gap shows up locally:

```sh
rm -rf "${TMPDIR:-/tmp}/tur-build"
for f in arc-basic arc-basic adt-basic; do
  /usr/bin/time -p ./build/tur build tests/fixtures/$f/input.tur -o /tmp/x 2>&1 | grep real
done
TUR_SHOW_CC=1 ./build/tur build tests/fixtures/arc-basic/input.tur -o /tmp/x 2>&1 | grep '^CC:'
```

## Residual risk

The other 41 sanitized-libturi fixtures that `run.sh` runs (`eval-import`
has its own runner) still run on the 10 s default. If
shard membership, a `TUR_TEST_FILTER`, or a new fixture that sorts before
`arc-` makes something else the first sanitized build, that fixture inherits
the cold cost. Timeouts on `httpd-*` or `reactor-*` builds on macOS would be
this report, not a new one.

## Fix directions

1. **Warm the sanitized link once, untimed, before the timed fixtures.**
   `run.sh` already does exactly this for the r7rs prelude
   (`r7rs-prelude-library-cold-compile`, the `_r7rs_warm` block): build a
   one-line program that imports `arc` (or any `-lturi` module) with the
   fixtures' `$TUR` and `$BUILD_CC`, and throw the result away. This covers
   whichever fixture comes first, so the two `expected.timeout` files could
   then be dropped.
2. **Measure on macOS** before choosing. A `TUR_SHOW_CC=1` build of
   `arc-basic` with timestamps, cold and warm, would say whether the cost
   is the compile, the link, or the first exec.
3. **Scale the default build budget for sanitized links** in `run.sh`,
   for example by reading `tur`'s `asan:` sidecar line. This is broader, and
   it hides real slowdowns along with the cold cost.
