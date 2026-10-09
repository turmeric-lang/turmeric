# `#lang r7rs` compile- and run-time cost audit (2026-10-05)

Why do r7rs fixtures keep needing a bigger `expected.timeout`? 132 of the 146
fixtures with an `expected.timeout` are r7rs fixtures, and almost all of them
sit at 60 s. This note measures where the time and memory go on each back end:
compiled (`tur build`), interpreter (`tur --interpret`) and JIT (`tur jit`). It
ranks the fixes by what they would buy.

**Short answer.** The programs are not slow to run. A compiled r7rs fixture
finishes in a median of 10 ms. The cost is in four places:

1. **Building.** Every build re-elaborates the whole stdlib and r7rs prelude
   (~1 s of Debug `tur`). It recompiles every imported SRFI or on-demand
   library into the program unit, which is never cached (2-5 s). It also pays
   a cold library compile (7-21 s) for each of ~10 prelude variants a suite run
   meets.
2. **The interpreter's memory.** Call frames are never freed, so a
   1e5-element loop takes 0.7-1 GB and 10-14 s under the Debug `tur`.
3. **`(scheme eval)` programs.** These link the 115 MB sanitized `libturi.a`
   and elaborate the stdlib a second time at run time.
4. **The JIT.** It feeds the whole 1.5 MB unit to c2mir on every run.

Two front-end fixes landed with this note. Two other items are filed as
reports, and the rest are directions below.

## Method

The Debug `tur` was used throughout (ASan + UBSan, as CI builds it), unless a
row says Release (`RelWithDebInfo`). The box was a 4-core container with
gcc 13.3. All 133 `#lang r7rs` fixtures were run **serially**, so the
numbers carry no contention. Each run recorded wall time, CPU time and peak
RSS (`wait4` rusage), in two passes: one on an empty prelude cache, then one
warm. Profiles are callgrind (instructions) and massif (heap) on a Release
`tur`. `/proc/<pid>/smaps` was sampled for RSS by mapping. The scripts are not
checked in; each number below gives the command shape needed to reproduce it.

## 1. Compiled path (`tests/run.sh`)

`run.sh` runs only `tur build` for an r7rs fixture (no fixture has an
`expected.c`), then the binary.

### Warm cache, serial, Debug `tur`

| | median | mean | max |
| --- | --- | --- | --- |
| `tur build` wall | 2.40 s | 2.94 s | 11.6 s (`r7rs-srfi-64-read-eval`) |
| `tur build` peak RSS | 197 MB | 203 MB | 254 MB |
| binary run wall | 0.01 s | 0.13 s | 2.6 s (`r7rs-tail-calls-stress`, 4e7 iterations) |
| binary run RSS | 11 MB | 19 MB | 153 MB (eval programs: the embedded interpreter) |

Over the whole corpus that is 391 s of `tur build` and 17 s of running.

**Where one build's time goes:**

| | trivial program | SRFI 13 program | `(scheme eval)` program |
| --- | --- | --- | --- |
| `tur` front end (Debug) | 0.95 s | 1.47 s | 1.21 s |
| cc of the program unit | 0.6 s (514 KB of C) | **4.2 s** (1.1 MB) | **4.6 s** (815 KB, `-fsanitize`) |
| link | 0.3 s | 0.3 s | 1.3 s (`-lturi`, 115 MB archive) |
| warm total, measured | 2.0 s | 6.4 s | 7.7 s |

On a Release `tur` the front end is 0.21 s for `emit-c` and 0.40 s for a split
`build`, because a split build emits C twice (see §5). A plain Turmeric
program's `emit-c` takes 0.026 s, so the r7rs front end costs ~10x that
before it looks at the program at all.

### The slow builds are imported libraries

The 25 slowest warm builds are all programs that import an SRFI or
`(scheme eval)`. Their extra cost is cc time on the **program** unit. An
imported library is not part of the cached library unit. Its definitions are
emitted into the program unit and compiled at `-O2` on every build. Compared
with a trivial program, the client unit of `r7rs-srfi-13` defines 376 more
functions (91 `srfi13_*` and 15 `srfi14_*`, plus their lambdas and drop
glue). `r7rs-eval` defines 206 more (143 `r7rs_*` from `r7rs/eval.tur` and
`read.tur`). Filed as
[r7rs-imported-libraries-recompiled-every-build](../reported/r7rs-imported-libraries-recompiled-every-build.md).

### Cold cache: the timeouts

| | count | wall | CPU |
| --- | --- | --- | --- |
| builds that compiled a new library object | 10 of 133 | 7.0-21.7 s | 16.5-47.9 s (in parallel pieces) |
| objects created by one serial pass | 10 (20 MB) | | |

This is
[r7rs-prelude-library-object-varies-with-the-program](../reported/r7rs-prelude-library-object-varies-with-the-program.md),
reproduced. It is the likeliest cause of the 10 s build timeouts. A cold
variant costs 17-48 CPU-seconds on a box whose other cores are running
fixtures. Every other fixture of the same variant then **waits on that
object's lock, with its own timer running** (`prelude_split_object`, up to
120 s). `run.sh` warms one variant first, but it warmed the wrong one: the
`(display 1)` program does not get the object most fixtures link. A program
that passes a `lambda` to `map` does. This change switches `run.sh`'s warm-up
to that program (the report's interim fix 3).

Which object each of the 133 fixtures links, with the cache warm
(`TUR_SHOW_CC=1 tur build`):

| prelude object | fixtures |
| --- | --- |
| the one the new warm-up (`map` over a `lambda`) builds | **105** |
| the one the old warm-up (`(display 1)`) builds | 10 |
| `(scheme eval)` variants (two) | 8 |
| `(scheme file)` / `(scheme process-context)` | 2 |
| `r7rs-threads-*`: one object per fixture except two that share | 6 |
| no split (`r7rs-reader-forms`, `region-escape-via-callcc`) | 2 |

So the warm-up used to cover 10 fixtures and now covers 105. The other 28
share 9 objects, and the first fixture to reach each one still compiles it
cold inside its timed build. That includes the old warm-up's object, which
nothing warms now. The real fix is recommendation 1 below.

## 2. Interpreter (`tests/run-turi.sh`, 15 s default)

There were 109 r7rs fixtures under `tur --interpret`, run serially on the
Debug `tur`:

| | median | mean | max |
| --- | --- | --- | --- |
| wall | 0.70 s | 1.44 s | 13.6 s |
| peak RSS | 143 MB | | 956 MB |

| fixture | wall (Debug) | RSS (Debug) | wall / RSS (Release) |
| --- | --- | --- | --- |
| `r7rs-srfi-14` | 13.6 s | 956 MB | 2.5 s / 491 MB |
| `r7rs-apply-long` | 10.8 s | 692 MB | 1.3 s / 329 MB |
| `r7rs-write-labels` | 10.6 s | 938 MB | 2.0 s / 394 MB |
| `r7rs-srfi-41` | 9.0 s | 840 MB | 1.5 s / 324 MB |
| `r7rs-apply-standard-variadic-many` | 6.7 s | 693 MB | |

These run close to the 15 s cap with no contention, and `run-turi.sh` runs
`nproc` of them at once.

**Memory.** At the peak of `r7rs-write-labels` (massif, Release), 87% of the
388 MB heap is call frames (`eval_frame_new`, eval.c:1116, 29%) and bindings
(`frame_bind`, eval.c:1169, 58%). Neither is ever freed: `eval_frame_free` is
a no-op because a closure may have captured the frame. The fixture's work is
one 100,000-element list, built and then written, at ~4 KB per step. The
Debug figure is 2.5x the Release one. Filed as
[turi-call-frames-never-reclaimed](../archive/turi-call-frames-never-reclaimed.md),
since resolved: see *Follow-up* below.

**CPU.** 24.5% of the instructions (inclusive) are in `eval_lookup`. It finds
every variable by walking the frame chain and calling `strcmp` on each
binding's name, so a global reference passes through every enclosing frame
before it reaches the hash table. `strcmp` alone is 11.8%, mostly from
`eval_lookup`'s 37.6 M calls. Binding names are interned symbol strings
nearly everywhere. A pointer-equality test before the `strcmp`, or resolving
a variable's frame depth and slot at elaboration time, would remove most of
this. The same report covers it.

**Follow-up (2026-10-05): fixed.** Call frames, the let / match-arm frames
under them, and their bindings now go back to a free list when nothing
captured them. Lookups compare names by pointer and first byte before
`strcmp`. Release `--interpret`: `r7rs-write-labels` 394 to 40 MB,
`r7rs-srfi-14` 491 to 132 MB, `r7rs-apply-long` 329 to 63 MB, `r7rs-srfi-41`
324 to 78 MB, each 30-40% faster. What is still kept (by-value struct
argument copies, programs with a re-entrant `call/cc`) is listed in the
archived report.

## 3. JIT (`tests/run-jit.sh`)

| fixture | Debug `tur jit` | Release `tur jit` |
| --- | --- | --- |
| `r7rs-closure-one-capture-call` | 2.1 s, 436 MB | 0.79 s, 120 MB |
| `r7rs-srfi-13` | 6.5 s, 775 MB | 2.2 s, 237 MB |
| `r7rs-tail-calls-stress` | 34.8 s, 801 MB | 11.4 s (native: 2.6 s) |

The JIT does not use the prelude split. It hands c2mir the single 1.5 MB
unit of 1,725 functions on every run, where cc compiles only 514 KB. MIR's
code generation is already lazy (`jit_set_lazy_gen_interface`), so the fixed
cost is c2mir parsing and lowering all of it. There are two directions:

- Drop the functions nothing reaches before c2mir. cc does the same for the
  one-unit build: this is the "Phase A" reasoning in
  `docs/archive/r7rs-programs-compile-slowly.md`, and `srfi_prune_program`
  already does it for SRFIs.
- Cache the library's MIR the way the split caches its object. MIR has a
  binary format (`MIR_write`/`MIR_read`).

The 4x run-time gap on the tail-call stress is MIR's code quality, not
start-up.

## 4. Front end: what is in the ~1 s (Release profile, `emit-c` of a trivial program)

Callgrind, 696 M instructions before the fixes below:

| share (self or inclusive) | what | status |
| --- | --- | --- |
| 13.1% self (33% incl.) | `tl_has_definstance_at_or_after`: re-scanned every later form (to depth 4) for each `defn`. Quadratic in the unit, and an r7rs unit is the whole stdlib | **fixed here** |
| 7.8% self | `scope_lookup_type_def`: a newest-first linear scan of the global scope (~10k bindings) for every call head, from `elab_call_inner`'s `:no-auto-ctor` check | **fixed here** |
| ~40% incl. | `emit_program`. A split build runs it twice (library unit, then program unit) | §5 |
| 9.5% incl. | `refine_resolve_call_sites`: refinement path conditions (`rt_collect_path_conds` / `rt_rebinds_mentioned` re-walk subtrees per hypothesis) | open |
| ~9% incl. | reader | open |
| ~3% self | `strcmp` in `elab_lookup_ctor` and `rt_head_is` (a name compared as a string on interned symbols) | open, cheap |

**The two fixes** (src/compiler/elab_toplevel.c, elab_core.c, elab_structs.c):

- **`inst_from[]`.** The "is there a `definstance` at or after this form?"
  answer is now computed once per unit, as a suffix array, before Pass 2. It
  uses the same depth-bounded walk per form.
- **`Scope.prev`.** The global scope already keeps a name index pointing at
  each name's newest binding. It now also keeps `prev[i]`, the next-older
  binding of the same name. `scope_lookup_type_def` walks that chain (a
  handful of bindings) instead of the whole scope, and returns the same
  binding the scan returned.

Both are exact rewrites. `emit-c` output was byte-identical, and so were
exit codes and stderr, on all ~2,760 happy-path and 714 `errors/` fixture
inputs, old binary against new. `bash tests/run.sh`: 3598 passed, 0 failed.

| `emit-c` of a trivial r7rs program | before | after |
| --- | --- | --- |
| Release | 0.21-0.30 s | 0.16 s |
| Debug (ASan) | 0.95 s | 0.80 s |
| Debug, SRFI 13 program | 1.47 s | 1.29 s |
| Debug, `(scheme eval)` program | 1.21 s | 1.03 s |

ASan's own overhead (≈4x, mostly malloc and shadow checks) dominates the
Debug numbers, so algorithmic fixes show less there.

## 5. Ranked recommendations

| # | change | buys | effort |
| --- | --- | --- | --- |
| 1 | Make the library unit program-independent (the fix 1 in [r7rs-prelude-library-object-varies-with-the-program](../reported/r7rs-prelude-library-object-varies-with-the-program.md)) | one cold compile per suite instead of ~10 (17-48 CPU-s each), and no fixture waiting on another's lock inside its timer. Most of the 60 s budgets exist for this | medium |
| 2 | Put imported libraries (SRFIs, `r7rs/eval`, `read`, `file`, ...) in a cached unit: per library, or as a variant of the library unit keyed by the import set | 2-4 s off every SRFI and eval build (cc of 200-375 functions) | medium |
| 3 | ~~Reclaim interpreter frames that nothing captured, and give `eval_lookup` a pointer-compare fast path~~ **done** | memory flat instead of ~4 KB/step (0.7-1 GB fixtures drop to tens of MB); ~10-20% interpreter CPU | medium / small |
| 4 | Once (1) holds, key the prelude cache on its *inputs* (tur version, stdlib content hash, autoload set, flags) and skip emitting the library unit on a hit | a split build's front end drops from ~2x `emit-c` to ~1x (0.40 s to ~0.2 s Release) | small, after 1 |
| 5 | JIT: prune unreachable functions before c2mir, or cache the library's MIR | 2.1 s to well under 1 s for a trivial program under Debug `tur jit` | medium |
| 6 | Remaining front-end hot spots: refinement path-condition walks, string compares on interned names, the reader | another ~10-15% of the front end | small each |
| 7 | `(scheme eval)` client: compile with `-fsanitize=address` only (not `undefined`) | 4.6 s to 3.8 s for that compile. Dropping ASan from the client entirely is **not** safe: linked against the sanitized `libturi.a`, `r7rs-eval` then fails with an ASan stack-buffer-overflow report in `ht_find` (stack shadow left by the deep-stack switch) | trivial |

Done here: the two front-end fixes in §4, and the `run.sh` warm-up change
(the interim fix 3 from the variant report). Recommendation 3 followed.

## 6. Compiled code under the collector (`tur_r7rs_gc`), 2026-10-05

The `tur_r7rs_gc` ctest target (`tests/run-r7rs-gc.sh`) hit its 720 s cap.
It builds every compiled r7rs and Saffron fixture (232) and runs each with
`TUR_GC_TORTURE=31`, a full collection every 31 allocations. Measured
serially:
- **Phase 1:** 923 s of torture runs against 13 s of plain runs, plus 281 s
  of builds.
- **Longest case:** `r7rs-tail-call-hand-on-through-static-call` at 292 s
  (0.17 s plain).

**Where a collection's time went** (callgrind, `r7rs-apply-long`; a byte
count per root source in an instrumented build):
- Marking the live heap was ~6 MB a collection. Data/bss was 89 KB, stacks
  7 KB.
- About 64 instructions per scanned word.
- Each candidate pointer cost a hash lookup of its 64 KiB chunk and a 64-bit
  divide (`(w - base) / size`) for its slot index.
- The drain looked each object's page up a second time.
- The sweep visited every slot one by one (8.7%).

**Fixed in src/runtime/r7gc.c (exact rewrites):**
- **Slot index:** `(off * inv) >> 48` with `inv = 2^48 / size + 1`, exact
  for every class at every chunk offset (`tests/check-r7gc-slot-of.sh`,
  ctest `tur_r7gc_slot_of`).
- **Mark stack:** holds (start, size) pairs, so there is one page lookup per
  object.
- **Mark phase:** remembers the last chunk's page, reset each collection.
- **Sweep:** works a bitmap word at a time with popcount, pushing free slots
  in the same order as before.

Instructions for the same 402 collections: 19.4 G to 8.4 G. Phase 1 torture
time: 923 s to 364 s (2.5x); the longest case 292 s to 99 s.
`tests/run-r7rs-gc.sh`: 249 passed, 0 failed, in 5 m 49 s (it had hit the
720 s cap).

**Still open:**
- **The torture interval is quadratic in live size:**
  [r7rs-gc-torture-quadratic-in-live-heap](../archive/r7rs-gc-torture-quadratic-in-live-heap.md).
  An opt-in `TUR_GC_TORTURE_SCALE=R` takes the worst case to 0.8 s.  Since
  2026-10-08 the harness runs with it at 64 (every-allocation cases excepted):
  3 m 33 s.
- **Eval programs scan `libturi`'s data:**
  [r7rs-gc-eval-programs-scan-libturi-data](../archive/r7rs-gc-eval-programs-scan-libturi-data.md).
  Every collection reads 46 MB, 38 ms against ~8 ms.
- **Builds:** most of the rest of the harness's time is building 232
  programs, with ~8 cold prelude variants for the thread fixtures. See §1
  and §5.

## Not a problem

- **Run time of compiled programs.** It is a median of 10 ms. The 4e7-call
  stress takes 2.6 s (65 ns per generic-arithmetic iteration). Thread
  fixtures spend their time in thread start-up and joins.
- **Peak RSS of `tur build`.** 175-254 MB under ASan, flat across fixtures.
  It is not why anything times out.
