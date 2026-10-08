---
title: Generators and Lazy Sequences Guide
category: Functional Patterns
description: Zero-overhead generators with `gen`/`yield`, lazy `Seq` combinators, and the `Range` type
---

# Generators and Lazy Sequences Guide

Turmeric provides two related features for lazy, incremental computation:

- **Generators** -- resumable functions using `gen`/`yield`, compiled to C state
  machines with no heap allocation per yield
- **Lazy sequences** (`Seq`) -- composable combinators built on generators, with
  no intermediate allocation when chained
- **Range types** -- first-class continuous intervals convertible to sequences

## Generators

### The `gen` form

A generator function uses `gen` in place of a function body and `yield` to
produce values one at a time. The compiler rewrites each generator into a C
struct plus a `_next` dispatch function -- without continuations or per-step
heap allocation.

```turmeric
(defn integers-from [start : int] : (Generator :int)
  (gen []
    (let [^mut i start]
      (while true
        (yield i)
        (set! i (+ i 1))))))

(defn range-gen [lo : int hi : int] : (Generator :int)
  (gen []
    (let [^mut i lo]
      (while (< i hi)
        (yield i)
        (set! i (+ i 1))))))
```

```sweet-exp
defn integers-from [start :int] : (Generator :int)
  gen []
    let [^mut i start]
      while true
        yield i
        set! i {i + 1}

defn range-gen [lo :int hi :int] : (Generator :int)
  gen []
    let [^mut i lo]
      while {i < hi}
        yield i
        set! i {i + 1}
```

### Consuming a generator

Use `gen-next` to advance the generator. It returns a `ptr<void>` -- non-NULL
when a value was yielded, NULL when exhausted. Use the `gen.tur` helpers:

```turmeric
(load "stdlib/gen.tur")

(let [g (range-gen 0 5)]
  (while (gen-some? (let [v (gen-next g)]
                      (when (gen-some? v)
                        (println (gen-unwrap v)))
                      v))))
```

```turmeric
;; gen-for-each macro -- preferred for side effects
(gen-for-each (range-gen 0 5)
              (fn [x] (println x)))
; prints 0 1 2 3 4
```

```sweet-exp
gen-for-each(range-gen(0 5) (fn [x] println(x)))
```

### `gen-collect` -- materialise to array

```turmeric
(let [arr (gen-collect (range-gen 1 6))]
  (println (gen-arr-len arr))        ; 5
  (println (gen-arr-get arr 2)))     ; 3
```

### `gen-nth` -- nth yielded value

```turmeric
(gen-nth (range-gen 10 20) 3)   ; => 13 (0-indexed)
```

### `yield*` -- re-yield from an inner generator

```turmeric
(defn concat-gen [g1 g2] : (Generator :int)
  (gen []
    (yield* g1)
    (yield* g2)))
```

### Supported control flow in `gen` bodies

| Form | Supported |
|---|---|
| `while` with `yield` inside | Yes |
| `if` / `cond` with `yield` in branches | Yes |
| `let` bindings that span a yield point | Yes (promoted to struct field) |
| `yield` inside `match` arms | No -- see Limitations below |
| Recursive generators | No -- see Limitations below |

### Limitations (1.0)

Two `yield` placements are hard compile errors in 1.0 because the gen/yield
state-machine lowering cannot represent them without the post-1.0 CPS pass:

**`yield` / `yield*` inside a `match` arm (`TUR-E0702`)**

The state machine needs to save and restore match-arm position across a
`yield`. Without full CPS this is not representable; the compiler rejects it:

```turmeric
;; ERROR: TUR-E0702
(defn broken [flag : int] : (Generator :int)
  (gen []
    (match flag
      0 (yield 42)   ;; 'yield' is not supported inside a 'match' arm
      _ (yield 99))))
```

Workaround -- yield before or after the `match`, or use `if`/`cond`:

```turmeric
(defn ok [flag : int] : (Generator :int)
  (gen []
    (let [v (if (= flag 0) 42 99)]
      (yield v))))
```

**Recursive generators (`TUR-E0703`)**

A `gen` body whose enclosing function calls itself is a recursive generator.
Suspending across a recursive call requires CPS; the compiler rejects it:

```turmeric
;; ERROR: TUR-E0703
(defn count-down [n : int] : (Generator :int)
  (gen []
    (yield n)
    (count-down (- n 1))))  ;; recursive call inside gen body
```

Workaround -- unroll the recursion into an explicit loop:

```turmeric
(defn count-down [start : int] : (Generator :int)
  (gen []
    (let [^mut n start]
      (while (>= n 0)
        (yield n)
        (set! n (- n 1))))))
```

Both limitations are tracked in Phase CF5 of
[control-flow-completeness-plan.md](https://github.com/turmeric-lang/turmeric/blob/main/docs/archive/history/control-flow-completeness-plan.md)
and require the post-1.0 CPS pass to lift.

### Generated C

The compiler emits a struct per `gen` body and a `_next` function with a
`switch` on the state tag. Each `yield` saves state and returns. This means:

- Zero dynamic allocation per `next` call
- Zero function-pointer indirection
- Inlineable by the C compiler in performance-critical paths

### Interpreter (turi) support

The tree-walking interpreter (`tur interpret`, `tur repl`, sandbox eval)
runs generators too. Instead of the compiled state machine it executes each
`gen` body on its own coroutine stack (the same fiber primitives that back
effect handlers): `yield` suspends the body back to the caller, and
`gen-next` resumes it until the next `yield` or until the body runs off its
end. `gen-done?` reports exhaustion exactly as the compiled path does -- it
flips to true only after a `gen-next` drives the body past its last `yield`,
so the idiomatic `(while (not (gen-done? g)) ...)` loop terminates
identically under both backends.

Consume generators through the `stdlib/gen.tur` helpers (`gen-some?`,
`gen-unwrap`, `gen-none`, and the `gen-for-each` / `gen-nth` / `yield*`
macros): the interpreter provides native implementations of these.
Hand-rolled inline-C pointer helpers that dereference the `gen-next` result
directly are *user inline-C* and remain a compiled-path-only feature (see the
[eval-api guide](eval-api.md)); use the stdlib helpers instead when you need
a program to run under `turi`.

---

## Lazy Sequences (`Seq`)

The `Seq` type wraps a generator-producing thunk. Combinators chain generators
without building intermediate collections.

```turmeric
(import seq/core     :refer [seq-from-vec seq-of empty-seq])
(import seq/builders :refer [seq/range seq/repeat])
(import seq/transform :refer [seq/map seq/filter seq/take seq/drop])
(import seq/combine  :refer [seq/zip seq/concat])
(import seq/consume  :refer [seq/into-vec seq/foldl seq/for-each seq/first])
```

### Creating sequences

```turmeric
;; From a range [lo, hi)
(seq/range 0 10)

;; From a range with step
(seq/range-step 0 20 2)      ; 0 2 4 6 8 10 12 14 16 18

;; Wrap a vec
(seq-from-vec [1 2 3 4 5])

;; Single-element sequence
(seq-of 42)

;; Infinite repetition
(seq/repeat "hello")

;; Infinite thunk calls
(seq/repeatedly (fn [] (rand-int 100)))

;; x, f(x), f(f(x)), ...
(seq/iterate 1 (fn [x] (* x 2)))    ; 1 2 4 8 16 ...

;; Cycle a finite sequence infinitely
(seq/cycle (seq/range 0 3))          ; 0 1 2 0 1 2 0 1 2 ...
```

```sweet-exp
seq/range(0 10)
seq/range-step(0 20 2)
seq-from-vec([1 2 3 4 5])
seq-of(42)
seq/repeat("hello")
seq/repeatedly((fn [] rand-int(100)))
seq/iterate(1 (fn [x] {x * 2}))
seq/cycle(seq/range(0 3))
```

### Transformations

All transformations are lazy -- no work is done until a consumer drives the
sequence:

```turmeric
;; map
(seq/map (fn [x] (* x x)) (seq/range 1 6))
; lazy: 1 4 9 16 25

;; filter
(seq/filter even? (seq/range 0 10))
; lazy: 0 2 4 6 8

;; take / drop
(seq/take 3 (seq/repeat 7))     ; lazy: 7 7 7
(seq/drop 2 (seq/range 0 5))    ; lazy: 2 3 4

;; take-while / drop-while
(seq/take-while (fn [x] (< x 5)) (seq/range 0 100))
; lazy: 0 1 2 3 4

;; map-indexed: receive (index value) pairs
(seq/map-indexed (fn [i x] (pair i x)) (seq/range 10 13))
; lazy: (0 10) (1 11) (2 12)

;; filter-map: map + filter in one pass (None values dropped)
(seq/filter-map (fn [x] (if (even? x) (some (* x 10)) (none)))
                (seq/range 0 5))
; lazy: 0 20 40

;; flat-map
(seq/flat-map (fn [x] (seq/range 0 x)) (seq/range 1 4))
; lazy: 0   0 1   0 1 2

;; flatten
(seq/flatten (seq-from-vec [(seq/range 0 2) (seq/range 5 7)]))
; lazy: 0 1 5 6
```

```sweet-exp
seq/map((fn [x] {x * x}) seq/range(1 6))
seq/filter(even? seq/range(0 10))
seq/take(3 seq/repeat(7))
seq/drop(2 seq/range(0 5))
seq/take-while((fn [x] {x < 5}) seq/range(0 100))
seq/map-indexed((fn [i x] pair(i x)) seq/range(10 13))
seq/filter-map((fn [x] if(even?(x) some({x * 10}) none())) seq/range(0 5))
seq/flat-map((fn [x] seq/range(0 x)) seq/range(1 4))
seq/flatten(seq-from-vec([seq/range(0 2) seq/range(5 7)]))
```

### Combinators

```turmeric
;; concat two sequences
(seq/concat (seq/range 0 3) (seq/range 10 13))
; lazy: 0 1 2 10 11 12

;; zip two sequences (stops at shorter)
(seq/zip (seq/range 0 3) (seq/range 10 13))
; lazy: (0 10) (1 11) (2 12)

;; zip-with a combining function
(seq/zip-with + (seq/range 0 3) (seq/range 10 13))
; lazy: 10 12 14

;; interleave: alternate elements
(seq/interleave (seq/range 0 3) (seq/range 10 13))
; lazy: 0 10 1 11 2 12
```

### Consumers (force evaluation)

```turmeric
;; collect to vec
(seq/into-vec (seq/range 0 5))          ; => [0 1 2 3 4]

;; collect to cons list
(seq/into-list (seq/range 0 3))         ; => (0 1 2)

;; nth element (0-indexed)
(seq/nth 2 (seq/range 10 20))           ; => (some 12)

;; first element
(seq/first (seq/filter even? (seq/range 1 10)))  ; => (some 2)

;; count elements
(seq/count (seq/range 0 100))           ; => 100

;; foldl: left fold with accumulator
(seq/foldl 0 + (seq/range 1 6))        ; => 15

;; reduce: fold with first element as seed
(seq/reduce + (seq/range 1 6))         ; => (some 15)

;; for-each: side effects
(seq/for-each println (seq/range 0 3)) ; prints 0 1 2

;; short-circuit consumers
(seq/any? even? (seq/range 1 10))      ; => true (stops at 2)
(seq/all? even? (seq/range 0 6 2))    ; => true  (checks 0 2 4)
(seq/find even? (seq/range 1 10))     ; => (some 2)
(seq/find-index even? (seq/range 1 10)) ; => (some 1)
```

### Chaining with `->>` (pipeline style)

```turmeric
(->> (seq/range 0 1000)
     (seq/filter even?)
     (seq/map (fn [x] (* x x)))
     (seq/take-while (fn [x] (< x 10000)))
     (seq/foldl 0 +))
; => 0 + 4 + 16 + 36 + ... + 9604
```

```sweet-exp
->>
  seq/range(0 1000)
  seq/filter(even?)
  seq/map((fn [x] {x * x}))
  seq/take-while((fn [x] {x < 10000}))
  seq/foldl(0 +)
```

No intermediate vecs are allocated; the chain compiles to nested state-machine
dispatch.

---

## `Range` Types

`Range` is a first-class value type representing a continuous interval with
inclusive, exclusive, or unbounded endpoints. It is not itself a sequence, but
integer ranges can be converted to `Seq`.

```turmeric
(import range :refer [closed-range open-range closed-open-range
                      range-contains? range-span seq/from-range])
```

### Constructors

| Function | Interval |
|---|---|
| `(closed-range lo hi)` | `[lo, hi]` -- both inclusive |
| `(open-range lo hi)` | `(lo, hi)` -- both exclusive |
| `(closed-open-range lo hi)` | `[lo, hi)` -- inclusive lower, exclusive upper |
| `(open-closed-range lo hi)` | `(lo, hi]` -- exclusive lower, inclusive upper |
| `(at-least-range lo)` | `[lo, +inf)` |
| `(greater-than-range lo)` | `(lo, +inf)` |
| `(at-most-range hi)` | `(-inf, hi]` |
| `(less-than-range hi)` | `(-inf, hi)` |
| `(singleton-range v)` | `[v, v]` |
| `(unbounded-range)` | `(-inf, +inf)` |

### Predicates

```turmeric
(range-contains? (closed-range 1 10) 5)     ; => true
(range-contains? (open-range   1 10) 10)    ; => false
(range-encloses? (closed-range 0 10) (closed-range 2 8))  ; => true
(range-overlaps? (closed-range 0 5)  (closed-range 3 8))  ; => true
(range-connected? (closed-open-range 0 5) (closed-range 5 10)) ; => true
(empty-range? (open-range 5 5))             ; => true
(singleton-range? (closed-range 7 7))       ; => true
(bounded-range? (at-least-range 0))         ; => false
```

### Set operations

```turmeric
;; Convex hull of two ranges
(range-span (closed-range 0 3) (closed-range 7 10))
; => [0, 10]

;; Gap between non-overlapping ranges
(range-gap (closed-open-range 0 5) (closed-range 7 10))
; => [5, 7)

;; Intersection of overlapping ranges
(range-intersection (closed-range 0 8) (closed-range 5 10))
; => [5, 8]
```

### Converting a Range to a Seq

For discrete (integer) ranges, use `seq/from-range` or `seq/from-range-step`:

```turmeric
;; [1, 5] => (Seq 1 2 3 4 5)
(seq/into-vec (seq/from-range (closed-range 1 5)))
; => [1 2 3 4 5]

;; [0, 10) with step 2 => (Seq 0 2 4 6 8)
(seq/into-vec (seq/from-range-step 2 (closed-open-range 0 10)))
; => [0 2 4 6 8]
```

---

## `stdlib/gen.tur` API Reference

| Symbol | Description |
|---|---|
| `gen-some?` | True if `gen-next` returned a value (non-NULL) |
| `gen-unwrap` | Extract the yielded value from a non-NULL `gen-next` result, at the generator's element type (a yielded `7.25` reads back as a float) |
| `gen-none` | Return NULL to represent no value |
| `gen-arr-new` | Allocate an empty growable array |
| `gen-arr-push!` | Append a value to a gen array |
| `gen-arr-len` | Number of elements in a gen array |
| `gen-arr-get` | Element at index (0-based) |
| `gen-collect` | Macro: drive a generator, collect all values into a gen array |
| `gen-for-each` | Macro: drive a generator, call `f` on each value |
| `gen-nth` | Macro: return the nth value from a generator, or `gen-none` |
| `yield*` | Macro: re-yield every value from an inner generator |

## `stdlib/seq/` Module Reference

| Module | Exported symbols |
|---|---|
| `seq/core` | `seq-iter`, `seq-of`, `empty-seq`, `seq-from-vec`, `seq-from-list` |
| `seq/builders` | `seq/range`, `seq/range-step`, `seq/repeat`, `seq/repeatedly`, `seq/cycle`, `seq/iterate`, `seq/unfold` |
| `seq/transform` | `seq/map`, `seq/filter`, `seq/take`, `seq/drop`, `seq/take-while`, `seq/drop-while`, `seq/map-indexed`, `seq/filter-map`, `seq/flat-map`, `seq/flatten` |
| `seq/combine` | `seq/concat`, `seq/chain`, `seq/zip`, `seq/zip-with`, `seq/interleave` |
| `seq/consume` | `seq/into-vec`, `seq/into-list`, `seq/nth`, `seq/first`, `seq/last`, `seq/count`, `seq/reduce`, `seq/foldl`, `seq/for-each`, `seq/any?`, `seq/all?`, `seq/find`, `seq/find-index` |

## `stdlib/range.tur` API Reference

| Function | Description |
|---|---|
| `closed-range` / `open-range` / `closed-open-range` / `open-closed-range` | Bounded constructors |
| `at-least-range` / `greater-than-range` | Unbounded above |
| `at-most-range` / `less-than-range` | Unbounded below |
| `singleton-range` / `unbounded-range` | Single value / no bounds |
| `range-contains?` | Test membership |
| `range-encloses?` | Test subset |
| `range-overlaps?` | Test nonempty intersection |
| `range-connected?` | Test adjacency (touching counts) |
| `bounded-range?` / `bounded-above?` / `bounded-below?` | Boundedness tests |
| `unbounded-above?` / `unbounded-below?` | Unboundedness tests |
| `empty-range?` / `nonempty-range?` / `singleton-range?` | Cardinality |
| `range-span` | Convex hull of two ranges |
| `range-gap` | Interval between non-overlapping ranges |
| `range-intersection` | Largest range within both |
| `seq/from-range` | Integer range to `Seq` (step 1) |
| `seq/from-range-step` | Integer range to `Seq` with custom step |
