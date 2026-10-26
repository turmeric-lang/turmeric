# Try Turmeric's browser specs fail (slowly) instead of skipping when the WASM bundle is not built

**Severity:** low (developer experience; a red result that reads as a product
bug). CI is unaffected -- it builds the bundle first.

**Status:** open. Found 2026-10-26 running `tests/minimap.spec.js` in a cloud
sandbox that has no Emscripten.

## What happens

`web/public/turmeric.js` and `web/public/turmeric.wasm` are build products of
`just wasm` (`.gitignore` lines for both), so a fresh clone has neither. Run any
spec that waits for the evaluator and it fails, one test at a time, after a full
30 s apiece:

```
$ cd web && npm ci && npx playwright test --project=desktop tests/minimap.spec.js
  5 failed
    Error: expect(locator).toHaveText(expected) failed
    Expected: "Ready"
    Received: "Failed to load WASM"
      - Expect "toHaveText" locator('#wasm-status-text') with timeout 30000ms
    > 20 |     await expect(page.locator('#wasm-status-text')).toHaveText('Ready', { timeout: 30_000 });
```

It is not a regression in whatever you were editing: the same five fail on an
untouched checkout (verified 2026-10-26 by stashing the working tree and
re-running). It looks like one only because nothing says "the bundle is
missing".

## Root cause

- **21 spec files** (25 call sites) do
  `expect(page.locator('#wasm-status-text')).toHaveText('Ready', { timeout: 30_000 })`:
  `minimap`, `smoke`, `lsp`, `diag`, `run-session`, `repl-intelligence`,
  `meta-commands`, `lang-picker`, `toolbar-overflow`, `security`,
  `deploy-gate`, `prod-smoke`, and the `mobile.*` specs.
- **Four spec files already do the right thing** and `test.skip` with a message
  when their build output is absent: `docs-pane`, `docs-offline` (the docs
  pack), `web-perf` (`turmeric.wasm`) and `trace`. The WASM-dependent specs just
  never got the same guard.
- The page knows at once: `web/main.js` sets the status to `Failed to load WASM`
  (lines ~1485 and ~2014) as soon as the worker's `importScripts` fails. The
  spec then keeps polling for `Ready` anyway until the 30 s timeout, so the
  failure is both late and unexplained.

## Cost

A contributor (or an agent) who runs the web suite on a clone without the bundle
waits ~30 s per test across 21 files and reads the output as dozens of
independent breakages. Anything that touches `web/main.js` is then hard to
validate: the specs that would catch a real regression are buried among the
unbuilt-bundle failures.

## Fix directions

1. **Skip with a reason, like the docs specs.** A shared helper
   (`web/tests/wasm.js`) whose `requireWasm(test)` runs in `beforeAll`, requests
   `/turmeric.wasm`, and `test.skip`s with "no WASM bundle -- run `tur run wasm`"
   when it is a 404. One line per spec file.
2. **Keep CI strict.** A skip must not let CI go green on a broken build, so the
   helper should *fail* when `process.env.CI` is set (the way `web-perf` and the
   deploy gate need it to), and skip only locally.
3. **Or fail fast.** If skipping is unwanted, race the wait: resolve on either
   `Ready` or `Failed to load WASM`, and fail at once with "the WASM bundle did
   not load (is `web/public/turmeric.wasm` built?)" instead of after 30 s.

Option 1 plus 2 is the smallest change that makes the common case (an unbuilt
bundle on a laptop or in a sandbox) quiet and keeps the gate where it matters.
