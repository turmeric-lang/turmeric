# Float refinement proofs assume exact reals; the runtime check runs in double

**Resolved 2026-10-08 (direction 2, the rounding-safe fragment, plus NaN).**
The stages now decide a `float` only where double and the rationals agree
(`refine_solver.h`, "float-proofs-assume-exact-reals"):

- **Rounding.** `linearize` treats a real-sorted `+ - * /` or negation as an
  opaque variable (`refine_real_arith`), so Fourier-Motzkin reasons about
  comparisons, bounds and transitivity between real terms and literals, which
  double computes exactly, and never about an identity rounding breaks.  EUF
  still sees the node as an application, which is sound: double arithmetic is
  a function.
- **NaN**, found while doing it, and the more likely of the two in real code.
  A NaN is on neither side of `<` and unequal to itself, so three rewrites
  assumed it away: `la_assert_cube` read `!(x < y)` as `y <= x`; `nnf` split
  `a != b` into `a < b | b < a`; and `vc_mk` folded `(= x x)` and `(<= x x)` to
  true.  The two-`if` `clamp` was "proved" in range and returned a NaN with its
  check elided.  Now a negated comparison or a disequality counts only when
  its real-sorted sides are NaN-free in the cube -- a literal, or an argument
  of a positive comparison literal, which no NaN satisfies
  (`refine_cube_nan_free`; S2, S1's disequality check and S3's re-check all
  ask it); the split is not done for a real side; and the fold is not done
  for a real term (`x < x` still folds to false, which a NaN satisfies too).
  Skipping a literal only weakens a refuter, so each of these costs at most a
  proof.

`float` literals in a contract message, and in the source `defmacro*` and the
inline-C JIT re-read, now print back to themselves (`form_print`): `%g` gave
`(>= r 0.0)` as `(>= r 0)`, and handed `2.0` to a `defmacro*` body as the int
`2`, which failed to elaborate (`macro-procedural-float-literal-round-trips`).

What it cost: `refine-float-lra`, `refine-float-measure` and
`reflect-float-measure` proved arithmetic facts under `--strict-refine`;
they now prove comparison facts (`0.25 <= x < 2.5 |- x < 3.75`,
`norm(v) < 3.25 |- norm(v) < 3.75`, `fsum(FNil) < 0.75`).  A reflected float
measure over a concrete longer list is no longer proved -- `h + fsum(t)`
rounds -- though the model search, which evaluates in double, still refutes
one (`errors/reflect-float-not-tightened`).  Pinned by
`refine-float-rounding-keeps-check` (the repro below) and
`refine-float-nan-keeps-check` (`clamp` of a NaN), both firing on both
engines, and `test_reals_are_doubles` in `tests/unit/refine_solver.c`.

Not covered, and not filed: congruence through a float equality
(`a = b |- f(a) = f(b)`), which double breaks only at signed zero (`0.0 =
-0.0`, but `1/0.0` is not `1/-0.0`).  Direction 3 (modelling the rounding) is
what would win back the arithmetic proofs, if a program ever needs them.

**Severity:** medium -- a genuine miscompile class, but only for a float
refinement whose proof depends on an arithmetic identity that IEEE rounding
breaks. Every obligation still has its runtime fallback; the fallback is what
gets elided here.

## Summary

S2 (`refine_solver_arith.c`) decides real-sorted VCs over exact rationals.
The emitted contract check evaluates the same predicate in C `double`
arithmetic. Where the two disagree, a proof S2 finds is a proof about numbers
the program never computes, and the runtime check it elides is one that would
have fired.

## Minimal repro

```turmeric
;; Over exact reals (x + 0.1) - 0.1 = x, so S2 proves the return refinement
;; and the check is elided.  In doubles it is false for x = 0.3.
(defn g [x : #refine{ v : float | (> v 0.0) }] : #refine{ r : float | (= r x) }
  (- (+ x 0.1) 0.1))

(defn main [] : int
  (println (g 0.3))
  0)
```

| build | result |
|---|---|
| `tur build` | `2 obligation(s): 2 proven`; prints `0.3`, exit 0 |
| `TUR_REFINE_NO_DISCHARGE=1 tur build` | `panic ...: Return contract violated` |

(0.3 + 0.1) - 0.1 is 0.30000000000000004 in double, and `=` on doubles is
exact. Strict inequalities near a boundary fail the same way
(`x < 1.0 |= x + 0.1 < 1.1` is provable and false for the largest double
below 1.0), and so does anything that can underflow
(`x > 0.0 |= x * 0.25 > 0.0` fails for the smallest subnormal).

## Root cause

`linearize` (`refine_solver_arith.c`) treats `+ - *` over real-sorted terms
as exact field operations, and `vc_mk` (`refine_vc.c`) folds real literals
with `double` arithmetic and then reasons about the result as a rational.
Nothing in the pipeline records that a real-sorted value is a `double`. The
in-house model search, by contrast, now evaluates reals in `double`, so a
counterexample it reports is one the program would reject; it is the
proving side that is optimistic.

This is not a bug in any one stage: it is the choice, never written down
until now, to give `float` the semantics of `Real`. Several refinement-type
systems make the same choice and carry the same caveat. The user guide now
states it (`docs/guides/refinement-types-guide.md`, "Floats are proved as
exact reals").

## Fix directions

1. **Keep the choice, keep the caveat** (the state after this note). Cheapest;
   float refinements stay useful for the common monotone shapes
   (`x >= 0.0 |= x * 0.25 >= 0.0`, `x >= 0.0 |= x + 7.1 > 0.0`), which are
   also true in double.
2. **A rounding-safe fragment.** Let S2/S3 decide a real-sorted cube only
   when its refutation needs no real *arithmetic*: comparisons between
   real variables and literals (transitivity, bounds) are exact in double
   because no operation rounds. A cube with an `ADD`/`SUB`/`MUL`/`DIV`/`NEG`
   node of sort `VS_REAL` answers `RT_UNKNOWN`. Sound, one predicate in
   `la_assert_cube` / `euf_assert_cube`, and it regresses
   `tests/fixtures/refine-float-lra` (both of its proofs use arithmetic)
   and `refine-float-measure` -- those would need `--strict-refine` removed
   or the expectations changed.
3. **Model the rounding.** Widen each real operation's result by a relative
   error bound (`fl(a op b)` lies in `(a op b)(1 +/- 2^-53)`), which turns an
   equality goal over reals into two inequalities with slack and keeps the
   monotone proofs. Correct and complete for the common cases, but a real
   project (interval reasoning inside Fourier-Motzkin), and it does not
   cover underflow, overflow, or NaN.

Direction 2 is the one consistent with the guide's stated invariant ("turning
`refined` on can never turn a correct program into a wrong one"); direction 1
is the one consistent with the existing fixtures. The choice belongs to the
maintainer, which is why this is a report and not a patch.
