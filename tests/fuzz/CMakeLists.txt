# tests/fuzz -- libFuzzer targets for the parsers that take untrusted bytes.
# security-audit-plan WP4; tests/fuzz/README.md has the how-to.
#
# Included from src/CMakeLists.txt when TUR_FUZZ is ON, so the src/ variables
# (TUR_INCLUDE_DIRS, TUR_CSTD) are in scope and tur_core is already compiled
# with -fsanitize=fuzzer-no-link (coverage for the code under test).  Each
# target links -fsanitize=fuzzer,address,undefined; the binaries land in
# <build>/fuzz/, and each is registered as a ctest that replays its seeds once
# (-runs=0), which is the smoke check the nightly job's real runs build on.

set(TUR_FUZZ_FLAGS -fsanitize=fuzzer,address,undefined -fno-sanitize-recover=undefined -g -O1)
set(TUR_FUZZ_OUT "${CMAKE_BINARY_DIR}/fuzz")
set(TUR_FUZZ_SRC "${CMAKE_SOURCE_DIR}/tests/fuzz")

# tur_fuzz_target(<name> SOURCES ... [LIBS ...] [NOWARN])
function(tur_fuzz_target name)
    cmake_parse_arguments(F "NOWARN" "" "SOURCES;LIBS" ${ARGN})
    add_executable(${name} ${F_SOURCES})
    target_include_directories(${name} PRIVATE
        ${TUR_INCLUDE_DIRS} ${CMAKE_SOURCE_DIR}/src ${TUR_FUZZ_SRC} ${CMAKE_CURRENT_BINARY_DIR})
    target_compile_options(${name} PRIVATE ${TUR_FUZZ_FLAGS} ${TUR_CSTD} -pthread)
    if(F_NOWARN)
        # Emitted C: the harness includes a whole generated program.
        target_compile_options(${name} PRIVATE -w)
    endif()
    target_link_options(${name} PRIVATE ${TUR_FUZZ_FLAGS} -pthread)
    target_link_libraries(${name} PRIVATE ${F_LIBS} ${CMAKE_DL_LIBS} m)
    set_target_properties(${name} PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${TUR_FUZZ_OUT}")
    add_test(NAME ${name}
             COMMAND ${name} -runs=0 "${TUR_FUZZ_SRC}/seeds/${name}"
             WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
endfunction()

# The targets over tur_core (reader, manifest, interpreter JSON) link its
# objects plus the one driver symbol they reference.
set(TUR_FUZZ_CORE $<TARGET_OBJECTS:tur_core> ${TUR_FUZZ_SRC}/fuzz_link_stub.c)
set(TUR_FUZZ_CORE_LIBS "")
if(HAVE_EDITLINE)
    list(APPEND TUR_FUZZ_CORE_LIBS ${LIBEDIT_LIB})
endif()

# --- self-contained: the code under test compiled straight in -------------
tur_fuzz_target(fuzz_lsp_frame
    SOURCES ${TUR_FUZZ_SRC}/fuzz_lsp_frame.c ${CMAKE_SOURCE_DIR}/src/lsp/lsp_io.c)
tur_fuzz_target(fuzz_image_header
    SOURCES ${TUR_FUZZ_SRC}/fuzz_image_header.c
            ${CMAKE_SOURCE_DIR}/src/runtime/image.c ${CMAKE_SOURCE_DIR}/src/runtime/sha256.c)
tur_fuzz_target(fuzz_serial_wire
    SOURCES ${TUR_FUZZ_SRC}/fuzz_serial_wire.c
            ${CMAKE_SOURCE_DIR}/src/runtime/serial.c ${CMAKE_SOURCE_DIR}/src/runtime/trail.c)

# --- over tur_core ----------------------------------------------------------
tur_fuzz_target(fuzz_reader
    SOURCES ${TUR_FUZZ_SRC}/fuzz_reader.c ${TUR_FUZZ_CORE} LIBS ${TUR_FUZZ_CORE_LIBS})
tur_fuzz_target(fuzz_manifest
    SOURCES ${TUR_FUZZ_SRC}/fuzz_manifest.c ${TUR_FUZZ_CORE} LIBS ${TUR_FUZZ_CORE_LIBS})
tur_fuzz_target(fuzz_json_interp
    SOURCES ${TUR_FUZZ_SRC}/fuzz_json_interp.c ${TUR_FUZZ_CORE} LIBS ${TUR_FUZZ_CORE_LIBS})
# Includes justrun.c itself (parse_justfile is static), so it links the
# libturi ARCHIVE: justrun.o is then never pulled, and nothing is defined
# twice.
tur_fuzz_target(fuzz_justfile
    SOURCES ${TUR_FUZZ_SRC}/fuzz_justfile.c ${TUR_FUZZ_SRC}/fuzz_link_stub.c
    LIBS libturi ${TUR_FUZZ_CORE_LIBS})

# --- over emitted C: stdlib inline C exactly as a compiled program ships it -
# `tur emit-c <entry>.tur` runs at build time (from the source root, where
# `(load "stdlib/...")` resolves); the harness #includes the result.
# hoist-includes.py then applies the one step `tur build` performs on emitted
# C that `tur emit-c` does not: hoisting `__tur_include__` payloads (httpd's
# HttpdConn typedef and statics) to file scope.
find_package(Python3 REQUIRED COMPONENTS Interpreter)
function(tur_fuzz_emitted name entry)
    cmake_parse_arguments(E "" "" "STDLIB;LIBS" ${ARGN})
    set(out "${CMAKE_CURRENT_BINARY_DIR}/${entry}.c")
    set(deps "")
    foreach(f ${E_STDLIB})
        list(APPEND deps "${CMAKE_SOURCE_DIR}/stdlib/${f}")
    endforeach()
    add_custom_command(
        OUTPUT "${out}"
        COMMAND sh -c "\"$<TARGET_FILE:tur>\" emit-c \"tests/fuzz/${entry}.tur\" > \"${out}.raw\""
        COMMAND ${Python3_EXECUTABLE} "${TUR_FUZZ_SRC}/hoist-includes.py" "${out}.raw" "${out}"
        DEPENDS tur "${TUR_FUZZ_SRC}/${entry}.tur" "${TUR_FUZZ_SRC}/hoist-includes.py" ${deps}
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
        COMMENT "Emitting ${entry}.c for ${name}"
        VERBATIM)
    # The generated file is #included, never compiled on its own; listing it
    # (header-only) plus OBJECT_DEPENDS makes the harness rebuild whenever
    # the entry program or the stdlib it loads changes.
    set_source_files_properties("${out}" PROPERTIES HEADER_FILE_ONLY TRUE GENERATED TRUE)
    set_source_files_properties("${TUR_FUZZ_SRC}/${name}.c" PROPERTIES OBJECT_DEPENDS "${out}")
    tur_fuzz_target(${name} SOURCES "${TUR_FUZZ_SRC}/${name}.c" "${out}" LIBS ${E_LIBS} NOWARN)
endfunction()

tur_fuzz_emitted(fuzz_json_compiled json_entry STDLIB json.tur)
tur_fuzz_emitted(fuzz_serial_cont serial_entry STDLIB serial.tur)
# httpd.tur autolinks -lturi -pthread, and loads json/string/effects.
tur_fuzz_emitted(fuzz_httpd_head httpd_entry
    STDLIB httpd.tur json.tur string.tur effects.tur
    LIBS libturi ${TUR_FUZZ_CORE_LIBS})

# One target that builds them all (the nightly job's build step).
add_custom_target(tur_fuzz_all DEPENDS
    fuzz_lsp_frame fuzz_image_header fuzz_serial_wire
    fuzz_reader fuzz_manifest fuzz_json_interp fuzz_justfile
    fuzz_json_compiled fuzz_serial_cont fuzz_httpd_head)
