# `mise` integration via a native asdf-style plugin

## Goal

Let users on [`mise`](https://mise.jdx.dev) manage Turmeric compiler versions
with the same ergonomics as any other tool (`mise install turmeric@0.23.1`,
`.mise.toml`, `.tool-versions`, shims, project-local pins) **without
requiring `tvm` to be installed**. `mise` users should be able to install
and use Turmeric with mise alone.

The plugin is a self-contained asdf-style plugin: a small set of shell
scripts that talk directly to GitHub releases, download (or build) the
release artifact, and lay it out where `mise` expects. `tvm` becomes
optional -- users who only want mise don't need it.

## Why native, not a `tvm` passthrough

A `tvm`-passthrough plugin (~4 shell scripts that shell out to `tvm`) was
the obvious shortcut, but it has real drawbacks:

- **Hard dependency on `tvm`.** Users who already have a mise workflow have
  to install a second tool just to bootstrap the first one. That's exactly
  the friction the plugin is supposed to remove.
- **Coupling to `tvm`'s internal shell functions** (`__tvm_cmd_install`,
  `__tvm_cmd_ls_remote`, `TVM_DIR` layout). These are not a stable API;
  treating them as one freezes `tvm`'s internals.
- **Confusing uninstall semantics.** Symlinking from `$TVM_DIR/versions/`
  into `$ASDF_INSTALL_PATH` shares disk but means `tvm uninstall` silently
  breaks mise's view of installed versions.
- **Two install paths to debug.** A user hitting a bad download sees
  `tvm`'s error surface filtered through mise's shim layer -- harder to
  triage than a single native path.

The native route is what most language version managers effectively
converged on: `rbenv` delegates to `ruby-build`, `nodenv` to `node-build`,
`asdf-python` to `python-build` -- each is a self-contained installer
that knows how to fetch and lay out a release without depending on a
separate "system" version manager. We follow that pattern.

`tvm` stays the recommended CLI for users not on mise; the plugin and
`tvm` are independent installers that happen to consume the same GitHub
release artifacts.

## How `mise` plugins work (the minimum we need)

`mise` natively supports asdf-style plugins. A plugin is a git repo with a
`bin/` directory containing standardized scripts. `mise` calls them with
predictable env vars (`ASDF_INSTALL_VERSION`, `ASDF_INSTALL_PATH`, ...) and
expects each to read stdin / write stdout in a documented format.

Required scripts for a minimal install-only plugin:

| Script | Contract |
|---|---|
| `bin/list-all` | Print all installable versions, newline-separated, oldest→newest. |
| `bin/install` | Install `$ASDF_INSTALL_VERSION` into `$ASDF_INSTALL_PATH`. Exit 0 on success. |
| `bin/list-bin-paths` | Print space- or newline-separated subpaths (relative to `$ASDF_INSTALL_PATH`) that should land on `PATH` -- for us, just `bin`. |
| `bin/latest-stable` | Print the single newest non-pre-release tag. |

Optional but cheap wins: `bin/exec-env` (export `TUR_STDLIB_DIR`),
`bin/help.overview`, `bin/help.deps`.

## Repo layout

A separate repo, **`asdf-turmeric`** (name follows the asdf-plugin convention
that `mise` recognizes by default):

```
asdf-turmeric/
  bin/
    list-all
    install
    list-bin-paths
    latest-stable
    exec-env          # optional: exports TUR_STDLIB_DIR
    help.overview     # optional
    help.deps         # optional
  lib/
    utils.bash        # shared helpers: target detection, download, verify, extract
  README.md
  LICENSE
  .github/workflows/ci.yml   # smoke-install a known tag on linux+macos
```

Hosting under `rjungemann/asdf-turmeric` keeps it out of the main
`turmeric` repo so plugin iterations don't churn the compiler tree, and so
the mise registry can point at a stable URL.

## What the plugin needs from the release workflow

These are the only contracts the plugin depends on. They are already true
of the existing Release workflow; the plan is to **freeze them as supported
public surface** the plugin can rely on:

1. **Tags** are `vMAJOR.MINOR.PATCH` on the `turmeric` repo, listed by
   `GET /repos/rjungemann/turmeric/releases`.
2. **Per-target prebuilt tarballs** are attached as release assets with a
   stable naming convention, e.g.
   `turmeric-vX.Y.Z-<arch>-<os>.tar.gz` (matching whatever `tvm` already
   downloads).
3. **A SHA-256 checksum file** is published alongside, e.g.
   `turmeric-vX.Y.Z-<arch>-<os>.tar.gz.sha256` (or a single
   `SHA256SUMS` per release).
4. **Extracted layout** is `bin/tur`, `stdlib/`, optionally
   `include/`, `lib/`.
5. **Source tarball** `turmeric-vX.Y.Z-src.tar.gz` (or the GitHub-generated
   `v<X.Y.Z>.tar.gz`) is available as a fallback for hosts with no
   prebuilt asset.

Any naming gap between what the workflow publishes today and what the
plugin needs to assume gets closed in `turmeric` first, in one PR, so
the plugin never has to special-case missing assets.

## Script designs

The scripts are deliberately small and share helpers via `lib/utils.bash`.

### `lib/utils.bash` (shared helpers)

```sh
# Resolve <arch>-<os> the same way the release workflow does.
turmeric_target() {
  local os arch
  case "$(uname -s)" in
    Linux)  os=linux ;;
    Darwin) os=macos ;;
    *) echo "unsupported OS: $(uname -s)" >&2; return 1 ;;
  esac
  case "$(uname -m)" in
    x86_64|amd64) arch=x86_64 ;;
    arm64|aarch64) arch=arm64 ;;
    *) echo "unsupported arch: $(uname -m)" >&2; return 1 ;;
  esac
  echo "${arch}-${os}"
}

turmeric_release_url() {
  local version="$1" target="$2"
  echo "https://github.com/turmeric-lang/turmeric/releases/download/v${version}/turmeric-v${version}-${target}.tar.gz"
}

turmeric_download() {  # url, dest
  curl --fail --location --silent --show-error -o "$2" "$1"
}

turmeric_verify_sha256() {  # file, expected
  local got
  if command -v sha256sum >/dev/null; then
    got="$(sha256sum "$1" | awk '{print $1}')"
  else
    got="$(shasum -a 256 "$1" | awk '{print $1}')"
  fi
  [ "$got" = "$2" ] || { echo "sha256 mismatch: $1" >&2; return 1; }
}
```

### `bin/list-all`

```sh
#!/usr/bin/env bash
set -euo pipefail
# Page through GitHub releases; print bare versions, oldest first.
curl --fail --silent --show-error \
  "https://api.github.com/repos/rjungemann/turmeric/releases?per_page=100" \
  | grep -E '"tag_name":' \
  | sed -E 's/.*"v?([0-9]+\.[0-9]+\.[0-9]+(-[A-Za-z0-9.+-]+)?)".*/\1/' \
  | awk 'NF' \
  | sort -V
```

No `jq` dependency -- a `grep | sed` pass over `tag_name` lines is enough
for the asdf contract and keeps the install footprint zero. (If the
release list grows past one page, switch to the `Link:` header pagination;
that's a 5-line addition.)

### `bin/install`

```sh
#!/usr/bin/env bash
set -euo pipefail
: "${ASDF_INSTALL_TYPE:?}"   # "version" or "ref"
: "${ASDF_INSTALL_VERSION:?}"
: "${ASDF_INSTALL_PATH:?}"
: "${ASDF_DOWNLOAD_PATH:=$(mktemp -d)}"

plugin_dir="$(cd "$(dirname "$0")/.." && pwd)"
. "$plugin_dir/lib/utils.bash"

v="$ASDF_INSTALL_VERSION"
target="$(turmeric_target)"
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT

url="$(turmeric_release_url "$v" "$target")"
tarball="$tmp/turmeric.tar.gz"

if turmeric_download "$url" "$tarball"; then
  # Verify checksum if the .sha256 sidecar is published.
  if turmeric_download "${url}.sha256" "$tarball.sha256" 2>/dev/null; then
    expected="$(awk '{print $1}' < "$tarball.sha256")"
    turmeric_verify_sha256 "$tarball" "$expected"
  fi
  mkdir -p "$ASDF_INSTALL_PATH"
  tar -xzf "$tarball" -C "$ASDF_INSTALL_PATH" --strip-components=1
else
  # No prebuilt asset for this target -- fall back to a source build.
  echo "no prebuilt asset for $target; building v$v from source" >&2
  src_url="https://github.com/turmeric-lang/turmeric/archive/refs/tags/v${v}.tar.gz"
  turmeric_download "$src_url" "$tarball"
  mkdir -p "$tmp/src"
  tar -xzf "$tarball" -C "$tmp/src" --strip-components=1
  (
    cd "$tmp/src"
    cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_POLICY_VERSION_MINIMUM=3.5 >/dev/null
    cmake --build build -j >/dev/null
    mkdir -p "$ASDF_INSTALL_PATH/bin" "$ASDF_INSTALL_PATH/stdlib"
    cp build/tur "$ASDF_INSTALL_PATH/bin/tur"
    cp -R stdlib/. "$ASDF_INSTALL_PATH/stdlib/"
  )
fi

# Sanity check.
"$ASDF_INSTALL_PATH/bin/tur" --version >/dev/null
```

The install path is fully self-contained: download, verify, extract,
done. The source-build fallback handles niche targets and tags
predating the prebuilt era. No `tvm`, no shared cache, no symlink
games -- `ASDF_INSTALL_PATH` owns its contents.

### `bin/list-bin-paths`

```sh
#!/usr/bin/env bash
echo "bin"
```

### `bin/latest-stable`

```sh
#!/usr/bin/env bash
set -euo pipefail
plugin_dir="$(cd "$(dirname "$0")/.." && pwd)"
"$plugin_dir/bin/list-all" | grep -E '^[0-9]+\.[0-9]+\.[0-9]+$' | tail -n1
```

Strict `MAJOR.MINOR.PATCH` filter drops pre-releases (`-rc.1`, `-beta`).

### `bin/exec-env` (optional)

```sh
#!/usr/bin/env bash
export TUR_STDLIB_DIR="$ASDF_INSTALL_PATH/stdlib"
```

So `tur` invoked through a mise shim still finds its bundled stdlib even
if the user has no `TUR_STDLIB_DIR` set.

## Build dependencies

The prebuilt path has no host build deps -- just `curl`, `tar`, and a
SHA-256 tool (`sha256sum` or `shasum`), all standard on Linux/macOS.

The source-build fallback needs `cmake` (≥3.5) and a C compiler. Document
this in `bin/help.deps` so mise can surface it:

```sh
#!/usr/bin/env bash
cat <<'EOF'
Prebuilt install: curl, tar, sha256sum/shasum (standard on Linux & macOS).
Source-build fallback (used when no prebuilt asset exists for your host):
  - cmake >= 3.5
  - a C compiler (gcc or clang)
EOF
```

## Plan of work

1. **(turmeric)** Freeze the release-asset contract.
   - Confirm/standardize tarball naming
     (`turmeric-vX.Y.Z-<arch>-<os>.tar.gz`).
   - Publish a `.sha256` sidecar (or `SHA256SUMS`) per asset if not
     already.
   - Document these as "supported public artifacts" in the release docs
     so the plugin can rely on them.
2. **(asdf-turmeric)** Spin up the new repo with the scripts above.
3. **CI for the plugin**: GitHub Actions matrix (`ubuntu-latest`,
   `macos-latest`) that installs `mise`, installs the plugin from the
   checkout (`mise plugin install turmeric .`), then runs
   `mise install turmeric@<known-good-tag>` and
   `mise exec -- tur --version`. Run a second job that disables prebuilt
   download (set the asset URL to a 404) to exercise the source-build
   fallback.
4. **Docs**: a short "Using Turmeric with mise" section in `docs/guides/`
   linking to the plugin repo; one paragraph in `tvm/README.md` ("If you
   already use mise, see `asdf-turmeric` -- you don't need `tvm`").
5. **Optional, later**: submit the plugin to the
   [mise registry](https://mise.jdx.dev/registry.html) so users can run
   `mise install turmeric@latest` without the explicit `plugin install`
   step.

## Out of scope

- A native Rust mise plugin (the `mise plugin new` Rust target). The shell
  plugin is enough; Rust adds a build step to a piece of code that is
  mostly `curl | tar` glue.
- Replacing `tvm`. `tvm` stays the recommended path for users not on
  mise; the plugin is purely additive and the two are independent.
- Auto-publishing the plugin from the main Turmeric release workflow.
  Plugin churn is decoupled from compiler releases.
- Sharing an on-disk cache between `tvm` and the plugin. Each owns its
  own install root. Users who run both pay the disk twice for any
  version they install twice -- acceptable, and avoids the uninstall
  coupling a shared layout would introduce.

## Open questions

- **`SHA256SUMS` file vs. per-asset `.sha256`.** Per-asset is simpler
  for `bin/install` (one fetch per artifact); a combined `SHA256SUMS`
  is friendlier for humans verifying by hand. Recommend per-asset
  sidecars, plus a combined file if cheap.
- **GitHub API rate limits in `list-all`.** Unauthenticated calls are
  60/hr per IP; on a CI runner that's typically fine. If it becomes a
  problem, allow `GITHUB_TOKEN` via env (asdf plugins commonly do).
- **`.tur-version` vs. `.tool-versions`.** Two different files mean two
  sources of truth in the same repo. Document that mise users should
  pick one (`.mise.toml` recommended); do not try to make `tvm` read
  `.tool-versions` or vice versa.

## Effort estimate

- Release-asset contract freeze (naming + checksum sidecar + docs):
  ~0.5 day.
- Plugin scripts (`list-all`, `install` with prebuilt + source-build
  fallback, helpers, optional scripts) + README: ~1 day.
- Plugin CI (matrix + source-build fallback job): ~0.5 day.

Total: ~2 days of focused work, off the v1 critical path. Slightly more
than a tvm-passthrough plugin (the install script does real work
instead of shelling out), and the payoff is that `mise` users get a
self-contained installer with no second tool to bootstrap.
