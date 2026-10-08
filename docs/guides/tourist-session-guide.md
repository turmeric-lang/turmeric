---
title: Tourist Session Middleware
category: Networking and Web
description: Cookie-backed sessions for tur-tourist with swappable storage backends (memory, file)
audience: developers adding login / per-user state to a tur-tourist app
since: tourist-session v0.1.0
---

# Tourist Session Middleware

`tur-tourist-session` is a session middleware spice for `tur-tourist`. It
manages a session cookie, loads and saves per-session data through a
**pluggable store**, and exposes a handful of context helpers
(`session-get`, `session-set!`, `session-destroy!`, ...) for use from
route handlers.

It ships two stores out of the box:

- **memory store** -- in-process, mutex-guarded; no external deps.
- **file store** -- one JSON file per session under a directory you
  choose; survives restarts.

A Valkey/Redis-backed store is deferred but trivially pluggable via the
public `store-new` constructor -- the store is a vtable, not a typeclass,
so any backend with three C-ABI functions plugs in.

For the underlying tourist surface (routes, `use!` / `use-after!`,
combinators), see [tourist-routing-guide.md](tourist-routing-guide.md)
and [web-stack-guide.md](web-stack-guide.md).

## Adding the spice

Add it to your `build.tur` alongside `tourist`:

```turmeric
:spices #{
  "tourist"         #{:url    "https://github.com/turmeric-lang/turmeric-spices"
                      :ref    "tourist-v0.2.5"
                      :subdir "spices/tourist"}
  "tourist-session" #{:url    "https://github.com/turmeric-lang/turmeric-spices"
                      :ref    "tourist-session-v0.1.0"
                      :subdir "spices/tourist-session"}
}
```

Then `tur fetch` (or rely on auto-discovery if the spice is a sibling
checkout). All modules are namespaced under `session/...`.

## The one-liner: drop it into a tourist app

`session-mw` is the middleware constructor. Place its result **directly
in the tourist item list** -- it is already a tourist after-middleware
item, not a value you wrap in `use!`.

```turmeric
(import tourist/app          :refer [tourist])
(import tourist/dsl          :refer [get!])
(import session/mw           :refer [session-mw])
(import session/config       :refer [dev-session-config])
(import session/memory-store :refer [memory-store-new])

(defn main [] : int
  (let [store (memory-store-new)
        srv   (tourist 3000
                (session-mw store (dev-session-config))
                (get! "/" home))]
    (server-stop srv)
    0))
```

`session-mw` has the signature:

```turmeric
(defn session-mw [store : Store cfg : SessionConfig] : int)
```

It returns a tourist item handle. Internally it installs the store and
cookie config into a process-global (read once per request, set once
before the server starts -- the same pattern tourist itself uses for
dispatch state), then returns a `(use-after! ...)` handle.

There is **no** separate before-middleware to register: session loading
is lazy. The first handler call to `session-get` / `session-set!` reads
the cookie, asks the store, and attaches per-request session state to
the context. The after-middleware decides whether to persist and emit a
`Set-Cookie` on the way out. A request that never touches the session
pays nothing and is handed no cookie.

## Cookie configuration

The `SessionConfig` struct controls both the cookie attributes and the
session TTL:

```turmeric
(defstruct SessionConfig :copy
  [cookie-name :cstr   ;; "tur_session"
   path        :cstr   ;; "/"
   domain      :cstr   ;; "" omits the Domain attribute
   max-age     :int    ;; TTL in seconds (also store-side expiry hint)
   secure?     :int    ;; 1 -> "; Secure"
   http-only?  :int    ;; 1 -> "; HttpOnly"
   same-site   :cstr   ;; "Strict" | "Lax" | "None"
   rolling?    :int])  ;; 1 -> re-save + re-cookie every request
                       ;;      (sliding-window expiry)
```

Boolean-ish fields use `:int` (0/1) to match the int-flag convention
used by the rest of tourist's data structures and to keep the struct
trivially copyable.

Two pre-built configs are exported:

```turmeric
(default-session-config)
;; => SessionConfig "tur_session" "/" "" 86400 1 1 "Lax" 0
;;    production: Secure on, HttpOnly on, 24 h, Lax, non-rolling

(dev-session-config)
;; => SessionConfig "tur_session" "/" "" 86400 0 1 "Lax" 0
;;    development: Secure off so the cookie survives plain http://
```

To customise, build one yourself with `make-struct`:

```turmeric
(import session/config :refer [SessionConfig])

(def my-cfg
  (make-struct SessionConfig
    "myapp_sid"   ;; cookie-name
    "/"           ;; path
    ""            ;; domain ("" => omit attribute)
    3600          ;; max-age (1 h)
    1             ;; secure?
    1             ;; http-only?
    "Strict"      ;; same-site
    1))           ;; rolling? -- slide the expiry on every request
```

Cookie attributes map straight to the rendered `Set-Cookie`. With
defaults the browser sees:

```
Set-Cookie: tur_session=<id>; Path=/; Max-Age=86400; HttpOnly; Secure; SameSite=Lax
```

**Note on signing.** Session IDs are 256 bits from the OS CSPRNG and
treated as opaque bearer tokens; they are not signed or encrypted. The
secret lives in the store, not the cookie. Do not try to put
application data in the cookie value -- write to the session map and
look it up server-side.

## Reading and writing the session

All handler-facing helpers live in `session/ctx`. They take the tourist
`Ctx` as their first argument, lazily initialising session state on
first call.

```turmeric
(import session/ctx :refer [session-get session-set! session-del!
                            session-destroy! session-id])

(defn handler [ctx : Ctx] : Response
  ...)
```

The five operations:

| Call | Type | Notes |
|------|------|-------|
| `(session-get ctx key)` | `(Result cstr cstr)` | `ok value` or `err "missing"`. Borrowed -- copy if you need it past the response. |
| `(session-set! ctx key val)` | `:void` | Marks the session dirty. Both key and value are copied into the session map. |
| `(session-del! ctx key)` | `:void` | Marks dirty. No-op if `key` is absent. |
| `(session-destroy! ctx)` | `:void` | Marks the session for destruction: the flush calls `store-delete` and emits an expiring `Set-Cookie`. |
| `(session-id ctx)` | `:cstr` | The 64-char hex session ID (borrowed). |

A read-only `session-get` does **not** dirty the session, so a request
that only reads the session generates no store write and no
`Set-Cookie` (unless `rolling?` is on). Writes always trigger a
`store-save` plus a fresh `Set-Cookie` on the response.

### Persist / re-cookie policy

The after-middleware (`session-flush`, registered for you by
`session-mw`) inspects per-request state and acts as follows:

| Situation | Action |
|-----------|--------|
| `session-destroy!` was called | `store-delete` + expiring `Set-Cookie` |
| Session was written (dirty) | `store-save` + fresh `Set-Cookie` |
| `rolling?` on, established session | `store-save` + fresh `Set-Cookie` (slide expiry) |
| Read-only / never touched | nothing |

## Storage backends

### Memory store

In-process, mutex-guarded singly-linked list of `(id, map, expires-at)`
entries. Suitable for development, tests, and single-process
deployments. State is lost on restart.

```turmeric
(import session/memory-store :refer [memory-store-new memory-store-count])

(let [store (memory-store-new)]
  ...
  ;; memory-store-count is a test/diagnostic helper:
  (println (cstr-of-int (memory-store-count store))))
```

Expiry is lazy: an entry past its deadline is evicted the next time its
ID is looked up.

### File store

One JSON file per session under a directory you provide. Survives
restarts. Suitable for single-host production deployments without an
external session store.

```turmeric
(import session/file-store :refer [file-store-new])

(let [store (file-store-new "/var/lib/myapp/sessions")]
  ...)
```

If the directory does not exist `file-store-new` creates it with mode
`0700`. Writes are atomic (`rename(2)`). Session IDs are hex so they
are always safe path components.

### Swapping backends

Because the store is an opaque `Store` handle threaded through
`session-mw`, swapping backends is a one-line change:

```turmeric
;; memory:
(session-mw (memory-store-new) (default-session-config))

;; file:
(session-mw (file-store-new "/var/lib/myapp/sessions")
            (default-session-config))
```

### Writing a custom store

The store is a small vtable: three C-ABI functions plus an opaque
state pointer. Implement them, then call `store-new`:

```turmeric
(import session/store :refer [Store store-new])

;; load   : (c-fn [state:int id:cstr] int)              ;; SessMap handle, 0 = miss
;; save   : (c-fn [state:int id:cstr map:int ttl:int] int)
;; delete : (c-fn [state:int id:cstr] int)
(defn make-my-store [conn : int] : Store
  (store-new my-load my-save my-delete conn))
```

Session data is a `SessMap` (see `session/data`) -- a compact owned
string-to-string map. Stores must clone on the way in (so the caller
can keep using its copy) and on the way out (so the working copy never
aliases the master copy under your lock).

## Logout and session invalidation

`session-destroy!` is the canonical "log out" call: it tells the flush
to delete the session from the store and to emit an expiring
`Set-Cookie` that clears the browser's copy.

```turmeric
(defn logout [ctx : Ctx] : Response
  (do
    (session-destroy! ctx)
    (redirect "/login")))
```

**Session fixation note.** This version does not regenerate the ID
after privilege changes (no `session-rotate!` yet). For login flows
that care about fixation, destroy and re-create:

```turmeric
(defn login [ctx : Ctx] : Response
  (do
    (session-destroy! ctx)                ;; drop any pre-login session
    (session-set! ctx "user_id" "42")     ;; lazy-init mints a fresh ID
    (redirect "/dashboard")))
```

The `session-destroy!` is honoured at flush time, and the subsequent
`session-set!` re-initialises the state struct for the next request
chain (the new ID is minted lazily on the next request that reads the
session, because the destroyed request emits the expiring cookie this
time around).

## Worked example: login / dashboard / logout

The full flow lives at
[`examples/login_app.tur`](https://github.com/turmeric-lang/turmeric-spices/tree/main/spices/tourist-session/examples/login_app.tur)
in the spice. Run with:

```sh
tur run spices/tourist-session/examples/login_app.tur
```

Reproduced here (handlers typed correctly -- never `:int`):

```turmeric
(defmodule session/examples/login-app
  (import tourist/app          :refer [tourist])
  (import tourist/dsl          :refer [get! post!])
  (import tourist/helpers      :refer [text redirect])
  (import tourist/types        :refer [Ctx])
  (import httpd/types          :refer [Response])
  (import httpd/server         :refer [server-stop])
  (import session/mw           :refer [session-mw])
  (import session/config       :refer [dev-session-config])
  (import session/memory-store :refer [memory-store-new])
  (import session/ctx          :refer [session-get session-set! session-destroy!])
  (export main)

;; "Log in" as user 42 and bounce to the dashboard.
(defn login-handler [ctx : Ctx] : Response
  (do
    (session-set! ctx "user_id" "42")
    (redirect "/dashboard")))

;; Greet the logged-in user, or send anonymous visitors to /login.
(defn dashboard-handler [ctx : Ctx] : Response
  (let [uid (session-get ctx "user_id")]
    (if (.is-ok uid)
      (text (str-concat "Hello, user " (.ok-val uid)))
      (redirect "/login"))))

;; Clear the session and its cookie.
(defn logout-handler [ctx : Ctx] : Response
  (do
    (session-destroy! ctx)
    (redirect "/login")))

(defn login-form [ctx : Ctx] : Response
  (text "POST to /login to sign in"))

(defn main [] : int
  (let [store (memory-store-new)
        srv   (tourist 3000
                (session-mw store (dev-session-config))
                (get!  "/login"     login-form)
                (post! "/login"     login-handler)
                (get!  "/dashboard" dashboard-handler)
                (post! "/logout"    logout-handler))]
    (server-stop srv)
    0))

) ;; end module
```

Curl session:

```sh
curl -i -c jar -b jar -X POST localhost:3000/login
;; -> 302, Set-Cookie: tur_session=<id>; Path=/; ...
curl -i -c jar -b jar localhost:3000/dashboard
;; -> 200, "Hello, user 42"
curl -i -c jar -b jar -X POST localhost:3000/logout
;; -> 302, Set-Cookie: tur_session=; Path=/; Max-Age=0
```

## API summary

| Function | Module | Returns | Notes |
|----------|--------|---------|-------|
| `session-mw store cfg` | `session/mw` | `:int` (tourist item) | Drop in the tourist item list (not wrapped in `use!`) |
| `session-flush ctx resp` | `session/mw` | `Response` | The after-middleware itself; exported for non-tourist hosts |
| `session-get ctx key` | `session/ctx` | `(Result cstr cstr)` | Borrowed value; copy to outlive the response |
| `session-set! ctx key val` | `session/ctx` | `:void` | Marks dirty; copies key + val |
| `session-del! ctx key` | `session/ctx` | `:void` | Marks dirty |
| `session-destroy! ctx` | `session/ctx` | `:void` | Store-delete + expiring cookie at flush |
| `session-id ctx` | `session/ctx` | `:cstr` | 64-char hex (borrowed) |
| `default-session-config` | `session/config` | `SessionConfig` | Secure, HttpOnly, 24 h, Lax |
| `dev-session-config` | `session/config` | `SessionConfig` | Secure off for HTTP localhost |
| `memory-store-new` | `session/memory-store` | `Store` | In-process, mutex-guarded |
| `memory-store-count store` | `session/memory-store` | `:int` | Live (non-evicted) session count |
| `file-store-new dir` | `session/file-store` | `Store` | One JSON file per session under `dir` |
| `store-new load save delete state` | `session/store` | `Store` | Custom-backend constructor |

## Security notes

- **Session IDs** are 32 bytes (256 bits) from the OS CSPRNG
  (`getentropy`, falling back to `/dev/urandom`), hex-encoded. They are
  opaque bearer tokens; treat them like passwords in transit.
- **Always enable `Secure` in production.** `dev-session-config` turns
  it off so the cookie survives plain HTTP for local development; do
  not ship that to a non-localhost environment.
- **`HttpOnly` is on by default.** Leave it on -- it blocks JavaScript
  reads of the cookie and is the cheapest mitigation against XSS-based
  session theft.
- **`SameSite=Lax` is on by default.** Use `"Strict"` for back-office
  apps; use `"None"` only with `Secure` on and only when you genuinely
  need third-party-context cookies.
- **No fixation rotation yet.** Call `session-destroy!` before
  `session-set!` on login/privilege change until `session-rotate!`
  lands.
- **Rolling sessions cost a store write per request.** Leave
  `rolling?` at 0 unless you need sliding-window expiry; an idle
  visitor's session times out and is evicted lazily on next load.

## See also

- [tourist-routing-guide.md](tourist-routing-guide.md) -- `url-map!`,
  `cascade!`, `mount!` for composing larger tourist apps.
- [web-stack-guide.md](web-stack-guide.md) -- the tourist / httpd /
  template overview.
