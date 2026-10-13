# `#lang r7rs`: every split unit recompiles the collector and the DK runtime

**Severity:** medium (build time). On a warm cache, the program unit of a
one-line `#lang r7rs` program is 549 KB of C and costs 0.54 s of cc at `-O2`.
That is 82% of a 0.66 s `tur build` with a Release `tur`. About 86% of the
compile goes to runtime helpers that the unit carries as `static` copies.
Roughly half of it is the conservative collector alone, which every unit
re-optimizes in full because `malloc` is redirected to it. The imported-library
unit
([r7rs-imported-libraries-recompiled-every-build](../archive/r7rs-imported-libraries-recompiled-every-build.md))
pays the same cost again, each time it is compiled cold. Found 2026-10-13 while
measuring where an importing program's build time goes.

## Measured

Linux, 4 cores, gcc. Times are the best of 3 (`-O2 -std=c99 -fno-strict-aliasing
-fwrapv`, compile only). The unit is the program unit of
`(import (scheme base) (scheme write)) (display 1) (newline)`.

| the program unit, compiled as ... | cc | cc with `-fsanitize=address,undefined` (programs importing `(scheme eval)`) |
| --- | --- | --- |
| emitted | 544 ms | 1605 ms |
| collector function bodies replaced by `extern` prototypes | 259 ms | 807 ms |
| every runtime helper's body replaced by a prototype (the floor: only the program's own code is compiled) | 76 ms | -- |

- **gcc's time report** for the emitted unit: 0.49 s of "opt and generate"
  (0.41 s of it callgraph expansion) against 0.25 s of parsing.
- **What survives into the object** is 52 functions and 16.7 KB of code.
  Twenty-nine of them (9.8 KB) are `tur_gc_*`: the allocation path through
  `tur_gc_collect_now`, `tur_gc_mark_roots`, `tur_gc_scan_stack`, the stop and
  resume signal handlers, and the fork hooks. Most of the rest is the DK
  continuation runtime (`dk_run_impl`, `__dk_enter0`, `__dk_reap_*`) and
  `tur_deep_*`.
- **The front end is not where an import costs.** A Release `tur check` (which
  also emits) takes 111 ms for the program above, and an import adds 10-30 ms:
  `(srfi 13)` 128 ms, `(scheme read)` 129 ms. A Debug `tur` takes 530 ms, plus
  40-210 ms per import. Under callgrind, `(srfi 13)` adds 23% more
  instructions, spread over elaboration. The largest single items in the whole
  front end are `elab_lookup_ctor`'s linear `strcmp` scan (about 4.6% of
  instructions, 636k `strcmp` calls), `rt_head_is` (about 3%) and the move checker's
  per-branch binding snapshots (`move_state_snapshot_bindings` /
  `_capture_current` / `_restore` plus `linear_state_snapshot_bindings`, about
  7%). All of these are small next to the cc time above.

## Repro

```sh
export TMPDIR=$(mktemp -d)
printf '#lang r7rs\n(import (scheme base) (scheme write))\n(display 1)\n(newline)\n' > p0.tur
TUR_SHOW_CC=1 tur build p0.tur -o p0      # warms the prelude cache
TUR_SHOW_CC=1 tur build p0.tur -o p0 2>&1 | grep '^CC' | tail -1   # the program unit's .c
cc -O2 -std=c99 -w -fno-strict-aliasing -fwrapv -Isrc/runtime -c -o u.o "$TMPDIR"/tur-build/*p0_tur.c -ftime-report
nm -S --size-sort u.o | grep ' [tT] '     # tur_gc_*, dk_*, tur_deep_*: static copies
```

## Root cause

- **The collector.** The emitter pastes `src/runtime/r7gc.c` verbatim into
  every unit (`emit_r7rs_gc_prologue`, `src/compiler/emit_module.c:12352`),
  then `#define`s `malloc`, `calloc`, `realloc`, `free`, `strdup` and
  `strndup` to its entry points (`emit_r7rs_gc_macros`). Its functions are
  `static`. The split's state transform (`emit_split_state`,
  `src/compiler/emit_split.h`) passes `static` functions through untouched, by
  design: "Stateless helpers ... are written into both and each unit compiles
  the ones it calls". The collector's *state* is already the library unit's
  alone (made `extern` in the other units). But every unit that allocates
  compiles the whole allocate-collect-mark-scan path again, because every
  allocation reaches it through the `malloc` macro.
- **The DK runtime and the deep stack** (`emit_dk_runtime.c`, e.g. the
  `static intptr_t dk_run_impl` at line 1516) are pasted and compiled in each
  unit the same way.

Nothing in the collector is per-unit. It scans process-wide roots (`__data_start`
to `_end`, thread stacks, registered TLS roots). Its state is shared. Whichever
unit's copy calls `tur_gc_init` first installs the signal handlers, and the
copies are interchangeable. One copy, defined in the library unit and called
from the others, behaves the same.

## Fix directions

1. **Define the collector once, in the library unit.** In a split build, paste
   `r7gc.c` with `static` removed from its function definitions, then let the
   existing machinery finish the job:
   - `emit_split_state` keeps them as external definitions in the library unit
     (LIB mode). Each name is recorded with `emit_split_note_export`, so it moves
     to the `tur_sl_` prefix, clear of libturi's own collector.
   - The same transform turns them into prototypes in every later unit
     (CLIENT mode).

   Things to watch:
   - The `static inline` helpers (`tur_gc_check_thread`, `tur_gc_slot_of`):
     export them, or keep them inline.
   - The `TUR_GC_NOASAN` attribute must appear on the prototypes too.
   - Measure the run-time cost of a cross-object call to `tur_gc_malloc`.
     Today it is already an out-of-line `static` call, not inlined.
   - The `(scheme eval)` unit compiles under ASan and the library object it
     links is the ASan one, so both must agree.
   - `tests/check-r7rs-prelude-split.sh` (no state defined in two units) and
     `tests/run-r7rs-gc.sh` (torture) are the checks that matter.

   Expected effect: about -0.29 s of cc on every warm r7rs build (about -0.8 s
   for an ASan-linked program), and the same again on each cold
   imported-library compile. Across the 126 compiled r7rs fixtures that is
   about 35 s of a 230 s serial warm walk.
2. **The same for the DK runtime and `tur_deep_*`.** The floor row above
   bounds what is left after (1): 259 ms down to 76 ms. Keep the helpers whose
   inlining matters at run time (tagging, boxing, carrier conversions) as
   `static inline`. The large, out-of-line pieces (`dk_run_impl`,
   `__dk_drive_after`, the reap list) are the candidates.
3. **Lower priority: the split re-emits the library unit on a warm cache.**
   Under callgrind (cc excluded), the `tur` process emits once for `emit-c`
   (561 M instructions for the program above), twice for a two-unit build
   (810 M), and three times when it imports (`(srfi 13)`: 1096 M against
   942 M for two units). On a warm cache, the library unit's text is emitted
   only to be hashed. The client emission needs only the tables it records:
   signatures, fat boxes, keyword records, exports, and `split_lib_defined`.
   Those tables could be cached beside the object under a key computed before
   emission: the compiler version, the auto-loaded stdlib's sources, and every
   option that changes emitted text. That would skip one emission per build,
   about 30% of `tur`'s own work in a build, or about 0.25 s on a Debug `tur`.
   A key that misses an input would link a program against stale signatures,
   which is why this ranks below (1) and (2).
