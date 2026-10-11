---
title: Developing Spices Guide
category: Package Management
description: Creating, testing, and publishing Turmeric spice packages
---

# Developing Spices Guide

A *spice* is a Turmeric package that other projects depend on. This guide
covers every step of authoring one: project structure, declaring exports,
wrapping C libraries with `:cmake-deps`, testing, versioning, and publishing.

See [Consuming Spices](consuming-spices-guide.md) if you only need to add
an existing spice to your project.

---

## When to Create a Spice

Create a spice when you want to:

- Share a library across multiple projects.
- Wrap a C library and expose it as a clean Turmeric API.
- Contribute to the official
  [turmeric-spices](https://github.com/turmeric-lang/turmeric-spices) monorepo.
- Distribute a library that others can add with `tur add`.

For code shared only within a single project, use the module system -- no
separate package is needed.

---

## Scaffolding the Project

```sh
tur new tur-mylib --lib
cd tur-mylib
```

This creates:

```
tur-mylib/
  build.tur        -- package manifest
  tur.lock         -- empty lock file (commit to VCS)
  src/
    lib.tur        -- stub exported function
  .gitignore
  README.md
```

By convention, name the package with a `tur-` prefix. Consumers drop the
prefix as the import alias: `tur-geom` becomes `geom`, `tur-math` becomes
`math`.

---

## The `build.tur` Manifest

The manifest filename can be either `build.tur` (plain s-expression) or
`build.tur.sweet` (sweet-exp syntax). Both are equivalent everywhere the
toolchain consults the manifest -- walk-up discovery, workspace member
resolution, transitive `:spices` deps, and the cwd-relative `tur add` /
`tur fetch` paths all accept either. When both files exist in the same
directory the plain `build.tur` takes precedence. `tur init --sweet`
scaffolds the sweet variant.

A complete library manifest:

```turmeric no-check
(defpackage tur-mylib
  :name        "tur-mylib"
  :version     "0.1.0"
  :description "A brief description of my library"
  :license     "MIT"
  :authors     ["Your Name <you@example.com>"]
  :repository  "https://github.com/you/tur-mylib"

  :exports #map{
    "mylib/core" ["some-fn" "another-fn"]
    "mylib/util" ["helper-fn"]
  })
```

```sweet-exp
defpackage tur-mylib
  :name        "tur-mylib"
  :version     "0.1.0"
  :description "A brief description of my library"
  :license     "MIT"
  :authors     ["Your Name <you@example.com>"]
  :repository  "https://github.com/you/tur-mylib"

  :exports #map{
    "mylib/core" ["some-fn" "another-fn"]
    "mylib/util" ["helper-fn"]
  }
```

### Key manifest fields

| Field | Required | Notes |
|---|---|---|
| `:name` | Yes | Must match `[a-z][a-z0-9-]*` |
| `:version` | Yes | Semver: `MAJOR.MINOR.PATCH` |
| `:tur-version` | Recommended | Which **compiler** versions this spice works with -- see [Declaring a compiler version range](#declaring-a-compiler-version-range-tur-version) |
| `:description` | Recommended | One-line summary |
| `:license` | Recommended | SPDX identifier (e.g. `"MIT"`) |
| `:authors` | Recommended | `"Name <email>"` list |
| `:repository` | Recommended | URL to the canonical Git repo |
| `:exports` | Yes (library) | Map of module path to exported symbol names |
| `:spices` | If needed | Turmeric package dependencies |
| `:cmake-deps` | If needed | C/C++ library dependencies |
| `:build-opts` | Rarely | `:c-flags` / `:link-libs` / `:link-flags`, plus `:c-sources` / `:c-includes` for [vendored C](#vendoring-c-sources-c-sources--c-includes) |

---

## Declaring a compiler version range (`:tur-version`)

`:version` is *your* version. `:tur-version` is which **`tur` versions your
spice is valid under** -- a different question, and the one consumers hit:

```turmeric
(defpackage my-spice
  :version     "0.4.0"
  :tur-version ">=0.32.2"      ; needs the :sealed defopaque attribute
  ...)
```

Declare it whenever you adopt anything version-dependent: new syntax, a
`:experiments` entry, or a manifest key. Without it, a consumer on an older
compiler gets an error about *your source* -- a caret under a line that is
perfectly correct, with nothing suggesting an upgrade:

```
error: defopaque: unexpected attribute -- expected :linear or :affine
  |
1 | (defopaque RGWorld :int :sealed)
  |                         ^^^^^^^
```

With a floor declared, they get the actual problem instead (`TUR-E0621`), naming
the required range and the running version.

### Syntax

Comma-separated conjuncts; all must hold. Each is a comparator or a caret:

| Form | Means |
|---|---|
| `">=0.32.2"` | a floor -- the common case |
| `">=0.32.2, <0.35.0"` | floor and ceiling |
| `"^0.32.2"` | compatible-update range (see below) |
| `"0.32.2"` | exactly that version (rarely what you want) |

`~`, `*`, and `||` are **not** supported and are an error rather than a silent
no-op, so a typo cannot quietly become a different constraint.

**The caret's 0.x rule** is the part that surprises people. `^X.Y.Z` means "from
`X.Y.Z` up to the next version that could break you" -- and for a pre-1.0
version that is the next **minor**, because 0.x minors are breaking by
convention:

- `^0.32.2` admits `0.32.9`, excludes `0.33.0`
- `^1.2.3` admits `1.9.9`, excludes `2.0.0`

Since `tur` is pre-1.0, `^0.32.2` is a fairly tight constraint. Prefer a plain
floor (`">=0.32.2"`) unless you specifically want to exclude the next minor.

### Floors are errors, ceilings are warnings

| Situation | Result |
|---|---|
| Range satisfied | silent |
| Compiler **below** the floor | `TUR-E0621`, hard error, non-zero exit |
| Compiler **above** the ceiling | `TUR-W0623`, warning, build continues |
| Range malformed | `TUR-E0622`, hard error |

The asymmetry is deliberate. Below a floor, your code genuinely will not
work. Above a ceiling, it merely has not been *tested* -- which is usually
fine, and making it fatal would mean every compiler release breaks every spice
until each author bumps a number. So declare a ceiling to record what you
tested, not to prevent use.

### Note on adoption

The key can only diagnose skew against compilers that already know about it
(0.32.2+); older ones ignore unknown manifest keys silently. So declaring it
helps the *next* consumer, not the one already on an old compiler -- which is a
reason to add it early rather than when you first need it.

This is also distinct from tvm's `.tur-version` file, which pins *which*
compiler to install in a directory. `:tur-version` states which compilers your
source is valid under; a spice consumed as a dependency has no say over the
former.

---

## Declaring Exports

The `:exports` map controls what is visible to consumers. Only listed
symbols are part of the public API; everything else is private.

```turmeric no-check
:exports #map{
  "mylib/types" ["Coord" "Rect" "Color"]
  "mylib/draw"  ["draw-rect" "draw-circle" "draw-line"]
  "mylib/io"    ["read-file" "write-file"]
}
```

Each key (`"mylib/types"`) becomes an importable path for consumers:

```turmeric
(import mylib/types :refer [Coord Rect])
(import mylib/draw  :refer [draw-rect])
```

```sweet-exp
import mylib/types :refer [Coord Rect]
import mylib/draw  :refer [draw-rect]
```

Internal helpers that should not be exposed are simply omitted from
`:exports`.

---

## Source Layout

Follow this layout so that module paths and file paths align without
extra configuration:

```
tur-mylib/
  build.tur
  tur.lock
  src/
    types.tur       -- exports "mylib/types"
    draw.tur        -- exports "mylib/draw"
    io.tur          -- exports "mylib/io"
    internal/
      helpers.tur   -- not exported; only imported by other src files
  tests/
    types_test.tur
    draw_test.tur
```

The module path `"mylib/types"` resolves to `src/types.tur`. A nested path
`"mylib/net/http"` resolves to `src/net/http.tur`.

---

## Multi-file Spices: `defmodule` + `import`

When a spice spans more than one source file, use `defmodule` with an explicit
`(export ...)` list and `(import <other-module> :refer [...])` to wire the
files together. The module system handles cross-file symbol sharing correctly
at every scale.

### The correct approach

Each file declares its own `defmodule`, exports what it offers, and imports
what it needs from siblings:

```turmeric
;; src/mylib/types.tur
(defmodule mylib/types
  (export Widget WidgetResult)
  (defstruct Widget [value :int])
  ...)
```

```turmeric
;; src/mylib/core.tur
(defmodule mylib/core
  (export make-widget widget-value)
  (import mylib/types :refer [Widget WidgetResult])

  (defn make-widget [v :int] :Widget ...)
  (defn widget-value [w :Widget] :int ...))
```

`tur check`, `tur emit-c`, and `tur build` all resolve intra-spice imports
automatically through the auto-spice include path -- no `-I` flags needed.
See [Per-file Commands Inside a Spice](#per-file-commands-inside-a-spice).

### Anti-pattern: cross-file type stubs

An older pattern copies function bodies across files so each file compiles
in isolation without imports:

```turmeric
;; src/mylib/io.tur -- ANTI-PATTERN
;; Stub copy of make-widget from core.tur; real body is return NULL.
(defn make-widget [v :int] #fx{Unsafe} :Widget
  ```c
  return NULL;
  ```)

(defn read-widget [path :cstr] :Widget ...)  ;; real implementation
```

This pattern fails in three ways:

- **Silent breakage**: stubs with `return NULL` or no-op bodies mean any
  code path that hits the stub produces garbage or does nothing -- with no
  diagnostic.
- **Linker errors when combined**: two files that both define `make-widget`
  as a `static` C function produce duplicate-symbol link failures the moment
  they are compiled together (via `(load ...)` or `tur build`).
- **Per-file checks appear clean**: each file is self-contained so
  `tur check`/`tur emit-c` on a single file passes while the combined build
  fails.

If you encounter the stub pattern in an existing spice, the fix is
mechanical: add a `(defmodule ...)` + `(export ...)` header to each file and
replace each stub block with `(import <module> :refer [...])`.

---

## Depending on Other Spices

Add Turmeric spice dependencies the same way any project does:

```sh
tur add https://github.com/turmeric-lang/turmeric-spices \
  --ref math-v0.1.0 --subdir spices/math --name math
```

This produces:

```turmeric no-check
:spices #map{
  "math" #map{:url    "https://github.com/turmeric-lang/turmeric-spices"
              :ref    "math-v0.1.0"
              :subdir "spices/math"}
}
```

```sweet-exp
:spices #map{
  "math" #map{:url    "https://github.com/turmeric-lang/turmeric-spices"
              :ref    "math-v0.1.0"
              :subdir "spices/math"}
}
```

Mark spices that are only needed for tests `:optional true` so consumers
are not forced to fetch them:

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

---

## Cross-spice Development in a Workspace

When your spices live in the same workspace (a parent directory with a `build.tur`
that lists all members under `:members`), you can import one sibling from another
**without publishing a release or running `tur fetch`**.

### Option A: workspace-member auto-resolution (no manifest entry needed)

If both spices are already listed in the workspace `build.tur`:

```turmeric
;; turmeric-spices/build.tur
(defpackage turmeric-spices
  :members ["spices/watch" "spices/notebook" ...])
```

Then `notebook` can `(import watch/watch ...)` directly -- the resolver walks up
to the workspace `build.tur`, finds that `watch` is a listed member, and adds its
`src/` to the search path automatically.

```sh
# No tur fetch or symlink, and no lock entry required:
cd spices/notebook
tur check src/notebook/cli.tur   # resolves watch/watch via workspace
```

The first time an undeclared sibling import resolves, `tur` prints a one-time
advisory:

```
warning: import 'watch/watch' resolved via workspace sibling
         'spices/watch'; declare it in :spices for release builds.
         (set TUR_DEBUG_RESOLVER=1 for full resolver tracing)
```

To confirm a name is a workspace member before importing:

```sh
tur add --workspace watch   # exits 0, prints "no manifest entry needed"
tur add --workspace typo    # fails if 'typo' is not in :members
```

### Option B: explicit `:path` dep (works outside workspaces too)

Add a `:path` entry pointing at the sibling spice directory:

```sh
tur add ../watch --path
```

This writes to `build.tur`:

```turmeric no-check
:spices #map{
  "watch" #map{:path "../watch"}
}
```

```sweet-exp
:spices #map{
  "watch" #map{:path "../watch"}
}
```

The resolver immediately adds `../watch/src` to the include path.
No `tur fetch` step is required, and no lock entry is written for this dep.
The path is resolved relative to the directory containing `build.tur`.

```sh
tur check src/notebook/cli.tur   # resolves watch/watch via :path
```

If the declared `:path` does not exist on disk (or contains no `build.tur`),
`tur fetch` reports a hard error rather than silently ignoring the missing dep.

**Cycles between local spices are legal.** Two `:path`-local or
workspace-sibling spices may declare each other in `:spices`; this is supported,
not a diagnostic. The transitive include-path walk terminates on a visited set
keyed by each package root's `realpath`, so a mutual cycle is traversed once and
both spices' `src/` still land on the include path. Do not "fix" this shape into
an error.

### `tur fetch --dry-run` for verification

To confirm how each dep will be classified before committing to a real fetch:

```sh
tur fetch --dry-run
# skip :path          watch
# skip workspace member  alpha
# fetch  URL          ansi  https://github.com/...  ansi-v0.1.4
# summary: 1 fetch, 2 skipped (local)
```

Local-source deps are never contacted or locked. Only URL deps appear in `tur.lock`.

### Release: switch to a URL dep for external consumers

The workspace auto-resolution and `:path` entries are local-development shortcuts.
For a spice that will be published and consumed outside the workspace, add a URL
entry alongside (or instead of) the local one:

```turmeric no-check
:spices #map{
  "watch" #map{:url    "https://github.com/turmeric-lang/turmeric-spices"
               :ref    "watch-v0.1.0"
               :subdir "spices/watch"}
}
```

```sweet-exp
:spices #map{
  "watch" #map{:url    "https://github.com/turmeric-lang/turmeric-spices"
               :ref    "watch-v0.1.0"
               :subdir "spices/watch"}
}
```

External consumers run `tur fetch` once to clone the ref and populate `tur.lock`.
Inside the workspace, a URL `:spices` entry whose name matches a workspace member
is resolved locally instead -- no spurious fetch occurs for deps you are developing
in the same workspace.

---

## Wrapping a C Library with `:cmake-deps`

The `:cmake-deps` block tells `tur build` to fetch and compile a C library
via CMake, then inject the resulting include dirs and link flags into the
compilation step. You write only Turmeric; CMake is an implementation detail.

### Declaring the dependency

```turmeric no-check
:cmake-deps #map{
  "sqlite3" #map{:url     "https://github.com/sqlite/sqlite"
                 :ref     "version-3.47.2"
                 :options #map{:BUILD_SHARED_LIBS "OFF"}}
}
```

```sweet-exp
:cmake-deps #map{
  "sqlite3" #map{:url     "https://github.com/sqlite/sqlite"
                 :ref     "version-3.47.2"
                 :options #map{:BUILD_SHARED_LIBS "OFF"}}
}
```

`:options` sets CMake cache variables (`-DKEY=VALUE`). Common patterns:

- Disable shared libs: `:BUILD_SHARED_LIBS "OFF"`
- Disable examples or tests: `:BUILD_EXAMPLES "OFF"`, `:YYJSON_BUILD_TESTS "OFF"`
- Control feature sets: depends on the upstream library's own CMake options

### Declaring C symbols in Turmeric

Use `include-c` to pull in the C header and `extern-c` to declare symbols:

```turmeric
;; src/db.tur
(include-c "sqlite3.h")

(extern-c sqlite3_open   [:cstr :ptr] :int)
(extern-c sqlite3_close  [:ptr]       :int)
(extern-c sqlite3_exec   [:ptr :cstr :ptr :ptr :ptr] :int)
(extern-c sqlite3_errmsg [:ptr] :cstr)

(defn db-open [path :cstr] (result :ptr)
  (let [db-ptr (make-ptr)]
    (let [rc (sqlite3_open path db-ptr)]
      (if (= rc 0)
        (ok (deref-ptr db-ptr))
        (err (cstr->str (sqlite3_errmsg (deref-ptr db-ptr))))))))
```

```sweet-exp
;; src/db.tur
include-c "sqlite3.h"

extern-c sqlite3_open   [:cstr :ptr] :int
extern-c sqlite3_close  [:ptr]       :int
extern-c sqlite3_exec   [:ptr :cstr :ptr :ptr :ptr] :int
extern-c sqlite3_errmsg [:ptr] :cstr

defn db-open [path :cstr] (result :ptr)
  let [db-ptr (make-ptr)]
    let [rc (sqlite3_open path db-ptr)]
      if {rc = 0}
        ok(deref-ptr(db-ptr))
        err(cstr->str(sqlite3_errmsg(deref-ptr(db-ptr))))
```

`extern-c` trusts the signature you give it -- double-check it against the
actual C header. No `-I` or `-L` flags are needed in your source; `tur build`
injects them from `cmake/spice-deps-manifest.json`.

### Constants: `#define`s do not survive linking -- re-export them

A C library's `#define ZMQ_REP 4` / `SQLITE_OK 0` constants never reach a
symbol table, so neither `extern-c` nor the dynamic FFI can resolve them --
consumers of your spice (and REPL users) would otherwise have to restate
magic numbers. Wrap each constant the library's API needs as a plain
definition, **adjacent to the `extern-c` block it belongs to** so drift
against the upstream header stays reviewable in one place:

```turmeric no-check
;; sqlite3.h result codes the API surface uses (keep next to the externs).
(defn SQLITE-OK   [] :int 0)
(defn SQLITE-ROW  [] :int 100)
(defn SQLITE-DONE [] :int 101)
```

Exported like any other defn, these are callable from consumers and from
the REPL, and a header bump that renumbers something is a one-line,
reviewable diff instead of a scavenger hunt through call sites.

### Inline C for small wrappers

When a binding is simpler to write directly in C, use an inline-C block:

```turmeric
(defn db-last-insert-rowid [db :ptr] :int
  ```c
  return (int)sqlite3_last_insert_rowid((sqlite3*)db);
  ```)
```

The closing ` ``` ` and its enclosing `)` must be on the same line. See
the [inline-C style rule](https://github.com/turmeric-lang/turmeric/blob/main/CLAUDE.md) for why.

### `:cmake-name` and `:targets` overrides

When the CMake `find_package` name or target name differs from the key in
`:cmake-deps`, supply overrides:

```turmeric
:cmake-deps #map{
  "sqlite" #map{:url        "https://github.com/sqlite/sqlite"
                :ref        "version-3.47.2"
                :cmake-name "SQLite3"
                :targets    ["SQLite::SQLite3"]}
}
```

### How the link line is derived

When a dep declares `:targets`, `tur` asks CMake what each target actually is
rather than guessing from its name. Two generator expressions do the work, and
both are evaluated when `cmake/spice-deps-manifest.json` is generated:

- **`$<TARGET_FILE:tgt>`** -- the exact file the target produces, linked by
  full path. This resolves `OUTPUT_NAME` (glfw's target is `glfw`, its archive
  is `libglfw3.a`), namespace aliasing (`PostgreSQL::PostgreSQL` is `libpq`),
  and static-vs-shared preference (zlib builds `libz.a` and `libz.1.dylib`
  into one directory, where a `-lz` would pick the dylib) in one move. A
  header-only `INTERFACE` target has no artifact and contributes nothing here.
- **`$<TARGET_PROPERTY:tgt,INTERFACE_LINK_LIBRARIES>`** -- the target's
  transitive requirements, recorded in the manifest's `link_flags` array.
  This is where `-framework Cocoa` and `-framework IOKit` come from, which is
  what makes the Objective-C macOS backends of glfw and raylib link at all: a
  framework cannot be spelled as `-l`.

The `INTERFACE_LINK_LIBRARIES` walk happens **in CMake, at configure time**,
because its entries cannot be classified afterwards. A bare entry may be a
library name (`m`) or a target name (raylib's property lists `glfw`) -- the
same shape, opposite handling -- and only `if(TARGET ...)` can tell them apart.
The walk:

- unwraps `$<LINK_ONLY:...>` and skips any other generator expression;
- for an entry that is a target with a linkable artifact, takes
  `$<TARGET_FILE:...>` (plus an rpath if it is shared);
- for an entry that is a target *without* one -- an `OBJECT_LIBRARY` or
  `INTERFACE_LIBRARY` -- **recurses into its requirements** rather than
  dropping it. raylib vendors glfw as an `OBJECT_LIBRARY`: its objects are
  already inside `libraylib.a`, so it contributes no library to link, but the
  Cocoa and IOKit frameworks its macOS backend needs are reachable no other
  way;
- passes anything else through as written.

Those tokens then become cc flags: a flag (`-framework Cocoa`, `-Wl,...`) or an
absolute path goes through verbatim, and a bare name becomes `-l<name>`. One
translation is applied -- an Apple framework arrives as an absolute path to the
`.framework` *directory*, which cannot be passed as a link input (`ld: file
cannot be mmap()ed`), so it is respelled `-framework <name>`.

A shared-library dependency also gets `-Wl,-rpath` pointing at its build
directory, which makes the binary runnable where it was built. That rpath is
not relocatable: a binary copied elsewhere still needs its shared dependency
installed or bundled.

### `:link-libs` and `:link-flags` overrides

When the derived link line is wrong, or when there is no target to ask about,
override it per dep:

```turmeric
:cmake-deps #map{
  ;; Header-only: contributes include dirs, links nothing.
  "raygui" #map{:url "https://github.com/raysan5/raygui" :ref "4.0"
                :link-libs []}

  ;; Link a specific name instead of the derived artifact.
  "libpq"  #map{:prefer-system true :cmake-name "PostgreSQL"
                :targets   ["PostgreSQL::PostgreSQL"]
                :link-libs ["pq"]}

  ;; Verbatim tokens, no prefix added.
  "audio"  #map{:url "https://example.invalid/audio" :ref "v1"
                :link-flags ["-framework AudioToolbox"]}
}
```

- **`:link-libs [...]`** replaces the derived link entirely -- both the `-l`
  name and the `$<TARGET_FILE:...>` artifact path. The empty list is a
  meaningful value, distinct from omitting the key: `:link-libs []` says
  "contribute include dirs, link nothing", which is the only way to express a
  header-only dep (raygui) or a code generator that builds no library at all
  (glad). Transitive `link_flags` are still emitted.
- **`:link-flags [...]`** appends verbatim tokens with no prefix added. This is
  the escape hatch for anything the structured keys cannot describe.

A project's own link needs the same escape hatch when it has no `:cmake-deps`
entry to hang it on -- for instance inline-C that calls a system framework
directly. That spelling is `:build-opts :link-flags`, the verbatim sibling of
`:build-opts :link-libs`:

```turmeric
:build-opts #map{
  :link-libs  ["m"]                     ;; -lm
  :link-flags ["-framework Foundation"] ;; passed through as written
}
```

### System-first resolution with `:prefer-system`

Heavy native libraries (mbedTLS, raylib, sqlite, libpq) are often already
installed system-wide via Homebrew / apt / dnf. Building them from source on
every clean `tur fetch` is slow. Add `:prefer-system true` to try CMake's
`find_package` first and fall back to the source build only when no system
copy is found:

```turmeric
:cmake-deps #map{
  "mbedtls" #map{:prefer-system true                ;; try find_package first
                 :cmake-name    "MbedTLS"           ;; name passed to find_package
                 :cmake-version "3.0"               ;; optional minimum version
                 :targets       ["MbedTLS::mbedtls"
                                 "MbedTLS::mbedx509"
                                 "MbedTLS::mbedcrypto"]
                 :url           "https://github.com/Mbed-TLS/mbedtls"  ;; fallback
                 :ref           "v3.6.2"
                 :options       #map{:ENABLE_PROGRAMS "OFF"
                                     :USE_STATIC_MBEDTLS_LIBRARY "ON"}}
}
```

Behaviour:

- `:prefer-system true` **requires** `:cmake-name` -- it is the name handed to
  `find_package`. Omitting it is a hard manifest error.
- `:cmake-version` is optional; when present it becomes the minimum version in
  `find_package(<name> <version> QUIET)`.
- When the system copy is found, the dep's include/link flags come from the
  imported target's `INTERFACE_INCLUDE_DIRECTORIES` and `$<TARGET_FILE_DIR:...>`
  -- skipping both the clone and the source build. Otherwise the existing `FetchContent` block
  runs exactly as before.
- The generated `spice-deps-manifest.json` records `"resolved_via": "system"`
  or `"fetch"` per dep, and `tur.lock` records `:resolved-via "system"` (with
  no git SHA -- the system package manager owns the version) or the usual
  `:url`/`:ref`/`:resolved` row for the fetch path.
- `:options` land in the CMake cache **before** `find_package`, not only
  before the fallback build, so a find module's hint variable steers the
  system search. Homebrew keeps some formulae keg-only -- outside every
  default search path -- and this is how a spice points at one:
  `:options #map{:PostgreSQL_ROOT "/opt/homebrew/opt/libpq"}`. A hint naming
  a directory that does not exist is not searched, so it is harmless on other
  platforms. A user can also set the same name as an environment variable
  (`export PostgreSQL_ROOT="$(brew --prefix libpq)"`), which CMake reads too.
- **No `:url` means system-only.** Some libraries have no source build to
  fall back on (libpq ships inside the PostgreSQL server tree). Omit `:url` and
  the dep resolves through `find_package` or not at all: when nothing is found,
  `tur fetch` stops with a message naming the dep and the package, rather than
  attempting a fetch with nothing to fetch. Nothing is downloaded, so nothing
  is pinned: `tur audit` lists the dep as `system only`.

```turmeric
:cmake-deps #map{
  "libpq" #map{:prefer-system true                  ;; no :url: system-only
               :cmake-name    "PostgreSQL"
               :targets       ["PostgreSQL::PostgreSQL"]
               :options       #map{:PostgreSQL_ROOT "/opt/homebrew/opt/libpq"}}
}
```

`:prefer-system` is opt-in; deps without it keep their FetchContent-only
behaviour unchanged.

#### Forcing the source build (`--refetch`)

To pin a build to the source copy and bypass any system package -- useful for
reproducible CI artefacts -- pass `--refetch` (or set `TUR_FETCH_FORCE_FETCH=1`
in the environment):

```sh
tur fetch --refetch          # ignore system copies, always build from source
```

This disables the `find_package` short-circuit for every `:prefer-system` dep
in the manifest that has a `:url` to build from. A system-only dep (no `:url`)
still resolves through `find_package`, since there is no source copy to prefer.

> **Caveats.** A binary linked against a Homebrew/apt shared library will fail
> at runtime on a machine without that library installed; pin the source build
> (or `--refetch`) for portable artefacts. See
> [tur-fetch-system-first-plan.md](https://github.com/turmeric-lang/turmeric/blob/main/docs/archive/history/tur-fetch-system-first-plan.md) for the
> design rationale and open questions.

For the full `:cmake-deps` field reference, the generated `cmake/CMakeLists.txt`
format, the `spice-deps-manifest.json` schema, and hash locking, see the
[CMake/CPM integration notes](https://github.com/turmeric-lang/turmeric/blob/main/docs/archive/cmake-cpm-integration-plan.md).

---

## Vendoring C Sources (`:c-sources` / `:c-includes`)

`:cmake-deps` is the right tool for a real native library that already has a
CMake (or system-package) presence. But some C is too small for that ceremony:
a single-file header library (`stb_image`, `miniaudio`), a four-file FFT
(KissFFT), a hand-tuned kernel. For those, **vendor the `.c` straight into the
spice** and let the spice build compile and link it -- no CMake, no fetch step.

Add two keys under `:build-opts`:

```turmeric
(defpackage tur-signal
  :name "tur-signal"
  :build-opts #map{
    :c-includes ["c/kissfft"]            ;; -I dirs (manifest-relative)
    :c-sources  ["c/kissfft/kiss_fft.c"  ;; .c files compiled + linked
                 "c/kissfft/kiss_fftr.c"
                 "c/glue/fft_shim.c"]
  }
  :exports #map{ "signal/fft" [fft-forward fft-inverse] })
```

### Rules

- **Manifest-relative paths only.** Both `:c-sources` and `:c-includes` resolve
  relative to the directory holding `build.tur`. An absolute path is a hard
  error -- if you need a system include path, declare a `:link-libs` /
  `:cmake-deps` dependency instead.
- **Validated at manifest load.** Each `:c-sources` entry must exist on disk and
  end in `.c`, `.cc`, or `.cpp`; each `:c-includes` entry must be an existing
  directory. A typo fails the build immediately with a diagnostic pointing at
  the offending entry, not with an opaque `cc` error later.
- **`:c-includes` reach the spice's own C only.** The `-I` dirs are visible both
  to the vendored `.c` and to inline-C blocks in this spice's `.tur` modules
  (so an inline-C block can `#include "kissfft/kiss_fftr.h"`). They are **not**
  exported to consumers -- vendored headers are an implementation detail.
- **`:c-sources` propagate across the whole `:spices` closure.** If spice B
  vendors a `.c` and spice A depends on B -- directly, or through any number
  of intermediate spices -- building A links B's vendored sources into A's
  binary automatically, exactly as far as `:cmake-deps` reach. Each source is
  compiled once by resolved path, so a spice reached by two routes (a diamond)
  or two deps vendoring the same third-party file never produce duplicate
  symbols. Consumers see B only through its `.tur` exports, so a consumer's
  inline-C should `extern`-declare any vendored symbol it calls rather than
  relying on B's private headers.
- **C++ is accepted but not auto-configured.** `.cc` / `.cpp` entries are
  allowed (some vendor libraries are C++ wearing a C name), but the build does
  not switch to a C++ driver for you -- add `-x c++` to `:c-flags` if needed.
- **No platform conditionals.** Every listed source is compiled on every
  platform; gate platform-specific code with `#ifdef` inside the source, as
  vendor libraries already do.

### Layout: vendored C goes under `c/`, never `src/`

The manifest-driven build walks `src/` looking for `.tur` modules; do not put
`.c` files there. Keep vendored C in its own `c/` tree:

```
tur-signal/
  build.tur
  src/
    signal/fft.tur        ;; .tur modules (walked by the build)
  c/
    kissfft/
      kiss_fft.c          ;; vendored sources (listed in :c-sources)
      kiss_fft.h          ;; vendored headers (dir listed in :c-includes)
    glue/
      fft_shim.c
```

### When to reach for this vs `:cmake-deps`

Vendor a `.c` when the library is **small, single-purpose, and has no native
package-manager presence** -- you would otherwise be copy-pasting a header into
an inline-C block anyway. Reach for `:cmake-deps` when the library is large,
has its own build system, or is commonly installed system-wide (where
`:prefer-system` saves a source build). If a library needs CMake or autotools
to configure itself, it is not a vendoring candidate.

---

## Writing the Public API

Every exported symbol should have a `;;;` docstring immediately above its
definition. The standard format (from CLAUDE.md):

```turmeric
;;; db-open -- open an SQLite database file.
;;;
;;; Parameters:
;;;   path -- filesystem path to the database file (created if absent)
;;;
;;; Returns:
;;;   (ok db) on success, (err message) if the file cannot be opened
;;;
;;; Example:
;;;   (match (db-open "app.db")
;;;     (ok db) (println "opened")
;;;     (err m) (do (println "failed:") (println m)))
;;;
;;; Since: Phase P2
(defn db-open [path :cstr] (result :ptr)
  ...)
```

Exported symbols without docstrings will be omitted from `tur run docs` output.

### Module docstring (optional)

A spice module can optionally include a module-level docstring at the very top
of the file, before the first `defn`, `defmacro`, `defstruct`, `definstance`,
or `defopaque`. Place a contiguous `;;;` block followed by a `;;` comment line
(which acts as the separator):

```turmeric
;;; myspice/db -- SQLite database bindings.
;;;
;;; Thin wrapper around libsqlite3; provides open/close/query/exec with
;;; result-typed error handling.
;;;
;;; Since: Phase P2
;; ---- SQLite bindings ----
(extern-c sqlite3_open ...)
```

Without a module docstring the page renders with no description block -- the
per-symbol cards still appear normally.

### Style: avoid `cons ... 0` chains in examples

In README quick-starts and docstring examples, do not show runtime list values
as `(cons x (cons y 0))` chains. The trailing `0` is the nil-of-list footgun --
new readers have no way to tell that `0` means "end of list," and the chain
itself reads in reversed nesting order.

Use the `list` macro instead. It expands to the same `tcons`/`tnil` cells, so
it's a drop-in for any API that today takes a `:int` cons list:

```turmeric
;; Avoid
(group-by f (cons "g" 0))
(plot (cons (axes) (cons (function f) 0)))

;; Prefer
(group-by f (list "g"))
(plot (list (axes) (function f)))
```

Pair-cons (`(cons key value)` with no trailing `0`) is fine -- a two-element
pair is a clear, idiomatic data shape. The footgun is the nil-terminated chain.

If your spice API can take a `Vec` instead of a cons list, prefer that and
document it with `(vec-of ...)`. The end goal is for `cons` to disappear from
quick-start surfaces entirely.

### Typed variadic rest parameters

A `& rest :T` parameter type-checks against `T` even when `T` is a user-defined
type -- a `defopaque` newtype, struct, ADT, or type application. The rest
element type is resolved to its full type and each argument is checked by
identity at the call site, so you can give a variadic API a real handle type
instead of an untyped `:int`:

```turmeric
(defopaque Route :int)

;; Each rest arg must be a Route; a raw :int or a different opaque is rejected.
(defn launch [& routes :Route] :ptr<void>
  ...)
```

Do not declare the rest as `:int` and cast handles back inside the body --
write the real type (a bare `:int` rest rejects opaque/struct/ADT values).
For an interface that mixes distinct handle types (e.g. middlewares and
routes), use two explicit `:list<T>` parameters rather than one untyped
rest -- a single `& rest` is one homogeneous element type by design.

### Typeclass names are global: tag them with your format or domain

A `defclass` is **not** scoped by its module's `(export ...)` list. It resolves
globally, across every spice a program loads. Two spices that each declare an
obviously-named class -- `Encode`, `Render`, `Size`, `Parse` -- cannot be used
in the same program, and whichever one claims the bare name makes the other
look like the special case.

Worse, the **method** names have to differ too, and for a harder reason than
style. Two classes declaring a method of the same name compile with zero
diagnostics and dispatch to whichever instance registered last, so reordering
two unrelated `definstance` forms silently changes which implementation runs.
There is no ambiguity error today; distinct method names are the only guard.

So: name a class for what it does **and for whose it is**.

```turmeric
(defclass EncodeJson [a] (encode-json [x] : cstr))    ;; tur-json
(defclass EncodeMp   [a] (encode-mp   [x] : Buf))     ;; tur-msgpack
```

The two serialization spices are the worked example. They ship the same
surface -- format-tagged `Encode*` / `Decode*` / `Decode*Checked` classes, a
`derive-*` family for `defstruct` / `defdata` / `defopaque`, and an
accumulate-all-errors checked-decode layer whose vocabulary mirrors
`stdlib/schema.tur` -- and they agree on shape, so a struct is a
map/object keyed by field name in both and a sum is externally tagged
`{"Ctor": {...}}` in both. Neither owns the unqualified spelling: tur-json
gave it up in 0.4.0 precisely so that a program can derive both codecs for one
struct.

**Which one to reach for.** Pick `tur-json` when the payload crosses a boundary
a human reads or a browser consumes -- config, HTTP APIs, logs. Pick
`tur-msgpack` when both ends are programs and you want the bytes smaller and
the parse cheaper -- internal RPC, on-disk records, message queues. The
decision surfaces in the types: json hands back a `cstr` you can print, msgpack
hands back an owned `Buf`, because a binary fragment contains NUL bytes and
`cstr` cannot hold one. tur-msgpack has no native dependency; tur-json builds
yyjson.

---

## Testing Your Spice

Add `tur-test` as an optional dependency and place test files in `tests/`:

```sh
tur add https://github.com/turmeric-lang/turmeric-spices \
  --ref test-v0.1.0 --subdir spices/test --name test
```

A test file using `tur-test`:

```turmeric
(import test/assert :refer [assert-eq assert-ok assert-err])
(import test/suite  :refer [describe it])
(import test/runner :refer [run-all])
(import mylib/core  :refer [some-fn])

(describe "some-fn"
  (it "returns the expected value"
    (assert-eq (some-fn 10) 42))
  (it "returns err for invalid input"
    (assert-err (some-fn nil))))

(run-all)
```

```sweet-exp
import test/assert :refer [assert-eq assert-ok assert-err]
import test/suite  :refer [describe it]
import test/runner :refer [run-all]
import mylib/core  :refer [some-fn]

describe "some-fn"
  it "returns the expected value"
    assert-eq(some-fn(10), 42)
  it "returns err for invalid input"
    assert-err(some-fn(nil))

run-all()
```

Run the test suite:

```sh
tur test
```

For the full testing API see [test-runner-contract.md](test-runner-contract.md).

---

## Versioning

- Use semver: `MAJOR.MINOR.PATCH`.
- Git tags must be `vMAJOR.MINOR.PATCH` for standalone repos (`v0.1.0`).
- For spices inside the `turmeric-spices` monorepo use the per-spice tag
  format `<spice>-vMAJOR.MINOR.PATCH` (`math-v0.1.0`, `sqlite-v0.2.1`).
- Pre-releases: `v0.2.0-alpha.1`.
- Bump `MAJOR` for breaking API changes, `MINOR` for additive changes,
  `PATCH` for bug fixes.

---

## Publishing

### As a standalone repository

```sh
git tag v0.1.0
git push && git push --tags
```

Consumers then add your spice with:

```sh
tur add https://github.com/you/tur-mylib --ref v0.1.0
```

### Contributing to turmeric-spices

The [turmeric-spices](https://github.com/turmeric-lang/turmeric-spices) monorepo
accepts spices that meet the bar for the ecosystem. To contribute:

1. Fork the monorepo and add your spice under `spices/<name>/`.
2. Add it to the workspace root `build.tur` `:members` list.
3. Include a `README.md` and at least one test file.
4. Open a pull request.

After merging, tag the spice's first release:

```sh
git tag math-v0.1.0
git push --tags
```

Consumers use `--subdir spices/<name>` when adding:

```sh
tur add https://github.com/turmeric-lang/turmeric-spices \
  --ref myspice-v0.1.0 --subdir spices/myspice --name myspice
```

### Spice registry (future)

Once `pkg.turmeric-lang.org` launches, `tur publish` will register the
package and consumers will use `tur add spice/<name>` without a Git URL.

---

## Per-file Commands Inside a Spice

`tur build <dir>` and `tur run` (project mode) know about the spice they
live in because they start by reading `build.tur`. For `tur build <dir>`
that means: descend into `src/` (recursively, including nested
`src/<pkg>/` trees), skip the manifest itself, compile every module,
resolve the include path from the project's own `src/` plus each
`:spices` dep's `src/`, and verify each declared `:exports` module has a
backing source file -- failing loudly otherwise.

Generated `.c`/`.h`/`.o` and the final library/exe land under
`<spice-root>/build/{obj,bin,lib}/` by default. Override per-build with
`--build-dir <dir>` / `-B <dir>`, with the `TUR_BUILD_DIR` env var, or
durably with `:build-dir "<path>"` in `build.tur` (path is relative to
the manifest dir). Precedence runs CLI flag > env > manifest > default.
The build dir is auto-created with a `.gitignore` of `*`, so its
contents never leak into VCS even if the dir itself gets tracked. (See
[manifest-driven-build-descent-plan.md](https://github.com/turmeric-lang/turmeric/blob/main/docs/archive/history/manifest-driven-build-descent-plan.md).)

`:engine "cc" | "jit" | "interp"` selects the default EXECUTION engine for
`tur run`, on the same ladder: `--engine` flag > `TUR_ENGINE` env >
manifest > `"cc"`.  An unknown value is a hard error (TUR-E0311); a `"jit"`
selection needs a `-DTUR_JIT=ON` build (the former `jit` experiment gate has
graduated -- only the build-time gate remains).  The engines differ in
semantics, not just speed
(`#?(:tur ... :turi ...)`, inline-C carve-outs), so when the choice is
load-bearing, pair it with a `:tur-version` floor: older binaries silently
ignore unknown manifest keys and would run under cc.
The per-file subcommands `tur check`, `tur emit-c`, `tur emit-h`,
`tur build <file>`, and `tur run <file>` get the same module resolution
automatically -- they walk up from the input file looking for a sibling
`build.tur` and add that spice's `src/` (and its `:spices` deps' `src/`)
to the include path. The interpreter entry points do too: `tur interpret`
(`tur --interpret`), `tur debug`, `tur eval --file`, and
`tur run --engine=interp`, so a spice's `tests/` interpret from the spice
root with no flags.

This means editors, format-on-save hooks, LSP clients, and quick
"compile this one file" loops work without per-spice configuration:

```sh
cd spices/frame
tur check src/frame/frame.tur     # resolves intra-spice imports
tur emit-c src/frame/schema.tur   # same
tur build src/frame/quickstart.tur -o /tmp/qs  # compiles one file, keeps the binary
tur run src/frame/quickstart.tur  # builds and executes
```

The walk-up is capped at 16 ancestor directories, so a stray
`build.tur` far above your working tree won't accidentally win.

### `-I` for extra directories

Explicit `-I <dir>` flags still work and take priority over
auto-discovered paths -- useful for fixtures, vendored copies, or when
you want to test against a different version of a dep:

```sh
tur check -I vendor/alternate src/main.tur
```

`-I` accepts both the spaced (`-I path`) and concatenated (`-Ipath`)
forms. For `tur interpret` and `tur debug` the flags go before the file:
everything after the file is the program's `*args*`, a `-I` included.

### `--no-auto-spice` escape hatch

If you need to compile a file as if no spice exists around it (rare --
typically only useful for resolver-fixture tests or when an unrelated
`build.tur` is in the ancestor chain), pass `--no-auto-spice`:

```sh
tur --no-auto-spice check tests/fixtures/resolver/input.tur
```

This restores the pre-auto-discovery behavior: only the input file's
own directory, the stdlib, and any explicit `-I` paths are searched.

### When auto-discovery does not apply

- `tur build <dir>` -- already configures itself from the directory's
  `build.tur` (reads the manifest, descends into `src/`, and resolves the
  include path); the per-file walk-up does not apply. Note that the
  single-file `tur build <file>` *does* auto-discover (it joins the
  per-file family above); only the directory form is manifest-driven.
- `tur format <file>` -- the formatter doesn't resolve imports, so
  include paths are irrelevant.

---

## Supporting CMake Consumers

If you want C and C++ projects to consume your spice via CMake or CPM
without knowing anything about Turmeric, run:

```sh
tur emit-cmake
```

This reads `build.tur` and generates `CMakeLists.txt`,
`<name>Config.cmake`, and helper modules. Commit those files, tag the
release, and a CPM consumer can add your library with:

```cmake
CPMAddPackage(
  NAME    tur-mylib
  URL     https://github.com/you/tur-mylib/archive/refs/tags/v0.1.0.tar.gz
  VERSION 0.1.0
)
target_link_libraries(my_app PRIVATE tur-mylib::all)
```

For the complete step-by-step see
[Using a Turmeric library from CMake](using-turmeric-from-cmake.md).

---

## Emscripten / WASM Support

`tur build --target wasm` compiles via `emcc` (Emscripten must be
installed), and `:cmake-deps` are configured through `emcmake`, so a
cmake-deps spice gets a WASM build automatically when the underlying C
library supports Emscripten. To make your spice Emscripten-compatible:

- Prefer C libraries with documented Emscripten support (yyjson, mbedTLS,
  PCRE2, and the SQLite amalgamation all qualify).
- Avoid libraries that require native threads or sockets without WASM
  fallbacks.
- Use `#ifdef __EMSCRIPTEN__` in inline-C blocks to handle differences
  (e.g. skipping native TLS when the browser provides its own TLS stack).

---

## Global Spices as Libraries

A spice installed with `tur install` is a command-line tool by default: its
`:bin` entries are symlinked into `~/.local/bin/` and become available as
`tur-<cmd>` (or via the `tur <cmd>` fallthrough). The global `spices/` root is
deliberately **not** on the default module-resolution path -- that would make
every build depend on what happens to be installed on the machine.

A project opts in per dependency, by declaring it `:global` in its
`build.tur`:

```turmeric
:spices #map{
  "notebook" #map{:global true}
}
```

That entry resolves through the install registry (`state.tur`), so the spice's
`src/` joins the project's module-resolution path and `(import notebook/core)`
works. Four things follow from where it resolves:

- **It is never fetched.** `tur install` owns the checkout, so `tur fetch`
  has nothing to do for a `:global` dep and writes no `tur.lock` row for it --
  the same treatment a `:path` dep gets.
- **A missing install is a hard error**, not a silent skip:
  `spice: 'notebook' declares :global true but no such spice is installed --
  run \`tur install <source>\` first`. Failing here is the point; the
  alternative surfaces a hundred lines later as `module 'notebook/core' not
  found` with no hint that a spice was never installed.
- **`:global` takes no `:url` and no `:path`.** They name a different
  resolution source, so declaring both is a manifest error rather than a
  silent precedence rule.
- **The spice must be installable**, which today means it declares at least
  one `:bin` entry -- `tur install` is a binary installer. A library-only
  spice cannot be registered yet, so it cannot be a `:global` dep either.

Not built: the `:global-policy` knob that would decide whether a missing
global install is auto-installed rather than reported, and version-range
validation against the installed version (the registry records a `:version`,
but nothing checks it against a requested range yet).

This is **deferred**; until it ships, a spice that wants to be reused as
a library should be added the normal way with `tur add`. See the
[global-spice-install plan](https://github.com/turmeric-lang/turmeric/blob/main/docs/archive/global-spice-install-plan.md#imports-from-global-spices)
for the full design sketch.

---

## Release Checklist

- [ ] `tur build --release` passes with no warnings
- [ ] `tur test` passes
- [ ] All public symbols are listed in `:exports`
- [ ] Each exported symbol has a `;;;` docstring (summary + params + returns + example)
- [ ] cmake-deps spices build cleanly on macOS and Linux
- [ ] `tur emit-cmake` succeeds if you intend to support CMake consumers
- [ ] Generated CMake files are committed if supporting CMake consumers
- [ ] `CHANGELOG.md` entry written
- [ ] Git tag pushed: `git tag v0.1.0 && git push --tags`

---

## Diagnostics to Watch For

### TUR-D0001 -- no leading colons inside `(fn ...)` types

Leading colons inside a `(fn ...)` type expression are deprecated and the
compiler emits **TUR-D0001** wherever they appear. Write the new form when
declaring function-typed parameters, return types, or higher-kinded
abstractions in your spice:

```turmeric
;; Wrong (TUR-D0001):
(defn map-fn [^fat g :(fn [:int] :int) n :int] :int (g n))

;; Right:
(defn map-fn [^fat g :(fn [int] int) n :int] :int (g n))
```

The structural `name : type` colon (the one separating a parameter name from
its type) is unaffected -- the rule only forbids colons **inside** a `(fn ...)`
type. If you are migrating an older spice forward, run
`tools/rewrite_fn_type_colons.py` over your tree, or fix the hits by hand.

### Name mangling and inline-C

If your spice exposes any inline-C bodies that reference sibling Turmeric
`defn`s by name, use the `__TUR_CNAME_<source-name>__` splice rather than
hand-spelling the mangled C identifier; the mangling scheme is reversible
and injective (#275) but is still an internal detail. See
[name-mangling-guide.md](name-mangling-guide.md) for the encoding and
[c-integration-guide.md "Inline C blocks"](c-integration-guide.md#calling-c-from-turmeric)
for the splice form.

---

## See Also

- [Consuming spices](consuming-spices-guide.md) -- adding and using spices in a project
- [Package management guide](package-management-guide.md) -- full `build.tur` manifest reference and `tur` CLI
- [C integration guide](c-integration-guide.md) -- `extern-c`, `include-c`, inline-C blocks
- [Name mangling guide](name-mangling-guide.md) -- reversible Turmeric->C name mangling for inline-C interop
- [Using a Turmeric library from CMake](using-turmeric-from-cmake.md) -- step-by-step guide for `tur emit-cmake`
- [Test runner contract](test-runner-contract.md) -- full testing API
