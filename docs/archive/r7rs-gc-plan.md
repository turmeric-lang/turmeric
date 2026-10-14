# A collector for compiled `#lang r7rs` programs (`--enable=r7rs-gc`)

Status: **graduated 2026-09-25** -- the collector is the allocator of every
compiled single-unit `#lang r7rs` program on Linux and macOS (`g_opt_r7rs_gc`
defaults true; `--enable=r7rs-gc` is a no-op on the GRADUATED list).
`TUR_R7RS_GC=0`, or `--no-r7rs-gc` on the build, builds a program without
it, which is what a program that starts threads does. Answered
[r7rs-heap-data-never-reclaimed](r7rs-heap-data-never-reclaimed.md)
for the compiled back end, the compiled half of
[r7rs-callcc-memory-never-freed](r7rs-callcc-memory-never-freed.md)
(its interpreter half stays open), and, on the compiled back end,
[r7rs-caught-raise-leaks-runtime-records](r7rs-caught-raise-leaks-runtime-records.md)
and [r7rs-remaining-scratch-leaks](r7rs-remaining-scratch-leaks.md)
(a record or a scratch string nobody frees is garbage like any other).

Progress since the first cut (2026-09-25, second pass): the runtime archive
allocates through the collector, so a Scheme value kept in a Turmeric map or
`rc<T>` cell is seen; a thread start under the collector is refused with the
reason; the collector has its macOS roots, and CI's `macos-latest` leg ran
`tur_r7rs_gc` green on them (105 s, run 36113659875), which was the first
evidence either way. Third pass, the same day: default on (section 5).

## 1. The problem

A Scheme value is a `:heap` box, and the memory model never frees one
(docs/guides/gc-guide.md: "a bare non-`rc<T>` box is never auto-freed on
*any* path"). A Scheme program's data is shared, mutable and cyclic, and the
language gives the programmer no way to say who owns it. So a long-running
Scheme program's memory only grows. A loop that builds a dead four-element
list a million times peaked at 429 MB (r7rs-lang-plan T8).

Reference counting plus the Bacon-Rajan cycle collector (the `rc<T>` path) was
the other candidate. It would need a retain and a release at every copy of an
`any` word in emitted code: in every dialect's dynamic path, in the prelude's
inline C, in the DK runtime. A tracing collector needs none of that.

## 2. The design

**Conservative mark-sweep, for one program's own allocations.**

- **The heap.** `src/runtime/r7gc.c`: 64 KiB chunks from `mmap`, each holding
  one size class (36 classes, 16 bytes to 32 KiB), and larger objects as
  chunk-aligned runs. A hash table maps each chunk to its descriptor, so any
  address -- including a pointer INTO an object, or one with a low tag bit --
  resolves to the object's start. All metadata lives in `mmap`'d memory, never
  in the data segment the collector scans.
- **How a program uses it.** Under the flag, the emitter pastes `r7gc.c` into
  the program's translation unit ahead of the preamble proper, then redirects
  that unit's allocator with object-like macros: `malloc`, `calloc`,
  `realloc`, `free`, `strdup`, `strndup`, and the region allocator's malloc
  fallback and its free (`emit_r7rs_gc_prologue`, emit_module.c). Everything
  the program allocates -- Scheme data, closure environments, DK frames,
  continuation images, vector buffers, the prelude's C -- is on the heap.
  Region runtime sources pasted into the same unit are compiled outside the
  macros (their tables are libc's).
- **The runtime archive allocates through it too.** `libturt_runtime.a` --
  the HAMT behind `stdlib/map`, the rc<T> blocks and cycle collector, the
  owned strings, the symbol table -- is compiled once, without the macros.
  Its TUs allocate through a hook instead (`src/runtime/rt_alloc.h`: a
  libc-defaulted table of the four entry points, redirected by object-like
  macros after each TU's includes), and the collector's constructor installs
  its own entry points before anything else in the unit runs
  (`constructor(101)`, a weak reference so a program that links no archive
  still links). A HAMT node is then a collected object like a pair: scanned
  while the map is reachable, and the Scheme values it holds found through
  it. The same files compiled beside a program by a stdlib autolink marker
  (bare-source mode) carry `rt_alloc.c` on the marker, and the link driver
  keeps a bare `.c` token's first occurrence only. Not hooked: `region.c` /
  `arena.c` (a generation's used bytes are roots in their own right) and
  `trail.c` (its arrays hang off `__thread` variables, which are not in the
  scanned data segment).
- **Roots**, scanned word by word:
  - the C stack, from the collector's frame to the thread's stack base
    (`pthread_getattr_np` on glibc, `pthread_get_stackaddr_np` on macOS),
    with the callee-saved registers spilled into a `jmp_buf` on it;
  - the executable's writable data: `__data_start` to `_end` on Linux, and
    on macOS every writable `LC_SEGMENT_64` of the main image (`__DATA`,
    `__DATA_CONST`), walked from its Mach-O header. The emitted runtime
    keeps its per-thread state in `TUR_THREAD_LOCAL` variables, which the
    flag turns into plain statics, so they are in this range, as are the
    archive's statics;
  - the used bytes of every live and retired region generation
    (`tur_region_each_used`, region.c), for a Turmeric caller that builds
    nodes in a bracket.
- **Marking and sweeping.** Objects are scanned whole, conservatively. The
  sweep rebuilds each class's free list; a large object's mapping is
  released.
- **When it runs.** When the bytes allocated since the last collection pass a
  threshold: 8 MiB, or twice the live size after the last collection, whichever
  is larger.
- **`free` still works.** An explicit `free` of a heap object returns it at
  once; a pointer the collector does not own goes to libc.
- **Continuations.** A `call/cc` image is now allocated after the macros
  (`r7k_measure`, prelude), so it is a heap object and scanned: a continuation
  keeps everything its stack points at, and is itself reclaimed once nothing
  refers to it. The DK frames T5 pins are reclaimed the same way.
- **Threads are refused.** The collector stops no other thread and reads the
  runtime's per-thread state as statics, so a second thread would allocate
  from an unlocked heap and hold roots nowhere the collector looks. Every
  start site in the unit -- the stdlib's `thread-spawn-fn`, `session-spawn`,
  the task-group timeout thread, the emitted multi-threaded scheduler --
  spells `pthread_create`, which the pasted source redirects to a function
  that prints the reason (naming this plan) and exits 70. A program that
  needs threads builds without the flag.

## 3. Measured (2026-09-25, Linux x86-64, gcc 13, default `-O2`)

| program | without | with `--enable=r7rs-gc` |
|---|---|---|
| a dead 4-element list, 10^6 times | 429 MB, 0.39 s | 10 MB, 0.23 s |
| 100,000 escaping `call/cc`s | 527 MB, 0.40 s | 10 MB, 0.13 s |
| a `delay-force` stream 10^6 deep | 108 MB | 10 MB |
| `(map + a b)` over 10^6-element lists (live data) | 590 MB, 0.75 s | 203 MB, 0.89 s |
| `read` of a 10^6-element list | 175 MB, 0.52 s | 75 MB, 0.94 s |
| 200,000 `raise`s caught by `guard` (r7rs-caught-raise-leaks-runtime-records) | 266 MB, 0.25 s | 30 MB, 0.28 s |

Garbage-heavy programs get faster, since they stop touching fresh memory.
Programs that keep a large live set pay for re-marking it: about 1.2x-1.8x
here.

**Correctness.**
- Every `#lang r7rs` fixture passes built with the flag and run with
  `TUR_GC_TORTURE=1`, a collection on EVERY allocation, except
  `r7rs-write-labels`, which is quadratic that way and passes at every 50th.
  Continuations, `eval`, the ports and the numeric tower are all among them.
- chibi's R7RS suite: 1223 passed, 2 settled, 0 failed with the flag, and the
  same under `TUR_GC_TORTURE=50`.
- The same fixtures pass built with ASan and UBSan as well
  (`TUR_GC_TORTURE=20`).
- `region-escape-via-callcc` (a Turmeric entry, a region, a Scheme library,
  `call/cc`) passes with the flag, under torture, and with `TUR_REGIONS=0`.
  (The flag is a no-op for a Turmeric entry file: `r7rs_gc_active` needs the
  `#lang r7rs` directive on the program itself.)
- The instrument: a C test of the collector on its own built two million list
  nodes in a 10 MB process.
- **The archive's blocks** (second pass): a Scheme program that keeps a list,
  a string and a vector only in a Turmeric persistent map, churns 600,000
  dead lists, and reads them back, is right under a collection on EVERY
  allocation (7.2 million collections), with the archive linked and with the
  bare sources; it segfaulted before the hook. `tests/fixtures/r7rs-gc-seam`
  is that program.

**The gate.** `tests/run-r7rs-gc.sh` (ctest `tur_r7rs_gc`, Linux and macOS)
builds every Scheme fixture with the flag and runs it with a collection every
31 allocations (`R7RS_GC_TORTURE=1` for the deep run); runs the map-seam case
under a collection on every allocation; checks that a thread start is
refused under the flag (exit 70, the reason on stderr) and runs without it;
and, on Linux, runs a churn loop under a 256 MiB address-space limit, which
must fit with the collector and must NOT fit without it (`ulimit -v` binds
nothing on macOS). `tests/fixtures/r7rs-gc-basic` and `r7rs-gc-seam` run
with the flag in the ordinary suite.

## 4. Limits

A conservative collector's one real failure is a **missing root**: memory it
does not scan holding the only pointer to one of its objects. What it does not
scan:

- **Memory libc allocates, and the trail.** A buffer libc keeps (a `FILE`,
  `getline`'s), and the backtracking trail's arrays and cells (`trail.c`,
  rooted in `__thread` variables the data-segment scan does not cover, so
  deliberately left on libc). A pure Scheme program puts no pointer to its
  data in either; a Scheme program that reaches `stdlib/trail` through the
  seam and stores a Scheme value in a `bt` cell would. The runtime archive's
  other blocks -- HAMT nodes, rc<T> blocks, strings, symbols -- ARE seen
  since the second pass (section 2).
- **Threads** were refused at the start site (section 2) until
  [docs/archive/r7rs-gc-threads-plan.md](r7rs-gc-threads-plan.md)
  (stages A to D, 2026-09-25): they now run in parallel under the
  collector, which stops the others by signal to collect, with every
  thread's stack, registers, thread-local state, key values and allocation
  cache as roots.
- **Other translation units.** Only a single-unit build uses it. A `--shared`
  or project build ignores the flag (each unit would have its own heap).
- **The interpreter.** `tur --interpret` keeps its values for the life of the
  process by design (gc-guide); the flag changes nothing there.
- **The JIT.** Under `tur jit` the collector compiles to plain libc: a
  JIT'd program's globals live in memory MIR allocates, not in the data
  segment the root scan reads (`TUR_JIT_ENGINE`, set by the engine's prelude).
- **Hoisted inline C** that allocates at file scope runs before the macros,
  so its memory is libc's and not scanned. The prelude's own `call/cc` helper
  was moved for this reason; a user's inline C would have to be too.
- **Linux/glibc and macOS.** Elsewhere (Windows) the entry points are plain
  libc and nothing is collected. The macOS roots were written without a
  macOS box to run them on; the `macos-latest` CI leg's `tur_r7rs_gc` ran
  them green on 2026-09-25 (run 36113659875, and again on the head of PR
  927), so they are confirmed, not only written.
- Conservatism retains a little: a stale stack word or an integer that happens
  to look like a heap address keeps its object alive.

## 5. Graduation

- ~~The archive-allocation hook, so a Scheme program may keep Scheme values in
  Turmeric maps and `rc<T>` cells.~~ Done (second pass, `rt_alloc.h`).
- ~~macOS.~~ Done; the `macos-latest` leg runs `tur_r7rs_gc` green.
- ~~A decision about threads: refuse the flag in a program that spawns them, or
  stop the world.~~ Refused, at the start site, with the reason.
- ~~Default on for `#lang r7rs` compiled programs on Linux and macOS (an
  `--enable=r7rs-gc` reference then a no-op, per the GRADUATED list), with a
  way to build without it for a program that starts threads; and the four
  reports archived.~~ Done (third pass): `g_opt_r7rs_gc` defaults true, the
  `r7rs-gc` row moved to `GRADUATED[]`, `TUR_R7RS_GC=0` / `--no-r7rs-gc`
  opt out (the thread refusal names them), `tests/run-r7rs-gc.sh` builds
  its "with" arms bare and its "without" arms under `TUR_R7RS_GC=0`, and
  the two `r7rs-gc-*` fixtures lost their `flags` file -- every `#lang
  r7rs` fixture of the ordinary suite now runs under the collector. Three
  reports archived; r7rs-callcc-memory-never-freed stays open for the
  interpreter, which is unchanged.
