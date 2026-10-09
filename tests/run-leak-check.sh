#!/usr/bin/env bash
# tests/run-leak-check.sh -- run COMPILED fixtures under AddressSanitizer with
# LeakSanitizer on.
#
# tests/run.sh compiles fixture programs WITHOUT sanitizers (run.sh:169) and
# runs them with detect_leaks=0 -- only `tur` itself is instrumented. So a leak,
# double free, or use-after-free in EMITTED code is invisible to the ordinary
# suite, which only checks printed output.
#
# Three harnesses already close that gap for one regression each
# (run-closure-env-leak.sh, run-fat-shim-leak.sh, run-gc-leak-gate.sh). This one
# generalizes the same mechanism so any fixture can opt in by dropping a
# `requires.leak-check` marker in its directory, instead of each new leak
# needing its own bespoke script.
#
# A fixture opted in must:
#   1. still print its expected.stdout, and
#   2. report no leak under LeakSanitizer.
#
# --------------------------------------------------------------------------
# What this can and cannot see
#
# LSan reports memory unreachable from roots. Anything held in a global registry
# is reachable BY CONSTRUCTION and will be called live no matter how dead it is
# -- run-gc-leak-gate.sh documents this at length for rc blocks in
# `gc_all_blocks`, and it is why that gate uses collector counters instead.
#
# So a clean run here means "nothing was orphaned", not "nothing was retained".
# For retention, count it (the CG6 (gc-live-blocks) style) rather than reaching
# for a leak checker.
# --------------------------------------------------------------------------
#
# Opt-in: a sanitized compile per fixture is slow, and this is a diagnostic gate
# rather than a correctness gate for everyday work.
#
# Exit status: 0 if every opted-in fixture is clean, 1 otherwise.

set -uo pipefail
cd "$(dirname "$0")/.."

TUR="${TUR:-./build/tur}"
BUILD_CC="${CC:-cc}"
PASS=0
FAIL=0
KNOWN=0

pass() { PASS=$((PASS + 1)); echo "PASS $1"; }
fail() { FAIL=$((FAIL + 1)); echo "FAIL $1 -- $2"; }

[ -x "$TUR" ] || { echo "leak-check: $TUR not built" >&2; exit 2; }

# LSan is Linux-only; on platforms without it ASan aborts at startup rather than
# reporting, which would read as a fixture failure rather than an unsupported
# host.  Skip cleanly instead, matching run-leak-gate.sh.
if [ "$(uname -s)" != "Linux" ]; then
    echo "leak-check: SKIP (LeakSanitizer is Linux-only; host is $(uname -s))"
    exit 0
fi

# TUR_CC_FLAGS REPLACES the default flags rather than appending, so the whole
# set has to be restated -- including the -L that finds the runtime archive.
# -O1 rather than -O2: ASan's stack traces are the point of a failure here, and
# -O2 inlines the allocation site out of them.
_build_dir="$(dirname "$TUR")"
CC_FLAGS="-O1 -g -std=c99 -Wall -fno-strict-aliasing -fsanitize=address -L${_build_dir}/src"

# Compile the runtime into each program rather than linking the prebuilt
# archive.  The archive is built WITHOUT ASan, so a use-after-free inside a
# runtime function -- rc_strong_decrement reading a control block the compiler
# already released -- ran uninstrumented and passed this gate
# (byvalue-recursive-shared-copies-leak: an rc-field struct copied out of a
# ^borrow double-decremented, and only `TUR_RUNTIME=source` showed it).  Set
# TUR_RUNTIME to override.
export TUR_RUNTIME="${TUR_RUNTIME:-source}"

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

shopt -s nullglob
# One nested level too (`tests/fixtures/typed/result-basic`); the work-dir
# name flattens the path so a nested fixture cannot collide with a top-level
# one of the same basename.
marked=(tests/fixtures/*/requires.leak-check tests/fixtures/*/*/requires.leak-check)
if [ ${#marked[@]} -eq 0 ]; then
    echo "leak-check: no fixture carries requires.leak-check"
    exit 0
fi

for marker in "${marked[@]}"; do
    dir="$(dirname "$marker")"
    base="$(basename "$dir")"
    name="$(printf '%s' "${dir#tests/fixtures/}" | tr '/' '_')"
    input="$dir/input.tur"
    [ -f "$input" ] || input="$dir/$base.tur"
    if [ ! -f "$input" ]; then
        fail "$name" "no input.tur"
        continue
    fi

    exe="$WORK/$name"
    # A `#lang saffron` program's allocator is the r7rs-gc collector by
    # default (any-widen-stored-in-an-adt-field-has-no-owner, 2026-09-28).
    # Its heap is invisible to LeakSanitizer -- reachable from the collector,
    # so nothing ever reads as leaked -- and ASan does not poison what it
    # frees, so a wrong static free would pass too.  This gate measures the
    # ownership the COMPILER emits, so build those on plain malloc/free;
    # tests/run-r7rs-gc.sh checks what the collector reclaims.
    saffron_gc=1
    IFS= read -r _first < "$input" || true
    [[ "$_first" == "#lang saffron"* ]] && saffron_gc=0
    if ! TUR_SAFFRON_GC="$saffron_gc" TUR_CC_FLAGS="$CC_FLAGS" CC="$BUILD_CC" \
         "$TUR" build "$input" -o "$exe" > "$WORK/$name.build" 2>&1; then
        fail "$name" "build failed: $(tail -3 "$WORK/$name.build" | tr '\n' ' ')"
        continue
    fi

    # exitcode=23 so a leak is distinguishable from the program's own nonzero
    # exit -- otherwise a fixture that legitimately exits 1 and one that leaks
    # look identical.
    # Capture to a FILE, not a $(...) substitution.  Command substitution strips
    # trailing newlines, so reconstructing the output with `printf '%s\n'` turns
    # a program that printed NOTHING into a single newline -- which differs from
    # a 0-byte expected.stdout and failed every such fixture as a bogus "stdout
    # mismatch".  Three of the first batch of opt-ins (rc-cycle-leak,
    # affine-drop, rc-elision-negative-conditional-drop) are exactly that shape.
    # use_stacks=0:use_registers=0: LeakSanitizer's default roots include
    # every stack and register, so a stale copy of a pointer left in a dead
    # frame keeps a leaked block "reachable".  Measured 2026-10-08
    # (carrier-sum-option-boxes-have-no-owner): the default hid 127 B across
    # the RM1 sweep, and a capturing closure in an `unwrap`ped Option read as
    # clean.  At exit main has returned, so nothing live is only on a stack.
    ASAN_OPTIONS="detect_leaks=1:exitcode=23" \
    LSAN_OPTIONS="use_stacks=0:use_registers=0" "$exe" \
        > "$WORK/$name.out" 2>"$WORK/$name.err"
    rc=$?

    if [ "$rc" -eq 23 ]; then
        summary=$(grep -m1 "SUMMARY: AddressSanitizer" "$WORK/$name.err" || echo "leak reported")
        # A fixture may carry `known-leak` naming an OPEN report.  It then shows
        # as KNOWN rather than failing the gate: the leak is real and filed, and
        # a permanently red gate is one nobody reads.  Delete the marker when
        # the report is fixed -- the gate turns red if the leak comes back.
        if [ -f "$dir/known-leak" ]; then
            KNOWN=$((KNOWN + 1))
            echo "KNOWN $name -- $summary"
            echo "    open report: $(head -1 "$dir/known-leak")"
            continue
        fi
        fail "$name" "$summary"
        sed -n '/Direct leak\|Indirect leak/,+4p' "$WORK/$name.err" | head -12 | sed 's/^/    /'
        continue
    fi

    # No leak, but the fixture claims one is known -- the report is stale and
    # the marker is now lying about the codebase.  That is worth failing on.
    if [ -f "$dir/known-leak" ]; then
        fail "$name" "marked known-leak but ran clean -- fix or delete $dir/known-leak"
        continue
    fi

    if [ -f "$dir/expected.stdout" ]; then
        if ! diff -q "$WORK/$name.out" "$dir/expected.stdout" > /dev/null 2>&1; then
            fail "$name" "stdout mismatch under ASan"
            continue
        fi
    fi
    pass "$name"
done

echo
echo "leak-check: $PASS passed, $FAIL failed, $KNOWN known-open"
[ "$FAIL" -eq 0 ]
