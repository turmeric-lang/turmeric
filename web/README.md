# Try Turmeric Web

A web-based REPL for the [Turmeric programming language](https://github.com/turmeric-lang/turmeric) that runs entirely in the browser using WebAssembly.

## Features

- **WASM-powered Turmeric runtime** - Full Turmeric evaluation in the browser
- **Monaco Editor** - Professional code editing with syntax highlighting
- **Interactive Console** - ANSI-colored output with execution history
- **Code Examples** - Pre-loaded examples covering key language features
- **Language picker** - Dialect radio group + `#lang` layer toggles that edit
  the `#lang` line in place (the buffer stays the source of truth); options
  are rendered from the runtime's registry export, never hardcoded in JS
- **URL Sharing** - Share your code via compressed URL hash
- **Editor intelligence** - Diagnostics, completion, hover, signature help,
  go-to-definition, and document symbols, served by the real `tur lsp` running
  in a WebAssembly worker
- **Tutorial System** - Guided learning with interactive tutorials (CLI only for now)
- **Responsive Design** - Works on desktop and mobile devices

## Quick Start

### Development

1. Install dependencies:
   ```bash
   cd web
   npm install
   ```

2. Build the WASM module (requires [Emscripten](https://emscripten.org)):
   ```bash
   tur run configure-wasm
   tur run wasm
   ```

3. Run the development server:
   ```bash
   tur run web-dev
   ```

4. Open [http://localhost:3000](http://localhost:3000) in your browser.

#### The service worker is off on localhost

`sw.js` is cache-first for same-origin static assets, and its precache names
`/main.js` and `/styles.css` -- the files a dev server exists to re-serve. A
worker left installed by one session hands the next one the previous session's
JS and CSS, which shows up as an unstyled page running code you already
changed, and needs a hard reload every time.

So on a loopback host the app does not register the worker, and unregisters
any it finds (dropping their caches) before reloading once to pick up the real
files. Nothing to do -- the first load after this change cleans up whatever
was already installed.

To work on the PWA itself, append `?sw=1`:

```
http://localhost:3000/try/?sw=1
```

That is also how `tests/mobile.split-and-pwa.spec.js` and
`tests/docs-offline.spec.js` still exercise the real worker. Production is
unaffected: the opt-out keys off the hostname.

### Production Build

```bash
tur run web
```

This will:
1. Build the WASM module
2. Install web dependencies
3. Create a production-optimized bundle in `web/dist/`

#### What the bundle costs a visitor

Two measurements run against the production build (performance-audit-plan
WP5), in CI's web-smoke job and by hand:

```bash
python3 tools/ci/web-bundle-size.py      # from the repo root: raw / gzip / brotli sizes
cd web && WEB_PERF=1 npx playwright test --project=desktop tests/web-perf.spec.js
```

The spec serves `dist/client` itself on a free port and times cold loads to
"Ready", the first evaluation, a repeat visit and LSP keystroke latency, from
the boot marks the page publishes as `window._turiBoot` (the eval worker's
stage times and `turi_wasm_init`'s phase table, `_turi_wasm_boot_timings`).
It writes `test-results/web-perf.json` and asserts only that the page works;
without `WEB_PERF=1` it skips, so the ordinary suite does not pay for it.

### Deploy

```bash
tur run migrate-ci-db   # only when a pull brought a new web/migrations/ file
tur run deploy-web
```

`deploy-web` rebuilds the wasm and the bundle and deploys the Worker to
Cloudflare. `migrate-ci-db` applies any pending migration to the production
D1 database behind `/ci` (below), and has to go first: a Worker that writes a
column the database does not have yet fails every CI report. With nothing
pending it prints "No migrations to apply!" and exits.

## The /ci dashboard and its D1 store

`/ci` (`ci/index.html`, `ci-metrics.js`) reads `/api/ci/*`, which `worker.js`
hands to `ci-api.js`. That module keeps a Cloudflare D1 database, `ci-metrics`
(binding `CI_DB` in `wrangler.jsonc`, schema in `migrations/`), and CI writes
each push's rows into it through `POST /api/ci/report` with
`tools/ci/report-metrics.py`. `ci-aggregate.js` holds the data rules (env
order, series names, daily medians, the test-count carry-forward) that the
Worker, the page and the page tests all share. Design and rationale:
`docs/upcoming/ci-metrics-d1-plan.md`.

Every read except `/api/ci/meta` carries the newest ingest id (`?v=`). Those
responses are immutable, and a stale `v` is redirected to the current one.

Locally, `npm run dev` runs the Worker against a local D1 under
`.wrangler/state`. To see the page with data:

```bash
npx wrangler d1 migrations apply ci-metrics --local
echo 'CI_METRICS_TOKEN=dev' > .dev.vars      # the report endpoint's token
npx vite --port 3100 --strictPort           # any port but 3000
# From the repo root, with a checkout of the ci-metrics branch in /tmp/cim:
CI_METRICS_TOKEN=dev CI_METRICS_URL=http://localhost:3100/api/ci/report \
  python3 tools/ci/report-metrics.py --backfill /tmp/cim
```

The Playwright run does not touch that database: `tests/ci-d1-setup.mjs` gives
its server a fresh one under `.wrangler/ci-test-state-<port>` and a test token
(see `playwright.config.js`).

In production, a schema change is a new file in `migrations/`, applied with
`tur run migrate-ci-db` (`npx wrangler d1 migrations apply ci-metrics
--remote`, run from `web/`) **before** deploying code that needs it. To see
what is pending without applying it, run
`npx wrangler d1 migrations list ci-metrics --remote` from `web/`. The report token is a Worker secret
(`wrangler secret put CI_METRICS_TOKEN`) and a repository secret of the same
name.

## Project Structure

```
web/
├── index.html          # Main HTML page
├── main.js             # Main JavaScript with WASM integration
├── styles.css          # Styling
├── package.json        # Node dependencies
├── vite.config.js      # Vite build configuration
├── lsp-client.js       # Monaco adapter for the language server
└── public/
    ├── turmeric.js      # Compiled WASM module (generated)
    ├── eval-worker.js   # Worker hosting the interpreter
    └── lsp-worker.js    # Worker hosting the language server
```

## Configuration

The web REPL can be configured via the `CONFIG` object in `main.js`:

```javascript
const CONFIG = {
    DEFAULT_CODE: `(println "Hello, Turmeric!")`,
    EXECUTION_TIMEOUT: 30000,   // the eval watchdog's limit, in ms
    MAX_OUTPUT_LENGTH: 10000
};
```

`EXECUTION_TIMEOUT` is how long one request may keep the eval worker busy. A
**Stop** button appears after a second; at the limit the page stops the worker
itself. Either way the worker is terminated and a fresh one booted, so the
session's definitions are lost (`stopEvalWorker` in `main.js`).

## Content-Security-Policy

Every response the site serves carries the policy in `csp.js`. It is applied in
three places from that one definition: the dev and preview servers
(`vite.config.js`), the built `dist/client/_headers` (stamped at build time;
`public/_headers` deliberately does not carry it), and every response
`worker.js` produces itself.

`script-src` has no `'unsafe-inline'`, so:

- no inline `<script>` and no `on...=` attribute, in hand-written pages or in
  the doc generators' output -- ship a file (`public/pwa-shell.js` is the
  pattern for something that must run before first paint);
- no script from another origin that the policy does not name. It names
  mermaid's jsDelivr path, and nothing else.

`tests/security.spec.js` loads every shipped page with the policy enforced and
fails on any violation, so a change that breaks it is caught there. Each
allowance in `csp.js` carries its reason; remove one only with it.

## Keyboard Shortcuts

| Shortcut | Action |
|----------|--------|
| `Ctrl+Enter` | Run code |
| `Ctrl+S` | Save to URL hash |

## Tutorial System

The tutorial system provides interactive learning. In the CLI REPL, use:

```
:tutorial              # List available tutorials
:tutorial basics      # Start the basics tutorial
:next                 # Go to next step
:prev                 # Go to previous step
:hint                 # Show hint for current step
:skip                 # Skip current step
:quit-tutorial        # Exit tutorial mode
```

The web version will have tutorial integration in a future update.

## Technical Details

### WASM API

The WASM module exposes the following functions:

- `turi_wasm_init()` - Initialize the runtime
- `turi_wasm_reset()` - Reset the evaluation environment
- `turi_wasm_rewind_to_prelude()` - Forget everything evaluated since the
  stdlib preload (source, elaboration session and runtime bindings), keeping
  the env and its stdlib; what Run does first
- `turi_wasm_shutdown()` - Shutdown the runtime
- `turi_wasm_eval(input)` - Evaluate code and return result as string
- `turi_wasm_eval_ex(input, out_result, out_error)` - Evaluate with separate result/error
- `turi_wasm_version()` - Get version string
- `turi_wasm_set_lang(name)` - Set the base dialect from a `#lang` directive
  tail (e.g. `"turmeric/sweet"`, `"saffron"`); resets the session when it
  changes, and rejects the call outright if anything trails the base name
- `turi_wasm_get_lang()` - Current base dialect (canonical spelling)
- `turi_wasm_lang_registry()` - JSON registry of the base dialects, exported
  from the C base axis for the language picker
- `turi_wasm_lsp_request(json)` - Feed one JSON-RPC message to the language
  server; returns a JSON array of the messages it produced
- `turi_wasm_lsp_flush()` - Analyze documents with pending edits
- `turi_wasm_lsp_reset()` - Drop every open document and start a fresh session

### Run and the session

The editor and the prompt share one evaluation session, but Run means "run
this program", not "append this program to the session":

1. The session rewinds to the preloaded stdlib (`turi_wasm_rewind_to_prelude`).
2. Every other tab's last successful Run is replayed, in tab order, with its
   output muted -- that is how one tab's definitions stay callable from another
   tab and from the prompt. Only tabs in the running tab's `#lang` dialect are
   replayed; a dialect change resets the session anyway.
3. The editor's program runs.

So pressing Run twice gives the same result both times, and a definition
deleted from the program stops resolving. Definitions typed at the prompt last
until the next Run. `:reset` rebuilds the environment from scratch.

The native checks for this are `tur_wasm_glue_session_unit`,
`tur_session_redefinition` and `tur_session_failure_recovery`; the page-level
ones are in `tests/run-session.spec.js`.

### Language server

The playground runs the same `tur lsp` the native editors use. No language
intelligence is implemented in JavaScript; `lsp-client.js` translates between
Monaco's provider APIs and JSON-RPC, and that is all it does.

It lives in its own worker with its own WASM instance, separate from the
evaluator. Analysis and evaluation must not queue behind each other -- a
three-second loop should not block completion -- and `turi_wasm_reset` tears
the eval environment back to prelude, which a server sharing that instance
would notice. Instantiation is lazy, on first editor focus: it is the same
`/turmeric.js` URL so it is a cache hit rather than a second download, but the
memory is real and a visitor who only reads code should not pay for it.

Every open tab is an open document, which is what makes `workspace/symbol` and
cross-tab go-to-definition work here without the filesystem crawl a native
client needs.

**If the server does not come up** -- an old cached `turmeric.wasm` without the
LSP exports, a browser with no `SharedArrayBuffer`, a worker that throws --
every provider returns empty, markers stay clear, and the footer indicator
hides itself. The playground is exactly as usable as it was before any of this
existed, which is the contract `web/tests/lsp.spec.js` checks first.

Rebuilding the bundle with the LSP exports:

```bash
cmake -S .. -B ../build-wasm -DTUR_WASM=ON -DCMAKE_BUILD_TYPE=Release
cmake --build ../build-wasm --target tur_wasm     # copies into web/public/
```

### Monaco Editor Configuration

The editor is configured with:
- Turmeric language support with custom syntax highlighting
- Light and dark themes that match system preferences
- Auto-closing brackets and quotes
- Line numbers and cursor position display
- Word wrap and smooth scrolling

## License

MIT License - see the main [Turmeric LICENSE](https://github.com/turmeric-lang/turmeric/blob/main/LICENSE) for details.
