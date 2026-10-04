# A Bool `^reflect` measure stops proving at four ground list elements

> **Resolved 2026-10-04.** Unit propagation in the DNF expansion
> (`expand`, `src/compiler/refine_solver.c`); the side note's dump gap is
> fixed in `rt_return_obligation_proven` (`src/compiler/elab_fns.c`). See
> "Resolution" at the end.

**Severity:** medium -- completeness, not soundness. The obligation falls to
TUR-W0372 and keeps its runtime check (a hard error under `--strict-refine`).
Found writing `examples/reflected-measures`, where `(ranked? v)` /
`(valid? v)` on a five-judge board could not be proved while the Int
measures `size`/`points` over the same board proved.

## Repro

```turmeric
(defdata Board [] (Entry [score : int rest : Board]) (Empty))
(defn ^reflect pos? [^borrow b : Board] : bool
  (match b (Empty) true (Entry s r) (and (> s 0) (pos? r))))
(defn f [b : #refine{ v : Board | (pos? v) }] : int 1)
(defn main [] : int (println (f (Entry 1 (Entry 2 (Entry 3 (Entry 4 (Empty))))))) 0)
```

`tur --enable=reflected-measures check --dump-refine=json` -- 3 elements:
`proven`; 4 elements: `unknown`, `"caps_hit": {"cubes": 1}`. Raising
`TUR_REFLECT_FUEL` does not help; fuel is not the limit.

## Root cause

A Bool measure's equation is asserted as an `iff`, written as two
implications (plan, "`if` needs no term"), i.e. two binary clauses per
unfolding. After the ground comparisons fold away, each unfolding is
`(or (not p_k) p_{k+1}) and (or (not p_{k+1}) p_k)`. The cube expansion
distributes these into DNF, so n unfoldings cost up to 4^n cubes; four
unfoldings exceed `REFINE_MAX_CUBES` (64, `src/compiler/refine_solver.h:30`).
The VC itself is a trivial equivalence chain (visible in the dump's
`vc_smtlib`).

## Resolution

The fix went in the solver, not the encoder: the VC is fine, the expansion was
naive. `expand` now does unit propagation before it splits a disjunction. A
literal that is a top-level conjunct is in every cube the frame produces, so a
disjunction holding that literal is satisfied and dropped without a split, and
a disjunct that is its complement can only produce a `p and (not p)` cube and
is skipped (the same reasoning that already dropped a cube holding `false`).
A disjunction left with one live disjunct is split first, so an `iff` chain
resolves link by link. Terms are hash-consed, so both tests are pointer
comparisons. Soundness: both rules only discard cubes that are unsatisfiable,
or that are implied by a cube still checked.

This is general, not reflection-specific. Over the 150 `refine-*` /
`reflect-*` fixtures (happy and `errors/`), the cube peak sum went 395 -> 212
and the max peak 64 -> 8; no fixture's cube or EUF-term peak grew except the
new `errors/reflect-bool-long-refuted`, which previously hit the cube cap
before EUF ran.

A Bool measure now proves until fuel runs out (default 8 unfoldings, the
documented `TUR_REFLECT_FUEL` budget; `TUR-W0385` past it), the same limit as
an Int measure. Fixtures: `reflect-bool-measure-long` (six elements, two Bool
measures) and its refuting twin `errors/reflect-bool-long-refuted`, which pins
that the pruning never drops the satisfiable cube (still `TUR-E0371` with a
model). `examples/reflected-measures` uses five-judge boards throughout again.

The side note: a return obligation proved by the RT4 per-path split now
records a pre-discharged obligation (`decided_by: "RT4 (per-path split)"`),
so `--dump-refine=json` lists it. It is marked discharged so
`refine_discharge_all` skips it and the stats are not counted twice.
