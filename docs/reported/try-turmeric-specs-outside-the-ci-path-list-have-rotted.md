# Four `repl-intelligence` specs fail against a built bundle, and CI never runs them

**Severity:** low-medium (test rot that hides REPL regressions; no product bug
established). CI is green because it does not run these specs.

**Status:** open. Found 2026-10-26, the first time the whole desktop suite could
run in a sandbox with a real WASM bundle (see
[the archived bundle report](../archive/web-specs-fail-instead-of-skipping-without-the-wasm-bundle.md)).

## What happens

A bundle built from this tree (Emscripten 6.0.10, `tur_wasm`), then
`npx playwright test --project=desktop` from `web/`: **272 tests, 221 passed, 45
failed, 6 skipped** (6.4 min). The 45:

| Failures | Spec | Cause |
| --- | --- | --- |
| 40 | `prod-smoke.spec.js` | drives the live turmeric-lang.com; needs network (documented in `running-tests-guide.md`) |
| 1 | `security.spec.js` ("`/docs/html/spices/index.html` is served ...") | the spices pages come from the sibling `../turmeric-spices` checkout, which was absent; CI has it |
| **4** | **`repl-intelligence.spec.js`** | **below** |

Only the last row is a finding. All four fail the same way alone with
`--workers=1`, so they are not load flakes. The tree under test differed from
`main` only in `;;;` docstring comments (nothing under `src/`), so the evaluator
and language server in that bundle are `main`'s.

## Why CI does not notice

`ci.yml`'s blocking desktop step runs an explicit path list (`smoke`, `diag`,
`guide-toggle`, `meta-commands`, `docs-pane`, `docs-offline`, `lsp`, `minimap`,
`footer`, `lang-picker`, `sw-dev`, `trace`, `security`, `ci`).
`repl-intelligence.spec.js` is not on it (nor is `prod-smoke`, which has its own
config). The latest CI run on `main` (2026-10-26) is green; nothing in it runs
these four.

## The four failures

1. **`:doc takes a symbol, so completion after it offers symbols`** -- stale.
   `window._turiRepl.metaCompletions(':doc co')` returns the documented-symbol
   list, which now includes `::` (the ascription form, documented 2026-10-22 in
   `stdlib/forms/types.tur`), and the test asserts no label starts with `:`. The
   spec was last touched 2026-10-21. Note the same call is not narrowed by the
   `co` prefix (it begins `&`, `&mut`, `*`, ...); whether the popup narrows it
   later was not checked.
2. **`a line that failed to evaluate defines nothing the prompt offers`** --
   stale premise. `(defn zzz-broken-fn [x : int] : int (nope x))` does not fail:
   the evaluator answers
   `warning [TUR-W0040]: unknown name 'nope'; will runtime-dispatch` and returns
   `#<fn zzz-broken-fn>`, so the definition is rightly in `sessionSource()`.
   `(zzz-broken-fn 1)` is what errors, at call time. The test needs a line that
   really fails to evaluate (a reader or type error).
3. **`an editor-only defn is not offered at the prompt until it is Run`** --
   unexplained. After Run the console prints `#<fn zzz-editor-only-fn>` and
   `sessionSource()` contains the definition, yet
   `completions('(zzz-e')` stays `[]` for the test's 20 s. The sibling test "a name
   defined at the prompt is offered afterwards" passes, so completions of a
   *Run* definition, specifically, are not reaching the prompt. This one may be a
   real product bug; it needs a look at how a Run feeds the prompt's document.
4. **`Escape dismisses the prompt popup without closing the docs pane`** --
   unexplained; times out at 60 s before its assertion. Not investigated.

## Fix directions

- Fix (1) and (2) -- the premises are known -- and look at (3) and (4).
- Then either add `repl-intelligence.spec.js` to the CI path list or say why it
  is excluded. That is the real hole: a spec nobody runs cannot fail.
- A one-line lint would keep it closed: every `web/tests/*.spec.js` is either in
  the CI list or named in a short exclusion list with a reason (as
  `tests/forms-undocumented.txt` does for forms).

## Repro

```sh
# Emscripten 6.0.10 on PATH, then:
cmake -S . -B build-wasm -DTUR_WASM=ON -DCMAKE_POLICY_VERSION_MINIMUM=3.5
cmake --build build-wasm -j --target tur_wasm      # ~2.5 min -> web/public/turmeric.{js,wasm}
cd web && npm ci
npx playwright test --project=desktop tests/repl-intelligence.spec.js --workers=1
```
