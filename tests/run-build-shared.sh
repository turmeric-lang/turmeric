#!/usr/bin/env bash
# tests/run-build-shared.sh -- RP0+RP1 smoke test for `tur build --shared`.
#
# Builds tests/fixtures/build-shared-smoke as a shared library, then
# compiles a hand-written C harness that dlopens it and calls the
# exported `smokelib__add42` symbol. Confirms the end-to-end pipe
# from `defmodule ... (export ...)` -> linker -> dlopen -> dlsym -> call.
#
# RP1 additions: assert that exports.manifest is produced with the right
# format and that --manifest <path> redirects the output.

set -uo pipefail
cd "$(dirname "$0")/.."

TUR="${TUR:-./build/tur}"
FIXTURE="tests/fixtures/build-shared-smoke"
WORK="$(mktemp -d -t tur-rp0.XXXXXX)"
trap 'rm -rf "$WORK"' EXIT

PASS=0
FAIL=0
pass() { PASS=$((PASS + 1)); echo "PASS $1"; }
fail() { FAIL=$((FAIL + 1)); echo "FAIL $1 -- $2"; }

if [ ! -x "$TUR" ]; then
    echo "tests: $TUR not built; run 'tur run build' first" >&2
    exit 2
fi

# Pick the shared-library extension. macOS dlopen accepts .so too, but use
# the platform-conventional name so the artifact looks right under `file`.
case "$(uname -s)" in
    Darwin)              LIB_EXT="dylib" ;;
    MINGW*|MSYS*|CYGWIN*) LIB_EXT="dll" ;;
    *)                   LIB_EXT="so" ;;
esac
LIB="$WORK/libsmoke.$LIB_EXT"

# The name `tur build --shared` picks when the caller gives no -o. This must
# track TUR_SHLIB_PREFIX/TUR_SHLIB_EXT in src/platform_fs.h: PE has no `lib`
# convention, so Windows gets a bare `<name>.dll`.
case "$(uname -s)" in
    MINGW*|MSYS*|CYGWIN*) DEF_PREFIX="";    DEF_EXT=".dll" ;;
    *)                    DEF_PREFIX="lib"; DEF_EXT=".so"  ;;
esac

# Build the shared library.
build_out=$("$TUR" build --shared "$FIXTURE" -o "$LIB" 2>&1)
build_rc=$?
if [ $build_rc -ne 0 ]; then
    fail "build-shared-smoke-link" "tur build --shared exit=$build_rc: $build_out"
    exit 1
fi
if [ ! -f "$LIB" ]; then
    fail "build-shared-smoke-link" "expected $LIB to exist"
    exit 1
fi
pass "build-shared-smoke-link"

# The .so must export smokelib__add42 (the mangled name for the (export
# add42) defn inside (defmodule smokelib ...)). nm is preferred; fall
# back to strings if nm is unavailable.
#
# Capture nm's output before matching rather than piping into `grep -q`: this
# script runs under `set -o pipefail`, and `grep -q` exits on the first match,
# which hands nm a SIGPIPE (141) and makes the *pipeline* fail even though the
# symbol was found.  Whether that races depends on how much nm still had
# buffered, so the piped form failed intermittently (~4 runs in 5 here) with a
# misleading "symbol not found".
if command -v nm >/dev/null 2>&1; then
    nm_out=$(nm "$LIB" 2>/dev/null)
    if grep -qE '(^| )_?smokelib__add42( |$)' <<< "$nm_out"; then
        pass "build-shared-smoke-symbol-present"
    else
        fail "build-shared-smoke-symbol-present" "smokelib__add42 not found in $LIB"
    fi
fi

# Compile and run the harness.
HARNESS="$WORK/harness"
CC_BIN="${CC:-cc}"
# -ldl: required on Linux for dlopen/dlsym; macOS rolls them into libSystem
# so the flag is harmless there. Windows has no libdl at all -- the harness
# includes src/platform_dl.h instead, so it needs -Isrc and no -l.
case "$(uname -s)" in
    Darwin)               DL_LIBS="" ;;
    MINGW*|MSYS*|CYGWIN*) DL_LIBS="" ;;
    *)                    DL_LIBS="-ldl" ;;
esac
if ! $CC_BIN -O0 -std=c99 -Wall -Isrc -o "$HARNESS" "$FIXTURE/harness.c" $DL_LIBS 2>"$WORK/cc.err"; then
    fail "build-shared-smoke-harness-compile" "cc failed: $(cat "$WORK/cc.err")"
    exit 1
fi
pass "build-shared-smoke-harness-compile"

if out=$("$HARNESS" "$LIB" 2>&1); then
    if grep -q 'smokelib__add42(100) = 142' <<< "$out"; then
        pass "build-shared-smoke-dlopen-call"
    else
        fail "build-shared-smoke-dlopen-call" "unexpected output: $out"
    fi
else
    fail "build-shared-smoke-dlopen-call" "harness exit=$?, output: $out"
fi

# RP1: exports.manifest is produced at the default location (<out>.manifest)
# and records the export with the expected mangled symbol and signature.
DEFAULT_MANIFEST="$LIB.manifest"
if [ -f "$DEFAULT_MANIFEST" ]; then
    pass "build-shared-smoke-manifest-default-exists"
else
    fail "build-shared-smoke-manifest-default-exists" "expected $DEFAULT_MANIFEST"
fi

EXPECTED='smokelib/add42 -> smokelib__add42 :: (:int) -> :int'
if grep -qxF "$EXPECTED" "$DEFAULT_MANIFEST" 2>/dev/null; then
    pass "build-shared-smoke-manifest-line"
else
    fail "build-shared-smoke-manifest-line" \
         "expected line not found. got: $(cat "$DEFAULT_MANIFEST" 2>/dev/null)"
fi

# --manifest <path> redirects manifest output and does NOT write the default.
LIB2="$WORK/libsmoke-alt.$LIB_EXT"
ALT_MANIFEST="$WORK/exports.manifest"
"$TUR" build --shared "$FIXTURE" -o "$LIB2" --manifest "$ALT_MANIFEST" \
       >"$WORK/build2.log" 2>&1
if [ ! -f "$ALT_MANIFEST" ]; then
    fail "build-shared-smoke-manifest-override" \
         "--manifest did not produce $ALT_MANIFEST: $(cat "$WORK/build2.log")"
elif [ -f "$LIB2.manifest" ]; then
    fail "build-shared-smoke-manifest-override" \
         "default $LIB2.manifest should not exist when --manifest is given"
elif ! grep -qxF "$EXPECTED" "$ALT_MANIFEST"; then
    fail "build-shared-smoke-manifest-override" \
         "expected line missing from $ALT_MANIFEST: $(cat "$ALT_MANIFEST")"
else
    pass "build-shared-smoke-manifest-override"
fi

# Default output name for a cwd-relative target ("." / "./") must derive the
# basename from the resolved directory, NOT produce "lib..so" / "lib.so".
# Regression guard for docs/archive/history/tur-build-shared-cwd-lib-double-dot.md.
# build-output-directory-plan: artifacts now land in <root>/build/lib/, so the
# assertion checks that path instead of the cwd.  We copy the module into a
# freshly named subdir so the expected artifact name is predictable, then run
# `tur build --shared .` from inside it (no -o).
NAMED_DIR="$WORK/cwdlib"
mkdir -p "$NAMED_DIR"
cp "$FIXTURE/add.tur" "$NAMED_DIR/add.tur"
TUR_ABS="$(cd "$(dirname "$TUR")" && pwd)/$(basename "$TUR")"
(
    cd "$NAMED_DIR" || exit 3
    "$TUR_ABS" build --shared . >build-cwd.log 2>&1
)
cwd_rc=$?
if [ "$cwd_rc" -ne 0 ]; then
    fail "build-shared-smoke-cwd-default-name" \
         "tur build --shared . exit=$cwd_rc: $(cat "$NAMED_DIR/build-cwd.log" 2>/dev/null)"
elif [ -e "$NAMED_DIR/build/lib/$DEF_PREFIX.$DEF_EXT" ] || \
     [ -e "$NAMED_DIR/build/lib/$DEF_PREFIX.$DEF_EXT.manifest" ]; then
    fail "build-shared-smoke-cwd-default-name" \
         "produced $DEF_PREFIX.$DEF_EXT artifact(s): $(ls "$NAMED_DIR"/build/lib/ 2>/dev/null)"
elif [ -e "$NAMED_DIR/build/lib/$DEF_PREFIX$DEF_EXT" ]; then
    fail "build-shared-smoke-cwd-default-name" \
         "produced nameless $DEF_PREFIX$DEF_EXT: $(ls "$NAMED_DIR"/build/lib/ 2>/dev/null)"
elif [ ! -e "$NAMED_DIR/build/lib/${DEF_PREFIX}cwdlib$DEF_EXT" ]; then
    fail "build-shared-smoke-cwd-default-name" \
         "expected build/lib/${DEF_PREFIX}cwdlib$DEF_EXT (cwd basename); got: $(ls "$NAMED_DIR"/build/lib/ 2>/dev/null)"
else
    pass "build-shared-smoke-cwd-default-name"
fi

# --manifest without --shared must be rejected.
if "$TUR" build "$FIXTURE" --manifest /tmp/should-not-exist.manifest \
        >"$WORK/build3.log" 2>&1; then
    fail "build-shared-smoke-manifest-requires-shared" \
         "expected non-zero exit; output: $(cat "$WORK/build3.log")"
elif ! grep -q -- '--manifest requires --shared' "$WORK/build3.log"; then
    fail "build-shared-smoke-manifest-requires-shared" \
         "missing expected diagnostic; got: $(cat "$WORK/build3.log")"
else
    pass "build-shared-smoke-manifest-requires-shared"
fi

# build-output-directory-plan: assert intermediate .c/.h land under
# <build_dir>/obj/ and not in the source tree.  Uses an explicit
# --build-dir <tmp> so the test is hermetic and never touches the fixture's
# own `build/` (if a previous local run left one behind).
BDR_FIXTURE="tests/fixtures/build-dir-relocates-artifacts"
BDR_BUILD="$WORK/bdr"
"$TUR" build --shared "$BDR_FIXTURE" --build-dir "$BDR_BUILD" \
    >"$WORK/bdr.log" 2>&1
bdr_rc=$?
if [ "$bdr_rc" -ne 0 ]; then
    fail "build-dir-relocates-artifacts" \
         "tur build exit=$bdr_rc: $(cat "$WORK/bdr.log")"
elif [ ! -f "$BDR_BUILD/obj/m.c" ] || [ ! -f "$BDR_BUILD/obj/m.h" ]; then
    fail "build-dir-relocates-artifacts" \
         "expected obj/m.{c,h} under $BDR_BUILD; got: $(ls -R "$BDR_BUILD" 2>/dev/null)"
elif [ ! -f "$BDR_BUILD/lib/${DEF_PREFIX}bdrelocates$DEF_EXT" ]; then
    fail "build-dir-relocates-artifacts" \
         "expected lib/${DEF_PREFIX}bdrelocates$DEF_EXT under $BDR_BUILD; got: $(ls "$BDR_BUILD/lib" 2>/dev/null)"
elif [ -f "$BDR_FIXTURE/src/m.c" ] || [ -f "$BDR_FIXTURE/src/m.h" ]; then
    fail "build-dir-relocates-artifacts" \
         "intermediates leaked into source tree: $(ls "$BDR_FIXTURE/src" 2>/dev/null)"
elif [ ! -f "$BDR_BUILD/.gitignore" ]; then
    fail "build-dir-relocates-artifacts" \
         "expected auto-.gitignore under $BDR_BUILD"
else
    pass "build-dir-relocates-artifacts"
fi

# build-output-directory-plan: TUR_BUILD_DIR env var override (no flag).
BDR_ENV="$WORK/bdr-env"
TUR_BUILD_DIR="$BDR_ENV" "$TUR" build --shared "$BDR_FIXTURE" \
    >"$WORK/bdr-env.log" 2>&1
bdr_env_rc=$?
if [ "$bdr_env_rc" -ne 0 ]; then
    fail "build-dir-env-override" \
         "tur build exit=$bdr_env_rc: $(cat "$WORK/bdr-env.log")"
elif [ ! -f "$BDR_ENV/obj/m.c" ]; then
    fail "build-dir-env-override" \
         "TUR_BUILD_DIR not honored; got: $(ls -R "$BDR_ENV" 2>/dev/null)"
else
    pass "build-dir-env-override"
fi

echo
echo "build-shared: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
