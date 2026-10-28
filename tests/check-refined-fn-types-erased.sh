#!/usr/bin/env bash
# tests/check-refined-fn-types-erased.sh -- refined-function-types-plan F1's
# exit test, kept as a guard.
#
# A refinement written in a `(fn ...)` type rides on the TY_FN as a side-car
# that type identity never reads (plan D1): type_eq, unification, mangling, C
# typedef naming and the ABI cache all ignore it.  A missed erasure site would
# be QUIET -- a refinement changing a typedef name or which overload is picked
# -- so this compiles one program twice, with and without refinements in its
# function types, and requires the emitted C to be byte-identical.
#
# Under --no-contracts, because with contracts on the refinements are MEANT to
# change the C: a call through a refined function type gains its call-site
# checks (F2).  Without contracts nothing enforces them, so nothing may change.
# The unrefined program is also compiled with the gate off, so turning the
# experiment on is shown to change nothing for a program that does not use it.
#
# Needs a built ./build/tur (override with TUR=).  Skips if absent.
set -uo pipefail
cd "$(dirname "$0")/.."

TUR="${TUR:-./build/tur}"
if [ ! -x "$TUR" ]; then
    echo "SKIP refined-fn-types-erased -- $TUR not built"
    exit 0
fi

WORK="$(mktemp -d "${TMPDIR:-/tmp}/tur-rfn-erased.XXXXXX")"
trap 'rm -rf "$WORK"' EXIT
mkdir -p "$WORK/refined" "$WORK/plain" "$WORK/gate-off"

# The same basename in each directory: the emitted C carries it as a string.
cat > "$WORK/refined/prog.tur" <<'TUR'
(defn safe-div [a : int b : #refine{ v : int | (not= v 0) }] : int (/ a b))
(defn pos-of [x : int] : #refine{ r : int | (> r 0) } (if (> x 0) x (- 1 x)))
(defn apply2 [f : (fn [int #refine{ v : int | (not= v 0) }] int) x : int] : int
  (f 10 (+ x 1)))
(defn div-by [g : (fn [int] #refine{ r : int | (> r 0) }) x : int] : int
  (let [d (g x)] (safe-div 100 d)))
(defstruct Holder [f : (fn [#refine{ v : int | (> v 0) }] int)])
(def at-pos : (fn [int #refine{ v : int | (> v 0) }] int) safe-div)
(defn make [] : (fn [int #refine{ v : int | (> v 0) }] int) safe-div)
(defn main [] : int
  (println (apply2 safe-div 4))
  (println (div-by pos-of 3))
  (println ((.f (Holder (fn [x : int] : int (* x 2)))) 5))
  (let [h : (fn [int #refine{ v : int | (> v 0) }] int) safe-div]
    (println (h 9 3)))
  (println (at-pos 8 4))
  (println ((make) 6 2))
  0)
TUR
# The same program with every `#refine{ v : T | p }` inside a `(fn ...)` type
# replaced by its base type `T`.
sed -E 's/\(fn \[int #refine\{ v : int \| [^}]*\}\] int\)/(fn [int int] int)/g;
        s/\(fn \[int\] #refine\{ r : int \| [^}]*\}\)/(fn [int] int)/g;
        s/\(fn \[#refine\{ v : int \| [^}]*\}\] int\)/(fn [int] int)/g' \
    "$WORK/refined/prog.tur" > "$WORK/plain/prog.tur"
cp "$WORK/plain/prog.tur" "$WORK/gate-off/prog.tur"

if grep -q 'fn \[[^]]*#refine' "$WORK/plain/prog.tur" ||
   grep -q '(fn \[int\] #refine' "$WORK/plain/prog.tur" ||
   ! grep -q '(fn \[int int\] int)' "$WORK/plain/prog.tur"; then
    echo "FAIL refined-fn-types-erased -- the sed did not strip exactly the fn-type refinements"
    exit 1
fi

fail=0
emit() {   # emit <dir> <flags...>
    local d="$1"; shift
    if ! ( cd "$WORK/$d" && "$OLDPWD/$TUR" "$@" --no-contracts emit-c prog.tur \
               > prog.c 2> prog.err ); then
        echo "FAIL refined-fn-types-erased -- emit-c failed for $d:"
        cat "$WORK/$d/prog.err"
        fail=1
    fi
}
emit refined --enable=refined-fn-types
emit plain --enable=refined-fn-types
emit gate-off
[ "$fail" = 0 ] || exit 1

if ! cmp -s "$WORK/refined/prog.c" "$WORK/plain/prog.c"; then
    echo "FAIL refined-fn-types-erased -- refinements in function types changed the emitted C:"
    diff "$WORK/plain/prog.c" "$WORK/refined/prog.c" | head -20
    exit 1
fi
if ! cmp -s "$WORK/plain/prog.c" "$WORK/gate-off/prog.c"; then
    echo "FAIL refined-fn-types-erased -- --enable=refined-fn-types changed the C of a program that writes no refined function type:"
    diff "$WORK/gate-off/prog.c" "$WORK/plain/prog.c" | head -20
    exit 1
fi
echo "PASS refined-fn-types-erased"
