# The `^reflect` fuzz population never reaches the RF3/RF4 encoder work

**RESOLVED 2026-10-03.** Fix directions 1, 2 and 4 are done. Direction 3
keeps `reflect_if` and `reflect_lie` where they are.

`tests/refine-fuzz-src.py` has a `shape_reflect`. It declares a `defdata`
list and four recursive `^reflect` measures over it with `match`: `fz-len`,
`fz-sum`, `fz-allpos?`, and `fz-sorted?`, which nests a match. Its rungs:

- ground and parameter-headed literals of depth 0..11, which cross the default
  fuel of 8 both ways;
- RF4 arm selection from a list parameter refined by a measure;
- a sabotaged sibling per rung: off-by-one goals, a zeroed head, a bumped head;
- a `^non-exhaustive` measure (RF2) and an inline-C measure (RF1's purity
  half), which must be rejected.

`--only-shape reflect` runs it at density, without random helpers or extra
targets. In mixed runs it has 3% of the slice that was `shape_random`'s, the
way `shape_loop` was carved out. Float mode uses only dyadic literals, so a
sum is exact in double and
[float-proofs-assume-exact-reals](float-proofs-assume-exact-reals.md)
does not surface as noise.

**Its first 20 cases found a soundness bug**, and it was not in the
reflection encoder. A `float` record field's selector was declared Int, so
every VC that mentioned one was contradictory and proved its goal. The unfixed
compiler gives 23 and 26 `BUG_soundness` at the two seeds below, all float
lists. Fixed in the same change and filed as
[refine-float-field-selector-declared-int](refine-float-field-selector-declared-int.md).
The reflection encoder itself came through clean.

RF3's acceptance, as the plan wrote it, measured 2026-10-03:

| run (`--n 400 --mode both`) | seed | cases | proven / refuted | SOUNDNESS | other BUG | suspicious | agree_abort | agree_clean | skip_invalid |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| `--only-shape reflect`, fixed compiler | 11 | 400 | 304 / 2 | **0** | 0 | 0 | 206 | 101 | 91 |
| `--only-shape reflect`, fixed compiler | 23 | 400 | 316 / 0 | **0** | 0 | 0 | 203 | 109 | 88 |
| `--only-shape reflect`, PR #1034 head (unfixed) | 11 | 400 | 396 / 2 | **23** | 0 | 0 | 183 | 101 | 91 |
| `--only-shape reflect`, PR #1034 head (unfixed) | 23 | 400 | 415 / 0 | **26** | 0 | 0 | 177 | 109 | 88 |
| mixed population, fixed compiler | 11 | 400 | 412 / 736 | **0** | 0 | 24 | 16 | 4 | 126 |
| mixed population, fixed compiler | 23 | 400 | 400 / 776 | **0** | 0 | 18 | 13 | 11 | 120 |

The original filing follows.


**Severity: medium (a non-optional acceptance criterion is not met, in the one
place the plan names as highest-risk).**
[reflected-measures-plan](../upcoming/reflected-measures-plan.md) RF3 states
its fuzz requirement in those words: "Source fuzzer
(`tests/refine-fuzz-src.py`) gains reflected shapes -- both prior soundness
bugs lived in the encoder, so `--n 400` at two seeds is non-optional." The
fuzzer did gain reflected shapes and the two-seed run passes. But the shapes
it gained are **scalar and non-recursive**, so they exercise the RF1
termination gate and nothing else: no recursive unfolding, no fuel, no arm
selection, no tag-fact selection. The encoder work RF3 and RF4 actually added
is unfuzzed. Filed 2026-10-02, found reviewing the `reflected-measures` row at
the v0.59.0 cut.

## The run, measured

Ran here against a v0.59.0 Debug build, both seeds the plan asks for:

```sh
python3 tests/refine-fuzz-src.py --n 400 --seed 11 --tur ./build/tur --mode both
python3 tests/refine-fuzz-src.py --n 400 --seed 23 --tur ./build/tur --mode both
```

| seed | cases | obligations decided | SOUNDNESS | other BUG | suspicious | agree_abort | rejected early | skip_invalid |
|---|---|---|---|---|---|---|---|---|
| 11 | 400 | 384 proven / 734 refuted | **0** | 0 | 23 | 17 | 230 | 125 |
| 23 | 400 | 389 proven / 783 refuted | **0** | 0 | 18 | 12 | 243 | 117 |

Clean. `agree_abort` is both legs agreeing the program violates its own
refinement at runtime -- the correct outcome, not a crash. `suspicious` is the
harness's documented report-only class (a universal refutation of a program
whose `main` happens not to pass a violating argument).

**This table is the missing record, and it should go in the plan.** But on its
own it does not discharge RF3's acceptance, for the reason below.

## What the population actually contains

`gen_helpers` (`tests/refine-fuzz-src.py:186`) picks uniformly from seven
helper kinds, two of which are reflected:

```python
kinds = ["pure_plain", "pure_fx", "pure_rec", "impure_c", "impure_c0",
         "reflect_if", "reflect_lie"]
```

- **`reflect_lie`** (`refine-fuzz-src.py:232`) is `(defn ^reflect h [a : T] : T
  (+ k (h a)))` -- non-total by construction, and required to be a hard
  `TUR-E0384`. This is real and valuable: it is the RF1 sabotage property
  running on every seed, and it is why an inconsistent equation cannot slip
  through unnoticed.
- **`reflect_if`** (`refine-fuzz-src.py:213`) is the only *admitted* measure
  the fuzzer generates:

  ```python
  "(defn ^reflect %s [a : %s] : %s\n  (if (%s a %s) %s %s))"
  ```

  `self.ty` is `int` or `float` (`--mode`), and the generator's own comment
  says why this shape was chosen: "Non-recursive with an `if`, so it unfolds
  at ANY argument (no arm to select)."

That comment is an accurate description of the coverage hole. Taking RF3/RF4's
own feature list against it:

| Mechanism | Where | Fuzzed? |
|---|---|---|
| RF1 termination gate | `rf_classify` | **yes** (`reflect_lie`) |
| RF1 purity gate | `rf_classify` | no -- `reflect_if` bodies are generated with `pure_only=True`, so they only ever call pure helpers |
| RF2 coverage rejections | same walk | no -- `reflect_if` contains no `match` at all |
| RF3 recursive unfolding, fuel 8, `TUR_REFLECT_FUEL`, fuel-exhaustion warning | `rf_unfold` / `rf_reduce` (`refine_collect.c`) | no -- the measure is never recursive |
| RF3 arm selection at a constructor-headed argument | `rf_match_pat` | no -- no ADT is ever the argument |
| RF4 non-ground arm selection from a tag fact | `rf_match_pat` / `rf_canon` | no |
| RF6.1 model search over measure applications | `model_collect_defs` / `model_def_of` | only at a non-recursive scalar measure |

So the two prior soundness bugs' neighbourhood -- the reduction loop and the
arm selector -- has fixture coverage (`reflect-len-ground`, `reflect-len-depth`,
`reflect-nonground-arm`, `reflect-fuel-exhausted` and their `errors/` siblings)
but no differential fuzz coverage. Fixtures test the shapes someone thought to
write; the fuzzer is what the plan reached for precisely because that is not
enough here.

## Why it came out this way

Compare the sibling feature. `loop-invariants` got a **dedicated shape** --
`shape_loop` (`refine-fuzz-src.py:956`) -- plus `--only-shape loop`, so it can
be fuzzed in isolation at full density, which is how its LI5 table reached 400
cases of actual loop programs per seed and found 12 soundness bugs (all one
root cause, since fixed).

`^reflect` got helper *kinds* instead of a shape. There is no
`--only-shape reflect`, so reflect density in any run is whatever the
seven-way lottery yields, diluted further by `agree_rejected_early` taking
~58% of all generated programs. There is no way to run the reflect population
at density today, which is likely why no reflect-specific table was ever
recorded in the plan.

## Fix directions

1. **Add `shape_reflect`, mirroring `shape_loop`.** It needs what `reflect_if`
   lacks: a `defdata` list or tree, a recursive measure over it with a `match`
   (the `len` / `all-pos?` / `sorted?` family the fixtures already use), a
   predicate that calls it at a constructor-headed literal of varying depth
   (to cross the fuel boundary in both directions), and a sabotaged sibling
   whose measure does not compute what its equation claims -- that last one is
   what turns a bad unfolding into a `BUG_soundness` rather than a silent
   agreement. Register it in `--only-shape` and carve it out of
   `shape_random`'s slice the way `shape_loop` was
   (`refine-fuzz-src.py:1050`).
2. **Then run `--n 400` at two seeds on `--only-shape reflect`** and put the
   table in the plan next to the one above, which is RF3's acceptance
   discharged as written.
3. **Keep `reflect_if` / `reflect_lie` where they are.** They do a different
   job -- they put reflected helpers into *other* shapes' programs, which is
   how the purity memo bug (`RtPureCtx::leaned_missing`) would surface. The
   new shape is additive.
4. **While there:** have the new shape generate a non-exhaustive `match` and an
   impure body so RF2 and the RF1 purity half are covered too. Both must be
   rejections, so they land as `skip_invalid` -- cheap to generate, and the
   gate is only load-bearing if something checks it keeps firing.

## Note on the graduation question

This is the one concrete piece of work RF3 asked for and did not get, so it is
worth doing whichever way the expiry review goes -- the row's `expires_at` is
`0.61.0`, so there is no deadline pressure. It is also the item that would
most change confidence in graduating: the feature's correctness argument rests
on the encoder, and the encoder is the part with no differential coverage.
