# `emit-c` segfaults when two imported modules each define a private `__error`

**Status: RESOLVED 2026-10-16** (see [Resolution](#resolution)).

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

## A second symptom: a private call resolved to the other module

Found the same day writing xml-html-parsers-plan B4. turmeric-spices'
`xml/sax` and `html/sax` each define private `__attrs-acc`, `__event`,
`__deliver`, `__deliver-chunk`, `__run` and `__push` (and private inline-C
helpers `__free`, `__feed`, `__next`, ...). A program importing one public
function from each does not compile:

```sh
# turmeric-spices at 8764e43 (branch claude/dom-editable-tree)
cd spices/html
cat > _pn.tur <<'TUR'
(defmodule pn (export)
(import xml/sax  :refer [sax-parse-string])
(import html/sax :refer [html-sax-parse-string])
(defn main [] : int 0))
TUR
tur emit-c _pn.tur
# html/src/html/sax.tur:379:4: error: symbol '__push' is private to module 'xml/sax'
# ... and the same for __attrs-acc, __event, __deliver, __deliver-chunk, __run
```

Every call inside `html/sax` to one of those six helpers -- its own,
defined a few lines up -- is resolved to `xml/sax`'s and then rejected as
private. Swap the two imports and the errors move to `xml/sax`, naming
`html/sax`. The colliding inline-C helpers resolve correctly; the six that
fail are the Turmeric-bodied ones. Two toy reductions (private helpers over
a type both modules import from a third; private helpers that call a
handler closure, as `__deliver` does) compile and run correctly, so the
trigger is still unknown -- the same state as the crash above, and very
likely the same per-name table.

Worked around in the spices by keeping the two SAX modules out of one
program (nothing needs both). Any new spice module with a private helper
whose name a sibling module also uses should take a module prefix until
this is fixed.

## Workaround

Give module-private helpers a module-specific prefix (`dom/edit` now uses
`__ed-`).

## Fix directions

- Key every private-name table by (module, name), as exported names already
  must be.
- A regression fixture: two modules with a private `__error` of different
  arity, both imported by one program.

## Resolution

Not a per-name table in the emitter: name resolution in the elaborator.
Every module's top-level defns, private ones included, share the global
scope, and two places treated a same-named binding there as their own:

- `elab_forward_declare_defns` (`src/compiler/elab_module.c`) skipped any
  name already bound. A module whose private helper's name an earlier module
  had used got no forward declaration, so its recursive (or forward) calls
  resolved to the other module's helper and failed as private. In the SAX
  pair that started at `__attrs-acc`, which calls itself, and cascaded
  through every helper that calls it.
- `elab_defn` (`src/compiler/elab_fns.c`) took any global `TY_FN` binding of
  its name for its forward declaration, and its HRT5 early update rewrote
  that binding's arity and argument types in place. The other module's
  already-elaborated `(Err (__error rc))` then called a zero-argument
  function with one argument, and the emitter crashed.

`scope_lookup_visible` (`src/compiler/elab_core.c`) returns the newest
binding the module being elaborated can see, skipping one private to another
module (`binding_private_elsewhere`, the same visibility rule
`elab_lookup_sym` enforces), and falls back to the newest of any kind so a
genuinely private reference is still reported. The forward-declare passes
(module and entry), `elab_defn` and `elab_lookup_sym` use it; `elab_defn`
also ignores another module's private binding outright, so it is shadowed,
never rewritten. A lookup that lands on another module's private defn now
also finds an exported one it shadows (`shared` in the fixture).

**Pinned by** `private-defn-collision-recursive` (self- and mutually
recursive private helpers, different arities, an exported name another
module reuses privately) and `private-defn-collision-arity` (the crash
reduced: the old compiler segfaults). Both turmeric-spices repros above pass:
`xml/sax` and `html/sax` import together, and `dom/edit` with its helper
renamed back to `__error` emits beside `html/dom`.
