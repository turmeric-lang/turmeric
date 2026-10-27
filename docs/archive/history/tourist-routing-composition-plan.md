# Plan: Routing Composition for tur-tourist (URLMap + Cascade)

> **Status:** Draft
> **Last Updated:** 2026-06-01
> **Type:** Spice Enhancement
> **Spice Location:** `turmeric-spices/spices/tourist`

---

## Overview

`tur-tourist` currently routes within a single app: a flat list of `use!`
middleware and route handlers that share one path namespace. This plan adds two
composable *routing combinators* inspired by Rack:

| Combinator | Rack analogue | Behaviour |
|------------|--------------|-----------|
| `url-map!` | `Rack::URLMap` | Mount sub-apps at path prefixes; dispatch the first prefix that matches |
| `cascade!` | `Rack::Cascade` | Try apps in order; pass to the next when the previous returns 404 (or any configurable set of statuses) |

Together they let large applications split into independently-defined sub-apps
that are composed at the top level, without any shared global state.

---

## Background

### Rack::URLMap

`Rack::URLMap` strips the matched prefix from `PATH_INFO` before calling the
mounted app, and restores `SCRIPT_NAME` so the sub-app can generate absolute
URLs correctly. The tourist equivalent:

- Match request path against each registered prefix (longest-prefix wins)
- Strip the matched prefix from `ctx` before calling the sub-app's handler
- Restore the original path in the response context so response helpers
  (`redirect`, etc.) use the full path

### Rack::Cascade

`Rack::Cascade` chains apps. If app A returns one of the configured
"pass-through" statuses (default: 404, 405), it forwards the original request
to app B. This is useful for layering a dynamic app over a static file server,
or for splitting an API across independently-deployed route groups.

---

## Scope

### In scope for v0.1.0

- `url-map!` -- prefix-based dispatch; longest-prefix-first matching;
  path stripping/restoring in the tourist context
- `cascade!` -- ordered fallback chain; configurable pass-through status set
  (default `{404 405}`)
- Both combinators compose with existing `use!` middleware and with each other
- Sub-apps may themselves use `url-map!` / `cascade!` (nested composition)
- All-Turmeric implementation; no new C dependencies

### Out of scope for v0.1.0

| Future enhancement | Description |
|--------------------|-------------|
| Host-based routing | Dispatch on `Host` header (virtual hosting) |
| Method-based cascade | Pass through on additional method mismatches |
| Async sub-apps | Sub-apps that return futures instead of immediate responses |
| Weighted cascade | Prioritise sub-apps by weight, not just order |

---

## API Design

### `url-map!`

```turmeric
;;; url-map! -- mount sub-apps at path prefixes.
;;;
;;; Parameters:
;;;   & entries :Mapping -- alternating prefix :cstr / handler :int pairs
;;;
;;; Returns:
;;;   A tourist-compatible handler :int that dispatches by longest prefix.
;;;
;;; Example:
;;;   (url-map!
;;;     "/api"    api-app
;;;     "/admin"  admin-app
;;;     "/"       public-app)
(url-map! & entries :Mapping)  ;; => :int (handler)
```

Matching rules:
- Prefixes are compared longest-first (independent of registration order)
- `"/"` is a catch-all fallback that matches every path
- An unmatched request (no prefix and no `"/"` entry) yields a 404
- The matched prefix is stripped before forwarding: a request for `/api/users`
  passed to a sub-app mounted at `/api` arrives as `/users`
- `(req-path ctx)` inside the sub-app returns the stripped path; a new
  `(req-full-path ctx)` accessor returns the original unstripped path

```turmeric
;; Typical usage
(import tourist/app      :refer [tourist])
(import tourist/dsl      :refer [get!])
(import tourist/routing  :refer [url-map!])
(import tourist/helpers  :refer [text])

(defn api-routes [] : int
  (url-map!
    "/users" (get! "/" (fn [ctx] (text "users list")))
    "/items" (get! "/" (fn [ctx] (text "items list")))))

(defn main [] : int
  (let [s (tourist 3000
            (url-map!
              "/api" (api-routes)
              "/"    (get! "/" (fn [ctx] (text "home")))))]
    (server-stop s)
    0))
```

```sweet-exp
import tourist/app      :refer [tourist]
import tourist/dsl      :refer [get!]
import tourist/routing  :refer [url-map!]
import tourist/helpers  :refer [text]

defn api-routes [] : int
  url-map!
    "/users" get! "/" fn [ctx] text("users list")
    "/items" get! "/" fn [ctx] text("items list")

defn main [] : int
  let [s tourist(3000
           url-map!
             "/api" api-routes()
             "/"    get! "/" fn [ctx] text("home"))]
    server-stop(s)
    0
```

### `cascade!`

```turmeric
;;; cascade! -- try apps in order, falling through on configurable statuses.
;;;
;;; Parameters:
;;;   & apps :int -- handler apps to try in order
;;;
;;; Returns:
;;;   A tourist-compatible handler :int.
;;;
;;; Example:
;;;   (cascade! dynamic-app static-file-app)
(cascade! & apps :int)  ;; => :int (handler)

;;; cascade-with! -- cascade with a custom pass-through status set.
;;;
;;; Parameters:
;;;   pass-statuses :int -- cons list of :int status codes to pass through
;;;   & apps        :int -- handler apps to try in order
;;;
;;; Example:
;;;   (cascade-with! (list 404 405) api-v2 api-v1)
(cascade-with! pass-statuses & apps :int)  ;; => :int (handler)
```

```turmeric
;; Serve dynamic routes first, fall back to static files on 404
(import tourist/routing :refer [cascade!])
(import tourist/static  :refer [serve-static!])

(tourist 3000
  (cascade!
    (get! "/about" (fn [ctx] (html "<h1>About</h1>")))
    (serve-static! "/" "./public")))
```

```sweet-exp
import tourist/routing :refer [cascade!]
import tourist/static  :refer [serve-static!]

tourist 3000
  cascade!
    get! "/about" fn [ctx] html("<h1>About</h1>")
    serve-static! "/" "./public"
```

### `req-full-path`

```turmeric
;;; req-full-path -- original request path before any prefix stripping.
;;;
;;; Inside a url-map! sub-app, req-path returns the stripped path.
;;; req-full-path always returns the original path as received by the server.
;;;
;;; Example:
;;;   ;; Request: GET /api/users
;;;   ;; Sub-app mounted at /api
;;;   (req-path      ctx)  ;; => "/users"
;;;   (req-full-path ctx)  ;; => "/api/users"
(req-full-path ctx)  ;; => :cstr
```

---

## Architecture

```
tourist/routing
  |
  +-- url-map!          -- builds a dispatch table (prefix -> handler)
  |     |                  sorted longest-first at construction time
  |     +-- url-map-dispatch -- walks table, strips prefix, calls sub-handler
  |
  +-- cascade!          -- wraps apps in a try-next chain
        |
        +-- cascade-dispatch -- runs app, checks status, falls through or returns
```

The tourist context `ctx` carries two path fields after this change:

| Field | Set by | Read by |
|-------|--------|---------|
| `ctx-path` | request parser (unchanged) | `req-path` (returns `ctx-stripped-path` if set, else `ctx-path`) |
| `ctx-stripped-path` | `url-map-dispatch` | `req-path` |
| `ctx-full-path` | `url-map-dispatch` (copies `ctx-path` before stripping) | `req-full-path` |

The context struct change is **backwards-compatible**: existing handlers that
call `req-path` continue to work because when `ctx-stripped-path` is unset,
`req-path` falls back to `ctx-path`.

---

## Spice Layout

```
turmeric-spices/spices/tourist/
  src/tourist/
    routing.tur        -- url-map!, cascade!, cascade-with!, req-full-path
    context.tur        -- add ctx-stripped-path and ctx-full-path fields
    request.tur        -- update req-path to prefer ctx-stripped-path
  tests/tourist/
    routing_test.tur   -- url-map matching, prefix stripping, cascade fallthrough
```

Changes touch only `routing.tur` (new), `context.tur` (two new fields),
and `request.tur` (one-line guard in `req-path`).

---

## Implementation Phases

- [ ] **TR0** -- Extend tourist context struct with `stripped-path` and
  `full-path` fields (both defaulting to 0/nil). Update `req-path` to return
  `stripped-path` when non-nil, `full-path` to always return original path.
  No behaviour change for existing apps.

- [ ] **TR1** -- `tourist/routing`: `url-map!` constructor; sorts entries
  longest-prefix-first at build time; `url-map-dispatch` strips prefix, stores
  `full-path`, calls the matched sub-handler. Return 404 when no prefix matches
  and there is no `"/"` catch-all.

- [ ] **TR2** -- `tourist/routing`: `cascade!` and `cascade-with!`; runs apps
  in order; checks response status against the pass-through set; returns the
  first non-pass-through response or the last response if all pass through.

- [ ] **TR3** -- Nested composition tests: `url-map!` inside `url-map!`,
  `cascade!` of `url-map!` apps, `url-map!` with `use!` middleware at multiple
  levels.

- [ ] **TR4** -- Documentation: update `docs/guides/web-stack-guide.md` with a
  "Routing Composition" section; add `docs/guides/tourist-routing-guide.md`
  with full examples including multi-app splitting and static fallback patterns.

---

## Design Notes

### Longest-prefix matching vs. registration order

Rack::URLMap uses longest-prefix matching. Registration order is irrelevant to
dispatch; the programmer can list prefixes in any order. This is the safer
default -- it avoids bugs where a short prefix like `"/"` accidentally swallows
requests meant for `"/api"`.

`url-map!` sorts at construction time (one allocation), so per-request
matching is a linear scan of a sorted cons list, not a sort. For typical
application sizes (under 50 mounted apps) this is fast enough.

### Path stripping and `redirect`

When a sub-app issues a `(redirect "/login")` the path is relative to the
sub-app's own root. `url-map!` does **not** rewrite redirect `Location` headers
-- this mirrors Rack's behaviour. If a sub-app must generate absolute URLs, it
should call `(req-full-path ctx)` to determine the prefix and construct the URL
manually, or use a base-url config value.

### Pass-through status set in `cascade!`

Rack hard-codes 404 and 405 as the pass-through statuses. `cascade!` defaults
to `{404 405}` but `cascade-with!` lets callers customise this, enabling
patterns like:

```turmeric
;; Also pass through 503 (maintenance mode of first app)
(cascade-with! (list 404 405 503) primary fallback)
```

### Middleware ordering with `url-map!`

`use!` middleware registered *before* `url-map!` runs before path dispatch:

```turmeric
(tourist 3000
  (use! auth-mw)      ;; runs first, for all paths
  (url-map!
    "/api"  api-app   ;; auth-mw already ran
    "/"     public-app))
```

Middleware registered *inside* a sub-app mounted via `url-map!` runs only for
that sub-app's paths. This matches Rack's behaviour and is the natural
consequence of tourist's function-composition middleware model.

---

## Risks and Open Questions

1. **Context struct versioning.** Adding fields to the tourist context struct is
   source-compatible but requires a coordinated release of `context.tur` and
   `request.tur`. Any spice that captures a raw `ctx` field index will break.
   Audit the existing field list before TR0.

2. **Path stripping and absolute redirects.** If a sub-app is unaware it has
   been stripped, redirects to absolute paths will be wrong. Document the
   `req-full-path` accessor prominently.

3. **Cascade and stateful middleware.** If a middleware in app A has side
   effects (e.g., opening a DB transaction), and the cascade falls through to
   app B, the side effects are not rolled back. Document that cascade apps
   should keep middleware side-effect-free, or use a finalizer pattern.

4. **Sorting overhead for large mount tables.** Sorting at construction time is
   O(n log n) on mount count. For 1000+ mounts this may be noticeable at
   startup. If needed, replace the cons list with a sorted array (phase TR1+).

---

## Files to Change

| File | Change |
|------|--------|
| `turmeric-spices/spices/tourist/src/tourist/context.tur` | Add `stripped-path`, `full-path` fields |
| `turmeric-spices/spices/tourist/src/tourist/request.tur` | Update `req-path`; add `req-full-path` |
| `turmeric-spices/spices/tourist/src/tourist/routing.tur` | New -- `url-map!`, `cascade!`, `cascade-with!` |
| `turmeric-spices/spices/tourist/tests/tourist/routing_test.tur` | New -- test suite |
| `docs/guides/web-stack-guide.md` | Add routing composition section |
| `docs/guides/tourist-routing-guide.md` | New -- full routing composition guide |

---

## Phase Status

| Phase | Title | Status |
|-------|-------|--------|
| TR0 | Context struct extension | Done |
| TR1 | `url-map!` | Done |
| TR2 | `cascade!` and `cascade-with!` | Done |
| TR3 | Nested composition tests | Done |
| TR4 | Documentation | Done |

## Implementation notes (post-build)

- The plan envisioned new `tourist/context.tur` and `tourist/request.tur`
  modules. The actual tourist layout keeps the ctx in `tourist/param.tur`
  (its first 8 fields shadow `__httpd_req` so the httpd accessors --
  including `req-path` -- work unchanged on a ctx handle). TR0 instead
  added a `full_path` field to `__tourist_ctx` and a
  `tourist-ctx-strip-path` mutator; the existing `req-path` from
  `httpd/request` automatically sees the stripped value because we
  swap `ctx->path` in place.
- The plan's `(url-map! "/api" handler "/" handler)` form mixes `:cstr`
  and `:int` in a variadic rest, which Turmeric's single-typed rest
  rejects. The implementation introduces a small `mount!` helper that
  boxes the pair into a single `:int` handle, so `url-map!` is declared
  `& mounts :int`.
- Tests live in the existing `tests/fixtures/<name>/` convention used by
  the rest of the tourist suite (no `tests/tourist/` directory was
  introduced).
