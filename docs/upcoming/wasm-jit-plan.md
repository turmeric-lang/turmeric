# Plan: the JIT engine in the browser (c2mir + MIR on wasm32)

> **Status:** P0, P1 and P2 done; P3 done 2026-10-28 except its gate,
> which needs one fork change (below). The engine builds from CMake as a
> second module, `turmeric-jit.{js,wasm}` (9.5 MB, `tur_wasm_jit`), and
> `tests/run-jit-wasm.sh` runs the corpus through it against
> `tests/jit-wasm-baseline.txt`: 1,712 fixtures compiled, 643 fell back to the
> interpreter with the right answer, 574 failed, 236 skipped (section 7, P3).
> **The gate fails on one example**: `closure` traps, because host C cannot
> call a function MIR interprets once its address is stored
> ([wasm-mir-thunk-address-is-not-callable-by-host-code](../reported/wasm-mir-thunk-address-is-not-callable-by-host-code.md)).
> The fix is a turmeric-lang/mir change: thunks as real table entries. It is
> first in section 6's next fork PR. Next: that fork PR, then P4, which
> also moves the stdlib into one package both modules share (5.11).
> **P0 run 2026-10-23** (section 7): the corpus under `TUR_JIT_GEN=interp`
> on a Release build is 3853/113/70; every interpreter-only failure is in
> one of three families (threads 82, continuations 10, deep recursion 2),
> and the MIR interpreter runs a tail-recursive loop ~10x slower than the
> generator and ~20x faster than the tree-walker. **Fork state verified
> 2026-10-23** (section 6.1): the vendored copy is byte-identical to the
> fork's `master`, every wasm branch is merged into it, and c2mp pins a
> commit 23 behind it, so there are no patches to reconcile -- only a pin
> to bump on c2mp's side and a CI leg to add on the fork's. The three gaps
> of section 3 are sized in section 3.1. **P1 pre-spike 2026-10-23**
> (section 7, `tools/wasm-jit-spike/`): MIR runs under emcc 6.0; `setjmp`
> cannot be called indirectly on wasm, and the host-owned landing works;
> depth 1,696; 13 ns per interpreted iteration, ~165 ns per trampolined
> native call. **P1 2026-10-23** (section 7, `tools/wasm-jit-spike/engine/`):
> the engine linked into a copy of the REPL module with a generated
> symbol table runs the inline-C program on the page -- a compiled turn
> costs 288 ms in steady state, c2mir on wasm is no slower than native.
> Four fork bugs found (section 6, fixed in mir#15); `setjmp` (5.6) and
> module size are what P3 starts with.
> **Type:** JIT engine (`src/jit_engine.c`, `external/mir/`), web build
> (`src/CMakeLists.txt` `tur_wasm`, `src/web/wasm_glue.c`), driver split
> (`src/driver/pipeline.c`), emitter (one knob, section 5.6).
> **Why now:** `turmeric-lang/c2mp` (the browser C REPL at
> `c.turmeric-lang.com`) has run c2mir and the MIR interpreter in the browser
> for months, on the **same MIR fork this repo vendors**. The wasm32 target it
> needed is already in `external/mir/` (`mir-wasm.c`, `c2mir/wasm32/`). What
> is missing is the wiring on this side, and this plan is the list of it.
> **Builds on:** [jit-guide](../guides/jit-guide.md),
> [mir-interp-tier-plan](../archive/mir-interp-tier-plan.md) (the
> `TUR_JIT_GEN=interp` vehicle, and a premise this plan reverses for wasm),
> [cc-path-preamble-split-plan](cc-path-preamble-split-plan.md) (the resident
> runtime half), [aot-compiled-repl-plan](aot-compiled-repl-plan.md) (C1's
> per-defn inline-C images), [jit-ffi-c2mir-plan](../archive/jit-ffi-c2mir-plan.md).
> **Sibling:** [raylib-web-playground-plan](raylib-web-playground-plan.md),
> whose "build service" phase this plan would make unnecessary.

## 1. Goal

Run a Turmeric program in the browser **the way `tur jit` runs it**: through
the emitted C, c2mir, and MIR -- so inline-C bodies, `extern-c`, and spices
written as inline-C work on the page, with compiled semantics rather than the
tree-walker's, and with no server.

Concretely, in Try Turmeric:

````turmeric
(defn c-mix [a : int b : int] : int
  ```c
  int64_t t = a;
  return t * 31 + b;
  ```)
(println (c-mix 3 4))
````

prints `97` instead of
`error: eval: inline-C not supported in interpreter mode`.

Non-goals: native-speed execution (section 4 says what speed to expect),
`spawn`/threads inside JIT'd code on the page (section 5.7), and replacing
the tree-walker, which stays the fallback and the LSP's engine.

## 2. What c2mp has already established

Everything in this table is measured in `turmeric-lang/c2mp`'s
`docs/limitations.md` and `docs/wasm-ffi-notes.md` (2026-08-26, fork
`cfb17d85`; later work merged to the fork's master, which
`external/mir/UPSTREAM` pins at `a6e75113`). Read it as "known to work",
not "known to work for us" -- section 5 is the delta.

| Fact | Where it lives now | What it means here |
|---|---|---|
| MIR has a wasm32/Emscripten target: interpreter only, `MIR_gen` absent. | `external/mir/mir-wasm.c` (639 lines), `external/mir/mir-code-alloc-wasm.c`, `external/mir/c2mir/wasm32/` | Already vendored. `cmake/mir.cmake` does not build it and `src/CMakeLists.txt` says "An Emscripten toolchain build gets no JIT (MIR's wasm32 target is not wired up as a host engine)". That sentence is this plan. |
| Calls from interpreted code to native go through a JS trampoline: `wasmTable.get(fp)` plus `apply`, with a signature string, `i64` as `BigInt`. | `mir-wasm.c` `_MIR_get_ff_call`, `mir_wasm_js_call` | Needs `-sWASM_BIGINT` and `wasmTable`,`UTF8ToString` exported. Every runtime call pays a JS round trip (section 5.5). |
| Interpreted-to-interpreted calls work (thunk = table slot, re-entered via `MIR_interp_arr`); native-to-interpreted callbacks work (`addFunction` on demand). | `mir-wasm.c` `_MIR_get_thunk`, `_MIR_get_interp_shim`, `wasm_ff_dispatch` | `qsort` with an interpreted comparator works. Needs `-sALLOW_TABLE_GROWTH` and `addFunction`,`stackAlloc`,`stackSave`,`stackRestore`. |
| Recursion depth: each interpreted call nests a wasm `eval` frame; V8 counts them against the JS stack. **1,122 frames at `-O2`, 194 at `-O0`.** Overflow is a catchable `RangeError`. | c2mp `limitations.md` section 1.1 | The optimisation level of the *interpreter* is a correctness knob. Our `tur_wasm` is `-O2` already. See section 5.8. |
| Throughput: `for (i<1e7) s+=i` in 94 ms, about 7x slower than `clang -O0`. | `limitations.md` section 7 | Section 4 puts this next to the tree-walker. |
| Variadic *calls* to native work (`%lld` correct); variadic functions *defined* in interpreted C do not. Struct-by-value in a variadic tail is rejected. | `limitations.md` section 1.2 | The emitted C defines no variadic functions. Inline-C that does will fail to compile, loudly. |
| ILP32: `long` and pointers are 4 bytes, `long double` is `double`, `sizeof (MIR_val_t)` is 16. | `limitations.md` sections 3, 5; `c2mir/wasm32/cwasm32.h` | The emitter already targets ILP32 under `tur build --target wasm` (`g_emit_for_wasm`). Carrier words stay `int64_t`. |
| No `dlsym` under a plain Emscripten build. c2mp hands `MIR_link` a table generated from one X-macro list, which also generates the headers it serves from MEMFS, so declared and linkable never drift. | c2mp `src/libc_wasm32.def`, `docs/wasm-libc-plan.md` | Our `jit_import_resolver` is `JIT_SHIMS[]` then `dlsym(RTLD_DEFAULT)`. The second half needs a table (section 5.3). |
| c2mir reads headers out of MEMFS through `ops.include_dirs`; a compiled-in `standard_includes` entry shadows a MEMFS file of the same name. | `wasm-libc-plan.md` section 1 | Section 5.4. |
| A persistent MIR context across REPL turns: each turn compiles as a small module against a declarations preamble, links into the same context. 0.5 to 0.8 ms per `1 + 1`. | `limitations.md` section 8 | The same shape as C1's resident images (aot-compiled-repl-plan). Section 5.9. |
| The interpreter runs in a Web Worker; Stop terminates the worker and replays the accepted turns into a fresh one. | c2mp `vite-wasm/public/worker.js` | Try Turmeric's eval worker already does the first half (`stopEvalWorker`). |
| Module size: 767 KB `.wasm` for c2mir + MIR interpreter + c2mp. | `vite-wasm/public/` | The engine is small; our module's size is libturi. |
| `exit`/`abort`/failed `assert` unwind the evaluation rather than ending the host. | `limitations.md` section 4 | Our `jit_mir_error` already `longjmp`s to a landing pad; `exit` from a program needs the same treatment on the page. |

## 3. What this repo already has, and the three gaps

| Have | Where |
|---|---|
| The interpreter mode as a production-path switch: `TUR_JIT_GEN=interp` selects `MIR_set_interp_interface` and skips `MIR_gen_init`. | `src/jit_engine.c` (`g_jit_interp_mode`) |
| The runtime half of every program resident in the host, with a 2,126-line declarations-only region (582 prototypes) for the program half to compile against. **`tur_rt_split.c` and `tur_rt_split_embed.c` are in `tur_core`, so `libturi_wasm` already carries the runtime.** | `src/runtime/generated/`, `src/driver/preamble_split.c`, `src/CMakeLists.txt:325-338` |
| A pruner that drops unreferenced prelude functions and the heavy system includes a split TU does not use. | `src/compiler/jit_prune.h` |
| `main` found by walking MIR's item list, not `dlsym`. | `jit_find_func` |
| 79 host shims for `__builtin_*` and pthread entry points, consulted before `dlsym`. | `JIT_SHIMS[]` |
| An embedder fallback: a host with no `cc` falls back to `turi_eval`; `tests/turi/jit-embed.c` asserts parity. | mir-interp-tier-plan section 7.3 |
| Per-defn inline-C images for the REPL (C1), and the image API that keeps them resident. | `tur_jit_compile_image`, `repl_inline_c_jit_build` in `src/driver/jit.c` |
| An Emscripten build of the interpreter with pthreads, 16 MB stack, `ALLOW_TABLE_GROWTH`, and the stdlib in MEMFS. | `tur_wasm` target |

The gaps:

1. **The front half is in the executable, not the library.** `compile_to_c`
   is `src/driver/pipeline.c`, listed under `TUR_DRIVER_SOURCES` with the
   comment "these belong to the executable, NOT tur_core: they call
   `system()`, `fork()`, the cc toolchain and the JIT engine". `libturi_wasm`
   cannot emit C today. Section 5.2.
2. **The engine is excluded from the wasm build by design.** I4 put the
   engine in `tur_jit_obj` precisely so that it "would not force MIR into
   the WASM build, where it has no business being". It has business there
   now. Section 5.1.
3. **Everything that assumes a process:** `dlsym`, `dlopen` autolink, a
   fresh pthread for `main` with a 1 GB stack, `setjmp` landings in the
   program half, system headers on disk. Sections 5.3 to 5.8.

### 3.1 What research each gap needed, and what it found (2026-10-23)

**Gap 1 is a file move, not a design problem.** `compile_to_c`
(`src/driver/pipeline.c:549`) makes no process call: its only environment
reads are `getenv` knobs. The `cc` invocations that keep `pipeline.c` and
`preamble_split.c` out of `tur_core` belong to the r7rs library-unit and
prelude caches (`prelude_compile_pieces`, `prelude_split_object`, through
`cc_system`), which the wasm front half declines anyway (the runtime is
host-resident). The post-passes `cmd_jit` runs are pure too:
`hoist_tur_include_directives` and `scan_autolink_markers` (`cc.c:41`,
`:372`; the latter only validates strings against shell syntax),
`jit_try_split_preamble` (`preamble_split.c:839`), and the pruner, which is
already in `tur_core` (`src/compiler/jit_prune.c`). So `tur_front` is those
functions, cut from the files that also hold `cc_system` callers. No
measurement needed; `tests/check-driver-headers.py` is the check.

**Gap 2 is a third platform arm, same shape as the two that exist.**
`jit_engine.c` already branches on `_WIN32` and `__APPLE__` in about
thirty places. Its process-shaped surface is 45 lines: `dlsym` in the
resolver (`:607`) and `jit_ffi.c:414`, four `dlopen`s in autolink
(`:687-703`), `sigaction`/`sigaltstack` for the entry fault handler
(`:753-829`, already `#ifndef _WIN32`), and the entry pthread with its
stack-size knobs (`:1216-1236`). `jit_run_entry` (`:835`) is three lines
of work (call `main`, drain `atexit`, flush), so the Emscripten arm calls
it inline. `getenv` works under Emscripten. Nothing to research; P1 is the
compile.

**Gap 3 splits into four, three of which P0 measured natively.**

- *Symbols.* The decls region holds 523 prototypes in roughly 100
  distinct signatures by value class (a rough parse; `v(p)`, `p()`, `v()`,
  `p(p,p)`, `j(j)`, `j(p)` cover half), one variadic. The pruned TU of a
  trivial program references 8 libc names (`malloc`, `free`, `printf`,
  `setjmp`, `longjmp`, `exit`, `atexit`, `abort`). The table of 5.3 is
  small; the stub set of 5.5 is on the order of 100, not `4^n`. Confirmed
  by reading, no spike needed.
- *Headers.* After pruning, the TU keeps `stdint stdbool stdio setjmp
  pthread stdlib stddef string stdarg fcntl time unistd errno ucontext`
  (plus Windows-only ones under `#ifdef`); `regex`, the socket headers and
  `hamt.h` are the prunable set (`jit_prune.c` `k_heavy`). Fourteen musl
  headers to serve from MEMFS, or to generate. Decision deferred to P1.
- *`setjmp`.* **Measured, real.** Every program's entry does
  `TUR_SETJMP(__dke.jb)` in the program half (`emit_cps_ir.c:15380`), and
  the continuation fixtures crash under the native interpreter in
  `__longjmp` with a garbage frame -- the dead-thunk-frame failure 5.6
  predicts. Ten fixtures. `TUR_SETJMP` is defined in one place per platform
  (`emit_dk_runtime.c:620-647`: `tur_sjlj_set`, `setjmp`,
  `__builtin_setjmp`), which is where an interpreter-engine lowering plugs
  in. The entry landing is hit only on an escape, which is why the trivial
  programs pass.
- *Threads.* **Measured: the dominant family.** 82 of the 94
  interpreter-only failures are threaded or socket programs (`httpd-*`,
  `session-*`, `r7rs-threads-*`, `stm-stress`, `fiber-scheduler-*`,
  `g4b-thread-local-global`) dying in `malloc(): corrupted top size`,
  `double free`, or `tur: jit: undeclared reg N` -- the interpreter's
  per-context state raced from several threads. On wasm `spawn` is excluded
  by 5.7 regardless; natively this says `TUR_JIT_GEN=interp` must never be
  offered for threaded programs either.
- *Depth.* **Measured: 2 fixtures** (`tailcall-musttail-deep`,
  `r7rs-deep-recursion-million`) overflow the interpreter's stack natively.
  The browser limit (5.8) is far lower, so the wasm count will be higher;
  P1 measures it.
- *Speed against the tree-walker.* **Measured** (section 4): on a
  10-million-step tail-recursive loop the MIR interpreter runs in 121 ms
  of run time against 11.7 ms for the generator and ~2.5 s for the
  tree-walker. c2mir's compile is 75-80 ms either way.

The three questions that needed Emscripten were answered the same day
with the spike in `tools/wasm-jit-spike/` (section 7.1): LLVM refuses any
indirect use of `setjmp` outright, so the interpreter's own
compare-and-call path can never reach it on wasm and the landing must be
host-owned (which works); recursion depth is 1,696 frames; a call through
the JS trampoline costs about 165 ns over an interpreted iteration.

## 4. What "JIT in wasm" can mean, and what to expect

Three tiers. This plan builds the first and sketches the second.

**T1: the MIR interpreter (c2mp's path).** c2mir compiles the emitted C to
MIR IR in the browser; MIR interprets it. No code generation. This is what
exists and is the whole of sections 5 to 7.

**T2: a MIR-to-wasm code generator.** A real JIT: emit a wasm binary per
module from MIR IR, `WebAssembly.instantiate` it against the main module's
memory and table, and install each function in a table slot. Not a port of
`mir-gen`: wasm has unlimited locals (no register allocation), typed
structured control flow (no `goto`), and no runtime patching. The honest
shape is a new `mir-gen-wasm.c` whose hard part is turning MIR's basic
blocks into `block`/`loop`/`br_table`, either properly (a relooper) or with
the dispatch-loop trick (one `loop` around a `br_table` on a block index,
which is simple and still many times faster than interpreting). Section 8
scopes it; it is not v0.

**T3: a C compiler in the browser.** clang+lld compiled to wasm exists and
works; it is a 30+ MB download and is the playground plan's build service
moved client-side. Rejected for the same reason that service was deferred:
cost at the wrong end.

Expected throughput, to set the frame before anyone measures:

| Engine | Tight integer loop | Source |
|---|---|---|
| `tur jit` native, `MIR_gen` | 1x (11.7 ms run) | P0, 2026-10-23, Release, x86-64 |
| `tur jit` native, `TUR_JIT_GEN=interp` | ~10x slower (121 ms run) | P0; agrees with mir-interp-tier-plan 6.3 |
| `tur interpret` (tree-walker), native | ~200x slower than gen, ~20x slower than the MIR interpreter (~2.5 s wall) | P0 |
| MIR interpreter on wasm32 (c2mp) | ~7x slower than `clang -O0` | c2mp limitations 7 |

The loop is `sum-to` over 10 million steps plus a 1-million-step float
accumulation (probe `7.1`); c2mir's compile is 75-80 ms in both engine
modes, and the pruner hands it 118 KB of the 280 KB TU. The claim this plan
rests on is therefore measured natively: a bytecode interpreter over
c2mir's output is an order of magnitude faster than the tree-walker on the
shape a playground program has, and it runs inline C. The wasm numbers will
be worse than native on both sides (P1), but the ratio has no reason to
invert.

One premise of the archived interp-tier plan reverses here and is worth
saying plainly: that plan closed because the MIR interpreter needs the same
executable memory as the generator, so it was "not an escape hatch". On
wasm there is no executable memory to want; thunks are table slots. The
interpreter is the only MIR engine wasm can have, and the 18% cold-code win
that plan measured is a bonus, not the reason.

## 5. Design (T1)

### 5.1 Build shape

As built in P3 (2026-10-28). The design originally added the engine to the
`tur_wasm` link. P1 measured the cost, so it became a second module instead:

- `TUR_WASM_JIT` (default ON under `TUR_WASM`) adds the `tur_wasm_jit`
  target, which builds `build-wasm/wasm-jit/turmeric-jit.{js,wasm}` beside
  the playground's `turmeric.{js,wasm}`. Only a page that asks for the Compile
  engine loads it, so the Interpret page pays nothing. The module contains
  the whole interpreter anyway, as its fallback (5.7).
- The target runs `tools/wasm-jit/build-module.sh` over a source list CMake
  writes. Three groups go through a generated Makefile with `-MMD` and
  per-group response files: `tur_core` plus `tur_front` (5.2) plus the web
  glue; the engine (`jit_engine.c`, `turi/jit_ffi.c`,
  `src/web/wasm_jit_glue.c`); and MIR's `mir.c` and `c2mir/c2mir.c`, with no
  `mir-gen.c`. The script then generates the import table (5.3), collects the
  headers (5.4) and links.
- `mir.c` and `c2mir.c` select the wasm32 target on `__EMSCRIPTEN__` by
  themselves (verified 2026-10-23: `mir.c:6996` and `:731`, `c2mir.c:41`,
  `:369` and `:12749`), so there is no `sed` step. `mir-wasm.c` defines no
  `MIR_gen*`, and Emscripten declares ucontext without defining it, so
  `src/web/wasm_jit_stubs.c` stubs both to abort loudly. That belongs in the
  fork (section 6).
- `jit_engine.c`'s `__EMSCRIPTEN__` arm stays small. It has no `dlopen`
  autolink (a marker is a link error that names the library), runs `main` on
  the calling thread, refuses thread creation (`JIT_WASM_REFUSED`), owns the
  landing for `exit`/`_exit`/`abort` (5.7), and puts the single-thread atomics
  in its prelude (section 7, P3).
- `turi_wasm_jit_run(src, args)` (`src/web/wasm_jit_glue.c`) answers with the
  engine line as JSON:
  `{"engine":"compiled"|"interpreted"|"error","exit":N,"reason":...}`.
- Size, measured: 9.54 MB (2.94 MB gzipped). Of that, 3.67 MB is code and
  5.86 MB is data, and 4.30 MB of the data is the embedded stdlib. The stock
  module embeds the same stdlib, which is what 5.11 is about.

### 5.2 The front half moves into a library

`compile_to_c` and what `cmd_jit` runs after it (`hoist_tur_include_directives`,
`scan_autolink_markers`, `jit_try_split_preamble`, the pruner call) are
pure functions over buffers; the driver file they live in also holds the
`cc` and `fork` paths, which is why it is excluded from `tur_core`.

Split `pipeline.c` and `preamble_split.c` along that line into a `tur_front`
OBJECT library (the in-memory front half: read, elaborate, passes, emit,
hoist, split, prune) that `tur` and `libturi_wasm` both consume, leaving
`cc.c`, `compile_link.c`, `cmd_*.c` and the subprocess parts where they are.
`tests/check-driver-headers.py` already polices which file defines what, so
the move is checkable. This is the one change that touches the native `tur`
and it is a move, not a rewrite.

**Measured 2026-10-28: no file needs splitting.** An `nm` walk over the
native driver objects found that `pipeline.c` and `preamble_split.c` reach
four other driver files directly -- `cc.c` (three flags and
`locate_runtime_lib`; the hoist, quoting and temp-path helpers), `paths.c`
(file reads, the stdlib root, `cc_system`), `spice.c` (spice and
reader-macro discovery) -- and those reach each other and nothing more.
What dragged the whole driver in was two flags defined in `cli.c`,
`g_no_auto_stdlib` and `g_no_auto_spice`: `cli.c` is the command table, so
one `bool` pulled every `cmd_*.c` along. With the flags defined beside their
readers (`pipeline.c`, `spice.c`), the closure is exactly those five files
plus `tur_core`. A program linked from them alone emitted C byte-identical
to `tur emit-c` on 119 sampled fixtures, and all five compile warning-free
under emcc with `tur_wasm`'s flags. So `tur_front` takes `cc.c` whole; its
link and autolink code is dead weight on the page, not a blocker.

Two things the plan did not foresee:

- **`tur_core` depends on its host for `tur_collect_symbols`.** `lsp/lsp.c`
  (in `tur_core`) calls it; `tur` supplies the driver's, the browser module
  `web/wasm_lsp.c`'s cheaper elaboration-only one, and each `tur_core` unit
  test a stub. The driver's lived in `pipeline.c`, which is why P1 had to
  leave `wasm_lsp.c` out. P2 moves it and `tur_check_only` to
  `driver/lsp_backend.c`, which stays in `tur`, so `tur_front` carries no
  LSP entry point and the module keeps its own.
- **The boundary needs a guard, not only the header check.**
  `check-driver-headers.py` checks which file declares what, not what a file
  references, so one new `cli.c` reference would silently re-close the
  front half over the whole driver. `tur_front_link_unit` links `tur_front`
  and `tur_core` alone and compiles two fixtures; a reference across the
  line fails its link with the symbol named (checked by adding one).

`tur_front` still calls `system()` through `cc_system`, for the prelude and
library caches. Emscripten's `system()` returns -1, so P3's glue keeps the
runtime host-resident and declines those paths, as P1's glue already does.

### 5.3 Symbols: a generated table instead of `dlsym`

`jit_import_resolver` keeps `JIT_SHIMS[]` first. Its `dlsym` tail becomes,
under Emscripten, a lookup in `tur_wasm_jit_symtab.c`, generated at build
time from:

- the 582 prototypes of the decls region (`tools/gen-runtime-split.py`
  already parses that text to produce it), and
- a libc/libm subset: what the emitted program half and the stdlib's inline
  C actually reference. c2mp's `libc_wasm32.def` is the model and a starting
  list; a reference the table lacks fails at `MIR_link` with the name, the
  same loud failure c2mp designed for.

Taking each function's address in C forces Emscripten to retain it and
yields the table index `MIR_load_external` wants. The whole table is
registered once per context, before `MIR_link`.

Rejected alternative: `-sMAIN_MODULE=2`, which makes `dlsym` work by
exporting every symbol. It would resolve names with no table to maintain,
at the cost of retaining everything and PIC codegen across the module. Keep
it as the fallback if the generated table proves brittle.

### 5.4 Headers

The decls region `#include`s about twenty system headers (`stdio.h`,
`pthread.h`, `setjmp.h`, `ucontext.h`, `regex.h`, the socket headers, ...).
On the page there is no disk. Two ways to serve them from MEMFS, to be
chosen by measurement in P1:

- **Embed Emscripten's sysroot subset** (`--embed-file` of the headers the
  decls region names, plus what they include). musl's headers are plain C11
  and c2mir parses musl-derived headers today on Linux. Larger, exact.
- **Generate declarations the way c2mp does**, from the same list that
  generates the symbol table, so headers and symbols cannot drift. Smaller,
  and the one c2mp settled on, but it is a second source of truth for types
  (`pthread_t`, `jmp_buf`, `struct sockaddr`) the runtime half also sees.

The pruner's "drop the heavy system includes nothing uses" already shrinks
the problem for most programs.

### 5.5 Foreign calls: exact-signature stubs for the runtime

Under T1 every call from the program half into the runtime half is a foreign
call, and `mir-wasm.c` makes each one a JS round trip with BigInt
conversions. For a tree-walker replacement that is the hot path, not the
edge case, and it has a second cost: a JS frame on the stack is what
Asyncify cannot unwind through, which matters the moment JIT'd code sits
under a fiber (continuations, `#lang r7rs` `call/cc`) or under the
playground's frame loop.

Measured (section 7.1): an interpreted loop iteration costs 13 ns; the same
iteration making one native call through the trampoline costs 180 ns. So a
runtime call is ~13 interpreted instructions' worth of overhead, and a
frame that makes ten thousand of them pays 1.7 ms.

The runtime half has a finite signature set. Generate, from the decls
region, one C stub per distinct signature (`void stub_N (void *fn, MIR_val_t
*ras)` doing an exact `call_indirect`), and have `_MIR_get_ff_call` prefer a
stub when the signature matches, falling back to the JS trampoline
otherwise. c2mp's FFI notes rejected enumerating stubs because the general
space is `4^nargs`; the runtime's actual set is a few dozen, which is a
different problem. This is a fork change (`mir-wasm.c` gains a stub
registry), so it rides one fork PR with anything else P1 finds.

### 5.6 `setjmp` in the program half

The emitter puts `setjmp` landings **in the program half**: effect handler
escapes (`emit_effects.c`, `tur_handler_node { jmp_buf buf; ... }` in the
decls region, `TUR_SETJMP`), `call/cc` escape landings in the CPS backend
(`emit_cps_ir.c`, `emit_callcc`), and the DK runtime's landing pad. An
interpreted function cannot call `setjmp` through a foreign-call stub: the
stub's frame is what gets recorded, and it is gone when the `longjmp`
arrives. This is true under `TUR_JIT_GEN=interp` natively too, which is why
P0 runs the corpus there first: it tells us which fixtures this already
breaks, with no Emscripten in the loop.

On wasm this is not merely a dead-frame hazard but a hard refusal:
`emcc` fails the build with `error in backend: Indirect use of setjmp is
not supported` the moment anything takes `setjmp`'s address (section 7.1).
So `setjmp` can never be in the symbol table, the interpreter's
`func_addr == setjmp_addr` path (`mir-interp.c:1540`, a direct call in
`eval`'s frame natively) is unreachable there, and the program half must
not call `setjmp` at all. `longjmp`'s address *can* be taken and called
through the trampoline.

The fix is to give the landing to the host. A `tur_rt_try (body, env)`
runtime function does the `setjmp` in compiled code and calls the body (an
interpreted function, reached through the callback path c2mp proved);
`longjmp` then unwinds interpreter frames, which MIR's interpreter tolerates
as long as nothing it holds across the call lives on its C stack (c2mp hit
exactly one such buffer, `call_res_args`, and moved it). **Verified
2026-10-23** in `tools/wasm-jit-spike/sjlj.c`: a compiled frame does a
direct `setjmp`, calls a function through `wasmTable.get(fp)()`, that
function calls `longjmp` by address through the same trampoline, and
control returns to the landing with the right value, twice in a row, with
the code after the `longjmp` never running. The JS frames in between do
not stop the unwind. The emitter selects
this lowering under a new `g_emit_for_interp` knob the wasm front half sets.
`emit_split.c:896-901` already treats `setjmp` as a frame-owning call the
splitter must not move, so the places to change are enumerable.

v0 may ship before this lands: a program whose TU still calls `setjmp` is
detected at prune time and takes the fallback (section 5.7), loudly.

### 5.7 The fallback contract on the page

`tur jit`'s step-6 fallback is `cc`. The page has no `cc`; its fallback is
`turi_eval`, exactly as the embed test (`tests/turi/jit-embed.c`) does it,
with the same parity assertion in CI. The W0070-shaped line goes to the
console so the user knows which engine ran. The families that always fall
back today (`__atomic_*`, `_Thread_local`) still do; two more join them on
the page: `spawn` (the MIR interpreter is not thread-safe per context, and
an interpreted thread entry would need its own context), and any
`-l<name>` autolink.

### 5.8 Stack depth

An interpreted call nests a wasm frame of the interpreter's `eval`; the
browser's JS stack limit bounds the depth. c2mp measured 1,122 frames at
`-O2`; the spike measured **1,696** on the vendored MIR under node 22's
default stack (section 7.1). Two kinds of overflow, with different
recoveries: the JS stack limit is a catchable `RangeError` that leaves the
wasm linear stack pointer and the interpreter's frame state where they
were, so the page must `stackRestore` and rebuild the context before the
next call (the spike does both; c2mp rebuilds the worker); the linear
stack (`-sSTACK_SIZE`) overflowing instead is a wasm trap after which the
module is not usable. Turmeric programs recurse: the emitter turns self tail calls into
loops (and `TUR_MUSTTAIL` where the compiler honours it, which emcc does
not), but a non-tail list recursion over 10k elements will overflow where
the native JIT's 1 GB stack would not. Two facts sharpen this (2026-10-23). The tree-walker on the page has **no**
such limit: since C4 (`turi-c-scoped-forms-heap-bounding`) every
interpreter recursion folds onto a heap work-stack, and `tur interpret`
runs a million-deep non-tail recursion natively where the MIR interpreter
overflows even on the JIT's 1 GiB entry thread (its frame is over 1 KiB).
So switching the page's engine from Interpret to Compile trades
memory-bounded depth for ~1,700 frames, which the toggle must say. Mutual
tail calls the T5 fusion did not collapse do not count against it: the
emitter's `TUR_MUSTTAIL` is a MIR tail call, which the interpreter runs in the
caller's activation
([mir-interp-tail-calls-grow-the-stack](../archive/mir-interp-tail-calls-grow-the-stack.md),
fixed 2026-10-24). One wasm-specific residue: the interpreter cannot reset the
stack pointer there, so a tail call from a function that allocated (a local
aggregate) leaves those allocas and a frame behind until the chain returns.
The AOT wasm target is in the same family at a higher ceiling:
[wasm-target-stack-is-emcc-default](../archive/wasm-target-stack-is-emcc-default.md).
Between them they are the "real program that needs more than 1 GiB of
stack" that [r7rs-heap-frames-plan](r7rs-heap-frames-plan.md) names as
what lifts its hold.

Three responses, cheapest first:

1. Report it: the worker catches `RangeError` and prints the same
   `stack overflow: recursion too deep` line the native engine prints, then
   rebuilds the worker (c2mp's model; Try's `stopEvalWorker` is most of it).
2. Measure the real frame size of our `-O2` interpreter and whether
   `-O3`/`-Os` on `mir-interp.c` alone buys depth.
3. Longer term, a fork change: heap-allocate the interpreter's frames so
   depth is bounded by linear memory, not the JS stack. Scope it only if 1
   and 2 leave real programs out.

### 5.9 Session model for the REPL

C1 already compiles each inline-C `defn` into a resident image and binds it
as a native; C2 (compile whole turns) is designed and not started. On the
page the same image API runs on the interpreter engine: an image is a MIR
module linked into the shared context, kept for the session, and redefinition
follows the REPL's existing rules. c2mp's persistent-context model (a
declarations preamble per turn, `MIR_set_func_redef_permission` for
re-included inline definitions, replay-on-rebuild as the recovery path) is
the reference for the parts C2 has not specified, and its measured cost
(sub-millisecond per turn) is the target.

### 5.10 Where it plugs in

- **Try Turmeric:** an engine toggle next to `#lang` -- Interpret (today)
  or Compile (this). Compile runs `turi_wasm_jit_run`; the console shows
  which engine produced the output. The LSP worker stays on the interpreter
  module.
- **The raylib playground:** its "Build" button and the build service it
  needed become this engine, client-side. Pure and inline-C spices (`math`,
  `ecs`, `json`'s Turmeric half, ...) need no baked image at all under it:
  c2mir compiles their source like any other inline C. Only cmake deps
  (raylib's `libraylib.a`) stay pre-linked, and their exports join the
  symbol table of 5.3. The playground plan's section 3 is amended to say so.
- **`tur jit` natively:** unchanged. `TUR_JIT_GEN=interp` stays the
  diagnostic it is.

### 5.11 The stdlib as one package both modules share

Looked into 2026-10-28, prototyped on the JIT module; P4 builds it.

**The problem.** Both modules embed `stdlib/` (`--embed-file stdlib@/stdlib`)
in their wasm data segment. It is 4.30 MB of the JIT module's 9.54 MB and of
the stock module's 6.85 MB. A page that loads both downloads it twice, and
it is the same 4.30 MB in each.

**The change.** Package the stdlib once, with Emscripten's own tool, and link
the package's loader into both modules instead of the files:

```sh
python3 $EMSDK/upstream/emscripten/tools/file_packager.py stdlib.data \
    --preload stdlib@/stdlib --js-output=stdlib.data.js
emcc ... --pre-js stdlib.data.js        # in place of --embed-file stdlib@/stdlib
```

`stdlib.data.js` holds the file list and offsets. Under `-sMODULARIZE` a
`--pre-js` runs inside the module factory, so the loader sees the instance's
`Module`. It finds `stdlib.data` through `Module.locateFile`, the same
mechanism that finds the `.wasm`. It also holds a run dependency, so
`turi_wasm_init` does not run until `/stdlib` is mounted. Under node it reads
the file from disk, so `tools/wasm-jit/run.js` and `tests/run-jit-wasm.sh`
need no change.

**Measured** (prototype relink of the P3 objects, emcc 6.0):

| | today | with the package |
|---|---|---|
| `turmeric-jit.wasm` | 9.54 MB (2.94 gz) | 5.23 MB (1.83 gz) |
| `turmeric.wasm` | 6.85 MB (2.06 gz) | about 2.55 MB (about 0.95 gz), by subtraction, not relinked |
| `stdlib.data` | (inside both) | 4.30 MB (1.10 gz), fetched once |

The prototype ran the smoke programs compiled and the `scheme-sweet` example
on the interpreter fallback, with the stdlib arriving from the package.

**What it buys.** On the Interpret page a first visit downloads the same
bytes as before: module plus package. A visitor who then picks Compile
downloads 1.83 MB gzipped instead of 2.94, because the stdlib is already
cached. The playground, `eval-worker.js` and `lsp-worker.js` each
instantiate `turmeric.js`, and all three read the package from the cache.

**What P4 has to get right:**

- **One package, built once.** The file list is baked into each module's JS
  at link time. If two links read different `stdlib.data.js` files, a module
  reads the wrong offsets from a package that does not match it. So
  `stdlib.data{,.js}` is its own CMake target, and both `tur_wasm` and
  `tur_wasm_jit` depend on it and link the same file.
- **Deploy coherence.** `web/public/sw.js` precaches `/turmeric.js` and
  `/turmeric.wasm` under a `CACHE_VERSION` that every build stamps
  (`web/vite.config.js`, `injectSwVersion`). `/stdlib.data` joins
  `PRECACHE_URLS`, and the JIT module's two files join it once the page loads
  the module. A deploy then replaces modules and package together, and a
  returning visitor cannot pair a new module with an old package. The `wasm`
  recipe copies `stdlib.data` into `web/public/` next to `turmeric.wasm`.
- **No new filesystem surface.** The package mounts at the same `/stdlib` the
  embed did, so `_turi_wasm_read_file`'s stdlib-only rule (go-to-definition,
  `wasm_lsp.c`) is unchanged.

**Left out, on purpose:**

- `docstrings.tur` is 1.10 MB of the 4.30. Only `(doc ...)` loads it, and
  the docs panel already has its own pack (`web/public/docs-pack/`).
  Splitting it into a second, lazily fetched package would save another
  quarter. That is a separate decision about how `(doc ...)` finds its table,
  not part of this change.
- The c2mir headers (0.35 MB) stay embedded in the JIT module, the only
  module that reads them.
- Splitting the code itself (`-sMAIN_MODULE`/`SIDE_MODULE`, or `wasm-split`)
  is not worth it. MIR and c2mir are about 1.3 MB of objects, and everything
  else is runtime and interpreter that both modules need. The import table
  (5.3) takes the address of every runtime function, which assumes one link.
  And dynamic linking under `-pthread` is the least reliable corner of
  Emscripten.

## 6. Fork changes (one PR, batched)

The MIR-fork rule in CLAUDE.md allows one open PR at a time, so fixes ride
together. The first batch, everything P1 found, merged on 2026-10-27 as
**turmeric-lang/mir#15** (`a6e75113`, vendored the same day; VENDORED.md has
the entry):

- **`mir-wasm.c` declares one hard register with a `NULL` name**
  (`target_hard_reg_names[] = {NULL}`, `MAX_HARD_REG 0`), and `_MIR_init`
  hashes every hard-register name, so it runs `strlen (NULL)`. Address 0
  reads as an empty string in a plain wasm module, which is why the fork's
  test and c2mp never saw it; under `-sSAFE_HEAP=1` it is a segfault at
  init. Found 2026-10-23. **Fixed:** named `"wasm_hr0"`.
- **The wasm32 target's basic types disagree with clang's.**
  `mirc_wasm32_emscripten.h` predefines `__PTRDIFF_TYPE__ int`, and its
  `stdint.h`/`stddef.h` make `intptr_t`, `uintptr_t` and `ptrdiff_t` `int`;
  clang on wasm32 uses `long` for all four (`emcc -dM`), and musl derives
  `_Addr`, hence `size_t`, from `__PTRDIFF_TYPE__`, so `size_t` arrived as
  two different types ("repeated declaration size_t"). Found 2026-10-23
  in P1. **Fixed:** all four are `long`, and `int_fast16_t`/`uint_fast16_t`,
  which were `int` where clang says `short`, follow clang too.
- **The wasm32 target lacks the compiler-provided type macros musl
  typedefs from**: `__WCHAR_TYPE__`, `__WINT_TYPE__` and the sixteen
  `__INT_FAST*`/`__INT_LEAST*`/`__UINT_*` ones. `bits/alltypes.h`'s
  `typedef __WCHAR_TYPE__ wchar_t;` then fails and every later declaration
  naming `wchar_t` (`mbtowc` in `stdlib.h`) derails the parser, which
  surfaced as a syntax error on an unrelated `static inline` function
  hundreds of lines later. **Fixed:** the target predefines them with
  `emcc -dM`'s values.
- **`__builtin_va_list` was undefined**, which `bits/alltypes.h` typedefs
  `va_list` and `__isoc_va_list` from, so `<stdio.h>` did not parse; P1
  worked around it in `JIT_PRELUDE`. **Fixed:** the target typedefs it as
  `void *[1]` and its `<stdarg.h>`'s `va_list` is that type. The prelude's
  other two musl defines, `__restrict` and `__inline`, were never needed:
  c2mir has both as keywords. The whole `__EMSCRIPTEN__` block is gone.
- **`wasm-tests/run.sh` was stale**: after `12ee70c` dropped the bundled libc
  headers, every case that includes one failed with `error in opening file
  stdio.h`. **Fixed:** it serves c2mir Emscripten's own musl sysroot
  (`-sNODERAWFS`), builds under `-sSAFE_HEAP`, and checks each type above
  through `_Generic`; reverting any one fix turns it red.
- **A wasm CI leg** (6.1): the fork's `test.yml` gains a `wasm` job on emsdk
  6.0.10, the version turmeric's CI pins. Green on its first run.
- Housekeeping: the fork's README no longer lists `-sWASM_BIGINT` as
  required (default in current Emscripten, deprecated in emcc 6). c2mp's
  Justfile dropped it too (c2mp#3).

One side finding: musl's `<math.h>` parses once the types are right (P1
recorded it failing on `__ISREL_DEF`), but c2mir keeps unused `static
inline` functions, so a unit that includes it imports `__fpclassify`,
`__fpclassifyf` and `__fpclassifyl` whether it calls them or not. The 5.3
symbol table needs those three if a pruned unit ever keeps `math.h`.

Still to batch into the next fork PR:

- Exact-signature stub registry in `mir-wasm.c` (5.5).
- **Thunks as real table entries** (found in P3, the largest failure family:
  [wasm-mir-thunk-address-is-not-callable-by-host-code](../reported/wasm-mir-thunk-address-is-not-callable-by-host-code.md)).
  A thunk address is a slot in a memory array, so host C that
  `call_indirect`s it traps with `table index is out of bounds`.
  `wasm_ff_dispatch` substitutes a real entry only for an i32 call argument.
  An address stored in a closure, a `DK` frame or a callback slot, or passed as
  an `int64_t` carrier, reaches host code raw. `_MIR_get_thunk` should reserve
  a table slot, and `_MIR_redirect_thunk` should fill it.
- **`MIR_gen` and ucontext stubs.** `mir-wasm.c` defines no `MIR_gen_init`,
  `MIR_gen_set_optimize_level` or `MIR_gen`, and Emscripten declares
  `getcontext`/`makecontext`/`swapcontext` without defining them. Every
  embedder links its own aborting stubs (`src/web/wasm_jit_stubs.c`), and
  the fork should carry them instead.
- If 5.8's option 3 is taken, the interpreter frame change.
- c2mir applies the integer promotions to a `_Generic` controlling
  expression, on every target: `_Generic ((short) 0, short: 1, int: 2)` is
  2 where C11 says 1. Found writing mir#15's test, which goes through
  pointer types instead. The emitter never writes `_Generic`, so only user
  inline C over a sub-`int` type sees it
  ([c2mir-generic-promotes-controlling-expression](../reported/c2mir-generic-promotes-controlling-expression.md)).

Nothing in this plan edits `external/mir/` by hand; the sync is
`tools/update-mir.sh` after the fork PR merges.

### 6.1 Fork state, verified 2026-10-23

Checked against a clone of `turmeric-lang/mir` and `git ls-remote`. Rows
marked 2026-10-27 were re-checked when mir#15 merged:

| Question | Answer |
|---|---|
| Does `external/mir/` match the fork? | Byte-identical to `master` at `fa8a5d69` (every file compared; `UPSTREAM` agrees). `tools/update-mir.sh` lists `mir-wasm.c` and `c2mir/wasm32` explicitly, so a re-sync keeps the target. **2026-10-27:** re-synced to `a6e75113` (mir#15; #13 and #14 were vendored on 2026-10-24); the sync changed only the five wasm32 files and `UPSTREAM`. |
| Is the wasm work on a branch? | No. `wasm` (`7106386`, the original target with foreign calls) and `fix/wasm-interp-recursion` (`cfb17d8`: interpreted-to-interpreted calls, callbacks via `addFunction`, `%lld`, bundled headers dropped, implicit-declaration warning) are both ancestors of `master`; the second merged as fork PR #2 (`b7e72a9`). A first shallow check reported them 109 commits "ahead" -- that was a `--depth=1` fetch cutting the merge commit's parents, not divergence. |
| What does c2mp pin? | `b7e72a9`, the PR #2 merge, in its `CMakeLists.txt`. That is **23 commits behind** `master` and a strict ancestor of it: this repo's pin is a superset of c2mp's. Its docs still cite `7106386` and `cfb17d85`. **2026-10-27:** `a6e75113`, the same commit this repo vendors (turmeric-lang/c2mp#3). |
| Does c2mp patch MIR? | Not any more. `scripts/emcc-setup.sh` (the `sed` patches, the stub `mir-wasm.c`, the types header) is unreferenced by its Justfile and CMake -- only `docs/wasm-ffi-notes.md` mentions it. **2026-10-27:** deleted in c2mp#3. |
| Does the fork's CI run the wasm target? | No. `.github/workflows/test.yml` is the four native legs; `wasm-tests/run.sh` (emcc + node, 4 cases) runs by hand only. **2026-10-27:** yes, the `wasm` job mir#15 added. |
| Has upstream moved? | Yes: `vnmakarov/mir` `master` is at `861c295`, past the fork's base `a8ab7c3`. Not measured how far; the "point `update-mir.sh` back at upstream when it has every fix" note in `VENDORED.md` stays open. |
| Stray branches | Merged and deletable: `__probe` (points at `b7e72a9`), `wasm`, `fix/wasm-interp-recursion`, `claude/aarch64-hfa-abi`, `claude/c2mir-rejects-uint128-hmemc3`, `feat/c2mir-macos-sdk-support`, `fix/llp64-uint-llong-conversion`, `fix/make-one-ret-distinct-targets`, `fix/pragma-pack-macro-arg`, `integration/pragma-pack-and-aarch64-hfa`, `jit/win64-wrapper-offsets`. Unmerged: `claude/ci-baseline-pre-mir10` (one CI-baseline commit). A session cannot delete remote branches (CLAUDE.md), so this is the owner's cleanup. **2026-10-27:** done; `master` is the only branch. |

So "reconcile the turmeric patches and the c2mp patches" reduced to three
small actions, none of them a merge:

1. **c2mp: done** in turmeric-lang/c2mp#3, merged 2026-10-27. Its
   `GIT_TAG` is `a6e75113` (fetched from `turmeric-lang/mir`),
   `scripts/emcc-setup.sh` is deleted, the stale doc citations are fixed, and
   its Justfile no longer passes `-sWASM_BIGINT`. Its native REPL suite and
   JS suite pass unchanged; `__attribute__ ((musttail))`, a syntax error on
   the old pin, now runs.
2. **Fork: done** in mir#15.
3. **This repo: done.** `external/mir` is at `a6e75113`, and the spike's
   scratch patches and the `JIT_PRELUDE` workaround are gone.

## 7. Phases

### P0 -- the corpus under the interpreter, natively: DONE 2026-10-23

`TUR=./build-rel/tur TUR_JIT_GEN=interp bash tests/run-jit.sh` on a Release
`-DTUR_JIT=ON` build, x86-64 Linux, 4 cores, 12-minute cap (it took about
9 minutes).

| | passed | failed | skipped | via cc fallback |
|---|---|---|---|---|
| interpreter | 3853 | 113 | 70 | 18 |
| generator, the 113 re-run | 94 | 19 | -- | -- |

The 19 that fail under both are the contract and refinement fixtures a
Release binary strips (`contract-*`, `refine-*`, `loop-invariant-*`,
`saffron-*refinement*`; jit-guide: "a fully green run needs a Debug
binary"). They are not engine failures. The 94 interpreter-only failures,
each re-run by hand to read its exit and stderr:

| family | count | how they die | wasm consequence |
|---|---|---|---|
| threads and sockets (`httpd-*` 43, `session-*` 23, `r7rs-threads-*` 15, `stm-stress`, `fiber-scheduler-mt-migration`, `threads-effects-tail-resume`, `g4b-thread-local-global`, `defstruct-field-session-project`, `r7rs-srfi-18`) | 82 | SIGSEGV (55), `malloc(): corrupted top size` / `double free` / `free(): invalid pointer` (16), `tur: jit: undeclared reg N of func ...` (2), silent empty output (13), 20 s timeout (8) | Already excluded by 5.7 (`spawn` falls back). Natively: the interpreter's context is not thread-safe, so `TUR_JIT_GEN=interp` is wrong for any threaded program. |
| continuations (`r7rs-callcc-*` 4, `r7rs-continuations`, `r7rs-eval-callcc-reentry`, `r7rs-toplevel-reentry`, `r7rs-gc-basic`, `r7rs-sicp-extras`, `r7rs-srfi-216`, `docs-sicp-guide-examples`) | 10 | SIGSEGV in `__longjmp` (gdb: `#0 __longjmp ... #1 0x3c0c5ef042f74a3e in ?? ()`), i.e. a `longjmp` into a frame that no longer exists | 5.6 is confirmed and is the one emitter/engine change the plan needs. |
| deep recursion (`tailcall-musttail-deep`, `r7rs-deep-recursion-million`) | 2 | `stack overflow: recursion too deep` | 5.8; the browser's limit is lower, so expect more. |

Nothing fell outside those three families. Everything else the corpus
holds -- effects, handlers, `defer`, regions, GC, the stdlib, inline C,
`#lang saffron`, the r7rs programs that do not capture a continuation --
runs under the interpreter with output identical to the generator.

### P1 pre-spike -- MIR alone under Emscripten: DONE 2026-10-23

`tools/wasm-jit-spike/run.sh`: the vendored MIR (`mir.c` + `c2mir.c`, the
scratch patch of section 6) built with emcc 6.0.13, a 60-line driver that
compiles C through c2mir with headers served from an embedded directory and
interprets it, driven from node 22. `spike.wasm` is 675 KB. Results:

| question | answer |
|---|---|
| Does the vendored wasm target build and run without the patch step c2mp used to need? | Yes (one latent bug, section 6). |
| Can `setjmp` be in the symbol table? | **No.** `emcc` refuses the build: `Indirect use of setjmp is not supported`. `longjmp` can. |
| Does a host-owned landing work across the JS trampoline? | **Yes** (`sjlj.c`): direct `setjmp` in a compiled frame, body entered through `wasmTable.get(fp)()`, `longjmp` by address through the trampoline, control returns to the landing with the value; repeatable. |
| Recursion depth | **1,696** frames (`int sum (int n)`, `-O2`, node default stack). A JS `RangeError`; recovery is `stackRestore` plus a context rebuild. |
| Interpreted loop, steady state | **13 ns per iteration** (`s += i`, 10 M iterations in 130 ms). The first run of a loop is ~3x slower while V8 tiers the interpreter up. Native `TUR_JIT_GEN=interp` ran the Turmeric loop at 12 ns per step, so the interpreter loses almost nothing to wasm. |
| One native call per iteration through the JS trampoline | **180 ns per iteration**: the trampoline costs ~165 ns per call. |
| An interpreted-to-interpreted call (`loop_tail`) | ~140 ns per call. |

What it did not do: link anything from `tur`. That is P1 proper, below,
and it is now a smaller step than this plan first wrote.

### P1 -- the engine in a copy of the `tur_wasm` link: DONE 2026-10-23

`tools/wasm-jit-spike/engine/build.sh` links `jit_engine.c`, `jit_ffi.c`,
MIR, the driver's front half and a generated symbol table into a copy of
the stock `tur_wasm` emcc command; `run.js` feeds it a `.tur` file under
node. The section 1 program prints `97`:

```
$ node run.js tests/cmix.tur
tur-wasm-jit: split=1 pruned=1 bytes=422261 -> 118334 (837 of 893 defs, 6 includes dropped)
TUR_JIT_TIMING  interp  c2mir  150.9
TUR_JIT_TIMING  interp  link   2.6
[out] 97
TUR_JIT_TIMING  interp  run    1.9
```

`hello` and the 10-million-step `loop` run too. Measured, node 22, emcc
6.0.13, the same loop three times in one process:

| | first run | steady state | native Release, `TUR_JIT_GEN=interp` |
|---|---|---|---|
| front half (emit + split + prune) | ~260 ms | ~60 ms | -- |
| c2mir | 164 ms | **63 ms** | 75-80 ms |
| link | 2.6 ms | 2.6 ms | 0.6 ms |
| run, `loop` | 526 ms | **170 ms** | 121 ms |
| whole turn | 889 ms | **288 ms** | -- |

So in steady state a compiled turn on the page costs under 300 ms, c2mir
on wasm is no slower than native, and the interpreted loop is 1.4x native.
The S2 split applies unchanged (the hash of the re-emitted runtime
matches the native artifacts), so the program half is 118 KB of C against
a 422 KB full unit, and the retry path was never needed for these three.

**What it took**, each a line in section 6 or a design decision above:

- **Engine arms** (`src/jit_engine.c`, `src/turi/jit_ffi.c`, all under
  `__EMSCRIPTEN__`): the resolver and `resolve_sym` consult the generated
  table; interpreter mode is forced; `main` is entered through
  `MIR_interp_arr`, because on wasm an interpreted function's `addr` is a
  thunk slot only `wasm_ff_dispatch` can call, so `box.fn (...)` traps with
  `table index is out of bounds`; and a `TUR_JIT_DUMP_FULL` dump of the
  exact text c2mir receives. P1 also put five lines of musl compatibility
  in `JIT_PRELUDE`; mir#15 made them redundant and they are gone (section
  6). The native build is untouched and still runs.
- **The glue** (`engine/wasm_jit_glue.c`): `cmd_jit`'s sequence minus the
  command line, with `g_interpret_mode` saved and cleared around the
  emitter exactly as `repl_jit_build` does -- the module's `turi_env_new`
  leaves it on, and under it the CPS backend evicts every handler ("this
  effect operation has no lowering here"), which was the first thing the
  effect program hit. Generator stubs for `MIR_gen_init`,
  `MIR_gen_set_optimize_level`, `MIR_gen`, which `mir-wasm.c` does not
  provide.
- **The symbol table** (`engine/gen-symtab.py`): every identifier in the
  declarations region that `llvm-nm` finds defined in the linked objects
  (463 after exclusions), minus the names the hosted arm turns into
  macros (asked of the preprocessor), minus the names no declaration
  covers (the compiler names them, the build drops them and retries),
  plus 187 libc names. Data objects matter: `g_tur_args` was the first
  missing import. The table TU reads the region as the hosted externs
  (`TUR_RT_SPLIT_HOSTED`), so every address is taken at its declared type.
- **Headers** (5.4, decided): musl's sysroot headers, embedded at
  `/sysinclude`. P1 needed five prelude defines and scratch patches to
  parse them; since mir#15 the vendored target parses them as they are.
  Fourteen headers parse. P1 recorded `math.h` failing (`__ISREL_DEF` over
  `__builtin_isless`); with the type fixes it parses, at the cost of three
  `__fpclassify*` imports (section 6). The pruned unit never includes it.
- **Three more fork fixes** (section 6), merged with the first as mir#15.
  Re-checked on 2026-10-27 against the vendored `a6e75113` with every patch
  and the prelude block removed: `cmix` prints 97, `hello` and `loop` run,
  and `eff` stops where it did (below).
- **Left out of the link:** `web/wasm_lsp.c` (its own `tur_collect_symbols`
  collides with the driver's now that `pipeline.c` is linked),
  `driver/cmd_run.c`, `cmd_misc.c` and `worker.c` (they include
  `<signal.h>` after `turi/env.h`, whose Emscripten `ucontext_t` stub
  conflicts with musl's), and `mir-gen.c`. `getcontext`/`makecontext`/
  `swapcontext` are stubbed to abort loudly in their own TU: the symbol
  table keeps the resident runtime's fiber code alive, and Emscripten
  declares ucontext without implementing it.

**What P1 leaves for P3**, measured:

- **`setjmp`** (5.6): `eff.tur` stops at `import of undefined item setjmp`,
  the predicted family, with the host-owned landing still to build. Note
  that `hello`, `cmix` and `loop` link without it: the pruner drops the
  entry landing from a program whose `main` cannot escape.
- **Size**: `turmeric-jit.wasm` is 30 MB against the stock module's 6.8 MB.
  The table takes the address of the whole runtime (nothing is dead to
  the linker any more), the driver is in, and nothing is `-Os`. Measuring
  what each costs is P3's first job; the plan's "only the iframe loads
  the big module" split (section 9.3) is the fallback.
- **The full-unit retry** (TUR-W0071) fails on `#include <stdatomic.h>`
  in the embedded `arena.c`: c2mir ships no such header and clang's uses
  `_Atomic`. Natively the same include resolves to the host's. Either
  the embedded runtime sources get a `__MIRC__` arm around it, or the
  retry stays unavailable on wasm and a split-TU failure is final.
- **Bare-module scale**: the loop's first run is 3x its steady state
  while V8 tiers the interpreter up, as in the bare spike.

### P2 -- `tur_front`: DONE 2026-10-28

The driver split of 5.2, landed on its own: `tur` links the same objects
and `tests/check-driver-headers.py` passes. No wasm in this PR.

Scoped 2026-10-28 (5.2): a `tur_front` OBJECT library of `pipeline.c`,
`preamble_split.c`, `cc.c`, `paths.c` and `spice.c` as they are; the two
`cli.c` flags defined beside their readers; `tur_collect_symbols` and
`tur_check_only` moved to `driver/lsp_backend.c`; and
`tur_front_link_unit` guarding the boundary.

### P3 -- the engine in the wasm build: DONE 2026-10-28, gate open

5.1, 5.3, 5.4 and `turi_wasm_jit_run`, as built (5.1). `tests/run-jit-wasm.sh`
runs every fixture through the module under node and ratchets against
`tests/jit-wasm-baseline.txt`. Each fixture's outcome is `compiled`,
`interpreted` (the engine refused it and the interpreter gave the right
answer) or `fail`. A worse outcome than the baseline fails the run, and a
better one is reported so its line can go.

**The corpus, 2026-10-28** (emcc 6.0.13, node 22, three resumable shards):

| Outcome | Fixtures |
|---|---|
| compiled | 1,712 |
| interpreted, right answer | 643 |
| failed | 574 |
| skipped (markers, `flags` files, multi-file fixtures) | 236 |

What the fallbacks and failures are, by reason (a fixture counts once):

| Family | Fixtures | Owner |
|---|---|---|
| `setjmp` not importable (the effect and panic landings) | 642 fell back right; 98 fell back and the interpreter got it wrong too | P5 (5.6) |
| `table index is out of bounds`: an interpreted function's address called by host C | 202 | next fork PR ([report](../reported/wasm-mir-thunk-address-is-not-callable-by-host-code.md)) |
| Asyncify: `emscripten_fiber_swap` in a module built without it | 106 | P5 (5.5) |
| The program starts threads (refused by design) | 60 | stays a fallback (risk 7) |
| Wrong answer on the compiled path: 64-bit carriers in pointer-width slots | 44 | [report](../reported/ilp32-word-slots-truncate-64-bit-carriers.md); `tur build --target wasm` has it too |
| Other traps: `exit` reaching the JS caller (17), `Aborted()` (13), null function (12), BigInt conversion (5), out-of-bounds memory (4), JS stack (2) | 53 | unsorted; the `exit` ones mean some path to `exit` misses 5.7's landing |
| Imports the module has no host for (`fork`, `socket`, `popen`, `symlink`, `getgid`, `fnmatch`) | 12 | stay fallbacks |

The in-module interpreter got the 98 "got it wrong too" fixtures wrong as
well. P3 did not sort them. P0's native families (threads, continuations,
deep recursion) are the likely bulk, but that is unchecked.

**Fixed while running it:** c2mir rejected 208 fixtures because musl has no
`stdatomic.h` (it is the compiler's header) and c2mir lacks the GCC
`__atomic_*`/`__sync_*` builtins and `__thread`. On this engine, and only
here, they are plain operations: it refuses every way a program starts a
thread, so the program runs on one thread, where they are sound. The C11
spelling is `tools/wasm-jit/stdatomic.h`, and the builtins are an
`__EMSCRIPTEN__` block of `JIT_PRELUDE`. c2mir reserves `typeof` without
parsing it, so a macro that returns the old value holds it in a `long long`.
Also fixed: the interpreter fallback now picks the program's `#lang` before
loading it (`turi_wasm_run_program`), as the page does. Before that, a saffron
or r7rs program fell back into a Turmeric session and failed. Rerunning the
217 `#lang` fixtures in the baseline moved 105 out of `fail`: 101 now fall
back right and 4 compile.

**The gate** (`bash tools/wasm-jit/check-examples.sh`: every Try Turmeric
example compiles or falls back, and none is wrong). 17 of the 18 examples
pass: 8 compiled, 9 fell back on `setjmp`. **`closure` traps**: its
`TUR_CLOSURE_DROP` calls the resident `tur_closure_drop`, which calls the
closure's drop function through a pointer the program stored, and that
pointer is a thunk address. This is the 202-fixture family above. Nothing in
this repo can fix it, because the thunk layout is `mir-wasm.c`'s. The gate
closes with the fork PR, and P4 must not offer the Compile engine before then.

**What P3 leaves:**

- For the fork PR (section 6): thunks as table entries, the `MIR_gen` stubs,
  and the `_Generic` fix. Then re-run the harness: the 202 should move to
  `compiled` or into another family.
- For P5: `setjmp` (740) and Asyncify (106) are 846 fixtures between them,
  by far the largest share of what is left.
- For P4: 5.11, and the page's handling of a trap (5.8). After a trap the
  module instance is not reusable, so the page reloads it.
- The full-unit retry (TUR-W0071) is still not wired on wasm. The glue takes
  the split TU or falls back. `stdatomic.h` was what made the retry fail in
  P1, and it now resolves, so wiring the retry is possible if a family needs
  it.
- Latency, measured on the smoke programs: a compiled run spends 125-180 ms
  in the front half and 110-170 ms in c2mir before `main` starts (the engine
  line's `front_ms` and `compile_ms`). The page shows these numbers. Cutting
  them for short programs is P6's business (C1's images), not P3's.

### P4 -- the page

The engine toggle, the console's engine line, `RangeError` as a stack
overflow message, Stop still working (worker teardown), a fresh module
instance after a trap (5.8), and the shared stdlib package (5.11). Playwright
covers one inline-C example end to end. Waits on the fork PR that closes P3's
gate.

### P5 -- stubs and `setjmp`

5.5 (fork PR, then sync) and 5.6 (emitter knob). Re-run P3's harness; the
baseline file should shrink, and that shrink is the test.

### P6 -- REPL session

5.9: C1's images on the page, then C2 as its plan specifies.

## 8. T2, scoped but not scheduled

What a MIR-to-wasm generator would need, so the cost is on record:

- A wasm binary writer (the format is small; types, functions, code, a
  names section for stack traces).
- Instruction selection from MIR insns to wasm ops: nearly one-to-one for
  arithmetic, compares, loads and stores; MIR registers become locals;
  `BLK` arguments become memory copies, as `mir-gen` does.
- Control flow: the dispatch-loop lowering first (every block a case of one
  `br_table` inside one `loop`), a relooper only if profiling says so.
- Linking: instantiate each module with the main module's `memory` and
  `table` imported; every MIR import resolves to a table index from the 5.3
  table; every function gets a table slot so interpreted or native code can
  call it; `_MIR_redirect_thunk` becomes a table write.
- Calls out of generated code are exact `call_indirect`s, so the JS
  trampoline and 5.5's stubs are not on the hot path at all.

It is a backend, and a few thousand lines in the fork. The payoff is the
difference between "runs inline C in the browser" and "runs it near native
speed", and the decision to build it should follow P3's numbers, not
precede them.

## 9. Risks and open questions

1. **Speed against the tree-walker.** Measured natively (section 4): the
   MIR interpreter is ~20x faster on a loop. P1 confirms the ratio on wasm.
2. **The JS trampoline tax** (5.5): measured at ~165 ns per call, against
   13 ns per interpreted iteration. Real but bounded; the stub registry
   removes it for the runtime's signatures. Its interaction with Asyncify
   and fibers stands: a JIT'd frame under a `call/cc` capture, or under the
   playground's frame loop, has JS frames below it, and until 5.5 programs
   that need to suspend across JIT'd code fall back.
3. **`setjmp`** (5.6) is the largest correctness item and the one P0 sizes.
4. **Depth** (5.8).
5. **Two sources of truth for headers** if 5.4 picks generation.
6. **Emscripten pinning.** c2mp pins by Justfile; our CI's "Set up
   Emscripten" step should pin the same major so the trampoline ABI
   (`wasmTable`, `addFunction`) does not move under both repos. Our CI pins
   6.0.10, and since mir#15 the fork's `wasm` job pins the same, so a bump
   is a diff in both rather than a red run in one.
7. **Threads.** The `tur_wasm` module is `-pthread`; the engine runs on the
   eval worker only. `spawn` in JIT'd code falls back.

## 10. Relationship to existing plans

- **raylib-web-playground-plan.** Its architecture B (a build service) is
  what this plan replaces; architecture A (baked interpreter images) is
  still the right first step for raylib itself, since raylib is a cmake
  dep either way. Amended there.
- **mir-interp-tier-plan (archived).** Its vehicle is this plan's P0. Its
  closing premise does not apply on wasm (section 4).
- **aot-compiled-repl-plan.** C2 and C3 get a second host; nothing in them
  changes.
- **wasm-spices-plan.** W1's `*args*` hook is shared; `tur build --target
  wasm` (AOT, emcc) and this (JIT, c2mir) are the two ways a program reaches
  the browser, and the web-emscripten tutorial should say which is which.
- **extras-repo-split / c2mp.** c2mp stays its own product. What this plan
  borrows from it is design and measurements, not code; the one shared
  artifact is the fork, which already serves both.
