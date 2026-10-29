// What Try Turmeric costs a visitor (performance-audit-plan WP5).
//
// One test, against the PRODUCTION build (dist/client, what ships), on a
// server it starts itself on a free port:
//
//   - cold loads, each in a fresh browser context (no service worker, no
//     HTTP cache): navigation -> the eval worker's 'ready' -> the "Ready"
//     status, with the worker's own stage times (turmeric.js, the wasm module,
//     turi_wasm_init and its preload phases) from window._turiBoot;
//   - the first evaluation: Run on a one-line program, click -> result;
//   - a repeat visit in the last context, once its service worker holds the
//     bundle;
//   - the language server: boot time, and keystroke -> diagnostics latency,
//     an edit that makes a call undefined and the edit that fixes it.
//
// It records numbers; it asserts only that the page works (Ready, the right
// result, markers that appear and clear). Timing bounds belong to a trend
// (WP6), not to a single run on a shared runner. Results go to
// test-results/web-perf.json (WEB_PERF_OUT overrides), which
// tools/ci/collect-playwright-timings.py --web-perf turns into /ci rows.
//
// Skipped unless WEB_PERF=1 (CI's web-smoke job sets it) so a local suite run
// does not pay for it, and skipped when there is no production build.
// Never on port 3000: that is the developer's own server (CLAUDE.md).

import { test, expect } from '@playwright/test';
import { spawn } from 'node:child_process';
import { existsSync, mkdirSync, writeFileSync } from 'node:fs';
import net from 'node:net';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const WEB_DIR = path.dirname(path.dirname(fileURLToPath(import.meta.url)));
const DIST_DIR = path.join(WEB_DIR, 'dist', 'client');
const OUT = process.env.WEB_PERF_OUT || path.join(WEB_DIR, 'test-results', 'web-perf.json');
const COLD_LOADS = Number(process.env.WEB_PERF_LOADS || 3);
const KEYSTROKES = Number(process.env.WEB_PERF_KEYS || 5);
const LSP_DEBOUNCE_MS = 150;   // lsp-client.js DEFAULT_DEBOUNCE_MS

let server = null;

function freePort() {
    return new Promise((resolve, reject) => {
        const s = net.createServer();
        s.once('error', reject);
        s.listen(0, '127.0.0.1', () => {
            const port = s.address().port;
            s.close(() => resolve(port));
        });
    });
}

function portOpen(port) {
    return new Promise((resolve) => {
        const socket = net.connect({ port, host: '127.0.0.1' });
        socket.once('connect', () => { socket.destroy(); resolve(true); });
        socket.once('error', () => { socket.destroy(); resolve(false); });
        socket.setTimeout(500, () => { socket.destroy(); resolve(false); });
    });
}

async function startServer(port) {
    server = spawn('npx', ['vite', 'preview', '--outDir', DIST_DIR, '--host', '127.0.0.1',
                           '--port', String(port), '--strictPort'], {
        cwd: WEB_DIR, stdio: 'ignore', detached: true,
    });
    const deadline = Date.now() + 40_000;
    while (Date.now() < deadline) {
        if (await portOpen(port)) return;
        await new Promise(r => setTimeout(r, 250));
    }
    throw new Error(`vite preview never opened port ${port}`);
}

function stopServer() {
    if (!server) return;
    // npx forks vite: signal the whole group, or the listener survives.
    try { process.kill(-server.pid, 'SIGKILL'); } catch { server.kill('SIGKILL'); }
    server = null;
}

function median(xs) {
    const v = xs.filter(x => typeof x === 'number' && Number.isFinite(x)).sort((a, b) => a - b);
    if (!v.length) return null;
    const m = v.length >> 1;
    return v.length % 2 ? v[m] : (v[m - 1] + v[m]) / 2;
}

/** Response bytes of every request the context finishes -- the page's, its
 *  workers' and the service worker's -- in arrival order. */
function trackBytes(ctx) {
    const log = [];
    ctx.on('requestfinished', async (req) => {
        try {
            const s = await req.sizes();
            log.push({ path: new URL(req.url()).pathname, bytes: s.responseBodySize });
        } catch { /* a request torn down with its context */ }
    });
    return log;
}

function sumBytes(entries) {
    return entries.reduce((n, e) => n + (e.bytes || 0), 0);
}

/** The largest downloads, merged by path. */
function topPaths(entries, n = 15) {
    const by = new Map();
    for (const e of entries) by.set(e.path, (by.get(e.path) || 0) + (e.bytes || 0));
    return [...by.entries()].sort((a, b) => b[1] - a[1]).slice(0, n)
        .map(([path, bytes]) => ({ path, bytes }));
}

/** Navigate, wait for Ready, run one program; ms since navigation start.
 *  `seq` makes the program's answer unique to this load: a repeat visit
 *  restores the console, which would otherwise already show the last answer. */
async function measureLoad(page, url, seq, bytesLog = null) {
    await page.goto(url);
    await expect(page.locator('#wasm-status-text')).toHaveText('Ready', { timeout: 90_000 });
    const bytesAtReady = bytesLog ? bytesLog.slice() : null;
    const boot = await page.evaluate(() => {
        const nav = performance.getEntriesByType('navigation')[0];
        return Object.assign({}, window._turiBoot || {}, {
            domContentLoadedAt: nav ? nav.domContentLoadedEventEnd : null,
            loadAt: nav ? nav.loadEventEnd : null,
        });
    });
    // A value the page cannot already be showing (a version banner could hold
    // a bare "3", a restored console the previous load's answer).
    const want = String(40000 + 7 * seq + 2);
    const ev = await page.evaluate(async ([n, answer]) => {
        window._turiEditor.setValue(`(+ ${40000 + 7 * n} 2)`);
        const con = document.getElementById('console');
        const t0 = performance.now();
        document.getElementById('run-btn').click();
        while (!con.textContent.includes(answer) && performance.now() - t0 < 20_000) {
            await new Promise(r => setTimeout(r, 2));
        }
        const t1 = performance.now();
        return { clickToResultMs: t1 - t0, resultAt: t1, ok: con.textContent.includes(answer) };
    }, [seq, want]);
    expect(ev.ok, `the first evaluation printed ${want}`).toBe(true);
    return { boot, eval: ev, bytesAtReady };
}

/** Boot the language server, then time keystroke -> markers, both ways. */
async function measureLsp(page, n) {
    await page.evaluate(() => window._turiEditor.focus());
    const booted = await page.evaluate(async () => {
        const t0 = performance.now();
        for (let i = 0; i < 200 && !window._turiLsp; i++) await new Promise(r => setTimeout(r, 25));
        if (!window._turiLsp) return { ok: false, ms: null };
        const ok = await window._turiLsp.ready;
        return { ok, ms: performance.now() - t0 };
    });
    if (!booted.ok) return { boot: booted, breakMs: [], fixMs: [] };
    const keys = await page.evaluate(async (count) => {
        const ed = window._turiEditor;
        const lsp = window._turiLsp;
        const sleep = (ms) => new Promise(r => setTimeout(r, ms));
        async function until(pred, limit = 20_000) {
            const t0 = performance.now();
            while (performance.now() - t0 < limit) {
                if (pred(lsp.markers())) return performance.now();
                await sleep(2);
            }
            return null;
        }
        ed.setValue('(defn g [x : int] : int x)\n(defn f [] : int (g 1))\n');
        const settled = await until(m => m.length === 0);
        await sleep(400);
        // 1-based column just after the `g` of `(g 1)` on line 2.
        const col = ed.getModel().getLineContent(2).indexOf('(g ') + 3;
        const breakMs = [];
        const fixMs = [];
        for (let i = 0; i < count; i++) {
            let t0 = performance.now();
            ed.executeEdits('web-perf', [{ range: { startLineNumber: 2, startColumn: col,
                                                    endLineNumber: 2, endColumn: col }, text: 'x' }]);
            let t1 = await until(m => m.length > 0);
            breakMs.push(t1 === null ? null : t1 - t0);
            await sleep(300);
            t0 = performance.now();
            ed.executeEdits('web-perf', [{ range: { startLineNumber: 2, startColumn: col,
                                                    endLineNumber: 2, endColumn: col + 1 }, text: '' }]);
            t1 = await until(m => m.length === 0);
            fixMs.push(t1 === null ? null : t1 - t0);
            await sleep(300);
        }
        return { settled: settled !== null, breakMs, fixMs };
    }, n);
    expect(keys.settled, 'the valid program settled with no markers').toBe(true);
    expect(keys.breakMs.every(x => x !== null), 'an undefined call always became a marker').toBe(true);
    expect(keys.fixMs.every(x => x !== null), 'the marker always cleared').toBe(true);
    return { boot: booted, ...keys };
}

test('Try Turmeric: cold load, first evaluation, repeat visit, LSP keystrokes', async ({ browser }) => {
    test.skip(process.env.WEB_PERF !== '1', 'set WEB_PERF=1 to measure (CI web-smoke job does)');
    test.skip(!existsSync(path.join(DIST_DIR, 'turmeric.wasm')),
              'no production build in web/dist/client: run `npm run build` first');
    test.setTimeout(420_000);

    let port = await freePort();
    while (port === 3000) port = await freePort();
    await startServer(port);
    // ?sw=1: on a loopback host the app skips the service worker unless asked
    // (web/README.md); production always has it, so the measurement does too.
    const url = `http://127.0.0.1:${port}/try/?sw=1`;
    try {
        const cold = [];
        let ctx = null;
        let page = null;
        let bytesLog = null;
        for (let i = 0; i < COLD_LOADS; i++) {
            if (ctx) await ctx.close();
            ctx = await browser.newContext();
            bytesLog = trackBytes(ctx);
            page = await ctx.newPage();
            cold.push(await measureLoad(page, url, i, bytesLog));
        }
        // Repeat visit: the last context's service worker has precached the
        // bundle by the time it is ready.
        // (`ready` never settles when nothing registers, hence the race.)
        const swReady = await page.evaluate(() => !navigator.serviceWorker ? false : Promise.race([
            navigator.serviceWorker.ready.then(() => true),
            new Promise(r => setTimeout(() => r(false), 15_000)),
        ]));
        // Let the install-time precache finish: wait until no request has
        // finished for 3 s (60 s cap), then everything a first visit fetched.
        for (let quiet = 0, seen = bytesLog.length, t = 0; quiet < 3 && t < 60; t++) {
            await page.waitForTimeout(1000);
            if (bytesLog.length === seen) quiet++; else { quiet = 0; seen = bytesLog.length; }
        }
        const firstVisit = bytesLog.slice();
        const atReady = cold[cold.length - 1].bytesAtReady || [];
        const download = {
            before_ready_bytes: sumBytes(atReady),
            before_ready_requests: atReady.length,
            first_visit_bytes: sumBytes(firstVisit),
            first_visit_requests: firstVisit.length,
            top_before_ready: topPaths(atReady),
            top_first_visit: topPaths(firstVisit),
            note: 'response body bytes as served by vite preview (uncompressed); ' +
                  'tools/ci/web-bundle-size.py has the gzip/brotli sizes',
        };
        const warm = await measureLoad(page, url, COLD_LOADS);
        warm.serviceWorkerReady = swReady;
        const lsp = await measureLsp(page, KEYSTROKES);
        await ctx.close();

        const b = (k) => median(cold.map(c => c.boot[k]));
        const keyMs = [...lsp.breakMs, ...lsp.fixMs];
        const metrics = {
            cold_ready_ms: b('statusReadyAt'),
            cold_first_result_ms: median(cold.map(c => c.boot.statusReadyAt + c.eval.clickToResultMs)),
            cold_monaco_ms: b('initWasmAt'),
            cold_worker_script_ms: b('script_ms'),
            cold_worker_module_ms: b('module_ms'),
            cold_worker_init_ms: b('init_ms'),
            first_eval_ms: median(cold.map(c => c.eval.clickToResultMs)),
            warm_ready_ms: warm.boot.statusReadyAt,
            warm_worker_module_ms: warm.boot.module_ms,
            lsp_boot_ms: lsp.boot.ms,
            lsp_keystroke_ms: median(keyMs),
            lsp_analysis_ms: median(keyMs.map(x => x - LSP_DEBOUNCE_MS)),
        };
        const phaseNames = Object.keys((cold[0].boot.phases) || {});
        const phases = Object.fromEntries(phaseNames.map(p => [p, median(cold.map(c => (c.boot.phases || {})[p]))]));

        const result = {
            schema: 'web-perf/1',
            ts: new Date().toISOString(),
            sha: process.env.GITHUB_SHA || null,
            cold_loads: COLD_LOADS,
            keystrokes: KEYSTROKES,
            lsp_debounce_ms: LSP_DEBOUNCE_MS,
            metrics,
            init_phases_ms: phases,
            download,
            raw: { cold: cold.map(({ bytesAtReady, ...c }) => c), warm, lsp },
        };
        mkdirSync(path.dirname(OUT), { recursive: true });
        writeFileSync(OUT, JSON.stringify(result, null, 2));

        const fmt = (v) => (v === null || v === undefined) ? '-' : `${v.toFixed(1)} ms`;
        console.log('web-perf (median of %d cold loads):', COLD_LOADS);
        for (const [k, v] of Object.entries(metrics)) console.log(`  ${k.padEnd(24)} ${fmt(v)}`);
        console.log('  turi_wasm_init phases:');
        for (const [k, v] of Object.entries(phases)) console.log(`    ${k.padEnd(22)} ${fmt(v)}`);
        const mb = (n) => `${(n / 1e6).toFixed(2)} MB`;
        console.log(`  downloaded before Ready  ${mb(download.before_ready_bytes)} in ${download.before_ready_requests} requests`);
        console.log(`  downloaded, first visit  ${mb(download.first_visit_bytes)} in ${download.first_visit_requests} requests`);
        for (const e of download.top_before_ready.slice(0, 8)) console.log(`    ${mb(e.bytes).padStart(9)}  ${e.path}`);
    } finally {
        stopServer();
    }
});
