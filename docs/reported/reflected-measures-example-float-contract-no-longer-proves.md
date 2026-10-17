# `examples/reflected-measures` no longer passes `--strict-refine`: its Float contract stays unknown

**Severity:** low for programs (the check is kept and the program prints the
right answer), medium for the `reflected-measures` graduation review. The
feature's one end-to-end program claims more than the solver proves, and
nothing tests the claim. This matters at graduation because the Float half of
the surface is what has quietly narrowed.

## Repro

```sh
tur check --strict-refine examples/reflected-measures/src/main.tur
```

On `main` at 0.65.5 (2026-10-17) this exits 1:

```
examples/reflected-measures/src/main.tur:148:12: error [TUR-W0372]: solver returned unknown for the refinement on argument 1 of 'within-cutoff' in 'main'; runtime check kept
note: the predicate (< (lap-total v) 60.5) could not be proved here, which is not evidence that it fails
```

The round is `(Lap 19.25 (Lap 20.5 (Lap 18.75 (NoLaps))))`, which totals
58.5. `TUR_REFINE_STATS=1` shows the unfolding ran: 7 of 8 obligations are
proven, 41 reflected unfoldings were asserted, and no obligation ran out of
fuel. The other seven obligations in the file (Int and Bool measures, the
in-place return obligation, and RF4) still prove. `tur run` prints `58.5`, because the
runtime check passes.

These claims are now false:

- `examples/reflected-measures/README.md`: "With it, every obligation in the
  file proves at compile time", the `--strict-refine` line in the command block
  ("every contract proved"), and the `within-cutoff` / Float (Real) table row.
- `examples/reflected-measures/src/main.tur:25-28`: "a clean check is the
  claim that every contract here is discharged at compile time".
- The 0.62.0 CHANGELOG entry ("every obligation proves under
  `--strict-refine`"). It was true when written; leave it alone.

### Where the boundary is

The same measure, without `^borrow`, under `--enable=reflected-measures`:

| Argument | Verdict |
| --- | --- |
| `(NoLaps)` against `(< (lap-total v) 60.5)` | proven: `0.0 < 60.5` has no arithmetic |
| `(Lap 19.25 (NoLaps))` | unknown |
| `(Lap 19.25 (Lap 20.5 (Lap 18.75 (NoLaps))))` | unknown |

A Float measure that uses only comparisons still proves at depth. In this
measure the recursive arm is `(let [m (slowest r)] (if (> t m) t m))`.
`(< (slowest v) 20.75)` over the same three laps proves. With a lap of
`20.875` it is unknown, the check is kept, and the program panics at run time,
which is sound.

## Root cause

This is not a reflection bug. It comes from the soundness fix for
[float-proofs-assume-exact-reals](../archive/float-proofs-assume-exact-reals.md)
(resolved 2026-10-08). `linearize` now treats a real-sorted `+ - * /` as an
opaque variable (`refine_real_arith`, `src/compiler/refine_solver.c:34`; the
reasoning is at `src/compiler/refine_solver.h:290`), because double rounding
can break identities that hold over the rationals.

Unfolding asserts `lap-total(Lap 19.25 ...) = 19.25 + lap-total(...)`. S2 then
sees `19.25 + x` as a fresh opaque term with no bound, so it cannot derive
`< 60.5`. The fix is correct, and `tests/fixtures/reflect-float-measure` was
narrowed to the base case to match (its header comment says why). The example
was not updated.

The example went stale without anyone noticing because no test runs it:
nothing in `tests/`, `CMakeLists.txt`, `.github/` or the `Justfile` mentions
`examples/reflected-measures`.

This was not bisected (the container's clone is shallow). The mechanism
matches the archived report exactly, and the example landed in 0.62.0
(2026-10-03), before the 2026-10-08 fix.

## Related drift in the same docs

The refinement guide's "Reflected measures" section
(`docs/guides/refinement-types-guide.md`) understates the feature:

- Line 664: "Call-site crossings do not receive an arm's tag facts today".
  They have since 2026-10-03
  ([reflect-two-provable-facts-report-as-not-holding](../archive/reflect-two-provable-facts-report-as-not-holding.md)).
- Line 705: "what is still open (non-ground unfolding, counterexamples
  through a reflected measure)". Both landed (RF4 and RF6.1).

The plan (`docs/upcoming/reflected-measures-plan.md`) has stale parts too:

- Its header still says "One item is outstanding: RF3's two-seed fuzz
  acceptance". That item was discharged 2026-10-03.
- "What graduation waits on" still says `expires_at 0.61.0`. The registry
  says 0.64.0, which has also passed.

## Fix directions

1. **Make the example honest.** Either swap `lap-total` for a measure that
   proves (the comparison-only `slowest` above, with `within-cutoff` stated as
   a per-lap bound), or keep the sum and label it as the case that keeps its
   runtime check. Then restore "every contract proves under
   `--strict-refine`" or drop the claim.
2. **Test it.** Add a ctest target, or a `run-flags.sh` case, that runs
   `tur check --strict-refine examples/reflected-measures/src/main.tur` and
   `tur run` on it. Then the next solver change that narrows the fragment says
   so in CI.
3. **Decide what to freeze before graduating.** State in the plan and the guide
   that a Float measure proves only where its equations need no rounding
   (comparisons, selection, min/max), not sums or products. That is the
   surface graduation would make permanent.
4. Fix the guide and plan drift listed above in the same change.
