#!/usr/bin/env bash
# link.sh -- relink the browser JIT module with extra emcc flags, for the
# Asyncify spike (raylib-web-playground-plan prerequisite 3).
#
#   bash tools/wasm-jit-spike/asyncify/link.sh OUT_DIR [emcc flag...]
#   bash tools/wasm-jit-spike/asyncify/link.sh /tmp/async -sASYNCIFY
#
# Reuses the objects, import table and headers of a built tur_wasm_jit target
# (build-wasm/wasm-jit; TUR_WASM_JIT_BUILD overrides) and adds one row to the
# import table, emscripten_sleep, so a program's inline C can yield.  The
# flags mirror tools/wasm-jit/build-module.sh's link; keep them in step.
set -euo pipefail
OUT=$1; shift
ROOT=$(cd "$(dirname "$0")/../../.." && pwd)
B=${TUR_WASM_JIT_BUILD:-$ROOT/build-wasm/wasm-jit}
PKG=$(dirname "$B")/stdlib-pkg
[ -f "$B/tur_wasm_jit_symtab.c" ] || { echo "link.sh: build the tur_wasm_jit target first ($B)" >&2; exit 2; }
mkdir -p "$OUT"
sed 's|^static const tur_wasm_sym tur_wasm_symtab\[\] = {|void emscripten_sleep (unsigned int);\n&\n  {"emscripten_sleep", (void *) \&emscripten_sleep},|' \
  "$B/tur_wasm_jit_symtab.c" > "$OUT/symtab.c"
grep -q '"emscripten_sleep"' "$OUT/symtab.c"
emcc -c "$OUT/symtab.c" -o "$OUT/symtab.o" "@$B/rt.rsp" -w
EXPORTS=_turi_wasm_init,_turi_wasm_reset,_turi_wasm_eval,_turi_wasm_set_compiled_repl,_turi_wasm_eval_ex,_turi_wasm_free_string,_turi_wasm_version,_turi_wasm_jit_run,_malloc,_free
METHODS=stringToUTF8,UTF8ToString,lengthBytesUTF8,ccall,cwrap,FS,ENV,wasmTable,addFunction,stackAlloc,stackSave,stackRestore
emcc "$B"/obj/*.o "$OUT/symtab.o" "$B/symtab_rt.o" "$B/stubs.o" -o "$OUT/turmeric-jit.js" \
  --pre-js "$PKG/stdlib.data.js" -sFORCE_FILESYSTEM=1 \
  --embed-file "$B/hdr/sysinclude@/sysinclude" \
  --embed-file "$B/hdr/src@/src" \
  -sMODULARIZE=1 -sEXPORT_NAME=TurmericJitModule \
  -sALLOW_MEMORY_GROWTH=1 -sALLOW_TABLE_GROWTH=1 -sWASM=1 \
  -pthread -sPTHREAD_POOL_SIZE_STRICT=0 -sEXIT_RUNTIME=0 \
  -sSTACK_SIZE=16777216 -sDEFAULT_PTHREAD_STACK_SIZE=8388608 \
  -sSTACK_OVERFLOW_CHECK=2 -sCHECK_NULL_WRITES=0 \
  -sINITIAL_MEMORY=134217728 \
  -sEXPORTED_FUNCTIONS="$EXPORTS" \
  -sEXPORTED_RUNTIME_METHODS="$METHODS" \
  -O2 -Wno-pthreads-mem-growth "$@"
cp "$PKG/stdlib.data" "$OUT/stdlib.data"
echo "link.sh: $OUT/turmeric-jit.wasm, $(wc -c < "$OUT/turmeric-jit.wasm") bytes"
