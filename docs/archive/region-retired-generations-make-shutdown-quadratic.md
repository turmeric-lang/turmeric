# Retired region generations make shutdown quadratic and every free linear

**Resolved 2026-10-15** (`claude/region-retire-registry`; performance audit fix
#6). Fix directions 1-3 below are in `src/runtime/region.c`:

1. **Shutdown:** `tur_region_shutdown` drops this thread's registry entries in
   one pass (`reg_remove_mine`). It clears the registry outright when every
   entry is this thread's; otherwise it filters it against this thread's
   sorted arenas.
2. **`reg_remove`:** scans from the end.
3. **`tur_region_owns`:** answers for retired generations through an
   open-addressing index of 64 KiB address blocks (`g_ridx`), filled at
   retirement through the new `arena_each_slab`. It is exact, because a
   retired arena never changes, and a hit is confirmed with `arena_owns`.

Measured on the WP0 box against `main`, with `remeasure.sh`, 3 sweeps per side:

| program | cc | jit |
| --- | --- | --- |
| `region-rounds-100k` | 827 -> 223 ms (0.269) | 1000 -> 397 ms (0.397) |
| `benchmarks/bench-region-retired-frees.tur` (new) | 2.75 s -> 22 ms (0.008) | 2.61 s -> 122 ms (0.047) |

`bench-region-retired-frees` makes 1M frees after 1,000 retirements. At 5,000
retirements the old walk took 51.5 s and the index 0.03 s.
`tests/fixtures/region-retired-index-owns` pins the ownership answer across
2,000 retirements and several table growths.

Direction 4 (a visible signal for brackets that always retire) is not done.
The reason the fixtures retired was its own report (since fixed),
[region-rewind-fixtures-never-rewind](region-rewind-fixtures-never-rewind.md).

**Severity:** medium (performance; unbounded in a long-running program).
Found 2026-10-14 by the performance audit's WP2
([wp2-runtime-hot-paths](../artifacts/perf-audit-2026-10/wp2-runtime-hot-paths.md),
R-3).

A region generation that cannot rewind is retired: its arena stays mapped
(something may still point in) and stays in the process-wide ownership
registry `g_reg` for the life of the process (`retire_top`,
`src/runtime/region.c:266`). A program whose brackets keep retiring therefore
pays, per retirement:

- **at shutdown, quadratically:** `tur_region_shutdown` (`region.c:387`)
  frees each retired arena with its own `reg_remove`, and `reg_remove`
  (`region.c:163`) scans `g_reg` from index 0 for its argument -- R x R/2
  compares for R retirements;
- **on every node free, linearly:** `tur_region_free` asks `tur_region_owns`
  (`region.c:337`), which walks every live AND retired generation and each
  one's slab list before it will `free()` a pointer;
- **in memory, one slab each** (~4 KiB), which is by design once a
  generation retires.

## Repro

WP0's `benchmarks/audit/sized/region-rounds-100k.tur` (a copy of
`tests/fixtures/region-with-region` with `(rounds 100 8 0)` raised to 100000;
every one of its brackets retires -- see
[region-rewind-fixtures-never-rewind](region-rewind-fixtures-never-rewind.md)):

```
rounds    wall    TUR_REGION_STATS
  1000     4 ms   pushes=1003   rewinds=0 retires=1003
 10000    40 ms   pushes=10003  rewinds=0 retires=10003
100000  1118 ms   pushes=100003 rewinds=0 retires=100003
```

10x the rounds costs 28x the time at the top end. `perf record` on the 100k
run: **71.4% of cycles in `reg_remove`**, all of it after `main` returns.
Peak RSS 413 MB. (Release `tur` v0.65.1, gcc 13.3, Ubuntu 24.04.)

## Fix directions

1. **Shutdown:** drop this thread's entries from `g_reg` in one filtering
   pass (or clear it outright when `g_reg_total == g_my_reg`), then free the
   arenas -- O(R) instead of O(R^2). This alone removes the 71%.
2. **`reg_remove`:** scan from the end. Brackets are LIFO, so the arena being
   removed is almost always the last one added, and the swap-with-last
   removal keeps that true -- O(1) for the common case.
3. **`tur_region_owns`:** an address-range index over live and retired slabs
   (sorted, binary-searched) instead of a walk of every generation, so a free
   in a program with many retirements stays cheap.
4. The deeper cost is the retirements themselves: a bracket that retires
   saves nothing and keeps a slab. Where a program retires every bracket (as
   the fixtures above do), that is worth a diagnostic or a counter a harness
   can see, not just `TUR_REGION_STATS`.
