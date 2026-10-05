# `tur --interpret`: call frames and bindings are never freed (~4 KB per step), and every variable lookup is a `strcmp` walk

**Severity:** medium (interpreter performance; drives the `run-turi.sh`
timeouts). Under the interpreter, memory grows with the number of calls a
program makes, not with what it keeps live. Wall time grows superlinearly
once RSS reaches the hundreds of MB. Four r7rs fixtures peak at 0.7-1 GB and
take 9-14 s under the Debug `tur`, with no contention, against
`run-turi.sh`'s 15 s cap. Found 2026-10-05 by the audit in
[docs/notes/r7rs-performance-audit.md](../notes/r7rs-performance-audit.md).

The cost has been measured before, as "the interpreter retains roughly 4 KiB
per step" (CLAUDE.md;
[ci-cps-tramp-turi-timeouts-under-load](../archive/ci-cps-tramp-turi-timeouts-under-load.md)).
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
