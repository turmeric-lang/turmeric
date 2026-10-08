---
title: Binding Forms Guide
category: Language Basics
description: def in a body, letrec, and named-let -- the three local binding idioms that complement let and defn
---

# Binding Forms Guide

Turmeric provides three complementary local-binding primitives beyond `let`:
body-level `def` for sequential bindings, `letrec` for mutually-recursive
helpers, and *named let* for tail-recursive loops. All three desugar through
the same elaborator primitive so their typing, continuation-discipline, and
codegen are consistent with `let`.

---

## `def` -- one form, two positions

`def` means "bind this name here". **Position, not spelling, selects what that
means:**

| Position | Meaning |
|----------|---------|
| Top level | A global binding, with static storage. Redefining is an error. |
| A body | A binding scoped over the rest of the body -- a `let` in disguise. Rebinding shadows. |

`define` is an accepted **alias** for `def`, with identical meaning in both
positions. Either spelling works either way; new code can simply use `def`
everywhere.

### Body-level sequential binding

In a body -- the body of a `defn`, `fn`, `let`, `do`, `when`, or `while` --
`def` splices a `let` wrapping all subsequent forms:

```turmeric
(do
  (def x 1)
  (def y (+ x 1))
  (println y))         ;; prints 2
```
```sweet-exp
do
  def x 1
  def y {x + 1}
  println(y)         ;; prints 2
```

This is exactly equivalent to:

```turmeric
(do
  (let [x 1]
    (let [y (+ x 1)]
      (println y))))
```
```sweet-exp
do
  let [x 1]
    let [y {x + 1}]
      println(y)
```

### Supported body positions

| Form | Example |
|------|---------|
| `defn` body | `(defn f [] (def x 1) x)` |
| `fn` body | `(fn [] (def x 1) x)` |
| `let` body | `(let [y 2] (def x y) x)` |
| `do` body | `(do (def x 1) x)` |
| `when` / `unless` / `while` body | `(when c (def x 1) (use x))` |
| macro expansion | any of the above, after expansion |

An *expression* position -- an `if` branch, a call argument, a `cond` test --
is neither the top level nor a body, and a binding there would have nothing to
scope over. That is a diagnostic:

```
error: `def` here has nothing to scope over: a `def` in an expression position
binds a name no later form can see. Put it at the top level, or in a body
(`do`, `fn`, `let`, `when`, `while`), or use `let` if you meant a binding
local to this expression
```

### Annotations

Which annotations are legal depends on the position, and each one is either
accepted or rejected *by name* -- none is silently dropped.

| Annotation | Body binding | Top level |
|---|---|---|
| `^mut` | yes | yes |
| `^linear` / `^unique` / `^affine` / `^relevant` | yes | no |
| `^persistent` | no | yes |
| `^deprecated "msg"` | no | yes |

All annotations that `let` accepts work on a body-level `def`:

```turmeric
(defn counter [] : int
  (def ^mut n 0)
  (set! n (+ n 1))
  n)
```
```sweet-exp
defn counter [] : int
  def ^mut n 0
  set!(n {n + 1})
  n
```

`^mut` also works at the top level, giving a mutable global -- static storage
that `set!` may write:

```turmeric
(def ^mut hits 0)

(defn hit [] : void
  (set! hits (+ hits 1)))
```
```sweet-exp
def ^mut hits 0

defn hit [] : void
  set!(hits {hits + 1})
```

Without `^mut`, a global is immutable and `set!` on it is an error.

A `^mut` global is process-wide mutable state with **no synchronization** --
nothing checks that you share one safely across threads. Adding `^atomic` makes
every read and every `set!` sequentially consistent:

```turmeric
(def ^atomic ^mut ready 0)
```
```sweet-exp
def ^atomic ^mut ready 0
```

That prevents torn access and stops the compiler caching the global in a
register, which is what would otherwise keep a spinning reader from ever seeing
another thread's store. It does **not** make `(set! c (+ c 1))` safe -- that is
a load then a store, not an atomic read-modify-write, so two threads still lose
updates. Use `stdlib/atomic.tur`'s CAS or fetch-add for a counter, or
`stdlib/mutex.tur` for anything wider. `^atomic` is eight-byte scalars only and
does not imply `^mut`.

`^thread-local` goes the other way: instead of synchronising one shared value,
each thread gets **its own copy**, initialized by running the declared
initializer on that thread.

```turmeric
(def ^thread-local scratch (make-buffer))   ;; one buffer per thread, not shared
```
```sweet-exp
def ^thread-local scratch make-buffer()   ;; one buffer per thread, not shared
```

Under `tur --interpret` it is a plain global: turi has no user-reachable thread
spawn, so there is no second thread for it to differ on. It does not combine
with `^atomic` -- a per-thread copy is unshared, so its accesses need no
synchronisation -- and its initializer may not reference another
`^thread-local`, because per-thread initialization order would otherwise become
observable.

The concurrency story for globals -- what `^atomic` and `^thread-local` cover,
and what they deliberately do not -- is in
[mutable-globals-guide.md](mutable-globals-guide.md).

For *scoped* ambient state -- a value a call tree should see but callers should
be able to rebind -- prefer a dynamic variable (`defdynamic` / `binding`, see
`stdlib/dynvar.tur`) over a mutable global. It is the same per-thread machinery
with a scope attached, and it does not leave a name any function can write.

`^persistent` and `^deprecated` are top-level annotations -- static storage and
a deprecation nudge on a global. Neither has a local meaning, so both are
rejected on a body binding.

The substructural annotations go the other way: they are body-only. `^linear`
and `^relevant` are verified when the binding's scope ends, and a global's
scope never ends; `^affine` would count elaboration sites across the whole
program rather than uses at run time; `^unique` asserts no aliasing, which a
name every function can reach cannot have. All four are rejected at the top
level with that reason rather than accepted and left unenforced.

Annotations may appear in any order before the name:

```turmeric
(def ^mut ^persistent cache (hamt/new))
(def ^persistent ^mut cache (hamt/new))   ;; identical
```
```sweet-exp
def ^mut ^persistent cache hamt/new()
def ^persistent ^mut cache hamt/new()   ;; identical
```

### Type annotations

A type annotation goes between the name and the init, in either the spaced or
the fused spelling, exactly as top-level `def` and `let` accept it:

```turmeric
(def total : int (+ a b))
(def total :int  (+ a b))
```
```sweet-exp
def total : int {a + b}
def total :int  {a + b}
```

### Semantics: let\* (sequential)

Each body-level `def` sees all earlier bindings but **not** later ones -- the
same rule as `let`. Self-recursion inside its init does not work:

```turmeric
;; Error: f is not in scope inside its own init
(def f (fn [n] (f n)))

;; Fix: use letrec (see below) or lift f to top-level defn
```
```sweet-exp
;; Error: f is not in scope inside its own init
def f (fn [n] f(n))

;; Fix: use letrec (see below) or lift f to top-level defn
```

For mutually-recursive helpers, use `letrec`.

---

## `letrec` -- Mutually-recursive local bindings

`letrec` is like `let` but pre-registers every name in the binding group
before any init is elaborated, so each init can reference any name in the
group:

```turmeric
(defn run [] : int
  (letrec [even? (fn [n : int] : bool (if (= n 0) true  (odd?  (- n 1))))
           odd?  (fn [n : int] : bool (if (= n 0) false (even? (- n 1))))]
    (println (if (even? 10) "even" "odd"))
    0))
```
```sweet-exp
defn run [] :int
  letrec [even? (fn [n :int] :bool if({n = 0} true  odd?({n - 1})))
          odd?  (fn [n :int] :bool if({n = 0} false even?({n - 1})))]
    println $ if(even?(10) "even" "odd")
    0
```

### Self-recursion

A single self-recursive function is the common case:

```turmeric
(defn main [] : int
  (letrec [fact (fn [n : int] : int
                  (if (= n 0) 1 (* n (fact (- n 1)))))]
    (println (fact 5))   ;; 120
    0))
```
```sweet-exp
defn main [] : int
  letrec [fact (fn [n : int] : int
                  if({n = 0} 1 {n * fact({n - 1})}))]
    println(fact(5))   ;; 120
    0
```

### Type annotation requirement

For mutual recursion, annotate the return type so the elaborator can
build placeholder types before the bodies are checked:

```turmeric
(letrec [a (fn [n : int] : int (b (- n 1)))
         b (fn [n : int] : int (if (= n 0) 0 (a n)))]
  (a 3))
```
```sweet-exp
letrec [a (fn [n : int] : int b({n - 1}))
        b (fn [n : int] : int if({n = 0} 0 a(n)))]
  a(3)
```

Omitting `:int` on mutually-referencing functions causes an "unknown type"
error; add `:ret` annotations to resolve it.

### Non-function bindings

`letrec` allows non-function bindings as long as they do not self-reference:

```turmeric
;; Fine: x is a plain value, no self-reference
(letrec [x 42]
  x)

;; Error: x cannot reference itself during initialization
(letrec [x (+ x 1)]
  x)
```
```sweet-exp
;; Fine: x is a plain value, no self-reference
letrec [x 42]
  x

;; Error: x cannot reference itself during initialization
letrec [x {x + 1}]
  x
```

---

## Named `let` -- Tail-recursive loops

Named let is the standard Scheme/Racket idiom for tail-recursive iteration.
Write `(let loop [bindings...] body...)` to both introduce a local function
`loop` and call it with the initial argument values:

```turmeric
;; Sum 1..n
(defn sum [n : int] : int
  (let loop [i n acc 0]
    (if (= i 0)
      acc
      (loop (- i 1) (+ acc i)))))
```
```sweet-exp
defn sum [n :int] :int
  let loop [i n acc 0]
    if {i = 0}
      acc
      loop({i - 1} {acc + i})
```

The named let desugars to:

```turmeric
(letrec [loop (fn [i : int acc : int] : int
                (if (= i 0) acc (loop (- i 1) (+ acc i))))]
  (loop n 0))
```
```sweet-exp
letrec [loop (fn [i : int acc : int] : int
                if({i = 0} acc loop({i - 1} {acc + i})))]
  loop(n 0)
```

### Type annotations in named let

Annotate binding names the same way as `let`:

```turmeric
(let loop [xs : int n :int 0]
  (if (= xs 0)
    n
    (loop (cons-tail xs) (+ n 1))))
```
```sweet-exp
let loop [xs : int n :int 0]
  if {xs = 0}
    n
    loop(cons-tail(xs) {n + 1})
```

---

## Choosing the right form

| Situation | Reach for |
|---|---|
| Simple sequential local name | body-level `def` or `let` |
| Self-recursive local function | `letrec [f (fn ...)]` |
| Mutually recursive local helpers | `letrec [f (fn ...) g (fn ...)]` |
| Tail-recursive loop with initial values | Named `let` |
| Top-level constant or function | top-level `def` / `defn` |

Body-level `def` and `let` share sequential semantics: pick whichever reads
more naturally in context. `def` saves one level of indentation when the body
is long; `let` keeps the binding and its use visually co-located.

Named `let` is idiomatic for loops; `letrec` is idiomatic for mutual helpers
that are too specific to lift to top level.

---

## See also

- [Style Guide](style-guide.md) -- indentation rules for `let` / `do` bodies
- [tur/macros API](../html/api/tur-macros.html) -- `for`, `cond`, `when`, and other loop/control macros (generated by `tur run docs`)
