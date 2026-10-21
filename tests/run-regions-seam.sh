#!/usr/bin/env bash
# tests/run-regions-seam.sh -- keep the regions OFF path from rotting, now
# that regions are the default.
#
# INVERTED at graduation (2026-09-05), per the flags guide's rule: when a
# feature graduates, the path it STOPPED being becomes the path nothing
# compiles any more.  Before the flip this harness ran the spine-carrying and
# bracket-carrying population with `--enable=regions` and asserted output
# equality against the flag-off run, so the R2 routing and the R4 brackets
# could not change an answer.  Since 2026-09-05 every `bash tests/run.sh`
# compiles that population WITH regions -- so this harness now runs it with
# `TUR_REGIONS=0`, the bisection hatch that restores the pre-graduation
# build: plain malloc at the four routed ctor sites, no bracket around
# bt-scope / with-region, no atexit shutdown.  A hatch nobody turns on decays
# into a hatch nobody notices (run-sr4-seam.sh's lesson, and the SR2 seam that
# rotted for want of exactly this); keeping the OFF path green is what makes a
# bisection a one-variable switch instead of a re-excavation.
#
# Two checks, mirroring run-option-niche-seam.sh:
#   1. A CANARY: under TUR_REGIONS=0, emit-c of region-scope-value-survives
#      must reference NO region symbol at all -- every region emission (the
#      preamble externs, the ctor routing, the bracket, the atexit) is gated on
#      the one predicate the hatch clears, so a single `tur_region_` in the
#      output means the env var was renamed or the gate stopped reading it,
#      and this fails LOUDLY instead of silently re-testing the default.  The
#      positive control -- the same file emitted on the default -- must open a
#      region, so a harness that could never see a bracket is caught too.
#   2. The population, compiled and RUN with regions OFF against each fixture's
#      committed expected.stdout -- which is the DEFAULT-ON expectation, so
#      this is also the output-equality assertion the pre-flip harness made,
#      with the arms swapped.
#   3. The runtime from SOURCE with regions OFF (`--runtime=source`, no
#      archive): hamt.c is host runtime and calls the region note directly,
#      but the program pastes no region runtime on this arm, so it failed to
#      link (regions-off-source-runtime-link-failure).  Every other check here
#      links the archive, which carries region.o, and could not see it.
#
# `region-scope-adt-result`, `-shapes`, `-vec-scalar`, `-nontail-cps` and
# `region-shutdown-order` are deliberately NOT listed: they are hook-driven
# (no input.tur for this loop) and each hook already runs BOTH arms itself,
# with TUR_REGIONS=0 as its off arm.
#
# Exit status: 0 if all checks pass, 1 otherwise.

set -uo pipefail
cd "$(dirname "$0")/.."

export ASAN_OPTIONS="${ASAN_OPTIONS:-detect_leaks=0}"

TUR="${TUR:-./build/tur}"
PASS=0
FAIL=0

pass() { PASS=$((PASS + 1)); echo "PASS $1"; }
fail() { FAIL=$((FAIL + 1)); echo "FAIL $1 -- $2"; }

if [ ! -x "$TUR" ]; then
    echo "FAIL regions-seam -- tur binary not found at $TUR (build first)"
    exit 1
fi

if command -v timeout >/dev/null 2>&1; then
    TIMEOUT="timeout 30"
else
    TIMEOUT=""
fi

WORKDIR="$(mktemp -d "${TMPDIR:-/tmp}/tur-regions-seam.XXXXXX")"
trap 'rm -rf "$WORKDIR"' EXIT

# --- 1. Canary: the hatch must actually bite, and the default must bracket. --
canary="tests/fixtures/region-scope-value-survives/input.tur"
if ! TUR_REGIONS=0 $TIMEOUT "$TUR" emit-c "$canary" > "$WORKDIR/off.c" 2>/dev/null; then
    fail "regions-canary" "emit-c of region-scope-value-survives failed under TUR_REGIONS=0"
elif grep -q 'tur_region_' "$WORKDIR/off.c"; then
    fail "regions-canary" \
        "a tur_region_ symbol is still emitted under TUR_REGIONS=0 -- the hatch did not bite.  If \
the env var was renamed or regions_enabled() stopped reading g_opt_regions, update this harness in \
the same change."
elif ! $TIMEOUT "$TUR" emit-c "$canary" > "$WORKDIR/on.c" 2>/dev/null; then
    fail "regions-canary" "emit-c of region-scope-value-survives failed on the default"
elif ! grep -q 'tur_region_push()' "$WORKDIR/on.c"; then
    fail "regions-canary" \
        "the default emit opened no region for a bt-scope -- regions are not on by default, so \
this harness would be comparing the off path against itself"
else
    pass "regions-canary (TUR_REGIONS=0 emits no region symbol; the default brackets)"
fi

# --- 2. The population, compiled and RUN with regions OFF. -------------------
# Spine-carrying fixtures (a `:heap` ADT monomorph ctor -- the node R2 routes):
#   refined-nonempty, constrained-defn-cons-return-monomorphize, zipper-basic,
#   re-string, option-niche-crossings, hkt-stdlib-result-ok-biased.
# Bracket-carrying fixtures (a bt-scope / with-region is a boundary now):
#   sx2-trail-combinators, sx2-dfs-driver, self-recursive-goal-into-fat-sink,
#   region-scope-value-survives, region-scope-escape-refused,
#   region-scope-void-body, region-with-region.
# Store-side / erasure / inline-C escapes (region-lock-hardening): every value
# is read AFTER the pop, so the default arm proves the note blocked the rewind
# and this arm proves the answer is the same without a region at all:
#   region-escape-via-store, region-escape-via-erasure,
#   region-escape-via-inline-c (which also pins the other direction -- a
#   bracket whose inline-C sees only scalars still rewinds), and
#   region-escape-via-callcc (r7rs-lang-plan T8: a Scheme continuation's
#   stack image, which also exercises the variadic dynamic call's rest
#   packing on this arm -- it called an undeclared region allocator).
# The panic jam (region-escape-through-unhooked-stores item 3): a generation a
# panic strands is retired by its bracket and by the catch boundary; with
# regions off there is nothing to strand and the depth reads 0 all the same:
#   region-catch-retires-stranded-generation.
# The dead generic-base constructor (dead-base-ctor-ignores-regions-off): a
# phantom-parameter `:heap` generic's base ctor called the region allocator
# whatever the hatch said, which is undeclared with regions off:
#   heap-phantom-generic-dead-base-ctor.
FIXTURES="
refined-nonempty
constrained-defn-cons-return-monomorphize
zipper-basic
re-string
option-niche-crossings
hkt-stdlib-result-ok-biased
sx2-trail-combinators
sx2-dfs-driver
self-recursive-goal-into-fat-sink
region-scope-value-survives
region-scope-escape-refused
region-scope-void-body
region-with-region
region-escape-via-store
region-escape-via-erasure
region-escape-via-inline-c
region-escape-via-callcc
region-catch-retires-stranded-generation
heap-phantom-generic-dead-base-ctor
"

for fx in $FIXTURES; do
    dir="tests/fixtures/$fx"
    input="$dir/input.tur"
    expected="$dir/expected.stdout"
    if [ ! -f "$input" ] || [ ! -f "$expected" ]; then
        fail "$fx" "fixture missing (input.tur or expected.stdout)"
        continue
    fi
    # A call to a region function the hatch left undeclared is an error on
    # gcc 14+ / clang 16+ but only a warning (and a truncated pointer) on older
    # compilers, so make it an error everywhere: dead-base-ctor-ignores-
    # regions-off passed this harness on gcc 13.
    out=$(CC="${CC:-cc} -Werror=implicit-function-declaration" \
          TUR_REGIONS=0 $TIMEOUT "$TUR" run "$input" 2>"$WORKDIR/err")
    rc=$?
    if [ "$rc" -ne 0 ] && [ -z "$out" ]; then
        fail "$fx" "did not run with regions off: $(tail -2 "$WORKDIR/err" | head -1)"
        continue
    fi
    if [ "$out" != "$(cat "$expected")" ]; then
        fail "$fx" "output differs with TUR_REGIONS=0 (vs committed default-on expected.stdout)"
        diff <(printf '%s\n' "$out") "$expected" | head -10
        continue
    fi
    pass "$fx"
done

# --- 3. Regions OFF with the runtime compiled from source. -------------------
src_fx="hamt-basic"
src_dir="tests/fixtures/$src_fx"
if TUR_REGIONS=0 $TIMEOUT "$TUR" build --runtime=source "$src_dir/input.tur" \
        -o "$WORKDIR/src-off" >/dev/null 2>"$WORKDIR/err"; then
    out=$($TIMEOUT "$WORKDIR/src-off" 2>/dev/null)
    if [ "$out" = "$(cat "$src_dir/expected.stdout")" ]; then
        pass "$src_fx (--runtime=source, regions off)"
    else
        fail "$src_fx (--runtime=source, regions off)" "output differs from expected.stdout"
    fi
else
    fail "$src_fx (--runtime=source, regions off)" \
        "did not build: $(grep -m1 -E 'error|undefined' "$WORKDIR/err")"
fi

echo
echo "regions-seam summary: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
