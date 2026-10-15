# The `TSan` CMake configuration does not build: test executables fail to link

**Severity: low.** Nothing builds this configuration today: the nightly TSan
workflow, `tests/run-tsan.sh` and `tur run tsan` all use a Release tree. But
`src/CMakeLists.txt` still advertises `TSan` as a build type, and a contributor
who configures it gets thousands of linker errors before `tur` is any use.
Found 2026-10-15 while giving `release` and `tsan` their own build trees
(the Justfile's `tsan` recipe used to pass `--config TSan` to the shared Debug
`build/`, which never selected this configuration at all).

## Repro

```sh
cmake -S . -B build-tsan-cfg -DCMAKE_BUILD_TYPE=TSan -DCMAKE_POLICY_VERSION_MINIMUM=3.5
cmake --build build-tsan-cfg -j
```

`tur` itself links. Then, among others:

```
[100%] Linking C executable ../tur_reactor_slot_reuse_unit
/usr/bin/ld: ... undefined reference to `__tsan_read8'
gmake[2]: *** [src/CMakeFiles/tur_reactor_slot_reuse_unit.dir/build.make:99: tur_reactor_slot_reuse_unit] Error 1
```

Seen failing: `tur_reactor_slot_reuse_unit`, `tur_reactor_linger_unit`,
`tur_gc_runtime_copy_parity`. The build was stopped there (it had produced
over 300 MB of linker output), so the full list is likely longer.

## Root cause

Under `TSan`, `tur_core`, `libturi` and the other runtime archives compile
with `-O1 -g -fsanitize=thread` (`src/CMakeLists.txt`, the
`$<$<CONFIG:TSan>:...>` generator expressions). Only `tur` adds
`-fsanitize=thread` at link time. Every test or helper executable that links
those archives spells its sanitizer flags per target, for `Debug` only:

```cmake
target_link_options(tur_gc_runtime_copy_parity PRIVATE -pthread
    $<$<CONFIG:Debug>:   ${TUR_ASAN_FLAGS}>
)
```

so the instrumented objects reach a link without the TSan runtime.

## Fix directions

1. Put the link flag on the archives' interface, so every consumer gets it:
   `target_link_options(libturi INTERFACE $<$<CONFIG:TSan>:-fsanitize=thread>)`
   (and the same for any other instrumented archive an executable links).
2. Or one directory-wide `add_link_options($<$<CONFIG:TSan>:-fsanitize=thread>)`
   near the top of `src/CMakeLists.txt`.
3. Or delete the `TSan` configuration. The fixture programs already carry
   `-fsanitize=thread` under `TUR_TSAN=1`, which is what has found every race
   so far (`.github/workflows/tsan.yml`). What the configuration would add is
   instrumentation of `tur` and its runtime archives themselves.

Whichever is chosen, `tur run tsan` could then build this configuration again
if instrumenting `tur` turns out to be worth its cost (the Justfile comment on
`tsan` points here).
