# An installed tool's `:bin` entry file is skipped by the conflict check but is still importable, so `(import main)` silently picks one tool

**Severity:** low-medium (silent wrong resolution). Filed 2026-10-24.
Scheduled as NB0 of
[namespaced-spice-binaries-plan](../upcoming/namespaced-spice-binaries-plan.md).

installed-spice-libraries D6 refuses to install a spice whose modules collide
with another installed spice's, because outside a project every installed
spice's `src/` shares one namespace. Its module list (`tur_spice_modules`,
`src/compiler/global.c:629`) leaves out `:bin` entry files, so that two tools
which both keep their entry at `src/main.tur` do not conflict. That decision is
right. But the *search path* still holds those files:
`spice_search.c` adds each installed spice's whole `src/`, and nothing there
knows about `:bin`. So a manifest-less file's `(import main)` resolves,
silently, to the entry file of whichever tool sorts first. The other tools'
entries are unreachable, and no ambiguity error is given, because the check
that would give one skips them.

## Repro (Release `tur`, a throwaway `TUR_HOME`)

Two spices, `alpha` and `beta`. Each has `:bin #map{"tur-<name>" "src/main.tur"}`,
and each `src/main.tur` is `(defmodule main (export <name>-only) ...)`.

```sh
tur install ./alpha --path && tur install ./beta --path   # both install (D6 skips main)
# outside any project:
(defmodule user/script (import main :refer [beta-only]) ...)
#   error: symbol 'beta-only' is not exported from module 'main'   <- it found alpha's
(defmodule user/script (import main :refer [alpha-only]) (defn main [] : int (alpha-only)))
#   cc: error: redefinition of 'main'                              <- alpha's entry pulled in whole
```

## Also: the bin-collision refusal names the wrong cause

Install a third spice that declares `tur-alpha` too, and the refusal is
`refusing to overwrite existing '.../bin/tur-alpha' (not owned by tur)`. But tur
did install it, as spice `alpha`. `inst_check_bin_target`
(`src/compiler/install.c:232`) consults the registry only for a non-symlink.
For a symlink it recognises only a target under `spices_dir`, and a `--path`
install links into the user's checkout instead, so the check falls through to
the "not owned" message. It should say `already provided by installed spice
'alpha'`, as the registry branch does.

## Fix directions

- In `spice_search`, when adding an installed spice's `src/`, exclude the
  modules its `:bin` entries name. Use the same `gs_bin_module` mapping
  `tur_spice_modules` uses, so the two cannot disagree. A tool's entry is then
  invisible to imports, exactly as it is to the conflict check. A spice that
  wants its CLI logic importable puts it in a library module that `main.tur`
  calls.
- Consider the same for `:spices` deps inside a project: a dependency's entry
  file is never a library either.
- Do the registry lookup in `inst_check_bin_target` before the symlink
  branches.
- Pin both: an installed-spice ctest with two `src/main.tur` tools (assert
  `module 'main' not found`), and the third-spice collision (assert the
  message names `alpha`).
