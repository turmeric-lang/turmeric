---
title: The Runtime Preamble and Its Linkage
category: Compiler Internals
description: Where the C runtime every compiled program needs comes from -- the emitted preamble, its feature gates and linkage postures, the runtime archives, the S2 split runtime and its generator, tur_tls.c -- which copy each build path links, and how to change any of it without silently losing the fast path
---

# The Runtime Preamble and Its Linkage (internals)

Every Turmeric program needs a C runtime: panics, the DK continuation machine,
fibers and the scheduler, STM, the reference counter and cycle collector,
regions, HAMT, strings, and more. This guide is for contributors who change
that runtime, or who are debugging a build that linked the wrong copy of it.
It covers:

- where the runtime's C text is written;
- how a program's preamble is assembled and gated;
- the several copies of the runtime that exist at once;
- which copy each build path actually compiles or links;
- the S2 split runtime that lets a build skip compiling the preamble;
- how to change any of this safely.

User-facing knobs (`--runtime=`, `TUR_RUNTIME`, `TUR_PREAMBLE_SPLIT`) are
described in the [tur compile / link guide](tur-compile-link-guide.md#tur-build---runtime----how-the-runtime-gets-linked).
This guide is the mechanism underneath them.

> **Trust the code over the comments.** Where a comment and this guide
> disagree, read the code. If the code disagrees with this guide, fix the
> guide. The comments that said a missing runtime archive disengages the
> split were corrected on 2026-10-21
> ([stale-comments-runtime-preamble](https://github.com/turmeric-lang/turmeric/blob/main/docs/archive/stale-comments-runtime-preamble.md)).

**"Split" means four different things in this codebase.** This guide is about
the first one.

| Name | What is split | Where |
|---|---|---|
| **S2 / preamble split** | the fixed runtime preamble, swapped for a declarations region; the definitions come from the host (`tur jit`) or `libturt_preamble.a` (`tur build`) | `jit_try_split_preamble`, `tools/gen-runtime-split.py` |
| r7rs prelude split | a `#lang r7rs` program, compiled as a library unit, an imported-library unit and a program unit | `src/compiler/emit_split.c`, `prelude_split_applies`; see the [r7rs implementation guide](r7rs-implementation-guide.md) |
| `--split-build` | `tur build` as a cacheable `tur compile` plus a `tur link` | [tur compile / link guide](tur-compile-link-guide.md) |
| separate compilation | a project's modules, one TU each, sharing `tur_runtime.h` | `cmd_build_multi`; see the [separate compilation internals guide](separate-compilation-internals-guide.md) |

---

## The short version

1. **The runtime is mostly text the compiler writes.**
   `emit_runtime_preamble` (`src/compiler/emit_module.c`) is a single function
   of about 4,600 lines, mostly `buf_puts` calls. It writes the runtime into the
   top of every emitted translation unit, ending at the marker
   `/* ==== tur: end of fixed runtime preamble ==== */`. The DK machine and
   its relatives come from string emitters in
   `src/compiler/emit_dk_runtime.c`. The region runtime and the r7rs collector
   are real `.c` files pasted in verbatim.
2. **Gates decide what a program's preamble contains.** Whole-program scans
   (`preamble_uses_*`) and flags set during elaboration (`g_needs_*`) turn
   blocks on. So do process-wide switches such as regions, `#lang r7rs` and
   Saffron.
3. **Postures decide linkage.**
   - In single-file mode, the runtime is `static` in the program's TU.
   - In separate compilation, one owner TU (`TUR_RT_OWNER`) defines the state.
   - In *archive posture* (DEDUP-4b), the reference counter, the collector and
     the region runtime are only declared, and `libturt_runtime.a` supplies
     them.
4. **Several copies of the runtime exist:**
   - the preamble emitted into each program;
   - the host runtime compiled into `tur` and `libturi.a` (`tur_core`);
   - `libturt_runtime.a`;
   - `libturt_preamble.a`;
   - the bare `src/runtime/*.c` sources that stdlib's `__tur_autolink__`
     markers name.
5. **The S2 split** generates, from one canonical emission (every gate on),
   three committed files under `src/runtime/generated/`: the runtime as one
   TU, its declarations, and an embedded copy of the declarations plus a hash.
   At build time the compiler re-emits the canonical text and hashes it. On a
   match, it swaps the program's preamble for the declarations. On a mismatch,
   it keeps the whole preamble. That is never wrong, only slower, so a stale
   artifact goes unnoticed unless a probe checks for it. CI has those probes.
6. **Per-thread state** that both halves of a split program touch lives in
   `src/runtime/tur_tls.c`. Each half reaches it in whichever of three
   spellings its compiler can manage.

---

## Source map

| File | Role |
|---|---|
| `src/compiler/emit_module.c` | `emit_runtime_preamble`, `emit_shared_runtime_header`, `emit_rt_split_source`, `emit_hoisted_includes`, `emit_panic_site_slot`, `emit_embedded_runtime_source`, the posture helpers (`emit_rt_global`, `emit_rt_tls`, `emit_rt_defs_begin`/`_end`, `emit_rcgc_global`, `emit_rcgc_prototypes`, `emit_region_runtime_bodies`), the gate scans (`preamble_uses_base_delimited`, `preamble_uses_callcc`, `preamble_uses_serial`) |
| `src/compiler/emit_dk_runtime.c`, `.h` | String emitters: `emit_tur_jmp_buf_prelude` (setjmp selection), `emit_cps_runtime_prelude` (the DK machine), `emit_cps_cloneable_bridge_prelude`, `emit_dk_resume_word_prelude`, `emit_cps_serial_runtime_prelude`, `emit_cps_callcc_prelude` |
| `cmake/embed_region_runtime.cmake`, `src/runtime/region_rt_embed.h` | Turn `src/runtime/{region,arena}.{h,c}` and `r7gc.c` into byte arrays the emitter pastes |
| `src/runtime/generated/tur_rt_split.c` | Generated: the runtime as one TU (S2) |
| `src/runtime/generated/tur_rt_split_decls.h` | Generated: the declarations region, kept for review and diffing |
| `src/runtime/generated/tur_rt_split_embed.c` | Generated: the declarations as a C string, plus `tur_rt_split_hash` |
| `src/runtime/rt_split_embed.h` | Declares the three symbols the embed file defines |
| `tools/gen-runtime-split.py` | Generates the three files from `tur emit-rt-split` |
| `src/runtime/tur_tls.c` | Host-resident thread-local storage, plus an accessor per slot |
| `src/runtime/tur_atomics.c` | Host-resident atomics for front ends without `__atomic_*` (c2mir) |
| `src/async/tur_sjlj_x64_win.S` | A setjmp/longjmp pair that unwinds nothing, for win64 |
| `src/driver/` | `resolve_rcgc_from_archive` and `preamble_split_auto_applies` (`pipeline.c`), `locate_runtime_lib` and `apply_runtime_lib_mode` (`cc.c`), `jit_try_split_preamble` and `cmd_emit_rt_split` (`preamble_split.c`), the `--runtime=` parsing (`cmd_build.c`, `compile_link.c`) and `TUR_RUNTIME` (`cli.c:drv_apply_env_overrides`) |
| `src/CMakeLists.txt` | `tur_core` (all runtime TUs, including `tur_rt_split.c`), `turt_runtime` (`TURT_RUNTIME_SOURCES`), `turt_preamble` |
| `tools/gc-copy-diff.py` | Diffs the emitted rc/GC replica against `src/runtime/{rc,gc,rc_free_queue}.c` |
| `docs/upcoming/cc-path-preamble-split-plan.md` | Open plan: the split on the cc path (default on Linux and Windows; macOS left) |

---

## Where the runtime's C text comes from

### Three ways it is authored

| Kind | Examples | How it reaches a program |
|---|---|---|
| **String emitters** | almost all of `emit_runtime_preamble`; `emit_closure_fat_runtime`; the `emit_dk_runtime.c` preludes | written into the preamble by `buf_puts` (or one long string literal per prelude) |
| **Host sources, pasted verbatim** | `src/runtime/region.h`, `arena.h`, `arena.c`, `region.c`; `src/runtime/r7gc.c` | embedded at build time by `cmake/embed_region_runtime.cmake`. `emit_embedded_runtime_source` pastes the bytes, replacing each local `#include "..."` line with a comment, because the emitter pastes those headers itself and a `cc` of the output has no `src/runtime` on its include path |
| **Host sources, never pasted** | `hamt.c`, `trail.c`, `symbols.c`, `tur_string.c`, `rt_alloc.c`; `rc.c`, `gc.c`, `rc_free_queue.c`; `contract_handler.c`; `tur_atomics.c`, `tur_tls.c` | compiled from source when a stdlib `__tur_autolink__` marker names them (for example, `stdlib/hamt.tur`'s `src/runtime/hamt.c src/runtime/rt_alloc.c`), or linked from `libturt_runtime.a`, or resolved in-process under the JIT |

The collector is the awkward case. `src/runtime/{rc,gc,rc_free_queue}.c` is
the maintained copy. `emit_runtime_preamble` also carries a hand-written
replica as string emitters, which a program runs only when it does not link the
archive (see [Postures](#postures-how-the-same-text-links-three-ways)).
`tools/gc-copy-diff.py` measures the drift between the two. On 2026-10-16 it
reported 29 identical functions, 30 divergent ones, and 16 that exist only in
the runtime.

### What a preamble looks like, in order

The order is load-bearing. Several comments in `emit_runtime_preamble` record
a build that broke because a block moved.

| Region | Notes |
|---|---|
| `/* generated by tur (phase 2) */` | the start marker `jit_try_split_preamble` searches for |
| prologue macros | `_DEFAULT_SOURCE`; `NOMINMAX` on `_WIN32`, which has to come before any Windows header; the `-Wunused-*` pragmas; `TUR_RT_LOCAL` (shared mode only); `TUR_THREAD_LOCAL`; `TUR_TLS_FRESH`; `TUR_ATOMIC_*` |
| `static void __tur_static_init(void);` | defined per program, below the marker |
| includes | `hamt.h` and `<dlfcn.h>` when gated; Winsock on `_WIN32`, else the BSD socket headers; then `<ucontext.h>` under `_XOPEN_SOURCE 700`, then `<setjmp.h>` and the rest. The socket headers have to come before `ucontext.h` on macOS |
| host prototypes | the `tur_atomic_*` externs for non-GNU front ends, and `tur_hamt_hash_xxh64`. These must come *after* `<stdint.h>`; a first attempt declared them up with the macros and broke every fixture |
| r7rs collector | `emit_r7rs_gc_prologue` pastes `r7gc.c` and `#define`s `malloc`/`free`/... to it, only when `r7rs_gc_active` (a single-unit `#lang r7rs` or Saffron program) |
| platform shims | `emit_win_ucontext_shim`, then `emit_tur_jmp_buf_prelude` (the landing-pad type every later block uses) |
| core runtime | cons cells, sets, handler and closure types, `emit_closure_fat_runtime`, then `emit_region_runtime_bodies` |
| STM, panics, catch-unwind; `emit_winsock_compat_shim` (gated on `g_needs_winsock`); the deep-stack helper | |
| CPS / DK machine and friends | gated (see below) |
| fibers, threads, cancellation, the multi-threaded scheduler, futures, `select`, timers | |
| `rc<T>` / GC | in runs bracketed by `emit_rt_defs_begin`/`_end` |
| session-type channels and routers | ends with `tur_role_close` |
| `/* ==== tur: end of fixed runtime preamble ==== */` | the end marker |

Two things are emitted **after** the marker by every caller, deliberately:

- `emit_hoisted_includes`: the `#include` lines elaboration lifted out of
  inline-C bodies. They are per-program, so inside the marked span they would
  make the span differ from the canonical text, and the split would disengage
  for every C-binding spice. That is exactly what
  [jit-s2-split-disengages-on-hoisted-inline-c-include](https://github.com/turmeric-lang/turmeric/blob/main/docs/archive/jit-s2-split-disengages-on-hoisted-inline-c-include.md)
  was.
- `emit_panic_site_slot`: the `tur_cur_site` thread-local. A thread-local
  inside the span is reached through a host accessor call in a split build,
  which would cost two calls around every `vec-get`. Below the marker it is a
  native thread-local under gcc/clang.

The end marker is emitted unconditionally, in every mode, so any consumer can
split any emitted C at it. Nothing in the compiler except the split reads it.

### Feature gates

Most of the preamble is unconditional. The gated parts are switched by four
kinds of input:

| Kind | Examples | Set by |
|---|---|---|
| Whole-program scans | `preamble_uses_base_delimited`, `preamble_uses_callcc`, `preamble_uses_serial`, `cps_expr_contains_cloneable_shift`, `expr_has_multishot_handler`, `emit_cps_ir_program_has_emittable` | a pure syntactic walk of `program` |
| Per-program flags | `g_needs_hamt`, `g_needs_dlfcn`, `g_needs_regex_h`, `g_needs_winsock`, `g_has_variadics`, `g_cps_path`, `g_trail_autoloaded` | elaboration and stdlib autoload (`src/runtime/globals.c` and others) |
| Process-wide switches | `regions_enabled()`, `g_opt_r7rs`, `g_opt_dynamic_any` (Saffron), `r7rs_gc_active` | dialect and experiment flags |
| Knobs baked into the text | `g_backtrack_depth` (`BACKTRACK_DEPTH_DEFAULT`) | command-line flags |

The CPS scans are computed **once**, as the `cps_uses_*` constants partway
through `emit_runtime_preamble`, and every later gate reads those constants.
The scans look for the *presence* of a form, not at whether the CT-IR backend
lowered it. That way a delimited form in a function the backend evicted still
gets its prelude (see the [CPS/DK internals guide](cps-dk-internals-guide.md)).

Every scan gate is written `g_rt_split_all_gates || shared || <scan>`.
`shared` forces the block on for `tur_runtime.h`, which has to serve every
module. `g_rt_split_all_gates` forces it on for the canonical S2 text.

---

## Postures: how the same text links three ways

`emit_runtime_preamble(out, program, shared)` writes one text, but how its
definitions link depends on the mode.

**Single-file** (`shared == false`, `emit_program`). One self-contained TU.
Globals are `static` (`emit_rt_global` writes `static <body>`), and most
runtime functions are written `static` literally. Two groups have external
linkage: the dozen or so functions spelled with the `rt_fn` prefix (the STM
API among them), which is empty in this mode, and the rc/GC API (`rcgc_api`).
The output is meant to be byte-identical to what it was before the other
postures existed, which is why so many helpers take a `shared` flag and do
nothing when it is false.

**Shared** (`shared == true`, `emit_shared_runtime_header` -> `tur_runtime.h`).
This is used by project builds and `--shared` libraries:

- `cmd_build_multi_files` writes `tur_runtime.h` plus a one-line owner TU,
  `tur_runtime.c`, which `#define`s `TUR_RT_OWNER` and includes the header.
- `emit_rt_global` defines each global only under `TUR_RT_OWNER` and declares
  it `extern` everywhere else.
- Ordinary runtime functions are replicated `static` into every module
  (`rt_fn` is `static ` in this mode). They all operate on the one set of
  globals.
- The rc/GC family is defined once, inside the owner guard, at `TUR_RT_LOCAL`
  (hidden) visibility, so a `.so` does not export its collector.
  `emit_rcgc_prototypes` gives the other modules the declarations (DEDUP-3).
  The pasted region runtime sits inside the same owner guard.
- `program` is `NULL`, and every scan gate is forced on.

**Archive** (`g_rcgc_from_archive`, DEDUP-4b). The rc/GC and region
definitions are left out, and the program links `libturt_runtime.a` instead:

- `emit_rt_defs_begin` records the buffer length, and `emit_rt_defs_end`
  truncates back to it, so the definitions are not just disabled but gone.
- `emit_rcgc_global` omits each global outright instead of declaring it
  `extern`. The two copies disagree on some types (`rc_free_queue` is an array
  in one and a struct in the other), and an `extern` with the wrong type links
  silently.
- `emit_region_runtime_bodies` writes a one-line comment instead of the pasted
  sources.
- `emit_rcgc_prototypes` spells the allocator's `value_type` parameter as
  `uint8_t` to match `rc.c`. The replica spells it `int`.

`resolve_rcgc_from_archive` (`src/driver/pipeline.c`) chooses the archive posture before
emission. The checks run in this order, and the first that applies wins:

1. `TUR_RCGC_FROM_ARCHIVE=0` -> replica.
2. `--runtime=source` -> replica.
3. Not emitting for a link `tur` performs itself (`g_emit_for_link` false:
   bare `tur emit-c`, `--shared`), and no `TUR_RCGC_FROM_ARCHIVE=1` -> replica.
4. Otherwise -> archive, if and only if `locate_runtime_lib` finds
   `libturt_runtime.a`. `=1` does not get around this: with no archive on disk
   you still get the replica.

`emit_rcgc_from_archive()` reports the decision back, so the link step can put
the archive on the line. `cmd_build_multi_files` re-probes for it. Its omission
was the "DEDUP-4b project half" bug: every multi-module executable failed with
`undefined reference to tur_region_shutdown`.

**Split units** (the r7rs prelude split). `emit_split_once_fns` runs blocks
that are the same in every unit, such as the DK machine, the deep-stack helper
and `r7gc.c`, through `emit_split_export_static_fns`. The library unit then
defines them once and the later units get prototypes. Outside a split it
writes the text unchanged.

**Thread-locals** in `emit_module.c` go through `emit_rt_tls`, which writes
`#if defined(__GNUC__) || defined(__clang__)` with the native variable, and
`#else` with an `extern` accessor from `tur_tls.c` and a `#define` of the
name. The `emit_dk_runtime.c` preludes spell the same two arms out by hand.
See [Per-thread state](#per-thread-state-tur_tlsc).

---

## The copies of the runtime

| Copy | Built from | Built by | Instrumented in Debug? |
|---|---|---|---|
| Emitted preamble | `emit_runtime_preamble` + pasted sources | the program's own `cc`, every build | no (the program's flags) |
| `tur_core` (inside `tur` and `libturi.a`) | every `src/runtime/*.c`, **including** `generated/tur_rt_split.c` and `generated/tur_rt_split_embed.c` | CMake, with `tur`'s own flags | yes (ASan/UBSan) |
| `libturt_runtime.a` | `TURT_RUNTIME_SOURCES`: `hamt`, `trail`, `symbols`, `tur_atomics`, `tur_tls`, `tur_string`, `rc`, `gc`, `rc_free_queue`, `rt_alloc`, `region`, `arena`, `contract_handler` | CMake target `turt_runtime`, `-O2 -std=c99 -fno-strict-aliasing -fwrapv` | no, by design |
| `libturt_preamble.a` | `generated/tur_rt_split.c` (+ `tur_sjlj_x64_win.S` on Windows) | CMake target `turt_preamble`, `-O2 -w -ffunction-sections -fdata-sections` | no |
| Bare sources | `src/runtime/*.c` named by `__tur_autolink__` markers | the program's `cc`, every build | no |
| Owner TU | `tur_runtime.{h,c}` | `cmd_build_multi`, per project build | no |

Notes on this table:

- `tur_rt_split.c` **replaced** the host's own older copies of the same
  subsystems (`runtime.c`, `cps_prompt.c`, `stm.c`, `scheduler.c`,
  `timer_wheel.c`). Keeping both in `tur_core` would have been a
  duplicate-symbol error, and would have let the JIT's `dlsym` bind a program
  to the wrong one (see the comment beside it in `src/CMakeLists.txt`). The
  host runtime *is* the generated preamble.
- A static archive member is extracted only to resolve an undefined symbol.
  That is why `libturt_runtime.a` can carry `rc.c` and `gc.c` safely: a
  replica-posture program defines those symbols itself and never pulls the
  members in. It is also why `libturt_preamble.a` is its **own** archive. A
  program with the whole preamble defines its symbols, and pulling in one
  member of `tur_rt_split.c` would collide on all the rest.
- `libturt_preamble.a` is one object. Without `--gc-sections` (ELF) or
  `-dead_strip` (Mach-O), the first reference pulls in the whole preamble. A
  one-line program grew from 23 KB to 93 KB stripped before the section flags
  went in. MinGW links without `--gc-sections` today.
- `cmake --build build --target tur` builds neither archive. Programs built
  by a `tur` from such a tree run the replica and the whole preamble, and
  recompile the bare sources.

### Which copy each build path uses

"Fixed runtime" means the part of the preamble above the marker, other than
rc/GC and regions.

| Build path | Fixed runtime | rc/GC + regions | hamt, trail, string, symbols |
|---|---|---|---|
| `tur emit-c f.tur` | inline (full preamble) | inline replica | bare sources, if the caller follows the markers |
| `tur build`/`tur run`, Linux or Windows, both archives found (default) | **`libturt_preamble.a`** (decls region in the TU) | `libturt_runtime.a` | `libturt_runtime.a` |
| the same, but macOS, `--debug`, `-fsanitize` in `TUR_CC_FLAGS`, `TUR_PREAMBLE_SPLIT=0`, no `libturt_preamble.a`, or a hash mismatch | inline | `libturt_runtime.a` | `libturt_runtime.a` |
| no archive locatable, or `--runtime=source` | inline | inline replica | bare sources, recompiled |
| `--runtime=lib` with only `libturi.a` found | inline | inline replica | `libturi.a` |
| `--runtime=split` | decls region (says so when it declines) | as `auto` | as `auto`; with no archive found it falls back to `-lturi` with a warning |
| `--target wasm` | inline (`emcc`; never the split) | inline replica (a located archive is native, so it is never taken -- not even under `TUR_RCGC_FROM_ARCHIVE=1`) | bare sources through `emcc` |
| `#lang r7rs` (Linux, macOS) | inline, in the r7rs library unit (an r7rs preamble never matches the canonical text) | as `auto` | as `auto` |
| `tur build <dir>` executable | `tur_runtime.h` + owner TU (never the split) | `libturt_runtime.a` if found (re-probed at link) | `libturt_runtime.a` if found; this hand-built link line scans no `__tur_autolink__` markers |
| `tur build --shared` | owner TU | owner-TU replica, hidden | no archive, no marker scan |
| `tur jit`, REPL spice image | host (`tur_core`'s `tur_rt_split.c`), when the hash matches; else inline | host (`rc.c`, `gc.c`) when the split engaged or an archive was found at emit time; else inline | host |

`apply_runtime_lib_mode` runs only when the program's autolink markers name a
bare `.c` source. That is effectively always: a one-line program reaches
`hamt.c` and `trail.c` through the stdlib it auto-loads. `TUR_SHOW_CC=1` prints
the final `cc` line, which shows every row of this table directly.

The [GC guide](gc-guide.md#which-copy-of-the-collector-runs) covers the rc/GC
column from the collector's side, including why a `.so` keeps its own.

---

## The S2 split runtime

The fixed part of the preamble is the same in every program, but every program
compiled it again: about 4,200 lines of a 9,300-line TU for a one-line program
in archive posture, measured 2026-10-16. S2 compiles it once. It started as a
JIT optimization ([jit-guide](jit-guide.md)) and became the cc path's default
on Linux and Windows on 2026-10-02
([plan](https://github.com/turmeric-lang/turmeric/blob/main/docs/upcoming/cc-path-preamble-split-plan.md)).

### The canonical text

`emit_rt_split_source` produces the preamble that the artifacts describe. It
is single-file linkage, feature-complete, and ends at the marker. Before it
emits, it saves and forces this state, and afterwards it restores it:

| Forced | Why |
|---|---|
| `g_rt_split_all_gates = true` | every `cps_uses_*` scan gate on; also selects the split-safe variants in `emit_tur_jmp_buf_prelude` and the `regex.h` block |
| `g_needs_hamt`, `g_needs_regex_h`, `g_has_variadics`, `g_cps_path`, `g_needs_winsock` = true | per-program facts, not knobs |
| archive posture (`emit_set_rcgc_from_archive(true)`) | so the text does not depend on whether this box has a `libturt_runtime.a` |
| `g_trail_autoloaded = true` | whether `trail.tur` was autoloaded into *this* compile differs between `tur emit-rt-split` and a real build |

Knobs (`--backtrack-depth`, experiments, dialect flags) are deliberately
**not** normalized. The text reflects the current process, so the hash catches
them. That is why a `#lang r7rs` or Saffron program always declines: its
`g_opt_r7rs` / `g_opt_dynamic_any` blocks change the text.

`tur emit-rt-split` prints this text, and `tur emit-rt-split --hash` prints its
xxHash64 (`tur_hamt_hash_xxh64`, the same function the guard uses).
`cmd_emit_rt_split` also forces the archive posture before calling.

### The generator

`tools/gen-runtime-split.py --tur ./build/tur [--out src/runtime/generated]`
runs `tur emit-rt-split` twice (the text and the hash), splits the text at the
marker, and walks the preamble line by line. The main rules:

| Shape | Runtime TU (`tur_rt_split.c`) | Decls half |
|---|---|---|
| file-scope function definition | body, with `static` (and `static inline`) removed | prototype |
| one of `INLINE_INTO_DECLS` (`tur_frame_init`, `tur_frame_push_defer`, `tur_frame_fire_lifo`) | body, de-static'd | the whole body as `static inline` |
| `static` object | definition, de-static'd | `extern` declaration |
| `static` function-pointer object (`(*name)(...)`) | as above | as above (a separate match, because the declarator has parentheses) |
| `static` thread-local named in the `TLS` table | `#if !_WIN32 && !TUR_TLS_FRESH` a direct read of `tur_tls.c`'s storage, `#else` its accessor | verbatim (see below) |
| `static` thread-local **not** in the table | a native external thread-local of its own | verbatim |
| `static` prototype whose definition is not in the preamble (`__tur_static_init`) | dropped | kept `static` |
| non-`static` initialized global (`tur_closure_headers_enabled`) | definition | `#ifdef TUR_RT_SPLIT_HOSTED` `extern` `#else` the definition `#endif` |
| `#if defined(__GNUC__) \|\| defined(__clang__)` followed by a thread-local | unchanged | rewritten to `... && !defined(TUR_RT_SPLIT_HOSTED)`, then given an extra `#elif` direct-read branch (`direct_tls_hosted`) |
| quoted project `#include "..."` | kept | wrapped in `#ifndef TUR_RT_SPLIT_HOSTED` |
| anything else | verbatim | verbatim |

Every rule in that table was added after a failure:

- `tur_frame_*` were inlined into the decls half because, behind an opaque
  call, a 536-byte `tur_frame` could no longer be optimized away. The stack
  frame of every scope with a defer grew about 6.7x, and a Windows fixture
  overflowed its 2 MiB stack.
- `tur_closure_headers_enabled` gets the guard because each simpler fix broke
  something. Declaring it `extern` broke the JIT's `jit_sync_config_globals`
  handshake, which needs a data item in the program. Making it `weak` broke the
  PE/COFF link.
- The thread-local guard rewrite exists because a hosted gcc build took the
  native branch and gave the program private copies of `tur_panicking` and
  `tur_current_fiber`. That broke every fiber, async, panic and catch-unwind
  fixture at once.

The preamble's choice between `#include "hamt.h"` and loose `extern`
declarations differs from program to program. That is why project includes
are guarded out of the hosted decls, and why `jit_try_split_preamble` copies
the program's own quoted includes back in. See
[cc-path-split-windows-and-hamt-findings](https://github.com/turmeric-lang/turmeric/blob/main/docs/archive/cc-path-split-windows-and-hamt-findings.md).

### The three committed files

| File | Contents | Consumed by |
|---|---|---|
| `tur_rt_split.c` | the runtime TU, with `source hash <h>` in its header | `tur_core` (the JIT's host runtime, and `libturi.a`); `libturt_preamble.a` |
| `tur_rt_split_decls.h` | the decls half, ending `/* ==== S2: host-resident runtime; declarations only ==== */` | **nobody at build time**; it exists for review and diffs |
| `tur_rt_split_embed.c` | `tur_rt_split_hash`, `tur_rt_split_decls[]`, `tur_rt_split_decls_len` | `tur_core` (it moved there from the JIT-only object when `--runtime=split` made the cc path need it) |

The generator writes with `newline='\n'`, so a regeneration on Windows is not
a whole-file CRLF diff.

### Engaging: `jit_try_split_preamble`

One function serves `cmd_jit`, the REPL's JIT image build, and `cmd_build`. It
returns false (the caller keeps the whole TU) when:

1. `TUR_JIT_NO_SPLIT` is set;
2. the hash of a **fresh canonical emission** under the current process state
   differs from `tur_rt_split_hash`;
3. the start or end marker is missing from the program's TU.

Note what the hash covers. It does not hash the program's own preamble, which
has fewer gates on. It checks that this compiler, in this process state, still
produces the text the artifacts were generated from. The swap then replaces the
program's preamble with the full declarations region, which declares a
superset of what the program's preamble defined.

On success the output is:

1. whatever came before the start marker (hoisted user text);
2. the embedded decls region;
3. every quoted `#include "..."` line the replaced span contained;
4. everything after the end marker.

Under `TUR_JIT_TIMING=1` each decline prints a tab-separated line,
`TUR_JIT_TIMING split disengaged <reason>`, to stderr.

On the cc path (`cmd_build_once`):

- `--runtime=split` always tries the swap and prints a message when it
  declines. It is refused for `--target wasm`.
- `--runtime=auto` tries it when `preamble_split_auto_applies()` holds and the
  build is not an r7rs prelude split. That requires:
  - `TUR_PREAMBLE_SPLIT` is not `0`;
  - the host is Linux or Windows, unless `TUR_PREAMBLE_SPLIT=1`;
  - not `--debug`;
  - no `-fsanitize` in `TUR_CC_FLAGS`;
  - the program's own preamble is in archive posture;
  - `libturt_runtime.a` and `libturt_preamble.a` sit side by side.
- `__tur_include__` hoisting runs **after** the swap. Hoisting first put
  `#include "hamt.h"` inside the span the swap deletes.
- The link line gets `-lturt_preamble -L<dir>` plus `--gc-sections` or
  `-dead_strip`. The compile gets `-DTUR_RT_SPLIT_HOSTED`, which selects the
  `extern` / direct-read / no-project-include arms of the decls.

The archive's hash is not checked. A `libturt_preamble.a` from another build,
sitting beside a newer `tur`, would link rather than decline. The plan lists
this under "Left".

### Per-thread state: `tur_tls.c`

c2mir parses `_Thread_local` and then treats the variable as an ordinary
global, and it has no `__thread` at all. So every runtime thread-local that
code compiled by c2mir might touch has a slot in `src/runtime/tur_tls.c`. The
slot is a real `__thread` variable in the host, with an accessor that returns
its address:

```c
__thread int tur_tls_panicking = 0;
int *tur_tls_panicking_ptr (void) { return &tur_tls_panicking; }
```

Slots are typed `void *`, `int`, `bool`, `int64_t`, `size_t`, `intptr_t`,
`jmp_buf` or aligned raw bytes. The emitted macro casts back to the
preamble-private type. A name reaches its storage in one of three ways:

| Who compiles it | Spelling | Where it comes from |
|---|---|---|
| gcc/clang, whole preamble, one TU | native `TUR_THREAD_LOCAL` variable | the first arm of `emit_rt_tls` |
| a real cc compiling one half of a split (Linux/macOS, no `TUR_TLS_FRESH`) | `extern __thread T tur_tls_x;` and `#define x (*((T *)&tur_tls_x))` | `direct_tls_hosted` / the generator's `DIRECT_TLS` arm (performance-audit R-2: an accessor call per panic-flag check cost about 25% of a hosted program's cycles) |
| c2mir; Windows; `TUR_TLS_FRESH` builds | `#define x (*(T *)tur_tls_x_ptr())` | the `#else` arm |

`TUR_TLS_FRESH` (clang, gcc on Windows, the sanitizers) is a separate
mechanism layered on top. These compilers keep a thread-local's *address* live
across a call, and a fiber can resume on another thread. So a thread-local
that moves with a fiber is read through a `noinline` accessor with a
memory-clobbering `asm`. See
[fiber-tls-address-reuse](https://github.com/turmeric-lang/turmeric/blob/main/docs/archive/fiber-tls-address-reuse.md).

The generator reads `tur_tls.c` with a regex instead of keeping its own table.
Keep each slot to the two-line shape above: a `__thread` definition, then a
one-`return` accessor. The generator exits with an error on a `static __thread`.

The generator's `TLS` table (name -> type and accessor) decides which
preamble thread-locals the **runtime TU** routes to that storage. A
thread-local outside the table becomes a separate native variable in
`tur_rt_split.c`, while the program half still reaches `tur_tls.c`'s slot
through its `#else` / direct arm. That gives two storages. It is harmless only
while no program-half code touches the name. Today the call/cc escape set
(`tur_escape_live*`) and the r7rs dynamic environment (`tur_r7rs_dyn`) are
outside the table. The r7rs prelude's inline-C reads both, and only r7rs
programs reach that code, which never take the split.

### CI checks

The split fails closed, so CI checks that it is actually taken. Each of these
probes was added after the split silently disengaged and nobody noticed:

| Job (`.github/workflows/ci.yml`) | Step | Asserts |
|---|---|---|
| `test` (Linux, fixtures leg) | "Verify the preamble split engages by default" | a default `tur build` of a one-liner has `-lturt_preamble` on its link line, and the binary runs |
| `windows` | same name | the same on Windows |
| `jit` | "Verify the S2 split path actually engages" | `TUR_JIT_SPLIT_DEBUG=1 tur jit` prints `probe == committed`. The error message names the two causes: stale artifacts, or a new gate missing from the forced set |
| `whole-preamble` | "Verify the split does NOT engage" | `TUR_PREAMBLE_SPLIT=0` keeps the whole preamble while `libturt_preamble.a` exists, then runs the suite on that path |

`tests/run.sh` asks the same question (one probe build) and folds the resolved
preamble mode into its stamp cache key. Before that, an A/B run of the two
paths reported green for a run that never happened
([report](https://github.com/turmeric-lang/turmeric/blob/main/docs/archive/run-sh-stamp-cache-ignores-the-preamble-split-mode.md)).

---

## Changing the preamble safely

Any change to the text above the marker changes the canonical hash. That
includes text in `emit_dk_runtime.c`, in a pasted `src/runtime` source, or in a
knob's default. Without a regeneration, the split disengages on every
platform, and only CI's probes notice.

### The steps

1. **Edit** the emitter.
2. **Rebuild all targets**, so both archives exist:
   `cmake --build build -j`, not `--target tur`.
3. **Regenerate**, from the repository root (the default `--out` is
   relative):

   ```sh
   python3 tools/gen-runtime-split.py --tur ./build/tur
   ```

   It prints the line counts, the hash, and the static prototypes it kept.
4. **Rebuild again.** `tur_core` and `libturt_preamble.a` compile the new
   files. The canonical text does not depend on them, so one regeneration is
   enough; it does not need a second pass.
5. **Check that it engages:**

   ```sh
   # must equal the 0x...ull constant in tur_rt_split_embed.c
   ./build/tur emit-rt-split --hash
   printf '(defn main [] : int 0)\n' > /tmp/p.tur
   # JIT: probe == committed
   TUR_JIT_SPLIT_DEBUG=1 ./build/tur jit /tmp/p.tur 2>&1 | grep split-debug
   # cc (Linux/Windows): the link line names -lturt_preamble
   TUR_SHOW_CC=1 ./build/tur build /tmp/p.tur -o /tmp/p 2>&1 \
     | grep -c turt_preamble
   ```

6. **Regenerate the fixture snapshots** with the loop in `CLAUDE.md`
   ("Fixture Snapshots"). The `tests/fixtures/*/expected.c` files are bare
   `tur emit-c` output: the whole preamble, in replica posture. Any preamble
   change moves all 157 of them. Commit them in the same PR.
7. **If you touched the rc/GC text**, run `tools/gc-copy-diff.py` (add `--diff`
   for the hunks), and port the change to `src/runtime/{rc,gc,rc_free_queue}.c`,
   or the other way round. Compiled programs that link the archive run the
   `.c` files. Bare `emit-c`, `--shared` and archive-less builds run the
   replica.
8. **Commit** the emitter change, the three generated files and the snapshots
   together.

### Adding a gate

A new per-program flag that changes text above the marker has to be added to
the forced set, the `rt_split_forced[]` table above `emit_rt_split_source`
(one row: the flag and its canonical value; the loop saves, forces and
restores it), and the artifacts regenerated. Otherwise every program that sets the flag hashes
differently and silently loses the split. `g_needs_dlfcn` is the live example.
`call-ptr` and `dlopen` set it, it is not forced, and so those programs keep
the whole preamble (on 2026-10-16 `jit-ffi-call-ptr` hashed to
`f0aa6e93ec129085` against a committed `ac7ad1dcc6c613ab`). The plan records
this. The CI probe cannot catch it, because its one-liner sets no flags.

A gate that is really a knob (it should change behavior, not just which code
is present) stays out of the forced set. Programs built with that knob then
decline, which is correct.

If a gated block would be wrong in the canonical text, branch on
`rt_split_canonical_emission()`. The `regex.h` block does this, so the
committed TU never contains the Windows `#error`.

### Adding a thread-local

1. Emit it with `emit_rt_tls`, giving the native definition, the `extern`
   declaration, the name, the accessor's return type, the accessor's name and a
   cast. The helper also registers it as an r7rs-gc root, through
   `r7gc_note_tls_root`.
2. Add the two-line slot to `src/runtime/tur_tls.c`. That file is in both
   `tur_core` and `TURT_RUNTIME_SOURCES`.
3. Add the name to the `TLS` table in `tools/gen-runtime-split.py` if the
   runtime half and the program half can both touch it.
4. If it belongs to the running code rather than the thread, swap it in
   `tur_fiber_block_resume` and give it a `TUR_TLS_FRESH` accessor.
   `tur_panicking` belongs to the thread and does not.
5. Regenerate.

### Spelling rules: c2mir, Windows, and two halves

The preamble is compiled by gcc, clang, MinGW gcc, `emcc` and c2mir, and a
split program has two halves that may be compiled by *different* front ends.

| Rule | Why |
|---|---|
| No `__thread` literal; use `TUR_THREAD_LOCAL` | c2mir only registers C11 `_Thread_local`. The macro picks by `__STDC_VERSION__`, so the cc path (`-std=c99`) keeps `__thread` |
| No `__atomic_*` literal; use `TUR_ATOMIC_*` | c2mir has no atomic builtins. The fallback calls `tur_atomics.c` |
| A preprocessor test on the compiler (`__GNUC__`) must not choose between two mechanisms that have to pair across halves | c2mir defines neither `__GNUC__` nor `__clang__`. A test on the **platform** (`_WIN32`) is safe, because c2mir predefines it |
| setjmp/longjmp: use `TUR_SETJMP`/`TUR_LONGJMP` and `tur_jmp_buf` | `emit_tur_jmp_buf_prelude`. In the canonical text, Windows always gets `tur_sjlj_set`/`_jump` (no SEH unwind; MIR emits no `.pdata`). In a whole-preamble TU, Windows+GNU gets `__builtin_setjmp` and Windows without GNU (c2mir) gets the sjlj pair |
| Declarations spelled in `uint64_t` etc. go after `<stdint.h>` | the macro block comes before every include |
| Every host function the preamble calls needs a prototype | an implicit declaration truncated `tur_hamt_hash_xxh64` to `int`, and c2mir on arm64 macOS passed it as variadic (SIGBUS) |
| `NOMINMAX` before any Windows header | `<winsock2.h>` pulls in `windef.h`'s `min`/`max` macros, which broke the emitted `max` function |
| Do not reach for inline asm, or for headers c2mir cannot open | c2mir has no inline asm (hence `tur_win_stack_base` in `tur_tls.c`). The pasted `arena.c` includes `<stdatomic.h>`, which the JIT's whole-preamble path has to be able to find |
| Nothing per-program above the marker | it breaks the hash for that program (hoisted includes, the panic site slot) |

### Adding a host runtime file

A `.c` file a compiled program calls by name, as opposed to text the emitter
pastes, belongs in **both** `TUR_CORE_SOURCES` (so the JIT and the
interpreter can resolve it) and `TURT_RUNTIME_SOURCES` (so a cc build can link
it). Two archived reports are about a file that was in only one list:
`symbols.c` missing from `libturi.a`
([libturi-symbols-basename-collision](https://github.com/turmeric-lang/turmeric/blob/main/docs/archive/history/libturi-symbols-basename-collision.md)),
and `contract_handler.c` missing from the lean archive
([o0-build-cannot-link-contract-handler](https://github.com/turmeric-lang/turmeric/blob/main/docs/archive/o0-build-cannot-link-contract-handler.md),
hidden at `-O2` because dead-code elimination removed the only reference).

---

## Pitfalls from the archive

| Report | What went wrong | Lesson |
|---|---|---|
| [jit-s2-split-disengages-on-hoisted-inline-c-include](https://github.com/turmeric-lang/turmeric/blob/main/docs/archive/jit-s2-split-disengages-on-hoisted-inline-c-include.md) | hoisted inline-C includes sat inside the marked span; every C-binding spice lost the split | keep per-program text below the marker |
| `emit_rt_split_source` comments; the `jit` CI step | the archive-posture flag, then `g_trail_autoloaded`, differed between `emit-rt-split` and a real build; the split disengaged everywhere, twice | a new state input to the preamble joins the forced set -- since 2026-10-21 one row in `rt_split_forced[]`, so the list is the code |
| [cc-path-split-windows-and-hamt-findings](https://github.com/turmeric-lang/turmeric/blob/main/docs/archive/cc-path-split-windows-and-hamt-findings.md) | (1) out-of-line `tur_frame_*` multiplied stack frames by about 6.7x (Windows `STATUS_STACK_OVERFLOW`, reported by bash as exit 127); (2) the frozen `hamt.h` include conflicted with loose externs | the split changes codegen as well as linkage; check inlining-sensitive helpers |
| [cc-path-preamble-split-plan](https://github.com/turmeric-lang/turmeric/blob/main/docs/upcoming/cc-path-preamble-split-plan.md), "Where it actually got to" | 16 of the first 20 split failures were duplicated thread-locals: the hosted gcc build took the native branch | both halves must reach one storage |
| [release-archive-cannot-compile](https://github.com/turmeric-lang/turmeric/blob/main/docs/archive/release-archive-cannot-compile.md) | the flat release layout matched no `locate_runtime_lib` candidate; source mode then wanted `src/runtime/*.c`, which the release does not ship | `locate_runtime_lib` probes `<exe_dir>` too; install and ship every archive |
| [o0-build-cannot-link-contract-handler](https://github.com/turmeric-lang/turmeric/blob/main/docs/archive/o0-build-cannot-link-contract-handler.md) | no archive defined the contract-handler pair; `-O2` hid it | test a link at `-O0` when you add an extern the preamble or stdlib names |
| [project-mode-rc-runtime-preamble-missing](https://github.com/turmeric-lang/turmeric/blob/main/docs/archive/history/project-mode-rc-runtime-preamble-missing.md) | project mode had no rc/frame runtime at all | the origin of `tur_runtime.h` and the owner TU |
| "DEDUP-4b, the project half" comment in `cmd_build_multi` | archive posture with no archive on the link line; graduating regions made it fail on every multi-module executable | an emission posture is a promise the link step must keep |
| [fiber-tls-address-reuse](https://github.com/turmeric-lang/turmeric/blob/main/docs/archive/fiber-tls-address-reuse.md) | clang and Windows gcc reused a thread-local's address after a fiber migrated threads | `TUR_TLS_FRESH` for fiber-owned thread-locals |
| [run-sh-stamp-cache-ignores-the-preamble-split-mode](https://github.com/turmeric-lang/turmeric/blob/main/docs/archive/run-sh-stamp-cache-ignores-the-preamble-split-mode.md) | an A/B of the two preamble paths stamp-skipped the second run | measure which path a run took; do not assume it |
| [r7rs-split-units-each-compile-the-runtime](https://github.com/turmeric-lang/turmeric/blob/main/docs/archive/r7rs-split-units-each-compile-the-runtime.md) | every r7rs unit compiled the collector and the DK machine | `emit_split_once_fns` |
| [r7rs-prelude-split-wrong-symbols-on-windows](https://github.com/turmeric-lang/turmeric/blob/main/docs/reported/r7rs-prelude-split-wrong-symbols-on-windows.md) (open) | weak data records resolved to the wrong bytes under GNU ld for PE | no weak data in the r7rs split; `selectany` on PE. The r7rs split stays off on Windows until a Windows run confirms the fix |

---

## Debugging

| Tool | Shows |
|---|---|
| `TUR_SHOW_CC=1 tur build f.tur` | the `cc` line: `-lturt_preamble`, `-lturt_runtime`, bare sources, `-DTUR_RT_SPLIT_HOSTED` |
| `TUR_JIT_TIMING=1` | a `split disengaged <reason>` line on each decline (JIT and cc) |
| `TUR_JIT_SPLIT_DEBUG=1` | `split-debug: probe=<h> len=<n> committed=<h>`. Set it to an absolute path to also dump the canonical probe text there |
| `TUR_JIT_DUMP_C=<path>` | the exact text the JIT hands c2mir (after the swap and pruning) |
| `TUR_JIT_NO_SPLIT=1` | skip the swap (JIT and cc) |
| `TUR_PREAMBLE_SPLIT=0\|1` | the cc default: off, or on outside Linux/Windows |
| `TUR_RUNTIME=auto\|lib\|source\|split`, `--runtime=` | the linkage mode |
| `TUR_RUNTIME_LIB=<file or dir>` | where `locate_runtime_lib` looks first |
| `TUR_RCGC_FROM_ARCHIVE=0\|1` | force the replica, or allow the archive posture for bare `emit-c` (still needs an archive on disk) |
| `tur emit-rt-split [--hash]` | the canonical text, or its hash |
| `tools/gc-copy-diff.py [--diff] [--count]` | the rc/GC replica against `src/runtime` |
| `diff <(tur emit-rt-split) /path/to/dump` | after `TUR_JIT_SPLIT_DEBUG=/path/to/dump`: where your process's text drifted from the canonical text |

To find *why* a hash differs, dump the probe from the failing invocation
(`TUR_JIT_SPLIT_DEBUG=/abs/path`) and diff it against plain
`tur emit-rt-split`. The first differing block names the gate or knob.

---

## Bugs found while writing this guide

Each was reproduced against the compiler this guide describes and filed in
[`docs/reported/`](https://github.com/turmeric-lang/turmeric/blob/main/docs/reported/README.md). When one is fixed and archived,
update the passage above that describes it and drop it from this list.

- [regions-off-source-runtime-link-failure](https://github.com/turmeric-lang/turmeric/blob/main/docs/archive/regions-off-source-runtime-link-failure.md) -- `TUR_REGIONS=0` fails to link when the runtime comes from source

## Maintenance

Update this guide in the same PR when you:

- add or remove a linkage posture, or a runtime archive or its members;
- add a state input to `emit_rt_split_source`'s forced set, or change a
  generator rule;
- add a `tur_tls.c` slot or a `TLS` table entry;
- change what `preamble_split_auto_applies` or `resolve_rcgc_from_archive`
  checks;
- add or retire a CI probe.

The line counts and hashes quoted here are dated measurements. Refresh them
with `tur emit-rt-split | wc -l` and `tur emit-rt-split --hash` when the shape
they illustrate changes. Cite functions, not line numbers.
