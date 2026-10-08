---
title: HTTP Server with stdlib/httpd
category: Networking and Web
description: Build HTTP/1.1 servers with stdlib/httpd -- handlers, routing, middleware, and keep-alive
---

# HTTP Server with `stdlib/httpd`

`stdlib/httpd` is a lightweight HTTP/1.0 and HTTP/1.1 server that ships
with the Turmeric tree. It pairs a `tur/reactor` listener thread with a
worker pool and a `Mutex<Queue>` of accepted sockets. Keep-alive,
routing, and middleware are built in. TLS is delivered separately via
the [`tur-tls` spice](httpd-tls-guide.md).

This guide covers the plaintext server. For HTTPS, layer it with
[httpd-tls-guide.md](httpd-tls-guide.md). For the underlying event loop,
see [reactor-guide.md](reactor-guide.md).

---

## Quick start

```turmeric
(load "stdlib/httpd.tur")
(load "stdlib/chan.tur")

(defn main [] : int
  (let [h (httpd-new 8080
            (fn [conn : ptr<void>] : nil
              (httpd-resp-status! conn 200)
              (httpd-resp-body!   conn "Hello, world!")))]
    (httpd-run h)
    (httpd-free h)
    0))
```
```sweet-exp
load "stdlib/httpd.tur"
load "stdlib/chan.tur"

defn main [] :int
  let [h httpd-new(8080
           (fn [conn :ptr<void>] :nil
             httpd-resp-status!(conn 200)
             httpd-resp-body!(conn "Hello, world!")))]
    httpd-run(h)
    httpd-free(h)
    0
```

`httpd-run` blocks the calling thread until `httpd-stop` is signalled
from another thread. The handler runs on a worker thread, not the
listener.

### Handler calling convention

The handler closure **must capture at least one variable** so it is
heap-allocated as a fat closure. A bare top-level `defn` will not work
as a handler -- wrap it:

```turmeric
(let [_     0
      h     (httpd-new 8080
              (fn [conn : ptr<void>] : nil
                (handle-request conn _)))]
  ...)
```
```sweet-exp
let [_ 0
     h httpd-new(8080
         (fn [conn :ptr<void>] :nil
           handle-request(conn _)))]
  ...
```

Inside the handler:

- `httpd-req-method`, `httpd-req-path`, `httpd-req-version`,
  `httpd-req-body`, `httpd-req-body-len` read the parsed request.
- `httpd-resp-status!` and `httpd-resp-body!` set the response.
- `httpd-resp-status-get` reads back the status set so far (useful for
  middleware).

---

## Constructors

| Function                                  | Use when                                                |
|-------------------------------------------|---------------------------------------------------------|
| `(httpd-new port handler)`                | 4-worker plaintext server                               |
| `(httpd-new-pool port workers handler)`   | Custom worker count                                     |
| `(httpd-new-pool-with-limit port workers handler max-pending)` | Custom worker count and pending-connection cap |
| `(httpd-new-tls port workers handler ctx)`| HTTPS termination -- see [httpd-tls-guide.md](httpd-tls-guide.md) |

A `port` of `0` lets the kernel choose; read it back with
`(httpd-port h)`. This is the recommended pattern for tests.

Accepted connections wait in a queue for a free worker, each holding a
descriptor. At most 512 wait (`max-pending` for
`httpd-new-pool-with-limit`, `0` = unlimited); past that a connection is
answered `503 Service Unavailable` and closed, as the async server does
past its in-flight cap.

### Binding and request limits

Servers listen on **127.0.0.1** unless the program asks for more. To be
reachable from the network, call `(httpd-set-bind-any! true)` before the
constructor (or set `TUR_HTTPD_BIND_ANY=1` in the environment); that binds
`0.0.0.0`. To name one interface instead -- IPv4 or IPv6 -- call
`(httpd-set-bind-addr! "192.168.1.5")` (or `"::1"`, or `"::"` for every IPv6
interface); it wins over `httpd-set-bind-any!`, `""` clears it, and an
address that does not parse returns `false` and changes nothing.
`TUR_BIND_LOOPBACK`, which the test harnesses export, forces loopback either
way -- of the named address's own family.

Before a handler runs, the server refuses, and closes the connection on:

| Request | Answer |
| --- | --- |
| `Content-Length` that is not plain decimal digits, or repeated with different values | 400 |
| `Content-Length` together with `Transfer-Encoding` | 400 |
| `Transfer-Encoding` alone (chunked bodies are not implemented) | 501 |
| `Content-Length` above the body cap (8 MiB by default; `httpd-set-max-body!`) | 413 |
| A body shorter than its `Content-Length` (timeout or peer close) | dropped, no answer |

The body cap is checked before anything is allocated for the body.
`mw-body-size` is a lower, per-route limit that runs inside the handler chain,
after the body has been read.

Response headers whose name or value contains a CR or LF, or whose name
contains a colon or a blank, are dropped when the response is written, so a
handler that copies request data into a header cannot split the response.
`mw-static` serves only regular files that resolve inside its root (symlinks
included), ignores the query string, and refuses any path segment starting
with `.` other than `.well-known`.

---

## Lifecycle

```turmeric
(let [h (httpd-new 0 handler)]
  ;; run on a background thread so the main thread can signal shutdown
  (let [server (spawn-server h)]
    ...                            ; do work, wait for a signal, etc.
    (httpd-stop h)                 ; thread-safe; wakes the listener
    (join-thread server)
    (httpd-free h)))               ; releases reactor + worker pool
```
```sweet-exp
let [h httpd-new(0 handler)]
  ;; run on a background thread so the main thread can signal shutdown
  let [server spawn-server(h)]
    ...                            ; do work, wait for a signal, etc.
    httpd-stop(h)                  ; thread-safe; wakes the listener
    join-thread(server)
    httpd-free(h)                  ; releases reactor + worker pool
```

`httpd-stop` delegates to `reactor-stop` on the listener's reactor and
is safe to call from a signal handler, another worker, or the main
thread. In-flight requests complete; new connections are refused once
`reactor-run` returns.

---

## Routing (H6)

`stdlib/httpd` ships a small router that matches on method and path
pattern, with support for `:name` path parameters:

```turmeric
(let [r (router-new)]
  (defroute r "GET"  "/"           home-handler)
  (defroute r "GET"  "/users/:id"  user-handler)
  (defroute r "POST" "/users"      create-user-handler)
  (let [h (httpd-new 8080
            (fn [conn : ptr<void>] : nil
              (router-dispatch r conn)))]
    (httpd-run h)
    (router-free r)
    (httpd-free h)))
```
```sweet-exp
let [r router-new()]
  defroute r "GET"  "/"           home-handler
  defroute r "GET"  "/users/:id"  user-handler
  defroute r "POST" "/users"      create-user-handler
  let [h httpd-new(8080
           (fn [conn :ptr<void>] :nil
             router-dispatch(r conn)))]
    httpd-run(h)
    router-free(r)
    httpd-free(h)
```

Inside a route handler, `(httpd-param conn "id")` returns the captured
segment as a `:cstr`. Unmatched requests receive a 404 automatically.

`defroute` is a thin macro over `router-add`; either form works.

---

## Middleware (H7)

Middleware wraps a handler and can short-circuit, mutate the response,
or log. The pattern is plain function composition -- no framework
machinery:

```turmeric
(defn log-mw [next : int]
  (fn [conn : ptr<void>] : nil
    (println (httpd-req-path conn))
    (httpd-call next conn)
    (println-status (httpd-resp-status-get conn))))

(let [base (fn [conn : ptr<void>] : nil
             (router-dispatch r conn))
      h    (httpd-new 8080 (log-mw base))]
  ...)
```
```sweet-exp
defn log-mw [next :int]
  (fn [conn :ptr<void>] :nil
    println(httpd-req-path(conn))
    httpd-call(next conn)
    println-status(httpd-resp-status-get(conn)))

let [base (fn [conn :ptr<void>] :nil
            router-dispatch(r conn))
     h    httpd-new(8080 log-mw(base))]
  ...
```

`httpd-call` invokes a captured handler closure on a connection -- it is
how a middleware passes control through to the next layer.

See [httpd-middleware-guide.md](httpd-middleware-guide.md) for the
full catalog of shipped middleware (logging, CORS, basic auth, body
size, rate limiting, static files, ...), the request-attribute side
channel (`httpd-set-attr!` / `httpd-req-attr`), and the rules for
composing or writing your own.

---

## Keep-alive (H4)

HTTP/1.1 keep-alive is on by default. A worker loops on the same socket
until:

- The client sends `Connection: close`.
- A 5-second `SO_RCVTIMEO` fires (no data within the idle window). The async
  server (`httpd-new-async`) applies the same 5-second bound to each wait
  while reading a request.
- The peer closes the socket.

HTTP/1.0 connections close after one request unless the client sends
`Connection: keep-alive`. No tuning knobs are exposed in v1; if you need
a different idle bound, set it on the listen socket before calling
`httpd-run`.

---

## Threading model

| Component       | Thread                                          |
|-----------------|-------------------------------------------------|
| Listener        | One thread runs `reactor-run` on the listen fd  |
| Worker pool     | N threads (default 4); each pops from a shared `Mutex<Queue<int>>` |
| Handler closure | Runs on whichever worker popped the connection  |

The listener thread does *not* call user code. Workers do. Handlers may
block; long-running handlers tie up a worker for the duration.

For TLS termination, the worker drives the handshake after popping the
fd -- see [httpd-tls-guide.md](httpd-tls-guide.md).

---

## Common patterns

### Tests with a kernel-assigned port

```turmeric
(let [donech (chan-new 1)
      h      (httpd-new 0
               (fn [conn : ptr<void>] : nil
                 (httpd-resp-status! conn 200)
                 (httpd-resp-body!   conn "ok")
                 (chan-send donech 1)))
      port   (httpd-port h)
      server (spawn-server h)
      client (spawn-client port)]
  (chan-recv donech)
  (httpd-stop h)
  (join-thread server)
  (join-thread client)
  (httpd-free h))
```
```sweet-exp
let [donech chan-new(1)
     h      httpd-new(0
              (fn [conn :ptr<void>] :nil
                httpd-resp-status!(conn 200)
                httpd-resp-body!(conn "ok")
                chan-send(donech 1)))
     port   httpd-port(h)
     server spawn-server(h)
     client spawn-client(port)]
  chan-recv(donech)
  httpd-stop(h)
  join-thread(server)
  join-thread(client)
  httpd-free(h)
```

This is the shape used by every fixture under
`tests/fixtures/httpd-h*`.

### Graceful shutdown from a signal

Pair `httpd-stop` with `reactor-add-signal` on a separate reactor, or
keep a shared `chan` that any thread (including a SIGINT handler set up
manually) can write to.

---

## Non-goals

- **No HTTP/2.** HTTP/1.1 only; HTTP/2 is not on the roadmap.
- **No async DNS.** `getaddrinfo` is blocking.
- **No built-in TLS.** Use [`tur-tls`](httpd-tls-guide.md) via
  `httpd-new-tls`.
- **No rate limiting or auth.** Compose via middleware.

---

## See also

- [httpd-tls-guide.md](httpd-tls-guide.md) -- HTTPS termination via `tur-tls`
- [reactor-guide.md](reactor-guide.md) -- the event loop the listener runs on
- [threading-guide.md](threading-guide.md) -- the `Mutex<Queue>` worker dispatch primitive
- `turmeric-spices/spices/ws-client/` -- client-side WebSocket spice
- [websocket-server-plan.md](https://github.com/turmeric-lang/turmeric/blob/main/docs/archive/history/websocket-server-plan.md) -- `ws-server` spice plan; upgrades an httpd connection to a WebSocket session
