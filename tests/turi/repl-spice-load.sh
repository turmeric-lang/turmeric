#!/usr/bin/env bash
# tests/turi/repl-spice-load.sh -- RP3 integration test.
#
# Scenarios:
#   1. REPL outside any project: silent (no "Loaded spice" line, no cache).
#   2. REPL inside a project: discovers build.tur, builds .tur-repl-cache/lib.so,
#      writes exports.manifest, prints "Loaded spice from ... (N export...)".
#   3. Rerun is a no-op: lib.so's mtime is unchanged.
#   4. Touching a source rebuilds: lib.so's mtime advances.
#   5. TUR_NO_AUTO_SPICE=1 opts out: no Loaded line, no cache dir created.
#   6. Auto-injected `.gitignore` entry: when a .gitignore exists, RP3 appends
#      `.tur-repl-cache/` exactly once.
#
# RP4 will exercise the symbol map via actual (import M) forms. This test
# pre-emptively asserts the data the loader put on the env (via the
# startup status line + manifest file inspection).

set -uo pipefail
cd "$(dirname "$0")/../.."

TUR="${TUR:-./build/tur}"
[ -x "$TUR" ] || { echo "tests: $TUR not built; run 'tur run build' first" >&2; exit 2; }
# The REPL invokes `tur` as a subprocess; point at the local build.  TUR_BIN
# must be absolute so the subprocess works regardless of where the loader is
# invoked from -- but prefixing $PWD unconditionally corrupts an already
# absolute $TUR into "$PWD/home/.../tur", so branch on which one it is.
case "$TUR" in
    /*) TUR_BIN="$TUR" ;;
    *)  TUR_BIN="$(cd "$(dirname "$PWD/$TUR")" && pwd)/$(basename "$TUR")" ;;
esac
export TUR_BIN

WORK="$(mktemp -d -t tur-rp3.XXXXXX)"
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

# The cached spice image is named by tur_spice_image_load(); its extension
# tracks TUR_SHLIB_EXT in src/platform_fs.h, so it is .dll on Windows.
case "$(uname -s)" in
    MINGW*|MSYS*|CYGWIN*) CACHE_LIB="lib-0.dll" ;;
    *)                    CACHE_LIB="lib-0.so"  ;;
esac

PASS=0
FAIL=0
pass() { PASS=$((PASS + 1)); echo "PASS $1"; }
fail() { FAIL=$((FAIL + 1)); echo "FAIL $1 -- $2"; }

# --- scenario 1: outside any project --------------------------------------
NOPROJ="$WORK/no-project"
mkdir -p "$NOPROJ"
out=$(cd "$NOPROJ" && echo ':quit' | "$TUR_BIN" repl 2>&1)
if grep -q 'Loaded spice' <<< "$out"; then
    fail "repl-spice-load-outside-project" \
         "unexpected 'Loaded spice' line: $out"
else
    pass "repl-spice-load-outside-project"
fi

# --- scenario 2: inside a project (initial build) -------------------------
PROJ="$WORK/proj"
mkdir -p "$PROJ/src"
cat > "$PROJ/build.tur" <<'EOF'
(defpackage rp3-fixture)
EOF
cat > "$PROJ/src/smoke.tur" <<'EOF'
(defmodule smokelib
  (export add42)
  (defn add42 [x :int] :int (+ x 42)))
EOF
out=$(cd "$PROJ" && echo ':quit' | "$TUR_BIN" repl 2>&1)
if grep -q 'Loaded spice from .*proj (1 export)' <<< "$out"; then
    pass "repl-spice-load-initial"
else
    fail "repl-spice-load-initial" "missing/wrong status line: $out"
fi

if [ -f "$PROJ/.tur-repl-cache/$CACHE_LIB" ] \
    && [ -f "$PROJ/.tur-repl-cache/exports.manifest" ]; then
    pass "repl-spice-load-cache-populated"
else
    fail "repl-spice-load-cache-populated" \
         "$CACHE_LIB or exports.manifest missing under .tur-repl-cache"
fi

EXPECTED='smokelib/add42 -> smokelib__add42 :: (:int) -> :int'
if grep -qxF "$EXPECTED" "$PROJ/.tur-repl-cache/exports.manifest"; then
    pass "repl-spice-load-manifest-line"
else
    fail "repl-spice-load-manifest-line" \
         "manifest content: $(cat "$PROJ/.tur-repl-cache/exports.manifest")"
fi

# --- scenario 3: rerun is a no-op (mtime unchanged) -----------------------
MT_BEFORE=$(stat -c %Y "$PROJ/.tur-repl-cache/$CACHE_LIB" 2>/dev/null \
            || stat -f %m "$PROJ/.tur-repl-cache/$CACHE_LIB" 2>/dev/null)
sleep 1
(cd "$PROJ" && echo ':quit' | "$TUR_BIN" repl >/dev/null 2>&1)
MT_AFTER=$(stat -c %Y "$PROJ/.tur-repl-cache/$CACHE_LIB" 2>/dev/null \
           || stat -f %m "$PROJ/.tur-repl-cache/$CACHE_LIB" 2>/dev/null)
if [ "$MT_BEFORE" = "$MT_AFTER" ]; then
    pass "repl-spice-load-rerun-skips-rebuild"
else
    fail "repl-spice-load-rerun-skips-rebuild" \
         "$CACHE_LIB was rebuilt unnecessarily ($MT_BEFORE -> $MT_AFTER)"
fi

# --- scenario 4: touching a source rebuilds -------------------------------
touch "$PROJ/src/smoke.tur"
(cd "$PROJ" && echo ':quit' | "$TUR_BIN" repl >/dev/null 2>&1)
MT_REBUILT=$(stat -c %Y "$PROJ/.tur-repl-cache/$CACHE_LIB" 2>/dev/null \
             || stat -f %m "$PROJ/.tur-repl-cache/$CACHE_LIB" 2>/dev/null)
if [ "$MT_REBUILT" != "$MT_AFTER" ]; then
    pass "repl-spice-load-rebuild-on-change"
else
    fail "repl-spice-load-rebuild-on-change" \
         "$CACHE_LIB mtime did not change after touching source"
fi

# --- scenario 5: TUR_NO_AUTO_SPICE=1 opts out -----------------------------
NOAUTOPROJ="$WORK/noauto"
mkdir -p "$NOAUTOPROJ/src"
cat > "$NOAUTOPROJ/build.tur" <<'EOF'
(defpackage rp3-noauto)
EOF
cat > "$NOAUTOPROJ/src/x.tur" <<'EOF'
(defmodule noauto
  (export id)
  (defn id [x :int] :int x))
EOF
out=$(cd "$NOAUTOPROJ" && echo ':quit' \
        | TUR_NO_AUTO_SPICE=1 "$TUR_BIN" repl 2>&1)
if grep -q 'Loaded spice' <<< "$out"; then
    fail "repl-spice-load-no-auto-spice" \
         "TUR_NO_AUTO_SPICE=1 was ignored: $out"
elif [ -d "$NOAUTOPROJ/.tur-repl-cache" ]; then
    fail "repl-spice-load-no-auto-spice" \
         "cache dir created despite opt-out"
else
    pass "repl-spice-load-no-auto-spice"
fi

# --- scenario 6: .gitignore handshake -------------------------------------
GIPROJ="$WORK/gitproj"
mkdir -p "$GIPROJ/src"
cat > "$GIPROJ/build.tur" <<'EOF'
(defpackage rp3-gi)
EOF
cat > "$GIPROJ/src/gi.tur" <<'EOF'
(defmodule gi (export one) (defn one [] :int 1))
EOF
cat > "$GIPROJ/.gitignore" <<'EOF'
*.o
EOF
(cd "$GIPROJ" && echo ':quit' | "$TUR_BIN" repl >/dev/null 2>&1)
if grep -q '^\.tur-repl-cache/$' "$GIPROJ/.gitignore"; then
    pass "repl-spice-load-gitignore-appended"
else
    fail "repl-spice-load-gitignore-appended" \
         ".gitignore content: $(cat "$GIPROJ/.gitignore")"
fi
# Second run must NOT duplicate the entry.
(cd "$GIPROJ" && echo ':quit' | "$TUR_BIN" repl >/dev/null 2>&1)
COUNT=$(grep -c '^\.tur-repl-cache/$' "$GIPROJ/.gitignore")
if [ "$COUNT" = "1" ]; then
    pass "repl-spice-load-gitignore-idempotent"
else
    fail "repl-spice-load-gitignore-idempotent" \
         "expected 1 entry, found $COUNT in: $(cat "$GIPROJ/.gitignore")"
fi

echo
echo "repl-spice-load: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
