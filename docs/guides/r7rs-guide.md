---
title: "R7RS Scheme -- #lang r7rs"
category: Getting Started
description: "#lang r7rs runs R7RS-small Scheme on the Turmeric compiler and runtime, compiled or interpreted, and lets Scheme and Turmeric modules import each other. This guide covers getting started, what is there, how it differs from the standard, and the tooling."
---

# R7RS Scheme

`#lang r7rs` runs R7RS-small Scheme on Turmeric's compiler and runtime. A
Scheme file compiles to C like any Turmeric file, runs under the interpreter
too, and can import Turmeric modules or be imported by them.

```scheme
#lang r7rs
(import (scheme base) (scheme write))

(define (fact n)
  (if (= n 0)
    1
    (* n (fact (- n 1)))))
(display (fact 20))
(newline)
```

```sh
tur run fact.tur          # compile and run
tur --interpret fact.tur  # the tree-walking interpreter
```

The dialect **graduated in v0.57.0** and is an ordinary base dialect, on the
same footing as `#lang turmeric` and `#lang saffron`: `tur dialects` lists it
as `stable`, nothing needs enabling, a project manifest cannot refuse it, and
no lifecycle warning is printed (`--enable=r7rs` is accepted as a no-op for a
release, with a TUR-W0063 notice). The stages, design decisions and known gaps
live in
[docs/archive/r7rs-lang-plan.md](https://github.com/turmeric-lang/turmeric/blob/main/docs/archive/r7rs-lang-plan.md).

The single-file examples in this guide are compiled and run, on both back
ends, by `tests/fixtures/docs-r7rs-guide-examples`; the library examples by
`tests/run-r7rs-import.sh`.

## Getting started

- **A program** is a file of top-level forms. `tur run prog.tur` builds and
  runs it; there is no `main` to write.
- **Build time**: the runtime and the prelude are compiled once and cached
  under `<tmpdir>/tur-build/prelude/`, so the first build after installing
  `tur` takes about 9 s on one core, about 4.5 s on four (it compiles in one
  piece per CPU, up to eight; `TUR_PRELUDE_JOBS=<n>` sets the number), and
  later ones about 1 s (on Linux and macOS; Windows still builds each
  program as one unit). `TUR_PRELUDE_SPLIT=0` builds the
  program as a single C unit instead; `TUR_SHOW_CC=1` shows the two
  compiles, or why a program was built as one unit.
- **`.scm` files** are Scheme without the `#lang r7rs` line: `tur run
  prog.scm`, `tur build prog.scm` (the binary is `prog`), `tur check` and
  `tur --interpret` all take one, a `(load "util.scm")` reads one, and
  `(import (mylib))` finds `mylib.scm` when there is no `mylib.tur`. A
  `#lang r7rs` line in a `.scm` file is allowed and changes nothing.
- **A project**: `tur init --r7rs demo` scaffolds one that builds with
  `tur build .` and tests with `tur test tests`. Add `--lib` for a
  `define-library` instead of a program.
- **The REPL**: `tur repl --lang r7rs`, or type `#lang r7rs` at any prompt.
  Results echo in Scheme's own spelling (`=> (a "b" #\c)`). Several values
  echo one per line (`(values 1 2)` is `=> 1` then `=> 2`), and nothing is
  echoed for `(values)`, a definition or the unspecified value. Definitions,
  macros and imports last for the session, and a later turn may `set!` any
  variable an earlier one defined. `(import (prefix (mylib) m:))` of your
  own library does not work at the prompt; import it plainly, or with
  `only` or `rename`. The prompt takes Scheme only; type
  `#lang turmeric` to switch to Turmeric (the session resets).
- **Sweet-expressions**: `#lang r7rs/sweet` is the same language written
  with SRFI-110's indentation, neoteric calls and `$`, over Scheme's own
  lexemes (`#t`, `#\(`, `|two words|`, `#;` and `#|...|#` all read as they
  do in `#lang r7rs`). `f{n - 1}` is SRFI-105's `(f (- n 1))`, and `f[x]` is
  `f(x)`, since Scheme's brackets are parens. A library may be written this
  way and imported by a plain `#lang r7rs` program, or the other way round
  (`tests/fixtures/r7rs-sweet`):

  ```scheme
  #lang r7rs/sweet
  import (scheme base) (scheme write)

  define (fact n)
    if {n <= 1}
      1
      {n * fact{n - 1}}

  display $ fact 20
  newline()
  ```

  `tur fmt` checks a sweet file and leaves its layout alone, which is its
  syntax.
- **Formatting**: `tur fmt` re-indents a Scheme file and never rewrites a
  token. Each line's leading whitespace is recomputed; `#t`, `#\x`,
  `|two words|` and `#e1.5` stay exactly as written.
- **Editors**: the vim pack and the VS Code grammar highlight the Scheme
  lexemes in a `#lang r7rs` file, and the language server analyses and
  formats it.

## What is there

All of R7RS-small, and `(scheme r5rs)`'s environments:

| Library | Status |
|---|---|
| `(scheme base)` | complete, including string, bytevector and file ports |
| `(scheme case-lambda)`, `(scheme lazy)`, `(scheme inexact)` | complete |
| `(scheme char)` | complete, Unicode 16.0.0 case mapping and classification included (`tools/fetch-ucd.sh`, one pinned ICU release) |
| `(scheme cxr)`, `(scheme complex)` | complete |
| `(scheme write)` | `write` and `display` label cycles; `write-shared`, `write-simple` |
| `(scheme read)` | `read`, datum labels and cycles included |
| `(scheme file)`, `(scheme time)`, `(scheme process-context)` | complete; loaded only when imported |
| `(scheme eval)`, `(scheme repl)`, `(scheme load)`, `(scheme r5rs)` | complete; importing one links the interpreter into a compiled program (see Eval) |

The core forms are all there: `define`, `lambda`, the `let` family and named
`let`, `do`, `case`, `cond` with `=>`, `when`/`unless`, `case-lambda`, the
`-values` forms, `define-record-type`, `define-library` and `import` with
`only`/`except`/`prefix`/`rename` (nested freely), `cond-expand`, `syntax-rules`, `guard`,
`parameterize`, `delay`, `delay-force`, quasiquote.

SRFIs are imported as `(srfi N)`, the way Racket's R7RS imports them. The
SRFIs that R7RS already includes import at no cost; see SRFIs below for the
full table.

## Lists, vectors, strings

```scheme
(define xs (list 3 1 2))
(write (map (lambda (x) (* x x)) xs))        ; (9 1 4)
(write (vector-map + #(1 2) #(10 20)))       ; #(11 22)
(write (string-upcase "shout"))              ; "SHOUT"
(let loop ((i 0) (acc '()))
  (if (< i 3)
    (loop (+ i 1) (cons i acc))
    (begin (write acc) (newline))))          ; (2 1 0)
```

Every Scheme procedure call is a proper tail call on both back ends, so a
named-`let` loop runs in constant space however long it runs.

A string is a sequence of characters. `string-length`, `string-ref`,
`substring` and the rest count characters, not bytes. A string a procedure
makes -- `make-string`, `string`, `string-copy`, `substring`,
`string-append`, `list->string` -- is mutable, so `string-set!`,
`string-fill!` and `string-copy!` work on it:

```scheme
(define s (string-copy "caf\xE9;"))
(string-set! s 0 #\C)
(write (list s (string-length s)))           ; ("Café" 4)
```

A string literal is immutable, which R7RS allows. Mutating one is an error
that names the fix: `string-copy` it first. So is mutating a string from
`symbol->string`, `number->string`, `read` or Turmeric.

## Numbers

An exact integer has no size limit, an exact non-integer is a ratio, and an
inexact real is a double:

```scheme
(write (list (/ 7 2) (/ 6 2) (exact->inexact 3) (+ 7.1 0.25) (expt 2 100)))
; (7/2 3 3.0 7.35 1267650600228229401496703205376)
```

A double is written in the shortest form that reads back as the same double,
with a `.` or an exponent so that it stays inexact: `7.0`, `7.1`, `+inf.0`,
`-inf.0`, `+nan.0`. A very large or very small one takes an exponent written
without a `+`: `1e21`, `1.7976931348623157e308`. R7RS allows the exponent
either way, and the reader takes both (`1e+21` is `1e21`). chibi-scheme
writes `e+308`, which is why two of its tests are counted as settled (see
Conformance).

An exact integer is a 64-bit int while it fits, which keeps the common case
fast. Arithmetic that leaves 64 bits continues as a **bignum**, and a result
that fits again is an int again. Nothing wraps, and nothing stops the
program. Bignums work everywhere integers do: literals, `read`,
`string->number` and `number->string` in any radix, `quotient` and the other
divisions, `gcd`, `expt`, `exact-integer-sqrt`, and `exact` of a large
double (`(exact 1e30)`). A comparison between a bignum and a double is exact,
so `(= (- (expt 2 1000) 1) (inexact (expt 2 1000)))` is `#f`.

A bignum is a Scheme value only. Passed to a Turmeric procedure that takes an
`int`, it is the import's checked cast error (`cast: any holds R7rsBig, not
int`); a procedure that takes `any` receives it as it is.

Division of exact numbers is exact: `(/ 7 2)` is the ratio 7/2, kept in
lowest terms, and `(/ 6 2)` is the integer 3. Ratios work through the whole
tower. Arithmetic and comparison are exact, and `floor`, `ceiling`,
`truncate` and `round` give exact integers (`round` takes a tie to even).
`numerator` and `denominator` give the parts. `exact` of a double gives its
exact value, so `(exact .5)` is 1/2. `(expt 2 -10)` is 1/1024, `(sqrt 4/9)`
is 2/3, and `rationalize` finds the simplest rational in an interval.
`inexact` gives the nearest double:

```scheme
(write (list (+ 1/2 1/3) (exact .5) (round 7/2) (inexact 1/3)))
; (5/6 1/2 4 0.3333333333333333)
```

`#e` reads a decimal exactly, so `#e1.2` is 6/5, not the value of the double
1.2. A ratio passed to a Turmeric `int` or `float` parameter is a checked
cast error, as a bignum is; convert it on the Scheme side with `inexact` or
`round`.

**Visible change:** before r7rs-lang-plan T2, `(/ 7 2)` was the inexact 3.5.

**Complex numbers** are there too: `3+4i`, `-i`, `1/2+3/4i`, `1.5+2i` and
polar `1@0.5`. Their parts can be any real, and a complex number is exact or
inexact as a whole. An exact-zero imaginary part leaves the real itself, so
`3+0i` is 3 and `(* +i +i)` is -1. An inexact zero part stays, so
`(real? 1.0+0.0i)` is `#f`. Arithmetic and `=` work part by part; `<` and
the other orderings on a non-real are `#f`. `sqrt` of a negative number is
imaginary (`(sqrt -4)` is `+2i`), and `exp`, `log`, `expt` and the trig
functions leave the reals when they have to. `real-part`, `imag-part`,
`magnitude`, `angle`, `make-rectangular` and `make-polar` are in
`(scheme complex)`:

```scheme
(write (list (* 1+2i 3-4i) (sqrt -4) (magnitude 3+4i) (make-rectangular 1 2)))
; (11+2i +2i 5 1+2i)
```

A complex number passed to a Turmeric `float` parameter is a checked cast
error, as a ratio is.

**Visible change:** before r7rs-lang-plan T6, `(sqrt -4)` was `+nan.0` and a
non-real literal did not compile.

Numbers are read by one parser: in a source file, by `read`, and by
`string->number`. It knows the whole R7RS number syntax, so a ratio or a
complex number is one token. Number syntax that is no number, such as
`1/0`, is refused with the reason. That happens at compile time for a
literal. `read` and `string->number` raise an error a program can `guard`.
The number is never split into a number and a stray symbol, and never read
as a different number:

```scheme
(write (list 10/2 #i3/2 3+0i (string->number "1e2")))   ; (5 1.5 3 100.0)
(string->number "1/0")
; error: string->number: `1/0`: an exact rational with a zero denominator is not a number
```

## Macros

`syntax-rules` with the full pattern language, and hygiene for the
template's own binders:

```scheme
(define-syntax swap!
  (syntax-rules ()
    ((_ a b) (let ((tmp a)) (set! a b) (set! b tmp)))))
(let ((p 1) (q 2))
  (swap! p q)
  (write (list p q)))                        ; (2 1)
```

The `tmp` in the template cannot capture a `tmp` at the use site. The other
direction holds too: a free identifier in a template means what it meant
where the macro was defined, however the use site binds that name.

```scheme
(define-syntax my-list (syntax-rules () ((_ x) (list x))))
(write (let ((list vector)) (my-list 1)))    ; (1)
```

A local variable shadows a keyword or a macro of the same name, as R7RS says:
`(let ((if even?)) (if 7))` calls `even?`.

A `define-library` can export its macros like any other name, and an
importer can take them under `only`, `prefix` and `rename`. A macro's template
keeps its meaning in the importer, even when it calls a procedure the library
does not export, and even when the importer defines the same name itself.
A macro is Scheme syntax, so a Turmeric module that imports the library gets
its procedures but not its macros.

## Control

```scheme
(write (call/cc (lambda (k) (+ 1 (k 42)))))  ; 42
(write (guard (e ((error-object? e) (error-object-message e)))
         (error "something broke" 'detail))) ; "something broke"
(define depth (make-parameter 0))
(write (parameterize ((depth 1)) (depth)))   ; 1
(dynamic-wind
  (lambda () (display "[in]"))
  (lambda () (display "body"))
  (lambda () (display "[out]")))             ; [in]body[out]
(define (count-to n)
  (let ((k #f) (i 0))
    (call/cc (lambda (c) (set! k c)))         ; k re-enters here
    (set! i (+ i 1))
    (if (< i n) (k #f) i)))
(write (count-to 3))                         ; 3
```

A continuation is **re-entrant**. Invoking it while its `call/cc` is still
running returns from it, which is an escape. Invoking it after the `call/cc`
has returned makes the `call/cc` return again. Either can happen any number
of times, so generators and coroutines written with `call/cc` work.

Invoking a continuation travels the `dynamic-wind` stack. The `after` thunks
of the extent being left run first, innermost first. Then the `before` thunks
of the extent being re-entered run, outermost first.

A variable keeps its latest value across a re-entry, since a continuation
restores control, not state. Each top-level form runs under its own prompt,
so a continuation captured in one is the rest of that form: re-entering it
from a later form finishes the earlier form and then continues after the
form that invoked it, as chibi and Racket do.

`guard` and `raise` escape without copying anything. So does a `call/cc`
whose continuation can only be called while the `call/cc` is running: every
use of `k` heads a call, whether directly, inside a lambda handed to
`for-each`, `map` or their vector and string twins, or inside a named `let`
whose name only heads calls. That covers the usual early exit from a loop:

```scheme
(call/cc (lambda (return)
  (for-each (lambda (x) (if (p x) (return x))) l)
  #f))
```

Any other `call/cc` copies the stack between it and the program's start, so
it costs time and memory in proportion to that depth. This includes one
whose `k` is stored, returned, or passed to your own procedure. Under `tur
--interpret` the copy is never freed, but one close to an earlier copy of
the same stack keeps only the words that changed, so a generator's steps
stay small. Such a continuation belongs to the thread that captured it:
invoked on any other thread it raises an error object, "call/cc:
continuation invoked on a thread other than the one that captured it",
before any `dynamic-wind` thunk runs. An uncaught
`raise` reports on the current error port and exits with status 70.

A standard procedure given the wrong type, or any procedure given the wrong
number of arguments, raises an error object when the call runs: `(car 5)`
raises "car: not a pair" with `5` as the irritant, and `(f 1 2)` of a
one-argument `f` raises "f: too many arguments (expects 1, got 2)". It is
never a compile-time error, even when the argument is a literal, so a call
that a test keeps from running, such as `(if (pair? x) (car x) 0)` with `x`
bound to `#f`, compiles and never raises.

## Eval

`eval` runs a datum as code, in an environment that names the libraries it
sees:

```scheme
(import (scheme base) (scheme write) (scheme eval) (scheme repl))
(write (eval '(* 6 7) (environment '(scheme base))))            ; 42
(define env (interaction-environment))
(eval '(define (twice x) (* 2 x)) env)
(write ((eval 'twice env) 21))                                   ; 42
(write ((eval '(lambda (f) (f 10)) env) (lambda (n) (+ n 1))))   ; 11
```

Importing `(scheme eval)`, `(scheme repl)`, `(scheme load)` or `(scheme
r5rs)` links the interpreter into a compiled program. A program that imports
none of them links nothing extra. The evaluator is one embedded R7RS session
per run, so a definition evaluated in `(interaction-environment)` stays for
later `eval`s, a `define-syntax` included, and a later `eval` may `set!` a
variable an earlier one defined. `load` evaluates a file's forms the same way.

Values cross between the program and the evaluator:

- **Data is copied.** Numbers, booleans, characters, strings, symbols, lists
  and vectors go over as their `write` text and are read back on the other
  side. A pair that evaluated code mutates is a different pair from the
  program's.
- **Procedures cross as handles.** A procedure `eval` returns is called like
  any other. A program procedure passed into evaluated code is called back.
  Each keeps its identity, so it is `eq?` to itself when it comes back.
- **Raised objects cross too.** A `raise` or `error` inside evaluated code
  reaches the program's `guard`, and a program procedure's raise reaches a
  `guard` in evaluated code.
- A datum that holds a procedure or a record cannot be written, so it is an
  error.

The interpreter (`tur --interpret`) runs `eval` through the same embedded
session, so both back ends give the same answers.

A built program finds the stdlib it was built against. `TUR_STDLIB_DIR`
overrides that, and the program sets it for itself when unset.

## Ports

String, bytevector and file ports, with the current ports as parameters:

```scheme
(let ((out (open-output-string)))
  (write '(a "b" #\c) out)
  (write (get-output-string out)))           ; "(a \"b\" #\\c)"
(let ((l (list 1 2 3)))
  (set-cdr! (cddr l) l)
  (write l))                                 ; #0=(1 2 3 . #0#)
(write (read (open-input-string "(1 (2 . 3) #(4))")))  ; (1 (2 . 3) #(4))
```

`write` and `display` label only the structure on a cycle, so they always
terminate. `write-shared` labels everything that appears twice, and
`write-simple` labels nothing.

## Libraries and Turmeric

A `define-library` compiles to a Turmeric module:

```scheme
#lang r7rs
(define-library (mylib)
  (export add)
  (import (scheme base))
  (begin
    (define (add a b) (+ a b))))
```

A Scheme program imports it as `(import (mylib))`, and a Turmeric module as
`(import mylib :refer [add])`. The library's exports are `any` on the Turmeric
side, narrowed with `cast`. In the other direction, a Scheme program reaches
Turmeric through the `(turmeric ...)` head:

```scheme
#lang r7rs
(import (scheme base) (scheme write) (turmeric stdlib/vec))
(define v (vec-new))
(vec-push! v 1)
(display (vec-len v))                        ; 1
```

The import is required: without it `vec-new` is not bound, and the error
names the import to add. `only`, `prefix`, `rename` and `except` work on a
Turmeric module as on any library.

A Turmeric keyword is spelled as a symbol from Scheme. `:k` in Turmeric and
`'k` in Scheme are the same value, so a map keyed by keywords is read with
`(map-get m 'k)`. In a Scheme file `:k` is an ordinary identifier, as R7RS
says, so `':k` is the symbol `:k` and `:::` can be a macro's ellipsis.

**Visible change:** before 2026-09-26, `:k` in a Scheme file was the Turmeric
keyword (and `':k` the symbol `k`).

Each argument crossing into a typed Turmeric function is checked against its
signature. A string crosses into a Turmeric `cstr` as a fresh UTF-8 copy.
Turmeric's strings stay immutable, so mutating the Scheme string afterwards
changes nothing on the Turmeric side. A Turmeric module `cast`ing a Scheme
library's string result to `cstr` gets the same copy.
`tests/run-r7rs-import.sh` pins both directions on both back ends.

## SRFIs

An SRFI is imported by its number, `(import (srfi N))`, the way Racket's
R7RS imports it:

```scheme
(import (scheme base) (scheme write) (srfi 45))
(define (from n) (lazy (eager (cons n (from (+ n 1))))))
(define (nth s n) (if (= n 0) (car (force s)) (nth (cdr (force s)) (- n 1))))
(write (nth (from 0) 1000))                  ; 1000
```

What an import does depends on the SRFI, and the table below says which
kind each one is:

- **built in**: R7RS adopted the SRFI, so its names are R7RS's own. The
  import is accepted and costs nothing; the program compiles to exactly the
  same code without it. Importing it next to `(scheme base)` is fine.
- **alias**: a few new names for R7RS procedures.
- **library**: an implementation, loaded when imported. Only the procedures
  the program (or a library it imports) reaches are compiled, so an import
  whose procedures go unused costs nothing.
- **no library**: the syntax is always on, so there is nothing to import.
  The import is an error that says so, as in Racket.
- **not planned**: refused, with the reason.
- **not yet**: planned, and refused until the stage in parentheses lands
  ([docs/archive/r7rs-srfi-plan.md](https://github.com/turmeric-lang/turmeric/blob/main/docs/archive/r7rs-srfi-plan.md)).

`only`, `except`, `prefix` and `rename` work on an SRFI as on any library,
and their names are checked against its export list. One imported name has
one binding (R7RS 5.2). Where an SRFI's procedure extends R7RS's compatibly
(SRFI 1's `map`, SRFI 13's `string-copy`), the SRFI's name is R7RS's own
procedure, so importing both is no conflict. Where it does not (SRFI 13's
`string-map` and `string-for-each`), or where a rename puts an SRFI's
procedure onto a name R7RS already has, importing it next to `(scheme base)`
is an error that names both libraries. Leave R7RS's out, move it or respell
it -- `(except (scheme base) string-map string-for-each)`, `(rename ...)`,
`(prefix (scheme base) b:)` -- or prefix the SRFI's. Defining a name an SRFI
import binds is an error too; `(except (srfi N) name)` keeps the name for the
program. Two SRFIs that give one name different meanings (SRFI 13's and
SRFI 69's `string-hash`) conflict the same way.

`cond-expand` knows the table too. `srfi-N` holds for every SRFI marked built
in, alias, library or no library, and `(library (srfi N))` holds for the ones
that can be imported. `(features)` lists the same `srfi-N` identifiers.

The Racket column says what Racket's `srfi` collection has for each one.
"re-export" means Racket's module only re-exports its core, and "library"
means it has an implementation of its own. For SRFIs that R7RS adopted,
Racket needs its own library where its core differs from the SRFI; here,
R7RS's own forms already are the SRFI's.

| SRFI | Title | Here | Racket | Notes |
|---|---|---|---|---|
| 0 | Feature-based conditional expansion construct | no library | no module | `cond-expand` is R7RS syntax; `srfi-N` identifiers answer for each SRFI here |
| 1 | List Library | library | library | chibi's implementation. The names it shares with `(scheme base)` and `(scheme cxr)` (`map`, `member`, `assoc`, `list-copy`, ...) are R7RS's own procedures, which have SRFI 1's extensions; the linear-update `!` procedures are the pure ones |
| 2 | AND-LET* | library | library | `and-let*`; a bare clause may be any expression, as in chibi |
| 4 | Homogeneous numeric vector datatypes | library | library | Written for Turmeric. A `u8vector` is a bytevector: its procedures are SRFI 66's, so importing both is not a conflict. The other nine types are disjoint records holding their elements, each checked on the way in, so an element outside the type's range raises an error object; `f32` and `f64` elements are any real, stored inexact, and an `f32` keeps a double's precision. As in Racket, there is no reader syntax: `#s16(1 2)` does not read (`#u8(...)` is R7RS's own), and `equal?` compares two non-u8 vectors by identity |
| 5 | A compatible let form with signatures and rest arguments | not yet (S8) | library |  |
| 6 | Basic String Ports | built in | re-export | `(scheme base)`'s own |
| 7 | Feature-based program configuration language | not yet (S8) | library |  |
| 8 | RECEIVE: Binding to multiple values | library | library | `receive`, the SRFI's own definition |
| 9 | Defining Record Types | built in | library | R7RS `define-record-type` is SRFI 9's |
| 11 | Syntax for receiving multiple values | built in | library | R7RS `let-values` takes dotted rest formals |
| 13 | String Libraries | library | library | Written for Turmeric: each procedure reads its string once, as a vector of characters, and returns fresh mutable strings. `string-map` and `string-for-each` are SRFI 13's (one string and a range), so they conflict with R7RS's: import `(except (scheme base) string-map string-for-each)`. The other names it shares with `(scheme base)` and `(scheme char)` are R7RS's procedures; `string-upcase` and `string-downcase` take SRFI 13's range and keep R7RS's full case mapping (`"stra\xDF;e"` upcases to `"STRASSE"`), while the `!` forms map one character to one. `string-filter` and `string-delete` take the criterion first, as the SRFI says, or the string first, as its drafts did. Imports SRFI 14 for its char sets |
| 14 | Character-set Library | library | library | Written for Turmeric: a set is an inversion list, so the algebra is one merge and membership a binary search. The standard sets cover all of Unicode, as SRFI 14's 2019 CharsetDefs note defines them (`char-set:letter` is the Alphabetic property, `char-set:punctuation` the P* categories, ...), and each is built the first time a program uses it. `char-set:full` is every Unicode scalar value. The linear-update `!` procedures are the pure ones |
| 16 | Syntax for procedures of variable arity | built in | re-export | `(scheme case-lambda)` |
| 17 | Generalized set! | library | library | `(set! (f arg ...) v)` is `((setter f) arg ... v)`. The target is an arm of the lowering's `set!`, on in a unit that imports this SRFI's `set!` under any name, so the export is R7RS's own and the import sits beside `(scheme base)` as one binding. Settable out of the box: `car`, `cdr`, the `c[ad]r` family, `vector-ref`, `string-ref` and `bytevector-u8-ref`. `setter` is keyed on procedure identity, which R7RS 6.1 gives a procedure and this implementation keeps |
| 19 | Time Data Types and Procedures | not yet (S8) | library |  |
| 23 | Error reporting mechanism | built in | re-export | R7RS `error` is SRFI 23's |
| 25 | Multi-dimensional Array Primitives | not yet (S8) | library |  |
| 26 | Notation for Specializing Parameters without Currying | library | library | `cut` and `cute`, the SRFI's reference implementation |
| 27 | Sources of Random Bits | library | library | The reference implementation: Pierre L'Ecuyer's MRG32k3a generator over exact integers, so a range above 2^32 and a unit finer than 2^-32 use bignums. A state from `random-source-state-ref` is a list that `random-source-state-set!` restores. `random-source-randomize!` seeds from `current-jiffy`; `default-random-source` starts from the same state in every run |
| 28 | Basic Format Strings | library | re-export (Racket's `format`) | SRFI 48's `format`, re-exported: one engine, so importing both SRFIs binds `format` once. SRFI 48's directives work here too |
| 29 | Localization | not yet (S8) | library |  |
| 30 | Nested Multi-line Comments | built in | empty module | `#\| \|#` nests; the library is empty, as Racket's is |
| 31 | A special form rec for recursive evaluation | library | library | `rec`, the SRFI's own definition |
| 34 | Exception Handling for Programs | built in | library | R7RS `guard`, `raise` and `with-exception-handler` are SRFI 34's |
| 35 | Conditions | library | library | The reference implementation, over records. An R7RS error object is a condition of types `&error` and `&message`, its message `error-object-message`'s, so `(error? e)` holds for what `error` raises; the reverse does not hold, and `error-object?` is false for an SRFI 35 condition. SRFI 64's `test-error` takes a condition type |
| 38 | External Representation for Data With Shared Structure | alias | library | `write-with-shared-structure` is `write-shared`; `read-with-shared-structure` is `read` |
| 39 | Parameter objects | built in | re-export | the converter runs on the initial value and on each `parameterize` |
| 40 | A Library of Streams | not planned | library | deprecated by its author in favour of SRFI 41 |
| 41 | Streams | library | library | chibi's implementation of the reference, over records and R7RS's `delay-force`, so an iterative stream runs in constant space |
| 42 | Eager Comprehensions | library | library | The reference implementation. The generic generator `:` is a lone colon, which reads as a symbol in `#lang r7rs`. A qualifier the program writes is expanded in the program's scope, so the program imports `(srfi 42)` for what it uses, SRFI 78's `check-ec` included |
| 43 | Vector Library | not yet (S8) | library | its index-first `vector-map` differs from R7RS's |
| 45 | Primitives for Expressing Iterative Lazy Algorithms | alias | library | `lazy` is `delay-force`, `eager` is `make-promise` |
| 48 | Intermediate Format Strings | library | library | The reference implementation. `~Y` pretty-prints with `write`, as the SRFI permits. Where `~w,dF` switches to exponent notation follows `number->string`: `3.2e11` prints in full here, as `320000000000.0` |
| 54 | Formatting | not yet (S8) | library |  |
| 57 | Records | not yet (S8) | library |  |
| 59 | Vicinity | not yet (S8) | library |  |
| 60 | Integers as Bits | library | library | The document's implementation. Integers are two's complement of any width; two fixnums take Turmeric's bit operations, and a bignum is taken 30 bits at a time. An argument that is not an exact integer raises an error object |
| 61 | A more general cond clause | library | library | `(generator guard => receiver)` clauses in `cond`, on in a file that imports it; the export is R7RS's own `cond`, so it sits beside `(scheme base)` |
| 62 | S-expression comments | no library | no module | `#;` is always on; the import is an error that says so, as in Racket |
| 63 | Homogeneous and Heterogeneous Arrays | not yet (S8) | library |  |
| 64 | A Scheme API for test suites | library | library | Taylan Kammer's R7RS implementation. The default runner prints to the current output port (no log file), and after a failure or an unexpected pass its outermost `test-end` exits with status 1, so `tur test` fails the file; `tur init --r7rs` scaffolds a test in it. `test-error` takes `#t`, a predicate or an SRFI 35 condition type (importing SRFI 64 imports SRFI 35 too, for this). `test-read-eval-string` is syntax, so only a program that uses it needs `(scheme eval)`. Failures print the form, with no file or line |
| 66 | Octet Vectors | alias | library | An octet vector is a bytevector, and SRFI 66's names are R7RS's procedures, checked: an element that is not an octet or an argument that is not a bytevector raises an error object naming the SRFI 66 procedure. `u8vector-copy!` takes R6RS's argument order (`source source-start target target-start n`), not `bytevector-copy!`'s |
| 67 | Compare Procedures | not yet (S8) | library |  |
| 69 | Basic hash tables | library | library | Written for Turmeric. `hash` agrees with `equal?` and `hash-by-identity` with `eq?`/`eqv?`. A table made without a hash function works for all five standard equivalences (`eq?`, `eqv?`, `equal?`, `string=?`, `string-ci=?`); for any other equivalence, pass the hash function that agrees with it |
| 71 | Extended LET-syntax for multiple values | not yet (S8) | library |  |
| 74 | Octet-Addressed Binary Blocks | not yet (S8) | library |  |
| 78 | Lightweight testing | library | library | The reference implementation. `check-ec` takes SRFI 42's qualifiers, so a program that uses it imports `(srfi 42)` as well. Nothing ends the program: to fail `tur test`, end with an `exit` on `check-passed?` |
| 86 | MU and NU simulating VALUES and CALL-WITH-VALUES | not yet (S8) | library |  |
| 87 | => in case clauses | built in | library | R7RS `case` takes `=>` |
| 98 | An interface to access environment variables | built in | library | re-exports `(scheme process-context)`'s two procedures |
| 105 | Curly-infix-expressions | no library | no module | `{a + b}` reads in every `#lang` |
| 216 | SICP Prerequisites (Portable) | library | no module | `true`, `false`, `nil`, `runtime` (microseconds), `random` (over SRFI 27), `cons-stream`, `the-empty-stream`, `stream-null?`. Until SRFI 18 lands, `parallel-execute` runs its thunks one after another (a schedule SICP 3.4 allows, never an interleaved one). See [Working through SICP](sicp-guide.md) |

Each built-in or alias row is one file, `stdlib/srfi/<N>.scm`, holding a
`(define-library (srfi N) ...)`. `tests/check-r7rs-srfi-sync.sh` checks this
table, those files, and the compiler's own table against each other.

One more library is built in the same way without being an SRFI:
`(sicp extras)` (`stdlib/sicp/extras.scm`) has the names Racket's
`#lang sicp` adds beyond SRFI 216 -- `inc`, `dec`, `identity`, `amb` --
and `amb-reset!`, which starts a new `amb` search from scratch. It is not
an `srfi-N` feature; `cond-expand` sees it as `(library (sicp extras))`.
See [Working through SICP](sicp-guide.md).

## Memory

A compiled program's allocator is a conservative mark-sweep collector
(docs/archive/r7rs-gc-plan.md): every pair, vector, string, record,
procedure, `call/cc` image and runtime record the program makes is reclaimed
once nothing reaches it. A loop that builds a dead four-element list a
million times peaks at 10 MB, and so does a loop that runs a `guard`, an
escaping `call/cc` and a re-entrant one a million times each. Values you keep in Turmeric maps or `rc<T>` cells through the
`(turmeric ...)` seam are seen through the node that holds them. A
collection runs when 8 MiB, or twice the live size, has been allocated since
the last one; `TUR_GC_TORTURE=N` collects every N allocations, for shaking
out a missing root. `TUR_R7RS_GC=0 tur build prog.tur`, or `tur --no-r7rs-gc
build prog.tur`, builds without the collector; the data then stays
allocated until the process exits, the way a Turmeric `:heap` box does.

**Threads** run in parallel under the collector, and their memory is
reclaimed too. A program starts them through the seam: `thread-spawn-fn`,
`session-spawn`, a task group, a future's timeout, or a Turmeric module's
own `pthread_create`. Each thread allocates from its own cache of slots. A
collection stops the other threads by signal wherever they are, the way the
Boehm collector does, so nothing is compiled into the program's loops. Nine
threads on one heap, eight of them taking turns on a shared persistent map
and one churning garbage, is a gate case
(`tests/fixtures/r7rs-threads-stress`).

Each thread has its own dynamic environment: the handlers `guard` and
`with-exception-handler` install, the `dynamic-wind` frames, and the values
`parameterize` binds. A `raise` on one thread never reaches a handler
another thread installed, and a `parameterize` on one thread changes nothing
another thread reads. A new thread starts with no handlers, no wind frames
and each parameter at the value `make-parameter` gave it. A fiber has its
own too, and carries it with it when it resumes on another thread
(`tests/fixtures/r7rs-threads-dynamic-env`,
`tests/fixtures/r7rs-threads-fiber-dynamic-env`).

- The stop signal restarts the system call it interrupts. The runtime knows
  these blocking calls: the joins, the condition waits, `nanosleep`,
  `poll`, `select`, `accept`, `connect`, `recv`, `read`, `waitpid`,
  `sem_wait`, `epoll_wait` and `kevent`. A program's own inline C that
  calls any other blocking function may see EINTR from it, and should
  retry.
- Detached threads leave nothing behind once they are gone.
- After a `fork`, the child can allocate, whatever the other threads were
  doing at that moment.
- A value kept with `pthread_setspecific` is kept alive. That is where
  `^thread-local` globals and a spawned thread's conveyed dynamic bindings
  live.
- A thread the program did not start (a library's own, calling back in)
  stops with the reason at its first allocation.

What it does not cover:

- **Other builds.** `--shared`, a project build (`tur build <dir>`), `tur
  jit` and the interpreter (`tur --interpret`) do not use it; the
  interpreter keeps its values for the life of the process by design.
- **Other platforms.** Linux (glibc) and macOS. Elsewhere the program
  allocates from libc and nothing is collected.
- **Memory libc allocates**, and the backtracking trail's arrays (`stdlib/
  trail`), are not scanned: a Scheme value stored only in a `bt` cell
  through the seam is not seen.

## Where it differs from R7RS

- **String literals are immutable.** R7RS allows this. See Lists,
  vectors, strings above.
- **A file holds one library, named after the file.** `(define-library (two
  a) ...)` lives in `two/a.tur`, the way a Turmeric module's path is its name,
  and a second `define-library` in the same file is an error.
- **`(except ...)` over a user library or a Turmeric module hides nothing.**
  Turmeric's import has no "all but", so the module is imported whole; over
  a `(scheme ...)` library an excluded name is the program's own to define.
- **A program may define a name it imports.** R7RS 5.2 calls
  `(define (square x) ...)` after `(import (scheme base))` an error; here,
  as in chibi, the program's definition shadows the standard one for the
  whole program (earlier uses and `(map square ...)` included), and the
  prelude and every SRFI keep their own. At the REPL it lasts across turns.
  A name from an imported SRFI is the exception: redefining it is an error
  whose message gives the `except` that frees the name. A `define-library`
  may define and export a standard name too, or one the Turmeric stdlib has
  (`None`, `list-length`). An importer that takes the name from the library
  gets the library's, even beside `(scheme base)`. A Turmeric module that
  imports the library sees such an export as `<name>--user`
  (`mylib/square--user`), since the bare name would collide.
- **`apply` takes at most eight arguments**, on both back ends, and so does a
  call through a variable on the compiled back end (`tur --interpret` has no
  such limit). A direct call to a named procedure has no limit. Past eight,
  pass the rest as a list.
- **`char-ready?` and `u8-ready?` on Windows always answer `#t`.** Elsewhere
  they ask the descriptor (a zero-timeout poll), so an idle console, or an
  open pipe with nothing in it, answers `#f`. A port at end of input answers
  `#t`, since a read there does not block.
- **A top-level continuation is the rest of its form, not of the program.**
  Each top-level form runs under its own prompt, as in chibi and Racket, so
  re-entering a continuation from a later form finishes the form that
  captured it and then carries on after the form that invoked it. Inside a
  procedure, re-entry is what R7RS describes. See Control above.
- **Brackets are parentheses.** `(let ([x 1]) x)` reads as it does in
  Racket and Chez. R7RS reserves `[` and `]`; this is the common reading.
- **Turmeric syntax is not Scheme.** A Turmeric form such as `defn`, `fn`,
  `match`, `::` or `->` in a Scheme file is an error that says where Turmeric code
  goes: a Turmeric module, imported with `(import (turmeric <module>))`. At
  the REPL, switch the prompt with `#lang turmeric` (which resets the
  session). A program may still define a procedure of that name for itself.
- **A Scheme file sees only Scheme, and what it imports.** Turmeric's `#`
  literals (`#map{...}`, `#set{...}`, `#rat{...}`, `#cx{...}`, `#?(...)`),
  inline C and `@` are read errors that name the Scheme spelling or the
  import. `true`, `false`, `nil` and `^tailcall` are ordinary identifiers.
  Turmeric's stdlib (`vec-new`, `map-assoc`, `some`, ...) is visible only
  through `(import (turmeric stdlib/<file>))`, under any import set. A
  Turmeric built-in (`println`, `str`, `mod`) is not visible at all: use the
  Scheme procedure, or call it from a Turmeric module. A name nothing binds is
  an error on both back ends. **Visible change (2026-09-27):** all of these
  used to work in a Scheme file with no import.
- **`eval` copies data.** A datum crosses into and out of `eval` as text, so
  evaluated code never shares a pair, vector or string with the program. A
  datum that holds a procedure or a record cannot cross. See Eval above.

## Conformance

chibi-scheme's R7RS test suite (`tests/r7rs/chibi-r7rs-tests.scm`) runs as
the ctest target `tur_r7rs_conformance`, which reports a count rather than a
verdict:

```sh
bash tests/run-r7rs-conformance.sh      # both back ends, about two minutes
python3 tests/r7rs/run-conformance.py --backend interp --list-failures
```

The suite has 1216 tests, and the runner counts **1223 passing
invocations** (a `test-numeric-syntax` form is two), **2 settled and none
failing**, the same on the interpreter and the compiled back end. A settled
test fails on a difference kept on purpose, where R7RS allows both answers
and chibi's test accepts only its own. The two settled tests are the exponent
spelling above (`1.7976931348623157e308` rather than `e+308`). The runner
checks each settled test's reason on every run, and counts it failed if the
reason stops holding; if one starts passing, the run fails so the entry gets
deleted.

The target fails only when the count drops below its floor, so raise the
floor in `tests/run-r7rs-conformance.sh` when the count goes up.

## See also

- [saffron-guide.md](saffron-guide.md) -- the dynamically typed Turmeric
  dialect whose substrate `#lang r7rs` shares.
- [syntax-guide.md](syntax-guide.md) -- the `#lang` line and every base dialect.
- [docs/archive/r7rs-lang-plan.md](https://github.com/turmeric-lang/turmeric/blob/main/docs/archive/r7rs-lang-plan.md)
  -- the plan, stage by stage, with what shipped.
