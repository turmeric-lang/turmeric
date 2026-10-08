---
title: Arrows and Signal Processing
category: Functional Patterns
description: Bare-function arrow combinators and building DSP signal graphs with stdlib/arrow.tur and stdlib/signal/
---

# Arrows and Signal Processing

Turmeric's `stdlib/arrow.tur` provides a small set of **bare-function arrow
combinators** over the `(->)` function arrow. They describe composable
computations that take an input and produce an output -- `arr`, `>>>`,
`arrow-first`, `arrow-second`, `par-comp`, `arrow-split`, `arrow-const`, and
`arrow-dup`.

The most immediate use case in Turmeric is **signal processing**: the
`stdlib/signal/` libraries build on these combinators to create composable
DSP graphs from pure building blocks.

## Two surfaces: bare functions and typeclass dispatch

Turmeric ships the arrow API as **two surfaces in one module**, `stdlib/arrow.tur`:

| Surface | When to reach for it |
|---------|----------------------|
| Bare functions | The simple, default path. You are working concretely with the function arrow `(->)` and do not need to be polymorphic over the arrow constructor. |
| Typeclass dispatch | You want code parametric over *any* `Arrow` instance, resolved through the instance dictionary -- the Haskell-style `Arrow` / `ArrowChoice` / `ArrowLoop` / `ArrowApply` hierarchy. |

The bare layer exposes plain functions: `arr`, `>>>`, `arrow-first`,
`arrow-second`, `par-comp`, `arrow-split`, `arrow-const`, `arrow-dup`. The
typeclass layer declares the classes and instantiates them at `(->)` with the
canonical method names (`arr`, `>>>`, `<<<`, `first`, `second`, `left`,
`right`, `+++`, `|||`, `app`).

Both surfaces live in the **same module** and share the names `arr` / `>>>`. A
bare call dispatches to the matching instance when the receiver's type selects
one, and falls back to the bare combinator otherwise -- so a single `(load
"stdlib/arrow.tur")` gives you both. (A free `defn` and a typeclass method of
the same name share the value namespace; see
[`docs/archive/history/typeclass-methods-share-value-namespace-with-defns.md`](https://github.com/turmeric-lang/turmeric/blob/main/docs/archive/history/typeclass-methods-share-value-namespace-with-defns.md).)

For the function arrow, `arr` lifts a function (the identity up to eta), and
`>>>` is left-to-right composition; both surfaces compute identical values --
see `tests/fixtures/arrow-instance-vs-bare`.

## Function Arrow Basics

```turmeric
(import stdlib/arrow.tur)

;; arr: lift a function (identity for the function Arrow)
(let [add1   (arr (fn [x] (+ x 1)))
      double (arr (fn [x] (* x 2)))]

  ;; >>> : compose left-to-right: add1 first, then double
  (let [pipeline (>>> add1 double)]
    (println (pipeline 5))))   ; => 12
```

```sweet-exp
import stdlib/arrow.tur

let [add1   arr((fn [x] {x + 1}))
     double arr((fn [x] {x * 2}))]

  let [pipeline >>>(add1 double)]
    println pipeline(5)        ; => 12
```

The dataflow reads left-to-right: input enters `add1`, the result flows into
`double`, and the final value exits.

## Arrow Laws

Any correct Arrow instance must satisfy:

```
arr id >>> f        = f                      ; identity
arr (g . f)         = arr f >>> arr g        ; composition
(f >>> g) >>> h     = f >>> (g >>> h)        ; associativity
first (arr f)       = arr (first f)          ; naturality
first (f >>> g)     = first f >>> first g    ; product distribution
```

Turmeric does not enforce these at compile time, but violating them produces
pipelines with unexpected behaviour.

## Product Combinators

`first` and `second` let you route a branch of a `Pair` through an arrow while
passing the other branch unchanged.

```turmeric
(import stdlib/arrow.tur)

(let [add1       (arr (fn [x] (+ x 1)))
      first-add1 (arrow-first add1)    ; apply add1 to fst, pass snd through
      p          (Pair 5 10)]
  (println (first-add1 p)))            ; => Pair(6, 10)
```

```sweet-exp
import stdlib/arrow.tur

let [add1       arr((fn [x] {x + 1}))
     first-add1 arrow-first(add1)
     p          Pair(5 10)]
  println first-add1(p)                ; => Pair(6, 10)
```

`arrow-first` and `arrow-second` are the plain-function helpers exported from
`stdlib/arrow.tur`. The bare layer exports only forward composition (`>>>`);
write `(>>> f g)` with the arguments in the order you want them applied. If you
need reverse composition (`<<<`), reach for the typeclass layer below --
operator mangling gives `>>>` and `<<<` distinct C identifiers
(`_gt_gt_gt` / `_lt_lt_lt`), so they coexist there as method names.

## Additional Combinators

`stdlib/arrow.tur` exports several convenience combinators:

### `par-comp` (parallel composition)

Apply `f` to the first component of a `Pair` and `g` to the second simultaneously.

```turmeric
(let [both (par-comp 0 0 0 0 (fn [x] (+ x 1)) (fn [x] (* x 2)))]
  (both (Pair 3 4)))   ; => Pair(4, 8)
```

```sweet-exp
let [both par-comp(0 0 0 0 (fn [x] {x + 1}) (fn [x] {x * 2}))]
  both Pair(3 4)       ; => Pair(4, 8)
```

Note: the first four `0` arguments are type-level placeholders (the type
variables `a b c d`); they are ignored at runtime.

### `arrow-split` (fanout)

Duplicate a value and apply two functions, collecting results in a `Pair`.

```turmeric
(let [split (arrow-split 0 0 0 (fn [x] (+ x 1)) (fn [x] (* x 2)))]
  (split 3))   ; => Pair(4, 6)
```

```sweet-exp
let [split arrow-split(0 0 0 (fn [x] {x + 1}) (fn [x] {x * 2}))]
  split 3      ; => Pair(4, 6)
```

### `arrow-const` and `arrow-dup`

```turmeric
((arrow-const 42) 999)        ; => 42  (ignores input)
(tuple2-1st (arrow-dup 7))    ; => 7   (Tuple2(7, 7))
```

```sweet-exp
arrow-const(42)(999)          ; => 42
tuple2-1st arrow-dup(7)       ; => 7
```

## Typeclass dispatch (`stdlib/arrow.tur`)

When you want to write code that is parametric over the arrow constructor --
not hard-wired to `(->)` -- use the typeclass layer. It declares the
Haskell-style hierarchy and instantiates it at the function arrow:

| Class | Superclass | Methods | `(->)` instance? |
|-------|------------|---------|------------------|
| `Category` | -- | `ident`, `comp` | yes |
| `Arrow` | `Category` | `arr`, `>>>`, `<<<`, `first`, `second` | yes |
| `ArrowChoice` | `Arrow` | `left`, `right`, `+++`, `\|\|\|` | yes (over `Either`) |
| `ArrowLoop` | `Arrow` | `arrow-loop` | yes (cell-based feedback -- see below) |
| `ArrowApply` | `Arrow` | `app` | yes |
| `ArrowZero` | `Category` | `zero-arrow` | no -- `(->)` has no zero (see Kleisli below) |
| `ArrowPlus` | `ArrowZero` | `plus-arrow` | no -- declared for other arrows |

A constraint on a class entails its superclass, so an `[^ArrowChoice A]`
function may call `comp`, and every instance needs its superclass instance for
the same arrow (`TUR-E0393` otherwise). `ArrowZero` sits over `Category`
rather than `Arrow`, as Haskell has it, because `Kleisli` below is a `Category`
with an honest zero arrow and no `Arrow` instance.

Every call resolves through the instance dictionary rather than a free
function:

```turmeric
(load "stdlib/arrow.tur")

(defn add1 [x : int] : int (+ x 1))
(defn mul2 [x : int] : int (* x 2))

(defn main [] : int
  (let [h (>>> add1 mul2)]      ; >>> dispatches to Arrow [(->)]
    (println (h 3)))            ; => 8
  (let [g (<<< add1 mul2)]      ; reverse composition, only on this surface
    (println (g 3)))            ; => 7  (add1(mul2(3)))
  0)
```

`arr` is eta-expanded in the instance (`(fn [x] (f x))`, not the bare `f`) so
the dispatched result carries a concrete arrow type and is directly callable;
see `docs/archive/history/instance-method-returning-untyped-param-loses-result-type.md`.

### `Category` -- identity and composition

`Category` is the base of the hierarchy: an identity arrow `ident` and forward
composition `comp` (`comp f g` runs `f`, then `g`). `comp` is a distinct method
name from `Arrow`'s `>>>`, so the two coexist without an operator collision.

`ident` is **nullary** -- there is no argument to dispatch on, so it resolves by
return-type / unique-instance dispatch (the mechanism from
[`return-type-dispatch-nullary-arrow-methods-plan`](https://github.com/turmeric-lang/turmeric/blob/main/docs/archive/history/return-type-dispatch-nullary-arrow-methods-plan.md)).
With only the `(->)` instance in scope, a bare `(ident)` resolves uniquely:

```turmeric
(load "stdlib/arrow.tur")

(defn add1 [x : int] : int (+ x 1))

(defn main [] : int
  (let [i (ident)] (println (i 41)))        ; identity arrow      => 41
  (let [h (comp (ident) add1)] (println (h 41)))  ; left identity  => 42
  (let [h (comp add1 (ident))] (println (h 41)))  ; right identity => 42
  0)
```

Once a **second** `Category` instance is in scope (e.g. `Kleisli` below), a bare
`(ident)` is ambiguous -- ascribe it to pick the head: `(:: (ident) :Kleisli)`.

### `ArrowZero` honestly: the `Kleisli` arrow (`stdlib/kleisli.tur`)

The `(->)` arrow has no `ArrowZero` instance because a total function `a -> b`
with no input cannot conjure an inhabitant of `b`. The honest home for
`zero-arrow` is an arrow whose codomain *has* a zero. `stdlib/kleisli.tur`
provides one: `Kleisli A B = A -> Option B`, where `none` is the zero.

```turmeric
(load "stdlib/kleisli.tur")

(defn safe-recip [x : int] : int (if (= x 0) (none) (some (/ 100 x))))
(defn add1m      [x : int] : int (some (+ x 1)))

(defn main [] : int
  ;; Category [Kleisli]: ident = \a -> some a; comp threads Option-bind
  (let [fg (:: (comp (kleisli safe-recip) (kleisli add1m)) :Kleisli)
        r  (k-apply fg 5)]
    (println (unwrap-or r -1)))          ; (100/5)+1 => 21

  ;; none short-circuits composition
  (let [fg (:: (comp (kleisli safe-recip) (kleisli add1m)) :Kleisli)
        r  (k-apply fg 0)]
    (println (some? r)))                 ; safe-recip 0 -> none => false

  ;; ArrowZero [Kleisli]: the honest zero arrow, \_ -> none
  (let [z (:: (zero-arrow) :Kleisli)]
    (println (some? (k-apply z 99))))    ; => false
  0)
```

`Kleisli` is the worked second `Category` instance the hierarchy needs; it is
also the reason `ArrowZero` stays declared-but-uninstantiated at `(->)`.

Ascription-disambiguated arrows (`(:: (ident) :Kleisli)`, etc.) are ordinary
unrestricted values and may be reused freely.

### `ArrowChoice` over `Either`

`ArrowChoice` routes an arrow over one arm of a binary sum. It builds on the
`Either` sum type (`stdlib/either.tur`, from
[`docs/archive/history/sum-types-either-plan.md`](https://github.com/turmeric-lang/turmeric/blob/main/docs/archive/history/sum-types-either-plan.md));
`left` acts on `Left` and passes `Right` through, `+++` fans two arrows over the
two arms, and `|||` collapses both arms to a common result:

```turmeric
(load "stdlib/arrow.tur")

(defn add1 [x : int] : int (+ x 1))
(defn mul2 [x : int] : int (* x 2))

(defn main [] : int
  (let [c (||| add1 mul2)]
    (println (c (Left 10)))     ; add1(10) => 11
    (println (c (Right 7))))    ; mul2(7)  => 14
  0)
```

### `ArrowLoop` -- feedback

`arrow-loop` feeds part of an arrow's output back as input: given an arrow
`a (b, d) (c, d)` it produces `a b c`, wiring the `d` output round to the `d`
input. In Haskell that knot is tied by laziness. Turmeric is strict, so the
`d` slot cannot simply *be* the value the same run is about to produce -- and
the fix is to hand the looped arrow a **`LoopCell`** in slot 1 instead of a
bare value. The cell splits "the value is written" from "the value is read",
which is all laziness was buying:

```
LoopCell  =  { filled : int64, value : int64 }   ; two words on the heap
```

Three combinators fill the cell at three different times. They share one
protocol -- slot 1 is always a `LoopCell` -- so a single looped arrow works
under all three.

| Combinator | The cell starts | Good for | Total? |
|---|---|---|---|
| `arrow-loop` (and its bare twin `arrow-loop-lazy`) | **empty**; filled with the `d` output when the run returns | knot-tying: the arrow parks the cell in its `c` output and something forces it *later* | no -- forcing mid-run is the `<<loop>>` black hole |
| `arrow-loop-fix` | **seeded** with `d0`, refilled once per pass until `d` stops moving or `fuel` runs out | an arrow that reads `d` *strictly*, where the answer is a fixpoint | yes -- `fuel` bounds it |
| `arrow-loop-delay` | **seeded** with `d0`, carried forward across calls | a unit delay in the feedback path: call *n* sees call *n-1*'s `d` | yes |

Reading the cell:

| Function | Meaning |
|---|---|
| `(loop-cell-of slot)` | the erased slot-1 value, typed as a `LoopCell` |
| `(loop-cell-ready? c)` | `true` when the value has been written; `false` inside an `arrow-loop` run |
| `(loop-cell-force c)` | the fed-back value, or a `<<loop>>` panic when it is not there yet |

#### Lazy feedback (knot-tying)

The looped arrow's `c` output is *defined by* the `d` output of the same run --
it parks the cell handle in `c` and the consumer forces it afterwards:

```turmeric
(load "stdlib/arrow.tur")

;; c = the feedback cell itself; d = b * 10, known only at the end of the run.
(defn park-cell [p] : int
  ```c typedef struct { int64_t e1; int64_t e2; } P;
  P *s = (P *)(intptr_t)p;
  P *r = (P *)malloc(sizeof(P));
  r->e1 = s->e2;
  r->e2 = s->e1 * 10;
  return (int64_t)(intptr_t)r; ```)

(defn main [] : int
  (let [lp (arrow-loop park-cell)
        c  (loop-cell-of (lp 7))]
    (println (loop-cell-force c)))   ; => 70
  0)
```

An arrow that forces the cell *during* the run has demanded its own output;
that is a genuine error, and it panics with `<<loop>>` rather than quietly
reading a sentinel. Guard with `loop-cell-ready?` when a miss is expected.

#### Fixpoint feedback

```turmeric
;; newton: d' = (d + b/d)/2, c = d -- iterate to floor(sqrt(b)).
((arrow-loop-fix newton 1 32) 144)    ; => 12
```

`fuel` is a bound, not an assertion: a non-converging arrow spends its passes
and returns the last `c` rather than hanging.

#### Delayed feedback

```turmeric
;; sum-step: d' = c = d + b -- a running total, seeded at 0.
(let [acc (arrow-loop-delay sum-step 0)]
  (acc 1) (acc 2) (acc 3))            ; => 1, 3, 6
```

The returned arrow is stateful: it owns one cell, so each call advances the
same feedback line. Call `arrow-loop-delay` again for an independent one.

#### Writing the looped arrow

The arrow layer runs on an erased two-slot heap pair of `int64`, so a looped
arrow written in Turmeric takes and returns that carrier and ascribes at the
edges:

```turmeric
(load "stdlib/tuple.tur")

(defn prev-step [p : int] : int             ; unit delay: emit the previous b
  (let [t (:: p (Tuple2 int int))
        b (tuple2-1st t)
        d (loop-cell-force (loop-cell-of (tuple2-2nd t)))]
    (:: (tuple2 d b) :int)))
```

An inline-C arrow redeclares the cell layout locally, the same way the pair
helpers in `stdlib/arrow.tur` redeclare `P`:

```c
typedef struct { int64_t filled; int64_t value; } LC;
```

Declaring the arrow directly over `(Tuple2 int int)` works too, and computes
the same values -- the compiler bridges the by-value aggregate to the carrier
at the fat-box boundary:

```turmeric
(defn prev-step [t : (Tuple2 int int)] : (Tuple2 int int)
  (tuple2 (loop-cell-force (loop-cell-of (tuple2-2nd t)))
          (tuple2-1st t)))
```

(That crossing used to return garbage and then segfault; see
`docs/archive/arrow-struct-typed-arrow-abi.md`. `tests/fixtures/arrow-struct-typed-arrow`
now asserts the two spellings agree under every member of the family.)

Fixtures: `arrow-loop-lazy-feedback`, `arrow-loop-fix`, `arrow-loop-delay`,
`arrow-struct-typed-arrow`, and `arrow-instance-loop-nonrecursive` (the
degenerate case, where the arrow ignores `d` entirely).

### Both surfaces agree

The dispatch surface computes the same values as the bare functions for `(->)`
-- `tests/fixtures/arrow-instance-vs-bare` runs the same pipeline both ways and
asserts identical output. The dispatch fixtures
(`arrow-instance-stdlib-basic`, `-choice`, `-loop-nonrecursive`, `-apply`,
`-closure-capture`) lock the typeclass path, including capturing closures
flowing through the instance dictionary.

---

## Signals and Signal Functions

> **Where the Signal/SF library lives:** the worked implementation is the
> `tur-signal` spice in `../turmeric-spices/spices/signal/` (see
> `docs/archive/history/tur-signal-rebuild-plan.md` for the rebuild plan and
> acceptance state). The code samples below are written against
> `stdlib/signal/...` paths; there is no `stdlib/signal/` in this repo, so
> treat them as the conceptual surface -- the same names are exported by
> `signal/core`, `signal/osc`, `signal/filter`, `signal/shaper`,
> `signal/envelope`, and `signal/compose` in the spice.

`stdlib/signal/core.tur` introduces the Signal abstraction:

```
Signal a  =  Time -> a
SF a b    =  Signal a -> Signal b
```

A **Signal** is just a function from time to a value. A **Signal Function** (SF)
transforms one signal into another. Because SF is itself a function `(Signal a ->
Signal b)`, the bare-function arrow combinators apply without any extra
machinery: `arr`, `>>>`, `arrow-first`, and `arrow-second` all work on SFs
out of the box.

### Core signal constructors

```turmeric
(import stdlib/signal/core.tur)

;; constant: ignore time, always return the same value
(let [dc (constant 5.0)]
  (dc 0.0)    ; => 5.0
  (dc 99.0))  ; => 5.0

;; time-signal: identity -- returns the time at each sample
(time-signal 2.5)   ; => 2.5

;; sample: evaluate a signal at a point in time
(sample (constant 3.0) 1.0)   ; => 3.0

;; map-signal: lift a function to pointwise operation over a signal
(let [louder (map-signal (fn [x] (* x 2.0)) (constant 0.5))]
  (louder 0.0))   ; => 1.0
```

```sweet-exp
import stdlib/signal/core.tur

let [dc constant(5.0)]
  dc(0.0)    ; => 5.0
  dc(99.0)   ; => 5.0

time-signal 2.5           ; => 2.5
sample(constant(3.0) 1.0) ; => 3.0

let [louder map-signal((fn [x] {x * 2.0}) constant(0.5))]
  louder(0.0)             ; => 1.0
```

### Combining signals

`pair-signals` zips two signals into a product signal, needed by combinators
like `mix` and `add` that take stereo or multi-channel input:

```turmeric
(let [prod (pair-signals (constant 1.0) (constant 2.0))]
  (prod 0.0))   ; => Pair(1.0, 2.0)
```

```sweet-exp
let [prod pair-signals(constant(1.0) constant(2.0))]
  prod(0.0)     ; => Pair(1.0, 2.0)
```

---

## DSP Primitives

`stdlib/signal/dsp.tur` provides ready-made oscillators, filters, and amplitude
operations, each as a Signal Function.

### Oscillators

```turmeric
(import stdlib/signal/core.tur)
(import stdlib/signal/dsp.tur)

(let [dummy  (constant ())
      ;; sine oscillator: freq Hz, initial phase in radians
      a440   (sine 440.0 0.0)
      ;; square wave: freq, duty cycle [0,1]
      sq     (square 220.0 0.5)
      ;; sawtooth wave: freq
      saw    (sawtooth 110.0)
      ;; triangle wave (stdlib/signal/dsp.tur -- if available)
      tri    (triangle 55.0)]

  ;; An oscillator SF is called with a (usually dummy) input signal
  ;; and returns the output signal.
  ((a440 dummy) 0.001))
```

```sweet-exp
import stdlib/signal/core.tur
import stdlib/signal/dsp.tur

let [dummy  constant(())
     a440   sine(440.0 0.0)
     sq     square(220.0 0.5)
     saw    sawtooth(110.0)
     tri    triangle(55.0)]

  a440(dummy)(0.001)
```

The oscillators ignore their input signal; the `dummy` argument satisfies the
SF interface so they compose uniformly with filters and other SFs.

### Filters

```turmeric
;; low-pass: exponential moving average.
;; alpha in (0,1) -- lower alpha = more smoothing (shorter cutoff)
(let [lp  (low-pass 0.1)
      sig (constant 1.0)
      out (lp sig)]
  (out 0.0)    ; starts at 0 (filter state is zero-initialised)
  (out 0.001))

;; high-pass: subtract the low-passed signal from the input
(let [hp  (high-pass 0.1)
      out (hp sig)]
  (out 0.001))
```

```sweet-exp
let [lp  low-pass(0.1)
     sig constant(1.0)
     out lp(sig)]
  out(0.0)
  out(0.001)

let [hp  high-pass(0.1)
     out hp(sig)]
  out(0.001)
```

### Amplitude operations

```turmeric
(let [sig    ((sine 440.0 0.0) (constant ()))

      ;; gain: scale every sample by a constant
      gained ((gain 0.5) sig)

      ;; mix: weighted blend of a pair signal
      ;; alpha=0 -> all first, alpha=1 -> all second
      blended ((mix 0.3) (pair-signals sig (constant 0.0)))

      ;; add: sample-wise sum of a pair signal
      summed  ((add) (pair-signals sig sig))]
  ...)
```

```sweet-exp
let [sig     sine(440.0 0.0)(constant(()))

     gained  gain(0.5)(sig)
     blended mix(0.3)(pair-signals(sig constant(0.0)))
     summed  add()(pair-signals(sig sig))]
  ...
```

---

## Building a DSP Signal Graph

Because SFs are plain functions, `>>>` wires them into graphs. Read each `>>>`
as "then": input flows left-to-right through each stage.

```turmeric
(import stdlib/signal/core.tur)
(import stdlib/signal/dsp.tur)

(let [dummy (constant ())
      input ((sine 440.0 0.0) dummy)

      ;; Signal graph:
      ;;   sine 440 Hz
      ;;     -> gain 0.5
      ;;     -> low-pass (alpha 0.1)
      ;;     -> DC offset +0.1
      ;;     -> clip [-0.8, 0.8]
      chain (>>> (>>> (>>> (gain 0.5)
                           (low-pass 0.1))
                      (offset 0.1))
                 (clip -0.8 0.8))

      output (chain input)]

  (output 0.0)
  (output 0.001)
  (output 0.002))
```

```sweet-exp
import stdlib/signal/core.tur
import stdlib/signal/dsp.tur

let [dummy  constant(())
     input  sine(440.0 0.0)(dummy)

     chain  >>>(>>>(>>>(gain(0.5) low-pass(0.1))
                        offset(0.1))
                   clip(-0.8 0.8))

     output chain(input)]

  output(0.0)
  output(0.001)
  output(0.002)
```

### Parallel paths with `pair-signals`

Route a signal down two parallel branches and mix the results:

```turmeric
(let [dummy     (constant ())
      raw       ((sine 440.0 0.0) dummy)

      ;; Branch A: full signal
      branch-a  raw

      ;; Branch B: low-passed version
      branch-b  ((low-pass 0.05) raw)

      ;; Mix 50/50
      output    ((mix 0.5) (pair-signals branch-a branch-b))]

  (output 0.001))
```

```sweet-exp
let [dummy    constant(())
     raw      sine(440.0 0.0)(dummy)

     branch-a raw
     branch-b low-pass(0.05)(raw)

     output   mix(0.5)(pair-signals(branch-a branch-b))]

  output(0.001)
```

### Multi-oscillator mix

```turmeric
(let [dummy  (constant ())
      a440   ((sine 440.0 0.0) dummy)   ; root
      e660   ((sine 660.0 0.0) dummy)   ; perfect fifth
      summed ((add) (pair-signals a440 e660))
      out    ((gain 0.5) summed)]       ; normalise

  (out 0.001))
```

```sweet-exp
let [dummy  constant(())
     a440   sine(440.0 0.0)(dummy)
     e660   sine(660.0 0.0)(dummy)
     summed add()(pair-signals(a440 e660))
     out    gain(0.5)(summed)]

  out(0.001)
```

---

## Examples

Runnable signal-processing examples ship with the `tur-signal` spice under
`../turmeric-spices/spices/signal/examples/` -- short, per-module programs
covering signal construction, oscillators, filters and shapers, envelopes,
and a simple voice. Clone the `turmeric-spices` repository next to this one
to run them.

## Quick Reference

```
stdlib/arrow.tur          -- arr, >>>, arrow-first, arrow-second,
                             par-comp, arrow-split, arrow-const, arrow-dup
stdlib/kleisli.tur        -- Kleisli, kleisli, k-apply
signal/core (spice)       -- constant, time-signal, sample, map-signal,
                             pair-signals
signal/osc (spice)        -- sine, square, sawtooth, triangle
signal/filter (spice)     -- low-pass, high-pass
signal/shaper (spice)     -- gain, mix, add, offset, clip, invert
```

## See Also

- **[polymorphism-guide.md](polymorphism-guide.md)** -- overview of all polymorphism mechanisms; the typeclasses section covers `Category`, `Arrow`, and `Kleisli` in context with the broader stdlib
- **[typeclass-internals-guide.md](typeclass-internals-guide.md)** -- how `definstance` lowers to a C dictionary, the closure-handle convention, and `definstance` idempotence
- **[hkt-guide.md](hkt-guide.md)** -- higher-kinded types; the `^arr` kind used by the Arrow typeclass
- **[effects-vs-monads.md](effects-vs-monads.md)** -- why sequencing usually goes through effect handlers rather than a monad transformer stack
- **[generators-guide.md](generators-guide.md)** -- lazy sequences; a complementary pull-based abstraction
