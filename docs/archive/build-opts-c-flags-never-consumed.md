# `:build-opts :c-flags` (and `:no-stdlib`) are parsed but never used

**Narrowed 2026-10-17:** `:c-flags` now reaches the compile.
`append_manifest_link_flags` (`src/driver/cc.c`) appends it verbatim ahead of the
link libraries, into the buffer every build path passes to the compile as well
as the link; `tests/spice-c-sources-tests.sh` ("c-flags") pins the repro below.
What is left is `:no-stdlib`, which is still parsed, validated and
round-tripped, and read by nothing. It has no defined meaning to implement:
decide what it should do (skip the auto-loaded prelude? link without
`libturi`?) or reject `:no-stdlib true` until it does something.

**Severity:** low now (was medium). A documented key does nothing,
silently.

## Repro

```turmeric
(defpackage cf
  :name "cf"
  :version "0.1.0"
  :build-opts #{:c-flags ["-DMY_ANSWER=42"]})
```

with `src/main.tur` returning `MY_ANSWER` from an inline-C body:

```
$ tur build .
error: 'MY_ANSWER' undeclared (first use in this function)
```

The same flag in `TUR_CC_FLAGS` builds.

## Root cause

`src/compiler/pkg.c` parses and validates `:c-flags` and `:no-stdlib` into the
manifest struct, but no build path reads them. The link-flag helper
(`append_manifest_link_flags` in `src/driver/cc.c`) appends `:c-includes`,
`:c-sources` and the link flags only.

[developing-spices-guide](../guides/developing-spices-guide.md) (the
`-x c++` advice) and
[package-management-guide](../guides/package-management-guide.md) document
`:c-flags` as working.

## Fix directions

- Append `:c-flags` in the same helper (or a sibling) on every build path
  that compiles a project's C, and honour `:no-stdlib`. Alternatively, reject
  the keys until they are implemented.

## Resolution (2026-10-21)

`:no-stdlib` is refused rather than implemented. The only stdlib switch the
compiler has, `--no-auto-stdlib`, is a stdlib-development tool (it skips the
auto-load list from the entry file onward when the entry is itself one of
those files), not "build without the stdlib", so there was no existing meaning
to wire the key to; building with no stdlib at all would be a new, partly
built feature, which belongs behind an experiment with a plan of its own.

- `src/compiler/pkg.c`: `:build-opts :no-stdlib` with any value but the
  literal `false` is a read error naming the key ("not implemented -- every
  build auto-loads the stdlib"), so the manifest is invalid and every command
  that reads it says so (TUR-E0624 for per-file commands). `false`, the
  documented default, is still accepted.
- `PkgManifest.no_stdlib` is gone, and the manifest writer no longer emits
  `:no-stdlib true`.
- `package-management-guide` no longer shows `:no-stdlib false` in its
  examples; `package-manager-internals-guide` describes the refusal and no
  longer says `:c-flags` is dropped (it reaches the compile since
  2026-10-17).

Pinned by `tests/spice-c-sources-tests.sh`, "no-stdlib": `true` fails the
build with the message, `false` builds and runs.
