# httpd Middleware Plan

> **Status:** Partially shipped -- forward-looking items remain
> **Last Updated:** 2026-08-21 (MW3 mw-recover shipped; mw-timeout still deferred)
> **Type:** Networking / stdlib
> **Supersedes (historically):** the bulk of the original v0 surface was
> reframed and shipped under the now-archived
> [httpd-middleware-async-plan](../archive/history/httpd-middleware-async-plan.md)
> Track M (M0-M8). This document tracks the remainder.

---

## Overview

`stdlib/httpd` already documents a middleware pattern (see
`docs/guides/httpd-guide.md` §Middleware H7): a middleware is a function
that takes the `next` handler and returns a new handler that wraps it.
The `httpd-call` primitive invokes a stored handler closure.

The first wave of middleware (logging, CORS, basic auth, cookies, body
parsers, multipart) shipped as Track M of the archived
`httpd-middleware-async-plan`. This plan now tracks the **remaining**
middleware -- rate limiting, static files, body-size enforcement,
timeouts, panic recovery -- plus the request-attribute storage that
several of them depend on.

The primitives (`httpd-call`, `router-mw`, handler calling convention)
are **not changed**. Everything added here is pure Turmeric library code
on top of the existing C layer.

---

## Current Progress (2026-06-02)

Naming note: shipped middleware use the `mw-*` prefix convention (e.g.
`mw-log`, `mw-cors`) rather than the `*-mw` suffix the original draft
proposed. The composition helper landed as `compose-middleware` (with a
typed variadic `compose-middleware-of`), not `compose-mw`.

| Original draft item        | Status     | Shipped as                                                  |
|----------------------------|------------|-------------------------------------------------------------|
| `compose-mw`               | Shipped    | `compose-middleware` macro + `compose-middleware-of` (M8)   |
| Logging middleware         | Shipped    | `mw-log` in `stdlib/httpd.tur` (M1)                         |
| CORS middleware            | Shipped    | `mw-cors`, `mw-cors-with`, `default-cors-opts` (M4)         |
| Basic-auth middleware      | Shipped    | `mw-basic-auth` with verifier closure (M5)                  |
| Body parsers (urlencoded + JSON) | Shipped (bonus) | `httpd-req-form`, `httpd-req-json`, `mw-json-body` (M3) |
| Cookie read/write          | Shipped (bonus) | `httpd-req-cookie`, `httpd-set-cookie!`, `CookieOpts` (M2) |
| Multipart parser           | Shipped (bonus) | `httpd-req-multipart-parse` (M7)                       |
| Request/response headers   | Shipped (bonus) | `httpd-req-header(?)`, `httpd-resp-header(-add)!` (M0) |
| Async handlers + middleware compose | Shipped (bonus) | Track A (A0-A4); see `httpd-async-guide.md`      |
| Body-size middleware       | Shipped    | `mw-body-size` in `stdlib/httpd.tur` (MW1); fixture `httpd-mw-body-size` |
| Rate-limit middleware      | Shipped    | `mw-rate-limit` + `RateLimitOpts` (MW2); fixture `httpd-mw-rate-limit` |
| Static-file middleware     | Shipped    | `mw-static` (MW2); fixture `httpd-mw-static`               |
| **Timeout middleware**     | **Pending** (blocked) | -- needs handler/timer race via async fiber primitives; worker-thread cancellation is not portable (no `pthread_timedjoin_np` on macOS) |
| **Panic-recovery middleware** | **Pending** (blocked) | -- requires usable `catch-unwind` over fat-closure thunks (today's lowering passes `NULL` env -- see `src/compiler/emit_expr.c` `EX_CATCH_UNWIND`) |
| Request attributes (`httpd-set-attr!` / `httpd-req-attr`) | Shipped | MW2; `attr_list` field on `HttpdConn`; `mw-basic-auth` publishes `"user"`; fixture `httpd-mw-basic-auth-attr` |
| Remote-IP helper (`httpd-req-remote-ip`) | Shipped (bonus) | MW2; backs `mw-rate-limit`; cached on the request attr `__remote_ip` |
| Compression middleware     | Spun out   | See [httpd-compression-zlib-spice-plan](history/httpd-compression-zlib-spice-plan.md) |

Fixtures landed alongside each shipped middleware under
`tests/fixtures/httpd-mw-*` and `tests/fixtures/httpd-async-*`. A
dedicated guide `docs/guides/httpd-middleware-guide.md` is **not yet
written** -- see Phase MW3 below.

---

## Goals / Non-Goals

### Goals (remaining v0)

- **Body-size middleware** -- reject requests whose `Content-Length`
  exceeds a configured limit with 413.
- **Rate-limit middleware** -- sliding-window token bucket per IP (stored
  in a `Mutex<hamt>` of counters); returns 429 on exhaustion.
- **Static-file middleware** -- serve files from a root directory; sets
  correct `Content-Type` from extension; returns 304 on matching
  `If-None-Match` / `ETag`.
- **Timeout middleware** -- enforce a per-request wall-clock budget;
  triggers a 503 if the downstream handler has not written a status
  after N ms. (Likely best built on the Track A await primitives so the
  timeout fires off the reactor rather than a sleeping worker.)
- **Panic-recovery middleware** -- catch a downstream panic / error
  signal, log it, and return a 500 rather than crashing the worker.
- `(httpd-req-attr conn key)` and `(httpd-set-attr! conn key val)` --
  attach arbitrary key/value context to a request (e.g. for auth
  middleware to pass the authenticated user name downstream; today
  `mw-basic-auth` runs its verifier inline and does not propagate the
  username).
- Dedicated middleware guide
  `docs/guides/httpd-middleware-guide.md` collecting all of the above.

### Non-Goals

- No streaming response body; body is still buffered in the existing
  `httpd-resp-body!` string. (Track A adds awaitable I/O primitives but
  the response body remains a single buffer.)
- No gzip / brotli response compression here -- tracked separately in
  [httpd-compression-zlib-spice-plan](history/httpd-compression-zlib-spice-plan.md).
- No session / cookie store (build on top of cookies + request attrs).
- No JWT validation (composable in user code using inline-C + mbedtls).
- No HTTP/2 push.

---

## API Surface (remaining items)

### Body-size middleware

```turmeric
;;; mw-body-size -- reject requests with Content-Length > max-bytes with 413.
;;;
;;; Parameters:
;;;   max-bytes -- maximum accepted body size
;;;   next      -- downstream handler
;;;
;;; Since: Phase MW1 (pending)
(defn mw-body-size [max-bytes : int next : int] : int ...)
```

### Panic-recovery middleware

```turmeric
;;; mw-recover -- catch panics from downstream; respond 500 and log.
;;;
;;; Parameters:
;;;   next -- downstream handler
;;;
;;; Since: Phase MW1 (pending)
(defn mw-recover [next : int] : int ...)
```

### Rate-limit middleware

```turmeric
(defstruct RateLimitOpts
  [requests : int   ;; max requests per window
   window-s : int   ;; window size in seconds
  ])

;;; mw-rate-limit -- sliding-window IP-based rate limiter.
;;;
;;; Returns 429 with a Retry-After header when the limit is exceeded.
;;;
;;; Parameters:
;;;   opts -- RateLimitOpts
;;;   next -- downstream handler
;;;
;;; Since: Phase MW2 (pending)
(defn mw-rate-limit [opts : RateLimitOpts next : int] : int ...)
```

### Static-file middleware

```turmeric
;;; mw-static -- serve files from root-dir for requests not handled by next.
;;;
;;; Checks next first; only serves a file if next returns 404.
;;; Sets Content-Type based on file extension. Returns 304 on ETag match.
;;;
;;; Parameters:
;;;   root-dir -- filesystem path to serve files from
;;;   next     -- downstream handler (usually a router)
;;;
;;; Since: Phase MW2 (pending)
(defn mw-static [root-dir : cstr next : int] : int ...)
```

### Timeout middleware

```turmeric
;;; mw-timeout -- enforce a per-request wall-clock budget.
;;;
;;; If next has not written a status after deadline-ms, the middleware
;;; emits 503 and cancels the downstream fiber. Requires Track A async
;;; handlers (httpd-await-timer) for non-blocking enforcement.
;;;
;;; Parameters:
;;;   deadline-ms -- per-request budget in milliseconds
;;;   next        -- downstream handler
;;;
;;; Since: Phase MW2 (pending, depends on Track A)
(defn mw-timeout [deadline-ms : int next : int] : int ...)
```

### Request attributes

Added to `stdlib/httpd`:

```turmeric
;;; httpd-set-attr! -- attach a key/value pair to a request.
;;;
;;; Parameters:
;;;   conn -- httpd connection pointer
;;;   key  -- attribute name
;;;   val  -- attribute value (cstr)
;;;
;;; Since: Phase MW2 (pending)
(defn httpd-set-attr! [conn : ptr<void> key : cstr val : cstr] : void ...)

;;; httpd-req-attr -- retrieve a previously set request attribute.
;;;
;;; Returns the empty string if the key has not been set.
;;;
;;; Parameters:
;;;   conn -- httpd connection pointer
;;;   key  -- attribute name
;;;
;;; Since: Phase MW2 (pending)
(defn httpd-req-attr [conn : ptr<void> key : cstr] : cstr ...)
```

Once shipped, update `mw-basic-auth` (already in tree) to call
`httpd-set-attr! conn "user" <verified-username>` so handlers can read
back the authenticated principal.

---

## Request attribute storage

Request attributes are stored in a small singly-linked list of
`(key . val)` pairs pinned to the `httpd_conn` struct. The conn struct
gains one `int64_t attr_list` field (a `cons` list, or 0). Field is
zeroed on connection accept and freed (shallow) after the handler
returns. This mirrors the response-header list added in M0.

This avoids a heap allocation for the common case (zero or one
attribute); deeper attribute maps are rare and the linear scan cost is
negligible for single-digit key counts.

---

## Phases (remaining)

### Phase MW1 -- Simple wrappers (no C changes)

- **Shipped:** `mw-body-size` in `stdlib/httpd.tur` rejects requests
  whose `Content-Length` exceeds the configured cap with a 413
  short-circuit. Internal helper `httpd-mw-content-length` parses the
  header to int (or -1 when absent / unparseable). Fixture
  `tests/fixtures/httpd-mw-body-size/` covers both the over-cap (413)
  and within-cap (200) paths and asserts the base handler is bypassed
  on the over-cap request.
- **Shipped (2026-08-21, as MW3):** `mw-recover`. The unblock landed in
  two parts: `EX_CATCH_UNWIND` / `EX_CATCH_PANIC_OF` gained a case in
  `collect_free_vars` (so a lifted catch thunk threads the enclosing
  names it references into its env), and the drop glue learned to skip
  `TUR_CLOSURE_DROP` for a catch thunk's borrowed `^fat` captures. See
  `stdlib/httpd.tur` (MW3) and `tests/fixtures/httpd-mw-recover/`, which
  sends two requests -- the panicking one and a following good one --
  because "the server survived" is the property that matters.

### Phase MW2 -- State-carrying middleware + request attrs

- **Shipped:** request attrs (`httpd-set-attr!` / `httpd-req-attr`) +
  `attr_list` field on `HttpdConn` + per-request cleanup in the worker
  loop (same shape as the M0 header list).
- **Shipped:** `httpd-req-remote-ip` (uses `getpeername` + `inet_ntop`;
  result cached on the `__remote_ip` request attr) -- the foundation
  for IP-based rate-limiting.
- **Shipped:** `mw-rate-limit` with a per-instance heap-allocated
  table (linear probing, 1024 slots) protected by a `pthread_mutex`.
  Sliding window per IP (FNV-1a hash); 429 + `Retry-After` on
  exhaustion. Note: the plan called for `Mutex<hamt>`; the shipped
  implementation uses a simpler fixed-size table that's easier to
  reason about and avoids dragging in stdlib/hamt into the closure
  environment. Capacity is fixed at 1024 -- the table fails open when
  full, which is acceptable for a v1 limiter.
- **Shipped:** `mw-static` with ETag (`"<size>-<mtime>"` in hex). Path
  traversal guard rejects any `..` segment. Content-Type from a small
  built-in extension table (`html`, `css`, `js`, `json`, `txt`, `png`,
  `jpg`/`jpeg`, `gif`, `svg`, `ico`, `wasm`). Composes after a router:
  serves a file only when `next` returned 404.
- **Shipped:** `mw-basic-auth` now publishes the verified username via
  `httpd-set-attr! "user"`; integration fixture
  `httpd-mw-basic-auth-attr` demonstrates the handler reading it back.
- **Deferred:** `mw-timeout`. The plan suggested building on
  `httpd-await-timer`, but enforcing a per-request budget requires
  racing the handler against a timer. The async path
  (`local-fiber-group` + `tur-local-spawn`) has the primitives to
  spawn the handler and cancel it on timeout, but composing that into
  a generic middleware that works under both `httpd-new-pool` (worker
  threads) and `httpd-new-async` (fibers) is non-trivial.
  Worker-thread cancellation via `pthread_timedjoin_np` is not
  portable (macOS lacks it) and would not unwind handler state
  cleanly. Revisit after the async path adds a `with-deadline` /
  `race` combinator.

### Phase MW3 -- Documentation

- **Shipped:** `docs/guides/httpd-middleware-guide.md` -- catalog of
  every shipped middleware with usage example, the request-attribute
  side channel, the rules for writing your own, async interop, and
  the deferred items.
- **Shipped:** `httpd-guide.md` §Middleware now links to the catalog.

---

## Resolved Decisions

1. **No new framework.** Everything is plain Turmeric function
   composition over the existing `httpd-call` primitive. The only new
   C-layer addition is the `attr_list` field on `httpd_conn` (mirroring
   the M0 header lists).
2. **`compose-middleware` argument order.** First argument = outermost
   (wraps last, runs first). Already shipped; matches Ring (Clojure) /
   Rack (Ruby): `(compose-middleware base mw-log mw-cors)` logs first
   on the way in and last on the way out.
3. **Static-file fallback.** `mw-static` defers to `next` first and only
   serves a file if `next` returned 404. This makes it safe to compose
   after a router without having to list every route.
4. **Rate-limit state.** The `Mutex<hamt>` lives in the closure captured
   by `mw-rate-limit`. Multiple server instances (or multiple calls to
   `mw-rate-limit`) get independent counters; sharing state is left to
   the caller.
5. **Timeout enforcement.** Built on Track A's `httpd-await-timer`
   rather than a worker-side sleep, so the timeout costs no thread.
6. **Naming.** All middleware use the `mw-*` prefix to match the
   already-shipped `mw-log` / `mw-cors` / `mw-basic-auth` / `mw-json-body`.

---

## See Also

- [httpd-guide.md](../guides/httpd-guide.md) -- HTTP/1.1 server primitives
- [httpd-tls-guide.md](../guides/httpd-tls-guide.md) -- TLS layer
- [httpd-async-guide.md](../guides/httpd-async-guide.md) -- async handlers (Track A)
- [httpd-middleware-async-plan.md](../archive/history/httpd-middleware-async-plan.md) -- archived plan that shipped M0-M8 + A0-A4
- [history/httpd-compression-zlib-spice-plan.md](history/httpd-compression-zlib-spice-plan.md) -- compression middleware spun out into its own plan
- [curried-call-cast-rough-edges-plan.md](curried-call-cast-rough-edges-plan.md) -- codegen rough edges surfaced during M4+M5
- [websocket-server-plan.md](websocket-server-plan.md) -- WebSocket upgrade (compatible with middleware)
- [threading-guide.md](../guides/threading-guide.md) -- `Mutex` and worker pool details
