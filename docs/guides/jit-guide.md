---
title: JIT Execution Engine Guide (`tur jit`)
category: Contributor Notes
description: How the in-process MIR JIT is built -- what MIR is, how the engine is wired into this project, and what it does differently from the cc path (the fallback contract, the permanent constraints, and the inline-C rules that only bite under the JIT)
---

# JIT Execution Engine Guide (`tur jit`)

`tur jit <file>` compiles and runs a program **in process**, through MIR's
c2mir C front end, with no subprocess `cc` and no disk artifacts. It is the
fastest way through the run-edit-run loop and it backs the spice REPL's
reload path.

This guide is for a contributor who has not worked on the engine before. It
covers, in order: **what MIR is**, **how the engine is put together in this
repo**, and **what differs from the `cc` path** -- the fallback contract, the
permanent constraints, and the harness rules. For how fast it is and when to
prefer it, see the execution-engine triangle in
[performance-guide.md](performance-guide.md).

One gate, at build time, and ON by default on 64-bit x86-64 and arm64:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug   # TUR_JIT defaults ON here
tur jit hello.tur                              # no run-time flag
```

There is no run-time gate. The `jit` experiment graduated in 0.34.0 and its
compatibility shim was retired in 0.38.0, so a command line or `build.tur` that
still says `--enable=jit` is now a hard `TUR-E0310` -- drop the flag. The build-time gate, `TUR_JIT`, compiles the
MIR sources vendored under `external/mir/` into `tur`; no configure reaches the
network. It defaulted OFF until 2026-10-02, when it still cloned MIR at
configure time. `-DTUR_JIT=OFF` leaves the engine out, and other hosts MIR
supports (ppc64, s390x, riscv64) opt in with `-DTUR_JIT=ON`. A build directory
configured before the default changed keeps its cached OFF until reconfigured.
On a binary built without the engine, `tur jit` says so and exits 2.

`cc` is the default engine; the JIT runs when you invoke `tur jit` directly,
or when engine selection asks for it (`--engine jit`, `TUR_ENGINE=jit`, or
`:engine "jit"` in `build.tur`).

---

## What MIR is

[MIR](https://github.com/vnmakarov/MIR) is a small, MIT-licensed JIT compiler
infrastructure written in plain C by Vladimir Makarov. It is a *library*, not
a tool: you link it into your process, hand it code, and get callable machine
code back. Upstream advertises roughly **100x faster code generation than
`gcc -O2`, producing code that runs at about 91% of `gcc -O2` speed** -- which
is exactly the trade a run-edit-run loop wants.

MIR ships several components. The ones that matter here:

| Component | Upstream file | Role |
|---|---|---|
| Core IR, loader, linker | `mir.c` / `mir.h` | the IR data types, `MIR_load_module`, `MIR_link`, text/binary I/O |
| C11 front end | `c2mir/c2mir.c` | a complete preprocessor + parser + type checker that emits MIR IR |
| Optimizing generator | `mir-gen.c` + `mir-gen-<arch>.c` | machine code from MIR IR |
| Interpreter | `mir-interp.c` | direct execution of MIR IR, no codegen |

**Why MIR and not LLVM, libtcc, or a hand-written back end?** Turmeric already
emits C. c2mir means the entire existing codegen path is reused verbatim and
**no per-architecture instruction selection is written at all**. The engine
choice table and the rejected alternatives (AsmJit, libtcc, libjit, sljit,
LLVM ORC, Cranelift, QBE, copy-and-patch) are in
`docs/archive/jit-engine-plan.md`.

### The IR, briefly

You will not write MIR IR by hand in this project -- c2mir is our only IR
producer -- but you will read it in stack traces and in `mir.h`, so the shape
is worth knowing.

A `MIR_context_t` owns everything. A context holds **modules**; a module holds
a list of **items**, which are one of `func`, `proto`, `import`, `export`,
`forward`, `data`, `ref_data`, `bss`, and a few relatives. Every item has an
`addr` field -- after linking, that is the thing you call or read. A **func**
item carries a name, result and argument types, a list of typed **virtual
registers** (unlimited, function-scoped; a register holds integers *or* floats
*or* doubles, never a mix), an instruction list, and a `machine_code` pointer
that is `NULL` until the generator has run.

Instructions are three-address and RISC-ish -- about 200 opcodes with the
operand type baked into the opcode itself (`ADD` / `ADDS` / `FADD` / `DADD` /
`LDADD`), plus extensions, conversions, comparisons that yield 0/1, plain
branches, fused compare-and-branch, `SWITCH`, `ALLOCA`, a `VA_*` family, and
`CALL`, whose first operand is an explicit **prototype** item. Types stop at
scalars (`I8`..`U64`, `F`, `D`, `LD`, `P` for pointer) plus `BLK`/`RBLK` for
aggregates passed or returned by memory. There is no SSA in the surface IR and
no type system to speak of; register allocation happens inside `MIR_gen`.

The useful mental model: MIR sits deliberately **between C and machine code**
-- typed virtual registers and explicit call signatures, and nothing else.

### What we use, and what we do not

We link exactly three upstream translation units (`cmake/mir.cmake`): `mir.c`,
`mir-gen.c`, and `c2mir/c2mir.c`. Everything we touch is in `src/jit_engine.c`
-- `MIR_init`, `c2mir_init`/`c2mir_compile`/`c2mir_finish`, `MIR_gen_init`,
`MIR_gen_set_optimize_level` (hardcoded to **2**), `MIR_load_module`,
`MIR_link`, `MIR_gen`, `MIR_gen_finish`, `MIR_finish`, plus `_MIR_get_wrapper`
and `_MIR_redirect_thunk` for our own lazy-gen interface.

We never build IR by hand (`MIR_new_module`/`MIR_new_func`/`MIR_new_insn` are
unused), never use binary MIR (`MIR_write`/`MIR_read`) or textual MIR
(`MIR_scan_string`), and never call `MIR_load_external` -- imports resolve
through the `import_resolver` callback we pass to `MIR_link`.

**We do not ship the MIR interpreter.** Despite the name, `tur jit` is not an
interpreter tier: it always generates machine code. `TUR_JIT_GEN=interp` exists
purely as spike instrumentation, and an interpreter tier was evaluated and *not*
adopted -- `MIR_set_interp_interface` still publishes native shims through the
same `MAP_JIT` code allocator, so it carries the identical entitlement and W^X
profile as the generator and is not an escape hatch on locked-down platforms.
The write-up is `docs/archive/mir-interp-tier-plan.md`.

### MIR is a pinned fork, not upstream

`external/mir/` is a copy of `turmeric-lang/mir` (formerly `rjungemann/mir`),
not `vnmakarov/mir`, at the commit `external/mir/UPSTREAM` records: upstream
master plus fixes that have not landed upstream. `external/mir/VENDORED.md` logs every one; the main ones:

Two are **MIR back-end** bugs -- wrong code or memory corruption:

- `make_one_ret` merged multi-value returns through the last `ret`'s operand
  list, aliasing both slots of a two-word struct return across a `goto`
  backedge -- exactly the emitter's self-tail-call loop shape.
- `try_spilled_reg_mem` in the register allocator kept a two-entry array for
  reload operands; `mul v, v, v` (coalesced out of a generic `square`) has the
  same spilled register in three positions, and the third write smashed the
  caller's frame.

Five are **c2mir front-end** gaps, and three of those are *silent* -- c2mir
compiles the input and gets the answer wrong:

- aarch64 `__uint128_t` modeled with alignment 8 where AAPCS64 requires 16,
  skewing `ucontext_t` (and with it `FiberBlock`).
- On Linux aarch64, `__uint128_t` not declared at all, so `<ucontext.h>` --
  every emitted unit -- failed to parse and `tur jit` always fell back to cc;
  and GCC `aligned` attributes dropped (glibc's `<sys/cdefs.h>` erases
  `__attribute__` for a non-GNU compiler), which made glibc's `ucontext_t` 16
  bytes too small once it did parse.
- `#pragma pack` accepted and ignored, laying structs out at natural alignment
  -- which the Apple and Windows SDKs rely on heavily.
- C23 `enum [tag] : type` unparseable, so anything reaching
  `<malloc/malloc.h>` failed.
- A leading GCC attribute in a struct member (`__unused long __padding;` in
  `<dirent.h>`) unparseable, which surfaced far away as "undeclared identifier"
  at every later `DIR *`.

**Never edit the copy.** Fix MIR in the fork, merge it there, then re-sync with
`bash tools/update-mir.sh <commit>`, which copies the file list, rewrites
`UPSTREAM`, and checks the three translation units still compile. To try a MIR
change first, point a build at a local checkout with
`-DTUR_MIR_SOURCE_DIR=<dir>`. Point the script back at `vnmakarov/mir` when
upstream has equivalents of every fix.

(The pin used to be a CMake cache variable that an existing build directory
kept silently, even after `rm -rf _deps`, and that nearly shipped a binary
built from unpatched upstream. A file in the tree has no such cache.)

---

## How the JIT is put together here

### The files

| Path | Role |
|---|---|
| `src/jit_engine.c` (~700 lines) | **the engine** -- c2mir compile, MIR link, MIR gen, run `main` on a sized-stack thread; also the persistent-image API |
| `src/jit_engine.h` | public API and embedding contract (`tur_jit_execute`, `TurJitImage`, `TUR_JIT_ERR_*`). Included **unconditionally**; capability is probed with `#ifdef TUR_HAVE_JIT` |
| `src/main.c` | the driver: `cmd_jit`, `jit_try_split_preamble`, `jit_sdk_include_dirs`, `cmd_emit_rt_split`, and the REPL's `repl_jit_build` hook |
| `external/mir/`, `tools/update-mir.sh` | the vendored MIR sources (`UPSTREAM` records the fork commit, `VENDORED.md` the fixes) and the script that re-syncs them |
| `cmake/mir.cmake` | defines the `tur_mir` static library from `external/mir/` (or `TUR_MIR_SOURCE_DIR`) |
| `src/CMakeLists.txt` | the `tur_jit_obj` object library and how it is wired onto `tur` and `libturi` |
| `src/runtime/generated/tur_rt_split*.{c,h}`, `src/runtime/rt_split_embed.h` | the S2 split runtime (see below) |
| `src/runtime/tur_atomics.c`, `src/runtime/tur_tls.c` | host-resident atomics and TLS, because c2mir has neither |
| `tests/run-jit.sh`, `tests/turi/repl-spice-jit.sh`, `tests/turi/jit-embed.c` | the three harnesses |
| `tools/jit-spike/` | the J0 spike harness the engine was ported from; not part of `tur` |

There is no `src/mir/` -- MIR is fetched at configure time.

### The CMake shape

Three things about the build are non-obvious and each was learned the hard way:

- **`tur_mir`** is a static library built from three upstream TUs, always at
  `-O3 -std=gnu11 -fsigned-char -fPIC -w` regardless of the enclosing build
  type, and deliberately **unsanitized**. MIR's own CMake project is *not*
  `add_subdirectory`'d (`SOURCE_SUBDIR` names a nonexistent directory) so we
  fetch sources without importing upstream's `c2m`/`m2b`/`b2m` targets or its
  ctest suite.
- **`tur_jit_obj`** is an OBJECT library, and is deliberately **not** folded
  into `tur_core`. Eighteen targets consume `$<TARGET_OBJECTS:tur_core>`,
  including `libturi_wasm`; putting the engine there breaks the WASM build and
  every unit test with `undefined reference to MIR_set_error_func`.
- **`tur` and `libturi` set `ENABLE_EXPORTS TRUE`** (`-rdynamic`). Symbol
  resolution for JIT'd code is `dlsym(RTLD_DEFAULT, ...)` against this very
  process; without exported symbols nothing resolves.

`TUR_HAVE_JIT=1` is PRIVATE on `tur` (object libraries carry no usage
requirements, so it must be set on the consumer too) and **PUBLIC** on
`libturi`, so an embedder's `#ifdef` probe works.

### What happens when you run `tur jit hello.tur`

1. **Gate.** `cmd_jit` checks `TUR_HAVE_JIT` -- the only gate.
2. **Front half, identical to `tur build`.** `compile_to_c` (reader,
   elaborate, kind/effect/CPS/borrow, emit C into memory) with
   `g_emit_for_link = true`, then `hoist_tur_include_directives`, then
   `scan_autolink_markers`.
3. **S2 preamble split.** `jit_try_split_preamble` re-emits the all-gates
   runtime preamble, hashes it, and compares against the hash baked into
   `tur_rt_split_embed.c`. On a match it swaps roughly 60% of the fixed
   preamble text for a declarations-only region, because that runtime is
   already resident in the host via `tur_rt_split.c`. `TUR_JIT_NO_SPLIT=1`
   opts out.
4. **Prune.** c2mir compiles every definition it is handed, where cc drops an
   unreferenced `static` function, so without this every program would compile
   the whole auto-loaded prelude. `jit_prune_split_source` (split TU) or
   `jit_prune_full_source` (a TU the split declined -- every `#lang r7rs` and
   `#lang saffron` program) drops the program half's `static` functions,
   objects, fat boxes and `extern` declarations that nothing live names, and on
   a split TU the heavy system includes nothing uses
   (`src/compiler/jit_prune.h`). The unpruned TU is kept for the W0071 retry.
   `TUR_JIT_NO_PRUNE=1` opts out.
5. **Include dirs.** `jit_sdk_include_dirs` produces `<root>/src` and
   `<root>/src/runtime` from `$TUR_SDK_ROOT`, or by walking up from the
   executable probing for `src/runtime/hamt.h`.
6. **Into the engine.** `tur_jit_execute` -> `jit_compile_and_link`:
   - `jit_load_autolink` -- each `-l<name>` marker becomes
     `dlopen("lib<name>.so", RTLD_NOW|RTLD_GLOBAL)`.
   - `JIT_PRELUDE` is concatenated ahead of the emitted TU.
   - `MIR_init`, then `MIR_set_error_func(jit_mir_error)` and a `setjmp`
     landing pad.
   - `c2mir_compile` pulls the in-memory buffer one char at a time through
     `jit_getc`.
   - `MIR_gen_init` and `MIR_gen_set_optimize_level(ctx, 2)`.
   - `MIR_load_module` over every module `MIR_get_module_list` reports.
   - `MIR_link(ctx, gen_iface, jit_import_resolver)`.
   - `jit_sync_config_globals`.
7. **Find and run `main`.** `jit_find_func` walks the module and item lists
   rather than calling `dlsym` -- which is what lets it see **static**
   functions, and is why single-TU spice emission keeps its `static` linkage
   unchanged. The function pointer is then called on a fresh pthread with a
   `TUR_MAIN_STACK_MB` stack (default 1024 MB on 64-bit, 64 MB on 32-bit; `TUR_JIT_STACK_MB` still works and wins), followed by `jit_atexit_drain`
   and `fflush(stdout)` **on that same thread**.
8. **Teardown**, strictly in this order: `MIR_gen_finish`, `c2mir_finish`,
   `MIR_finish`.

Anything that goes wrong returns `TUR_JIT_ERR_COMPILE`, `TUR_JIT_ERR_LINK`, or
`TUR_JIT_ERR_RUN`, and the driver falls back -- see the next section.

### The C the JIT sees is not `tur emit-c` output

Worth internalizing before you debug a JIT-only failure by eyeballing
`tur emit-c`. Relative to `tur emit-c`, the JIT's input differs in five ways:

1. `g_emit_for_link = true`, so the `rc<T>`/GC runtime comes from the archive
   instead of being replicated into the preamble.
2. `hoist_tur_include_directives` has lifted `__tur_include__` directives to
   the top of the TU.
3. The engine has prepended `JIT_PRELUDE`.
4. The S2 splice has replaced most of the fixed preamble.
5. The pruner has dropped every prelude function the program never reaches.
   `TUR_JIT_DUMP_C` writes the text c2mir actually got, so its line numbers
   match a `<tur-jit>:LINE` diagnostic.

(`tur emit-c` also passes manifest reader macros, which `cmd_jit` does not.)
The right comparison is `tur build`, whose front half `cmd_jit` copies exactly;
`TUR_JIT_NO_SPLIT=1 TUR_JIT_NO_PRUNE=1` removes the largest remaining
differences.

### The pieces that exist only because of the JIT

**`JIT_PRELUDE`** (`src/jit_engine.c`) is a string prepended to every TU, in
two halves. First, Apple/aarch64 SDK repairs: MIR's own prelude defines
`__arm64__` with an *empty* replacement list, so every SDK `#if __arm64__`
becomes a bare `#if` and the guarded block silently vanishes -- the prelude
redefines it and the `TARGET_*` family properly. Second, prototypes for the
`__builtin_*` functions that reach the emitted C from **inline C** in
`stdlib/math.tur` and friends. Those prototypes are load-bearing: an
undeclared `__builtin_sqrt` gets an implicit `int` return, the call reads the
integer return register while the shim delivers in `xmm0`, and
`floor(sqrt(25.0))` comes out as 1.

The prelude deliberately does **not** fake `__thread` or lower `__atomic_*`
to non-atomic operations, even though the spike shim did. Shipping those would
trade a clean compile error for silent corruption under `spawn`. **Do not add
them.**

**`jit_import_resolver`** checks a small `JIT_SHIMS[]` table, then falls
through to `dlsym(RTLD_DEFAULT, name)`. The table holds the `__builtin_*`
wrappers, our `atexit` interceptor, and `_OSSwapInt{16,32,64}` -- Darwin
spells those `__DARWIN_OS_INLINE`, so c2mir emits no definition and every
network-byte-order program (the whole httpd family, 27 fixtures) died at
`MIR_link` on `import of undefined item _OSSwapInt16`.

**Lazy generation, under our own lock.** The default is lazy `MIR_gen`, but
through `jit_lazy_gen_locked`, not MIR's `MIR_set_lazy_gen_interface`. MIR-gen
is not thread-safe and it generates on the context's single shared `gen_ctx`;
Turmeric has `spawn`, fibers, and a work-stealing scheduler, which produced
three different assertions across five runs of one fixture. The replacement is
built from public primitives only (`_MIR_get_wrapper`, `_MIR_redirect_thunk`,
`MIR_gen`, `func->machine_code`), so no fork patch was needed. The
**double-check inside the lock is the load-bearing half** -- a plain mutex
still lets two threads that both got past the stub generate the same function
twice and trip `_MIR_duplicate_func_insns`.

**`jit_mir_error`.** MIR's default error handler prints and `exit()`s the
process. Ours prints and `longjmp`s to the landing pad so the fallback can
happen. Before this existed, 13 fixtures whose stdlib inline C used
`__atomic_*` died with empty output instead of falling back. On that unwind the
MIR context is **deliberately leaked**: tearing it down from an undefined
intermediate state is how a fallback becomes a crash.

**`jit_sync_config_globals`** copies the program's strong
`tur_closure_headers_enabled` onto the host's weak copy after `MIR_link` --
see the weak-symbol constraint below.

**The S2 split runtime.** `tur_rt_split.c` is a generated TU linked into
`tur_core` that *replaces* the host's own copies of the cont/panic, CPS
prompt, STM, scheduler, and timer-wheel runtime, so `dlsym(RTLD_DEFAULT)`
cannot resolve a JIT'd program into a different vintage of the same function.
`tur_rt_split_embed.c` carries the declarations region plus a hash of the
preamble it was generated from; `cmd_emit_rt_split` (`tur emit-rt-split
[--hash]`) is what `tools/gen-runtime-split.py` regenerates them from, and it
uses the same hash spelling as the JIT-time compare so the two can never
disagree.

### Where else the engine is used

- **The spice REPL** (`tur repl --engine jit`). `src/turi/spice_loader.c`
  carries a `TurSpiceJitHook` function-pointer vtable -- a vtable because the
  loader lives in `tur_core` while the engine is only linked into `tur`. With
  the hook installed, `tur_spice_image_load` skips the rebuild-check, the
  subprocess build, and the `dlopen` entirely, and compiles the whole spice as
  one TU in memory; symbol binding goes through the hook instead of `dlsym`.
  Every load compiles fresh, which is the point -- there is no cached artifact
  to go stale. `repl_jit_build` in `src/main.c` does the interesting work:
  a shadow directory mapping module name to source file (symlinks; hard links
  or copies on Windows, where `symlink()` is an `ENOSYS` stub), a synthetic
  `__jit_root.tur` so imports dedupe through the ordinary module machinery,
  and a save/clear of `g_interpret_mode` around the compile so the REPL's
  interpreted posture does not select `#?(:turi ...)` branches into native
  code. Known v1 limit: transitive `:spices` deps are not auto-appended.
- **Embedding.** `tur_jit_compile_image` / `tur_jit_image_sym` /
  `tur_jit_image_free` give a plain `libturi` consumer the same engine;
  `tests/turi/jit-embed.c` is the proof, cross-checking `turi_eval` against a
  JIT'd image on the same computation. Note that c2mir discards the
  `constructor` attribute, so this path calls `__tur_static_init` explicitly.
- **Tests.** `tests/run-jit.sh` (whole corpus), `tur_repl_spice_jit`,
  `tur_jit_embed`, plus a dedicated `-DTUR_JIT=ON` CI leg that guards against
  silently losing the flag and reporting green having run nothing.

---

## The fallback contract

c2mir accepts a smaller language than gcc or clang. When a translation unit
fails to compile or link under MIR, `tur jit` **falls back to `cc`** and runs
the program that way. The fallback is the design, not a failure mode: a
program that takes it is slower to start but produces identical output.

The corollary is what matters for correctness work: **a fallback is a loud,
safe outcome. A silent layout or ABI divergence is not.** Every constraint in
the next section exists because it is in the second category -- the code
compiles under both engines and means different things.

A few families always take the fallback, deliberately:

- `__atomic_*` / `__sync_*` builtins. MIR binds one name to one signature, so
  any shim would write the wrong width for some caller.
- `_Thread_local` / `__thread`. Collapsing a thread-local to a global would
  trade a clean compile error for silent cross-thread corruption under
  `spawn`.

### The two ladders

There are two, and they are distinct:

- **Reduced TU to full TU (TUR-W0071).** Only when the engine was handed a
  split and/or pruned TU and the result was `TUR_JIT_ERR_COMPILE` or
  `TUR_JIT_ERR_LINK`: retry the full, unpruned TU in the engine.
  `tests/run-jit.sh` fails a fixture that passes only on this retry (unless it
  then fell back to cc anyway), since that is how a reference the pruner missed
  would show up. `TUR_JIT_ERR_RUN` is deliberately *not*
  retried -- a run failure is the program's own, and a panic aborts identically
  either way.
- **Engine to `cc` (TUR-W0070).** On **any** non-OK result, warn and call
  `cmd_run` with the original argv. Never a hard stop.

Roughly, the error classes mean: `COMPILE` -- `c2mir_compile` said no (subset
gap, a GNU construct in user inline C, `__atomic_*`, `__thread`), or there was
no `main` item; `LINK` -- an autolink `dlopen` failed, or MIR raised inside
`MIR_link` on an unresolved import; `RUN` -- `pthread_create` failed.

**The one hole worth knowing about:** under lazy generation, a *generation*
failure surfaces at first call -- after output may already have been written,
and past the point the landing pad can unwind -- so W0070 cannot catch it.
`TUR_JIT_GEN=eager` is the diagnostic; it doubles as a verification pass
because generation failures then happen while the `cc` fallback is still
reachable.

---

## Constraints that are permanent

### Codegen must not emit GNU-only constructs -- `__auto_type` above all

c2mir is a C11 front end. `__auto_type` is a GCC/clang extension it does not
implement, and a construct it cannot parse is not a per-function problem: the
**whole translation unit** is rejected, so one such line anywhere in the
emitter takes every program that reaches it off the engine.

This is easy to do by accident, because `__auto_type` is genuinely the right
tool for the job it keeps getting reached for -- giving a hoisted temp the
value's EXACT emitted representation (carrier `int64_t` vs by-value aggregate
vs typed pointer) without re-deriving it, which the repr heuristic gets wrong
for some carrier calls. Three sites in the emitter have now had to solve it
separately: `emit_fns.c`'s panic-check hoist (names the callee's return type
from the forward-declaration pass, and keeps `__auto_type` only for indirect
calls), `emit_cps_ir.c`'s cps->direct temp, and the region erasure note in
`emit_expr.c` (notes a bare identifier in place, since it is already an
lvalue, and names the type via `emit_binding_repr_c_name` otherwise).

**The failure is silent per program, so the suite checks it in aggregate.**
The engine's per-program `TUR-W0070` fallback to the cc path is a
*correctness* safety net, so the answers stay right; `run-jit.sh` counts a
fallback as a PASS by design. When `__auto_type` was added to the region
erasure note, the stdlib prelude's own erasing ascription put **every**
fixture on the cc path and the suite still printed `0 failed`. It surfaced
only because the fallback then broke for ten fixtures on one platform for an
unrelated reason. Two checks now close that gap: `run-jit.sh` runs one
trivial program through the engine before the fixture loop and fails if it
falls back (the tree-wide case, in a second), and the fixtures allowed to
fall back are listed by name in `tests/jit-fallback-baseline.txt` -- a new
fallback fails the run, a reclaimed one is reported so its line can be
removed, and `TUR_JIT_FALLBACK_UPDATE=1 bash tests/run-jit.sh` regenerates
the file. See
[jit-suite-reports-pass-when-the-engine-is-disabled](https://github.com/turmeric-lang/turmeric/blob/main/docs/archive/jit-suite-reports-pass-when-the-engine-is-disabled.md)
(a blob URL rather than a relative one because `docs/reported/` is not
rendered into the pack -- the Justfile's `docs` recipe spells out why).

So when you add an emitter construct, check it: build with `-DTUR_JIT=ON` and
run one fixture, and if `TUR-W0070` appears where it did not before, the
engine has stopped reading your output.

```sh
TUR_JIT_DUMP_C=/tmp/jit.c ./build-jit/tur jit <fixture>.tur
# a c2mir diagnostic names <tur-jit>:LINE:COL against exactly this text
```

The same applies to statement expressions (`({ ... })`), nested functions,
`typeof`, and `__attribute__` spellings beyond the ones the fork implements.

One more that gcc and clang hide: a cast to a struct type, even the identity
cast `(tur_tagged_t)(x)` on an `x` that already is one. Both host compilers
take it as an extension; c2mir enforces C11 6.5.4 and reports
`conversion to non-scalar type requested`. `TUR_APPLYn_T` casts every
argument to its declared C type, so a closure invoked through it with a
by-value struct argument (the Saffron dynamic call, whose arguments are all
`tur_tagged_t`) falls back to cc -- which is why the dynamic-call emitter
spells the prototype cast out and passes the boxes uncast. Under this
diagnostic, look for a struct-typed `Ai` before anything else.

A bug that used to compile but answer wrong, fixed in the fork since
`96c34860`: on x86-64, a statement expression whose VALUE is a struct
(`({ ...; TUR_TAG(...); })`, or one that yields a by-value ADT) placed in a
call's argument list or as a local's initializer overwrote a sibling argument
or parameter. The cause was in c2mir's front end (the interpreter, `-ei`,
answered wrong too): it reserved the statement expression's result slot in the
frame while the body was still being checked, and only afterwards laid the
stack variables out from offset 0 -- so the slot landed on the first of them,
typically a by-value struct parameter. The emitter had already stopped
producing the shape (it builds every such value with statements in the body),
so today only user inline C could reach it, and
`tests/fixtures/jit-inline-c-struct-stmtexpr-slot` fails if the MIR pin ever
drops below the fix. See
[jit-x86-64-struct-valued-statement-expression-miscompiles](https://github.com/turmeric-lang/turmeric/blob/main/docs/archive/jit-x86-64-struct-valued-statement-expression-miscompiles.md)
for the shapes and the sites. On an older pin, a Linux-only `stdout mismatch`
in the JIT suite on a program that passes under cc is the signature.

### `__attribute__((packed))` is silently ignored

c2mir lays packed structs out at natural alignment and emits no diagnostic
worth catching. The struct compiles, the program runs, and its offsets
disagree with the host's.

`#pragma pack` **is** implemented in our fork (tracked through the
preprocessor, so a `pack(push)` spanning an `#include` stays correct -- which
is how the Apple and Windows SDKs actually use it). The attribute spelling
goes through a different path and is still ignored.

Two rules follow:

- Do not use `__attribute__((packed))` in inline C that may run under the JIT.
  Use `#pragma pack` if you need packing at all.
- **Codegen must never start emitting packed structs** without fixing c2mir
  first. This one is load-bearing rather than advisory: there is no
  diagnostic and no fallback to catch it.

### Host and c2mir must agree on every shared struct layout

Any runtime struct whose layout the host `cc` and c2mir compute differently
is a silent ABI seam. Program-side inline C reading such a struct gets wrong
offsets with nothing to tell it so.

The instance that found this: c2mir modeled `__uint128_t` as a two-word
struct with alignment 8 where clang gives 16, which skewed `ucontext_t` --
and with it `FiberBlock` -- by 32 bytes. `async-await-channel` and
`fiber-scheduler` hung under the JIT and only under the JIT. It is fixed in
the MIR fork, but the shape recurs.

The durable mitigations:

- Keep `ucontext_t`-bearing (and other layout-fragile) structs behind
  host-resident accessors rather than re-declaring them program-side.
- Assert `sizeof` / `offsetof` agreement at JIT startup for the structs that
  cross the boundary.
- Never hand-roll a `typedef` for a struct the stdlib already defines. Three
  mutually inconsistent local `TaskGroupBlock` typedefs in `emit_module.c`
  once put a `pthread_mutex_lock` at the wrong offset; the symptom was silent
  empty stdout on arm64 macOS.

### An unprototyped call is broken differently under each engine

c2mir treats a call with no prototype in scope as **all-anonymous-variadic**.
On Apple arm64 that shifts arguments by a register: shimming Darwin's
`_OSSwapInt16` by address alone made `htons(8080)` return `0xb8f6` instead of
`0x901f` -- corrupted ports and header lengths, not a crash.

On the `cc` path the same omission is merely bad (an implicit `int` return
truncates 64-bit values). Declare prototypes for everything; see
[c-integration-guide.md](c-integration-guide.md).

### `bool` truthiness diverges

Reading a byte that is neither 0 nor 1 through a `bool` lvalue is undefined,
and the two back ends resolve it differently: clang masks to bit 0, c2mir
tests the whole byte. A latent layout bug can therefore be invisible on
Linux/`cc` (glibc happens to zero the byte) and deterministic under the JIT
(macOS leaves `0x5A` there).

### Weak-symbol overrides do not cross the JIT boundary

The runtime declares `__attribute__((weak)) int tur_closure_headers_enabled`
and the emitted program overrides it with a strong definition. Under `cc` the
linker resolves that. Under the JIT, host code in this process was linked
long ago and reads its own weak copy, so `jit_sync_config_globals` copies the
program's value onto the host global after `MIR_link`.

That works for weak **data**. Weak **functions** cannot be fixed by copying a
value -- `scheduler_common.c` still carries six weak no-op
`tur_scheduler_*_st` functions with the same hazard. Any new weak default in
the runtime is a latent JIT bug; prefer an explicit runtime call from the
program's static init over a link-time handshake.

### `atexit` is intercepted

Registering the real `atexit` is worse than failing: the handler is JIT'd code
and `MIR_gen_finish` unmaps it before libc drains its list, so the process
dies in freed code at exit. The JIT owns the list (64 slots) and drains it
LIFO on the entry thread while the generated code is still mapped -- on that
thread specifically, because handlers may read host TLS the program wrote
there.

### `-I` does not reach c2mir

Include directories for the JIT come from `jit_sdk_include_dirs`
(`src/main.c`), rooted at `TUR_SDK_ROOT` or discovered by walking up from the
executable. A `-I` on the `tur jit` command line does not affect what c2mir
can find.

---

## Running the fixture corpus under the JIT

`tests/run-jit.sh` runs the corpus through `tur jit`. It defaults to
`./build-turjit/tur`, falling back to `./build/tur`, and probes the binary for
`usage: tur jit` -- a build without the engine **skips the whole run and exits
0**, so read the summary, not just the exit code. Fixtures that emit TUR-W0070
are tallied as `PASS_FALLBACK` and deliberately not stamped, so a future engine
improvement gets the chance to reclaim them. `JIT_KNOWN_MISCOMPILE` is the
mechanism for carrying a compile-path miscompile without hiding it, and is
currently empty.

Two things about it are easy to get wrong:

- **Environment parity with `tests/run.sh` is mandatory.** `run.sh` exports
  `TUR_BIND_LOOPBACK=1`, which `stdlib/httpd.tur` and `stdlib/async_socket.tur`
  read at run time to bind `INADDR_LOOPBACK` instead of `INADDR_ANY`. When
  `run-jit.sh` did not export it, an httpd fixture failed on macOS only -- BSD
  permits a wildcard bind while a specific address holds the port where Linux
  refuses -- and it read as a JIT defect for a while. Any env var `run.sh`
  exports, this harness must export.
- **A Release-built `tur` strips contracts under NDEBUG.** A fixture that
  asserts contract or refinement runtime behavior must carry `--keep-contracts`
  in its own `flags` rather than inheriting it from how `tur` was built. See
  [test-runner-contract.md](test-runner-contract.md). A fully green run needs a
  Debug binary, because refinement discharge is Debug-only.

On macOS, `CC` must name the same compiler that built `tur`, or fallback
links die on `___asan_version_mismatch_check_v8` and the numbers are garbage
in a direction that looks like a product bug. See the macOS build notes in
the top-level `CLAUDE.md`.

---

## Tuning knobs

All environment variables -- deliberately never `--enable=` experiments, because
they are diagnostics rather than semantics.

| variable | default | effect |
|---|---|---|
| `TUR_JIT_GEN` | lazy | `eager` restores whole-program generation (slower start, but generation failures surface while the fallback is still reachable); `interp` is spike instrumentation only |
| `TUR_MAIN_STACK_MB` | 1024 (64 on 32-bit) | entry stack size in MiB, the same variable that sizes a compiled `#lang r7rs` program's `main` stack; MIR does not do gcc's sibling-call optimization, so a deep recursion the `cc` path survives can overflow here. A value that is not a positive number is reported and ignored |
| `TUR_JIT_STACK_MB` | unset | the older, JIT-only name for `TUR_MAIN_STACK_MB`; wins when both are set |
| `TUR_JIT_NO_SPLIT` | unset | skip the S2 preamble splice and compile the full runtime preamble |
| `TUR_JIT_NO_PRUNE` | unset | hand c2mir every definition the emitter wrote, including the prelude the program never reaches |
| `TUR_JIT_TIMING` | unset | `1` prints per-phase timings and RSS to **stderr**, so fixture stdout stays byte-comparable |
| `TUR_JIT_KEEP_C2MIR` | unset | `1` keeps a persistent image's C front-end state until the image is freed, instead of releasing it once the image is initialized (bisection only; it costs ~25 MB per resident image) |
| `TUR_SDK_ROOT` | discovered | root for the runtime headers c2mir needs |

Full corpus results are identical between lazy and eager; lazy saves 23-36% of
end-to-end wall time.

---

## See also

- [performance-guide.md](performance-guide.md) -- the interpreter / JIT / `cc`
  triangle, measured, and the self-tail-call rules that decide which of your
  loops are actually loops.
- [c-integration-guide.md](c-integration-guide.md) -- inline-C rules, several
  of which only bite under the JIT.
- [experimental-flags-guide.md](experimental-flags-guide.md) -- the mechanism
  the retired `--enable=jit` gate came from, and what a graduated name does now.
- [repl.md](repl.md) -- the spice REPL the J2 image path backs.
- `docs/archive/jit-engine-plan.md` -- why MIR, the engine-choice table, and
  the phase-by-phase status.
- `docs/archive/jit-engine-j0-findings.md` -- the numbered findings log; the
  empirical record behind nearly every claim in this guide.
- `docs/archive/mir-interp-tier-plan.md` -- why there is no interpreter tier.

This guide carries what is settled; those carry what is moving.
