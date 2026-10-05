#!/usr/bin/env bash
# check-running-tests-guide.sh -- docs/guides/running-tests-guide.md names
# every TUR_*/TURI_* variable the three fixture harnesses read with a default.
#
# A harness variable nobody can find is a variable nobody uses -- or worse, one
# someone guesses at: an unknown name is silently ignored, so a guessed filter
# runs the whole suite.  The check reads `${VAR:-...}` / `${VAR:=...}` in
# tests/run.sh, tests/run-turi.sh and tests/run-jit.sh, and fails for any such
# variable the guide does not mention.  Part of tests/check-docs-lint.sh.
set -u
cd "$(dirname "$0")/.."

guide=docs/guides/running-tests-guide.md
harnesses=(tests/run.sh tests/run-turi.sh tests/run-jit.sh)

if [ ! -f "$guide" ]; then
    echo "FAIL check-running-tests-guide: $guide is missing"
    exit 1
fi

vars=$(grep -ohE '\$\{(TUR|TURI)_[A-Z0-9_]+:[-=]' "${harnesses[@]}" |
       sed -E 's/^\$\{//; s/:[-=]$//' | sort -u)
if [ -z "$vars" ]; then
    # A pattern that matches nothing would pass vacuously; say so instead.
    echo "FAIL check-running-tests-guide: found no harness variables -- has the spelling changed?"
    exit 1
fi

missing=0
n=0
for v in $vars; do
    n=$((n + 1))
    if ! grep -qE "\`$v[\`=]" "$guide"; then
        echo "FAIL check-running-tests-guide: $v is read by a fixture harness but not documented in $guide"
        missing=$((missing + 1))
    fi
done

if [ "$missing" -ne 0 ]; then
    echo "  Add a row for each to $guide (section 2, 3 or 4)."
    exit 1
fi
echo "PASS check-running-tests-guide ($n harness variables, all documented)"
