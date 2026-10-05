#!/usr/bin/env bash
# tests/check-r7rs-tail-calls-small-stack.sh --
# docs/archive/r7rs-tail-call-through-static-call-not-proper.md: tail calls
# that mix procedure values and top-level procedures stay proper when
# compiled.  A compiled `#lang r7rs` program runs on a 1 GiB stack (its main
# moves to a big-stack thread), which would hide a tail call that grows the
# stack, so this runs the fixture with that move off (TUR_NO_DEEP_STACK=1)
# in an 8 MiB stack.  Linux and macOS.
set -uo pipefail
cd "$(dirname "$0")/.."
TUR_REL="${TUR:-./build/tur}"
TUR="$(cd "$(dirname "$TUR_REL")" && pwd)/$(basename "$TUR_REL")"
if [ ! -x "$TUR" ]; then
    echo "check-r7rs-tail-calls-small-stack: $TUR not built" >&2
    exit 2
fi
case "$(uname -s)" in
    Linux|Darwin) ;;
    *) echo "SKIP check-r7rs-tail-calls-small-stack: Linux and macOS only"; exit 0 ;;
esac
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

FIX=tests/fixtures/r7rs-tail-call-hand-on-through-static-call
if ! "$TUR" build "$FIX/input.tur" -o "$TMP/tail" > "$TMP/tail.build" 2>&1; then
    echo "FAIL check-r7rs-tail-calls-small-stack: tur build failed: $(grep -m1 -i error "$TMP/tail.build")"
    exit 1
fi
out="$( (ulimit -s 8192 2>/dev/null; TUR_NO_DEEP_STACK=1 "$TMP/tail") 2>"$TMP/tail.err")"; rc=$?
if [ "$rc" != 0 ] || [ "$out" != "$(cat "$FIX/expected.stdout")" ]; then
    echo "FAIL check-r7rs-tail-calls-small-stack: exited $rc in an 8 MiB stack: $(head -2 "$TMP/tail.err")"
    exit 1
fi
echo "PASS check-r7rs-tail-calls-small-stack: tail calls through procedure values run in an 8 MiB stack"
