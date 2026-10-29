import { CONTENT_SECURITY_POLICY } from './csp.js';
import { GH_REPO } from './repo.js';
import { handleCiApi } from './ci-api.js';

// C-1 in docs/upcoming/security-audit-plan.md.
//
// This script used to be `brew install --HEAD rjungemann/turmeric/turmeric`,
// which built whatever was on `main` at the instant you ran it and verified
// nothing. A bad afternoon on main shipped to every new install, and the
// formula is HEAD-only so there was no pinned alternative behind the same
// command.
//
// It now bootstraps tvm and installs the latest RELEASE, whose tarball tvm
// checks against that release's sha256sums.txt and -- as of WP7's C-2 fix --
// refuses to unpack if the check cannot run. Three properties change:
//
//   1. What lands is a tagged release, not a moving branch.
//   2. It is checksum-verified, and fails closed when it cannot be.
//   3. It works on Linux, which the Homebrew one-liner never did.
//
// tvm itself is fetched from the release's own assets (the workflow attaches
// the tvm.sh / install.sh it tested with, pinned to a commit of
// turmeric-lang/tvm), or for older releases from the tagged tree, rather than
// from main, so the bootstrap does not reintroduce the very thing it removes.
// Either way it is pinned to the release. That is the same
// trust root as the release (whoever can move the tag can move the assets),
// not a stronger one -- the guarantee being added here is "a pinned, verified
// artifact", not "a second independent signer".
//
// `brew install --HEAD` still works and is still documented, as the explicit
// opt-in for people who want to build main on purpose.
const INSTALL_SCRIPT = `#!/bin/sh
set -eu

# Turmeric installer -- https://turmeric-lang.com
#
# Installs tvm (the Turmeric version manager) and then the latest release.
# The release tarball is verified against the release's sha256sums.txt.
#
#   TVM_DIR=~/somewhere   install tvm elsewhere (default ~/.tvm)

REPO="${GH_REPO}"
API="\${TUR_INSTALL_API:-https://api.github.com/repos/\$REPO}"
RAW="\${TUR_INSTALL_RAW:-https://raw.githubusercontent.com/\$REPO}"
ASSETS="\${TUR_INSTALL_ASSETS:-https://github.com/\$REPO/releases/download}"

have() { command -v "\$1" >/dev/null 2>&1; }

fetch() {
  if have curl; then curl -fsSL "\$1"
  elif have wget; then wget -qO - "\$1"
  else
    echo "The Turmeric installer needs curl or wget." >&2
    exit 1
  fi
}

# --- which release? --------------------------------------------------------
# Resolved at run time rather than baked in, so this script does not need
# redeploying on every release. sed rather than jq: jq is not standard.
TAG="\$(fetch "\$API/releases/latest" \\
        | sed -n 's/.*"tag_name"[[:space:]]*:[[:space:]]*"\\([^"]*\\)".*/\\1/p' \\
        | head -n1)"
if [ -z "\$TAG" ]; then
  echo "Could not determine the latest Turmeric release." >&2
  echo "Check https://github.com/\$REPO/releases and install with tvm:" >&2
  echo "  https://github.com/turmeric-lang/tvm#readme" >&2
  exit 1
fi
VER="\${TAG#v}"
echo "Turmeric \$VER"

# --- bootstrap tvm, pinned to that release's tag ---------------------------
TMP="\$(mktemp -d "\${TMPDIR:-/tmp}/tur-install.XXXXXX")"
trap 'rm -rf "\$TMP"' EXIT INT TERM
mkdir -p "\$TMP/tvm"
for f in tvm.sh install.sh; do
  # tvm lives in turmeric-lang/tvm; the release workflow attaches the copy it
  # was tested with to each release. A release cut BEFORE that move has no such
  # asset but still has tvm/ in its tagged tree, so fall back to that.
  if ! fetch "\$ASSETS/\$TAG/\$f" > "\$TMP/tvm/\$f" 2>/dev/null || [ ! -s "\$TMP/tvm/\$f" ]; then
    fetch "\$RAW/\$TAG/tvm/\$f" > "\$TMP/tvm/\$f" || {
      echo "Could not download \$f for \$TAG." >&2; exit 1; }
  fi
  # A proxy that 200s an error page would otherwise be sourced as a shell
  # script. Non-empty is a weak check, but it is the cheap one worth having.
  [ -s "\$TMP/tvm/\$f" ] || { echo "\$f downloaded empty." >&2; exit 1; }
done

sh "\$TMP/tvm/install.sh"

# --- install the compiler --------------------------------------------------
TVM_DIR="\${TVM_DIR:-\$HOME/.tvm}"
# shellcheck disable=SC1090
. "\$TVM_DIR/tvm.sh"

# Branch on whether a prebuilt asset EXISTS for this host, not on whether the
# install failed. Falling back to a source build on any error would quietly
# paper over a checksum mismatch -- the one failure that must be loud.
if __tvm_target >/dev/null 2>&1; then
  tvm install --activate "\$VER"
else
  echo ""
  echo "No prebuilt binary for \$(uname -s)/\$(uname -m) -- building \$VER from source."
  echo "This needs cmake and a C compiler, and takes a few minutes."
  echo ""
  tvm install --build --activate "\$VER"
fi

cat <<EOF

Turmeric \$VER installed.

  Open a new shell (your shell rc was updated), or run:
      export TVM_DIR="\$TVM_DIR"
      . "\$TVM_DIR/tvm.sh"

  Then:
      tur --help

  Other versions:      tvm ls-remote / tvm install <version>
  Build main instead:  brew install --HEAD \${REPO}/turmeric
  Playground:          https://turmeric-lang.com/try

EOF
`;

export default {
  async fetch(request, env, ctx) {
    const url = new URL(request.url);
    const { hostname, pathname } = url;

    // The D1 store behind /ci: CI reports here, the page reads from here
    // (ci-api.js, docs/archive/ci-metrics-d1-plan.md).
    if (pathname.startsWith('/api/ci/')) {
      return handleCiApi(request, env, ctx);
    }

    if (pathname === '/install') {
      return new Response(INSTALL_SCRIPT, {
        headers: {
          'Content-Type': 'text/plain; charset=utf-8',
          'Cache-Control': 'no-cache',
          'Content-Security-Policy': CONTENT_SECURITY_POLICY,
        },
      });
    }

    // Rewrite try.turmeric-lang.com/* -> turmeric-lang.com/try/*
    // so both URLs serve the same page without a redirect round-trip.
    const response = await env.ASSETS.fetch(request);

    // SharedArrayBuffer (required for Emscripten pthreads) is only available
    // in cross-origin isolated contexts.
    const headers = new Headers(response.headers);
    headers.set('Cross-Origin-Opener-Policy', 'same-origin');
    headers.set('Cross-Origin-Embedder-Policy', 'require-corp');
    // `_headers` rules do not reach a response the Worker returns, so the
    // policy is set here too -- `set`, not `append`, so an asset response that
    // already carries it does not end up with two copies (two policies are
    // both enforced, and the header would read as a comma-joined pair).
    headers.set('Content-Security-Policy', CONTENT_SECURITY_POLICY);
    return new Response(response.body, {
      status: response.status,
      statusText: response.statusText,
      headers,
    });
  },
};
