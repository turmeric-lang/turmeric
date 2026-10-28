#!/usr/bin/env bash
# tests/set-bang-rc-release-check.sh -- set-bang-rc-release
# (docs/archive/history/set-bang-does-not-release-old-rc-value.md).
#
# Every ownership shape of `(set! b v)` and `(set! (.f b) v)` on an rc binding,
# built under AddressSanitizer and checked for BOTH leaks (live rc blocks once
# the scope has closed) and memory errors.
#
# The fixture suite pins the common shapes; this pins the whole matrix under a
# sanitizer, including the two that deliberately still leak. Opt-in: a sanitized
# compile per case is slow.
#
# Exit status: 0 if every case holds, 1 otherwise.
S="${TMPDIR:-/tmp}/set-bang-check.$$"; mkdir -p "$S"; trap 'rm -rf "$S"' EXIT
TUR=./build/tur
CC_FLAGS="-O2 -std=c99 -Wall -fno-strict-aliasing -fsanitize=address -g"
pass=0; fail=0

prelude='(defstruct ^move Node [next : rc<Node>])
(defn null-rc-node [] : ptr<void>
  ```c
  return NULL;
  ```)
(defn live [] : int
  ```c
  extern uint32_t gc_all_blocks_count;
  return (int64_t)gc_all_blocks_count;
  ```)
(defn fresh [] : rc<Node>
  (rc/of (make-struct Node (null-rc-node))))'

check() { # name expected body...
  local name="$1" expect="$2" prog="$3"
  printf '%s\n%s\n' "$prelude" "$prog" > "$S/sc.tur"
  if ! TUR_CC_FLAGS="$CC_FLAGS" "$TUR" build "$S/sc.tur" -o "$S/sc" >"$S/sc.err" 2>&1; then
    echo "FAIL $name -- compile error"; sed 's/^/    /' "$S/sc.err" | head -4; fail=$((fail+1)); return
  fi
  local out
  out=$(ASAN_OPTIONS=detect_leaks=0 "$S/sc" 2>&1)
  if echo "$out" | grep -q "AddressSanitizer"; then
    echo "FAIL $name -- ASan error"; echo "$out" | grep -m2 "ERROR\|SUMMARY" | sed 's/^/    /'; fail=$((fail+1)); return
  fi
  local got; got=$(echo "$out" | tail -1)
  if [ "$got" = "$expect" ]; then echo "PASS $name (live=$got)"; pass=$((pass+1))
  else echo "FAIL $name -- expected live=$expect, got '$got'"; fail=$((fail+1)); fi
}

# 1. The reported leak: repeated assignment of freshly allocated values.
check "loop-fresh" 0 '(defn main [] : int
  (let [^mut h (fresh) ^mut i 0]
    (while (< i 50)
      (set! h (fresh))
      (set! i (+ i 1))))
  (println (live))
  0)'

# 2. Explicit clone of another binding: both bindings own a +1.
check "clone-other" 0 '(defn main [] : int
  (let [a (fresh) ^mut h (fresh)]
    (set! h (rc/clone a)))
  (println (live))
  0)'

# 3. Bare var: a MOVE -- source auto-drop suppressed, ownership transfers.
check "move-other" 0 '(defn main [] : int
  (let [a (fresh) ^mut h (fresh)]
    (set! h a))
  (println (live))
  0)'

# 4. The new value READS the old one; the clone must be taken before release.
check "reads-old" 0 '(defn main [] : int
  (let [^mut h (fresh) ^mut i 0]
    (while (< i 20)
      (set! h (rc/of (make-struct Node (rc/clone h))))
      (set! i (+ i 1))))
  (println (live))
  0)'

# 5. Self-assignment: binding is moved, so no release may be emitted. It leaks
#    the block (live=1) exactly as it did before this change -- `(set! h h)`
#    suppresses the auto-drop entirely. Pinned so a future release here, which
#    would leave the binding dangling, shows up as a change rather than silently.
check "self-assign" 1 '(defn main [] : int
  (let [^mut h (fresh)]
    (set! h h))
  (println (live))
  0)'

# 6. Clone-of-self in a loop: increment then release must balance.
check "clone-self-loop" 0 '(defn main [] : int
  (let [^mut h (fresh) ^mut i 0]
    (while (< i 20)
      (set! h (rc/clone h))
      (set! i (+ i 1))))
  (println (live))
  0)'

# 7. Explicit drop then reassign: hand-managed, no release may be emitted.
#    (The reassigned value is then never released -- a pre-existing leak this
#    change deliberately does not touch, hence live=1 rather than 0.)
check "explicit-drop-then-set" 1 '(defn main [] : int
  (let [^mut h (fresh)]
    (rc/drop h)
    (set! h (fresh)))
  (println (live))
  0)'

# 8. Walking a chain by field read -- the borrow shape that needed clone-on-read.
check "field-read-walk" 0 '(defn main [] : int
  (let [tail (fresh)
        head (rc/of (make-struct Node (rc/clone tail)))]
    (let [^mut h (rc/clone head)]
      (set! h (.next h))))
  (println (live))
  0)'

# 9. ^mut initialized from a parameter (ownership moved in from the caller).
check "from-param" 0 '(defn take-it [p : rc<Node>] : int
  (let [^mut h p]
    (set! h (fresh)))
  0)
(defn main [] : int
  (take-it (fresh))
  (println (live))
  0)'

# 10. Nested scopes: an inner let assigning its own binding.
check "nested-scope" 0 '(defn main [] : int
  (let [^mut outer (fresh)]
    (let [^mut inner (fresh)]
      (set! inner (fresh)))
    (set! outer (fresh)))
  (println (live))
  0)'

# 11. Field <- field read: the value must take its own +1, or the two fields
#     alias one block that both later release.
check "field-from-field" 0 '(defn main [] : int
  (let [a (rc/of (make-struct Node (fresh)))
        b (rc/of (make-struct Node (fresh)))]
    (set! (.next a) (.next b)))
  (println (live))
  0)'

# 12. Field <- read of the SAME field: the value must be materialized before the
#     old pointer is released, or the read is a use-after-free.
check "field-from-self" 0 '(defn main [] : int
  (let [a (rc/of (make-struct Node (fresh)))]
    (set! (.next a) (.next a)))
  (println (live))
  0)'

# 13. Field <- explicit clone, the already-idiomatic shape.
check "field-from-clone" 0 '(defn main [] : int
  (let [a (rc/of (make-struct Node (fresh)))
        b (fresh)]
    (set! (.next a) (rc/clone b)))
  (println (live))
  0)'

# 14. Field <- bare var: a move, source auto-drop suppressed.
check "field-from-var" 0 '(defn main [] : int
  (let [a (rc/of (make-struct Node (fresh)))
        b (fresh)]
    (set! (.next a) b))
  (println (live))
  0)'

echo
echo "set-bang-rc-release: $pass passed, $fail failed"
[ "$fail" -eq 0 ]
