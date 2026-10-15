#!/usr/bin/env bash
# benchmarks/run-benchmarks.sh -- benchmark runner for Turmeric HKT (Phase §8)
#
# Measures the overhead of dictionary passing vs. monomorphic direct calls:
# each benchmark that has a C baseline gets an overhead ratio against it.
#
# Layout:
#   benchmarks/<name>.tur              -- benchmark source
#   benchmarks/<name>-baseline.c       -- monomorphic C baseline (optional)
#   benchmarks/<name>.time             -- upper bound in milliseconds (optional)
#   benchmarks/audit/expected/benchmarks__<name>.stdout
#                                      -- pinned answer (optional; checked)
#
# Usage:
#   TUR=./build-release/tur ./benchmarks/run-benchmarks.sh         -- all
#   TUR=./build-release/tur ./benchmarks/run-benchmarks.sh <name>  -- one
#
# Env:
#   TUR             compiler (default ./build-release/tur); a sanitized one is
#                   refused, see benchmarks/tur-probe.sh
#   REPS            best-of-N wall time (default 3)
#   BENCHMARK_TIMEOUT  per-run timeout in seconds (default 60)
#   RESULTS_FILE    markdown report (default benchmarks/output/benchmark-results.md;
#                   benchmarks/benchmark-results.md is a curated page, not output)
#
# The whole-corpus baseline (every engine, RSS, sizes) is
# benchmarks/audit/run-corpus.py; this script answers the narrower ratio
# question.  bench-capture-restore is skipped in the all-benchmarks sweep: it
# is SX0(a)'s multi-minute sweep and has its own runner.

set -u
cd "$(dirname "$0")/.."
. benchmarks/tur-probe.sh

TUR="${TUR:-./build-release/tur}"
[ -x "$TUR" ] || { echo "benchmarks: no tur at $TUR (set TUR=, e.g. a Release build)" >&2; exit 2; }
require_unsanitized_tur "$TUR" benchmarks

# C compiler for the baselines -- honour CC, fall back to cc.  The baselines
# get the flags the driver gives emitted C, so the ratio compares like with like.
BUILD_CC="${CC:-cc}"
BASELINE_CFLAGS="-O2 -std=c99 -Wall -fno-strict-aliasing -fwrapv"

REPS="${REPS:-3}"
TIMEOUT="${BENCHMARK_TIMEOUT:-60}"
OUTPUT_DIR="./benchmarks/output"
RESULTS_FILE="${RESULTS_FILE:-$OUTPUT_DIR/benchmark-results.md}"
mkdir -p "$OUTPUT_DIR"

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m'

# BSD/macOS date has no %N; use python3 for the clock there.
if date +%s%N 2>/dev/null | grep -q 'N$'; then
    now_ms() { python3 -c 'import time; print(int(time.time()*1000))'; }
else
    now_ms() { echo $(( $(date +%s%N) / 1000000 )); }
fi

# best_ms <exe> -- best-of-REPS wall time in ms, or "timeout"/"rc=N".
best_ms() {
    local best="" s e rc
    for _ in $(seq "$REPS"); do
        s=$(now_ms)
        timeout "$TIMEOUT" "$1" >/dev/null 2>&1
        rc=$?
        e=$(now_ms)
        [ "$rc" -eq 124 ] && { echo timeout; return; }
        [ "$rc" -ne 0 ] && { echo "rc=$rc"; return; }
        if [ -z "$best" ] || [ $((e - s)) -lt "$best" ]; then best=$((e - s)); fi
    done
    echo "$best"
}

run_benchmark() {
    local name="$1"
    local tur_file="benchmarks/${name}.tur"
    local exe_file="$OUTPUT_DIR/${name}"
    local baseline_c="benchmarks/${name}-baseline.c"
    local baseline_exe="$OUTPUT_DIR/${name}-baseline"
    local expected_time="benchmarks/${name}.time"
    local pin="benchmarks/audit/expected/benchmarks__${name}.stdout"

    echo -n "Running benchmark: ${name}... "

    # `tur build`, not emit-c + a bare cc: emitted C links against the runtime
    # archives, which only the driver knows how to find.
    if ! "$TUR" build "$tur_file" -o "$exe_file" >"$OUTPUT_DIR/${name}.build.log" 2>&1; then
        echo -e "${RED}FAIL${NC} (build failed; see $OUTPUT_DIR/${name}.build.log)"
        return 1
    fi

    # Output parity before timing: a benchmark must not look fast by doing less.
    # Only `exact` pins are compared here; a self-timing benchmark's pin is
    # masked (cols=/keep-lines=) and only benchmarks/audit/run-corpus.py reads it.
    local check
    check=$(awk -v p="$tur_file" '$1 == p { print $3 }' benchmarks/audit/corpus.txt)
    if [ -f "$pin" ] && [ "$check" = exact ]; then
        if ! timeout "$TIMEOUT" "$exe_file" 2>/dev/null | cmp -s - "$pin"; then
            echo -e "${RED}FAIL${NC} (stdout differs from $pin)"
            return 1
        fi
    fi

    local elapsed
    elapsed=$(best_ms "$exe_file")
    case "$elapsed" in
        timeout|rc=*) echo -e "${RED}FAIL${NC} ($elapsed)"; return 1 ;;
    esac

    local passed=true expected=""
    if [ -f "$expected_time" ]; then
        expected=$(cat "$expected_time")
        [ "$elapsed" -gt "$expected" ] && passed=false
    fi
    if $passed; then
        echo -e "${GREEN}PASS${NC} (${elapsed}ms, best of $REPS)"
        echo "- **${name}**: ${elapsed}ms" >> "$RESULTS_FILE"
    else
        echo -e "${RED}FAIL${NC} (${elapsed}ms > ${expected}ms expected)"
        echo "- **${name}**: ${elapsed}ms (EXCEEDED ${expected}ms)" >> "$RESULTS_FILE"
    fi

    if [ -f "$baseline_c" ]; then
        if $BUILD_CC $BASELINE_CFLAGS -o "$baseline_exe" "$baseline_c" >/dev/null 2>&1; then
            local base
            base=$(best_ms "$baseline_exe")
            case "$base" in
                timeout|rc=*) echo "  Baseline: $base" ;;
                0) echo "  Baseline: <1ms (too short for a ratio; raise its N)"
                   echo "  - Baseline: <1ms" >> "$RESULTS_FILE" ;;
                *)
                    local ratio
                    ratio=$(awk -v a="$elapsed" -v b="$base" 'BEGIN { printf "%.2f", a / b }')
                    echo "  Baseline: ${base}ms, Overhead ratio: ${ratio}x"
                    echo "  - Baseline: ${base}ms, Overhead ratio: ${ratio}x" >> "$RESULTS_FILE"
                    if awk -v r="$ratio" 'BEGIN { exit !(r > 2) }'; then
                        echo -e "  ${YELLOW}WARNING${NC}: Overhead exceeds 2x - consider filing an issue"
                        echo "  - **WARNING**: Overhead exceeds 2x" >> "$RESULTS_FILE"
                    fi ;;
            esac
        else
            echo "  Baseline: C compilation failed"
        fi
    fi
    return 0
}

echo "========================================"
echo "Turmeric HKT Dictionary Passing Benchmarks"
echo "========================================"
echo ""

{
    echo "# Dictionary Passing Benchmark Results"
    echo "Generated: $(date) -- $("$TUR" --version 2>&1 | head -1), best of $REPS"
    echo ""
    echo "## Results"
    echo ""
} > "$RESULTS_FILE"

PASS=0
FAIL=0
if [ $# -eq 0 ]; then
    for tur_file in benchmarks/*.tur; do
        name=$(basename "$tur_file" .tur)
        [ "$name" = bench-capture-restore ] && continue
        if run_benchmark "$name"; then PASS=$((PASS + 1)); else FAIL=$((FAIL + 1)); fi
        echo ""
    done
else
    for name in "$@"; do
        if [ -f "benchmarks/${name}.tur" ]; then
            if run_benchmark "$name"; then PASS=$((PASS + 1)); else FAIL=$((FAIL + 1)); fi
        else
            echo "Benchmark '$name' not found"
            FAIL=$((FAIL + 1))
        fi
    done
fi

echo "========================================"
echo "Results: $PASS passed, $FAIL failed (report: $RESULTS_FILE)"
echo "========================================"
[ $FAIL -gt 0 ] && exit 1
exit 0
