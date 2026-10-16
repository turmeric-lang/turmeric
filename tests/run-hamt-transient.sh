#!/usr/bin/env bash
# tests/run-hamt-transient.sh -- in-place HAMT transients (performance audit
# R-5) against a model.
#
# Compiles the HAMT runtime + tests/test_hamt_transient.c with AddressSanitizer
# + UndefinedBehaviorSanitizer (and LeakSanitizer on Linux) and runs it.  The
# acceptance is that random transient set / update / delete sequences agree
# with an array model, leave the source map and maps derived from it
# unchanged, and free every node (a transient delete used to leak its tree).
#
# Exit status: 0 if the test binary exits 0 with no sanitizer report, else 1.

set -uo pipefail
cd "$(dirname "$0")/.."

CC="${CC:-cc}"
SRC_RUNTIME="src/runtime"
TEST_SRC="tests/test_hamt_transient.c"
BIN="$(mktemp -t tur-hamt-transient-XXXXXX)"

# region-lock-hardening: hamt.c notes stored words through region.c, so the
# standalone compile links it (and arena.c under it) exactly as every library
# that carries hamt.c does.
if ! "$CC" -std=c11 -fsanitize=address,undefined -I "$SRC_RUNTIME" \
        "$TEST_SRC" "$SRC_RUNTIME/hamt.c" "$SRC_RUNTIME/rt_alloc.c" "$SRC_RUNTIME/region.c" "$SRC_RUNTIME/arena.c" -pthread -o "$BIN" 2>/tmp/transient-cc.log; then
    echo "FAIL hamt-transient -- compile failed"
    cat /tmp/transient-cc.log
    rm -f "$BIN"
    exit 1
fi

run_out=$(ASAN_OPTIONS="detect_leaks=1:exitcode=23" \
          UBSAN_OPTIONS="halt_on_error=1:exitcode=24" "$BIN" 2>&1)
rc=$?

if grep -q "detect_leaks is not supported" <<< "$run_out"; then
    run_out=$(ASAN_OPTIONS="exitcode=23" UBSAN_OPTIONS="halt_on_error=1:exitcode=24" \
              "$BIN" 2>&1)
    rc=$?
    echo "(note: LeakSanitizer unsupported here; ran ASan/UBSan only)"
fi

echo "$run_out"
rm -f "$BIN"

if [ "$rc" -ne 0 ]; then
    echo "FAIL hamt-transient -- sanitizer reported an issue (rc=$rc)"
    exit 1
fi
if ! grep -q "all hamt transient tests passed" <<< "$run_out"; then
    echo "FAIL hamt-transient -- test did not complete"
    exit 1
fi
echo "PASS hamt-transient"
exit 0
