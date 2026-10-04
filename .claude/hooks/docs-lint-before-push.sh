#!/usr/bin/env bash
# PreToolUse (Bash): run the CI "Docs lint" checks before a `git push` of this
# repository, and block the push when they fail.
#
# tests/check-docs-lint.sh takes well under a second and fails on things that
# are easy to forget -- a new docs/reported/ file with no row in its README is
# the usual one -- so catching it here saves a red CI round trip.
#
# Scope: only a push of THIS repository.  The command's directory is the hook
# input's cwd, moved by the last `cd DIR` or `git -C DIR` in the command; a
# push from any other repository (e.g. a sibling turmeric-spices clone) passes
# through untouched.
#
# The checks read the WORKING TREE, not the commits being pushed, so an
# uncommitted fix makes them pass while the pushed commit would still fail.
input=$(cat)
cmd=$(jq -r '.tool_input.command // empty' <<<"$input")
cwd=$(jq -r '.cwd // empty' <<<"$input")
# Match against the command with quoted strings blanked, so a commit message
# that mentions `git push` is not taken for a push.
bare=$(sed -E "s/'[^']*'//g; s/\"[^\"]*\"//g" <<<"$cmd")

grep -Eq '(^|[^A-Za-z0-9_-])git([[:space:]]+-C[[:space:]]+[^[:space:];&|]+)?[[:space:]]+push([^A-Za-z0-9_-]|$)' <<<"$bare" || exit 0

project=${CLAUDE_PROJECT_DIR:-}
[ -n "$project" ] || exit 0
dir=${cwd:-$PWD}
target=$(grep -Eo '(^|[;&|[:space:]])(cd|git[[:space:]]+-C)[[:space:]]+[^[:space:];&|]+' <<<"$bare" | tail -n 1 |
         sed -E 's/^[;&|[:space:]]*(cd|git[[:space:]]+-C)[[:space:]]+//')
if [ -n "$target" ]; then
    target=${target/#\~/$HOME}
    case "$target" in /*) dir=$target ;; *) dir=$dir/$target ;; esac
fi
top=$(git -C "$dir" rev-parse --show-toplevel 2>/dev/null) || exit 0
ptop=$(git -C "$project" rev-parse --show-toplevel 2>/dev/null) || exit 0
[ "$top" = "$ptop" ] || exit 0

if out=$(bash "$ptop/tests/check-docs-lint.sh" 2>&1); then
    exit 0
fi
jq -n --arg r "Docs lint fails, so CI's \"Docs lint\" job would fail on this push. Fix it, commit the fix, then push again. tests/check-docs-lint.sh output:
$(grep -v '^PASS' <<<"$out")" \
    '{hookSpecificOutput: {hookEventName: "PreToolUse", permissionDecision: "deny", permissionDecisionReason: $r}}'
exit 0
