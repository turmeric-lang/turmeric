# The scheduler drops a finished fiber from its queue and never frees it

**RESOLVED 2026-11-07, the same day it was filed: fix direction 1.**
`scheduler-spawn-detached` hands a fiber to the scheduler, which frees it
right after the resume in which it finished. See "Resolution" at the end.

**Severity: low-medium (a leak per spawned fiber; a long-lived service grows
without bound).** Found 2026-11-07 while making the Cloudflare guide's
`service.tur` run
([cloudflare-guide-service-captures-into-fiber-new](cloudflare-guide-service-captures-into-fiber-new.md)).

A fiber handed to `scheduler-spawn` is resumed by `tur_scheduler_run` /
`tur_scheduler_run_to_completion` (`src/compiler/emit_module.c`, the scheduler
preamble) until it is done. When it is, the loop just does not re-enqueue it:

```c
tur_fiber_block_resume(f, 0);
s->current_fiber = NULL;
if (!f->done && !f->parked) tur_scheduler_enqueue(s, f);
```

Nothing calls `tur_fiber_block_free` on it. The program still holds the
`FiberHandle` it passed in, so it can `fiber-free` the fiber itself -- but only
by keeping every handle and polling `fiber-done?`, which no example does. A
server that spawns a fiber per connection (the guide's service) keeps every
FiberBlock, its fiber-local list and its stack (`tur_fiber_block_new`'s
default is 1 MiB, malloc'd) for the life of the process.

## Repro

```turmeric
(load "stdlib/scheduler.tur")
(def ^mut left 1000)
(defn body [] : nil                     ; each fiber spawns the next
  (when (> left 0)
    (set! left (- left 1))
    (scheduler-spawn (scheduler-current) (fiber-new body 0))))
(defn vm-kb [] : int                    ; VmSize, from /proc/self/status
  ```c
  FILE *f = fopen("/proc/self/status", "r"); char line[256]; int64_t kb = -1;
  while (f && fgets(line, sizeof line, f)) if (!strncmp(line, "VmSize:", 7)) kb = atoll(line + 7);
  if (f) fclose(f);
  return kb;
  ```)
(defn main [] : int
  (let [s  (scheduler-new)
        v0 (vm-kb)]
    (scheduler-spawn s (fiber-new body 0))
    (scheduler-run-to-completion s)
    (println (/ (- (vm-kb) v0) 1024)))  ; MiB the run left behind
  0)
```

Linux x86-64, 2026-11-07: `1007` -- 1,001 finished fibers, about 1 MiB each.
With `left` at 10 it prints `11`.

LeakSanitizer does not show it. A finished fiber's stack still holds the
pointer to the fiber it spawned, and the scheduler's ring buffer keeps the
slots it dequeued, so the conservative scan finds a chain from a live root
and reports one stack at most.

## Fix directions

1. **A detached spawn.** `scheduler-spawn-detached` (or a flag on the
   FiberBlock set by a `scheduler-spawn` variant) hands ownership to the
   scheduler, and both run loops free a detached fiber once `done` -- after
   the resume returns, when nothing runs on its stack. `scheduler-spawn` keeps
   today's meaning, since a caller holding the handle may still read
   `fiber-done?` or `fiber-arg`. The FiberBlock gains one field, which moves
   the scheduler preamble, so the fixture snapshots that carry it regenerate
   in the same change.
2. **Until then, the program owns it**: keep the handles and `fiber-free` the
   done ones (the guide's "Fiber-body note" says so).

## Resolution (2026-11-07)

- **The FiberBlock gains `sched_detached`** (last field;
  `src/compiler/emit_module.c`). The three single-threaded scheduler loops
  (`tur_scheduler_run`, `tur_scheduler_run_to_completion`,
  `tur_scheduler_run_one`) hand a resumed fiber to one helper,
  `tur_scheduler_requeue`: runnable goes back on the queue, parked waits for
  its unpark, done is dropped -- and `tur_fiber_block_free`d when detached.
  The free runs on the scheduler's stack after the resume returned, so
  nothing is running on the fiber's.
- **`scheduler-spawn-detached [sched f : FiberHandle]`** (`stdlib/scheduler.tur`)
  sets the flag and spawns. `scheduler-spawn` keeps its meaning: the caller
  still owns that fiber and may ask `fiber-done?` of it.
- The Cloudflare guide's service spawns detached, and its "Fiber-body note"
  says why.
- Not changed: the multi-threaded scheduler (`stdlib/scheduler_mt.tur`) and
  the work-stealing one (`src/async/scheduler.c`) have their own loops and
  still never free a finished fiber; nothing there spawns per connection
  yet. A TaskGroup's fibers are the group's and are not to be detached.
- The repro above prints `1` with `scheduler-spawn-detached` in place of
  `scheduler-spawn` (it printed `1007`). Pinned by
  `tests/fixtures/scheduler-spawn-detached-frees-done`: a 300-fiber chain,
  each fiber spawning the next with its depth in a fiber-local, must leave
  VmSize within 64 MiB of where it started (with plain `scheduler-spawn` it
  prints `grew`), and a plain-spawned fiber's handle still answers
  `fiber-done?` after the run. Off Linux, or under an ASan runtime (whose
  quarantine keeps freed stacks mapped), the measurement reads -1 and that
  line passes vacuously.
- The fixed preamble moved, so `src/runtime/generated/` and the 162
  `expected.c` snapshots were regenerated in the same change.
