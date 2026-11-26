# An INTERFACE library in a `:cmake-deps` `:targets` list fails the CMake generate step

**Severity:** low-medium (hard configure failure with an error that names
tur's generated file, not the manifest; blocks header-only CMake targets and
"bundle" targets). Listing an `INTERFACE` library in `:targets` makes
`tur fetch` fail: the generated `cmake/CMakeLists.txt` emits
`$<TARGET_FILE_DIR:t>` for every target, and CMake rejects that expression on
a target with no artifact.

Found 2026-11-26 building `spices/manifold` in turmeric-spices, trying to hang
the C++ runtime on an INTERFACE wrapper target
([cmake-dep-cxx-runtime-not-linked](cmake-dep-cxx-runtime-not-linked.md)). The
same failure is what a header-only library's own CMake target
(`add_library(x INTERFACE)`) would hit.

## Repro (offline)

```sh
R=$(mktemp -d) && cd "$R" && mkdir -p deps/lib app/src
cat > deps/lib/CMakeLists.txt <<'EOF'
cmake_minimum_required(VERSION 3.20)
project(lib C)
add_library(top STATIC top.c)
add_library(bundle INTERFACE)
target_link_libraries(bundle INTERFACE top m)
EOF
printf 'int top_value(void) { return 42; }\n' > deps/lib/top.c
cat > app/build.tur <<'EOF'
(defpackage app
  :name "app"
  :version "0.1.0"
  :cmake-deps #map{"lib" #map{:path "../../deps/lib" :targets ["bundle"]}})
EOF
cat > app/src/main.tur <<'EOF'
(defn answer [] : int
  ```c
  extern int top_value(void);
  return top_value();
  ```)

(defn main [] : int
  (println (answer))
  0)
EOF
cd app && tur fetch && tur run src/main.tur
```

Expected: `42`, with `libtop.a` and `-lm` on the link line. Actual (turmeric
`main` at `d4ad1b0`):

```
CMake Error at CMakeLists.txt:132 (file):
  Error evaluating generator expression:

    $<TARGET_FILE_DIR:bundle>

  Target "bundle" is not an executable or library.

CMake Generate step failed.  Build files cannot be regenerated correctly.
spice: cmake configure failed (status 256)
spice: fetch completed with errors
```

With `:targets ["top"]` the same project fetches and runs.

## Root cause

`emit_link_lines` (`src/compiler/pkg.c:3670`) writes `link_dirs` as an
unguarded `$<TARGET_FILE_DIR:t>` per target (`pkg.c:3679`). The comment just
above it already knows the hazard for the artifact itself:

> `$<TARGET_FILE:...>` is an error on an INTERFACE library (a header-only dep
> has no artifact), so it is wrapped in a TYPE guard.

and `_tur_append_artifact` / `_tur_walk_iface` do guard on
`_TUR_LINKABLE` types. `link_dirs` was left out of that guard. So the
interface walk for an INTERFACE target is implemented and would produce the
right flags; the generate step dies first on the directory line.

## Fix directions

1. Guard `link_dirs` the way the artifact is guarded: emit the directory only
   when the target's `TYPE` is in `_TUR_LINKABLE`. Since `:targets` deps link
   by absolute artifact path through `link_flags`, `link_dirs` is redundant for
   them anyway and could be dropped for every `:targets` dep.
2. Pin it with a `spice-resolver-tests.sh` case next to
   `cmake-dir-markers-dropped-without-a-warning`: an INTERFACE target whose
   `INTERFACE_LINK_LIBRARIES` names a static library and a bare `m`.
