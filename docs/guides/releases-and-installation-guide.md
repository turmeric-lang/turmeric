---
title: Releases and Installation
category: Getting Started
description: How to install Turmeric from a prebuilt release or Homebrew, what's in the tarball, and how a maintainer cuts a new release.
---

# Releases and Installation

This guide covers two audiences:

- **End users** who want to install `tur` from a tagged release or via
  Homebrew, and need to know what's in the tarball and which subcommands
  work outside the source repo.
- **Maintainers** who want to cut a new tagged release (or change how
  releases are built).

---

## Installing Turmeric

There are five supported paths today. If you expect to switch between
releases (bisecting a regression, matching a spice's required version),
the version manager (Option 1) automates the manual prebuilt-binary dance
of Option 2.

### Option 1: Version manager (`tvm`)

The bundled [Turmeric Version Manager](https://github.com/turmeric-lang/turmeric/blob/main/tvm/README.md) installs,
caches, and switches between releases per-shell -- the `nvm`/`rustup`
model. Bootstrap it once from a checkout:

```sh
sh tvm/install.sh        # installs into ~/.tvm and wires up your shell rc
```

Then, in a new shell:

```sh
tvm install 0.36.0       # download + cache a prebuilt release (verify: see below)
tvm use 0.36.0           # activate it for this shell
tvm alias default 0.36.0 # make it the default for new shells
tvm ls-remote            # list versions available to download
tvm run 0.17.0 --version # one-shot invoke without switching (great for bisects)
```

`tvm install` consumes the same GitHub Release tarballs described in
Option 2, checks them against the release's `sha256sums.txt`, and extracts each
version under `~/.tvm/versions/<v>/`.

> **The checksum check fails closed.** A missing, unreachable or empty
> `sha256sums.txt`, an asset with no row in it, and a system with no `sha256`
> tool are all refusals -- the download is deleted and the install stops.
> `--insecure` is the single opt-out, and it does **not** apply to a checksum
> *mismatch*: a check that ran and said no is not a check that could not run.
> An explicit `--from` source skips the check, since you have named the
> artifact yourself, and says so.
>
> The sums file is fetched from the same origin as the asset, so on its own it
> establishes integrity, not authenticity. The stronger check is the release's
> build provenance, which is signed through Sigstore and names the workflow run
> that produced the bytes:
>
>     # Releases built before the 2026-10-02 move to the turmeric-lang org
>     # (v0.59.0 and earlier) -- note --owner, not --repo:
>     gh attestation verify turmeric-<tag>-<target>.tar.gz --owner rjungemann
>     # Releases built after it:
>     gh attestation verify turmeric-<tag>-<target>.tar.gz --repo turmeric-lang/turmeric
>
> The owner is bound into the signature, so the right flag follows the
> release's **vintage**, not where the repo lives now. A pre-move asset is
> recorded under the account that owned the repo when it was built, and a
> transfer does not move that record -- so `--repo` fails for those assets
> under *either* owner name and `--owner rjungemann` is the form that works.
> A failure here means you used the wrong one, not that the download is
> compromised.
>
> When no prebuilt
asset exists for a tag (older than the prebuild matrix, or an
unpublished platform), `tvm install --build <v>` falls back to a CMake
source build. See [`tvm/README.md`](https://github.com/turmeric-lang/turmeric/blob/main/tvm/README.md) for the full
command set, including `.tur-version` auto-switching.

### Option 2: Prebuilt binary from GitHub Releases

For every tag matching `v*` pushed to the repository, a
[GitHub Release](https://github.com/turmeric-lang/turmeric/releases) is
published with four archives and a `sha256sums.txt`:

```
turmeric-vX.Y.Z-linux-x86_64.tar.gz
turmeric-vX.Y.Z-linux-aarch64.tar.gz
turmeric-vX.Y.Z-macos-arm64.tar.gz
turmeric-vX.Y.Z-windows-x86_64.zip
sha256sums.txt
```

All four unpack to the same **prefix layout**:

```
bin/tur            (bin/tur.exe on Windows)
lib/libturt_runtime.a, libturi.a
include/turi/*.h
share/turmeric/stdlib/
```

Keep that shape. `tur` finds its runtime archive at `<exe_dir>/../lib` and its
standard library at `<exe_dir>/../share/turmeric/stdlib`, so the tree is one an
installed toolchain already has. Put `<extracted>/bin` on `PATH`, or symlink
`bin/tur` -- the walk-up resolves through a symlink either way.

Windows ships a `.zip` rather than a `.tar.gz` because Explorer opens one and
not the other, and `tur.exe` is statically linked against the MinGW support
libraries, so the archive has no DLLs beside it and needs nothing on `PATH` to
start. That is the only remaining per-platform difference.

**Archives up to v0.46.0 were different.** The three `.tar.gz` targets shipped
flat -- `tur`, the `.a` files and `stdlib/` all at the archive root -- while
Windows used the prefix layout from the start. Those older archives still work:
`locate_runtime_lib` probes `<exe_dir>` as well, deliberately, so an archive
published before the unification still compiles with a `tur` built after it.
Adjust the paths below if you are unpacking one.

**Windows also needs a C toolchain to compile anything.** `tur.exe` runs on its
own, but `tur build` and `tur run` invoke `cc`, and `tur jit` reads the UCRT
headers that come with it. Install [MSYS2](https://www.msys2.org/) and its
UCRT64 gcc:

```sh
pacman -S mingw-w64-ucrt-x86_64-gcc
```

then run `tur` from a UCRT64 shell, or put `C:\msys64\ucrt64\bin` on `PATH`.
Without it `tur run` reports `cc invocation failed`. (`TUR_JIT_SYS_INCLUDE`
overrides where the JIT looks for those headers, if yours live elsewhere.)

Pick the tarball for your platform, verify, and extract:

```sh
# Apple Silicon macOS example. Adjust the URL for your platform/version.
TAG=v0.36.0
ARCH=macos-arm64
curl -fLO "https://github.com/turmeric-lang/turmeric/releases/download/${TAG}/turmeric-${TAG}-${ARCH}.tar.gz"
curl -fLO "https://github.com/turmeric-lang/turmeric/releases/download/${TAG}/sha256sums.txt"

# Verify just the file you downloaded:
shasum -a 256 -c <(grep "${ARCH}" sha256sums.txt)

# Extract somewhere stable:
mkdir -p ~/.local/turmeric
tar -xzf "turmeric-${TAG}-${ARCH}.tar.gz" -C ~/.local/turmeric

# Make `tur` runnable:
ln -s ~/.local/turmeric/bin/tur ~/.local/bin/tur  # ensure ~/.local/bin is on PATH
```

The macOS binary is unsigned. On first run macOS will quarantine it:

```sh
xattr -d com.apple.quarantine ~/.local/turmeric/bin/tur
```

There is no precompiled Intel-Mac (`macos-x86_64`) tarball. Intel-Mac
users should use Option 3 or Option 4.

### Option 3: Homebrew (source build)

```sh
brew install --HEAD rjungemann/turmeric
# (if a tap isn't published yet:)
brew install --HEAD https://raw.githubusercontent.com/turmeric-lang/turmeric/main/Formula/turmeric.rb
```

The formula builds from the latest commit on `main` (CMake source build,
~10s on modern hardware). It installs:

- `tur` at `<prefix>/bin/tur`
- The standard library at `<prefix>/share/turmeric/stdlib/`

On Apple Silicon, `<prefix>` is `/opt/homebrew`; on Intel macOS and
Linuxbrew it's `/usr/local` or `/home/linuxbrew/.linuxbrew`.

There is no stable (versioned) Homebrew formula -- only `--HEAD`, which
compiles whatever `main` is at that moment and verifies no checksum. That makes
it a way to track development, not a way to install the compiler.

**The advertised `curl ... | sh` installer no longer uses it.** It installs
`tvm` and then the latest release, checksum-verified as described above, and
works on Linux as well as macOS. Use `brew install --HEAD` only when you
specifically want to build `main`. See the
[Security Guide](security-guide.md#t4-the-supply-chain).

### Option 4: Building from source

```sh
git clone https://github.com/turmeric-lang/turmeric.git
cd turmeric
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DCMAKE_POLICY_VERSION_MINIMUM=3.5
cmake --build build -j                    # debug build, lands at build/tur

cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release -DCMAKE_POLICY_VERSION_MINIMUM=3.5
cmake --build build-release -j            # optimized build, drops `tur` in build-release/
```

You'll need CMake 3.20+, a C99 compiler, and `libedit` (for the REPL).
See [`devcontainer-guide.md`](devcontainer-guide.md) for a fully-scripted
Linux dev environment.

---

## What's in the tarball

After extracting, the tarball lays out like:

```ascii
.
|-- bin/tur                   # the CLI
|-- lib/libturt_runtime.a     # runtime archive `tur build` links against
|-- lib/libturi.a             # static library for C embedding
|-- include/turi/             # public headers (eval.h, env.h, value.h, fiber.h)
`-- share/turmeric/stdlib/    # the standard library (~137 .tur files)
```

`tur` finds `stdlib/` via a probe defined in `src/main.c`
(`resolve_stdlib_root`), in this order:

1. The `TUR_STDLIB_DIR` environment variable, if set.
2. Walk up from `tur`'s directory looking for `stdlib/macros.tur`
   (matches when `stdlib/` sits next to the binary -- the pre-v0.47 tarball
   layout, and a dev build tree).
3. `<exe_dir>/../share/turmeric/stdlib/macros.tur` (the current tarball layout,
   and Homebrew's).

Both are probed at every level of the walk-up, which is why an archive from
either era resolves without a code change or an env var.

If you move `tur` somewhere with neither, set `TUR_STDLIB_DIR` to the directory
containing the stdlib `.tur` files.

---

## What works (and what doesn't) outside the source repo

### Works

- `tur --version`
- `tur interpret <file.tur>` -- tree-walking interpreter; no C compile
- `tur repl` -- interactive REPL
- `tur check <file.tur>` -- type checking only
- `tur doc <symbol>` -- documentation lookup
- `tur explain <code>` -- diagnostic-code lookup
- `tur format` -- source formatter
- Library embedding via `libturi.a` + `include/turi/`

### Does *not* yet work outside the repo tree

- `tur run <file.tur>` and `tur build <file.tur>`

These go through the C-codegen path and link against runtime sources
referenced by autolink markers in stdlib (e.g. `stdlib/hamt.tur` contains
`/* __tur_autolink__: src/runtime/hamt.c -Isrc/runtime */`). The compiler
anchors those relative paths at the located turmeric root (the parent of
the resolved `stdlib/`), but the release tarball ships no `src/runtime/`
next to its `stdlib/`, so a `tur run` from a downloaded release fails
with:

```
clang: error: no such file or directory: 'src/runtime/hamt.c'
tur: cc invocation failed (status 256)
```

This is a known limitation, tracked in
[`docs/release-binaries-plan.md`](https://github.com/turmeric-lang/turmeric/blob/main/docs/archive/release-binaries-plan.md) under
"Discovered during execution: runtime sources also missing". Until it's
resolved, the `--interpret` and library-embedding paths are the
fully-supported uses of a downloaded release.

### Option 5: Docker

The repository ships a [`Dockerfile`](https://github.com/turmeric-lang/turmeric/blob/main/Dockerfile) that builds `tur` from
the local source tree and packages it into a self-contained Ubuntu 22.04 image.
This is the easiest path on Linux if you do not want to install CMake or deal
with libedit versions.

**Build the image** (run from the repository root):

```sh
docker build -t turmeric .
```

The multi-stage build compiles a Release binary and copies only the binary,
stdlib, and C runtime sources into the final image (~200 MB).

**REPL:**

```sh
docker run --rm -it turmeric
```

**Interpret a file** (no C compiler involved):

```sh
docker run --rm -v "$(pwd)":/workspace turmeric \
    tur interpret /workspace/hello.tur
```

**Compile and run a file:**

```sh
docker run --rm -v "$(pwd)":/workspace turmeric \
    tur run /workspace/hello.tur
```

**Type-check a file:**

```sh
docker run --rm -v "$(pwd)":/workspace turmeric \
    tur check /workspace/hello.tur
```

The image sets `TUR_STDLIB_DIR` and wraps the `tur` binary in a small shell
script so that `tur run`/`tur build` can resolve the C runtime sources (which
are referenced by relative paths at compile time). Mount your project with
`-v "$(pwd)":/workspace` and pass the absolute container path to your file.

---

## Cutting a release (maintainers)

The release pipeline lives at `.github/workflows/release.yml` and is
triggered automatically on `git push` of any tag matching `v*`.

The `/cut-minor-release` and `/cut-major-release` skills drive the full
flow with preconditions and confirmations. Experiment expiry is
**advisory and never blocks a release**: the skills surface any registry
entry whose `expires_at` is at or before the version being cut and then
proceed; the author follows up by graduating the experiment (delete its
row in `src/runtime/experiments.c`; the feature becomes always-on),
shelving it, or bumping `expires_at` with a one-line rationale. See
[experimental-flags-guide.md](experimental-flags-guide.md#expiry-policy).

### Steps

1. Bump the version in `VERSION` (the single source of truth -- read by
   CMake, baked into `tur --version`, and reused by `web/` and other
   build outputs). The version string is plain `MAJOR.MINOR.PATCH`,
   no leading `v`.
2. Commit the bump.
3. Tag the bump commit:
   ```sh
   git tag v0.36.0    # match the new VERSION
   git push origin main
   git push origin v0.36.0
   ```
4. The workflow runs (~1-2 minutes per matrix leg, ~3 minutes total),
   builds three binaries, and publishes a GitHub Release.
5. Verify the release page lists three tarballs plus `sha256sums.txt`.

### What the workflow does

For each matrix leg (`linux-x86_64`, `linux-aarch64`, `macos-arm64`):

1. Checks out the tagged commit.
2. Installs `libedit` (Apple/Linux differ on package manager).
3. Configures and builds with CMake in Release mode.
4. Runs `tur --version` as a smoke test.
5. Packages `bin/tur` + `lib/*.a` + `include/turi/*.h` +
   `share/turmeric/stdlib/` into a `tar.gz`.
6. Unpacks that archive and compiles a two-line program with it. `tur
   --version` above proves only that the binary starts, which is a weaker
   claim than it looks: the archives passed that check for several releases
   while being unable to compile anything. This is the step that would have
   caught it, and it is what verifies whichever layout is packaged.
7. Uploads the artifact.

Windows is a separate job rather than a fourth matrix leg: every step needs
`shell: msys2 {0}` and the toolchain arrives through `setup-msys2`, neither of
which fits the matrix. It does the same steps, plus one that has no
equivalent elsewhere -- **verifying the binary is self-contained**. A default
MinGW build needs `libwinpthread-1.dll` and `edit.dll` out of the MSYS2 tree,
and off an MSYS2 `PATH` such a binary exits 127 with no output and no
diagnostic. So the job both reads the import table (`ldd`, rejecting anything
resolved inside the MSYS2 tree) and actually runs `tur --version` with MSYS2
stripped from `PATH`, which is the failure a user would hit.

A final job downloads all artifacts, generates `sha256sums.txt`, and
publishes the release with auto-generated notes.

The `release` job has `if: always() && needs.build.result != 'cancelled'`,
so if one matrix leg fails (e.g. a future runner image breaks Linux
aarch64), the other binaries still ship -- you'll get a partial release
that you can re-run or supplement.

### Iterating on the workflow itself

To test changes to `release.yml` without burning real version numbers,
use a throwaway tag like `v0.0.0-test1`:

```sh
git tag v0.0.0-test1
git push origin v0.0.0-test1

# Watch the run:
gh run list --workflow=release.yml --limit 1
gh run watch <run-id>

# Clean up afterwards:
gh release delete v0.0.0-test1 --cleanup-tag --yes
git tag -d v0.0.0-test1
```

Increment the suffix (`-test2`, `-test3`, ...) per iteration so each
failed attempt's history is preserved.

### Troubleshooting

- **Job stuck "awaiting a runner" for hours.** The runner image the leg
  pins to is no longer scheduled. Update `matrix.<leg>.os` to a current
  GitHub-hosted runner.
- **macOS configure fails with `command not found`.** A semicolon in a
  CMake flag is being interpreted by the shell. Wrap the value with
  literal double-quotes in the matrix value
  (`cmake_extra: '"-Dfoo=a;b"'`) so the shell treats it as one argument
  after `${{ matrix.cmake_extra }}` substitution.
- **`actions/checkout` or `actions/upload-artifact` Node-20 warning.**
  Informational until June 2026. Bump the action to the latest major
  when an upstream Node-24 release ships, or opt in early with the
  `FORCE_JAVASCRIPT_ACTIONS_TO_NODE24=true` env var on the job.
- **`brew test turmeric` fails with `unbound variable: when`.** The
  formula did not install `stdlib/` under `<prefix>/share/turmeric/`.
  Confirm the formula's `install` block contains
  `(share/"turmeric").install "stdlib"`.

---

## See also

- `Formula/turmeric.rb` -- the Homebrew formula.
- `.github/workflows/release.yml` -- the release pipeline.
- `src/main.c` (`resolve_stdlib_root`) -- the stdlib-discovery logic
  that makes both the tarball and Homebrew layouts work without code
  changes.
