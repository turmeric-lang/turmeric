// smoke.mjs -- the arcade module's smoke test (raylib-web-playground-plan
// P1): in Playwright's Chromium, on a cross-origin-isolated page, a raylib
// program in an iframe draws its first frame, its console line arrives while
// its frame loop runs, Stop (the iframe removed) leaves the page alone, and a
// program the engine refuses is reported as refused rather than handed to an
// interpreter that has no raylib.
//
//   node tools/wasm-jit/arcade/smoke/smoke.mjs MODULE_DIR
//
// MODULE_DIR holds turmeric-jit.{js,wasm} and stdlib.data
// (build-wasm/wasm-arcade/wasm-jit).  The page is served here, on a port the
// OS picks, with the COOP/COEP headers the pthreads module needs.  Playwright
// comes from web/node_modules (npm ci in web/), else from a global install;
// ARCADE_CHROMIUM names another Chromium (JSPI needs 137 or later).
import http from 'node:http';
import fs from 'node:fs';
import path from 'node:path';
import { createRequire } from 'node:module';
import { fileURLToPath } from 'node:url';

const HERE = path.dirname(fileURLToPath(import.meta.url));
const ROOT = path.resolve(HERE, '../../../..');
const MOD = process.argv[2] && path.resolve(process.argv[2]);
if (!MOD || !fs.existsSync(path.join(MOD, 'turmeric-jit.wasm'))) {
  console.error(`smoke.mjs: no turmeric-jit.wasm in ${MOD || '(no MODULE_DIR given)'}`);
  process.exit(2);
}

function loadPlaywright() {
  for (const [from, name] of [[path.join(ROOT, 'web/package.json'), '@playwright/test'],
                              [import.meta.url, 'playwright']]) {
    try { return createRequire(from)(name); } catch (_) { /* next */ }
  }
  console.error('smoke.mjs: no Playwright (run `npm ci` in web/)');
  process.exit(2);
}
const { chromium } = loadPlaywright();

const HOST = `<!doctype html><meta charset="utf-8"><title>arcade smoke</title>
<body><div id="host"></div><script>
window.lines = []; window.results = []; window.boots = [];
addEventListener('message', (e) => {
  const m = e.data || {};
  if (m.type === 'line') lines.push(m.text);
  else if (m.type === 'boot') boots.push(m);
  else if (m.type === 'result') results.push(m.answer);
  else if (m.type === 'ready') window.readyFrame && window.readyFrame(e.source);
});
window.run = (src) => {
  stop();
  lines = []; results = []; boots = [];
  const f = document.createElement('iframe');
  f.width = 480; f.height = 270;
  window.readyFrame = (w) => { if (w === f.contentWindow) w.postMessage({ type: 'run', src }, location.origin); };
  f.src = '/screen.html';
  document.getElementById('host').appendChild(f);
};
window.stop = () => { document.getElementById('host').replaceChildren(); };
</script>`;

const TYPES = { '.html': 'text/html', '.js': 'text/javascript', '.wasm': 'application/wasm',
                '.json': 'application/json' };
const server = http.createServer((req, res) => {
  const url = new URL(req.url, 'http://x');
  let file = null, body = null;
  if (url.pathname === '/') body = HOST;
  else if (url.pathname === '/screen.html') file = path.join(HERE, 'screen.html');
  else if (url.pathname.startsWith('/engine/')) {
    file = path.join(MOD, path.normalize(url.pathname.slice('/engine/'.length)));
    if (!file.startsWith(MOD + path.sep)) file = null;   // no ../ out of the module dir
  }
  if (file && !fs.existsSync(file)) file = null;
  if (!file && body === null) { res.writeHead(404); res.end(); return; }
  res.writeHead(200, {
    'Content-Type': file ? TYPES[path.extname(file)] || 'application/octet-stream' : 'text/html',
    'Cross-Origin-Opener-Policy': 'same-origin',
    'Cross-Origin-Embedder-Policy': 'require-corp',
    'Cross-Origin-Resource-Policy': 'same-origin',
  });
  res.end(body !== null ? body : fs.readFileSync(file));
});
await new Promise((r) => server.listen(0, '127.0.0.1', r));
const origin = `http://127.0.0.1:${server.address().port}`;

const HELLO = `(defmodule hello
  (export)
  (import raylib/core  :refer [init-window window-should-close begin-drawing
                               end-drawing clear-background])
  (import raylib/text  :refer [draw-text])
  (import raylib/color :refer [raywhite black])
  (defn main [] : int
    (init-window 480 270 "hello")
    (let [^mut n 0]
      (while (not (window-should-close))
        (begin-drawing)
        (clear-background (raywhite))
        (draw-text "Hello, Turmeric!" 120 120 20 (black))
        (end-drawing)
        (set! n (+ n 1))
        (when (= n 30) (println "frame 30"))))
    0))`;

// pthread_create is one of the imports the browser engine refuses
// (jit_engine.c, wasm-jit-plan 5.7): MIR_link fails before main runs.
const THREADS = `(defn start [] : int
  \`\`\`c
  pthread_t t;
  return pthread_create(&t, NULL, NULL, NULL);
  \`\`\`)
(defn main [] : int
  (println "main ran")
  (start))`;

// A defscript header is satisfied from the packages (J2): math is offered, so
// the header compiles and its import resolves; nope is not, so the engine
// says so.  7.1 + 3.25: a float that would show truncation.
const HEADER = (name) => `(defscript :spices #map{"${name}" #map{:url "https://github.com/turmeric-lang/turmeric-spices"
                                       :ref "math-v0.1.0"
                                       :subdir "spices/math"}})
(defmodule hdr
  (export)
  (import math/vec2 :refer [vec2 v2-add v2-x])
  (defn main [] : int
    (println (v2-x (v2-add (vec2 7.1 0.0) (vec2 3.25 1.0))))
    0))`;

let failed = 0;
const check = (ok, what, detail) => {
  console.log(`${ok ? 'PASS' : 'FAIL'} ${what}${detail ? ` -- ${detail}` : ''}`);
  if (!ok) failed++;
};

const launch = process.env.ARCADE_CHROMIUM ? { executablePath: process.env.ARCADE_CHROMIUM } : {};
const browser = await chromium.launch(launch);
try {
  const page = await browser.newPage();
  // Keep the WebGL drawing buffer, so the frame can be read back; raylib's
  // GLFW port builds its own context attributes.
  await page.addInitScript(() => {
    const orig = HTMLCanvasElement.prototype.getContext;
    HTMLCanvasElement.prototype.getContext = function (type, attrs) {
      if (type === 'webgl2' || type === 'webgl')
        attrs = Object.assign({}, attrs || {}, { preserveDrawingBuffer: true });
      return orig.call(this, type, attrs);
    };
  });
  await page.goto(origin + '/');
  const t0 = Date.now();
  await page.evaluate((s) => window.run(s), HELLO);
  await page.waitForFunction(() => window.boots.length > 0, null, { timeout: 60_000 });
  const boot = await page.evaluate(() => window.boots[0]);
  check(boot.ok && boot.jspi, 'the module boots in an iframe, with JSPI',
        boot.ok ? `${Math.round(boot.ms)} ms` : boot.error);
  check(String(boot.spices) === 'math,raylib', 'the program\'s spices are mounted from packages, deps first',
        boot.ok ? `mounted ${boot.spices.join(', ')} of ${boot.offered.join(', ')} in ${Math.round(boot.mountMs)} ms` : '');

  // The first frame: RAYWHITE, with the text's dark pixels in its box.
  const frame = page.frameLocator('#host iframe').locator('#canvas');
  const read = () => frame.evaluate((c) => {
    const gl = c.getContext('webgl2');
    if (!gl) return { error: 'no webgl2 context' };
    const bg = new Uint8Array(4);
    gl.readPixels(4, 4, 1, 1, gl.RGBA, gl.UNSIGNED_BYTE, bg);
    const w = 200, h = 24, px = new Uint8Array(w * h * 4);
    gl.readPixels(120, c.height - 120 - h, w, h, gl.RGBA, gl.UNSIGNED_BYTE, px);
    let dark = 0;
    for (let i = 0; i < px.length; i += 4) if (px[i] + px[i + 1] + px[i + 2] < 200) dark++;
    return { bg: [...bg].slice(0, 3), dark };
  });
  let f = { dark: 0 };
  for (let i = 0; i < 150 && !(f.dark > 50); i++) {
    await page.waitForTimeout(200);
    f = await read().catch((e) => ({ error: String(e), dark: 0 }));
  }
  const firstFrameMs = Date.now() - t0;
  check(f.dark > 50 && String(f.bg) === '245,245,245', 'the first frame is drawn (RAYWHITE, dark text)',
        f.error || `bg ${f.bg}, ${f.dark} dark pixels, ${firstFrameMs} ms from Run`);

  // The console line, printed by the blocking loop on frame 30: the loop
  // yields (raylib's sleep suspends under JSPI) and its output streams.
  await page.waitForFunction(() => window.lines.includes('frame 30'), null, { timeout: 30_000 })
    .then(() => check(true, 'a console line arrives while the frame loop runs'),
          () => check(false, 'a console line arrives while the frame loop runs'));
  const lines = await page.evaluate(() => window.lines);
  check(lines.some((l) => l.includes('Device initialized successfully')), "raylib's device line reaches the console");
  check((await page.evaluate(() => window.results.length)) === 0, 'the loop is still running (no answer yet)');

  // Stop: the iframe is the teardown, and the page carries on.
  await page.evaluate(() => window.stop());
  const after = await page.evaluate(() => ({ frames: document.querySelectorAll('iframe').length, sum: 1 + 1 }));
  check(after.frames === 0 && after.sum === 2, 'Stop removes the screen and the page stays live');

  // A refusal is the answer, not a fallback.
  await page.evaluate((s) => window.run(s), THREADS);
  await page.waitForFunction(() => window.results.length > 0, null, { timeout: 60_000 });
  const answer = await page.evaluate(() => window.results[0]);
  const out = await page.evaluate(() => window.lines);
  check(answer.engine === 'refused' && /starts threads/.test(answer.reason || '') && !out.includes('main ran'),
        'a program the engine refuses is reported as refused', JSON.stringify(answer));

  // A defscript header naming an offered spice: mounted, and the program runs.
  const runFor = async (src) => {
    await page.evaluate((s) => window.run(s), src);
    await page.waitForFunction(() => window.results.length > 0, null, { timeout: 60_000 });
    return page.evaluate(() => ({ answer: window.results[0], lines: window.lines, boot: window.boots[0] }));
  };
  let r = await runFor(HEADER('math'));
  check(r.answer.engine === 'compiled' && r.lines.includes('10.35') && String(r.boot.spices) === 'math',
        'a defscript header is satisfied from the mounted packages',
        `${JSON.stringify(r.answer)} out=${JSON.stringify(r.lines.slice(-2))}`);
  r = await runFor(HEADER('nope'));
  check(r.answer.engine === 'error' && /spice 'nope' is not in this build/.test(r.answer.reason || ''),
        'a defscript header naming a spice the build lacks is refused by name', JSON.stringify(r.answer));

  // The language server (P2): the stock module in lsp-worker.js, with every
  // offered spice mounted, analyses the quick start clean and flags an import
  // of a module no spice has.
  const lsp = await page.evaluate(async (srcs) => {
    const w = new Worker('/engine/lsp-worker.js');
    const next = () => new Promise((res) => w.addEventListener('message', (e) => res(e.data), { once: true }));
    w.postMessage({ type: 'init' });
    const boot = await next();
    if (boot.type !== 'ready') return { boot };
    let id = 0;
    const ask = async (request) => {
      w.postMessage({ type: 'lsp', id: ++id, request: JSON.stringify({ jsonrpc: '2.0', ...request }) });
      return (await next()).messages || [];
    };
    await ask({ id: 100, method: 'initialize', params: {} });
    const diags = async (uri, text) => {
      const out = await ask({ method: 'textDocument/didOpen',
                              params: { textDocument: { uri, languageId: 'turmeric', version: 1, text } } });
      const pub = out.find((m) => m.method === 'textDocument/publishDiagnostics' && m.params.uri === uri);
      return pub ? pub.params.diagnostics.map((d) => d.severity + ' ' + d.message) : null;
    };
    const r = { boot, hello: await diags('file:///hello.tur', srcs[0]),
                bad: await diags('file:///bad.tur', srcs[1]) };
    w.terminate();
    return r;
  }, [HELLO, HELLO.replace('raylib/text ', 'raylib/nope ')]);
  check(lsp.boot && lsp.boot.type === 'ready' && String(lsp.boot.spices) === 'raylib,math',
        'the language server boots with every offered spice mounted', JSON.stringify(lsp.boot));
  check(Array.isArray(lsp.hello) && lsp.hello.length === 0,
        'the quick start analyses clean (raylib imports resolve)', JSON.stringify(lsp.hello));
  check(Array.isArray(lsp.bad) && lsp.bad.some((d) => d.startsWith('1 ')),
        'an import of a module no spice has is flagged', JSON.stringify(lsp.bad));
} finally {
  await browser.close();
  server.close();
}
console.log(failed ? `smoke: ${failed} failed` : 'smoke: all passed');
process.exit(failed ? 1 : 0);
