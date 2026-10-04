# A Bool `^reflect` measure stops proving at four ground list elements

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

## Fix directions

- At a **ground** argument the reduced body contains no free variables: fold
  the unfolding chain to a constant before asserting, or assert only the
  direction the goal's polarity needs (positive goal -> `p_{k+1} => p_k`
  chain suffices, one clause per step, no doubling).
- Unit-propagate equivalences (`p_k <=> p_{k+1}`) as an equality class before
  cube expansion, so a chain collapses to one literal.

## Side note (separate, minor)

A return obligation proved through `rt_prove_paths` (the per-arm path, e.g.
`runner-up-ranked?` in the example, `tail-sorted?` in
`tests/fixtures/reflect-nonground-arm`) emits no record in
`--dump-refine=json` when it proves; it appears only when it fails.
