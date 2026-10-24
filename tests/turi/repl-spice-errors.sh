#!/usr/bin/env bash
# tests/turi/repl-spice-errors.sh -- RP7 integration test for the
# polished error-surface messages.
#
# Scenarios:
#   1. Stale exports.manifest references a non-existent symbol:
#      the diagnostic names the .so and points at (reload) +
#      `rm -rf .tur-repl-cache`.
#   2. Spice build fails (compile error in user source): output ends
#      with the actionable "fix the error above, then type (reload)"
#      line.
#   3. After a build failure the user fixes the source and (reload)
#      self-heals -- the spice loads and exports are callable
#      without restarting the REPL.
#   4. The "no dispatcher for shape" path already shipped in RP4
#      with the right pointer; verify the message format is intact.
#   5. (reload) outside any spice project says "no spice project here"
#      (not just "no spice loaded") so the user knows the failure
#      mode is "no build.tur nearby", not "loader hadn't run yet".

set -uo pipefail
cd "$(dirname "$0")/../.."

TUR="${TUR:-./build/tur}"
[ -x "$TUR" ] || { echo "tests: $TUR not built; run 'tur run build' first" >&2; exit 2; }
export TUR_BIN
case "$TUR" in
    /*) TUR_BIN="$TUR" ;;
    *)  TUR_BIN="$(cd "$(dirname "$TUR")" && pwd)/$(basename "$TUR")" ;;
esac

WORK="$(mktemp -d -t tur-rp7.XXXXXX)"
trap 'rm -rf "$WORK"; rm -f "$WORK.fifo"' EXIT

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

# -------- scenario 1: stale exports.manifest -----------------------------
P1="$WORK/stale"
mkdir -p "$P1/src"
cat > "$P1/build.tur" <<'EOF'
(defpackage rp7-stale)
EOF
cat > "$P1/src/lib.tur" <<'EOF'
(defmodule sh (export real) (defn real [] :int 1))
EOF
# Populate the cache by running once cleanly.
(cd "$P1" && echo ':quit' | "$TUR_BIN" repl >/dev/null 2>&1)
# Corrupt the manifest in place.
cat > "$P1/.tur-repl-cache/exports.manifest" <<'EOF'
sh/real -> sh__real :: () -> :int
sh/ghost -> sh__ghost :: () -> :int
EOF
# Touch the image so the freshness check doesn't rebuild and overwrite our edits.
sleep 1
touch "$P1/.tur-repl-cache/$CACHE_LIB"
out=$(cd "$P1" && echo ':quit' | "$TUR_BIN" repl 2>&1)
if   grep -q "stale exports.manifest" <<< "$out" \
  && grep -q "sh__ghost" <<< "$out" \
  && grep -qF "$CACHE_LIB" <<< "$out" \
  && grep -q "(reload)" <<< "$out" \
  && grep -q "rm -rf .tur-repl-cache" <<< "$out"; then
    pass "rp7-stale-manifest-hint"
else
    fail "rp7-stale-manifest-hint" "$out"
fi

# -------- scenario 2: build failure prints actionable hint ---------------
P2="$WORK/badbuild"
mkdir -p "$P2/src"
cat > "$P2/build.tur" <<'EOF'
(defpackage rp7-bad)
EOF
cat > "$P2/src/lib.tur" <<'EOF'
(defmodule sh (export f) (defn f [] :int (no-such-name)))
EOF
out=$(cd "$P2" && echo ':quit' | "$TUR_BIN" repl 2>&1)
if   grep -q "spice rebuild failed" <<< "$out" \
  && grep -q "fix the error above" <<< "$out" \
  && grep -q "(reload)" <<< "$out"; then
    pass "rp7-build-failure-hint"
else
    fail "rp7-build-failure-hint" "$out"
fi

# -------- scenario 3: (reload) self-heal after fixing source -------------
P3="$WORK/heal"
mkdir -p "$P3/src"
cat > "$P3/build.tur" <<'EOF'
(defpackage rp7-heal)
EOF
cat > "$P3/src/lib.tur" <<'EOF'
(defmodule sh (export f) (defn f [] :int (no-such-name)))
EOF
FIFO="$WORK.fifo"
mkfifo "$FIFO"
OUT3="$P3/out.log"
(cd "$P3" && "$TUR_BIN" repl < "$FIFO") >"$OUT3" 2>&1 &
REPL_PID=$!
exec 3>"$FIFO"
# Wait for the failed startup load to FINISH before fixing the source.  A
# fixed `sleep 1` here was a race: on a loaded CI runner (the auxiliary
# suites run under -j) the REPL had not yet read src/lib.tur when the fix
# landed, so the STARTUP load succeeded, `(reload)` then said "no changes",
# and the "loaded 1 export" assertion below failed -- the rp7-reload-self-heal
# flake in docs/archive/ci-two-fixtures-flake-on-hosted-runners.md.  Poll
# for the startup diagnostic instead (the same line scenario 2 asserts),
# then still give the clock a second so the rewrite's mtime moves past the
# failed build's.
for _ in $(seq 1 300); do
    grep -q "fix the error above" "$OUT3" 2>/dev/null && break
    sleep 0.2
done
sleep 1
# Fix the source.
cat > "$P3/src/lib.tur" <<'EOF'
(defmodule sh (export f) (defn f [] :int 42))
EOF
sleep 1
printf '(reload)\n(f)\n:quit\n' >&3
exec 3>&-
wait "$REPL_PID"
if   grep -q '(reload) loaded 1 export' "$OUT3" \
  && grep -qx '=> 42' "$OUT3"; then
    pass "rp7-reload-self-heal"
else
    fail "rp7-reload-self-heal" "$(cat "$OUT3")"
fi
rm -f "$FIFO"

# -------- scenario 4: no-shim message (regression guard) -----------------
# This message lives in src/turi/ffi_thunk.c: an export whose library has no
# `__ffi` shim (built before 2026-07-22) on a build without the JIT.  It is
# hard to trigger -- every library the REPL builds now has its shims -- so
# grep the binary's strings for the actionable wording instead; if the text
# changes, this test catches it.  (The FFI shape table that used to answer
# here was retired by ffi-spices-integration-plan S5.)
# Materialise strings(1) output to a file first; piping into `grep -q`
# under `set -o pipefail` propagates SIGPIPE from grep's early exit
# back to strings and flips the test result.
TUR_STRINGS_TMP="$WORK/tur.strings"
strings "$TUR" >"$TUR_STRINGS_TMP" 2>/dev/null || true
if grep -q 'has no __ffi shim' "$TUR_STRINGS_TMP" \
   && grep -q 'Rebuild the spice' "$TUR_STRINGS_TMP"; then
    pass "rp7-no-shim-message-intact"
else
    fail "rp7-no-shim-message-intact" \
         "missing the 'has no __ffi shim ... Rebuild the spice' message in tur binary"
fi

# -------- scenario 5: (reload) outside any project ----------------------
NOPROJ="$WORK/noproj"
mkdir -p "$NOPROJ"
out=$(cd "$NOPROJ" && printf '(reload)\n:quit\n' | "$TUR_BIN" repl 2>&1)
if grep -q "no spice project here" <<< "$out"; then
    pass "rp7-reload-outside-project-message"
else
    fail "rp7-reload-outside-project-message" "$out"
fi

echo
echo "repl-spice-errors: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
