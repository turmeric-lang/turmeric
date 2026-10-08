# Plan: Local Cross-Spice Development Workflow

> **Status:** Complete (LS1-LS8 done)
> **Last Updated:** 2026-05-29
> **Type:** Tooling / Compiler UX
> **Related:**
> - `docs/guides/developing-spices-guide.md` (spice authoring conventions)
> - `../turmeric-spices/build.tur` (workspace manifest with `:members`)
> - `docs/tur-watch-spice-plan.md` (the spice whose cutover surfaced this)

---

## Overview

`tur` currently has two ways to declare a spice dependency in `build.tur`:

```turmeric
;; URL-based (works end-to-end, but requires a published ref)
"ansi" #{:url    "https://github.com/turmeric-lang/turmeric-spices"
         :ref    "ansi-v0.1.4"
         :subdir "spices/ansi"}

;; Path-based (accepted by the manifest parser, written by `tur add --path`,
;; but does not actually resolve modules)
"ansi" #{:path "../ansi"}
```

In practice the path form is inert: `tur fetch`, `tur check`, `tur run`,
and `tur build` all ignore it for module resolution. This means **local
cross-spice development in a workspace monorepo does not work out of the
box**, even though the workspace already declares the spices as
`:members` of a parent `build.tur` and the source is sitting right next
to the consumer.

The current workaround -- discovered during the `tur-watch` -> notebook
cutover (see `docs/tur-watch-spice-plan.md` WT7) -- is to:

1. fake the fetched-dep layout by hand: `mkdir spices/notebook/spices && ln -sfn ../../../ spices/notebook/spices/watch-main`,
2. hand-edit `spices/notebook/tur.lock` to add a stub entry with
   all-zero `:resolved` / `:sha256` values,
3. remember to never run `tur fetch` (it clobbers the symlink with
   whatever happens to be on the remote `main`).

This is, in the user's words, "incredibly counter-intuitive behavior."
It is also unnecessary: every piece of information the resolver needs
already exists in the workspace manifest.

This plan makes local cross-spice work behave the way users already
expect.

---

## The problem, concretely

Reproduction (real session):

```sh
cd turmeric-spices/spices/watch
# ... develop watch-v0.1.0 ...

cd ../notebook
# Try to import the new spice the obvious way:
tur add ../watch --path
# build.tur now has :spices { "watch" #{:path "../watch"} }

tur check src/notebook/cli.tur
#   error: module 'watch/watch' not found
#   searched:
#     stdlib/watch/watch.tur
#     spices/notebook/src/watch/watch.tur
#   (note: path-dep src/ is NOT in the search list)

tur fetch
#   spice: lock file written to tur.lock
#   (note: spices/ directory remains empty; :path dep was silently ignored)
```

Several distinct papercuts compound:

1. **`:path` deps are accepted but inert.** The manifest parser reads
   them, `tur add --path` writes them, but the module resolver never
   consults them.

2. **Workspace `:members` are not transitively visible.** The top-level
   `turmeric-spices/build.tur` declares `:members ["spices/watch", ...]`
   so the workspace knows watch exists, yet sibling members cannot import
   it. Being a member is purely a workspace bookkeeping fact.

3. **The auto-spice mechanism only fires for fetched URL deps.** The
   project `CLAUDE.md` describes:

   > "every `:spices` dep declared in the manifest contributes its
   > `src/` too"

   In practice this happens only after `tur fetch` has materialized the
   dep under `spices/<name>-<ref>/<subdir>/`. The :path branch never
   produces such a directory.

4. **`tur fetch` is destructive to local symlinks.** Once you do the
   workaround symlink, the next `tur fetch` re-clones the URL and
   overwrites it. There is no "leave my local override alone" mode.

5. **Partial-fetch failure swallows healthy deps.** When one URL fails
   to fetch (e.g. a stale tag like `test-v0.1.0` that has since moved),
   the resolver does not just drop *that* dep -- it appears to drop
   neighboring deps from the search path as well, breaking `tur check`
   on files that have nothing to do with the failed dep.

6. **Lockfile gates resolution even for local sources.** A dep that is
   present on disk but absent from `tur.lock` is treated as unresolved
   and excluded from the search path. There is no "lockfile-optional"
   mode for local-source deps.

7. **Hand-editing the lockfile is the documented workaround.** The
   stub entry trick used in the watch cutover (`:resolved "00...00"
   :sha256 "00...00"`) bypasses the gate but is obviously not something
   we want to ship as the recommended path.

---

## Goals

After this plan lands:

1. `tur add ../sibling-spice --path` writes a `:path` entry that the
   resolver actually uses. No symlink, no lockfile hand-editing, no
   `tur fetch` round-trip.

2. Inside a workspace (`build.tur` with `:members`), member spices can
   `(import other-member/foo)` from each other with no manifest
   ceremony beyond being members. The workspace IS the dep graph.
   A one-time warning fires the first time an implicit sibling import
   resolves, so drift between actual imports and declared `:spices`
   is visible without requiring a lint pass.

3. `tur fetch` understands local-source deps (`:path` or workspace
   members) and **does not touch them**. It only fetches URL-backed
   deps.

4. The lockfile becomes irrelevant for local-source deps. Lock entries
   are generated only for URL deps where reproducibility actually
   matters.

5. Partial-fetch failure isolates the failure: deps that did fetch (or
   are local) keep working, only the broken one is reported.

6. The documented workflow for "start a new spice in this workspace
   and import it from a sibling" is *one* command and *one* manifest
   edit, with no symlinks or lockfile hand-edits anywhere.

Non-goals:

- Replacing `:url + :ref` deps for published consumption -- those
  remain the canonical reproducible-build mechanism.
- A new package registry. The workspace + URL combination is enough.
- Auto-discovering deps without any manifest entry. Users still
  declare what they depend on; the resolver just stops requiring a
  fetch step when the source is already local.

---

## Design

There are three independent improvements, each useful on its own. They
combine into the "just works" experience described above.

### 1. `:path` deps actually resolve

When a `:spices` entry has a `:path` field, `tur check` / `tur run` /
`tur build` / `tur emit-c` MUST add `<:path>/src` to the module search
path during resolution. No `tur fetch` step required, no lockfile
entry required.

- The path is resolved relative to the directory containing the
  consuming `build.tur`.
- Absolute paths are accepted but warned against (they break sharing).
- Search path priority: `-I` flags > sibling-spice deps > stdlib > the
  importing file's own directory. (This matches the current ordering
  for URL deps after fetch.)

### 2. Workspace members see each other automatically

When `tur` walks up from the current file looking for `build.tur` and
finds one whose parent has its own `build.tur` declaring
`:members [... this-spice-dir ...]`, the resolver:

1. reads the parent's `:members` list,
2. for each sibling member that exports any of the symbols the current
   file imports, adds `<sibling>/src` to the search path,
3. **if the import was satisfied by a sibling member that the current
   spice does not declare in its own `:spices`, emits a one-time
   warning** -- once per (consumer, producer) pair per `tur` invocation
   -- pointing at the import that triggered it. The warning is on by
   default; `TUR_DEBUG_RESOLVER=1` upgrades it to a full breadcrumb
   showing every search-path addition.

No `:spices` entry is required for sibling-member access -- workspace
membership is the consent boundary, matching npm / Cargo / Bazel
workspaces. The warning exists so that drift between "what I import"
and "what I declare" surfaces early, before a `:spices` entry is
needed for external publication.

The current `:spices` declaration in a member spice is then strictly
for *external* publication: it declares the URL + ref future consumers
outside the workspace will use. The workspace itself ignores it for
local resolution.

Implicit access is workspace-scoped only. Two sibling directories
that share a parent dir but have **no** workspace `build.tur` (or one
with no `:members`) do not auto-resolve each other -- the workspace
manifest is the consent boundary, full stop. A two-spice prototype
that wants this needs a five-line parent `build.tur` to opt in.

Implicit-import warnings apply only to direct sibling resolution
(question 1 of the open-questions resolution). Transitive deps inherited
through a sibling (§3 below) stay silent, on the theory that the
consumer is paying attention to their direct deps and inheritance is
already covered by the producer's own manifest.

### 3. `tur fetch` skips local-source deps

For each `:spices` entry, `tur fetch`:

- if `:path` is set, **skip the fetch**, and do not write a lock entry,
- if the dep name matches a workspace member, **skip**,
- otherwise resolve `:url + :ref` as today.

`tur fetch` never overwrites or modifies a local-source dep's
directory, so symlinks (if anyone still wants them) survive.

**Missing `:path` is a hard error.** If a `:path` is declared but does
not exist on disk (or does not contain a `build.tur`), `tur fetch`
fails with a clear diagnostic naming the manifest line that declared
it. The same applies at resolution time for `tur check` / `tur run`.
This matches how missing URL refs behave today and prevents the
silent-skip class of "why isn't my dep working" bugs.

### 3a. Transitive deps are inherited automatically

If sibling A imports symbols from sibling B, and B itself declares a
URL dep on `foo`, then `foo`'s `src/` is added to A's search path as
well. The resolver walks the dep graph transitively, matching how
Cargo and npm already work. A does not need to redeclare `foo` to
import a B symbol that internally references `foo` types.

The trade-off is honest: A's release package may now silently depend
on `foo` even though A's own manifest never names it. The
implicit-import warning from §2 deliberately does **not** fire for
transitive deps (only for direct sibling imports), so the warning
stays a useful drift signal rather than noise. A future
`tur lint --imports` will diff the full transitive set against direct
declarations for release-readiness checks.

### 4. Lockfile only tracks URL deps

`tur.lock` becomes a record of "what remote SHAs we resolved" only.
Local-source deps do not appear. The resolver does not consult the
lockfile when a dep is local-source; reproducibility for local deps is
the user's git history, not the lockfile.

This eliminates the stub-entry workaround entirely.

### 5. Isolated partial-fetch failure

When `tur fetch` cannot resolve one URL dep:

- the resolver still adds search paths for every other dep (local or
  successfully fetched),
- `tur check` / `tur run` proceeds and only fails if it actually needs
  a symbol from the missing dep,
- the missing dep is reported once, clearly, with the URL and the git
  error that caused it.

Today, a failed fetch on `test@test-v0.1.0` causes the resolver to
silently drop `ansi` and `png` search paths from neighboring files.
That cascade has to stop.

### 6. CLI ergonomics (ships with the plan)

- `tur add <relative-path> --path` writes `:path` (already does today;
  now actually works).
- `tur add --workspace <name>` is a new shortcut that asserts `<name>`
  is a workspace member and adds no manifest entry, since none is
  needed for sibling access. Errors if `<name>` is not a member of
  the enclosing workspace.
- `tur fetch --dry-run` lists what would be fetched + what would be
  skipped (local) -- useful for sanity-checking that the workspace is
  configured correctly, and the recommended one-shot for "did I wire
  this up right?"

Both ship in LS6 of this plan; neither is deferred.

---

## Implementation phases

- [x] **LS1** -- Make `:path` deps populate the module search path.
  Smallest possible change: in the spice-resolution code, when
  iterating `:spices` entries, branch on `:path` vs `:url` and add
  `<:path>/src` directly. No lockfile interaction. Add a single test
  fixture that imports a sibling spice purely via `:path`.

- [x] **LS2** -- Teach the resolver to read the parent `build.tur`'s
  `:members` and auto-add sibling-member `src/` directories. Ship
  on by default (no gating flag) -- the behavior is additive and
  there is no plausible workflow that *wants* a sibling to be
  invisible. Wire in the one-time implicit-import warning at the
  same time.

- [x] **LS3** -- Make `tur fetch` skip local-source deps: any entry
  with `:path`, plus any name matching a workspace member. Update lock
  emission so they are not recorded.

- [x] **LS4** -- Decouple resolution from the lockfile for local
  deps: when a `:spices` entry is local-source, do not require a
  matching lockfile row. (URL deps still consult the lockfile for
  reproducibility.)  In practice this also closes the
  workspace-member-declared-as-URL hole in `cmd_run`'s project mode:
  a `:spices "alpha" #{:url ...}` entry whose name is also a
  workspace sibling now resolves to the sibling's directory instead
  of triggering a spurious `need_fetch=true` and inserting a phantom
  `spices/alpha-<ref>/` path into the search list.  `pkg_is_workspace_member`
  / `pkg_workspace_member_path` are the public helpers behind both
  decisions.

- [x] **LS5** -- Isolate partial fetch failures so one broken URL
  cannot remove unrelated deps from the search path.  `pkg_fetch_all`
  already continued past per-dep failures, but `cmd_run`'s project-mode
  path aborted the whole build the moment any URL fetch returned
  non-zero, which cascaded to dropping every healthy dep from the
  search list.  The fix in `cmd_run` warns ("one or more dependencies
  failed to fetch; continuing with healthy deps on disk") and writes
  whatever lock entries `pkg_fetch_all` managed to populate, then falls
  through to `resolve_include_dirs_from_manifest` -- whose existing
  per-dep stat-and-skip already omits the missing dir, so only files
  that actually `(import broken-dep/...)` see a `module not found`.
  Regression coverage in `tests/spice-resolver-tests.sh`:
  - LS5-A: `tur check` on a healthy-only entry succeeds with a
    `:url "file:///nonexistent..."` sibling dep declared.
  - LS5-B: `tur run --release` in the same project produces a binary
    whose exit code is the healthy dep's return value (42).
  - LS5-C: a file that *does* import the broken dep still fails
    (isolation, not silence).

- [x] **LS6** -- Add `tur add --workspace <name>` shortcut and
  `tur fetch --dry-run` reporting. Update `tur-add`'s help text.
  `cmd_pkg_add` now accepts `--workspace <name>`, validates membership
  via `pkg_is_workspace_member(".", name)`, prints a "no manifest
  entry needed" hint, and exits without touching `build.tur`. Mixing
  `--workspace` with `--path` / `--ref` / `--subdir` / `--optional` /
  `--name`, or passing a URL/path-shaped name, is rejected up front.
  `cmd_pkg_fetch` gained `--dry-run`, which iterates direct
  `:spices` / `:cmake-deps` entries and reports each one as `fetch
  URL`, `fetch cmake`, `skip :path`, or `skip workspace member`
  (using `pkg_is_workspace_member` / `pkg_workspace_member_path`),
  then prints a one-line summary. No network calls; `tur.lock` is
  never opened. Top-level usage now lists `tur add --workspace
  <name>` and `tur fetch [--update|--dry-run]`. Regression coverage
  in `tests/spice-resolver-tests.sh`:
  - LS6-A: `tur add --workspace alpha` from a sibling member exits 0,
    prints the "workspace sibling" hint, and leaves `build.tur`
    byte-identical.
  - LS6-B: `tur add --workspace nope` fails with a diagnostic naming
    the unknown member.
  - LS6-C: `tur fetch --dry-run` classifies a mix of URL / `:path` /
    workspace-member deps, never writes `tur.lock`, and never emits
    the `spice: fetching '...'` line that would indicate a real
    fetch attempt (proved by including a bogus `:url` that would
    otherwise hard-fail).

- [x] **LS7** -- Documentation refresh:
  - `docs/guides/developing-spices-guide.md` -- new "Cross-spice
    development in a workspace" section showing the one-edit workflow.
  - Update `CLAUDE.md` (this repo) and
    `../turmeric-spices/CLAUDE.md` to drop the "fetch first" caveat.
  - Remove the stub-lockfile + symlink workaround previously used in
    `spices/notebook/spices/watch-main`:
    - Deleted the `watch-main` symlink.
    - Removed the all-zero stub row from `spices/notebook/tur.lock`.
    - Switched `spices/notebook/build.tur` watch dep from `:url` to
      `:path "../watch"` (bypasses the `:name "tur-watch"` vs `"watch"`
      name mismatch that blocked workspace-resolution).

- [x] **LS8** -- Migration sweep: any spice that currently leans on a
  URL dep purely for in-workspace access switches to `:path` or
  workspace-member resolution.
  - Audited all 20 workspace members for URL deps pointing to workspace
    siblings.
  - **`notebook/watch`** -- the one "purely in-workspace" dep; converted
    to `:path "../watch"` in LS7.
  - **Pinned-release deps** (`test-v0.1.0`, `math-v0.1.0`,
    `plutovg-v0.1.0`, `scscm-v0.1.0`): serve external consumers; remain
    as URL. (`tur-scscm` dep in tidal already resolves via workspace
    because its dep name matches scscm's manifest `:name`.)
  - **`notebook/png`** (`:ref "main"`): real SHA in lock, needed by
    external notebook consumers for image functionality; remains as URL.
  - Result: no further conversions beyond LS7.

---

## Acceptance criteria

1. Starting from a clean checkout of `turmeric-spices` with no
   `tur.lock` and no fetched `spices/<dep>/` dirs, this command
   sequence succeeds:

   ```sh
   cd turmeric-spices/spices/notebook
   tur check src/notebook/cli.tur          # resolves watch/watch via workspace
   tur run tests/exec_test.tur             # all tests pass
   ```

   No symlink. No `tur fetch`. No lockfile edit. The first invocation
   prints a single warning of the form:

   ```
   warning: import 'watch/watch' resolved via workspace sibling
            'spices/watch'; declare it in :spices for release builds.
            (set TUR_DEBUG_RESOLVER=1 for full resolver tracing)
   ```

2. `tur add ../watch --path` from a sibling spice produces a working
   import on the next `tur check` -- no intermediate `tur fetch`,
   no symlink.

3. `tur fetch` in a workspace with one broken URL dep:
   - prints the failure for that one dep,
   - returns non-zero **only** if a still-pending import depends on
     the failed dep,
   - does not regress search paths for healthy deps.

4. `tur fetch` is a no-op for local-source deps -- the symlink that
   currently has to be re-created after every fetch is no longer
   touched.

5. `tur.lock` after the refresh contains entries only for URL deps.

6. The notebook cutover workaround
   (`spices/notebook/spices/watch-main` symlink + stub lockfile row)
   is deleted, replaced by the workspace-resolution path, and the
   commit removing it changes nothing else.

---

## Resolved decisions

These were open in an earlier draft of this plan and have been settled
interactively. Documented here so future readers can see the rationale.

| # | Question | Decision |
|---|---|---|
| 1 | Must a workspace consumer declare its sibling producer in `:spices`? | **No** -- implicit, with a one-time warning when an undeclared sibling is resolved. Workspace is the consent boundary. |
| 2 | Transitive: does A inherit B's URL deps when A imports from B? | **Yes** -- walk the dep graph transitively (Cargo/npm-style). Future `tur lint --imports` audits direct vs transitive at release time. |
| 3 | `tur fetch` when `:path` is missing on disk? | **Hard error.** Symmetric with missing URL refs. |
| 4 | Does `tur.lock` record anything for `:path` deps? | **No entry at all.** Lockfile tracks remote SHAs only. |
| 5 | Two siblings with no workspace `build.tur` -- auto-resolve? | **No.** Workspace manifest is required. Two-spice prototypes need a five-line parent manifest to opt in. |
| 6 | Ship LS2 (workspace resolution) behind a feature flag? | **No flag.** Ship on. Behavior is additive; no plausible workflow wants siblings invisible. |
| 7 | Scope of the implicit-import warning? | **Workspace siblings only**, not transitive. Keeps the signal high. |
| 8 | Do `tur fetch --dry-run` + `tur add --workspace` ship with the plan? | **Yes**, both land in LS6. |

## Risks and remaining open questions

1. **`:path` deps and `:url` deps must agree on `:exports`.** If the
   manifest of a `:path` dep changes, the consumer sees it on the
   next `tur check`. That is correct, but it also means a sloppy
   local edit can break a downstream consumer with no fetch step in
   between. Acceptable trade-off -- it is the same as editing a file
   inside your own project.

2. **Transitive failure attribution.** When a transitive URL dep
   fails to resolve, the user sees an error pointing at a `:spices`
   entry they did not write. The error message must name both the
   direct dep that pulled it in *and* the missing transitive dep, or
   debugging gets painful fast.

3. **CMake-deps interaction.** `tur.lock` also tracks `:cmake-deps`.
   That side stays as-is; this plan only touches the `:spices`
   resolution path.

4. **IDE / LSP coupling.** The Turmeric LSP runs the same resolver.
   Workspace resolution + path deps should "just work" there too,
   but worth testing explicitly in LS7. The one-time warning should
   surface in the LSP as a diagnostic on the import line, not as a
   stderr message that disappears.

5. **Cycle detection in transitive walking.** Two workspace members
   that import from each other already work (the resolver handles
   the cycle at import time). Once we also walk transitive URL deps,
   we need to make sure a published dep graph cycle does not loop
   the fetcher. Standard fix: mark-visited during the walk.

6. **Warning rate-limiting under repeated invocations.** "Once per
   `tur` invocation" is the right scope for a single command, but
   editor / LSP integrations re-invoke the resolver constantly. The
   LSP path should suppress the warning after first-shown-this-session
   rather than first-shown-this-invocation, to avoid drowning the
   diagnostics panel.

---

## Out of scope (future work)

- A real package registry / index.
- Versioned local-source deps (`:path` with a `:version-constraint`).
- Auto-publishing workspace members on tag push.
- A `tur workspace status` command summarizing what is local vs
  fetched vs broken (would be nice; not blocking).
