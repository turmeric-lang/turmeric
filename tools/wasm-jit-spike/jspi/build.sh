#!/usr/bin/env bash
# build.sh -- build the browser JIT module with wasm-native setjmp/longjmp
# (wasm exception handling) instead of Emscripten's JS-based one, and
# optionally with JSPI in place of Asyncify.  The JSPI spike of
# raylib-web-playground-plan prerequisite 3, and the measured fix of
# docs/reported/wasm-jit-recursion-depth-is-about-300.md.
#
#   bash tools/wasm-jit-spike/jspi/build.sh OUT_DIR            # wasm setjmp only
#   bash tools/wasm-jit-spike/jspi/build.sh OUT_DIR jspi       # plus -sJSPI
#
# It derives a variant of tools/wasm-jit/build-module.sh rather than copying
# it: every compile gets -fwasm-exceptions, emscripten_sleep joins the import
# table, and the link gets -fwasm-exceptions -sSUPPORT_LONGJMP=wasm (and with
# `jspi`, -sJSPI -sJSPI_EXPORTS=turi_wasm_jit_run, so turi_wasm_jit_run returns
# a Promise).  Each substitution asserts its text is still there, so a change
# to the real script fails here loudly instead of silently building the wrong
# thing.  Needs a configured build-wasm/ (its sources.txt and stdlib package)
# and the emsdk environment, as the real script does.  OUT_DIR is a full
# object build on first use; a second run with the other mode only relinks.
set -euo pipefail
OUT=$1
MODE=${2:-sjlj}
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
rep('BASE=($INCS -O2 -std=c11', 'BASE=($INCS -fwasm-exceptions -O2 -std=c11')
rep('put_rsp "$OUT/mir.rsp" "-I$MIR" "-I$MIR/c2mir" -O2 -std=gnu11',
    'put_rsp "$OUT/mir.rsp" "-I$MIR" "-I$MIR/c2mir" -fwasm-exceptions -O2 -std=gnu11')
rep('&& emcc -c "$OUT/tur_wasm_jit_symtab_rt.c" -o "$OUT/symtab_rt.o" -O2 -w',
    '&& emcc -c "$OUT/tur_wasm_jit_symtab_rt.c" -o "$OUT/symtab_rt.o" -fwasm-exceptions -O2 -w')
rep('&& emcc -c "$OUT/tur_wasm_jit_stubs.c" -o "$OUT/stubs.o" -O2 -w',
    '&& emcc -c "$OUT/tur_wasm_jit_stubs.c" -o "$OUT/stubs.o" -fwasm-exceptions -O2 -w')
rep('    --stubs "$OUT/tur_wasm_jit_stubs.c"\n',
    '    --stubs "$OUT/tur_wasm_jit_stubs.c"\n'
    '  sed -i \'s|^static const tur_wasm_sym tur_wasm_symtab\\[\\] = {|void emscripten_sleep (unsigned int);\\n&\\n  {"emscripten_sleep", (void *) \\&emscripten_sleep},|\' "$SYMTAB"\n')
rep('  -O2 -Wno-pthreads-mem-growth\n', '  -O2 -Wno-pthreads-mem-growth $EXTRA_LINK\n')
# the derived script lives in OUT_DIR; its helpers stay in tools/wasm-jit
rep('HERE=$(cd "$(dirname "$0")" && pwd)', 'HERE=$TUR_WASM_JIT_TOOLS')
open(sys.argv[2], 'w').write(src)
EOF
EXTRA_LINK="-fwasm-exceptions -sSUPPORT_LONGJMP=wasm"
[ "$MODE" = jspi ] && EXTRA_LINK="$EXTRA_LINK -sJSPI -sJSPI_EXPORTS=turi_wasm_jit_run"
export EXTRA_LINK TUR_WASM_JIT_TOOLS="$ROOT/tools/wasm-jit"
bash "$OUT/build-module-sjlj.sh" "$OUT/wasm-jit" "$BUILD/wasm-jit/sources.txt" "$VERSION" \
     "${TUR_MIR_SOURCE_DIR:-$ROOT/external/mir}" "$BUILD/stdlib-pkg"
echo "build.sh ($MODE): $OUT/wasm-jit/turmeric-jit.{js,wasm}"
