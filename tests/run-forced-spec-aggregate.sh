#!/usr/bin/env bash
# tests/run-forced-spec-aggregate.sh -- pin forced-spec-loses-aggregate-type.
#
# `app` calls `util`'s generics at `util`'s by-value structs.  Under separate
# compilation `util` emits those specializations for `app` (a forced ABI
# spec), and the record of one used to keep only each position's TypeKind:
# `util` rebuilt a struct as the int64_t carrier, `app` declared the same
# symbol with the struct, and app.c failed to compile.
#
# A bare directory (no build.tur) is built module by module, which is the
# path that forces specs; a single-main project inlines every module into
# one TU and never exercises it.  `emit-c --output-dir` is the other driver.

set -uo pipefail
cd "$(dirname "$0")/.."
REPO="$PWD"

TUR="$REPO/build/tur"
FIXTURE="$REPO/tests/fixtures/forced-spec-aggregate-multi-module"
WORK="$(mktemp -d -t tur-fsag.XXXXXX)"
trap 'rm -rf "$WORK"' EXIT

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
cp "$FIXTURE/util.tur" "$FIXTURE/app.tur" "$WORK/proj/"

# --- 1) Separate-compilation build and run. --------------------------------
build_out=$("$TUR" build "$WORK/proj" --build-dir "$WORK/bd" -o "$WORK/app" 2>&1)
build_rc=$?
if [ $build_rc -ne 0 ]; then
    fail "forced-spec-aggregate-build" "tur build exit=$build_rc: $build_out"
else
    pass "forced-spec-aggregate-build"
    run_out=$("$WORK/app" 2>&1)
    if [ "$run_out" = "$(cat "$FIXTURE/expected.stdout")" ]; then
        pass "forced-spec-aggregate-run"
    else
        fail "forced-spec-aggregate-run" "got: $run_out"
    fi
fi

# --- 2) emit-c --output-dir: util.h declares the specs with the structs. ----
if "$TUR" emit-c --output-dir "$WORK/out" "$WORK/proj/util.tur" \
        "$WORK/proj/app.tur" >/dev/null 2>&1 &&
   grep -q '^tur_adt_Pos util__ident__spec__tur_adt_Pos_tur_adt_Pos(tur_adt_Pos);' "$WORK/out/util.h" &&
   grep -q '^tur_adt_Box util__pick__spec__tur_adt_Box_tur_adt_Pos_tur_adt_Box(tur_adt_Pos, tur_adt_Box);' "$WORK/out/util.h"; then
    pass "forced-spec-aggregate-emit-c-header"
else
    fail "forced-spec-aggregate-emit-c-header" \
         "$(grep -n 'spec__tur_adt' "$WORK/out/util.h" 2>&1 | head -5)"
fi

echo "forced-spec-aggregate: $PASS passed, $FAIL failed"
[ $FAIL -eq 0 ]
