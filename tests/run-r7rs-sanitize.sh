#!/usr/bin/env bash
# tests/run-r7rs-sanitize.sh -- r7rs-lang-plan T8: every `#lang r7rs` fixture
# compiled with AddressSanitizer and UndefinedBehaviorSanitizer, and run.
#
# tests/run.sh builds fixture programs WITHOUT sanitizers, so a use-after-free,
# an overflow or undefined behavior in the prelude's inline C, the emitted
# Scheme code or the runtime under it passes the ordinary suite whenever the
# output happens to come out right.  This harness makes those fatal:
#
#   - ASan aborts on the first memory error;
#   - UBSan is built with -fno-sanitize-recover, so a finding aborts too;
#   - each fixture must still print its expected.stdout and exit with its
#     expected.exit (0 when there is none);
#   - a fixture carrying `sanitize.torture` (an interval) runs with
#     TUR_GC_TORTURE set to it: the r7rs-threads-* stress cases, whose point
#     is the collector stopping threads mid-allocation, under ASan.
#
# LEAKS ARE NOT CHECKED here (detect_leaks=0), on purpose: a Scheme program's
# pairs, vectors, strings and procedures are never freed -- the memory model
# has no collector for them -- so every fixture leaks by design.  That is
# docs/reported/r7rs-heap-data-never-reclaimed.md; the scratch leaks T8 found
# are fixed, and the plan's T8 note tabulates what is freed and what is not.
#
# The interpreter side needs no harness of its own: `tur --interpret` is the
# sanitized Debug binary, so tests/run-turi.sh already runs every r7rs fixture
# under ASan and UBSan.
#
# Built at -O1 (ASan's stack traces keep their frames) plus
# -foptimize-sibling-calls.  A CPS loop no longer needs it -- a self tail call
# is a backedge and a mutual one a jump inside a fused group
# (docs/archive/cps-self-tail-call-relies-on-sibling-call.md) -- but the flag
# is kept so a remaining C tail call elsewhere does not turn a sanitizer run
# into a stack-depth test.
#
# Linux only, like run-leak-check.sh.  Exit 0 when every fixture is clean.

set -uo pipefail
cd "$(dirname "$0")/.."

TUR="${TUR:-./build/tur}"
BUILD_CC="${CC:-cc}"
[ -x "$TUR" ] || { echo "r7rs-sanitize: $TUR not built" >&2; exit 2; }
if [ "$(uname -s)" != "Linux" ]; then
    echo "r7rs-sanitize: SKIP (host is $(uname -s))"
    exit 0
fi

_build_dir="$(cd "$(dirname "$TUR")" && pwd)"
TUR="$_build_dir/$(basename "$TUR")"
CC_FLAGS="-O1 -foptimize-sibling-calls -g -std=c99 -Wall -fno-strict-aliasing -fno-omit-frame-pointer -fsanitize=address,undefined -fno-sanitize-recover=undefined -L${_build_dir}/src"

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

# The population: the r7rs-* fixtures and any other fixture whose entry file
# is a Scheme program.  A fixture driven by a hook.sh has no input.tur.
fixtures=()
for d in tests/fixtures/*/; do
    d="${d%/}"
    [ -f "$d/input.tur" ] && [ -f "$d/expected.stdout" ] || continue
    case "$(basename "$d")" in
        r7rs-*) fixtures+=("$d") ;;
        *) IFS= read -r first < "$d/input.tur"
           [[ "$first" == "#lang r7rs"* ]] && fixtures+=("$d") ;;
    esac
done

one() {
    local name; name="$(basename "$1")"
    one_case "$1" > "$WORK/$name.result" 2>&1
}
one_case() {
    local dir="$1" name rc want flags args=()
    name="$(basename "$dir")"
    flags=""; [ -f "$dir/flags" ] && flags="$(cat "$dir/flags")"
    # Not `mapfile`: macOS ships bash 3.2, which has no such builtin, so the
    # fixture ran with no arguments there (the read loop run-jit.sh uses).
    if [ -f "$dir/run.args" ]; then
        local _ra
        while IFS= read -r _ra; do
            [ -z "$_ra" ] && continue
            args+=("$_ra")
        done < "$dir/run.args"
    fi
    local stdin=/dev/null; [ -f "$dir/input.stdin" ] && stdin="$dir/input.stdin"
    # shellcheck disable=SC2086
    if ! TUR_CC_FLAGS="$CC_FLAGS" CC="$BUILD_CC" timeout 600 "$TUR" $flags build \
            "$dir/input.tur" -o "$WORK/$name" > "$WORK/$name.build" 2>&1; then
        echo "FAIL $name -- build failed: $(grep -m1 -i error "$WORK/$name.build" | cut -c1-160)"
        return
    fi
    # A fixture that is about the collector under contention asks for
    # frequent collections here too (sanitize.torture: the interval).
    local torture=""; [ -f "$dir/sanitize.torture" ] && torture="$(tr -d '[:space:]' < "$dir/sanitize.torture")"
    (cd "$dir" && ASAN_OPTIONS="detect_leaks=0:halt_on_error=1" \
        UBSAN_OPTIONS="print_stacktrace=1:halt_on_error=1" \
        env ${torture:+TUR_GC_TORTURE="$torture"} \
        timeout 120 "$WORK/$name" "${args[@]}" < "$stdin" \
        > "$WORK/$name.out" 2> "$WORK/$name.err") 2> /dev/null
    rc=$?
    want=0; [ -f "$dir/expected.exit" ] && want="$(tr -d '[:space:]' < "$dir/expected.exit")"
    if grep -q "ERROR: AddressSanitizer" "$WORK/$name.err"; then
        echo "FAIL $name -- $(grep -m1 'ERROR: AddressSanitizer' "$WORK/$name.err" | sed 's/.*AddressSanitizer: //' | cut -c1-120)"
        grep -m6 '^    #' "$WORK/$name.err" | sed 's/^/    /'
    elif grep -q "runtime error:" "$WORK/$name.err"; then
        echo "FAIL $name -- UBSan: $(grep -m1 'runtime error:' "$WORK/$name.err" | cut -c1-160)"
    elif [ "$rc" = 124 ]; then
        echo "FAIL $name -- timed out (>120s)"
    elif { [ "$want" = nonzero ] && [ "$rc" = 0 ]; } || { [ "$want" != nonzero ] && [ "$rc" != "$want" ]; }; then
        echo "FAIL $name -- exit $rc, expected $want: $(tail -1 "$WORK/$name.err" | cut -c1-120)"
    elif ! diff -q "$WORK/$name.out" "$dir/expected.stdout" > /dev/null; then
        echo "FAIL $name -- stdout differs under the sanitizers"
        diff "$WORK/$name.out" "$dir/expected.stdout" | head -6 | sed 's/^/    /'
    else
        echo "PASS $name"
    fi
}
export -f one one_case
export TUR BUILD_CC CC_FLAGS WORK

printf '%s\n' "${fixtures[@]}" | xargs -P "$(nproc)" -I{} bash -c 'one "$@"' _ {}
for d in "${fixtures[@]}"; do cat "$WORK/$(basename "$d").result"; done | tee "$WORK/results"
pass=$(grep -c '^PASS' "$WORK/results")
fail=$(grep -c '^FAIL' "$WORK/results")
echo
echo "r7rs-sanitize: $pass passed, $fail failed of ${#fixtures[@]}"
[ "$fail" -eq 0 ]
