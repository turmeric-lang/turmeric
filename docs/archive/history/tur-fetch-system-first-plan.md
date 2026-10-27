# `tur fetch` System-First Resolution -- Plan (SF0--SF4)

> **Status:** Implemented (SF0-SF3 in `tur`; SF4 docs landed, spice-repo
> migrations tracked separately since spices live in `../turmeric-spices`).
>
> **Flag:** None at the language level. Behaviour is opt-in per `:cmake-deps`
> entry; spices that do not opt in keep their current `FetchContent`-only
> codegen. `tur fetch --refetch` (or `TUR_FETCH_FORCE_FETCH=1`) forces the
> source-build path.
>
> **Last updated:** 2026-05-31
>
> **Related:**
> - `src/compiler/pkg.c` (lines 1668-1718) -- the `:cmake-deps` CMakeLists
>   generator that this plan extends.
> - `examples/snake/CMakeLists.txt` -- hand-written reference for the
>   `find_package(... QUIET)` -> `CPMAddPackage` fallback pattern.
> - `../turmeric-spices/spices/tls/build.tur` -- the first spice that
>   benefits from this; today it builds mbedTLS from source on every fresh
>   checkout (~1-2 min).

---

## Motivation

`tur fetch` currently emits a `FetchContent_Declare` + `FetchContent_MakeAvailable`
block for every `:cmake-deps` entry. The dep is always cloned from source
and built locally, even when a system package manager already provides a
compatible copy.

This is the wrong default for heavy native libraries:

| Dep    | System install                  | Source build (first fetch)    |
|--------|---------------------------------|-------------------------------|
| raylib | `brew install raylib`           | ~30s + ~5MB build dir          |
| mbedTLS| `brew install mbedtls`          | ~90s + ~30MB build dir         |
| sqlite | `brew install sqlite`           | ~20s + ~8MB build dir          |
| libpq  | `brew install libpq`            | (not currently fetchable)     |

Every clean `tur fetch` on a CI runner rebuilds these from scratch.
Developers who have already installed the system copy via Homebrew /
apt / dnf would prefer to skip the rebuild entirely.

The pattern is well-established outside of `tur fetch`:
`examples/snake/CMakeLists.txt` already uses
`find_package(raylib QUIET)` -> `CPMAddPackage` fallback by hand. The goal
of this plan is to lift that pattern into `tur fetch` codegen so every
spice benefits uniformly without each one re-implementing it.

---

## Non-goals

- **No system-only mode.** "Fail the build if no system copy is found" is
  out of scope; the user can always pre-install the system package. The
  point of the fallback is *resilience*, not enforcement.
- **No version negotiation.** The system copy is accepted as-is if
  `find_package` finds something matching the declared version range.
  Mixing system mbedTLS 3.5 with a spice pinned to 3.6 is the user's
  problem; we surface a CMake message but do not refuse.
- **No vcpkg / Conan / nix bridges.** This plan only covers CMake's
  built-in `find_package`. Adding other resolvers is a follow-up.
- **No header-only or pkg-config branches.** A future SF5 could add
  `pkg_check_modules` for libraries that do not ship `*Config.cmake`,
  but pkg-config is not part of this plan.

---

## Design

### Choice between three shapes

The user's two suggestions from the conversation that prompted this
plan:

1. **A flag is enough.** Add `:prefer-system true` to the dep map.
   Codegen wraps the existing FetchContent block in
   `if (NOT <name>_FOUND)` after a `find_package(... QUIET)`.
2. **An array of sources.** Add `:sources [...]` listing resolution
   strategies in order: `[system fetch]`, `[system local-clone fetch]`,
   etc. Each entry is a self-contained strategy with its own keys.

Plus a third for completeness:

3. **Implicit from `:cmake-name`.** If a dep declares `:cmake-name` AND
   `:url`, generate the hybrid block automatically. No new manifest
   key required; the presence of both signals "use one, fall back to
   the other."

#### Comparison

| Aspect                     | (1) `:prefer-system` flag | (2) `:sources` array | (3) implicit from `:cmake-name` |
|---------------------------|---------------------------|----------------------|---------------------------------|
| Manifest verbosity         | +1 key                    | nested arrays        | 0 (reuses `:cmake-name`)        |
| Discoverability            | obvious                   | obvious              | hidden behaviour                |
| Extensibility (vcpkg, etc) | needs more keys later     | strategies pluggable | locks us in                     |
| Order control              | fixed (system then fetch) | explicit             | fixed                           |
| Common case ergonomics     | one extra line            | nested map per strat | zero extra typing               |

**Recommendation: (1) `:prefer-system` flag, paired with the existing
`:cmake-name` and `:targets` keys.** Reasoning:

- The common case is binary: "try system, fall back to source." A
  boolean covers it without nested syntax.
- `:cmake-name` is already required for `find_package` to know what to
  look for; reusing it keeps the manifest minimal.
- Order is fixed because there is only one sensible order: cheap
  (system) before expensive (clone + build).
- If we later need pluggable resolvers (vcpkg, pkg-config, ...), we
  can graduate to `:sources` as a superset without breaking
  `:prefer-system`.

The hidden-behaviour option (3) was rejected because it makes a spice's
build behaviour depend on whether the author *happens* to set both
`:cmake-name` and `:url` -- two unrelated-looking keys that suddenly
interact. Explicit opt-in is safer.

### Manifest shape (final)

```turmeric
:cmake-deps #{
  "mbedtls" #{
    :prefer-system true              ;; NEW: try find_package first
    :cmake-name    "MbedTLS"         ;; existing -- name passed to find_package
    :cmake-version "3.0"             ;; NEW: optional minimum version
    :targets       ["MbedTLS::mbedtls"
                    "MbedTLS::mbedx509"
                    "MbedTLS::mbedcrypto"]  ;; existing
    :url           "https://github.com/Mbed-TLS/mbedtls"  ;; existing -- fallback
    :ref           "v3.6.2"                 ;; existing -- fallback
    :options       #{:ENABLE_PROGRAMS "OFF"
                    :USE_STATIC_MBEDTLS_LIBRARY "ON"}
  }
}
```

When `:prefer-system true`:

- Codegen emits `find_package(<cmake-name> [<cmake-version>] QUIET)` first.
- Wraps the existing `FetchContent_Declare`/`MakeAvailable` block in
  `if (NOT <cmake-name>_FOUND)`.
- The manifest-JSON section reads from `find_package` outputs (target
  properties via `$<TARGET_PROPERTY:...>` generator expressions) when
  the system path was taken, falls back to `${<name>_SOURCE_DIR}` /
  `${<name>_BINARY_DIR}` otherwise.

When `:prefer-system` is absent or `false`: behaviour is unchanged --
the existing FetchContent-only block is emitted.

### Generated CMakeLists fragment

```cmake
# mbedtls
find_package(MbedTLS 3.0 QUIET)
if (NOT MbedTLS_FOUND)
    FetchContent_Declare(mbedtls
      GIT_REPOSITORY https://github.com/Mbed-TLS/mbedtls
      GIT_TAG        v3.6.2
    )
    set(ENABLE_PROGRAMS OFF CACHE BOOL "" FORCE)
    set(USE_STATIC_MBEDTLS_LIBRARY ON CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(mbedtls)
    set(_mbedtls_resolved_via "fetch" CACHE INTERNAL "")
else()
    set(_mbedtls_resolved_via "system" CACHE INTERNAL "")
endif()
```

### Manifest JSON shape

`spice-deps-manifest.json` gains a `resolved_via` field per dep so the
downstream build can introspect which path was taken (useful for cache
keys, CI logging, and `tur run` reproducibility).

```json
{
  "mbedtls": {
    "resolved_via": "system",
    "include_dirs": ["/opt/homebrew/include"],
    "link_dirs":    ["/opt/homebrew/lib"],
    "link_libs":    ["mbedtls", "mbedx509", "mbedcrypto"]
  }
}
```

For the `fetch` branch, `include_dirs` and `link_dirs` come from the
existing FetchContent property logic. For the `system` branch, they
come from extracting `INTERFACE_INCLUDE_DIRECTORIES` and
`IMPORTED_LOCATION` on the first declared `:targets` entry.

### `tur.lock` interaction

The lockfile records `:resolved` and `:sha256` only for the `fetch`
path. When the system copy was used, the entry is written as:

```turmeric
"mbedtls" #{:resolved-via "system" :system-version "3.6.2"}
```

This lets reproducibility audits notice when a developer is testing
against a different system version than CI. `tur fetch --refetch` (a
SF4 follow-up) can force the fetch path to override the system copy
for one-off pinning.

---

## Phases

| Step | Task | Status |
|---|---|---|
| SF0 | Add `:prefer-system` and `:cmake-version` keys to the manifest parser (`pkg.c::parse_cmake_deps`). Pure data; no codegen change. Reject combinations that make no sense (`:prefer-system true` without `:cmake-name`). | Done |
| SF1 | Extend `pkg_gen_cmake_deps` to emit the hybrid `find_package` -> FetchContent block when `:prefer-system true`. Existing deps unchanged. | Done |
| SF2 | Extend the JSON-manifest generator to record `resolved_via` and to pull paths from system targets when `find_package` succeeded. | Done |
| SF3 | Extend `tur.lock` writer to record `:resolved-via` and `:system-version`. Add a `tur fetch --refetch` flag to bypass the system path. | Done |
| SF4 | Document the new keys in `docs/guides/developing-spices-guide.md`. Opt `spices/tls`, `spices/raylib` etc. into `:prefer-system` (in `../turmeric-spices`, tracked separately). | Docs done; spice migrations pending |

SF0-SF1 are the meaningful change; SF2-SF4 are polish and rollout.

### Implementation notes

- The system/fetch branch divergence (namespaced `MbedTLS::mbedtls` targets
  for the system import vs the unaliased `mbedtls` from the FetchContent
  build) is resolved at CMake configure time via a `_<name>_resolved_via`
  cache variable, so a single generated `CMakeLists.txt` handles both paths.
- `resolved_via` / `system_version` flow: `find_package` -> generated
  `spice-deps-manifest.json` -> `pkg_cmake_manifest_read` -> `tur.lock`.
- Coverage lives in `tests/spice-resolver-tests.sh` (cases `SF0`, `SF1/SF2`,
  `SF2b`, `SF3`), exercised hermetically with a fake `find_package` config so
  no network is required.
- Multi-dir system includes: a target's `INTERFACE_INCLUDE_DIRECTORIES` is a
  CMake `;`-list, so the system branch wraps each target's property in
  `$<JOIN:...,", ">` to rewrite the `;` separators into JSON array element
  boundaries at `file(GENERATE)` time. A package exporting several include
  dirs lands as `["d1", "d2"]` rather than a single `"d1;d2"` string that
  would otherwise produce a bogus `-Id1;d2` flag (regression test `SF2b`).

---

## Migration path

Existing spices keep working unchanged because `:prefer-system` is
opt-in. The recommended rollout order, easiest first:

1. **`spices/tls`** -- first user; the spice has not shipped, so there
   are no in-the-wild fetches to invalidate.
2. **`spices/raylib`** -- well-known system package on every platform;
   the snake example already proves the pattern works.
3. **`spices/sqlite`** -- ubiquitous system package, but the spice
   pins a specific source-built version for portability; opt-in here
   is a soft "warning, your version may differ" rather than a default.
4. **`spices/postgres`** -- libpq is reliably installed system-wide;
   add `:prefer-system true` to its existing `:cmake-name "PostgreSQL"`
   block.

Each migration is one PR adding `:prefer-system true` (plus
`:cmake-version` where appropriate) to one manifest.

---

## Open questions

- **Version mismatch policy.** If `find_package(MbedTLS 3.0)` succeeds
  but the user actually has 3.5 installed and the spice was tested
  against 3.6, do we warn? Proposed: emit a CMake `message(STATUS ...)`
  noting the system version vs the pinned fallback version, but never
  block. Loud-and-skippable beats quiet-but-occasionally-wrong.
- **Target name collisions.** If the system package exports
  `MbedTLS::mbedtls` and the FetchContent build exports the unaliased
  `mbedtls`, the spice's `target_link_libraries` lines need to know
  which to use. The cleanest fix is to *always* go through the
  manifest JSON's `link_libs` (already populated correctly per branch)
  rather than referencing CMake targets directly in spice code.
- **Static vs shared from system packages.** Homebrew mbedTLS ships
  shared `.dylib`; the source build is pinned to static `.a`. A
  binary built against the system copy and then deployed to a machine
  without mbedTLS installed will fail at runtime. Document this in
  the guide; a follow-up SF5 can add `:require-static true` that
  rejects the system path when it provides only shared libraries.
- **CI reproducibility.** CI without the system package falls through
  to fetch; CI with it gets system. Different artefacts, same source
  tree. Proposed: CI pipelines should set `TUR_FETCH_FORCE_FETCH=1`
  (or pass `--refetch` once that lands in SF3) to pin behaviour, with
  developer machines free to use whichever is fastest.
