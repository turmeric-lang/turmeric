// Try Turmeric service worker.
//
// Strategy:
//   - Precache the PWA shell (/try/), the heavy WASM/JS assets, and the whole
//     docs pack on install.
//   - Runtime: cache-first for same-origin static assets (with background
//     revalidation) and for /docs-pack/*, network-first for HTML navigations
//     and /docs/*.
//
// The docs pack is precached UNCONDITIONALLY. There is no "download docs for
// offline" toggle and no opt-in: installing Try Turmeric means having the
// docs, the same way it already means having the compiler. A toggle would be
// one more state to explain, test, and get wrong, and it would make the
// in-app docs pane a feature that sometimes has no content. The pack's size
// budget (enforced at generation time by tools/genpack.py) is what keeps that
// affordable -- if the pack ever outgrows it, the answer is a smaller pack,
// never a switch. See docs/archive/offline-docs-plan.md (OD3).
//
// CACHE_VERSION must change on every DEPLOY so `activate` evicts the old caches
// AND the changed sw.js bytes make the browser re-install the worker --
// otherwise a cache-first asset (turmeric.js / turmeric.wasm) is served from a
// stale precache forever, no matter how much newer the deploy is.
//
// Every deploy, not every release: keying the token on VERSION alone made an
// out-of-band fix invisible to exactly the people who had visited before. The
// Saffron language-picker fix deployed green and the live site kept rendering
// the four stale bases, because the cache was still `tur-try-v1-0.45.0` from
// the release cut an hour earlier.
//
// The token below is rewritten at build time by the `injectSwVersion` plugin in
// vite.config.js, which stamps `tur-try-v1-<VERSION>-<short-sha>` into
// dist/client/sw.js. The literal here is the dev/no-build fallback; keep its
// version half in sync with VERSION so an un-built serve is still correct --
// and note that until 2026-08-26 the plugin looked in dist/sw.js, which the
// Cloudflare plugin does not write, so this literal was the *only* thing
// keeping the version right.
const CACHE_VERSION = 'tur-try-v1-0.63.2';
const PRECACHE = `${CACHE_VERSION}-precache`;
const RUNTIME  = `${CACHE_VERSION}-runtime`;

const PRECACHE_URLS = [
    '/try/',
    // The shell's blocking <head> script (pwa-shell.js). It lives in public/, so
    // it is not an /assets/ reference precacheShellAssets would find.
    '/pwa-shell.js',
    '/main.js',
    '/styles.css',
    '/site.css',
    '/site.js',
    '/turmeric.js',
    '/turmeric.wasm',
    // The REPL's own workers. Nothing else names them -- they are constructed
    // from a string literal in main.js / lsp-client.js, so precacheShellAssets,
    // which mines the shell's markup, cannot see them. Without these two rows
    // an offline visit loads the page and then cannot evaluate anything, which
    // makes "installed means you have the compiler" quietly untrue.
    // Precaching them also keeps a controlled reload off the network path that
    // WebKit breaks; see fetchFromNetwork below.
    '/eval-worker.js',
    '/lsp-worker.js',
    '/doc-names.json',
    '/favicon.svg',
    '/logo.svg',
    '/logo-icon.svg',
    '/manifest.webmanifest',
    '/icons/icon-192.png',
    '/icons/icon-512.png',
    '/icons/apple-touch-icon.png',
];

// The shell page whose asset graph we mine at install time (see
// precacheShellAssets). Its built <script>/<link> tags name the hashed bundles
// that PRECACHE_URLS above cannot: /main.js and /styles.css are the dev-server
// paths, and a production build serves /assets/try-<hash>.js instead.
const SHELL_URL = '/try/';

const DOCS_PACK_BASE = '/docs-pack';
const DOCS_PACK_INDEX = `${DOCS_PACK_BASE}/index.json`;

// Synthetic URL. Not a real asset -- a cache entry the worker writes and the
// docs pane reads back with a plain fetch, so the pane can say "docs ready for
// offline" or "N of M pages cached" instead of discovering a hole mid-flight.
const PACK_STATUS_URL = `${DOCS_PACK_BASE}/pack-status`;

/** Fetch one URL into `cache`. Returns true when it landed. */
async function cacheOne(cache, url) {
    try {
        const res = await fetch(url, { cache: 'reload' });
        if (res && res.ok) {
            await cache.put(url, res.clone());
            return true;
        }
    } catch (_) { /* offline-only first load, or a genuinely missing file */ }
    return false;
}

/**
 * Precache the hashed bundles the shell page actually loads.
 *
 * A brand-new worker does not control the page that installed it -- the first
 * visit's asset requests were already in flight, uncontrolled, before
 * `clients.claim()` ran, so runtime caching never saw them. Without this the
 * app only survives going offline if the reader happened to load it twice,
 * which makes "installed means you have it" quietly conditional on a coin
 * flip. Read the shell's own markup for its /assets/ references rather than
 * having the build inject a list into this file: hashed names then cannot
 * drift out of sync with what shipped.
 */
async function precacheShellAssets(cache) {
    let html;
    try {
        const res = await fetch(SHELL_URL, { cache: 'reload' });
        if (!res || !res.ok) return;
        await cache.put(SHELL_URL, res.clone());
        html = await res.text();
    } catch (_) {
        return;
    }
    const refs = new Set();
    for (const m of html.matchAll(/(?:src|href)="(\/assets\/[^"]+)"/g)) refs.add(m[1]);
    await Promise.all([...refs].map((url) => cacheOne(cache, url)));

    // One level deeper: a bundled stylesheet names its own assets -- the
    // webfonts, mostly -- with url(/assets/...), and those are not in the HTML.
    // Without them an offline page renders in system fonts, which looks like
    // something is broken even though everything works.
    const nested = new Set();
    for (const url of refs) {
        if (!url.endsWith('.css')) continue;
        const hit = await cache.match(url);
        if (!hit) continue;
        const css = await hit.text();
        for (const m of css.matchAll(/url\(["']?(\/assets\/[^"')]+)["']?\)/g)) {
            if (!refs.has(m[1])) nested.add(m[1]);
        }
    }
    await Promise.all([...nested].map((url) => cacheOne(cache, url)));
}

/**
 * Precache the docs pack, driven by index.json's own `files` list.
 *
 * PRECACHE_URLS cannot name 281-odd fragments statically, and a second
 * hand-maintained manifest would drift from the pack. index.json already
 * enumerates exactly what the pack contains, so it doubles as the precache
 * manifest: a page that is in the pack is precached by construction.
 *
 * Per-URL and failure-tolerant, like the shell precache above -- one 404 must
 * not brick the worker. What that tolerance costs is certainty, so record what
 * actually landed and let the pane surface a partial pack rather than pretend.
 */
async function precacheDocsPack(cache) {
    // Refresh the index if the network allows, but fall back to the cached
    // copy: this also runs on `activate`, which happens offline, and a failed
    // refresh must not be mistaken for "there is no pack".
    await cacheOne(cache, DOCS_PACK_INDEX);
    const indexRes = await cache.match(DOCS_PACK_INDEX);
    if (!indexRes) {
        await writePackStatus(cache, { expected: 0, cached: 0, version: null,
                                       missing: [], reason: 'index-unavailable' });
        return;
    }

    let index;
    try {
        index = await indexRes.json();
    } catch (_) {
        await writePackStatus(cache, { expected: 0, cached: 0, version: null,
                                       missing: [], reason: 'index-unparseable' });
        return;
    }

    const files = Array.isArray(index.files) ? index.files : [];
    const missing = [];
    // Bounded concurrency: hundreds of parallel fetches on a phone is a way to
    // make the first load worse, and the shell is already cached by now so the
    // REPL is usable while this finishes.
    const queue = files.slice();
    const workers = Array.from({ length: 6 }, async () => {
        while (queue.length) {
            const rel = queue.shift();
            const url = `${DOCS_PACK_BASE}/${rel}`;
            if (await cache.match(url)) continue;
            if (!await cacheOne(cache, url)) missing.push(rel);
        }
    });
    await Promise.all(workers);

    await writePackStatus(cache, {
        expected: files.length,
        cached: files.length - missing.length,
        version: index.version || null,
        // Capped: the pane only needs to know *that* the pack is short, and an
        // unbounded list of a few hundred paths is not worth caching.
        missing: missing.slice(0, 25),
    });
}

async function writePackStatus(cache, status) {
    await cache.put(PACK_STATUS_URL, new Response(JSON.stringify(status), {
        headers: { 'Content-Type': 'application/json' },
    }));
}

/**
 * Re-attempt whatever the install could not fetch.
 *
 * An install is per-URL failure-tolerant, so a flaky first load can leave the
 * pack short. Retry on the next load rather than waiting for a version bump --
 * an unconditional guarantee that quietly waits a release to become true is
 * not unconditional.
 */
async function repairDocsPack() {
    const cache = await caches.open(PRECACHE);
    const statusRes = await cache.match(PACK_STATUS_URL);
    if (!statusRes) return;
    let status;
    try { status = await statusRes.json(); } catch (_) { return; }
    if (status.cached >= status.expected && status.expected > 0) return;
    await precacheDocsPack(cache);
}

self.addEventListener('install', (event) => {
    event.waitUntil((async () => {
        const cache = await caches.open(PRECACHE);
        // addAll is atomic -- if any URL 404s the whole install fails. Add
        // them individually so a missing optional file doesn't brick the SW.
        await Promise.all(PRECACHE_URLS.map((url) => cacheOne(cache, url)));
        await precacheShellAssets(cache);
        // The shell is cached first and the pack after, so the REPL is usable
        // before the docs finish landing.
        await precacheDocsPack(cache);
        await self.skipWaiting();
    })());
});

self.addEventListener('activate', (event) => {
    event.waitUntil((async () => {
        const keys = await caches.keys();
        await Promise.all(keys
            .filter((k) => k !== PRECACHE && k !== RUNTIME)
            .map((k) => caches.delete(k)));
        await self.clients.claim();
        // Best-effort top-up for a previous install that came up short.
        repairDocsPack().catch(() => {});
    })());
});

self.addEventListener('message', (event) => {
    if (event.data === 'SKIP_WAITING') self.skipWaiting();
    if (event.data === 'REPAIR_DOCS_PACK') {
        event.waitUntil?.(repairDocsPack().catch(() => {}));
    }
});

const isHtmlNavigation = (request) =>
    request.mode === 'navigate' ||
    (request.method === 'GET' &&
     request.headers.get('accept') &&
     request.headers.get('accept').includes('text/html'));

self.addEventListener('fetch', (event) => {
    const request = event.request;
    if (request.method !== 'GET') return;

    const url = new URL(request.url);
    if (url.origin !== self.location.origin) return;

    // pack-status is written by this worker, not served by the origin. Answer
    // it straight from the cache so it never costs a doomed network round-trip.
    if (url.pathname === PACK_STATUS_URL) {
        event.respondWith((async () => {
            const hit = await caches.match(PACK_STATUS_URL);
            return hit || new Response('{}', {
                status: 404, headers: { 'Content-Type': 'application/json' } });
        })());
        return;
    }

    // The docs pack is cache-first with background revalidation, not
    // network-first: it is versioned by the SW cache name, CACHE_VERSION
    // already rotates per release, and the alternative is a docs pane that
    // waits on a network timeout for every page on a bad connection.
    if (url.pathname.startsWith(`${DOCS_PACK_BASE}/`)) {
        event.respondWith(cacheFirst(request));
        return;
    }

    // Network-first for HTML navigations and /docs/* so doc pages stay fresh.
    if (isHtmlNavigation(request) || url.pathname.startsWith('/docs/')) {
        event.respondWith(networkFirst(request));
        return;
    }

    // Cache-first for everything else (assets, wasm).
    event.respondWith(cacheFirst(request));
});

/**
 * The offline fallback for a /docs/html/* page.
 *
 * Serving the REPL shell for a docs URL -- the old behaviour -- reads as a
 * bug: the reader asked for a guide and got a code editor. Point them at the
 * copy that is actually on their device instead.
 */
async function docsOfflinePage(url) {
    const ref = packRefForDocsUrl(url.pathname);
    const target = ref ? `/try/#doc=${ref}` : '/try/';
    const what = ref ? 'This page is available in Try Turmeric'
                     : 'The documentation is available in Try Turmeric';
    return new Response(`<!DOCTYPE html>
<html lang="en"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Offline | Turmeric Docs</title>
<style>
  body { margin:0; min-height:100vh; display:flex; align-items:center;
         justify-content:center; background:#0C0A08; color:#EAE0D2;
         font-family:'DM Sans',system-ui,sans-serif; padding:2rem; }
  .card { max-width:34rem; text-align:center; }
  h1 { color:#EFA030; font-size:1.4rem; margin:0 0 .75rem; }
  p { color:#8A7D6E; line-height:1.6; margin:0 0 1.5rem; }
  a { display:inline-block; padding:.6rem 1.2rem; color:#0C0A08; background:#D48B1C;
      border-radius:6px; text-decoration:none; font-weight:500; }
</style></head>
<body><div class="card">
  <h1>You're offline</h1>
  <p>${what} &mdash; its documentation is stored on this device and works with no
     connection.</p>
  <a href="${target}">Open the docs in Try Turmeric</a>
</div></body></html>`, {
        status: 200,
        headers: { 'Content-Type': 'text/html; charset=utf-8' },
    });
}

/** '/docs/html/guides/hkt-guide.html' -> 'guides/hkt-guide', or null. */
function packRefForDocsUrl(pathname) {
    let m = pathname.match(/^\/docs\/html\/(guides|api)\/([A-Za-z0-9_\-]+)\.html$/);
    if (m) return `${m[1]}/${m[2]}`;
    m = pathname.match(/^\/docs\/html\/spices\/([A-Za-z0-9_\-]+)\/?$/);
    if (m) return `spices/${m[1]}`;
    return null;
}

/**
 * Fetch `request` from the network, around a WebKit defect.
 *
 * WebKit -- real Safari, not just Playwright's build; confirmed by hand on
 * Safari 27.0 -- rejects `fetch(request)` inside a service worker with
 * `TypeError: Load failed` when `request` is a top-level dedicated worker
 * script (`destination: 'worker'`) that the browser already holds in its HTTP
 * cache. Chromium serves the identical request without complaint.
 *
 * That pairing is a returning visitor. The first, uncontrolled load fetches
 * /eval-worker.js over the network and the HTTP cache keeps it; on a later
 * controlled load the same request arrives here, throws, `respondWith`
 * rejects, `new Worker()` fails, and the REPL says "Failed to load WASM" --
 * with advice to refresh that cannot help, because the refresh is controlled
 * too.
 *
 * It is the `destination` carried on the Request object that trips it:
 * `new Request(req)` and `fetch(req, { cache: 'reload' })` fail the same way,
 * while re-issuing the bare URL -- which carries no worker destination --
 * succeeds on every engine. So retry that way, and only for worker
 * destinations: a blanket retry would add a second doomed round-trip to every
 * genuinely offline request.
 *
 * See docs/archive/webkit-sw-controlled-reload-fails-wasm-init.md.
 */
function isWorkerScript(request) {
    return request.destination === 'worker' || request.destination === 'sharedworker';
}

async function fetchFromNetwork(request) {
    try {
        return await fetch(request);
    } catch (err) {
        if (!isWorkerScript(request)) throw err;
        return await fetch(request.url, { credentials: 'same-origin' });
    }
}

async function networkFirst(request) {
    const cache = await caches.open(RUNTIME);
    try {
        const fresh = await fetch(request);
        if (fresh && fresh.ok) cache.put(request, fresh.clone());
        return fresh;
    } catch (e) {
        const cached = await cache.match(request) || await caches.match(request);
        if (cached) return cached;
        const url = new URL(request.url);
        if (url.pathname.startsWith('/docs/')) return docsOfflinePage(url);
        const shell = await caches.match('/try/');
        if (shell) return shell;
        throw e;
    }
}

async function cacheFirst(request) {
    const cached = await caches.match(request);
    if (cached) {
        // Background revalidate; ignore failure (offline).
        fetchFromNetwork(request).then(async (res) => {
            if (res && res.ok) {
                const cache = await caches.open(RUNTIME);
                cache.put(request, res.clone());
            }
        }).catch(() => {});
        return cached;
    }
    try {
        const fresh = await fetchFromNetwork(request);
        if (fresh && fresh.ok) {
            const cache = await caches.open(RUNTIME);
            cache.put(request, fresh.clone());
        }
        return fresh;
    } catch (e) {
        const fallback = await caches.match('/try/');
        if (fallback && isHtmlNavigation(request)) return fallback;
        throw e;
    }
}
