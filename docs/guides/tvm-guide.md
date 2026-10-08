---
title: Turmeric Version Manager (`tvm`)
category: CLI Tools
description: Install, switch between, and pin Turmeric compiler releases the same way nvm does for Node -- per-user, no root, no system package manager.
---

# Turmeric Version Manager (`tvm`)

`tvm` downloads, caches, and switches between Turmeric compiler releases.
It is intentionally close to [`nvm`](https://github.com/nvm-sh/nvm) in
spirit: if you know `nvm install <v>` / `nvm use <v>`, you already know
`tvm`. Per-user, no root, no system package manager.

---

## Install

From a checkout of the [turmeric repo](https://github.com/turmeric-lang/turmeric):

```sh
sh tvm/install.sh
```

The installer copies `tvm.sh` into `$TVM_DIR` (default `~/.tvm`),
creates the per-version cache layout under it, and appends a two-line
init snippet to your shell rc (`~/.zshrc`, `~/.bashrc`, or
`~/.profile`):

```sh
export TVM_DIR="$HOME/.tvm"
[ -s "$TVM_DIR/tvm.sh" ] && . "$TVM_DIR/tvm.sh"
```

Open a new shell, or source the snippet directly. After that, `tvm` is
available as a shell function.

Override the install root by exporting `TVM_DIR` before running the
installer:

```sh
TVM_DIR="$HOME/.turmeric-vm" sh tvm/install.sh
```

---

## Quick reference

```sh
tvm install 0.24.0              # download + cache the v0.24.0 prebuilt
tvm install --activate 0.24.0   # install + use + set as default, one step
tvm install --build 0.17.0      # build from source against a tag (no prebuilt)
tvm use 0.24.0                  # activate for this shell
tvm use system                  # fall back to whatever `tur` is on PATH
tvm deactivate                  # drop back to the system tur
tvm alias default 0.24.0        # what new shells start with
tvm auto on                     # cd-driven .tur-version auto-switch

tvm current                     # active version
tvm ls                          # installed versions + aliases
tvm ls-remote                   # versions available to download
tvm which 0.24.0                # absolute path to that version's tur
tvm doctor                      # diagnose the setup

tvm run 0.24.0 build .          # one-shot: use a specific version
tvm exec 0.24.0 -- sh -c '...'  # run arbitrary command with that version on PATH
tvm uninstall 0.22.0

tvm completion zsh > _tvm       # shell completion
```

`tvm --help` prints the same surface.

---

## Switching: per-shell vs. persistent

`tvm use <v>` is **per-shell**. It mutates `PATH` and `TUR_STDLIB_DIR`
inside the current shell only -- new terminals are unaffected.

`tvm alias default <v>` is **persistent**. The alias is written to
`$TVM_DIR/aliases/default` and applied when `tvm.sh` is sourced in a new
shell. Setting `default` is the closest thing to "install Turmeric
system-wide" that `tvm` offers.

`tvm install --activate <v>` is the shortcut for "install, `use`, and
set as `default`" in a single step -- the right move when you are
setting up a new machine or bumping the version everyone on the team
should be on.

```sh
tvm install --activate 0.24.0   # this version, here and in new shells
```

---

## `.tur-version` auto-switching

Drop a one-line `.tur-version` file at a project root and turn on auto
mode:

```sh
# inside a project
echo 0.23.3 > .tur-version

# once per session (persisted via shell rc reload)
tvm auto on
```

`cd`-ing into that directory (or any subdirectory) switches the active
compiler to the requested version automatically -- the `.nvmrc` model.
`cd`-ing back out switches to the previous version (or to your default
alias).

`tvm auto` with no argument prints the current state (`on` or `off`).
`tvm auto off` disables it.

The hook is installed into `chpwd_functions` on zsh and into
`PROMPT_COMMAND` on bash. If you use a non-stock shell setup that
overrides those, expect to wire the hook yourself -- `tvm doctor`
will warn when the hook is not attached.

---

## Building from source

When a release tag has no prebuilt asset for your `(os, arch)` -- old
versions, niche targets -- pass `--build`:

```sh
tvm install --build 0.17.0
```

`tvm` clones (or reuses) a source checkout under
`$TVM_DIR/cache/sources/`, checks out the tag, runs `cmake ... -DCMAKE_BUILD_TYPE=Release`,
and stages the resulting binary into `$TVM_DIR/versions/0.17.0/bin/tur`
atomically. The bootstrap requires `cmake` and a C compiler on `PATH`
(same prerequisites as building Turmeric from source by hand).

`--build` also works with `--activate`:

```sh
tvm install --build --activate 0.17.0
```

---

## Inspect what's installed

```sh
$ tvm current
0.24.0

$ tvm ls
* 0.24.0
  0.23.3
  0.22.0
  aliases:
    default -> 0.24.0

$ tvm ls-remote
  0.24.0   2026-06-23   prebuilt
  0.23.3   2026-06-23   prebuilt
  0.23.2   2026-06-23   prebuilt
  ...

$ tvm which 0.24.0
/Users/you/.tvm/versions/0.24.0/bin/tur
```

`ls-remote` reads
[the release manifest from GitHub Releases](https://github.com/turmeric-lang/turmeric/releases)
and marks each entry `prebuilt` (host-compatible asset exists) or
`source` (must be installed with `--build`).

---

## One-shot invocations

When you want to use a non-active version for exactly one command,
don't `tvm use`. Use `run` or `exec`:

```sh
# one-shot tur invocation: tvm picks the binary, you supply args
tvm run 0.23.2 build my-project/

# arbitrary command with that version on PATH (linters, scripts, CI):
tvm exec 0.23.2 -- bash ./scripts/regen-fixtures.sh
```

`run`'s arguments after the version are passed verbatim to
`<TVM_DIR>/versions/<v>/bin/tur`. `exec` sets `PATH` and
`TUR_STDLIB_DIR` and then `exec`s the command after `--`.

Useful for CI matrices, bisecting which version introduced a behavior
change, or for running `just regen-fixtures` against an old compiler.

---

## Diagnosing problems: `tvm doctor`

`tvm doctor` prints a checklist of common misconfigurations:

```sh
$ tvm doctor
tvm: TVM_DIR=/Users/you/.tvm (exists)
tvm: tvm.sh sourced from /Users/you/.tvm/tvm.sh
tvm: active version: 0.24.0 (from `tvm use`)
tvm: default alias: 0.24.0
tvm: auto-switch: on
tvm: chpwd hook attached
tvm: 3 versions installed
tvm: cache size: 47M
```

Run it any time `tvm` is not behaving the way you expect, before
reporting an issue. Common failures it flags:

- `TVM_DIR` set but the directory does not exist.
- `tvm.sh` not sourced in this shell -- `tvm` works as a binary in `$PATH`
  only via the shell function; you need the init snippet in your rc.
- A version directory exists but its `bin/tur` is missing or
  unexecutable.
- An alias points at a version that is not installed.
- `auto on` is set but the shell hook (`chpwd_functions` on zsh,
  `PROMPT_COMMAND` on bash) is not attached -- happens when your shell
  config clobbers those after `tvm.sh` is sourced.

---

## When `tur` says the stdlib does not match

`TUR_STDLIB_DIR` is an ordinary environment variable, so it is inherited by
everything a shell spawns and it outlives the install that set it. `tvm use`
sets it deliberately; the trouble is a *stale* value pointing a current `tur` at
an older release's stdlib. That compiles, and fails much later as something like

```
error: no member named 'is_ok' in 'tur_result_box_t'
```

which names neither the stdlib nor the variable, and reads as a compiler bug. It
cost a full investigation once before the check below existed.

Every stdlib now carries a `VERSION` stamp beside its sources, and `tur` compares
it against its own version at startup. Three things it can say:

```
tur: TUR_STDLIB_DIR=<dir> is turmeric 0.36.0, but this compiler is v0.42.2.
```
A confirmed mismatch. Unset the variable, or `tvm use` the version you meant.

```
tur: TUR_STDLIB_DIR=<dir> overrides the stdlib beside this binary (<dir>), and
     carries no VERSION stamp to check it against v0.42.2.
```
The stdlib predates the stamp, so it is from an older release. Same fix.

```
tur: the stdlib at <dir> is turmeric 0.36.0, but this compiler is v0.42.2 --
     they must match.
```
No variable involved: the stdlib shipped beside the binary disagrees with it,
which means a broken or half-updated install rather than a shell problem.

A stdlib whose stamp **matches** is silent, including when it sits somewhere
other than beside the binary -- pointing a compiler at another tree deliberately
is a legitimate thing to do, and it no longer warns for it.

Worth knowing about the acquisition path, because it does not look like an
environment problem: a version manager whose `python3` is a shim can re-export
its tool environment into the process it launches, so a shell with no
`TUR_STDLIB_DIR` can spawn one that has it.

---

## Shell completion

```sh
tvm completion zsh > ~/.zsh/completions/_tvm
tvm completion bash > /usr/local/etc/bash_completion.d/tvm
```

Completes subcommands, installed versions for `use`/`which`/`uninstall`,
and `ls-remote` entries for `install`.

---

## How it works

`tvm` consumes the artifacts published by
[`.github/workflows/release.yml`](https://github.com/turmeric-lang/turmeric/blob/main/.github/workflows/release.yml).
Each release ships one tarball per `(os, arch)`:

```
turmeric-v<tag>-<target>.tar.gz   # target: linux-x86_64 | linux-aarch64 | macos-arm64
  tur                              # the compiler binary
  libturi.a                        # static interpreter library
  include/turi/*.h                 # headers for linking against libturi
  stdlib/                          # stdlib snapshot for that release
```

`tvm install` downloads the asset matching the current host, verifies
its SHA-256 against the release's `sha256sums.txt`, and extracts it
atomically into `$TVM_DIR/versions/<v>/`. `tvm use` then mutates `PATH`
and exports `TUR_STDLIB_DIR=$TVM_DIR/versions/<v>/stdlib` so the
compiler's resource lookup is unambiguous, even for versions installed
side-by-side.

### Directory layout

```
$TVM_DIR/                  # default: ~/.tvm
  tvm.sh                   # shell entry point (sourced from your rc)
  versions/<v>/
    bin/tur                # extracted compiler
    lib/libturi.a          # interpreter library
    include/turi/*.h
    stdlib/                # per-version stdlib snapshot
  aliases/
    default                # plain-text file: "0.24.0"
  cache/
    downloads/             # raw tarballs (kept; reuses on reinstall)
    sources/               # source checkout reused by --build
```

Nothing outside `$TVM_DIR` is touched, and `tvm uninstall <v>` removes
exactly `versions/<v>/` -- no leftover state.

---

## See also

- `tvm/README.md` -- short cheat sheet next to the source.
- `tvm doctor` -- the first stop for any "why isn't this working"
  question.
- [Releases page](https://github.com/turmeric-lang/turmeric/releases) --
  what `ls-remote` is reading from.
