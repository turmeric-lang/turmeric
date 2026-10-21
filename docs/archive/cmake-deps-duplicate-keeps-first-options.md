# Two spices declaring one `:cmake-deps` entry with different options: whichever is collected first wins, silently

**Severity:** medium (silent misconfiguration of a native dependency). When two
spices in one program declare the same `:cmake-deps` name with the same `:url`
and `:ref`, `tur` builds the dependency once, as it should, but with only one of
the two declarations. The other's `:options`, `:wasm-options`, `:targets`,
`:cmake-name`, `:link-libs` and `:link-flags` are dropped with no diagnostic.
Which declaration survives depends on the order of the consumer's `:spices`
map. `tur fetch` exits 0 either way.

Found 2026-10-17 while planning `wss://` for tur-nng
([nng-tls-plan](../upcoming/spices/nng-tls-plan.md)), which needs one extra
option on the `mbedtls` entry that three other spices already declare.

## Repro (offline; a local git repo stands in for the upstream)

```sh
R=$(mktemp -d) && cd "$R"
mkdir -p dep spice-a spice-b app/src
cat > dep/CMakeLists.txt <<'EOF'
cmake_minimum_required(VERSION 3.20)
project(dep C)
set(DEP_FLAVOR "default" CACHE STRING "which flavor of dep to build")
message(STATUS "dep: DEP_FLAVOR=${DEP_FLAVOR}")
add_library(dep STATIC dep.c)
target_compile_definitions(dep PRIVATE "DEP_FLAVOR=\"${DEP_FLAVOR}\"")
EOF
printf 'const char *dep_flavor(void) { return DEP_FLAVOR; }\n' > dep/dep.c
(cd dep && git init -q && git add -A &&
 git -c user.name=r -c user.email=r@example.com commit -qm dep && git tag v1)
for s in a b; do cat > spice-$s/build.tur <<EOF
(defpackage spice-$s
  :name "spice-$s"
  :version "0.1.0"
  :cmake-deps #map{
    "dep" #map{:url     "file://$R/dep"
               :ref     "v1"
               :targets ["dep"]
               :options #map{:DEP_FLAVOR "wanted-by-$s"}}})
EOF
done
cat > app/build.tur <<'EOF'
(defpackage app
  :name "app"
  :version "0.1.0"
  :spices #map{
    "spice-a" #map{:path "../spice-a"}
    "spice-b" #map{:path "../spice-b"}})
EOF
cat > app/src/main.tur <<'EOF'
(defn flavor [] : cstr
  ```c
  extern const char *dep_flavor(void);
  return dep_flavor();
  ```)

(defn main [] : int
  (println (flavor))
  0)
EOF
cd app && tur fetch && tur run src/main.tur     # prints wanted-by-b
```

`tur fetch` exits 0 and prints no warning. `cmake/CMakeLists.txt` has a single
`set(DEP_FLAVOR "wanted-by-b" CACHE STRING "" FORCE)`. Swap the two `:spices`
entries, delete `cmake/`, and the same commands print `wanted-by-a`.

Measured 2026-10-17 on `main` 3d9e02a2 (tur v0.65.5), Linux x86-64.

## Root cause

`append_cmake_dep_with_conflict_check` (`src/compiler/pkg.c:3634`) compares a
new entry with an existing one of the same name on `:url`, `:ref` and `:path`
only. When those three match it returns `true` and discards the new entry
whole. Its own comment says so: "Mismatched :cmake-name or :options is
silently kept on the existing copy". `docs/guides/package-manager-internals-guide.md`
documents the same behaviour ("the first copy wins silently").

"First" is collection order in `pkg_collect_transitive_cmake_deps`: the root
manifest's own entries, then a stack-based walk over `:spices`
(`worklist[--n_work]`, `pkg.c:4092`). The **last** spice listed in a `:spices`
map is visited first, so a consumer reordering its own dependencies changes how
a third party's native library is configured.

This is a departure from the plan that introduced the union.
[transitive-cmake-deps-plan](../archive/history/transitive-cmake-deps-plan.md)
Phase 3 said identical `:url`, `:ref`, `:options` and `:targets` should merge
silently and "any divergence" should be a hard error naming both manifests.

## Live instances in turmeric-spices (`main` fa42ece)

- **`mbedtls` v3.6.2**, declared by `http`, `tls` and `ws-client` with three
  different option sets: `ws-client` sets neither `USE_SHARED_MBEDTLS_LIBRARY`
  nor `USE_STATIC_MBEDTLS_LIBRARY`, and `http` adds `BUILD_SHARED_LIBS`. They
  happen to be equivalent today because mbedTLS's defaults match. That stops
  being true under nng-tls-plan, which needs
  `DISABLE_PACKAGE_CONFIG_AND_INSTALL "OFF"` on the entry. Without it, nng's
  `install(EXPORT)` refers to an unexported `mbedtls` target and the configure
  exits 1. The plan works around it by adding the option to every copy.
- **`raylib` 5.5**, declared by `raylib` (with `:wasm-options`
  `PLATFORM "Web"`, `GRAPHICS "GRAPHICS_API_OPENGL_ES3"`) and by `sdf-raylib`
  (with no `:wasm-options`). In a program using both, a web build that collects
  `sdf-raylib`'s copy first configures raylib with its default `Desktop`
  platform under `emcmake`. Not run here (no emsdk in the sandbox). The
  mechanism is the one the repro shows.

## Fix directions

1. **Union, and fail only on a real conflict.** Merge `:options` and
   `:wasm-options` key by key. Fail with both origins printed when one key has
   two different values. Union `:targets`. That accepts the three `mbedtls`
   copies as they are, lets a spice add a key the others did not think of
   (nng's `DISABLE_PACKAGE_CONFIG_AND_INSTALL`), and gives a web build the
   `raylib` spice's `:wasm-options` whichever copy came first. This is the
   change nng-tls-plan would most like to see. With it, the plan's edits to the
   sibling manifests become unnecessary.
2. **At minimum, warn.** Print both origins whenever anything other than
   `:url` / `:ref` / `:path` differs, which is what the plan's Phase 3 asked
   for.
3. A deliberate override of a conflicting value is a separate gap:
   [cmake-deps-no-cross-spice-override-or-order](cmake-deps-no-cross-spice-override-or-order.md).

A pin can follow the repro's shape: two spices, one dep, one option each,
asserting the merged `cmake/CMakeLists.txt` (union) or a non-zero exit
(conflict).

## Resolved 2026-10-21: fix direction 1, union with a hard error on a real conflict

`append_cmake_dep_with_conflict_check` (`src/compiler/pkg.c`) now merges a
same-name entry at the same `:url`/`:ref` (or `:path`) into the one already
collected (`merge_cmake_dep`): `:options` and `:wasm-options` key by key,
with a key set to two different values a hard error that names both
manifests and the key; `:targets`, `:link-libs` and `:link-flags` union; a
differing `:cmake-name` or `:cmake-version` is a conflict; `:prefer-system`
unions to "prefer the system package", with a note. The repro above now
configures `dep` with both spices' keys, and a true conflict exits 2:

```
tur: conflicting :cmake-deps option for "dep": DEP_FLAVOR
  ./../spice-c: "other"
  ./../spice-a: "shared"
```

Pinned in `tests/run-transitive-cmake-deps.sh` (`cmake-deps-option-union`,
`cmake-deps-option-conflict`, a local git repo as the upstream). The three
`mbedtls` copies in turmeric-spices merge as they are, and nng-tls-plan's
edits to the sibling manifests are no longer needed. A deliberate override
of a conflicting value is still
[cmake-deps-no-cross-spice-override-or-order](../reported/cmake-deps-no-cross-spice-override-or-order.md).
