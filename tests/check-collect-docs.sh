#!/usr/bin/env bash
# check-collect-docs.sh -- tools/ci/collect-docs.py must count a report as
# archived however it left docs/reported/, a plain `git mv` to docs/archive/
# included, so /ci's "reports archived minus filed" chart is right.
# Wraps tests/tools/check_collect_docs.py; skips cleanly without python3.
set -u
cd "$(dirname "$0")/.."
if ! command -v python3 >/dev/null 2>&1; then
    echo "SKIP check-collect-docs -- python3 not available"
    exit 0
fi
exec python3 tests/tools/check_collect_docs.py
