# tvm -- Turmeric Version Manager

`tvm` downloads, caches, and switches between Turmeric compiler releases the
same way [`nvm`](https://github.com/nvm-sh/nvm) does for Node. It is a
per-user, shell-installable tool: no root, no system package manager.

It is intentionally close to `nvm` in spirit -- if you know
`nvm install <v>` / `nvm use <v>`, you already know `tvm`.

See [`docs/archive/tur-version-manager-plan.md`](../docs/archive/tur-version-manager-plan.md)
for the design rationale.

> **Already on `mise` or `asdf`?** You don't need `tvm`. The
> [`asdf-turmeric`](https://github.com/turmeric-lang/asdf-turmeric) plugin
> consumes the same release artifacts and integrates with both tools --
> see [docs/guides/mise-asdf-guide.md](../docs/guides/mise-asdf-guide.md).

## Install

From a checkout of this repo:

```sh
sh tvm/install.sh
```

This copies `tvm.sh` into `$TVM_DIR` (default `~/.tvm`) and appends an init
snippet to your shell rc (`~/.zshrc`, `~/.bashrc`, or `~/.profile`). Open a
new shell, or source it directly:

```sh
export TVM_DIR="$HOME/.tvm"
. "$TVM_DIR/tvm.sh"
```

## Usage

```sh
tvm install 0.23.1          # download + cache the v0.23.1 prebuilt binary
tvm use 0.23.1              # activate it for this shell
tvm use system             # fall back to whatever tur is on PATH
tvm alias default 0.23.1   # persist a default for new shells
tvm install --build 0.17.0 # no prebuilt asset? build from source against the tag
tvm install --activate 0.23.1  # install + use + set as default in one step
tvm ls                     # list installed versions and aliases
tvm ls-remote              # list versions available to download
tvm current                # print the active version
tvm which 0.23.1           # absolute path to that version's tur
tvm run 0.23.1 build .     # one-shot: invoke a specific version
tvm exec 0.23.1 -- sh -c '...'  # run a command with that version on PATH
tvm uninstall 0.22.0
tvm doctor                 # diagnose the setup
tvm completion zsh > _tvm  # shell completion
```

### Per-shell vs. persistent

- `tvm use <v>` changes the active compiler for **this shell only**, by
  prepending `$TVM_DIR/versions/<v>/bin` to `PATH`.
- `tvm alias default <v>` sets what **new** shells start with (applied when
  `tvm.sh` is sourced).

### `.tur-version` auto-switching

Drop a `.tur-version` file (one line, a version tag) at a project root and
enable auto mode:

```sh
tvm auto on
```

`cd`-ing into that directory (or any subdirectory) then switches to the
requested version automatically -- the `.nvmrc` model.

## How it works

`tvm` consumes the artifacts published by
[`.github/workflows/release.yml`](../.github/workflows/release.yml). Each
release ships one tarball per `(os, arch)`:

```
turmeric-v<tag>-<target>.tar.gz   # target: linux-x86_64 | linux-aarch64 | macos-arm64
  bin/tur                          # the compiler binary
  lib/libturt_runtime.a            # runtime archive `tur build` links against
  lib/libturi.a                    # static interpreter library
  include/turi/*.h                 # headers to link against libturi
  share/turmeric/stdlib/           # stdlib snapshot for that release
```

`tvm install` downloads the asset matching the current host, verifies its
SHA-256 against the release's `sha256sums.txt`, and extracts it atomically
into `$TVM_DIR/versions/<v>/`. `tvm use` activates a version's stdlib by
exporting `TUR_STDLIB_DIR` so resource lookup is unambiguous.

Releases up to v0.46.0 shipped the three `.tar.gz` targets **flat** instead --
`tur`, the `.a` files and `stdlib/` all at the archive root -- so `tvm install`
normalizes either shape into the prefix layout above on the way in, and reads
either shape back off disk for versions installed before that. The
rearrangement is not cosmetic: `tur` finds its runtime archive by probing
`<exe_dir>/../lib`, so a version with `bin/tur` and `libturt_runtime.a` left at
the root is one that cannot compile anything.

### Directory layout (`$TVM_DIR`, default `~/.tvm`)

```
~/.tvm/
  tvm.sh                              # shell entry point (sourced from your rc)
  versions/<v>/bin/tur                # extracted compilers
  versions/<v>/lib/*.a                # runtime + interpreter archives
  versions/<v>/share/turmeric/stdlib/ # that version's stdlib
  aliases/default                     # plain file: "0.23.1"
  cache/downloads/                    # raw tarballs
  cache/sources/                      # source checkout reused by --build
```

## Tests

Run the offline suite (uses a fake local release; no network or compiler):

```sh
sh tvm/tests/run.sh
```
