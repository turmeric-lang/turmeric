# Labelled SMT-LIB2 corpus

A standing regression for the in-house staged decision procedure (S0--S3),
replayed by `tur_refine_corpus` with **no solver linked**.

This exists because of the Z3 retirement criteria in
[docs/archive/refinement-types-plan.md](../../../docs/archive/refinement-types-plan.md).
`refine_libz3.c` was a *live* cross-check that only existed on a dev build with
a system Z3, and **it was deleted in 0.32.5** -- that safety net is gone. What
survives it is a corpus whose labels are **data in the repo**, which is what
this is. As of 0.32.5 this corpus and the source-level fuzzer
(`tests/refine-fuzz-src.py`) are the whole of the standing solver-soundness
check, so a soundness failure here is no longer backed up by anything else.

## What is checked

SMT-LIB `:status` is a claim about the SATISFIABILITY of the assertion set. The
chain decides ENTAILMENT. The two line up by taking every `(assert phi)` as a
hypothesis and `false` as the goal:

```
hyps |- false   is VALID   iff   hyps is UNSAT
```

which gives the check its shape:

| `:status` | chain answers `RT_VALID`      | chain answers `RT_INVALID` (a model) | `RT_UNKNOWN`            |
|-----------|-------------------------------|--------------------------------------|-------------------------|
| `unsat`   | correct -- it found the proof | **MODEL FAILURE**                    | acceptable (incomplete) |
| `sat`     | **SOUNDNESS FAILURE**         | correct                              | correct                 |

A `sat` benchmark answered VALID is the one-directional soundness invariant
broken: the chain claimed a contradiction in a constraint set that has a model.
That is the property the corpus defends, and checking it needs only the label --
never a live solver.

An `unsat` benchmark answered with a MODEL is the other direction, and it is
not incompleteness: only the bounded model search answers `RT_INVALID`, and
only after evaluating every assertion true under a concrete assignment. A
witness for a contradictory set means the reader or the evaluator gives some
term a meaning the script did not write. That is how SMT-LIB's Euclidean `mod`
read as C's `%` surfaced (`qf_lia_mod_nonneg_unsat` had a model, `x = -2`), and
since 2026-09-05 it counts as a failure too.

Incompleteness is never a failure. `RT_UNKNOWN` is always a safe answer, and a
stage that legitimately decides more later must not break this corpus.

## Layout

- `*.smt2` at the top level -- **hand-written**, one idea each, chosen to cover
  the fragment: QF_UF congruence (including at arity 2 and through
  transitivity), QF_IDL negative-weight cycles, QF_LIA integer-gap and scaling,
  QF_LRA strictness over the reals where the integer version is unsat,
  QF_UFLIA cases needing both theories to combine, boolean structure
  (`or`, `=>`, `distinct`, `let`), and reader/solver robustness shapes:
  integer-literal ite branches in a Real logic, an Int and a Real literal
  for one value in one congruence class (`qf_uflira_int_real_literal_sat`,
  the pair S1 once called a conflict), a 1500-deep nullary
  `define-fun` chain, and a 1000-deep arithmetic term (the shape that used
  to overflow `linearize`).
- `generated/` -- **machine-generated**, labelled by Z3, curated to 10 per
  (theory, status) bucket so the set stays balanced and reviewable.
- `unsupported_define_sort_skip.smt2` -- deliberately OUTSIDE the fragment
  (`define-sort` is an unsupported command). The reader must skip it whole
  rather than guess a translation; its presence keeps that path exercised, so
  a reader that silently mis-parsed would not report a pass for work it did
  not do. This role used to belong to `unsupported_ite_skip.smt2`, until ite
  lifting brought ite inside the fragment -- that file lives on as
  `qf_lia_ite_lifted_unsat.smt2`, pinning the lifting instead.

## Provenance of the labels

Every label agrees with **both Z3 and cvc5**. Two scripts, both **development
scaffolding** in exactly the way `refine_libz3.c` was -- neither is built,
linked, or run by the test suite, and `tur_refine_corpus` does not import them.
That separation is why they SURVIVE the oracle's deletion: they run at
authoring time from a pip install, and labelling a *new* benchmark still needs
a reference solver even though nothing links one any more:

```sh
pip install z3-solver          # required
pip install cvc5               # optional, recommended
python3 tests/corpus/validate-labels.py          # every label vs both
python3 tests/corpus/generate-corpus.py --out DIR --n 2000 --seed 7
```

Two solvers rather than one, because **Z3 was the thing being retired** (and
now has been). A label confirmed only by Z3 inherits whatever Z3 gets wrong,
and this corpus exists precisely so the in-house chain can be trusted now that
Z3 is gone -- which makes the two-solver seal more important after the
retirement, not less. cvc5 is a
different implementation lineage, so agreement between them is meaningfully
stronger than either alone. cvc5 is optional; without it the script still
checks Z3 and says which seal each label carries (`z3+cvc5` or `z3 only`).

`validate-labels.py` is the one that matters for review: it re-checks the
committed labels, including the generated ones, so a rendering bug in the
generator cannot quietly install a wrong label. A wrong label is worse than no
corpus:

- a benchmark wrongly labelled `sat` that is really unsat can never fail, so it
  silently stops testing anything;
- a benchmark wrongly labelled `unsat` that is really sat inverts the check --
  the harness would demand a proof of something false and read the correct
  refusal as weakness.

## Adding benchmarks

Prefer a hand-written benchmark with a comment saying *why* the status holds;
that comment is what a reviewer checks against, and it is what makes a wrong
label catchable without running anything. Run `validate-labels.py` before
committing.

Generated benchmarks are for breadth, and for the soak: generate thousands into
a scratch directory and replay them looking for a soundness failure. Only a
bounded, curated subset belongs in the repo -- the point is a regression net,
not a benchmark farm.

## Importing the SMT-LIB benchmark library

A note on names, because they are easy to conflate and this repo got it wrong
once. **"SMT-LIB" is two different things:**

1. **The standard** -- a specification: the SMT-LIB 2.6 language, the theory
   declarations (`Ints`, `Reals`, `ArraysEx`, ...), and the logic declarations
   (`QF_UF`, `QF_LIA`, `QF_NRA`, ...). These are reference *documents*. The
   `(set-info :status ...)` attribute this corpus depends on is defined here.
2. **The benchmark library** -- a separate, much larger *data* artifact: the
   collection of benchmark files, per logic, each carrying a `:status` label.
   This is the thing worth importing.

**SMT-COMP is neither.** It is the annual solver competition (run alongside the
SMT workshop). It *draws* its problems from the benchmark library and publishes
results and tooling; it is not itself a benchmark distribution. An earlier
version of this file said benchmarks could be fetched "from the SMT-COMP
archives", which was wrong.

The distinction has teeth. The crates.io search below found 38 `.smt2` files in
the Rust `smtlib` crate -- and every one is a **logic declaration from the
standard**:

```
(logic QF_NRA
 :smt-lib-version 2.6
 :written-by "Cesare Tinelli"
 ...
```

No assertions, no `:status`, nothing to solve. The crate vendored the
*reference*, which is what a parser needs and what a corpus does not.

### Getting the real thing

The record metadata for **SMT-LIB release 2025 (non-incremental benchmarks)**
(Zenodo record `16740866`) is committed at
[`tests/corpus/smt-lib-benchmark-data-2025.json`](../smt-lib-benchmark-data-2025.json).
It lists 90 per-logic tarballs -- 4.89 GB in total -- with sizes, md5 checksums
and content URLs. The metadata is committed; the tarballs are not.

The release is **CC-BY-4.0**, so a sample may be redistributed inside this repo
with attribution. `import-smtlib.py` writes an `ATTRIBUTION` file alongside
whatever it imports; leave it in place.

```sh
pip install zstandard
python3 tests/corpus/import-smtlib.py --list                     # what exists
python3 tests/corpus/import-smtlib.py --logics QF_UFLIA --dry-run
python3 tests/corpus/import-smtlib.py --logics QF_UFLIA,QF_UF --sample 25
```

It downloads to a scratch cache, **md5-verifies before extracting**, takes a
deterministic sample per logic (seeded, so an import is reproducible), and
flattens each logic into its own directory. It says how many it kept out of how
many exist, and says so explicitly when it kept fewer than asked -- a silent
shortfall is the same failure mode as a silently-skipped benchmark.

Sizes for the logics in the fragment, so a sample can be scoped:

| logic | compressed | logic | compressed |
|---|---|---|---|
| `QF_UFLIA` | 18.9 MB | `QF_UF` | 54.2 MB |
| `QF_UFIDL` | 35.6 MB | `QF_RDL` | 10.0 MB |
| `QF_LRA` | 181.5 MB | `QF_UFLRA` | 162.3 MB |
| `QF_IDL` | 427.4 MB | `QF_LIA` | 687.5 MB |

After importing, run `validate-labels.py` over the corpus. The library's own
`:status` labels are authoritative, but re-checking is how a truncated or
mis-imported file shows up as a disagreement rather than as a mysterious
soundness failure later.

### Already done -- you probably want the clone, not the importer

**The import has been run and its output is published.** Unless you are
producing a *different* sample, do not re-run `import-smtlib.py` -- clone
[`github.com/turmeric-lang/smt-lib-benchmarks`](https://github.com/turmeric-lang/smt-lib-benchmarks)
instead. See [The external corpus](#the-external-corpus-not-vendored-here)
below. The rest of this section is for producing a new sample.

### If the record's host is blocked: mirror it

Historical note, kept because the escape hatch is still useful: `zenodo.org`
was blocked by the egress policy of the container this corpus was originally
built in (the proxy answered 403 to CONNECT). That is an environment
restriction, not a property of the data -- the library **was** obtained, from
an environment with ordinary network access, and the result is the external
repo linked above.

**Serve the tarballs from a mirror.** S3 and `raw.githubusercontent.com` are
both reachable from this environment (verified: a public S3 object fetched
200/28 KB). Upload the per-logic tarballs under their own names and point the
importer at the bucket:

```sh
python3 tests/corpus/import-smtlib.py \
    --base-url https://YOUR-BUCKET.s3.amazonaws.com/smtlib \
    --logics QF_UFLIA,QF_UF --sample 25
```

The **md5 from the committed record is still enforced**, so a mirror can only
supply the same bytes the record describes -- it cannot substitute different
data, and a truncated or tampered copy is rejected before anything is
extracted. That is tested: a mirror serving same-length, different-content
bytes fails with a checksum mismatch, imports nothing, and exits non-zero.

Package registries are also reachable and were searched rather than assumed,
since they would be a viable transport for vendored data:

| source | result |
|---|---|
| PyPI (`pysmt` wheel + sdist, `cvc5`, `sudoku-smt-solvers`, full simple-index grep) | no benchmarks |
| npm (`smtlib`, `smtlib-ext`, `smtliblib`) | no `.smt2` at all |
| crates.io (`smt2parser`, `smtlib`, `smtlib-lowlevel`, `easy-smt`) | 38 `.smt2` -- all **standard logic declarations**, not benchmarks |
| `proxy.golang.org` | reachable, no module ships a corpus |

## The external corpus (not vendored here)

A 200-benchmark sample of the real library lives in its own repository:
**<https://github.com/turmeric-lang/smt-lib-benchmarks>** (25 per logic, seed 1,
from the 2025 release). It is deliberately NOT vendored into this tree.

```sh
git clone https://github.com/turmeric-lang/smt-lib-benchmarks /tmp/smtlib-bench
TUR_CORPUS_TIMEOUT=3 ./build/tur_refine_corpus /tmp/smtlib-bench/smtlib-2025
```

Why it stays out, in order of weight:

1. **It is 99 MB for 200 files** -- the real library is not small. One QF_LIA
   benchmark in that sample is over a million lines. That does not belong in
   the history of a compiler repo.
2. **Much of it is not decided.** These are competition-grade problems; the
   in-house chain answers `RT_UNKNOWN` on many and exceeds any sane time
   budget on others. A committed regression should be things that are actually
   decided, so a change in the answer means something. The committed corpus
   here is chosen on exactly that basis.
3. **Its value is as a SWEEP, not a gate.** Running it once found three real
   defects in this harness -- no time budget, block-buffered output, and let
   caps sized for hand-written input -- none of which the committed corpus
   could ever have surfaced. That is what it is for: run it when the reader or
   the solver changes, not on every build.

The committed corpus and the external one answer different questions. This one
asks "does a known-answer benchmark still get the known answer"; that one asks
"does anything in the wild break us".

### Reader coverage of the external sample

Measured 2026-07-26, all sweeps on the same box, same clone, default 10s
budget. "After item 1" is logic-directed numeral typing (numerals in
`QF_LRA`/`QF_RDL`/`QF_UFLRA` are real-sorted, matching SMT-LIB; regression
pair `qf_lra_ite_int_numerals_{unsat,sat}.smt2`). "After item 2" adds the
depth-cap raises (let 4000 -> 6000, term 6000 -> 8000, both measured
against the actual overflow point), nullary `define-fun` memoization
(regression pair `qf_lra_macro_chain_{unsat,sat}.smt2`), the `linearize`
depth bound (regression `qf_lra_deep_arith_chain_sat.smt2`), and the
distinctive child exit codes that make sanitizer crashes count as
`CRASH!` rather than "unlabelled":

|  | before | after item 1 | after item 2 |
|---|---|---|---|
| parse | 193 | 197 | **200** |
| skipped | 7 | 3 | **0** |
| unsat, proved | 6 | 6 | 6 |
| unsat, not proved | 75 | 79 | 79 |
| sat, correctly not proved | 70 | 70 | **72** |
| over budget (10s) | 28 | 28 | 29 |
| unlabelled | 14 | 14 | 14 |
| crashes / soundness failures | 0 | 0 | 0 |

Item 1: the four QF_LRA `spider_benchmarks` skips ("ite branches disagree
on sort") all parse and land as "unsat, not proved" -- the reader's fault
became ordinary solver incompleteness. Item 2: the three depth/macro skips
parse and land "over budget", and -- the part that is real coverage, not
reclassification -- **three other CPAchecker benchmarks moved from "over
budget" to decided**: they had been burning the whole budget re-expanding
nullary macros exponentially, and finish inside it once each def is
translated exactly once. (One QF_IDL sudoku benchmark drifted ok ->
over-budget: it sits at the 10s line and moves with machine load, not with
these changes.) The over-budget and unlabelled counts are timing- and
sample-dependent; re-measure rather than compare across machines. Full
measurements and fix rationale:
[docs/archive/history/corpus-reader-tail-plan.md](../../../docs/archive/history/corpus-reader-tail-plan.md)
(archived -- the tail it tracked is closed).

## Running it

```sh
./build/tur_refine_corpus tests/corpus/smtlib     # or: ctest -R tur_refine_corpus
```
