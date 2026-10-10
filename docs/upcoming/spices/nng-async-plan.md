# Plan: `tur-nng` Async Interface (NG5: poll fds, aio, contexts)

> Status: draft, revision 2 (2026-10-10). Not started in the spice. The design
> below was spiked end to end against tur v0.63.9 and nng v1.12.4 (see
> "Spike record"). The spike code was not committed.
> Tracks: `docs/archive/nng-spice-plan.md` NG5 follow-ups (aio + ctx, reactor fd).
> Scope: `spices/nng/` in turmeric-spices. No compiler change is needed. Two
> compiler/stdlib defects were found on the way and are fixed (see "Found on
> the way").
> Builds on: `tur-nng` 0.1.0 (NG0-NG4, shipped), nng pinned v1.12.4.
> Needs: tur >= v0.63.7 for `(Result (Option T) E)` from inline C (the nested
> builders fix).
> Type: Networking / spice (turmeric-spices).

---

## What revision 2 changed

Revision 1 was written from nng's 2.0-era docs and from how the reactor was
assumed to behave. Checking it against the pinned v1.12.4 source and the
actual reactor backends broke most of its code. This table lists every change
of substance, so a reviewer of revision 1 can see what moved and why.

| # | Revision 1 said | What is true (v1.12.4 / tur v0.63.9) | Change |
| --- | --- | --- | --- |
| 1 | `nng_socket_get_recv_poll_fd` / `_send_poll_fd` | Not in 1.12.4. The fds are read with `nng_socket_get_int(s, NNG_OPT_RECVFD / NNG_OPT_SENDFD, &fd)` | NG-A bodies rewritten |
| 2 | The send fd "becomes writable" | The send fd becomes **readable** when a send would not block (`nng_options.5`) | Register both fds for `READ` |
| 3 | The fd is level-triggered, so the callback fires again | The epoll backend is level-triggered, but the kqueue backend registers with `EV_CLEAR` (edge-triggered, `src/async/io_kqueue.c:139`). nng writes one byte per empty-to-ready transition | A callback must **drain to empty**, or it stalls on macOS |
| 4 | Call blocking `recv-str` in the reactor callback | A blocking call parks the reactor thread if another consumer won the race | Callback uses `try-recv-*` only |
| 5 | `try-recv-*` returns `(none)` on any failure | That hides `NNG_ECLOSED` and every other real error | `(Result (Option T) int)`: none means only `NNG_EAGAIN` |
| 6 | (strategy listed `try-send`, API had none) | -- | `try-send-str` / `try-send-payload` added |
| 7 | `nng_socket_send(sk, aio)` / `nng_socket_recv` | Not in 1.12.4. The names are `nng_sock_send` / `nng_sock_recv` (aliases `nng_send_aio` / `nng_recv_aio`) | NG-B bodies rewritten |
| 8 | Send through `nng_aio_set_iov`, no `nng_msg` | A socket aio send with no message attached fails with `NNG_EINVAL` (`src/nng.c:249`). The iov is for byte streams | Send builds an `nng_msg` |
| 9 | (no handling) | A failed send leaves the message with the caller (`nng_send_aio.3`) | The spice's C callback frees it |
| 10 | NULL completion callback; collect with wait or `aio-busy?` | That cannot feed the reactor or a fiber without blocking or spinning | Spice-owned C callback plus a completion pipe (`aio-poll-fd`). No Turmeric code runs on nng's threads |
| 11 | `aio-stop` before `aio-free`; reuse after stop | `nng_aio_stop` is terminal: later submits fail. `nng_aio_free` already cancels and waits (`nng_aio_free.3`, `nni_aio_fini`) | `aio-stop` dropped; `aio-free` is safe in flight |
| 12 | A stopped op reports `NNG_ESTOPPED` | There is no `NNG_ESTOPPED` in 1.x. Stop and cancel report `NNG_ECANCELED` | Text fixed |
| 13 | `aio-recv-payload` frees the message | It left the freed pointer attached to the aio, so a second call double-frees | Take detaches the message first. A second take is `NNG_ESTATE` |
| 14 | Submits return `nil` | Submitting on a busy aio trips an nng assertion (undefined in release builds) | Submits return `(Result nil int)` and refuse a busy aio with `NNG_EBUSY` |
| 15 | SUB has no contexts | REQ, REP, **SUB**, SURVEYOR and RESPONDENT have contexts. PUB, PUSH, PULL, PAIR and BUS give `NNG_ENOTSUP` | Table fixed; `ctx-subscribe` added |
| 16 | NG-C depends on NG-B (contexts are async-only) | `nng_ctx_sendmsg` / `nng_ctx_recvmsg` exist: contexts have blocking ops too | NG-C's blocking half has no dependency |
| 17 | `nng_close` blocks while contexts are open | The header says so. The 1.12.4 code reaps idle contexts and waits only for in-progress API calls. A later `nng_ctx_close` returns `NNG_ECLOSED` (spiked) | `ctx-close` returns `(Result nil int)`; order is advice, not a deadlock |
| 18 | Opaque named `Ctx` | `tourist/types.tur` already owns `Ctx`, and opaque names resolve globally (the reason `Buf` became `Payload`) | `NngCtx` |
| 19 | An `Aio` is one-thread, like a `Socket` | nng socket calls take the socket's locks, so they are safe across threads. What is not shareable is a stateful protocol's one global context: a second pending REP receive is `NNG_ESTATE` (`rep.c:438`), and a second REQ send cancels the first | Motivation for NG-C rewritten |
| 20 | Examples | The aio example imported `recv-aio` from `nng/msg`. The ctx example sent with no message. The reactor loop never terminated. Every example passed `nil` as `ptr<void>` user-data, which did not compile on v0.63.9 (fixed since; see "Found on the way") | All examples rewritten and spiked |
| 21 | Poll fds and contexts can coexist | nng documents poll fds as unsupported on a socket with contexts in use (`nng_ctx.5`) | Stated as a per-socket rule |

---

## Overview

`tur-nng` 0.1.0 ships a blocking-only surface: one linear `Socket`, blocking
`send-str` / `recv-str` / `send-payload` / `recv-payload`, and an owned
`Payload`. Its concurrency story is "blocking calls on OS threads." The
archived plan deferred two async seams as NG5:

1. **Reactor integration.** nng exposes pollable fds that plug into
   `stdlib/reactor.tur`.
2. **`nng_aio` + `nng_ctx`.** nng's own async operations, and per-request
   contexts that run protocol state machines concurrently on one socket.

This plan graduates both as three phases:

```
NG-A  poll fds + try ops    readiness: "a receive would not block now"
      (nng/socket, nng/msg)  -> reactor-add-fd, then try-recv-* until empty

NG-B  Aio                    completion: "this operation has finished"
      (nng/aio, net-new)     -> aio-wait | aio-try-result | aio-poll-fd

NG-C  NngCtx                 independent protocol state on one socket
      (nng/ctx, net-new)     -> blocking ctx ops (threads) | ctx aio ops (NG-B)
```

NG-A is readiness-based and NG-B is completion-based. Both end at a pollable
fd, so both compose with the same consumers:

| Consumer | Readiness (NG-A) | Completion (NG-B) |
| --- | --- | --- |
| A plain thread | `try-recv-*` in a loop | `aio-wait` |
| `tur/reactor` callback | `reactor-add-fd (poll-fd->int (recv-poll-fd s)) READ` | `reactor-add-fd (poll-fd->int (aio-poll-fd a)) READ` |
| `LocalFiberGroup` fiber (direct style) | `local-park-fd g fd READ ms` | `local-park-fd g fd READ ms` |
| Global scheduler `async`/`await` | follow-up (see below) | follow-up |

All three are new API. None changes the blocking surface.

---

## Motivation

### Reactor integration (NG-A)

The blocking surface costs one OS thread per concurrent socket operation. A
fan-in aggregator, a survey collector, or a pair relay wants one event loop
instead. `stdlib/reactor.tur` already provides one, and nng hands out the
seam: `NNG_OPT_RECVFD` / `NNG_OPT_SENDFD` give the read end of a pipe that nng
raises when the socket becomes ready and clears when it drains
(`src/core/pollable.c`). The callback is an ordinary Turmeric closure, and it
runs on the reactor's own thread. No new ownership model is involved.

### `nng_aio` (NG-B)

Readiness multiplexes *receives* on sockets. It does not give you a timed,
cancellable, in-flight *operation*, or several operations outstanding at once.
`nng_aio` does: submit, then collect later. Per-operation timeouts and cancel
come with it, and N submits in flight on one thread are the fan-out.

The hard part is completion. nng runs a completion callback on an arbitrary
worker thread with no locks held, so a Turmeric fat closure must never run
there. Revision 1 avoided the callback entirely, which left the caller only
two options: block in `aio-wait`, or spin on `aio-busy?`. Neither composes
with a reactor or a fiber. Revision 2 installs **the spice's own C callback**.
It does three things, none of which touches Turmeric code:

1. It frees the message of a failed send.
2. It marks the operation complete.
3. It writes one byte to a lazily created completion pipe.

That pipe's read end is `aio-poll-fd`. A reactor or a fiber parks on it, and
the collect then runs on the consumer's thread. This gives callback-driven
async with no cross-thread closure and no stdlib change.

### `nng_ctx` (NG-C)

A stateful protocol's socket-level operations share one global context:

- **REQ**: a second request cancels the first (`req.c:704`, `NNG_ECANCELED`).
- **REP**: a second pending receive fails with `NNG_ESTATE` (`rep.c:438`). So
  N REP worker threads on **one** REP socket do not work today. The README's
  "one thread per REP worker" holds only for one worker per socket.

`nng_ctx` gives each request its own state machine on a shared socket. The
classic concurrent server is N contexts on one REP socket, each with its own
receive-then-reply cycle. nng 1.12.4 also has blocking context operations
(`nng_ctx_sendmsg` / `nng_ctx_recvmsg`), so that server works as N
`stdlib/thread.tur` threads with no aio at all. The aio versions add the
single-threaded form: many in-flight requests or replies driven by one thread.

---

## Non-goals

- **Per-protocol typed sockets.** This is its own NG5 item. "No recv fd on
  PUB" and "no contexts on PUSH" stay runtime `err`s carrying `NNG_ENOTSUP`,
  the same shape "receive on a PUB socket" already has.
- **Global-scheduler `await` on nng.** See "Follow-ups". The `LocalFiberGroup`
  path gives direct-style code today; the global scheduler has no public
  fd-park primitive.
- **Zero-copy `nng_msg` as a public type.** Messages are copied into and out
  of `Payload`, so the spice keeps one ownership model.
- **TLS / WebSocket / ZeroTier**, **explicit dialer/listener handles**, and
  **the nng 2.0 line**: unchanged from v0.
- **Windows.** Spices CI covers Linux and macOS. The completion pipe and the
  reactor's IOCP backend have not been looked at together. See risk 7.

---

## Design decisions

**D1 -- A poll fd is a `PollFd`, not an `:int`.** nng's poll fds, and the
spice's completion fd, may only be polled. Reading, writing or closing one
corrupts nng's state (`nng_options.5`: "Applications should never attempt to
read or write to the returned file descriptor"). That makes it a distinct
kind of thing, so it gets `(defopaque PollFd :int)`. It is non-linear, because
its owner closes it. `poll-fd->int` is the explicit step to the reactor's
`:int` fd parameter. This is the CLAUDE.md "no lazy `:int`" rule applied to a
case where the stdlib `Fd` would be wrong: `Fd` invites `close`.

**D2 -- The drain rule.** A readiness callback calls `try-recv-*` until it
returns `(ok (none))`. It never calls a blocking receive. Draining is required
on kqueue, where `EV_CLEAR` reports one edge per empty-to-ready transition, and
harmless on epoll. Never blocking keeps a lost race from parking the loop.

**D3 -- `try-*` result shapes.**
- `try-recv-*` returns `(Result (Option T) int)`: `(ok (some v))` is a
  message, `(ok (none))` means nothing is ready (nng's `NNG_EAGAIN` from
  `NNG_FLAG_NONBLOCK`, `src/nng.c:152`), and `(err code)` is a real failure.
- `try-send-*` returns `(Result bool int)`: `(ok true)` means sent, `(ok false)`
  means it would block, and `(err code)` is a real failure.

The caller never learns `NNG_EAGAIN`.

**D4 -- `Aio` is a spice-owned box, not a bare `nng_aio *`.** `turnng_aio`
(`c/nng/turnng_aio.c`) holds the `nng_aio *`, the in-flight operation kind,
a completion flag, and the lazy pipe. The box is what lets the spice give
these guarantees:

- No `nng_msg` ever reaches Turmeric code or leaks. A failed send's message
  is freed by the callback. An untaken receive is freed by the next submit or
  by `aio-free`.
- A busy `Aio` is never resubmitted: `NNG_EBUSY`, rather than nng's assertion.
- A message is taken at most once: `NNG_ESTATE` on the second take.

**D5 -- Submit, then collect.** Submits return `(Result nil int)`. An `err`
means nothing was submitted. An `ok` means the outcome arrives through a
collector:

- `aio-wait`: blocks, returns `(Result nil int)`.
- `aio-try-result`: returns `(Option (Result nil int))`, with `none` while the
  operation is in flight.
- `aio-take-payload` / `aio-take-str`: after a successful receive, returns
  `(Result Payload int)` / `(Result cstr int)`. It detaches the message.

`aio-poll-fd` is readable from completion until the result is collected (any
of the three above observing completion) or until the next submit. A
reactor-driven loop therefore neither spins on epoll nor stalls on kqueue.

**D6 -- Timeouts.** A fresh `Aio` uses `NNG_DURATION_DEFAULT`, so the
operation inherits the socket's (or context's) `set-recv-timeout-ms` /
`set-send-timeout-ms` (`nni_aio_normalize_timeout`, `socket.c:843`). A context
copies its socket's timeouts at `ctx-open` (`socket.c:1367`).
`aio-set-timeout` overrides the inherited timeout, and the override persists
across reuse:

| Value | Meaning |
| --- | --- |
| `-2` (`NNG_DURATION_DEFAULT`) | inherit the socket or context timeout |
| `-1` | wait forever |
| `0` | poll |
| `ms > 0` | fail after `ms` milliseconds |

**D7 -- Contexts are named `NngCtx`, and come in two flavours.** The blocking
ops (`ctx-send-payload` and friends) need nothing from NG-B. The aio ops
(`ctx-send-payload-aio`, `ctx-recv-aio`) reuse NG-B's `Aio`. A socket uses
either poll fds (NG-A) or contexts (NG-C), never both (`nng_ctx.5`).

**D8 -- Teardown order.** Each step is advice, and each wrong order fails
safe:

1. Remove the reactor source for any `PollFd` before closing its owner. A
   stale epoll registration on a reused fd number is the hazard.
2. `aio-free` cancels and waits for an in-flight op.
3. `ctx-close` precedes `close`. The reverse order gives `NNG_ECLOSED` from
   `ctx-close`.

---

## API surface

Representative inline-C bodies are shown. Every other body follows the same
pattern as its sibling. Docstrings in the real modules follow the CLAUDE.md
`;;;` standard. They are trimmed here.

### NG-A -- poll fds and try ops (`nng/socket.tur`, `nng/msg.tur`)

```turmeric
;; nng/socket.tur -- additions

;;; PollFd -- an fd that may only be polled (never read, written or closed).
(defopaque PollFd :int)

(defn poll-fd->int [fd : PollFd] : int (:: fd :int))

;;; recv-poll-fd -- readable while a message is waiting.
;;; err NNG_ENOTSUP on a protocol that cannot receive (PUB, PUSH).
(defn recv-poll-fd [^borrow s : Socket] : (Result PollFd int)
  ```c
  #include <nng/nng.h>
  nng_socket sk;
  sk.id = (uint32_t)s;
  int fd = -1;
  int rv = nng_socket_get_int(sk, NNG_OPT_RECVFD, &fd);
  if (rv != 0) return tur_err_int((int64_t)rv);
  return tur_ok_int((int64_t)fd);
  ```)

;;; send-poll-fd -- READABLE (not writable) while a send would not block.
;;; err NNG_ENOTSUP on a protocol that cannot send (SUB, PULL).
(defn send-poll-fd [^borrow s : Socket] : (Result PollFd int)
  ...)  ;; same body with NNG_OPT_SENDFD
```

```turmeric
;; nng/msg.tur -- additions

;;; try-recv-str -- receive without blocking.
;;; (ok (some s)) a message; (ok (none)) nothing ready; (err code) a failure.
(defn try-recv-str [^borrow s : Socket] #fx{Net} : (Result (Option cstr) int)
  ```c
  #include <nng/nng.h>
  nng_socket sk;
  sk.id = (uint32_t)s;
  void  *data = NULL;
  size_t len  = 0;
  int rv = nng_recv(sk, &data, &len, NNG_FLAG_ALLOC | NNG_FLAG_NONBLOCK);
  if (rv == NNG_EAGAIN) return tur_ok_int(tur_none());
  if (rv != 0) return tur_err_int((int64_t)rv);
  char *out = (char *)malloc(len + 1);
  if (out == NULL) { nng_free(data, len); return tur_err_int((int64_t)NNG_ENOMEM); }
  if (len > 0) memcpy(out, data, len);
  out[len] = '\0';
  nng_free(data, len);
  return tur_ok_int(tur_some_ptr(out));
  ```)

(defn try-recv-payload [^borrow s : Socket] #fx{Net} : (Result (Option Payload) int)
  ...)  ;; recv-payload's body, NONBLOCK, the same EAGAIN arm

;;; try-send-str -- send without blocking.
;;; (ok true) sent; (ok false) would block; (err code) a failure.
(defn try-send-str [^borrow s : Socket msg : cstr] #fx{Net} : (Result bool int)
  ```c
  #include <nng/nng.h>
  nng_socket sk;
  sk.id = (uint32_t)s;
  const char *p = (const char *)msg;
  if (p == NULL) p = "";
  int rv = nng_send(sk, (void *)(uintptr_t)p, strlen(p), NNG_FLAG_NONBLOCK);
  if (rv == NNG_EAGAIN) return tur_ok_int(0);
  if (rv != 0) return tur_err_int((int64_t)rv);
  return tur_ok_int(1);
  ```)

(defn try-send-payload [^borrow s : Socket b : Payload] #fx{Net} : (Result bool int)
  ...)
```

Poll-fd support by protocol (v1.12.4):

| Protocol | `recv-poll-fd` | `send-poll-fd` |
| --- | --- | --- |
| REQ, REP, PAIR, BUS, SURVEYOR, RESPONDENT | yes | yes |
| PUB, PUSH | `NNG_ENOTSUP` | yes |
| SUB, PULL | yes | `NNG_ENOTSUP` |

Canonical reactor loop (spiked):

```turmeric
;; drain -- every ready message, never blocking. Required on kqueue (D2).
(defn drain [^borrow s : Socket n : int] : int
  (let [r (try-recv-str s)]
    (if (ok? r)
      (match (ok-val r)
        (Some m) (do (handle m) (cstr-free m) (drain s (+ n 1)))
        (None)   n)
      n)))

(let [sub (ok-val (sub-open))
      _   (dial sub "inproc://feed")
      _   (sub-subscribe sub "")
      r   (reactor-new)
      fd  (ok-val (recv-poll-fd sub))
      src (reactor-add-fd r (poll-fd->int fd) READ
            (fn [id events user] : nil
              (do (set! seen (drain sub seen))
                  (when (>= seen want) (reactor-stop r))))
            (:: 0 :ptr<void>))]   ;; `nil` from the release after v0.63.9
  (reactor-run r)
  (reactor-remove r src)
  (reactor-free r)
  (close sub))
```

### NG-B -- `Aio` (`nng/aio.tur`, net-new; `c/nng/turnng_aio.c`, net-new)

```turmeric
(defmodule nng/aio
  (import nng/socket  :refer [Socket PollFd])
  (import nng/payload :refer [Payload])
  (export Aio
          aio-alloc aio-free aio-set-timeout aio-cancel
          send-payload-aio send-str-aio recv-aio
          aio-wait aio-try-result aio-take-payload aio-take-str
          aio-poll-fd)

;;; Aio -- one nng asynchronous operation slot, owned by the spice (D4).
(defopaque Aio :ptr<void> :linear)

(defn aio-alloc [] : (Result Aio int)
  ```c
  extern void *turnng_aio_new(int *);
  int rv = 0;
  void *a = turnng_aio_new(&rv);
  if (a == NULL) return tur_err_int((int64_t)rv);
  return tur_ok_ptr(a);
  ```)

;;; aio-free -- the linear consumer. Cancels and waits for an in-flight op,
;;; frees any untaken message, closes the completion pipe.
(defn aio-free [a : Aio] : nil ...)

(defn aio-set-timeout [^borrow a : Aio ms : int] : nil ...)  ;; D6
(defn aio-cancel      [^borrow a : Aio] : nil ...)           ;; -> NNG_ECANCELED

;;; Submits. err = nothing submitted (NNG_EBUSY if `a` is in flight,
;;; NNG_ENOMEM); ok = collect later. The payload is BORROWED (copied into a
;;; fresh nng_msg), as in the blocking send-payload.
(defn send-payload-aio [^borrow s : Socket ^borrow a : Aio b : Payload] #fx{Net} : (Result nil int)
  ```c
  extern int turnng_aio_send_bytes(void *, uint32_t, uint32_t, int, const void *, size_t);
  extern int64_t turnng_payload_len(const void *);
  extern void   *turnng_payload_data(void *);
  void  *data = turnng_payload_data((void *)(intptr_t)b);
  size_t len  = (size_t)turnng_payload_len((const void *)(intptr_t)b);
  int rv = turnng_aio_send_bytes((void *)(intptr_t)a, (uint32_t)s, 0, 0, data, len);
  if (rv != 0) return tur_err_int((int64_t)rv);
  return tur_ok_int(0);
  ```)
(defn send-str-aio [^borrow s : Socket ^borrow a : Aio msg : cstr] #fx{Net} : (Result nil int) ...)
(defn recv-aio     [^borrow s : Socket ^borrow a : Aio]            #fx{Net} : (Result nil int) ...)

;;; Collectors (D5).
(defn aio-wait [^borrow a : Aio] : (Result nil int) ...)

(defn aio-try-result [^borrow a : Aio] : (Option (Result nil int))
  ```c
  extern int turnng_aio_try_result(void *);   /* -1 while in flight */
  int rv = turnng_aio_try_result((void *)(intptr_t)a);
  if (rv < 0) return tur_none();
  if (rv != 0) return tur_some_int(tur_err_int((int64_t)rv));
  return tur_some_int(tur_ok_int(0));
  ```)

;;; err: the receive's own failure code, or NNG_ESTATE if there is nothing to
;;; take (not a receive, still in flight, already taken).
(defn aio-take-payload [^borrow a : Aio] : (Result Payload int) ...)
(defn aio-take-str     [^borrow a : Aio] : (Result cstr int) ...)

;;; aio-poll-fd -- readable from completion until collected or resubmitted.
(defn aio-poll-fd [^borrow a : Aio] : (Result PollFd int) ...)

) ;; end defmodule nng/aio
```

The C box (`c/nng/turnng_aio.c`, the spiked shape; error paths trimmed):

```c
typedef struct turnng_aio {
    nng_aio        *aio;
    pthread_mutex_t mu;
    int             op;         /* NONE / SEND / RECV: set at submit */
    int             signalled;  /* completed, not yet collected or re-armed */
    int             rfd, wfd;   /* completion pipe; -1 until aio-poll-fd */
} turnng_aio;

/* Runs on an nng worker thread. Touches only C state -- never Turmeric. */
static void turnng_aio_cb(void *arg) {
    turnng_aio *a = arg;
    if (a->op == OP_SEND && nng_aio_result(a->aio) != 0) {   /* caller owns it */
        nng_msg *m = nng_aio_get_msg(a->aio);
        if (m) { nng_aio_set_msg(a->aio, NULL); nng_msg_free(m); }
    }
    pthread_mutex_lock(&a->mu);
    a->signalled = 1;
    if (a->wfd >= 0) { char b = 1; (void)!write(a->wfd, &b, 1); }
    pthread_mutex_unlock(&a->mu);
}

/* Every submit: refuse a busy aio, free an untaken receive, drain the pipe. */
static int turnng_aio_arm(turnng_aio *a, int op) {
    if (nng_aio_busy(a->aio)) return NNG_EBUSY;
    drop_attached_msg(a);
    pthread_mutex_lock(&a->mu);
    if (a->signalled && a->rfd >= 0) drain_pipe(a->rfd);
    a->signalled = 0;
    a->op = op;
    pthread_mutex_unlock(&a->mu);
    return 0;
}

/* Free: stop (waits for the callback), drop any message, then free. Stop
 * must come first -- the callback may run during the cancel and it touches
 * mu / wfd. */
void turnng_aio_free(void *p) {
    turnng_aio *a = p;
    nng_aio_stop(a->aio);
    drop_attached_msg(a);
    nng_aio_free(a->aio);
    if (a->rfd >= 0) { close(a->rfd); close(a->wfd); }
    pthread_mutex_destroy(&a->mu);
    free(a);
}
```

The remaining pieces of the box:

- **Send** is `nng_msg_alloc(&m, 0)`, then `nng_msg_append(m, data, len)`,
  `nng_aio_set_msg`, and `nng_sock_send` (or `nng_ctx_send` for a context).
- **Receive** is `nng_sock_recv` (or `nng_ctx_recv`).
- **Take** copies the message body into a fresh `Payload` with
  `turnng_payload_of_bytes`. Then it calls `nng_aio_set_msg(aio, NULL)`, frees
  the message, and clears the signal.
- **The completion pipe** is created on the first `aio-poll-fd`, under `mu`.
  It is pre-raised if the operation already completed. Both ends are
  non-blocking and close-on-exec.

The spike implemented clearing the signal on submit only. Clearing it on a
collect (D5) is the one delta from the spiked code.

Canonical uses (each spiked):

```turmeric
;; 1. Plain thread: submit, collect.
(let [a (ok-val (aio-alloc))]
  (recv-aio pull a)
  (when (ok? (aio-wait a))
    (let [b (ok-val (aio-take-payload a))]
      (use b)
      (payload-free b)))
  (aio-free a))

;; 2. Reactor: completion fd in the loop.
(recv-aio pull a)
(reactor-add-fd r (poll-fd->int (ok-val (aio-poll-fd a))) READ
  (fn [id events user] : nil
    (do (let [t (aio-take-payload a)]
          (when (ok? t) (do (use (ok-val t)) (payload-free (ok-val t)))))
        (recv-aio pull a)))       ;; re-arm for the next message
  (:: 0 :ptr<void>))

;; 3. Direct style in a LocalFiberGroup fiber: no callback in sight.
(local-spawn g
  (fn [u : ptr<void>] : nil
    (do (recv-aio pull a)
        (local-park-fd g (poll-fd->int (ok-val (aio-poll-fd a))) READ 2000)
        (let [t (aio-take-payload a)] ...)))
  (:: 0 :ptr<void>))
```

### NG-C -- `NngCtx` (`nng/ctx.tur`, net-new)

```turmeric
(defmodule nng/ctx
  (import nng/socket  :refer [Socket])
  (import nng/payload :refer [Payload])
  (import nng/aio     :refer [Aio])
  (export NngCtx ctx-open ctx-close
          ctx-send-payload ctx-recv-payload ctx-send-str ctx-recv-str
          ctx-send-payload-aio ctx-send-str-aio ctx-recv-aio
          ctx-set-recv-timeout-ms ctx-set-send-timeout-ms
          ctx-subscribe ctx-unsubscribe)

;;; NngCtx -- independent protocol state on a shared socket. `nng_ctx` is a
;;; by-value { uint32_t id }, packed like Socket. Named NngCtx because
;;; tourist owns `Ctx` and opaque names resolve globally.
(defopaque NngCtx :int :linear)

;;; ctx-open -- err NNG_ENOTSUP on PUB, PUSH, PULL, PAIR, BUS.
(defn ctx-open [^borrow s : Socket] : (Result NngCtx int)
  ```c
  #include <nng/nng.h>
  nng_socket sk;
  sk.id = (uint32_t)s;
  nng_ctx c;
  int rv = nng_ctx_open(&c, sk);
  if (rv != 0) return tur_err_int((int64_t)rv);
  return tur_ok_int((int64_t)c.id);
  ```)

;;; ctx-close -- the linear consumer. NNG_ECLOSED if the socket closed first
;;; (harmless: the socket already reaped it).
(defn ctx-close [c : NngCtx] : (Result nil int) ...)

;;; Blocking ops: nng_ctx_sendmsg / nng_ctx_recvmsg, copy semantics as v0.
(defn ctx-send-payload [^borrow c : NngCtx b : Payload] #fx{Net} : (Result nil int) ...)
(defn ctx-recv-payload [^borrow c : NngCtx]             #fx{Net} : (Result Payload int) ...)
(defn ctx-send-str     [^borrow c : NngCtx msg : cstr]  #fx{Net} : (Result nil int) ...)
(defn ctx-recv-str     [^borrow c : NngCtx]             #fx{Net} : (Result cstr int) ...)

;;; Aio ops: the NG-B submit/collect contract, on the context.
(defn ctx-send-payload-aio [^borrow c : NngCtx ^borrow a : Aio b : Payload] #fx{Net} : (Result nil int) ...)
(defn ctx-send-str-aio     [^borrow c : NngCtx ^borrow a : Aio msg : cstr]  #fx{Net} : (Result nil int) ...)
(defn ctx-recv-aio         [^borrow c : NngCtx ^borrow a : Aio]             #fx{Net} : (Result nil int) ...)

(defn ctx-set-recv-timeout-ms [^borrow c : NngCtx ms : int] : (Result nil int) ...)
(defn ctx-set-send-timeout-ms [^borrow c : NngCtx ms : int] : (Result nil int) ...)

;;; SUB contexts filter independently: nng_sub0_ctx_subscribe. A SUB context
;;; with no subscription receives nothing, exactly like a SUB socket.
(defn ctx-subscribe   [^borrow c : NngCtx topic : cstr] : (Result nil int) ...)
(defn ctx-unsubscribe [^borrow c : NngCtx topic : cstr] : (Result nil int) ...)

) ;; end defmodule nng/ctx
```

Concurrent REP server, threads, blocking context ops (no NG-B needed):

```turmeric
;; One REP socket, N workers, each with its own context.
(defn worker [^borrow rep : Socket] : nil
  (let [c (ok-val (ctx-open rep))]
    (serve-loop c)                ;; ctx-recv-payload -> handle -> ctx-send-payload
    (ctx-close c)))
```

Two requests in flight on one REQ socket, one thread (spiked; the REP side
used two contexts and replied in reverse order, and each REQ context still
got its own answer):

```turmeric
(ctx-send-str-aio c1 a1 "q1")
(ctx-send-str-aio c2 a2 "q2")
(aio-wait a1) (aio-wait a2)       ;; both requests accepted
(ctx-recv-aio c1 a1)
(ctx-recv-aio c2 a2)
(aio-wait a1) (aio-wait a2)       ;; both replies in
(aio-take-str a1)                 ;; c1's reply, whatever order REP answered in
```

---

## Phases

### NG-A -- poll fds and try ops

**Tasks**
- `PollFd`, `poll-fd->int`, `recv-poll-fd`, `send-poll-fd` in `nng/socket.tur`.
- `try-recv-str`, `try-recv-payload`, `try-send-str`, `try-send-payload` in
  `nng/msg.tur`.
- `:exports` rows in `build.tur` for the new names.
- `tests/nng/poll_test.tur`:
  - `try-recv-str` on an empty queue is `(ok (none))`, and returns immediately.
  - `try-recv-*` on a closed or unsupported socket is `err`, not `none`.
  - `try-send-str` on a PUSH with no peer is `(ok false)`.
  - `recv-poll-fd` on PUB and `send-poll-fd` on SUB are `err` carrying
    `NNG_ENOTSUP`.
  - Reactor drain: three messages are queued before the loop runs; one
    callback must drain all three. This is the assertion that catches a
    non-draining callback on the macOS (kqueue) CI leg. On Linux it passes
    either way.
  - Every reactor test also arms a `reactor-add-timer` guard that stops the
    loop. A regression then fails the test rather than hanging the suite.
- README: a "Event loops" section with the drain rule and the PUB/SUB
  settle-delay note. The nng guide (`turmeric-spices/docs/guides/nng-guide.md`)
  gets the same.
- No `errors/` fixtures: no new linear types.

**Acceptance**
- `tur test tests/nng` green on both CI legs, including the drain test.
- No reactor test can hang the suite.

### NG-B -- `Aio`

**Tasks**
- `c/nng/turnng_aio.c` (the box above), added to `:build-opts :c-sources`.
- `src/nng/aio.tur`, plus an `:exports` row.
- `tests/nng/aio_test.tur`:
  - Round trip over PAIR (submit both, wait both, take, compare).
  - A second take is `NNG_ESTATE`.
  - `aio-try-result` is `none` in flight and `some` after.
  - `aio-set-timeout 50` on a quiet socket waits to `NNG_ETIMEDOUT`.
  - A resubmit while in flight is `err NNG_EBUSY`.
  - `aio-cancel` waits to `NNG_ECANCELED`.
  - A failed send (SUB cannot send) waits to `NNG_ENOTSUP`.
  - `aio-free` with a receive in flight returns.
  - The completion fd drives a reactor callback.
  - The completion fd parks and resumes a `LocalFiberGroup` fiber.
  - Collecting clears the fd: after a take, a 0 ms `reactor-poll` dispatches
    nothing.
- `errors/`: `nng-aio-double-free.tur` (TUR-E0101),
  `nng-aio-use-after-free.tur` (TUR-E0101), and `nng-aio-leak-no-free.tur`
  (TUR-E0100). Each gets an `expect_reject` row in `errors/run.sh` with its
  own witness substring.
- Leak check of the aio paths under valgrind or ASan: the failed send, the
  untaken receive, and free in flight.

**Acceptance**
- Suite green on both legs. Three new `errors/` rows reject for their own
  reason.
- No `nng_msg` or `turnng_aio` allocation is reported lost. Result-box leaks
  from discarded statement-position `Result`s are a compiler-side open report
  (risk 5), not this spice's.

### NG-C -- `NngCtx`

**Tasks**
- `src/nng/ctx.tur`, plus an `:exports` row. The blocking half needs only v0.
  The aio half needs NG-B.
- `tests/nng/ctx_test.tur`:
  - Two in-flight requests on one REQ socket, answered out of order by two
    REP contexts on one thread. Each reply matches its own request.
  - The blocking concurrent REP server: two `stdlib/thread.tur` workers on
    one REP socket, each with its own context, two clients, both answered.
  - `ctx-open` on PUB is `NNG_ENOTSUP`.
  - `ctx-close` after the socket closed returns `NNG_ECLOSED` and does not
    hang.
  - A SUB context with `ctx-subscribe` receives, and one without it does not.
- `errors/`: `nng-ctx-double-close.tur`, `nng-ctx-use-after-close.tur`, and
  `nng-ctx-leak-no-close.tur`, with their `errors/run.sh` rows.
- README: correct the "one thread per REP worker" line. Several workers on
  one REP socket need contexts.

**Acceptance**
- Suite green on both legs. Three new `errors/` rows.
- The concurrent REP server works on one socket. Without contexts the same
  test shape fails with `NNG_ESTATE`.

### Order

```
NG-A  [independent]          smallest; ship first
NG-B  [independent]
NG-C  blocking half [independent] -- aio half [needs NG-B]
```

The recommended order is NG-A, then NG-B, then NG-C. NG-C's blocking half may
land before NG-B if the concurrent REP server is wanted first.

---

## Implementation notes

- **Test design: back-pressure deadlocks a single thread.** Three blocking
  `send-str`s on PUSH, with PULL not yet receiving, hung the spike on the
  third send: PULL buffers almost nothing by default. Tests that queue
  messages should use PUB/SUB (PUB never blocks; allow a short settle delay
  for the slow joiner, as `pubsub_test.tur` does) or `try-send-*`.
- **`set!` from a reactor closure** needs `(def ^mut x ...)`. Read-only
  captures of a linear `Socket` / `Aio` inside a reactor or fiber closure are
  legal (the substructural guide: consuming, not capturing, is what counts).
- **Effects.** `#fx{Net}` goes on the try ops, the submits and the blocking
  context ops. It does not go on alloc/free/timeout/cancel/collect/poll-fd,
  which are local to the handle.
- **No `:cmake-deps` change.** aio and ctx are core nng.
- **`PollFd` lifetime.** A socket's poll fds live as long as the socket. An
  aio's completion fd lives as long as the `Aio`.

---

## Risks and open questions

1. **Completion fds cost two fds per `Aio` that asks for one.** Fine for tens
   or hundreds. A fan-out of thousands wants a shared completion queue: one
   pipe plus a lock-free list of completed boxes per reactor. That is a
   follow-up, not v1 of this layer. Measure first.
2. **Reactor source vs. fd lifetime** (D8). Removing the source before
   `aio-free` / `close` is documented, not enforced. A `Watch` linear handle
   returned by a spice-side `reactor-watch` wrapper could enforce it. Judge
   whether the extra type earns its keep once real callers exist.
3. **`Aio` across threads.** `NNG_EBUSY` catches a double submit, but two
   threads racing *collectors* on one `Aio` are not prevented. The rule is
   documented: an `Aio` belongs to one thread at a time. Linearity keeps it
   to one owner; a borrow handed to a second thread is the caller's bug.
4. **Context support is protocol-dependent at runtime.** `NNG_ENOTSUP` from
   `ctx-open` stays a runtime `err` until typed sockets exist.
5. **Discarded `(Result nil int)` boxes leak.** The spike's valgrind run
   showed every definite leak was a submit, `aio-wait` or `ctx-close`
   result used in statement position. The same is true of today's
   `send-str`. That is
   [carrier-sum-option-boxes-have-no-owner](../../reported/carrier-sum-option-boxes-have-no-owner.md)
   (open, compiler-side). It is 16 bytes a call, which matters for a hot
   submit loop. Bind and inspect results there. Do not reshape the API
   around it.
6. **The header disagrees with the code on `nng_close`.** The plan relies on
   neither: `ctx-close` first is the documented order, and the reverse fails
   safe. A future nng bump should re-run the "close socket first" test.
7. **Windows.** nng's own poll fds use a socketpair there. The spice's
   completion pipe would need the same, and the reactor's IOCP backend may
   not watch either. Out of scope until spices CI has a Windows leg.

---

## Follow-ups (not this plan)

- **Global-scheduler `await` on nng.** `stdlib/async_socket.tur` parks a
  fiber with runtime internals (`tur_io_register` + `tur_fiber_block_yield`).
  A spice could copy that, but it would be coupling to unexported runtime
  API. The right move is a public stdlib primitive. `(fiber-await-fd fd
  events timeout-ms)`, the global-scheduler twin of `local-park-fd`, would
  make `recv-aio` + `fiber-await-fd` + `aio-take-payload` work inside
  `(async ...)`. That is a turmeric-side plan.
- **Shared completion queue** (risk 1).
- **Typed sockets**, **zero-copy `nng_msg`**, and **dialer/listener handles**:
  unchanged NG5 items.

---

## Spike record (2026-10-10)

Run against tur v0.63.9 (the `install-tur.sh` release) and nng v1.12.4
(`tur fetch --update`), on Linux x86-64 (epoll). The spike lived under
`spices/nng/tests/spike/` and `c/nng/turnng_aio.c`, and was removed afterwards.
The existing suite was green before and after (5 suites).

| Check | Result |
| --- | --- |
| `try-recv-str` on an empty PULL | `(ok (none))`, immediate |
| `recv-poll-fd` on PUB | `err 9` (`NNG_ENOTSUP`) |
| Reactor drain, three queued PUB/SUB messages | one callback took all 3; `reactor-stop` from the callback returned |
| aio round trip over PAIR | submit ok, wait ok, take matched; second take `err 11` (`NNG_ESTATE`) |
| `aio-try-result` in flight / after | `none` / `some` |
| `aio-set-timeout 50`, quiet socket | `err 5` (`NNG_ETIMEDOUT`) |
| Resubmit while in flight | `err 4` (`NNG_EBUSY`) |
| `aio-cancel` | `err 20` (`NNG_ECANCELED`) |
| Completion fd in `reactor-add-fd` | fired once; take inside the callback matched |
| `aio-free` with a receive in flight | returned |
| aio send on SUB | `err 9`; the callback freed the message |
| Two REQ contexts in flight, two REP contexts, one thread, reverse-order replies | `q1@s1`, `q2@s2`: each context got its own reply |
| `ctx-open` on PUB | `err 9` |
| `ctx-close` after the socket closed | `err 7` (`NNG_ECLOSED`), no hang |
| Completion fd in `local-park-fd` | fiber woke with `events=1`, take matched |
| valgrind (definite leaks) | none from `nng_msg` or `turnng_aio`; only discarded-`Result` boxes (risk 5) and the spike's own debug strings |

Not exercised: the kqueue (macOS) leg, ctx blocking ops, ctx subscribe,
`try-send-*`, and clear-on-collect (D5's delta). The phase tests above cover
each one.

### Found on the way

Both are **resolved** on `main` after v0.63.9 and archived in
[nil-argument-to-ptr-param-emits-void-expression](../../archive/nil-argument-to-ptr-param-emits-void-expression.md):

- `tur check` accepted `nil` for a `ptr<void>` parameter, and cc then rejected
  `((void)0)`. A literal `nil` there is now the null pointer.
- A captureless callback passed to a `reactor-add-*` wrapper crashed the
  reactor, because the wrapper took `cb : int`. The callbacks are `^fat` now.

This plan's examples spell the null user-data `(:: 0 :ptr<void>)`, so they
also run on v0.63.9, which is what `install-tur.sh` fetches today. From the
next release, `nil` is the idiomatic spelling. Every reactor callback in this
plan captures something, so the second defect never reached them.

---

## See also

- `docs/archive/nng-spice-plan.md` -- the v0 plan and its NG5 section.
- `spices/nng/README.md` -- the shipped surface, "Not in v0".
- `docs/guides/reactor-guide.md` -- `reactor-add-fd`, `local-park-fd`, the
  threading model.
- `docs/guides/inline-c-results-guide.md` -- nested `(Result (Option T) E)`
  builders.
- `docs/guides/substructural-types-guide.md` -- linear captures in closures.
- nng v1.12.4: `include/nng/nng.h`, `docs/man/nng_aio*.3.adoc`,
  `nng_send_aio.3.adoc`, `nng_ctx.5.adoc`, `nng_options.5.adoc`
  (`NNG_OPT_RECVFD` / `NNG_OPT_SENDFD`).
