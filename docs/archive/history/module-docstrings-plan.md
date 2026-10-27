# Plan: Module-level docstrings in generated API docs

> **Status:** Draft Plan
> **Last Updated:** 2026-05-24
> **Type:** Tooling / Documentation

---

## Overview

`tools/gendocs.py` produces per-definition cards from `;;;` docstrings,
but there is no first-class concept of a *module-level* description.
The closest thing today is a hack inside `_index_card_html`
(`tools/gendocs.py:1184-1206`): it scans the top of each `.tur` file
for the first `;; ` (double-semicolon) line and uses it as the index
card blurb. The per-module page itself shows nothing -- it jumps
straight from the `<h1>` module name to the first `def-card`.

This plan promotes the module description to a real, structured doc
block:

1. A top-of-file `;;;` block (same marker as defn docstrings) becomes
   the *module docstring*.
2. The parser captures it; the renderer shows it as a paragraph
   under the module heading on the per-module page.
3. The index-card blurb switches to reading the module docstring's
   summary line (with the legacy `;; ` line as a fallback so the docs
   degrade gracefully until every file is annotated).

Goal: a reader landing on `tur/list.html` (or any stdlib module page)
sees a paragraph explaining what the module is *for* before scrolling
through the API.

---

## Non-goals

- **Auto-generating descriptions.** This plan does not infer prose
  from exports or filenames; if a file has no module docstring, it
  falls back to the existing `;; ` line, and if that's also missing
  the page renders without a description block (same as today).
- **Rewriting every stdlib file in one commit.** The parser and
  renderer changes land first; backfilling docstrings into the ~30
  stdlib files (and any spice modules) happens incrementally.
- **A new docstring grammar.** The module docstring reuses the
  existing `;;;` format -- summary line, optional `Since:`, optional
  `Example:`. No new section types are introduced here.
- **Cross-module link generation, "see also" graphs, or topic tags.**
  Out of scope; this plan is just about per-module prose.

---

## Surface syntax

A module docstring is a contiguous `;;;` block that appears **before
the first `defn` / `defmacro` / `defstruct` / `definstance` /
`defopaque` form** in a file. It may appear before or after `(defmodule
...)` and `(export ...)` forms -- those don't count as "real"
definitions for this purpose.

```turmeric
;;; tur/list -- singly-linked Cons/nil list utilities.
;;;
;;; This module is the legacy untyped list implementation. Prefer
;;; tur/tlist for new code; this exists for bootstrap and
;;; backwards-compatibility with pre-TC1-B spices.
;;;
;;; Since: Phase B1
;;; Deprecated: superseded by tur/tlist (Phase TC1-B)

(defmodule tur/list
  (export cons head tail ...))

(defstruct Cons [value : int next : int])
```

### Format rules

- First line: `;;; <module-name> -- <one-sentence summary>`. The
  `<module-name>` mirrors the `(defmodule ...)` form for greppability
  but is stripped before rendering, exactly like defn summaries.
- Blank `;;;` lines separate sections.
- Recognized sections: `Since:`, `Deprecated:`, `Example:`. These are
  the same parsers used for defn docstrings -- no new code paths.
- `Parameters:` and `Returns:` are accepted by the parser (to share
  code) but ignored by the module-page renderer; lint warns if either
  appears on a module docstring.
- A non-`;;;` line that is itself a real definition resets the buffer
  and the block does **not** count as a module docstring. (Same reset
  rule as defn docstrings.) Regular `;;` comments between the `;;;`
  block and the first form are allowed.

---

## Implementation

### 1. Parser changes (`tools/gendocs.py`)

`parse_tur_file` (gendocs.py:193) currently flushes the `doc_buf` only
when it hits a definition. Extend it to capture a *module docstring*
as a separate field on the returned module dict:

- Add `module['docstring'] = None` to the initial dict (gendocs.py:222).
- During the line walk, when we see the **first** real definition
  (`defn` / `defmacro` / `defstruct` / `definstance` / `defopaque`),
  if `module['docstring']` is still `None` **and** there is an earlier
  `;;;` block that wasn't already consumed by a closer definition,
  promote it to `module['docstring']` via `_parse_docstring`. The
  buffer is then reset before the existing per-definition logic runs.
- To make "earlier `;;;` block" detectable, track the most recent
  fully-flushed `;;;` block separately from the running `doc_buf`.
  Concretely: when a `;;;` block ends (we hit a non-`;;`/non-`;;;`
  line that isn't a definition, e.g. `(defmodule ...)` or
  `(export ...)`), if `module['docstring']` is None and no real
  definition has been seen yet, snapshot it into a
  `pending_module_doc` slot. When the first real definition arrives,
  promote that slot into `module['docstring']` and clear it.
- Add a small unit-style check (a doctest inside gendocs.py or a new
  `tests/python/test_gendocs_module_doc.py`) covering:
  - module docstring before `(defmodule ...)`
  - module docstring after `(defmodule ...)` but before `(export ...)`
  - no module docstring (the first `;;;` is a defn docstring)
  - legacy `;; ` line still recoverable via the fallback path

### 2. Renderer changes

`render_module_page` (gendocs.py:1114) builds the page; insert the
module description right after the `module-heading` block and before
the first `def-card`:

```python
if module.get('docstring'):
    md = module['docstring']
    summary = md['summary']
    dash_idx = summary.find(' -- ')
    if dash_idx != -1:
        summary = summary[dash_idx + 4:]
    content += '  <div class="module-doc">\n'
    if summary:
        content += f'    <p class="module-doc-summary">{html_module.escape(summary)}</p>\n'
    # render Returns body lines (if any) as paragraphs? -- not needed
    if md['example']:
        content += '    <div class="def-section">\n'
        content += '      <div class="def-section-label">Example</div>\n'
        content += f'      <pre class="def-example">{html_module.escape(md["example"])}</pre>\n'
        content += '    </div>\n'
    if md['deprecated']:
        content += '    <p class="def-deprecated"><span class="def-deprecated-label">Deprecated</span> '
        content += f'{html_module.escape(md["deprecated"])}</p>\n'
    if md['since']:
        content += f'    <p class="def-since">Since: {html_module.escape(md["since"])}</p>\n'
    content += '  </div>\n'
```

Add a CSS rule in `CSS` (gendocs.py:478) for the new wrapper:

```css
.module-doc {
  margin-bottom: 2rem;
  padding: 1rem 1.25rem;
  background: var(--bg-surface);
  border: 1px solid var(--border);
  border-left: 3px solid var(--gold);
  border-radius: 6px;
}
.module-doc-summary {
  color: var(--text-primary);
  font-size: 0.95rem;
  line-height: 1.65;
}
```

### 3. Index card fallback

`_index_card_html` (gendocs.py:1184) currently scans for the first
`;; ` line. Change it to prefer the parsed module docstring's
summary (with the legacy `;; ` heuristic kept as a fallback):

```python
mod_summary = ''
if module.get('docstring') and module['docstring']['summary']:
    s = module['docstring']['summary']
    dash_idx = s.find(' -- ')
    mod_summary = s[dash_idx + 4:] if dash_idx != -1 else s
else:
    # legacy fallback: first ';; ' line at top of file
    ...existing scan...
```

### 4. `--emit-tur` / `--emit-json` integration

The runtime `(doc <name>)` lookup keys off function names, so module
docstrings are not eligible there as-is. Two options:

- **MVP:** also register the module name itself as a key, so
  `(doc 'tur/list)` returns the module docstring. Implement in
  `emit_docstrings_tur` (gendocs.py:1320) by iterating `modules` and
  appending one entry per module whose `docstring` is non-None.
- **Web search:** in `collect_doc_entries` (gendocs.py:1407), emit one
  extra entry per module with `kind='module'`, so the search bar can
  surface modules alongside defns.

Both are low-risk extensions of existing emit paths.

### 5. Backfill

Once the parser + renderer are in, work through `stdlib/*.tur` adding
a `;;;` module block to each file. Suggested order:

1. **Core types first** (`list`, `tlist`, `option`, `toption`,
   `result`, `tresult`, `pair`, `str`, `vec`, `map`, `hamt`) -- these
   are the entry points new users hit.
2. **Macros / contract / typeclass core** next.
3. **Concurrency, effects, I/O, ports, etc.** last.

Each backfill commit can touch a small batch (5-10 files) so reviews
stay readable. Files that already have a `;; ` header line get that
line lifted into the new `;;;` block as a starting point.

For spice modules, the same convention applies but is opt-in --
spices that don't add module docstrings simply render with no
description block (current behavior).

---

## Testing

- **Unit:** parser tests as listed in step 1.
- **Snapshot:** add one stdlib file with a module docstring (say,
  `stdlib/option.tur` once backfilled) and a golden-file check that
  `docs/html/api/tur-option.html` contains the rendered
  `<div class="module-doc">...` block. `just docs` already runs in
  CI; the snapshot test piggybacks on its output.
- **Regression:** a file *without* a module docstring still renders
  cleanly (no empty `<div class="module-doc"></div>`).
- **Runtime:** if step 4's MVP lands, add a tiny fixture that calls
  `(doc 'tur/list)` and asserts the summary line comes back.

---

## Open questions

1. **Markdown in summary?** Current defn summaries are plain text.
   Allowing inline backticks / links in the module summary would let
   us cross-link to related modules, but means importing a Markdown
   renderer (or hand-rolling escapes). Recommend deferring -- keep
   parity with defn summaries for now.
2. **Should `(defmodule ...)` itself carry the docstring as a
   metadata form?** That would let it survive macro expansion and
   appear in the runtime symbol table. Larger change; defer until
   the language has a story for attaching metadata to forms in
   general.
3. **Multi-paragraph bodies.** The current `;;;` format treats
   anything between section headers as a single block. If we want
   true paragraphs in the module description, we'd need to teach
   `_parse_docstring` to preserve blank-line separation. Easy to add
   later if the one-paragraph budget proves too tight.

---

## Phasing

| Phase | Scope |
| --- | --- |
| MD0 | Parser captures `module['docstring']`; unit tests pass. |
| MD1 | Per-module page renders the `module-doc` block; CSS added. |
| MD2 | Index-card fallback switches to the parsed docstring. |
| MD3 | `--emit-tur` registers the module name as a doc-lookup key. |
| MD4 | Backfill stdlib core modules (list, option, result, ...). |
| MD5 | Backfill remaining stdlib modules; document the convention in CLAUDE.md and the developing-spices guide. |
