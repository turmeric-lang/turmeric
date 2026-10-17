# `tur jit <file>` does not discover spice includes

**RESOLVED 2026-10-17** -- see [Resolution](#resolution-2026-10-17). Pinned by `tests/run-engine-select.sh` (`jit-discovers-spice-includes`, JIT builds only).

**Severity:** low. An import that `tur run --engine jit` resolves fails under
`tur jit`.

## Repro

A spice with `src/demo/util.tur` and a file `other/main.tur` that imports
`demo/util`:

```
$ tur run --engine jit other/main.tur
42
$ tur jit other/main.tur
other/main.tur:2:3: error: module 'demo/util' not found
```

## Root cause

Per-file commands walk up to the enclosing `build.tur` and add the spice's
`src/` (and its deps') to the search path through
`auto_append_spice_includes` (CLAUDE.md "Per-file Commands Inside a Spice").
`src/main.c:cmd_jit` never calls it.

## Fix directions

- Call `auto_append_spice_includes` in `cmd_jit`, honouring `--no-auto-spice`,
  as the other per-file commands do.

## Resolution (2026-10-17)

`cmd_jit` calls `auto_append_spice_includes` before `compile_to_c` and sets
the LS2 resolver context around it, as `tur check` does. `--no-auto-spice` is
honoured inside the helper.
