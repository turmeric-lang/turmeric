# Plan: `tur/httpd` -- Lightweight HTTP Server

> **Status:** Draft Plan
> **Last Updated:** 2026-05-30
> **Type:** Stdlib Design + Implementation Roadmap
> **Related:**
> - `docs/archive/history/tur-reactor-plan.md` -- event loop backing the listener thread (R1-R8 shipped)
> - `docs/archive/history/reactor-run-fibers-plan.md` -- local fiber driver on top of the reactor (shipped, F1-F8)
> - `stdlib/reactor.tur` -- `tur/reactor` module (R1-R6 complete)
> - `src/async/io.h`, `src/async/io_epoll.c`, `src/async/io_kqueue.c` -- platform I/O backend
> - `stdlib/chan.tur`, `stdlib/select.tur` -- cross-thread coordination

---

## Overview

`tur/httpd` is a minimal, single-process HTTP/1.1 server for use in
Turmeric applications and spices. The design goal is the smallest
possible surface area that lets a `tur/httpd` instance:

- Accept TCP connections on one or more ports.
- Dispatch each request to a user-supplied handler closure.
- Support keep-alive connections efficiently (R8 milestone, see below).
- Integrate cleanly with the rest of the Turmeric stdlib (channels,
  reactors, typed structs).

---

## Architecture

### Listener thread

The listener thread runs a `tur/reactor` loop:

```turmeric
(import reactor)

(defn httpd-listener [listen-fd : int stop-ch : ptr<void>] : nil
  (let [r (reactor-new)]
    ;; Accept loop: each ready event calls accept() and hands the
    ;; socket to the worker pool.
    (reactor-add-fd r listen-fd READ
      (fn [id events user]
        (let [client (accept-conn listen-fd)]
          (when (!= client -1)
            (pool-submit worker-pool client))))
      nil)
    ;; Shutdown: a value on stop-ch ends the loop.
    (reactor-add-chan r stop-ch
      (fn [id v user]
        (reactor-stop r))
      nil)
    (reactor-run r)
    (reactor-free r)))
```

This replaces the earlier design where `accept()` was called directly
in a blocking loop or required a `Mutex<accept-queue>` to serialise
access from multiple threads.

### Worker pool

Worker threads receive connected sockets via a `Mutex<Queue<socket>>`
(unchanged from the original design). Each worker thread reads the full
request, calls the user handler, writes the response, and closes the
socket (HTTP/1.0 semantics until keep-alive is implemented).

Optionally, in the keep-alive milestone (H3), each worker owns its own
`tur/reactor` instance so a single worker can multiplex many idle
keep-alive connections without burning a thread per connection.

### Shutdown

Shutdown is signalled via the `stop-ch` channel. The main thread (or
a signal handler registered with `reactor-add-signal`) sends a value to
`stop-ch`:

```turmeric
(chan-send stop-ch 1)
(reactor-wake listener-reactor)
```

The listener's `reactor-add-chan` source fires on the next poll, calls
`reactor-stop`, and the listener loop exits cleanly.

---

## API sketch

```turmeric
;; Create an httpd instance.
(defn httpd-new [port : int handler : fn<Request :nil>] : Httpd ...)

;; Start listening. Blocks until httpd-stop is called.
(defn httpd-run [h : Httpd] : nil ...)

;; Ask httpd-run to return after the current request batch.
;; Thread-safe (delegates to reactor-stop on the listener reactor).
(defn httpd-stop [h : Httpd] : nil ...)

;; Free all resources.
(defn httpd-free [h : Httpd] : nil ...)
```

### Request and Response types

```turmeric
(defstruct Request
  [method  : cstr
   path    : cstr
   version : cstr
   headers : ptr<void>   ;; vec of (name, value) pairs
   body    : cstr])

(defstruct Response
  [status  : int
   headers : ptr<void>   ;; vec of (name, value) pairs
   body    : cstr])
```

---

## Phases

| Step | Task |
|---|---|
| H1 | TCP listener: `bind` + `listen` + `accept`; plain blocking thread-per-connection |
| H2 | Reactor listener: replace blocking accept loop with `tur/reactor` (this plan) |
| H3 | Worker reactor: each worker owns a reactor for keep-alive multiplexing |
| H4 | HTTP/1.1 parser: chunked transfer encoding, persistent connections |
| H5 | TLS: optional `mbedtls` / `openssl` integration via `tur/tls` spice |
| H6 | Routing DSL: `defroute`, method guards, path parameters |
| H7 | Middleware: composable request/response transformations |

H1 is the MVP that unblocks spices that need a test server. H2 requires
`tur/reactor` R1-R6 (complete). H3 requires H2.

---

## Integration with `tur/reactor`

The listener thread uses these reactor operations (all in `stdlib/reactor.tur`):

| Operation | Purpose |
|---|---|
| `reactor-new` | Create the listener's event loop |
| `reactor-add-fd` | Register the listen socket for READ (accept-ready) |
| `reactor-add-chan` | Register the shutdown channel (one-shot, re-register for drain) |
| `reactor-add-signal` | Optional: catch SIGTERM/SIGINT to initiate clean shutdown |
| `reactor-run` | Block until stopped |
| `reactor-stop` | Called from shutdown channel callback or signal callback |
| `reactor-wake` | Called by any thread that wants prompt shutdown notification |
| `reactor-free` | Clean up after run returns |

See `docs/archive/history/tur-reactor-plan.md` for the reactor API reference and the
threading model. The key constraint: a `TurReactor` is not thread-safe;
the listener reactor is owned exclusively by the listener thread.

---

## Non-goals

- **No HTTPS in H1-H4.** TLS is H5 and is explicitly a separate spice.
- **No HTTP/2.** Out of scope for this plan; add as a follow-on.
- **No async DNS.** `getaddrinfo` stays synchronous; resolve the address
  before passing to `httpd-new`.
- **No built-in rate limiting or auth.** These belong in middleware (H7).
