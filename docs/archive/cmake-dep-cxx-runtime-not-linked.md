# A C++ `:cmake-deps` library links without the C++ runtime (undefined `operator new`, `__cxa_*`)

**Severity:** medium (link failure; every static C++ dependency, on every
platform). A `:cmake-deps` library written in C++ builds, and the program that
uses it then fails to link: tur links emitted C with `cc`, and nothing puts
libstdc++ or libc++ on that line.

Found 2026-11-26 building `spices/manifold` in turmeric-spices
([Manifold](https://github.com/elalish/manifold) is C++17 with a C API). The
`developing-spices-guide` table calls `:cmake-deps` "C/C++ library
dependencies", and `package-management-guide` says the block "declares C and
C++ packages to link against".

## Repro (offline; one tiny C++ library)

```sh
R=$(mktemp -d) && cd "$R" && mkdir -p deps/vecsum app/src
cat > deps/vecsum/CMakeLists.txt <<'EOF'
cmake_minimum_required(VERSION 3.20)
project(vecsum CXX)
add_library(vecsum STATIC vecsum.cpp)
EOF
cat > deps/vecsum/vecsum.cpp <<'EOF'
#include <vector>
extern "C" int vecsum(int n) {
  std::vector<int> v;
  for (int i = 1; i <= n; i++) v.push_back(i);
  int s = 0;
  for (int x : v) s += x;
  return s;
}
EOF
cat > app/build.tur <<'EOF'
(defpackage app
  :name "app"
  :version "0.1.0"
  :cmake-deps #map{"vecsum" #map{:path "../../deps/vecsum" :targets ["vecsum"]}})
EOF
cat > app/src/main.tur <<'EOF'
(defn answer [] : int
  ```c
  extern int vecsum(int);
  return vecsum(10);
  ```)

(defn main [] : int
  (println (answer))
  0)
EOF
cd app && tur fetch && tur run src/main.tur
```

Expected: `55`. Actual, on Linux with GNU ld (turmeric `main` at `d4ad1b0`):

```
libvecsum.a(vecsum.cpp.o): undefined reference to `std::__throw_length_error(char const*)'
libvecsum.a(vecsum.cpp.o): undefined reference to `operator delete(void*, unsigned long)'
libvecsum.a(vecsum.cpp.o): undefined reference to `operator new(unsigned long)'
libvecsum.a(vecsum.cpp.o): undefined reference to `__gxx_personality_v0'
collect2: error: ld returned 1 exit status
tur: cc invocation failed (status 256)
```

`cmake/spice-deps-manifest.json` has `"link_flags": [".../libvecsum.a"]` and
nothing else.

## Root cause

The link line is assembled from what CMake *targets* report:
`$<TARGET_FILE:t>` plus the walk over `INTERFACE_LINK_LIBRARIES`
(`_tur_walk_iface`, `src/compiler/pkg.c:3735`). The C++ runtime is on neither.
It is a property of the C++ *compiler driver*, which adds it implicitly when
`c++` links -- so no target lists it -- and tur's link command is
`cc ... -o out` (`drv_link_command_run`, `src/driver/cc.c:1279`).

A shared C++ dependency should not show this, since a `.so` / `.dylib` records
its own dependency on the runtime. That was not tested here.

## Why the obvious workaround is not enough

```turmeric
:cmake-deps #map{"vecsum" #map{:path "../../deps/vecsum" :targets ["vecsum"]
                               :link-flags ["-lstdc++"]}}
```

fixes Linux and is wrong on macOS, where the runtime is libc++ (`-lc++`), and
`build.tur` has no per-platform key to choose between them. What
`spices/manifold` does instead is wrap the dependency in a `:path` CMake
project that asks CMake and records the answer on the target:

```cmake
foreach(_lib IN LISTS CMAKE_CXX_IMPLICIT_LINK_LIBRARIES)
  if(_lib MATCHES "^(stdc\\+\\+|c\\+\\+|c\\+\\+abi)$")
    set_property(TARGET manifoldc APPEND PROPERTY INTERFACE_LINK_LIBRARIES "${_lib}")
  endif()
endforeach()
```

That works (verified on Linux; the manifest gains a bare `stdc++`, which the
consumer spells `-lstdc++`), but every C++ spice would have to carry the same
shim, and a spice that fetches by `:url` has nowhere to put it.

## Fix directions

1. **Have the generated `cmake/CMakeLists.txt` do what the shim does.** For
   each dep, if any static library reached in the link walk has C++ sources,
   append the C++ runtime entries of `CMAKE_CXX_IMPLICIT_LINK_LIBRARIES` to
   that dep's `link_flags`. `project(SpiceDeps)` (`pkg.c:5038`) names no
   languages, so CMake enables C and CXX and the variable should be set there
   already. This is the smallest change and is correct per toolchain.
2. **Link with the C++ driver when a dep needs it.** Record "needs C++" in the
   manifest and have `drv_link_command_run` use `c++` (or `$CXX`) for the link
   step only. More general -- it also covers whatever else the C++ driver adds
   -- but it changes which program links every such project.
3. At minimum, a `tur fetch` warning when a `:targets` entry is a static
   library with C++ sources, naming the missing runtime. The current failure is
   a page of mangled symbols that never mentions tur.

## Not established

How `spices/rtaudio` and `spices/rtmidi` -- both C++ libraries behind a C API,
declared with a plain `:url` and no `:targets` -- link today. They were not
built while investigating this (they need ALSA headers the sandbox lacked).
If they link, something about their shape supplies the runtime and is worth
knowing before choosing a fix.

## Resolved 2026-11-26: fix direction 1

The generated `cmake/CMakeLists.txt` now does what the shim did. The link walk
calls `_tur_note_cxx` on every target it reaches; that records a static (or
object) library as C++ when its `LINKER_LANGUAGE` is `CXX`, when one of its
`SOURCES` has an extension in `CMAKE_CXX_SOURCE_FILE_EXTENSIONS`, or, for an
imported target, when `IMPORTED_LINK_INTERFACE_LANGUAGES` names `CXX`. When
one was recorded, `_tur_append_cxx_runtime` appends the `stdc++` / `c++` /
`c++abi` entries of `CMAKE_CXX_IMPLICIT_LINK_LIBRARIES` to that dep's
`link_flags`, after its archives and before its `:link-flags`
(`emit_link_flags_prelude`, `src/compiler/pkg.c`). A dep with no `:targets` is
checked by the name it links, which covers the `rtaudio` / `rtmidi` shape.

The repro prints `55`, with `"link_flags": [".../libvecsum.a", "stdc++"]`.
Manifold v3.5.5 fetched by a plain `:url` with `:targets ["manifoldc"]` links
and runs with no wrapper project. Verified on Linux with GNU; the macOS branch
(`c++`) is the same code path but was not run. Pinned as
`cmake-dep-cxx-runtime-linked` in `tests/spice-resolver-tests.sh`, which also
checks that a C library in the same project gets no runtime.

The "Not established" question is answered by reading the two manifests:
`rtaudio` and `rtmidi` set `BUILD_SHARED_LIBS OFF` and declare nothing that
would supply the runtime, so they had this bug too.
