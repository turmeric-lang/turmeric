#!/usr/bin/env bash
set -e
FIXTURE_DIR="$(cd "$(dirname "$0")" && pwd)"
c="$1/out.c"
"$TUR" --enable=loop-invariants emit-c "$FIXTURE_DIR/input.tur" > "$c" 2> "$1/emit.err"
# Both obligations proved, so neither runtime check survives.
for msg in "Loop invariant failed on entry" "Loop invariant not re-established"; do
    if grep -q "$msg" "$c"; then
        echo "a proved check was emitted: $msg" >&2
        exit 1
    fi
done
TUR_REFINE_STATS=1 "$TUR" --enable=loop-invariants check "$FIXTURE_DIR/input.tur" \
    2> "$1/stats.err" > /dev/null
grep -q "refine: invariant: 4 proven, 0 unproven, 0 loop(s) declined" "$1/stats.err" || {
    echo "stats line missing:" >&2; cat "$1/stats.err" >&2; exit 1; }
# run.sh compares THIS script's stdout to expected.stdout.
"$TUR" --enable=loop-invariants run "$FIXTURE_DIR/input.tur" 2> /dev/null
