#!/usr/bin/env bash
# region-bracket-lost-when-bt-scope-specializes / region-walk-refuses-every-adt-result:
# a `bt-scope` whose result is an ADT must OPEN a region, must hand back a value
# that outlives the pop, and must rewind exactly the shapes the static walk can
# prove -- no more.
#
# Three things this asserts that an output-only fixture cannot:
#
#   1. The bracket EXISTS.  A non-scalar result made the call resolve to an ABI
#      specialization and take the CPS emitter's `/* cps->cps */` arm, which
#      carries no push/pop -- so the program compiled, ran, printed the right
#      answer, and silently never opened a region.  Zero diagnostic, zero
#      output difference.  Only the emitted C shows it, so that is what is
#      grepped.
#
#   2. The retire/rewind SPLIT is exactly the proven set.  `pick` returns a
#      field-less enum -- by value, reaches nothing -- and must REWIND
#      (tur_region_pop_checked); this is the shape the bare-TY_ADT lookup
#      admitted, and it retired before that change.  `one-round` returns
#      `(RIP :int :int)`, and NOW REWINDS too: the scalar-field widening
#      (region-walk-refuses-every-adt-result, resolved) reads the declared
#      FORM `:int` and proves the result reaches no node, where the old walk
#      refused on the field's NULL full_type.  `mutual` returns a spine reached
#      through a cycle and still must RETIRE (tur_region_pop) -- its `:MA` field
#      is not a scalar keyword, so the widening leaves it refused.  The counts
#      are asserted (retire=1 rewind=2), so a future widening that admits
#      `mutual` shows up as a changed number here AND as a wrong answer in (3).
#
#   3. The values SURVIVE.  This is the SR4 lesson, and the reclamation plan
#      says it applies to regions with more force: a fixture that only checks
#      the program builds would pass with a rewind that reclaimed memory still
#      in use, because the wrong answer is a stale read, not a link error.
#      `mutual` is the case that actually produced one during this work -- a
#      mutually-recursive result whose spine a kind-based field accept wrongly
#      proved safe, printing 0 instead of 42.
set -u
TMP="$1"
TUR="${TUR:-./build/tur}"

cat > "$TMP/in.tur" <<'EOF'
(load "stdlib/trail.tur")

(defdata ^heap Link (Link [v : int nxt : int]))
(defdata RPair (RIP :int :int))
(defdata Color (Red) (Green) (Blue))

;; Mutually recursive, and NEITHER def is self-recursive -- so the walk's
;; is_self_recursive guard does not see it.
(defdata ^copy MB (MBnil) (MBcons :int :MA))
(defdata ^copy MA (MAnil) (MAcons :int :MB))

(defn build [n : int acc : int] : int
  (if (<= n 0) acc (build (- n 1) (:: (Link n acc) :int))))

(defn chain-sum [c : int] : int
  ```c
  struct { int64_t v; int64_t nxt; } *p = (void *)(intptr_t)c;
  int64_t acc = 0;
  while (p) { acc += p->v; p = (void *)(intptr_t)p->nxt; }
  return acc;
  ```)

;; The bracket that used to vanish: a record result, not a scalar.  RETIRES.
(defn one-round [n : int] : RPair
  (bt-scope (fn [] (RIP n (chain-sum (build n 0))))))

;; A field-less enum result: provable, so REWINDS -- and the spine built inside
;; is reclaimed under it.  The value read back is what the walk promised.
(defn pick [n : int] : Color
  (bt-scope (fn [] (if (> (chain-sum (build n 0)) 5) (Green) (Red)))))

(defn mkA [n : int] : MA (if (<= n 0) (MAnil) (MAcons n (MBcons n (mkA (- n 1))))))
(defn sumA [a : MA acc : int] : int
  (match a (MAnil) acc
           (MAcons x b) (match b (MBnil) (+ acc x)
                                 (MBcons y r) (sumA r (+ acc (+ x y))))))

;; The result IS the spine, reached through a cycle rather than a self-link.
;; RETIRES.
(defn mutual [n : int] : MA (bt-scope (fn [] (mkA n))))

(defn main [] : int
  (match (one-round 4) (RIP a b) (println (+ a b)))            ;; 4 + 10 = 14
  (println (sumA (mutual 6) 0))                               ;; 2*(1+..+6) = 42
  (match (pick 4) (Red) (println 0) (Green) (println 1) (Blue) (println 2))  ;; 10 > 5: 1
  (match (pick 1) (Red) (println 0) (Green) (println 1) (Blue) (println 2))  ;; 1 > 5: 0
  0)
EOF

"$TUR" emit-c "$TMP/in.tur" 2>/dev/null > "$TMP/out.c"

if grep -q 'tur_region_push()' "$TMP/out.c"; then
    echo "bracket: opened"
else
    echo "bracket: MISSING (the bt-scope emitted no region)"
fi

# The extern declarations spell `tur_region_pop(int depth)` and would count.
# Count in the PROGRAM half only (after the preamble marker): the preamble
# now carries src/runtime/region.{h,c} verbatim (regions on by default,
# self-contained emitted C), whose declarations, definitions and prose
# would otherwise be counted as bracket sites.
retire=$(sed -n '/end of fixed runtime preamble/,$p' "$TMP/out.c" | grep -c 'tur_region_pop(')
rewind=$(sed -n '/end of fixed runtime preamble/,$p' "$TMP/out.c" | grep -c 'tur_region_pop_checked(')
echo "retire=$retire rewind=$rewind"

echo "regions off: $(TUR_REGIONS=0 "$TUR" run "$TMP/in.tur" 2>/dev/null | tr '\n' ' ')"
echo "regions on:  $("$TUR" run "$TMP/in.tur" 2>/dev/null | tr '\n' ' ')"
exit 0
