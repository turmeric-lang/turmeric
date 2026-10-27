---
title: HTTPD Compression + tur/zlib Spice Plan
category: Planning
description: Break the M6 compression middleware out of the now-archived httpd-middleware-async plan into its own focused effort. Authors a new `tur/zlib` spice in ../turmeric-spices wrapping system zlib, then implements `mw-compress` in stdlib/httpd-compress.tur depending on it, with fixtures gated behind `requires.spices`.
---

# HTTPD Compression + `tur/zlib` Spice -- Plan

> **Status:** Draft Plan
> **Last Updated:** 2026-06-02
> **Type:** Stdlib + spice authoring (cross-repo)
> **Related:**
> - `docs/archive/history/httpd-middleware-async-plan.md` -- the parent plan; M6
>   was the one PR deferred because the `tur/zlib` spice didn't exist.
>   All other phases of that plan shipped 2026-06-02.
> - `../turmeric-spices/spices/png/build.tur`,
>   `../turmeric-spices/spices/json/build.tur` -- reference shape for a
>   spice that wraps a `:cmake-deps` C library.
> - `stdlib/httpd.tur` -- M0 response-header surface (mw-compress writes
>   Content-Encoding + Vary via httpd-resp-header!).
> - `docs/guides/httpd-guide.md`, `docs/guides/httpd-async-guide.md` --
>   destinations for the brief "compression middleware" section once M6
>   ships.

---

## Goal

Two deliverables, one cross-repo:

1. **`tur/zlib` spice** in `../turmeric-spices/spices/zlib/` wrapping
   system zlib (or upstream `madler/zlib` via `:cmake-deps`).  Exposes
   gzip encode / decode and (optionally) raw deflate, with binary-safe
   buffer in / buffer out signatures.

2. **`mw-compress` middleware** in `stdlib/httpd-compress.tur`,
   depending on the `tur/zlib` spice, layered on top of the M0
   response-header surface from the parent plan.  Negotiates
   `gzip` via `Accept-Encoding`, compresses `resp_body`, sets
   `Content-Encoding`/`Vary`, rewrites `Content-Length`.

The deliverables are independent: the spice is useful on its own
(zlib/gzip is a common need); mw-compress is the first stdlib consumer.

---

## Track Z -- the `tur/zlib` spice

### Phase Z0 -- Scaffold `../turmeric-spices/spices/zlib/`

Mirror the existing `png` / `json` spice layout:

```
../turmeric-spices/spices/zlib/
  build.tur         ;; defpackage with :cmake-deps for zlib
  README.md
  src/
    zlib.tur        ;; Turmeric module: tur/zlib
  tests/
    zlib-roundtrip/ ;; gzip-encode -> gzip-decode -> equality
```

`build.tur`:

```turmeric
(defpackage tur-zlib
  :name        "tur-zlib"
  :version     "0.1.0"
  :description "zlib + gzip encode/decode for Turmeric"
  :license     "Zlib"
  :cmake-deps #{
    "zlib" #{:url     "https://github.com/madler/zlib"
             :ref     "v1.3.1"
             :options #{:ZLIB_BUILD_EXAMPLES "OFF"
                        :BUILD_SHARED_LIBS    "OFF"}}
  }
  :exports #{
    "tur/zlib" ["gzip-encode" "gzip-decode"
                "deflate-raw" "inflate-raw"]
  })
```

Track open question Z-OQ1: prefer pinning a known-good upstream ref
(reproducible) vs. linking to system zlib (lighter, faster CI).
Recommendation: pin upstream via `:cmake-deps` for reproducibility,
matching the `png` spice's pattern.

### Phase Z1 -- Turmeric surface

`src/zlib.tur` exposes binary-safe buffer in / buffer out:

```turmeric
;;; gzip-encode -- gzip-compress a raw byte buffer.
;;;
;;; Parameters:
;;;   data     -- input buffer (ptr<void>)
;;;   data-len -- input byte count
;;;
;;; Returns:
;;;   A newly-allocated GzipBuf (opaque ptr<void>) with the
;;;   compressed bytes; the caller frees via `gzip-buf-free`.
;;;
;;; Errors:
;;;   Returns NULL on allocation failure or zlib internal error.
(defn gzip-encode [data : ptr<void> data-len : int] : ptr<void> ...)

;;; gzip-decode -- gzip-decompress to a new buffer.
;;;
;;; Accepts the standard gzip wrapper (magic bytes 1F 8B).  Use
;;; `inflate-raw` for raw deflate streams.
(defn gzip-decode [data : ptr<void> data-len : int] : ptr<void> ...)

;;; gzip-buf-data / gzip-buf-len -- accessors for the result buffer.
(defn gzip-buf-data [b : ptr<void>] : ptr<void> ...)
(defn gzip-buf-len  [b : ptr<void>] : int       ...)
(defn gzip-buf-free [b : ptr<void>] : nil       ...)
```

The GzipBuf wrapper keeps `data + len` in one allocation so the caller
gets binary-safe output (gzip output can contain embedded NUL).  Matches
the opaque-handle pattern used by `httpd-req-file` in M7 (avoids the
defstruct-by-value codegen rough edges).

### Phase Z2 -- Roundtrip fixture

`tests/zlib-roundtrip/`:

- Encodes a known input (e.g. "hello world" repeated 100x to ensure
  the gzip output is materially shorter than the input).
- Decodes the result.
- Verifies byte-for-byte equality with the original.
- Prints `roundtrip=ok` on success.

Spices land with their own ctest target -- match the existing
`png`/`json` fixture pattern in `../turmeric-spices/`.

### Phase Z3 -- Document in spice README

Short README walking through the encode/decode pair, the GzipBuf
lifecycle, and the spice's link-time dependency on zlib.

---

## Track M6 -- `mw-compress` in stdlib

### Phase M6-0 -- `stdlib/httpd-compress.tur` scaffold

A new top-level stdlib file separate from `stdlib/httpd.tur` so users
who do not want a zlib dependency do not pull it in transitively:

```turmeric
;;; stdlib/httpd-compress.tur -- gzip response compression middleware.
;;;
;;; Loads tur/zlib (from ../turmeric-spices); fails at module resolution
;;; when the spice is absent.
;;;
;;; Since: Phase H8 (M6)

(load "stdlib/httpd.tur")
(load "tur/zlib")
```

The mw-compress factory + its inline-C bodies live here.

Track open question M6-OQ1: should the `(load "tur/zlib")` line use
the spice-import form `(import tur/zlib ...)` instead?  Resolution
depends on how stdlib files reference spices today -- check the spice
guide, prefer the canonical form.

### Phase M6-1 -- `mw-compress` factory

```turmeric
;;; mw-compress -- gzip the response body when the client accepts gzip.
;;;
;;; Inspects Accept-Encoding for "gzip"; when present AND the response
;;; body length exceeds `min-bytes` AND the response is not already
;;; compressed, gzips the body, sets Content-Encoding: gzip,
;;; Vary: Accept-Encoding, and rewrites Content-Length.  Other paths
;;; pass through.
;;;
;;; Skips entries with an existing Content-Encoding (preserves caller
;;; intent; do not double-gzip).
;;;
;;; Runs as the OUTERMOST post-processing middleware so it sees the
;;; final body produced by the handler + any inner middleware.
;;;
;;; Parameters:
;;;   min-bytes -- minimum response body size to compress
;;;   next      -- downstream handler closure
;;;
;;; Example:
;;;   (compose-middleware base mw-log (mw-compress-with 256))
;;;
;;; Since: Phase H8 (M6)
(defn mw-compress-with [min-bytes : int next : int] : ptr<void> ...)

;;; mw-compress -- defaults wrapper (256-byte threshold).
(defn mw-compress [next : int] : ptr<void>
  (mw-compress-with 256 next))
```

Internally:

1. Call `next`.
2. Read `conn->resp_body` + `conn->resp_body_len`.
3. Read request header `Accept-Encoding` (M0); if no `gzip`, return.
4. Check response header `Content-Encoding` (M0); if set, return.
5. If `body_len < min-bytes`, return.
6. `gzip-encode` the body.
7. Replace the response body with the compressed bytes (need a new
   stdlib helper or extend `httpd-resp-body!` to accept raw bytes +
   length, since the body may contain NULs; the existing
   `httpd-resp-body!` takes a cstr).
8. `httpd-resp-header! conn "Content-Encoding" "gzip"`.
9. `httpd-resp-header! conn "Vary" "Accept-Encoding"`.

Track open question M6-OQ2: introduce `httpd-resp-body-bytes!` taking
`(ptr<void>, int)` so binary response bodies (including gzip output)
work without trickery, OR extend `httpd-resp-body!` to detect a
length-prefixed form.  Recommend the dedicated `-bytes!` setter --
matches `httpd-part-data` from M7 (binary-safe-by-design surface).

### Phase M6-2 -- Fixture (spice-gated)

`tests/fixtures/httpd-mw-compress/`:

- `requires.spices` marker so the suite SKIPs when `../turmeric-spices/`
  (and the zlib spice) isn't present, per the parent plan's
  cross-cutting "Optional dependencies" section.
- POSTs a request with `Accept-Encoding: gzip`.
- Handler responds with a >256-byte text body.
- Client reads the response, asserts `Content-Encoding: gzip` is set,
  `Vary: Accept-Encoding` is set, and decompressing the body via the
  spice's `gzip-decode` recovers the original.

A second sub-fixture (or a second request on the same socket) verifies
the no-gzip path: client omits `Accept-Encoding`, handler responds,
mw-compress passes through unchanged.

### Phase M6-3 -- Brief docs entry

Short paragraph appended to `docs/guides/httpd-guide.md` (in the
"middleware" section) and a one-line entry in
`docs/guides/httpd-async-guide.md`'s reference table.  Cross-link the
zlib spice's README from both.

---

## Dependencies / ordering

```
Z0 (scaffold)  --> Z1 (Turmeric surface) --> Z2 (roundtrip)
                                          \-> Z3 (README)

M6-0 (stdlib scaffold, depends on Z1) --> M6-1 (factory) -->
                                          M6-2 (fixture) --> M6-3 (docs)
```

Track Z ships independently as v0.1.0 of `tur-zlib`.  Track M6 lands
once Z1 is available and a deterministic ref is pinned.

---

## Cross-cutting concerns

- **Lifetime / leaks.**  `gzip-encode` allocates the GzipBuf; mw-compress
  is responsible for freeing it after the per-request response write.
  Use a fresh tail field on `HttpdConn` (`void *resp_compress_buf`)
  freed in the iteration cleanup, mirroring how M3 handles
  `req_json_cache` and M7 handles `req_multipart_parts`.
- **Binary safety.**  Gzip output is raw bytes; do not route it through
  `httpd-resp-body!` (which calls `strlen`).  See M6-OQ2.
- **Skip already-encoded.**  Per the parent plan: do not double-gzip
  bodies the handler explicitly compressed (e.g. served a precomputed
  `.gz` blob).
- **Threading.**  Both the blocking pool and the async server run
  mw-compress identically (it's a post-processing wrapper on the
  H7 hook; nothing fiber-aware).  Verified via the A3 sanity check
  in the parent plan.

---

## Out of scope

- Brotli, zstd, deflate-raw negotiation.  Stick to gzip in v0.1.
  Add more codecs in a follow-up once the negotiation surface
  stabilises.
- Streaming compression (encoding the response body progressively).
  The current httpd already buffers the full body in `resp_body`
  before writing; streaming compression is a Track A follow-up the
  parent plan calls out.
- Decompressing request bodies.  Symmetric and useful, but a separate
  feature -- file as a sibling once mw-compress is in use.

---

## Risk

- **Low for Z.**  zlib is a well-known C library with a 30-year stable
  ABI; the spice wrapper is a thin buffer-in/buffer-out shape.
- **Medium for M6.**  Touches `HttpdConn` (new resp_compress_buf
  field), introduces a new `httpd-resp-body-bytes!` setter,
  cross-references M0 headers, and ships as a stdlib file with an
  external dependency -- the first stdlib module to do so.  Plan to
  document the load-time failure mode clearly (the
  `(load "tur/zlib")` line errors out with a useful message when the
  spice is missing, NOT a cryptic link error).

---

## Open questions

1. **Z-OQ1.**  Pin upstream zlib via `:cmake-deps` (reproducible)
   vs. link to system zlib (lighter)?  Recommendation: pin upstream.
2. **M6-OQ1.**  `(load "tur/zlib")` vs. an `(import tur/zlib ...)`
   spice-import form -- confirm canonical syntax against the spice
   guide.
3. **M6-OQ2.**  Dedicated `httpd-resp-body-bytes!` (recommended) vs.
   length-prefixed extension of `httpd-resp-body!`.
4. **M6-OQ3.**  Default `min-bytes` threshold (256 in this draft).
   Common server defaults range 256-1024; benchmark briefly on a
   representative body mix.
5. **Codec negotiation order.**  `Accept-Encoding` can be a
   comma-separated list with q-values; the parser in M6-1 starts with
   a simple `strstr "gzip"` check.  Upgrade to full RFC 7231 parsing
   once / if more codecs land (Out of scope above).
