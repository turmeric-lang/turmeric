# Plan: tourist on stdlib httpd, and the Express-shaped middleware set

> Status: proposed (2026-10-04). Nothing landed.
> Scope: turmeric `stdlib/httpd.tur` (connection upgrade, new middleware);
> `spices/httpd/` (becomes a typed layer over stdlib httpd);
> `spices/tourist/` and `spices/tourist-session/` (move onto it).
> Not on tur-signal's call surface, so it does not gate v1.
> Measured against turmeric-spices `origin/main` 9d25587 and turmeric
> `origin/main` 7690314c1.

## Motivation

A user arriving from express.js expects a framework on top of a plain HTTP
server, plus a shelf of middleware: CORS, logging, error recovery, body
parsing and limits, rate limiting, auth, compression, security headers,
request IDs, proxy awareness.

Turmeric has most of that shelf, but on the **wrong server**. There are two
independent HTTP server implementations:

| Stack | Where | Middleware today |
|---|---|---|
| `stdlib/httpd.tur` | turmeric repo | `mw-cors`, `mw-log`, `mw-recover`, `mw-static` (ETag/304), `mw-json-body`, `mw-body-size`, `mw-rate-limit`, `mw-basic-auth`, gzip via `stdlib/httpd-compress.tur`; cookie get/set; form + multipart parsing; `httpd-req-remote-ip` |
| `spices/httpd` + `spices/tourist` | this repo | `use!` / `use-after!` hooks; sessions + CSRF (`tourist-session`); static files; templates; JSON codecs (`httpd/handler`) |

Tourist is built on `spices/httpd` (`tourist/app.tur:31`), not on stdlib
httpd, so none of the stdlib middleware reaches a tourist app. Apart from
sessions/CSRF, a tourist app today has no CORS, logging, recovery, rate
limiting, auth, compression, form parsing, or cookie helpers outside the
session spice.

## Decisions (2026-10-04)

1. **stdlib httpd stays a server users build on.** It is Node's `http`
   plus connect-style middleware: a supported base, not an internal detail.
2. **Tourist moves onto stdlib httpd**, as express sits on `http`. One
   server underneath means one place for server fixes, and middleware
   written for stdlib httpd works under a tourist app, which is the
   layering a Node user already expects.

## Why moving is sensible

stdlib httpd is the more complete server on almost every axis:

| | stdlib httpd | spices/httpd |
|---|---|---|
| Keep-alive | yes (`httpd.tur:7`) | no; always `Connection: close` (`httpd/write.tur:28`) |
| Read timeout | 5 s `SO_RCVTIMEO` (`httpd.tur:677`) | none; only `SO_REUSEADDR` is set, so 8 silent clients wedge the default pool |
| Body size cap | `httpd-set-max-body!` | none; `srv-recv-request` reallocs to any `Content-Length` (`httpd/server.tur:168-239`) |
| Peer address | `httpd-req-remote-ip` | none |
| Binary bodies | `httpd-resp-body-bytes!` | `cstr` only (`httpd/response.tur:123`) |
| Fiber-per-request mode | `httpd-new-async` | no |
| TLS | hook table (`httpd-register-tls-impl`) | `httpd/tls` |
| Cookies, forms, multipart | yes | no (cookies live in `tourist-session`) |
| Panic recovery | `mw-recover` via `catch-unwind` | none |
| Middleware set | see above | none |
| **Connection upgrade (WebSocket)** | **no** | yes: `Conn`, `conn-mark-upgraded!`, `server-start-conn` |
| **Typed value API** | no; handlers take `ptr<void>` and mutate it | `Request`/`Response` `defopaque`s, `Handler` typeclass, JSON codecs |

So the move is worth it, and it has two prerequisites, the last two rows:
stdlib needs a connection-upgrade handoff (tourist-ws and ws-server depend
on it), and the typed API has to survive. The way to keep the typed API is
to make `spices/httpd` the typed layer *over* stdlib httpd, the role that
Node's `req`/`res` objects play over the socket. That layer keeps its
public surface, so the four spices that depend on it (`tourist`,
`tourist-session`, `tourist-ws`, `ws-server`) do not change.

Moving also **removes** the spice server's security holes (no body cap, no
read timeout) instead of fixing them a second time.

## Non-goals

- HTTP/2, streaming request/response bodies, chunked uploads.
- Cancelling a running handler. Threads are not cancellable; a deadline
  can answer the client early but cannot stop the work.
- `method-override`, view engines beyond the existing template spice.
- Tightening stdlib httpd's own `ptr<void>` conn typing. The typed layer
  wraps it in a `defopaque`; retyping stdlib's API is a separate change.

## Design rules for every new API

- **No `:int` stand-ins** (turmeric CLAUDE.md). Middleware factories return
  `Item`; callbacks are spelled-out fn types; flags are `bool`; "maybe"
  values are `Option`/`Result`. Per-request values that are not integers
  (request ID, client IP) get typed accessors, not `ctx-attr-set!` (whose
  value slot is `int`).
- **Configuration by `defstruct` options plus a `default-*-opts`**, the
  shape stdlib's `CorsOpts` already uses.
- **Write server-level middleware once, in stdlib.** Tourist reuses it
  rather than porting it. Tourist-only versions exist only where routing
  or `Ctx` matters.
- **Safe defaults.** Nothing trusts proxy headers, enables credentials, or
  emits a CSP unless asked.
- **No process globals for middleware config**: two differently
  configured instances in one process must work.

## Phases

### H0 -- Stopgap on spices/httpd (optional, small)

H1-H2 are the real fix but span two repos. If they will not land soon,
close the two denial-of-service holes in place first: a body cap
(reject over 1 MiB with `413` before allocating) and `SO_RCVTIMEO` on
accepted fds in `srv-recv-request` and the TLS path. About 20 lines;
deleted by H2.

**Acceptance:** an oversized `Content-Length` gets `413` without a large
allocation; nine stalled clients do not block a tenth request.

### H1 -- Connection upgrade in stdlib httpd (turmeric repo)

**Tasks**
- A handler can claim the connection: `httpd-conn-upgrade!` marks it, the
  worker writes the response (the `101`) and then, instead of looping on
  keep-alive or closing, hands the fd and its TLS state to an
  `on-upgrade` callback that owns them from then on.
- Accessors the typed layer needs: `httpd-conn-fd`, `httpd-conn-tls`.
- Pool mode first. In async (fiber) mode, refuse an upgrade with a clear
  error until it is designed; do not silently drop the connection.
- Make the read timeout configurable (`httpd-set-read-timeout!`; default
  stays 5 s), and confirm the header block is size-capped (add a cap and
  `431` if not; not yet checked).

**Acceptance**
- A stdlib fixture upgrades a connection, exchanges bytes on the raw fd
  after the `101`, and the worker neither closes nor reuses that fd.
- Same over TLS.

### H2 -- spices/httpd becomes a typed layer over stdlib httpd

**Tasks**
- Keep the public modules and names: `httpd/types`, `httpd/request`,
  `httpd/response`, `httpd/server` (`server-start*`, `server-stop`),
  `httpd/handler` (`serve`, JSON codecs), `httpd/conn`, `httpd/tls`,
  `httpd/typed`.
- `Request` wraps the stdlib conn as a borrowed view: `req-method`,
  `req-path`, `req-header` and so on forward to `httpd-req-*`. Add
  `req-remote-addr`.
- `Response` stays a value. When the handler returns, the layer writes
  it to the conn with `httpd-resp-status!` / `httpd-resp-header-add!` /
  `httpd-resp-body-bytes!`. Add `with-body-bytes`, `with-header-set`
  (case-insensitive replace) and `resp-header [resp name] : (Option cstr)`
  next to the append-only `with-header` (`httpd/response.tur:75`).
- `server-start*` builds a stdlib handler closure around the user fn and
  calls `httpd-new-pool`; the conn variants use H1's upgrade; `httpd/tls`
  registers the tls spice with `httpd-register-tls-impl`.
- Add `server-start-opts` taking a `ServerOpts` struct (pool size, body
  cap, read timeout), forwarded to the stdlib setters.
- Delete the spice's socket, accept, pool and recv code. `httpd/parse` and
  `httpd/write` have no callers outside `spices/httpd` (checked); keep
  them only if its own tests still need them.
- Declare the minimum turmeric version that has H1 in `build.tur`.

**Acceptance**
- The `httpd`, `tourist`, `tourist-session`, `tourist-ws` and `ws-server`
  suites are green with no source changes outside `spices/httpd`.
- New tests: `413` on an oversized body, `408` (or close) on a stalled
  client, a tenth request served while nine clients stall, two requests
  on one keep-alive connection, a NUL-containing body round-trips,
  `req-remote-addr` is `127.0.0.1` on loopback.

### H3 -- Tourist calling convention

**Tasks**
- Settle whether `use!` closures may capture. The `use!` docstring says
  yes, and dispatch uses `TUR_APPLY1` (fat closures, `tourist/dsl.tur:158`);
  `tourist-session/README.md` ("Writing a custom store") says middleware
  is a bare fn pointer, which is why `session-mw` keeps its config in a
  global. A fixture with capturing `use!` / `use-after!` closures under
  concurrent requests decides it. Fix whichever side is wrong.
- Add **`use-around!`**: `(fn [Ctx (fn [Ctx] Response)] Response)`, the
  Express/Koa `next` shape, for Ctx-aware middleware that brackets the
  route.
- Check that `ctx-add-header!` headers land on a short-circuit response
  from a `use!` item, not only on route responses.

**Acceptance**
- Capture fixture green and the README matches it; nested `use-around!`
  items run outside-in.

### H4 -- Server-level middleware: write once in stdlib, use under tourist

**Tourist side:** `tourist-opts` gains a `:wrap` field, a stdlib middleware
stack (`compose-middleware`) applied around tourist's whole dispatch, the
way `app.use(cors())` applies connect middleware in Express. That makes the
existing stdlib set available to tourist apps with no port:

| Express | Used from tourist as |
|---|---|
| `cors` | `mw-cors-opts` |
| `morgan` | `mw-log` |
| error handler | `mw-recover` (real panic recovery via `catch-unwind`) |
| `express-rate-limit` | `mw-rate-limit` |
| basic auth | `mw-basic-auth` |
| body `limit` | `mw-body-size` |
| `compression` | `stdlib/httpd-compress.tur` |

**stdlib side** (turmeric repo), improvements and additions to that set:

- `CorsOpts`: a list of allowed origins with exact-match echo and
  `Vary: Origin`, and `allow-credentials` as `bool`; reject `"*"` with
  credentials when the options are built.
- `mw-log`: a sink `(fn [cstr] unit)` (default stdout) and the request ID
  in each line.
- **New `mw-secure-headers` + `SecureHeadersOpts`** (helmet). Defaults:
  `X-Content-Type-Options: nosniff`, `X-Frame-Options: DENY`,
  `Referrer-Policy: no-referrer`, `Cross-Origin-Opener-Policy: same-origin`.
  Opt-in only: HSTS (on by accident it locks a domain to https) and CSP as
  a caller-supplied string (a wrong default breaks apps silently).
- **New `mw-request-id` + `httpd-req-id`.** Accept an incoming
  `X-Request-Id` only if it is 1-128 chars of `[A-Za-z0-9._-]`; otherwise
  generate 128 random bits as hex. Echo it as a response header.
- **New trust-proxy support.** `TrustProxyOpts` holds trusted CIDRs
  (default: none). `httpd-req-ip` walks `X-Forwarded-For` right to left and
  returns the first hop not in a trusted range, falling back to the peer
  address; `httpd-req-proto` honors `X-Forwarded-Proto` only from a trusted
  peer. `mw-rate-limit` and `mw-log` switch to `httpd-req-ip`.
- **New `mw-etag`** for dynamic responses: weak ETag from a hash of a `200`
  `GET`/`HEAD` body; matching `If-None-Match` gives `304`. (`mw-static`
  already handles static files.)
- **Request deadline** (lowest priority): `mw-timeout` answers `503` when
  a handler overruns. The handler keeps running, and the doc must say so.

**Acceptance**
- A tourist fixture runs a route under `:wrap` with recover, CORS, log and
  rate limit, and sees a panic become `500`, a preflight answered, a log
  line, and a `429`.
- stdlib fixtures for each new middleware, including rejection paths:
  malicious request ID (CRLF, 1 KiB) replaced, `X-Forwarded-For` ignored
  without trust config and honored with one, `304` on a matching ETag.

### H5 -- Tourist conveniences on `Ctx`

Thin, typed forwards to stdlib, so routes need not reach the conn:

- Cookies: `req-cookie [ctx name] : (Option cstr)`,
  `set-cookie! [ctx CookieOpts]`. `tourist-session` moves onto these and
  drops `session/cookie.tur`'s own parser.
- Forms: `form-param [ctx key] : (Result cstr cstr)`.
- Multipart: `req-file [ctx field] : (Option Part)` with typed `Part`
  accessors over `httpd-req-file` / `httpd-part-*`.
- `req-ip [ctx]`, `req-proto [ctx]`, `req-id [ctx]` over H4's stdlib
  accessors.
- Per-route middleware, where routing matters (auth on one sub-app, a
  tighter body limit on an upload route): `Ctx`-level `basic-auth` and
  `body-limit` as `use!` items, sharing stdlib's verifier and
  constant-time compare.
- Static files: switch `tourist/static.tur` to `mw-static`'s file serving
  so tourist gains ETag/304 (it emits neither today).

**Acceptance**
- One fixture per helper, and the `tourist-session` suite stays green
  after the cookie move.
- The tourist README gains a "Middleware" section mapping each Express
  package to its tourist or stdlib name.

## Ordering

```
H0 (stopgap, optional)
H1 (stdlib upgrade) --> H2 (typed layer) --> H5 (Ctx conveniences)
H3 (convention) ------------------------/
H4 stdlib additions: start any time; tourist :wrap needs H2
```

H1 and the stdlib half of H4 are turmeric PRs; everything else is in this
repo. H2 is the pivot: once it lands, tourist is on stdlib httpd and the
spice server code is gone.

## Risks and open questions

1. **Behavior change from keep-alive.** The spice server always closed the
   connection; stdlib keeps it open. A test or client that reads to EOF
   to find the end of a response will hang. Sweep the five dependent
   suites for that pattern in H2.
2. **Upgrade in async mode** is deferred (H1). An app that wants
   WebSockets runs the pool server until it is designed.
3. **Version coupling.** `spices/httpd` now needs a turmeric with H1;
   spices CI re-pins turmeric `main` on every run, so a spices PR can go
   red because of a turmeric change. Declare the minimum version.
4. **Default pool size** drops from 8 (spice) to 4 (stdlib `httpd-new`).
   `server-start` should keep passing 8 to preserve behavior.
5. **`stdlib/httpd-compress.tur` loads zlib by a relative path**
   (`../turmeric-spices/spices/zlib/...`), which resolves only in a
   side-by-side checkout. Tourist users get compression through `:wrap`,
   so that load needs to work from an installed toolchain. Fix in H4.
6. **Rate-limit state is per process.** Multi-process deployments need a
   store (Valkey, as sessions do); leave a seam, ship memory only.
7. **Header duplicates.** Several middleware set headers a route may
   already have set (secure headers, `Content-Encoding`, ETag), and
   `Vary` is added by more than one. Stdlib's `httpd-resp-header!`
   replaces and `httpd-resp-header-add!` appends; each middleware must use
   the right one, and `Vary` should merge tokens rather than repeat.
