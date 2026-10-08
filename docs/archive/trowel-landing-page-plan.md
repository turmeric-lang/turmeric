# Trowel landing page (`turmeric-lang.com/trowel`) -- plan

Ship a single marketing page at `turmeric-lang.com/trowel` that introduces
**Trowel**, the native Turmeric code editor, and drives downloads. The page
reuses the existing marketing design system (`/` and `/tour`) so it reads as a
first-class part of the site rather than a bolt-on. The app screenshot
(`web/public/trowel-screenshot.png`) is the visual centerpiece.

## What Trowel is (source of truth for copy)

Trowel is a **native macOS editor for Turmeric** -- a "open a file, press Run,
see output" desktop app. It is built in **C++ with Qt 6** (Scintilla / SciTE for
the editing surface), and it **bundles the Turmeric compiler** (currently
Turmeric v0.29.1) so a single download is self-contained -- no separate `tur`
install and no Finder-launch PATH surprises. Latest release is **v0.0.4**
(`github.com/turmeric-lang/trowel/releases`).

> NOTE: Trowel was rewritten off the earlier Lite XL prototype. Ignore the
> old `tools/trowel/README.md` implementation details (Lua plugin, Lite XL
> keybindings, `core.error`/`core.view`, palette-sync) -- they describe the
> retired build. The user-facing behavior below is confirmed by the screenshot
> and repo; **verify exact keybindings / command names against the current
> Trowel build before writing them into UI copy.**

Feature pillars (visible in `web/public/trowel-screenshot.png` -- tabbed source
editor on top, live REPL pane below, plus a toolbar of run / reload / format /
sweet-exp-toggle / REPL / settings actions):

- **Run in one click / keystroke** -- save + run the current file, output
  streams into the pane below.
- **Embedded REPL** -- a live `tur repl` pane in a split under the editor (the
  screenshot shows `turmeric> (+ 1 1) => 2` with the wordmark banner).
- **Docstrings + autocomplete** -- symbol docs and completion for stdlib +
  buffer-local names (backed by the bundled compiler's doc index).
- **Sweet-exp aware** -- toolbar toggle between s-expr and sweet-exp; Turmeric
  syntax highlighting.
- **Project scaffolding** -- create new binary / sweet / library projects
  (`tur init` under the hood).
- **Turmeric-native theming** -- the "Spice Market" dark palette matching the
  site, and a light variant.
- **Native + self-contained** -- a real Qt 6 macOS app, not Electron; ships the
  compiler in the bundle.

Positioning line to anchor the hero: *the fastest way to go from a blank file
to running Turmeric* -- a native editor with the compiler and a REPL built in.

## Where it lives / wiring

The site is a Vite multi-page build served by a Cloudflare Worker
(`web/worker.js` -> `env.ASSETS`). New static pages are added as rollup inputs;
`/try` and `/tour` already work this way, and directory-index asset serving maps
`/trowel` -> `/trowel/index.html` with no worker change.

1. **New page file:** `web/trowel/index.html` (mirror `web/tour/index.html`'s
   head: `/site.css`, page-specific `<style>`, `/site.js`, favicon, OG/Twitter
   meta pointed at `https://turmeric-lang.com/trowel`).
2. **Register the rollup input** in `web/vite.config.js` under
   `environments.client.build.rollupOptions.input`:
   ```js
   trowel: resolve(__dirname, 'trowel/index.html'),
   ```
3. **Nav link:** add `['/trowel', 'Trowel']` to the `links` array in the
   `SiteNav` web component (`web/site.js`) so it appears in the top nav and the
   mobile panel on every page. Set `active="Trowel"` handling as the other pages
   do. (Decide ordering: after `Tour`, before `Try It`, reads well.)
4. **Version injection:** if the page prints a version, use the
   `%TURMERIC_VERSION%` token -- `injectVersion()` in `vite.config.js` already
   replaces it at build time.
5. **No new headers / worker routes** needed. COOP/COEP already blanket-applied;
   the page ships no WASM so it does not depend on cross-origin isolation.

## Design system reuse (do not reinvent)

Pull every primitive from `web/site.css` + `web/vars.css`; add only
Trowel-specific rules in the page's `<style>` block (same pattern as
`/tour`). Reuse:

- `site-nav` / `site-sidebar` / `site-footer` web components.
- Hero scaffold: `.hero`, `.hero-grid`, `.hero-glow`, `.hero-inner`,
  `.hero-eyebrow` + `.eyebrow-dot`, `.hero-actions`, `.btn-hero`
  `.btn-hero-primary` / `.btn-hero-secondary`, `.arrow`.
- `.code-card` + `.code-card-bar` + `win-dot wd-r/wd-y/wd-g` +
  `.code-card-filename` for any code sample and for framing the screenshot's
  "window chrome" look.
- Section primitives: `.section-label`, `.section-title` (with `<em>` gold
  italics), `.section-desc`, `.split-section` / `.split-section.reverse`,
  `.feature-grid` / `.feature-card` / `.feat-icon` / `.feat-title` /
  `.feat-desc` / `.feat-badge`, `.started-section` / `.step-card`, `.cta-section`
  / `.cta-box`, `.divider`, `.reveal` (scroll-in animation).
- Tokens: `--gold`, `--gold-bright`, `--bg-card`, `--border`, `--text-primary`,
  `--text-sec`, DM Sans display / Iosevka mono.

Keep the page ASCII-only in prose per repo convention (use `--`, never em
dashes).

## Page structure (top to bottom)

1. **Nav** -- `<site-nav active="Trowel">` + `<site-sidebar>`.

2. **Hero** (centered, `/tour`-style compact hero):
   - Eyebrow: `New -- v%TURMERIC_VERSION%` (or "The Turmeric editor").
   - `h1`: e.g. "Write Turmeric in <em>Trowel</em>" or "A tiny <em>native</em>
     editor for Turmeric".
   - `.hero-desc`: one-liner -- native, ~3-5 MB, run + REPL + docstrings, no
     Electron.
   - `.hero-actions`: primary "Download for macOS" (links to the latest GitHub
     release DMG) + secondary "Install with Homebrew" (anchors to the download
     section) and/or "View on GitHub" (`github.com/turmeric-lang/trowel`).
   - **Screenshot, prominent, directly under the hero actions.** Wrap
     `web/public/trowel-screenshot.png` so it reads as the product shot:
     - Option A (recommended): drop it into a `.code-card`-style frame with the
       three `win-dot`s and a `lens-example-2.tur -- Trowel` filename bar, so
       it inherits the site's window-chrome aesthetic. The screenshot already
       includes its own macOS chrome, so prefer a **plain bordered/rounded,
       shadowed** container (`border:1px solid var(--border)`,
       `border-radius:14px`, soft `box-shadow`, subtle gold glow behind) to
       avoid double chrome. Pick one; do not stack two title bars.
     - Constrain with `max-width` (~880-960px), `width:100%`, `height:auto`,
       centered; add a faint `.hero-glow`/radial gold behind it for depth.
     - `loading="eager"`, explicit `width`/`height` attrs to avoid layout
       shift, descriptive `alt` ("Trowel editor showing a Turmeric source file
       above a live REPL pane").
   - `.install-hint` row -- reuse the homepage's install-command styling for the
     one-liner: `brew install --cask rjungemann/trowel/trowel` (with the shared
     copy button from `site.js`). No separate `tur` install needed -- the
     compiler ships in the app.

3. **Feature grid** (`.feature-grid`, ~4-6 `.feature-card`s) -- one per pillar,
   each with an icon glyph (match homepage style: `▶` run, `↻`/`⟳` REPL, `◇`
   docstrings, `✦` themes, `⊕` scaffold, `⚡` native) and a short `.feat-badge`.
   Keep badges descriptive rather than citing exact keybindings until those are
   confirmed against the current build:
   - **Run instantly** -- save + run the file, output right below. Badge:
     `Run · one keystroke`.
   - **Embedded REPL** -- live `tur repl` split pane. Badge: `built-in REPL`.
   - **Docs + autocomplete** -- docstrings and completion at the cursor. Badge:
     `stdlib + buffer docs`.
   - **Scaffold projects** -- new binary / sweet / library projects. Badge:
     `tur init`.
   - **Compiler included** -- bundles Turmeric; nothing else to install. Badge:
     `Turmeric bundled`.
   - **Themed for Turmeric** -- Spice Market dark + light. Badge:
     `dark · light`.

4. **Split section -- Run + REPL** (`.split-section`): prose on one side, a
   `.code-card` (or a cropped screenshot of the REPL pane) on the other showing
   the save -> run -> output flow. Mirror the homepage's alternating
   `.split-section` / `.split-section.reverse` rhythm. Optionally a second split
   (`.reverse`) for docstrings/autocomplete.

5. **Get Started / Download section** (`.started-section` with `.step-card`s or
   a dedicated download row). Downloads are real today -- no "coming soon" for
   macOS:
   - Step 01 -- Install: `brew install --cask rjungemann/trowel/trowel` (signed
     + notarized cask), **or** download the `.dmg` from the latest GitHub
     release (`github.com/turmeric-lang/trowel/releases`, currently v0.0.4) and
     drag to Applications.
   - Step 02 -- Open a `.tur` file.
   - Step 03 -- Press Run to build + run; output appears in the pane below.
   - Step 04 -- Open the built-in REPL and evaluate as you go.
   - Platform buttons: **macOS** ships now (link the release DMG + the Homebrew
     command). **Linux** and **Windows** are near-future -- show them with
     "coming soon" styling, no dead links.
   - Use the real, verified destinations only: the `rjungemann/trowel` repo,
     its releases page, and the cask command above. Do not hardcode a specific
     asset filename that may change per release -- link the release page (or the
     `/releases/latest` redirect) rather than a pinned `.dmg` URL.

6. **CTA** (`.cta-section` / `.cta-box`): "Ready to build with Turmeric?" ->
   primary download + secondary "Read the Guides" (`/docs/html/guides/`).

7. **Footer** -- `<site-footer>`.

### Optional niceties (nice-to-have, not blocking)

- **OS auto-detect** in `site.js` (or a small inline module): read
  `navigator.platform` / UA to relabel the primary hero button ("Download for
  macOS/Linux/Windows") and reorder the download cards. Fall back to "Download"
  when unknown. Keep it progressive-enhancement (page works with JS off).
- **`prefers-reduced-motion`**: the `.reveal`/glow animations already respect it
  via `site.css`; verify the screenshot glow does too.
- A short "Built with Qt 6 + Scintilla" credit line near the footer (license
  courtesy).

## Assets

- `web/public/trowel-screenshot.png` -- already present; the hero shot. Confirm
  it is reasonably sized for web (compress if it is a huge raw retina PNG;
  consider adding a `.webp` alongside and a `<picture>` with PNG fallback).
- Reuse `/logo-icon.svg`, `/logo.svg`, `/favicon.svg` (nav + favicon).
- Optional: a dedicated `og:image` for `/trowel` (a framed crop of the
  screenshot at 1200x630) so social cards show the editor. Until one exists,
  reuse `/og-image.png`.

## Implementation steps

1. `web/trowel/index.html` -- scaffold head + `<style>` from `web/tour/index.html`;
   swap in Trowel copy, hero screenshot block, feature grid, splits, download,
   CTA, footer.
2. `web/vite.config.js` -- add the `trowel` rollup input.
3. `web/site.js` -- add the `Trowel` nav link (top + mobile) to `SiteNav`.
4. Wire `active="Trowel"` on the new page's `<site-nav>`.
5. (Optional) OS-detect enhancement + `og:image`.
6. Build + preview: `cd web && npm run build && npm run preview`, visit
   `/trowel`, verify nav highlight, screenshot rendering at desktop + mobile
   widths, copy buttons, `.reveal` animations, and that no page scrolls
   horizontally.
7. Deploy alongside the next web deploy (same flow the release skills use for
   `/` and `/tour`).

## Acceptance

- `/trowel` renders with the shared nav/footer and is reachable from the top nav
  on every marketing page.
- The screenshot is the visual centerpiece of the hero -- large, framed,
  crisp, no double window chrome, no layout shift, good `alt`.
- Feature grid + at least one split section communicate Run, REPL, docstrings,
  scaffolding, bundled-compiler, theming -- copy accurate to the current Qt 6
  build (no retired Lite XL keybindings/commands asserted as fact).
- Download section: macOS DMG (GitHub release) + `brew install --cask
  rjungemann/trowel/trowel` as live links; Linux/Windows shown as "coming
  soon"; no dead or invented URLs.
- Responsive down to ~360px; no horizontal scroll; passes a quick Lighthouse
  a11y pass (contrast, alt text, focus states inherited from `site.css`).
- ASCII-only prose; U.S. spelling.

## Open questions

- **Nav slot / naming:** "Trowel" as a top-level nav item, or nested under a
  "Tools" menu? Recommend top-level given it is the flagship editor.
- **Exact keybindings / command names:** confirm the current Qt 6 build's Run /
  REPL / docstring shortcuts before writing them into feature badges or copy
  (the old Lite XL bindings no longer apply).
- **Bundled Turmeric version:** the app bundles a pinned Turmeric (v0.29.1 as of
  v0.0.4). Decide whether the page states a version or just "the latest
  Turmeric, built in" to avoid churn on every Trowel release.
- **Screenshot framing:** plain shadowed frame (recommended, since the PNG
  already has macOS chrome) vs. re-chrome in a `.code-card`. Confirm before
  building.
- Do we want a light-theme companion screenshot to pair with the "Themed for
  Turmeric" card?
