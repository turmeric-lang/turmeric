# Plan: `tur-nng` Async Interface (NG5: aio, ctx, reactor)

> Status: draft -- not started.
> Tracks: `docs/archive/nng-spice-plan.md` NG5 follow-ups (aio + ctx, reactor fd).
> Scope: `spices/nng/` in turmeric-spices; no compiler dependency expected.
> Builds on: `tur-nng` 0.1.0 (NG0-NG4, shipped), nng pinned v1.12.4.
> Type: Networking / spice (turmeric-spices).

---

## Overview

`tur-nng` 0.1.0 shipped a blocking-only surface: one linear `Socket`, blocking
`send-str` / `recv-str` / `send-payload` / `recv-payload`, and an owned
`Payload`. The concurrency story was "blocking calls on OS threads via
`stdlib/thread.tur`," matching valkey. The archived plan called out two async
seams as deliberate NG5 follow-ups:

1. **`nng_aio` + `nng_ctx`** -- nng's own async I/O machinery and per-request
   contexts for concurrent protocol state machines.
2. **Reactor integration** -- nng exposes pollable receive/send file descriptors
   that plug into `stdlib/reactor.tur`.

This plan graduates both. They are **orthogonal and independently shippable**:
the reactor path is the Turmeric-native event-loop story (closures fire on the
reactor's thread), and the aio path is nng's own async machinery (submit,
collect later). They serve different patterns and can coexist on one socket.

---

## Motivation

### Why reactor integration

The blocking surface forces one OS thread per concurrent socket operation. A
server that multiplexes many sockets -- a pub/sub fan-in aggregator, a survey
collector, a pair relay -- wants a single-threaded event loop instead.
`stdlib/reactor.tur` already provides one: `reactor-add-fd` watches a file
descriptor and invokes a Turmeric closure when it becomes readable.

nng exposes the seam directly:

```c
NNG_DECL int nng_socket_get_recv_poll_fd(nng_socket, int *fdp);
NNG_DECL int nng_socket_get_send_poll_fd(nng_socket, int *fdp);
```

The fd becomes readable when a message is available; the reactor callback then
calls the existing blocking `recv-str` / `recv-payload` to dequeue it. No new
ownership model, no new opaque types beyond the fd accessor -- the reactor
callback IS a Turmeric fat closure, dispatched on the reactor's own thread
(`src/async/reactor.c`, `call_tur_fd_cb`). This is the same dispatch path
`tur-httpd` and `tur-watch` already use.

### Why `nng_aio`

The reactor path multiplexes *reads*; it does not give you non-blocking
*submit* or fan-out. `nng_aio` is nng's own async I/O handle: you submit a
send or receive with an `nng_aio *`, and collect the result later via
`nng_aio_wait` (blocking) or `nng_aio_busy` + `nng_aio_result` (non-blocking
poll). This is the natural fit for `tur-thread-pool`'s `Future<R>` fan-out:
submit N receives, await them in any order.

The hard part is the completion callback. `nng_aio_alloc` takes a
`void (*)(void *)` that fires on an **arbitrary nng worker thread** with no
locks held. Turmeric fat closures cannot safely be invoked from there -- the
same constraint that makes `thread-spawn-fn` require a raw C function pointer.
The plan's model is: allocate with a **NULL callback**, submit, and collect
via `aio-wait` or poll `aio-busy?` + `aio-result`. This gives "async submit,
sync collect" -- not callback-driven event loops (that is what the reactor
path is for).

### Why `nng_ctx`

A REQ socket allows only one outstanding request at a time on its global
context. `nng_ctx` creates independent per-request contexts on one socket, each
running the protocol's state machine independently -- so a single REQ socket
can have multiple requests in flight simultaneously. Contexts are async-only
(`nng_ctx_send` / `nng_ctx_recv` take an `nng_aio *`), so this layer depends
on the aio layer.

---

## Non-goals

- **Per-protocol typed sockets.** Still its own NG5 follow-up; this plan keeps
  the single `Socket` type. `nng_ctx_open` returns `NNG_ENOTSUP` for protocols
  that lack contexts, and that surfaces as a runtime `err` -- the same shape
  "receive on a PUB socket" already takes.
- **TLS, WebSocket, ZeroTier transports.** `NNG_ENABLE_TLS` stays OFF.
- **Zero-copy `nng_msg` as a public type.** The aio recv path copies out of
  `nng_aio_get_msg` into the existing `Payload` and calls `nng_msg_free`
  immediately, preserving the single-ownership model. The aio send path uses
  `nng_aio_set_iov` to borrow the caller's buffer, avoiding `nng_msg`
  entirely for the simple case. Exposing `nng_msg *` as a typed handle is a
  separate follow-up if the copy cost ever matters.
- **Completion-callback-driven async.** nng's completion callback fires on an
  arbitrary thread; the plan uses the NULL-callback + wait/poll model. A
  future stdlib "C callback trampoline" (if one is planned) would unlock the
  callback path; this plan does not assume one exists.
- **Explicit dialer/listener handles.** Unchanged from v0.
- **nng 2.0 line.** The `nng_recvmsg`-only receive path is a real rewrite,
  not a bump.

---

## Strategy

Three layers, shipped as independent phases. Each is a net-new module or a
small addition to an existing one; none changes the blocking surface.

```
NG-A (reactor fds)  ──►  recv-fd / send-fd + try-recv / try-send
                          [no new opaques, no ownership complexity]

NG-B (nng_aio)      ──►  Aio opaque + send-aio / recv-aio
                          [NULL-callback model; wait or poll to collect]

NG-C (nng_ctx)      ──►  Ctx opaque + ctx-send / ctx-recv
                          [depends on NG-B; concurrent REQ/REP]
```

NG-A and NG-B are independent and can land in either order. NG-C depends on
NG-B. The recommended shipping order is NG-A first (smallest, highest immediate
value), then NG-B, then NG-C.

---

## API Surface

### NG-A -- Pollable FDs + Non-blocking Try (`nng/socket.tur`, `nng/msg.tur`)

Two accessors on `nng/socket.tur` that hand back the pollable fd, plus
non-blocking "try" variants on `nng/msg.tur` that use `NNG_FLAG_NONBLOCK`.

```turmeric
;; spices/nng/src/nng/socket.tur -- additions

;;; recv-fd -- a pollable fd that becomes readable when a message is available.
;;; Register it with reactor-add-fd (READ); when the reactor fires, call
;;; recv-str / recv-payload to dequeue. The fd is level-triggered: it stays
;;; readable until the message is consumed.
;;;
;;; Returns:
;;;   (Result int int) -- ok carries the fd; err carries the nng error code.
(defn recv-fd [^borrow s : Socket] : (Result int int)
  ```c
  #include <nng/nng.h>
  nng_socket sk;
  sk.id = (uint32_t)s;
  int fd = -1;
  int rv = nng_socket_get_recv_poll_fd(sk, &fd);
  if (rv != 0) return tur_err_int((int64_t)rv);
  return tur_ok_int((int64_t)fd);
  ```)

;;; send-fd -- a pollable fd that becomes writable when the socket can accept a send.
(defn send-fd [^borrow s : Socket] : (Result int int)
  ```c
  #include <nng/nng.h>
  nng_socket sk;
  sk.id = (uint32_t)s;
  int fd = -1;
  int rv = nng_socket_get_send_poll_fd(sk, &fd);
  if (rv != 0) return tur_err_int((int64_t)rv);
  return tur_ok_int((int64_t)fd);
  ```)
```

```turmeric
;; spices/nng/src/nng/msg.tur -- additions

;;; try-recv-str -- non-blocking receive; returns (none) if nothing is ready.
;;; Uses NNG_FLAG_NONBLOCK: nng returns NNG_EAGAIN (not NNG_ETIMEDOUT) when
;;; the queue is empty, so the caller distinguishes "nothing yet" from
;;; "timed out" without learning either error code.
;;;
;;; Returns:
;;;   (Option cstr) -- (some cstr) with a fresh malloc'd string, or (none).
(defn try-recv-str [^borrow s : Socket] #fx{Net} : (Option cstr)
  ```c
  #include <nng/nng.h>
  nng_socket sk;
  sk.id = (uint32_t)s;
  void  *data = NULL;
  size_t len  = 0;
  int rv = nng_recv(sk, &data, &len, NNG_FLAG_ALLOC | NNG_FLAG_NONBLOCK);
  if (rv != 0) return tur_none();
  char *out = (char *)malloc(len + 1);
  if (out == NULL) { nng_free(data, len); return tur_none(); }
  if (len > 0) memcpy(out, data, len);
  out[len] = '\0';
  nng_free(data, len);
  return tur_some_ptr(out);
  ```)

;;; try-recv-payload -- non-blocking binary receive; (none) if nothing is ready.
(defn try-recv-payload [^borrow s : Socket] #fx{Net} : (Option Payload)
  ```c
  #include <nng/nng.h>
  extern void *turnng_payload_of_bytes(const void *, int64_t);
  nng_socket sk;
  sk.id = (uint32_t)s;
  void  *data = NULL;
  size_t len  = 0;
  int rv = nng_recv(sk, &data, &len, NNG_FLAG_ALLOC | NNG_FLAG_NONBLOCK);
  if (rv != 0) return tur_none();
  void *out = turnng_payload_of_bytes(data ? data : "", (int64_t)len);
  nng_free(data, len);
  if (out == NULL) return tur_none();
  return tur_some_ptr(out);
  ```)
```

Canonical reactor-driven receive loop:

```turmeric
(import nng/socket :refer [recv-fd close set-recv-timeout-ms])
(import nng/msg    :refer [recv-str])
(import tur/reactor :refer [Reactor reactor-new reactor-add-fd
                            reactor-run reactor-free READ])

(let [sub (ok-val (sub-open))
      _   (dial sub "inproc://feed")
      _   (sub-subscribe sub "")
      r   (reactor-new)
      fd  (ok-val (recv-fd sub))]
  (reactor-add-fd r fd READ
    (fn [id events user]
      (let [msg (recv-str sub)]
        (when (ok? msg)
          (println (ok-val msg)))))
    nil)
  (reactor-run r)
  (reactor-free r)
  (close sub))
```

### NG-B -- `nng_aio` Async Send/Recv (`nng/aio.tur`, net-new module)

A new module `nng/aio.tur` exporting a linear `Aio` handle and the async
send/recv operations. The model is NULL-callback: allocate, submit, collect
via `aio-wait` (blocking) or poll `aio-busy?` + `aio-result` (non-blocking).

```turmeric
;; spices/nng/src/nng/aio.tur -- net-new module

(defmodule nng/aio
  (import nng/socket  :refer [Socket])
  (import nng/payload :refer [Payload payload-free])
  (export Aio
          aio-alloc aio-free aio-stop
          aio-wait aio-busy? aio-result aio-cancel
          aio-set-timeout
          send-aio recv-aio
          aio-recv-payload))

;;; Aio -- a linear handle to an nng asynchronous I/O operation.
;;; Allocated with a NULL completion callback: collect via aio-wait or
;;; poll aio-busy? + aio-result. One Aio handles one outstanding operation
;;; at a time; reuse it with aio-reset after collecting the result.
;;;
;;; Since: Phase NG-B
(defopaque Aio :ptr<void> :linear)

;;; aio-alloc -- allocate a fresh Aio with no completion callback.
;;;
;;; Returns:
;;;   (Result Aio int) -- err carries the nng error code.
(defn aio-alloc [] : (Result Aio int)
  ```c
  #include <nng/nng.h>
  nng_aio *aio = NULL;
  int rv = nng_aio_alloc(&aio, NULL, NULL);
  if (rv != 0) return tur_err_int((int64_t)rv);
  return tur_ok_ptr(aio);
  ```)

;;; aio-free -- free the Aio. Must not be in use (call aio-stop first if
;;; an operation might still be outstanding).
(defn aio-free [a : Aio] : nil
  ```c
  #include <nng/nng.h>
  nng_aio_free((nng_aio *)(intptr_t)a);
  ```)

;;; aio-stop -- stop any outstanding operation and wait for it to complete.
;;; The stopped operation's result is NNG_ESTOPPED. Safe to call before
;;; aio-free when an operation might still be in flight.
(defn aio-stop [^borrow a : Aio] : nil
  ```c
  #include <nng/nng.h>
  nng_aio_stop((nng_aio *)(intptr_t)a);
  ```)

;;; aio-wait -- block until the operation completes. After this, read
;;; aio-result and (for recv) aio-recv-payload.
(defn aio-wait [^borrow a : Aio] : nil
  ```c
  #include <nng/nng.h>
  nng_aio_wait((nng_aio *)(intptr_t)a);
  ```)

;;; aio-busy? -- true if the operation is still in progress (non-blocking).
(defn aio-busy? [^borrow a : Aio] : bool
  ```c
  #include <nng/nng.h>
  return nng_aio_busy((nng_aio *)(intptr_t)a) ? true : false;
  ```)

;;; aio-result -- the nng error code for the completed operation.
;;; 0 = success; NNG_ETIMEDOUT, NNG_ECLOSED, NNG_ECANCELED, etc. on failure.
;;; Use err-str (from nng/socket) to render it.
(defn aio-result [^borrow a : Aio] : int
  ```c
  #include <nng/nng.h>
  return (int)nng_aio_result((nng_aio *)(intptr_t)a);
  ```)

;;; aio-cancel -- attempt to cancel an in-progress operation.
;;; The result will be NNG_ECANCELED if the cancel succeeds.
(defn aio-cancel [^borrow a : Aio] : nil
  ```c
  #include <nng/nng.h>
  nng_aio_cancel((nng_aio *)(intptr_t)a);
  ```)

;;; aio-set-timeout -- per-operation timeout in milliseconds.
;;; Overrides the socket-level recv/send timeout for this one operation.
;;; NNG_DURATION_INFINITE (-1) means wait forever.
(defn aio-set-timeout [^borrow a : Aio ms : int] : nil
  ```c
  #include <nng/nng.h>
  nng_aio_set_timeout((nng_aio *)(intptr_t)a, (nng_duration)ms);
  ```)

;;; send-aio -- submit an async send of a byte buffer.
;;; BORROWS the payload: nng copies via the iov, so `b` is still the caller's.
;;; Does not block; collect with aio-wait / aio-result.
(defn send-aio [^borrow s : Socket ^borrow a : Aio b : Payload] #fx{Net} : nil
  ```c
  #include <nng/nng.h>
  extern int64_t turnng_payload_len(const void *);
  extern void   *turnng_payload_data(void *);
  nng_socket sk;
  sk.id = (uint32_t)s;
  nng_aio *aio = (nng_aio *)(intptr_t)a;
  void   *data = turnng_payload_data((void *)(intptr_t)b);
  size_t  len  = (size_t)turnng_payload_len((const void *)(intptr_t)b);
  if (data == NULL) { data = (void *)""; len = 0; }
  nng_iov iov;
  iov.iov_buf = data;
  iov.iov_len = len;
  nng_aio_set_iov(aio, 1, &iov);
  nng_socket_send(sk, aio);
  ```)

;;; recv-aio -- submit an async receive. Does not block; collect with
;;; aio-wait, then read aio-result and aio-recv-payload.
(defn recv-aio [^borrow s : Socket ^borrow a : Aio] #fx{Net} : nil
  ```c
  #include <nng/nng.h>
  nng_socket sk;
  sk.id = (uint32_t)s;
  nng_socket_recv(sk, (nng_aio *)(intptr_t)a);
  ```)

;;; aio-recv-payload -- extract the received message as an owned Payload.
;;; Call ONLY after aio-wait returns and aio-result is 0. The message is
;;; copied out of nng_aio_get_msg into a fresh Payload and the nng_msg is
;;; freed immediately, preserving the single-ownership model.
;;;
;;; Returns:
;;;   (Result Payload int) -- err carries the nng error code if the
;;;   operation failed or the message is absent.
(defn aio-recv-payload [^borrow a : Aio] : (Result Payload int)
  ```c
  #include <nng/nng.h>
  extern void *turnng_payload_of_bytes(const void *, int64_t);
  nng_aio *aio = (nng_aio *)(intptr_t)a;
  nng_msg *msg = nng_aio_get_msg(aio);
  if (msg == NULL) return tur_err_int((int64_t)NNG_ESTATE);
  void   *body = nng_msg_body(msg);
  size_t  len  = nng_msg_len(msg);
  void *out = turnng_payload_of_bytes(body ? body : "", (int64_t)len);
  nng_msg_free(msg);
  if (out == NULL) return tur_err_int((int64_t)NNG_ENOMEM);
  return tur_ok_ptr(out);
  ```)

) ;; end defmodule nng/aio
```

Canonical aio fan-out (with `tur-thread-pool`):

```turmeric
(import nng/aio :refer [aio-alloc aio-free aio-wait aio-result aio-recv-payload])
(import nng/socket :refer [pull-open dial close])
(import nng/msg :refer [recv-aio])

(let [pull (ok-val (pull-open))
      _    (dial pull "inproc://jobs")
      a    (ok-val (aio-alloc))]
  (recv-aio pull a)
  (aio-wait a)
  (let [rv (aio-result a)]
    (if (= rv 0)
      (let [payload (ok-val (aio-recv-payload a))]
        (println (payload->hex payload))
        (payload-free payload))))
  (aio-free a)
  (close pull))
```

### NG-C -- `nng_ctx` Concurrent Contexts (`nng/ctx.tur`, net-new module)

A new module `nng/ctx.tur` exporting a linear `Ctx` handle. Contexts are
async-only: `ctx-send` / `ctx-recv` take an `Aio` from NG-B.

```turmeric
;; spices/nng/src/nng/ctx.tur -- net-new module

(defmodule nng/ctx
  (import nng/socket :refer [Socket])
  (import nng/aio    :refer [Aio])
  (export Ctx ctx-open ctx-close ctx-send ctx-recv
          ctx-set-recv-timeout-ms ctx-set-send-timeout-ms)

;;; Ctx -- a linear handle to an nng per-request context.
;;; A context runs the protocol's state machine independently of the
;;; socket's global context, so a single REQ socket can have multiple
;;; requests in flight. nng_socket_close blocks while contexts are open,
;;; so ctx-close must happen before close.
;;;
;;; Since: Phase NG-C
(defopaque Ctx :int :linear)

;;; ctx-open -- create a context on a socket.
;;; Returns NNG_ENOTSUP if the protocol does not support separate contexts.
(defn ctx-open [^borrow s : Socket] : (Result Ctx int)
  ```c
  #include <nng/nng.h>
  nng_socket sk;
  sk.id = (uint32_t)s;
  nng_ctx ctx;
  int rv = nng_ctx_open(&ctx, sk);
  if (rv != 0) return tur_err_int((int64_t)rv);
  return tur_ok_int((int64_t)ctx.id);
  ```)

;;; ctx-close -- close the context. The single linear consumer.
;;; Must be called before the parent socket's close (nng_socket_close
;;; blocks while contexts are open).
(defn ctx-close [c : Ctx] : nil
  ```c
  #include <nng/nng.h>
  nng_ctx ctx;
  ctx.id = (uint32_t)c;
  nng_ctx_close(ctx);
  ```)

;;; ctx-send -- submit an async send on the context. Collect via the Aio.
(defn ctx-send [^borrow c : Ctx ^borrow a : Aio] #fx{Net} : nil
  ```c
  #include <nng/nng.h>
  nng_ctx ctx;
  ctx.id = (uint32_t)c;
  nng_ctx_send(ctx, (nng_aio *)(intptr_t)a);
  ```)

;;; ctx-recv -- submit an async receive on the context. Collect via the Aio.
(defn ctx-recv [^borrow c : Ctx ^borrow a : Aio] #fx{Net} : nil
  ```c
  #include <nng/nng.h>
  nng_ctx ctx;
  ctx.id = (uint32_t)c;
  nng_ctx_recv(ctx, (nng_aio *)(intptr_t)a);
  ```)

;;; ctx-set-recv-timeout-ms -- per-context receive timeout.
(defn ctx-set-recv-timeout-ms [^borrow c : Ctx ms : int] : (Result nil int)
  ```c
  #include <nng/nng.h>
  nng_ctx ctx;
  ctx.id = (uint32_t)c;
  int rv = nng_ctx_set_ms(ctx, NNG_OPT_RECVTIMEO, (nng_duration)ms);
  if (rv != 0) return tur_err_int((int64_t)rv);
  return tur_ok_int(0);
  ```)

;;; ctx-set-send-timeout-ms -- per-context send timeout.
(defn ctx-set-send-timeout-ms [^borrow c : Ctx ms : int] : (Result nil int)
  ```c
  #include <nng/nng.h>
  nng_ctx ctx;
  ctx.id = (uint32_t)c;
  int rv = nng_ctx_set_ms(ctx, NNG_OPT_SENDTIMEO, (nng_duration)ms);
  if (rv != 0) return tur_err_int((int64_t)rv);
  return tur_ok_int(0);
  ```)

) ;; end defmodule nng/ctx
```

Canonical concurrent REQ (multiple in-flight requests on one socket):

```turmeric
(import nng/socket :refer [req-open dial close])
(import nng/ctx    :refer [ctx-open ctx-close ctx-send ctx-recv])
(import nng/aio    :refer [aio-alloc aio-free aio-wait aio-result])

(let [req (ok-val (req-open))
      _   (dial req "inproc://demo")
      c1  (ok-val (ctx-open req))
      c2  (ok-val (ctx-open req))
      a1  (ok-val (aio-alloc))
      a2  (ok-val (aio-alloc))]
  ;; Two requests in flight on one REQ socket, each on its own context.
  (ctx-send c1 a1)
  (ctx-send c2 a2)
  (aio-wait a1)
  (aio-wait a2)
  (ctx-recv c1 a1)
  (ctx-recv c2 a2)
  (aio-wait a1)
  (aio-wait a2)
  (aio-free a1)
  (aio-free a2)
  (ctx-close c1)
  (ctx-close c2)
  (close req))
```

---

## Phases

### NG-A -- Pollable FDs + Non-blocking Try

**Tasks**
- Add `recv-fd` / `send-fd` to `nng/socket.tur`. Two small inline-C functions
  over `nng_socket_get_recv_poll_fd` / `nng_socket_get_send_poll_fd`.
- Add `try-recv-str` / `try-recv-payload` to `nng/msg.tur`. Same body as the
  blocking variants but with `NNG_FLAG_NONBLOCK` added to the flags, and
  `tur_none()` on `NNG_EAGAIN` instead of `tur_err_int`.
- Add `recv-fd`, `send-fd`, `try-recv-str`, `try-recv-payload` to the
  `:exports` map in `build.tur`.
- Tests: a reactor-driven pub/sub round trip (register `recv-fd` on the SUB
  socket, run the reactor, assert the message arrives in the callback); a
  `try-recv-str` on an empty queue returns `(none)` without blocking.
- No new `errors/` fixtures (no new linear types).

**Acceptance**
- `tur test tests/nng` green, including the new reactor and try-recv tests.
- A reactor-driven receive loop works end to end over `inproc://`.
- `try-recv-str` on an empty queue returns immediately with `(none)`.

### NG-B -- `nng_aio` Async Send/Recv

**Tasks**
- New module `spices/nng/src/nng/aio.tur` with the `Aio` opaque and the
  operations above.
- Add `nng/aio` to `:exports` in `build.tur` with the exported names.
- The `Aio` opaque is `:ptr<void>` (nng's `nng_aio *`), `:linear`, consumed by
  `aio-free`. `aio-stop` is a borrow that stops + waits; call it before
  `aio-free` if an operation might be in flight.
- The send path uses `nng_aio_set_iov` with a single `nng_iov` pointing at
  the `Payload`'s data region -- no `nng_msg` allocation. The recv path uses
  `nng_aio_get_msg` + copy into `Payload` + `nng_msg_free`, the same
  copy-then-free pattern the blocking path uses.
- Tests: an async send/recv round trip (submit, wait, check result, extract
  payload); a timeout test (`aio-set-timeout` with a short timeout on a quiet
  socket, assert `aio-result` is `NNG_ETIMEDOUT`); a cancel test (submit,
  `aio-cancel`, wait, assert `aio-result` is `NNG_ECANCELED`).
- `errors/` fixtures: `aio-double-free` (free the same Aio twice ->
  TUR-E0101), `aio-use-after-free` (operate on a freed Aio -> TUR-E0101),
  `aio-leak-no-close` (allocate and never free -> TUR-E0100).

**Acceptance**
- `tur test tests/nng` green, including the new aio tests.
- An async round trip works end to end over `inproc://`.
- `aio-set-timeout` produces `NNG_ETIMEDOUT` on a quiet socket.
- `aio-cancel` produces `NNG_ECANCELED`.
- All three `errors/` fixtures fail to compile with the expected diagnostic.

### NG-C -- `nng_ctx` Concurrent Contexts

**Tasks**
- New module `spices/nng/src/nng/ctx.tur` with the `Ctx` opaque and the
  operations above.
- Add `nng/ctx` to `:exports` in `build.tur` with the exported names.
- The `Ctx` opaque is `:int` (packs `nng_ctx.id`, same trick as `Socket`),
  `:linear`, consumed by `ctx-close`.
- `ctx-open` returns `NNG_ENOTSUP` for protocols that lack contexts; surface
  that as a runtime `err`, the same shape "receive on a PUB socket" takes.
- Tests: two concurrent REQ requests on one socket via two contexts, both
  answered; `ctx-open` on a protocol without contexts returns `err` with
  `NNG_ENOTSUP`.
- `errors/` fixtures: `ctx-double-close`, `ctx-use-after-close`,
  `ctx-leak-no-close`.

**Acceptance**
- `tur test tests/nng` green, including the new ctx tests.
- Two in-flight REQ requests on one socket complete via two contexts.
- `ctx-open` on a PUB socket returns `err` with `NNG_ENOTSUP`.
- All three `errors/` fixtures fail to compile with the expected diagnostic.

---

## Dependency graph

```
NG-A (reactor fds + try-recv)     [independent]

NG-B (nng_aio)                   [independent]
          |
          v
NG-C (nng_ctx)                   [depends on NG-B]
```

NG-A and NG-B can land in either order or in parallel. NG-C depends on NG-B
because `ctx-send` / `ctx-recv` take an `Aio`.

---

## Implementation Notes

- **Handle packing.** `nng_ctx` is `struct { uint32_t id; }` -- the same
  by-value packing as `nng_socket`. The `Ctx` opaque carries the id in its
  int64 and each inline-C body reconstitutes `nng_ctx ctx; ctx.id =
  (uint32_t)c;` at the call boundary, exactly as `Socket` does. `nng_aio *`
  is an opaque pointer, so `Aio` is `:ptr<void>` -- no packing trick needed.

- **NULL-callback model.** `nng_aio_alloc(&aio, NULL, NULL)` creates an Aio
  with no completion callback. The operation completes silently; the caller
  collects via `nng_aio_wait` (blocks) or polls `nng_aio_busy` +
  `nng_aio_result`. This is the only model that works without a C callback
  trampoline, because nng's completion callback fires on an arbitrary worker
  thread and Turmeric fat closures cannot safely cross that boundary. The
  reactor path (NG-A) is the callback-driven event-loop story; the aio path
  is the submit/collect story.

- **Send via `nng_aio_set_iov`, not `nng_msg`.** `nng_socket_send` accepts an
  `nng_aio *` whose payload is set via `nng_aio_set_iov`. A single `nng_iov`
  pointing at the `Payload`'s data region (`turnng_payload_data` +
  `turnng_payload_len`) is the simplest path and avoids `nng_msg_alloc` /
  `nng_msg_append` / `nng_aio_set_msg`. nng copies the iov data, so the
  `Payload` is borrowed, not consumed -- the same ownership contract as the
  blocking `send-payload`.

- **Recv via `nng_aio_get_msg` + copy.** `nng_socket_recv` completes by
  associating an `nng_msg *` with the Aio, retrieved via `nng_aio_get_msg`.
  `aio-recv-payload` copies `nng_msg_body` / `nng_msg_len` into a fresh
  `Payload` via `turnng_payload_of_bytes` and calls `nng_msg_free`
  immediately -- the same copy-then-free pattern the blocking `recv-payload`
  uses with `nng_recv` + `NNG_FLAG_ALLOC`. No nng allocator ownership escapes
  into Turmeric code.

- **`NNG_FLAG_NONBLOCK` is `2u`.** The try-recv variants OR it into the flags
  passed to `nng_recv`. nng returns `NNG_EAGAIN` (error code 8) when the
  queue is empty, which the wrapper turns into `tur_none()`. This is distinct
  from `NNG_ETIMEDOUT` (5): a timed-out receive is an `err`, a "nothing yet"
  receive is `(none)`. The caller does not need to learn either code.

- **Pollable fd semantics.** `nng_socket_get_recv_poll_fd` hands back a
  level-triggered fd: it stays readable as long as a message is available.
  The reactor callback fires once per readiness edge; after consuming the
  message with `recv-str` / `recv-payload`, the fd goes quiet until the next
  message arrives. The fd is not `close()`d by the spice -- it is owned by
  nng and lives as long as the socket.

- **`nng_socket_close` blocks while contexts are open.** This is documented
  in nng's header and is why `ctx-close` must happen before `close`. The
  `errors/` fixture for a leaked `Ctx` (TUR-E0100) catches the common case;
  a program that closes the socket first and then tries to close the context
  gets a runtime `NNG_ECLOSED` from `nng_ctx_close`, which is the right
  failure mode.

- **No `#fx{Net}` on `aio-alloc` / `aio-free` / `aio-wait` / `aio-busy?` /
  `aio-result` / `aio-set-timeout`.** These are local operations on the Aio
  handle, not network I/O. The network effect is in `send-aio` / `recv-aio` /
  `ctx-send` / `ctx-recv`, which carry `#fx{Net}`.

- **`aio-stop` vs `aio-free`.** `nng_aio_free` must not be called on an Aio
  that is in use. `nng_aio_stop` stops any outstanding operation and waits
  for it to complete, so the safe teardown sequence is `aio-stop` then
  `aio-free` when an operation might still be in flight. If the caller knows
  the operation has completed (e.g., after `aio-wait`), `aio-free` alone is
  enough. `aio-stop` is a borrow, not a consume, so it can be called
  multiple times safely.

- **`build.tur` exports.** Each new module adds its names to the `:exports`
  map. The `nng/aio` and `nng/ctx` entries are new rows; the `nng/socket` and
  `nng/msg` entries gain the new names. No `:cmake-deps` change -- nng is
  already fetched and built with the aio and ctx APIs compiled in (they are
  core, not optional).

---

## Risks and open questions

1. **`nng_aio` lifetime vs `Aio` linearity.** `nng_aio_free` on an in-use Aio
   is undefined. The `:linear` discipline makes double-free and use-after-free
   compile-time errors under `-Xsubstructural`, but it cannot prevent
   `aio-free` on an Aio whose operation is still in flight -- that is a
   runtime ordering bug. `aio-stop` exists as the safe pre-free step; document
   it prominently. Consider whether `aio-free` should internally call
   `nng_aio_stop` (defensive, but changes the contract from "must not be in
   use" to "stops for you" -- nng's own `nng_aio_free` does not stop, so this
   would be a spice-side safety net). Recommendation: do not auto-stop; match
   nng's contract and document `aio-stop` as the caller's responsibility.

2. **`nng_aio_set_iov` guarantee.** nng guarantees `nng_aio_set_iov` succeeds
   for `n <= 4` iov entries; this plan uses exactly 1. If it ever fails
   (returns non-zero), the send would submit with no payload. The
   `send-aio` body should check the return and surface it -- but
   `send-aio` returns `nil`, not `(Result ...)`, because the result is
   collected later via `aio-result`. If `nng_aio_set_iov` fails, set the
   Aio's result to the error code without submitting. Open question: can
   `nng_aio_set_iov` fail for `n = 1`? The header says "guaranteed to
   succeed if n <= 4," so in practice no -- but a defensive check costs
   nothing and avoids a silent no-payload send.

3. **Reactor fd and `NNG_FLAG_NONBLOCK` interaction.** The reactor path
   (register `recv-fd`, call blocking `recv-str` in the callback) and the
   try-recv path (`NNG_FLAG_NONBLOCK`) are two ways to do the same thing.
   The reactor path is for event loops; the try-recv path is for polling
   loops. They should not be mixed on the same socket: a reactor callback
   that calls `try-recv-str` would get `(none)` if another consumer already
   dequeued the message between the fd becoming readable and the callback
   firing. Document that a socket is either reactor-driven or polled, not
   both.

4. **`nng_ctx` protocol support.** Not all protocols support contexts. REQ,
   REP, SURVEYOR, RESPONDENT do; PUB, SUB, PUSH, PULL, PAIR, BUS do not (or
   do not benefit -- they have no per-request state). `ctx-open` returns
   `NNG_ENOTSUP` for the unsupported ones. The plan surfaces that as a
   runtime `err`, matching the v0 convention for protocol mismatches. A
   future per-protocol typed-sockets plan (separate NG5 item) would make this
   a compile error instead.

5. **`aio-recv-payload` and `NNG_ESTATE`.** If the caller calls
   `aio-recv-payload` before `aio-wait` returns, or after a failed
   `aio-result`, `nng_aio_get_msg` returns NULL. The wrapper returns
   `tur_err_int(NNG_ESTATE)` (error code 11) -- "operation in wrong state."
   This is a programming error, not a network condition; document it as
   "call only after `aio-wait` and a successful `aio-result`."

6. **Thread safety of `Aio`.** An `nng_aio` is not thread-safe: only one
   thread should operate on a given Aio at a time. The `:linear` discipline
   ensures only one caller holds the handle, but it does not prevent the
   caller from passing a borrowed `Aio` to two threads. This is the same
   constraint as the blocking `Socket` (one socket, one thread at a time),
   and the same documentation answer: an `Aio` is used by one thread, even
   if the operation it submits runs on nng's worker pool.

---

## Acceptance (whole plan)

- A reactor-driven receive loop multiplexes nng sockets in a single-threaded
  event loop via `recv-fd` + `stdlib/reactor.tur`.
- `try-recv-str` / `try-recv-payload` return `(none)` immediately on an empty
  queue, without blocking.
- An async send/recv round trip works via `aio-alloc` / `send-aio` /
  `recv-aio` / `aio-wait` / `aio-result` / `aio-recv-payload`.
- `aio-set-timeout` and `aio-cancel` produce `NNG_ETIMEDOUT` and
  `NNG_ECANCELED` respectively.
- Two in-flight REQ requests on one socket complete via two `nng_ctx`
  contexts.
- `ctx-open` on a protocol without contexts returns `err` with
  `NNG_ENOTSUP`.
- All `errors/` fixtures (aio and ctx, three each) fail to compile with the
  expected linear-lifecycle diagnostics.
- The blocking surface from v0 is unchanged; all existing tests pass.
- README updated with the async surface, the reactor integration example,
  and the "which async path" guidance (reactor for event loops, aio for
  submit/collect, ctx for concurrent REQ).

---

## See Also

- `docs/archive/nng-spice-plan.md` -- the v0 plan; NG5 follow-ups section.
- `spices/nng/README.md` -- the shipped surface, "Not in v0" section.
- `stdlib/reactor.tur` -- `reactor-add-fd`, the reactor integration seam.
- `stdlib/thread.tur` -- the v0 concurrency companion (blocking on threads).
- `spices/thread-pool/` -- `Future<R>` fan-out, the aio collect pattern's
  natural companion.
- `spices/valkey/` -- the structural model for linear handles and error
  fixtures.
