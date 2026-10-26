---
title: State Machines Guide
category: Advanced Control Flow
description: Five different approaches to modeling state machines in turmeric
tags:
  - effects
  - comparison
---

# State Machines in Turmeric

## Approaches Overview

Five approaches, from simplest to most sophisticated:

| Approach | Best for |
|----------|----------|
| Pure transition function | Simple, pure, easily testable machines |
| Algebraic effects | Transitions with side effects (I/O, logging) |
| Free monad | Pluggable interpretation / multiple backends |
| Arrow composition | Signal-processing pipelines with feedback |
| Serializable workflows | Machines that suspend and resume across restarts |

---

## Pure Transition Function

Model states and events as `defdata` variants, write a `transition` function
with nested `match`. Guards (`when`) handle conditional transitions.

```turmeric
(defdata State (Idle) (Running :int) (Done))
(defdata Event (Start) (Tick :int) (Stop))

(defn transition [state : State event : Event] : State
  (match state
    (Idle)      (match event
                  (Start)   (Running 0)
                  (Tick _)  (Idle)
                  (Stop)    (Idle))
    (Running n) (match event
                  (Tick x)  (Running (+ n x))
                  (Stop)    (Done)
                  (Start)   (Running n))
    (Done)      (Done)))
```

```sweet-exp
defdata State (Idle) (Running :int) (Done)
defdata Event (Start) (Tick :int) (Stop)

defn transition [state :State event :Event] :State
  match state
    (Idle)
    match event
      (Start)
      Running(0)
      (Tick _)
      Idle()
      (Stop)
      Idle()
    (Running n)
    match event
      (Tick x)
      Running({n + x})
      (Stop)
      Done()
      (Start)
      Running(n)
    (Done)
    Done()
```

---

## State + Algebraic Effects

When transitions need side effects, declare them with `defeffect` and keep the
transition logic pure. A `handle` block wires in the real implementation later,
making the machine testable with mock handlers.

```turmeric
(defeffect Emit [msg :cstr] :nil ^extends IO)
(defeffect Store [key :cstr val :int] :nil ^extends IO)

(defn step [state : State event : Event] : State
  (match state
    (Running n) (match event
                  (Tick x) (do
                    (perform (Emit "tick"))
                    (perform (Store "count" (+ n x)))
                    (Running (+ n x)))
                  ...)
    ...))
```

```sweet-exp
defeffect Emit [msg :cstr] :nil ^extends IO
defeffect Store [key :cstr val :int] :nil ^extends IO

defn step [state :State event :Event] :State
  match state
    (Running n)
    match event
      (Tick x)
      do
        perform(Emit("tick"))
        perform(Store("count" {n + x}))
        Running({n + x})
      ...
    ...
```

See `stdlib/effects.tur` and `docs/guides/effects-system-guide.md`.

---

## Free Monad

Use `Free` from `stdlib/free.tur` to describe transitions as data. The machine
produces a description of what it wants to do; a separate interpreter decides
how to execute it. Useful when you need multiple backends (testing, production,
simulation) without changing the machine definition.

See `stdlib/free.tur`.

---

## Arrow Composition

`stdlib/arrow.tur` provides `>>>` for sequential composition and `ArrowLoop`
for feedback. Suited to machines that are really signal-processing pipelines
where each stage transforms and routes values.

```turmeric
;;; route each input through pre-processing, then the core step
(>>> pre-process core-step)
```

```sweet-exp
;;; route each input through pre-processing, then the core step
>>>(pre-process core-step)
```

See `stdlib/arrow.tur`.

---

## Serializable Workflows

`stdlib/workflow.tur` provides `serial-shift`/`serial-reset` for machines that
need to suspend and resume across process restarts -- e.g. a multi-step approval
workflow that checkpoints its continuation to disk.

See `docs/guides/serializable-continuations-guide.md` and
`docs/guides/web-continuations-guide.md`.

---

## Recommendation

Start with **approach 1** (pure `defdata` + `match`). The combination of
`defdata`, `match`, and `when` guards is expressive enough for most machines.
Layer in algebraic effects (approach 2) when transitions need side effects,
keeping the transition logic itself pure. Reach for `Free` or `workflow` only
when you specifically need pluggable interpretation or persistence.
