# Plan: `reactor-run-fibers` -- Local Fiber Driver on Top of `tur/reactor`

> **Status:** SHIPPED (F1-F8). Archived 2026-05-31.
> F1-F8 landed `LocalFiberGroup` + `local-spawn` + `reactor-run-fibers` +
> `local-park-{fd,chan}` in `src/async/reactor.c` / `stdlib/reactor.tur`,
> with the five fixtures listed under "Module layout" all passing. F9 (the
> global-scheduler rewrite) was always out of scope here and is now tracked
> separately in
> `docs/upcoming/scheduler-on-local-fiber-group-plan.md`.
> **Last Updated:** 2026-05-31
> **Type:** Stdlib Design + Implementation Roadmap
> **Related:**
> - `docs/archive/history/tur-reactor-plan.md` -- parent plan; R1-R8 shipped, this plan covers the stretch R9 item
> - `stdlib/reactor.tur` -- the reactor module this builds on
> - `src/async/scheduler.c`, `src/async/timer_wheel.c` -- the existing global fiber scheduler
> - `stdlib/async_socket.tur`, `stdlib/async_file.tur`, `stdlib/async_pipe.tur` -- fiber-blocking I/O
> - `docs/guides/reactor-guide.md` -- user-facing guide that currently flags this as future work
> - `docs/guides/async-await-guide.md` -- the global-scheduler counterpart

---

## Overview

`reactor-run-fibers` is a thin convenience driver that lets a caller run
a *local* group of fibers on top of a `tur/reactor` instance, without
taking over (or even starting) the global cooperative scheduler. It is
the third programming style sketched in the reactor guide, and the only
one not yet implemented.

The shape is small:

- A caller owns a `Reactor` (one per thread, as today).
- They spawn fibers into a `LocalFiberGroup` bound to that reactor.
- `reactor-run-fibers` pumps the local ready-queue between
  `reactor-poll` calls, and maps fiber park/unpark onto
  `reactor-add-fd` / source removal.
- When the group is empty and no sources remain, the driver returns.

This is also the design vehicle for a follow-up rewrite of
`src/async/scheduler.c` -- once `reactor-run-fibers` is the canonical
fiber driver, the global scheduler can become "the process-wide reactor's
fiber driver" without an API break for either layer.

---

## Why split this out from the reactor plan

The parent reactor plan delivered the I/O multiplexer (R1-R8 are
complete; see `docs/archive/history/tur-reactor-plan.md`). R9 was flagged there
as explicitly out-of-scope and listed only to anchor the API. Promoting
it to its own plan lets us:

- Treat scheduler interop as its own design conversation (fiber park
  semantics, cancellation, channel select integration) rather than a
  footnote.
- Keep the shipped reactor doc archived and stable.
- Make the dependency on the global-scheduler rewrite explicit instead
  of buried.

There is no new C dependency in this plan either. Everything composes
existing pieces: `Reactor`, the fiber context-switch primitives already
used by `src/async/scheduler.c`, and the channel waiter infrastructure
extended in R6.

---

## API sketch

`reactor-run-fibers` lives alongside `reactor-run` in
`stdlib/reactor.tur`. The handle type is new; everything else is
existing reactor surface.

### Types

```turmeric
;; Opaque local-fiber-group handle. One group is bound to one reactor for its
;; lifetime; do not share across threads (same rule as Reactor itself).
(defopaque LocalFiberGroup)
```

### Lifecycle

```turmeric
;; Create a fiber group bound to `r`. The group borrows `r`; do not free
;; the reactor while the group is alive.
(defn local-fiber-group-new [r : Reactor] : LocalFiberGroup ...)

;; Free the group. Any fiber still parked is cancelled (its park source
;; is removed from the reactor and the fiber's stack is unwound).
(defn local-fiber-group-free [g : LocalFiberGroup] : nil ...)
```

### Spawning and running

```turmeric
;; Spawn a fiber into the group. Returns a fiber id (>= 0), -1 on error.
;; The fiber starts on the next `reactor-run-fibers` tick.
(defn local-spawn
  [g : LocalFiberGroup
   body : fn<:ptr<void> :nil>
   user-data : ptr<void>] : int ...)

;; Drive the reactor and pump the group's ready-queue until either:
;;   - the group is empty AND the reactor has no remaining sources, or
;;   - `reactor-stop` is called on the bound reactor.
;; Returns the number of fibers that ran to completion.
(defn reactor-run-fibers [g : LocalFiberGroup] : int ...)
```

### Park / unpark bridge

Fiber-blocking I/O inside a local-group fiber must park on the *bound*
reactor, not the global scheduler. The bridge is a small park helper
that local-group-aware I/O wrappers call:

```turmeric
;; Park the currently-running fiber on `fd` waiting for `events`. The
;; fiber resumes when the reactor fires the source; the source is
;; removed before the fiber runs again (one-shot semantics, like
;; scheduler park today).
;;
;; Must be called from inside a `local-spawn`ed fiber. Outside of one
;; (e.g. from a bare callback), returns -1 and leaves the caller running.
(defn local-park-fd
  [g : LocalFiberGroup
   fd : int
   events : int
   timeout-ms : int] : int ...)
```

The channel analogue uses the R6 channel bridge:

```turmeric
(defn local-park-chan
  [g : LocalFiberGroup
   ch : Chan<:int>] : int ...)
```

These two are enough to retarget `async-socket-*`, `async-file-*`, and
`async-pipe-*` at a local group via a thin compile-time switch
(`*current-fiber-driver*`-style dynvar, not detailed here).

---

## Threading model

Same as the reactor:

- `LocalFiberGroup` is **not** thread-safe.
- The group, its reactor, and every fiber spawned into it live on one
  OS thread.
- Cross-thread handoff happens via channels + `reactor-wake`, exactly as
  the reactor plan describes.

This deliberately mirrors the parent plan so callers do not have to
learn a second concurrency model.

---

## Relationship to `src/async/scheduler.c`

The follow-up rewrite of the global scheduler is **out of scope** for
this plan but is the reason the API shape matters:

- After this plan ships, the global scheduler can be implemented as a
  process-wide `Reactor` + `LocalFiberGroup` pair, with `spawn` /
  `await` desugared into `local-spawn` / `local-park-*` calls against
  that pair.
- The migration is purely internal: the public `spawn`/`await` surface
  does not change.
- `async-socket-*` and friends keep their current signatures; only
  their park implementation switches to call `local-park-fd` against
  whichever group is "current" on this thread.

This plan does **not** ship that migration. It ships the building block.

---

## Module layout

```
stdlib/
  reactor.tur                    -- adds LocalFiberGroup + local-spawn +
                                    reactor-run-fibers + local-park-{fd,chan}
src/async/
  reactor.c                      -- adds the fiber pump + park bridge
                                    (~150 lines; reuses fiber ctx switch
                                    primitives already in scheduler.c)
tests/fixtures/
  reactor-fibers-smoke/          -- one fiber prints + exits
  reactor-fibers-park-fd/        -- two fibers, pipe pair, one writes
                                    and the other parks on READ
  reactor-fibers-park-chan/      -- park on a channel, unpark on send
  reactor-fibers-cancel-on-free/ -- local-fiber-group-free cancels parked
                                    fiber and unregisters its source
  reactor-fibers-stop-mid-run/   -- reactor-stop from inside a fiber
                                    unwinds the driver cleanly
```

---

## Phases

| Step | Task |
|---|---|
| F1 | Extract the fiber context-switch + stack-management bits from `src/async/scheduler.c` into a reusable internal header (no behavior change to the global scheduler) |
| F2 | `LocalFiberGroup` type + `local-fiber-group-new` / `-free`; ready-queue data structure (linked list keyed by fiber id, mirrors scheduler's) |
| F3 | `local-spawn` + `reactor-run-fibers` driver loop; smoke fixture (one fiber, no I/O) |
| F4 | `local-park-fd` + park bridge into `reactor-add-fd`; pipe-pair fixture |
| F5 | `local-park-chan` reusing the R6 channel bridge; channel fixture |
| F6 | Cancellation on `local-fiber-group-free` (unregister sources, unwind stacks); fixture |
| F7 | `reactor-stop` interaction (driver returns cleanly mid-tick); fixture |
| F8 | Docstrings + `docs/guides/reactor-guide.md` Style 3 update (replace "future milestone" with a worked example) |
| F9 | (Follow-up, separate plan) Rewrite `src/async/scheduler.c` on top of `LocalFiberGroup` + a process-global reactor |

F1-F4 are the MVP and unblock a `tur/httpd` worker that wants to
multiplex many keep-alive connections via fibers without booting the
global scheduler. F5-F7 round out the surface. F8 is shipping
discipline. F9 is explicitly out-of-scope and listed only to anchor the
API.

---

## Non-goals

- **No global-scheduler rewrite in this plan.** That is the follow-up
  this plan unblocks; it gets its own design doc.
- **No multi-group work-stealing.** A `LocalFiberGroup` runs only the
  fibers spawned into it, on the thread that owns it. Cross-group work
  is explicit via channels + `reactor-wake`.
- **No new I/O primitives.** This plan only adds a fiber driver. New
  user-facing async wrappers (e.g. local-group variants of
  `async-socket-*`) are a downstream concern.
- **No fiber-local storage.** Out of scope; revisit if a consumer needs
  it.
- **No structured concurrency / nursery API.** A `LocalFiberGroup` is a
  flat bag of fibers; richer lifetimes (cancellation scopes, supervisor
  trees) are a separate design conversation.

---

## Open questions

- **Park timeout semantics.** `local-park-fd` takes a `timeout-ms`; on
  timeout it should resume the fiber and return a sentinel (`-2`?
  distinct from the `-1` "not in a fiber" error). Confirm the sentinel
  before locking in the API.
- **Re-entrant `reactor-run-fibers`.** Calling it from inside a fiber
  spawned by the same group should be a hard error (deadlock-prone).
  Decide whether to enforce at runtime or document only.
- **Interaction with the global scheduler.** If both are running on the
  same thread (e.g. someone calls `reactor-run-fibers` from a global
  fiber), behavior should be defined: probably "park the outer fiber
  for the duration." Needs a fixture either way.
- **Cancellation cleanup hooks.** When
  `local-fiber-group-free` cancels parked fibers, do we run any
  user-registered cleanup (defer-style)? Defer until a real consumer
  needs it.
