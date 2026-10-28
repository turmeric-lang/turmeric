#!/usr/bin/env bash
# RM3 R5 graduation item 2 (regions-plan.md): the static escape walk admits
# shapes ONE AT A TIME, each pinned here with its retire/rewind verdict AND its
# value read back after the pop.  This is the second batch, after the
# scalar-field ADT result that region-scope-adt-result pins.
#
# ADMITTED (must REWIND -- tur_region_pop_checked):
#   r-struct  a `defstruct` with scalar fields.  Every defstruct lowers to a
#             record ADT (structdef-retirement DS-D), so it reaches the same
#             TY_ADT arm as a `defdata` and its `[x : int y : float]` fields are
#             scalar keywords the form-based widening admits.  Nothing special
#             was built for it; this pins that it stays admitted.
#   r-cstr    a variant holding a `:cstr`.  A C string is never region memory
#             (no path routes a char buffer through the generation allocator),
#             and `cstr` is on the scalar-keyword list.
#   r-enum    a variant holding a field-less by-value enum.  The form `Color` is
#             NOT a scalar keyword, so it is admitted the other way: the field's
#             `full_type` is recorded (record_full in resolve_ctor_field) and the
#             walk descends into Color, whose every ctor is field-less and so
#             reaches nothing.  That is the inference: nothing else could have
#             let it through.
#   r-sum     a variant holding a non-recursive multi-variant SUM OF SCALARS,
#             `(Circle :float) (Sq :float :float)`.  Same route as r-enum, and
#             this is the one worth knowing the name of: adt_is_byvalue_product
#             (types.c) admits an SR1 by-value sum candidate and walks every
#             variant's fields, so `Shape` counts as "by-value product" for
#             record_full, gets a full_type, and the walk descends into it.
#
# REFUSED (must RETIRE -- tur_region_pop):
#   r-bad     a variant holding a sum ONE OF WHOSE ARMS holds a `:heap` node.
#             The walk descends into the sum and refuses at that arm: a union
#             is only as safe as its widest arm.
#   r-rec     a variant holding a SELF-RECURSIVE sum.  Its recursive field's
#             form names the def, which is never a scalar keyword, so it refuses
#             -- the spine is exactly what a rewind must not reclaim.
#   r-heap    a variant HOLDING a `:heap` node.  The result reaches region
#             memory, so the walk refuses on `is_heap`.  It was once DEFINED but
#             not called: its int field read back as garbage through the
#             bracket, because the direct-path spec matcher handed the call the
#             sibling by-value record's clone (docs/archive/capturing-thunk-
#             returning-heap-field-record-garbles-int.md, fixed 2026-09-05).
#             Now run like the others; its 10 is the retired-region value.
#
# The counts are the ratchet: a widening that admits `r-heap` shows up as a
# changed number here AND, because a retired region reclaims nothing, could
# only ever turn into a wrong answer in the run lines below -- which is the SR4
# lesson this whole family asserts values for.  region-scope-adt-result keeps
# the mutual-recursion negative; this file does not duplicate it.
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

(defstruct P2 [x : int y : float])
(defn r-struct [n : int] : P2
  (with-region (fn [] (make-struct P2 :x (chain-sum (build n 0)) :y 1.5))))

(defdata Named (N :int :cstr))
(defn r-cstr [n : int] : Named
  (with-region (fn [] (N (chain-sum (build n 0)) "hi"))))

(defdata Color (Red) (Green) (Blue))
(defdata Tag (T :int :Color))
(defn r-enum [n : int] : Tag
  (with-region (fn [] (T (chain-sum (build n 0)) (Green)))))

(defdata Shape (Circle :float) (Sq :float :float))
(defdata HoldsShape (HSh :int :Shape))
(defn r-sum [n : int] : HoldsShape
  (with-region (fn [] (HSh (chain-sum (build n 0)) (Circle 1.5)))))

(defdata BadShape (BCircle :float) (BLink :Link))
(defdata HoldsBad (HB :int :BadShape))
(defn r-bad [n : int] : HoldsBad
  (with-region (fn [] (HB (chain-sum (build n 0)) (BCircle 1.5)))))

(defdata RShape (RLeaf :float) (RNode :RShape))
(defdata HoldsR (HR :int :RShape))
(defn r-rec [n : int] : HoldsR
  (with-region (fn [] (HR (chain-sum (build n 0)) (RLeaf 1.5)))))

(defdata HoldsLink (HL :int :Link))
(defn r-heap [n : int] : HoldsLink
  (with-region (fn [] (HL (chain-sum (build n 0)) (Link n 0)))))

(defn main [] : int
  (match (r-struct 4) (P2 a b) (println a))   ;; 10
  (match (r-cstr 4)   (N a s)  (println a))   ;; 10
  (match (r-enum 4)   (T a c)  (println a))   ;; 10
  (match (r-sum 4)    (HSh a s) (println a))  ;; 10
  (match (r-bad 4)    (HB a b)  (println a))  ;; 10 (retired: identical to flag-off)
  (match (r-rec 4)    (HR a r)  (println a))  ;; 10 (retired: identical to flag-off)
  (match (r-heap 4)   (HL a l)  (println a))  ;; 10 (retired: the result holds a node)
  0)
EOF

"$TUR" emit-c "$TMP/in.tur" 2>/dev/null > "$TMP/out.c"

if grep -q 'tur_region_push()' "$TMP/out.c"; then
    echo "bracket: opened"
else
    echo "bracket: MISSING"
fi
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
