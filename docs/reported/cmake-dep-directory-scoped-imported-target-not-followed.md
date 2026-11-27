# A `:cmake-deps` library linked through an imported target the dependency made itself is not linked

**Severity:** medium (the program fails to link on undefined references, and
`tur fetch` reports success with no warning).

`tur fetch` walks a dependency's `INTERFACE_LINK_LIBRARIES` from the generated
top-level `CMakeLists.txt`. An `IMPORTED` target is visible only in the
directory that created it and below, unless it was created `GLOBAL`. So an
imported target the dependency creates in its own `CMakeLists.txt` -- the
usual way to wrap a vendored or system library -- is not a target where the
walk runs. Its name is passed through as if it were a library name, and what
it stands for never reaches the link line.

#1486 covered one source of these: targets a dependency gets from
`find_package`, by setting `CMAKE_FIND_PACKAGE_TARGETS_GLOBAL` (CMake 3.24 and
newer). A target made with a plain `add_library(... IMPORTED)`, and any
`find_package` target on CMake older than 3.24, are still not followed.

Verified 2026-11-27 on `main` at `853f866`, CMake 3.28.

## Repro

```
deps/lib/CMakeLists.txt
deps/lib/core.c
deps/lib/top.c
app/build.tur
app/src/main.tur
```

`deps/lib/CMakeLists.txt`:

```cmake
cmake_minimum_required(VERSION 3.20)
project(lib C)
add_library(core STATIC core.c)
add_library(Vendor::core INTERFACE IMPORTED)
set_target_properties(Vendor::core PROPERTIES INTERFACE_LINK_LIBRARIES core)
add_library(top STATIC top.c)
target_link_libraries(top PRIVATE Vendor::core)
```

`deps/lib/core.c` and `deps/lib/top.c`:

```c
int core_value(void) { return 41; }
```

```c
int core_value(void);
int top_value(void) { return core_value() + 1; }
```

`app/build.tur`:

```turmeric no-check
(defpackage app
  :name "app"
  :version "0.1.0"
  :cmake-deps #map{
    "lib" #map{:path "../../deps/lib" :targets ["top"]}})
```

`app/src/main.tur`:

```turmeric
(defn answer [] : int
  ```c
  extern int top_value(void);
  return top_value();
  ```)

(defn main [] : int
  (println (answer))
  0)
```

```sh
cd app
tur fetch                 # exits 0, prints no warning
tur run src/main.tur
```

```
top.c:(.text+0x9): undefined reference to `core_value'
collect2: error: ld returned 1 exit status
```

`cmake/spice-deps-manifest.json` has
`"link_flags": [".../libtop.a", "Vendor::core"]`: the name, not `libcore.a`.
Adding `GLOBAL` to the `add_library` line gives
`[".../libtop.a", ".../libcore.a"]` and the program prints 42.

## Root cause

`_tur_walk_iface`, emitted by `emit_link_iface_helper`
(`src/compiler/pkg.c:3919`), tests each entry with `if(TARGET ...)`.
The walk is called from the generated top-level file, after
`FetchContent_MakeAvailable` / `add_subdirectory` has returned, so the
dependency's directory scope is gone and its non-global imported targets
with it. The entry falls through to the "plain library name" case. A name
containing `::` can never be a library file -- CMake itself insists such a
name is a target (policy CMP0028) -- so the consumer of the manifest drops
it without saying so.

## Fix directions

- Warn, at the least: an entry containing `::` that is not a target at the
  walk gets the same `spice: warning:` line an undecidable generator
  expression gets, naming the entry and the target that links it, instead of
  being written to the manifest.
- Run the walk where the targets exist.
  `cmake_language(DEFER DIRECTORY <dir> CALL ...)` (CMake 3.19) runs a
  command at the end of a given directory's processing, in its scope; the
  generated file can schedule the walk for each dependency's source
  directory and collect the results in global properties, as it already does
  for `_TUR_NEEDS_CXX`. A dependency whose targets live in nested
  subdirectories needs the deferral per subdirectory, which the `SUBDIRECTORIES`
  directory property enumerates.
- Or promote: `set_target_properties(<t> PROPERTIES IMPORTED_GLOBAL TRUE)`
  can only be done from the directory that created the target, so it has the
  same need for a deferred call.
