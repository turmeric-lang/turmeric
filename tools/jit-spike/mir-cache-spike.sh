#!/usr/bin/env bash
# tools/jit-spike/mir-cache-spike.sh -- performance-audit-plan E-2.
#
# For each program: dump the unit `tur jit` hands c2mir (TUR_JIT_DUMP_C), pruned
# and with TUR_JIT_NO_PRUNE=1, then time c2mir on it against MIR_read of the
# MIR image it produces (mir-cache-spike.c).  One TSV row per program and mode.
#
# Usage:
#   TUR=./build-release/tur bash tools/jit-spike/mir-cache-spike.sh prog.tur...
#
# TUR must be a Release -DTUR_JIT=ON build; the harness links that build's
# libtur_mir.a, so the MIR it measures is the MIR `tur jit` runs.  Each program
# is RUN once per mode to get its dump (stdin /dev/null, 120 s cap).

set -u
cd "$(dirname "$0")/../.."
TUR="${TUR:-./build-release/tur}"
OUT="${OUT:-/tmp/mir-cache-spike}"
CC="${CC:-cc}"
LIBMIR="$(dirname "$TUR")/libtur_mir.a"
[ -f "$LIBMIR" ] || LIBMIR="$(dirname "$TUR")/src/libtur_mir.a"
[ -x "$TUR" ] || { echo "mir-cache-spike: no tur at $TUR" >&2; exit 2; }
[ -f "$LIBMIR" ] || { echo "mir-cache-spike: no $LIBMIR (configure -DTUR_JIT=ON)" >&2; exit 2; }
mkdir -p "$OUT"

# The engine's prelude, verbatim, so the harness compiles exactly what tur does.
awk '/^static const char JIT_PRELUDE\[\] =/ {on=1} on {print} on && /";[[:space:]]*$/ {exit}' \
    src/jit_engine.c > "$OUT/jit-prelude.h"
grep -q 'TUR_JIT_ENGINE' "$OUT/jit-prelude.h" ||
    { echo "mir-cache-spike: could not extract JIT_PRELUDE from src/jit_engine.c" >&2; exit 2; }

"$CC" -O2 -std=gnu11 -Wall -I external/mir -I external/mir/c2mir -I "$OUT" \
      -o "$OUT/mir-cache-spike" tools/jit-spike/mir-cache-spike.c "$LIBMIR" \
      -ldl -lm -lpthread || exit 2

printf 'program\tmode\tunit_B\tbmir_B\tfuncs\tc2mir_ms\twrite_ms\tread_ms\tready_c2mir_ms\tready_read_ms\tspeedup\tfirst_ready_c2mir_ms\tfirst_ready_read_ms\ttur_c2mir_ms\n'
for prog in "$@"; do
    # A fixture's file is input.tur; name it by its directory instead.
    name=$(basename "$prog" .tur)
    [ "$name" = input ] && name=$(basename "$(dirname "$prog")")
    for mode in pruned unpruned; do
        unit="$OUT/$name.$mode.c"
        rm -f "$unit"
        if [ "$mode" = unpruned ]; then np=1; else np=0; fi
        TUR_JIT_NO_PRUNE=$np TUR_JIT_DUMP_C="$unit" TUR_JIT_SPLIT_DEBUG="$OUT/probe.c" \
            TUR_JIT_TIMING=1 timeout 120 "$TUR" jit "$prog" </dev/null >/dev/null 2>"$OUT/$name.$mode.err"
        [ -s "$unit" ] || { echo "mir-cache-spike: $prog ($mode): no dump" >&2; continue; }
        # The include dirs tur gave c2mir, from its jit-debug line.
        incs=()
        while read -r d; do incs+=(-I "$d"); done < <(
            grep -m1 '^jit-debug: n_incs=' "$OUT/$name.$mode.err" | grep -o '\[[^]]*\]' | tr -d '[]')
        tur_c2mir=$(awk -F'\t' '$1=="TUR_JIT_TIMING" && $3=="c2mir" {print $4; exit}' "$OUT/$name.$mode.err")
        res=$("$OUT/mir-cache-spike" "$unit" "$OUT/$name.$mode.bmir" "${incs[@]}" 2>"$OUT/$name.$mode.spike.err") ||
            { echo "mir-cache-spike: $prog ($mode): harness failed, see $OUT/$name.$mode.spike.err" >&2; continue; }
        echo "$res" | awk -F'\t' -v p="$name" -v m="$mode" -v t="${tur_c2mir:--}" '
            NR==1 {u=$2; b=$6; f=$8}
            NR==2 {c=$2; w=$4; r=$6}
            NR==3 {rc=$2; rr=$4; s=$6}
            NR==4 {fc=$2; fr=$4}
            END {printf "%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n", p, m, u, b, f, c, w, r, rc, rr, s, fc, fr, t}'
    done
done
