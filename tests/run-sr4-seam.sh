#!/usr/bin/env bash
# tests/run-sr4-seam.sh -- keep the SR4 seam's green state from rotting.
#
# Recursive sums (Term, Subst, Stream, Regex, ...) flow BY VALUE by default
# since RM4 (2026-09-02; see the is_self_recursive test in src/compiler/types.c
# and the SR plan's SR4 section).  TUR_SR4_RECURSIVE_CARRIER=1 restores the
# int64 carrier for A/B measurement, which means nothing in the ordinary suite
# ever compiles a recursive sum through the carrier any more.  A gate nobody
# turns on decays to a gate nobody notices (the lesson of
# docs/archive/sanitizer-gate-not-armed-in-ci.md); this harness keeps the
# carrier path green as the compiler moves, so the A/B stays a one-line
# switch instead of a re-excavation.  (Before the flip it kept the by-value
# path green the same way; the population below is unchanged.)
#
# Two checks:
#   1. A CANARY: emit-c of a logic fixture must show the carrier ctor
#      signature (`static int64_t ctor_Subst_SBind(`).  By value the same ctor
#      returns tur_adt_Subst, so if the env var is ever renamed or the gate
#      stops reading it, this harness fails LOUDLY instead of silently
#      re-testing the default path.
#   2. Every fixture below -- the recursive-sum population plus the SR1/fat-ABI
#      fixtures the seam must not perturb -- builds and prints its committed
#      expected.stdout with the seam ON.
#
# Exit status: 0 if all checks pass, 1 otherwise.

set -uo pipefail
cd "$(dirname "$0")/.."

export TUR_SR4_RECURSIVE_CARRIER=1
# The compiler binary is ASan-instrumented in Debug; emitted programs are not.
# Leak-checking the compile path stays the ordinary suite's job -- this
# harness answers "does the seam still produce correct programs".
export ASAN_OPTIONS="${ASAN_OPTIONS:-detect_leaks=0}"

TUR="${TUR:-./build/tur}"
PASS=0
FAIL=0

pass() { PASS=$((PASS + 1)); echo "PASS $1"; }
fail() { FAIL=$((FAIL + 1)); echo "FAIL $1 -- $2"; }

if [ ! -x "$TUR" ]; then
    echo "FAIL sr4-seam -- tur binary not found at $TUR (build first)"
    exit 1
fi

# Stock macOS ships no timeout(1); mirror the other harnesses' fallback.
if command -v timeout >/dev/null 2>&1; then
    TIMEOUT="timeout 30"
else
    TIMEOUT=""
fi

WORKDIR="$(mktemp -d "${TMPDIR:-/tmp}/tur-sr4-seam.XXXXXX")"
trap 'rm -rf "$WORKDIR"' EXIT

# --- 1. Canary: the seam must actually bite. ------------------------------
canary_c="$WORKDIR/canary.c"
if ! $TIMEOUT "$TUR" emit-c tests/fixtures/logic-unify-basic/input.tur \
        > "$canary_c" 2>/dev/null; then
    fail "sr4-seam-canary" "emit-c of logic-unify-basic failed under the seam"
elif grep -q "static int64_t ctor_Subst_SBind(" "$canary_c"; then
    pass "sr4-seam-canary (ctor_Subst_SBind returns the int64 carrier)"
else
    fail "sr4-seam-canary" \
        "ctor_Subst_SBind is not the carrier -- the seam did not bite.  If \
TUR_SR4_RECURSIVE_CARRIER was renamed or the is_self_recursive gate moved, \
update this harness in the same change."
fi

# --- 2. The recursive-sum population, compiled and RUN under the seam. -----
# logic-* / hkt-stdlib-logic-instances: Term/Subst/Stream (stdlib/logic.tur).
# re-* / reader-macros-rx-literal: Regex/RxCls/RxPos/RxStrs (stdlib/re.tur).
# fix / recursive-types/*: user-declared recursive sums.
# sr1-* / fat-dispatch-*: non-recursive by-value fixtures the seam must not
# perturb (they pin the SR1 layout and the unified fat ABI).
FIXTURES="
logic-unify-basic
logic-unify-fail
logic-query
logic-fresh
logic-conjoined
logic-disjoined
logic-reify
logic-occurs-check
logic-lazy-infinite
hkt-stdlib-logic-instances
re-string
re-union-patterns
re-pure-match-find-replace
reader-macros-rx-literal
fix
recursive-types/simple-tree
recursive-types/mutual-recursion
recursive-types/fix-type
recursive-types/hkt-recursive
sr1-sum-byvalue
sr1-sum-vec-element
fat-dispatch-wide-byval-arg
fat-dispatch-parametric-monomorph-return
"

for fx in $FIXTURES; do
    dir="tests/fixtures/$fx"
    input="$dir/input.tur"
    expected="$dir/expected.stdout"
    if [ ! -f "$input" ] || [ ! -f "$expected" ]; then
        fail "$fx" "fixture missing (input.tur or expected.stdout)"
        continue
    fi
    bin="$WORKDIR/$(echo "$fx" | tr '/' '_').bin"
    build_out=$($TIMEOUT "$TUR" build "$input" -o "$bin" 2>&1)
    if [ $? -ne 0 ] || [ ! -x "$bin" ]; then
        fail "$fx" "build failed: $(echo "$build_out" | grep -m1 'error' || echo "$build_out" | tail -1)"
        continue
    fi
    actual=$($TIMEOUT "$bin" 2>/dev/null)
    rc=$?
    if [ $rc -ne 0 ]; then
        fail "$fx" "program exited $rc"
        continue
    fi
    if [ "$actual" = "$(cat "$expected")" ]; then
        pass "$fx"
    else
        fail "$fx" "stdout mismatch (expected $(head -c 40 "$expected" | tr '\n' ' ')...)"
    fi
done

echo ""
echo "sr4-seam summary: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ] || exit 1
exit 0
