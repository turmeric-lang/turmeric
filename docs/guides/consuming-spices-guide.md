---
title: Consuming Spices Guide
category: Package Management
description: Adding, fetching, and using spice packages in a Turmeric project
---

# Consuming Spices Guide

A *spice* is a Turmeric package that your project depends on. This guide
covers how to find, add, fetch, and import spices -- including pure-Turmeric
spices and C library bindings via `:cmake-deps`.

See [Developing Spices](developing-spices-guide.md) if you want to create
and publish a spice of your own.

---

## Official First-Party Spices

The canonical source for official spices is the
[turmeric-spices](https://github.com/turmeric-lang/turmeric-spices) monorepo.
Its packages include:

| Spice | Description | C deps? |
|---|---|---|
| `tur-test` | Testing framework: describe/it/TAP output | None |
| `tur-math` | 2D/3D vector and matrix math | None |
| `tur-sqlite` | SQLite3 bindings | SQLite amalgamation |
| `tur-raylib` | Raylib graphics and input | Raylib |
| `tur-json` | JSON parsing and serialization | yyjson |
| `tur-http` | Async HTTP/HTTPS client | mbedTLS |
| `tur-regex` | PCRE2 regular expression bindings | PCRE2 |

The web stack (`tur-httpd`, `tur-template`, `tur-tourist`, `tur-tls`,
`tur-ws-client`/`tur-ws-server`) and the data/DSP spices (`tur-frame`,
`tur-stats`, `tur-signal`, `tur-ecs`, ...) live there too -- see their
per-spice guides. Each spice lives in its own subdirectory and is versioned
independently with a per-package tag: `<spice>-vMAJOR.MINOR.PATCH`.

---

## Adding Spices

### Official spices from turmeric-spices

Use `tur add` with `--subdir` to pull a spice from the monorepo:

```sh
tur add https://github.com/turmeric-lang/turmeric-spices \
  --ref test-v0.1.0 --subdir spices/test --name test

tur add https://github.com/turmeric-lang/turmeric-spices \
  --ref math-v0.1.0 --subdir spices/math --name math

tur add https://github.com/turmeric-lang/turmeric-spices \
  --ref sqlite-v0.1.0 --subdir spices/sqlite --name sqlite

tur add https://github.com/turmeric-lang/turmeric-spices \
  --ref json-v0.1.0 --subdir spices/json --name json
```

Each command updates `build.tur` and `tur.lock` automatically.

### Third-party spices from a Git URL

```sh
tur add https://github.com/alice/tur-geom --ref v0.2.1
```

The spice name defaults to the last path segment with any `tur-` prefix
stripped (`tur-geom` becomes `geom`). Override with `--name`:

```sh
tur add https://github.com/alice/tur-geom --ref v0.2.1 --name geometry
```

If `--ref` is omitted the tool resolves to HEAD and warns:

```
Warning: no --ref specified; resolved to HEAD (a1b2c3d4).
Pin with: tur add https://github.com/alice/tur-geom --ref a1b2c3d4
```

Always pin with an explicit tag or commit SHA to keep builds reproducible.

### Local path spices (development)

```sh
tur add ../tur-utils --path
```

Use this while actively developing a dependency alongside your project.
Local path spices are not recorded in `tur.lock` and are never fetched
from the network.

### Globally installed spices

A spice you installed with `tur install` can be consumed as a library by
declaring it `:global` -- there is no `tur add` flag for this yet, so write the
entry by hand:

```turmeric
:spices #map{
  "notebook" #map{:global true}
}
```

It resolves through the install registry rather than `<project>/spices/`, so
nothing is fetched and no `tur.lock` row is written (as with a `:path` dep). If
the spice is not installed, `tur fetch` says so and fails rather than letting
the build reach `module not found`. `:global` takes neither `:url` nor `:path`
-- those name a different resolution source. See
[Global Spices as Libraries](developing-spices-guide.md#global-spices-as-libraries)
for the full rules.

---

## What `tur add` Changes

Before adding any spice:

```turmeric
(defpackage my-app
  :name    "my-app"
  :version "0.1.0")
```

```sweet-exp
defpackage(my-app
  :name    "my-app"
  :version "0.1.0")
```

After `tur add https://github.com/alice/tur-geom --ref v0.2.1`:

```turmeric no-check
(defpackage my-app
  :name    "my-app"
  :version "0.1.0"
  :spices #map{
    "geom" #map{:url "https://github.com/alice/tur-geom"
                :ref "v0.2.1"}
  })
```

```sweet-exp
defpackage my-app
  :name    "my-app"
  :version "0.1.0"
  :spices #map{
    "geom" #map{:url "https://github.com/alice/tur-geom"
                :ref "v0.2.1"}
  }
```

---

## Fetching and Updating

```sh
tur fetch               # download everything in tur.lock, at the pinned commits
tur fetch --update      # upgrade spices to the latest allowed versions
tur fetch --frozen      # what CI should run: fail if tur.lock would change
```

`tur run` and `tur build` invoke `tur fetch` automatically when any spice
is missing. Pass `--offline` to skip the network and fail if a spice is
absent:

```sh
tur run --offline
```

---

## The Lock File

`tur.lock` records the resolved commit SHA and SHA-256 hash for every fetched
spice. Always commit it to version control; it is what makes builds
reproducible across machines and CI runs.

```turmeric no-check
;;; tur.lock -- generated by tur. Do not edit by hand.
;;; Commit this file to version control for reproducible builds.

(deflockfile
  :format-version 1
  :spices #{
    "geom" #{:url        "https://github.com/alice/tur-geom"
             :ref        "v0.2.1"
             :resolved   "a1b2c3d4e5f6..."
             :sha256     "abc123..."
             :fetched-at "2026-05-22T09:00:00Z"}
    "math" #{:url        "https://github.com/turmeric-lang/turmeric-spices"
             :ref        "math-v0.1.0"
             :subdir     "spices/math"
             :resolved   "d6e7f8a9b0c1..."
             :sha256     "def456..."
             :fetched-at "2026-05-22T09:00:03Z"}
  })
```

```sweet-exp
;;; tur.lock -- generated by tur. Do not edit by hand.
;;; Commit this file to version control for reproducible builds.

deflockfile
  :format-version 1
  :spices #{
    "geom" #{:url        "https://github.com/alice/tur-geom"
             :ref        "v0.2.1"
             :resolved   "a1b2c3d4e5f6..."
             :sha256     "abc123..."
             :fetched-at "2026-05-22T09:00:00Z"}
    "math" #{:url        "https://github.com/turmeric-lang/turmeric-spices"
             :ref        "math-v0.1.0"
             :subdir     "spices/math"
             :resolved   "d6e7f8a9b0c1..."
             :sha256     "def456..."
             :fetched-at "2026-05-22T09:00:03Z"}
  }
```

Rules:
- `tur fetch` creates or updates `tur.lock`.
- `tur build` uses the lock and will not silently upgrade versions.
- Local `:path` spices are not recorded in the lock file.
- Builds fail when a fetched spice's hash does not match the lock entry.

---

## Importing Spice Modules

Spice names map directly to import paths. A spice named `geom` that exports
`geom/vector` and `geom/matrix` is imported like any other module:

```turmeric
(import geom/vector :refer [vector-2d cross-product])
(import geom/matrix :as mat)

(defn main [] :int
  (let [v (vector-2d 1.0 2.0)
        m (mat/mat4-identity)]
    (println v)
    0))
```

```sweet-exp
import geom/vector :refer [vector-2d cross-product]
import geom/matrix :as mat

defn main [] :int
  let [v (vector-2d 1.0 2.0)
       m mat/mat4-identity()]
    println(v)
    0
```

The compiler resolves `geom/vector` to `spices/geom-v0.2.1/src/vector.tur`.
No extra configuration is needed.

---

## Optional Spices

Mark a spice `:optional true` when it is only needed for tests or dev
tooling. Optional spices that are absent do not cause a build error.

```turmeric no-check
:spices #map{
  "test" #map{:url    "https://github.com/turmeric-lang/turmeric-spices"
              :ref    "test-v0.1.0"
              :subdir "spices/test"
              :optional true}
}
```

```sweet-exp
:spices #map{
  "test" #map{:url    "https://github.com/turmeric-lang/turmeric-spices"
              :ref    "test-v0.1.0"
              :subdir "spices/test"
              :optional true}
}
```

When `tur test` is run, optional spices are fetched so test files can import
them. When `tur build` is run in production, they are skipped.

An optional spice that cannot be fetched is reported and skipped: `tur fetch`
still writes `tur.lock` and exits `1`, distinct from the `2` a required
spice's failure earns (and the `0` of a clean fetch), so a CI job can warn on
the first and fail on the second. See the exit-status table in
[package-management-guide.md](package-management-guide.md#fetch-dependencies-without-building).

---

## C/CMake Dependencies

Some spices (like `tur-sqlite` or `tur-raylib`) wrap a C library. When you
add them, `tur build` fetches and compiles the C library via CMake
automatically -- no CMake knowledge required on your end.

You can also add a C library directly to your project without going through
a spice:

```sh
tur add-cmake https://github.com/raysan5/raylib --ref 5.5 \
  --opt BUILD_SHARED_LIBS=OFF --opt BUILD_EXAMPLES=OFF
```

This appends a `:cmake-deps` entry to `build.tur`:

```turmeric no-check
(defpackage my-app
  :name    "my-app"
  :version "0.1.0"
  :cmake-deps #map{
    "raylib" #map{:url     "https://github.com/raysan5/raylib"
                  :ref     "5.5"
                  :options #map{:BUILD_SHARED_LIBS "OFF"
                                :BUILD_EXAMPLES   "OFF"}}
  })
```

```sweet-exp
defpackage my-app
  :name    "my-app"
  :version "0.1.0"
  :cmake-deps #map{
    "raylib" #map{:url     "https://github.com/raysan5/raylib"
                  :ref     "5.5"
                  :options #map{:BUILD_SHARED_LIBS "OFF"
                                :BUILD_EXAMPLES   "OFF"}}
  }
```

When `tur build` runs it:

1. Generates `cmake/CMakeLists.txt` from the `:cmake-deps` block (via the
   automatic `tur fetch`).
2. Invokes CMake to fetch and compile the C library.
3. Reads `cmake/spice-deps-manifest.json` for include dirs, lib dirs, link
   libs, and link flags.
4. Passes those flags to `cc` automatically.

Step 3's link flags are what make a Cocoa- or Objective-C-backed library (glfw,
raylib) link on macOS: they carry the `-framework Cocoa` / `-framework IOKit`
entries CMake records on the target, which cannot be spelled as `-l`. They also
carry the dep's artifact by full path, so a target whose name differs from its
library's (glfw's target `glfw` builds `libglfw3.a`) resolves correctly. Both
are derived automatically from `:targets`; see
[`:link-libs` and `:link-flags` overrides](developing-spices-guide.md#link-libs-and-link-flags-overrides)
for when you need to override them.

Declare the C symbols you need in Turmeric with `extern-c` (no header
include is needed for the declarations themselves; an inline-C body that
wants the header can hoist `#include <raylib.h>` to file scope with a
`__tur_include__` marker -- see the
[C integration guide](c-integration-guide.md)):

```turmeric
(extern-c InitWindow        [:int :int :cstr] :void)
(extern-c CloseWindow       [] :void)
(extern-c WindowShouldClose [] :bool)
(extern-c BeginDrawing      [] :void)
(extern-c EndDrawing        [] :void)

(defn main [] :int
  (InitWindow 800 600 "Hello")
  (while (not (WindowShouldClose))
    (BeginDrawing)
    (EndDrawing))
  (CloseWindow)
  0)
```

```sweet-exp
extern-c InitWindow        [:int :int :cstr] :void
extern-c CloseWindow       [] :void
extern-c WindowShouldClose [] :bool
extern-c BeginDrawing      [] :void
extern-c EndDrawing        [] :void

defn main [] :int
  InitWindow(800 600 "Hello")
  while not(WindowShouldClose())
    BeginDrawing()
    EndDrawing()
  CloseWindow()
  0
```

No `-I` or `-L` flags are needed; `tur build` injects them from the manifest.

CMake deps are also tracked in `tur.lock` with SHA-256 hashes for integrity
verification. For the generated `cmake/CMakeLists.txt` format, the manifest
schema, security considerations, and the outbound direction (publishing a
Turmeric library so CMake projects can consume it), see the
[CMake/CPM integration notes](https://github.com/turmeric-lang/turmeric/blob/main/docs/archive/cmake-cpm-integration-plan.md).

---

## Security

- **`tur.lock`'s hash is checked, not just recorded.** A fetch that brings back
  a tree differing from the recorded hash **fails**, naming both hashes and
  pointing at `tur fetch --update` as the deliberate way to accept the change.
  A refused fetch leaves the recorded hash alone, so the failure does not
  evaporate on the next run. `tur run`, `tur build` and `tur audit` all re-hash
  the trees they are about to use, so an edit made to `spices/` after a fetch
  is caught by whichever you reach for.
- **A fetch checks out the commit the lock recorded**, not wherever `:ref`
  points now. A branch-shaped `:ref` that has moved upstream still gives you
  the locked commit; `tur fetch --update` is how you take the new one. If the
  locked commit can no longer be fetched (the history was rewritten), the
  fetch fails and keeps no clone -- it does not fall back to the branch, which
  would quietly undo the pin. A tag is still the clearer `:ref`, and read a
  new spice before you add it. See the
  [Security Guide](https://github.com/turmeric-lang/turmeric/blob/main/docs/guides/security-guide.md)
  for the promise this is measured against.
- **`tur fetch --frozen`** fetches exactly what `tur.lock` pins and never
  writes it: a dependency with no row, or a `:url` / `:ref` / commit / tree
  that differs from its row, is an error. That is the `npm ci` /
  `cargo --locked` shape, and the one CI should run.
- Any `:cmake-deps` entry is a trust decision equivalent to executing build
  scripts from that repository. Audit before adding.
- `tur audit` lists every origin the build fetches code from -- Turmeric
  spices and cmake-deps alike -- with each one's ref and, where `tur.lock`
  has pinned it, the resolved commit and SHA-256. Origins with no lock entry
  are called out, so "what am I trusting, and is it pinned?" is one command
  rather than a manual reconciliation of `build.tur` against `tur.lock`.

  ```sh
  tur audit
  ```

  It also **verifies**: every dependency that is present on disk and pinned in
  `tur.lock` is re-hashed and compared, and the `Integrity:` section says
  whether they all match. What it still does not do is check a **signature** --
  there is no maintainer-key infrastructure, so the hashes tell you the tree is
  the one `tur.lock` recorded, not who wrote it. A `:path` dep is reported but
  not flagged as unpinned: it resolves from local source and has nothing to
  pin, and flagging it would train you to ignore the warning that matters.

---

## Conflict Resolution

When two spices require incompatible versions of a shared dependency the
build fails with a diagnostic. There is no silent multiple-version
shadowing. You must either upgrade one spice or pin versions to a compatible
range.

---

## Common Error Messages

| Condition | Message |
|---|---|
| No `build.tur` in the directory tree | `No build.tur found. Run tur new <name> to create a project.` |
| Spice already in the manifest | `'geom' is already a dependency. To change its ref, edit the :spices entry for 'geom' in build.tur, then run tur fetch --update.` |
| Network failure | `Failed to reach https://github.com/...: <reason>` |
| Ref not found | `Ref 'v99.0.0' not found in https://github.com/alice/tur-geom` |
| SHA mismatch on re-fetch | `Integrity check failed for 'geom'. Run tur fetch --force to re-download.` |

A spice can declare which compiler versions it supports with `:tur-version`; if
your `tur` is below that floor the build stops with `TUR-E0621` (above a
declared ceiling is `TUR-W0623`, a warning, and a malformed range in the spice's
own manifest is `TUR-E0622`) -- see
[Declaring a compiler version range](developing-spices-guide.md#declaring-a-compiler-version-range-tur-version).

---

## CLI Quick Reference

```sh
# Add a Turmeric spice
tur add <url>
tur add <url> --ref <tag-or-sha>
tur add <url> --ref <ref> --subdir <path> --name <alias>
tur add <path> --path

# Add a C/CMake dependency
tur add-cmake <url> --ref <ref>
tur add-cmake <url> --ref <ref> --opt KEY=VALUE

# Fetch and update
tur fetch
tur fetch --update

# Build and run
tur build
tur build --release
tur run
tur run --release
tur run --offline
tur test
```

---

## See Also

- [Package management guide](package-management-guide.md) -- full `build.tur` manifest reference and `tur` CLI
- [Developing spices](developing-spices-guide.md) -- creating and publishing your own spice
- [C integration guide](c-integration-guide.md) -- `extern-c`, `include-c`, inline-C blocks
- [Using a Turmeric library from CMake](using-turmeric-from-cmake.md) -- publishing a Turmeric library for C/C++ consumers
