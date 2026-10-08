include_guard(GLOBAL)
# ---------------------------------------------------------------------------
# MIR (c2mir + MIR-gen) -- the in-process JIT engine behind `tur jit`.
# docs/archive/jit-engine-plan.md (J0 chose it), docs/guides/jit-guide.md.
# ---------------------------------------------------------------------------
# MIR is a C11 front end (c2mir) plus an optimizing JIT back end, so `tur`'s
# existing emit-C path is reused verbatim and no per-architecture instruction
# selection is written at all.
#
# The sources are VENDORED under external/mir/: exactly the three translation
# units below and every file they #include, copied from the turmeric-lang/mir fork
# at the commit external/mir/UPSTREAM records.  external/mir/VENDORED.md has the
# fixes the fork carries over upstream and how to change them -- never by
# editing the copy; tools/update-mir.sh re-syncs it from the fork.
#
# Until 2026-10-02 this file cloned the fork with FetchContent at configure
# time.  That network fetch is why TUR_JIT defaulted OFF -- a default configure
# must not reach the network, the same posture as the Z3 block in the top-level
# CMakeLists -- and why the pin lived in a CACHE variable an existing build dir
# kept silently.  The copy removes both: no configure reaches the network, and
# the pin is a file in the tree.
#
# TUR_MIR_SOURCE_DIR builds against another MIR tree instead -- a local
# checkout of the fork, to try a MIR change before it is merged and synced.
set(TUR_MIR_SOURCE_DIR "${CMAKE_SOURCE_DIR}/external/mir"
    CACHE PATH "MIR source tree for the JIT engine (default: the vendored copy)")
set(mir_SOURCE_DIR "${TUR_MIR_SOURCE_DIR}")
if(NOT EXISTS "${mir_SOURCE_DIR}/mir.c" OR NOT EXISTS "${mir_SOURCE_DIR}/c2mir/c2mir.c")
  message(FATAL_ERROR
    "TUR_MIR_SOURCE_DIR=${mir_SOURCE_DIR} holds no MIR sources (mir.c, "
    "c2mir/c2mir.c).  The vendored copy lives in external/mir/; restore it "
    "with `git checkout -- external/mir` or `bash tools/update-mir.sh`, or "
    "configure with -DTUR_JIT=OFF to build without the JIT engine.")
endif()
if(EXISTS "${mir_SOURCE_DIR}/UPSTREAM")
  file(STRINGS "${mir_SOURCE_DIR}/UPSTREAM" _tur_mir_commit REGEX "^MIR_COMMIT=")
  string(REPLACE "MIR_COMMIT=" "" _tur_mir_commit "${_tur_mir_commit}")
  message(STATUS "JIT engine: MIR ${_tur_mir_commit} (${mir_SOURCE_DIR})")
else()
  message(STATUS "JIT engine: MIR from ${mir_SOURCE_DIR}")
endif()

# We declare exactly the three TUs `tur` links -- the IR/loader core, the
# generator, and the C front end -- rather than add_subdirectory'ing MIR's own
# CMakeLists, which adds c2m, m2b, b2m, an llvm2mir target that runs
# find_package(LLVM REQUIRED), and a full ctest suite.
if(NOT TARGET tur_mir)
  add_library(tur_mir STATIC
    "${mir_SOURCE_DIR}/mir.c"
    "${mir_SOURCE_DIR}/mir-gen.c"
    "${mir_SOURCE_DIR}/c2mir/c2mir.c"
  )
  target_include_directories(tur_mir PUBLIC
    "${mir_SOURCE_DIR}"
    "${mir_SOURCE_DIR}/c2mir"
  )
  # MIR wants gnu11 + -fsigned-char and is built -O3 regardless of the enclosing
  # build type: an -O0 MIR-gen would make every compile-latency number in the
  # J0 report meaningless.  It is also deliberately NOT sanitized --
  # ASan-instrumenting the generator skews compile latency and (per plan
  # section 6) cannot see inside JIT'd code anyway.
  target_compile_options(tur_mir PRIVATE
    -O3 -std=gnu11 -fsigned-char -fPIC -w
  )
  find_package(Threads)
  if(Threads_FOUND)
    target_compile_definitions(tur_mir PUBLIC "MIR_PARALLEL_GEN")
    target_link_libraries(tur_mir PUBLIC Threads::Threads)
  endif()
  if(UNIX)
    target_link_libraries(tur_mir PUBLIC m ${CMAKE_DL_LIBS})
  endif()
endif()
