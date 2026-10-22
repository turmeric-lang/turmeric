#!/usr/bin/env bash
# tests/turi/repl-lang-saffron.sh -- saffron-lang-plan S8: the REPL can be a
# Saffron session, by flag or by `#lang` at the prompt.
#
# The distinguishing probe is a FLOAT ADD through an unannotated defn:
#
#   (defn add [a b] (+ a b))
#   (add 1.5 2.25)
#
# Under Turmeric's defaults the parameters are `int`, so this is
# `TUR-E0001: function 'add' arg 1: expected int, got float`.  Under Saffron
# they are `any` and it answers 3.75.  `(add 1 2)` would NOT distinguish them --
# it is 3 either way -- which is why every case here uses fractional floats.
#
# What this pins, and why each half existed:
#
#   1. `--lang saffron` starts a Saffron session.  New flag.
#   2. `#lang saffron` at the prompt switches an already-running one.  This was
#      ACCEPTED AND SILENTLY IGNORED before: the REPL read the line with
#      the plain `detect_lang`, which reports only the READER axis, so `saffron`
#      came back as plain `turmeric`, the "already set" early-out fired, and the
#      language half was dropped.  A reader following docs/guides/saffron-guide.md
#      types that line first.
#   3. The DEFAULT is still Turmeric.  Without this a "fix" that made every
#      session Saffron would pass 1 and 2.
#   4. A bad `--lang` is REPORTED, exits nonzero, and does not start a
#      session -- it must not fall back to Turmeric silently.  The exit code
#      was not asserted at first: every error path in this subcommand reused
#      `usage_repl()`, which returns 0, so `tur repl --bogus-flag` also exited
#      0 (cli-usage-error-paths-exit-zero, since fixed: error paths go through
#      `drv_usage_error`, which exits 2, and the status is asserted here too).
#
# Skips cleanly (exit 0) when the binary is missing, like its siblings.

set -uo pipefail
cd "$(dirname "$0")/../.."

TUR="${TUR:-./build/tur}"
[ -x "$TUR" ] || [ ! -x "${TUR}.exe" ] || TUR="${TUR}.exe"
if [ ! -x "$TUR" ]; then
    echo "SKIP: $TUR not built"
    echo "TUR_SKIP: $TUR not built"
    exit 0
fi

fails=0

# `2>&1` throughout: the Turmeric case's answer is a DIAGNOSTIC, so a
# stdout-only capture would see an empty string for both arms and pass by
# accident.
run_repl() {   # run_repl <input> [flags...]
    local input="$1"; shift
    printf '%s' "$input" | "$TUR" repl "$@" 2>&1
}

SAFFRON_PROBE='(defn add [a b] (+ a b))
(add 1.5 2.25)
(defn k [x] (type-of x))
(k 7.1)
'

# --- 1. --lang saffron -----------------------------------------------------
out="$(run_repl "$SAFFRON_PROBE" --lang saffron)"
if grep -q '3\.75' <<<"$out" && grep -q '"float"' <<<"$out"; then
    echo "ok   --lang saffron: unannotated params are any (3.75, \"float\")"
else
    echo "FAIL --lang saffron: expected 3.75 and \"float\""
    sed 's/^/       /' <<<"$out" | tail -8
    fails=$((fails + 1))
fi

# --- 2. #lang saffron at the prompt ----------------------------------------
out="$(run_repl "#lang saffron
$SAFFRON_PROBE")"
if grep -q '3\.75' <<<"$out" && grep -q 'language set to saffron' <<<"$out"; then
    echo "ok   #lang saffron at the prompt: switches the language, says so"
else
    echo "FAIL #lang saffron at the prompt: expected a language switch and 3.75"
    sed 's/^/       /' <<<"$out" | tail -8
    fails=$((fails + 1))
fi

# --- 3. the default is still Turmeric --------------------------------------
out="$(run_repl '(defn add [a b] (+ a b))
(add 1.5 2.25)
')"
if grep -q 'expected int, got float' <<<"$out"; then
    echo "ok   default: still Turmeric (int parameter default)"
else
    echo "FAIL default: a plain REPL should keep Turmeric's int default"
    sed 's/^/       /' <<<"$out" | tail -8
    fails=$((fails + 1))
fi

# --- 4. a bad dialect is reported, and starts nothing -----------------------
out="$("$TUR" repl --lang saffrom 2>&1)"; rc=$?
if [ "$rc" -ne 0 ] && grep -q "unknown --lang 'saffrom'" <<<"$out" \
   && ! grep -q 'type :help for help' <<<"$out"; then
    echo "ok   --lang saffrom: named in an error, exit $rc, no session started"
else
    echo "FAIL --lang saffrom: expected the error, a nonzero exit (got $rc) and no REPL banner"
    sed 's/^/       /' <<<"$out" | head -4
    fails=$((fails + 1))
fi

if [ "$fails" -ne 0 ]; then
    echo "repl-lang-saffron: $fails check(s) failed"
    exit 1
fi
echo "repl-lang-saffron: all checks passed"
