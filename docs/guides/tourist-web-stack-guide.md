---
title: Tourist Web Stack End-to-End
category: Networking and Web
description: Build a complete web application with tourist routing, httpd, TLS, sessions, and template rendering -- the full stack wired together
audience: developers building a production web application with Turmeric
since: tourist v0.2.5, tourist-session v0.1.0, tls v0.1.0, template v0.2.0
tags:
  - tourist
  - httpd
---

# Tourist Web Stack End-to-End

Turmeric's web stack is four composable spices:

| Spice | Role |
|-------|------|
| `tur-httpd` | Threaded HTTP/1.1 server |
| `tur-tls` | TLS termination (mbedTLS) |
| `tur-tourist` | Routing + middleware micro-framework |
| `tur-tourist-session` | Cookie-backed sessions with pluggable stores |
| `tur-template` | ERB/EJS-style string templating |

Each layer can be used independently. This guide wires them all together
into a single application: HTTPS server, routed handlers, session-based
authentication, and template-rendered views.

The individual guides cover each layer in depth:
[web-stack-guide](web-stack-guide.md),
[httpd-tls-guide](httpd-tls-guide.md),
[tourist-routing-guide](tourist-routing-guide.md),
[tourist-session-guide](tourist-session-guide.md).

---

## Installation

```turmeric no-check
:spices #map{
  "httpd"           #map{:url    "https://github.com/turmeric-lang/turmeric-spices"
                         :ref    "httpd-v0.1.0"
                         :subdir "spices/httpd"}
  "tls"             #map{:url    "https://github.com/turmeric-lang/turmeric-spices"
                         :ref    "tls-v0.1.0"
                         :subdir "spices/tls"}
  "tourist"          #map{:url    "https://github.com/turmeric-lang/turmeric-spices"
                         :ref    "tourist-v0.2.5"
                         :subdir "spices/tourist"}
  "tourist-session"  #map{:url    "https://github.com/turmeric-lang/turmeric-spices"
                         :ref    "tourist-session-v0.1.0"
                         :subdir "spices/tourist-session"}
  "template"         #map{:url    "https://github.com/turmeric-lang/turmeric-spices"
                         :ref    "template-v0.2.0"
                         :subdir "spices/template"}
}
```

Then `tur fetch`.

---

## The application

A simple authenticated web app with three pages:

- `/` -- public home page (template-rendered)
- `/login` -- POST sets a session, redirects to `/dashboard`
- `/dashboard` -- session-gated, shows the user's name
- `/logout` -- destroys the session, redirects to `/`

---

## 1. TLS setup

Generate a self-signed certificate for development:

```sh
../turmeric-spices/spices/tls/tools/gen-cert.sh /tmp/tls-smoke-test
```

Then load it in the application:

```turmeric
(import tls/ctx   :refer [tls-ctx-new tls-ctx-free
                           tls-ctx-load-cert-pem tls-ctx-load-key-pem])
(import tls/httpd :refer [tls-httpd-init])

(defn setup-tls [] : int
  (tls-httpd-init)
  (let [ctx (tls-ctx-new)]
    (tls-ctx-load-cert-pem ctx "/tmp/tls-smoke-test/test-cert.pem")
    (tls-ctx-load-key-pem  ctx "/tmp/tls-smoke-test/test-key.pem")
    ctx))
```

`tls-httpd-init` registers the spice's TLS functions with stdlib/httpd.
Call it once before starting the server. The `ctx` handle is passed to
`httpd-new-tls` (or threaded through tourist) to enable HTTPS.

For production, use Let's Encrypt certificates:
`/etc/letsencrypt/live/example.com/fullchain.pem` and `privkey.pem`. See
the [TLS guide](httpd-tls-guide.md) for the full details.

---

## 2. Session middleware

```turmeric
(import session/mw           :refer [session-mw])
(import session/config       :refer [default-session-config])
(import session/memory-store :refer [memory-store-new])

(let [store (memory-store-new)
      cfg   (default-session-config)]
  (session-mw store cfg))
```

`session-mw` returns a tourist after-middleware item. Place it directly in
the tourist item list -- it is already a middleware handle, not a value
you wrap in `use!`.

For production, swap the memory store for a file store:

```turmeric
(import session/file-store :refer [file-store-new])

(let [store (file-store-new "/var/lib/myapp/sessions")]
  (session-mw store (default-session-config)))
```

The `default-session-config` sets `Secure` on, `HttpOnly` on, 24-hour TTL,
`SameSite=Lax`. For development over plain HTTP, use `dev-session-config`
which sets `Secure` off so the cookie survives `http://`.

---

## 3. Template rendering

```turmeric
(import template/render :refer [render render-file])
(import template/env    :refer [env-new env-set env-set-list env-free])

(defn render-page [template-path : cstr title : cstr user : cstr] : cstr
  (let [e (env-new)]
    (env-set e "title" title)
    (env-set e "user"  user)
    (let [r (render-file template-path e)]
      (env-free e)
      (if (ok? r)
        (ok-val r)
        "render error"))))
```

`render-file` returns `(Result cstr cstr)` -- `ok` is a heap-allocated
string the caller frees, `err` is a static diagnostic. The template
engine supports `<%= var %>` interpolation, `<% if cond %>...<% else
%>...<% end %>` conditionals, `<% for [x list] %>...<% end %>` loops, and
`<% let x val %>...<% end %>` bindings.

A simple view template (`views/home.html.tur`):

```html
<h1><%= title %></h1>
<% if user %>
  <p>Welcome back, <%= user %>. <a href="/logout">Logout</a></p>
<% else %>
  <p><a href="/login">Login</a></p>
<% end %>
```

---

## 4. Route handlers

```turmeric
(import tourist/dsl     :refer [get! post!])
(import tourist/helpers :refer [html redirect status text])
(import tourist/param   :refer [capture param])
(import session/ctx     :refer [session-get session-set! session-destroy!
                                session-id])
(import httpd/request   :refer [req-body])

;; Home page -- public, template-rendered
(defn home-handler [ctx : int] : int
  (let [user-res (session-get ctx "user")
        user     (if (ok? user-res) (ok-val user-res) "")]
    (html (render-page "views/home.html.tur" "Home" user))))

;; Login form (GET) and login action (POST)
(defn login-get [ctx : int] : int
  (html "<form method='post'><input name='username'><button>Login</button></form>"))

(defn login-post [ctx : int] : int
  (let [username (ok-val (param ctx "username"))]
    (if (= username 0)
      (status 400 (text "missing username"))
      (do
        (session-set! ctx "user" username)
        (redirect "/dashboard")))))

;; Dashboard -- session-gated
(defn dashboard-handler [ctx : int] : int
  (let [user-res (session-get ctx "user")]
    (if (ok? user-res)
      (html (render-page "views/dashboard.html.tur" "Dashboard" (ok-val user-res)))
      (redirect "/login"))))

;; Logout -- destroy session
(defn logout-handler [ctx : int] : int
  (session-destroy! ctx)
  (redirect "/"))
```

### Session helpers

| Call | Returns | Notes |
|------|---------|-------|
| `(session-get ctx key)` | `(Result cstr cstr)` | `ok` value or `err "missing"` |
| `(session-set! ctx key val)` | `:void` | Marks session dirty; triggers save + Set-Cookie |
| `(session-del! ctx key)` | `:void` | Marks dirty |
| `(session-destroy! ctx)` | `:void` | Deletes session, emits expiring Set-Cookie |
| `(session-id ctx)` | `:cstr` | 64-char hex session ID (borrowed) |

A read-only `session-get` does not dirty the session, so a request that
only reads generates no store write and no `Set-Cookie` (unless
`rolling?` is on).

---

## 5. Auth middleware

A `use!` middleware that gates access to protected routes:

```turmeric
(import tourist/middleware :refer [use!])
(import stdlib/option      :refer [none-value some])

(defn require-auth [ctx : int] : int
  (let [user-res (session-get ctx "user")]
    (if (ok? user-res)
      (none-value)                              ;; continue to route
      (some (redirect "/login")))))             ;; short-circuit to login
```

`use!` items run in declaration order before any route handler. A
middleware function receives the context and returns `option<int>`:
`none-value` continues, `some(resp)` short-circuits.

---

## 6. Wiring it all together

```turmeric
(import tourist/app :refer [tourist])
(import tourist/dsl :refer [get! post!])
(import tourist/middleware :refer [use!])
(import tourist/static :refer [serve-static!])
(import tourist/routing :refer [url-map! mount!])
(import httpd/server :refer [server-stop])
(import tls/ctx   :refer [tls-ctx-free])
(import tls/httpd :refer [tls-httpd-init])
(import session/mw :refer [session-mw])
(import session/config :refer [default-session-config])
(import session/memory-store :refer [memory-store-new])

(defn main [] : int
  ;; TLS
  (tls-httpd-init)
  (let [tls-ctx (tls-ctx-new)]
    (tls-ctx-load-cert-pem tls-ctx "/tmp/tls-smoke-test/test-cert.pem")
    (tls-ctx-load-key-pem  tls-ctx "/tmp/tls-smoke-test/test-key.pem")

    (let [;; Sessions
          store   (memory-store-new)
          sess-mw (session-mw store (default-session-config))

          ;; Routes
          public-routes
            (url-map!
              (mount! "/" (get! "/" home-handler))
              (mount! "/login"
                (url-map!
                  (mount! "/" (get! "/" login-get)
                               (post! "/" login-post)))))

          protected-routes
            (url-map!
              (mount! "/dashboard" (get! "/" dashboard-handler))
              (mount! "/logout"   (get! "/" logout-handler)))

          ;; Server
          srv (tourist 8443
                sess-mw
                (serve-static! "/assets" "./public")
                (use! (fn [ctx]
                  (let [path (req-path ctx)]
                    ;; gate /dashboard and /logout behind auth
                    (if (or (str-prefix? path "/dashboard")
                            (str-prefix? path "/logout"))
                      (require-auth ctx)
                      (none-value)))))
                public-routes
                protected-routes)]
      ;; run until killed
      (server-stop srv)
      (tls-ctx-free tls-ctx)
      0)))
```

```sweet-exp
#lang sweet-exp
import tourist/app :refer [tourist]
import tourist/dsl :refer [get! post!]
import tourist/middleware :refer [use!]
import tourist/static :refer [serve-static!]
import tourist/routing :refer [url-map! mount!]
import httpd/server :refer [server-stop]
import tls/ctx :refer [tls-ctx-free]
import tls/httpd :refer [tls-httpd-init]
import session/mw :refer [session-mw]
import session/config :refer [default-session-config]
import session/memory-store :refer [memory-store-new]

defn main [] :int
  tls-httpd-init()
  let [tls-ctx tls-ctx-new()]
    tls-ctx-load-cert-pem(tls-ctx "/tmp/tls-smoke-test/test-cert.pem")
    tls-ctx-load-key-pem(tls-ctx "/tmp/tls-smoke-test/test-key.pem")
    let [store            memory-store-new()
         sess-mw          session-mw(store default-session-config())
         public-routes    url-map!(mount!("/" get!("/" home-handler))
                                   mount!("/login"
                                          url-map!(mount!("/" get!("/" login-get)
                                                              post!("/" login-post)))))
         protected-routes url-map!(mount!("/dashboard" get!("/" dashboard-handler))
                                   mount!("/logout" get!("/" logout-handler)))
         srv              tourist(8443
                                  sess-mw
                                  serve-static!("/assets" "./public")
                                  use!((fn [ctx]
                                         (let [path req-path(ctx)]
                                           (if or(str-prefix?(path "/dashboard")
                                                  str-prefix?(path "/logout"))
                                             require-auth(ctx)
                                             none-value()))))
                                  public-routes
                                  protected-routes)]
      server-stop(srv)
      tls-ctx-free(tls-ctx)
      0
```

### What the wiring does

1. **TLS init** -- `tls-httpd-init` registers the TLS ops with stdlib/httpd.
   The `tls-ctx` loads the cert and key.
2. **Session middleware** -- `session-mw` installs the store and cookie
   config. It is an after-middleware: it loads the session lazily on first
   `session-get`/`session-set!` and persists on the way out.
3. **Static files** -- `serve-static!` serves `/assets/**` from `./public/`,
   rejecting path traversal.
4. **Auth middleware** -- `use!` gates `/dashboard` and `/logout` behind
   `require-auth`, which checks the session and redirects to `/login` if
   absent.
5. **Route composition** -- `url-map!` + `mount!` split public and
   protected routes into sub-apps. The matched prefix is stripped before
   the inner sub-app runs, so handlers see paths relative to their mount
   point.

---

## 7. Production checklist

| Concern | Recommendation |
|---------|----------------|
| TLS certificates | Use Let's Encrypt; see [TLS guide](httpd-tls-guide.md) |
| Session store | File store for single-host; custom vtable for Redis/Valkey |
| Cookie config | `default-session-config`: Secure, HttpOnly, SameSite=Lax |
| Static files | `serve-static!` with a dedicated mount point |
| Thread pool | `tourist` uses `server-start-pool` internally; tune pool size |
| Graceful shutdown | `server-stop` closes the listening socket and joins threads |
| Session fixation | Destroy + re-create on privilege change (no `session-rotate!` yet) |

### What is not yet available

- **Certificate rotation at runtime** -- restart to pick up renewed certs.
- **ACME / OCSP stapling** -- not implemented in `tur-tls` v0.1.0.
- **Session ID rotation** -- `session-rotate!` is planned but not shipped.
- **WebSocket in the same server** -- use `tur-tourist-ws` with
  `ws-route!`; see the [WebSocket guide](websocket-guide.md).

---

## See also

- [Web Stack Guide](web-stack-guide.md) -- httpd + template + tourist basics
- [TLS Guide](httpd-tls-guide.md) -- HTTPS termination with tur-tls
- [Tourist Routing Guide](tourist-routing-guide.md) -- url-map! and cascade! composition
- [Tourist Session Guide](tourist-session-guide.md) -- session stores, cookie config, custom backends
- [WebSocket Guide](websocket-guide.md) -- ws-client, ws-server, tourist-ws
