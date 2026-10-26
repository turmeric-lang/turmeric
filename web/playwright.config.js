import { defineConfig, devices } from '@playwright/test';
import { execFileSync } from 'node:child_process';
import { existsSync, readdirSync, readFileSync } from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const HERE = path.dirname(fileURLToPath(import.meta.url));

// Port 3000 is the developer's own dev server (`tur run web-dev`). The suite must
// never use it: pointed at a running server, it silently tests whatever
// checkout that server was started from, and the developer's session with it.
// So the suite always starts its OWN server, on a free port picked here, and
// never reuses one.
//
// The port is picked once, in the runner, and handed down through the
// environment: every worker re-evaluates this file, and a fresh random pick
// there would disagree with the server the runner started. Set TRY_TEST_PORT
// to pin it (any free port except 3000).
if (!process.env.TRY_TEST_PORT) {
    process.env.TRY_TEST_PORT = execFileSync(process.execPath, [
        '-e',
        "const s = require('net').createServer();" +
        "s.listen(0, '127.0.0.1', () => { process.stdout.write(String(s.address().port)); s.close(); });",
    ]).toString().trim();
}
const PORT = process.env.TRY_TEST_PORT;
if (PORT === '3000') {
    throw new Error('TRY_TEST_PORT=3000 is the developer dev server; pick any other port');
}
const ORIGIN = `http://localhost:${PORT}`;

// /api/ci/* under test runs on a local D1 of its own (tests/ci-d1-setup.mjs
// wipes and migrates it before the server starts) and a known report token.
// Both reach the dev server through vite.config.js. Set here, in the runner, so
// the server and the test workers agree.
process.env.TUR_CI_TEST_STATE ??= `.wrangler/ci-test-state-${PORT}`;
process.env.TUR_CI_TEST_TOKEN ??= 'ci-test-token';

// A spec that waits for the evaluator -- `#wasm-status-text` reading Ready --
// needs web/public/turmeric.js and turmeric.wasm. Those are build products of
// `tur run wasm` (gitignored), so a fresh clone, a laptop without Emscripten and
// a cloud sandbox have neither. Left in, every such test burns its full 30 s and
// fails with "Failed to load WASM", which reads as a product bug in whatever you
// were editing (docs/reported/web-specs-fail-instead-of-skipping-without-the-wasm-bundle.md).
//
// So when the bundle is absent those specs are left out and the runner says so
// once. CI is never excused: there a missing bundle is a broken build, so every
// spec runs and fails loudly. `docs-pane` and the others that do not wait for the
// evaluator keep running either way -- the pane must not depend on the boot.
function specsNeedingWasm() {
    if (process.env.CI || existsSync(path.join(HERE, 'public', 'turmeric.wasm'))) return [];
    const dir = path.join(HERE, 'tests');
    const waits = /toHaveText\(\s*(?:['"]Ready['"]|\/Ready\/)/;
    const needing = readdirSync(dir).filter(
        f => f.endsWith('.spec.js') && waits.test(readFileSync(path.join(dir, f), 'utf8')));
    // Every worker re-evaluates this file; say it once, from the runner.
    if (needing.length && !process.env.TRY_TEST_WASM_NOTICE) {
        process.env.TRY_TEST_WASM_NOTICE = '1';
        console.warn(
            `web/public/turmeric.wasm is not built, so ${needing.length} spec files that wait ` +
            'for the evaluator are left out of this run. Build it with `tur run wasm` ' +
            '(Emscripten; see docs/guides/running-tests-guide.md) to run them. CI runs all of them.');
    }
    return needing.map(f => new RegExp(f.replace(/[.*+?^${}()|[\]\\]/g, '\\$&') + '$'));
}
const WASM_SPECS = specsNeedingWasm();

export default defineConfig({
    testDir: './tests',
    timeout: 60_000,
    // Retries on CI only, and the number is measured rather than defensive.
    // From the web_desktop / web_mobile rows on the `ci-metrics` branch over
    // 266 commits: the suites failed on 42 and 73 of them, which sounds far
    // too flaky to gate on -- but collapsing CONSECUTIVE failures gives only
    // 10 and 7 episodes, the longest running 21 and 49 commits. Nearly all of
    // that is two standing bugs apiece, since fixed. Isolated single-commit
    // failures, which is the actual flake signal, are 6/266 (2.3%) for
    // desktop and 5/266 (1.9%) for mobile.
    //
    // ~2% is low enough to gate on and too high to gate on undamped: it is
    // about one spuriously blocked PR in 43. Two retries take an independent
    // 2% per-test flake to ~1e-5, which is what makes the desktop suite
    // BLOCKING in ci.yml defensible. Zero locally on purpose -- a flaky test
    // someone just wrote should fail in front of them, not hide behind a
    // retry. A test that only passes on retry is still reported `flaky` by
    // the list reporter, so the signal is not lost either.
    retries: process.env.CI ? 2 : 0,
    use: {
        baseURL: ORIGIN,
    },
    projects: [
        {
            name: 'desktop',
            use: { ...devices['Desktop Chrome'] },
            testIgnore: [/mobile\..*\.spec\.js$/, ...WASM_SPECS],
        },
        {
            name: 'mobile',
            use: { ...devices['iPhone 13'] },
            testMatch: /mobile\..*\.spec\.js$/,
            testIgnore: WASM_SPECS,
        },
    ],
    webServer: {
        command: `node tests/ci-d1-setup.mjs && npm run dev -- --port ${PORT} --strictPort`,
        url: ORIGIN,
        reuseExistingServer: false,
        timeout: 30_000,
    },
});
