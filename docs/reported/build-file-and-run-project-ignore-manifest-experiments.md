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
