#!/usr/bin/env bash
# tests/check-r7rs-split-lib-cache.sh -- r7rs-split-units-each-compile-the-runtime:
# a warm split build reads the library unit's emission back from the cache
# (SplitLibCache, src/driver/preamble_split.c) instead of emitting it, and nothing else about
# the build changes.  For each program below, in a private TMPDIR (so the
# cache starts empty):
#
#   - built with the cache off, then twice with it on, the program unit's text
#     and the objects it links (the library unit's and, for an import, the
#     imported-library unit's -- each named by a hash of its text) are the
#     same all three times;
#   - the last build took the library unit from the cache -- or, for a program
#     that reads a module of its own, says why it did not;
#   - TUR_SPLIT_LIB_CACHE=verify (which emits the unit anyway and compares)
#     finds the entry equal to what this program emits;
#   - the program prints its fixture's expected output.
#
# Then two damaged entries: one naming a binding no program has, one carrying
# a different signature for a library function.  Each build must notice, emit
# the unit instead, still print the right output, and put back an entry the
# next build reads.
#
# TUR_SPLIT_LIB_CACHE_ALL=1 runs the first part over every `#lang r7rs`
# fixture instead of the list below.
set -uo pipefail
cd "$(dirname "$0")/.."
TUR_REL="${TUR:-./build/tur}"
TUR="$(cd "$(dirname "$TUR_REL")" && pwd)/$(basename "$TUR_REL")"
if [ ! -x "$TUR" ]; then
    echo "check-r7rs-split-lib-cache: $TUR not built" >&2
    exit 2
fi
case "$(uname -m)" in
    x86_64|amd64|aarch64|arm64) ;;
    *) echo "SKIP check-r7rs-split-lib-cache: 64-bit targets only"; exit 0 ;;
esac
case "$(uname -s)" in
    Linux|Darwin) ;;
    *) echo "SKIP check-r7rs-split-lib-cache: the split is off by default on $(uname -s)"; exit 0 ;;
esac
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
mkdir -p "$TMP/tmp"
export TMPDIR="$TMP/tmp"
unset TUR_SPLIT_LIB_CACHE TUR_PRELUDE_SPLIT
# A fixture that links libturi needs to find it, as tests/run.sh arranges.
TUR_DIR="$(dirname "$TUR")"
export TUR_CC_FLAGS="${TUR_CC_FLAGS:--O2 -std=c99 -Wall -fno-strict-aliasing -L$TUR_DIR/src}"
FAILED=0
fail() { echo "FAIL check-r7rs-split-lib-cache: $*"; FAILED=1; }

# Programs whose library unit must come from the cache: a plain one, two that
# import stdlib libraries (three units), one whose imported-library split is
# declined (retried as two units, loading the entry twice), one with CPS tail
# groups (numbered per unit), and one that imports a Turmeric stdlib module.
FIXTURES="r7rs-named-let-sum r7rs-strings r7rs-srfi-13 r7rs-read r7rs-sicp-extras r7rs-cps-mutual-tail-unoptimized r7rs-gc-seam"
# Programs that read a module of their own: never cached.
NOCACHE="r7rs-include"
if [ "${TUR_SPLIT_LIB_CACHE_ALL:-0}" = 1 ]; then
    FIXTURES=""
    NOCACHE=""
    for d in tests/fixtures/*/; do
        f=$(basename "$d")
        [ -f "$d/input.tur" ] || continue
        # No pipe into grep -q under pipefail (tests/check-pipefail-grep-q.sh).
        head3=$(head -3 "$d/input.tur")
        [[ $'\n'"$head3" == *$'\n#lang r7rs'* ]] || continue
        FIXTURES="$FIXTURES $f"
    done
fi

# The program unit's .c and the cached objects the link names.
units() {
    local link
    link=$(grep '^CC: ' "$1" | grep -v ' -c -o ' | tail -1)
    printf '%s\n' "$link" | grep -o "[^ '\"]*/prelude/[0-9a-f]*\.o" | sed 's|.*/||' | sort | tr '\n' ' '
    echo
    printf '%s\n' "$link" | sed "s/.* -o [^ ]* ['\"]\{0,1\}\([^ '\"]*\.c\)['\"]\{0,1\} .*/\1/"
}

# build <dir> <tag> [env...]: build <dir>/input.tur, leaving <tag>.log,
# <tag>.objs, <tag>.c and the program <tag>.
build() {
    local d="$1" tag="$2"; shift 2
    local flags=""
    [ -f "$d/flags" ] && flags=$(cat "$d/flags")
    # shellcheck disable=SC2086
    if ! (cd "$d" && env "$@" TUR_SHOW_CC=1 "$TUR" $flags build input.tur -o "$d/$tag") \
            >"$d/$tag.log" 2>&1; then
        return 1
    fi
    units "$d/$tag.log" > "$d/$tag.units"
    head -1 "$d/$tag.units" > "$d/$tag.objs"
    local cli
    cli=$(sed -n 2p "$d/$tag.units")
    [ -f "$cli" ] && cp "$cli" "$d/$tag.c"
    return 0
}

check_output() {
    local f="$1" d="$2" tag="$3"
    local exp="tests/fixtures/$f/expected.stdout"
    [ -f "$exp" ] || return 0
    (cd "$d" && "$d/$tag" >"$d/$tag.out" 2>/dev/null; true)
    if ! diff -q "$d/$tag.out" "$exp" >/dev/null; then
        fail "$f ($tag): output differs from $exp"
        diff "$d/$tag.out" "$exp" | head -10 | sed 's/^/    /'
    fi
}

for f in $FIXTURES $NOCACHE; do
    [ -f "tests/fixtures/$f/input.tur" ] || { fail "$f: no input.tur"; continue; }
    d="$TMP/$f"
    mkdir -p "$d"
    # A private copy: the program unit's C lands at a path derived from the
    # input's, which must not be one a concurrent suite run also writes.
    cp -r "tests/fixtures/$f/." "$d/"
    build "$d" off TUR_SPLIT_LIB_CACHE=0 || { fail "$f: build failed (cache off)"; tail -5 "$d/off.log"; continue; }
    grep -q "library unit emitted" "$d/off.log" || { echo "SKIP $f: not a split build"; continue; }
    build "$d" first || { fail "$f: build failed (first with the cache)"; tail -5 "$d/first.log"; continue; }
    build "$d" warm || { fail "$f: build failed (warm)"; tail -5 "$d/warm.log"; continue; }
    case " $NOCACHE " in
        *" $f "*)
            grep -q "library unit not cached" "$d/warm.log" ||
                fail "$f: reads a module of its own, yet its library unit was cached"
            ;;
        *)
            if ! grep -q "library unit from the cache" "$d/warm.log"; then
                fail "$f: the warm build emitted the library unit"
                grep "^tur:" "$d/warm.log" | sed 's/^/    /' | head -10
            fi
            ;;
    esac
    for tag in first warm; do
        cmp -s "$d/off.c" "$d/$tag.c" ||
            fail "$f: the program unit differs with the cache ($tag build)"
        cmp -s "$d/off.objs" "$d/$tag.objs" ||
            fail "$f: links other library objects with the cache ($tag build): $(cat "$d/off.objs") vs $(cat "$d/$tag.objs")"
    done
    if grep -q "does not fit\|names bindings\|failed with the cached\|differs from the emitted" \
            "$d/first.log" "$d/warm.log"; then
        fail "$f: the cache entry did not fit"
        grep -h "^tur:" "$d/first.log" "$d/warm.log" | sed 's/^/    /' | head -10
    fi
    build "$d" verify TUR_SPLIT_LIB_CACHE=verify || { fail "$f: build failed (verify)"; continue; }
    if grep -q "differs from the emitted" "$d/verify.log"; then
        fail "$f: the cached library unit is not the one this program emits"
        grep "differs from the emitted" "$d/verify.log" | sed 's/^/    /'
    fi
    check_output "$f" "$d" warm
done

# ---- damaged entries -------------------------------------------------------
f=r7rs-named-let-sum
d="$TMP/$f"
key=$(grep -o "cache key [0-9a-f]*" "$d/warm.log" 2>/dev/null | head -1 | awk '{print $3}')
ent="$TMPDIR/tur-build/prelude/$key.lib"
if [ -z "$key" ] || [ ! -f "$ent" ]; then
    fail "damage: no cache entry for $f (key '${key}')"
else
    # damage <label> <awk program>: rewrite the entry in place (same length:
    # the header counts its bytes), build, and expect the fallback.
    damage() {
        local label="$1" prog="$2"
        awk "$prog" "$ent" > "$ent.new" && cat "$ent.new" > "$ent" && rm -f "$ent.new"
        build "$d" "dmg-$label" || { fail "damage/$label: build failed"; tail -5 "$d/dmg-$label.log"; return; }
        if grep -q "library unit from the cache" "$d/dmg-$label.log" &&
           ! grep -q "does not fit\|names bindings" "$d/dmg-$label.log"; then
            fail "damage/$label: the damaged entry was used"
        fi
        grep -q "library unit emitted" "$d/dmg-$label.log" ||
            fail "damage/$label: the library unit was not emitted in its place"
        cmp -s "$d/off.c" "$d/dmg-$label.c" || fail "damage/$label: the program unit differs"
        check_output "$f" "$d" "dmg-$label"
        build "$d" "after-$label" || { fail "damage/$label: rebuild failed"; return; }
        grep -q "library unit from the cache" "$d/after-$label.log" ||
            fail "damage/$label: the entry was not put back"
    }
    # A binding the record says the unit defines, renamed: its last letter
    # changed, so no program has it.
    damage name 'BEGIN{s=0;done=0} /^D [0-9]+$/{s=1; print; next}
                 s && !done && /^F\t/ {c=substr($0,length($0)); n=(c=="q")?"z":"q";
                 print substr($0,1,length($0)-1) n; done=1; next} {print}'
    # A library function's signature, changed (`int64_t` -> `int32_t`): the
    # program unit declares it otherwise, which refuses the split.
    damage sig 'BEGIN{s=0;done=0} /^S [0-9]+$/{s=1; print; next} /^Y [0-9]+$/{s=0}
                s && !done && /\tint64_t\(/ {sub(/\tint64_t\(/, "\tint32_t("); done=1} {print}'
    # An exported name the program unit uses, forgotten: its reference keeps
    # the unprefixed name, which nothing defines, so the link fails, and the
    # build is retried around an emitted library unit.  The header counts the
    # record's bytes, so it is rewritten too.
    # The prelude's toplevel runner (`r7rs-toplevel__`): `main` calls it, and
    # no runtime archive defines one by that name.
    used=$(grep -o 'tur_sl_r7rs_hytoplevel[A-Za-z0-9_]*(' "$d/warm.c" | head -1 |
           sed 's/^tur_sl_//; s/($//')
    if [ -z "$used" ]; then
        fail "damage/export: the program unit names nothing of the library unit"
    else
        awk -v drop="$used" '
            NR <= 3 { head[NR] = $0; next }
            /^E [0-9]+$/ { e = 1; sub(/[0-9]+$/, $2 - 1); rec[++n] = $0; next }
            /^B [0-9]+$/ { e = 0 }
            e && $0 == drop { next }
            { rec[++n] = $0 }
            END {
                len = 0
                for (i = 1; i <= n; i++) len += length(rec[i]) + 1
                split(head[3], h, " ")
                print head[1]; print head[2]; print h[1] " " h[2] " " len
                for (i = 1; i <= n; i++) print rec[i]
            }' "$ent" > "$ent.new" && cat "$ent.new" > "$ent" && rm -f "$ent.new"
        if ! build "$d" dmg-export; then
            fail "damage/export: build failed"; tail -5 "$d/dmg-export.log"
        else
            grep -q "failed with the cached library unit" "$d/dmg-export.log" ||
                fail "damage/export: the failed link was not retried around an emitted unit"
            check_output "$f" "$d" dmg-export
            build "$d" after-export && grep -q "library unit from the cache" "$d/after-export.log" ||
                fail "damage/export: the entry was not put back"
        fi
    fi
fi

if [ "$FAILED" = 0 ]; then
    echo "PASS check-r7rs-split-lib-cache"
fi
exit "$FAILED"
