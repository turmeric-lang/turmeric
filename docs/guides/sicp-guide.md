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

**Which to use.** Start with `tur --interpret`. It runs a file at once.
Switch to `tur run` when a
program is slow, for example the timing exercises in 1.2.6. The first
`tur run` after installing takes several seconds while Turmeric builds its
runtime once; later runs take about a second to compile.

**The REPL** is the closest thing to the book's transcripts. Type
`(define (square x) (* x x))`, then `(square 21)`, and the value is echoed
as `=> 441`. A definition echoes nothing. Definitions and `define-syntax`
macros last for the whole session. `:quit` leaves.

## The top of every file

A Scheme file starts with an `import` line naming the libraries it uses. For
SICP, this is the whole line:

```scheme
(import (scheme base) (scheme write) (srfi 216))
```

- `(scheme base)` -- the core: `define`, `if`, `cond`, lists, numbers.
- `(scheme write)` -- `display`, `write`, `newline`.
- `(srfi 216)` -- the definitions the book assumes (next section).

A file with **no** import line also works. It gets the standard procedures,
so a pasted snippet like `(define (square x) (* x x)) (display (square 5))`
runs as is. Write the line anyway once you use any of the book's assumed
names below.

## Definitions the book assumes

SICP uses a few names that MIT Scheme provides and standard Scheme does not.
[SRFI 216](https://srfi.schemers.org/srfi-216/srfi-216.html), "SICP
Prerequisites", is the standard library of exactly those, and
`(import (srfi 216))` brings them in:

| Name | What it is |
|---|---|
| `true`, `false` | `#t` and `#f` |
| `nil` | the empty list, `'()` |
| `runtime` | the time in microseconds, as an exact integer (1.2.6) |
| `random` | `(random 10)` is an integer from 0 to 9; `(random 1.0)` is a float below 1.0 |
| `cons-stream` | 3.5's stream constructor: syntax, so its second part is not evaluated until forced |
| `the-empty-stream`, `stream-null?` | the empty stream and its test |
| `parallel-execute`, `test-and-set!` | 3.4's concurrency primitives; see Chapter 3 below |

`stream-car` and `stream-cdr` are not in it: the book defines those itself
in 3.5.1, and you will type them in when you get there.

If you define one of these names yourself while importing `(srfi 216)`, the
program is refused with a message saying so. That happens if you paste in
an older copy of this guide's definitions, for example. Delete your
definition, or keep it and import `(except (srfi 216) nil)`, naming
whichever ones you define.

### Extras

Some exercises use `inc`, `dec` and `identity` as if they already existed,
and 4.3 is easier to explore with a real `amb` (more on that in Chapter 4
below). Racket's `#lang sicp` provides these, so course materials often
assume them. They are not part of SRFI 216; they are in a library of their
own. Add it to your import line if you need them:

```scheme
(import (scheme base) (scheme write) (srfi 216) (sicp extras))
```

It gives you `inc`, `dec`, `identity`, `amb`, and `amb-reset!` (see
Chapter 4). As with `(srfi 216)`, defining one of these names yourself is
refused; import `(except (sicp extras) inc)`, naming whichever ones you
define.

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
  Both back ends run a recursive process a million calls deep. A compiled
  program (`tur run`) goes much further: 1.2.1's linear-recursive sum
  reaches twenty million calls. Past its limit a compiled program stops
  with `stack overflow: recursion too deep`.
  Iterative processes (tail calls) run in
  constant space on both, as the book says they should: a ten-million-step
  iterative loop, or `even?`/`odd?` calling each other a million times, is
  fine.
- **Timing (exercises 1.22-1.24).** `runtime` from SRFI 216 returns
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

- **Streams (3.5)** work with `cons-stream` from SRFI 216. `stream-map` with
  several streams (the version using `apply`), `integers`, `fibs` and the
  sieve of Eratosthenes give the book's answers.
- **Concurrency (3.4).** The section's code runs: `parallel-execute`,
  `test-and-set!`, and the serializers and mutexes the book builds from
  them. `parallel-execute` runs each procedure on a thread of its own, so
  the interleavings the section is about really happen: run the book's
  unserialized `(set! x (* x x))` / `(set! x (+ x 1))` pair a few hundred
  times and you will see the lost updates (11 and 100) next to 101 and
  121. Serialize the two and only 101 and 121 come up. A program that
  calls `parallel-execute` gets a different interleaving each run; that is
  the point. Under `tur --interpret` the threads take turns, switching
  only when one waits, sleeps or spins on `test-and-set!`, so code with
  none of those -- like that unserialized pair -- runs one procedure to
  the end before the next starts. Use `tur run` to watch interleavings.
  If you want threads directly, `(import (srfi 18))` has them.

### Chapter 4 -- the metacircular evaluator

This is where SICP has you write a Scheme interpreter in Scheme. It defines
its own `eval` and `apply` after saving the real one:

```scheme
(define apply-in-underlying-scheme apply)
(define (apply procedure arguments) ...)   ; the evaluator's own apply
```

This works as printed, on both back ends: `apply-in-underlying-scheme`
keeps the standard `apply`, and the evaluator's own `apply` and `eval`
replace them from their definitions on. The book's whole 4.1.1-4.1.4
evaluator, driver loop included, runs as a test in
[sicp-corpus](https://github.com/turmeric-lang/sicp-corpus). Defining a
procedure again, as the book does throughout (`make-rat` in 2.1, `length`
in 2.2), replaces the old definition, as in the book.

**Trying 4.3's puzzles before building the `amb` evaluator.** With
`(sicp extras)` imported, `amb` works directly in Scheme, so you can run the
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
  `(srfi 216)` is missing from the import line. For `inc`, `dec`,
  `identity` or `amb`, it is `(sicp extras)`.
- **`error: Unknown operation -- TABLE frob`**: that is the book's own
  `(error "Unknown operation -- TABLE" m)` working as intended. The message
  is printed, then the objects, and the program stops.
- **`stack overflow: recursion too deep`** under `tur run`: a recursive
  process went tens of millions of calls deep (see Chapter 1). Make it
  iterative, as 1.2.1 shows. If it really needs to be that deep,
  `TUR_MAIN_STACK_MB=4096 tur run file.scm` gives it a 4 GiB stack instead of
  1 GiB; a smaller number makes a missing base case stop sooner and use
  less memory on the way.
- **Turmeric's own syntax in examples elsewhere** (square brackets in
  `defn`, `:int` types) is the main Turmeric language, not Scheme. In a
  `.scm` file you are always writing plain Scheme.

## Rough edges

None known today. When a defect turns up, it gets a report and an entry
here, which goes away when the fix lands.

## What is coming

[sicp-corpus](https://github.com/turmeric-lang/sicp-corpus) runs the
book's code against Turmeric every day, so breakage is caught before you
hit it. See the
[plan](https://github.com/turmeric-lang/turmeric/blob/main/docs/archive/r7rs-srfi-18-216-sicp-plan.md).

## See also

- [R7RS Scheme](r7rs-guide.md) -- the full reference for `#lang r7rs`:
  every library, the REPL, and where Turmeric's Scheme differs from the
  standard.
