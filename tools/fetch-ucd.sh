#!/usr/bin/env bash
# tools/fetch-ucd.sh -- the Unicode Character Database files behind
# `(scheme char)` for `#lang r7rs` (tools/gen-r7rs-unicode.py --ucd).
#
# Pinned to one ICU release tag, so a regeneration is reproducible and the
# Unicode version the tables carry moves only when someone changes UCD_TAG
# here.  ICU ships the UCD files under icu4c/source/data/unidata/; unicode.org
# itself is not reachable from every build box (a proxy that allows GitHub
# and nothing else), which is why the source is ICU's copy.
#
#   bash tools/fetch-ucd.sh [dir]        # default dir: build/ucd
#   python3 tools/gen-r7rs-unicode.py --ucd build/ucd
#   python3 tools/gen-md-slug-data.py --ucd build/ucd   # stdlib/markdown/slug-data.tur
#
# ICU release-76-1 carries Unicode 16.0.0 (release-74-2: 15.1.0; main: newer).
set -euo pipefail
UCD_TAG="${UCD_TAG:-release-76-1}"
DIR="${1:-build/ucd}"
BASE="https://raw.githubusercontent.com/unicode-org/icu/${UCD_TAG}/icu4c/source/data/unidata"
mkdir -p "$DIR"
for f in UnicodeData SpecialCasing CaseFolding DerivedCoreProperties; do
    curl -sSf -o "$DIR/$f.txt" "$BASE/$f.txt"
    echo "fetched $DIR/$f.txt ($(head -1 "$DIR/$f.txt" | sed 's/^# //'))"
done
echo "$UCD_TAG" > "$DIR/ICU_TAG"
