---
title: Opaque Types (`defopaque`)
category: Advanced Types
description: Named nominal newtypes over a representation type -- what they are, what they're for, and how to construct, unwrap, and combine them with substructural disciplines
---

# Opaque Types (`defopaque`)

An *opaque type* is a named, nominally-distinct newtype that wraps a single
representation type. At the C ABI it is the representation type and nothing
more -- so the wrapper costs zero bytes and zero instructions -- but to the
type checker it is a distinct type that does not unify with its
representation. That is the whole point: you get *type-level* separation
between, say, a file descriptor and the byte count you were about to pass
where the fd belongs, without paying for a struct.

```turmeric
(defopaque Fd     :int)
(defopaque Pid    :int)
(defopaque Chan   :ptr<void> :linear)
(defopaque Future :ptr<void> :affine)
```

The representation type is one of:

| Representation | When to use |
|---|---|
| `:int`        | A small integer handle (POSIX fd, OS pid, table index, generation-counted id, branded int). |
| `:ptr<void>`  | A pointer to a C-allocated control block (channel, mutex, reactor, opaque library handle). |
| `:ptr`        | A bare pointer when you don't need to thread the pointee type through. |

Both lower to `int64_t` at the C boundary, so an inline-C body that takes
or returns an opaque writes the C signature in terms of `int64_t` (and
casts to/from the real pointer type internally).

## What opaques are for

1. **Type-safe handles to C resources.** A `FILE*`, `pthread_mutex_t*`, or
   socket fd is just a pointer or int at the ABI. Wrapping it in a
   `defopaque` means the checker rejects passing an unrelated `:int` (a
   length, a byte count, the wrong handle) into the operations that expect
   it.
2. **Branded integers.** Two ids of the same shape but different meaning
   -- `UserId` and `RoomId`, `EventSourceId` and `TimerId` -- should be
   distinct so a swap is a compile error, not a 3 a.m. page.
3. **Hiding representation across module boundaries.** A library can
   export `(defopaque Token :int)` and never reveal whether the token is
   an array index, a hash, or a serial number. The consumer sees only the
   accessor functions you choose to export.
4. **Attaching a substructural discipline.** Adding `:linear` or
   `:affine` makes the handle *exactly-once* or *at-most-once* -- the checker enforces that a
   `Chan :linear` is consumed by exactly one `chan-free` call, ruling
   out leaks and double-frees.

Opaques are *not* for:

- Multi-field aggregates -- use `defstruct`.
- Sum types / tagged unions -- use `defdata` or `defgadt`.
- Hiding the body of a polymorphic abstraction (e.g. "some `T` with a
  `Show` instance") -- use existentials (see
  [existential-types-guide.md](existential-types-guide.md)).

## Defining an opaque

```
(defopaque Name :rep-type)
(defopaque Name :rep-type :linear)
(defopaque Name :rep-type :affine)
(defopaque Name :rep-type :sealed)          ; see below
(defopaque Name :rep-type :affine :sealed)  ; attributes compose
```

The optional trailing keywords are a *set*. `:linear` and `:affine`
promote the newtype to a substructural handle -- without either, the
opaque is freely copyable (`is_copy = true`); with one it becomes a
resource the checker tracks for single use. The two are mutually
exclusive ("exactly once" and "at most once" are contradictory claims).

`:sealed` is orthogonal to those: it governs *who may use `::` on the
type*, not how many times a value may be used, so it composes with
either. See [Sealing an opaque](#sealing-an-opaque-sealed) below.

The C ABI of a substructurally-marked opaque is identical to a freely
copyable one -- the handle still lowers to `int64_t`; only the
elaborator's usage tracking changes. See
[substructural-types-guide.md](substructural-types-guide.md) for the
discipline; see
[uniqueness-types-guide.md](uniqueness-types-guide.md) for the
`^unique` alternative.

A `defopaque` produces no constructors or accessors of its own. You
provide those as ordinary `defn`s, using the `(:: expr :Type)` cast form
to move between the wrapper and its representation.

## Constructing and unwrapping: `(:: expr :Type)`

The `(::)` form is a *type ascription* that doubles as the bridge between
an opaque and its representation. It compiles to nothing at the C level
-- only the static type of `expr` changes.

```turmeric
(defopaque Fd :int)

;; Wrap a raw int into an Fd:
(defn int->fd [n : int] : Fd (:: n :Fd))

;; Unwrap an Fd back to int:
(defn fd->int [fd : Fd] : int (:: fd :int))

;; Use either side as needed:
(defn fd-valid? [fd : Fd] : bool (>= (:: fd :int) 0))
```

The same pattern works for `:ptr<void>` opaques:

```turmeric
(defopaque Chan :ptr<void> :linear)

;; Inside an inline-C body the C type is int64_t; cast as you would
;; any other handle:
(defn chan-new [cap : int] : Chan
  ```c
  ChanBlock *ch = (ChanBlock *)malloc(sizeof(ChanBlock));
  /* ... init ch ... */
  return (int64_t)(intptr_t)ch;
  ```)

(defn chan-send [^borrow ch : Chan val : int] : nil
  ```c
  ChanBlock *c = (ChanBlock *)(intptr_t)ch;
  /* ... use c ... */
  return 0;
  ```)
```

Conventions worth following:

- Provide `name->rep` and `rep->name` helpers (`fd->int` / `int->fd`)
  if external callers need the raw representation -- it's cheaper than
  exporting `(::)` casts at every call site, and it gives you a place to
  hang `;;;` docstrings, validation, or future invariant checks.
- Keep the `(::)` cast inside the wrapper module. Consumers should be
  able to use your opaque without writing a cast.
- For `:int` handles that have a sentinel error value (e.g. `-1` for
  POSIX fds), expose a `name-valid?` predicate instead of forcing
  callers to compare integers.

## Sealing an opaque: `:sealed`

> `:sealed` enforces unconditionally. `--enable=sealed-opaque` (the retired
> experiment gate) was a warning-only no-op through 0.37.0 and is a hard
> `TUR-E0310` from 0.38.0 -- a `build.tur` still naming it needs the flag
> removed. See
> [sealed-opaque-plan.md](https://github.com/turmeric-lang/turmeric/blob/main/docs/archive/sealed-opaque-plan.md).

`::` is a **coercing** cast, not a checked one. That means a plain
`defopaque` does *not* encapsulate its handle: any module can unwrap a
value to the representation and re-wrap the result as a fresh value of
the opaque type. Both directions compile, anywhere.

That is fine for interop, and it silently bounds every guarantee built
on top of an opaque handle:

```turmeric
(let [__b (& w)]                 ; w is immutably borrowed
  (let [w2 (:: (:: w :int) H)]   ; unwrap, re-wrap -> a NEW handle
    (h-bump! w2)))               ; w2 is OWNED, not the borrowed w: no TUR-E0200
```

Mutating the borrowed `w` directly is correctly rejected. Mutating the
alias is not -- so a uniqueness-based argument ("no second mutable
handle can exist here") does not survive one `::`.

`:sealed` closes that. Inside the declaring module `::` behaves as
always; outside it, both directions are `TUR-E0302`:

| use site | `(:: n H)` | `(:: h :int)` |
| --- | --- | --- |
| declaring module | allowed | allowed |
| any other module | **TUR-E0302** | **TUR-E0302** |

Sealing the *unwrap* direction as well as fabrication is what makes the
representation genuinely private rather than merely awkward to rebuild:
once the raw carrier escapes, inline-C can do anything with it.

**What `:sealed` does not claim.** It is a compile-time discipline over
the `::` surface, not a capability and not a runtime protection.
inline-C in any module can still cast an `int64_t` to whatever it likes.
So sealing raises an aliasing bypass from "one `::` away, in ordinary
code" to "requires deliberate inline-C" -- which is a real improvement,
and is the honest claim to make in your module's docs. If you document a
sealed handle as an adversarial guarantee, you are overselling it.

**Moduleless code is not separated, and that is the accepted behavior --
not a gap waiting to be closed.** A `defopaque` outside any `defmodule`
belongs to the implicit top-level module, so two moduleless files both
count as "the declaring module" and `::` is allowed between them. This
matches how the rest of the module system treats moduleless code, and
changing it would mean inventing a per-file notion of module that exists
only for this one check. If you want a handle sealed, put it in a
`defmodule` -- which is where a library that has something worth sealing
already lives. Single-file programs are where sealing has the least to
offer anyway.

## Inline-C and the ABI

Every opaque -- whether `:int` or `:ptr<void>` -- lowers to `int64_t` at
the C boundary. An inline-C body always sees `int64_t` parameters and
returns `int64_t`; cast to your real pointer or int type inside the
block.

```turmeric
(defopaque Pid :int)

(defn pid-kill [p : Pid sig : int] : int
  ```c
  return kill((pid_t)p, (int)sig);
  ```)
```

```turmeric
(defopaque Reactor :ptr<void> :linear)

(defn reactor-new [] : Reactor
  ```c
  TurReactor *r = tur_reactor_new();
  return (int64_t)(intptr_t)r;
  ```)
```

This uniform `int64_t` carrier is the same mechanism described in
[type-erasure-guide.md](type-erasure-guide.md); opaques are one of the
three sites where the compiler collapses high-level types to the carrier.

When the constructor is **fallible** -- it acquires the handle in C and
can fail -- hand back a typed `(Result Handle E)` / `(Option Handle)`
rather than the bare opaque, built with the preamble helpers
`tur_ok_ptr` / `tur_err_int` / `tur_some_ptr` / `tur_none`. See
[inline-c-results-guide.md](inline-c-results-guide.md) for the worked
pattern.

## Substructural opaques

Adding `:linear` or `:affine` flips two checker bits on the underlying
`StructDef` (`is_copy = false`, `is_linear` / `is_affine = true`):

- A `:linear` handle must be consumed exactly once. Letting it go out of
  scope or copying it is `TUR-E0100`; using it twice is `TUR-E0101`.
- A `:affine` handle must be consumed at most once -- dropping it is
  fine, double-use is not.
- Multi-read operations annotate their handle parameter with `^borrow`
  so they observe the handle without consuming it. Only the
  `*-free`/`-close`/`-shutdown` operation does the actual consume. See
  `stdlib/chan.tur` for a worked example.

## Inheriting an opaque

There is no inheritance. A `defopaque` is a leaf node in the type lattice
-- it does not unify with its representation, nor with any other opaque,
even one declared with the same representation. To "convert" between two
opaques you write a function that unwraps to the shared representation
and wraps back:

```turmeric
(defopaque Celsius    :float)
(defopaque Kelvin     :float)

(defn celsius->kelvin [c : Celsius] : Kelvin
  (:: (+ (:: c :float) 273.15) :Kelvin))
```

## Phantom type parameters

`defopaque` accepts an optional type-parameter vector between the name and
the base type:

```turmeric
(defopaque NonEmpty [A] :int)
(defopaque Const [r a] :int)
```

The carrier stays the declared base type (always `int64_t` at the C
level), but the newtype becomes a type constructor -- it can be spelled
`(NonEmpty A)` in annotations and track an element or index type at the
type level without storing it.

## Real-world examples

| Module | Opaque | Shape |
|---|---|---|
| [`stdlib/fd.tur`](https://github.com/turmeric-lang/turmeric/blob/main/stdlib/fd.tur) | `Fd` | `:int` -- POSIX file descriptor, `-1` is the error sentinel |
| [`stdlib/process.tur`](https://github.com/turmeric-lang/turmeric/blob/main/stdlib/process.tur) | `Pid`, `ChildHandle` | `:int` (one `:linear`) -- OS process ids |
| [`stdlib/chan.tur`](https://github.com/turmeric-lang/turmeric/blob/main/stdlib/chan.tur) | `Chan`, `AsyncChan` | `:ptr<void> :linear` -- channel control blocks |
| [`stdlib/future.tur`](https://github.com/turmeric-lang/turmeric/blob/main/stdlib/future.tur) | `Promise`, `Future` | `:ptr<void>` with `:linear` / `:affine` -- write end vs read end of the same `FutureCell` |
| [`stdlib/threadpool.tur`](https://github.com/turmeric-lang/turmeric/blob/main/stdlib/threadpool.tur) | `WorkQueueHandle`, `ThreadPoolHandle`, `DynThreadPoolHandle`, `FutureHandle` | `:ptr<void>` -- static and dynamic pools have distinct block layouts and are nominally distinct |
| [`stdlib/thread.tur`](https://github.com/turmeric-lang/turmeric/blob/main/stdlib/thread.tur) | `ThreadHandle` | `:ptr<void>` -- returned by `thread-spawn-fn`; `thread-join` / `-detach` / `cancel-thread` take it |
| [`stdlib/fiber.tur`](https://github.com/turmeric-lang/turmeric/blob/main/stdlib/fiber.tur) | `FiberHandle` | `:ptr<void>` -- consumed by `fiber-resume` / `-free` / scheduler unpark |
| [`stdlib/mutex.tur`](https://github.com/turmeric-lang/turmeric/blob/main/stdlib/mutex.tur), [`stdlib/condvar.tur`](https://github.com/turmeric-lang/turmeric/blob/main/stdlib/condvar.tur), [`stdlib/rwlock.tur`](https://github.com/turmeric-lang/turmeric/blob/main/stdlib/rwlock.tur) | `Mutex`, `CondVar`, `RwLock` | `:ptr<void>` -- `condvar-wait [c : CondVar m : Mutex]` rejects transposed callers |
| [`stdlib/taskgroup.tur`](https://github.com/turmeric-lang/turmeric/blob/main/stdlib/taskgroup.tur) | `TaskGroup`, `TaskHandle` | `:ptr<void>` -- `task-group-join [group : TaskGroup handle : TaskHandle]` |
| [`stdlib/reactor.tur`](https://github.com/turmeric-lang/turmeric/blob/main/stdlib/reactor.tur) | `Reactor`, `EventSourceId` | mixed -- pointer for the reactor, branded `:int` for the source id |
| [`stdlib/atomic.tur`](https://github.com/turmeric-lang/turmeric/blob/main/stdlib/atomic.tur) | `AtomicCell` | `:ptr<void>` -- pointer to a heap-allocated atomic word |
| [`stdlib/stm.tur`](https://github.com/turmeric-lang/turmeric/blob/main/stdlib/stm.tur) | `TVar` | `:ptr` -- transactional-variable handle, distinct from the boxed `:ptr` values it holds |
| [`stdlib/timer.tur`](https://github.com/turmeric-lang/turmeric/blob/main/stdlib/timer.tur) | `TimerId` | `:int` -- branded handle returned by `reactor-add-timer` |
| [`stdlib/fs.tur`](https://github.com/turmeric-lang/turmeric/blob/main/stdlib/fs.tur) | `StatInfo`, `TmpFile` | `:int` -- stat block vs temp-file handle; cannot be transposed |
| [`stdlib/io.tur`](https://github.com/turmeric-lang/turmeric/blob/main/stdlib/io.tur) | `FileHandle`, `FileStream`, `DirListing`, `FileSystem` | `:ptr<void>` -- `FileHandle` is `:linear`; `FileStream` wraps `FILE*` |
| [`stdlib/ref.tur`](https://github.com/turmeric-lang/turmeric/blob/main/stdlib/ref.tur) | `RefHandle` | `:int` -- heap pointer from `ref-new`, distinct from the `Ref` struct |

Read those modules for the full pattern: a `defopaque` declaration
immediately followed by the constructor (`*-new`), one or more
borrow-taking operations, and a consuming `*-free` (when the resource is
substructural).

## See also

- [structs-guide.md](structs-guide.md) -- when you need named fields,
  not just a wrapper.
- [substructural-types-guide.md](substructural-types-guide.md) -- the
  `:linear` / `:affine` discipline in depth.
- [uniqueness-types-guide.md](uniqueness-types-guide.md) -- `^unique`,
  an orthogonal ownership story.
- [c-integration-guide.md](c-integration-guide.md) -- inline-C blocks,
  the `int64_t` carrier, and the FFI boundary.
- [type-erasure-guide.md](type-erasure-guide.md) -- where opaques sit
  in the compiler's erasure story.
