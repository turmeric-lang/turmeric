## Try Turmeric: Multi-tab UI, Project Zip Download, and Zip Load

> **Status:** Phases 1, 2, and 3 landed. Phase 1.5 (cross-tab `load`
> bridge via concatenation) is the only remaining piece of the
> original plan.

## Status (as of 2026-06-28)

- [x] **Phase 1 -- multi-tab UI + persistence + migration.** Landed in
      `web/main.js` (tab keys `tur.try.tabs.v1` at line 105) and
      `web/styles.css`. Coverage in `web/tests/mobile.tabs.spec.js` and
      `web/tests/mobile.tabs-migration.spec.js`.
- [x] **Drag-reorder (sub-task of Phase 1, prerequisite for 1.5).** Landed.
- [ ] **Phase 1.5 -- cross-tab `load` concatenation bridge.** NOT landed.
      Verified: `grep -n "load-bridge\|spliceForRun\|spliceTabs\|;; ---\|concatenat" web/main.js` returns nothing,
      and no `web/tests/mobile.load-bridge.spec.js` exists.
- [x] **Phase 2 -- download project as zip.** Landed: `buildZip()` at
      `web/main.js:1483`, `downloadProject()` at `:1618`. Coverage in
      `web/tests/mobile.download.spec.js`.
- [x] **Phase 3 -- load project from zip.** Landed: `readStoreZip()` at
      `web/main.js:1646`, `loadProjectFromBytes()` at `:1804`. Coverage in
      `web/tests/mobile.load.spec.js`.
- [x] **Phase 5 -- worker / headers validation.** Verified: No worker or
      headers changes required. Client-side code is bundled in `main.js` and
      service worker caches it correctly.
- [x] **Phase 6 -- zip roundtrip E2E tests.** Landed in `web/tests/mobile.zip-roundtrip.spec.js` which verifies download and drag-and-drop end-to-end.
- [x] **Phase 7 -- rollout verification.** Successfully verified. Programmatic and manual checks (matching zip structure to a standard local `tur` project structure, validating browser-side download-load round-trips via Playwright E2E tests, verifying caching/offline operation) have all been completed and confirmed successful.

Only Phase 1.5 remains.


> **Type:** Web platform / Try Turmeric
> **Predecessor:** [`docs/archive/try-turmeric-pwa-mobile-plan.md`](try-turmeric-pwa-mobile-plan.md)

## Goal

Promote `/try` from a single-buffer playground to a small in-browser
"project" surface:

1. A real **multi-tab editor** where each tab is a `.tur` file with its
   own persisted buffer, cursor, and scroll.
2. **Download the whole tab set as a zip** that mirrors a `tur init`
   project layout (`build.tur` + `src/*.tur`), so the user can hand it
   straight to a local `tur build`.
3. **Load a zip back** (drag-and-drop or file picker) to repopulate the
   tab set, with the active tab restored from the zip's manifest.

This is a follow-on to the shipped Try Turmeric PWA work; it deliberately
does not touch the compiler, the WASM runtime, or the doc panel. Scope is
`web/try/`, `web/main.js`, `web/styles.css`, `web/public/`, and the
Playwright tests in `web/tests/`.

## Status

Phase 1 (multi-tab UI + persistence + migration) is live in `web/main.js`
and `web/styles.css`. Each tab carries an independent Monaco
`ITextModel` (so undo history survives switches), the `+ New` button
creates `untitled-N.tur`, hover-`×` closes (disallowed when one tab
remains), double-click renames inline, and the tab set persists under
`tur.try.tabs.v1` / `tur.try.activeTab.v1` with a one-shot migration
from the legacy `tur.try.buffer.v1` / `cursor` / `scroll` keys.

Drag-to-reorder (plan §1.3 bullet 3) is implemented: pointer-capture
with a 6px threshold to disambiguate click-vs-drag, a 2px gold
left-border on the would-be drop target (or the trailing `+ New`
button when dropping at the end), and a click-suppression flag so the
release doesn't switch tabs. Was promoted from "polish" to
"prerequisite for 1.5" because concatenation makes tab order a
user-visible Run input. Playwright multi-tab specs are in
`web/tests/mobile.tabs.spec.js` and `mobile.tabs-migration.spec.js`
(9 tests, all green under the `mobile` project).

## 1. Multi-tab UI

### 1.1 Data model

Each tab is a small JS object owned by `main.js`:

```js
{
    id: 'tab-1',               // stable id, generated on creation
    name: 'main.tur',          // user-editable filename, must end in .tur
    content: '',               // current buffer
    cursor: { lineNumber: 1, column: 1 },
    scrollTop: 0,
    createdAt: 1734567890123,
}
```

The full editor state is `{ tabs: Tab[], activeId: string }`. A single
Monaco editor instance is reused; switching tabs swaps the underlying
`monaco.editor.ITextModel` rather than re-creating the editor (so undo
history per file is preserved by Monaco's own model store).

### 1.2 Tab strip

The existing `.editor-tabs` row gains:

- One `<button class="tab-button">` per tab, with the active tab styled
  as it is today.
- A close affordance on hover (`×` button inside each tab; not on the
  active tab if it's the last one).
- A double-click-to-rename interaction that swaps the label for a small
  `<input>` (Enter commits, Escape cancels).
- A `+ New` button that opens a fresh `untitled-N.tur` tab where `N` is
  the smallest integer that produces a non-colliding name.

### 1.3 Interactions

- **Switching tabs** persists the outgoing tab's cursor/scroll, swaps
  the Monaco model, and applies the incoming tab's cursor/scroll.
- **Closing the last tab** is disallowed (we always keep one tab
  open -- closing the last would leave the editor blank with no UI to
  create a new one without first opening a side panel).
- **Reordering** by horizontal drag of the tab buttons. Reuses the
  pointer-capture pattern from `initHScrollDrag` in `main.js`; a 6px
  drag threshold disambiguates click-vs-drag. Reorder mutates the
  `tabs[]` array in place and persists via the same debounced
  `safeWrite`. **Load-bearing for Phase 1.5**: concatenation splices
  tabs in tab-strip order with the active tab last, so users need
  drag-reorder to control which sibling definitions are spliced
  before the active tab. Keyboard reorder (Ctrl/Alt + Shift + Left
  / Right on a focused tab button) is a nice-to-have; defer until
  someone asks.

### 1.4 Run semantics

`Run` (Ctrl/Cmd+Enter) executes the **active tab's** content.

Cross-tab imports through the WASM driver's own resolver are out of
scope for v1 -- the runtime flattens its input to a single buffer.
**Phase 1.5 (§2.5)** ships a concatenation bridge that splices the
tab set into that single buffer at Run time, so the common case
("library tab + main tab") works without driver changes. The full
fix -- a virtual-FS hook so the driver sees real files and module
names -- is a separate plan. The download-as-zip path (Phase 2) is
the long-term escape hatch: export, run `tur build` locally.

## 2. Persistence

Builds on the keys reserved in the predecessor plan:

| Key | Shape | Notes |
| --- | --- | --- |
| `tur.try.tabs.v1` | `Tab[]` (with `content`, `cursor`, `scrollTop`) | One write per debounced change |
| `tur.try.activeTab.v1` | `string` (active tab id) | Cheap, write on switch |

### 2.1 Migration from single-buffer

On first load of the multi-tab build:

- If `tur.try.tabs.v1` is absent **and** `tur.try.buffer.v1` is present,
  synthesize one tab named `main.tur` with the legacy buffer's
  content/cursor/scroll, write the new keys, and delete the legacy keys
  (`tur.try.buffer.v1`, `tur.try.cursor.v1`, `tur.try.scroll.v1`).
- If `tur.try.tabs.v1` is also absent and no legacy buffer exists, seed
  the tab set with one `main.tur` tab holding the current default Hello
  World example.

The migration runs exactly once per browser -- after it writes
`tur.try.tabs.v1`, the legacy branch never fires again.

### 2.2 Hydration order

Same priority as today, applied to the tab set rather than a single buffer:

1. URL hash share-link (still maps to a single tab named after the hash
   slug -- on hydration, the share-link buffer replaces the active tab's
   content and the rest of the tab set is preserved).
2. `localStorage` tab set (or migration).
3. Default Hello World tab (only if both above are empty).

Tutorial mode (`?tutorial=...`) bypasses tab hydration entirely; the
tutorial owns a single ephemeral "tutorial" tab that is not persisted.

### 2.3 Reset workspace

`resetWorkspace()` already clears `tur.try.*`; extend it to also clear
the multi-tab keys. No new UI is needed.

### 2.4 Quota

Per-tab writes go through `safeWrite`, which already handles
`QuotaExceededError` by disabling persistence for the session. For
large projects this is fine -- the user can still download the zip
before reloading.

## 2.5 Cross-tab `load` bridge (Phase 1.5)

Phase 1 leaves §1.4 standing: the WASM driver still flattens its
input to a single buffer, so `(load "sibling.tur")` and
`(import sibling)` from one tab cannot see another tab's content. The
download-as-zip path (Phase 2) is the long-term answer, but a small
concatenation bridge gets cross-tab definitions working in-browser
without touching the WASM driver -- similar to how the Processing
spin-offs (Processing.js, p5.js sketches, OpenProcessing) splice the
sketch's `.pde` / `.js` files together before handing them to the
engine.

### 2.5.1 Runtime behavior

`runCode()` synthesises a single buffer at submission time:

1. Take every tab whose name ends in `.tur`, in tab-strip order.
2. Concatenate `;; --- <tab.name> ---\n` followed by the tab's content,
   each block ending in a trailing newline.
3. The **active tab is placed last** so its top-level forms (and any
   `main`) run after sibling definitions are in scope.

The WASM driver receives the synthesised buffer; nothing else in the
pipeline changes. Format / Share / URL-hash still operate on the
**active tab only** -- the splicing is a Run-time concern, not a
persistence concern.

A single-tab workspace produces the same buffer it does today (just
the active tab's content; the bridge is a no-op).

### 2.5.2 Diagnostics

When the splice produces diagnostics, line numbers refer to the
synthesised buffer, not any one tab. To keep error messages legible
without teaching the driver about virtual files:

- The `;; --- <name> ---` marker comments are intentionally
  `;;`-prefixed so they survive the tokenizer as no-op headers.
- The console output runs a small post-processor over diagnostic
  lines: when a `line N` reference falls inside a known tab range
  (computed from the splice), rewrite the message to
  `<tab.name>:line K`, where K is the offset within that tab.
- The post-processor is best-effort -- on mismatch it leaves the
  message untouched rather than guessing.

### 2.5.3 Out of scope for 1.5

- **Name collisions**: if two tabs define the same symbol, last write
  wins (per concatenation order). No warning. Users move to Phase 2
  + local `tur build` to get real namespacing.
- **`defmodule` collisions**: same caveat -- two tabs declaring the
  same module name silently collide.
- **Relative `load` / `import` paths**: the bridge does not synthesise
  a virtual filesystem; an `import` whose resolution depends on a
  physical path keeps failing the way it does today. Users hit this
  rarely on `/try`; the eventual fix is the virtual-FS hook in §1.4.
- **Selective inclusion**: every tab is included on every Run. No
  `:exclude-from-run` flag, no per-tab "library / sketch" labels.
  Keep the model boring; users who need precision can delete tabs.

### 2.5.4 UI surface

None. The bridge is invisible -- the same Run button does the same
thing, just over the synthesised buffer. The only user-visible
artifact is the rewritten line-number prefix in diagnostics, and a
status-bar hint ("Ran 3 tabs") shown for one second after a
multi-tab Run.

### 2.5.5 Tests

Add to the Playwright suite:

- `mobile.load-bridge.spec.js` -- two tabs (`util.tur` defining
  `square`, `main.tur` calling it); Run on `main.tur` succeeds.
- Diagnostic rewrite spec: an intentional error in `util.tur`
  surfaces with the tab name and the in-tab line number.

### 2.5.6 Possible future directions

Captured here so the bridge can grow into the virtual-FS hook
without surprising future contributors:

1. **Implicit `build.tur`**: instead of splicing, synthesise a
   `build.tur` (`:main "<active>.tur"`, `:src ["<other>.tur" ...]`)
   plus a virtual `src/` and teach the WASM driver to resolve
   modules from a JS-side `Map<string, string>`. Once that hook
   lands, the splice is unnecessary and `defmodule` / `import` work
   as on disk.
2. **Per-tab module hints**: a `;; @module foo/bar` magic comment
   could pre-seed module names so the bridge can sort tabs
   topologically (deps first) rather than relying on user-visible
   tab order.
3. **REPL-side incremental load**: the in-browser REPL (eventually)
   could accept tab additions as `(reload "<name>")` rather than
   re-splicing the whole project on every Run. Out of scope until
   the REPL gets a separate WASM entry point.
4. **Cross-tab share-link**: the URL-hash share-link could encode
   the entire spliced buffer (still single-shot, no FS). Cheap once
   the splice exists; deferred because the zip is the better
   "share a project" path.

These are **non-goals for 1.5**. Listed so they don't get rediscovered
from scratch when Phase 2/3 ships and someone reaches for the next
thing.

## 3. Download project as zip

**Landed.** `web/main.js` has a ~50-line store-only ZIP writer
(`buildZip()` + a one-shot CRC-32 table), `buildProjectEntries()` for
the logical layout, and `downloadProject()` for the user-facing
trigger. The toolbar gets a `Download` button next to `Share`; the
mobile `⋯` menu gets a "Download project" item that exercises the
same handler. Playwright coverage in `web/tests/mobile.download.spec.js`
(4 tests, all green): layout for a single tab, slugified collision-free
layout for multi-tab, end-to-end download click → ZIP bytes round-trip
through an inline reader, and filename pattern. The reader is store-
only; Phase 3 will lift it to handle deflate or pull in `fflate` if a
zip-with-deflate ever lands.

### 3.1 Trigger

A new `Download` button in the editor toolbar (icon-only on
`max-width: 600px`, like the other actions). Sibling of the existing
`Share` button.

### 3.2 Zip layout

The zip mirrors the layout produced by `tur init`:

```
try-turmeric-<slug>.zip
  build.tur
  src/
    main.tur
    <other-tab-name>.tur
    ...
  .turmeric/
    workspace.json    # the active tab id + tab metadata, for re-import
```

- `build.tur` is generated from a small template: project name (derived
  from the zip's slug), declared `:main "main.tur"` if a `main.tur`
  exists in the tab set, otherwise the first tab is the main.
- `src/<name>.tur` files come from each tab's content. Tab names that
  do not end in `.tur` get the extension appended; names with path
  separators or other shell-unfriendly characters are slugified.
- `.turmeric/workspace.json` carries the in-browser-only metadata
  (active tab id, tab order, cursor positions) so a round-trip
  download-then-upload preserves the user's session exactly.

### 3.3 Implementation

- Use a tree-shakeable client-side zip library; preferred:
  [`fflate`](https://github.com/101arrowz/fflate) (≈12 KB minified,
  zero deps, sync zip in a worker). Acceptable alternative: a hand-rolled
  store-only zip writer (no deflate) -- text files compress poorly and a
  ~30-line writer in `main.js` keeps the dependency surface flat.
- Generate the blob in-memory, then trigger a download via an `<a>`
  with `URL.createObjectURL`.
- Filename: `try-turmeric-<timestamp>.zip` by default; the user can
  rename in the save dialog.

### 3.4 No server round-trip

The zip is built entirely in the browser. The Cloudflare worker is
unchanged. This keeps the feature available offline (PWA standalone
mode) and avoids any privacy footgun -- user code never leaves the tab.

## 4. Load project from zip

**Landed.** Split out into its own plan at
[`docs/archive/try-turmeric-load-project-from-zip-plan.md`](try-turmeric-load-project-from-zip-plan.md)
for any follow-on work (drop overlay polish, iOS Safari validation,
larger-project support). The summary: `web/main.js` has a store-only
ZIP reader (`readStoreZip()`), `parseProjectZip()` for
`{ tabs, activeId }` shaping, `applyProjectLoad()` for the replace,
and `loadProjectFromBytes()` for the user-confirm gate + 5 MB size
guard. Coverage in `web/tests/mobile.load.spec.js` (6 tests).

## 5. Worker / headers

No changes. The zip lib runs client-side; the SW caches it like any
other static module.

## 6. Tests

Add to the existing Playwright suite under `web/tests/`:

- `mobile.tabs.spec.js` -- create / switch / close / rename, reload,
  active tab restored, per-tab cursor restored.
- `mobile.zip-roundtrip.spec.js` -- create three tabs, click Download
  (capture via Playwright's download API), then drag the zip back onto
  the editor and assert the tab set + active tab match.
- A migration spec that seeds the legacy single-buffer keys, reloads,
  and asserts the synthesized `main.tur` tab exists and the legacy
  keys are gone.

Manual checks before release:

- Round-trip a downloaded zip through a local `tur build .` to confirm
  the generated `build.tur` is valid.
- Verify the Open / drop affordance works on iOS Safari (file input
  with `accept=".zip"`) -- iOS sometimes restricts custom file types in
  PWAs.

## 7. Rollout

1. Multi-tab UI + persistence + migration (largest single piece,
   independently shippable). **Landed.**
1.5. Drag-reorder + cross-tab `load` concatenation bridge. Depends on
   Phase 1; ships before Phases 2/3 because tab order becomes a
   user-visible Run-semantics input the moment the bridge exists.
2. Download-as-zip (tiny addition once tabs exist).
3. Load-from-zip + drop overlay (depends on 2 for the format).

Each step is independently revertable; only step 1 carries a migration
risk, mitigated by writing the new keys before deleting the legacy ones
(if the migration write fails, the legacy keys survive intact). Phase
1.5 is also revertable in isolation -- the splice happens in
`runCode()` and can be replaced with the original single-tab call by
flipping one branch.

## Open questions

- Should the share-link URL hash encode the entire tab set, or stay
  single-buffer as today? Encoding the set bloats the URL quickly; a
  zip download is the better mechanism for "share a project." Keep
  share-link single-tab for v1.
- Does the WASM driver eventually need to see other tabs (e.g. for
  multi-file `import` resolution)? A separate plan, gated on the
  driver growing a virtual file-system hook.
- Should `tur init`-style templates be selectable when creating a new
  workspace? Out of scope for v1 -- the default Hello World is enough;
  curated templates can land via the existing Examples dropdown.
