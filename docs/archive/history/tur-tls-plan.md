# Plan: `tur/tls` Spice + `tur/httpd` H5 Integration

> **Status:** Draft Plan
> **Last Updated:** 2026-05-30
> **Type:** Spice Design + Stdlib Integration Roadmap
> **Related:**
> - `docs/tur-httpd-plan.md` -- httpd H1-H4, H6, H7 shipped; H5 deferred to this plan
> - `stdlib/httpd.tur` -- the HTTP server that will consume `tur/tls`
> - `../turmeric-spices/spices/postgres/build.tur` -- reference for a `cmake-dep` spice manifest
> - `stdlib/reactor.tur` -- event loop the listener shares with TLS handshakes

---

## Overview

`tur/tls` is a thin Turmeric wrapper over a real C TLS library.  It lives
in `../turmeric-spices/spices/tls/` because:

1. TLS pulls a large native dependency (mbedTLS or OpenSSL) into the
   build, which is unacceptable in a default `tur` install.
2. The build system already handles native dependencies cleanly for
   spices via the `:cmake-deps` manifest key (see `tur-postgres`).
3. Two backends (mbedTLS, OpenSSL) can coexist as independent
   `tur-tls-mbedtls` and `tur-tls-openssl` spices exposing the same
   user-facing API.

The H5 milestone of `docs/tur-httpd-plan.md` is delivered by adding TLS
support to `stdlib/httpd.tur` that is **invisibly active when the spice
is present** and a no-op when it is not.  Concretely, `stdlib/httpd.tur`
declares a small set of weakly-linked TLS hooks; the spice provides
their definitions and a constructor `httpd-new-tls` that wires a TLS
context into the existing worker pool.

The goal is to keep `stdlib/httpd.tur` runnable with zero TLS
dependencies (no libssl, no libmbedtls) while letting a user who imports
`tls` get HTTPS with a one-line change.

---

## Non-goals

- **No client-side TLS in v1.**  `tur/tls` is server-only for the H5
  milestone.  A client-side spice can follow once the server API is
  validated.
- **No certificate management.**  Loading PEMs from disk is supported,
  but rotation, ACME, OCSP stapling, and CT logging are out of scope.
- **No protocol selection (HTTP/2, ALPN).**  Plain HTTP/1.1 over TLS only.
  ALPN negotiation can be added later once HTTP/2 is on the roadmap.
- **No SNI multiplexing.**  A single cert per `Httpd` instance.  Hosting
  multiple certs on one port is a v2 feature.
- **No mTLS.**  Client certificate verification is documented as a
  future hook but not implemented in v1.

---

## Architecture

### Backend choice

mbedTLS is the recommended first backend:

- Small binary (~300 KB linked) versus OpenSSL (~3 MB).
- Permissive license (Apache 2.0) compatible with Turmeric.
- Stable C API that maps cleanly to Turmeric `extern-c` declarations.
- Available in Homebrew (`mbedtls`) and most Linux distros (`libmbedtls-dev`).
- No global init state -- a `mbedtls_ssl_context` is self-contained,
  which fits Turmeric's per-thread reactor model.

OpenSSL can be added as a parallel spice (`tls-openssl`) sharing the
same `tls/*` module names.

### Layered design

```
                    +---------------------+
                    |  user code          |
                    |  (import tls)       |
                    +----------+----------+
                               |
       +-----------------------+----------------------+
       |                                              |
       v                                              v
+----------------+                          +----------------+
| stdlib/httpd   |  weak hooks (resolved    |  tls/ctx       |
| (this repo)    |  at link time if the     |  tls/conn      |
|                |  spice is loaded)        |  (the spice)   |
+----------------+                          +----------------+
       |                                              |
       v                                              v
+----------------+                          +----------------+
| recv() / send()|                          | mbedtls_ssl_*  |
| raw POSIX I/O  |                          | TLS state mach |
+----------------+                          +----------------+
```

### Connection model

Today, `HttpdConn` (in `stdlib/httpd.tur`) holds a raw `int fd` and
calls `recv()` / `send()` directly.  After H5:

```c
typedef struct {
    int        fd;
    void      *tls;        // tls_conn_t * or NULL (plaintext)
    int64_t    handler;
    ...
} HttpdConn;
```

If `tls` is non-NULL, `httpd-handle` calls `tur_tls_read`/`tur_tls_write`
instead of `recv`/`send`.  Those two function pointers are resolved as
**weak symbols**: when the spice is linked, the spice's symbols win;
when it is not, the stubs in `stdlib/httpd.tur` return -1 so calling
`httpd-new-tls` without the spice fails cleanly.

This is the same pattern the test suite already uses for
`__tur_autolink__` directives -- the build system harvests link flags
from emitted C, and the spice's `:cmake-deps` block contributes
`-lmbedtls -lmbedx509 -lmbedcrypto` automatically.

### Listener and handshake

The H2 reactor accept callback is unchanged: when a TCP connection
arrives, it is accepted, added to the worker queue, and a worker
eventually pops it.  Only after a worker pops does the handshake start:

1. Worker pops `cfd` from the queue.
2. If the `HttpdBlock` was created via `httpd-new-tls`, the worker:
   a. Calls `tur_tls_wrap_fd(ctx, cfd)` to get a `tls_conn_t *`.
   b. Drives `tur_tls_handshake(conn)` to completion (blocking, with
      the same 5s `SO_RCVTIMEO` already in place).
   c. On failure: logs, closes fd, returns to queue.
3. Otherwise plaintext path runs unchanged.

Driving the handshake inside the worker (not the listener) keeps the
H2 accept loop fast and matches how nginx, Caddy, etc. split listener
and handshake responsibilities.

### Keep-alive over TLS

H4 keep-alive composes naturally: the worker loop calls
`tur_tls_read`/`tur_tls_write` for every iteration on the same
`tls_conn_t *`.  TLS records are framed transparently by mbedTLS; the
HTTP/1.1 parser sees a byte stream and does not need to know.

### Shutdown

`httpd-stop` closes the listen fd and stops the listener reactor as
before.  In-flight TLS connections are interrupted by the 5s
`SO_RCVTIMEO`; the worker's TLS shutdown sends a `close_notify` alert
on its way out so well-behaved clients see a clean shutdown rather
than a TCP reset.

---

## `tur/tls` Spice Layout

```
../turmeric-spices/spices/tls/
  build.tur                 -- spice manifest (see below)
  README.md                 -- user-facing usage + cert-generation snippets
  src/
    tls/ctx.tur             -- SSL_CTX-equivalent: certs, keys, ciphers
    tls/conn.tur            -- per-connection SSL_state-equivalent
    tls/autolink.tur        -- one-line autolink hint (-lmbedtls ...)
  tests/
    fixtures/
      tls-roundtrip/        -- self-signed cert; echo server smoke test
      tls-handshake-fail/   -- mismatched cipher / bad cert; expect error
```

### Manifest (`build.tur`)

```turmeric
(defpackage tur-tls
  :name        "tur-tls"
  :version     "0.1.0"
  :description "TLS termination for tur/httpd via mbedTLS"
  :license     "MIT"
  :cmake-deps #{
    "mbedtls" #{:cmake-name "MbedTLS"
                :targets    ["MbedTLS::mbedtls"
                             "MbedTLS::mbedx509"
                             "MbedTLS::mbedcrypto"]}
  }
  :exports #{
    "tls/ctx"  ["tls-ctx-new" "tls-ctx-free"
                 "tls-ctx-load-cert-pem" "tls-ctx-load-key-pem"
                 "tls-ctx-set-ciphers"]
    "tls/conn" ["tls-wrap-fd" "tls-handshake" "tls-read" "tls-write"
                 "tls-shutdown" "tls-free"]
  })
```

### Core API

```turmeric
;; -- tls/ctx -----------------------------------------------------------------

(defn tls-ctx-new [] : ptr<void>)
;;   New mbedTLS server context with TLS 1.2+ defaults.

(defn tls-ctx-free [ctx : ptr<void>] : nil)

(defn tls-ctx-load-cert-pem [ctx : ptr<void> path : cstr] : int)
;;   Load a PEM cert chain.  Returns 0 on success.

(defn tls-ctx-load-key-pem [ctx : ptr<void> path : cstr] : int)
;;   Load a PEM private key.  Returns 0 on success.

(defn tls-ctx-set-ciphers [ctx : ptr<void> spec : cstr] : int)
;;   Restrict ciphersuites by ASCII spec; "" keeps mbedTLS defaults.

;; -- tls/conn ----------------------------------------------------------------

(defn tls-wrap-fd [ctx : ptr<void> fd : int] : ptr<void>)
;;   Allocate a tls_conn_t bound to fd.  Does NOT block.

(defn tls-handshake [conn : ptr<void>] : int)
;;   Drive the handshake to completion.  Returns 0 on success.

(defn tls-read  [conn : ptr<void> buf : ptr<void> len : int] : int)
(defn tls-write [conn : ptr<void> buf : ptr<void> len : int] : int)
;;   Same semantics as recv/send: bytes transferred or -1 on error.

(defn tls-shutdown [conn : ptr<void>] : nil)
;;   Send close_notify (best-effort) before closing the fd.

(defn tls-free [conn : ptr<void>] : nil)
```

### Self-signed cert helper (test-only)

The spice ships a tiny `tools/gen-cert.sh` that produces a
self-signed cert + key in `/tmp` so fixtures can exercise the
handshake without depending on a real PKI.  This is the same trick
the postgres spice uses for its server-fixture tests.

---

## `stdlib/httpd.tur` Changes for H5

Additions, all backwards compatible:

| Field / Function | Purpose |
|---|---|
| `void *tls` in `HttpdConn` | Per-connection TLS state, NULL when plaintext |
| `void *tls_ctx` in `HttpdBlock` | Shared TLS context for the listener |
| `httpd-new-tls [port wkrs h ctx]` | Constructor that stores `ctx` and routes accepted fds through `tls-wrap-fd` + `tls-handshake` |
| `httpd-read`  | Internal: `recv()` if `conn->tls == NULL`, else `tls-read` |
| `httpd-write` | Internal: `send()` if `conn->tls == NULL`, else `tls-write` |
| Updated `httpd-handle` | Replaces direct `recv`/`send` with `httpd-read`/`httpd-write`; calls `tls-shutdown` + `tls-free` before `close(fd)` |

Weak symbol declarations live in `stdlib/httpd.tur`:

```c
__attribute__((weak)) void *tur_tls_wrap_fd(void *, int)       { return NULL; }
__attribute__((weak)) int   tur_tls_handshake(void *)          { return -1; }
__attribute__((weak)) int   tur_tls_read (void *, void *, int) { return -1; }
__attribute__((weak)) int   tur_tls_write(void *, void *, int) { return -1; }
__attribute__((weak)) void  tur_tls_shutdown(void *)           { }
__attribute__((weak)) void  tur_tls_free(void *)               { }
```

When the spice is loaded its strong definitions take over; otherwise
`httpd-new-tls` returns NULL with a clear error message so the user
knows the spice is missing.

`httpd-new` (plaintext) is unchanged.

---

## Phases

| Step | Task | Status |
|---|---|---|
| T1 | Scaffold `../turmeric-spices/spices/tls/` with `build.tur`, README, autolink hint; verify `tur fetch` resolves mbedTLS via `:cmake-deps` | Done |
| T2 | `tls/ctx`: `tls-ctx-new`, `tls-ctx-free`, `tls-ctx-load-cert-pem`, `tls-ctx-load-key-pem`; smoke test that constructs a context and frees it | Done -- `tests/fixtures/ctx-lifecycle/` exercises ctx-new + load-cert + load-key + ctx-free. Required tooling fixes that landed alongside: (a) `pkg.c::pkg_gen_cmake_deps` now emits per-target link_dirs via `$<TARGET_FILE_DIR:...>` and a list of link_libs from `:targets`; (b) `__tur_include__` hoisting works in multi-file builds; (c) `find_project_root` callers now `realpath()` the start so spice-deps-manifest.json is found when invoked from a relative path like `.` or `main.tur`. |
| T3 | `tls/conn`: `tls-wrap-fd`, `tls-handshake`, `tls-read`, `tls-write`, `tls-shutdown`, `tls-free`; tls-roundtrip fixture connects to a server thread and exchanges one record | Done -- `tests/fixtures/tls-roundtrip/` drives a full handshake + echo over a `socketpair`: the server thread (pthread) uses the spice's public Turmeric API (`tls-ctx-*` + `tls-conn-*`); the client side uses mbedTLS client mode in inline-C with `MBEDTLS_SSL_VERIFY_NONE` against the same self-signed cert from T2's `tools/gen-cert.sh`. `tls-free` deliberately does **not** close the fd -- caller owns the fd lifecycle (H5 wants this). |
| T4 | Add weak TLS hooks + `tls` / `tls_ctx` fields to `stdlib/httpd.tur`; verify all existing httpd fixtures still pass (no behavioural change when `tls` is NULL) | Done -- spice symbols are `static` (TU-local), so the design uses a file-scope function-pointer table `httpd_tls_ops` populated via `httpd-register-tls-impl` rather than ELF weak symbols. `HttpdBlock` gained `void *tls_ctx`; `HttpdConn` (inside `httpd-handle` only) gained `int64_t tls`. `httpd-handle` takes a 3rd `tls` argument; `httpd-worker` performs the wrap-fd + handshake when `hb->tls_ctx` is set. All 1129 fixtures still pass. |
| T5 | Implement `httpd-new-tls` and route accepted fds through the TLS wrappers; add `httpd-h5-tls` fixture (analogous to httpd-h1-basic but over TLS) | Done -- `httpd-new-tls [port workers handler ctx]` lives in `stdlib/httpd.tur` (validates ctx + the registered op pointers, then reuses `httpd-new-pool` and stashes the ctx in `HttpdBlock.tls_ctx`). The spice ships `tls/httpd` exporting `tls-httpd-init`, which captures pointers to the spice's six TLS-conn statics and forwards them to `httpd-register-tls-impl`. The `tests/fixtures/httpd-h5-tls` fixture stays inside the main turmeric tree (no spice dep, no cert generation) by registering plaintext-passthrough stubs and asserting that every hook in the H5 routing path fires in the expected order; the full mbedTLS-over-TCP round-trip is already covered by the spice's `tls-roundtrip` fixture (T3). |
| T6 | Document client cert generation in `spices/tls/README.md` and link from `docs/guides/httpd-tls-guide.md` | Done -- spice README rewritten to cover the four shipped modules (autolink/ctx/conn/httpd) with a cert-generation section that wraps `tools/gen-cert.sh` and a plaintext OpenSSL recipe; `docs/guides/httpd-tls-guide.md` written end-to-end (migration from `httpd-new` to `httpd-new-tls`, what happens under the hood, local verification with curl + `openssl s_client`, pointers to both test fixtures). |

T1-T3 ship the spice independently; T4-T6 ship the httpd integration.
A user only needs T1-T3 to use raw TLS for other protocols (e.g. SMTP);
the httpd part is additive.

---

## Build-system interaction

The spice's `:cmake-deps` block triggers `tur fetch` to look for an
mbedTLS install via CMake's `find_package(MbedTLS)`.  Homebrew
(`brew install mbedtls`) and Debian (`apt install libmbedtls-dev`) both
provide the standard CMake config files, so no per-platform branching
is required in the spice itself.

`stdlib/httpd.tur` ships no new link flags.  The
`-lmbedtls -lmbedx509 -lmbedcrypto` link line comes from the spice's
autolink hint and is only emitted into a binary that actually imports
the spice.

---

## Testing strategy

| Fixture | What it covers |
|---|---|
| `tls-roundtrip` (in spice) | Wrap two ends of a socketpair, handshake, exchange one record, shutdown.  No HTTP. |
| `tls-handshake-fail` (in spice) | Mismatched cert/key, expect `tls-handshake` to return non-zero |
| `httpd-h5-tls` (in this repo) | One client thread does an HTTPS GET against a server using the test cert, asserts response body matches |
| `httpd-h5-tls-keepalive` (this repo, optional) | Two HTTP/1.1 requests on the same TLS connection -- proves keep-alive composes with TLS |

The httpd fixtures gain a new `requires.spices` marker (already
honoured by `tests/run.sh`) so CI skips them when the sibling
`turmeric-spices` checkout is absent, preserving the zero-dependency
default build.

---

## Risks and open questions

- **mbedTLS API churn.**  mbedTLS 3.x reorganised its headers from
  2.x.  Pin to >= 3.0 in the spice manifest and document that 2.x is
  unsupported.
- **Non-blocking handshakes.**  The plan drives the handshake
  synchronously inside the worker, which can starve other workers
  during a slow client.  If this becomes a problem, an H5.1 follow-up
  can move the handshake into the listener reactor using
  `MBEDTLS_ERR_SSL_WANT_READ`/`WANT_WRITE`.
- **Cert reload.**  Reloading certs at runtime requires either swapping
  the `tls_ctx` (and accepting that in-flight handshakes use the old
  one) or holding a reader-writer lock around the context.  Documented
  as a v2 feature.
- **SIGPIPE under TLS.**  Already masked process-wide by `httpd-new`;
  no extra action needed.

---

## Migration guide for users

Before (plaintext):

```turmeric
(let [h (httpd-new 8080 my-handler)]
  (httpd-run h))
```

After (HTTPS):

```turmeric
(import tls/ctx :refer [tls-ctx-new tls-ctx-load-cert-pem tls-ctx-load-key-pem])

(let [ctx (tls-ctx-new)]
  (tls-ctx-load-cert-pem ctx "/etc/letsencrypt/live/example.com/fullchain.pem")
  (tls-ctx-load-key-pem  ctx "/etc/letsencrypt/live/example.com/privkey.pem")
  (let [h (httpd-new-tls 8443 4 my-handler ctx)]
    (httpd-run h)))
```

The handler, router, and middleware code from H4/H6/H7 do not change.
