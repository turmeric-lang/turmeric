#!/usr/bin/env bash
# tests/lsp/run-cross-file-diagnostics.sh -- does the LSP get files other
# than the open document right (loaded/imported diagnostics, sibling imports)?
#
# Wrapper around cross-file-diagnostics.py, in the shape run-stdio-smoke.sh uses:
# skip cleanly (exit 0) when python3 or the binary is missing, so a machine
# without them does not fail the suite.
set -u
cd "$(dirname "$0")/../.."

if ! command -v python3 >/dev/null 2>&1; then
    echo "SKIP: python3 not available -- LSP cross-file diagnostics test skipped"
    echo "TUR_SKIP: python3 unavailable"
    exit 0
fi

TUR="${TUR:-./build/tur}"
[ -x "$TUR" ] || [ ! -x "${TUR}.exe" ] || TUR="${TUR}.exe"
if [ ! -x "$TUR" ]; then
    echo "SKIP: $TUR not built" >&2
    echo "TUR_SKIP: $TUR not built"
    exit 0
fi

exec python3 tests/lsp/cross-file-diagnostics.py "$TUR"
