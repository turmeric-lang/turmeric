#!/usr/bin/env bash
# check-forms-documented.sh -- every special form and compiler builtin is
# documented in stdlib/forms/, or listed in tests/forms-undocumented.txt.
#
# Wraps tests/tools/check_forms_documented.py; skips cleanly without python3.
if ! command -v python3 >/dev/null 2>&1; then
    echo "SKIP check-forms-documented -- python3 not available"
    exit 0
fi
cd "$(dirname "$0")/.."
exec python3 tests/tools/check_forms_documented.py
