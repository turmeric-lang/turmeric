#!/usr/bin/env bash
# tests/run-option-niche-seam.sh -- keep the option niche's OFF path from
# rotting, now that the niche is the default.
#
# An eligible `(Option P)` is carried as its payload pointer by default since
# the experiment graduated out of --enable=option-niche on 2026-09-03
# (docs/archive/sr3-option-niche-plan.md), and a Vec stores such an element
# as that word (container-element-form-plan, CE2).  TUR_OPTION_NICHE=0 restores
# the tagged 16-byte monomorph and the boxed slot for bisection -- which means
# nothing in the ordinary suite compiles the eligible population through the
# tagged form any more.  A gate nobody turns on decays to a gate nobody
# notices (the SR4 seam's lesson); this harness keeps the OFF path green so a
# bisection stays a one-variable switch instead of a re-excavation.  Before the
# flip it kept the niche path green the same way, and it is where the class-2
# double-wrap defect of 2026-09-03 should have been caught -- the population
# now carries the generic-helper and closure-comparator shapes that found it.
#
# Two checks, mirroring run-sr4-seam.sh:
#   1. A CANARY: emit-c of httpd-req-string-opt under TUR_OPTION_NICHE=0 must
#      mint the tagged `tur_adt_Option__String` typedef and must NOT emit the
#      niche identity ctor.  If the env var is renamed or the gate stops reading
#      it, this fails LOUDLY instead of silently re-testing the default.
#   2. The niche-eligible population, compiled and RUN with the niche OFF
#      against each fixture's committed expected.stdout.
#
# Exit status: 0 if all checks pass, 1 otherwise.

set -uo pipefail
cd "$(dirname "$0")/.."

export ASAN_OPTIONS="${ASAN_OPTIONS:-detect_leaks=0}"

TUR="${TUR:-./build/tur}"
export TUR_OPTION_NICHE=0
PASS=0
FAIL=0

pass() { PASS=$((PASS + 1)); echo "PASS $1"; }
fail() { FAIL=$((FAIL + 1)); echo "FAIL $1 -- $2"; }

if [ ! -x "$TUR" ]; then
    echo "FAIL option-niche-seam -- tur binary not found at $TUR (build first)"
    exit 1
fi

if command -v timeout >/dev/null 2>&1; then
    TIMEOUT="timeout 30"
else
    TIMEOUT=""
fi

WORKDIR="$(mktemp -d "${TMPDIR:-/tmp}/tur-option-niche-seam.XXXXXX")"
trap 'rm -rf "$WORKDIR"' EXIT

# Fixture binaries link libturi like the main suite's do.
_tur_build_dir=$(dirname "$TUR")
export TUR_CC_FLAGS="${TUR_CC_FLAGS:--O2 -std=c99 -Wall -fno-strict-aliasing -L${_tur_build_dir}/src}"

# --- 1. Canary: the flag must actually bite. ------------------------------
canary_c="$WORKDIR/canary.c"
if ! $TIMEOUT "$TUR" emit-c tests/fixtures/httpd-req-string-opt/input.tur \
        > "$canary_c" 2>/dev/null; then
    fail "option-niche-canary" "emit-c of httpd-req-string-opt failed under TUR_OPTION_NICHE=0"
elif grep -q "static void \* ctor_Option_Some__String(" "$canary_c"; then
    fail "option-niche-canary" \
        "ctor_Option_Some__String is still the niche identity -- TUR_OPTION_NICHE=0 did not bite.  If \
the env var was renamed or sr3_option_niche moved, update this harness in the \
same change."
elif ! grep -q "typedef struct tur_adt_Option__String" "$canary_c"; then
    fail "option-niche-canary" \
        "the tagged tur_adt_Option__String typedef was not minted with the niche off"
else
    pass "option-niche-canary (tagged Option__String typedef present, no identity ctor)"
fi

# --- 2. The eligible population, compiled and RUN with the niche OFF. -------
# option-niche-*: the niche's own fixtures (the ordinary suite covers them on
#   the default path; running them here with the niche off is the point).
#   option-niche-vec-closure-cmp carries the closure-comparator shapes
#   (typed `vec-eq?` comparator, let-bound slot word, `(eq? v w)` through
#   the synthesized `__cmp_slot_` comparator); its output is the same either
#   way.  The generic-helper shape (option-niche-vec-word, a `push-it`/
#   `get-it` spec) is NOT here: it asserts the word form itself ("word" vs
#   "box"), and its `get-it` spec does not even build with the niche off --
#   docs/archive/generic-vec-read-wrapper-spec-returns-carrier-word.md, the
#   pre-existing tagged-element defect.  The ordinary suite covers it.
# httpd-req-string-opt / bound-string / inline-c-option-byval-param: the
#   (Option String) census shapes -- header lookups, bounds, inline-C
#   builders straddling the by-value param ABI.
# inline-c-result-builder / inline-c-typed-result-option: Result stays
#   carrier/by-value next to a niche Option in one program.
# option-of-tvec-eq: the original SR3 gate fixture -- an eligible
#   option<Vec int> through a typeclass Eq dictionary's erased crossing.
FIXTURES="
option-niche-string
option-niche-crossings
option-niche-vec-closure-cmp
httpd-req-string-opt
bound-string
inline-c-option-byval-param
inline-c-result-builder
inline-c-typed-result-option
option-of-tvec-eq
"

for fx in $FIXTURES; do
    dir="tests/fixtures/$fx"
    input="$dir/input.tur"
    expected="$dir/expected.stdout"
    if [ ! -f "$input" ] || [ ! -f "$expected" ]; then
        fail "$fx" "fixture missing (input.tur or expected.stdout)"
        continue
    fi
    exe="$WORKDIR/$fx.bin"
    mkdir -p "$(dirname "$exe")"
    if ! $TIMEOUT "$TUR" build "$input" -o "$exe" > /dev/null 2>"$WORKDIR/err"; then
        fail "$fx" "build failed with the niche off: $(tail -2 "$WORKDIR/err" | head -1)"
        continue
    fi
    if ! $TIMEOUT "$exe" > "$WORKDIR/out" 2>/dev/null; then
        fail "$fx" "run failed (nonzero exit) with the niche off"
        continue
    fi
    if ! diff -q "$expected" "$WORKDIR/out" > /dev/null 2>&1; then
        fail "$fx" "stdout mismatch with the niche off"
        continue
    fi
    pass "$fx"
done

echo
echo "option-niche-seam summary: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
