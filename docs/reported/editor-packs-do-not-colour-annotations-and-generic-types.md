# The editor packs and Trowel do not colour `^annotations` and generic types the way the docs and Try Turmeric now do

**Severity:** low (cosmetic and consistency; the same program reads differently
in each editor). Nothing here changes how a program compiles.

**Status:** open. Filed 2026-10-26, right after the docs highlighter and Try
Turmeric's Monaco editor were brought into line. The four packs live in their
own repos, so the work is theirs; this report is the index of what to change.

## The target

Set by `web/editor-palette.json` (the one written-down copy of the palette) and
implemented in `tools/genguides.py` (the docs) and `web/editor-language.js` (Monaco):

- **A `^name` is one token, caret included, coloured by what it means.** The
  reader takes a `^` as the start of a metadata symbol (`src/compiler/reader.c`),
  so the name is any symbol; the elaborator then reads it one of four ways, and
  Try Turmeric's editor follows it (`CARET_MODIFIERS` in `web/editor-language.js`):
  - a **modifier** from the closed list the elaborator interns (`^mut`, `^fat`,
    `^linear`, `^borrow`, ... -- the `sym_caret_*` table in
    `src/compiler/elab_core.c`, plus `^tailcall`): palette `annotation`, teal
    (`#7AC4B8` dark, `#127065` light);
  - a **kind** (`^f`, `^^f`, `^&cols`, `^w`) or a typeclass **constraint**
    (`^Monoid`, `^Eq`): palette `kind`, violet (`#C4A0E8` / `#7748AE`). Any
    `^name` that is not a modifier is a kind, as in the compiler;
  - an **extern-c C type** (`^int`, `^ptr<void>`): palette `type`, violet.
  - Since 2026-10-27 the modifier list also holds the flags of a type
    declaration, which are written before the name:
    `(defstruct ^copy ^heap Cell ...)`, `(defopaque ^linear ^sealed Tok ...)`.
    The new names are `^copy`, `^move`, `^heap`, `^sealed`, `^non-null` and
    `^no-auto-ctor` (`^linear` and `^affine` were already there). None of the
    four packs keeps a closed list today (each colours every `^name` one way,
    see the table below), so these names need nothing of them beyond the
    fixes below; once a pack splits modifiers from kinds, the new names belong
    on the modifier side. See
    [caret-type-attributes-plan](../upcoming/caret-type-attributes-plan.md).
- **A type is one token, generic arguments included** (`type`, the same violet
  as `kind`): `:int`, `:ptr<void>`, `:option<ptr<int>>`, a bare `ptr<void>` as in
  `(fn [ptr<void>] nil)`, and the spaced `name : cstr` / `] : void`. Not `ptr`
  coloured with `<void>` trailing in the default colour.
- **Builtins** (`println`, `map`, `apply`, ...: `keywordOperator`) are periwinkle
  (`#8AB0E8` / `#3B63B0`), so they no longer share a colour with types and kinds.
- Since 2026-10-27 this is the palette in `web/editor-palette.json`, Try
  Turmeric's editor and the docs pages (`tools/genguides.py`, `hl-ann` /
  `hl-kind` / `hl-type` / `hl-builtin`). The four packs have not moved.
- `<`, `<=`, `char<?` and `string->cstr` are not generics and stay as they were.

## What each pack does today

Read from a shallow clone of each repo on 2026-10-26.

| Pack | `^name` colour | `^name` matches | `:type` | bare `ptr<void>` |
| --- | --- | --- | --- | --- |
| Try Turmeric (Monaco) | modifier teal, kind / constraint violet | reader's symbol set | one token, generics included, spaced form too | one token |
| docs (`genguides.py`) | modifier teal, kind / constraint violet | reader's symbol set | one token, spaced form too | one token |
| **vim-turmeric** | **teal** (`turmericAttr` links `StorageClass`, which `colors/*.vim` sets to the `Type` teal, so an annotation reads as a type) | `\^[a-z][a-z0-9-]*` | `:` + name incl. `<>`, but only after whitespace or `[` | not matched |
| **vscode-turmeric** | **gold, bold** (`storage.modifier.turmeric` is the keyword colour in both themes) | `\^[a-z][a-z0-9-]*` | `type-annotation` rule includes `<>`; same whitespace/`[` lookbehind | not matched |
| **turmeric-mode** (Emacs) | violet (`font-lock-preprocessor-face`) -- already right | `\^[a-zA-Z][a-zA-Z0-9_-]*` | **keyword literal**: `:name` (generics and all) is `font-lock-constant-face`, sage; only `defdata`/`defstruct` names and a type-keyword list get `font-lock-type-face` | not matched |
| **trowel** | **gold** (`metadata` style is `#EFA030` in `turmeric-dark.theme.json`) | `^` + alpha, then alnum / `_` / `-` (`scanner_turmeric.cpp` ~549) | **`:name` is a keyword literal** (`keywordLit`, sage); only names in `TypeKeywords()` are `Type` | not matched |

## Fix directions, per repo

**vim-turmeric** (`syntax/turmeric.vim`, `colors/`)
- Widen `turmericAttr` to the reader's set (`\^[^ \t()\[\]{}";,']\+`), so
  `^state/GameState`, `^&cols` and `^Monoid` match.
- Link it to a violet group, not `StorageClass`: `colors/turmeric-*.vim` already
  paint `Special` / `PreProc` violet (`#C4A0E8`), or give it its own group.
- Add a bare-generic rule (`name<...>` -> `Type`) next to `turmericType`.

**vscode-turmeric** (`syntaxes/turmeric.tmLanguage.json`, `themes/`)
- Widen `param-attribute` the same way.
- Give annotations their own scope (for example
  `storage.modifier.annotation.turmeric`) and a violet rule in both themes, so
  they stop sharing the bold-gold `storage.modifier`.
- Add a bare-generic rule scoped `entity.name.type.turmeric`.
- `tests/run.sh` pins each regex with must-match / must-not-match cases; add the
  new shapes there (and `<` / `<=` / `char<?` as must-not-match).

**turmeric-mode** (`turmeric-mode.el`, `turmeric-*-theme.el`)
- Widen the `^` regex at line ~145 to the reader's set.
- Decide how `:name` splits between keyword literal and type (below). The rule at
  line ~148 takes every `:name`, `<>` included, as `font-lock-constant-face`; a
  type-annotation rule has to come before it to win.
- Add a `font-lock-type-face` rule for bare `name<...>`.

**trowel** (`src/editor/scanner_turmeric.cpp`, `resources/*.theme.json`)
- Widen the `^` scan to the reader's symbol-continuation characters (it stops at
  `/`, `&` and `^` today).
- Change the `metadata` colour from the keyword gold to the violet, in the dark
  and light themes.
- Decide how `:name` is split between a keyword literal and a type (below), and
  add generic-argument handling to whichever style the type takes.

## An open design question

`x :int` (a type annotation) and `:refer` (a keyword literal) are the same
characters. Try Turmeric and the docs colour every `:name` teal, so a map key
such as `#map{:url "..."}` is teal too. Vim and VS Code narrow it with a
lookbehind (a `:` after whitespace or `[`), which also cannot tell `[:a 1]` from
`[x :int]`. Trowel colours every `:name` sage, so there a type is a literal.
There is no way to tell them apart lexically; the packs should agree on which
way to be wrong, and the palette doc should say so. The least surprising choice
is probably the docs' and Monaco's (every `:name` is teal), since a keyword
literal is rare beside the annotations in typed code -- but that is a call for
the owners of the packs.

## The palette contract does not force this

Each editor repo's CI runs this repo's `tests/check-editor-palette.py` against
`web/editor-palette.json` on `main`. That file now has an `annotation` key, but
its value is the `keywordOperator` hex, which every pack's theme already
contains, so all four stay green. The check proves a colour is *present*, not
that `^annotations` use it. Teaching it to look for an annotation rule would need
each repo's theme layout understood, which is why it is not done here.

## How to verify a fix

In each editor, open this snippet and confirm the whole of each marked token is
one colour:

```turmeric
(defn bump [^borrow c : ptr<Counter> by : int] : int c)
(defn apply-to [^fat f : (fn [ptr<void>] nil) ^linear h : option<ptr<void>>] : nil nil)
(defn step [^state/GameState s ^&cols c ^Monoid m] : int s)
(def ^mut total 0)
(< 0 1) (<= 0 1) (char<? a b) (string->cstr s)   ; the last four are not generics
```

`^borrow`, `^fat`, `^linear` and `^mut` should be the modifier colour (teal);
`^state/GameState`, `^&cols`, `^Monoid`, `:ptr<Counter>`, `ptr<void>` and
`:option<ptr<void>>` the type/kind colour (violet).
