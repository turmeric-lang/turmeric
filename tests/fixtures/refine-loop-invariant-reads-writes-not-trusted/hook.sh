#!/usr/bin/env bash
set -e
FIXTURE_DIR="$(cd "$(dirname "$0")" && pwd)"
c="$1/out.c"
"$TUR" emit-c "$FIXTURE_DIR/input.tur" > "$c" 2> "$1/emit.err"
# Preservation is proved only in `clean`; the other six keep their check.
n=$(grep -c "Loop invariant not re-established" "$c" || true)
[ "$n" = "6" ] || { echo "expected 6 kept re-establishment checks, got $n" >&2; exit 1; }
# The two definite writers are reported, and nothing else is.
w=$(grep -c "TUR-W0383" "$1/emit.err" || true)
[ "$w" = "2" ] || { echo "expected 2 TUR-W0383, got $w" >&2; cat "$1/emit.err" >&2; exit 1; }
grep -q "input.tur:33:.*TUR-W0383" "$1/emit.err" || { echo "no W0383 on liar" >&2; exit 1; }
grep -q "input.tur:39:.*TUR-W0383" "$1/emit.err" || { echo "no W0383 on liar2" >&2; exit 1; }
TUR_REFINE_STATS=1 "$TUR" check "$FIXTURE_DIR/input.tur" \
    2> "$1/stats.err" > /dev/null
grep -q "refine: invariant: 8 proven, 6 unproven, 0 loop(s) declined" "$1/stats.err" || {
    echo "stats line missing:" >&2; cat "$1/stats.err" >&2; exit 1; }
"$TUR" run "$FIXTURE_DIR/input.tur" 2> /dev/null
