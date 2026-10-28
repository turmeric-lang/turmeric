---
title: Consuming Spices Guide
category: Package Management
description: Adding, fetching, and using spice packages in a Turmeric project
tags:
  - spices
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

## Using spices without a project

A one-off script or a REPL session has no `build.tur` to declare spices in.
For those, an **installed** spice is importable directly:

```sh
tur install https://github.com/turmeric-lang/turmeric-spices \
  --ref json-v0.3.0 --subdir spices/json
cd ~/scratch                  # no build.tur anywhere above
tur run report.tur            # (import json/decode) resolves
tur repl                      # and at the prompt too
```

Any spice with modules can be installed -- one with no `:bin` entries is a
library-only install (see
[Global Spices as Libraries](developing-spices-guide.md#global-spices-as-libraries)).

### Which files see the installed spices

Only a file with **no `build.tur` above it** (within 16 parent directories):
`tur run`, `tur check`, `tur build <file>`, `tur --interpret`, `tur jit`,
`tur emit-c`, `tur expand`, `tur eval --file`, the LSP, and libturi embedders
that call `turi_env_set_search_path_for`. `tur run -` (stdin) and `tur repl`
are anchored at the current directory.

A file inside a project sees exactly the spices its manifest declares, as
always -- a project build must not depend on what happens to be installed on
the machine, and a script has no lock file whose reproducibility the installed
set could break. Inside a project, reach an installed spice with a
[`:global` dep](#globally-installed-spices), or with `:spice` at the REPL.

### Search order

Outside a project an import is searched in:

1. the importing file's directory,
2. the stdlib,
3. `-I` dirs, in command-line order,
4. every installed spice's own `src/`, in install-name order (byte-wise),
5. each installed spice's own dependencies, in the same order.

Steps 1-3 are what was searched before; installed spices only fill a gap. An
import that resolves without them resolves to the same file with them, and the
spice you installed by name beats another version of it that some other
installed spice depends on. A failed import lists the installed dirs it tried,
labelled `(installed spice <name>)`, and names `tur install` in its hint.

Linking follows the imports: an installed spice's `:c-sources`, `:cmake-deps`
and `:link-libs` reach a program only when the program imports one of its
modules, so one broken or platform-specific installed spice does not break
every script.

### Turning it off

`--no-global-spices` (a global flag) or `TUR_NO_GLOBAL_SPICES=1` removes steps
4 and 5. `--no-auto-spice` implies it -- it already means "add nothing to the
search path on my behalf". The repository's test harnesses set
`TUR_NO_GLOBAL_SPICES=1`, so a developer's installed spices cannot leak into
the fixture suite.

### Declaring spices in the script itself (`defscript`)

An installed spice makes a script work on *your* machine. To make it work on
anyone's, declare what it needs in the file, in a `(defscript ...)` header.
This is experimental, so it needs `--enable=script-deps` -- on the `#!` line,
on the command line, or in `~/.config/turmeric/experiments.tur`:

```turmeric no-check
#!/usr/bin/env -S tur --enable=script-deps
(defscript
  :spices #map{"json" #map{:url    "https://github.com/turmeric-lang/turmeric-spices"
                           :ref    "json-v0.3.0"
                           :subdir "spices/json"}})

(defmodule report
  (import json/decode)
  (defn main [] : int
    ...))
```

```sh
chmod +x report.tur
./report.tur data.json       # first run fetches json-v0.3.0; later runs are offline
```

The rules:

- **The header is the file's first form.** Only a `#!` line, a `#lang` line
  and comments may come before it (TUR-E0630 otherwise, and in a module the
  script imports). It reads the same in sweet-exp: a `defscript` line at
  column 0 with the options indented under it.
- **`:spices` is the build.tur entry grammar.** `:url` + `:ref` [+ `:subdir`],
  `:path` (relative to the script), `:global true` (an installed spice, from
  the registry), `:optional`. Every `:url` dep needs a `:ref` -- a tag, a
  branch or a commit id -- and nothing but `:spices` is accepted (TUR-E0631);
  `:cmake-deps` and friends need a project.
- **A file inside a project cannot have one** (TUR-E0632): its build.tur
  governs it.
- **A script with a header is hermetic.** It sees its declared spices (and
  their dependencies), its own directory, the stdlib and `-I` -- the installed
  spices are *not* searched. Add `"name" #map{:global true}` to use one.
- **Linking follows the imports**, as for installed spices: a declared
  spice's `:c-sources`, `:cmake-deps` and `:link-libs` reach the program when
  it imports one of its modules.

Every per-file command honors the header -- `tur run`, `tur check`,
`tur build <file>`, `tur --interpret`, `tur jit`, `tur emit-c` -- and the LSP
resolves it from the cache without ever fetching.

#### Where the spices go

Fetched spices land in the **script-dep cache**,
`~/.local/share/turmeric/cache/script-spices/` (under `$TUR_HOME` when set):
one immutable tree per (url, subdir, commit), each set up as a fetched
project, so its own `:spices` and `:cmake-deps` come with it. The cache is
never your install set -- `tur list` does not show it, and two scripts can pin
two versions of one spice side by side.

A tag or branch `:ref` is resolved once and remembered in the cache's
`index.tur`; later runs need no network. A run that fetches says so on
stderr (`tur: fetching json (json-v0.3.0) for report.tur`).

| Command | Effect |
| --- | --- |
| `tur run --offline report.tur` | resolve from the cache only; a missing spice is TUR-E0634 |
| `tur run --update report.tur` | resolve tag and branch refs again |
| `tur fetch --script report.tur` | warm the cache without running |
| `tur cache list` | the entries, their size and when they were last used |
| `tur cache clean [--older-than <days>]` | remove entries (all, or the ones not used for `<days>`) |

#### Editing the header

```sh
tur --enable=script-deps add --script report.tur \
    https://github.com/turmeric-lang/turmeric-spices --ref json-v0.3.0 --subdir spices/json
tur --enable=script-deps add --script report.tur ./helpers --path --name helpers
```

`tur add --script` creates the header (after the `#!` and `#lang` lines) or
adds the entry to it, written back through the formatter. Comments inside an
existing header are not kept.

#### Locking a script

A one-off script should not grow a second file unless you want it to, so the
lock is opt-in:

```sh
tur --enable=script-deps fetch --script report.tur --lock     # writes report.tur.lock
```

`report.tur.lock` uses tur.lock's format: a row per fetched spice with its
commit and tree hash, and one per dependency of each, named `<spice>/<dep>`.
While it exists:

- every run resolves from it -- even `--update` (move it with
  `tur fetch --script report.tur --lock --update`);
- every cached tree is hash-checked against it, and one modified after it was
  fetched is TUR-E0634 (`tur cache clean`, then run again);
- a `:url` spice the lock does not pin as the header declares it is a stale
  lock -- TUR-E0634, naming `tur fetch --script report.tur --lock` -- not a
  silent re-resolve.

`:path` and `:global` spices are never locked, as in build.tur.

#### Running a script by name

`tur report.tur a b` is `tur run report.tur -- a b`, which is what lets a
`#!/usr/bin/env -S tur --enable=script-deps` line work: global flags before
the file apply, and every word after it is the script's. A built-in command
always wins (`tur check.tur` runs a script called `check.tur`; `tur check`
is the command). Windows has no `#!`: run `tur report.tur args` there.

## Running a spice's tool without installing it (`tur x`)

`tur install` makes a spice's `:bin` a command you keep. To run one once --
the way `npx` or `uvx` do -- use `tur x`:

```sh
tur x https://github.com/turmeric-lang/turmeric-spices \
      --ref notebook-v0.1.0 --subdir spices/notebook -- render hello.tur.md
tur x ~/src/my-tool -- --help        # a local checkout, built in place
tur x notebook -- new hello.tur.md   # an installed spice, run directly
```

Everything after `--` is the program's. The source is read the way
`tur install` reads it:

| Source | What `tur x` does |
| --- | --- |
| an installed name | runs the binary `tur install` built; never builds |
| `<spice>/<bin>` (an installed spice) | runs that spice's binary, linked or shadowed -- the same as `tur <spice>/<bin>` (a directory of that name stays a path) |
| a path (has a `/`, or starts with `.` or `~`) | builds the `:bin` into `<checkout>/build/`, where `tur install` puts it, and again whenever a file under the checkout is newer than the binary |
| a URL (`--ref`, `--subdir`) | fetches into the script-dep cache and builds the `:bin` there once |

A URL lands in the same cache as a [`defscript` header](#declaring-spices-in-the-script-itself-defscript)'s
deps -- `<data>/cache/script-spices/<name>-<sha12>`, set up as a fetched
project, so its own `:spices` and `:cmake-deps` come with it -- and a tool and
a script naming the same commit share one tree. The first run fetches and
builds; later runs need no network. A `--ref` naming a branch is resolved once
and remembered; `--refresh` resolves it again (an older tree stays until
`tur cache clean` removes it). Without `--ref`, the default branch.

- `--bin <name>` picks a binary when the spice declares several; with one it
  is implied, and with several and no `--bin`, `tur x` lists them and exits 2.
- `--offline` never fetches: a URL not in the cache is an error.
- `--refresh` on a path rebuilds even when nothing changed.

`tur x` registers nothing: no `state.tur` row, nothing in `~/.local/bin/`,
nothing added to the project-less search path. It also ignores any
`build.tur` in the current directory -- inside a project or outside one it
means the same thing.

## Two installed spices may ship one command name

A spice's `:bin` names start with `tur-`, and `tur foo` runs `tur-foo` from
`PATH` (as `git foo` runs `git-foo`). Two installed spices can both ship
`tur-foo`; both stay usable:

- **The bare name has one holder.** `tur-foo` on `PATH`, and `tur foo`, run the
  spice that was installed first, until you move it.
- **`tur <spice>/<bin>` always runs that spice's binary**, whichever spice
  holds the bare name: `tur beta/foo -- args` (or `tur x beta/foo -- args`). The
  spice may be named with or without its `tur-` prefix, and so may the bin.
  Use it in a script to pin which tool you mean.
- **Installing the second provider succeeds with a warning.** Its binary is
  built but not linked, and its library modules are importable as usual:

  ```text
  tur install: warning: 'tur-foo' is already provided by installed spice 'alpha';
    beta's copy is installed but not linked. Run it as `tur beta/foo`,
    or make it the default with `tur install --link beta/foo`.
  ```

`tur install --link beta/foo` moves the bare name to beta's build. It neither
fetches nor rebuilds, and prints whose it was; `tur install --link beta` moves
every name beta declares. On a source (`tur install --link <url>` or
`tur install --link ./beta --path`) it installs and takes every name.
`--force` takes them too, but `--link` never overwrites a file `tur` did not
install -- a `tur-foo` of your own in `~/.local/bin/` still needs `--force`.

`tur list` shows who holds what -- `tur-foo (shadowed by alpha; tur beta/foo)`
-- and `tur list --json` gives each spice a `"bins"` list of
`{"name", "linked", "held_by"}`.

`tur uninstall alpha` removes `tur-foo` only if alpha holds it, and then
promotes nothing: you chose which tool `tur-foo` meant, and uninstall cannot
know which of the remaining providers you would pick. It says what is left:

```text
tur uninstall: note: 'tur-foo' is now unlinked; beta still provides it.
  Run it as `tur beta/foo`, or make it the default with
  `tur install --link beta/foo`.
```

A name nobody holds is free: the next `tur install` of a spice that declares it
takes it. A spice already installed shadowed stays shadowed (an upgrade or
reinstall never changes who holds a name); becoming the holder takes `--link`.

`<spice>@<version>/<bin>` is reserved for picking among versions installed side
by side, which is not supported yet; `tur beta@0.2.0/foo` says so.

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

`tur.lock` records the resolved commit and a content hash of the tree for
every fetched spice. Always commit it to version control; it is what makes builds
reproducible across machines and CI runs.

```turmeric no-check
;; tur.lock -- generated by `tur fetch`.  Do not edit by hand; commit it.
(deflockfile
  :format-version 1
  :spices #{
    "geom" #{:url "https://github.com/alice/tur-geom" :ref "v0.2.1" :resolved "a1b2c3d4e5f6a7b8c9d0e1f2a3b4c5d6e7f8a9b0" :sha256 "tree1:dc4d2c00b1e74a6f9c3d8e2f1a0b5c4d7e6f8a9b0c1d2e3f4a5b6c7d8e9f0a1b" :fetched-at "2026-10-21T09:00:00Z" :transitive ["math@math-v0.1.0"] }
    "math" #{:url "https://github.com/turmeric-lang/turmeric-spices" :ref "math-v0.1.0" :resolved "d6e7f8a9b0c1d2e3f4a5b6c7d8e9f0a1b2c3d4e5" :sha256 "tree1:7a1c9e0d3b5f2a4c6e8d0b1f3a5c7e9d2b4f6a8c0e1d3f5b7a9c2e4d6f8b0a1c" :fetched-at "2026-10-21T09:00:03Z" }
  }
  :cmake-deps #{
    "raylib" #{:url "https://github.com/raysan5/raylib" :ref "5.0" :resolved "ae50bfa2cc569c5a9f4a3c0a4e2b7d8e9f0a1b2c" :fetched-at "2026-10-21T09:00:05Z" }
  })
```

`tur fetch` writes plain s-expressions, never the sweet-exp form. A spice
row's `:sha256` is a `tree1:` hash of the checked-out tree, not of an archive;
`:subdir` is not recorded.

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

(defn main [] : int
  (let [v (vector-2d 1.0 2.0)
        m (mat/mat4-identity)]
    (println v)
    0))
```

```sweet-exp
import geom/vector :refer [vector-2d cross-product]
import geom/matrix :as mat

defn main [] : int
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

(defn main [] : int
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

defn main [] : int
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

# A script's own spices (defscript header; --enable=script-deps)
tur add --script <file> <url> --ref <ref>
tur fetch --script <file> [--update] [--lock]
tur run [--offline|--update] <file>
tur <file> <args...>
tur cache list
tur cache clean [--older-than <days>]

# Run a spice's tool once, without installing it
tur x <url> [--ref <ref>] [--subdir <dir>] [--bin <name>] [-- <args...>]
tur x <path> [-- <args...>]
tur x <installed-name> [-- <args...>]
tur <spice>/<bin> [<args...>]          # an installed spice's binary, linked or not
tur install --link <spice>[/<bin>]     # make it the one `tur-<bin>` runs
```

---

## See Also

- [Package management guide](package-management-guide.md) -- full `build.tur` manifest reference and `tur` CLI
- [Developing spices](developing-spices-guide.md) -- creating and publishing your own spice
- [C integration guide](c-integration-guide.md) -- `extern-c`, `include-c`, inline-C blocks
- [Using a Turmeric library from CMake](using-turmeric-from-cmake.md) -- publishing a Turmeric library for C/C++ consumers
