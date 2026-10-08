# `--target wasm` suppresses `CMAKE_POLICY_VERSION_MINIMUM`, so low-floor cmake deps fail only on the wasm arm

**Severity:** medium -- blocks `tur build --target wasm` for any project whose
`:cmake-deps` graph contains a dependency with
`cmake_minimum_required(VERSION <3.5)`, with an error the spice author cannot
act on. Native builds of the same manifest succeed.

**Status:** **RESOLVED 2026-09-30** by a5edd6df ("wasm target: make the
cmake-dep web arm reachable"), which dropped the `!wasm` conjunct exactly as
the fix direction below says; the line now reads
`if (cmake_major_version() >= 4 && !getenv("TUR_CMAKE_NO_POLICY_MIN"))` and
its comment records why (`src/compiler/pkg.c`, `pkg_cmake_build`). The report
was left in `docs/reported/` when that commit landed and was archived
2026-10-01. Originally verified 2026-09-30 with a minimal repro (below).

## Summary

`pkg_cmake_build` passes CMake 4's compatibility escape hatch **only on the
native arm**:

```c
/* src/compiler/pkg.c:4023 */
if (!wasm && cmake_major_version() >= 4 && !getenv("TUR_CMAKE_NO_POLICY_MIN"))
    buf_printf(&cmd, " -DCMAKE_POLICY_VERSION_MINIMUM=3.5");
```

The `!wasm` guard has no stated rationale, and the comment immediately above
it argues for the flag in terms that apply equally to both arms:

> a dependency that has not raised its floor (hiredis, and plenty of other
> stable C libraries) aborts the whole configure and *nothing* builds ... The
> spice author does not control that floor, so failing here punishes the wrong
> person.

Under CMake >= 4 that reasoning holds verbatim for `emcmake`. The result is an
arm-dependent failure: the identical manifest configures natively and dies for
wasm.

## Minimal repro

Needs CMake >= 4 (measured with 4.3.3) and Emscripten on PATH (5.0.5-git).

```sh
mkdir lowfloor && cd lowfloor
cat > CMakeLists.txt <<'CM'
cmake_minimum_required(VERSION 3.2)
project(lowfloor C)
add_library(lowfloor STATIC a.c)
CM
echo 'int lf(void){return 1;}' > a.c

# What pkg.c does on the NATIVE arm (flag passed) -- succeeds:
cmake -S . -B b-native -DCMAKE_POLICY_VERSION_MINIMUM=3.5   # exit 0

# What pkg.c does on the WASM arm (flag suppressed) -- fails:
emcmake cmake -S . -B b-wasm                                # exit 1

# The same wasm configure with the flag -- succeeds:
emcmake cmake -S . -B b-wasm-fix -DCMAKE_POLICY_VERSION_MINIMUM=3.5  # exit 0
```

Measured results:

| Arm | Flag | Exit | Message |
|---|---|---|---|
| native | passed (pkg.c does) | 0 | -- |
| wasm | suppressed (pkg.c does) | **1** | `Compatibility with CMake < 3.5 has been removed from CMake.` |
| wasm | passed | 0 | -- |

## Root cause

`src/compiler/pkg.c:4023`, the `!wasm &&` conjunct.

## Why it has not been hit yet

The obvious first web dep is raylib, and raylib 5.5's own floor is
`cmake_minimum_required(VERSION 3.5)` -- exactly at CMake 4's cutoff, so it
passes either way. Its bundled GLFW *is* below the floor
(`3.4...3.28`) but is only added on the `PLATFORM=Desktop` path, so a correct
`PLATFORM=Web` configure never reaches it. Verified: `emcmake cmake
-DPLATFORM=Web` on raylib 5.5 configures and builds clean under CMake 4.3.3
with no policy flag.

So the bug is latent behind raylib specifically and shows up on the second
cmake dep -- `hiredis`, the case pkg.c's own comment names, among others.

## Fix directions

Drop the `!wasm` conjunct:

```c
if (cmake_major_version() >= 4 && !getenv("TUR_CMAKE_NO_POLICY_MIN"))
    buf_printf(&cmd, " -DCMAKE_POLICY_VERSION_MINIMUM=3.5");
```

The comment's justification for suppressing it natively on CMake 3.x (the
variable goes unused and CMake reports it under "Manually-specified variables
were not used by the project", which is noise on every configure) is already
handled by the `cmake_major_version() >= 4` test, which is arm-independent.
`TUR_CMAKE_NO_POLICY_MIN=1` remains the opt-out.

Worth a fixture if the wasm cmake-dep path ever gets one; today nothing in
`tests/` configures a cmake dep under `emcmake`.

## Related

Found while resolving the open questions in
`docs/upcoming/hold/wasm-spices-plan.md` (Q2). That plan's section 3 records
the same line, and its W1.5 phase is where a wasm cmake-dep configure first
gets exercised.
