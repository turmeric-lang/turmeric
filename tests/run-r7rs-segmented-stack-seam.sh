#!/usr/bin/env bash
# tests/run-r7rs-segmented-stack-seam.sh -- the r7rs segmented stack
# (docs/upcoming/r7rs-heap-frames-plan.md, option C) across the `#lang r7rs`
# fixture corpus, on one arm at a time.
#
#   SEG_ARM=off  (the default since graduation, 2026-10-25) the bisection
#                hatch: every `#lang r7rs` fixture compiled AND run with
#                TUR_R7RS_SEGMENTED_STACK=0, against its committed
#                expected.stdout and expected.exit -- the build every r7rs
#                program got before the feature graduated.
#   SEG_ARM=on   the default build, run with TUR_SEG_BASE_MB=8 so the deep
#                fixtures cross onto segments rather than staying on the
#                1 GiB stack `bash tests/run.sh` gives them.
#
# INVERTED AT GRADUATION, per the flags guide ("Graduating flips a default --
# so the old default's harness INVERTS"): every compiled r7rs program carries
# the check now, so `bash tests/run.sh` covers the ON path and nothing else
# compiles the off path -- this harness keeps the hatch working.  The canary
# below is what makes that arm honest.
#
# A CANARY first: under the hatch, emit-c of the deep-recursion fixture must
# carry no TUR_SEG_CHECK and no runtime (TUR_SEG_ON; the collector's and the
# prelude's copies of the record types, tur_seg_rec and tur_seg_piece, are in
# every r7rs program); the default build must carry the check, and a lingering
# --enable=r7rs-segmented-stack (a graduated no-op) must change nothing.  A
# renamed variable or a gate that stopped reading it fails here, loudly,
# instead of re-testing the default.
#
# Linux x86-64 and arm64, where the check is compiled in; elsewhere the arms
# are the same build and this skips.  Fixtures marked requires.interp-only,
# requires.dedicated-runner or requires.stress are left out, as run.sh leaves
# them out; requires.spices ones are left out when ../turmeric-spices is
# absent.  Exit 0 when everything passes.
set -uo pipefail
cd "$(dirname "$0")/.."

TUR="${TUR:-./build/tur}"
[ -x "$TUR" ] || { echo "r7rs-segmented-stack-seam: $TUR not built" >&2; exit 2; }
TUR="$(cd "$(dirname "$TUR")" && pwd)/$(basename "$TUR")"
case "$(uname -s)/$(uname -m)" in
    Linux/x86_64|Linux/aarch64) ;;
    *) echo "r7rs-segmented-stack-seam: SKIP (no check is compiled on $(uname -s)/$(uname -m))"; exit 0 ;;
esac
ARM="${SEG_ARM:-off}"
case "$ARM" in
    on)  ARM_ENV=(TUR_SEG_BASE_MB=8) ;;
    off) ARM_ENV=(TUR_R7RS_SEGMENTED_STACK=0) ;;
    *) echo "r7rs-segmented-stack-seam: SEG_ARM=$ARM: on or off" >&2; exit 2 ;;
esac
export ASAN_OPTIONS="${ASAN_OPTIONS:-detect_leaks=0}"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT
FAIL=0

# --- 1. The canary. ----------------------------------------------------------
canary=tests/fixtures/r7rs-segmented-stack-deep-recursion/input.tur
"$TUR" emit-c "$canary" > "$WORK/on.c" 2> /dev/null
TUR_R7RS_SEGMENTED_STACK=0 "$TUR" emit-c "$canary" > "$WORK/hatch.c" 2> /dev/null
"$TUR" --enable=r7rs-segmented-stack emit-c "$canary" > "$WORK/noop.c" 2> /dev/null
if ! grep -q 'TUR_SEG_CHECK(' "$WORK/on.c"; then
    echo "FAIL seam-canary -- the default build emitted no TUR_SEG_CHECK: this harness could never see the check"
    FAIL=$((FAIL + 1))
elif grep -q 'TUR_SEG_CHECK(\|define TUR_SEG_ON' "$WORK/hatch.c"; then
    echo "FAIL seam-canary -- under TUR_R7RS_SEGMENTED_STACK=0 the C still carries the check or its runtime:" \
         "the hatch did not bite (renamed, or emit_seg_stack_wanted stopped reading it)"
    FAIL=$((FAIL + 1))
elif ! cmp -s "$WORK/on.c" "$WORK/noop.c"; then
    echo "FAIL seam-canary -- a lingering --enable=r7rs-segmented-stack changed the C; it should be a no-op"
    FAIL=$((FAIL + 1))
else
    echo "PASS seam-canary"
fi

# --- 2. The population. ------------------------------------------------------
fixtures=()
for d in tests/fixtures/*/; do
    d="${d%/}"
    [ -f "$d/input.tur" ] && [ -f "$d/expected.stdout" ] || continue
    IFS= read -r first < "$d/input.tur"
    [[ "$first" == "#lang r7rs"* ]] || continue
    [ -f "$d/requires.interp-only" ] || [ -f "$d/requires.dedicated-runner" ] || [ -f "$d/requires.stress" ] && continue
    [ -f "$d/requires.spices" ] && [ ! -d ../turmeric-spices ] && continue
    fixtures+=("$d")
done

one() {
    local dir="$1" name rc want flags args=()
    name="$(basename "$dir")"
    flags=""; [ -f "$dir/flags" ] && flags="$(cat "$dir/flags")"
    if [ -f "$dir/run.args" ]; then
        local _ra
        while IFS= read -r _ra; do
            [ -z "$_ra" ] && continue
            args+=("$_ra")
        done < "$dir/run.args"
    fi
    local stdin=/dev/null; [ -f "$dir/input.stdin" ] && stdin="$dir/input.stdin"
    local tmo=60; [ -f "$dir/expected.timeout" ] && tmo="$(tr -d '[:space:]' < "$dir/expected.timeout")"
    # shellcheck disable=SC2086
    if ! env "${ARM_ENV[@]}" timeout 600 "$TUR" $flags build "$dir/input.tur" \
            -o "$WORK/$name" > "$WORK/$name.build" 2>&1; then
        echo "FAIL $name -- build failed: $(grep -m1 -i error "$WORK/$name.build" | cut -c1-160)"
        return
    fi
    (cd "$dir" && env "${ARM_ENV[@]}" timeout "$((tmo * 3))" "$WORK/$name" "${args[@]}" \
        < "$stdin" > "$WORK/$name.out" 2> "$WORK/$name.err")
    rc=$?
    want=0; [ -f "$dir/expected.exit" ] && want="$(tr -d '[:space:]' < "$dir/expected.exit")"
    if [ "$rc" = 124 ]; then
        echo "FAIL $name -- timed out (>$((tmo * 3))s), SEG_ARM=$ARM"
    elif { [ "$want" = nonzero ] && [ "$rc" = 0 ]; } || { [ "$want" != nonzero ] && [ "$rc" != "$want" ]; }; then
        echo "FAIL $name -- exit $rc, expected $want, SEG_ARM=$ARM; stderr tail:"
        tail -4 "$WORK/$name.err" | cut -c1-300 | sed 's/^/    /'
    elif ! diff -q "$WORK/$name.out" "$dir/expected.stdout" > /dev/null; then
        echo "FAIL $name -- stdout differs, SEG_ARM=$ARM"
        diff "$WORK/$name.out" "$dir/expected.stdout" | head -6 | sed 's/^/    /'
    else
        echo "PASS $name"
    fi
}
export -f one
export TUR WORK ARM
export ARM_ENV_S="${ARM_ENV[*]}"
# Arrays do not cross `bash -c`; the one word each arm sets does.
printf '%s\n' "${fixtures[@]}" |
    xargs -P "$(nproc)" -I{} bash -c 'ARM_ENV=($ARM_ENV_S); one "$1" > "$WORK/$(basename "$1").result" 2>&1' _ {}
for d in "${fixtures[@]}"; do cat "$WORK/$(basename "$d").result"; done | tee "$WORK/results"
n_pass=$(grep -c '^PASS' "$WORK/results")
n_fail=$(grep -c '^FAIL' "$WORK/results")
FAIL=$((FAIL + n_fail))
echo "r7rs-segmented-stack-seam (SEG_ARM=$ARM): $n_pass passed, $n_fail failed of ${#fixtures[@]} fixtures"
[ "$FAIL" = 0 ]
