# The 2026-10 performance audit, ranked

The one-page result of [performance-audit-plan](../../archive/performance-audit-plan.md).
Each work package has its own write-up with the method and the raw data;
this page is the part the plan says a reader should be able to act on
without reading the rest: **what to build next, across every surface, in
order.** The audit lands no optimizations itself (plan section 0).

Box: the WP0 baseline ([README.md](README.md)), except where a row says CI.
Write-ups: [WP0](wp0-baseline.md) baseline, [WP1](wp1-compile-latency.md)
compile latency, [WP2](wp2-runtime-hot-paths.md) emitted-program runtime,
[WP3](wp3-memory.md) memory, [WP4](wp4-engines.md) interpreter and JIT,
[WP5](wp5-web.md) web; WP6 (nightly tracking) and WP7 (the guides) changed
CI and the guides rather than producing a write-up.

## Pick from here

Ordered by what a change buys against what it costs, leaning runtime on a tie
(plan section 8, decision 5). Each row names the write-up that has the
measurement and the fix direction. The "buys" figures are this box's; a PR
that implements a row checks its own with a before/after there
([README.md, "Re-measuring on the baseline box"](README.md#re-measuring-on-the-baseline-box)).

| # | change | buys | effort | where |
| --- | --- | --- | --- | --- |
| 1 | **Read the split preamble's thread-locals directly** (`extern __thread` in the hosted split, or a `const` accessor) | ~25% of cycles across the corpus, up to 57% on CPS loops; keeps the split's 25-40% cc saving | small-medium | WP2 #1, WP1 #3 (R-2) |
| 2 | **`realloc` in `vec-push!` growth** | -46% wall on a push loop | small | WP2 #2 (R-6) |
| 3 | **Subset the Iosevka fonts, and precache only woff2** | ~2 MB a first visit downloads before Ready, and 4.7 MB of `.woff` the service worker fetches and a woff2 browser never uses | small | WP5 #1 |
| 4 | **c2mir: inline small struct copies** (one MIR fork change, batched) | -29% on Saffron's dynamic-call JIT runs, -28% on `tailcall-dyn-deep` | small | WP4 #2 (E-3) |
| 5 | **Interpreter: resolve a module reference's qualified key once; probe without building an error** | the 1.5x instructions code inside a `defmodule` pays | small | WP4 #3 (E-1) |
| 6 | **Retired regions: O(1) registry removal, a cheap shutdown** | 71% of `region-rounds-100k`; frees that stay cheap in a long run | small-medium | WP2 #4 (R-3, filed) |
| 7 | **Prune the cc path's unit the way `jit_prune` prunes the JIT's** | gcc spends 87% of a small build's cc time parsing prelude C nothing uses; an empty build is 151 ms, 135 of it cc | medium | WP1 #1 (C-1) |
| 8 | **Cache the MIR image of a program's unit** | ~72 ms of a ~110 ms quick `tur jit` run | medium | WP4 #1 (E-2) |
| 9 | **Start the eval worker and the doc-names fetch beside the Monaco import** | 0.25-0.4 s of a ~0.9 s local cold Ready off the critical path | small | WP5 #4 |
| 10 | **`re/replace-all` through a string builder** | linear instead of quadratic in its matches (201 ms at 16k matches today, ~8 s at 100k) | small | below (R-9) |
| 11 | **Interpreter: cache a dynamic operator's builtin per call site; compare names by pointer** | ~15% of interpreted Saffron, ~3% of the Turmeric loops | small | WP4 #4 (E-1) |
| 12 | **In-place HAMT transients** | ~2/3 of a batch insert | medium | WP2 #3 (R-5) |
| 13 | **Take the 168 stdlib files startup never reads out of the wasm** | ~0.48 MB of the 1.6 MB wasm download, 2.8 MB of file system in each of two workers | medium | WP5 #2 (W-1) |
| 14 | **Pool async continuation nodes; skip `calloc`'s zeroing** | most of the ~813 ns `async` park/resume turn | medium | WP2 #5, WP4 (R-10) |
| 15 | **Pre-compress `turmeric.wasm` at brotli -q 11** | 0.37 MB (23%) of the wasm transfer | small-medium | WP5 #3 |
| 16 | **`--release` = `-O3`** (decision 2), LTO optional | ~6%, ~4% more with LTO | small | WP2 #7 (R-7) |
| 17 | **R-1 properly**: effect rows for function values, plus drop-before-call | 42-45% of instructions on combinator-style code, ~250 stdlib functions off the CPS path | large | WP2 #6 (R-1, R-11) |
| 18 | **A build-time prelude for the web worker** (W-2) | ~100 ms of every eval-worker start, ~20% of a repeat visit | medium-large | WP5 #5 |
| 19 | **A cached prelude unit for plain Turmeric** | the ~13 ms of front end + emit the prelude costs every build | medium-large | WP1 #2 (C-1) |

Smaller items the write-ups rank but this table leaves out: the DK reap
list's array reused across entries (WP3 #2), `--dump-cps-coloring` printing
the emit-time coloring (WP3 #4), `tur check` stopping at emission (WP1 #4),
front-end string compares (WP1 #5), `tur build <file>` honouring the
manifest's `:experiments` (WP1 #6).

## Filed

Reports the audit filed or re-measured, in `docs/reported/` while open:
`region-retired-generations-make-shutdown-quadratic` (R-3; fixed since, now in
`docs/archive/`), `region-rewind-fixtures-never-rewind` (WP2),
`jit-string-literal-identity-differs-from-cc` (WP0), and
`carrier-sum-option-boxes-have-no-owner` (M-3, re-measured).

## Closing the definition of done

Plan section 7, item by item:

| item | state |
| --- | --- |
| WP0's baseline: corpus, box header, every metric on all three engines | done ([wp0-baseline.md](wp0-baseline.md)) |
| `tur build --timings` prints a zone table and writes JSON; the guide documents it | done (WP1) |
| every R, M, C, E and W row has a number and a ranked row, a "not a problem" entry, or a report | done -- the plan's section 2 says which, row by row |
| D-1 to D-5 resolved in the guides | done (WP7) |
| the nightly publishes runtime, compile-time and binary-size rows; `/ci` shows them | built (WP6); the rows start with its first night after merge |
| the triangle and the memory cost table regenerated from checked-in harnesses, dated | done (WP4, WP3) |

Three rows needed a measurement after the work packages, taken for the
close-out on the WP0 box:

- **R-9, string concatenation in loops.** A static census of `stdlib/` and
  `examples/`: 20 concat call sites in the stdlib, 7 of them inside a looping
  or self-calling defn; 3 in examples, none in a loop. The one that matters is
  `re-replace-go` (`stdlib/re.tur`), which grows its result with `str-concat`
  once per match: `re/replace-all` over an N-character string with N matches
  takes 5 / 14 / 51 / 201 ms at N = 2k / 4k / 8k / 16k -- 4x per doubling, the
  quadratic the memory guide warns about.
- **M-4, wide elements in containers.** `TUR_REPR_CENSUS=1 tur emit-c` over
  the 72 corpus and example programs: 4 decide `container-elem boxed-agg`
  beyond the prelude (which decides none), and all four are Saffron, whose
  dynamic values are 16 B. No plain-Turmeric example and nothing in the
  stdlib boxes a wide container element.
- **C-4, the two solver reports.** Both were already resolved
  (2026-09-28 and 2026-09-05); the plan's "marked OPEN" was stale.
