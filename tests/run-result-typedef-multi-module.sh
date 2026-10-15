#!/usr/bin/env bash
# tests/run-result-typedef-multi-module.sh -- pin
# `result-typedef-duplicated-across-modules`: a multi-module project where
# two modules export functions returning the same `(Result cstr cstr)`
# instantiation, and a third module imports both.  Each module's generated
# `.h` emits a `typedef struct Result__cstr__cstr { ... }`; without a
# per-instantiation include guard the third TU fails to compile with
# `redefinition of 'Result__cstr__cstr'`.
#
# The fix wraps every monomorphic-instantiation typedef in
# `#ifndef TUR_TY_<Name> / #define / #endif` (src/compiler/types.c
# emit_registered_struct_app_rec + emit_registered_adt_app_rec).
#
# IMPORTANT: the collision only manifests under *separate compilation* --
# one `.h` per module, several of them landing in a single translation unit
# (here `app__main.c` `#include`s both `app__produce.h` and
# `app__consume.h`).  That is the exact shape of the original repro,
# `tur build spices/tourist`, which is a shared library (no `main`).  A plain
# `tur build <dir>` on a single-`main` project instead reroutes to a
# whole-program single-file build that inlines every module into one TU and
# de-dups the typedef within that TU -- so it never stresses the cross-module
# collision.  We therefore drive the library (separate-compilation) build to
# pin the bug, and additionally smoke-test the whole-program executable build
# so the Result values are shown to round-trip at runtime.

set -uo pipefail
cd "$(dirname "$0")/.."
REPO="$PWD"

TUR="$REPO/build/tur"
FIXTURE="$REPO/tests/fixtures/result-typedef-multi-module"
WORK="$(mktemp -d -t tur-rtmm.XXXXXX)"
trap 'rm -rf "$WORK"' EXIT

# Leaks in the Debug (ASan) build of tur are out of scope here; we are
# exercising the codegen path, not chasing compiler-internal leaks.
export ASAN_OPTIONS="${ASAN_OPTIONS:-detect_leaks=0}"

PASS=0
FAIL=0
pass() { PASS=$((PASS + 1)); echo "PASS $1"; }
fail() { FAIL=$((FAIL + 1)); echo "FAIL $1 -- $2"; }

if [ ! -x "$TUR" ]; then
    echo "tests: $TUR not built; run 'tur run build' first" >&2
    exit 2
fi

cp -R "$FIXTURE" "$WORK/proj"
PROJ="$WORK/proj"
LIB_BD="$WORK/lib-bd"
SO="$WORK/libapp.so"
BIN="$WORK/app"

# --- 1) Separate-compilation (shared library) build: the genuine repro. -----
# Each module gets its own `.h`; `app__main`'s TU pulls in both sibling
# headers, so without the guard cc fails with `redefinition of
# 'Result__cstr__cstr'`.
build_out=$(cd "$WORK" && "$TUR" build --shared "$PROJ" \
    --build-dir "$LIB_BD" -o "$SO" 2>&1)
build_rc=$?
if [ $build_rc -ne 0 ]; then
    # The specific failure we are pinning against: a `redefinition` error on
    # the Result__cstr__cstr typedef.
    if grep -q "redefinition of 'Result__cstr__cstr'" <<< "$build_out"; then
        fail "result-typedef-no-redefinition" "redefinition of Result__cstr__cstr re-emerged: $build_out"
    else
        fail "result-typedef-no-redefinition" "tur build --shared exit=$build_rc: $build_out"
    fi
else
    pass "result-typedef-no-redefinition"
fi

# The shared library must have been produced.
if [ -f "$SO" ]; then
    pass "result-typedef-shared-lib-exists"
else
    fail "result-typedef-shared-lib-exists" "shared library not produced at $SO"
fi

# Sanity: more than one module's emitted header must contain the include-guard
# for Result__cstr__cstr.  This proves the fix is in place (rather than the
# build accidentally passing because only one module emitted the typedef).
guarded_count=0
for h in "$LIB_BD/obj"/*.h; do
    [ -f "$h" ] || continue
    if grep -q "TUR_TY_tur_adt_Result__cstr__cstr" "$h"; then
        guarded_count=$((guarded_count + 1))
    fi
done
if [ "$guarded_count" -ge 2 ]; then
    pass "result-typedef-guard-emitted-per-module"
else
    fail "result-typedef-guard-emitted-per-module" \
        "expected TUR_TY_Result__cstr__cstr guard in >=2 module headers, got $guarded_count"
fi

# --- 2) Whole-program executable build: runtime smoke. ----------------------
# Confirms the single-`main` whole-program path still links and the Result
# values round-trip (the program returns 42).
exe_out=$(cd "$WORK" && "$TUR" build "$PROJ" -o "$BIN" 2>&1)
exe_rc=$?
if [ $exe_rc -ne 0 ]; then
    fail "result-typedef-binary-exists" "tur build exit=$exe_rc: $exe_out"
elif [ -x "$BIN" ]; then
    pass "result-typedef-binary-exists"
    "$BIN"
    run_rc=$?
    if [ "$run_rc" -eq 42 ]; then
        pass "result-typedef-binary-runs"
    else
        fail "result-typedef-binary-runs" "binary returned $run_rc, expected 42"
    fi
else
    fail "result-typedef-binary-exists" "binary not produced at $BIN"
fi

echo
echo "summary: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
