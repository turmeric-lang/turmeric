#!/usr/bin/env bash
# tests/run-interp-show-inline-c.sh -- a USER `Show`/`show` instance with an
# inline-C body, under --interpret.
#
# turi-show-instance-with-inline-c-body-prints-a-pointer: the interpreter's
# stdlib natives for `Show`'s inline-C numeric instances are registered under
# the elaborator's mangled instance names (`__inst_Show_show_float`, ...), and
# the "keep native override" branch in eval.c kept such a native for ANY impl
# that landed on the same key.  A user program defining its own minimal
# `(defclass Show [a] (show [x] : cstr))` with an inline-C `Show [float]` body
# therefore ran stdlib's native, which returns an owned String handle -- so
# `(println (.show 7.35))` printed the handle's address, silently, where every
# other inline-C shape the tree-walker cannot run says so.  The override is now
# kept only for an impl defined under stdlib/.
#
# This needs its OWN runner rather than a fixture: tests/run-turi.sh PASS-skips
# every program containing a user inline-C block (the TI7 carve-out), which is
# exactly why nothing caught it.
#
# What is asserted (the isolation table from the report, plus the controls).
# Rows 1-4 accept either honest outcome -- the simple executor claims the body
# and answers correctly, or the clean inline-C diagnostic with a non-zero exit
# -- and reject a bare integer (the pointer) on stdout:
#   1. Show/show  + inline-C, float receiver (unclaimed today: diagnostic).
#   2. Show/show  + inline-C, int receiver   (claimed today: `int:735`).
#   3. Show/render + inline-C                 (never broke; must not start).
#   4. P/f        + inline-C.
#   5. Show/show with a PURE body:             prints the answer, exit 0 (the
#      user `Show` path must keep working).
#   6. Compiled Show/show + inline-C:          `float:7.35` (unchanged).
#   7. stdlib's own Show under --interpret:    `show-line` on a float still
#      answers through the native (the override the fix must keep).
#
# Usage: bash tests/run-interp-show-inline-c.sh
# Environment: TUR  path to the compiler (default: ./build/tur)

set -u
cd "$(dirname "$0")/.."

TUR="${TUR:-./build/tur}"
# The rows above are about what the interpreter ITSELF does with the body, so
# the inline-C JIT (repl-jit-inline-c, on by default on a JIT build since
# 2026-10-17) is held off: it would otherwise compile a body the executor
# declines, and the "clean diagnostic" outcome would never be seen.
export TUR_REPL_JIT_INLINE_C=0
if [ ! -x "$TUR" ]; then
    echo "run-interp-show-inline-c: $TUR not built" >&2
    exit 2
fi

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
FAILED=0

# The tree-walker retains process-lifetime closures by design, so leak
# detection is off; the ADDRESS sanitizer stays on.
export ASAN_OPTIONS=detect_leaks=0

write_case() {  # $1 file  $2 class  $3 method  $4 type  $5 literal  $6 fmt
    cat > "$1" <<EOF
(defclass $2 [a] ($3 [x] : cstr))
(definstance $2 [$4]
  ($3 [x] : cstr
    \`\`\`c
    char *b = (char *)malloc(32);
    snprintf(b, 32, "$4:$6", x);
    return (const char *)b;
    \`\`\`))
(defn main [] : int (println (.$3 $5)) 0)
EOF
}

write_case "$TMP/show-float.tur"  Show show   float 7.35 '%.2f'
write_case "$TMP/show-int.tur"    Show show   int   735  '%lld'
write_case "$TMP/show-render.tur" Show render float 7.35 '%.2f'
write_case "$TMP/p-f.tur"         P    f      float 7.35 '%.2f'

# The interpreter has exactly two honest outcomes for an inline-C body: a
# matcher in the simple executor claims it and returns the RIGHT answer (the
# `%lld` int body is claimed today), or nothing claims it and the clean
# diagnostic fires.  Either passes.  A bare integer on stdout -- the pointer --
# is the defect.
expect_honest() {  # $1 label  $2 file  $3 the right answer, if a matcher claims it
    local out rc
    out="$("$TUR" --interpret "$2" 2>&1)"
    rc=$?
    if [ $rc -eq 0 ] && [ "$out" = "$3" ]; then
        echo "PASS $1: the simple executor claimed the body and answered '$3'"
    elif [ $rc -eq 0 ]; then
        echo "FAIL $1: --interpret exited 0 with '$out' (expected '$3' or the inline-C diagnostic)"
        FAILED=1
    elif ! printf '%s' "$out" | grep -q "inline-C not supported in interpreter mode"; then
        echo "FAIL $1: expected 'inline-C not supported in interpreter mode', got:"
        echo "$out" | head -3
        FAILED=1
    elif printf '%s\n' "$out" | grep -qE '^[0-9]{6,}$'; then
        echo "FAIL $1: a bare integer (a pointer) reached stdout"
        echo "$out" | head -3
        FAILED=1
    else
        echo "PASS $1: clean inline-C diagnostic, no pointer"
    fi
}

expect_honest "interp-show-inline-c: Show/show float" "$TMP/show-float.tur"  "float:7.35"
expect_honest "interp-show-inline-c: Show/show int"   "$TMP/show-int.tur"    "int:735"
expect_honest "interp-show-inline-c: Show/render"     "$TMP/show-render.tur" "float:7.35"
expect_honest "interp-show-inline-c: P/f"             "$TMP/p-f.tur"         "float:7.35"

# 5. A pure-bodied user Show still answers.
cat > "$TMP/pure.tur" <<'EOF'
(defclass Show [a] (show [x] : cstr))
(definstance Show [float] (show [x] : cstr "a-float"))
(defn main [] : int (println (.show 7.35)) 0)
EOF
out="$("$TUR" --interpret "$TMP/pure.tur" 2>&1)"
if [ "$out" != "a-float" ]; then
    echo "FAIL interp-show-inline-c: pure user Show expected 'a-float', got '$out'"
    FAILED=1
else
    echo "PASS interp-show-inline-c: pure user Show still answers"
fi

# 6. The compiled path is unchanged.
out="$("$TUR" run "$TMP/show-float.tur" 2>/dev/null)"
if [ "$out" != "float:7.35" ]; then
    echo "FAIL interp-show-inline-c: compiled expected 'float:7.35', got '$out'"
    FAILED=1
else
    echo "PASS interp-show-inline-c: compiled still answers 'float:7.35'"
fi

# 7. stdlib's own Show keeps its native under --interpret.
cat > "$TMP/stdlib-show.tur" <<'EOF'
(load "stdlib/typeclass-show.tur")
(defn main [] : int (show-line 7.35) 0)
EOF
out="$("$TUR" --interpret "$TMP/stdlib-show.tur" 2>&1)"
if [ "$out" != "7.35" ]; then
    echo "FAIL interp-show-inline-c: stdlib show-line expected '7.35', got '$out'"
    FAILED=1
else
    echo "PASS interp-show-inline-c: stdlib Show [float] keeps its native"
fi

if [ $FAILED -ne 0 ]; then
    echo "run-interp-show-inline-c: FAILED"
    exit 1
fi
echo "run-interp-show-inline-c: PASS"
