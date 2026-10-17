# CMake's `::@(0x...)` directory markers print a "dropping link flag" warning on every build that links tur-nng

**Severity:** low (noise). Every program that links the turmeric-spices `nng`
spice prints five lines like:

```
spice: dropping link flag '::@(0x5627cb52d200)' from cmake/spice-deps-manifest.json -- it contains a character the shell would read as syntax
```

The token is harmless and was always going to be skipped. But the wording
describes it as possible shell injection, on every build, which teaches
readers to skip the one diagnostic that is meant to catch that.

Found 2026-10-17 while spiking [nng-tls-plan](../upcoming/spices/nng-tls-plan.md).

## Repro

In turmeric-spices (`main` fa42ece), with `tur` from `main` 3d9e02a2
(v0.65.5):

```sh
cd spices/nng
tur fetch
tur test tests/nng 2>&1 | grep -c 'dropping link flag'    # five per test program
```

With TLS turned on (nng-tls-plan) it is six per program: mbedTLS's interface
adds a marker of its own.

## Root cause

CMake wraps the `INTERFACE_LINK_LIBRARIES` entries that `target_link_libraries`
adds from a directory other than the target's own in `::@(<directory id>)` ...
`::@` markers. nng adds its platform libraries from subdirectories, so its
interface carries five. `_tur_walk_iface` (emitted by `emit_link_iface_helper`,
`src/compiler/pkg.c:3365`) passes any entry that is not a target through as a
string, markers included.

`pkg_cmake_manifest_append_cc_flags` (`pkg.c:4954`) then runs
`cmake_manifest_token_ok` on each link flag **before** `append_link_flag_token`.
The second already drops any token containing `::` silently, and
[nng-spice-plan](../archive/nng-spice-plan.md) ("Link flags") recorded the
markers as "Nothing to fix" on that basis. The shell-safety check runs first
now, sees the parentheses, and prints the warning before the token reaches the
silent skip.

## Fix directions

- Drop the markers at the source: in `_tur_walk_iface`, skip any entry
  matching `^::@`. The manifest then stops carrying them, which also removes
  the puzzle the nng plan noted for readers of `spice-deps-manifest.json`.
- Or, in `pkg_cmake_manifest_append_cc_flags`, skip `::` tokens before the
  shell-safety check, keeping the check for tokens that will actually be
  used.
