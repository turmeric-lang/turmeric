---
title: Channels and Select Guide
category: Concurrency and Async
description: Buffered channels for inter-fiber communication and multi-channel select
---

# Channels and Select Guide

Buffered channels for passing values between fibers, and `select` for
waiting on multiple channel operations at once.

## Overview

Turmeric provides two channel types in `stdlib/chan.tur`:

- **`Chan<A>`** -- a synchronous blocking channel. `chan-send` blocks when
  the buffer is full; `chan-recv` blocks when it is empty.
- **`AsyncChan<A>`** -- a buffered channel with the same blocking semantics,
  plus non-blocking `try-send` / `try-recv` variants and a `count` accessor.

Both share the same underlying ring-buffer struct (mutex, two condition
variables, and a fixed-capacity `int64_t` array). Both are `:linear` opaque
handles: each owns a heap-allocated `ChanBlock` that must be freed exactly
once with `chan-free` / `async-chan-free`.

For protocol-checked communication, `stdlib/schan.tur` wraps a `Chan` in a
session-typed handle (`SChan<P>`) whose phantom type encodes the send/recv
sequence. See [session-types-guide.md](session-types-guide.md) for the
protocol layer; this guide covers the untyped channel surface.

`stdlib/select.tur` provides the `select` form for waiting on multiple
channel operations simultaneously with fair, waiter-based blocking.

## Quick Start

```turmeric
;; Create a channel, send and receive on the same fiber
(let [ch (chan-new 4)]
  (chan-send ch 42)
  (println (chan-recv ch))  ; => 42
  (chan-free ch))
```

```sweet-exp
#lang sweet-exp

let [ch chan-new(4)]
  chan-send(ch 42)
  println(chan-recv(ch))  ; => 42
  chan-free(ch)
```

## Chan -- Synchronous Blocking Channel

### API

| Function | Type | Description |
|---|---|---|
| `chan-new` | `[cap : int] -> (Chan A)` | Allocate a channel with `cap` slots (minimum 1) |
| `chan-send` | `[^borrow ch : (Chan A), val : A] -> nil` | Enqueue a value; blocks when full |
| `chan-recv` | `[^borrow ch : (Chan A)] -> A` | Dequeue a value; blocks when empty |
| `chan-free` | `[ch : (Chan A)] -> nil` | Destroy the channel and free its memory |

`chan-send` and `chan-recv` declare their channel parameter `^borrow`, so
you can send and receive repeatedly without consuming the handle. Only
`chan-free` consumes it.

### Producer/Consumer

A channel connects a producer fiber to a consumer fiber:

```turmeric
(defn producer [ch : (Chan int)] : nil
  (chan-send ch 1)
  (chan-send ch 2)
  (chan-send ch 3))

(defn consumer [ch : (Chan int)] : int
  (+ (chan-recv ch) (chan-recv ch) (chan-recv ch)))

(let [ch (chan-new 8)]
  (producer ch)
  (println (consumer ch))  ; => 6
  (chan-free ch))
```

```sweet-exp
#lang sweet-exp

defn producer [ch : (Chan int)] : nil
  chan-send(ch 1)
  chan-send(ch 2)
  chan-send(ch 3)

defn consumer [ch : (Chan int)] : int
  {chan-recv(ch) + chan-recv(ch) + chan-recv(ch)}

let [ch chan-new(8)]
  producer(ch)
  println(consumer(ch))  ; => 6
  chan-free(ch)
```

### Blocking semantics

`chan-send` blocks (via `pthread_cond_timedwait` with a 5 ms poll interval)
until the buffer has room. `chan-recv` blocks the same way until a value is
available. Both check for thread cancellation during the wait, so a fiber
that is part of a cancelled task group will exit rather than deadlocking.

A capacity of 0 is clamped to 1. The ring buffer is a fixed-size array; it
does not grow dynamically.

## AsyncChan -- Buffered Channel with Try Variants

### API

| Function | Type | Description |
|---|---|---|
| `async-chan-new` | `[cap : int] -> (AsyncChan A)` | Allocate an async channel |
| `async-chan-send` | `[^borrow ch, val : A] -> nil` | Blocking send |
| `async-chan-recv` | `[^borrow ch] -> A` | Blocking receive |
| `async-chan-try-send` | `[^borrow ch, val : A] -> bool` | Non-blocking send; `false` if full |
| `async-chan-try-recv` | `[^borrow ch] -> A` | Non-blocking receive; `INT64_MIN` if empty |
| `async-chan-count` | `[^borrow ch] -> int` | Current item count |
| `async-chan-free` | `[ch : (AsyncChan A)] -> nil` | Destroy the channel |

### Non-blocking operations

`async-chan-try-send` returns `false` immediately if the buffer is full
instead of blocking. `async-chan-try-recv` returns `INT64_MIN` if the
buffer is empty -- compare the result against `INT64_MIN` to detect the
empty case:

```turmeric
(let [ch (async-chan-new 2)]
  (async-chan-try-send ch 10)            ; => true
  (async-chan-try-send ch 20)            ; => true
  (if (async-chan-try-send ch 30)
    (println "sent")
    (println "full -- did not send"))    ; => "full -- did not send"
  (let [v (async-chan-try-recv ch)]
    (if (= v INT64_MIN)
      (println "empty")
      (println v)))                      ; => 10
  (async-chan-free ch))
```

```sweet-exp
#lang sweet-exp

let [ch async-chan-new(2)]
  async-chan-try-send(ch 10)             ; => true
  async-chan-try-send(ch 20)             ; => true
  if async-chan-try-send(ch 30)
    println("sent")
    println("full -- did not send")     ; => "full -- did not send"
  let [v async-chan-try-recv(ch)]
    if =(v INT64_MIN)
      println("empty")
      println(v)                         ; => 10
  async-chan-free(ch)
```

## Select -- Multi-Channel Wait

The `select` form waits on multiple channel operations simultaneously and
executes the body of the first clause that becomes ready. It is a built-in
form elaborated by the compiler (not a macro in `stdlib/select.tur`); the
stdlib module provides the runtime helpers the form calls.

### Syntax

```turmeric
(select
  ((ch1 :recv v) body1)
  ((ch2 :send 42) body2)
  (:default default-body))
```

Each channel clause is a two-element list: a descriptor and a body. The
descriptor is either `(chan :recv binding)` or `(chan :send value)`. The
`:default` arm is optional; when present, it fires if no channel is ready
immediately.

`select` returns the value of the clause body that fired. All clause bodies
must be type-compatible.

### How it works

The runtime does a fair non-blocking scan (xorshift32) across all clauses
first. If a clause is ready, it fires immediately. If none is ready and
there is no `:default`, the runtime registers a waiter node on every channel
and blocks until one signals. This avoids a busy-wait: the fiber sleeps
until a `chan-send` or `chan-recv` on one of the channels wakes it.

### Example: fan-in

Read from whichever of two channels has a value first:

```turmeric
(let [a (chan-new 4)
      b (chan-new 4)]
  (chan-send a 100)
  (select
    ((a :recv x) (println x))   ; => 100
    ((b :recv y) (println y)))
  (chan-free a)
  (chan-free b))
```

```sweet-exp
#lang sweet-exp

let [a chan-new(4)
     b chan-new(4)]
  chan-send(a 100)
  select
    ((a :recv x) println(x))   ; => 100
    ((b :recv y) println(y))
  chan-free(a)
  chan-free(b)
```

### Example: send or default

Try to send on one channel, fall back if neither is ready:

```turmeric
(let [fast (chan-new 1)
      slow (chan-new 1)]
  ;; Both channels are empty, so :default fires
  (select
    ((fast :recv v) (println v))
    ((slow :recv v) (println v))
    (:default (println "no data yet")))
  (chan-free fast)
  (chan-free slow))
```

```sweet-exp
#lang sweet-exp

let [fast chan-new(1)
     slow chan-new(1)]
  ;; Both channels are empty, so :default fires
  select
    ((fast :recv v) println(v))
    ((slow :recv v) println(v))
    (:default println("no data yet"))
  chan-free(fast)
  chan-free(slow)
```

## Session-Typed Channels (SChan)

`stdlib/schan.tur` wraps a `Chan` in a phantom-typed handle that encodes the
protocol -- the sequence of send and recv steps -- at the type level. Each
operation consumes the handle and returns the continuation at the next
protocol step, so skipping a step or reordering is a compile-time error.

```turmeric
;; A protocol: send an int, then receive an int, then close
(let [c (:: (schan-new 4) (SChan (SSend int (SRecv int SClose))))]
  (let [c2 (schan-send c 42)]
    (let [p (schan-recv c2)
          v  (pair-fst p)
          c3 (pair-snd p)]
      (println v)
      (schan-close c3))))
```

```sweet-exp
#lang sweet-exp

;; A protocol: send an int, then receive an int, then close
let [c (:: (schan-new 4) (SChan (SSend int (SRecv int SClose))))]
  let [c2 schan-send(c 42)]
    let [p schan-recv(c2)
         v pair-fst(p)
         c3 pair-snd(p)]
      println(v)
      schan-close(c3)
```

See [session-types-guide.md](session-types-guide.md) for the full protocol
language.

## Linearity and Ownership

`Chan` and `AsyncChan` are `:linear` -- each owns a heap-allocated
`ChanBlock` (mutex, condvars, ring buffer) that must be freed exactly once.
Under `-Xlinear`, the checker rejects:

- **TUR-E0100** -- dropping a channel without calling `chan-free`.
- **TUR-E0101** -- using a channel after `chan-free` (double free).

The `^borrow` annotation on `chan-send` / `chan-recv` (and the async
variants) means the channel handle is read, not consumed, by those
operations. Only `chan-free` / `async-chan-free` consumes it.

`SChan<P>` is also `:linear`: each protocol step consumes the handle and
returns the continuation, so the protocol cannot be replayed or skipped.

## What This Guide Does Not Cover

- **Fiber scheduling** -- channels block fibers, but the fiber scheduler
  itself is covered in [async-await-guide.md](async-await-guide.md).
- **Thread pools** -- for fan-out work over OS threads, see
  [thread-pool-guide.md](thread-pool-guide.md).
- **Structured concurrency** -- `TaskGroup` for scoped task spawning is
  covered in [structured-concurrency-guide.md](structured-concurrency-guide.md).
- **Session-typed protocols** -- the full `SChan` protocol language is in
  [session-types-guide.md](session-types-guide.md).
