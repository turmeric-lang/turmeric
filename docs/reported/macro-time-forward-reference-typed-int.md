# Macro-time code types a forward function reference as `int`

**Severity: low-medium (expressiveness).** In code elaborated for macro
time -- a `defmacro*` body's `letrec`, or a module imported `:for-macros`
-- a call to a function defined *later* is typed with `int` parameters.
Mutually recursive helpers over `Syntax` therefore fail at definition time.
The same top-level forward reference works compiled and under
`tur interpret`. Found 2026-10-16 prototyping hiccup vector rewriting
(`docs/upcoming/spices/xml-html-parsers-plan.md`, D-1); reproduced on
`main` at `d60c40f6`.

## Repro

```turmeric
;; mxwalk.tur
(defmodule mxwalk
  (export count-syms)
  (defn count-syms [f : Syntax] : int
    (if (syntax-sym? f) 1 (count-list f 0)))
  (defn count-list [f : Syntax i : int] : int
    (if (syntax-list? f)
      (if (< i (syntax-len f)) (+ (count-syms (syntax-nth f i)) (count-list f (+ i 1))) 0)
      0)))

;; main.tur
(defmodule main
  (import mxwalk :for-macros)
  (defmacro* nsyms [f] (int->syntax (count-syms f)))
  (defn main [] : int (println (nsyms (a (b c) 1))) 0))
```

```
./mxwalk.tur:4:39: error [TUR-E0001]: function 'count-list' arg 1: expected int, got Syntax
main.tur:2:3: error: :for-macros import of 'mxwalk' failed at macro time: elaboration error
```

The same two helpers in a `letrec` inside the `defmacro*` fail the same way
(`function 'cl' arg 1: expected int, got Syntax`). `docs/guides/macros-guide.md`
already notes the related `(def helper (fn ...))` self-call typing `:int`.

## Root cause

Not traced. The likely shape: macro-time elaboration checks each definition
before the next one's signature is known, where the compiled path declares
every top-level signature first.

## Workaround

One self-recursive function that dispatches on a mode argument, with the
non-recursive helpers defined above it. A function's call to itself is
typed correctly.

## Fix directions

- Declare every top-level `defn` signature in a `:for-macros` module before
  elaborating any body, as the compiled path does.
- In `letrec`, bind each name to its declared fn type before elaborating
  any init.
