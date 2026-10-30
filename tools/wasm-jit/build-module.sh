#!/usr/bin/env bash
# tools/wasm-jit/build-module.sh -- build the browser JIT module,
# turmeric-jit.{js,wasm} (wasm-jit-plan P3).  Run by the tur_wasm_jit CMake
# target; it can be run by hand with the same arguments.
#
#   build-module.sh OUT_DIR SOURCES_FILE VERSION [MIR_DIR [STDLIB_PKG_DIR]]
#
# SOURCES_FILE lists one source per line as "<group> <path>", paths absolute:
#   rt      the stock module's sources (tur_core and the web glue) and tur_front
#   engine  jit_engine.c, turi/jit_ffi.c and the JIT glue: rt flags plus
#           TUR_HAVE_JIT and MIR's include dirs
#   mir     MIR's translation units, with MIR's own flags
# MIR_DIR is the MIR tree those come from and whose headers the engine
# includes: external/mir unless CMake's TUR_MIR_SOURCE_DIR says otherwise.
# STDLIB_PKG_DIR holds the shared stdlib package (stdlib.data and its loader
# stdlib.data.js, CMake's tur_stdlib_pkg; wasm-jit-plan 5.11); it defaults to
# OUT_DIR's sibling stdlib-pkg/, which is where CMake writes it.
# Needs emcc and em-config on PATH (the emsdk environment), python3 and make.
#
# Extras (raylib-web-playground-plan J0/J1: the arcade module is this module
# plus raylib, not a fork of the script), all optional, all from the
# environment:
#   TUR_WASM_JIT_EXTRA_OBJECTS   objects and archives to link in, whitespace-
#                                separated (libraylib.a, rl_web_loop.o).  Each
#                                object, and every member of an archive, joins
#                                the --wasm-objects list, so its functions get
#                                import-table rows at their wasm signatures and
#                                exact-signature call stubs -- J1 by
#                                construction, struct-by-value calls included,
#                                since at the wasm level an aggregate is an
#                                i32 address on both sides.
#   TUR_WASM_JIT_EXTRA_HEADERS   header files copied into /sysinclude by
#                                basename (raylib.h), for the program's
#                                inline C to #include.
#   TUR_WASM_JIT_EXTRA_LINK      extra emcc link flags, word-split (-sUSE_GLFW=3,
#                                an --embed-file of a spice's src/, ...).
#
# Four steps:
#   1. compile every source to an object under OUT_DIR/obj, through a
#      generated Makefile so a rerun rebuilds only what changed (-MMD);
#   2. generate the import table (gen-symtab.py) from the runtime's
#      declarations region and what the objects define, and compile it;
#   3. collect the headers c2mir may open: the musl closure of
#      tools/wasm-jit/headers.txt, tools/wasm-jit/stdatomic.h, runtime/hamt.h
#      and turi/r7rs_embed.h;
#   4. link, with the shared stdlib package's loader (it mounts /stdlib) and
#      the headers embedded at /sysinclude and /src.
set -euo pipefail

OUT=$1
SOURCES=$2
VERSION=$3
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
MIR=${4:-$ROOT/external/mir}
PKG=${5:-$(dirname "$OUT")/stdlib-pkg}
[ -f "$PKG/stdlib.data.js" ] || { echo "build-module.sh: no stdlib package in $PKG (cmake --build <dir> --target tur_stdlib_pkg)" >&2; exit 2; }
JOBS=${TUR_WASM_JIT_JOBS:-$(nproc 2>/dev/null || echo 4)}

command -v emcc >/dev/null || { echo "build-module.sh: emcc is not on PATH" >&2; exit 2; }
LLVM_BIN=$(em-config LLVM_ROOT)
SYSINC=$(em-config CACHE)/sysroot/include
NM=$LLVM_BIN/llvm-nm
mkdir -p "$OUT/obj"

# --- 1. objects --------------------------------------------------------------
INCS="-I$ROOT/src -I$ROOT/src/compiler -I$ROOT/src/passes -I$ROOT/src/runtime -I$ROOT/src/async -I$ROOT/src/web -I$ROOT/src/lsp -I$ROOT/src/driver"
# Flags go through response files, so TUR_VERSION's quotes survive both the
# Makefile's shell and a direct emcc call (clang reads \" in one as a quote).
# A file is rewritten only when its flags change, and every object depends on
# its group's file, so a flag change rebuilds exactly what it affects.
put_rsp() {   # put_rsp FILE FLAG...
  local f=$1; shift
  printf '%s\n' "$@" > "$f.new"
  if cmp -s "$f.new" "$f"; then rm -f "$f.new"; else mv "$f.new" "$f"; fi
}
# setjmp/longjmp on wasm exception handling (-fwasm-exceptions at every
# compile, -sSUPPORT_LONGJMP=wasm at the link), not Emscripten's JS-based
# default.  MIR's interpreter calls setjmp in eval's own frame on wasm
# (mir#17), and under the JS mode LLVM then sends every call eval makes
# through a JavaScript invoke_* wrapper: a JS frame per interpreted call,
# which cut a Turmeric recursion to about 300 frames in a worker and made
# every call a JS round trip.  Under wasm EH the landing is a try region and
# the calls are plain; measured 700 frames in the worker, 1,500 under node,
# and no change across the corpus
# (docs/reported/wasm-jit-recursion-depth-is-about-300.md).  The one thing
# this rules out is Asyncify, which the module never used; a page that needs
# a compiled program to yield uses JSPI on this build
# (tools/wasm-jit-spike/jspi/).
SJLJ_CFLAGS=(-fwasm-exceptions)
# TUR_WASM_JIT_JSPI=1 builds the JSPI variant, the raylib playground's
# (raylib-web-playground-plan J4): turi_wasm_jit_run returns a Promise, and a
# compiled program's emscripten_sleep -- raylib's frame loop, or inline C,
# which the import table then carries -- suspends the wasm stack until the
# page's event loop has run, with no change to the wasm itself.  It needs this
# build's wasm-native setjmp (a JS frame under eval breaks the suspension) and
# a browser with JSPI (Chrome 137+ today), which is why it is not the default:
# Try Turmeric's Compile engine has to load everywhere.  Only the import table
# and the link differ, so flipping it relinks.
JSPI=${TUR_WASM_JIT_JSPI:-0}
JSPI_LINK=()
SYMTAB_EXTRA=()
if [ "$JSPI" = 1 ]; then
  JSPI_LINK=(-sJSPI -sJSPI_EXPORTS=turi_wasm_jit_run)
  SYMTAB_EXTRA=(--jspi)
fi
# shellcheck disable=SC2086
# -msimd128 as in tur_wasm: the interpreter's numvec natives vectorize
# (simd-and-vectorization-plan, open question 5).
BASE=($INCS "${SJLJ_CFLAGS[@]}" -O2 -msimd128 -std=c11 -Wall -Wextra -Wno-unused-parameter -D_DEFAULT_SOURCE -pthread
      "-DTUR_VERSION=\\\"$VERSION\\\"")
put_rsp "$OUT/rt.rsp" "${BASE[@]}" -pedantic
# The engine as tur_jit_obj builds it natively: no -pedantic, which MIR's
# headers do not pass (extra semicolons from its DLIST macros).
put_rsp "$OUT/engine.rsp" "${BASE[@]}" "-I$MIR" "-I$MIR/c2mir" -DTUR_HAVE_JIT=1
put_rsp "$OUT/mir.rsp" "-I$MIR" "-I$MIR/c2mir" "${SJLJ_CFLAGS[@]}" -O2 -std=gnu11 -fsigned-char -w -pthread
RT_FLAGS="@$OUT/rt.rsp"
ENGINE_FLAGS="@$OUT/engine.rsp"
MIR_FLAGS="@$OUT/mir.rsp"

MK=$OUT/objects.mk
{
  echo "# generated by tools/wasm-jit/build-module.sh"
  echo "all:"
  objs=()
  while read -r group src; do
    [ -n "$group" ] || continue
    case $group in
      rt) flags=$RT_FLAGS ;;
      engine) flags=$ENGINE_FLAGS ;;
      mir) flags=$MIR_FLAGS ;;
      *) echo "build-module.sh: unknown group '$group' in $SOURCES" >&2; exit 2 ;;
    esac
    rel=${src#"$ROOT"/}
    obj=$OUT/obj/$(printf '%s' "$rel" | tr '/' '_' | sed 's/\.c$/.o/')
    objs+=("$obj")
    echo "$obj: $src ${flags#@}"
    printf '\t@emcc -c %s -o %s -MMD -MP %s\n' "$src" "$obj" "$flags"
    echo "-include ${obj%.o}.d"
  done < "$SOURCES"
  echo "all: ${objs[*]}"
} > "$MK"
make -s -j"$JOBS" -f "$MK" all

# --- 2. the import table -------------------------------------------------------
DECLS=$ROOT/src/runtime/generated/tur_rt_split_decls.h
SYMTAB=$OUT/tur_wasm_jit_symtab.c
# The resident runtime -- what a program's imports can name: src/runtime and
# src/async (the generated runtime split among them), not the compiler or
# the interpreter.
ls "$OUT"/obj/src_runtime_*.o "$OUT"/obj/src_async_*.o > "$OUT/runtime-objects.txt"
# The extras (see the header): archives are unpacked so the generator can
# read each member's symbols and signatures.
EXTRA_OBJS=()
rm -rf "$OUT/extra"
if [ -n "${TUR_WASM_JIT_EXTRA_OBJECTS:-}" ]; then
  mkdir -p "$OUT/extra"
  # shellcheck disable=SC2086
  for x in $TUR_WASM_JIT_EXTRA_OBJECTS; do
    [ -f "$x" ] || { echo "build-module.sh: TUR_WASM_JIT_EXTRA_OBJECTS: no $x" >&2; exit 2; }
    EXTRA_OBJS+=("$x")
    case $x in
      *.a)
        d=$OUT/extra/$(basename "$x" .a)
        mkdir -p "$d"
        (cd "$d" && "$LLVM_BIN/llvm-ar" x "$x")
        ls "$d"/*.o >> "$OUT/runtime-objects.txt" ;;
      *) echo "$x" >> "$OUT/runtime-objects.txt" ;;
    esac
  done
fi
"$NM" --defined-only --extern-only "$OUT"/obj/*.o > "$OUT/nm.txt"
# Names the hosted declarations turn into macros (the thread-locals routed
# through tur_tls_* accessors) are not symbols: ask the preprocessor.
printf '#define TUR_RT_SPLIT_HOSTED 1\n#include "runtime/generated/tur_rt_split_decls.h"\n' > "$OUT/macro-probe.c"
# shellcheck disable=SC2086
emcc -E -dM "$OUT/macro-probe.c" $RT_FLAGS | awk '{print $2}' | sed 's/(.*//' | sort -u > "$OUT/exclude.txt"
# A name the region mentions only inside a macro or a comment has no
# declaration to take the address of: let the compiler name it, drop it, retry.
symtab_ok=0
for _attempt in 1 2 3 4 5 6 7 8; do
  python3 "$HERE/gen-symtab.py" "$DECLS" "$OUT/nm.txt" "$SYMTAB" "$OUT/exclude.txt" \
    --header "runtime/hamt.h=$ROOT/src/runtime/hamt.h" \
    --accessors "$ROOT/src/runtime/tur_tls.c" \
    --wasm-objects "$OUT/runtime-objects.txt" "$OUT/tur_wasm_jit_symtab_rt.c" \
    --stubs "$OUT/tur_wasm_jit_stubs.c" ${SYMTAB_EXTRA[@]+"${SYMTAB_EXTRA[@]}"}
  # shellcheck disable=SC2086
  if emcc -c "$SYMTAB" -o "$OUT/symtab.o" $RT_FLAGS -w 2> "$OUT/symtab-cc.log" \
     && emcc -c "$OUT/tur_wasm_jit_symtab_rt.c" -o "$OUT/symtab_rt.o" "${SJLJ_CFLAGS[@]}" -O2 -w 2>> "$OUT/symtab-cc.log" \
     && emcc -c "$OUT/tur_wasm_jit_stubs.c" -o "$OUT/stubs.o" "${SJLJ_CFLAGS[@]}" -O2 -w 2>> "$OUT/symtab-cc.log"; then
    symtab_ok=1
    break
  fi
  grep -o "use of undeclared identifier '[A-Za-z_0-9]*'" "$OUT/symtab-cc.log" \
    | grep -o "'.*'" | tr -d "'" >> "$OUT/exclude.txt"
done
[ "$symtab_ok" = 1 ] || { cat "$OUT/symtab-cc.log" >&2; exit 1; }

# --- 3. headers --------------------------------------------------------------
HDR=$OUT/hdr
rm -rf "$HDR"
mkdir -p "$HDR/sysinclude" "$HDR/src/runtime" "$HDR/src/turi"
grep -v '^\s*#' "$HERE/headers.txt" | grep -v '^\s*$' | sed 's/.*/#include <&>/' > "$OUT/headers-probe.c"
emcc -M -nostdinc -isystem "$SYSINC" -D_GNU_SOURCE "$OUT/headers-probe.c" -o "$OUT/headers.d"
tr ' \\' '\n\n' < "$OUT/headers.d" | grep "^$SYSINC/" | sort -u | while read -r h; do
  rel=${h#"$SYSINC"/}
  mkdir -p "$HDR/sysinclude/$(dirname "$rel")"
  cp "$h" "$HDR/sysinclude/$rel"
done
cp "$ROOT/src/runtime/hamt.h" "$HDR/src/runtime/hamt.h"
# What musl's sysroot lacks: stdatomic.h is the compiler's header, not libc's
# (this one is single-threaded, see its header), and the #lang r7rs eval
# twins include turi/r7rs_embed.h from stdlib/r7rs/eval.tur.
cp "$HERE/stdatomic.h" "$HDR/sysinclude/stdatomic.h"
cp "$ROOT/src/turi/r7rs_embed.h" "$HDR/src/turi/r7rs_embed.h"
# shellcheck disable=SC2086
for h in ${TUR_WASM_JIT_EXTRA_HEADERS:-}; do
  [ -f "$h" ] || { echo "build-module.sh: TUR_WASM_JIT_EXTRA_HEADERS: no $h" >&2; exit 2; }
  cp "$h" "$HDR/sysinclude/$(basename "$h")"
done

# --- 4. link -----------------------------------------------------------------
EXPORTS=_turi_wasm_init,_turi_wasm_reset,_turi_wasm_eval,_turi_wasm_set_compiled_repl,_turi_wasm_eval_ex,_turi_wasm_free_string,_turi_wasm_version,_turi_wasm_jit_run,_turi_wasm_add_include_dir,_turi_wasm_clear_include_dirs,_malloc,_free
METHODS=stringToUTF8,UTF8ToString,lengthBytesUTF8,ccall,cwrap,FS,ENV,wasmTable,addFunction,stackAlloc,stackSave,stackRestore
# STACK_OVERFLOW_CHECK=2 bounds-checks every write of the stack pointer.  At -O2
# nothing else does, and a program can reach the bottom of the 16 MiB linear
# stack before the JS stack limit: a large local in a recursion, the allocas a
# MIR tail call keeps, c2mir's per-call va buffers.  Unchecked, that overwrote
# memory below the stack and trapped later as an out-of-bounds access or a
# signature mismatch; checked, it aborts with "stack overflow" on stderr (under
# wasm EH the exception JavaScript sees is a plain `unreachable`, so the page
# reads the stderr line, web/public/jit-worker.js).  See
# docs/reported/wasm-linear-stack-overflow-unchecked.md.
# The check also turns on CHECK_NULL_WRITES, which plants a canary word at
# address 0, so a program that READS through a null pointer sees that word
# instead of the 0 it saw before and its answer changes.  The reads it found
# are a defect of their own, shape 4 of
# docs/reported/ilp32-word-slots-truncate-64-bit-carriers.md, so the canary
# stays off here rather than turning a stack check into a behaviour change.
# shellcheck disable=SC2086
emcc "$OUT"/obj/*.o "$OUT/symtab.o" "$OUT/symtab_rt.o" "$OUT/stubs.o" \
  ${EXTRA_OBJS[@]+"${EXTRA_OBJS[@]}"} ${TUR_WASM_JIT_EXTRA_LINK:-} -o "$OUT/turmeric-jit.js" \
  --pre-js "$PKG/stdlib.data.js" -sFORCE_FILESYSTEM=1 \
  --embed-file "$HDR/sysinclude@/sysinclude" \
  --embed-file "$HDR/src@/src" \
  -sMODULARIZE=1 -sEXPORT_NAME=TurmericJitModule \
  -sALLOW_MEMORY_GROWTH=1 -sALLOW_TABLE_GROWTH=1 -sWASM=1 \
  -pthread -sPTHREAD_POOL_SIZE_STRICT=0 -sEXIT_RUNTIME=0 \
  -sSTACK_SIZE=16777216 -sDEFAULT_PTHREAD_STACK_SIZE=8388608 \
  -sSTACK_OVERFLOW_CHECK=2 -sCHECK_NULL_WRITES=0 \
  "${SJLJ_CFLAGS[@]}" -sSUPPORT_LONGJMP=wasm \
  ${JSPI_LINK[@]+"${JSPI_LINK[@]}"} \
  -sINITIAL_MEMORY=134217728 \
  -sEXPORTED_FUNCTIONS="$EXPORTS" \
  -sEXPORTED_RUNTIME_METHODS="$METHODS" \
  -O2 -Wno-pthreads-mem-growth
# The loader fetches stdlib.data beside the module's script: put a copy here
# so node (run.js, tests/run-jit-wasm.sh) finds it as the page does.
cp "$PKG/stdlib.data" "$OUT/stdlib.data"
echo "turmeric-jit$([ "$JSPI" = 1 ] && echo " (JSPI)"): $(wc -c < "$OUT/turmeric-jit.wasm") bytes ($(grep -c . "$OUT/headers.d" >/dev/null; tr ' \\' '\n\n' < "$OUT/headers.d" | grep -c "^$SYSINC/") headers embedded)"
