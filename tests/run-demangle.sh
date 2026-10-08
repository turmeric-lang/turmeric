#!/usr/bin/env bash
# run-demangle.sh -- end-to-end tests for `tur demangle`
# (docs/upcoming/profiling-plan.md, P0).
#
# The token recognizer's unit tests live in tests/mangle_test.c (built as
# tur_mangle_unit); this harness covers the CLI surface -- stream filtering,
# argument mode, the flags -- plus the one property that cannot be checked
# from a fixed table: the false-positive RATE over a real symbol table.
set -u
cd "$(dirname "$0")/.."

TUR="./build/tur"
[ -x "$TUR" ] || { echo "tests: $TUR not built; run 'cmake --build build' first" >&2; exit 2; }

PASS=0
FAIL=0

# check <name> <expected> <actual>
check() {
    local name="$1" want="$2" got="$3"
    if [ "$want" = "$got" ]; then
        PASS=$((PASS+1))
        echo "PASS $name"
    else
        FAIL=$((FAIL+1))
        echo "FAIL $name"
        echo "    expected: $want"
        echo "    actual:   $got"
    fi
}

# --- argument mode (c++filt's): one decoded name per line ------------------
check "args/module-qualified" "geom/vector/add2" \
      "$("$TUR" demangle geom__vector__add2)"
check "args/kebab-and-arrow"  "list->vec" \
      "$("$TUR" demangle list_hy_gtvec)"
check "args/bare-global"      "add-two" \
      "$("$TUR" demangle add_hytwo)"
check "args/trailing-predicate" "done?" \
      "$("$TUR" demangle done_qu)"
check "args/slash-namespaced" "hamt/get" \
      "$("$TUR" demangle hamt_slget)"
check "args/libc-guard-prefix" "double" \
      "$("$TUR" demangle tur_u_double)"
check "args/passthrough"      "tur_hamt_get" \
      "$("$TUR" demangle tur_hamt_get)"
check "args/multiple" \
      "$(printf 'geom/vector/add2\nlist->vec\nmain')" \
      "$("$TUR" demangle geom__vector__add2 list_hy_gtvec main)"

# --- stream mode: rewrite tokens, leave every other byte alone -------------
# Shaped like `perf report` output, which is the motivating use case.
perf_in=$(printf '%s\n' \
    '  32.10%  myprog  [.] geom__vector__add2+0x1f' \
    '  18.02%  myprog  [.] tur_hamt_get' \
    '   9.11%  myprog  [.] list_hy_gtvec' \
    '   4.00%  libc.so [.] malloc')
perf_want=$(printf '%s\n' \
    '  32.10%  myprog  [.] geom/vector/add2+0x1f' \
    '  18.02%  myprog  [.] tur_hamt_get' \
    '   9.11%  myprog  [.] list->vec' \
    '   4.00%  libc.so [.] malloc')
check "stream/perf-report" "$perf_want" "$(printf '%s\n' "$perf_in" | "$TUR" demangle)"

# Punctuation, indentation and repeated tokens survive untouched.
check "stream/punctuation" \
      "call add-two, then (geom/vector/add2); add-two again" \
      "$(echo 'call add_hytwo, then (geom__vector__add2); add_hytwo again' | "$TUR" demangle)"

# A stream with no trailing newline still flushes its final token.
check "stream/no-trailing-newline" "add-two" \
      "$(printf 'add_hytwo' | "$TUR" demangle)"

# Empty input is not an error.
check "stream/empty" "" "$(printf '' | "$TUR" demangle)"

# --- flags -----------------------------------------------------------------
# --strict: module-qualified names only.
check "strict/keeps-qualified" "geom/vector/add2 add_hytwo" \
      "$(echo 'geom__vector__add2 add_hytwo' | "$TUR" demangle --strict)"
# --annotate: keep the C spelling so downstream tools can still match it.
check "annotate" "geom/vector/add2[geom__vector__add2]" \
      "$(echo 'geom__vector__add2' | "$TUR" demangle --annotate)"
# An unknown flag is a usage error, not a silent pass-through.
echo 'x' | "$TUR" demangle --nope >/dev/null 2>&1
check "flags/unknown-rejected" "2" "$?"

# --- precision ratchet -----------------------------------------------------
# ./build/tur is pure C: it contains no Turmeric-derived symbols, so EVERY
# rewrite over its symbol table is a false positive. Recognition is
# undecidable from text alone (see the header of src/cli/demangle.c), so the
# bar is a rate, not zero -- but it is a rate that must not drift. Adding a
# noisy mnemonic to the signal set in is_signal_mnemonic() sends this
# straight back to ~149, which is where it started.
#
# Measured at 8 when this landed. The ceiling is deliberately loose so a
# different toolchain's symbol table does not fail the build spuriously; it is
# tight enough to catch a regression of the kind described above.
FP_CEILING=25
if command -v nm >/dev/null 2>&1; then
    syms=$(nm "$TUR" 2>/dev/null | awk '{print $NF}' | sed 's/^_//' | sort -u)
    # The JIT engine (TUR_JIT, on by default since 2026-10-02) links the
    # vendored MIR from external/mir/: third-party C whose names are not ours
    # to tune the signal set against (`add__func__def`, and on macOS a dozen
    # `.cold.N` copies of it, each counted).  Measure tur's own C, as the
    # ceiling was calibrated: drop every symbol libtur_mir.a DEFINES (three
    # nm fields -- an undefined reference has two), matched on the base name
    # so the compiler's `.cold.N` / `.part.N` clones go with it.
    mir_lib="$(dirname "$TUR")/libtur_mir.a"
    if [ -f "$mir_lib" ]; then
        mir_syms=$(nm "$mir_lib" 2>/dev/null | awk 'NF == 3 {print $3}' \
                   | sed 's/^_//; s/\..*$//' | sort -u)
        syms=$(printf '%s\n' "$syms" \
               | awk 'NR == FNR { m[$0] = 1; next }
                      { b = $0; sub(/\..*$/, "", b); if (!(b in m)) print }' \
                     <(printf '%s\n' "$mir_syms") -)
    fi
    n_syms=$(printf '%s\n' "$syms" | grep -c . || true)
    if [ "${n_syms:-0}" -lt 1000 ]; then
        echo "SKIP precision-ratchet - nm produced only ${n_syms:-0} symbols (stripped binary?)"
    else
        n_fp=$(paste -d' ' <(printf '%s\n' "$syms") \
                           <(printf '%s\n' "$syms" | "$TUR" demangle --annotate) \
                 | awk '$2 ~ /\[/' | grep -c . || true)
        if [ "${n_fp:-0}" -le "$FP_CEILING" ]; then
            PASS=$((PASS+1))
            echo "PASS precision-ratchet - $n_fp/$n_syms false positives (ceiling $FP_CEILING)"
        else
            FAIL=$((FAIL+1))
            echo "FAIL precision-ratchet - $n_fp/$n_syms false positives exceeds $FP_CEILING"
            echo "    a mnemonic was probably added to is_signal_mnemonic() that a C"
            echo "    author's underscore also produces; see src/cli/demangle.c"
            paste -d' ' <(printf '%s\n' "$syms") \
                        <(printf '%s\n' "$syms" | "$TUR" demangle --annotate) \
              | awk '$2 ~ /\[/' | head -20 | sed 's/^/      /'
        fi
    fi
else
    echo "SKIP precision-ratchet - nm not available"
fi

echo
echo "demangle summary: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ] || exit 1
exit 0
