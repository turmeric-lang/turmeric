# The scheduler drops a finished fiber from its queue and never frees it

**Severity: low-medium (a leak per spawned fiber; a long-lived service grows
without bound).** Found 2026-11-07 while making the Cloudflare guide's
`service.tur` run
([cloudflare-guide-service-captures-into-fiber-new](../archive/cloudflare-guide-service-captures-into-fiber-new.md)).

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
