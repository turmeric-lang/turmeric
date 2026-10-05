---
title: Web Continuations Tutorial
category: Tutorials
description: Multi-page web forms using serializable continuations (guestbook example)
---

# Web Continuations Tutorial -- Multi-Page Forms Without Session State

A **step-by-step guide** to building a multi-page web form application in Turmeric using serializable continuations. Each step introduces one new concept. By the end you will have a working guestbook server that captures, stores, and resumes continuations across HTTP round-trips.

> **Prerequisites**: Read [serializable-continuations-guide.md](serializable-continuations-guide.md) first. You should understand `serial-reset`, `serial-shift`, and `serial-resume` before starting.

> **Related guides**: [web-continuations-guide.md](web-continuations-guide.md) (the compact reference), [c-integration-guide.md](c-integration-guide.md), [checkpointing-guide.md](checkpointing-guide.md), [developing-spices-guide.md](developing-spices-guide.md) (`build.tur` and `:c-sources`)

> **The code**: every snippet below is lifted from `examples/guestbook/`, which builds and is driven end to end by `tests/run-guestbook.sh`. Where the shape is dictated by the capture grammar of serializable continuations (one frame, one env value, an `int` hole, effect-free leaves), the tutorial says so rather than pretending otherwise.

---

## Tutorial Overview

| Step | Title | Key Concepts |
|------|-------|--------------|
| 0 | Project Layout | `build.tur`, one `defmodule` per file, `:c-sources`, build targets |
| 1 | Minimal HTTP Listener | The C shim, inline-C bindings, request accessors |
| 2 | Hello World Handler | Rendering HTML strings, routing, sending a response |
| 3 | Single-Page Form | Parsing form bodies, escaping, the templates |
| 4 | Introducing Continuations | `serial-reset`, `serial-shift`, `serial-resume`, the continuation store |
| 5 | Two-Page Flow | Name form -> message form: a leaf, a receiver, a page, `advance` |
| 6 | Three-Page Preview | Preview page, Back as a `decision` field on one continuation |
| 7 | Persisting the Store | The entries file, atomic append |
| 8 | Confirmation and Posting | Confirm, writing the entry, the thank-you page |
| 9 | Security Hardening | HMAC token signing, expiry, input sanitization, the smoke test |

---

## Background: The Web-Programming Problem

### The Stateless Mismatch

HTTP is stateless. A web form spanning multiple pages is inherently stateful. The standard solutions all require the programmer to manually manage state:

| Approach | Mechanism | Drawbacks |
|----------|-----------|-----------|
| Hidden form fields | Encode state in `<input type="hidden">` | Tedious, error-prone, exposed to tampering |
| Server-side sessions | Cookie holds session ID, server stores map | Memory leak risk, sticky sessions, scaling issues |
| URL query params | State embedded in URL | URL length limits, bookmarking problems |
| Explicit state machine | Each page is a named step in a DB table | Lots of boilerplate, business logic fragmented |

### The Continuation Solution

The continuation approach, popularized by PLT Scheme's `web-server/servlet` and Racket's `send/suspend`, threads the control-flow problem away entirely:

```
handler-1 runs to a "send this form, then resume" point
  -> captures continuation k
  -> stashes k under token T
  -> sends HTML page with action="/?k=T"

browser submits form
  -> server looks up T, loads k
  -> resumes k with form data
  -> k is now inside handler-1 again, with form data in hand
  -> proceeds naturally to the next step
```

From the programmer's perspective, each page is "capture what happens when this form is posted, send the form". There is no session table and there are no hidden form fields: the state a later page needs rides inside the captured frame, on disk, under a token only the server can mint.

### Racket as the Reference Point

Racket's `send/suspend` API looks like this (for comparison):

```racket
(define (enter-name)
  (send/suspend
    (lambda (k-url)
      (response/xexpr
        `(html (body
          (form ([action ,k-url])
            (input ([name "name"]))
            (input ([type "submit"] [value "Next"])))))))))
```

Turmeric's `serial-shift` plays the same role as `send/suspend`, with the difference that the continuation is serialized to bytes and stored on disk rather than living in a heap closure -- and that the captured region is one frame (the page's "submitted" call), not an arbitrary stack.

---

## Step 0: Project Layout

### Directory Structure

The guestbook lives at `examples/guestbook/` and is a **spice**: a `build.tur`
manifest, one module per file under `src/`, and the C shim vendored through
`:c-sources`.

```
examples/guestbook/
  build.tur            -- the manifest: (defpackage guestbook ... :c-sources ["httpd.c"])
  CMakeLists.txt       -- optional CMake target (emit-c main.tur + httpd.c)
  httpd.c              -- tiny raw-socket HTTP listener (~230 lines)
  src/
    main.tur           -- entry point: starts the listener, runs the serve loop
    httpd.tur          -- inline-C bindings over httpd.c
    strutil.tur        -- str+, slicing, form-field, the pointer casts for the hole
    security.tur       -- html-escape, HMAC-SHA256, sign/verify tokens
    templates.tur      -- HTML rendering for every page
    store.tur          -- the entries file: append / read
    conts.tur          -- the continuation store: token -> data/conts/<token>.bin
    handlers.tur       -- the pages: leaves, receivers, resets, advance
    router.tur         -- GET /, GET /entries, POST /submit?k=TOKEN
  data/                -- created at runtime
    entries.txt        -- one line per entry
    conts/             -- one file per continuation
```

```turmeric
;; build.tur
(defpackage guestbook
  :name "guestbook"
  :build-opts #map{
    :c-sources ["httpd.c"]
  })
```
```sweet-exp
;; build.tur.sweet
defpackage guestbook :name "guestbook" :build-opts #map{:c-sources ["httpd.c"]}
```

Because there is a manifest, every per-file command resolves the imports on
its own: `tur check src/router.tur` works, `tur run src/main.tur` links
`httpd.c`, and `tur build examples/guestbook` puts the binary in
`examples/guestbook/build/bin/guestbook`.

### Modules

Each file is a `defmodule` that imports what it uses by name:

```turmeric
(defmodule router
  (import strutil :refer [form-field cstr-eq? cstr-as-int])
  (import templates :refer [render-error])
  (import conts :refer [load-continuation])
  (import handlers :refer [start-flow advance list-entries])
  (import httpd :refer [httpd/method httpd/path httpd/query httpd/body httpd/send!])
  (export dispatch)
  ...)
```
```sweet-exp
defmodule router
  (import strutil :refer [form-field cstr-eq? cstr-as-int])
  (import templates :refer [render-error])
  (import conts :refer [load-continuation])
  (import handlers :refer [start-flow advance list-entries])
  (import httpd :refer [httpd/method httpd/path httpd/query httpd/body httpd/send!])
  (export dispatch)
  ...
```

### Build Targets

`tur run src/main.tur` is the everyday build. The CMake target exists so the
example sits beside the others (`cmake -DTUR_EXAMPLES=ON`, then
`cmake --build build --target guestbook`); it runs `tur emit-c src/main.tur`
-- which emits every imported module into one C file -- and compiles that
with `httpd.c`. The root `Justfile` keeps its `guestbook` / `run-guestbook`
recipes.

Run it:

```sh
GUESTBOOK_SECRET="a long random string" PORT=8080 tur run examples/guestbook/src/main.tur
```

---

## Step 1: Minimal HTTP Listener

### The C Shim

Rather than pulling in a third-party library, the tutorial ships a tiny
`httpd.c` that uses only POSIX sockets. Three functions are exported:

```c
/* Bind a TCP socket and start listening on port. 0 on success, -1 on error. */
int httpd_start(int port);

/* Block until the next HTTP request arrives.  Writes method, path, query and
 * body into the out-params; the caller frees the four strings. */
void httpd_next_request(char **out_method, char **out_path,
                        char **out_query, char **out_body);

/* Send an HTTP/1.1 response and close the connection. */
void httpd_send_response(int status, const char *content_type, const char *body);
```

One thing the shim needs that is easy to miss: `strdup` and `strncasecmp` are
POSIX, not ISO C. Without `_DEFAULT_SOURCE` / `_POSIX_C_SOURCE` at the top of
the file the compiler sees no prototype under `-std=c11`, assumes `int
strdup()`, truncates the returned pointer, and the first request segfaults.

### Turmeric Bindings

`httpd.tur` wraps the shim in inline-C. The four out-params are awkward to
carry across the boundary, so one function owns them as static storage and
the accessors read from it:

```turmeric
(defmodule httpd
  (export httpd/start httpd/wait! httpd/method httpd/path httpd/query httpd/body
          httpd/send!)

  ;;; httpd/request-slot -- static request storage (internal).
  ;;; op 0: accept + parse the next request (frees the previous one).
  ;;; op 1..4: method / path / query / body of the current request.
  (defn httpd/request-slot [op : int] : cstr
    ```c
    extern void httpd_next_request(char **m, char **p, char **q, char **b);
    static char *method = NULL, *path = NULL, *query = NULL, *body = NULL;
    switch (op) {
      case 0:
        free(method); free(path); free(query); free(body);
        method = path = query = body = NULL;
        httpd_next_request(&method, &path, &query, &body);
        return "";
      case 1: return method ? method : "";
      case 2: return path   ? path   : "";
      case 3: return query  ? query  : "";
      case 4: return body   ? body   : "";
      default: return "";
    }
    ```)

  (defn httpd/start [port : int] : int
    ```c
    extern int httpd_start(int port);
    return httpd_start((int)port);
    ```)

  (defn httpd/wait! [] : int
    (do (httpd/request-slot 0) 0))

  (defn httpd/method [] : cstr (httpd/request-slot 1))
  (defn httpd/path   [] : cstr (httpd/request-slot 2))
  (defn httpd/query  [] : cstr (httpd/request-slot 3))
  (defn httpd/body   [] : cstr (httpd/request-slot 4))

  (defn httpd/send! [status : int content-type : cstr body : cstr] : int
    ```c
    extern void httpd_send_response(int status, const char *ct, const char *body);
    httpd_send_response((int)status, content_type, body);
    return status;
    ```))
```
```sweet-exp
defmodule httpd
  (export httpd/start httpd/wait! httpd/method httpd/path httpd/query httpd/body
          httpd/send!)

  ;;; httpd/request-slot -- static request storage (internal).
  ;;; op 0: accept + parse the next request (frees the previous one).
  ;;; op 1..4: method / path / query / body of the current request.
  defn httpd/request-slot [op : int] : cstr
    ```c
    extern void httpd_next_request(char **m, char **p, char **q, char **b);
    static char *method = NULL, *path = NULL, *query = NULL, *body = NULL;
    switch (op) {
      case 0:
        free(method); free(path); free(query); free(body);
        method = path = query = body = NULL;
        httpd_next_request(&method, &path, &query, &body);
        return "";
      case 1: return method ? method : "";
      case 2: return path   ? path   : "";
      case 3: return query  ? query  : "";
      case 4: return body   ? body   : "";
      default: return "";
    }
    ```

  defn httpd/start [port : int] : int
    ```c
    extern int httpd_start(int port);
    return httpd_start((int)port);
    ```

  defn httpd/wait! [] : int
    do
      httpd/request-slot(0)
      0

  defn httpd/method [] : cstr httpd/request-slot(1)
  defn httpd/path   [] : cstr httpd/request-slot(2)
  defn httpd/query  [] : cstr httpd/request-slot(3)
  defn httpd/body   [] : cstr httpd/request-slot(4)

  defn httpd/send! [status : int content-type : cstr body : cstr] : int
    ```c
    extern void httpd_send_response(int status, const char *ct, const char *body);
    httpd_send_response((int)status, content_type, body);
    return status;
    ```
```

The `extern` declarations inside the bodies are all the glue there is: the
symbols resolve because `build.tur` compiles and links `httpd.c`.

There is no request struct. A request is "whatever `httpd/wait!` last read",
and a response is a call to `httpd/send!` -- the shim owns the socket.

---

## Step 2: Hello World Handler

### Sending a Page

The smallest handler renders a string and sends it:

```turmeric
(defn send-html [html : cstr] : int
  (httpd/send! 200 "text/html" html))

(defn list-entries [] : int
  (send-html (render-entries (store-lines))))
```
```sweet-exp
defn send-html [html : cstr] : int
  httpd/send!(200 "text/html" html)

defn list-entries [] : int
  send-html $ render-entries(store-lines())
```

An earlier draft of this example routed every response through an algebraic
effect (`perform HttpEffect (send-html ...)`) handled in the main loop. The
shipped example sends directly, because the pages of Step 4 run inside
serializable-continuation contexts, and a function that performs an effect
is *colored* for the CPS backend -- which the capture grammar does not admit
in a context callee. Direct sends keep every page function plain.

### The Serve Loop

`main.tur` binds the port and loops: read a request, dispatch it, sweep
expired continuations.

```turmeric
(defn serve-loop [max : int] : int
  (let [^mut served 0]
    (do
      (while (or (= max 0) (< served max))
        (do
          (httpd/wait!)
          (dispatch)
          (evict-expired-conts!)
          (set! served (+ served 1))))
      served)))

(defn main [] : int
  (let [port (cstr->int-or (env-or "PORT" "8080") 8080)
        max  (cstr->int-or (env-or "GUESTBOOK_MAX_REQUESTS" "0") 0)]
    (do
      (store-init!)
      (if (< (httpd/start port) 0)
        (do (println (str+ "guestbook: failed to start HTTP listener on port " (int->cstr port)))
            1)
        (do
          (println (str+ "guestbook: listening on http://127.0.0.1:" (int->cstr port) "/"))
          (serve-loop max)
          0)))))
```
```sweet-exp
defn serve-loop [max : int] : int
  let [^mut served 0]
    do
      while {{max = 0} or {served < max}}
        do
          httpd/wait!()
          dispatch()
          evict-expired-conts!()
          set!(served {served + 1})
      served

defn main [] : int
  let [port cstr->int-or(env-or("PORT" "8080") 8080)
       max  cstr->int-or(env-or("GUESTBOOK_MAX_REQUESTS" "0") 0)]
    do
      store-init!()
      if {httpd/start(port) < 0}
        do
          println $ str+("guestbook: failed to start HTTP listener on port " int->cstr(port))
          1
        do
          println $ str+("guestbook: listening on http://127.0.0.1:" int->cstr(port) "/")
          serve-loop(max)
          0
```

`GUESTBOOK_MAX_REQUESTS=N` serves N requests and exits; that is how the
smoke test in Step 9 drives the server without having to kill it.

### The Router

```turmeric
(defn dispatch [] : int
  (let [get?  (cstr-eq? (httpd/method) "GET")
        post? (cstr-eq? (httpd/method) "POST")
        path  (httpd/path)]
    (cond
      (and get? (cstr-eq? path "/"))         (start-flow)
      (and get? (cstr-eq? path "/entries"))  (list-entries)
      (and post? (cstr-eq? path "/submit"))  (resume-handler)
      else (send-error 404 "Page not found."))))
```
```sweet-exp
defn dispatch [] : int
  let [get?  cstr-eq?(httpd/method() "GET")
       post? cstr-eq?(httpd/method() "POST")
       path  httpd/path()]
    cond
      {get? and cstr-eq?(path "/")}
      start-flow()
      {get? and cstr-eq?(path "/entries")}
      list-entries()
      {post? and cstr-eq?(path "/submit")}
      resume-handler()
      else
      send-error(404 "Page not found.")
```

---

## Step 3: Single-Page Form

### Rendering the Name Form

`templates.tur` builds every page from `str+`, a variadic concatenation
helper from `strutil.tur`, inside a shared shell:

```turmeric
(defn page-shell [title : cstr content : cstr] : cstr
  (str+ "<!doctype html><html><head><meta charset=\"utf-8\"><title>" title
        "</title>...</head><body><h1>" title "</h1>" content
        "<p class=\"muted\"><a href=\"/\">Sign the guestbook</a> &middot; "
        "<a href=\"/entries\">Read the entries</a></p></body></html>"))

(defn render-name-form [action : cstr] : cstr
  (page-shell "Guestbook"
    (str+ "<form method=\"post\" action=\"" action "\">"
          "<label>Your name <input name=\"name\" autofocus></label>"
          "<button>Next</button></form>")))
```
```sweet-exp
defn page-shell [title : cstr content : cstr] : cstr
  str+("<!doctype html><html><head><meta charset=\"utf-8\"><title>" title
       "</title>...</head><body><h1>" title "</h1>" content
       "<p class=\"muted\"><a href=\"/\">Sign the guestbook</a> &middot; "
       "<a href=\"/entries\">Read the entries</a></p></body></html>")

defn render-name-form [action : cstr] : cstr
  page-shell
    "Guestbook"
    str+("<form method=\"post\" action=\"" action "\">"
         "<label>Your name <input name=\"name\" autofocus></label>"
         "<button>Next</button></form>")
```

`action` is the URL the form posts to. In Step 4 it will carry a
continuation token; for now imagine `/submit`.

### Parsing Form Fields

A POST body is `application/x-www-form-urlencoded`: `name=Ada+%3CL%3E&x=1`.
`form-field` in `strutil.tur` finds a field and percent-decodes it,
returning an `(Option cstr)` built with the typed inline-C builders
(`tur_some_ptr` / `tur_none`):

```turmeric
(defn form-field [encoded : cstr name : cstr] : (Option cstr)
  ```c
  ... walk `&`-separated segments, match the key, decode %XX and '+' ...
  return tur_some_ptr(raw);   /* or tur_none() */
  ```)

;; handlers.tur
(defn field-or [body : cstr name : cstr dflt : cstr] : cstr
  (match (form-field body name)
    (Some v) v
    (None)   dflt))
```
```sweet-exp
defn form-field [encoded : cstr name : cstr] : (Option cstr)
  ```c
  ... walk `&`-separated segments, match the key, decode %XX and '+' ...
  return tur_some_ptr(raw);   /* or tur_none() */
  ```

;; handlers.tur
defn field-or [body : cstr name : cstr dflt : cstr] : cstr
  (match (form-field body name)
    (Some v) v
    (None)   dflt)
```

### Escaping

Everything a visitor typed is escaped once, when it is read, and stays
escaped for the rest of its life (frames, the entries file, the pages):

```turmeric
(html-escape "<b>")   ; => "&lt;b&gt;"   (& < > " escaped; newline -> <br>)
```
```sweet-exp
html-escape("<b>")   ; => "&lt;b&gt;"   (& < > " escaped; newline -> <br>)
```

---

## Step 4: Introducing Continuations

### The Core Idea

Page 1 wants to say: "send the name form; when it comes back, run
`name-submitted` with the posted body." That sentence is a `serial-reset`
whose rest is the call, with the shift in argument position:

```turmeric
(defn start-flow [] : int
  (serial-reset
    (name-submitted "" (serial-shift suspend-name-page 0))))
```
```sweet-exp
defn start-flow [] : int
  serial-reset
    name-submitted("" serial-shift(suspend-name-page 0))
```

`serial-shift` captures the rest of the reset -- the pending call
`(name-submitted "" <hole>)` -- as a `serial-cont` and hands it to the
receiver `suspend-name-page`. The receiver does not resume it:

```turmeric
;;; action-for -- store k and return the form action URL that resumes it.
(defn action-for [k : serial-cont] : cstr
  (str+ "/submit?k=" (store-continuation k)))

;;; suspend-name-page -- page 1's receiver.
(defn suspend-name-page [k : serial-cont] : int
  (send-html (render-name-form (action-for k))))
```
```sweet-exp
;;; action-for -- store k and return the form action URL that resumes it.
defn action-for [k : serial-cont] : cstr
  str+("/submit?k=" store-continuation(k))

;;; suspend-name-page -- page 1's receiver.
defn suspend-name-page [k : serial-cont] : int
  send-html $ render-name-form(action-for(k))
```

The reset then returns the receiver's value and the request is over. The
continuation lives on only as a file. When the browser posts the form, the
router rebuilds it and resumes it with the body:

```turmeric
(defn resume-handler [] : int
  (match (form-field (httpd/query) "k")
    (None) (send-error 400 "Missing continuation token.")
    (Some t)
      (match (load-continuation t)
        (Err m) (send-error 404 m)
        (Ok k)  (advance (serial-resume k (cstr-as-int (httpd/body)))))))
```
```sweet-exp
defn resume-handler [] : int
  (match (form-field (httpd/query) "k")
    (None) (send-error 400 "Missing continuation token.")
    (Some t)
      (match (load-continuation t)
        (Err m) (send-error 404 m)
        (Ok k)  (advance (serial-resume k (cstr-as-int (httpd/body))))))
```

and `name-submitted` runs, with the body in the hole:

```turmeric
(defn name-submitted [ignored : cstr body-i : int] : int
  (let [body (int-as-cstr body-i)]
    (do
      (set! flow-name (html-escape (field-or body "name" "Anonymous")))
      (set! flow-message "")
      (step-message))))
```
```sweet-exp
defn name-submitted [ignored : cstr body-i : int] : int
  let [body int-as-cstr(body-i)]
    do
      set!(flow-name html-escape(field-or(body "name" "Anonymous")))
      set!(flow-message "")
      step-message()
```

Three things the capture grammar dictates, stated once:

1. **The hole is an `int`.** The body is passed as its address
   (`cstr-as-int`) and read back with `int-as-cstr`. The bytes on disk hold
   the *frame*, never the body -- it is only ever in the process that read it.
2. **The frame carries one environment value** -- the first argument
   (`""` here; the name on page 2). It must be an `int`, a `cstr`, or a
   `Serializable` value.
3. **The leaf is uncolored.** `name-submitted` does not start the next page's
   reset; it stashes what the next page needs and returns a **step code**.
   `advance`, called by the router *outside* any reset, does the rest.

### Token Generation and Storage

`conts.tur` turns a `serial-cont` into a file and back:

```turmeric
(defn store-continuation [k : serial-cont] : cstr
  (let [token (random-hex-64)
        bytes (serial-cont->bytes k)]
    (do
      (cont-to-file bytes (cont-path token))
      (bytes-release bytes)
      (sign-token token (server-secret)))))

(defn load-continuation [signed : cstr] : (Result serial-cont cstr)
  (match (verify-token signed (server-secret))
    (None) (err "That link was not issued by this server.")
    (Some token)
      (let [path (cont-path token)
            age  (file-age-seconds path)]
        (if (< age 0)
          (err "Unknown or already used link. Please start over.")
          (if (> age (cont-ttl-seconds))
            (err "That link has expired. Please start over.")
            (bytes->serial-cont (cont-from-file path)))))))
```
```sweet-exp
defn store-continuation [k : serial-cont] : cstr
  let [token random-hex-64()
       bytes serial-cont->bytes(k)]
    do
      cont-to-file(bytes cont-path(token))
      bytes-release(bytes)
      sign-token(token server-secret())

defn load-continuation [signed : cstr] : (Result serial-cont cstr)
  (match (verify-token signed (server-secret))
    (None) (err "That link was not issued by this server.")
    (Some token)
      (let [path (cont-path token)
            age  (file-age-seconds path)]
        (if {age < 0}
          (err "Unknown or already used link. Please start over.")
          (if {age > (cont-ttl-seconds)}
            (err "That link has expired. Please start over.")
            (bytes->serial-cont (cont-from-file path))))))
```

`serial-cont->bytes` marshals the frame by its stable name plus its
environment; `bytes->serial-cont` validates every frame against this
program's registry before rebuilding, so a file written by an older build
comes back as an `Err`, which the router shows as a 404 page.

---

## Step 5: Two-Page Flow

Page 2 is the same three pieces again -- a page, a receiver, a leaf -- plus
one arm in `advance`. What differs is that the frame now carries state:

```turmeric
;; What the next page needs, handed from a resumed leaf to `advance`.
(def ^mut flow-name    : cstr "")
(def ^mut flow-message : cstr "")

(defn page-message [] : int
  (serial-reset
    (message-submitted flow-name (serial-shift suspend-message-page 0))))

(defn suspend-message-page [k : serial-cont] : int
  (send-html (render-message-form (action-for k) flow-name flow-message)))

(defn message-submitted [name : cstr body-i : int] : int
  (let [body (int-as-cstr body-i)]
    (do
      (set! flow-name name)
      (set! flow-message (html-escape (field-or body "message" "")))
      (step-preview))))

(defn advance [step : int] : int
  (cond
    (= step (step-message))  (page-message)
    (= step (step-preview))  (page-preview)
    (= step (step-thankyou)) (send-html (render-thankyou (store-lines)))
    else 0))
```
```sweet-exp
;; What the next page needs, handed from a resumed leaf to `advance`.
def ^mut flow-name    : cstr ""
def ^mut flow-message : cstr ""

defn page-message [] : int
  serial-reset
    message-submitted(flow-name serial-shift(suspend-message-page 0))

defn suspend-message-page [k : serial-cont] : int
  send-html $ render-message-form(action-for(k) flow-name flow-message)

defn message-submitted [name : cstr body-i : int] : int
  let [body int-as-cstr(body-i)]
    do
      set!(flow-name name)
      set!(flow-message html-escape(field-or(body "message" "")))
      step-preview()

defn advance [step : int] : int
  cond
    {step = step-message()}
    page-message()
    {step = step-preview()}
    page-preview()
    {step = step-thankyou()}
    send-html(render-thankyou(store-lines()))
    else
    0
```

`page-message` captures the *value* of `flow-name` at capture time as the
frame's env; when the message form is posted -- minutes later, after other
visitors' requests, after a server restart -- `message-submitted` receives
that name as its first argument. The globals are only a hand-off between a
leaf and `advance` within one request.

### What Happens at Runtime

```
GET /                    start-flow: capture (name-submitted "" _), write k1 to
                         data/conts/<t1>.bin, send name form (action=/submit?k=t1.sig)
POST /submit?k=t1.sig    load k1, resume with "name=Ada": name-submitted -> step-message
                         advance -> page-message: capture (message-submitted "Ada" _),
                         write k2, send message form (action=/submit?k=t2.sig)
POST /submit?k=t2.sig    load k2, resume: message-submitted "Ada" "message=..." -> step-preview
                         advance -> page-preview ...
```

Each `POST` resumes exactly the page that produced its form, with exactly
that page's state, from a file.

---

## Step 6: Three-Page Preview with Back Navigation

### One Continuation, Two Buttons

The preview shows the entry with **Confirm** and **Back**. Both are submit
buttons in one form posting one continuation; the pressed button's
`decision` value says which:

```turmeric
(defn render-preview [action : cstr name : cstr message : cstr] : cstr
  (page-shell "Preview"
    (str+ "<blockquote><p>" message "</p><footer>&mdash; " name "</footer></blockquote>"
          "<form method=\"post\" action=\"" action "\">"
          "<button name=\"decision\" value=\"confirm\">Confirm</button> "
          "<button name=\"decision\" value=\"back\">Back</button></form>")))
```
```sweet-exp
defn render-preview [action : cstr name : cstr message : cstr] : cstr
  page-shell
    "Preview"
    str+("<blockquote><p>" message "</p><footer>&mdash; " name "</footer></blockquote>"
         "<form method=\"post\" action=\"" action "\">"
         "<button name=\"decision\" value=\"confirm\">Confirm</button> "
         "<button name=\"decision\" value=\"back\">Back</button></form>")
```

The preview's frame needs both the name and the message, and a frame carries
one env value -- so they travel as one tab-separated `cstr`:

```turmeric
(defn pack-state [name : cstr message : cstr] : cstr
  (str+ name "\t" message))

(defn page-preview [] : int
  (let [state (pack-state flow-name flow-message)]
    (serial-reset
      (preview-decided state (serial-shift suspend-preview-page 0)))))
```
```sweet-exp
defn pack-state [name : cstr message : cstr] : cstr
  str+(name "\t" message)

defn page-preview [] : int
  let [state pack-state(flow-name flow-message)]
    serial-reset
      preview-decided(state serial-shift(suspend-preview-page 0))
```

### How Back Navigation Works

`preview-decided` splits the state and reads the decision. Back returns
`step-message` with the message left in `flow-message`, so `advance` shows
the message form again, prefilled; Confirm goes on to Step 8.

```turmeric
(defn preview-decided [state : cstr body-i : int] : int
  (let [body     (int-as-cstr body-i)
        tab      (cstr-index-of state "\t" 0)
        name     (cstr-slice state 0 tab)
        message  (cstr-slice state (+ tab 1) (cstr-len state))
        decision (field-or body "decision" "confirm")]
    (do
      (set! flow-name name)
      (set! flow-message message)
      (if (cstr-eq? decision "back")
        (step-message)
        (do
          (store-append! name message)
          (step-thankyou))))))
```
```sweet-exp
defn preview-decided [state : cstr body-i : int] : int
  let [body     int-as-cstr(body-i)
       tab      cstr-index-of(state "\t" 0)
       name     cstr-slice(state 0 tab)
       message  cstr-slice(state {tab + 1} cstr-len(state))
       decision field-or(body "decision" "confirm")]
    do
      set!(flow-name name)
      set!(flow-message message)
      if cstr-eq?(decision "back")
        step-message()
        do
          store-append!(name message)
          step-thankyou()
```

The browser's own Back button works too: an earlier page's form still names
an unexpired token, and posting it resumes that page again -- the same
mechanism Racket's `send/suspend` gets for free.

---

## Step 7: Persisting the Guestbook Store

The entries file is one line per entry, `posted-at<TAB>name<TAB>message`.
Name and message were HTML-escaped when read (which also removed tabs and
newlines), so the format needs no quoting:

```turmeric
(defn store-append! [name : cstr message : cstr] : int
  ```c
  #include <time.h>
  FILE *old = fopen("data/entries.txt", "rb");
  FILE *tmp = fopen("data/entries.tmp", "wb");
  ... copy old into tmp, append the new line ...
  fprintf(tmp, "%lld\t%s\t%s\n", (long long)time(NULL), name, message);
  fclose(tmp);
  return rename("data/entries.tmp", "data/entries.txt") == 0 ? 1 : 0;
  ```)

(defn store-lines [] : cstr   ;; the whole file, or "" when there is none yet
  ...)
```
```sweet-exp
defn store-append! [name : cstr message : cstr] : int
  ```c
  #include <time.h>
  FILE *old = fopen("data/entries.txt", "rb");
  FILE *tmp = fopen("data/entries.tmp", "wb");
  ... copy old into tmp, append the new line ...
  fprintf(tmp, "%lld\t%s\t%s\n", (long long)time(NULL), name, message);
  fclose(tmp);
  return rename("data/entries.tmp", "data/entries.txt") == 0 ? 1 : 0;
  ```

defn store-lines [] : cstr   ;; the whole file, or "" when there is none yet
  ...
```

Writes go through a temp file and `rename`, so a crash mid-write leaves the
previous file intact. `render-entries` / `render-thankyou` walk the lines
with `cstr-index-of` / `cstr-slice` and emit one `<li>` per entry.

The store needs no `Serializable` instance because nothing in it is
captured: pages capture the escaped strings, and the store is read fresh on
every render.

---

## Step 8: Confirmation, Posting, and Thank-You

Confirm is the `else` branch of `preview-decided` above: append the entry,
return `step-thankyou`. The thank-you page is rendered by `advance`, not by
the leaf -- the templates go through `str+`, and a leaf that called into them
would be colored and rejected as a context callee. (`str+` is a plain
inline-C cons walk with no `unsafe` block precisely so the *receivers* may
call the templates; they are context callees too.)

```turmeric
(defn render-thankyou [lines : cstr] : cstr
  (page-shell "Thank you!"
    (str+ "<p>Your entry has been added.</p><ul>" (entry-rows lines) "</ul>")))
```
```sweet-exp
defn render-thankyou [lines : cstr] : cstr
  page-shell
    "Thank you!"
    str+("<p>Your entry has been added.</p><ul>" entry-rows(lines) "</ul>")
```

---

## Step 9: Security Hardening

### HMAC Token Signing

A raw token names a file under `data/conts/`. Signing it means a client
cannot probe the store by guessing names: the router refuses anything whose
signature does not verify before it touches the filesystem.

```turmeric
(defn sign-token [token : cstr secret : cstr] : cstr
  (str+ token "." (hmac-sha256-hex secret token)))

(defn verify-token [signed : cstr secret : cstr] : (Option cstr)
  (let [dot (cstr-index-of signed "." 0)]
    (if (< dot 0)
      (none)
      (let [token (cstr-slice signed 0 dot)
            sig   (cstr-slice signed (+ dot 1) (cstr-len signed))]
        (if (cstr-eq-ct sig (hmac-sha256-hex secret token))
          (some token)
          (none))))))
```
```sweet-exp
defn sign-token [token : cstr secret : cstr] : cstr
  str+(token "." hmac-sha256-hex(secret token))

defn verify-token [signed : cstr secret : cstr] : (Option cstr)
  let [dot cstr-index-of(signed "." 0)]
    if {dot < 0}
      none()
      let [token cstr-slice(signed 0 dot)
           sig   cstr-slice(signed {dot + 1} cstr-len(signed))]
        if cstr-eq-ct(sig hmac-sha256-hex(secret token))
          some(token)
          none()
```

`hmac-sha256-hex` is a self-contained inline-C SHA-256 (the same core as
`stdlib/image.tur`'s build stamp) with the ipad/opad construction;
`cstr-eq-ct` compares in constant time. The key is `GUESTBOOK_SECRET`; the
default `dev-insecure-secret` is for local testing only.

### Token Expiry

`load-continuation` refuses a file older than `cont-ttl-seconds` (30
minutes) by its mtime, and the serve loop calls `evict-expired-conts!`
after every request to delete them.

### Input Sanitization

- `html-escape` at the moment of reading (`name-submitted`,
  `message-submitted`) -- the only place raw visitor text exists.
- `form-field` percent-decodes; a field that is absent is `(None)`, never
  an empty pointer.
- The body never reaches the disk: it rides through the hole as an address
  within one request.

### The Smoke Test

`tests/run-guestbook.sh` (ctest `tur_guestbook_smoke`) is the proof that all
of this works through real HTTP. It builds the example with `tur run`, starts
it with `GUESTBOOK_MAX_REQUESTS=9` on a private port, and uses `curl` to:
fetch page 1, post a name, post a message, press Back, post a new message,
Confirm, post a tampered token (refused), and read `/entries` -- every hop
resuming a continuation rebuilt from a file the previous response named.

```
PASS page 1 is the name form
PASS page 1 action carries a signed token
PASS page 2 greets the escaped name
PASS page 3 previews the message
PASS page 3 previews the name
PASS Back returns to the message form, prefilled
PASS Confirm shows the thank-you page with the entry
PASS a tampered token is refused
PASS /entries lists the posted entry
PASS /entries does not list the abandoned draft
guestbook: 10 passed, 0 failed
```

---

## Where to Go Next

- Add a page: one leaf, one receiver, one page function, one `advance` arm.
- Capture a struct instead of a packed string: give it a `Serializable`
  instance (`serializable-continuations-guide.md`) and pass it as the frame
  env.
- Swap the store: `store-continuation` / `load-continuation` are the whole
  contract (`web-continuations-guide.md`, "Continuation Store Contract").
