cmake_minimum_required(VERSION 3.20)

# ---------------------------------------------------------------------------
# Feature-test macro: glibc hides POSIX/BSD declarations (strdup, isatty,
# clock_gettime, ...) under strict -std=c11/-pedantic unless a feature macro
# is defined. Apple libc exposes them regardless, so this is a no-op there.
# Must be defined before any system header is included, hence a -D flag.
# ---------------------------------------------------------------------------
add_compile_definitions(_DEFAULT_SOURCE)

# WIN3: the reactor's Windows I/O backend (async/io_iocp.c) links against
# Winsock. tur_core is an OBJECT library, so its Winsock dependency does not
# propagate to the many executables that embed its objects (tur, the tur_*
# test harnesses). A directory-scoped link_libraries applies to every target
# defined below, which is the simplest way to cover them all.
if(WIN32)
    link_libraries(ws2_32)
endif()

# ---------------------------------------------------------------------------
# Feature-test macro: macOS gates the POSIX-XSI ucontext_t (used by
# TuriEnv::sched_ctx for the async scheduler) behind _XOPEN_SOURCE.
# Without it, <sys/_types/_ucontext.h> exposes a legacy 56-byte stub,
# and TUs that pull in <signal.h> / <sys/wait.h> BEFORE turi/env.h get
# the small stub baked into their view of `struct TuriEnv` -- producing
# a struct-layout mismatch (sizeof differs by ~832 bytes) and silent
# null-pointer reads of fields declared after sched_ctx. Forcing it
# project-wide guarantees every TU agrees on TuriEnv's layout.
# _DARWIN_C_SOURCE keeps the macOS-specific extensions visible on top
# of POSIX-XSI (mkstemps, NSIG, st_mtimespec, ...).
# ---------------------------------------------------------------------------
if(APPLE)
    add_compile_definitions(_XOPEN_SOURCE=700 _DARWIN_C_SOURCE)
endif()

# ---------------------------------------------------------------------------
# Warnings-as-errors, except during the Windows bring-up.
#
# The Windows port is in progress (docs/archive/windows-support-plan.md).
# A first-time portability warning on a platform nobody has compiled for yet is
# information, not a defect to halt on -- promoting it to a hard error just
# hides the next twenty behind it.  Restore -Werror here once the port is warning
# clean; every other platform keeps it today.
# ---------------------------------------------------------------------------
if(WIN32)
    set(TUR_WERROR "")
else()
    set(TUR_WERROR -Werror)
endif()

# ---------------------------------------------------------------------------
# Format-string policing (security-audit-plan WP5).  -Wformat=2 adds
# -Wformat-nonliteral: a format that is not a string literal cannot be checked,
# and one the PROGRAM supplies is how the interpreter's printf/snprintf
# emulation (S-2, S-3) turned a `%s` into a dereference of an integer.  The
# printf-style wrappers (buf_printf, diag_emit, turi_errorf, ...) carry
# TUR_PRINTF_FMT, so every call's arguments are checked against its format too.
# Under TUR_WERROR a new one fails the build.  The interpreter's emulation
# formats with a copy of the program's string that WP3's check rebuilt; it is
# not a literal, so those sites in turi/eval.c are silenced by name.
# ---------------------------------------------------------------------------
set(TUR_FORMAT_WARNINGS -Wformat=2 -Wformat-security)

# ---------------------------------------------------------------------------
# C standard: gnu11 on Windows, c11 everywhere else.
#
# -std=c11 defines __STRICT_ANSI__, and MinGW uses that to hide every
# non-ISO CRT declaration.  That breaks more than the POSIX calls we reach for
# (mkdir, getcwd, ...): it breaks MinGW's OWN headers, because <dirent.h>
# declares a `struct _finddata_t` field whose definition lives behind the same
# gate in <io.h> -- so <dirent.h> fails to compile with "field 'dd_dta' has
# incomplete type" before any of our code is even reached.
#
# -std=gnu11 is the same language with the platform's declarations left visible.
# It is not a laxity knob; it is the only spelling under which MinGW's headers
# are self-consistent.
# ---------------------------------------------------------------------------
if(WIN32)
    set(TUR_CSTD -std=gnu11)
else()
    set(TUR_CSTD -std=c11)
endif()

# ---------------------------------------------------------------------------
# Phase S1: detect libedit for REPL line editing + history
# ---------------------------------------------------------------------------
find_library(LIBEDIT_LIB edit)
if(LIBEDIT_LIB)
    set(HAVE_EDITLINE 1)
    message(STATUS "libedit found: ${LIBEDIT_LIB} — enabling REPL line editing")
else()
    set(HAVE_EDITLINE 0)
    message(STATUS "libedit not found — REPL will use plain fgets")
endif()

# ---------------------------------------------------------------------------
# Phase S0: turi eval core sources (shared between tur and libturi)
# ---------------------------------------------------------------------------
set(TURI_EVAL_SOURCES
    turi/value.c
    turi/env.c
    turi/fiber.c
    turi/eval.c
    # Vec/Set/Map/HAMT interpreter native overrides, relocated from main.c so
    # they land in libturi for all interpreter consumers (turi-interp-collections).
    turi/collections_native.c
    # string_native.c -- interpreter native overrides for the owned String type
    # (owned-string-type-plan), all delegating to src/runtime/tur_string.c so
    # --interpret matches the compiled path.
    turi/string_native.c
    # preload.c -- shared stdlib preload sequence (macros + typed collections),
    # relocated from main.c so the native --interpret path and the WASM REPL
    # load the same list (web-repl-missing-stdlib-preload).
    turi/preload.c
    # docstrings.c -- reads stdlib/docstrings.tur's doc table from C, for
    # `tur doc`, the playground doc panel, and the doc-lookup/doc-print natives
    # (playground-session-hygiene-plan PS2/PS3).
    turi/docstrings.c
    # interpreter_natives.c -- the wk_register_* / native_* inline-C override
    # block relocated from main.c so `tur repl` and the WASM REPL register the
    # same interpreter natives as --interpret (web-repl-repl-inline-c-native-gap).
    turi/interpreter_natives.c
    # native_caps.c -- the capability each builtin native requires, checked at
    # the one native dispatch in eval.c (security-audit-plan WP3, S-1).
    turi/native_caps.c
    # macro_env.c -- the per-compile macro-time interpreter env
    # (macro-system-direction-plan Stage 1): elab_macro_env_get/_dispose,
    # declared in compiler/elab.h, implemented turi-side to keep the include
    # direction turi -> compiler.
    turi/macro_env.c
    # dap.c -- Debug Adapter Protocol server (debugger Phase 3), a thin shell
    # over the Phase 2 interpreter debugger's control API in eval.c.
    turi/dap.c
    # trace.c -- the time-travel recorder (editor-intelligence follow-through,
    # track T). Drives the same Phase 3 control API dap.c does: a pause
    # handler that captures and immediately resumes step-in IS a tracer.
    turi/trace.c
    turi/repl.c
    # r7rs-lang-plan T4: the evaluator behind R7RS `eval` -- one embedded
    # R7RS env that a program importing (scheme eval) links in, and that
    # `tur --interpret` reaches through native twins.
    turi/r7rs_embed.c
    # RP3: REPL spice-image loader. Walks up to build.tur, dlopens
    # .tur-repl-cache/lib.so, primes a symbol map from exports.manifest.
    turi/spice_loader.c
    # RP4: TuriValue<->scalar marshaling layer that installs each
    # spice export as a TuriNativeFn binding the eval loop can call.
    turi/ffi_thunk.c
    # jit-ffi-c2mir-plan: the MIR-free half of the JIT-scoped dynamic FFI --
    # provider hook + signature vocabulary.  The c2mir-backed provider itself
    # lives in turi/jit_ffi.c inside tur_jit_obj (JIT builds only).
    turi/jit_ffi_hook.c
    # aot-compiled-repl-plan C1: compile an inline-C defn the interpreter
    # cannot run, through a host hook (main.c registers it on a JIT build).
    # MIR-free, like jit_ffi_hook.c.
    turi/inline_c_jit.c
)

# ---------------------------------------------------------------------------
# Include directories: src/ plus each subsystem subdirectory, so that the
# existing bare `#include "foo.h"` directives keep resolving after the
# source-layout split. turi/ is intentionally omitted -- turi sources resolve
# their own headers via the compiler's current-directory rule, which also
# avoids the async/fiber.h vs turi/fiber.h basename clash.
# ---------------------------------------------------------------------------
set(TUR_INCLUDE_DIRS
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/compiler
    ${CMAKE_CURRENT_SOURCE_DIR}/passes
    ${CMAKE_CURRENT_SOURCE_DIR}/runtime
    ${CMAKE_CURRENT_SOURCE_DIR}/async
    ${CMAKE_CURRENT_SOURCE_DIR}/web
    ${CMAKE_CURRENT_SOURCE_DIR}/lsp
)

# ---------------------------------------------------------------------------
# Compiler sources (all except main.c and turi eval core)
# ---------------------------------------------------------------------------
set(TUR_CORE_SOURCES
    # compiler/ -- frontend pipeline: text -> IR
    compiler/builtins.c
    compiler/diag.c
    compiler/elab_core.c
    compiler/elab_macros.c
    compiler/elab_unsafe.c
    compiler/elab_forms.c
    compiler/elab_memory.c
    compiler/elab_effects.c
    compiler/elab_fns.c
    compiler/elab_call.c
    compiler/elab_module.c
    compiler/elab_structs.c
    compiler/elab_types.c
    compiler/elab_typeclasses.c
    compiler/elab_concurrent.c
    compiler/elab_sessions.c
    compiler/elab_global.c
    compiler/project.c
    compiler/elab_dynvars.c
    compiler/elab_toplevel.c
    compiler/elab_reflect.c
    compiler/mono_specs.c
    compiler/emit_core.c
    compiler/emit_dk_runtime.c
    compiler/emit_cps_ir.c
    compiler/emit_effects.c
    compiler/emit_expr.c
    compiler/emit_stmt.c
    compiler/emit_fns.c
    compiler/emit_module.c
    compiler/emit_split.c
    compiler/jit_prune.c
    compiler/expr.c
    compiler/fmt.c
    compiler/mangle.c
    compiler/stack_guard.c
    compiler/forms.c
    compiler/span_audit.c
    compiler/pkg.c
    compiler/justrun.c
    compiler/global.c
    compiler/install.c
    compiler/reader.c
    compiler/reader_macros.c
    compiler/lang_dialects.c
    compiler/scheme_lower.c
    compiler/stdlib_autoload.c
    # RT1--RT3 + S0--S3 (docs/archive/refinement-types-plan.md): refinement
    # types.  These stages are the whole solver -- the dev-only Z3 oracle that
    # once sat behind them was retired in 0.32.5, so there is no solver
    # dependency here to gate, in any build type.
    compiler/refine_vc.c
    compiler/refine_collect.c
    compiler/refine_smtlib.c
    compiler/refine_discharge.c
    compiler/refine_report.c
    compiler/refine_solver.c
    compiler/refine_solver_s0.c
    compiler/refine_solver_euf.c
    compiler/refine_solver_arith.c
    compiler/refine_solver_no.c
    compiler/symbols.c
    compiler/typeclass.c
    compiler/types.c
    # passes/ -- analysis and transformation passes
    passes/borrow_check.c
    passes/cps.c
    passes/cps_ir.c
    passes/effect.c
    passes/effect_check.c
    passes/effect_lower.c
    passes/kind_check.c
    passes/lifetime_elision.c
    passes/lifetimes.c
    passes/rc_elision.c
    passes/srfi_prune.c
    # runtime/ -- memory, data structures, execution
    runtime/arc.c
    runtime/arena.c
    runtime/buf.c
    # RP2: hand-rolled FFI dispatcher table. Generated by
    # tools/gen_ffi_dispatch.py -- see that script's header for the
    # naming convention and regeneration steps.
    runtime/ffi_dispatch.c
    # RP4: generated shape-switch that bridges the interpreter's
    # marshaling layer to the typed trampolines above.
    runtime/ffi_dispatch_thunk.c
    runtime/cps_rt.c
    # CT4 host API kept out of the generated runtime TU: emitted programs
    # resolve the contract-handler registry by address into the host.  Was
    # part of runtime/runtime.c before the S2 swap below.
    runtime/contract_handler.c
    runtime/experiments.c
    runtime/gc.c
    runtime/globals.c
    runtime/hamt.c
    runtime/image.c
    runtime/interp.c
    runtime/rc.c
    # r7rs-gc: the archive TUs' allocator hook (rt_alloc.h); dual-listed below.
    runtime/rt_alloc.c
    runtime/sha256.c
    runtime/rc_free_queue.c
    runtime/region.c
    runtime/serial.c
    # 6(a): atomics for front ends without the GCC builtins (the JIT). Host-side
    # by design -- see the file header and jit-engine-j0-findings.md 14.
    runtime/tur_atomics.c
    # TLS1: thread-local runtime state for front ends without TLS (the JIT).
    # Same host-residency pattern; see the file header and findings 15.
    runtime/tur_tls.c
    # SYM5: the interned-symbol registrar an emitted program calls from its
    # static init.  Dual-listed in TURT_RUNTIME_SOURCES like every other
    # runtime TU here; it was the one member of that list missing from this
    # one, which left tur_sym_register out of libturi.a entirely.  A static
    # archive only contributes members that resolve an undefined symbol, so
    # the duplicate definition costs an existing link nothing -- but a
    # whole-archive consumer (the JIT, which resolves runtime symbols by NAME
    # at run time) needs it present.  docs/archive/history/libturi-symbols-basename-collision.md.
    runtime/symbols.c
    # SX1 (solver-extension-plan): the backtrackable-state trail.  Dual-listed
    # in TURT_RUNTIME_SOURCES like every other runtime TU here, so a compiled
    # program can link the one maintained copy.
    runtime/trail.c
    runtime/tur_string.c
    # S2 (jit-engine-plan, findings 19.4 item 2 / seam 3): the generated
    # runtime TU -- the emitted preamble, every gate on, statics externalized,
    # TLS routed through tur_tls.c -- REPLACES the host's diverged copies of
    # the same subsystems (runtime/runtime.c panic/cont, runtime/cps_prompt.c
    # DK machine, runtime/stm.c, async/scheduler.c, async/timer_wheel.c).
    # Those five TUs shared 58 external symbol names with this one and had
    # ZERO consumers inside tur/libturi (verified by nm over every member;
    # the unit tests compile their subject .c directly and keep working).
    # Carrying both vintages would be a duplicate-symbol error at link and,
    # worse, would leave dlsym(RTLD_DEFAULT) resolving a JIT'd program's
    # runtime calls into the WRONG vintage -- the exact seam-3 crash the S2
    # proof hit.  The runtime library must BE the runtime.
    # Regenerate with tools/gen-runtime-split.py when the preamble changes.
    runtime/generated/tur_rt_split.c
    # ...and its declarations image (tur_rt_split_hash / tur_rt_split_decls).
    # This used to live in tur_jit_obj under `if(TUR_JIT)`, on the stated
    # grounds that "non-JIT builds never read it".  `--runtime=split` made the
    # CC PATH read it, which broke that premise: main.c's jit_try_split_preamble
    # is called from cmd_build, and a non-JIT build then failed with
    # `jit_try_split_preamble used but never defined [-Werror]` -- invisible in
    # any -DTUR_JIT=ON tree, which is every tree it was developed in.
    #
    # The split is a cc-path feature that happens to have grown out of a JIT
    # probe (the function name still says so), so its artifacts belong with the
    # compiler, not the engine.  Cost of moving it: a non-JIT build now carries
    # the ~100KB string TU it used to skip.
    runtime/generated/tur_rt_split_embed.c
    # async/ -- async I/O, fibers, scheduler
    async/atomic_queue.c
    async/fiber.c
    async/io.c
    async/reactor.c
    # web/ -- WASM glue and playground tutorial
    web/tutorial.c
    ${TURI_EVAL_SOURCES}
    # lsp/ -- LSP server (JSON-RPC over stdio, part of the tur binary)
    lsp/lsp_io.c
    lsp/lsp_sink.c
    lsp/lsp_collect.c
    lsp/lsp_scope.c
    lsp/lsp_json.c
    lsp/lsp_docs.c
    lsp/lsp_util.c
    lsp/lsp.c
    lsp/lsp_session.c
    # cli/ -- standalone subcommands
    cli/lsp_lite.c
    cli/completion.c
    cli/demangle.c
)

# ---------------------------------------------------------------------------
# Architecture detection
#
# Under MSYS2/MinGW, CMake leaves CMAKE_SYSTEM_PROCESSOR *and*
# CMAKE_HOST_SYSTEM_PROCESSOR empty, and the MSYS2 shell does not export the
# PROCESSOR_ARCHITECTURE that CMake would otherwise read them from -- so all
# three of the usual sources come back blank.  Fall through them in order and
# then ask the compiler directly, which always knows: GCC and Clang both answer
# `-dumpmachine` with a target triple ("x86_64-w64-mingw32").
#
# Case is normalised because native Windows reports "AMD64", not "amd64".
# ---------------------------------------------------------------------------
set(TUR_ARCH "${CMAKE_SYSTEM_PROCESSOR}")
if(NOT TUR_ARCH)
    set(TUR_ARCH "${CMAKE_HOST_SYSTEM_PROCESSOR}")
endif()
if(NOT TUR_ARCH)
    set(TUR_ARCH "$ENV{PROCESSOR_ARCHITECTURE}")
endif()
if(NOT TUR_ARCH)
    execute_process(
        COMMAND "${CMAKE_C_COMPILER}" -dumpmachine
        OUTPUT_VARIABLE TUR_ARCH
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
    )
endif()
string(TOLOWER "${TUR_ARCH}" TUR_ARCH)

# Architecture-specific assembly (added to both tur and libturi)
if(WIN32)
    # WIN3 tier C: real Windows x64 context switch (RCX first-arg, preserves
    # RSI/RDI/XMM6-15). Replaces the earlier aborting fiber_ctx_win.c stub.
    list(APPEND TUR_CORE_SOURCES async/fiber_ctx_x64_win.S)
    # The DK trampoline's landing pad.  libc longjmp on win64 is an SEH unwind
    # that dies on a fiber stack; the cc path uses __builtin_setjmp, but the JIT
    # path cannot, because c2mir has no such builtin and setjmp/longjmp must
    # pair.  An ordinary extern call is something BOTH halves of an S2 split can
    # make.  See async/tur_sjlj_x64_win.S.
    list(APPEND TUR_CORE_SOURCES async/tur_sjlj_x64_win.S)
elseif(TUR_ARCH MATCHES "arm64|aarch64")
    list(APPEND TUR_CORE_SOURCES async/fiber_ctx_arm64.S)
elseif(TUR_ARCH MATCHES "x86_64|amd64")
    list(APPEND TUR_CORE_SOURCES async/fiber_ctx_x64.S)
else()
    message(FATAL_ERROR "Unsupported architecture '${TUR_ARCH}'; expected arm64/aarch64/x86_64/amd64")
endif()

# Platform-specific I/O backends
if(WIN32)
    # Windows -- IOCP backend (io.h already selects IO_BACKEND_IOCP here).
    # Currently a stub: every entry point fails cleanly rather than silently
    # pretending to poll.  See docs/archive/windows-support-plan.md (WIN3).
    list(APPEND TUR_CORE_SOURCES async/io_iocp.c)
elseif(UNIX AND NOT APPLE AND NOT FREEBSD AND NOT OPENBSD AND NOT NETBSD)
    # Linux - use epoll
    list(APPEND TUR_CORE_SOURCES async/io_epoll.c)
elseif(APPLE OR FREEBSD OR OPENBSD OR NETBSD)
    # BSD/macOS - use kqueue
    list(APPEND TUR_CORE_SOURCES async/io_kqueue.c)
endif()

# ffi_dispatch.c and ffi_dispatch_thunk.c are generated files that cast
# void* to function pointers -- forbidden by strict ISO C but intentional
# and safe here.  Suppress -Wpedantic for these two files only.
set_source_files_properties(
    runtime/ffi_dispatch.c
    runtime/ffi_dispatch_thunk.c
    PROPERTIES COMPILE_OPTIONS "-Wno-pedantic"
)

# tur_rt_split.c is EMITTED C (the runtime preamble, generated by
# tools/gen-runtime-split.py): it carries the same fn-ptr/object-ptr casts
# every compiled Turmeric program does, plus ucontext idioms gcc's -Wclobbered
# flags.  It is compiled by `tur build` consumers under TUR_CC_FLAGS
# (-O2 -std=c99 -Wall), so hold it to that bar here rather than the
# compiler's own pedantic profile.  (-Wno-clobbered / -Wno-cast-function-type
# are gcc-only; clang ignores unknown -Wno-* silently unless another
# diagnostic fires, which -Wno-unknown-warning-option covers.)
set_source_files_properties(
    runtime/generated/tur_rt_split.c
    PROPERTIES COMPILE_OPTIONS
    "-Wno-pedantic;-Wno-cast-function-type;-Wno-clobbered;-Wno-unknown-warning-option"
)

# The decls image is one enormous string literal, exceeding C99's 4095-char
# minimum guarantee by design; every supported compiler handles it, so silence
# the portability nudge for that one generated TU.  Unconditional, alongside the
# source it applies to -- it lived inside `if(TUR_JIT)` while the TU did, and a
# property set in a branch the TU no longer builds under is silently no-op.
set_source_files_properties(
    runtime/generated/tur_rt_split_embed.c
    PROPERTIES COMPILE_OPTIONS "-Wno-overlength-strings"
)

# ---------------------------------------------------------------------------
# RM3 regions (on by default): the region runtime sources, embedded.  An
# emitted program that does not link libturt_runtime.a gets the bodies of
# src/runtime/{arena,region}.c pasted into its owner TU by the emitter
# (emit_region_runtime_bodies), the way the rc<T>/GC runtime is replicated
# under the DEDUP-4b archive posture.  Generated at build time from the real
# sources so there is one implementation, not a hand-written replica; see
# cmake/embed_region_runtime.cmake and src/runtime/region_rt_embed.h.
# ---------------------------------------------------------------------------
set(TUR_REGION_EMBED_C ${CMAKE_CURRENT_BINARY_DIR}/generated/region_rt_embed.c)
add_custom_command(
    OUTPUT ${TUR_REGION_EMBED_C}
    COMMAND ${CMAKE_COMMAND}
        -DSRC_DIR=${CMAKE_CURRENT_SOURCE_DIR}/runtime
        -DOUT=${TUR_REGION_EMBED_C}
        -P ${CMAKE_SOURCE_DIR}/cmake/embed_region_runtime.cmake
    DEPENDS
        ${CMAKE_SOURCE_DIR}/cmake/embed_region_runtime.cmake
        runtime/region.h runtime/region.c runtime/arena.h runtime/arena.c
        runtime/r7gc.c
    COMMENT "Embedding src/runtime/{arena,region}.{h,c} for the emitter"
    VERBATIM
)
list(APPEND TUR_CORE_SOURCES ${TUR_REGION_EMBED_C})

# ---------------------------------------------------------------------------
# tur_core: OBJECT library shared by tur executable and libturi
# ---------------------------------------------------------------------------
add_library(tur_core OBJECT ${TUR_CORE_SOURCES})

# ffi_dispatch.c is generated and uses void*->function-pointer casts that are
# valid POSIX/extension but not strictly ISO C; suppress -Wpedantic for it only.
set_source_files_properties(runtime/ffi_dispatch.c PROPERTIES
    COMPILE_OPTIONS "-Wno-pedantic")

# Subsystem subdirectories on the include path so bare #include directives
# (e.g. #include "arena.h") resolve regardless of which subdir the file lives in.
target_include_directories(tur_core PUBLIC ${TUR_INCLUDE_DIRS})

# Common warning flags
target_compile_options(tur_core PRIVATE
    -Wall -Wextra ${TUR_WERROR} ${TUR_FORMAT_WARNINGS} -Wno-unused-parameter ${TUR_CSTD} -pedantic -pthread
    # GCC's -Wformat-truncation flags bounded, memory-safe snprintf path
    # building (pkg.c); clang has no such warning. Keep it non-fatal on GCC.
    $<$<C_COMPILER_ID:GNU>:-Wno-format-truncation>
)
# Per-build-type flags
target_compile_options(tur_core PRIVATE
    $<$<CONFIG:Debug>:   -Og -g ${TUR_ASAN_FLAGS} -DTUR_DEBUG=1>
    $<$<CONFIG:Release>: -O2 -DNDEBUG>
    $<$<CONFIG:TSan>:    -O1 -g -fsanitize=thread>
)
# security-audit-plan WP4: coverage instrumentation for the fuzz targets in
# tests/fuzz, which link these objects.  (Only in a TUR_FUZZ build dir.)
if(TUR_FUZZ)
    target_compile_options(tur_core PRIVATE -fsanitize=fuzzer-no-link)
endif()
# Phase S3: ucontext_t is deprecated on macOS but still functional
if(APPLE)
    target_compile_options(tur_core PRIVATE -Wno-deprecated-declarations)
endif()
# Phase S1: editline feature flag
if(HAVE_EDITLINE)
    target_compile_definitions(tur_core PRIVATE TURI_HAVE_EDITLINE=1)
endif()

# Inject version string from VERSION file at compile time (E2)
# file(READ) runs at configure time and does not, by itself, make CMake
# reconfigure when VERSION changes -- so a version bump followed by an
# incremental `cmake --build` leaves the compiler binary with the stale
# TUR_VERSION define (observed: stdlib 0.63.3 vs compiler v0.63.2). Declaring
# VERSION as a CMAKE_CONFIGURE_DEPENDS forces a reconfigure on any change to
# it, which re-reads the file and re-emits the define.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/VERSION")
file(READ "${CMAKE_SOURCE_DIR}/VERSION" _TUR_VERSION_RAW)
string(STRIP "${_TUR_VERSION_RAW}" _TUR_VERSION_STR)
target_compile_definitions(tur_core PRIVATE TUR_VERSION="${_TUR_VERSION_STR}")

# ---------------------------------------------------------------------------
# tur: main compiler executable
# ---------------------------------------------------------------------------
add_executable(tur main.c lsp/mcp.c $<TARGET_OBJECTS:tur_core>)

# J1 (jit-engine-plan): the in-process MIR JIT engine behind `tur jit`.
# Only under -DTUR_JIT=ON: vendors MIR (cmake/mir.cmake, included by the
# top-level guard), compiles the engine TU, and exports tur's symbols so the
# engine's dlsym(RTLD_DEFAULT) resolver can see the linked-in runtime.
if(TUR_JIT)
  # I4 (mir-interp-tier-plan section 2.4): the engine is its own OBJECT
  # library, added to BOTH `tur` and libturi, so a C host embedding the
  # library can reach the image API instead of shelling out to `tur`.
  #
  # Deliberately NOT folded into tur_core: eighteen targets consume
  # $<TARGET_OBJECTS:tur_core>, including libturi_wasm and a dozen small unit
  # tests.  Putting the engine there drags MIR onto every one of their link
  # lines -- which fails outright for the WASM build, and for the unit tests
  # showed up as `undefined reference to MIR_set_error_func` across the
  # board.  A separate object library keeps the blast radius to the two
  # targets that actually want an engine.
  # jit-ffi-c2mir-plan F1: turi/jit_ffi.c rides in the engine's object
  # library, not tur_core -- it calls the image API and must not drag MIR
  # onto tur_core's eighteen consumers.
  # tur_rt_split_embed.c is NOT here any more -- it moved to tur_core, because
  # --runtime=split made the cc path read it. See the note at its new home.
  add_library(tur_jit_obj OBJECT jit_engine.c turi/jit_ffi.c)
  target_include_directories(tur_jit_obj PRIVATE ${TUR_INCLUDE_DIRS})
  target_link_libraries(tur_jit_obj PRIVATE tur_mir)
  target_compile_definitions(tur_jit_obj PRIVATE TUR_HAVE_JIT=1)
  target_compile_options(tur_jit_obj PRIVATE
      -Wall -Wextra ${TUR_WERROR} ${TUR_FORMAT_WARNINGS} -Wno-unused-parameter ${TUR_CSTD} -pthread
      $<$<CONFIG:Debug>:   -Og -g ${TUR_ASAN_FLAGS} -DTUR_DEBUG=1>
      $<$<CONFIG:Release>: -O2 -DNDEBUG>
      $<$<CONFIG:TSan>:    -O1 -g -fsanitize=thread>
  )
  # MIR's headers are not -pedantic-clean (extra semicolons from its DLIST
  # macros); suppress for the one TU that includes them, same treatment as
  # the generated ffi_dispatch.c above.
  set_source_files_properties(jit_engine.c PROPERTIES
    COMPILE_OPTIONS "-Wno-pedantic")
  # $<TARGET_OBJECTS:> carries objects only -- no usage requirements, no
  # transitive link -- so each consumer states the engine's needs itself.
  target_sources(tur PRIVATE $<TARGET_OBJECTS:tur_jit_obj>)
  # main.c's cmd_jit is compiled into `tur` directly, so it needs the define
  # on its own target -- it does not inherit tur_jit_obj's.
  target_compile_definitions(tur PRIVATE TUR_HAVE_JIT=1)
  target_link_libraries(tur PRIVATE tur_mir)
  # The engine resolves program symbols with dlsym(RTLD_DEFAULT), so whatever
  # process hosts it must export its own symbols.  For `tur` that is this
  # property; an embedding host has to set the equivalent itself (see the
  # ENABLE_EXPORTS note in jit_engine.h).
  set_target_properties(tur PROPERTIES ENABLE_EXPORTS TRUE)
  # MinGW: ENABLE_EXPORTS alone left tur.exe with NO export table on this
  # toolchain (measured: objdump -p showed none), so dlsym(RTLD_DEFAULT)
  # could not resolve a single runtime symbol and every JIT link died on the
  # first tur_* import.  Say it explicitly.  ld's --export-all-symbols still
  # excludes the MinGW runtime objects (printf and friends) -- those resolve
  # through JIT_SHIMS in jit_engine.c instead.
  if(WIN32)
    target_link_options(tur PRIVATE "-Wl,--export-all-symbols")
  endif()
endif()

# tur also needs the subsystem subdirs on the include path for main.c.
target_include_directories(tur PRIVATE ${TUR_INCLUDE_DIRS})

# Common warning flags
target_compile_options(tur PRIVATE
    -Wall -Wextra ${TUR_WERROR} ${TUR_FORMAT_WARNINGS} -Wno-unused-parameter ${TUR_CSTD} -pedantic
    $<$<C_COMPILER_ID:GNU>:-Wno-format-truncation>
)

# Per-build-type flags
target_compile_options(tur PRIVATE
    $<$<CONFIG:Debug>:   -Og -g ${TUR_ASAN_FLAGS} -DTUR_DEBUG=1>
    $<$<CONFIG:Release>: -O2 -DNDEBUG>
    $<$<CONFIG:TSan>:    -O1 -g -fsanitize=thread>
)

# Phase T19: Fiber work requires pthread for spinlocks
target_compile_options(tur PRIVATE -pthread)
target_link_options(tur PRIVATE -pthread)
target_link_libraries(tur PRIVATE m)
# RP3: turi/spice_loader.c uses dlopen/dlsym; on Linux those live in
# libdl (libSystem on macOS already exports them, but linking -ldl
# there is a harmless no-op).
target_link_libraries(tur PRIVATE ${CMAKE_DL_LIBS})

target_link_options(tur PRIVATE
    $<$<CONFIG:Debug>:   ${TUR_ASAN_FLAGS}>
    $<$<CONFIG:TSan>:    -fsanitize=thread>
)

# Phase S1: link libedit when available
if(HAVE_EDITLINE)
    target_link_libraries(tur PRIVATE ${LIBEDIT_LIB})
    target_compile_definitions(tur PRIVATE TURI_HAVE_EDITLINE=1)
endif()

# E2: inject version into tur executable (main.c reads TUR_VERSION)
target_compile_definitions(tur PRIVATE TUR_VERSION="${_TUR_VERSION_STR}")


# Ensure the compiler binary ends up at build/tur (same path the test scripts
# and games expect).
set_target_properties(tur PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
)

# ---------------------------------------------------------------------------
# Phase S0: libturi — static library for C embedding of the eval API
# ---------------------------------------------------------------------------
add_library(libturi STATIC $<TARGET_OBJECTS:tur_core>)
set_target_properties(libturi PROPERTIES OUTPUT_NAME "turi")
target_include_directories(libturi PUBLIC ${TUR_INCLUDE_DIRS})
target_link_options(libturi INTERFACE -pthread)
# RP3: propagate libdl to anyone linking libturi (the REPL helpers
# transitively call dlopen via turi/spice_loader.c).
target_link_libraries(libturi INTERFACE ${CMAKE_DL_LIBS})
# turi_env_new now pulls in interpreter_natives.c.o (the stdlib native
# overrides auto-registered for every interpreter env), whose math shims
# reference sqrt/floor -- propagate libm to anyone linking libturi.
target_link_libraries(libturi INTERFACE m)
# WIN3: propagate ws2_32 to anyone linking libturi (io_iocp.c uses Winsock).
if(WIN32)
    target_link_libraries(libturi INTERFACE ws2_32)
endif()

# I4: libturi carries the JIT engine when the build has one.  PUBLIC on both
# so an embedding host gets MIR on its link line and TUR_HAVE_JIT in its
# preprocessor -- the same capability probe `tur` itself compiles against, so
# a host can write one #ifdef and fall back to the tree-walking interpreter
# when the library was built without a JIT.
if(TUR_JIT)
    target_sources(libturi PRIVATE $<TARGET_OBJECTS:tur_jit_obj>)
    target_link_libraries(libturi PUBLIC tur_mir)
    target_compile_definitions(libturi PUBLIC TUR_HAVE_JIT=1)
endif()
if(HAVE_EDITLINE)
    target_link_libraries(libturi INTERFACE ${LIBEDIT_LIB})
endif()

# ---------------------------------------------------------------------------
# tur-link-and-build-split-plan Phase 6 prerequisite: libturt_runtime.a
# ---------------------------------------------------------------------------
# A lean, deliberately NON-sanitized static archive of exactly the runtime TUs
# that stdlib reaches via bare-source `__tur_autolink__` hints
# (src/runtime/{hamt,symbols,tur_string}.c).  `tur build --runtime=lib` links
# this instead of recompiling those sources on every build.
#
# Crucially it is built WITHOUT the Debug ASan/UBSan flags: linking the full
# (Debug) libturi.a pulls -fsanitize into the program and turns LeakSanitizer on
# for the whole binary, which trips fixtures that intentionally leak
# process-lifetime allocations.  This archive keeps the runtime-lib link
# behaviorally identical to the bare-source recompile (non-sanitized), so it can
# back `--runtime=lib` as a default.  See the plan's ASan caveat (Section 5/6).
#
# Compile flags mirror the default `tur build` cc_flags (-O2 -std=c99
# -fno-strict-aliasing) so the objects are ABI-identical to a bare-source
# compile; no -Werror so a stray warning here never fails the archive build.
# DEDUP-4b (gc-cycle-collection-plan): rc/gc/rc_free_queue join the archive so a
# compiled program can LINK the one maintained copy of the reference counter and
# cycle collector instead of carrying a hand-written replica of it in its
# preamble.  Safe to add ahead of the emit-side switch: while the emitted copy
# still defines these symbols itself, the linker simply never pulls these
# members in (a static archive member is only extracted to resolve an undefined
# symbol), so the archive grows but no existing link changes.
set(TURT_RUNTIME_SOURCES
    runtime/hamt.c
    runtime/trail.c
    runtime/symbols.c
    runtime/tur_atomics.c
    runtime/tur_tls.c
    runtime/tur_string.c
    runtime/rc.c
    runtime/gc.c
    runtime/rc_free_queue.c
    # r7rs-gc (docs/archive/r7rs-gc-plan.md): the allocator hook the members
    # above allocate through, so a program's collector can own their blocks.
    # libc until installed; a stdlib autolink marker compiles it beside hamt.c,
    # tur_string.c and symbols.c in bare-source mode.
    runtime/rt_alloc.c
    # RM3 regions: an emitted spine ctor calls tur_region_alloc_or_malloc when
    # --enable=regions is on, so the definition has to be linkable by the
    # program, not only by the compiler.  Same reasoning as the rc/gc members
    # above; with the flag off nothing references it and the linker never
    # extracts this member.
    runtime/region.c
    runtime/arena.c
    # o0-build-cannot-link-contract-handler: stdlib/contract.tur's
    # set-contract-handler! / with-contract-handler call these by extern.  At
    # -O2 the unused helpers are dropped and nothing references them, which hid
    # that no archive a program links defined them; at -O0 every program failed
    # to link.  Same "only extracted when referenced" reasoning as above.
    runtime/contract_handler.c
)
add_library(turt_runtime STATIC ${TURT_RUNTIME_SOURCES})
set_target_properties(turt_runtime PROPERTIES OUTPUT_NAME "turt_runtime")
target_include_directories(turt_runtime PRIVATE ${TUR_INCLUDE_DIRS})
target_compile_options(turt_runtime PRIVATE
    -O2 -std=c99 -fno-strict-aliasing -Wall -Wno-unused-parameter
    $<$<C_COMPILER_ID:GNU>:-Wno-format-truncation>
)
if(APPLE)
    target_compile_options(turt_runtime PRIVATE -Wno-deprecated-declarations)
endif()

# ---------------------------------------------------------------------------
# cc-path-preamble-split-plan: the runtime preamble, as a linkable archive
# ---------------------------------------------------------------------------
# Every emitted TU carries the fixed runtime preamble -- 4360 of 8310 lines for
# a one-line program, byte-identical every time -- and `tur build` recompiles it
# per fixture.  `tur build --runtime=split` links this instead, which measures
# at 29% off the cc call.
#
# Its OWN archive, deliberately not a member of turt_runtime: today's programs
# define these symbols themselves, so a link that pulled this member in to
# resolve one symbol would collide on all the rest.  It is linked
# all-or-nothing, and only by a program compiled with the decls region in place
# of the preamble.
#
# tur_sjlj_x64_win.S comes along on Windows: the split runtime calls
# tur_sjlj_set/_jump for the DK trampoline's non-local jump, and on the JIT path
# those resolve through jit_engine.c's JIT_SHIMS -- a cc link has no such table
# and needs the object.
add_library(turt_preamble STATIC runtime/generated/tur_rt_split.c)
if(WIN32)
    target_sources(turt_preamble PRIVATE async/tur_sjlj_x64_win.S)
endif()
set_target_properties(turt_preamble PROPERTIES OUTPUT_NAME "turt_preamble")
target_include_directories(turt_preamble PRIVATE ${TUR_INCLUDE_DIRS})
# Non-sanitized and warning-quiet for the same reason turt_runtime is: this is
# emitted C compiled as a library, not tur's own code.
#
# One section per function and per object, so a program's link can drop the
# parts of the preamble it never calls (`tur build` passes --gc-sections on
# ELF).  The archive is a single object, so without these the linker can only
# take all of it: a one-line program's code grew 4x under the split.
target_compile_options(turt_preamble PRIVATE
    -O2 -std=c99 -fno-strict-aliasing -w -ffunction-sections -fdata-sections
)

# ---------------------------------------------------------------------------
# Phase S8: install targets — libturi, public headers, tur binary, man page
# ---------------------------------------------------------------------------
include(GNUInstallDirs)

install(TARGETS libturi
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    COMPONENT libturi
)

# DEDUP-4b: the lean runtime archive has to ship too.  `--runtime=lib` PREFERS
# it over libturi.a (it is the non-sanitized one), and AUTO links only it -- so
# leaving it out of the install set meant an installed SDK could never take the
# archive path at all, silently recompiling the runtime sources on every build.
# It also gates the rc<T>/GC de-dup: without this archive an installed toolchain
# has nothing to link the collector from, and falls back to the emitted replica.
install(TARGETS turt_runtime
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    COMPONENT libturi
)

# cc-path-preamble-split-plan: a default build takes the preamble split only
# when this archive sits beside libturt_runtime.a, so an install without it
# would quietly keep recompiling the whole preamble in every program.
install(TARGETS turt_preamble
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    COMPONENT libturi
)

install(FILES
    turi/eval.h
    turi/env.h
    turi/value.h
    turi/fiber.h
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/turi
    COMPONENT libturi
)

install(TARGETS tur
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    COMPONENT tur
)

install(FILES ${CMAKE_SOURCE_DIR}/man/man1/tur-repl.1
    DESTINATION ${CMAKE_INSTALL_MANDIR}/man1
    COMPONENT tur
)

# ---------------------------------------------------------------------------
# Phase S0: tur_eval_basic — C embedding smoke test
# ---------------------------------------------------------------------------
add_executable(tur_eval_basic ${CMAKE_SOURCE_DIR}/tests/turi/eval-basic.c)
target_include_directories(tur_eval_basic PRIVATE ${TUR_INCLUDE_DIRS})
target_link_libraries(tur_eval_basic PRIVATE libturi)
target_compile_options(tur_eval_basic PRIVATE
    -Wall -Wextra -Wno-unused-parameter ${TUR_CSTD} -pthread
    $<$<CONFIG:Debug>:   -Og -g ${TUR_ASAN_FLAGS} -DTUR_DEBUG=1>
    $<$<CONFIG:Release>: -O2 -DNDEBUG>
)
target_link_options(tur_eval_basic PRIVATE -pthread
    $<$<CONFIG:Debug>:   ${TUR_ASAN_FLAGS}>
)
set_target_properties(tur_eval_basic PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
)

# ---------------------------------------------------------------------------
# I4 (mir-interp-tier-plan section 2.4): tur_jit_embed -- proves a plain
# libturi embedder can reach the JIT engine, and that a build without one
# still works through the interpreter fallback.  Builds in BOTH
# configurations on purpose: TUR_HAVE_JIT arrives (or does not) through
# libturi's PUBLIC compile definition, so this target is also the check that
# the capability probe propagates to real consumers.
# ---------------------------------------------------------------------------
add_executable(tur_jit_embed ${CMAKE_SOURCE_DIR}/tests/turi/jit-embed.c)
target_include_directories(tur_jit_embed PRIVATE ${TUR_INCLUDE_DIRS})
target_link_libraries(tur_jit_embed PRIVATE libturi)
target_compile_options(tur_jit_embed PRIVATE
    -Wall -Wextra -Wno-unused-parameter ${TUR_CSTD} -pthread
    $<$<CONFIG:Debug>:   -Og -g ${TUR_ASAN_FLAGS} -DTUR_DEBUG=1>
    $<$<CONFIG:Release>: -O2 -DNDEBUG>
)
target_link_options(tur_jit_embed PRIVATE -pthread
    $<$<CONFIG:Debug>:   ${TUR_ASAN_FLAGS}>
)
# The engine resolves the compiled program's external symbols with
# dlsym(RTLD_DEFAULT), so the hosting executable must export its own -- the
# same requirement `tur` satisfies with ENABLE_EXPORTS.  An embedding host
# that omits this gets link failures from the JIT'd code, not from its own
# build, which is why it is called out in jit_engine.h.
set_target_properties(tur_jit_embed PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
    ENABLE_EXPORTS TRUE
)

# ---------------------------------------------------------------------------
# web-repl-lang-switch-drops-stdlib: a `#lang` switch must keep the pinned
# stdlib preload (turi_env_pin_prelude) instead of emptying src_acc.
# ---------------------------------------------------------------------------
add_executable(tur_lang_switch_prelude ${CMAKE_SOURCE_DIR}/tests/turi/lang-switch-prelude.c)
target_include_directories(tur_lang_switch_prelude PRIVATE ${TUR_INCLUDE_DIRS})
target_link_libraries(tur_lang_switch_prelude PRIVATE libturi)
target_compile_options(tur_lang_switch_prelude PRIVATE
    -Wall -Wextra -Wno-unused-parameter ${TUR_CSTD} -pthread
    $<$<CONFIG:Debug>:   -Og -g ${TUR_ASAN_FLAGS} -DTUR_DEBUG=1>
    $<$<CONFIG:Release>: -O2 -DNDEBUG>
)
target_link_options(tur_lang_switch_prelude PRIVATE -pthread
    $<$<CONFIG:Debug>:   ${TUR_ASAN_FLAGS}>
)
set_target_properties(tur_lang_switch_prelude PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
)

# ---------------------------------------------------------------------------
# PS1 (playground-session-hygiene-plan): a failed turn must not leave the
# session unable to re-run a program (the discarded elaboration session is
# rebuilt by replaying the committed turns).
# ---------------------------------------------------------------------------
add_executable(tur_session_failure_recovery ${CMAKE_SOURCE_DIR}/tests/turi/session-failure-recovery.c)
target_include_directories(tur_session_failure_recovery PRIVATE ${TUR_INCLUDE_DIRS})
target_link_libraries(tur_session_failure_recovery PRIVATE libturi)
target_compile_options(tur_session_failure_recovery PRIVATE
    -Wall -Wextra -Wno-unused-parameter ${TUR_CSTD} -pthread
    $<$<CONFIG:Debug>:   -Og -g ${TUR_ASAN_FLAGS} -DTUR_DEBUG=1>
    $<$<CONFIG:Release>: -O2 -DNDEBUG>
)
target_link_options(tur_session_failure_recovery PRIVATE -pthread
    $<$<CONFIG:Debug>:   ${TUR_ASAN_FLAGS}>
)
set_target_properties(tur_session_failure_recovery PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
)

# PS4 (playground-session-hygiene-plan): every def* form is redefinable by a
# later turn; same-turn duplicates and stdlib-owned definitions stay refused.
add_executable(tur_session_redefinition ${CMAKE_SOURCE_DIR}/tests/turi/session-redefinition.c)
target_include_directories(tur_session_redefinition PRIVATE ${TUR_INCLUDE_DIRS})
target_link_libraries(tur_session_redefinition PRIVATE libturi)
target_compile_options(tur_session_redefinition PRIVATE
    -Wall -Wextra -Wno-unused-parameter ${TUR_CSTD} -pthread
    $<$<CONFIG:Debug>:   -Og -g ${TUR_ASAN_FLAGS} -DTUR_DEBUG=1>
    $<$<CONFIG:Release>: -O2 -DNDEBUG>
)
target_link_options(tur_session_redefinition PRIVATE -pthread
    $<$<CONFIG:Debug>:   ${TUR_ASAN_FLAGS}>
)
set_target_properties(tur_session_redefinition PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
)

# ---------------------------------------------------------------------------
# macro-system-direction-plan Stage 1: the macro-time interpreter env.
# Nested turi evaluation inside a (simulated) compile must never leave
# g_interpret_mode flipped; capabilities are denied and fuel is bounded.
# ---------------------------------------------------------------------------
add_executable(tur_macro_env_nested ${CMAKE_SOURCE_DIR}/tests/turi/macro-env-nested.c)
target_include_directories(tur_macro_env_nested PRIVATE ${TUR_INCLUDE_DIRS})
target_link_libraries(tur_macro_env_nested PRIVATE libturi)
target_compile_options(tur_macro_env_nested PRIVATE
    -Wall -Wextra -Wno-unused-parameter ${TUR_CSTD} -pthread
    $<$<CONFIG:Debug>:   -Og -g ${TUR_ASAN_FLAGS} -DTUR_DEBUG=1>
    $<$<CONFIG:Release>: -O2 -DNDEBUG>
)
target_link_options(tur_macro_env_nested PRIVATE -pthread
    $<$<CONFIG:Debug>:   ${TUR_ASAN_FLAGS}>
)
set_target_properties(tur_macro_env_nested PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
)

# ---------------------------------------------------------------------------
# map-show-keyword-key-raw-int (root cause B): the REPL auto-show path must
# show a collection's elements through their own Show instances rather than the
# int-carrier representative, which printed raw carrier integers for Sym AND
# cstr keys alike.
# ---------------------------------------------------------------------------
add_executable(tur_show_collection_elems ${CMAKE_SOURCE_DIR}/tests/turi/show-collection-elems.c)
target_include_directories(tur_show_collection_elems PRIVATE ${TUR_INCLUDE_DIRS})
target_link_libraries(tur_show_collection_elems PRIVATE libturi)
target_compile_options(tur_show_collection_elems PRIVATE
    -Wall -Wextra -Wno-unused-parameter ${TUR_CSTD} -pthread
    $<$<CONFIG:Debug>:   -Og -g ${TUR_ASAN_FLAGS} -DTUR_DEBUG=1>
    $<$<CONFIG:Release>: -O2 -DNDEBUG>
)
target_link_options(tur_show_collection_elems PRIVATE -pthread
    $<$<CONFIG:Debug>:   ${TUR_ASAN_FLAGS}>
)
set_target_properties(tur_show_collection_elems PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
)

# ---------------------------------------------------------------------------
# TR2.0 (turi-incremental-elaboration-design): cross-eval reader-macro coverage
# ---------------------------------------------------------------------------
add_executable(tur_reader_macro_cross_eval ${CMAKE_SOURCE_DIR}/tests/turi/reader-macro-cross-eval.c)
target_include_directories(tur_reader_macro_cross_eval PRIVATE ${TUR_INCLUDE_DIRS})
target_link_libraries(tur_reader_macro_cross_eval PRIVATE libturi)
target_compile_options(tur_reader_macro_cross_eval PRIVATE
    -Wall -Wextra -Wno-unused-parameter ${TUR_CSTD} -pthread
    $<$<CONFIG:Debug>:   -Og -g ${TUR_ASAN_FLAGS} -DTUR_DEBUG=1>
    $<$<CONFIG:Release>: -O2 -DNDEBUG>
)
target_link_options(tur_reader_macro_cross_eval PRIVATE -pthread
    $<$<CONFIG:Debug>:   ${TUR_ASAN_FLAGS}>
)
set_target_properties(tur_reader_macro_cross_eval PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
)

# ---------------------------------------------------------------------------
# TR2 (turi-incremental-elaboration-design): A/B differential -- the incremental
# parse path must be result-identical to the default whole-blob path
# ---------------------------------------------------------------------------
add_executable(tur_incremental_elab_diff ${CMAKE_SOURCE_DIR}/tests/turi/incremental-elab-differential.c)
target_include_directories(tur_incremental_elab_diff PRIVATE ${TUR_INCLUDE_DIRS})
target_link_libraries(tur_incremental_elab_diff PRIVATE libturi)
target_compile_options(tur_incremental_elab_diff PRIVATE
    -Wall -Wextra -Wno-unused-parameter ${TUR_CSTD} -pthread
    $<$<CONFIG:Debug>:   -Og -g ${TUR_ASAN_FLAGS} -DTUR_DEBUG=1>
    $<$<CONFIG:Release>: -O2 -DNDEBUG>
)
target_link_options(tur_incremental_elab_diff PRIVATE -pthread
    $<$<CONFIG:Debug>:   ${TUR_ASAN_FLAGS}>
)
set_target_properties(tur_incremental_elab_diff PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
)

# ---------------------------------------------------------------------------
# DEDUP-4 (gc-cycle-collection-plan): behavioral parity assertions on the
# RUNTIME copy of the GC (src/runtime/{rc,gc,rc_free_queue}.c), pinning the
# semantics the hand-written emitted copy has always had.  Compiled fixtures
# only ever exercise the emitted copy, so divergence here is otherwise silent.
# ---------------------------------------------------------------------------
add_executable(tur_gc_runtime_copy_parity ${CMAKE_SOURCE_DIR}/tests/turi/gc-runtime-copy-parity.c)
target_include_directories(tur_gc_runtime_copy_parity PRIVATE ${TUR_INCLUDE_DIRS})
target_link_libraries(tur_gc_runtime_copy_parity PRIVATE libturi)
target_compile_options(tur_gc_runtime_copy_parity PRIVATE
    -Wall -Wextra -Wno-unused-parameter ${TUR_CSTD} -pthread
    $<$<CONFIG:Debug>:   -Og -g ${TUR_ASAN_FLAGS} -DTUR_DEBUG=1>
    $<$<CONFIG:Release>: -O2 -DNDEBUG>
)
target_link_options(tur_gc_runtime_copy_parity PRIVATE -pthread
    $<$<CONFIG:Debug>:   ${TUR_ASAN_FLAGS}>
)
set_target_properties(tur_gc_runtime_copy_parity PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
)

# ---------------------------------------------------------------------------
# turi-interp-collections-libturi: Vec/Set/HAMT C-embedding parity harness
# ---------------------------------------------------------------------------
add_executable(tur_collections_embed ${CMAKE_SOURCE_DIR}/tests/turi/collections-embed.c)
target_include_directories(tur_collections_embed PRIVATE ${TUR_INCLUDE_DIRS})
target_link_libraries(tur_collections_embed PRIVATE libturi)
target_compile_options(tur_collections_embed PRIVATE
    -Wall -Wextra -Wno-unused-parameter ${TUR_CSTD} -pthread
    $<$<CONFIG:Debug>:   -Og -g ${TUR_ASAN_FLAGS} -DTUR_DEBUG=1>
    $<$<CONFIG:Release>: -O2 -DNDEBUG>
)
target_link_options(tur_collections_embed PRIVATE -pthread
    $<$<CONFIG:Debug>:   ${TUR_ASAN_FLAGS}>
)
set_target_properties(tur_collections_embed PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
)

# ---------------------------------------------------------------------------
# turi-interp-stdlib-natives-libturi: option/result/str/math C-embedding parity
# ---------------------------------------------------------------------------
add_executable(tur_stdlib_natives_embed ${CMAKE_SOURCE_DIR}/tests/turi/stdlib-natives-embed.c)
target_include_directories(tur_stdlib_natives_embed PRIVATE ${TUR_INCLUDE_DIRS})
target_link_libraries(tur_stdlib_natives_embed PRIVATE libturi)
target_compile_options(tur_stdlib_natives_embed PRIVATE
    -Wall -Wextra -Wno-unused-parameter ${TUR_CSTD} -pthread
    $<$<CONFIG:Debug>:   -Og -g ${TUR_ASAN_FLAGS} -DTUR_DEBUG=1>
    $<$<CONFIG:Release>: -O2 -DNDEBUG>
)
target_link_options(tur_stdlib_natives_embed PRIVATE -pthread
    $<$<CONFIG:Debug>:   ${TUR_ASAN_FLAGS}>
)
set_target_properties(tur_stdlib_natives_embed PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
)

# ---------------------------------------------------------------------------
# turi-env-owned-value-arena-pool-plan: env-teardown leak harness
# ---------------------------------------------------------------------------
add_executable(tur_env_teardown ${CMAKE_SOURCE_DIR}/tests/turi/env-teardown.c)
target_include_directories(tur_env_teardown PRIVATE ${TUR_INCLUDE_DIRS})
target_link_libraries(tur_env_teardown PRIVATE libturi)
target_compile_options(tur_env_teardown PRIVATE
    -Wall -Wextra -Wno-unused-parameter ${TUR_CSTD} -pthread
    $<$<CONFIG:Debug>:   -Og -g ${TUR_ASAN_FLAGS} -DTUR_DEBUG=1>
    $<$<CONFIG:Release>: -O2 -DNDEBUG>
)
target_link_options(tur_env_teardown PRIVATE -pthread
    $<$<CONFIG:Debug>:   ${TUR_ASAN_FLAGS}>
)
set_target_properties(tur_env_teardown PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
)

# ---------------------------------------------------------------------------
# turi-value-pool-scratch-promotion-plan: long-lived-env steady-state harness
# ---------------------------------------------------------------------------
add_executable(tur_env_longlived ${CMAKE_SOURCE_DIR}/tests/turi/env-longlived.c)
target_include_directories(tur_env_longlived PRIVATE ${TUR_INCLUDE_DIRS})
target_link_libraries(tur_env_longlived PRIVATE libturi)
target_compile_options(tur_env_longlived PRIVATE
    -Wall -Wextra -Wno-unused-parameter ${TUR_CSTD} -pthread
    $<$<CONFIG:Debug>:   -Og -g ${TUR_ASAN_FLAGS} -DTUR_DEBUG=1>
    $<$<CONFIG:Release>: -O2 -DNDEBUG>
)
target_link_options(tur_env_longlived PRIVATE -pthread
    $<$<CONFIG:Debug>:   ${TUR_ASAN_FLAGS}>
)
set_target_properties(tur_env_longlived PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
)

# ---------------------------------------------------------------------------
# libturi-per-embed-env-and-peripherals: Gaps 1-4 embed-API harness
# ---------------------------------------------------------------------------
add_executable(tur_embed_peripherals ${CMAKE_SOURCE_DIR}/tests/turi/embed-peripherals.c)
target_include_directories(tur_embed_peripherals PRIVATE ${TUR_INCLUDE_DIRS})
target_link_libraries(tur_embed_peripherals PRIVATE libturi)
target_compile_options(tur_embed_peripherals PRIVATE
    -Wall -Wextra -Wno-unused-parameter ${TUR_CSTD} -pthread
    $<$<CONFIG:Debug>:   -Og -g ${TUR_ASAN_FLAGS} -DTUR_DEBUG=1>
    $<$<CONFIG:Release>: -O2 -DNDEBUG>
)
target_link_options(tur_embed_peripherals PRIVATE -pthread
    $<$<CONFIG:Debug>:   ${TUR_ASAN_FLAGS}>
)
set_target_properties(tur_embed_peripherals PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
)

# ---------------------------------------------------------------------------
# SB0: tur_eval_sandbox -- sandboxed environment test harness
# ---------------------------------------------------------------------------
add_executable(tur_eval_sandbox ${CMAKE_SOURCE_DIR}/tests/turi/sandbox-eval.c)
target_include_directories(tur_eval_sandbox PRIVATE ${TUR_INCLUDE_DIRS})
target_link_libraries(tur_eval_sandbox PRIVATE libturi)
target_compile_options(tur_eval_sandbox PRIVATE
    -Wall -Wextra -Wno-unused-parameter ${TUR_CSTD} -pthread
    $<$<CONFIG:Debug>:   -Og -g ${TUR_ASAN_FLAGS} -DTUR_DEBUG=1>
    $<$<CONFIG:Release>: -O2 -DNDEBUG>
)
target_link_options(tur_eval_sandbox PRIVATE -pthread
    $<$<CONFIG:Debug>:   ${TUR_ASAN_FLAGS}>
)
set_target_properties(tur_eval_sandbox PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
)

# ---------------------------------------------------------------------------
# RP2: hand-rolled FFI dispatcher unit test
# ---------------------------------------------------------------------------
# The dispatchers live in tur_core via runtime/ffi_dispatch.c; the test
# is standalone (no other tur_core symbols needed) but easiest to link
# against the same object library so include paths and warning flags
# match the rest of the runtime.
add_executable(tur_ffi_dispatch_unit
    ${CMAKE_SOURCE_DIR}/tests/runtime/ffi-dispatch-unit.c
    $<TARGET_OBJECTS:tur_core>
)
target_include_directories(tur_ffi_dispatch_unit PRIVATE ${TUR_INCLUDE_DIRS})
target_compile_options(tur_ffi_dispatch_unit PRIVATE
    -Wall -Wextra -Wno-unused-parameter ${TUR_CSTD} -pthread
    $<$<CONFIG:Debug>:   -Og -g ${TUR_ASAN_FLAGS} -DTUR_DEBUG=1>
    $<$<CONFIG:Release>: -O2 -DNDEBUG>
)
target_link_options(tur_ffi_dispatch_unit PRIVATE -pthread
    $<$<CONFIG:Debug>:   ${TUR_ASAN_FLAGS}>
)
target_link_libraries(tur_ffi_dispatch_unit PRIVATE m)
if(HAVE_EDITLINE)
    target_link_libraries(tur_ffi_dispatch_unit PRIVATE ${LIBEDIT_LIB})
    target_compile_definitions(tur_ffi_dispatch_unit PRIVATE TURI_HAVE_EDITLINE=1)
endif()
set_target_properties(tur_ffi_dispatch_unit PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
)

# ---------------------------------------------------------------------------
# SX1 (solver-extension-plan): unit tests for the backtrackable-state trail.
# Links the runtime source directly rather than tur_core -- the trail has no
# compiler dependencies, and keeping the target that narrow is what lets a
# failure here mean exactly one thing.
# ---------------------------------------------------------------------------
add_executable(tur_trail
    ${CMAKE_SOURCE_DIR}/tests/unit/trail.c
    runtime/trail.c
)
target_include_directories(tur_trail PRIVATE ${TUR_INCLUDE_DIRS})
target_compile_options(tur_trail PRIVATE
    -Wall -Wextra -Wno-unused-parameter ${TUR_CSTD} -pthread
    $<$<CONFIG:Debug>:   -Og -g ${TUR_ASAN_FLAGS} -DTUR_DEBUG=1>
    $<$<CONFIG:Release>: -O2 -DNDEBUG>
)
target_link_options(tur_trail PRIVATE -pthread
    $<$<CONFIG:Debug>:   ${TUR_ASAN_FLAGS}>
)
set_target_properties(tur_trail PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
)
# Registers because the top-level CMakeLists calls enable_testing() BEFORE
# add_subdirectory(src) (it did not always: see
# src-cmakelists-add-test-never-registers, archived).
# tests/check-ctest-registration.sh checks that every add_test here shows
# up in `ctest -N`.
add_test(NAME tur_trail COMMAND tur_trail)

# ---------------------------------------------------------------------------
# security-audit-plan M-3: LSP/DAP Content-Length framing.  Links lsp_io.c
# alone -- the transport has no other dependency.  POSIX only: the read-loop
# half drives the framing over pipe(2).
# ---------------------------------------------------------------------------
if(NOT WIN32)
    add_executable(tur_lsp_io_unit
        ${CMAKE_SOURCE_DIR}/tests/unit/lsp_io.c
        lsp/lsp_io.c
    )
    target_include_directories(tur_lsp_io_unit PRIVATE ${TUR_INCLUDE_DIRS})
    target_compile_options(tur_lsp_io_unit PRIVATE
        -Wall -Wextra -Wno-unused-parameter ${TUR_CSTD}
        $<$<CONFIG:Debug>:   -Og -g ${TUR_ASAN_FLAGS} -DTUR_DEBUG=1>
        $<$<CONFIG:Release>: -O2 -DNDEBUG>
    )
    target_link_options(tur_lsp_io_unit PRIVATE
        $<$<CONFIG:Debug>:   ${TUR_ASAN_FLAGS}>
    )
    set_target_properties(tur_lsp_io_unit PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
    )
    add_test(NAME tur_lsp_io_unit COMMAND tur_lsp_io_unit)
endif()

# ---------------------------------------------------------------------------
# RT2/S0--S3: refinement normalized-VC + decision-procedure unit test
# (docs/archive/refinement-types-plan.md).  Exercises hash-consing,
# constant folding, each in-house stage, the cube expansion, the bounded
# counterexample search, and the SMT-LIB serializer -- including the
# "must NOT prove" direction, which is where the soundness invariant lives.
# ---------------------------------------------------------------------------
# pkg_hash_dir -- the spice lockfile's integrity hash.  Links tur_core because
# the walk lives with the rest of the package manager.
add_executable(tur_pkg_hash
    ${CMAKE_SOURCE_DIR}/tests/unit/pkg_hash.c
    $<TARGET_OBJECTS:tur_core>
)
target_include_directories(tur_pkg_hash PRIVATE ${TUR_INCLUDE_DIRS})
target_compile_options(tur_pkg_hash PRIVATE
    -Wall -Wextra -Wno-unused-parameter ${TUR_CSTD} -pthread
    $<$<CONFIG:Debug>:   -Og -g ${TUR_ASAN_FLAGS} -DTUR_DEBUG=1>
    $<$<CONFIG:Release>: -O2 -DNDEBUG>
)
target_link_options(tur_pkg_hash PRIVATE -pthread
    $<$<CONFIG:Debug>:   ${TUR_ASAN_FLAGS}>
)
target_link_libraries(tur_pkg_hash PRIVATE m)
if(HAVE_EDITLINE)
    target_link_libraries(tur_pkg_hash PRIVATE ${LIBEDIT_LIB})
    target_compile_definitions(tur_pkg_hash PRIVATE TURI_HAVE_EDITLINE=1)
endif()
set_target_properties(tur_pkg_hash PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
)

add_executable(tur_refine_solver
    ${CMAKE_SOURCE_DIR}/tests/unit/refine_solver.c
    $<TARGET_OBJECTS:tur_core>
)
target_include_directories(tur_refine_solver PRIVATE ${TUR_INCLUDE_DIRS})
target_compile_options(tur_refine_solver PRIVATE
    -Wall -Wextra -Wno-unused-parameter ${TUR_CSTD} -pthread
    $<$<CONFIG:Debug>:   -Og -g ${TUR_ASAN_FLAGS} -DTUR_DEBUG=1>
    $<$<CONFIG:Release>: -O2 -DNDEBUG>
)
target_link_options(tur_refine_solver PRIVATE -pthread
    $<$<CONFIG:Debug>:   ${TUR_ASAN_FLAGS}>
)
target_link_libraries(tur_refine_solver PRIVATE m)
if(HAVE_EDITLINE)
    target_link_libraries(tur_refine_solver PRIVATE ${LIBEDIT_LIB})
    target_compile_definitions(tur_refine_solver PRIVATE TURI_HAVE_EDITLINE=1)
endif()
set_target_properties(tur_refine_solver PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
)

# ---------------------------------------------------------------------------
# Z3 retirement: replay a LABELLED SMT-LIB2 corpus against the in-house chain,
# with no solver dependency of its own.  The live cross-check (refine_libz3.c)
# only ever existed on a dev build with a system Z3; what had to survive its
# deletion -- which happened in 0.32.5 -- is a corpus whose labels are DATA in
# the repo.  This target IS that surviving regression, and is now the whole of
# the standing solver-soundness check, so it is built unconditionally.
# See tests/unit/refine_corpus.c and tests/corpus/smtlib/README.md.
#
# "Unconditionally" means "on every platform that can run it".  The harness
# sandboxes each corpus case behind fork() + waitpid(WNOHANG), so a case that
# crashes or spins is contained and reported rather than taking the run down --
# that isolation IS the harness, not an incidental detail, and there is no
# fork() on Windows to build it on.  Rather than ship a Windows variant with
# the containment quietly removed (a corpus that hangs instead of reporting is
# worse than one that is absent), the target is POSIX-only.  The solver it
# tests is pure portable C and is covered on Linux/macOS in CI.
# ---------------------------------------------------------------------------
if(NOT WIN32)
add_executable(tur_refine_corpus
    ${CMAKE_SOURCE_DIR}/tests/unit/refine_corpus.c
    $<TARGET_OBJECTS:tur_core>
)
target_include_directories(tur_refine_corpus PRIVATE ${TUR_INCLUDE_DIRS})
target_compile_options(tur_refine_corpus PRIVATE
    -Wall -Wextra -Wno-unused-parameter ${TUR_CSTD} -pthread
    $<$<CONFIG:Debug>:   -Og -g ${TUR_ASAN_FLAGS} -DTUR_DEBUG=1>
    $<$<CONFIG:Release>: -O2 -DNDEBUG>
)
target_link_options(tur_refine_corpus PRIVATE -pthread
    $<$<CONFIG:Debug>:   ${TUR_ASAN_FLAGS}>
)
target_link_libraries(tur_refine_corpus PRIVATE m)
if(HAVE_EDITLINE)
    target_link_libraries(tur_refine_corpus PRIVATE ${LIBEDIT_LIB})
    target_compile_definitions(tur_refine_corpus PRIVATE TURI_HAVE_EDITLINE=1)
endif()
set_target_properties(tur_refine_corpus PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
)
endif()  # NOT WIN32

# ---------------------------------------------------------------------------
# RT3: the VC-level differential fuzzer (tests/unit/refine_fuzz.c) compared the
# in-house solver against Z3 and was deleted with the oracle in 0.32.5.  Its
# replacement is not a solver fuzzer at all: tests/refine-fuzz-src.py is a
# SOURCE-level differential fuzzer that compiles generated programs twice
# (gate off vs on) and needs no oracle, which is the coverage that actually
# mattered -- both known soundness bugs lived in the source->VC encoder, ABOVE
# where the VC-level fuzzer started.
# ---------------------------------------------------------------------------

# ---------------------------------------------------------------------------
# UC-2: user-level experiments file reader unit test
# (docs/archive/history/user-config-experiments-plan.md).  Exercises
# experiments_read_user_config against a temp $XDG_CONFIG_HOME.
# ---------------------------------------------------------------------------
add_executable(tur_experiments_user_config
    ${CMAKE_SOURCE_DIR}/tests/unit/experiments_user_config.c
    $<TARGET_OBJECTS:tur_core>
)
target_include_directories(tur_experiments_user_config PRIVATE ${TUR_INCLUDE_DIRS})
target_compile_options(tur_experiments_user_config PRIVATE
    -Wall -Wextra -Wno-unused-parameter ${TUR_CSTD} -pthread
    $<$<CONFIG:Debug>:   -Og -g ${TUR_ASAN_FLAGS} -DTUR_DEBUG=1>
    $<$<CONFIG:Release>: -O2 -DNDEBUG>
)
target_link_options(tur_experiments_user_config PRIVATE -pthread
    $<$<CONFIG:Debug>:   ${TUR_ASAN_FLAGS}>
)
target_link_libraries(tur_experiments_user_config PRIVATE m)
if(HAVE_EDITLINE)
    target_link_libraries(tur_experiments_user_config PRIVATE ${LIBEDIT_LIB})
    target_compile_definitions(tur_experiments_user_config PRIVATE TURI_HAVE_EDITLINE=1)
endif()
set_target_properties(tur_experiments_user_config PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
)

# ---------------------------------------------------------------------------
# ACB: emit_carrier_bridge unit test
# ---------------------------------------------------------------------------
add_executable(tur_codegen_carrier_bridge
    ${CMAKE_SOURCE_DIR}/tests/compiler/test_emit_carrier_bridge.c
    $<TARGET_OBJECTS:tur_core>
)
target_include_directories(tur_codegen_carrier_bridge PRIVATE ${TUR_INCLUDE_DIRS})
target_compile_options(tur_codegen_carrier_bridge PRIVATE
    -Wall -Wextra -Wno-unused-parameter ${TUR_CSTD} -pthread
    $<$<CONFIG:Debug>:   -Og -g ${TUR_ASAN_FLAGS} -DTUR_DEBUG=1>
    $<$<CONFIG:Release>: -O2 -DNDEBUG>
)
target_link_options(tur_codegen_carrier_bridge PRIVATE -pthread
    $<$<CONFIG:Debug>:   ${TUR_ASAN_FLAGS}>
)
target_link_libraries(tur_codegen_carrier_bridge PRIVATE m)
if(HAVE_EDITLINE)
    target_link_libraries(tur_codegen_carrier_bridge PRIVATE ${LIBEDIT_LIB})
    target_compile_definitions(tur_codegen_carrier_bridge PRIVATE TURI_HAVE_EDITLINE=1)
endif()
set_target_properties(tur_codegen_carrier_bridge PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
)

# ---------------------------------------------------------------------------
# Semver + :tur-version range unit test (pkg_semver_* were dead code carrying
# two bugs; :tur-version is the first caller).
# ---------------------------------------------------------------------------
add_executable(tur_semver_range_unit
    ${CMAKE_SOURCE_DIR}/tests/compiler/test_semver_range.c
    $<TARGET_OBJECTS:tur_core>
)
target_include_directories(tur_semver_range_unit PRIVATE ${TUR_INCLUDE_DIRS})
target_compile_options(tur_semver_range_unit PRIVATE
    -Wall -Wextra -Wno-unused-parameter ${TUR_CSTD} -pthread
    $<$<CONFIG:Debug>:   -Og -g ${TUR_ASAN_FLAGS} -DTUR_DEBUG=1>
    $<$<CONFIG:Release>: -O2 -DNDEBUG>
)
target_link_options(tur_semver_range_unit PRIVATE -pthread
    $<$<CONFIG:Debug>:   ${TUR_ASAN_FLAGS}>
)
target_link_libraries(tur_semver_range_unit PRIVATE m)
if(HAVE_EDITLINE)
    target_link_libraries(tur_semver_range_unit PRIVATE ${LIBEDIT_LIB})
    target_compile_definitions(tur_semver_range_unit PRIVATE TURI_HAVE_EDITLINE=1)
endif()
set_target_properties(tur_semver_range_unit PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
)

# ---------------------------------------------------------------------------
# Theme E (TP3): arbitrary-arity Kind helper unit test
# ---------------------------------------------------------------------------
add_executable(tur_kind_arity_unit
    ${CMAKE_SOURCE_DIR}/tests/compiler/test_kind_arity.c
    $<TARGET_OBJECTS:tur_core>
)
target_include_directories(tur_kind_arity_unit PRIVATE ${TUR_INCLUDE_DIRS})
target_compile_options(tur_kind_arity_unit PRIVATE
    -Wall -Wextra -Wno-unused-parameter ${TUR_CSTD} -pthread
    $<$<CONFIG:Debug>:   -Og -g ${TUR_ASAN_FLAGS} -DTUR_DEBUG=1>
    $<$<CONFIG:Release>: -O2 -DNDEBUG>
)
target_link_options(tur_kind_arity_unit PRIVATE -pthread
    $<$<CONFIG:Debug>:   ${TUR_ASAN_FLAGS}>
)
target_link_libraries(tur_kind_arity_unit PRIVATE m)
if(HAVE_EDITLINE)
    target_link_libraries(tur_kind_arity_unit PRIVATE ${LIBEDIT_LIB})
    target_compile_definitions(tur_kind_arity_unit PRIVATE TURI_HAVE_EDITLINE=1)
endif()
set_target_properties(tur_kind_arity_unit PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
)

# ---------------------------------------------------------------------------
# structdef-retirement DS-D: the :heap typed-pointer ABI lowering unit test
# (tur_heap_abi_unit) pinned type_c_name over a hand-built StructDef.  StructDef
# has been deleted; the :heap typed-pointer lowering now lives on the record-ADT
# monomorph/app path and is covered end-to-end by the fixture suite, so the
# isolated StructDef unit pin was retired with the type.
# ---------------------------------------------------------------------------

# ---------------------------------------------------------------------------
# Variadic HKT rows (Direction 1): kind-level `List Type` (KIND_TYPEROW) unit test
# ---------------------------------------------------------------------------
add_executable(tur_kind_row_unit
    ${CMAKE_SOURCE_DIR}/tests/compiler/test_kind_row.c
    $<TARGET_OBJECTS:tur_core>
)
target_include_directories(tur_kind_row_unit PRIVATE ${TUR_INCLUDE_DIRS})
target_compile_options(tur_kind_row_unit PRIVATE
    -Wall -Wextra -Wno-unused-parameter ${TUR_CSTD} -pthread
    $<$<CONFIG:Debug>:   -Og -g ${TUR_ASAN_FLAGS} -DTUR_DEBUG=1>
    $<$<CONFIG:Release>: -O2 -DNDEBUG>
)
target_link_options(tur_kind_row_unit PRIVATE -pthread
    $<$<CONFIG:Debug>:   ${TUR_ASAN_FLAGS}>
)
target_link_libraries(tur_kind_row_unit PRIVATE m)
if(HAVE_EDITLINE)
    target_link_libraries(tur_kind_row_unit PRIVATE ${LIBEDIT_LIB})
    target_compile_definitions(tur_kind_row_unit PRIVATE TURI_HAVE_EDITLINE=1)
endif()
set_target_properties(tur_kind_row_unit PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
)

# ---------------------------------------------------------------------------
# Variadic HKT rows (Layer 2): TY_TYPEROW type-variant unit test
# ---------------------------------------------------------------------------
add_executable(tur_typerow_unit
    ${CMAKE_SOURCE_DIR}/tests/compiler/test_typerow.c
    $<TARGET_OBJECTS:tur_core>
)
target_include_directories(tur_typerow_unit PRIVATE ${TUR_INCLUDE_DIRS})
target_compile_options(tur_typerow_unit PRIVATE
    -Wall -Wextra -Wno-unused-parameter ${TUR_CSTD} -pthread
    $<$<CONFIG:Debug>:   -Og -g ${TUR_ASAN_FLAGS} -DTUR_DEBUG=1>
    $<$<CONFIG:Release>: -O2 -DNDEBUG>
)
target_link_options(tur_typerow_unit PRIVATE -pthread
    $<$<CONFIG:Debug>:   ${TUR_ASAN_FLAGS}>
)
target_link_libraries(tur_typerow_unit PRIVATE m)
if(HAVE_EDITLINE)
    target_link_libraries(tur_typerow_unit PRIVATE ${LIBEDIT_LIB})
    target_compile_definitions(tur_typerow_unit PRIVATE TURI_HAVE_EDITLINE=1)
endif()
set_target_properties(tur_typerow_unit PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
)

# ---------------------------------------------------------------------------
# TS2: compiler-side EX_REINTERPRET codegen test
# ---------------------------------------------------------------------------
add_executable(tur_codegen_reinterpret
    ${CMAKE_SOURCE_DIR}/tests/compiler/reinterpret-codegen.c
    $<TARGET_OBJECTS:tur_core>
)
target_include_directories(tur_codegen_reinterpret PRIVATE ${TUR_INCLUDE_DIRS})
target_compile_options(tur_codegen_reinterpret PRIVATE
    -Wall -Wextra -Wno-unused-parameter ${TUR_CSTD} -pthread
    $<$<CONFIG:Debug>:   -Og -g ${TUR_ASAN_FLAGS} -DTUR_DEBUG=1>
    $<$<CONFIG:Release>: -O2 -DNDEBUG>
)
target_link_options(tur_codegen_reinterpret PRIVATE -pthread
    $<$<CONFIG:Debug>:   ${TUR_ASAN_FLAGS}>
)
target_link_libraries(tur_codegen_reinterpret PRIVATE m)
if(HAVE_EDITLINE)
    target_link_libraries(tur_codegen_reinterpret PRIVATE ${LIBEDIT_LIB})
    target_compile_definitions(tur_codegen_reinterpret PRIVATE TURI_HAVE_EDITLINE=1)
endif()
set_target_properties(tur_codegen_reinterpret PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
)

# ---------------------------------------------------------------------------
# TY4: lifetime elision + outlives-cycle unit test
# ---------------------------------------------------------------------------
add_executable(tur_lifetime_unit
    ${CMAKE_SOURCE_DIR}/tests/lifetime_unit.c
    $<TARGET_OBJECTS:tur_core>
)
target_include_directories(tur_lifetime_unit PRIVATE ${TUR_INCLUDE_DIRS})
target_compile_options(tur_lifetime_unit PRIVATE
    -Wall -Wextra -Wno-unused-parameter ${TUR_CSTD} -pthread
    $<$<CONFIG:Debug>:   -Og -g ${TUR_ASAN_FLAGS} -DTUR_DEBUG=1>
    $<$<CONFIG:Release>: -O2 -DNDEBUG>
)
target_link_options(tur_lifetime_unit PRIVATE -pthread
    $<$<CONFIG:Debug>:   ${TUR_ASAN_FLAGS}>
)
target_link_libraries(tur_lifetime_unit PRIVATE m)
if(HAVE_EDITLINE)
    target_link_libraries(tur_lifetime_unit PRIVATE ${LIBEDIT_LIB})
    target_compile_definitions(tur_lifetime_unit PRIVATE TURI_HAVE_EDITLINE=1)
endif()
set_target_properties(tur_lifetime_unit PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
)

# CPS4 (cps-transform-plan): heap-reified continuation + trampoline runtime unit.
# Compiles cps_rt.c directly (no tur_core link) so leak detection validates only
# the CPS runtime -- it is self-contained and frees everything it allocates.
add_executable(tur_cps_rt_unit
    ${CMAKE_SOURCE_DIR}/tests/cps_rt_unit.c
    ${CMAKE_SOURCE_DIR}/src/runtime/cps_rt.c
)
target_include_directories(tur_cps_rt_unit PRIVATE ${TUR_INCLUDE_DIRS})
target_compile_options(tur_cps_rt_unit PRIVATE
    -Wall -Wextra -Wno-unused-parameter ${TUR_CSTD} -pthread
    $<$<CONFIG:Debug>:   -Og -g ${TUR_ASAN_FLAGS} -DTUR_DEBUG=1>
    $<$<CONFIG:Release>: -O2 -DNDEBUG>
)
target_link_options(tur_cps_rt_unit PRIVATE -pthread
    $<$<CONFIG:Debug>:   ${TUR_ASAN_FLAGS}>
)
target_link_libraries(tur_cps_rt_unit PRIVATE m)
if(HAVE_EDITLINE)
    target_link_libraries(tur_cps_rt_unit PRIVATE ${LIBEDIT_LIB})
    target_compile_definitions(tur_cps_rt_unit PRIVATE TURI_HAVE_EDITLINE=1)
endif()
set_target_properties(tur_cps_rt_unit PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
)
# Escaping a filesystem path for embedding in generated Turmeric source.
# Header-only, so this links nothing -- but the bug it guards took down the
# whole stdlib prelude on an installed tur.exe (every backslash of a Windows
# path read as a string escape), and the in-tree build could not see it
# because the stdlib root resolves to the literal "stdlib" there.  The cases
# are reachable on POSIX too, so the Linux legs enforce it.
add_executable(tur_source_literal_unit
    ${CMAKE_SOURCE_DIR}/tests/source_literal_unit.c
)
target_include_directories(tur_source_literal_unit PRIVATE ${TUR_INCLUDE_DIRS})
target_compile_options(tur_source_literal_unit PRIVATE
    -Wall -Wextra -Wno-unused-parameter ${TUR_CSTD}
    $<$<CONFIG:Debug>:   -Og -g ${TUR_ASAN_FLAGS} -DTUR_DEBUG=1>
    $<$<CONFIG:Release>: -O2 -DNDEBUG>
)
target_link_options(tur_source_literal_unit PRIVATE
    $<$<CONFIG:Debug>:   ${TUR_ASAN_FLAGS}>
)
set_target_properties(tur_source_literal_unit PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
)


# CPS5 (cps-transform-plan): multi-prompt delimited-control machine unit.
# Reactor source-slot recycling: that it happens, and that a slot freed inside
# a callback is not reusable until the next poll.  Links libturi rather than
# naming sources: reactor.c reaches into the fiber layer, so the source list
# would be most of the runtime anyway.
add_executable(tur_reactor_slot_reuse_unit
    ${CMAKE_SOURCE_DIR}/tests/reactor_slot_reuse_unit.c
)
target_include_directories(tur_reactor_slot_reuse_unit PRIVATE ${TUR_INCLUDE_DIRS})
target_compile_options(tur_reactor_slot_reuse_unit PRIVATE
    -Wall -Wextra -Wno-unused-parameter ${TUR_CSTD} -pthread
    $<$<CONFIG:Debug>:   -Og -g ${TUR_ASAN_FLAGS} -DTUR_DEBUG=1>
    $<$<CONFIG:Release>: -O2 -DNDEBUG>
)
target_link_options(tur_reactor_slot_reuse_unit PRIVATE -pthread
    $<$<CONFIG:Debug>:   ${TUR_ASAN_FLAGS}>
)
target_link_libraries(tur_reactor_slot_reuse_unit PRIVATE libturi m)
set_target_properties(tur_reactor_slot_reuse_unit PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
)
if(WIN32)
    target_link_libraries(tur_reactor_slot_reuse_unit PRIVATE ws2_32)
endif()

# tur_reactor_linger_close: a reply sent before the request is read still
# reaches the peer (no RST), and the linger ends on the peer's close, on its
# timeout, or at reactor teardown.
add_executable(tur_reactor_linger_unit
    ${CMAKE_SOURCE_DIR}/tests/reactor_linger_unit.c
)
target_include_directories(tur_reactor_linger_unit PRIVATE ${TUR_INCLUDE_DIRS})
target_compile_options(tur_reactor_linger_unit PRIVATE
    -Wall -Wextra -Wno-unused-parameter ${TUR_CSTD} -pthread
    $<$<CONFIG:Debug>:   -Og -g ${TUR_ASAN_FLAGS} -DTUR_DEBUG=1>
    $<$<CONFIG:Release>: -O2 -DNDEBUG>
)
target_link_options(tur_reactor_linger_unit PRIVATE -pthread
    $<$<CONFIG:Debug>:   ${TUR_ASAN_FLAGS}>
)
target_link_libraries(tur_reactor_linger_unit PRIVATE libturi m)
set_target_properties(tur_reactor_linger_unit PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
)
if(WIN32)
    target_link_libraries(tur_reactor_linger_unit PRIVATE ws2_32)
endif()

add_executable(tur_cps_prompt_unit
    ${CMAKE_SOURCE_DIR}/tests/cps_prompt_unit.c
    ${CMAKE_SOURCE_DIR}/src/runtime/cps_prompt.c
)
target_include_directories(tur_cps_prompt_unit PRIVATE ${TUR_INCLUDE_DIRS})
target_compile_options(tur_cps_prompt_unit PRIVATE
    -Wall -Wextra -Wno-unused-parameter ${TUR_CSTD} -pthread
    $<$<CONFIG:Debug>:   -Og -g ${TUR_ASAN_FLAGS} -DTUR_DEBUG=1>
    $<$<CONFIG:Release>: -O2 -DNDEBUG>
)
target_link_options(tur_cps_prompt_unit PRIVATE -pthread
    $<$<CONFIG:Debug>:   ${TUR_ASAN_FLAGS}>
)
target_link_libraries(tur_cps_prompt_unit PRIVATE m)
if(HAVE_EDITLINE)
    target_link_libraries(tur_cps_prompt_unit PRIVATE ${LIBEDIT_LIB})
    target_compile_definitions(tur_cps_prompt_unit PRIVATE TURI_HAVE_EDITLINE=1)
endif()
set_target_properties(tur_cps_prompt_unit PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
)

# SHA-256 unit test -- known-answer vectors plus the streaming property that
# pkg_hash_dir's multi-update tree walk depends on.
add_executable(tur_sha256_unit
    ${CMAKE_SOURCE_DIR}/tests/sha256_unit.c
    ${CMAKE_SOURCE_DIR}/src/runtime/sha256.c
)
target_include_directories(tur_sha256_unit PRIVATE ${TUR_INCLUDE_DIRS})
target_compile_options(tur_sha256_unit PRIVATE
    -Wall -Wextra -Wno-unused-parameter ${TUR_CSTD} -pthread
    $<$<CONFIG:Debug>:   -Og -g ${TUR_ASAN_FLAGS} -DTUR_DEBUG=1>
    $<$<CONFIG:Release>: -O2 -DNDEBUG>
)
target_link_options(tur_sha256_unit PRIVATE -pthread
    $<$<CONFIG:Debug>:   ${TUR_ASAN_FLAGS}>
)
set_target_properties(tur_sha256_unit PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
)

# AI1 (application-image-dumps-plan): image header codec unit test.
add_executable(tur_image_unit
    ${CMAKE_SOURCE_DIR}/tests/image_unit.c
    ${CMAKE_SOURCE_DIR}/src/runtime/image.c
    ${CMAKE_SOURCE_DIR}/src/runtime/sha256.c
)
target_include_directories(tur_image_unit PRIVATE ${TUR_INCLUDE_DIRS})
target_compile_options(tur_image_unit PRIVATE
    -Wall -Wextra -Wno-unused-parameter ${TUR_CSTD} -pthread
    $<$<CONFIG:Debug>:   -Og -g ${TUR_ASAN_FLAGS} -DTUR_DEBUG=1>
    $<$<CONFIG:Release>: -O2 -DNDEBUG>
)
target_link_options(tur_image_unit PRIVATE -pthread
    $<$<CONFIG:Debug>:   ${TUR_ASAN_FLAGS}>
)
target_link_libraries(tur_image_unit PRIVATE m)
if(HAVE_EDITLINE)
    target_link_libraries(tur_image_unit PRIVATE ${LIBEDIT_LIB})
    target_compile_definitions(tur_image_unit PRIVATE TURI_HAVE_EDITLINE=1)
endif()
set_target_properties(tur_image_unit PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
)

# ---------------------------------------------------------------------------
# LSP docstring-scanner unit test
# ---------------------------------------------------------------------------
add_executable(tur_docscanner_unit
    ${CMAKE_SOURCE_DIR}/tests/lsp/docscanner_test.c
    ${CMAKE_SOURCE_DIR}/src/lsp/lsp_docs.c
    ${CMAKE_SOURCE_DIR}/src/lsp/lsp_util.c
)
target_include_directories(tur_docscanner_unit PRIVATE ${TUR_INCLUDE_DIRS})
target_compile_options(tur_docscanner_unit PRIVATE
    -Wall -Wextra -Wno-unused-parameter ${TUR_CSTD}
    $<$<CONFIG:Debug>:   -Og -g ${TUR_ASAN_FLAGS} -DTUR_DEBUG=1>
    $<$<CONFIG:Release>: -O2 -DNDEBUG>
)
target_link_options(tur_docscanner_unit PRIVATE
    $<$<CONFIG:Debug>:   ${TUR_ASAN_FLAGS}>
)
set_target_properties(tur_docscanner_unit PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
)

# ---------------------------------------------------------------------------
# LSP session unit test (try-turmeric-lsp-plan, §4)
#
# Links tur_core -- which is everything except main.c -- and supplies its own
# tur_collect_symbols. That is not a workaround for the link: it is the point.
# A fixed stub index makes completion ordering, path remapping, and last-good
# retention assertable, where the real compiler would tie every assertion to
# whatever the stdlib currently exports.
# ---------------------------------------------------------------------------
add_executable(tur_lsp_session_unit
    ${CMAKE_SOURCE_DIR}/tests/lsp/session_test.c
    $<TARGET_OBJECTS:tur_core>
)
target_include_directories(tur_lsp_session_unit PRIVATE ${TUR_INCLUDE_DIRS})
target_compile_options(tur_lsp_session_unit PRIVATE
    -Wall -Wextra -Wno-unused-parameter ${TUR_CSTD}
    $<$<CONFIG:Debug>:   -Og -g ${TUR_ASAN_FLAGS} -DTUR_DEBUG=1>
    $<$<CONFIG:Release>: -O2 -DNDEBUG>
)
target_link_options(tur_lsp_session_unit PRIVATE
    $<$<CONFIG:Debug>:   ${TUR_ASAN_FLAGS}>
)
target_compile_options(tur_lsp_session_unit PRIVATE -pthread)
target_link_options(tur_lsp_session_unit PRIVATE -pthread)
target_link_libraries(tur_lsp_session_unit PRIVATE m ${CMAKE_DL_LIBS})
# tur_core carries repl.c, whose line editing is libedit's whenever libedit was
# found. Omitting this links clean on a box without libedit and fails to link
# on one with it -- the wrong way round for a check meant to catch things early.
if(HAVE_EDITLINE)
    target_link_libraries(tur_lsp_session_unit PRIVATE ${LIBEDIT_LIB})
    target_compile_definitions(tur_lsp_session_unit PRIVATE TURI_HAVE_EDITLINE=1)
endif()
set_target_properties(tur_lsp_session_unit PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
)

# ---------------------------------------------------------------------------
# WASM LSP analysis-backend test (try-turmeric-lsp-plan, L0)
#
# src/web/wasm_lsp.c is the front end the browser uses in place of main.c's
# compile_to_c.  Nothing in it is actually wasm-specific -- the Emscripten
# build differs only in where the stdlib is mounted, which is a TUR_STDLIB_DIR
# lookup -- so it runs here against the in-tree stdlib and the real compiler.
# Without this the file would only ever execute somewhere nothing can check it.
# ---------------------------------------------------------------------------
add_executable(tur_lsp_wasm_backend_unit
    ${CMAKE_SOURCE_DIR}/tests/lsp/wasm_backend_test.c
    ${CMAKE_SOURCE_DIR}/src/web/wasm_lsp.c
    $<TARGET_OBJECTS:tur_core>
)
target_include_directories(tur_lsp_wasm_backend_unit PRIVATE ${TUR_INCLUDE_DIRS})
target_compile_options(tur_lsp_wasm_backend_unit PRIVATE
    -Wall -Wextra -Wno-unused-parameter ${TUR_CSTD}
    -pthread
    $<$<CONFIG:Debug>:   -Og -g ${TUR_ASAN_FLAGS} -DTUR_DEBUG=1>
    $<$<CONFIG:Release>: -O2 -DNDEBUG>
)
target_link_options(tur_lsp_wasm_backend_unit PRIVATE
    -pthread
    $<$<CONFIG:Debug>:   ${TUR_ASAN_FLAGS}>
)
target_link_libraries(tur_lsp_wasm_backend_unit PRIVATE m ${CMAKE_DL_LIBS})
if(HAVE_EDITLINE)
    target_link_libraries(tur_lsp_wasm_backend_unit PRIVATE ${LIBEDIT_LIB})
    target_compile_definitions(tur_lsp_wasm_backend_unit PRIVATE TURI_HAVE_EDITLINE=1)
endif()
set_target_properties(tur_lsp_wasm_backend_unit PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
)

# ---------------------------------------------------------------------------
# try-turmeric-lang-toggle-plan T0/T1: the browser's language-mode entry
# points (turi_wasm_set_lang / get_lang / lang_registry), run natively via
# libturi_wasm for the same reason as the analysis backend above -- nothing
# in wasm_glue.c is actually wasm-specific, and inside a wasm module nothing
# in CI can look at it.
# ---------------------------------------------------------------------------
add_executable(tur_wasm_glue_lang_unit
    ${CMAKE_SOURCE_DIR}/tests/wasm_glue_lang_unit.c
)
target_include_directories(tur_wasm_glue_lang_unit PRIVATE ${TUR_INCLUDE_DIRS})
target_compile_options(tur_wasm_glue_lang_unit PRIVATE
    -Wall -Wextra -Wno-unused-parameter ${TUR_CSTD}
    -pthread
    $<$<CONFIG:Debug>:   -Og -g ${TUR_ASAN_FLAGS} -DTUR_DEBUG=1>
    $<$<CONFIG:Release>: -O2 -DNDEBUG>
)
target_link_options(tur_wasm_glue_lang_unit PRIVATE
    -pthread
    $<$<CONFIG:Debug>:   ${TUR_ASAN_FLAGS}>
)
target_link_libraries(tur_wasm_glue_lang_unit PRIVATE libturi_wasm m ${CMAKE_DL_LIBS})
if(HAVE_EDITLINE)
    target_link_libraries(tur_wasm_glue_lang_unit PRIVATE ${LIBEDIT_LIB})
    target_compile_definitions(tur_wasm_glue_lang_unit PRIVATE TURI_HAVE_EDITLINE=1)
endif()
set_target_properties(tur_wasm_glue_lang_unit PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
)

# playground-session-hygiene-plan: the playground's eval session driven the way
# Try Turmeric drives it -- doc-panel lookups, `(doc ...)`, Run twice --
# natively via libturi_wasm, like the lang unit above.
add_executable(tur_wasm_glue_session_unit
    ${CMAKE_SOURCE_DIR}/tests/wasm_glue_session_unit.c
)
target_include_directories(tur_wasm_glue_session_unit PRIVATE ${TUR_INCLUDE_DIRS})
target_compile_options(tur_wasm_glue_session_unit PRIVATE
    -Wall -Wextra -Wno-unused-parameter ${TUR_CSTD}
    -pthread
    $<$<CONFIG:Debug>:   -Og -g ${TUR_ASAN_FLAGS} -DTUR_DEBUG=1>
    $<$<CONFIG:Release>: -O2 -DNDEBUG>
)
target_link_options(tur_wasm_glue_session_unit PRIVATE
    -pthread
    $<$<CONFIG:Debug>:   ${TUR_ASAN_FLAGS}>
)
target_link_libraries(tur_wasm_glue_session_unit PRIVATE libturi_wasm m ${CMAKE_DL_LIBS})
if(HAVE_EDITLINE)
    target_link_libraries(tur_wasm_glue_session_unit PRIVATE ${LIBEDIT_LIB})
    target_compile_definitions(tur_wasm_glue_session_unit PRIVATE TURI_HAVE_EDITLINE=1)
endif()
set_target_properties(tur_wasm_glue_session_unit PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
)

# SX8c: the playground's solver entry point, exercised natively.
# `turi_smt_check` otherwise only runs inside a wasm module, where nothing in
# CI can look at it.  Nothing in wasm_glue.c is wasm-specific, so linking
# libturi_wasm runs the real thing -- against the same S0..S3 chain the browser
# build links.  See docs/archive/solver-extension-plan.md (SX8c).
add_executable(tur_wasm_glue_smt_unit
    ${CMAKE_SOURCE_DIR}/tests/wasm_glue_smt_unit.c
)
target_include_directories(tur_wasm_glue_smt_unit PRIVATE ${TUR_INCLUDE_DIRS})
target_compile_options(tur_wasm_glue_smt_unit PRIVATE
    -Wall -Wextra -Wno-unused-parameter ${TUR_CSTD}
    -pthread
    $<$<CONFIG:Debug>:   -Og -g ${TUR_ASAN_FLAGS} -DTUR_DEBUG=1>
    $<$<CONFIG:Release>: -O2 -DNDEBUG>
)
target_link_options(tur_wasm_glue_smt_unit PRIVATE
    -pthread
    $<$<CONFIG:Debug>:   ${TUR_ASAN_FLAGS}>
)
target_link_libraries(tur_wasm_glue_smt_unit PRIVATE libturi_wasm m ${CMAKE_DL_LIBS})
if(HAVE_EDITLINE)
    target_link_libraries(tur_wasm_glue_smt_unit PRIVATE ${LIBEDIT_LIB})
    target_compile_definitions(tur_wasm_glue_smt_unit PRIVATE TURI_HAVE_EDITLINE=1)
endif()
set_target_properties(tur_wasm_glue_smt_unit PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
)

# RM3 region allocator unit test (docs/archive/regions-plan.md).  Pins the
# SAFETY properties -- ownership across retirement, a conservative pop that
# does not reclaim, a refused mismatched depth -- not that malloc works.
add_executable(tur_region_unit
    ${CMAKE_SOURCE_DIR}/tests/region_unit.c
    ${CMAKE_SOURCE_DIR}/src/runtime/region.c
    ${CMAKE_SOURCE_DIR}/src/runtime/arena.c
)
target_include_directories(tur_region_unit PRIVATE ${TUR_INCLUDE_DIRS})
target_compile_options(tur_region_unit PRIVATE
    -Wall -Wextra -Wno-unused-parameter ${TUR_CSTD} -pthread
    $<$<CONFIG:Debug>:   -Og -g ${TUR_ASAN_FLAGS} -DTUR_DEBUG=1>
    $<$<CONFIG:Release>: -O2 -DNDEBUG>
)
# region-lock-hardening: the generation stack is per-thread and ownership is
# process-wide; the unit test spawns a thread to pin both halves.
target_link_options(tur_region_unit PRIVATE -pthread
    $<$<CONFIG:Debug>:   ${TUR_ASAN_FLAGS}>
)
set_target_properties(tur_region_unit PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
)

# Injective name-mangler round-trip + injectivity unit test
# (reversible-name-mangling-plan, task T2).
add_executable(tur_mangle_unit
    ${CMAKE_SOURCE_DIR}/tests/mangle_test.c
    ${CMAKE_SOURCE_DIR}/src/compiler/mangle.c
    # `tur demangle`'s token recognizer (profiling-plan P0) is tested here
    # rather than through the CLI: the decode is pure, and which tokens it
    # accepts is the part worth pinning. cli/demangle.c depends on nothing
    # but mangle.h and stdio.
    ${CMAKE_SOURCE_DIR}/src/cli/demangle.c
)
target_include_directories(tur_mangle_unit PRIVATE ${TUR_INCLUDE_DIRS})
target_compile_options(tur_mangle_unit PRIVATE
    -Wall -Wextra -Wno-unused-parameter ${TUR_CSTD}
    $<$<CONFIG:Debug>:   -Og -g ${TUR_ASAN_FLAGS} -DTUR_DEBUG=1>
    $<$<CONFIG:Release>: -O2 -DNDEBUG>
)
target_link_options(tur_mangle_unit PRIVATE
    $<$<CONFIG:Debug>:   ${TUR_ASAN_FLAGS}>
)
set_target_properties(tur_mangle_unit PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
)

# ---------------------------------------------------------------------------
# Phase WASM: WebAssembly build target for try.turmeric-lang.io
# ---------------------------------------------------------------------------

# WASM glue source
set(WASM_GLUE_SOURCES
    web/wasm_glue.c
    web/tutorial.c
    # try-turmeric-lsp-plan: the LSP bridge, plus the WASM-side analysis
    # backend.  tur_core carries the whole server already; what it does not
    # carry is tur_collect_symbols, which lives in main.c (a CLI, and not part
    # of this source set).  Nothing referenced the server, so wasm-ld collected
    # it away and the dangling reference never surfaced -- exporting an entry
    # point makes it live, and this file is what resolves it.
    web/wasm_lsp.c
)

# Create a static library for the WASM glue + libturi
add_library(libturi_wasm STATIC ${WASM_GLUE_SOURCES} $<TARGET_OBJECTS:tur_core>)
set_target_properties(libturi_wasm PROPERTIES OUTPUT_NAME "turi_wasm")
target_include_directories(libturi_wasm PRIVATE ${TUR_INCLUDE_DIRS})
target_link_options(libturi_wasm INTERFACE -pthread)

# Option to build the WASM module (requires Emscripten)
option(TUR_WASM "Build WebAssembly module for try.turmeric-lang.io" OFF)

if(TUR_WASM)
    # Find emcc (Emscripten compiler)
    find_program(EMCC emcc)
    if(NOT EMCC)
        message(WARNING "emcc not found - WASM target will not be built. Install Emscripten from https://emscripten.org")
    else()
        message(STATUS "emcc found: ${EMCC} — building WASM module")
        
        # Get all the source files we need to compile
        set(WASM_SOURCES ${WASM_GLUE_SOURCES} ${TUR_CORE_SOURCES})

        # Remove architecture-specific assembly and platform I/O backends —
        # emcc targets wasm32 and cannot assemble native code; I/O is stubbed.
        list(REMOVE_ITEM WASM_SOURCES
            async/fiber_ctx_arm64.S
            async/fiber_ctx_x64.S
            async/io_epoll.c
            async/io_kqueue.c
        )
        # Deduplicate (tutorial.c appears in both WASM_GLUE_SOURCES and TUR_CORE_SOURCES)
        list(REMOVE_DUPLICATES WASM_SOURCES)
        
        # Build the WASM module using Emscripten
        # We compile all sources into object files first, then link with emcc
        add_custom_target(tur_wasm
            COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/wasm
            COMMAND ${EMCC}
                -I${CMAKE_CURRENT_SOURCE_DIR}
                -I${CMAKE_CURRENT_SOURCE_DIR}/compiler
                -I${CMAKE_CURRENT_SOURCE_DIR}/passes
                -I${CMAKE_CURRENT_SOURCE_DIR}/runtime
                -I${CMAKE_CURRENT_SOURCE_DIR}/async
                -I${CMAKE_CURRENT_SOURCE_DIR}/web
                ${WASM_SOURCES}
                # Bundle the stdlib into the module's virtual FS at /stdlib so the
                # interpreter can (load ...) macros + typed collections at init.
                # The default cwd is "/", so a cwd-relative "stdlib/<file>" load
                # (the form the stdlib modules use internally) resolves here too.
                # web-repl-missing-stdlib-preload.
                --embed-file ${CMAKE_SOURCE_DIR}/stdlib@/stdlib
                -o ${CMAKE_BINARY_DIR}/wasm/turmeric.js
                -sMODULARIZE=1
                -sALLOW_MEMORY_GROWTH=1
                -sALLOW_TABLE_GROWTH=1
                -sWASM=1
                -pthread
                -sPTHREAD_POOL_SIZE_STRICT=0
                -sEXIT_RUNTIME=0
                -sSTACK_SIZE=16777216
                -sDEFAULT_PTHREAD_STACK_SIZE=8388608
                -sINITIAL_MEMORY=67108864
                # _turi_wasm_read_file is deliberately a named operation rather
                # than adding FS to EXPORTED_RUNTIME_METHODS: it reads .tur
                # files under the stdlib mount and nothing else, so
                # go-to-definition can open a stdlib buffer without the page
                # gaining a general filesystem (wasm_lsp.c,
                # try-turmeric-navigation-and-minimap-plan M4).
                -sEXPORTED_FUNCTIONS=_turi_wasm_init,_turi_wasm_reset,_turi_wasm_rewind_to_prelude,_turi_wasm_shutdown,_turi_wasm_eval,_turi_wasm_eval_ex,_turi_wasm_version,_turi_wasm_format,_turi_wasm_lsp_request,_turi_wasm_lsp_flush,_turi_wasm_lsp_reset,_turi_wasm_read_file,_turi_wasm_set_lang,_turi_wasm_get_lang,_turi_wasm_lang_registry,_turi_doc_lookup,_turi_smt_check,_turi_type_of,_turi_explain,_turi_wasm_trace_run,_turi_wasm_trace_stats,_turi_wasm_trace_seek,_turi_wasm_trace_state,_turi_wasm_trace_site_at,_turi_wasm_trace_find_line,_turi_wasm_trace_output_full,_turi_wasm_trace_buffer,_turi_wasm_trace_buffer_len,_turi_wasm_trace_release,_malloc,_free
                -sEXPORTED_RUNTIME_METHODS=stringToUTF8,UTF8ToString,lengthBytesUTF8
                -sEXPORT_NAME=TurmericModule
                -sMODULARIZE=1
                -O2
                -Wall -Wextra -Wno-unused-parameter ${TUR_CSTD} -pedantic
                -D_DEFAULT_SOURCE
            WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
            # The emitter's embedded region runtime is a generated TU in
            # TUR_CORE_SOURCES; emcc compiles it straight from the list above,
            # so it must exist before this target runs.
            DEPENDS ${TUR_REGION_EMBED_C}
            COMMENT "Building Turmeric WASM module (pthreads, lazy pool)"
            VERBATIM
        )

        # Copy the WASM output to the web directory for development
        add_custom_command(TARGET tur_wasm POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                ${CMAKE_BINARY_DIR}/wasm/turmeric.js
                ${CMAKE_SOURCE_DIR}/web/public/turmeric.js
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                ${CMAKE_BINARY_DIR}/wasm/turmeric.wasm
                ${CMAKE_SOURCE_DIR}/web/public/turmeric.wasm
            COMMENT "Copying WASM module to web/public"
        )
    endif()
endif()

# security-audit-plan WP4: the libFuzzer targets.  Added from here, not the
# top level, so TUR_INCLUDE_DIRS / TUR_CSTD and tur_core are in scope.
if(TUR_FUZZ)
    add_subdirectory(${CMAKE_SOURCE_DIR}/tests/fuzz ${CMAKE_BINARY_DIR}/fuzz-targets)
endif()
