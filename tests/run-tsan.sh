#!/usr/bin/env bash
# tests/run-tsan.sh -- the concurrency fixtures under ThreadSanitizer.
#
# Runs tests/run.sh with TUR_TSAN=1 (every fixture program compiled with
# -fsanitize=thread) over the fixtures that exercise threads: every one that
# carries a `requires.tsan` marker -- those are skipped by an ordinary run and
# run NOWHERE else -- plus the STM, async, fiber, channel, select, future,
# mutex, atomic, scheduler and thread-pool families.  A data race is a failed
# fixture: TSan exits 66 after reporting one.
#
# Nightly in .github/workflows/tsan.yml (security-audit-plan WP5).  Locally:
#
#   cmake -S . -B build-tsan -DCMAKE_BUILD_TYPE=Release -DCMAKE_POLICY_VERSION_MINIMUM=3.5
#   cmake --build build-tsan -j
#   TUR=./build-tsan/tur timeout 720 bash tests/run-tsan.sh
#
# or `tur run test-tsan`, which does all three.
#
# TUR must not be an ASan build (the default Debug one is): the fixture
# programs link against the runtime archives next to it, and ASan and TSan
# cannot share a process.  Release, or Debug with -DTUR_DEBUG_SANITIZE=OFF.
#
# Env:
#   TUR   compiler to test (default ./build-tsan/tur, then ./build/tur)

set -uo pipefail
cd "$(dirname "$0")/.."

TUR="${TUR:-./build-tsan/tur}"
[ -x "$TUR" ] || TUR=./build/tur
if [ ! -x "$TUR" ]; then
    echo "run-tsan: no compiler at $TUR" >&2
    exit 2
fi
_cache="$(dirname "$TUR")/CMakeCache.txt"
if [ -f "$_cache" ] && grep -q '^CMAKE_BUILD_TYPE:STRING=Debug$' "$_cache" &&
   ! grep -q '^TUR_DEBUG_SANITIZE:BOOL=OFF$' "$_cache"; then
    echo "run-tsan: $TUR is an ASan build; use Release or -DTUR_DEBUG_SANITIZE=OFF" >&2
    exit 2
fi

# Families that spawn threads or schedule across them.
FAMILY_RE='^(stm|async|asyncchan|fiber|chan|channel|select|future|promise|thread|threads|threaded|mutex|rwlock|semaphore|barrier|atomic|scheduler|work-queue|producer-consumer)(-|$)'

# Excluded, each with the reason it cannot run under TSan.  A fixture only
# leaves this list with the reason gone.
#
#   threads-effects-tail-resume: TSan's own trace allocator faults
#     (__tsan::TracePartAlloc, SEGV) about 16k longjmp-driven tail resumes
#     into a worker; the program is clean in every other build.  The one race
#     TSan did report on it, the handler-group counter, is fixed.
EXCLUDE=" threads-effects-tail-resume "

names=()
for d in tests/fixtures/*/; do
    n="$(basename "$d")"
    [ -f "$d/input.tur" ] || continue
    if [ -f "$d/requires.tsan" ] || [[ "$n" =~ $FAMILY_RE ]]; then
        case "$EXCLUDE" in *" $n "*) continue ;; esac
        names+=("$n")
    fi
done
if [ ${#names[@]} -eq 0 ]; then
    echo "run-tsan: no fixtures selected" >&2
    exit 2
fi
echo "run-tsan: ${#names[@]} fixtures under -fsanitize=thread (TUR=$TUR)"

filter="^($(IFS='|'; echo "${names[*]}"))\$"
TUR="$TUR" TUR_TSAN=1 TUR_TEST_FILTER="$filter" bash tests/run.sh
