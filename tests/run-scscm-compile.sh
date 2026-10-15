#!/usr/bin/env bash
# tests/run-scscm-compile.sh -- build + run the scscm-compile fixture project.
#
# Builds tests/fixtures/scscm-compile as a tur build project (it imports the
# scscm spice via :path), runs the resulting binary, and diffs stdout against
# expected.stdout.  Skips with PASS when ../turmeric-spices is absent.
#
# Generated .c/.h files from the fixture land in $WORK (temp dir, cleaned up
# by EXIT trap); those from the :path dep land in ../turmeric-spices/spices/
# scscm/ as usual.

set -uo pipefail
cd "$(dirname "$0")/.."
REPO="$PWD"

TUR="$REPO/build/tur"
FIXTURE="$REPO/tests/fixtures/scscm-compile"
EXPECTED="$FIXTURE/expected.stdout"
SPICE_DIR="$REPO/../turmeric-spices/spices/scscm"

PASS=0
FAIL=0
pass() { PASS=$((PASS + 1)); echo "PASS $1"; }
fail() { FAIL=$((FAIL + 1)); echo "FAIL $1 -- $2"; }

if [ ! -x "$TUR" ]; then
    echo "tests: $TUR not built; run 'tur run build' first" >&2
    exit 2
fi

# Skip when the sibling turmeric-spices repo is absent.
if [ ! -d "$SPICE_DIR" ]; then
    echo "SKIP scscm-compile (turmeric-spices absent)"
    echo "TUR_SKIP: sibling ../turmeric-spices checkout absent"
    echo
    echo "summary: $PASS passed, $FAIL failed"
    exit 0
fi

# Compiler-path leaks are not in scope for this fixture test.
export ASAN_OPTIONS="${ASAN_OPTIONS:-detect_leaks=0}"

WORK="$(mktemp -d -t tur-scscm-compile.XXXXXX)"
trap 'rm -rf "$WORK"' EXIT

BIN="$WORK/scscm-compile-test"

# Run from $WORK so generated .c/.h files land there (not in the repo).
# The project root stays $FIXTURE so the :path dep resolves correctly
# relative to build.tur's location.
build_out=$(cd "$WORK" && "$TUR" build "$FIXTURE" -o "$BIN" 2>&1)
build_rc=$?
if [ $build_rc -ne 0 ]; then
    fail "scscm-compile-build" "tur build exit=$build_rc: $build_out"
    echo
    echo "summary: $PASS passed, $FAIL failed"
    exit 1
fi
pass "scscm-compile-build"

actual_stdout="$WORK/actual.stdout"
"$BIN" > "$actual_stdout" 2>/dev/null

if diff -u "$EXPECTED" "$actual_stdout" > /dev/null 2>&1; then
    pass "scscm-compile-stdout"
else
    diff_out="$(diff -u "$EXPECTED" "$actual_stdout" 2>&1)"
    fail "scscm-compile-stdout" "$diff_out"
fi

echo
echo "summary: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
