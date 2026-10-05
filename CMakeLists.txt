cmake_minimum_required(VERSION 3.20)
project(turmeric LANGUAGES C ASM)

# Export compile_commands.json for clangd.
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# ---------------------------------------------------------------------------
# Debug-build sanitizers (AddressSanitizer + UBSan)
# ---------------------------------------------------------------------------
# The Debug build instruments `tur` (and, in the eval_import test, the C it
# generates) with -fsanitize=address,undefined. This is ON by default on every
# platform -- the sanitizers are load-bearing coverage and we never want to
# silently carve a platform out of testing them.
#
# Known failure mode: on some newer macOS/dyld the ASan runtime *baked into the
# binary at link time by an outdated clang* deadlocks in InitializeShadowMemory
# while walking the dyld shared cache -- it spins in a startup spinlock *before*
# main(), so every `tur` invocation (even `tur --version`) hangs. That is a
# toolchain/OS runtime bug, not a bug in `tur`, and it reproduces with a bare
# `int main(){}` compiled `-fsanitize=address`.
#
#   * Real fix (keeps coverage): build with a current LLVM whose ASan runtime
#     knows the new dyld cache layout, e.g. Homebrew LLVM:
#         brew install llvm
#         cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug \
#               -DCMAKE_C_COMPILER="$(brew --prefix llvm)/bin/clang"
#   * Escape hatch (drops leak/UB detection locally): explicitly opt out --
#         cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DTUR_DEBUG_SANITIZE=OFF
#
# We deliberately do NOT auto-disable on any platform: a silent, permanent loss
# of sanitizer coverage in CI is worse than a loud hang the `tur --version`
# smoke check catches.
option(TUR_DEBUG_SANITIZE
  "Instrument the Debug build with AddressSanitizer + UBSan"
  ON)

# MinGW GCC ships no ASan/UBSan runtime at all -- there is no libasan.a or
# libubsan.a in the toolchain -- so -fsanitize=address,undefined fails at LINK
# time with "cannot find -lasan" and no Debug build can succeed.
#
# This is not the macOS situation described above and does not contradict the
# "never auto-disable" rule.  There the runtime exists and merely misbehaves, so
# staying loud is the right call.  Here the feature does not exist on the
# toolchain, so the only choices are "disable it" or "no Windows Debug build";
# the loss of coverage is announced rather than silent.  MSYS2's clang DOES ship
# compiler-rt, so the opt-out is scoped to GNU rather than to Windows at large.
if(TUR_DEBUG_SANITIZE AND WIN32 AND CMAKE_C_COMPILER_ID STREQUAL "GNU")
  set(TUR_DEBUG_SANITIZE OFF)
  message(STATUS
    "tur: Debug sanitizers disabled -- MinGW GCC has no ASan/UBSan runtime. "
    "Build with the MSYS2 clang toolchain to get sanitizer coverage on Windows.")
endif()

# security-audit-plan WP4: libFuzzer targets for the parsers that take
# untrusted bytes (tests/fuzz/README.md).  Clang only -- libFuzzer ships with
# compiler-rt.  Use a build directory of its own; it instruments tur_core:
#   cmake -S . -B build-fuzz -DCMAKE_BUILD_TYPE=Debug \
#         -DCMAKE_C_COMPILER=clang -DTUR_FUZZ=ON
option(TUR_FUZZ "Build the libFuzzer parser targets in tests/fuzz (clang only)" OFF)
if(TUR_FUZZ AND NOT CMAKE_C_COMPILER_ID MATCHES "Clang")
  message(FATAL_ERROR
    "TUR_FUZZ needs clang: libFuzzer ships with compiler-rt. "
    "Configure with -DCMAKE_C_COMPILER=clang.")
endif()

if(TUR_DEBUG_SANITIZE)
  set(TUR_ASAN_FLAGS "-fsanitize=address,undefined")
else()
  set(TUR_ASAN_FLAGS "")
  message(STATUS "tur: Debug sanitizers disabled (TUR_DEBUG_SANITIZE=OFF)")
endif()

# ---------------------------------------------------------------------------
# RT3: the Z3 refinement scaffold was RETIRED in 0.32.5.
#
# `TUR_REFINE_Z3_ORACLE`, the `find_package(Z3)` block, `refine_libz3.c` and
# every `#ifdef TUR_REFINE_Z3_ORACLE` are gone, per the retirement criteria in
# docs/archive/refinement-types-plan.md ("Z3 retirement criteria").  Both
# criteria were met: the in-house chain discharges the fixtures the bootstrap
# carried, and oracle trust was established over a labelled corpus, a 3600-
# benchmark generated soak, a 7000-VC differential run, and a real-program
# dogfood (tur-ecs) -- all clean.
#
# Nothing replaces it, because nothing shipped ever used it: the in-house
# staged decision procedure (S0..S3, src/compiler/refine_solver_*.c) is and was
# the only solver a user runs.  What survives as the standing regression is the
# labelled SMT-LIB corpus under tests/corpus/smtlib/, replayed by the
# `tur_refine_corpus` ctest target with NO solver linked -- see below.
#
# ---------------------------------------------------------------------------
# CPM: package manager for fetching Raylib (and any future C dependencies).
# ---------------------------------------------------------------------------
include(cmake/CPM.cmake)

# ---------------------------------------------------------------------------
# Turmeric compiler (native CMake build)
# ---------------------------------------------------------------------------
# ---------------------------------------------------------------------------
# Windows: link the MinGW support libraries statically.
#
# A default MinGW build of tur.exe needs libwinpthread-1.dll and edit.dll from
# the MSYS2 tree.  Off an MSYS2 PATH it does not start at all -- exit 127, no
# output, no diagnostic -- so a release archive containing only the exe is dead
# on arrival, and the failure gives the user nothing to go on.
#
# ON by default rather than release-only.  A release-only knob is a code path
# that is first exercised while cutting a release, which is the worst moment to
# discover it; this way every Windows CI build links the way the shipped binary
# does.  Both libraries have a static form in MSYS2 (libwinpthread.a,
# libedit.a), so there is no functional difference -- only a larger exe.
#
# CMAKE_FIND_LIBRARY_SUFFIXES is the load-bearing half: without it
# find_library(edit) returns libedit.dll.a, the IMPORT library, and hands ld an
# absolute path that -static does not override -- so the DLL dependency
# survives and the flag looks like it worked.
option(TUR_WIN_STATIC "Statically link the MinGW support libraries" ON)
if(WIN32 AND TUR_WIN_STATIC)
    set(CMAKE_FIND_LIBRARY_SUFFIXES ".a" ".lib")
    add_link_options(-static)
endif()

# Tests are collected from a subdirectory only if testing was enabled BEFORE
# it was added, so `enable_testing()` sits above `add_subdirectory(src)`.  It
# used to come after, in the "Tests (CTest)" section below, and every
# `add_test` inside src/CMakeLists.txt was silently dropped -- cmake succeeded,
# the executable built, `ctest -N` never listed it
# (src-cmakelists-add-test-never-registers).  tests/check-ctest-registration.sh
# guards the ordering.
enable_testing()

# J1: build `tur` itself with the in-process MIR JIT engine (`tur jit`, the
# `jit` engine of `tur run`/`tur repl`, and the c2mir-backed dynamic FFI the
# interpreter uses for extern-c).  Since the `jit` experiment graduated
# (2026-08-17) this is the ONLY gate on `tur jit`: a binary built with it runs
# `tur jit` unprompted, and one built without it says so.  It does not change
# the default ENGINE -- `tur run` still resolves to "cc" unless asked.
#
# ON by default since 2026-10-02, on the hosts CI exercises the engine on:
# 64-bit x86-64 and arm64 (Linux, macOS, MinGW).  It used to be OFF because it
# cloned MIR over the network at configure time; MIR is vendored under
# external/mir/ now (cmake/mir.cmake), so a default configure still never
# reaches the network.  MIR also targets ppc64, s390x and riscv64, which no CI
# leg covers -- -DTUR_JIT=ON opts those in.  An Emscripten toolchain build
# gets no JIT (MIR's wasm32 target is not wired up as a host engine).
# Neither do Apple's embedded platforms (iOS, tvOS, watchOS, visionOS): they
# forbid a JIT outright, so the interpreter is the engine there, and an arm64
# processor alone must not turn MIR on (docs/reported/turi-ios-readiness-
# unverified.md).
#
# An EXISTING build directory keeps the value it cached, so one configured
# before this default changed stays OFF until reconfigured with -DTUR_JIT=ON
# (or rebuilt from a fresh directory).
#
# Declared ABOVE add_subdirectory(src), which reads it: declared after, a fresh
# configure handed src/ an unset TUR_JIT, and only a second configure (which
# found it in the cache) saw the default.
set(_tur_jit_default OFF)
if(NOT EMSCRIPTEN AND CMAKE_SIZEOF_VOID_P EQUAL 8
   AND NOT CMAKE_SYSTEM_NAME MATCHES "^(iOS|tvOS|watchOS|visionOS)$"
   AND CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64|amd64|aarch64|arm64|ARM64)$")
  set(_tur_jit_default ON)
endif()
option(TUR_JIT
  "build tur with the in-process MIR JIT engine (tur jit; vendored MIR)"
  ${_tur_jit_default})
if(TUR_JIT)
  include(cmake/mir.cmake)   # defines tur_mir from external/mir/
endif()

add_subdirectory(src)

# ---------------------------------------------------------------------------
# J0 JIT spike (docs/archive/jit-engine-plan.md) -- OFF by default.
# ---------------------------------------------------------------------------
# A scratch harness over the same vendored MIR as TUR_JIT (cmake/mir.cmake);
# the spike binary is never part of `tur`.
option(TUR_JIT_SPIKE
  "build the Phase J0 JIT spike harness (not part of tur)"
  OFF)
if(TUR_JIT_SPIKE)
  include(cmake/mir.cmake)   # guarded; a no-op when TUR_JIT already included it
  add_subdirectory(tools/jit-spike)
endif()

# ---------------------------------------------------------------------------
# Tests (CTest)  -- enable_testing() is above add_subdirectory(src); see there.
# ---------------------------------------------------------------------------

# Escaping a filesystem path for embedding in generated Turmeric source.  The
# bug it guards took down the entire stdlib prelude on an installed tur.exe --
# every backslash of a Windows path read as a string escape, so `when` was an
# unknown name -- and the in-tree build could not see it, because the stdlib
# root resolves to the literal "stdlib" there, which has nothing to escape.
# The cases are reachable on POSIX too ("\\" and '"' are both legal in a POSIX
# filename), so the Linux legs enforce it rather than Windows alone.
# Does the JSON-RPC stdio transport answer at all?  `tur lsp` and `tur dap`
# frame on a literal CRLF CRLF, and on Windows fd 0 is opened in TEXT mode,
# which strips exactly those CRs -- so both servers exited 0 having printed
# nothing, for every editor, silently.  65 passing LSP assertions missed it
# because the harness that runs them had never run on Windows.  This needs no
# fixtures and takes about a second, so it can run everywhere.
add_test(
  NAME lsp_dap_stdio_smoke
  COMMAND bash tests/lsp/run-stdio-smoke.sh
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)

# saffron-lang-plan S8: the LSP analyses a `#lang saffron` buffer as Saffron --
# a valid one reports nothing, a broken one reports.  True today by inheritance
# (the LSP compiles a temp file and `#lang` detection is content-based) rather
# than by design: the LSP never mentions the dialect.  Two other entry points
# have already been caught hardcoding READER_TURMERIC and breaking on `#lang`,
# so this pins the third.  Drives a live server because diagnostics are
# published asynchronously -- a session that exits first reports nothing, which
# looks exactly like a clean file, so the script asserts a BROKEN buffer
# reports before it believes a clean one.
add_test(
  NAME lsp_saffron_diagnostics
  COMMAND bash tests/lsp/run-saffron-diagnostics.sh
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)
set_tests_properties(lsp_saffron_diagnostics PROPERTIES TIMEOUT 300)
# r7rs-lang-plan R9: a `#lang r7rs` buffer analyses as Scheme (by inheritance,
# measured and pinned) and FORMATS -- textDocument/formatting answered "no
# edits" for every `#lang` document until it ran fmt_format_document.
add_test(
  NAME lsp_r7rs_diagnostics
  COMMAND bash tests/lsp/run-r7rs-diagnostics.sh
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)
set_tests_properties(lsp_r7rs_diagnostics PROPERTIES TIMEOUT 300)
# lsp-publishes-other-files-diagnostics-under-one-uri and
# lsp-sibling-import-resolves-against-scratch-dir: a loaded/imported file's
# diagnostic lands on the form that names it (with the real location as
# relatedInformation), and entry-relative resolution starts from the document,
# not from the temp directory its scratch copy sits in.
add_test(
  NAME lsp_cross_file_diagnostics
  COMMAND bash tests/lsp/run-cross-file-diagnostics.sh
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)
set_tests_properties(lsp_cross_file_diagnostics PROPERTIES TIMEOUT 300)

add_test(NAME source_literal_unit COMMAND tur_source_literal_unit)

add_test(
  NAME tur_tests
  COMMAND bash tests/run.sh
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)
# tests/run.sh and tests/run-turi.sh each fan out across every core on their
# own (JOBS defaults to nproc). Under `ctest -jN` they are therefore not N
# tests sharing a machine but N x nproc processes on it, and both harnesses
# impose per-fixture timeouts (10s compiled, 15s interpreted) that a
# 4x-oversubscribed box blows straight through. The result is a batch of
# timeout FAILs that look like product bugs and are not reproducible in
# isolation.
#
# RUN_SERIAL tells ctest to give these two the machine to themselves, which is
# what they already assume.
set_tests_properties(tur_tests PROPERTIES RUN_SERIAL TRUE)
# Emitted-program leak gate. tests/run.sh compiles fixture programs WITHOUT
# sanitizers and runs them with detect_leaks=0, so only `tur` itself is
# leak-checked -- a leak in EMITTED code passes the ordinary suite silently,
# which only compares printed output. This harness compiles an opted-in fixture
# (`requires.leak-check` marker) with -fsanitize=address and LeakSanitizer on.
#
# Opt-in because a sanitized compile per fixture is slow. RUN_SERIAL for the
# same reason tur_tests is: it fans out across nproc on its own.
add_test(
  NAME tur_leak_check
  COMMAND bash tests/run-leak-check.sh
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)
set_tests_properties(tur_leak_check PROPERTIES RUN_SERIAL TRUE)
# r7rs-lang-plan T8: every `#lang r7rs` fixture compiled with ASan and UBSan
# (UB fatal) and run -- a memory error in the prelude's inline C, the emitted
# Scheme code or the runtime fails it.  Leaks are not checked: Scheme heap data
# is never freed by design (docs/reported/r7rs-heap-data-never-reclaimed.md).
# Fans out across nproc on its own, so RUN_SERIAL.
add_test(
  NAME tur_r7rs_sanitize
  COMMAND bash tests/run-r7rs-sanitize.sh
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)
set_tests_properties(tur_r7rs_sanitize PROPERTIES TIMEOUT 720 RUN_SERIAL TRUE)
# The r7rs-gc collector (docs/archive/r7rs-gc-plan.md): every Scheme and
# compiled Saffron fixture built with the collector (the default for both
# dialects) and run in torture mode (a collection every 31 allocations), and
# churn loops (Scheme and Saffron) that must fit a 256 MiB address space with
# the collector and not without it.
add_test(
  NAME tur_r7rs_gc
  COMMAND bash tests/run-r7rs-gc.sh
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)
set_tests_properties(tur_r7rs_gc PROPERTIES TIMEOUT 720 RUN_SERIAL TRUE)
# The collector's divide-free slot index ((off * inv) >> 48, r7gc.c
# tur_gc_slot_of) equals off / size for every size class at every offset.
add_test(
  NAME tur_r7gc_slot_of
  COMMAND bash tests/check-r7gc-slot-of.sh
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)
set_tests_properties(tur_r7gc_slot_of PROPERTIES TIMEOUT 720)
add_test(
  NAME tur_cli_tests
  COMMAND bash tests/run-cli.sh
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)
# `tur demangle` (docs/upcoming/profiling-plan.md P0): CLI surface plus the
# false-positive ratchet over ./build/tur's own symbol table. The token
# recognizer's unit tests are in tur_mangle_unit.
add_test(
  NAME tur_demangle_tests
  COMMAND bash tests/run-demangle.sh
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)

# The compiler's recursion backstop.  There is no depth CAP any more -- the
# driver runs on a TUR_STACK_MB-sized stack -- so the remaining guard fires on
# real stack headroom, and the only practical way to assert it is to shrink the
# stack rather than to grow the program.  A fixture cannot set an env var, and a
# fixture tuned to sit just under a cliff is exactly the stack-size canary the
# retired errors/expr-nesting-depth-limit became, so this lives in a harness.
add_test(
  NAME tur_compiler_stack_guard
  COMMAND bash tests/run-compiler-stack-guard.sh
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)
# RM3: the region allocator's safety properties (docs/archive/regions-plan.md).
add_test(
  NAME tur_region_unit
  COMMAND "${CMAKE_BINARY_DIR}/tur_region_unit"
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)
# RM3 R2: --enable=regions with no region open must change nothing observable.
add_test(
  NAME tur_regions_seam
  COMMAND bash tests/run-regions-seam.sh
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)
# region-lock-hardening: source-level differential fuzz over region brackets.
# Generated programs let nodes escape a bracket through every route the lock
# knows (result type, store, erasure, stored closure, nested bracket) and read
# them back after the pop, on the default arm under ASan and under
# TUR_REGIONS=0, against a predicted stdout and a predicted rewind / retire
# count.  Smoke size -- run tests/regions-fuzz-src.py directly for a session.
add_test(
  NAME tur_regions_fuzz_src
  COMMAND bash tests/run-regions-fuzz-src.sh
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)
set_tests_properties(tur_regions_fuzz_src PROPERTIES TIMEOUT 900)
# RM3 R3: the poison backstop must actually fire.  A backstop that silently
# stops firing looks exactly like a program with no stragglers.
add_test(
  NAME tur_region_poison
  COMMAND bash tests/check-region-poison.sh
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)
# NW6 acceptance gate: a freshly scaffolded spice (`tur new`) passes its own
# `tur run ci` (clean + check + test) end-to-end. Hermetic: no git, no
# network, no ../turmeric-spices, no `just`. See docs/tur-new-ci-bootstrap-plan.md.
add_test(
  NAME tur_new_bootstrap
  COMMAND bash tests/run-tur-new.sh
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)
# `:tur-version` in build.tur (no-compiler-version-constraint-in-manifest):
# a satisfied range is silent, a missed floor is a hard error AND a non-zero
# exit, an exceeded ceiling only warns, and a malformed range is an error.
add_test(
  NAME tur_version_range
  COMMAND bash tests/run-tur-version.sh
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)
# Regression for tur-run-alias-breaks-snapshot-ci-guard: `alias NAME :=
# TARGET` lines used to abort the whole Justfile parse, making recipes
# after them unreachable.
add_test(
  NAME tur_run_alias
  COMMAND bash tests/run-tur-run-alias.sh
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)
# `tur just` is a synonym for `tur run`: both the Justfile task runner and
# the classic compile-and-run path answer to either spelling, and `tur jit`
# keeps its own meaning.
add_test(
  NAME tur_just_synonym
  COMMAND bash tests/run-tur-just-synonym.sh
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)
# OD4: `tur doc <symbol>` reads the full stdlib docstring table shipped with
# the binary (not just the builtin list), and `tur docs` locates the rendered
# guides. Both are the no-network half of the offline-docs work.
add_test(
  NAME tur_docs_cli
  COMMAND bash tests/run-tur-docs.sh
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)
# Regression for tur-run-just-assignment-eval-plan: `name := VALUE`
# assignments used to be raw byte fragments; verify the balanced scanner
# + RHS expression evaluator handle function calls, `/` and `+` concat,
# conditionals, and forward-reference errors.
add_test(
  NAME tur_run_rhs_eval
  COMMAND bash tests/run-tur-run-rhs-eval.sh
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)
# `tur completion` and the `tur run --list` contract it is built on: aliases
# listed, hidden recipes withheld but runnable, unsupported features degrading
# the listing rather than blanking it, and well-formed JSON.
add_test(
  NAME tur_completion
  COMMAND bash tests/run-tur-completion.sh
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)
# Recipe attributes and the shebang/`[script]` path: every attribute tur run
# claims to support is matched by name and does what it says, and an unknown
# attribute is refused rather than silently ignored (the silent-ignore hole in
# docs/archive/tur-run-silently-ignores-parameterized-attributes.md).
add_test(
  NAME tur_run_attrs
  COMMAND bash tests/run-tur-run-attrs.sh
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)
add_test(
  NAME tur_fmt_tests
  COMMAND bash tests/run-fmt.sh
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)
# SR4 seam-rot gate.  Recursive sums flow BY VALUE by default since RM4
# (2026-09-02); TUR_SR4_RECURSIVE_CARRIER=1 restores the int64 carrier for A/B
# measurement, so nothing else in CI ever compiles a recursive sum through the
# carrier and that path would rot silently as the compiler moves.  This harness
# re-runs the recursive-sum fixture population under the seam (build + run +
# stdout compare, ~22 fixtures) behind a canary that asserts the seam actually
# bit (the logic ctor returns int64_t), so a renamed env var fails loudly
# instead of silently re-testing the default path.  RUN_SERIAL for the same
# reason as the suites above.
add_test(
  NAME tur_sr4_seam
  COMMAND bash tests/run-sr4-seam.sh
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)
# option-niche seam-rot gate, the same argument as tur_sr4_seam above.
# The niche graduated 2026-09-03 (docs/archive/sr3-option-niche-plan.md);
# TUR_OPTION_NICHE=0 restores the tagged monomorph and this harness keeps THAT
# path green with an inverted canary, exactly as tur_sr4_seam does.
add_test(
  NAME tur_option_niche_seam
  COMMAND bash tests/run-option-niche-seam.sh
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)
# tur_sr2_seam was RETIRED 2026-08-27 with tests/run-sr2-seam.sh, and is
# RESTORED 2026-09-04 as the mirror-image OFF-path gate, the same argument as
# tur_sr4_seam and tur_option_niche_seam above.
#
# The retirement's reasoning was half right.  It observed that by value IS the
# default now, so every `bash tests/run.sh` compiles all eleven of the old
# harness's fixtures that way -- true, and it does cover the ON path.  What it
# missed is that the same flip left the CARRIER path with nothing compiling it
# at all, which is the exact condition the harness had been built for, with the
# two paths swapped.  Both sibling seams flipped their harness at graduation
# rather than deleting it; this one is now flipped too, a release late.
#
# The rot that accumulated in the meantime is why the argument matters: the
# restored harness found a hard C compile error and a silent wrong answer on the
# carrier path, plus a layering defect that makes TUR_SR2_APP_SUM_BYVALUE=0
# abort the compiler unless TUR_OPTION_NICHE=0 comes with it.  See
# docs/reported/sr2-carrier-seam-rotted.md and the harness header.
add_test(
  NAME tur_sr2_seam
  COMMAND bash tests/run-sr2-seam.sh
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)
# `tur check` sweep over examples/, ratcheted against
# examples/examples-check-baseline.txt. Nothing referenced examples/ before
# this, so nothing compiled them and they rotted -- 9 of 17 were failing when
# the sweep was added. See docs/archive/datalog-examples-do-not-compile.md.
add_test(
  NAME tur_examples_check
  COMMAND bash tests/check-examples.sh
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)
# examples/guestbook end to end: build it with `tur run` (build.tur's
# :c-sources links the httpd.c shim), serve a bounded number of requests on a
# private port, and walk the four-page flow with curl -- every hop resumes a
# continuation rebuilt from bytes on disk.  Skips itself when curl is absent.
add_test(
  NAME tur_guestbook_smoke
  COMMAND bash tests/run-guestbook.sh
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)
set_tests_properties(tur_guestbook_smoke PROPERTIES SKIP_RETURN_CODE 2)
# parse-check subcommand: AST-equality of turmeric + sweet-exp file pairs,
# the backing check for tools/check-guide-pairs.py.
add_test(
  NAME tur_parse_check_tests
  COMMAND bash tests/run-parse-check.sh
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)
add_test(
  NAME tur_span_tests
  COMMAND bash tests/check-span-unknown.sh
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)
# concrete-codegen-layout-kind-enumerations-drift: append_type_mangle and
# type_has_concrete_codegen_layout must stay exhaustive over TypeKind, and the
# mangle must stay injective against type_eq, or two distinct instantiations
# silently share one monomorph C name.  Source-text check, no build needed.
add_test(
  NAME tur_typekind_mangle_tests
  COMMAND bash tests/check-typekind-mangle-exhaustive.sh
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)
# Same report, behavioural half: compile programs that instantiate one ADT at
# two type arguments and assert the two monomorphs do not land on one C name
# (and that no #ifndef-guarded name is defined twice with differing bodies).
add_test(
  NAME tur_monomorph_name_collision
  COMMAND bash tests/check-monomorph-name-collision.sh
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)
# user-defn-named-div-collides-with-libc: tur_name_collides_libc() bsearches
# libc_names[], so the table's sort order is a correctness precondition -- one
# entry out of place makes bsearch quietly miss names and the symptom is a cc
# error cascade in generated C, not a failed lookup.  Also guards the table
# against silent drift from src/jit_win_prelude.h, whose declarations join the
# emitted TU's set on the Windows JIT path -- a name in the prelude and not in
# libc_names is a WRONG ANSWER there, not a build error.  Runs on every
# platform on purpose: the defect is Windows-only to observe but is introduced
# by editing these two files anywhere.  Source-text check, no build needed.
add_test(
  NAME tur_libc_collision_list
  COMMAND bash tests/check-libc-collision-list.sh
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)
# Increment 4 stage 4 (repr-decision-function-plan): representation-DECISION
# predicates must not gain new inline call sites -- new code consults
# repr_of / the shared predicates at chokepoints, or consciously updates the
# baseline.  Source-text check, no build needed.
add_test(
  NAME tur_repr_decision_ratchet
  COMMAND bash tests/check-repr-decision-ratchet.sh
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)
# F2.6 (compiled-shallow-handlers-plan): acceptance probes for `handle-shallow`.
# The distinguishing shallow behavior (a resume that re-performs) is
# interpreter-only and hard-errors on the compiled fiber fallback, so it lives
# in a probe rather than the compile-everything fixture harness.
add_test(
  NAME tur_shallow_handler_probes
  COMMAND bash tests/shallow-handler-probes.sh
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)
# debugger Phase 1: breakpoint-span coverage gate. Every fixture's elaborated
# AST must carry a usable source span on each breakpoint-eligible node (defn,
# let form, call site, top-level form). See docs/archive/history/debugger-plan.md.
add_test(
  NAME tur_span_coverage
  COMMAND bash tests/check-span-coverage.sh
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)
# debugger Phase 2: the interactive interpreter debugger (`tur debug`). Drives a
# scripted session (break / step / next / finish / backtrace / locals / print /
# (break) builtin / continue) over a fixture and asserts the output. The fixture
# carries requires.dedicated-runner so tests/run.sh skips it (debug is an
# interpreter-only path). See docs/archive/history/debugger-plan.md.
add_test(
  NAME tur_debugger
  COMMAND bash tests/run-debugger.sh
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)
# debugger Phase 3: the Debug Adapter Protocol server (`tur dap`). Drives a
# scripted JSON-RPC session (tests/dap-driver.py) over a fixture and asserts the
# handshake, breakpoints (plain + conditional), step/next/stepOut, the call
# stack, locals/evaluate, output events, and the exit code. The fixture carries
# requires.dedicated-runner so tests/run.sh skips it (DAP is an interpreter-only
# path). See docs/archive/history/debugger-plan.md.
add_test(
  NAME tur_dap
  COMMAND bash tests/run-dap.sh
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)
# editor-intelligence follow-through, track T1: the time-travel recorder.
# Records a fixture with `tur trace`, reads it back with `tur trace --dump`,
# and asserts the step/frame counts, that a delta only carries a binding whose
# rendering moved, that program output is both forwarded and interleaved, and
# that the step cap truncates loudly. The fixture carries
# requires.dedicated-runner so tests/run.sh leaves it to this target -- the
# recorder is an interpreter feature and the compiled path does not share it.
# See docs/archive/editor-intelligence-follow-through-plan.md.
add_test(
  NAME tur_trace
  COMMAND bash tests/run-trace.sh
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)
# debugger Phase 4: native source maps for emit-C. `tur --debug` threads
# `#line N "file.tur"` directives into the generated C (gated off by default so
# emit-c snapshots are unchanged) and compiles single-file builds with -g -Og,
# so gdb/lldb step through `.tur` source and backtraces name turmeric frames.
# The harness asserts both the gated #line emission and a turmeric-source gdb
# backtrace of a deliberate crash; it skips the gdb half cleanly when gdb is
# absent. The fixture carries requires.dedicated-runner so tests/run.sh leaves
# it to this target. See docs/archive/history/debugger-plan.md.
add_test(
  NAME tur_phase4_gdb
  COMMAND bash tests/run-phase4-gdb.sh
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)
# debugger Phase 5 (rich type display in native debuggers): N1 confirms the
# deterministic by-value carrier type names emit-c produces (Option__int,
# Result, ...) round-trip into the binary's DWARF; N2 confirms the gdb
# pretty-printers in tools/debug/turmeric_gdb.py render a Result parameter as
# `(ok 14)` and a returned Option as `(some 42)` rather than the raw C carrier.
# Skips the gdb half cleanly when gdb is absent. The fixture carries
# requires.dedicated-runner so tests/run.sh leaves it to this target. See
# docs/archive/history/debugger-plan.md and docs/archive/history/debugger-native-types-plan.md.
add_test(
  NAME tur_phase5_gdb
  COMMAND bash tests/run-phase5-gdb.sh
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)
# HttpdConn single-definition invariant: no inline-C block may reintroduce a
# local redeclaration of the consolidated httpd connection structs.
add_test(
  NAME tur_httpd_conn_single_def
  COMMAND bash tests/check-httpd-conn-single-def.sh
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)
# (export-from ...) costs nothing at runtime: re-exporting a name must resolve
# to the defining module's mangled symbol, not emit a forwarding wrapper per
# hop. The behaviour fixture (module-export-from) passes either way, so the
# invariant needs its own codegen guard.
add_test(
  NAME tur_export_from_no_wrapper
  COMMAND bash tests/check-export-from-no-wrapper.sh
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)
# SF let-bind regression: a let-bound two-level closure (signal-function) whose
# inner closure captures and calls the outer ^fat param must keep its declared
# outer arg type (tur check guard; see
# docs/archive/history/let-bound-sf-loses-outer-arg-type-when-inner-captures.md).
add_test(
  NAME tur_sf_let_bind_inner_call
  COMMAND bash tests/check-sf-let-bind-inner-call.sh
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)
# WR2 stdlib ownership guard: no stored rc<T> field may land in stdlib without an
# explicit `rc-cycle-ok` review marker, since a live strong rc<T> cycle is not
# reclaimable today. See docs/archive/history/stdlib-weak-ref-audit-plan.md and the
# "Ownership across the stdlib" section of docs/guides/gc-guide.md.
add_test(
  NAME tur_stdlib_no_rc_cycles
  COMMAND bash tests/check-stdlib-no-rc-cycles.sh
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)
# Numeric-tower guard: emitted C must never contain `_Complex`, `<complex.h>`,
# or the `__mul*c3` / `__div*c3` compiler-runtime helpers -- c2mir's C11 subset
# excludes the first and the JIT's symbol boundary should not grow the rest.
# See docs/upcoming/numeric-tower-rational-complex-plan.md section 1.
add_test(
  NAME tur_no_c_complex
  COMMAND bash tests/check-no-c-complex.sh
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)
# The same C11-subset rule for atomics: the emitted runtime spells them through
# the preamble's TUR_ATOMIC_* layer, never a literal `__atomic_*` builtin --
# c2mir has none, and one in the preamble sends every `tur jit` program to the
# cc fallback (TUR-W0070).
add_test(
  NAME tur_no_atomic_builtins
  COMMAND bash tests/check-no-atomic-builtins.sh
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)
# Self-test for run.sh's emitted-C pointer/integer ratchet: builds a canary whose
# emitted C provably mixes a pointer and an integer and asserts the warning
# pattern still fires.  A grep that matches nothing is indistinguishable from a
# clean corpus, which is exactly how two sweeps behind
# docs/archive/emitted-c-pointer-integer-warnings-unwatched.md reported a false
# zero.  run.sh runs this at startup too; the target makes it visible to ctest.
add_test(
  NAME tur_cc_warn_ratchet
  COMMAND bash tests/check-cc-warn-ratchet.sh
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)
# The explicit-cast half of the float<->int ratchet above.  -Wfloat-conversion
# sees only IMPLICIT conversions, and the emitter spells a carrier crossing as
# an explicit `(int64_t)(d)` -- the mechanism behind dozens of silent 7.1 -> 7
# answers found one seam at a time.  clang's AST records every conversion, so
# this reports each float<->int VALUE conversion in emitted program code that
# is neither an `as` the program asked for (TUR_AS) nor an exact literal nor a
# hand-written inline-C body.  Swept to zero over the corpus before landing
# (2512 programs).  Self-tests its detector first; prints UNAVAILABLE rather
# than passing when there is no clang.  Fans out across nproc, so serial.
add_test(
  NAME tur_emitted_float_conversions
  COMMAND python3 tests/check-emitted-float-conversions.py --self-test --corpus
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)
set_tests_properties(tur_emitted_float_conversions PROPERTIES
  RUN_SERIAL TRUE TIMEOUT 1500 DEPENDS tur)
# generic-spec-carrier-crossings: the lint above only sees programs that exist,
# and the fixture corpus held none of the generic-body shapes that were wrong.
# This generates them -- a value typed as the enclosing generic's own `A`, every
# producer x sink x representation -- and checks each cell compiled, interpreted
# and linted.  Gated by a baseline that can only shrink (a FIXED cell fails the
# run until it is removed).  Fans out across nproc, so serial.
add_test(
  NAME tur_generic_spec_matrix
  COMMAND python3 tests/generic-spec-matrix.py
          --tur "$<TARGET_FILE:tur>"
          --baseline tests/generic-spec-matrix.baseline
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)
# TIMEOUT 4500, raised from 3000 on 2026-10-01: the unsharded suite measured
# 2050s on the macOS leg, 68% of the old budget, which is not enough headroom
# for a cold runner or a nightly that adds cells.  CI shards this test
# (TUR_GSM_SHARD, see .github/workflows/ci.yml), so the budget is now sized for
# the UNSHARDED run -- a local `ctest` and the nightly arm64 leg -- not for what
# any one CI job does.
set_tests_properties(tur_generic_spec_matrix PROPERTIES
  RUN_SERIAL TRUE TIMEOUT 4500 DEPENDS tur)
# TUR_TEST_SHARD (run-jit.sh) and TUR_TURI_SHARD (run-turi.sh) must each
# partition the corpus: disjoint slices whose union is the whole thing.  Neither
# half is observable from a sharded run itself -- a shard that silently skipped
# fixtures, or ran some twice, still prints a clean summary -- so nothing else in
# the suite would notice.  Two harnesses, two variables, two proofs: their slices
# are computed over different corpora, so one partition holding says nothing
# about the other.
#
# Enumeration only (it drives each harness's own LIST mode and runs no fixture),
# so it needs no JIT engine and no interpreter.  NOT "about a minute", as this
# said until 2026-10-02: measured 114s median on Linux and 261s on macOS (max
# 416s) before the turi assertions were added.  It is Linux-only now -- its
# verdict is a function of the fixture tree alone, so the macOS copy re-derived
# the same answer; see LEG_INVARIANT in tests/ctest-parts.py.
add_test(
  NAME tur_shard_partition
  COMMAND bash tests/run-shard-partition.sh
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)
# The same invariant one level up: the `test` job's ctest PARTS must between
# them run every registered suite.  A suite in no part stops running and CI
# still goes green -- `--no-tests=error` catches only a pattern that empties a
# part outright, not one that loses a test from the middle.  That check used to
# be a sentence in ci.yml telling a human to add up `ctest -N` totals.
# Enumeration only (`ctest -N`), so it costs no test time and needs no tur.
add_test(
  NAME tur_ctest_partition
  COMMAND python3 tests/ctest-parts.py --check "${CMAKE_BINARY_DIR}"
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)
# `cmd | grep -q` under `set -o pipefail` reports a FALSE failure when the
# pattern IS found: grep exits at the first match, the writer takes SIGPIPE
# (141), and pipefail promotes that to the pipeline's status.  Load- and
# payload-dependent, so it reads as a product bug and re-runs green.  Bitten
# three times; see docs/archive/pipefail-grep-q-false-failures.md.
add_test(
  NAME tur_pipefail_grep_q_lint
  COMMAND bash tests/check-pipefail-grep-q.sh
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)
# A `const char *` accessor that returns a function-scoped `static char buf[]`
# is correct only while every caller consumes the name before asking for
# another -- and emitters do the opposite, gathering one name per field before
# writing the declaration, so every name becomes the LAST name.  No crash, no
# ASan report, no diagnostic: just well-formed C with the wrong type in it.
# Bitten twice; see docs/archive/c-name-accessors-share-static-buffers.md.
add_test(
  NAME tur_static_cname_buffer_lint
  COMMAND bash tests/check-static-cname-buffers.sh
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)
# The bare-name constructor alias must fail CLOSED: no alias when the name has
# more than one meaning, so inline C naming it fails at cc instead of silently
# reaching whichever ADT was emitted first.  It did not, when the uniqueness
# test keyed on the raw name and the #ifndef guard on the mangled one -- a
# silent wrong constructor with no diagnostic at any layer.  See
# docs/reported/separator-fold-collides-emitted-c-names.md.
add_test(
  NAME tur_ctor_alias_ambiguity
  COMMAND bash tests/check-ctor-alias-ambiguity.sh
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)
# stdlib/VERSION is what lets tur tell its own stdlib from another release's.
# A stamp that drifts is worse than no stamp, because it is trusted: a stale one
# makes a mismatched stdlib look like a match AND makes the correct one warn.
# See docs/archive/stdlib-dir-guard-accepts-mismatched-stdlib.md.
add_test(
  NAME tur_stdlib_version_stamp
  COMMAND bash tests/check-stdlib-version-stamp.sh
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)
# docs/reported/README.md is the triage index: it must list every open finding
# and nothing else.  It has drifted BOTH ways -- four rows left behind after
# their reports were archived (so triage read four findings that were not open),
# and a severity-HIGH report that never got a row at all.  Cheap to check,
# expensive to miss, and it can only fail on a docs edit.
add_test(
  NAME tur_reported_index_lint
  COMMAND bash tests/check-reported-index.sh
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)
# Every add_test declared under src/ must be visible to `ctest -N`.  It was
# not, for months: enable_testing() ran after add_subdirectory(src), so
# tur_trail built, passed by hand, and never ran in CI.  The ordering is fixed
# above; this keeps it fixed.
add_test(
  NAME tur_ctest_registration_lint
  COMMAND bash tests/check-ctest-registration.sh "${CMAKE_BINARY_DIR}"
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)
# tools/gendocs.py read `[a : int] : int` as two parameters and a return type
# of ':' -- 53% of the stdlib's signatures -- while the fused `[a :int]` was
# fine (gendocs-misparses-the-spaced-annotation-form).  Both spellings, the
# untyped form and the compound/variadic/#fx cases, without the renderer's
# `markdown` dependency.
add_test(
  NAME tur_gendocs_parse_lint
  COMMAND bash tests/check-gendocs-parse.sh
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)
# Spice-aware per-file resolver: SC0/SC1 (later SC phases extend the script).
add_test(
  NAME tur_spice_resolver_tests
  COMMAND bash tests/spice-resolver-tests.sh
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)
# Spice-level auxiliary C sources: :c-sources / :c-includes (spices-c-sources-plan).
add_test(
  NAME tur_spice_c_sources_tests
  COMMAND bash tests/spice-c-sources-tests.sh
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)
add_test(
  NAME tur_flags_tests
  COMMAND bash tests/run-flags.sh
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)
# macos-jit-hang-loses-both-diagnostics: a ctest-level bound, so a hang fails
# this test by name instead of running until the CI job's wall clock.  This
# target had none, and it is in the JIT leg of CI because it owns the
# `jit-ffi-*` cases -- dynamic FFI, callbacks and threads.  Generous against a
# ~1-2 min run: this bounds a hang, it does not assert performance.
set_tests_properties(tur_flags_tests PROPERTIES TIMEOUT 900)
# engine-selection-plan E1-E3: `tur run` engine resolution ladder
# (--engine > TUR_ENGINE > build.tur :engine > cc), the unsatisfiable-engine
# hard errors, and the P0 manifest-experiment gate.  Runs on every build --
# the jit rows assert whichever outcome (run vs hard error) this binary has.
add_test(
  NAME tur_engine_select
  COMMAND bash tests/run-engine-select.sh
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)
# Tutorial content must actually run: tutorials/*.yaml is data, so no compiler
# ever reads it, which is how quickstart.yaml came to teach an Option/for/struct
# API that never existed. Replays each tutorial's own steps through one REPL
# session.
add_test(
  NAME tur_tutorial_steps
  COMMAND bash tests/run-tutorial-quickstart.sh
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)
# `:entry "<path>"` in build.tur -- the first rung of `tur run`'s project-mode
# entry-point ladder (:entry > src/main.tur > the single .tur under src/),
# plus the dangling-:entry hard error.
add_test(
  NAME tur_manifest_entry
  COMMAND bash tests/run-manifest-entry.sh
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)
# Off-tree (load "stdlib/<file>") resolution: a freestanding script run from
# outside the repo resolves the stdlib via the exe-relative root, not just
# cwd-relative. See docs/archive/history/one-off-script-print-and-annotation-ergonomics.md
# (Finding 3).
add_test(
  NAME tur_offtree_load
  COMMAND bash tests/run-offtree-load.sh
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)
# End-to-end MCP + LSP server tests: drives `tur mcp` and `tur lsp` over
# stdio JSON-RPC and checks tools, hover, definition, documentSymbol,
# completion (incl. dynamic stdlib discovery), and the TUR_NO_MCP kill
# switch.  Skips cleanly if python3 is unavailable.
add_test(
  NAME tur_mcp_lsp_tests
  COMMAND bash tests/lsp/run-mcp-lsp.sh
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)
# PH3.2: LeakSanitizer regression gate for composite-type diagnostics. Runs the
# sanitized `tur` directly on handler-mismatch error fixtures with
# detect_leaks=1 and asserts no leak (the normal suite compiles generated
# programs without ASan, so a leak in tur's own error-reporting path -- e.g. the
# type_name composite-string leak fixed in PH2 -- would otherwise slip past).
# See docs/handler-typecheck-and-typename-followups-plan.md.
add_test(
  NAME tur_leak_gate
  COMMAND bash tests/run-leak-gate.sh
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)
# D4 sign-off probes for the (graduated) stackless catch-unwind lowering: the
# five cu-rec / cu-catch-deep / atom-rec / mutual-rec / fiber-rec shapes run at
# 1,000,000 depth under a reduced stack (ulimit -s 256).  Slow / memory-heavy,
# so deliberately kept out of the default `tests/run.sh` fast suite; this is the
# dedicated target the graduation plan (docs/archive/history/catch-unwind-graduation-plan.md,
# Part A) calls for.  Invoke explicitly:
#   ctest --test-dir build -R stackless_signoff_probes --output-on-failure
add_test(
  NAME stackless_signoff_probes
  COMMAND bash tests/stackless-signoff-probes.sh
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)
# Defense-in-depth backstop: the script already caps each probe with a portable
# per-probe timeout, but a ctest-level TIMEOUT guarantees the whole target can
# never wedge the job even if that logic is bypassed. A clean run finishes in a
# few seconds; 300s is comfortably above the sum of every per-probe cap.
set_tests_properties(stackless_signoff_probes PROPERTIES TIMEOUT 300)
# Fat-closure-env scoped-free gate. Compiles tests/fixtures/closure-env-no-leak
# (a capturing closure built in a loop and in a repeatedly-invoked effect
# handler) with ASan/UBSan + LeakSanitizer ON and asserts the emitted program is
# leak-clean: the scoped-free pass must release each non-escaping closure env at
# scope exit. The normal suite compiles spawned programs without ASan, so this
# leak would otherwise slip past. See docs/archive/history/fat-closure-env-leak.md.
add_test(
  NAME tur_closure_env_leak
  COMMAND bash tests/run-closure-env-leak.sh
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)
# Sibling gate for the fat-sink shim box: a bare fn passed to a `^fat` parameter
# used to malloc a { shim, orig } box per CALL that nothing ever freed (109 MiB
# over 4e6 iterations).  Same reason for a dedicated runner -- the normal suite
# compiles the spawned program without ASan.  See
# docs/archive/fat-sink-shim-box-leaks-per-call.md.
add_test(
  NAME tur_fat_shim_leak
  COMMAND bash tests/run-fat-shim-leak.sh
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)
# WKC2: boxed-key ownership gate. Compiles the HAMT runtime + a C unit test
# with ASan/UBSan (+LSan on Linux) and asserts that a wide-keyed map
# build+drop is leak-clean and double-free/use-after-free clean: a boxed key
# shared across structurally-shared persistent versions is freed exactly once.
# See docs/float32-map-key-carrier-plan.md (WKC2).
add_test(
  NAME tur_hamt_owned_keys
  COMMAND bash tests/run-hamt-owned-keys.sh
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)
# saffron-lang-plan S7 / D5, reverse direction: a Turmeric module importing a
# Saffron one.  The import path hardcoded READER_TURMERIC and never ran `#lang`
# detection, so a Saffron module could not be imported at all -- the directive
# reached the reader as source.  Needs its own runner because a multi-module
# fixture requires a dedicated runner anyway.
add_test(
  NAME tur_saffron_import
  COMMAND bash tests/run-saffron-import.sh
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)
# r7rs-lang-plan R3 / D9: the Turmeric <-> R7RS seam in both directions on
# both back ends -- a Turmeric module importing a `define-library`, and a
# Scheme program importing a Turmeric module through `(turmeric ...)` with
# `only`/`prefix`/`rename`.  A multi-module case, so a runner, not a fixture.
add_test(
  NAME tur_r7rs_import
  COMMAND bash tests/run-r7rs-import.sh
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)
# r7rs-lang-plan T4: `(import (scheme eval))` links the interpreter into a
# compiled program, and a program without the import links nothing new; the
# built program finds its stdlib from any directory.
add_test(
  NAME tur_r7rs_eval_link
  COMMAND bash tests/check-r7rs-eval-link.sh
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)
set_tests_properties(tur_r7rs_eval_link PROPERTIES ENVIRONMENT "ASAN_OPTIONS=detect_leaks=0")
# tur-init-help-scaffolds-and-overwrites: `tur init` must never destroy a
# working tree.  `tur init --help` did not print help -- the flag fell past the
# project-name slot, the cwd basename was used as the name, and scaffolding
# overwrote an existing .gitignore and README.md.  Nothing in the suite ran
# `tur init` in a directory whose contents it cared about (run-tur-new.sh and
# run-sweet-manifest.sh both scaffold inside mktemp -d), which is why nothing
# caught it.
add_test(
  NAME tur_init_no_clobber
  COMMAND bash tests/run-init-no-clobber.sh
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)
# interp-inline-c-opaque-segv-in-any-reflection: `any` reflection over an
# inline-C `defopaque` value under --interpret.  `type-of` on such a value used
# to SEGFAULT -- the inline-C result path re-tags a TURI_INT as TURI_STRUCT when
# the declared return type is TY_ADT, and an opaque IS a TY_ADT, so the payload
# 7 became a `TuriStruct *` of 0x7.  Needs its own runner rather than a fixture:
# tests/run-turi.sh PASS-skips every program with a user inline-C block (the TI7
# carve-out), which is exactly why nothing caught the crash.
add_test(
  NAME tur_interp_inline_c_opaque
  COMMAND bash tests/run-interp-inline-c-opaque.sh
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)
# turi-show-instance-with-inline-c-body-prints-a-pointer: a USER `Show [float]`
# whose `show` body is inline-C printed a heap pointer under --interpret with no
# diagnostic.  The interpreter's stdlib natives are registered under the mangled
# instance names (`__inst_Show_show_float`) and the "keep native override"
# branch kept one for ANY same-named impl -- so the user's cstr-returning method
# ran stdlib's owned-String native.  Same reason as above for a dedicated
# runner: run-turi.sh PASS-skips every inline-C program.
add_test(
  NAME tur_interp_show_inline_c
  COMMAND bash tests/run-interp-show-inline-c.sh
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)
# Prereq 2a (turi-open-reports-prereqs.md / turi-map-set-hamt-interpreter-gap.md):
# the context-carrying key comparator family (tur_hamt_*_eq_ctx) that lets a
# stateful/turi-closure comparator ride along via a `void *ctx` word.  Compiles
# tests/test_hamt_eq_ctx.c with ASan/UBSan (+LSan on Linux) and asserts ctx is
# threaded to every compare, content-keyed ops work, and plain/ctx ops nest.
add_test(
  NAME tur_hamt_eq_ctx
  COMMAND bash tests/run-hamt-eq-ctx.sh
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)
# HAMT delete-path sibling refcount regression
# (docs/archive/history/hamt-delete-sibling-refcount.md).  Compiles
# tests/test_hamt_del_lineage.c with ASan/UBSan (+LSan on Linux) and asserts
# that freeing a whole persistent-map lineage containing a tur_hamt_del-derived
# map is double-free / use-after-free clean.  Pre-fix this tripped a
# heap-use-after-free in tur_hamt_node_release.
add_test(
  NAME tur_hamt_del_lineage
  COMMAND bash tests/run-hamt-del-lineage.sh
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)
# collections-cannot-hold-rc-values, map side, step (b): caller-supplied VALUE
# ownership ops.  Value ownership used to be a bool selecting the hardcoded
# boxed-value refcount, which an rc<T> value cannot use.  Compiles
# tests/test_hamt_val_ops.c with ASan/UBSan (+LSan on Linux) and asserts the
# caller's ops are the ones that run -- across structural sharing, and for
# maps derived by a later set/del -- and that they balance exactly.
add_test(
  NAME tur_hamt_val_ops
  COMMAND bash tests/run-hamt-val-ops.sh
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)
# RP0: `tur build --shared <dir>` produces a dlopen-able shared library.
# The script builds tests/fixtures/build-shared-smoke as a .so and runs a
# hand-written C harness that dlsyms the exported defn and calls it.
add_test(
  NAME tur_build_shared
  COMMAND bash tests/run-build-shared.sh
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)
# Manifest-driven `tur build <dir>` descent: builds tests/fixtures/
# build-project-smoke (a build.tur project with modules under src/<pkg>/),
# resolving cross-module imports, and asserts the manifest is not compiled
# as source. See docs/manifest-driven-build-descent-plan.md.
add_test(
  NAME tur_build_project
  COMMAND bash tests/run-build-project.sh
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)
# WP2 of docs/upcoming/security-audit-plan.md: the compiler driver used to
# splice repo-supplied text -- a manifest's :link-flags, a spice's :c-sources,
# an __tur_autolink__ marker, a Justfile backtick -- into the shell command
# string it hands to system(), and into directories it then wrote to.  Each
# fixture under tests/fixtures/security-driver/ carries one such payload; the
# assertion is a diagnostic AND the absence of the file the payload creates,
# because "failed to build" and "ran, then failed to build" look identical
# otherwise.
add_test(
  NAME tur_security_driver
  COMMAND bash tests/run-security-driver.sh
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)
# result-typedef-duplicated-across-modules regression: two modules export
# functions returning the same (Result cstr cstr) instantiation; a third
# module imports both.  Without a per-instantiation include guard around
# the `typedef struct Result__cstr__cstr` in each module's .h, the third
# TU fails to compile with `redefinition`.  See
# docs/archive/result-typedef-duplicated-across-modules.md.
add_test(
  NAME tur_result_typedef_multi_module
  COMMAND bash tests/run-result-typedef-multi-module.sh
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)
# any-type-ids-are-per-tu: an `any` box id used to be this TU's intern INDEX,
# so two translation units numbered the same type differently and a value
# widened in one module was misidentified in another (type-of, is?, cast, and
# the drop's boxed flag all read the wrong row).  `tur build <dir>` inlines
# every module into one TU and hides it, which is why every existing `any`
# fixture missed this -- so the pin drives separate compilation.  See
# docs/archive/any-type-ids-are-per-tu.md.
add_test(
  NAME tur_any_type_id_multi_module
  COMMAND bash tests/run-any-type-id-multi-module.sh
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)
# Sweet-exp manifest support: verifies build.tur.sweet is treated as a
# fully equivalent manifest by walk-up discovery, pkg_manifest_read, and
# tur init --sweet.  See docs/archive/history/build-tur-sweet-manifest-plan.md.
add_test(
  NAME tur_sweet_manifest
  COMMAND bash tests/run-sweet-manifest.sh
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)
# scscm-compile: build the scscm-compile fixture project (imports the scscm
# spice via :path) and diff its stdout against expected.stdout.  Skips with
# PASS when ../turmeric-spices is absent.
add_test(
  NAME tur_scscm_compile
  COMMAND bash tests/run-scscm-compile.sh
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)
# RP2: unit test for the generated FFI dispatcher trampolines in
# src/runtime/ffi_dispatch.{h,c}. The executable is defined in
# src/CMakeLists.txt as tur_ffi_dispatch_unit.
add_test(
  NAME tur_ffi_dispatch_unit
  COMMAND "${CMAKE_BINARY_DIR}/tur_ffi_dispatch_unit"
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)
# RT2/S0--S3: refinement VC + decision-procedure unit test
# (docs/archive/refinement-types-plan.md).
add_test(
  NAME tur_refine_solver
  COMMAND "${CMAKE_BINARY_DIR}/tur_refine_solver"
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)
# Z3 retirement: the labelled SMT-LIB corpus, replayed with no Z3 present.
# Labels come from the corpus files, not from a live solver, which is the whole
# point -- this is what stays behind when the scaffold is deleted.
# POSIX-only: the harness needs fork() to sandbox each case (src/CMakeLists.txt).
if(NOT WIN32)
add_test(
  NAME tur_refine_corpus
  COMMAND "${CMAKE_BINARY_DIR}/tur_refine_corpus" tests/corpus/smtlib
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)
endif()
# Source-level differential fuzz: generated programs compiled and run with the
# `refined` gate off and on, asserting the gate never turns a program the
# runtime contract caught into one that exits 0.  tur_refine_solver fuzzes
# BELOW the encoder and is blind to translation bugs; this one starts at the
# top.  Smoke size here -- run tests/refine-fuzz-src.py directly for a session.
add_test(
  NAME tur_refine_fuzz_src
  COMMAND bash tests/run-refine-fuzz-src.sh
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)
set_tests_properties(tur_refine_fuzz_src PROPERTIES TIMEOUT 600)
# Typed-boundary fuzz: correct-by-construction programs routing known values
# through random wrappers (structs, ADTs, Option/Result, Vec, closures) and
# boundary crossings, asserting check-accepted => compiles, links, runs, and
# prints the predicted output.  Shapes reproducing open docs/reported entries
# are excluded via known_bug_slug (pinned by --known-probes instead), so a
# finding here is a NEW checker/codegen disagreement.  Smoke size -- run
# tests/type-fuzz-src.py directly for a session.
add_test(
  NAME tur_type_fuzz_src
  COMMAND bash tests/run-type-fuzz-src.sh
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)
set_tests_properties(tur_type_fuzz_src PROPERTIES TIMEOUT 600)
# Saffron dynamic-surface fuzz: correct-by-construction `#lang saffron`
# programs routing known scalars through random `any` plumbing (wrappers,
# routes, terminal dynamic operations), asserting check-accepted => the C
# compiles, runs and prints the predicted output AND `tur --interpret` agrees.
# Shapes reproducing the open findings in
# docs/reported/saffron-dynamic-surface-pass.md are excluded via KNOWN (pinned
# by --known-probes instead), so a finding here is NEW.  Smoke size -- run
# tests/saffron-fuzz-src.py directly for a session.
add_test(
  NAME tur_saffron_fuzz_src
  COMMAND bash tests/run-saffron-fuzz-src.sh
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)
set_tests_properties(tur_saffron_fuzz_src PROPERTIES TIMEOUT 600)
# Representation trace smoke (representation-consolidation-meta-plan,
# increment 0): --emit-abi-trace's repr-trace lines classify every fn-typed
# parameter's representation and every emit-side representation bridge, so
# consolidation increments can diff traces.  This pins each classification.
add_test(
  NAME tur_repr_trace
  COMMAND bash tests/run-repr-trace.sh
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)
set_tests_properties(tur_repr_trace PROPERTIES TIMEOUT 120)
# RT5a: the refinement solver on wasm32 -- compiles under Emscripten AND gives
# the same answers at 32-bit pointers.  Skips cleanly when emcc is absent.
add_test(
  NAME tur_refine_wasm
  COMMAND bash tests/run-refine-wasm.sh
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)
set_tests_properties(tur_refine_wasm PROPERTIES TIMEOUT 600)
# UC-2: user-level experiments file reader unit test
# (docs/archive/history/user-config-experiments-plan.md). Standalone executable
# defined in src/CMakeLists.txt as tur_experiments_user_config.
add_test(
  NAME tur_experiments_user_config
  COMMAND "${CMAKE_BINARY_DIR}/tur_experiments_user_config"
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)
# UC-3: end-to-end precedence/suppression matrix for the user-level
# experiments file, driven through the built `tur` against synthetic
# $XDG_CONFIG_HOME homes (docs/archive/history/user-config-experiments-plan.md).
add_test(
  NAME tur_experiments_user_config_e2e
  COMMAND bash tests/run-experiments-user-config.sh
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)
# RP3: integration test for the REPL's auto spice-image loader. Runs
# `tur repl` inside fixture projects and asserts the discover/build/
# dlopen/manifest pipeline behaves correctly (initial load, freshness
# skip, rebuild-on-change, opt-out, .gitignore handshake).
add_test(
  NAME tur_repl_spice_load
  COMMAND bash tests/turi/repl-spice-load.sh
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)
# saffron-lang-plan S8: the REPL as a Saffron session -- by `--lang saffron`,
# and by `#lang saffron` typed at the prompt (which was accepted and silently
# ignored: the handler read only the READER axis).  The probe is a float add
# through an unannotated defn, since `(add 1 2)` is 3 under either dialect.
add_test(
  NAME tur_repl_lang_saffron
  COMMAND bash tests/turi/repl-lang-saffron.sh
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)
set_tests_properties(tur_repl_lang_saffron PROPERTIES TIMEOUT 300)
# r7rs-lang-plan R9: the REPL as an R7RS session -- `--lang r7rs` (refused
# before), `#lang r7rs` at the prompt (switched the language but kept
# Turmeric's preload and skipped the renames), the Scheme echo, and :reset.
add_test(
  NAME tur_repl_lang_r7rs
  COMMAND bash tests/turi/repl-lang-r7rs.sh
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)
set_tests_properties(tur_repl_lang_r7rs PROPERTIES TIMEOUT 300)
# saffron-lang-plan S8: `tur init --saffron` scaffolds a project that BUILDS.
# The assertion is the build, not the file text: checking for a `#lang` line
# would have passed while the LIBRARY scaffold could not compile at all
# (load_project_prelude hardcoded READER_TURMERIC, so the autoloaded Saffron
# prelude died on its own first line).
add_test(
  NAME tur_init_saffron
  COMMAND bash tests/run-init-saffron.sh
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)
set_tests_properties(tur_init_saffron PROPERTIES TIMEOUT 300)
# r7rs-lang-plan R9: `tur init --r7rs` -- both shapes build, the binary runs,
# both test files pass under `tur test`, and the scaffold is fmt-clean.  It
# found three project-mode defects the single-file fixtures never reach.
add_test(
  NAME tur_init_r7rs
  COMMAND bash tests/run-init-r7rs.sh
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)
set_tests_properties(tur_init_r7rs PROPERTIES TIMEOUT 600)
# r7rs-lang-plan R10: chibi-scheme's R7RS test suite, as a pass COUNT with a
# floor, so a regression fails and progress only needs the floor raised.  Both
# back ends; the compiled one is a single build of the whole suite, 2-4 minutes
# under the Debug `tur`, and the runner's per-program timeout is sized for it
# (tests/run-r7rs-conformance.sh).  Skips cleanly without python3.
#
# NOT RUN_SERIAL.  It was, on the grounds that "the compiled pass is one large
# C build" -- but one large single-threaded `cc` is the case RUN_SERIAL serves
# worst: it idles every core but one for the whole run, and on ctest's
# scheduler RUN_SERIAL is a full barrier, so the cost is paid twice (nothing
# runs beside it, and nothing runs while the queue drains into it).  The
# harness is concurrency-safe: tests/r7rs/run-conformance.py writes each
# program to its own `tempfile.NamedTemporaryFile`, so two runs share no path.
add_test(
  NAME tur_r7rs_conformance
  COMMAND bash tests/run-r7rs-conformance.sh
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)
set_tests_properties(tur_r7rs_conformance PROPERTIES TIMEOUT 720)
# r7rs-srfi-plan D7: each SRFI's own test suite (tests/r7rs/srfi/<N>/), through
# the same runner, with a floor per SRFI.  Two shards (every other suite,
# R7RS_SRFI_SHARD), since S7 the whole set is over seven minutes on a 4-core
# box, too near the 720 s timeout for a slower runner.
#
# NOT RUN_SERIAL, and that is the point of sharding rather than a detail of it.
# The split was made to stay under the per-test TIMEOUT, but while both shards
# were RUN_SERIAL ctest ran them one after the other, so the pair cost the same
# wall time as the unsharded suite it replaced -- the timeout was fixed and the
# clock was not.  Neither shard fans out internally (no nproc, no background
# jobs), so RUN_SERIAL left two of three cores idle for both of them; the
# runner is concurrency-safe for the same reason as tur_r7rs_conformance above.
foreach(_srfi_shard 1 2)
  add_test(
    NAME tur_r7rs_srfi_suites_${_srfi_shard}
    COMMAND bash tests/run-r7rs-srfi-suites.sh
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
  )
  set_tests_properties(tur_r7rs_srfi_suites_${_srfi_shard} PROPERTIES
    TIMEOUT 720 ENVIRONMENT "R7RS_SRFI_SHARD=${_srfi_shard}/2")
endforeach()
# R10: the Unicode tables behind (scheme char) are generated twice -- the
# prelude's inline C and the interpreter's natives -- and must be identical.
add_test(
  NAME tur_r7rs_unicode_sync
  COMMAND bash tests/check-r7rs-unicode-sync.sh
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)
# T0/T1: the shared C of Scheme numbers -- src/compiler/r7rs_bignum.inc and
# r7rs_numsyntax.inc (the source reader and the interpreter) -- and their
# copies in stdlib/r7rs/bignum.tur and numsyntax.tur (the compiled back end)
# must be identical.
add_test(
  NAME tur_r7rs_inc_sync
  COMMAND bash tests/check-r7rs-inc-sync.sh
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)
# r7rs-srfi-plan D4: SRFI_LIBS[] in scheme_lower.c, the stdlib/srfi/<N>.scm
# files, and the guide's SRFI table say the same thing about every SRFI.
add_test(
  NAME tur_r7rs_srfi_sync
  COMMAND bash tests/check-r7rs-srfi-sync.sh
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)
# r7rs-srfi-plan S3: src/passes/srfi_prune.c drops the SRFI definitions a
# program never reaches -- an unused (import (srfi 1)) emits the same C as no
# import, and one that calls `fold` emits `fold` and what it calls.
add_test(
  NAME tur_r7rs_srfi_prune
  COMMAND bash tests/check-r7rs-srfi-prune.sh
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)
set_tests_properties(tur_r7rs_srfi_prune PROPERTIES TIMEOUT 720)
# r7rs-tail-call-through-static-call-not-proper: tail calls through
# procedure values stay proper in a plain 8 MiB stack (the deep-stack
# thread off), where the program's 1 GiB stack cannot hide a regression.
add_test(
  NAME tur_r7rs_tail_calls_small_stack
  COMMAND bash tests/check-r7rs-tail-calls-small-stack.sh
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)
set_tests_properties(tur_r7rs_tail_calls_small_stack PROPERTIES TIMEOUT 720)
# r7rs-programs-compile-slowly: a `#lang r7rs` program builds as two units,
# the prelude compiled once and cached.  No piece of state may be defined in
# both, and two programs must link the same cached library object.
add_test(
  NAME tur_r7rs_prelude_split
  COMMAND bash tests/check-r7rs-prelude-split.sh
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)
set_tests_properties(tur_r7rs_prelude_split PROPERTIES TIMEOUT 720)
# turi-call-frames-never-reclaimed: `tur --interpret` hands an activation's
# call frame (and its let / match-arm frames) back when it returns uncaptured.
# Runs one program with and without reclamation and asserts peak RSS halves.
add_test(
  NAME tur_turi_frame_reclaim
  COMMAND python3 tests/check-turi-frame-reclaim.py $<TARGET_FILE:tur>
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)
set_tests_properties(tur_turi_frame_reclaim PROPERTIES TIMEOUT 720 DEPENDS tur)
# saffron-lang-plan S8: the editor syntax packs.  The vim half is ACTUALLY
# LOADED in a real Vim and queried for highlight groups -- which is what caught
# the fused-vs-spaced annotation bug.  The vscode half is structure and pattern
# checks only: there is no TextMate engine here, so it validates the JSON, that
# the grammar is wired into package.json, and each regex against inputs it must
# and must not match.  Skips cleanly without vim or python3.
add_test(
  NAME tur_editor_syntax
  COMMAND bash tests/run-editor-syntax.sh
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)
set_tests_properties(tur_editor_syntax PROPERTIES TIMEOUT 300)
# RP4: integration test for the FFI binding layer. Builds a fixture
# spice with several signature shapes and calls each export from the
# REPL, asserting correct results, qualified-name routing, and clear
# typed-error feedback.
add_test(
  NAME tur_repl_spice_call
  COMMAND bash tests/turi/repl-spice-call.sh
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)
# ffi-spices-integration-plan S1: a spice declaring its C dependency via
# the cmake deps manifest (link_dirs/link_libs, no autolink marker) is
# loadable and callable at the REPL -- through cc's link line on the
# subprocess path, and through the engine's autolink dlopen (-L dir
# probing) on the in-process TUR_ENGINE=jit path.  Hermetic: builds its
# own .so in the test temp dir.
add_test(
  NAME tur_repl_spice_linklibs
  COMMAND bash tests/turi/repl-spice-linklibs.sh
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)
# RP5: integration test for the `(reload)` REPL form. Exercises the
# no-spice / no-changes / rebuild-after-edit / arity-check paths.
add_test(
  NAME tur_repl_spice_reload
  COMMAND bash tests/turi/repl-spice-reload.sh
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)
set_tests_properties(tur_repl_spice_reload PROPERTIES RUN_SERIAL TRUE)
# RP6: integration test for `tur repl --watch`. Mutates source mid-
# session and asserts the next call sees the new code (with --watch)
# vs the old one (without). Also covers the no-changes silent path
# and CLI argument parsing.
add_test(
  NAME tur_repl_spice_watch
  COMMAND bash tests/turi/repl-spice-watch.sh
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)
# RUN_SERIAL: this test drives a live REPL through a FIFO and calls cc to
# AOT-compile a spice; under a parallel ctest run with ASAN it can time out
# waiting for the first eval result.  Serial execution avoids that flake.
set_tests_properties(tur_repl_spice_watch PROPERTIES RUN_SERIAL TRUE)
# RP7: regression guards for the polished error surfaces -- stale
# manifest, build failure (with self-heal via reload), no-dispatcher
# hint format, and the "no spice project here" reload outcome.
add_test(
  NAME tur_repl_spice_errors
  COMMAND bash tests/turi/repl-spice-errors.sh
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)
# J2 (jit-engine-plan 3.3): the IN-PROCESS spice build behind
# `repl --engine jit` -- load with no .so artifact, calls (incl. the
# __ffi high-arity shim and a fractional float), the compile-error
# surface, and (reload) self-heal.  The script self-skips on a binary
# without the engine; the registration is gated the same way.
if(TUR_JIT)
  add_test(
    NAME tur_repl_spice_jit
    COMMAND ${CMAKE_COMMAND} -E env TUR=$<TARGET_FILE:tur>
            bash tests/turi/repl-spice-jit.sh
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
  )
  # macos-jit-hang-loses-both-diagnostics: RUN_SERIAL was the only property
  # here, so a hang in either of these whole-corpus targets ran until the CI
  # job's `timeout-minutes` killed the job -- which skips the remaining
  # `if: always()` steps and destroys the log.  A ctest TIMEOUT fails the test
  # instead, and names which one, which is most of the diagnosis.
  #
  # The numbers are hang bounds, not performance assertions.  Measured off the
  # ci-metrics branch, tur_jit_fixture_tests ran ~440s median on Linux, ~348s
  # on a 5-core mac and ~730s median / ~940s p90 on the 3-core mac CI draws
  # ~97% of the time, against a corpus of ~2663 fixtures; 1500 was ~1.6x that
  # p90.  repl-spice-jit is a short script.
  #
  # 2026-10-01: the corpus has outgrown that bound and 1500 was raised to 2400.
  # It is now 3413 dirs, and the FAST (5-core) draw alone takes 1102s -- above
  # the old 3-core p90 -- so 1500 had stopped being a hang bound and had become
  # a throughput assertion that a slow draw fails.  rjungemann/turmeric#1002
  # tripped it: 2393 dirs in 1500s = 1.6/s against the same hour's green `main`
  # run at 3.1/s, i.e. work at half speed, not a stall.  2400 is ~1.1x the
  # ~2140s that slow rate implies for the whole corpus and ~2.2x the fast draw.
  # See docs/reported/macos-jit-leg-stall-unexplained.md.
  #
  # Keep the ordering per-test TIMEOUT < the workflow's whole-ctest alarm <
  # the job's timeout-minutes: the alarm is the backstop for several targets
  # running long at once, and only the per-test value NAMES the offender, so
  # an alarm that can fire first destroys the diagnostic it exists to protect.
  # Raising this one means raising the alarm in .github/workflows/ci.yml too.
  set_tests_properties(tur_repl_spice_jit PROPERTIES RUN_SERIAL TRUE TIMEOUT 600)
  # aot-compiled-repl-plan C1: --enable=repl-jit-inline-c compiles an inline-C
  # defn the interpreter cannot run, at the prompt and under --interpret.  A
  # short script (a dozen REPL sessions); it runs in the `test` job's aux part,
  # not the `jit` job, like any newly registered suite (tests/ctest-parts.py).
  add_test(
    NAME tur_repl_jit_inline_c
    COMMAND ${CMAKE_COMMAND} -E env TUR=$<TARGET_FILE:tur>
            bash tests/turi/repl-jit-inline-c.sh
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
  )
  set_tests_properties(tur_repl_jit_inline_c PROPERTIES TIMEOUT 300)
  # J3 (jit-engine-plan section 5): the full fixture corpus through
  # `tur jit`.  RUN_SERIAL like the other whole-corpus
  # harnesses -- it fans out across nproc internally.  TUR is pinned to
  # THIS build's binary (the script's default would probe a sibling
  # build dir).  Note a fully green run needs a Debug binary (refine-*
  # discharge is Debug-only), same as tests/run.sh.
  add_test(
    NAME tur_jit_fixture_tests
    COMMAND ${CMAKE_COMMAND} -E env TUR=$<TARGET_FILE:tur>
            bash tests/run-jit.sh
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
  )
  set_tests_properties(tur_jit_fixture_tests PROPERTIES RUN_SERIAL TRUE TIMEOUT 2400)
endif()
# Type-checks a curated allowlist of non-auto-loaded stdlib files so
# regressions in their loadability are caught even when no fixture
# imports them. See docs/stdlib-check-allowlist-plan.md.
add_test(
  NAME tur_stdlib_checks
  COMMAND bash tests/run-stdlib-checks.sh
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)
# GS-M2: global spice install / uninstall / subcommand fallthrough.
add_test(
  NAME tur_install_tests
  COMMAND bash tests/run-install.sh
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)
# `tur fetch` + the lockfile integrity check, against a local file:// repo.
add_test(
  NAME tur_spice_fetch_tests
  COMMAND bash tests/run-spice-fetch.sh
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)
# The harness skips itself when git is absent.  Under ctest that would be a
# pass that tested nothing, and a source tree you cloned has git -- so here
# a missing one is a failure with an actionable message.
set_tests_properties(tur_spice_fetch_tests PROPERTIES
  ENVIRONMENT "TUR_REQUIRE_GIT=1")
# tools/ci/sentry-report.py, against a stub Sentry on a loopback port.
#
# This carries more weight than a CI helper usually would: the fuzz and TSan
# workflows send findings to Sentry and NOTHING else -- no public artifact, no
# target names in the log -- so a bug here loses a memory-safety finding
# silently, which is worse than the public issue it replaced.  Needs no tur and
# no network.
add_test(
  NAME tur_sentry_report_tests
  COMMAND bash tests/run-sentry-report.sh
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)
add_test(
  NAME turi_fixture_tests
  COMMAND bash tests/run-turi.sh
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)
# Self-parallelizing across nproc, like tur_tests above -- see the note there.
# This is the target a consumer reported failing under `ctest -j4` and passing
# in isolation.
set_tests_properties(turi_fixture_tests PROPERTIES RUN_SERIAL TRUE)
# Phase S0: tur repl / libturi smoke tests
add_test(
  NAME tur_repl_tests
  COMMAND bash tests/turi/eval-test.sh
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)
# Phase S1: REPL smoke tests (multi-line, :type, :doc, :reload, history)
add_test(
  NAME tur_repl_smoke
  COMMAND bash tests/turi/repl-smoke.sh
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)
# repl-continuation-counter-misreads-reader-syntax: a multi-line form is
# complete when the READER says so -- ```c fences, strings across lines,
# #| |# comments and #\( literals do not hang or split the prompt.
add_test(
  NAME tur_repl_multiline_input
  COMMAND bash tests/turi/repl-multiline-input.sh
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)
# The REPL surface a GUI host drives: OSC 133 busy/idle markers, the logical
# working directory, :load-string, and TUR_STDLIB_DIR validation.
add_test(
  NAME tur_repl_host_integration
  COMMAND bash tests/turi/repl-host-integration.sh
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)
# The tree-walking interpreter never frees its closures/registered natives
# (process-lifetime by design), so leak detection is off here as it is for the
# other REPL-driving harnesses.
set_tests_properties(tur_repl_host_integration PROPERTIES
  ENVIRONMENT "ASAN_OPTIONS=detect_leaks=0"
)
# Eval-mode unknown-call diagnostic: an unbound call head under --interpret must
# warn (TUR-W0040) at elaboration time rather than silently deferring to runtime.
add_test(
  NAME tur_eval_unknown_call_warn
  COMMAND bash tests/turi/eval-unknown-call-warn.sh
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)
set_tests_properties(tur_eval_unknown_call_warn PROPERTIES
  ENVIRONMENT "ASAN_OPTIONS=detect_leaks=0"
)
# Phase S2: (import turi/eval) from Turmeric source
add_test(
  NAME tur_eval_import
  COMMAND bash tests/turi/eval-import.sh
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)
# Set TUR_CC_FLAGS so the generated C can find turi/eval.h and libturi.a.
# In Debug builds libturi.a is ASAN-instrumented so we must also pass
# -fsanitize=address,undefined to avoid undefined ASAN symbol errors. When the
# Debug sanitizers are disabled (TUR_DEBUG_SANITIZE=OFF) libturi.a is clean, so
# the fixture compile must stay clean too -- gate on the same flag.
if(CMAKE_BUILD_TYPE STREQUAL "Debug" AND TUR_ASAN_FLAGS)
  set(_eval_import_asan " ${TUR_ASAN_FLAGS}")
else()
  set(_eval_import_asan "")
endif()
set_tests_properties(tur_eval_import PROPERTIES
  ENVIRONMENT "TUR_CC_FLAGS=-O2 -std=c99 -Wall${_eval_import_asan} -I${CMAKE_CURRENT_SOURCE_DIR}/src -I${CMAKE_CURRENT_SOURCE_DIR}/src/compiler -I${CMAKE_CURRENT_SOURCE_DIR}/src/passes -I${CMAKE_CURRENT_SOURCE_DIR}/src/runtime -I${CMAKE_CURRENT_SOURCE_DIR}/src/async -I${CMAKE_CURRENT_SOURCE_DIR}/src/web -L${CMAKE_CURRENT_BINARY_DIR}/src;ASAN_OPTIONS=detect_leaks=0"
)
add_test(
  NAME tur_eval_basic
  COMMAND "${CMAKE_BINARY_DIR}/tur_eval_basic"
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)
# I4: a libturi embedder reaches the JIT engine (and falls back to the
# tree-walking interpreter when the build has no engine).  The interpreter
# half registers process-lifetime closures it never frees by design, so this
# takes the same detect_leaks=0 the other turi harnesses do.
add_test(
  NAME tur_jit_embed
  COMMAND "${CMAKE_BINARY_DIR}/tur_jit_embed"
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)
set_tests_properties(tur_jit_embed PROPERTIES
  ENVIRONMENT "ASAN_OPTIONS=detect_leaks=0"
)
# web-repl-lang-switch-drops-stdlib: a reader-type change truncates src_acc back
# to the pinned stdlib preload rather than emptying it, so the first collection
# literal after a `#lang` switch still resolves hamt-of.
add_test(
  NAME tur_lang_switch_prelude
  COMMAND "${CMAKE_BINARY_DIR}/tur_lang_switch_prelude"
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)
set_tests_properties(tur_lang_switch_prelude PROPERTIES
  ENVIRONMENT "ASAN_OPTIONS=detect_leaks=0"
)
# PS1 (playground-session-hygiene-plan): after a failed turn, re-running a
# program must work and must not be blamed on an auto-loaded stdlib module.
add_test(
  NAME tur_session_failure_recovery
  COMMAND "${CMAKE_BINARY_DIR}/tur_session_failure_recovery"
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)
set_tests_properties(tur_session_failure_recovery PROPERTIES
  ENVIRONMENT "ASAN_OPTIONS=detect_leaks=0"
)
# PS4 (playground-session-hygiene-plan): def* forms redefinable across turns.
add_test(
  NAME tur_session_redefinition
  COMMAND "${CMAKE_BINARY_DIR}/tur_session_redefinition"
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)
set_tests_properties(tur_session_redefinition PROPERTIES
  ENVIRONMENT "ASAN_OPTIONS=detect_leaks=0"
)
# macro-system-direction-plan Stage 1: nested macro-time turi env inside a
# compile-posture process -- g_interpret_mode bracketing, TURI_CAP_NONE,
# fuel bound, session-owned teardown.
add_test(
  NAME tur_macro_env_nested
  COMMAND "${CMAKE_BINARY_DIR}/tur_macro_env_nested"
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)
set_tests_properties(tur_macro_env_nested PROPERTIES
  ENVIRONMENT "ASAN_OPTIONS=detect_leaks=0"
)
# map-show-keyword-key-raw-int (root cause B): the auto-show display path keeps
# the full result type beside the head-only tag, so a collection's elements
# resolve their own Show instance instead of the int-carrier representative.
add_test(
  NAME tur_show_collection_elems
  COMMAND "${CMAKE_BINARY_DIR}/tur_show_collection_elems"
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)
set_tests_properties(tur_show_collection_elems PROPERTIES
  ENVIRONMENT "ASAN_OPTIONS=detect_leaks=0"
)
# TR2.0: reader macros defined in one eval must stay usable across later evals
# (pins the contract the incremental-elaboration change must preserve).
add_test(
  NAME tur_reader_macro_cross_eval
  COMMAND "${CMAKE_BINARY_DIR}/tur_reader_macro_cross_eval"
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)
set_tests_properties(tur_reader_macro_cross_eval PROPERTIES
  ENVIRONMENT "ASAN_OPTIONS=detect_leaks=0"
)
# TR2: the incremental parse path must be result-identical to the default
# whole-blob re-parse path (A/B differential over scripted eval sessions).
add_test(
  NAME tur_incremental_elab_diff
  COMMAND "${CMAKE_BINARY_DIR}/tur_incremental_elab_diff"
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)
set_tests_properties(tur_incremental_elab_diff PROPERTIES
  ENVIRONMENT "ASAN_OPTIONS=detect_leaks=0"
)
# DEDUP-4: the runtime copy of the GC must behave like the emitted copy.
add_test(
  NAME tur_gc_runtime_copy_parity
  COMMAND "${CMAKE_BINARY_DIR}/tur_gc_runtime_copy_parity"
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)
set_tests_properties(tur_gc_runtime_copy_parity PROPERTIES
  ENVIRONMENT "ASAN_OPTIONS=detect_leaks=0"
)
# SB0: sandboxed environment test harness
add_test(
  NAME tur_eval_sandbox
  COMMAND "${CMAKE_BINARY_DIR}/tur_eval_sandbox"
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)
# turi-interp-collections-libturi: Vec/Set/HAMT parity through the libturi embed
# API. Leak detection OFF -- the tree-walker's collection buffers are
# process-lifetime (matching the other turi harnesses' policy).
add_test(
  NAME tur_collections_embed
  COMMAND "${CMAKE_BINARY_DIR}/tur_collections_embed"
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)
set_tests_properties(tur_collections_embed PROPERTIES
  ENVIRONMENT "ASAN_OPTIONS=detect_leaks=0"
)
# turi-interp-stdlib-natives-libturi: option/result/str/math embed-API parity.
# Leak detection OFF -- the tree-walker's carrier allocations are
# process-lifetime (matching the other turi harnesses' policy).
add_test(
  NAME tur_stdlib_natives_embed
  COMMAND "${CMAKE_BINARY_DIR}/tur_stdlib_natives_embed"
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)
set_tests_properties(tur_stdlib_natives_embed PROPERTIES
  ENVIRONMENT "ASAN_OPTIONS=detect_leaks=0"
)
# LeakSanitizer ships with ASan on Linux but is ABSENT on macOS, where
# detect_leaks=1 aborts at process startup ("detect_leaks is not supported on
# this platform") before the harness runs at all. The two leak harnesses below
# force detection ON, so gate it by platform: keep the leak assertion as a
# Linux-CI guarantee, and on macOS fall back to detect_leaks=0 (the
# create/eval/free cycle still runs under ASan's use-after-free / OOB checks,
# just without the leak count). See CLAUDE.md "Leak detection (ASan/LSan)".
if(APPLE)
  set(TUR_LEAK_HARNESS_ASAN "ASAN_OPTIONS=detect_leaks=0")
else()
  set(TUR_LEAK_HARNESS_ASAN "ASAN_OPTIONS=detect_leaks=1")
endif()
# libturi-per-embed-env-and-peripherals: Gaps 1-4 embed-API harness. Runs with
# leak detection FORCED ON (Linux) -- the create/reset/free cycles plus the
# owned module_base_dir copy must leave zero turi-attributable live allocations.
add_test(
  NAME tur_embed_peripherals
  COMMAND "${CMAKE_BINARY_DIR}/tur_embed_peripherals"
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)
set_tests_properties(tur_embed_peripherals PROPERTIES
  ENVIRONMENT "${TUR_LEAK_HARNESS_ASAN}"
)
# turi-env-owned-value-arena-pool-plan: env-teardown leak harness. Unlike the
# other turi targets (which default detect_leaks=0 because the interpreter
# never frees its process-lifetime closures), this one runs with leak
# detection FORCED ON -- a create/eval/free cycle must leave zero
# turi-attributable live allocations once the value pool is reclaimed.
add_test(
  NAME tur_env_teardown
  COMMAND "${CMAKE_BINARY_DIR}/tur_env_teardown"
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)
set_tests_properties(tur_env_teardown PROPERTIES
  ENVIRONMENT "${TUR_LEAK_HARNESS_ASAN}"
)
# turi-value-pool-scratch-promotion-plan: long-lived-env steady-state + cross-
# reset correctness harness. Exercises the scratch/permanent split with escape
# promotion enabled. Leak detection OFF: the control env deliberately runs with
# promotion disabled (the process-lifetime interpreter pattern) and never frees
# its transient value pool until turi_env_free, so LSan would flag that choice;
# ASan's use-after-poison checking (the point of this test) stays on.
add_test(
  NAME tur_env_longlived
  COMMAND "${CMAKE_BINARY_DIR}/tur_env_longlived"
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)
set_tests_properties(tur_env_longlived PROPERTIES
  ENVIRONMENT "ASAN_OPTIONS=detect_leaks=0"
)
add_test(
  NAME tur_codegen_reinterpret
  COMMAND "${CMAKE_BINARY_DIR}/tur_codegen_reinterpret"
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)
# Semver + :tur-version range unit test: pins the two bugs pkg_semver_* carried
# while dead (pre-release ignored, trailing garbage accepted) plus the range
# grammar, including the 0.x caret rule.
add_test(
  NAME tur_semver_range_unit
  COMMAND "${CMAKE_BINARY_DIR}/tur_semver_range_unit"
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)
# Theme E (TP3): arbitrary-arity Kind helper round-trip unit test
add_test(
  NAME tur_kind_arity_unit
  COMMAND "${CMAKE_BINARY_DIR}/tur_kind_arity_unit"
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)
# structdef-retirement DS-D: tur_heap_abi_unit retired with StructDef (the :heap
# typed-pointer lowering is now on the record-ADT path, fixture-covered).
# Variadic HKT rows (Direction 1): kind-level List Type (KIND_TYPEROW) unit test
add_test(
  NAME tur_kind_row_unit
  COMMAND "${CMAKE_BINARY_DIR}/tur_kind_row_unit"
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)
# Variadic HKT rows (Layer 2): TY_TYPEROW type-variant unit test
add_test(
  NAME tur_typerow_unit
  COMMAND "${CMAKE_BINARY_DIR}/tur_typerow_unit"
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)
# TY4: lifetime elision + outlives-cycle unit test
add_test(
  NAME tur_lifetime_unit
  COMMAND "${CMAKE_BINARY_DIR}/tur_lifetime_unit"
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)
set_tests_properties(tur_lifetime_unit PROPERTIES
  ENVIRONMENT "ASAN_OPTIONS=detect_leaks=0"
)
# CPS4 (cps-transform-plan): heap-reified continuation + trampoline runtime unit
add_test(
  NAME tur_cps_rt_unit
  COMMAND "${CMAKE_BINARY_DIR}/tur_cps_rt_unit"
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)
# CPS5 (cps-transform-plan): multi-prompt delimited-control machine unit
add_test(
  NAME tur_cps_prompt_unit
  COMMAND "${CMAKE_BINARY_DIR}/tur_cps_prompt_unit"
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)
# Reactor slot recycling (deferred by one poll -- see the note in reactor.c)
add_test(
  NAME tur_reactor_slot_reuse_unit
  COMMAND "${CMAKE_BINARY_DIR}/tur_reactor_slot_reuse_unit"
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)
# Graceful socket close through the reactor (tur_reactor_linger_close)
add_test(
  NAME tur_reactor_linger_unit
  COMMAND "${CMAKE_BINARY_DIR}/tur_reactor_linger_unit"
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)
# Spice lockfile integrity hash (pkg_hash_dir)
add_test(
  NAME tur_pkg_hash
  COMMAND "${CMAKE_BINARY_DIR}/tur_pkg_hash"
  WORKING_DIRECTORY "${CMAKE_BINARY_DIR}"
)
# SHA-256 unit test (runtime/sha256.c, shared by image.c and pkg_hash_dir)
add_test(
  NAME tur_sha256_unit
  COMMAND "${CMAKE_BINARY_DIR}/tur_sha256_unit"
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)
# AI1 (application-image-dumps-plan): image header codec unit test
add_test(
  NAME tur_image_unit
  COMMAND "${CMAKE_BINARY_DIR}/tur_image_unit"
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)
# LSP docstring scanner unit test
add_test(
  NAME tur_docscanner_unit
  COMMAND "${CMAKE_BINARY_DIR}/tur_docscanner_unit"
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)
# Transport-free LSP session tests (try-turmeric-lsp-plan, §4).  Drives
# scripted JSON-RPC through lsp_session_handle with a stubbed analysis
# backend -- no pipe, no subprocess, and no dependence on what the stdlib
# happens to export today.  This is the browser path, tested without a browser.
add_test(
  NAME tur_lsp_session_unit
  COMMAND "${CMAKE_BINARY_DIR}/tur_lsp_session_unit"
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)
# The playground's analysis backend (src/web/wasm_lsp.c), run natively against
# the in-tree stdlib.  It normally only executes inside a wasm module, where a
# mistake surfaces months later as "completion in the browser is empty";
# nothing in it is actually wasm-specific, so it runs here instead.
add_test(
  NAME tur_lsp_wasm_backend_unit
  COMMAND "${CMAKE_BINARY_DIR}/tur_lsp_wasm_backend_unit" "${CMAKE_SOURCE_DIR}/stdlib"
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)
# The browser's language-mode entry points (set_lang / get_lang /
# lang_registry), run natively against libturi_wasm
# (try-turmeric-lang-toggle-plan).  cwd matters: the glue preloads the
# stdlib from the cwd-relative "stdlib" root, mirroring the MEMFS mount.
# The interpreter env's closures/natives are process-lifetime by design, so
# leak detection is off, matching the other turi-driving harnesses.
# SX8c: turi_smt_check, the playground's door onto the refinement solver.
add_test(
  NAME tur_wasm_glue_smt_unit
  COMMAND "${CMAKE_BINARY_DIR}/tur_wasm_glue_smt_unit"
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)
set_tests_properties(tur_wasm_glue_smt_unit PROPERTIES TIMEOUT 120)

add_test(
  NAME tur_wasm_glue_lang_unit
  COMMAND "${CMAKE_BINARY_DIR}/tur_wasm_glue_lang_unit"
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)
set_tests_properties(tur_wasm_glue_lang_unit PROPERTIES
  ENVIRONMENT "ASAN_OPTIONS=detect_leaks=0"
)
# playground-session-hygiene-plan: doc-panel lookups must not touch the eval
# session, `(doc ...)` must evaluate, and Run must be re-runnable.
add_test(
  NAME tur_wasm_glue_session_unit
  COMMAND "${CMAKE_BINARY_DIR}/tur_wasm_glue_session_unit"
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)
set_tests_properties(tur_wasm_glue_session_unit PROPERTIES
  ENVIRONMENT "ASAN_OPTIONS=detect_leaks=0"
)
# Injective name-mangler round-trip + injectivity unit test
add_test(
  NAME tur_mangle_unit
  COMMAND "${CMAKE_BINARY_DIR}/tur_mangle_unit"
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)
# Phase S3: algebraic effects via eval core
add_test(
  NAME tur_eval_effects
  COMMAND bash tests/turi/eval-effects.sh
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)
# Phase S4: structs, exceptions, defer
add_test(
  NAME tur_eval_s4
  COMMAND bash tests/turi/eval-s4.sh
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)
# TCO: tail call optimization in eval
add_test(
  NAME tur_eval_tco
  COMMAND bash tests/turi/eval-tco.sh
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)

# Phase S7: async/await, fiber scheduler
add_test(
  NAME tur_eval_async_basic
  COMMAND bash tests/turi/eval-async-basic.sh
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)
add_test(
  NAME tur_eval_async_composition
  COMMAND bash tests/turi/eval-async-composition.sh
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)
add_test(
  NAME tur_eval_async_effects
  COMMAND bash tests/turi/eval-async-effects.sh
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)
add_test(
  NAME tur_eval_async_timeout
  COMMAND bash tests/turi/eval-async-timeout.sh
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)
add_test(
  NAME tur_eval_async_cancel
  COMMAND bash tests/turi/eval-async-cancel.sh
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)
add_test(
  NAME tur_eval_async_error
  COMMAND bash tests/turi/eval-async-error.sh
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)
add_test(
  NAME tur_eval_async_io
  COMMAND bash tests/turi/eval-async-io.sh
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)
# Phase TI4: Software Transactional Memory (tvar/atomically/stm/or-else) under
# the interpreter.  Runs via `tur --interpret` (not `tur run`, which compiles).
add_test(
  NAME tur_eval_stm
  COMMAND bash tests/turi/eval-stm.sh
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)

# Make sure tur is built before any test is run
set_tests_properties(tur_tests tur_cli_tests tur_span_tests tur_flags_tests
                     tur_install_tests
                     turi_fixture_tests
                     tur_repl_tests tur_repl_smoke tur_eval_import tur_eval_basic
                     tur_eval_sandbox tur_codegen_reinterpret tur_ffi_dispatch_unit
                     tur_eval_effects tur_eval_s4 tur_eval_tco
                     tur_eval_async_basic tur_eval_async_composition
                     tur_eval_async_effects tur_eval_async_timeout
                     tur_eval_async_cancel tur_eval_async_error tur_eval_async_io
                     tur_eval_stm tur_env_teardown tur_env_longlived tur_embed_peripherals
                     tur_collections_embed tur_stdlib_natives_embed
                     tur_reader_macro_cross_eval tur_incremental_elab_diff
                     tur_lang_switch_prelude tur_session_failure_recovery
                     tur_session_redefinition
                     tur_show_collection_elems
                     tur_macro_env_nested
                     tur_gc_runtime_copy_parity tur_cc_warn_ratchet
                     tur_experiments_user_config_e2e
  PROPERTIES DEPENDS tur
)

# LeakSanitizer ships enabled with ASan on Linux but is absent on macOS, so the
# Debug/ASan CI run on Ubuntu would otherwise fail on a deliberate design
# choice: the tree-walking turi/eval interpreter intentionally does not free
# its closures or registered-native environment entries (they live for the
# process lifetime -- see turi_env_register_native / eval_expr_impl). Disable
# leak detection for the targets that exercise the interpreter so the test run
# still catches real memory errors (use-after-free, out-of-bounds) without
# flagging that choice.
#
# The compiler/codegen path used to leak too: emit_resolve_type and
# emit_abi_instantiate_type cloned transient Type nodes for ABI specialization
# that were never freed. Those are now arena-managed and bulk-freed at the end
# of each emit pass (see EmitCtx.type_arena), so the pure compiler-path targets
# below run WITH leak detection ON as a regression guard. tur_eval_import sets
# ASAN_OPTIONS via its own ENVIRONMENT above.
set_tests_properties(tur_flags_tests
                     tur_install_tests
                     turi_fixture_tests
                     tur_repl_tests tur_repl_smoke tur_eval_basic
                     tur_eval_sandbox tur_codegen_reinterpret tur_ffi_dispatch_unit
                     tur_eval_effects tur_eval_s4 tur_eval_tco
                     tur_eval_async_basic tur_eval_async_composition
                     tur_eval_async_effects tur_eval_async_timeout
                     tur_eval_async_cancel tur_eval_async_error tur_eval_async_io
                     tur_eval_stm
  PROPERTIES ENVIRONMENT "ASAN_OPTIONS=detect_leaks=0"
)

# ---------------------------------------------------------------------------
# Examples (opt-in — requires Raylib)
# OFF by default so that `cmake --build` / `ctest` never trigger a Raylib
# compile.  Enable with:  cmake -DTUR_EXAMPLES=ON ...
# or via:  just configure-examples
# ---------------------------------------------------------------------------
option(TUR_EXAMPLES "Build example programs (requires Raylib)" OFF)
if (TUR_EXAMPLES)
    add_subdirectory(examples)
endif()
