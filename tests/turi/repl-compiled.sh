#!/usr/bin/env bash
# tests/turi/repl-compiled.sh -- aot-compiled-repl-plan C2, steps 1 and 2.
#
# Under --enable=compiled-repl every REPL turn runs as a compiled program: the
# session's definition table plus a `main` that runs the turn and prints its
# result as the REPL would.  A `def` lives in a host slot (step 2), so its
# value and its mutations carry from turn to turn, and a redefinition
# initializes it again.  This checks the transcripts a session prints against
# the interpreter's: scalars, definitions, slots, and results printed through
# Show or their constructors.  Without the flag nothing changes.
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

# --- Globals live in slots: mutation, redefinition, closures, strings -----------
cat > "$WORK/t2" <<'EOF'
(def ^mut n 5)
(defn bump [] : int (do (set! n (+ n 1)) n))
(bump)
(bump)
n
(def ^mut n 100)
(bump)
(def greeting "hello")
(defn greet [] : cstr greeting)
(greet)
(def scale 2.5)
(* scale 2.0)
(def g (fn [x : int] : int (+ x n)))
(g 1)
(bump)
(g 1)
EOF
interp="$(repl_out < "$WORK/t2")"
comp="$(repl_out --enable=compiled-repl < "$WORK/t2")"
# The one line that differs: the interpreter names a lambda's FnDef.
same "a mutable global, a redefinition, a closure over a global: as interpreted" \
     "$(results "$interp" | sed 's/^=> #<fn __fn_[0-9]*>$/=> #<fn>/')" "$(results "$comp")"

# --- A slot outlives a region bracket (CLAUDE.md, region store hooks) -----------
cat > "$WORK/t3" <<'EOF'
(defdata ^heap Link (Link [v : int nxt : int]))
(defn link-v [l : Link] : int (match l (Link a b) a))
(def kept (with-region (fn [] (Link 41 0))))
(link-v kept)
(def ^mut held (Link 0 0))
(defn grab [n : int] : int (with-region (fn [] (do (set! held (Link n 0)) 1))))
(grab 7)
(link-v held)
EOF
out="$(repl_out --enable=compiled-repl < "$WORK/t3")"
same "a node made in a bracket and kept in a def is read after it" \
     "$(printf '=> #<fn link-v>\n=> Link { v = 41, nxt = 0 }\n=> 41\n=> Link { v = 0, nxt = 0 }\n=> #<fn grab>\n=> 1\n=> 7')" \
     "$(results "$out" | grep -v 'adt-type')"

# --- An early turn's code and data outlive its MIR context --------------------
# Each turn's context is finished once the turn has run; its machine code and
# its data sections are kept (jit_finish_kept_context).  A string literal, a
# lambda and a static array made in turn 1 are read 100 turns later.
{
    echo '(def s "kept-from-turn-1")'
    echo '(def g (fn [x : int] : int (* x 3)))'
    echo '(defn tbl [i : int] : int ```c static const int64_t t[4] = {7, 11, 13, 17}; return t[i & 3]; ```)'
    echo '(def h tbl)'
    for i in $(seq 1 100); do echo "(+ $i 0)"; done
    echo 's'
    echo '(g 5)'
    echo '(h 2)'
} > "$WORK/t4"
out="$(repl_out --enable=compiled-repl < "$WORK/t4")"
same "a string, a lambda and a static table from turn 1, read 100 turns later" \
     "$(printf '=> "kept-from-turn-1"\n=> 15\n=> 13')" "$(results "$out" | tail -3)"

# --- Results print as the interpreter prints them ------------------------------
# Show where the type has an instance (Vec, Map, a user's own), the
# constructor layout for a struct or ADT without one, nested.  A positional
# field shows its value alone, both ways (it printed "(null) = 3").
cat > "$WORK/t5" <<'EOF'
[1 2 3]
(defstruct Pt [x : int y : int])
(Pt 1 2)
(defdata Sh (Circle [r : float]) (Sq [s : int]) (Dot))
(Circle 2.5)
(Dot)
(some 3)
(none)
(ok 1)
#map{:a 1}
#set{1 2}
(vec-of "a" "b")
(defstruct Line [a : Pt b : Pt name : cstr])
(Line (Pt 1 2) (Pt 3 4) "l")
(defdata Box [A] (Box A))
(Box "s")
(defstruct P [x : int])
(definstance Show [P] (show [p] : String (string/from-cstr "P!")))
(P 1)
EOF
interp="$(repl_out < "$WORK/t5")"
comp="$(repl_out --enable=compiled-repl < "$WORK/t5")"
same "Show, structs, ADTs, nesting, generics: compiled matches interpreted" \
     "$(results "$interp")" "$(results "$comp")"
has "a positional field prints its value alone" "=> Some { 3 }" "$interp"

# A Vec whose elements have no Show prints each element as the REPL would.
# (The interpreter shows these with another type's instance: reported in
# docs/reported/repl-show-uses-wrong-instance-for-vec-elements.md.)
out="$(repl_out --enable=compiled-repl <<'EOF'
(defstruct Q [y : int])
[[(Q 1)] [(Q 2) (Q 3)]]
EOF
)"
same "a Vec of a type without Show, nested" \
     "$(printf '=> #<struct-type Q>\n=> [[Q { y = 1 }] [Q { y = 2 } Q { y = 3 }]]')" "$(results "$out")"

# --- Redefining a type drops the defs that hold its values -------------------------
# A slot keeps the old layout, and the def's source may now mean something
# else ((S 1) is a partial application once S has two fields), so the def is
# dropped, with a note, rather than read as the new type.
out="$(repl_out --enable=compiled-repl <<'EOF'
(defstruct S [a : int])
(def s (S 1))
(def keep 7)
(defstruct S [a : int b : int])
s
keep
(def s (S 2 3))
s
EOF
)"
has "the redefinition names the dropped def" \
    "note: S changed, so the defs holding its old values are dropped; define them again to use them: s" "$out"
has "reading it is a compile error, not a misread value" "unbound symbol 's'" "$out"
same "an unrelated def stays, and the dropped one can be defined again" \
     "$(printf '=> #<struct-type S>\n=> S { a = 1 }\n=> 7\n=> #<struct-type S>\n=> 7\n=> S { a = 2, b = 3 }\n=> S { a = 2, b = 3 }')" \
     "$(results "$out")"

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
