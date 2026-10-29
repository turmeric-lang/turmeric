-- The store behind /ci (docs/archive/ci-metrics-d1-plan.md).
--
-- CI POSTs each push's rows to /api/ci/report (web/ci-api.js), which writes
-- them here in one transaction. Column names are the row keys the collectors
-- already write, so a report needs no renaming. A key a collector adds later is
-- ignored until a migration adds its column.

-- One row per build environment: the page's envKey() tuple, plus sanitize,
-- which rows carry but the page's key never used.
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
-- Unsharded rows store shard 0/0, so the key works (SQLite treats NULLs as
-- distinct in a UNIQUE key). kind is NULL for ctest suites and 'web-perf' for
-- the browser measurements, which the suite views leave out.
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
CREATE INDEX suite_run_ts ON suite_run (ts);
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
CREATE INDEX repo_loc_ts ON repo_loc (ts);

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
CREATE INDEX docs_counts_ts ON docs_counts (ts);

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

-- Bumped by every successful report. Read URLs carry the newest id, which is
-- what lets their responses be immutable.
CREATE TABLE ingest (
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  at INTEGER NOT NULL,
  run_id TEXT, run_attempt INTEGER, sha TEXT,
  suite_rows INTEGER, loc_rows INTEGER, docs_rows INTEGER, bench_rows INTEGER
);
