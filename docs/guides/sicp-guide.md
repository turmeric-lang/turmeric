---
title: "Working through SICP with Turmeric"
category: Tutorials
description: "Run the code from Structure and Interpretation of Computer Programs in Turmeric's Scheme dialect: installing, the interpreter and the REPL, the few definitions the book assumes, chapter-by-chapter notes, and the rough edges to know about."
---

# Working through SICP with Turmeric

This guide is for you if you are reading *Structure and Interpretation of
Computer Programs* (SICP), maybe for a class, and want somewhere to run the
book's code. You need a little Scheme and no Turmeric at all.

Turmeric is a systems language from the Lisp family. It ships a
**Scheme dialect**, R7RS-small, the current standard Scheme. Scheme code runs
in Turmeric's interpreter, which gives you instant feedback, or is compiled to
a native program when you want speed. SICP was written for MIT Scheme, a
dialect a little older than R7RS, so you add a handful of definitions (below)
and then the book's code runs as printed.

## Getting set up

- **In the browser, nothing to install:** open
  [Try Turmeric](https://turmeric-lang.com/try) and pick **Scheme**
  (`#lang r7rs`) from the language picker.
- **On your machine:** follow [turmeric-lang.com/install](https://turmeric-lang.com/install).
  You get one command, `tur`.

Save your code in a file ending in `.scm`. Then pick a way to run it:

```sh
tur --interpret ex1.scm   # the interpreter: starts instantly; use this by default
tur run ex1.scm           # compile to a native program, then run it
tur repl --lang r7rs      # an interactive prompt
```

**Which to use.** Start with `tur --interpret`. It runs a file at once and
handles very deep recursion (see Chapter 1 below). Switch to `tur run` when a
program is slow, for example the timing exercises in 1.2.6. The first
`tur run` after installing takes several seconds while Turmeric builds its
runtime once; later runs take about a second to compile.

**The REPL** is the closest thing to the book's transcripts. Type
`(define (square x) (* x x))`, then `(square 21)`, and the value is echoed
as `=> 441`. A definition echoes nothing. Definitions and `define-syntax`
macros last for the whole session. `:quit` leaves.

## The top of every file

A Scheme file starts with an `import` line naming the standard libraries it
uses. For SICP, this covers nearly everything:

```scheme
(import (scheme base) (scheme write) (scheme time) (srfi 27))
```

- `(scheme base)` -- the core: `define`, `if`, `cond`, lists, numbers.
- `(scheme write)` -- `display`, `write`, `newline`.
- `(scheme time)` -- clocks, for `runtime` below.
- `(srfi 27)` -- random numbers, for `random` below.

A file with **no** import line also works. It gets the standard procedures,
so a pasted snippet like `(define (square x) (* x x)) (display (square 5))`
runs as is. Write the line anyway once you start using `runtime`, `random`
or streams.

## Definitions the book assumes

SICP uses a few names that MIT Scheme provides and standard Scheme does not:
`true`, `false`, `nil`, `runtime`, `random`, and the stream syntax
`cons-stream`. Paste this block under your import line once, and the
book's code that uses them runs unchanged:

```scheme
;; --- SICP prelude ---
(define true #t)
(define false #f)
(define nil '())
(define (runtime)                      ; microseconds, as an exact integer
  (round (/ (* (current-jiffy) 1000000) (jiffies-per-second))))
(define (random n)                     ; (random 10) => 0..9, (random 1.0) => a float
  (if (exact-integer? n)
      (random-integer n)
      (* n (random-real))))
(define-syntax cons-stream             ; 3.5: must be syntax, not a procedure,
  (syntax-rules ()                     ; so the second part is not evaluated yet
    ((_ a b) (cons a (delay b)))))
(define the-empty-stream '())
(define (stream-null? s) (null? s))
;; --- end of SICP prelude ---
```

Leave `stream-car` and `stream-cdr` out: the book defines those itself in
3.5.1, and you will type them in when you get there.

### Extras

Some exercises use `inc`, `dec` and `identity` as if they already existed,
and 4.3 is easier to explore with a real `amb` (more on that in Chapter 4
below). Racket's `#lang sicp` provides these, so course materials often
assume them. Paste this block too if you need them:

```scheme
;; --- SICP extras ---
(define (inc x) (+ x 1))
(define (dec x) (- x 1))
(define (identity x) x)
(define (amb-reset!)                   ; start a new search from scratch
  (set! amb-fail (lambda () (error "amb tree exhausted"))))
(define amb-fail #f)
(amb-reset!)
(define-syntax amb
  (syntax-rules ()
    ((_ alt ...)
     (let ((prev-fail amb-fail))
       (call/cc
        (lambda (sk)
          (call/cc
           (lambda (fk)
             (set! amb-fail
                   (lambda ()
                     (set! amb-fail prev-fail)
                     (fk 'fail)))
             (sk alt)))
          ...
          (prev-fail)))))))
;; --- end of SICP extras ---
```

## Chapter by chapter

### Chapter 1 -- procedures and processes

The chapter's code runs as printed. Square roots by Newton's method,
`count-change`, `fast-expt`, `gcd`, the prime tests and the higher-order
procedures (`sum`, `fixed-point`, `deriv`) all give the book's answers.
Some things that are worth knowing:

- **Exact numbers.** Integers never overflow: `(fast-expt 2 100)` prints all
  31 digits. Dividing integers gives an exact fraction, as in the book:
  `(/ 6 4)` is `3/2`. Use a decimal point (`1.0`) when you want a float.
- **Deep recursion.** 1.2.1 contrasts recursive and iterative processes.
  The interpreter happily runs a recursive process a million calls deep. A
  compiled program (`tur run`) handles a hundred thousand but not a
  million. See "Rough edges" below. Iterative processes (tail calls) run in
  constant space on both, as the book says they should: a ten-million-step
  iterative loop, or `even?`/`odd?` calling each other a million times, is
  fine.
- **Timing (exercises 1.22-1.24).** `runtime` from the prelude returns
  microseconds. Modern machines are fast, and a compiled program faster
  still, so you will need much larger primes than the book suggests before
  the times are measurable.

### Chapter 2 -- data

Rational numbers, `accumulate`/`flatmap`, eight queens and symbolic
differentiation run as printed.

- **`put` and `get`** (2.4.3 onward) are not built in. The book assumes
  they exist and only shows how to build them in 3.3.3. Until then, paste
  this, which is the 3.3.3 table:

  ```scheme
  (define (make-table)
    (let ((local-table (list '*table*)))
      (define (lookup key-1 key-2)
        (let ((subtable (assoc key-1 (cdr local-table))))
          (if subtable
              (let ((record (assoc key-2 (cdr subtable))))
                (if record (cdr record) false))
              false)))
      (define (insert! key-1 key-2 value)
        (let ((subtable (assoc key-1 (cdr local-table))))
          (if subtable
              (let ((record (assoc key-2 (cdr subtable))))
                (if record
                    (set-cdr! record value)
                    (set-cdr! subtable
                              (cons (cons key-2 value) (cdr subtable)))))
              (set-cdr! local-table
                        (cons (list key-1 (cons key-2 value))
                              (cdr local-table)))))
        'ok)
      (define (dispatch m)
        (cond ((eq? m 'lookup-proc) lookup)
              ((eq? m 'insert-proc!) insert!)
              (else (error "Unknown operation -- TABLE" m))))
      dispatch))
  (define operation-table (make-table))
  (define get (operation-table 'lookup-proc))
  (define put (operation-table 'insert-proc!))
  ```

- **The picture language** (2.2.4: `beside`, `below`, `wave`) is not
  available. Do those exercises on paper, or in Racket's `#lang sicp`.

### Chapter 3 -- state

`set!`, bank accounts, `set-car!`/`set-cdr!`, queues and tables run as
printed. The larger programs built from them (the digital circuit
simulator, constraint propagation) use nothing else, but are not yet part of
Turmeric's test suite.

- **Streams (3.5)** work with the prelude's `cons-stream`. `stream-map` with
  several streams (the version using `apply`), `integers`, `fibs` and the
  sieve of Eratosthenes give the book's answers.
- **Concurrency (3.4).** `parallel-execute` and `test-and-set!` are not
  available yet. Read 3.4 for the ideas; its code will not run.

### Chapter 4 -- the metacircular evaluator

This is where SICP has you write a Scheme interpreter in Scheme. It defines
its own `eval` and `apply` after saving the real one:

```scheme
(define apply-in-underlying-scheme apply)
(define (apply procedure arguments) ...)   ; the evaluator's own apply
```

**This does not work in Turmeric today** (see "Rough edges"). Compiled, the
program loops forever; interpreted, it stops with `unbound variable:
apply--user`. The fix is a rename: call the evaluator's procedures `mc-eval`
and `mc-apply`, and change every call to them. Keep
`(define apply-in-underlying-scheme apply)` as it is; only the
definitions of the book's `eval` and `apply` are renamed:

```scheme
(define apply-in-underlying-scheme apply)

(define (mc-eval exp env)
  (cond ((self-evaluating? exp) exp)
        ...
        ((application? exp)
         (mc-apply (mc-eval (operator exp) env)
                   (list-of-values (operands exp) env)))
        (else (error "Unknown expression type -- EVAL" exp))))

(define (mc-apply procedure arguments)
  (cond ((primitive-procedure? procedure)
         (apply-primitive-procedure procedure arguments))
        ...))
```

With that change the evaluator works on both back ends (a trimmed version
computes `(fact 20)` through it). The book's later evaluators (the
analyzing evaluator in 4.1.7, the lazy one in 4.2, the `amb` evaluator in
4.3) need the same rename.

**Trying 4.3's puzzles before building the `amb` evaluator.** With the
extras block loaded, `amb` works directly in Scheme, so you can run the
section's examples (`require`, `an-integer-between`, the
multiple-dwelling puzzle) as ordinary programs and check your answers to
the exercises against them. Each `(amb)` with no choices left backtracks to
the most recent choice point; when there are none left at all, you get
`error: amb tree exhausted`. One thing differs from the book's evaluator,
which starts every new problem fresh: here the choice points of the last
search are still live. Call `(amb-reset!)` before starting a new search,
or exhausting it will jump back into the previous one.

This is a shortcut for experimenting, not a replacement for 4.3.3. Building
the evaluator is the point of the section.

### Chapter 5 -- register machines

The register-machine simulator and the explicit-control evaluator use only
lists, `set-cdr!`, `assoc` and tables, all of which work. They are big
programs and are not yet part of Turmeric's test suite, so if one misbehaves,
it is worth reporting.

## When something goes wrong

- **`unbound variable: nil`**, or `true`, `runtime`, `cons-stream`:
  the SICP prelude is missing. Paste it under the import line. For `inc`,
  `dec`, `identity` or `amb`, it is the extras block.
- **`error: Unknown operation -- TABLE frob`**: that is the book's own
  `(error "Unknown operation -- TABLE" m)` working as intended. The message
  is printed, then the objects, and the program stops.
- **A program prints nothing and exits** under `tur run`: probably a very
  deep recursion (see Chapter 1). Run it with `tur --interpret`.
- **Turmeric's own syntax in examples elsewhere** (square brackets in
  `defn`, `:int` types) is the main Turmeric language, not Scheme. In a
  `.scm` file you are always writing plain Scheme.

## Rough edges

These are known defects, each with an open report. When one is fixed, its
entry here goes away.

- **Saving a standard procedure, then redefining it** (the start of 4.1)
  gives the variable the new definition, or fails with `unbound variable:
  apply--user`. Rename instead, as shown in Chapter 4.
  [Report](https://github.com/turmeric-lang/turmeric/blob/main/docs/reported/r7rs-saved-standard-procedure-follows-redefinition.md).
- **Defining `eval` while importing `(scheme eval)`** fails to compile
  under `tur run` with a C compiler error. Leave `(scheme eval)` out of the
  import line (SICP does not need it).
  [Report](https://github.com/turmeric-lang/turmeric/blob/main/docs/reported/r7rs-redefining-eval-with-scheme-eval-fails-to-compile.md).
- **`apply` with more than eight arguments** stops with `apply: more than 8
  arguments is not supported`, even for `+` or `append`. For a sum or a
  maximum over a long list, use `accumulate` (2.2.3) instead of `(apply +
  lst)`.
  [Report](https://github.com/turmeric-lang/turmeric/blob/main/docs/reported/r7rs-apply-variadic-over-eight-arguments.md).
- **A file name starting with a digit** (`1.1.scm`, `3.5-streams.scm`)
  fails under `tur run` with C compiler errors once the file imports a
  library of your own. Start file names with a letter: `sec-1.1.scm`.
  `tur --interpret` is not affected.
  [Report](https://github.com/turmeric-lang/turmeric/blob/main/docs/reported/r7rs-program-file-named-with-leading-digit-fails-to-compile.md).
- **Very deep recursion in a compiled program** (around a million calls)
  ends the program with no message. Use `tur --interpret`, which handles it.
  [Report](https://github.com/turmeric-lang/turmeric/blob/main/docs/reported/r7rs-deep-recursion-segfaults-silently.md).

## What is coming

Built-in support is planned: `(import (srfi 216))` will replace the SICP
prelude above, including `parallel-execute` and `test-and-set!` for 3.4,
and a built-in library will replace the extras block. A collection of the
book's code, run regularly against Turmeric, will catch breakage before you
do. See the
[plan](https://github.com/turmeric-lang/turmeric/blob/main/docs/upcoming/r7rs-srfi-18-216-sicp-plan.md).

## See also

- [R7RS Scheme](r7rs-guide.md) -- the full reference for `#lang r7rs`:
  every library, the REPL, and where Turmeric's Scheme differs from the
  standard.
