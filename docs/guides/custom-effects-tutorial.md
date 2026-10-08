---
title: Custom Effects Tutorial
category: Tutorials
description: Writing custom effects
---

# Custom Effects Tutorial

A step-by-step guide to algebraic effects in Turmeric. Each section builds on the previous one, starting from the simplest possible effect and working up to real-world patterns like mock I/O and dependency injection.

> **Prerequisites**: Working `build/tur` binary (run `just build`, or the CMake bootstrap in the README). No prior knowledge of algebraic effects is required.

---

## Table of Contents

[What are algebraic effects?](#what-are-algebraic-effects)
[Defining and performing an effect](#defining-and-performing-an-effect)
[Resuming with a value](#resuming-with-a-value)
[Effects with parameters](#effects-with-parameters)
[Multiple effects in one handler](#multiple-effects-in-one-handler)
[Nested handlers](#nested-handlers)
[Multiple sequential performs](#multiple-sequential-performs)
[Effects with `defer`](#effects-with-defer)
[Effects with refs and rc](#effects-with-refs-and-rc)
[Wrapping a handler in a macro](#wrapping-a-handler-in-a-macro)
[Bridging effects to panics](#bridging-effects-to-panics)
[Checking the continuation with `cont?`](#checking-the-continuation-with-cont)
[Real-world pattern: mock I/O](#real-world-pattern-mock-io)
[Real-world pattern: injectable logging](#real-world-pattern-injectable-logging)
[Summary and next steps](#summary-and-next-steps)

---

## What are algebraic effects?

An algebraic effect is a typed, named operation that a function can **perform** without knowing who will handle it. The caller supplies a **handler** that intercepts the operation and decides what to do, including whether to resume the suspended computation with a return value.

Think of it as a structured, type-safe alternative to:
- Global mutable state (replace with an injectable value via `Ask`)
- Printf-style I/O scattered through business logic (replace with a `Write` effect)
- Callbacks (replace with a `perform` that yields control and resumes)

The three primitives are:

| Form | What it does |
|---|---|
| `(defeffect Name [param :type ...] :return-type)` | Declare an effect |
| `(perform (Name arg ...))` | Perform (suspend) the effect |
| `(handle expr (Name [p ...] k) body ...)` | Handle the effect; `k` is the continuation |
| `(resume k value)` | Resume the suspended computation with `value` |

---

## Defining and performing an effect

The simplest effect takes no parameters and returns nothing useful. It is a signal.

```turmeric
;; Declare an effect that signals "I want to emit a line of output."
(defeffect Emit [] :nil)

;; A function that emits twice.
(defn greet [] : nil
  (do
    (perform (Emit))
    (perform (Emit))))

;; Handle by printing a fixed message each time.
(handle (greet)
  (Emit [] k) (do (println "hello!") (resume k nil)))
```
```sweet-exp
;; Declare an effect that signals "I want to emit a line of output."
defeffect Emit [] :nil

;; A function that emits twice.
defn greet [] :nil
  do
    perform(Emit())
    perform(Emit())

;; Handle by printing a fixed message each time.
(handle (greet)
  (Emit [] k) (do (println "hello!") (resume k nil)))
```

**Output:**
```
hello!
hello!
```

Key points:
- `defeffect` declares the name, parameter list, and return type.
- `perform` suspends the current function and passes control to the nearest enclosing `handle`.
- The handler receives the continuation `k` and calls `resume k nil` to let the function continue.
- Forgetting to call `resume` terminates the computation at that point.

---

## Resuming with a value

When an effect has a non-`nil` return type, `resume` passes a value back to the call site of `perform`. This is how you inject values into otherwise pure code.

```
;; An effect that "asks" for an integer from the environment.
(defeffect Ask [] :int)

(defn use-ask [] :int
  (+ 1 (perform (Ask))))

;; Handler supplies 41; the computation receives 41, adds 1, and returns 42.
(println (handle (use-ask)
  (Ask [] k) (resume k 41)))
; => 42
```

The return type of `perform (Ask)` is `:int` (the declared return type of the `Ask` effect), so the result flows directly into `(+ 1 ...)`.

---

## Effects with parameters

Effects can carry data to the handler. Declare typed parameters just like function arguments.

```turmeric
;; An effect that asks the handler to double a value.
(defeffect Double [x :int] :int)

(defn use-double [n :int] :int
  (perform (Double n)))

(println (handle (use-double 21)
  (Double [x] k) (resume k (* x 2))))
; => 42
```
```sweet-exp
;; An effect that asks the handler to double a value.
defeffect Double [x :int] :int

defn use-double [n :int] :int
  perform(Double(n))

println
  (handle (use-double 21)
    (Double [x] k) (resume k {x * 2}))
; => 42
```

The handler pattern `(Double [x] k)` binds `x` to the argument supplied by the performer and `k` to the continuation. You can compute any value to pass back via `resume`.

---

## Multiple effects in one handler

A single `handle` block can match several different effects.

```turmeric
(defeffect Ask  []       :int)
(defeffect Tell [x :int] :nil)

;; Performs both effects: asks for a number, then tells the result.
(defn use-both [] : int
  (let [result (+ 1 (perform (Ask)))]
    (perform (Tell result))
    result))

(println (handle (use-both)
  (Ask  []    k) (resume k 41)
  (Tell [x]   k) (do (println x) (resume k nil))))
; prints: 42
; returns: 42
```
```sweet-exp
defeffect Ask  []       :int
defeffect Tell [x :int] :nil

;; Performs both effects: asks for a number, then tells the result.
defn use-both [] :int
  let [result {1 + perform(Ask())}]
    perform(Tell(result))
    result

println
  (handle (use-both)
    (Ask  []  k) (resume k 41)
    (Tell [x] k) (do (println x) (resume k nil)))
; prints: 42
; returns: 42
```

Handlers are tried in order. Each clause is independent; unhandled effects bubble up to the next enclosing `handle`.

Multiple effects can also carry different result transforms:

```turmeric
(defeffect Add [x :int] :int)
(defeffect Mul [x :int] :int)

(defn compute [] : int
  (* (perform (Add 3)) (perform (Mul 4))))

(println (handle (compute)
  (Add [x] k) (resume k (+ x 10))   ; 3+10 = 13
  (Mul [x] k) (resume k (* x 2))))  ; 4*2  =  8
; => 104
```
```sweet-exp
defeffect Add [x :int] :int
defeffect Mul [x :int] :int

defn compute [] :int
  {perform(Add(3)) * perform(Mul(4))}

println
  (handle (compute)
    (Add [x] k) (resume k {x + 10})   ; 3+10 = 13
    (Mul [x] k) (resume k {x * 2}))   ; 4*2  =  8
; => 104
```

---

## Nested handlers

Handlers nest lexically. The innermost matching handler wins.

```turmeric
(defeffect Val [] :int)

(defn get-val [] : int
  (perform (Val)))

;; Outer handler supplies 10; inner overrides with 42 for its scope.
(println (handle
  (+ (get-val)
     (handle (get-val)
       (Val [] k) (resume k 42)))
  (Val [] k) (resume k 10)))
; => 52  (10 + 42)
```
```sweet-exp
defeffect Val [] :int

defn get-val [] :int
  perform(Val())

;; Outer handler supplies 10; inner overrides with 42 for its scope.
println
  (handle
    {get-val() +
       handle(get-val()
         (Val [] k) resume(k 42))}
    (Val [] k) (resume k 10))
; => 52  (10 + 42)
```

Use this to test functions under different conditions without changing the function itself.

---

## Multiple sequential performs

Each `perform` is an independent suspension. The handler is re-entered for every perform, and each invocation gets its own fresh `k`.

```turmeric
(defeffect Choose [n :int] :int)

;; Calls Choose twice sequentially.
(defn pick-two [] : int
  (+ (perform (Choose 1)) (perform (Choose 2))))

(println (handle (pick-two)
  (Choose [n] k) (resume k (* n 10))))
; First:  1 * 10 = 10
; Second: 2 * 10 = 20
; => 30
```
```sweet-exp
defeffect Choose [n :int] :int

;; Calls Choose twice sequentially.
defn pick-two [] :int
  {perform(Choose(1)) + perform(Choose(2))}

println
  (handle (pick-two)
    (Choose [n] k) (resume k {n * 10}))
; First:  1 * 10 = 10
; Second: 2 * 10 = 20
; => 30
```

Continuations are **one-shot**: you must call `resume k` exactly once per handler activation.

---

## Effects with `defer`

`defer` cleanup runs correctly even when `perform` is inside the same `do` block. The continuation is resumed before deferred forms unwind.

```turmeric
(defeffect Ask [] :int)

(defn deferred-ask [] : int
  (do
    (defer (println "cleanup"))
    (perform (Ask))))

(println (handle (deferred-ask)
  (Ask [] k) (resume k 42)))
; prints: cleanup
; prints: 42
```
```sweet-exp
defeffect Ask [] :int

defn deferred-ask [] :int
  do
    defer(println("cleanup"))
    perform(Ask())

println
  (handle (deferred-ask)
    (Ask [] k) (resume k 42))
; prints: cleanup
; prints: 42
```

The `defer` fires when `deferred-ask` returns, which happens after the handler resumes `k` with 42.

---

## Effects with refs and rc

Borrows and reference-counted values that are live at the point of `perform` remain valid across the perform/resume boundary.

```turmeric
;; Refs are live during perform.
(defeffect GetBase [] :int)

(defn sum-with-base [] : int
  (let [base (ref 100)]
    (+ (deref base) (perform (GetBase)))))

(println (handle (sum-with-base)
  (GetBase [] k) (resume k 42)))
; => 142
```
```sweet-exp
;; Refs are live during perform.
defeffect GetBase [] :int

defn sum-with-base [] :int
  let [base ref(100)]
    {deref(base) + perform(GetBase())}

println
  (handle (sum-with-base)
    (GetBase [] k) (resume k 42))
; => 142
```

```turmeric
;; RC values are live during perform; no leaks.
(defeffect GetCount [] :int)

(defn use-rc [] : int
  (let [r (rc/of 42)]
    (+ 0 (perform (GetCount)))))

(println (handle (use-rc)
  (GetCount [] k) (resume k 42)))
; => 42
```
```sweet-exp
;; RC values are live during perform; no leaks.
defeffect GetCount [] :int

defn use-rc [] :int
  let [r rc/of(42)]
    {0 + perform(GetCount())}

println
  (handle (use-rc)
    (GetCount [] k) (resume k 42))
; => 42
```

The borrow checker enforces that no borrow escapes its declared scope, even through effect boundaries.

---

## Wrapping a handler in a macro

Handlers that are used in many places can be packaged as macros for a cleaner call site.

```turmeric
(defeffect Write [s :cstr] :nil)

;; Package the handler as a macro so callers don't repeat boilerplate.
(defmacro with-write [body]
  (handle body
    (Write [s] k) (do (println s) (resume k nil))))

;; Usage:
(with-write
  (do
    (perform (Write "hello"))
    (perform (Write "world"))))
; hello
; world
```
```sweet-exp
defeffect Write [s :cstr] :nil

;; Package the handler as a macro so callers don't repeat boilerplate.
defmacro with-write [body]
  (handle body
    (Write [s] k) (do (println s) (resume k nil)))

;; Usage:
with-write
  do
    perform(Write("hello"))
    perform(Write("world"))
; hello
; world
```

This is the standard pattern used in `stdlib/effects.tur` for `with-write`, `with-getenv`, and `with-read-console`.

---

## Bridging effects to panics

An effect handler can abort the computation (not resume) and panic instead.
`stdlib/effects.tur` ships this pattern as `with-fail-panic`:

```turmeric
(defeffect Fail [msg :cstr] :nil)

(defmacro with-fail-panic [body]
  (handle body
    (Fail [msg] k) (panic msg)))   ; no resume -- aborts the computation

(with-fail-panic
  (do
    (println "before fail")
    (perform (Fail "something went wrong"))
    (println "after fail")))          ; never reached
; before fail
; panic: something went wrong
```
```sweet-exp
defeffect Fail [msg :cstr] :nil

defmacro with-fail-panic [body]
  (handle body
    (Fail [msg] k) (panic msg))   ; no resume -- aborts the computation

with-fail-panic
  do
    println("before fail")
    perform(Fail("something went wrong"))
    println("after fail")          ; never reached
; before fail
; panic: something went wrong
```

Not calling `resume` at all is valid -- the computation past the `perform` is simply abandoned. To turn the panic back into a value at a boundary, wrap the whole thing in `catch-unwind` (see the [Error Handling Guide](error-handling-guide.md)).

---

## Checking the continuation with `cont?`

`(cont? k)` returns `true` if the continuation has not been consumed. For algebraic effects the static one-shot check guarantees `k` is always unconsumed in the handler body, so this is mostly useful for defensive assertions.

```turmeric
(defeffect Ask [] :int)

(defn ask-with-check [] : int
  (perform (Ask)))

(println (handle (ask-with-check)
  (Ask [] k)
  (do
    (println (cont? k))   ; => true
    (resume k 42))))
; true
; 42
```
```sweet-exp
defeffect Ask [] :int

defn ask-with-check [] :int
  perform(Ask())

println
  (handle (ask-with-check)
    ;; The one clause -- report that k is still live, then resume with 42.
    (Ask [] k)
      (do
        (println (cont? k))   ; => true
        (resume k 42)))
; true
; 42
```

---

## Real-world pattern: mock I/O

Replace real I/O with a test double by swapping the handler. The business logic never changes.

```turmeric
(defeffect Read  []        :int)
(defeffect Write [s :cstr] :nil)

;; Pure business logic -- no I/O primitives.
(defn echo-doubled [] : int
  (let [n (perform (Read))]
    (perform (Write (int->str (* n 2))))
    0))

;; Production handler: real stdin/stdout.
(defmacro with-real-io [body]
  (handle body
    (Read  []    k) (resume k (read-int-console))
    (Write [s]   k) (do (println s) (resume k nil))))

;; Test handler: fixed input, captured output.
(defmacro with-test-io [input body]
  (handle body
    (Read  []    k) (resume k input)
    (Write [s]   k) (do (println s) (resume k nil))))

;; In production:
;;   (with-real-io (echo-doubled))
;;
;; In tests:
(with-test-io 21
  (echo-doubled))
; => 42
```
```sweet-exp
defeffect Read  []        :int
defeffect Write [s :cstr] :nil

;; Pure business logic -- no I/O primitives.
defn echo-doubled [] :int
  let [n perform(Read())]
    perform(Write(int->str({n * 2})))
    0

;; Production handler: real stdin/stdout.
defmacro with-real-io [body]
  (handle body
    (Read  []  k) (resume k (read-int-console))
    (Write [s] k) (do (println s) (resume k nil)))

;; Test handler: fixed input, captured output.
defmacro with-test-io [input body]
  (handle body
    (Read  []  k) (resume k input)
    (Write [s] k) (do (println s) (resume k nil)))

;; In production:
;;   with-real-io echo-doubled()
;;
;; In tests:
with-test-io 21
  echo-doubled()
; => 42
```

The function `echo-doubled` is completely isolated from I/O. Swapping the handler is the only change needed to go from production to test mode.

---

## Real-world pattern: injectable logging

Define a `Log` effect with levels. Wire it to a real logger in production, suppress it in benchmarks, and capture it in tests.

```turmeric
(defeffect Log [level :cstr msg :cstr] :nil)

;; Business logic
(defn process [x : int] : int
  (perform (Log "info" "starting"))
  (let [result (* x 2)]
    (perform (Log "info" "done"))
    result))

;; Handler: print everything
(defmacro with-stderr-log [body]
  (handle body
    (Log [level msg] k)
      (do (println msg) (resume k nil))))

;; Handler: suppress all logs
(defmacro with-silent-log [body]
  (handle body
    (Log [level msg] k) (resume k nil)))

;; Handler: only print warnings and errors
(defmacro with-warn-log [body]
  (handle body
    (Log [level msg] k)
      (if (or (= level "warn") (= level "error"))
        (do (println msg) (resume k nil))
        (resume k nil))))

(with-stderr-log (println (process 21)))
; starting
; done
; 42
```
```sweet-exp
defeffect Log [level :cstr msg :cstr] :nil

;; Business logic
defn process [x :int] :int
  perform $ Log "info" "starting"
  let [result {x * 2}]
    perform $ Log "info" "done"
    result

;; Handler: print everything
defmacro with-stderr-log [body]
  (handle body
    (Log [level msg] k) (do (println msg) (resume k nil)))

;; Handler: suppress all logs
defmacro with-silent-log [body]
  (handle body
    (Log [level msg] k) (resume k nil))

;; Handler: only print warnings and errors
defmacro with-warn-log [body]
  (handle body
    ;; Warnings and errors print; every other level is dropped silently.
    (Log [level msg] k)
      (if {{level = "warn"} or {level = "error"}}
        (do (println msg) (resume k nil))
        (resume k nil)))

with-stderr-log
  println(process(21))
; starting
; done
; 42
```

---

## Summary and next steps

### What you've learned

| Topic | Form |
|---|---|
| Declare an effect | `(defeffect Name [p :T ...] :R)` |
| Perform an effect | `(perform (Name arg ...))` |
| Handle effects | `(handle expr (Name [p ...] k) body ...)` |
| Resume computation | `(resume k value)` |
| Abort computation | Omit `resume` (e.g. panic instead) |
| Multiple effects | Multiple clauses in one `handle` |
| Nested scoping | Inner `handle` shadows outer for same effect |
| Macro wrappers | `(defmacro with-x [body] (handle body ...))` |
| Interop with defer/ref/rc | Works transparently across perform/resume |
| Bridge to panics | Handler calls `panic` instead of `resume` |

### stdlib effects

`stdlib/effects.tur` provides ready-made effects and handlers:

| Effect | Handler macro | What it does |
|---|---|---|
| `Write [s :cstr]` | `with-write` | Routes writes to `println` |
| `Fail [msg :cstr]` | `with-fail-panic` | Converts failures to panics |
| `Read []` | `with-read-console` | Reads an int from stdin |
| `GetEnv [key :cstr]` | `with-getenv` | Delegates to C `getenv(3)` |

### Further reading

- [effects-system-guide.md](effects-system-guide.md) -- algebraic effects reference: effect rows, capability effects, deep vs shallow handlers
- [`tests/fixtures/effect-*`](https://github.com/turmeric-lang/turmeric/tree/main/tests/fixtures/) -- full fixture test suite for every effect feature
- [`stdlib/future.tur`](https://github.com/turmeric-lang/turmeric/blob/main/stdlib/future.tur) -- async/await built on the effect substrate
- [`stdlib/stm.tur`](https://github.com/turmeric-lang/turmeric/blob/main/stdlib/stm.tur) -- STM built on the effect substrate
