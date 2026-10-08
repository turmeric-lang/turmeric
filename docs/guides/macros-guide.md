---
title: Macros Guide
category: Language Basics
description: `defmacro` as a compile-time function from syntax to syntax -- quasiquote and splicing, manual hygiene, procedural `defmacro*`, macro-time imports, and why an expansion is type checked exactly like hand-written code
---

# Macros (`defmacro`)

Turmeric macros are compile-time functions from **syntax to syntax**.  A
`defmacro` runs during elaboration, receives its arguments as unevaluated
forms, and returns a form; the returned form then flows through the FULL
elaborator -- type checking, typeclass dispatch, effect rows, refinements,
borrow checking, everything.  Macro-generated code is not a second-class
citizen: it is checked exactly like code you wrote by hand, in both engines
(compiled and `--interpret`).

```turmeric
(defmacro twice [e] `(+ ~e ~e))

(twice 21)       ; expands to (+ 21 21) => 42
(twice "oops")   ; expands to (+ "oops" "oops") => TUR-E0006 type error
```

That second line is the load-bearing property: the expansion is wrong, and
the TYPE CHECKER says so.  When a diagnostic points into generated code, the
compiler appends a note naming the call you actually wrote:

```
error [TUR-E0006]: operator lookup failed for '+': ... first arg type cstr
note: in expansion of macro 'twice' -- the diagnostics above are inside
      code this call generated
```

Every core control-flow form you use daily -- `cond`, `when`, `for`,
`do-m`, the contract forms -- is a `defmacro` in `stdlib/macros.tur`; read
that file for a working style reference.

## Quasiquote, unquote, splice

| Syntax | Meaning |
|---|---|
| `` `form `` | quasiquote: build the form as a template |
| `~e` | unquote: substitute the value of `e` into the template |
| `~@e` | unquote-splicing: splice a list's elements into the surrounding form |

Splicing works into calls, vectors, and (via structural recursion) any
depth of the template.  A macro can also call **functions and other
macros** inside a splice expression, so a template can *generate* its
spliced sequence rather than merely forwarding one.

## Hygiene: deliberately manual

Turmeric macros are **unhygienic by design** (Common-Lisp style): a binding
introduced by a template captures a same-named binding at the call site.

```turmeric
(defmacro bad [e] `(let [tmp 99] (+ tmp ~e)))
(let [tmp 1] (bad tmp))    ; => 198, NOT 100: the template's tmp captured yours
```

The discipline is `gensym`: mint a fresh symbol for every binding a
template introduces.

```turmeric
(defmacro good [e]
  (let [t (gensym "tmp")]
    `(let [~t 99] (+ ~t ~e))))
(let [tmp 1] (good tmp))   ; => 100
```

Rule of thumb: any `let`, `fn` parameter, or loop variable your template
creates gets a `gensym`.  Capture is occasionally what you want (anaphoric
macros); when you rely on it, say so in the macro's docstring.

`gensym` freshness is real, not just a counter: a candidate name is checked
against the symbol table -- every symbol the reader has seen, including the
whole current file -- and already-interned names are skipped.  So a
hand-written `tmp_0` in your code cannot be captured by `(gensym "tmp")`;
the macro simply receives `tmp_1` (or later).

`(gensym)` only means something inside macro machinery (templates and
quasiquote), where it is expanded at compile time.  In ordinary runtime
code it is a hard error -- a fresh symbol has no runtime value.

## `^syntax` parameters: receiving raw AST

By default a macro argument is substituted into the template as code.  A
parameter marked `^syntax` instead binds the raw, unevaluated form, so the
macro body can WALK it with the compile-time builtins:

```turmeric
(defmacro field-name [^syntax decl]
  `(println ~(symbol-name (first decl))))
```

`^syntax` composes with variadic rest params (`& ^syntax decls`), which is
the workhorse for "iterate over my arguments" macros.

## Recursion over arguments

A variadic macro recurses over its rest list with `first` / `rest`, using
**`empty?`** as the base case:

```turmeric
(defmacro sum-all [& ^syntax xs]
  (if (empty? xs)
    `0
    `(+ ~(first xs) (sum-all ~@(rest xs)))))

(sum-all 1 2 3 4 5)   ; => 15
```

Two traps, both of which end in "maximum macro expansion depth exceeded"
(the cap is 256):

- **`nil?` is not the empty-rest test.**  An empty rest is an EMPTY LIST
  form, not nil, so `(nil? xs)` is always false and the base case never
  fires.  Use `(empty? xs)`.
- **There is no compile-time arithmetic** (see Limitations).  A "count
  down from N" macro cannot terminate: `~(- n 1)` splices the unevaluated
  form `(- n 1)`, which never equals `0`.  Drive recursion by argument
  list STRUCTURE, never by counting.

## The compile-time builtin set

Inside a macro body (outside templates, and inside `~`/`~@` escapes) the
compile-time evaluator provides:

- list/vector: `first`, `rest`, `second`, `cons`, `list`, `vec`, `nil?`,
  `empty?`, `list?`, `vec?`
- symbols and names: `symbol-name`, `str->sym`, `str-append`, `dot-sym`
  (single-argument: `(dot-sym x)` makes the `.x` accessor symbol),
  `gensym`
- syntax inspection: `type-ann?`, `type-ann-inner` (peek at a `: T`
  annotation form)
- logic: `=` (literals and symbols), `not`, and `if`

Plus calls to other macros and to compile-time-evaluable functions inside
splices.  That is the whole set -- notably absent: arithmetic, string
comparison beyond `=`, and any type inspection (see Limitations).  The set
is defined once, in `CT_BUILTIN_TABLE` in `src/compiler/elab_macros.c`,
which drives both the evaluator's dispatch and the decision to route a
substituted template through the evaluator at all.

One subtlety of that routing: a template runs through the compile-time
evaluator when it still carries quasiquote machinery or calls one of the
builtins above -- EXCEPT `=` and `not`, which appear in templates that are
pure runtime code and therefore do not trigger evaluation on their own.
A macro body whose only compile-time work is `=`/`not`/`if` should thread
it through a form that does trigger (in practice any real body has a
quasiquote, which always triggers).

## Procedural macros (`defmacro*`)

Where a `defmacro` body is a *template*, a `defmacro*` body is ordinary
Turmeric, evaluated at expansion time by the in-process interpreter
(the macro-time env).  Each parameter arrives as a **Syntax** value
wrapping the raw call-site form; the body computes with the full
language -- arithmetic, strings, recursion, local functions -- plus the
syntax vocabulary (`read-string`, `syntax-first/rest/nth/len/tag`,
`syntax->int` / `int->syntax` and friends, `syntax-list/vec/cons`,
`syntax-gensym`, `syntax=?`, `syntax-error`), and returns the expansion
as a Syntax.

```turmeric
(defmacro* const-sum [a b]                 ; compile-time arithmetic --
  (int->syntax (+ (syntax->int a)          ; impossible in a template
                  (syntax->int b))))

(const-sum 40 2)   ; compiles to the literal 42

(defmacro* twice [e] `(+ ~e ~e))

(twice (f))        ; expands to (+ (f) (f))
```

Quasiquote inside a `defmacro*` body is sugar for the syntax
constructors -- `~expr` splices a Syntax-valued expression (any
computation, not just a parameter), and `~@expr` splices the elements of
a list-shaped Syntax:

```turmeric
(defmacro* sum-first-last [& xs]
  `(+ ~(syntax-first xs) ~(syntax-nth xs (- (syntax-len xs) 1))))

(defmacro* call-all [f & xs] `(~f ~@xs 100))
(call-all add3 1 2)   ; expands to (add3 1 2 100)
```

The lowering is purely syntactic (`` `(+ ~e ~e) `` becomes
`(syntax-list (sym->syntax "+") e e)`), so the body stays ordinary typed
Turmeric.  Nested quasiquote and `~@` into vector templates are not
supported -- build those with the constructors.

Facts that matter in practice:

- **Definition-time checking.**  The body is compiled as
  `(fn [Syntax...] Syntax)` when the `defmacro*` is defined, so type
  errors in the body surface at the definition, not at some later call.
- **A variadic `& rest` arrives as ONE Syntax** wrapping the argument
  list; walk it with `syntax-len` / `syntax-nth` and ordinary integer
  recursion (see `tests/fixtures/macro-procedural-variadic/`).
- **Sandboxed and bounded.**  Macro-time code runs with every capability
  denied (no I/O, FFI, inline-C, async, import) and a step-fuel bound, so
  a runaway macro is a diagnostic, not a hung compile.
- **`syntax-error msg stx`** raises an expansion-time diagnostic at the
  offending argument's span with your message.
- **Kinds mix freely**: a procedural expansion may call template macros
  and vice versa; `tur expand` traces both identically.
- **The stdlib is loaded at macro time.**  The macro env gets the REPL's
  full preload (core macros like `cond`/`when`/`for`, the typed
  collections, typeclasses) plus the string files (`cstr.tur`,
  `str-build.tur`) -- so `str-concat`, `int->str`, and friends work in
  macro bodies, which is what name-synthesis macros live on.
- **Recursive helpers use `letrec`**, which types the self-call
  correctly:
  ```turmeric
  (defmacro* sum-lits [& xs]
    (letrec [walk (fn [i : int acc : int] : int
                    (if (< i (syntax-len xs))
                      (walk (+ i 1) (+ acc (syntax->int (syntax-nth xs i))))
                      acc))]
      (int->syntax (walk 0 0))))
  ```
  A `(def helper (fn ...))` also works, but its self-call resolves by
  runtime dispatch (a TUR-W0040 note) and types `:int`, so an
  `(if p stx (helper ...))` will not unify -- prefer `letrec`.
- **The derive pattern ports cleanly**: see
  `tests/fixtures/macro-procedural-derive/`, where a `defmacro*` builds
  the same `Show` instance the stdlib template `derive-show-cstr` emits,
  with a typed recursive field walker instead of macro-recursion
  contortions.

## Macro-time imports (`:for-macros`)

`(import m :for-macros)` inside a `defmodule` evaluates module `m` into
the macro-time env, so `defmacro*` bodies can call its functions at
expansion time -- shared macro-time helper libraries:

```turmeric
;; mhelp.tur
(defmodule mhelp
  (export mx-add)
  (defn mx-add [a : int b : int] : int (+ a b)))

;; main.tur
(defmodule prog
  (import mhelp :for-macros)
  (defmacro* csum [a b]
    (int->syntax (mx-add (syntax->int a) (syntax->int b))))
  (defn main [] : int (println (csum 40 2)) 0))   ; compiles to 42
```

The rules:

- **Macro-time ONLY.**  `:for-macros` never imports the module at
  runtime, and it cannot combine with `:as`/`:refer` -- a module needed
  in both phases is imported twice: `(import m)` and
  `(import m :for-macros)`.  This is the deliberate, Racket-inspired
  phase distinction: which code runs at compile time is always explicit
  in the source, never inferred.
- The module's functions bind under their **bare names** in the macro
  env (qualified `m/name` forms do not resolve there).
- Repeat `:for-macros` imports of the same module in one compile are
  deduped.
- Resolution uses the compiler's own module search (importing file's
  directory, then the stdlib, then `-I` include dirs).
- A `defmacro*` DEFINED in another module needs no `:for-macros` at all
  -- export it and `:refer` it like any macro
  (`tests/fixtures/macro-cross-module-procedural/`).

## Bounded type reflection (`syntax-struct-fields`)

`(syntax-struct-fields T)` -- available only inside a running macro
expansion -- takes a Syntax symbol naming a single-constructor record (a
`defstruct`) and returns its field names as a Syntax list of symbols.
This is the R3-sanctioned shape of type reflection: a bounded, total,
flat projection of the compile's registry; no Type value ever becomes a
macro-time value.  It is what lets a derive macro take just the type:

```turmeric
(defmacro* derive-show3 [TypeName]
  (letrec [fields (syntax-struct-fields TypeName)
           ...]
    `(definstance Show [~TypeName] ...)))

(derive-show3 P3)   ; no field list -- see tests/fixtures/macro-reflect-derive/
```

An unknown name, an opaque newtype, or a multi-constructor/positional
data type is a plain expansion-time diagnostic ("walk its variants
explicitly").

## Procedural reader macros (by composition)

A reader macro whose template expands into a `defmacro*` call gives the
read-time syntax a full-language expander -- the RM5 "function expanders"
plan point, delivered by composition instead of a second mechanism:

```turmeric
(defmacro* csum* [& xs] ...compile-time fold...)
(reader-macros/define 'csum :datum-bracket '(csum* $body))

(println #csum[1 2 3 4 5])   ; compiles to the literal 15
```

See `tests/fixtures/reader-macros-procedural/`.

## Effectful macros (`--macro-caps=io`)

Macro-time code runs with every capability denied, and that covers native
functions reached by name as well as the builtins: a `defmacro*` body that
calls `process/spawn` or `r7rs-unlink__` gets an expansion-time diagnostic
naming the capability, and nothing runs
(`tests/fixtures/errors/macro-native-denied/`).  For the rare
legitimately-effectful macro (an embed-file style generator), the global
flag `--macro-caps=io` re-grants I/O and file access by path; anything else
-- process, environment, ffi, unsafe, inline-C, async -- is refused by the
flag parser and never available at expansion time.  Without the flag, an I/O
call in a macro body is a plain expansion-time diagnostic
(`tests/fixtures/errors/macro-io-denied/`).

## Refusing procedural macros (`--no-proc-macros`)

The global flag `--no-proc-macros` refuses every `defmacro*` definition, call
and `:for-macros` import with a diagnostic, so no macro-time code runs at all.
Template `defmacro` still expands, since substitution runs nothing.  It is
rust-analyzer's `procMacro.enable = false`: the switch for pointing `tur check`
at a tree you have not read yet (`tests/fixtures/errors/no-proc-macros/`).
- **Unhygienic like `defmacro`** -- mint bindings with `syntax-gensym`.

Prefer a plain `defmacro` template when substitution is all you need; it
expands without spinning the interpreter.  Reach for `defmacro*` the
moment you need computation the CT evaluator refuses (counting,
arithmetic, string synthesis, structural analysis).

## Multi-form bodies

A `defmacro` body may be several forms: every form before the last is
compile-time SETUP, evaluated in order by the compile-time evaluator's
`do` (with `(def x v)` spliced into let bindings), and the LAST form's
value is the template.

```turmeric
(defmacro square-sum [a b]
  (def a2 (list * a a))
  (def b2 (list * b b))
  `(+ ~a2 ~b2))

(square-sum 3 4)   ; => 25
```

A multi-form body always runs through the compile-time evaluator -- a
`(do setup... template)` sequence has no meaning as a literal template.

## Generating names, and whole typed declarations

`symbol-name` + `str-append` + `str->sym` synthesize identifiers, and a
single macro invocation can emit SEVERAL top-level definitions by wrapping
them in `(do ...)` -- top-level `do` splices into the program:

```turmeric
(defmacro def-record [name T]
  `(do
     (defstruct ~(str->sym (str-append (symbol-name name) "Rec"))
       [val : ~T count : int])
     (defn ~(str->sym (str-append (symbol-name name) "-mk")) [v : ~T]
       : ~(str->sym (str-append (symbol-name name) "Rec"))
       (make-struct ~(str->sym (str-append (symbol-name name) "Rec")) v 1))))

(def-record Score :float)
;; defines struct ScoreRec [val : float count : int]
;; and (Score-mk 7.1) : ScoreRec
```

## Types in templates: full power in the write direction

Type positions are ordinary syntax at expansion time, so a macro can
interpolate ANY type -- simple, compound, or synthesized -- into any type
position, and the checker resolves it downstream:

```turmeric
;; a passed-in type token
(defmacro typed-id [T] `(fn [x : ~T] : ~T x))
((typed-id :int) 42)

;; compound types, one template -> several typed instantiations
(defmacro defvecfn [name T dflt]
  `(defn ~name [v : (Vec ~T)] : ~T
     (if (> (vec-len v) 0) (vec-get v 0) ~dflt)))
(defvecfn first-int :int 0)
(defvecfn first-str :cstr "none")

;; synthesized names resolve (and FAIL like hand-written types when wrong)
`(defn make-it [] : ~(str->sym (str-append (symbol-name prefix) "Bar")) ...)
```

This extends to ascriptions (`(:: e (Vec ~T))`), `defstruct` field types,
inline-C-adjacent signatures, and even `#row{...}` elements
(`#row{~T int}`).  A synthesized type that resolves to nothing is a real
elaboration error, not silent acceptance -- the checker genuinely
evaluates what you spliced.

The one thing a macro CANNOT do with types is the read direction: see
Limitations.

## Emitting inline-C

A template may contain an inline-C block; the enclosing `defn` form is
emitted like any other.  Remember the repo style rule: the closing
` ``` ` and its `)` stay on one line (` ```) `).

## Limitations, stated plainly

These are design boundaries, not bugs.  Each has a written rationale in
[row-types-followups-plan.md](https://github.com/turmeric-lang/turmeric/blob/main/docs/upcoming/hold/row-types-followups-plan.md)
(R3) and the documents it cites; the forward direction -- procedural
macros running the full language on turi, with this evaluator frozen --
is [macro-system-direction-plan.md](https://github.com/turmeric-lang/turmeric/blob/main/docs/archive/macro-system-direction-plan.md).

- **No compile-time arithmetic.**  The evaluator's logic is `=`/`not`/`if`
  over forms; there is no `+`, `-`, or `<` at expansion time.  Repetition
  is driven by argument list structure (recurse with `first`/`rest`/
  `empty?`), never by a counter.  If a counting macro seems necessary,
  restate the input so the count is a LIST (`(m a b c)` rather than
  `(m 3 x)`).
- **Macros cannot ask types.**  There is no `type-of` at expansion time:
  a macro cannot branch on an argument's inferred type.  The compile-time
  value domain is forms and compile-time functions only -- no `Type` case.
  Type-directed behavior belongs one layer down, in typeclasses, which
  dispatch on types with full checking.  (This is R3 of the row-types
  follow-ups plan, deliberately unscheduled; the guide you are reading is
  the loop-closure for it.)
- **Rows are not values** at runtime or expansion time (R2 of the same
  plan): `#row{...}` interpolates into type positions but cannot be
  inspected, matched, or passed as data.
- **Hygiene is manual** (see above) -- intended, not pending.
- **Expansion depth is capped at 256.**  Hitting the cap almost always
  means a base case that never fires; the diagnostic's note lists the two
  usual causes.
- **`dot-sym` is single-argument** -- it builds a `.field` accessor
  symbol.  General name synthesis is `str->sym` + `str-append` +
  `symbol-name`.
- **`=` compares literals and symbols**, not arbitrary forms; deep
  structural comparison of two syntax trees needs hand-rolled recursion.

## Debugging expansions

- **`tur expand <file.tur>`** type-checks the file and prints every macro
  expansion to stdout as it happens, each under a
  `;; <macro-name> @ <file>:<line>:<col>` header naming the call site.
  Nested expansions print too (inner-first, in elaboration order), so a
  recursive macro's whole unfolding is visible.  Diagnostics stay on
  stderr, so the stdout trace is golden-file-able.
- **`:expand <form>`** at the REPL expands the form's head macro exactly
  ONCE and prints the result -- one step at a time, for template and
  `defmacro*` macros alike, including macros defined at earlier prompts.
- A diagnostic inside generated code carries the note
  `in expansion of macro '<name>' ...` pointing at the outermost call you
  wrote.  Inner nested-macro frames are deliberately silent -- one note
  per user-visible call.
- To see what a macro produces in context, `tur expand` is the first
  stop; to see the final compiled shape, inspect the emitted C with
  `tur emit-c` (generated defns appear under their synthesized names), or
  evaluate the expansion under `tur --interpret` for the fastest
  iteration loop.
- `maximum macro expansion depth exceeded` -> re-read "Recursion over
  arguments" above; the answer is nearly always `empty?` or the
  no-arithmetic rule.

## History

The macro system's sharp edges were filed and fixed as they were hit;
the paper trail is in `docs/archive/history/` (splice-into-vector,
unquote in type position, inline-C emission, multiple top-level forms,
compile-time calls in splices, `^syntax` parameters, and more).  If a
limitation you hit is not listed above, check there before assuming it is
by design.
