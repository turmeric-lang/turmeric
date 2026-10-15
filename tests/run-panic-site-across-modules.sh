#!/usr/bin/env bash
# tests/run-panic-site-across-modules.sh -- a panic raised in one module's
# inline-C body, called from another module, names the CALLER's line when the
# modules are compiled separately.
#
# panic-location-names-the-runtime-not-the-call-site: a call to a function
# whose inline-C body may panic sets the thread's panic site around itself,
# and the body's `tur_panic` reads it.  The slot was `static` in every
# translation unit, so in a multi-file build (`tur build <dir>` without a
# manifest, `tur build --shared`) the caller's module set its own copy and the
# callee's module read another, empty one: the panic named a line of
# tur_runtime.h.  The caller did not site the call at all either -- the
# callee's signature is in the other module's header, not in this TU's
# signature table.  The runtime header now gives the owner TU the one slot
# (`extern` everywhere else), and the call is sited with an `__auto_type` temp.
#
# A manifest project with one `main` is built as ONE translation unit, which
# hides this -- so the fixture is a bare directory, which takes the
# separate-compilation path.

set -uo pipefail
cd "$(dirname "$0")/.."
REPO="$PWD"

TUR="$REPO/build/tur"
FIXTURE="$REPO/tests/fixtures/panic-site-across-modules"
WORK="$(mktemp -d -t tur-panicsite.XXXXXX)"
trap 'rm -rf "$WORK"' EXIT

# Leaks in the Debug (ASan) build of tur are out of scope here; this exercises
# the codegen path, not compiler-internal leaks.
export ASAN_OPTIONS="${ASAN_OPTIONS:-detect_leaks=0}"

PASS=0
FAIL=0
pass() { PASS=$((PASS + 1)); echo "PASS $1"; }
fail() { FAIL=$((FAIL + 1)); echo "FAIL $1 -- $2"; }

if [ ! -x "$TUR" ]; then
    echo "tests: $TUR not built; run 'tur run build' first" >&2
    exit 2
fi

mkdir -p "$WORK/proj"
cp "$FIXTURE"/*.tur "$WORK/proj/"
BIN="$WORK/prog"

build_out=$(cd "$WORK/proj" && "$TUR" build . --build-dir "$WORK/bd" -o "$BIN" 2>&1)
build_rc=$?
if [ $build_rc -ne 0 ] || [ ! -x "$BIN" ]; then
    fail "panic-site-across-modules-build" "tur build exit=$build_rc: $build_out"
else
    pass "panic-site-across-modules-build"
    # The structural half: two module TUs, one slot.
    if [ -f "$WORK/bd/obj/main.c" ] && [ -f "$WORK/bd/obj/checks.c" ] &&
       grep -q "extern TUR_THREAD_LOCAL const tur_site_t \*tur_cur_site;" \
            "$WORK/bd/obj/tur_runtime.h"; then
        pass "panic-site-across-modules-one-slot"
    else
        fail "panic-site-across-modules-one-slot" \
             "expected separate main.c/checks.c and an owner-gated slot in tur_runtime.h"
    fi
    out=$("$BIN" 2>"$WORK/stderr")
    rc=$?
    if [ "$rc" -eq 0 ]; then
        fail "panic-site-across-modules-panics" "exited 0; stdout: $out"
    elif [ "$out" != "2" ]; then
        fail "panic-site-across-modules-panics" "stdout '$out', expected '2'"
    elif ! grep -qF "panic at main.tur:9: checked: past end" "$WORK/stderr"; then
        fail "panic-site-across-modules-names-caller" \
             "stderr: $(head -3 "$WORK/stderr")"
    else
        pass "panic-site-across-modules-names-caller"
    fi
fi

echo
echo "summary: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
