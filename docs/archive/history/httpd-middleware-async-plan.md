# Plan: `tur/httpd` -- Standard Middleware Library + Async Handlers

> **Status:** Draft Plan
> **Last Updated:** 2026-06-02
> **Type:** Stdlib Design + Implementation Roadmap
> **Related:**
> - `stdlib/httpd.tur` -- `tur/httpd` server (Phase H1-H7 shipped)
> - `docs/archive/history/tur-httpd-stdlib-plan.md` -- original H1-H7 roadmap
> - `docs/archive/history/tur-reactor-plan.md` -- event loop backing the listener (R1-R6 complete)
> - `docs/archive/history/reactor-run-fibers-plan.md` -- local fiber driver on the reactor (shipped, F1-F8)
> - `docs/guides/httpd-guide.md`, `docs/guides/reactor-guide.md`, `docs/guides/async-await-guide.md`
> - `src/async/reactor.c`, `src/async/reactor.h`, `src/async/io_epoll.c`, `src/async/io_kqueue.c`

---

## Overview

`tur/httpd` is functional through **Phase H7**: a reactor-backed listener
thread, a fixed worker pool draining a shared fd queue, HTTP/1.1 keep-alive,
a small routing DSL (H6), and a *middleware composition convention* (H7).
What H7 shipped is the **hook** -- `httpd-call` plus the
`(fn [next] (fn [conn] ...))` wrapping convention -- not an actual library
of middleware. This plan fills that gap and adds async handler support.

It covers two tracks, which can ship independently:

1. **Track M -- Standard middleware library.** Cookie, CORS, JSON body
   parser, URL-encoded body parser, compression, request logging, HTTP
   Basic Auth, and multipart form upload, built on the H7 convention.

2. **Track A -- Asynchronous handlers.** Let a handler suspend on reactor
   I/O (a downstream socket, a timer, a channel) and resume without tying
   up a worker thread, now that the reactor (R1-R6) is complete.

### What exists today (the substrate)

`HttpdConn` (per-request, on the worker stack) currently carries:

```
{ int fd; int64_t handler;
  char method[16]; char path[1024]; char version[16];
  char *body; int body_len;
  int resp_status; char *resp_body; int resp_body_len;
  int client_keep_alive;
  void *route_params; }   // RouteParam linked list (H6)
```

Request accessors: `httpd-req-method`, `-path`, `-version`, `-body`,
`-body-len`. Response setters: `httpd-resp-status!`, `httpd-resp-body!`,
plus `httpd-resp-status-get` (H6). Middleware hook: `httpd-call` (H7).

### The gap that blocks half the middleware

The request reader scans headers **only** for `Connection` and
`Content-Length` (`stdlib/httpd.tur:234`) and discards the rest. The
response writer hardcodes exactly two headers
(`stdlib/httpd.tur:310-311`):

```c
"%s %d %s\r\nContent-Length: %d\r\nConnection: %s\r\n\r\n"
```

There is **no general request-header accessor and no response-header
setter**. Cookie parsing reads `Cookie:`; CORS reads `Origin:` and writes
`Access-Control-*`; Basic Auth reads `Authorization:` and writes
`WWW-Authenticate:`; compression reads `Accept-Encoding:` and writes
`Content-Encoding:`; the body parsers branch on `Content-Type:`; multipart
needs the `boundary` parameter from `Content-Type:`. Every one of these
needs arbitrary header read/write. So that capability is **Phase M0**, a
hard prerequisite for M2-M8.

---

## Track M -- Standard Middleware Library

### Phase M0 -- Request/response header access (prerequisite)

Extend `HttpdConn` and the read/write paths so middleware can read any
request header and set arbitrary response headers (including repeated
headers such as `Set-Cookie`).

- **Storage** (resolved, OQ1). The current read buffer is a *stack*
  `char buf[8192]` in `httpd-handle` (`stdlib/httpd.tur:172`): it is valid
  during the handler call but overwritten on the next keep-alive request and
  capped at 8 KB. Rather than index into it, **parse request headers into a
  conn-owned list** of `{name, value}` freed at end of request, and **make
  the read buffer growable** so large header blocks no longer truncate.
  Response headers accumulate in the same kind of conn-owned growable list,
  also freed at end of request.
- **Request API**
  - `(httpd-req-header conn :ptr<void> name :cstr) :cstr` -- case-insensitive
    lookup; empty string when absent.
  - `(httpd-req-header? conn :ptr<void> name :cstr) :int` -- presence test.
- **Response API**
  - `(httpd-resp-header! conn :ptr<void> name :cstr value :cstr) :nil` --
    set/replace.
  - `(httpd-resp-header-add! conn :ptr<void> name :cstr value :cstr) :nil` --
    append (for `Set-Cookie`, `Vary`, etc.).
- **Writer change.** Emit accumulated response headers after the hardcoded
  `Content-Length`/`Connection` line; suppress a user-set `Content-Length`
  to avoid duplication. Default `Content-Type: text/plain; charset=utf-8`
  unless the handler set one.

This phase is the only one that touches the C request/response core; M1-M8
are pure Turmeric on top of it.

### Phase M1 -- Logging middleware

Pure H7 wrapper, needs nothing beyond existing accessors -- ship first as
the reference middleware.

```turmeric
(defn mw-log [next : int] : int
  (fn [c : ptr<void>] : nil
    (let [start (mono-ms)]
      (httpd-call next c)
      (println (str-concat (httpd-req-method c)
                 (str-concat " " (str-concat (httpd-req-path c)
                   (str-concat " -> " (int->cstr (httpd-resp-status-get c))))))))))
```

- Common Log / combined-ish single line: method, path, status, byte count,
  elapsed ms.
- Time source: reuse the reactor's `CLOCK_MONOTONIC` ms helper.
- Optional `mw-log-with` taking a sink closure so callers can route to a
  file/logger instead of stdout (defaults baked via partial application,
  per the arity style guide).

### Phase M2 -- Cookie middleware (depends on M0)

- `(httpd-req-cookie conn :ptr<void> name :cstr) :cstr` -- parse the
  `Cookie:` header (`a=1; b=2`) and return one value.
- `(httpd-set-cookie! conn :ptr<void> opts :CookieOpts) :nil` -- emit a
  `Set-Cookie` via `httpd-resp-header-add!`.
- `CookieOpts` defstruct (per the >5-param rule): `name`, `value`, `path`,
  `domain`, `max-age`, `secure`, `http-only`, `same-site`. Provide
  `default-cookie-opts` and currying-friendly constructors.

### Phase M3 -- Body parsers: JSON + URL-encoded (depend on M0)

Both branch on `Content-Type:` and read `httpd-req-body`.

- **URL-encoded** (`application/x-www-form-urlencoded`): pure Turmeric
  percent-decode + `&`/`=` split into an assoc structure.
  - `(httpd-req-form conn :ptr<void> field :cstr) :cstr`
  - `(httpd-req-form-all conn :ptr<void>) :ptr<void>` (map/list of pairs).
- **JSON** (`application/json`): delegate to the existing `json` stdlib
  module. `json/decode [s :cstr] :int` (`stdlib/json.tur:651`) already
  returns a heap node tree (or `0` on parse error) with a clean recursive
  `json/free [node :int] :void` (`stdlib/json.tur:694`).
  - `(httpd-req-json conn :ptr<void>) :ptr<void>` -- parsed JSON value, or
    a none/err on malformed input. **Per-request lifetime (resolved, OQ4):**
    a thin wrapper caches the decoded tree on the conn and `json/free`s it in
    the iteration cleanup (alongside `conn->body`), so handlers never free it
    and the leak-checked `build` path stays clean.
  - `mw-json-body` variant that pre-parses and 400s on malformed JSON so
    handlers can assume a valid document.
- Guardrails: respect a configurable max body size; the worker already
  bounds reads by `Content-Length`.

### Phase M4 -- CORS middleware (depends on M0)

- `mw-cors` configured by a `CorsOpts` defstruct: `allow-origin`,
  `allow-methods`, `allow-headers`, `expose-headers`, `allow-credentials`,
  `max-age`.
- Reads `Origin:`; on a preflight (`OPTIONS` + `Access-Control-Request-*`)
  short-circuits with 204 and the `Access-Control-Allow-*` headers without
  calling `next`; otherwise calls `next` and decorates the response.
- `default-cors-opts` = permissive `*` for development; document the
  credentials caveat (`*` + credentials is invalid).

### Phase M5 -- HTTP Basic Auth middleware (depends on M0)

- `mw-basic-auth` taking a realm and a verifier closure
  `(fn [user :cstr pass :cstr] :int)`.
- Reads `Authorization: Basic <base64>`, base64-decodes (add a small
  `base64-decode` helper to stdlib if absent), splits on `:`, calls the
  verifier. On failure: 401 + `WWW-Authenticate: Basic realm="..."` and
  does **not** call `next`.
- Constant-time compare helper for the credential check to avoid timing
  leaks; document that Basic Auth must run over TLS (point at
  `httpd-new-tls`, Phase H5).

### Phase M6 -- Compression middleware (depends on M0)

- `mw-compress`: reads `Accept-Encoding:`, and when the response body
  exceeds a threshold and the negotiated codec is supported, compresses
  `resp_body`, sets `Content-Encoding:` and `Vary: Accept-Encoding`, and
  rewrites `Content-Length`.
- **Codec sourcing (resolved, OQ2).** Turmeric stdlib has no deflate/gzip
  anywhere. Source `gzip` via an optional `tur/zlib` spice in
  `../turmeric-spices` (link against system zlib); compile the middleware
  only when present and gate its fixtures behind `requires.spices`. No
  pure-Turmeric deflate fallback -- when the spice is absent the middleware
  is simply unavailable.
- Runs as the **outermost** post-processing middleware so it sees the
  final body. Skip already-encoded or tiny bodies.

### Phase M7 -- Multipart form upload (depends on M0, M3)

- `multipart/form-data` parser keyed off the `boundary` parameter in
  `Content-Type:`.
- Streaming-friendly API over the already-buffered body:
  - `(httpd-req-multipart conn :ptr<void>) :ptr<void>` -- list of parts.
  - `Part` defstruct: `name`, `filename`, `content-type`, `data`,
    `data-len`.
  - Convenience: `(httpd-req-file conn :ptr<void> field :cstr) :ptr<void>`.
- Enforce a max upload size and a max part count; document that the current
  worker buffers the whole body (true streaming uploads land with Track A).

### Phase M8 -- Composition helper + docs

- Land `compose-middleware` (referenced in the H7 header comment but not
  yet implemented): apply a list left-to-right so
  `(compose-middleware [mw-log mw-cors mw-auth] base)` reads in execution
  order.
- A `mw-stack` convenience that wires the common default stack
  (log -> cors -> body-parse) for quick starts.
- New guide `docs/guides/httpd-middleware-guide.md` with a worked example
  per middleware; fixtures under `tests/fixtures/httpd-mw-*`.

### Ordering / dependencies (Track M)

```
M1 (logging, no deps)
M0 (headers)  -->  M2 cookie
                   M3 body parsers  --> M7 multipart
                   M4 cors
                   M5 basic auth
                   M6 compression
                                       M8 compose + docs
```

Ship M1 immediately (proves the convention), then M0, then M2-M7 in any
order, M8 last.

---

## Track A -- Asynchronous Handlers

### Motivation

Today a worker thread is occupied for the entire lifetime of a request. A
handler that calls a slow upstream (DB, another HTTP service) blocks its
worker, so throughput is capped at `n_workers` concurrent slow requests.
The reactor (R1-R6) already multiplexes fd/timer/signal/channel readiness
on a single thread; Track A lets a handler **yield** to the reactor while
waiting and resume later, freeing the worker.

### Design options

There are two viable suspension mechanisms; the plan recommends starting
with A2 because the fiber substrate already exists.

- **A1 -- Callback/continuation handlers.** A handler returns a "pending"
  marker instead of a body and registers a reactor callback that completes
  the response later. Lowest-level, no fiber machinery, but inverts control
  flow and complicates keep-alive bookkeeping (the worker must not write
  the response or recycle the conn until completion).

- **A2 -- Fiber-per-request on a reactor-driven scheduler (recommended).**
  Run handlers as fibers (the `reactor-run-fibers` driver, F1-F8, already
  ships). An `await`-style call suspends the fiber, registers interest with
  the reactor, and resumes the fiber on readiness. The handler stays
  straight-line code; the worker model changes from "thread blocks on
  handler" to "reactor thread drives many in-flight fibers."

### Phase A0 -- Non-blocking conn I/O primitives

- Make the client socket non-blocking for async handlers and expose:
  - `(httpd-await-readable conn :ptr<void>) :nil`
  - `(httpd-await-writable conn :ptr<void>) :nil`
  - `(httpd-await-timer ms :int) :nil`
  - `(httpd-await-chan ch :ptr<void>) :int`
  Each registers the corresponding reactor source and suspends the current
  fiber until the callback fires, then resumes.
- These are the building blocks; everything else composes from them.

### Phase A1 -- Async execution model

- Introduce an **async server constructor** that runs handlers as fibers on
  a reactor instead of (or alongside) the blocking worker pool:
  - `(httpd-new-async port :int handler :int) :ptr<void>`
  - Internally: the listener reactor accepts; each accepted fd spawns a
    request fiber via the F1-F8 local-fiber driver on the **same** reactor
    thread. **Threading (resolved, OQ3):** start with a single reactor
    thread and one `LocalFiberGroup` (the fiber driver's natural shape --
    one group binds to one reactor, no work-stealing); measure before
    sharding into a pool of reactor threads.
- A request fiber: read (awaiting readability as needed) -> run handler ->
  write (awaiting writability) -> keep-alive loop or close.
- Keep the existing thread-pool path unchanged and default; async is opt-in
  so synchronous handlers keep their simple model.

### Phase A2 -- `await` surface for handlers

- Reuse the `async-await` guide's surface so handler code reads
  synchronously:

  ```turmeric
  (defn handler [c : ptr<void>] : nil
    (let [row (await (db-query c "SELECT ..."))]   ;; suspends, frees the thread
      (httpd-resp-body! c (render row))))
  ```

- Provide an async HTTP client helper (or document using
  `reactor-add-fd` + `httpd-await-*`) so a handler can fan out to upstreams
  without blocking.
- Cancellation/timeout: integrate `httpd-await-timer` with a per-request
  deadline so a stuck upstream cannot pin a fiber forever.

### Phase A3 -- Middleware interop

- Confirm Track M middleware composes over async handlers unchanged: the
  `(fn [next] (fn [conn] ...))` shape is agnostic to whether `next`
  suspends, as long as `httpd-call` is on the fiber stack (it is -- it is a
  plain call). Add a fixture that runs `mw-log`/`mw-cors` over an awaiting
  handler.
- Compression/multipart (M6/M7) gain true streaming once async I/O lands;
  note the follow-up but do not block Track M on it.

### Phase A4 -- Backpressure, limits, docs

- Cap in-flight fibers per reactor thread; when exceeded, either queue
  accepts or 503. Surface a config knob.
- New guide `docs/guides/httpd-async-guide.md` cross-linking
  `reactor-guide.md` and `async-await-guide.md`.
- Fixtures: an async echo, an async upstream-fanout, and a slow-handler
  throughput test demonstrating concurrency beyond `n_workers`.

### Ordering / dependencies (Track A)

```
A0 (await primitives) --> A1 (async server) --> A2 (await surface)
                                                  --> A3 (mw interop)
                                                  --> A4 (limits + docs)
```

Track A depends only on the shipped reactor + fiber driver; it is
independent of Track M except A3, which validates they compose.

---

## Cross-cutting concerns

- **Memory / leaks.** New per-request allocations (parsed headers, cookie
  jars, multipart parts, response-header list) must be freed at end of
  request; the compiled `emit-c`/`build` path is leak-checked
  (`bash tests/run.sh` with LSan on). Async fibers that intentionally hold
  process-lifetime reactor callbacks may need `requires.no-leak-check` on
  their fixtures (see CLAUDE.md leak policy).
- **Spice vs stdlib boundary.** Codec-dependent middleware (M6 gzip) and
  any external-library handlers belong in `../turmeric-spices`, gated like
  other `requires.spices` fixtures; the pure-Turmeric middleware (M1-M5,
  M7, M8) live in stdlib (`stdlib/httpd.tur` or a new
  `stdlib/httpd-mw.tur`).
- **Testing.** Each phase ships a `tests/fixtures/httpd-*` fixture; codegen
  snapshots (`expected.c`) must be regenerated and committed alongside any
  change to the httpd C core (per CLAUDE.md fixture policy).
- **Docs.** `;;;` docstrings on every new public defn (full docstring for
  exported API), then `just docs` to regenerate `docs/api/`.

## Milestones / suggested PR breakdown

| PR | Scope | Depends on |
|----|-------|-----------|
| 1 | M1 logging + `compose-middleware` (M8 core) | -- |
| 2 | M0 request/response header access | -- |
| 3 | M2 cookies | M0 |
| 4 | M3 body parsers (urlencoded + json) | M0 |
| 5 | M4 cors, M5 basic auth | M0 |
| 6 | M7 multipart | M0, M3 |
| 7 | M6 compression (spice-gated) | M0 |
| 8 | A0+A1 async server + await primitives | reactor (done) |
| 9 | A2+A3 await surface + mw interop | PR 8, Track M |
| 10 | A4 limits, guides, throughput fixtures | PR 9 |

## Open questions (resolved 2026-06-02)

1. **Header storage:** ~~index-into-buffer vs. parse-into-owned-list.~~
   **RESOLVED -- parse-into-owned for both, and lift the 8 KB cap.** The
   read buffer is a stack `char buf[8192]` (`stdlib/httpd.tur:172`): valid
   during the handler call but overwritten across keep-alive requests and
   truncating at 8 KB. M0 copies request headers into a conn-owned list
   (freed at end of request) and makes the read buffer growable. See
   Phase M0.
2. **Compression dep:** ~~pure-Turmeric deflate vs. `tur/zlib` spice.~~
   **RESOLVED -- require the `tur/zlib` spice, skip the middleware when
   absent** (gate fixtures with `requires.spices`); no pure-Turmeric
   fallback. See Phase M6.
3. **Async threading:** ~~single reactor thread vs. pool.~~ **RESOLVED --
   single reactor + one `LocalFiberGroup` first, measure, then shard if
   needed.** Matches the fiber driver's one-group-per-reactor shape. See
   Phase A1.
4. **JSON ownership:** ~~does `json` expose a clean parse/free?~~
   **RESOLVED -- yes** (`json/decode`/`json/free`, `stdlib/json.tur:651`/`694`),
   wrapped per-request: `httpd-req-json` caches the tree on the conn and
   frees it in the iteration cleanup so handlers never free it. See
   Phase M3.
