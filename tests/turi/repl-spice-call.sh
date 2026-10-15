#!/usr/bin/env bash
# tests/turi/repl-spice-call.sh -- RP4 integration test.
#
# Drives `tur repl` inside a fixture spice project and asserts that
# every export installed by the binding layer (RP3 + RP4) is callable
# from the prompt with correct results, qualified-name routing, and
# typed-error feedback for mismatches.

set -uo pipefail
cd "$(dirname "$0")/../.."

TUR="${TUR:-./build/tur}"
[ -x "$TUR" ] || { echo "tests: $TUR not built; run 'tur run build' first" >&2; exit 2; }
export TUR_BIN
case "$TUR" in
    /*) TUR_BIN="$TUR" ;;
    *)  TUR_BIN="$(cd "$(dirname "$TUR")" && pwd)/$(basename "$TUR")" ;;
esac

WORK="$(mktemp -d -t tur-rp4.XXXXXX)"
trap 'rm -rf "$WORK"' EXIT
# The REPL's spice-binding installer allocates one FfiBindingUd plus one
# qualified-name key per export and deliberately never frees either --
# src/turi/ffi_thunk.c:533 ("Leaked at process exit") and :551 ("qkey is
# referenced by the env binding's name field; do not free here").  Those are
# process-lifetime by design, like the rest of the tree-walking interpreter,
# so LeakSanitizer's report is noise here and pollutes the output these
# assertions diff against.  Same opt-out repl-spice-jit.sh already carries;
# the compiler/codegen path stays leak-checked (see CLAUDE.md).
export ASAN_OPTIONS="${ASAN_OPTIONS:+$ASAN_OPTIONS:}detect_leaks=0"

PROJ="$WORK/proj"
mkdir -p "$PROJ/src"
cat > "$PROJ/build.tur" <<'EOF'
(defpackage rp4-call-fixture)
EOF
cat > "$PROJ/src/lib.tur" <<'EOF'
(defmodule sh
  (export add42 mul scale answer noisy sum12 wide-mix imix9 vsum)
  (defn add42 [x :int] :int (+ x 42))
  (defn mul [a :int b :int] :int (* a b))
  (defn scale [x :float y :float] :float (* x y))
  (defn answer [] :int 42)
  (defn noisy [x :int] :void (println x))
  ;; interpreter-arbitrary-arity-ffi: arity 12 exceeds the generated
  ;; shape table's default --max-arity (6), so this is only callable via
  ;; the per-export `__ffi` shim the spice build now emits.
  (defn sum12 [a :int b :int c :int d :int e :int f :int
               g :int h :int i :int j :int k :int l :int] :int
    (+ a (+ b (+ c (+ d (+ e (+ f (+ g (+ h (+ i (+ j (+ k l))))))))))))
  ;; High-arity with a mixed int/float register split -- the params
  ;; interleave int/float, so the shim must read fv[1],fv[3],fv[5],fv[7]
  ;; (not fv[0..3]); a wrong buffer/index would corrupt the float sum.
  (defn wide-mix [a :int b :float c :int d :float e :int f :float
                  g :int h :float] :float
    (+ b (+ d (+ f h))))
  ;; Wide all-int, int return, to pin the int-slot indexing at arity 9.
  (defn imix9 [a :int b :int c :int d :int e :int
               f :int g :int h :int i :int] :int
    (- a (+ b (+ c (+ d (+ e (+ f (+ g (+ h i)))))))))
  ;; ffi-spices-integration-plan S2: a variadic export.  The REPL shim
  ;; folds everything past the positional prefix into a right-folded
  ;; cons list and passes it in the trailing int64 slot; these helpers
  ;; walk it on the compiled side.
  (defn vhead [lst :int] #fx{Unsafe} :int
    ```c
    typedef struct { int64_t head; int64_t tail; } __tur_cons_cell;
    __tur_cons_cell *p = (__tur_cons_cell *)(intptr_t)lst;
    return p ? p->head : 0;
    ```)
  (defn vtail [lst :int] #fx{Unsafe} :int
    ```c
    typedef struct { int64_t head; int64_t tail; } __tur_cons_cell;
    __tur_cons_cell *p = (__tur_cons_cell *)(intptr_t)lst;
    return p ? p->tail : 0;
    ```)
  (defn vsum-acc [lst :int acc :int] #fx{Unsafe} :int
    (if (= lst 0) acc (vsum-acc (vtail lst) (+ acc (vhead lst)))))
  (defn vsum [x :int & rest :int] #fx{Unsafe} :int
    (+ x (vsum-acc rest 0))))
EOF

PASS=0
FAIL=0
pass() { PASS=$((PASS + 1)); echo "PASS $1"; }
fail() { FAIL=$((FAIL + 1)); echo "FAIL $1 -- $2"; }

run() {
    # Echoes one or more REPL lines into `tur repl`, capturing stdout
    # plus stderr. Stdin's final line is :quit so editline doesn't hang
    # waiting on input.
    local prog="$1"
    (cd "$PROJ" && printf '%s\n:quit\n' "$prog" | "$TUR_BIN" repl 2>&1)
}

# --- happy paths ---------------------------------------------------------

out=$(run '(add42 100)')
if grep -qx '=> 142' <<< "$out"; then pass "rp4-call-int->int"
else fail "rp4-call-int->int" "$out"; fi

# Qualified name resolves to the same shim.
out=$(run '(sh/add42 99)')
if grep -qx '=> 141' <<< "$out"; then pass "rp4-call-qualified-name"
else fail "rp4-call-qualified-name" "$out"; fi

out=$(run '(mul 6 7)')
if grep -qx '=> 42' <<< "$out"; then pass "rp4-call-int-int->int"
else fail "rp4-call-int-int->int" "$out"; fi

out=$(run '(scale 2.5 4.0)')
if grep -qx '=> 10' <<< "$out"; then pass "rp4-call-float-float->float"
else fail "rp4-call-float-float->float" "$out"; fi

out=$(run '(scale 1.5 3.0)')
if grep -qx '=> 4.5' <<< "$out"; then pass "rp4-call-float-non-integer-result"
else fail "rp4-call-float-non-integer-result" "$out"; fi

out=$(run '(answer)')
if grep -qx '=> 42' <<< "$out"; then pass "rp4-call-zero-arg"
else fail "rp4-call-zero-arg" "$out"; fi

# Side-effect call returning :void: must print the arg AND yield => nil.
out=$(run '(noisy 99)')
if grep -qx '99' <<< "$out" && grep -qx '=> nil' <<< "$out"; then
    pass "rp4-call-void-return"
else
    fail "rp4-call-void-return" "$out"
fi

# --- arbitrary arity via per-export FFI shim -----------------------------
# These arities (12, 9) exceed the generated shape table's default
# --max-arity (6). Before the per-export `__ffi` shim they failed at call
# time with "no registered dispatcher for shape"; now they resolve.

out=$(run '(sum12 1 2 3 4 5 6 7 8 9 10 11 12)')
if grep -qx '=> 78' <<< "$out"; then pass "rp4-call-arity12-all-int"
else fail "rp4-call-arity12-all-int" "$out"; fi

# Interleaved int/float params: the shim must read fv[1],fv[3],fv[5],fv[7]
# (b+d+f+h = 1.5+2.25+0.25+4.0 = 8.0), proving per-position class dispatch.
out=$(run '(wide-mix 1 1.5 2 2.25 3 0.25 4 4.0)')
if grep -qx '=> 8' <<< "$out"; then pass "rp4-call-arity8-int-float-split"
else fail "rp4-call-arity8-int-float-split" "$out"; fi

# Wide all-int, int return: 100 - (1+2+3+4+5+6+7+8) = 64.
out=$(run '(imix9 100 1 2 3 4 5 6 7 8)')
if grep -qx '=> 64' <<< "$out"; then pass "rp4-call-arity9-int-return"
else fail "rp4-call-arity9-int-return" "$out"; fi

# --- error surface -------------------------------------------------------

out=$(run '(add42)')
if grep -q "expects 1 arg, got 0" <<< "$out"; then
    pass "rp4-call-arity-too-few"
else
    fail "rp4-call-arity-too-few" "$out"
fi

out=$(run '(add42 1 2)')
if grep -q "expects 1 arg, got 2" <<< "$out"; then
    pass "rp4-call-arity-too-many"
else
    fail "rp4-call-arity-too-many" "$out"
fi

# Float passed where :int-class is required: must reject.
out=$(run '(add42 1.5)')
if grep -q "expected :int-class, got float" <<< "$out"; then
    pass "rp4-call-type-mismatch"
else
    fail "rp4-call-type-mismatch" "$out"
fi

# --- after a call, the env is still usable ----------------------------

out=$(run '(add42 1)
(+ 1 2)')
# After the FFI call we should still get the => 3 from the second expr.
if grep -qx '=> 43' <<< "$out" && grep -qx '=> 3' <<< "$out"; then
    pass "rp4-call-env-unaffected"
else
    fail "rp4-call-env-unaffected" "$out"
fi

# --- S2: variadic export marshalling ------------------------------------
# The REPL shim folds args past the positional prefix into a right-folded
# cons list (float heads as IEEE-754 bit patterns) and passes it in the
# trailing int64 slot -- the same list the compiled call sites build.

out=$(run '(vsum 1 2 3 4)')
if grep -qx '=> 10' <<< "$out"; then pass "s2-variadic-rest"
else fail "s2-variadic-rest" "expected => 10 from (vsum 1 2 3 4); got: $(echo "$out" | tail -3 | tr '\n' ' ')"
fi

out=$(run '(vsum 5)')
if grep -qx '=> 5' <<< "$out"; then pass "s2-variadic-empty-rest"
else fail "s2-variadic-empty-rest" "expected => 5 from (vsum 5); got: $(echo "$out" | tail -3 | tr '\n' ' ')"
fi

out=$(run '(vsum)')
if grep -q 'expects at least 1' <<< "$out"; then pass "s2-variadic-underflow"
else fail "s2-variadic-underflow" "expected an at-least-arity error for (vsum); got: $(echo "$out" | tail -3 | tr '\n' ' ')"
fi

echo
echo "repl-spice-call: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
