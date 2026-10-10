#!/usr/bin/env bash
# tests/turi/repl-smoke.sh — Phase S1 REPL smoke tests
# Verifies multi-line input, meta-commands (:help, :type, :doc), and
# persistent definitions across expressions.
set -euo pipefail

REPL="${1:-./build/tur}"
PASS=0
FAIL=0

# Pipe lines to the REPL and capture stdout (ANSI codes stripped for robustness)
repl_out() {
    printf '%s\n' "$@" | "$REPL" repl 2>/dev/null | sed 's/\x1b\[[0-9;]*m//g'
}

check() {
    local desc="$1"
    local expected="$2"
    local actual="$3"
    if grep -qF "$expected" <<< "$actual"; then
        echo "PASS: $desc"
        PASS=$((PASS + 1))
    else
        echo "FAIL: $desc"
        echo "  expected substring: $expected"
        echo "  got: $actual"
        FAIL=$((FAIL + 1))
    fi
}

# Compare the LAST `=> ...` line of a session, exactly.
#
# `check` above uses `grep -qF "$expected"`, and grep treats a MULTI-LINE fixed
# pattern as a set of alternatives -- any one line matching is a hit.  So a
# multi-line expectation written for `check` passes as soon as its first line
# appears anywhere, which for a before/after session is the BEFORE line: the
# check would report PASS on a compiler where the after-line is wrong.  Found
# the hard way while adding the reader-switch cases below.
check_last() {
    local desc="$1" expected="$2" actual="$3"
    local last
    last="$(echo "$actual" | grep '^=> ' | tail -1)"
    if [ "$last" = "$expected" ]; then
        echo "PASS: $desc"
        PASS=$((PASS + 1))
    else
        echo "FAIL: $desc"
        echo "  expected final result line: $expected"
        echo "  got:                        $last"
        FAIL=$((FAIL + 1))
    fi
}

# --- Basic evaluation ---
check "(+ 1 2)"       "=> 3"       "$(repl_out '(+ 1 2)')"
check "(* 6 7)"       "=> 42"      "$(repl_out '(* 6 7)')"
check "string literal" '"hello"'   "$(repl_out '"hello"')"

# --- Collection results render via their Show instance (repl-show-collections) ---
# The REPL preloads typeclass-show.tur so a bare Vec/Set/Map result prints its
# contents instead of an opaque pointer (turi_try_show_by_tag).
check "vec display"   "=> [1 2 3]"         "$(repl_out '(vec-of 1 2 3)')"
check "vec literal"   "=> [9 8 7]"         "$(repl_out '[9 8 7]')"
check "empty vec"     "=> []"              "$(repl_out '(vec-new)')"
check "set display"   "=> #set{7 8 9}"     "$(repl_out '(set-of 7 8 9)')"
check "map display"   "=> #map{1 10 2 20}" "$(repl_out '#map{1 10 2 20}')"
# (show 5) yields the cstr "5", which the prompt prints quoted.
check "show int"      '=> "5"'             "$(repl_out '(show 5)')"

# --- Carrier-list ops resolve to the runtime natives, not the unhandled
#     elaborator builtin (repl-list-head-over-cons-returns-nil).  Without the
#     native-function stubs the REPL preload injects, `cons` elaborates to a
#     BS_FUNC_CALL builtin the tree-walker cannot execute, so it (and any
#     list-head over it) returned nil at the prompt while --interpret and the
#     compiled path returned 65. ---
check "list-head over cons" "=> 65" "$(repl_out '(list-head (cons 65 (cons 66 0)))')"
check "head over cons"      "=> 42" "$(repl_out '(head (cons 42 0))')"

# --- Multi-line: split across two input lines ---
check "multi-line +"  "=> 3"  "$(repl_out '(+' '1 2)')"
check "multi-line let" "=> 7" "$(repl_out '(let [x 3' '      y 4] (+ x y))')"

# --- Persistent state ---
check "defn persists" "=> 25" "$(repl_out '(defn sq [x] (* x x))' '(sq 5)')"

# --- Reader macros persist across REPL turns (RM Q#5) ---
# A `(reader-macros/define ...)` on one line should affect subsequent reads;
# the registry is owned by TuriEnv, not the per-turn reader.
RM_OUT="$(repl_out "(reader-macros/define 'pi :none '3.14159)" '(println #pi)')"
check "reader macro persists" "3.14159" "$RM_OUT"

# --- :type meta-command ---
TYPE_OUT="$(repl_out ':type (+ 1 2)')"
check ":type int"     ": int"    "$TYPE_OUT"

TYPE_BOOL="$(repl_out ':type (< 1 2)')"
check ":type bool"    ": bool"   "$TYPE_BOOL"

# --- :doc meta-command ---
DOC_OUT="$(repl_out ':doc +')"
check ":doc +"        "add"      "$DOC_OUT"

DOC_DEFN="$(repl_out '(defn greet [] "hi")' ':doc greet')"
check ":doc user def" "greet"    "$DOC_DEFN"

# --- :help meta-command ---
HELP_OUT="$(repl_out ':help')"
check ":help shows :quit"    ":quit"   "$HELP_OUT"
check ":help shows :type"    ":type"   "$HELP_OUT"
check ":help shows :doc"     ":doc"    "$HELP_OUT"
check ":help shows :reload"  ":reload" "$HELP_OUT"

# --- :quit exits cleanly ---
EXIT_CODE=0
printf ':quit\n' | "$REPL" repl >/dev/null 2>&1 || EXIT_CODE=$?
if [ "$EXIT_CODE" -eq 0 ]; then
    echo "PASS: :quit exits 0"
    PASS=$((PASS + 1))
else
    echo "FAIL: :quit exited with code $EXIT_CODE"
    FAIL=$((FAIL + 1))
fi

# --- :reload <file> ---
TMP=$(mktemp /tmp/repl_smoke_XXXXXX.tur)
echo '(defn answer [] 42)' > "$TMP"
RELOAD_OUT="$(repl_out ":reload $TMP" '(answer)')"
rm -f "$TMP"
check ":reload file" "=> 42" "$RELOAD_OUT"

# --- :explain ---
# Case 1: Bare :explain with no recent diagnostic to explain
EXPLAIN_NONE_OUT="$(repl_out ":explain")"
check "bare :explain with no error" "no recent diagnostic to explain" "$EXPLAIN_NONE_OUT"

# Case 2: Specific code lookup (:explain TUR-E0001)
EXPLAIN_SPEC_OUT="$(repl_out ":explain TUR-E0001")"
check ":explain TUR-E0001" "Type mismatch" "$EXPLAIN_SPEC_OUT"

# Case 3: Specific code lookup normalized to uppercase (:explain tur-e0001)
EXPLAIN_NORM_OUT="$(repl_out ":explain tur-e0001")"
check ":explain tur-e0001" "Type mismatch" "$EXPLAIN_NORM_OUT"

# Case 4: Specific code miss (:explain TUR-E9999)
EXPLAIN_MISS_OUT="$(repl_out ":explain TUR-E9999")"
check ":explain TUR-E9999" "unknown diagnostic code 'TUR-E9999'" "$EXPLAIN_MISS_OUT"

# Case 5: Bare :explain after a diagnostic is triggered
EXPLAIN_AFTER_OUT="$(repl_out "(defn f [] invalid-name)" ":explain")"
check "bare :explain after unbound symbol" "Unbound symbol" "$EXPLAIN_AFTER_OUT"

# --- A `#lang` reader switch must not break GENERIC typeclass resolution ---
# lang-switch-breaks-generic-instance-resolution.
#
# A reader switch calls turi_env_reset_to_prelude, which drops the ElabSession
# and RE-ELABORATES the pinned prelude without re-evaluating it.  Generic bodies
# still bound in env->globals therefore keep a dict baked against the OLD
# TypeClassEnv while last_tc_env points at the rebuilt one, so the pointer
# compare in gde_reresolve_method matched nothing and the baked int-carrier
# representative answered every element `show`:
#
#     (show (:: #map{:a 1} (Map Sym int)))   => "#map{:a 1}"
#     #lang turmeric/neoteric
#     (show (:: #map{:a 1} (Map Sym int)))   => "#map{91328184844352 1}"
#
# Three properties of the check matter, each one having been got wrong first:
#
#  * The element type must ride the int64 carrier.  A Sym key does; an int key
#    renders correctly either way and measures nothing.
#  * It must be an explicit `(show ...)`, not a bare collection result.  A bare
#    result goes through the C-side auto-show tier (turi_try_show_by_tag), which
#    is NOT the broken path -- the report says so and it reproduces that way.
#  * Nothing may be `(load ...)`ed first.  Loading stdlib/str.tur to reduce the
#    rendering to an integer MASKS the defect completely: the load re-registers
#    instances into the live registry and the output is correct even on the
#    broken compiler.
#
# The monomorphic control beside each one is what says a failure is about
# CONSTRAINED-instance resolution rather than Show or Sym in general.
LANG_GENERIC='(show (:: #map{:a 1} (Map Sym int)))'

LS_BEFORE_OUT="$(repl_out "$LANG_GENERIC")"
check "generic Show[Map] before any switch" '=> "#map{:a 1}"' "$LS_BEFORE_OUT"

LS_AFTER_OUT="$(repl_out "$LANG_GENERIC" '#lang turmeric/neoteric' "$LANG_GENERIC")"
check_last "generic Show[Map] survives a reader switch" \
           '=> "#map{:a 1}"' "$LS_AFTER_OUT"

# Switch-back is the property that distinguished this defect from the
# stdlib-drop bug (web-repl-lang-switch-drops-stdlib): that one recovered when
# the original reader was restored, this one did not.
LS_BACK_OUT="$(repl_out "$LANG_GENERIC" '#lang turmeric/sweet' '#lang turmeric' "$LANG_GENERIC")"
check_last "generic Show[Map] survives switch and switch-back" \
           '=> "#map{:a 1}"' "$LS_BACK_OUT"

# Control: a monomorphic instance over the same primitive element type was never
# affected by this defect.
LS_MONO_OUT="$(repl_out '(show :hello)' '#lang turmeric/neoteric' '(show :hello)')"
check_last "monomorphic Show[Sym] survives a reader switch" \
           '=> ":hello"' "$LS_MONO_OUT"

# --- Redefining a defn on a LATER turn replaces it (PS4) ---
# duplicate-defn-in-one-file-reaches-the-c-compiler made a second defn of one
# name in one file (or one turn) an error.  An earlier turn's defn is the
# exception, exactly as for `def`: the prompt's redefine-and-retry workflow.
REDEF_OUT="$(repl_out '(defn f [] : int 1)' '(f)' '(defn f [] : int 2)' '(f)')"
check_last "defn redefined on a later turn" '=> 2' "$REDEF_OUT"
REDEF_ARITY_OUT="$(repl_out '(defn f [] : int 1)' '(f)' '(defn f [x : int] : int x)' '(f 9)')"
check_last "defn redefined with a new arity on a later turn" '=> 9' "$REDEF_ARITY_OUT"
# ...but two defns of one name in ONE turn are a duplicate, like two defs.
SAME_TURN_OUT="$(printf '%s\n' '(defn g [] : int 1) (defn g [] : int 2)' \
                 | "$REPL" repl 2>&1 | sed 's/\x1b\[[0-9;]*m//g')"
check "two defns of one name in one turn are rejected" \
      "defn: 'g' is already defined" "$SAME_TURN_OUT"

# --- A top-level (import ...) at the prompt (notebook-eval-no-module-base-dir) ---
# One turn imports, a later turn calls what it referred or aliased.  It used to
# be "import is only allowed inside defmodule" at the prompt.
IMP_DIR="$(mktemp -d)"
mkdir -p "$IMP_DIR/mods"
printf '%s\n' '(defmodule mods/sq (export sq) (defn sq [x : int] : int {x * x}))' \
    > "$IMP_DIR/mods/sq.tur"
REPL_ABS="$(cd "$(dirname "$REPL")" && pwd)/$(basename "$REPL")"
IMP_OUT="$(cd "$IMP_DIR" && printf '%s\n' '(import mods/sq :refer [sq])' '(sq 7)' \
           | TUR_NO_AUTO_SPICE=1 "$REPL_ABS" repl 2>/dev/null | sed 's/\x1b\[[0-9;]*m//g')"
check_last "top-level import :refer, called on a later turn" '=> 49' "$IMP_OUT"
IMP_AS_OUT="$(cd "$IMP_DIR" && printf '%s\n' '(import mods/sq :as m)' '(m/sq 6)' \
              | TUR_NO_AUTO_SPICE=1 "$REPL_ABS" repl 2>/dev/null | sed 's/\x1b\[[0-9;]*m//g')"
check_last "top-level import :as, called on a later turn" '=> 36' "$IMP_AS_OUT"
rm -rf "$IMP_DIR"

echo ""
echo "$PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
