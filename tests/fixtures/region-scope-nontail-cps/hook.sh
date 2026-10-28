#!/usr/bin/env bash
# RM3 R5 graduation item 3, the CPS CT_LETCALL arm: R4 left it with NO region
# bracket, deliberately -- every probed shape lowered to CT_TAILCALL, so a copy
# there would have been untested safety code.  This pins WHY it is unreachable
# for a region-scope callee, so the arm can stay bracket-free by proof rather
# than by absence of a probe.
#
# The IR builder (cps_ir.c) emits CT_LETCALL only for an UNCOLORED callee; a
# colored callee always becomes CT_TAILCALL (with a join continuation when the
# call is not in tail position).  Both region scopes -- bt-scope and
# with-region -- are colored, because each invokes a fat thunk.  So a region
# bracket can never land on the LETCALL arm.
#
# Asserted here with `with-region` (the newer of the two; R4 probed bt-scope)
# in the two NON-tail positions that would produce a LETCALL for an uncolored
# callee -- let-bound and used, and as an arithmetic operand -- inside a
# function that is CPS-lowered (its __cps twin is emitted).  Both calls must
# carry a bracket, and the values must survive.
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
;; let-bound (non-tail) then used; and as an operand (non-tail).
(defn two [n : int] : int
  (let [a (with-region (fn [] (chain-sum (build n 0))))]
    (+ a (with-region (fn [] (chain-sum (build (* 2 n) 0)))))))
(defn main [] : int (println (two 4)) 0)   ;; 10 + 36 = 46
EOF

"$TUR" emit-c "$TMP/in.tur" 2>/dev/null > "$TMP/out.c"
echo "caller cps-lowered: $(grep -c 'two__cps' "$TMP/out.c" | awk '{print ($1>0)?"yes":"no"}')"
# Count in the PROGRAM half only (after the preamble marker): the preamble
# now carries src/runtime/region.{h,c} verbatim (regions on by default,
# self-contained emitted C), whose declarations, definitions and prose
# would otherwise be counted as bracket sites.
echo "push=$(sed -n '/end of fixed runtime preamble/,$p' "$TMP/out.c" | grep -c 'tur_region_push()') rewind=$(sed -n '/end of fixed runtime preamble/,$p' "$TMP/out.c" | grep -c 'tur_region_pop_checked(') retire=$(sed -n '/end of fixed runtime preamble/,$p' "$TMP/out.c" | grep -c 'tur_region_pop(')"
echo "letcall to with-region: $("$TUR" --dump-cps check "$TMP/in.tur" 2>&1 | grep -ci 'letcall.*with-region')"
echo "regions off: $(TUR_REGIONS=0 "$TUR" run "$TMP/in.tur" 2>/dev/null | tr '\n' ' ')"
echo "regions on:  $("$TUR" run "$TMP/in.tur" 2>/dev/null | tr '\n' ' ')"
exit 0
