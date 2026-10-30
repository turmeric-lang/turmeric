#!/usr/bin/env bash
# build.sh -- the arcade module (raylib-web-playground-plan P0 / J0): the JSPI
# build of the browser JIT module with raylib's web archive, the raylib spice's
# one native TU (rl_web_loop.c) and raylib.h linked and embedded, and the
# raylib and math spices' src/ trees embedded at /spices/<name>/src for the
# page to put on the module search path (turi_wasm_add_include_dir).
#
#   RAYLIB_SRC=<raylib 5.5 checkout> RAYLIB_LIB=<web libraylib.a> \
#   TUR_SPICES=<turmeric-spices checkout> bash tools/wasm-jit-spike/arcade/build.sh OUT_DIR
#
# The archive is raylib built under emcmake with -DPLATFORM=Web
# -DGRAPHICS=GRAPHICS_API_OPENGL_ES3 and CMAKE_C_FLAGS="-pthread
# -fwasm-exceptions" (the module is a pthread, wasm-EH link; an archive built
# without either does not link into it).  Everything else is
# tools/wasm-jit-spike/jspi/build.sh with the extras of
# tools/wasm-jit/build-module.sh set; see there.
set -euo pipefail
OUT=$1
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../../.." && pwd)
: "${RAYLIB_SRC:?RAYLIB_SRC: a raylib 5.5 checkout}"
: "${RAYLIB_LIB:?RAYLIB_LIB: the web libraylib.a}"
: "${TUR_SPICES:?TUR_SPICES: a turmeric-spices checkout}"
[ -f "$RAYLIB_SRC/src/raylib.h" ] || { echo "build.sh: no $RAYLIB_SRC/src/raylib.h" >&2; exit 2; }
[ -f "$RAYLIB_LIB" ] || { echo "build.sh: no $RAYLIB_LIB" >&2; exit 2; }
[ -f "$TUR_SPICES/spices/raylib/c/raylib/rl_web_loop.c" ] || { echo "build.sh: no raylib spice under $TUR_SPICES" >&2; exit 2; }
command -v emcc >/dev/null || { echo "build.sh: emcc is not on PATH" >&2; exit 2; }
mkdir -p "$OUT"

# The spice's frame-callback shim, compiled the way the module's own objects
# are (pthread, wasm EH, -O2).
emcc -c "$TUR_SPICES/spices/raylib/c/raylib/rl_web_loop.c" -o "$OUT/rl_web_loop.o" \
  -pthread -fwasm-exceptions -O2

export TUR_WASM_JIT_EXTRA_OBJECTS="$RAYLIB_LIB $OUT/rl_web_loop.o"
export TUR_WASM_JIT_EXTRA_HEADERS="$RAYLIB_SRC/src/raylib.h"
# The GL flags pkg_web_compose_link_flags (src/compiler/pkg.c) gives a
# :web :canvas :gl :es3 project, and the two spices' sources.
export TUR_WASM_JIT_EXTRA_LINK="-sUSE_GLFW=3 -sGL_ENABLE_GET_PROC_ADDRESS -sMAX_WEBGL_VERSION=2 \
  --embed-file $TUR_SPICES/spices/raylib/src@/spices/raylib/src \
  --embed-file $TUR_SPICES/spices/math/src@/spices/math/src"
bash "$ROOT/tools/wasm-jit-spike/jspi/build.sh" "$OUT"
echo "build.sh: arcade module at $OUT/wasm-jit (raylib $(cd "$RAYLIB_SRC" && git describe --tags --always 2>/dev/null || echo ?))"
