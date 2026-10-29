# A module-qualified constructor call mistypes as a function argument

**Severity: low.** `(lib/shapes/Square 4)` -- a `defdata` constructor named
with its module -- types as an anonymous `<adt>` when it is passed straight to
a function, so the call is refused with TUR-E0001. The unqualified
constructor, and a qualified *function* call, work. Found 2026-10-28 building
xml-html-followups-plan follow-up D1: a macro's expansion names everything
module-qualified so that it works in a file that imported only the macro, and
tur-html's `hiccup` calls wrapper functions (`html/hiccup/mk-elem`) instead of
its constructors because of this.

## Repro

```sh
mkdir -p /tmp/qc/lib && cd /tmp/qc
cat > lib/shapes.tur <<'X'
(defmodule lib/shapes
  (export Shape Circle Square)
  (defdata ^copy Shape (Circle int) (Square int)))
X
cat > main.tur <<'X'
(defmodule main
  (import lib/shapes :refer [Shape])
  (defn area [s : Shape] : int
    (match s
      (Circle r) (* 3 (* r r))
      (Square w) (* w w)))
  (defn main [] : int
    (println (area (lib/shapes/Square 4)))
    0))
X
tur run main.tur -I .
# main.tur:8:20: error [TUR-E0001]: function 'area' arg 1: expected Shape, got <adt>
```

The same with `^heap ^copy`. Binding the value first and passing the local
is refused the same way; a defn whose declared return type is `Shape`
returning `(lib/shapes/Square 4)` is accepted.

## Root cause

Not investigated. The qualified name resolves to the constructor (the program
compiles when the result flows only into a declared return type), but the
expression's type is an ADT the checker does not identify with `Shape`.

## Fix directions

Resolve a qualified constructor to the same `Type` the unqualified one gets
(the ADT registered by its `defdata`), wherever the elaborator types a call
head of the form `module/Ctor`.

## Resolution (2026-10-29)

The call head resolved to the constructor's binding, but every constructor
lookup in `elab_call_inner` (src/compiler/elab_call.c) -- the one that
patches the call's result with its ADT among them -- is by the name the call
spells, and no constructor is named `lib/shapes/Square`. A qualified head
(full or alias) that resolves to a constructor now continues under the
constructor's own name, so `(lib/shapes/Square 4)` and `(s/Square 4)` type as
`Shape` wherever the bare constructor does: a `defdata`'s, a `^heap` one's,
a `defstruct`'s. Pinned by `tests/fixtures/qualified-ctor-as-argument` (both
engines). tur-html's `hiccup` keeps its `mk-*` wrappers for another reason
(a macro expansion names helpers of the macro's own module, which a using
file need not import); the report's own case is fixed.
