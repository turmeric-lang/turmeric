---
title: Using Turmeric with mise or asdf
category: Package Management
description: Install and switch Turmeric compiler versions with the asdf-turmeric plugin (works for both mise and asdf)
---

# Using Turmeric with `mise` or `asdf`

The [`asdf-turmeric`](https://github.com/turmeric-lang/asdf-turmeric) plugin
installs Turmeric compiler versions from the official GitHub releases and
plugs into both [`mise`](https://mise.jdx.dev) and
[`asdf`](https://asdf-vm.com). You do **not** need
[`tvm`](https://github.com/turmeric-lang/turmeric/blob/main/tvm/README.md) to use it -- the plugin is a self-contained
installer.

Pick whichever tool you already use; the plugin works the same either way.

## Why this exists alongside `tvm`

`tvm` is the recommended path for users who don't already run a version
manager. If you are already invested in `mise` or `asdf` for other
languages, this plugin lets you manage Turmeric the same way -- shared
shims, shared `.tool-versions`/`.mise.toml`, no second tool to bootstrap.

The plugin and `tvm` are independent installers that consume the same
GitHub release artifacts. Use whichever fits your setup; you don't need
both.

## With `mise`

### Install `mise`

If you don't have `mise` yet, follow
<https://mise.jdx.dev/getting-started.html>. On macOS:

```sh
brew install mise
echo 'eval "$(mise activate zsh)"' >> ~/.zshrc      # or .bashrc
exec $SHELL
```

### Add the plugin and install Turmeric

```sh
mise plugin install turmeric https://github.com/turmeric-lang/asdf-turmeric.git
mise install turmeric@latest
```

### Pin a version

Globally:

```sh
mise use --global turmeric@0.23.1
```

Per-project (writes `.mise.toml` in the current directory):

```sh
mise use turmeric@0.23.1
```

`tur` on `PATH` then resolves to the pinned version. The plugin exports
`TUR_STDLIB_DIR` automatically, so the bundled stdlib is found without
any extra setup.

### One-off invocations

```sh
mise exec turmeric@0.23.1 -- tur --version
```

## With `asdf`

### Install `asdf`

If you don't have `asdf` yet, follow
<https://asdf-vm.com/guide/getting-started.html>. On macOS:

```sh
brew install asdf
echo '. /opt/homebrew/opt/asdf/libexec/asdf.sh' >> ~/.zshrc      # or .bashrc
exec $SHELL
```

### Add the plugin and install Turmeric

```sh
asdf plugin add turmeric https://github.com/turmeric-lang/asdf-turmeric.git
asdf install turmeric latest
```

### Pin a version

Globally:

```sh
asdf global turmeric 0.23.1
```

Per-project (writes `.tool-versions` in the current directory):

```sh
asdf local turmeric 0.23.1
```

## Source-build fallback

When no prebuilt asset exists for the host (currently macOS x86_64, or
older tags from before the prebuilt era), the plugin downloads the source
tarball and builds with `cmake` instead. This needs:

- `cmake` >= 3.5
- a C compiler (`gcc` or `clang`)
- `libedit` (for the REPL)

Pass `ASDF_TURMERIC_FORCE_SOURCE=1` to force the source-build path even
when a prebuilt asset is available.

## `.tool-versions` vs `.tur-version`

`tvm` reads `.tur-version`; `mise`/`asdf` read `.mise.toml` /
`.tool-versions`. Pick **one** source of truth per repo. If you are
using `mise` or `asdf`, `.mise.toml` (or `.tool-versions`) is the
canonical pin; do not also commit a `.tur-version`.

## Troubleshooting

- **GitHub API rate-limit on `list-all`.** Unauthenticated calls are
  capped at 60/hr per IP. Set `GITHUB_TOKEN` in the environment and the
  plugin will pass it on the API + asset requests.
- **`no prebuilt asset for <target>; building from source` on install.**
  Expected on hosts the release workflow doesn't target (e.g. macOS
  x86_64). The build needs `cmake` >= 3.5 and a C compiler installed.
- **`sha256 mismatch` during install.** The plugin downloaded a
  corrupted tarball. Retry the install; if it persists, the release
  artifacts may have been overwritten -- file an issue on
  [`turmeric`](https://github.com/turmeric-lang/turmeric/issues).
