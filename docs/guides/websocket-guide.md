---
title: WebSockets
category: Networking and Web
description: Build RFC 6455 WebSocket clients and servers in Turmeric with the tur-ws-client and tur-ws-server spices
audience: developers writing real-time clients or upgrading tur-httpd endpoints to WebSocket
since: ws-client v0.1.0 / ws-server v0.1.0
---

# WebSockets

Two sibling spices ship WebSocket support for Turmeric:

- **`ws-client`** -- RFC 6455 client. Connect to any `ws://` or `wss://`
  endpoint, exchange text and binary frames over a small blocking API.
- **`ws-server`** -- RFC 6455 server upgrade for `tur-httpd`. Lift an
  ordinary HTTP request to a WebSocket session with `ws-upgrade`.

Both spices share the same opaque-handle design: `WsConn` owns the
socket and a reassembly buffer; `WsFrame` is a view into the most-recent
message in that buffer, valid only until the next `recv`.

Plain `ws://` has **no native dependencies** -- the handshake's SHA-1 and
base64 are inlined. TLS (`wss://`) is layered on bundled **mbedTLS 3.6.2**
and gated at compile time on `__has_include(<mbedtls/ssl.h>)`: if mbedTLS
is not available the spice still builds and plaintext still works; a TLS
connect just returns an error explaining how to enable it.

## TL;DR

```turmeric
;; Client
(let [c (ws-connect "ws://localhost:9000/echo")]
  (when (not (ws-conn-null? c))
    (ws-send c "hello")
    (let [f (ws-recv c)]
      (println (ws-frame-text f)))
    (ws-close c)
    (ws-free c)))

;; Server (inside a tur-httpd two-arg handler)
(defn handler [conn : Conn req : Request] : Response
  (if (= (ws-upgrade conn req
           (fn [ws : WsConn] : void
             (let [f (ws-server-recv ws)]
               (ws-server-send ws (ws-frame-text f))
               (ws-server-close ws)))) 1)
    (resp-ok "text/plain" "")            ;; placeholder; worker skips it
    (bad-request "not a websocket request")))
```

## Installation

Both spices follow the standard `:spices` declaration in `build.tur`.

### Client only

```turmeric
:spices #map{
  "ws-client" #map{:url    "https://github.com/turmeric-lang/turmeric-spices"
                   :ref    "ws-client-v0.1.0"
                   :subdir "spices/ws-client"}
}
```

### Server (depends on `httpd`)

```turmeric
:spices #map{
  "httpd"     #map{:url    "https://github.com/turmeric-lang/turmeric-spices"
                   :ref    "httpd-v0.1.0"
                   :subdir "spices/httpd"}
  "ws-server" #map{:url    "https://github.com/turmeric-lang/turmeric-spices"
                   :ref    "ws-server-v0.1.0"
                   :subdir "spices/ws-server"}
}
```

Within the `turmeric-spices` workspace these resolve as siblings with no
fetch needed for the plaintext path. For `wss://` (or to terminate TLS on
the server side), run `tur fetch` once to vendor mbedTLS.

## The frame model

Both client and server return frames the same way: `recv` reads bytes off
the wire, reassembles any fragmented data message into the connection's
internal buffer, and returns a `WsFrame` handle whose kind is one of:

| Kind        | Predicate         | Notes                                          |
|-------------|-------------------|------------------------------------------------|
| text        | `(ws-text? f)`    | UTF-8 data message. Use `ws-frame-text`.       |
| binary      | `(ws-binary? f)`  | Binary data message. Use `ws-frame-data`/`-len`. |
| ping        | `(ws-ping? f)`    | Already auto-ponged. Payload is surfaced.      |
| pong        | `(ws-pong? f)`    | Surfaced; no automatic action.                 |
| close       | `(ws-closed? f)`  | Peer initiated close, or transport EOF.        |
| timeout     | `(ws-timeout? f)` | Receive timeout fired (see `ws-set-timeout`).  |
| error       | `(ws-error? f)`   | Transport error.                               |

Accessors:

| Function              | Returns                                                |
|-----------------------|--------------------------------------------------------|
| `ws-frame-kind f`     | `:int` -- one of the `WS_KIND_*` constants            |
| `ws-frame-data f`     | `:ptr<void>` -- payload pointer (valid until next recv) |
| `ws-frame-len  f`     | `:int` -- payload byte length                          |
| `ws-frame-text f`     | `:cstr` -- payload as NUL-terminated string            |

**Borrow rules:** `ws-frame-data` and `ws-frame-text` point into the
connection's reassembly buffer. They are valid only until the next
`ws-recv` / `ws-server-recv` on the same connection, or until the
connection is freed. Callers that need the payload longer must copy it.

## Client

The client API lives in `ws-client/client`:

```turmeric
(import ws-client/client
  :refer [ws-connect ws-connect-with-ca ws-connect-insecure ws-conn-null? ws-last-error
          ws-send ws-send-bytes ws-recv
          ws-close ws-free ws-set-timeout
          ws-frame-kind ws-frame-data ws-frame-len ws-frame-text
          ws-text? ws-binary? ws-ping? ws-pong?
          ws-closed? ws-timeout? ws-error?])
```

### Opening a connection

```turmeric
(defn ws-connect [uri : cstr] : WsConn)
```

`uri` must start with `ws://` or `wss://`. Host, port, and path are
parsed out; the port defaults to 80 / 443. The handshake runs
synchronously and the call returns either a live `WsConn` or the null
sentinel `0` on failure. Test with `ws-conn-null?` and read
`ws-last-error` for the reason:

```turmeric
(let [c (ws-connect "ws://127.0.0.1:9000/echo")]
  (if (ws-conn-null? c)
    (do (println "connect failed:") (println (ws-last-error)) 1)
    ;; ... use c ...
    0))
```

### Sending

```turmeric
(defn ws-send       [conn : WsConn msg : cstr]                 : int)
(defn ws-send-bytes [conn : WsConn ptr : ptr<void> len : int]  : int)
```

Both return the number of bytes handed to the transport, or a negative
value on error. Client-to-server frames are always masked (RFC 6455
5.3); the codec handles this for you.

### Receiving

```turmeric
(defn ws-recv [conn : WsConn] : WsFrame)
```

Blocks until a complete message arrives or the receive timeout fires.
Ping frames are answered with a Pong automatically and then surfaced
(so you can log them); Pong and Close frames are surfaced as well.

### Timeouts

```turmeric
(defn ws-set-timeout [conn : WsConn ms : int] : void)
```

Sets `SO_RCVTIMEO` on the underlying socket. `ms = 0` blocks
indefinitely. When the timeout fires, `ws-recv` returns a frame whose
kind is `:timeout` -- not an error.

### Closing

```turmeric
(defn ws-close [conn : WsConn] : void)   ;; clean RFC 6455 close handshake
(defn ws-free  [conn : WsConn] : void)   ;; unconditional release
```

`ws-close` sends a Close frame, drains briefly waiting for the peer's
Close echo, then shuts the socket. `ws-free` releases the handle's
memory and tears down any TLS state. Always pair `ws-connect` with
`ws-free` -- typically `ws-close` first for a graceful shutdown, then
`ws-free`.

### End-to-end client example

Lifted (and trimmed) from `ws-client/tests/fixtures/ws-echo-text/main.tur`:

```turmeric
(defmodule ws-echo-text
  (import ws-client/client
    :refer [ws-connect ws-conn-null? ws-last-error
            ws-send ws-recv ws-close ws-free ws-set-timeout
            ws-frame-text ws-text? ws-timeout?])

  (defn cstr-eq? [a : cstr b : cstr] : bool
    ```c
    #include <string.h>
    return strcmp((const char *)a, (const char *)b) == 0;
    ```)

  (defn main [] : int
    (let [c (ws-connect "ws://127.0.0.1:9000/echo")]
      (if (ws-conn-null? c)
        (do (println "connect failed:") (println (ws-last-error)) 1)
        (do
          (ws-set-timeout c 2000)
          (ws-send c "hello websocket")
          (let [f1 (ws-recv c)]
            (when (and (ws-text? f1)
                       (cstr-eq? "hello websocket" (ws-frame-text f1)))
              (println "echo round-trip OK")))
          (ws-set-timeout c 200)
          (let [f2 (ws-recv c)]
            (when (ws-timeout? f2)
              (println "idle timeout fired as expected")))
          (ws-close c)
          (ws-free c)
          0)))))
```

## Server

The server API lives in `ws-server/server` and plugs into `tur-httpd`'s
two-arg handler shape (`server-start-conn` / `serve-conn`), where each
handler receives a `Conn` alongside the `Request`.

```turmeric
(import ws-server/server
  :refer [WsConn ws-upgrade
          ws-server-send ws-server-send-bytes ws-server-recv ws-server-close
          ws-set-server-timeout
          ws-frame-kind ws-frame-data ws-frame-len ws-frame-text
          ws-text? ws-binary? ws-ping? ws-pong?
          ws-closed? ws-timeout? ws-error?])
```

### `ws-upgrade`

```turmeric
(defn ws-upgrade [conn    : Conn
                  req     : Request
                  handler : fn] : int)
```

`handler` is a `(fn [ws : WsConn] : void)`. `ws-upgrade`:

1. Validates the `Upgrade: websocket` and `Sec-WebSocket-Key` headers.
2. Writes `HTTP/1.1 101 Switching Protocols` directly to the socket.
3. Marks the `Conn` as upgraded (so httpd's worker loop will not flush
   another response or close the fd).
4. Wraps the fd into a `WsConn` and calls your `handler`.
5. Frees the `WsConn` once your handler returns.

Return value:

- **`1`** -- upgraded and the handler ran. The `Response` your handler
  returns is **ignored** by the worker loop, but you must still return
  something to satisfy the type checker; use a placeholder like
  `(resp-ok "text/plain" "")`.
- **`0`** -- the request was not a valid WebSocket upgrade. Return a
  normal HTTP error response (e.g. `bad-request`).

### Sending and receiving

The server-side mirror of the client API. The key wire-level difference
is that server-to-client frames are sent **unmasked** (RFC 6455 5.1);
the codec handles this for you.

```turmeric
(defn ws-server-send       [ws : WsConn msg : cstr]                : int)
(defn ws-server-send-bytes [ws : WsConn ptr : ptr<void> len : int] : int)
(defn ws-server-recv       [ws : WsConn]                           : WsFrame)
(defn ws-server-close      [ws : WsConn]                           : void)
(defn ws-set-server-timeout [ws : WsConn ms : int]                 : void)
```

Lifecycle: do **not** use a server `WsConn` after the handler returns
-- `ws-upgrade` frees it on the way out.

### End-to-end server example

Adapted from `ws-server/tests/fixtures/echo/server.tur`:

```turmeric
(defmodule ws-echo-server
  (import httpd/types    :refer [Conn Request])
  (import httpd/server   :refer [server-start-conn])
  (import httpd/request  :refer [req-path])
  (import httpd/response :refer [resp-ok bad-request not-found])
  (import ws-server/server
    :refer [WsConn ws-upgrade ws-server-recv ws-server-send ws-server-close
            ws-set-server-timeout
            ws-frame-text ws-text? ws-binary?
            ws-closed? ws-error? ws-timeout?])

  (defn cstr-eq? [a : cstr b : cstr] : bool
    ```c
    #include <string.h>
    return strcmp((const char *)a, (const char *)b) == 0;
    ```)

  (defn block-forever [] #fx{Unsafe} : void
    ```c
    #include <unistd.h>
    for (;;) sleep(3600);
    ```)

  ;; Read frames until the peer closes, the transport errors, or the
  ;; receive timeout fires. Echo every data frame back.
  (defn echo-loop [ws : WsConn] : void
    (let [f (ws-server-recv ws)]
      (if (or (ws-closed? f) (ws-error? f) (ws-timeout? f))
        (ws-server-close ws)
        (do
          (when (or (ws-text? f) (ws-binary? f))
            (ws-server-send ws (ws-frame-text f)))
          (echo-loop ws)))))

  (defn handler [conn : int req : int] : int
    (let [c (:: conn Conn)
          r (:: req Request)]
      (if (cstr-eq? (req-path r) "/ws")
        (if (= (ws-upgrade c r
                 (fn [ws : WsConn] : void
                   (do (ws-set-server-timeout ws 10000)
                       (echo-loop ws)))) 1)
          (:: (resp-ok "text/plain" "") :int)
          (:: (bad-request "not a websocket request") :int))
        (:: (not-found "") :int))))

  (defn main [] : int
    (let [_ (server-start-conn 9000 handler)]
      (unsafe (block-forever))
      0)))
```

### Broadcast / fan-out

A WebSocket session occupies its worker thread for the lifetime of the
connection. For a chat / pub-sub style server, drive the listener with
`server-start-pool-conn` (a worker pool variant) and share a
`Mutex<Vec<WsConn>>` hub across handlers. The pattern from
`ws-server/tests/fixtures/broadcast/server.tur`:

```turmeric
(load "stdlib/mutex.tur")

(def hub-mutex (mutex-new))
(def hub (:: (vec-new) (Vec WsConn)))

(defn hub-register! [ws : WsConn] : nil
  (do (mutex-lock hub-mutex)
      (vec-push! hub ws)
      (mutex-unlock hub-mutex)))

(defn hub-send-each [msg : cstr i : int n : int] : nil
  (when (< i n)
    (do (ws-server-send (vec-get hub i) msg)
        (hub-send-each msg (+ i 1) n))))

(defn hub-broadcast! [msg : cstr] : nil
  (do (mutex-lock hub-mutex)
      (hub-send-each msg 0 (vec-len hub))
      (mutex-unlock hub-mutex)))
```

Register on connect, deregister on disconnect (under the same mutex)
before the handler returns -- otherwise a broadcast could dispatch into
a `WsConn` that `ws-upgrade` is about to free.

## TLS (`wss://`)

TLS is fully optional in both directions. Build the spice with mbedTLS
on the include path (the bundled `cmake-deps` entry handles this when
you `tur fetch`) and:

- **Client:** `ws-connect "wss://..."` verifies the server certificate
  against the platform's system CA store by default (and checks the
  hostname), so an untrusted, expired, or mismatched certificate fails
  the connect rather than silently succeeding. Three entry points fix
  the trust policy at the call site:

  ```turmeric
  (ws-connect "wss://example.com/feed")                      ;; system CA store (default)
  (ws-connect-with-ca "wss://localhost:8443/socket" ca-path)  ;; a caller-supplied CA bundle
  (ws-connect-insecure "wss://localhost:8443/socket")         ;; UNSAFE: skip verification (dev only)
  ```

  Use `ws-connect-with-ca` for a private or self-signed CA (e.g. a local
  dev server); reach for `ws-connect-insecure` only as an explicit,
  greppable opt-out -- never in production.
- **Server:** drive the listener with `httpd/tls`'s
  `server-start-tls-conn`. The httpd `Conn` carries a TLS context in
  its `tls` slot; `ws-upgrade` reads it via `conn-tls` and runs the
  101 handshake and every frame over `mbedtls_ssl_*`. No extra code
  in your handler.

When mbedTLS is absent, plain `ws://` still works; a `wss://` connect
or TLS listener will fail at runtime with an error pointing at the
missing dependency.

## Composing with `httpd` / `tourist`

The server spice is built on `tur-httpd`'s two-arg handler shape
(`server-start-conn`, `server-start-pool-conn`, `server-start-tls-conn`).
Mix WebSocket and ordinary REST endpoints in the same handler -- the
`ws-upgrade` call only fires on requests that carry the right Upgrade
headers, and falls through (`0`) otherwise. Route on `req-path` in your
handler exactly as the echo example does.

`tur-tourist`'s default one-arg handler shape (`Request -> Response`)
does not expose the `Conn` that `ws-upgrade` requires, but the
**`tourist-ws`** spice adapts around that: `ws-route!` declares a
WebSocket endpoint that slots into an ordinary tourist item list next to
`get!`/`post!`/`use!`, sharing the same middleware chain.

```turmeric
(import tourist/app      :refer [tourist-conn])
(import tourist/dsl      :refer [get!])
(import tourist-ws/route :refer [ws-route!])
(import ws-server/server :refer [ws-server-recv ws-server-send ws-server-close])
(import ws-core/conn     :refer [WsConn])

(tourist-conn 8080
  (get! "/api" (fn [ctx] (text "hello from REST")))
  (ws-route! "/ws"
    (fn [ws : WsConn] : void
      (let [f (ws-server-recv ws)]
        (ws-server-send ws (ws-frame-text f))
        (ws-server-close ws)))))
```

Serve with **`tourist-conn`**, not `tourist` -- `ws-route!` needs the
underlying httpd `Conn` to perform the upgrade, and only the Conn-aware
`tourist-conn` entry point makes it available (`tourist-conn` is
otherwise a drop-in for `tourist`: same item list, same middleware
chain, same dispatch). A `ws-route!` served under plain `tourist` has no
`Conn` to upgrade and answers `400 Bad Request`. See the
[tourist-ws spice](https://github.com/turmeric-lang/turmeric-spices/tree/main/spices/tourist-ws)
for the full API and the REST+WebSocket coexistence fixture.

## Limitations and gotchas

- **`WsConn` lifetime (server).** Do not retain a server `WsConn`
  beyond the handler -- `ws-upgrade` frees it on return. The broadcast
  hub example works because it deregisters under the same mutex it
  uses to fan out.
- **Frame borrow window.** `ws-frame-data` / `ws-frame-text` point into
  the connection's reassembly buffer. The next `recv` (or a `free`)
  invalidates them. Copy if you need the bytes longer.
- **Blocking I/O.** Both spices are synchronous. One thread per
  connection on the server side; one connection per process on the
  client side (no async / event-loop variant yet).
- **Client certificate verification is on by default.** `ws-connect`
  verifies against the system CA store; use `ws-connect-with-ca` for a
  private CA, or the explicitly-named `ws-connect-insecure` to opt out
  (development only -- never in production).
- **Shared opaque names across spices.** Both `ws-client` and
  `ws-server` export their own `WsConn` and `WsFrame` opaques. They
  cannot be imported into the same program; the test suite intentionally
  drives the server from a separate ws-client process. If you need both
  roles in one binary, alias one set on import.
- **Per-message extensions.** No `permessage-deflate` or other RFC 7692
  extensions. The handshake never negotiates them.
- **Subprotocols.** No `Sec-WebSocket-Protocol` negotiation. The
  request header is not read or echoed; add it manually in your handler
  if you need it.
- **Single-frame send.** `ws-send` / `ws-server-send` always emit a
  single FIN-set frame. There is no explicit fragmentation API for
  outbound messages. Inbound fragmented messages are reassembled by
  `recv` before being surfaced.
- **`ws-upgrade` return type.** Uses an `:int` status (`1` / `0`)
  rather than a `result<>`; matches the spice's house style but is a
  candidate for tightening once the rest of the httpd surface migrates.

## See also

- [httpd guide](httpd-guide.md) -- the HTTP server that `ws-server` upgrades.
- [httpd TLS guide](httpd-tls-guide.md) -- terminate TLS for `wss://`.
- [Tourist routing guide](tourist-routing-guide.md) -- routing combinators
  for the one-arg HTTP handler shape.
- RFC 6455 -- the WebSocket Protocol.
