#!/usr/bin/env bash
# tests/check-jit-prune-baggage.sh -- a JIT'd program carries only the stdlib
# it reaches (src/compiler/jit_prune.c).
#
# Every emitted program holds the whole stdlib prelude.  Three things in it
# used to be roots the pruner never questioned, so every program kept what
# they named: the constructor aliases (`#define ctor_Tuple2
# ctor_Tuple2_Tuple2`) kept every stdlib constructor, `__tur_any_drop_keep`
# kept `__tur_any_drop`, and `__tur_fatbox_init` (with the box declarations
# and `#undef min`) kept every stdlib function value and `min`/`max`.
# `(println 1)` carried 32 function definitions; it now carries 9, and a
# compiled REPL turn kept on the page went from ~394 KB to ~229 KB.
#
# This compiles, with `tur jit` and TUR_JIT_DUMP_FULL (the text c2mir got):
#
#   p1  `(println 1)`: none of that baggage is defined, and it prints;
#   p2  a defn passed as a function value and a lambda: their fat boxes are
#       kept and the calls through them answer right;
#   p3  a program constructing `Some`: the constructor it calls,
#       `ctor_Option_Some__int` (the monomorph), is kept;
#
# and checks that none needed the full-TU retry (TUR-W0071), and that with
# pruning off (TUR_JIT_NO_PRUNE=1) p1 does carry the baggage, so the first
# check is the pruner's doing.  SKIPs on a binary without the JIT engine.
set -uo pipefail
cd "$(dirname "$0")/.."
TUR="${TUR:-${1:-./build/tur}}"
[ -x "$TUR" ] || { echo "tests: no tur binary at $TUR" >&2; exit 2; }

probe=$("$TUR" jit /nonexistent-tur-jit-probe.tur 2>&1 || true)
case "$probe" in
  *"carries no JIT"*)
     echo "SKIP check-jit-prune-baggage ($TUR carries no JIT engine)"; exit 0 ;;
esac

export ASAN_OPTIONS="${ASAN_OPTIONS:+$ASAN_OPTIONS:}detect_leaks=0"
export TUR_NO_GLOBAL_SPICES=1
WORK="$(mktemp -d -t tur-prune.XXXXXX)"
trap 'rm -rf "$WORK"' EXIT

PASS=0
FAIL=0
ok()  { echo "PASS: $1"; PASS=$((PASS + 1)); }
bad() { echo "FAIL: $1"; FAIL=$((FAIL + 1)); }

# run NAME -- writes $WORK/NAME.{out,err,c}
run() {
    local name="$1"
    TUR_JIT_DUMP_FULL="$WORK/$name.c" "$TUR" jit "$WORK/$name.tur" \
        > "$WORK/$name.out" 2> "$WORK/$name.err"
}

defines() {   # defines FILE NAME: FILE holds a definition of function NAME
    grep -qE "^(static )?[^;=(]*[ *]$2\([^;]*\) *\{" "$1"
}

cat > "$WORK/p1.tur" <<'EOF'
(defn main [] : int
  (println 1)
  0)
EOF
cat > "$WORK/p2.tur" <<'EOF'
(defn add [a : int b : int] : int (+ a b))
(defn apply2 [f : (fn [int int] int)] : int (f 2 3))
(defn main [] : int
  (println (apply2 add))
  (println (apply2 (fn [a : int b : int] : int (* a b))))
  0)
EOF
cat > "$WORK/p3.tur" <<'EOF'
(defn main [] : int
  (println (match (some 3) (Some x) x (None) 0))
  0)
EOF

for p in p1 p2 p3; do run "$p"; done

# --- p1: the baggage is gone, and the program runs ----------------------------
if [ "$(cat "$WORK/p1.out")" = "1" ]; then ok "p1 prints 1"; else bad "p1 printed: $(cat "$WORK/p1.out")"; fi
kept=""
for fn in ctor_Tuple2_Tuple2 ctor_Tuple8_Tuple8 ctor_Grid_Grid ctor_Schema_Schema \
          ctor_Option_Some __tur_any_drop __tur_fatshim_bool_int64_t_int64_t min max; do
    defines "$WORK/p1.c" "$fn" && kept="$kept $fn"
done
if [ -z "$kept" ]; then ok "p1 defines no unreached stdlib function"; else bad "p1 still defines:$kept"; fi

# --- p2: function values keep their boxes, and answer right ---------------------
if [ "$(cat "$WORK/p2.out")" = "$(printf '5\n6')" ]; then ok "p2: calls through a defn value and a lambda"
else bad "p2 printed: $(cat "$WORK/p2.out")"; fi
if grep -q "TUR_FATBOX_DEF(" "$WORK/p2.c"; then ok "p2 keeps the fat boxes it uses"
else bad "p2 dropped every fat box"; fi

# --- p3: a constructor a program uses is kept -------------------------------------
if [ "$(cat "$WORK/p3.out")" = "3" ]; then ok "p3 prints 3"; else bad "p3 printed: $(cat "$WORK/p3.out")"; fi
if defines "$WORK/p3.c" ctor_Option_Some__int; then ok "p3 keeps ctor_Option_Some__int"
else bad "p3 dropped ctor_Option_Some__int"; fi

# --- none needed the full-TU retry ---------------------------------------------------
retried=""
for p in p1 p2 p3; do grep -q "TUR-W0071" "$WORK/$p.err" && retried="$retried $p"; done
if [ -z "$retried" ]; then ok "no program needed the full-TU retry"; else bad "retried:$retried"; fi

# --- control: unpruned, p1 carries it ----------------------------------------------
TUR_JIT_NO_PRUNE=1 TUR_JIT_DUMP_FULL="$WORK/p1-full.c" "$TUR" jit "$WORK/p1.tur" > /dev/null 2>&1
if defines "$WORK/p1-full.c" ctor_Tuple2_Tuple2; then ok "control: unpruned p1 defines ctor_Tuple2_Tuple2"
else bad "control: unpruned p1 has no ctor_Tuple2_Tuple2 -- the first check proves nothing"; fi

echo ""
echo "check-jit-prune-baggage: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
