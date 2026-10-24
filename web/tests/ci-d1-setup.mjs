// Give the Playwright dev server a fresh local D1 for /api/ci/*.
//
// playwright.config.js runs this before `npm run dev`, with TUR_CI_TEST_STATE
// naming a throwaway directory (per port, so two runs cannot share one). It is
// wiped and the migrations are applied to it, so ci-api.spec.js starts from an
// empty store and never writes into the developer's own .wrangler/state.
// vite.config.js points the Worker's D1 at the same directory.
import { execFileSync } from 'node:child_process';
import { rmSync } from 'node:fs';
import { dirname, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';

const web = resolve(dirname(fileURLToPath(import.meta.url)), '..');
const state = process.env.TUR_CI_TEST_STATE;
if (!state) throw new Error('TUR_CI_TEST_STATE is not set; run this through playwright.config.js');

rmSync(state, { recursive: true, force: true });
execFileSync(resolve(web, 'node_modules/.bin/wrangler'), [
  'd1', 'migrations', 'apply', 'ci-metrics', '--local', '--persist-to', state,
], { cwd: web, stdio: ['ignore', 'ignore', 'inherit'], env: { ...process.env, CI: '1' } });
