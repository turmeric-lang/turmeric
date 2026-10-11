# `cmake-deps/` shim follow-up: what the turmeric link-line fix makes removable

**Status: done -- 2026-10-10.** Both items that were outstanding are done in
turmeric-lang/turmeric-spices#95; the only shim left is opengl's, for glad
(a code generator), which this audit always expected to stay.

- **postgres** is a `:prefer-system` dep with `:cmake-name "PostgreSQL"`,
  `:targets ["PostgreSQL::PostgreSQL"]` and no `:url`; the `pq` re-export
  target and `cmake-deps/postgres/` are gone. The Homebrew keg-only probe
  became `:options #map{:PostgreSQL_ROOT "/opt/homebrew/opt/libpq"}`, which
  needed a compiler change: a `:prefer-system` dep's `:options` used to be
  written only inside the fetch fallback, i.e. after the `find_package` they
  were meant to steer. They now land before it. The same change makes a
  `:prefer-system` dep with no `:url` system-only: when nothing is found,
  configure stops on a message naming the dep and the package, instead of an
  empty `FetchContent_Declare` failing on "No download info given",
  `--refetch` no longer skips its `find_package`, and `tur audit` lists it as
  system-only rather than as unpinned forever. Tests SF4/SF5 in
  `tests/spice-resolver-tests.sh`. What the static hint loses against the
  shim's `brew --prefix libpq`: an Intel Mac or a custom Homebrew prefix,
  which sets `PostgreSQL_ROOT` in the environment instead (FindPostgreSQL
  reads it).
- **raygui** vendors `raygui.h` 4.0 under `c/raygui/` and lists
  `raygui_impl.c` in `:c-sources` -- the destination guessed below, and no
  compiler change was needed. One trap, worth knowing for any single-header
  library moved this way: `raygui/core.tur`'s inline-C also defined
  `RAYGUI_IMPLEMENTATION`. Under the shim that was harmless -- whenever it took
  effect, the archive member holding the other copy was simply never pulled --
  but a `:c-sources` object is always linked, so `tur build` failed on a
  duplicate of every `Gui*` symbol. Exactly one TU may define it.

**Previous status: partly landed -- updated 2026-08-29; re-checked 2026-10-07.** Both
items under "Still outstanding" below are still outstanding on turmeric-spices
`origin/main`: `spices/postgres/build.tur` still declares `:path
"../cmake-deps/postgres"` with `:targets ["pq"]` (and its comment still
explains the `-lPostgreSQL` workaround), and `spices/raygui/cmake-deps/raygui/`
still carries `raygui_impl.c`. The rest of this document, including the
sections written before the fix landed, is historical context for those two. rjungemann/turmeric#791 is
merged to turmeric `main`, so the changes that were gated on it are now done in
this branch:

- **The CI workspace-root workaround is removed.** Verified against the merged
  compiler: `tur fetch` in `spices/opengl` with the workspace root in place
  emits exactly one dep (`opengl-deps`), not 17.
- **The opengl shim's `set_target_properties(glfw PROPERTIES OUTPUT_NAME glfw)`
  is removed.** glfw now builds as `libglfw3.a` under its own name and tur
  links it by `$<TARGET_FILE:glfw>`; `tur fetch` and `tur build .` both exit 0
  and all eight modules type-check.
- **glad is vendored**, so `python3-jinja2` is gone from CI and no spice needs
  Python to build.

**Still outstanding** (not gated on anything upstream, just untested here):

- **postgres**: move to `:prefer-system` + `:targets ["PostgreSQL::PostgreSQL"]`
  and drop the `pq` re-export target, keeping the Homebrew keg-only
  `PostgreSQL_ROOT` probe. Not done because libpq is not installed on the box
  this was written on, so it could not be verified.
- **raygui**: the implementation TU (`raygui_impl.c`) still needs somewhere to
  live; `:c-sources` is the plausible destination and that is a redesign, not a
  deletion.

**Original status:** Nothing in this document should land until the turmeric
change that motivates it is merged and CI is building a compiler that has it.
Filed 2026-08-28 so the work is not lost.

## Update 2 (2026-08-28): `spices/opengl` builds

The last upstream blocker is fixed. `pkg_collect_transitive_cmake_deps` no
longer seeds every workspace `:members` sibling, so building one spice no
longer configures every member's native deps -- for `spices/opengl` that was
**15** dependencies (mbedtls, sqlite3, libpq, rtaudio, ...) where it needs
glfw and glad. It is now 1.

`spices/opengl` fetches, configures, builds `libglfw.a` + `libglad.a`, and
`tur build .` succeeds on macOS. `spices/raygui`'s raylib links with no shim.
Both spices the framework report listed as compiler-blocked are now unblocked.

Two consequences for this repo:

1. **The CI workaround in `.github/workflows/ci.yml` is redundant, but not
   yet removable.** The "Fetch C dependencies" step moves the root `build.tur`
   aside so the directory stops looking like a workspace, precisely to stop the
   compiler pulling in all 17 native libs. The job checks out
   `turmeric-lang/turmeric` with no `ref:`, i.e. **the default branch**, so the
   hack cannot come out until the scoping fix is on turmeric `main` -- a merged
   PR elsewhere is not enough. Left in place with the removal condition and a
   verification recipe recorded in the comment. Its original comment is the
   best statement of the problem anyone wrote; worth preserving in whatever
   commit finally removes it.
2. **glad's `jinja2` prerequisite is gone** -- resolved in this branch by
   vendoring the generator's output (`spices/opengl/cmake-deps/opengl/glad/`,
   ~1.4 MB), the same thing raylib does with its own checked-in copy. The
   `python3-jinja2` line in CI's apt install is now unnecessary for this spice.
   Verified with no jinja2 installed: fetch and build both succeed with zero
   Python invocations.

## Background

Two turmeric reports were fixed together on 2026-08-28:

- `cmake-deps-cannot-express-framework` -- `-framework Cocoa` could not be
  spelled at any layer, so no Cocoa-backed dep linked on macOS.
- `cmake-deps-link-name-not-overridable` -- the `-l` name was derived from the
  CMake target's *name*, with no override.

Both had the same root cause: `tur` reconstructed a link line from a target
name by string manipulation. It now asks CMake instead --
`$<TARGET_FILE:tgt>` for the artifact and
`$<TARGET_PROPERTY:tgt,INTERFACE_LINK_LIBRARIES>` for transitive requirements
(which is where `-framework` lives) -- and adds `:link-libs` / `:link-flags`
overrides on a `:cmake-deps` entry.

The three shims in this repo were written against the old behavior, and their
comments say so explicitly. This is a per-shim audit of what changes.

## The headline: no shim disappears entirely

Each shim mixes "work around the tur bug" with "do real work". Only the first
part goes.

### `spices/postgres/cmake-deps/postgres` -- mostly removable

Its own comment states the reason it exists:

> tur derives the -l name from the CMake target's basename, so
> `PostgreSQL::PostgreSQL` became `-lPostgreSQL` -- but the file is libpq.so
> ... The target name and the library's base name simply differ here, and
> nothing in the `:cmake-deps` surface lets them differ.

That is exactly what was fixed. `$<TARGET_FILE:PostgreSQL::PostgreSQL>` now
resolves to the real `libpq` artifact, and `:link-libs ["pq"]` is available as
an explicit override. The re-export target (`add_library(pq ...)`) is no longer
needed.

**What must stay:** the Homebrew probe. libpq is keg-only, so `FindPostgreSQL`
needs `PostgreSQL_ROOT` pointed at `brew --prefix libpq`. That is not a tur
bug and has no `:cmake-deps` equivalent today, so either the shim keeps
existing solely for that, or the probe moves into `:options`.

**Proposed:**

```turmeric
:cmake-deps #map{
  "libpq" #map{:prefer-system true
               :cmake-name    "PostgreSQL"
               :targets       ["PostgreSQL::PostgreSQL"]}
}
```

plus whatever carries `PostgreSQL_ROOT` on macOS. Untested here -- libpq is not
installed on the box this was written on.

### `spices/opengl/cmake-deps/opengl` -- half removable

Two reasons, one fixed:

- **glfw** (fixed). The shim notes the target is `glfw` but `OUTPUT_NAME` is
  `glfw3`, and the archive lands in `<BINARY_DIR>/src`. It compensates with
  `set_target_properties(glfw PROPERTIES OUTPUT_NAME glfw)` -- renaming a
  target purely so the old `-l` derivation would resolve. **That line can go.**
  Verified directly against upstream glfw 3.4 with the new compiler: a spice
  declaring `:targets ["glfw"]` links `libglfw3.a` by path and picks up
  `-framework Cocoa -framework IOKit -framework CoreFoundation` from
  `INTERFACE_LINK_LIBRARIES`, with no shim and no rename.
- **glad** (not fixed, and not a tur bug). glad 2.0.6 is a Python *generator*;
  its checkout has no root `CMakeLists.txt` and builds nothing. Something must
  still generate the loader. This half of the shim stays.

The `GLFW_BUILD_WAYLAND OFF` workaround for headless Linux runners is also
unrelated to the fix, though it could move to `:options` if the shim were
otherwise emptied -- which it is not, because of glad.

### `spices/raygui/cmake-deps/raygui` -- not removable

raygui is header-only *with an implementation translation unit*
(`raygui_impl.c` lives in the shim). `:link-libs []` now expresses "contribute
include dirs, link nothing", which is the right shape for the *link* side, but
something still has to *compile* the implementation. `:c-sources` is the
plausible destination and that is a redesign, not a deletion.

## Blocked on a second, unrelated turmeric bug

`spices/opengl` cannot currently be built at all to validate any of this. Its
transitive `:path` cmake-dep is absolutized by the resolver and then prefixed
again by the emitter:

```
add_subdirectory(".//Users/.../spices/raygui/../cmake-deps/raygui" ...)
```

CMake configure fails, so no dep builds. This reproduces identically on the
pre-fix compiler (`turmeric` `423f6546`), so it is not caused by the link-line
change. Filed upstream as
`transitive-path-cmake-dep-absolutized-then-reprefixed`.

Note it compounds with this repo's own
`spices-ci-fetch-failure-downgraded-to-warning`: the fetch failure becomes a
`::warning::`, the job proceeds, and the real error surfaces later as a
misleading missing-library message.

## Suggested order when unblocked

1. Land the upstream transitive-`:path` fix; confirm `spices/opengl` configures.
2. Drop the `OUTPUT_NAME glfw` rename from the opengl shim; confirm the macOS
   job goes green. This is the smallest independently verifiable step.
3. Move postgres to `:prefer-system` + `:targets`, keeping the keg-only probe.
4. Retry the `raygui` / `opengl` macOS CI jobs that the framework report
   listed as compiler-blocked.
5. Leave the raygui implementation-TU question for a `:c-sources` discussion.
