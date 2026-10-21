#!/usr/bin/env bash
# tests/run-transitive-cmake-deps.sh -- regression checks for the
# transitive `:cmake-deps` resolver (docs/archive/history/transitive-cmake-deps-plan.md).
#
# Covers the two cheap-and-deterministic cases the plan asks for:
#   1. transitive-cmake-deps-cycle    -- A→B→A in :spices; resolver must
#                                        terminate via its visited set.
#   2. transitive-cmake-deps-conflict -- two siblings declare the same
#                                        :cmake-deps name with different :refs;
#                                        resolver must emit the "conflicting
#                                        :cmake-deps" diagnostic and exit
#                                        non-zero before invoking cmake.
#
# Plus two cases for the *include-path* walk, which is a separate traversal
# over the same :spices graph and needed its own visited set
# (docs/archive/history/spice-cycle-include-path-blowup.md):
#   4. spice-cycle-three-hop          -- a -> b -> c -> a; terminates, and
#                                        every src/ on the ring still lands on
#                                        the include path.
#   5. spice-diamond-shared-dep       -- one dep reached by two parents; the
#                                        visited set must dedupe the WALK
#                                        without pruning the include dir.
#
# The plan's third case (transitive-cmake-deps-basic, an end-to-end build
# through a sibling's :cmake-deps) is exercised by the cascade fixture in
# `../turmeric-spices/spices/tourist/tests/fixtures/cascade/` -- it is not
# replicated here because a hermetic version would need a vendored cmake
# project and a working `cmake` on the runner.

set -uo pipefail
cd "$(dirname "$0")/.."

TUR_REL="./build/tur"
TUR_ABS="$(cd "$(dirname "$TUR_REL")" && pwd)/$(basename "$TUR_REL")"
WORK="$(mktemp -d -t tur-tcd.XXXXXX)"
trap 'rm -rf "$WORK"' EXIT

PASS=0
FAIL=0
pass() { PASS=$((PASS + 1)); echo "PASS $1"; }
fail() { FAIL=$((FAIL + 1)); echo "FAIL $1 -- $2"; }

if [ ! -x "$TUR_ABS" ]; then
    echo "tests: $TUR_ABS not built; run 'tur run build' first" >&2
    exit 2
fi

# ---------------------------------------------------------------------------
# 1. Cycle terminates and the program builds + runs.
#    Uses an off-tree copy so the fixture's `build/` is hermetic.
CYCLE_SRC="tests/fixtures/transitive-cmake-deps-cycle"
CYCLE_DIR="$WORK/cycle"
cp -r "$CYCLE_SRC" "$CYCLE_DIR"
if timeout 30 "$TUR_ABS" build "$CYCLE_DIR/spice-a" \
        >"$WORK/cycle.log" 2>&1; then
    if [ ! -x "$CYCLE_DIR/spice-a/build/bin/spice-a" ]; then
        fail "transitive-cmake-deps-cycle-builds" \
             "binary missing at $CYCLE_DIR/spice-a/build/bin/spice-a"
    else
        out="$("$CYCLE_DIR/spice-a/build/bin/spice-a" 2>&1)"
        if [ "$out" = "cycle-ok" ]; then
            pass "transitive-cmake-deps-cycle-builds"
        else
            fail "transitive-cmake-deps-cycle-builds" \
                 "unexpected program output: '$out'"
        fi
    fi
else
    fail "transitive-cmake-deps-cycle-builds" \
         "tur build exit=$?: $(cat "$WORK/cycle.log")"
fi

# ---------------------------------------------------------------------------
# 2. Mismatched same-name :cmake-deps across siblings must fail with the
#    conflict diagnostic; cmake is never invoked.
CONF_SRC="tests/fixtures/transitive-cmake-deps-conflict"
CONF_DIR="$WORK/conflict"
cp -r "$CONF_SRC" "$CONF_DIR"
(
    cd "$CONF_DIR" || exit 3
    "$TUR_ABS" fetch >fetch.log 2>&1
)
conf_rc=$?
if [ "$conf_rc" -eq 0 ]; then
    fail "transitive-cmake-deps-conflict-detected" \
         "expected non-zero exit; got rc=$conf_rc, log=$(cat "$CONF_DIR/fetch.log")"
elif ! grep -q 'conflicting :cmake-deps for "fakelib"' "$CONF_DIR/fetch.log"; then
    fail "transitive-cmake-deps-conflict-detected" \
         "missing conflict diagnostic; got: $(cat "$CONF_DIR/fetch.log")"
else
    pass "transitive-cmake-deps-conflict-detected"
fi

# ---------------------------------------------------------------------------
# 3. Workspace members are NOT seeded by default, and the escape hatch still
#    seeds them.
#
#    member-b does not list member-a in :spices, reaching it only through the
#    workspace :members resolution.  Both members declare the same-named
#    "fakelib" with different :refs, so seeding member-a makes the conflict
#    diagnostic fire -- which makes this fixture a precise probe for whether
#    seeding happened at all.
#
#    workspace-transitive-native-deps-plan originally seeded every member, so
#    the diagnostic was the assertion.  That was reverted: seeding all members
#    means building ONE spice configures EVERY member's native deps, which is
#    wasteful and fatal two ways -- one dep that cannot configure aborts the
#    whole build, and unrelated members collide in the single CMake target
#    namespace.  See pkg_workspace_wide_cmake_deps().  The declared :spices
#    closure (case 2 above) is the supported way to inherit a sibling's native
#    dep.  Both directions are pinned here so neither default drifts silently.
WMD_SRC="tests/fixtures/workspace-member-cmake-deps"
WMD_DIR="$WORK/wmd"
cp -r "$WMD_SRC" "$WMD_DIR"
(
    cd "$WMD_DIR/member-b" || exit 3
    "$TUR_ABS" fetch >fetch.log 2>&1
)
if grep -q 'conflicting :cmake-deps for "fakelib"' "$WMD_DIR/member-b/fetch.log"; then
    fail "workspace-member-cmake-deps-not-seeded-by-default" \
         "member-a's :cmake-deps was seeded from :members; expected only member-b's own closure. log=$(cat "$WMD_DIR/member-b/fetch.log")"
else
    pass "workspace-member-cmake-deps-not-seeded-by-default"
fi

rm -rf "$WMD_DIR"
cp -r "$WMD_SRC" "$WMD_DIR"
(
    cd "$WMD_DIR/member-b" || exit 3
    TUR_CMAKE_DEPS_WORKSPACE_WIDE=1 "$TUR_ABS" fetch >fetch.log 2>&1
)
if grep -q 'conflicting :cmake-deps for "fakelib"' "$WMD_DIR/member-b/fetch.log"; then
    pass "workspace-member-cmake-deps-seed-escape-hatch"
else
    fail "workspace-member-cmake-deps-seed-escape-hatch" \
         "TUR_CMAKE_DEPS_WORKSPACE_WIDE=1 did not seed sibling :cmake-deps; got: $(cat "$WMD_DIR/member-b/fetch.log")"
fi

# ---------------------------------------------------------------------------
# 4/5. spice-cycle-include-path-blowup: the transitive :spices walk that unions
#      dep src/ dirs into the include path needs the same visited set the
#      :cmake-deps walk above already has. Without one, a manifest cycle
#      recursed forever -- each lap resolved through one more `../` hop, so the
#      paths never repeated textually and the output dedup never fired. It
#      surfaced as a multi-kilobyte `-I` that cc rejected with "File name too
#      long" on an unrelated system header, or as a stack overflow under ASan.
#
#      Case 1 above covers the two-spice ring. These two cover the ways a
#      too-clever fix goes wrong: a three-hop ring (which a "is this dep my
#      parent?" check would miss) and a diamond (which an over-eager visited
#      set would prune, dropping an include dir that is genuinely needed).
run_spice_walk_case() {  # <fixture> <member> <binary> <expected-output>
    local fixture="$1" member="$2" bin="$3" want="$4"
    local name="${fixture}-builds"
    local src="tests/fixtures/$fixture"
    local dir="$WORK/$fixture"
    cp -r "$src" "$dir"
    if timeout 30 "$TUR_ABS" build "$dir/$member" >"$WORK/$fixture.log" 2>&1; then
        if [ ! -x "$dir/$member/build/bin/$bin" ]; then
            fail "$name" "binary missing at $dir/$member/build/bin/$bin"
        else
            local out
            out="$("$dir/$member/build/bin/$bin" 2>&1)"
            if [ "$out" = "$want" ]; then
                pass "$name"
            else
                fail "$name" "expected '$want', got '$out'"
            fi
        fi
    else
        fail "$name" "tur build exit=$?: $(cat "$WORK/$fixture.log")"
    fi
}

run_spice_walk_case spice-cycle-three-hop     a   a   "three-hop-ok"
run_spice_walk_case spice-diamond-shared-dep  app app "diamond-ok"

# 6. tur-run-cmake-marker-skips-after-failed-configure: `tur run` took the
#    generated cmake/CMakeLists.txt -- written before the configure that can
#    fail -- as "deps built", so after one failure every later run skipped the
#    dep build and compiled with no dep flags.  A dep whose configure always
#    fails must fail the SECOND run the same way as the first.
if command -v cmake >/dev/null 2>&1; then
    FC="$WORK/failcfg"
    mkdir -p "$FC/src" "$FC/dep"
    cat >"$FC/build.tur" <<'EOF'
(defpackage failcfg
  :name "failcfg"
  :version "0.1.0"
  :cmake-deps #map{"dep" #map{:path "../dep" :targets ["dep"]}})
EOF
    printf 'cmake_minimum_required(VERSION 3.10)\nproject(dep C)\nmessage(FATAL_ERROR "failcfg-configure-boom")\n' \
        >"$FC/dep/CMakeLists.txt"
    printf '(defn main [] : int 0)\n' >"$FC/src/main.tur"
    for i in 1 2; do
        ( cd "$FC" && timeout 120 "$TUR_ABS" run ) >"$WORK/failcfg$i.log" 2>&1
    done
    if grep -q "failcfg-configure-boom" "$WORK/failcfg1.log" &&
       grep -q "failcfg-configure-boom" "$WORK/failcfg2.log"; then
        pass "run-retries-failed-cmake-dep-build"
    else
        fail "run-retries-failed-cmake-dep-build" \
             "second run did not retry the dep configure: $(tail -n 5 "$WORK/failcfg2.log")"
    fi
else
    echo "SKIP run-retries-failed-cmake-dep-build (no cmake)"
fi

# ---------------------------------------------------------------------------
# 6. cmake-dep-link-walk-stops-at-archive: `top` links `mid`, which links
#    `core`.  The generated link line must put libcore.a after libmid.a
#    after libtop.a, or GNU ld (which never revisits an archive) reports
#    core_value undefined.  The program prints 42 when the order is right.
if command -v cmake >/dev/null 2>&1; then
    CHAIN_SRC="tests/fixtures/cmake-deps-archive-chain"
    CHAIN_DIR="$WORK/chain"
    cp -r "$CHAIN_SRC" "$CHAIN_DIR"
    if (cd "$CHAIN_DIR/app" && timeout 120 "$TUR_ABS" fetch >fetch.log 2>&1 \
            && timeout 120 "$TUR_ABS" build . >build.log 2>&1) \
       && [ -x "$CHAIN_DIR/app/build/bin/app" ] \
       && [ "$("$CHAIN_DIR/app/build/bin/app" 2>&1)" = "42" ]; then
        pass "cmake-deps-archive-chain-links"
    else
        fail "cmake-deps-archive-chain-links" \
             "expected the app to build and print 42: $(tail -n 8 "$CHAIN_DIR/app/fetch.log" "$CHAIN_DIR/app/build.log" 2>/dev/null)"
    fi
else
    echo "SKIP cmake-deps-archive-chain-links (no cmake)"
fi

# ---------------------------------------------------------------------------
# 7./8. cmake-deps-duplicate-keeps-first-options: two spices declare one
#    :cmake-deps name at the same :url/:ref.  Their :options UNION (a key only
#    one of them sets still configures the dep), and a key set to two
#    different values is a hard error naming both manifests.  The dep is a
#    local git repo so FetchContent needs no network.
if command -v cmake >/dev/null 2>&1 && command -v git >/dev/null 2>&1; then
    U="$WORK/union"
    mkdir -p "$U/dep" "$U/spice-a" "$U/spice-b" "$U/spice-c" "$U/app/src" "$U/app2/src"
    cat >"$U/dep/CMakeLists.txt" <<'CMK'
cmake_minimum_required(VERSION 3.20)
project(dep C)
set(DEP_FLAVOR "default" CACHE STRING "which flavor of dep to build")
set(DEP_EXTRA "none" CACHE STRING "an extra knob")
add_library(dep STATIC dep.c)
target_compile_definitions(dep PRIVATE "DEP_FLAVOR=\"${DEP_FLAVOR}\"" "DEP_EXTRA=\"${DEP_EXTRA}\"")
CMK
    printf 'const char *dep_flavor(void) { return DEP_FLAVOR "/" DEP_EXTRA; }\n' >"$U/dep/dep.c"
    (cd "$U/dep" && git init -q && git add -A \
        && git -c user.name=r -c user.email=r@example.com commit -qm dep && git tag v1)
    for s in a b c; do
        case $s in
            a) opts=':DEP_FLAVOR "shared"' ;;
            b) opts=':DEP_FLAVOR "shared" :DEP_EXTRA "from-b"' ;;
            c) opts=':DEP_FLAVOR "other"' ;;
        esac
        cat >"$U/spice-$s/build.tur" <<MAN
(defpackage spice-$s :name "spice-$s" :version "0.1.0"
  :cmake-deps #map{"dep" #map{:url "file://$U/dep" :ref "v1" :targets ["dep"]
                              :options #map{$opts}}})
MAN
    done
    cat >"$U/app/src/main.tur" <<'TUR'
(defn flavor [] : cstr
  ```c
  extern const char *dep_flavor(void);
  return dep_flavor();
  ```)
(defn main [] : int (println (flavor)) 0)
TUR
    cp "$U/app/src/main.tur" "$U/app2/src/main.tur"
    cat >"$U/app/build.tur" <<'MAN'
(defpackage app :name "app" :version "0.1.0"
  :spices #map{"spice-a" #map{:path "../spice-a"} "spice-b" #map{:path "../spice-b"}})
MAN
    cat >"$U/app2/build.tur" <<'MAN'
(defpackage app2 :name "app2" :version "0.1.0"
  :spices #map{"spice-a" #map{:path "../spice-a"} "spice-c" #map{:path "../spice-c"}})
MAN
    if (cd "$U/app" && timeout 120 "$TUR_ABS" fetch >fetch.log 2>&1) \
       && grep -q 'set(DEP_FLAVOR "shared"' "$U/app/cmake/CMakeLists.txt" \
       && grep -q 'set(DEP_EXTRA "from-b"' "$U/app/cmake/CMakeLists.txt" \
       && (cd "$U/app" && timeout 120 "$TUR_ABS" build . >build.log 2>&1) \
       && [ "$("$U/app/build/bin/app" 2>&1)" = "shared/from-b" ]; then
        pass "cmake-deps-option-union"
    else
        fail "cmake-deps-option-union" \
             "expected both keys in cmake/CMakeLists.txt and the program to print shared/from-b: $(tail -n 8 "$U/app/fetch.log" "$U/app/build.log" 2>/dev/null)"
    fi
    (cd "$U/app2" && "$TUR_ABS" fetch >fetch.log 2>&1)
    rc=$?
    if [ "$rc" -ne 0 ] \
       && grep -q 'conflicting :cmake-deps option for "dep": DEP_FLAVOR' "$U/app2/fetch.log" \
       && grep -q 'spice-a' "$U/app2/fetch.log" && grep -q 'spice-c' "$U/app2/fetch.log"; then
        pass "cmake-deps-option-conflict"
    else
        fail "cmake-deps-option-conflict" \
             "expected a non-zero exit naming DEP_FLAVOR and both spices; rc=$rc, log=$(cat "$U/app2/fetch.log")"
    fi
else
    echo "SKIP cmake-deps-option-union / cmake-deps-option-conflict (no cmake or git)"
fi

echo
echo "transitive-cmake-deps: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
