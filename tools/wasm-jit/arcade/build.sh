#!/usr/bin/env bash
# build.sh -- the arcade module (raylib-web-playground-plan P1 / J0): the
# JSPI build of the browser JIT module with raylib's web archive, the raylib
# spice's one native TU (rl_web_loop.c) and raylib.h linked and embedded,
# and beside it the spice packages (J2): OUT_DIR/wasm-jit/spices/, one
# <name>.json per spice and an index.json (pack-spices.py), which the page
# mounts after the module has started (spices.js) -- the spices' Turmeric
# sources are data, so adding one is a repack, not a relink.
# Run by the tur_wasm_arcade CMake target; it can be run by hand.
#
#   RAYLIB_SRC=<raylib 5.5 checkout> TUR_SPICES=<turmeric-spices checkout> \
#   bash tools/wasm-jit/arcade/build.sh OUT_DIR
#
#   RAYLIB_LIB=<web libraylib.a>   use this archive instead of building one
#                                  into OUT_DIR/raylib (build-raylib.sh)
#   TUR_ARCADE_SPICE_LIST="a b"    the spices to package (default: raylib
#                                  math); the list must be closed under each
#                                  spice's :spices deps
#   TUR_WASM_BUILD_DIR=<dir>       the configured -DTUR_WASM=ON build whose
#                                  wasm-jit/sources.txt and stdlib-pkg/ the
#                                  module is made from (default build-wasm/)
#
# The result is OUT_DIR/wasm-jit/turmeric-jit.{js,wasm}, stdlib.data, spices/
# and spices.js (the loader, copied so a page takes it with the module).  It
# is the JSPI module (TUR_WASM_JIT_JSPI=1), never built over
# build-wasm/wasm-jit, whose module web/public serves to Try Turmeric's
# Compile engine: under JSPI turi_wasm_jit_run returns a Promise, which that
# page does not await, and a browser without JSPI cannot load it at all.
# OUT_DIR is a full object build the first time and a relink after.
set -euo pipefail
OUT=$1
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../../.." && pwd)
BUILD=${TUR_WASM_BUILD_DIR:-$ROOT/build-wasm}
: "${RAYLIB_SRC:?RAYLIB_SRC: a raylib 5.5 checkout}"
: "${TUR_SPICES:?TUR_SPICES: a turmeric-spices checkout}"
[ -f "$BUILD/wasm-jit/sources.txt" ] || { echo "build.sh: no $BUILD/wasm-jit/sources.txt (configure it with -DTUR_WASM=ON)" >&2; exit 2; }
[ -f "$TUR_SPICES/spices/raylib/c/raylib/rl_web_loop.c" ] || { echo "build.sh: no raylib spice under $TUR_SPICES" >&2; exit 2; }
command -v emcc >/dev/null || { echo "build.sh: emcc is not on PATH" >&2; exit 2; }
mkdir -p "$OUT"
OUT=$(cd "$OUT" && pwd)

if [ -z "${RAYLIB_LIB:-}" ]; then
  bash "$HERE/build-raylib.sh" "$RAYLIB_SRC" "$OUT/raylib"
  RAYLIB_LIB=$OUT/raylib/libraylib.a
fi
[ -f "$RAYLIB_LIB" ] || { echo "build.sh: no $RAYLIB_LIB" >&2; exit 2; }

# The spice's frame-callback shim, compiled the way the module's own objects
# are (pthread, wasm EH, -O2).
emcc -c "$TUR_SPICES/spices/raylib/c/raylib/rl_web_loop.c" -o "$OUT/rl_web_loop.o" \
  -pthread -fwasm-exceptions -O2

export TUR_WASM_JIT_JSPI=1
export TUR_WASM_JIT_EXTRA_OBJECTS="$RAYLIB_LIB $OUT/rl_web_loop.o"
export TUR_WASM_JIT_EXTRA_HEADERS="$RAYLIB_SRC/src/raylib.h"
# The GL flags pkg_web_compose_link_flags (src/compiler/pkg.c) gives a
# :web :canvas :gl :es3 project.
export TUR_WASM_JIT_EXTRA_LINK="-sUSE_GLFW=3 -sGL_ENABLE_GET_PROC_ADDRESS -sMAX_WEBGL_VERSION=2"
bash "$ROOT/tools/wasm-jit/build-module.sh" "$OUT/wasm-jit" "$BUILD/wasm-jit/sources.txt" \
  "$(tr -d '\n' < "$ROOT/VERSION")" "${TUR_MIR_SOURCE_DIR:-$ROOT/external/mir}" "$BUILD/stdlib-pkg"
# shellcheck disable=SC2086
rm -rf "$OUT/wasm-jit/spices"
python3 "$HERE/pack-spices.py" "$TUR_SPICES" "$OUT/wasm-jit/spices" ${TUR_ARCADE_SPICE_LIST:-raylib math}
cp "$HERE/spices.js" "$OUT/wasm-jit/spices.js"
grep -q promising "$OUT/wasm-jit/turmeric-jit.js" || { echo "build.sh: the module is not a JSPI build (no promising export)" >&2; exit 1; }
echo "build.sh: arcade module at $OUT/wasm-jit (raylib $(git -C "$RAYLIB_SRC" describe --tags --always 2>/dev/null || echo '?'))"
