---
title: Effects System Guide
category: Advanced Control Flow
description: Algebraic effects, dependency injection, custom control flow
---

# Effects System Guide

This guide explains Turmeric's algebraic effects system and when to use it.

## Overview

Turmeric implements **algebraic effect handlers** inspired by OCaml 5, enabling ergonomic asynchronous programming, custom control flow, and composable abstractions. Effects allow you to perform operations that handlers can intercept and re-route dynamically.

## Core Concepts

### Effects, Performs, and Handlers

Three core primitives:

- **`(defeffect Name [param :type ...] :return-type)`** -- Declares a new effect `Name`.
- **`(perform (Name arg ...))`** -- Raises the effect, searching the dynamic handler stack for a matching handler.
- **`(handle expr (Name [p ...] k) body ...)`** -- Installs a handler. When the effect fires, `k` is the captured continuation; call `(resume k value)` to continue.

```turmeric
;; Declare an effect
(defeffect Read [] :str)

;; Perform the effect
(def name (perform (Read)))

;; Handle the effect
(handle
  (let [name (perform (Read))]
    (println (str "Hello " name)))
  (Read [] k) (resume k "World"))
```
```sweet-exp
;; Declare an effect
defeffect Read [] :str

;; Perform the effect
def name perform(Read())

;; Handle the effect
(handle
  (let [name (perform (Read))]
    (println (str "Hello " name)))
  (Read [] k) (resume k "World"))
```

### One-Shot Continuations

Continuations in Turmeric are **one-shot**: calling `(resume k v)` consumes `k`, preventing reuse. This matches Turmeric's ownership model. (See [Logic Programming Guide](logic-programming-guide.md) for cloneable continuations.)

### Properties

- **Effects are not in the type system** -- Whether `perform` is handled is checked dynamically. Unhandled effects raise an exception at runtime.
- **Dynamic dispatch** -- The closest matching handler in the call stack handles the effect.
- **Composable** -- Handlers can chain; inner handlers shadow outer ones for the same effect.

### Composing Handlers

Compose handlers for different effects by **nesting `handle` forms** -- the
inner handler runs inside the outer one, and each effect is routed to the
handler that declares it:

```turmeric
(defeffect Log [msg :cstr] :nil)
(defeffect Counter [] :int)

(handle
  (handle
    (do (perform (Log "tick")) (+ (perform (Counter)) 1))
    (Counter [] k) (resume k 41))
  (Log [msg] k) (do (println msg) (resume k nil)))
;; prints "tick", evaluates to 42
```
```sweet-exp
defeffect Log [msg :cstr] :nil
defeffect Counter [] :int

(handle
  (handle
    (do (perform (Log "tick")) {(perform (Counter)) + 1})
    (Counter [] k) (resume k 41))
  (Log [msg] k) (do (println msg) (resume k nil)))
;; prints "tick", evaluates to 42
```

### First-Class Handler Values

A handler is also a **value**: you can create one, pass it, apply it to a body,
and compose two of them.

- **Create** a handler with the `(handler (E [params] k) body)` literal. It has
  type `(handler E V R)` and carries the case detached from any body.
- **Apply** a handler value with `(with-handler hv body)` -- the body runs with
  `hv`'s dispatch table installed, exactly as if its case were written inline in
  a `handle`. (`with-handler` with case/body pairs instead of a single value
  argument remains the inline-`handle` sugar.)
- **Compose** two handlers over *different* effects with
  `(compose-handlers h1 h2)`; `h1` is the **outer** handler.

```turmeric
(defeffect Log [msg :cstr] :nil)
(defeffect Counter [] :int)

;; compose-handlers builds one handler value over both effects (h1 = Counter is
;; the outer handler).  Applying it is identical to the nested handle above.
(with-handler
  (compose-handlers
    (handler (Counter [] k)   (resume k 41))
    (handler (Log [msg] k)    (do (println msg) (resume k nil))))
  (do (perform (Log "tick")) (+ (perform (Counter)) 1)))
;; prints "tick", evaluates to 42
```
```sweet-exp
defeffect Log [msg :cstr] :nil
defeffect Counter [] :int

;; compose-handlers builds one handler value over both effects (h1 = Counter is
;; the outer handler).  Applying it is identical to the nested handle above.
with-handler
  compose-handlers
    handler (Counter [] k)
      resume(k 41)
    handler (Log [msg] k)
      do
        println(msg)
        resume(k nil)
  do
    perform(Log("tick"))
    {perform(Counter()) + 1}
;; prints "tick", evaluates to 42
```

Semantics (precedence, the disjoint-effect rule, answer-type agreement, and
per-case continuation discipline) are specified in
[first-class-handlers-semantics.md](https://github.com/turmeric-lang/turmeric/blob/main/docs/archive/history/first-class-handlers-semantics.md).

- **Overlap is rejected.** Composing two handlers for the *same* effect is a
  compile error (`TUR-E0251`); composition is defined only for disjoint effect
  sets.
- **Effects discharge from the row.** `(with-handler hv body)` removes `hv`'s
  handled effect(s) from the body's effect row; a leftover (unhandled) effect
  propagates to the enclosing function just as it does for `handle`.
- **Continuation discipline is per case.** `^linear` / `^multishot` / the
  default affine `k` are enforced inside a handler value identically to inline
  `handle`, and composition never blends two handlers' disciplines.

## Common Use Cases

### Direct-Style Async

Effects enable ergonomic async/await (see [Async/Await Guide](async-await-guide.md)):

```turmeric
(async
  (await (read-file "data.txt"))
  (println "done"))
```
```sweet-exp
async
  await(read-file("data.txt"))
  println("done")
```

### Custom Control Flow

Implement generators, early returns, or custom exception handling:

```turmeric
;; Generator: yield values one at a time
(defeffect Yield [v :int] :nil)

(handle
  (do
    (perform (Yield 1))
    (perform (Yield 2)))
  (Yield [v] k) (do (println v) (resume k nil)))
```
```sweet-exp
;; Generator: yield values one at a time
defeffect Yield [v :int] :nil

(handle
  (do
    (perform (Yield 1))
    (perform (Yield 2)))
  (Yield [v] k) (do (println v) (resume k nil)))
```

### Dependency Injection

Mock I/O operations in tests:

```turmeric
(defeffect ReadFile [path :cstr] :str)

;; Production
(handle code
  (ReadFile [path] k) (resume k (read-file-real path)))

;; Tests
(handle code
  (ReadFile [path] k) (resume k "mock data"))
```
```sweet-exp
defeffect ReadFile [path :cstr] :str

;; Production
(handle code
  (ReadFile [path] k) (resume k (read-file-real path)))

;; Tests
(handle code
  (ReadFile [path] k) (resume k "mock data"))
```

### Transactional Retry

Automatic conflict resolution (see [STM Tutorial](stm-tutorial.md)):

```turmeric
(defeffect Retry [] :nil)

(handle
  (fn []
    (when (< (read-tvar x) 10)
      (perform (Retry))))
  ;; Re-run transaction on conflict
  (Retry [] k) (resume k nil))
```
```sweet-exp
defeffect Retry [] :nil

(handle
  (fn []
    (when {(read-tvar x) < 10}
      (perform (Retry))))
  ;; Re-run transaction on conflict
  (Retry [] k) (resume k nil))
```

## Effect Rows (Typed Effects)

Effect rows track which effects a function may perform as part of its type. The
compiler infers rows automatically; you can also annotate them explicitly.

### Syntax

An effect row appears between the parameter list and the return type:

```turmeric
;; Annotated: may perform the Write effect
(defn log-msg [msg : cstr] #fx{Write} : nil
  (perform (Write msg)))

;; Pure: performs no effects
(defn add [a : int b : int] #fx{} : int
  (+ a b))

;; Row-polymorphic: propagates the row of the function argument
(defn run-twice [f :(fn [] #fx{e} int)] #fx{e} : int
  (+ (f) (f)))
```
```sweet-exp
;; Annotated: may perform the Write effect
defn log-msg [msg :cstr] #fx{Write} :nil
  perform(Write(msg))

;; Pure: performs no effects
defn add [a :int b :int] #fx{} :int
  {a + b}

;; Row-polymorphic: propagates the row of the function argument
defn run-twice [f :(fn [] #fx{e} int)] #fx{e} :int
  {f() + f()}
```

The row `#fx{e}` is a row variable: `run-twice` performs whatever effects `f` performs, no more.

### Compiler flags

| Flag | Effect |
|---|---|
| `--dump-effects` | Print each top-level `defn`'s inferred effect row after checking |
| `--strict-effects` | Warn (`TUR-W0030`) on any function -- a `defn`, a `fn` literal, an instance method -- that performs effects but declares no row; also `TUR-W0032` on a row variable that is always concrete. Callers propagate the inferred row either way |
| `-Werror=strict-effects` | Make the `--strict-effects` warnings errors, so a build can gate on annotations. Implies `--strict-effects` |
| `--lint-effects` | Deprecated alias for `--strict-effects` (`TUR-W0050`); it was a duplicate of it |

A `fn` literal is named in these messages by where it is -- *anonymous function
in 'dfs-or'* -- with the span on the literal itself, and the advice spells the
row to add (`add #fx{Bt} after its parameter vector`).

### Module-level visibility

Effects can be declared `^private` to prevent leakage outside their defining module:

```turmeric
(defeffect ^private InternalLog [msg :cstr] :nil)
```
```sweet-exp
defeffect ^private InternalLog [msg :cstr] :nil
```

A `^private` effect cannot be `perform`ed or `handle`d outside the module that declares it.
Cross-module effect rows are automatically filtered: if a callee internally performs a private
effect, the caller's inferred row does not include it.

To export an effect explicitly:

```turmeric
(defmodule MyLib
  (export (effect Write) (effect Read))
  ...)
```
```sweet-exp
defmodule MyLib
  export (effect Write) (effect Read)
  ...
```

Other modules import it with `:refer [(effect Write)]`.

### Capability effect tags (`^capability`)

The algebraic effects above (`Write`, `Read`, `Log`, ...) are *performed* and
*handled*. Some side effects, though, are not expressed with `perform` at all --
they happen inside inline-C or `extern-c` calls (touching the file system,
opening a socket, reading the clock). To bring those under effect-row
discipline, declare a **capability effect** with `^capability`:

```turmeric
(defeffect IO [] :nil ^capability)
(defeffect FS [] :nil ^extends IO ^capability)
```
```sweet-exp
defeffect IO [] :nil ^capability
defeffect FS [] :nil ^extends IO ^capability
```

A capability effect is a coarse *authority* tag rather than something you
`perform`. It behaves differently from an ordinary effect in two ways:

- **Justified by annotation alone.** A function tagged `#fx{FS}` is never flagged
  as over-annotated (`TUR-W0031`), even though its body never performs `FS` --
  the annotation *is* the justification.
- **Propagated from the declared row.** Capability effects are never inferred
  from a body, so they propagate from a callee's *declared* row into the
  caller's inferred row. A `#fx{}` (pure) caller of an `#fx{FS}`-tagged function
  therefore fails effect-row checking with `TUR-E0009`, exactly like the
  built-in `#fx{Unsafe}`.

Five capability tags are **compiler-known**, like `Unsafe`: they resolve in
every program, with nothing loaded, and are used to annotate the I/O-touching
modules. [`stdlib/effects.tur`](https://github.com/turmeric-lang/turmeric/blob/main/stdlib/effects.tur)
re-declares them, which is where their docs live; a declaration of one of these
names is accepted only when it matches the built-in exactly (`(defeffect FS []
:nil ^extends IO ^capability)`), and anything else is an error.

| Tag       | Used by                                     |
|-----------|---------------------------------------------|
| `#fx{IO}`   | umbrella, parent of the others              |
| `#fx{FS}`   | `fs.tur`, `csv.tur` file helpers, `tmpfile` |
| `#fx{Net}`  | `net.tur`, `async_socket.tur`, `httpd.tur`  |
| `#fx{Proc}` | `process.tur`, `env.tur`                     |
| `#fx{Rand}` | `random.tur`                                |

A sixth, `Bt`, is an ordinary `^capability` declaration in the autoloaded
`trail.tur`, next to the trail mutators it annotates:

| Tag       | Declared in       | Used by                                                     |
|-----------|-------------------|-------------------------------------------------------------|
| `#fx{Bt}`   | `trail.tur`       | every trail mutator, `bt-scope`, `with-untrailed`, `dfs-solve` |

`Bt` is not under `IO`: mutating the backtracking trail is process-local
state, not an authority over a resource outside the process. See the
[Backtrackable State Guide](backtrackable-state-guide.md#the-bt-capability).

Every name in `#fx{...}` must be declared. An uppercase name that no
`defeffect` in the compile declares (and that is not compiler-known) is
**`TUR-E0026`**, with a did-you-mean for a near miss and the module to load for
the `stdlib/effects.tur` names:

```
error [TUR-E0026]: unknown effect 'Write' in effect row: no defeffect declares
  it ('Write' is declared in stdlib/effects.tur, which is not autoloaded;
   add (load "stdlib/effects.tur"))
```

It used to be dropped silently, so `#fx{Typo}` checked as `#fx{}` -- and a
caller's `#fx{}` then passed the `TUR-E0009` check the tag existed to buy.
That is how `#fx{Bt}` sat decorative on the trail mutators for a month before
`Bt` was declared. The same check caught a stdlib `#fx{FS}` that had never
resolved and a fixture's `#fx{|e}` (a row variable is just `e`). `#fx{...}`
holds effects and row variables only: compiler attributes are `^attr`s
(`^construct`, `^byval`, `(match ^non-exhaustive ...)`).

Discipline stays **opt-in**: a function with no effect-row annotation is never
checked, so existing code that ignores effect rows keeps compiling. Only when a
caller annotates its own row (e.g. declares `#fx{}` or `#fx{Net}`) does the compiler
enforce that it has the capabilities of everything it calls.

```turmeric
(import tur/fs :refer [fs/read-text])

;; OK: declares the FS capability it relies on.
(defn load-config [path : cstr] #fx{FS} : (Result cstr IoError)
  (fs/read-text path))

;; ERROR (TUR-E0009): claims purity but reaches the file system.
(defn load-config-bad [path : cstr] #fx{} : (Result cstr IoError)
  (fs/read-text path))

;; OK: un-annotated, so the row is not checked at all.
(defn load-config-unchecked [path : cstr] : (Result cstr IoError)
  (fs/read-text path))
```
```sweet-exp
import tur/fs :refer [fs/read-text]

;; OK: declares the FS capability it relies on.
defn load-config [path : cstr] #fx{FS} : (Result cstr IoError)
  fs/read-text(path)

;; ERROR (TUR-E0009): claims purity but reaches the file system.
defn load-config-bad [path : cstr] #fx{} : (Result cstr IoError)
  fs/read-text(path)

;; OK: un-annotated, so the row is not checked at all.
defn load-config-unchecked [path : cstr] : (Result cstr IoError)
  fs/read-text(path)
```

### Printing: `println` or `(perform (Write s))`?

There are two ways to write a line, and they are not redundant.

- **`println`** is a builtin that declares **`#fx{IO}`**. Printing is tracked:
  an annotated function that prints must say so, and `#fx{}` means *does not
  even print*. It is not handleable -- a capability tag is declared, never
  performed -- so nothing can intercept, redirect or capture what it writes.
- **`(perform (Write s))`** is an algebraic effect from `stdlib/effects.tur`
  (`Write ^extends IO`). A handler decides what happens to the line:
  `with-write` prints it, a test handler can collect it into a buffer, another
  can drop it.

Reach for `println` for output you are not trying to control, and for
`perform (Write s)` when a caller should be able to intervene. In Haskell
terms, `println` is `putStrLn` -- visible in the type as `IO`, concrete --
and `Write` is an output effect in an effect library, abstract until a
handler interprets it. (Before `println` carried `#fx{IO}` it was closer to
`Debug.Trace.trace`: output the types could not see.)

```turmeric
(load "stdlib/effects.tur")   ; for Write; IO needs no load

;; ERROR (TUR-E0009): prints, so it is not pure.
(defn add-noisily [a : int b : int] #fx{} : int
  (println "adding")
  (+ a b))

;; OK: says it prints.
(defn add-loudly [a : int b : int] #fx{IO} : int
  (println "adding")
  (+ a b))

;; OK: a handler decides where the line goes.
(defn add-logged [a : int b : int] #fx{Write} : int
  (perform (Write "adding"))
  (+ a b))
```
```sweet-exp
load "stdlib/effects.tur"   ; for Write; IO needs no load

;; ERROR (TUR-E0009): prints, so it is not pure.
defn add-noisily [a : int b : int] #fx{} : int
  println("adding")
  {a + b}

;; OK: says it prints.
defn add-loudly [a : int b : int] #fx{IO} : int
  println("adding")
  {a + b}

;; OK: a handler decides where the line goes.
defn add-logged [a : int b : int] #fx{Write} : int
  perform(Write("adding"))
  {a + b}
```

`IO` is the parent of `Write`, `FS`, `Net`, `Proc` and `Rand`, so `#fx{IO}`
covers a function that prints and also performs `Write` or reads a file;
`#fx{Write}` alone does not cover `println`. Unannotated code is unaffected,
as everywhere else: only a function that declares a row is checked against it.

### Benefits

- **Polymorphism** -- row variables let higher-order functions propagate caller effects. This holds through `^fat` callback parameters as well: a parameter typed `(fn [T] #fx{E} R)` with a non-empty row is callable, and a named effectful `defn` can be passed as its value (see [Fat Closure Annotation Guide](fat-closure-annotation-guide.md#interaction-with-other-annotations)).
- **Compile-time checking** -- the compiler verifies that annotated functions do not perform unlisted effects (`TUR-E0009`).
- **Capability discipline** -- `^capability` tags put inline-C side effects (FS, Net, Proc, Rand) and printing (`IO`, on `println`) under the same row checking, opt-in per caller.
- **Auditing** -- `--dump-effects` shows the full effect signature of every function.

## Integration with Ownership and Defer

Effects interact with Turmeric's `defer` mechanism:

- `defer` cleanup runs correctly even when `perform` is inside the same `do` block (see [Custom Effects Tutorial](custom-effects-tutorial.md#effects-with-defer)).
- Capturing a continuation across a `defer` boundary is handled: the continuation's environment is cleaned up if it crosses a `defer` boundary.

## The Prompt Model and Unbounded Capture (CPS substrate)

Delimited control in Turmeric is built on a single **multi-prompt** substrate
(the Dybvig--Peyton-Jones--Sabry model); see
[`cps-transform-plan.md`](https://github.com/turmeric-lang/turmeric/blob/main/docs/archive/history/cps-transform-plan.md). The operators you
already use map onto prompts and sub-continuations:

| Operator | Prompt action |
|---|---|
| `reset` | push a fresh prompt |
| `shift` | capture the sub-continuation up to the nearest prompt; **re-install** that prompt on resume |
| `shift0` | capture up to the nearest prompt; **do not** re-install it on resume |
| `call/cc*` | capture a **multi-shot** sub-continuation (re-enter it any number of times) |
| undelimited `call/cc` | capture up to the **implicit root prompt** at program entry |

Two properties of the substrate matter in practice:

- **Unbounded capture.** A continuation is a heap-reified closure chain, not a
  fixed-size native-stack snapshot. Capturing it is O(1) and works at any call
  depth -- the fiber fast path's 16-frame ceiling (`tur_cont_alloc` returning
  `NULL` past `TUR_CONT_MAX_CAPTURED_FRAMES`) does not apply on this path. This is what lets
  `call/cc` reach all the way back to the top of the program.

- **Implicit root prompt.** There is a prompt around program entry, so a bare
  `call/cc` with no enclosing `reset` still has something to capture up to and
  resume.

Performance is preserved by **selectivity**: only functions that can actually
reach a control operator (`shift`/`perform`/`call/cc`/...) are CPS-converted
(the "coloring" analysis, viewable with `--dump-cps-coloring`); direct-style hot
code keeps its native calling convention and pays no trampoline or allocation
cost. See the plan for the full model.

### Performs inside loops and conditionals

A `perform` reachable from a `while` body is the shape of every event loop
and every "perform per item" traversal, and it is supported: the loop lowers
to a recursive `__cps` helper that threads the enclosing handler chain, so an
interior effect reaches an outer handler and the loop resumes where it left
off. A `perform` inside an `if`/`when` arm in statement position is likewise
fine -- the code after the conditional runs exactly once per resume.

```turmeric
(defeffect Done [score : int] : nil)

(defn run [] : nil
  (let [^mut i 0]
    (while (< i 10)
      (when (= i 3) (perform (Done i)))   ; abort or resume, either way
      (set! i (+ i 1)))))
```
```sweet-exp
defeffect Done [score : int] : nil

defn run [] : nil
  let [^mut i 0]
    while {i < 10}
      when {i = 3} perform(Done(i))   ; abort or resume, either way
      set!(i {i + 1})
```

The lowering carries a loop's `^mut` state in the helper's parameters when a
variable is assigned once and unconditionally, and in a shared cell otherwise
(assigned inside an `if`/`match` arm, more than once per iteration, or read
in a branch after its assignment), so the game-loop shape -- a tick
accumulator decremented inside a `when` -- lowers as written. A loop followed
by more statements that read the carried state is fine too. The limits that
remain evict the function to the direct emitter, with a located error at the
`perform`:

- a `while` nested inside a `while` that performs;
- a conditional assignment whose value itself performs (`(when c (set! x
  (perform ...)))` -- assign the performed value to a `let` first);
- a loop that sits inside a handler clause and performs an effect handled
  further out (hoisting the loop into a helper does not escape this one: the
  helper is evicted with its caller).

`TUR_TRACE_EVICT=1` prints which form evicted which function, and
`TUR_TRACE_CORE=1` names the form the structural check rejected.

## Continuations (`call/cc`, `escape`)

`call/cc` and `escape` capture an **undelimited** continuation against the
implicit program-wide prompt. They are enabled by default -- no flag and no
enclosing `reset` is required. (The legacy `-Xcallcc` flag is still accepted
as a deprecated no-op.)

```turmeric
;; k aborts the pending (+ 100 ...) and returns 41 at the call/cc site; the
;; outer (+ 1 ...) makes 42 -- with no enclosing reset.
(defn answer [] : int
  (+ 1 (call/cc (fn [k] (+ 100 (k 41))))))   ; => 42

;; f that ignores k just returns its body value.
(defn plain [] : int
  (+ 1 (call/cc (fn [k] 10))))                ; => 11
```
```sweet-exp
;; k aborts the pending (+ 100 ...) and returns 41 at the call/cc site; the
;; outer (+ 1 ...) makes 42 -- with no enclosing reset.
defn answer [] : int
  {1 + call/cc((fn [k] {100 + k(41)}))}   ; => 42

;; f that ignores k just returns its body value.
defn plain [] : int
  {1 + call/cc((fn [k] 10))}                ; => 11
```

**Undelimited vs. delimited.** This is the one thing `call/cc` adds over
`shift`/`reset`: capture reaches the implicit root prompt, not the nearest
`reset`. A nearer explicit `reset` does **not** shorten `call/cc`/`escape`'s
reach -- use `shift`/`shift0` or `call/cc*` when you want delimited capture.

| Operator | Capture extent | Re-install prompt on `(k v)`? |
|---|---|---|
| `shift` | nearest `reset` | yes |
| `shift0` | nearest `reset` | no |
| `escape` | implicit root prompt (undelimited) | no (abort flavor) |
| `call/cc` | implicit root prompt (undelimited) | n/a (one-shot upward) |
| `call/cc*` | nearest `cloneable-reset` | n/a (multi-shot) |

**`escape` is the abort flavor.** `(escape f)` hands `f` a continuation whose
`(k v)` unwinds to the escape site with `v` *without* re-installing a prompt
(the `shift0`-style behavior). It is the idiomatic early-exit:

```turmeric
(defn first-positive [] : int
  (escape (fn [k]
    (when (> -3 0) (k -3))
    (when (>  7 0) (k 7))     ; first positive: aborts here with 7
    -1)))                     ; default if nothing matched
```
```sweet-exp
defn first-positive [] : int
  escape
    (fn [k]
      (when {-3 > 0} k(-3))
      (when {7 > 0} k(7))   ; first positive: aborts here with 7
      -1)                   ; default if nothing matched
```

**Typing.** `f` has type `cont<T> -> T`, where `T` is the prompt's answer type;
the `(call/cc f)` / `(escape f)` expression itself has type `T`. An unannotated
continuation parameter (`(fn [k] ...)`) defaults to the escape continuation
flavor, so the `(k v)` application sugar lowers to the right resume runtime --
you do not have to spell `:escape-cont` or call `tur_escape_resume` by hand.
Annotate explicitly (`:cont`, `:escape-cont`, `:serial-cont`) only when you want
a non-default flavor.

**One-shot.** The captured `k` is `^unique` by default (calling it more than once
is `TUR-E0100` / `TUR-E0101`). Opt into exactly-once accounting with `^linear`:

```turmeric
(defn use-once [] : int
  (call/cc (fn [^linear k] (k 42))))   ; k must be invoked exactly once
```
```sweet-exp
defn use-once [] : int
  call/cc((fn [^linear k] k(42)))   ; k must be invoked exactly once
```

For a **multi-shot**, cloneable/re-enterable continuation use `call/cc*` instead
(captured against an enclosing `cloneable-reset`); see the
[Logic Programming Guide](logic-programming-guide.md).

## See Also

- [Whole-Program CPS Transform Plan](https://github.com/turmeric-lang/turmeric/blob/main/docs/archive/history/cps-transform-plan.md) -- the prompt substrate, unbounded capture, and implicit root prompt
- [Serializable Continuations Guide](serializable-continuations-guide.md) -- a heap-reified sub-continuation is a flat chain, directly serializable
- [Async/Await Guide](async-await-guide.md) -- Effects-based async/await syntax
- [Logic Programming Guide](logic-programming-guide.md) -- Backtracking via cloneable continuations
- [STM Tutorial](stm-tutorial.md) -- Composable transactions with effects
- [Custom Effects Tutorial](custom-effects-tutorial.md) -- Step-by-step walkthrough of all effect patterns
- [Effects vs. Monads](effects-vs-monads.md) -- Choosing between an effect handler and a monad value
