#!/usr/bin/env bash
# tests/turi/repl-compiled.sh -- aot-compiled-repl-plan C2, step 1.
#
# Under --enable=compiled-repl every REPL turn runs as a compiled program: the
# session's definition table plus a `main` that runs the turn and prints its
# result as the REPL would.  This checks the transcript a session prints, and
# the differences step 1 documents: a mutable global does not survive a turn
# (every initializer re-runs), and a value with no scalar printer shows as
# #<Type>.  Without the flag nothing changes.
#
# Asserts on the evaluated output, never on exit status alone: a feature that
# silently did nothing would still exit 0.
#
# Self-skips (exit 0) on a binary with no JIT engine, probed as
# repl-jit-inline-c.sh probes; the ctest registration is gated on TUR_JIT.
set -uo pipefail
cd "$(dirname "$0")/../.."

TUR="${TUR:-${1:-./build/tur}}"
[ -x "$TUR" ] || { echo "tests: no tur binary at $TUR" >&2; exit 2; }

probe=$("$TUR" jit /nonexistent-tur-jit-probe.tur 2>&1 || true)
case "$probe" in
  *"carries no JIT"*)
     echo "SKIP repl-compiled ($TUR carries no JIT engine)"; exit 0 ;;
esac

export ASAN_OPTIONS="${ASAN_OPTIONS:+$ASAN_OPTIONS:}detect_leaks=0"
WORK="$(mktemp -d -t tur-c2.XXXXXX)"
trap 'rm -rf "$WORK"' EXIT

PASS=0
FAIL=0

repl_out() {   # stdin -> `tur repl` with the given extra flags, ANSI stripped
    TUR_NO_AUTO_SPICE=1 TMPDIR="$WORK" "$TUR" repl "$@" 2>&1 \
        | sed 's/\x1b\[[0-9;]*m//g'
}

# The `=> ...` lines, in order, one per line.
results() { grep '^=> ' <<< "$1"; }

same() {
    local desc="$1" expected="$2" actual="$3"
    if [ "$expected" = "$actual" ]; then
        echo "PASS: $desc"; PASS=$((PASS + 1))
    else
        echo "FAIL: $desc"
        echo "  expected:"; sed 's/^/    /' <<< "$expected"
        echo "  got:"; sed 's/^/    /' <<< "$actual"
        FAIL=$((FAIL + 1))
    fi
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

# --- Scalars, definitions, redefinition: the same transcript both ways -------
cat > "$WORK/t1" <<'EOF'
(def kept 41)
(defn f [x : int] : int (* x 2))
(f 3)
"abc"
3.25
true
(println "hi")
(defstruct Pt [x : int y : int])
(+ kept (f 1))
(defn f [x : int] : int (* x 10))
(f 3)
EOF
interp="$(repl_out < "$WORK/t1")"
comp="$(repl_out --enable=compiled-repl < "$WORK/t1")"
same "scalars, defs, a redefinition: compiled matches interpreted" \
     "$(results "$interp")" "$(results "$comp")"
has "the program's own output comes before its => line" "hi" "$comp"
has "the experiment warns" "experimental feature 'compiled-repl'" "$comp"

# --- Inline C needs no special path: the whole turn is compiled ---------------
out="$(repl_out --enable=compiled-repl <<'EOF'
(defn c-mix [a : int b : int] : int ```c int64_t t = a; for (int i = 0; i < 2; i++) t += i; return t * 31 + b; ```)
(c-mix 3 4)
(defn hyp [a : float b : float] : float ```c double sqrt(double); return sqrt(a * a + b * b); ```)
(hyp 3.1 4.2)
EOF
)"
same "inline-C defns compile with the turn" \
     "$(printf '=> #<fn c-mix>\n=> 128\n=> #<fn hyp>\n=> 5.22015')" "$(results "$out")"

# --- A failing turn changes nothing --------------------------------------------
out="$(repl_out --enable=compiled-repl <<'EOF'
(defn g [x : int] : int (+ x 1))
(defn g [x : int] : int (+ x "a"))
(g 1)
(defn h [] : int ```c this is not C; ```)
(g 2)
EOF
)"
same "a turn that fails to elaborate or compile leaves the table as it was" \
     "$(printf '=> #<fn g>\n=> 2\n=> 3')" "$(results "$out")"
has "the c2mir rejection is reported" "the turn did not compile" "$out"

# --- Step 1's documented differences --------------------------------------------
out="$(repl_out --enable=compiled-repl <<'EOF'
(def ^mut n 5)
(defn bump [] : int (do (set! n (+ n 1)) n))
(bump)
(bump)
[1 2 3]
EOF
)"
same "step 1: every initializer re-runs, so a mutation lasts one turn; a Vec is #<Type>" \
     "$(printf '=> 5\n=> #<fn bump>\n=> 6\n=> 6\n=> #<(Vec int)>')" "$(results "$out")"

# --- Off by default ----------------------------------------------------------------
out="$(repl_out <<'EOF'
(def ^mut n 5)
(defn bump [] : int (do (set! n (+ n 1)) n))
(bump)
(bump)
EOF
)"
same "without the flag the turn is interpreted" \
     "$(printf '=> 5\n=> #<fn bump>\n=> 6\n=> 7')" "$(results "$out")"

# --- No scratch file is left behind ----------------------------------------------
left=$(find "$WORK" -name 'tur-repl-turn-*' | head -1)
if [ -z "$left" ]; then
    echo "PASS: scratch files removed"; PASS=$((PASS + 1))
else
    echo "FAIL: scratch file left behind: $left"; FAIL=$((FAIL + 1))
fi

echo ""
echo "repl-compiled: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
