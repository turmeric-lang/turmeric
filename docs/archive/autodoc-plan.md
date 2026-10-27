# Auto-Generated Documentation Plan

## Overview

Four interconnected goals:

1. **Docstrings** — establish a standard `;;;` doc-comment format and add them to all stdlib files
2. **Doc generator** — a `tools/gendocs.py` script that parses `.tur` files and emits styled HTML
3. **Runtime `doc` lookup** — a `(doc fn-name)` macro that prints documentation in the REPL/at runtime
4. **Try Turmeric integration** — the web app links to the generated docs, and `(doc name)` works inside the browser REPL

---

## 1. Docstring Standard

### Format

Use `;;;` (triple-semicolon) as the doc-comment marker, distinct from `;;` (regular inline comment). A docstring block immediately precedes a `defn`, `defmacro`, `defstruct`, or `definstance` definition.

```turmeric
;;; cons -- prepend a value to a list.
;;;
;;; Parameters:
;;;   value -- the element to prepend
;;;   next  -- the existing list (or nil-value for empty)
;;;
;;; Returns:
;;;   A new Cons cell pointing to next.
;;;
;;; Example:
;;;   (cons 1 (cons 2 (nil-value)))  ; => (1 2)
;;;
;;; Since: Phase B1
(defn cons [value next] : int
  ...)
```

### Required fields

| Field | Required | Notes |
|-------|----------|-------|
| One-line summary (first `;;;` line) | Yes | Appears in index and hover tooltips |
| `Parameters:` block | If non-zero arity | One `;;;   name -- desc` line per param |
| `Returns:` | Yes, unless `:void` | Describe the return value |
| `Example:` | Yes | At least one usage example |
| `Since:` | When known | Phase tag, e.g. `Phase B1` |

### Conventions

- First line: `;;; name -- brief summary` (name repeated for greppability)
- Blank `;;;` lines separate sections (like blank lines in Python docstrings)
- Examples use `; => result` comment to show expected output
- Internal helpers (e.g. `tur-contract-check`) get a shorter one-liner only

### Files to document (53 top-level stdlib files)

Priority order:

**Tier 1 — Core, used everywhere**
- `list.tur`, `option.tur`, `result.tur`, `pair.tur`, `str.tur`, `vec.tur`
- `macros.tur`, `contract.tur`, `hamt.tur`, `map.tur`

**Tier 2 — Concurrency and effects**
- `chan.tur`, `mutex.tur`, `rwlock.tur`, `condvar.tur`, `stm.tur`
- `thread.tur`, `threadpool.tur`, `taskgroup.tur`, `fiber.tur`
- `effects.tur`, `async_file.tur`, `async_pipe.tur`, `async_socket.tur`
- `scheduler.tur`, `scheduler_mt.tur`

**Tier 3 — Functional / typeclass**
- `functor.tur`, `applicative.tur` (if present), `monad.tur` (if present)
- `typeclass.tur`, `arrow.tur`, `arrow_laws.tur`, `comonad.tur`, `free.tur`
- `backtrack.tur`, `logic.tur`, `zipper.tur`, `parsec.tur`

**Tier 4 — Utilities and I/O**
- `io.tur`, `log.tur`, `time.tur`, `timer.tur`, `random.tur`
- `ref.tur`, `rc.tur`, `atomic.tur`, `sync.tur`, `select.tur`
- `slice.tur`, `grid.tur`, `serial.tur`, `safe.tur`
- `capability.tur`, `future.tur`, `workflow.tur`
- `raylib.tur`, `test.tur`

**Tier 5 — Subdirectory modules**
- `scscm/`, `tidal/`, `turi/`, `test/`
  *(Note: `signal/` extracted to `tur-signal` spice)*

---

## 2. Doc Generator (`tools/gendocs.py`)

### Architecture

```
stdlib/*.tur  ──┐
stdlib/**/*.tur ┤
                ├─► parser ──► AST model ──► HTML emitter ──► docs/api/
                │                         └─► docstrings.tur (for goal 3)
tools/gendocs.py
```

### Parser

A standalone Python script (no external deps beyond stdlib) that reads `.tur` source files and extracts:

1. **Module name** — from `(defmodule tur/foo ...)`
2. **Exports** — the `(export ...)` list
3. **Definitions** — each `defn`, `defmacro`, `defstruct`, `definstance`
   - Name
   - Parameters with type annotations (`:bool`, `:int`, `:ptr<void>`, etc.)
   - Return type
   - Whether it is exported
4. **Docstrings** — the `;;;` block immediately above a definition

The parser is line-oriented (no full S-expr parser needed for extraction). Algorithm:

```
for each .tur file:
  scan for defmodule name
  scan for (export ...) — may span multiple lines; accumulate until closing )
  for each line:
    if starts with ;;; → accumulate into current docstring buffer
    elif starts with (defn / (defmacro / (defstruct / (definstance:
      attach accumulated docstring to this definition
      reset docstring buffer
    else:
      reset docstring buffer (non-;;; line breaks docstring attachment)
```

### Output structure

```
docs/api/
  index.html          ← module index with one-liners
  tur-list.html       ← per-module page
  tur-option.html
  tur-result.html
  ...
  style.css           ← shared stylesheet
```

### Per-module page layout

```
[header: Turmeric stdlib / tur/list]

[module description paragraph]

[sidebar: TOC — exported names only, anchored links]

[content: for each exported definition]
  ┌───────────────────────────────────────┐
  │ defn cons                             │  ← kind badge
  │ (cons value next) → :int             │  ← signature line
  │                                       │
  │ Prepend a value to a list.            │  ← one-liner
  │                                       │
  │ Parameters                            │
  │   value  the element to prepend       │
  │   next   the existing list (or nil)   │
  │                                       │
  │ Returns                               │
  │   A new Cons cell pointing to next.   │
  │                                       │
  │ Example                               │
  │   (cons 1 (cons 2 (nil-value)))       │
  │   ; => (1 2)                          │
  │                                       │
  │ Since: Phase B1                       │
  └───────────────────────────────────────┘

[non-exported definitions listed in a collapsed "Internal" section]
```

### Design / color scheme

Inspired by turmeric spice — warm yellows, deep oranges, earthy browns — on a dark background.

**Palette**

| Token | Hex | Usage |
|-------|-----|-------|
| `--gold`       | `#E8A020` | Module names, headings, links |
| `--orange`     | `#C4581A` | Kind badges (defn / defmacro / defstruct) |
| `--cream`      | `#F5E6C8` | Body text |
| `--faint`      | `#8A7560` | Secondary text, since tags, internal labels |
| `--bg-dark`    | `#1A1208` | Page background |
| `--bg-card`    | `#261C10` | Definition card background |
| `--bg-code`    `| `#0F0A05` | Code block background |
| `--border`     | `#3D2A18` | Card borders, rule lines |
| `--highlight`  | `#4A3010` | Hovered card background |

**Typography**
- Body: system-ui or Inter (loaded from local only — no CDN)
- Code: `'JetBrains Mono', 'Fira Code', monospace`
- Headings: slightly larger, `--gold` color

**Kind badges** (pill labels left of definition name)
- `defn` → amber pill
- `defmacro` → orange pill
- `defstruct` → olive pill
- `definstance` → muted teal pill

### CLI usage

```sh
# Generate all docs
python3 tools/gendocs.py stdlib/ --out docs/api/

# Generate single module
python3 tools/gendocs.py stdlib/list.tur --out docs/api/

# Also emit docstrings.tur (for goal 3)
python3 tools/gendocs.py stdlib/ --out docs/api/ --emit-tur stdlib/docstrings.tur
```

### Makefile target

```makefile
docs:
	python3 tools/gendocs.py stdlib/ --out docs/api/ --emit-tur stdlib/docstrings.tur

.PHONY: docs
```

---

## 3. Runtime `doc` Lookup

### Approach: generated lookup table

At doc-generation time, `gendocs.py --emit-tur` writes `stdlib/docstrings.tur`:

```turmeric
;; AUTO-GENERATED — do not edit. Run: make docs
(defmodule tur/docstrings
  (export doc-lookup)

(def doc-table
  (hamt-set (hamt-set (hamt-new)
    "cons" "cons -- prepend a value to a list.\n\nParameters:\n  ...")
    "head" "head -- get the first element of a list.\n\n...")
  ...)

(defn doc-lookup [name : cstr] : cstr
  (hamt-get doc-table name))
)
```

### `(doc)` macro in `macros.tur`

```turmeric
;;; doc -- print documentation for a function or macro.
;;;
;;; Parameters:
;;;   name -- symbol or string name to look up
;;;
;;; Example:
;;;   (doc cons)     ; prints: cons -- prepend a value to a list. ...
;;;   (doc "cons")   ; same, string form
;;;
;;; Since: Phase D1 (auto-docs)
(defmacro doc [name]
  (let [entry (doc-lookup (str name))]
    (if (some? entry)
      (println entry)
      (println (str "No documentation found for: " name)))))
```

Dependency: `macros.tur` imports `tur/docstrings`. Since `tur/` modules are auto-loaded, `doc` is globally available.

### REPL integration

In a REPL session:

```
tur> (doc cons)
cons -- prepend a value to a list.

Parameters:
  value  the element to prepend
  next   the existing list (or nil-value for empty)

Returns:
  A new Cons cell pointing to next.

Example:
  (cons 1 (cons 2 (nil-value)))  ; => (1 2)

Since: Phase B1
```

### Release builds

When contracts are stripped (`--no-contracts`), the doc table is large but inert. A future `--no-docs` flag (Phase D2) can strip `docstrings.tur` from the auto-load list so release binaries shed the table entirely.

---

## 4. Try Turmeric Integration

### Web app links to the generated docs

The Try Turmeric site (`web/index.html`) already reserves a `[Docs]` nav slot. Once `docs/api/` is generated, wire it up:

- The `[Docs]` nav link points to `docs/api/index.html` (the module index).
- Each example snippet in the web editor gets a **"View docs"** link next to any stdlib function it uses, pointing to the relevant anchor on the module page (e.g. `docs/api/tur-list.html#cons`).
- A **doc panel** sits to the right of (or below) the output console. When `(doc name)` is evaluated in the browser REPL, the result is rendered in this panel as formatted HTML (pulled from the pre-built `docs/api/` pages or a companion `search-index.json`) rather than as plain text in the console. The plain-text fallback still goes to the console for parity with the CLI REPL.

```
┌──────────────────────────────────────────────────────────────┐
│  Turmeric   [Try REPL]  [Tutorial]  [Docs ←]  [GitHub]      │
├──────────────┬───────────────────────┬────────────────────────┤
│  Editor      │  Console              │  Doc Panel             │
│              │                       │                        │
│  (doc cons)  │  (evaluated silently) │  cons                  │
│              │                       │  Prepend a value to…   │
│              │                       │                        │
│              │                       │  Parameters            │
│              │                       │    value  …            │
│              │                       │    next   …            │
│              │                       │                        │
│              │                       │  [Open full docs ↗]    │
└──────────────┴───────────────────────┴────────────────────────┘
```

The "Open full docs" link opens the full module page in a new tab.

### `(doc name)` in the WASM build

The WASM build of libturi needs `stdlib/docstrings.tur` bundled into it at compile time, the same way other `tur/` stdlib files are. Changes required:

1. **`make docs` runs before `make wasm`** (or is a prerequisite target) so `stdlib/docstrings.tur` is up-to-date before Emscripten compiles.
2. **`src/main.c` auto-load list** — add `tur/docstrings` alongside the other implicitly loaded stdlib modules so it is available in all execution contexts including WASM.
3. **WASM `println` routing** — in the WASM build, `println` already routes to the console via an Emscripten `EM_JS` shim. `(doc name)` works out of the box through this path for the plain-text console output.
4. **Doc panel bridge** — additionally expose a JS-callable function from `wasm_glue.c`:
   ```c
   // Returns the raw doc string for `name`, or NULL if not found.
   // JS side calls this to populate the doc panel without printing to console.
   EMSCRIPTEN_KEEPALIVE
   const char *turi_doc_lookup(const char *name);
   ```
   The frontend calls `turi_doc_lookup("cons")` after every eval that matches `(doc ...)`, parses the result, and renders it in the doc panel as HTML.

5. **Offline / no-network** — the doc lookup goes through the WASM module itself (not a network fetch to `docs/api/`), so it works in the browser with no server and in embedded/offline deployments.

---

## Implementation Phases

| Phase | Work | Deliverable |
|-------|------|-------------|
| **D0** | Finalize `;;;` docstring spec; add to CLAUDE.md | This document |
| **D1** | Add docstrings to Tier 1 stdlib (10 files) | Annotated core files |
| **D2** | Write `tools/gendocs.py` parser + HTML emitter + CSS | `docs/api/` site |
| **D3** | `--emit-tur` flag; `stdlib/docstrings.tur` generation | Generated file |
| **D4** | `(doc name)` macro in `macros.tur`; REPL smoke test | Runtime lookup |
| **D5** | Wire `[Docs]` nav link + doc panel in `web/index.html` | Try Turmeric links |
| **D6** | `turi_doc_lookup` in `wasm_glue.c`; doc panel bridge | `(doc)` in browser REPL |
| **D7** | Docstrings for Tier 2–3 stdlib | Broader coverage |
| **D8** | Docstrings for Tier 4–5; subdirectory modules | Full coverage |
| **D9** | `make docs` target; prerequisite for `make wasm`; CI check | Automation |

---

## Open Questions

- **`docs/turmeric-homepage.html` and `docs/design-notes.md`**: These files don't exist yet. Once created, `gendocs.py` should pull the exact CSS color variables and font choices from the homepage so the API docs match the project site visually.
- **Search**: A client-side search box (no server) using a pre-built JSON index (`docs/api/search-index.json`) would be a nice D8 addition.
- **Cross-links**: `See also:` entries in docstrings should become `<a href>` links in the HTML output.
- **Versioning**: The `Since:` field currently tracks internal phase names. If Turmeric gains public version numbers, `gendocs.py` should map phases → versions using a table in `docs/phase-versions.md`.
