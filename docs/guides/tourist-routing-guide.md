---
title: Tourist Routing Composition
category: Networking and Web
description: Compose tur-tourist HTTP sub-apps with mount and prefix combinators for larger applications
audience: developers building larger HTTP applications with tur-tourist
since: tourist v0.2.0
tags:
  - tourist
---

# Tourist Routing Composition

`tur-tourist` defaults to a flat list of routes and middleware: every
`get!`/`post!`/`use!` you pass to `tourist` shares one path namespace.
That works well for small services, but larger applications usually want
to split into independently-defined sub-apps that get composed at the
top level. `tourist/routing` adds two Rack-inspired combinators for
exactly that:

- **`url-map!`** -- mount sub-apps at path prefixes; longest-prefix wins.
- **`cascade!`** / **`cascade-with!`** -- try apps in order; fall through
  on a configurable set of statuses (default `{404, 405}`).

Both combinators compose with `use!` middleware, with each other, and
recursively (a sub-app can itself be a `url-map!` or `cascade!`).

## The `mount!` helper

Turmeric's variadic `& rest :T` requires a single rest type, so the
natural Rack form `(url-map! "/api" api-app "/" public-app)` (mixing
`:cstr` and `:int`) cannot type-check. `mount!` boxes a `(prefix, item)`
pair into a single `:int` handle, so `url-map!` can declare
`& mounts :int` and accept any number of them.

```turmeric
(import tourist/routing :refer [url-map! mount!])

(url-map! (mount! "/api"   api-app)
          (mount! "/admin" admin-app)
          (mount! "/"      public-app))
```

```sweet-exp
import tourist/routing :refer [url-map! mount!]

url-map!
  mount! "/api"   api-app
  mount! "/admin" admin-app
  mount! "/"      public-app
```

`item` may be any of:

- a `Route` (handle from `get!` / `post!` / ...);
- another sub-app (handle from `url-map!` / `cascade!` / `cascade-with!`).

## `url-map!`

```turmeric
(url-map! & mounts :int)  ;; => sub-app handle
```

```sweet-exp
url-map! & mounts :int  ;; => sub-app handle
```

Matching rules:

- Prefixes are sorted **longest-first at construction time**; registration
  order is irrelevant. `(mount! "/api" a) (mount! "/api/v2" b)` and the
  reverse both put `/api/v2` ahead of `/api`.
- A prefix matches if the request path either equals it or has `/` as its
  next character (so `/api` matches `/api` and `/api/users`, but **not**
  `/apiv2`).
- `"/"` is a catch-all: it matches every path and does **not** strip
  anything. Use it as the last/widest mount when you want a default
  handler at the same dispatch level.
- When a match is found, the matched prefix is stripped from `ctx->path`
  before the inner sub-app runs. The original path is preserved on the
  ctx and available via `req-full-path` from `tourist/param`.
- If stripping consumes the entire path (request `/api`, mount `/api`),
  the sub-app sees `/` so its inner `(get! "/" ...)` still matches.
- If no prefix matches, `url-map!` returns 0 (no match) and the outer
  dispatcher continues -- ultimately falling back to a 404 if nothing
  else handles the request.

```turmeric
(import tourist/app     :refer [tourist])
(import tourist/dsl     :refer [get!])
(import tourist/helpers :refer [text])
(import tourist/routing :refer [url-map! mount!])
(import tourist/param   :refer [req-full-path])
(import httpd/request   :refer [req-path])

(defn echo [ctx]
  (text (req-path ctx)))      ;; sees stripped path

(defn api-routes []
  (url-map! (mount! "/users" (get! "/" echo))
            (mount! "/items" (get! "/" echo))))

(tourist 3000
  (url-map! (mount! "/api" (api-routes))
            (mount! "/"    (get! "/" (fn [_] (text "home"))))))
```

```sweet-exp
import tourist/app     :refer [tourist]
import tourist/dsl     :refer [get!]
import tourist/helpers :refer [text]
import tourist/routing :refer [url-map! mount!]
import tourist/param   :refer [req-full-path]
import httpd/request   :refer [req-path]

defn echo [ctx]
  text $ req-path ctx       ;; sees stripped path

defn api-routes []
  url-map!
    mount! "/users" get!("/" echo)
    mount! "/items" get!("/" echo)

tourist 3000
  url-map!
    mount! "/api" api-routes()
    mount! "/"    get!("/" (fn [_] text("home")))
```

A request for `GET /api/users` hits `echo` with `req-path` = `/` and
`req-full-path` = `/api/users`.

### `req-full-path`

```turmeric
(import tourist/param :refer [req-full-path])
```

```sweet-exp
import tourist/param :refer [req-full-path]
```

Inside a sub-app mounted via `url-map!`, `(req-path ctx)` returns the
path with the mount prefix stripped. `(req-full-path ctx)` always
returns the original path as received by the server. Use it when a
sub-app needs to construct absolute URLs (e.g. in `Location` headers
for redirects, or for canonical URL generation) -- the combinator does
not rewrite redirect targets on the way out.

## `cascade!` and `cascade-with!`

```turmeric
(cascade!      & apps :int)                  ;; pass-through = {404, 405}
(cascade-with! passes :int & apps :int)      ;; passes is a cons list of int
```

```sweet-exp
cascade!      & apps :int                  ;; pass-through = {404, 405}
cascade-with! passes :int & apps :int      ;; passes is a cons list of int
```

`cascade!` runs each app in turn on the same ctx. If an app returns 0
("did not match this request") or a response whose status appears in
the pass-through set, `cascade!` frees that response (where applicable)
and tries the next app. The first non-pass response is returned. If
every app passes through, `cascade!` itself returns 0 so the outer
dispatcher can continue.

```turmeric
(import tourist/routing :refer [cascade!])
(import tourist/static  :refer [serve-static!])

;; Dynamic routes first; fall back to static files on 404.
(tourist 3000
  (cascade!
    (get! "/about" (fn [_] (html "<h1>About</h1>")))
    (serve-static! "/" "./public")))
```

```sweet-exp
import tourist/routing :refer [cascade!]
import tourist/static  :refer [serve-static!]

;; Dynamic routes first; fall back to static files on 404.
tourist 3000
  cascade!
    get! "/about" (fn [_] html("<h1>About</h1>"))
    serve-static! "/" "./public"
```

`cascade-with!` takes an explicit pass-through list, built with the
variadic `passes!` constructor. Use it when the first app should also
fall through on, say, 503 (maintenance):

```turmeric
;; Treat 404, 405, and 503 as fall-through statuses.
(cascade-with! (passes! 404 405 503)
               primary
               fallback)
```

```sweet-exp
;; Treat 404, 405, and 503 as fall-through statuses.
cascade-with!
  passes! 404 405 503
  primary
  fallback
```

## Composing the combinators

Sub-apps nest. Two patterns that come up often:

### Versioning with cascade

```turmeric
(defn api-app []
  (cascade!
    (url-map! (mount! "/v2" (v2-app)))
    (url-map! (mount! "/v1" (v1-app)))))
```

```sweet-exp
defn api-app []
  cascade!
    url-map! $ mount! "/v2" v2-app()
    url-map! $ mount! "/v1" v1-app()
```

A request for `/v2/things` hits `v2-app`. A request for `/v1/things`
falls through `v2-app` (no `/v1` prefix → returns 0) and is served by
`v1-app`. A request for `/v3/things` 404s.

### Middleware scoping

`use!` runs in declaration order before route dispatch. Middleware
registered at the **top level** runs for every request -- including
those routed through `url-map!` and `cascade!`:

```turmeric
(tourist 3000
  (use! auth-mw)              ;; runs for all paths
  (url-map! (mount! "/api"  api-app)
            (mount! "/"     public-app)))
```

```sweet-exp
tourist 3000
  use! auth-mw              ;; runs for all paths
  url-map!
    mount! "/api"  api-app
    mount! "/"     public-app
```

Middleware registered **inside a sub-app** runs only for that sub-app's
paths. This matches Rack's behaviour and is a natural consequence of
tourist's function-composition middleware model.

## Semantics and pitfalls

- **Path strip mutates the ctx, in place.** `url-map!` swaps `ctx->path`
  to point at the stripped suffix (or a literal `"/"` if the prefix
  consumed the whole path) and restores it after the sub-app returns.
  Don't capture `ctx->path` across the boundary -- read it fresh inside
  your handler, or use `req-full-path` for the original.
- **Cascade and stateful middleware.** If an app's middleware has side
  effects (opening a DB transaction, locking a resource), and the
  cascade falls through to the next app, those effects are not rolled
  back. Keep cascade-eligible middleware side-effect-free, or use a
  finalizer pattern that runs regardless of pass-through.
- **Redirects don't see the mount.** `(redirect "/login")` from a sub-app
  is relative to the sub-app's own root. If you need an absolute path,
  derive it from `req-full-path` (e.g. compute the prefix as
  `full - stripped` and prepend it manually).
- **Mount-table sort is O(n log n)** at construction time. For thousand-
  prefix tables you'd notice startup cost; the per-request lookup is a
  linear scan of a sorted cons list. Typical apps with fewer than a few
  dozen mounts are unaffected.

## API summary

| Function | Module | Returns | Notes |
|----------|--------|---------|-------|
| `url-map! & mounts :int` | `tourist/routing` | sub-app `:int` | Sort longest-prefix-first at build |
| `cascade! & apps :int` | `tourist/routing` | sub-app `:int` | Pass-through = `{404, 405}` |
| `cascade-with! passes :int & apps :int` | `tourist/routing` | sub-app `:int` | Custom pass-through set |
| `mount! prefix :cstr item :int` | `tourist/routing` | mount handle `:int` | For `url-map!` only |
| `passes! & statuses :int` | `tourist/routing` | status-list `:int` | For `cascade-with!` only |
| `req-full-path ctx :int` | `tourist/param` | `:cstr` | Original path, unstripped |

See also:

- [tourist-session-guide.md](tourist-session-guide.md) -- cookie-backed
  sessions with swappable storage (memory, file).
- [web-stack-guide.md](web-stack-guide.md) -- the broader
  tourist/httpd/template overview.
