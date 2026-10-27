# Plan: Session Middleware for tur-tourist (Swappable Storage)

> **Status:** Draft
> **Last Updated:** 2026-06-01
> **Type:** Spice
> **Spice Location:** `turmeric-spices/spices/tourist-session`

---

## Overview

This plan describes `tur-tourist-session`: a session middleware spice for
`tur-tourist` that:

1. Reads / writes a session cookie on every request
2. Loads and saves session data through a **pluggable store interface** so the
   storage backend is swappable without changing application code
3. Ships with three concrete store implementations out of the box:
   - **Memory store** -- in-process hash table; no external deps
   - **File store** -- per-session files on disk; survives restarts
   - **Valkey/Redis store** -- external key-value store; works across processes
     and servers

The session API is modelled on Ruby's Rack session middleware
(`Rack::Session::Cookie`, `Rack::Session::Memcache`, etc.) adapted to
Turmeric's ownership model and the tourist context pattern.

---

## Background

HTTP is stateless. Session middleware creates the illusion of per-user
continuity by assigning a random, unguessable session ID to each browser and
storing associated data server-side (or signed/encrypted client-side). The
session ID travels as an `HttpOnly; Secure` cookie.

Turmeric's `tur-tourist` already supports arbitrary `use!` middleware. A session
spice should slot into that pattern with one `(use! (session-mw store))` call,
after which any route handler can call `(session-get ctx "key")` and
`(session-set! ctx "key" "value")`.

---

## Scope

### In scope for v0.1.0

- **Session middleware** (`tourist/session`):
  - Reads `Cookie` header on each request; generates a new session ID if absent
  - Loads session data from the store at request start
  - Flushes dirty session data to the store at request end
  - Sets `Set-Cookie` in the response (HttpOnly, configurable Secure/SameSite)
  - Expiry: absolute TTL (default 24 h); rolling sessions optional via config

- **Store interface** (`session/store`):
  - `store-load`  -- load a session map by ID; returns `option<map<:cstr,:cstr>>`
  - `store-save`  -- persist a session map
  - `store-delete`-- delete a session by ID (used for `session-destroy!`)
  - Protocol defined in pure Turmeric via typeclass; any spice can implement it

- **Memory store** (`session/memory-store`):
  - Thread-safe in-process map protected by a `Mutex`
  - Sessions expire after TTL using a simple sweep on `store-load`
  - No external dependencies; suitable for development and single-process apps

- **File store** (`session/file-store`):
  - Each session serialised as a JSON file under a configurable directory
  - File named `<session-id>.json`; atomic write via `rename`
  - Survives process restarts; single-machine only

- **Valkey/Redis store** (`session/valkey-store`):
  - Connects to a Valkey (or Redis) server via TCP using the RESP protocol
  - Session stored as a Redis `HASH` with a `EXPIRE` TTL
  - Reconnect on disconnection; configurable max-retry count
  - Requires `tur-valkey` spice (see below)

- **Session helpers in tourist context**:
  - `(session-get  ctx key)`   -- `option<:cstr>`
  - `(session-set! ctx key val)` -- `:void`
  - `(session-del! ctx key)`   -- `:void`
  - `(session-destroy! ctx)`   -- `:void` (deletes session from store + clears cookie)
  - `(session-id ctx)`         -- `:cstr` (current session ID)

- **Cookie config struct** for `HttpOnly`, `Secure`, `SameSite`, `Path`,
  `Domain`, and `Max-Age`

### Out of scope for v0.1.0

| Future enhancement | Description |
|--------------------|-------------|
| Signed / encrypted cookie store | HMAC or AES-GCM session data in the cookie itself |
| PostgreSQL store | Session backed by a database table |
| SQLite store | Embedded database store for single-process apps |
| Session rotation | Regenerate session ID after privilege escalation (CSRF defence) |
| Flash messages | One-shot session values consumed on next read |
| Cluster-aware expiry | Coordinated TTL across Valkey cluster nodes |

---

## API Design

### `session-mw`

```turmeric
;;; session-mw -- create session middleware bound to a store.
;;;
;;; Parameters:
;;;   store  :int          -- store handle (memory-store, file-store, or valkey-store)
;;;   config :SessionConfig -- cookie and TTL configuration
;;;
;;; Returns:
;;;   A tourist use! compatible middleware :int.
;;;
;;; Example:
;;;   (use! (session-mw (memory-store-new) default-session-config))
(session-mw store config)  ;; => :int (middleware)
```

### `SessionConfig`

```turmeric
;;; Session middleware configuration.
(defstruct SessionConfig
  cookie-name  :cstr   ;; default "tur_session"
  path         :cstr   ;; default "/"
  domain       :cstr   ;; default 0 (omit Domain attribute)
  max-age      :int    ;; TTL in seconds; default 86400 (24 h)
  secure?      :int    ;; 1 = add Secure attribute; default 1
  http-only?   :int    ;; 1 = add HttpOnly attribute; default 1
  same-site    :cstr   ;; "Strict" | "Lax" | "None"; default "Lax"
  rolling?     :int)   ;; 1 = reset TTL on each request; default 0

;;; Sensible defaults (all sites).
(def default-session-config
  (SessionConfig "tur_session" "/" 0 86400 1 1 "Lax" 0))

;;; Development defaults (HTTP-friendly: Secure=0).
(def dev-session-config
  (SessionConfig "tur_session" "/" 0 86400 0 1 "Lax" 0))
```

### Session context helpers

```turmeric
;;; session-get -- read a session value.
;;; Returns none-value if the key is absent.
(session-get ctx key)    ;; => option<:cstr>

;;; session-set! -- write a session value. Marks session dirty.
(session-set! ctx key val)  ;; => :void

;;; session-del! -- remove a key from the session. Marks session dirty.
(session-del! ctx key)   ;; => :void

;;; session-destroy! -- delete the session from the store and clear the cookie.
(session-destroy! ctx)   ;; => :void

;;; session-id -- return the current session ID string.
(session-id ctx)         ;; => :cstr
```

### Store interface (typeclass)

```turmeric
;;; SessionStore typeclass -- implement to provide a custom store.
(deftypeclass SessionStore [S]
  ;;; Load a session by ID. Returns none-value if session does not exist or
  ;;; has expired.
  (store-load   [s :S id :cstr] :int)       ;; => option<map<:cstr,:cstr>>

  ;;; Save a session. Overwrites any existing session with the same ID.
  ;;; ttl is the store-side TTL hint in seconds (may be ignored by stores
  ;;; that manage TTL externally, e.g. Valkey EXPIRE).
  (store-save   [s :S id :cstr data :int ttl :int] :void)

  ;;; Delete a session. No-op if the ID does not exist.
  (store-delete [s :S id :cstr] :void))
```

Applications that need a custom store (e.g., a database-backed store) implement
this typeclass. The middleware is parameterised on `[S : SessionStore]` so the
type checker enforces the contract.

---

## Memory Store

```turmeric
;;; memory-store-new -- create a new in-process session store.
;;; Thread-safe; uses a Mutex<HashMap<:cstr, SessionEntry>>.
;;; Sessions expire lazily on load if their wall-clock deadline has passed.
(memory-store-new)  ;; => :MemoryStore

;;; memory-store-count -- number of live sessions (for tests and dashboards).
(memory-store-count store)  ;; => :int
```

Internal representation:

```turmeric
(defstruct SessionEntry
  data      :int    ;; map<:cstr, :cstr> handle
  expires-at :int)  ;; Unix timestamp (seconds)
```

The memory store does no background sweeping. Expired entries are evicted
lazily when their ID is loaded. A future `memory-store-sweep` call can be added
for long-running servers that accumulate stale sessions.

---

## File Store

```turmeric
;;; file-store-new -- create a session store backed by the filesystem.
;;;
;;; Parameters:
;;;   dir :cstr -- directory to store session files (must be writable)
;;;
;;; Session files: <dir>/<session-id>.json
;;; Expiry metadata stored in the JSON envelope.
(file-store-new dir)  ;; => :FileStore
```

File format (JSON envelope):

```json
{
  "expires_at": 1717200000,
  "data": {
    "user_id": "42",
    "csrf_token": "abc123"
  }
}
```

Writes are atomic: data is written to `<file>.tmp` and then `rename`d to
`<file>`. On POSIX, `rename` is atomic across a single filesystem.

---

## Valkey/Redis Store

```turmeric
;;; valkey-store-new -- create a session store backed by Valkey or Redis.
;;;
;;; Parameters:
;;;   host    :cstr -- e.g. "127.0.0.1"
;;;   port    :int  -- e.g. 6379
;;;   db      :int  -- Redis DB index (0-15)
;;;   prefix  :cstr -- key prefix, e.g. "sess:" (default "tur_sess:")
;;;
;;; Connections are per-worker (one TCP connection per thread from a pool).
(valkey-store-new host port db prefix)  ;; => :ValkeyStore
```

Redis key layout:

```
HSET  tur_sess:<session-id>  user_id "42"  csrf_token "abc123"
EXPIRE tur_sess:<session-id>  86400
```

`store-load` uses `HGETALL`; `store-save` uses `HMSET` + `EXPIRE` in a pipeline;
`store-delete` uses `DEL`.

The store holds a `Mutex<Queue<ValkeyConn>>` connection pool. When a worker
thread needs a connection it pops from the pool (blocking if empty); after use
it pushes back. A connection that has been idle for > 60 s is replaced on next
use (detected by a failed `PING`).

### `tur-valkey` spice dependency

The Valkey/Redis store is provided by a new minimal `tur-valkey` spice
(separate `build.tur`, peer to `tourist-session`). It implements:

- RESP2 client (inline commands + bulk strings)
- `PING`, `GET`, `SET`, `HSET`, `HGETALL`, `HMSET`, `EXPIRE`, `DEL`
- TCP reconnect with exponential back-off (3 retries, max 2 s)
- No TLS in v0.1.0 (use a TLS proxy like `stunnel` for encrypted Valkey)

The valkey spice is intentionally minimal -- it is not a general Redis client.
Other spices that need Redis should depend on a future `tur-redis` general
client spice.

---

## Full Usage Example

```turmeric
(import tourist/app      :refer [tourist])
(import tourist/dsl      :refer [get! post!])
(import tourist/helpers  :refer [text redirect])
(import tourist/middleware :refer [use!])
(import session/mw       :refer [session-mw dev-session-config])
(import session/memory-store :refer [memory-store-new])
(import session/ctx      :refer [session-get session-set! session-destroy!])

(defn login-handler [ctx : int] : int
  (session-set! ctx "user_id" "42")
  (redirect "/dashboard"))

(defn dashboard-handler [ctx : int] : int
  (let [uid (session-get ctx "user_id")]
    (if (ok? uid)
      (text (str-concat "Hello, user " (ok-val uid)))
      (redirect "/login"))))

(defn logout-handler [ctx : int] : int
  (session-destroy! ctx)
  (redirect "/login"))

(defn main [] : int
  (let [store (memory-store-new)
        s     (tourist 3000
                (use! (session-mw store dev-session-config))
                (get!  "/login"     (fn [ctx] (redirect "/login-form")))
                (post! "/login"     login-handler)
                (get!  "/dashboard" dashboard-handler)
                (post! "/logout"    logout-handler))]
    (server-stop s)
    0))
```

```sweet-exp
import tourist/app      :refer [tourist]
import tourist/dsl      :refer [get! post!]
import tourist/helpers  :refer [text redirect]
import tourist/middleware :refer [use!]
import session/mw       :refer [session-mw dev-session-config]
import session/memory-store :refer [memory-store-new]
import session/ctx      :refer [session-get session-set! session-destroy!]

defn login-handler [ctx : int] : int
  session-set!(ctx "user_id" "42")
  redirect("/dashboard")

defn dashboard-handler [ctx : int] : int
  let [uid session-get(ctx "user_id")]
    if ok?(uid)
      text(str-concat("Hello, user " ok-val(uid)))
      redirect("/login")

defn logout-handler [ctx : int] : int
  session-destroy!(ctx)
  redirect("/login")

defn main [] : int
  let [store memory-store-new()
       s     tourist(3000
               use! session-mw(store dev-session-config)
               get!  "/login"     fn [ctx] redirect("/login-form")
               post! "/login"     login-handler
               get!  "/dashboard" dashboard-handler
               post! "/logout"    logout-handler)]
    server-stop(s)
    0
```

---

## Architecture

```
tourist/session (use! middleware)
  |
  +-- session/cookie    -- parse Cookie header; build Set-Cookie string
  |
  +-- session/store     -- SessionStore typeclass definition
  |
  +-- session/ctx       -- attach session data to tourist context;
  |                        session-get / session-set! / session-del! / session-destroy!
  |
  +-- session/mw        -- session-mw: load-on-request, save-on-response
  |
  +-- session/id        -- session ID generation (32 random bytes, hex-encoded)
  |
  session/memory-store  -- MemoryStore (Mutex<HashMap>)
  session/file-store    -- FileStore (per-session JSON files)
  session/valkey-store  -- ValkeyStore (RESP2 connection pool)
        |
        tur-valkey (peer spice)  -- minimal RESP2 client
```

The middleware adds two fields to the tourist context at request start:

| Field | Type | Set by |
|-------|------|--------|
| `ctx-session-id` | `:cstr` | `session/mw` on load |
| `ctx-session-data` | `:int` (map handle) | `session/mw` on load |
| `ctx-session-dirty` | `:int` (bool) | `session-set!` / `session-del!` / `session-destroy!` |

On response flush, the middleware checks `ctx-session-dirty`; if set, it calls
`store-save` and appends `Set-Cookie` to the response headers.

---

## Spice Layout

```
turmeric-spices/spices/tourist-session/
  build.tur
  README.md
  src/session/
    id.tur            -- session ID generation (CSPRNG, hex encode)
    cookie.tur        -- Cookie header parsing; Set-Cookie construction
    store.tur         -- SessionStore typeclass
    ctx.tur           -- session-get, session-set!, session-del!, session-destroy!, session-id
    mw.tur            -- session-mw constructor; request-load / response-flush logic
    memory-store.tur  -- MemoryStore implementation
    file-store.tur    -- FileStore implementation
    valkey-store.tur  -- ValkeyStore implementation (depends on tur-valkey)
  tests/session/
    id_test.tur
    cookie_test.tur
    memory_store_test.tur
    file_store_test.tur
    valkey_store_test.tur   -- requires a running Valkey; tagged requires.valkey
    integration_test.tur    -- full tourist + session middleware round-trip

turmeric-spices/spices/valkey/
  build.tur
  README.md
  src/valkey/
    conn.tur          -- TCP connect, RESP2 send/recv
    resp.tur          -- RESP2 serialiser and parser
    pool.tur          -- Mutex<Queue<ValkeyConn>> pool
    commands.tur      -- PING, SET, GET, HSET, HGETALL, HMSET, EXPIRE, DEL
  tests/valkey/
    resp_test.tur
    commands_test.tur     -- requires.valkey
```

---

## Implementation Phases

- [ ] **SS0** -- `session/id`: CSPRNG-based session ID generation (32 bytes from
  `getrandom(2)` or `/dev/urandom`, hex-encoded to 64-char `:cstr`). Collision
  probability negligible; no uniqueness check required against the store.

- [ ] **SS1** -- `session/cookie`: parse `Cookie:` header (semicolon-separated
  `name=value` pairs); construct `Set-Cookie` string with all config attributes.

- [ ] **SS2** -- `session/store`: `SessionStore` typeclass with `store-load`,
  `store-save`, `store-delete`. Session data type is `map<:cstr,:cstr>`
  (HAMT-backed; imported from `tur-map`).

- [ ] **SS3** -- `session/memory-store`: thread-safe `MemoryStore` using
  `Mutex<HashMap<cstr,SessionEntry>>`. Lazy expiry on `store-load`. Basic tests.

- [ ] **SS4** -- `session/ctx` and `session/mw`: extend tourist context with
  session fields; implement `session-mw` constructor, per-request load (using
  cookie session ID or generating a new one), per-response flush (save + Set-Cookie
  when dirty).

- [ ] **SS5** -- Integration test: full tourist app with `use! (session-mw ...)`,
  verify cookie round-trip, dirty-flag flush, `session-destroy!` clears cookie,
  missing cookie creates fresh session.

- [ ] **SS6** -- `session/file-store`: JSON serialisation of session entry;
  atomic write via `rename`; load with expiry check; tests.

- [ ] **SS7** -- `tur-valkey` spice: RESP2 client (`conn`, `resp`, `pool`,
  `commands`); tests that run against a live Valkey process (tagged
  `requires.valkey`; skipped in CI without a Valkey service).

- [ ] **SS8** -- `session/valkey-store`: `ValkeyStore` backed by the `tur-valkey`
  pool; `HGETALL`/`HMSET`/`EXPIRE`/`DEL` session lifecycle; tests tagged
  `requires.valkey`.

- [ ] **SS9** -- Documentation and release: `docs/guides/session-guide.md`;
  update `docs/guides/web-stack-guide.md` with a session section; tag
  `tourist-session-v0.1.0` and `valkey-v0.1.0`.

---

## Design Notes

### Why a separate spice, not built into tourist?

Session management requires optional external dependencies (Valkey). Bundling
it into `tur-tourist` would force all tourist users to depend on the Valkey
client even if they never use sessions. A separate spice keeps the core light
and lets the session spice evolve independently (e.g., adding a PostgreSQL
store without touching tourist internals).

### Session ID security

Session IDs are 32 random bytes (256 bits) from the OS CSPRNG, hex-encoded.
This provides 256 bits of entropy, making brute-force enumeration infeasible.
The middleware does **not** sign or encrypt the session ID -- the ID itself is
a bearer token. Applications that need tamper-evident tokens (e.g., for
client-side sessions) should use the future signed-cookie store.

### Thread safety

Each worker thread handles one request at a time. The session map loaded into
`ctx-session-data` is **not shared** between workers -- it is per-request. The
store implementations (memory, file, valkey) are the only shared state, and
they each use appropriate locking:

- `MemoryStore`: `Mutex<HashMap>` -- one lock per map operation
- `FileStore`: filesystem rename atomicity
- `ValkeyStore`: `Mutex<Queue<ValkeyConn>>` connection pool; each operation
  uses one connection for its duration

### Rolling sessions

When `rolling?` is set in `SessionConfig`, `session/mw` always saves and
re-issues `Set-Cookie` even when the session is not dirty. This resets the
`Max-Age` on each request, implementing a sliding-window expiry. Rolling
sessions increase store write pressure; they are off by default.

### Valkey RESP2 vs RESP3

`tur-valkey` v0.1.0 uses RESP2 (the universal protocol). RESP3 (available in
Redis 6+ and Valkey 7+) offers typed responses and push notifications. RESP3
support can be added in a later version without changing the `session/valkey-store`
API.

---

## Risks and Open Questions

1. **Session fixation.** The middleware does not regenerate session IDs after
   login. Applications must call a future `(session-rotate! ctx)` (post-v0.1.0)
   or manually `(session-destroy! ctx)` + re-create after privilege escalation.
   Document this prominently.

2. **`getrandom` availability.** `getrandom(2)` is Linux 3.17+. On older
   kernels, fall back to reading `/dev/urandom`. macOS and FreeBSD use
   `arc4random_buf`. The `session/id` module must handle all three.

3. **Map serialisation format.** The file and Valkey stores need a
   stable serialisation for `map<:cstr,:cstr>`. Using `tur-json` for the file
   store is straightforward. For Valkey, `HSET` maps keys and values to Redis
   fields directly (no extra encoding needed). Coordinate with `tur-json` and
   `tur-map` on the canonical round-trip.

4. **Cookie max size.** If session data is accidentally stored in the cookie
   (future signed-cookie store), browsers reject cookies over ~4 KB. Not a
   risk for the server-side stores in v0.1.0, but worth noting for the future
   cookie store.

5. **Valkey connection pool sizing.** Default pool size is `num_workers + 2`.
   On highly concurrent servers this may be too small; expose a
   `valkey-store-new-pool` variant that accepts an explicit pool size.

6. **File store on Windows.** `rename` semantics on Windows differ (not atomic
   when the destination exists). Flag the file store as `requires.posix` and
   document the limitation. A Windows-safe path can use `MoveFileEx` with
   `MOVEFILE_REPLACE_EXISTING` in a future platform layer.

---

## Files to Change

| File | Change |
|------|--------|
| `turmeric-spices/spices/tourist-session/build.tur` | New spice; depends on `tur-tourist`, `tur-map`, `tur-json` |
| `turmeric-spices/spices/tourist-session/src/session/*.tur` | New modules (id, cookie, store, ctx, mw, memory-store, file-store, valkey-store) |
| `turmeric-spices/spices/tourist-session/tests/session/*.tur` | New test suite |
| `turmeric-spices/spices/valkey/build.tur` | New minimal RESP2 client spice |
| `turmeric-spices/spices/valkey/src/valkey/*.tur` | New modules (conn, resp, pool, commands) |
| `turmeric-spices/spices/valkey/tests/valkey/*.tur` | New test suite (tagged requires.valkey) |
| `turmeric-spices/README.md` | Add entries for `tur-tourist-session` and `tur-valkey` |
| `docs/guides/session-guide.md` | New -- full session middleware guide |
| `docs/guides/web-stack-guide.md` | Add "Sessions" section |

---

## Phase Status

| Phase | Title | Status |
|-------|-------|--------|
| SS0 | Session ID generation | Pending |
| SS1 | Cookie parsing and construction | Pending |
| SS2 | SessionStore typeclass | Pending |
| SS3 | Memory store | Pending |
| SS4 | Context extension + middleware | Pending |
| SS5 | Integration tests | Pending |
| SS6 | File store | Pending |
| SS7 | tur-valkey RESP2 client | Pending |
| SS8 | Valkey store | Pending |
| SS9 | Documentation and release | Pending |
