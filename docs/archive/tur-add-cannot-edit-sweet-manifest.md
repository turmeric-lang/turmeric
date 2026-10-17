# `tur add` cannot edit a `build.tur.sweet` manifest

**RESOLVED 2026-10-17** -- see [Resolution](#resolution-2026-10-17). Pinned by
SW6 in `tests/run-sweet-manifest.sh`.

**Severity:** low. `tur init --sweet` creates a manifest that `tur add` then
cannot edit.

## Repro

```
$ tur init --sweet
$ tur add https://example.invalid/foo.git
<unknown>:1:1: error: unexpected character '#' (0x23)
tur add: failed to parse 'build.tur.sweet'
```

## Root cause

`src/compiler/pkg.c:pkg_build_tur_add_spice` re-reads the manifest text to
splice in the new entry. It neither strips the `#lang sweet-exp` line nor
selects the sweet-exp reader, as `pkg_manifest_read` does from the extension.

The investigation also suspects (not reproduced) that `collect_web_link_flags`
and `do_install_spec` hard-code `build.tur`, and that `pkg_fetch_all`'s check
for a root `:path` dependency looks only for `build.tur`.

## Fix directions

- Route every manifest read through the extension-aware reader, and write the
  new entry in sweet-exp syntax for a `.sweet` manifest (or splice it as an
  s-expression, which sweet-exp accepts).

## Resolution (2026-10-17)

Two defects, both fixed in `src/compiler/pkg.c`:

- The editor (`pkg_build_tur_edit`, shared by `tur add` and `tur add-cmake`)
  strips the `#lang` line and picks the reader from the file's extension. For a
  `.sweet` manifest it splices the new entry into the original text at the
  offset the sweet-exp source map gives (`pkg_sweet_splice_entry`), written as
  an s-expression, which sweet-exp accepts; the comments and layout stay.
- The manifest reader skipped every key written on its own indented line: the
  preprocessor wraps `  :spices ...` as `(:spices ...)`, so even a hand-written
  sweet `:spices` entry did nothing. `pkg_dp_unwrap_sweet_lines` flattens those
  one-element lists back into the `defpackage` before the keys are read.

The suspected hard-coded `build.tur` sites were real too and now go through
`pkg_resolve_manifest_path`: `collect_web_link_flags` (`src/main.c`), the
install reader and both listing paths in `src/compiler/install.c`, and
`pkg_fetch_all`'s check that a root `:path` dependency has a manifest.

Pinned by SW6 in `tests/run-sweet-manifest.sh`: `tur add --path` twice and
`tur add-cmake` twice on a `build.tur.sweet`, then `tur fetch --dry-run` must
see all four deps, each slot must appear once, and the comment must survive.
