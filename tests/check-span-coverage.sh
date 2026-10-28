#!/usr/bin/env bash
#
# check-span-coverage.sh -- debugger Phase 1 span-coverage gate.
#
# Walks every fixture under tests/fixtures/ and runs `tur audit-spans` on its
# entry source.  A fixture FAILS the gate when the elaborated AST contains a
# breakpoint-eligible node (a top-level form, defn, let form, or call site)
# with no usable source span -- exactly the holes a debugger would choke on
# when resolving a breakpoint or stack frame to a source location.
#
# `tur audit-spans` exit codes:
#   0  clean       -- audited, every breakpoint-eligible node has a span
#   3  holes       -- audited, one or more nodes lack a usable span  -> FAIL
#   1  no-elaborate -- the file did not elaborate (error fixture, or a
#                      requires.spices fixture whose sibling repo is absent) -> SKIP
#   2  file error  -- input unreadable                                       -> SKIP
#
# The audits run in parallel across the machine's cores.  Each one elaborates
# the whole stdlib under a sanitized Debug `tur` (~0.1 s), and there are
# ~3200 fixtures, so the serial loop this used to be was the single longest
# auxiliary suite in CI by a wide margin: 5.5 min on the 3-core macOS runner,
# 3.8 min on Linux, per the /ci timings.  Every audit is independent, so
# `xargs -P` does the fan-out.  Set TUR_SPAN_JOBS to override the worker count.
#
# The harness's own process count is kept flat on purpose, because on macOS
# fork+exec is the expensive part.  It used to spend a `bash -c`, a `cksum` and
# a `cut` per fixture to name its result file, and then tallied with three
# `sed`s per fixture in a serial loop: ~13,000 processes, 29 s of the 135 s
# run on a 4-core Linux box with `tur` replaced by `true`, 22 s of it in the
# serial tally.  Now each worker takes a batch of fixtures, a result file is
# named by the fixture's zero-padded ordinal (so the glob is already in path
# order), and one awk reads them all back.
#
# Environment:
#   TUR_SPAN_JOBS    worker count (default: the machine's cores)
#   TUR_SPAN_SHARD   audit a round-robin slice, "i/N" 1-based (default: all).
#                    Its own variable, not TUR_TEST_SHARD, for the reason
#                    tests/run-turi.sh gives: this runs inside the 160-test
#                    `aux` ctest part, and the timings collector tags rows from
#                    the job's environment.  Clamped exactly as run-turi.sh
#                    clamps TUR_TURI_SHARD.
#   TUR_SPAN_LIST=1  print the fixture inputs this invocation would audit, one
#                    per line, and exit 0 without needing a built `tur`.
#                    tests/run-shard-partition.sh reads it to prove that
#                    TUR_SPAN_SHARD partitions the corpus.
#
# This is the gate for the Phase 1 exit criterion in
# docs/archive/history/debugger-plan.md.  See docs/artifacts/debugger-spans-audit.md
# for the audit write-up.

set -uo pipefail
cd "$(dirname "$0")/.."

TUR_SPAN_SHARD="${TUR_SPAN_SHARD:-}"
SHARD_INDEX=0
SHARD_TOTAL=1
if [ -n "$TUR_SPAN_SHARD" ]; then
  case "$TUR_SPAN_SHARD" in
    */*)
      shard_left="${TUR_SPAN_SHARD%/*}"
      shard_right="${TUR_SPAN_SHARD#*/}"
      case "$shard_left" in ''|*[!0-9]*) shard_left=1 ;; esac
      case "$shard_right" in ''|*[!0-9]*) shard_right=1 ;; esac
      if [ "$shard_right" -lt 1 ]; then shard_right=1; fi
      if [ "$shard_left" -lt 1 ]; then shard_left=1; fi
      if [ "$shard_left" -gt "$shard_right" ]; then shard_left="$shard_right"; fi
      SHARD_TOTAL="$shard_right"
      SHARD_INDEX=$((shard_left - 1))
      ;;
  esac
fi

# enumerate -- "<ordinal>\0<input>\0" for every fixture this shard audits.
# The ordinal counts every fixture with an entry source, in glob order, before
# the shard filter, so the slices of one N are disjoint and cover the corpus.
enumerate() {
  local n=0 d input
  for d in tests/fixtures/*/; do
    input="${d}input.tur"
    [ -f "$input" ] || input="${d}$(basename "$d").tur"
    [ -f "$input" ] || continue
    if [ "$SHARD_TOTAL" -le 1 ] || [ $((n % SHARD_TOTAL)) -eq "$SHARD_INDEX" ]; then
      printf '%06d\0%s\0' "$n" "$input"
    fi
    n=$((n + 1))
  done
}

if [ "${TUR_SPAN_LIST:-0}" = "1" ]; then
  enumerate | tr '\000' '\n' | awk 'NR % 2 == 0'
  exit 0
fi

TUR="${TUR:-./build/tur}"
if [ ! -x "$TUR" ]; then
  echo "FAIL span-coverage: tur binary not found at $TUR (build first)"
  exit 1
fi
case "$TUR" in
  /*) ;;
  *)  TUR="$(pwd)/$TUR" ;;
esac
export TUR

JOBS="${TUR_SPAN_JOBS:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)}"
RES="$(mktemp -d -t tur-span.XXXXXX)"
trap 'rm -rf "$RES"' EXIT
export RES

# audit_batch <ordinal> <input> [<ordinal> <input> ...] -- one result file per
# fixture, "$RES/<ordinal>": "<verdict>\n<input>\n<audit output>".  The path is
# stored inside rather than reconstructed from the name, so fixture names with
# any character in them round-trip.
audit_batch() {
  local key input out rc
  while [ "$#" -ge 2 ]; do
    key="$1" input="$2"
    shift 2
    out="$("$TUR" audit-spans "$input" 2>/dev/null)"
    rc=$?
    case $rc in
      0) printf 'pass\n%s\n' "$input" > "$RES/$key" ;;
      3) printf 'fail\n%s\n%s\n' "$input" "$out" > "$RES/$key" ;;
      *) printf 'skip\n%s\n' "$input" > "$RES/$key" ;;  # 1 = no-elaborate, 2 = file error, anything else
    esac
  done
}
export -f audit_batch

# 32 arguments = 16 fixtures per worker process: few enough that the last
# batches still spread across the workers, many enough that the per-batch bash
# is noise.
enumerate | xargs -0 -n 32 -P "$JOBS" bash -c 'audit_batch "$@"' _

# Every fixture of the slice must have left a result.  A worker that died
# mid-batch would otherwise shrink the denominator without a word.
want="$(enumerate | tr -cd '\000' | wc -c | tr -d ' ')"
want=$((want / 2))

# The tally, in one process.  The glob is in ordinal order, which is path
# order, so the report is stable.
set -- "$RES"/*
[ -e "$1" ] || set -- /dev/null   # no results: awk must not read stdin
awk -v want="$want" -v shard="$TUR_SPAN_SHARD" '
  FNR == 1 { verdict = $0; got++; next }
  FNR == 2 {
    input = $0
    if (verdict == "pass") pass++
    else if (verdict == "skip") skip++
    else { fail++; report = report "FAIL span-coverage: " input "\n" }
    next
  }
  verdict == "fail" { report = report "  " $0 "\n" }
  END {
    slice = (shard != "" ? " [shard " shard "]" : "")
    if (got != want) {
      printf "FAIL span-coverage: %d of %d audit(s) left no result%s\n", want - got, want, slice
      exit 1
    }
    if (fail) {
      printf "%s", report
      printf "FAIL span-coverage: %d fixture(s) with breakpoint-span holes (pass=%d skip=%d)%s\n", fail, pass, skip, slice
      exit 1
    }
    printf "PASS span-coverage: %d clean, %d skipped (did not elaborate)%s\n", pass, skip, slice
  }
' "$@"
