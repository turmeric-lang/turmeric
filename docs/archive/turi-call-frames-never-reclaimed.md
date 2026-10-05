# `tur --interpret`: call frames and bindings are never freed (~4 KB per step), and every variable lookup is a `strcmp` walk

> **RESOLVED 2026-10-05, by fix directions 1 and 2.** An activation's call
> frame, and the let / match-arm frames made under it, go back to a free list
> when the activation returns (or a tail call replaces it) with nothing having
> captured them. Frame names are compared by pointer and first byte before
> `strcmp`. Peak RSS of the four fixtures below fell 75-90%. Pinned by
> `tests/check-turi-frame-reclaim.py` (ctest `tur_turi_frame_reclaim`).
> Details in *Resolution* at the end.

**Severity:** medium (interpreter performance; drives the `run-turi.sh`
timeouts). Under the interpreter, memory grows with the number of calls a
program makes, not with what it keeps live. Wall time grows superlinearly
once RSS reaches the hundreds of MB. Four r7rs fixtures peak at 0.7-1 GB and
take 9-14 s under the Debug `tur`, with no contention, against
`run-turi.sh`'s 15 s cap. Found 2026-10-05 by the audit in
[docs/notes/r7rs-performance-audit.md](../notes/r7rs-performance-audit.md).

The cost has been measured before, as "the interpreter retains roughly 4 KiB
per step" (CLAUDE.md;
[ci-cps-tramp-turi-timeouts-under-load](ci-cps-tramp-turi-timeouts-under-load.md)).
That fix shrank the fixtures. This report is about the mechanism.

## Repro

```sh
export ASAN_OPTIONS=detect_leaks=0
# Debug tur: 10.6 s, 938 MB.  Release tur: 2.0 s, 394 MB.
tur --interpret tests/fixtures/r7rs-write-labels/input.tur
```

The fixture builds a 100,000-element list and `write`s it. A program that
only does `(display 1)` runs in 0.08 s and 26 MB (Release).

| fixture | Debug wall / RSS | Release wall / RSS |
| --- | --- | --- |
| `r7rs-srfi-14` | 13.6 s / 956 MB | 2.5 s / 491 MB |
| `r7rs-apply-long` | 10.8 s / 692 MB | 1.3 s / 329 MB |
| `r7rs-write-labels` | 10.6 s / 938 MB | 2.0 s / 394 MB |
| `r7rs-srfi-41` | 9.0 s / 840 MB | 1.5 s / 324 MB |

## Root cause

**Memory.** At the peak (massif, Release `tur`), 98.6% of the heap is arena
slabs. Of the heap:

- 58% is `frame_bind` (src/turi/eval.c:1169). Each binding is allocated from
  `turi_val_alloc` (the env's value pool), with a comment saying "bindings
  hang off a frame a closure may capture".
- 29% is `eval_frame_new` (eval.c:1116).

`eval_frame_free` (eval.c:1158) is deliberately a no-op: "closures may
capture frame pointers and outlive the scope that created them." The comment
also assumes "worker processes are short-lived (one fixture per fork)". That
no longer holds for `tur --interpret`, which runs the whole program in one
process.

**CPU.** `eval_lookup` (eval.c:1189) walks every frame on the chain and
calls `strcmp` on every binding's name before it falls back to the global
hash table (`turi_env_get`, then `ht_find` with another `strcmp`). For the
same fixture (callgrind, Release) that comes to:

- `eval_lookup`: 24.5% of instructions, inclusive.
- `strcmp`: 11.8%. Of that, 8.1 points come from 37.6 M calls in
  `eval_lookup`.
- `ht_find`: 6.2%.

`eval_frame_update` and the tyvar lookup just above it have the same shape.

## Fix directions

1. **Reclaim frames nothing captured.** Mark a frame as captured when a
   closure, a continuation (`call/cc`, the DK machinery) or a `delay` takes
   it as its environment. Mark its ancestors too, since a captured child
   keeps its parent chain alive. When a call returns and its frame is not
   marked, give the frame and its bindings back to a free list. Bindings
   could live inline in the frame (a small array) instead of as a
   pool-allocated linked list. The effect is that memory tracks live data
   instead of step count.
2. **Pointer equality first.** Binding names and variable names come from
   interned `Symbol`s nearly everywhere (`fn_expr->as.var.binding->name->name`).
   Test `b->name == name` before `strcmp` in `eval_lookup`,
   `eval_frame_update` and `ht_find`. Better still, have every frame binding
   use the interned pointer and drop the `strcmp`. The tyvar lookup at
   eval.c:1152 already does the pointer check.
3. **Resolve once.** Record a variable's frame depth and slot at elaboration
   time (de Bruijn-style), or cache the resolved global binding on the
   `Expr`. A global reference then stops walking the frame chain.

## Pinned by

Nothing yet. A check could run a 1e5-step loop under `--interpret` and
assert a peak RSS bound. `tests/run-r7rs-gc.sh` does this for the compiled
side.

## Resolution (2026-10-05)

### The mechanism (src/turi/eval.c)

- **`EvalFrame.escaped`.** `frame_escape` sets it on a frame and every
  ancestor, so an escaped frame's parents are always escaped too. It is called
  everywhere a frame can outlive its activation:
  - every closure capture (`->captured = frame`);
  - a generator's body frame;
  - a fiber effect continuation's body frame;
  - a work-stack effect capture: the handler frame and every frame in the
    captured slice.

  Clones made for a multishot resume, and frames promoted to `value_perm`,
  are born escaped.
- **`eval_frame_new_call`.** Creates the call frame of a driven activation.
  The three sites are the driver's direct call, the driver's folded call, and
  `eval_apply_driven`'s seeded activation. The frame is `reclaimable` and
  comes from `env->frame_free` when that list has one.
- **`eval_frame_new_owned`.** Used for the driver's `let` / `letrec` frames
  and `eval_match_resolve_with`'s arm frames. It links the new frame to its
  nearest call-frame ancestor's `owned` list. A let in tail position never
  gets a completion of its own (the F1 tail leak), so it has to go with the
  activation.
- **`frame_release`.** Runs from `DK_CALL_RET` when the call returns, and from
  the tail-call reuse when it replaces an activation. It hands the frame, its
  owned frames and all their bindings to the env's free lists. `frame_bind`
  takes bindings from that list. Nothing is released when:
  - the frame escaped (and then nothing under it escaped either);
  - a re-entrant continuation exists (`g_turi_cont_pinned`, the existing
    policy, because a stack image may complete the call again);
  - a debugger is attached (its stack holds activation frames);
  - `TUR_TURI_FRAME_RECLAIM=0` is set (for bisecting: the free lists also
    hide a use-after-release from ASan).
- **Free lists.** They live in the env (`frame_free`, `binding_free`) and are
  emptied when scratch promotion resets `value_scratch`, which holds their
  nodes.
- **Name comparison.** `eval_lookup`, `eval_frame_update` and env.c's
  `ht_find` compare names by pointer, then first byte, then `strcmp`.

### Measured (Release `tur --interpret`, serial)

| fixture | before | after |
| --- | --- | --- |
| `r7rs-write-labels` | 2.00 s, 394 MB | 1.30 s, 40 MB |
| `r7rs-srfi-14` | 2.53 s, 491 MB | 1.49 s, 132 MB |
| `r7rs-apply-long` | 1.31 s, 329 MB | 0.88 s, 63 MB |
| `r7rs-srfi-41` | 1.47 s, 324 MB | 1.00 s, 78 MB |

Debug (ASan) `tur --interpret`, as `run-turi.sh` runs it:

| fixture | before | after |
| --- | --- | --- |
| `r7rs-write-labels` | 10.6 s, 938 MB | 8.7 s, 506 MB |
| `r7rs-srfi-14` | 13.6 s, 956 MB | 8.7 s, 519 MB |
| `r7rs-apply-long` | 10.8 s, 692 MB | 5.6 s, 367 MB |
| `r7rs-srfi-41` | 9.0 s, 840 MB | 6.9 s, 540 MB |

Most of what is left in the Debug column is ASan's free quarantine (256 MB by
default), which holds the driver's per-call argument arrays after they are
`free`d. With `ASAN_OPTIONS=quarantine_size_mb=0`, `r7rs-write-labels` peaks
at 111 MB, and at 670 MB with `TUR_TURI_FRAME_RECLAIM=0`.

`bash tests/run-turi.sh`: 2648 passed, 0 failed. `bash tests/run.sh`: 3598
passed, 0 failed.

### What is still kept

Each item has an open report:

- **By-value struct arguments.** Copied into the pool on every call and never
  freed; most are `any` boxes and never-written structs:
  [turi-immutable-struct-args-copied-per-call](../reported/turi-immutable-struct-args-copied-per-call.md).
- **Effect continuations.** Each `perform` keeps its continuation, handler
  frame and captured frames:
  [turi-effect-perform-keeps-its-continuation](../reported/turi-effect-perform-keeps-its-continuation.md).
- **Tyvar and dictionary pins, and frames made off the driver's call path:**
  [turi-call-pins-and-side-frames-not-reclaimed](../reported/turi-call-pins-and-side-frames-not-reclaimed.md).
- **Programs with a re-entrant `call/cc`.** The first capture switches
  reclamation off for the rest of the run:
  [r7rs-callcc-memory-never-freed](../reported/r7rs-callcc-memory-never-freed.md),
  section *2026-10-05*.
- **Frames captured by a closure**, with everything under them, as before.
  That is correct until the interpreter can tell a closure is dead.
