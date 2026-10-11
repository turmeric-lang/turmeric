# Fix: `:cmake-deps` link lines are asked of CMake, not derived from target names

Resolves, in one pass as both asked:

- [cmake-deps-cannot-express-framework](../cmake-deps-cannot-express-framework.md) (high, macOS blocker)
- [cmake-deps-link-name-not-overridable](../cmake-deps-link-name-not-overridable.md) (medium)

Landed 2026-08-28 against `main` at `423f6546`.

## The one sentence

`tur` reconstructed a link line from a CMake target's *name* by string
manipulation; it now asks CMake what the target actually is.

## What changed

**A `link_flags` array in `cmake/spice-deps-manifest.json`** -- verbatim
link-line tokens, no prefix added. This is the representation the framework
report found missing at all three layers (schema, emitter, consumer).

**`emit_link_flags_prelude` (`src/compiler/pkg.c`) sources it from the target
itself**, per `:targets` entry:

- `$<TARGET_FILE:tgt>` -- the exact artifact, replacing the name-derived
  `-L<dir> -l<basename>`. Taken only when the target's `TYPE` is one that has
  an artifact; an `INTERFACE_LIBRARY` or `OBJECT_LIBRARY` has none and
  `$<TARGET_FILE:...>` on it is a hard error.
- the target's `INTERFACE_LINK_LIBRARIES` -- the transitive requirements,
  walked at CMake **configure** time.

> The first cut of this used two generator expressions and classified the
> results in C. That is recorded here because it does not work and the reason
> is not obvious: a genexpr can *read* `INTERFACE_LINK_LIBRARIES` but cannot
> *iterate* it, and its entries cannot be told apart afterwards -- a bare entry
> is either a library name (`m`) or a target name (raylib's property lists
> `glfw`), identical in shape and opposite in handling. Guessing "library"
> re-emits the `-lglfw` this whole pass exists to eliminate. Only
> `if(TARGET ...)` can decide, so the walk has to run where CMake still is.
> See "Follow-on fixes" below.

**`pkg_cmake_manifest_append_cc_flags` classifies each token** instead of
hardcoding `-I`/`-L`/`-l`: a flag (`-framework Cocoa`) or absolute path passes
through verbatim, a bare name becomes `-l`, and two shapes are skipped -- see
"What is still dropped" below.

**Two manifest overrides**, `:link-libs` and `:link-flags`, on a `:cmake-deps`
entry; plus `:build-opts :link-flags` for a project's own link. `:link-libs`
records key presence separately from count, because `:link-libs []` is a value
("link nothing") distinct from an absent key, and is the only way to express a
header-only dep (raygui) or a generator that builds no library (glad).

## Evidence

One fixture demonstrates both reports, because glfw hits both defects. A spice
declaring `:cmake-deps {"glfw" ... :targets ["glfw"]}` with an inline-C call to
`glfwGetVersionString`, built on arm64-apple-darwin27 with Apple clang:

| | manifest | result |
| --- | --- | --- |
| before (`423f6546`) | `"link_libs": ["glfw"]` | `ld: library 'glfw' not found` |
| after | `"link_libs": []`, `link_flags` = `libglfw3.a`, `-framework Cocoa`, `-framework IOKit`, `-framework CoreFoundation` | links, runs, prints `3.4.0 Cocoa NSGL Null EGL OSMesa monotonic` |

Note which defect bites first: the archive is `libglfw3.a`, not `libglfw.a`, so
the `OUTPUT_NAME` half fails at `-lglfw` before the link ever gets far enough to
miss the frameworks. That is why the two reports had to be fixed together --
fixing either alone leaves the fixture red, and fixing the name alone would have
made the framework gap look like a *new* failure rather than the next one.

The frameworks were confirmed load-bearing independently, by linking the same
program against the resolved `libglfw3.a` with and without them: without, the
exact failure the report describes (`Undefined symbols for architecture arm64`,
`_OBJC_CLASS_$_NSApplication` and friends); with, it links.

Override behavior, each verified from a clean project:

- `:link-libs ["glfw3"]` -- `link_libs` becomes `["glfw3"]`, the
  `$<TARGET_FILE:...>` entry is suppressed (the user has taken over artifact
  selection), transitive `link_flags` are still emitted, and it links.
- `:link-libs []` -- no `-l` and no artifact path, only the transitive flags.
  The link then fails on `_glfwGetVersionString`, which is the *correct*
  meaning of "link nothing"; a genuinely header-only dep has nothing to
  resolve. This is the check that `[]` is distinguished from absent.
- `:link-flags ["-framework Cocoa" "-framework IOKit"]` -- appended verbatim
  after the derived tokens.

## What is still dropped

`$<LINK_ONLY:...>` is unwrapped by the configure-time walk and its contents
resolved normally. Any *other* generator expression appearing as an
`INTERFACE_LINK_LIBRARIES` entry is skipped -- it cannot be evaluated at
configure time, and nothing has needed one yet. `:link-flags` is the escape
hatch if something does.

A namespaced entry that names a real target is now resolved to its artifact
like any other; one that names no target (`Threads::Threads`, when the Threads
package was found by a subproject and is not a target in our scope) falls
through as text and is skipped by the consumer's `::` rule. That last case is
a genuine remaining gap, and a narrow one: it costs `-lpthread`, which is in
libc on both platforms turmeric builds on.

## Suites

`run-transitive-cmake-deps` 5/0, `test-emit-cmake` 15/0, `spice-resolver-tests`
80/0, `run-build-project` 38/0, `run-manifest-entry` 11/0, `run-sweet-manifest`
5/0, `repl-spice-linklibs` 1/0, `run-tur-new` green, plus `tests/run.sh`.

## Follow-on

`turmeric-spices` carries three hand-written `cmake-deps/` shims (`opengl`,
`postgres`, `raygui`). **None is wholly obsolete** -- an earlier draft of this
note said they were "now removable", which overstated it. Each shim mixes
workaround with real work:

- `postgres` exists because "tur derives the -l name from the target basename,
  so `PostgreSQL::PostgreSQL` became `-lPostgreSQL` but the file is libpq.so"
  (its own comment). The re-export target that fixes that is now unnecessary.
  The Homebrew keg-only `PostgreSQL_ROOT` probe it also does is still needed.
- `opengl` exists for two reasons; only one is fixed. glfw's `OUTPUT_NAME`/
  directory mismatch is handled now, so its
  `set_target_properties(glfw PROPERTIES OUTPUT_NAME glfw)` -- a target rename
  whose only purpose was to satisfy the old `-l` derivation -- can go. glad is
  a Python *generator* that builds no library, so that half of the shim stays.
- `raygui` is header-only *with an implementation TU* (`raygui_impl.c`); something
  has to compile it. `:link-libs []` expresses the link side, not the compile
  side. Candidate for `:c-sources`, which is a redesign rather than a deletion.

Tracked in [cmake-link-line-shim-followup.md](cmake-link-line-shim-followup.md)
(now done; originally in the spices repo at `docs/cmake-link-line-shim-followup.md`,
branch `claude/cmake-link-line-followup`, turmeric-lang/turmeric-spices#61).

That audit is now partly superseded by the follow-on fixes below: a
raylib-backed spice builds and runs on macOS with **no shim at all**, so the
`raygui` entry's reasoning needs revisiting -- what remains there is the
implementation TU, not anything about frameworks. `spices/opengl` still does
not configure, but for a new and structural reason
([transitive-cmake-deps-collide-on-duplicate-targets](../../reported/transitive-cmake-deps-collide-on-duplicate-targets.md)),
not the `:path` bug that blocked it when the audit was written.

## Follow-on fixes, same day

Testing this against real spices rather than fixtures uncovered four more
defects, each masked by the previous one. All are fixed and archived:

- [transitive-path-cmake-dep-absolutized-then-reprefixed](../transitive-path-cmake-dep-absolutized-then-reprefixed.md)
  and [cmake-dep-path-base-dir-inconsistent](../cmake-dep-path-base-dir-inconsistent.md)
  -- a `:path` cmake-dep was doubled, and then resolved against the wrong base.
- [cmake-deps-fail-on-cmake-4-policy-floor](../cmake-deps-fail-on-cmake-4-policy-floor.md)
  -- one dep with an old `cmake_minimum_required` aborted every configure.
- [cmake-dep-shared-target-links-without-rpath](../cmake-dep-shared-target-links-without-rpath.md)
  -- shared deps linked clean and died at load.

And it corrected two mistakes in the original `link_flags` work, both of which
only a raylib-shaped dependency exposes: a bare `INTERFACE_LINK_LIBRARIES`
entry that is a *target name* was being turned into a `-l` (reintroducing the
exact bug this pass set out to fix), and an Apple framework's absolute
`.framework` path is not a valid link input and must be respelled
`-framework <name>`. The property walk now runs in CMake at configure time,
where `if(TARGET ...)` can make the distinction, and recurses through
non-linkable targets so a vendored `OBJECT_LIBRARY`'s framework requirements
are not lost.

**A raylib spice now builds and runs on macOS with no `cmake-deps/` shim** --
one of the two spices the framework report listed as blocked. The remaining
blocker for `spices/opengl` is
[transitive-cmake-deps-collide-on-duplicate-targets](../../reported/transitive-cmake-deps-collide-on-duplicate-targets.md),
which is structural and still open.
