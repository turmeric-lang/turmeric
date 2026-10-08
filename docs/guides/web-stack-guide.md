---
title: Web Stack Guide
category: Networking and Web
description: Building HTTP servers with tur-httpd, tur-template, and tur-tourist -- the composable web stack for Turmeric
---

# Web Stack Guide

Turmeric's web stack is three composable spices:

| Spice | Analogue | Purpose |
|-------|----------|---------|
| `tur-template` | ERB / EJS | String templating engine |
| `tur-httpd` | Civetweb / Mongoose | Threaded HTTP/1.1 server |
| `tur-tourist` | Haskell's scotty | Routing + middleware micro-framework |

Each layer can be used independently. `tur-tourist` builds on both of the
others; `tur-httpd` and `tur-template` have no shared dependency.

---

## Installation

### `tur-httpd` only

```turmeric no-check
:spices #map{
  "httpd" #map{:url    "https://github.com/turmeric-lang/turmeric-spices"
               :ref    "httpd-v0.1.0"
               :subdir "spices/httpd"}
}
```
```sweet-exp
:spices
#map{
  "httpd" #map{:url    "https://github.com/turmeric-lang/turmeric-spices"
               :ref    "httpd-v0.1.0"
               :subdir "spices/httpd"}
}
```

### Full stack (httpd + template + tourist)

```turmeric no-check
:spices #map{
  "httpd"    #map{:url    "https://github.com/turmeric-lang/turmeric-spices"
                  :ref    "httpd-v0.1.0"
                  :subdir "spices/httpd"}
  "template" #map{:url    "https://github.com/turmeric-lang/turmeric-spices"
                  :ref    "template-v0.1.0"
                  :subdir "spices/template"}
  "tourist"  #map{:url    "https://github.com/turmeric-lang/turmeric-spices"
                  :ref    "tourist-v0.1.0"
                  :subdir "spices/tourist"}
}
```
```sweet-exp
:spices
#map{
  "httpd"    #map{:url    "https://github.com/turmeric-lang/turmeric-spices"
                  :ref    "httpd-v0.1.0"
                  :subdir "spices/httpd"}
  "template" #map{:url    "https://github.com/turmeric-lang/turmeric-spices"
                  :ref    "template-v0.1.0"
                  :subdir "spices/template"}
  "tourist"  #map{:url    "https://github.com/turmeric-lang/turmeric-spices"
                  :ref    "tourist-v0.1.0"
                  :subdir "spices/tourist"}
}
```

---

## `tur-httpd` -- Raw HTTP server

`tur-httpd` is the foundation: a minimal POSIX-socket HTTP/1.1 server with a
bounded thread pool. A handler is a plain Turmeric function that receives a
request handle and returns a response handle.

### Modules

| Module | Contents |
|--------|----------|
| `httpd/server` | `server-start`, `server-start-pool`, `server-stop` |
| `httpd/request` | `req-method`, `req-path`, `req-query`, `req-body`, `req-header` |
| `httpd/response` | `resp-ok`, `with-header`, `with-body` |

### Quick start

```turmeric
(import httpd/server   :refer [server-start server-stop])
(import httpd/request  :refer [req-path])
(import httpd/response :refer [resp-ok])

(defn handler [req : int] : int
  (resp-ok "text/plain"
    (str-concat "You requested: " (req-path req))))

(defn main [] : int
  (let [s (server-start 8080 handler)]
    ;; run until killed
    (server-stop s)
    0))
```
```sweet-exp
import httpd/server :refer [server-start server-stop]
import httpd/request :refer [req-path]
import httpd/response :refer [resp-ok]
defn handler [req :int] :int
  resp-ok("text/plain" str-concat("You requested: " req-path(req)))
defn main [] :int
  let [s (server-start 8080 handler)]
    ;; run until killed
    server-stop(s)
    0
```

### Starting the server

```turmeric
;; Bounded thread pool (default -- recommended)
(server-start      port handler)
(server-start-pool port handler pool-size)

;; Thread-per-connection (legacy)
(server-start-spawn port handler)
```
```sweet-exp
;; Bounded thread pool (default -- recommended)
server-start(port handler)
server-start-pool(port handler pool-size)
;; Thread-per-connection (legacy)
server-start-spawn(port handler)
```

`server-start` returns a server handle. Pass it to `server-stop` to close
the listening socket and join the pool threads.

### Request accessors

```turmeric
(req-method  req)       ;; "GET", "POST", "PUT", "DELETE", ...
(req-path    req)       ;; "/users/42"
(req-query   req)       ;; "page=1&limit=10"  (raw query string)
(req-body    req)       ;; request body as cstr (may be "")
(req-header  req "x-api-key")  ;; value of a header, or 0 if absent
```
```sweet-exp
req-method(req)
;; "GET", "POST", "PUT", "DELETE", ...
req-path(req)
;; "/users/42"
req-query(req)
;; "page=1&limit=10"  (raw query string)
req-body(req)
;; request body as cstr (may be "")
req-header(req "x-api-key")
;; value of a header, or 0 if absent
```

### Response constructors

```turmeric
;; Convenience: status 200 with a content-type and body
(resp-ok "text/html"  "<h1>Hello</h1>")
(resp-ok "text/plain" "OK")
(resp-ok "application/json" "{\"ok\":true}")

;; Custom status
(with-header (resp-ok "text/plain" "Not found") "x-reason" "missing")
```
```sweet-exp
;; Convenience: status 200 with a content-type and body
resp-ok("text/html" "<h1>Hello</h1>")
resp-ok("text/plain" "OK")
resp-ok("application/json" "{\"ok\":true}")
;; Custom status
with-header(resp-ok("text/plain" "Not found") "x-reason" "missing")
```

---

## `tur-template` -- ERB/EJS-style templating

`tur-template` renders strings from templates that embed `<%= expr %>` and
`<% for var in list %>` / `<% end %>` constructs.

### Modules

| Module | Contents |
|--------|----------|
| `template/render` | `render` (lex+parse+render in one call) |
| `template/env` | `env-new`, `env-set`, `env-set-list`, `env-free` |

### Quick start

```turmeric
(import template/render :refer [render])
(import template/env    :refer [env-new env-set env-free])

(defn greet [name : cstr] : String
  (let [e (env-new)]
    (env-set e "name" name)
    (let [out (render "Hello, <%= name %>!" e)]
      (env-free e)
      (string/adopt-cstr out))))  ;; owned result -- adopt render's heap buffer
```
```sweet-exp
import template/render :refer [render]
import template/env :refer [env-new env-set env-free]
defn greet [name :cstr] :String
  let [e (env-new)]
    env-set(e "name" name)
    let [out (render "Hello, <%= name %>!" e)]
      env-free(e)
      string/adopt-cstr(out)  ;; owned result -- adopt render's heap buffer
```

`render` returns a heap-allocated `cstr` that the caller must free -- exactly the
manual-free footgun the owned `String` type removes. Returning `String` (adopt
`render`'s buffer with `string/adopt-cstr`) hands the caller an owned value it
releases once with `string/release`, with no raw `free` to forget. See
[strings-guide.md](strings-guide.md) for `cstr` vs `str` vs `String`.

### Template syntax

| Syntax | Meaning |
|--------|---------|
| `<%= var %>` | Substitute the value of `var` |
| `<% for item in list %>` ... `<% end %>` | Repeat for each item in a list variable |
| `<%# comment %>` | Comment (not rendered) |
| `<%%` | Literal `<%` in output |

### Env API

```turmeric
(env-new)                           ;; create an Env handle
(env-set  env "key" "value")        ;; bind a scalar string
(env-set-list env "items" lst)      ;; bind a cons list of cstr values
(env-free env)                      ;; release the Env
```
```sweet-exp
env-new()                           ;; create an Env handle
env-set(env "key" "value")         ;; bind a scalar string
env-set-list(env "items" lst)      ;; bind a cons list of cstr values
env-free(env)                      ;; release the Env
```

### Rendering from a file

```turmeric
(import template/render :refer [render-ast])
(import template/token  :refer [lex])
(import template/parse  :refer [parse])

(let [src  (slurp "views/index.html")   ;; read file as cstr
      toks (lex src)
      ast  (parse toks)
      e    (env-new)]
  (env-set e "title" "Home")
  (render-ast ast e))
```
```sweet-exp
import template/render :refer [render-ast]
import template/token :refer [lex]
import template/parse :refer [parse]
let [src  (slurp "views/index.html")   ;; read file as cstr
      toks (lex src)
      ast  (parse toks)
      e    (env-new)]
  env-set(e "title" "Home")
  render-ast(ast e)
```

---

## `tur-tourist` -- Routing micro-framework

`tur-tourist` layers routing, response helpers, middleware, and static file
serving on top of `tur-httpd`.

### Modules

| Module | Contents |
|--------|----------|
| `tourist/app` | `tourist` -- start the server with routes + middleware |
| `tourist/dsl` | `get!`, `post!`, `put!`, `delete!`, `any!` -- route constructors |
| `tourist/helpers` | `text`, `html`, `json-body`, `redirect`, `status` |
| `tourist/param` | `capture`, `param` -- URL captures and query params |
| `tourist/middleware` | `use!` -- middleware constructor |
| `tourist/static` | `serve-static!` -- static file serving |

### Quick start

```turmeric
(import tourist/app     :refer [tourist])
(import tourist/dsl     :refer [get! post!])
(import tourist/helpers :refer [text html])
(import tourist/param   :refer [capture param])

(defn main [] : int
  (let [s (tourist 3000
            (get! "/hello/:name"
              (fn [ctx]
                (text (str-concat "Hello, " (ok-val (capture ctx "name")) "!"))))
            (get! "/search"
              (fn [ctx]
                (let [q (ok-val (param ctx "q"))]
                  (text (str-concat "Searching for: " q)))))
            (post! "/echo"
              (fn [ctx]
                (text (req-body ctx)))))]
    (server-stop s)
    0))
```
```sweet-exp
import tourist/app :refer [tourist]
import tourist/dsl :refer [get! post!]
import tourist/helpers :refer [text html]
import tourist/param :refer [capture param]
defn main [] :int
  let [s (tourist 3000
            (get! "/hello/:name"
              (fn [ctx]
                (text (str-concat "Hello, " (ok-val (capture ctx "name")) "!"))))
            (get! "/search"
              (fn [ctx]
                (let [q (ok-val (param ctx "q"))]
                  (text (str-concat "Searching for: " q)))))
            (post! "/echo"
              (fn [ctx]
                (text (req-body ctx)))))]
    server-stop(s)
    0
```

### Route constructors

```turmeric
(get!    pattern handler)    ;; match GET  requests
(post!   pattern handler)    ;; match POST requests
(put!    pattern handler)    ;; match PUT  requests
(delete! pattern handler)    ;; match DELETE requests
(any!    pattern handler)    ;; match any method
```
```sweet-exp
get!(pattern handler)
;; match GET  requests
post!(pattern handler)
;; match POST requests
put!(pattern handler)
;; match PUT  requests
delete!(pattern handler)
;; match DELETE requests
any!(pattern handler)
;; match any method
```

Pattern syntax:
- `"/exact"` -- literal match
- `"/user/:id"` -- `:id` captures one path segment
- `"/files/*"` -- `*` captures the rest of the path (greedy)

### Request context

Handler functions receive a *tourist context* `ctx`, not a raw httpd request.
The context transparently forwards all `httpd/request` accessors:

```turmeric
(req-method ctx)         ;; "GET"
(req-path   ctx)         ;; "/user/42"
(req-body   ctx)         ;; request body
(req-header ctx "name")  ;; header value
```
```sweet-exp
req-method(ctx)
;; "GET"
req-path(ctx)
;; "/user/42"
req-body(ctx)
;; request body
req-header(ctx "name")
;; header value
```

Plus tourist-specific helpers:

```turmeric
(capture ctx "id")       ;; URL segment capture -- result<:cstr>
(param   ctx "page")     ;; query parameter    -- result<:cstr>
```
```sweet-exp
capture(ctx "id")
;; URL segment capture -- result<:cstr>
param(ctx "page")
;; query parameter    -- result<:cstr>
```

Both return `result<:cstr>`. Unwrap with `ok-val` after checking for `err`:

```turmeric
(let [res (capture ctx "id")]
  (if (ok? res)
    (text (ok-val res))
    (status 400 (text "missing id"))))
```
```sweet-exp
let [res (capture ctx "id")]
  if ok?(res)
    text(ok-val(res))
    status(400 text("missing id"))
```

### Response helpers

```turmeric
(text body)              ;; 200 text/plain
(html body)              ;; 200 text/html
(json-body body)         ;; 200 application/json
(redirect path)          ;; 302 Location: path
(status code resp)       ;; override the status code of any response
```
```sweet-exp
text(body)
;; 200 text/plain
html(body)
;; 200 text/html
json-body(body)
;; 200 application/json
redirect(path)
;; 302 Location: path
status(code resp)
;; override the status code of any response
```

### Middleware

`use!` items run in declaration order before any route handler. A middleware
function receives a request and returns `option<:int>`:
- `none-value` -- continue to the next middleware / route
- `some(resp)` -- short-circuit and return this response immediately

```turmeric
(import tourist/middleware :refer [use!])
(import stdlib/option      :refer [none-value some])

(defn auth-mw [ctx : int] : int
  (if (= (req-header ctx "x-api-key") 0)
    (some (status 401 (text "Unauthorized")))
    (none-value)))

(tourist 3000
  (use! auth-mw)
  (get! "/private" (fn [ctx] (text "secret"))))
```
```sweet-exp
import tourist/middleware :refer [use!]
import stdlib/option :refer [none-value some]
defn auth-mw [ctx :int] :int
  if {req-header(ctx "x-api-key") = 0}
    some(status(401 text("Unauthorized")))
    none-value()
tourist(3000 use!(auth-mw) get!("/private" (fn [ctx] text("secret"))))
```

### Static file serving

```turmeric
(import tourist/static :refer [serve-static!])

(tourist 3000
  (serve-static! "/assets" "./public")
  (get! "/" (fn [ctx] (html "<h1>Hello</h1>"))))
```
```sweet-exp
import tourist/static :refer [serve-static!]
tourist(3000 serve-static!("/assets" "./public") get!("/" (fn [ctx] html("<h1>Hello</h1>"))))
```

`serve-static!` matches `GET /assets/**` paths and serves files from
`./public/` on disk. It rejects path-traversal attempts (`..`).

### Routing composition

`tourist/routing` provides two Rack-inspired combinators for composing
larger applications out of independently-defined sub-apps:

| Combinator       | Behaviour |
|------------------|-----------|
| `url-map!`       | Mount sub-apps at path prefixes; longest-prefix-first |
| `cascade!`       | Try apps in order; fall through on 404/405 |
| `cascade-with!`  | Cascade with an explicit pass-through status list |

A `mount!` helper boxes a `(prefix, sub-app)` pair into a single handle so
`url-map!` can accept them through Turmeric's single-typed variadic rest.

```turmeric
(import tourist/routing :refer [url-map! cascade! mount!])

(defn api-routes [] : int
  (url-map! (mount! "/users" (get! "/" users-handler))
            (mount! "/items" (get! "/" items-handler))))

(tourist 3000
  (cascade! (url-map! (mount! "/api" (api-routes))
                      (mount! "/"    (get! "/" home-handler)))
            (serve-static! "/" "./public")))
```
```sweet-exp
import tourist/routing :refer [url-map! cascade! mount!]

defn api-routes [] : int
  url-map!
    mount!("/users" get!("/" users-handler))
    mount!("/items" get!("/" items-handler))

tourist 3000
  cascade!
    url-map!
      mount!("/api" api-routes())
      mount!("/"    get!("/" home-handler))
    serve-static!("/" "./public")
```

`url-map!` strips the matched prefix from `ctx->path` before dispatching
the inner sub-app, so handlers see paths relative to their mount point
(e.g. a request for `/api/users` arrives at the `/users` sub-app as `/`).
The original path is preserved on the ctx and is available via
`req-full-path` from `tourist/param`. `cascade!` frees pass-through
responses (default 404 and 405) and tries the next app; the first non-pass
response wins.

See [tourist-routing-guide.md](tourist-routing-guide.md) for the full
composition guide, semantics, and patterns.

---

## Template rendering in tourist handlers

Using `tur-template` with `tur-tourist`:

```turmeric
(import tourist/app      :refer [tourist])
(import tourist/dsl      :refer [get!])
(import tourist/helpers  :refer [html])
(import template/render  :refer [render])
(import template/env     :refer [env-new env-set env-free])

(defn render-view [name : cstr env : int] : cstr
  (let [out (render name env)]
    (env-free env)
    out))

(defn main [] : int
  (let [s (tourist 3000
            (get! "/hello/:name"
              (fn [ctx]
                (let [e (env-new)
                      n (ok-val (capture ctx "name"))]
                  (env-set e "name" n)
                  (html (render-view "Hello, <%= name %>!" e))))))]
    (server-stop s)
    0))
```
```sweet-exp
import tourist/app :refer [tourist]
import tourist/dsl :refer [get!]
import tourist/helpers :refer [html]
import template/render :refer [render]
import template/env :refer [env-new env-set env-free]
defn render-view [name :cstr env :int] :cstr
  let [out (render name env)]
    env-free(env)
    out
defn main [] :int
  let [s (tourist 3000
            (get! "/hello/:name"
              (fn [ctx]
                (let [e (env-new)
                      n (ok-val (capture ctx "name"))]
                  (env-set e "name" n)
                  (html (render-view "Hello, <%= name %>!" e))))))]
    server-stop(s)
    0
```

`render-view` returns `render`'s heap buffer past `env-free`, so the caller owns
those bytes. As with `greet` above, you can make that ownership explicit by
returning an owned `String` (`(string/adopt-cstr (render name env))`) and having
the handler borrow it back with `string/to-cstr` for `html`. See
[strings-guide.md](strings-guide.md).

---

## WebSocket endpoints

`tourist`'s one-arg handler shape has no `Conn` to upgrade, but the
`tourist-ws` spice adds `ws-route!`, a route constructor that upgrades to
a WebSocket and shares the item list and middleware chain with `get!` /
`post!` / `use!` -- served via `tourist-conn` rather than `tourist`. See
the [WebSockets guide](websocket-guide.md#composing-with-httpd--tourist)
for the full pattern.

## See also

- [Threading Guide](threading-guide.md) -- OS threads, Mutex, Arc (used by tur-httpd internally)
- [Error Handling Guide](error-handling-guide.md) -- `result<:cstr>` unwrapping patterns
- [WebSockets Guide](websocket-guide.md) -- `ws-client` / `ws-server` / `tourist-ws`
