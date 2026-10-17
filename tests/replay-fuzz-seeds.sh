#!/usr/bin/env bash
# tests/replay-fuzz-seeds.sh -- replay one harness's recorded fuzz seeds.
#
# Reads tests/fuzz-seed-corpus.txt and re-runs every seed recorded for the
# named harness at smoke size.  See that file's header for why, and
# docs/archive/type-confusion-detection-plan.md section F2.
#
# Usage: replay-fuzz-seeds.sh <harness> <driver.py> <tur> <n>
#
# <n> is the smoke case count.  A corpus row may raise it for its own seed,
# or name the cases to run instead, with a third column (see the corpus
# header).
#
# Exits non-zero if any replayed seed fails.  An empty corpus exits 0 after
# saying so -- silence would be indistinguishable from "the file went
# missing", which is the failure mode this project keeps re-learning.
set -uo pipefail
cd "$(dirname "$0")/.."

HARNESS="$1"; DRIVER="$2"; TUR="$3"; N="$4"
CORPUS="tests/fuzz-seed-corpus.txt"

if [ ! -f "$CORPUS" ]; then
  echo "  replay-fuzz-seeds: $CORPUS is missing -- recorded seeds are not being replayed" >&2
  exit 1
fi

rc=0
found=0
while read -r h seed cases _rest; do
  case "$h" in ''|'#'*) continue ;; esac
  [ "$h" = "$HARNESS" ] || continue
  case "$seed" in ''|*[!0-9]*)
    echo "  replay-fuzz-seeds: $HARNESS: ignoring non-numeric seed '$seed'" >&2
    continue ;;
  esac
  # Optional third column.  A case's program depends only on (seed, case
  # index), so a finding at index k replays as cases 0..k, i.e. --n k+1 -- but
  # not at the smoke N when k >= N, which is where most of a --n 400 nightly's
  # findings land.  A count raises n (never below smoke N); a range A-B runs
  # cases A..B alone (--first A --n B+1), so a finding at 373 costs one case
  # on every smoke run rather than 374.
  first=0
  n="$N"
  case "$cases" in
    ''|'#'*) ;;
    *[0-9]-*[0-9])
      a="${cases%%-*}"; b="${cases#*-}"
      case "$a$b" in
        *[!0-9]*) echo "  replay-fuzz-seeds: $HARNESS seed $seed: ignoring malformed case range '$cases'" >&2 ;;
        *) if [ "$a" -le "$b" ]; then first="$a"; n=$((b + 1))
           else echo "  replay-fuzz-seeds: $HARNESS seed $seed: ignoring backwards case range '$cases'" >&2
           fi ;;
      esac ;;
    *[!0-9]*)
      echo "  replay-fuzz-seeds: $HARNESS seed $seed: ignoring non-numeric case count '$cases'" >&2 ;;
    *) [ "$cases" -gt "$N" ] && n="$cases" ;;
  esac
  found=$((found + 1))
  if [ "$first" -gt 0 ]; then
    echo "  replaying $HARNESS seed $seed (cases $first..$((n - 1)))"
    python3 "$DRIVER" --tur "$TUR" --first "$first" --n "$n" --seed "$seed" || rc=1
  else
    echo "  replaying $HARNESS seed $seed (n $n)"
    python3 "$DRIVER" --tur "$TUR" --n "$n" --seed "$seed" || rc=1
  fi
done < "$CORPUS"

if [ "$found" -eq 0 ]; then
  echo "  replay-fuzz-seeds: no recorded seeds for '$HARNESS' (corpus is empty)"
fi
exit $rc
