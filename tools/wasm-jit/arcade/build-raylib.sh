#!/usr/bin/env bash
# build-raylib.sh -- raylib's web archive, as the arcade module links it
# (raylib-web-playground-plan J0).
#
#   build-raylib.sh RAYLIB_SRC OUT_DIR      -> OUT_DIR/libraylib.a
#
# RAYLIB_SRC is a raylib checkout (the raylib spice pins 5.5).  The options
# are the spice's :wasm-options -- PLATFORM=Web, GRAPHICS_API_OPENGL_ES3,
# WebGL2 -- and the C flags are the module's: the module is a pthread,
# wasm-exceptions link (tools/wasm-jit/build-module.sh), and an archive
# compiled without either does not link into it.  Needs the emsdk
# environment (emcmake) and cmake.  A rerun with the same flags rebuilds
# nothing.
set -euo pipefail
SRC=$1
OUT=$2
[ -f "$SRC/src/raylib.h" ] || { echo "build-raylib.sh: no $SRC/src/raylib.h" >&2; exit 2; }
command -v emcmake >/dev/null || { echo "build-raylib.sh: emcmake is not on PATH (source the emsdk environment)" >&2; exit 2; }
mkdir -p "$OUT"
OUT=$(cd "$OUT" && pwd)
SRC=$(cd "$SRC" && pwd)
# A build directory configured from another checkout is CMake's hard error
# ("does not match the source ... used to generate cache"): start it over.
if [ -f "$OUT/build/CMakeCache.txt" ] &&
   ! grep -qxF "CMAKE_HOME_DIRECTORY:INTERNAL=$SRC" "$OUT/build/CMakeCache.txt"; then
  rm -rf "$OUT/build"
fi
emcmake cmake -S "$SRC" -B "$OUT/build" -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
  -DCMAKE_BUILD_TYPE=Release -DPLATFORM=Web -DGRAPHICS=GRAPHICS_API_OPENGL_ES3 \
  -DBUILD_SHARED_LIBS=OFF -DBUILD_EXAMPLES=OFF -DBUILD_GAMES=OFF \
  -DCMAKE_C_FLAGS="-pthread -fwasm-exceptions" > "$OUT/configure.log"
cmake --build "$OUT/build" -j"$(nproc 2>/dev/null || echo 4)" --target raylib > "$OUT/build.log"
cp "$OUT/build/raylib/libraylib.a" "$OUT/libraylib.a"
cp "$SRC/src/raylib.h" "$OUT/raylib.h"
echo "build-raylib.sh: $OUT/libraylib.a (raylib $(git -C "$SRC" describe --tags --always 2>/dev/null || echo '?'))"
