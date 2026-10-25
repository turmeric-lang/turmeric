# Macro-time code types a forward function reference as `int`

**Status: RESOLVED 2026-10-17** (see [Resolution](#resolution)).

**Severity: low-medium (expressiveness).** In code elaborated for macro
time -- a `defmacro*` body's `letrec`, or a module imported `:for-macros`
-- a call to a function defined *later* is typed with `int` parameters.
Mutually recursive helpers over `Syntax` therefore fail at definition time.
The same top-level forward reference works compiled and under
`tur interpret`. Found 2026-10-16 prototyping hiccup vector rewriting
([xml-html-followups-plan](../upcoming/spices/xml-html-followups-plan.md), D-1); reproduced on
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

## Resolution

Not macro time as such: the forward-declare pre-pass. Every pre-pass that
declares a function before its body is elaborated (a module's, the entry
file's, and `letrec`'s) types parameters through `fwd_decl_scan_params`
(`src/compiler/elab_core.c`), which commits a parameter's kind only for a
short list of bare type names and leaves the rest on the `int` placeholder.
`Syntax` was not on the list. The same shape compiled outside macro time
only because `Syntax` hardly appears there; a `cstr` walker was always fine.
Under `tur --interpret` it failed the same way, since a `:for-macros`
import is elaborated by the same pre-pass whatever the engine.

`TY_SYNTAX` joins the committed kinds (a bare `Syntax` determines it
completely), and the module and entry pre-passes resolve a `: Syntax`
result too -- `letrec`'s return peek already did.

**Pinned by** `macro-time-forward-reference-syntax`: a `:for-macros` module
with a mutually recursive walker returning `int` and one returning
`Syntax`, each callee defined after its caller, and the `letrec` form inside
a `defmacro*`. Both engines pass it; the old compiler reports "expected int,
got Syntax" twice. `docs/guides/macros-guide.md` says definition order no
longer matters.
