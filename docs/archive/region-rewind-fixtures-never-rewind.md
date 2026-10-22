# The two region rewind fixtures never rewind

**RESOLVED 2026-10-21** -- see [Resolution](#resolution-2026-10-21). The
fixtures pin their `region-stats` line now.

**Severity:** medium (test coverage). Found 2026-10-14 by the performance
audit's WP2 ([wp2-runtime-hot-paths](../artifacts/perf-audit-2026-10/wp2-runtime-hot-paths.md),
R-3). Nothing is miscomputed; what is lost is the test.

`tests/fixtures/region-with-region` and `tests/fixtures/region-scope-value-survives`
exist to catch a rewind that reclaims memory still in use: they read every
value after the pop and loop 100 rounds over the pooled slabs, "so a rewind
that corrupted them would drift". Their comments say the pop rewinds
("the pop REWINDS (the static walk clears it)"; "both locks pass and the pop
rewinds"). **It never does.** Every generation retires:

```
$ tur build tests/fixtures/region-with-region/input.tur -o rw
$ TUR_REGION_STATS=1 ./rw
10
5050
3600
10
region-stats: pushes=103 rewinds=0 retires=103
```

`region-scope-value-survives` prints the same counts, and
`region-scope-void-body` rewinds 0 of 3. Both fixtures check only stdout, so
they pass -- and a regression that corrupted a rewound generation would pass
them too, because they no longer rewind one. (Regions do work where the data
stays typed: `benchmarks/bench-regions-subst` rewinds 9039 of 9039 brackets,
and 15 of the 21 programs in the tree that open a region rewind at least
once.)

## Root cause

Both fixtures build their chain through an erasing ascription and sum it in
inline C over an erased `:int`:

```turmeric
(defn build [n : int acc : int] : int
  (if (<= n 0) acc (build (- n 1) (:: (Link n acc) :int))))
(defn chain-sum [c : int] : int  ```c ... ```)
```

Both are hooked escape notes (CLAUDE.md, "Region Store Hooks": every erasing
ascription, and the implicit typed-node -> `:int` argument of an inline-C
callee). Each note flags the generation escaped, so `tur_region_pop_checked`
retires it -- the conservative answer the hooks exist to give. The fixtures
were written before those hooks and were never updated to keep their spine
typed.

## Fix directions

1. Rewrite the fixtures so the chain never crosses into an erased type: a
   typed `Link` spine built and summed in Turmeric, the shape
   `bench-regions-subst` uses (it rewinds every bracket).
2. Make the rewind part of what is asserted, not just the answer -- e.g. the
   fixture runs with `TUR_REGION_STATS=1` and pins the `region-stats` line in
   `expected.stderr` (check whether `tests/run.sh`'s per-fixture `run_env`
   can carry it), so a fixture that stops rewinding fails instead of quietly
   becoming a retire test.
3. Sweep for others: any fixture whose comment promises a rewind should show
   `rewinds > 0`; the audit's sweep found these three.

## Resolution (2026-10-21)

All three fix directions.

1. **Typed spines.** `region-with-region`, `region-scope-value-survives` and
   `region-scope-void-body` build a `(defdata Chain :heap (Link [v : int nxt
   : Chain]) (End))` with a typed `build` and sum it with a `match`, so
   nothing is erased and no escape note fires. Their answers are unchanged.
2. **The rewind is asserted.** `tests/run.sh` reads a new per-fixture
   `run.env` (`NAME=value` lines added to the compiled program's
   environment; documented in running-tests-guide). Each fixture's
   `run.env` sets `TUR_REGION_STATS=1` and its `expected.stderr` pins the
   line: `pushes=103 rewinds=103 retires=0` for the first two, `pushes=3
   rewinds=3 retires=0` for the void body. A `stderr.compiled-only` marker
   keeps `run-turi.sh` to stdout, since the interpreter has no regions.
   Swapping the old erased `region-scope-void-body` back in fails with
   `rewinds=0 retires=3`, so the pin has teeth.
3. **The sweep.** Building and running every fixture that opens a
   `with-region` / `bt-scope` with `TUR_REGION_STATS=1` found one more:
   `region-scope-escape-refused`'s control bracket ("nothing escapes, so this
   one really does rewind") summed the same erased spine as its two escape
   cases and retired (`rewinds=0 retires=4`), so a compiler that never
   rewound passed it. Its control now uses a typed spine, and it pins
   `pushes=4 rewinds=1 retires=3`. The remaining fixtures with a zero rewind
   count are ones whose comments promise a retire
   (`region-escape-via-store-future`, `region-escape-via-erasure`,
   `region-retired-index-owns`).

`tests/run-regions-seam.sh` (the `TUR_REGIONS=0` arm) compares stdout only,
so the fixtures still serve it unchanged; `run.sh`, `run-turi.sh`,
`run-jit.sh` and the seam all pass them. regions-internals-guide's measured
table and its advice on pinning the line are updated.
