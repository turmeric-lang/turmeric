---
title: Session Types Guide
category: Concurrency and Async
description: Model protocols as types, whether the protocol has two participants, or more
---

# Session Types Guide

> This guide is the user-facing reference for `stdlib/schan.tur` and the
> broader session-types story; the design record is the archived
> [`stdlib-session-typed-channels-plan`](https://github.com/turmeric-lang/turmeric/blob/main/docs/archive/history/stdlib-session-typed-channels-plan.md).

Turmeric supports session types -- a type discipline that statically verifies
communication protocols between concurrent processes. The feature is enabled
by default; no compiler flag is required.

## Table of Contents

- [Session Types Guide](#session-types-guide)
  - [Table of Contents](#table-of-contents)
  - [Binary Session Types (SS0-SS4)](#binary-session-types-ss0-ss4)
    - [make-session, send, recv, close](#make-session-send-recv-close)
    - [Choice: choose-left, choose-right, offer](#choice-choose-left-choose-right-offer)
    - [Recursive protocols: Rec](#recursive-protocols-rec)
    - [Timeouts](#timeouts)
    - [Payload types](#payload-types)
    - [Duality](#duality)
    - [Effect integration](#effect-integration)
    - [Running a peer: session-spawn](#running-a-peer-session-spawn)
  - [Multi-Party Session Types (SS5-SS8)](#multi-party-session-types-ss5-ss8)
    - [defprotocol](#defprotocol)
    - [make-protocol, send-to, recv-from, close](#make-protocol-send-to-recv-from-close)
    - [Timed receives: timeout, recv-timeout-from](#timed-receives-timeout-recv-timeout-from)
    - [Three or more roles](#three-or-more-roles)
    - [Projection algorithm](#projection-algorithm)
  - [Checking a protocol under the interpreter](#checking-a-protocol-under-the-interpreter)
  - [Session-Typed Channel Wrappers (stdlib/schan.tur)](#session-typed-channel-wrappers-stdlibschantur)
  - [Error Codes](#error-codes)
  - [Getting More Help](#getting-more-help)

---

## Binary Session Types (SS0-SS4)

Binary session types describe a two-party communication channel. One end of the
channel runs a _protocol_ `P`; the other end runs the _dual_ protocol `dual(P)`.

### make-session, send, recv, close

```turmeric
;; Allocate a two-ended channel for the protocol Send<int, Close>.
(let [[a b] (make-session (Send int Close))]
  ...)
```

```sweet-exp
;; Allocate a two-ended channel for the protocol Send<int, Close>.
let [[a b] make-session((Send int Close))]
  ...
```

The pair `[a b]` gives both endpoints. Endpoint `a` has type
`Session[Send int Close]`; endpoint `b` has the dual type
`Session[Recv int Close]`.

**send** -- advance the sender side by one message:

```turmeric
(let [a (send a 42)]  ; a advances from Send<int,Close> to Close
  (close a))
```

```sweet-exp
let [a send(a 42)]  ; a advances from Send<int,Close> to Close
  close(a)
```

**recv** -- advance the receiver side by one message; returns `[value new-ch]`:

```turmeric
(let [[v b] (recv b)]  ; v = 42, b advances from Recv<int,Close> to Close
  (close b))
```

```sweet-exp
let [[v b] recv(b)]  ; v = 42, b advances from Recv<int,Close> to Close
  close(b)
```

**close** -- consume the channel after the protocol ends (type `Close`).

> **Payload types: use `int` or `bool` for now.** The compiled runtime carries
> every message as an `int64_t`. A `float` payload is **silently truncated**
> (`7.25` arrives as `7`); a `cstr` or a delegated endpoint fails to build on
> macOS; a by-value struct fails to build everywhere. All four type-check, and
> all four are correct under `tur --interpret`. See
> [session-payloads-are-int64-only](https://github.com/turmeric-lang/turmeric/blob/main/docs/archive/session-payloads-are-int64-only.md).

### Choice: choose-left, choose-right, offer

When the sender can choose between two branches use `choose-left` / `choose-right`;
the receiver uses `offer` and pattern-matches on `Left`/`Right`:

```turmeric
;; Sender side: Choose<Send int Close, Close>
(let [ch (choose-left ch)]  ; picks the Left branch
  (let [ch (send ch 7)]
    (close ch)))

;; Receiver side: Branch<Recv int Close, Close>
(match (offer ch)
  (Left ch)
    (let [[n ch] (recv ch)]
      (close ch))
  (Right ch)
    (close ch))
```

```sweet-exp
;; Sender side: Choose<Send int Close, Close>
let [ch choose-left(ch)]  ; picks the Left branch
  let [ch send(ch 7)]
    close(ch)

;; Receiver side: Branch<Recv int Close, Close>
match offer(ch)
  (Left ch)
  let [[n ch] recv(ch)]
    close(ch)
  (Right ch)
  close(ch)
```

### Recursive protocols: Rec

`Rec` introduces a recursive protocol variable `self` that unrolls on each
loop iteration:

```turmeric
;; Server: repeat (recv int, send int) until client closes
(defn echo-server [^linear ch :(Session (Rec self (Branch (Recv int (Send int self)) Close)))] : nil
  (match (offer ch)
    (Left ch)
      (let [[n ch] (recv ch)]
        (let [ch (send ch n)]
          (echo-server ch)))
    (Right ch)
      (close ch)))
```

```sweet-exp
;; Server: repeat (recv int, send int) until client closes
defn echo-server [^linear ch :(Session (Rec self (Branch (Recv int (Send int self)) Close)))] :nil
  match offer(ch)
    (Left ch)
    let [[n ch] recv(ch)]
      let [ch send(ch n)]
        echo-server(ch)
    (Right ch)
    close(ch)
```

See `stdlib/session.tur` for the `echo-server-loop` and `echo-client-call`
helpers that wrap this pattern.

### Timeouts

`recv-timeout` is a non-blocking receive that returns a `Choice` value:

```turmeric
(match (recv-timeout ch 500)  ; 500 ms deadline
  (Left [v ch]) (do (println v) (close ch))
  (Right ch)    (do (println "timed out") (close ch)))
```

```sweet-exp
match recv-timeout(ch 500)  ; 500 ms deadline
  (Left [v ch])
  do
    println(v)
    close(ch)
  (Right ch)
  do
    println("timed out")
    close(ch)
```

Multi-party role endpoints have the same bounded wait, `recv-timeout-from`,
on a step the global protocol declares timed -- see
[Timed receives](#timed-receives-timeout-recv-timeout-from) below.

### Payload types

A message crosses the channel as one machine word. `int`, `bool`, the sized
integers, `float` (bit-preserved -- `7.25` arrives as `7.25`), `cstr`, pointer
handles (`ptr<T>`, `rc<T>`, an opaque type over a pointer, a `:heap` record),
and session endpoints themselves (delegation: `(Send (Session P) ...)`) all
travel intact on both backends. A by-value `defstruct` or ADT does not fit the
word and is rejected at elaboration with `TUR-E0212` naming the type; send it
behind `rc<T>` / `ref<T>`, or declare the record `:heap`.

### Duality

The duality rule governs how the two ends of a channel relate:

| Protocol (one end) | Dual (other end) |
|--------------------|------------------|
| `Send T P`         | `Recv T dual(P)` |
| `Recv T P`         | `Send T dual(P)` |
| `Choose P Q`       | `Branch dual(P) dual(Q)` |
| `Branch P Q`       | `Choose dual(P) dual(Q)` |
| `Close`            | `Close`          |
| `Rec self P`       | `Rec self dual(P)` |

`make-session` accepts one protocol type and automatically produces the dual
for the second endpoint.

### Effect integration

Session channels and algebraic effects can coexist freely. A function may
perform effects while holding a linear channel, as long as the channel is
consumed exactly once along every code path:

```turmeric
(defeffect Log [msg :cstr] :nil)

(defn logged-send [^linear ch :(Session (Send int Close)) val : int] : int
  (perform (Log "before send"))
  (let [ch (send ch val)]
    (close ch)
    0))
```

```sweet-exp
defeffect Log [msg :cstr] :nil

defn logged-send [^linear ch :(Session (Send int Close)) val :int] :int
  perform(Log("before send"))
  let [ch send(ch val)]
    close(ch)
    0
```

See `tests/fixtures/session-effects/` for a complete example.

---

### Running a peer: session-spawn

A session op blocks until its peer arrives, so the other endpoint has to run
somewhere else. `stdlib/session.tur` provides the portable way to do that:

```turmeric
(load "stdlib/session.tur")

(let [[s r] (make-session (Send int Close))]
  (let [t (session-spawn (fn [] (let [[n r] (recv r)] (println n) (close r))))]
    (let [s (send s 42)]
      (close s)
      (session-join t))))
```

`session-spawn` takes a zero-argument thunk and returns a `SessionPeer`;
`session-join` waits for it. Compiled, the peer is an OS thread; under
`tur --interpret` it is a scheduler fiber. The same source runs on both, and
every example below uses this pair.

> A peer written as `(async (fn [] ...))` works too: an `async` body that
> captures a session endpoint runs on its own OS thread when compiled (a
> scheduler fiber under `--interpret`), and `await` joins it -- so
> `(async (fn [] (server-loop r)))` is the same peer `session-spawn` makes,
> with a future for its result. The one shape that still hangs is an `async`
> body that makes BOTH endpoints and waits on itself; the compiler warns at
> that site with `TUR-W0043`. See
> [compiled-async-fiber-deadlocks-on-a-session-op](https://github.com/turmeric-lang/turmeric/blob/main/docs/archive/compiled-async-fiber-deadlocks-on-a-session-op.md).

## Multi-Party Session Types (SS5-SS8)

Multi-party session types generalize binary sessions to N >= 2 participants.
A _global protocol_ specifies all interactions between named roles; the compiler
projects it onto each role's local view.

### defprotocol

Declare a global protocol with `defprotocol`:

```turmeric
(defprotocol Ping [A B]
  (-> A B int)    ; A sends an int to B
  (-> B A int))   ; B replies with an int to A
```

```sweet-exp
defprotocol Ping [A B]
  (-> A B int)    ; A sends an int to B
  (-> B A int)    ; B replies with an int to A
```

The role list `[A B]` names each participant. Each `(-> From To type)` line
is one message transfer.

### make-protocol, send-to, recv-from, close

After declaring a protocol, allocate role endpoints with `make-protocol`:

```turmeric
(let [[ra rb] (make-protocol Ping)]
  ...)
```

```sweet-exp
let [[ra rb] make-protocol(Ping)]
  ...
```

`ra` has type `(Role Ping A)` and `rb` has type `(Role Ping B)`. Each role
endpoint is linear -- it must be consumed exactly once.

**send-to** -- send a message to a named role peer:

```turmeric
(let [ra (send-to ra B 42)]  ; A sends 42 to B; ra advances in protocol
  ...)
```

```sweet-exp
let [ra send-to(ra B 42)]  ; A sends 42 to B; ra advances in protocol
  ...
```

**recv-from** -- receive a message from a named role peer; returns `[value new-ch]`:

```turmeric
(let [[v rb] (recv-from rb A)]  ; B receives from A; v = 42
  ...)
```

```sweet-exp
let [[v rb] recv-from(rb A)]  ; B receives from A; v = 42
  ...
```

**close** -- close the role endpoint after the protocol is complete:

```turmeric
(close ra)
```

```sweet-exp
close(ra)
```

Full two-role ping example:

```turmeric
(defprotocol Ping [A B]
  (-> A B int)
  (-> B A int))

(defn role-a [^linear ch :(Role Ping A)] : nil
  (let [ch (send-to ch B 42)]
    (let [[v ch] (recv-from ch B)]
      (println v)
      (close ch))))

(defn role-b [^linear ch :(Role Ping B)] : nil
  (let [[v ch] (recv-from ch A)]
    (let [ch (send-to ch A v)]
      (close ch))))

(defn main [] : int
  (let [[ra rb] (make-protocol Ping)]
    (let [t (session-spawn (fn [] (role-b rb)))]
      (role-a ra)
      (session-join t)))
  0)
```

```sweet-exp
defprotocol Ping [A B]
  (-> A B int)
  (-> B A int)

defn role-a [^linear ch :(Role Ping A)] :nil
  let [ch send-to(ch B 42)]
    let [[v ch] recv-from(ch B)]
      println(v)
      close(ch)

defn role-b [^linear ch :(Role Ping B)] :nil
  let [[v ch] recv-from(ch A)]
    let [ch send-to(ch A v)]
      close(ch)

defn main [] :int
  let [[ra rb] make-protocol(Ping)]
    let [t session-spawn((fn [] role-b(rb)))]
      role-a(ra)
      session-join(t)
  0
```

### Three or more roles

`defprotocol` and `make-protocol` support any number of roles (N >= 2). For
a three-role pipeline:

```turmeric
(defprotocol Pipeline [A B C]
  (-> A B int)   ; A sends to B
  (-> B C int))  ; B forwards to C

(defn main [] : int
  (let [[ra rb rc] (make-protocol Pipeline)]
    (let [ta (session-spawn (fn [] (role-a ra)))]
      (let [tb (session-spawn (fn [] (role-b rb)))]
        (role-c rc)
        (session-join ta)
        (session-join tb))))
  0)
```

```sweet-exp
defprotocol Pipeline [A B C]
  (-> A B int)   ; A sends to B
  (-> B C int)   ; B forwards to C

defn main [] :int
  let [[ra rb rc] make-protocol(Pipeline)]
    let [ta session-spawn((fn [] role-a(ra)))]
      let [tb session-spawn((fn [] role-b(rb)))]
        role-c(rc)
        session-join(ta)
        session-join(tb)
  0
```

The runtime uses a shared router so all N role endpoints communicate through
a single lock-based message router allocated on the heap.

### Timed receives: timeout, recv-timeout-from

A timeout is the only recovery a protocol has against a participant that
stalls, and a multi-party protocol has more participants to stall. Declare
the step that may time out with `timeout`, giving the message and what
follows each outcome:

```turmeric
(defprotocol Relay [A B C]
  (timeout (-> A B int)          ; B may stop waiting for A's value
    [ok      (-> B C int)]       ; ... it arrived: forward it
    [expired (-> B C int)]))     ; ... it did not: tell C anyway
```

```sweet-exp
defprotocol Relay [A B C]
  (timeout (-> A B int)          ; B may stop waiting for A's value
    [ok      (-> B C int)]       ; ... it arrived: forward it
    [expired (-> B C int)])      ; ... it did not: tell C anyway
```

The branches are named `ok` and `expired` (in either order); either may be
empty, and any forms after the `timeout` follow both. The deadline is not
part of the protocol -- the receiver supplies it, in milliseconds, at the op.
`recv-timeout-from` is the multi-party `recv-timeout`: match `Left` for the
value and the endpoint at `ok`, `Right` for the endpoint at `expired`:

```turmeric
(defn relay-b [^linear ch :(Role Relay B)] : nil
  (match (recv-timeout-from ch A 500)
    (Left pair)
      (let [[v ch] pair]
        (close (send-to ch C v)))
    (Right ch)
      (close (send-to ch C -1))))
```

```sweet-exp
defn relay-b [^linear ch :(Role Relay B)] :nil
  match recv-timeout-from(ch A 500)
    (Left pair)
    let [[v ch] pair]
      close $ send-to ch C v
    (Right ch)
    close $ send-to ch C -1
```

The sender and every other role use their ordinary ops: A just `send-to`s B,
and C just `recv-from`s B. That is sound because of one rule, checked when
the protocol is declared:

- **Only the receiver learns whether the deadline passed**, so every other
  role -- the sender included -- must continue the same way in both branches.
  A protocol that gives C a message only in `ok` is rejected with
  `TUR-E0220`; the fix is what `Relay` does, a message from the receiver in
  each branch. (The same rule makes the sender's projection
  `Send int (Timeout P P)`, the binary dual of the receiver's
  `Recv int (Timeout Q P)`, so a two-role timed protocol projects onto exactly
  what `make-session` builds for a binary `recv-timeout`.)

At run time, a sender whose receiver has already given up does not block:
the router drops that one message. The late value is never delivered to the
receiver's *next* receive from the same peer, so after `expired` the protocol
resumes exactly where it says. `recv-from` is also accepted on a timed step,
as a receive that never gives up (the `ok` branch).
`tests/fixtures/session-mp-timeout` (and its `--interpret` twin
`session-mp-timeout-turi`) runs both outcomes and the dropped-late-message
case.

### Projection algorithm

At compile time the elaborator _projects_ the global protocol onto each
role's local view. Projection removes all interactions that do not involve
the current role:

- A message `(-> From To T)` projects to `Send T rest` for `From`, and
  `Recv T rest` for `To`. For any other role R, it is transparent (role R
  keeps its own remaining protocol).
- Choice branches that a role does not participate in must be _uniform_
  across branches (mergeability condition). If they are not, the compiler
  emits `TUR-E0220`.
- A timed receive `(timeout (-> From To T) [ok ...] [expired ...])` projects
  to `Recv T (Timeout ok expired)` for `To`. Every other role must project
  the two branches identically (`TUR-E0220` otherwise); `From` gets
  `Send T (Timeout P P)` and a bystander gets `P`.

The projection check happens where a `(project G R)` type is written; a
protocol containing a `timeout` is also projected onto every role when it is
declared, since its role endpoints rely on the uniformity rule above.

---

## Checking a protocol under the interpreter

Session types prove that each endpoint follows its own protocol. They do not
prove deadlock freedom *across* independently typed channels: two participants
can each be waiting for the other, and every channel still type-checks.

Run the program under `tur --interpret` first. The interpreter's rendezvous is
cooperative and single-threaded, so it can see when no participant can make
progress, and it stops with a clean error and a nonzero exit instead of
hanging:

```
$ tur --interpret protocol.tur
tur: eval: session recv deadlocked (no sender) on Session[Send[int, Close]] -- no participant can make progress; this is a protocol deadlock in the program, not an interpreter limit (the compiled binary would hang here)
```

The same holds for a main-context `await` on a task that is parked with
nothing else runnable (`eval: await deadlocked`). The compiled binary has no
global view of its threads and hangs in both situations, so a `deadlocked`
error under `--interpret` is a bug in the protocol as written, not an
interpreter limitation. There are no known false positives: a peer that sleeps
before sending is waited out, not reported. Runnable examples:
`tests/fixtures/errors/session-deadlock-no-peer-turi` (a receive nobody will
ever satisfy), `session-deadlock-mutual-turi` (two participants each blocked
on the other) and `session-deadlock-awaited-turi` (the same cycle reached
through `await`).

## Error Codes

| Code | Meaning |
|------|---------|
| `TUR-E0210` | Session endpoints are not dual |
| `TUR-E0211` | Linear session channel dropped without being closed |
| `TUR-E0212` | Session operation does not match the current protocol state (wrong op, use-after-close, reuse of a consumed endpoint, payload type mismatch) |
| `TUR-E0220` | Global protocol is not projectable (mergeability failure) |
| `TUR-E0221` | Role not declared in the protocol |
| `TUR-E0222` | Role implementation does not match the projected local type |
| `TUR-E0223` | Global protocol not well-formed (undeclared role used) |
| `TUR-W0043` | Session op inside an `async` body on endpoints the body makes itself: may deadlock the compiled program; hand one end to a `session-spawn` peer |

---

## Session-Typed Channel Wrappers (stdlib/schan.tur)

The built-in session types above are their own typed-channel runtime. When you
instead want to put a protocol discipline over an ordinary buffered channel --
e.g. a worker-pool request/response, or an RPC pipe -- `stdlib/schan.tur`
provides a thin generic wrapper, `SChan<p>`, that carries a protocol *phantom*
`p` advanced by each operation:

```turmeric
(import schan :refer [SChan SSend SRecv SClose
                      schan-new schan-send schan-recv schan-close])
```

The phantom is built from three type-level tags (the session-type names `Send` /
`Recv` / `Close` are reserved primitive constructors, so the wrapper uses the
`S`-prefixed spellings):

| Tag | Meaning |
|-----|---------|
| `(SSend T R)` | send a `T`, then continue as `R` |
| `(SRecv T R)` | receive a `T`, then continue as `R` |
| `SClose`      | the protocol terminus |

Each operation consumes the channel at one protocol state and returns it at the
next, so the phantom is threaded through the result type:

```turmeric
schan-send  : SChan<SSend T R> -> T  -> SChan<R>
schan-recv  : SChan<SRecv T R>       -> Pair<T SChan<R>>
schan-close : SChan<SClose>          -> nil
```

A round trip of `SSend int (SRecv int SClose)`:

```turmeric
(let [c0 (:: (schan-new 2) (SChan (SSend int (SRecv int SClose))))
      c1 (schan-send c0 7)      ;; c1 : SChan<SRecv int SClose>
      p  (schan-recv c1)        ;; p  : Pair<int SChan<SClose>>
      v  (pair-fst p)           ;; v  : int  (= 7)
      c2 (pair-snd p)]          ;; c2 : SChan<SClose>
  (schan-close c2))
```

Because the phantom advances with every step, **skipping or reordering a step is
a compile-time type error**. Calling `schan-close` while the channel is still at
`SChan<SRecv int SClose>` fails with a `TUR-E0001` phantom mismatch:

```
error [TUR-E0001]: function 'schan-close' arg 1:
  expected (SChan SClose),
  got (SChan (SRecv int SClose))
```

`SChan` is `:linear`, so the protocol additionally cannot be *replayed* --
each step consumes its handle exactly once.

Runnable examples: `tests/fixtures/schan-roundtrip` (single round trip),
`tests/fixtures/schan-worker-pool` (a request/response served by worker threads
reading from the wrapped channel), and `tests/fixtures/errors/schan-skip-step`
(the phantom-mismatch failure). The wrapper sits on top of the low-level
[`tur/chan`](https://github.com/turmeric-lang/turmeric/blob/main/stdlib/chan.tur) channels, which keep their untyped surface
for callers that do not want the protocol discipline.

> **`schan-recv` used to take a cell.** Until 2026-08-20 it returned only the
> continuation and wrote the received value through a caller-allocated
> out-parameter (`schan-cell-new` / `-get` / `-free`, all now removed). That was
> a workaround for a monomorphizer defect -- a generic function could not return
> a parametric aggregate whose element type is a phantom carried inside an
> opaque argument. The defect was fixed 2026-06-05
> (`docs/archive/history/generic-struct-opaque-element-miscompile.md`), so the
> `Pair<T SChan<R>>` signature the design always wanted is what ships now.

---

## Getting More Help

Use `tur explain` to get a detailed description of any error code:

```sh
tur explain TUR-E0212
tur explain TUR-E0220
```

The test fixtures under `tests/fixtures/session-*` and `tests/fixtures/errors/session-*`
provide runnable examples for every feature and failure mode described in this guide.
