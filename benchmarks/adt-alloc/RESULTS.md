---
title: ADT allocation ceiling -- results
category: Benchmarks
description: What the seven representations cost, measured on a reconstructed harness -- and the finding that region reclamation alone buys 82% of the total win with no ABI change at all.
---

# ADT allocation ceiling -- results

Generated 2026-08-25 by `benchmarks/adt-alloc/ceiling.c`, best of seven timed
runs per point, each representation in its own process. Raw data:
[ceiling-results.csv](ceiling-results.csv).

**Read the provenance note first.** The original harness (2026-08-22, commit
`00052f16`) was never committed -- `.gitignore`'s blanket `*.c` had negations
for `src/ tests/ examples/ docs/ tools/` but not `benchmarks/`, so `git add`
skipped it silently and the commit that published its numbers landed with only
the README. This harness is **reconstructed** from the report's description and
from the layouts `tur emit-c` actually emits for `stdlib/logic.tur`'s `Term` and
`Subst`. It is not the original file recovered. Where the numbers disagree with
the published ones, this is the one that can be re-run.

## The numbers

`ns_per_op`, where one op is one binding built and one looked up (n=8 bindings
per substitution). Checksums are identical across all seven rows at every pass
count, which is what stops a representation from looking fast by doing less.

| | representation | 1k | 16k | 64k | vs A | published |
|---|---|---:|---:|---:|---:|---:|
| A | boxed, leaked (**today**) | 63.1 | 65.7 | 66.6 | 1.00x | 1.0x |
| B | boxed, **freed** | 26.4 | 26.4 | 28.2 | **2.49x** | 2.6x |
| C | Term by value, spine boxed, leaked | 41.7 | 46.6 | 50.5 | 1.41x | 1.8x |
| D | by value **and** freed | 16.3 | 14.6 | 16.5 | 4.50x | **18x** |
| E | boxed, leaked, slab | 32.0 | 38.2 | 36.7 | 1.72x | 2.4x |
| F | by value, **arena**-reclaimed | 6.4 | 6.0 | 6.0 | **10.95x** | -- |
| G | boxed, **arena**-reclaimed | 9.2 | 8.6 | 9.1 | **7.64x** | -- |

Rows F and G are new; the original had only A-E.

## Three findings

**1. The reclamation MECHANISM is the whole story, not the ABI.**

This is the result that matters, and it inverts the ordering rationale in the
[SR plan](../../docs/archive/sum-representation-plan.md).

Row G is region reclamation over **today's boxed representation** -- no
by-value lowering, no ABI change, no `adt_is_flat_product` work, none of SR1 or
SR4. It reaches **7.6x**. Row F adds the entire by-value ABI change on top and
reaches 11.0x.

So the arena alone buys roughly **70% of the total win with none of the ABI
work**, and everything SR1+SR4 propose -- weeks of codegen, 33 red fixtures at
the SR1 gate, and a rewrite of `stdlib/logic.tur` off the carrier -- is the
increment from 7.6x to 11.0x.

The same relationship appears in the malloc rows: by-value is worth +81% when
allocation is expensive (B 2.49x -> D 4.50x) and +43% when it is cheap
(G 7.64x -> F 10.95x). By-value removes one of the two allocations per binding,
so it is worth proportionally *less* the cheaper allocation gets. Fix the
allocator and you shrink the prize the ABI change is competing for.

**2. The published 18x does not reproduce, and the gap is the reclamation
mechanism.** Row D measures 4.50x here against a published 18x. That published
figure implies ~5 ns/op while still doing one `malloc` per binding, which is
not achievable -- a malloc/free pair alone costs more than that. The number the
original was probably reaching is row F (10.95x), which reclaims by resetting a
region rather than freeing per node. Stated carefully: **"18x needs both" is
not supported; what the transformative number needs is a region, and the ABI
change is a secondary multiplier on it.**

**3. Reclamation is flat; leaking is not.** B (26.4/26.4/28.2), F
(6.4/6.0/6.0) and G (9.2/8.6/9.1) are flat across a 64x range. A
(63.1/65.7/66.6), C (41.7->50.5) and E (32.0->38.2) all climb. That confirms
the report's own corrected finding -- this is a footprint problem before it is
a speed problem -- and it is another count against the slab, which is the only
proposed *fix* that still climbs.

## What reproduces and what does not

**B reproduces** -- 2.49x here against a published 2.6x. That is the row the
[slab-shelving decision](../../docs/archive/multi-variant-adts-always-heap-allocate.md)
turns on, and it holds.

**The slab looks worse, not better.** E measures 1.72x against a published 2.4x,
while B holds at 2.49x. So reclamation beats the slab by a wider margin than the
published numbers suggested (2.49 vs 1.72, not 2.6 vs 2.4). The decision to
shelve the slab is strengthened, not weakened. Part of the reason is visible in
row A: glibc's `malloc` with nothing ever freed is *already* close to a bump
allocator, because it just walks the top chunk -- so a slab is competing against
a cheaper baseline than it looks.

**C and D come in lower** than published (1.41x vs 1.8x, 4.50x vs 18x). C is
close enough to be machine variance; D is not, and finding 2 is why.

Absolute ns differ from the published run throughout -- different machine -- so
only ratios are comparable, which is what the README always said.

## The caveat on rows F and G

**An arena needs a region whose end is known**, and that is the hard part, not
the bump pointer. This harness resets at the end of each pass because the pass
boundary is obvious; real code has no such boundary handed to it, and inferring
one is region inference.

That does not make F and G hypothetical, but it does say where they apply:
they are the honest ceiling **for workloads with a natural region**, and the one
that motivated this whole report -- the refinement solver's `logic.tur`
substitution, built per query and discarded whole -- is exactly such a workload.
A per-query arena there is not region inference; it is one `arena_reset()` at a
call site that already exists.

Generalising to arbitrary user `defdata` is a different and much larger
question, and nothing here answers it.

## Re-run 2026-09-02 (RM0(a))

Same harness, unchanged, 16k passes, one process per row, on a slower box
(`A` = 103.4 ns/op here). Ratios over A: B 2.6x, C 1.3x, D 3.9x, E 1.4x,
F 13.1x, G 7.0x. Checksums identical (1472000). The ordering and the
mechanism-over-ABI finding hold; absolute numbers are not comparable across
the two boxes and are not added to the CSV.
