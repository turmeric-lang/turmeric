---
title: Web Continuations Guide
category: Advanced Control Flow
description: Compact reference: one continuation per page, the continuation store, routing model
---

# Web Continuations Guide

A compact reference for building multi-page web applications in Turmeric using serializable continuations. This guide assumes you understand `serial-reset` / `serial-shift` / `serial-resume` and the capture grammar -- see [serializable-continuations-guide.md](serializable-continuations-guide.md) for that background.

For a full step-by-step walkthrough with the complete guestbook example (`examples/guestbook/`), see [web-continuations-tutorial.md](web-continuations-tutorial.md).

---

## The One-Continuation-Per-Page Pattern

The unit of a flow is a page: a `serial-reset` whose rest is a call to that
page's *submitted* function with the hole in argument position, and a named
receiver that stores the continuation and sends the form.

```turmeric
;; The page: capture "what happens when the message form is posted".
(defn page-message [] : int
  (serial-reset
    (message-submitted flow-name (serial-shift suspend-message-page 0))))

;; The receiver: store k under a signed token, send the form whose action
;; carries the token.  It returns WITHOUT resuming k -- the request is over.
(defn suspend-message-page [k : serial-cont] : int
  (send-html (render-message-form (action-for k) flow-name flow-message)))

;; The leaf: runs when the browser posts.  `name` is the frame's env (what
;; page-message captured), `body-i` is the POST body arriving through the hole.
(defn message-submitted [name : cstr body-i : int] : int
  (let [body (int-as-cstr body-i)]
    (do
      (set! flow-name name)
      (set! flow-message (html-escape (field-or body "message" "")))
      (step-preview))))
```
```sweet-exp
;; The page: capture "what happens when the message form is posted".
defn page-message [] : int
  serial-reset
    message-submitted(flow-name serial-shift(suspend-message-page 0))

;; The receiver: store k under a signed token, send the form whose action
;; carries the token.  It returns WITHOUT resuming k -- the request is over.
defn suspend-message-page [k : serial-cont] : int
  send-html $ render-message-form(action-for(k) flow-name flow-message)

;; The leaf: runs when the browser posts.  `name` is the frame's env (what
;; page-message captured), `body-i` is the POST body arriving through the hole.
defn message-submitted [name : cstr body-i : int] : int
  let [body int-as-cstr(body-i)]
    do
      set!(flow-name name)
      set!(flow-message html-escape(field-or(body "message" "")))
      step-preview()
```

Three rules of the capture grammar shape this:

- **The frame carries one environment value** (`name` above, an `int` or a
  `cstr`, or a value with a `Serializable` instance). Pack more than one into
  one `cstr` (the preview page carries `name<TAB>message`).
- **The hole is an `int`.** The POST body travels as its address
  (`cstr-as-int` / `int-as-cstr`); both ends are the same process, and the
  bytes on disk hold the frame, not the body.
- **No effect may escape the leaf or the receiver**, and neither may start a
  `serial-reset` of its own. They run outside the handlers around the reset
  (the receiver at capture, the leaf on resume), so an effect they do not
  handle themselves has nowhere to go; calling through a function value -- a
  template passed as `(fn [cstr] cstr)` -- is fine. So a leaf does not start
  the next page; it returns a **step code**, and `advance`, called by the
  router outside every reset, starts the next page's reset (or renders a
  terminal page):

```turmeric
(defn advance [step : int] : int
  (cond
    (= step (step-message))  (page-message)
    (= step (step-preview))  (page-preview)
    (= step (step-thankyou)) (send-html (render-thankyou (store-lines)))
    else 0))
```
```sweet-exp
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

**When to use it:** any time the flow needs to pause, show a page, and resume
when the user submits a form -- "click to continue" pages included (a form
with one submit button).

**When not to use it:** for pages that resume nothing (a static about page, a
404) -- send the HTML directly.

---

## Continuation Store Contract

The continuation store (`conts.tur`) satisfies:

```
;; Store k and return the signed token that resumes it.
(store-continuation k) : cstr

;; Rebuild the continuation a signed token names.
(load-continuation signed) : (Result serial-cont cstr)

;; Delete continuation files older than the TTL; returns how many.
(evict-expired-conts!) : int
```

### Token Format

`<64 hex chars>.<64 hex chars>`: 256 bits of randomness (the file name)
followed by the HMAC-SHA256 of it under `GUESTBOOK_SECRET`. `verify-token`
compares signatures in constant time; a token the server did not issue never
reaches the filesystem.

### Storage Interface

File-per-token storage in `data/conts/`:

| Operation | Path | Notes |
|-----------|------|-------|
| `store-continuation k` | Writes `data/conts/<token>.bin` | `serial-cont->bytes` framed by `cont-to-file` |
| `load-continuation signed` | Reads `data/conts/<token>.bin` | `Err` for a bad signature, a missing file, a file older than the TTL, or bytes `bytes->serial-cont` cannot rebuild |

### Eviction Policy

The main loop calls `evict-expired-conts!` after every request; it removes
files whose mtime is older than the TTL (30 minutes). `load-continuation`
applies the same age check, so a token that outlived the sweep is still
refused.

---

## What Crosses the Boundary

A frame's environment marshals **by value**: an `int` inline, a `cstr` as its
bytes, anything else through its `Serializable` instance. The guestbook keeps
every captured value a `cstr` (already HTML-escaped) so no instance is needed;
a flow that captures a struct writes one (`serializable-continuations-guide.md`,
"The `Serializable` Typeclass"). Resource types (file handles, sockets) cannot
be captured -- the listener lives outside every reset, in `httpd.tur`'s static
storage.

---

## Routing Model

The router maps `POST /submit?k=TOKEN` to `serial-resume`:

```
POST /submit?k=TOKEN
  -> parse TOKEN from the query string           (form-field (httpd/query) "k")
  -> verify the signature, age and bytes         (load-continuation t)
  -> resume with the POST body                   (serial-resume k (cstr-as-int (httpd/body)))
  -> the page's leaf runs, returns a step code
  -> advance starts the next page's reset        (advance step)
  -> its receiver sends the next form (or the thank-you page is sent)
```

Every response is sent by whoever produces it (`httpd/send!`); a request's
response is exactly one send.

### Routing Table

| Method | Path | Handler |
|--------|------|---------|
| GET | `/` | Start a new flow (`start-flow`, page 1) |
| POST | `/submit` | Resume a continuation (`resume-handler`, then `advance`) |
| GET | `/entries` | List all entries (no continuation needed) |
| anything else | any | 404 |

---

## Composing Flows

### Sequential Pages

Pages compose through step codes, not through nesting: a leaf returns the
code of the page that should follow, and `advance` is the one place that
knows the order. Adding a page is one leaf, one receiver, one page function,
one `cond` arm.

### Back Navigation

The preview page's form has two submit buttons posting the same
continuation, distinguished by a `decision` field. `preview-decided` reads
it: `back` stashes name and message and returns `step-message` (the message
form comes back prefilled), `confirm` appends the entry and returns
`step-thankyou`. One token per page; the browser's own Back button reaching an
older form works the same way, since an unexpired token can be posted again.

### Nested Boundaries

Do not nest `serial-reset` inside a reset context, and do not call a function
that contains one from a leaf: the context collector rejects the capture
(`TUR-E0706`). One reset per page, started from outside.

### Resuming From a Different Code Path

A token stored by one request can be resumed by any code path that can read
the file -- an admin approval endpoint, a CLI -- as long as it is the same
program: frames are marshalled by name and `bytes->serial-cont` validates
every name against the running program's registry.

---

## Comparison to Racket's `send/suspend`

| Concept | Racket | Turmeric |
|---------|--------|----------|
| Boundary | `(send/suspend proc)` | `(serial-reset ...)` per page |
| Pause and hand URL to renderer | `send/suspend` calls `proc` with the resume URL | `(serial-shift receiver 0)` hands `k` to the named receiver, which stores it and renders the form with `/submit?k=<token>` |
| Resume URL token | Racket generates a URL using an in-memory store | `store-continuation` generates a signed hex token backed by files |
| Resume | Browser follows URL -> Racket resumes heap closure | Browser POSTs token -> router calls `serial-resume k body` |
| Persistence | Continuations lost on server restart (default) | Continuations persist across restarts (files on disk) |
| Type safety | Dynamic | `k : serial-cont` -- an opaque, typed handle; `bytes->serial-cont` validates before resuming |
| Back navigation | Re-using an earlier URL | Re-using an earlier token (same mechanism) |

---

## Limitations

**No true call/cc semantics.** `serial-shift` is a *delimited* shift -- it captures only up to the enclosing `serial-reset`. You cannot capture the continuation of the entire program.

**One frame, one env, one int hole.** The capture grammar is a single-scalar-hole chain; a page's state is one `cstr` (or `Serializable` value) and the resume value is an `int`. Pack state, and pass bulky data by reference (an id into the store), not by value.

**Effects stay out of leaves.** Anything the reset context calls must not let an effect escape it (TUR-E0706) -- a leaf runs again every time the page resumes, possibly in another process. A named receiver MAY perform an effect it does not handle: the reset calls it on its own continuation, so the effect reaches the handlers around the `serial-reset` (an `if` in the context and a capturing `(fn [k] ...)` receiver are fine too, as long as the closure captures plain values). The guestbook keeps its page-transition effects in `advance` anyway, and its receivers call the templates and the store directly -- written before those restrictions were lifted.

**Token size.** A guestbook continuation is one frame: a few hundred bytes. Monitor `data/conts/` if a flow captures larger values.

**No sharing.** If the same value is reachable via two different paths in a captured continuation, serialization produces two independent copies. On resume, mutations to one copy do not affect the other.

**Single-threaded listener.** The tutorial HTTP listener handles one request at a time. Two simultaneous form submissions are serialized at the socket level. For concurrent flows, run multiple server processes or switch to a multi-threaded listener.

**Schema versioning.** If you change the code between writing a token and resuming it, the frame names in the stored bytes may no longer exist in the running program; `bytes->serial-cont` then returns `Err "bytes->serial-cont: unknown frame (written by a different program?)"` rather than resuming garbage, and the router shows it as a 404 page. Either drain all pending continuations before deploying, or keep a version beside each token and reject stale ones up front. See [serializable-continuations-guide.md](serializable-continuations-guide.md) -- Error Handling.
