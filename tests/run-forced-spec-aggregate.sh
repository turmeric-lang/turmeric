#!/usr/bin/env bash
# tests/run-forced-spec-aggregate.sh -- pin forced-spec-loses-aggregate-type
# and forced-spec-at-foreign-aggregate.
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

# --- 3) forced-spec-at-foreign-aggregate: a THIRD module's struct. ----------
# `types` exports Pos; `util` and `app` both import it.  `util` emits `ident`
# at Pos for `app`, and Pos is not among util's own items: the owner resolves
# it through the defs its elaboration saw (it includes types.h already).
F3="$REPO/tests/fixtures/forced-spec-foreign-aggregate-third-module"
mkdir -p "$WORK/third"
cp "$F3/types.tur" "$F3/util.tur" "$F3/app.tur" "$WORK/third/"
build_out=$("$TUR" build "$WORK/third" --build-dir "$WORK/bd3" -o "$WORK/app3" 2>&1)
build_rc=$?
if [ $build_rc -ne 0 ]; then
    fail "forced-spec-third-module-build" "tur build exit=$build_rc: $build_out"
else
    run_out=$("$WORK/app3" 2>&1)
    if [ "$run_out" = "$(cat "$F3/expected.stdout")" ]; then
        pass "forced-spec-third-module"
    else
        fail "forced-spec-third-module" "got: $run_out"
    fi
fi

# --- 4) ... and the CALLER's own struct: `app` defines Q and calls util's
# generic at it.  util cannot name Q (it does not include app.h), so no unit
# can emit the clone; the build stops with a diagnostic naming Q instead of a
# C compiler error at app's call.
F4="$REPO/tests/fixtures/forced-spec-foreign-aggregate-callers-own"
mkdir -p "$WORK/own"
cp "$F4/util.tur" "$F4/app.tur" "$WORK/own/"
build_out=$("$TUR" build "$WORK/own" --build-dir "$WORK/bd4" -o "$WORK/app4" 2>&1)
build_rc=$?
if [ $build_rc -ne 0 ] &&
   printf '%s' "$build_out" | grep -q '`ident` is called at `Q` from another module' &&
   ! printf '%s' "$build_out" | grep -q 'incompatible type for argument'; then
    pass "forced-spec-callers-own-struct-refused"
else
    fail "forced-spec-callers-own-struct-refused" "exit=$build_rc: $build_out"
fi

echo "forced-spec-aggregate: $PASS passed, $FAIL failed"
[ $FAIL -eq 0 ]
