# Plan: Installed spices outside a project -- library installs, a project-less search path, and REPL loading

> **Status:** Draft, not started. Written 2026-10-21.
> **Last Updated:** 2026-10-21
> **Type:** Tooling / package management (`src/compiler/install.c`,
> `src/compiler/global.c`, `src/compiler/spice_search.c`), build driver
> (`src/main.c`), REPL (`src/turi/repl.c`, `src/turi/env.c`).
> **Follow-up:** [script-spice-deps-plan](script-spice-deps-plan.md) (spices
> declared inside a single script) assumes this plan has landed.
> **See also:** [global-spice-install-plan](../archive/global-spice-install-plan.md)
> (`tur install`, `state.tur`, `:bin`),
> [global-spice-library-consumption](../archive/global-spice-library-consumption.md)
> (`:global true` deps),
> [interpret-takes-no-include-path-or-spice-discovery](../archive/interpret-takes-no-include-path-or-spice-discovery.md)
> (`-I` on the interpreter),
> [spice-repl-plan](../archive/spice-repl-plan.md) (the REPL's spice image),
> [notebook-cells-cannot-call-inline-c-spices](../reported/notebook-cells-cannot-call-inline-c-spices.md)
> (`turi_env_attach_spice`).

## Goal

Make a spice usable without a project. After this plan, a user can do:

```sh
tur install https://github.com/turmeric-lang/turmeric-spices \
  --ref json-v0.3.0 --subdir spices/json --name json
cd ~/scratch                  # no build.tur anywhere above
tur run report.tur            # (import json/decode) resolves
tur repl                      # (import json/decode) works at the prompt too
```

and, from anywhere, including inside a project:

```text
$ tur repl -I ~/src/my-utils/src
> :spice json
Loaded spice json 0.3.0 from ~/.local/share/turmeric/spices/json-json-v0.3.0/spices/json (14 exports)
```

Projects do not change. A file under a `build.tur` sees exactly the spices its
manifest declares, as it does today. The installed set reaches only files that
have no manifest, and the REPL when the user asks for it.

## Where things stand

`tur install` already gives every user a global install area
(`~/.local/share/turmeric/`, overridable with `TUR_HOME` / `XDG_DATA_HOME`) and
a registry (`state.tur`). Two things keep an installed spice out of reach of a
script or a REPL session:

1. **Every install needs a `:bin`.** `do_install_spec` refuses a spice with
   none ("declares no :bin entries; nothing to install",
   `src/compiler/install.c:515`). A library-only spice cannot be installed at
   all.
2. **Installed spices are not on any default search path.** The only consumer
   is a project dep declared `:global true` (`src/compiler/pkg.c:299-315`,
   resolved through `tur_installed_spice_dir`, `src/compiler/global.c:463`).
   With no `build.tur`, `spice_search_append` returns as soon as
   `spice_find_root` finds no manifest (`src/compiler/spice_search.c:153-154`),
   so a manifest-less file searches only its own directory, the stdlib and its
   `-I` dirs.

The REPL is narrower again:

- `tur repl` accepts only `--watch`, `--lang` and `--engine`
  (`src/main.c:14636-14661`). There is no `-I`.
- Its only spice mechanism is RP3 auto-discovery of an enclosing `build.tur`
  from cwd, which builds that one spice as a shared library and binds its
  exports (`src/turi/repl.c:1405-1435`).
- The session's `include_dirs` are never set, so a top-level `(import M)`
  resolves only against cwd and the stdlib.

The pieces this plan reuses already exist:

| Piece | Where | Used for |
| --- | --- | --- |
| Install registry + path helpers | `src/compiler/global.{h,c}` (`tur_global_*`, `tur_state_*`, `tur_installed_spice_dir`) | Finding installed spices by name |
| Per-file search-path walk | `spice_search_append`, `src/compiler/spice_search.c:148`; shared by `auto_append_spice_includes` (`src/main.c:5133`) and libturi's `turi_env_set_search_path_for` (`src/turi/env.c:654`) | The one place the project-less fallback goes |
| Transitive dep closure for a spice root | `resolve_include_dirs_from_manifest` / `collect_dep_dirs_recursive`, `src/main.c:5420-5500` (static in `main.c`; project mode only. The per-file walk's SC5 step reads one level of `:spices`, `src/compiler/spice_search.c:178-214`) | An installed spice's own `src/` plus its deps' |
| Link inputs for a spice root | `collect_build_aux` (`src/main.c:2957-2988`): the `cmake/` deps manifest, `append_manifest_link_flags`, `collect_spice_aux_c` (`src/main.c:5608`) | `:c-sources` / `:cmake-deps` of the installed spices a program imports |
| Per-include-slot provenance | LS2's `module_include_workspace_producer` (`src/compiler/elab_internal.h:789-800`), matched at `src/compiler/elab_module.c:889-903` | Knowing which installed spice satisfied an import |
| Project fetch | `pkg_fetch_all`, `src/compiler/pkg.h:486` (+ `pkg_gen_cmake_deps` and the cmake build) | Fetching an installed spice's own deps inside its install root |
| Attach a compiled spice image to an interpreter env | `turi_env_attach_spice` / `turi_env_attach_spice_for_module`, `src/turi/env.c:562-648` (built for the notebook embedder; `tur repl` does not call it yet) | Running an installed spice's inline-C at the REPL |
| `-I` parsing | `parse_include_flags` / `is_include_flag` (`src/main.c`) | `tur repl -I` |

## Design decisions

### D1. Projects stay hermetic; only manifest-less contexts see the installed set

A **manifest-less context** is one where `spice_find_root` finds no `build.tur`
(or `build.tur.sweet`) within `TUR_SPICE_WALK_MAX` ancestors of the anchor. The
anchor is the input file for per-file commands, cwd for `tur run -` (stdin) and
for the REPL. Only manifest-less contexts get the installed set on their search
path.

This keeps the original plan's rule ("the global `spices/` root is
deliberately not on the default module-resolution path",
[developing-spices-guide](../guides/developing-spices-guide.md#global-spices-as-libraries))
for the case it was written for: a project build should not depend on what
happens to be installed on the machine. A one-off script or a REPL session has
no lock file and no reproducibility contract to break. Making it reproducible
is the job of the [follow-up plan](script-spice-deps-plan.md).

Inside a project, nothing changes. `:global true` stays the per-dep opt-in, and
the REPL's `:spice` (D10) is an explicit request typed at an interactive
prompt, so it is allowed there too.

### D2. Search order: installed spices fill gaps, they never shadow

For a manifest-less context the module search order becomes:

1. the importing file's directory,
2. the stdlib,
3. `-I` dirs, in command-line order,
4. **installed spices**: every installed spice's own `src/`, in install-name
   order (byte-wise, so the order does not depend on the locale or on
   `state.tur`'s entry order),
5. **their dependencies**: each installed spice's transitive `:spices`
   closure, in the same order and deduplicated by real path.

Steps 1-3 are today's order unchanged. An installed spice can only satisfy an
import that would otherwise fail with "module not found". It never changes
what an import that resolves today resolves to.

Putting every top-level installed spice ahead of every dependency means the
spice the user installed by name beats a different version of it that some
other installed spice happens to depend on.

### D3. No `--lib` flag: any spice with modules can be installed

The chat sketch behind this plan proposed `tur install <url> --lib`. The plan
drops the flag. All it would decide is whether the `n_bins == 0` refusal fires,
and that refusal protects nothing: an installed spice is already a library to
every `:global true` dep. So:

- A spice with `:bin` entries installs as today, and it is also a library.
- A spice with no `:bin` but a `src/` directory is a **library-only install**:
  no build step, nothing in `~/.local/bin/`.
- A spice with neither is refused ("declares no :bin entries and has no src/;
  nothing to install").

Library-only installs also close the third "Not done" item of
[global-spice-library-consumption](../archive/global-spice-library-consumption.md):
a `:global true` dep can then name a library-only spice.

### D4. An installed spice is a fetched project

Today `tur install` fetches a spice's `:spices` deps itself. It handles only
the first level (`src/compiler/install.c:582-642`). It puts the clones into the
**shared** `spices/` directory next to the installed spices, as
`<name>-<ref>`, and passes their `src/` to the `:bin` build as `-I` flags.
Three problems follow:

- A dep of a dep is never fetched.
- `:cmake-deps` and `:c-sources` of a dep never reach the build.
- Every project-mode resolver, which expects `<root>/spices/<name>-<ref>`
  (SC5 in `spice_search_append`, `collect_dep_dirs_recursive`,
  `collect_spice_aux_c`, the REPL image's `tur build --shared --bundle-deps`),
  looks in the wrong place for an installed spice's deps.

The fix is to install a spice the way `tur fetch` sets up a project:

- run `pkg_fetch_all` with the installed root as the project dir, so deps land
  in `<install root>/spices/` with a `tur.lock`,
- generate and build `cmake/` from `:cmake-deps`,
- build each `:bin` entry with the transitive include list project mode
  computes for that root, instead of a hand-collected first-level `-I` list.
  The link inputs already come from `collect_build_aux`'s walk up from the
  entry file, which finds the installed root's `build.tur`.

After that, an installed root looks exactly like a fetched project, and every
consumer added below goes through code that already works for projects. The
shared `spices/` namespace then holds installed spices only, which D5 relies on.

### D5. `state.tur` records where an install lives

The install directory is chosen from `derived_name` (`--name`, else the URL's
last segment; `src/compiler/install.c:468-480`). The registry entry is keyed by
`install_name` (`--name`, else the manifest's `:name`, else the derived name;
`:526-528`). Every reader reconstructs the directory from the **entry name**:
`tur_installed_spice_dir` (`src/compiler/global.c:463-504`) and `tur list`'s
`inst_entry_src_dir` (`src/compiler/install.c:992-1012`).

When the manifest's `:name` differs from the URL's last segment and no
`--name` is given, the reconstruction misses. That is the normal case for a
`turmeric-spices` monorepo install. For example:

- `tur install https://github.com/turmeric-lang/turmeric-spices --ref json-v0.3.0 --subdir spices/json`
  clones into `spices/turmeric-spices-json-v0.3.0/`, but registers `json`.
- Readers then look for `spices/json-json-v0.3.0/spices/json` and miss.
- So `tur list` reports the source as missing, a `:global true` dep says "not
  installed", and `tur uninstall json` leaves the clone behind.

This was found by reading the code and has not been reproduced. Every example
in the docs passes `--name`, which hides it.

`tur uninstall` has a related problem. It deletes **every** directory under
`spices/` whose name starts with `<name>-` (`src/compiler/install.c:948-970`).
So `tur uninstall plot` also removes `plot-extras-HEAD/`, and also the
`plot-<ref>/` clone that another installed spice fetched as a dependency
(D4's shared namespace).

Both problems go away when the entry records its directory:

- Add `:dir` (the install root) to each `state.tur` entry, and `:src-dir` too
  when `:subdir` applies.
- `tur_installed_spice_dir`, `tur list` and `tur uninstall` read the recorded
  directory. They fall back to today's reconstruction only for an entry that
  predates the field.
- `tur uninstall` deletes exactly the recorded root.

`state.tur` is read key by key (`gs_map_get_kw`, `src/compiler/global.c:175`),
so an older `tur` ignores the new keys. It drops them when it rewrites the file;
the fallback covers that.

### D6. Two installed spices may not provide the same module

The installed set behaves like one namespace in a manifest-less context, so
`tur install` checks module paths the same way it checks `:bin` names:

- A module path is the path of a `.tur` file relative to `src/`.
- If a module path in the new spice already exists in another installed
  spice's `src/`, the install is refused, naming both spices.
- `--force` overrides with a warning, and D2's order then decides which one
  resolves.
- Dependencies are not checked. They are per-spice and versioned, and D2 puts
  them after every top-level spice.

### D7. Link inputs come only from installed spices the program imports

Every installed spice's `src/` goes on the search path; that costs one
directory probe per import. Linking every installed spice's `:c-sources` and
`:cmake-deps` libraries into every manifest-less build would be wrong:

- one broken or platform-specific spice would break every script,
- unrelated vendored C could collide at link time.

So installed-spice include slots carry provenance, the way LS2 tags
workspace-sibling slots (`module_include_workspace_producer`). The elaborator
already knows which slot satisfied each import (`matched_ii`,
`src/compiler/elab_module.c:889`). It records the installed roots it used.
Both build paths call `collect_build_aux` after the C is generated
(`src/main.c:4668` and `:9147`), so it can feed those roots through the same
three collectors it runs for a project root:

- the `cmake/` deps manifest,
- `append_manifest_link_flags`,
- `collect_spice_aux_c`.

A spice reached only as another spice's dependency contributes link inputs
through that spice's closure, exactly as in project mode.

### D8. One switch to turn it off, and the test harnesses use it

- `--no-global-spices` (a global flag, parsed next to `--no-auto-spice`) and
  `TUR_NO_GLOBAL_SPICES=1` remove steps 4-5 of D2.
- `--no-auto-spice` implies it. That flag already means "add nothing to the
  search path on my behalf".
- `tests/run.sh`, `tests/run-turi.sh`, `tests/run-jit.sh` and
  `tests/run-flags.sh` export `TUR_NO_GLOBAL_SPICES=1`.

Almost every fixture lives outside any `build.tur` (the repo root has none). So
without the switch, a developer's installed spices would leak into the suite:
an `errors/` fixture that expects "module not found" would start passing an
import on that one machine.

### D9. No `--enable` gate

This is resolver behavior, not language surface. It only turns a "module not
found" outside a project into a successful resolution, and D8 is its off
switch. CLAUDE.md's experimental-features rule exempts build-system options of
this kind (`-I`, `--build-dir`), so no `EXPERIMENTS[]` row is added.

### D10. The REPL gets `-I`, `:spice`, and the D2 fallback

- **`tur repl -I <dir>`** (repeatable, `-Idir` too) sets the session's
  `include_dirs`. Each dir is made absolute at startup, so `:cd` does not change
  what it means.
- **The D2 fallback:** a REPL started where cwd has no `build.tur` above it
  gets the installed spices after the `-I` dirs. The search path is set through
  `turi_env_set_search_path_for`, anchored at cwd, so `tur --interpret` and
  `tur repl` share one list. One side effect: inside a spice, a top-level
  `(import own/module)` now resolves from `src/`, which it does not today.
- **Compiled code for imported spices:** before evaluating a turn, the REPL
  calls `turi_env_attach_spice_for_module` for each module the turn imports.
  That is the hook the notebook embedder already uses, and it is why an
  inline-C function from an installed spice runs instead of being refused. The
  image is built once and cached in `<spice root>/.tur-repl-cache/`
  (`src/turi/spice_loader.c:286`).
- **`:spice <name-or-path>`** loads a spice into the session explicitly:
  - an installed spice by its registry name, or a local checkout by path,
  - it works inside a project as well as outside one,
  - it appends the spice's `src/` and dep closure to the session's search path,
    and attaches its image so its exports are bound under bare and qualified
    names (the RP4 rules),
  - `:spice` with no argument lists the loaded spices and the installed ones
    not yet loaded.

## Milestones

### IL0 -- Fix the install layout (prerequisite)

- [ ] `state.tur` entries record `:dir` (and `:src-dir` with `:subdir`) (D5).
      Readers prefer them, and fall back to reconstruction for old entries.
- [ ] `tur_installed_spice_dir`, `inst_entry_src_dir` and `cmd_pkg_uninstall`
      use the recorded directory. Uninstall removes exactly that tree.
- [ ] Move the transitive include walk (`resolve_include_dirs_from_manifest` /
      `collect_dep_dirs_recursive`) out of `main.c` into tur_core, next to
      `spice_search_append`, so `install.c`, IL2 and libturi can all call it.
- [ ] `tur install` sets the installed root up as a fetched project (D4):
      `pkg_fetch_all` into `<root>/spices/`, the `cmake/` build, and `:bin`
      builds with the root's transitive include list. Retire the bespoke
      first-level fetch at `install.c:575-649`.
- [ ] `tur upgrade` runs the same pipeline.
- [ ] `tests/run-install.sh` gains three cases:
      - a git install from a local bare repo (`file://` URL) with `--subdir`
        and no `--name`, checking the D5 regression through `tur list`,
        `:global true` and `tur uninstall`,
      - uninstalling `plot` leaves `plot-extras` alone,
      - an installed `:bin` spice whose dep has its own dep builds.

### IL1 -- Library-only installs

- [ ] Drop the `n_bins == 0` refusal. Refuse only a spice with neither `:bin`
      nor `src/` (D3).
- [ ] Module-path conflict check against the installed set, with `--force`
      (D6).
- [ ] `tur list`:
      - the tree marks library-only entries,
      - `--json` gains `"kind": "library" | "tool"` (a tool also has modules)
        plus the `modules` list.
- [ ] `tur uninstall` / `tur upgrade` / `tur list --outdated` accept library
      entries. They have no bins to unlink or relink.
- [ ] `:global true` can name a library-only spice. Remove that caveat from
      `developing-spices-guide.md`.
- [ ] `run-install.sh`:
      - install a library-only spice with `--path`,
      - `tur list` and `--json` show it,
      - the conflict refusal and `--force`,
      - a `:global true` project builds against it.

### IL2 -- The project-less search path

- [ ] `spice_search_append`: when `spice_find_root` finds no manifest, append
      D2 steps 4-5 from the registry, unless disabled (D8). This puts the
      fallback in tur_core, so every caller gets it at once:
      - `auto_append_spice_includes`: `tur check`, `tur run <file>`,
        `tur build <file>`, `tur --interpret`, `tur jit`, `tur emit-c`,
        `tur expand`, `tur eval --file`,
      - libturi's `turi_env_set_search_path_for` (embedders such as the
        notebook spice; they can set `TUR_NO_GLOBAL_SPICES=1`).
- [ ] Step 5 uses the transitive walk IL0 moved into tur_core. It is worth
      switching SC5, the per-file walk's own one-level `:spices` step, to the
      same walk in this change: a file inside a spice whose dep has a dep of
      its own then resolves the grand-dep too. That only fills gaps; it changes
      no import that resolves today.
- [ ] `tur run -` (stdin) anchors the walk at cwd.
- [ ] Provenance (D7): installed-spice slots carry their spice name and root,
      in a parallel array next to LS2's, and the elaborator records the roots
      it resolved through.
- [ ] Diagnostics for a failed import:
      - the `searched:` list labels installed-spice dirs with the spice name,
        and collapses them to one line beyond a handful,
      - outside a project, the hint names `tur install` alongside `-I`.
- [ ] `--no-global-spices` / `TUR_NO_GLOBAL_SPICES=1`, and the implication
      from `--no-auto-spice` (D8). Add both to `usage`, the shell completions
      (`src/cli/completion.c`), and the test harnesses.
- [ ] The LSP runs its own manifest walk-up (`src/lsp/lsp.c:1338`). Give it the
      same fallback, so an editor resolves the same imports `tur check` does.
- [ ] Tests, using a scratch `TUR_HOME`:
      - `tur run` / `tur check` / `tur --interpret` of a file outside any
        project import an installed spice,
      - the same file under a `build.tur` that does not declare it still fails,
      - `--no-global-spices` and `--no-auto-spice` turn it off,
      - `-I` and a sibling module still win over an installed module of the
        same path.

### IL3 -- Link inputs for imported installed spices

- [ ] `collect_build_aux` feeds the roots recorded in IL2 through its three
      collectors (D7), and dedups against a project root when both apply.
- [ ] A test: an installed spice with `:c-sources` is imported by a project-less
      `tur run` and links. A second installed spice whose `:c-sources` do not
      compile on purpose does not break a script that never imports it.

### IL4 -- REPL search path and `-I`

- [ ] `tur repl` parses `-I` (D10) and rejects a trailing bare `-I`, as the
      interpreter arm does.
- [ ] The REPL sets its search path with `turi_env_set_search_path_for`,
      anchored at cwd. It is computed once at startup; `:cd` does not
      recompute it (documented).
- [ ] Before each turn's evaluation, the REPL calls
      `turi_env_attach_spice_for_module` for each top-level `(import M ...)` in
      the turn.
- [ ] `TUR_NO_AUTO_SPICE=1` keeps meaning "no discovery at all", installed
      spices included.
- [ ] A `hook.sh` fixture pipes a session into `tur repl` with a scratch
      `TUR_HOME`. It imports an installed spice's inline-C function and calls
      it, and it imports through `-I`.

### IL5 -- `:spice`

- [ ] `:spice <name>` and `:spice <path>` (D10). A path is anything containing
      `/`, or starting with `.` or `~`. An unknown name gets "not installed --
      `tur install <source>` first".
- [ ] `:spice` with no argument lists loaded and installable spices.
- [ ] Add `:spice` to the command table (`src/turi/repl.c:213`), `:help`, and
      tab completion. Installed names complete after `:spice `.
- [ ] `(reload)` also rebuilds an attached spice whose sources changed. Only a
      `--path` install or a local checkout can change; use
      `tur_spice_image_is_fresh`.
- [ ] Fixture cases:
      - `:spice` by name and by path,
      - a second `:spice` of the same spice is a no-op,
      - `:spice` inside a project,
      - a bad name.

### IL6 -- Docs

- [ ] `docs/guides/consuming-spices-guide.md`: a "Using spices without a
      project" section covering install, project-less resolution and its order,
      the off switch, and why projects do not see the installed set.
- [ ] `docs/guides/developing-spices-guide.md`, "Global Spices as Libraries":
      library-only installs, D6 conflicts, D4 (an installed spice's deps live
      inside it).
- [ ] `docs/guides/repl.md`: `-I`, `:spice`, and importing installed spices
      outside a project.
- [ ] CLAUDE.md's "Per-file Commands Inside a Spice" gains one line about
      the project-less fallback.

## Open questions

1. **Prebuild the REPL image at install time?** IL4 builds an installed
   spice's `.tur-repl-cache/` on first import, which makes the first import
   slow (it is a `tur build --shared`). `tur install` could build it up front.
   That roughly doubles install time and only pays off for REPL users.
   Proposed: lazy by default, with `tur install --prebuild`, decided after
   measuring.
2. **Diamond deps across installed spices.** Two installed spices that depend
   on different refs of the same spice put both `src/` dirs on the path, and
   first match wins. Project mode has the same behavior today. Not made worse
   here, and not solved.
3. **`:reset` and attached spices.** Should `:spice` loads survive `:reset` /
   `:run` the way `-I` does? Proposed: yes, re-bound after the reset, since
   they are session configuration rather than session state. This needs a look
   at what `turi_env_reset` keeps.
4. **A tool spice's internal modules.** Under D3, `tur install` of a tool also
   puts its modules on the project-less path. If a tool's internals turn out to
   collide in practice (D6 refusals with no real library involved), add
   `tur install --no-lib`. Not proposed until it happens.

## Out of scope

- Spices declared inside a script, a cache of script deps, and lock files:
  [script-spice-deps-plan](script-spice-deps-plan.md).
- `:global-policy` (auto-installing a missing `:global` dep) and version-range
  checks for `:global` deps. These are the remaining "Not done" items of
  global-spice-library-consumption and are unchanged here.
- The web REPL. It has no filesystem and no install area.
