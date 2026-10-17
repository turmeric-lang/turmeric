#!/usr/bin/env bash
# tests/check-guide-pairs-tool.sh -- tools/check-guide-pairs.tur reports what
# it should.
#
# tests/guide-pairs/sample.md seeds one outcome per check: a pair that agrees
# and one that does not, an empty side, a non-ASCII side, an unpaired block
# and a no-check one, pairs in a list item and a block quote (which the
# CommonMark parse finds), manifests that parse, do not and opt out, a
# ```turmeric line inside another fence (which is no block), and an
# early-closing fence.  The report, scratch paths and the tur path aside, must
# be tests/guide-pairs/expected.txt, and the exit status 1.  Then the exit
# statuses: 0 for a clean file, 1 for --require-pairs with no pair, 2 for a
# --tur that does not exist.
#
# Usage: bash tests/check-guide-pairs-tool.sh [path/to/tur]
set -u
cd "$(dirname "$0")/.."

TUR="${1:-${TUR:-./build/tur}}"
[ -x "$TUR" ] || { echo "tests/check-guide-pairs-tool.sh: $TUR not built" >&2; exit 2; }

PASS=0
FAIL=0
pass() { PASS=$((PASS+1)); echo "PASS $1"; }
fail() { FAIL=$((FAIL+1)); echo "FAIL $1: $2"; }

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

# `tur run` is how CI and the Justfile run it; build once and reuse the binary
# for the exit-status cases.
if ! "$TUR" build tools/check-guide-pairs.tur -o "$WORK/cgp" > "$WORK/build.log" 2>&1; then
  cat "$WORK/build.log"
  echo "FAIL build: tools/check-guide-pairs.tur does not compile"
  exit 1
fi

NAME="sample-report"
# The report is stdout.  stderr is kept apart: `tur run` passes on the C
# compiler's warnings about the generated code there, and clang (macOS's cc)
# has some that gcc does not.
"$TUR" run tools/check-guide-pairs.tur -- --tur "$TUR" --strict-unpaired --verbose \
  tests/guide-pairs/sample.md > "$WORK/out.txt" 2> "$WORK/err.txt"
RC=$?
sed -E -e 's#[^ ]*/tur-guide-pairs-[0-9]+/#<scratch>/#g' \
       -e 's#^(Checker +: ).* \(#\1<tur> (#' "$WORK/out.txt" > "$WORK/got.txt"
if [ "$RC" -ne 1 ]; then
  cat "$WORK/err.txt"
  fail "$NAME" "exit $RC, expected 1"
elif ! diff -u tests/guide-pairs/expected.txt "$WORK/got.txt"; then
  cat "$WORK/err.txt"
  fail "$NAME" "report differs from tests/guide-pairs/expected.txt"
else
  pass "$NAME"
fi

NAME="clean-file-exits-0"
printf '# t\n\n```turmeric\n(+ 1 2)\n```\n```sweet-exp\n+(1 2)\n```\n' > "$WORK/clean.md"
"$WORK/cgp" --tur "$TUR" --strict-unpaired "$WORK/clean.md" > "$WORK/clean.txt" 2>&1
RC=$?
if [ "$RC" -eq 0 ] && grep -q '^Pairs ok         : 1$' "$WORK/clean.txt"; then
  pass "$NAME"
else
  fail "$NAME" "exit $RC: $(tr '\n' ' ' < "$WORK/clean.txt")"
fi

NAME="require-pairs-without-pairs-exits-1"
printf '# t\n\nNo code here.\n' > "$WORK/none.md"
"$WORK/cgp" --tur "$TUR" --require-pairs "$WORK/none.md" > "$WORK/none.txt" 2>&1
RC=$?
if [ "$RC" -eq 1 ] && grep -q 'error: no pairs found' "$WORK/none.txt"; then
  pass "$NAME"
else
  fail "$NAME" "exit $RC: $(tr '\n' ' ' < "$WORK/none.txt")"
fi

NAME="missing-tur-exits-2"
"$WORK/cgp" --tur "$WORK/no/such/tur" "$WORK/clean.md" > "$WORK/missing.txt" 2>&1
RC=$?
if [ "$RC" -eq 2 ] && grep -q 'no tur at' "$WORK/missing.txt"; then
  pass "$NAME"
else
  fail "$NAME" "exit $RC: $(tr '\n' ' ' < "$WORK/missing.txt")"
fi

echo "summary: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
