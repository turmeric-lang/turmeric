# Global flags after `--` are consumed by `tur` instead of passed to the program

**RESOLVED 2026-10-17** -- see [Resolution](#resolution-2026-10-17). Pinned by `tests/run-flags.sh` (`global-flags-stop-at-double-dash`).

**Severity:** low-medium. Arguments meant for the program change how it is
compiled, and the program does not receive them.

## Repro

```turmeric
(defn main [] : int
  (println (list-length *args*))
  0)
```

```
$ tur run nargs.tur -- a --strict-effects --no-contracts b
2
```

The program should receive four arguments. `--strict-effects` also takes
effect for the compile.

## Root cause

`src/main.c`'s global-flag strip loop and the `parse_*` helpers scan the whole
`argv` and do not stop at `--`.

## Fix directions

- Stop flag parsing at the first `--` in both the strip loop and the
  per-command parsers. Pass everything after it to the program verbatim.
- Add a `run-flags.sh` case.

## Resolution (2026-10-17)

`tur_main_inner` cuts `argc` at the first `--` while the `parse_*` helpers and
the strip loop run, and puts the tail back (the `--` included) afterwards.
The strip loop only moves words below the cut, so the tail is untouched in
between. The per-command parsers that forward arguments (`cmd_run`,
`cmd_jit`) already stopped at `--`. The repro's program now sees all four
arguments and the compile keeps its default flags.
