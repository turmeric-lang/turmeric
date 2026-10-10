---
title: Reactor Guide
category: Concurrency and Async
description: Lightweight evented I/O with tur/reactor
---

# Reactor Guide

`tur/reactor` is a single-threaded event loop for multiplexing file
descriptors, timers, OS signals, and channel receives without requiring
the cooperative fiber scheduler.

## When to use `tur/reactor`

Use the reactor when you need to:

- Watch one or more file descriptors in a dedicated listener thread (e.g.
  a TCP accept loop in `tur/httpd`).
- Fire callbacks after a delay or on a repeating interval without a
  background thread.
- Catch an OS signal cleanly from within an event loop.
- Bridge a cross-thread channel into a reactor loop with low latency.

If your code already runs inside a fiber and uses `async-socket-*` /
`async-file-*`, those facilities already park on the same `IOBackend`
underneath -- you do not need the reactor. The reactor is for code that
runs *outside* the fiber scheduler, or for dedicated non-fiber threads.

## Quick start

```turmeric
(load "stdlib/reactor.tur")

(defn main [] : int
  (let [r (reactor-new)]
    (reactor-add-timer r 0
      (fn [id user] : nil (println "hello from timer"))
      nil)
    (reactor-run r)
    (reactor-free r)
    0))
```

```sweet-exp
load "stdlib/reactor.tur"

defn main [] :int
  let [r reactor-new()]
    reactor-add-timer r 0
      (fn [id user] :nil
        (println "hello from timer"))
      nil
    reactor-run r
    reactor-free r
    0
```

## Programming styles

### Style 1 -- Pure callback (no scheduler needed)

The simplest pattern: register sources, run the loop, clean up.
This is the pattern used by the `tur/httpd` listener thread.

```turmeric
(load "stdlib/reactor.tur")

;; accept-loop is a plain OS thread entry point -- no fibers required.
(defn start-listener [listen-fd : int ^borrow stop-ch : (Chan int)] : nil
  (let [r (reactor-new)]
    ;; Accept connections: each READ event means accept() will not block.
    (reactor-add-fd r listen-fd READ
      (fn [id events user]
        (let [client (accept-conn listen-fd)]
          (when (not= client -1)
            (pool-submit worker-pool client))))
      nil)
    ;; Shutdown: one value on stop-ch ends the loop (one-shot).
    (reactor-add-chan r stop-ch
      (fn [id v user] : nil
        (reactor-stop r))
      nil)
    ;; Block until stopped.
    (reactor-run r)
    (reactor-free r)))
```

```sweet-exp
load "stdlib/reactor.tur"

;; accept-loop is a plain OS thread entry point -- no fibers required.
defn start-listener [listen-fd :int ^borrow stop-ch :(Chan int)] :nil
  let [r reactor-new()]
    ;; Accept connections: each READ event means accept() will not block.
    reactor-add-fd r listen-fd READ
      (fn [id events user]
        (let [client accept-conn(listen-fd)]
          (when {client not= -1}
            (pool-submit worker-pool client))))
      nil
    ;; Shutdown: one value on stop-ch ends the loop (one-shot).
    reactor-add-chan r stop-ch
      (fn [id v user] :nil
        (reactor-stop r))
      nil
    ;; Block until stopped.
    reactor-run r
    reactor-free r
```

### Style 2 -- Reactor + channels (cross-thread coordination)

A channel bridges the reactor thread and any other OS thread. The sender
calls `reactor-wake` after `chan-send` so the blocking poll returns
promptly.

```turmeric
(load "stdlib/reactor.tur")
(load "stdlib/chan.tur")

;; In the reactor thread:
(defn run-with-stop [^borrow stop-ch : (Chan int)] : nil
  (let [r (reactor-new)]
    ;; ... register fd sources, timers, etc. ...

    ;; Register one-shot shutdown watcher.
    (reactor-add-chan r stop-ch
      (fn [id v user] : nil (reactor-stop r))
      nil)

    (reactor-run r)
    (reactor-free r)))

;; From any other thread (safe because reactor-wake is thread-safe):
(defn request-shutdown [^borrow stop-ch : (Chan int) ^borrow r : Reactor] : nil
  (chan-send stop-ch 1)
  (reactor-wake r))
```

```sweet-exp
load "stdlib/reactor.tur"
load "stdlib/chan.tur"

;; In the reactor thread:
defn run-with-stop [^borrow stop-ch :(Chan int)] :nil
  let [r reactor-new()]
    ;; ... register fd sources, timers, etc. ...

    ;; Register one-shot shutdown watcher.
    reactor-add-chan r stop-ch
      (fn [id v user] :nil
        (reactor-stop r))
      nil

    reactor-run r
    reactor-free r

;; From any other thread (safe because reactor-wake is thread-safe):
defn request-shutdown [^borrow stop-ch :(Chan int) ^borrow r :Reactor] :nil
  chan-send stop-ch 1
  reactor-wake r
```

The key constraint: `reactor-add-chan` is one-shot. If you need a
persistent channel watcher, re-register from inside the callback:

```turmeric
(defn watch-chan-loop [^borrow r : Reactor ^borrow ch : (Chan int)] : nil
  (reactor-add-chan r ch
    (fn [id v user] : nil
      (handle-message v)
      ;; Re-register to watch the next value.
      (watch-chan-loop r ch))
    nil))
```

```sweet-exp
defn watch-chan-loop [^borrow r :Reactor ^borrow ch :(Chan int)] :nil
  reactor-add-chan r ch
    (fn [id v user] :nil
      (handle-message v)
      ;; Re-register to watch the next value.
      (watch-chan-loop r ch))
    nil
```

### Style 3 -- Reactor drives a local fiber group

For libraries that want fibers but don't want to take over the global
scheduler, `reactor-run-fibers` is a thin convenience layer that:

- pumps a local fiber ready-queue between `reactor-poll` calls
- maps fiber park/unpark onto `reactor-add-fd` / `reactor-add-chan` and
  source removal (one-shot, like the scheduler's park today)

You own a `Reactor` (one per thread, as always), create a
`LocalFiberGroup` bound to it, spawn fibers with `local-spawn`, and call
`reactor-run-fibers` to drive them. Inside a fiber, block with
`local-park-fd` / `local-park-chan` instead of the global-scheduler
`await`. The pump returns when the group is empty and the reactor has no
remaining sources, or when `reactor-stop` is called.

```turmeric
(load "stdlib/reactor.tur")

;; Echo one line from each of two pipe read-ends, concurrently, on a single
;; thread -- no global scheduler involved.
(defn serve [g : ptr<void> read-fd : int] : nil
  ;; Park until the fd is readable (or 5s elapses), then handle it.
  (let [ev (local-park-fd g read-fd READ 5000)]
    (if (= ev -2)
      (handle-timeout read-fd)
      (handle-readable read-fd))))

(defn main [] : int
  (let [r (reactor-new)
        g (local-fiber-group-new r)]
    (local-spawn g (fn [u] : nil (serve g conn-a)) nil)
    (local-spawn g (fn [u] : nil (serve g conn-b)) nil)
    ;; Drives both fibers: each runs until it parks on its fd, the reactor
    ;; polls, and whichever fd fires first resumes its fiber.
    (reactor-run-fibers g)
    (local-fiber-group-free g)
    (reactor-free r)
    0))
```

`local-park-fd` returns the fired event mask, `-2` on timeout, or `-1`
if called outside a group fiber. `local-park-chan` borrows a `(Chan A)`
(`local-park-async-chan` an `(AsyncChan A)`). It returns `(some v)` with the
received value, typed `A`, so a float channel yields its float, or `(none)`
outside a group fiber.

A value already in the channel when a fiber parks (or when `reactor-add-chan`
registers) is delivered on the next poll. One sent after that needs the
sender to call `reactor-wake` after `chan-send`, so that the pump's blocking
poll returns promptly. This is as with `reactor-add-chan`.

A `LocalFiberGroup` follows the same threading rules as the reactor: the
group, its reactor, and every fiber live on one thread. Freeing the
group cancels any still-parked fiber (its source is unregistered and its
stack is freed; no cleanup hooks run).

This is also the path where a future rewrite of `src/async/scheduler.c`
would land -- the scheduler becomes "the global reactor's fiber driver"
without an API break. The driver itself shipped via
`docs/archive/history/reactor-run-fibers-plan.md`; that scheduler migration is
tracked separately in
`docs/archive/scheduler-on-local-fiber-group-plan.md`.

## API reference

### Lifecycle

| Function | Description |
|---|---|
| `reactor-new` | Create a reactor for the current thread. Returns nil on WASM or init failure. |
| `reactor-free` | Free the reactor and unregister all attached sources. |

### Source registration

| Function | Returns | Notes |
|---|---|---|
| `reactor-add-fd` | source-id | Watch an fd for READ, WRITE, ERROR, or HUP. |
| `reactor-modify-fd` | 0 / -1 | Change the event mask for an existing fd source. |
| `reactor-remove` | 0 / -1 | Unregister any source by id. Does not close the fd. |
| `reactor-add-timer` | source-id | One-shot timer; auto-deactivates after firing. |
| `reactor-add-interval` | source-id | Repeating timer; stays active until `reactor-remove`. |
| `reactor-add-signal` | source-id | Catch an OS signal. Not available on WASM. |
| `reactor-add-chan` | source-id | One-shot watcher on a borrowed `(Chan A)`. Re-register for persistence. Keep the channel alive while the watch is live. |
| `reactor-add-async-chan` | source-id | The same, on a borrowed `(AsyncChan A)`. |

### Running

| Function | Notes |
|---|---|
| `reactor-poll` | Single iteration; blocks up to `timeout-ms` (-1 = forever, 0 = non-blocking). |
| `reactor-run` | Loop until stopped or all sources removed. |
| `reactor-stop` | Thread-safe. Causes `reactor-run` / `reactor-run-fibers` to return after the current dispatch round. |
| `reactor-wake` | Thread-safe. Interrupts a blocking `reactor-poll` / `reactor-run`. |

### Local fiber driver

| Function | Returns | Notes |
|---|---|---|
| `local-fiber-group-new` | group | Create a fiber group bound to a reactor. One group per reactor; not thread-safe. |
| `local-fiber-group-free` | nil | Free the group; cancels any still-parked fiber (source removed, stack freed). |
| `local-spawn` | fiber-id / -1 | Spawn a fiber `(fn [user :ptr<void>] :nil)`; runs on the next pump tick. |
| `reactor-run-fibers` | #completed / -1 | Pump the group until empty + no sources, or `reactor-stop`. -1 if re-entered. |
| `local-park-fd` | events / -2 / -1 | Park the running fiber on an fd; -2 on timeout, -1 if not in a group fiber. |
| `local-park-chan` | `(Option A)` | Park the running fiber on a borrowed `(Chan A)`; `(none)` if not in a group fiber. |
| `local-park-async-chan` | `(Option A)` | The same, on a borrowed `(AsyncChan A)`. |

### Event mask constants

| Constant | Value | Meaning |
|---|---|---|
| `READ` | 1 | fd is readable or accept is ready |
| `WRITE` | 2 | fd is writable or connect completed |
| `ERROR` | 4 | Error condition (always reported; cannot be masked) |
| `HUP` | 8 | Peer hung up or EOF |

## Callback signatures

Each source type uses a distinct callback arity:

```turmeric
;; reactor-add-fd, reactor-add-signal:
(fn [id : int events : int user : ptr<void>] : nil ...)

;; reactor-add-timer, reactor-add-interval:
(fn [id : int user : ptr<void>] : nil ...)

;; reactor-add-chan, reactor-add-async-chan:
(fn [id : int value : int user : ptr<void>] : nil ...)

;; local-spawn (fiber body):
(fn [user : ptr<void>] : nil ...)
```

`id` is the source id returned at registration. `events` is a bitmask of
`READ | WRITE | ERROR | HUP`. For signal callbacks, `events` carries the
signal number. For channel callbacks, `value` is the channel's carrier word,
dequeued for you: the int itself for a `(Chan int)`. For another element type,
leave the slot untyped and convert in the body: `(:: v cstr)` for a word-sized
element, `(bits->float v)` for a float.

Keep every callback slot untyped, `int`, or `ptr<void>`. The reactor calls a
callback as `(env, int64, int64, int64)`, and those three spellings are the
ones whose boxed C type matches exactly. A `float` slot is a compile error,
because it would read a register the reactor never loaded. A `cstr` slot
compiles and runs on x86-64 and arm64, but its C type differs from the one it
is called through, which is a `-fsanitize=function` trap.

## Threading model

A reactor is **not** thread-safe. One reactor per thread. The only
thread-safe operations are `reactor-wake` and `reactor-stop`. All source
registration, modification, and callback dispatch must happen on the
thread that calls `reactor-poll` or `reactor-run`.

To hand work across threads: use a channel and call `reactor-wake` after
`chan-send`. The channel watcher fires on the next poll iteration.

## Linking

Programs that load `stdlib/reactor.tur` must link against `libturi`:

```turmeric
;; The autolink hint is included when you (load "stdlib/reactor.tur").
;; For standalone files that only declare the tur_reactor_* externs, add:
(defn reactor-link [] : int
  ```c /* __tur_autolink__: -lturi */
  return 0;
  ```)
```

```sweet-exp
;; The autolink hint is included when you load "stdlib/reactor.tur".
;; For standalone files that only declare the tur_reactor_* externs, add:
defn reactor-link [] :int
  ```c /* __tur_autolink__: -lturi */
  return 0;
  ```
```

When running `tur build`, this is handled automatically by the
`reactor/autolink-hint` function that ships with the stdlib module.

## Platform notes

| Platform | I/O Backend | Signal backend |
|---|---|---|
| Linux | epoll | signalfd |
| macOS / BSD | kqueue | self-pipe + sigaction |
| WASM | no-op sentinel | not available |

`reactor-new` returns nil on WASM. Guard with a nil check if your code
must compile for multiple targets.

## See also

- [tur/reactor API](../api/tur-reactor.html) -- module reference and full API docstrings
- [tur/chan API](../api/tur-chan.html) -- channel creation (`chan-new`, `chan-send`, `chan-recv`)
- [Async/Await Guide](async-await-guide.md) -- fiber-based async I/O (alternative for fiber code)
