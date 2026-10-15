#!/usr/bin/env bash
# test-emit-cmake.sh -- integration test for `tur emit-cmake` (Phase B)
set -u
cd "$(dirname "$0")/.."

TUR="${TUR:-./build/tur}"
[ -x "$TUR" ] || { echo "tests: $TUR not built; run 'tur run build' first" >&2; exit 2; }

PASS=0
FAIL=0
TMPDIR_BASE="$(mktemp -d /tmp/tur-emit-cmake-test.XXXXXX)"
trap 'rm -rf "$TMPDIR_BASE"' EXIT

pass() { PASS=$((PASS+1)); echo "PASS $1"; }
fail() { FAIL=$((FAIL+1)); echo "FAIL $1: $2"; }

# ---------------------------------------------------------------------------
# Test 1: emit-cmake in a directory with no build.tur exits non-zero
# ---------------------------------------------------------------------------
T="$TMPDIR_BASE/no-manifest"
mkdir -p "$T"
if (cd "$T" && "$OLDPWD/$TUR" emit-cmake) 2>/dev/null; then
    fail "no-manifest" "should have failed but exited 0"
else
    pass "no-manifest"
fi

# ---------------------------------------------------------------------------
# Test 2: emit-cmake produces the expected files for a minimal library
# ---------------------------------------------------------------------------
T="$TMPDIR_BASE/minimal-lib"
mkdir -p "$T/src"

cat > "$T/build.tur" <<'EOF'
(defpackage my-lib
  :version "1.2.3"
  :description "A test library"
  :exports ["src/math.tur"])
EOF

cat > "$T/src/math.tur" <<'EOF'
(defn add [a :int b :int] :int
  (+ a b))
EOF

(cd "$T" && "$OLDPWD/$TUR" emit-cmake) > /dev/null 2>&1
RC=$?

if [ "$RC" -ne 0 ]; then
    fail "minimal-lib-exit" "emit-cmake exited $RC"
else
    pass "minimal-lib-exit"
fi

# CMakeLists.txt should exist
if [ -f "$T/CMakeLists.txt" ]; then
    pass "minimal-lib-cmakelists-exists"
else
    fail "minimal-lib-cmakelists-exists" "CMakeLists.txt not created"
fi

# Config cmake should exist
if [ -f "$T/my-libConfig.cmake" ]; then
    pass "minimal-lib-config-exists"
else
    fail "minimal-lib-config-exists" "my-libConfig.cmake not created"
fi

# FindTurmeric.cmake should exist
if [ -f "$T/cmake/FindTurmeric.cmake" ]; then
    pass "minimal-lib-find-turmeric-exists"
else
    fail "minimal-lib-find-turmeric-exists" "cmake/FindTurmeric.cmake not created"
fi

# AddTurmericTarget.cmake should exist
if [ -f "$T/cmake/AddTurmericTarget.cmake" ]; then
    pass "minimal-lib-add-target-exists"
else
    fail "minimal-lib-add-target-exists" "cmake/AddTurmericTarget.cmake not created"
fi

# CMakeLists.txt should reference the project name
if grep -q "project(my-lib" "$T/CMakeLists.txt"; then
    pass "minimal-lib-project-name"
else
    fail "minimal-lib-project-name" "CMakeLists.txt missing project(my-lib ...)"
fi

# CMakeLists.txt should reference the version
if grep -q "VERSION 1.2.3" "$T/CMakeLists.txt"; then
    pass "minimal-lib-version"
else
    fail "minimal-lib-version" "CMakeLists.txt missing VERSION 1.2.3"
fi

# CMakeLists.txt should reference the source file
if grep -q "src/math.tur" "$T/CMakeLists.txt"; then
    pass "minimal-lib-source-ref"
else
    fail "minimal-lib-source-ref" "CMakeLists.txt does not reference src/math.tur"
fi

# Config cmake should contain version and FOUND vars
if grep -q "my-lib_VERSION" "$T/my-libConfig.cmake" && \
   grep -q "my-lib_FOUND" "$T/my-libConfig.cmake"; then
    pass "minimal-lib-config-vars"
else
    fail "minimal-lib-config-vars" "my-libConfig.cmake missing version/found vars"
fi

# ---------------------------------------------------------------------------
# Test 3: emit-cmake with --output-dir writes to specified directory
# ---------------------------------------------------------------------------
T="$TMPDIR_BASE/output-dir"
SRC="$TMPDIR_BASE/minimal-lib"   # reuse the project from test 2
OUT="$TMPDIR_BASE/out-dir"
mkdir -p "$OUT"

(cd "$SRC" && "$OLDPWD/$TUR" emit-cmake --output-dir "$OUT") > /dev/null 2>&1
RC=$?

if [ "$RC" -ne 0 ]; then
    fail "output-dir-exit" "emit-cmake --output-dir exited $RC"
else
    pass "output-dir-exit"
fi

if [ -f "$OUT/CMakeLists.txt" ]; then
    pass "output-dir-cmakelists"
else
    fail "output-dir-cmakelists" "CMakeLists.txt not written to output-dir"
fi

if [ -f "$OUT/cmake/FindTurmeric.cmake" ]; then
    pass "output-dir-find-turmeric"
else
    fail "output-dir-find-turmeric" "cmake/FindTurmeric.cmake not in output-dir"
fi

# ---------------------------------------------------------------------------
# Test 4: emit-cmake scans src/ when no :exports field is given
# ---------------------------------------------------------------------------
T="$TMPDIR_BASE/no-exports"
mkdir -p "$T/src"

cat > "$T/build.tur" <<'EOF'
(defpackage scan-lib
  :version "0.1.0")
EOF

cat > "$T/src/utils.tur" <<'EOF'
(defn noop [] :void)
EOF

(cd "$T" && "$OLDPWD/$TUR" emit-cmake) > /dev/null 2>&1
RC=$?

if [ "$RC" -ne 0 ]; then
    fail "no-exports-exit" "emit-cmake (no :exports) exited $RC"
else
    pass "no-exports-exit"
fi

if grep -q "src/utils.tur" "$T/CMakeLists.txt" 2>/dev/null; then
    pass "no-exports-scan"
else
    fail "no-exports-scan" "CMakeLists.txt does not include scanned src/utils.tur"
fi

# ---------------------------------------------------------------------------
# Summary
# ---------------------------------------------------------------------------
echo
echo "emit-cmake summary: $PASS passed, $FAIL failed"
[ $FAIL -eq 0 ] && exit 0 || exit 1
