---
title: Deeply recursive async/await returns a garbage (pointer) value
category: Reported
description: A recursion threaded through async/await was reported to return a raw heap pointer past depth ~100-500. RESOLVED as not reproducible at HEAD (Phase C1) -- the committed fiber/scheduler path settles futures correctly at every tested depth; the symptom appears to have been an artifact of the reporter's uncommitted C2/C3 working tree.
---

# Deeply recursive `async`/`await` returns a garbage (pointer) value

**Status:** RESOLVED -- not reproducible at HEAD (Phase C1), no code change.
See the Resolution section at the end.

Severity: medium (wrong result, silent -- no crash, no error)

## Summary

A recursion that threads through `async`/`await` returns the correct value at
small depth but a garbage value (a raw heap pointer, e.g. `91328185248752` =
`0x531...`) once the depth exceeds roughly 100-500.  No guard fires, no
SIGSEGV -- `await` just yields a non-int value.

## Minimal repro (interpreter)

```turmeric
(defn a-rec [n : int] : int
  (if (= n 0) 0 (+ 1 (await (async (fn [] : int (a-rec (- n 1))))))))
(defn main [] : int (println (a-rec 500)) 0)
```

```
$ ASAN_OPTIONS=detect_leaks=0 ./build/tur --interpret repro.tur
91328185248752      # expected 500
```

Measured: `a-rec 3/5/10/100` -> correct (3/5/10/100); `a-rec 500/1000/2000/5000`
-> the same garbage pointer.  The threshold sits between 100 and 500.

## Notes

- Each `(async (fn ...))` spawns a fiber with its own `mmap`'d stack
  (`TURI_ASYNC_STACK_SIZE`) and `await` suspends to the scheduler, so a deep
  chain leaves ~N fibers suspended simultaneously, each awaiting the next.  The
  garbage-pointer result suggests the future's value slot is read before/after
  it is settled (or a fiber-stack / scheduler-depth corruption), not a C-stack
  overflow (it does not crash).
- This is **not** a heap-bounding trip: it does not hit the `eval_depth` guard,
  so it does not block retiring the guard (Phase C4).  It is a distinct
  correctness defect in the fiber scheduler / future value propagation.
- Pre-existing: `EX_ASYNC` / `EX_AWAIT` and the scheduler are untouched by
  turi-c-scoped-forms-heap-bounding C1-C3 (which only changed `catch-unwind`,
  `atomically`, `ws_capturable`, and the `resume` value-arg path).  Found while
  measuring C3.

## Fix directions

Instrument the future settle/read path (`turi_future_resolve` /
`turi_future_val` / the `EX_AWAIT` resumed branch) at depth ~200 to find where a
non-int value enters `await`'s return.  Check the fiber stack size and whether
deep suspended-fiber chains corrupt `f->result` or read it while
`TURI_FUTURE_PENDING`.

## Resolution

**Could not reproduce at HEAD (Phase C1, commit `83263dc`).** The committed
fiber/scheduler + future path returns the correct value at every tested depth;
no code change was warranted.

Investigation:

- The exact report repro was run verbatim with the same Debug/ASan build and
  `ASAN_OPTIONS=detect_leaks=0`.  `a-rec` returns the correct integer at depths
  **3, 5, 10, 100, 500, 1000, 2000, 5000, 10000, 20000, 50000, 60000, 70000** --
  no garbage pointer, no crash, at or beyond the reported 100-500 threshold and
  past the `vm.max_map_count` region count.
- The exact depth-500 repro was run **30 times** (ASLR re-randomizing the mmap
  layout each run): 0/30 produced anything other than `500`, ruling out a
  nondeterministic fiber-stack/scheduler corruption on this code.
- Non-int results (the report's "pointer value" symptom) were exercised too: a
  `:float` recursion threaded through async at depth 800 returns the correct
  `1200.0`, so `f->result` is not being misread as a raw pointer for
  heap/boxed results either.
- The settle path is already correct: `async_fiber_thunk` (`src/turi/eval.c`)
  resolves the fiber's future with `env->return_value` when the body exits via a
  `return`/TCO (not a stale `result` register), and Phase A's value-pool
  scratch/permanent split with escape promotion (#609, already in the tree when
  this report was filed) keeps a settled future's value live across fiber
  switches.

Most likely explanation: the report was authored while *measuring C3*, but only
**C1** landed on this branch (`HEAD`); C2/C3 were still in the reporter's
uncommitted working tree.  The garbage-value symptom appears to have been an
artifact of that WIP state (a work-stack change transiently sharing state with
the fiber path), not of the committed fiber/scheduler code, which is correct.

If the symptom reappears once C2/C3 land, it should be re-filed as a fresh
finding against that code, using the depth-sweep + repeat-run reproduction above
as the confirmation harness.
