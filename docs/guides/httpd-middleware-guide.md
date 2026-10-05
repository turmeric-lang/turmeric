---
title: HTTP Middleware Catalog (stdlib/httpd)
category: Networking and Web
description: Reference for the shipped httpd middleware -- logging, CORS, basic auth, JSON, cookies, multipart, body-size, rate-limit, static files -- plus the request-attribute side channel and the composition helpers.
---

# HTTP Middleware Catalog

`stdlib/httpd` ships a small collection of middleware on top of the
H7 calling convention documented in
[httpd-guide.md](httpd-guide.md#middleware-h7). A middleware is just a
function that takes the next handler closure and returns a wrapping
closure -- no framework machinery, no registration step. This guide is
the catalog of the built-ins, plus the rules for composing them and
writing your own.

## The shape

Every middleware in this guide has the shape

```turmeric
(defn mw-foo [next : int] : ptr<void>
  (let [_n next ...]
    (fn [c : ptr<void>] : nil
      ;; pre-processing here ...
      (httpd-call _n c)
      ;; post-processing here ...
      )))
```

```sweet-exp
defn mw-foo [next :int] :ptr<void>
  let [_n next ...]
    (fn [c :ptr<void>] :nil
      ;; pre-processing here ...
      httpd-call(_n c)
      ;; post-processing here ...
      )
```

Pre-processing runs before `(httpd-call _n c)`; post-processing runs
after. A middleware can short-circuit by setting the response and
*not* calling `_n` -- that is how `mw-basic-auth` emits a 401, how
`mw-body-size` emits a 413, and how `mw-rate-limit` emits a 429.

Middleware that takes configuration (`mw-cors-with`, `mw-rate-limit`,
`mw-static`, ...) is a function of `(opts ..., next)` -- the partial
application `(mw-foo opts)` is a one-argument function suitable for
[`compose-middleware`](#composing-middleware).

## Composing middleware

Use `compose-middleware` to nest:

```turmeric
(let [composed (compose-middleware base
                                   mw-log
                                   mw-cors
                                   (mw-basic-auth "app" verify))]
  (httpd-new 0 composed))
```

```sweet-exp
let [composed compose-middleware(base
                                 mw-log
                                 mw-cors
                                 mw-basic-auth("app" verify))]
  httpd-new(0 composed)
```

The macro expands to `(mw-log (mw-cors ((mw-basic-auth "app" verify) base)))`
-- the **leftmost middleware is the outermost wrapper**, its
pre-processing runs first, its post-processing runs last (Ring /
Rack ordering).

`compose-middleware-of` is the runtime variadic form for chains built
dynamically (e.g. from a config flag). Each argument must be a *fat*
closure value, not a bare defn name -- see the docstring on
`stdlib/httpd.tur` for the bridging idiom.

## The shipped catalog

The table below summarises every middleware in `stdlib/httpd`. Each
entry links to its docstring in the source for the full surface.

| Name                      | Purpose                                          | Phase |
|---------------------------|--------------------------------------------------|-------|
| `mw-log`                  | One line per request -- method, path, status, body bytes, elapsed ms | M1 |
| `mw-cors` / `mw-cors-with`| CORS preflight + Access-Control-Allow-Origin decoration | M4 |
| `mw-basic-auth`           | HTTP Basic Auth via user verifier closure; publishes `"user"` attr on success | M5 |
| `mw-json-body`            | Pre-parse JSON request body; 400 on malformed input | M3 |
| `mw-body-size`            | Reject requests with Content-Length above a cap (413) | MW1 |
| `mw-rate-limit`           | Sliding-window per-IP rate limiter (429 + Retry-After) | MW2 |
| `mw-static`               | Fall back to static files when `next` returned 404 (with ETag + 304) | MW2 |
| `mw-compress` / `mw-compress-with` | gzip the response body when client sends `Accept-Encoding: gzip` (requires `tur/zlib` spice) | M6 |
| `mw-recover`              | Catch a downstream panic and respond 500; the server keeps serving | MW3 |

### mw-body-size (MW1)

Reads the `Content-Length` request header and short-circuits the
request with `413 Payload Too Large` when it exceeds the cap. The cap
is on the announced size; chunked transfer-encoding is not currently
supported by the underlying httpd layer, so the header is
authoritative.

```turmeric
(let [composed (mw-body-size 1048576 base)]   ; 1 MiB cap
  (httpd-new 0 composed))
```

```sweet-exp
let [composed mw-body-size(1048576 base)]   ; 1 MiB cap
  httpd-new(0 composed)
```

### mw-rate-limit (MW2)

Sliding-window per-IP rate limiter. Each `mw-rate-limit` instance
allocates its own fixed-size counter table (1024 slots, FNV-1a hash,
linear probing) protected by a `pthread_mutex`. When the same client
IP exceeds `requests` requests within `window-s` seconds the
middleware responds with `429 Too Many Requests` plus a
`Retry-After: <seconds>` header -- without calling `next`.

```turmeric
(let [opts     (make-struct RateLimitOpts 100 60)   ; 100 req / 60 s
      composed (mw-rate-limit opts base)]
  (httpd-new 0 composed))
```

```sweet-exp
let [opts     make-struct(RateLimitOpts 100 60)   ; 100 req / 60 s
     composed mw-rate-limit(opts base)]
  httpd-new(0 composed)
```

Multiple `mw-rate-limit` instances do not share state. Two
compositions of `mw-rate-limit` each get an independent table; share
by reusing the same wrapped closure. The table holds 2048 IPs, keyed by
the IP string itself (its hash only picks one of 256 sets of 8). A new IP
takes an empty entry of its set, else one whose window has ended, else the
one whose window started longest ago -- so the limiter never fails open:
a flood of distinct IPs cannot leave every later IP unlimited, it only
restarts the windows of the IPs it evicts. Each check touches at most 8
entries.

The client IP comes from [`httpd-req-remote-ip`](#client-ip), which
caches its result on the `__remote_ip` request attribute so repeated
lookups within one request do not re-`getpeername(2)`.

### mw-static (MW2)

Defers to `next` first; only serves a file when `next` returned 404
(the default no-route signal from `router-dispatch`). The file path is the
request path joined onto the configured `root-dir`; any `..` segment
is rejected as a path-traversal guard.

```turmeric
(let [composed (mw-static "./public" (router-mw r))]
  (httpd-new 0 composed))
```

```sweet-exp
let [composed mw-static("./public" router-mw(r))]
  httpd-new(0 composed)
```

The response carries a `Content-Type` derived from the file extension
(small built-in table covering HTML / CSS / JS / JSON / TXT / common
image formats / WASM) and an `ETag` of the form `"<size>-<mtime>"`
(both in hex). A subsequent request carrying a matching
`If-None-Match` short-circuits with `304 Not Modified` and an empty
body -- the file is `stat`'d but never `read`.

Files are read fully into memory. For very large files use a
streaming backend instead -- the public surface today returns a
buffered body via `httpd-resp-body!`.

### mw-basic-auth + request attrs

Once authentication succeeds, `mw-basic-auth` publishes the verified
username via `(httpd-set-attr! conn "user" <username>)` so downstream
handlers can read it back. See the [request attributes](#request-attributes-mw2)
section for the full surface.

```turmeric
(let [verify   (fn [u : cstr p : cstr] : int
                 (let [_t "_force-fat-closure"]
                   if {1 = cstr-eq-const-time(u "admin")}
                     (cstr-eq-const-time p "s3cret")
                     0)))
      base     (fn [c : ptr<void>] : nil
                 (let [u (httpd-req-attr c "user")]
                   (httpd-resp-status! c 200)
                   (httpd-resp-body!   c u)))
      composed (mw-basic-auth "app" verify base)]
  (httpd-new 0 composed))
```

```sweet-exp
let [verify   (fn [u :cstr p :cstr] :int
                (let [_t "_force-fat-closure"]
                  (if {1 = (cstr-eq-const-time u "admin")}
                    (cstr-eq-const-time p "s3cret")
                    0)))
     base     (fn [c :ptr<void>] :nil
                (let [u (httpd-req-attr c "user")]
                  (httpd-resp-status! c 200)
                  (httpd-resp-body!   c u)))
     composed mw-basic-auth("app" verify base)]
  httpd-new(0 composed)
```

### mw-compress (M6)

`mw-compress` gzips the response body when the client sends
`Accept-Encoding: gzip`, sets `Content-Encoding: gzip` plus
`Vary: Accept-Encoding`, and is otherwise a no-op. The codec lives in
the `tur/zlib` spice (`../turmeric-spices/spices/zlib`); install it
into your workspace, then `(load "stdlib/httpd-compress.tur")` from
your program.

```turmeric
(load "stdlib/httpd-compress.tur")

(let [base     (fn [c : ptr<void>] : nil
                 (httpd-resp-status! c 200)
                 (httpd-resp-body!   c large-html))
      composed (compose-middleware base mw-log mw-compress)]
  (httpd-new 0 composed))
```

```sweet-exp
load "stdlib/httpd-compress.tur"

let [base     (fn [c :ptr<void>] :nil
                 httpd-resp-status!(c 200)
                 httpd-resp-body!(c large-html))
     composed compose-middleware(base mw-log mw-compress)]
  httpd-new(0 composed)
```

Notes:

- Install as the OUTERMOST post-processing middleware (leftmost in
  `compose-middleware`) so it sees the final body produced by inner
  layers.
- The default threshold is 256 bytes; bodies smaller than that pass
  through uncompressed. Use `mw-compress-with` to pick a different
  minimum (`(mw-compress-with 1024 base)` for a 1 KiB floor).
- Handlers that already set `Content-Encoding` (e.g. served a
  precomputed `.gz` blob) pass through untouched -- mw-compress never
  double-gzips.
- Binary-safe: replaces the response body via
  `httpd-resp-body-bytes!`, so gzip output (which contains embedded
  NUL bytes) is emitted exactly as produced.
- Only `Content-Encoding: gzip` is negotiated. Brotli, zstd, and raw
  deflate are out of scope for v0.1.

See also: the [`tur-zlib` README](https://github.com/turmeric-lang/turmeric-spices/tree/main/spices/zlib).

### mw-recover (MW3)

`mw-recover` runs `next` under `catch-unwind`. When a downstream
handler panics, the unwind is caught at the middleware boundary, the
worker thread survives, and the client gets a `500 Internal Server
Error` instead of a dropped connection.

```turmeric
(let [base     (fn [c : ptr<void>] : nil
                 (httpd-resp-body! c (render-page c)))   ; may panic
      composed (compose-middleware base mw-recover mw-log)]
  (httpd-new 0 composed))
```

```sweet-exp
let [base     (fn [c :ptr<void>] :nil
                 httpd-resp-body!(c render-page(c)))
     composed compose-middleware(base mw-recover mw-log)]
  httpd-new(0 composed)
```

Notes:

- Put it OUTERMOST (leftmost in `compose-middleware`) among the layers
  you want protected -- it can only recover panics raised by handlers
  it wraps. Anything outside it still runs its post-processing on the
  500 response, which is usually what you want for `mw-log`.
- The 500 is only written when the handler had **not** already set a
  status before it panicked. A handler that got as far as
  `(httpd-resp-status! c 201)` and then blew up keeps its own status
  rather than having it silently rewritten.
- It recovers the *request*, not the *state*: a panic partway through a
  handler may have left application state half-updated. Use it as a
  last line of defence, not as control flow.
- The panic message still goes to stderr, so a recovered panic is
  visible in the server log rather than swallowed.

## Request attributes (MW2)

A small per-request key/value side channel attached to the connection.
Useful for middleware that wants to pass context downstream without
mutating headers or body. The store is freed automatically when the
handler returns -- it is not visible to subsequent requests on the
same keep-alive connection.

```turmeric
(httpd-set-attr! c "user" "alice")
(let [u (httpd-req-attr c "user")] ...)   ; => "alice"
(let [x (httpd-req-attr c "missing")] ...) ; => ""
```

```sweet-exp
httpd-set-attr!(c "user" "alice")
let [u httpd-req-attr(c "user")] ...   ; => "alice"
let [x httpd-req-attr(c "missing")] ... ; => ""
```

Attribute keys are case-sensitive plain cstrings. Both `key` and
`val` are copied into per-request storage; the caller may reuse or
free the originals. Keys starting with `__` are conventionally
reserved for internal helpers (`__remote_ip` is the cached client
IP); user code should pick its own non-`__` keys.

## Client IP

```turmeric
(let [ip (httpd-req-remote-ip c)] ...)
```

```sweet-exp
let [ip httpd-req-remote-ip(c)] ...
```

Returns the client's IP as a cstr, derived from `getpeername(2)` on
the connection fd. IPv4 addresses are formatted dotted-quad
("203.0.113.4"); IPv6 colon-hex. The result is cached on the
`__remote_ip` request attribute, so repeated calls within one request
are cheap.

If you sit behind a reverse proxy, read the `X-Forwarded-For` header
via `httpd-req-header` instead -- `httpd-req-remote-ip` reports the
*transport* peer, which is the proxy.

## Writing a middleware

A middleware is just a defn whose last expression is a fat closure.
Capture at least one variable in the outer `let` so the closure is
fat-shaped (which the `httpd-call` dispatcher requires):

```turmeric
(defn mw-add-header [name : cstr value : cstr next : int] : ptr<void>
  (let [_n next
        _k name
        _v value]
    (fn [c : ptr<void>] : nil
      (httpd-call _n c)
      (httpd-resp-header! c _k _v))))
```

```sweet-exp
defn mw-add-header [name :cstr value :cstr next :int] :ptr<void>
  let [_n next
       _k name
       _v value]
    (fn [c :ptr<void>] :nil
      httpd-call(_n c)
      httpd-resp-header!(c _k _v))
```

Short-circuit by *not* calling `(httpd-call _n c)`:

```turmeric
(defn mw-require-https [next : int] : ptr<void>
  (let [_n next]
    (fn [c : ptr<void>] : nil
      (if (= 1 (httpd-req-header? c "X-Forwarded-Proto"))
        (httpd-call _n c)
        (do
          (httpd-resp-status! c 400)
          (httpd-resp-body!   c "HTTPS required"))))))
```

```sweet-exp
defn mw-require-https [next :int] :ptr<void>
  let [_n next]
    (fn [c :ptr<void>] :nil
      (if {1 = httpd-req-header?(c "X-Forwarded-Proto")}
        httpd-call(_n c)
        (do
          httpd-resp-status!(c 400)
          httpd-resp-body!(c "HTTPS required"))))
```

Use request attrs to thread context downstream:

```turmeric
(defn mw-request-id [next : int] : ptr<void>
  (let [_n next]
    (fn [c : ptr<void>] : nil
      (let [id (httpd-req-header c "X-Request-Id")]
        (httpd-set-attr! c "request_id" id)
        (httpd-call _n c)
        (httpd-resp-header! c "X-Request-Id" (httpd-req-attr c "request_id"))))))
```

```sweet-exp
defn mw-request-id [next :int] :ptr<void>
  let [_n next]
    (fn [c :ptr<void>] :nil
      (let [id httpd-req-header(c "X-Request-Id")]
        httpd-set-attr!(c "request_id" id)
        httpd-call(_n c)
        httpd-resp-header!(c "X-Request-Id" httpd-req-attr(c "request_id"))))
```

## Async interop

Middleware composes the same way under `httpd-new-async`: the wrapped
closure runs inside a request fiber instead of on a worker thread,
and `(httpd-await-readable conn)` / `(httpd-await-writable conn)` /
`(httpd-await-timer conn ms)` are usable inside the wrapped handler
to suspend the fiber. See
[httpd-async-guide.md](httpd-async-guide.md) for the full async
model.

The shipped middleware in this catalog is all synchronous -- they
never themselves await -- but they do not block awaiting middleware
written by a user. The fiber-group binding lives on the conn
(`fiber_group` field), so a downstream handler stays fiber-friendly
through any number of middleware wraps.

## Not yet shipped

The following items are tracked in
[`docs/archive/httpd-middleware-plan.md`](https://github.com/turmeric-lang/turmeric/blob/main/docs/archive/httpd-middleware-plan.md)
but not yet in stdlib:

- **`mw-timeout`** -- per-request wall-clock budget. Needs a
  handler/timer race; today's worker-pool path has no portable
  cancellation primitive, and the async path needs a `with-deadline`
  combinator before this can ship cleanly.

## See also

- [httpd-guide.md](httpd-guide.md) -- HTTP/1.1 server primitives
- [httpd-async-guide.md](httpd-async-guide.md) -- async handlers
- [httpd-tls-guide.md](httpd-tls-guide.md) -- TLS via tur-tls spice
- [threading-guide.md](threading-guide.md) -- `Mutex` and worker pool details
