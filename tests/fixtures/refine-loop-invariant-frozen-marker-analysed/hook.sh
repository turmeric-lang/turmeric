#!/usr/bin/env bash
set -e
FIXTURE_DIR="$(cd "$(dirname "$0")" && pwd)"
c="$1/out.c"
"$TUR" --enable=loop-invariants emit-c "$FIXTURE_DIR/input.tur" > "$c" 2> "$1/emit.err"
# Entry is PROVED, and a `#reads` head no longer vetoes eliding its check.
if grep -q "Loop invariant failed on entry" "$c"; then
    echo "a proved entry check was emitted" >&2; exit 1
fi
# Preservation is NOT proved (a by-value mutator can grow `v`), so its check
# must survive.  Elision is per obligation, not per loop.
grep -q "Loop invariant not re-established" "$c" || {
    echo "the unproven re-establishment check was elided" >&2; exit 1; }
TUR_REFINE_STATS=1 "$TUR" --enable=loop-invariants check "$FIXTURE_DIR/input.tur" \
    2> "$1/stats.err" > /dev/null
grep -q "refine: invariant: 3 proven, 1 unproven, 0 loop(s) declined" "$1/stats.err" || {
    echo "stats line missing:" >&2; cat "$1/stats.err" >&2; exit 1; }
# run.sh compares THIS script's stdout to expected.stdout, so the program's
# own output has to come from here (same shape as loop-invariant-count-up).
"$TUR" --enable=loop-invariants run "$FIXTURE_DIR/input.tur" 2> /dev/null
