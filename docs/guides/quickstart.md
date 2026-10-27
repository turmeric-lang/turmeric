---
title: Quickstart
category: Getting Started
description: Prose introduction: expressions, functions, control flow, Option, Result, collections, closures, structs, and algebraic effects
tags:
  - syntax
  - effects
  - result-option
---

# Turmeric Quickstart

This guide introduces Turmeric through short, self-contained examples. It
covers the same ground as the interactive REPL tutorial
(`docs/guides/repl-tutorial.md`) but in prose form, with code blocks you can
paste or run from a file.

By the end you will have seen: expressions, functions, conditionals, recursion,
`Option`, `Result`, vectors, closures, structs, and algebraic effects.

---

## Getting started

**CLI:** run `tur repl` in your terminal. Exit with `:quit` or Ctrl-D.

**Web REPL:** open the Try Turmeric page in your browser -- no installation
needed.

**Don't have `tur` yet?** Install it with the version manager
(`curl -sSf https://turmeric-lang.com/install | sh` installs
[`tvm`](https://github.com/turmeric-lang/tvm#readme) and the latest release), or see
the [Releases and Installation guide](releases-and-installation-guide.md)
for Homebrew, prebuilt binaries, Docker, and build-from-source.

---

## Expressions and naming

### Prefix notation

Turmeric is a Lisp-family language. Every operation is written as a
parenthesised list with the operator first:

```turmeric
(+ 1 2)       ; => 3
(* 6 7)       ; => 42
(- 100 58)    ; => 42
```
```sweet-exp
{1 + 2}       ; => 3
{6 * 7}       ; => 42
{100 - 58}    ; => 42
```

Strings are double-quoted. Booleans are `true` and `false`. `println` prints
a value followed by a newline:

```turmeric
(println "Hello, Turmeric!")   ; prints: Hello, Turmeric!
(= 1 1)                        ; => true
(not true)                     ; => false
```
```sweet-exp
println("Hello, Turmeric!")    ; prints: Hello, Turmeric!
{1 = 1}                        ; => true
not(true)                      ; => false
```

### Local bindings with `let`

`let` introduces names scoped to its body:

```turmeric
(let [x 10 y 20] (+ x y))   ; => 30
```
```sweet-exp
let [x 10 y 20] {x + y}   ; => 30
```

The bindings are made in order, and each value sees the names bound before
it, so one `let` handles sequential bindings -- no need to nest them:

```turmeric
(let [x 10 y (* x 2)] (+ x y))   ; => 30
```
```sweet-exp
let [x 10 y {x * 2}] {x + y}   ; => 30
```

### Defining functions

`defn` defines a named function. Parameter types and return type are
annotated with `:type`:

```turmeric
(defn square [x : int] : int (* x x))

(square 9)    ; => 81
```
```sweet-exp
defn square [x : int] : int
  {x * x}

square(9)    ; => 81
```

In the REPL, top-level definitions persist across expressions. `:doc square`
prints the signature; `:type (square 3)` shows the inferred type without
evaluating.

---

## Control flow

### `if` as an expression

`if` always produces a value -- both branches are required:

```turmeric
(defn abs [n : int] : int
  (if (< n 0) (- 0 n) n))

(abs -5)    ; => 5
(abs  5)    ; => 5
```
```sweet-exp
defn abs [n : int] : int
  if {n < 0}
    {0 - n}
    n

abs(-5)    ; => 5
abs(5)     ; => 5
```

### Multi-branch with `cond`

`cond` chains tests in order. `else` is the fallback:

```turmeric
(defn sign [n : int] : int
  (cond (> n 0) 1
        (< n 0) -1
        else    0))

(sign  3)    ; => 1
(sign -3)    ; => -1
(sign  0)    ; => 0
```
```sweet-exp
defn sign [n : int] : int
  cond
    {n > 0}
    1
    {n < 0}
    -1
    else
    0

sign(3)     ; => 1
sign(-3)    ; => -1
sign(0)     ; => 0
```

### One-arm conditionals: `when` and `unless`

Use `when` and `unless` for side effects that only run under a condition:

```turmeric
(when   (< x 0) (println "negative"))
(unless (= x 0) (println "non-zero"))
```
```sweet-exp
when {x < 0}
  println("negative")
unless {x = 0}
  println("non-zero")
```

Both return nil when the condition is not met.

### Recursion

Turmeric has no loop keyword. Repeat work through recursion or the `for`
macro (see Collections below):

```turmeric
(defn factorial [n : int] : int
  (if (<= n 1) 1 (* n (factorial (- n 1)))))

(factorial 10)    ; => 3628800
```
```sweet-exp
defn factorial [n : int] : int
  if {n <= 1}
    1
    {n * factorial({n - 1})}

factorial(10)    ; => 3628800
```

---

## Option and Result

### `Option`: values that might be absent

`some` wraps a value; `none` represents absence.

```turmeric
(some? (some 42))    ; => true
(none? (none))       ; => true
(some? (none))       ; => false
```
```sweet-exp
some?(some(42))    ; => true
none?(none())      ; => true
some?(none())      ; => false
```

Extract the value with `unwrap` (panics on none) or provide a fallback
with `unwrap-or`:

```turmeric
(unwrap     (some 99))    ; => 99
(unwrap-or  (none) -1)    ; => -1
```
```sweet-exp
unwrap(some(99))        ; => 99
unwrap-or(none() -1)    ; => -1
```

Returning `Option` instead of crashing is idiomatic for operations that
might not produce a value:

```turmeric
(defn safe-div [a : int b : int]
  (if (= b 0) (none) (some (/ a b))))

(unwrap    (safe-div 10 2))     ; => 5
(unwrap-or (safe-div 10 0) -1)  ; => -1
```
```sweet-exp
defn safe-div [a : int b : int]
  if {b = 0}
    none()
    some({a / b})

unwrap $ safe-div 10 2           ; => 5
unwrap-or(safe-div(10 0) -1)     ; => -1
```

### `Result`: success or failure

`ok` wraps a success value; `err` wraps an error value:

```turmeric
(ok? (ok 100))               ; => true
(err? (ok 100))              ; => false
(result-unwrap-or (err 0) -1)    ; => -1
```
```sweet-exp
ok?(ok(100))                    ; => true
err?(ok(100))                   ; => false
result-unwrap-or(err(0) -1)     ; => -1
```

Use `Result` when the error case carries a useful value (an error code, a
message, a structured error type) that the caller should inspect.

### Branching with `cond`

Combine predicates inside `cond` to dispatch on either type:

```turmeric
(defn describe-result [r]
  (cond (ok? r)  (println "ok!")
        (err? r) (println "err!")
        else     (println "unknown")))

(defn describe-option [o]
  (cond (some? o) (println "some!")
        (none? o) (println "none!")
        else      (println "unknown")))
```
```sweet-exp
defn describe-result [r]
  cond
    ok?(r)
    println("ok!")
    err?(r)
    println("err!")
    else
    println("unknown")

defn describe-option [o]
  cond
    some?(o)
    println("some!")
    none?(o)
    println("none!")
    else
    println("unknown")
```

See `docs/guides/error-handling-guide.md` for the full story: `panic`,
`result-must`, contract macros, and more.

---

## Collections

### Vectors

`vec-new` allocates an empty growable array. `vec-push!` appends in place.
`vec-get` reads by zero-based index. `vec-len` returns the current length:

```turmeric
(let [v (vec-new)]
  (vec-push! v 10)
  (vec-push! v 20)
  (vec-push! v 30)
  (println (vec-len v))      ; 3
  (println (vec-get v 1)))   ; 20
```
```sweet-exp
let [v vec-new()]
  vec-push!(v 10)
  vec-push!(v 20)
  vec-push!(v 30)
  println(vec-len(v))       ; 3
  println $ vec-get v 1     ; 20
```

The `!` suffix on `vec-push!` signals mutation -- the function modifies the
vector in place rather than returning a new one.

### Counting with `while`

Turmeric has **no counted `for`**. `for` is the monadic comprehension
(`(for [x coll] body)`, from `stdlib/macros.tur`); a counted loop is `while`
plus a `^mut` binding and `set!`:

```turmeric
(let [^mut i 0]
  (while (< i 5)
    (println i)
    (set! i (+ i 1))))
; prints 0 through 4
```
```sweet-exp
let [^mut i 0]
  while {i < 5}
    println(i)
    set!(i {i + 1})
; prints 0 through 4
```

`^mut` is what makes the binding writable -- without it `set!` is rejected --
so the loop counter is visibly the one mutable thing in the form.

Building a sequence follows the same shape:

```turmeric
(let [squares (vec-new)
      ^mut i  1]
  (while (<= i 5)
    (vec-push! squares (* i i))
    (set! i (+ i 1)))
  (let [^mut j 0]
    (while (< j 5)
      (println (vec-get squares j))
      (set! j (+ j 1)))))
; prints 1 4 9 16 25
```
```sweet-exp
let [squares vec-new()
     ^mut i  1]
  while {i <= 5}
    vec-push!(squares {i * i})
    set!(i {i + 1})
  let [^mut j 0]
    while {j < 5}
      println $ vec-get squares j
      set!(j {j + 1})
; prints 1 4 9 16 25
```

---

## Closures and higher-order functions

### Anonymous functions with `fn`

`fn` creates a function value that captures its lexical scope:

```turmeric
(let [add5 (fn [x : int] : int (+ x 5))]
  (add5 10))    ; => 15
```
```sweet-exp
let [add5 (fn [x : int] : int {x + 5})]
  add5(10)    ; => 15
```

Closures can be returned from functions, giving each call site its own
captured environment:

```turmeric
(defn make-adder [n : int] (fn [x : int] : int (+ x n)))

(let [add3 (make-adder 3)
      add7 (make-adder 7)]
  (println (add3 10))    ; 13
  (println (add7 10)))   ; 17
```
```sweet-exp
defn make-adder [n : int]
  (fn [x : int] : int {x + n})

let [add3 make-adder(3)
     add7 make-adder(7)]
  println(add3(10))    ; 13
  println(add7(10))    ; 17
```

### Functions as arguments

Functions are first-class values. Pass them to other functions:

```turmeric
(defn apply-twice [f x : int] : int (f (f x)))

(apply-twice (fn [x : int] : int (* x 2)) 3)    ; => 12
```
```sweet-exp
defn apply-twice [f x : int] : int
  f(f(x))

apply-twice((fn [x : int] : int {x * 2}) 3)    ; => 12
```

`apply-twice` is not special -- it is just a function whose first parameter
happens to be another function.

---

## Structs

`defstruct` defines a named record type. The constructor shares the struct
name; field accessors are generated as `StructName-fieldname`:

```turmeric
(defstruct Point [x : int y : int])

(let [p (Point 3 4)]
  (println (.x p))    ; 3
  (println (.y p)))   ; 4
```
```sweet-exp
defstruct Point [x : int y : int]

let [p Point(3 4)]
  println(.x(p))    ; 3
  println(.y(p))    ; 4
```

Struct values are heap-allocated. `:type (Point 1 2)` in the REPL confirms
the type.

A complete example combining struct and function:

```turmeric
(defstruct Rect [width : int height : int])

(defn area [r] : int
  (* (Rect-width r) (Rect-height r)))

(area (Rect 6 7))    ; => 42
```
```sweet-exp
defstruct Rect [width : int height : int]

defn area [r] : int
  {Rect-width(r) * Rect-height(r)}

area(Rect(6 7))    ; => 42
```

---

## Algebraic effects in five minutes

Effects separate the declaration of an operation from its implementation.
Instead of calling a concrete function, a computation *performs* an effect;
a *handler* installed by the caller decides what to do.

### Declaring and performing

`defeffect` declares a named effect:

```turmeric
(defeffect Log [msg : cstr] : void)
```
```sweet-exp
defeffect Log [msg : cstr] : void
```

`perform` raises the effect and suspends until a handler responds:

```turmeric
(defn do-work [] : void
  (perform (Log "starting"))
  (perform (Log "done")))
```
```sweet-exp
defn do-work [] : void
  perform(Log("starting"))
  perform(Log("done"))
```

Without a handler, the runtime raises an unhandled-effect error.

### Handling

`handle` wraps a computation and provides handler clauses. `k` is the
continuation -- calling `(resume k v)` passes `v` back to the `perform`
expression and continues the computation:

```turmeric
(handle (do-work)
  (Log [msg] k)
    (do (println msg) (resume k (nil-value))))
; prints:
; starting
; done
```
```sweet-exp
(handle (do-work)
  (Log [msg] k) (do (println msg) (resume k (nil-value))))
; prints:
; starting
; done
```

### Swapping handlers (dependency injection)

The same computation runs under different handlers without any changes to
`do-work`:

```turmeric
(handle (do-work)
  (Log [msg] k)
    (do (println (str-concat "[LOG] " msg)) (resume k (nil-value))))
; prints:
; [LOG] starting
; [LOG] done
```
```sweet-exp
(handle (do-work)
  (Log [msg] k) (do (println (str-concat "[LOG] " msg)) (resume k (nil-value))))
; prints:
; [LOG] starting
; [LOG] done
```

A silent handler discards all messages:

```turmeric
(handle (do-work)
  (Log [msg] k) (resume k (nil-value)))
; prints nothing; do-work still completes
```
```sweet-exp
(handle (do-work)
  (Log [msg] k) (resume k (nil-value)))
; prints nothing; do-work still completes
```

### Effects that return values

When an effect's return type is not `:void`, the `perform` expression
evaluates to whatever the handler passes to `resume`:

```turmeric
(defeffect Ask [] : int)

(defn use-ask [] : int
  (+ 1 (perform (Ask))))

(handle (use-ask)
  (Ask [] k) (resume k 41))
; => 42
```
```sweet-exp
defeffect Ask [] : int

defn use-ask [] : int
  {1 + perform(Ask())}

(handle (use-ask)
  (Ask [] k) (resume k 41))
; => 42
```

See `docs/guides/effects-system-guide.md` for the full reference: effect rows,
composable handlers, and one-shot vs. cloneable continuations.

---

## Loading files

Write Turmeric code to a `.tur` file and load it into any REPL session with
`:reload`:

```turmeric
; hello.tur
(defn greet [name : cstr] : void
  (println (str-concat "Hello, " (str-concat name "!"))))
```
```sweet-exp
; hello.tur
defn greet [name : cstr] : void
  println $ str-concat "Hello, " str-concat(name "!")
```

```
> :reload hello.tur
reloaded hello.tur
> (greet "world")
Hello, world!
```

If the file contains an error, a diagnostic is printed and the session
continues. Edit the file and `:reload` again without restarting.

---

## Next steps

**Deeper on today's topics**

- `docs/guides/error-handling-guide.md` -- `panic`, `result-must`, contract
  macros (`assert!`, `require!`, `ensure!`)
- `docs/guides/effects-system-guide.md` -- full effects reference

**Concurrency**

- `docs/guides/threading-guide.md` -- OS threads, `Mutex`, `Atomic`, channels
- `docs/guides/async-await-guide.md` -- async/await with fibers
- `docs/guides/stm-tutorial.md` -- software transactional memory

**Type system**

- `docs/guides/hkt-guide.md` -- Functor, Monad, Applicative
- `docs/guides/hrt-guide.md` -- rank-2 polymorphic parameters
- `docs/guides/gadts-guide.md` -- generalized algebraic data types
- `docs/guides/type-annotations-guide.md` -- compound annotation syntax

**Modules and packages**

- `docs/guides/module-system-guide.md` -- namespacing and exports
- `docs/guides/package-management-guide.md` -- projects, spices, `tur.lock`

**API reference**

- `docs/html/api/index.html` -- generated from `;;;` docstrings

**Interactive**

- `docs/guides/repl-tutorial.md` -- the 22-step companion to this guide; type
  along in the REPL
