---
title: Formatter Guide
category: Editor and IDE
description: `tur format` CLI and web REPL Format button
---

# Code Formatting in Turmeric

Turmeric ships a built-in formatter that produces canonical, consistent style
for `.tur` source files. It is a pure pretty-printer: it reads source text,
walks the `Form` tree produced by the existing reader, and re-emits formatted
output. It never touches the elaborator or code generator, so it works on any
syntactically valid file regardless of type errors.

## CLI usage

```sh
# Format a file and print to stdout
tur format myfile.tur

# Format stdin and print to stdout
tur format < myfile.tur

# Overwrite a file in-place
tur format myfile.tur > myfile.tur.tmp && mv myfile.tur.tmp myfile.tur

# Check whether a file is already formatted (exits 1 if not)
tur format --check myfile.tur

# Show a unified diff of what formatting would change
tur format --diff myfile.tur
```

`tur format` reads with the reader the file's extension selects (`.tur.sweet`
is sweet-exp, `.scm` is Scheme), and a `#lang` line takes over when the
extension says plain Turmeric. Input on stdin carries no extension, so name
its dialect with `--lang` -- the same vocabulary as `tur fmt --lang`, which
takes a reader name (`sweet`, `neoteric`, `r7rs`, ...) or any base
`tur dialects` lists (`saffron/sweet`, `turmeric/neoteric`, ...):

```sh
tur format --lang sweet < buffer.tur.sweet
```

Prefer `tur fmt`, which has the same `--lang` and also formats in place.

The `--check` flag is useful in CI:

```sh
# Fail the build if any .tur file is not canonically formatted
find stdlib -name '*.tur' | xargs -n1 tur format --check
```

## Web REPL

The Try Turmeric web interface has a **Format** button in the editor toolbar
(between Clear and Share). It calls `turi_wasm_format()` from the WASM module
and replaces the editor contents with the formatted output.

Keyboard shortcut: `Alt+Shift+F` (or `Option+Shift+F` on macOS).

## Formatting rules

### Indentation

Two-space indentation by default. A form is emitted on one line if it fits
within the configured line width (default 80 columns); otherwise it is broken
into a block layout.

### Special forms

These forms have well-known argument roles and use custom layouts:

| Form | Layout |
|---|---|
| `defn` | `(defn name params :ret\n  body...)` |
| `defmacro` | same as `defn` |
| `fn` | `(fn params\n  body...)` |
| `let` | `(let [bindings...]\n  body...)` -- each binding pair on its own line |
| `if` | `(if test\n  then\n  else)` |
| `when` / `unless` | `(when test\n  body...)` |
| `do` | `(do\n  form...)` |
| `case` | `(case expr\n  pat result...)` -- each arm on its own line |
| `loop` | `(loop [bindings]\n  body...)` |
| `handle` | `(handle expr\n  arm...)` |
| `defclass` | `(defclass Name [params]\n  method...)` |
| `definstance` | `(definstance Name Class Type\n  impl...)` |
| `defeffect` | `(defeffect Name [] :ret)` |

### Regular function calls

- If the whole form fits on the current line, emit it inline.
- Otherwise emit the head and first argument on the same line, then indent
  remaining arguments by two spaces from the opening paren.

### Vectors, maps, sets

- `[...]` -- inline if total width fits; otherwise one element per line.
- `#map{...}` maps -- inline if total width fits; otherwise one key-value pair
  per line.
- `#set{...}` sets -- inline if total width fits; otherwise one element per
  line. (The `#s(...)` set form round-trips as-is.)
- `#fx{...}` effect rows are preserved in their explicit spelling.

### Comments

- `;;` and `;;;` comments at the top level are emitted on their own line.
- Inline `;` comments after an expression are preserved on the same line.
- One blank line is preserved between top-level forms; two blank lines before
  a `;;` section header.

### Type annotations

`:keyword` type annotations stay on the same line as the expression they
annotate and are never line-wrapped independently.

### Quote / quasiquote sugar

Sugar forms are preserved: `'x`, `` `x ``, `,x`, `,@x` stay as-is rather
than being expanded to their `(quote ...)` equivalents.

### Inline C blocks

`` ```c ... ``` `` blocks are emitted verbatim; the formatter does not
reformat embedded C.

### Dialects whose layout is their syntax

A sweet-exp buffer (`turmeric/sweet`, `saffron/sweet`, `r7rs/sweet`) is
**checked and kept as written**. Its indentation is part of its meaning, so
the formatter parses it -- a syntax error still fails the run -- and returns
the text with only the ends normalized: no leading blank lines and exactly one
trailing newline. A Scheme (`r7rs`) buffer is re-indented and never reprinted.
Every other reader goes through the pretty-printer described above.

## Example

Before formatting:

```turmeric
(defn add [x : int y : int] : int (+ x y))
(defn factorial [n : int] : int (if (<= n 1) 1 (* n (factorial (- n 1)))))
```

```sweet-exp
defn add [x :int y :int] :int
  {x + y}
defn factorial [n :int] :int
  if {n <= 1}
    1
    {n * factorial({n - 1})}
```

After `tur format` (a sweet-exp file is kept as written -- see
[Dialects whose layout is their syntax](#dialects-whose-layout-is-their-syntax)):

```turmeric
(defn add [x : int y : int] : int
  (+ x y))

(defn factorial [n : int] : int
  (if (<= n 1)
    1
    (* n (factorial (- n 1)))))
```

```sweet-exp
defn add [x :int y :int] :int
  {x + y}
defn factorial [n :int] :int
  if {n <= 1}
    1
    {n * factorial({n - 1})}
```

## See also

- `src/compiler/fmt.h` -- Public C API (`fmt_print`, `FmtOptions`)
- `src/compiler/fmt.c` -- Formatter implementation
- [lsp-guide.md](lsp-guide.md) -- `textDocument/formatting` runs the same
  document formatter as `tur fmt`
