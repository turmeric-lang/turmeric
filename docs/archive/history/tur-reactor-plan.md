# Plan: `tur/reactor` -- Lightweight Evented Reactor

> **Status:** Draft Plan
> **Last Updated:** 2026-05-29
> **Type:** Stdlib Design + Implementation Roadmap
> **Related:**
> - `src/async/io.h`, `src/async/io_epoll.c`, `src/async/io_kqueue.c` (native backend already in tree)
> - `src/async/scheduler.c`, `src/async/timer_wheel.c` (fiber scheduler uses the same backend)
> - `stdlib/async_socket.tur`, `stdlib/async_file.tur`, `stdlib/async_pipe.tur` (fiber-blocking I/O)
> - `stdlib/select.tur`, `stdlib/chan.tur` (channel select; complementary, not redundant)
> - `docs/tur-httpd-plan.md` (first consumer)
> - `docs/guides/async-await-guide.md`

---

## Overview

`tur/reactor` is a single-threaded, lightweight evented reactor that lets
user code multiplex multiple file descriptors, timers, and channel signals
in one loop. It is the missing piece between Turmeric's low-level I/O
backend (`src/async/io.{c,h}`) and the high-level fiber-blocking helpers
(`async-socket-*`, `async-file-*`): a *reusable, user-facing* event loop
that doesn't require buying into the cooperative scheduler and doesn't
require each spice to reinvent `epoll`/`kqueue`/`select` boilerplate in
inline-C.

The first consumer is `tur/httpd` -- where the listener thread today is
forced into thread-per-conn or pool-per-conn because there is no shared
reactor it can multiplex on. Other near-term consumers: a future
`tur/websockets`, a future `tur/sse` spice, `tur/repl --watch` (file
descriptors + timers), and any spice that needs to select between a
socket, a timer, and a wake pipe.

Goal: zero new C dependencies. Use what's already in `src/async/io.h`
(epoll on Linux, kqueue on BSD/macOS; wepoll vendored as two files for
Windows; no-op sentinel for WASM). **libuv is not required** and is explicitly rejected for
the initial milestone -- the existing `IOBackend` already does what libuv
does for our needs (FDs + timers + cross-thread wake), and libuv would
roughly double the build surface area for marginal gain. We leave the
door open: the `tur/reactor` API is deliberately small enough that a
libuv-backed implementation could be swapped in later without breaking
callers.

---

## Why not just reuse the fiber scheduler?

The cooperative scheduler in `src/async/scheduler.c` already drives
`IOBackend` and is what `async-socket-accept`/`-recv`/`-send` park on.
That's the right tool when:

- The caller is happy to write code as straight-line fiber code (`spawn`,
  `await`, blocking-looking calls).
- The caller wants the scheduler to handle work-stealing, timers, and
  cross-fiber channels for them.

It's the wrong tool when:

- The caller is on an OS thread (e.g. `tur/httpd`'s listener thread) and
  doesn't want the whole program forced into fiber-mode.
- The caller wants explicit callback or pull-style control over the loop
  (one-shot poll, custom event ordering, integration with an existing
  blocking loop).
- The caller wants to embed event handling inside a library that should
  work with **or without** the global scheduler running.

`tur/reactor` is the layer below the scheduler. The scheduler can (and
should, in a follow-up) be rewritten in terms of it, but that's not the
delivery of this plan.

---

## What exists today

| Layer | File | Audience | Style |
|---|---|---|---|
| Backend vtable | `src/async/io.h` | C only | callback-per-fd, `poll(timeout_ms)` |
| epoll backend | `src/async/io_epoll.c` | C only | -- |
| kqueue backend | `src/async/io_kqueue.c` | C only | -- |
| Timer wheel | `src/async/timer_wheel.c` | C only | -- |
| Emitted preamble (`tur_io_register`) | `src/compiler/emit_module.c` ~L2478 | runtime only | `select()`-based; tightly coupled to `FiberBlock` |
| Fiber-blocking sockets | `stdlib/async_socket.tur` | Turmeric, scheduler-mode only | parks current fiber |
| Channel select | `stdlib/select.tur`, `stdlib/chan.tur` | Turmeric | waits on channels, not fds |

The reactor sits *next to* the channel select -- they don't compose
today, and a `tur/httpd` request handler that wants to "wait for either a
client message or a shutdown channel" has no clean way to express it.
This plan makes that composition possible.

---

## API sketch

`tur/reactor` lives in `stdlib/reactor.tur`. It is one new module. The
underlying C primitives (`IOBackend`, `timer_wheel`) already exist; the
new C surface is a thin set of bindings (`tur_reactor_*`) that mirror the
public Turmeric API one-to-one. This keeps the audit surface small.

### Types

```turmeric
;; Opaque reactor handle. Threads do NOT share a Reactor instance -- create one
;; per thread that needs to poll. Cross-thread wakeup is the explicit `reactor-wake`.
(defopaque Reactor)

;; Bitmask of events to watch / events that fired
(def READ      1)   ;; readable / accept ready
(def WRITE     2)   ;; writable / connect complete
(def ERROR     4)   ;; error condition (always reported, you cannot mask it out)
(def HUP       8)   ;; peer hung up / EOF
```

### Lifecycle

```turmeric
;; Create a reactor for the current thread.
;; Backend is chosen at C compile time (epoll / kqueue / select fallback).
(defn reactor-new [] : Reactor ...)

;; Free a reactor; unregisters everything still attached.
(defn reactor-free [r : Reactor] : nil ...)
```

### Source registration

A *source* is anything that can fire an event. The reactor uniformly
represents sources by an `int64_t` id returned at registration time so
callers don't have to track FD-vs-timer-vs-channel separately.

```turmeric
;; Watch a file descriptor. Returns a source id (>= 0), -1 on error.
;; `cb` is called with (id, events, user-data) each time the fd is ready.
;; Events are level-triggered to match epoll's default and kqueue.
(defn reactor-add-fd
  [r  : Reactor
   fd : int
   events : int                       ;; READ | WRITE bitmask
   cb : fn<:int :int :ptr<void> :nil>
   user-data : ptr<void>] : int ...)

(defn reactor-modify-fd [r : Reactor id : int new-events : int] : int ...)
(defn reactor-remove   [r : Reactor id : int] : int ...)

;; One-shot timer; fires once at now+ms.
(defn reactor-add-timer
  [r : Reactor
   ms : int
   cb : fn<:int :ptr<void> :nil>
   user-data : ptr<void>] : int ...)

;; Repeating timer; fires every `interval-ms`, first fire at +`first-ms`.
(defn reactor-add-interval
  [r : Reactor
   first-ms : int
   interval-ms : int
   cb : fn<:int :ptr<void> :nil>
   user-data : ptr<void>] : int ...)

;; Watch an OS signal. Returns a source id (>= 0), -1 on error.
;; `cb` is called with (id, signum, user-data) when the signal fires.
;; Implemented via signalfd on Linux and EVFILT_SIGNAL on kqueue (macOS/BSD).
;; The signal is blocked from normal delivery while registered -- unregister
;; to restore default disposition. Not supported on WASM (returns -1).
(defn reactor-add-signal
  [r  : Reactor
   signum : int
   cb : fn<:int :int :ptr<void> :nil>
   user-data : ptr<void>] : int ...)
```

### Running

```turmeric
;; Block up to `timeout-ms` (-1 = forever, 0 = non-blocking poll).
;; Dispatches every ready callback before returning.
;; Returns the number of callbacks dispatched.
(defn reactor-poll [r : Reactor timeout-ms : int] : int ...)

;; Run until `reactor-stop` is called or no sources remain.
(defn reactor-run [r : Reactor] : nil ...)

;; Ask reactor-run to return after the current dispatch round.
;; Safe to call from a callback or from another thread.
(defn reactor-stop [r : Reactor] : nil ...)

;; Wake a reactor blocked in poll/run from another thread.
;; Implemented via the existing `IOBackend::wake` (eventfd / kqueue user event).
(defn reactor-wake [r : Reactor] : nil ...)
```

### Channel bridge

The bridge is what makes "select between a socket and a shutdown
channel" expressible in pure Turmeric:

```turmeric
;; Register a channel-recv as a source. The reactor fires `cb` once a
;; value is available, then unregisters the source (one-shot, like
;; epoll's EPOLLONESHOT). For long-lived channel watchers, re-register
;; from the callback. Internally, this uses the existing chan waiter
;; mechanism + a per-reactor eventfd / self-pipe.
(defn reactor-add-chan
  [r : Reactor
   ch : Chan<:int>
   cb : fn<:int :int :ptr<void> :nil>      ;; (id, value, user)
   user-data : ptr<void>] : int ...)
```

The channel bridge implementation is the *only* substantive new C code
in this plan (~80 lines): we extend `TurChan` with a new waiter kind
`TUR_CHAN_WAITER_REACTOR` that, when its slot is satisfied, writes a
byte to the reactor's wake fd along with the source id; the reactor's
own poll loop picks the wake fd up like any other readable fd and
dispatches the queued callback. No new dependency, no rewrite of
`stdlib/chan.tur`.

---

## Programming styles supported

### 1. Pure callback (no scheduler needed)

`tur/httpd` listener thread, no fibers required:

```turmeric
(def r (reactor-new))

(reactor-add-fd r listen-fd READ
  (fn [id events user]
    (let [client (accept listen-fd)]
      (handle-client-on-pool client)))
  0)

(reactor-add-fd r shutdown-pipe-read READ
  (fn [id events user]
    (reactor-stop r))
  0)

(reactor-run r)
(reactor-free r)
```

### 2. Reactor + channels (cross-thread coordination)

```turmeric
(def stop (chan 1))

;; from another thread:
(chan-send stop 1)
(reactor-wake r)

;; in the reactor thread:
(reactor-add-chan r stop
  (fn [id v user] (reactor-stop r))
  0)
(reactor-run r)
```

### 3. Reactor drives a *local* fiber group

For libraries that want fibers but don't want to take over the global
scheduler (e.g. an embedded `tur/httpd` instance inside a larger app),
`reactor-run-fibers` is a thin convenience layer (later milestone -- not
in the initial cut) that:

- pumps a local fiber queue between `reactor-poll` calls
- maps fiber park/unpark to reactor-add-fd / source-remove

This is the path where a future rewrite of `src/async/scheduler.c` would
land -- the scheduler becomes "the global reactor's fiber driver." Out of
scope for the initial milestone; flagged here so the API stays
forward-compatible.

---

## Threading model

- A `Reactor` is **not** thread-safe. Each thread that wants to poll
  owns its own.
- The **only** thread-safe operations are `reactor-wake` and
  `reactor-stop` (both of which just signal the wake fd / set an atomic
  flag).
- All source registration / mutation / callback dispatch happens on the
  thread that calls `reactor-poll` / `reactor-run`.

This matches what we do today with `IOBackend` and is what tur/httpd
actually wants (a single listener thread, a pool of worker threads, each
worker owning its own reactor if it wants per-connection multiplexing).

For libraries that genuinely need multi-reactor coordination (e.g. a
worker hands a connection back to the listener), the pattern is "cross
the boundary via a channel + `reactor-wake`," exactly the same pattern
the scheduler uses internally today.

---

## Integration with `tur/httpd`

Replaces this in `docs/tur-httpd-plan.md`:

> Each accepted connection is handled on a new OS thread. A global
> `Mutex<accept-queue>` serializes `accept()` so only one thread calls it.

With:

> The listener thread runs a `tur/reactor` loop with the listen fd
> registered for READ. On `accept()` ready, the listener pushes the
> connection onto the worker pool's `Mutex<Queue<socket>>` (unchanged
> from the current plan) and calls `reactor-wake` on one worker.
> Optionally, each worker also runs its own reactor so a single worker
> can multiplex many keep-alive connections in the keep-alive
> milestone.

This makes the keep-alive milestone (currently flagged in the httpd
plan's Open Questions) a straightforward addition rather than an API
break.

---

## Backend mapping

| Reactor API | epoll | kqueue | wepoll (Windows) | select fallback (WASM / pre-wepoll) |
|---|---|---|---|---|
| `reactor-add-fd READ`  | `EPOLLIN` | `EVFILT_READ`  | `EPOLLIN`  | `FD_SET(rfds)` |
| `reactor-add-fd WRITE` | `EPOLLOUT` | `EVFILT_WRITE` | `EPOLLOUT` | `FD_SET(wfds)` |
| timer | `timerfd` (Linux 2.6.25+) or wheel | `EVFILT_TIMER` | timer wheel + capped timeout | timer wheel + capped timeout |
| wake | `eventfd` | `EVFILT_USER` | self-pipe (loopback socket) | self-pipe |
| channel bridge | reuses wake fd | reuses wake fd | reuses wake fd | reuses wake fd |
| level-triggered semantics | default | default | default | natural |
| edge-triggered (post-v1) | optional | optional | **unsupported** | n/a |

`src/async/io_epoll.c` and `src/async/io_kqueue.c` already implement
everything in rows 1, 2, and 4. The timer wheel exists
(`src/async/timer_wheel.c`). The channel bridge is the only genuinely
new C code.

### Windows: wepoll over a raw IOCP rewrite

For the Windows backend we plan to use [wepoll](https://github.com/piscisaureus/wepoll),
which emulates the epoll API on top of IOCP (by talking to `\Device\Afd`
directly). This gives readiness semantics *and* IOCP-grade scalability while
keeping the reactor model -- so `io_epoll.c` -> `io_wepoll.c` is roughly
`#include "wepoll.h"` plus the fd-type fix below, instead of a separate
proactor backend.

Trade-off summary:

- `select()` -- portable, sockets-only, O(n), `FD_SETSIZE` ceiling. Fine as a
  stopgap; doesn't scale.
- **wepoll** -- real scaling, keeps the reactor loop, `io_epoll.c` reusable.
  Sockets only (no files, pipes, consoles -- matches our Windows scope).
  No `EPOLLET` (we're level-triggered in v1 anyway). Vista+.
- IOCP / libuv -- best Windows fit on paper, but requires designing the whole
  reactor around completions. Rejected for the same reasons libuv is (see
  Overview).

**Build integration.** wepoll ships as two files (`wepoll.c` + `wepoll.h`) on
its `dist` branch with no `CMakeLists.txt`. Vendor them under
`src/async/vendor/wepoll.{c,h}` rather than fetching via CPM -- two files that
change ~once a year don't benefit from a fetch step, and it keeps the "zero
new external deps" claim literally true.

### The one real Windows gotcha: `SOCKET` is not `int`

On 64-bit Windows, sockets are `SOCKET` (a `UINT_PTR`), not small int fds, and
`epoll_create` returns a `HANDLE` (void*), not an int. wepoll's `epoll_data_t`
even adds a `SOCKET sock` member for this. Anywhere `IOBackend` /
`io_epoll.c` stores an fd as `int`, it silently truncates on Windows -- this
is exactly the bug that made libev refuse a real Windows backend.

Fix: introduce a platform fd typedef wide enough to hold a `SOCKET`, leaving
the Unix backends untouched.

```c
// src/async/platform_fd.h
#ifdef _WIN32
  typedef intptr_t platform_fd_t;   // holds a SOCKET / HANDLE
#else
  typedef int      platform_fd_t;   // ordinary unix fd
#endif
```

With that in place, `io_wepoll.c` is close to a thin sibling of `io_epoll.c`.
If fds are hardcoded `int` anywhere in `IOBackend` / `src/async/`, widening
that type is the main porting cost to budget for the Windows milestone.

---

## Module layout

```
stdlib/
  reactor.tur         -- public API + thin inline-C bindings into tur_reactor_*
src/async/
  reactor.h           -- C surface for the Turmeric bindings
  reactor.c           -- composes IOBackend + timer_wheel + chan bridge
                         (~250 lines; no new external deps)
tests/fixtures/
  reactor-fd-readable/        -- pipe-pair smoke test
  reactor-timer/              -- one-shot + interval
  reactor-wake-cross-thread/  -- wake from another OS thread
  reactor-signal/             -- SIGINT fires reactor-add-signal callback
  reactor-chan-bridge/        -- reactor-add-chan fires correctly
  reactor-stop-from-callback/ -- reactor-stop inside a callback unwinds cleanly
```

---

## Phases

| Step | Task |
|---|---|
| R1 | `src/async/reactor.{h,c}`: thin wrapper over `IOBackend`; FD register / modify / remove / poll / wake / stop |
| R2 | `stdlib/reactor.tur`: bindings for R1; opaque `Reactor`, READ/WRITE/ERROR/HUP constants, fd API |
| R3 | Fixture tests: FD readable, FD writable, modify, remove (pipe-pair, no socket dep) |
| R4 | Timer support: one-shot + interval (delegate to `timer_wheel.c`, integrate into the poll timeout calculation) |
| R5 | Cross-thread `reactor-wake` + `reactor-stop`; signal handling via `reactor-add-signal` (`signalfd` on Linux, `EVFILT_SIGNAL` on kqueue); fixtures: wake from a second OS thread, `SIGINT` fires callback |
| R6 | Channel bridge: extend `TurChan` waiter kinds with `TUR_CHAN_WAITER_REACTOR`; `reactor-add-chan` |
| R7 | Migrate `tur/httpd` listener thread to `tur/reactor`; update `docs/tur-httpd-plan.md` to reference this plan |
| R8 | Docstrings (`;;;` blocks for every exported defn), `just docs`, `docs/guides/reactor-guide.md` with three styles example |
| R9 | (Stretch) `reactor-run-fibers` convenience driver for a local fiber group; sets up the rewrite of `scheduler.c` in a follow-up |

R1-R3 are the MVP and unblock `tur/httpd`'s keep-alive milestone. R4-R6
round out the API. R7 is a small migration. R8 is shipping discipline.
R9 is explicitly out-of-scope and listed only to anchor the API.

---

## Non-goals

- **No libuv.** See Overview.
- **No multi-reactor work-stealing.** Each reactor is single-threaded;
  cross-reactor work is explicit via channels + wake.
- **No protocol layer.** This is the I/O multiplexer only; HTTP framing,
  WebSocket frames, etc. live in their consumer spices.
- **No edge-triggered mode in v1.** Level-triggered matches both
  `IOBackend` defaults and the way `tur/httpd` wants to read; we can add
  an `EDGE` flag later if a consumer needs it.
- **No async DNS.** `getaddrinfo` stays blocking; resolve before
  registering with the reactor. (Future: a thread-pool-backed
  `reactor-resolve` can be added without an API break.)

---

## Open questions

- **Signal handling.** Resolved: `reactor-add-signal` is included in R5.
  Uses `signalfd` on Linux and `EVFILT_SIGNAL` on kqueue (macOS/BSD).
  The signal is blocked from normal delivery while registered; callers
  restore disposition by removing the source. WASM no-ops. This gives
  `tur/httpd` a clean `SIGINT` story without signal-handler globals or
  a self-pipe workaround.
- **Timer resolution.** Resolved for v1: always use `timer_wheel.c`
  (ms-granular) for portability. Post-v1: if a consumer needs sub-ms
  accuracy, conditionally delegate to `timerfd` on Linux or
  `EVFILT_TIMER` on kqueue, with the wheel as the fallback for other
  platforms. Not needed for the current consumer set (httpd, websockets,
  sse).
- **WASM.** Resolved: `reactor-new` returns `nil` on WASM (no silent
  sentinel). All other reactor operations that receive a `nil` reactor
  return `-1` / `nil` and set an error, so callers are forced to handle
  the unsupported case explicitly rather than silently doing nothing.
  This diverges from the existing async-socket no-op pattern -- that
  pattern is considered a mistake; don't propagate it here.
- **Windows.** Resolved: `io_wepoll.c` backed by vendored
  `src/async/vendor/wepoll.{c,h}` (two files, no new external deps).
  fd storage widened to `platform_fd_t` (`intptr_t` on Windows, `int`
  on Unix) via `src/async/platform_fd.h`. No select fallback, no IOCP
  proactor. Tracked alongside the larger Windows-support effort in
  `docs/upcoming/windows-support-plan.md`.
