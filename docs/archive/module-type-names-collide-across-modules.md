# Two modules cannot each define a type with the same name

**Severity: medium (a library's private type name is reserved program-wide).**
Found 2026-10-28 building xml-html-followups-plan follow-ups D1b-D3.
**Resolved 2026-10-29** -- see "Resolution" at the end.

A type a module defines is registered under its bare name in one global
namespace, so two modules that each define `Attr` cannot both be loaded,
even when neither exports it to the file that loads them, and even when the
two are different kinds of type. Functions and macros do not have this
problem: they are module-qualified.

## Repro

```sh
mkdir -p /tmp/tc/lib && cd /tmp/tc
cat > lib/a.tur <<'T'
(defmodule lib/a
  (export Attr mk-a)
  (defstruct ^copy Attr [n : int])
  (defn mk-a [] : int (.n (Attr 1))))
T
cat > lib/b.tur <<'T'
(defmodule lib/b
  (export Attr mk-b)
  (defopaque Attr :ptr<void>)
  (defn mk-b [] : int 2))
T
cat > main.tur <<'T'
(defmodule main
  (import lib/a :refer [mk-a])
  (import lib/b :refer [mk-b])
  (defn main [] : int (println (+ (mk-a) (mk-b))) 0))
T
tur run -I . main.tur
```

```
./lib/b.tur:3:14: error: defopaque: 'Attr' is already defined
```

A second shape is silent until the export check: a module whose type shares
its name with a type a top-level `load` defined. tur-dom-core's `dom/ownstr`
defines its own `String` (an ABI mirror of stdlib's); a program that also
does `(load "stdlib/list-typed.tur")`, which brings in stdlib `String`, gets

```
dom/ownstr.tur:16:1: error: exported symbol 'String' is not defined in this module
```

with no diagnostic at the `defopaque` itself.

## Where it bit

- tur-html's hiccup `Attr` collided with tur-dom-core's SAX `Attr`
  (`dom/event`) the first time a test loaded both; hiccup's types are now
  `HTag`, `HAttr`, `HAttrName`, ... `ecs` already has a `Tag`.
- No program that uses tur-xml, tur-html or tur-dom-core can load
  `stdlib/list-typed.tur` (the `String` shape), so `(List A)` and
  `list-map` are unavailable next to them. tur-html's hiccup rewrites
  `list-map` over an inline `fn` (D1b), and its test has to define
  `list-foldl` itself to exercise that.

## Root cause

`elab_defopaque` (src/compiler/elab_structs.c, ~line 1121) and the
`defstruct` / `defdata` paths look the name up in the current scope and
refuse it when a binding exists; types are bound by bare name in
`e->global`, not under the module's name as `defn`s are. The export check
(src/compiler/elab_module.c ~2072) then compares
`defining_module_name`, which is why the `load` shape fails there.

## Fix directions

- Qualify type names by their module, as functions are: `lib/a/Attr` and
  `lib/b/Attr` are different types, `:refer [Attr]` brings one in under the
  bare name, and a clash is an error only where both are referred.
- At least: report the clash at the second definition in the `load` shape
  too, naming the first definer, instead of failing the export check.

## Resolution (2026-10-29)

The first fix direction, done by renaming rather than by threading a module
through every type lookup. `elab_defmodule` (src/compiler/elab_module.c),
once the module's imports are loaded and before anything in its body is
declared, collects the type and constructor names the body defines
(`defstruct`, `defdata`, `defgadt`, `defopaque`, `deftype`, `defalias`, and
the constructors of the first two). A name some other module, the stdlib, or
a file outside any module already holds is renamed `<module>/<name>`
throughout the body (`elab_module_type_renames`, `type_renames_apply`), in
symbols, keywords and compound spellings such as `ptr<Attr>`. The export list
follows, and the module records its renames (`DefModule.rename_from` /
`rename_to`, copied to `ElabModule`).

An importer reaches a renamed type by `:refer [Attr]` (elab_apply_import
looks the referred name up under its new one), by `alias/Attr`, by the full
`lib/b/Attr`, or by a bare `Attr` when exactly one of its imports provides
the name and nothing outside any module holds it. Every lookup, diagnostic
and C name downstream sees a distinct name, so none of them changed; and a
name nobody else holds is not renamed, so a program that compiled before
compiles to the same C.

The `load` shape is the same case: a stdlib `load` inside a module body is
elaborated ahead of the module's imports, so the module's own `String` is
the one renamed, and a bare `String` in the loading module means the
stdlib's.

Writing the fixture turned up the function half of the problem: two modules
exporting a function of one name stacked two visible bindings, and the newest
answered, so `(:refer [f])` from `lib/a` called `lib/b`'s `f` whenever
`lib/b` was imported after it. `scope_lookup_visible` (src/compiler/elab_core.c)
now prefers, among several visible global bindings, the one the module
defines or refers, and referring one name from two modules is an error at
the second import. The interpreter had the same collision at run time: it
looked globals up by bare name, and only a module's PRIVATE defns had a
`<module>/<name>` key, so `lib/a`'s own call to its exported `f` ran
`lib/b`'s. An exported module defn is now registered under that key too, and
a reference the elaborator resolved to a module's global is looked up under
its module's key first (`eval_lookup_b`, src/turi/eval.c).

Pinned by `tests/fixtures/module-type-name-collision` (structs, constructors,
a stdlib `Pair`, an ABI-mirror `String` beside a stdlib `load`, a sole
provider), `errors/module-type-name-collision-distinct`,
`refer-wins-over-later-export` and `errors/refer-same-name-from-two-modules`.
The module-system guide's "Two modules with the same name" describes the
rules.
