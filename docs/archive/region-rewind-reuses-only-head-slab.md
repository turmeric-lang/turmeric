# A rewound region reused only its head slab, so regions in a loop grew without bound

**RESOLVED 2026-10-05**, found and fixed together while writing
`docs/guides/memory-usage-guide.md`. Severity was medium: it defeated the
main use of `with-region`.

## What happened

`tur_region_pop_checked` rewinds a generation with `arena_reset` and pools
the arena for the next `tur_region_push`. `arena_reset` kept every slab, but
`arena_alloc_aligned` (`src/runtime/arena.c`) only ever tried `a->head`; when
the head filled it malloc'd a fresh slab and prepended it. After a reset the
head is one emptied slab, so the next generation refilled that one and
malloc'd all the others again. The emptied ones stayed on the chain and were
never used.

Measured with a 10000-cell `:heap` list built in a `with-region` per round
(every round rewound, `TUR_REGION_STATS=1`): peak heap 267 KB at 1 round,
**9.9 MB at 50 rounds**, about what the same program uses with no region.

## Fix

`Arena` gained a `spare` list. `arena_reset` keeps the head slab on the
chain and parks the rest on `spare`; the allocator takes a spare slab (when
it is big enough) before calling malloc. `arena_free` and `arena_owns` cover
both lists. After the fix the 50-round program peaks at 267 KB, the same as
1 round.

Pinned by `tests/region_unit.c` check (i): fill five slabs, reset, fill
again; every address must fall in a slab the first fill used. It fails
before the fix. The arena source is embedded in emitted programs, so the
fixture snapshots moved by the same 51 lines each.
