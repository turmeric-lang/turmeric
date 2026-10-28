#!/usr/bin/env bash
# RM3 R5 graduation item 2, fifth batch: PARAMETRIC results.  The static
# escape walk used to refuse every TY_APP but `(Vec <scalar>)` on sight; it
# now (a) substitutes a monomorph's type arguments into its field types before
# asking whether a field reaches a node, and (b) treats the four malloc-backed
# collections (Vec / Map / Set / MutableMap) as reaching exactly what their
# ELEMENT types reach.  One shape per defn, each pinned by its retire/rewind
# verdict AND its value read back after the pop -- the SR4 lesson: a rewind
# of something still live prints stale words here, never a link error.
#
# ADMITTED (must REWIND -- tur_region_pop_checked):
#   p-pair   `(Pair int int)`: stdlib's defstruct Pair [A B] with A, B := int;
#            the substituted fields are scalars.
#   p-opt    `(Option int)`: a parametric SUM -- every ctor's substituted
#            fields are walked; `(Some :A)` with A := int reaches nothing.
#   p-nest   `(Pair (Pair int int) int)`: the def is its own type argument.
#            Arguments are walked BEFORE the def goes on the cycle path, so
#            nesting is not read as a cycle; the by-value child sits in a
#            plain-malloc box (the byval<->carrier field bridge).
#   v-pair   `(Vec (Pair int int))`: a Vec of BY-VALUE AGGREGATES.  The push
#            bridge boxes each element with plain malloc; the box reaches what
#            the pair's fields reach, which is nothing.
#   m-int    `(Map int int)`: handle and HAMT nodes are inline-C / runtime
#            malloc (map.tur, src/runtime/hamt.c); int64 keys and values.
#   s-int    `(Set int)`: same, set.tur.
#
# REFUSED (must RETIRE -- tur_region_pop):
#   p-link   `(Pair Link int)`: the substituted `fst` IS the `:heap` node.
#   o-link   `(Option Link)`: same, through a sum.
#   m-link   `(Map int Link)`: the HAMT's values are pointers into the
#            generation.
#
# The retired shapes are RUN too (their values are identical to the flag-off
# program by construction); the counts are the ratchet.
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

(defn p-pair [n : int] : (Pair int int)
  (with-region (fn [] (pair (chain-sum (build n 0)) 7))))

(defn p-opt [n : int] : (Option int)
  (with-region (fn [] (some (chain-sum (build n 0))))))

(defn p-nest [n : int] : (Pair (Pair int int) int)
  (with-region (fn [] (pair (pair (chain-sum (build n 0)) 1) 2))))

(defn v-pair [n : int] : (Vec (Pair int int))
  (with-region (fn []
    (let [v (:: (vec-new) (Vec (Pair int int)))]
      (vec-push! v (pair (chain-sum (build n 0)) 3))
      v))))

(defn m-int [n : int] : (Map int int)
  (with-region (fn [] (map-assoc (:: (map-new) (Map int int)) 1 (chain-sum (build n 0))))))

(defn s-int [n : int] : (Set int)
  (with-region (fn [] (set-add (:: (set-new) (Set int)) 10 (chain-sum (build n 0))))))

(defn p-link [n : int] : (Pair Link int)
  (with-region (fn [] (pair (Link n 0) (chain-sum (build n 0))))))

(defn o-link [n : int] : (Option Link)
  (with-region (fn [] (some (Link (chain-sum (build n 0)) 0)))))

(defn m-link [n : int] : (Map int Link)
  (with-region (fn [] (map-assoc (:: (map-new) (Map int Link)) 1 (Link (chain-sum (build n 0)) 0)))))

(defn main [] : int
  (println (.fst (p-pair 4)))                       ;; 10
  (match (p-opt 4) (Some x) (println x) (None) (println -1))   ;; 10
  (println (.fst (.fst (p-nest 4))))                ;; 10
  ;; `vec-get` + ascription; `vec-get-byval` works too since
  ;; docs/archive/vec-get-byval-struct-element-returns-carrier.md was fixed, and
  ;; its own fixture pins that spelling -- this one keeps the read it was
  ;; written with.
  (println (.fst (:: (vec-get (v-pair 4) 0) (Pair int int))))   ;; 10
  (println (:: (map-get (m-int 4) 1) :int))         ;; 10
  (println (set-member? (s-int 4) 10 10))           ;; true (retire/rewind-neutral)
  (println (.snd (p-link 4)))                       ;; 10 (retired)
  (match (o-link 4) (Some l) (println (.v l)) (None) (println -1))  ;; 10 (retired)
  (println (.v (:: (map-get (m-link 4) 1) Link)))   ;; 10 (retired)
  0)
EOF

"$TUR" emit-c "$TMP/in.tur" 2>"$TMP/emit.err" > "$TMP/out.c"
if grep -q 'tur_region_push()' "$TMP/out.c"; then echo "bracket: opened"; else echo "bracket: MISSING"; cat "$TMP/emit.err" | head -5; fi
retire=$(sed -n '/end of fixed runtime preamble/,$p' "$TMP/out.c" | grep -c 'tur_region_pop(')
rewind=$(sed -n '/end of fixed runtime preamble/,$p' "$TMP/out.c" | grep -c 'tur_region_pop_checked(')
echo "retire=$retire rewind=$rewind"
echo "regions off: $(TUR_REGIONS=0 "$TUR" run "$TMP/in.tur" 2>/dev/null | tr '\n' ' ')"
echo "regions on:  $("$TUR" run "$TMP/in.tur" 2>/dev/null | tr '\n' ' ')"
exit 0
