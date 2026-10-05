---
title: Interactive REPL Tutorial
category: Getting Started
description: 22-step interactive tutorial to follow at `tur repl` or the web REPL
---

# Turmeric Interactive REPL Tutorial

This tutorial walks you through Turmeric in 22 steps, one expression at a
time. Open the REPL with `tur repl` (or load the web REPL) and type along.

Each step shows exactly what to type, what to expect back, and a brief
explanation. "Try it yourself" prompts invite you to experiment before moving
on -- they are optional but recommended.

The companion prose guide (`docs/guides/quickstart.md`) covers the same ground
with more explanation if you prefer reading over typing.

---

## How to use this tutorial

1. Start the REPL: `tur repl`
2. Type the expression shown under "Type this" and press Enter.
3. Compare your output to "Expected output".
4. Read "What happened", then try the variation at the bottom of the step.
5. Move to the next step.

If you get stuck: `:doc <name>` prints documentation for any symbol.
`:help` lists all meta-commands. A blank line abandons an incomplete
expression.

---

## Part 1 -- Expressions and the REPL

### Step 1 -- Your first expression

**What you'll learn:** how the REPL works and how arithmetic is written.

Type this:

```turmeric
(+ 1 2)
```
```sweet-exp
{1 + 2}
```

Expected output:

```
3
```

**What happened:** Turmeric uses prefix notation -- the operator comes first,
inside parentheses, followed by its arguments. `(+ 1 2)` means "add 1 and 2".
The REPL prints the result immediately.

> **REPL tip:** `:help` lists all meta-commands. `:quit` or Ctrl-D exits.

> **Try it yourself:** evaluate `(- 100 58)` and `(* 6 7)`. What does
> `(/ 144 12)` return?

---

### Step 2 -- Strings and booleans

**What you'll learn:** string literals, boolean values, equality, and `println`.

Type this:

```turmeric
(println "Hello, Turmeric!")
```
```sweet-exp
println("Hello, Turmeric!")
```

Expected output:

```
Hello, Turmeric!
```

Then try:

```turmeric
(= 1 1)
```
```sweet-exp
{1 = 1}
```

Expected output:

```
true
```

**What happened:** String literals are written in double quotes. `println`
prints a value followed by a newline and returns nil (which the REPL does not
print). `=` tests equality and returns a boolean.

> **Try it yourself:** evaluate `(not true)` and `(not false)`. Print a
> greeting of your own.

---

### Step 3 -- Binding names with `let`

**What you'll learn:** how to introduce local names scoped to an expression.

Type this:

```turmeric
(let [x 10 y 20] (+ x y))
```
```sweet-exp
let [x 10 y 20] {x + y}
```

Expected output:

```
30
```

**What happened:** `let` binds names to values for the duration of its body.
The bindings `x` and `y` do not exist outside the `let`. You can have as many
bindings as you like in the same vector.

> **REPL tip:** If you open a parenthesis and press Enter, the REPL keeps
> reading (shown with an indented prompt) until the expression is balanced.
> A blank line abandons an incomplete expression.

> **Try it yourself:** bind three values `a`, `b`, `c` and compute
> `(+ a (* b c))`.

---

### Step 4 -- Defining functions with `defn`

**What you'll learn:** how to define a named function that persists across REPL
expressions.

Type this:

```turmeric
(defn square [x : int] : int (* x x))
```
```sweet-exp
defn square [x :int] :int
  {x * x}
```

Then call it:

```turmeric
(square 9)
```
```sweet-exp
square(9)
```

Expected output:

```
81
```

**What happened:** `defn` defines a named function. The parameter list
`[x :int]` gives each parameter a name and type annotation. The return type
`:int` follows the parameter list. Top-level definitions persist for the rest
of the REPL session.

> **REPL tip:** `:doc square` prints the signature of a user-defined symbol.
> `:type (square 3)` shows the inferred return type without evaluating.

> **Try it yourself:** define a `cube` function and call it with a few values.

---

## Part 2 -- Control Flow and Recursion

### Step 5 -- `if` and `cond`

**What you'll learn:** conditional expressions; `if` always produces a value.

Type this:

```turmeric
(defn abs [n : int] : int
  (if (< n 0) (- 0 n) n))
```
```sweet-exp
defn abs [n :int] :int
  if {n < 0}
    {0 - n}
    n
```

Then:

```turmeric
(abs -5)
```
```sweet-exp
abs(-5)
```

Expected output:

```
5
```

Now try a multi-branch form:

```turmeric
(defn sign [n : int] : int
  (cond
    (> n 0) 1
    (< n 0) -1
    else    0))
```
```sweet-exp
defn sign [n :int] :int
  cond
    {n > 0}
    1
    {n < 0}
    -1
    else
    0
```

```turmeric
(sign -3)
```
```sweet-exp
sign(-3)
```

Expected output:

```
-1
```

**What happened:** `if` takes a condition, a "then" expression, and an "else"
expression -- both branches must produce a value. `cond` chains tests in order
and evaluates the expression next to the first truthy test; `else` is the
fallback.

> **Try it yourself:** write a `clamp` function that takes `n`, `lo`, and `hi`
> and returns `lo` if `n < lo`, `hi` if `n > hi`, or `n` otherwise.

---

### Step 6 -- Recursion

**What you'll learn:** writing recursive functions; base case and recursive case.

Type this:

```turmeric
(defn factorial [n : int] : int
  (if (<= n 1) 1 (* n (factorial (- n 1)))))
```
```sweet-exp
defn factorial [n :int] :int
  if {n <= 1}
    1
    {n * factorial({n - 1})}
```

Then:

```turmeric
(factorial 10)
```
```sweet-exp
factorial(10)
```

Expected output:

```
3628800
```

**What happened:** `factorial` calls itself with a smaller argument until it
reaches the base case `(<= n 1)`. Turmeric has no loop keyword -- iteration is
expressed through recursion or macros like `for` (step 13).

> **Try it yourself:** write a recursive `fib` function that computes Fibonacci
> numbers. Try `(fib 30)`.

---

### Step 7 -- `when` and `unless`

**What you'll learn:** one-arm conditionals for side-effectful code.

Type this:

```turmeric
(when true (println "yes"))
```
```sweet-exp
when true
  println("yes")
```

Expected output:

```
yes
```

Then:

```turmeric
(unless false (println "also yes"))
```
```sweet-exp
unless false
  println("also yes")
```

Expected output:

```
also yes
```

**What happened:** `when` evaluates its body only when the condition is truthy
and returns nil otherwise. `unless` is the opposite -- it evaluates its body
when the condition is falsy. Both are macros defined in `stdlib/macros.tur`.

> **Try it yourself:** use `when` to print a warning only when a value is
> negative.

---

## Part 3 -- Data: Option and Result

### Step 8 -- `Option`: constructors and predicates

**What you'll learn:** how to represent a value that may or may not be present.

Type this:

```turmeric
(some? (some 42))
```
```sweet-exp
some?(some(42))
```

Expected output:

```
true
```

Then:

```turmeric
(none? (none))
```
```sweet-exp
none?(none())
```

Expected output:

```
true
```

Then:

```turmeric
(some? (none))
```
```sweet-exp
some?(none())
```

Expected output:

```
false
```

**What happened:** `some` wraps a value in an optional container.
`none` represents the absence of a value. The predicates `some?`
and `none?` test which case you have. Evaluating `(none)` alone
produces a nil result, which the REPL does not print -- that is normal.

> **Try it yourself:** evaluate `(none? (some 0))`. Is a `some`
> of zero still `some`?

---

### Step 9 -- `Option`: safe unwrapping

**What you'll learn:** extracting a value from an `Option` without crashing,
and writing functions that return `Option` instead of panicking.

Type this:

```turmeric
(unwrap (some 99))
```
```sweet-exp
unwrap(some(99))
```

Expected output:

```
99
```

Then:

```turmeric
(unwrap-or (none) -1)
```
```sweet-exp
unwrap-or(none() -1)
```

Expected output:

```
-1
```

Now define a division function that never crashes:

```turmeric
(defn safe-div [a : int b : int]
  (if (= b 0) (none) (some (/ a b))))
```
```sweet-exp
defn safe-div [a :int b :int]
  if {b = 0}
    none()
    some({a / b})
```

```turmeric
(unwrap (safe-div 10 2))
```
```sweet-exp
unwrap $ safe-div 10 2
```

Expected output:

```
5
```

```turmeric
(unwrap-or (safe-div 10 0) -1)
```
```sweet-exp
unwrap-or(safe-div(10 0) -1)
```

Expected output:

```
-1
```

**What happened:** `unwrap` extracts the inner value or panics if the
option is none. `unwrap-or` provides a fallback instead of panicking.
Returning `Option` from a function lets callers decide how to handle the
missing case.

> **Try it yourself:** write `safe-head` that returns `(none)` for an
> empty vector and `(some (vec-get v 0))` otherwise. You will need
> `vec-len` from step 12 to check emptiness.

---

### Step 10 -- `Result`: success or failure

**What you'll learn:** the two-case `Result` type that carries either a success
value or an error value.

Type this:

```turmeric
(ok? (ok 100))
```
```sweet-exp
ok?(ok(100))
```

Expected output:

```
true
```

Then:

```turmeric
(err? (ok 100))
```
```sweet-exp
err?(ok(100))
```

Expected output:

```
false
```

Then:

```turmeric
(result-unwrap-or (err 0) -1)
```
```sweet-exp
result-unwrap-or(err(0) -1)
```

Expected output:

```
-1
```

**What happened:** `ok` wraps a success value; `err` wraps an error value.
`ok?` and `err?` test which case you have. `result-unwrap-or` extracts the
success value or returns a fallback when the result is an error. Like
`none`, evaluating `(ok 100)` or `(err 0)` alone prints nothing.

> **Try it yourself:** rewrite `safe-div` from step 9 to return `(ok (/ a b))`
> on success and `(err 0)` on division by zero.

---

### Step 11 -- Branching on `Option` and `Result` with `cond`

**What you'll learn:** composing predicates inside `cond` to dispatch on
wrapped values.

Type this:

```turmeric
(defn describe-result [r]
  (cond
    (ok? r)  (println "ok!")
    (err? r) (println "err!")
    else     (println "unknown")))
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
```

```turmeric
(describe-result (ok 1))
```
```sweet-exp
describe-result(ok(1))
```

Expected output:

```
ok!
```

Then:

```turmeric
(defn describe-option [o]
  (cond
    (some? o) (println "some!")
    (none? o) (println "none!")
    else      (println "unknown")))
```
```sweet-exp
defn describe-option [o]
  cond
    some?(o)
    println("some!")
    none?(o)
    println("none!")
    else
    println("unknown")
```

```turmeric
(describe-option (none))
```
```sweet-exp
describe-option(none())
```

Expected output:

```
none!
```

**What happened:** `cond` with `ok?` / `err?` / `some?` / `none?`
gives you a clean, readable way to branch on wrapped values. This is the
standard lightweight dispatch pattern before reaching for full pattern matching.

> **Try it yourself:** write `show-div` that calls `safe-div` and prints either
> the result or the string "division by zero".

---

## Part 4 -- Collections

### Step 12 -- Vectors

**What you'll learn:** mutable growable arrays -- creating them, adding
elements, and reading back by index.

Type this:

```turmeric
(let [v (vec-new)]
  (vec-push! v 10)
  (vec-push! v 20)
  (vec-push! v 30)
  (println (vec-len v))
  (println (vec-get v 1)))
```
```sweet-exp
let [v vec-new()]
  vec-push!(v 10)
  vec-push!(v 20)
  vec-push!(v 30)
  println(vec-len(v))
  println $ vec-get v 1
```

Expected output:

```
3
20
```

**What happened:** `vec-new` allocates an empty vector. `vec-push!` appends a
value in place (the `!` suffix signals mutation). `vec-len` returns the current
length; `vec-get` reads by zero-based index.

> **Try it yourself:** build a vector containing the integers 0 through 4 by
> calling `vec-push!` five times, then print each element.

---

### Step 13 -- The `for` macro

**What you'll learn:** counted iteration with `while` and `set!`; combining a
loop with a vector built in step 12.

Type this:

```turmeric
(let [^mut i 0]
  (while (< i 5)
    (println i)
    (set! i (+ i 1))))
```
```sweet-exp
let [^mut i 0]
  while {i < 5}
    println(i)
    set!(i {i + 1})
```

Expected output:

```
0
1
2
3
4
```

**What happened:** Turmeric has no counted `for` -- `for` is the monadic
comprehension `(for [x coll] body)`. A counted loop is `while` over a
condition, with `set!` advancing a counter. The `^mut` on the binding is what
permits `set!`; without it the binding is immutable and `set!` is rejected, so
the one mutable thing in the loop is spelled out.

> **REPL tip:** `:doc while` and `:doc set!` show the forms' signatures.

> **Try it yourself:** use `for` to push the first five square numbers into a
> vector, then print the vector's length.

---

## Part 5 -- Higher-Order Functions and Closures

### Step 14 -- Closures and `fn`

**What you'll learn:** anonymous functions that capture their surrounding scope.

Type this:

```turmeric
(let [add5 (fn [x : int] : int (+ x 5))]
  (add5 10))
```
```sweet-exp
let [add5 (fn [x :int] :int {x + 5})]
  add5(10)
```

Expected output:

```
15
```

Then define a function that returns a closure:

```turmeric
(defn make-adder [n : int] (fn [x : int] : int (+ x n)))
```
```sweet-exp
defn make-adder [n :int]
  (fn [x :int] :int {x + n})
```

```turmeric
(let [add3 (make-adder 3)] (add3 7))
```
```sweet-exp
let [add3 make-adder(3)]
  add3(7)
```

Expected output:

```
10
```

**What happened:** `fn` creates an anonymous function. When `make-adder` runs,
the returned closure captures the value of `n` from the enclosing scope and
carries it along. Each call to `make-adder` produces a distinct closure with
its own `n`.

> **Try it yourself:** write `make-multiplier` that returns a closure
> multiplying its argument by `n`.

---

### Step 15 -- Passing functions as arguments

**What you'll learn:** functions are first-class values that can be passed to
and returned from other functions.

Type this:

```turmeric
(defn apply-twice [f x : int] : int (f (f x)))
```
```sweet-exp
defn apply-twice [f x :int] :int
  f(f(x))
```

```turmeric
(apply-twice (fn [x : int] : int (* x 2)) 3)
```
```sweet-exp
apply-twice((fn [x :int] :int {x * 2}) 3)
```

Expected output:

```
12
```

**What happened:** `apply-twice` accepts any function `f` and an integer `x`,
applies `f` to `x`, then applies `f` again to the result. The inline `fn`
doubles its argument, so `3` becomes `6` then `12`.

> **Try it yourself:** write `apply-n` that takes a function, a starting value,
> and a count, and applies the function that many times.

---

## Part 6 -- Structs

### Step 16 -- Defining structs

**What you'll learn:** grouping related values into a named record type.

Type this:

```turmeric
(defstruct Point [x : int y : int])
```
```sweet-exp
defstruct Point [x :int y :int]
```

Then construct an instance and read its fields:

```turmeric
(let [p (Point 3 4)]
  (println (.x p))
  (println (.y p)))
```
```sweet-exp
let [p Point(3 4)]
  println(.x(p))
  println(.y(p))
```

Expected output:

```
3
4
```

**What happened:** `defstruct` declares a record type with named fields.
The constructor shares the struct's name. For each field `f` in struct `S`,
`defstruct` generates an accessor `S-f`. Struct values are allocated on the
heap and passed by pointer.

> **REPL tip:** `:type (Point 1 2)` confirms the inferred type of a struct
> expression.

> **Try it yourself:** define a `Rect` struct with `width :int` and
> `height :int` fields. Write an `area` function that returns `(* (Rect-width r)
> (Rect-height r))`.

---

## Part 7 -- Algebraic Effects

### Step 17 -- Declaring and performing effects

**What you'll learn:** how to declare a named effect and invoke it; what
happens without a handler.

Type this:

```turmeric
(defeffect Log [msg :cstr] :void)
```
```sweet-exp
defeffect Log [msg :cstr] :void
```

Then define a function that performs it:

```turmeric
(defn do-work [] : void
  (perform (Log "starting"))
  (perform (Log "done")))
```
```sweet-exp
defn do-work [] :void
  perform(Log("starting"))
  perform(Log("done"))
```

Now call it without a handler:

```turmeric
(do-work)
```
```sweet-exp
do-work()
```

Expected output:

```
error: unhandled effect Log
```

**What happened:** `defeffect` declares a named effect with a parameter list
and return type. `perform` raises the effect and suspends the current
computation, searching up the call stack for a matching handler. With none
present the runtime raises an error. This is intentional -- the handler comes
in the next step.

> **Try it yourself:** declare a second effect `Warn [msg :cstr] :void` and
> call `(perform (Warn "careful"))` inside `do-work`.

---

### Step 18 -- Handling effects

**What you'll learn:** writing a `handle` block that intercepts an effect and
resumes the computation.

Type this:

```turmeric
(handle (do-work)
  (Log [msg] k)
    (do (println msg) (resume k (nil-value))))
```
```sweet-exp
(handle (do-work)
  (Log [msg] k) (do (println msg) (resume k (nil-value))))
```

Expected output:

```
starting
done
```

**What happened:** `handle` wraps a computation and installs a handler clause
for `Log`. When `do-work` performs `Log`, execution jumps to the clause body.
`msg` receives the argument; `k` is the captured one-shot continuation.
`(resume k (nil-value))` passes nil back to the `perform` expression and
continues `do-work` from where it left off.

> **REPL tip:** `k` is one-shot -- calling `(resume k v)` consumes it.
> Attempting to resume twice is a runtime error.

> **Try it yourself:** change the handler body to also print a counter
> alongside each message, e.g. `"[1] starting"`.

---

### Step 19 -- Effects for dependency injection

**What you'll learn:** swapping handlers to change behavior without modifying
the computation.

Type this:

```turmeric
(handle (do-work)
  (Log [msg] k)
    (do (println (str-concat "[LOG] " msg)) (resume k (nil-value))))
```
```sweet-exp
(handle (do-work)
  (Log [msg] k) (do (println (str-concat "[LOG] " msg)) (resume k (nil-value))))
```

Expected output:

```
[LOG] starting
[LOG] done
```

**What happened:** `do-work` is unchanged. Only the handler differs -- it now
prefixes each message with `[LOG]`. Effects separate the declaration of an
operation from its implementation, making it straightforward to swap in
logging, testing, or no-op handlers without touching the business logic.

> **Try it yourself:** write a silent handler that discards log messages
> (calls `resume` without printing). Confirm that `do-work` still completes.

---

### Step 20 -- Effects that return values

**What you'll learn:** an effect whose `perform` expression evaluates to a
value supplied by the handler.

Type this:

```turmeric
(defeffect Ask [] :int)
```
```sweet-exp
defeffect Ask [] :int
```

```turmeric
(defn use-ask [] : int
  (+ 1 (perform (Ask))))
```
```sweet-exp
defn use-ask [] :int
  {1 + perform(Ask())}
```

```turmeric
(handle (use-ask)
  (Ask [] k) (resume k 41))
```
```sweet-exp
(handle (use-ask)
  (Ask [] k) (resume k 41))
```

Expected output:

```
42
```

**What happened:** `Ask` has return type `:int`, so `(perform (Ask))`
evaluates to whatever the handler passes to `resume`. The handler supplies
`41`; `use-ask` adds `1`, giving `42`. The handler controls the value the
computation receives back from `perform`.

> **Try it yourself:** change `(resume k 41)` to `(resume k 99)` and observe
> the new result of `use-ask`.

---

## Part 8 -- Wrap-up

### Step 21 -- Loading a file with `:reload`

**What you'll learn:** writing Turmeric code to a file and loading it into
the current REPL session.

First, save the following to a file called `hello.tur` in your working
directory:

```turmeric
(defn greet [name : cstr] : void
  (println (str-concat "Hello, " (str-concat name "!"))))
```
```sweet-exp
defn greet [name :cstr] :void
  println $ str-concat "Hello, " str-concat(name "!")
```

Then in the REPL:

```turmeric no-check
:reload hello.tur
```

Expected output:

```
reloaded hello.tur
```

Now call the function:

```turmeric
(greet "world")
```
```sweet-exp
greet("world")
```

Expected output:

```
Hello, world!
```

**What happened:** `:reload <file>` evaluates the contents of the file into
the current session, making all its definitions available immediately. If the
file contains an error, a diagnostic is printed and the session continues. You
can edit the file and `:reload` again without restarting.

> **Try it yourself:** add a second function to `hello.tur`, `:reload` it, and
> call the new function.

---

### Step 22 -- Where to go next

**What you'll learn:** nothing to type -- just a map of where to head next.

You have covered the core of Turmeric: expressions, functions, control flow,
`Option`, `Result`, vectors, closures, structs, and algebraic effects. Here
are the natural next stops depending on what interests you:

**Deeper on today's topics**

- `docs/guides/error-handling-guide.md` -- `Result`, `Option`, `panic`, and
  contract macros (`assert!`, `require!`, `ensure!`)
- `docs/guides/effects-system-guide.md` -- full effects reference: effect rows,
  composing handlers, one-shot vs. cloneable continuations

**Concurrency**

- `docs/guides/threading-guide.md` -- OS threads, `Mutex`, `Atomic`, channels
- `docs/guides/async-await-guide.md` -- async/await with fibers
- `docs/guides/stm-tutorial.md` -- software transactional memory

**Type system**

- `docs/guides/hkt-guide.md` -- Functor, Monad, Applicative (higher-kinded
  types)
- `docs/guides/hrt-guide.md` -- rank-2 polymorphic function parameters
- `docs/guides/gadts-guide.md` -- generalized algebraic data types
- `docs/guides/type-annotations-guide.md` -- compound annotation syntax

**Modules and packages**

- `docs/guides/module-system-guide.md` -- namespacing and exports
- `docs/guides/package-management-guide.md` -- projects, spices, `tur.lock`

**API reference**

- `docs/html/api/index.html` -- generated reference from `;;;` docstrings

> **REPL tip:** `:doc <sym>` works for any symbol in scope. It is the fastest
> way to look something up without leaving the REPL.
