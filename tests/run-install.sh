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

# ------------------------------------------------------------------ #
# installed-spice-libraries IL0/IL1: install layout, library installs #
# ------------------------------------------------------------------ #

# A local bare repo from a directory, served as a file:// URL.  pkg_git_fetch
# clones --depth 1, which wants a URL rather than a plain path.
mkrepo() {   # mkrepo <src-dir> -> prints the file:// URL of a bare clone
    ( cd "$1" && git init -q && git add -A &&
      git -c user.email=t@example.invalid -c user.name=t commit -qm init ) >/dev/null 2>&1
    git clone -q --bare "$1" "$1.git" >/dev/null 2>&1
    echo "file://$1.git"
}

# 34. D5: a monorepo `--subdir` install with no --name clones under the URL's
#     name (mono-HEAD) but registers the manifest's :name (json).  Every reader
#     used to rebuild the directory from the entry name and miss.
MONO="$WORK/mono"
mkdir -p "$MONO/spices/json/src/json"
cat > "$MONO/spices/json/build.tur" <<'EOF'
(defpackage json
  :name    "json"
  :version "0.3.0")
EOF
cat > "$MONO/spices/json/src/json/decode.tur" <<'EOF'
(defmodule json/decode
  (export json-answer)
  (defn json-answer [] : int 7))
EOF
MONO_URL="$(mkrepo "$MONO")"
out="$("$TUR" install "$MONO_URL" --subdir spices/json 2>&1)"
rc=$?
if [ "$rc" -eq 0 ] && [ -d "$TUR_HOME/spices/mono-HEAD/spices/json" ] &&
   grep -q ':dir ' "$TUR_HOME/state.tur" && grep -q ':src-dir ' "$TUR_HOME/state.tur"; then
    ok "subdir install without --name records :dir/:src-dir"
else
    bad "subdir install without --name records :dir/:src-dir" "rc=$rc out: $out"
fi
out="$("$TUR" --json list 2>&1)"
if python3 - "$out" <<'PY' 2>/dev/null
import json, sys
d = json.loads(sys.argv[1])
e = [x for x in d["installed"] if x["name"] == "json"][0]
assert e["source_dir"].endswith("mono-HEAD/spices/json"), e["source_dir"]
assert e["kind"] == "library" and e["modules"] == ["json/decode"], e
PY
then
    ok "list finds the subdir install's source"
else
    bad "list finds the subdir install's source" "$out"
fi
JAPP="$WORK/json-consumer"
mkdir -p "$JAPP/src"
cat > "$JAPP/build.tur" <<'EOF'
(defpackage japp
  :version "0.1.0"
  :spices #map{"json" #map{:global true}})
EOF
cat > "$JAPP/src/main.tur" <<'EOF'
(defmodule japp/main
  (import json/decode :refer [json-answer])
  (defn main [] : int
    (println (json-answer))
    0))
EOF
out="$(cd "$JAPP" && "$TUR_ABS" build . -o "$JAPP/appbin" 2>&1)"
rc=$?
if [ "$rc" -eq 0 ] && [ "$("$JAPP/appbin")" = "7" ]; then
    ok ":global dep resolves a subdir install"
else
    bad ":global dep resolves a subdir install" "rc=$rc out: $out"
fi
out="$("$TUR" uninstall json 2>&1)"
rc=$?
if [ "$rc" -eq 0 ] && [ ! -e "$TUR_HOME/spices/mono-HEAD" ]; then
    ok "uninstall removes the subdir install's clone"
else
    bad "uninstall removes the subdir install's clone" "rc=$rc out: $out"
fi

# 35. D5: `tur uninstall plot` removes plot's clone and nothing else -- it used
#     to delete every spices/plot-* directory, plot-extras-HEAD included.
for nm in plot plot-extras; do
    mkdir -p "$WORK/$nm/src/$nm"
    printf '(defpackage %s :name "%s" :version "0.1.0")\n' "$nm" "$nm" > "$WORK/$nm/build.tur"
    printf '(defmodule %s/core (export one) (defn one [] : int 1))\n' "$nm" > "$WORK/$nm/src/$nm/core.tur"
done
PLOT_URL="$(mkrepo "$WORK/plot")"
EXTRAS_URL="$(mkrepo "$WORK/plot-extras")"
"$TUR" install "$PLOT_URL" >/dev/null 2>&1
"$TUR" install "$EXTRAS_URL" >/dev/null 2>&1
out="$("$TUR" uninstall plot 2>&1)"
rc=$?
if [ "$rc" -eq 0 ] && [ ! -e "$TUR_HOME/spices/plot-HEAD" ] &&
   [ -d "$TUR_HOME/spices/plot-extras-HEAD" ]; then
    ok "uninstall plot leaves plot-extras alone"
else
    bad "uninstall plot leaves plot-extras alone" "rc=$rc out: $(ls "$TUR_HOME/spices") $out"
fi
"$TUR" uninstall plot-extras >/dev/null 2>&1

# 36. D4: an installed :bin spice whose dep has a dep of its own builds -- the
#     install is set up as a fetched project, so the whole closure lands in
#     the spice's own spices/ and reaches the build.
mkdir -p "$WORK/deep-b/src/deepb" "$WORK/deep-a/src/deepa" "$WORK/deep-tool/src"
cat > "$WORK/deep-b/build.tur" <<'EOF'
(defpackage deepb :name "deepb" :version "0.1.0")
EOF
cat > "$WORK/deep-b/src/deepb/core.tur" <<'EOF'
(defmodule deepb/core
  (export base)
  (defn base [] : int 40))
EOF
B_URL="$(mkrepo "$WORK/deep-b")"
cat > "$WORK/deep-a/build.tur" <<EOF
(defpackage deepa :name "deepa" :version "0.1.0"
  :spices #map{"deepb" #map{:url "$B_URL"}})
EOF
cat > "$WORK/deep-a/src/deepa/core.tur" <<'EOF'
(defmodule deepa/core
  (import deepb/core :refer [base])
  (export answer)
  (defn answer [] : int (+ (base) 2)))
EOF
A_URL="$(mkrepo "$WORK/deep-a")"
cat > "$WORK/deep-tool/build.tur" <<EOF
(defpackage deeptool :name "deeptool" :version "0.1.0"
  :spices #map{"deepa" #map{:url "$A_URL"}}
  :bin #{ "tur-deeptool" "src/main.tur" })
EOF
cat > "$WORK/deep-tool/src/main.tur" <<'EOF'
(defmodule deeptool/main
  (import deepa/core :refer [answer])
  (defn main [] : int
    (println (answer))
    0))
EOF
out="$("$TUR" install "$WORK/deep-tool" --path 2>&1)"
rc=$?
if [ "$rc" -eq 0 ] && [ "$("$TUR_HOME/bin/$(binfile tur-deeptool)" 2>&1)" = "42" ] &&
   [ -d "$WORK/deep-tool/spices/deepb" ]; then
    ok "installed tool with a transitive dep builds"
else
    bad "installed tool with a transitive dep builds" "rc=$rc out: $out"
fi
"$TUR" uninstall deeptool >/dev/null 2>&1

# 37. D3: a spice with no :bin but a src/ installs as a library.
LIB="$WORK/mathx"
mkdir -p "$LIB/src/mathx"
cat > "$LIB/build.tur" <<'EOF'
(defpackage mathx :name "mathx" :version "1.2.0")
EOF
cat > "$LIB/src/mathx/core.tur" <<'EOF'
(defmodule mathx/core
  (export triple)
  (defn triple [x : int] : int (* x 3)))
EOF
out="$("$TUR" install "$LIB" --path 2>&1)"
rc=$?
if [ "$rc" -eq 0 ] && [[ "$out" == *"[library]"* ]]; then
    ok "library-only install"
else
    bad "library-only install" "rc=$rc out: $out"
fi
out="$("$TUR" list 2>&1)"
if [[ "$out" == *"mathx 1.2.0 [library]"* ]] && [[ "$out" == *"library-only"* ]]; then
    ok "list marks a library-only install"
else
    bad "list marks a library-only install" "$out"
fi
out="$("$TUR" --json list 2>&1)"
if python3 - "$out" <<'PY' 2>/dev/null
import json, sys
d = json.loads(sys.argv[1])
e = [x for x in d["installed"] if x["name"] == "mathx"][0]
assert e["kind"] == "library" and e["bin"] == [] and e["modules"] == ["mathx/core"], e
t = [x for x in d["installed"] if x["name"] == "sample"][0]
assert t["kind"] == "tool" and "sample/core" in t["modules"] and "main" not in t["modules"], t
PY
then
    ok "list --json kind and modules"
else
    bad "list --json kind and modules" "$out"
fi

# 38. D3: neither :bin nor src/ -- nothing to install.
mkdir -p "$WORK/empty-spice"
echo '(defpackage empty :name "empty" :version "0.1.0")' > "$WORK/empty-spice/build.tur"
out="$("$TUR" install "$WORK/empty-spice" --path 2>&1)"
rc=$?
if [ "$rc" -ne 0 ] && [[ "$out" == *"no :bin entries and has no src/"* ]]; then
    ok "spice with neither :bin nor src/ is refused"
else
    bad "spice with neither :bin nor src/ is refused" "rc=$rc out: $out"
fi

# 39. D6: a second spice providing the same module path is refused; --force
#     installs it with a warning.
LIB2="$WORK/mathx-fork"
mkdir -p "$LIB2/src/mathx"
echo '(defpackage mathxfork :name "mathx-fork" :version "0.1.0")' > "$LIB2/build.tur"
cp "$LIB/src/mathx/core.tur" "$LIB2/src/mathx/core.tur"
out="$("$TUR" install "$LIB2" --path 2>&1)"
rc=$?
if [ "$rc" -ne 0 ] && [[ "$out" == *"module 'mathx/core' is already provided by installed spice 'mathx'"* ]]; then
    ok "module conflict refused"
else
    bad "module conflict refused" "rc=$rc out: $out"
fi
out="$("$TUR" install "$LIB2" --path --force 2>&1)"
rc=$?
if [ "$rc" -eq 0 ] && [[ "$out" == *"warning: module 'mathx/core'"* ]]; then
    ok "module conflict --force installs with a warning"
else
    bad "module conflict --force installs with a warning" "rc=$rc out: $out"
fi
"$TUR" uninstall mathx-fork >/dev/null 2>&1

# 40. A :global dep can name a library-only spice; upgrade and uninstall
#     accept library entries.
cat > "$APP/build.tur" <<'EOF'
(defpackage app
  :version "0.1.0"
  :spices #map{"mathx" #map{:global true}})
EOF
cat > "$APP/src/main.tur" <<'EOF'
(defmodule app/main
  (import mathx/core :refer [triple])
  (defn main [] : int
    (println (triple 14))
    0))
EOF
out="$(cd "$APP" && "$TUR_ABS" build . -o "$APP/appbin" 2>&1)"
rc=$?
if [ "$rc" -eq 0 ] && [ "$("$APP/appbin")" = "42" ]; then
    ok ":global dep on a library-only spice builds"
else
    bad ":global dep on a library-only spice builds" "rc=$rc out: $out"
fi
out="$("$TUR" upgrade mathx 2>&1)"
rc=$?
if [ "$rc" -eq 0 ] && [[ "$out" == *"upgraded 'mathx'"* ]]; then
    ok "upgrade accepts a library entry"
else
    bad "upgrade accepts a library entry" "rc=$rc out: $out"
fi
out="$("$TUR" uninstall mathx 2>&1)"
rc=$?
if [ "$rc" -eq 0 ] && [ -d "$LIB/src" ] && ! grep -q '"mathx"' "$TUR_HOME/state.tur"; then
    ok "uninstall a library entry (keeps the --path checkout)"
else
    bad "uninstall a library entry (keeps the --path checkout)" "rc=$rc out: $out"
fi

echo
echo "install summary: $PASS passed, $FAIL failed"
if [ "$FAIL" -ne 0 ]; then
    for f in "${FAILED[@]}"; do echo "  FAILED: $f"; done
    exit 1
fi
exit 0
