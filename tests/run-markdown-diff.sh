#!/usr/bin/env bash
# tests/run-markdown-diff.sh -- tur/markdown's HTML against the reference
# implementations', over every Markdown file in the repository and seeded
# random documents.  ctest tur_markdown_diff.
#
#   - Plain CommonMark against commonmark.js 0.31.2 (markdown-library-plan
#     M2).  Needs node, and commonmark.js installed from the pinned lockfile:
#       (cd tests/markdown-spec/oracle && npm ci)
#   - The GFM extensions against cmark-gfm (M3).  Needs cmake, and a
#     github/cmark-gfm checkout at ../cmark-gfm, at the commit
#     tests/markdown-spec/oracle/cmark-gfm.pin names.
#
# Each comparison PASS-skips without its reference.  CI's aux part sets up
# both.  Arguments go to tests/markdown-spec/diff.py (--seeds, --docs,
# --no-corpus, --show, --cmark-gfm).
set -euo pipefail
cd "$(dirname "$0")/.."
exec python3 tests/markdown-spec/diff.py "$@"
