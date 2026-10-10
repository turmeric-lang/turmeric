# The Try Turmeric browser job reports green with failing tests, and discards the report that would explain them

**Severity: medium** (no compiler defect; a CI *visibility* hole. The job's
check mark is not evidence the browser suites passed, and one of the failures
it is currently hiding may be a real mobile product bug.)

**Status:** RESOLVED 2026-09-02 (see the end; the mobile WebKit reload failure is now its own open report). Filed 2026-09-01 while investigating the red
`tur_reported_index_lint` on
[run 33460242737](https://github.com/turmeric-lang/turmeric/actions/runs/33460242737)
(that lint failure is separate and already fixed). Found by reading the job
log, not by a local repro -- see "How this was verified" below.

## Summary

The `Try Turmeric smoke test (browser)` job on that run is marked **passed**.
Five Playwright tests failed inside it:

| Failing test | Step |
|---|---|
| `minimap.spec.js:34` Minimap > renders on a desktop-width editor | desktop |
| `minimap.spec.js:80` Minimap > the overview ruler has lanes to paint diagnostics into | desktop |
| `smoke.spec.js:105` Try Turmeric smoke tests > Force update clears caches and reloads | desktop |
| `mobile.split-and-pwa.spec.js:28` Mobile layout + PWA > split fraction persists across reload | mobile |
| `mobile.split-and-pwa.spec.js:47` Mobile layout + PWA > editor buffer persists across reload | mobile |

Both suite steps ended in `##[error]Process completed with exit code 1`. The
job is green anyway.

Three mechanisms combine. The first is deliberate; the second and third are the
ones that make it expensive:

1. **The suites do not gate.** `Run broader smoke suite (desktop, non-blocking)`
   and `Run mobile smoke suite (non-blocking)` are both
   `continue-on-error: true` (`.github/workflows/ci.yml`). Only
   `Run deploy-gate smoke test` blocks. That part is deliberate and documented.

2. **The evidence is thrown away in exactly the case it is needed.** The
   last step is:

   ```yaml
   - name: Upload Playwright report on failure
     if: failure()
     uses: actions/upload-artifact@v4
     with:
       name: playwright-report
       path: web/playwright-report/
   ```

   `continue-on-error: true` means a failing suite step never puts the job into
   a failure state, so `failure()` is false and the upload never runs. The two
   conditions are mutually exclusive: **the report is uploaded only when the
   suites did not fail.** Confirmed against the run -- its artifact list is
   `timings-*` and `jit-ctest-log-*` only, with no `playwright-report` despite
   five failures.

3. **The suites are absent from `/ci`.** The public dashboard at
   `web/ci/index.html` reports every ctest suite -- duration trend, sparkline,
   suite table, skip ledger -- but the browser suites contribute no rows to
   `timings.jsonl` at all, because the whole pipeline is fed by
   `ctest --output-junit` and `publish-timings` only `needs: [test, jit]`. So
   the one surface that exists to show suite health over time does not know
   these suites exist. Details and the wiring needed are in fix direction 2.

The net effect is that a Try Turmeric regression is invisible on the Actions
summary page **and** on `/ci`, and reconstructing it means scrolling ~5000
lines of raw job log. The `error-context.md` files each failure names are
inside the artifact that was not uploaded.

## Why this is worth a report rather than a shrug

Non-blocking is a defensible choice for a flaky browser suite. Silently
discarding the failure report is not, and this job has a track record of
`continue-on-error` hiding things that mattered. Three separate comments in
`ci.yml` are post-mortems of exactly that:

- The entire 32-test mobile project once died at
  `browserType.launch: Executable doesn't exist ... webkit-*/pw_run.sh` before
  any test body ran, "and the step's continue-on-error hid it."
- `prod-smoke.spec.js` was accidentally matched by a bare `smoke.spec.js`
  positional and "contributed three standing failures ... which this step's
  continue-on-error quietly absorbed."
- `docs-offline.spec.js` skips itself without a production build, "which looks
  identical to passing."

That is three prior incidents of the same shape. This is the fourth, and the
missing artifact upload is why each one costs a log dig.

## The five failures, triaged

Diagnosed from the log; the first three are confidently test-side, the last
two are not yet attributed.

### 1 + 2: minimap selectors are not scoped to an editor (test bug)

Both are strict-mode/count violations from `.monaco-editor` matching more than
one element:

```
Locator: locator('.monaco-editor .minimap')
Error: strict mode violation: ... resolved to 2 elements:
    1) <div data-mprt="8" ... class="minimap slider-mouseover">
    2) ... aka locator('#repl-input > .monaco-editor > .overflow-guard > .minimap')
```

```
Locator:  locator('.monaco-editor .decorationsOverviewRuler')
Expected: 1
Received: 2
```

There are genuinely two Monaco instances now: `web/main.js:1907` creates the
main editor on `#editor`, and `web/main.js:3071` creates `promptEditor` on
`#repl-input` for the REPL prompt. `web/main.js:3065` also adds the
`monaco-editor` class to a `repl-suggest-overlay` appended to `document.body`,
so an unscoped `.monaco-editor` can match a third node.

The minimap renders fine; the spec was written when one editor existed. Fix is
to scope to the editor under test (`#editor .monaco-editor .minimap`) rather
than to relax the assertion. `web/tests/minimap.spec.js:38` and `:92`.

### 3: `location.reload` is no longer redefinable (test bug)

```
Error: page.evaluate: TypeError: Cannot redefine property: reload
```

`web/tests/smoke.spec.js:105` stubs the destructive bits so the test "observes
intent without actually unregistering the SW or navigating away", which needs
`Object.defineProperty` over `location.reload`. Current Chromium makes that
property non-configurable, so the stub throws before the assertion. The
technique needs replacing (intercept at the app's call site, or assert via a
`page.on('framenavigated')` listener), not the assertion.

### 4 + 5: WASM fails to load after a reload on mobile/WebKit (UNATTRIBUTED -- triage first)

Both mobile failures are the same assertion, and this is the one that could be
a real product bug:

```
Locator:  locator('#wasm-status-text')
Expected: "Ready"
Received: "Failed to load WASM"
    4 x  "Initializing WASM..."
    1 x  "Loading WASM module..."
    58 x "Failed to load WASM"
```

Both specs do `await page.reload()` and then wait for `Ready`. The initial load
works -- 30 of 32 mobile tests pass, so the app boots -- and the status text
walks Initializing -> Loading -> **Failed** only on the reload path. If that
reproduces against a real iOS Safari, a user who reloads Try Turmeric on mobile
gets a dead REPL, which would be a more serious bug than this report's own
severity.

Do not assume it is a test bug because the other three are. Candidates worth
separating: service-worker cache interaction on the reload (the desktop
`sw-dev`/`docs-offline` specs pass, so it is not obviously a general SW fault),
a WebKit-specific WASM fetch/instantiate path, or a CI-only artifact of the
Playwright WebKit build. **If this turns out to be a product defect, file it as
its own report** -- it is only bundled here because this job is where it
surfaced.

## Fix directions

The point of the fix is that the next regression announces itself. In rough
priority:

1. **Always upload the Playwright report.** Change `if: failure()` to
   `if: always()` (or `if: !cancelled()`) on the upload step. This is the
   one-line change that makes every other diagnosis cheap, and it is
   independent of any gating-policy decision.
2. **Report the browser suites on `/ci`, like every other suite.** This is the
   durable fix for the visibility hole, and it is missing today: the
   `/ci` dashboard (`web/ci/index.html`, fed by `web/ci-metrics.js`) renders
   rows out of `timings.jsonl` on the `ci-metrics` branch, and **the browser
   suites contribute no rows at all.** The pipeline is
   ctest-only end to end:

   - `tools/ci/collect-suite-timings.py` reads JUnit XML written by
     `ctest --output-junit` and emits one row per suite:
     `{"suite", "status", "skip_reason", "duration_ms", ...}` plus the
     build-shape tuple.
   - The `publish-timings` job declares `needs: [test, jit]` and downloads
     artifacts matching `pattern: timings-*`.

   The Try Turmeric job is in neither list and uploads no `timings-*` artifact,
   so `desktop` and `mobile` are invisible on the dashboard -- no duration
   trend, no sparkline, no suite-table row, and nothing in the skip ledger.
   Wiring them in means:

   - Emit JUnit from Playwright (`--reporter=junit`) for each suite step.
   - Teach the collector that dialect, or convert it. Note the granularity
     mismatch: ctest's XML is one `testcase` per *suite*, Playwright's is one
     per *test*. These should land as two suite rows (`web_desktop`,
     `web_mobile`) with aggregate duration and status, not as 137 rows.
   - Upload the result as a `timings-*`-matching artifact, and add the web job
     to `publish-timings`'s `needs:`.
   - **Publish an honest `status`.** A `continue-on-error` step that failed must
     produce `status: "fail"`, which is the whole point: `/ci` then shows "ran
     red, ignored" even while the Actions job is green. A suite that did not run
     at all (the WebKit-install failure mode the `ci.yml` comment describes)
     should reach the **skip ledger** -- the panel already exists for exactly
     this and is titled "Suites that did not fully run in the latest build".

   Note `publish-timings` is `if: always() && push && main`, so PR runs collect
   but do not publish; the browser rows inherit that and appear on `main`
   pushes.
3. **Surface the result in the job too.** Have each non-blocking step emit a
   `::warning::` (or a job-summary line) with its failure count, so the Actions
   summary page also distinguishes "ran clean" from "ran red, ignored". Cheaper
   than direction 2 and complementary to it, not a substitute -- a warning is
   per-run, the dashboard is the trend.
4. **Fix the three test-side failures** (1, 2, 3 above) so the suites have a
   true clean baseline. A suite that is always a bit red cannot be promoted to
   gating and trains readers to ignore it.
5. **Triage 4+5 on their own merits** before touching the specs.
6. **Then reconsider gating.** Once the desktop suite is genuinely clean, the
   argument for `continue-on-error` on it is much weaker. Flipping the gate is
   a policy change; `ci.yml` already treats an analogous JIT flip as
   "deliberately left to a human", so leave the decision to one -- but do not
   leave the suite permanently red as the reason it can never be flipped.

## Resolution tasks -- do not close without these

This report is about a check that was not really checking. Verifying the fix
by running the specs locally would repeat the original mistake, so:

- [ ] **Verify the browser suites actually appear on `/ci`.** Per direction 2:
      after a `main` push, `desktop` and `mobile` must show up as rows on
      <https://turmeric-lang.com/ci> alongside the ctest suites -- present in
      the suite table, with a duration trend and a sparkline, and reaching the
      skip ledger when a suite does not run. Check the published
      `timings.jsonl` on the `ci-metrics` branch actually carries their rows;
      the dashboard renders what is in that file, so "the page looks fine" is
      not the check. **This is a reporting task, not a testing task:** the goal
      is that `/ci` *reports* these suites, not that anything smoke-tests the
      `/ci` page (`web/tests/ci.spec.js` already covers the page itself and is
      not what this asks for).
- [ ] **Verify a red browser run publishes `status: "fail"`, not a gap.** The
      failure mode to avoid is a dashboard that shows the suites only when they
      pass -- that would reproduce this exact bug one level up. Force a failing
      assertion, push, and confirm the row is published and reads as failing.
- [ ] **Verify each changed spec is named in a step in `ci.yml`** and therefore
      runs. Playwright only picks up the paths listed in the desktop step, so a
      spec file absent from that list "passes by not running" -- the trap the
      `docs-offline` comment already documents. A green local `npx playwright
      test` proves nothing about what CI executes.
- [ ] **Verify the artifact upload actually fires on a red run.** Push a branch
      with a deliberately failing browser assertion and confirm a
      `playwright-report` artifact appears on the run. `if: always()` is easy to
      write and easy to get wrong next to `continue-on-error`; the only proof is
      an artifact list with the report in it.
- [ ] **Verify the failure is visible from the Actions run summary** without
      opening the raw log -- a warning annotation or job-summary row, per
      direction 3.
- [ ] **Confirm the test counts moved in the expected direction.** The desktop
      step reported `3 failed / 107 passed` and mobile `2 failed / 30 passed`
      on run 33460242737. A fix that lowers the failure count by dropping tests
      from the run is a regression wearing a fix's clothes -- check the passed
      count did not fall.
- [ ] **State in the resolution note which of the five were test bugs and which
      were product bugs**, and link any product bug filed separately out of 4+5.

## How this was verified

From the CI log and workflow source only -- the failure counts, error text,
step config, and the run's artifact list are all quoted above from
run 33460242737. The two Monaco creation sites and the overlay class are read
out of `web/main.js` in the tree. **No local browser run was attempted**, so
the triage in "the five failures" is a reading of the evidence and not a
reproduction; item 4+5 in particular is explicitly unattributed. Anyone picking
this up should reproduce before fixing.

## Resolution (2026-09-02)

Both fix directions, plus the three test-side failures:

1. **The report is always uploaded.** The step is `if: ${{ !cancelled() }}`
   (with `if-no-files-found: ignore`), so the artifact exists precisely when
   a suite failed -- the case `failure()` could never see behind
   `continue-on-error`.
2. **The browser suites have `/ci` rows.** Each non-blocking suite step has
   an `id` and writes Playwright JUnit (`--reporter=list,junit`,
   `PLAYWRIGHT_JUNIT_OUTPUT_NAME`); `tools/ci/collect-playwright-timings.py`
   aggregates each file into ONE row shaped like the ctest rows
   (`web_desktop` / `web_mobile`, with `passed` / `failed` / `skipped` /
   `discovered` like the fixture suites), uploaded as `timings-web`, and
   `publish-timings` now `needs: [test, jit, web-smoke]`. `status` is
   honest: `fail` when any test failed even though the job stayed green;
   `skip` with a reason when the suite never produced JUnit (the WebKit
   download case), so the skip ledger sees it. A "Report browser suite
   outcomes" step writes the three outcomes to the job summary and raises a
   `::warning::` annotation for a failed non-blocking suite, so the next
   regression announces itself on the run page.
3. **Failures 1 + 2** (`minimap.spec.js`): the selectors are scoped to
   `#editor .monaco-editor ...`; the REPL prompt is a second Monaco instance.
4. **Failure 3** (`smoke.spec.js` Force update): `location.reload` is
   non-configurable in current Chromium, so the app reloads through a small
   `hardReload()` that defers to `window.__turiReload` when a test installs
   one; the spec installs it instead of `Object.defineProperty`.
5. **Failures 4 + 5** (mobile/WebKit "Failed to load WASM" after
   `page.reload()`) are **not** diagnosed here -- there is no WebKit in this
   environment. They have their own report,
   `docs/reported/webkit-sw-controlled-reload-fails-wasm-init.md` (the existing report; the duplicate filed from this log was folded into it), and the
   always-uploaded report plus the `web_mobile` row are what make triaging
   it cheap.

### Fix direction 6 (gating) -- taken 2026-10-02

Left to a human at the time, with the caveat "do not leave the suite
permanently red as the reason it can never be flipped". That is exactly what
had happened: `docs-offline.spec.js`'s cold-pane test became a standing failure
and held the desktop suite at `1 failed` for 21 consecutive commits. Marking it
(docs/archive/docs-offline-cold-pane-never-boots.md) restored a clean baseline,
and the desktop step is now BLOCKING. The test itself was fixed and un-marked
on 2026-10-10: a `Vary: Origin` mismatch in sw.js's cache lookups.

The decision was made on this report's own `/ci` data rather than on the raw
fail rate, which would have argued the other way: 42 failures in 266 commits
(16%) looks ungateable, but collapses to 10 episodes -- the failures are
standing bugs, not flake. Isolated single-commit failures were 6/266 (2.3%),
damped to ~1e-5 by `retries: 2`. Mobile stays non-blocking, not for test
quality (its isolated-flake rate is lower, 1.9%) but because its WebKit
download step is `continue-on-error` and gating would promote an infrastructure
failure into a blocked PR. Details in test-suite-portability-guide section 6a.

Not verified end to end: the browser job needs Emscripten and a Playwright
install, neither available locally. The specs and `main.js` pass
`node --check`, the collector was exercised on a synthetic Playwright JUnit
file (fail / skip aggregation, a missing file), and the workflow parses.
