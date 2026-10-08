# A `:path` cmake-dep reached transitively is absolutized, then re-prefixed

**RESOLVED 2026-08-28.** The emitter no longer prefixes an already-absolute
`:path` (new `cmake_dep_path_dir`, making the same leading-'/' test the
`:spices` walk already made), and the collector now always produces an
absolute path so the two cannot disagree. The base-directory defect this
was masking is
[cmake-dep-path-base-dir-inconsistent](cmake-dep-path-base-dir-inconsistent.md),
fixed in the same pass.

**Severity: medium (blocker for the spices that use it)** -- the generated
`cmake/CMakeLists.txt` contains an `add_subdirectory` with a `./` glued onto an
absolute path, so CMake configure fails and no dependency builds. Found
2026-08-28 while trying to build `spices/opengl` to validate the
`:cmake-deps` link-line fix. **Pre-existing** -- reproduces identically on
`tur v0.40.0` at `423f6546`, before that change.

## Summary

A `:path` cmake-dep declared by **the spice you are building** emits correctly.
The same dep reached **transitively** (through a workspace sibling's `:spices`
graph) has already been rewritten to an absolute path by the transitive
resolver, and the emitter then prefixes it again.

From one generated `cmake/CMakeLists.txt` in `spices/opengl` -- both lines, one
file:

```cmake
# line 10 -- opengl's OWN :path dep. Correct.
add_subdirectory("././../cmake-deps/opengl" "${CMAKE_BINARY_DIR}/_local/opengl-deps-build")

# line 58 -- raygui's :path dep, reached transitively. Broken.
add_subdirectory(".//Users/.../spices/raygui/../cmake-deps/raygui" "${CMAKE_BINARY_DIR}/_local/raygui-build")
```

`add_subdirectory(".//Users/...")` is a relative path to CMake, so it fails.

## The other invocation path doubles it differently

`tur build <dir>` produces the same defect with the project dir instead of `.`,
which is the shape that names both halves at once:

```
CMake Error at CMakeLists.txt:10 (add_subdirectory):
  add_subdirectory given source
  "/Users/.../spices/opengl//Users/.../spices/opengl/../cmake-deps/opengl"
  which is not an existing directory.
```

That is `project_dir + "/" + d->path` where `d->path` is already absolute --
`pkg.c`'s local-path branch (`snprintf(abs_path, ..., "%s/%s", project_dir,
d->path)`) assumes `d->path` is relative, which is true as parsed from
`build.tur` and false after transitive resolution.

## Repro

```sh
git clone https://github.com/turmeric-lang/turmeric-spices
cd turmeric-spices/spices/opengl
tur fetch          # look at cmake/CMakeLists.txt: the raygui line has .//Users/...
tur build .        # CMake Error: add_subdirectory given source ... doubled path
```

## Why CI does not catch it

The spices CI runs `tur fetch` from each spice directory and treats a fetch
failure as a `::warning::` rather than an error (see
[spices-ci-fetch-failure-downgraded-to-warning](spices-ci-fetch-failure-downgraded-to-warning.md)),
so a spice whose transitive `:path` dep never configured proceeds to the build
step and fails later with a misleading missing-library error. The two reports
compound: this one produces the failure, that one hides its cause.

## Fix direction

Normalize once and record which form is stored. Either have the transitive
resolver keep `:path` entries relative to the manifest that declared them (and
carry that manifest's dir alongside), or have the emitter test
`d->path[0] == '/'` and skip the prefix. The second is a two-line change and
the first is the one that stays correct when a third caller appears.

Worth confirming whether `:path` deps in `:spices` (as opposed to
`:cmake-deps`) have the same asymmetry.

## Guides to update when fixed

- docs/guides/developing-spices-guide.md -- the `:cmake-deps` `:path` section,
  if the stored form becomes part of the documented contract.
