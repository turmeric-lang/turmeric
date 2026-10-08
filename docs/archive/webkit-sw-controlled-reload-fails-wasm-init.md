# Try Turmeric fails to boot on a WebKit reload once the service worker controls the page

**Severity:** medium, and **high if it reproduces on real iOS Safari** --
confirming that is step 1 below. The break is on the *second* load, so it hits
returning visitors rather than first-time ones, and the message it shows them
("Please refresh the page") is advice that cannot work: the service worker
still controls the reload.

**Status:** RESOLVED 2026-09-09 -- see the resolution at the bottom, which
corrects several of the guesses below. The original text is kept as filed.

**Severity, settled:** the *symptom* is **test-environment only** -- it does
not reproduce against the built site. The *WebKit defect underneath it* is
real and reproduces on real macOS Safari.

Invisible until 2026-08-28. The `mobile` Playwright project is
`devices['iPhone 13']`, which is WebKit, but CI installed only Chromium, so all
32 mobile tests died at `browserType.launch: Executable doesn't exist at
.../webkit-*/pw_run.sh` before any test body ran -- and the step's
`continue-on-error` hid it. Installing WebKit
(try-turmeric-navigation-and-minimap-plan, section 10.5) turned 32 launch
errors into 30 passes and these 2 real failures.

## Repro

```
npx playwright test --project=mobile tests/mobile.split-and-pwa.spec.js
```

Both failures are in `web/tests/mobile.split-and-pwa.spec.js`, and both have
the same shape -- first load fine, reload broken:

| Line | Test |
| --- | --- |
| 25 | split fraction persists across reload |
| 44 | editor buffer persists across reload |

```
Locator:  locator('#wasm-status-text')
Expected: "Ready"
Received: "Failed to load WASM"

  3 x locator resolved to <span id="wasm-status-text">Initializing WASM...</span>
  1 x locator resolved to <span id="wasm-status-text">Loading WASM module...</span>
 59 x locator resolved to <span id="wasm-status-text">Failed to load WASM</span>

  > 53 |  await expect(page.locator('#wasm-status-text')).toHaveText('Ready', ...)
```

Both reach `Ready` on the `goto` at the top of the test (`gotoTry`,
`mobile.split-and-pwa.spec.js:3`) and only fail after `page.reload()`.

Observed on CI run
[33151156197](https://github.com/turmeric-lang/turmeric/actions/runs/33151156197),
job `Try Turmeric smoke test (browser)`, head `9a6ee5c7`.

## What is established

1. **Not the SharedArrayBuffer path.** "Failed to load WASM" is the `else`
   branch at `web/main.js:1178`. The `shared-array-buffer-unavailable` branch
   two lines up (`web/main.js:1168`) prints a different message and did not
   fire, so cross-origin isolation survived the reload and `SharedArrayBuffer`
   was present. This rules out the obvious first guess.

2. **The worker got as far as trying.** The status went
   `Initializing WASM...` -> `Loading WASM module...` -> `Failed to load WASM`,
   so `eval-worker.js` was created and ran. By elimination the `init-error` it
   posted was one of `web/public/eval-worker.js:167`
   (`TurmericModule not found after importScripts`), `:175`
   (`turi_wasm_init failed: <n>`), or `:182` (`String(err)` from the module
   factory). Which one is unknown -- see step 1 below.

3. **The service worker itself is fine.** `service worker registers and
   precaches WASM` (`mobile.split-and-pwa.spec.js:59`) **passed** on the same
   run: `navigator.serviceWorker.ready` resolved with an active worker, and
   `/turmeric.wasm` was found in a cache.

4. **Chromium is unaffected.** The desktop suite's 87 tests include
   reload-based ones (`minimap.spec.js` "the toggle turns it off, and the
   choice survives a reload") and pass.

5. **First load vs reload is exactly the uncontrolled/controlled boundary.**
   `web/public/sw.js:82-88` states it: a brand-new worker does not control the
   page that installed it, because that page's asset requests were already in
   flight before `clients.claim()` ran. So on the first load `/turmeric.js`
   and `/turmeric.wasm` come from the network, and on the reload they come from
   `cacheFirst` (`web/public/sw.js:344`) out of the Cache API.

That last point is the whole finding: **the failure appears exactly when the
wasm assets are replayed from the Cache API instead of fetched.**

## Candidate mechanisms -- none confirmed

- **Content-Type lost on the Cache API round-trip.**
  `WebAssembly.instantiateStreaming` rejects anything not served as
  `application/wasm`. Emscripten normally falls back to `arrayBuffer()`, so
  this alone should not be fatal -- unless the fallback is also failing.
- **A COEP/CORP judgement on a worker-provided response.** The document is
  `Cross-Origin-Embedder-Policy: require-corp` (`web/public/_headers`). A
  same-origin response needs no CORP, but a service-worker-replayed response
  may be treated differently by WebKit than by Blink.
- **A truncated body.** Both `cacheOne` (`sw.js:68`) and `cacheFirst`
  (`sw.js:344`) do `cache.put(..., res.clone())` while returning or discarding
  the original. `turmeric.wasm` is several MB; a clone-then-put that stores a
  short body would produce exactly this -- a module that fetches but will not
  instantiate. Testable: compare `(await caches.match('/turmeric.wasm')).blob()`
  size against `Content-Length`.

## Fix directions

1. **Get the actual error first.** ~~The CI logs carry no browser console.~~
   **Done 2026-09-02:** `mobile.split-and-pwa.spec.js` now collects
   `page.on('console')` / `page.on('pageerror')` for every test and, on a
   failure, attaches the dump plus the page's `#console` text to the
   Playwright report (`browser-console` attachment) and echoes it into the
   job log, so the next CI run names the `init-error` (`main.js:1164` has
   already `console.error`'d the real one). Everything below is guesswork
   until that run is read.
2. **Confirm on real Safari.** Playwright's WebKit is not Safari -- it is
   WebKit built for Linux, and service-worker plus COEP behaviour is exactly
   the area where the two can differ. If real iOS Safari reproduces it, this
   is high severity and a shipping bug for every returning mobile visitor. If
   it does not, it is a test-environment issue and should be recorded as one.
3. **Check the cached body length** per the third candidate above.
4. If it is the Cache API replay, the narrow fix is to exclude
   `/turmeric.wasm` and `/turmeric.js` from `cacheFirst` on WebKit, or to
   revalidate their length before serving. Do not reach for disabling the
   service worker: offline support is the feature it exists for
   (`docs/archive/` offline-docs work), and the failure is on one engine.

## What this is not

Not caused by the navigation/minimap work; that change installed WebKit in CI
and so revealed it, but these two tests were failing (as launch errors) before
it.

Nor is it a regression, as far as the history goes. `web/public/sw.js` and
`web/tests/mobile.split-and-pwa.spec.js` arrived in the same merge (#759), and
`playwright install` in `.github/workflows/ci.yml` named only `chromium` from
then until `9a6ee5c7`. So these two tests have never once executed, on any
head, and there is no green run of them to bisect back to. Whatever this is,
it has been true for the whole life of the service worker.

## Seen again 2026-09-01 / triage is cheap now (2026-09-02)

The same two failures were re-observed in the browser job on run
33460242737 while resolving
[try-turmeric-browser-suites-green-while-failing](../archive/try-turmeric-browser-suites-green-while-failing.md)
(a duplicate report filed from that log was folded into this one). What
changed on the CI side makes step 1 cheaper: the browser job now always
uploads the `playwright-report` artifact (the `error-context.md` for each
failure is in it), and the `web_mobile` row on `/ci` carries the failure
count, so a regression or a fix shows as a trend rather than a log dig. The
desktop `sw-dev` / `docs-offline` specs pass, so it is not a general
service-worker fault; the mobile project may simply be the one place a
controlled reload is exercised on WebKit.


---

# Resolution -- 2026-09-09

Reproduced by hand on macOS (Playwright WebKit **and** real Safari 27.0),
root-caused, fixed, and pinned. Two of the three candidate mechanisms above
are **wrong**, and the framing in the title is **half wrong**: the Cache API
replay is what makes it *work*, not what breaks it.

## The answers the report asked for

**Fix direction 1 -- which `init-error`?** *None of the three.* The status text
never came from `eval-worker.js` at all. The worker **never started**, so
`main.js`'s `evalWorker.addEventListener('error', ...)` rejected first, with
`new Error(String(e.message || e))` over an `ErrorEvent` whose `message` is
`undefined` on WebKit. The console line is:

```
Failed to initialize WASM: Error: [object Event]
```

preceded by the one that actually names it:

```
http://localhost:3000/eval-worker.js
  :: FetchEvent.respondWith received an error: TypeError: Load failed
```

So the elimination in "What is established" item 2 was sound but its premise
was not -- there was a fourth exit, above all three.

**Fix direction 3 -- is the cached body short?** **No.** Measured on WebKit,
service worker controlling, immediately before the failing reload:

| | cached blob | `Content-Length` | cached `Content-Type` |
| --- | --- | --- | --- |
| `/turmeric.wasm` | **3616659** | **3616659** | `application/wasm` |
| `/turmeric.js` | **96530** | **96530** | `text/javascript` |

Byte-exact, and the MIME type survives the round-trip. The `clone()`-then-`put`
theory is dead, and so is the lost-`Content-Type` theory: neither wasm asset
was ever the problem. **`/eval-worker.js` was**, and it was not in any cache
at all -- nothing named it, so `PRECACHE_URLS` never listed it and
`precacheShellAssets` (which mines the shell's *markup*) could not see a
worker built from a string literal in `main.js`.

**Fix direction 2 -- does real Safari reproduce?** **Yes.** Driven by hand on
**macOS Safari 27.0 (22625.1.22.11.4)**, cross-origin isolated, with a
self-contained minimal page. Safari's own words:

```
SW: destination=worker mode=same-origin
SW: fetch(RequestObject)  -> THREW: TypeError: Load failed
SW: fetch(urlString)      -> ok 200
SW: fetch(req,cache:reload)-> ok 304
SW: fetch(new Request(req))-> THREW: TypeError: Load failed
RESULT: WORKER FAILED TO LOAD  => BUG REPRODUCES
  onerror message=undefined String(e)=[object Event]
```

(Safari Remote Automation is an admin/GUI toggle that could not be enabled
non-interactively, so this was read off a screenshot rather than WebDriver.
Playwright WebKit 26.4 gives the same verdict, differing only on the
`cache: 'reload'` row, which it also throws on.)

## What actually breaks

Inside a service worker, WebKit rejects

```js
fetch(request)          // request.destination === 'worker', mode 'same-origin'
```

with `TypeError: Load failed` **once that script is already in the browser's
HTTP cache**. Chromium serves the identical request. Two ingredients, both
required -- a cold cache passes, which is why the first load is always fine:

1. the `worker` **destination carried on the Request object** -- `new
   Request(req)` and `fetch(req, {cache:'reload'|'no-store'})` fail the same
   way, while `fetch(request.url)` (no worker destination) succeeds; and
2. the script already sitting in the HTTP cache, put there by the first,
   uncontrolled load.

`cacheFirst` then misses (nothing precached `/eval-worker.js`), calls
`fetch(request)`, throws, rethrows past the `isHtmlNavigation` fallback,
`respondWith` rejects, `new Worker()` fails, and the REPL prints advice that
cannot work because the refresh is controlled too.

## Where the report's framing was wrong

> the failure appears exactly when the wasm assets are replayed from the Cache
> API instead of fetched.

Backwards. A **cache hit is the healthy path** -- a precached worker script is
served straight from the Cache API and WebKit is happy. The failure needs a
cache **miss**, which forces the `fetch(request)` that WebKit rejects. The
uncontrolled/controlled boundary was the right observation attached to the
wrong half of the branch.

## Severity, corrected: the shipped site was never affected

The reported symptom reproduces **only against the vite dev server**
(`npm run dev`) -- which is exactly what `playwright.config.js` starts, and so
all CI ever measured. Against the **built** site (`vite build` + `wrangler dev`
over `dist/`), the two reload tests pass **with the pre-fix `sw.js`**, on
WebKit. Verified both ways, and it is not the CORP header: adding
`Cross-Origin-Resource-Policy: same-origin` to dev was tried as a fix, made no
difference to the reported failure, and was dropped rather than shipped as an
unproven change.

So this was a **test-environment issue**, as the report allowed for -- but not
a phantom. The WebKit defect is real, confirmed on real Safari, and the only
thing standing between it and a production reader was that `cacheFirst`
happened to hit. The fix removes the luck.

While confirming that, a second, **dev-server-only** failure surfaced and is
worth recording so nobody re-chases it: on `npm run dev`, a reload fired
before the worker finishes installing is refused outright --

```
Refused to load '/eval-worker.js' worker because of Cross-Origin-Embedder-Policy
```

-- with **no fetch event dispatched and no service worker involved at all**.
The control settles it: it reproduces with the service worker removed
entirely (`/try/` without `?sw=1`), and never on the built site. It is the dev
server's revalidation behaviour meeting WebKit's COEP check, not anything in
`sw.js`.

## The fix

`web/public/sw.js`, two parts:

- **`fetchFromNetwork(request)`** replaces the two `fetch(request)` calls in
  `cacheFirst`. On rejection it retries `fetch(request.url)` -- but only when
  `request.destination` is `worker`/`sharedworker`, so a genuinely offline
  request still fails once instead of twice.
- **`/eval-worker.js` and `/lsp-worker.js` join `PRECACHE_URLS`.** This is a
  real offline hole independent of WebKit: the REPL's evaluator was never
  precached, so "installing Try Turmeric means having the compiler" was untrue
  for the one asset that makes it a REPL. It also keeps the hot path on the
  cache hit that every engine handles.

`clients.claim()` was suspected, tried, and **kept** -- removing it fixed
nothing.

## Test coverage

`web/tests/mobile.split-and-pwa.spec.js`:

- New: **`the eval worker survives a service-worker-controlled reload`** --
  asserts `/eval-worker.js` is precached, that the reload is controlled, and
  that the evaluator actually *runs* afterwards. Verified to **fail with the
  pre-fix `sw.js` and pass with it**, which the two existing reload tests do
  only as "status never reached Ready".
- `gotoTry` now awaits `navigator.serviceWorker.ready` before any test
  reloads, which removes the dev-server-only COEP race above. Confirmed **not**
  to mask the bug: with the pre-fix `sw.js` the specs are still red after the
  wait.
- Fixed an unrelated **stale assertion** in `editor buffer persists across
  reload`, which this work unmasked (it had never run to that line). It slept
  400ms for "the 250ms persistence debounce", but an edit reaches localStorage
  through **two chained 250ms debounces** -- `persistContent` (`main.js:2161`)
  then `persistTabs` (`main.js:231`) -- so ~500ms is the floor. It now polls
  for the write. Engine-independent: it failed identically on Chromium with no
  service worker.

Results on WebKit (`devices['iPhone 13']`): full `mobile` project **33/33**,
twice, on the dev server, and **33/33** against the built site. The `desktop`
project is unchanged -- its 6 failures (3 `repl-intelligence`, 3 `prod-smoke`
against the live domain) reproduce identically with these changes stashed.

## Worth doing next, not done here

The whole two-day divergence exists because the browser suites only ever run
against `npm run dev`. A `mobile` project pointed at `vite build` +
`wrangler dev` would have shown in one run that the shipped site was fine, and
would catch the reverse -- a real production-only regression -- which nothing
currently does.
