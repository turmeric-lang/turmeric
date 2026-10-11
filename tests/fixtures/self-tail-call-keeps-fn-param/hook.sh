#!/usr/bin/env bash
# self-tco-refuses-fn-typed-params: build the program UNOPTIMIZED -- the
# harness's own flags with -O0 last, so no sibling-call optimization can stand
# in for the backedge -- and run it.  run.sh compares its stdout.
set -e
TMPDIR="$1"
FIXTURE_DIR="$(cd "$(dirname "$0")" && pwd)"
CC="$CC" TUR_CC_FLAGS="$TUR_CC_FLAGS -O0" "$TUR" build "$FIXTURE_DIR/input.tur" -o "$TMPDIR/exe" 2>/dev/null
"$TMPDIR/exe"
