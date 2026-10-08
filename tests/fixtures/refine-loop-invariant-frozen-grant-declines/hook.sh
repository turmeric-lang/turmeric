#!/usr/bin/env bash
set -e
FIXTURE_DIR="$(cd "$(dirname "$0")" && pwd)"
c="$1/out.c"
"$TUR" emit-c "$FIXTURE_DIR/input.tur" > "$c" 2> "$1/emit.err"
# Preservation is unproven in BOTH loops, so both re-establishment checks stay.
n=$(grep -c "Loop invariant not re-established" "$c" || true)
[ "$n" = "2" ] || { echo "expected 2 kept re-establishment checks, got $n" >&2; exit 1; }
# Entry is proved in both, so neither entry check survives.
if grep -q "Loop invariant failed on entry" "$c"; then
    echo "a proved entry check was emitted" >&2; exit 1
fi
TUR_REFINE_STATS=1 "$TUR" check "$FIXTURE_DIR/input.tur" \
    2> "$1/stats.err" > /dev/null
grep -q "refine: invariant: 2 proven, 2 unproven, 0 loop(s) declined" "$1/stats.err" || {
    echo "stats line missing:" >&2; cat "$1/stats.err" >&2; exit 1; }
"$TUR" run "$FIXTURE_DIR/input.tur" 2> /dev/null
