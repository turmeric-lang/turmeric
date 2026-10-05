#!/usr/bin/env bash
# tests/check-r7rs-deep-recursion.sh -- docs/archive/r7rs-deep-recursion-
# segfaults-silently.md: a compiled `#lang r7rs` program's non-tail
# recursion depth is its C stack (stdlib/r7rs/stack.tur).
#
#   - Linux: a program re-executes itself once with RLIMIT_STACK raised to
#     1 GiB (or the hard limit), so SICP 1.2.1's linear recursive process
#     runs a million deep and prints its answer.  Skipped when the hard
#     limit is below 256 MiB, where there is nothing to raise it to.
#   - Linux and macOS: a recursion that runs out anyway says so on stderr,
#     "stack overflow (recursion too deep)", and still dies of SIGSEGV
#     (exit 139).  Run with TUR_STACK_REEXEC=0, so it overflows the default
#     stack at once rather than after touching a GiB.
set -uo pipefail
cd "$(dirname "$0")/.."
TUR_REL="${TUR:-./build/tur}"
TUR="$(cd "$(dirname "$TUR_REL")" && pwd)/$(basename "$TUR_REL")"
if [ ! -x "$TUR" ]; then
    echo "check-r7rs-deep-recursion: $TUR not built" >&2
    exit 2
fi
case "$(uname -s)" in
    Linux|Darwin) ;;
    *) echo "SKIP check-r7rs-deep-recursion: Linux and macOS only"; exit 0 ;;
esac
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
FAILED=0
fail() { echo "FAIL check-r7rs-deep-recursion: $*"; FAILED=1; }

printf '#lang r7rs\n(import (scheme base) (scheme write))\n(define (sum-to n) (if (= n 0) 0 (+ n (sum-to (- n 1)))))\n(write (sum-to 1000000))\n(newline)\n' > "$TMP/deep.scm"
printf '#lang r7rs\n(import (scheme base) (scheme write))\n(define (down n) (+ 1 (down (+ n 1))))\n(write (down 0))\n(newline)\n' > "$TMP/runaway.scm"
for p in deep runaway; do
    if ! "$TUR" build "$TMP/$p.scm" -o "$TMP/$p" > "$TMP/$p.build" 2>&1; then
        fail "$p: tur build failed: $(grep -m1 -i error "$TMP/$p.build")"
    fi
done

if [ "$(uname -s)" = Linux ]; then
    hard="$(ulimit -H -s)"
    if [ "$hard" != unlimited ] && [ "$hard" -lt 262144 ]; then
        echo "SKIP deep: the hard stack limit is ${hard} KiB"
    else
        out="$("$TMP/deep" 2>"$TMP/deep.err")"; rc=$?
        if [ "$rc" != 0 ] || [ "$out" != 500000500000 ]; then
            fail "a million-deep recursion printed '$out' (exit $rc), expected 500000500000: $(head -2 "$TMP/deep.err")"
        fi
    fi
fi

( TUR_STACK_REEXEC=0 "$TMP/runaway" > /dev/null 2> "$TMP/runaway.err" ) 2>/dev/null; rc=$?
if [ "$rc" != 139 ]; then
    fail "a runaway recursion exited $rc, expected 139 (SIGSEGV)"
fi
if ! grep -q "stack overflow (recursion too deep)" "$TMP/runaway.err"; then
    fail "a runaway recursion said nothing about the stack: '$(head -2 "$TMP/runaway.err")'"
fi

if [ $FAILED -ne 0 ]; then
    exit 1
fi
echo "PASS check-r7rs-deep-recursion: deep recursion runs, a runaway one says why it died"
