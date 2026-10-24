-- The runner's CPU model on every suite row (tools/ci/collect-suite-timings.py
-- read_cpu_model). A hosted pool mixes machine generations under one
-- (os, nproc), and that is most of a suite's run-to-run spread, so /ci shows it
-- and filters on it. A row field rather than part of `env`: the env key is
-- what ?env= links and the series' history hang on, and every row written
-- before this column existed would otherwise be orphaned. NULL on those rows.
--
-- Apply with `npx wrangler d1 migrations apply ci-metrics --remote` BEFORE
-- deploying the Worker that writes it (web/README.md).
ALTER TABLE suite_run ADD COLUMN cpu TEXT;
