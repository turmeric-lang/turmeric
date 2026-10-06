# `tur_r7rs_gc`: torture at a fixed interval re-marks a large live heap thousands of times (quadratic), so the suite is minutes of re-marking

**Severity:** medium (CI time; the `tur_r7rs_gc` ctest target hit its 720 s
cap on a 4-core container). `tests/run-r7rs-gc.sh` runs every compiled r7rs
and Saffron fixture with `TUR_GC_TORTURE=31`, a full collection every 31
allocations. A collection marks the whole live heap. A program that builds a
large structure therefore pays its live size once per 31 allocations for the
rest of its run. That is O(allocations x live) in total: quadratic in the
program's size, and nearly all of it re-marks objects that have not changed.
Found 2026-10-05.

## Measured (serial, Debug `tur`, 4-core container)

| fixture | plain run | torture 31, before | torture 31, after the 2026-10-05 mark-path work |
| --- | --- | --- | --- |
| `r7rs-tail-call-hand-on-through-static-call` (300,000-closure chain) | 0.17 s | 292 s | 99 s |
| `r7rs-srfi-41` | 0.12 s | 99 s | 38 s |
| `r7rs-apply-long` (100,000-element lists) | 0.05 s | 93 s | 28 s |
| `r7rs-apply-standard-variadic-many` | 0.04 s | 91 s | 31 s |
| `r7rs-write-labels` | 0.10 s | 70 s | 21 s |

Phase 1 of the harness (232 programs) spends 923 s of torture runs before and
364 s after, against 13 s of plain runs. Its longest single case bounds the
harness's wall time whatever the parallelism.

The 2026-10-05 work made each collection ~2.7x cheaper. It did not change the
shape: on `r7rs-apply-long`, 12,967 collections each mark up to ~6 MB.

## The knob, and what it buys

`TUR_GC_TORTURE_SCALE=R` (src/runtime/r7gc.c, off by default) stretches the
torture interval to `max(TUR_GC_TORTURE, live objects / R)`, recomputed after
each collection. A small heap keeps the every-31 interval, and a large one
gets proportionally fewer collections, which keeps total marking work linear.
Output is correct throughout:

| fixture | interval 31 | `SCALE=64` | `SCALE=16` |
| --- | --- | --- | --- |
| `r7rs-tail-call-hand-on-through-static-call` | 89.8 s, 29,036 collections | 0.81 s, 514 | 0.33 s, 151 |
| `r7rs-apply-long` | 27.4 s, 12,967 | 0.32 s, 470 | 0.12 s, 184 |
| `r7rs-srfi-41` | 34.6 s, 41,664 | 1.16 s, 4,111 | 0.36 s, 1,340 |

The whole harness: 5 m 49 s at a fixed 31, 4 m 04 s with
`TUR_GC_TORTURE_SCALE=64` exported, 249 passed either way. The rest of its
time is building the 232 programs and the serial thread phase, which includes
the thread fixtures' cold prelude variants
([r7rs-prelude-library-object-varies-with-the-program](r7rs-prelude-library-object-varies-with-the-program.md)).

## The decision this needs

Turning the knob on in the harness weakens the gate for exactly the programs
with large live heaps. A missing root fails only if a collection lands while
the object is reachable through nothing scanned.
- A window longer than the interval is caught every time at either setting.
- A shorter window is caught with probability window / interval per
  occurrence. These programs repeat each allocation site tens of thousands of
  times, so a stretched interval still lands in each site's window many
  times.
- The program's small-heap phases (start-up, the prelude) keep the every-31
  interval.

Choices:

1. **Export `TUR_GC_TORTURE_SCALE=64` in `tests/run-r7rs-gc.sh`** for phase 1
   (and phase 3's every-31 cases). Keep a fixed interval for the cases the
   harness runs at every allocation.
2. **Leave the gate as is** and keep the 720 s budget. It passes in ~6 min
   on 4 cores now.
3. **A second, scaled pass in CI**, keeping the fixed-interval run nightly.

## Pinned by

Nothing yet. If option 1 is taken, the harness's summary line should say the
scale it ran at.
