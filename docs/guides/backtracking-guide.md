---
title: Backtracking Guide
category: Advanced Control Flow
description: Nondeterministic backtracking using the list monad in Turmeric's stdlib/backtrack.tur
---

# Turmeric Backtracking Guide

## Overview

Turmeric's backtracking support is implemented as a **list monad** in
`stdlib/backtrack.tur`. Computations that may yield multiple results (or no
result) are represented as linked lists of `int64_t` values. This gives you
classic nondeterministic/backtracking semantics with a familiar monadic API.

---

## Core Operations

| Function | Signature | Description |
|---|---|---|
| `mzero` | `() -> :int` | The empty list -- zero alternatives (failure) |
| `mreturn` | `(x) -> :int` | Wrap a single value -- one alternative (success) |
| `mplus` | `(xs ys) -> :int` | Concatenate two alternative lists |
| `mbind` | `(ma f) -> :int` | Flatmap: apply `f` to each alternative |
| `guard` | `(pred :bool) -> :int` | Keep current branch if `pred` is true, else fail |
| `fresh` | `(f) -> :int` | Apply fat closure `f` to a fresh (unbound) logic variable |
| `once` | `(xs) -> :int` | Truncate to first alternative |
| `interleave` | `(xs ys) -> :int` | Fair interleaving of two streams |
| `run-backtrack` | `(xs) -> :int` | Identity -- return all results |
| `run-backtrack-depth` | `(xs n) -> :int` | Return first N results only |

`fresh` introduces a placeholder logic variable: it calls its (fat) closure
argument with the unbound sentinel (`INT64_MIN`), which is distinct from any
practical logic value. To enumerate a range of integer alternatives, build
the list with `mplus`/`mreturn` (see `ints-upto` below).

---

## Basic Usage

```turmeric
;; All integers in [lo, hi) as alternatives
(defn ints-upto [lo : int hi : int] : int
  (if (>= lo hi)
    (mzero)
    (mplus (mreturn lo) (ints-upto (+ lo 1) hi))))

;; Return all even numbers from 1..10
(let [evens (mbind (ints-upto 1 11)
                   (fn [x]
                     (if (= (mod x 2) 0)
                       (mreturn x)
                       (mzero))))]
  (bt-print (run-backtrack evens)))

; Outputs:
;
; 2
; 4
; 6
; 8
; 10
```

```sweet-exp
;; All integers in [lo, hi) as alternatives
defn ints-upto [lo :int hi :int] :int
  if {lo >= hi}
    mzero()
    mplus(mreturn(lo) ints-upto({lo + 1} hi))

;; Return all even numbers from 1..10
let [evens mbind(ints-upto(1 11)
                 (fn [x]
                   if {mod(x 2) = 0}
                     mreturn(x)
                     mzero)))]
  bt-print(run-backtrack(evens))

; Outputs:
;
; 2
; 4
; 6
; 8
; 10
```

---

## `backtrack-do` Macro

The `backtrack-do` macro provides Haskell-`do`-notation-style sequencing for
the backtracking monad:

```turmeric
;; Pythagorean triples with a+b+c = 24 (ints-upto from above)
(backtrack-do
  a (ints-upto 1 24)
  b (ints-upto a 24)
  c (mreturn (- 24 (+ a b)))
  _ (guard (= (* a a) (+ (* b b) (* c c))))
  (mreturn (list a b c)))
```

```sweet-exp
;; Pythagorean triples with a+b+c = 24 (ints-upto from above)
backtrack-do
  a
  ints-upto(1 24)
  b
  ints-upto(a 24)
  c
  mreturn({24 - {a + b}})
  _
  guard({{a * a} = {{b * b} + {c * c}}})
  mreturn(list(a b c))
```

Each `var expr` line binds `var` to each alternative produced by `expr`. The
final expression is the body for each combination. `_` discards the value when
you only care about side-effects (e.g., `guard`).

---

## Depth Limiting

Use `run-backtrack-depth` when you only need the first N results:

```turmeric
;; Take only the first 5 solutions
(bt-print (run-backtrack-depth all-solutions 5))
```

```sweet-exp
;; Take only the first 5 solutions
bt-print(run-backtrack-depth(all-solutions 5))
```

You can also pass `--backtrack-depth N` to the compiler as a flag, which emits
`#define BACKTRACK_DEPTH_DEFAULT N` in the generated C preamble -- useful for
runtime dispatch when the stdlib is extended to check this constant.

---

## N-Queens Example

```turmeric
;; Count solutions to N-Queens for N=6 using backtrack-do
(defn safe? [col row packed n] : bool
  ;; check if placing at col,row is safe given previous placements in packed
  ...)

(defn queens [n] : int
  (let [result (mreturn 0)]  ;; start with empty board (encoded as 0)
    (let [board (mbind result (fn [packed]
      ;; for each row 0..n-1, expand the board
      ...))]
      board)))
```

```sweet-exp
;; Count solutions to N-Queens for N=6 using backtrack-do
defn safe? [col row packed n] :bool
  ;; check if placing at col,row is safe given previous placements in packed
  ...

defn queens [n] :int
  let [result mreturn(0)]  ;; start with empty board (encoded as 0)
    let [board mbind(result
                     (fn [packed]
                       ;; for each row 0..n-1, expand the board
                       ...))]
      board
```

See `tests/fixtures/backtrack-n-queens/input.tur` for the full self-contained
implementation.

---

## Integration with Algebraic Effects

The backtracking monad can be combined with algebraic effects. Effects run
outside the monad boundary and the results are wrapped with `mreturn`:

```turmeric
(defeffect Choose [a :int b :int] :int)

(defn make-choice [] : int
  (let [x (perform (Choose 10 20))]
    (mreturn (* x 2))))

(defn main []
  (let [result (handle (make-choice)
                 (Choose [a b] k)
                 (resume k a))]   ;; always pick 'a'
    (bt-print result)))
```

```sweet-exp
defeffect Choose [a :int b :int] :int

defn make-choice [] :int
  let [x perform(Choose(10 20))]
    mreturn({x * 2})

defn main []
  let [result (handle (make-choice)
                (Choose [a b] k)
                (resume k a))]   ;; always pick 'a'
    bt-print(result)
```

See `tests/fixtures/backtrack-integration-effects/input.tur` for a complete
working example.

---

## Sudoku Solver Example

The file `tests/fixtures/backtrack-sudoku/input.tur` demonstrates a 4x4
mini-Sudoku solver using iterative backtracking within inline C code. It
produces the unique solution to the given puzzle.

---

## Memory and Performance

- Every alternative is a heap-allocated `Cell { int64_t value; int64_t next; }`.
- Use `run-backtrack-depth` to bound result count and avoid building large lists.
- For performance-sensitive use, consider using the benchmarks in
  `benchmarks/bench-backtrack-n-queens.tur` as a baseline.
- The `--dump-clone-plan` compiler flag prints a summary of every
  `cloneable-shift` site with its captured bindings and resolved `Clone`
  instance methods -- useful for debugging continuation capture overhead.

---

## Compiler Flags

| Flag | Description |
|---|---|
| `--backtrack-depth N` | Emit `#define BACKTRACK_DEPTH_DEFAULT N` in the C preamble |
| `--dump-clone-plan` | After the CPS pass, print each cloneable-shift site to stderr |

---

## Clone Capture Checking

The compile-time capture check (`TUR-E0014`) runs once per `cloneable-reset`,
over the free variables of the reset body (the delimited context the
continuation reifies). A same-function binding needs a `Clone` instance only
if the continuation actually references it -- a non-Clone value that is merely
in lexical scope at a `cloneable-shift` is not flagged. Bindings from
enclosing functions and top-level `def`s are not capture candidates and are
never checked. Some owning types without a `Clone` instance are still
admitted when the codegen can give the captured frame its own per-resume
teardown; a genuinely captured owning value outside that set is rejected.
See `tests/fixtures/cloneable-capture-precision/` for the locked behavior.

---

## See Also

- [Backtrackable State Guide](backtrackable-state-guide.md) -- the other search surface: trailed mutable cells with mark/undo, and `stdlib/backtrack-dfs.tur`'s driver over them. Reach for it when the state is large, when answers must be reified out of live cells, or when you need to keep some of what a failed branch learned; the list monad here stays ahead on small fixed enumeration
- [tur/backtrack API](../html/api/tur-backtrack.html) -- standard library implementation (generated by `tur run docs`)
- [parser-combinators-tutorial.md](parser-combinators-tutorial.md) -- worked example: parser combinators built directly on the list monad described here
- [`tests/fixtures/backtrack-*/`](https://github.com/turmeric-lang/turmeric/tree/main/tests/fixtures/) -- test fixtures for all backtracking features
- [`benchmarks/bench-backtrack-*.tur`](https://github.com/turmeric-lang/turmeric/tree/main/benchmarks/) -- performance benchmarks
