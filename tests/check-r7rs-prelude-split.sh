#!/usr/bin/env bash
# tests/check-r7rs-prelude-split.sh -- r7rs-programs-compile-slowly: a
# `#lang r7rs` program built as two units (src/compiler/emit_split.h) keeps
# ONE copy of every piece of state.
#
# The library unit (the runtime preamble and the auto-loaded stdlib) is
# compiled once and cached; the program unit declares what it uses from it.
# A variable both units define is two variables: a parameter object, a
# handler stack or the collector's heap would silently fork, and the program
# would run on one copy while the prelude runs on the other.  The emitter
# decides unit by unit what to define and what to declare, and the state
# transform (emit_split_state) rewrites the text it writes verbatim.  This is
# the mechanical check on both: for each program below,
#
#   - no data symbol is DEFINED in both objects (`nm`: B/b D/d G/g S/s),
#     except the per-unit `any` name tables each unit registers for itself;
#   - two different programs link the SAME cached library object, so the
#     cache is doing its job;
#   - each program prints its fixture's expected output.
#
# Read-only data (R/r) may be duplicated freely.  The keyword records are
# read-only too, but they must NOT be weak: PE/COFF has no weak data that
# folds, so the library unit defines each record and the program unit
# declares it (docs/reported/r7rs-prelude-split-wrong-symbols-on-windows.md).
# A weak `__tur_sym_` symbol (V/v) in either unit fails.
set -uo pipefail
cd "$(dirname "$0")/.."
TUR_REL="${TUR:-./build/tur}"
TUR="$(cd "$(dirname "$TUR_REL")" && pwd)/$(basename "$TUR_REL")"
if [ ! -x "$TUR" ]; then
    echo "check-r7rs-prelude-split: $TUR not built" >&2
    exit 2
fi
CC_BIN="${CC:-cc}"
case "$(uname -m)" in
    x86_64|amd64|aarch64|arm64) ;;
    *) echo "SKIP check-r7rs-prelude-split: 64-bit targets only"; exit 0 ;;
esac
# The split is on by default on Linux and Darwin (prelude_split_applies,
# src/main.c); Windows keeps one unit until its report closes
# (docs/reported/r7rs-prelude-split-wrong-symbols-on-windows.md).
case "$(uname -s)" in
    Linux|Darwin) ;;
    *) echo "SKIP check-r7rs-prelude-split: the split is off by default on $(uname -s)"; exit 0 ;;
esac
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
FAILED=0
fail() { echo "FAIL check-r7rs-prelude-split: $*"; FAILED=1; }

# Per-unit by design: each unit registers its own rows of `any` type names
# (Mach-O spells a C name with a leading `_`).
ALLOW='^_?(__tur_any_chunk|__tur_any_rows)$'

# WRITABLE data only -- read-only data may be duplicated freely, and the two
# platforms need different questions asked to tell the two apart.
#
# ELF: `nm`'s one-letter class already separates them, R/r being rodata.
#
# Mach-O: it does NOT.  Plain `nm` spells every non-text local `s` and every
# non-text external `S`, whatever section it sits in, so a `static const` table
# is indistinguishable from mutable state.  The collector pasted into both units
# has one (`tur_gc_class_size`), which made this check fail on Darwin the first
# time it ran there with nothing actually wrong.  `nm -m` names the section
# instead, so ask for the writable ones by name: __data / __bss / __common, plus
# the two a `__thread` variable produces (__thread_vars holds its descriptor,
# __thread_bss its initial image).  __TEXT,__const, __TEXT,__cstring and
# __DATA,__const are not state: the last one sits outside __TEXT only because it
# needs relocating on load, and is read-only thereafter.
#
# The ELF arm has the same blind spot one section over: `.data.rel.ro` is
# read-only after load but `nm` classes it `d`, so a `static const` table of
# function pointers reads as writable state there.  Nothing in the collector has
# one today -- r7gc.c's allocator table is a local for exactly this reason --
# and if that changes, this arm needs `nm --format=sysv` and a section filter
# too, not another name on ALLOW.
defined_data() {
    if [ "$(uname -s)" = Darwin ]; then
        # The `^_` keeps assembler-local labels (`ltmp1`, `l_.str`) out: a C
        # name in a Mach-O object always carries the leading underscore.
        nm -m "$1" 2>/dev/null |
            awk '/\(__DATA,__(data|bss|common|thread_vars|thread_bss)\)/ {
                     n = $NF; sub(/\.[0-9]+$/, "", n); if (n ~ /^_/) print n }' |
            sort -u
    else
        nm "$1" 2>/dev/null |
            awk '$2 ~ /^[BbDdGgSs]$/ && $3 ~ /./ { n = $3; sub(/\.[0-9]+$/, "", n); print n }' |
            sort -u
    fi
}

FIXTURES="r7rs-named-let-sum r7rs-strings r7rs-ports r7rs-procedure-identity r7rs-colon-identifiers r7rs-gc-seam r7rs-type-errors-raise"
# The library object each of the first two programs linked (bash 3.2 has no
# associative arrays, and macOS ships it).
LIB_A=""
LIB_B=""
for f in $FIXTURES; do
    src="tests/fixtures/$f/input.tur"
    [ -f "$src" ] || { fail "$f: no $src"; continue; }
    d="$TMP/$f"
    mkdir -p "$d"
    # A private copy: the program unit's C lands at a path derived from the
    # input's, which must not be one a concurrent suite run also writes.
    cp -r "tests/fixtures/$f/." "$d/"
    log="$d/build.log"
    if ! (cd "$d" && TUR_PRELUDE_SPLIT=1 TUR_SHOW_CC=1 "$TUR" build input.tur -o "$d/prog") \
            >"$log" 2>&1; then
        fail "$f: build failed"; sed 's/^/    /' "$log" | tail -20; continue
    fi
    if grep -q "prelude split declined\|prelude split build failed" "$log"; then
        fail "$f: the split was not used"; grep "split" "$log" | sed 's/^/    /'; continue
    fi
    link=$(grep '^CC: ' "$log" | grep -v ' -c -o ' | tail -1)
    # The driver shell-quotes every path it splices into the cc command (WP2 of
    # the security audit, D-1b), so the TUR_SHOW_CC line carries quoted tokens
    # -- '/tmp/tur-build/prelude/<hash>.o' -- where it used to carry bare ones.
    # Excluding the quote characters from these character classes is what makes
    # both extractors quote-agnostic: POSIX sh gets ' and cmd.exe gets ".
    lib=$(printf '%s\n' "$link" | grep -o "[^ '\"]*/prelude/[0-9a-f]*\.o" | head -1)
    cli=$(printf '%s\n' "$link" | sed "s/.* -o [^ ]* ['\"]\{0,1\}\([^ '\"]*\.c\)['\"]\{0,1\} .*/\1/")
    if [ -z "$lib" ] || [ ! -f "$lib" ] || [ ! -f "$cli" ]; then
        fail "$f: could not find the two units in the link line"
        printf '    %s\n' "$link"; continue
    fi
    case "$f" in
        r7rs-named-let-sum) LIB_A="$lib" ;;
        r7rs-strings)       LIB_B="$lib" ;;
    esac
    if ! "$CC_BIN" -O2 -std=c99 -w -fno-strict-aliasing -Isrc/runtime -c -o "$d/client.o" "$cli" \
            >"$d/cc.log" 2>&1; then
        fail "$f: the program unit does not compile on its own"; tail -5 "$d/cc.log"; continue
    fi
    weak=$(nm "$lib" "$d/client.o" 2>/dev/null | awk '$2 ~ /^[Vv]$/ && $3 ~ /__tur_sym_/ { print $3 }' | sort -u)
    if [ -n "$weak" ]; then
        fail "$f: weak keyword records (they do not fold on Windows):"
        printf '%s\n' "$weak" | sed 's/^/    /' | head -5
    fi
    both=$(comm -12 <(defined_data "$lib") <(defined_data "$d/client.o") | grep -Ev "$ALLOW")
    if [ -n "$both" ]; then
        fail "$f: state defined in both units:"
        printf '%s\n' "$both" | sed 's/^/    /' | head -20
    fi
    exp="tests/fixtures/$f/expected.stdout"
    if [ -f "$exp" ]; then
        if ! (cd "$d" && "$d/prog" >"$d/out" 2>/dev/null; true) || ! diff -q "$d/out" "$exp" >/dev/null; then
            fail "$f: output differs from $exp"
            diff "$d/out" "$exp" | head -10 | sed 's/^/    /'
        fi
    fi
done

if [ -n "$LIB_A" ] && [ -n "$LIB_B" ] && [ "$LIB_A" != "$LIB_B" ]; then
    fail "r7rs-named-let-sum and r7rs-strings built different library units" \
         "(${LIB_A##*/} vs ${LIB_B##*/}): the cache never hits"
fi

# r7rs-prelude-library-object-varies-with-the-program: the library unit is the
# stdlib's alone, so programs that differ only in what THEY define link the
# same object.  Two that used to fork it: a global named `f` (the CPS coloring
# resolved stdlib `__cons-fmap`'s callback parameter `f` to it,
# docs/archive/cps-coloring-resolves-a-parameter-to-a-same-named-global.md),
# and a module whose C block `#define`s a macro (hoisted into both units).
d="$TMP/stable"
mkdir -p "$d"
cat > "$d/cmod.tur" <<'TUREOF'
(defmodule cmod
  (export cmod-n)
  ```c
  #define CMOD_N 7
  ```
  (defn cmod-n [] : int
    ```c
    return CMOD_N;
    ```))
TUREOF
printf '#lang r7rs\n(import (scheme base) (scheme write) (turmeric cmod))\n(display (cmod-n))\n(newline)\n' \
    > "$d/withc.tur"
printf '#lang r7rs\n(import (scheme base) (scheme write))\n(define f (lambda (x) (+ x 1)))\n(define (g y) (f y))\n(display (g 1))\n(newline)\n' \
    > "$d/withf.tur"
for p in withc:7 withf:2; do
    name="${p%%:*}"; want="${p##*:}"
    if ! (cd "$d" && TUR_PRELUDE_SPLIT=1 TUR_SHOW_CC=1 "$TUR" build "$name.tur" -o "$d/$name") \
            >"$d/$name.log" 2>&1; then
        fail "stable/$name: build failed"; tail -20 "$d/$name.log" | sed 's/^/    /'; continue
    fi
    lib=$(grep '^CC: ' "$d/$name.log" | grep -v ' -c -o ' | tail -1 |
          grep -o "[^ '\"]*/prelude/[0-9a-f]*\.o" | head -1)
    if [ -n "$LIB_A" ] && [ "$lib" != "$LIB_A" ]; then
        fail "stable/$name built its own library unit (${lib##*/} vs ${LIB_A##*/}):" \
             "something the program defines reached the library unit's text"
    fi
    got=$("$d/$name" 2>/dev/null)
    [ "$got" = "$want" ] || fail "stable/$name printed '$got', not '$want'"
done
# ...nor an `-I` naming a directory with no C header in it (cause 3 of the
# same report): `tur build -I . p.tur` and `tur build p.tur` compile
# byte-identical library text, so they must link one object.
if (cd "$d" && TUR_PRELUDE_SPLIT=1 TUR_SHOW_CC=1 "$TUR" build -I "$d" withf.tur -o "$d/withI") \
        >"$d/withI.log" 2>&1; then
    lib=$(grep '^CC: ' "$d/withI.log" | grep -v ' -c -o ' | tail -1 |
          grep -o "[^ '\"]*/prelude/[0-9a-f]*\.o" | head -1)
    if [ -n "$LIB_A" ] && [ "$lib" != "$LIB_A" ]; then
        fail "stable/withI built its own library unit (${lib##*/} vs ${LIB_A##*/}):" \
             "an -I that supplies no header reached the cache key"
    fi
else
    fail "stable/withI: build failed"; tail -20 "$d/withI.log" | sed 's/^/    /'
fi

# r7rs-prelude-library-cold-compile: a cold cache compiles the library unit
# in pieces (emit_split_pieces), one per CPU, and links them into the one
# object.  Every piece has the unit's declarations and static helpers, so
# state could fork there too: a static variable left static in two pieces, or
# a function-local `static` in a helper two pieces compile.  Build the
# library from a cold cache whole (TUR_PRELUDE_JOBS=1) and in four pieces:
# no writable data name may be defined twice in the pieces' object and more
# times than in the whole one, and the program must print its expected
# output.  (Once where the whole unit has none is not a fork: exported from
# piece 0, a static the whole unit's optimizer dropped stays.)
# A panic site (`static const tur_site_t __tur_site_N = { "file", line }`,
# emit_site_ref_text) is read-only, but its string pointer needs a relocation,
# so it lands in .data.rel.ro and nm reports it as data.  A copy in each piece
# that names it forks nothing -- every copy holds the same file and line -- so
# it is not counted.
counted_data() {   # defined_data, one line per definition, piece prefix off
    if [ "$(uname -s)" = Darwin ]; then
        nm -m "$1" 2>/dev/null |
            awk '/\(__DATA,__(data|bss|common|thread_vars|thread_bss)\)/ {
                     n = $NF; sub(/\.[0-9]+$/, "", n); sub(/^_tur_sp_/, "_", n)
                     if (n ~ /^___tur_site_[0-9]+$/) next
                     if (n ~ /^_/) print n }' | sort
    else
        nm "$1" 2>/dev/null |
            awk '$2 ~ /^[BbDdGgSs]$/ && $3 ~ /./ {
                     n = $3; sub(/\.[0-9]+$/, "", n); sub(/^tur_sp_/, "", n)
                     if (n ~ /^__tur_site_[0-9]+$/) next
                     print n }' | sort
    fi
}
PF=r7rs-type-errors-raise
PLIB_1=""; PLIB_4=""
for jobs in 1 4; do
    d="$TMP/pieces-$jobs"
    mkdir -p "$d/tmp"
    cp -r "tests/fixtures/$PF/." "$d/"
    if ! (cd "$d" && TMPDIR="$d/tmp" TUR_PRELUDE_SPLIT=1 TUR_PRELUDE_JOBS=$jobs TUR_SHOW_CC=1 \
            "$TUR" build input.tur -o "$d/prog") >"$d/build.log" 2>&1; then
        fail "pieces: the build with TUR_PRELUDE_JOBS=$jobs failed"; tail -20 "$d/build.log" | sed 's/^/    /'
        continue
    fi
    lib=$(ls "$d"/tmp/tur-build/prelude/*.o 2>/dev/null | head -1)
    [ -n "$lib" ] || { fail "pieces: no library object with TUR_PRELUDE_JOBS=$jobs"; continue; }
    if [ "$jobs" = 4 ]; then
        PLIB_4="$lib"
        grep -q -- ' -r -nostdlib ' "$d/build.log" ||
            fail "pieces: TUR_PRELUDE_JOBS=4 compiled the library whole (see TUR_SHOW_CC)"
        grep -q "did not compile in" "$d/build.log" &&
            fail "pieces: the pieces did not compile: $(grep -m1 'did not compile in' "$d/build.log")"
    else
        PLIB_1="$lib"
    fi
    if ! (cd "$d" && "$d/prog" >"$d/out" 2>/dev/null; true) ||
       ! diff -q "$d/out" "tests/fixtures/$PF/expected.stdout" >/dev/null; then
        fail "pieces: with TUR_PRELUDE_JOBS=$jobs the output differs"
        diff "$d/out" "tests/fixtures/$PF/expected.stdout" | head -10 | sed 's/^/    /'
    fi
done
if [ -n "$PLIB_1" ] && [ -n "$PLIB_4" ]; then
    forked=$(awk 'NR == FNR { w[$0]++; next } { p[$0]++ }
                  END { for (n in p) if (p[n] > 1 && p[n] > w[n]) print n " (" p[n] " vs " w[n] + 0 ")" }' \
                 <(counted_data "$PLIB_1") <(counted_data "$PLIB_4"))
    if [ -n "$forked" ]; then
        fail "pieces: state defined in more than one piece (pieces vs whole):"
        printf '%s\n' "$forked" | sed 's/^/    /' | head -20
    fi
fi

if [ "$FAILED" -eq 0 ]; then
    echo "PASS check-r7rs-prelude-split"
fi
exit "$FAILED"
