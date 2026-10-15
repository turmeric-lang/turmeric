# The two region rewind fixtures never rewind

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
