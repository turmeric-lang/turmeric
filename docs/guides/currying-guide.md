---
title: Currying Guide
category: Language Basics
description: Haskell-style partial application and over-application of Turmeric functions
---

# Currying Guide

Turmeric supports Haskell-style currying: every N-argument function is also a valid
`(N-1)`-argument function that returns a closure waiting for the remaining argument.
There is no special syntax -- supply fewer arguments than declared and the elaborator
synthesizes a partial-application closure for you.

---

## Quick reference

| Expression | `f` type | Result type | Behavior |
|---|---|---|---|
| `(f a)` | `(-> A B)` | `B` | fully saturated |
| `(f a)` | `(-> A B C)` | `(-> B C)` | partial application |
| `(f a b)` | `(-> A B C)` | `C` | fully saturated |
| `(f a b c)` | `(-> A B C)` where `C = (-> D E)` | `E` | over-application |
| `((f a) b)` | `(-> A B C)` | `C` | explicit two-step, same as `(f a b)` |

---

## Partial application

Supply fewer arguments than the function's arity; the result is a closure waiting for
the rest.

```turmeric
(defn add [a : int b : int] : int
  (+ a b))

(let [inc (add 1)]
  (inc 41))     ; => 42
```
```sweet-exp
defn add [a : int b : int] : int
  {a + b}

let [inc add(1)]
  inc(41)     ; => 42
```

The same works for built-in operators:

```turmeric
;; Operator section -- (+ 1) is a unary increment.
(map (+ 1) xs)

;; Fold with a partial-applied combiner.
(fold (+ 0) xs)
```
```sweet-exp
;; Operator section -- (+ 1) is a unary increment.
map(+(1) xs)

;; Fold with a partial-applied combiner.
fold(+(0) xs)
```

The partially-applied closure carries the full type of its remaining parameters,
including type annotations on rank-2 polymorphic slots and effect rows. A
partial application of an `#fx{IO}` function is itself `#fx{IO}`.

---

## Over-application

When a function returns another function, you can chain calls inline:

```turmeric
(defn make-adder [n : int] : (fn [int] int)
  (fn [x : int] : int (+ n x)))

(make-adder 10 5)     ; => 15, equivalent to ((make-adder 10) 5)
```
```sweet-exp
defn make-adder [n : int] : (fn [int] int)
  (fn [x : int] : int {n + x})

make-adder(10 5)     ; => 15, equivalent to ((make-adder 10) 5)
```

The elaborator detects that the intermediate result is callable and threads the
remaining arguments into it. Chains of arbitrary depth work the same way.

Over-applying past a non-function return type is a type error:

```turmeric
(add 1 2 3)
; TUR-E0002: function returns int, which is not callable --
;            did you mean to pass all 2 arguments?
```

---

## The `curry` macro

For cases where explicit currying syntax reads better than partial application,
`stdlib/macros.tur` provides:

```turmeric
;;; curry -- convert a 2-argument function to its curried form.
(defmacro curry [f]
  `(fn [a] (fn [b] (~f a b))))
```

Usage:

```turmeric
(defn add [a : int b : int] : int (+ a b))

(let [add1 ((curry add) 1)]
  (add1 41))    ; => 42
```
```sweet-exp
defn add [a : int b : int] : int {a + b}

let [add1 curry(add)(1)]
  add1(41)    ; => 42
```

Most of the time you don't need `curry` -- direct partial application (`(add 1)`)
is shorter and produces the same closure.

---

## How it works

Turmeric uses the **worker/wrapper** model (the same approach as OCaml and MLton),
not GHC-style full currying:

- The compiled function keeps its N-argument C calling convention (the *worker*).
  Fully-saturated calls have zero overhead -- no extra allocation, no extra indirection.
- When a call site provides fewer arguments than the function expects, the elaborator
  synthesizes a fresh anonymous closure (the *wrapper*) that captures the supplied
  arguments and exposes the remaining arity.

So `(add 1)` allocates a small heap closure that remembers `1`, but `(add 1 2)`
compiles straight to a C call with two arguments. The only function type in the
type system is `(-> ...)`; partial-application closures share it with everything else.

---

## Limitations and gotchas

- **Variadic `defn` is not auto-curried.** A function declared with `& rest` does
  not produce a curried entry point. You can under-saturate up to the fixed
  positional params (returning a variadic closure), but you cannot partially
  apply *into* the rest slot. See [Function Arity Style Guide](https://github.com/turmeric-lang/turmeric/blob/main/CLAUDE.md#function-arity-style-guide).

- **No hard arity cap.** There is no fixed limit on positional parameters
  (declaring more than 16 draws the `TUR-W0041` style nudge), and a partial
  application's remaining-parameter list is always a subset of the original
  arity.

- **One heap allocation per partial.** Each under-saturated call site allocates
  one closure. For tight inner loops, hoist the partial application outside the
  loop or call the function fully-saturated.

- **Type inference is local.** When using `(curry f)` or writing your own wrapper,
  the elaborator needs enough surrounding context to fix the result type. If you
  see `TUR-E0001` on a curried call, ascribe the intended type at the binding site.

---

## See also

- [Binding Forms Guide](binding-forms-guide.md) -- `let`, `fn`, `defn` semantics
- [Effects System Guide](effects-system-guide.md) -- how effect rows flow through partial applications
- `stdlib/macros.tur` -- the `curry` macro source
