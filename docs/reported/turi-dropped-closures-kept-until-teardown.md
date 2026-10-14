# `tur --interpret`: a dropped closure and the frame it captured stay until the env is freed

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
