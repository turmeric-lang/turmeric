# `emit-c` segfaults when two imported modules each define a private `__error`

**Severity: medium.** The compiler crashes (SIGSEGV in the C emitter, no
diagnostic) on a program that imports from two modules which each define a
private helper of the same name with different signatures. Private names are
not exported, so nothing in either module is wrong; renaming one helper makes
the crash go away. Found 2026-10-16 writing turmeric-spices' `dom/edit`
(xml-html-parsers-plan follow-up B2); reproduced with a Release `tur` built
from `main` at `d60c40f6`.

## Repro

The trigger needs more context than two toy modules: a minimal two-module
repro with `(defn __h [] : Pt ...)` and `(defn __h [x : int] : Kind ...)`
compiles fine, as does one where each is wrapped in `Err`. It reproduces
reliably from the spice tree:

```sh
# turmeric-spices at f54e51b (branch claude/dom-editable-tree)
cd spices
sed -i 's/__ed-error/__error/g' dom-core/src/dom/edit.tur
cat > html/fixtures/_b.tur <<'TUR'
(defmodule bis (export)
(import dom/edit :refer [dom-new])
(import html/dom :refer [html-parse-fragment])
(defn main [] : int 0)
)
TUR
cd html && tur emit-c fixtures/_b.tur      # Segmentation fault
```

The two helpers:

- `html/dom` (`spices/html/src/html/dom.tur:133`):
  `(defn __error [] : ParseError (ParseError ...))`, called as
  `(Err (__error))` in `__wrap`.
- `dom/edit`: `(defn __error [rc : int] : EditError (cond ...))` over a
  nine-variant `:copy` sum, called as `(Err (__error rc))` in `__done`.

Each import alone compiles; `dom/edit` with `xml/dom` (whose private
`__error` takes one argument) compiles. Leaving only `__new-raw` colliding
(both one-argument) does not crash either; leaving `__error` does.

## Where

```
#0 emit_value_dispatch ()
#1 emit_value.part ()
#2 emit_value_dispatch ()
#3 emit_value.part ()
#4 emit_tail ()
#5 emit_tail ()
#6 emit_fn_def_body ()
#7 emit_fn_def ()
#8 emit_program_inner ()
```

Not traced further. The likely shape: some per-name table (a signature or
specialization cache) is keyed by the bare private name rather than by module,
so emitting one module's `__error` call finds the other's signature -- here a
zero-parameter function called with one argument, or the reverse.

## Workaround

Give module-private helpers a module-specific prefix (`dom/edit` now uses
`__ed-`).

## Fix directions

- Key every private-name table by (module, name), as exported names already
  must be.
- A regression fixture: two modules with a private `__error` of different
  arity, both imported by one program.
