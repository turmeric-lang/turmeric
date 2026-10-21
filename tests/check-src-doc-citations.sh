#!/usr/bin/env bash
# check-src-doc-citations.sh -- every `docs/.../*.md` path a source comment
# cites must exist.
#
# Reports move from docs/reported/ to docs/archive/ (and plans from docs/ or
# docs/upcoming/ to docs/archive/), and the comments that cite them are left
# behind: a 2026-08-13 sweep repointed 255 dead paths, and the 2026-10-17
# stale-comments reports found 29 more.  A citation is the one thing in a
# comment that can be checked mechanically, so it is.
#
# A citation that wraps across a `*` continuation line is not matched and
# so not checked; that undercounts, which is fine for a ratchet.  A path
# with a `<placeholder>` in it is not a citation.
set -u
cd "$(dirname "$0")/.."

fail=0
while IFS= read -r p; do
    [ -f "$p" ] && continue
    echo "FAIL check-src-doc-citations -- $p is cited in src/ but does not exist:"
    grep -rnF "$p" src | sed 's/^/     /'
    fail=1
done < <(grep -rhoE 'docs/[A-Za-z0-9/._-]+\.md' src | sort -u)

if [ "$fail" -ne 0 ]; then
    echo "     Repoint each citation at where the file lives now (usually docs/archive/)."
    exit 1
fi
n=$(grep -rhoE 'docs/[A-Za-z0-9/._-]+\.md' src | sort -u | wc -l | tr -d ' ')
echo "PASS check-src-doc-citations ($n distinct docs paths cited in src/, all present)"
