#!/usr/bin/env bash
# tests/check-serve-release.sh -- `tur serve` works from a release archive.
#
# project-less-tools-plan PT1.  `tur serve` runs a program shipped in the
# stdlib (stdlib/tools/serve.tur), so it depends on the release layout in a
# way no other command does: the program must be in the archive, the staged
# `tur` must find it, and each engine must build it there.  Stages the archive
# with scripts/stage-release.sh (the script release.yml packages through), then
# from an unrelated directory runs the staged `tur serve --port 0` on the JIT
# (the default) and on the cc path, fetches a file and an index, and stops it.
#
# Usage: bash tests/check-serve-release.sh [build-dir]   (default: build)

set -u
cd "$(dirname "$0")/.."
REPO="$(pwd -P)"
BUILD="${1:-build}"
case $BUILD in /*) ;; *) BUILD="$REPO/$BUILD" ;; esac

if [ ! -x "$BUILD/tur" ] && [ ! -x "$BUILD/tur.exe" ]; then
    echo "check-serve-release: no tur in $BUILD" >&2
    exit 2
fi
if [ ! -f "$BUILD/libtur_mir.a" ]; then
    echo "SKIP check-serve-release: $BUILD has no libtur_mir.a (TUR_JIT=OFF); a release always ships one"
    exit 0
fi
if ! command -v curl >/dev/null 2>&1; then
    echo "SKIP check-serve-release: no curl to fetch with"
    exit 0
fi

TMP="$(cd "$(mktemp -d)" && pwd -P)"
pid=""
cleanup() {
    [ -n "$pid" ] && kill "$pid" 2>/dev/null && wait "$pid" 2>/dev/null
    rm -rf "$TMP"
}
trap cleanup EXIT
FAILED=0
fail() { echo "FAIL check-serve-release: $*"; FAILED=1; }

if ! bash scripts/stage-release.sh "$BUILD" "$TMP/dist" > "$TMP/stage.log" 2>&1; then
    cat "$TMP/stage.log"
    echo "FAIL check-serve-release: scripts/stage-release.sh could not stage the archive"
    exit 1
fi
TUR="$TMP/dist/bin/tur"
[ -x "$TUR" ] || TUR="$TMP/dist/bin/tur.exe"

mkdir -p "$TMP/work/site/docs"
printf 'hello from tur serve' > "$TMP/work/site/hello.txt"
printf '<p>docs</p>'          > "$TMP/work/site/docs/index.html"

for engine in jit cc; do
    (cd "$TMP/work" && TUR_ENGINE=$engine exec "$TUR" serve site --port 0 -q) \
        > "$TMP/out" 2> "$TMP/err" &
    pid=$!
    for _ in $(seq 1200); do
        grep -q '^Serving ' "$TMP/out" 2>/dev/null && break
        kill -0 "$pid" 2>/dev/null || break
        sleep 0.1
    done
    port=$(sed -nE 's|^Serving .* on http://127\.0\.0\.1:([0-9]+)/.*|\1|p' "$TMP/out")
    if [ -z "$port" ]; then
        fail "$engine: the staged tur serve did not start"
        sed 's/^/    /' "$TMP/err"
    else
        body=$(curl --noproxy '*' -s "http://127.0.0.1:$port/hello.txt")
        [ "$body" = "hello from tur serve" ] || fail "$engine: /hello.txt answered '$body'"
        body=$(curl --noproxy '*' -s "http://127.0.0.1:$port/docs/")
        [ "$body" = "<p>docs</p>" ] || fail "$engine: /docs/ answered '$body'"
        [ $FAILED -eq 0 ] && echo "ok check-serve-release: $engine engine serves from the staged archive"
    fi
    kill "$pid" 2>/dev/null
    wait "$pid" 2>/dev/null
    pid=""
done

exit $FAILED
