import { CONTENT_SECURITY_POLICY } from './csp.js';
import { GH_REPO } from './repo.js';

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
// tvm itself is fetched at the release TAG rather than from main, so the
// bootstrap does not reintroduce the very thing it removes. That is the same
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
  echo "  https://github.com/\$REPO/blob/main/tvm/README.md" >&2
  exit 1
fi
VER="\${TAG#v}"
echo "Turmeric \$VER"

# --- bootstrap tvm, pinned to that release's tag ---------------------------
TMP="\$(mktemp -d "\${TMPDIR:-/tmp}/tur-install.XXXXXX")"
trap 'rm -rf "\$TMP"' EXIT INT TERM
mkdir -p "\$TMP/tvm"
for f in tvm.sh install.sh; do
  fetch "\$RAW/\$TAG/tvm/\$f" > "\$TMP/tvm/\$f" || {
    echo "Could not download tvm/\$f at \$TAG." >&2; exit 1; }
  # A proxy that 200s an error page would otherwise be sourced as a shell
  # script. Non-empty is a weak check, but it is the cheap one worth having.
  [ -s "\$TMP/tvm/\$f" ] || { echo "tvm/\$f downloaded empty." >&2; exit 1; }
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

const TIMINGS_BASE = `https://raw.githubusercontent.com/${GH_REPO}/ci-metrics`;

// Both files on the `ci-metrics` branch are append-only NDJSON partitioned by
// year (tools/ci/publish-timings.sh), so one proxy serves both.
//
// On Jan 1 the current year's file does not exist until the first push to main
// lands, so a miss falls back to the previous year rather than 502ing. The year
// actually served comes back in X-Metrics-Year, because "which partition is
// this" is not derivable from the body.
async function proxyMetricsNDJSON(url, stem, missing) {
  const asked = url.searchParams.get('year') ?? '';
  const year = /^\d{4}$/.test(asked)
    ? asked
    : String(new Date().getUTCFullYear());

  for (const y of [year, String(Number(year) - 1)]) {
    const res = await fetch(`${TIMINGS_BASE}/${stem}-${y}.jsonl`, {
      cf: { cacheTtl: 300, cacheEverything: true },
    });
    if (res.ok) {
      return new Response(res.body, {
        headers: {
          'Content-Type': 'application/x-ndjson; charset=utf-8',
          'Cache-Control': 'public, max-age=300',
          'X-Metrics-Year': y,
          'Content-Security-Policy': CONTENT_SECURITY_POLICY,
        },
      });
    }
  }

  return new Response(`${missing}\n`, {
    status: 502,
    headers: {
      'Content-Type': 'text/plain; charset=utf-8',
      'Content-Security-Policy': CONTENT_SECURITY_POLICY,
    },
  });
}

async function proxyMetricsFile(name, missing) {
  const res = await fetch(`${TIMINGS_BASE}/${name}`, {
    cf: { cacheTtl: 300, cacheEverything: true },
  });
  if (res.ok) {
    return new Response(res.body, {
      headers: {
        'Content-Type': 'application/x-ndjson; charset=utf-8',
        'Cache-Control': 'public, max-age=300',
        'Content-Security-Policy': CONTENT_SECURITY_POLICY,
      },
    });
  }
  return new Response(`${missing}\n`, {
    status: 502,
    headers: {
      'Content-Type': 'text/plain; charset=utf-8',
      'Content-Security-Policy': CONTENT_SECURITY_POLICY,
    },
  });
}

export default {
  async fetch(request, env) {
    const url = new URL(request.url);
    const { hostname, pathname } = url;

    if (pathname === '/install') {
      return new Response(INSTALL_SCRIPT, {
        headers: {
          'Content-Type': 'text/plain; charset=utf-8',
          'Cache-Control': 'no-cache',
          'Content-Security-Policy': CONTENT_SECURITY_POLICY,
        },
      });
    }

    // CI suite timings, proxied from the `ci-metrics` orphan branch so the
    // browser stays same-origin and the payload has one place to be shrunk.
    // TODO: once suite-timings-<year>.jsonl passes ~5 MB, aggregate here
    // (group by run x suite, drop the raw rows) instead of streaming it whole.
    if (pathname === '/api/ci-timings') {
      // ?window=recent: the last few days only (suite-timings-recent.jsonl,
      // rewritten by every publish). The full year is 40+ MB, and the page opens
      // on a 7-day range, so it asks for the window first and the whole log only
      // when a wider range is picked. No year fallback: the file is not
      // partitioned. A 502 here tells the page to fall back to the full log.
      if (url.searchParams.get('window') === 'recent') {
        return proxyMetricsFile('suite-timings-recent.jsonl', 'no recent timings available');
      }
      return proxyMetricsNDJSON(url, 'suite-timings', 'no timings available');
    }

    // Tracked line counts, one row per push to main. Far smaller than the
    // timings (one row a push, not one per suite per leg), so it never needs
    // the aggregation the TODO above describes.
    if (pathname === '/api/ci-loc') {
      return proxyMetricsNDJSON(url, 'repo-loc', 'no line counts available');
    }

    // Open plans and reports, one row per push to main. Same shape as the
    // line counts: a property of the commit, no runner dimension.
    if (pathname === '/api/ci-docs') {
      return proxyMetricsNDJSON(url, 'docs-counts', 'no doc counts available');
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
