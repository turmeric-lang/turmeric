---
title: Notebook Guide
category: Editor and IDE
description: Literate-programming .tur.md notebooks with tur-notebook -- TUI interface, HTML export, and scripted exec mode
---

# Turmeric Notebook Guide

`tur-notebook` brings literate-programming `.tur.md` notebooks to the Turmeric
ecosystem -- think Jupyter, but backed by the Turmeric evaluator with a
terminal-first TUI, standalone HTML export, and a scriptable `exec` mode.

---

## Installing

Clone `turmeric-spices` next to your `turmeric` checkout:

```sh
git clone https://github.com/turmeric-lang/turmeric-spices ../turmeric-spices
```

Declare the spice in your project's `build.tur`:

```turmeric no-check
:spices #map{
  "notebook" #map{:url    "https://github.com/turmeric-lang/turmeric-spices"
                  :ref    "notebook-v0.1.0"
                  :subdir "spices/notebook"}
}
```
```sweet-exp
:spices
#map{
  "notebook" #map{:url    "https://github.com/turmeric-lang/turmeric-spices"
                  :ref    "notebook-v0.1.0"
                  :subdir "spices/notebook"}
}
```

Then run `tur add` or `tur build` to fetch and link it.

---

## Quick Start

### Create a new notebook

```sh
nb new my-notes.tur.md
```

This writes a starter `.tur.md` file with a prose heading and one
`turmeric` code fence.

### Open the TUI

```sh
nb tui my-notes.tur.md
```

Navigate cells with `j`/`k`, edit in your `$EDITOR` with `e`, evaluate with
`Enter`, and quit with `q` (you will be prompted if there are unsaved changes).
Press `?` for the full key binding list.

### Run cells non-interactively

```sh
# Evaluate every cell and print outputs
nb exec --all my-notes.tur.md

# Evaluate from a specific cell onward
nb exec --cell my-cell-id my-notes.tur.md
```

### Export to HTML

```sh
nb export my-notes.tur.md -o my-notes.html
```

Produces a self-contained HTML file with the vendored stylesheet.

### Render to terminal markdown

```sh
nb render my-notes.tur.md
```

---

## Notebook File Format

A notebook is a Markdown file where Turmeric code blocks are designated with
an info string that starts with `turmeric`:

````markdown
# My Notebook

Some prose.

```turmeric {id=setup}
(def x 42)
```

```turmeric {id=use-x depends=setup}
(+ x 1)
```
````

### Cell attributes

Attributes appear in a `{...}` block after `turmeric` on the opening fence:

| Attribute | Type | Description |
|-----------|------|-------------|
| `id` | string | Unique cell identifier (required for `exec --cell` and caching) |
| `eval` | `true`/`false` | Set to `false` to skip evaluation (default: `true`) |
| `depends` | string | Space-separated list of cell ids this cell depends on |
| `cache` | `true`/`false` | Cache output; re-use unless dependencies change (default: `false`) |
| `error` | `continue` | Continue evaluating subsequent cells if this cell errors |

---

## Cell Types

### Turmeric cells

Cells with the `turmeric` language tag are evaluated by the embedded Turmeric
interpreter session. All cells in a notebook share one session, so bindings
defined in earlier cells are visible in later ones.

### Prose cells

Everything outside a code fence is prose. Prose is rendered as Markdown in
the TUI and included verbatim in the HTML export.

---

## exec Subcommand

`nb exec` is the non-interactive entry point for running notebooks from
scripts, CI, or editor integrations.

```sh
# Evaluate all cells
nb exec --all notebook.tur.md

# Evaluate from a named cell onward
nb exec --cell analysis notebook.tur.md
```

Outputs are printed to stdout in the format `[<cell-id>] => <repr>`.
Error outputs are printed to stderr.
Exit code is 0 on success, 1 on evaluation or I/O error.

---

## new Subcommand

```sh
nb new my-notebook.tur.md
```

Writes a minimal starter notebook to the given path. Refuses to overwrite an
existing file (exit code 1 if the path already exists).

---

## Image Hook

Cells can advertise image files to the TUI via the image hook convention:

```turmeric
(import notebook/image :refer [image-hook-record-path])

;; After generating /tmp/plot.png:
(image-hook-record-path "/tmp/plot.png")
```
```sweet-exp
import notebook/image :refer [image-hook-record-path]
;; After generating /tmp/plot.png:
image-hook-record-path("/tmp/plot.png")
```

The TUI reads the advertised paths and displays the images inline using:

- **Kitty graphics protocol** -- if `$TERM` is `xterm-kitty`
- **iTerm2 inline images** -- if `$TERM_PROGRAM` is `iTerm.app`
- **Text fallback** -- prints the file path for all other terminals

The image hook works by printing a `__NB_IMG__: <path>` marker to stdout,
which the session evaluator captures and strips before returning output.

---

## Math (KaTeX)

Notebooks support LaTeX math via [KaTeX](https://katex.org). Math is rendered
client-side in the browser when you export to HTML.

### Inline math

Wrap LaTeX between single `$` delimiters on the same line:

```markdown
The variance is $\sigma^2 = \frac{1}{n}\sum_{i=1}^{n}(x_i - \mu)^2$.
```

An opening `$` is only recognized when it is **not** immediately followed by a
digit or whitespace, so currency amounts like `$5` and `$10` are left as
literal text.

### Display math

Wrap multi-line expressions between `$$` on their own lines:

````markdown
The Gaussian PDF is

$$
f(x) = \frac{1}{\sigma\sqrt{2\pi}}\,
       e^{-\tfrac{1}{2}\left(\tfrac{x-\mu}{\sigma}\right)^2}
$$
````

A one-line form `$$ E = mc^2 $$` is also accepted.

### HTML export

Math is injected automatically during `export html`:

```sh
tur nb export html notebook.tur.md
```

The `--math` flag controls KaTeX injection:

| Flag | Behavior |
|------|----------|
| `--math=auto` (default) | Inject KaTeX only if the document contains math nodes |
| `--math=always` | Always inject (useful when cells emit math at runtime) |
| `--math=never` | Never inject; math nodes render as raw `$$...$$` source |

By default the KaTeX CSS and JS are **inlined** into the HTML file so it works
offline. Use `--assets=sibling` to write `katex.min.css`, `katex.min.js`, and
`auto-render.min.js` as separate files next to the HTML instead:

```sh
tur nb export html notebook.tur.md --assets=sibling
```

Font files (`katex-fonts/`) are always written as siblings regardless of mode.

### Cell output math (`math=true`)

Add `math=true` to a cell's fence attributes to have the cell's stdout
rendered as math rather than preformatted text:

````markdown
```turmeric {math=true}
(println "$$ E = mc^2 $$")
```
````

The cell output is scanned for `$$...$$` and `$...$` spans and wrapped in
`<div class="math-output">`. This also implies `--math=always` for the page
even if no prose math nodes are present.

### Unsupported macros

KaTeX implements a large subset of LaTeX but not everything. If you hit an
unsupported macro (`\newcommand` chains, `tikzpicture`, etc.) the expression
renders in red with the error message. As a fallback, generate the math as an
image from your preferred TeX engine and embed it via the image pipeline:

```turmeric
(import notebook/image :refer [image-hook-record-path])
;; generate /tmp/eq.png from LaTeX here ...
(image-hook-record-path "/tmp/eq.png")
```
```sweet-exp
import notebook/image :refer [image-hook-record-path]
;; generate /tmp/eq.png from LaTeX here ...
image-hook-record-path("/tmp/eq.png")
```

The full list of supported KaTeX functions is at
<https://katex.org/docs/supported.html>.

---

## TUI Key Bindings

| Key | Action |
|-----|--------|
| `j` / `k` | Move down / up |
| `e` | Edit focused cell in `$EDITOR` |
| `Enter` | Evaluate focused cell |
| `o` | Toggle focused-output view |
| `i` | Insert cell below |
| `d` | Delete focused cell |
| `p` | Paste cell below |
| `/` | Start search |
| `n` / `N` | Next / previous search match |
| `?` | Show help overlay |
| `q` | Quit (prompts if unsaved changes) |

Custom key bindings can be provided with `--keybindings <file.tur>`, which is
merged onto the built-in map.

---

## Further Reading

- [`docs/notebook-spice-plan.md`](https://github.com/turmeric-lang/turmeric/blob/main/docs/archive/history/notebook-spice-plan.md) -- full milestone plan
- [`../turmeric-spices/spices/notebook/README.md`](https://github.com/turmeric-lang/turmeric-spices/blob/main/spices/notebook/README.md) -- spice README
- [`developing-spices-guide.md`](developing-spices-guide.md) -- how to develop and publish spices
