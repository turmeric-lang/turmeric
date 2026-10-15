#!/usr/bin/env bash
# benchmarks/memory/measure.sh -- regenerate the memory-usage guide's cost table.
#
# This is the harness docs/guides/memory-usage-guide.md describes in "How the
# numbers were measured", generalized from one probe to a directory of them
# and checked in, so the guide's cost table is re-measured rather than retyped
# (performance-audit-plan WP3, M-1).
#
# Usage:
#   TUR=./build-release/tur bash benchmarks/memory/measure.sh [options] [probe.tur ...]
#
#   With no probe arguments every benchmarks/memory/probes/*.tur is measured.
#
# Options:
#   --out DIR   also write DIR/memory-costs.csv and DIR/memory-costs.md
#               (DIR is created; nothing is written into the repo otherwise)
#   --n N       measure every probe at N instead of its own N.  The expect-N
#               stdout check is then skipped (the pinned line is for the
#               probe's own N); the expect-0 check still runs.
#   -j J        probes measured concurrently (default 2, at most 4)
#   --keep      keep the scratch directory and print where it is
#   -h, --help  this text
#
# Env:
#   TUR         the compiler (default <repo>/build-release/tur).  A sanitized
#               tur is refused (benchmarks/tur-probe.sh): an ASan build's
#               numbers are shadow-memory bookkeeping, not results.
#   TMPDIR      where the scratch directory is made (mktemp -d).  Builds,
#               binaries and valgrind logs all live there, never in the repo.
#   VALGRIND    valgrind binary (default valgrind)
#   MASSIF_PEAK_INACCURACY  massif --peak-inaccuracy (default 0.0: the exact
#               peak; massif's own default of 1.0 can under-read the peak by
#               1% of the baseline, which is several bytes per element here)
#   PROBE_TIMEOUT  seconds per native run, per valgrind run (default 300)
#
# The method (the guide's, unchanged):
#
#   Each probe is a program with `__N__` where the element count goes.  It is
#   built twice, with `__N__` = 0 and = N (default 10000), and each binary is
#   run under valgrind memcheck and under valgrind --tool=massif.  Every
#   number is (value at N - value at 0) / N, so start-up, the stdio buffer,
#   the runtime's one-time tables and a probe's warm-up element all cancel:
#
#     allocs/elt  memcheck "total heap usage" allocations
#     bytes/elt   memcheck "total heap usage" bytes allocated -- TRAFFIC: every
#                 allocation counts, even one freed straight away
#     peak/elt    the largest mem_heap_B + mem_heap_extra_B snapshot massif
#                 took -- how big the process gets, malloc's per-block
#                 bookkeeping and 16-byte rounding included
#     peak req/elt  the mem_heap_B part of that same snapshot: the bytes the
#                 program asked malloc for, without the overhead.  For a
#                 structure of many small blocks (a HAMT) the two differ by a
#                 third; for one big buffer (a Vec) they are the same.
#     lost/elt    memcheck LEAK SUMMARY "definitely lost" bytes
#     +ind/elt    memcheck LEAK SUMMARY "indirectly lost" bytes: blocks only a
#                 lost block points to.  A leaked linked list of N cells is one
#                 definitely-lost head and N-1 indirectly-lost cells, so the
#                 two columns together are what was leaked.
#
# Probe annotations (comment lines anywhere in the probe):
#
#   ;; row: <label>              the table's Feature cell (default: file name)
#   ;; guide: <text>             the guide's 2026-10-05 cell, printed beside
#                                the measurement so a moved row is visible
#   ;; measure: N=<n>            this probe's N (default 10000); N=1 measures
#                                a one-time cost
#   ;; expect-0: <line>          the first line of stdout at N=0
#   ;; expect-N: <line>          the first line of stdout at the probe's N
#   ;; expect-regions-0: <s>     TUR_REGION_STATS=1's line at N=0, after
#   ;; expect-regions-N: <s>     "region-stats: " (e.g. "pushes=1 rewinds=1 retires=0")
#
# A probe whose stdout or region-stats line does not match its expect line
# FAILS and is not measured: a probe that does less than it says must not
# produce a small number.  So does one that fails to build or exits non-zero.
# The script exits 1 if any probe failed.

set -u

here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
repo=$(cd "$here/../.." && pwd)
. "$repo/benchmarks/tur-probe.sh"

usage() { sed -n '2,/^$/{s/^# \{0,1\}//;p}' "${BASH_SOURCE[0]}"; }

OUT=""
N_OVERRIDE=""
JOBS=2
KEEP=0
PROBES=()
needs_arg() { [ $# -ge 2 ] || { echo "measure.sh: $1 needs an argument" >&2; exit 2; }; }
while [ $# -gt 0 ]; do
    case "$1" in
        --out)  needs_arg "$@"; OUT="$2"; shift 2 ;;
        --n)    needs_arg "$@"; N_OVERRIDE="$2"; shift 2 ;;
        -j)     needs_arg "$@"; JOBS="$2"; shift 2 ;;
        -j*)    JOBS="${1#-j}"; shift ;;
        --keep) KEEP=1; shift ;;
        -h|--help) usage; exit 0 ;;
        -*)     echo "measure.sh: unknown option $1" >&2; exit 2 ;;
        *)      PROBES+=("$(cd "$(dirname "$1")" && pwd)/$(basename "$1")"); shift ;;
    esac
done
[ ${#PROBES[@]} -eq 0 ] && PROBES=("$here"/probes/*.tur)
case "$JOBS" in ''|*[!0-9]*) echo "measure.sh: -j wants a number" >&2; exit 2 ;; esac
[ "$JOBS" -lt 1 ] && JOBS=1
[ "$JOBS" -gt 4 ] && JOBS=4
if [ -n "$N_OVERRIDE" ]; then
    case "$N_OVERRIDE" in ''|*[!0-9]*|0) echo "measure.sh: --n wants a positive number" >&2; exit 2 ;; esac
fi

TUR="${TUR:-$repo/build-release/tur}"
case "$TUR" in
    /*) ;;
    */*) TUR="$(cd "$(dirname "$TUR")" 2>/dev/null && pwd)/$(basename "$TUR")" ;;
    *)  TUR="$(command -v "$TUR" || echo "$TUR")" ;;   # a bare name: from PATH
esac
[ -x "$TUR" ] || { echo "measure.sh: no tur at $TUR (set TUR=, e.g. a Release build)" >&2; exit 2; }
require_unsanitized_tur "$TUR" measure.sh

VALGRIND="${VALGRIND:-valgrind}"
command -v "$VALGRIND" >/dev/null 2>&1 || { echo "measure.sh: no valgrind on PATH" >&2; exit 2; }
PEAK_INACC="${MASSIF_PEAK_INACCURACY:-0.0}"
PTIMEOUT="${PROBE_TIMEOUT:-300}"

work=$(mktemp -d "${TMPDIR:-/tmp}/tur-memory.XXXXXX") || exit 2
if [ "$KEEP" = 1 ]; then
    echo "measure.sh: scratch kept at $work" >&2
else
    trap 'rm -rf "$work"' EXIT
fi

# annot <file> <key> -- the text after ";; <key>:" on the first such line.
annot() {
    sed -n "s/^[[:space:]]*;;[[:space:]]*$2:[[:space:]]*//p" "$1" | head -1 | sed 's/[[:space:]]*$//'
}

# measure_one <probe> <n> <dir> -- build and run the probe at one N.  Writes
# <dir>/n<n>.vals: "0 allocs bytes peak peakreq deflost indlost possible
# reachable" (peakreq is the mem_heap_B part of the peak snapshot: the bytes
# the program asked for, without malloc's overhead), or a one-word failure
# ("build", "exit=<rc>", "valgrind"), plus <dir>/n<n>.stdout / .stderr from
# the native run.
measure_one() {
    local probe="$1" n="$2" dir="$3" base src bin
    base=$(basename "$probe" .tur)
    # A per-probe, per-N file name: single-file `tur build` keeps its
    # intermediate C in a shared directory keyed on the base name.
    src="$dir/${base}__n$n.tur"
    bin="$dir/${base}__n$n.bin"
    sed "s/__N__/$n/g" "$probe" > "$src"
    if ! (cd "$dir" && "$TUR" build "$src" -o "$bin") > "$dir/build-n$n.log" 2>&1; then
        echo "build" > "$dir/n$n.vals"; return
    fi
    TUR_REGION_STATS=1 timeout "$PTIMEOUT" "$bin" > "$dir/n$n.stdout" 2> "$dir/n$n.stderr"
    local rc=$?
    if [ "$rc" -ne 0 ]; then echo "exit=$rc" > "$dir/n$n.vals"; return; fi
    timeout "$PTIMEOUT" "$VALGRIND" --log-file="$dir/mc-n$n.log" "$bin" > /dev/null 2>&1
    timeout "$PTIMEOUT" "$VALGRIND" --tool=massif --peak-inaccuracy="$PEAK_INACC" \
        --massif-out-file="$dir/ms-n$n.out" "$bin" > /dev/null 2> "$dir/ms-n$n.log"
    local mc ms
    mc=$(awk '
        { gsub(",", "") }
        /total heap usage:/ { a = $5; b = $9 }
        /definitely lost:/  { d = $4 }
        /indirectly lost:/  { i = $4 }
        /possibly lost:/    { p = $4 }
        /still reachable:/  { r = $4 }
        END { if (a == "") print "none"; else print a+0, b+0, d+0, i+0, p+0, r+0 }' "$dir/mc-n$n.log")
    ms=$(awk -F= '/^mem_heap_B=/ { h = $2 }
                  /^mem_heap_extra_B=/ { t = h + $2; if (m == "" || t > m) { m = t; mh = h } }
                  END { if (m == "") print "none"; else print m+0, mh+0 }' "$dir/ms-n$n.out" 2>/dev/null)
    if [ "$mc" = none ] || [ -z "$ms" ] || [ "$ms" = none ]; then
        echo "valgrind" > "$dir/n$n.vals"; return
    fi
    local a b d i p r pk pq
    read -r a b d i p r <<< "$mc"
    read -r pk pq <<< "$ms"
    echo "0 $a $b $pk $pq $d $i $p $r" > "$dir/n$n.vals"
}

# region_line <stderr file> -- the text after "region-stats: ", or "".
region_line() { sed -n 's/^region-stats: //p' "$1" | tail -1; }

# check_expect <probe> <dir> <n> <label 0|N> <do-stdout-check> -- "" when the
# run matches its annotations, else the reason.
check_expect() {
    local probe="$1" dir="$2" n="$3" which="$4" do_out="$5" want got
    if [ "$do_out" = 1 ]; then
        want=$(annot "$probe" "expect-$which")
        got=$(head -1 "$dir/n$n.stdout" | tr -d '\r')
        if [ -n "$want" ] && [ "$got" != "$want" ]; then
            echo "stdout at N=$n is '$got', expected '$want'"; return
        fi
    fi
    want=$(annot "$probe" "expect-regions-$which")
    if [ -n "$want" ]; then
        got=$(region_line "$dir/n$n.stderr")
        if [ "$got" != "$want" ]; then
            echo "region-stats at N=$n is '$got', expected '$want'"; return
        fi
    fi
}

# measure_probe <probe> -- one table row, written to $work/rows/<name>.
measure_probe() {
    local probe="$1" name dir n label guide status="ok" why="" checked
    name=$(basename "$probe" .tur)
    dir="$work/$name"; mkdir -p "$dir"
    label=$(annot "$probe" row); [ -z "$label" ] && label="$name"
    guide=$(annot "$probe" guide); [ -z "$guide" ] && guide="--"
    n=$(annot "$probe" measure | sed -n 's/^N=\([0-9][0-9]*\).*/\1/p')
    [ -z "$n" ] && n=10000
    checked=1
    if [ -n "$N_OVERRIDE" ]; then n="$N_OVERRIDE"; checked=0; fi

    measure_one "$probe" 0 "$dir"
    measure_one "$probe" "$n" "$dir"

    local v0 vN
    v0=$(cat "$dir/n0.vals"); vN=$(cat "$dir/n$n.vals")
    case "$v0" in 0\ *) ;; *) status="FAIL"; why="N=0: $v0 (see $dir)";; esac
    if [ "$status" = ok ]; then
        case "$vN" in 0\ *) ;; *) status="FAIL"; why="N=$n: $vN (see $dir)";; esac
    fi
    if [ "$status" = ok ]; then
        why=$(check_expect "$probe" "$dir" 0 0 1)
        [ -z "$why" ] && why=$(check_expect "$probe" "$dir" "$n" N "$checked")
        [ -n "$why" ] && status="FAIL"
    fi
    if [ "$status" = ok ]; then
        if [ -z "$(annot "$probe" expect-0)" ] || { [ "$checked" = 1 ] && [ -z "$(annot "$probe" expect-N)" ]; }; then
            status="unchecked"
        fi
    fi
    local regions
    regions=$(region_line "$dir/n$n.stderr" 2>/dev/null)
    case "$regions" in ""|"pushes=0 "*) regions="--" ;; esac

    if [ "$status" = FAIL ]; then
        {
            echo "measure.sh: FAIL $name -- $why"
            for lg in "$dir"/build-n*.log "$dir"/n*.stderr; do
                [ -s "$lg" ] && grep -m3 -iE 'error|panic|abort|fault' "$lg" | sed "s|^|  $(basename "$lg"): |"
            done
        } >&2
        printf '%s\t%s\t%s\t%s\t%s\n' "$name" "$label" "$n" "FAIL" "$why" > "$work/rows/$name"
        return
    fi
    echo "measure.sh: measured $name (N=$n)" >&2
    local x0 a0 b0 p0 q0 d0 i0 o0 r0 x1 a1 b1 p1 q1 d1 i1 o1 r1
    read -r x0 a0 b0 p0 q0 d0 i0 o0 r0 <<< "$v0"
    read -r x1 a1 b1 p1 q1 d1 i1 o1 r1 <<< "$vN"
    # Row fields (tab-separated), in this order -- the report below reads them:
    #   0 name  1 label  2 n  3 status  4 guide  5 regions
    #   6 allocs/elt  7 bytes/elt  8 peak/elt  9 peakreq/elt  10 deflost/elt  11 indlost/elt
    #   12.. the raw values at 0 and N: allocs bytes peak peakreq deflost indlost possible reachable
    awk -v name="$name" -v label="$label" -v guide="$guide" -v n="$n" -v st="$status" \
        -v reg="$regions" \
        -v a0="$a0" -v b0="$b0" -v p0="$p0" -v q0="$q0" -v d0="$d0" -v i0="$i0" -v o0="$o0" -v r0="$r0" \
        -v a1="$a1" -v b1="$b1" -v p1="$p1" -v q1="$q1" -v d1="$d1" -v i1="$i1" -v o1="$o1" -v r1="$r1" \
        'function per(x) {   # allocations per element: 2 places, more when tiny
            x = x / n
            if (x != 0 && x < 0.01 && x > -0.01) return sprintf("%.5f", x)
            return sprintf("%.2f", x)
         }
         function b(x) { return sprintf("%.1f", x / n) }
         BEGIN {
            OFS = "\t"
            print name, label, n, st, guide, reg,
                  per(a1 - a0), b(b1 - b0), b(p1 - p0), b(q1 - q0), b(d1 - d0), b(i1 - i0),
                  a0, a1, b0, b1, p0, p1, q0, q1, d0, d1, i0, i1, o0, o1, r0, r1
        }' > "$work/rows/$name"
}

mkdir -p "$work/rows"
echo "measure.sh: $("$TUR" --version 2>&1 | head -1); $($VALGRIND --version); ${#PROBES[@]} probe(s), -j$JOBS" >&2
for probe in "${PROBES[@]}"; do
    [ -f "$probe" ] || { echo "measure.sh: no such probe $probe" >&2; exit 2; }
    while [ "$(jobs -rp | wc -l)" -ge "$JOBS" ]; do wait -n; done
    measure_probe "$probe" &
done
wait

# The report, in the order the probes were given (the glob is sorted).
md="$work/memory-costs.md"
csv="$work/memory-costs.csv"
csvq() { local s="${1//\"/\"\"}"; printf '"%s"' "$s"; }
{
    echo "| Probe | Feature | N | allocs/elt | bytes/elt | peak/elt | peak req/elt | lost/elt | +ind/elt | regions | guide (2026-10-05) |"
    echo "| --- | --- | --: | --: | --: | --: | --: | --: | --: | --- | --- |"
} > "$md"
echo "probe,feature,n,status,allocs_per_elt,bytes_per_elt,peak_per_elt,peakreq_per_elt,deflost_per_elt,indlost_per_elt,allocs_0,allocs_n,bytes_0,bytes_n,peak_0,peak_n,peakreq_0,peakreq_n,deflost_0,deflost_n,indlost_0,indlost_n,possible_0,possible_n,reachable_0,reachable_n,region_stats_n,guide" > "$csv"
failed=0
for probe in "${PROBES[@]}"; do
    name=$(basename "$probe" .tur)
    IFS=$'\t' read -r -a f < "$work/rows/$name"
    if [ "${f[3]}" = FAIL ]; then
        failed=$((failed + 1))
        echo "| \`${f[0]}\` | ${f[1]} | ${f[2]} | FAIL | | | | | | | ${f[4]} |" >> "$md"
        # 23 empty columns: the 6 per-element values, the 16 raw ones, regions.
        echo "${f[0]},$(csvq "${f[1]}"),${f[2]},FAIL$(printf ',%.0s' $(seq 24))$(csvq "${f[4]}")" >> "$csv"
        continue
    fi
    mark=""; [ "${f[3]}" = unchecked ] && mark=" (stdout unchecked)"
    echo "| \`${f[0]}\` | ${f[1]}$mark | ${f[2]} | ${f[6]} | ${f[7]} | ${f[8]} | ${f[9]} | ${f[10]} | ${f[11]} | ${f[5]} | ${f[4]} |" >> "$md"
    raw=$(IFS=,; echo "${f[*]:12:16}")
    echo "${f[0]},$(csvq "${f[1]}"),${f[2]},${f[3]},${f[6]},${f[7]},${f[8]},${f[9]},${f[10]},${f[11]},$raw,$(csvq "${f[5]}"),$(csvq "${f[4]}")" >> "$csv"
done

cat "$md"
if [ -n "$OUT" ]; then
    mkdir -p "$OUT" && cp "$md" "$csv" "$OUT/" && echo "measure.sh: wrote $OUT/memory-costs.csv and $OUT/memory-costs.md" >&2
fi
[ "$failed" -gt 0 ] && { echo "measure.sh: $failed probe(s) FAILED" >&2; exit 1; }
exit 0
