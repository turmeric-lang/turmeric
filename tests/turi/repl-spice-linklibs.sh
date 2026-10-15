#!/usr/bin/env bash
# tests/turi/repl-spice-linklibs.sh -- ffi-spices-integration-plan S1.
#
# A spice that declares its C dependency the recommended way -- via the
# cmake deps manifest's link_dirs/link_libs (no __tur_autolink__ marker
# anywhere in its sources) -- must be loadable and callable at the REPL:
#
#   - non-JIT builds: the subprocess `tur build --shared` path, where
#     collect_build_aux injects -L/-l into cc's link line;
#   - JIT builds: the in-process hook (repl_jit_build), which now threads
#     the same flags into the engine's autolink string so
#     jit_load_autolink can dlopen the library (including from -L dirs,
#     which it used to ignore) before MIR_link resolves.
#
# The dependency is a .so built HERE, in the test's temp dir, so the test
# is hermetic -- no system -dev package, no turmeric-spices checkout.

set -uo pipefail
cd "$(dirname "$0")/../.."

TUR="${TUR:-./build/tur}"
[ -x "$TUR" ] || { echo "tests: $TUR not built; run 'tur run build' first" >&2; exit 2; }
export TUR_BIN
case "$TUR" in
    /*) TUR_BIN="$TUR" ;;
    *)  TUR_BIN="$(cd "$(dirname "$TUR")" && pwd)/$(basename "$TUR")" ;;
esac

CC="${CC:-cc}"
command -v "$CC" >/dev/null 2>&1 || { echo "SKIP: no C compiler"; exit 0; }

WORK="$(mktemp -d -t tur-s1-linklibs.XXXXXX)"
trap 'rm -rf "$WORK"' EXIT
# The REPL's spice-binding installer allocates one FfiBindingUd plus one
# qualified-name key per export and deliberately never frees either --
# src/turi/ffi_thunk.c:533 ("Leaked at process exit") and :551 ("qkey is
# referenced by the env binding's name field; do not free here").  Those are
# process-lifetime by design, like the rest of the tree-walking interpreter,
# so LeakSanitizer's report is noise here and pollutes the output these
# assertions diff against.  Same opt-out repl-spice-jit.sh already carries;
# the compiler/codegen path stays leak-checked (see CLAUDE.md).
export ASAN_OPTIONS="${ASAN_OPTIONS:+$ASAN_OPTIONS:}detect_leaks=0"

# --- the C dependency ----------------------------------------------------
mkdir -p "$WORK/deps"
cat > "$WORK/deps/shim.c" <<'EOF'
long long tur_test_shim_add(long long a, long long b) { return a + b + 90; }
EOF
"$CC" -shared -fPIC -o "$WORK/deps/libtstshim.so" "$WORK/deps/shim.c" \
    || { echo "SKIP: cannot build test .so"; exit 0; }

# --- the spice -----------------------------------------------------------
PROJ="$WORK/proj"
mkdir -p "$PROJ/src" "$PROJ/cmake"
cat > "$PROJ/build.tur" <<'EOF'
(defpackage s1-linklibs-fixture)
EOF
# The shape `tur build` records for a resolved :cmake-deps entry.  No
# __tur_autolink__ marker exists anywhere in this spice -- the manifest is
# the only thing naming the library, which is exactly the point.
cat > "$PROJ/cmake/spice-deps-manifest.json" <<EOF
{"tstshim": {"resolved_via": "test", "include_dirs": [],
             "link_dirs": ["$WORK/deps"], "link_libs": ["tstshim"]}}
EOF
cat > "$PROJ/src/lib.tur" <<'EOF'
(defmodule s1shim
  (export shim-answer)
  ;; Calls into libtstshim.so; the block-scope extern is the only
  ;; declaration -- the definition lives in the manifest-declared library.
  (defn shim-answer [a :int b :int] :int
    ```c
    extern long long tur_test_shim_add(long long, long long);
    return tur_test_shim_add(a, b);
    ```))
EOF

PASS=0
FAIL=0
pass() { PASS=$((PASS + 1)); echo "PASS $1"; }
fail() { FAIL=$((FAIL + 1)); echo "FAIL $1 -- $2"; }

# JIT probe (command substitution, not a pipeline -- set -o pipefail).
HAS_JIT=1
probe_out=$("$TUR_BIN" jit /dev/null 2>&1 || true)
case "$probe_out" in *"no JIT engine"*) HAS_JIT=0 ;; esac

# The two paths prove different halves, so the environment differs:
#  - JIT hook (TUR_ENGINE=jit selects it; graduation kept the subprocess
#    path the default): run WITHOUT LD_LIBRARY_PATH.  The engine must find
#    the library through the manifest's -L dir (jit_load_autolink's new
#    dir probing); a bare-soname fallback lookup cannot succeed.
#  - subprocess .so: cc links with -L/-l, but the resulting DT_NEEDED is
#    resolved by the system loader at dlopen time, which needs
#    LD_LIBRARY_PATH for a library outside the default search path --
#    standard ELF behavior (a real :cmake-deps dep is typically built
#    static for exactly this reason).
run() {
    local prog="$1"
    if [ "$HAS_JIT" = "1" ]; then
        (cd "$PROJ" && printf '%s\n:quit\n' "$prog" \
             | TUR_ENGINE=jit "$TUR_BIN" repl 2>&1)
    else
        (cd "$PROJ" && printf '%s\n:quit\n' "$prog" \
             | LD_LIBRARY_PATH="$WORK/deps" "$TUR_BIN" repl 2>&1)
    fi
}

out=$(run '(shim-answer 4 5)')
if grep -qx '=> 99' <<< "$out"; then
    pass "s1-linklibs-call"
else
    fail "s1-linklibs-call" "expected => 99 calling through the manifest-declared library; got: $(echo "$out" | tail -5 | tr '\n' ' ')"
fi

echo
echo "repl-spice-linklibs summary: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
