#!/usr/bin/env bash
# Check that the GitHub Release for the current turmeric VERSION exists and
# that its build workflow finished successfully (waiting if still in flight).
#
# Usage:
#   ./scripts/wait-for-release.sh                 # uses ../turmeric/VERSION
#   TURMERIC_REPO_DIR=/path/to/turmeric ./scripts/wait-for-release.sh
#   POLL_INTERVAL=15 TIMEOUT=1800 ./scripts/wait-for-release.sh
#
# Exit codes:
#   0  release exists, workflow succeeded, assets uploaded
#   1  release missing, workflow failed/cancelled, or timeout reached
#   2  usage / environment problem (missing gh, bad VERSION, etc.)

set -euo pipefail

REPO="${TURMERIC_REPO:-turmeric-lang/turmeric}"
REPO_DIR="${TURMERIC_REPO_DIR:-.}"
WORKFLOW="${WORKFLOW_FILE:-release.yml}"
POLL_INTERVAL="${POLL_INTERVAL:-20}"
TIMEOUT="${TIMEOUT:-1800}"

die()  { printf 'error: %s\n' "$*" >&2; exit 1; }
fail() { printf 'FAIL: %s\n' "$*" >&2; exit 1; }
usage_err() { printf 'usage error: %s\n' "$*" >&2; exit 2; }

command -v gh >/dev/null || usage_err "gh CLI not found on PATH"
command -v jq >/dev/null || usage_err "jq not found on PATH"

[ -n "$REPO_DIR" ] && [ -d "$REPO_DIR" ] || usage_err "turmeric repo not found (set TURMERIC_REPO_DIR)"
[ -f "$REPO_DIR/VERSION" ] || usage_err "$REPO_DIR/VERSION does not exist"

VERSION="$(tr -d '[:space:]' < "$REPO_DIR/VERSION")"
[ -n "$VERSION" ] || usage_err "VERSION file is empty"
TAG="v$VERSION"

printf 'checking release %s for %s ...\n' "$TAG" "$REPO"

# 1. Does the workflow run for this tag exist?
RUN_JSON="$(gh run list \
  --repo "$REPO" \
  --workflow "$WORKFLOW" \
  --event push \
  --limit 50 \
  --json databaseId,headBranch,status,conclusion,url,createdAt \
  2>/dev/null || true)"

if [ -z "$RUN_JSON" ] || [ "$RUN_JSON" = "[]" ]; then
  fail "no $WORKFLOW runs found for $REPO"
fi

# headBranch for tag-push events is the tag name (e.g., "v0.14.4").
RUN_ID="$(printf '%s' "$RUN_JSON" \
  | jq -r --arg tag "$TAG" '[.[] | select(.headBranch == $tag)] | sort_by(.createdAt) | last | .databaseId // empty')"

if [ -z "$RUN_ID" ]; then
  fail "no release workflow run found for tag $TAG (was the tag pushed?)"
fi

printf 'found workflow run %s\n' "$RUN_ID"

# 2. Poll the workflow run until it leaves status=in_progress/queued.
DEADLINE=$(( $(date +%s) + TIMEOUT ))
while :; do
  STATUS_JSON="$(gh run view "$RUN_ID" --repo "$REPO" --json status,conclusion,url)"
  STATUS="$(printf '%s' "$STATUS_JSON" | jq -r .status)"
  CONCLUSION="$(printf '%s' "$STATUS_JSON" | jq -r .conclusion)"
  URL="$(printf '%s' "$STATUS_JSON" | jq -r .url)"

  case "$STATUS" in
    completed)
      if [ "$CONCLUSION" != "success" ]; then
        fail "workflow $CONCLUSION for $TAG -- $URL"
      fi
      break
      ;;
    queued|in_progress|waiting|pending|requested)
      NOW=$(date +%s)
      if [ "$NOW" -ge "$DEADLINE" ]; then
        fail "timed out after ${TIMEOUT}s waiting for $TAG -- $URL"
      fi
      printf '  status=%s, polling again in %ss ...\n' "$STATUS" "$POLL_INTERVAL"
      sleep "$POLL_INTERVAL"
      ;;
    *)
      fail "unexpected workflow status '$STATUS' for $TAG -- $URL"
      ;;
  esac
done

# 3. Release object exists and has at least one asset attached.
REL_JSON="$(gh release view "$TAG" --repo "$REPO" --json tagName,assets,isDraft,url 2>/dev/null || true)"
if [ -z "$REL_JSON" ]; then
  fail "workflow succeeded but no release found for $TAG"
fi

IS_DRAFT="$(printf '%s' "$REL_JSON" | jq -r .isDraft)"
ASSET_COUNT="$(printf '%s' "$REL_JSON" | jq '.assets | length')"
REL_URL="$(printf '%s' "$REL_JSON" | jq -r .url)"

[ "$IS_DRAFT" = "false" ]  || fail "release $TAG is still a draft -- $REL_URL"
[ "$ASSET_COUNT" -gt 0 ]   || fail "release $TAG has no assets -- $REL_URL"

printf 'OK: %s is published with %s asset(s) -- %s\n' "$TAG" "$ASSET_COUNT" "$REL_URL"
exit 0
