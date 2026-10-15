#!/usr/bin/env bash
# tests/run-sweet-manifest.sh -- verifies build.tur.sweet is treated as a
# fully equivalent manifest by walk-up discovery and pkg_manifest_read.

set -u
cd "$(dirname "$0")/.."

TUR="$PWD/build/tur"
[ -x "$TUR" ] || { echo "tests: $TUR not built; run 'tur run build' first" >&2; exit 2; }

FIXTURE="tests/fixtures/pkg-sweet-manifest"
ENTRY="$FIXTURE/src/main.tur"

PASS=0
FAIL=0
FAILED=()

assert_exit() {
    local want="$1" label="$2"; shift 2
    local err rc
    err=$(mktemp)
    "$@" >/dev/null 2>"$err"
    rc=$?
    if [ "$rc" -eq "$want" ]; then
        echo "PASS $label"
        PASS=$((PASS + 1))
    else
        echo "FAIL $label -- expected exit $want, got $rc"
        echo "  cmd: $*"
        echo "  stderr:"; sed 's/^/    /' "$err"
        FAIL=$((FAIL + 1))
        FAILED+=("$label")
    fi
    rm -f "$err"
}

# SW1: `tur check <src/main.tur>` walks up, finds build.tur.sweet, parses it
# through the sweet-exp reader, succeeds.
assert_exit 0 "SW1: tur check auto-discovers build.tur.sweet" \
    "$TUR" check "$ENTRY"

# SW2: `tur emit-c` also auto-discovers and parses the sweet manifest.
emit_out=$(mktemp)
"$TUR" emit-c "$ENTRY" >"$emit_out" 2>/dev/null
emit_rc=$?
if [ "$emit_rc" -eq 0 ] && [ -s "$emit_out" ]; then
    echo "PASS SW2: tur emit-c produces C for sweet-manifest project"
    PASS=$((PASS + 1))
else
    echo "FAIL SW2: tur emit-c failed (exit=$emit_rc, output size=$(wc -c <"$emit_out"))"
    FAIL=$((FAIL + 1))
    FAILED+=("SW2: tur emit-c sweet manifest")
fi
rm -f "$emit_out"

# SW3: precedence -- when both manifests exist, build.tur wins.
TMP=$(mktemp -d -t tur-sweet-prio.XXXXXX)
trap 'rm -rf "$TMP"' EXIT
mkdir -p "$TMP/src"
cat >"$TMP/build.tur" <<'EOF'
(defpackage "plain-wins"
  :version "0.0.1")
EOF
cat >"$TMP/build.tur.sweet" <<'EOF'
#lang sweet-exp
defpackage "sweet-loses"
  :version "0.0.2"
EOF
cat >"$TMP/src/main.tur" <<'EOF'
(defn main [] : int 0)
EOF
assert_exit 0 "SW3: precedence -- tur check resolves with plain manifest present" \
    "$TUR" check "$TMP/src/main.tur"

# SW4: tur init --sweet must produce build.tur.sweet.
INIT_DIR=$(mktemp -d -t tur-sweet-init.XXXXXX)
( cd "$INIT_DIR" && "$TUR" init --sweet --no-git sweet-init-test ) >/dev/null 2>&1
if [ -f "$INIT_DIR/build.tur.sweet" ] && [ ! -f "$INIT_DIR/build.tur" ]; then
    echo "PASS SW4: tur init --sweet emits build.tur.sweet"
    PASS=$((PASS + 1))
else
    echo "FAIL SW4: tur init --sweet did not produce build.tur.sweet"
    ls -la "$INIT_DIR"
    FAIL=$((FAIL + 1))
    FAILED+=("SW4: tur init --sweet")
fi

# SW5: tur init refuses to run when build.tur.sweet already exists.
INIT_REFUSE_ERR=$(mktemp)
( cd "$INIT_DIR" && "$TUR" init --no-git already-here ) >/dev/null 2>"$INIT_REFUSE_ERR"
refuse_rc=$?
if [ "$refuse_rc" -ne 0 ] && grep -q "build.tur.sweet already exists" "$INIT_REFUSE_ERR"; then
    echo "PASS SW5: tur init refuses with build.tur.sweet present"
    PASS=$((PASS + 1))
else
    echo "FAIL SW5: tur init should have refused"
    sed 's/^/    /' "$INIT_REFUSE_ERR"
    FAIL=$((FAIL + 1))
    FAILED+=("SW5: tur init refuse")
fi
rm -f "$INIT_REFUSE_ERR"
rm -rf "$INIT_DIR"

echo
echo "summary: $PASS passed, $FAIL failed"
if [ "$FAIL" -ne 0 ]; then
    echo "failed cases:"
    for f in "${FAILED[@]}"; do echo "  - $f"; done
    exit 1
fi
exit 0
