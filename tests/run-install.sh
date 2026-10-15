#!/usr/bin/env bash
# tests/run-install.sh -- exercise `tur install` / `tur uninstall` /
# subcommand fallthrough end to end, isolated under a temporary TUR_HOME.

set -u
cd "$(dirname "$0")/.."

TUR="${TUR:-./build/tur}"
[ -x "$TUR" ] || { echo "tests: $TUR not built; run 'tur run build' first" >&2; exit 2; }

PASS=0
FAIL=0
FAILED=()

note() { printf "  %s\n" "$*"; }
ok()   { PASS=$((PASS+1)); echo "PASS $1"; }
bad()  { FAIL=$((FAIL+1)); FAILED+=("$1"); echo "FAIL $1"; [ -n "${2:-}" ] && note "$2"; }

# Two things this script has to spell per-platform.  Both are identities off
# Windows, so the POSIX assertions are unchanged.
case "$(uname -s)" in
  MINGW*|MSYS*|CYGWIN*) IS_WINDOWS=1 ;;
  *)                    IS_WINDOWS=0 ;;
esac

# The FILE NAME a :bin entry takes in bin_dir.  Windows resolves a bare
# command name through PATHEXT, so `tur install` writes `tur-foo.exe`; a test
# that plants a file "at the same bin path" has to plant it at that path.
binfile() {
  if [ "$IS_WINDOWS" = 1 ]; then echo "$1.exe"; else echo "$1"; fi
}

# tur prints paths the way Windows spells them (C:/Users/...), while $WORK
# here is an MSYS path (/tmp/... or /c/Users/...).  Comparing tur output
# against a shell variable needs one spelling; this is it.
native() {
  if [ "$IS_WINDOWS" = 1 ]; then cygpath -m "$1"; else echo "$1"; fi
}

WORK="$(mktemp -d -t tur-install-tests-XXXXXX)"
trap 'rm -rf "$WORK"' EXIT

export TUR_HOME="$WORK/tur-home"

# ------------------------------------------------------------------ #
# Fixture: a minimal binary spice with a single :bin entry.
# ------------------------------------------------------------------ #
SPICE="$WORK/sample-spice"
mkdir -p "$SPICE/src"
cat > "$SPICE/build.tur" <<'EOF'
;;; build.tur -- sample binary spice for installer tests.
(defpackage sample
  :name    "sample"
  :version "0.1.0"
  :bin #{
    "tur-sample" "src/main.tur"
  })
EOF
cat > "$SPICE/src/main.tur" <<'EOF'
;;; sample binary entrypoint.
(defn main [] :int
  (println "hello from sample")
  0)
EOF

# ------------------------------------------------------------------ #
# 1. Install from --path                                              #
# ------------------------------------------------------------------ #
out="$("$TUR" install "$SPICE" --path 2>&1)"
rc=$?
if [ "$rc" -ne 0 ]; then
    bad "install --path" "$out"
else
    ok "install --path"
fi

# 2. state.tur was written and lists the entry.
if grep -q '"sample"' "$TUR_HOME/state.tur" 2>/dev/null; then
    ok "state.tur written"
else
    bad "state.tur written" "missing or empty"
fi

# 3. bin symlink exists and is executable.
if [ -x "$TUR_HOME/bin/tur-sample" ]; then
    ok "bin/tur-sample exists"
else
    bad "bin/tur-sample exists"
fi

# 4. Binary runs and prints the expected line.
out="$("$TUR_HOME/bin/tur-sample" 2>&1)"
if [ "$out" = "hello from sample" ]; then
    ok "bin runs"
else
    bad "bin runs" "got: $out"
fi

# 5. Subcommand fallthrough: `tur sample` should exec tur-sample.
out="$(PATH="$TUR_HOME/bin:$PATH" "$TUR" sample 2>&1)"
if [ "$out" = "hello from sample" ]; then
    ok "subcommand fallthrough"
else
    bad "subcommand fallthrough" "got: $out"
fi

# 6. Unknown subcommand: exits nonzero with a clean diagnostic.
out="$(PATH="$TUR_HOME/bin:$PATH" "$TUR" nonexistent-zzz 2>&1)"
rc=$?
if [ "$rc" -ne 0 ] && echo "$out" | grep -q "is not a tur command"; then
    ok "unknown subcommand diagnostic"
else
    bad "unknown subcommand diagnostic" "rc=$rc out: $out"
fi

# 7. Reinstalling our own spice replaces the symlink without --force.
out="$("$TUR" install "$SPICE" --path 2>&1)"
rc=$?
if [ "$rc" -eq 0 ]; then
    ok "reinstall replaces own symlink"
else
    bad "reinstall replaces own symlink" "$out"
fi

# 8. Conflict: a foreign file at the same bin path is refused without --force.
echo "#!/bin/sh" > "$TUR_HOME/bin/$(binfile tur-conflict)"
chmod +x "$TUR_HOME/bin/$(binfile tur-conflict)"
# Create a second spice with the same bin name.
SPICE2="$WORK/conflict-spice"
mkdir -p "$SPICE2/src"
cat > "$SPICE2/build.tur" <<'EOF'
(defpackage conflict
  :name    "conflict"
  :version "0.1.0"
  :bin #{ "tur-conflict" "src/main.tur" })
EOF
cat > "$SPICE2/src/main.tur" <<'EOF'
(defn main [] :int (println "conflict bin") 0)
EOF
out="$("$TUR" install "$SPICE2" --path 2>&1)"
rc=$?
if [ "$rc" -ne 0 ] && echo "$out" | grep -q "refusing to overwrite"; then
    ok "conflict refused without --force"
else
    bad "conflict refused without --force" "rc=$rc out: $out"
fi

# 9. --force overrides the conflict.
out="$("$TUR" install "$SPICE2" --path --force 2>&1)"
rc=$?
if [ "$rc" -eq 0 ]; then
    ok "conflict accepted with --force"
else
    bad "conflict accepted with --force" "$out"
fi

# 10. Uninstall round-trips: removes symlink and clears state entry.
out="$("$TUR" uninstall sample 2>&1)"
rc=$?
if [ "$rc" -eq 0 ] && [ ! -e "$TUR_HOME/bin/tur-sample" ] && \
   ! grep -q '"sample"' "$TUR_HOME/state.tur"; then
    ok "uninstall round-trip"
else
    bad "uninstall round-trip" "rc=$rc out: $out"
fi

# 11. :bin name validation -- a manifest with a non-tur- prefix should be rejected.
BAD="$WORK/bad-bin-spice"
mkdir -p "$BAD/src"
cat > "$BAD/build.tur" <<'EOF'
(defpackage bad
  :name    "bad"
  :version "0.1.0"
  :bin #{ "noprefix" "src/main.tur" })
EOF
cat > "$BAD/src/main.tur" <<'EOF'
(defn main [] :int 0)
EOF
out="$("$TUR" install "$BAD" --path 2>&1)"
rc=$?
if [ "$rc" -ne 0 ] && echo "$out" | grep -q "must start with 'tur-'"; then
    ok ":bin prefix validation"
else
    bad ":bin prefix validation" "rc=$rc out: $out"
fi

# 12. TUR_HOME / XDG_DATA_HOME override -- swap TUR_HOME and confirm install
#     goes there.
ALT_HOME="$WORK/alt-home"
( TUR_HOME="$ALT_HOME" "$TUR" install "$SPICE" --path >/dev/null 2>&1 )
if [ -x "$ALT_HOME/bin/tur-sample" ] && [ -f "$ALT_HOME/state.tur" ]; then
    ok "TUR_HOME override"
else
    bad "TUR_HOME override"
fi

# ------------------------------------------------------------------ #
# M3: tur list                                                        #
# ------------------------------------------------------------------ #

# Reinstall sample (it was uninstalled above) so list has content.
"$TUR" install "$SPICE" --path >/dev/null 2>&1

# 13. Default tree output: contains the name, [path] tag, binary line, footer.
out="$("$TUR" list 2>&1)"
if echo "$out" | grep -q "sample" && \
   echo "$out" | grep -q "\[path\]" && \
   echo "$out" | grep -q "tur-sample" && \
   echo "$out" | grep -qE "spice(s)? installed"; then
    ok "list default tree"
else
    bad "list default tree" "$out"
fi

# 14. PATH-health line: NOT on path when bin_dir absent from $PATH.
if echo "$out" | grep -q "PATH entry:" && echo "$out" | grep -q "NOT on PATH"; then
    ok "list PATH-health line (not on path)"
else
    bad "list PATH-health line (not on path)" "$out"
fi

# 15. PATH-health line: active when bin_dir on $PATH.
out="$(PATH="$TUR_HOME/bin:$PATH" "$TUR" list 2>&1)"
if echo "$out" | grep -q "PATH entry:.*active"; then
    ok "list PATH-health line (active)"
else
    bad "list PATH-health line (active)" "$out"
fi

# 16. --json output contains expected keys and the entry.
out="$("$TUR" --json list 2>&1)"
if echo "$out" | grep -q '"installed":' && \
   echo "$out" | grep -q '"name": "sample"' && \
   echo "$out" | grep -q '"bin": \["tur-sample"\]'; then
    ok "list --json structure"
else
    bad "list --json structure" "$out"
fi

# 17. --json is valid JSON (best-effort with python3 if available).
if command -v python3 >/dev/null 2>&1; then
    if echo "$out" | python3 -c "import sys,json; json.load(sys.stdin)" 2>/dev/null; then
        ok "list --json parses as JSON"
    else
        bad "list --json parses as JSON"
    fi
fi

# 18. --verbose succeeds and still names the spice.
out="$("$TUR" list --verbose 2>&1)"
rc=$?
if [ "$rc" -eq 0 ] && echo "$out" | grep -q "sample"; then
    ok "list --verbose runs"
else
    bad "list --verbose runs" "rc=$rc out: $out"
fi

# 19. --verbose with --json is rejected.
out="$("$TUR" --json list --verbose 2>&1)"
rc=$?
if [ "$rc" -ne 0 ] && echo "$out" | grep -q "mutually exclusive"; then
    ok "list --verbose + --json rejected"
else
    bad "list --verbose + --json rejected" "rc=$rc out: $out"
fi

# ------------------------------------------------------------------ #
# M4: tur upgrade                                                     #
# ------------------------------------------------------------------ #

# 20. --dry-run reports what would happen without mutating state.tur.
state_before="$(cat "$TUR_HOME/state.tur")"
out="$("$TUR" upgrade --dry-run sample 2>&1)"
state_after="$(cat "$TUR_HOME/state.tur")"
if echo "$out" | grep -q "would rebuild 'sample'" && \
   [ "$state_before" = "$state_after" ]; then
    ok "upgrade --dry-run is read-only"
else
    bad "upgrade --dry-run is read-only" "$out"
fi

# 21. upgrade <name> rebuilds and re-symlinks.
out="$("$TUR" upgrade sample 2>&1)"
rc=$?
if [ "$rc" -eq 0 ] && echo "$out" | grep -q "upgraded 'sample'" && \
   [ -x "$TUR_HOME/bin/tur-sample" ]; then
    ok "upgrade <name> rebuilds"
else
    bad "upgrade <name> rebuilds" "rc=$rc out: $out"
fi

# Install a second spice so --all has more than one target.
SPICE3="$WORK/another-spice"
mkdir -p "$SPICE3/src"
cat > "$SPICE3/build.tur" <<'EOF'
(defpackage another
  :name    "another"
  :version "0.2.0"
  :bin #{ "tur-another" "src/main.tur" })
EOF
cat > "$SPICE3/src/main.tur" <<'EOF'
(defn main [] :int (println "another") 0)
EOF
"$TUR" install "$SPICE3" --path >/dev/null 2>&1

# 22. upgrade --all walks every entry in state.tur.
out="$("$TUR" upgrade --all 2>&1)"
rc=$?
ok_count=$(echo "$out" | grep -c "upgraded '")
if [ "$rc" -eq 0 ] && [ "$ok_count" -ge 2 ]; then
    ok "upgrade --all iterates"
else
    bad "upgrade --all iterates" "rc=$rc count=$ok_count out: $out"
fi

# 23. upgrade --all with an explicit name is rejected.
out="$("$TUR" upgrade --all sample 2>&1)"
rc=$?
if [ "$rc" -ne 0 ] && echo "$out" | grep -q "mutually exclusive"; then
    ok "upgrade --all + name rejected"
else
    bad "upgrade --all + name rejected" "rc=$rc out: $out"
fi

# 24. upgrade for an unknown name fails cleanly.
out="$("$TUR" upgrade nonexistent-zzz 2>&1)"
rc=$?
if [ "$rc" -ne 0 ] && echo "$out" | grep -q "is not installed"; then
    ok "upgrade unknown name rejected"
else
    bad "upgrade unknown name rejected" "rc=$rc out: $out"
fi

# ------------------------------------------------------------------ #
# M5: polish flags                                                    #
# ------------------------------------------------------------------ #

# 25. --print-path-snippet emits both shell snippets and never installs.
out="$("$TUR" install --print-path-snippet 2>&1)"
rc=$?
NAT_HOME="$(native "$TUR_HOME")"
if [ "$rc" -eq 0 ] && \
   echo "$out" | grep -q "export PATH=\"$NAT_HOME/bin:\$PATH\"" && \
   echo "$out" | grep -q "set -gx PATH $NAT_HOME/bin \$PATH"; then
    ok "install --print-path-snippet"
else
    bad "install --print-path-snippet" "rc=$rc out: $out"
fi

# 26. --print-path-snippet works even with no state.tur / empty install.
empty="$WORK/empty-home"
out="$(TUR_HOME="$empty" "$TUR" install --print-path-snippet 2>&1)"
rc=$?
if [ "$rc" -eq 0 ] && echo "$out" | grep -q "$(native "$empty")/bin"; then
    ok "install --print-path-snippet (empty home)"
else
    bad "install --print-path-snippet (empty home)" "rc=$rc out: $out"
fi

# 27. list --outdated on a --path-only install reports "up to date"
#     (no remote to query) and exits 0.
out="$("$TUR" list --outdated 2>&1)"
rc=$?
if [ "$rc" -eq 0 ] && echo "$out" | grep -q "up to date"; then
    ok "list --outdated (path installs only)"
else
    bad "list --outdated (path installs only)" "rc=$rc out: $out"
fi

# 28. list --outdated --json emits a valid JSON envelope with checked/skipped.
out="$("$TUR" --json list --outdated 2>&1)"
rc=$?
if [ "$rc" -eq 0 ] && \
   echo "$out" | grep -q '"outdated":' && \
   echo "$out" | grep -q '"checked":' && \
   echo "$out" | grep -q '"skipped":'; then
    ok "list --outdated --json"
else
    bad "list --outdated --json" "rc=$rc out: $out"
fi

# 29. list --outdated + --verbose is rejected.
out="$("$TUR" list --outdated --verbose 2>&1)"
rc=$?
if [ "$rc" -ne 0 ] && echo "$out" | grep -q "mutually exclusive"; then
    ok "list --outdated + --verbose rejected"
else
    bad "list --outdated + --verbose rejected" "rc=$rc out: $out"
fi

# ------------------------------------------------------------------ #
# global-spice-library-consumption: a spice installed here is consumable
# as a LIBRARY through a `#{:global true}` manifest dep -- it resolves from
# the install registry (state.tur), is never fetched, and its src/ joins the
# consuming project's include path.
# ------------------------------------------------------------------ #

# 30. Give the sample spice a module and re-install it, so there is something
#     to import.  (The :bin entry stays: `tur install` is a binary installer,
#     so a library-only spice cannot be registered today.)
mkdir -p "$SPICE/src/sample"
cat > "$SPICE/src/sample/core.tur" <<'EOF'
(defmodule sample/core
  (export sample-answer)
  (defn sample-answer [] : int 42))
EOF
out="$("$TUR" install "$SPICE" --path --force 2>&1)"
rc=$?
if [ "$rc" -eq 0 ]; then
    ok "re-install sample with a library module"
else
    bad "re-install sample with a library module" "rc=$rc out: $out"
fi

# 31. A project declaring the installed spice as :global builds against it.
# These cases `cd` into the consuming project, so the compiler path must be
# absolute -- $TUR is "./build/tur" by default.
case "$TUR" in
    /*) TUR_ABS="$TUR" ;;
    *)  TUR_ABS="$(cd "$(dirname "$TUR")" && pwd)/$(basename "$TUR")" ;;
esac

APP="$WORK/global-consumer"
mkdir -p "$APP/src"
cat > "$APP/build.tur" <<'EOF'
(defpackage app
  :version "0.1.0"
  :spices #map{"sample" #map{:global true}})
EOF
cat > "$APP/src/main.tur" <<'EOF'
(defmodule app/main
  (import sample/core :refer [sample-answer])
  (defn main [] : int
    (println (sample-answer))
    0))
EOF
out="$(cd "$APP" && "$TUR_ABS" fetch 2>&1)"
rc=$?
if [ "$rc" -eq 0 ]; then
    ok "fetch accepts a :global dep (nothing to fetch)"
else
    bad "fetch accepts a :global dep" "rc=$rc out: $out"
fi

out="$(cd "$APP" && "$TUR_ABS" build . -o "$APP/appbin" 2>&1)"
rc=$?
if [ "$rc" -eq 0 ] && [ -x "$APP/appbin" ] && [ "$("$APP/appbin")" = "42" ]; then
    ok "build resolves modules from a :global spice"
else
    bad "build resolves modules from a :global spice" "rc=$rc out: $out"
fi

# 32. A :global dep naming a spice that is NOT installed is a hard error,
#     naming the fix rather than failing later as "module not found".
cat > "$APP/build.tur" <<'EOF'
(defpackage app
  :version "0.1.0"
  :spices #map{"nosuch" #map{:global true}})
EOF
out="$(cd "$APP" && "$TUR_ABS" fetch 2>&1)"
rc=$?
if [ "$rc" -ne 0 ] && [[ "$out" == *"no such spice is installed"* ]]; then
    ok ":global dep for an uninstalled spice is rejected"
else
    bad ":global dep for an uninstalled spice is rejected" "rc=$rc out: $out"
fi

# 33. :global together with :url/:path is a manifest error -- the two describe
#     different resolution sources.
cat > "$APP/build.tur" <<'EOF'
(defpackage app
  :version "0.1.0"
  :spices #map{"sample" #map{:global true :url "https://example.invalid/x.git"}})
EOF
out="$(cd "$APP" && "$TUR_ABS" fetch 2>&1)"
rc=$?
if [ "$rc" -ne 0 ] && [[ "$out" == *"takes neither"* ]]; then
    ok ":global + :url is rejected"
else
    bad ":global + :url is rejected" "rc=$rc out: $out"
fi

echo
echo "install summary: $PASS passed, $FAIL failed"
if [ "$FAIL" -ne 0 ]; then
    for f in "${FAILED[@]}"; do echo "  FAILED: $f"; done
    exit 1
fi
exit 0
