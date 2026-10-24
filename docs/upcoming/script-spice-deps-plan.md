# Plan: Spices declared inside a script -- a `defscript` header, a dep cache, and an opt-in lock

> **Status:** SD0-SD5 landed 2026-10-22 behind `--enable=script-deps`
> (experiment row `script-deps`, introduced 0.70.0). SD6 (cache the compiled
> script) is open: researched and measured 2026-10-24, found worth doing, and
> rewritten from those measurements -- see its section. The plan stays here
> until the experiment graduates.
> Follow-up to [installed-spice-libraries-plan](../archive/installed-spice-libraries-plan.md),
> which it builds on.
> **Last Updated:** 2026-10-22
> **Type:** Tooling / package management (`src/compiler/pkg.c`,
> `src/compiler/global.c`), build driver (`src/driver/`), one new reserved
> top-level form, behind `--enable=script-deps`.
> **See also:** [installed-spice-libraries-plan](../archive/installed-spice-libraries-plan.md),
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

(defmodule report
  (import json/parse :refer [json-parse])
  (defn main [] : int
    ...))
```

(`import` is only allowed inside a `defmodule`, so a script that imports
wraps its body in one; a `defscript` header may precede the `defmodule`.)

```sh
chmod +x report.tur
./report.tur data.json        # first run fetches json-v0.3.0; later runs are offline
```

The file is now self-describing. Someone else can run it without installing
anything first. It does not depend on what the author has installed, and it
does not add anything to the reader's installed set. An opt-in lock file makes
it reproducible byte for byte.

## What the previous plan leaves, and why this is separate

After [installed-spice-libraries-plan](../archive/installed-spice-libraries-plan.md), a
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

- [x] Export `parse_spices` (`src/compiler/pkg.c:221`) so `defscript` can
      call it, with no behavior change to `build.tur`. Its diagnostics say
      "build.tur: spice ..."; they should name the file they came from
      instead.
- [x] Driver pre-read of the first form (SD-D1):
      - a missing header is fine,
      - a misplaced header, an unknown key, a URL dep without `:ref`, and a
        header inside a project are each an error, in a new code in the
        package range (TUR-E06xx).
- [x] The elaborator treats the consumed header as a no-op top-level form. It
      errors on a `defscript` anywhere the driver did not consume one (not
      first, or in an imported module).
- [x] The `EXPERIMENTS[]` row, `g_opt_script_deps`, the lifecycle warning,
      and the "needs `--enable=script-deps`" error (SD-D7).
- [x] `tur fmt` round-trips the header (`SF_DEFSCRIPT`). Add the
      `stdlib/forms/` docstring.
- [x] Fixtures:
      - header accepted and ignored by elaboration,
      - each SD0 error,
      - the flag-off error,
      - a sweet-exp header.

### SD1 -- The dep cache

- [x] `cache/script-spices/` layout and `index.tur` reader/writer (SD-D3,
      SD-D4), next to `state.tur`'s in `src/compiler/global.c`.
- [x] Fetch a `(url, subdir, SHA)` entry into a temp dir, set it up as a
      fetched project (the IL0 helper), and rename it into place.
- [x] `--update` re-resolves tag and branch refs. `--offline` never touches
      the network (SD-D5).
- [x] Tests use local bare repos through `file://` URLs, so they need no
      network:
      - cold fetch,
      - warm offline run,
      - `--update` after the tag moves,
      - `--offline` with a cold cache fails with the right message,
      - two scripts pin two SHAs of one spice.

### SD2 -- Wiring the header into the build

- [x] For a file with a header, the include path is: its directory, the
      stdlib, `-I`, then each declared dep's root `src/` and transitive closure
      (the walk the first plan moved into tur_core). The installed-spice
      fallback is off (SD-D2). `:global true` entries resolve from the
      registry.
- [x] Link inputs come from the declared closure, through
      `collect_build_aux`'s collectors, as the first plan's D7 does for
      installed roots.
- [x] Commands: `tur run`, `tur check`, `tur build <file>`, `tur --interpret`,
      `tur jit`, `tur emit-c`. That is every caller of
      `auto_append_spice_includes`, so the header is honored in one place.
- [x] Tests:
      - a header dep is importable,
      - an installed spice that the header does not declare is not,
      - `:global true` works,
      - a `:path` dep resolves relative to the script,
      - a dep with `:c-sources` links.

### SD3 -- Shebang execution

- [x] The `tur <file> args...` dispatch rule (SD-D6), checked after built-ins
      and before `try_external_subcommand`.
- [x] Tests:
      - an executable script with `#!/usr/bin/env -S tur --enable=script-deps`
        receives its arguments,
      - exit status passes through,
      - a script named like a subcommand,
      - a missing file is still a clean usage error.
- [x] Windows has no shebang. Document `tur report.tur args` there. A `.tur`
      file association is out of scope.

### SD4 -- Editing and locking

- [x] `tur add --script <file> <url> [--ref] [--subdir] [--name]` creates or
      updates the header, written back through the formatter. Add
      `tur add --script <file> <path> --path` for local deps.
- [x] `tur fetch --script <file> [--update] [--lock]` pre-warms the cache
      and, with `--lock`, writes or refreshes `<file>.lock` (SD-D4).
- [x] With a sidecar present, runs resolve from it, hash-check every tree, and
      reject a stale lock.
- [x] Tests:
      - the add round trip,
      - lock written, then honored,
      - a tampered cache entry fails the hash check,
      - a dep added after locking reports a stale lock.

### SD5 -- Editor and inspection

- [x] The LSP reads the header the way the driver does, so an editor resolves
      the same imports `tur run` does (`src/lsp/lsp.c`'s own walk-up, as in
      the first plan's IL2).
- [x] `tur cache list` shows script-dep entries with their size and when they
      were last used. `tur cache clean [--older-than <days>]` removes them.
      A run stamps an entry's last use, which is what `--older-than` reads.

### SD6 -- Cache the compiled script

Rewritten 2026-10-24 from measurements. The first version (kept in git
history) asked to measure before committing and guessed that `--engine jit`
might make a cache unnecessary. The measurements say otherwise.

#### Findings (2026-10-24)

A single-file `tur run` resolves to the **cc** engine unless `--engine`,
`TUR_ENGINE` or a manifest says otherwise (`drv_resolve_engine`,
`src/driver/spice.c:699`). It writes a temporary executable, runs it, and
deletes it on every invocation (`src/driver/cmd_run.c:395-404`). Measured on a
release build of `tur` 0.70.5, Linux x86-64, 4 cores, gcc, warm caches:

| Program | `tur run`, cc (default) | `tur run --engine jit` | the built binary run directly |
| --- | --- | --- | --- |
| a script with a `defscript` header declaring the json spice (`:path` dep; yyjson linked) | 150-200 ms; `--timings`: cc 138 ms of 171 ms (81%), front end and emit ~30 ms, the program 1.6 ms | 360-400 ms | 2-3 ms |
| `stdlib/tools/serve.tur` (loads all 6,754 lines of `httpd.tur`) | 1.1-1.2 s (gcc `-O2`); `tur build` 2.0 s, cc 96% | 260 ms | 2 ms |

- **The C compiler is the cost.** Turmeric's own phases are 30-60 ms; gcc is
  the rest. A cache hit removes nearly all of it: 60x to 500x faster.
- **The JIT is not the answer.** It is twice as slow as cc on the small
  script, and on the large one still about 100x slower than a cached binary.
  It cannot be cached the same way either: it compiles in process and leaves
  no executable.
- **The same compile-run-discard path serves more than scripts.**
  `tur serve` compiles `serve.tur` on every launch, on cc always on Windows
  (`cmd_serve`), so 1-2 s each start there; `tur x` (project-less-tools PT3)
  runs a spice's tool the same way.
- **There is precedent for a compile cache, and for its security.**
  `prelude_split_object` (`src/driver/preamble_split.c:291`) caches a compiled
  object keyed by a hash of its text, the compiler and every flag, and
  publishes it by rename. `cc.c:241` (WP2 D-4) records why a shared cache
  directory must be checked, not assumed: a planted file there was linked
  into users' binaries. SD6 caches files that are *executed*, so the same rule
  applies with more force.

#### Design

**Scope.** The cc path of a single-file run: `tur run <file>`, `tur <file>`,
and later `tur serve` and `tur x`. First only for files with a `defscript`
header, under the existing `script-deps` gate; widened once it has run
without a stale hit.

**Where.** `<global cache>/script-bins/<key>` -- beside the dep cache
(`tur_script_cache_dir`, `src/compiler/global.c:663`), so `tur cache list` and
`tur cache clean --older-than` cover it. The directory is checked as WP2 D-4
checks `tur-build`: a real directory (not a symlink), owned by the user, not
writable by group or other. A directory that fails the check disables the
cache for the run (compile as today) and says so once; it is never repaired.

**The key.** A hash over everything that can change the binary:

- `tur`'s version and build id, the engine, and every flag that reaches the
  compile (`--enable` set, `-I`, reader macros, `--debug`, sanitizers);
- the C toolchain: the resolved `CC`, its version string, and the C and link
  flags, including `TUR_*` variables that change code generation;
- the content of every file the front end loaded: the script, stdlib modules,
  each dep's modules;
- every declared dep's commit, as SD2 resolves it;
- **native link inputs**: each static library a dep's `:cmake-deps` built
  (e.g. `libyyjson.a`) and each `:c-sources` file. The first version of SD6
  omitted these; without them, rebuilding a dep's C would leave the cached
  binary in use.

**Checking a hit without compiling.** The set of loaded files is only known
after a front-end run, so the entry records it. Each entry stores, beside the
binary, a manifest: the key's fixed inputs, and for every loaded file and link
input its path, size, modification time and content hash, plus the
modification time of every directory searched on the module path (a new file
that would now shadow a loaded one changes its directory). A run:

1. computes the fixed part of the key (version, flags, toolchain, script
   path and content) and looks up candidate entries;
2. for a candidate, `stat`s each recorded file and directory; if size and
   modification time match, it trusts the recorded hash, otherwise it rehashes
   and rejects on a mismatch;
3. on a hit, execs the cached binary with the script's arguments; on a miss,
   compiles as today, then publishes.

The common hit is one hash of the script plus a few dozen `stat` calls.

**Publishing.** Compile to a private name in the cache directory and rename
it into place (as `prelude_split_object` does), then write the manifest last,
so its presence means complete. Two runs racing on one key both compile; one
rename wins, and the loser's binary is discarded.

**Escape hatches.** `tur run --no-cache` (also `TUR_NO_SCRIPT_CACHE=1`)
compiles as today. `tur cache clean` removes binaries with the deps. `--timings`
reports a hit as its own phase, so a slow run is easy to diagnose.

**Not cached.** `--engine jit` and `--interpret` (no executable), a file
inside a project (a project has `tur build`), and any run with a sanitizer
the cached binary was not built with -- the key covers it, but the point is
stated.

#### Checklist

- [ ] Key computation and manifest format; the fast `stat` check.
- [ ] Cache lookup, exec on hit, publish on miss, in the cc arm of
      single-file `tur run` (`cmd_run.c`).
- [ ] Directory checks (WP2 D-4 rules), `--no-cache` / `TUR_NO_SCRIPT_CACHE`.
- [ ] `tur cache list` shows binaries (size, last used); `tur cache clean
      [--older-than]` removes them. A hit stamps last use.
- [ ] `--timings` names the hit.
- [ ] Tests (`tests/run-script-deps.sh`): a second run is a hit and produces
      the same output; **each key input, changed one at a time, forces a
      rebuild** -- the script, an imported stdlib module (via `-I` shadowing),
      a dep's module, a dep's commit, a dep's static library, a `:c-sources`
      file, an `--enable` flag, `CC`; a new file shadowing a loaded module
      forces a rebuild; a group-writable cache directory disables the cache;
      `--no-cache` compiles; two concurrent first runs both succeed.
- [ ] Measure again: hit time for the json script and for `serve.tur`.
- [ ] Then widen: every single-file `tur run` on cc, `tur serve` (Windows
      first), `tur x`.

**Risk.** A stale hit runs old code silently, which is worse than a slow run.
That is why the key is conservative, why every input has a test that changes
it, and why the first scope is the gated `defscript` path.

#### Found while researching SD6

- **The Goal example did not compile** (a bare `import` outside a
  `defmodule`); fixed above.
- **`:path` deps and native code.** A `:path` dep's own `:cmake-deps` are not
  fetched by the script run: the json checkout needed one `tur fetch` before
  `yyjson.h` was found. Arguably right for a local checkout, but the
  consuming-spices guide should say so in one sentence. (`:url` deps go
  through the script-dep cache, which sets them up.)
- **`tur run --engine jit` prints three `unknown pragma` warnings** for every
  program, even `(println 7.1)`. `tur serve` silences them
  (`tur_jit_set_quiet_warnings`, `cmd_run.c:1130`); a plain JIT run does not.
  A small separate fix.

## As built (2026-10-22)

Where the implementation settled a detail the design left open, or departed
from it:

- **Where it lives.** `pkg_script_header_read` / `pkg_script_header_locate`
  (`src/compiler/pkg.c`) read the header; `src/compiler/script_deps.c` turns it
  into spice roots (the cache, the lock, `tur fetch/add --script`,
  `tur cache`); `spice_search_append` (`src/compiler/spice_search.c`) calls it,
  so every caller of `auto_append_spice_includes` -- and libturi's
  `turi_env_set_search_path_for` -- honors the header in one place. A header
  that cannot be honored stops the driver (`exit(1)` after the diagnostic);
  the editor carries on.
- **Reading only the header.** The driver locates the header with a text scan
  (no read at all for a file without one) and hands the reader just the
  header's bytes, so a reader error later in the file is reported once, by
  the compile.
- **The consumed header in the elaborator.** `elaborate_program_session` drops
  the program's first form when it is a `defscript` with only a preamble
  (shebang, `#lang`, comments) before it in its file. Every other `defscript`
  reaches `elab_defscript`: TUR-E0633 with the flag off, else TUR-E0630. The
  "defmodule must be the first form" check lets a `defscript` precede the
  `defmodule`, so a script can still be a module.
- **Codes.** TUR-E0630 misplaced, E0631 malformed (unknown key, `:url` without
  `:ref`), E0632 in a project, E0633 flag off, E0634 a dep that cannot be had
  (fetch failure, `--offline` miss, stale lock, hash mismatch).
- **Cache entry names** are `<name>-<sha12>`, plus `-<6 hex>` of the
  `:subdir` when there is one, so two subdirectories of one monorepo commit
  under the same dep name do not collide. Each entry carries a
  `.tur-script-entry` metadata file (url, ref, subdir, commit, tree hash) --
  written last, so its presence is what "complete" means -- and a
  `.tur-last-used` stamp `tur cache clean --older-than` reads. `.git` is
  dropped: the tree is the content.
- **Atomic publication.** A `:cmake-deps` build records absolute paths into
  the tree, so a tree cannot be renamed after it is set up. On POSIX the tree
  keeps its private name (`.tree-...`) and the visible name is a symlink to
  it, created atomically; the loser of a race deletes its own tree. Windows
  renames the tree into place (a declared spice with native deps may need a
  second fetch there).
- **The tree hash** is `pkg_hash_dir` with the setup output left out
  (`pkg_hash_dir_excluding`: the top-level `spices/`, `cmake/`, `tur.lock` and
  the two bookkeeping files), so a locked tree verifies after it was set up.
- **The lock's closure rows** are named `<spice>/<dep>` and copied from the
  entry's own tur.lock. Building an entry under a lock seeds its tur.lock with
  them, so the `tur fetch` that sets it up checks out the pinned commits; a
  cached entry whose dependency is at another commit, or whose tree no longer
  hashes to the lock's, is TUR-E0634. A lock wins over `--update`;
  `tur fetch --script f --lock --update` moves it.
- **`--offline` / `--update`** are `tur run` flags (`--offline` already
  existed there); `tur fetch --script` takes `--update`.
- **The editor.** The LSP honors a header with or without the flag, reads the
  cache only (`g_script_deps_editor`: never fetch, print or stamp), and
  completes the declared spices' modules instead of the installed set's.
- **`tur <file> args`.** `drv_parse_global_flags` stops at the script, as it
  does at `--`, so every word after the file is the script's. A word shaped
  like a script (a `/`, or `.tur` / `.tur.sweet`) that names no file is a
  usage error naming it (exit 2).
- **Tests.** `tests/run-script-deps.sh` (ctest `tur_script_deps_tests`) covers
  each milestone's list against local `file://` repos; the fixtures
  `defscript-header-consumed`, `errors/defscript-needs-flag` and
  `errors/defscript-not-first` run in both `run.sh` and `run-turi.sh`. The
  LSP half of SD5 (resolution and completion from the cache) has no automated
  test yet.

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

- Everything in [installed-spice-libraries-plan](../archive/installed-spice-libraries-plan.md),
  which this plan assumes.
- Registries and name-only dependencies (`"json" "0.3"` resolved by name).
  Turmeric has no package index; every dep names its source.
- `:cmake-deps` and `:c-sources` declared directly in a header (as opposed to
  arriving through a declared spice). This is a likely follow-up once the
  `:spices`-only header has settled.
