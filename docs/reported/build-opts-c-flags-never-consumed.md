# `:build-opts :c-flags` (and `:no-stdlib`) are parsed but never used

**Narrowed 2026-10-17:** `:c-flags` now reaches the compile.
`append_manifest_link_flags` (`src/main.c`) appends it verbatim ahead of the
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
(`append_manifest_link_flags` in `src/main.c`) appends `:c-includes`,
`:c-sources` and the link flags only.

[developing-spices-guide](../guides/developing-spices-guide.md) (the
`-x c++` advice) and
[package-management-guide](../guides/package-management-guide.md) document
`:c-flags` as working.

## Fix directions

- Append `:c-flags` in the same helper (or a sibling) on every build path
  that compiles a project's C, and honour `:no-stdlib`. Alternatively, reject
  the keys until they are implemented.
