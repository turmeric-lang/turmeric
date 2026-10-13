#!/usr/bin/env bash
# tests/run-shared-imported-ctor.sh -- pin `shared-build-imported-ctor-undeclared`.
#
# A constructor is emitted `static` in its defining module's .c, and a module
# header carries only the ADT's layout.  Under separate compilation a module
# that BUILDS a value of an imported ADT called `ctor_Shape_Rect` with nothing
# declaring it: an implicit `int`, then "invalid initializer".  The importer
# now emits the constructors it calls into its own TU.
#
# The same harness shape as run-any-type-id-multi-module.sh, for the same
# reason: `tur build <dir>` on a single-`main` project inlines every module
# into ONE TU, where every ADT is local and the bug cannot show.  Only
# `tur build --shared` splits the program, so this builds the library, links
# its per-module C into an executable and runs it, then checks the
# whole-program build prints the same thing.

set -uo pipefail
cd "$(dirname "$0")/.."
REPO="$PWD"

TUR="$REPO/build/tur"
FIXTURE="$REPO/tests/fixtures/shared-imported-ctor"
WORK="$(mktemp -d -t tur-impctor.XXXXXX)"
trap 'rm -rf "$WORK"' EXIT

# Compiler-internal leaks in the Debug (ASan) tur are out of scope here.
export ASAN_OPTIONS="${ASAN_OPTIONS:-detect_leaks=0}"

PASS=0
FAIL=0
pass() { PASS=$((PASS + 1)); echo "PASS $1"; }
fail() { FAIL=$((FAIL + 1)); echo "FAIL $1 -- $2"; }

if [ ! -x "$TUR" ]; then
    echo "tests: $TUR not built; run 'just build' first" >&2
    exit 2
fi

cp -R "$FIXTURE" "$WORK/proj"
PROJ="$WORK/proj"
LIB_BD="$WORK/lib-bd"
SO="$WORK/libshapes.so"
BIN="$WORK/app"
MULTI="$WORK/multi"

EXPECTED=$'8\n15\n0\nrect\n2\n6\n3\n41\n36\n21'

# --- 1) Separate compilation: the repro. -----------------------------------
build_out=$(cd "$WORK" && "$TUR" build --shared "$PROJ" \
    --build-dir "$LIB_BD" -o "$SO" 2>&1)
build_rc=$?
if [ $build_rc -ne 0 ]; then
    fail "imported-ctor-shared-build" "tur build --shared exit=$build_rc: $build_out"
else
    pass "imported-ctor-shared-build"
fi

MAIN_C="$LIB_BD/obj/app__main.c"

# --- 2) The structural pin: main.c defines the constructor it calls. -------
if [ -f "$MAIN_C" ]; then
    if grep -qE '^static tur_adt_Shape ctor_Shape_Rect\(' "$MAIN_C"; then
        pass "imported-ctor-defined-in-importer"
    else
        fail "imported-ctor-defined-in-importer" \
            "app__main.c calls ctor_Shape_Rect but does not define it"
    fi
fi

# --- 3) Run the genuinely multi-TU program. --------------------------------
# libturi.a is ASan-instrumented in a Debug build: try a plain link first and
# fall back, as run-any-type-id-multi-module.sh does.
if [ -f "$MAIN_C" ]; then
    RT_LIBS="$REPO/build/src/libturt_runtime.a $REPO/build/src/libturi.a"
    link_out=$(cc -I"$LIB_BD/obj" -o "$MULTI" "$LIB_BD/obj"/*.c \
                  $RT_LIBS -lm -lpthread 2>&1)
    if [ ! -x "$MULTI" ]; then
        link_out="$link_out
--- retry with -fsanitize=address ---
$(cc -fsanitize=address -I"$LIB_BD/obj" -o "$MULTI" "$LIB_BD/obj"/*.c \
      $RT_LIBS -lm -lpthread 2>&1)"
    fi
    if [ -x "$MULTI" ]; then
        pass "imported-ctor-multi-tu-links"
        multi_out=$("$MULTI" 2>&1)
        if [ "$multi_out" = "$EXPECTED" ]; then
            pass "imported-ctor-multi-tu-runs"
        else
            fail "imported-ctor-multi-tu-runs" "expected:
$EXPECTED
got:
$multi_out"
        fi
    else
        fail "imported-ctor-multi-tu-links" "could not link the emitted TUs: $link_out"
    fi
fi

# --- 4) Whole-program build: the two modes must agree. ---------------------
exe_out=$(cd "$WORK" && "$TUR" build "$PROJ" -o "$BIN" 2>&1)
exe_rc=$?
if [ $exe_rc -ne 0 ]; then
    fail "imported-ctor-whole-program-build" "tur build exit=$exe_rc: $exe_out"
elif [ -x "$BIN" ]; then
    pass "imported-ctor-whole-program-build"
    whole_out=$("$BIN" 2>&1)
    if [ "$whole_out" = "$EXPECTED" ]; then
        pass "imported-ctor-whole-program-runs"
    else
        fail "imported-ctor-whole-program-runs" "expected:
$EXPECTED
got:
$whole_out"
    fi
else
    fail "imported-ctor-whole-program-build" "binary not produced at $BIN"
fi

echo
echo "summary: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
