# CI Metrics in D1 -- CI reports to the Worker, /ci reads chart-ready data

> **Status: DONE, archived 2026-10-29.** Phases 1-3 shipped 2026-10-23 in
> #1355; phase 4 (retirement) on 2026-10-29, after the store had run in
> production since its deploy (0.70.4, 2026-10-24), with every main push and
> nightly reporting to it (ingest 70 by then). D1 is now the only store behind /ci: nothing writes the
> `ci-metrics` orphan branch any more, and the Worker no longer proxies it.
> One step is left to the owner, since sessions cannot delete remote
> branches: delete the `ci-metrics` branch (after the optional last backfill
> top-up under "Retirement as built"). Database `ci-metrics`
> (`b25ff4cf-0e0c-4233-b79a-b15147fa4a6e`); `CI_METRICS_TOKEN` is a Worker
> secret and a repository secret. The owner signed off on everything this
> gives up (git history of the data, the year files, raw-row access via
> GitHub): none of it is kept or replaced. What implementation changed from
> this draft is in "As built" and "Retirement as built", at the end.

## Why

/ci downloads raw NDJSON that `tools/ci/publish-timings.sh` appends to the
`ci-metrics` branch, through a Worker that proxies `raw.githubusercontent.com`
(`web/worker.js`, `proxyMetricsNDJSON`). Measured 2026-10-23:

| What the page fetches | Raw | Gzipped |
|---|---|---|
| Default view: `suite-timings-recent.jsonl` (last 8 days, every environment) | 12.35 MB | 0.51 MB |
| Any range wider than 7 days: `suite-timings-2026.jsonl` | 65.4 MB | 2.7 MB |
| `repo-loc-2026.jsonl` + `docs-counts-2026.jsonl` | 0.40 MB | 0.03 MB |

The full log grows ~1.4 MB a day (12.2 MB on Sep 15, 65.4 MB on Oct 23) and
would reach ~160 MB by Dec 31. Rows per push grew from ~240 to ~320 over the
same period. The recent window is 9-16 MB depending on push rate. The proxy
labels everything `application/x-ndjson`, and it is not established that
Cloudflare compresses that type.

The owner opens /ci about 100 times a day, mostly on a phone. At 12.8 MB a
view that is ~1.3 GB a day. Every view parses ~32k rows to draw one
environment's suites.

The page cannot be made small while the store is a file: the Worker would
have to parse 12-65 MB per request to aggregate it, which is past a Worker's
CPU budget. A database can answer "this environment, this range" directly.

## Decisions

- **D1, not KV.** CI appends rows from several legs at once and the page
  queries time ranges. KV has no append and no transactions (two legs
  read-modify-writing one value lose rows), key-per-run means listing and
  fetching ~130 keys per view, and reads are eventually consistent. D1 is
  SQLite: plain `INSERT`s, indexed range queries, unique keys for idempotency.
- **No archive, no export of the old files, no year partitions.** One table
  per dataset, kept forever. At ~5k timing rows a day it is ~2M rows a year
  (a few hundred MB), well inside D1's per-database size.
- **Writes go through the Worker, authenticated by one bearer token.** CI never
  talks to D1 directly and needs no Cloudflare credentials.
- **The server sends chart-ready data, not rows.** Per-run points for the 1d
  and 7d ranges (tooltips name the commit); daily medians for 30d, 90d and
  all-time.
- **Responses are immutable and versioned.** Every read URL carries the
  current ingest version, so the browser and Cloudflare's edge cache them
  forever and a repeat view between pushes downloads nothing but a small
  `/api/ci/meta`.

Expected payloads, computed from the 2026-10-23 data for the broadest
environment (Linux, GNU 13.3, 4 cores, JIT):

| Range | Rows behind it | Response | Gzipped |
|---|---|---|---|
| 7d, one point per run | 18.6k | ~290 KB | ~95 KB |
| 30d, daily medians | 51k | ~85 KB | ~25 KB |
| All time, daily medians | 52k | ~85 KB | ~25 KB |

Against 12.8 MB today. Daily medians stay roughly constant in size as the
data grows, since there are only ~230 suites and one point per suite per day.

## Schema

`web/migrations/0001_ci_metrics.sql`, applied with
`wrangler d1 migrations apply`.

```sql
-- One row per build environment. The page's envKey() tuple, plus sanitize,
-- which rows carry but the key never used.
CREATE TABLE env (
  id          INTEGER PRIMARY KEY,
  build_type  TEXT NOT NULL,
  os          TEXT NOT NULL,
  cc          TEXT NOT NULL,
  nproc       INTEGER NOT NULL,
  jit         INTEGER NOT NULL,
  sanitize    INTEGER NOT NULL DEFAULT 0,
  UNIQUE (build_type, os, cc, nproc, jit, sanitize)
);

-- One row per suite (or shard of a suite) per CI run per environment.
-- Unsharded rows store shard 0/0, so the unique key works (SQLite treats
-- NULLs as distinct in UNIQUE). kind is NULL for ctest suites and 'web-perf'
-- for the browser measurements, which the suite views leave out.
CREATE TABLE suite_run (
  run_id               TEXT NOT NULL,
  run_attempt          INTEGER NOT NULL,
  env_id               INTEGER NOT NULL REFERENCES env(id),
  suite                TEXT NOT NULL,
  shard_index          INTEGER NOT NULL DEFAULT 0,
  shard_total          INTEGER NOT NULL DEFAULT 0,
  kind                 TEXT,
  ts                   INTEGER NOT NULL,
  sha                  TEXT NOT NULL,
  branch               TEXT,
  duration_ms          REAL NOT NULL,
  status               TEXT NOT NULL,          -- pass | skip | fail
  skip_reason          TEXT,
  partial_skip_reason  TEXT,
  passed               INTEGER,
  failed               INTEGER,
  skipped              INTEGER,
  discovered           INTEGER,
  samples              INTEGER,
  PRIMARY KEY (run_id, run_attempt, env_id, suite, shard_index, shard_total)
);
CREATE INDEX suite_run_env_ts ON suite_run (env_id, ts);
CREATE INDEX suite_run_counts ON suite_run (suite, ts) WHERE passed IS NOT NULL;

-- One row per push: properties of the commit, no environment.
CREATE TABLE repo_loc (
  run_id TEXT NOT NULL, run_attempt INTEGER NOT NULL,
  ts INTEGER NOT NULL, sha TEXT NOT NULL, branch TEXT,
  code_lines INTEGER, test_lines INTEGER, bench_lines INTEGER,
  example_lines INTEGER, generated_lines INTEGER,
  code_files INTEGER, test_files INTEGER, bench_files INTEGER,
  example_files INTEGER, generated_files INTEGER, uncounted_files INTEGER,
  code_by_lang TEXT, test_by_lang TEXT,        -- JSON objects
  PRIMARY KEY (run_id, run_attempt)
);

CREATE TABLE docs_counts (
  run_id TEXT NOT NULL, run_attempt INTEGER NOT NULL,
  ts INTEGER NOT NULL, sha TEXT NOT NULL, branch TEXT,
  open_plans INTEGER, open_reports INTEGER,
  active_plans INTEGER, godot_plans INTEGER, spices_plans INTEGER,
  held_plans INTEGER, v1_plans INTEGER,
  turmeric_guides INTEGER, spice_guides INTEGER, spices INTEGER,
  spice_names TEXT,                            -- JSON array
  reports_filed INTEGER, reports_archived INTEGER,
  PRIMARY KEY (run_id, run_attempt)
);

-- The nightly benchmark rows (benchmarks/audit/bench-rows.py).
CREATE TABLE bench (
  run_id TEXT NOT NULL, run_attempt INTEGER NOT NULL,
  suite TEXT NOT NULL,                         -- bench:<program>/<engine>/<metric>
  ts INTEGER NOT NULL, sha TEXT, branch TEXT,
  program TEXT, engine TEXT, metric TEXT,
  value REAL, unit TEXT, cv REAL, noisy INTEGER, reps INTEGER, status TEXT,
  os TEXT, cc TEXT, nproc INTEGER, build_type TEXT, tur_version TEXT,
  PRIMARY KEY (run_id, run_attempt, suite)
);
CREATE INDEX bench_ts ON bench (ts);

-- Bumped by every successful report; read URLs carry it.
CREATE TABLE ingest (
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  at INTEGER NOT NULL,
  run_id TEXT, run_attempt INTEGER, sha TEXT,
  suite_rows INTEGER, loc_rows INTEGER, docs_rows INTEGER, bench_rows INTEGER
);
```

Column names are the row keys the collectors already write, so the payload
needs no renaming. A field a collector adds later is ignored until a
migration adds its column. That is the one cost of a schema over free-form
JSON, and it is deliberate: a misspelt key fails loudly instead of charting as
a gap.

## Write path

### `POST /api/ci/report`

- `Authorization: Bearer <CI_METRICS_TOKEN>`, compared in constant time
  against the Worker secret of the same name. Anything else gets a 401.
- Body: `{"timings": [...], "loc": [...], "docs": [...], "bench": [...]}`,
  each array optional, holding the same row objects the collectors write
  today. Rejected above 5 MB (one push is ~120 KB).
- **One SQL statement per table, not one per row.** Each array is bound as a
  single JSON string and expanded in SQL:

  ```sql
  INSERT INTO suite_run (run_id, run_attempt, env_id, suite, ...)
  SELECT j.value ->> '$.run_id', j.value ->> '$.run_attempt',
         (SELECT id FROM env WHERE ...), j.value ->> '$.suite', ...
  FROM json_each(?1) AS j
  ON CONFLICT DO UPDATE SET duration_ms = excluded.duration_ms, ...;
  ```

  preceded by an `INSERT OR IGNORE INTO env ... SELECT DISTINCT ... FROM
  json_each(?1)`. All statements go in one `db.batch()`, which D1 runs as a
  transaction, so a report lands whole or not at all. This keeps a report to
  about six statements, well under D1's per-invocation query cap and its
  100-bound-parameter limit, whatever the row count.
- `ON CONFLICT ... DO UPDATE` makes a retried report, or a re-run of the same
  `run_attempt`, overwrite rather than duplicate. A new `run_attempt` is a new
  row, as it is today.
- Response: `{"ingest": <id>, "suite_rows": n, ...}`. A 4xx names the first
  bad row and field.

### CI changes

- `tools/ci/publish-timings.sh` is replaced by `tools/ci/report-metrics.py`:
  read the same input files, POST once, retry 5 times with backoff on a 5xx
  or a network error, never on a 4xx. Same flags as the shell script
  (`timings.jsonl --loc --docs --bench`), so the workflow steps change only
  their command.
- `ci.yml` `publish-timings` job: drop `permissions: contents: write`, drop
  `fetch-depth: 0`, keep the concurrency group (harmless, and it orders the
  ingest ids). Pass `CI_METRICS_TOKEN: ${{ secrets.CI_METRICS_TOKEN }}`.
  Fork PRs still never publish: the job's `if:` is unchanged, and they get no
  secrets anyway.
- `nightly-arm64.yml`: same swap for its publish job. Its "Fetch the
  benchmark history" step (`git fetch ... ci-metrics`, `git show
  bench-<yr>.jsonl`) becomes `curl -fsS
  "https://turmeric-lang.com/api/ci/bench?format=ndjson&days=60"`.
- The job stays non-gating. A publish failure is a red job on a main run, as
  it is today.

## Read path

All `GET`, all same-origin.

| Endpoint | Returns | Cached |
|---|---|---|
| `/api/ci/meta` | `{version, latest_ts, envs: [{id, label, ts, breadth}]}`: the environment picker, ordered as `buildEnvs()` orders it today | `max-age=30` |
| `/api/ci/timings?env=&range=&v=` | `{suites, runs, cells, latest}`, below | immutable |
| `/api/ci/counts?v=` | the Test counts timeline, already carried forward | immutable |
| `/api/ci/loc?v=` | loc rows, without the `*_by_lang` objects except on the newest row | immutable |
| `/api/ci/docs?v=` | docs rows, without `spice_names` except on the newest row that has it | immutable |
| `/api/ci/bench?v=` | bench rows (`kind = 'bench'` view) | immutable |
| `/api/ci/bench?format=ndjson&days=N` | raw bench rows for the nightly tripwire | `no-store` |

`v` is `meta.version`, the latest `ingest.id`. A versioned response is sent
with `Cache-Control: public, max-age=31536000, immutable` and also stored with
the Workers Cache API. A request whose `v` is not current gets a 302 to the
current one, so a stale tab cannot pin old data. A typical day of ~17 pushes
and ~100 views computes each response at most ~17 times. Every other view is
served from the browser or the edge.

### `/api/ci/timings` shape

```json
{
  "env": 3, "range": "7d", "mode": "run",
  "suites": ["tur_tests", "tur_tests [1/4]", ...],
  "runs":   [["a4adfe2b29cb", 1792103488], ...],
  "cells":  [[suiteIdx, runIdx, ms, status], ...],
  "latest": [{"suite": ..., "ms": ..., "status": ..., "skip_reason": ...,
              "partial_skip_reason": ...}, ...]
}
```

- `status` is 0/1/2 for pass/skip/fail, so the page's status filter still
  works client-side.
- In `"mode": "day"` (30d, 90d, all) there is no `runs`, and a cell is
  `[suiteIdx, day, median_ms, n, status]`, bucketed by suite, UTC day and
  status. Mean, p90 and delta in the sparkline table become statistics over
  the daily medians, and the table header says so in that mode.
- `latest` is every row of the environment's newest run, which feeds the
  tiles, the skip ledger and the provenance line without a second request.
- The range is anchored on the environment's newest row, not wall-clock, as
  `rangeCutoff()` is today.

### Where the aggregation code lives

The Test counts timeline (`suiteCounts()` / `countTimeline()` in
`web/ci-metrics.js`) picks each suite's home environment, sums shards and
carries counts forward. It moves verbatim into `web/ci-aggregate.js`, which the
Worker imports to build `/api/ci/counts`. The page keeps importing it for
rendering helpers, so there is one implementation, not a port. The env
ordering in `buildEnvs()` moves the same way, for `/api/ci/meta`.

## Page changes (`web/ci-metrics.js`)

- `boot()` fetches `/api/ci/meta`, then `timings` (default env, 7d), `loc`
  and `docs` in parallel, and `counts` / `bench` only when their tab is
  opened.
- Changing the environment or range is one request for that pair. The
  `windowed` / `loadFull()` machinery, `RECENT_RANGES` and the provenance
  line's file name go away. The line shows the ingest version and time
  instead.
- Rendering takes the decoded `cells` where it took `bySuite(envRows())`.
  The chart, sparkline and table code is otherwise unchanged.
- The fetch timeout and the visible boot error from the stuck-spinner fix
  (branch `claude/ci-page-stuck-loading`) stay.

## Backfill

`tools/ci/report-metrics.py --backfill <dir>` reads the four year files and
the bench file from a checkout of the `ci-metrics` branch and POSTs them to
the same endpoint in chunks of 2,000 rows. It is idempotent (same unique
keys), so an interrupted backfill is re-run, not repaired. ~175k timing rows
is ~90 requests. Run it once, after the first CI report has gone in, so no
push lands between the backfill and the switch. Overlapping rows are
overwritten, not doubled.

## Retirement (last phase)

- `tools/ci/publish-timings.sh`: deleted.
- `web/worker.js`: `TIMINGS_BASE`, `proxyMetricsNDJSON`, `proxyMetricsFile`
  and the four `/api/ci-*` routes: deleted.
- `web/tests/ci.spec.js`: the "worker proxies ..." tests become D1
  round-trip tests (below). The page tests' route stubs serve the new
  shapes.
- Docs that name the branch: `tools/ci/collect-loc.py`,
  `tools/ci/collect-docs.py` docstrings, `benchmarks/audit/bench-rows.py`
  (`--history` text), `web/repo.js`, `web/playwright.config.js`,
  `web/tests/web-perf.spec.js`,
  `docs/guides/test-suite-portability-guide.md` (line ~325),
  `docs/guides/performance-guide.md` (line ~827), and the comments in
  `ci.yml` / `nightly-arm64.yml` that explain the branch.
- The `ci-metrics` branch itself: the owner deletes it. Sessions cannot
  delete remote branches.

## Phases

1. **Store and write path.** Migration, `POST /api/ci/report`,
   `report-metrics.py`, both workflows reporting to D1. `publish-timings.sh`
   keeps running alongside it, so /ci is untouched.
   *Done when* a main push produces an `ingest` row, and the D1 row counts
   for that run match the artifact.
2. **Backfill.** One run of `--backfill`, then a spot check: per-env row
   counts for a past day match the branch's file.
3. **Read path and page.** The read endpoints, `web/ci-aggregate.js`, and the
   page switched over. *Done when* the default view's transfer, measured in
   devtools on a cold load, is under 150 KB, and a reload with no push in
   between transfers only `/api/ci/meta`.
4. **Retirement.** Everything in the list above. The bench tripwire reads the
   API.

Phases 1-3 shipped together in #1355 (the owner chose one PR over three).
Phase 4 is its own PR from `main`, after the store has run in production for a
while.

## Testing

- `wrangler dev` / the Vite Cloudflare plugin run a local D1 (Miniflare), so
  Playwright's own server has a real database. A test-only seed (rows built
  as `locFixture()` / `docsFixture()` build them, relative to now) is posted
  through the real endpoint with a test token set in `.dev.vars`.
- Round trip: POST a report, GET `meta`, `timings` and `counts`, assert the
  rows came back. POST the same report again: counts unchanged
  (idempotency). POST with a bad token: 401. POST a row with a missing
  `suite`: 400 naming it.
- Versioning: a GET with a stale `v` redirects to the current one, and a
  current one carries `immutable`.
- The existing page tests keep passing against the new shapes.

## Prerequisites (owner)

1. `wrangler d1 create ci-metrics`, then paste the printed binding into
   `web/wrangler.jsonc`:
   `"d1_databases": [{"binding": "CI_DB", "database_name": "ci-metrics",
   "database_id": "<id>", "migrations_dir": "migrations"}]`.
2. Generate a token (`openssl rand -hex 32`) and store it twice:
   `wrangler secret put CI_METRICS_TOKEN` (Worker), and a repository secret
   `CI_METRICS_TOKEN` (GitHub, Settings -> Secrets and variables -> Actions).
3. ~~Confirm the Workers plan.~~ Workers Paid (2026-10-23).

## Risks and things to verify before phase 1

The limits here are from memory, not checked against Cloudflare's current
documentation. Check them before relying on them.

- **Worker CPU time: settled.** Building `/api/ci/counts` or an all-time
  daily view reads tens of thousands of rows, which needs more than the free
  plan's ~10 ms. The account is on Workers Paid, whose default CPU limit
  covers it, so these are computed on a cache miss. No write-time `view`
  table is needed.
- **D1 usage** is far inside the paid plan's included reads and writes:
  ~5k rows written a day and, with the caching above, well under 1M rows read
  a day. A cache miss on all-time `timings` reads ~50k rows, and there are at
  most a few dozen misses a day.
- **The edge cache is per data center.** A first view from a new location
  computes once. That is irrelevant at this traffic.
- **`json_each` statement size.** One push's array is ~120 KB, under D1's
  statement and bound-value size limits. A backfill chunk of 2,000 rows is
  ~750 KB. Lower the chunk size if that is rejected.

## As built (2026-10-23)

What implementation measured or changed relative to the draft above.

- **Files.** `web/migrations/0001_ci_metrics.sql`; `web/ci-api.js` (every
  `/api/ci/*` route); `web/ci-aggregate.js` (the shared rules);
  `tools/ci/report-metrics.py` (report and `--backfill`); `web/ci-metrics.js`
  (the page, on the new endpoints); `web/tests/ci-api.spec.js` (D1 round
  trips) and `web/tests/ci-d1-setup.mjs` (the test server's own local D1).
- **D1 limits, checked against Cloudflare's docs:** 100 KB per statement, 100
  bound parameters, 1,000 queries per invocation (Paid), and **2 MB per bound
  value**. The draft bound each array as one JSON string, so a report over
  2 MB would have failed. `ci-api.js` splits any array over 1 MB into several
  statements in the same batch, so it stays one transaction.
- **The service worker cached `/api/*`.** `/sw.js` registers with scope `/`
  from /try and answered everything it did not recognize cache-first. Any
  browser that had opened /try therefore served /ci one fetch stale, and would
  have kept `/api/ci/meta` one ingest behind forever. It would also have stored
  a copy of every versioned response. `sw.js` now leaves `/api/` alone.
- **Duplicate rows collapse.** 70 key pairs on the branch (`tur_flags_tests`,
  2026-10-03..04, when it ran in two jobs of one environment) share a primary
  key, so D1 keeps the later row of each: 174,123 rows in, 174,053 stored.
  That episode drew a zig-zag, which this removes.
- **Env keys stay strings.** `/api/ci/meta` returns each environment's
  existing `envKey()` string, and `/api/ci/timings?env=` takes it, so `?env=`
  links keep working. Two `env` rows that differ only in `sanitize` merge
  under one key (none do today).
- **Daily medians are computed in SQL** (window functions). The same rule is
  `dailyMedians()` in JS, which the page-test stubs use, and
  `ci-api.spec.js` holds the two equal. **Test counts are computed in JS** on the
  Worker, from the counted rows (31k), with the page's own
  `suiteCounts()`/`countTimeline()`, now in `ci-aggregate.js`.
- **The page tests stub with the server's code.** The `/api/ci/*` stubs in
  `ci.spec.js` build each response with `ci-aggregate.js`, the module the
  Worker uses, so a stub cannot drift from a real response.
- **Measured, real data (all 174k rows, local D1, 2026-10-23):** the default
  view (broadest environment, 7d) is 313 KB raw, **94 KB gzipped**. 30d is
  27 KB gzipped, all time 28 KB, counts 7 KB, loc 13 KB, docs 8 KB, meta under
  1 KB. A repeat request for the same version comes from the Workers cache. The
  server's timings, daily medians, env order and count timeline were checked
  equal to the JS rules over the same rows, for the broadest and narrowest
  environments at every range.
- **Backfill** went in through the real endpoint into a local D1 (174,581 rows
  on the branch at 22:25 UTC; 10 s for ~90 requests), then was copied to the
  remote database as SQL, so /ci has its history from the first deploy. Pushes
  after that and before the deploy land only on the branch (the endpoint does
  not exist in production until the deploy), so after deploying, run the
  idempotent backfill once more from a fresh checkout of the branch:
  `CI_METRICS_TOKEN=$(cat ~/.config/turmeric/ci-metrics-token)
  python3 tools/ci/report-metrics.py --backfill <checkout>`.
- **Both writers run in phases 1-3.** Each publish job reports to D1 first,
  then appends to the branch in an `if: always()` step, so either failing
  leaves the other's copy whole. The nightly's tripwire history already reads
  `/api/ci/bench?format=ndjson&days=60`.
- **Not changed:** the "latest run" the tiles and skip ledger read is still
  "rows with the environment's newest `ts`". Each CI job stamps its own `ts`,
  so that is often one job's handful of suites, not the whole run. It was
  already so, and it is unchanged here.

## Retirement as built (2026-10-29)

- **Checked first.** Both writers had reported cleanly since the deploy: the
  main push of 2026-10-29 01:36 UTC logged `ingest 70: 233 suite, 1 loc,
  1 docs, 0 bench row(s)`, and the nightly arm64 run of 2026-10-28 passed its
  "Report to the /ci store" step.
- **The writers.** `tools/ci/publish-timings.sh` is deleted, and so are the
  "Append to the ci-metrics branch" steps in `ci.yml` and `nightly-arm64.yml`.
  Neither publish job asks for `contents: write` any more, so no job in
  either workflow has more than a read token. The nightly's publish job
  checks out shallow. **ci.yml's keeps `fetch-depth: 0`**, contrary to the
  CI-changes list above: `collect-docs.py` walks main's first parents for
  `reports_filed` / `reports_archived`, which are null in a shallow clone.
  Both jobs keep `group: ci-metrics`, which orders the ingest ids.
- **The Worker.** `TIMINGS_BASE`, `proxyMetricsNDJSON`, `proxyMetricsFile`
  and the `/api/ci-timings`, `/api/ci-loc`, `/api/ci-docs` and
  `/api/ci-bench` routes are deleted. Nothing on the page called them since
  phase 3.
- **Tests.** The four "worker proxies ..." tests in `web/tests/ci.spec.js`
  are deleted rather than rewritten: the D1 round trips they were to become
  already exist in `web/tests/ci-api.spec.js` (timings, loc, docs and bench,
  idempotency, auth, versioning). They also asserted on live production rows
  (line counts over 10k, plan buckets summing to `open_plans`); that check of
  the collectors' output against the real tree goes with them.
- **Docs.** The branch is no longer named as a live store in the collectors'
  docstrings, `bench-rows.py`, `benchmarks/audit/tracked.txt`, `web/repo.js`,
  `web/csp.js`, `web/ci/index.html`, the two Playwright files, the
  `ci.yml` / `nightly-arm64.yml` comments, `web/README.md`, the
  test-suite-portability guide and the performance guide. Historical mentions
  (CHANGELOG entries, archived and open reports quoting measurements taken off
  the branch) are left as written.
- **Kept:** `report-metrics.py --backfill`. Pushes that landed between the
  first backfill (2026-10-23 22:25 UTC) and the deploy reached only the
  branch, and "As built" asks for one idempotent top-up from a fresh checkout
  of it after deploying. If that was not run, run it once before deleting the
  branch:
  `git clone --branch ci-metrics --single-branch --depth 1
  https://github.com/turmeric-lang/turmeric /tmp/cim`, then
  `CI_METRICS_TOKEN=... python3 tools/ci/report-metrics.py --backfill /tmp/cim`.
- **Left to the owner:** delete the `ci-metrics` branch.
