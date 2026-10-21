#!/usr/bin/env bash
# tests/spice-c-sources-tests.sh -- spices-c-sources-plan.
#
# Exercises :c-sources / :c-includes under :build-opts:
#
#   - A single spice that vendors a hand-written .c plus a private header dir,
#     used by both the aux .c and an inline-C block in a .tur module, builds
#     and runs (the binary exits with the value the vendored C returns).
#   - :c-includes -I reaches the inline-C compile (the .tur block #includes the
#     private header).
#   - Cross-spice: a consumer that :spices-depends on a vendor spice links the
#     vendor's vendored .c into its binary.
#   - Parse-time validation: a missing source, a non-.c extension, and an
#     absolute path each fail the build with a DIAG_ERROR (not a segfault).
#   - Manifest round-trip: :c-sources / :c-includes survive read -> write
#     (via `tur add-cmake`, which splices its entry into build.tur through the
#     comment-preserving formatter; it used pkg_manifest_write until
#     add-cmake-manifest-rewrite-loses-keys).
#
# Owned by its own ctest target; the fixtures carry requires.dedicated-runner
# (and live under tests/fixtures/spices/, which the by-value run.sh does not
# descend into) so the main suite does not also try to build them.

set -u
cd "$(dirname "$0")/.."

TUR="${TUR:-./build/tur}"
[ -x "$TUR" ] || { echo "tests: $TUR not built; run 'tur run build' first" >&2; exit 2; }

# Leak detection is irrelevant to these CLI-level assertions and the
# build-failure paths intentionally exit before freeing everything; turn it off
# so a partial-free on an error path does not mask a real test failure.
export ASAN_OPTIONS="${ASAN_OPTIONS:+$ASAN_OPTIONS:}detect_leaks=0"

SPICES="tests/fixtures/spices"

PASS=0
FAIL=0
FAILED=()

# assert_build_runs <expected-binary-exit> <label> <spice-dir> <binary-name>
# Builds the spice, runs the produced binary, and asserts its exit code.
assert_build_runs() {
    local want="$1" label="$2" dir="$3" binname="$4"
    rm -rf "$dir/build"
    local berr; berr=$(mktemp)
    "$TUR" build "$dir" >/dev/null 2>"$berr"
    local brc=$?
    if [ "$brc" -ne 0 ]; then
        echo "FAIL $label -- build failed (exit $brc)"
        echo "  stderr:"; sed 's/^/    /' "$berr"
        FAIL=$((FAIL + 1)); FAILED+=("$label"); rm -f "$berr"; rm -rf "$dir/build"; return
    fi
    rm -f "$berr"
    local bin="$dir/build/bin/$binname"
    if [ ! -x "$bin" ]; then
        echo "FAIL $label -- expected binary '$bin' not produced"
        FAIL=$((FAIL + 1)); FAILED+=("$label"); rm -rf "$dir/build"; return
    fi
    "$bin" >/dev/null 2>&1
    local rrc=$?
    rm -rf "$dir/build"
    if [ "$rrc" -eq "$want" ]; then
        echo "PASS $label"
        PASS=$((PASS + 1))
    else
        echo "FAIL $label -- binary exited $rrc, want $want"
        FAIL=$((FAIL + 1)); FAILED+=("$label")
    fi
}

# assert_build_fails <needle> <label> <spice-dir>
# Asserts the build exits non-zero AND prints `needle` to stderr.
assert_build_fails() {
    local needle="$1" label="$2" dir="$3"
    rm -rf "$dir/build"
    local berr; berr=$(mktemp)
    "$TUR" build "$dir" >/dev/null 2>"$berr"
    local brc=$?
    rm -rf "$dir/build"
    if [ "$brc" -ne 0 ] && grep -F -q "$needle" "$berr"; then
        echo "PASS $label"
        PASS=$((PASS + 1))
    else
        echo "FAIL $label -- exit $brc (want non-zero) / stderr missing: $needle"
        echo "  stderr:"; sed 's/^/    /' "$berr"
        FAIL=$((FAIL + 1)); FAILED+=("$label")
    fi
    rm -f "$berr"
}

# --- Single-spice: vendored .c + private header reached by aux .c and inline-C.
assert_build_runs 42 \
    "single spice: vendored :c-sources + :c-includes build and run (exit 42)" \
    "$SPICES/spice-with-aux-c" "spice-with-aux-c"

# --- Cross-spice: consumer links the vendor spice's :c-sources.
assert_build_runs 7 \
    "cross-spice: consumer links dep's :c-sources (exit 7)" \
    "$SPICES/consumer-of-aux" "consumer-of-aux"

# --- Two hops: the vendor's .c reaches a consumer that only knows it through
# an intermediate spice (c-sources-propagate-only-one-level).
assert_build_runs 7 \
    "two hops: :c-sources reach a consumer via an intermediate spice (exit 7)" \
    "$SPICES/consumer-two-hops" "consumer-two-hops"

# --- Diamond: the vendor reached both directly and via the intermediate
# contributes its source exactly once (no duplicate symbols).
assert_build_runs 7 \
    "diamond: a vendor reached by two routes links once (exit 7)" \
    "$SPICES/consumer-diamond" "consumer-diamond"

# --- Error cases: each must DIAG_ERROR and fail the build, not segfault.
assert_build_fails "not found" \
    "error: missing :c-sources file fails the build" \
    "$SPICES/aux-c-missing-source"

assert_build_fails "must have a .c, .cc, or .cpp extension" \
    "error: non-.c :c-sources extension fails the build" \
    "$SPICES/aux-c-bad-extension"

assert_build_fails "must be a relative path" \
    "error: absolute :c-sources path fails the build" \
    "$SPICES/aux-c-absolute-path"

# --- Manifest round-trip: keys survive read -> write.
RT=$(mktemp -d)
mkdir -p "$RT/c/inc"; : > "$RT/c/foo.c"; : > "$RT/c/inc/foo.h"
cat > "$RT/build.tur" <<'EOF'
(defpackage rt-test
  :name "rt-test"
  :build-opts #{
    :c-includes ["c/inc"]
    :c-sources  ["c/foo.c"]
  })
EOF
RT_ABS_TUR="$PWD/$TUR"
( cd "$RT" && "$RT_ABS_TUR" add-cmake https://example.com/libfoo.git ) >/dev/null 2>&1
if grep -F -q ':c-includes ["c/inc"]' "$RT/build.tur" \
   && grep -F -q ':c-sources ["c/foo.c"]' "$RT/build.tur"; then
    echo "PASS round-trip: :c-sources / :c-includes survive read -> write"
    PASS=$((PASS + 1))
else
    echo "FAIL round-trip: keys lost after rewrite"
    echo "  build.tur:"; sed 's/^/    /' "$RT/build.tur"
    FAIL=$((FAIL + 1)); FAILED+=("round-trip")
fi
rm -rf "$RT"

# --- :build-opts link flags must reach a --shared link -----------------------
# The executable path honoured :link-libs / :link-flags from
# ffi-spices-integration-plan S1 on; cmd_build_multi_files (the `--shared
# <dir>` path) carried a stale copy of the cmake-manifest read that predated S1
# and silently dropped both. Assert on the CC line itself (TUR_SHOW_CC), which
# is the observable that was wrong: no library has to exist for -L to be
# emitted, so this runs identically on every platform.
SB="$SPICES/shared-build-opts-link"
rm -rf "$SB/build"
sb_cc=$(TUR_SHOW_CC=1 "$TUR" build --shared "$SB" 2>&1 | grep -m1 '^CC:')
rm -rf "$SB/build"
if [ -z "$sb_cc" ]; then
    echo "FAIL shared build-opts: no CC: line from TUR_SHOW_CC (did the --shared build run?)"
    FAIL=$((FAIL + 1)); FAILED+=("shared build-opts")
elif printf '%s' "$sb_cc" | grep -q -- ' -lm' \
     && printf '%s' "$sb_cc" | grep -q -- ' -L/tur-shared-build-opts-marker'; then
    echo "PASS shared build-opts: :link-libs and :link-flags reach the --shared link line"
    PASS=$((PASS + 1))
else
    echo "FAIL shared build-opts: :build-opts tokens missing from the --shared link line"
    echo "  CC: $sb_cc"
    FAIL=$((FAIL + 1)); FAILED+=("shared build-opts")
fi

# --- :build-opts :c-flags must reach the compile -------------------------------
# build-opts-c-flags-never-consumed: :c-flags was parsed, validated and
# documented, and no build path read it, so `-DMY_ANSWER=42` there left the
# inline-C body with an undeclared MY_ANSWER.
CF=$(mktemp -d -t tur-c-flags.XXXXXX)
mkdir -p "$CF/src"
cat >"$CF/build.tur" <<'EOF'
(defpackage cf
  :name "cf"
  :version "0.1.0"
  :build-opts #map{:c-flags ["-DMY_ANSWER=42"]})
EOF
cat >"$CF/src/main.tur" <<'EOF'
(defn answer [] : int
  ```c
  return MY_ANSWER;
  ```)
(defn main [] : int
  (println (answer))
  0)
EOF
cf_out=""
if "$TUR" build "$CF" >"$CF/build.log" 2>&1; then
    cf_out=$("$CF/build/bin/cf" 2>&1)
fi
if [ "$cf_out" = "42" ]; then
    echo "PASS c-flags: :build-opts :c-flags reach the compile"
    PASS=$((PASS + 1))
else
    echo "FAIL c-flags: :c-flags did not reach the compile (out=[$cf_out])"
    sed 's/^/    /' "$CF/build.log" | tail -n 5
    FAIL=$((FAIL + 1)); FAILED+=("c-flags")
fi

# --- :build-opts :no-stdlib true is refused, false is accepted ----------------
# build-opts-c-flags-never-consumed: :no-stdlib was parsed and read by
# nothing, so `true` silently built with the stdlib.  Until it means
# something the manifest reader refuses it.
sed -i.bak 's/#map{:c-flags \["-DMY_ANSWER=42"\]}/#map{:no-stdlib true}/' "$CF/build.tur"
printf '(defn main [] : int (println 7) 0)\n' >"$CF/src/main.tur"
ns_true_ok=0
if ! "$TUR" build "$CF" >"$CF/build.log" 2>&1 &&
   grep -q ':no-stdlib is not implemented' "$CF/build.log"; then
    ns_true_ok=1
fi
sed -i.bak 's/:no-stdlib true/:no-stdlib false/' "$CF/build.tur"
ns_false_out=""
if "$TUR" build "$CF" >"$CF/build-false.log" 2>&1; then
    ns_false_out=$("$CF/build/bin/cf" 2>&1)
fi
if [ "$ns_true_ok" = 1 ] && [ "$ns_false_out" = "7" ]; then
    echo "PASS no-stdlib: :no-stdlib true is refused, false builds"
    PASS=$((PASS + 1))
else
    echo "FAIL no-stdlib: true refused=$ns_true_ok, false out=[$ns_false_out]"
    sed 's/^/    /' "$CF/build.log" | tail -n 5
    FAIL=$((FAIL + 1)); FAILED+=("no-stdlib")
fi
rm -rf "$CF"

# definstance-not-dispatchable-across-modules: a typeclass instance defined in
# one module of a spice must be dispatchable from another.  The method is never
# `is_exported` (a definstance has no export list), so it used to be emitted
# `static` and left out of the module header -- `tur check` was clean and cc
# died with `call to undeclared function '__inst_Dbl_dbl_Box'`.
#
# Needs a MULTI-MODULE build: the single-file path has no boundary to cross,
# which is how this survived.  Asserting on the build alone would be a weak
# test (a future regression could re-static it and still link if the importing
# TU happened to emit its own copy), so the header declaration is checked too.
XMI="$SPICES/cross-module-instance"
rm -rf "$XMI/build"
if "$TUR" build "$XMI" >/dev/null 2>&1 \
   && grep -q '__inst_Dbl_dbl_Box' "$XMI/build/obj/xmi__base.h" \
   && ! grep -q 'static .*__inst_Dbl_dbl_Box' "$XMI/build/obj/xmi__base.c"; then
    echo "PASS cross-module instance: dispatch links, method is declared and external"
    PASS=$((PASS + 1))
else
    echo "FAIL cross-module instance: a definstance in one module is not usable from another"
    FAIL=$((FAIL + 1)); FAILED+=("cross-module instance")
fi
rm -rf "$XMI/build"

echo
echo "summary: $PASS passed, $FAIL failed"
if [ "$FAIL" -ne 0 ]; then
    echo "failed cases:"
    for f in "${FAILED[@]}"; do echo "  - $f"; done
    exit 1
fi
exit 0
