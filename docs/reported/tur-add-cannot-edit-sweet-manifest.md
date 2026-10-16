# `tur add` cannot edit a `build.tur.sweet` manifest

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
