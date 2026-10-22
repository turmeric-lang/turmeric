# Plan: Project-less tools -- `tur serve` in one line, and a spice binary run from anywhere

> **Status:** Draft, not started. Written 2026-10-21.
> **Last Updated:** 2026-10-21
> **Type:** Tooling / CLI (`src/driver/cli.c`, `src/driver/cmd_run.c`,
> `src/compiler/install.c`), stdlib (`stdlib/httpd.tur`, a new
> `stdlib/tools/serve.tur`).
> **Builds on:** [installed-spice-libraries-plan](../archive/installed-spice-libraries-plan.md)
> (IL0's install layout and "an installed root is a fetched project" helper)
> and [script-spice-deps-plan](script-spice-deps-plan.md) (SD-D3's cache
> directory, SD-D6's `tur <file> args` dispatch). PT0-PT2 below need neither;
> PT3 reuses both and says what it carries itself when they have not landed.
> **See also:** [global-spice-install-plan](../archive/global-spice-install-plan.md)
> (`tur install`, `:bin`, the `tur <name>` fallthrough -- all landed),
> [engine-selection-plan](../archive/engine-selection-plan.md) (`tur run`'s
> engine delegation), [security-audit-plan](security-audit-plan.md) (M-4, the
> static-file hardening this plan keeps),
> [releases-and-installation-guide](../guides/releases-and-installation-guide.md)
> (why `tur run` does not work from a release tarball today).

## Goal

Two things, which turn out to be one mechanism.

**1. A spice binary runs from anywhere, with no `build.tur` and no install.**
Today a spice's `:bin` entries are reachable two ways: `tur install` builds
them into `~/.local/bin/` and `tur <name>` finds them on `PATH`, or you clone
the spice, `cd` into it and build by hand. Both work without a project at run
time, but neither is ad hoc. There is no `npx` / `uvx` / `cargo run -p`: no way
to say "run this tool once, from this source, with these arguments" and have
`tur` fetch, build, cache and exec it. After this plan:

```sh
tur x https://github.com/turmeric-lang/turmeric-spices \
      --ref notebook-v0.1.0 --subdir spices/notebook -- render hello.tur.md
tur x ~/src/my-tool -- --help        # a local checkout, built in place
tur x notebook -- new hello.tur.md   # an installed spice, exec'd directly
```

The first form fetches and builds on first use and is offline afterwards.
Nothing is registered, nothing lands in `~/.local/bin/`, and the project-less
search path is untouched.

**2. Serving a directory is one line, on a fresh `tur`, with nothing installed.**
`python3 -m http.server` is the on-ramp that makes people keep Python around.
The same thing in Turmeric is about twelve lines today
(`stdlib/httpd.tur`'s `mw-static` does the hard part), and still falls short
of Python on `GET /`. After this plan:

```sh
tur serve                 # . on http://127.0.0.1:8000/
tur serve dist -p 8080    # a directory and a port
tur serve --bind any      # every interface, the way Python defaults
```

`tur serve` is a Turmeric program shipped with the stdlib and run through
the engine selection `tur run` already has. It is the first built-in tool
written in Turmeric; the machinery that runs it is the machinery of goal 1
pointed at a program we ship rather than one we fetch. A third party who
wants a different server publishes a spice and gets `tur x <url>` for free.

## Where things stand

| Piece | Where | State |
| --- | --- | --- |
| Static files: Content-Type table, binary-safe body, strong ETag + `If-None-Match`, traversal / dotfile / symlink containment | `httpd-static-serve!`, `stdlib/httpd.tur:5377`; `mw-static` `:5536` | Done. Verified from a scratch directory outside the repo: `/a.txt`, `/sub/b.txt`, `/index.html` serve with the right type and an ETag; `/../serve.tur` and `/nope` are 404. |
| `GET /` with an `index.html` present | `stdlib/httpd.tur:5444` requires a regular file | **404.** No index fallback. |
| `GET /sub/` | same | **404.** No directory listing. |
| Bind address | `httpd-set-bind-any!` `:228`, `httpd-set-bind-addr!` `:257` | Loopback by default (security-audit M-4). Python binds all interfaces. |
| Request log | `mw-log` `:4504`, `mw-log-to` `:4543` | Done. |
| Directory reading | `fs/read-dir` `stdlib/fs.tur:1057`, `fs/stat` `:62`, `fs/stat-size` `:106`, `fs/stat-mtime` `:123`, `fs/stat-kind` `:372` | Done. |
| Argument parsing in a script | `*args*` (after `--`), `stdlib/args.tur` | Done. |
| Engine delegation for a single file | `run_delegate_engine`, `src/driver/cmd_run.c:34`; `tur jit` `src/driver/cli.c:1037` | Done. The interpreter has no httpd natives (`src/turi/` never mentions httpd), so `interp` is not an engine for this program. |
| Locating the shipped stdlib | `resolve_stdlib_root()`, `src/driver/cc.c:142` | Done. |
| `:bin` build | `tur install` step 4, `src/compiler/install.c:553` builds each entry into `<src_dir>/build/<bin_name>` by `system()`-ing `tur build <entry> -I ... -o ...` (`:690-714`) | Done, but only reachable through `tur install`. No driver path builds or runs a `:bin` otherwise (`n_bins` has no reader outside `install.c` and `pkg.c`). |
| `tur <name>` fallthrough | `drv_try_external_subcommand`, `src/driver/cli.c:1272` | Done. `argv[1]` must be `[A-Za-z0-9_-]+` (`:1277-1279`), so a file name never reaches it. Built-ins always win. |
| Install layout, cache directory, shebang dispatch | IL0 (D4/D5), SD-D3, SD-D6 | Planned, not started. |
| `tur run` from a release tarball | [releases-and-installation-guide](../guides/releases-and-installation-guide.md#does-not-yet-work-outside-the-repo-tree) | The cc path needs `src/runtime/`, which the tarball does not ship. The JIT path does not. |
| cc-path noise | the httpd preamble's `__tur_include__` statics (`stdlib/httpd.tur:94-105`, `:2903`, `:5162`, ...) | A `tur run` of a program that loads `stdlib/httpd.tur` prints ~20 `-Wunused-function` / `-Wunused-variable` warnings from gcc. The JIT path is silent. |

## Design decisions

### PT-D1. Three spellings of "one line", one mechanism

| Spelling | What runs | Needs |
| --- | --- | --- |
| `tur serve [dir] [flags]` | `stdlib/tools/serve.tur`, shipped with `tur` | nothing installed, no network |
| `tur x <source> [-- args]` | a spice's `:bin`, fetched and built into a cache | network on first use |
| `tur serve.tur 8000` | the user's own script (SD-D6) | a file |

All three end in the same call: "build this entry with these include dirs,
then exec it with these arguments", which is what `run_delegate_engine` and
`install.c`'s step 4 each do today in their own words. PT1 factors that into
one function in the driver that the three callers share.

### PT-D2. `tur serve` is a stdlib program behind a driver arm, not C

The server is written in Turmeric, at `stdlib/tools/serve.tur`, because the
whole server already is: `httpd-new`, `mw-static`, `mw-log`, `fs/read-dir`.
A C implementation would duplicate the hardened static path for no gain and
could not be read as an example. The driver arm is thin:

- `serve` joins the command table in `src/driver/cli.c` (next to `run`).
- It resolves `<stdlib root>/tools/serve.tur` through `resolve_stdlib_root()`,
  the same lookup every stdlib `load` uses, so it works from the dev tree and
  from an installed prefix alike.
- It runs the file with `tur run`'s engine selection: the JIT when this build
  carries one, otherwise the cc path. `interp` is refused with the engine
  error `run_delegate_engine` already prints, since the interpreter has no
  httpd. `--engine=<e>` is honored for debugging.
- Everything after `serve` is handed to the program as `*args*`; no `--` is
  needed because the driver arm takes no flags of its own.
- A built program is cached the way `tur run <file>` caches today
  (`/tmp/tur-build/<hash>.c`); the first `tur serve` on a machine pays one
  build (0.8 s warm on the cc path here), later ones do not.

Two rough edges on the cc path are fixed in the same milestone, because they
are the first thing a newcomer sees:

- The httpd preamble statics get `__attribute__((unused))` where the C
  compiler would otherwise warn. Every program that loads `stdlib/httpd.tur`
  benefits, not only `tur serve`.
- A release build without a JIT needs `src/runtime/` to run `tur serve`, the
  same gap `tur run` has from a tarball. That gap is tracked in the
  releases guide; this plan does not solve it, but PT1's test runs `tur serve`
  against an installed prefix (`cmake --install`) so the dependency is visible
  rather than discovered by a user.

### PT-D3. Defaults match Python where it is safe, and differ where it is not

| | `python3 -m http.server` | `tur serve` | Why |
| --- | --- | --- | --- |
| Directory | cwd, `-d DIR` | `.`, positional `[dir]` | one line |
| Port | 8000, positional | 8000, `-p N` / `--port N`; `0` picks a free port and prints it | fixtures and scripts need a free port |
| Bind | all interfaces, `-b ADDR` | `127.0.0.1`; `--bind any` or `--bind <addr>` | security-audit M-4 chose loopback for every stdlib server; a dev server that is reachable from the coffee shop by default is the one thing Python gets wrong |
| `GET /` | `index.html`, else a listing | same | PT0 |
| Dotfiles | served | refused (404) | `httpd-static-serve!` already refuses them; the listing hides them too |
| Request log | one line per request on stderr | same, via `mw-log` | |
| Startup line | `Serving HTTP on 0.0.0.0 port 8000 (http://0.0.0.0:8000/) ...` | `Serving . on http://127.0.0.1:8000/ (Ctrl-C to stop)` | |
| Workers | one thread | `httpd-new`'s four | |
| Stop | Ctrl-C | Ctrl-C; `httpd-run` blocks and SIGINT ends the process | no signal handling added |

Other flags: `--no-listing` (index only; a directory without one is 404),
`--index <name>` (default `index.html`), `-q` / `--quiet` (no request log).
Parsed with `stdlib/args.tur`; an unknown flag prints the usage and exits 2.

### PT-D4. Index and listing are an option on the static path, not a change to `mw-static`

`mw-static` and `httpd-static-serve!` keep their audited behavior: a request
that resolves to a directory is a miss. Both features are opt-in through a
small options struct, so a framework that composes `mw-static` after a router
does not grow a directory listing by surprise:

```turmeric
(defstruct StaticOpts
  [index   : cstr   ;; file to serve for a directory path; "" disables
   listing : bool]) ;; render an HTML listing when there is no index

(defn httpd-static-serve-with! [conn : ptr<void> opts : StaticOpts root : cstr rel : cstr] : bool ...)
(defn mw-static-with [opts : StaticOpts root-dir : cstr ^fat next : (fn [ptr<void>] nil)] : ptr<void> ...)
```

`httpd-static-serve!` becomes `httpd-static-serve-with!` with
`(StaticOpts "" false)`, so nothing existing changes. The new path, after the
containment check `httpd-static-serve!` already performs (the resolved
directory must sit under the resolved root):

1. A directory path without a trailing `/` gets `301` to the same path with
   one, so the listing's relative links resolve. Python does the same.
2. With `index` set and `<dir>/<index>` a regular file, serve it through the
   existing file path, ETag and all.
3. With `listing` set, render HTML: entries from `fs/read-dir`, sorted
   byte-wise, directories first and suffixed `/`, names HTML-escaped and
   percent-encoded in the `href`, size and mtime from `fs/stat`, dotfiles
   omitted (they are unreachable anyway). `Content-Type: text/html;
   charset=utf-8`, no ETag (the body depends on the directory's contents, and
   `fs/stat` of the directory does not cover them).
4. Otherwise a miss, as today.

The MIME table (`stdlib/httpd.tur:5323`) gains the handful a static site
needs and lacks today: `mjs`, `map`, `xml`, `webp`, `woff`, `woff2`, `mp4`,
`md`. Unknown stays `application/octet-stream`.

Files are still read fully into memory (`:5319-5320`). A 2 GB file through
`tur serve` is a known limit, not a goal; streaming is out of scope.

### PT-D5. `tur x <source> [--bin name] [-- args]` runs a spice binary without registering it

The source grammar is `tur install`'s, so nothing new is learned:

| `<source>` | Resolution | Build |
| --- | --- | --- |
| an installed name | the registry entry's directory (`state.tur :dir`, IL0 D5; today's reconstruction as fallback) | none: exec the bin `tur install` already built |
| a path (contains `/`, or starts with `.` or `~`) | that checkout | the `:bin` is built into `<checkout>/build/`, exactly where `install.c:553` puts it, with the include list the IL0 helper computes for that root; rebuilt when any source under `src/` is newer than the binary |
| a URL, with `--ref` / `--subdir` | a cache entry under `$TUR_HOME/cache/script-spices/<name>-<sha12>/` (SD-D3's directory and naming), set up as a fetched project by the IL0 helper | once per entry, into the entry's `build/`; the entry is immutable afterwards |

Then exec `<entry's build dir>/<bin>` with the arguments after `--`, exit
with its status. `--bin <name>` picks one when the spice declares several;
with exactly one it is implied; with several and no `--bin` the names are
listed and the command exits 2.

What `tur x` deliberately does not do:

- It never writes `state.tur`, never links into `~/.local/bin/`, and never
  adds anything to the project-less search path. Running a tool once is not
  installing it. `tur install` remains the way to get `tur <name>`.
- It never fetches when the entry exists. `--refresh` re-resolves a moving
  ref (a branch name) and builds a new entry; the old one stays until
  `tur cache prune` (SD plan) removes it.
- It does not read a `build.tur` from cwd. Inside a project or outside one,
  `tur x` means the same thing.

When PT3 lands before IL0 and SD1, it carries the minimum itself: the cache
directory and the `<name>-<sha12>` naming exactly as SD-D3 specifies, and the
first-level dep fetch `install.c:575-649` does today. Both are replaced by the
shared helpers when those plans land, with the same on-disk layout, so no
entry has to be rebuilt.

### PT-D6. `tur <file.tur> args` lands here, and `tur run <file> args` stops dropping arguments

SD-D6 says its dispatch rule "is independent of the header and could land on
its own". It is the third spelling in PT-D1, so it lands in this plan:

- When no built-in matches and `argv[1]` names an existing file (contains a
  `/`, or ends in `.tur` / `.tur.sweet`), run it as `tur run <file> --
  <rest>`. The fallthrough's character rule (`cli.c:1277`) already excludes
  such names from the `tur-<name>` probe, so the two rules cannot both match.
- `tur run <file> a b` passes `a b` to the program. Today `tur run` scans
  every token before `--` (`src/driver/cmd_run.c:112-129`): `-I`, `--release`,
  `--offline` and `--engine` are recognized anywhere, the first bare token is
  the file, and every later bare token is dropped without a word (`:128`).
  The new rule keeps the flags where they are and makes the later bare tokens
  program arguments. A program that wants a literal `--release` of its own
  still gets it after `--`.

`tur serve.tur 8000` then works with the twelve-line script from the goal,
and the shebang line `#!/usr/bin/env tur` works for any script that takes
arguments.

### PT-D7. Built-ins win, so `tur serve` shadows a user's `tur-serve`

That is the rule the fallthrough already has, and this plan keeps it. A user
who installs a different server as `tur-serve` reaches it as `tur x serve`
or by its binary name; `tur --help`'s "External commands:" section lists it.
Not worth a `--no-builtin` escape until someone asks.

### PT-D8. No `--enable` gate

A subcommand, two stdlib functions and an options struct are not an in-flight
language feature. `tur x`'s cache is a build-system concern like `-I` and
`--build-dir`, which CLAUDE.md's experimental-features rule exempts. SD-D7
gates `script-deps` because of the header format; `tur x` has no new surface
in a source file.

## Milestones

### PT0 -- Index and listing in `stdlib/httpd.tur`

- [ ] `StaticOpts`, `httpd-static-serve-with!`, `mw-static-with` (PT-D4).
      `httpd-static-serve!` and `mw-static` delegate with `(StaticOpts "" false)`.
- [ ] The directory redirect, the index path, and the listing renderer.
      HTML-escape and percent-encode helpers live next to the renderer (check
      `stdlib/httpd-string.tur` first; reuse if present).
- [ ] MIME additions.
- [ ] `__attribute__((unused))` on the preamble statics that gcc warns about
      on the cc path (PT-D2). `tests/check-cc-warn-ratchet.sh` records the
      new baseline.
- [ ] Fixture `tests/fixtures/httpd-static-index-listing` (compiled, in the
      shape of `httpd-mw-static`): `/` serves `index.html`; `/sub` is `301` to
      `/sub/`; `/sub/` without an index lists `b.txt` and omits `.hidden`;
      `--no-listing` behavior gives 404; a listing of a directory containing
      `<a>.txt` is escaped; `mw-static` unchanged (`/` still 404).

### PT1 -- `tur serve`

- [ ] `stdlib/tools/serve.tur`: flags per PT-D3, `mw-log` unless `-q`,
      `mw-static-with` over a 404 base handler, the startup line, exit 2 on
      a bad flag or a directory that does not exist.
- [ ] One driver function "build entry with includes, exec with args" shared
      by `run_delegate_engine`, the `serve` arm and (PT3) `tur x`.
- [ ] The `serve` row in `src/driver/cli.c`, its usage text, and the
      completion script (`src/cli/completion.c`).
- [ ] `tools/gendocs.py` skips `stdlib/tools/` (programs, not modules), or
      documents it as a "Tools" page; decide when the first one exists.
- [ ] Fixture `tests/fixtures/cli-serve/hook.sh` (in the shape of
      `httpd-mw-log/hook.sh`): `tur serve --port 0 <dir>` in the background,
      read the port from the startup line, curl `/`, `/sub/`, a file, a
      dotfile, and `/../`, then kill it. Runs under `TUR_NO_GLOBAL_SPICES=1`
      like the rest of the suite.
- [ ] A ctest that installs to a scratch prefix (`cmake --install`) and runs
      `tur serve --port 0` from an unrelated directory, so the release-layout
      dependency in PT-D2 is a red test rather than a user report.
- [ ] `docs/guides/web-emscripten-tutorial.md:188` replaces
      `python3 -m http.server 8080 --directory hello-wasm` with
      `tur serve hello-wasm -p 8080`. Same for the archived plan's copy if it
      is referenced by a guide.

### PT2 -- `tur <file.tur> args` and `tur run <file> args`

- [ ] The dispatch rule (PT-D6) in `main.c` after the command-table lookup
      and before `drv_try_external_subcommand`.
- [ ] `tur run <file> a b` passes `a b` through; `-I`, `--release`,
      `--offline` and `--engine` keep working on either side of the file.
- [ ] Fixtures: `tur hello.tur a b` prints both args; `tur check.tur` runs
      the file, not `tur check`; `tur run f.tur a` reaches `*args*`;
      `tur run f.tur -- a` still does.

### PT3 -- `tur x`

- [ ] Source resolution for name, path and URL (PT-D5). Name and path need
      no network.
- [ ] The cache entry for a URL source: SD-D3's directory and naming, the
      IL0 fetched-project helper when it exists, else the carried minimum.
- [ ] Build the chosen `:bin` with the shared driver function from PT1;
      the freshness check for a path source.
- [ ] `--bin`, `--refresh`, `--offline` (refuse to fetch), and the usage text.
- [ ] `tests/run-install.sh` cases with a scratch `TUR_HOME`: `tur x` of a
      `file://` bare repo builds once and runs twice (the second run with the
      network unreachable), `tur x <path>` rebuilds after a source edit,
      `tur x <installed>` execs without building, a two-bin spice needs
      `--bin`, and `state.tur` and `~/.local/bin/` are untouched throughout.

### PT4 -- Docs

- [ ] `docs/guides/consuming-spices-guide.md`: "Running a spice's tool
      without installing it" (`tur x`), next to IL6's project-less section.
- [ ] `docs/guides/tur-run-guide.md` or a new `docs/guides/tur-serve-guide.md`:
      `tur serve`'s flags, the loopback default and how to open it up, the
      limits (in-memory files, no TLS, no ranges).
- [ ] `docs/guides/developing-spices-guide.md`, the `:bin` section: a spice
      with a `:bin` is runnable with `tur x` before it is installed.
- [ ] CLAUDE.md: one line under "Per-file Commands Inside a Spice" for
      `tur <file.tur> args`, and one under "Dev server port" noting that
      `tur serve` defaults to 8000, never 3000.
- [ ] README's quick-start gains the three-line serve example.

## Open questions

1. **The name `tur x`.** It follows `uvx` / `npx`; `tur exec` is clearer and
   longer; `tur run --spice <source>` overloads a verb that already means
   two things. Proposed: `x`, with `exec` as an alias if the first review
   wants it.
2. **Bind default.** Loopback is the right default for a stdlib server. For
   a dev tool whose main use is "look at this on my phone", `--bind any` is
   one flag away and the startup line can say so. Keeping loopback; revisit if
   the flag turns out to be typed every time.
3. **Windows.** The httpd fixtures build native Windows binaries today, so
   the server itself runs there. `tur serve` on Windows depends on the JIT
   (the cc path needs `src/runtime/`) and on `%LOCALAPPDATA%` for the cache;
   PT3's cache code goes through `global.c`'s existing path helpers, so this
   should fall out, but it is untested until the Windows CI leg runs PT1's
   hook fixture.
4. **Should the listing carry an ETag?** Skipped in PT-D4 because the
   directory's mtime changes on entry add/remove but not on an entry's
   content change, so a weak tag could be wrong. Python sends none either.
5. **`tur serve` of a project.** Inside a `build.tur`, should `tur serve`
   default to the project's build output (`<build-dir>/bin/` or a
   `:web-root` key) rather than `.`? Not in this plan; `.` is what people
   expect, and a flag is cheap to add later.

## Out of scope

- TLS, range requests, compression, and streaming large files. `tur serve`
  is a dev tool; `stdlib/httpd.tur` has `mw-etag`, `httpd-compress` and a TLS
  hook for programs that need more.
- CGI (`python3 -m http.server --cgi`, removed in Python 3.13).
- A registry of spice names resolvable without a URL. `tur x <name>` means
  an installed name only.
- Spices declared inside a script, lock files, and `tur cache prune`:
  [script-spice-deps-plan](script-spice-deps-plan.md).
- Shipping `src/runtime/` in release tarballs so the cc path works from a
  downloaded `tur`: tracked from the releases guide, surfaced by PT1's ctest,
  not solved here.
