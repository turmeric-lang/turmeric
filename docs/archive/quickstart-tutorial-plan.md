---
title: Quickstart & Interactive REPL Tutorial Plan
category: Getting Started
description: Authoring plan and step outline for the quickstart.md prose guide and repl-tutorial.md 22-step interactive tutorial
---

# Quickstart & Interactive REPL Tutorial Plan

> **Status: SHIPPED.** All three deliverables exist:
> [quickstart.md](../guides/quickstart.md), [repl-tutorial.md](../guides/repl-tutorial.md), and
> `tutorials/quickstart.yaml` (consumed by the REPL `:tutorial quickstart`
> meta-command). This document is the authoring plan, kept as the reference
> for the shared step order and the YAML schema.

## Overview

Two companion documents:

1. **`docs/guides/quickstart.md`** -- A standalone written guide for readers who
   prefer prose over a step-by-step prompt-and-response format. Covers the same
   ground as the tutorial but with more explanation, links to deeper guides, and
   code blocks they can paste or run from a file.

2. **`docs/guides/repl-tutorial.md`** -- A 22-step interactive tutorial designed
   to be followed at the REPL (`tur repl` or the web REPL at the Try Turmeric
   page). Each step shows the exact expression to type, the expected output, and
   a short explanation. Optional "Try it yourself" prompts invite experimentation
   before moving on.

Both documents share the same step order and concept progression so a reader
can bounce between them freely.

A parallel machine-readable form (`tutorials/quickstart.yaml`) mirrors every
step so the `:tutorial quickstart` meta-command engine (see
`archive/try-turmeric-and-tutorial-plan.md`) can consume the same content
without duplicating authoring work.

---

## Audience

- Someone who has never used Turmeric but is comfortable with at least one
  programming language.
- Does not assume Lisp/Scheme familiarity (parentheses and prefix notation are
  explained in step 1).
- Assumes the reader can run `tur repl` in a terminal or has the web REPL open.

---

## Concept Progression (22 Steps)

### Part 1 -- Expressions and the REPL (steps 1-4)

**Step 1 -- Starting the REPL and your first expression**

Concept: the REPL banner, prefix notation, integer arithmetic.

```
> (+ 1 2)
3
> (* 6 7)
42
```

REPL note: `:help` lists all meta-commands. `:quit` or Ctrl-D exits.

Try it: evaluate `(- 100 58)` and `(/ 144 12)`.

---

**Step 2 -- Strings and booleans**

Concept: string literals (`:cstr`), `true`/`false`, `println`.

```
> (println "Hello, Turmeric!")
Hello, Turmeric!
> true
true
> (= 1 1)
true
```

Try it: print your own greeting. Use `(not true)` and see what you get.

---

**Step 3 -- Binding names with `let`**

Concept: `let` introduces local, immutable bindings scoped to its body.

```
> (let [x 10 y 20] (+ x y))
30
```

REPL note: multi-line input works -- keep typing after an open paren; the REPL
waits until parentheses are balanced. A blank line abandons an incomplete
expression.

Try it: bind three values and compute a sum.

---

**Step 4 -- Defining functions with `defn`**

Concept: `defn`, parameter list, return-type annotation, top-level persistence.

```
> (defn square [x :int] :int (* x x))
> (square 9)
81
> :doc square
square : (:int -> :int)
```

REPL note: `:type <expr>` shows the inferred type without evaluating.

Try it: define a `cube` function and call it.

---

### Part 2 -- Control Flow and Recursion (steps 5-7)

**Step 5 -- Conditionals: `if` and `cond`**

Concept: `if` as an expression (always has a value), `cond` for multiple branches.

```
> (defn abs [n :int] :int (if (< n 0) (- 0 n) n))
> (abs -5)
5
> (defn sign [n :int] :int
    (cond (> n 0) 1 (< n 0) -1 else 0))
> (sign -3)
-1
```

Try it: write a `clamp` function that keeps a value within [lo, hi].

---

**Step 6 -- Recursion**

Concept: recursive `defn`, base case, accumulator pattern.

```
> (defn factorial [n :int] :int
    (if (<= n 1) 1 (* n (factorial (- n 1)))))
> (factorial 10)
3628800
```

Try it: write a recursive `fib` function. What happens with `(fib 30)`?

---

**Step 7 -- `when` and `unless`**

Concept: the `when` and `unless` macros for one-arm conditionals; nil results.

```
> (when true (println "yes"))
yes
> (unless false (println "also yes"))
also yes
```

Try it: guard a print behind a condition of your choice.

---

### Part 3 -- Data: Option and Result (steps 8-11)

**Step 8 -- `Option`: constructors and predicates**

Concept: `some`, `none`, `some?`, `none?` --
representing a value that may or may not be present.

```
> (some 42)
> (some? (some 42))
true
> (none)
> (none? (none))
true
> (some? (none))
false
```

REPL note: nil results print nothing; that is expected for `(none)`.

Try it: evaluate `(none? (some 0))` -- is a `some` of zero still
`some`?

---

**Step 9 -- `Option`: safe unwrapping**

Concept: `unwrap`, `unwrap-or`; writing a function that returns
an `Option` instead of crashing.

```
> (unwrap (some 99))
99
> (unwrap-or (none) -1)
-1
> (defn safe-div [a :int b :int]
    (if (= b 0) (none) (some (/ a b))))
> (safe-div 10 2)
> (unwrap (safe-div 10 2))
5
> (unwrap-or (safe-div 10 0) -1)
-1
```

Try it: write `safe-head` that returns `(none)` for an empty vector and
`(some (vec-get v 0))` otherwise.

---

**Step 10 -- `Result`: success or failure**

Concept: `ok`, `err`, `ok?`, `err?`, `result-unwrap-or` -- a two-case type
that carries either a success value or an error value.

```
> (ok 100)
> (err 404)
> (ok? (ok 100))
true
> (err? (ok 100))
false
> (result-unwrap-or (err 0) -1)
-1
```

Try it: rewrite `safe-div` to return `(ok (/ a b))` on success and
`(err 0)` on division by zero.

---

**Step 11 -- Branching on `Option` and `Result` with `cond`**

Concept: composing predicates inside `cond` to dispatch on wrapped values --
a lightweight alternative to full pattern-match syntax.

```
> (defn describe-result [r]
    (cond (ok? r)  (println "ok!")
          (err? r) (println "err!")
          else     (println "unknown")))
> (describe-result (ok 1))
ok!
> (defn describe-option [o]
    (cond (some? o) (println "some!")
          (none? o) (println "none!")
          else      (println "unknown")))
> (describe-option (none))
none!
```

Try it: write `show-div` that calls `safe-div` and prints either the result
or "division by zero".

---

### Part 4 -- Collections (steps 12-13)

**Step 12 -- Vectors**

Concept: `vec-new`, `vec-push!`, `vec-get`, `vec-len`, mutable growable arrays.

```
> (let [v (vec-new)]
    (vec-push! v 10)
    (vec-push! v 20)
    (vec-push! v 30)
    (println (vec-len v))
    (println (vec-get v 1)))
3
20
```

Try it: build a vector of the first five squares using a loop (see step 13).

---

**Step 13 -- Counting with `while`**

Concept: a counted loop as `while` + `^mut` + `set!`; combining it with a
vector. (Turmeric has no counted `for` -- `for` is the monadic comprehension.)

```
> (let [^mut i 0] (while (< i 5) (println i) (set! i (+ i 1))))
0
1
2
3
4
```

REPL note: `for` is a macro defined in `stdlib/macros.tur`; `:doc for` shows
its signature.

Try it: accumulate squares in a vector inside a `for` loop.

---

### Part 5 -- Higher-Order Functions and Closures (steps 14-15)

**Step 14 -- Closures and `fn`**

Concept: anonymous functions with `fn`, capturing lexical scope.

```
> (let [add5 (fn [x :int] :int (+ x 5))]
    (add5 10))
15
> (defn make-adder [n :int] (fn [x :int] :int (+ x n)))
> (let [add3 (make-adder 3)] (add3 7))
10
```

Try it: write a `make-multiplier` that returns a closure.

---

**Step 15 -- Passing functions as arguments**

Concept: functions are first-class values; passing a closure to another function.

```
> (defn apply-twice [f x :int] :int (f (f x)))
> (apply-twice (fn [x :int] :int (* x 2)) 3)
12
```

Try it: write `apply-n` that applies a function `n` times.

---

### Part 6 -- Structs and Type Definitions (step 16)

**Step 16 -- Defining structs**

Concept: `defstruct`, field access via generated getters, constructing instances.

```
> (defstruct Point [x :int y :int])
> (let [p (Point 3 4)]
    (println (.x p))
    (println (.y p)))
3
4
```

REPL note: `:type (Point 1 2)` confirms the struct type.

Try it: define a `Rect` struct with `width` and `height` fields and write an
`area` function for it.

---

### Part 7 -- Algebraic Effects (steps 17-20)

**Step 17 -- Declaring and performing effects**

Concept: `defeffect`, `perform` -- declaring a named effect and invoking it.
Without a handler in scope the runtime raises an unhandled-effect error; that
error is intentional and is resolved in the next step.

```
> (defeffect Log [msg :cstr] :void)
> (defn do-work [] :void
    (perform (Log "starting"))
    (perform (Log "done")))
> (do-work)
error: unhandled effect Log
```

Try it: declare a second effect `Warn` with the same signature and call it
inside `do-work`.

---

**Step 18 -- Handling effects**

Concept: `handle` block, the continuation `k`, `resume` -- intercepting an
effect and choosing what happens next.

```
> (handle (do-work)
    (Log [msg] k)
      (do (println msg) (resume k (nil-value))))
starting
done
```

REPL note: `k` is a one-shot continuation; calling `(resume k v)` continues
`do-work` from the point of the `perform`.

Try it: change the handler body to print a line count alongside each message.

---

**Step 19 -- Effects for dependency injection**

Concept: swapping handlers to change behavior without touching `do-work` -- the
same computation, different interpretation.

```
> (handle (do-work)
    (Log [msg] k)
      (do (println (str-concat "[LOG] " msg)) (resume k (nil-value))))
[LOG] starting
[LOG] done
```

Try it: write a "silent" handler that discards log messages entirely and
confirms that `do-work` still completes without error.

---

**Step 20 -- Effects that return values**

Concept: an effect whose return type is not `:void`; the handler supplies the
value that the `perform` expression evaluates to.

```
> (defeffect Ask [] :int)
> (defn use-ask [] :int
    (+ 1 (perform (Ask))))
> (handle (use-ask)
    (Ask [] k) (resume k 41))
42
```

Try it: change the handler to return a different integer and observe how the
result of `use-ask` changes.

---

### Part 8 -- Wrap-up (steps 21-22)

**Step 21 -- Loading a file with `:reload`**

Concept: writing code to a `.tur` file and loading it into the REPL session.

Instruction: save the following to `hello.tur`:

```turmeric
(defn greet [name : cstr] : void
  (println (str-concat "Hello, " (str-concat name "!"))))
```
```sweet-exp
defn greet [name : cstr] : void
  println(str-concat("Hello, " str-concat(name "!")))
```

Then in the REPL:

```
> :reload hello.tur
reloaded hello.tur
> (greet "world")
Hello, world!
```

Try it: add a second function to the file, reload, and call it.

---

**Step 22 -- Where to go next**

Concept: orientation within the broader ecosystem.

No code to type -- just a curated reading list:

- `docs/guides/error-handling-guide.md` -- deeper `Result`/`Option`/`panic`
- `docs/guides/effects-system-guide.md` -- full effects reference
- `docs/guides/hkt-guide.md` -- Functor, Monad, Applicative
- `docs/guides/threading-guide.md` -- OS threads and channels
- `docs/guides/stm-tutorial.md` -- software transactional memory
- `docs/guides/module-system-guide.md` -- modules and namespacing
- `docs/html/api/index.html` -- generated API reference

REPL tip: `:doc <sym>` works for any stdlib name. Start there when you
encounter an unfamiliar function.

---

## Document Structure

### `docs/guides/repl-tutorial.md`

```
# Turmeric Interactive REPL Tutorial

## How to use this tutorial
  (start tur repl, follow steps, type the shown expressions)

## Part 1 -- Expressions and the REPL
  Step 1 ... Step 4

## Part 2 -- Control Flow and Recursion
  Step 5 ... Step 7

## Part 3 -- Data: Option and Result
  Step 8 ... Step 11

## Part 4 -- Collections
  Step 12 ... Step 13

## Part 5 -- Higher-Order Functions and Closures
  Step 14 ... Step 15

## Part 6 -- Structs
  Step 16

## Part 7 -- Algebraic Effects
  Step 17 ... Step 20

## Part 8 -- Wrap-up
  Step 21 ... Step 22
```

Each step follows this template:

```markdown
### Step N -- Title

**What you'll learn:** one sentence.

Type this:

```turmeric
(expression)
```
```sweet-exp
expression()
```

Expected output:

```
result
```

**What happened:** two or three sentences.

> **REPL tip:** (optional meta-command or shortcut relevant to this step)

> **Try it yourself:** open-ended variation to reinforce the concept.
```

### `docs/guides/quickstart.md`

```
# Turmeric Quickstart

## Installation
  (tur repl, web REPL link)

## Expressions and naming
  (prose covering steps 1-4, copyable code blocks)

## Control flow
  (prose covering steps 5-7)

## Option and Result
  (prose covering steps 8-11)

## Collections
  (prose covering steps 12-13)

## Closures and higher-order functions
  (prose covering steps 14-15)

## Structs
  (prose covering step 16)

## Algebraic effects in five minutes
  (prose covering steps 17-20, with a self-contained example)

## Loading files
  (prose covering step 21)

## Next steps
  (curated links -- same as step 22)
```

---

## Tutorial Engine YAML Format

Each step in `repl-tutorial.md` has a 1-to-1 counterpart in
`tutorials/quickstart.yaml` so the `:tutorial quickstart` meta-command can
drive the same content interactively. The YAML schema follows the format
defined in `archive/try-turmeric-and-tutorial-plan.md`.

Example for step 8:

```yaml
- id: option_constructors
  title: "Option: constructors and predicates"
  instruction: |
    Evaluate (some 42) and check whether it is some using some?.
    Then create an empty option with (none) and check none?.
  expected: "(none? (none))"
  alternate_accept:
    - "(some? (some 42))"
    - "(none? (none))"
  hints:
    - "Use some to wrap a value and none for the empty case."
    - "some? and none? are the predicates."
  success_message: "Correct! some holds a value; none is the empty case."
  verify: "(none? (none))"
```

Fields that map from the Markdown template:

| Markdown field | YAML field |
|----------------|-----------|
| Step title | `title` |
| "Type this" block | `expected` (primary) + `alternate_accept` |
| "Try it yourself" | omitted from YAML (free-form, not validated) |
| REPL tip | `hints` last entry |
| "What happened" | `success_message` |

The full `tutorials/quickstart.yaml` will contain one entry per step (22
total). Steps with no single "correct" expression (step 22 -- orientation)
use `verify: nil` and `expected: ""` to signal that the engine just displays
the instruction and advances.

---

## Web REPL Integration

The step structure maps directly to the tutorial panel described in
`archive/try-turmeric-and-tutorial-plan.md`. When the web tutorial UI is built:

- The **instruction** field renders in the panel header.
- The **expected** / **alternate_accept** expressions drive the green-tick
  validation when the user's output matches.
- The **hints** list feeds the `[Hint]` button (revealed one at a time).
- The **success_message** appears on correct completion before advancing.
- The progress bar shows `current_step / 22`.

No changes to `repl-tutorial.md` are needed to support this -- the YAML file
is the bridge. Authors edit the Markdown; a script (to be written) regenerates
the YAML from it, or both files are maintained in parallel.

---

## Resolved Questions

1. **`:tutorial` meta-command** -- Both a Markdown guide and a parallel
   `tutorials/quickstart.yaml` will be produced. The YAML schema is documented
   above so the engine can consume it when built. Resolved.

2. **Web REPL integration** -- Documented above. The YAML fields map 1-to-1
   to the tutorial panel UI. Resolved.

3. **`str-concat`** -- Added to `stdlib/str.tur` as a two-argument
   `:cstr -> :cstr` function that heap-allocates a new NUL-terminated buffer.
   Step 21 nests two calls: `(str-concat "Hello, " (str-concat name "!"))`.
   Resolved.

4. **Effects handler syntax** -- Confirmed against `docs/guides/effects-system-guide.md`.
   Syntax is `(handle expr (Name [p] k) body)` with `(resume k value)`.
   Steps 17-20 use this form throughout. Resolved.

5. **Scope of steps** -- Expanded from 20 to 22: Option/Result gained one
   step (constructors/predicates split from unwrapping), Effects gained one
   step (value-returning effects). Resolved.
