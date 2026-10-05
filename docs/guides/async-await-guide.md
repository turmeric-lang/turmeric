---
title: Async/Await Guide
category: Concurrency and Async
description: Async/await with fibers and delimited continuations
---

# Async/Await Guide

Ergonomic asynchronous programming in Turmeric using fibers and delimited continuations.

## Overview

Turmeric's `async`/`await` syntax enables direct-style asynchronous programming for I/O-bound and concurrent tasks. The implementation builds on delimited continuations and integrates with Turmeric's effect system.

## Quick Start

```turmeric
;; Define an async function
(async
  (def data (await (read-file "data.txt")))
  (def result (await (process-data data)))
  (println result))

;; Type: returns a Future<T>
(def fut (async
  (+ 1 (await (fetch 2)))))

;; Block until completion
(println (await fut))  ; => 3
```

```sweet-exp
;; Define an async function
async
  def data await(read-file("data.txt"))
  def result await(process-data(data))
  println(result)

;; Type: returns a Future<T>
def fut
  async
    {1 + await(fetch(2))}

;; Block until completion
println(await(fut))  ; => 3
```

## Core Concepts

### Fibers

A **fiber** is a user-space thread (lightweight thread) that:
- Has its own execution state (implemented via delimited continuations)
- Can **yield** (suspend) and **resume**
- Runs on an OS thread managed by a scheduler
- Has no separate OS stack (avoids thread creation overhead)

### Async Blocks

- **`(async body)`** -- Creates a fiber that executes `body`. Returns a `Future<T>` that can be awaited.
- **`(await fut)`** -- Suspends the current fiber until `fut` completes. Used only inside `async` blocks.

### Futures

- **`Future<T>`** -- Represents a computation that may not be done yet.
- Can be awaited with `(await fut)`.
- Composable: multiple `await`s sequence operations.

### Task panics

A panic inside an `(async ...)` body is that **task's** failure, not the
spawner's. It is caught at the task boundary and the future is left in a
rejected state carrying the panic message, so:

- the spawn returns normally -- the statement after it runs;
- a task whose failure nobody demands never terminates the program;
- `(await f)` on a rejected task re-raises the task's panic **at the await**,
  which a `catch-unwind` there catches, and which prints the task's own
  message and aborts when no handler is in scope.

```turmeric
(let [fut (async boom)
      r   (catch-unwind (fn [] : int (await fut)))]
  (if (err? r) (println "task failed") (println "ok")))
```

See the "Panic inside async tasks" section of the
[Error Handling Guide](error-handling-guide.md) for what is still planned
(cancel-vs-panic precedence, async-main exit codes, the WASM lowering).

### Scheduling

The default scheduler is single-threaded: all fibers run on one OS thread, avoiding data races. A multi-threaded work-stealing scheduler (fibers distributed across a pool of OS threads) is available separately via `stdlib/scheduler_mt.tur`.

## Design Decisions

| Decision | Rationale |
|----------|-----------|
| **Fiber-based model** | Leverages delimited continuations; avoids C stack issues |
| **`await` desugars to `shift`** | Integrates with existing CPS infrastructure |
| **`async` desugars to `reset`** | Minimal new machinery; natural fit for the foundation |
| **`Future<T>` as return type** | Composable, can be awaited or passed to other code |
| **No implicit thread spawning** | Explicit control for predictable performance |
| **Integration with `defer`** | Cleanup on fiber completion; consistent with scope model |

## Under the Hood

### Desugaring

```turmeric
;; Surface syntax
(async (+ 1 (await (fetch 2))))

;; Desugars to (conceptually)
(reset
  (fn []
    (+ 1 (shift k
      (fiber-suspend (fetch 2) k)))))

;; The scheduler later resumes k with the result of (fetch 2)
```

```sweet-exp
;; Surface syntax
async({1 + await(fetch(2))})

;; Desugars to (conceptually)
reset
  (fn []
    {1 +
       shift(k
         fiber-suspend(fetch(2) k))})

;; The scheduler later resumes k with the result of fetch(2)
```

### Comparison to Threads

| Feature | Threads | Async/Await |
|---------|-------------------|----------------------|
| **Model** | OS-level 1:1 threads | User-space fibers |
| **Overhead** | ~10-100us per thread | ~1us per fiber |
| **Scalability** | 100s-1000s max | 100k+ feasible |
| **Stack** | Real OS stack | CPS-based (heap) |
| **Use case** | CPU-bound parallelism | I/O-bound concurrency |

## Common Patterns

### Sequential Operations

```turmeric
(async
  (def a (await (fetch-a)))
  (def b (await (fetch-b)))
  (process a b))
```

```sweet-exp
async
  def a await(fetch-a())
  def b await(fetch-b())
  process(a b)
```

### Concurrent Operations

```turmeric
;; Fork two concurrent operations; wait for both
(async
  (let [fut-a (async (fetch-a))
        fut-b (async (fetch-b))
        a     (await fut-a)
        b     (await fut-b)]
    (process a b)))
```

```sweet-exp
;; Fork two concurrent operations; wait for both
async
  let [fut-a async(fetch-a())
       fut-b async(fetch-b())
       a     await(fut-a)
       b     await(fut-b)]
    process(a b)
```

### Error Handling

Effect handlers work within async blocks. `try-with` is sugar for `handle`:
the body comes first, followed by `(EffectName [params] k)` clause heads and
their handler bodies (see [Effects System Guide](effects-system-guide.md)):

```turmeric
(async
  (try-with
    (await (fetch-file "missing.txt"))
    (FileError [path] k) (resume k "default")))
```

```sweet-exp
async
  try-with
    await(fetch-file("missing.txt"))
    (FileError [path] k)
    resume(k "default")
```

## Send Requirements for Async Bodies

### The Send-across-await rule

Every value whose binding is in scope at an `(await ...)` point within an
inline `(async (fn [] ...))` closure must be `Send`. Non-Send types include:

| Type | Reason not Send |
|---|---|
| `rc<T>` | Single-threaded refcount; races on cross-thread resume |
| `ref<T>` | Owning reference tied to the creating fiber |
| `&T` / `&mut T` | Borrows; lifetime is fiber-bound |
| `cont<T>` | Continuation captures C stack frame |

The compiler enforces this conservatively: **any binding in scope at the
`await`** is treated as live, whether or not it is actually used after the
await.

```turmeric no-check
;; ERROR TUR-E0022: rc<int> is not Send
(async (fn []
  (let [x (rc/of 42)]
    (await (async zero))  ;; x is in scope -- not Send
    (rc/deref x))))

;; OK: int is Send
(async (fn []
  (let [x 42]
    (await (async zero))  ;; x is in scope -- Send
    x)))
```

### Workaround

Consume or drop non-Send values before the `await`:

```turmeric no-check
(async (fn []
  (let [x (rc/of 42)
        v (rc/deref x)]   ;; extract the value first
    (rc/drop x)           ;; drop x before the await point
    (await (async zero))  ;; only v (an int) is in scope
    v)))
```

### Scope

The check runs at **every** `await`, wherever it appears. Writing the body
inline as `(async (fn [] ...))` and lifting it into a named `defn` handed to
`(async my-fn)` are diagnosed the same way -- the two are semantically
identical, so hoisting is not an escape hatch:

```turmeric no-check
;; Both of these are TUR-E0022.
(defn main [] : nil
  (await (async (fn [] : int
    (let [x (rc/of 42)]
      (await (async zero))    ;; x live across an await
      (rc/deref x))))))

(defn f [] : int
  (let [x (rc/of 42)]
    (await (async zero))      ;; ... and so is this
    (rc/deref x)))
(defn main [] : nil (await (async f)))
```

This is conservative in the direction of safety: *every* binding in scope at
an `await` must be Send, whether or not it is genuinely read afterwards. To
carry a non-Send value across an await, end its scope before the await point
(a nested `let`) or convert it to a Send representation.

## I/O Bindings

Turmeric doesn't provide built-in async I/O primitives. Import via FFI:

```turmeric
;; Example: libuv or custom C bindings
(defn read-file [path]
  (with-future-from-ffi "libuv_read" path))

(async
  (println (await (read-file "data.txt"))))
```

```sweet-exp
;; Example: libuv or custom C bindings
defn read-file [path]
  with-future-from-ffi("libuv_read" path)

async
  println(await(read-file("data.txt")))
```

## See Also

- [Effects System Guide](effects-system-guide.md) -- Effects foundation
- [Threading Guide](threading-guide.md) -- OS-level threads and primitives
- [STM Tutorial](stm-tutorial.md) -- Composable concurrent transactions
