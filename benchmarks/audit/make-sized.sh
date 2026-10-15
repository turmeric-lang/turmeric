#!/bin/bash
# benchmarks/audit/make-sized.sh -- generate benchmarks/audit/sized/*.tur.
#
# Each is a copy of a corpus fixture with one size literal raised, so the
# program runs long enough (~0.4-2 s compiled on the WP0 box) for a timing to
# mean something.  The plan's WP0 asks for the *-deep / *-stress fixtures
# "sized up"; the knobs came from the fixture sweep (wp0-baseline.md).
#
# Re-run after a source fixture changes, then re-pin:
#   bash benchmarks/audit/make-sized.sh
#   python3 benchmarks/audit/run-corpus.py --repin --reps 1 --only sized/
set -e
cd "$(dirname "$0")/../.."
OUT=benchmarks/audit/sized
mkdir -p "$OUT"

# sized <name> <fixture> <what changed> <sed -E expression>
sized() {
  local name="$1" fx="$2" what="$3" expr="$4"
  local src="tests/fixtures/$fx/input.tur" dst="$OUT/$name.tur"
  if cmp -s <(sed -E "$expr" "$src") "$src"; then
    echo "make-sized: $name: the expression changed nothing in $src" >&2
    exit 1
  fi
  {
    echo ";; SIZED COPY of $src for the performance audit"
    echo ";; (benchmarks/audit/corpus.txt).  Changed: $what."
    echo ";; Everything below is the fixture's own text; its comments describe"
    echo ";; the fixture's size.  The pinned answer is"
    echo ";; benchmarks/audit/expected/benchmarks__audit__sized__$name.stdout."
    echo ";; Regenerate (after the fixture changes): bash benchmarks/audit/make-sized.sh"
    sed -E "$expr" "$src"
  } > "$dst"
  echo "$dst"
}

sized effect-nontail-ticks-1m effect-nontail-resume-under-outer \
  "(ticks 20000 0) -> (ticks 1000000 0)" \
  's/\(ticks 20000 0\)/(ticks 1000000 0)/'
sized cps-tramp-resume-8m cps-tramp-resume-deep-1m \
  "(loop 1000000 0) -> (loop 8000000 0)" \
  's/\(loop 1000000 0\)/(loop 8000000 0)/'
sized closure-let-loop-10m closure-let-in-self-tail-loop \
  "(loop 0 10000 0) -> (loop 0 10000000 0)" \
  's/\(loop 0 10000 0\)/(loop 0 10000000 0)/'
sized fn-value-join-1000x fn-value-call-join-reclaimed \
  "loop 2000 -> 2000000, loop-clo and loop-val 1000 -> 1000000" \
  's/\(loop 0 2000 0\)/(loop 0 2000000 0)/; s/\(loop-clo 0 1000 0\)/(loop-clo 0 1000000 0)/; s/\(loop-val 0 1000 0\)/(loop-val 0 1000000 0)/'
sized region-rounds-100k region-with-region \
  "(rounds 100 8 0) -> (rounds 100000 8 0)" \
  's/\(rounds 100 8 0\)/(rounds 100000 8 0)/'
sized mut-map-reassign-1m mut-map-reassign \
  "(while (< i 3) ...) -> (while (< i 1000000) ...)" \
  's/\(while \(< i 3\)/(while (< i 1000000)/'
