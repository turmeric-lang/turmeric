#!/usr/bin/env bash
# tests/turi/repl-jit-inline-c.sh -- aot-compiled-repl-plan C1.
#
# With --enable=repl-jit-inline-c on a TUR_JIT build, an inline-C defn the
# interpreter cannot run (try_exec_simple_inline_c declines its body) is
# compiled in process on its first call and called through its __ffi shim.
# Every body below has a loop, which the pattern executor does not run, so
# each one exercises the compiled path rather than the executor.
#
# Asserts on the evaluated output, never on exit status alone: a feature that
# silently did nothing would still exit 0.
#
# Self-skips (exit 0) on a binary with no JIT engine (configured
# -DTUR_JIT=OFF), probed the way repl-spice-jit.sh probes; the ctest
# registration is additionally gated on TUR_JIT.
set -uo pipefail
cd "$(dirname "$0")/../.."

TUR="${TUR:-${1:-./build/tur}}"
[ -x "$TUR" ] || { echo "tests: no tur binary at $TUR" >&2; exit 2; }

probe=$("$TUR" jit /nonexistent-tur-jit-probe.tur 2>&1 || true)
case "$probe" in
  *"carries no JIT"*)
     echo "SKIP repl-jit-inline-c ($TUR carries no JIT engine)"; exit 0 ;;
esac

# The interpreter keeps its closures for the process lifetime by design; a
# sanitized `tur repl` would otherwise report them (CLAUDE.md leak policy).
export ASAN_OPTIONS="${ASAN_OPTIONS:+$ASAN_OPTIONS:}detect_leaks=0"
WORK="$(mktemp -d -t tur-c1.XXXXXX)"
trap 'rm -rf "$WORK"' EXIT

PASS=0
FAIL=0

repl_out() {   # stdin -> `tur repl` with the given extra flags, ANSI stripped
    TUR_NO_AUTO_SPICE=1 TMPDIR="$WORK" "$TUR" repl "$@" 2>&1 \
        | sed 's/\x1b\[[0-9;]*m//g'
}

has() {
    local desc="$1" needle="$2" actual="$3"
    if grep -qF -- "$needle" <<< "$actual"; then
        echo "PASS: $desc"; PASS=$((PASS + 1))
    else
        echo "FAIL: $desc"
        echo "  expected to contain: $needle"
        echo "  got:"; sed 's/^/    /' <<< "$actual"
        FAIL=$((FAIL + 1))
    fi
}

lacks() {
    local desc="$1" needle="$2" actual="$3"
    if grep -qF -- "$needle" <<< "$actual"; then
        echo "FAIL: $desc"
        echo "  expected NOT to contain: $needle"
        echo "  got:"; sed 's/^/    /' <<< "$actual"
        FAIL=$((FAIL + 1))
    else
        echo "PASS: $desc"; PASS=$((PASS + 1))
    fi
}

# c-mix(3, 4) with three rounds of t = t * 31 + b: 3 -> 97 -> 3011 -> 93345.
C_MIX=(
    '(defn c-mix [a : int b : int] : int'
    '  ```c'
    '  int64_t t = a;'
    '  for (int i = 0; i < 3; i++) t = t * 31 + b;'
    '  return t;'
    '  ```)'
)

# --- Gate off: today's error, unchanged ----------------------------------
out="$(printf '%s\n' "${C_MIX[@]}" '(c-mix 3 4)' ':quit' | repl_out)"
has   "gate off: the interpreter still refuses" \
      "inline-C not supported in interpreter mode" "$out"
lacks "gate off: nothing compiled" "93345" "$out"

# --- Gate on: a loop body runs ---------------------------------------------
out="$(printf '%s\n' "${C_MIX[@]}" '(c-mix 3 4)' '(c-mix 0 1)' ':quit' \
       | repl_out --enable=repl-jit-inline-c)"
has   "loop body: compiled and called"     "=> 93345" "$out"
has   "loop body: second call (cached)"    "=> 993"   "$out"
lacks "loop body: no refusal"              "inline-C not supported" "$out"
has   "experiment lifecycle warning fires" "repl-jit-inline-c" "$out"

# --- Branches, a hoisted #include, and every scalar result kind ------------
# The float probe uses 7.1, not an integer (CLAUDE.md): 7.1 * 2.5^2 = 44.375.
out="$(printf '%s\n' \
    '(defn count-upper [s : cstr] : int' \
    '  ```c' \
    '  #include <ctype.h>' \
    '  int64_t n = 0;' \
    '  for (const char *p = s; *p; p++) if (isupper((unsigned char)*p)) n++;' \
    '  return n;' \
    '  ```)' \
    '(count-upper "Hello Turmeric World")' \
    '(defn fpow [x : float y : float] : float' \
    '  ```c' \
    '  double t = x;' \
    '  for (int i = 0; i < 2; i++) t = t * y;' \
    '  return t;' \
    '  ```)' \
    '(fpow 7.1 2.5)' \
    '(defn sign-word [n : int] : cstr' \
    '  ```c' \
    '  for (int i = 0; i < 1; i++) { if (n > 0) return "pos"; if (n < 0) return "neg"; }' \
    '  return "zero";' \
    '  ```)' \
    '(sign-word -5)' \
    '(sign-word 0)' \
    '(defn all-even [a : int b : int] : bool' \
    '  ```c' \
    '  int64_t xs[2] = { a, b };' \
    '  for (int i = 0; i < 2; i++) if (xs[i] % 2 != 0) return 0;' \
    '  return 1;' \
    '  ```)' \
    '(all-even 4 8)' \
    '(all-even 4 7)' \
    ':quit' | repl_out --enable=repl-jit-inline-c)"
has "hoisted #include: isupper"      "=> 3"      "$out"
has "float args and result"          "=> 44.375" "$out"
has "cstr result (negative)"         '=> "neg"'  "$out"
has "cstr result (zero)"             '=> "zero"' "$out"
has "bool result true"               "=> true"   "$out"
has "bool result false"              "=> false"  "$out"

# --- Redefinition drops the cached function --------------------------------
out="$(printf '%s\n' "${C_MIX[@]}" '(c-mix 3 4)' \
    '(defn c-mix [a : int b : int] : int' \
    '  ```c' \
    '  int64_t t = a;' \
    '  for (int i = 0; i < 2; i++) t = t * 37 + b;' \
    '  return t;' \
    '  ```)' \
    '(c-mix 3 4)' \
    ':quit' | repl_out --enable=repl-jit-inline-c)"
# 3 -> 115 -> 4259
has "redefinition: first body"      "=> 93345" "$out"
has "redefinition: new body called" "=> 4259"  "$out"

# --- Refusals are clean errors, and the session carries on -----------------
out="$(printf '%s\n' \
    '(defn helper [x : int] : int (* x 2))' \
    '(defn uses-helper [x : int] : int' \
    '  ```c' \
    '  int64_t s = 0;' \
    '  for (int i = 0; i < 2; i++) s += helper(x);' \
    '  return s;' \
    '  ```)' \
    '(uses-helper 5)' \
    '(+ 2 3)' \
    '(defstruct Pt [x : int y : int])' \
    '(defn px [p : Pt] : int' \
    '  ```c' \
    '  int64_t s = 0;' \
    '  for (int i = 0; i < 1; i++) s += 1;' \
    '  return s;' \
    '  ```)' \
    '(px (Pt 1 2))' \
    '(+ 3 4)' \
    ':quit' | repl_out --enable=repl-jit-inline-c)"
has "refusal: body calling a Turmeric defn" \
    "could not be compiled by the REPL JIT" "$out"
has "refusal: session continues"           "=> 5"  "$out"
has "refusal: struct parameter named" \
    "takes a parameter of type Pt" "$out"
has "refusal: session continues again"     "=> 7"  "$out"

# --- The value boundary: interpreter memory never reaches compiled code -----
# A pointer parameter takes a handle an earlier JIT'd call returned (or nil);
# an int-class parameter refuses a word that addresses the interpreter's own
# memory.  Both used to reach compiled code, which read -- and freed --
# interpreter allocations as its own and took the REPL process down.
out="$(printf '%s\n' \
    '(defn make-cell [v : int] : ptr<void>' \
    '  ```c' \
    '  #include <stdlib.h>' \
    '  int64_t *c = malloc(sizeof *c);' \
    '  for (int i = 0; i < 1; i++) *c = v;' \
    '  return c;' \
    '  ```)' \
    '(defn cell-get [c : ptr<void>] : int' \
    '  ```c' \
    '  int64_t s = -1;' \
    '  for (int i = 0; i < 1; i++) if (c) s = *(int64_t *)c;' \
    '  return s;' \
    '  ```)' \
    '(cell-get (make-cell 41))' \
    '(cell-get nil)' \
    '(cell-get (pack 42 (exists [a] [(Show a)] a)))' \
    '(+ 4 5)' \
    '(defn word-set [h : int] : int' \
    '  ```c' \
    '  int64_t s = 0;' \
    '  for (int i = 0; i < 1; i++) s = h != 0;' \
    '  return s;' \
    '  ```)' \
    '(word-set (vec-new))' \
    '(word-set 12)' \
    ':quit' | repl_out --enable=repl-jit-inline-c)"
has "boundary: a compiled handle round-trips"  "=> 41" "$out"
has "boundary: nil reaches a pointer param"    "=> -1" "$out"
has "boundary: interpreter value refused at a pointer param" \
    "is not a handle compiled code returned" "$out"
has "boundary: session continues"              "=> 9"  "$out"
has "boundary: interpreter allocation refused at an int param" \
    "is a value the interpreter allocated" "$out"
has "boundary: a plain number still passes"    "=> 1"  "$out"

# --- tur --interpret runs it too --------------------------------------------
cat > "$WORK/prog.tur" <<'EOF'
(defn c-mix [a : int b : int] : int
  ```c
  int64_t t = a;
  for (int i = 0; i < 3; i++) t = t * 31 + b;
  return t;
  ```)
(defn main [] : int
  (println (c-mix 3 4))
  0)
EOF
out="$(TUR_NO_AUTO_SPICE=1 TMPDIR="$WORK" "$TUR" --interpret \
       --enable=repl-jit-inline-c "$WORK/prog.tur" 2>&1)"
has "--interpret: compiled and called" "93345" "$out"

# --- No scratch file is left behind -----------------------------------------
left=$(find "$WORK" -name 'tur-repl-jit-*' | head -1)
if [ -z "$left" ]; then
    echo "PASS: scratch files removed"; PASS=$((PASS + 1))
else
    echo "FAIL: scratch file left behind: $left"; FAIL=$((FAIL + 1))
fi

echo ""
echo "repl-jit-inline-c: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
