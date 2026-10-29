# The float-conversion check ignores a fixture's `flags`

**Severity:** low (test coverage). `tests/check-emitted-float-conversions.py`
(ctest `tur_emitted_float_conversions`, and inside both source fuzzers) runs
`tur emit-c <fixture>` with no flags.

## What it misses

- **15 fixtures are never checked.** They need an experiment flag to compile,
  so without it `emit-c` fails and the checker reads the failure as "not a
  program this check applies to" -- silently. Counted 2026-10-28:
  - 6 `--enable=quote-data`;
  - 4 `--enable=discontinue`;
  - 4 `--enable=refined-fn-types`, all four of that experiment's positive
    fixtures, so nothing it emits is covered;
  - 1 `--enable=script-deps`.
- **Up to 127 more fixtures with a `flags` file are checked under different
  flags than they run with.** A flag that changes codegen (`--no-contracts`,
  `--strict-refine`'s effect on elision, any experiment) means the C that is
  linted is not the C the suite runs.

## Root cause

`lint_tur` (`tests/check-emitted-float-conversions.py:77`) builds the command
as `[tur, "emit-c", path]`, and `corpus_inputs` never reads `flags`. A failed
`emit-c` in corpus mode is skipped without a count.

## Fix directions

- Read `<dir>/flags` in `corpus_inputs` / `lint_tur` and pass it, as
  `tests/run.sh` does (`fixture_flags`).
- Count corpus fixtures whose `emit-c` failed and print the number. A gated
  fixture silently dropping out is what this report is.
- After the fix, run `--corpus` once: the refined-fn-types fixtures emit
  call-site checks over `float` arguments that have never been linted.
