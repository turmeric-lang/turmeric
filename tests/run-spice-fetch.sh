#!/usr/bin/env bash
# tests/run-spice-fetch.sh -- `tur fetch` and the lockfile integrity check, end
# to end, against a local git repo.
#
# Everything this covers was broken on Windows and covered by nothing: the CI
# job there runs tests/run.sh directly and never ctest, so the whole package
# manager had no check on that platform at all.  In one sitting that hid four
# separate defects, every one of them silent:
#
#   - pkg_git_fetch interpolated POSIX '...' quoting into a string cmd.exe runs,
#     so git got `''./spices/demo''` and nothing could be fetched
#     (docs/reported/windows-spice-fetch-shell-quoting.md)
#   - pkg_lock_read sized with ftell and read in text mode, so it rejected every
#     tur.lock tur itself wrote
#     (docs/archive/windows-text-mode-read-rejects-own-files.md)
#   - the integrity hash shelled out to sha256sum, which MinGW does not ship
#     (docs/archive/pkg-hash-shells-out-to-sha256sum.md)
#   - rename() does not replace on Windows, so the lock was write-once
#     (docs/archive/windows-rename-does-not-replace.md)
#
# A `file://` remote keeps this hermetic: no network, no credentials, and the
# same code path a real URL dep takes.

set -u
cd "$(dirname "$0")/.."

TUR="${TUR:-./build/tur}"
[ -x "$TUR" ] || { echo "tests: $TUR not built; run 'just build' first" >&2; exit 2; }
# A self-skip is a green check that tested nothing, which is the same disease
# this harness was written to catch -- the first CI run of it skipped exactly
# here, because the MSYS2 environment ships no git, and reported success.  So
# where git is guaranteed (every CI leg), a missing one is fatal; elsewhere it
# still skips, since `tur fetch` genuinely cannot work without git.
if ! command -v git >/dev/null 2>&1; then
    if [ "${TUR_REQUIRE_GIT:-0}" = 1 ]; then
        echo "FAIL run-spice-fetch -- git not found, and TUR_REQUIRE_GIT=1" >&2
        exit 1
    fi
    echo "tests: git not found; skipping (set TUR_REQUIRE_GIT=1 to make this fatal)" >&2
    exit 0
fi

# An absolute path spelled the way the platform's own tools expect.  tur is a
# native binary on Windows and does not understand an MSYS /c/... path.
case "$(uname -s)" in
  MINGW*|MSYS*|CYGWIN*) native() { cygpath -m "$1"; } ;;
  *)                    native() { echo "$1"; } ;;
esac

PASS=0
FAIL=0
FAILED=()
note() { printf "  %s\n" "$*"; }
ok()   { PASS=$((PASS+1)); echo "PASS $1"; }
bad()  { FAIL=$((FAIL+1)); FAILED+=("$1"); echo "FAIL $1"; [ -n "${2:-}" ] && note "$2"; }

WORK="$(mktemp -d -t tur-fetch-tests-XXXXXX)"
trap 'rm -rf "$WORK"' EXIT

case "$TUR" in
  /*|?:*) TUR_ABS="$(native "$TUR")" ;;
  *)      TUR_ABS="$(native "$(pwd)")/${TUR#./}" ;;
esac

# ------------------------------------------------------------------ #
# The spice being depended on, as a local git repo.
# ------------------------------------------------------------------ #
mkdir -p "$WORK/demo/src"
cat > "$WORK/demo/build.tur" <<'EOF'
(defpackage demo
  :name    "demo"
  :version "0.1.0"
  :exports ["demo"])
EOF
cat > "$WORK/demo/src/demo.tur" <<'EOF'
(defmodule demo
  (export answer)
  (defn answer [] : int 42))
EOF
git -C "$WORK/demo" init -q
# Pin line endings: the tree hash is over CONTENT, so a machine with
# core.autocrlf=true must not produce a different fixture.
git -C "$WORK/demo" config core.autocrlf false
git -C "$WORK/demo" add -A
git -C "$WORK/demo" -c user.email=t@t -c user.name=t commit -qm init

# ------------------------------------------------------------------ #
# The consumer.
# ------------------------------------------------------------------ #
mkdir -p "$WORK/app/src"
cat > "$WORK/app/build.tur" <<EOF
(defpackage app
  :name    "app"
  :version "0.1.0"
  :spices  #map{"demo" #map{:url "file://$(native "$WORK/demo")"}})
EOF
cat > "$WORK/app/src/main.tur" <<'EOF'
(defmodule main
  (import demo :refer [answer])
  (defn main [] : int
    (println (answer))
    0))
EOF

cd "$WORK/app" || exit 1

# 1. fetch clones the dep.
out="$("$TUR_ABS" fetch 2>&1)"
if [ -d spices/demo ] && [ -f spices/demo/build.tur ]; then
    ok "fetch clones a file:// spice"
else
    bad "fetch clones a file:// spice" "$out"
fi

# 2. the lock records a hash tagged with the algorithm that produced it, so a
#    lockfile an older tur wrote is never compared against a newer hash.
if grep -q ':sha256 "tree1:[0-9a-f]\{64\}"' tur.lock 2>/dev/null; then
    ok "tur.lock records a tagged tree hash"
else
    bad "tur.lock records a tagged tree hash" "$(cat tur.lock 2>&1)"
fi

verdict() {
    out="$("$TUR_ABS" run 2>&1)"
    # Case-insensitive on purpose: this asks "was tampering reported", not
    # "is the diagnostic spelled exactly this way". WP7's C-3 work rewrote the
    # message to print both hashes and name `tur fetch --update`, and the
    # capitalisation alone used to break this.
    if echo "$out" | grep -qi "integrity check failed"; then echo tampered
    elif echo "$out" | grep -q "^42";                    then echo ran
    else echo "other: $out"; fi
}

# 3. a clean tree runs, and is not reported as tampered.
v="$(verdict)"
[ "$v" = ran ] && ok "clean tree runs" || bad "clean tree runs" "$v"

# 4. the hash covers CONTENT, not the git metadata beside it: running git in a
#    fetched spice used to change .git/index's mtimes and so its hash, and the
#    next `tur run` reported tampering on a tree nobody had touched.
git -C spices/demo status >/dev/null 2>&1
v="$(verdict)"
[ "$v" = ran ] && ok "a git command in the tree does not look like tampering" \
               || bad "a git command in the tree does not look like tampering" "$v"

# 5. a real edit IS tampering.
echo ';; tampered' >> spices/demo/src/demo.tur
v="$(verdict)"
[ "$v" = tampered ] && ok "an edited dependency is reported" \
                    || bad "an edited dependency is reported" "$v"

# 6. and restoring it clears.
git -C spices/demo checkout -- . 2>/dev/null
v="$(verdict)"
[ "$v" = ran ] && ok "restoring the dependency clears the report" \
               || bad "restoring the dependency clears the report" "$v"

# 7. a SECOND fetch must rewrite the lock.  On Windows rename() does not replace
#    an existing file, so this is where a write-once lockfile shows up.
out="$("$TUR_ABS" fetch --update 2>&1)"
if grep -q ':sha256 "tree1:[0-9a-f]\{64\}"' tur.lock 2>/dev/null \
   && ! echo "$out" | grep -qi "rename failed"; then
    ok "a second fetch rewrites the lock"
else
    bad "a second fetch rewrites the lock" "$out"
fi

# 8. tur-fetch-exit-code-optional-vs-required: the exit status tells an
#    optional-dep failure from a required one.  A consumer whose `:optional`
#    dep points nowhere still gets its lock written and exits 1; the same
#    dep declared required exits 2.  Both nonzero -- the point is that CI can
#    warn on 1 and fail on 2 instead of guessing.
mkdir -p "$WORK/app2/src"
cat > "$WORK/app2/build.tur" <<EOF
(defpackage app2
  :name    "app2"
  :version "0.1.0"
  :spices  #map{"demo"  #map{:url "file://$(native "$WORK/demo")"}
                "ghost" #map{:url "file://$(native "$WORK/no-such-repo")"
                             :optional true}})
EOF
cp "$WORK/app/src/main.tur" "$WORK/app2/src/main.tur"
out="$(cd "$WORK/app2" && "$TUR_ABS" fetch 2>&1)"; rc=$?
if [ "$rc" -eq 1 ] && [ -d "$WORK/app2/spices/demo" ] \
   && grep -q '"demo"' "$WORK/app2/tur.lock" 2>/dev/null \
   && grep -q "failed to fetch optional 'ghost'" <<< "$out"; then
    ok "an unfetchable :optional dep is skipped, the lock written, exit 1"
else
    bad "an unfetchable :optional dep is skipped, the lock written, exit 1" "rc=$rc $out"
fi

mkdir -p "$WORK/app3/src"
cat > "$WORK/app3/build.tur" <<EOF
(defpackage app3
  :name    "app3"
  :version "0.1.0"
  :spices  #map{"demo"  #map{:url "file://$(native "$WORK/demo")"}
                "ghost" #map{:url "file://$(native "$WORK/no-such-repo")"}})
EOF
cp "$WORK/app/src/main.tur" "$WORK/app3/src/main.tur"
out="$(cd "$WORK/app3" && "$TUR_ABS" fetch 2>&1)"; rc=$?
if [ "$rc" -eq 2 ] && grep -q "failed to fetch 'ghost'" <<< "$out"; then
    ok "an unfetchable REQUIRED dep exits 2"
else
    bad "an unfetchable REQUIRED dep exits 2" "rc=$rc $out"
fi

# 9. and a clean fetch is still 0 (the pair, so neither half passes for the
#    wrong reason).
"$TUR_ABS" fetch >/dev/null 2>&1; rc=$?
[ "$rc" -eq 0 ] && ok "a clean fetch exits 0" || bad "a clean fetch exits 0" "rc=$rc"

# ------------------------------------------------------------------ #
# C-3 (docs/upcoming/security-audit-plan.md): the lock VERIFIES, it does not
# merely record.
#
# Everything above tests a dependency edited AFTER a fetch, which the old code
# did catch. The three below are what it did not: a fetch that re-downloads at
# all, a fetch that compares what came back against the pin, and the commands
# other than `tur run` doing the same check.
# ------------------------------------------------------------------ #

cd "$WORK/app" || exit 1
PINNED="$(grep -o ':sha256 "[^"]*"' tur.lock 2>/dev/null | head -n1)"

# 10. the fresh-clone shape: tur.lock committed, spices/ gitignored and absent.
#     The "already in the lock, skip it" test never checked whether the
#     directory was THERE, so this printed "using cached 'demo'" and fetched
#     nothing, leaving the build to fail later with "module not found".
rm -rf spices
out="$("$TUR_ABS" fetch 2>&1)"
if [ -d spices/demo ]; then
    ok "fetch re-downloads when spices/ is missing"
else
    bad "fetch re-downloads when spices/ is missing" "$out"
fi

# 11. upstream moves under a branch-shaped :ref.  The lock recorded a commit,
#     and a fresh fetch checks THAT commit out -- not the branch tip
#     (lock-tracks-ref-not-resolved-commit).  It used to clone the tip, fail
#     the integrity check, and leave `--update` (re-pin to the new commit) as
#     the only way forward: the recorded commit was the one thing `tur fetch`
#     would not fetch.
RESOLVED="$(grep -o ':resolved "[^"]*"' tur.lock 2>/dev/null | head -n1)"
cat >> "$WORK/demo/src/demo.tur" <<'EOF'

(defmodule demo-extra
  (export backdoor)
  (defn backdoor [] : int 1337))
EOF
git -C "$WORK/demo" -c user.email=t@t -c user.name=t commit -qam upstream-moved
rm -rf spices
out="$("$TUR_ABS" fetch 2>&1)"; rc=$?
if [ "$rc" -eq 0 ] && [ -d spices/demo ] && ! grep -q backdoor spices/demo/src/demo.tur; then
    ok "fetch checks out the pinned commit after the branch moved"
else
    bad "fetch checks out the pinned commit after the branch moved" "rc=$rc $out"
fi
if [ "$(grep -o ':sha256 "[^"]*"' tur.lock 2>/dev/null | head -n1)" = "$PINNED" ] &&
   [ "$(grep -o ':resolved "[^"]*"' tur.lock 2>/dev/null | head -n1)" = "$RESOLVED" ]; then
    ok "a pinned fetch leaves the pin as it was"
else
    bad "a pinned fetch leaves the pin as it was" "$(cat tur.lock 2>&1)"
fi

# 11b. ...and when the pinned commit is GONE from upstream (history rewritten
#      and collected), the fetch fails rather than quietly taking the branch.
#      A fallback to :ref would turn the pin back into branch-tracking on
#      exactly the remotes least worth trusting.  The upstream is restored
#      afterwards for 12.
cp -R "$WORK/demo" "$WORK/demo-saved"
DEMO_BRANCH="$(git -C "$WORK/demo" symbolic-ref --short HEAD)"
git -C "$WORK/demo" checkout -q --orphan tur-rewritten
git -C "$WORK/demo" -c user.email=t@t -c user.name=t commit -qm rewritten
git -C "$WORK/demo" branch -q -D "$DEMO_BRANCH"
git -C "$WORK/demo" branch -q -m tur-rewritten "$DEMO_BRANCH"
git -C "$WORK/demo" reflog expire --expire=now --all
git -C "$WORK/demo" gc -q --prune=now 2>/dev/null
rm -rf spices
out="$("$TUR_ABS" fetch 2>&1)"; rc=$?
if [ "$rc" -ne 0 ] && grep -qi "cannot be fetched" <<< "$out" && [ ! -d spices/demo ]; then
    ok "fetch refuses when the pinned commit is gone, and keeps no clone"
else
    bad "fetch refuses when the pinned commit is gone, and keeps no clone" "rc=$rc $out"
fi
rm -rf "$WORK/demo" && mv "$WORK/demo-saved" "$WORK/demo"

# 12. --update is the deliberate escape hatch the diagnostic names.
out="$("$TUR_ABS" fetch --update 2>&1)"; rc=$?
NOW="$(grep -o ':sha256 "[^"]*"' tur.lock 2>/dev/null | head -n1)"
if [ "$rc" -eq 0 ] && [ -n "$NOW" ] && [ "$NOW" != "$PINNED" ]; then
    ok "tur fetch --update re-pins to the new content"
else
    bad "tur fetch --update re-pins to the new content" "rc=$rc $out"
fi

# 13. `tur build` verifies too. It never did -- the comparison was open-coded
#     inside `tur run` and existed nowhere else, so the command that turns a
#     dependency's source into a binary you keep was the one that did not look.
echo ';; tampered again' >> spices/demo/src/demo.tur
out="$("$TUR_ABS" build . 2>&1)"; rc=$?
if [ "$rc" -ne 0 ] && grep -qi "integrity check failed" <<< "$out"; then
    ok "tur build refuses a tampered dependency"
else
    bad "tur build refuses a tampered dependency" "rc=$rc $out"
fi

# 14. and `tur audit` reports rather than closing with "it verifies nothing".
out="$("$TUR_ABS" audit 2>&1)"
if grep -qi "integrity check failed" <<< "$out"; then
    ok "tur audit reports a tampered dependency"
else
    bad "tur audit reports a tampered dependency" "$out"
fi

# 15. the pair: a clean tree makes audit say so, so 14 cannot pass by shouting
#     at everything.
git -C spices/demo checkout -- . 2>/dev/null
out="$("$TUR_ABS" audit 2>&1)"
if grep -q "matches tur.lock" <<< "$out"; then
    ok "tur audit is quiet on a clean tree"
else
    bad "tur audit is quiet on a clean tree" "$out"
fi

# 16. --frozen (the `npm ci` / `cargo --locked` shape) fetches exactly what the
#     lock pins and never writes it: a fresh checkout reproduces the pin and
#     tur.lock is byte-for-byte untouched -- fetched_at included.
rm -rf spices
LOCK_BEFORE="$(cat tur.lock)"
out="$("$TUR_ABS" fetch --frozen 2>&1)"; rc=$?
if [ "$rc" -eq 0 ] && [ -d spices/demo ] && [ "$(cat tur.lock)" = "$LOCK_BEFORE" ]; then
    ok "fetch --frozen reproduces the pin and leaves tur.lock untouched"
else
    bad "fetch --frozen reproduces the pin and leaves tur.lock untouched" "rc=$rc $out"
fi

# 17. ...and fails, without writing, when the manifest asks for something the
#     lock does not pin.
cp build.tur build.tur.saved
cat > build.tur <<EOF
(defpackage app
  :name    "app"
  :version "0.1.0"
  :spices  #map{"demo"  #map{:url "file://$(native "$WORK/demo")"}
                "demo2" #map{:url "file://$(native "$WORK/demo")"}})
EOF
out="$("$TUR_ABS" fetch --frozen 2>&1)"; rc=$?
if [ "$rc" -ne 0 ] && grep -qi "would change" <<< "$out" && [ "$(cat tur.lock)" = "$LOCK_BEFORE" ]; then
    ok "fetch --frozen refuses a dependency the lock does not pin"
else
    bad "fetch --frozen refuses a dependency the lock does not pin" "rc=$rc $out"
fi
mv build.tur.saved build.tur

# 18. --frozen with --update is a usage error, not a silent choice.
out="$("$TUR_ABS" fetch --frozen --update 2>&1)"; rc=$?
if [ "$rc" -eq 2 ] && [ "$(cat tur.lock)" = "$LOCK_BEFORE" ]; then
    ok "fetch --frozen --update is refused"
else
    bad "fetch --frozen --update is refused" "rc=$rc $out"
fi

echo
echo "spice-fetch summary: $PASS passed, $FAIL failed"
if [ "$FAIL" -gt 0 ]; then
    echo "failed:"
    for f in "${FAILED[@]}"; do echo "  - $f"; done
    exit 1
fi
exit 0
