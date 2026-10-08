# `docs browse offline on a cold pane` has been failing on `main`, absorbed by `continue-on-error`

**Severity: medium.** One desktop browser test fails on every run, and takes two
more down with it. Nothing is red: the step is `continue-on-error`, so the job is
green and only a workflow annotation says otherwise.

> **Marked `test.fixme` on 2026-10-02 -- still OPEN, still not diagnosed.**
> The marker stops this from being the desktop suite's permanent baseline (see
> "Marked, not fixed" at the end); it does not fix anything, and the two tests
> this was taking down with it now run. Whoever fixes the service-worker path
> must delete the marker by hand -- `test.fixme` does not fail when the gap
> closes, so nothing else will notice.

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
