---
title: Syntax Guide
category: Getting Started
description: How to read and write Turmeric -- s-expression and sweet-expression syntax
---

# Turmeric Syntax Guide

This guide is the front door to Turmeric's surface syntax. It teaches you to
*read* and *write* Turmeric source in both dialects:

1. The default **S-expression** dialect used by `.tur` files. SRFI-105
   **curly-infix** is enabled here too -- `{a + b}` reads as `(+ a b)` in
   every dialect, no `#lang` directive required.
2. The **sweet-expression** dialect activated by `#lang turmeric/sweet`
   (legacy alias: `#lang sweet-exp`) or a `.tur.sweet` extension (indentation
   + neoteric + `$` + curly-infix).

The `#lang` line names one base dialect and nothing else; see
[Part 2.5](#part-25----lang-base-dialects).

It does not re-explain the semantics of every special form -- the deep-dive
guides own that. Instead it shows you the *shape* of the language and points
you at the right reference for the details.

Throughout, code is shown in paired blocks: a `turmeric` block followed by its
`sweet-exp` equivalent. Use the toggle above each pair to switch between them.

---

## Part 1 -- S-expression syntax

The default dialect is a Lisp. Every program is a tree of **forms**: atoms
(numbers, strings, symbols, keywords) and lists that group them.

### Lexical conventions

**Identifiers** are symbols like `square`, `vec-push!`, `option-some?`, or
`make-adder`. Conventionally, a trailing `!` marks mutation (`vec-push!`) and a
trailing `?` marks a predicate (`option-none?`). Operators like `+`, `-`, `*`,
`<=`, and `=` are ordinary identifiers used in prefix position.

**Keywords** start with a colon: `:int`, `:as`, `:name`. They are
self-evaluating and most often appear as type annotations, option names, and
map keys.

**Numbers** include integers (`42`, `-5`), floats (`3.14`, `0.1`), and the
radix forms documented in the reader reference -- hex (`0xFF`), binary
(`0b1010`), and octal (`0o755`).

**Strings** are double-quoted with C-style escapes (`"line\n"`, `"tab\tend"`,
`"quote\"inside"`).

**ASCII only.** Source must be ASCII -- the reader hangs on non-ASCII bytes
such as UTF-8 em dashes. Always write `--` (double hyphen), never an em dash.

For the exhaustive catalogue of literal forms (radix integers, character
escapes, keyword rules), see the
[Reader Forms Reference](reader-forms-guide.md#literals).

### Comments

```turmeric no-check
(println "hi")  ; line comment -- to end of line

#|
  Block comment. These nest:
  #| inner |#
|#
(println "ok")
```

Doc comments use the triple-semicolon marker `;;;` and sit immediately above a
`defn`, `defmacro`, `defstruct`, or `definstance`. The docstring format (its
required `Parameters:`/`Returns:`/`Example:`/`Since:` sections) is specified in
the project's docstring standard. See the
[Reader Forms Reference](reader-forms-guide.md#comments) for the full comment
catalogue including the planned datum comment `#;`.

### The form tree

Turmeric has four bracketed containers, each legal in specific positions:

| Form | Syntax | Where it is legal |
|---|---|---|
| List | `(f x y)` | everywhere -- the universal form |
| Vector | `[a b c]` | binding position (`let`/`defn` params); expression position |
| Map literal | `#map{:k v}` | expression position |
| Set literal | `#set{a b}` | expression position |

In binding position `[...]` is a *binding spec* (parameter list or `let`
bindings). In expression position, `[...]` lowers to `(vec-of ...)`. See the
[Data Literals Guide](data-literals-guide.md) for the literal collection
semantics.

### Function application

A list evaluates as a call: the head is the function, the rest are arguments.
Zero-argument calls are just an empty-argument list.

```turmeric
(+ 1 2)            ; => 3
(square 9)         ; => 81
(vec-new)          ; zero-arg call
```
```sweet-exp
{1 + 2}            ; => 3
square(9)          ; => 81
vec-new()          ; zero-arg call
```

### Special forms you will see first

These are the forms a newcomer meets immediately. This guide only shows their
*shape*; follow the links for semantics.

`def` and `defn` -- bind a value or a function. Parameter and return types are
annotated with `:type`:

```turmeric
(defn square [x : int] : int (* x x))

(square 9)    ; => 81
```
```sweet-exp
defn square [x :int] :int
  {x * x}

square(9)    ; => 81
```

`let` -- local bindings scoped to its body:

```turmeric
(let [x 10 y 20] (+ x y))   ; => 30
```
```sweet-exp
let [x 10 y 20] {x + y}   ; => 30
```

`if` -- an expression; both branches required:

```turmeric
(defn abs [n : int] : int
  (if (< n 0) (- 0 n) n))
```
```sweet-exp
defn abs [n :int] :int
  if {n < 0}
    {0 - n}
    n
```

`cond` -- ordered multi-way branch with an `else` fallback:

```turmeric
(defn sign [n : int] : int
  (cond (> n 0) 1
        (< n 0) -1
        else    0))
```
```sweet-exp
defn sign [n :int] :int
  cond
    >(n 0)
    1
    <(n 0)
    -1
    else
    0
```

The fallback clause is also spelled `:else` -- the keyword is the older
spelling and still accepted, in `cond` and in `case` alike, so the two may be
mixed within one file.

`else` is not a global binding: outside a clause head it is still an unbound
symbol, so `(if else 1 2)` remains an error. The one program the bare spelling
changes is an exotic one -- a `cond` whose test position reads a *variable*
named `else`. Rename that variable; the clause head now wins.

`when` -- one-armed conditional for side effects:

```turmeric
(when (< x 0) (println "negative"))
```
```sweet-exp
when {x < 0}
  println("negative")
```

`fn` -- an anonymous function capturing its lexical scope:

```turmeric
(let [add5 (fn [x : int] : int (+ x 5))]
  (add5 10))    ; => 15
```
```sweet-exp
let [add5 (fn [x :int] :int {x + 5})]
  add5(10)    ; => 15
```

`for` -- bind a counter over a half-open range:

```turmeric
(for i 0 5 (println i))
```
```sweet-exp
for i 0 5 println(i)
```

Other forms you will meet -- `do` (sequence side effects), `while` (loop while
a condition holds), `import`/`export` (module wiring), `defstruct` (record
types), and `defmacro` (syntax extension). Each has a dedicated guide:

- Binding forms (`let`, `letrec`, named let): [binding-forms-guide.md](binding-forms-guide.md)
- Structs: [structs-guide.md](structs-guide.md)
- Modules, `import`/`export`: [module-system-guide.md](module-system-guide.md)
- Macros (`defmacro`): [macros-guide.md](macros-guide.md)

A `while` may carry a written loop invariant directly after its condition --
`(while (< i n) :invariant (>= i 0) body...)` -- which is checked on entry and
after every iteration and, when proved, usable after the loop (see
[refinement-types-guide.md](refinement-types-guide.md)).

### Type-annotation syntax

Types appear after a name with a leading colon. In a parameter list each
parameter is followed by its type; the return type follows the closing `]` of
the parameter vector:

```turmeric
(defn add [a : int b : int] : int (+ a b))
```
```sweet-exp
defn add [a :int b :int] :int
  {a + b}
```

Compound types use parenthesised constructors -- `(vec T)`, `(-> a b)`,
`forall`, and friends. A variadic tail is written `& rest :T`. The full
compound-annotation grammar is in the
[Type Annotations Guide](type-annotations-guide.md); variadic rules are in the
function-arity section of the project conventions.

### Naming a type -- `defalias` vs `deftype`

Two forms bind a type name, and they do different things.

`defalias` is the **transparent** alias. The name and its target are the same
type everywhere, so the alias never shows up in unification -- it is a
readability tool, not a new type. The target can be any type expression:

```turmeric
(defalias Sample    :int)                            ; primitive
(defalias IntList   (Cons int))                      ; type application
(defalias Point     P)                               ; struct / ADT name
(defalias Backtrack (fn [] int))                     ; function type
(defalias NonZero   #refine{ q : int | (not= q 0) }) ; refinement
```
```sweet-exp
defalias Sample    :int                              ; primitive
defalias IntList   (Cons int)                        ; type application
defalias Point     P                                 ; struct / ADT name
defalias Backtrack (fn [] int)                       ; function type
defalias NonZero   #refine{ q : int | (not= q 0) }   ; refinement
```

Because it is transparent, `(defn f [b : Backtrack] : int (b))` applies `b`
exactly as `(fn [] int)` would, and a `Point` is accepted anywhere a `P` is.

`deftype` is the **recursive type binder**. It wraps its body in a recursive
type, which is what `Fix`/`Free` need, and takes its parameters as a bracket
vector:

```turmeric
(deftype Fix [^f] (f (Fix f)))
```

That wrapping makes `deftype` the wrong tool for naming a non-recursive type:
the bound name is a distinct nominal type, so use sites fail to unify with the
body. Reach for `defalias` there. The one exception is a bare refinement body,
which `deftype` binds transparently for `stdlib/refine.tur`'s benefit --
`defalias` handles that case too, and is the clearer spelling in new code.

Neither form takes alias type parameters: `(defalias Name [a] ...)` is an
error, and a `deftype`'s parameters belong to the recursive type, not to an
alias. For a named parametric shape, use `defstruct`/`defdata`.

Two more forms are adjacent but distinct: `defopaque` makes a **nominal**
newtype (deliberately *not* interchangeable with its representation), and
`defstruct` declares a record.

### Indentation conventions

Turmeric source follows Clojure-style indentation.

**Regular calls** align arguments under the first argument:

```turmeric no-check
(some-long-fn arg1
              arg2
              arg3)
```

**Special forms and macros** (`defn`, `fn`, `let`, `if`, `when`, `do`, `cond`,
`for`, `while`) use a fixed 2-space body indent regardless of column:

```turmeric no-check
(defn greet [name :cstr] :void
  (println name))
```

**Binding vectors** align pairs under one another, one pair per line. Never
split a name from its value across lines:

```turmeric no-check
(let [x   1
      y   2
      foo (+ x y)]
  foo)
```

The canonical formatter enforces these rules automatically -- see
[Canonical formatting](#canonical-formatting) below.

### Inline-C blocks

Turmeric can embed C in a fenced ` ```c ... ``` ` block inside a `defn` body.
The closing triple-backtick **must share its line with the enclosing `)`** --
written ` ```) ` -- so that Markdown renderers do not mistake it for the end of
a surrounding documentation code fence:

```turmeric no-check
(defn file-size [f] :int
  ```c
  FILE* file = (FILE*)f;
  return (int)ftell(file);
  ```)
```

See the [C Integration Guide](c-integration-guide.md) for FFI details and the
[Reader Forms Reference](reader-forms-guide.md#inline-c-block----c--) for the
fence grammar.

---

## Part 2 -- Sweet-expression syntax

Sweet-expressions are an alternate surface syntax that removes most parentheses
without changing the underlying form tree. Every sweet-exp program reads to the
*same* AST as its s-expression equivalent -- the toggle widgets in this guide
are verified to be parse-equal.

### Opting in

Activate sweet-exp one of two ways:

- A `#lang sweet-exp` directive on the first line of a `.tur` file.
- A `.tur.sweet` file extension.

A complete runnable snippet opens with the directive:

```turmeric
(defn main [] :int 0)
```
```sweet-exp
#lang sweet-exp

defn main [] :int
  0
```

Inline fragments in prose omit the directive (as the paired examples in this
guide do). If the directive is absent and the file is not `.tur.sweet`, the
reader stays in plain s-expression mode and treats indentation as
insignificant.

Both activations work the same way whether the file is the one you compile or
one pulled in by `(load "...")`: a loaded file's dialect is read from its own
first line and its own extension, independently of whatever dialect the loading
file is written in. When both are present the extension picks the base dialect
and the directive is a redundant hint.

### The three tools

Sweet-exp gives you three independent tools. Use whichever reduces noise for a
given expression; they compose freely.

**1. Indentation (t-expr)** -- a leading-whitespace block replaces the outer
`(...)` of a form. Use it for top-level forms and multi-line bodies:

```turmeric
(defn factorial [n : int] : int
  (if (<= n 1) 1 (* n (factorial (- n 1)))))
```
```sweet-exp
defn factorial [n :int] :int
  if {n <= 1}
    1
    {n * factorial({n - 1})}
```

**2. Neoteric `f(x y)`** -- replaces `(f x y)` for inline calls. The opening
paren must touch the function name with no space. Operators work too --
`+(x y)` is `(+ x y)`:

```turmeric
(println (vec-len v))
```
```sweet-exp
println(vec-len(v))
```

**3. Rest-of-line `$`** -- replaces the outer `(...)` when a line's only
argument is itself a single nested call. Prefer it over neoteric when the outer
call takes exactly one argument:

```turmeric
(println (vec-get squares i))
```
```sweet-exp
println $ vec-get squares i
```

`$` wraps everything to its right in one pair of parens -- but only when the
rest of the line is a bare token sequence that needs the wrap. When the rest is
*already* one complete delimited expression -- a neoteric call, a parenthesised
form, a curly-infix group, a data literal -- the wrap is suppressed, so `$`
composes with the other two tools instead of double-applying them:

| Rest-of-line shape | Written | Reads as |
|---|---|---|
| bare token sequence | `println $ vec-get squares i` | `(println (vec-get squares i))` |
| neoteric call | `println $ vec-get(squares i)` | `(println (vec-get squares i))` |
| parenthesised form | `println $ (vec-get squares i)` | `(println (vec-get squares i))` |
| curly-infix group | `println $ {a + b}` | `(println (+ a b))` |
| chained `$` | `println $ normalize $ vec3(x y z)` | `(println (normalize (vec3 x y z)))` |

A *bare atom* after `$` is still wrapped, per SRFI-110 -- `f $ g` reads as
`(f (g))`, a zero-argument call, not `(f g)`. Pass a function value with
plain juxtaposition (`f(g)` or `(f g)`) rather than `$`.

### Curly-infix `{a op b}`

Arithmetic and comparison read more naturally in infix. `{a + b}` lowers to
`(+ a b)`; nesting makes precedence visual:

```turmeric
(let [hyp (sqrt (+ (* a a) (* b b)))] hyp)
```
```sweet-exp
let [hyp sqrt({*(a a) + *(b b)})] hyp
```

### Data literals inside sweet-exp

The `#map{...}`, `#set{...}`, and `[...]` literals work transparently inside
sweet-exp -- the reader dispatch sits below the
sweet-exp layer, so neoteric and curly-infix compose inside a literal. See the
[Data Literals Guide](data-literals-guide.md) for the full semantics.

### What still uses traditional parens

A few forms stay clearer in s-expression syntax even in a sweet-exp file:

- **`import` / `export`** -- short enough that indentation adds nothing.
- **`cons` lists** -- `(cons x (cons y 0))` scans better than nested neoteric.
- **`fn` lambdas** -- always `(fn [x : int] : int {x * 10})`. See below.
- **`handle` blocks** -- see below.
- **Inline-C blocks** -- the ` ```c ``` ` fence is already special; keep the
  body as-is (the enclosing `defn` may still use sweet-exp form).
- **Trivially short expressions** -- `(nil-value)`, `(ok-val r)`, and similar
  one-liners.

### `fn` -- keep the parens

A lambda is almost always an *argument* to something else, so the sweet forms
bury the one delimiter that tells you where the lambda ends. Neoteric `fn(` in
particular reads as a call to a function named `fn`:

```sweet-exp
; Hard to scan -- where does the lambda stop?
over(px fn([v : int] : int {v * 10}) p)

; Clear -- the lambda is visibly one argument of three
over(px (fn [v : int] : int {v * 10}) p)
```

Curly-infix and neoteric still apply *inside* the body. Only the `fn` head keeps
its parens.

One consequence is worth stating outright, because it bites: **a `(...)` form
switches the indentation layer off for everything inside it** -- indentation
*and* `$`. Only delimiter-based syntax survives: s-expression calls, neoteric
calls `f(x)`, and curly-infix `{a + b}`. Once a lambda is parenthesised, its
body must be delimiter-complete:

```sweet-exp
; BROKEN -- `when` was relying on indentation the parens just took away, so
; this parses as four flat elements: fn, [], when, {..}, perform(..)
(fn []
  when {read-tvar(x) < 10}
    perform(Retry()))

; Correct
(fn []
  (when {read-tvar(x) < 10}
    perform(Retry())))
```

`$` is caught for you. A `$` inside brackets is rejected with **TUR-E0332**,
pointing at the marker:

```sweet-exp
; TUR-E0332: `$` has no meaning inside brackets
(fn [msg : cstr] : unit
  log/error $ str("a" msg))

; Correct -- give the call a delimiter
(fn [msg : cstr] : unit
  log/error(str("a" msg)))
```

`tur explain TUR-E0332` has the long form. The same `$` is correct as soon as
it is outside every bracket, where the indentation layer is live again:

```sweet-exp
defn f [msg : cstr] : nil
  log/error $ str("a" msg)      ; (log/error (str "a" msg))
```

### `handle` -- keep the clause and its body on one line

`handle` takes a *flat* argument list that pairs up two at a time:
`(handle expr (Eff [p] k) body (Eff2 [p] k) body ...)`. Sweet-exp indentation
gives every element its own line, which splits each clause from the body that
answers it -- so the pairing, the only thing a reader needs, is exactly what the
layout destroys:

```sweet-exp
; Bad -- which body belongs to which clause? Count lines and hope.
println
  handle compute()
    (Add [x] k)
    resume(k {x + 10})
    (Mul [x] k)
    resume(k {x * 2})
```

Write `handle` in traditional parens, one clause per line:

```sweet-exp
; Good -- the pairing is on the page
println
  (handle (compute)
    (Add [x] k) (resume k {x + 10})   ; 3+10 = 13
    (Mul [x] k) (resume k {x * 2}))   ; 4*2  =  8
```

When a clause body **shares the line with its clause**, write it in
s-expression form. Curly-infix `{a + b}` stays; it is more readable for
arithmetic and is legal in every dialect. Mixing neoteric into a packed
one-liner makes it a two- or three-syntax hybrid that scans worse than either
pure form:

```sweet-exp
; Bad -- neoteric, s-expr and a neoteric zero-arg call on one line
(Log [msg] k) (do println(msg) (resume k nil-value()))

; Good
(Log [msg] k) (do (println msg) (resume k (nil-value)))
```

A **multi-line body** under a `;;` comment is ordinary code -- neoteric and
curly-infix read fine there, exactly as in a `(fn ...)` body. The one-liner rule
is about line density, not about banning neoteric inside `handle`:

```sweet-exp
(handle
  (let [new-state update-state(state)]
    draw(new-state)
    game-loop(new-state))
  ;; Render is forwarded to the backend, then the loop continues.
  (Render [obj] k)
    (do handle-render(obj)
        (resume k)))
```

`match` has the same flat pair-up-two-at-a-time shape as `handle`, so it takes
the same treatment: one pattern per line with the body it answers.

When a body is too long to share the line, give the clause its own `;;` comment
and indent the body under it -- punctuate the block rather than letting the
clauses run together:

```sweet-exp
(handle (serve req)
  ;; Log, then continue with the untouched request.
  (Log [msg] k)
    (do (println msg)
        (resume k nil))
  ;; A missing session is not fatal -- resume with the anonymous user.
  (Session [] k)
    (resume k (anonymous-user)))
```

### Mixing styles

You can mix s-expressions and sweet-exp tools within one sweet-exp file -- the
two share a form tree, so a traditional `(...)` list is always legal inside an
indented block, and neoteric/`$`/curly-infix may appear anywhere an expression
is expected. The guidance above (when to fall back to parens) is a *style*
recommendation, not a parser restriction. The canonical formatter normalises
plain `.tur` files but does not rewrite a sweet-exp file into s-expressions or
vice versa -- the dialect is a property of the source you choose to write.

### Side-by-side: a complete function

Here is `make-adder` -- a function returning a closure -- shown in both
dialects. (Lifted from the quickstart so it stays honest.)

```turmeric
(defn make-adder [n : int] (fn [x : int] : int (+ x n)))

(let [add3 (make-adder 3)
      add7 (make-adder 7)]
  (println (add3 10))    ; 13
  (println (add7 10)))   ; 17
```
```sweet-exp
defn make-adder [n :int]
  (fn [x :int] :int {x + n})

let [add3 make-adder(3)
     add7 make-adder(7)]
  println(add3(10))    ; 13
  println(add7(10))    ; 17
```

---

## Part 2.5 -- `#lang` base dialects

A `#lang` line selects one base dialect. Its full shape is:

```
#lang <language>[/<reader>]
```

and nothing else on the line. It is read *before the first form*, so the
choice is declared up front and is guaranteed file-scoped. It must be the
**first line**: the detector skips spaces and tabs before it, but not
comments, so a comment above the directive silently disables it.

`tur dialects` lists the bases, and `tur dialects --json` emits the same thing
machine-readably.

### A base names a language and a reader

The first, possibly slash-namespaced, token names **two** things: which
*language* the forms are elaborated as, and which *reader* parses them. For
`turmeric` and `saffron` they are independent axes, so each is spellable over
every reader; `r7rs` brings its own reader and takes no slash.

| Base | Language | Reader |
|---|---|---|
| `turmeric` | turmeric | plain s-expression (the default; curly-infix is always on) |
| `turmeric/curly-infix` | turmeric | curly-infix emphasis (same as the default) |
| `turmeric/neoteric` | turmeric | curly-infix + neoteric `f(x)` |
| `turmeric/sweet` | turmeric | full sweet-expressions (indentation + neoteric + `$`) |
| `saffron` | saffron | plain s-expression |
| `saffron/curly-infix` | saffron | curly-infix emphasis |
| `saffron/neoteric` | saffron | curly-infix + neoteric |
| `saffron/sweet` | saffron | full sweet-expressions |
| `r7rs` | r7rs | the Scheme reader (`#t`/`#f`, `#\c`, `#(...)`, `,`/`,@`, dotted pairs, `\|sym\|`, `#x`/`#e`... prefixes) |

**Saffron** is the dynamically typed dialect: an unannotated parameter or
return defaults to `any` instead of `int`, and the file gets a dynamic operator
layer, dynamic calls and dynamic field access. Annotations stay legal, and a
Saffron module links against a Turmeric one in the same program. See
[the Saffron guide](saffron-guide.md).

**R7RS** is R7RS-small Scheme, over Saffron's dynamic substrate under a
Scheme reader
([r7rs-lang-plan.md](https://github.com/turmeric-lang/turmeric/blob/main/docs/archive/r7rs-lang-plan.md)).
It **graduated in v0.57.0**: `tur dialects` shows it as `stable`,
`tur experiments` no longer lists a row, the `#lang r7rs` line is all that is
needed, and no lifecycle warning is printed. Today it has the reader (R1), the core
forms (R2): `define`, `lambda`, the `let` family, `do`, `cond`/`case`,
`and`/`or`, `set!`, `case-lambda`, multiple values and the core list, equality
and output procedures, with Scheme truthiness (only `#f` is false); and the
data and the seam (R3): mutable pairs, chars, vectors, bytevectors, records,
`quote`/`quasiquote` as data, `equal?` on cycles, `define-library`, and
`import` with `only`/`prefix`/`rename` and the `(turmeric <module>)` head, so
a Scheme program calls a Turmeric module or the stdlib and a Turmeric module
imports a `define-library` (its exports are `any`, narrowed with `cast`).
Strings are character sequences; a literal is an immutable `cstr` and a
string a procedure makes is mutable (T3). R4 adds
`syntax-rules` -- `define-syntax`, `let-syntax`, `letrec-syntax`,
`syntax-error`, the full pattern language and renaming hygiene (a template's
own binders cannot capture a use-site name; R10 closes the other direction, so
a template's free identifiers mean what they meant where the macro was
defined). R5 adds the
numbers: exact integers are int64 and continue as bignums past it (T1),
inexact reals are doubles, `(/ 7 2)` is the exact ratio 7/2 (T2), `3+4i` is
a complex number and `(sqrt -4)` is `+2i` (T6), and the R7RS
predicate, rounding, division, `expt`/`sqrt`/transcendental and radix
`number->string`/`string->number` surface is there. R6 adds control:
`call/cc` (re-entrant since T5: a continuation can be invoked after its
call/cc has returned, any number of times, which re-runs `dynamic-wind`
`before` thunks),
`dynamic-wind`, `with-exception-handler`/`raise`/`raise-continuable`/`guard`
and error objects (an uncaught `raise` reports on stderr and exits 70),
`parameterize`/`make-parameter`, and `delay`/`delay-force`/`force`. Every
Scheme procedure call is a proper tail call on both back ends, and the
compiled back end runs everything the interpreter does (a dynamic call is
capped at four arguments, so `apply` is too). R7 completes the libraries
short of ports: the rest of `(scheme base)`, and `(scheme char)` (ASCII case
mapping), `(scheme cxr)`, `(scheme complex)`, `(scheme time)`,
`(scheme process-context)` and `(scheme file)`'s `file-exists?` and
`delete-file`. The last three are loaded only when imported. `include` is
refused with the reason. `(scheme eval)`, `(scheme repl)`, `(scheme load)` and
`(scheme r5rs)` give `eval`, `environment`, `interaction-environment`,
`null-environment`, `scheme-report-environment` and `load` (T4). Importing
one links the interpreter into a compiled program, and a program that imports
none links nothing extra. Evaluated code runs in one embedded R7RS session
per run: data crosses by copy, and procedures and raised objects cross in
both directions.
R8 adds ports: string, bytevector and file ports, `read-char`/`read-line`/
`read-string`/`read-u8` and the rest of the R7RS I/O procedures, the current
ports as parameters (`(parameterize ((current-output-port p)) ...)`),
`write`/`display` that label cycles (`#0=(1 2 . #0#)`), `write-shared` and
`write-simple`, `(scheme read)`, and `(scheme file)`'s `open-input-file`,
`call-with-output-file`, `with-output-to-file` and the rest. One caution for
the compiled back end: a top-level `define` whose initializer has an effect
(opening a file, reading input) runs before the program's top-level
expressions, so put such code inside a procedure. R9 adds the tooling: `tur
repl --lang r7rs`, `tur fmt` (which re-indents a Scheme file and never rewrites
a token), `tur init --r7rs`, the language server, and editor highlighting.
R10 runs chibi-scheme's R7RS test suite as a ctest target
(`tur_r7rs_conformance`) that reports a pass count: 1082 of the 1216 tests
written in it pass, on both back ends, and the rest are the named carve-outs
(bignums, exact rationals, complex numbers, mutable strings, `eval`,
re-entrant `call/cc`); Section 9's tasks have since closed all of them, for
1223 passing invocations and none failing (two tests of chibi's own float
spelling, `e+308`, are counted as settled: R7RS allows both). A compiled
single-unit Scheme program allocates through the conservative collector on
Linux and macOS (`TUR_R7RS_GC=0` or `--no-r7rs-gc` opts out, which a program
that starts threads must do), and every Scheme fixture runs under ASan and
UBSan in `tur_r7rs_sanitize` (T8). `(scheme char)` maps and
classifies all of Unicode, from tables generated out of the Unicode
database. The full
reference is [r7rs-guide.md](r7rs-guide.md).

`turmeric`, `saffron` and `r7rs` are all **stable bases** -- none is gated.
`#lang saffron` and `#lang r7rs` each need no `--enable=` flag and no
`:experiments` entry, and print no lifecycle warning; `tur dialects` lists all
ten bases as `stable`.

`turmeric/sweet` is the preferred spelling for the sweet-exp base. The older
`#lang sweet-exp` is still accepted as a legacy alias, so
`#lang sweet-exp` and `#lang turmeric/sweet` are equivalent -- migrate to the
slash-namespaced form when convenient. A `.tur.sweet` extension selects the
sweet base without any directive.

Bases do not compose (sweet-exp is a whole indentation pass; curly/neoteric are
flags on the same reader), which is exactly why they share the one slash-named
slot.

### Nothing follows the base

A token after the base name is a compile error (`TUR-E0330`), never silently
ignored:

```
error [TUR-E0330]: `#lang` takes a single base dialect; unexpected trailing
token 'boguslayer' in path/to/file.tur
```

There used to be a second axis here -- an additive, order-independent set of
**layers** after the base, each flipping on a `#`-dispatch or an
elaboration gate. It was decommissioned. Its two members are both better
served elsewhere, and that is the rule to follow when you want one:

- A **one-off syntax convenience** belongs in a `#use-reader-macros` file. It
  is the supported per-file mechanism and it is what the layer set was
  competing with. See
  [the reader-forms guide](reader-forms-guide.md) for the directive.
- A **`#`-dispatch that everyone should have** belongs in the reader's
  built-ins, unconditionally. That is where `#s"..."` went: the owned-String
  literal is now always available, so `#lang turmeric stringed` is no longer
  a thing you write.

  ```turmeric
  (load "stdlib/string.tur")     ; the CODE it expands into; no directive
  (defn main [] : int
    (let [g #s"hello"]           ; owned String, not a borrowed cstr
      (string/len g)))
  ```

- A **semantic gate** belongs in `EXPERIMENTS[]` behind `--enable=<name>`,
  with the lifecycle warning and `expires_at` that carries. A semantic layer
  was never a second enable path -- it *was* its experiment, scoped to one
  file -- so the flag is the real mechanism and the layer was an alias for it.
  Static discharge of `#refine{...}` predicates, the one semantic layer there
  ever was, is unconditional now; see
  [refinement-types-guide.md](refinement-types-guide.md).

**Migration.** `stringed` -- the only layer token that ever shipped -- is
still accepted for one minor line, warned once (`TUR-W0064`) and ignored, so a
file that opted in per-file keeps compiling across the boundary. Drop the
token; nothing is lost, because what it turned on is now always on. At 0.50.0
it becomes the same `TUR-E0330` any other trailing token gets. See
[the decommission plan](https://github.com/turmeric-lang/turmeric/blob/main/docs/archive/lang-layers-decommission-plan.md).

---

## Part 3 -- Reference appendix

### Form cheat sheet

One row per construct: the s-expr form, its sweet-exp form, and a one-line
gloss.

| Construct | S-expression | Sweet-expression | Gloss |
|---|---|---|---|
| Call | `(f x y)` | `f(x y)` | function application |
| Operator | `(+ x y)` | `{x + y}` or `+(x y)` | infix arithmetic / prefix op |
| One-arg call | `(f (g x))` | `f $ g(x)` | rest-of-line argument |
| Define fn | `(defn f [x :int] :int ...)` | `defn f [x :int] :int` + indent | named function |
| Anon fn | `(fn [x :int] :int ...)` | `fn([x :int] :int ...)` | closure |
| Local binding | `(let [x 1] ...)` | `let [x 1]` + indent | scoped names |
| Conditional | `(if c a b)` | `if c` + indent | two-armed expression |
| Multi-branch | `(cond p1 e1 else e)` | `cond` + indented pairs | ordered dispatch |
| Side-effect guard | `(when c ...)` | `when c` + indent | one-armed conditional |
| Counted loop | `(for i 0 n ...)` | `for i 0 n ...` | range iteration |
| Sequence | `(do a b)` | `do` + indent | evaluate in order |
| Vector literal | `[a b c]` | `[a b c]` | growable array |
| Map literal | `#map{:k v}` | `#map{:k v}` | HAMT map |
| Set literal | `#set{a b}` | `#set{a b}` | set |

### Where to go deeper

| Topic | In-depth guide |
|---|---|
| Every reader form / literal grammar | [reader-forms-guide.md](reader-forms-guide.md) |
| Data literal semantics | [data-literals-guide.md](data-literals-guide.md) |
| Local binding forms | [binding-forms-guide.md](binding-forms-guide.md) |
| Compound type annotations | [type-annotations-guide.md](type-annotations-guide.md) |
| Structs | [structs-guide.md](structs-guide.md) |
| Modules / `import` / `export` | [module-system-guide.md](module-system-guide.md) |
| C interop / inline-C | [c-integration-guide.md](c-integration-guide.md) |
| Canonical formatting | [formatter-guide.md](formatter-guide.md) |
| End-to-end prose tour | [quickstart.md](quickstart.md) |

### Canonical formatting

`tur format` rewrites a file into canonical layout -- applying the indentation
rules above so you never have to hand-align. Use `--check` in CI to fail on
drift:

```turmeric no-check
tur format myfile.tur          # rewrite in place (via redirect)
tur format --check myfile.tur  # exit non-zero if not canonical
```

A before/after gives the flavour -- ragged input on the left, canonical output
on the right:

```turmeric no-check
;; before
(defn add [a :int b :int] :int
    (+ a
  b))

;; after `tur format`
(defn add [a :int b :int] :int
  (+ a b))
```

See the [Formatter Guide](formatter-guide.md) for the complete rule set and
editor integration.

### Common mistakes

A short list of pitfalls newcomers hit:

1. **Inline-C fence on its own line.** The closing ` ``` ` must share a line
   with `)` (written ` ```) `); a lone ` ``` ` breaks Markdown rendering.
2. **Splitting a binding pair across lines.** Keep each `name value` (or
   `name :type value`) on one line in a `let`/`loop` vector.
3. **A space before a neoteric paren.** `f (x)` is *not* neoteric application
   -- it reads as `f` followed by `(x)`. Write `f(x)`.
4. **Non-ASCII bytes.** An em dash or smart quote pasted from a doc will hang
   the reader. Use `--` and straight quotes only.
5. **Expecting `#lang sweet-exp` to enable neoteric in a plain file.** Without
   the directive (or a `.tur.sweet` extension) the reader stays in s-expression
   mode and indentation is insignificant.
6. **Mixing up `[...]` positions.** `[...]` is a value (lowers to `vec-of`)
   in expression position and a binding spec (parameter list / `let` bindings)
   in binding position -- the context determines which.
7. **Naming a definition after a special form.** `(defn return ...)`,
   `(defn match ...)`, `(defmacro open ...)` are accepted, but a bare call site
   dispatches to the form, never to your definition. See
   [Reserved names](#reserved-names) below -- the compiler flags this as
   `TUR-W0042` at the definition.
8. **An expression at `defmodule` top level.** There is no module-level
   side-effect position; see
   [What may appear inside a `defmodule`](#what-may-appear-inside-a-defmodule).
9. **A definition as the last form of a function body.** Legal anywhere else in
   the body, but not last -- see
   [Definitions inside a function body](#definitions-inside-a-function-body).

### Definitions inside a function body

A `defn` (or any `def*` form) may appear inside a function body. It is lifted to
file scope and stays callable by name from the enclosing function:

```turmeric
(defn outer [] : int
  (defn inner [x : int] : int (+ x 1))
  (inner 5))                  ; => 6
```

**It may not be the last form**, unless the function owes no value. A definition
produces nothing, so a function declared `: int` whose body ends with one has
nothing to return -- that is `TUR-E0713`:

```turmeric
(defn outer [] : int
  (let [x (helper)] x)
  (defn other [] : int 7))    ; error [TUR-E0713]
```

In practice this is almost always a **missing close paren**: `outer` was meant
to end after `x`, and everything following it got swallowed into its body. An
editor will happily reindent around the slip, so the diagnostic names it. Until
2026-08-29 this was silent -- the function returned 0, which is a successful
exit status, a `false`, and a "nothing found" all at once.

A `: nil` / `: void` function may end with a definition, since it owes no value.

### What may appear inside a `defmodule`

A `(defmodule ...)` body is a list of **definitions**, not statements. A
non-definition form as a direct child is rejected with `TUR-E0711`:

```turmeric
(defmodule dt
  (defn shout [] : int (println "hi") 1)
  (shout)                     ; error [TUR-E0711] -- never evaluated
  (defn main [] : int 0))
```

There is no module-level side-effect position, so put the call in `main` (or a
function `main` calls). This was silent until 2026-08-29: the form was fully
elaborated -- type errors inside it reported like live code -- and then dropped
from codegen, which cost 12 spices their entire test suites without a single
warning.

Legal as a direct child:

| Form | Note |
| --- | --- |
| `defn` `defmacro` `defmacro*` | |
| `defstruct` `defdata` `defopaque` `deftype` `defalias` | |
| `defclass` `definstance` `defeffect` | |
| `def` | a module-level global; its initializer *does* run |
| `extern-c` | |
| `import` `export` `export-from` | must come first, before any definition |
| a bare ` ```c ` block | supplies file-scope C declarations |
| `defer` | runs at process exit, not at module load |

The last two look like expressions and are deliberate. `defer` is the only
non-definition form that executes.

The rule is applied to the form *after* macro expansion, so a macro that
expands to a definition is fine wherever the definition would be:

```turmeric
(defmacro make-adder [name n]
  `(defn ,name [x : int] : int (+ x ,n)))

(make-adder add5 5)           ; OK -- expands to a defn
```

### Reserved names

A call head is matched against the special forms **by name, before any binding,
macro, or typeclass-method lookup**. Naming a `defn` or `defmacro` after one of
them is accepted and the binding is created, but every bare `(name ...)` call
site elaborates as the form, so the definition is unreachable by its bare name.
The compiler emits **`TUR-W0042`** at the definition; run
`tur explain TUR-W0042` for the full write-up.

`return` is the one that bites most often -- it is the conventional name for a
monadic unit, and `(return x)` is always the early-return form. Use `pure`.

Reserved in call-head position:

| Group | Names |
|---|---|
| Binding / control | `def` `define` `let` `let*` `letrec` `if` `do` `unsafe` `set!` `while` `case` `defer` `return` `match` `quote` `gensym` `?` `->` `->>` |
| Definition forms | `defn` `fn` (and its Greek-letter lambda alias) `extern-c` `defmacro` `defmodule` `import` `export` `load` `defstruct` `make-struct` `defopaque` `defdata` `defgadt` `defclass` `definstance` `defkind` `defrec` `deftype` `defalias` `defdynamic` `defeffect` `defprotocol` |
| Generators | `gen` `yield` `gen-next` `gen-done?` |
| References / rc / weak | `ref` `deref` `drop!` `ref?` `weak` `weak?` `upgrade` `lref/new` `rc/of` `rc/clone` `rc/drop` `rc->ptr` `rc/strong-count` `rc/from-ref` `ref/from-rc` |
| Continuations | `reset` `shift` `shift0` `call/cc` `call/cc*` `escape` `cloneable-reset` `cloneable-shift` `serial-reset` `serial-shift` `cont?` |
| Effects | `binding` `perform` `handle` `handle-shallow` `try-with` `with-handler` `resume` `discontinue` `compose-handlers` |
| Types / casts | `as` `type-of` `cast` `is?` `coerce` `&` `&mut` `forall` `exists` `type-app` `::` `pack` `open` |
| Sessions | `make-protocol` `make-session` |
| Panic | `panic` `panic-with` `catch-unwind` `catch-panic-of` `panic-payload-type` `panic-payload-value` `panic-payload-file` `panic-payload-line` `panic-payload-downcast` |
| Unsafe / FFI | `ptr-deref` `ptr-write` `ptr-add` `ptr-sub` `ptr-null?` `ptr-of` `unsafe-cast` `reinterpret` `transmute` `array-get-unchecked` `array-set-unchecked` `raw-malloc` `raw-free` `raw-realloc` `raw-memcpy` `raw-memset` `c-call` `dlopen` `dlsym` `dlclose` |
| STM / GC | `stm` `retry` `gc!` `gc-enable!` `gc-disable!` `gc-auto!` `gc-collections` `gc-objects-freed` `gc-live-blocks` `gc-candidate-high-water` |

A leading `.` is reserved too: `(.method obj ...)` is method-call syntax, so a
definition named `.foo` is never reachable as a call head.

**Not reserved -- these are deliberately shadowable.** A user definition of
`handler`, `with`, `default-of`, or a session op (`send` `recv` `close` `offer`
`send-to` `recv-from` `choose-left` `choose-right` `recv-timeout`) wins over the
form, and the map surface (`map-new` `assoc` `dissoc` `get` `has?` `count`
`merge`) falls back to ordinary call resolution. The arity-gated forms (`async`
`await` `select` `atomically` `check` `or-else` `thread-spawn` and the `tvar-*`
ops) intercept only one call shape, so a definition at a different arity is
genuinely callable -- no warning is emitted for those, and the shadowing is
worth avoiding anyway.

Inside a `defmodule`, a shadowing definition remains reachable through its
**qualified** name (`mymod/return`) -- a qualified head symbol never matches a
special form -- but the bare name stays shadowed, so renaming is still better.
