#!/usr/bin/env bash
# bench-refined-fn-result-check.sh -- refined-function-types-plan F4: what does
# the check on a call through a refined function type cost?
#
# One hot loop, `n` calls through a function-typed parameter `g`, compiled once
# per spelling of `g`'s type:
#
#   plain    (fn [int] int)                          -- the baseline
#   result   (fn [int] #refine{ r : int | (> r 0) }) -- F2's post-call check
#   param    (fn [#refine{ v : int | (>= v 0) }] int) -- an argument check the
#            solver cannot prove (`i` is a mutated loop counter), so it is kept
#   wrapper  plain type, but `g` is a checking closure around the real
#            function -- what F4 option 2 (wrap at the coercion site) would pay
#            for every coercion it cannot prove
#
# The real function behind `g` is chosen at run time, so the C compiler cannot
# inline or devirtualize the call.  Built with `--keep-contracts`: a Release
# `tur` strips contract checks without it, so by default none of these cost
# anything in a release build.
#
# Every function that calls through a function value is CPS-colored today
# (cps-dk-internals-guide, "Over-coloring (R-1)"), so these loops measure the
# check inside the DK machinery.  The same script then times the emitted check
# sequence on a direct loop (refined-fn-result-check-isolated.c), which is what
# it will cost once R-1 is narrowed.  Results and the decision they support:
# benchmarks/refined-fn-result-check-results.md.
#
# usage: bash benchmarks/bench-refined-fn-result-check.sh [TUR] [N] [REPS]
#        (ISO_N=<calls> sizes the isolated run; default 1e9)
set -euo pipefail
cd "$(dirname "$0")/.."
TUR="${1:-./build-release/tur}"
N="${2:-200000000}"
REPS="${3:-5}"
WORK="$(mktemp -d "${TMPDIR:-/tmp}/tur-f4-bench.XXXXXX")"
trap 'rm -rf "$WORK"' EXIT

gen() {   # gen <variant> <type of g> <how main passes the function>
    cat > "$WORK/$1.tur" <<TUR
(load "stdlib/args.tur")

(defn inc1 [x : int] : int (+ x 1))
(defn inc2 [x : int] : int (+ x 2))

(defn sum-through [g : $2 n : int] : int
  (let [^mut acc 0
        ^mut i   0]
    (while (< i n)
      (set! acc (+ acc (g i)))
      (set! i (+ i 1)))
    acc))

(defn main [] : int
  (let [spec (args/spec-option (args/spec-new) "--n" "int" (some "1000"))
        res  (args/parse spec *args*)
        n    (args/get-int res "n")
        f    (if (> n 1) inc1 inc2)]
    (println (sum-through $3 n)))
  0)
TUR
}
gen plain   "(fn [int] int)"                          "f"
gen result  "(fn [int] #refine{ r : int | (> r 0) })" "f"
gen param   "(fn [#refine{ v : int | (>= v 0) }] int)" "f"
gen wrapper "(fn [int] int)" \
    "(fn [x : int] : int (let [r (f x)] (if (> r 0) r (panic \"result check\"))))"

for v in plain result param wrapper; do
    "$TUR" --enable=refined-fn-types --keep-contracts build "$WORK/$v.tur" \
        -o "$WORK/$v" 2> "$WORK/$v.log" || { cat "$WORK/$v.log"; exit 1; }
done

median() { sort -n | awk '{a[NR]=$1} END {print a[int((NR+1)/2)]}'; }
echo "n=$N reps=$REPS tur=$TUR"
printf '%-8s %10s %10s %8s\n' variant "median ms" "ns/call" "vs plain"
base=""
for v in plain result param wrapper; do
    ms=$(for _ in $(seq "$REPS"); do
             s=$(date +%s%N); "$WORK/$v" --n "$N" > /dev/null; e=$(date +%s%N)
             echo $(( (e - s) / 1000000 ))
         done | median)
    [ -z "$base" ] && base=$ms
    printf '%-8s %10s %10s %8s\n' "$v" "$ms" \
        "$(awk -v m="$ms" -v n="$N" 'BEGIN{printf "%.2f", m*1e6/n}')" \
        "$(awk -v m="$ms" -v b="$base" 'BEGIN{printf "%.2fx", m/b}')"
done

echo
echo "isolated (direct loop, cc -O2, best of 5):"
cc -O2 -o "$WORK/iso" benchmarks/refined-fn-result-check-isolated.c
"$WORK/iso" "${ISO_N:-1000000000}"
