# Plan: Spices declared inside a script -- a `defscript` header, a dep cache, and an opt-in lock

> **Status:** Deferred. Written 2026-10-21 as the follow-up to
> [installed-spice-libraries-plan](installed-spice-libraries-plan.md), and it
> **assumes that plan has landed** (IL0-IL3 at least). Not scheduled.
> **Last Updated:** 2026-10-21
> **Type:** Tooling / package management (`src/compiler/pkg.c`,
> `src/compiler/global.c`), build driver (`src/main.c`), one new reserved
> top-level form, behind `--enable=script-deps`.
> **See also:** [installed-spice-libraries-plan](installed-spice-libraries-plan.md),
> [global-spice-install-plan](../archive/global-spice-install-plan.md),
> [consuming-spices-guide](../guides/consuming-spices-guide.md) (the `:spices`
> entry grammar this reuses), [experimental-flags-guide](../guides/experimental-flags-guide.md).

## Goal

A single `.tur` file declares the spices it needs, and `tur run` gets them:

```turmeric
#!/usr/bin/env -S tur --enable=script-deps
(defscript
  :spices #map{
    "json" #map{:url    "https://github.com/turmeric-lang/turmeric-spices"
                :ref    "json-v0.3.0"
                :subdir "spices/json"}})

(import json/decode)

(defn main [] : int
  ...)
```

```sh
chmod +x report.tur
./report.tur data.json        # first run fetches json-v0.3.0; later runs are offline
```

The file is now self-describing. Someone else can run it without installing
anything first. It does not depend on what the author has installed, and it
does not add anything to the reader's installed set. An opt-in lock file makes
it reproducible byte for byte.

## What the previous plan leaves, and why this is separate

After [installed-spice-libraries-plan](installed-spice-libraries-plan.md), a
file outside any project can import any **installed** spice: `tur install`
once, then import from anywhere. That covers the person who owns the machine,
but a script built that way is not portable:

- It works only where the same spices are installed, at compatible versions.
- Nothing in the file says which spices those are.
- It silently changes meaning when someone runs `tur upgrade`.

This plan adds the missing piece: a declaration in the file that resolves
**without** the installed set. It was split out because it is a larger design
surface:

- a new reserved form,
- a cache with its own lifecycle,
- a lock format,
- shebang execution.

None of it is needed for the REPL and quick-script use the first plan serves.

## Prior art

| Tool | Where deps are declared | Where they go | Lock |
| --- | --- | --- | --- |
| uv (PEP 723) | A `# /// script` TOML comment block at the top of the `.py` file; `uv add --script` edits it | A cached environment per script, outside any user install | Opt-in sidecar, `uv lock --script` writes `<script>.py.lock` |
| Cargo single-file packages (RFC 3502/3503) | A manifest in frontmatter at the top of the `.rs` file | Cargo's usual registry and target caches | Cargo's lock, kept beside the build |
| Deno | Each import names its source (`jsr:`, `npm:`, a URL) | A global module cache | `deno.lock` |
| Babashka | `(babashka.deps/add-deps '{:deps ...})`, called at run time | The Clojure deps cache | None |

What carries over:

1. **The declaration lives in the file** (all four).
2. **It is resolved before compilation.** Turmeric resolves imports at
   elaboration time, so it is in the uv / Cargo / Deno position, not
   Babashka's run-time call.
3. **Dependencies go in a cache that is not the user's install set**, and
   several versions coexist (uv, Deno).
4. **The lock is opt-in and lives beside the script** (uv). A one-off script
   should not grow a second file unless its author wants reproducibility.

## Design decisions

### SD-D1. A `(defscript ...)` header form, first in the file

The header is an ordinary form: `defscript` followed by keyword options. Its
`:spices` value uses **the same entry grammar as `build.tur`**: `:url`, `:ref`,
`:subdir`, `:path`, `:optional` and `:global`. That grammar is parsed by the
same function, `parse_spices` (`src/compiler/pkg.c:221-318`, static today), so
its validation (`tur_git_ref_ok`, `tur_url_ok`, `tur_rel_path_ok`) and its
`:global` rules come along unchanged.

- **It must be the first form**: after an optional shebang line and an
  optional `#lang` line, with comments allowed in between. The driver reads it
  with the file's own reader before elaboration starts, because the include
  path has to be known before the first import is resolved. Being first keeps
  that read cheap: no macro expansion and no second pass. A `defscript`
  anywhere else, or in a module the script imports, is an error naming the
  rule.
- **Why a form and not a comment block** (uv's choice): Turmeric has a reader,
  `tur fmt` and an elaborator. A real form gets formatted
  (`src/compiler/fmt.c` grows an `SF_DEFSCRIPT` next to `SF_DEFPACKAGE`),
  gets checked, and makes an older `tur` fail loudly ("unknown form") instead
  of silently ignoring a comment and failing later on an import.
- **Why not reuse `defpackage`:** a script has no name, version, `:exports`,
  `:bin` or `:members`. One form meaning two things in two kinds of file is
  worse than one more form.
- **Accepted keys, first version:** `:spices` only. `:cmake-deps`,
  `:c-sources` and `:engine` are plausible later additions. Any other key is
  an error that names the `build.tur` alternative ("this needs a project:
  `tur init`").
- Under sweet-exp, the header is the same form in indentation syntax.
  `#lang r7rs` files are out of scope.

### SD-D2. A script with a header is hermetic

- A file with a `defscript` header sees its declared deps, its own directory,
  the stdlib and `-I`, and nothing else.
- The first plan's installed-spice fallback (its D2) is **off** for it.
- To use an installed spice, the header says so with the existing shape,
  `"notebook" #map{:global true}`, resolved through the registry exactly as in
  `build.tur`.

So adding a header is the moment a script becomes reproducible. Removing it
puts the script back on the installed set.

A header in a file that is inside a project (`spice_find_root` finds a
`build.tur`) is an error: the project's manifest governs that file. This
follows the same reasoning as the `:global` + `:url` rejection: a silent
precedence rule between two manifests is worse than a diagnostic.

### SD-D3. Dependencies live in a content-addressed cache, not the install set

Fetched script deps go under the data root's `cache/` directory. `global.h`
documents that directory as the "shared git fetch cache", and nothing uses it
today (`tur_global_cache_dir` has no caller outside `global.c`):

```
~/.local/share/turmeric/cache/script-spices/
  index.tur                         # (url, ref) -> resolved SHA, fetched-at
  <name>-<sha12>/                   # one immutable tree per (url, subdir, SHA)
    build.tur, src/, spices/, cmake/, tur.lock
```

- Entries are **never** registered in `state.tur` and never on the
  project-less search path. Two scripts can pin two versions of the same spice
  side by side.
- Each entry is set up as a **fetched project**, using the helper the first
  plan's IL0 introduces for installed roots: its own `:spices` deps go in its
  `spices/`, and its `:cmake-deps` are built in its `cmake/`. Every
  project-mode resolver then works on it unchanged, exactly as for an
  installed spice.
- An entry is built in a temporary directory and renamed into place when
  complete. A concurrent `tur run` of another script that wants the same entry
  either finds the finished tree or builds its own and loses the rename race
  harmlessly. An interrupted fetch never leaves a half-built tree under a valid
  name.

### SD-D4. Resolution, pinning and the opt-in lock

- **`:ref` is required** for a `:url` dep in a header. There is no "HEAD": a
  header that floats is not a declaration of anything.
- A tag or branch `:ref` is resolved to a SHA once, recorded in `index.tur`,
  and reused until `tur run --update` (or `tur fetch --script <f> --update`).
  A 40-hex `:ref` is already a pin and skips resolution.
- `:path` deps are relative to the script's directory, are never fetched or
  cached, and are recorded in no lock (as in `build.tur`).
- **The lock is opt-in:** `tur fetch --script <file> --lock` writes
  `<file>.lock` beside the script, in `tur.lock`'s format: a row per dep in
  the closure, with its SHA and tree hash. When the sidecar exists:
  - every run resolves from it,
  - every fetched tree is hash-checked as `tur.lock` is today,
  - a header dep missing from the lock is an error, "lock is stale -- run
    `tur fetch --script <file> --lock`", rather than a silent re-resolve.

### SD-D5. Fetching is visible, and `--offline` is honored

- A run that has to fetch says so on stderr, one line per dep:
  `tur: fetching json (json-v0.3.0) for report.tur`.
- `--offline` runs only from the cache. A missing entry fails and names the
  dep and the command that would fetch it.
- There is no interactive prompt. The trust model is `tur add`'s: the header
  at the top of the file is the list of what will be fetched and built.

### SD-D6. Shebang execution: `tur <script> args...`

The reader already skips a `#!` first line (`src/compiler/reader.c:5803-5812`;
its comment names `#!/usr/bin/env tur`). The command line does not follow
through:

- **`tur report.tur a b` is a usage error.** The external-subcommand
  fallthrough rejects any `argv[1]` containing `.` or `/`
  (`src/main.c:13236-13239`).
- **`tur run report.tur a b` silently drops `a b`.** Only arguments after `--`
  reach the program (`src/main.c:6904-6922`).

So no shebang line can run a script with arguments today. This plan adds one
dispatch rule:

- When no built-in subcommand matches and `argv[1]` names an existing file
  (it contains a `/` or ends in `.tur` / `.tur.sweet`), `tur` runs it as
  `tur run <file> -- <rest of argv>`.
- Global flags before the file (`--enable=...`, `--no-global-spices`) work as
  they do before any subcommand.
- Built-in subcommands always win. A script that happens to be called
  `check.tur` and is run as `tur check.tur` still works, because `check.tur`
  is not `check`.

This rule is independent of the header and could land on its own.

### SD-D7. Behind `--enable=script-deps` until the format settles

`defscript` is a new reserved form, and its key set, the lock format and the
cache layout will all move during development. So it ships under the
experimental-features rule (CLAUDE.md):

- One `EXPERIMENTS[]` row in `src/runtime/experiments.c`, with every
  descriptor field filled:
  - name `script-deps`,
  - `plan_path` pointing here,
  - `introduced` set to the landing version,
  - an advisory `expires_at` a few minors later,
  - `XF_LIFECYCLE_PROTOTYPE`,
  - `opt_global` set to `&g_opt_script_deps`.
- `experiment_warn_if_used("script-deps")` is called where the driver consumes
  the header.
- Without the flag, a `defscript` form is an error that names the flag.
- A shebang script carries the flag on its `#!` line, as in the Goal example.
  It can also come from the user experiments file
  (`~/.config/turmeric/experiments.tur`). There is no project manifest to set
  it in, by definition.
- The new form gets a `;;@form` docstring under `stdlib/forms/`
  (`tests/check-forms-documented.sh`).

## Milestones

### SD0 -- The header: grammar, reading, gating

- [ ] Export `parse_spices` (`src/compiler/pkg.c:221`) so `defscript` can
      call it, with no behavior change to `build.tur`. Its diagnostics say
      "build.tur: spice ..."; they should name the file they came from
      instead.
- [ ] Driver pre-read of the first form (SD-D1):
      - a missing header is fine,
      - a misplaced header, an unknown key, a URL dep without `:ref`, and a
        header inside a project are each an error, in a new code in the
        package range (TUR-E06xx).
- [ ] The elaborator treats the consumed header as a no-op top-level form. It
      errors on a `defscript` anywhere the driver did not consume one (not
      first, or in an imported module).
- [ ] The `EXPERIMENTS[]` row, `g_opt_script_deps`, the lifecycle warning,
      and the "needs `--enable=script-deps`" error (SD-D7).
- [ ] `tur fmt` round-trips the header (`SF_DEFSCRIPT`). Add the
      `stdlib/forms/` docstring.
- [ ] Fixtures:
      - header accepted and ignored by elaboration,
      - each SD0 error,
      - the flag-off error,
      - a sweet-exp header.

### SD1 -- The dep cache

- [ ] `cache/script-spices/` layout and `index.tur` reader/writer (SD-D3,
      SD-D4), next to `state.tur`'s in `src/compiler/global.c`.
- [ ] Fetch a `(url, subdir, SHA)` entry into a temp dir, set it up as a
      fetched project (the IL0 helper), and rename it into place.
- [ ] `--update` re-resolves tag and branch refs. `--offline` never touches
      the network (SD-D5).
- [ ] Tests use local bare repos through `file://` URLs, so they need no
      network:
      - cold fetch,
      - warm offline run,
      - `--update` after the tag moves,
      - `--offline` with a cold cache fails with the right message,
      - two scripts pin two SHAs of one spice.

### SD2 -- Wiring the header into the build

- [ ] For a file with a header, the include path is: its directory, the
      stdlib, `-I`, then each declared dep's root `src/` and transitive closure
      (the walk the first plan moved into tur_core). The installed-spice
      fallback is off (SD-D2). `:global true` entries resolve from the
      registry.
- [ ] Link inputs come from the declared closure, through
      `collect_build_aux`'s collectors, as the first plan's D7 does for
      installed roots.
- [ ] Commands: `tur run`, `tur check`, `tur build <file>`, `tur --interpret`,
      `tur jit`, `tur emit-c`. That is every caller of
      `auto_append_spice_includes`, so the header is honored in one place.
- [ ] Tests:
      - a header dep is importable,
      - an installed spice that the header does not declare is not,
      - `:global true` works,
      - a `:path` dep resolves relative to the script,
      - a dep with `:c-sources` links.

### SD3 -- Shebang execution

- [ ] The `tur <file> args...` dispatch rule (SD-D6), checked after built-ins
      and before `try_external_subcommand`.
- [ ] Tests:
      - an executable script with `#!/usr/bin/env -S tur --enable=script-deps`
        receives its arguments,
      - exit status passes through,
      - a script named like a subcommand,
      - a missing file is still a clean usage error.
- [ ] Windows has no shebang. Document `tur report.tur args` there. A `.tur`
      file association is out of scope.

### SD4 -- Editing and locking

- [ ] `tur add --script <file> <url> [--ref] [--subdir] [--name]` creates or
      updates the header, written back through the formatter. Add
      `tur add --script <file> <path> --path` for local deps.
- [ ] `tur fetch --script <file> [--update] [--lock]` pre-warms the cache
      and, with `--lock`, writes or refreshes `<file>.lock` (SD-D4).
- [ ] With a sidecar present, runs resolve from it, hash-check every tree, and
      reject a stale lock.
- [ ] Tests:
      - the add round trip,
      - lock written, then honored,
      - a tampered cache entry fails the hash check,
      - a dep added after locking reports a stale lock.

### SD5 -- Editor and inspection

- [ ] The LSP reads the header the way the driver does, so an editor resolves
      the same imports `tur run` does (`src/lsp/lsp.c`'s own walk-up, as in
      the first plan's IL2).
- [ ] `tur cache list` shows script-dep entries with their size and when they
      were last used. `tur cache clean [--older-than <days>]` removes them.
      A run stamps an entry's last use, which is what `--older-than` reads.

### SD6 -- Optional: cache the compiled script

Single-file `tur run` builds to a temporary executable and deletes it after
every run (`src/main.c:6953-7001`). A script with a heavy dep pays the whole
compile on every invocation, and shebang use makes that visible.

- [ ] Cache the executable under `cache/script-bins/`, keyed by:
      - the `tur` version, build flags and engine,
      - the content hash of every file the elaborator loaded (it already knows
        the set),
      - every dep's SHA.
- [ ] A hit execs the cached binary directly.
- [ ] Measure before committing to it. With `--engine jit` the compile may
      already be cheap enough.

## Open questions

1. **Share git objects across cache entries?** Every tag of the
   `turmeric-spices` monorepo is a full clone under SD-D3. A bare mirror per
   URL (`cache/git/<urlhash>.git`) with entries checked out from it would cut
   disk use and fetch time. That is an optimization, and it also applies to
   `tur install` and `tur fetch`. Decide after SD1 measures real sizes.
2. **A header inside a project: error or warning?** SD-D2 says error. A
   spice's `examples/*.tur` might reasonably want an extra dep the spice
   itself does not need. If that comes up, the alternative is "the header's
   deps are added on top of the project's", which brings back a two-manifest
   precedence question. Revisit with a real case.
3. **`tur repl` with a script's deps.** `tur repl --deps-from report.tur` (or
   `:spice --from report.tur`) would start a session on a script's declared
   closure, which is useful when developing the script. It is cheap once SD2
   exists, but it is not part of this plan's core.
4. **Branch refs.** SD-D4 accepts a branch `:ref` and pins it in `index.tur`
   until `--update`. Should a branch ref warn, as `tur add` warns when
   `--ref` is omitted? Probably yes, once.

## Out of scope

- Everything in [installed-spice-libraries-plan](installed-spice-libraries-plan.md),
  which this plan assumes.
- Registries and name-only dependencies (`"json" "0.3"` resolved by name).
  Turmeric has no package index; every dep names its source.
- `:cmake-deps` and `:c-sources` declared directly in a header (as opposed to
  arriving through a declared spice). This is a likely follow-up once the
  `:spices`-only header has settled.
