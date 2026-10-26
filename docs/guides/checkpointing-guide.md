---
title: Checkpointing and Persistent Workflows
category: Advanced Control Flow
description: Cloneable continuations for persistent workflows and checkpointing
tags:
  - continuations
  - checkpointing
  - serialization
  - persistent-workflows
---

# Checkpointing and Persistent Workflows with Serializable Continuations

Save and resume computations across process boundaries using serializable continuations.

## Overview

Turmeric supports **serializable continuations**, enabling suspended computations to be marshalled to bytes, persisted to disk or sent over a network, then resumed in a fresh process. This enables:

- Persistent multi-step workflows (pause and resume days later)
- Distributed task migration (send half-finished computation to another node)
- Checkpointing (save state periodically; restart from checkpoint on crash)
- Web continuations (Racket-style: continuation as URL token)
- Mobile agents (send code + state to remote peer)
- Debugger snapshots (freeze and replay)

The concrete surface is small:

- `serial-reset` / `serial-shift` -- special forms that delimit a
  serializable region and capture the continuation (as an opaque
  `ptr<void>` handle).
- `save-cont!` / `resume-cont!` in `stdlib/workflow.tur` -- serialize a
  captured continuation to a bytes value, and rebuild + resume one from
  bytes. `workflow-suspend` / `workflow-resume` are aliases.
- `serial-cont->bytes` / `bytes->serial-cont` / `serial-resume` in
  `stdlib/serial.tur` -- the typed spellings: marshal a `serial-cont`,
  rebuild one as a `(Result serial-cont cstr)` (a foreign or damaged buffer
  is an `Err`, never an abort), and resume (`(k v)`).
- `cont-to-file` / `cont-from-file` in `stdlib/serial.tur` -- write/read
  the bytes value to/from disk.

This guide focuses on the checkpointing patterns built on top of that
surface. See also [serializable-continuations-guide.md](serializable-continuations-guide.md).

## Core Concept

Delimited continuations reify the call stack as a heap-allocated frame chain. Serialization walks this chain, emitting a stable encoding, then reconstructs it on load. `tests/fixtures/workflow-roundtrip/` locks the round trip.

```turmeric no-check
(load "stdlib/workflow.tur")

;; The serial-shift handler receives the captured continuation as an
;; opaque ptr<void>. Serialize it and write it to disk.
(defn save-to-disk [k : ptr<void>] : int
  (do
    (cont-to-file (save-cont! k) "continuation.dat")
    0))

;; Capture "the rest of the computation" (+ 1 _) and park it on disk.
(serial-reset (+ 1 (serial-shift save-to-disk 0)))

;; Later, in another process running the same binary:
(let [b (cont-from-file "continuation.dat")]
  (resume-cont! b 42))   ; runs (+ 1 42) => 43
```

```sweet-exp
load("stdlib/workflow.tur")

;; The serial-shift handler receives the captured continuation as an
;; opaque ptr<void>. Serialize it and write it to disk.
defn save-to-disk [k : ptr<void>] : int
  do
    cont-to-file(save-cont!(k) "continuation.dat")
    0

;; Capture "the rest of the computation" (+ 1 _) and park it on disk.
serial-reset {1 + serial-shift(save-to-disk 0)}

;; Later, in another process running the same binary:
let [b cont-from-file("continuation.dat")]
  resume-cont!(b 42)   ; runs (+ 1 42) => 43
```

`(serial-shift handler default)` takes exactly two arguments: a function
that receives the captured continuation, and a default expression. If the
handler resumes the continuation, the resumed computation's result becomes
the value of the enclosing `serial-reset`; if it does not (as above), the
handler's own return value does.

## Serialization Design

### Stable Frame Encoding

Function pointers are not portable across processes, so no code address is
ever written to the byte stream. Each captured frame is encoded as a
**stable name tag plus its environment value**; on deserialization the tag
is resolved back to the current binary's frame function through a
self-registered name table. Captured `cstr` and `Serializable` environment
values are copied into the buffer by content.

### The Serializable Typeclass

Not all captured values can be serialized. Types opt in via the
`Serializable` typeclass in `stdlib/serial.tur`:

```turmeric no-check
(defclass Serializable [a]
  (serialize [x] : ptr<void>)          ; marshal to a length-prefixed bytes value
  (deserialize [b : ptr<void>] : a))   ; unmarshal; panics on malformed input

;; Shipped instances: int, bool, float, cstr, ptr<void>, Pair, Option.
```

```sweet-exp
defclass Serializable [a]
  serialize [x] : ptr<void>            ; marshal to a length-prefixed bytes value
  deserialize [b : ptr<void>] : a      ; unmarshal; panics on malformed input

;; Shipped instances: int, bool, float, cstr, ptr<void>, Pair, Option.
```

`deserialize` panics on malformed input -- there is no `Result`-returning
variant at this level.

### Resource Types

File handles, sockets, and other system resources cannot be serialized
directly. The pattern is to serialize a **stable representation** and
re-acquire the resource on resume -- e.g. a `Serializable` instance for a
file-handle wrapper that serializes the *path* and re-opens the file in
`deserialize`. Keep the raw handle out of the captured frame and carry the
path (a `cstr`) instead.

### Ownership Model and Serialization

`save-cont!` copies the frame chain and its captured environment values into
a fresh, length-prefixed byte buffer that the caller owns. `resume-cont!`
rebuilds a brand-new chain from those bytes, so the resumed run shares no
heap state with the process that produced the checkpoint -- resuming in
another process is exactly as safe as resuming in the same one, which is
what `tests/fixtures/workflow-roundtrip/` demonstrates by round-tripping
through bytes and resuming in place.

## Example: Persistent Workflow

A multi-step business process that survives crashes:

```turmeric no-check
(defn process-order [order-id]
  ;; Step 1: Validate order
  (def order (load-order order-id))
  (when (not (valid-order? order))
    (panic "Invalid order"))
  (checkpoint "order-validated" order)

  ;; Step 2: Charge payment (slow network call)
  (def charge-result (charge-payment (order-payment-info order)))
  (checkpoint "payment-charged" charge-result)

  ;; Step 3: Fulfill order
  (def fulfillment (fulfill order charge-result))
  (checkpoint "order-fulfilled" fulfillment)

  fulfillment)

;; Checkpointing macro: persist the continuation, then keep going --
;; resuming through the serialized bytes, so the checkpoint is exercised
;; on every run (the workflow-roundtrip shape).
(defmacro checkpoint [name value]
  `(serial-shift
     (fn [k : ptr<void>] : int
       (let [b (save-cont! k)]
         (cont-to-file b (str-concat "checkpoint-" ~name ".bin"))
         (resume-cont! b ~value)))
     0))

;; On crash, resume from the last checkpoint file.
(defn resume-from-checkpoint [name : cstr value : int] : int
  (let [b (cont-from-file (str-concat "checkpoint-" name ".bin"))]
    (if (ptr-null? b)
      (panic "checkpoint missing or unreadable")
      (resume-cont! b value))))
```

```sweet-exp
defn process-order [order-id]
  ;; Step 1: Validate order
  def order load-order(order-id)
  when not(valid-order?(order))
    panic("Invalid order")
  checkpoint("order-validated" order)

  ;; Step 2: Charge payment (slow network call)
  def charge-result charge-payment(order-payment-info(order))
  checkpoint("payment-charged" charge-result)

  ;; Step 3: Fulfill order
  def fulfillment fulfill(order charge-result)
  checkpoint("order-fulfilled" fulfillment)

  fulfillment

;; Checkpointing macro: persist the continuation, then keep going --
;; resuming through the serialized bytes, so the checkpoint is exercised
;; on every run (the workflow-roundtrip shape).
defmacro checkpoint [name value]
  `(serial-shift
     (fn [k : ptr<void>] : int
       (let [b (save-cont! k)]
         (cont-to-file b (str-concat "checkpoint-" ~name ".bin"))
         (resume-cont! b ~value)))
     0)

;; On crash, resume from the last checkpoint file.
defn resume-from-checkpoint [name : cstr value : int] : int
  let [b cont-from-file(str-concat("checkpoint-" name ".bin"))]
    if ptr-null?(b)
      panic("checkpoint missing or unreadable")
      resume-cont!(b value)
```

## Example: Distributed Task Migration

Send a half-finished computation to another node:

```turmeric no-check
;; Node A: long-running job; after task1 it captures "the rest of the job"
;; and ships it to another node instead of running tasks 2 and 3 locally.
(defn ship-to-node-b [k : ptr<void>] : int
  (do
    (send-to-node-b (save-cont! k))
    0))

(serial-reset
  (let [task1-result (run-task1)
        handoff      (serial-shift ship-to-node-b 0)]
    (run-task3 (run-task2 handoff))))

;; Node B (same binary): resume with task1's result
(def result (resume-cont! (receive-bytes) task1-result))
```

```sweet-exp
;; Node A: long-running job; after task1 it captures "the rest of the job"
;; and ships it to another node instead of running tasks 2 and 3 locally.
defn ship-to-node-b [k : ptr<void>] : int
  do
    send-to-node-b(save-cont!(k))
    0

serial-reset
  let [task1-result run-task1()
       handoff      serial-shift(ship-to-node-b 0)]
    run-task3(run-task2(handoff))

;; Node B (same binary): resume with task1's result
def result resume-cont!(receive-bytes() task1-result)
```

## Example: Web Continuations (Racket-style)

Serialize "what to do when the form is submitted" as a URL token:

```turmeric no-check
;; Initial page: capture the continuation, store its bytes, hand back a token.
(defn park-checkout [k : ptr<void>] : int
  (let [token (save-continuation-to-db (save-cont! k))]
    (render-page
      (form :action (str-concat "/checkout-submit?token=" token)))))

(defn get-checkout [req]
  (serial-reset
    (checkout-flow (serial-shift park-checkout 0))))

;; Form submission handler: load the bytes and resume.
(defn post-checkout-submit [token req]
  (resume-cont! (load-continuation-from-db token)
                (parse-form-data req)))
```

```sweet-exp
;; Initial page: capture the continuation, store its bytes, hand back a token.
defn park-checkout [k : ptr<void>] : int
  let [token save-continuation-to-db(save-cont!(k))]
    render-page
      form(:action str-concat("/checkout-submit?token=" token))

defn get-checkout [req]
  serial-reset
    checkout-flow(serial-shift(park-checkout 0))

;; Form submission handler: load the bytes and resume.
defn post-checkout-submit [token req]
  resume-cont!(load-continuation-from-db(token)
               parse-form-data(req))
```

This is the pattern the guestbook example ships end-to-end -- see
[web-continuations-guide.md](web-continuations-guide.md) and
`examples/guestbook/`.

## Example: Checkpointing Long-Running Computation

Periodic snapshots for crash recovery:

```turmeric no-check
(defn snapshot-and-continue [k : ptr<void>] : int
  (let [b (save-cont! k)]
    (cont-to-file b "checkpoint-latest.bin")
    (resume-cont! b 0)))

(defn analyze-large-dataset [data]
  (defn checkpoint-every-n [n items]
    (let [processed []]
      (for-each-with-index items
        (fn [i item]
          (set! processed (conj processed (process item)))
          (when (= (mod (+ i 1) n) 0)
            ;; Checkpoint every n items, then keep going
            (serial-shift snapshot-and-continue 0))))))

  (checkpoint-every-n 1000 data))
```

```sweet-exp
defn snapshot-and-continue [k : ptr<void>] : int
  let [b save-cont!(k)]
    cont-to-file(b "checkpoint-latest.bin")
    resume-cont!(b 0)

defn analyze-large-dataset [data]
  defn checkpoint-every-n [n items]
    let [processed []]
      for-each-with-index items
        (fn [i item]
          set!(processed conj(processed process(item)))
          when {mod({i + 1} n) = 0}
            ;; Checkpoint every n items, then keep going
            serial-shift(snapshot-and-continue 0))

  checkpoint-every-n(1000 data)
```

## Reconstruction and Error Handling

Failures surface as null/zero sentinels and panics, not `Result` values:

- `save-cont!` returns `nil` when handed a nil continuation; otherwise a
  length-prefixed bytes buffer the caller owns.
- `cont-to-file` returns `1` on success, `0` on any I/O failure.
- `cont-from-file` returns `nil` (NULL) on any I/O or allocation failure --
  check it with `ptr-null?` before resuming.
- `resume-cont!` returns `0` when handed a nil bytes value.
- A `Serializable` `deserialize` of a malformed captured value panics.

```turmeric no-check
(let [b (cont-from-file "checkpoint.bin")]
  (if (ptr-null? b)
    (panic "cannot resume: checkpoint missing or unreadable")
    (resume-cont! b 0)))
```

```sweet-exp
let [b cont-from-file("checkpoint.bin")]
  if ptr-null?(b)
    panic("cannot resume: checkpoint missing or unreadable")
    resume-cont!(b 0)
```

**Same-build rule.** Frames are resolved by *name* against the running
binary's frame registry. A checkpoint written by a different build whose
frame names no longer match is **not detected** -- a missing name resolves
to a null frame and resuming it is undefined behavior. Only resume
checkpoints produced by the same binary, and version your checkpoint files
yourself (e.g. embed a build id in the filename or alongside the bytes).

## Performance Considerations

### Serialization Overhead

- **Small continuations (~1-10 frames):** Microsecond-scale serialization
- **Large continuations (100+ frames):** Millisecond-scale; consider streaming
- **Deep captured state:** Copy overhead proportional to state size

### Strategies

1. **Limit continuation depth** -- Design workflows to have shallow call stacks.
2. **Minimize captured state** -- Use identifiers (e.g., order ID) instead of entire objects.
3. **Lazy serialization** -- For large state, write once, reference by ID on resumption.
4. **Incremental checkpointing** -- Save deltas instead of full continuation.

## API Summary

```turmeric no-check
;; Special forms: delimit a serializable region / capture the continuation
(serial-reset body)
(serial-shift handler default)   ; handler : (fn [k : ptr<void>] ...)

;; stdlib/workflow.tur
(save-cont! k)                   ; k -> bytes ptr<void> (nil if k is nil)
(resume-cont! b v)               ; rebuild from bytes b, resume with v -> int
(workflow-suspend k)             ; alias for save-cont!
(workflow-resume b v)            ; alias for resume-cont!

;; stdlib/serial.tur file helpers
(cont-to-file b path)            ; write bytes; 1 on success, 0 on failure
(cont-from-file path)            ; read bytes; nil on failure

;; Checkpoint macro (example, defined above)
(checkpoint name value)
```

```sweet-exp
;; Special forms: delimit a serializable region / capture the continuation
serial-reset body
serial-shift handler default    ; handler : (fn [k : ptr<void>] ...)

;; stdlib/workflow.tur
save-cont!(k)                   ; k -> bytes ptr<void> (nil if k is nil)
resume-cont!(b v)               ; rebuild from bytes b, resume with v -> int
workflow-suspend(k)             ; alias for save-cont!
workflow-resume(b v)            ; alias for resume-cont!

;; stdlib/serial.tur file helpers
cont-to-file(b path)            ; write bytes; 1 on success, 0 on failure
cont-from-file(path)            ; read bytes; nil on failure

;; Checkpoint macro (example, defined above)
checkpoint(name value)
```

## See Also

- [Logic Programming Guide](logic-programming-guide.md) -- Cloneable continuations for backtracking
- [Async/Await Guide](async-await-guide.md) -- One-shot continuations for async I/O
- [Effects System Guide](effects-system-guide.md) -- Dynamic effect handling
