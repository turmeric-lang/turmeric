#!/usr/bin/env bash
# tests/run-fnsan.sh -- the fixture corpus under clang's -fsanitize=function.
#
# Every indirect call the emitted C makes has to go through a function pointer
# of EXACTLY the callee's type: WebAssembly's call_indirect traps on any
# difference, and when the types disagree on a `double` or a 16-byte `any` the
# native build gives a silent wrong answer instead.  clang checks that at
# runtime with -fsanitize=function; in trap mode (no UBSan runtime needed) a
# mismatch is SIGILL, which tests/run.sh reports as a failed fixture.
#
# The corpus reached zero traps on 2026-10-02 (the ptr<void> / fn-parameter
# slot decision in docs/archive/emitted-c-indirect-calls-are-not-type-exact.md),
# so this is a gate: any FAIL is a new mismatched call.  To find the site,
# build the fixture at -O0 -g with the same flags and run it under gdb -- at
# -O1 and above the trap is attributed to whatever got inlined around it.
#
# Two things this needs that the default suite does not:
#   * clang.  GCC has no equivalent.  CC overrides the choice.
#   * an UNSANITIZED libturi.a.  The Debug build's is compiled with gcc's ASan,
#     which clang cannot link against, so the fixtures that link -lturi would
#     fail to build and quietly drop out of the count.  Point
#     TUR_FNSAN_LIB_DIR at one (default build-nosan/src):
#       cmake -S . -B build-nosan -DCMAKE_BUILD_TYPE=Debug -DTUR_DEBUG_SANITIZE=OFF
#       cmake --build build-nosan -j --target libturi
#
# The detector is proven ARMED before the suite runs: a canary with one
# mismatched and one matched indirect call must trap on the first and run the
# second (tests/fuzz_arm.py).  A detector that matches nothing looks exactly
# like a clean run, so an unarmed run fails rather than passing vacuously.
#
# A fixture whose trap is a KNOWN open defect carries a `known.fnsan` marker
# naming the report.  It is left out of the gate run, then run on its own and
# required to STILL trap: a marked fixture that runs clean fails this script
# with "delete known.fnsan", so the list cannot go stale (the expected.xfail
# discipline).  A marker is for a defect with a report, never to park a new
# trap.
#
# tests/run.sh reads the rest of the environment as usual, so
# TUR_TEST_FILTER / TUR_TEST_SHARD compose.

set -uo pipefail
cd "$(dirname "$0")/.."

CC="${CC:-clang}"
LIB_DIR="${TUR_FNSAN_LIB_DIR:-build-nosan/src}"

if ! command -v "$CC" >/dev/null 2>&1; then
  echo "FAIL run-fnsan: no $CC on PATH (set CC to a clang)"
  exit 1
fi
if [ ! -f "$LIB_DIR/libturi.a" ]; then
  echo "FAIL run-fnsan: no unsanitized libturi.a in $LIB_DIR"
  echo "  cmake -S . -B build-nosan -DCMAKE_BUILD_TYPE=Debug -DTUR_DEBUG_SANITIZE=OFF"
  echo "  cmake --build build-nosan -j --target libturi"
  exit 1
fi

if ! python3 - "$CC" <<'EOF'
import sys
sys.path.insert(0, "tests")
import fuzz_arm
sys.exit(0 if fuzz_arm._probe(sys.argv[1]) else 1)
EOF
then
  echo "FAIL run-fnsan: $CC did not trap the canary's mismatched call --"
  echo "  -fsanitize=function is not armed, and a run would pass vacuously"
  exit 1
fi
echo "fnsan: ARMED ($CC, canary trapped); libturi from $LIB_DIR"

LIB_ABS="$(cd "$LIB_DIR" && pwd)"
export CC
export TUR_CC_FLAGS="-O2 -std=c99 -Wall -Wfloat-conversion -Werror=implicit-function-declaration -fno-strict-aliasing -fsanitize=function -fsanitize-trap=function -L$LIB_ABS"

known=()
for m in tests/fixtures/*/known.fnsan; do
  [ -f "$m" ] || continue
  known+=("$(basename "$(dirname "$m")")")
done
exclude=""
for k in ${known[@]+"${known[@]}"}; do
  exclude="${exclude:+$exclude|}^${k}\$"
  echo "fnsan: known trap, checked separately: $k ($(head -1 "tests/fixtures/$k/known.fnsan"))"
done

rc=0
TUR_TEST_EXCLUDE="$exclude" bash tests/run.sh || rc=$?

# Each known trap must still trap -- and trap, not fail some other way.
for k in ${known[@]+"${known[@]}"}; do
  if [ -n "${TUR_TEST_FILTER:-}" ] && ! [[ "$k" =~ $TUR_TEST_FILTER ]]; then
    continue
  fi
  log=$(TUR_TEST_FILTER="^${k}\$" TUR_TEST_EXCLUDE="" bash tests/run.sh 2>&1)
  if grep -q "^PASS $k" <<< "$log"; then
    echo "FAIL $k -- carries known.fnsan but no longer traps: delete the marker"
    rc=1
  elif ! grep -qE "exit(ed)? 132" <<< "$log"; then
    echo "FAIL $k -- carries known.fnsan but failed WITHOUT a trap:"
    grep -E "^FAIL" <<< "$log" | sed 's/^/    /'
    rc=1
  else
    echo "KNOWN $k -- still traps (see its known.fnsan)"
  fi
done
exit "$rc"
