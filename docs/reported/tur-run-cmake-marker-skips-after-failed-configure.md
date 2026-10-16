# `tur run` skips the cmake-deps build after a failed configure

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
