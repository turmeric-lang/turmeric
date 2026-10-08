---
title: Solver Integer Tail Plan (S2c-lite)
category: Planning
description: Where the refinement solver's limits actually are once the numeric caps are measured (nowhere near), what shapes ordinary code writes that the solver could not decide, and the first slice of the S2c integer tail plus the encoder axioms that close them.
---

# Solver Integer Tail (S2c-lite)

**Status:** **complete** -- Phase 1 **LANDED 2026-09-05** (commit dee5563f,
v0.44.0; this document was written with it).  Phase 3(b) (`tur smt`
`div`/`mod` with their Euclidean meaning, both directions) and Phase 4 (both
halves: the fuzzer's `shape_integer` population and the six `QF_LIA`
div/mod corpus benchmarks) landed the same day and shipped in v0.44.2.
Phases 2 (sigma-substitution) and 3(a) (dark shadow / branch-and-bound) are
**parked by decision** on measured evidence -- the same disposition as SX4
/ SX6 in [solver-extension-plan.md](../archive/solver-extension-plan.md) --
with their triggers instrumented (`eq no-unit split` and `LA int feasible`
under `TUR_REFINE_STATS=1`, carried as rows in
`benchmarks/cap-sweep-results.md`, plus `benchmarks/run-unknown-oracle.py`,
which found zero missed proofs in any swept population).  Neither has fired:
the 2026-09-06 sweep's only Phase 2 hit is a `sat`-labelled corpus benchmark,
and no solver-arithmetic change has landed since.  Re-open from those rows,
not from this file.  Nothing here was on the critical path to v1; every item
is additive to a solver that already ships and is sound, and every step below
is an equivalence over the integers, so the one-directional invariant (never
`RT_VALID` unless entailed) is preserved by construction.  Archived
2026-09-28.

Companion to [solver-extension-plan.md](../archive/solver-extension-plan.md) (SX), which
this plan does not replace: SX is about *incrementality and boolean
structure*; this is about *what the arithmetic stage can decide at all*.  SX7
("integer completeness, the long tail") is the SX phase this work belongs
under, and SX7's own note -- prototype in Turmeric first, port to C if the
stats move -- was not followed here, deliberately: the two steps that landed
are ~150 lines of exact int64 arithmetic in the existing C stage, cheaper to
write than to prototype.

## 1. The question, and the measurement that reframed it

The ask was to "expand the limits of the solver's constraints, and possibly
rebuild part of the solver infrastructure."  The word *limits* has two
readings in this solver, and they came apart on measurement.

### 1.1 The numeric caps do not bite

`refine_solver.h` carries nine caps.  `benchmarks/run-cap-sweep.sh` as it
stood before this work (the 2026-09-05 03:21 run of
`benchmarks/cap-sweep-results.md`, byte-identical to the 2026-08-26 baseline
except the header) across 124 corpus benchmarks, 85 in-tree refinement
fixtures and 200 fuzzer programs:

| cap | limit | worst peak anywhere | headroom |
|---|---:|---:|---:|
| `REFINE_MAX_CUBES` | 64 | 40 (a generated benchmark; 4 in real code) | 38% / 94% |
| `REFINE_MAX_CUBE_LITS` | 64 | 13 | 80% |
| `REFINE_MAX_EXPAND_DEPTH` | 256 | 8 | 97% |
| `REFINE_MAX_LA_VARS` | 32 | 9 | 72% |
| `REFINE_MAX_LA_CONSTR` | 512 | 42 | 92% |
| `REFINE_MAX_EUF_TERMS` | 512 | 512, one deliberate 1000-deep stress unit | -- |
| `NO_MAX_SHARED` | 16 | 9 | 44% |
| `RT_CS_PATH_MAX_HYPS` | 8 | 5 | 38% |

Zero hits on any cap on any real obligation.  Raising a number here buys
nothing measurable, and SX0(b) already parked the two phases (SX4 simplex,
SX6 lazy SMT) that a cap hit would have justified.

Regenerated after Phase 1 landed, the file moves in two places only: the
`model_vars` limit column reads 8, and the `la_constr` peaks DROP (corpus 42
-> 10, fuzzer 9 -> 6) because an equality now occupies one constraint rather
than two and is eliminated before Fourier-Motzkin can grow through it.  The
new `refine-int-divmod-by-literal` fixture becomes the in-tree worst unit for
cubes (8 of 64) and cube literals (11 of 64) -- the disjunctive sign axiom
doing exactly what 2.2 says it does, well inside the caps.

### 1.2 The corpus and the tree are already fully covered

- `tests/corpus/smtlib/`: **68 of 68** in-fragment `unsat` benchmarks are
  proved; the one `unsat` answered `unknown` is the deliberately
  out-of-fragment `define-sort` file.  There is no lost proof in the corpus
  for a stronger stage to find.
- In-tree: 89 refinement/GADT fixtures, **131 obligations, 115 proven, 16
  unknown -- and all 16 are fixtures that exist to pin an Unknown**
  (`refine-let-shadow-not-split`, `refine-nonlinear-warn`,
  `refine-match-field-wrong`, ...).
- Outside `tests/`, `#refine{` appears in exactly one file: the named
  refinements in `stdlib/refine.tur`.  There is no user population yet whose
  Unknowns could be counted.

So neither instrument the SX plan gates on could have shown a hole, and
neither did.  That is not the same as there being no hole.

### 1.3 Ordinary shapes the solver could not decide

Nine probes, each an ordinary function a user would write on day one.
Before / after this plan's Phase 1:

| probe | shape | before | after |
|---|---|---|---|
| p1 | `(= (mod n 2) 0) |- (= (mod (+ n 2) 2) 0)` | unknown | **proven** |
| p2 | `n >= 0 |- 0 <= (/ n 2) <= n` | unknown | **proven** |
| p3 | `2v = 2x + 1 |- false` (parity) | unknown | **proven** |
| p4a | 3-param function, false return refinement | refuted | refuted |
| p4b | **4-param** function, same false refinement | **unknown, silent** | **refuted, with witness** |
| p5 | `|- (>= (* x x) 0)` | unknown (`TUR-W0373`) | **proven** (still warns) |
| p7 | two disequalities on a bounded int | proven | proven |
| p8 | `abs` via `if` | proven | proven |
| p9 | `clamp` with three params | proven | proven |

Three of the four failures are one thing: **the solver treated `int` as
`real`**.  Fourier-Motzkin works over the rationals; `(mod n 2)` and
`(/ n 2)` were opaque terms; `2v = 2x + 1` has the rational solution
`v = x + 1/2`.  The grammar in the refinement-types guide has listed
`(/ e <literal>)` and `(mod e <literal>)` as legal predicate syntax the whole
time, so p1 and p2 are the feature as documented not working.  The fourth
(p4b) is the counterexample cap `MODEL_MAX_VARS = 3`, which the 2026-09-04
instrumentation commit had already flagged as "the cap most likely to
surprise" -- and whose sweep read clean only because the fuzzer generates at
most two parameters.

**Why the three swept populations were blind to all of it:** the fuzzer
(`tests/refine-fuzz-src.py`) generates `+ - *` by a constant and comparisons;
it never emits `/`, `mod`, a square, or more than two parameters.  The corpus
has no `div`/`mod` benchmark at all (`grep -l '(mod ' tests/corpus/smtlib
-r` finds nothing).  The in-tree fixtures were written against the solver
that existed.  A measurement can only report on the shapes its population
contains -- which is Phase 4 below.

## 2. Phase 1 -- LANDED 2026-09-05

Four changes, all in the existing stages, no new stage, no flag (every step
is sound by construction and the runtime check is unaffected either way, so
there is nothing to gate).

### 2.1 S2: the Omega test's two exact integer steps (`refine_solver_arith.c`)

- **Integer normalization** (`int_normalize`).  For a constraint whose every
  variable is int-sorted: clear the coefficient denominators, then
  `sum a_i x_i < b  ==>  <= ceil(b) - 1`, `<= b  ==>  <= floor(b)`, then
  divide by `g = gcd(a_i)` and floor the bound again.  Each step is an
  equivalence over the integers, so the constraint keeps exactly its integer
  models and the rational relaxation FM runs on gets tighter.  It subsumes
  the strict-tightening the file always had (`e < 0 -> e <= -1`) and adds
  the gcd step (`2q >= -1 -> q >= 0`).  Applied at assertion time and to
  every constraint FM derives.
- **Equality elimination** (`eq_eliminate`).  Equalities are now kept as
  equalities in `LinC` (`is_eq`) rather than split into two inequalities at
  assertion time.  Before FM, each one gets the **gcd divisibility test**
  (`sum a_i x_i = b` with `g` not dividing `b` has no integer solution -> the
  cube is unsat; this is parity), and then is **substituted away** through
  any unit-coefficient variable, so the constraints it lands in re-normalize
  and the test re-runs on what the substitution produced.  Mixed real/int
  equations substitute through any pivot (exact Gaussian step, no integer
  claim).  An all-integer equation with no unit coefficient (`2x + 3y = 1`)
  falls back to the two-inequality reading -- exactly as complete as before
  this phase existed.

`la_unsat` still works on a private copy, so `la_entails_eq`'s
truncate-and-retry discipline and the S3 exchange are unchanged.  A failed
gcd test leaves a canonical `0 < 0` in the set rather than a flag, so the
same truncation restores it.

### 2.2 Encoder: `(/ a k)` and `(mod a k)` axiomatized (`refine_collect.c`)

For an int-sorted `a` and a nonzero integer literal `k`, `enc_divmod_axioms`
adds two hypotheses about the (still opaque) terms `q = (/ a k)` and
`r = (mod a k)`:

```
a = k*q + r
(0 <= a  and  0 <= r <= |k|-1)  or  (a < 0  and  -(|k|-1) <= r <= 0)
```

These are the **truncating** semantics of C's `/` and `%`, which is what `/`
and `mod` on ints compile to (`builtins.c` maps `mod` to `%`; the
interpreter's `eval.c` uses `%`), so the axioms describe exactly the value
the runtime check would observe.  They are not SMT-LIB's floor/Euclidean
`div`/`mod`: the SMT-LIB reader behind `tur smt` does not go through this
encoder and receives no axioms, which keeps it sound for its own semantics
(the mismatch is pre-existing and is Phase 3(b) below).

The sign clause is a disjunction, so each axiomatized term doubles the cube
count.  After `ENC_MAX_DIVMOD_SPLITS = 4` terms in one VC the weaker
conjunctive bound `-(|k|-1) <= r <= |k|-1` is asserted instead, so an
obligation that used to prove *without* knowing anything about its `mod`
terms cannot be pushed over `REFINE_MAX_CUBES` by learning about them.

### 2.3 Encoder: squares are non-negative

`enc_nonlinear` still abstracts `(* a b)` to an uninterpreted symbol and
still flags `TUR-W0373`, but when `a == b` (structural equality, so
`(* (- x 1) (- x 1))` counts) it also asserts `(<= 0 (* a a))`.  A sign fact
about an opaque term, true for every integer and every real.  Only products:
`x / x` and `x mod x` share the abstraction path and have no such law.

### 2.4 Counterexample search: budget by evaluations, not width

`MODEL_MAX_VARS` 3 -> 8 (now only an array-size backstop) and a new
`MODEL_MAX_EVALS = 131072` on `n_cand ** n_vars`, which is the cost the
odometer actually pays.  A five-variable VC over five candidates (3125
evaluations) is cheaper than a three-variable one over sixteen (4096); the
old cap had that backwards.  Budget declines are counted on their own row
(`model evals out`) under `TUR_REFINE_STATS=1`; the `model vars` /
`model vars run` rows keep their meaning for the width backstop.

**Effect on the corpus:** 8 more `sat` benchmarks (16 of 56, up from 8) now
report a model instead of `unknown`; no `unsat` verdict moved.  On the fuzzer
the count of "gate-off ran clean, gate-on rejected at compile time" cases
(report-only by design, since `TUR-E0371` is universally quantified while
the program exercises one input) is 1 at the checked-in seed, and that one is
a correct refutation (`(* p 2) > 0` with `p >= 0`, witness `p = 0`).

### 2.5 Acceptance, as run

- `tests/unit/refine_solver.c`: 76 checks (was 61), including the
  soundness direction for every new step -- `2v = 2x + 1` over the reals is
  NOT refuted; `2x + 3y = 1` (integer-feasible) is NOT refuted;
  `2q >= -1` does NOT give `q >= 0` over the reals; the six-variable /
  eight-candidate VC declines on the budget row and not the width row.
- Corpus: 125 verdicts, the 8 `unknown -> sat` moves above and nothing else.
- `tests/run-refine-fuzz-src.sh`: 0 soundness bugs, 0 other bug classes.
- In-tree refinement fixtures: verdict counts identical before and after
  (115 proven / 16 unknown, the same 16).
- New fixtures: `refine-int-divmod-by-literal`, `refine-int-parity`,
  `refine-square-nonneg` (each with `--strict-refine` in `flags`, so an
  Unknown is a build failure and the fixture pins PROVEN, not merely
  "ran"), `errors/refine-model-search-four-vars`, and
  `refine-model-vars-cap` re-pointed at the new limits with a second subject
  for the budget row.
- Full `bash tests/run.sh`: 2810 passed, 0 failed (commit dee5563f); refine ctests pass; `benchmarks/run-cap-sweep.sh` regenerated against this compiler -- no cap moved except the `model_vars` limit column and the constraint count an equality now occupies.

## 3. Phase 2 -- the rest of the Omega equality phase (open, instrumented)

**Trigger:** an obligation whose equation has no unit coefficient shows up
outside a test -- the telemetry to add first is a counter on the
`eq_eliminate` fallback branch (how many equalities were split rather than
eliminated), reported under `TUR_REFINE_STATS=1`.

**Instrumented and measured 2026-09-05.**  `eq no-unit split` under
`TUR_REFINE_STATS=1` counts, at OBLIGATION level, obligations the whole
chain (model search included) left unknown after such a split (the raw
per-call count is printed beside it; it is inflated by every stage, probe
and hint that re-decides the same VC -- a three-obligation probe read 14).
The cap sweep carries it as a row for all three populations
(`benchmarks/cap-sweep-results.md`):

| population | unknown obligations after a no-unit split |
|---|---:|
| SMT-LIB corpus (130 units) | 1 -- `gen_mixed_sat_00054`, labelled `sat`: unknown is the right answer |
| in-tree fixtures (88) | 0 |
| fuzzer (200 programs) | 0 |

So the shape exists only in one generated corpus benchmark that has a
model, where eliminating the equation could not produce a proof.  Parked,
with the instrument in place; the row is what to watch.

- **Pugh's sigma-substitution** for `sum a_i x_i = b` with all `|a_i| > 1`:
  pick the smallest `|a_k|`, `m = |a_k| + 1`, introduce `sigma` with
  `a_k x_k = -m sigma + sum (a_i mod-hat m) x_i + ...`, which lowers the
  coefficient magnitudes each round and terminates.  Costs one fresh LA
  variable per round against `REFINE_MAX_LA_VARS` (peak 9 of 32).
- **Size:** ~60 lines in `eq_eliminate`.  Unit tests: `2x + 3y = 1`
  becomes eliminable (still satisfiable -- the test is that the pivot step
  does not manufacture a refutation), and `6x + 10y = 3` (gcd 2, already
  caught) stays caught.

## 4. Phase 3 -- inequality-side integer completeness (open)

Systems that are rationally feasible and integer-infeasible with no equation
in sight: `2x >= 1, 2x <= 1` normalizes to `x >= 1, x <= 0` and IS caught by
Phase 1, but `3x + 3y >= 1, 3x + 3y <= 2` is not (both normalize to bounds on
`x + y` that leave `[1/3, 2/3]`, then floor/ceil to `x + y >= 1, <= 0` -- also
caught; the real gap is multi-variable systems where the integer hull is not
reached by per-constraint rounding).

- **(a) Dark shadow / branch-and-bound**, depth-limited, `RT_UNKNOWN` past
  the limit.  This is what the archived plan and SX7 call S2c proper.
  **Trigger:** a real obligation, not a constructed one.  The probe set in
  1.3 did not produce one; ordinary bounds reasoning does not need it.

  **Instrumented and measured 2026-09-05, two ways.**  First the
  population: `LA int feasible` under `TUR_REFINE_STATS=1` counts obligations
  left unknown after S2 declined a cube that is all-integer, has a constraint
  over two or more variables, and whose RATIONAL relaxation Fourier-Motzkin
  found feasible -- exactly the sets the dark shadow would look at.  Corpus
  23 units (all `sat`-labelled, where unknown is correct), fixtures 5, fuzzer
  17.  That is an upper bound only: most such sets have integer models too.

  Then the payoff, which is the number that matters.
  `benchmarks/run-unknown-oracle.py` hands EVERY obligation the compiler
  left unknown -- its `vc_smtlib` from `--dump-refine=json` -- to Z3 and
  counts the ones Z3 calls `unsat`, i.e. proofs the in-house chain missed.
  Reading: **283 unknown obligations (15 fixture, 268 fuzzer), 283 `sat`,
  0 `unsat`.**  Together with the corpus, where all 72 in-fragment `unsat`
  benchmarks are already proved, there is not one obligation in any swept
  population that branch-and-bound, the dark shadow, or any complete
  integer procedure could turn from unknown into proven.  Parked on that
  evidence, with both instruments in place.

  Running the oracle found a defect it was not looking for: a third of the
  dumped VCs would not parse in Z3.  The serializer wrote uninterpreted
  names verbatim -- `tickm#0` (read as a malformed bit-vector literal),
  `match`, `_`, `if` (reserved words, which SMT-LIB makes the same symbol
  even quoted) -- declared uniform `Int`/`Real` parameter sorts for an
  abstracted form that takes a Bool argument, and named `QF_UFLRA` for a
  VC that also carries ints.  `tur smt`'s own reader took all of it, which
  is why the external-replay promise of SX8a was never actually kept for
  those obligations.  Fixed in `refine_smtlib_emit`: non-simple names are
  quoted, reserved names are renamed (`|match~rw|`), parameter sorts are
  read off a real application, and a mixed VC says `QF_UFLIRA`.  The
  oracle now exits non-zero on an unparsed VC, so the promise stays kept.
- ~~**(b) `tur smt` `div`/`mod` semantics.**  The SMT-LIB reader translates
  `div`/`mod` to `VC_DIV`/`VC_MOD`, whose constant folding and model
  evaluation use C truncation, while SMT-LIB specifies floor division and a
  non-negative remainder.  No corpus benchmark uses either today.  Either
  translate to the truncating form with an explicit sign adjustment, or
  refuse `div`/`mod` in the reader as out-of-fragment until then.  Small,
  and worth doing before anyone adds a `QF_LIA` benchmark with `mod`.~~
  **LANDED 2026-09-05, the first way, in both directions.**  (SMT-LIB's
  `div`/`mod` are *Euclidean* -- `0 <= mod < |n|`, the quotient absorbing
  the divisor's sign -- which for a positive divisor is floor division; the
  paragraph above said "floor" and that was the imprecise half of it.)

  Measured first: `(< (mod x 3) 0)`, unsatisfiable in SMT-LIB, came back
  `sat` with the model `x = -2` -- the bounded search evaluating `%`.  The
  reader now BUILDS the Euclidean value from the truncating pair the VC
  already has, `r_t = VC_MOD(a, k)`, `q_t = VC_DIV(a, k)`:

  ```
  mod_E = ite(r_t < 0,  r_t + |k|,     r_t)
  div_E = ite(r_t < 0,  q_t - sgn(k),  q_t)
  ```

  lifted through the same fresh-variable `ite` the reader already uses,
  with the truncating axioms asserted for `(a, k)` -- `enc_divmod_axioms`
  moved onto the VC as `vc_add_divmod_axioms` so the encoder and the
  reader assert one set, not two copies.  So `tur smt` does not merely
  stop answering wrong; it decides these: `(< (mod x 3) 0)` is proved
  unsat by S2, and `(= (mod x 2) 0), (= (mod (+ x 1) 2) 0)` too.  A
  literal dividend folds outright.  A non-literal divisor (nonlinear, no
  axiom) and division by zero (uninterpreted in SMT-LIB) are refused whole
  as outside the fragment -- before this the reader accepted `(div x y)`,
  left it opaque to the stages, and let the model search evaluate it with
  C semantics.

  The other direction had the same defect: the serializer (`vc_smtlib` in
  `--dump-refine=json`, `TUR_REFINE_DUMP`) emitted the compiler's
  truncating `(/ a k)` as a bare SMT-LIB `(div a k)`, mis-stating every
  negative-dividend obligation to an external solver.  It now spells the
  truncating value in Euclidean terms
  (`(ite (>= a 0) (div a k) (- (div (- a) k)))`, likewise for `mod`), so a
  dumped VC replays faithfully -- pinned by feeding a dumped `(/ n 2)`
  obligation straight back into `tur smt`, which proves it.

  Two tests grew teeth with it.  `tur_refine_corpus` now runs the bounded
  model search after the chain, exactly as `tur smt` does (it replayed a
  SUBSET of the solver before, so a wrong model was invisible to it), and
  counts a MODEL on an `unsat`-labelled benchmark as a failure (before,
  only VALID-on-`sat` was; a witness for a contradictory set is a
  reader/evaluator bug, not incompleteness, and it is exactly what this
  defect looked like).  Verified against the pre-fix reader with the new
  harness: 2 MODEL failures (`qf_lia_mod_nonneg_unsat`,
  `qf_lia_div_rounds_down_unsat`), and 0 with the fix; no verdict on the
  pre-existing 125 benchmarks moved.  Six hand-written `QF_LIA` benchmarks
  with `div`/`mod` are in the corpus -- which closes Phase 4's corpus half
  too.  Fixture: `tests/fixtures/sx8a-tur-smt-div-mod`, which also pins the
  `--dump-refine=json` -> `tur smt` round trip proving all four of a
  `/`+`mod` program's obligations.

  One more reader change rode along, because the round trip needed it: two
  occurrences of the SAME arithmetic `ite` now share one lifted variable
  (memoized by term identity, definitions re-added on every hit so a
  session `pop` cannot strand the variable undefined), and the serializer's
  truncating idiom is recognized on read and mapped back to the node it
  was written from plus its axioms (`tr_trunc_idiom`).  Without both, the
  six idiom occurrences in one dumped `mod` obligation minted eighteen
  variables, each with its own disjunctive definition, and the replay went
  over the cube cap.  The lift itself also got cheaper: one disjunction
  `(c and t = a) or (not c and t = b)` instead of the equivalent pair of
  implications, which the naive DNF expanded to four cube combinations
  rather than two -- `qf_lia_div_mod_identity_unsat` sat at exactly 64 of
  64 cubes on the old form and reads 40 (the pre-existing corpus peak) on
  the new one, with every corpus verdict identical.

## 5. Phase 4 -- make the instruments able to see this class

**Fuzzer half LANDED 2026-09-05.**  The corpus half stays open behind
Phase 3(b), as written below.

The blind spot in 1.3 is the actionable finding for the measurement side,
and it is cheap:

- ~~`tests/refine-fuzz-src.py`: generate `(/ e k)`, `(mod e k)`, `(* e e)`,
  and 3-5 parameters, at low weight.~~  **Done.**  A `shape_integer`
  (int mode, ~5% of programs) generates the 1.3 probes as families rather
  than instances: parity through `mod` with a refined-parameter residue
  and a body that keeps or moves it; `/` and `mod` by a literal in
  `{-4..-2, 2..4}` with the dividend's sign bounded by the parameter or
  left free, and a claim that either holds under C truncation or fails on
  one side of zero (the SMT-LIB floor reading would make the `mod`
  "remainder is non-negative" claim true; truncation makes it false for
  every negative argument, and `main`'s literals are two-signed -- that
  is the trap an encoder asserting the wrong semantics would fall into,
  as a `BUG_soundness`); squares of a parameter or an offset of it,
  including one rung the sign law cannot decide (`x*x - x >= 0`), kept
  deliberately as the abstracted-nonlinear population; and 3-5 parameter
  sums with every parameter bounded by a `:pre` (a genuine multi-variable
  proof) or unbounded (false, so the model search has to find the witness
  across all of them).  `expr()` also reaches `mod` and a square at low
  weight, so they show up inside every other shape's arithmetic.

  Validity: 60 of 60 `shape_integer` programs compile, 43 proven / 55
  refuted / 8 unknown -- all 8 the deliberately abstract square rungs.
  Differential runs, 0 bugs in every class: n=200 seed=1 mode=both (5
  suspicious, unchanged from the pre-widening baseline on the same seed;
  183 proven / 203 refuted against 175 / 188), and n=150 seed=7 mode=int
  (9 suspicious, two of them the wide rung's false refinement refuted with
  a 4- and a 5-variable witness while the program's own arguments happened
  to pass -- the report-only class doing exactly what it is for).  The
  ctest smoke size (`tests/run-refine-fuzz-src.sh`) passes.

- ~~`tests/corpus/smtlib/`: a handful of hand-written `QF_LIA` benchmarks with
  `div`/`mod` and parity, labelled by both reference solvers per the corpus
  README -- **after** Phase 3(b), or the labels will disagree with the
  reader's semantics.~~  **Done with Phase 3(b), 2026-09-05**: six
  benchmarks (`qf_lia_mod_negative_dividend_sat`, `qf_lia_mod_nonneg_unsat`,
  `qf_lia_div_rounds_down_unsat`, `qf_lia_div_mod_identity_unsat`,
  `qf_lia_div_negative_divisor_sat`, `qf_lia_parity_shift_unsat`), each
  with its reason in the file, sealed by `validate-labels.py` against BOTH
  Z3 and cvc5 (131 labels checked, 0 disagreements).  Every one is a
  discriminator: the truncating reading answers the opposite label on the
  first three.
- ~~`benchmarks/run-cap-sweep.sh`: nothing to change; it already reports the
  new `model evals out` row as a plain count, the way it reports FM blow-ups.~~
  **Wrong on both counts, and both are fixed.**  (1) The compiler printed
  the `model evals out` row but the sweep's parser dropped the line; it
  now reads it into a `model_evals (search budget)` row, counted as a cap
  hit since a budget decline leaves the obligation unknown.  (2) The sweep
  constructed the generator with mode `"both"`, which `Gen` does not take
  -- its `ty` property reads anything but `"int"` as float -- so every
  sweep before this one measured a FLOAT-ONLY fuzzer population.  That is
  why the first regeneration after the widening moved nothing: `mod`, the
  axioms and `shape_integer` are int-mode only.  The sweep now alternates
  int/float per case exactly as the fuzzer's own driver does.  Regenerated
  against this compiler, the fuzzer rows finally carry the class: peaks
  `model_vars` 3 -> 5, `cubes` 8 -> 16, `cube_lits` 5 -> 12, `la_vars`
  7 -> 9, `la_constr` 6 -> 11, `euf_terms` 23 -> 25; hits stay 0 on every
  cap, `model_evals` included, so nothing here reopens SX4 or SX6.  The
  corpus and fixture populations are byte-identical to the previous run.

## 6. Explicitly not doing

- **Not** interpreting `(* x y)`, `(/ x y)`, `(mod x y)` with both sides
  variable.  Nonlinear stays abstracted; the square axiom is the one sign
  law that costs nothing.
- **Not** removing any cap, and **not** raising the numeric caps on this
  evidence -- there is none.
- **Not** loop invariants.  A `while` accumulator is Unknown for a reason
  that has nothing to do with arithmetic; see
  [hold/loop-invariants-plan.md](loop-invariants-plan.md).
- **Not** an incremental simplex.  The S2 rebuild here is the integer
  layer *on top of* FM, behind the same `la_*` seam; SX4 stays parked on
  its own gate.

## 7. References

- Pugh, *The Omega Test: a fast and practical integer programming algorithm
  for dependence analysis* (1991) -- normalization, the equality phase with
  the mod-hat trick, dark and grey shadows.
- Dutertre, de Moura, *A Fast Linear-Arithmetic Solver for DPLL(T)* (2006)
  -- the SX4 simplex this plan does not build.
- [docs/archive/refinement-types-plan.md](../archive/refinement-types-plan.md)
  S2a-S2c; [solver-extension-plan.md](../archive/solver-extension-plan.md) SX0(b), SX7.
