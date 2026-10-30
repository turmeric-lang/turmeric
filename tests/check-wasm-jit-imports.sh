#!/usr/bin/env bash
# tests/check-wasm-jit-imports.sh -- the browser JIT module resolves an import
# through a directory added with turi_wasm_add_include_dir
# (raylib-web-playground-plan prerequisite 9: the page mounts a spice's src/
# in the module's file system and names it; `(import raylib/core)` must then
# resolve on the Compile engine, and on the interpreter it falls back to).
#
# Runs tests/wasm-jit-imports/main.tur, which imports arcade/greet from
# tests/wasm-jit-imports/spice/src, through tools/wasm-jit/run.js -I (the
# host tree copied into the module and added to its search path) and checks
# that the engine compiled it and printed what the native `tur run -I` prints.
#
#   bash tests/check-wasm-jit-imports.sh
#
# Environment: TUR (a native tur, default build/tur), TUR_WASM_JIT_MODULE
# (default build-wasm/wasm-jit).  Exits 1 on a difference, 2 when a piece
# is missing.
set -u
cd "$(dirname "$0")/.."
TUR="${TUR:-build/tur}"
MODULE_DIR="${TUR_WASM_JIT_MODULE:-build-wasm/wasm-jit}"
[ -x "$TUR" ] || { echo "check-wasm-jit-imports: no tur at $TUR" >&2; exit 2; }
[ -f "$MODULE_DIR/turmeric-jit.wasm" ] || { echo "check-wasm-jit-imports: no $MODULE_DIR/turmeric-jit.wasm" >&2; exit 2; }
export TUR_NO_GLOBAL_SPICES=1
export ASAN_OPTIONS="${ASAN_OPTIONS:-detect_leaks=0}"

HERE=tests/wasm-jit-imports
WORK="$(mktemp -d "${TMPDIR:-/tmp}/tur-jit-imports.XXXXXX")"
trap 'rm -rf "$WORK"' EXIT

"$TUR" run -I "$HERE/spice/src" "$HERE/main.tur" > "$WORK/want" 2> "$WORK/want.err"
want_rc=$?
if [ "$want_rc" != 0 ]; then
    echo "check-wasm-jit-imports: the native run failed (exit $want_rc):" >&2
    sed 's/^/    /' "$WORK/want.err" >&2
    exit 2
fi
grep -q "hello from a mounted spice, arcade" "$WORK/want" || {
    echo "check-wasm-jit-imports: the native run did not print the greeting:" >&2
    sed 's/^/    /' "$WORK/want" >&2
    exit 2
}

# Without -I the import must fail: otherwise the check proves nothing.
node tools/wasm-jit/run.js --module "$MODULE_DIR" --result "$WORK/none.json" "$HERE/main.tur" \
    > "$WORK/none.out" 2> "$WORK/none.err"
if grep -q "hello from a mounted spice" "$WORK/none.out"; then
    echo "check-wasm-jit-imports: the import resolved WITHOUT -I; the check is not testing the include dir" >&2
    exit 1
fi

node tools/wasm-jit/run.js --module "$MODULE_DIR" --result "$WORK/got.json" -I "$HERE/spice/src" "$HERE/main.tur" \
    > "$WORK/got" 2> "$WORK/got.err"
got_rc=$?
engine=$(grep -o '"engine":"[a-z]*"' "$WORK/got.json" | cut -d'"' -f4)
reason=$(grep -o '"reason":"[^"]*"' "$WORK/got.json" | cut -d'"' -f4)
if ! cmp -s "$WORK/want" "$WORK/got" || [ "$got_rc" != "$want_rc" ]; then
    echo "check-wasm-jit-imports: WRONG output on the module (engine $engine, exit $got_rc${reason:+, $reason}):" >&2
    diff "$WORK/want" "$WORK/got" | sed 's/^/    /' >&2
    sed 's/^/    stderr: /' "$WORK/got.err" >&2
    exit 1
fi
if [ "$engine" != compiled ]; then
    echo "check-wasm-jit-imports: right output, but the engine did not compile it ($engine${reason:+: $reason})" >&2
    exit 1
fi
echo "PASS check-wasm-jit-imports (compiled; $(wc -l < "$WORK/got" | tr -d ' ') lines match the native run)"
