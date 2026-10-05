# Nightly arm64 suite timings are uploaded, never plotted

**Severity:** low -- observability only; no test signal is lost. **RESOLVED 2026-10-04** -- see the end.

**Summary.** `.github/workflows/nightly-arm64.yml` runs the full
`tur_generic_spec_matrix`, `tur_emitted_float_conversions` and
`turi_fixture_tests` on macOS arm64 every night and collects their timings
into `timings.jsonl`, but only uploads that file as a run artifact. The
`publish-timings` job in `ci.yml` aggregates one run of `ci.yml` and never sees
another workflow's artifacts, so the full-matrix arm64 durations never reach
`origin/ci-metrics:suite-timings-2026.jsonl` or `web/ci-metrics.js`.

**Repro.** Any nightly run (e.g. `37198125754`, 2026-10-04) has a
`timings.jsonl` artifact; `git show origin/ci-metrics:suite-timings-2026.jsonl`
has no row with that `run_id`, and no macOS `tur_generic_spec_matrix` row with
`shard_index` null since #1019.

**Root cause.** `nightly-arm64.yml` "Collect suite timings" step (the comment
above it says so: "teaching it a second workflow is a separate change").

**Fix directions.**
- Give `nightly-arm64.yml` its own publish step that appends to the
  `ci-metrics` branch, reusing whatever `ci.yml`'s `publish-timings` job does
  (same concurrency group, so the two never race on a push).
- Or a `workflow_run`-triggered publisher that handles both workflows.

Rows are unsharded (`shard_index`/`shard_total` null), so once published they
continue the pre-sharding macOS series directly.

Origin: the one follow-up left open by
[ci-aux-suite-latency-plan](ci-aux-suite-latency-plan.md) (9.1, 11.4).

## Resolved 2026-10-04

Fix direction 1. `nightly-arm64.yml` has a `publish-timings` job: it runs
after `suites` (`always()`, `main` only), downloads that run's
`nightly-arm64-<run_id>` artifact and appends `timings.jsonl` through the same
`tools/ci/publish-timings.sh` ci.yml uses, under the same repository-wide
`ci-metrics` concurrency group. The write token stays on that ubuntu job, off
the macOS one.

The rows land in the existing `Debug | macOS | AppleClang | 3 | jit`
environment without colliding with the PR leg: the nightly runs
`tur_generic_spec_matrix`, `turi_fixture_tests` and
`tur_emitted_float_conversions` unsharded, the PR leg's macOS rows for the
first two carry `[1/4]`, and the float lint does not run there at all. So the
nightly continues the pre-sharding full-matrix series.

**Verify** on the first scheduled run after this merges: a
`ci-metrics: N suite rows ... [skip ci]` commit whose rows carry that run's
`run_id`.
