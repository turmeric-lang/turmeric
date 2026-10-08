# Plan: Global Spice Install -- `tur install` and Spice Binaries on `$PATH`

> **Status:** Draft Plan
> **Last Updated:** 2026-05-26
> **Type:** Tooling / CLI / Package Management

---

## Overview

Today every spice in Turmeric is **project-scoped**: `tur add` records it in a
`build.tur`, `tur fetch` drops sources under `./spices/<name>-<ref>/`, and the
spice is only usable from that consuming project. There is no mechanism for
shipping a spice as a *standalone command-line tool* a user can invoke from
any directory.

This plan introduces:

1. **Global (per-user) spices** -- a parallel install root at
   `~/.local/share/turmeric/` (XDG-compliant) holding spices that any
   project on the machine can see, with binary shims dropped into
   `~/.local/bin/` so they pick up the user's existing `$PATH`.
2. **`:bin` declarations** -- a new `build.tur` field that names the
   executables a spice exposes and points each at its entrypoint module.
3. **`tur install`** -- a verb that fetches, builds, and links binaries
   into `~/.local/bin/`. Pairs with `tur uninstall` and `tur upgrade`.
4. **`tur list`** -- a shallow inventory of every globally-installed spice
   and what it provides (binaries, exported modules), so users can answer
   "what's installed?" without grepping a directory.
5. **`tur <name>` subcommand fallthrough** -- when `tur foo bar` is run
   and `foo` is not a built-in subcommand, exec `tur-foo bar` from `$PATH`.
   This makes `tur nb render foo.tur.md` Just Work after
   `tur install tur-notebook`.

Together these let a freshly-installed `tur` user run:

```sh
tur install https://github.com/turmeric-lang/turmeric-spices \
  --ref notebook-v0.1.0 --subdir spices/notebook --name notebook
tur nb new hello.tur.md
tur nb tui hello.tur.md
```

with no project scaffolding. The `tur nb ...` form works because of the
subcommand fallthrough described in
[M2](#m2----tur-install--subcommand-fallthrough-the-v1-ux); the
`tur-nb` binary that `tur install` drops into `~/.local/bin/` can also
be invoked directly.

---

## Design Decisions

### Global root: `~/.local/share/turmeric/`

Follow the XDG Base Directory spec. Spice sources, state, and caches live
under `$XDG_DATA_HOME/turmeric/` (default `~/.local/share/turmeric/`).
Binaries land in `~/.local/bin/`, the conventional XDG user-bin directory
that most modern distros already include in `$PATH`.

Rationale:

- XDG keeps Turmeric out of `$HOME`'s top level alongside dozens of other
  tools' dotdirs.
- `~/.local/bin/` is widely pre-mounted on `$PATH` (systemd's
  `user-dirs.defaults`, Debian's `~/.profile`, Fedora, recent macOS via
  Homebrew, etc.), so most users skip the PATH-onboarding step entirely.
- Reusing the shared user-bin dir means `tur install` plays nicely with
  `pipx`, `cargo install --root ~/.local`, and similar -- one PATH entry
  covers all of them.
- Respect `$XDG_DATA_HOME` if set (override path becomes
  `$XDG_DATA_HOME/turmeric/`). Independently, `TUR_HOME=/some/path`
  overrides the data root for users who want a fully isolated sandbox
  (matches `RUSTUP_HOME`, `GOPATH`, `BUN_INSTALL`).

Layout:

```
~/.local/share/turmeric/
  spices/
    notebook-0.1.0/        # one dir per (name, version) tuple
      build.tur
      src/...
      build/tur-nb         # the compiled artifact, linked from ~/.local/bin/
    notebook-0.2.0-rc1/
    plot-0.3.1/
  cache/                   # git fetch cache, shared across projects
  state.tur                # installed-spice registry (see "tur list")

~/.local/bin/
  tur-nb                   # symlink -> ~/.local/share/turmeric/spices/notebook-0.1.0/build/tur-nb
  tur-plot                 # symlink -> ~/.local/share/turmeric/spices/plot-0.3.1/build/tur-plot
```

The spices dir is **not** in any module-resolution path by default.
Global spices are imported by name only when explicitly requested; see
[Imports from global spices](#imports-from-global-spices) below.

To avoid colliding with non-Turmeric tools in `~/.local/bin/`, every
binary that `tur install` writes is namespaced with the `tur-` prefix
(see [`:bin` field](#bin-field-in-buildtur) below); conflicts with
non-Turmeric files are detected and refused without `--force`.

### Binary location: shim vs. symlink vs. copy

Three options for what lives in `~/.local/bin/tur-nb`:

| Approach | Pro | Con |
|---|---|---|
| **Symlink** to the build artifact | Cheap, always up to date if the source rebuilds | Breaks if user moves `~/.local/share/turmeric/`; not portable to Windows |
| **Hard copy** of the artifact | Self-contained; works after spice dir is deleted | Stale unless `tur install` re-runs |
| **Shim script** (`exec ~/.local/share/turmeric/spices/foo/build/tur-nb "$@"`) | Easy to add pre/post hooks; works cross-platform | One more `exec` per invocation; needs shell awareness |

**Recommendation: symlink on Unix, hard copy on Windows.** The symlink
keeps the bin/ entry in sync if the user re-runs `tur build` from inside
the spice's source dir (uncommon, but a friendly affordance for spice
developers who pin their dev install to a local path). Windows lacks
predictable symlink permissions, so fall back to a copy and re-copy on
`tur upgrade`.

A shim script is the right choice if we later need to inject env vars
(e.g., `TUR_STDLIB=...` to point at a specific stdlib version per spice).
We can switch without breaking the user-facing CLI.

### `:bin` field in `build.tur`

Today a spice that wants a binary writes a top-level `main.tur` and `main.c`
by convention; `tur build <dir>` builds the directory and the executable
name defaults to the basename of the directory. There is no declarative
"this is a binary spice, and the binary should be named X" marker.

Add `:bin` to the manifest:

```turmeric
(defpackage tur-notebook
  :name        "tur-notebook"
  :version     "0.1.0"
  :bin {
    "tur-nb" "src/main.tur"
  }
  :exports { ... })
```

Rules:

- `:bin` maps a **binary name** (the string that ends up in `~/.tur/bin/`)
  to its **entrypoint module path** (relative to the spice root). The path
  is resolved through the spice's normal module-resolution rules, so it
  may reference any module the spice itself exports.
- A spice may declare zero, one, or many binaries. A library-only spice
  (`tur-math`, `tur-test`) omits `:bin` entirely.
- Binary names **must** follow the `tur-<cmd>` convention. The
  subcommand fallthrough (see below) only searches for `tur-<cmd>`, and
  the `tur-` prefix scopes the namespace in the shared `~/.local/bin/`
  directory so spice tools don't collide with unrelated binaries.
  `tur install` rejects manifests with `:bin` names lacking the prefix.
- Conflicts (two installed spices each providing `tur-nb`, or a
  non-Turmeric `tur-nb` already in `~/.local/bin/`) are an error;
  `tur install` refuses with a diagnostic naming the existing owner.
  `tur install --force` overrides.

This replaces the implicit "if main.tur exists, the package is a binary"
inference, which is too ambiguous for spices that *also* export libraries
(notebook does both).

### `tur install`

```sh
tur install <url> [--ref <tag-or-sha>] [--subdir <path>] [--name <alias>]
tur install <path> --path           # install a local checkout (for dev)
tur install <name>                  # re-install / repair from state.tur
tur install --upgrade <name>        # fetch the latest ref allowed by lock
```

Pipeline:

1. Resolve `<url>` + `<ref>` through the same fetch path `tur add` uses,
   landing the source under `~/.local/share/turmeric/spices/<name>-<resolved-version>/`.
2. Read its `build.tur`. If `:bin` is empty, exit with
   `"spice '<name>' declares no :bin; nothing to install"`.
3. Build the spice in release mode (`tur build --release`), producing
   each declared binary in `~/.local/share/turmeric/spices/<name>-<ver>/build/`.
4. For each `:bin` entry, symlink (or copy on Windows) into
   `~/.local/bin/`.
5. Append to `~/.local/share/turmeric/state.tur` (see [State file](#state-file)).
6. If `~/.local/bin/` is not on `$PATH`, print a one-time message with
   shell-specific snippets (bash/zsh/fish) on how to add it. If it is
   already on `$PATH` (the common case), say nothing.

`tur install` is **not** the same as `tur add --global`. `tur add` is for
*library* spices a project imports; `tur install` is for *command-line
tools* a user runs. They share the fetch layer but diverge on storage,
build, and visibility.

**Security model.** `tur install` runs arbitrary third-party code at
build and invocation time, same as `cargo install` or `pipx install`.
v1 ships no sandbox; trust-model surfacing, source-vs-prebuilt
distinctions, and capability gating are all post-v1.0. If capability
gating is added later, it belongs in the runtime (enforced at syscall
boundaries), not in the package manager.

### `tur uninstall` and `tur upgrade`

```sh
tur uninstall <name>            # remove spices/<name>-* and ~/.local/bin/ entries
tur upgrade <name>              # re-resolve ref, rebuild, relink
tur upgrade --all               # for every entry in state.tur
```

Uninstall is the inverse of install: drop the symlinks, drop the source
tree, remove the state entry. Upgrade is `uninstall && install` with the
previous flags re-applied from `state.tur`, preserving `--ref` if it was
an exact pin and re-resolving if it was a moving target.

### `tur list`

User-facing inventory command. Two output modes:

```sh
tur list                # shallow tree (default)
tur list --installed    # alias of the above
tur list --json         # for tooling
```

Default output:

```
~/.local/share/turmeric/spices/
├── notebook-0.1.0       (https://github.com/turmeric-lang/turmeric-spices @ notebook-v0.1.0)
│   ├── binaries:        tur-nb
│   └── exports:         notebook/cmark, notebook/cell, notebook/format,
│                        notebook/session, notebook/cache, notebook/eval,
│                        notebook/render-md, notebook/render-html, notebook/tui
├── plot-0.3.1           (https://github.com/turmeric-lang/turmeric-spices @ plot-v0.3.1)
│   ├── binaries:        tur-plot
│   └── exports:         plot/render, plot/scales, plot/themes
└── math-0.1.0           (https://github.com/turmeric-lang/turmeric-spices @ math-v0.1.0)
    └── exports:         math/vec2, math/vec3, math/mat4

3 spices installed (2 with binaries).
PATH entry: /Users/you/.local/bin (active)
```

Design notes:

- **Shallow tree** -- one level of nesting (spice -> binaries/exports),
  not the full module-export listing. Modules are summarized in a single
  line, wrapping at the terminal width. `--verbose` expands to full
  per-module listings with summaries pulled from `;;;` module docstrings.
- **Origin line** -- shows the url + ref so users can answer "where did
  this come from?" without opening `state.tur`.
- **PATH-health hint** -- the last line confirms `~/.local/bin/` is on
  `$PATH`. If it isn't, the message becomes `PATH entry: /Users/you/.local/bin
  (NOT on PATH -- run 'tur install --print-path-snippet')`.

`tur list` is read-only and offline; it never touches the network. For
"what's available to install?" we'd add a separate `tur search` later --
out of scope for this plan.

### Subcommand fallthrough: `tur nb` -> `tur-nb`

Today `src/main.c` ends `cmd_main` with `return usage();` when the
subcommand isn't recognized. Change the tail of the dispatch chain to:

```c
/* No built-in matched. Try $PATH for tur-<cmd>. */
return try_external_subcommand(argc, argv);  /* exec tur-<cmd> or print usage */
```

`try_external_subcommand`:

1. Build `tur-<argv[1]>`.
2. Search `$PATH` for it. If not found, print usage and exit 1.
3. `execvp` it with `argv[2..]` as its argv. The exec replaces the
   current process, so exit codes and signals propagate naturally.
4. On exec failure (file exists but isn't executable, etc.) print a
   diagnostic and exit 1.

This is the same model git uses for `git-foo` / `git foo`. It costs
~30 lines of C and unlocks every future user-installed CLI tool.

A built-in subcommand always wins. We commit to never naming a built-in
verb the same as a popular spice binary; `tur nb`, `tur plot`, and
`tur fmt` stay reserved for the spice tools.

### State file

`~/.local/share/turmeric/state.tur` records what's installed and how,
so `tur list` and `tur upgrade` don't need to re-discover the directory:

```turmeric
(defstate
  :format-version 1
  :installed {
    "notebook" {:url      "https://github.com/turmeric-lang/turmeric-spices"
                :ref      "notebook-v0.1.0"
                :subdir   "spices/notebook"
                :resolved "a1b2c3..."
                :version  "0.1.0"
                :bin      ["tur-nb"]
                :installed-at "2026-05-26T14:00:00Z"}
    "plot"     {:url      "https://github.com/turmeric-lang/turmeric-spices"
                :ref      "plot-v0.3.1"
                :subdir   "spices/plot"
                :resolved "d4e5f6..."
                :version  "0.3.1"
                :bin      ["tur-plot"]
                :installed-at "2026-05-26T14:01:00Z"}
  })
```

It is not a lock file -- each project still has its own `tur.lock` for
*library* spices it depends on. `state.tur` is purely the user's
"what tools have I installed?" record.

If the user deletes the file by hand, `tur install <name>` recreates
the entry; `tur list` falls back to scanning
`~/.local/share/turmeric/spices/` and warning.

---

## Imports from global spices

A global spice can also act as a library. By default, `tur build` in a
project does **not** look in `~/.local/share/turmeric/spices/`, to
preserve build reproducibility. To opt in, the project's `build.tur`
references the global spice by name:

```turmeric
:spices {
  "notebook" {:global true}
}
```

`tur fetch` validates the global install exists at the matching version
and records its resolved SHA in `tur.lock`. If the user later runs the
project on a machine that doesn't have the global spice installed,
`tur fetch` either installs it automatically or errors out, depending
on a project-level `:global-policy {:install :error | :install :auto}`.

This is **deferred to v2.** v1 ships global spices for binaries only;
library reuse can come later when there's a real consumer asking for it.

---

## Milestones

### M1 -- Foundations (no UX yet)

- [x] `~/.local/share/turmeric/` layout created on first use (`spices/`,
      `cache/`, `state.tur`).
- [x] `~/.local/bin/` created if missing.
- [x] `XDG_DATA_HOME` and `TUR_HOME` env overrides respected everywhere.
- [x] `:bin` field parses in `build.tur` and round-trips through
      `tur format`. Validator rejects names without a `tur-` prefix.
- [x] `state.tur` reader / writer.

### M2 -- `tur install` + subcommand fallthrough (the v1 UX)

This milestone ships the user-visible loop together so a fresh `tur`
installation can run `tur install tur-notebook && tur nb new foo.tur.md`
end to end.

- [x] `tur install <url> [--ref] [--subdir] [--name]` fetches, builds,
      and symlinks one binary spice.
- [x] `tur install <path> --path` for local-checkout dev installs.
- [x] `tur uninstall <name>` reverses everything `install` did.
- [x] Conflict detection: two spices owning the same `:bin` name, or a
      pre-existing non-Turmeric file at `~/.local/bin/<name>`, fails
      with a diagnostic. `--force` overrides.
- [x] First-install PATH check: if `~/.local/bin/` is not on `$PATH`,
      print bash/zsh/fish snippets. Otherwise stay quiet.
- [x] `try_external_subcommand` in `src/main.c`: unknown `tur <foo>`
      execs `tur-foo` from `$PATH`. Built-ins always win.
- [x] `tur --help` lists installed external subcommands separately from
      built-ins ("External commands:" section), discovered by globbing
      `tur-*` on `$PATH`.

### M3 -- `tur list`

- [x] Default shallow-tree output.
- [x] `--verbose` expands export listings.
- [x] `--json` emits structured output for tooling.
- [x] PATH-health line (active vs. NOT on PATH).

### M4 -- `tur upgrade`

- [x] `tur upgrade <name>` re-runs install with prior flags.
- [x] `tur upgrade --all` iterates `state.tur`.
- [x] `tur upgrade --dry-run` reports what would change without
      modifying anything.

### M5 -- Polish

- [x] `tur install --print-path-snippet` prints just the shell snippet,
      for users who dismissed the first-run message.
- [x] `tur list --outdated` flags installs whose ref points at a tag
      newer than the resolved SHA on disk. Walks every non-path entry,
      runs `git ls-remote`, and reports `resolved -> remote` for each
      stale entry; supports `--json`.
- [ ] **Deferred.** Telemetry: `tur install` was originally slated to
      emit a hashed `install <spice> <version>` event to "the same
      opt-in metrics pipeline used elsewhere," but no such pipeline
      exists in the repo yet. Picking it up requires designing the
      transport, opt-in surface, and key-hashing scheme as its own
      plan; that work is out of scope for the global-spice rollout.
- [x] Document the global-spice-as-library opt-in path (deferred to v2)
      -- short pointer added at the bottom of
      [developing-spices-guide.md](../guides/developing-spices-guide.md#global-spices-as-libraries-v2).

---

## Open Questions

1. ~~**Stdlib pin for `~/.local/bin/` shims?**~~ Resolved: no per-spice
   stdlib pin. Each spice records the `tur` version it was built against
   in `state.tur`; on host `tur` upgrade, the next `tur` invocation
   detects stale entries and prompts the user to run `tur upgrade --all`.
   Matches the `cargo install` re-install-on-toolchain-upgrade model.
   Implement during M2.

2. ~~**Per-name or per-(name, major) versioning?**~~ Resolved: **N=1
   most-recent by default**, with `--keep-old` as an opt-in on
   `tur install` for users who want multi-version coexistence (testing,
   rollback). `tur install <name>@<ver>` re-symlinks to a specific
   version if it's already on disk; otherwise fetches it. `tur upgrade`
   always overwrites (it's the "I want the new one" verb) regardless of
   how the original install was flagged. No `tur gc` for v1 -- users who
   opt into `--keep-old` are responsible for cleanup.

3. ~~**Windows support.**~~ Resolved for v1: `tur install` refuses
   cleanly on Windows with a diagnostic pointing at the future
   Windows-support plan. M1 keeps path-handling abstracted (one function
   to swap `~/.local/share/turmeric/` for `%LOCALAPPDATA%\Turmeric\`)
   so the eventual plan has a clean integration point. Windows is
   post-v1.0; see `docs/upcoming/windows-support-plan.md` when it lands.

4. ~~**Sandboxing.**~~ Resolved: no sandbox in v1. See the "Security
   model" note under [`tur install`](#tur-install). Trust-model
   surfacing, source-vs-prebuilt distinctions, and capability gating
   are all post-v1.0; if added later, gating belongs in the runtime,
   not the package manager.

---

## See Also

- [Consuming spices guide](../guides/consuming-spices-guide.md) -- project-scoped
  `tur add` flow that this plan complements
- [Developing spices guide](../guides/developing-spices-guide.md) -- spice
  authoring; `:bin` field will land here once M1 ships
- [tur-run plan](tur-run-plan.md) -- the unrelated task-runner subcommand;
  named `tur run`, not to be confused with this plan
- [Notebook spice plan](../notebook-spice-plan.md) -- motivating use case
  for the `tur install tur-notebook` -> `tur nb` flow
