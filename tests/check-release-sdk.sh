#!/usr/bin/env bash
# tests/check-release-sdk.sh -- a release archive can build a libturi embedder.
#
# Stages the archive tree a release publishes, with the script release.yml
# packages through (scripts/stage-release.sh), then -- from outside this
# checkout, with the staged bin/tur and nothing else -- builds and runs the
# two programs in tests/release-sdk/:
#
#   turi-eval.tur    `(import turi/eval)`: "turi/eval.h" and its closure, -lturi
#   scheme-eval.tur  `(import (scheme eval))`: "turi/r7rs_embed.h", -lturi
#
# Every release from v0.47.0 shipped an include/turi/ that could not be
# compiled and a `tur` that never pointed at it, and nothing saw it until a
# downstream suite ran against a tarball
# (docs/archive/release-tarball-turi-headers-are-unusable.md).  release.yml now
# checks the same thing on the real archive; this runs it on every PR, so a
# header that gains an include or a probe that moves fails here first.
#
# Asserts, per program: it builds, its compile line takes -I/-L from the staged
# prefix and nothing from this checkout's src/, and it prints 42.
#
# Usage: bash tests/check-release-sdk.sh [build-dir]   (default: build)
# Environment: CC  the C compiler stage-release.sh probes with (default: cc)

set -u
cd "$(dirname "$0")/.."
REPO="$(pwd)"
BUILD="${1:-build}"
case $BUILD in /*) ;; *) BUILD="$REPO/$BUILD" ;; esac

if [ ! -x "$BUILD/tur" ] && [ ! -x "$BUILD/tur.exe" ]; then
    echo "check-release-sdk: no tur in $BUILD" >&2
    exit 2
fi
# The archive always carries the JIT engine's MIR (release builds keep TUR_JIT
# on).  A local build configured without it cannot stage that archive.
if [ ! -f "$BUILD/libtur_mir.a" ]; then
    echo "SKIP check-release-sdk: $BUILD has no libtur_mir.a (TUR_JIT=OFF); a release always ships one"
    exit 0
fi

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
FAILED=0
fail() { echo "FAIL check-release-sdk: $*"; FAILED=1; }

if ! bash scripts/stage-release.sh "$BUILD" "$TMP/dist" > "$TMP/stage.log" 2>&1; then
    cat "$TMP/stage.log"
    echo "FAIL check-release-sdk: scripts/stage-release.sh could not stage the archive"
    exit 1
fi
TUR="$TMP/dist/bin/tur"
[ -x "$TUR" ] || TUR="$TMP/dist/bin/tur.exe"

mkdir "$TMP/work"
cp tests/release-sdk/turi-eval.tur tests/release-sdk/scheme-eval.tur "$TMP/work/"

# Nothing from this checkout's environment may help: the staged tur has to
# find its stdlib, runtime archives, headers and libturi on its own.  The
# Debug libturi.a is ASan-instrumented, and the interpreter keeps its
# closures for the life of the process, so leak detection stays off.
clean_env=(env -u TUR_STDLIB_DIR -u TUR_CC_FLAGS -u TUR_SDK_ROOT
               -u TUR_RUNTIME_LIB -u TUR_BUILD_DIR
               ASAN_OPTIONS=detect_leaks=0 TUR_SHOW_CC=1)

for prog in turi-eval scheme-eval; do
    if ! (cd "$TMP/work" && "${clean_env[@]}" "$TUR" build "$prog.tur" -o "$prog.bin") \
            > "$TMP/$prog.log" 2>&1; then
        fail "$prog.tur did not build against the staged archive:"
        grep -v '^CC: ' "$TMP/$prog.log" | tail -8 | sed 's/^/    /'
        continue
    fi
    cc_line=$(grep '^CC: ' "$TMP/$prog.log" | tail -1)
    if [[ "$cc_line" != *"-I$TMP/dist/include"* ]]; then
        fail "$prog.tur: the compile line has no -I$TMP/dist/include"
    fi
    if [[ "$cc_line" != *"-L$TMP/dist/lib"* ]]; then
        fail "$prog.tur: the compile line has no -L$TMP/dist/lib"
    fi
    if [[ "$cc_line" == *"$REPO/src"* ]]; then
        fail "$prog.tur: the compile line reaches into this checkout ($REPO/src), so it proves nothing about the archive"
    fi
    out=$(cd / && env -u TUR_STDLIB_DIR ASAN_OPTIONS=detect_leaks=0 "$TMP/work/$prog.bin" 2>"$TMP/$prog.err")
    if [ "$out" != "42" ]; then
        fail "$prog.bin printed '$out', want 42 ($(tail -3 "$TMP/$prog.err"))"
    fi
done

if [ "$FAILED" -eq 0 ]; then
    echo "check-release-sdk: ok ($(tail -1 "$TMP/stage.log" | sed 's/^stage-release: //'))"
fi
exit "$FAILED"
