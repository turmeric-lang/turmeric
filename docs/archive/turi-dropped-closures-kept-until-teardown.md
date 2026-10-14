# `tur --interpret`: a dropped closure and the frame it captured stay until the env is freed

**Resolved 2026-10-14 by fix direction 1** (see Resolution at the end): a
`#lang r7rs` env now runs a pool sweep that hands every frame, binding,
closure and small struct nothing reaches back to a free list. The repro's
`guard` loop peaks at 31.5 MB for 80,000 iterations, the same as for 20,000.

**Severity:** low-medium (interpreter memory). A Scheme `guard` in a loop grows
the interpreter by about 1.7 KB an iteration, with no `call/cc` anywhere: 80,000
iterations peak at 163 MB on a Release `tur`. The same program compiled peaks
at 10 MB. Any loop that makes a closure capturing its frame grows the same
way, by about 0.28 KB a closure. `guard` makes several. Found 2026-10-14 while
measuring r7rs-callcc-memory-never-freed: a `guard` loop was the control, and
it grew with no continuation at all.

This is the interpreter's documented policy (docs/guides/gc-guide.md, "The
interpreter is different": closures are never freed individually; the pool is
reclaimed at `turi_env_free`). What is new here is the size of it for ordinary
Scheme: a long-running program that uses `guard` -- or `parameterize`,
`dynamic-wind`, `delay`, or a `lambda` passed to a procedure -- in its main loop
grows without bound under `--interpret`.

## Repro

```scheme
#lang r7rs
(import (scheme base) (scheme write))
(define (loop i acc)
  (if (= i 80000) acc
      (loop (+ i 1) (+ acc (guard (e (#t 1)) 1)))))
(write (loop 0 0))
(newline)
```

Peak RSS, Release `tur`, measured 2026-10-14:

| program | 20,000 iterations | 80,000 iterations | per iteration |
| --- | --- | --- | --- |
| `(guard (e (#t 1)) 1)` | 61 MB | 163 MB | 1.7 KB |
| `(apply1 (lambda (x) (+ x i)))`, with `(define (apply1 f) (f 1))` | 32 MB | 49 MB | 0.28 KB |
| the `guard` program, compiled (`tur build`) | -- | 10 MB | -- |

## Root cause

- **The closure** is bump-allocated from the env's value pool
  (`EX_FN` / `EX_CLOSURE`, src/turi/eval.c:12543 and :12553), which has no
  per-object free.
- **The frame it captures** is marked escaped (`frame_escape`, eval.c:1376,
  called at the same two sites). `frame_release` never hands an escaped
  frame back to the free list, because a closure may outlive the activation
  and the interpreter does not know when it dies. So the activation's frame,
  its bindings and every frame above it that escaped with it stay too.
- **`guard`** lowers (src/compiler/scheme_lower.c:2934, `lower_guard`) to a
  receiver `(fn [k] ...)`, a handler `(fn [var] ...)`, a body thunk and a
  value thunk, around `r7rs-call/ec__`, which makes the escape procedure
  `(fn [& vs] ...)`, and `r7rs-with-exception-handler`, which makes wind
  frames and their thunks. Each of those captures a frame.

The compiled back end frees all of this (the r7rs-gc collector graduated
2026-09-25), so only the interpreter grows.

## Fix directions

1. **Reuse the continuation sweep's marker as a general collector for an r7rs
   env.** Since r7rs-callcc-memory-never-freed (2026-10-14) an env running
   `#lang r7rs` keeps an object map of its value pool and has a conservative
   marker that walks every root the interpreter has (`turi_cont_gc_*`, eval.c;
   `turi_val_*` mark API, value.c). A sweep triggered by pool growth instead of
   by captures could hand every unmarked escaped frame and binding back to the
   free lists, and closures could get a free list of their own (they are
   fixed-size). The marker's doubts (a reachable Set or Map, effect and
   generator frames) and gates (a program importing Turmeric modules, fibers)
   would apply unchanged. The pool itself stays a bump arena: only frames,
   bindings and closures go back, which is most of what grows here.
2. **Release a closure's frame when the closure provably dies with it.** The
   elaborator knows when a `lambda` is only ever called and never stored (the
   `guard` receiver and handler are passed to known prelude procedures). An
   escape analysis like `callcc_escape_only` (scheme_lower.c) could mark such
   a closure "no escape", and the interpreter could then skip `frame_escape`
   for it. This is narrower than (1) but has no runtime cost.
3. **Scratch promotion mid-run.** The REPL's promotion rewinds the scratch
   pool at each top-level boundary (turi-interp-incremental-reclamation-plan).
   A program inside one long top-level form never reaches a boundary, so this
   does not help without a way to promote from inside an evaluation.

## Resolution (2026-10-14)

Fix direction 1, extended to structs. A first measurement split the
1,768 bytes a `guard` iteration kept: 10 frames (560 B), 11 bindings
(352 B), 8 closures (640 B) and 3 small structs with their field arrays
(216 B). Direction 2 could have saved some frames but none of the
closures, and direction 3 needs a top-level boundary the loop never
reaches. So the sweep had to free all four kinds.

**The object map knows each object's kind.** Before, one bit said "frame or
binding" and the object's extent told them apart. That was a guess the
continuation sweep could afford, because a wrong one only kept something
alive; a sweep that hands objects back cannot afford it. Three bit arrays now
give each start's kind exactly: `TURI_VAL_FRAME`, `_BINDING`, `_CLOSURE` or
`_STRUCT` (src/turi/value.c, `turi_val_note_kind`).
`turi_val_each_unmarked` visits every noted object a marking pass did not
reach.

**What is reclaimed, and where it goes** (src/turi/eval.c):

- **Closures** for `fn` values (`EX_FN`, `EX_CLOSURE`, a lifted lambda
  re-homed on its frame) come from `closure_alloc`, which uses a free list.
  Natives and defns, made once, stay plain.
- **Structs** of up to 8 fields are now one block, header and field array
  together (`struct_alloc`), with a free list per field count. Larger structs
  stay plain.
- **Frames and bindings** the sweep hands back go on lists of their own
  (`frame_swept`, `binding_swept`). Ones an activation hands back as it
  returns stay on the old lists, and those are taken first.
- **Epochs.** A closure and a struct carry the capture epoch they were made
  in, as a frame does. The continuation sweep's image filter
  (`gc_word`) can then tell that a stale word in an image names one made
  since, and skip it.
- **Order.** `turi_cont_gc_reclaim` runs after `turi_cont_gc_release`. It
  marks everything already on a free list, without reading it, then zeroes
  each unmarked object and pushes it onto its list. Under ASan, everything
  but the link word is poisoned.

**When it runs.** `TuriEnv.pool_since` counts the bytes of values made: fresh
from the pool, or taken from a list a sweep filled. Recycling an
activation's frames does not count, so a call loop that makes no garbage
never triggers a sweep. Once the count reaches the budget,
`pool_sweep_due` is raised, and the driver sweeps at the top of its next
step, where every value it holds is on its work stack or in a local the
stack scan reads. A native-resume application's detached argument array is
registered for the marker (`DriveReg.loose`).

The budget (src/turi/interpreter_natives.c, `r7k_pool_rebudget`) is the
largest of 4 MB, twice what the last sweep read, and half of what sits on
the free lists, since the sweep walks those too. A sweep that declines
retries after 256 KB, doubling each time it declines again.

The sweep shares the continuation sweep's marker, roots and gates
(`r7k_maybe_sweep`), with these additions:

- **Roots.** `catch_panic_value`, and the loose arguments above.
- **Gates.**
  - Timers or pending I/O.
  - Any mutable map: the map keeps its values in its own memory. The R7RS
    libraries make none.
  - `env->no_pool_sweep`, set by the `(scheme eval)` bridge.

The `eval` gate fixes a hole the continuation sweep already had. The bridge
keeps the program's procedures and pending arguments in its own arrays,
which no sweep reads. The stress runs below found it: a struct the program
had passed in was read after it had been reclaimed. Now the embedded env
and, from the first `eval`, the program's env do not sweep.

### Measured

Peak RSS on a Release `tur` (each figure checked over three runs):

| program | before | after |
| --- | --- | --- |
| `guard` loop, 20,000 iterations | 61 MB | 31.6 MB |
| `guard` loop, 80,000 iterations | 163.6 MB | 31.5 MB |
| `lambda` loop, 80,000 iterations | 49.4 MB | 31.5 MB |
| `(apply + (map (lambda ...) l))`, 30,000 iterations | 126.6 MB | 31.6 MB |
| 300,000 live pairs, then 60,000 `guard`s | 150.7 MB | 94.8 MB |
| 300,000 pairs dropped, then 60,000 `guard`s | 150.7 MB | 70.7 MB |
| recursive `fib 24` | 27.2 MB | 27.2 MB |

Time:

- `tur` executes 3.3% more instructions for 10,000 `guard` iterations
  (973 M to 1005 M). Walking the unmarked objects is 1.3% of that, handing
  them back 1.2%, and noting kinds 0.6%.
- The other loops run within run-to-run noise.
- The two programs with 300,000 pairs pay for marking them while the list
  is built. The one that keeps its list is within noise. The one that drops
  it went from 1.3 s to 1.6 s.

### Checked

- **`tests/check-turi-pool-sweep.py`** (ctest `tur_turi_pool_sweep`)
  covers two things:
  - The new fixture `r7rs-pool-sweep-places` keeps a closure in every kind
    of place a program can, while making garbage. Run with a sweep every
    64 KB and `TUR_TURI_POOL_GC_QUARANTINE=1`, which poisons what a sweep
    would hand back instead of reusing it, it ran about 1,100 sweeps, all
    completed, which handed back about 70 MB, with no ASan report.
  - Three loops grow by at most 1 MB from N to 4N iterations, against
    17-69 MB with `TUR_TURI_POOL_GC=0`.
- **A negative control.** Dropping one root (the dynamic-environment slots)
  makes that fixture fail at once with a use-after-poison.
- **Quarantine runs over the Scheme fixtures.** Every r7rs, SICP and SRFI
  interpreter fixture passed (139/139) under quarantine with a sweep every
  1 MB. With a sweep every 256 KB the same held, except four fixtures that
  only ran past the 60 s limit; each passed alone under quarantine with a
  sweep every 64 KB.
- **The same fixtures with reuse.** With a sweep every 256 KB and the
  handed-back objects reused (poisoned only while on a free list), all
  140/140 pass, the new fixture included.
- **The R7RS conformance suite and both SRFI suites** pass.
- **Full suites.** `run-turi.sh` 2770/0, `tests/run.sh` 3797/0. The
  `tur_turi_callcc_reclaim`, `tur_turi_frame_reclaim`, r7rs import,
  eval-link, REPL, LSP and stack ctests pass. In the call/cc check, storing a
  closure per iteration now grows +5 MB and storing a continuation +2 MB,
  against +124 MB with the sweeps off.
