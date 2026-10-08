---
title: Mutable Globals Guide
category: Language Basics
description: def ^mut, what the compiler checks about a global write, and the concurrency story -- including what it deliberately does not cover
---

# Mutable Globals Guide

> Everything on this page is unconditional -- naming a global in a write frame,
> the read-only-outside-the-module rule, `^atomic`, and `^thread-local` need
> no `--enable` flag. `--enable=global-state` was a warning-only no-op through
> 0.37.0 and is a hard `TUR-E0310` from 0.38.0; delete it. The
> design of record is
> [`docs/archive/mutable-globals-plan.md`](https://github.com/turmeric-lang/turmeric/blob/main/docs/archive/mutable-globals-plan.md).

## The form

`^mut` on a top-level `def` gives a global with static storage that `set!` may
write:

```turmeric
(def ^mut hits 0)

(defn hit [] : void
  (set! hits (+ hits 1)))
```

Without `^mut` a global is immutable and `set!` on it is an error. `^mut` is the
only thing that grants write permission -- no other annotation confers it as a
side effect.

Initialization runs in source order before `main`, so a `def` may read an
earlier global. A forward reference is `TUR-E0003`, not a zero-initialized
surprise.

### Any type may be bound at module level

There is no restriction on the *type* of a global. In particular, a value whose
type comes from `defopaque` -- including one declared `:linear` -- is an
ordinary global:

```turmeric
(def hub-mutex (mutex-new))     ; Mutex is (defopaque Mutex :ptr<void> :linear)

(defn broadcast [msg : cstr] : nil
  (mutex-lock hub-mutex)
  ...
  (mutex-unlock hub-mutex))
```

A process-lifetime mutex, connection pool, or shared registry behind a
module-level `def` is the intended spelling. Linearity is satisfied by the
initializer: the value is produced once, at static-init, and lives for the
process -- one production, one lifetime.

You may see older code holding the carrier and casting back at each borrow
(`(def hub-mutex (:: (mutex-new) :int))`, then `(:: hub-mutex Mutex)`). That was
a workaround for a codegen bug that emitted no storage for such a global while
still emitting references to it; it was fixed 2026-08-29. It costs an unchecked
cast at every use, so replace it with the direct spelling.

## Reach for something else first

A mutable global is a name every function in the program can read and, inside
its module, write. That is occasionally what you want and usually not.

| If you want | Prefer |
|---|---|
| a value a call tree should see, that callers can rebind | a **dynamic variable** -- `defdynamic` / `binding`, see [dynamic-vars-guide.md](dynamic-vars-guide.md) |
| state owned by one subsystem | a struct threaded through its functions, or a `defopaque` handle |
| a constant | a plain `def` -- no `^mut` |
| one scratch value per thread | `^thread-local` (below), not a shared global plus a lock |
| a genuinely process-wide counter, cache, or registry | a mutable global |

The dynvar case is the one most often mistaken for a global: it is the same
per-thread machinery with a scope attached, and it does not leave a name any
function can write.

## What the compiler checks

### A global write is invisible to `#fx{}`

An effect row tracks *algebraic* effects. It infers nothing from `set!`, from a
mutable global, or from inline C, so a function that writes a global can
truthfully declare `#fx{}`. That is not a gap to be fixed in `#fx{}` -- an
empty row is a veto, not evidence -- and it is why the write-frame vocabulary
below exists instead.

### A `#writes` frame may name a global

A `#writes` frame names **parameters and mutable globals**, so a body that
maintains global state can say so and be checked, instead of being declined
outright for writing something the frame had no way to name:

```turmeric
(def ^mut hits 0)

(defn bump! [] #writes [hits] : void
  (set! hits (+ hits 1)))
```

| Body | Verdict |
|---|---|
| writes only globals the frame names | VERIFIED |
| writes a global the frame does not name | `TUR-E0382`, naming the global |
| the walk cannot tell | UNVERIFIED (silent) |

Declared-but-never-written is fine -- a frame is an *upper bound*. Frames may
mix parameters and globals: `#writes [a hits]`.

Frames are checked (WF2's three verdicts) as of 0.37.0, when the `write-frames`
experiment graduated -- before that a frame parsed and imposed nothing unless
you passed `--enable=write-frames`.

`#reads` is deliberately **not** part of this and still rejects a non-parameter
name. It is the annotation that *grants* congruence, so a global there would let
a promise about mutable global state pay out in proofs. See
[stateful-refinements-guide.md](stateful-refinements-guide.md).

### An exported global is read-only outside its module

A module that exports a counter for reading does not thereby export it for
writing:

```turmeric
(defmodule ctr
  (export hit peek hits)
  (def ^mut hits 0)
  (defn hit [] : void (set! hits (+ hits 1)))
  (defn peek [] : int hits))
```

An importer may read `hits` and call `hit`. `(set! hits 99)` from outside is an
error naming the owning module. To permit it, the module says so at the
definition site:

```turmeric
(export hit peek (mut hits))    ;; writable from outside
```

The permission belongs in the export list because it is a statement about the
module's interface. `(mut ...)` on a function or an immutable global is rejected
by name.

This only applies across a real module boundary -- a single-file program has no
owning module and is untouched.

## Concurrency

**A plain `^mut` global has no synchronization of any kind.** Two threads
writing one is a data race, and nothing diagnoses it. Turmeric has real OS
threads (`thread-spawn-fn`, `stdlib/threadpool.tur`, `stdlib/future.tur`), so
this is reachable from ordinary code.

Two annotations help, each with a narrow job.

### `^atomic` -- one shared value, indivisible accesses

```turmeric
(def ^atomic ^mut ready 0)
```

Every read becomes a sequentially-consistent load and every `set!` a
sequentially-consistent store. That buys three things: no torn access, no
hoisting (a bare global read in a loop may be cached in a register, so a
spinning reader would otherwise never see another thread's store), and
sequentially-consistent ordering.

**It does not make `(set! c (+ c 1))` safe.** That is a load *then* a store, not
an atomic read-modify-write; two threads still lose updates. `^atomic` makes
each half indivisible, it does not fuse them. For a counter use
`stdlib/atomic.tur`'s CAS or fetch-add; for anything wider use
[`stdlib/mutex.tur`](threading-guide.md).

Eight-byte scalars only -- `:int`, `:float`, `:cstr`, `:ptr`. A narrower or
wider type is rejected with a reason. `^atomic` does not imply `^mut`.

### `^thread-local` -- one copy per thread

```turmeric
(def ^thread-local scratch (make-buffer))
```

Each thread gets its own copy, materialized on first access and initialized by
running the initializer **on that thread** -- so each thread gets its own
buffer, not a share of one. The copy is freed when the thread exits.

It does not combine with `^atomic`: a per-thread copy is unshared, so atomic
accesses would suggest a synchronization that is not happening. Its initializer
may not reference another `^thread-local`, because per-thread initialization
order would otherwise become observable.

Under `tur --interpret` it is a plain global. turi has no user-reachable thread
spawn, so there is no second thread for it to differ on.

### What is not covered

Said plainly, because the annotations above can read as more than they are:

- **Nothing here makes a program data-race free.** `^atomic` and
  `^thread-local` are opt-in tools for two specific shapes. A `^mut` global
  without either is unsynchronized, and no diagnostic will say so.
- **No compound atomic updates.** Read-modify-write, compare-and-swap loops, and
  multi-variable invariants are yours to build, out of `stdlib/atomic.tur` and
  `stdlib/mutex.tur`.
- **No lock ordering, no deadlock detection, no race detection.** If you take
  two locks, their order is your problem.
- **No happens-before reasoning in the type system.** `^atomic` gives you
  sequential consistency at the access; it does not let the compiler prove
  anything about what another thread observed.

If you need more than "one indivisible value" or "one copy per thread", you need
a lock and the discipline that goes with it. The compiler will not check that
discipline for you.

## Quick reference

| Annotation | Position | Effect | Gate |
|---|---|---|---|
| `^mut` | top level | `set!` is allowed | none |
| `^atomic` | top level | accesses are sequentially consistent; needs `^mut`; 8-byte scalars | none |
| `^thread-local` | top level | one copy per thread, initialized per thread | none |
| `#writes [g]` | on a `defn` | the frame may name a global, and is checked | none |
| `(export (mut g))` | in `defmodule` | importers may write `g` | none |

## See also

- [binding-forms-guide.md](binding-forms-guide.md) -- `def` in both positions, and the full annotation table
- [dynamic-vars-guide.md](dynamic-vars-guide.md) -- the scoped alternative to a mutable global
- [threading-guide.md](threading-guide.md) -- threads, mutexes, and atomics
- [stateful-refinements-guide.md](stateful-refinements-guide.md) -- `#reads` / `#writes` frames
- [module-system-guide.md](module-system-guide.md) -- exports and imports
