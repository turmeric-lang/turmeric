#!/usr/bin/env bash
# stdlib-list-null-check-retires-regions: `tnil?` and `tlength` test a typed
# `(Cons A)` for the empty link with `(= (:: l :int) 0)`.  That erasing
# ascription used to note the node as an escape, so every `with-region` that
# walked a stdlib list RETIRED instead of rewinding.  A word compared with 0
# and dropped goes nowhere, so that one ascription is no longer noted
# (emit_builtin's region_erasure_compare_only).
#
# Three programs, each run with TUR_REGION_STATS=1 (one line at exit):
#   walk     -- tlength over a tcons-of list, 5 brackets  -> 5 rewinds
#   null     -- tnil? on a tcons-of list, 5 brackets      -> 5 rewinds
#   control  -- the same erasure used in ARITHMETIC, whose
#               word could go anywhere                    -> 5 retires
# The control is what keeps the first two from being vacuous: the erasure
# note still fires for every use but the null test.  Answers are printed too.
set -u
TMP="$1"
TUR="${TUR:-./build/tur}"

cat > "$TMP/build.tur" <<'EOF'
(defn build [n : int acc : (Cons int)] : (Cons int)
  (if (= n 0) acc (build (- n 1) (tcons-of n acc))))
EOF

prog() {   # prog <name> <bracket-body>
    cat > "$TMP/$1.tur" <<EOF
(load "$TMP/build.tur")
(defn rounds [k : int acc : int] : int
  (if (= k 0) acc
    (rounds (- k 1) (+ acc (with-region (fn [] : int $2))))))
(defn main [] : int (println (rounds 5 0)) 0)
EOF
    "$TUR" build "$TMP/$1.tur" -o "$TMP/$1" > "$TMP/$1.build" 2>&1 \
        || { echo "$1: build failed: $(grep -m1 -i error "$TMP/$1.build")"; return; }
    out="$(TUR_REGION_STATS=1 "$TMP/$1" 2> "$TMP/$1.err")"
    echo "$1: $out $(grep -o 'rewinds=[0-9]* retires=[0-9]*' "$TMP/$1.err")"
}

prog walk    '(tlength (build 1000 (tnil)))'
prog null    '(if (tnil? (build 1000 (tnil))) 0 1)'
prog control '(if (= (+ (:: (build 1000 (tnil)) :int) 0) 0) 0 1)'
exit 0
