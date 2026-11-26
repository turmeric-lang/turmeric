# A `:cmake-deps` link dependency written as a generator expression is dropped (undefined reference)

**Severity:** medium (link failure; latent in any library that links a
dependency conditionally). When a target lists a dependency through a
generator expression -- the usual CMake spelling for an optional one,
`$<$<BOOL:${WITH_X}>:x>` -- tur's link walk skips the entry, so the dependency
never reaches the link line and the program fails with undefined references.
Nothing is printed when the entry is skipped.

Found 2026-11-26 building `spices/manifold` in turmeric-spices. Manifold links
its parallel backend this way
(`target_link_libraries(manifold PRIVATE $<$<BOOL:${MANIFOLD_PAR}>:TBB::tbb>)`),
so `MANIFOLD_PAR=ON` cannot work through `tur fetch`; the spice ships
single-threaded because of it.

## Repro (offline; two tiny local C libraries)

```sh
R=$(mktemp -d) && cd "$R" && mkdir -p deps/lib app/src
cat > deps/lib/CMakeLists.txt <<'EOF'
cmake_minimum_required(VERSION 3.20)
project(lib C)
option(WITH_CORE "link core" ON)
add_library(core STATIC core.c)
add_library(top STATIC top.c)
target_link_libraries(top PRIVATE $<$<BOOL:${WITH_CORE}>:core>)
EOF
printf 'int core_value(void) { return 41; }\n' > deps/lib/core.c
printf 'int core_value(void);\nint top_value(void) { return core_value() + 1; }\n' > deps/lib/top.c
cat > app/build.tur <<'EOF'
(defpackage app
  :name "app"
  :version "0.1.0"
  :cmake-deps #map{"lib" #map{:path "../../deps/lib" :targets ["top"]}})
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

Expected: `42`. Actual (turmeric `main` at `d4ad1b0`):

```
libtop.a(top.c.o): in function `top_value':
top.c:(.text+0x9): undefined reference to `core_value'
collect2: error: ld returned 1 exit status
tur: cc invocation failed (status 256)
```

`cmake/spice-deps-manifest.json` has `"link_flags": [".../libtop.a"]`.

**Control.** Replace the last CMake line with
`target_link_libraries(top PRIVATE core)` and the manifest becomes
`[".../libtop.a", ".../libcore.a"]` and the program prints `42`. The only
difference is the generator expression.

## Root cause

`top`'s `INTERFACE_LINK_LIBRARIES` holds `$<LINK_ONLY:$<$<BOOL:ON>:core>>`.
`_tur_walk_iface` (`src/compiler/pkg.c:3735`) unwraps `$<LINK_ONLY:...>` and
then discards anything that still starts with `$<`:

```cmake
string(REGEX REPLACE "^\\$<LINK_ONLY:(.*)>$" "\\1" _e "${_e}")
if(NOT _e MATCHES "^\\$<" AND NOT _e MATCHES "^::@")      # pkg.c:3747
```

The walk runs at configure time, where a generator expression cannot be
evaluated, so it has no way to tell what `$<$<BOOL:ON>:core>` names. The
consumer's `link_flag_token_skipped` (`pkg.c:5761`) drops unevaluated genexes
as well, by design: the comment above it notes they "survive into the JSON as
literal text".

## Fix directions

1. **Let the generate step evaluate it.** A generator expression can only be
   evaluated at generate time, and `file(GENERATE)` is already how the
   manifest is written. `$<GENEX_EVAL:$<TARGET_PROPERTY:top,INTERFACE_LINK_LIBRARIES>>`
   there should reduce `$<$<BOOL:ON>:core>` to `core`. What comes back is a
   name, which may be a target or a library, so the classification the walk
   does today would have to move with it (`$<TARGET_NAME_IF_EXISTS:...>` /
   `$<TARGET_EXISTS:...>` exist for that). Not tried here.
2. **Handle the common shapes textually.** `$<$<BOOL:lit>:x>`,
   `$<BUILD_INTERFACE:x>` and `$<TARGET_NAME_IF_EXISTS:x>` cover most optional
   dependencies and can be reduced at configure time without a full evaluator.
   Narrow, but it would have covered this case.
3. **Say so.** Whichever of these lands, an entry the walk cannot resolve
   should print one line -- `spice: link dependency '$<...>' of target 'top'
   is a generator expression tur cannot follow` -- instead of vanishing. The
   failure today names a symbol, not the dependency that was dropped or why.
