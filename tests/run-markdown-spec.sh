#!/usr/bin/env bash
# tests/run-markdown-spec.sh -- the CommonMark and GFM spec examples through
# tur/markdown (markdown-library-plan 6.1).  ctest tur_markdown_spec.
#
# The examples are in a sibling checkout of turmeric-lang/commonmark-spec,
# ../commonmark-spec (MD_SPEC_DIR overrides it), kept out of this repository
# because they are CC-BY-SA.  When it is absent this PASS-skips and says how to
# get it, the way requires.spices fixtures do.  CI fetches the commit in
# tests/markdown-spec/PINNED; a checkout at another commit gets a warning.
#
# tests/markdown-spec/run.py does the work: every example runs compiled and
# under --interpret (MD_SPEC_ENGINE: compiled, interpret, jit, both or all;
# default both), and each example tests/markdown-spec/ratchet.txt lists must
# pass on every engine.  Extra arguments go to run.py: --show N prints the
# first N failures, --only c1,c2 narrows the run, --update adds newly passing
# examples to the ratchet.
set -u
cd "$(dirname "$0")/.."

SPEC="${MD_SPEC_DIR:-../commonmark-spec}"
TUR="${TUR:-./build/tur}"
PINNED="$(sed -n '1s/^\([0-9a-f]\{40\}\).*/\1/p' tests/markdown-spec/PINNED)"

if [ ! -f "$SPEC/commonmark/0.31.2/spec.json" ]; then
    echo "SKIP tur_markdown_spec: no spec examples at $SPEC.  To run them:"
    echo "  git clone https://github.com/turmeric-lang/commonmark-spec $SPEC"
    echo "  git -C $SPEC checkout $PINNED"
    exit 0
fi

HAVE="$(git -C "$SPEC" rev-parse HEAD 2>/dev/null || echo unknown)"
if [ "$HAVE" != "$PINNED" ]; then
    echo "WARNING: $SPEC is at $HAVE, not the pinned $PINNED (tests/markdown-spec/PINNED)"
fi

exec python3 tests/markdown-spec/run.py --tur "$TUR" --spec "$SPEC" \
    --engine "${MD_SPEC_ENGINE:-both}" "$@"
