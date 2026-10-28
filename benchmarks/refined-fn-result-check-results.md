# Refined function types: cost of the call-site checks (F4)

refined-function-types-plan F4 asked whether the post-call result check on a
call through a refined function type (F2, D4b) is cheap enough to keep, or
should be removed by wrapping unproved coercions in a checking closure
(Findler-Felleisen). Measured 2026-10-28 on a 4-core Intel Xeon @ 2.10GHz,
Linux, `cc -O2` (the `tur build` default).

**Decision: keep the check (option 1).** The wrapper (option 2) is not built.

Regenerate: `bash benchmarks/bench-refined-fn-result-check.sh` (it needs a
Release `tur` in `build-release/`).

## A Release build pays nothing by default

A Release `tur` strips contract checks unless `--keep-contracts` is given (see
the compiler-flags guide), and these checks are contract checks. Everything
below is the cost with checks ON: a Debug `tur`, or `--keep-contracts`.

## Compiled, as emitted today

`n` calls through a function-typed parameter, with the real function chosen at
run time so the C compiler cannot devirtualize. Release `tur`,
`--keep-contracts`, n = 2e8. Two runs: the first a median of 5, the second a
median of 3.

| variant | run 1 ns/call | vs plain | run 2 ns/call | vs plain |
|---|---|---|---|---|
| plain `(fn [int] int)` | 71.07 | 1.00x | 66.67 | 1.00x |
| result check `(fn [int] #refine{ r : int \| (> r 0) })` | 70.38 | 0.99x | 71.09 | 1.07x |
| argument check, kept `(fn [#refine{ v : int \| (>= v 0) }] int)` | 73.11 | 1.03x | 68.39 | 1.03x |
| checking wrapper (option 2, unproved coercion) | 158.69 | 2.23x | 154.69 | 2.32x |

About 70 ns a call is not the check. Every function that calls through a
function value is CPS-colored today (cps-dk-internals-guide, "Over-coloring
(R-1)"), so the loop runs on the DK machinery. Against that the checks fall
within run-to-run spread: -1% to +7% across the two runs, and plain itself
moved 7% between them. The wrapper more than doubles the cost in both runs,
because it is a second call through a function value on that same machinery.

## Compiled, with the CPS cost taken away

`refined-fn-result-check-isolated.c` is the emitted check sequence on a direct
loop. That is what a call will cost once R-1 is narrowed. 1e9 calls, best of 5,
four runs (`-O2` three times, `-O3` once):

| variant | ns/call |
|---|---|
| plain | 1.63 -- 1.84 |
| result check | 1.43 -- 1.47 |
| argument check, kept | 1.43 -- 1.49 |
| checking wrapper | 2.82 -- 3.00 |

The checks measure as free: a compare against a constant and a predicted
branch, hidden behind the indirect call. They measured faster than plain in
every run, which is code-alignment noise, not a speed-up. The wrapper costs
about 1.2 ns, or +65--80%: the second indirect call.

## Interpreter (`tur --interpret`)

`stdlib/args.tur` does not run under the interpreter, so `n` is fixed at 1e6 in
the source. Three runs each:

| variant | ms |
|---|---|
| plain | 462 -- 580 |
| result check | 975 -- 1058 |
| hand-written checking wrapper (`if` + `panic`) | 737 -- 828 |
| control: a plain `defn` | 576 -- 715 |
| control: the same `defn` with a return refinement it cannot prove | 894 -- 971 |

Here a check costs about 0.3--0.5 us a call, which roughly doubles a trivial
loop. It is not specific to function types: the control rows show the same
cost for an ordinary `defn` return check. The cost is the interpreter's generic
call to `tur-contract-check-at` (the hand-written wrapper's inline `if` beats
it). A wrapper would not remove it for an unproved coercion, because the check
moves into the wrapper and gains a call. Making contract checks cheaper in the
interpreter would help every contract; it is not an F4 question.

## Why option 1

- Compiled, the check is free next to the call it guards: noise today, and
  within noise on a direct loop.
- Option 2 helps only coercions it can PROVE, whose calls would then skip the
  check. That saves nothing measurable. Every unproved coercion pays an
  allocation plus a second indirect call on every call (+65--80% on a direct
  loop, +123--132% today), which is the opposite of the plan's aim.
- It would also add a closure allocation per coercion that needs region hooks
  and drop glue, for no measured benefit.

Revisit only if a profile shows a hot higher-order loop whose cost really is
the check, once R-1 is narrowed. The isolated benchmark is the place to look.
