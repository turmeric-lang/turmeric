# Per-Spice Docs Pages Plan

> **Status:** Draft Plan
> **Last Updated:** 2026-05-24
> **Type:** Tooling / Docs / Web Platform

---

## Overview

Today, `just docs` produces a single `/docs/html/spices/index.html` page that is
just the rendered `../turmeric-spices/README.md` table. This is a flat overview;
there is no per-spice landing page, no per-spice API reference, and the
generated content lives in the main repo's `docs/html/` tree but is only
consumed by the web app via static links.

This plan replaces that single page with:

1. A **Spices index page** that lists every spice with its tier, short
   description, and links to:
   - the spice's **front-page** (rendered from its own `README.md`), and
   - the spice's **API reference** (auto-generated from `;;;` docstrings in
     `spices/<name>/src/`).
2. A **per-spice front page** at `/docs/html/spices/<name>/index.html`, derived
   from `../turmeric-spices/spices/<name>/README.md`, with the same look and
   feel as the guides pages (sidebar TOC, syntax-toggle widgets, footer).
3. A **per-spice API reference** at `/docs/html/spices/<name>/api/`, generated
   by reusing `tools/gendocs.py` against `spices/<name>/src/`.
4. All output hosted under `web/` the same way stdlib API docs and guides are
   today (Vite picks up `docs/html/` via the existing static-copy path; no new
   server routes needed).
5. README examples written as **adjacent turmeric + sweet-exp fenced blocks**,
   so `inject_syntax_toggles` (already used by guides and the current spices
   page) wraps them into the existing tabbed widget without further code
   changes.

The plan also includes **backfilling READMEs** for every spice that does not
have one (everything except `scscm` and `tidal`).

---

## Current state

- `tools/genspices.py` reads `../turmeric-spices/README.md` (or fetches it from
  GitHub) and emits a single `docs/html/spices/index.html`. It rewrites relative
  `spices/<name>/` links to point at the GitHub tree URL.
- `tools/gendocs.py` walks a directory of `.tur` files and renders one HTML
  page per module, plus an `index.html`, into `docs/html/api/`. It already
  knows how to extract `;;;` blocks, signatures, and examples.
- `tools/genguides.py` exposes `inject_syntax_toggles`, `toc_tokens_to_sidebar`,
  `SYNTAX_TOGGLE_JS`, `TURMERIC_HIGHLIGHT_JS`, and `GUIDE_CSS`, which
  `genspices.py` already imports. The same helpers will power the new pages.
- `justfile:159` -- `docs: guides spices` then runs `gendocs.py`. The `spices`
  target at `justfile:168` invokes `genspices.py`.
- `web/site.js:77` and the headers in `genguides.py` / `genspices.py` link to
  `/docs/html/spices/`. Those links will continue to work because the new
  index lives at the same path.
- Existing READMEs in the spices repo:
  `../turmeric-spices/spices/scscm/README.md`,
  `../turmeric-spices/spices/tidal/README.md`. The other ~18 spices have no
  README.

---

## Target output layout

```
docs/html/spices/
  index.html                  -- new index: table + links to each spice
  style.css                   -- shared (re-uses ../api/style.css via existing link)
  <name>/
    index.html                -- README front page (one per spice)
    api/
      index.html              -- module list for this spice
      <module>.html           -- one page per .tur file under src/
      search.js               -- per-spice symbol search index
```

`<name>` is the spice's directory name in `../turmeric-spices/spices/`
(e.g. `json`, `opengl`, `scscm`). The header nav across all spice pages keeps
the existing top-level link `/docs/html/spices/` highlighted.

---

## Phases

### Phase P0 -- README backfill (in `turmeric-spices` repo)

Land before P1 ships so the new index has real content.

For every spice without a README, add `spices/<name>/README.md` with this
template (kept short -- detail belongs in the API docs):

```markdown
# tur-<name>

<one-paragraph summary lifted from the top-level README table>

## Overview

<2-3 paragraphs: what it is, what it's good for, tier, C deps if any>

## Install

```turmeric
:spices {
  "<name>" {:url    "https://github.com/turmeric-lang/turmeric-spices"
            :ref    "<name>-v0.1.0"
            :subdir "spices/<name>"}
}
```

## Quick start

```turmeric
;; turmeric s-expression
(import "<name>")
(<name>/do-thing 42)
```

```sweet-exp
#lang sweet-exp
;; sweet-exp version of the same example
import "<name>"
<name>/do-thing(42)
```

## See also

- [API reference](api/)
- Source: <https://github.com/turmeric-lang/turmeric-spices/tree/main/spices/<name>>
```

**Backfill list (~18 spices):** `c-dsl`, `glsl`, `http`, `json`, `math`,
`opengl`, `osc`, `plutovg`, `png`, `postgres`, `raylib`, `regex`, `rtaudio`,
`rtmidi`, `sqlite`, `test`, `valkey`, `wav`. (`scscm` and `tidal` already
have READMEs; audit them and add a sweet-exp companion to each existing code
block so they match the new convention.)

**Authoring convention for examples:** every code example that appears in a
README must be provided as **two adjacent fenced blocks** -- ` ```turmeric `
immediately followed by ` ```sweet-exp ` (no prose between). `genguides.py`'s
`inject_syntax_toggles` already detects this exact pattern and wraps the pair
into the tabbed widget; no parser changes needed.

**ASCII-only** (per CLAUDE.md). Use `--`, not em dashes.

### Phase P1 -- New `tools/genspices.py`

Rewrite (do not extend) `tools/genspices.py` so it walks
`../turmeric-spices/spices/*/` rather than rendering the top-level README.

Outline:

1. **Discover spices.** List immediate subdirs of
   `../turmeric-spices/spices/`. Fail loudly if the sibling repo is missing
   (drop the GitHub fallback -- per-spice generation needs the full source
   tree, not just one README; cloning lazily is out of scope for this plan).
2. **Per spice, render the front page.** Read
   `spices/<name>/README.md`. If present, render with the same markdown
   pipeline `genguides.py` uses (`fenced_code`, `tables`, `toc`,
   `inject_syntax_toggles`, sidebar from `toc_tokens_to_sidebar`). Write to
   `docs/html/spices/<name>/index.html`. If absent, emit a stub page
   (title + "Docs in progress" + link to API + GitHub source link).
3. **Per spice, generate the API reference.** Shell out to
   `tools/gendocs.py` (or import its `main` / a new `render_tree(src, out)`
   function) with `src=../turmeric-spices/spices/<name>/src/` and
   `out=docs/html/spices/<name>/api/`. Reuse the existing template; no
   `--emit-tur` or `--emit-json` (those are stdlib-only).
4. **Render the new top-level index.** Build
   `docs/html/spices/index.html` from spice metadata. Source of truth for
   tier / description / C-dep columns: parse the table in
   `../turmeric-spices/README.md` (already loaded today). Each row's spice
   name becomes a link to `./<name>/index.html`; add a small "API" link
   next to it pointing at `./<name>/api/`. Keep the existing
   `PAGE_HEADER` so the nav stays consistent.

Reuse from `genguides.py`: `SIDEBAR_TOGGLE_JS`, `SYNTAX_TOGGLE_JS`,
`TURMERIC_HIGHLIGHT_JS`, `GUIDE_CSS`, `inject_syntax_toggles`,
`toc_tokens_to_sidebar`. Reuse from `gendocs.py`: factor out a
`render_tree(src_dir, out_dir, *, brand_label=None)` entry point so the per-
spice call passes a label like "tur-json API" for the page title and sidebar
header.

### Phase P2 -- `gendocs.py` refactor

`tools/gendocs.py` currently has `main()` as the only public entry point and
hard-codes "stdlib" branding in a few places. Extract:

- `def render_tree(src_dir: Path, out_dir: Path, *, brand: str = "stdlib",
  emit_tur: Path | None = None, emit_json: Path | None = None) -> None`

This makes the per-spice call clean and keeps the existing CLI shape
intact (`main` just parses args and delegates). Find branding strings with
`grep -n "stdlib" tools/gendocs.py` and route them through `brand`.

No behavior change for the stdlib build -- the existing `just docs`
invocation continues to pass `stdlib/` and ends up calling
`render_tree(Path("stdlib/"), ..., brand="stdlib", emit_tur=..., emit_json=...)`.

### Phase P3 -- `justfile` wiring

Update `justfile:159-169`:

```just
docs: guides spices
    python3 tools/gendocs.py stdlib/ --out docs/html/api/ \
        --emit-tur stdlib/docstrings.tur \
        --emit-json web/public/doc-names.json

spices:
    python3 tools/genspices.py --out docs/html/spices/
```

Stays the same in shape -- `genspices.py` now does more work internally but
the target signature is unchanged. `just docs` still produces a single
self-contained `docs/html/` tree.

Also: verify `web/vite.config.js` (and whatever mechanism copies `docs/html/`
into `web/public/docs/` for serving) picks up the new nested directories. If
`docs/html/` is copied wholesale, no change is needed; if there's a manual
allowlist, extend it. Confirm by running `just docs && cd web && npm run
build` and checking `web/dist/docs/html/spices/json/index.html` exists.

### Phase P4 -- Web nav touch-up (optional, small)

The header in `web/site.js:74-78` and `web/index.html:53,426` already points
to `/docs/html/spices/`. No URL change is required. Optionally, on
`/docs/html/spices/index.html`, add a one-line note ("Each spice has its own
docs -- click through for API reference and examples.") so first-time
visitors notice the change.

### Phase P5 -- Validation

- `just docs` produces non-empty `docs/html/spices/<name>/index.html` and
  `docs/html/spices/<name>/api/index.html` for every spice in the sibling
  repo.
- Spot-check one Tier 1 spice (`tur-test`) and one Tier 3 spice (`tur-json`)
  in the browser via `just web-dev`: sidebar TOC renders, syntax toggles
  switch between turmeric and sweet-exp blocks, API search works on the
  per-spice page.
- `tools/check-guide-pairs.py` already exists for guides; consider extending
  or cloning it to fail CI when a README has a `turmeric` block without an
  adjacent `sweet-exp` sibling. (Stretch goal.)

---

## Open questions

- **Cross-spice symbol search.** Stdlib has a single `search.js` indexing all
  modules. Per-spice search is naturally scoped to that spice. Do we also
  want a global search across stdlib + all spices? Out of scope for v1, but
  a follow-up could merge per-spice `search.js` files into a single payload
  consumed by an "all docs" search bar.
- **Versioning.** Today the docs always reflect `main` of the sibling repo.
  When spice releases have versions (`scscm-v0.1.0`, etc.), should pages
  show a version selector? Out of scope; revisit if/when a release-tagged
  workflow is in place.
- **GitHub-fallback parity.** Dropping the GitHub-fetch fallback means
  `just docs` will fail without a sibling checkout. Acceptable trade-off
  because the full source tree is now required, but if CI runs `just docs`
  from a clean checkout we may need a small "git clone if missing" step in
  the `spices` target.

---

## Out of scope

- Hosting docs at a separate origin or behind a CDN.
- Authoring tooling for READMEs (e.g. a `tur doc init` scaffolder).
- Auto-translating between turmeric and sweet-exp syntax. Examples are
  hand-authored as adjacent fenced blocks.
- Per-symbol deep links from the index page (you'd land on the spice's API
  page, then search/scroll). Could be added later by merging per-spice
  symbol indices into the top-level index.
