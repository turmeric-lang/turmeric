# Spice Plan: tur-notebook

> **Status:** Implementation complete -- NB0-NB12 implemented in `../turmeric-spices/spices/notebook`; pending only the v0.1.0 tag / release sweep
> **Last Updated:** 2026-05-27
> **Type:** Spice Design / Tooling

---

## Current status

Status below reflects the implementation currently living in the sibling
`../turmeric-spices` repository, under `spices/notebook/`.

- [x] `tur-notebook` spice manifest exists (`build.tur`) and exports `notebook/cmark`,
  `notebook/cell`, `notebook/format`, `notebook/session`, `notebook/cache`, and
  `notebook/eval`
- [x] Parser work through NB2 is present: block parsing, inline parsing / HTML
  emission, GFM tables / task lists / strikethrough, and fence-attribute preservation
- [x] Notebook structure work through NB3 is present: cell attribute parsing,
  cell ids, `.tur.md` parse / serialize, and in-place cell updates
- [x] Execution work through NB5 is present: session lifecycle, per-cell eval,
  `eval=false`, `error=continue`, disk cache, and `depends=` cache invalidation
- [x] Markdown rendering is present: `notebook/render-md`, `notebook/cli`,
  `src/main.tur`, `render`, `export md`, and a first `--watch` implementation
- [x] HTML rendering is present: `notebook/render-html`, vendored
  `src/notebook/style.css`, `export html`, syntax-toggle wrapping for adjacent
  turmeric/sweet-exp cells, and image rendering from `cell-output.image-paths`
- [x] Command-mode TUI plumbing is present: `notebook/ansi`,
  `notebook/keys`, `notebook/tui`, and `tur nb tui` support navigation and
  execution keys (`j`, `k`, `gg`, `G`, `Enter`, `Shift-Enter`, `R`, `r`, `s`, `q`)
- [x] Edit-mode workflow is present in the TUI: `e` shells out to `$EDITOR`,
  structural actions (`a`, `b`, `dd`, `p`) update the notebook in place,
  modified notebooks track dirty state, and `q` warns before quitting unsaved changes
- [x] Remaining CLI surface after NB7 is present: `tur nb exec` (with
  `--cell` / `--all`) and `tur nb new` (starter scaffold) ship in `notebook/cli`
  and are covered by `tests/exec_test.tur`
- [x] NB10 interactive polish is present: search (`/`, `n`, `N`) spans cell
  source and output text, `?` opens a help overlay, `o` toggles focused output,
  and `--keybindings` merges user overrides onto the default TUI bindings
- [x] Later interactive pieces are present: `notebook/image` ships the stdout
  marker hook, base64 PNG encoder, and TUI inline image display via the Kitty
  graphics protocol / iTerm2 inline-image protocol with a `[image: path]`
  text fallback; the TUI integrates image paths from `cell-output.image-paths`
- [x] Guide is present at `../turmeric-spices/docs/guides/notebook-guide.md`
- [ ] Release follow-through is not present yet (examples, `notebook-v0.1.0` tag)

---

## Overview

One new spice for the `turmeric-spices` monorepo:

| Spice | Tag | Depends on | Purpose |
|-------|-----|------------|---------|
| `tur-notebook` | `notebook-v0.1.0` | (none, pure Turmeric + libturi) | Literate-programming format + interactive REPL |

`tur-notebook` is a pair of complementary tools that share one file format:

1. **The format.** A strict superset of CommonMark. A `.tur.md` file is plain
   markdown -- it opens cleanly in GitHub, VS Code preview, Obsidian, and
   pandoc with no plugin -- where fenced code blocks tagged `turmeric` or
   `sweet-exp` are *executable cells*. Everything else is prose.
2. **The renderer / exporter.** `tur nb render foo.tur.md` evaluates every
   cell in order in a fresh session and emits either:
   - `foo.md` -- the source with each cell followed by an `output` fenced
     block (Quarto / R Markdown pattern), or
   - `foo.html` -- a standalone HTML page using a vendored copy of the
     stdlib docs CSS so it matches the rest of the docs site.
   A dedicated `tur nb export <fmt> foo.tur.md` subcommand is provided as a
   discoverable alias (see [CLI](#cli)).
3. **The interactive TUI.** `tur nb tui foo.tur.md` opens the file in a
   terminal UI with **Jupyter-style modal navigation** -- a *command mode*
   for moving between cells and re-running them, and an *edit mode* that
   shells out to `$EDITOR` for the focused cell. State persists across
   re-runs of individual cells (one long-lived interpreter session per TUI
   instance).

The interactive piece is the most consequential design choice in this plan;
see [Interactive design](#interactive-design) below for the rationale and the
alternatives I considered.

`tur-notebook` has **no C dependencies**. It links against **libturi** for
the embedded interpreter (already exposed via the WASM glue layer), uses
raw ANSI escape sequences for the TUI rather than pulling in ncurses, and
ships a **pure-Turmeric CommonMark subset parser** (`notebook/cmark`) so the
spice has no third-party C code to vendor. The parser scope is deliberately
narrowed to "what notebooks actually use"; see
[Parser scope](#parser-scope) below for the included / deferred feature
list.

---

## File format

A `.tur.md` file is CommonMark with two recognized fence languages:

- ```` ```turmeric ```` -- a Turmeric cell (s-expression syntax).
- ```` ```sweet-exp ```` -- a sweet-expression cell (`#lang sweet-exp`
  implicit).

Optional cell **attributes** on the fence line follow Quarto's syntax:

````markdown
```turmeric {id=load-data eval=true echo=true output=true}
(import frame/csv :refer [read-csv default-csv-opts])
(def iris (read-csv "iris.csv" (default-csv-opts)))
```
````

Recognized attributes (all optional):

| Attribute | Default | Meaning |
|-----------|---------|---------|
| `id` | auto (`cell-1`, `cell-2`, ...) | Stable cell handle for TUI / errors / re-runs |
| `eval` | `true` | If `false`, the cell is rendered but not executed |
| `echo` | `true` | If `false`, the source is hidden in rendered output (cell still runs) |
| `output` | `true` | If `false`, the output block is suppressed in rendered output |
| `error` | `halt` | `halt` stops on error; `continue` records the error in output and proceeds |
| `cache` | `false` | If `true`, output is cached by source-hash on disk under `.turnb-cache/` |
| `depends` | (none) | Comma-separated list of `id`s this cell needs evaluated first |
| `image` | `inline` | `inline` embeds graphics as base64 PNG in the rendered file; `file` writes a sibling `.png` and links to it |

Unknown attributes are ignored with a warning so future versions can extend
the set without breaking older readers.

### Output blocks

Renderers attach a fenced block tagged `output` immediately after each
executed cell. The block's first line is a metadata comment (`;; cell-id=...
status=ok elapsed=12ms`); the body is captured stdout followed by
the cell's last-expression value as `;; => value`.

````markdown
```output
;; cell-id=load-data status=ok elapsed=42ms
;; => <frame 150x5>
```
````

Errors render with `status=error` and the error message body. Graphics
outputs (when a cell returns a plutovg surface or writes a PNG) render as
either an inline base64 image tag (`![](data:image/png;base64,...)`) or a
sibling-file link, depending on `image=`.

### Why a superset of CommonMark and not a bespoke format

- Existing markdown viewers (GitHub, VS Code preview, Obsidian, pandoc) show
  the file as ordinary markdown with no plugin. Reviewers do not need any
  tooling to read it.
- We do not need to write a custom parser; we feed the file through any
  CommonMark library (a tiny one is vendored as part of NB1) and walk the
  AST for fence nodes whose info string starts with `turmeric` or `sweet-exp`.
- Round-trip safe: render emits markdown that is itself valid `.tur.md`, so
  rendered + re-edited + re-rendered files do not drift.

---

## Conventions

Standard spice layout:

```
spices/notebook/
  build.tur
  src/notebook/
    cmark.tur        -- "notebook/cmark"   pure-Turmeric CommonMark subset parser
    format.tur       -- "notebook/format"  parse + serialize .tur.md (uses cmark)
    cell.tur         -- "notebook/cell"    cell struct, attribute parsing
    session.tur      -- "notebook/session" embedded interpreter session
    eval.tur         -- "notebook/eval"    cell evaluation + output capture
    cache.tur        -- "notebook/cache"   source-hash cache for cache=true cells
    render_md.tur    -- "notebook/rmd"     evaluate + emit .md
    render_html.tur  -- "notebook/rhtml"   evaluate + emit .html (vendored CSS)
    style.css        -- vendored copy of stdlib docs CSS (no code; static asset)
    image.tur        -- "notebook/image"   PNG capture + base64 / sibling-file
    tui.tur          -- "notebook/tui"     interactive TUI driver
    keys.tur         -- "notebook/keys"    key parsing, default bindings
    ansi.tur         -- "notebook/ansi"    cursor / color / clear helpers
    cli.tur          -- "notebook/cli"     argv dispatch for `tur nb ...`
  tests/notebook/
    cmark_test.tur          -- block + inline parser, GFM tables, golden fixtures
    format_test.tur
    cell_test.tur
    eval_test.tur
    render_md_test.tur
    render_html_test.tur
    cache_test.tur
    tui_test.tur            -- TUI tested via scripted key sequences against a fake terminal
  examples/
    quickstart.tur.md
    stats-walkthrough.tur.md
    plot-gallery.tur.md
```

---

## Architecture

```
                 .tur.md file
                      |
                      v
            notebook/cmark        (pure-Turmeric block + inline parse)
                      |
                      v
            notebook/format       (fence walk, prose/cell node list)
                      |
                      v
            notebook/cell list    (id, lang, source, attrs, span)
                      |
        +-------------+--------------+
        |                            |
        v                            v
  notebook/eval                notebook/tui
   (sequential)              (interactive REPL)
        |                            |
        +----> notebook/session <----+
        |     (libturi interpreter)  |
        |                            |
        v                            v
  notebook/render-md         redraw / dispatch
  notebook/render-html
        |
        v
   .md or .html
```

The renderer and TUI both go through `notebook/eval`, which goes through
`notebook/session`, which wraps a single libturi interpreter handle. The
two callers differ only in: (a) which cells to evaluate, (b) how to display
output. State (definitions, imports, refs) is owned by the session and
persists across cells -- exactly like a Jupyter kernel.

---

## CLI

A single binary, `tur-nb`, with subcommands:

```sh
tur nb render foo.tur.md                         # writes foo.md beside the source (default)
tur nb render foo.tur.md --to html               # writes foo.html
tur nb render foo.tur.md --to html --out site/   # write into site/foo.html
tur nb render foo.tur.md --cache                 # use .turnb-cache/ for cache=true cells
tur nb render foo.tur.md --watch                 # re-render on save (uses inotify/kqueue)

# Export is a discoverable alias for `render --to <fmt>`; same flags accepted.
tur nb export html foo.tur.md                    # writes foo.html beside the source
tur nb export html foo.tur.md --out site/        # write into site/foo.html
tur nb export md foo.tur.md                      # writes foo.md beside the source
tur nb export md foo.tur.md --no-output          # strip all output blocks (clean source export)
tur nb export md foo.tur.md --no-source          # outputs only (rare; useful for diffing runs)

tur nb tui foo.tur.md                            # interactive
tur nb tui foo.tur.md --no-color                 # disable ANSI colors
tur nb tui foo.tur.md --keybindings ~/.turnb-keys # override default keybindings

tur nb exec foo.tur.md --cell load-data          # one-shot: run named cell, print output, exit
tur nb exec foo.tur.md --all                     # like render, but to stdout, no file emission

tur nb new foo.tur.md                            # scaffold a starter file
```

Both `render` and `export` go through the same evaluator and emit identical
artifacts; the split is purely ergonomic. `export` reads as a verb that
matches user intent ("give me an HTML copy of this notebook") and is easier
to discover via `tur nb --help`. Future export targets (PDF, slides, ipynb)
will live as new `export` subcommand values and ship as separate spices
(see [Future spices](#future-spices)); the v0.1.0 set is just `md` and `html`,
both of which require no additional dependencies.

`tur-nb` is registered as a `bin` in `build.tur` so `tur install tur-notebook`
puts it on `$PATH`.

---

## Interactive design

This is the design decision the user explicitly flagged as open. The
recommendation here is **option B (modal TUI)**, with the static renderer
underneath. Section explains the alternatives and why this is the pick.

### Recommendation: modal TUI over the same renderer

The TUI is a thin layer over `notebook/eval`. It loads the file, renders
the cell list to the terminal, and listens for keystrokes in one of two
modes -- exactly like Jupyter Notebook's classic UI (or `vim`, or `less +F`).

**Command mode** (cell-level navigation; outer loop):

| Key | Action |
|-----|--------|
| `j` / `k` or arrow down/up | Move focus to next / previous cell |
| `gg` / `G` | Jump to first / last cell |
| `Enter` | Re-run the focused cell |
| `Shift-Enter` | Run focused cell, then move focus to next |
| `Ctrl-Enter` | Run focused cell, do not move focus |
| `R` | Restart session and re-run all cells in order |
| `r` | Re-run all cells from focus onward |
| `e` | Edit the focused cell (drops into `$EDITOR`; see below) |
| `a` | Insert a new cell above |
| `b` | Insert a new cell below |
| `dd` | Delete the focused cell (yank to register) |
| `p` | Paste yanked cell below |
| `o` | Toggle output visibility for the focused cell |
| `s` | Save the file (re-serialize with current outputs) |
| `/` | Search across cell sources |
| `n` / `N` | Next / previous search match |
| `?` | Help overlay |
| `q` | Quit (prompts to save if dirty) |

**Edit mode** is intentionally *minimal*. When the user hits `e` in command
mode, the TUI:

1. Writes the focused cell's source to a temp file.
2. Spawns `$EDITOR +<line>` (vim, helix, nano, emacs -- whatever the user
   already has configured).
3. On editor exit, reads the temp file back, replaces the cell source, and
   re-renders.

This avoids building an in-process editor (a substantial body of work with
plenty of edge cases around UTF-8, undo, paste, etc.) and respects the user's
existing keybindings, theme, and plugins. It also keeps the TUI surface small
and easy to test (no text-editing state machine to verify).

**Why modal navigation:**

- Jupyter's command/edit split is the muscle memory most data-science users
  already have; mirroring it lowers the learning curve.
- `vim`-style movement in command mode is the lingua franca of terminal
  tools; everything that scrolls cells (`less`, `man`, `tig`, `lazygit`)
  works this way.
- Modes let us bind single keys to high-frequency actions (`Enter`, `e`, `R`)
  without conflicts -- which is what makes a notebook UI feel snappy.

### Alternatives considered

**A. Editor extensions (VS Code / Neovim / Emacs).**

- *Pros:* WYSIWYG inline rendering, no separate tool to launch, integrates
  with existing dev environment.
- *Cons:* N implementations to maintain (one per editor); each extension has
  its own lifecycle, packaging, and review process; we lose users on editors
  we do not target. The TUI gives us a single binary that works with any
  editor via `$EDITOR`, and a VS Code extension can later be added on top of
  it if demand warrants -- it would just shell out to `tur-nb` for execution.

**B. Pure static renderer (no interactivity at all).**

- *Pros:* Simplest possible scope; the renderer is the entire product.
- *Cons:* The feedback loop (edit -> save -> re-render -> view) is
  intolerable for exploratory work, which is the main use case the user
  described. We do ship the static renderer, but only as a foundation
  beneath the TUI.

**C. Stateless renderer + watch mode.**

- *Pros:* No TUI to maintain; works in any terminal with any editor; clean
  reproducibility story.
- *Cons:* Every change re-runs the *entire* file. For workloads where a
  single early cell is expensive (loading data, fitting a model), this is
  slow. Cache (`cache=true`) helps but does not eliminate the cold-start
  cost. The TUI's per-cell re-run with persistent kernel state is the
  Jupyter UX users expect.

**D. Hybrid (the recommendation): TUI + watch-mode renderer.**

- The TUI is for active exploration (kernel stays warm, you re-run one
  cell at a time, edit via `$EDITOR`).
- `tur nb render --watch` is for "I'm writing prose, just keep the output
  fresh" workflows.
- They share the same parser, evaluator, and session machinery; the
  divergence is purely at the output layer.

### Cell execution semantics in the TUI

- The session is a single Turmeric interpreter that lives for the lifetime
  of the TUI process.
- Re-running a cell does **not** roll back state; it re-evaluates the
  source against the current session, exactly like Jupyter. Users who want
  a clean slate hit `R` (restart and run all).
- Each cell's last-expression value is captured and rendered below the cell
  in a styled output region. Stdout / stderr are captured per-cell.
- If a cell raises, the error is captured into the output region;
  subsequent cell runs continue to work (the session is not torn down).

### File saving

- The file is dirty whenever a cell's source or output differs from disk.
- `s` re-serializes the cell list back to `.tur.md`. Cells without outputs
  in memory are written without `output` blocks; cells with outputs are
  written with current outputs (so saving after a run gives you a
  "rendered" file). Users who prefer source-only files can save before
  running, or set `output=false` on cells they want to keep clean.

### Why not a browser-based UI

A browser tab serving cells via WebSockets is the obvious "modern" answer
(it is what Jupyter itself, Marimo, Pluto.jl, and Observable do). It is also
substantially more code: HTTP server, WebSocket protocol, JavaScript front
end, asset pipeline. v0.1.0 ships a terminal tool that fits the rest of the
Turmeric ecosystem (the existing REPL is a CLI). A browser front end is a
natural follow-up spice (`tur-notebook-web`) that reuses `notebook/session`
and `notebook/eval` unchanged.

---

## Style and option types

```turmeric
;;; cell -- one executable code block in a .tur.md file.
(defstruct cell
  id          :cstr   ;; user-supplied or auto-generated
  lang        :int    ;; 0 = turmeric, 1 = sweet-exp
  source      :cstr   ;; cell body (without fence lines)
  attrs       :int    ;; alist of (name . value) cons pairs
  start-line  :int    ;; 1-based line in source file (for error messages)
  end-line    :int)

;;; cell-output -- the result of evaluating one cell.
(defstruct cell-output
  cell-id     :cstr
  status      :int    ;; 0 = ok, 1 = error, 2 = skipped (eval=false)
  stdout      :cstr
  value-repr  :cstr   ;; printed form of last-expression value (or "")
  error-msg   :cstr   ;; error text if status = 1
  elapsed-ms  :int
  image-paths :int)   ;; cons list of :cstr (PNG paths produced during evaluation)

;;; render-opts -- options for notebook/render-md and notebook/render-html.
(defstruct render-opts
  output-dir     :cstr  ;; 0 (nil ptr) = same dir as source
  use-cache      :int   ;; 0 = off, 1 = on
  image-mode     :int   ;; 0 = inline (base64), 1 = sibling file
  include-source :int   ;; 0 = hide all cell sources, 1 = honor each cell's echo attr
  fail-fast      :int)  ;; 0 = continue on error (record in output), 1 = stop and exit nonzero

(default-render-opts)   ;; => render-opts with sensible defaults

;;; tui-opts -- options for notebook/tui.
(defstruct tui-opts
  color           :int  ;; 0 = monochrome, 1 = 256-color (default)
  cell-width      :int  ;; 0 = auto from terminal, else fixed
  scrollback      :int  ;; lines of output to keep per cell (default 1000)
  keybindings-file :cstr) ;; 0 = built-in defaults

(default-tui-opts)
```

---

## Modules and exports

### notebook/cmark

A pure-Turmeric CommonMark subset parser. Walks the input once at the block
level (lines -> blocks), then walks block contents once at the inline level
(text -> spans). Produces an AST that `notebook/format` walks to find
executable cells and that `notebook/render-html` walks to emit HTML.

See [Parser scope](#parser-scope) below for the feature matrix.

```turmeric
;; AST tags returned by md-node-tag.
(md-tag-document)        ;; 0
(md-tag-heading)         ;; 1   ATX 1-6
(md-tag-paragraph)       ;; 2
(md-tag-blockquote)      ;; 3
(md-tag-list)            ;; 4   ordered or unordered
(md-tag-list-item)       ;; 5   may carry a task-list checkbox marker
(md-tag-code-block)      ;; 6   fenced (with info string) or indented
(md-tag-thematic-break)  ;; 7
(md-tag-table)           ;; 8   GFM
(md-tag-table-row)       ;; 9
(md-tag-table-cell)      ;; 10
(md-tag-text)            ;; 11  inline literal
(md-tag-emph)            ;; 12
(md-tag-strong)          ;; 13
(md-tag-code-span)       ;; 14
(md-tag-link)            ;; 15
(md-tag-image)           ;; 16
(md-tag-strikethrough)   ;; 17  GFM
(md-tag-autolink)        ;; 18
(md-tag-hard-break)      ;; 19
(md-tag-soft-break)      ;; 20
(md-tag-raw-html)        ;; 21  inline only; block-level HTML is deferred

;; Parse: produces an md-node rooted at md-tag-document.
(md-parse s)                               ;; => md-node :int

;; Inspection.
(md-node-tag n)                            ;; => :int
(md-node-children n)                       ;; => list<md-node>
(md-node-text n)                           ;; => :cstr   (for text/code-span/code-block)
(md-node-info n)                           ;; => :cstr   (fence info string, link url, image src, etc.)
(md-node-meta n)                           ;; => :int    (heading level, list start number, table alignment bitset, ...)

;; Walk helpers used by format + render layers.
(md-walk-fences n callback)                ;; => :void   (visit every code-block child in document order)
(md-source-span n)                         ;; => (cons start-line end-line)

;; Emit normalized CommonMark back from an AST.  Used by notebook/format
;; nodes->str and by tests that round-trip parse(serialize(parse(x))) == parse(x).
(md-emit n)                                ;; => :cstr

;; Emit HTML.  link-resolver and image-resolver let callers rewrite URLs
;; (e.g. to embed sibling PNGs as data URLs).  Pass 0 for default identity.
(md-emit-html n link-resolver image-resolver)
                                           ;; => :cstr
```

The parser is **commonmark-spec conformant on the included feature set**;
tests assert behavior against extracts from the CommonMark 0.30 spec test
suite for every block / inline kind we claim to support. Features outside
the included set parse as their literal source text (e.g. raw HTML blocks
appear as paragraphs containing escaped angle brackets), so files written
against full GitHub-flavored markdown still render -- they just render
sub-optimally.

---

### notebook/format

```turmeric
;; Parse a .tur.md file into a list of nodes: text spans and cell structs
;; preserve original document order.  Internally calls md-parse and walks
;; the resulting AST for fence nodes tagged "turmeric" / "sweet-exp".
(parse-file path)                          ;; => result<list<node>>
(parse-string s)                           ;; => result<list<node>>

;; node-tag => 0 (prose chunk, md-node) or 1 (cell struct)
(node-tag n)                               ;; => :int
(node-prose n)                             ;; => :int    (md-node when tag = 0)
(node-cell n)                              ;; => cell    (when tag = 1)

;; Serialize back to .tur.md source.  Prose chunks round-trip via md-emit;
;; cells re-form as ```turmeric / ```sweet-exp fences with their attrs.
(nodes->str nodes)                         ;; => :cstr

;; Update a single cell's source in place inside a node list (returns a new
;; list; node lists are immutable).
(nodes-update-cell nodes cell-id new-source)
                                           ;; => result<list<node>>
```

---

### notebook/cell

```turmeric
;; Parse the attribute block from a fence info string like "turmeric {id=foo
;; eval=false}".  Returns an alist of (name . value).
(parse-attrs s)                            ;; => alist<:cstr :cstr>

(attr-get attrs name default)              ;; => :cstr
(attr-get-bool attrs name default)         ;; => :int  0 or 1
(attr-get-int attrs name default)          ;; => :int

;; Build a cell from its parts (used by format parser and TUI insert).
(cell-make id lang source attrs start-line end-line)
                                           ;; => cell

;; Auto-assign ids to cells whose id attribute is missing.
(cells-assign-ids cells)                   ;; => list<cell>
```

---

### notebook/session

```turmeric
;; Open a fresh interpreter session.  Returns an opaque handle.
(session-open)                             ;; => session :int
(session-close s)                          ;; => :void

;; Evaluate a string in the session.  Captures stdout, stderr, last-expr value.
;; Returns a cell-output (with cell-id = "") suitable for display.
(session-eval s source lang)               ;; => cell-output
;; lang = 0 (turmeric) or 1 (sweet-exp)

;; Reset the session (drops all definitions; equivalent to close + open).
(session-reset s)                          ;; => :void
```

`session-open` initializes a libturi interpreter and installs an output
capture hook (a small inline-C bridge that redirects `printf` and friends
into a per-cell `tur-frame`-style growable buffer; flushed when the cell
finishes).

---

### notebook/eval

```turmeric
;; Evaluate one cell against the session.  Honors cell attrs (eval, error,
;; cache, depends).  Returns the cell-output, ready to render.
(eval-cell session cell cache)             ;; => cell-output

;; Evaluate every cell in document order.  Returns a parallel list of outputs.
(eval-all session cells cache opts)        ;; => list<cell-output>

;; Evaluate from a given cell id forward (used by TUI's 'r' key).
(eval-from session cells start-id cache opts)
                                           ;; => list<cell-output>
```

---

### notebook/cache

```turmeric
;; Cache key = SHA-256 of (cell source + sorted attribute list + list of
;; previous-cell hashes for cells in `depends`).  Stored under .turnb-cache/.
(cache-open dir)                           ;; => cache :int
(cache-close c)                            ;; => :void

(cache-get c key)                          ;; => option<cell-output>
(cache-put c key output)                   ;; => :void
(cache-clear c)                            ;; => :void
```

---

### notebook/render-md and notebook/render-html

```turmeric
;; Evaluate and emit foo.md beside foo.tur.md (or under opts.output-dir).
(render-md path opts)                      ;; => result<:cstr>   (path written)

;; Render to HTML using the same CSS as the stdlib API docs.
(render-html path opts)                    ;; => result<:cstr>

;; In-memory variants (for tests, scripting, the TUI's preview pane).
(render-md-string nodes outputs opts)      ;; => :cstr
(render-html-string nodes outputs opts)    ;; => :cstr
```

The HTML renderer walks the AST through `md-emit-html` for prose, wraps each
cell in a styled `<pre class="turmeric-cell">` (or `sweet-exp-cell`), and
attaches its output block as a sibling `<pre class="cell-output">`. Styling
comes from `src/notebook/style.css`, a **vendored snapshot** of the stdlib
docs CSS -- copying the file keeps `tur-notebook` self-contained as a
spice (no path back into the main turmeric repo at runtime) while
preserving the visual match with the docs site. The snapshot is refreshed
manually when the docs CSS changes meaningfully; the README documents the
update procedure.

It also injects the syntax-toggle widget present in guides, so turmeric /
sweet-exp cells of the same content render side-by-side when both are
provided.

---

### notebook/image

```turmeric
;; Hook for spices that produce images (tur-plot, tur-plutovg).  The TUI
;; and renderers install this hook on the session; when the cell calls
;; (plot-write-png ...) or similar, the path is captured for the output.
(image-hook-install session)               ;; => :void
(image-hook-take session)                  ;; => list<:cstr>  (and clears)

;; For inline images: read a PNG and produce a base64 data URL.
(png->data-url path)                       ;; => result<:cstr>
```

Spices opt in by calling `image-hook-record-path` from their save-to-disk
paths (a tiny addition documented in the integration guide). Nothing
breaks if a spice does not opt in -- it just means the user has to write
the PNG explicitly and include it via markdown image syntax.

---

### notebook/tui

```turmeric
;; Launch the interactive TUI on a .tur.md file.
;; Returns the process exit code (0 = clean quit, 1 = quit with unsaved changes after warning).
(tui-run path opts)                        ;; => result<:int>

;; Lower-level entry: open with an already-parsed node list (used by tests).
(tui-run-nodes nodes opts)                 ;; => result<:int>
```

---

### notebook/keys

```turmeric
;; Default key bindings as an alist of (key-string . action-symbol-cstr).
(default-keybindings)                      ;; => alist<:cstr :cstr>

;; Load user keybindings from a file (one "key action" per line, # = comment).
(load-keybindings path)                    ;; => result<alist>

;; Action dispatcher.
(action-dispatch tui-state action)         ;; => tui-state
```

Actions are named strings (`"cell-next"`, `"cell-edit"`, `"run-from-here"`,
`"quit"`, ...) so users can rebind without source changes.

---

### notebook/ansi

A handful of helpers; this module exists so the TUI can be written without
caring about escape-code minutiae.

```turmeric
(ansi-clear-screen)                        ;; => :void
(ansi-move-to row col)                     ;; => :void
(ansi-set-fg color-idx)                    ;; => :void
(ansi-set-bg color-idx)                    ;; => :void
(ansi-reset)                               ;; => :void
(ansi-bold) (ansi-italic) (ansi-underline) ;; => :void

(term-size)                                ;; => (cons rows cols)
(term-enable-raw)                          ;; => :void
(term-disable-raw)                         ;; => :void
(term-read-key)                            ;; => :cstr  ("j", "<C-Enter>", "<Up>", ...)
```

---

### notebook/cli

```turmeric
;; Argv dispatch.  Returns the process exit code.
(cli-main args)                            ;; => :int
;; args = cons list of :cstr (program name omitted; matches *args*)
```

Subcommands: `render`, `tui`, `exec`, `new`. Unknown subcommand prints help
and exits 2.

---

## Implementation phases

The first three phases stand up the markdown parser, since everything else
depends on it. Each parser phase ships against a curated subset of the
CommonMark 0.30 spec test suite as its acceptance criterion.

- [x] **NB0** -- `build.tur`; `notebook/cmark` block-level parser:
  paragraphs, ATX headings (`#`..`######`), setext headings, blank lines,
  fenced code blocks (` ``` ` and `~~~` with info strings), indented code
  blocks, thematic breaks (`---`, `***`, `___`), blockquotes, bullet lists
  (`-`, `*`, `+`), ordered lists (`1.`, `1)`). AST built from md-node
  structs; `md-emit` round-trips block-level documents.

- [x] **NB1** -- `notebook/cmark` inline parser: text runs, emphasis (`*`,
  `_`), strong (`**`, `__`), inline code spans (`` ` ``), inline links
  (`[text](url)`), inline images (`![alt](src)`), autolinks
  (`<http://...>`, `<email@host>`), hard breaks (line ending in `\` or
  two spaces), soft breaks, character escapes, ASCII punctuation entities.
  `md-emit-html` produces conformant HTML for everything implemented so
  far.

- [x] **NB2** -- `notebook/cmark` GFM extensions: pipe tables (with `:--`
  alignment markers), task list items (`- [ ]`, `- [x]`), strikethrough
  (`~~`); cell attribute strings on fence info lines (Quarto-style
  `{id=foo eval=true}` -- recognized but not interpreted yet).

- [x] **NB3** -- `notebook/format` (parse-file / parse-string / nodes->str /
  nodes-update-cell) walking the AST for turmeric/sweet-exp fences;
  `notebook/cell` (attribute parser, defaults, id assignment); golden-file
  tests against fenced blocks with all known attributes.

- [x] **NB4** -- `notebook/session` wrapping libturi (open, eval, close,
  reset); stdout / stderr capture via the inline-C output-redirect hook;
  `notebook/eval` (eval-cell, eval-all, eval-from) with `eval`, `echo`,
  `output`, `error` attributes honored.

- [x] **NB5** -- `notebook/cache` (SHA-256-keyed disk cache under
  `.turnb-cache/`); `cache=true` and `depends=` attributes honored;
  invalidation test: changing an upstream cell busts every downstream cell
  that lists it in `depends`.

- [x] **NB6** -- `notebook/render-md` writing valid round-trippable `.md`;
  `notebook/cli render` and `tur nb export md` subcommands; `--watch`
  flag using `kqueue` on Darwin and `inotify` on Linux (small inline-C
  bridge per OS).

- [x] **NB7** -- `notebook/render-html` using the vendored `style.css`;
  `tur nb export html` subcommand; image embedding (base64 inline and
  sibling-file modes); syntax-toggle wrapping for turmeric+sweet-exp
  sibling cells. A compiler regression in recursive module loading was fixed so
  `src/main.tur` can import `notebook/cli` normally again.

- [x] **NB8** -- `notebook/ansi` (raw mode, key reading, cursor moves);
  `notebook/keys` (default bindings, file loader, action dispatch);
  `notebook/tui` with command mode navigation (`j`, `k`, `gg`, `G`,
  `Enter`, `Shift-Enter`, `R`, `r`, `s`, `q`). No edit mode yet.

- [x] **NB9** -- TUI edit mode: `e` spawns `$EDITOR` on a temp file, reads
  it back, updates the cell, re-renders. Insert / delete / paste (`a`,
  `b`, `dd`, `p`). File-dirty detection and save prompt on quit.

- [x] **NB10** -- TUI search (`/`, `n`, `N`); help overlay (`?`); output
  toggle (`o`); search across both cell sources and output text. User
  keybindings override file (`--keybindings`) merged onto built-in defaults;
  `tur nb tui` also accepts `--no-color`.

- [x] **NB11** -- `notebook/image` hook (stdout marker convention,
  `image-hook-record-path`, `png->data-url`, `image-display-tui`); the TUI
  renders image paths recorded into `cell-output.image-paths` via the Kitty
  graphics protocol or iTerm2 inline-image protocol when detected, with a
  `[image: path]` text fallback on other terminals; integration with
  `tur-plot` / `tur-plutovg` is opt-in: cells call `(image-hook-record-path
  path)` after `plot-write-png` / `surface-write-png` to advertise the PNG.
  Sixel was deferred to a follow-up. See the notebook guide for the full
  integration pattern.

- [x] **NB12** -- `notebook/cli exec` (`--cell <id>` and `--all`) and
  `notebook/cli new` (starter file scaffold) ship in `src/notebook/cli.tur`;
  `tests/exec_test.tur` covers the happy and error paths for both; README in
  `turmeric-spices` lists the spice; `docs/guides/notebook-guide.md` covers
  cells, rendering, the TUI, caching, plots, reproducibility, and
  keybindings. The `notebook-v0.1.0` tag is the only remaining step and will
  be cut once the v0.1.0 examples (`quickstart.tur.md`,
  `stats-walkthrough.tur.md`, `plot-gallery.tur.md`) are added under
  `spices/notebook/examples/`.

---

## Parser scope

The pure-Turmeric parser ships a deliberately *narrow* CommonMark subset --
"what a Turmeric notebook author actually writes." Anything outside this set
is parsed as a literal text fragment, which preserves the source bytes for
round-trip but renders verbatim in HTML output. Authors who hit a missing
feature can either avoid it or file an issue against the post-v0 roadmap.

### Included

**Block-level (NB0):**

- Paragraphs (with lazy continuation)
- ATX headings (`#` through `######`)
- Setext headings (`===` and `---` underlines)
- Blank lines
- Fenced code blocks (` ``` ` and `~~~`), with info strings carrying
  language + Quarto-style attribute block
- Indented code blocks (4-space)
- Thematic breaks (`---`, `***`, `___`)
- Blockquotes (`>`, including lazy and nested)
- Unordered lists (`-`, `*`, `+`) with tight / loose detection
- Ordered lists (`1.`, `1)`) with tight / loose detection and `start` attribute

**Inline (NB1):**

- Text runs with HTML entity passthrough
- Emphasis (`*`, `_`) and strong (`**`, `__`)
- Inline code spans (`` ` ``, with multi-backtick delimiters)
- Inline links (`[text](url "title")`)
- Inline images (`![alt](src "title")`)
- Autolinks (`<https://...>`, `<user@host>`)
- Hard breaks (trailing `\` or two spaces)
- Soft breaks
- Backslash escapes for ASCII punctuation

**GFM extensions (NB2):**

- Pipe tables with `:--` / `--:` / `:--:` alignment markers
- Task list items (`- [ ]`, `- [x]`)
- Strikethrough (`~~`)

### Deferred (renders as literal source)

- HTML blocks and inline raw HTML (substantial spec; not needed for
  notebooks; future spice `tur-notebook-html-passthrough` if demand
  appears)
- Reference-style links and images (two-pass parser; rarely written by
  hand)
- Link reference definitions
- Footnotes (GFM extension; can be added in v0.2)
- Definition lists
- ~~Math blocks (`$$ ... $$`)~~ -- implemented in KaTeX plan
  ([`docs/notebook-katex-plan.md`](../../notebook-katex-plan.md))

### Size estimate

A working subset implementation is on the order of 1500-2000 lines of
Turmeric, dominated by inline emphasis-pair resolution (the most fiddly
part of CommonMark). The block parser is straightforward line-by-line
state. Tests piggyback on the public CommonMark spec test suite, filtered
to the included feature set.

### Why this scope and not full CommonMark

Full CommonMark is well-specified but large -- the reference implementation
(cmark) is ~6kLoC of careful C. Most of that complexity lives in the
features we are explicitly deferring (HTML block sniffing, reference link
definitions, edge cases in nested list lazy continuation). For *notebooks
about Turmeric code*, the included subset covers everything users actually
write. Shrinking the scope buys us a parser we can finish, test, and
maintain in pure Turmeric.

---

## Design notes

### Why one binary with subcommands

The renderer and TUI share parser, evaluator, cache, and session code; the
divergence is small enough that two binaries would mostly duplicate setup.
A single `tur-nb` with subcommands matches `git`, `cargo`, `gh`, and is the
shape Turmeric users already know from the main `tur` driver.

### How the TUI captures output

Each cell evaluation installs a stdout capture hook on the session, runs
the source, and reads back the captured buffer. The hook is implemented in
inline-C as a `fflush` + `freopen` to a `tmpfile()` per cell -- robust
across `printf`, `fprintf(stdout, ...)`, and inline-C `puts`. Stderr is
captured the same way. Last-expression value comes from libturi's
existing REPL eval entry point, which already returns a printable form.

### Why no in-process editor

Building a real text editor (multi-line, UTF-8, undo, paste, syntax-aware
indent) is on the order of a thousand lines of careful code, and we would
forever be re-inventing decisions the user has already made in their
`vim`/`helix`/`emacs` config. Shelling out to `$EDITOR` is the same pattern
used by `git commit`, `crontab -e`, `gh pr edit`, and `visidata`. It cleanly
inherits the user's plugins, theme, and muscle memory.

### Why mode-based and not chord-based

Modal navigation (Jupyter, vim, less) gives us single-key bindings for the
high-frequency actions (`Enter`, `e`, `R`). A chord-based scheme (`Ctrl-X
Ctrl-E`) would be more keystrokes per action and would conflict with
`$EDITOR`'s own bindings once the user is inside it. The mode itself is
indicated by a one-character cue in the status bar (`-- COMMAND --` /
`-- EDITING (cell-id) --`) so users always know which mode they are in.

### Reproducibility and seeds

Notebooks that use randomness (any cell calling into `tur-stats`'s `rng-*`
or any future PRNG) should pass an explicit seed. The notebook tooling
does not auto-seed. Reasons: implicit auto-seeding would make notebooks
that *look* reproducible silently non-reproducible the moment they are
edited; explicit `(rng-make 42)` in user code is unambiguous. The
notebook guide spells this out.

### Why HTML render reuses gendocs CSS

The stdlib API reference, guides, and (per `per-spice-docs-plan.md`) spice
docs all share one stylesheet. Notebook output naturally belongs in the
same visual family -- a rendered analysis is documentation. Reusing the
CSS is free and avoids a parallel style system.

### Watch-mode rendering

`--watch` calls `notebook/render-md` (or `--to html`) once on launch, then
listens for write events on the source file via `kqueue` / `inotify`. On
each event it debounces 150 ms (handles editors that write atomically via
rename) and re-renders. State across renders is *not* shared -- each
re-render is a fresh session -- because watch mode is for "I'm writing
prose, just keep the rendered file fresh" and predictability matters more
than warm caches. For warm caches use the TUI.

---

## Risks and open questions

1. **Output capture and inline-C.** Capturing stdout via `freopen(tmpfile)`
   is portable but interferes with anything that holds a `FILE*` to the
   original stdout. We document that cells must not stash `stdout` across
   cells (a contrived case, but worth calling out).

2. **Terminal image protocols are not universal.** Kitty, iTerm2, WezTerm,
   Konsole, and (sometimes) Alacritty all use different escape sequences
   for inline images; many terminals support none. NB9's fallback chain
   (Kitty graphics -> sixel -> placeholder) leaves users on plain xterm
   without inline images; they see `[image: foo-cell-3.png]` and can open
   the file. The renderer (`tur nb render`) produces fully-portable HTML or
   markdown-with-base64, so the TUI's limitation does not affect shareable
   artifacts.

3. **Session restart cost.** `R` (restart and re-run) re-imports every
   spice the notebook touches; for large dependency trees this can take
   seconds. Acceptable for v0.1.0; a longer-term optimization is to fork
   from a snapshot taken at session-open, but Turmeric's interpreter
   does not currently expose that hook.

4. **Parser scope creep.** The included CommonMark subset is the *most
   likely place* the v0.1.0 plan slips, because every notebook author who
   tries to write something unusual (a nested admonition, a footnote, a
   raw `<details>` block) is one issue away from arguing the missing
   feature is essential. Mitigation: NB0/NB1/NB2 each ship against a
   frozen subset of the CommonMark spec test suite, and any addition
   requires an explicit revision to the [Parser scope](#parser-scope)
   section before code lands. Features outside the list are tracked as
   "deferred" and addressed in v0.2 or a follow-up extension spice, not
   smuggled into v0.1.0.

5. **Multi-cell undo.** v0.1.0 has no undo for cell-level edits (insert,
   delete, paste). The save-on-quit prompt protects against accidental
   data loss; full undo is tracked for v0.2.

---

## Shared work

### turmeric-spices README

Add row once v0.1.0 is tagged:

| Spice | Description | Tier | Depends on |
|-------|-------------|------|------------|
| `tur-notebook` | Literate `.tur.md` notebooks: renderer (md/html) + interactive TUI | 1 -- pure Turmeric | (none) |

### Guide

Deliver `docs/guides/notebook-guide.md` alongside the `v0.1.0` tag. Sections:

1. Your first `.tur.md` (cells, attributes, running it)
2. Rendering: markdown vs HTML, watch mode
3. The TUI: command mode tour, editing via `$EDITOR`, restart vs partial re-run
4. Caching expensive cells (`cache=true`, `depends=`)
5. Embedding plots and images
6. Reproducibility: seeds, frozen outputs, `tur nb exec` in CI
7. Customizing keybindings

### Integration notes

- Pair with `tur-frame` for data loading cells (`read-csv` is the prototypical
  expensive cell that benefits from `cache=true`).
- Pair with `tur-stats` for reproducible analyses (deterministic seeds make
  notebook outputs byte-stable across re-renders, so diffs in version control
  are meaningful).
- Pair with `tur-plot` for inline figures (NB9 image hook is wired through
  `plot-write-png` automatically; in the TUI on a supporting terminal,
  figures display inline below the cell).
- The `per-spice-docs-plan.md` HTML pipeline can ingest rendered `.tur.md`
  files as guide pages; this is a follow-up integration once both plans
  land.

---

## Future spices

| Spice | Purpose | Why separate |
|-------|---------|--------------|
| `tur-notebook-web` | Browser-based notebook UI (WebSockets + `tur-notebook` core) | Different runtime model; large JS asset surface |
| `tur-notebook-vscode` | VS Code extension wrapping `tur-nb` | Editor-specific lifecycle, marketplace packaging |
| `tur-notebook-export` | Export to PDF (via wkhtmltopdf or weasyprint) and `.ipynb` | Pulls heavy external tools |
| `tur-notebook-slides` | RevealJS-backed slide output (`tur nb render --to slides`) | Different layout engine |
