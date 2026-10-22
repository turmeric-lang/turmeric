# Plan: `(quote (a b c))` is data, not code

> **Status:** PROPOSED 2026-10-21 (from control-forms-gaps item 1). Direction:
> Lisp semantics -- `quote` never evaluates its argument. **Representation
> decided 2026-10-21: a `Datum` ADT (option 1 below).** Not started.

## Problem

```turmeric
(println (quote (+ 1 2)))    ; 3 on both engines (also '(+ 1 2))
```

In a typed program, `quote` of a symbol is right -- `(quote hello)` is the `Sym`
`hello` -- but `quote` of anything else is elaborated as code. The elaborator says
so (`src/compiler/elab_toplevel.c`, `case F_QUOTE`): "Non-symbol quoted forms
(literals, lists): fall back to the legacy 'elaborate as expression' behaviour.
Runtime list-of-values construction for `(quote (a b c))` is a follow-up."
`#lang r7rs` already quotes lists properly (its values are dynamically typed
pairs); this is about typed Turmeric and Saffron.

## Why it is not a one-liner

A quoted list has no type in typed Turmeric today. `(quote (+ 1 2))` holds a
symbol and two integers; `'(1 "a" foo)` mixes three kinds. The stdlib has no
datum type, and `List` is homogeneous.

## Representation (decide first)

1. **A `Datum` ADT** in a new `stdlib/datum.tur`, auto-loaded on first use:
   `(defdata Datum (DSym Sym) (DInt int) (DFloat float) (DStr cstr) (DBool bool)
   (DList (List Datum)))`, plus `show` / `Eq` instances and accessors.
   `(quote (+ 1 2))` is `(DList [(DSym +) (DInt 1) (DInt 2)])`. Typed, works on
   every engine, and is what a macro-free DSL would pattern-match.
2. **`(List any)`** -- reuses the dynamic `any` surface. Natural in Saffron
   (`#lang saffron`, where `any` is the default), noisy in typed code.
3. **Both:** `Datum` in typed files, `(List any)` in Saffron.

**Decided: 1.** Option 3's Saffron arm only if Saffron users ask.

Either way, keep `(quote sym)` a `Sym` at the top level (DSL helpers rely on it,
see `docs/archive/history/defgodot-script-macro-vec-quote-semantics.md`); a
symbol *inside* a quoted list is a `(DSym sym)`. And `(quote 5)` / `(quote "s")`:
a self-evaluating literal quoted is the literal itself, as in Lisp.

## Interaction with macros

Macros work on forms at compile time and use `quasiquote` / `unquote`
(`quasiquote_expand_form`); that path is untouched. A quoted list in a macro's
*expansion* still means a datum at run time, which is a change for any macro that
relied on `'(...)` being evaluated. A 2026-10-21 search of typed files (`stdlib/`, `examples/`, fixture inputs;
excluding `#lang r7rs`, comments and docstrings) found one family:
`reader-macros/define` takes its expansion template quoted --
`(reader-macros/define 'sum :datum-bracket '(+ $body))` in
`reader-macros-bare`, `-datum`, `-load-transitive`, `-procedural` and
`-string-body`. Those are read at compile time by the reader-macro directive and
never elaborated as a runtime `quote`; the change must keep them that way (check
where `reader-macros/define` consumes its arguments before touching `F_QUOTE`).

## Steps

1. Write `stdlib/datum.tur` (the `Datum` ADT, `show`, `Eq`, accessors) with
   docstrings.
2. `case F_QUOTE` builds the datum for a list / vector form (a vector quoted is a
   list datum too, or its own `DVec`; decide with 1).
3. The interpreter shares the elaborator, so it gets the same lowering.
4. Fixtures: `(quote (+ 1 2))` prints the datum on all three engines; nested
   lists; a quoted symbol stays a `Sym`; the `errors/` shape for a quoted form the
   datum cannot hold (if any). Update the `quote` entry in
   `stdlib/forms/control.tur`.
5. Gate behind `--enable=quote-data` while the representation settles (it
   changes the meaning of existing code), per the experimental-features rule, and
   graduate it once the stdlib and corpus agree.
