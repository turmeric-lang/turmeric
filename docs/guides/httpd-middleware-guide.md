---
title: HTTP Middleware Catalog (stdlib/httpd)
category: Networking and Web
description: Reference for the shipped httpd middleware -- logging, CORS, basic auth, JSON, cookies, multipart, body-size, rate-limit, static files, security headers, request ids, trusted proxies, ETags, timeouts -- plus the request-attribute side channel and the composition helpers.
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
| `mw-cors` / `mw-cors-opts` / `mw-cors-with` | CORS preflight + Access-Control-Allow-Origin decoration, origin allow-list | M4 |
| `mw-basic-auth`           | HTTP Basic Auth via user verifier closure; publishes `"user"` attr on success | M5 |
| `mw-json-body`            | Pre-parse JSON request body; 400 on malformed input | M3 |
| `mw-body-size`            | Reject requests with Content-Length above a cap (413) | MW1 |
| `mw-rate-limit`           | Sliding-window per-IP rate limiter (429 + Retry-After) | MW2 |
| `mw-static`               | Fall back to static files when `next` returned 404 (with ETag + 304) | MW2 |
| `mw-compress` / `mw-compress-with` | gzip the response body when client sends `Accept-Encoding: gzip` (requires `tur/zlib` spice) | M6 |
| `mw-recover`              | Catch a downstream panic and respond 500; the server keeps serving | MW3 |
| `mw-log-to`               | `mw-log`'s line handed to your own `(fn [cstr] nil)` sink | H4 |
| `mw-secure-headers` / `mw-secure-headers-opts` | nosniff, frame, referrer and opener policies; HSTS and CSP opt-in | H4 |
| `mw-request-id`           | Keep a well-formed `X-Request-Id` or mint one; `httpd-req-id` reads it | H4 |
| `mw-trust-proxy`          | Believe `X-Forwarded-For` / `-Proto` from listed proxies; `httpd-req-ip` / `httpd-req-proto` | H4 |
| `mw-etag`                 | Weak ETag on 200 GET/HEAD responses, 304 on a matching `If-None-Match` | H4 |
| `mw-timeout`              | Answer 503 when a handler overruns its deadline | H4 |

### mw-cors (M4)

Answers a preflight (`OPTIONS` with `Access-Control-Request-Method`) with 204
and the Access-Control-Allow-* headers, without calling `next`, and decorates
every other response. `allow-origin` in `CorsOpts` is `"*"`, one origin, or a
comma-separated list: a listed origin is matched exactly against the request's
`Origin` and echoed back, with `Vary: Origin` (merged into any `Vary` already
set, never repeated); any other origin gets no Access-Control-Allow-Origin,
which is how a browser is told no. `allow-credentials` is a `bool`.

```turmeric
(let [opts     (ok-val (cors-opts "https://app.example.com, https://admin.example.com"
                                  "GET, POST" "Content-Type" "" true 600))
      composed (compose-middleware base (mw-cors-opts opts))]
  (httpd-new 0 composed))
```

```sweet-exp
let [opts     ok-val(cors-opts("https://app.example.com, https://admin.example.com"
                               "GET, POST" "Content-Type" "" true 600))
     composed compose-middleware(base mw-cors-opts(opts))]
  httpd-new(0 composed)
```

`cors-opts` returns `(Result CorsOpts cstr)` and refuses `"*"` with
credentials, which the CORS spec forbids. Options built with `make-struct`
that combine them anyway are served without Allow-Credentials (a line goes to
stderr). The middleware copies its configuration when it is built, so the
strings may be computed and freed afterwards. `mw-cors` is the permissive
development default (`default-cors-opts`).

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

The client is keyed by [`httpd-req-ip`](#client-ip): the transport peer,
or -- with an [`mw-trust-proxy`](#mw-trust-proxy-h4) outside the limiter --
the client address it read from `X-Forwarded-For`. Behind a proxy, put
`mw-trust-proxy` outside `mw-rate-limit`, or every client shares the proxy's
bucket.

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
                   (if (= 1 (cstr-eq-const-time u "admin"))
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
`Vary: Accept-Encoding`, and is otherwise a no-op. It is the
`httpd-compress` module, and the codec is the `tur/zlib` spice: declare the
spice in your `build.tur`, import the module, and it resolves like any other
spice import -- from an installed toolchain too.

```turmeric no-check
:spices #map{"tur-zlib" #map{:url    "https://github.com/turmeric-lang/turmeric-spices"
                             :ref    "v0.1.0"
                             :subdir "spices/zlib"}}
```

```turmeric
(import httpd-compress :refer [mw-compress])

(let [base     (fn [c : ptr<void>] : nil
                 (httpd-resp-status! c 200)
                 (httpd-resp-body!   c large-html))
      composed (compose-middleware base mw-log mw-compress)]
  (httpd-new 0 composed))
```

```sweet-exp
import httpd-compress :refer [mw-compress]

let [base     (fn [c :ptr<void>] :nil
                 httpd-resp-status!(c 200)
                 httpd-resp-body!(c large-html))
     composed compose-middleware(base mw-log mw-compress)]
  httpd-new(0 composed)
```

The `import` goes inside your program's `defmodule`. (Before
tourist-on-stdlib-httpd H4 this was `(load "stdlib/httpd-compress.tur")`, which
loaded the spice from a path relative to a side-by-side turmeric-spices
checkout.)

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

### mw-log / mw-log-to (M1, H4)

`mw-log` prints one line per request to stdout after `next` returns:

```
GET /users/7 -> 200 412 3ms ip=203.0.113.7 rid=6f1c0e2a9b3d4c5e8f7a6b5c4d3e2f10
```

-- method, path, status, response body bytes, elapsed milliseconds, then
` ip=` when an `mw-trust-proxy` outside it resolved the client address and
` rid=` when an `mw-request-id` outside it assigned an id. The method and path
are the client's bytes, so anything outside printable ASCII is written `\xHH`.
`mw-log-to` hands the same line (no newline) to a closure of your own -- a
file, a structured logger, a buffer a test reads:

```turmeric
(let [sink     (fn [line : cstr] : nil (eprintln line))
      composed (compose-middleware base (mw-log-to sink))]
  (httpd-new 0 composed))
```

```sweet-exp
let [sink     (fn [line :cstr] :nil eprintln(line))
     composed compose-middleware(base mw-log-to(sink))]
  httpd-new(0 composed)
```

The line is borrowed: it lives until the request finishes, so copy it to keep
it. The sink runs on whichever worker served the request, so it must be safe
to call from several threads at once.

### mw-secure-headers (H4)

The response headers a browser-facing app should send by default, after
`next` returns, each only if the route did not set it itself:

| Header | Default |
|---|---|
| `X-Content-Type-Options` | `nosniff` |
| `X-Frame-Options` | `DENY` |
| `Referrer-Policy` | `no-referrer` |
| `Cross-Origin-Opener-Policy` | `same-origin` |
| `Strict-Transport-Security` | off (`hsts-max-age` -1) |
| `Content-Security-Policy` | off (`""`) |

HSTS and CSP are opt-in because both can lock users out when set wrong -- a
stray HSTS on a host that still needs plain HTTP, a CSP that blocks your own
scripts. `mw-secure-headers-opts` takes a `SecureHeadersOpts`; `""` switches a
header off:

```turmeric
(let [opts (make-struct SecureHeadersOpts
             true                   ; content-type-options (nosniff)
             "SAMEORIGIN"           ; frame-options
             "strict-origin"        ; referrer-policy
             "same-origin"          ; cross-origin-opener-policy
             31536000               ; hsts-max-age (-1 = no HSTS)
             true                   ; hsts-include-subdomains
             "default-src 'self'")] ; content-security-policy
  (httpd-new 0 (mw-secure-headers-opts opts base)))
```

```sweet-exp
let [opts make-struct(SecureHeadersOpts
             true                   ; content-type-options (nosniff)
             "SAMEORIGIN"           ; frame-options
             "strict-origin"        ; referrer-policy
             "same-origin"          ; cross-origin-opener-policy
             31536000               ; hsts-max-age (-1 = no HSTS)
             true                   ; hsts-include-subdomains
             "default-src 'self'")] ; content-security-policy
  httpd-new(0 mw-secure-headers-opts(opts base))
```

`(default-secure-headers-opts)` returns the defaults in the table above.

### mw-request-id (H4)

Gives every request an id and echoes it as `X-Request-Id`. An incoming
`X-Request-Id` is kept when it is 1-128 characters of `[A-Za-z0-9._-]` -- so an
id a load balancer assigned follows the request through -- and anything else
(missing, too long, a space or quote in it) is replaced by 32 random hex
digits. Downstream code reads it with `httpd-req-id`, which is `none` without
the middleware:

```turmeric
(let [handler (fn [c : ptr<void>] : nil
                (let [id (httpd-req-id c)]
                  (when (some? id) (log-with-id (unwrap id)))))]
  (httpd-new 0 (mw-request-id handler)))
```

```sweet-exp
let [handler (fn [c :ptr<void>] :nil
               (let [id httpd-req-id(c)]
                 (when some?(id) log-with-id(unwrap(id)))))]
  httpd-new(0 mw-request-id(handler))
```

Put it outside `mw-log` / `mw-log-to` and the log line carries ` rid=<id>`.

### mw-trust-proxy (H4)

Behind a reverse proxy the transport peer is the proxy. `mw-trust-proxy`
lets the peers you list tell the server who the client is:

```turmeric
(let [tp (trust-proxy-opts "loopback, 10.0.0.0/8")]
  (when (ok? tp)
    (httpd-new 0 (mw-trust-proxy (ok-val tp) base))))
```

```sweet-exp
let [tp trust-proxy-opts("loopback, 10.0.0.0/8")]
  when ok?(tp)
    httpd-new(0 mw-trust-proxy(ok-val(tp) base))
```

- `trusted` is a comma-separated list of addresses (`10.0.0.5`, `::1`), CIDR
  blocks (`10.0.0.0/8`, `fd00::/8`) and the keywords `loopback`, `linklocal`
  and `uniquelocal`. `trust-proxy-opts` returns `err` naming an entry that is
  none of those; `(default-trust-proxy-opts)` trusts nothing.
- A peer that is not trusted is the client, and its `X-Forwarded-For` and
  `X-Forwarded-Proto` are ignored -- anyone can send them.
- A trusted peer's `X-Forwarded-For` is read right to left: hops that are
  themselves trusted proxies are skipped, and the first that is not is the
  client. (The leftmost entries are whatever the client wrote, so they are
  never believed over a trusted hop's word.) A hop that is not an address
  ends the walk at the last good one.
- `X-Forwarded-Proto` is believed from a trusted peer, as `http` or `https`
  only.

`httpd-req-ip` and `httpd-req-proto` read the result, falling back to the
peer address and the connection's own protocol when the middleware did not
run. `mw-rate-limit` keys on `httpd-req-ip`, and `mw-log` adds ` ip=`.

### mw-etag (H4)

After `next` returns a 200 to a GET or HEAD, sets a weak ETag computed from
the body, and answers a matching `If-None-Match` with `304 Not Modified` and
no body:

```turmeric
(httpd-new 0 (compose-middleware api-handler mw-etag))
```

```sweet-exp
httpd-new(0 compose-middleware(api-handler mw-etag))
```

- The tag is `W/"<length>-<FNV-1a 64 hash>"` -- weak, because it promises
  equal bytes, not equal encodings: put `mw-compress` *outside* `mw-etag`.
- A route that set its own `ETag` keeps it, and `If-None-Match` is checked
  against that one. Comparison is weak (`W/` ignored on both sides), `*`
  matches, and a comma-separated list matches if any entry does.
- The body is still built every time; `mw-etag` saves the transfer, not the
  work. `mw-static` does the same for files, from their size and mtime.

Independent of `mw-etag`, the server never writes a body on a `1xx`, `204` or
`304` response (one a handler set is dropped, along with `Content-Length`),
and answers `HEAD` with the headers a GET would get -- `Content-Length`
included -- and no body.

### mw-timeout (H4)

Answers `503 Service Unavailable` when a handler takes longer than `ms`
milliseconds:

```turmeric
(httpd-new-pool 0 8 (mw-timeout 2000 slow-report-handler))
```

```sweet-exp
httpd-new-pool(0 8 mw-timeout(2000 slow-report-handler))
```

`next` runs on a helper thread against a private copy of the request (body,
headers, attributes, route params); the worker waits for it up to the
deadline. In time, the copy's response -- status, headers, body, attributes --
moves onto the real request as if `next` had run in place. Late, the client
gets the 503 at the deadline. Know what that does and does not do:

- **The handler is not stopped.** Threads cannot be cancelled safely, so a
  late handler runs to completion on its helper thread; the deadline frees
  the *client* and the worker, not the CPU. Its response is discarded, and it
  cannot reach the client (the copy has no socket) or upgrade the
  connection.
- Let late handlers finish before `httpd-free`, which releases the closures
  they are running.
- On `httpd-new-async` it runs `next` inline with no deadline: a helper
  thread may not touch the fiber's reactor.

Place it inside the middleware whose post-processing should see the 503
(`mw-log`, `mw-recover`) and outside the handler work it bounds.

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
free the originals. Keys starting with `__` are reserved for the
built-ins (`__remote_ip` is the cached peer address, `__client_ip` /
`__client_proto` what `mw-trust-proxy` resolved, `__request_id` the
`mw-request-id` id); user code should pick its own non-`__` keys.

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

`httpd-req-remote-ip` is always the *transport* peer -- behind a reverse
proxy, the proxy. `httpd-req-ip` is the client: what
[`mw-trust-proxy`](#mw-trust-proxy-h4) resolved from `X-Forwarded-For`, or the
peer when it did not run. Use `httpd-req-ip` for anything about the client
(logging, rate limits, allow-lists); never read `X-Forwarded-For` yourself
without checking who sent it.

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

Pick the header setter by what the header means, because a route or another
middleware may have set it already: `httpd-resp-header!` replaces (one-valued
headers such as `Content-Type`, `ETag`), `httpd-resp-header-add!` appends
(`Set-Cookie`), and `httpd-resp-vary!` adds a token to `Vary` only if it is not
listed yet -- several middleware add to `Vary`, and repeating it is wrong.

Use request attrs to thread context downstream:

```turmeric
(defn mw-tenant [next : int] : ptr<void>
  (let [_n next]
    (fn [c : ptr<void>] : nil
      (let [t (httpd-req-header c "X-Tenant")]
        (httpd-set-attr! c "tenant" t)
        (httpd-call _n c)
        (httpd-resp-header! c "X-Tenant" (httpd-req-attr c "tenant"))))))
```

```sweet-exp
defn mw-tenant [next :int] :ptr<void>
  let [_n next]
    (fn [c :ptr<void>] :nil
      (let [t httpd-req-header(c "X-Tenant")]
        httpd-set-attr!(c "tenant" t)
        httpd-call(_n c)
        httpd-resp-header!(c "X-Tenant" httpd-req-attr(c "tenant"))))
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

- **A deadline for `mw-timeout` on the async server.** It runs `next` inline
  there today; bounding a fiber needs a `with-deadline` combinator on the
  reactor.
- **Cancelling a late handler.** `mw-timeout` answers the client at the
  deadline but cannot stop the handler (see [mw-timeout](#mw-timeout-h4)).

## See also

- [httpd-guide.md](httpd-guide.md) -- HTTP/1.1 server primitives
- [httpd-async-guide.md](httpd-async-guide.md) -- async handlers
- [httpd-tls-guide.md](httpd-tls-guide.md) -- TLS via tur-tls spice
- [threading-guide.md](threading-guide.md) -- `Mutex` and worker pool details
