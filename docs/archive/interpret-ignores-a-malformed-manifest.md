# `tur --interpret` ran the program anyway on a malformed `build.tur`

**Severity:** low-medium. Not a wrong answer, but a parity gap that hid a real
configuration error.

**RESOLVED 2026-10-21.** Filed 2026-09-16 on the branch
`claude/lang-layers-decommission-v2nb9m`, which never merged; re-checked and
fixed on `main`.

## Repro

```sh
mkdir -p /tmp/p2
printf '(defpackage broken :name\n'   > /tmp/p2/build.tur   # unterminated list
printf '(defn main [] : int 0)\n'     > /tmp/p2/input.tur

./build/tur check       /tmp/p2/input.tur   # error ... TUR-E0624; exit 1
./build/tur --interpret /tmp/p2/input.tur   # error: unterminated list; exit 0
```

`tur run --engine=interp` and `tur debug` took the same path. On the original
branch the interpreter did not read the manifest at all; by 2026-10-21 it did
(for `:experiments` and for spice discovery) and printed the reader's error --
twice -- but still ran the program.

## Why it mattered

The manifest is what puts the spice's own `src/` and its `:spices` deps on the
module search path. The compiled entry points refuse to continue on one they
cannot read (TUR-E0624, `pkg_manifest_reassert` in `compile_to_c`), so a typo
in `build.tur` is reported as itself. Under the interpreter it surfaced as an
unrelated "unknown name" further down, if at all.

## Fix

`cmd_eval_h` (`src/driver/cmd_eval.c`) checks `pkg_manifest_malformed()` after the
`:experiments` read, which is the first read of the manifest on that path, and
refuses the same way `compile_to_c` does: `diag_reset()`, then
`pkg_manifest_reassert()`, exit 1. The sticky verdict is reset first, because
the DAP server launches one program after another in the same process. As on
the compiled path, `--no-auto-spice` does not suppress it -- the `:experiments`
read still finds the manifest. Each error now prints once.

The `diag_reset()` is load-bearing, not tidy: see
[manifest-read-leaves-a-dangling-file-registration](../reported/manifest-read-leaves-a-dangling-file-registration.md).

Pinned by `tests/fixtures/errors/lang-trailing-token-manifest`, which runs on
both engines (it carried `requires.compiled` on the original branch for
exactly this gap).
