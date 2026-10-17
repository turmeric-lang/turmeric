#!/usr/bin/env bash
# tests/run-offtree-load.sh -- a freestanding `.tur` script that lives outside
# the repo and does `(load "stdlib/<file>")` must resolve the stdlib via the
# exe-relative stdlib root, not just cwd-relative.
#
# Finding 3 of docs/archive/history/one-off-script-print-and-annotation-ergonomics.md:
# before the fix, `(load "stdlib/math.tur")` only opened the file when run from
# the repo root (cwd-relative). A `/tmp/foo.tur` run from elsewhere failed with
# "load: cannot open 'stdlib/math.tur'", so the load-line suggested by the
# "unknown function" hint did not work off-tree. `import` already fell back to
# the resolved stdlib root; this test pins the same behavior for `(load ...)`.

set -u
cd "$(dirname "$0")/.."

TUR_REL="./build/tur"
[ -x "$TUR_REL" ] || { echo "tests: $TUR_REL not built; run 'tur run build' first" >&2; exit 2; }
TUR="$(cd "$(dirname "$TUR_REL")" && pwd)/$(basename "$TUR_REL")"

PASS=0
FAIL=0
FAILED=()

note() { printf "  %s\n" "$*"; }
ok()   { PASS=$((PASS+1)); echo "PASS $1"; }
bad()  { FAIL=$((FAIL+1)); FAILED+=("$1"); echo "FAIL $1"; [ -n "${2:-}" ] && note "$2"; }

WORK="$(mktemp -d -t tur-offtree-load-XXXXXX)"
trap 'rm -rf "$WORK"' EXIT

# A script that lives OUTSIDE the repo and loads a stdlib file by its
# "stdlib/<file>" path -- exactly what the unknown-function hint suggests.
cat > "$WORK/probe.tur" <<'EOF'
(load "stdlib/math.tur")
(defn main [] : int
  (println (float->int 7.25))
  0)
EOF

# ------------------------------------------------------------------ #
# 1. `tur run` from the temp dir resolves stdlib off-tree.            #
#    Unset TUR_STDLIB_DIR so resolution must come from the exe walk,  #
#    not an inherited repo-root env var.                              #
# ------------------------------------------------------------------ #
out="$(cd "$WORK" && env -u TUR_STDLIB_DIR "$TUR" run probe.tur 2>&1)"
rc=$?
if [ "$rc" -eq 0 ] && [ "$out" = "7" ]; then
    ok "run: off-tree (load \"stdlib/math.tur\") resolves"
else
    bad "run: off-tree (load \"stdlib/math.tur\") resolves" "rc=$rc out=[$out]"
fi

# ------------------------------------------------------------------ #
# 2. `tur check` from the temp dir also resolves (compile path).      #
# ------------------------------------------------------------------ #
out="$(cd "$WORK" && env -u TUR_STDLIB_DIR "$TUR" check probe.tur 2>&1)"
rc=$?
if [ "$rc" -eq 0 ]; then
    ok "check: off-tree (load \"stdlib/math.tur\") resolves"
else
    bad "check: off-tree (load \"stdlib/math.tur\") resolves" "rc=$rc out=[$out]"
fi

# ------------------------------------------------------------------ #
# 3. A genuinely missing stdlib file still errors (no false recovery).#
# ------------------------------------------------------------------ #
cat > "$WORK/bad.tur" <<'EOF'
(load "stdlib/does-not-exist.tur")
(defn main [] : int 0)
EOF
out="$(cd "$WORK" && env -u TUR_STDLIB_DIR "$TUR" check bad.tur 2>&1)"
rc=$?
if [ "$rc" -ne 0 ] && printf '%s' "$out" | grep -q "load: cannot open"; then
    ok "check: missing stdlib file still errors"
else
    bad "check: missing stdlib file still errors" "rc=$rc out=[$out]"
fi

# ------------------------------------------------------------------ #
# 4. A cwd with its own stdlib/ does not shadow the resolved stdlib    #
#    (stdlib-load-reads-cwd-copy-first).  The cwd copy used to be read  #
#    first, so a build run from another checkout mixed its own prelude  #
#    with that checkout's explicitly loaded modules.  The decoy defines #
#    only `re/decoy`; the real stdlib/re.tur does not, so the call must #
#    be unbound on both engines.                                       #
# ------------------------------------------------------------------ #
mkdir -p "$WORK/shadow/stdlib"
printf '(defn re/decoy [] : int 41)\n' > "$WORK/shadow/stdlib/re.tur"
cat > "$WORK/shadow/probe.tur" <<'EOF'
(load "stdlib/re.tur")
(defn main [] : int
  (println (re/decoy))
  0)
EOF
for mode in run --interpret; do
    out="$(cd "$WORK/shadow" && ASAN_OPTIONS=detect_leaks=0 "$TUR" $mode probe.tur 2>&1)"
    rc=$?
    if [ "$rc" -ne 0 ] && ! printf '%s' "$out" | grep -qx "41"; then
        ok "$mode: a cwd stdlib/ copy does not shadow the resolved stdlib"
    else
        bad "$mode: a cwd stdlib/ copy does not shadow the resolved stdlib" "rc=$rc out=[$out]"
    fi
done

# ------------------------------------------------------------------ #
# Summary                                                            #
# ------------------------------------------------------------------ #
echo
echo "summary: $PASS passed, $FAIL failed"
if [ "$FAIL" -ne 0 ]; then
    printf 'FAILED: %s\n' "${FAILED[@]}"
    exit 1
fi
exit 0
