# A `:heap` value of another module's type passed to a closure has no cast to the word parameter

**Severity: medium** (clang rejects the emitted C with `-Wint-conversion`;
gcc 13 warns and the program runs, so Linux CI passes and macOS fails).

**Status: RESOLVED 2026-10-11** (see [Resolution](#resolution)). Found
2026-10-10 implementing
[xml-html-parsers-plan](xml-html-parsers-plan.md) on `tur`
v0.64.0. Worked around in turmeric-spices `spices/dom-core/src/dom/event.tur`:
the SAX drivers call handlers through `sax-dispatch`, defined in the module
that defines `SaxEvent`.

## Repro (three modules)

```turmeric
;; src/geo/ev.tur
(defmodule geo/ev (export Ev A B mk)
(defdata Ev :heap :copy (A int) (B int int))
(defn mk [x : int] : Ev (B x x)))

;; src/geo/drv.tur
(defmodule geo/drv (export call)
(import geo/ev :refer [Ev A B mk])
(defn call [h : (fn [Ev] int) x : int] : int
  (let [e (mk x)
        r (h e)]
    r)))

;; main.tur
(defmodule main (export)
(import geo/ev :refer [Ev A B])
(import geo/drv :refer [call])
(defn main [] : int
  (println (call (fn [e : Ev] : int (match e (A x) x (B x y) (+ x y))) 4))
  0))
```

`CC=clang tur run main.tur`:

```
error: incompatible pointer to integer conversion passing 'tur_adt_Ev *'
       to parameter of type 'int64_t' [-Wint-conversion]
```

The same program with `Ev` defined in `geo/drv` itself compiles, and so does
calling the closure from a helper defined in `geo/ev`.

## Fix direction

The closure call site casts every other argument to the thunk's erased word
type; a `:heap` argument whose type was imported is the one that misses it.

## Resolution

Not cross-module: the three modules folded into one file fail the same way on
`main` (`(defn call [h : (fn [Ev] int) x : int] ...)` and `(h e)` on a
`let`-bound `:heap` `e`). The call goes through the decl-typed fat-dispatch arm
in `emit_value_dispatch` (`src/compiler/emit_expr.c`), which already carried a
`ptr_into_carrier` bridge for a pointer value into an `int64_t` slot. It asked
`emit_type_c_name` of the argument's Type how the value was spelled -- the
word, for a `:heap` ADT -- while the `let` had declared the local as the typed
`tur_adt_Ev *`. The bridge now asks the localvar ctype table first for a bare
identifier (what the local was declared as), falling back to the Type.

**Pinned by** `heap-adt-arg-to-closure-call` (a plain and a `^fat` function
parameter called with a `let`-bound `:heap` value; the old compiler warns
`-Wint-conversion` twice, which the run.sh ratchet fails).
