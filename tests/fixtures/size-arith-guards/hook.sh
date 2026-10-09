#!/usr/bin/env bash
# Build prog.tur once, then run each case in its own process and print its
# stdout, its exit status and the first line of its stderr.  Most cases end
# the program on purpose; the status and the message are what they pin.
set -e
TMPDIR="$1"
FIXTURE_DIR="$(cd "$(dirname "$0")" && pwd)"
CC="$CC" "$TUR" build "$FIXTURE_DIR/prog.tur" -o "$TMPDIR/prog" > "$TMPDIR/build.log" 2>&1 || {
    cat "$TMPDIR/build.log" >&2
    exit 1
}
for c in 0 1 2 3 4 5 6 7 8 9; do
    echo "== case $c"
    set +e
    echo "piped" | ASAN_OPTIONS=detect_leaks=0 "$TMPDIR/prog" "$c" 2> "$TMPDIR/err"
    rc=$?
    set -e
    echo "exit=$rc"
    # An index out of bounds is a panic now: its line names the runtime's own
    # file and line (not portable), so keep the message after that prefix.
    head -n 1 "$TMPDIR/err" | sed -E 's/^panic at [^ ]*:[0-9]+: //'
done
