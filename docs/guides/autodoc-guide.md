---
title: Autodoc Guide
category: CLI Tools
description: Writing `;;;` docstrings and generating API docs with `tools/gendocs.py`
tags:
  - docstrings
  - api-docs
  - documentation
  - gendocs
---

# Autodoc Guide

Turmeric's documentation system has three layers:

1. **`;;;` docstrings** -- triple-semicolon comments above any `defn`, `defmacro`,
   `defstruct`, `defclass`, `definstance`, or `defopaque`.
2. **HTML API docs** -- `tools/gendocs.py` parses the docstrings and emits a
   styled site under `docs/html/api/`.
3. **Runtime `(doc name)`** -- looks up the docstring for a name at the REPL or
   in the web playground. The same generator can emit `stdlib/docstrings.tur`,
   the lookup table `(doc name)`, `tur doc`, and the playground doc panel read.

This guide covers all three.

---

## Writing docstrings

Use `;;;` (triple-semicolon), distinct from `;;` (regular inline comment).
A docstring block must sit **immediately above** the definition it documents --
a blank line or any non-`;;;` line resets the buffer.

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
|---|---|---|
| One-line summary (first `;;;` line) | Yes | `;;; name -- brief description` |
| `Parameters:` block | If non-zero arity | One `;;;   name -- desc` line per param |
| `Returns:` | Yes, unless `:void` | Describe the return value |
| `Example:` | Yes | At least one usage example with `; => result` |
| `Methods:` | On a `defclass` | One `;;;   name -- desc` line per method |
| `Laws:` | On a `defclass` with laws | The equations, one per line |
| `Since:` | When known | Phase tag, e.g. `Phase B1` |

### Conventions

- First line: `;;; name -- brief summary` -- the name is repeated so the line is
  greppable.
- Blank `;;;` lines separate sections, like Python docstrings.
- Examples use `; => result` to show expected output.
- Prose is kept. Paragraphs between the summary and the first section render
  under the summary; paragraphs after a section (`Superclass: Eq.`, a
  `Note:`) render after the example. A section ends at the next header or at
  an **unindented** paragraph after a blank line, so keep section bodies
  indented.
- Inside prose, an indented run renders as a code block, and lines starting
  with `- ` render as a bullet list.
- **ASCII only** -- use `--` (double hyphen), never em dashes. The Turmeric
  parser hangs on non-ASCII bytes.
- Internal helpers (e.g. `tur-contract-check`, `__functor_*`) get a one-liner
  only -- no Parameters/Returns/Example blocks needed.

### Docstring levels

| Audience | Format |
|---|---|
| Exported / public API | Full block (summary + Parameters + Returns + Example + Since) |
| Internal helpers | Single-line `;;; name -- what it does` |
| Typeclasses (`defclass`) | Summary + Parameters (the type parameters) + Methods + Laws + Example + Since |
| Typeclass instances (`definstance`) | Single-line summary |

### Typeclasses

A `defclass` docstring documents the class and every method. The generator
reads the method signatures from the form itself, so a `Methods:` entry only
has to say what the method does; the card shows each signature with its
description, the `Laws:` block verbatim, and an **Instances** line listing
every type the stdlib gives an instance, linked to that `definstance`.

```turmeric
;;; Functor -- map a function over every element of a container, keeping its shape.
;;;
;;; Parameters:
;;;   ^f -- the container's type constructor (kind * -> *)
;;;
;;; Methods:
;;;   fmap -- a container of the same shape as `container` with `g` applied to
;;;           each element.
;;;
;;; Laws:
;;;   (fmap c (fn [x] x))      = c
;;;   (fmap (fmap c g) h)      = (fmap c (fn [x] (h (g x))))
;;;
;;; Example:
;;;   (unwrap-or (fmap (some 20) (fn [x : int] : int (+ x 1))) 0)   ; => 21
;;;
;;; Since: Phase CCL
(defclass Functor [^f] (fmap [container : (f a) g : (fn [a] b)] : (f b)))
```

A `Methods:` entry may name several methods that share a description
(`lt? lte? -- ...`). A method can instead be documented in place, with a `;;;`
block directly above it inside the class body; that wins over a `Methods:`
entry for the same name. A class is always listed as public API: classes are
global, so an export list never names one.

`(doc 'Functor)` prints the class with its method signatures, and
`(doc 'fmap)` prints the method -- unless a function of the same name exists,
which keeps the name.

### Special forms and builtins

A name the compiler implements itself -- `+`, `ptr-add`, `rc/of`, `handle` --
has no definition for a docstring to sit on. Document it in `stdlib/forms/`
with a `;;;` block over a **doc-only declaration**: a `;;@builtin` line in the
`defn` signature grammar for something called like a function, or a `;;@form`
line holding the syntax as written for a special form. Both are `;;` comments,
so the compiler skips them; `tools/gendocs.py` reads them.

```turmeric
;;; ptr-add -- advance a pointer by a number of BYTES.
;;;
;;; `(char *)p + n`: the offset is in bytes, not words.
;;;
;;; Parameters:
;;;   p -- the base address
;;;   n -- the offset in bytes
;;;
;;; Returns:
;;;   The address `n` bytes past `p`.
;;;
;;; Example:
;;;   (let [p (raw-malloc 16)] (raw-free p))
;;@builtin (ptr-add [p : ptr<void> n : int] : ptr<void>)

;;; unsafe -- run code that uses the unsafe builtins.
;;; ...
;;@form (unsafe body ...)
```

Consecutive declaration lines for one name are one entry: a form with several
shapes, or a builtin with several overloads, writes one line per shape and the
card shows them all. The doc-lookup entry carries the signature too, since
there is no source to read it from, and a stdlib entry wins over the one-line
builtin table in `src/turi/docstrings.c` (`tur_doc_prefer`).

`tests/check-forms-documented.sh` (ctest `tur_forms_documented_lint`) keeps
the set complete: every reserved special form and public builtin must be
documented, or be listed in `tests/forms-undocumented.txt`, which may only
shrink; and a declaration must name something the compiler knows.

### Module docstrings

A contiguous `;;;` block at the very top of a file -- **before the first real
definition** -- becomes the module docstring. Terminate it with a `;;` line or
any non-comment, non-blank form (e.g. `(defmodule ...)`, `(export ...)`). A
blank line also terminates it when its summary names the module
(`;;; tur/list -- ...`), so the first definition's own block can follow.

```turmeric
;;; tur/list -- untyped singly-linked Cons/nil list.
;;;
;;; Legacy list implementation; prefer tur/list for new code.
;;;
;;; Since: Phase B1
;; List type for Turmeric                  <-- terminates the module block
(defstruct Cons ...)
```

The generator renders the module block as the description paragraph on the
per-module HTML page and registers the module name in the runtime lookup
table, so `(doc 'tur/list)` prints the summary.

---

## Generating docs

The `tur run docs` recipe (or `python3 tools/gendocs.py` directly) regenerates
the HTML site and the runtime lookup table.

```sh
tur run docs

# Equivalent direct invocation:
python3 tools/gendocs.py stdlib/ \
    --out docs/html/api/ \
    --emit-tur stdlib/docstrings.tur
```

The `tur run docs` recipe additionally emits `web/public/doc-names.json`
(the web REPL search index) via `--emit-json`, folding in spice symbols
with `--extra-json`.

**Generated artifacts live under `docs/html/api/` -- do not edit by hand.**
Each regen overwrites the tree.

### Output layout

```
docs/html/api/
  index.html          -- module index with one-liners
  tur-list.html       -- per-module page
  tur-option.html
  ...
  style.css           -- shared stylesheet
```

Each per-module page has:

- The module docstring as a header paragraph.
- A sidebar TOC of exported names with anchor links.
- One card per exported definition: kind badge (`defn` / `defmacro` /
  `defstruct` / `defclass` / `definstance`), signature, summary, description,
  Parameters, Methods (a class), Returns, Laws, Example, notes, Instances (a
  class), Since.
- A collapsed "Internal" section for non-exported definitions.

### Single-module regen

When iterating on docstrings in a single file:

```sh
python3 tools/gendocs.py stdlib/list.tur --out docs/html/api/
```

---

## Runtime `(doc name)`

`stdlib/docstrings.tur` is **auto-generated** by `gendocs.py --emit-tur`. It
defines the `tur/docstrings` module, whose `doc-lookup` function maps a name
to its docstring via a static table:

```
tur> (doc cons)
cons -- allocate a cons cell: a head word and a tail.

(cons [head :T tail :int] :int)

The cell is the `{head, tail}` pair of 8-byte words that `*args*`,
variadic rest parameters and tur/list's untyped lists are built from.  The
head may be any word-sized value; the tail is another cell or 0, the empty
list.  Read it back with `car` / `cdr` (tur/list).  The result is a bare
`int`, so the type checker cannot help; prefer `(List T)` from
tur/list-typed in new code.

Parameters:
  head -- the first element
  tail -- the rest of the list, or 0

Returns:
  The new cell, as an `int`.

Example:
  (car (cdr (cons 1 (cons 2 0))))  ; => 2
```

`(doc name)` accepts a bare symbol (`(doc cons)`), a quoted one
(`(doc 'cons)`), or a string (`(doc "cons")`). The macro reads the name at
expansion time and expands to `(doc-print "cons")`, which prints the entry --
or, for a special form with no stdlib entry yet, its one-line description
from `src/turi/docstrings.c` -- or a "No documentation found." message.

`doc-print` and `doc-lookup` are interpreter natives, available in `tur repl`,
`tur --interpret`, and the web playground with no import. They read the
generated file directly from C (`src/turi/docstrings.c`) rather than running
the table's inline-C body, which the interpreter cannot execute. A compiled
program has no `doc-print`; from a shell, use `tur doc cons`, which reads the
same file.

---

## Web REPL integration

The Try Turmeric web app reads the same table from the WASM build.
`src/web/wasm_glue.c` exports:

```c
EMSCRIPTEN_KEEPALIVE
const char *turi_doc_lookup(const char *name);
```

The doc panel calls it when you pick a name from the docs search, follow a
symbol link, run `:doc name` or `(doc name)`, or hover a name the search index
has no summary for, and renders the result. It checks the builtin table, then reads
`stdlib/docstrings.tur` from the stdlib tree the build embeds. It never
evaluates anything, so a lookup cannot change the program you are running.

Because the lookup reads the WASM module's own embedded files (not a network
fetch), `(doc name)` works offline and in embedded deployments.

---

## CI and regeneration discipline

- `tur run wasm` depends on `tur run docs` so the WASM build always carries an
  up-to-date `stdlib/docstrings.tur`.
- Treat `docs/api/` and `stdlib/docstrings.tur` as build artifacts: regenerate
  them in the same commit as the docstring change, never in a follow-up.
- A non-`;;;` line resets the parser's docstring buffer. If a docstring is not
  showing up, check for a stray blank line or `;;` comment between it and its
  definition.

---

## See also

- `tools/gendocs.py` -- the generator script
- `stdlib/docstrings.tur` -- the auto-generated lookup table (do not edit)
- [REPL Guide](repl.md) -- using `(doc name)` interactively
