# Plan: Namespaced spice binaries -- `tur <spice>/<bin>`, and a bin-name collision that installs with a warning

> **Status:** Implemented 2026-10-28 (NB0-NB4 in one change; see "As built"
> at the end). D2 (install with a warning) and D4's spelling
> (`tur install --link`, not `tur link`) decided 2026-10-24, as is D5's
> "uninstalling the holder leaves the name unlinked".
> **Type:** Tooling / CLI (`src/compiler/install.c`, `src/compiler/global.c`,
> `src/compiler/spice_search.c`, `src/driver/cli.c`, `src/driver/cmd_x.c`).
> **Builds on:** [global-spice-install-plan](global-spice-install-plan.md)
> (`tur install`, `:bin`, the `tur <name>` fallthrough),
> [installed-spice-libraries-plan](installed-spice-libraries-plan.md)
> (D5's install layout, D6's module-conflict check) and
> [project-less-tools-plan](project-less-tools-plan.md) (PT3's
> `tur x`).
> **Absorbs:** [installed-bin-entry-modules-still-importable](installed-bin-entry-modules-still-importable.md)
> (NB0 below).

## Goal

Two installed spices can both ship a binary named `tur-foo`, and both stay
usable:

- The bare command (`tur-foo` on `PATH`, and `tur foo`) belongs to one of them:
  the first one installed, unless the user moves it.
- **`tur alpha/foo`** always runs alpha's `tur-foo`, whichever spice holds the
  bare name.
- Installing the second spice **succeeds with a warning**. It no longer refuses.
  The spice's library modules and its qualified binary are useful even when the
  bare name is taken, and refusing threw both away over a naming clash.

A tool's `:bin` entry file (typically `src/main.tur`) is a program and never a
library module. It is hidden from imports as well as from the conflict check.

## Where things stand (checked 2026-10-24, `main` acd35812)

- **Bin names are already half-namespaced.** A `:bin` name must start with
  `tur-` (`pkg.c:1240`). `tur foo` falls through to `tur-foo` on `PATH`, the way
  git does (`drv_try_external_subcommand`, `cli.c:1366`). That function rejects
  any name holding something other than `[A-Za-z0-9_-]`, so a `/` reaches no
  external command today.
- **`tur x <installed-name> --bin <b>`** already runs one binary of one
  installed spice, from `<src_dir>/build/<bin>`, without going through `PATH`
  (`cmd_x.c:290`).
- **A collision refuses.** `inst_check_bin_target` (`install.c:232`) fails the
  install unless `--force` is given, and `--force` overwrites the other
  spice's link. Between two `--path` installs the refusal is also mis-worded,
  as "not owned by tur" (the report above).
- **The registry records bins per spice** (`TurStateEntry.bin`, `global.h:74`),
  not which spice holds each bare name. On POSIX the symlink's target says who
  owns it. On Windows an installed bin is a copy (`inst_place_bin`), and the
  registry is the only record.
- **`:bin` entries are skipped by D6** (`tur_spice_modules`, `global.c:629`),
  but `spice_search` still puts every installed `src/` file on the
  project-less path. So `(import main)` resolves to the first installed tool's
  entry (the report).

## Design

### D1. The bare name has one holder, and the registry records it

Add a per-bin `:linked` flag to each `state.tur` entry. Today an entry has
`:bin ["tur-foo" "tur-bar"]`. It becomes `:bin [{:name "tur-foo" :linked true}
...]`, and the reader keeps accepting the old string list as all-linked, so an
existing registry needs no migration. "Who holds `tur-foo`" is then a registry
question on every platform. That stops symlink-target sniffing from being the
source of truth, and it also fixes the "not owned by tur" wording.

A file at `bin_dir/tur-foo` that no registry entry claims is still the user's
own. Installing over it still refuses without `--force`, as today. Shadowing
applies only between spices tur installed.

### D2. A collision installs, shadowed, with a warning

When `tur-foo` is held by another installed spice, `tur install beta`:

1. builds beta's `tur-foo` into `<src_dir>/build/` as usual;
2. does not touch `bin_dir/tur-foo`, and records the bin with
   `:linked false`;
3. prints one warning per shadowed bin:

   ```
   tur install: warning: 'tur-foo' is already provided by installed spice 'alpha';
     beta's copy is installed but not linked. Run it as `tur beta/foo`,
     or make it the default with `tur install --link beta/foo`.
   ```

`--force` keeps its meaning: install and take every bare name the spice
declares (as `--link` would, D4). It is no longer the only way past a
collision.

D6's module conflicts are a different matter and keep refusing. Two spices
providing the same module make an import ambiguous, and a warning cannot tell
the reader which one they got. Only a binary has a qualified form to fall back
on.

### D3. `tur <spice>/<bin>`: the qualified command

In `drv_try_external_subcommand`, a name of the form `<spice>/<bin>`, with
exactly one `/`, and `[A-Za-z0-9_-]` on each side:

- resolves `<spice>` as an installed name (with or without its `tur-` prefix,
  as the other installed-name lookups do);
- resolves `<bin>` as `tur-<bin>`, or as itself if it already starts with
  `tur-`;
- execs `<src_dir>/build/<bin>` (with `inst_bin_name`'s `.exe` on Windows),
  exactly as `tur x <spice> --bin <bin>` does, sharing that code rather than
  copying it;
- errors with the spice's bin list when `<bin>` is not one of them, and with
  "no installed spice named ..." when `<spice>` is unknown.

No built-in subcommand contains a `/`, so the form cannot collide with one.
`tur x` accepts the same spelling: `tur x beta/foo -- args`. The qualified form
works whether the bin is linked or shadowed, so a script can always use it to
pin which tool it means.

### D4. Moving the bare name: `tur install --link <spice>/<bin>`

`tur install --link beta/foo` points `bin_dir/tur-foo` at beta's build. It
flips the two registry flags (`alpha :linked false`, `beta :linked true`) and
prints what it displaced. It is a flag on `install`, not a top-level
command: `tur link` is already the compile linker, which links the objects
`tur compile` lowers (`cli.c:944`, `--link-flags`). A second meaning for
"link" at the top level would collide outright. Given an already-installed spice it neither fetches
nor rebuilds: it only moves the link. Given one that is not installed, it is
`tur install` followed by the move. `tur install --link alpha` (no `/<bin>`)
takes every bare name alpha declares, which is what `--force` does to
another spice's links; unlike `--force`, it never overwrites a file `tur` did
not install. `tur list` marks a shadowed bin as
`tur-foo (shadowed by alpha; tur beta/foo)`, and `tur list --json` carries the
`:linked` flag.

### D5. Uninstall and upgrade never choose a holder

- `tur uninstall alpha` removes `bin_dir/tur-foo` only if the registry says
  alpha holds it. If another installed spice still provides `tur-foo`, the
  bare name is **left unlinked**. Nothing is promoted, and a note says what
  is left:

  ```
  tur uninstall: note: 'tur-foo' is now unlinked; beta still provides it.
    Run it as `tur beta/foo`, or make it the default with
    `tur install --link beta/foo`.
  ```

  The user picked which tool `tur-foo` meant, either by install order or with
  `--link`. Uninstall cannot know which remaining provider they would pick, and
  quietly substituting a different tool under a familiar command is the
  surprise this plan exists to remove. The qualified form keeps working
  throughout, so nothing becomes unreachable.
- Uninstalling a shadowed provider touches no link.
- An unlinked name is free. The next `tur install` of a spice that declares it
  takes it under D2's ordinary rule, the same as a name nobody ever held.
  Shadowed providers stay shadowed: becoming the holder takes `--link`.
- `tur upgrade` keeps each bin's flag. A newly added `:bin` is placed by D2's
  rule, and a removed one is unlinked only if it was linked.

### D6. `:bin` entry files are not importable

`spice_search` leaves out the modules an installed spice's `:bin` entries name.
It derives that set with the same `gs_bin_module` mapping as `tur_spice_modules`,
so the conflict check and the search path cannot disagree. Then
`(import main)` outside a project is `module 'main' not found`, not a silent
pick. A tool that wants its logic importable keeps it in a library module that
`main.tur` calls, which is the shape `tur-nb` already has
(`spices/notebook/src/main.tur` over `src/notebook/`).

Inside a project the same applies to a `:spices` dependency's entry files.
That is the same mistake through a different resolver. Whether a project's
**own** `src/main.tur` stays importable by its own modules is decided in NB0
by looking at whether anything in the corpus does it. The default is to leave
it alone.

### D7. Later: `<spice>@<version>/<bin>`

The `/` form leaves room for `tur beta@0.2.0/foo`. That needs two versions of
one spice installed side by side, and today the registry is keyed by name
(`tur_state_find`) and installs into `spices/<name>-<ref>`. Out of scope here.
In D3, reject an `@` with "versioned selection is not supported yet" so the
syntax stays free.

## Stages

- **NB0, hide entry files (D6) and fix the refusal wording.** Close the report:
  `spice_search` exclusion, the registry lookup moved ahead of the symlink
  branches in `inst_check_bin_target`, and the corpus check for D6's project
  half. Lands alone; it changes no command.
- **NB1, the registry flag (D1).** The `:linked` field, read compatibly from
  the old list, and written by install, upgrade and uninstall at their
  current behaviour (everything linked). No user-visible change.
- **NB2, the qualified command (D3).** `tur <spice>/<bin>` and
  `tur x <spice>/<bin>`, the `@` rejection, and docs in
  `consuming-spices-guide`. Useful even before NB3, since it always reaches a
  specific spice's build.
- **NB3, install with a warning (D2) and uninstall's unlinked name (D5).** The
  behaviour change. CHANGELOG entry: a collision no longer fails `tur install`.
- **NB4, `tur install --link` and the `tur list` marks (D4).**

## Tests

All in `tests/run-install.sh`, under a throwaway `TUR_HOME`:

- two `--path` spices that each declare `tur-foo`. The second install exits 0
  with the warning; `tur-foo` runs the first; `tur second/foo` runs the second;
- `tur install --link second/foo` swaps them without a rebuild, and
  `tur list --json` shows the flags;
- uninstalling the holder removes `tur-foo`, prints the note, and leaves the
  other provider shadowed (`tur-foo` is not found; `tur second/foo` runs).
  Installing a third provider afterwards takes the name, and uninstalling a
  shadowed provider leaves the link alone;
- a foreign file at `bin_dir/tur-foo` still refuses without `--force`;
- two tools with `src/main.tur`: a manifest-less `(import main)` reports
  `module 'main' not found` (NB0);
- `tur nosuch/foo`, `tur first/nosuch` and `tur first@1/foo` each fail with
  their message;
- Windows (copies, not links): the same, through the registry flag alone.

## Open questions

None at the time of writing. The three design calls (install with a warning,
`--link` on `install`, and uninstall leaving the name unlinked) were made
2026-10-24.

## As built (2026-10-28)

All five stages landed together. Where the code differs from the text above:

- **D1.** `TurStateEntry.bin_linked` runs parallel to `bin`;
  `tur_state_bin_holder` answers "who holds `tur-foo`".
  `inst_plan_bin` (`install.c`, replacing `inst_check_bin_target`) asks the
  registry first on every platform; the symlink checks remain only for a file
  no entry claims (an own link whose registry row was lost still installs).
- **D3.** The `<spice>/<bin>` dispatch sits in `main.c`, after the script check
  and before `drv_script_word_shaped`'s "no such file" error, not in
  `drv_try_external_subcommand` -- every word containing `/` is script-shaped,
  so the fallthrough was never reached. An existing file named `alpha/foo`
  stays a script. `tur x` treats `<spice>/<bin>` the same unless a directory of
  that name exists, which stays a path. Both share `drv_run_qualified_bin` /
  `x_installed_exe` in `cmd_x.c`, which also back `tur x <name> --bin <b>`; `--bin`
  now accepts the bin without its `tur-` prefix.
- **D4.** `--link` is a boolean flag on `tur install`. When its argument names
  an installed spice (`beta` or `beta/foo`, without `--path`), it only moves
  names; otherwise the argument is a source, installed with every name taken.
  `tur list --json` keeps the old `"bin"` string list and adds
  `"bins": [{"name", "linked", "held_by"}]`.
- **D5.** A reinstall (`tur install` of an already-installed spice) keeps
  flags like an upgrade does. `tur upgrade` no longer passes `--force`
  internally; it sets `InstallSpec.upgrade`, which keeps module conflicts
  warn-only as before. A bin the manifest no longer declares has its link
  removed if the spice held it.
- **D6 / NB0.** The search path hides entry files per include dir
  (`spice_search_module_hidden`), for installed spices, their dependencies,
  a project's `:spices` deps and its workspace siblings. The corpus check found
  nothing importing a project's own entry module (`(import main)` appears
  nowhere), so the project's own `src/` is left as it was.

Tests: `tests/run-install.sh` 65-73.
