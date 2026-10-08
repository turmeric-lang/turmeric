---
title: Structured Concurrency Guide
category: Concurrency and Async
description: TaskGroup for scoped task spawning and Future/Promise for asynchronous results
---

# Structured Concurrency Guide

Turmeric's `tur/taskgroup` and `tur/future` modules provide structured
concurrency: spawn tasks into a scoped group, wait for all of them to
complete, and cancel them together when something goes wrong.

## Overview

**TaskGroup** (`stdlib/taskgroup.tur`) is a scoped container for fibers.
Tasks are spawned into a group, and the group is not done until every task
has completed. When a task fails or the group is explicitly cancelled, all
running tasks receive a cancellation signal and are expected to cooperate
by checking `fiber-cancelled?` and exiting promptly.

**Future/Promise** (`stdlib/future.tur`) is a one-shot producer/consumer
pair. A `Promise` is the write end (fulfill or fail); a `Future` is the
read end (block until settled, or poll non-blockingly). `async`/`await`
syntax builds on futures; this guide covers the lower-level API.

## TaskGroup

### API

| Function | Type | Description |
|---|---|---|
| `task-group-new` | `[] -> TaskGroup` | Create an empty task group |
| `task-group-spawn` | `[^borrow g, f : ptr<void>] -> TaskHandle` | Spawn a fiber into the group |
| `task-group-join` | `[^borrow g, handle : TaskHandle] -> nil` | Wait for a specific task |
| `task-group-wait` | `[^borrow g] -> nil` | Block until all tasks complete |
| `task-group-cancel` | `[^borrow g] -> nil` | Cooperatively cancel all tasks |
| `task-group-cancelled?` | `[^borrow g] -> bool` | Check if the group is cancelled |
| `task-group-done?` | `[^borrow g] -> bool` | Check if all tasks are done |
| `task-group-cancel-reason` | `[^borrow g] -> int` | Why the group was cancelled |
| `task-group-free` | `[g : TaskGroup] -> nil` | Destroy the group |

`TaskGroup` is `:linear`: it owns a heap-allocated `TaskGroupBlock`
(mutex + condvar) that must be freed exactly once with `task-group-free`.
All accessors declare their group parameter `^borrow`, so a group may be
spawned into, waited on, and cancelled any number of times without
consuming it; only `task-group-free` consumes it.

`TaskHandle` is a plain opaque handle (not linear): a group hands out
many task handles (one per spawn) that are not individually freed.

### Basic Usage

```turmeric
(defn worker [n : int] : nil
  (println n)
  (task-group-task-done my-group))

(def my-group : TaskGroup (task-group-new))

(defn run-workers [] : nil
  (task-group-spawn my-group (fn [] (worker 1)))
  (task-group-spawn my-group (fn [] (worker 2)))
  (task-group-spawn my-group (fn [] (worker 3)))
  (task-group-wait my-group)
  (task-group-free my-group))
```

```sweet-exp
#lang sweet-exp

defn worker [n : int] : nil
  println(n)
  task-group-task-done(my-group)

def my-group : TaskGroup task-group-new()

defn run-workers [] : nil
  task-group-spawn(my-group (fn [] worker(1)))
  task-group-spawn(my-group (fn [] worker(2)))
  task-group-spawn(my-group (fn [] worker(3)))
  task-group-wait(my-group)
  task-group-free(my-group)
```

Each spawned task must call `task-group-task-done` when it finishes
(normally or via cancellation) so the group knows it has completed. When
all tasks have called `task-group-task-done`, the group is marked done
and `task-group-wait` returns.

### Cancellation

Cancellation is cooperative. `task-group-cancel` sets the group's
cancelled flag and the thread-local cancelled flag for fibers in the
group. Tasks must periodically check `fiber-cancelled?` or
`task-group-should-exit?` and exit if cancelled:

```turmeric
(defn long-running [] : nil
  (while (not (fiber-cancelled?))
    (do-work)
    (if (task-group-should-exit? my-group)
      (do (task-group-task-done my-group) (return))
      nil))
  (task-group-task-done my-group))
```

```sweet-exp
#lang sweet-exp

defn long-running [] : nil
  while not(fiber-cancelled?())
    do-work()
    if task-group-should-exit?(my-group)
      do(task-group-task-done(my-group) return())
      nil
  task-group-task-done(my-group)
```

### Cancel Reasons

`task-group-cancel-with-reason` sets a reason code alongside the
cancellation. Convenience wrappers cover the common cases:

| Function | Reason code | Meaning |
|---|---|---|
| `task-group-cancel` | 0 | Manual cancel |
| `task-group-cancel-panic` | 1 | A child task panicked |
| `task-group-cancel-timeout` | 2 | A timeout fired |
| `task-group-cancel-error` | 3 | An error occurred |

Read the reason with `task-group-cancel-reason`.

### Joining Individual Tasks

`task-group-join` waits for a specific spawned task to complete, identified
by the `TaskHandle` returned by `task-group-spawn`. This is useful when you
need a task's result before proceeding but do not want to wait for the
entire group:

```turmeric
(let [h (task-group-spawn my-group (fn [] (compute-result)))]
  (task-group-join my-group h)
  ;; task h is done; other tasks may still be running
  ...)
```

```sweet-exp
#lang sweet-exp

let [h task-group-spawn(my-group (fn [] compute-result()))]
  task-group-join(my-group h)
  ;; task h is done; other tasks may still be running
  ...
```

## Future and Promise

### API

| Function | Type | Description |
|---|---|---|
| `promise-new` | `[] -> Promise` | Allocate an unsettled shared cell |
| `future-handle` | `[^borrow p] -> Future` | Borrow a Promise, derive a Future (bumps refcount) |
| `future-of-cell` | `[p : Promise] -> Future` | Transfer a Promise into a Future (consumes p) |
| `promise-pair` | macro: `[cell] -> (Promise Future)` | Expand to both handles from one cell |
| `promise-fulfill` | `[p : Promise, v : int] -> nil` | Settle with a value (consumes p) |
| `promise-fail` | `[p : Promise, e : int] -> nil` | Settle with an error (consumes p) |
| `future-get` | `[^borrow f] -> ptr<void>` | Block until settled; returns a Result |
| `future-done?` | `[^borrow f] -> bool` | Non-blocking check |
| `future-of` | `[v : int] -> Future` | Pre-fulfilled future |
| `future-free` | `[f : Future] -> nil` | Free the future's reference |
| `promise-free` | `[p : Promise] -> nil` | Free the promise's reference |

`Promise` is `:linear` (must be settled or freed exactly once). `Future`
is `:affine` (may be dropped, but not used after `future-free`). Both
share a `FutureCell` with a reference count, so the two aliases can be
freed independently -- whichever drop takes the count to 0 tears the cell
down. The refcount uses atomic operations, so a Promise fulfilled on one
thread and a Future freed on another are race-free.

### Producer/Consumer with Promise/Future

```turmeric
(let [p (promise-new)]
  (let [f (future-handle p)]
    (promise-fulfill p 42)       ; producer settles (consumes p)
    (let [result (future-get f)] ; consumer blocks until settled
      ;; result is a ptr<void> to a Result in canonical tagged layout
      (println (ok? result))     ; => true
      (println (ok-val result))  ; => 42
      (future-free f))))
```

```sweet-exp
#lang sweet-exp

let [p promise-new()]
  let [f future-handle(p)]
    promise-fulfill(p 42)        ; producer settles (consumes p)
    let [result future-get(f)]   ; consumer blocks until settled
      ;; result is a ptr<void> to a Result in canonical tagged layout
      println(ok?(result))       ; => true
      println(ok-val(result))    ; => 42
      future-free(f)
```

### Pre-fulfilled Future

`future-of` creates a future that is already settled with a value. It is
immediately done:

```turmeric
(let [f (future-of 99)]
  (println (future-done? f))  ; => true
  (future-free f))
```

```sweet-exp
#lang sweet-exp

let [f future-of(99)]
  println(future-done?(f))   ; => true
  future-free(f)
```

### Failure

`promise-fail` settles the promise with an error code. `future-get`
returns a `Result` whose `err?` is true:

```turmeric
(let [p (promise-new)]
  (let [f (future-handle p)]
    (promise-fail p 1)          ; settle with error code 1
    (let [result (future-get f)]
      (println (err? result))   ; => true
      (println (err-val result)) ; => 1
      (future-free f))))
```

```sweet-exp
#lang sweet-exp

let [p promise-new()]
  let [f future-handle(p)]
    promise-fail(p 1)           ; settle with error code 1
    let [result future-get(f)]
      println(err?(result))     ; => true
      println(err-val(result))  ; => 1
      future-free(f)
```

## Combining TaskGroup and Future

A common pattern: spawn tasks into a group, each fulfilling a promise with
its result, then wait for the group and collect results from the futures.

```turmeric
;; Spawn N workers, each fulfills a promise with its result
(let [g (task-group-new)]
  (let [p1 (promise-new)
        p2 (promise-new)
        f1 (future-handle p1)
        f2 (future-handle p2)]
    (task-group-spawn g (fn [] (do (promise-fulfill p1 (compute-1)) (task-group-task-done g))))
    (task-group-spawn g (fn [] (do (promise-fulfill p2 (compute-2)) (task-group-task-done g))))
    (task-group-wait g)
    (let [r1 (ok-val (future-get f1))
          r2 (ok-val (future-get f2))]
      (println (+ r1 r2))
      (future-free f1)
      (future-free f2)
      (task-group-free g))))
```

```sweet-exp
#lang sweet-exp

;; Spawn N workers, each fulfills a promise with its result
let [g task-group-new()]
  let [p1 promise-new()
       p2 promise-new()
       f1 future-handle(p1)
       f2 future-handle(p2)]
    task-group-spawn(g (fn [] do(promise-fulfill(p1 compute-1()) task-group-task-done(g))))
    task-group-spawn(g (fn [] do(promise-fulfill(p2 compute-2()) task-group-task-done(g))))
    task-group-wait(g)
    let [r1 ok-val(future-get(f1))
         r2 ok-val(future-get(f2))]
      println({r1 + r2})
      future-free(f1)
      future-free(f2)
      task-group-free(g)
```

## async/await

The `async`/`await` syntax builds on futures. `(async body)` creates a
fiber that executes `body` and returns a `Future<T>`. `(await fut)`
suspends the current fiber until the future completes. See
[async-await-guide.md](async-await-guide.md) for the full syntax and
semantics.

## What This Guide Does Not Cover

- **async/await syntax** -- covered in
  [async-await-guide.md](async-await-guide.md).
- **Channels** -- for passing values between fibers, see
  [channels-and-select-guide.md](channels-and-select-guide.md).
- **Thread pools** -- for bounded OS-thread fan-out, see
  [thread-pool-guide.md](thread-pool-guide.md).
- **Substructural types** -- `:linear` and `:affine` disciplines are
  covered in [substructural-types-guide.md](substructural-types-guide.md).
