#!/usr/bin/env bash
# tests/run-sr2-seam.sh -- keep the SR2 seam's OFF path from rotting, now that
# by-value parametric sums are the default.
#
# A MULTI-VARIANT PARAMETRIC sum monomorph ((Opt2 int), (PRes cstr), and above
# all (Option A) / (Result A B)) flows BY VALUE by default since
# parametric-sum-byvalue graduated on 2026-08-27
# (docs/archive/sum-representation-plan.md SR2c).  TUR_SR2_APP_SUM_BYVALUE=0
# restores the int64 carrier for bisection -- which means nothing in the
# ordinary suite compiles a parametric sum through the carrier any more.  A gate
# nobody turns on decays to a gate nobody notices (the lesson of
# docs/archive/sanitizer-gate-not-armed-in-ci.md); this harness keeps the
# carrier path green so a bisection stays a switch instead of a re-excavation.
#
# This file is the mirror image of the harness of the same name that was RETIRED
# at graduation, and it inherits that harness's M7 population unchanged.
# Retiring it was the one thing the graduation got wrong.  The reasoning on
# record -- "every `bash tests/run.sh` compiles all eleven of its fixtures that
# way" now -- is true, and covers the ON path; it just leaves the OFF path with
# no cover at all, which is the state the harness had been built to prevent with
# the two paths swapped.  Both sibling seams took the other branch and FLIPPED
# their harness rather than deleting it (run-sr4-seam.sh, and
# run-option-niche-seam.sh: "Before the flip it kept the niche path green the
# same way").
#
# ---------------------------------------------------------------------------
# WHY THIS HARNESS SETS TWO ENV VARS, NOT ONE
#
# TUR_SR2_APP_SUM_BYVALUE=0 is NOT a one-variable switch, and that is a finding
# of this harness rather than a design anyone wrote down.  SR3's Option niche
# (TUR_OPTION_NICHE, default ON since 2026-09-03) is layered ON TOP of by-value
# parametric sums: it narrows an eligible `(Option P)` to its payload pointer,
# a representation that only exists because SR2 put the sum by value in the
# first place.  Turning SR2 off underneath a live niche pulls the rug out --
# measured 2026-09-04, six fixtures, four of them a COMPILER ABORT rather than
# a diagnostic (inline-c-option-byval-param, inline-c-carrier-producer-byval-
# positions, option-niche-string, option-niche-vec-closure-cmp) and one a
# silent wrong answer (httpd-req-string-opt).  All six recover when the niche
# comes off too.
#
# So the only coherent bisection mode is BOTH hatches down, and that is what
# this harness pins.  Anyone reaching for TUR_SR2_APP_SUM_BYVALUE=0 alone gets
# a crash whose cause is two layers away from the switch they flipped; see
# docs/archive/sr2-carrier-seam-rotted.md.
# ---------------------------------------------------------------------------
#
# Two checks, mirroring run-sr4-seam.sh and run-option-niche-seam.sh:
#   1. A CANARY: emit-c of parsec-tutorial under the seam must show the CARRIER
#      ctor signature (`static int64_t ctor_PRes_POK__*`).  By value the same
#      ctor returns its monomorph, so if the env var is ever renamed or the gate
#      stops reading it, this harness fails LOUDLY instead of silently
#      re-testing the default path.  (The ctor is spelled `ctor_PRes_POK__*` --
#      ADT-qualified, with a `ctor_POK` alias #define -- not the bare
#      `ctor_POK__*` the retired ON-path harness matched.  That rename is itself
#      evidence of the rot: the old canary regex matches nothing today, so a
#      naive un-retirement would have passed its canary vacuously.)
#   2. Every fixture below builds and prints its committed expected.stdout with
#      the seam ON.  VALUES, not just the build: this whole family's failures
#      hide behind function-pointer casts that cc cannot see through, and the
#      acceptance test (parsec-tutorial) has a history of building cleanly and
#      then SEGV'ing.
#
# NOT a snapshot check.  `TUR_SR2_APP_SUM_BYVALUE=0 bash tests/run.sh` reports
# ~157 failures, ~148 of which are `expected.c` codegen mismatches -- the
# snapshots are committed for the DEFAULT representation, so of course they move
# when the representation does.  That is noise, not signal, and it is why this
# harness compares stdout the way its retired ancestor did.
#
# Exit status: 0 if all checks pass, 1 otherwise.

set -uo pipefail
cd "$(dirname "$0")/.."

export TUR_SR2_APP_SUM_BYVALUE=0
export TUR_OPTION_NICHE=0            # see "WHY THIS HARNESS SETS TWO ENV VARS"
# The compiler binary is ASan-instrumented in Debug; emitted programs are not.
# Leak-checking the compile path stays the ordinary suite's job -- this harness
# answers "does the seam still produce correct programs".  The carrier path
# mallocs per construction and leaks by design (that leak is the whole reason by
# value became the default), so leak detection would fire on every fixture.
export ASAN_OPTIONS="${ASAN_OPTIONS:-detect_leaks=0}"

TUR="${TUR:-./build/tur}"
PASS=0
FAIL=0

pass() { PASS=$((PASS + 1)); echo "PASS $1"; }
fail() { FAIL=$((FAIL + 1)); echo "FAIL $1 -- $2"; }

if [ ! -x "$TUR" ]; then
    echo "FAIL sr2-seam -- tur binary not found at $TUR (build first)"
    exit 1
fi

# Stock macOS ships no timeout(1); mirror the other harnesses' fallback.
if command -v timeout >/dev/null 2>&1; then
    TIMEOUT="timeout 60"
else
    TIMEOUT=""
fi

WORKDIR="$(mktemp -d "${TMPDIR:-/tmp}/tur-sr2-seam.XXXXXX")"
trap 'rm -rf "$WORKDIR"' EXIT

# Fixture binaries link libturi like the main suite's do.
_tur_build_dir=$(dirname "$TUR")
export TUR_CC_FLAGS="${TUR_CC_FLAGS:--O2 -std=c99 -Wall -fno-strict-aliasing -L${_tur_build_dir}/src}"

# --- 1. Canary: the seam must actually bite. ------------------------------
canary_c="$WORKDIR/canary.c"
if ! $TIMEOUT "$TUR" emit-c tests/fixtures/parsec-tutorial/input.tur \
        > "$canary_c" 2>/dev/null; then
    fail "sr2-seam-canary" "emit-c of parsec-tutorial failed under TUR_SR2_APP_SUM_BYVALUE=0"
elif grep -qE "^static tur_adt_PRes__[A-Za-z0-9_]+ ctor_PRes_POK__" "$canary_c"; then
    fail "sr2-seam-canary" \
        "ctor_PRes_POK__* still returns its monomorph by value -- \
TUR_SR2_APP_SUM_BYVALUE=0 did not bite.  If the env var was renamed or the \
sr2_app_sum_byvalue gate moved, update this harness in the same change."
elif ! grep -qE "^static int64_t ctor_PRes_POK__" "$canary_c"; then
    fail "sr2-seam-canary" \
        "ctor_PRes_POK__* is neither by-value nor the int64 carrier -- the emitted \
shape changed and this canary no longer describes it."
else
    pass "sr2-seam-canary (ctor_PRes_POK__* returns the int64 carrier)"
fi

# --- 2. The population, compiled and RUN under the carrier. ----------------
# Part A -- inherited verbatim from the retired ON-path harness (the M7
# worklist).  These are USER parametric sums, and they were the whole
# population when the seam ran the other way:
#   hkt-cata-*: the fixpoint-functor carrier family (ExprF/Expr, ReF/Re).
#   hkt-fmap-byvalue-sum-element: a Functor instance over a parametric sum,
#     monomorphised per result element -- the discriminating probe reads the
#     SECOND child, so a layout mismatch shows up as a wrong value.
#   class-method-hkt-tyvar-grounding: a method-level tyvar in result position,
#     and the nested-ctor field channel -- the inner `(None)` of `(Some (None))`.
#   conv-with-narrowed-variant-parametric: the let use-site look-ahead -- an
#     unannotated `(Empty)` grounded from a later `(getv e)`.
#   poly-combinator-application-element-inference: a closure lifted out of a
#     POLYMORPHIC combinator, cloned per instantiation so its dispatch is typed.
#     Pins a non-zero expected.exit.
#   parsec-tutorial: the gate document's acceptance test.  PRes is the Option
#     shape verbatim, driven through parser-combinator closures.
#
# Part B -- the post-SR2b population, which the retired harness could not have
# had.  Its eleven fixtures were chosen when Option and Result were still
# discriminated records, so the seam only ever moved user ADTs.  SR2b made the
# two most-used types in the language the population, and THAT is what the OFF
# path now has to carry: the by-value/carrier Option and Result crossings, the
# `?` operator, inline-C builders straddling the boundary, catch-unwind returns,
# typeclass and HKT instances over both, and the niche fixtures that still work
# with both hatches down.
FIXTURES="
hkt-cata-captureless-fn-carrier-arm
hkt-cata-fmap-byvalue-carrier
hkt-cata-fn-arg-carrier
hkt-cata-fn-carrier-recursive
hkt-cata-mixed-fn-value-carrier
hkt-cata-wide-byvalue-carrier
hkt-fmap-byvalue-sum-element
class-method-hkt-tyvar-grounding
conv-with-narrowed-variant-parametric
poly-combinator-application-element-inference
parsec-tutorial
option-basic
option-byvalue-param-none-safe
option-construct-byvalue-return-spec
option-consumers-byvalue-arg
option-control-form-construct
option-map-byvalue-result-into-carrier-consumer-let-inside-arg
option-map-capturing-closure
option-map-literal-none-unannotated-lambda
option-niche-crossings
option-niche-string
option-result-c-abi
byvalue-option-if-join-call-arm
byvalue-option-over-parametric-monomorph
byval-result-field-access
byvalue-result-param-predicate
result-basic
result-collect
result-combinators
result-display
result-from-into
result-question-op
result-question-op-chain
result-question-op-typed
result-byvalue-tail-var-no-double-unbox
result-bridge-tail-call-to-inline-c
inline-c-result-builder
inline-c-typed-result-option
inline-c-option-byval-param
inline-c-carrier-producer-byval-positions
catch-unwind-branch-result-return
catch-unwind-byvalue-result-return
stackless-catch-unwind-result
cps-result-carrier-unbox
hkt-stdlib-option-result-instances
hkt-stdlib-result-ok-biased
hkt-do-m-result
class-method-result-into-generic
typeclass-return-dispatch-result-wrapped
constrained-loop-vec-push-byvalue-result-element
stdlib-result
stdlib-result-runtime
httpd-req-string-opt
sum-passthrough-param-not-dropped
option-niche-vec-closure-cmp
option-eq-nominal-payload-dispatch
"

# NOTHING IS EXCLUDED ANY MORE.  All three defects in
# docs/archive/sr2-carrier-seam-rotted.md are closed, so the two fixtures that
# were carried out of this list are back in it:
#
#   sum-passthrough-param-not-dropped -- excluded 2026-09-04, back the same day:
#     its "invalid initializer" was the ROS pointer-box slot read inline by the
#     nested-carrier-match binder.
#
#   option-niche-vec-closure-cmp -- the SILENT wrong answer (defect 3), back
#     2026-09-04.  `(eq? (some "aa") (some "aa"))` was false on the carrier
#     because no ABI specialization was minted, so the generic Eq[Option] body
#     kept the `int` representative and compared String payloads by pointer.
#     Two causes, both needed: `match` was missing from the spec-minting walk
#     entirely, and the walk did not recognise a dispatch receiver spelled as an
#     ascription to the instance's OWN constraint var.
#
# option-eq-nominal-payload-dispatch is defect 3's minimal repro, kept here as
# well as in the ordinary suite because it is green on the default path for a
# DIFFERENT reason than on this one (there the C signature changes and mints the
# spec by accident; here the dispatch trigger has to do it).
#
# NOT excluded-as-defect, by design on the niche axis and covered as such by
# run-option-niche-seam.sh: option-niche-vec-word (asserts the word form
# itself), option-niche-carrier-some-null-aborts and
# option-niche-null-payload-aborts (abort-behaviour fixtures that pin the
# niche's own runtime check).

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
    # A fixture may pin a non-zero exit (expected.exit), the way the main suite
    # does -- poly-combinator-application-element-inference returns 42 from main
    # by design.  The main suite also accepts the literal `nonzero`.  Absent the
    # file, 0 is the expectation.
    want_rc=0
    [ -f "$dir/expected.exit" ] && want_rc=$(cat "$dir/expected.exit")
    if [ "$want_rc" = "nonzero" ]; then
        if [ "$rc" -eq 0 ]; then
            fail "$fx" "program exited 0 (expected nonzero)"
            continue
        fi
    elif [ "$rc" != "$want_rc" ]; then
        fail "$fx" "program exited $rc (expected $want_rc)"
        continue
    fi
    if [ "$actual" = "$(cat "$expected")" ]; then
        pass "$fx"
    else
        fail "$fx" "stdout mismatch (expected $(head -c 40 "$expected" | tr '\n' ' ')...)"
    fi
done

echo ""
echo "sr2-seam summary: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ] || exit 1
exit 0
