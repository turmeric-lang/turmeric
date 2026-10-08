---
title: Thread Pool
category: Concurrency and Async
description: Bounded POSIX worker-thread pool with back-pressure for fan-out work over ptr<void> items
audience: developers running CPU- or IO-bound work across a fixed set of OS threads
since: thread-pool v0.1.0
---

# Thread Pool

`tur-thread-pool` is a bounded, fixed-size POSIX worker-thread pool built
on top of `pthread_mutex_t` and `pthread_cond_t`. Workers pull
`ptr<void>` work items off a single ring buffer and hand each item to a
user-supplied callback. The producer blocks (rather than dropping work)
when the queue is saturated, giving cooperative back-pressure.

This is the generic core extracted from `tur-httpd`'s old `httpd/pool`,
which previously hard-coded a connection-fd item type and an HTTP
worker dispatch. The HTTP pool is now a thin adapter over this spice;
the same primitive is suitable for any "one producer, N workers,
opaque work item" pattern.

## TL;DR

- Three functions: `pool-new`, `pool-submit`, `pool-stop`.
- Work items are `ptr<void>`; the pool never inspects them.
- A single `void (*)(void *item)` callback handles every item.
- Submit blocks under back-pressure; stop drains every queued item
  before joining workers.
- Out of scope for v0.1.0: per-task futures, priority lanes, dynamic
  resize, work-stealing.

## Installation

Add the spice to your `build.tur` `:spices` map (pin to a specific
ref in real projects):

```turmeric
(defpackage my-app
  :name    "my-app"
  :version "0.1.0"
  :spices  #{
    "thread-pool" #{:url    "https://github.com/turmeric-lang/turmeric-spices"
                    :ref    "main"
                    :subdir "spices/thread-pool"}
  })
```

Then import the module:

```turmeric
(import thread-pool/pool :as tp)
```

## Creating a pool

```turmeric
(tp/pool-new size : int callback : ptr<void>) : ptr<void>
```

`pool-new` allocates the ring buffer, spawns `size` worker threads, and
returns an opaque pool handle. The handle is `NULL` if allocation or
every thread spawn failed -- always check before submitting.

- `size` is clamped to `>= 1`. Passing `0` or a negative value gives you
  exactly one worker rather than an error.
- The ring buffer capacity is `max(size * 4, 64)` items. With four
  workers the queue holds 64 items; with thirty-two workers it holds
  128. The formula matches the original `httpd/pool` design.
- `callback` is a `void (*)(void *item)` function pointer, smuggled in
  as a `ptr<void>`. It must remain valid (i.e. the function must not be
  unloaded) for the full lifetime of the pool.

A minimal scaffold:

```turmeric
(defmodule example
  (import thread-pool/pool :as tp)

(defn work-cb-ptr [] : ptr<void>
  ```c
  extern void example__work_hycb(void *);
  return (void *)example__work_hycb;
  ```)

(defn work-cb [item : ptr<void>] : void
  ```c
  int *v = (int *)item;
  /* ... do work with *v ... */
  free(v);
  ```)

(defn main [] : int
  (let [pool (tp/pool-new 4 (work-cb-ptr))]
    (do
      ;; ... (tp/pool-submit pool item) ...
      (tp/pool-stop pool)
      0)))
)
```

### Why the callback is exposed via `extern`

The callback's address is taken in one translation unit (`work-cb-ptr`)
and consumed in another (the pool's worker loop). A `static` C function
-- which is what a plain `defn` body typically lowers to -- would not
be reachable across translation units, so the callback must be a
non-static (external) symbol. Declaring it with `extern void
example__work_hycb(void *);` inside `work-cb-ptr` forces the linker to
resolve the address rather than constant-fold a stale local copy.

This is the same trick `httpd/server` uses to expose
`srv_worker_loop_trampoline` to `httpd/pool`. If you forget the
`extern`, the symptom is a link error rather than runtime breakage.

## Submitting work

```turmeric
(tp/pool-submit pool : ptr<void> item : ptr<void>) : void
```

Each `item` is an opaque pointer the pool never inspects. Allocate it
before the call; the callback that eventually receives it owns it and
must free it.

```turmeric
;; make-item -- allocate an int work item carrying value `v`.
(defn make-item [v : int] : ptr<void>
  ```c
  int *p = (int *)malloc(sizeof(int));
  if (p) *p = (int)v;
  return (void *)p;
  ```)

(tp/pool-submit pool (make-item 42))
```

### Back-pressure

If the ring buffer is full, `pool-submit` blocks on the pool's
`not-full` condition variable until a worker dequeues an item. There
is no drop policy, no overflow buffer, and no timeout -- the producer
simply waits.

This is intentional: the pool is designed for a single producer thread
that paces itself against worker throughput. If you need
fire-and-forget submission with overflow handling, layer your own
bounded queue or rate limiter in front of `pool-submit`.

### What you do NOT get back

`pool-submit` returns `:void`. There is no future, no promise, and no
correlation ID. The pool's contract is purely fan-out: results, error
signalling, and completion notification are entirely the caller's
problem. The usual pattern is to bake whatever the callback needs to
report results -- a mutex-guarded accumulator, an output channel, a
completion counter -- into the item itself, or to reach those values
through shared statics inside the callback's translation unit.

The test suite shows a minimal version of the latter approach: a
`static pthread_mutex_t` guarding a `long g_sum` and `long g_count` that
the callback updates per item, with `acc-sum` / `acc-count` accessors
exposed to the Turmeric layer for assertions:

```turmeric
```c
#include <pthread.h>
static pthread_mutex_t g_mu    = PTHREAD_MUTEX_INITIALIZER;
static long            g_sum   = 0;
static long            g_count = 0;

void tp_test_work_cb(void *item) {
  int *v = (int *)item;
  pthread_mutex_lock(&g_mu);
  g_sum   += (long)(*v);
  g_count += 1;
  pthread_mutex_unlock(&g_mu);
  free(v);
}
```
```

## Awaiting and shutdown

```turmeric
(tp/pool-stop pool : ptr<void>) : void
```

`pool-stop` sets the pool's `stop` flag, broadcasts both condition
variables to wake any blocked workers and producers, and then joins
every worker before freeing the pool's resources.

Two important guarantees:

1. **Drain-on-stop.** Workers do not exit on the stop signal alone --
   they only return once the queue is empty AND `stop` is set. Every
   item that was successfully submitted before `pool-stop` began will
   be passed to the callback exactly once.
2. **Join-on-return.** When `pool-stop` returns, every worker thread
   has been `pthread_join`ed and the pool's mutex, condition variables,
   ring buffer, and worker array have all been freed.

`pool-stop` on a `NULL` handle is a no-op, so it is safe to call after
a failed `pool-new`.

### Ordering relative to the producer

Calling `pool-submit` on a handle that is already inside `pool-stop`
is a use-after-free hazard: by the time the worker runs the callback,
the pool struct may be gone. The rule is simple:

> Always join the producer thread (or otherwise establish that no more
> `pool-submit` calls are in flight) **before** calling `pool-stop`.

When the producer is the main thread itself -- the common case for
batch jobs -- this is automatic: control returns from the last
`pool-submit` strictly before `pool-stop` runs.

If the pool is being stopped concurrently from a different thread, the
implementation does have a fallback: `pool-submit` notices the `stop`
flag inside the critical section and silently discards the item
(without invoking the callback) rather than touching freed memory.
**That fallback exists to make shutdown safe, not to license
post-stop submissions** -- a discarded item leaks because the
producer no longer has a handle to free it.

## Lifetime discipline

The pool handle is a raw `ptr<void>`, not a linear `defopaque`, so the
type system will not enforce the discipline for you. The contract you
must uphold by construction:

- Every `pool-new` is paired with exactly one `pool-stop`. Forgetting
  `pool-stop` leaks every worker thread and the ring buffer; calling
  it twice is a double-free.
- Items submitted to the pool become owned by the callback. Do not
  read, write, or free the item from the producer after `pool-submit`
  returns.
- The callback is shared across every worker thread. Anything it
  touches that is not stack-local must be protected by a mutex, an
  atomic, or a per-worker partition.
- The callback function pointer must outlive the pool. Inline-C
  callbacks compiled into the same binary are fine; anything obtained
  by `dlsym` must keep the handle alive at least until `pool-stop`
  returns.

A future revision may wrap the handle in a `defopaque` newtype to make
double-stop a type error; for now, keep the lifecycle on a single
control path (often `let` + nested `do`).

## A complete worked example

This mirrors test case 1 from the spice's own suite -- 10 unit items,
4 workers, drain-and-stop, assert the accumulator:

```turmeric
(defmodule example
  (import thread-pool/pool :as tp)

```c
#include <stdlib.h>
#include <pthread.h>
static pthread_mutex_t g_mu    = PTHREAD_MUTEX_INITIALIZER;
static long            g_sum   = 0;
static long            g_count = 0;

void ex_work_cb(void *item) {
  int *v = (int *)item;
  pthread_mutex_lock(&g_mu);
  g_sum   += (long)(*v);
  g_count += 1;
  pthread_mutex_unlock(&g_mu);
  free(v);
}
```

(defn work-cb-ptr [] : ptr<void>
  ```c
  extern void ex_work_cb(void *);
  return (void *)ex_work_cb;
  ```)

(defn acc-count [] : int
  ```c
  long v;
  pthread_mutex_lock(&g_mu);
  v = g_count;
  pthread_mutex_unlock(&g_mu);
  return (int)v;
  ```)

(defn make-item [v : int] : ptr<void>
  ```c
  int *p = (int *)malloc(sizeof(int));
  if (p) *p = (int)v;
  return (void *)p;
  ```)

(defn submit-n [pool : ptr<void> n : int] : void
  (when (> n 0)
    (do
      (tp/pool-submit pool (make-item 1))
      (submit-n pool (- n 1)))))

(defn main [] : int
  (let [pool (tp/pool-new 4 (work-cb-ptr))]
    (do
      (submit-n pool 10)
      (tp/pool-stop pool)
      (if (= (acc-count) 10)
        (do (println "ok") 0)
        (do (println "FAIL") 1)))))
)
```

## Composing with other primitives

The thread pool deliberately keeps its interface narrow, which makes
it easy to combine with other concurrency tools without coupling:

- **As a worker stage behind a channel.** Drain a channel in the
  producer thread and call `pool-submit` for each value. Back-pressure
  on the pool transparently back-pressures the channel because the
  drain loop stops calling `recv` while it is blocked inside
  `pool-submit`.
- **As the dispatcher behind an accept loop.** This is exactly the
  `tur-httpd` pattern: a single accept thread submits connection
  handles, the pool fans out to per-request handlers.
- **As a CPU-bound stage in a multi-stage pipeline.** Earlier stages
  feed items; the callback writes results into the next stage's queue
  (or onto a shared accumulator with its own synchronization).

The spice does not currently ship adapters for channels, sessions, or
async runtimes -- composition is done by hand at the callback boundary.

## Limitations

Documented honestly from the v0.1.0 source:

- **No per-task futures or results.** `pool-submit` returns `:void`.
  Results, errors, and completion signalling are entirely the
  callback's responsibility.
- **One callback per pool.** Every item goes to the same
  `void (*)(void *item)`. Heterogeneous work means encoding a tag
  inside the item and dispatching inside the callback.
- **Single FIFO queue.** No priority lanes, no fairness controls.
- **Fixed worker count.** No dynamic resize and no work-stealing
  between pools.
- **Unbounded blocking on `pool-submit`.** No timeout, no try-submit,
  no overflow callback. Pace the producer or layer a queue in front.
- **Raw `ptr<void>` handle.** No linear-type enforcement of the
  one-stop-per-new discipline (see "Lifetime discipline" above).
- **POSIX only.** Built on `pthread_mutex_t` and `pthread_cond_t`; no
  Windows backend.
- **Residual items on a worker-spawn failure path are dropped.**
  If every spawn fails, `pool-new` itself returns `NULL` and frees
  the ring; the documented "every submitted item is delivered exactly
  once" guarantee only holds once `pool-new` returns a non-null handle
  (which implies at least one worker is draining).

If you need any of the missing features above, build them in your
own layer above this primitive rather than forking the spice -- the
intent of v0.1.0 is to be the smallest sharp tool that the HTTP
server, batch jobs, and ad-hoc fan-out can all reuse.
