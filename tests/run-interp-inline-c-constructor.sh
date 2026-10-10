#!/usr/bin/env bash
# tests/run-interp-inline-c-constructor.sh -- the interpreter's inline-C
# constructor/destructor patterns, under --interpret.
#
# turi-inline-c-constructor-free-pair: two defects in try_exec_simple_inline_c
# (src/turi/eval.c), both found running the stats spice in a notebook cell.
#
#   1. The constructor pattern builds its "malloc" in the env's value pool
#      (turi_val_alloc), and the `free(` pattern handed that pointer to libc
#      free: `(dist-free (dist-normal 0.0 1.0))` was an AddressSanitizer
#      bad-free on the Debug tur, and heap corruption on a Release one.
#   2. The "string fat-pointer" special case matched `->p` and `->len` as bare
#      substrings, so a body reading `d->p1` and writing `out->length` -- stats'
#      random-n, two allocations and a sampling loop -- was "constructed" as a
#      fat pointer over its first argument and returned a wrong value silently.
#      It declines now, and the clean "inline-C not supported" error fires.
#
# This needs its OWN runner rather than a fixture: tests/run-turi.sh PASS-skips
# every program containing a user inline-C block (the TI7 carve-out), so no
# fixture in the tree can assert an inline-C body under --interpret at all.
#
# Usage: bash tests/run-interp-inline-c-constructor.sh
# Environment: TUR  path to the compiler (default: ./build/tur)

set -u
cd "$(dirname "$0")/.."

TUR="${TUR:-./build/tur}"
if [ ! -x "$TUR" ]; then
    echo "run-interp-inline-c-constructor: $TUR not built" >&2
    exit 2
fi
# The interpreter keeps its closures for the process lifetime by design
# (CLAUDE.md leak policy); a bad-free is still reported with leaks off.
export ASAN_OPTIONS="${ASAN_OPTIONS:+$ASAN_OPTIONS:}detect_leaks=0"

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
FAILED=0

cat > "$TMP/pair.tur" <<'EOF'
(defn box-new [v : int] : int
  ```c
  typedef struct { int64_t v; int64_t w; } NbBox;
  NbBox *b = (NbBox *)malloc(sizeof(NbBox));
  b->v = v; b->w = 0;
  return (int64_t)(intptr_t)b;
  ```)
(defn box-free [b : int] : void
  ```c
  free((void *)(intptr_t)b);
  ```)
(defn main [] : int
  (let [b (box-new 7)]
    (box-free b)
    (println 1)
    0))
EOF

cat > "$TMP/fat.tur" <<'EOF'
(defn pick [d : int n : int] : int
  ```c
  typedef struct { int64_t tag; double p1; } NbDist;
  typedef struct { int64_t length; double *vals; } NbCol;
  NbDist *dist = (NbDist *)(intptr_t)d;
  double *vals = (double *)malloc(sizeof(double) * (size_t)(n > 0 ? n : 1));
  int64_t i = 0;
  while (i < n) { vals[i] = dist->p1 + (double)i; i++; }
  NbCol *out = (NbCol *)calloc(1, sizeof(NbCol));
  out->length = n; out->vals = vals;
  return (int64_t)(intptr_t)out;
  ```)
(defn dist-new [] : int
  ```c
  typedef struct { int64_t tag; double p1; } NbDist;
  NbDist *d = (NbDist *)malloc(sizeof(NbDist));
  d->tag = 1; d->p1 = 2.5;
  return (int64_t)(intptr_t)d;
  ```)
(defn col-len [c : int] : int
  ```c
  typedef struct { int64_t length; double *vals; } NbCol;
  return ((NbCol *)(intptr_t)c)->length;
  ```)
(defn main [] : int
  (println (col-len (pick (dist-new) 3)))
  0)
EOF

# 1. The free that pairs with an emulated constructor does not crash.
out=$("$TUR" --interpret "$TMP/pair.tur" 2>&1); rc=$?
if [ $rc -eq 0 ] && [ "$out" = "1" ]; then
    echo "PASS constructor/free pair under --interpret"
else
    echo "FAIL constructor/free pair under --interpret (exit $rc)"
    echo "$out" | grep -m3 "ERROR\|SUMMARY\|^1$" | sed 's/^/    /'
    FAILED=1
fi

# 2. A loop body that only spells `->p1` / `->length` is not a fat-pointer
#    constructor: the interpreter declines it rather than answer wrongly.
out=$("$TUR" --interpret "$TMP/fat.tur" 2>&1)
if echo "$out" | grep -q "inline-C not supported"; then
    echo "PASS loop body with ->p1/->length declines under --interpret"
else
    echo "FAIL loop body with ->p1/->length was claimed under --interpret"
    echo "$out" | head -3 | sed 's/^/    /'
    FAILED=1
fi

# 3. Control: the compiled path answers the program's own value.
out=$("$TUR" run "$TMP/fat.tur" 2>&1); rc=$?
if [ $rc -eq 0 ] && [ "$out" = "3" ]; then
    echo "PASS compiled control answers 3"
else
    echo "FAIL compiled control (exit $rc): $out"
    FAILED=1
fi

exit $FAILED
