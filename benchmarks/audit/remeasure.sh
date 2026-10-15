#!/usr/bin/env bash
# benchmarks/audit/remeasure.sh -- a before/after on the WP0 baseline box.
#
# The performance audit's numbers compare only with numbers from the same box
# (performance-audit-plan, decision 1), so the payoff of a fix the audit
# ranked is measured here: the same corpus, the same runner, two `tur`s.
#
# Usage (inside the WSL2 distro, from the checkout whose corpus and pins to
# use -- normally the "after" one):
#
#   bash benchmarks/audit/remeasure.sh BEFORE_TUR AFTER_TUR OUT_DIR \
#        [--sweeps N] [-- RUN_CORPUS_ARGS...]
#
#   BEFORE_TUR / AFTER_TUR  Release -DTUR_JIT=ON builds, each in its own
#                           checkout (a tur reads the stdlib beside it, so a
#                           stdlib fix needs its own tree), e.g.
#                           /srv/perf/main/build-release/tur and
#                           /srv/perf/fix/build-release/tur
#   --sweeps N              sweeps per side (default 3, as WP0)
#   RUN_CORPUS_ARGS         passed to run-corpus.py, e.g. --only 'triangle|sized'
#                           or --engines cc,jit or --corpus benchmarks/audit/tracked.txt
#
# What it does: warns when the host is not on the High performance plan or the
# box is busy; runs the sweeps ALTERNATING before / after (so slow drift on the
# host lands on both sides, not on one); merges each side with merge-sweeps.py;
# and writes OUT_DIR/compare.md (and compare.csv) with compare-runs.py.  Each
# sweep is run-corpus.py --reps 5 --cooldown 3, the WP0 method.  The full corpus
# is roughly 10 minutes a sweep, so the default six sweeps take about an hour;
# --only narrows it.

set -u
cd "$(dirname "$0")/../.."

usage() {
    sed -n '2,30p' "$0" | sed 's/^# \{0,1\}//'
    exit 2
}
[ $# -ge 3 ] || usage
BEFORE=$1
AFTER=$2
OUT=$3
shift 3
SWEEPS=3
if [ "${1:-}" = "--sweeps" ]; then
    SWEEPS=${2:?--sweeps needs a number}
    shift 2
fi
[ "${1:-}" = "--" ] && shift
EXTRA=("$@")

for t in "$BEFORE" "$AFTER"; do
    [ -x "$t" ] || { echo "remeasure: no tur at $t" >&2; exit 2; }
done

# The box, checked the way WP0 set it up (docs/artifacts/perf-audit-2026-10/
# README.md): warnings, not refusals -- the numbers are still numbers, but a
# result taken this way should say so.
PC=/mnt/c/Windows/System32/powercfg.exe
if [ -x "$PC" ]; then
    scheme=$("$PC" /getactivescheme 2>/dev/null | tr -d '\r')
    case "$scheme" in
        *8c5e7fda-e8bf-4a96-9a85-a6e23a8c635c*) ;;
        *) echo "remeasure: WARNING: the host is not on the High performance plan: ${scheme:-unknown}" >&2 ;;
    esac
fi
# The 1-minute load average still counts a build that just finished, so give
# it up to two minutes to settle before calling the box busy -- and then two
# more: an all-core build leaves the CPU heat-soaked and boosting lower, and
# an A/A run started 30 s after one read mut-map's JIT run at 2.08 s against
# 1.71 s an hour later (docs/artifacts/perf-audit-2026-10/README.md).
busy() { awk -v l="$(cut -d' ' -f1 /proc/loadavg 2>/dev/null || echo 0)" 'BEGIN { exit !(l > 1.0) }'; }
if busy; then
    echo "remeasure: the box is busy (load $(cut -d' ' -f1 /proc/loadavg)); waiting for it to settle" >&2
    for _ in $(seq 24); do busy || break; sleep 5; done
    if busy; then
        echo "remeasure: WARNING: load average $(cut -d' ' -f1 /proc/loadavg) -- is a build or suite running?" >&2
    else
        echo "remeasure: settled; two more minutes for the CPU to cool" >&2
        sleep 120
    fi
fi

mkdir -p "$OUT"
for i in $(seq "$SWEEPS"); do
    for side in before after; do
        tur=$BEFORE
        [ "$side" = after ] && tur=$AFTER
        echo "remeasure: sweep $i/$SWEEPS, $side ($(date +%T))" >&2
        if ! python3 benchmarks/audit/run-corpus.py --tur "$tur" --reps 5 --cooldown 3 \
                --out "$OUT/$side-$i" "${EXTRA[@]+"${EXTRA[@]}"}" > "$OUT/$side-$i.log" 2>&1; then
            echo "remeasure: run-corpus failed ($side, sweep $i); see $OUT/$side-$i.log" >&2
            tail -5 "$OUT/$side-$i.log" >&2
            exit 1
        fi
    done
done

for side in before after; do
    dirs=()
    for i in $(seq "$SWEEPS"); do dirs+=("$OUT/$side-$i"); done
    python3 benchmarks/audit/merge-sweeps.py "$OUT/$side" "${dirs[@]}" > /dev/null || exit 1
done

python3 benchmarks/audit/compare-runs.py "$OUT/before" "$OUT/after" --csv "$OUT/compare.csv" \
    | tee "$OUT/compare.md"
{
    echo
    echo "Box: $(grep -m1 'model name' /proc/cpuinfo | cut -d: -f2- | sed 's/^ //'), $(nproc) vCPUs; ${scheme:-power plan unknown}"
    echo "before: $("$BEFORE" --version 2>&1 | head -1) ($BEFORE)"
    echo "after:  $("$AFTER" --version 2>&1 | head -1) ($AFTER)"
    echo "corpus commit: $(git rev-parse --short HEAD); $SWEEPS sweeps a side, alternating; $(date -u +%Y-%m-%dT%H:%MZ)"
} | tee -a "$OUT/compare.md"
