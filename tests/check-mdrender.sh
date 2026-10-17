#!/usr/bin/env bash
# check-mdrender.sh -- tools/mdrender.py and tools/md/render.tur, the Markdown
# renderer genguides.py and genspices.py build the docs site with
# (markdown-library-plan M4).  ctest tur_md_render.
#
# Usage: tests/check-mdrender.sh [TUR]
#
# Wraps tests/tools/check_mdrender.py, which builds the renderer with TUR
# (default $TUR, then ./build/tur) and checks the HTML, headings and raw-HTML
# report it gives back.  Skips cleanly without python3.
set -u
cd "$(dirname "$0")/.."
if ! command -v python3 >/dev/null 2>&1; then
    echo "SKIP check-mdrender -- python3 not available"
    exit 0
fi
if [ $# -ge 1 ]; then
    export TUR="$1"
fi
exec python3 tests/tools/check_mdrender.py
