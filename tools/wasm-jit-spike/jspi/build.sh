#!/usr/bin/env bash
# build.sh -- build the browser JIT module with JSPI, so a compiled program's
# emscripten_sleep suspends the wasm stack and the page gets control back
# between frames.  The JSPI spike of raylib-web-playground-plan prerequisite
# 3.  (The wasm-native setjmp it first measured, the fix of
# docs/reported/wasm-jit-recursion-depth-is-about-300.md, is the default
# build's link mode since 2026-10-30, so this script no longer adds it.)
#
#   bash tools/wasm-jit-spike/jspi/build.sh OUT_DIR            # the sleep import only
#   bash tools/wasm-jit-spike/jspi/build.sh OUT_DIR jspi       # plus -sJSPI
#
# It derives a variant of tools/wasm-jit/build-module.sh rather than copying
# it: emscripten_sleep joins the import table, and with `jspi` the link gets
# -sJSPI -sJSPI_EXPORTS=turi_wasm_jit_run, so turi_wasm_jit_run returns a
# Promise.  Each substitution asserts its text is still there, so a change to
# the real script fails here loudly instead of silently building the wrong
# thing.  Needs a configured build-wasm/ (its sources.txt and stdlib package)
# and the emsdk environment, as the real script does.  OUT_DIR is a full
# object build on first use; a second run with the other mode only relinks.
set -euo pipefail
OUT=$1
MODE=${2:-sleep}
ROOT=$(cd "$(dirname "$0")/../../.." && pwd)
BUILD=${TUR_WASM_BUILD_DIR:-$ROOT/build-wasm}
[ -f "$BUILD/wasm-jit/sources.txt" ] || { echo "build.sh: no $BUILD/wasm-jit/sources.txt (configure build-wasm with -DTUR_WASM=ON and build tur_wasm_jit once)" >&2; exit 2; }
VERSION=$(tr -d '\n' < "$ROOT/VERSION")
mkdir -p "$OUT"
python3 - "$ROOT/tools/wasm-jit/build-module.sh" "$OUT/build-module-sjlj.sh" <<'EOF'
import sys
src = open(sys.argv[1]).read()
def rep(old, new):
    global src
    assert src.count(old) == 1, "build-module.sh changed under the spike: " + old[:60]
    src = src.replace(old, new)
# the base build already links setjmp/longjmp on wasm exception handling,
# which JSPI needs (a JS invoke_* frame under eval breaks the suspension)
rep('"${SJLJ_CFLAGS[@]}" -sSUPPORT_LONGJMP=wasm', '"${SJLJ_CFLAGS[@]}" -sSUPPORT_LONGJMP=wasm')
rep('    --stubs "$OUT/tur_wasm_jit_stubs.c"\n',
    '    --stubs "$OUT/tur_wasm_jit_stubs.c"\n'
    '  sed -i \'s|^static const tur_wasm_sym tur_wasm_symtab\\[\\] = {|void emscripten_sleep (unsigned int);\\n&\\n  {"emscripten_sleep", (void *) \\&emscripten_sleep},|\' "$SYMTAB"\n')
rep('  -O2 -Wno-pthreads-mem-growth\n', '  -O2 -Wno-pthreads-mem-growth $EXTRA_LINK\n')
# the derived script lives in OUT_DIR; its helpers stay in tools/wasm-jit
rep('HERE=$(cd "$(dirname "$0")" && pwd)', 'HERE=$TUR_WASM_JIT_TOOLS')
open(sys.argv[2], 'w').write(src)
EOF
EXTRA_LINK=""
[ "$MODE" = jspi ] && EXTRA_LINK="-sJSPI -sJSPI_EXPORTS=turi_wasm_jit_run"
export EXTRA_LINK TUR_WASM_JIT_TOOLS="$ROOT/tools/wasm-jit"
bash "$OUT/build-module-sjlj.sh" "$OUT/wasm-jit" "$BUILD/wasm-jit/sources.txt" "$VERSION" \
     "${TUR_MIR_SOURCE_DIR:-$ROOT/external/mir}" "$BUILD/stdlib-pkg"
echo "build.sh ($MODE): $OUT/wasm-jit/turmeric-jit.{js,wasm}"
