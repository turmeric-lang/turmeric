# `tur run` skips the cmake-deps build after a failed configure

**RESOLVED 2026-10-17** -- see [Resolution](#resolution-2026-10-17). Pinned by
`run-retries-failed-cmake-dep-build` (`tests/run-transitive-cmake-deps.sh`).

**Severity:** medium. After one failed dependency build, every later `tur run`
builds with no dependency flags at all, and the real error is gone.

## What happens

`src/main.c:cmd_run` decides whether the project's `:cmake-deps` are already
built by checking that `cmake/CMakeLists.txt` exists:

```c
snprintf(cmake_lists, sizeof(cmake_lists), "%s/cmake/CMakeLists.txt", root);
bool cmake_built = (stat(cmake_lists, &_cmst) == 0);
if (!cmake_built) { pkg_gen_cmake_deps(...); pkg_cmake_build(...); }
```

`pkg_gen_cmake_deps` writes that file **before** `pkg_cmake_build` runs, so a
configure or build failure leaves it behind. The next `tur run` skips the
dependency build and compiles the program without the `-I/-L/-l` flags
from `cmake/spice-deps-manifest.json`. It then fails on a missing header or
symbol, which points away from the real cause.

`tur build <dir>` already fixed the same check: it keys on the per-target
`spice-deps-manifest*.json` instead. Its comment explains why the shared
`CMakeLists.txt` is the wrong marker.

## Fix directions

- Use the same marker as `tur build <dir>`
  (`cmake/<pkg_cmake_manifest_name(target)>`), and remove a stale
  `CMakeLists.txt` when the build fails.

## Resolution (2026-10-17)

`cmd_run` (`src/main.c`) keys "deps built" on the dep manifest the compile
reads (`cmake/<pkg_cmake_manifest_name(NULL)>`), as `tur build <dir>` already
did. CMake writes that only on a successful configure. A configure can
succeed and the build after it still fail, so both commands also remove the
manifest when the dependency build fails, and the next run retries.

Pinned by `run-retries-failed-cmake-dep-build` in
`tests/run-transitive-cmake-deps.sh`: a `:path` dep whose configure always
fails must fail the second `tur run` the same way as the first.
