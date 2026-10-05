#!/usr/bin/env bash
#
# Append suite timing rows -- and optionally a repo line-count row -- to the
# `ci-metrics` orphan branch.
#
# Phase 3A of docs/archive/suite-timing-trends-plan.md.  The branch carries no
# source -- just year-partitioned JSONL -- so it never builds and never merges.
# CI does not run on it: .github/workflows/ci.yml triggers only on `main`, and
# the commit message additionally carries [skip ci].
#
# Usage: publish-timings.sh timings.jsonl [--loc loc.jsonl] [--docs docs.jsonl]
#
# --loc and --docs go in the SAME commit rather than through a second
# invocation.  Two pushes to one branch from one job is two chances to be
# rejected and two retry loops racing each other for the tip; one commit
# carrying all files cannot half-land.
#
# Deliberate deviation from the plan: it specified a force-push.  A force-push
# can silently discard rows another run appended between our fetch and our push,
# which is the one outcome an append-only metrics log must never have.  The
# concurrency group in the workflow already serializes publishers, so a plain
# push should succeed; on the rare rejection we re-fetch, re-append onto the new
# tip, and retry.  Nothing is ever overwritten.
set -euo pipefail

BRANCH="${TIMINGS_BRANCH:-ci-metrics}"
ATTEMPTS="${TIMINGS_PUSH_ATTEMPTS:-5}"
YEAR="$(date -u +%Y)"

INPUT=""
LOC_INPUT=""
DOCS_INPUT=""
while [ $# -gt 0 ]; do
    case "$1" in
        --loc)
            LOC_INPUT="${2:?--loc needs a file}"
            shift 2
            ;;
        --docs)
            DOCS_INPUT="${2:?--docs needs a file}"
            shift 2
            ;;
        -*)
            echo "publish-timings: unknown option $1" >&2
            exit 2
            ;;
        *)
            INPUT="$1"
            shift
            ;;
    esac
done

: "${INPUT:?usage: publish-timings.sh <timings.jsonl> [--loc <loc.jsonl>] [--docs <docs.jsonl>]}"

abspath() { echo "$(cd "$(dirname "$1")" && pwd)/$(basename "$1")"; }

if [ ! -s "$INPUT" ]; then
    echo "publish-timings: $INPUT is missing or empty; nothing to publish" >&2
    exit 0
fi

INPUT_ABS="$(abspath "$INPUT")"
FILE="suite-timings-${YEAR}.jsonl"
ROWS="$(wc -l < "$INPUT_ABS" | tr -d ' ')"

# A missing or empty --loc is a warning, not a failure: the timings are the
# reason this job exists and must publish without the line counts.
LOC_FILE="repo-loc-${YEAR}.jsonl"
LOC_ABS=""
LOC_ROWS=0
if [ -n "$LOC_INPUT" ]; then
    if [ -s "$LOC_INPUT" ]; then
        LOC_ABS="$(abspath "$LOC_INPUT")"
        LOC_ROWS="$(wc -l < "$LOC_ABS" | tr -d ' ')"
    else
        echo "publish-timings: $LOC_INPUT is missing or empty; skipping line counts" >&2
    fi
fi

# Same reasoning for --docs: a missing file costs one panel, not the page.
DOCS_FILE="docs-counts-${YEAR}.jsonl"
DOCS_ABS=""
DOCS_ROWS=0
if [ -n "$DOCS_INPUT" ]; then
    if [ -s "$DOCS_INPUT" ]; then
        DOCS_ABS="$(abspath "$DOCS_INPUT")"
        DOCS_ROWS="$(wc -l < "$DOCS_ABS" | tr -d ' ')"
    else
        echo "publish-timings: $DOCS_INPUT is missing or empty; skipping doc counts" >&2
    fi
fi

if [ -z "$(git config user.email || true)" ]; then
    git config user.email "github-actions[bot]@users.noreply.github.com"
    git config user.name  "github-actions[bot]"
fi

WT="$(mktemp -d)"
cleanup() {
    git worktree remove --force "$WT" >/dev/null 2>&1 || true
    rm -rf "$WT"
}
trap cleanup EXIT

attempt=1
while [ "$attempt" -le "$ATTEMPTS" ]; do
    rm -rf "$WT"

    if git ls-remote --exit-code --heads origin "$BRANCH" >/dev/null 2>&1; then
        # Existing branch: check it out into a throwaway worktree. Full history
        # (no --depth) so the push is a fast-forward rather than a shallow-push
        # rejection.
        git fetch --force origin "$BRANCH:refs/remotes/origin/$BRANCH"
        git worktree add -f -B "$BRANCH" "$WT" "origin/$BRANCH" >/dev/null
    else
        # First ever run: create the orphan with no parent and no source files.
        git worktree add -f --detach "$WT" HEAD >/dev/null
        (
            cd "$WT"
            git checkout --orphan "$BRANCH" >/dev/null 2>&1
            git rm -rf --cached . >/dev/null 2>&1 || true
            find . -mindepth 1 -maxdepth 1 -not -name '.git' -exec rm -rf {} +
            cat > README.md <<'EOF'
# ci-metrics

Data-only branch. No source, no build, no CI.

`suite-timings-<year>.jsonl` holds one JSON object per CTest suite per CI run.
`repo-loc-<year>.jsonl` holds one object per CI run with the tracked line counts
of the commit, split product / test / bench / example / generated.
`docs-counts-<year>.jsonl` holds one object per CI run with the counts of open
plans (docs/upcoming/) and reports (docs/reported/). All three are appended by
`tools/ci/publish-timings.sh` on pushes to `main`. See
`docs/archive/suite-timing-trends-plan.md` on `main` for the timings schema and
the reason timings are only comparable within a fixed
(build_type, os, cc, nproc, jit) tuple; line counts carry no such dimension,
since they are a property of the commit rather than of the runner.

Never merge this branch into `main`.
EOF
        )
    fi

    cat "$INPUT_ABS" >> "$WT/$FILE"
    [ -n "$LOC_ABS" ] && cat "$LOC_ABS" >> "$WT/$LOC_FILE"
    [ -n "$DOCS_ABS" ] && cat "$DOCS_ABS" >> "$WT/$DOCS_FILE"

    (
        cd "$WT"
        git add "$FILE" README.md 2>/dev/null || git add "$FILE"
        [ -n "$LOC_ABS" ] && git add "$LOC_FILE"
        [ -n "$DOCS_ABS" ] && git add "$DOCS_FILE"
        if git diff --cached --quiet; then
            echo "publish-timings: no change to commit" >&2
            exit 0
        fi
        # [skip ci] is the second layer of the "this branch never builds"
        # guarantee; the workflow's own `branches: [main]` filter is the first.
        git commit -q -m "ci-metrics: ${ROWS} suite rows, ${LOC_ROWS} loc row(s), ${DOCS_ROWS} docs row(s) from ${GITHUB_SHA:-local} [skip ci]"
    )

    if git -C "$WT" push origin "$BRANCH"; then
        echo "publish-timings: appended ${ROWS} rows to ${BRANCH}/${FILE}" >&2
        [ -n "$LOC_ABS" ] \
            && echo "publish-timings: appended ${LOC_ROWS} rows to ${BRANCH}/${LOC_FILE}" >&2
        [ -n "$DOCS_ABS" ] \
            && echo "publish-timings: appended ${DOCS_ROWS} rows to ${BRANCH}/${DOCS_FILE}" >&2
        exit 0
    fi

    echo "publish-timings: push rejected (attempt ${attempt}/${ATTEMPTS}); re-fetching and retrying" >&2
    attempt=$((attempt + 1))
    sleep $(( attempt * 2 ))
done

echo "publish-timings: giving up after ${ATTEMPTS} attempts" >&2
exit 1
