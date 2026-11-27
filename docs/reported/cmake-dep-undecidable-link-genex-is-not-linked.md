# A `:cmake-deps` link entry behind a generator expression tur cannot decide at configure time is left out

**Severity:** low (the program fails to link, but `tur fetch` now names the
entry and says what to do).

Since #1486 the link walk reduces the generator expressions it can decide
while configuring: `BOOL`, `NOT`, `AND`, `OR`, `IF`, `STREQUAL`, `PLATFORM_ID`,
`CONFIG`, the compiler-id tests, `LINK_ONLY`, `BUILD_INTERFACE` and a few
more. Anything else is still not linked. It is no longer silent: `tur fetch`
prints a `spice: warning:` line. The remedy it offers is `:link-flags`, which
is a string in `build.tur` and cannot name a path inside the fetched build
tree, so for a library the dependency builds itself there is no clean
workaround.

Verified 2026-11-27 on `main` at `853f866`, CMake 3.28.

## Repro

The layout of
[cmake-dep-directory-scoped-imported-target-not-followed](cmake-dep-directory-scoped-imported-target-not-followed.md),
with this `deps/lib/CMakeLists.txt`:

```cmake
cmake_minimum_required(VERSION 3.20)
project(lib C)
add_library(core STATIC core.c)
add_library(top STATIC top.c)
target_link_libraries(top PRIVATE
  $<$<VERSION_GREATER_EQUAL:${CMAKE_C_COMPILER_VERSION},4.0>:core>)
```

```sh
cd app
tur fetch
tur run src/main.tur
```

```
spice: warning: link dependency '$<LINK_ONLY:$<$<VERSION_GREATER_EQUAL:13.3.0,4.0>:core>>' of target 'top' is a generator expression tur cannot evaluate; it is NOT linked. Add what it names with :link-flags.
```

```
top.c:(.text+0x9): undefined reference to `core_value'
collect2: error: ld returned 1 exit status
```

Expressions seen in real projects that the walk does not reduce: the version
comparisons (`VERSION_LESS`, `VERSION_GREATER_EQUAL`, ...),
`TARGET_PROPERTY`, `IN_LIST`, `COMPILE_LANGUAGE` / `LINK_LANGUAGE` and their
`_AND_ID` forms, `TARGET_LINKER_FILE` and the other `TARGET_*_FILE` family
outside the artifact the walk asks for itself.

## Root cause

`_tur_reduce_genex` (`src/compiler/pkg.c:3742`) is a hand-written reducer
over a fixed list of expression names. It exists because the walk has to
decide, at configure time, whether an entry names a target to recurse into,
and CMake evaluates generator expressions only at generate time. What the
reducer does not recognise it cannot classify.

## Fix directions

- Add the cheap ones: the six version comparisons and `IN_LIST` are pure
  string functions of their arguments, like `STREQUAL`.
- Let CMake evaluate the rest. The manifest is already written with
  `file(GENERATE)`, so an undecided entry can be written as
  `$<GENEX_EVAL:...>` and resolved at generate time. That yields a name, not
  a walk: if the name is a target, its own link interface is not followed.
  `$<TARGET_GENEX_EVAL:tgt,$<TARGET_PROPERTY:tgt,INTERFACE_LINK_LIBRARIES>>`
  gives one level; a fixed depth of nesting would cover most dependency
  graphs.
- Make the warning actionable for a library in the build tree: accept a
  target name in a `:link-targets` key, resolved to its artifact the way
  `:targets` entries are.
