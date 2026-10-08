# The last 7 skips in the SMT-LIB corpus reader

**Status:** **item 1 landed 2026-07-26; all acceptance criteria met.**
Logic-directed numeral typing in `tests/unit/refine_corpus.c`; regression
pair `qf_lra_ite_int_numerals_{unsat,sat}.smt2` sealed z3+cvc5; committed
corpus green at 122 benchmarks / 0 soundness failures. The external-sample
sweep ran before and after on the same box: skips 7 -> 3, all four
`spider_benchmarks` files parse (landing as "unsat, not proved" -- the
predicted conversion), no new crashes; measured tally recorded in the corpus
README. Item 2 **landed 2026-07-26** in the revised sequence produced by
the measured probe (see "Item 2 findings"): distinctive child exit codes
(sanitizer crashes now count as `CRASH!`, not "unlabelled" passes), a
depth-bounded `linearize` (the solver-side stack overflow that sat BELOW
the reader's caps -- sound via the existing bad-constraint drop), cap
raises 4000 -> 6000 / 6000 -> 8000 against the measured limit, and
translate-once nullary `define-fun` memoization. External sample: **200 of
200 parse, 0 skips, 0 crashes**, and three CPAchecker benchmarks that used
to burn the budget in exponential macro re-expansion now complete.
Regressions committed: `qf_lra_macro_chain_{unsat,sat}.smt2`,
`qf_lra_deep_arith_chain_sat.smt2`; the two harness defects are archived in
`docs/archive/corpus-child-crashes-silent-under-asan.md`. This plan is
**done**. Feasibility notes at the bottom.

`tests/unit/refine_corpus.c` replays labelled SMT-LIB benchmarks against the
in-house chain with no solver linked (see
[the corpus README](../../../tests/corpus/smtlib/README.md)). Against the
200-benchmark external sample it now parses 193; this plan is about the
remaining 7.

## Where the 7 are

Measured on the sample at `github.com/turmeric-lang/smt-lib-benchmarks`:

| cause | count | logic |
|---|---|---|
| `ite` branches disagree on sort | 4 | QF_LRA (`spider_benchmarks`) |
| `let` nested too deeply | 1 | QF_RDL |
| term nested too deeply | 1 | QF_RDL |
| macro expansion too deep | 1 | QF_UFLRA |

## 1. The four `ite` sort mismatches -- the interesting one

These are QF_LRA benchmarks whose `ite` branches are integer literals:

```
(ite ?v_7 2 1)
```

In a Real logic there are no Int-sorted terms; `2` and `1` denote **reals**. The
reader carries numerals as integers and only coerces at the points it knows
about -- `/`, and `ite` branches where one side is already a real literal.
`tr_as_real` rewrites a `VC_CONST_INT` node, so it cannot fix the case that
actually occurs here: a **lifted `ite` variable** declared `VS_INT` sitting
opposite a real-sorted term. A declared variable has no literal to rewrite.

Two fixes, and the cheap one is also the principled one:

- **Type numerals by the declared logic.** In a pure-Real logic (`QF_LRA`,
  `QF_RDL`, `QF_UFLRA`), `tr_numeral` produces a real literal. That is what
  SMT-LIB means in those logics, it fixes all four, and it removes a whole class
  of latent coercion bugs rather than the four instances of it. The reader
  already records `set-logic`; it just ignores it.
- **Propagate an expected sort downward** through `tr_term`. General, handles
  mixed logics like `QF_UFLIA` where both sorts genuinely coexist, and is a
  much larger change to a file whose value is that it is simple enough to audit.

**Do the first.** The second is only warranted if a mixed-sort logic later turns
up the same problem, and none has.

Note this is the same class of bug as the real-division defect the corpus caught
(`(/ 1 2)` folding to 0, which proved a satisfiable benchmark contradictory).
That one was a soundness failure; this one is a skip. The difference is luck --
which is an argument for the logic-directed fix over another point patch.

## 2. The three depth caps -- mechanical

`TR_MAX_LET_DEPTH` (4000), `TR_MAX_TERM_DEPTH` (6000) and the macro-expansion
bound each stop one benchmark. All three are the same underlying limit:
`tr_term` recurses on the **C stack**, and a stack overflow in the forked child
is reported as `CRASH!` -- counted as a failure, which is loud but wrong for
what is really a reader limit.

Options, in increasing order of effort:

- **Raise the caps and measure.** Cheapest. The caps were already raised once
  (512 -> 4000, 2000 -> 6000) with no child crashing, so there is headroom, but
  "raise until it stops failing" is how you find the stack overflow the hard
  way. If this is the chosen route, raise them **with a deliberate probe** of
  where the child actually dies, so the new cap is set below a measured limit
  rather than above an unmeasured one.
- **Give the child a bigger stack** (`setrlimit(RLIMIT_STACK)` before `fork`,
  or run the translation on a thread with an explicit stack size). Moves the
  real limit rather than the guess at it.
- **Make `tr_term` iterative.** Removes the class entirely. Also removes the
  property that makes this reader reviewable in one sitting. Hard to justify for
  three benchmarks.

### Item 2 findings (measured 2026-07-26)

The deliberate probe ran (ASan Debug build, 8 MB stack -- the configuration
the suite actually uses). The results reshape this item:

**The three files, measured.** `tempo-width-5` nests to depth **4300**
(cap 4000); `skdmxa-3x3-12` to **6857** (cap 6000) -- both just over, and
both run clean with the caps lifted: they parse, translate, and then safely
exceed the solver budget ("skip" would become "over budget", as this plan
predicted). Translation stack is measured safe to **16,000** deep let
chains under ASan. The macro benchmark is a different species entirely:
the file is **flat** (max paren depth 2) with 18,209 nullary `define-fun`s
whose reference chain is 6,187 deep and whose naive expansion is
**>= 1e18 tree nodes** -- 18,055 of the defs reference more than one other
def, so re-expanding per reference is exponential. No cap raise can ever
admit this file; the only fix that works is translating each nullary def
**once** at definition time (bodies are flat and reference only earlier
defs, so memoized translation is linear and recursion depth collapses to
the body's textual depth, ~2). Roughly 25 lines.

**Two live defects found by the probe, present under the current caps:**

1. **`linearize` overflows the stack far below the reader's term cap.**
   A pure-arithmetic chain `(+ 1 (+ 1 ...))` crashes
   `refine_solver_arith.c:179` at depth **1000** in the ASan build
   (survives 750) -- the reader's 6000 cap is ~7x above the solver's real
   capacity, so a legal, in-cap corpus file can crash the child today.
   Cause: `LinExp` (~800 B: 32 exact-rational coefficients) is passed and
   returned **by value**, several copies per frame, inflated by ASan
   redzones to multi-KB frames. Fix direction: depth-bound `linearize`
   using its existing `bad` escape (a `bad` constraint is dropped, which
   only ever weakens a refutation -- sound by construction), bound ~500
   with margin below the measured floor; ~10 lines. An iterative rewrite
   or pass-by-pointer plumbing are larger alternatives.
2. **In the ASan build, that crash is silently counted as a pass.** ASan
   aborts with exit code 1, which collides with `OUT_UNLABELLED`, so the
   forked child's stack overflow tallies as "unlabelled" -- not `CRASH!`.
   The "a crash is loud" design holds only for signal deaths; sanitizer
   deaths are laundered into a category that never fails the suite. Fix:
   move the outcome enum to distinctive values (e.g. 40..46) and treat any
   unexpected exit status as a crash; ~5 lines. This should land **first**
   -- it is what makes defect 1 (and anything like it) visible.

**Revised sequencing (landed 2026-07-26, in this order):** (1) exit-code
fix, (2) `linearize` depth bound, (3) cap raises -- `TR_MAX_LET_DEPTH`
4000 -> 6000 (covers 4300 with margin, keeps total depth under the term
cap) and `TR_MAX_TERM_DEPTH` 6000 -> 8000 (covers 6857; translation is
measured safe to 16k, and with the `linearize` bound in place the deeper
window is no longer a crash window), (4) nullary-macro memoization.
Measured yield: 3 skips -> 0 (all three "over budget", as expected), the
crash classification verified end-to-end (a 256k-deep probe file now
reports `CRASH!` where it used to tally "unlabelled"), and one thing the
prediction missed on the good side: **three other CPAchecker benchmarks
converted from "over budget" to decided**, because their budget had been
going to exponential macro re-expansion rather than solving.

## Why this is optional

**The bottleneck is no longer the reader.** Of the 200 benchmarks, 193 parse and
189 carry a `:status`; 142 are decided and **40 exceed the time budget**.
Landing all seven of these would move parsing from 193 to 200 and decided from
142 to at most 149 -- and probably fewer, since the benchmarks that defeat the
reader are the large ones, which are exactly the ones that then time out in the
solver.

So the realistic yield is **a handful of benchmarks, several of which will
convert from "skipped" to "over budget"** rather than to "decided". That is not
nothing -- a skip and a timeout are different failures and the first is the
reader's fault -- but it is not coverage.

The case for doing it anyway is (1) the sort-mismatch fix is principled and
closes a bug class rather than four instances, and (2) skips are the category a
reader can actually fix, so leaving them is leaving the one thing under our
control. The case against is that solver throughput now dominates and effort
spent here does not touch it.

**Recommendation: do item 1, skip item 2.** The logic-directed numeral typing is
worth landing on its own merits. The three depth caps are one benchmark each,
and the honest fix for them is work out of proportion to the return.

## Acceptance criteria

- Numerals in `QF_LRA` / `QF_RDL` / `QF_UFLRA` are real-sorted; the four
  `spider_benchmarks` files parse.
- A committed regression pinning the shape -- an `ite` whose branches are
  integer literals in a Real logic -- with its label confirmed by both z3 and
  cvc5, as every corpus label is.
- No regression in the committed corpus, and no new `CRASH!` in the external
  sample.
- The skip tally in the corpus README updated with real numbers, not projected
  ones.

## Feasibility notes (2026-07-26)

A feasibility pass validated item 1 end-to-end with a throwaway spike (not
committed; diff below). Findings, in decreasing order of weight:

### Item 1 is a ~15-line change, validated end-to-end

The spike: a `reals_only` flag on `Tr`, set from `(set-logic ...)` against
`{QF_LRA, QF_RDL, QF_UFLRA}`, read at the one numeral site in `tr_term`.
Results:

- **The repro shape skips before, parses after.** Two probe benchmarks in the
  spider shape -- an inner `(ite c 2 1)` lifting to a `VS_INT` variable that
  sits opposite a real term in an outer `ite` -- both skip with exactly the
  reported reason (`ite branches disagree on sort`) on the unmodified reader.
  With the spike, both parse; the `unsat` one is **proved** (not merely
  parsed) and the `sat` twin is correctly not proved.
- **No committed-corpus regression.** `tur_refine_corpus tests/corpus/smtlib`
  is bit-identical before and after: 119 benchmarks, 66 proved, 53 sat-ok,
  0 skips, PASS.
- **The label-provenance process works in a fresh container.** `pip install
  z3-solver cvc5` both succeed; `tests/corpus/validate-labels.py` seals both
  probe labels `z3+cvc5`. So the acceptance criterion "confirmed by both z3
  and cvc5" costs nothing beyond running the existing script.
- **Solver-side support already exists.** `refine_solver_arith.c` handles
  `VC_CONST_REAL` everywhere it handles `VC_CONST_INT`, and `rat_of_double`
  converts integer-valued doubles **exactly** up to `1e15` (denominator 1 is
  tried first). Above `1e15` the constant goes `bad` and its constraint is
  dropped -- sound (can only weaken a refutation), so the worst case for a
  huge-constant LRA benchmark is proved -> unproved, never unsound. Numerals
  that large also cannot round: doubles are exact below 2^53 ~ 9e15.

One correction to the plan text: the reader does **not** "already record"
`set-logic` -- line ~642 discards it in the ignored-commands list. The fix
splits it out and captures the atom; that is 8 of the 15 lines.

The spike diff (against `tests/unit/refine_corpus.c`):

```diff
@@ Tr struct
     uint32_t   n_lets, cap_lets;
     uint32_t   n_ite;                /* serial for fresh ite-lifting variables */
+    bool       reals_only;           /* pure-Real logic: numerals denote reals */
 } Tr;
@@ tr_term atom case
         int64_t iv; double dv;
-        if (tr_numeral(a, &iv)) return vc_int(t->vc, iv);
+        if (tr_numeral(a, &iv))
+            return t->reals_only ? vc_real(t->vc, (double)iv)
+                                 : vc_int(t->vc, iv);
@@ bench_load command loop
-        if (sx_head_is(cmd, "set-logic") || sx_head_is(cmd, "set-option") ||
+        if (sx_head_is(cmd, "set-logic")) {
+            /* In a pure-Real logic there are no Int-sorted terms: numerals
+             * denote reals (SMT-LIB 2.6, Reals theory declaration). */
+            if (cmd->n == 2 && cmd->kids[1]->kind == SX_ATOM) {
+                const char *lg = cmd->kids[1]->atom;
+                t.reals_only = strcmp(lg, "QF_LRA")   == 0 ||
+                               strcmp(lg, "QF_RDL")   == 0 ||
+                               strcmp(lg, "QF_UFLRA") == 0;
+            }
+            continue;
+        }
+        if (sx_head_is(cmd, "set-option") ||
```

The two probe benchmarks are worth committing as the acceptance-criteria
regression pair (labels already sealed):

```
(set-logic QF_LRA) (set-info :status unsat)
(declare-fun c () Bool) (declare-fun d () Bool) (declare-fun x () Real)
(assert (= x (ite d (ite c 2 1) 0.5)))
(assert (> x 2.5))          ; x is one of {2, 1, 0.5} -- all <= 2.5
```

and the `sat` twin with `(> x 1.5)`.

### Item 2's "macro expansion too deep" -- superseded by measurement

(An earlier version of this note hypothesised that the macro-depth check
merely conflated total term depth with macro nesting and a dedicated
counter would un-skip the QF_UFLRA benchmark. The conflation is real --
both checks compare total depth against `TR_MAX_DEF_DEPTH * 20` -- but the
measured file has a genuine 6,187-deep reference chain and an exponential
naive expansion, so a counter alone fixes nothing. See "Item 2 findings"
above for the measured picture and the memoization fix that does work.)

The two QF_RDL depth-cap skips are genuinely what the plan says. One
observation that de-risks a measured raise: `sx_read` recurses on the C
stack with **no cap at all** during parse, so the two skipped files have
already demonstrated that the stack survives their full nesting depth once
(parse succeeds; only `tr_term` bails). By the same token, `sx_read` -- not
`tr_term` -- is the first thing a deeper file would overflow, so any cap
raise should consider both. The plan's recommendation (skip item 2, modulo
the macro-counter fix above) stands.

### Two adjacent stale spots, found in passing

- `tests/corpus/smtlib/unsupported_ite_skip.smt2` **no longer skips** -- the
  ite lifting made it parse, and it now reports `unsat, proved`. Its header
  comment and the README's "keeps the skip path exercised" claim are stale;
  the committed corpus currently exercises the skip path with **nothing**.
  If item 1 lands, rename/relabel this file and add a genuinely-outside
  fixture (e.g. a parametric `declare-sort` or `push`/`pop`).
- The corpus README has no skip tally for the external sample; the numbers
  (193/7, 142 decided, 40 over budget) live only in this plan. The
  acceptance criterion "update the tally" implies adding one.

### What could not be checked here

The external 200-benchmark sample (`github.com/turmeric-lang/smt-lib-benchmarks`)
was not pulled -- this session's GitHub access is scoped to the main repo
only. So "the four spider_benchmarks files parse" and the real post-fix
tally still need one sweep run wherever that clone is available. Everything
else above is verified in-tree.
