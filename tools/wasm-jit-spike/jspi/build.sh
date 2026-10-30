#!/usr/bin/env bash
# build.sh -- the JSPI build of the browser JIT module, in a directory of its
# own: tools/wasm-jit/build-module.sh with TUR_WASM_JIT_JSPI=1, fed the
# configured build-wasm/'s source list and stdlib package.  The raylib
# playground's module (raylib-web-playground-plan J4; prerequisite 3 measured
# it).  Never built over build-wasm/wasm-jit, whose module web/public serves
# to Try Turmeric's Compile engine: under JSPI turi_wasm_jit_run returns a
# Promise, which that page does not await, and a browser without JSPI cannot
# load it at all.
#
#   bash tools/wasm-jit-spike/jspi/build.sh OUT_DIR
#
# Needs a configured build-wasm/ (its wasm-jit/sources.txt and stdlib-pkg/;
# TUR_WASM_BUILD_DIR overrides) and the emsdk environment.  OUT_DIR is a full
# object build the first time and a relink after.  Drive the result from a
# browser (index.html, harness.js, worker.js, jspi.spec.js beside this file);
# node 22's V8 has no JSPI.
set -euo pipefail
OUT=$1
ROOT=$(cd "$(dirname "$0")/../../.." && pwd)
BUILD=${TUR_WASM_BUILD_DIR:-$ROOT/build-wasm}
[ -f "$BUILD/wasm-jit/sources.txt" ] || { echo "build.sh: no $BUILD/wasm-jit/sources.txt (configure build-wasm with -DTUR_WASM=ON and build tur_wasm_jit once)" >&2; exit 2; }
VERSION=$(tr -d '\n' < "$ROOT/VERSION")
TUR_WASM_JIT_JSPI=1 bash "$ROOT/tools/wasm-jit/build-module.sh" "$OUT/wasm-jit" "$BUILD/wasm-jit/sources.txt" "$VERSION" \
    "${TUR_MIR_SOURCE_DIR:-$ROOT/external/mir}" "$BUILD/stdlib-pkg"
echo "build.sh: $OUT/wasm-jit/turmeric-jit.{js,wasm}"
