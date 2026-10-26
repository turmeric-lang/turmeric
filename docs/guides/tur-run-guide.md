---
title: Tur Run Guide
category: CLI Tools
description: Built-in Justfile-compatible task runner for building, testing, and managing Turmeric projects without installing just
tags:
  - build-system
---

# tur run -- Justfile-compatible task runner

`tur run` is a built-in Justfile task runner. Any spice that ships a
`Justfile` can be driven by `tur run` without installing the upstream
`just` binary.

`tur just` is a synonym for `tur run` -- the alias resolves to the canonical
`run` command before dispatch, so **every** form on this page accepts either
spelling, including the classic `tur run <file.tur>` compile-and-run path.
Diagnostics still print the canonical `tur run:` prefix. Abbreviations work
too: `tur ju` and `tur ru` both land on `run`. (`tur jit` is a separate
command and is unaffected -- `jit` is not a prefix of `just`.)

## Quick start

```sh
tur run              # list recipes (or run 'default' if defined)
tur run build        # run the 'build' recipe
tur run test         # run the 'test' recipe (runs 'build' dep first)
tur run tag 0.2.0    # pass an argument to a recipe
```

## Creating a Justfile

```sh
tur run --init       # write a starter Justfile into the current directory
```

The generated `Justfile` contains the standard spice recipes: `build`,
`release`, `test`, `watch`, `clean`, `docs`, `fmt`, `check`, `tag`, `install`,
`ci`.  Every new spice created with `tur new` also gets this template.

## Listing recipes

```sh
tur run --list           # plain text
tur run --list --json    # machine-readable (editor integrations, completion)
tur run --list --all     # include hidden recipes
```

The listing includes **aliases** alongside recipes, since an alias is a name
you can actually run:

```
  build         # Build the spice (debug profile).
  b             # alias for `build`
```

It **excludes** recipes marked `[private]` and recipes whose name starts with
`_`, matching `just`. Hidden means hidden, not disabled: `tur run _helper`
still runs. Pass `--all` to list them.

A Justfile feature `tur run` does not support (see
[Deferred](#deferred-clear-error-with-upgrade-hint)) does **not** blank the
listing. The offending line is skipped, a note goes to stderr, and every
recipe that did parse is still listed -- so shell completion keeps working in
a project that also uses features only real `just` handles. Executing a recipe
out of such a Justfile is still a hard error.

The JSON form is the one to build tooling on. Each entry carries `name`, and
optionally `doc`, `params` (each with `name`, `default`, `variadic`), and
`alias` (the target recipe, for alias entries). All strings are properly
escaped.

## Flags

| Flag | Description |
|------|-------------|
| `--list`, `-l` | List all recipes with doc comment and parameters |
| `--all`, `-a` | With `--list`: also show `[private]` and `_`-prefixed recipes |
| `--json` | JSON output for `--list` |
| `--dry-run` | Print resolved commands; do not execute them |
| `--verbose` | Echo recipe metadata (start, end) to stderr |
| `--justfile PATH` | Use an explicit Justfile instead of searching upward |
| `--chdir DIR` | Change to DIR before running the recipe |
| `--init` | Write a starter Justfile into cwd |
| `--force` | Overwrite an existing Justfile (with `--init`) |

## Exit codes

| Code | Meaning |
|------|---------|
| 0 | Recipe succeeded |
| 1 | Recipe ran but exited non-zero (exit code propagated) |
| 2 | CLI / parse / unsupported-feature error |
| 127 | Justfile not found |

## Supported Justfile subset (v0.1.0)

`tur run` implements a subset of the Justfile syntax compatible with
upstream `just` 1.x. For any Justfile that uses only this subset,
`just <recipe>` and `tur run <recipe>` produce identical results.

### Included

- **Recipe definitions** with shell-command bodies (tab or 4-space indent)
- **Recipe dependencies**: `test: build`, including chained dependencies
  and dependencies-with-arguments: `test: (build "release")`
- **Recipe parameters** with optional defaults and variadic rest:
  `tag VERSION:`, `build mode='debug':`, `forward +ARGS:`
- **Variable assignment**: `name := "value"` and `export name := "value"`
- **Interpolation**: `{{ var }}` (and `{{var}}`) in recipe bodies and
  dep arguments
- **Line prefixes**: `@` (silent) and `-` (continue on non-zero exit);
  `@-` and `-@` both accepted
- **Comments**: `#` line comments. A run of `#` comments immediately
  above a recipe header becomes the recipe's **doc comment** shown in
  `tur run --list`
- **Settings**: `set shell := ["sh", "-c"]`, `set dotenv-load := true`,
  `set positional-arguments := true`, `set windows-shell := [...]`
- **Built-in functions** in interpolation:
  `env_var("NAME")`, `env_var_or_default("NAME", "fallback")`,
  `os()`, `os_family()`, `arch()`, `justfile_directory()`,
  `invocation_directory()`, `uppercase(s)`, `lowercase(s)`, `trim(s)`,
  `quote(s)`, `path_exists(p)`, `replace(s, from, to)`, `join(a, b)`,
  `error(msg)`
- **Backtick substitution**: `` x := `git rev-parse HEAD` `` in assignments,
  and in interpolation
- **Modules and imports**: `mod name` (from `name.just`, `name/mod.just` or
  `name/justfile`; run its recipes as `tur run name::recipe`) and
  `import 'file'`
- **Recipe attributes**: `[private]`, `[group('name')]` (grouped sections in
  `--list`), `[doc("...")]`, `[confirm]` / `[confirm("prompt")]`, `[no-cd]`,
  `[no-exit-message]`, and the platform selectors `[unix]`, `[linux]`,
  `[macos]`, `[windows]`, `[openbsd]`; comma-separated lists such as
  `[private, group('x')]` are accepted
- **Variable overrides**: `--set VAR VALUE` (or `--set VAR=VALUE`)
- **Aliases**: `alias b := build`, resolved on invocation and shown in
  `tur run --list`
- **Hidden recipes**: `[private]` and the `_name` convention both hide a
  recipe from `--list` while leaving it runnable by name
- **Conditional expressions** in assignment right-hand sides:
  `x := if os() == "macos" { "brew" } else { "apt" }`
- **Listing**: `tur run` with no recipe lists recipes; `tur run --list`
  is the explicit form
- **The `default` recipe**: if a recipe named `default` exists, `tur run`
  with no arguments runs it instead of listing

### Not supported (clear error with upgrade hint)

- Recipe attributes not listed above, such as `[parallel]`, `[script]` or
  `[extension(...)]`. An unknown attribute is refused, never silently
  skipped, because skipping one can change what a recipe does (an ignored
  `[confirm]` would drop the prompt guarding a destructive command).

When `tur run` runs a recipe out of a Justfile using an unsupported feature
it prints this and exits 2:

```
tur run: unsupported Justfile feature at Justfile:14: recipe attribute [parallel]
        Install `just` (https://just.systems) to run this recipe, or
        remove the [parallel] attribute if the recipe is portable.
```

Under `--list` the same text appears on stderr prefixed `tur run: note:`, and
the listing still prints the recipes that parsed.

## Shell completion

`tur completion` prints a completion script for `zsh` or `bash`. Both complete
subcommands, per-subcommand flags, and `.tur` file arguments -- and for
`tur run`, the recipe names of whatever Justfile encloses the directory you
are completing in, with their doc comments as descriptions.

```sh
# zsh -- install into your fpath
tur completion zsh > "${fpath[1]}/_tur" && compinit
# zsh -- or source it directly from ~/.zshrc
source <(tur completion zsh)

# bash
tur completion bash > /usr/local/etc/bash_completion.d/tur
source <(tur completion bash)          # or straight from ~/.bashrc
```

The Homebrew formula installs both automatically.

Recipe candidates come from `tur run --list`, so everything above applies:
aliases complete, hidden recipes do not, and a Justfile with unsupported
features still completes for the recipes that parse. `tur run <TAB>` also
offers `.tur` files, because `tur run` accepts either.

## Disambiguation with `tur run <file.tur>`

`tur run <file.tur>` (with a `.tur` or `.tur.sweet` extension) uses the
classic compile-and-run path. `tur run build` (no `.tur` extension)
invokes the Justfile task runner.

The words after the file are the program's `*args*`:
`tur run report.tur 2026 --csv` runs `report.tur` with `2026 --csv`.
`tur run`'s own flags (`-I`, `--release`, `--offline`, `--update`,
`--engine`) are still read on either side of the file, so a program that
takes a literal `--release` of its own gets it after `--`:
`tur run report.tur -- --release`.  `tur report.tur 2026 --csv` is the same
run, and is what a `#!/usr/bin/env tur` line uses.  (Before
project-less-tools PT2 the words after the file were dropped unless they
came after `--`.)

## Writing Justfiles for spices

Use only the supported subset so your spice works with both `tur run`
and upstream `just`. The template from `tur run --init` already does this.

### Shell injection

Recipe bodies are shell strings; `{{ var }}` substitutes without quoting.
This matches upstream `just` behaviour. Use `"{{ var }}"` in the body to
protect against values that contain spaces, or `{{ quote(var) }}` when the
value cannot be wrapped in double quotes.

### Dotenv loading

```just
set dotenv-load := true
```

Reads a `.env` file from the Justfile's directory and sets environment
variables (variables already in the environment win).

## Compatibility table

| Upstream `just` version | Compatible |
|-------------------------|-----------|
| 1.x (as of 2026-05) | For the supported subset above |

## See also

- `tur new --help` -- scaffold a new spice with the standard Justfile
- `tur run --init` -- add the standard Justfile to an existing spice
- [https://just.systems](https://just.systems) -- upstream `just` binary
