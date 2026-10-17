## Try Turmeric: Load project from zip

> **Status:** DONE -- archived 2026-10-17. Landed in v1 (`readStoreZip` /
> `parseProjectZip` / `applyProjectLoad` in `web/main.js`, tests in
> `web/tests/mobile.load.spec.js`). The v2 ideas below (drop overlay polish,
> iOS Safari validation, larger-project support) were never scheduled; they
> stay here as reference, and a new plan picks them up if one is wanted.
>
> **Predecessor:** [`docs/archive/try-turmeric-multi-tab-and-projects-plan.md`](try-turmeric-multi-tab-and-projects-plan.md)
> (§4 of the multi-tab + projects plan)

## Status (as of 2026-06-28)

- [x] **Landed in v1.** `web/main.js` has a matching store-only ZIP
      reader (`readStoreZip()`, EOCD-scan + CD walk; method != 0
      throws), `parseProjectZip()` for `{ tabs, activeId }` shaping
      (workspace.json honored when present; missing/no-workspace falls
      back to main.tur or alphabetical first), `applyProjectLoad()` for
      the actual replace (disposes outgoing Monaco models, swaps in
      the new tab set, persists), and `loadProjectFromBytes()` for the
      user-confirm gate + size guard (5 MB user-facing limit; 50 MB
      parse safety belt). The drop overlay sits inside
      `.editor-wrapper` and is gated by `.editor-pane.drag-over` so it
      never interferes with editor input. Coverage in
      `web/tests/mobile.load.spec.js` (6 tests): round-trip,
      workspace.json activeId restoration, non-zip rejection,
      all-empty-src rejection, oversized-zip rejection, and the
      file-input change end-to-end.

## Goal

Let a user repopulate the Try Turmeric tab set from a zip produced by
the Download flow (or `tur init` on disk). Drag-and-drop or file
picker; the active tab is restored from the zip's manifest. This is the
inverse of the Phase 2 Download path; it deliberately does not touch
the compiler, the WASM runtime, or the doc panel. Scope is `web/try/`,
`web/main.js`, `web/styles.css`, `web/public/`, and the Playwright
tests in `web/tests/`.

## 1. Trigger

Two entry points, both wired to the same handler:

- **Drag-and-drop**: drop a `.zip` anywhere on the editor pane. A
  drop-target overlay appears on `dragenter` (covers the editor, dims
  the rest) and disappears on `dragleave` / `drop`.
- **Open menu item**: a small `Open` button next to `Download` opens a
  hidden `<input type="file" accept=".zip">`.

## 2. Parse rules

- Reject anything that isn't a zip (magic bytes `PK\x03\x04`) with a
  toast.
- Walk the entries; for each `*.tur` under `src/` create a tab. The
  filename (minus `src/`) becomes the tab name.
- If `.turmeric/workspace.json` is present and parses, restore active
  tab id, tab order, and per-tab cursor/scroll from it. Tabs whose
  workspace entry references a missing file are silently dropped.
- If no `workspace.json`, the active tab is the entry named `main.tur`
  (or the first tab in alphabetical order if there is no `main.tur`).
- A zip whose `src/` is empty (or whose only `.tur` files are 0 bytes)
  is rejected with a toast -- no destructive replace.

## 3. Conflict resolution

Loading a zip is a **replace**, not a merge. Show a confirm:

> "Loading this project will replace your current tabs. The current
> workspace is in your browser's localStorage and can be recovered by
> reloading without confirming."

(Implementation note: don't actually write the new tab set to
`localStorage` until the user confirms. A reject leaves the existing
tab set intact.)

## 4. Size limit

Reject zips larger than 5 MB to avoid blowing localStorage quota. The
in-memory parse limit is generous (50 MB) so a too-big zip still surfaces
a clean error rather than an OOM.

## 5. Worker / headers

No changes. The zip lib runs client-side; the SW caches it like any
other static module.

## 6. Tests

Add to the existing Playwright suite under `web/tests/`:

- `mobile.load.spec.js` -- drop a known zip, assert tab set and active
  tab match.
- A round-trip spec paired with the Download path: create three tabs,
  click Download (capture via Playwright's download API), then drag the
  zip back onto the editor and assert the tab set + active tab match.

Manual checks before release:

- Round-trip a downloaded zip through a local `tur build .` to confirm
  the generated `build.tur` is valid.
- Verify the Open / drop affordance works on iOS Safari (file input
  with `accept=".zip"`) -- iOS sometimes restricts custom file types in
  PWAs.

## 7. Rollout

Independently revertable: the load path is feature-additive and never
mutates persisted state until the user confirms the replace. Reverting
removes the Open button, the drop overlay, and the handler; the rest
of the editor is unaffected.

## Open questions

- Should we offer a "merge" mode (append zip tabs alongside existing
  ones, deduplicating by name) in addition to the current replace
  semantics? Deferred -- replace is the obvious behavior; merge can
  land if users ask.
- Should the 5 MB cap be configurable, or auto-raised when the browser
  reports a larger localStorage quota? Deferred until someone hits the
  cap in practice.
