# The API docs miss most language primitives, and render several forms lossily

**Severity: medium (documentation).** Nothing fails to build, but a reader of
turmeric-lang.com/docs/html/api, `(doc ...)` or `tur doc` cannot find a large
part of the language:

1. The ~137 special forms and ~40 compiler builtins have no API entry at all.
2. Four definition forms the stdlib documents with `;;;` (`deftype`,
   `defdynamic`, `def`, `defalias`) never render.
3. Five forms that do render lose part of their declaration.
4. A name containing `=` is published under the part before the `=`.

Found auditing doc coverage after the typeclass docs work (2026-10-19), which
fixed the same class of bug for `defclass` (`tools/gendocs.py` ignored it, so
tur/comonad never mentioned `extract`). Line numbers below are as of that
change, on branch `claude/typeclass-api-docs-1mmix5`.

## 1. Compiler primitives have no API reference

> **Narrowed 2026-10-19:** the decision is the doc-only stdlib file. Pages
> under `stdlib/forms/` hold `;;;` blocks over `;;@builtin` / `;;@form`
> declaration lines -- `;;` comments the compiler skips and `tools/gendocs.py`
> reads (autodoc-guide, "Special forms and builtins"), so the site, `(doc ...)`
> and `tur doc` read one source, doctests included; a stdlib entry now wins
> over the one-line C table (`tur_doc_prefer`). **Every public builtin in
> `table_` is documented** (`tur/forms/operators`, `unsafe`, `references`,
> `core`; `bit-shr` in `tur/bits`). The drift check exists:
> `tests/check-forms-documented.sh` (ctest `tur_forms_documented_lint`).
> **Left: 100 special forms**, listed by group in
> `tests/forms-undocumented.txt` -- the definition forms, binding and control,
> casts and existentials, effects and handlers, continuations, generators and
> STM, panics, and the GC counters. Writing the builtin pages turned up five
> implementation bugs, filed separately (`integer-mod-by-zero-...`,
> `sized-int-arithmetic-...`, `unsafe-builtins-...`, `ref-weak-builtins-...`,
> `show-line-of-option-...`).

The API pages and `stdlib/docstrings.tur` are generated from stdlib `;;;`
docstrings only, so anything the compiler implements itself has no source for
`tools/gendocs.py` to read.

- **Special forms.** `reserved_special_forms_` (`src/compiler/elab_call.c:358`)
  lists 137: `let*`, `letrec`, `case`, `match`, `handle`, `handle-shallow`,
  `resume`, `discontinue`, `shift`/`reset` and the cloneable/serial variants,
  `call/cc*`, `gen`/`yield`, `ref`/`deref`, `::`, `cast`, `is?`, `pack`/`open`,
  `stm`/`retry`, `panic-payload-*`, `gc!` and the `gc-*` counters, ...  None has
  an API card or a `stdlib/docstrings.tur` entry.
- **Builtin functions.** `table_` in `src/compiler/builtins.c:11`. About 40 of
  its names have no doc entry anywhere: `+.` `-.` `*.` `/.` `not=`, `bit-and`
  `bit-or` `bit-shl` `bit-xor`, `cons`, `drop!`, every `ptr-*` and `raw-*`,
  `array-get-unchecked` / `array-set-unchecked`, `transmute`, `reinterpret`,
  `unsafe-cast`, `rc/of` `rc/clone` `rc/drop` `rc/strong-count`, `weak`
  `weak?` `upgrade`, `dlopen` `dlsym` `dlclose`.
- **What exists today.** `turi_doc_lookup_builtin`
  (`src/turi/docstrings.c:195`) holds 57 one-line descriptions -- 32 of the
  reserved forms. `tur doc`, `(doc ...)`, the REPL's `:doc` and the web doc
  panel fall back to it; the site does not render it, and the other 105
  reserved forms are in neither table. The syntax guide's "Reserved names" table
  (`docs/guides/syntax-guide.md:988`) lists the forms by name only. Most are
  explained somewhere in a topical guide; `handle-shallow`, `discontinue`,
  `call/cc*`, `retry`, `c-call`, the `panic-payload-*` accessors and the
  unsafe `ptr-*` / `raw-*` / `transmute` group get a passing mention at most.

### Repro

```sh
tur doc handle          # (handle expr (effect/op args k) body ...) -- handle effects
tur doc handle-shallow  # no documentation for 'handle-shallow'
tur doc shift           # no documentation for 'shift'
tur doc transmute       # no documentation for 'transmute'
```

### Fix directions

This one needs a decision on where the text lives, so `(doc 'handle)`, `tur
doc` and the site read one source:

- **A doc-only stdlib file** (e.g. `stdlib/forms.tur`) of `;;;` blocks, each
  above a declaration-shaped stub gendocs recognizes but the compiler skips.
  Reuses the whole existing pipeline, doctests included; the risk is a stub
  the compiler does not skip.
- **Grow the C table** into signature + summary + example rows and have
  gendocs render it as a "Special forms" page (and fold it into
  `doc-names.json`). Keeps the text next to the compiler; needs a second
  renderer.

Either way, add a check that every name in `reserved_special_forms_` and every
non-internal `table_` name has an entry, so the list cannot drift again.

## 2. Definition forms gendocs does not recognize

`def_re` (`tools/gendocs.py:524`) matches `defn defmacro defstruct defdata
defgadt defclass definstance defopaque defeffect`. Anything else is skipped,
docstring and all:

| Form | Documented but invisible |
| --- | --- |
| `deftype` | the refinement types `Nat` `Pos` `NonZero` `Neg` `Byte` `Percent` (`stdlib/refine.tur`) -- the module page has zero cards |
| `defdynamic` | `*log-level*` `*locale*` `*random-seed*` `*current-module*` (`stdlib/dynvar.tur`) |
| `def` | `PI` (`stdlib/math.tur`), the reactor's `READ` `WRITE` `ERROR` `HUP`, the `SCHEMA_*` tags |
| `defalias` | `ReplicaId` (spice `crdt`) |

`extern-c` (179 stdlib declarations) is skipped too; nearly all are internal C
shims (`tur_reactor_new`, ...), so that is probably right, but a public one
such as `getenv` (`stdlib/effects.tur`) is invisible as a result.

### Repro

```sh
python3 tools/gendocs.py stdlib/refine.tur --out /tmp/api
grep -c 'class="def-card"' /tmp/api/tur-refine.html       # 0
python3 tools/gendocs.py stdlib/dynvar.tur --out /tmp/api
grep -c '\*log-level\*' /tmp/api/tur-dynvar.html           # 0
```

### Fix directions

Add the four kinds to `def_re` with a renderer each: `(deftype Nat
#refine{ x : int | (>= x 0) })` verbatim, `(defdynamic *log-level* :int 1)`
with its type and default, `(def PI 3.141592653589793)` with its value, and
`(defalias ReplicaId Sym)`. Treat a `__`-suffixed or `__`-prefixed name as
internal, as for `defn` -- the r7rs prelude has many `def ..__` globals.

## 3. Forms that render lossily

All in `_parse_def_line` (`tools/gendocs.py:867`) and `_render_signature`
(`tools/gendocs.py:1951`):

| Source | Rendered | Cause |
| --- | --- | --- |
| `(defstruct Pair [A B] (fst A) (snd B))` | `(defstruct Pair [A B])` | a type-parameter vector returns before the `(field T)` list is read (`:937`); every generic struct (`Cons`, `Map`, `Lens`, ...) shows no fields |
| `(defstruct Cons :heap [A] ...)` | `(defstruct Cons [A])` | the annotation regex knows `copy`/`move`/`linear`, not `heap` |
| `(defdata Either :copy [L R] (Left L) (Right R))` | `(... (Left) (Right))` | `ctor_names` (`:903`) keeps each arm's head and drops its payload types |
| `(defgadt Nat [] (Zero : (Nat)) (Succ (Nat) : (Nat)))` | `(defgadt Nat (Zero) (Nat) (Succ) (Nat) (Nat))` | the same regex reads each arm's return-type annotation as another constructor |
| `(defeffect FS [] :nil ^extends IO ^capability)` | `(FS :nil)` | `defeffect` falls through to the `defn` renderer: no keyword, `^extends` / `^capability` dropped |
| `(defeffect Fail [msg :cstr] :nil)` | `(Fail [msg :cstr] :nil))` | the form's closing paren is read as part of the return type (`_return_type_after_bracket`, `:145`) |
| `(defopaque ArgSpec :int)` | `(ArgSpec)` | same fall-through: no keyword, no representation type |

### Repro

```sh
python3 -c "
import sys; sys.path.insert(0, 'tools')
from gendocs import parse_tur_file, _render_signature
for f, n in [('stdlib/pair.tur', 'Pair'), ('stdlib/either.tur', 'Either'),
             ('stdlib/nat.tur', 'Nat'), ('stdlib/effects.tur', 'Fail'),
             ('stdlib/args.tur', 'ArgSpec')]:
    print([_render_signature(d) for d in parse_tur_file(f)['definitions']
           if d['name'] == n][0])"
```

### Fix directions

Parse `defdata` / `defgadt` arms as balanced forms (the `_defclass_members`
splitter added for `defclass` does this) and keep each arm whole; for a GADT
arm, split on its top-level `:` into fields and result. Read a generic
struct's `(field T)` list after its type-parameter vector. Give `defeffect`
and `defopaque` their own `_render_signature` branches, strip the form's
closing paren before reading a return type, and add `heap` to the annotation
set.

## 4. A name containing `=` is truncated

The name pattern in `_parse_def_line` (`tools/gendocs.py:867`) is
`[\w/\-!?<>*+]+`, which has no `=`. So `char=?` is published as `char`,
`sym=?` as `sym`, and 31 r7rs comparison wrappers (`r7rs-string<=?`, ...) the
same way. In the spices: `frame=?`, `entity=?`, `buf=?`, `payload=?`,
`key=?`, `string=?`.

### Repro

```sh
tur doc 'char=?'      # no documentation for 'char=?'
tur doc char          # char=? -- are two characters the same?
```

### Fix directions

Take the name as everything up to whitespace or a bracket, the way
`_parse_defclass` reads a class name (`[^\s\[\]()]+`), and add `char=?`,
`<=f` and `r7rs-string<=?` to `tests/tools/check_gendocs_parse.py`.

## Notes

Groups 2-4 are generator fixes in the size of the defclass change and can land
as one PR with parse tests. Group 1 waits on the decision above. Regenerate
`stdlib/docstrings.tur` with any of them -- new and corrected names change its
keys.
