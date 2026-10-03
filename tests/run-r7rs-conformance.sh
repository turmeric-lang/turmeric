#!/usr/bin/env bash
# tests/run-r7rs-conformance.sh -- r7rs-lang-plan R10: chibi-scheme's R7RS
# test suite (tests/r7rs/chibi-r7rs-tests.scm, BSD, see CHIBI-COPYING), run
# through tests/r7rs/run-conformance.py, which REPORTS A COUNT.
#
# The count is the point: it moves as stages land instead of arriving as a
# verdict at the end.  This target fails only on a REGRESSION -- fewer passes
# than the floor below, or a SETTLED test (a difference kept on purpose, see
# run-conformance.py) that now passes and should be deleted from the table.
# Raise the floor when the count goes up.
#
#   R7RS_CONFORMANCE_BACKEND  both (default) | interp | compiled
#                             The whole suite as one program per round: a few
#                             seconds on the interpreter, and one large build
#                             on the compiled back end (see the timeout below).
#   R7RS_CONFORMANCE_FLOOR    override the floor (applied to each back end).
#   R7RS_CONFORMANCE_TIMEOUT  seconds per program run (default 480).
#
# Skips cleanly (exit 0) without python3 or the built binary.

set -uo pipefail
cd "$(dirname "$0")/.."
ROOT="$PWD"

TUR="${TUR:-$ROOT/build/tur}"
[ -x "$TUR" ] || [ ! -x "${TUR}.exe" ] || TUR="${TUR}.exe"
if [ ! -x "$TUR" ]; then
    echo "SKIP run-r7rs-conformance: $TUR not built"
    exit 0
fi
if ! command -v python3 >/dev/null 2>&1; then
    echo "SKIP run-r7rs-conformance: python3 not found"
    exit 0
fi

# The floor: the count recorded in the plan (R10, raised by Section 9 tasks), of
# 1216 tests written in the suite -- the same on both back ends.
FLOOR="${R7RS_CONFORMANCE_FLOOR:-1223}"
BACKEND="${R7RS_CONFORMANCE_BACKEND:-both}"

# The per-program timeout.  The harness's own default (240 s) is sized for a
# small program; this one is not.  The compiled round builds the whole suite as
# one program -- ~94 KB of Scheme, ~5.6 MB of emitted C -- and under the
# sanitized Debug `tur` that is ~95 s of emit-c and ~150 s of single-threaded
# `cc -O2` on an idle 4-core box, 248 s end to end.  In CI it shares the
# runner's cores with the other r7rs suites (it is not RUN_SERIAL), and on the
# slower ubuntu runners it ran 187-247 s: three main runs on 09-30 died at the
# 240 s cap with "round 1: timeout -- 1181 form(s) not run" and 0 passed.
# 480 s leaves room for that, and one round plus the interpreter pass still
# fits well inside ctest's TIMEOUT 720 (CMakeLists.txt), which stays the guard
# against a real hang.  A second compiled round would not fit, but one only
# happens after a form crashes the program, and with the floor at today's full
# pass count such a run is almost certainly below it already.  Why the build
# was this large, and how it came down to ~43 s on that box (2026-10-03, so
# 480 is now headroom, not a requirement):
# docs/archive/r7rs-conformance-program-emits-megabytes-of-c.md.
TIMEOUT="${R7RS_CONFORMANCE_TIMEOUT:-480}"

exec python3 tests/r7rs/run-conformance.py --tur "$TUR" --backend "$BACKEND" \
    --min-pass "$FLOOR" --timeout "$TIMEOUT"
