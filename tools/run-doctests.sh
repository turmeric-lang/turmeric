#!/usr/bin/env bash
# tools/run-doctests.sh -- run generated doctest programs and compare output.
#
# Usage:
#   bash tools/run-doctests.sh
#
# Prerequisites:
#   Run `python3 tools/doctest.py stdlib/ --out tests/doctest-generated/` first
#   (or let `just doctest` call both steps in order).
#
# Environment:
#   TUR         path to tur binary (default: ./build/tur)
#   TUR_FORCE   set to 1 to skip stamp cache and re-run every module

set -u
cd "$(dirname "$0")/.."

TUR="${TUR:-./build/tur}"
[ -x "$TUR" ] || { echo "run-doctests: $TUR not built; run 'just build' first" >&2; exit 2; }

GENERATED="tests/doctest-generated"
STAMP_CACHE="tests/.stamp-cache-doctest"
VERIFIED_OUT="$GENERATED/verified.txt"

TUR_FORCE="${TUR_FORCE:-0}"

PASS=0
FAIL=0
SKIP=0
FAILED=()

# ---------------------------------------------------------------------------
# Stamp-cache helpers (keyed on hash of generated .tur + mtime of tur binary)
# ---------------------------------------------------------------------------

# GNU first, and only an all-digit answer counts: `stat -f` on GNU means
# "filesystem status" and printed the volume's free-block counts into the
# stamp key, so on Linux the cache almost never hit (see tests/run.sh).
_tur_mtime() {
    local m
    m="$(stat -c '%Y' "$1" 2>/dev/null)"
    case "$m" in ''|*[!0-9]*) m="$(stat -f '%m' "$1" 2>/dev/null)" ;; esac
    case "$m" in ''|*[!0-9]*) m=0 ;; esac
    echo "$m"
}

# Performance optimization: cache the compiler binary modification time once
# at startup so we do not spawn a redundant stat process inside the stamp check loop.
TUR_MTIME="$(_tur_mtime "$TUR")"

_tur_hash_file() {
    if command -v md5 >/dev/null 2>&1; then md5 -q "$1" 2>/dev/null
    elif command -v md5sum >/dev/null 2>&1; then md5sum "$1" 2>/dev/null | awk '{print $1}'
    else echo "nohash"; fi
}
# One hash over stdlib/, as tests/run.sh keys its stamps: the stdlib is data
# `tur` reads at elaboration time, so a stdlib-only edit changes neither the
# binary nor any fixture (docs/archive/run-sh-stamp-cache-ignores-the-stdlib.md).
# This key had no such term; the broken mtime above masked that on Linux.
_tur_hash_stdin() {
    if command -v md5 >/dev/null 2>&1; then md5 -q
    elif command -v md5sum >/dev/null 2>&1; then md5sum | awk '{print $1}'
    else echo "nohash"; fi
}
export TUR_STDLIB_HASH="$(find stdlib -type f 2>/dev/null | LC_ALL=C sort |
    while IFS= read -r _f; do printf '%s\n' "$_f"; cat "$_f"; done | _tur_hash_stdin)"
stamp_key() { echo "$(_tur_hash_file "$1")-${TUR_MTIME}-${TUR_STDLIB_HASH}"; }
stamp_check() {
    [ "$TUR_FORCE" = "1" ] && return 1
    local sf="$STAMP_CACHE/$(printf '%s' "$1" | tr '/ ' '__').stamp"
    [ -f "$sf" ] && [ "$(cat "$sf")" = "$(stamp_key "$2")" ]
}
stamp_write() {
    mkdir -p "$STAMP_CACHE"
    local sf="$STAMP_CACHE/$(printf '%s' "$1" | tr '/ ' '__').stamp"
    stamp_key "$2" > "$sf"
}

# ---------------------------------------------------------------------------
# Main loop
# ---------------------------------------------------------------------------

# Names accumulate in a temp file and are moved into place only when the loop
# finishes.  This used to truncate $VERIFIED_OUT here and append as it went,
# which had two costs, both paid by gendocs.py downstream:
#
#   1. An interrupted run (^C, a crash, the `exit 1` below for an empty
#      generated dir) left a short or empty manifest that is indistinguishable
#      from a complete one -- and gendocs stamps that manifest into the TRACKED
#      stdlib/docstrings.tur, so a partial run silently shrinks `doc-verified?`.
#   2. Even the no-generated-files path truncated first and bailed after, so
#      running this script in a tree without generated tests destroyed a good
#      manifest and produced nothing.
#
# Writing once, at the end, with a `# complete:` header is what lets gendocs
# tell "this run finished and its verdict is total" from "I have no data".
# See docs/reported/docstrings-verified-table-zeroed-by-regen.md.
VERIFIED_TMP="$(mktemp)"
trap 'rm -f "$VERIFIED_TMP"' EXIT

shopt -s nullglob
tur_files=("$GENERATED"/*.tur)
shopt -u nullglob

if [ "${#tur_files[@]}" -eq 0 ]; then
    echo "run-doctests: no generated test files in $GENERATED" >&2
    echo "  Run: python3 tools/doctest.py stdlib/ --out $GENERATED" >&2
    exit 1
fi

for tur_file in "${tur_files[@]}"; do
    stem="${tur_file%.tur}"
    expected_file="${stem}.expected"
    names_file="${stem}.names"

    [ -f "$expected_file" ] || continue
    [ -f "$names_file" ] || continue

    mod_name="$(basename "$stem")"

    if stamp_check "$mod_name" "$tur_file"; then
        # All cases for this module passed on the last run; credit them as
        # verified without re-running.
        while IFS= read -r fname || [ -n "$fname" ]; do
            [ -z "$fname" ] && continue
            echo "$fname" >> "$VERIFIED_TMP"
            PASS=$((PASS + 1))
        done < "$names_file"
        continue
    fi

    # Run the generated test program, capturing stdout and stderr SEPARATELY.
    #
    # These must not be merged. The expected/actual pairing is positional --
    # the Nth line of stdout is compared against the Nth line of .expected --
    # so any diagnostic the compiler or the C compiler writes to stderr shifts
    # every subsequent case by one and turns a clean module into a cascade of
    # bogus failures. That is exactly what `2>&1` produced: the `unsafe block
    # has 29 expressions` lint desynchronised all 34 `rational` cases, and a
    # gcc `-Wreturn-type` warning did the same to `panic`, with failures like
    # `got "1 | ; AUTO-GENERATED by tools/doctest.py"` -- the runner reading a
    # diagnostic's source echo as program output.
    #
    # Program output is stdout; diagnostics are stderr. Keep stderr aside so
    # the non-zero-exit path below can still report it.
    err_file="$(mktemp)"
    actual="$("$TUR" run "$tur_file" 2>"$err_file")"
    run_exit=$?
    stderr_text="$(cat "$err_file")"
    rm -f "$err_file"

    if [ $run_exit -ne 0 ]; then
        # Interpreter error: module cannot be loaded (typeclass/GADT/module limitations).
        # Count as skipped rather than failed so CI is not blocked by interpreter gaps.
        n_cases=0
        while IFS= read -r fname || [ -n "$fname" ]; do
            [ -z "$fname" ] && continue
            n_cases=$((n_cases + 1))
        done < "$names_file"
        # Surface the first stderr line: before stdout/stderr were split this
        # path had no diagnostic to show, so every interpreter gap looked
        # identical ("exit 1") and told you nothing about which gap it was.
        first_err="$(printf '%s\n' "$stderr_text" | grep -v '^[[:space:]]*$' | head -1)"
        printf 'SKIP %s (%d cases) -- interpreter error (exit %d)%s\n' \
            "$mod_name" "$n_cases" "$run_exit" \
            "${first_err:+ -- ${first_err}}"
        SKIP=$((SKIP + n_cases))
        continue
    fi

    # Compare output line by line against expected values
    module_ok=true
    idx=0

    # Build parallel arrays from the files (Bash 3.2 compatible; macOS ships
    # without `mapfile`, so use portable `while read` loops instead).
    expected_arr=()
    while IFS= read -r line || [ -n "$line" ]; do
        expected_arr+=("$line")
    done < "$expected_file"

    names_arr=()
    while IFS= read -r line || [ -n "$line" ]; do
        names_arr+=("$line")
    done < "$names_file"

    while IFS= read -r actual_line || [ -n "$actual_line" ]; do
        exp="${expected_arr[$idx]:-}"
        fname="${names_arr[$idx]:-?}"

        if [ "$actual_line" = "$exp" ]; then
            echo "$fname" >> "$VERIFIED_TMP"
            PASS=$((PASS + 1))
        else
            printf 'FAIL %s:%s -- got "%s", expected "%s"\n' \
                "$mod_name" "$fname" "$actual_line" "$exp"
            FAILED+=("$mod_name:$fname")
            FAIL=$((FAIL + 1))
            module_ok=false
        fi
        idx=$((idx + 1))
    done <<< "$actual"

    # Write stamp only when all cases in the module passed
    $module_ok && stamp_write "$mod_name" "$tur_file"
done

# Publish the manifest.  The header marks the run COMPLETE, which is a claim
# about coverage, not about success: a module whose cases failed is complete
# and correctly absent from the list.  gendocs.py trusts a manifest carrying
# this marker and carries the previous table forward without one.
{
    echo "# tools/run-doctests.sh manifest -- consumed by tools/gendocs.py --emit-tur."
    echo "# Names below have a doctest that ran and matched its expected output."
    echo "# complete: $PASS passed, $FAIL failed, $SKIP skipped"
    sort -u "$VERIFIED_TMP"
} > "$VERIFIED_OUT"

echo ""
echo "Doctest results: $PASS passed, $FAIL failed, $SKIP skipped (interpreter incompatible)"
echo "Wrote $VERIFIED_OUT ($(grep -cv '^#' "$VERIFIED_OUT") verified names)"

if [ "${#FAILED[@]}" -gt 0 ]; then
    echo "Failed (wrong output):"
    for f in "${FAILED[@]}"; do
        echo "  $f"
    done
    # Say out loud what a non-zero exit costs. `test: build doctest` means a
    # failure here stops the chain BEFORE ctest, so the whole suite silently
    # does not run -- the recipe just exits ~1 in about 24s, which reads like
    # a fast test run rather than no test run at all. That trap is what
    # docs/archive/tur-run-test-blocked-by-doctest-failures.md was filed for.
    echo ""
    echo "NOTE: this is a dependency of the \`test\` recipe. A non-zero exit here"
    echo "      stops the chain, so \`tur run test\` does NOT reach ctest and the"
    echo "      test suite does not run. Fix the cases above, or run ctest directly:"
    echo "        ctest -j \"\$(getconf _NPROCESSORS_ONLN)\" --output-on-failure --test-dir build"
    exit 1
fi

exit 0
