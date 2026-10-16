# `--no-color` is documented but rejected in every position

**Severity:** low. `tur --help` and the shell completions advertise
`--no-color`, but no command accepts it.

## Repro

```
$ tur check f.tur --no-color; echo $?
2
$ tur --no-color check f.tur; echo $?
64
```

## Root cause

`src/main.c` lists `--no-color` in its help text and has a handler for it
("Phase 8: Handle --no-color flag"), and `src/cli/completion.c` offers it. But
the global strip loop never removes it, so the subcommand's parser sees an
unknown flag (trailing position), and command resolution sees it as the
command name (leading position).

## Fix directions

- Strip `--no-color` with the other global flags and set the diagnostics color
  mode from it. Add a `run-flags.sh` case for both positions.
