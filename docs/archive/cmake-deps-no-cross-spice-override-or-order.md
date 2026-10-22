# A spice cannot override an option on, or be configured after, a `:cmake-deps` entry that another spice owns

> **Resolved 2026-10-21.** Both gaps are closed in
> `pkg_collect_transitive_cmake_deps` (`src/compiler/pkg.c`), which now keeps
> the `:spices` graph it walks instead of a flat visited list:
>
> - **Order.** Entries are collected in post-order: a spice's `:spices`
>   closure before the spice, the root manifest last, so a library's
>   `if (TARGET ...)` probe sees what a dependency spice declared. The link
>   manifest (`spice-deps-manifest.json`, whose entry order is the link order)
>   is written the other way round, groups highest first, so a dependent's
>   archives still precede the archives they need. Within one manifest the
>   declared order stands for both.
> - **Override.** Every value an option key is given is recorded with the
>   manifest that gave it, and the key is settled after the whole graph is
>   merged: the winning value is the one whose manifest depends on every
>   manifest that set a different value. A spice overrides its dependency,
>   the root overrides anything, and the root (or any common dependent) can
>   settle a conflict between two unrelated spices by setting the option
>   itself. Two unrelated spices that disagree with nothing above them still
>   fail, naming both. The rule is the "dependent beats dependency" direction
>   below; no `:override` key was needed.
>
> Pinned by `tests/run-transitive-cmake-deps.sh`:
> `cmake-deps-dependent-overrides-option`, `cmake-deps-root-overrides-option`
> and `cmake-deps-dependencies-configure-first` (the three-library repro
> below, which now prints `top: found target mid`); `cmake-deps-option-conflict`
> still pins the unrelated-spices error. Found on the way and fixed in the
> same change: two spices naming one local library by `:path` always
> conflicted, because the incoming path was compared as written against the
> stored, absolutized one (`cmake-deps-shared-path-dep-merges`). Documented in
> [developing-spices-guide](../guides/developing-spices-guide.md#native-dependencies-shared-across-spices).

**Severity:** low (expressiveness hole; nothing is broken today). Two related
gaps in how `tur` merges native dependencies across spices. Each forces a
spice that builds on a shared native library to re-declare that library itself
rather than depend on the spice that owns it.

Found 2026-10-17 while weighing a separate `nng-wss` spice against turning TLS
on inside `tur-nng`
([nng-tls-plan](../upcoming/spices/nng-tls-plan.md), "Why not a separate
spice"). The separate spice needed both of the gaps below closed, and the plan
chose the in-spice route because they are not.

## Gap 1: no way to override a value another spice set

nng only grows `wss://` when it is *built* with `NNG_ENABLE_TLS=ON`. A spice
that wants that has to change the options of the `nng` entry the `nng` spice
declares. It can re-declare `"nng"` with the same `:url` / `:ref` and its own
`:options`, and since 2026-10-21 the two copies are merged key by key
([cmake-deps-duplicate-keeps-first-options](../archive/cmake-deps-duplicate-keeps-first-options.md)).
That union cannot settle it:
`NNG_ENABLE_TLS` is `OFF` in one copy and `ON` in the other, a true conflict,
and nothing says which spice should win.

[transitive-cmake-deps-plan](../archive/history/transitive-cmake-deps-plan.md)
(open question 4) anticipated this: "an explicit `:override` field in the
enclosing manifest's own `:cmake-deps` could force a choice". It was never
built.

Other cases with the same shape: a spice that wants raylib with a different
`GRAPHICS` API than the `raylib` spice picks, or mbedTLS with a non-default
`mbedtls_config.h` feature.

## Gap 2: a spice's own entries are configured before its dependencies' entries

`pkg_gen_cmake_deps` emits entries in collection order, and collection puts
the root manifest's own `:cmake-deps` first (`pkg.c:4012`, "Root manifest's
own deps go first"), then the walk over `:spices`. In the generated
`CMakeLists.txt`, then, a spice's own native libraries are added before those
of the spices it depends on.

That breaks the common CMake idiom where a library reuses a dependency already
in the project. nng's `src/supplemental/tls/mbedtls/CMakeLists.txt` does
`if (TARGET mbedtls) ... else() find_package(MbedTLS REQUIRED)`. If `tur-nng`
got mbedTLS by declaring the `tls` spice in `:spices`, nng would be configured
first, see no `mbedtls` target, and go looking for an installed mbedTLS. A
spice can only get the configure order it needs by declaring the shared
library itself, ahead of its own entry, which is what nng-tls-plan does.

Seen on `main` 3d9e02a2 (tur v0.65.5) with the three-library repro from
[cmake-dep-link-walk-stops-at-archive](../archive/cmake-dep-link-walk-stops-at-archive.md),
rearranged: the `midcore` entry moves into a spice `midspice`, and the app
declares `"top"` itself plus `:spices #map{"midspice" #map{:path
"../midspice"}}`. The generated `cmake/CMakeLists.txt` adds `top` (line 79)
before `midcore` (line 83), and `top`'s configure prints
`top: no target mid yet` from an `if (TARGET mid)` probe. (The program still
links, because the final link order happens to come out right. nng would have
fallen through to `find_package(MbedTLS REQUIRED)` instead.) Running it needs
`midspice/cmake/` to exist first:
[cmake-dep-path-through-missing-cmake-dir](../archive/cmake-dep-path-through-missing-cmake-dir.md).

## Fix directions

- **Override.** Give precedence to the dependent over the dependency: a
  spice's value for a key beats the value from any spice in its own `:spices`
  closure, and the root beats everyone. Or add an explicit per-entry
  `:override true` that only a manifest closer to the root may use. Either
  way, two unrelated spices that disagree still fail with both origins named.
- **Order.** Emit dependencies first: walk a spice's `:spices` closure before
  appending its own entries (post-order), keeping the "root wins" rule for
  conflicts separate from the emission order. A per-entry `:after ["mbedtls"]`
  key is the explicit alternative.

Closing both would make a separate opt-in spice for a native feature
(`nng-wss`, a TLS-enabled build of some other library) a manifest-only job.
It would also let `tur-nng` drop TLS from its default build again, since TLS
becomes something a consumer opts into rather than something every nng user
links.
