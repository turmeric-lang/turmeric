---
title: Using a Turmeric Library from CMake
category: Package Management
description: How to publish a Turmeric library for consumption by C and C++ projects via CMake or CPM using `tur emit-cmake`
tags:
  - build-system
  - ffi
---

# Using a Turmeric Library from CMake

This guide explains how to publish a Turmeric library so that C and C++ projects
can import it through CMake or CPM without knowing anything about Turmeric.

The `tur emit-cmake` command does the heavy lifting: it reads your `build.tur`
and generates all of the CMake files a consumer needs.

---

## Prerequisites

- A Turmeric library project created with `tur init --lib <name>`
- The library builds cleanly with `tur build`
- `tur` is on `PATH` on the machine where CMake will build the consumer project

---

## Step 1 -- Declare exported sources

Open `build.tur` and add an `:exports` field listing the `.tur` source files
that form the public API of your library:

```turmeric
(defpackage my-geom
  :version "0.3.0"
  :description "2-D geometry primitives"
  :license "MIT"

  :exports [
    "src/vector.tur"
    "src/matrix.tur"
  ])
```

If you omit `:exports`, `tur emit-cmake` falls back to scanning every `*.tur`
file in `src/`.

---

## Step 2 -- Generate the CMake files

Run this command from your project root (where `build.tur` lives):

```sh
tur emit-cmake
```

It creates four files:

| File | Purpose |
|---|---|
| `CMakeLists.txt` | Top-level CMake project that compiles your library |
| `<name>Config.cmake` | `find_package(<name>)` support |
| `cmake/FindTurmeric.cmake` | Locates the `tur` binary on the consumer's machine |
| `cmake/AddTurmericTarget.cmake` | Helper macros (used internally) |

All four files are auto-generated -- do not edit them by hand. Re-run
`tur emit-cmake` whenever you add or remove source files, or change the version.

### Optional: write to a different directory

```sh
tur emit-cmake --output-dir dist/
```

This writes `dist/CMakeLists.txt`, `dist/<name>Config.cmake`, and
`dist/cmake/*.cmake`.

---

## Step 3 -- Commit and tag

```sh
git add CMakeLists.txt my-geomConfig.cmake cmake/
git commit -m "chore: regenerate cmake files for v0.3.0"
git tag v0.3.0
git push && git push --tags
```

CPM identifies releases by tag, so a clean tag is required.

---

## Step 4 -- Add to a C/C++ project via CPM

In the consuming project's `CMakeLists.txt`:

```cmake
cmake_minimum_required(VERSION 3.20)
project(my_app LANGUAGES C)

# CPM bootstrap (download once, cache afterwards)
include(cmake/CPM.cmake)

CPMAddPackage(
  NAME    my-geom
  URL     https://github.com/alice/my-geom/archive/refs/tags/v0.3.0.tar.gz
  VERSION 0.3.0
)

add_executable(my_app src/main.c)
target_link_libraries(my_app PRIVATE my-geom::all)
```

CPM fetches the archive, runs the generated `CMakeLists.txt`, which in turn
invokes `tur emit-c --output-dir` to compile the `.tur` sources to C, then
builds the static library. The consumer never calls `tur` directly.

### Using the generated header

The Turmeric compiler generates one `.h` per source file:

```c
/* src/main.c */
#include "vector.h"   /* generated from src/vector.tur */
#include "matrix.h"   /* generated from src/matrix.tur */

int main(void) {
    my_geom_vector_2d v = my_geom_vector_2d_make(1.0f, 2.0f);
    /* ... */
    return 0;
}
```

`target_link_libraries` with `my-geom::all` causes CMake to add the include
directory automatically -- no manual `-I` flags needed.

---

## Using `find_package` instead of CPM

If the library is installed to a standard prefix:

```sh
# in the library project
cmake -B build -DCMAKE_INSTALL_PREFIX=/usr/local
cmake --build build
cmake --install build
```

Then in the consumer:

```cmake
find_package(my-geom REQUIRED)
target_link_libraries(my_app PRIVATE my-geom::all)
```

---

## How it works internally

`tur emit-cmake` generates a `CMakeLists.txt` that:

1. Calls `find_package(Turmeric REQUIRED)` to locate the `tur` binary via
   `cmake/FindTurmeric.cmake`.
2. Registers an `add_custom_command` that runs
   `tur emit-c --output-dir <build-dir> <source files>` to produce one `.h`
   and one `.c` per `.tur` module.
3. Passes those `.c` files to `add_library(<name> STATIC ...)`.
4. Exports an alias target `<name>::all` for consumers.

The `<name>Config.cmake` file is what `find_package(<name>)` reads. It sets
`<name>_VERSION` and `<name>_FOUND`, then includes the install targets file.

---

## Checklist for library authors

- [ ] `tur build --release` passes
- [ ] `:exports` lists all public `.tur` files in `build.tur`
- [ ] `tur emit-cmake` runs without errors
- [ ] Generated files are committed to the repository
- [ ] Release is tagged: `git tag vX.Y.Z`
- [ ] CPM fetch test passes from a clean directory
- [ ] `cmake/` usage example included in README

---

## See also

- [Package management guide](package-management-guide.md) -- Turmeric-to-Turmeric dependencies
- [C integration guide](c-integration-guide.md) -- calling C from Turmeric
