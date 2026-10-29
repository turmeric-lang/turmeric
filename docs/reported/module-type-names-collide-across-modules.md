# Two modules cannot each define a type with the same name

**Severity: medium (a library's private type name is reserved program-wide).**
Found 2026-10-28 building xml-html-followups-plan follow-ups D1b-D3.

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
