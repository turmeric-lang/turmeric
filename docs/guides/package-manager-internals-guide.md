---
title: Package Manager Internals
category: Compiler Internals
description: How tur reads build.tur, resolves :spices and workspace siblings, writes and honours tur.lock, generates and consumes the :cmake-deps CMake project, and threads :build-opts into the C compile -- with the traps each stage leaves for spice authors
---

# Package Manager Internals

This guide is for contributors changing how `tur` handles packages ("spices"):
the manifest reader, dependency resolution, `tur.lock`, the generated
`:cmake-deps` project, and the `:build-opts` that reach the C compiler. It
describes the code as it stands.

User-facing material lives elsewhere:

- [package-management-guide.md](package-management-guide.md),
  [consuming-spices-guide.md](consuming-spices-guide.md) and
  [developing-spices-guide.md](developing-spices-guide.md) -- what a spice
  author writes;
- [tur-new-guide.md](tur-new-guide.md) -- the scaffold;
- [using-turmeric-from-cmake.md](using-turmeric-from-cmake.md) --
  `tur emit-cmake`;
- [build-driver-internals-guide.md](build-driver-internals-guide.md) and
  [separate-compilation-internals-guide.md](separate-compilation-internals-guide.md)
  -- what happens after the package manager hands over include dirs and flags;
- [compiler-internals.md](compiler-internals.md) -- the pipeline overall.

> **Trust the code over the comments.** `pkg.h` still describes `:sha256` as
> "SHA-256 of the archive" (it is a `tree1:` content hash for spices and a git
> SHA for cmake deps), and several user guides show lockfile shapes `tur`
> never writes. The sibling repo's `turmeric-spices/CLAUDE.md` says a lock row
> makes `tur fetch` skip a missing dependency and that `:resolved` is never
> checked out; both stopped being true for `:spices` (see
> [What the lock actually does](#what-the-lock-actually-does)). Where this
> guide and the code disagree, fix the guide.

---

## The short version

1. **Manifest.** `build.tur` (or `build.tur.sweet`) holds one
   `(defpackage <name> :key value ...)` form. `pkg_manifest_read` parses it
   with the ordinary Turmeric reader into a `PkgManifest`. Unknown keys are
   ignored; a broken known key fails the whole read.
2. **Resolution.** A `:spices` entry is a workspace sibling, a `:path`, a
   `:global` install, or a git `:url`/`:ref` cloned into
   `<root>/spices/<name>-<ref>`. Four separate walkers turn entries into
   directories; they agree on the priority order but not on everything else.
3. **Lock.** `tur fetch` records each fetched spice's commit (`:resolved`) and
   tree hash (`:sha256`) in `tur.lock`, re-checks out the recorded commit on a
   fresh clone, and fails if the re-fetched tree hashes differently.
4. **CMake deps.** The `:cmake-deps` closure becomes
   `<root>/cmake/CMakeLists.txt`; configuring it writes
   `cmake/spice-deps-manifest.json` (or `-wasm.json`), whose `-I/-L/-l`
   tokens every later C compile reads -- whether or not they are current.
5. **Build options.** `:link-libs`, `:link-flags`, `:c-sources` and
   `:c-includes` reach the cc line; `:c-flags` and `:no-stdlib` are parsed and
   validated but nothing reads them.

---

## Source map

| File | Role |
|---|---|
| `src/compiler/pkg.h` | All public types (`PkgManifest`, `PkgSpice`, `PkgCmakeDep`, `PkgLockFile`, `PkgCmakeManifest`) and entry points |
| `src/compiler/pkg.c` | Manifest read/write, lockfile, tree hash, semver ranges, git fetch, `pkg_fetch_all`, the workspace walks, `:cmake-deps` collection and CMake generation, the JSON manifest reader, the scaffold (`scaffold_project_ext`), and the CLI commands `new`, `init`, `add`, `add-cmake`, `fetch`, `audit`, `emit-cmake` |
| `src/compiler/install.c` | `tur install`/`uninstall`/`list`/`upgrade`: global spices under the XDG layout, `state.tur`, bin symlinks; also `pkg_rm_rf` |
| `src/compiler/spice_search.c` | `spice_find_root` and `spice_search_append`: the include path a single file inside a spice gets (shared with libturi embedders) |
| `src/main.c` | The consumers: `cmd_run` project mode, `cmd_test`, `tur build <dir>`, `collect_build_aux`, `append_manifest_link_flags`, `collect_spice_aux_c`, `resolve_include_dirs_from_manifest`, `discover_manifest_reader_macros`, `apply_manifest_experiments`, `resolve_build_dir`, `resolve_engine` |
| `src/compiler/emit_core.c` | `tur_emit_hoisted_include`: the `__has_include` wrapper around hoisted inline-C includes |
| `src/compiler/elab_module.c` | The one-time "resolved via workspace sibling" advisory, inside module resolution |
| `src/compiler/global.c` | `tur_installed_spice_dir` and the `state.tur` registry behind `:global true` |

Not package code despite the name: `src/compiler/project.c` is session-type
projection (SS6). The scaffold lives in `pkg.c`.

Tests that pin this area: `tests/unit/pkg_hash.c` (tree hash),
`tests/compiler/test_semver_range.c` (`:tur-version`), the manifest fuzzer
`tests/fuzz/fuzz_manifest.c`, and the `security-driver` fixtures.

---

## Commands at a glance

| Command | Entry | Reads lock | Fetches `:spices` | Builds `:cmake-deps` | Writes lock |
|---|---|---|---|---|---|
| `tur fetch` | `pkg.c:cmd_pkg_fetch` | yes | always (`pkg_fetch_all`) | always | yes, unless `--frozen` |
| `tur add <url>` | `pkg.c:cmd_pkg_add` | yes | yes | no | on success |
| `tur add-cmake` | `pkg.c:cmd_pkg_add_cmake` | no | no | no (prints "run `tur fetch`") | no |
| `tur run` (project) | `main.c:cmd_run` | yes, verifies | only if a dep dir is missing | only if `cmake/CMakeLists.txt` is absent | after a fetch or cmake build |
| `tur build <dir>` | `main.c` dir branch | yes, verifies | never | only if this arm's JSON manifest is absent | after a cmake build |
| `tur test` | `main.c:cmd_test` | no | never | never | no |
| `tur install` | `install.c:do_install_spec` | no | direct deps only, unpinned | never | no (writes `state.tur`) |

Everything below explains one column of this table.

---

## 1. Reading the manifest

### Finding it

`pkg_resolve_manifest_path(dir, ...)` probes `<dir>/build.tur`, then
`<dir>/build.tur.sweet`, accepting only regular files; plain wins when both
exist. `pkg_resolve_manifest_cwd` is the cwd-relative twin used by the CLI
commands, and `pkg_is_manifest_name` keeps both names out of source scans.

Walk-ups use it at every step: `spice_find_root` (per-file commands, up to
`TUR_SPICE_WALK_MAX` = 16 levels) and `main.c:find_project_root` (unbounded).

A few readers hard-code `build.tur` and so ignore a `.sweet` manifest:
`install.c:do_install_spec`, and `main.c:collect_web_link_flags` (the `:web`
block of a wasm link). New code should go through the resolver.

### Parsing it

`pkg_manifest_read_status` (and its wrapper `pkg_manifest_read`):

1. Reads the file, then strips a leading `#lang` line with `detect_lang`. A
   trailing token after the dialect is TUR-E0330, exactly as in a `.tur`.
2. Registers a `SourceFile` whose `reader_type` comes from
   `reader_type_from_extension(path)`, so `build.tur.sweet` goes through the
   sweet-exp preprocessor with no caller change.
3. Runs `read_all` and takes the **first** list headed by `defpackage`. Item 1
   is the package name (symbol or string); `:name` later overrides it.
4. Walks the rest as keyword/value pairs. A non-keyword item is skipped by
   itself (`i -= 1`), so a stray symbol does not shift the pairing.
5. Dispatches on the keyword through one `if/else if` chain. **There is no
   final `else`: an unknown key is silently ignored.** That is the documented
   compatibility story (the `:engine` comment says so), and it means a typo in
   a key name (`:cmake-dep`) is invisible.
6. A repeated key replaces the earlier value; each slot frees what it held.
7. Judges the read by `diag_had_error()`. On failure the half-filled
   `PkgManifest` is freed and zeroed, so callers never see a partial manifest.

Map-shaped slots go through `expect_map`, which accepts `#{...}` (`F_MAP`) and
`#map{...}` (`F_MAP_LITERAL`), rejects an effect row (`#fx{...}`, `@{...}`) as
TUR-E0620, and turns a bare `{...}` -- curly-infix, not a map -- into a
"use `#map{...}`" hint (`report_non_map`).

### The keys the code reads

| Key | Field(s) | Validation at read | Read by |
|---|---|---|---|
| `:name` `:version` `:description` `:license` `:repository` `:homepage` `:authors` | scalars, `authors[]` | none | output naming, `tur install`, `tur list` |
| `:tur-version` | `tur_version` | range syntax (TUR-E0622), floor (TUR-E0621 error), ceiling (TUR-W0623 warning) | the read itself |
| `:spices` | `spices[]` | name is a path segment, `:ref` a safe git ref, `:url` shell-safe, `:subdir` contained, `:path` shell-safe, `:global` excludes `:url`/`:path` | all resolvers (section 2) |
| `:cmake-deps` | `cmake_deps[]` | name and `:cmake-name` are CMake identifiers; `:url`, `:ref`, `:path`, `:cmake-version` safe; `:prefer-system` needs `:cmake-name` | section 4 |
| `:build-opts` | `c_flags`, `link_libs`, `link_flags`, `no_stdlib`, `c_sources`, `c_includes` | link tokens grammar-checked (`check_link_vec`); C paths relative, contained, existing, `.c/.cc/.cpp` | section 5 |
| `:exports` | `exports[]` (module names only) | effect row rejected | `tur build <dir>` (export has a file), `tur emit-cmake` |
| `:members` | `members[]` | each a contained relative path | workspace walks |
| `:build-dir` | `build_dir` | contained relative path | `main.c:resolve_build_dir` |
| `:entry` | `entry` | none (existence checked by `cmd_run`) | `cmd_run` |
| `:engine` | `engine` | `"cc"`/`"jit"`/`"interp"` (TUR-E0311) | `main.c:resolve_engine` |
| `:experiments` | `experiments[]`, `has_experiments_key` | must be a vector | `apply_manifest_experiments` (unknown name exits with TUR-E0310); the key's presence suppresses the user file |
| `:reader-macros` | `reader_macros[]` | none | `resolve_manifest_reader_macros` |
| `:bin` | `bin_names[]`, `bin_paths[]` | names start with `tur-` | `tur install` |
| `:web` | `web` | `:gl`, `:heap`, `:main-loop` values | `collect_web_link_flags` (wasm only) |

### Failure: ABSENT, OK, MALFORMED

`PkgManifestStatus` separates "no manifest here" (`ABSENT`, the normal
walk-up result) from "a manifest exists and is broken" (`MALFORMED`). The
distinction matters because every compile entry point calls `diag_reset()`,
which would erase the manifest's error and exit 0. So a malformed verdict is
recorded in sticky process state (`pkg_manifest_mark_malformed_slot`) and
re-emitted as TUR-E0624 by `pkg_manifest_reassert()`, which `main.c` calls
right after each `diag_reset()`, beside `pkg_tur_version_reassert()`.

Two early returns bypass that bookkeeping: a **reader** error (an unbalanced
paren) and a file with **no `defpackage` form** both return `false` with the
status still `ABSENT` and nothing marked. The reader error prints `error:`,
but `tur check` on a file in that spice still exits 0. Any new early return in
this function must mark the manifest malformed.

### Writing it

There are two writers, and they are not equivalent:

- `pkg.c:pkg_build_tur_add_spice` (used by `tur add`) re-reads the file into
  forms, splices the new `:spices` entry in, and re-emits through `fmt_print`
  with the original source for comment preservation. It keeps the map's
  `#map{}`/`#{}` spelling. It does not set a reader type or strip `#lang`,
  so it cannot parse a `build.tur.sweet`.
- `pkg.c:pkg_manifest_write` regenerates the file from the struct. It is used
  by the scaffold and by `tur add-cmake`. It writes only what the struct
  models, and not all of that: comments, `:members`, an empty
  `:experiments []`, a spice's `:subdir` and `:global`, and the symbol vectors
  of a `:exports` map are all lost, and a `:global` spice comes back as
  `#{:url ""}`, which the next read rejects.

---

## 2. Resolving dependencies

### Where a spice lives on disk

| Source | Directory | Fetched | Lock row |
|---|---|---|---|
| workspace sibling (no `:path`) | the sibling member's dir | never | removed by `tur fetch` |
| `:path "<rel>"` | `<declaring manifest dir>/<rel>` | never | removed |
| `:global true` | `tur_installed_spice_dir(name)` (`state.tur`) | never | removed |
| `:url` + `:ref` | `<root>/spices/<name>-<ref>` | `git clone` | yes |
| `:url`, no `:ref` | `<root>/spices/<name>` | `git clone` (default branch) | yes |

`:subdir` is appended after that (not for `:path` in
`pkg_resolve_spice_dep_dir`, which treats `:path` as already pointing at the
package root). The include path then prefers `<dir>/src` and falls back to
`<dir>`.

### Four walkers

They agree on the priority order (workspace sibling, then `:path`, then
fetched) but differ in scope:

| Walker | Used by | Transitive | Workspace siblings |
|---|---|---|---|
| `pkg.c:pkg_fetch_all` | `tur fetch`, `tur add`, `tur run` | yes, BFS through each fetched spice's own `:spices` | skipped only for root entries |
| `main.c:resolve_include_dirs_from_manifest` | `tur run`, `tur test`, `tur build <dir>` | yes, `VisitedRoots` stops cycles | a `:spices` entry naming a sibling resolves to it |
| `spice_search.c:spice_search_append` | `tur check`, `emit-c`, `run <file>`, the LSP, libturi | **no**, direct `:spices` only | every other member's `src/` is added, declared or not |
| `pkg.c:pkg_resolve_spice_dep_dir` | the `:cmake-deps` and `:c-sources` closures | yes (its callers walk) | yes |

`tur install` has a fifth, private loop in `do_install_spec`: direct deps only,
fetched with plain `pkg_git_fetch` into the global spices dir, no lock.

### Git

`pkg_git_fetch(url, ref, dest)` shells out (every argument through
`pkg_cmd_arg`, which quotes for the platform's shell):

- `dest` absent: `git clone --depth 1 [--branch <ref>] -- <url> <dest>`;
- `dest` present: `git -C <dest> fetch --depth 1 origin -- <ref|HEAD>` then
  `checkout FETCH_HEAD`.

It returns `git rev-parse HEAD` (`pkg_git_resolve`). `pkg_git_fetch_pinned`
adds the lock pin: after the clone it checks out the recorded commit -- first
by a depth-1 fetch of the bare SHA, then, for servers that refuse that
(`file://`, plain `git daemon`), by unshallowing and checking out. It never
falls back to the branch tip; an unreachable pin deletes the clone and fails.
A pin that is not 40 or 64 hex digits is refused before it reaches git.

### `pkg_fetch_all`

A BFS over `FetchItem`s seeded from the root's `:spices`:

1. `:global`: a root dep that is not installed is an error; drop any lock row.
2. `:path`: a root dep whose directory lacks a `build.tur` is an error (note:
   that check probes `build.tur` only); drop any lock row. Transitive `:path`
   entries are not checked.
3. A root entry whose name matches a workspace member
   (`collect_workspace_member_names`) is skipped and its lock row dropped.
   Transitive entries are not matched against the workspace and fetch by URL.
4. Name conflict: the first `(url, ref)` seen for a name wins; a later
   different one prints `spice: conflict!` and fails the run (the BFS order
   means the root's declaration wins).
5. The lock decision and fetch (next section).
6. The fetched spice's manifest (under `:subdir` if set) is read; its `:spices`
   are enqueued and recorded in the row's `:transitive` as `name@ref`.

A failed fetch of an `:optional` dep drops its row and sets
`*out_optional_failed`; `cmd_pkg_fetch` turns that into exit 1, versus 2 for
a required failure.

### Workspaces

`:members` on an ancestor manifest makes it a workspace root. The walk
(`collect_workspace_member_names`, `pkg_workspace_member_path`,
`collect_workspace_sibling_dirs`, and the copy in `spice_search_append`)
climbs from the project dir, and the **first readable manifest ends the
walk**: if it has no `:members`, or does not list this project (compared by
`realpath`), there is no workspace. A member's name is its manifest `:name`,
falling back to the member path's basename.

The advisory -- `import 'x' resolved via workspace sibling 'spices/x';
declare it in :spices for release builds` -- comes from
`elab_module.c` when an import is satisfied by an include dir that
`spice_search_append` tagged with a producer. It fires once per include dir
(`warned_per_inc`) and is suppressed when the consumer's `:spices` names the
producer. That comparison uses the member path's **basename**, not the
sibling's `:name`, so a member whose directory and `:name` differ keeps
warning after it is declared. `TUR_DEBUG_RESOLVER=1` traces the additions.

`tur add --workspace <name>` only asserts membership; it never edits the
manifest.

---

## 3. `tur.lock`

### Format

`pkg_lock_write` always writes plain s-expressions (never sweet-exp), to a
temp file renamed into place:

```turmeric
(deflockfile
  :format-version 1
  :spices #{
    "leaf" #{:url "file:///.../remote" :ref "main"
             :resolved "e0cf8ba0...81557"
             :sha256 "tree1:dc4d2c00...e2f5"
             :fetched-at "2026-10-16T15:58:02Z" :transitive ["x@v1"] }
  }
  :cmake-deps #{
    "zlib" #{:url "..." :ref "v1.3.1" :resolved "<git sha>"
             :sha256 "<same git sha>" :fetched-at "..." }
    "libpq" #{:resolved-via "system" :system-version "16.2" }
  })
```

- `:resolved` is the commit `git rev-parse HEAD` reported.
- A spice's `:sha256` is `pkg_hash_dir`'s `tree1:` content hash: every regular
  file's path, size and bytes in sorted path order, skipping `.git`, `build`,
  `.tur-abi-cache` and `.tur-repl-cache`. If hashing fails it falls back to
  the git SHA, which `pkg_hash_comparable` then refuses to compare.
- A cmake row's `:sha256` is just the git SHA again. A system-resolved row
  keeps only `:resolved-via` and `:system-version`.
- Rows for `:path`, workspace and `:global` deps are removed, so the lock
  records only URL deps.

`pkg_lock_read` tolerates a missing file (returns `false`, which every caller
treats as "no lock yet") and a file with no `deflockfile` (an empty lock). It
reads in `"rb"` mode so a CRLF lock on Windows is not rejected.

### What the lock actually does

For each URL spice, `pkg_fetch_all` decides:

| Lock row | `spices/<name>-<ref>` on disk | `--update` | Action |
|---|---|---|---|
| yes | yes | no | "using cached"; nothing fetched, row unchanged |
| yes | no | no | clone, then check out `:resolved` if the row's `:url` and `:ref` still match the manifest; compare the tree hash with the row's `:sha256` |
| no | any | no | fetch the `:ref` tip; record it |
| any | any | yes | fetch the `:ref` tip; re-pin, no comparison |

A tree-hash mismatch prints `integrity check FAILED`, fails the run, and
leaves the row alone so the next run fails too. Both claims in
`turmeric-spices/CLAUDE.md` are therefore stale for `:spices`. Reproduced
with a local `file://` remote: fetch at commit C1, push C2 to the same
branch, delete `spices/`, `tur fetch` again -- the clone lands on C1 and the
lock differs only in `:fetched-at`. A second `tur fetch` with the directory
present leaves the lock byte-identical.

Two things in that note do still hold:

- **`:cmake-deps` are not pinned.** The generated CMake hands FetchContent the
  manifest's `:ref` as `GIT_TAG`; the lock's `:resolved` is only compared
  afterwards by `pkg_cmake_verify_lock` (`tur fetch` without `--update`,
  native `cmake/build` only), which reports a mismatch but cannot prevent one.
- **The lock row is trusted when the directory exists.** A present directory
  is not re-verified by `tur fetch`; it is verified by `tur run` and
  `tur build <dir>` instead.

### Verification and `--frozen`

`pkg_verify_locked_spices(root, manifest, lock, cmd)` re-hashes every present,
lock-pinned direct URL spice and fails on a difference. Its callers are
`tur run` project mode, `tur build <dir>` and `tur audit`. An absent directory
is not a failure there; each caller decides what absence means.

`tur fetch --frozen` builds `lock_pin_signature` (every row's name, url, ref,
resolved, sha256, resolved-via, system-version and transitive list -- not
`:fetched-at`) before and after the fetch, fails if it changed, and never
writes the lock. `--frozen` with `--update` is refused, and so is `--frozen`
with no lock.

### Who writes the lock

`tur fetch` writes it on every run except `--frozen`, even after errors.
`tur add` writes it after a successful fetch. `tur run` writes it after a
fetch triggered by a missing dep or after a cmake build. `tur build <dir>`
writes it after a cmake build. `:fetched-at` changes only for rows that were
actually re-fetched, and for every cmake row each time `pkg_cmake_build`
runs.

The scaffold writes an empty `tur.lock` and does **not** list it in the
generated `.gitignore`; whether to commit it is a project decision.

---

## 4. `:cmake-deps`

### Collecting the closure

`pkg_collect_transitive_cmake_deps(root, manifest, include_workspace_siblings,
...)` unions the root's `:cmake-deps` (first, so its declarations win) with
those of every spice in the `:spices` closure, walked with a worklist and a
`realpath`-keyed visited set. All three callers pass
`pkg_workspace_wide_cmake_deps()`, which is false unless
`TUR_CMAKE_DEPS_WORKSPACE_WIDE=1`: seeding every workspace sibling configured
unrelated members' native deps and collided in CMake's single target
namespace.

`append_cmake_dep_with_conflict_check` dedups identical `(name, url, ref,
path)` entries and fails on a same-name mismatch, printing both origins.
Differences in `:cmake-name`, `:targets` or `:options` are not compared; the
first copy wins silently. It also absolutizes a relative `:path` against
`<origin>/cmake/` -- the generated file lives in `cmake/`, so a spice writes
`:path "../cmake-deps/foo"` to mean `<spice>/cmake-deps/foo`.

### Generating `cmake/CMakeLists.txt`

`pkg_gen_cmake_deps` writes one CMake project, `SpiceDeps`, with
`CMAKE_POSITION_INDEPENDENT_CODE` defaulted ON (static deps also link into
`tur build --shared` images), the `TUR_FETCH_FORCE_FETCH` option, and the
`_tur_walk_iface` helpers. Each dep takes one of three shapes:

| Dep | Emitted | `_<name>_resolved_via` |
|---|---|---|
| `:path` | options, then `add_subdirectory(<abs> _local/<name>-build)` | `path` |
| `:prefer-system` | options, then `find_package(<cmake-name> [ver] QUIET)` (skipped under `TUR_FETCH_FORCE_FETCH` when there is a `:url`); if not found, FetchContent, or a `FATAL_ERROR` naming the package when there is no `:url` | `system` or `fetch` |
| otherwise | `FetchContent_Declare(<name> GIT_REPOSITORY <url> GIT_TAG <ref>)`, options, `FetchContent_MakeAvailable` | `fetch` |

`:options` become `set(K "V" CACHE BOOL|STRING "" FORCE)` -- BOOL only for
boolean spellings -- and `:wasm-options` the same inside `if(EMSCRIPTEN)`.
A pair whose key is not a CMake identifier or whose value contains CMake
syntax is skipped with a `spice:` message (`cmake_opt_ok`).

### The JSON manifest

The same file then builds a JSON string and writes it with
`file(GENERATE)`, so generator expressions such as `$<TARGET_FILE:...>`
expand to real paths. The filename is chosen by CMake, not by `tur`:
`spice-deps-manifest-wasm.json` under Emscripten, otherwise
`spice-deps-manifest.json` (`pkg_cmake_manifest_name`). Each entry carries:

| Field | With `:targets` | Without `:targets` |
|---|---|---|
| `include_dirs` | each target's `INTERFACE_INCLUDE_DIRECTORIES` | `${<name>_SOURCE_DIR}/include` if it exists, else `${<name>_SOURCE_DIR}`; for `:path`, `<abs>/include` else `<abs>` |
| `link_dirs` | `$<TARGET_FILE_DIR:t>` per target | `${<name>_BINARY_DIR}` (`_local/<name>-build` for `:path`) |
| `link_libs` | empty (the artifact is in `link_flags`) | `:cmake-name`, else the dep name |
| `link_flags` | `$<TARGET_FILE:t>` (plus an rpath for a shared lib), then each target's `INTERFACE_LINK_LIBRARIES` resolved by `_tur_walk_iface`, then `:link-flags` | `:link-flags` only |

A `:link-libs` key overrides both columns' library choice: its entries become
`link_libs`, target artifacts are dropped, and `:link-libs []` links nothing
(a header-only dep, or a generator such as glad). For a `:prefer-system` dep
the system branch keys genexes on the namespaced target (`MbedTLS::mbedtls`)
and the fetch branch on its basename (`mbedtls`), so both are emitted under a
CMake `if`.

So the heuristics are: an include dir guessed from the source layout, and a
`-l` name guessed from `:cmake-name` or the key. `:targets` replaces both with
what CMake knows, which covers an `OUTPUT_NAME` that differs from the target
(glfw's `libglfw3.a`), namespace aliases, static-versus-shared choice, and
`-framework` requirements.

### Configuring and building

`pkg_cmake_build(root, manifest, lock, target)` runs `cmake -S cmake -B
cmake/build` (`emcmake cmake ... cmake/build-wasm` for `"wasm"`), adding
`-DCMAKE_POLICY_VERSION_MINIMUM=3.5` on CMake 4 unless
`TUR_CMAKE_NO_POLICY_MIN` is set, and `-DTUR_FETCH_FORCE_FETCH=ON` when
`tur fetch --refetch` set that variable. Then `cmake --build`. Afterwards it
reads the JSON back for each dep's `resolved_via` and updates the lock rows of
`:url` deps (`:path` deps are never locked).

### Consuming the JSON

`pkg_cmake_manifest_read` is a small hand-written JSON reader; a missing file
is success with zero entries. `pkg_cmake_manifest_append_cc_flags` turns each
entry into ` -I`, ` -L`, ` -l` tokens, then the `link_flags`, classified by
`append_link_flag_token`: a leading `-` or `/` passes through, a
`.framework` directory path becomes `-framework <name>`, unevaluated `$<...>`
and `Ns::Target` names are dropped, and a bare word becomes `-l<word>`. Every
token must pass `tur_arg_is_shell_safe` (`cmake_manifest_token_ok`) or it is
dropped with a message.

Two places call it, both reading only the **project root's** JSON:
`main.c:collect_build_aux` (called by `cmd_build`, `cmd_compile` and the
REPL's in-process spice loader, so it covers `tur run`, single-file
`tur build`, and `tur test`, which compiles each test through `cmd_build`)
and the native-only block in the separate-compilation path of
`tur build <dir>`.

### When the CMake project is regenerated

| Command | Regenerates and builds when |
|---|---|
| `tur fetch` | every run |
| `tur run` | `cmake/CMakeLists.txt` does not exist |
| `tur build <dir>` | this arm's JSON manifest does not exist |
| `tur test`, `tur check`, single-file `tur build` | never |

None of the markers looks at the manifest's content, so the **stale-manifest
trap** follows directly: once a JSON file exists, `tur build <dir>` and every
compile reuse its `-I/-L/-l` paths after `:cmake-deps` changed, after the
directories they name were deleted, or when it was committed from another
machine. CMake only rewrites the JSON on a successful configure, so a failed
`tur fetch` leaves the old one in force. Run `tur fetch` after changing
`:cmake-deps`, and never commit `cmake/`.

The two markers also disagree. A failed configure leaves `CMakeLists.txt`
behind and no JSON, so `tur run` fails once and then, on the next run,
skips the dependency build and compiles with no dependency flags at all;
`tur build <dir>` retries. Reproduced with a `:path` cmake-dep whose
`CMakeLists.txt` calls `message(FATAL_ERROR ...)`.

---

## 5. `:build-opts` and the C command line

| Key | Reaches cc through | Scope |
|---|---|---|
| `:link-libs` | `main.c:append_manifest_link_flags`, as `-l<name>` | the project root's manifest only |
| `:link-flags` | the same, verbatim (one entry may be several tokens) | root only |
| `:c-includes` | `main.c:collect_spice_aux_c`, as quoted `-I<spice dir>/<entry>` | root and the whole `:spices` closure |
| `:c-sources` | `collect_spice_aux_c`, as extra TUs | root and the whole closure, each source once by `realpath` |
| `:c-flags` | **nowhere** | -- |
| `:no-stdlib` | **nowhere** | -- |

The link vectors are grammar-checked at read time (`check_link_vec`,
`tur_link_token_ok`), which is what lets `append_manifest_link_flags` splice
them unquoted. `:c-flags` gets the same check and is then dropped: a
`:c-flags ["-DMY_ANSWER=42"]` spice whose inline C returns `MY_ANSWER` fails
with `'MY_ANSWER' undeclared`, and passes with the same flag in
`TUR_CC_FLAGS`. The user guides still document it.

`:c-sources` and `:c-includes` are validated against the manifest directory
when the manifest is read, so a missing file fails every read of that
manifest -- including reads made only to resolve a dependency.

### Hoisted includes and `__has_include`

An inline-C `#include` is hoisted to file scope, and
`emit_core.c:tur_emit_hoisted_include` wraps every **angle** include:

```c
#if __has_include(<yyjson.h>)
#include <yyjson.h>
#else
#pragma message("tur: inline-C requested <yyjson.h>, not found on the ...")
#endif
```

The wrap exists because several stdlib headers are per-platform
alternatives; those carry a `tur:optional` marker and get a silent comment in
the `#else` instead. Quoted includes are emitted bare and fail loudly.

So a wrong include dir -- a missing or stale JSON manifest, a `:targets`
include that does not exist, a wrong `:c-includes` -- does not stop the
compile. It produces one `#pragma message` naming the header, then implicit
declarations and type errors, or a link error, far from the cause. Read the
first `tur: inline-C requested` note before anything after it.

---

## 6. Scaffolding and global installs

`tur new <name>` (`cmd_pkg_new`) and `tur init` (`cmd_pkg_init`) both call
`scaffold_project_ext`. It first runs itself in **probe mode**: every target
goes through `scaffold_write` or `scaffold_probe_note`, which only record
existing files, and any collision refuses the whole scaffold unless `--force`.
Then it writes `build.tur` (via `pkg_manifest_write`) or, with `--sweet`, a
literal sweet-exp `build.tur.sweet`; an empty `tur.lock`; `src/` and `tests/`
templates for the chosen kind and dialect (`--saffron`, `--r7rs`); a
`.gitignore` covering `build/`, `spices/`, `cmake/CMakeLists.txt`,
`cmake/build/` and `cmake/spice-deps-manifest.json` (not the `-wasm`
variants or `cmake/build-wasm/`); `README.md`, an optional `LICENSE`, the
Justfile (`justrun_write_template`), CI, and a git commit. Names must match
`[a-z][a-z0-9-]*`, 2 to 64 characters, and not be `tur`, `build` or `test`.

`tur install` (`install.c:do_install_spec`) is a different system. It clones
into the XDG spices dir as `<name>-<ref>` with plain `pkg_git_fetch`, requires
a non-empty `:bin`, fetches direct `:spices` into the same global dir, builds
each binary by running `tur build <entry> -I ...` (from the turmeric source
root when `inst_find_turmeric_root` finds one), symlinks it into the user
bin dir, and records the install in `state.tur`. It reads no lock and builds
no `:cmake-deps`. A project consumes such an install as a library through
`:spices #{"x" #{:global true}}`, which resolves through
`tur_installed_spice_dir`.

---

## 7. Adding a manifest key

1. **Model it.** Add a field to `PkgManifest` (or `PkgSpice`/`PkgCmakeDep`) in
   `pkg.h`. If "absent" and "empty" mean different things, add a presence flag
   (`has_link_libs`, `has_experiments_key`).
2. **Parse it** in the `pkg_manifest_read_status` chain. Free the old value
   first, since a repeated key replaces it. Use `expect_map` for maps.
3. **Validate at read time** with `diag_emit(DIAG_ERROR, ...)` on the value's
   span, never `fprintf`: the read is judged by `diag_had_error()`. Anything
   that reaches a shell, a CMake file or a path join must pass the matching
   `tur_*_ok` check from `src/tur_argcheck.h`. If the slot parser returns
   `false`, set `bad_slot` so TUR-E0624 names it.
4. **Free it** in `pkg_manifest_free` and in the slot's `*_clear` helper;
   copy it in `deep_copy_cmake_dep` for a cmake-dep field.
5. **Round-trip it** in `pkg_manifest_write`, or `tur add-cmake` and the
   scaffold will drop it.
6. **Consume it** from every path that should honour it. Check both
   `collect_build_aux` and the separate-compilation block of `tur build <dir>`
   (they were once hand-copied twins, which is how `:link-libs` reached one and
   not the other), and decide whether it applies to the root only or to the
   whole `:spices` closure. A key with no consumer is the `:c-flags` bug.
7. **Test it**: a fixture or a `security-driver` case for the validation, and
   a seed in `tests/fuzz/fuzz_manifest.c` if the parser is non-trivial.
8. **Document it** in the user guide that owns the key and in the key table
   above.

---

## Maintenance

Update this guide in the same PR when you:

- add, remove or change the validation of a manifest key;
- change a resolver's priority order or scope, or add a new walker;
- change what `tur.lock` records or when a command reads or writes it;
- change the generated CMake, the JSON manifest fields, or the condition under
  which a command regenerates them;
- change which `:build-opts` reach the C command line.

Cite functions, not line numbers. The tables in sections 2, 3 and 4 were
checked against the code and against scratch projects with a local `file://`
remote; re-check them the same way rather than from the comments.
