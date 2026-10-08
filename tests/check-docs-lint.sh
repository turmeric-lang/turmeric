#!/usr/bin/env bash
# check-docs-lint.sh -- every check the CI "Docs lint" job runs, in one place.
#
# These are the checks that read docs/ and need no compiler, so they take well
# under a second.  The CI job (.github/workflows/ci.yml, docs-lint) and the
# Claude Code pre-push hook (.claude/hooks/docs-lint-before-push.sh) both call
# this script, so a check added here is enforced by both.  Each check is also
# its own ctest target (CMakeLists.txt) for the full suite.
#
# Runs every check even after one fails, then exits non-zero if any failed.
set -u
cd "$(dirname "$0")/.."

fail=0
for check in tests/check-reported-index.sh tests/check-r7rs-srfi-sync.sh \
             tests/check-running-tests-guide.sh; do
    bash "$check" || fail=1
done
exit "$fail"
