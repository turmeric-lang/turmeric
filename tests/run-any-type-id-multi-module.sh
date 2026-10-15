#!/usr/bin/env bash
# tests/run-any-type-id-multi-module.sh -- pin `any-type-ids-are-per-tu`.
#
# An `any` box carries a type id.  That id used to be `TUR_ANY_ID_BASE +
# first-seen index` within one `EmitCtx` -- and `EmitCtx` is per translation
# unit, so two TUs numbered the same type differently.  A value widened in one
# module was then misidentified in another, four ways:
#
#   1. `type-of` returned a DIFFERENT type's name.
#   2. `is?` was a false negative on the correct type.
#   3. A valid `cast` panicked ("cast: any holds Beta, not Beta" in the
#      degenerate case, another type's name in general).
#   4. `__tur_any_drop` read the wrong row's `boxed` flag, so one TU called
#      free() on a handle another TU owned -- or leaked, in the mirror case.
#
# IMPORTANT, and the reason this harness exists at all: `tur build <dir>` on a
# single-`main` project reroutes to a WHOLE-PROGRAM single-file build that
# inlines every module into one TU.  One `EmitCtx`, one consistent numbering,
# bug invisible.  Every existing `any` fixture goes through that path, which is
# exactly why four wrong behaviours went unnoticed.  Only separate compilation
# (`tur build --shared`, and `emit-c --output-dir`) splits the numbering, so
# this drives the library build to pin it -- and runs BOTH build modes, because
# agreeing with itself is not the same as being right.
#
# The fix makes the id a hash of the type's identity key (no coordination
# needed between TUs) and publishes name+boxed rows into a runtime registry
# that lookups walk the union of.

set -uo pipefail
cd "$(dirname "$0")/.."
REPO="$PWD"

TUR="$REPO/build/tur"
FIXTURE="$REPO/tests/fixtures/any-type-id-multi-module"
WORK="$(mktemp -d -t tur-anyid.XXXXXX)"
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

cp -R "$FIXTURE" "$WORK/proj"
PROJ="$WORK/proj"
LIB_BD="$WORK/lib-bd"
SO="$WORK/libany.so"
BIN="$WORK/app"
MULTI="$WORK/multi"

EXPECTED=$'Gamma\nDelta\nBeta\n1\n7\nHeapThing\n11\nfn\n1\n0\n42'

# --- 1) Separate compilation: the genuine repro. ---------------------------
build_out=$(cd "$WORK" && "$TUR" build --shared "$PROJ" \
    --build-dir "$LIB_BD" -o "$SO" 2>&1)
build_rc=$?
if [ $build_rc -ne 0 ]; then
    fail "any-type-id-shared-build" "tur build --shared exit=$build_rc: $build_out"
else
    pass "any-type-id-shared-build"
fi

PRODUCER_C="$LIB_BD/obj/producer.c"
MAIN_C="$LIB_BD/obj/main.c"

# --- 2) The structural pin: both TUs must mint the SAME id for a shared type.
# This is the assertion that would have caught the original defect directly,
# without depending on a program happening to observe the divergence.
if [ -f "$PRODUCER_C" ] && [ -f "$MAIN_C" ]; then
    pass "any-type-id-per-module-c-emitted"
    for ty in Beta HeapThing; do
        # Rows are emitted as `{ <id>LL, "<Name>", <boxed>, <drop> },` -- the
        # drop-glue slot arrived with the deep __tur_any_drop fix.  Match the
        # PREFIX through the boxed flag rather than the whole row: id and boxed
        # are what this assertion is about, and a prefix keeps the test from
        # breaking again the next time the row grows a field.
        p_row=$(grep -oE "\{ -?[0-9]+LL, \"$ty\", [01]," "$PRODUCER_C" | head -1)
        m_row=$(grep -oE "\{ -?[0-9]+LL, \"$ty\", [01]," "$MAIN_C" | head -1)
        if [ -z "$p_row" ] || [ -z "$m_row" ]; then
            fail "any-type-id-agrees-$ty" \
                "row for $ty missing (producer='$p_row' main='$m_row')"
        elif [ "$p_row" = "$m_row" ]; then
            # Same id AND same boxed flag -- failure mode 4 rides on the latter.
            pass "any-type-id-agrees-$ty"
        else
            fail "any-type-id-agrees-$ty" \
                "TUs disagree: producer='$p_row' main='$m_row'"
        fi
    done
    # any-fn-tag-does-not-discriminate-signatures: the same pin for a FUNCTION
    # payload, which cannot use the loop above because its row's NAME is "fn"
    # for every signature -- `head -1` would compare producer's one fn row
    # against whichever of main's two came first.  Take producer's id and
    # require main to carry that exact id, which is the real question: does a
    # key that is a rendered SIGNATURE (not an ADT name) hash the same in two
    # TUs?  A closure rides the tag's value word, so boxed must be 0 -- if it
    # ever flipped, a drop in main would free() a closure producer owns.
    p_fn=$(grep -oE '\{ -?[0-9]+LL, "fn", [01],' "$PRODUCER_C" | head -1)
    if [ -z "$p_fn" ]; then
        fail "any-type-id-agrees-fn" "producer emitted no \"fn\" row"
    elif ! grep -qF "$p_fn" "$MAIN_C"; then
        fail "any-type-id-agrees-fn" \
            "main.c carries no row matching producer's '$p_fn'"
    else
        # Pull the boxed flag out by POSITION.  This used to read the last
        # field (`${p_fn##*, }`), which silently became the drop-glue slot when
        # the row grew one -- the kind of check that keeps passing while
        # measuring the wrong thing.
        fn_boxed=$(printf '%s' "$p_fn" | sed -E 's/.*"fn", ([01]),.*/\1/')
        if [ "$fn_boxed" != "0" ]; then
            fail "any-type-id-agrees-fn" "closure row must be boxed=0, got '$p_fn'"
        else
            pass "any-type-id-agrees-fn"
        fi
    fi

    # Beta is by-value (heap-boxed at the widen); HeapThing rides the value word
    # and must NOT be freed by a drop site.  If these ever coincide the boxed
    # flag has stopped meaning anything and mode 4 is back.
    if grep -qE '\{ -?[0-9]+LL, "Beta", 1,' "$PRODUCER_C" &&
       grep -qE '\{ -?[0-9]+LL, "HeapThing", 0,' "$PRODUCER_C"; then
        pass "any-type-id-boxed-flags-distinct"
    else
        fail "any-type-id-boxed-flags-distinct" \
            "expected Beta boxed=1 and HeapThing boxed=0 in $PRODUCER_C"
    fi
else
    fail "any-type-id-per-module-c-emitted" \
        "expected per-module C at $PRODUCER_C and $MAIN_C"
fi

# --- 3) Run the genuinely multi-TU program. --------------------------------
# The structural check above is the precise pin; this is the end-to-end proof
# that the four behaviours are right when the ids really do come from separate
# translation units.  libturi.a carries the contract handler; in a Debug build
# it is ASan-instrumented, so try a plain link first and fall back.
if [ -f "$PRODUCER_C" ] && [ -f "$MAIN_C" ]; then
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
        pass "any-type-id-multi-tu-links"
        multi_out=$("$MULTI" 2>&1)
        if [ "$multi_out" = "$EXPECTED" ]; then
            pass "any-type-id-multi-tu-runs"
        else
            fail "any-type-id-multi-tu-runs" \
                "expected:
$EXPECTED
got:
$multi_out"
        fi
    else
        fail "any-type-id-multi-tu-links" "could not link the emitted TUs: $link_out"
    fi
fi

# --- 4) Whole-program build: the two modes must agree. ---------------------
# Single-TU numbering was always self-consistent, so this half never failed --
# it is here so a future change cannot fix one mode by breaking the other.
exe_out=$(cd "$WORK" && "$TUR" build "$PROJ" -o "$BIN" 2>&1)
exe_rc=$?
if [ $exe_rc -ne 0 ]; then
    fail "any-type-id-whole-program-build" "tur build exit=$exe_rc: $exe_out"
elif [ -x "$BIN" ]; then
    pass "any-type-id-whole-program-build"
    whole_out=$("$BIN" 2>&1)
    if [ "$whole_out" = "$EXPECTED" ]; then
        pass "any-type-id-whole-program-runs"
    else
        fail "any-type-id-whole-program-runs" \
            "expected:
$EXPECTED
got:
$whole_out"
    fi
else
    fail "any-type-id-whole-program-build" "binary not produced at $BIN"
fi

echo
echo "summary: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
