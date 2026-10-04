#!/usr/bin/env bash
set -e
FIXTURE_DIR="$(cd "$(dirname "$0")" && pwd)"
"$TUR" --strict-refine run "$FIXTURE_DIR/input.tur" 2> "$1/err"
# The only line allowed is the experiment's lifecycle warning.
if grep -v "TUR-W0060" "$1/err" | grep -q .; then
    echo "a bare loop produced a diagnostic:" >&2; cat "$1/err" >&2; exit 1
fi
