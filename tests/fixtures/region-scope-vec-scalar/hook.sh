#!/usr/bin/env bash
# RM3 R5 graduation item 2, fourth batch: a `Vec` of SCALARS returned from a
# region bracket REWINDS -- the plan's own named example of an over-
# conservative refusal ("a bracket returning a Vec of scalars never rewinds").
#
# Why it was refused: a parametric monomorph `(Vec int)` is a TY_APP, so it
# never reached the walk's TY_ADT arm and fell to `default:`.  Why it is safe
# to admit, one reason per lock:
#   (1) Vec's OWN storage -- handle and element buffer -- is plain inline-C
#       malloc in stdlib/vec.tur, never the region router (which is emitted
#       only at the four ADT-ctor sites).  So the escaping handle is never
#       region memory: the runtime lock (tur_region_note_escape) is satisfied.
#       A compiler-warranted name check, the option-niche plan's warrant.
#   (2) The ELEMENTS are what could reach, and a scalar element is an int64
#       word in that malloc'd buffer: nothing to reach.
#
# What it must NOT admit (the negative): a Vec of `:heap` nodes.  Its buffer
# holds pointers INTO the generation, so the walk refuses on the element and
# the bracket retires -- byte-for-byte the flag-off program, safe to run.
#
# The SR4 lesson, with more force than usual: the elements are read back
# AFTER the pop.  A rewind that reclaimed the buffer (or a Vec whose storage
# had quietly become region-routed) would print stale words here, not a link
# error -- and under the Debug poison would trap.  So the values, not merely
# the build, are the assertion.
set -u
TMP="$1"
TUR="${TUR:-./build/tur}"

cat > "$TMP/in.tur" <<'EOF'
(defdata ^heap Link (Link [v : int nxt : int]))
(defn build [n : int acc : int] : int
  (if (<= n 0) acc (build (- n 1) (:: (Link n acc) :int))))
(defn chain-sum [c : int] : int
  ```c
  struct { int64_t v; int64_t nxt; } *p = (void *)(intptr_t)c;
  int64_t acc = 0; while (p) { acc += p->v; p = (void *)(intptr_t)p->nxt; } return acc;
  ```)

;; ADMIT: a Vec of scalars built inside the region; the spines that produced
;; its elements are reclaimed, the Vec (malloc) and its words survive.
(defn v-int [n : int] : (Vec int)
  (with-region (fn []
    (let [v (vec-new)]
      (vec-push! v (chain-sum (build n 0)))
      (vec-push! v (chain-sum (build (* 2 n) 0)))
      v))))

;; REFUSE: a Vec of :heap nodes -- the buffer holds region pointers.  RETIRES.
(defn v-link [n : int] : (Vec Link)
  (with-region (fn []
    (let [v (vec-new)]
      (vec-push! v (Link n 0))
      v))))

(defn main [] : int
  (let [v (v-int 4)]
    (println (vec-len v))              ;; 2
    (println (:: (vec-get v 0) int))   ;; 1+2+3+4 = 10
    (println (:: (vec-get v 1) int)))  ;; 1+..+8  = 36
  (let [w (v-link 3)]
    (println (vec-len w)))             ;; 1 (retired: identical to flag-off)
  0)
EOF

"$TUR" emit-c "$TMP/in.tur" 2>/dev/null > "$TMP/out.c"
if grep -q 'tur_region_push()' "$TMP/out.c"; then echo "bracket: opened"; else echo "bracket: MISSING"; fi
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
