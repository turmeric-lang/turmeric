#!/usr/bin/env bash
# tests/check-r7rs-srfi-prune.sh -- r7rs-srfi-plan S3: an SRFI costs a
# program only what the program reaches.
#
# `(import (srfi N))` splices the SRFI's definitions into the program once
# per compile (D3), and src/passes/srfi_prune.c drops each one nothing
# outside the SRFI files reaches before emission.  This compares `tur emit-c`
# for three programs:
#
#   p0  `(write 1)`, no SRFI;
#   p1  p0 importing (srfi 1) and calling none of it;
#   p2  p1 calling `fold`.
#
# and checks that:
#
#   - p1's C is p0's, up to the numbering of lifted lambdas and temporaries
#     (an unused import costs nothing);
#   - p2's C defines `fold` and the helpers it calls, and none of SRFI 1's
#     other procedures (spot-checked: lset-xor, delete, filter, partition);
#   - with the pass off (TUR_NO_SRFI_PRUNE=1), p1 does carry SRFI 1, so the
#     first check is the pass's doing and not an empty splice;
#   - p2 builds and prints the right answer.
#
# tests/run-r7rs-import.sh covers the cross-module cases (an SRFI a library
# uses and the program does not, procedures used as values).
set -uo pipefail
cd "$(dirname "$0")/.."
TUR_REL="${TUR:-./build/tur}"
TUR="$(cd "$(dirname "$TUR_REL")" && pwd)/$(basename "$TUR_REL")"
if [ ! -x "$TUR" ]; then
    echo "check-r7rs-srfi-prune: $TUR not built" >&2
    exit 2
fi
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
FAILED=0
fail() { echo "FAIL check-r7rs-srfi-prune: $*"; FAILED=1; }

# One file name for all three, in three directories: the program's module is
# named after its file, and that name is in the C.
mkdir -p "$TMP/p0" "$TMP/p1" "$TMP/p2"
printf '#lang r7rs\n(import (scheme base) (scheme write))\n(write 1)\n' > "$TMP/p0/prog.scm"
printf '#lang r7rs\n(import (scheme base) (scheme write) (srfi 1))\n(write 1)\n' > "$TMP/p1/prog.scm"
printf '#lang r7rs\n(import (scheme base) (scheme write) (srfi 1))\n(write (fold + 0 (list 1 2 3)))\n' > "$TMP/p2/prog.scm"

for p in p0 p1 p2; do
    if ! (cd "$TMP/$p" && "$TUR" emit-c prog.scm > prog.c 2> emit.err); then
        fail "$p: tur emit-c failed: $(grep -vE 'W006[01]' "$TMP/$p/emit.err" | head -3)"
    fi
done
(cd "$TMP/p1" && TUR_NO_SRFI_PRUNE=1 "$TUR" emit-c prog.scm > unpruned.c 2>/dev/null)

# Lifted lambdas, envs and temporaries are numbered in elaboration order, and
# the splice's own definitions shift the numbers; the digits are not the code.
norm() { sed -E 's/[0-9]+/N/g' "$1"; }
if ! diff <(norm "$TMP/p0/prog.c") <(norm "$TMP/p1/prog.c") > "$TMP/p0p1.diff"; then
    fail "an unused (import (srfi 1)) changes the C beyond numbering ($(grep -c '^[<>]' "$TMP/p0p1.diff") lines differ):"
    head -10 "$TMP/p0p1.diff"
fi
if ! grep -q 'srfi1_' "$TMP/p1/unpruned.c"; then
    fail "with the pass off, p1's C holds no SRFI 1 definition -- the check above proves nothing"
fi
# The C spelling of srfi1--NAME: `-` is `_hy`, so srfi1--fold is srfi1_hy_hyfold.
if ! grep -q 'srfi1_hy_hyfold' "$TMP/p2/prog.c"; then
    fail "p2 calls fold, but its C does not define it"
fi
for name in lset_hyxor delete filter partition; do
    if grep -q "srfi1_hy_hy$name\b" "$TMP/p2/prog.c"; then
        fail "p2 calls only fold, but its C still defines SRFI 1's $name"
    fi
done
out="$(cd "$TMP/p2" && "$TUR" build prog.scm -o prog 2>/dev/null && ./prog)"
if [ "$out" != "6" ]; then
    fail "p2 printed '$out', expected '6'"
fi

# r7rs-srfi-18-216-sicp-plan T0b: (srfi 216) imports (srfi 27) for `random`,
# and since T3 (srfi 18) for its threads.  Unused, it must cost what an
# unused SRFI 1 does -- no procedure of either, nor of SRFI 18's C half
# (stdlib/r7rs/thread.tur).  What is left is not definitions the pass may
# drop: the record types' declarations (SRFI 27's one, SRFI 18's eight) and
# (scheme time)'s three procedures, about 9 KB of a 1.5 MB program, where
# SRFI 27 alone used to be 142 KB.  Calling `random` reaches the generator's
# drawing procedures and not the rest of SRFI 27 (pseudo-randomize!, the
# record's closures): spot-checked by name.
mkdir -p "$TMP/q1" "$TMP/q2"
printf '#lang r7rs\n(import (scheme base) (scheme write) (srfi 216))\n(write 1)\n' > "$TMP/q1/prog.scm"
printf '#lang r7rs\n(import (scheme base) (scheme write) (srfi 216))\n(write (list (< -1 (random 10) 10) (< 0.0 (random 1.5) 1.5)))\n' > "$TMP/q2/prog.scm"
for p in q1 q2; do
    if ! (cd "$TMP/$p" && "$TUR" emit-c prog.scm > prog.c 2> emit.err); then
        fail "$p: tur emit-c failed: $(grep -vE 'W006[01]' "$TMP/$p/emit.err" | head -3)"
    fi
done
p0_size=$(wc -c < "$TMP/p0/prog.c")
q1_size=$(wc -c < "$TMP/q1/prog.c")
if [ $((q1_size - p0_size)) -gt 16384 ]; then
    fail "an unused (import (srfi 216)) adds $((q1_size - p0_size)) bytes of C (budget 16384) -- is SRFI 27's default-random-source, or SRFI 18's current-thread parameter, kept again? TUR_SRFI_PRUNE_DEBUG=1 says what reached it"
fi
if grep -q 'srfi27_hy_hy\|srfi18_hy_hy\|r7rs_hythread' "$TMP/q1/prog.c"; then
    fail "an unused (import (srfi 216)) still defines SRFI 27 or SRFI 18 procedures: $(grep -o 'srfi27_hy_hy[A-Za-z0-9_]*\|srfi18_hy_hy[A-Za-z0-9_]*\|r7rs_hythread[A-Za-z0-9_]*' "$TMP/q1/prog.c" | sort -u | head -5 | tr '\n' ' ')"
fi
if ! grep -q 'srfi27_hy_hyrandom_hyinteger' "$TMP/q2/prog.c"; then
    fail "q2 calls random, but its C does not define SRFI 27's random-integer"
fi
for name in mrg32k3a_hypseudo_hyrandomize_hystate default_hyrandom_hysource make_hyrandom_hysource; do
    if grep -q "srfi27_hy_hy$name" "$TMP/q2/prog.c"; then
        fail "q2 calls only random, but its C still defines SRFI 27's $name"
    fi
done
out="$(cd "$TMP/q2" && "$TUR" build prog.scm -o prog 2>/dev/null && ./prog)"
if [ "$out" != "(#t #t)" ]; then
    fail "q2 printed '$out', expected '(#t #t)'"
fi

if [ $FAILED -ne 0 ]; then
    exit 1
fi
echo "PASS check-r7rs-srfi-prune: an unused (srfi 1) costs nothing; fold costs fold; an unused (srfi 216) carries no SRFI 27 or 18"
