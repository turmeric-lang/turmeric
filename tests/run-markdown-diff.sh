#!/usr/bin/env bash
# tests/run-markdown-diff.sh -- tur/markdown's HTML against commonmark.js's,
# over every Markdown file in the repository and seeded random documents
# (markdown-library-plan M2).  ctest tur_markdown_diff.
#
# Needs node, and commonmark.js installed from the pinned lockfile:
#
#   (cd tests/markdown-spec/oracle && npm ci)
#
# Without either the run PASS-skips.  CI's aux part installs both.  Arguments
# go to tests/markdown-spec/diff.py (--seeds, --docs, --no-corpus, --show).
set -euo pipefail
cd "$(dirname "$0")/.."
exec python3 tests/markdown-spec/diff.py "$@"
