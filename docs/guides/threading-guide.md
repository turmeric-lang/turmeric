---
title: Threading and Concurrency Primitives Guide
category: Concurrency and Async
description: OS threads, `Arc<T>`, `Mutex<T>`, `Atomic<T>`, channels
---

# Threading and Concurrency Primitives Guide

Safe concurrent programming with OS threads, atomic types, and synchronization primitives.

## Overview

Turmeric provides **1:1 OS threads** built on POSIX threads (pthreads) plus thread-safe primitives (atomic cells, mutexes, read-write locks, condition variables). Integration with Turmeric's ownership model (`ref<T>`, borrow checking, `Send` checks) ensures memory safety.

Higher-level primitives -- channels, futures, task groups, thread pools, semaphores -- are implemented in stdlib.

## Thread Model

### Creating Threads

The simplest way to run a closure on another thread and get its result back
is `async`/`await` (a future-backed spawn):

```turmeric
;; Spawn a new thread; returns a future
(def fut
  (async
    (fn []
      (println "Hello from thread!")
      42)))

;; Block until the thread completes and get the result
(println (await fut))  ; prints 42
```
```sweet-exp
;; Spawn a new thread; returns a future
def fut
  async
    (fn []
      println("Hello from thread!")
      42)

;; Block until the thread completes and get the result
println(await(fut))  ; prints 42
```

The lower-level surface is `stdlib/thread.tur`: `(thread-spawn-fn fn-ptr arg)`
spawns a pthread running a C-ABI function pointer (no captures; pack state
into `arg`) and returns a `ThreadHandle`; `(thread-join t)` /
`(thread-detach t)` consume the handle. The `(thread-spawn (fn [] ...))`
form is the elaborator-level Send-safety gate for closures crossing a thread
boundary.

### Properties

- **1:1 model:** Each Turmeric thread maps to one OS thread.
- **OS scheduling:** OS scheduler handles preemption and load balancing.
- **Overhead:** ~10-100us per thread creation; suitable for 100s-1000s of threads.
- **Full FFI access:** Threads are real OS threads; FFI calls work naturally.

### Thread-Local Storage

A top-level `def` annotated `^thread-local` gets one copy per thread,
initialized per thread:

```turmeric
;; Declare a thread-local global
(def ^thread-local my-tls 42)

;; Read it (each thread sees its own copy)
my-tls  ; => 42

;; Set it (affects only this thread's copy)
(set! my-tls 100)
```
```sweet-exp
;; Declare a thread-local global
def ^thread-local my-tls 42

;; Read it (each thread sees its own copy)
my-tls  ; => 42

;; Set it (affects only this thread's copy)
set!(my-tls 100)
```

See [mutable-globals-guide.md](mutable-globals-guide.md) for the full rules
(`^thread-local` may not reference another `^thread-local` in its
initializer, etc.).

## Shared Ownership: Arc

**Arc** (atomic reference counting) is the thread-safe counterpart to
`rc<T>`: where `rc<T>` uses plain counts (single-threaded), the Arc control
block uses atomic counts so multiple OS threads can safely clone and drop a
shared value. The runtime lives in `src/runtime/arc.{c,h}`; the language
surface is `stdlib/arc.tur`, which is not auto-loaded:

```turmeric no-check
(defmodule app
  (import arc :refer [arc-new arc-clone arc-get arc-strong-count arc-drop])
  (defn main [] : int
    (let [a (arc-new 42)
          b (arc-clone a)]
      (println (arc-get b))            ; 42
      (println (arc-strong-count a))   ; 2
      (arc-drop b)
      (arc-drop a))
    0))
```

`Arc` and `ArcWeak` are distinct opaque handles, so passing one where the
other belongs is a type error rather than a runtime abort.

### Properties

- **Atomic:** Reference count increments/decrements are atomic (thread-safe).
- **Clone/drop:** cloning increments the strong count; dropping decrements it atomically and frees at zero.
- **Shared but not mutable:** an Arc gives shared read-only access. Guard mutable shared state with a mutex or use an atomic cell.
- **No cycle collector.** This is the one place Arc is *weaker* than `rc<T>`,
  which has a Bacon-Rajan collector. A cycle of strong Arcs leaks; break it
  by hand with `arc-downgrade` / `arc-upgrade`.

### Weak references

`arc-downgrade` yields an `ArcWeak` that does not keep the value alive.
`arc-upgrade` returns `(Option Arc)` -- not a nullable handle, because "the
value may already be gone" is the entire point of a weak reference, so the
caller has to say what happens in that case:

```turmeric no-check
(let [a (arc-new 42)
      w (arc-downgrade a)]
  (arc-drop a)                     ; last strong reference gone
  (let [u (arc-upgrade w)]
    (println (if (some? u) 1 -1))) ; -1
  (arc-weak-drop w))
```

`arc-weak-count` reports the weak handles a caller actually holds: the
runtime keeps a +1 sentinel while any strong reference lives, so the control
block is not freed under a live weak handle, and that sentinel is subtracted
out.

### When to Use Arc

- Sharing read-only data across threads.
- Ownership is genuinely shared (multiple threads live simultaneously).
- Passing objects to spawned threads.

## Mutual Exclusion: Mutex

**Mutex** (`stdlib/mutex.tur`) protects mutable shared state. A `Mutex` is a
linear opaque handle; lock and unlock borrow it, and `mutex-free` is the one
legal consumption:

The scoped form is `with-lock`, which makes the body's value the form's value
and puts the release where it cannot be forgotten:

```turmeric no-check
(let [m (mutex-new)]
  (println (with-lock m (+ 1 41)))   ; => 42
  (mutex-free m))
```

`stdlib/rwlock.tur` has the matching `with-read-lock` / `with-write-lock`.

Two things to know about all three. The lock expression is evaluated **twice**
-- once to acquire, once to release -- so pass a variable, never a call. That
is forced rather than sloppy: `Mutex` is `:linear`, so a `let` binding *moves*
the handle and the checker then rejects the enclosing scope for dropping it
unconsumed (TUR-E0100), which rules out binding it once inside the macro. And
the release is not panic-safe: if the body panics the lock stays held, so put
the unlock on an unwind path with `catch-unwind` where that matters.

The raw pair is still there when you need the acquire and release in different
scopes:

```turmeric
(def lock (mutex-new))
(def ^mut counter 0)

;; Acquire, modify, release
(mutex-lock lock)
(set! counter (+ counter 1))
(mutex-unlock lock)

;; Non-blocking attempt
(when (mutex-try-lock lock)
  (do (set! counter (+ counter 1))
      (mutex-unlock lock)))

(mutex-free lock)
```
```sweet-exp
def lock mutex-new()
def ^mut counter 0

;; Acquire, modify, release
mutex-lock(lock)
set!(counter {counter + 1})
mutex-unlock(lock)

;; Non-blocking attempt
when mutex-try-lock(lock)
  do
    set!(counter {counter + 1})
    mutex-unlock(lock)

mutex-free(lock)
```

### Properties

- **Prevents races:** Only one thread can hold the lock at a time.
- **Composable:** Multiple mutexes can be held (but beware deadlocks with out-of-order release).
- **Blocking:** If another thread holds the lock, `mutex-lock` blocks until available (`mutex-try-lock` never blocks).
- **Linear handle:** the substructural checker rejects locking a freed mutex or dropping one without `mutex-free`. `stdlib/concurrent.tur` adds a `mutex-guard-lock`/`mutex-guard-unlock` RAII-style pair.

### Read-Write Locks: RwLock

For read-heavy workloads, use `RwLock` (`stdlib/rwlock.tur`):

```turmeric
;; Multiple readers or one writer
(def rw (rwlock-new))

;; Read lock (multiple threads can hold simultaneously)
(rwlock-rdlock rw)
;; ... read the shared data ...
(rwlock-unlock rw)

;; Write lock (exclusive)
(rwlock-wrlock rw)
;; ... mutate the shared data ...
(rwlock-unlock rw)

(rwlock-free rw)
```
```sweet-exp
;; Multiple readers or one writer
def rw rwlock-new()

;; Read lock (multiple threads can hold simultaneously)
rwlock-rdlock(rw)
;; ... read the shared data ...
rwlock-unlock(rw)

;; Write lock (exclusive)
rwlock-wrlock(rw)
;; ... mutate the shared data ...
rwlock-unlock(rw)

rwlock-free(rw)
```

`rwlock-try-rdlock` / `rwlock-try-wrlock` are the non-blocking variants.

## Atomic Cells

**`AtomicCell`** (`stdlib/atomic.tur`) is a heap-allocated int64 cell with
lock-free, sequentially-consistent operations:

```turmeric
;; Atomic integer cell
(def counter (atomic-new 0))

;; Atomic load
(println (atomic-load counter))  ; => 0

;; Atomic store
(atomic-store! counter 42)

;; Atomic compare-and-swap
(atomic-cas! counter 42 100)  ; true if value was 42; cell now 100

;; Atomic add/sub/swap (each returns the OLD value)
(atomic-add! counter 5)
(atomic-sub! counter 2)
(atomic-swap! counter 7)

(atomic-free counter)
```
```sweet-exp
;; Atomic integer cell
def counter atomic-new(0)

;; Atomic load
println(atomic-load(counter))  ; => 0

;; Atomic store
atomic-store!(counter 42)

;; Atomic compare-and-swap
atomic-cas!(counter 42 100)  ; true if value was 42; cell now 100

;; Atomic add/sub/swap (each returns the OLD value)
atomic-add!(counter 5)
atomic-sub!(counter 2)
atomic-swap!(counter 7)

atomic-free(counter)
```

The cell holds an `int` (64-bit); flags, counters, and pointer-sized values
all fit. For thread-safe mutable globals there is also the `^atomic`
annotation on `def ^mut` -- see
[mutable-globals-guide.md](mutable-globals-guide.md).

### When to Use Atomic

- High-frequency operations where lock contention is too expensive.
- Fine-grained counters, flags, pointers.
- Building higher-level synchronization primitives.

## Futures and Promises

**`Future<T>`** and **`Promise<T>`** decouple the producer and consumer of an
asynchronous result. Both share a single heap-allocated cell containing a
mutex, condvar, and value/error slot.

```turmeric
;; Create a promise/future pair (same underlying cell)
(def p (promise-new))
(def f p)

;; Producer thread fulfills the promise
(async (fn [] (promise-fulfill p 42)))

;; Consumer blocks on the future
(def result (future-get f))
(println (ok? result))     ; => true
(println (ok-val result))  ; => 42
(promise-free p)
```
```sweet-exp
;; Create a promise/future pair (same underlying cell)
def p promise-new()
def f p

;; Producer thread fulfills the promise
async((fn [] promise-fulfill(p 42)))

;; Consumer blocks on the future
def result future-get(f)
println(ok?(result))     ; => true
println(ok-val(result))  ; => 42
promise-free(p)
```

### Core API

| Function | Notes |
|---|---|
| `promise-new` | Returns an unsettled `FutureCell*` |
| `promise-fulfill [p v]` | Settles ok with value `v`; aborts if already settled |
| `promise-fail [p e]` | Settles with error code `e`; aborts if already settled |
| `promise-free [p]` | Frees the shared cell |
| `future-get [f]` | Blocks until settled; returns a heap `Result*` |
| `future-done? [f]` | Non-blocking settled check |
| `future-cancel [f]` | Settles with `exn = -2` if not yet settled |
| `future-cancelled? [f]` | Returns true if cancelled (`exn == -2`) |
| `future-free [f]` | Frees the shared cell |

### Pre-Settled Futures

```turmeric
(def f (future-of 99))       ; immediately fulfilled
(future-done? f)             ; => true

(def e (future-error-of 7))  ; immediately rejected
```
```sweet-exp
def f future-of(99)       ; immediately fulfilled
future-done?(f)           ; => true

def e future-error-of(7)  ; immediately rejected
```

### Combinators

```turmeric
;; Map a function over the fulfilled value
(def f2 (future-map f (fn [v] (* v 2))))

;; Flat-map: fn must return a new future
(def f3 (future-then f (fn [v] (future-of (+ v 1)))))
```
```sweet-exp
;; Map a function over the fulfilled value
def f2 future-map(f (fn [v] {v * 2}))

;; Flat-map: fn must return a new future
def f3 future-then(f (fn [v] future-of({v + 1})))
```

### Multi-Combinators

| Function | Behaviour |
|---|---|
| `future-race [fa fb]` | First to settle wins |
| `future-all2 [fa fb]` | Both must succeed; result carries `fa`'s value |
| `future-any2 [fa fb]` | First to fulfill wins; both rejecting yields `fb`'s error |
| `future-join [fa fb]` | Both must succeed; result is a `Tuple2` pair |
| `future-race-n [futures n]` | Variadic race over a pointer array |
| `future-all-n [futures n]` | All must succeed |
| `future-any-n [futures n]` | First fulfillment wins |

```turmeric
;; Join two futures into a pair
(def tup-future (future-join fa fb))
(def tup-result (future-get tup-future))
(when (ok? tup-result)
  (let [tup (ok-val tup-result)]
    (println (tuple-first tup))
    (println (tuple-second tup))
    (tuple-free tup)))
```
```sweet-exp
;; Join two futures into a pair
def tup-future future-join(fa fb)
def tup-result future-get(tup-future)
when ok?(tup-result)
  let [tup ok-val(tup-result)]
    println(tuple-first(tup))
    println(tuple-second(tup))
    tuple-free(tup)
```

### Timeouts

```turmeric
;; Race a computation against a deadline
(def result (future-get (future-with-timeout task-future 5000)))
(if (future-cancelled? result)
  (println "timed out")
  (println (ok-val result)))

;; Stand-alone timeout future (rejects with exn = -1 after ms)
(def t (future-timeout 1000))
```
```sweet-exp
;; Race a computation against a deadline
def result future-get(future-with-timeout(task-future 5000))
if future-cancelled?(result)
  println("timed out")
  println(ok-val(result))

;; Stand-alone timeout future (rejects with exn = -1 after ms)
def t future-timeout(1000)
```

## Synchronization Primitives

### Channels

Turmeric provides two channel types backed by the same ring-buffer layout:

- **`Chan`** -- synchronous, blocking send/recv only.
- **`AsyncChan`** -- buffered, blocking by default, with non-blocking `try` variants.

#### Synchronous Channel (Chan)

```turmeric
(def ch (chan-new 8))

;; Producer thread
(async
  (fn []
    (chan-send ch 1)
    (chan-send ch 2)
    (chan-send ch 3)))

;; Consumer
(println (chan-recv ch))  ; => 1
(println (chan-recv ch))  ; => 2
(println (chan-recv ch))  ; => 3
(chan-free ch)
```
```sweet-exp
def ch chan-new(8)

;; Producer thread
async
  (fn []
    chan-send(ch 1)
    chan-send(ch 2)
    chan-send(ch 3))

;; Consumer
println(chan-recv(ch))  ; => 1
println(chan-recv(ch))  ; => 2
println(chan-recv(ch))  ; => 3
chan-free(ch)
```

| Function | Signature | Notes |
|---|---|---|
| `chan-new` | `[cap :int] :ptr<void>` | Allocates ring-buffer with given capacity |
| `chan-send` | `[ch val :int] :nil` | Blocks when full |
| `chan-recv` | `[ch] :int` | Blocks when empty |
| `chan-free` | `[ch] :nil` | Destroys mutex/condvar and frees buffer |

#### Async Buffered Channel (AsyncChan)

```turmeric
(def ch (async-chan-new 16))

(async-chan-send ch 99)

;; Non-blocking send (returns false if full)
(if (async-chan-try-send ch 100) ...)

;; Non-blocking recv (returns INT64_MIN if empty)
(let [v (async-chan-try-recv ch)] ...)

(println (async-chan-count ch))  ; current item count
(async-chan-free ch)
```
```sweet-exp
def ch async-chan-new(16)

async-chan-send(ch 99)

;; Non-blocking send (returns false if full)
if async-chan-try-send(ch 100) ...

;; Non-blocking recv (returns INT64_MIN if empty)
let [v async-chan-try-recv(ch)] ...

println(async-chan-count(ch))  ; current item count
async-chan-free(ch)
```

| Function | Signature | Notes |
|---|---|---|
| `async-chan-new` | `[cap :int] :ptr<void>` | |
| `async-chan-send` | `[ch val :int] :nil` | Blocks when full |
| `async-chan-recv` | `[ch] :int` | Blocks when empty |
| `async-chan-try-send` | `[ch val :int] :bool` | Returns false if full; never blocks |
| `async-chan-try-recv` | `[ch] :int` | Returns `INT64_MIN` if empty; never blocks |
| `async-chan-count` | `[ch] :int` | Current item count (locked) |
| `async-chan-free` | `[ch] :nil` | |

### Multi-Channel Select

`select` waits on multiple channel operations and runs the body of the first
clause that is ready. Each clause is `((chan :recv v) body)` -- binding the
received value to `v` -- or `((chan :send val) body)`, plus an optional
`(:default body)` arm; `select` returns the selected clause body's value:

```turmeric
(def ch-a (chan-new 4))
(def ch-b (chan-new 4))

;; Poll with a default arm (never blocks)
(select ((ch-a :recv v) (println (str "from ch-a: " v)))
        ((ch-b :recv v) (println (str "from ch-b: " v)))
        (:default       (println "nothing ready")))

;; Send-or-drop
(select ((ch-a :send 99) 99)
        (:default (println "ch-a full, dropping")))
```
```sweet-exp
def ch-a chan-new(4)
def ch-b chan-new(4)

;; Poll with a default arm (never blocks)
select
  ((ch-a :recv v) println(str("from ch-a: " v)))
  ((ch-b :recv v) println(str("from ch-b: " v)))
  (:default       println("nothing ready"))

;; Send-or-drop
select
  ((ch-a :send 99) 99)
  (:default println("ch-a full, dropping"))
```

Clause bodies must be type-compatible; the value of the selected body is the
value of the whole `select`.

When multiple clauses are simultaneously ready, `select` picks one uniformly at
random using an xorshift32 PRNG so that no single channel is systematically
favoured. When no clause is ready and no `:default` is present, `select` blocks
on all channels concurrently and wakes as soon as any one of them becomes ready.

### Condition Variables

Block until a condition is signaled (`stdlib/condvar.tur`):

```turmeric
(def m  (mutex-new))
(def cv (condvar-new))

;; Thread A: wait for a signal (atomically releases m while waiting)
(mutex-lock m)
(condvar-wait cv m)
(println "woken!")
(mutex-unlock m)

;; Thread B: signal one waiter (or condvar-broadcast for all)
(condvar-signal cv)
```
```sweet-exp
def m  mutex-new()
def cv condvar-new()

;; Thread A: wait for a signal (atomically releases m while waiting)
mutex-lock(m)
condvar-wait(cv m)
println("woken!")
mutex-unlock(m)

;; Thread B: signal one waiter (or condvar-broadcast for all)
condvar-signal(cv)
```

### Semaphore

A counting semaphore implemented with mutex + condvar (portable; `pthread_sem_t`
unnamed form is unavailable on macOS).

```turmeric
;; Binary semaphore (mutex-like)
(def s (sem-new 1))

;; Limit concurrency to 3 parallel workers
(def sem (sem-new 3))

(async
  (fn []
    (sem-acquire sem)
    (do-work)
    (sem-release sem)))
```
```sweet-exp
;; Binary semaphore (mutex-like)
def s sem-new(1)

;; Limit concurrency to 3 parallel workers
def sem sem-new(3)

async
  (fn []
    sem-acquire(sem)
    do-work()
    sem-release(sem))
```

| Function | Notes |
|---|---|
| `sem-new [initial]` | `initial=0` starts locked; `initial=1` is a binary semaphore |
| `sem-acquire [s]` | Decrements; blocks when count is 0 |
| `sem-release [s]` | Increments and wakes one blocked acquirer |
| `sem-free [s]` | |

### One-Time Initialization: Once

`once-call` guarantees an initializer runs exactly once no matter how many
threads call it concurrently.

```turmeric
(def flag (once-flag-new))

;; Safe to call from any number of threads
(once-call flag init-resource)

(once-flag-free flag)
```
```sweet-exp
def flag once-flag-new()

;; Safe to call from any number of threads
once-call(flag init-resource)

once-flag-free(flag)
```

| Function | Notes |
|---|---|
| `once-flag-new` | Allocates a `pthread_once_t` on the heap |
| `once-call [flag init-fn]` | Calls `init-fn` at most once across all threads |
| `once-flag-free [flag]` | |

## Thread Pools

### WorkQueue

A thread-safe FIFO used internally by both pool variants; also available
directly for custom worker patterns. A sentinel value `INT64_MIN` returned by
`work-queue-pop` signals that the queue has been closed.

```turmeric
;; Unbounded queue (grows dynamically)
(def q (work-queue-new))

;; Bounded queue (push blocks when full)
(def bq (work-queue-new-bounded 64))

(work-queue-push q 42)
(let [v (work-queue-pop q)] ...)  ; blocks until item available

;; Shutdown: wake all blocked producers and consumers
(work-queue-close q)
(work-queue-free q)
```
```sweet-exp
;; Unbounded queue (grows dynamically)
def q work-queue-new()

;; Bounded queue (push blocks when full)
def bq work-queue-new-bounded(64)

work-queue-push(q 42)
let [v work-queue-pop(q)] ...  ; blocks until item available

;; Shutdown: wake all blocked producers and consumers
work-queue-close(q)
work-queue-free(q)
```

| Function | Notes |
|---|---|
| `work-queue-new` | Unbounded; doubles capacity on growth |
| `work-queue-new-bounded [cap]` | Fixed ring-buffer; push blocks when full |
| `work-queue-push [q v]` | Blocks on bounded queue when full; no-op after close |
| `work-queue-pop [q]` | Blocks until item available; returns `INT64_MIN` after close |
| `work-queue-close [q]` | Broadcasts to all blocked threads |
| `work-queue-free [q]` | |

### Fixed Thread Pool

A pool of `n` worker threads. Tasks are C function pointers; results are
delivered via `Future` (see the Futures section).

```turmeric
(def tp (thread-pool-new 4))

;; Submit returns a Future fulfilled with the task's return value
(def fut (thread-pool-submit tp my-work-fn my-arg))
(def result (future-get fut))
(println (ok-val result))

(thread-pool-shutdown tp)  ; closes queue and joins all workers
(thread-pool-free tp)      ; must be called after shutdown
```
```sweet-exp
def tp thread-pool-new(4)

;; Submit returns a Future fulfilled with the task's return value
def fut thread-pool-submit(tp my-work-fn my-arg)
def result future-get(fut)
println(ok-val(result))

thread-pool-shutdown(tp)  ; closes queue and joins all workers
thread-pool-free(tp)      ; must be called after shutdown
```

| Function | Notes |
|---|---|
| `thread-pool-new [n]` | Spawns `n` worker threads immediately |
| `thread-pool-submit [tp task-fn task-arg]` | Returns a `FutureCell*` |
| `thread-pool-shutdown [tp]` | Closes queue, joins workers |
| `thread-pool-free [tp]` | Call after shutdown |

### Auto-Scaling Thread Pool

Starts with `min-threads` workers and grows up to `max-threads` when all
current workers are busy.

```turmeric
(def dtp (thread-pool-new-dynamic 2 8))

(def fut (thread-pool-dynamic-submit dtp my-work-fn nil))
(def result (future-get fut))

(thread-pool-dynamic-shutdown dtp)
(thread-pool-dynamic-free dtp)
```
```sweet-exp
def dtp thread-pool-new-dynamic(2 8)

def fut thread-pool-dynamic-submit(dtp my-work-fn nil)
def result future-get(fut)

thread-pool-dynamic-shutdown(dtp)
thread-pool-dynamic-free(dtp)
```

| Function | Notes |
|---|---|
| `thread-pool-new-dynamic [min max]` | Spawns `min` workers; scales to `max` |
| `thread-pool-dynamic-submit [tp task-fn task-arg]` | Spawns a new worker if idle count is 0 and below max |
| `thread-pool-dynamic-shutdown [tp]` | |
| `thread-pool-dynamic-free [tp]` | Call after shutdown |

## Structured Concurrency: TaskGroup

**`TaskGroup`** provides structured concurrency: spawn a group of fibers, then
wait for or cancel all of them together. When a task panics, the group is
automatically cancelled.

Cancellation is **cooperative** -- tasks must periodically check
`task-group-should-exit?` or `fiber-cancelled?` and exit if set.

### Basic Usage

```turmeric
;; Manual lifecycle
(def g (task-group-new))

(task-group-spawn g
  (fn []
    (println "worker A")
    (task-group-task-done g)))

(task-group-spawn g
  (fn []
    (println "worker B")
    (task-group-task-done g)))

(task-group-wait g)
(task-group-free g)

;; Preferred: task-group-with macro (calls wait automatically)
(def g (task-group-new))
(task-group-with g
  (task-group-spawn g my-fiber-a)
  (task-group-spawn g my-fiber-b))
(task-group-free g)
```
```sweet-exp
;; Manual lifecycle
def g task-group-new()

task-group-spawn g
  (fn []
    println("worker A")
    task-group-task-done(g))

task-group-spawn g
  (fn []
    println("worker B")
    task-group-task-done(g))

task-group-wait(g)
task-group-free(g)

;; Preferred: task-group-with macro (calls wait automatically)
def g task-group-new()
task-group-with g
  task-group-spawn(g my-fiber-a)
  task-group-spawn(g my-fiber-b)
task-group-free(g)
```

### Cooperative Cancellation

```turmeric
(def g (task-group-new))

(task-group-spawn g
  (fn []
    (while (not (task-group-should-exit? g))
      (do-work))
    (task-group-task-done g)))

;; Cancel from another thread or the parent
(task-group-cancel g)
(task-group-wait g)
(task-group-free g)
```
```sweet-exp
def g task-group-new()

task-group-spawn g
  (fn []
    (while not(task-group-should-exit?(g))
      do-work())
    task-group-task-done(g))

;; Cancel from another thread or the parent
task-group-cancel(g)
task-group-wait(g)
task-group-free(g)
```

### Timeouts

```turmeric
;; Auto-cancel after 5 seconds
(def g (task-group-new))
(task-group-with-timeout g 5000
  (task-group-spawn g long-running-task))
(task-group-free g)
```
```sweet-exp
;; Auto-cancel after 5 seconds
def g task-group-new()
task-group-with-timeout g 5000
  task-group-spawn(g long-running-task)
task-group-free(g)
```

### Lifecycle API

| Function | Notes |
|---|---|
| `task-group-new` | Allocates an empty group |
| `task-group-spawn [group f]` | Increments task count; returns fiber handle |
| `task-group-task-done [group]` | Each spawned task must call this on exit |
| `task-group-join [group handle]` | Wait for one specific fiber |
| `task-group-wait [group]` | Block until all tasks complete |
| `task-group-done? [group]` | Non-blocking group completion check |
| `task-handle-done? [handle]` | Non-blocking per-fiber check |
| `task-group-free [group]` | |

### Cancellation API

| Function | Notes |
|---|---|
| `task-group-cancel [group]` | Manual cancel; reason = 0 |
| `task-group-cancel-with-reason [group reason]` | 0=manual 1=panic 2=timeout 3=error |
| `task-group-cancel-panic [group]` | Convenience: reason = 1 |
| `task-group-cancel-timeout [group]` | Convenience: reason = 2 |
| `task-group-cancel-error [group]` | Convenience: reason = 3 |
| `task-group-cancel-reason [group]` | Returns the reason code |
| `task-group-cancelled? [group]` | Check group-level cancel flag |
| `fiber-cancelled?` | Check thread-local cancel flag for current fiber |
| `task-group-should-exit? [group]` | Combines both checks (preferred in task bodies) |
| `fiber-should-exit?` | Alias for `fiber-cancelled?` |

### Macros

| Macro | Notes |
|---|---|
| `task-group-with [group & body]` | Runs body then calls `task-group-wait` |
| `task-group-with-timeout [group ms & body]` | Auto-cancels after `ms` milliseconds |
| `task-group-with-cancellation [group & body]` | Skips body if already cancelled |

### Async Integration

```turmeric
;; Spawn an async computation into a task group; get a Future back
(def g (task-group-new))
(def fut (task-group-spawn-async g my-thunk))
(def result (future-get fut))
(task-group-free g)

;; Macro form
(task-group-async g my-thunk)
```
```sweet-exp
;; Spawn an async computation into a task group; get a Future back
def g task-group-new()
def fut task-group-spawn-async(g my-thunk)
def result future-get(fut)
task-group-free(g)

;; Macro form
task-group-async(g my-thunk)
```

**Panic propagation** is automatic: if a fiber spawned into a group panics, the
group is cancelled with reason 1 (panic). No extra API is needed.

## Safety Guarantees

### Send and Sync Traits

Marker traits control what types can be safely shared:

- **`Send`** -- Type can be moved across thread boundaries. If `T : Send`, `Arc<T>` can be cloned and sent to another thread.
- **`Sync`** -- Type can be safely shared via `&T` in multiple threads. If `T : Sync`, multiple threads can hold `&T` simultaneously without a `Mutex`.

```turmeric no-check
;; These are Send (safe to move to threads)
int, bool, string, (Pair a b) [Send a, Send b]

;; These are Sync (safe to share via &)
int, bool, Mutex<T> [T : Sync]

;; NOT Sync (require Mutex for shared access)
Rc<T> (thread-local ref counting)
ref<T> (single-thread ownership)
```
```sweet-exp
;; These are Send (safe to move to threads)
int, bool, string, (Pair a b) [Send a, Send b]

;; These are Sync (safe to share via &)
int, bool, Mutex<T> [T : Sync]

;; NOT Sync (require Mutex for shared access)
Rc<T> (thread-local ref counting)
ref<T> (single-thread ownership)
```

Most library types implement these traits automatically based on their fields.

### Borrow Checking Across Threads

Turmeric's borrow checker enforces:

A closure crossing a thread boundary (via `thread-spawn`, `async`, or a
task-group spawn) is Send-checked: capturing a non-`Send` value -- a `ref<T>`,
a continuation, a borrow -- is rejected with `TUR-E0010` (not Send) or
`TUR-E0011` (not Sync) instead of racing at runtime. Copyable scalars capture
freely; shared structures cross via an Arc or a channel. See
`tests/fixtures/errors/thread-send-ref` and
`tests/fixtures/errors/thread-send-cont` for the rejected shapes.

## Common Patterns

### Producer-Consumer

```turmeric
(def ch (chan-new 16))

;; Producer
(async
  (fn []
    (for-each items
      (fn [item] (chan-send ch item)))
    (chan-send ch :done)))

;; Consumer
(let loop []
  (let [v (chan-recv ch)]
    (when (not= v :done)
      (process v)
      (loop))))

(chan-free ch)
```
```sweet-exp
def ch chan-new(16)

;; Producer
async
  (fn []
    (for-each items
      (fn [item] chan-send(ch item)))
    chan-send(ch :done))

;; Consumer
let loop []
  let [v chan-recv(ch)]
    when {v not= :done}
      process(v)
      loop()

chan-free(ch)
```

### Thread-Safe Counter

```turmeric
(def counter (atomic-new 0))

(for-each (range 10)
  (fn [i]
    (async
      (fn []
        (atomic-add! counter 1)))))

;; ... after joining the workers ...
(println (atomic-load counter))  ; => 10
```
```sweet-exp
def counter atomic-new(0)

for-each range(10)
  (fn [i]
    (async
      (fn []
        atomic-add!(counter 1))))

;; ... after joining the workers ...
println(atomic-load(counter))  ; => 10
```

### Barrier

`stdlib/barrier.tur` is a counting barrier: `barrier-wait` blocks until the
Nth caller arrives, then releases all N together and resets for the next
round. That reuse is the point -- a barrier is for **phased** work, where
every worker must finish phase 1 before any starts phase 2, which is what
separates it from a one-shot latch.

```turmeric no-check
(load "stdlib/barrier.tur")

(let [b (barrier-new 3)]
  ;; ... each of three threads calls (barrier-wait b) between phases ...
  (barrier-free b))
```

`barrier-wait` returns `true` for exactly **one** of the N threads released
per round -- the arrival that tripped it, mirroring
`PTHREAD_BARRIER_SERIAL_THREAD`. That lets a single thread do the
between-phases work (swap buffers, print a summary) without a second lock:

```turmeric no-check
(if (barrier-wait b) (println "phase complete") 0)
```

It is built from mutex + condvar rather than `pthread_barrier_t`, which is an
optional POSIX feature macOS does not ship -- the same reason
`stdlib/sync.tur` hand-rolls its semaphore instead of using `sem_t`.
Internally it is sense-reversing: each round bumps a generation counter and a
waiter sleeps until *its* generation ends, so a thread that loops back around
cannot be released by its own next round.

Free the barrier only after every waiter has been released; destroying it
while threads are parked on it destroys the condvar out from under them.

The STM alternative -- a TVar plus `check` -- is sketched in the
[STM Tutorial](stm-tutorial.md#barrier), and is the right choice when the
rendezvous needs to compose with other transactional state.

### Structured Concurrency with TaskGroup

```turmeric
;; Spawn N workers; cancel all if one fails or times out
(def g (task-group-new))
(task-group-with-timeout g 10000
  (for-each tasks
    (fn [task]
      (task-group-spawn g
        (fn []
          (when (not (task-group-should-exit? g))
            (run-task task))
          (task-group-task-done g))))))
(task-group-free g)
```
```sweet-exp
;; Spawn N workers; cancel all if one fails or times out
def g task-group-new()
task-group-with-timeout g 10000
  for-each tasks
    (fn [task]
      (task-group-spawn g
        (fn []
          (when not(task-group-should-exit?(g))
            run-task(task))
          task-group-task-done(g))))
task-group-free(g)
```

### Fan-Out with Thread Pool and Futures

```turmeric
(def tp (thread-pool-new 4))

;; Submit all tasks and collect futures
(def futures
  (map items (fn [item]
    (thread-pool-submit tp process-item item))))

;; Await all results
(for-each futures
  (fn [fut]
    (let [r (future-get fut)]
      (if (ok? r)
        (collect (ok-val r))
        (log-error (err-val r))))))

(thread-pool-shutdown tp)
(thread-pool-free tp)
```
```sweet-exp
def tp thread-pool-new(4)

;; Submit all tasks and collect futures
def futures
  map items
    (fn [item]
      thread-pool-submit(tp process-item item))

;; Await all results
for-each futures
  (fn [fut]
    (let [r future-get(fut)]
      (if ok?(r)
        collect(ok-val(r))
        log-error(err-val(r)))))

thread-pool-shutdown(tp)
thread-pool-free(tp)
```

## Web REPL (WASM) Threading Constraints

The web REPL runs Turmeric inside a WebAssembly module compiled with
Emscripten pthreads. The threading model differs from native `tur run` in a
few ways:

### Worker pool

The WASM build uses `-sPTHREAD_POOL_SIZE_STRICT=0`, which means the Worker
pool grows lazily on demand. There is no hard cap on concurrent threads.
The first time a thread is created beyond the current pool size there is
additional latency while the browser spawns a new Worker; subsequent reuse
of that Worker is fast.

This limit does not apply to native builds, which use unrestricted POSIX
threads.

### Eval Worker

All code evaluation in the browser REPL runs inside a dedicated eval Worker
(`eval-worker.js`). This is necessary because `Atomics.wait` (which
Emscripten uses for `pthread_cond_wait`) is prohibited on the browser's main
thread. The eval Worker can block freely on channel operations and `select`
without freezing the tab.

### No `pthread_cancel`

Emscripten does not implement `pthread_cancel`. Turmeric's cooperative
cancellation design (cancel flag + condvar check) is unaffected by this --
it does not call `pthread_cancel`.

### Cross-origin isolation

The site must serve `Cross-Origin-Opener-Policy: same-origin` and
`Cross-Origin-Embedder-Policy: require-corp` on every response for
`SharedArrayBuffer` (required by Emscripten pthreads) to be available in
the browser. Self-hosted deployments must set these headers; the hosted
`turmeric-lang.com` site does this via the Cloudflare Worker.

### Optimization level

The threaded WASM build uses `-O2` rather than `-O3`. Older `wasm-opt`
(Binaryen) versions can mishandle the shared-memory semantics required by
pthreads at `-O3`. Once the threaded build has been smoke-tested at `-O3`
and confirmed clean, the flag will be restored.

---

## See Also

- [Async/Await Guide](async-await-guide.md) -- Lightweight fibers for I/O concurrency
- [STM Guide](stm-guide.md) -- Software transactional memory
- [STM Tutorial](stm-tutorial.md) -- Composable transactions (alternative to locks)
- [Effects System Guide](effects-system-guide.md) -- Dependency injection and exception handling

## See also

- [mutable-globals-guide.md](mutable-globals-guide.md) -- `def ^mut`, `^atomic`, `^thread-local`, and what the concurrency annotations do not cover
