# Plan: `(quote (a b c))` is data, not code

> **Status:** BETA BEHIND `--enable=quote-data` since 2026-10-24 (implemented
> 2026-10-22, from control-forms-gaps item 1); graduates in 0.71.0 after one
> release cycle of soak. Direction: Lisp semantics -- `quote` never evaluates
> its argument. **Representation decided 2026-10-21: a `Datum` ADT (option 1
> below).** Steps 1-5 landed for typed Turmeric and Saffron on all three
> engines, and every graduation item but the gate drop is closed -- see the
> "Graduation" checklist at the end.

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
  the flag is on. (As landed, a quote only a `(load ...)`ed file or a macro
  expansion produced was not seen; graduation item 1 below closed that.)
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

## Graduation

The row moved to `XF_LIFECYCLE_BETA` on 2026-10-24 with `expires_at` 0.71.0
(the current VERSION was 0.70.5; a beta soaks one release cycle). The surface
below is frozen; only item 4 is left.

- [x] **1. Demand-load beyond the entry file** (done 2026-10-24). A quoted list
  in a loaded or imported module, or in a macro expansion, needed an explicit
  `(load "stdlib/datum.tur")`. Now every file the program reads is scanned, not
  just the entry:
  - `elab_quote_data_demand_load` (elab_toplevel.c) runs over a load-expanded
    form list -- the whole program in `elaborate_program`, after `(load ...)`
    expansion, and each imported module's forms in `elab_load_module` -- and,
    when a user form quotes a list and datum.tur is neither in the
    load-visited set nor in scope, splices in the forms of
    `(load "stdlib/datum.tur")`: ahead of the user's forms, or ahead of the
    module's own (so its `defmodule` stays the first form of its file, as the
    R7RS prelude does for an imported Scheme library). That is exactly what
    the program would get by writing the load, which is the form every engine
    already ran, so compiled C, `tur jit` and `tur --interpret` (whose user
    file arrives as a `(load ...)`) all get it from one place.
  - **Why a scan, not a load at the first `elab_quote`.** The quote is usually
    elaborated inside a defn body, where loading means elaborating datum.tur's
    definitions mid-body -- and a signature that names `Datum` above the first
    quote (`(defn f [] : Datum '(...))`, the common case) is resolved before
    any quote is reached. Scanning each file as it is read loads it before
    anything in that file needs it.
  - **Macros.** Only user files are scanned (no stdlib macro produces a quote;
    checked by grep), and a macro's expansion is seen through its definition:
    `form_quotes_a_list` also answers yes for `'~x` / `(quote ~x)` in a
    template, `'quote` (`(list 'quote x)`), `syntax-quote`, and the string
    `"quote"` (a `defmacro*` building it with `sym->syntax`). A false yes only
    loads a module the program did not need. The one quote it cannot see is
    one whose `quote` head a macro computes some other way; `elab_quote`'s
    diagnostic still names the explicit load for that.
  - A program that never quotes a list still loads nothing (checked: a
    flag-on program defining its own `datum->str` and `list-first` builds and
    runs on all three engines). Never for `#lang r7rs`: Scheme forms are
    skipped, and a Scheme entry turns the scan off.
  - Fixtures: `quote-data-load` (the quote is only in a loaded file, which
    also names `Datum` in signatures), `quote-data-import` (only in an
    imported module), `quote-data-macro` (only in expansions: a quasiquote
    template, a `(list 'quote x)` body, a `defmacro*` using `syntax-quote`).
- [x] **2. Option 3's Saffron arm** (`(List any)` in Saffron) -- decided, stays
  unbuilt: the `Datum` already works there through `any`, matching and
  printing included. Revisit only if Saffron users ask for untyped lists.
- [x] **3. `defmacro*` bodies see a quoted list as `Syntax`** (decided and
  implemented 2026-10-24). As landed they got the runtime `Datum` -- and only
  when the entry file happened to quote a list, since the macro-time env
  reused that autoload decision; otherwise the body failed to elaborate. A
  `Datum` was also useless there: it cannot be returned as the expansion
  (`(defmacro* m [] '(+ 3 4))` was "did not return a syntax object").
  - **Rationale.** A `defmacro*` body is code that computes forms, and its
    quasiquote already lowers to the syntax constructors. In Lisp `'(a b)` and
    `` `(a b) `` are the same value; keeping that identity means `'(a b)` in a
    `defmacro*` body is the `Syntax` `(a b)`. It is also what CL's
    `defmacro` does with a quoted list in its body (the list is the
    expansion), and it carries spans, which a `Datum` does not. The phase
    decides the representation of a quoted form -- `Datum` at run time,
    `Syntax` at macro time -- while a quoted symbol is a `Sym` in both.
  - `sxqq_walk` (elab_macros.c) lowers a quoted list or vector, and
    `(quote ...)` written out, through `sxqq_lower_quoted` under the flag: the
    leaves as the quasiquote lowering does them, a nested quote to
    `syntax-quote`, and an unquote inside the quote refused (it has no
    constructor; splicing is the quasiquote's job). A template `defmacro`
    is unchanged: its body is the expansion, so a quote there is a quote in
    the expanded code -- a `Datum` at run time.
  - Fixtures: `quote-data-defmacro-star` (returned, compared with
    `syntax=?`, walked with `syntax-len`, nested quote, quoted symbol), and
    `errors/quote-data-defmacro-star-unquote`.
- [x] **Corpus check** (re-run 2026-10-24). Every fixture directory holding a
  quote-shaped form (`'(`, `'[`, `(quote `, `'quote`, `syntax-quote`,
  `"quote"`, `'~`; 115 directories) was run with `--enable=quote-data` forced
  on through the user experiments file, under all three harnesses. A program
  without one of those cannot change behaviour: the flag acts only through
  `elab_quote` of a list, the `defmacro*` lowering, and datum.tur's load, and
  all three need one. Results with the flag on, after the fix below:
  `tests/run.sh` 113 passed, 1 failed; `tests/run-turi.sh` 101 passed,
  1 failed, 9 skipped of 111; `tests/run-jit.sh` 110 passed, 1 failed -- the
  one failure on each being `quote-data-off`. Two fixtures changed:
  - `quote-data-off` -- the gate-off fixture, by design; it inverts at item 4.
  - `errors/nil-tail-macro-expanded-definition` -- its template macro wrote
    the definition it expands to as `(quote (defclass ...))`, relying on a
    quoted list being evaluated. Under the flag that is a `Datum`, so the
    nil-tail error it pins never fired (the `Datum` then passed `: int`
    through the existing typed-node-to-`:int` erasure). Rewritten as a
    quasiquote, which spells the same expansion under both meanings.
  - `stdlib/` quotes no list outside comments, strings and inline-C, and
    nothing under `examples/` holds a quote-shaped form.
- [ ] **4. Drop the gate** (the graduation itself, at or before 0.71.0): delete
  the `EXPERIMENTS[]` row, move the name to `GRADUATED[]`, make
  `g_opt_quote_data` unconditional (`elab_quote`, `elab_try_println_datum`,
  `sxqq_walk`, the scans in stdlib_autoload.c, `elab_quote_data_demand_load`,
  `pipeline.c` / `repl.c` / `cmd_eval.c`), drop the `flags` files of the
  `quote-data-*` fixtures, invert or retire `quote-data-off` (the legacy
  meaning has no hatch left), update the `quote` entry in
  `stdlib/forms/control.tur` and the macros guide, and archive this plan.
