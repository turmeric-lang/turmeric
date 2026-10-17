# A broken `build.tur` prints an error but `tur check` exits 0

**RESOLVED 2026-10-17** -- see [Resolution](#resolution-2026-10-17). Pinned by `tests/spice-resolver-tests.sh` (`malformed-manifest-unterminated-fails-tur-check`, `malformed-manifest-no-defpackage-fails-tur-check`).

**Severity:** medium. Scripts and CI that rely on the exit status pass a
project whose manifest does not parse.

## Repro

```
$ cat build.tur
(defpackage bad
  :name "bad"
  :version "0.1.0"
$ tur check src/main.tur; echo $?
build.tur:1:1: error: unterminated list (missing ')')
0
```

A manifest with no `defpackage` form behaves the same.

## Root cause

`src/compiler/pkg.c:pkg_manifest_read_status` returns early on a read error
and on a missing `defpackage` without marking the manifest malformed. Callers
treat "no manifest" as "not in a project" and carry on.

## Fix directions

- Mark the manifest malformed on both early-return paths so callers fail.
  Add a `run-flags.sh` case for each.

## Resolution (2026-10-17)

Both early returns in `pkg_manifest_read_status` (`src/compiler/pkg.c`) set
`PKG_MANIFEST_MALFORMED` and call `pkg_manifest_mark_malformed`, so the
sticky verdict is re-asserted after `diag_reset()` as TUR-E0624 and the
command exits 1. The cases went into `spice-resolver-tests.sh`, beside the
existing malformed-manifest cases, rather than `run-flags.sh`.
