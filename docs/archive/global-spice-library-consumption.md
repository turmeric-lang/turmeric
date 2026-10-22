# Globally-installed spices cannot be consumed as libraries via :global deps

**Severity: low** (expressiveness hole; documented as "v2") -- a spice
installed with `tur install` cannot be declared as a manifest dependency.
Found in the 2026-08-20 docs audit.

## Repro

`(defpackage app :spices #map{"notebook" #map{:global true}})` + `tur fetch` --
the `:global` key is not parsed (no handling in src/compiler/pkg.c's
manifest-dep loop; only `:url`/`:ref`/`:subdir`/`:path`/`:optional` shapes
exist).

## Root cause

Never implemented; design sketch in docs/archive/global-spice-install-plan.md
("Imports from global spices").

## Fix direction

Add the `:global true` dep shape to `pkg_manifest_read`'s `:spices` parsing,
resolve against `tur_global_spices_dir()` (src/compiler/install.c), validate
version, record the resolved SHA in tur.lock; add a `:global-policy` knob.

## Guides to update when fixed

- docs/guides/developing-spices-guide.md ("Global Spices as Libraries (v2)")
- docs/guides/consuming-spices-guide.md

## Resolution (2026-08-21)

Implemented along the fix direction, minus the two pieces its own last sentence
bundles in (see "Not done" below).

- `PkgSpice.is_global` + `:global` parsing in `pkg_manifest_read`'s `:spices`
  loop. Declaring `:global` together with `:url` or `:path` is a manifest
  error: they name different resolution sources, and a silent precedence rule
  is worse than a diagnostic. That error goes through `diag_emit`, not
  `fprintf` -- `pkg_manifest_read` judges the read by `diag_had_error()`, so a
  bare stderr write would have left the manifest ACCEPTED with a message
  nobody acted on.
- `tur_installed_spice_dir` (global.c) resolves an installed spice's source
  dir by name from `state.tur`, mirroring `tur list`'s reconstruction
  (`--path` install -> the local checkout; git install ->
  `spices/<name>-<sanitized-ref>` plus any `:subdir`).
- **Four** resolution ladders had to learn it, not one: `resolve_spice_dep_dir`
  in pkg.c, two in `main.c` (the recursive include-path collector and the
  `:c-sources` propagator), and the SC5 project-mode include builder. Each had
  its own copy of the workspace-sibling -> `:path` -> `spices/<name>-<ref>`
  ladder. A `:global` entry resolves from the registry **and nowhere else** --
  falling back to a project-local guess would silently use a different spice
  than the manifest asked for.
- `tur fetch` skips a `:global` dep (nothing to fetch, no lock row -- the same
  treatment `:path` gets) and reports an uninstalled one as a hard error naming
  `tur install`, rather than leaving it to surface later as
  `module 'notebook/core' not found`.

Pinned by four new cases in `tests/run-install.sh` (34 passing): the end-to-end
build against an installed spice, the fetch no-op, the uninstalled-spice error,
and the `:global` + `:url` rejection.

### Not done

- **`:global-policy`.** Auto-install-on-missing is a policy decision with real
  consequences (a build that installs software), and the error path is now
  clear enough to work without it.
- **Version validation and the `tur.lock` SHA.** The registry records a
  `:version` and, for git installs, a resolved SHA, but nothing yet checks a
  requested range against it or writes the SHA into `tur.lock`. Worth doing
  when `:global` deps grow a version constraint syntax -- as it stands there is
  no range to validate against.
- **Library-only spices still cannot be installed.** `tur install` requires at
  least one `:bin` entry, so a `:global` dep must name a spice that also ships
  a binary. That is a `tur install` limitation this change did not touch; it is
  called out in the guide.  *Closed 2026-10-22 by
  [installed-spice-libraries-plan](installed-spice-libraries-plan.md) (IL1):
  a spice with a `src/` and no `:bin` installs as a library.*

## Guides updated

- `docs/guides/developing-spices-guide.md` -- "Global Spices as Libraries" is
  no longer "(v2)"; it documents what resolves, what is not fetched, the two
  error cases, the `:bin` requirement, and what is still missing.
- `docs/guides/consuming-spices-guide.md` -- a "Globally installed spices"
  subsection next to local-path spices.
