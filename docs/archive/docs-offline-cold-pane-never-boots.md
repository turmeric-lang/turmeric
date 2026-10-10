# `docs browse offline on a cold pane` has been failing on `main`, absorbed by `continue-on-error`

**Severity: medium.** One desktop browser test fails on every run, and takes two
more down with it. Nothing is red: the step is `continue-on-error`, so the job is
green and only a workflow annotation says otherwise.

> **RESOLVED 2026-10-10.** A `Vary` mismatch, not a missing precache entry.
> `vite preview` answers every request with `Vary: Origin`. sw.js precaches
> the shell's hashed bundles with a bare `fetch(url)`, which sends no Origin,
> but the shell loads them as `crossorigin` module script and stylesheets,
> which do. Offline, `caches.match(request)` honoured the Vary and missed
> four entries that were in the precache, so the shell HTML came back with no
> JS or CSS and `window.turmericApp` never appeared. Every page-facing lookup
> in sw.js now passes `{ ignoreVary: true }`. The `test.fixme` (marked
> 2026-10-02) is deleted, and all four offline-docs tests run and pass. Details
> in *Resolution* at the end, including a second defect found on the way:
> `waitForController` could never fail.

Filed 2026-10-01, found while adding `tests/ci.spec.js` to the desktop suite's
file list (rjungemann/turmeric#1009) -- the annotation fired on that PR and
turned out to be about a neighbouring spec.

## What fails

`web/tests/docs-offline.spec.js:145` -- `offline docs > docs browse offline on a
cold pane`:

```
TimeoutError: page.waitForFunction: Timeout 30000ms exceeded.

  153 |         await stopServer();
  154 |         await page.reload();
> 155 |         await page.waitForFunction(() => !!window.turmericApp, null, { timeout: 30_000 });
```

Step 1 of the test passes: the service worker takes control and the whole docs
pack reaches the cache (the preceding test, `the pack precaches on install
without anyone opening the docs`, asserts exactly that and is green). What does
not happen is step 2: with the origin stopped, a reload never boots the app from
cache.

The describe block is `mode: 'serial'`, so the two tests after it --
`an offline /docs/html/ page points at the copy on the device` and `a partial
pack is reported by the pane and repaired on request` -- **did not run**. Three
of the four offline-docs assertions are therefore unexercised, not one.

## Reproduces on `main`, not just on a PR

| run | head | result |
| --- | --- | --- |
| [36901587136](https://github.com/turmeric-lang/turmeric/actions/runs/36901587136) (`main`) | `8bb60d0` | 1 failed, 1 skipped, 2 did not run, 128 passed |
| [36911685873](https://github.com/turmeric-lang/turmeric/actions/runs/36911685873) (#1009) | `57cd436` | 1 failed, 2 skipped, 140 passed |

Same test, same line, same 30s timeout on both. The +12 on the PR is the `/ci`
spec it adds; it is unrelated to this.

## Why nobody noticed

`Run broader smoke suite (desktop, non-blocking)` carries `continue-on-error:
true`, so `gh run view` renders the step with a green check even when it exited
1. The only surface is the `::warning` the next step emits, which reads
"non-blocking, but see the playwright-report artifact and the web_desktop row on
/ci". This is the arrangement
[try-turmeric-browser-suites-green-while-failing](../archive/try-turmeric-browser-suites-green-while-failing.md)
was written about; the JUnit row it added means `/ci` carries an honest
`status: fail` for `web_desktop`, which is where this shows up as a trend rather
than as a surprise.

## Not reproduced locally -- the mechanism below is a lead, not a finding

This is filed from two CI runs. It needs a production build (`npm run build`
after `tur run docs` and the wasm build) because the spec skips itself without
`dist/sw.js`, which was not available here. So:

- **Established**: the test fails, on `main`, at that line, repeatably, and
  silently.
- **Not established**: why. Do not treat any of the following as diagnosed.

Where to start, cheapest first:

1. The preceding test proves the *pack* is cached. What the reload needs is the
   app shell -- `/try/`, its module graph, and `turmeric.wasm`. Check whether
   `sw.js`'s precache list still covers the shell after the Vite output moved to
   `dist/client/` (`vite.config.js`'s `isClientBuild` note records that the
   Cloudflare plugin split the output and that `injectSwVersion` had been
   writing to the wrong path for a while).
2. `page.reload()` with the origin stopped is a navigation the worker must
   answer from cache. If the worker is controlling but the navigation request
   misses, the page loads empty and `window.turmericApp` never appears -- which
   is exactly the observed symptom. The error-context artifact
   (`test-results/docs-offline-...-cold-pane-desktop/error-context.md`,
   uploaded as `playwright-report`) should show which.
3. `waitForController` passing at step 1 does not mean the worker survives the
   reload; check `clients.claim()` / the activate path against the
   cache-version stamp.

## Fix directions

Whatever the cause, the reporting is separately worth changing: a test that has
failed on every run for an unknown length of time is not telling anyone
anything. Either fix it, or mark it `test.fixme` with a pointer here so the
suite's failure count means "something new broke".

## Marked, not fixed (2026-10-02)

The second of those two options, taken -- the cause is still unknown and the
three "where to start" leads above are untouched.

`web/tests/docs-offline.spec.js` now reads `test.fixme('docs browse offline on
a cold pane', ...)` with a comment pointing here. What that buys:

- **The desktop suite gets a clean baseline.** It was `1 failed / 141 passed`
  on every run, so a genuinely new regression would have read as `2 failed` and
  looked like more of the same. Now any failure at all is new.
- **Two assertions come back.** The block is `mode: 'serial'`, so the failure
  was skipping `an offline /docs/html/ page points at the copy on the device`
  and `a partial pack is reported by the pane and repaired on request`. A
  *skip* does not cascade in serial mode, so both run again.
- **`/ci` still shows it.** `tools/ci/collect-playwright-timings.py` derives
  `status` as `"fail" if failed else "pass"`, so `web_desktop` becomes an
  honest `pass` carrying `skipped: 2` and a `partial_skip_reason`, which is
  what puts it in the skip ledger rather than hiding it.

What it does **not** buy, and the reason this report stays open:

- The offline cold-pane path is still broken, so the feature it covers is
  unverified in CI.
- `test.fixme` has no `expected.xfail`-style self-closing property. A fixture
  whose `expected.xfail` starts passing FAILS with "delete expected.xfail"; a
  `test.fixme` that starts passing stays silently skipped forever. The marker
  is therefore an obligation on whoever fixes this, recorded here because
  nothing in the tooling will raise it.

This also clears the precondition that
[try-turmeric-browser-suites-green-while-failing](../archive/try-turmeric-browser-suites-green-while-failing.md)
fix direction 6 named -- "do not leave the suite permanently red as the reason
it can never be flipped". Whether to then drop `continue-on-error` from the
desktop step is still the policy call that report left to a human.

## Resolution (2026-10-10)

### Reproduced

On a local production build: native `tur`, `tur run docs`, the wasm module
built with emsdk 6.0.10 (CI's pin), then `npm run build`. With the `fixme`
removed, the test failed at the same `waitForFunction`, with the same 30s
timeout, and the same two tests did not run. So this was never a CI-only
artefact.

### Mechanism

What a throwaway diagnostic spec (same server and steps, with console, failed
requests, and a lightly instrumented copy of `dist/client/sw.js`) showed after
`stopServer()` and `page.reload()`:

- **The navigation was answered.** The page came back titled "Try Turmeric"
  from the precached `/try/` (via `networkFirst`'s shell fallback), and the
  worker's fetch handler was running. So lead 2 (navigation miss) and lead 3
  (the worker not surviving the reload) were both ruled out.
- **Four subresources failed with `net::ERR_FAILED`:** `/assets/try-<hash>.js`,
  `/assets/site-<hash>.js`, `/assets/site-<hash>.css` and
  `/assets/try-<hash>.css`. Every one was a `cacheFirst` miss, and the
  precache listing taken just before the server stopped held all four. So
  lead 1 was ruled out too: `precacheShellAssets` finds and caches the hashed
  bundles (and the fonts) correctly under `dist/client/`. The static
  `/main.js`, `/styles.css`, `/site.css` and `/site.js` rows in
  `PRECACHE_URLS` do 404 in a production build, but those are dev-server
  paths and the misses are harmless.
- **The misses were exactly the `crossorigin` tags.** The non-crossorigin
  subresources were served from cache: `/turmeric.js`, `/pwa-shell.js`, and
  `/docs-pack/guide.{css,js}`.

The reason: every response from `vite preview` carries `Vary: Origin`. That
is its CORS middleware, and `curl -sI` shows the header whether or not the
request sends an Origin. The Cache API honours `Vary`: a stored entry matches
only if the headers it names are the same on the stored request and the
incoming one. The stored request here is the worker's own `fetch(url)`, a
same-origin GET that sends no Origin, so its header list in the precache is
empty. The incoming requests come from `<script type="module" crossorigin>`
and `<link rel="stylesheet" crossorigin>`. Those are cors-mode, and Chromium
sends `Origin: http://localhost:3111` with them. The headers differ, the
lookup misses, the network is gone, `respondWith` rejects, and the main
module never runs. `window.turmericApp` is assigned at that module's top
level, so it never appears.

Why only a **cold** boot shows it: online, the miss falls through to the
network, and `cacheFirst` stores the response in the runtime cache keyed by
the page's own request, Origin header included. The next lookup then
matches. A cold offline boot is the case that depends on the precache entry
alone, because the first visit was uncontrolled and the runtime cache never
saw those requests. That is the case `precacheShellAssets` exists for, and
the case this test exercises.

**Production: not established.** turmeric-lang.com was not reachable from the
sandbox this was diagnosed in. Cloudflare's static-asset serving is not known
to send `Vary: Origin`, so the live site may never have had this defect. The
fix is right either way. Any host or intermediary that adds a `Vary` on a
header the worker's `fetch(url)` does not send would bring the defect back,
and honouring `Vary` buys this worker nothing: the precache is keyed by URL
by construction.

### Fix

`web/public/sw.js`: a `MATCH = { ignoreVary: true }` constant, used by every
lookup that answers a page's request. That means `cacheFirst`, both lookups in
`networkFirst`, and the two `/try/` shell fallbacks. It applies when matching,
so entries that are already cached benefit without being fetched again.

### The test could not see what it was waiting for

`waitForController` was an `async` predicate passed to `page.waitForFunction`.
Playwright's poller does `const success = predicate(); if (success) ...`
(playwright-core's `coreBundle.js`) and never awaits the result. An `async`
predicate's Promise is truthy, so the wait passed on its first poll whether
or not the page had a controller, and it could not have failed. It is now a
synchronous `!!navigator.serviceWorker?.controller`. Its budget is now
waitForPackCached's 120s, because sw.js claims the page in `activate`, and
`activate` follows an `install` that precaches the whole pack.
`sw-dev.spec.js` had the same pattern in two waits. Those are now
`expect.poll` over a helper that returns -1 while the page is between
documents. The teardown under test ends in a reload, which the no-op wait
had never had to survive, and `expect.poll` does not retry a callback that
throws.

### Verification

Run with the container's Chromium 141 (`launchOptions.executablePath`), not
Playwright 1.63's pinned 153. CI uses the pinned build.

- `tests/docs-offline.spec.js`: 4/4. `--repeat-each=3 --workers=1`: 12/12.
- **Counter-check:** with the pre-fix `dist/client/sw.js` copied back into
  the same build, the new spec fails at the same wait (now line 159), with
  2 tests not run.
- `tests/sw-dev.spec.js`: 15/15 under `--repeat-each=5`.
- Do not stress this spec with `--repeat-each` across several workers. Every
  copy shares port 3111 and kills "its" server, so one copy's `stopServer`
  cuts another's install short ("pack never finished caching" with the end
  of the alphabet missing). CI runs one copy.
