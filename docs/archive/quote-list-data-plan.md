# Plan: `(quote (a b c))` is data, not code

> **Status:** GRADUATED 2026-10-23, in the 0.70 line (from control-forms-gaps
> item 1; behind `--enable=quote-data` from 2026-10-22, 0.69.0). `quote` never
> evaluates its argument: a quoted list or vector is a `Datum` (option 1
> below) on every engine, typed Turmeric and Saffron alike. See
> [Graduated](#graduated-2026-10-23).

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

## What landed (2026-10-22)

Behind `--enable=quote-data` (an `EXPERIMENTS[]` prototype row, introduced
0.69.0, advisory `expires_at` 0.73.0).

- **`stdlib/datum.tur`** -- `(defdata Datum :copy (DSym Sym) (DInt int) (DFloat
  float) (DStr cstr) (DBool bool) (DList (List Datum)) (DVec (List Datum)))`,
  `datum->str` (the source rendering), `datum=?` plus `Eq` / `Show` instances,
  the `datum-*?` predicates, `datum-items`, `datum-length`, `datum-nth`.
  - **`:copy`**, so one quoted value can be used twice. That needed `Sym`
    admitted as a `:copy` ADT field (`typekind_is_copy_for_struct`,
    elab_structs.c); `TYPE_SYM` was already `CK_COPY` everywhere else.
  - **A quoted vector is its own `DVec`** (step 2's open choice), so a DSL can
    tell `(f [x] ...)`'s binding vector from a call, and `'[1]` is not `'(1)`.
  - **`x : T` inside a quoted list** (the reader's `F_TYPE_ANN`) is the two
    elements written, `(DSym :)` then `T`, so `'(defn f [x : int] ...)`
    renders as written.
  - **A keyword** is a `DSym` named `":k"`, so it renders as written and is
    distinct from the symbol `k`.
  - A width suffix (`5i32`, `1.5f32`) is dropped -- `DInt` holds an `int`. A
    character literal, a map, a set, an inline-C block cannot be held: that is
    the `errors/` shape (`quote: a map-literal cannot be held in a Datum`).
- **Lowering** -- `elab_quote` (elab_toplevel.c), shared by the `F_QUOTE` case
  and the `quote`-headed call in elab_call.c, rewrites a quoted list to
  constructor forms (`(DList (list-of (DSym '+) (DInt 1) ...))`) and elaborates
  them, so the interpreter and both compiled back ends get one lowering. A
  quoted symbol is still a `Sym`; a quoted literal is the literal.
- **`println` of a `Datum`** prints its rendering. Typed, `(println d)` is
  `(println (datum->str d))`. In Saffron, where the argument is usually `any`,
  it is `(let [g d] (if (is? g Datum) (println (datum->str g)) (println g)))`,
  so a datum prints the same through an unannotated parameter, and every other
  value keeps its dynamic `println` (`elab_try_println_datum`, elab_call.c).
- **Loaded on first use.** datum.tur loads list-typed.tur, string.tur and the
  typeclass stack, and autoloading reserves every name in them (MF3) -- for
  every program, under an LSP that runs with every experiment on. So it joins
  the autoload only when the entry program quotes a list: a form scan in
  `tur_stdlib_prepend_forms`, a lexical scan of the entry file for
  `--interpret`. Project mode (`tur build .`) and the REPL load it whenever
  the flag is on. A quote only a `(load ...)`ed file or a macro expansion
  produces is not seen; `elab_quote` then says to `(load "stdlib/datum.tur")`.
  Never for `#lang r7rs`, which quotes through its own pairs.
- **Interpreter natives** for datum.tur's five inline-C helpers
  (interpreter_natives.c, native_caps.c), as `str-concat` / `int->str` have:
  an autoloaded module's inline-C cannot go through the REPL JIT, which
  re-autoloads the same module and collides with it.
- **Corpus check** -- the `reader-macros-*` fixtures this plan named give the
  same output with the flag on (their templates are consumed at read time).
  No stdlib code quotes a list.
- Fixtures: `quote-data-typed`, `quote-data-saffron`, `quote-data-off`,
  `errors/quote-data-unholdable`; all pass under `run.sh` and `run-turi.sh`,
  and `tur jit` prints the same.
- Found on the way, fixed: `tur run --enable=<x> prog` on a `#lang r7rs` file
  crashed, because global-flag parsing shortened `argv` but not `g_main_argc`,
  and the split-library cache key walked past the NULL (main.c).

## Graduated (2026-10-23)

The four graduation items, as done:

1. **Demand-load beyond the entry file.** The load pre-pass
   (`load_expand_forms`, elab_toplevel.c) splices `(load "stdlib/datum.tur")`
   ahead of the first form it walks that quotes a list -- in the entry file,
   a loaded file, an imported module's body, or a macro's template -- once
   per compile through the visited set, which the autoload seeds. Project
   mode and the REPL no longer load datum.tur up front; a project or a turn
   that quotes a list gets it from the same splice, and nothing else reserves
   its names (MF3). The entry-file scan stays for `tur --interpret`, whose
   preload runs before the entry is read. Only a quote a macro builds at
   expansion time from its argument is not seen; elab_quote's message says
   so and names the `load`. Fixture: `quote-data-demand-load`.
2. **Option 3's Saffron arm** stays unbuilt, as decided: the `Datum` works
   there through `any`.
3. **`defmacro*` bodies see a `Datum`**, as everywhere else (decided
   2026-10-23). `stdlib/datum-syntax.tur`, loaded into the macro environment,
   converts: `datum->syntax` (the datum rendered and read back, so a keyword,
   a vector and a `: T` annotation come back as written) and `syntax->datum`
   (the datum `(quote s)` would build; a map or character literal stops the
   expansion). `syntax-type-ann-inner` is the native it needed. A `defmacro*`
   that returns a `Datum` is told to wrap it in `datum->syntax` or use
   quasiquote, instead of "a non-syntax value". The REPL echoes a datum by its
   `Show` instance (`turi_try_show` now falls back from a constructor's name
   to its type's). Fixtures: `quote-data-macro`,
   `errors/quote-data-macro-returns-datum`.
4. **The gate is gone.** The `EXPERIMENTS[]` row moved to `GRADUATED[]`
   (a lingering `--enable=quote-data` is a TUR-W0063 no-op until 0.71.0),
   `g_opt_quote_data` is deleted, `quote-data-off` (the flag-off meaning) is
   retired and the other fixtures lost their `flags` files.

   **The corpus, again.** One more fixture than the 2026-10-21 search found
   relied on a quoted list meaning code: `errors/nil-tail-macro-expanded-definition`
   wrote its macro's template as `(quote (defclass ...))`. Its template is a
   quasiquote now. It also surfaced a type hole that predates this plan: a
   `:copy` ADT with a list field -- a `Datum` -- passes a committed `: int` /
   `: cstr` return check, so that macro's `Datum` was returned as an int with no
   error ([carrier-adt-returns-through-a-scalar-declaration](../reported/carrier-adt-returns-through-a-scalar-declaration.md)).
