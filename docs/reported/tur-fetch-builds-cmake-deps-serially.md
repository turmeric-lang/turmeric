# `tur fetch` builds `:cmake-deps` one file at a time

**Severity:** low (time only; the result is the same).

`tur fetch` runs `cmake --build <dir>` with no parallelism argument. With the
Unix Makefiles generator, CMake's default on Linux and macOS, that compiles
one translation unit at a time however many cores the machine has. Every cold
fetch of a native dependency pays for it, and so does each CI shard in
turmeric-spices, which fetches with `--update` on every run.

Measured 2026-11-27 on `main` at `853f866`, four cores, a cold fetch of
`spices/manifold` (Manifold v3.5.5, Clipper2, oneTBB v2022.3.0; 66 C++
translation units):

| Command | Wall time | CPU time |
|---|---|---|
| `tur fetch` | 3 min 44 s | 3 min 21 s |
| `CMAKE_BUILD_PARALLEL_LEVEL=4 tur fetch` | 1 min 5 s | 3 min 5 s |

The environment variable is CMake's own and already works; nothing in `tur`
or its guides mentions it.

## Root cause

`src/compiler/pkg.c:5676`:

```c
buf_puts(&cmd, "cmake --build ");
cmake_ok = pkg_cmd_arg(&cmd, cmake_bld) && cmake_ok;
```

No `--parallel`, for the native and the wasm build alike.

## Fix directions

- Pass `--parallel <n>` with `n` the number of online processors, unless
  `CMAKE_BUILD_PARALLEL_LEVEL` is set, in which case leave it to CMake. Give
  the number explicitly: `--parallel` with no value becomes a bare `make -j`
  under the Makefiles generator, which has no limit.
- A `tur fetch -j <n>` flag for the user who wants fewer.
- Parallel builds interleave compiler output. `tur fetch` prints the build
  log as it goes; an error is then harder to find, so on failure print the
  tail of the failing target's output again, or re-run the failed step
  serially.
- Until then, a line about `CMAKE_BUILD_PARALLEL_LEVEL` in
  `docs/guides/developing-spices-guide.md`.
