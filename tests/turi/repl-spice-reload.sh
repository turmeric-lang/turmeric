#!/usr/bin/env bash
# tests/turi/repl-spice-reload.sh -- RP5 integration test.
#
# Scenarios:
#   1. (reload) outside a project: clean error, no crash.
#   2. (reload) in a project with no source changes: "no changes".
#   3. (reload) after a source edit: rebuilds, new behavior is live.
#   4. (reload) with extra args: arity error.
#
# Scenario 3 requires the REPL to stay alive while the source file is
# mutated. We drive that via a FIFO so input is fed line by line rather
# than queued up in a single pipe buffer that the REPL would drain
# before we got around to editing the file.

set -uo pipefail
cd "$(dirname "$0")/../.."

TUR="${TUR:-./build/tur}"
[ -x "$TUR" ] || { echo "tests: $TUR not built; run 'tur run build' first" >&2; exit 2; }
export TUR_BIN
case "$TUR" in
    /*) TUR_BIN="$TUR" ;;
    *)  TUR_BIN="$(cd "$(dirname "$TUR")" && pwd)/$(basename "$TUR")" ;;
esac
# Ensure the tur binary is on PATH so the REPL's spice-rebuild subprocess
# can find it when running `tur build` to AOT-compile the spice.
export PATH="$(dirname "$TUR_BIN"):$PATH"

WORK="$(mktemp -d -t tur-rp5.XXXXXX)"
trap 'rm -rf "$WORK"; rm -f "$WORK.fifo"' EXIT

PASS=0
FAIL=0
pass() { PASS=$((PASS + 1)); echo "PASS $1"; }
fail() { FAIL=$((FAIL + 1)); echo "FAIL $1 -- $2"; }

# -------- scenario 1: outside any project --------------------------------
NOPROJ="$WORK/no-project"
mkdir -p "$NOPROJ"
out=$(cd "$NOPROJ" && printf '(reload)\n:quit\n' | "$TUR_BIN" repl 2>&1)
if grep -q "no spice loaded" <<< "$out"; then
    pass "rp5-reload-outside-project"
else
    fail "rp5-reload-outside-project" "$out"
fi

# -------- scenario 2: in-project, no changes -----------------------------
PROJ="$WORK/proj"
mkdir -p "$PROJ/src"
cat > "$PROJ/build.tur" <<'EOF'
(defpackage rp5-fixture)
EOF
cat > "$PROJ/src/lib.tur" <<'EOF'
(defmodule sh
  (export add42)
  (defn add42 [x :int] :int (+ x 42)))
EOF
out=$(cd "$PROJ" && printf '(reload)\n:quit\n' | "$TUR_BIN" repl 2>&1)
# First call rebuilds (no cache yet), second invocation would say "no
# changes" -- this test runs a single REPL, so the (reload) call here
# happens *after* the startup load fired, and the sources are fresh.
if grep -q "no changes" <<< "$out"; then
    pass "rp5-reload-no-changes"
else
    fail "rp5-reload-no-changes" "$out"
fi

# -------- scenario 3: edit-while-running rebuilds ------------------------
PROJ3="$WORK/proj3"
mkdir -p "$PROJ3/src"
cat > "$PROJ3/build.tur" <<'EOF'
(defpackage rp5-edit-fixture)
EOF
cat > "$PROJ3/src/lib.tur" <<'EOF'
(defmodule sh
  (export add42)
  (defn add42 [x :int] :int (+ x 42)))
EOF
FIFO="$WORK.fifo"
mkfifo "$FIFO"
OUT3="$WORK/out3"
(cd "$PROJ3" && "$TUR_BIN" repl < "$FIFO") >"$OUT3" 2>&1 &
REPL_PID=$!
# Open the FIFO for write so the REPL doesn't see EOF until we close it.
exec 3>"$FIFO"
printf '(add42 0)\n' >&3

# Wait until the first eval has printed a result before mutating the source.
# This guarantees the REPL has loaded the spice and the new mtime will be
# strictly greater than the loaded library's mtime.
_ticks=0
while ! grep -q '^=> 42$' "$OUT3" 2>/dev/null; do
    sleep 0.1; _ticks=$((_ticks+1))
    [ "$_ticks" -ge 600 ] && break  # 60s timeout
done

# Guarantee new mtime > library mtime on 1s-granularity filesystems.
_mtime_before="$(stat -c %Y "$PROJ3/src/lib.tur" 2>/dev/null || stat -f %m "$PROJ3/src/lib.tur" 2>/dev/null)"
cat > "$PROJ3/src/lib.tur" <<'EOF'
(defmodule sh
  (export add42)
  (defn add42 [x :int] :int (+ x 100)))
EOF
_mtime_now="$(stat -c %Y "$PROJ3/src/lib.tur" 2>/dev/null || stat -f %m "$PROJ3/src/lib.tur" 2>/dev/null)"
while [ -n "$_mtime_before" ] && [ -n "$_mtime_now" ] && [ "$_mtime_now" -le "$_mtime_before" ]; do
    sleep 0.3; touch "$PROJ3/src/lib.tur"
    _mtime_now="$(stat -c %Y "$PROJ3/src/lib.tur" 2>/dev/null || stat -f %m "$PROJ3/src/lib.tur" 2>/dev/null)"
done

printf '(reload)\n(add42 0)\n:quit\n' >&3
exec 3>&-
wait "$REPL_PID"

if grep -q '^=> 42$' "$OUT3" \
    && grep -q '(reload) rebuilt 1 export' "$OUT3" \
    && grep -q '^=> 100$' "$OUT3"; then
    pass "rp5-reload-edit-while-running"
else
    fail "rp5-reload-edit-while-running" \
         "expected to see => 42 then 'rebuilt' then => 100, got: $(cat "$OUT3")"
fi
rm -f "$FIFO"

# -------- scenario 4: arity check ----------------------------------------
out=$(cd "$PROJ" && printf '(reload 1)\n:quit\n' | "$TUR_BIN" repl 2>&1)
if grep -q "(reload) takes no arguments" <<< "$out"; then
    pass "rp5-reload-arity-check"
else
    fail "rp5-reload-arity-check" "$out"
fi

echo
echo "repl-spice-reload: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
