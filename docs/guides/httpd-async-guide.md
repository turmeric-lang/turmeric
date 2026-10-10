---
title: HTTPD Async Guide
category: Networking and Web
description: Run handlers as fibers on a reactor thread -- non-blocking I/O, await primitives, in-flight cap, and middleware interop
---

# `stdlib/httpd` -- Async Handlers

`stdlib/httpd` ships two complementary execution models:

| Path | When to reach for it |
|---|---|
| **Blocking thread-pool** (`httpd-new` / `httpd-new-pool`) | CPU-bound handlers; small concurrency; minimal mental overhead. A worker thread is occupied for the lifetime of each request. |
| **Async fiber server** (`httpd-new-async` / `httpd-new-async-with-limit`) | Slow upstreams (DB, downstream HTTP, queues), websocket-style long-poll, or any handler that wants to *suspend* on something the reactor watches (an fd, a timer). Concurrency is bounded by reactor capacity, not by `n_workers`. |

Both paths use the same handler shape -- `(fn [conn] :nil ...)` -- and
the same Track M middleware library composes over either.

Cross-reference: [`reactor-guide.md`](reactor-guide.md) for the
underlying event loop, and [`async-await-guide.md`](async-await-guide.md)
for the language-level suspension model that runs on top of fibers.

---

## Substrate

`httpd-new-async port handler` creates:

1. A `tur/reactor` (event loop).
2. A `local-fiber-group` bound to that reactor.
3. A non-blocking listener registered on the reactor for `READ` events.

When a connection arrives, the accept callback `local-spawn`s a request
fiber whose body runs the non-blocking version of the request lifecycle:

- `O_NONBLOCK` on the accepted fd.
- `recv` loop with `EWOULDBLOCK` -> `local-park-fd READ`, 5 s per wait.
- Header parse (the same `httpd-parse-head` the blocking pool uses, with the
  same refusals and body cap -- see
  [httpd-guide.md](httpd-guide.md#binding-and-request-limits)) + body read.
- Dispatch the user handler.
- `send` loop with `EWOULDBLOCK` -> `local-park-fd WRITE`.
- HTTP/1.1 keep-alive loop, or close.

The whole thing runs on **one** thread by default (one group, one
reactor; shard later if measurement demands it).

```turmeric
(load "stdlib/httpd.tur")

(defn main [] : int
  (let [handler (fn [c : ptr<void>] : nil
                  (httpd-resp-status! c 200)
                  (httpd-resp-body! c "hello async"))
        h       (httpd-new-async 8080 handler)]
    (httpd-run-async h)        ;; blocks until httpd-stop-async fires
    (httpd-async-free h)
    0))
```

```sweet-exp
load("stdlib/httpd.tur")

defn main [] :int
  let [handler (fn [c :ptr<void>] :nil
                 (httpd-resp-status! c 200)
                 (httpd-resp-body! c "hello async"))
       h       httpd-new-async(8080 handler)]
    httpd-run-async(h)        ;; blocks until httpd-stop-async fires
    httpd-async-free(h)
    0
```

---

## Await primitives

Inside a handler running on the async server, three primitives let you
explicitly suspend the current fiber and free the reactor thread to
drive other in-flight requests:

| Primitive | Resumes when... |
|---|---|
| `(httpd-await-readable conn)` | The conn's fd has data ready to read. |
| `(httpd-await-writable conn)` | The conn's fd can be written to. |
| `(httpd-await-timer conn ms)` | `ms` milliseconds have elapsed. |

All three are no-ops when the conn is from the blocking thread-pool
path (the conn's `fiber_group` field is NULL), so handlers that use
them stay portable across both server types.

```turmeric
(defn slow-handler [c : ptr<void>] : nil
  ;; Suspend for ~50ms before responding. Under the blocking pool this
  ;; ties up a worker; under async it parks the fiber and frees the
  ;; reactor thread for other requests.
  (httpd-await-timer c 50)
  (httpd-resp-status! c 200)
  (httpd-resp-body! c "ok"))
```

```sweet-exp
defn slow-handler [c :ptr<void>] :nil
  ;; Suspend for ~50ms before responding. Under the blocking pool this
  ;; ties up a worker; under async it parks the fiber and frees the
  ;; reactor thread for other requests.
  httpd-await-timer(c 50)
  httpd-resp-status!(c 200)
  httpd-resp-body!(c "ok")
```

Need a channel? There is no `httpd-await-chan` yet. `local-park-chan` from
[`stdlib/reactor.tur`](reactor-guide.md) parks a fiber on a channel, but it
takes the `LocalFiberGroup`, and a handler cannot reach the conn's group from
Turmeric: it is a field of the C conn struct, which the `httpd-await-*` forms
read in their inline C. An `httpd-await-chan` built the same way, over
`tur_local_park_chan_status`, is the way to add one.

---

## In-flight cap (A4)

`httpd-new-async-with-limit port handler max-in-flight` caps the number
of concurrent request fibers.  When the cap is hit, the accept callback
sends a quick `HTTP/1.0 503 Service Unavailable` directly from the
reactor thread and closes the connection without spawning a fiber:

```turmeric
;; Cap at 100 in-flight requests; the 101st through Nth get 503.
(let [h (httpd-new-async-with-limit 8080 handler 100)]
  (httpd-run-async h)
  (httpd-async-free h))
```

```sweet-exp
;; Cap at 100 in-flight requests; the 101st through Nth get 503.
let [h httpd-new-async-with-limit(8080 handler 100)]
  httpd-run-async(h)
  httpd-async-free(h)
```

`(httpd-new-async port handler)` is sugar for a cap of **512** -- below the
common 1024 open-file soft limit, so a connection flood is answered 503
before it exhausts the process's descriptors.  A cap of `0` means
unlimited, and has to be asked for by name.  In production you usually
want a cap matched to your upstream's tolerance.

---

## Middleware interop

The H7 middleware shape -- `(fn [next] (fn [conn] ...))` -- composes
unchanged over async handlers.  `httpd-call` is a plain Turmeric call
sitting on the fiber stack, so `next` can park transparently:

```turmeric
(let [base    (fn [c : ptr<void>] : nil
                (httpd-await-timer c 10)
                (httpd-resp-body! c "ok"))
      stack   (compose-middleware base mw-log mw-cors)
      h       (httpd-new-async 8080 stack)]
  (httpd-run-async h)
  (httpd-async-free h))
```

```sweet-exp
let [base    (fn [c :ptr<void>] :nil
               (httpd-await-timer c 10)
               (httpd-resp-body! c "ok"))
     stack   compose-middleware(base mw-log mw-cors)
     h       httpd-new-async(8080 stack)]
  httpd-run-async(h)
  httpd-async-free(h)
```

The same applies to `mw-basic-auth`, `mw-json-body`, `mw-cors-with`,
`mw-compress` (from `stdlib/httpd-compress.tur`, depends on the
`tur/zlib` spice), and `compose-middleware-of` (the runtime form).
See [`httpd-middleware-guide.md`](httpd-middleware-guide.md) for the
full middleware library.

---

## Common patterns

### Per-request deadline

```turmeric
;; Per-request timeout: race the work against a 5-second timer.
;; (Channels make the "first to fire" pattern straightforward.)
(defn handler-with-deadline [c : ptr<void>] : nil
  (httpd-await-timer c 5000)
  ;; ... if the slow work didn't finish, set 504 ...
  )
```

```sweet-exp
;; Per-request timeout: race the work against a 5-second timer.
;; (Channels make the "first to fire" pattern straightforward.)
defn handler-with-deadline [c :ptr<void>] :nil
  httpd-await-timer(c 5000)
  ;; ... if the slow work didn't finish, set 504 ...
```

### Async upstream fan-out

A handler can register an arbitrary fd with the same reactor (via
`reactor-add-fd`) and await its readiness without blocking the worker.
For the common case of "call a slow downstream HTTP service", build a
small client helper on top of `tur/reactor`; the handler is then plain
straight-line Turmeric.

---

## When NOT to go async

- Handlers that are CPU-bound (cryptographic hashing, large JSON
  serialisation): one fiber thread will not parallelise the work.  Use
  the blocking thread pool, or shard via multiple processes.
- Handlers that call into C libraries that perform blocking system
  calls themselves (`fread`, blocking DNS, etc.): the fiber will not
  suspend; the entire reactor thread stalls.  Either wrap the call in
  a worker thread you `await` on, or use the blocking pool.
- "Trivial" servers (a few QPS, no slow upstreams): the blocking pool
  is simpler and faster end-to-end.

---

## Reference

| Defn | Purpose |
|---|---|
| `httpd-new-async port handler` | Async server, in-flight cap 512. |
| `httpd-new-async-with-limit port handler max-in-flight` | Async server with cap. |
| `httpd-run-async h` | Drive the fiber pump; blocks until stopped. |
| `httpd-stop-async h` | Request a graceful shutdown. |
| `httpd-async-free h` | Tear down state.  Call after run returns. |
| `httpd-async-port h` | Read the bound port (useful with `port=0`). |
| `httpd-await-readable conn` | Suspend on READ readiness. |
| `httpd-await-writable conn` | Suspend on WRITE readiness. |
| `httpd-await-timer conn ms` | Suspend for `ms` milliseconds. |
