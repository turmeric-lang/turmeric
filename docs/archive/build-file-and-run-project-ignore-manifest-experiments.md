# Some commands ignore the manifest's `:experiments`

**Severity:** medium. The same file in the same project compiles with an
experiment enabled under some commands and disabled under others.

## Repro

`build.tur`:

```turmeric
(defpackage xp
  :name "xp"
  :version "0.1.0"
  :experiments [reflected-measures])
```

`src/main.tur` uses `^reflect` (the `reflected-measures` experiment).

| Command | Experiment enabled? |
| --- | --- |
| `tur check src/main.tur` | yes |
| `tur run src/main.tur` | yes |
| `tur build .` | yes |
| `tur build src/main.tur` | **no**: warns "`^reflect` is ignored" |
| `tur compile src/main.tur` | **no** |
| `tur run` (project mode) | **no** |

## Root cause

`check`, `run <file>` and `build <dir>` get manifest experiments through
`discover_manifest_reader_macros` (which also wires reader macros). The
`build <file>` and `compile` arms of `src/main.c:tur_main_inner`, and
`cmd_run`'s project mode, read the manifest themselves and never apply
`:experiments`. CLAUDE.md's precedence rule (user file < manifest < CLI) is
therefore true only for some commands.

## Fix directions

- Route every command that compiles a file through the same manifest
  discovery (`discover_manifest_reader_macros`, or a shared helper that applies
  `:experiments`, reader macros and spice includes together).
- Add a fixture or `run-flags.sh` case per command.

## Resolution (2026-10-20)

Re-checked against `main` first: `tur run <file>` had already been moved onto
`discover_manifest_reader_macros` (its comment in `cmd_run` still describes the
hand-rolled copy it replaced), so the report's table was out of date for that
row, and it was short on the other side -- the interpreter's file arms skipped
`:experiments` too. Against a one-experiment project (`:experiments
[reflected-measures]`, a `^reflect` source) the commands that dropped the
gate were:

| Command | Before |
| --- | --- |
| `tur build src/main.tur` | gate off (`^reflect is ignored`) |
| `tur compile src/main.tur` | gate off |
| `tur run` (project mode) | gate off |
| `tur --interpret`, `interpret`, `debug`, `eval --file`, `trace` | gate off |

Everything else (`check`, `expand`, `emit-c`, `emit-h`, `run <file>`,
`build <dir>`, `jit`, `test`) already had it.

- **One path to the manifest.** `main.c:load_manifest_apply_experiments` finds
  the enclosing spice, reads its manifest and applies the user-level file
  (which a manifest `:experiments` key suppresses) and the manifest's own
  list. `discover_manifest_reader_macros` is that plus the reader-macro
  resolution, and `apply_enclosing_manifest_experiments` is the experiments
  half alone.
- **The arms.** `build <file>` and `compile` drop their hand-rolled
  `find_spice_root` + `pkg_manifest_read` + `resolve_manifest_reader_macros`
  walk for `discover_manifest_reader_macros`. Project-mode `run` already holds
  the parsed manifest, so it applies both lists from it directly. The
  interpreter's `cmd_eval_h` -- behind `--interpret`, `interpret`, `debug`,
  `eval --file`, `trace` and the DAP launch -- calls
  `apply_enclosing_manifest_experiments` before the env is built. It does not
  start honouring the manifest's `:reader-macros`; that is a separate feature,
  and these arms never had it.
- **Pinned** by sixteen cases in `tests/run-experiments-user-config.sh`. Each
  runs one command in a project whose manifest names an unknown experiment and
  expects TUR-E0310 with exit 2, which needs no live experiment (the registry
  has had none to probe with for most of its life). Eight of the sixteen fail
  without the fix.

Not covered: `tur repl` and `tur fmt`. `fmt` does not elaborate, so no gate
reaches it; the REPL discovers its spice by its own route (see
docs/guides/repl.md) and takes experiments from the session's own flags.
