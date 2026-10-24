# /ci "latest run" is the newest single job upload, not the newest run

**Severity:** low-medium (the dashboard's headline tiles and skip ledger are
wrong on live data; nothing outside /ci is affected).

## Symptom

On the Tests tab, against the 2026-10-23 `ci-metrics` data for
`Linux · GNU 13.3 · 4 cores · JIT`:

- **Suites: 1** (`216 seen in range`), where a run reports ~205 suites.
- **Total wall time: 15m 58s**, described as "Sum of every suite in the latest
  run" -- it is one suite.
- The **skip ledger** and the **Failing** tile read the same one-suite slice, so
  a skip or failure reported by any other job of the newest run is invisible.

## Root cause

Each CI job uploads its own suites with its own `ts`: in the 7 days to
2026-10-23 one `run_id` carries up to 6 distinct `ts` values, and that 7-day
window has ~90 runs but 705 distinct `ts`. The Worker treats one `ts` as one
run:

- `web/ci-api.js` `timings()` picks `latestTs` as the newest `ts` and selects
  `latest` with `AND ts = ?` -- the newest job's rows only.
- `web/ci-aggregate.js` `timingsFromRows()` (the page tests' stub) does the
  same with `r.ts === latestTs`, so the fixtures, which give every suite of a
  run one `ts`, never show it.

The same split is why the duration chart's footer counts those as "705 runs" over a
week of ~90 runs, and why `wireHover` has to snap only to x values a drawn
series has a point at.

## Fix directions

- Take `latest` as the newest row **per series** within the newest `run_id`
  (or within the newest day), not the rows at one `ts`.
- Or key the run axis on `(run_id, run_attempt)` with the run's max `ts`, so
  `runs` / `shapeRuns` count workflow runs and the footer says what it means.
- Make the page-test fixture give each job its own `ts` so the stub
  reproduces the live shape.
