#!/usr/bin/env bash
# tests/run-stdlib-checks.sh -- type-check curated stdlib files in isolation.
#
# Runs `tur check` over an allowlist of stdlib/*.tur files.
# Non-auto-loaded files are checked with no extra flags.
# Auto-loaded files (Bucket A) are checked with --no-auto-stdlib, which
# suppresses re-loading the one file that matches the input so its
# definitions don't collide with itself; all other stdlib deps still load.
#
# To add a non-auto-loaded file: verify `./build/tur check stdlib/<file>`
# succeeds, then append to STDLIB_FILES with an empty flag entry.
#
# To add an auto-loaded (Bucket A) file: append to STDLIB_FILES with a
# "--no-auto-stdlib" flag entry.
#
# See docs/stdlib-check-allowlist-plan.md for the broader plan.
set -u
cd "$(dirname "$0")/.."

TUR="${TUR:-./build/tur}"
[ -x "$TUR" ] || { echo "tests: $TUR not built; run 'just build' first" >&2; exit 2; }

# Parallel arrays: file path and extra flags for that file (empty = none).
STDLIB_FILES=(
    stdlib/arrow.tur

    # Bucket A: auto-loaded stdlib files; need --no-auto-stdlib to avoid
    # duplicate-definition errors when checked in isolation.
    stdlib/macros.tur
    stdlib/safe.tur
    stdlib/contract.tur
    stdlib/hamt.tur
    stdlib/typeclass-eq.tur
    stdlib/typeclass-functor.tur
    stdlib/char.tur
    stdlib/map.tur
    stdlib/vec.tur
    stdlib/slice.tur
    stdlib/option.tur
    stdlib/result.tur
    stdlib/pair.tur
    stdlib/tuple.tur
    stdlib/list.tur
    stdlib/grid.tur
    stdlib/zipper.tur
    stdlib/set.tur
    stdlib/mutmap.tur

    # Bucket B: non-auto-loaded stdlib files that pass tur check standalone.
    # Files needing feature flags are documented inline.
    stdlib/effects.tur
    stdlib/future.tur
    stdlib/threadpool.tur
    stdlib/typeclass.tur
    stdlib/dynvar.tur
    stdlib/equal.tur
    stdlib/gadt-vec.tur
    stdlib/nat.tur
    stdlib/sized.tur
    # KB-030: Eq [str] is non-orphan now that the orphan checker credits a
    # built-in primitive type to its designated home file (str -> str.tur).
    stdlib/str.tur
    # interp-string-natives-and-range-show-plan: dependency-free leaf holding the
    # str-concat / int->str builders (loaded by str.tur and range-bound.tur).
    stdlib/str-build.tur
    # KB-027: rc.tur dispatches Functor/Foldable/Clone on the built-in rc<T>
    # constructor (kind * -> *, home rc.tur) instead of ptr<void>.
    stdlib/rc.tur
    # KB-029: session.tur checks under -Xsessions now that the un-expressible
    # tuple return annotation on echo-client-call is left to inference.
    stdlib/session.tur
    # S1: session-typed channel wrappers (parameterized defopaque phantoms).
    stdlib/schan.tur
    # N1/N2 (numeric-tower-rational-complex-plan): the exact Rational and the
    # hand-written Complex.  Both are pure Turmeric over typeclass.tur + math.tur.
    stdlib/rational.tur
    stdlib/complex.tur
)
STDLIB_FLAGS=(
    ""

    # Bucket A flags
    "--no-auto-stdlib"
    "--no-auto-stdlib"
    "--no-auto-stdlib"
    "--no-auto-stdlib"
    "--no-auto-stdlib"
    "--no-auto-stdlib"
    "--no-auto-stdlib"
    "--no-auto-stdlib"
    "--no-auto-stdlib"
    "--no-auto-stdlib"
    "--no-auto-stdlib"
    "--no-auto-stdlib"
    "--no-auto-stdlib"
    "--no-auto-stdlib"
    "--no-auto-stdlib"
    "--no-auto-stdlib"
    "--no-auto-stdlib"
    "--no-auto-stdlib"
    "--no-auto-stdlib"

    # Bucket B flags
    ""
    ""
    ""
    ""
    ""
    ""
    ""
    ""
    ""
    ""
    ""
    ""
    ""
    ""
    ""
    ""
)

PASS=0
FAIL=0
FAILED_FILES=()

for i in "${!STDLIB_FILES[@]}"; do
    f="${STDLIB_FILES[$i]}"
    flag="${STDLIB_FLAGS[$i]}"

    args=("$TUR" check)
    [[ -n "$flag" ]] && args+=("$flag")
    args+=("$f")

    if "${args[@]}" >/dev/null 2>&1; then
        PASS=$((PASS+1))
    else
        FAIL=$((FAIL+1))
        FAILED_FILES+=("$f")
        echo "FAIL $f"
        "${args[@]}" 2>&1 | sed 's/^/    /'
    fi
done

echo
echo "stdlib-checks: $PASS passed, $FAIL failed"
if [ "$FAIL" -gt 0 ]; then
    exit 1
fi
exit 0
