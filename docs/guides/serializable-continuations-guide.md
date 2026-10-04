---
title: Serializable Continuations Guide
category: Advanced Control Flow
description: Serializable continuations for persistent workflows and cross-process computation
---

# Serializable Continuations Guide

Save and resume computations across process boundaries using serializable continuations.

## Overview

Turmeric's **serializable continuations** enable suspended computations to be marshalled to bytes, persisted to disk or sent over a network, then resumed in a fresh process. This builds on the delimited continuations (`shift`/`reset`), which reify the call stack as a heap-allocated closure chain.

Use cases include:
- **Persistent workflows** -- Pause and resume multi-step business processes
- **Distributed task migration** -- Send half-finished computations to other nodes
- **Checkpointing** -- Save state periodically; restart from last checkpoint on crash
- **Web continuations** -- Racket-style: serialize "what to do next" as a URL token
- **Mobile agents** -- Send code + state to a remote peer for execution
- **Debugger snapshots** -- Freeze and replay running program state

## Core Concepts

### What is a Serializable Continuation?

A **continuation** represents "the rest of the computation" -- a suspended state
that can be resumed later. A **serializable continuation** can be converted to
bytes and restored, even in a different process running the same program.

`serial-reset` delimits the region; `serial-shift` captures the rest of that
region and hands it to a **handler function** as an opaque `serial-cont`. The
handler decides what happens: resume it now with `(k v)` / `(serial-resume k v)`,
or marshal it with `serial-cont->bytes`, put the bytes somewhere, and let
another process rebuild it with `bytes->serial-cont`:

```turmeric
(load "stdlib/serial.tur")

;; The handler: keep the continuation on disk, do not resume it now.
(defn save-to-disk [k : serial-cont] : int
  (do (cont-to-file (serial-cont->bytes k) "my-continuation.bin")
      0))

;; The reset yields the handler's value (0) because the handler did not resume.
(defn capture [] : int
  (serial-reset (let [x 42] (+ x (serial-shift save-to-disk 0)))))

;; Later, in another process: rebuild and resume with 100 -> 142.
(defn restore [] : int
  (match (bytes->serial-cont (cont-from-file "my-continuation.bin"))
    (Ok k)  (serial-resume k 100)
    (Err m) (do (println m) 0)))
```
```sweet-exp
load("stdlib/serial.tur")

;; The handler: keep the continuation on disk, do not resume it now.
defn save-to-disk [k : serial-cont] : int
  do
    cont-to-file(serial-cont->bytes(k) "my-continuation.bin")
    0

;; The reset yields the handler's value (0) because the handler did not resume.
defn capture [] : int
  serial-reset $ let [x 42] {x + serial-shift(save-to-disk 0)}

;; Later, in another process: rebuild and resume with 100 -> 142.
defn restore [] : int
  match bytes->serial-cont(cont-from-file("my-continuation.bin"))
    Ok(k)
    serial-resume(k 100)
    Err(m)
    do
      println(m)
      0
```

`(serial-shift handler default)` takes exactly two arguments: the handler (a
top-level `defn` or a `fn` literal taking one `serial-cont`) and a default
value. If the handler resumes the continuation, the resumed computation's result
becomes the value of the enclosing `serial-reset`; if it does not, the handler's
own return value is. The `default` is the value the hole takes on the
interpreter's structural path; pass `0`.

**Capture scope.** The continuation must be a *supported delimited context*: a
single-scalar-hole chain of `let` prelude + `+ - * /` + 2-arg calls + one `if`.
Contexts outside that grammar are rejected with `TUR-E0706` (see
`docs/archive/history/serial-shift-unsupported-context-miscompile.md`). Design
the region around the shift so the "rest" is a named call: the image-dump
combinator does exactly this (`(serial-shift handler 0)` followed by `(loop)`).
Two further rules follow from how the frames are marshalled by name:

- **No effect may escape the callee in the context.** A resumed frame calls
  it as a plain function, outside any handler that enclosed the reset, so it
  may not perform an effect it does not handle itself -- and neither may
  anything it calls. Calling through a function value, or performing an
  effect the callee (or something it calls) handles, is fine. It also cannot
  contain a `serial-reset` of its own, so a multi-page flow does not nest
  resets: each page's callee returns a code and the code that runs *outside*
  the reset starts the next one (the guestbook example's `advance`).
- **The handler** (the receiver) is different: it runs once, at capture, and
  it MAY perform an effect it does not handle itself. The reset then calls it
  as an ordinary function on its own continuation, so the effect reaches the
  handlers around the `serial-reset` -- `(defn recv [k : serial-cont] : int
  (k (perform (Ask))))` under `(handle (run) (Ask [] r) (resume r 41))` works
  compiled as it does interpreted
  (`docs/archive/serial-receiver-effect-cannot-reach-enclosing-handler.md`).
  That holds with an `if` branch point in the context too, and for a
  capturing `(fn [k] ...)` receiver whose captures are plain values (ints,
  floats, strings). What is still `TUR-E0706`: a closure receiver
  capturing anything else (name the receiver instead), and an effect escaping
  a context callee as above -- handle it inside the callee.

`TUR_TRACE_CORE=1` names the collector rule (`[CTX-REJECT] cps_ir.c:<line>`)
that rejected a context, which is faster than guessing.

### The `Serializable` Typeclass

Not all values can be serialized. Types must opt-in via the `Serializable` typeclass:

```turmeric
(defclass Serializable [a]
  (serialize   [x] : ptr<void>)
  (deserialize [b : ptr<void>] : a))
```
```sweet-exp
defclass Serializable [a]
  serialize   [x] : ptr<void>
  deserialize [b : ptr<void>] : a
```

`serialize` returns a length-prefixed `bytes` buffer (carried as
`:ptr<void>`). `stdlib/serial.tur` ships instances for the primitives and
two containers:

```turmeric no-check
(definstance Serializable [int] ...)
(definstance Serializable [bool] ...)
(definstance Serializable [float] ...)
(definstance Serializable [cstr] ...)
(definstance Serializable [ptr<void>] ...)
(definstance Serializable [Pair] ...)
(definstance Serializable [Option] ...)
```

Types that **do not** implement `Serializable` (file handles, raw pointers,
`Mutex<T>`) cannot be captured in a serializable continuation. The elaborator
enforces this at the `serial-reset` boundary (`TUR-E0018`). A captured
environment marshals **by value**: an `int` inline, a `cstr` as its bytes, and
any other type through its `Serializable` instance -- so a resumed continuation
holds fresh copies, never the writer's heap addresses.

### Resource Types

File handles, sockets, and other system resources cannot be serialized
directly, and there is no separate "resource" typeclass. The pattern is to
serialize a **stable representation** and re-acquire the resource on resume --
a `Serializable` instance for a file-handle wrapper that serializes the *path*
and re-opens the file in `deserialize`. Keep the raw handle out of the captured
frame and carry the wrapper. For process-level resources, the image-dump
reload hooks (`docs/guides/image-dumps-guide.md`, "Resources") do this at the
load boundary.

## Surface API

### Core Functions

```turmeric no-check
;; Delimit a serializable region
(serial-reset body)

;; Capture the rest of the region and hand it to handler as a serial-cont
(serial-shift handler default)      ; handler : (fn [serial-cont] T)

;; Serialize a continuation to bytes ({int64 len; data}, caller-owned)
(serial-cont->bytes k) : ptr<void>

;; Rebuild a continuation from bytes (validated; a foreign/damaged buffer is Err)
(bytes->serial-cont b) : (Result serial-cont cstr)

;; Resume a continuation with a value -- the same thing as (k v)
(serial-resume k v) : int

;; File helpers over the bytes
(cont-to-file b path) : int          ; 1 on success
(cont-from-file path) : ptr<void>    ; NULL on failure
```

`save-cont!` / `resume-cont!` in `stdlib/workflow.tur` are the older spellings
of `serial-cont->bytes` and "rebuild then resume" (`resume-cont!` aborts on a
malformed buffer where `bytes->serial-cont` returns `Err`); both surfaces stay.

### The `serial-cont` Type

`serial-cont` is the opaque handle to a captured frame chain. There is no
struct to look inside: it is applied like a function (`(k v)`), marshalled with
`serial-cont->bytes`, and typed at handler parameters (`[k : serial-cont]`).
The value it is resumed with is an `int`-carried scalar -- the hole grammar
above -- and the result of resuming is the enclosing reset's result.

## Examples

Every example below is the same shape: a named handler that stashes the bytes
and a resume site that rebuilds them. Helpers such as `save-checkpoint` are the
application's own.

### Persistent Workflow

A multi-step business process that survives crashes. Each step's continuation
is written under a checkpoint name; on restart the latest checkpoint is
rebuilt and resumed with the value the step was waiting for:

```turmeric no-check
(defn after-validation [k : serial-cont] : int
  (do (save-checkpoint "order-validated" (serial-cont->bytes k)) 0))

(defn process-order [order-id : int] : int
  (serial-reset
    (fulfill order-id (serial-shift after-validation 0))))

;; Resume from the last checkpoint with the value the step was waiting for.
(defn resume-order [order-id : int charge-result : int] : int
  (match (bytes->serial-cont (load-latest-checkpoint order-id))
    (Ok k)  (serial-resume k charge-result)
    (Err m) (do (println m) 0)))
```

### Distributed Task Migration

Send a half-finished computation to another node. Node B runs the same binary,
so the frame names in the bytes resolve there:

```turmeric no-check
;; Node A: capture, ship the bytes, do not resume locally.
(defn migrate [k : serial-cont] : int
  (do (send-to-node-b (serial-cont->bytes k)) 0))

(def result
  (serial-reset (+ (run-task1) (serial-shift migrate 0))))

;; Node B: rebuild and resume with its own input.
(defn handle-migration [bytes : ptr<void> input : int] : int
  (match (bytes->serial-cont bytes)
    (Ok k)  (serial-resume k input)
    (Err m) (do (println m) -1)))
```

### Web Continuations (Racket-style)

Serialize "what to do when the form is submitted" as a URL token. The handler
stores the bytes under a fresh token and renders the form whose action carries
it; the submit route rebuilds the continuation and resumes it with the posted
value. `docs/guides/web-continuations-guide.md` is the full treatment.

```turmeric no-check
(defn suspend-for-form [k : serial-cont] : int
  (let [token (store-continuation (serial-cont->bytes k))]
    (do (render-form (str "/checkout-submit?token=" token)) 0)))

(defn get-checkout [] : int
  (serial-reset (finish-checkout (serial-shift suspend-for-form 0))))

(defn post-checkout-submit [token : cstr cc-number : int] : int
  (match (bytes->serial-cont (load-continuation token))
    (Ok k)  (serial-resume k cc-number)
    (Err m) (do (println m) 0)))
```

### Checkpointing a Long-Running Computation

Periodic snapshots for crash recovery: capture at the top of each chunk, keep
going, and on restart resume the latest one. See
`docs/guides/checkpointing-guide.md` for the complete worked example.

```turmeric no-check
(defn checkpoint [k : serial-cont] : int
  (do (cont-to-file (serial-cont->bytes k) "checkpoint.bin")
      (serial-resume k 0)))            ; keep going right away

(defn analyze-chunk [i : int] : int
  (serial-reset (process-chunk i (serial-shift checkpoint 0))))

(defn recover-analysis [] : int
  (match (bytes->serial-cont (cont-from-file "checkpoint.bin"))
    (Ok k)  (serial-resume k 0)
    (Err m) (do (println m) 0)))
```

## Error Handling

### A buffer the program cannot rebuild

Frames marshal by **stable name**, and `bytes->serial-cont` validates the whole
record stream against this program's frame registry before rebuilding
anything. A buffer written by a different program -- or by an earlier build
whose frame names moved -- comes back as `Err`, never as an abort or a resumed
garbage chain:

```turmeric no-check
(match (bytes->serial-cont bytes)
  (Ok k)  (serial-resume k value)
  (Err m) (println m))
;; "bytes->serial-cont: unknown frame (written by a different program?)"
```

The messages are static strings, each prefixed `bytes->serial-cont: `:
`null bytes`, `short buffer`, `bad frame count`, `truncated frame`,
`frame name too long`, `unknown frame (written by a different program?)`,
`truncated env`, `bad env kind`, `bad frame tag`. `resume-cont!` performs no
such check; prefer `bytes->serial-cont` for anything read from disk or the
network.

### Handling Unserializable Types

If you try to capture an unserializable value, the elaborator produces an error:

```ascii
error: binding `handle : file-handle` captured inside `serial-reset`
       but `file-handle` does not implement `Serializable`
  --> src/main.tur:42:5
   |
42 |   (serial-shift stash 0)
   |                 ^ `handle` captured here
   = help: use resource marshalling or move outside serial-reset boundary
```

To fix: implement `Serializable` for a wrapper that carries a stable
representation (see "Resource Types"), or restructure so the handle is not in
scope at the shift.

### Sharing and cycles

The bytes hold each frame's environment **by value**, so there is nothing to
share and nothing to cycle: two frames that captured the same `cstr` write it
twice and rebuild two strings. A `Serializable` instance for your own type
decides what its bytes contain; an instance that walks a cyclic structure
without a visited set will not terminate, so give such a type an
identifier-based representation instead.

## Interaction with Ownership

`serial-cont->bytes` copies: an `int` environment is written inline, a `cstr`
as its bytes, and any other environment type through its `Serializable`
instance. Rebuilding allocates fresh values. Consequences:

- A resumed continuation never aliases the writer's heap -- mutating the
  original after capture does not change what the bytes hold.
- Two captures of the same value are two independent copies on resume.
- Reference-counted values are cloned into the encoding, not shared; the
  `Serializable` instance for the pointee is what runs.

## Standard Library Support

### `stdlib/serial.tur`

The `Serializable` class and instances, the typed continuation trio, and the
file helpers:

```turmeric no-check
(serial-cont->bytes k)                 ; k : serial-cont -> bytes
(bytes->serial-cont b)                 ; bytes -> (Result serial-cont cstr)
(serial-resume k v)                    ; = (k v)
(cont-to-file b "/tmp/checkpoint.bin") ; 1 on success, 0 on failure
(cont-from-file "/tmp/checkpoint.bin") ; bytes, or NULL on any I/O failure
```

### `stdlib/workflow.tur`

**Stable API surface (Phase 21).** Downstream code -- including the
application-image-dumps plan (`docs/archive/history/application-image-dumps-plan.md`,
AI2) -- builds on exactly these four entry points; treat their signatures as
the SemVer contract:

| Function | Signature | Role |
|---|---|---|
| `save-cont!` | `[k : ptr<void>] : ptr<void>` | marshal a captured continuation `k` to a `bytes` buffer (caller-owned) |
| `resume-cont!` | `[b : ptr<void> v : int] : int` | rebuild the chain from `b` and resume it on `v` |
| `workflow-suspend` | `[k : ptr<void>] : ptr<void>` | alias of `save-cont!` |
| `workflow-resume` | `[b : ptr<void> v : int] : int` | alias of `resume-cont!` |

`k` is the opaque handle the `serial-shift` receiver is passed (a DK chain
carried as `ptr<void>`; `serial-cont` is the same handle with its own name).
These are thin shims over the
`tur_serial_cont_serialize`/`_deserialize`/`_resume` runtime, which is emitted
whenever a program contains serial syntax or calls one of those three on a
`serial-cont` (so loading `stdlib/serial.tur` for `serial-resume` alone is
enough; the emitted C never references the runtime undeclared).

A workflow step that waits for an outside decision is the handler-that-does-
not-resume shape:

```turmeric no-check
;; Suspend: persist the step, answer "pending" (0) for now.
(defn await-approval [k : serial-cont] : int
  (do (db-save-continuation order-id (serial-cont->bytes k)) 0))

(defn process-approval [order-id : int] : int
  (serial-reset (fulfill-if order-id (serial-shift await-approval 0))))

;; Resume from the database with the decision (1 = approved).
(defn resume-approval [order-id : int approved : int] : int
  (match (bytes->serial-cont (db-load-continuation order-id))
    (Ok k)  (serial-resume k approved)
    (Err m) (do (println m) 0)))
```

## Best Practices

### Minimize Captured State

Only capture what you need. Use identifiers instead of entire objects: a
handler that closes over an `order-id` writes eight bytes; one whose frame
holds a large record writes the whole record through its instance.

### Limit Continuation Depth

Deep call stacks increase serialization time and size. Design workflows with
shallow stacks where possible -- and remember the capture grammar above: the
rest of the region should be a short chain ending in a named call.

### Version Long-Term Storage

`bytes->serial-cont` rejects a buffer whose frame names this program does not
know, which is the common failure after a deploy. To fail earlier and with a
better message, keep your own version beside the bytes -- a version in the
file name, or a header your storage layer writes -- and check it before
calling `bytes->serial-cont`.

### Security Considerations

Rebuilding a continuation from bytes is `eval` of whatever the bytes name.
`bytes->serial-cont` guarantees structural validity and that every frame is one
this program registered; it does not authenticate the writer. For anything
that crosses a trust boundary:

- sign the bytes (an HMAC over the buffer) and verify before rebuilding;
- keep an allowlist of the tokens/paths you will rebuild from;
- treat stored continuations like stored code, not like stored data.

## Comparison to Alternatives

| Approach | Pros | Cons |
|----------|------|------|
| **Serializable Continuations** | Automatic, composable, captures exact state | Requires Serializable typeclass |
| **Hand-Written State Machines** | Full control, no overhead | Tedious, error-prone, manual updates |
| **Green-Thread Snapshots** | Captures full thread state | Not portable, captures OS resources, complex |
| **Persistent Processes** | Live state, hot reloading | Requires VM support, not suitable for C target |
| **Application Image Dumps** | Warm-start (skip init) like Lisp/pdumper; build-stamp safe | Cross-binary pinned; only serializable continuations cross. See [image-dumps-guide.md](image-dumps-guide.md) |

The `save-cont!` / `resume-cont!` surface above is the SemVer-pinned API that
`stdlib/image.tur` (application image dumps) builds on (plan AI0.4).

## Residual Liveness Imprecision (1.0 limitation)

The same conservative capture check that applies to cloneable continuations
(`TUR-E0014`) applies here: bindings in scope at a `serial-shift` site inside
the same function are checked against `Serializable`, even if they are not
actually referenced in the continuation body.  Bindings from enclosing outer
functions are excluded (CF7.3), but same-function bindings that happen to be
in lexical scope may be flagged even if they are dead at the shift point.

Full precision requires the post-1.0 CPS liveness pass (tracked in
[control-flow-completeness-plan.md](https://github.com/turmeric-lang/turmeric/blob/main/docs/archive/history/control-flow-completeness-plan.md) CF7.5).

**Workaround:** consume or drop non-Serializable values before the shift point,
or restructure so only Serializable bindings remain in scope.

## See Also

- [Checkpointing Guide](checkpointing-guide.md) -- More examples of persistent workflows
- [Async/Await Guide](async-await-guide.md) -- One-shot continuations for async I/O
- [Logic Programming Guide](logic-programming-guide.md) -- Cloneable continuations for backtracking
- [Effects System Guide](effects-system-guide.md) -- Algebraic effects and custom control flow
