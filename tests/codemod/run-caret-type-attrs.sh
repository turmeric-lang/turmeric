#!/usr/bin/env bash
# Runner for tools/caret-type-attrs-rewrite.py against the test corpus.
#
# Each directory under tests/codemod/caret-type-attrs/ holds before.<ext> and
# after.<ext> (ext: tur, tur.sweet or md).  The runner rewrites a copy of the
# first and compares it byte for byte with the second, then runs the tool on
# the result and requires that to change nothing (idempotence).
set -u
cd "$(dirname "$0")/../.."

CORPUS_DIR="tests/codemod/caret-type-attrs"
TOOL="tools/caret-type-attrs-rewrite.py"
[ -d "$CORPUS_DIR" ] || { echo "missing $CORPUS_DIR" >&2; exit 2; }
[ -f "$TOOL" ]       || { echo "missing $TOOL" >&2; exit 2; }
command -v python3 >/dev/null 2>&1 || { echo "SKIP caret-type-attrs codemod -- python3 not available"; exit 0; }

pass=0
fail=0
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT

for dir in "$CORPUS_DIR"/*/; do
    name="$(basename "$dir")"
    before=""; after=""; ext=""
    for e in tur.sweet tur md; do
        if [ -f "$dir/before.$e" ]; then before="$dir/before.$e"; after="$dir/after.$e"; ext="$e"; fi
    done
    if [ -z "$before" ] || [ ! -f "$after" ]; then
        echo "FAIL $name -- missing before/after"; fail=$((fail + 1)); continue
    fi
    work="$tmp/$name.$ext"
    cp "$before" "$work"
    if ! python3 "$TOOL" --write "$work" >/dev/null 2>"$tmp/err"; then
        echo "FAIL $name -- rewriter exited non-zero"; cat "$tmp/err"
        fail=$((fail + 1)); continue
    fi
    if ! diff -u "$after" "$work"; then
        echo "FAIL $name -- output diverges from after.$ext"; fail=$((fail + 1)); continue
    fi
    if ! python3 "$TOOL" --check "$work" >/dev/null 2>&1; then
        echo "FAIL $name -- a second run changed the output (not idempotent)"
        fail=$((fail + 1)); continue
    fi
    echo "PASS $name"; pass=$((pass + 1))
done

echo "caret-type-attrs codemod: $pass passed, $fail failed"
[ "$fail" -eq 0 ]
