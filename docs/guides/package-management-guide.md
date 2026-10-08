---
title: Package Management Guide (Spice)
category: Package Management
description: Creating projects, adding spices, `build.tur`, `tur.lock`, CLI reference
---

# Turmeric Package Management Guide (Spice)

Turmeric's package manager is called **Spice**. Every project has a single
`build.tur` manifest at its root. The `tur` CLI handles creating projects,
adding dependencies, fetching sources, and building -- no separate package
manager binary is needed.

This guide covers:

1. Creating a new project
2. The `build.tur` manifest
3. Adding dependencies (spices)
4. The lock file (`tur.lock`)
5. Building and running
6. Project directory layout
7. C/CMake dependencies
8. Common CLI reference
9. Module integration
10. Versioning and security

---

## Creating a New Project

```sh
tur new my-app          # executable project (new directory)
tur new my-lib --lib    # library project (new directory)
tur init                # scaffold inside the current directory
tur init --lib          # library, inside the current directory
tur new my-app --no-git # skip automatic git init
```

`tur new <name>` creates the directory and scaffolds it. `tur init` does the
same thing inside an existing empty directory (following the same convention
as `git init`).

**Name rules:** must match `[a-z][a-z0-9-]*`. Names starting with a digit or
containing uppercase letters are rejected with a suggestion.

### Files created

```
my-app/
  build.tur     -- package manifest
  tur.lock      -- empty lock file (commit to VCS)
  src/
    main.tur    -- hello-world entry point (executable)
  tests/
    my-app_test.tur
  .gitignore    -- ignores build/, spices/, caches, generated cmake files
  README.md
  LICENSE
  Justfile
  .github/workflows/ci.yml
```

For `--lib`, `src/main.tur` is replaced by `src/<name>.tur` containing a
`defmodule` with a stub exported function.

### Generated `build.tur`

```
(defpackage my-app
  :name    "my-app"
  :version "0.1.0")
```

### Generated `src/main.tur`

```turmeric
;;; my-app -- entry point.
;;
(defn main [] :int
  (println "Hello from my-app!")
  0)
```

```sweet-exp
;;; my-app -- entry point.
;;
defn main [] :int
  println("Hello from my-app!")
  0
```

After scaffolding, run the project immediately:

```sh
cd my-app
tur run
```

---

## The `build.tur` Manifest

Every Turmeric project has one `build.tur` at its root. It is a valid
Turmeric source file evaluated at build time. The top-level form is
`defpackage`.

> **Map slots take `#map{...}`, not bare `{...}`.** `:spices`, `:cmake-deps`,
> `:options`, `:build-opts`, `:bin`, and `:exports` are maps, and so is each
> per-entry value. A bare `{...}` is SRFI-105 curly-infix arithmetic in every
> dialect, so it is a hard parse error in a manifest -- the compiler says
> ``:spices must be a map -- use `#map{...}` ``. The older `#{...}` spelling is
> equally valid and is what `tur add` and the `tur.lock` writer emit; `#map{...}`
> is the canonical one to type.
>
> Manifest snippets in the guides are checked, not just proofread:
> `tools/check-guide-pairs.py` shape-checks every fenced block in `README.md`
> and `docs/guides/` whose first form is `(defpackage ...)` by running
> `tur fetch --dry-run` over it. A ```` ```turmeric no-check ```` fence does
> **not** opt out of that -- `no-check` only means "this block has no sweet-exp
> companion", which is true of nearly every manifest snippet. Use
> ```` ```turmeric no-manifest-check ```` for a snippet that is deliberately not
> a valid manifest. The `tur.lock` examples below keep `#{...}` on purpose: that
> is what the lockfile writer emits.

### Full example

```turmeric no-check
;;; build.tur -- project manifest for "geom"
(defpackage geom
  :name        "geom"
  :version     "0.2.1"
  :description "2D/3D geometry library for Turmeric"
  :license     "MIT"
  :authors     ["Alice Smith <alice@example.com>"]
  :repository  "https://github.com/alice/tur-geom"

  ;; Which `tur` compiler versions this package's source is valid under
  :tur-version ">=0.32.2"

  ;; Turmeric package dependencies
  ;; (first-party spices from https://github.com/turmeric-lang/turmeric-spices)
  :spices #map{
    "math"  #map{:url    "https://github.com/turmeric-lang/turmeric-spices"
                 :ref    "math-v0.1.0"
                 :subdir "spices/math"}
    "test"  #map{:url    "https://github.com/turmeric-lang/turmeric-spices"
                 :ref    "test-v0.1.0"
                 :subdir "spices/test"
                 :optional true}
    "utils" #map{:path "../tur-utils"}   ; local dev path
  }

  ;; C/C++ packages (CPM-compatible)
  :cmake-deps #map{
    "raylib" #map{:url     "https://github.com/raysan5/raylib"
                  :ref     "5.0"
                  :options #map{:BUILD_SHARED_LIBS "OFF"
                                :BUILD_EXAMPLES   "OFF"}}
    "cjson"  #map{:url "https://github.com/DaveGamble/cJSON"
                  :ref "v1.7.16"}
  }

  ;; Compiler and C toolchain options
  :build-opts #map{
    :c-flags   ["-O3" "-DGEOM_PRECISION=f64"]
    :link-libs ["m"]
    :no-stdlib false
  }

  ;; What this package exports to consumers
  :exports #map{
    "geom/vector" ["vector-2d" "vector-3d" "cross-product"]
    "geom/matrix" ["matrix-2x2" "matrix-3x3" "multiply"]
  })
```

```sweet-exp
;;; build.tur -- project manifest for "geom"
defpackage geom
  :name        "geom"
  :version     "0.2.1"
  :description "2D/3D geometry library for Turmeric"
  :license     "MIT"
  :authors     ["Alice Smith <alice@example.com>"]
  :repository  "https://github.com/alice/tur-geom"

  ;; Which `tur` compiler versions this package's source is valid under
  :tur-version ">=0.32.2"

  ;; Turmeric package dependencies
  ;; (first-party spices from https://github.com/turmeric-lang/turmeric-spices)
  :spices #map{
    "math"  #map{:url    "https://github.com/turmeric-lang/turmeric-spices"
                 :ref    "math-v0.1.0"
                 :subdir "spices/math"}
    "test"  #map{:url    "https://github.com/turmeric-lang/turmeric-spices"
                 :ref    "test-v0.1.0"
                 :subdir "spices/test"
                 :optional true}
    "utils" #map{:path "../tur-utils"}   ; local dev path
  }

  ;; C/C++ packages (CPM-compatible)
  :cmake-deps #map{
    "raylib" #map{:url     "https://github.com/raysan5/raylib"
                  :ref     "5.0"
                  :options #map{:BUILD_SHARED_LIBS "OFF"
                                :BUILD_EXAMPLES   "OFF"}}
    "cjson"  #map{:url "https://github.com/DaveGamble/cJSON"
                  :ref "v1.7.16"}
  }

  ;; Compiler and C toolchain options
  :build-opts #map{
    :c-flags   ["-O3" "-DGEOM_PRECISION=f64"]
    :link-libs ["m"]
    :no-stdlib false
  }

  ;; What this package exports to consumers
  :exports #map{
    "geom/vector" ["vector-2d" "vector-3d" "cross-product"]
    "geom/matrix" ["matrix-2x2" "matrix-3x3" "multiply"]
  }
```

### Minimal manifests

A library with one Turmeric dependency:

```turmeric no-check
(defpackage my-lib
  :name    "my-lib"
  :version "0.1.0"
  :spices  #map{"core" #map{:url "https://github.com/turm/tur-core" :ref "v1.0.0"}})
```

```sweet-exp
defpackage my-lib
  :name    "my-lib"
  :version "0.1.0"
  :spices  #map{"core" #map{:url "https://github.com/turm/tur-core" :ref "v1.0.0"}}
```

A standalone binary with no dependencies:

```turmeric
(defpackage hello
  :name    "hello"
  :version "0.1.0")
```

```sweet-exp
(defpackage hello :name "hello" :version "0.1.0")
```

### `:tur-version` -- compiler compatibility

`:version` is this package's own version. `:tur-version` is a separate,
optional key naming which **`tur` compiler versions this package's source is
valid under** -- a comma-separated range such as `">=0.32.2"` or
`">=0.32.2, <0.35.0"`. A compiler below the floor is `TUR-E0621` (a hard
error), a malformed range is `TUR-E0622`, and a compiler above a declared
ceiling is `TUR-W0623` (a warning). Declare it whenever the package adopts
version-dependent syntax, an `:experiments` entry, or a new manifest key. See
[Declaring a compiler version range](developing-spices-guide.md#declaring-a-compiler-version-range-tur-version)
in the developing-spices guide for the full syntax and the caret rule.

---

## Adding Dependencies (Spices)

Dependencies in Turmeric are called *spices*. Use `tur add` to add one; it
updates both `build.tur` and `tur.lock` for you.

### Adding from a Git URL

```sh
tur add https://github.com/alice/tur-geom
tur add https://github.com/alice/tur-geom --ref v0.2.1
```

The spice name defaults to the last path component of the URL with any `tur-`
prefix stripped (`tur-geom` becomes `geom`). Override with `--name`:

```sh
tur add https://github.com/alice/tur-geom --ref v0.2.1 --name geometry
```

If `--ref` is omitted, the tool resolves to the default branch HEAD and warns:

```
Warning: no --ref specified; will resolve to HEAD.
Pin with: tur add https://github.com/alice/tur-geom --ref <tag-or-sha>
```

### Adding a local path dependency

```
tur add ../tur-utils --path
```

Use this for monorepo workspaces or while actively developing a dependency.
Local path spices are never written to `tur.lock`.

### Official first-party spices

The [turmeric-spices](https://github.com/turmeric-lang/turmeric-spices) monorepo
contains the official first-party spice library: `tur-test`, `tur-math`,
`tur-sqlite`, `tur-raylib`, `tur-json`, `tur-http`, and `tur-regex`.

Add any of them with `tur add` using the `:subdir` key:

```sh
tur add https://github.com/turmeric-lang/turmeric-spices \
  --ref http-v0.1.0 --subdir spices/http --name http

tur add https://github.com/turmeric-lang/turmeric-spices \
  --ref json-v0.1.0 --subdir spices/json --name json
```

### Adding from the Spice registry (future)

```sh
tur add spice/http
tur add spice/json
```

The official registry at `pkg.turmeric-lang.org` is not yet live. Until it
is, `tur add spice/<pkg>` prints:

```
The Spice registry is not yet available.
Add the package directly with a Git URL:
  tur add https://github.com/turmeric-spice/tur-http --ref v0.1.0
```

### What `tur add` changes

Before:

```
(defpackage my-app
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

### Error messages

| Condition | Message |
|---|---|
| Not in a project directory | `No build.tur found. Run tur new <name> to create a project.` |
| Spice already present | `'geom' is already a dependency. To change its ref, edit the :spices entry for 'geom' in build.tur, then run tur fetch --update.` |
| Clone or ref failure | `spice: git failed for 'geom' ref 'v99.0.0' in 'spices'` |
| SHA mismatch on re-fetch | `spice: SHA mismatch detected -- run tur fetch --update to re-fetch` |

---

## The Lock File (`tur.lock`)

`tur.lock` records the exact resolved commit SHA and SHA-256 hash for every
fetched spice. It uses the same Turmeric S-expression syntax as `build.tur`
so it can be parsed by the same reader and diffed cleanly in version control.

```turmeric no-check
;;; tur.lock -- generated by tur. Do not edit by hand.
;;; Commit this file to version control for reproducible builds.

(deflockfile
  :format-version 1
  :spices #{
    "geom" #{:url        "https://github.com/alice/tur-geom"
             :ref        "v0.2.1"
             :resolved   "a1b2c3d4e5f6..."   ;;; full commit SHA
             :sha256     "abc123..."
             :fetched-at "2026-05-14T09:00:00Z"}
    "math" #{:url        "https://github.com/turmeric-lang/turmeric-spices"
             :ref        "math-v0.1.0"
             :resolved   "d6e7f8a9b0c1..."
             :sha256     "def456..."
             :fetched-at "2026-05-14T09:00:03Z"}
  }
  :cmake-deps #{
    "raylib" #{:url      "https://github.com/raysan5/raylib"
               :ref      "5.0"
               :resolved "5.0"
               :sha256   "ghi789..."}
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
             :resolved   "a1b2c3d4e5f6..."   ;;; full commit SHA
             :sha256     "abc123..."
             :fetched-at "2026-05-14T09:00:00Z"}
    "math" #{:url        "https://github.com/turmeric-lang/turmeric-spices"
             :ref        "math-v0.1.0"
             :resolved   "d6e7f8a9b0c1..."
             :sha256     "def456..."
             :fetched-at "2026-05-14T09:00:03Z"}
  }
  :cmake-deps #{
    "raylib" #{:url      "https://github.com/raysan5/raylib"
               :ref      "5.0"
               :resolved "5.0"
               :sha256   "ghi789..."}
  }
```

**Rules:**

- Always commit `tur.lock` to version control.
- `tur fetch` creates or updates the lock file.
- `tur build` uses the lock file and will not silently upgrade versions.
- Local `:path` spices are not recorded in the lock file.
- Builds fail if a fetched spice's hash does not match the lock file entry.

---

## Building and Running

### Run the project

```sh
tur run                   # debug build + run
tur run --release         # release build + run
tur run --offline         # skip network; fail if any spice is missing
tur run -- --flag arg     # pass arguments to the binary
tur run src/other.tur     # run a specific file (no build.tur required)
```

`tur run` walks up from the current directory to find `build.tur`, resolves
dependencies, compiles, and executes the binary. The exit code matches the
binary's exit code.

### Entry point resolution

| Condition | Entry file |
|---|---|
| `build.tur` has `:entry "src/foo.tur"` | `src/foo.tur` |
| `src/main.tur` exists | `src/main.tur` |
| Single `*.tur` file in `src/` | that file |
| None of the above | error with suggestion |

`:entry` is a path relative to the manifest directory (an absolute path is
honored as written):

```turmeric no-check
(defpackage app
  :name    "app"
  :version "0.1.0"
  :entry   "src/cli.tur")
```

The rungs are tried in order, so `:entry` wins even when `src/main.tur` also
exists. It is authoritative rather than a hint: a `:entry` that does not name
an existing file is a hard error, never a quiet fall-through to `src/main.tur`
-- running a different program than the manifest asked for is the failure mode
worth being loud about.

```
tur run: :entry "src/nope.tur" in build.tur does not name a file
  Looked for /home/alice/app/src/nope.tur
```

`:entry` applies to project-mode `tur run` (a bare `tur run` inside the
project). `tur run <file>` names its entry directly, and `tur build <dir>`
compiles every module under `src/` rather than picking one.

#### A program with no entry point

A single-file (or whole-program) build that defines no `main` and has no
top-level statements still builds: the compiler synthesizes an empty `main`
that runs static init and returns 0, so the program runs, prints nothing, and
exits 0. When a top-level function is spelled like an entry point but is not
one -- `-main` (the Clojure spelling), `main-`, `Main`, `_main` -- the build
warns at that definition:

```
warning [TUR-W0624]: no `main` and no top-level statements: the synthesized
entry point does nothing, so this program will run and print nothing.
`-main` is never called -- name it `main`, or call it from a top-level form
```

Rename it `main`, or call it from a top-level form. A module that is meant
to be loaded rather than run (every stdlib file, a spice's `src/`) has no
near-miss and stays quiet.


### Compile without running

```sh
tur build .            # project build (directory with a build.tur)
tur build src/foo.tur  # single-file build
```

### Fetch dependencies without building

```sh
tur fetch              # fetch all spices listed in tur.lock
tur fetch --update     # update spices to the latest allowed versions
```

`tur fetch`'s exit status separates "an optional dep was unavailable" from
"a required one was":

| Exit | Meaning |
| --- | --- |
| `0` | every dep fetched (or served from the lock) |
| `1` | only `:optional true` deps failed; they are skipped, the lock is written, and the project builds without them |
| `2` | a required dep or step failed: a `:spices` entry, a `:cmake-deps` build, the manifest, or the lock write |

A CI job can therefore warn-and-continue on `1` and fail on `2` rather than
treating every nonzero status the same way.

### Run the test suite

```sh
tur test tests/
```

`tur test <dir>` compiles and runs every `.tur` test file in the directory.

See [test-runner-contract.md](test-runner-contract.md) for the test framework
API.

---

## Project Directory Layout

```turmeric no-check
my-project/
  build.tur              -- package manifest (written by the author)
  tur.lock               -- reproducibility lock (committed to VCS)
  src/
    main.tur
    util.tur
  spices/                -- fetched spice sources (gitignored)
    math-v1.5.2/
      build.tur
      src/
    test-v0.3.0/
      build.tur
      src/
  cmake/                 -- generated CMake helpers (generated parts gitignored)
    CMakeLists.txt       -- generated from :cmake-deps by tur fetch
  build/                 -- build artifacts (gitignored)
```

```sweet-exp
my-project/
  build.tur              -- package manifest (written by the author)
  tur.lock               -- reproducibility lock (committed to VCS)
  src/
    main.tur
    util.tur
  spices/                -- fetched spice sources (gitignored)
    math-v1.5.2/
      build.tur
      src/
    test-v0.3.0/
      build.tur
      src/
  cmake/                 -- generated CMake helpers (generated parts gitignored)
    CMakeLists.txt       -- generated from :cmake-deps by tur fetch
  build/                 -- build artifacts (gitignored)
```

Recommended `.gitignore`:

```turmeric
build/
spices/
.tur-cache/
.tur-repl-cache/
cmake/CMakeLists.txt
cmake/build/
cmake/spice-deps-manifest.json
*.o
```

```sweet-exp
build/
spices/
.tur-cache/
.tur-repl-cache/
cmake/CMakeLists.txt
cmake/build/
cmake/spice-deps-manifest.json
*.o
```

---

## C/CMake Dependencies

The `:cmake-deps` block declares C and C++ packages to link against.
`tur fetch` (and the fetch step of `tur run`) generates `cmake/CMakeLists.txt`
from this block and invokes CMake automatically -- no CMake files need to be
written by hand.

```turmeric no-check
:cmake-deps #map{
  "raylib"  #map{:url     "https://github.com/raysan5/raylib"
                 :ref     "5.0"
                 :options #map{:BUILD_SHARED_LIBS "OFF"
                               :BUILD_EXAMPLES   "OFF"}}

  "sqlite3" #map{:url "https://github.com/sqlite/sqlite"
                 :ref "version-3.45.0"}
}
```

```sweet-exp
:cmake-deps #map{
  "raylib"  #map{:url     "https://github.com/raysan5/raylib"
                 :ref     "5.0"
                 :options #map{:BUILD_SHARED_LIBS "OFF"
                               :BUILD_EXAMPLES   "OFF"}}

  "sqlite3" #map{:url "https://github.com/sqlite/sqlite"
                 :ref "version-3.45.0"}
}
```

Add a CMake dependency from the command line:

```sh
tur add-cmake https://github.com/raysan5/raylib --ref 5.0
```

The entry goes into `:cmake-deps` instead of `:spices`.

For projects that need direct control of the CMake build, see
[cmake-cpm-integration-plan.md](https://github.com/turmeric-lang/turmeric/blob/main/docs/archive/cmake-cpm-integration-plan.md).

---

## Common CLI Reference

```sh
# Project creation
tur new <name>             # create a new executable project
tur new <name> --lib       # create a new library project
tur init                   # scaffold inside the current directory

# Dependencies
tur add <url>              # add a spice from a Git URL
tur add <url> --ref <ref>  # pin to a specific tag, branch, or SHA
tur add <path> --path      # add a local path dependency
tur add-cmake <url>        # add a C/CMake dependency
tur fetch                  # download all spices from tur.lock
tur fetch --update         # update to latest allowed versions
tur audit                  # list every origin the build fetches code from

# Build and run
tur build <dir>            # compile a project directory
tur build <file.tur>       # compile a single file
tur run                    # compile and execute
tur run --release          # compile (release) and execute
tur run --offline          # run without network access
tur test <dir>             # run all .tur test files in a directory

# Diagnostics
tur emit-c src/main.tur    # print generated C to stdout
```

---

## Module Integration

Spice names map directly to module import paths. Given a spice named `geom`
that exports `geom/vector` and `geom/matrix`:

```turmeric
;; In my-app/src/main.tur
(import geom/vector :refer [vector-2d cross-product])
(import geom/matrix :as mat)
```

```sweet-exp
;; In my-app/src/main.tur
import geom/vector :refer [vector-2d cross-product]
import geom/matrix :as mat
```

The compiler resolves:

- `geom/vector` -> `spices/geom-v0.2.1/src/vector.tur`
- `geom/matrix` -> `spices/geom-v0.2.1/src/matrix.tur`

The module system handles namespacing within a single package; Spice handles
dependencies between packages. See [module-system-guide.md](module-system-guide.md)
for the full module system reference.

---

## Versioning and Security

### Semantic versioning

- Versions follow semver: `MAJOR.MINOR.PATCH`
- Git tags must be named `vMAJOR.MINOR.PATCH` (e.g., `v1.2.3`)
- Pre-releases: `v0.2.0-alpha.1`
- Future registry constraints: `^0.2.0`, `~1.5`, `1.2.3` (exact)
- Git-URL spices always pin to the exact `:ref`; no range resolution

### Security guarantees

- All fetched spices are verified against the SHA-256 hash in `tur.lock`.
- Builds fail if the hash does not match; no silent re-downloads.
- GPG-signed Git tags are supported (verification is opt-in in v1).
- Source-only distribution -- no precompiled binaries in the lock file.

### Conflict resolution

- Two spices requiring incompatible versions of a third spice cause a build
  error with a clear diagnostic.
- The user must either upgrade one spice or pin versions to a compatible
  range.
- There is no silent multiple-version shadowing.

### Private repositories

SSH URLs work with standard Git credential helpers:

```sh
tur add git@github.com:myorg/private-lib.git --ref v1.0.0
```

Set `GIT_SSH_COMMAND` in your environment to control the SSH client used
during fetch.
