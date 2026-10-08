---
title: HTTPS with stdlib/httpd + tur-tls
category: Networking and Web
description: How to terminate TLS on an httpd-new server using the tur-tls spice
---

# HTTPS with `stdlib/httpd` and `tur-tls`

`stdlib/httpd` ships in the Turmeric tree and is plaintext by default.
TLS termination is an opt-in capability provided by the
[`tur-tls`](https://github.com/turmeric-lang/turmeric-spices/tree/main/spices/tls)
spice. This guide shows how to wire the two together so a one-line
change converts an `httpd-new` server into an `httpd-new-tls` server.

The integration was designed under the constraints in
[`docs/archive/history/tur-tls-plan.md`](https://github.com/turmeric-lang/turmeric/blob/main/docs/archive/history/tur-tls-plan.md):

- Zero TLS dependency in a default `tur` install. Programs that do not
  import the spice still link cleanly.
- One indirection in the I/O hot path: a small file-scope function-pointer
  table that the spice populates at startup.
- The handler, router, and middleware code from H4/H6/H7 do not change.

---

## What the spice provides

The spice exports four Turmeric modules:

| Module          | Purpose                                                       |
|-----------------|---------------------------------------------------------------|
| `tls/ctx`       | Server-side `SSL_CTX`: load PEM cert + key, configure ciphers |
| `tls/conn`      | Per-connection state: wrap fd, handshake, encrypted read/write|
| `tls/autolink`  | One-line `__tur_autolink__` hint that pulls in mbedTLS libs   |
| `tls/httpd`     | Bridge module -- registers the TLS ops with stdlib/httpd      |

For protocols other than HTTP, only `tls/ctx` and `tls/conn` are needed.
The `tls/httpd` module exists solely to flip stdlib/httpd from
plaintext-only to TLS-capable.

---

## Generating a server certificate

For local development and CI, the spice ships
[`tools/gen-cert.sh`](https://github.com/turmeric-lang/turmeric-spices/blob/main/spices/tls/tools/gen-cert.sh).
It writes a self-signed `test-cert.pem` + `test-key.pem` into a target
directory and is suitable only for throwaway test runs:

```sh
../turmeric-spices/spices/tls/tools/gen-cert.sh /tmp/tls-smoke-test
```

The script wraps a single OpenSSL invocation:

```sh
openssl req -x509 -newkey rsa:2048 -nodes \
    -keyout "$KEY" -out "$CERT" \
    -days 1 -subj "/CN=localhost"
```

If you have OpenSSL on your `PATH`, you can run that line directly --
nothing about the cert is spice-specific.

### Production certificates

For real deployments, use whatever PEM cert + key your CA issues. Let's
Encrypt's `certbot` is the common path:

```sh
sudo certbot certonly --standalone -d example.com
```

This drops `fullchain.pem` and `privkey.pem` under
`/etc/letsencrypt/live/example.com/`. Both files are PEM-encoded and
load straight into the spice via `tls-ctx-load-cert-pem` and
`tls-ctx-load-key-pem`.

The v0.1.0 line does **not** implement:

- Certificate rotation at runtime (you must restart to pick up renewed
  certs).
- ACME / OCSP stapling / CT logging.
- Encrypted PEM private keys (omit `-aes256` from your `openssl` invocation
  or use `-nodes`).
- SNI multiplexing (one cert per `Httpd` instance).
- mTLS / client certificate verification.

These are explicitly punted to follow-ups in the
[plan's non-goals section](https://github.com/turmeric-lang/turmeric/blob/main/docs/archive/history/tur-tls-plan.md#non-goals).

---

## Migrating a server from `httpd-new` to `httpd-new-tls`

The plaintext version:

```turmeric
(load "stdlib/httpd.tur")

(defn my-handler [conn : ptr<void>] : nil
  (httpd-resp-status! conn 200)
  (httpd-resp-body!   conn "Hello over HTTP"))

(let [h (httpd-new 8080 my-handler)]
  (httpd-run h)
  (httpd-free h))
```
```sweet-exp
load("stdlib/httpd.tur")
defn my-handler [conn :ptr<void>] :nil
  httpd-resp-status!(conn 200)
  httpd-resp-body!(conn "Hello over HTTP")
let [h httpd-new(8080 my-handler)]
  httpd-run(h)
  httpd-free(h)
```

The HTTPS version differs only in the constructor + a tiny preamble:

```turmeric
(load "stdlib/httpd.tur")
(import tls/ctx   :refer [tls-ctx-new tls-ctx-free
                           tls-ctx-load-cert-pem tls-ctx-load-key-pem])
(import tls/httpd :refer [tls-httpd-init])

(defn my-handler [conn : ptr<void>] : nil
  (httpd-resp-status! conn 200)
  (httpd-resp-body!   conn "Hello over HTTPS"))

(defn main [] : int
  (tls-httpd-init)                              ;; (a)
  (let [ctx (tls-ctx-new)]                      ;; (b)
    (tls-ctx-load-cert-pem ctx "/etc/letsencrypt/live/example.com/fullchain.pem")
    (tls-ctx-load-key-pem  ctx "/etc/letsencrypt/live/example.com/privkey.pem")
    (let [h (httpd-new-tls 8443 4 my-handler ctx)] ;; (c)
      (httpd-run h)
      (httpd-free h)
      (tls-ctx-free ctx)
      0)))
```
```sweet-exp
load("stdlib/httpd.tur")
import tls/ctx :refer [tls-ctx-new tls-ctx-free
                       tls-ctx-load-cert-pem tls-ctx-load-key-pem]
import tls/httpd :refer [tls-httpd-init]
defn my-handler [conn :ptr<void>] :nil
  httpd-resp-status!(conn 200)
  httpd-resp-body!(conn "Hello over HTTPS")
defn main [] :int
  tls-httpd-init()
  ;; (a)
  let [ctx tls-ctx-new()]
    ;; (b)
    tls-ctx-load-cert-pem(ctx "/etc/letsencrypt/live/example.com/fullchain.pem")
    tls-ctx-load-key-pem(ctx "/etc/letsencrypt/live/example.com/privkey.pem")
    let [h httpd-new-tls(8443 4 my-handler ctx)]
      ;; (c)
      httpd-run(h)
      httpd-free(h)
      tls-ctx-free(ctx)
      0
```

The three new lines:

- **(a)** `tls-httpd-init` captures pointers to the spice's six per-conn
  TLS functions and forwards them to `httpd-register-tls-impl`. Call it
  once before any `httpd-new-tls`. Idempotent.
- **(b)** Build a TLS context, load cert + key. Bytes-for-bytes the
  shape sketched in the [Quick start](https://github.com/turmeric-lang/turmeric-spices/blob/main/spices/tls/README.md#quick-start-preview)
  section of the spice README.
- **(c)** `httpd-new-tls` is the TLS-aware constructor. Compared to
  `httpd-new-pool` it adds one trailing arg (the ctx) and validates
  both the ctx and the registered op table before returning -- so if
  `tls-httpd-init` was forgotten, you get a clear error message instead
  of a mystifying handshake failure.

Routing (H6), middleware (H7), keep-alive (H4) and request parsing all
compose with TLS unchanged. The TLS record framing is transparent to
the HTTP/1.1 parser; the handler sees a byte stream.

---

## What happens under the hood

After `tls-httpd-init`:

1. A pthread accepts a TCP connection on the listen fd (same as
   plaintext httpd).
2. A worker pops the fd and calls the registered `tls-wrap-fd`, which
   binds the fd to an mbedTLS server SSL context. The wrap is
   non-blocking.
3. The worker drives `tls-handshake` to completion. On failure the
   worker logs, calls `tls-free`, closes the fd and pops the next
   connection.
4. `httpd-handle` runs its keep-alive loop. Every `recv` / `send` is
   replaced with the registered `tls-read` / `tls-write`. The HTTP/1.1
   parser is unaware that the byte stream is encrypted.
5. When the loop exits, the worker calls `tls-shutdown` (sending
   `close_notify` to the peer) followed by `tls-free`, then closes the
   fd. Well-behaved clients see a clean TLS shutdown rather than a TCP
   reset.

If you skip `tls-httpd-init`, `httpd-new-tls` returns NULL and prints
the missing-init hint to stderr.

---

## Verifying a server locally

With the test cert in `/tmp/tls-smoke-test/` and an HTTPS server bound
to 8443, both of these should print `Hello over HTTPS`:

```sh
curl -k https://localhost:8443/
openssl s_client -connect localhost:8443 -quiet <<< $'GET / HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n'
```

`-k` (curl) and the absence of `-verify_return_error` (s_client) are
needed because the self-signed cert isn't in any trust store. With a
real CA-issued cert these flags drop away.

---

## Test fixtures

Two fixtures cover the integration:

- [`tur-tls/tests/fixtures/tls-roundtrip`](https://github.com/turmeric-lang/turmeric-spices/blob/main/spices/tls/tests/fixtures/tls-roundtrip/main.tur)
  -- spice-side mbedTLS round-trip over a socketpair, no HTTP. Asserts
  that `tls-wrap-fd` + `tls-handshake` + `tls-read`/`tls-write` produce
  a clean TLS exchange.
- [`tests/fixtures/httpd-h5-tls`](https://github.com/turmeric-lang/turmeric/tree/main/tests/fixtures/httpd-h5-tls)
  -- in-tree integration test for the H5 routing path. Uses plaintext
  passthrough stubs (no real mbedTLS) to verify that `httpd-new-tls`
  refuses to build without ops, then that `wrap_fd` / `handshake` /
  `read` / `write` / `shutdown` / `free` all fire in the expected order
  on a real accepted connection.

---

## See also

- [tur-tls plan](https://github.com/turmeric-lang/turmeric/blob/main/docs/archive/history/tur-tls-plan.md) -- design rationale + roadmap.
- [tur-httpd plan](https://github.com/turmeric-lang/turmeric/blob/main/docs/archive/history/tur-httpd-plan.md) -- the H1-H7 milestones that
  this guide composes with.
- [tur-tls spice README](https://github.com/turmeric-lang/turmeric-spices/blob/main/spices/tls/README.md).
- [mbedTLS API docs](https://mbed-tls.readthedocs.io/).
