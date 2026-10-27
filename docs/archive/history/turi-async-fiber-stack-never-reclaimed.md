---
title: Async fiber stacks are never reclaimed until env teardown (O(N) growth)
category: Reported
description: Every (async ...) mmap'd a fresh 512 KB fiber stack that was only
  released at env teardown, so a program that spawns N async fibers over its
  lifetime held N x 512 KB of mappings even though all but a bounded set were
  long since done. RESOLVED (2026-07-06) by reclaiming a fiber's stack the
  moment the scheduler observes it in TURI_FIBER_DONE, tombstoning the matching
  coro-stack node so teardown does not double-free.
---

# Async fiber stacks are never reclaimed until env teardown (O(N) growth)

**Status:** RESOLVED (2026-07-06). Fiber stacks are now munmap'd early, on the
scheduler side, as soon as a fiber reaches `TURI_FIBER_DONE` -- see the
Resolution section at the end.

Severity: low-medium (interpreter is process-lifetime, so this is bounded
growth, not an unbounded malloc leak; but RSS/VM grows linearly with the number
of `async` fibers spawned and is only released when the env is torn down --
matters for a long-running interpreter session or a deep/wide async workload,
not for `tur build`)

## Summary

Every `(async (fn ...))` mmaps a fresh `TURI_ASYNC_STACK_SIZE` (512 KB) fiber
stack. When the fiber finishes (`TURI_FIBER_DONE`), its stack is **not**
munmap'd -- it is only reclaimed in the teardown walk at env free. So a program
that spawns N async fibers over its lifetime holds N × 512 KB of mmap
reservations (plus the faulted-in resident pages) until the interpreter exits,
even though all but a bounded set of fibers are long since done.

This is not the same defect as the (now-archived) "deeply recursive async
returns garbage" report -- values are correct. This is purely the fiber-stack
memory footprint growing with the number of performs of `async`, not with the
number *live* at once.

## Minimal repro

```turmeric
(defn a-rec [n : int] : int
  (if (= n 0) 0 (+ 1 (await (async (fn [] : int (a-rec (- n 1))))))))
(defn main [] : int (println (a-rec 8000)) 0)
```

Peak `VmRSS` scales with depth (each fiber's stack stays mapped for the whole
run; only the touched pages are resident, so ~22 KB resident per fiber here):

```
depth  500 -> peak VmRSS ~95 MB
depth 2000 -> peak VmRSS ~128 MB
depth 8000 -> peak VmRSS ~259 MB
```

The full 512 KB *virtual* reservation per fiber also accumulates, so a wide
enough async fan-out can hit `vm.max_map_count` (one mmap region per fiber) or
exhaust address space well before RSS becomes the limit.

Note (post-fix): the `a-rec` shape above nests its awaits, so *every* fiber is
suspended (not done) simultaneously at peak -- its peak is genuinely the
number *live* at once, which early reclaim cannot lower. The reclaim win shows
on the *sequential* shape, where each `async` settles fully before the next is
spawned:

```turmeric
(defn loop-async [n : int acc : int] : int
  (if (= n 0)
    acc
    (loop-async (- n 1) (+ acc (await (async (fn [] : int 1)))))))
(defn main [] : int (println (loop-async 20000 0)) 0)
```

## Root cause

- Stack is mmap'd per fiber in `EX_ASYNC` and tracked for teardown-time
  reclaim: `src/turi/eval.c:7448` (mmap) and `src/turi/eval.c:7493`
  (`turi_env_track_coro_stack`).
- The fiber is marked `TURI_FIBER_DONE` in `async_fiber_thunk`
  (`src/turi/eval.c:287`) but nothing frees `fiber->stack` there or when the
  scheduler observes the DONE fiber.
- The only release site is the teardown walk in `turi_env_free`'s helper
  (`src/turi/fiber.c:881-893`), which munmaps every tracked coro stack at env
  destruction.

So `coro_stacks` is an append-only list for the life of the env; a DONE
fiber's stack is dead but stays mapped.

## Fix directions

- Reclaim a fiber's stack when it reaches `TURI_FIBER_DONE` -- but carefully:
  the scheduler swaps *out of* the fiber's own stack in `async_fiber_thunk`
  (`swapcontext(&fiber->ctx, &env->sched_ctx)`), so the stack is still in use
  at the moment DONE is set. The munmap must happen from the scheduler side
  *after* control has returned to `sched_ctx` (e.g. in the `turi_await_future`
  / `turi_run_event_loop` loop, right after `swapcontext` returns for a fiber
  whose `state == TURI_FIBER_DONE`), never from within the thunk itself.
- Because the stack is registered in `env->coro_stacks` for teardown, freeing
  it early also has to unlink (or tombstone) the matching `TuriCoroStack` node
  so the teardown walk does not double-munmap. Simplest: give the fiber a
  back-pointer to its `TuriCoroStack` node and clear `node->base` (the walk
  already skips `base == NULL`) when reclaiming early.
- Alternatively, pool/free-list the fiber stacks: keep a small LIFO of returned
  512 KB stacks and hand them back out to the next `async`, capping live+cached
  stacks instead of munmapping on every completion (avoids mmap/munmap churn in
  tight async loops). This bounds the footprint to the peak concurrency rather
  than the cumulative spawn count.

---

## Resolution (2026-07-06)

Implemented the first, minimal fix direction: scheduler-side early reclaim on
`TURI_FIBER_DONE`, with a coro-stack-node tombstone so teardown never
double-frees.

**Changes**

- `src/turi/fiber.h` -- added a `struct TuriCoroStack *stack_node` back-pointer
  to `TuriFiber`, and declared `turi_fiber_reclaim_if_done`.
- `src/turi/env.h` / `src/turi/fiber.c` -- `turi_env_track_coro_stack` now
  *returns* the tracking node so the caller can hold a back-pointer (generators
  ignore it).
- `src/turi/fiber.c` -- new `turi_fiber_reclaim_if_done(env, fiber)`: if the
  fiber is `TURI_FIBER_DONE` and still owns a stack, `munmap`/`free` it, set the
  matching `TuriCoroStack` node's `base` to NULL (the teardown walk already
  skips `base == NULL`), and null out `fiber->stack`/`fiber->stack_node`. Called
  right after each of the four scheduler-side `swapcontext(&sched_ctx,
  &fiber->ctx)` returns (`turi_await_future`, `turi_run_event_loop`,
  `native_with_timeout`, `native_async_race`) -- i.e. once control is back on
  the scheduler stack and the finished fiber is no longer executing on its own.
- `src/turi/eval.c` (`EX_ASYNC`) -- store the tracking node into
  `fiber->stack_node` at spawn.

**Why this is safe.** A fiber only ever runs via a scheduler-side swap, and its
thunk always ends by swapping back to `sched_ctx`, so control returns to exactly
one of the four reclaim sites with `state == TURI_FIBER_DONE`. At that point the
fiber's stack is idle. Nulling `fiber->stack` makes a second reclaim a no-op,
and the node tombstone makes the two release paths (early vs. teardown) mutually
exclusive. Generator stacks are untouched (they pass through
`turi_env_track_coro_stack` but keep no back-pointer and are reclaimed only at
teardown, as before).

**Measured effect** (Debug/ASan build, `loop-async` sequential shape):

| spawns | peak RSS before | peak RSS after |
| ---: | ---: | ---: |
| 5000  | 133 MB | 94 MB |
| 20000 | 280 MB | 121 MB |

Per-spawn growth drops from ~10 KB to ~1.8 KB; the residual ~1.8 KB/spawn is the
pre-existing pool-allocated `TuriFiber`/`TuriFuture` structs (a separate,
process-lifetime allocation not in scope here), not the 22 KB/fiber stacks.

**Tests.** `ctest` targets `tur_env_teardown` and `tur_eval_async_*` (basic,
composition, effects, timeout, cancel, error, io) all pass -- a double-free from
the tombstone would surface in the teardown test under LSan. Full
`bash tests/run.sh`: **1951 passed, 0 failed.**

Not pursued: the free-list/pooling alternative. Early reclaim already bounds the
mapped-stack footprint to peak concurrency; a LIFO cache would only trade
munmap/mmap churn for retained mappings and can be layered on later if a tight
async loop's syscall rate ever shows up in a profile.
