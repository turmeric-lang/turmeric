---
title: Tur New Guide
category: CLI Tools
description: Scaffold a new Turmeric spice (library or binary) with standard layout, build manifest, tests, and optional CI
tags:
  - tur-new
  - scaffolding
  - spices
  - project-creation
  - ci
---

# tur new -- spice scaffolder

`tur new` creates a new Turmeric spice (library or binary) with a
standard directory layout, build manifest, test file, README, `.gitignore`,
standard `Justfile`, and optional CI workflow.

## Quick start

```sh
tur new my-spice              # scaffold ./my-spice/ (library)
tur new my-spice --kind bin   # binary spice
cd my-spice && tur run        # list available tasks
tur run build                 # build the spice
tur run test                  # run tests
```

## Generated layout

```
my-spice/
  build.tur                   -- manifest: :name, :version, :spices []
  tur.lock                    -- dependency lockfile
  Justfile                    -- standard recipe set (unless --no-justfile)
  README.md                   -- one-paragraph description + recipe hint
  .gitignore                  -- build/, .tur-cache/, *.o, etc.
  LICENSE                     -- only if --license != none
  src/
    my_spice.tur              -- module skeleton with ;;; module docstring
  tests/
    my_spice_test.tur         -- one passing test that imports the module
  .github/workflows/
    ci.yml                    -- runs `tur run ci` on ubuntu + macos
                                 (omitted with --no-ci)
```

For `--kind bin`, `src/my_spice.tur` is replaced with a `defn main [] :int`
entry point.

## Usage

```sh
tur new <name> [options]
tur new --here [options]
```

## Options

| Flag | Description |
|------|-------------|
| `--kind lib\|bin` | Library (default) or binary spice |
| `--bin`, `--lib` | Shorthand for `--kind` |
| `--author "Name <e@x>"` | Author string (default: `git config user.name/email`) |
| `--license SPDX` | Write `LICENSE` file (`MIT`, `Apache-2.0`, `BSD-3-Clause`, `none`) |
| `--no-git` | Skip `git init` and initial commit |
| `--no-ci` | Skip `.github/workflows/ci.yml` |
| `--no-justfile` | Skip the standard `Justfile` template |
| `--dry-run` | Print the file list that would be created; do not write |
| `--here` | Scaffold into the current directory (must not have `build.tur`) |

## Exit codes

| Code | Meaning |
|------|---------|
| 0 | Spice scaffolded successfully |
| 1 | Target directory exists and is non-empty, or I/O error |
| 2 | CLI / validation error (invalid name, unknown flag, conflicting flags) |

## Spice-name rules

Names must match `[a-z][a-z0-9-]*` (lowercase letters, digits, and hyphens;
2-64 characters; must start with a letter). The same rules apply to the
`:name` field in `build.tur`.

Reserved names (`tur`, `build`, `test`) are rejected.

## Author resolution

1. `--author "Name <email>"`
2. `GIT_AUTHOR_NAME` + `GIT_AUTHOR_EMAIL` environment variables
3. `git config user.name` + `git config user.email`
4. Placeholder `"Anonymous <unknown@local>"` (with a warning)

The scaffolded files always parse and build regardless of the author value.

## Adding the Justfile to an existing spice

```sh
cd existing-spice && tur run --init
```

This writes the same template as `tur new`. If a `Justfile` already exists,
the command refuses to overwrite and suggests `--force` or manual merging.

## Standard recipes

Every spice scaffolded by `tur new` supports these recipes:

| Recipe | Description |
|--------|-------------|
| `build` | Debug build |
| `release` | Optimized build |
| `test` | Run the test suite (depends on `build`) |
| `watch` | Re-build on source changes |
| `clean` | Remove build artifacts |
| `docs` | Generate API docs from `;;;` docstrings |
| `fmt` | Format sources in place |
| `check` | Type-check + format-check |
| `tag VERSION` | Create a release tag |
| `install` | Install into the local registry |
| `ci` | Full CI: clean + check + test + docs |

## See also

- `tur run --help` -- Justfile task runner
- `tur run --init` -- add the Justfile to an existing spice
- [tur-run-guide.md](tur-run-guide.md) -- task runner guide
