# Turmeric 2 -- Claude Code Guide

## Reporting Bugs

We are on **one track of work to the finish line of v1.** Forward progress
toward that finish line takes priority. Land changes that move the track
forward; fix rough edges on the way to the line rather than blocking on them.

When you hit a real bug, defect, or expressiveness hole that you are NOT
fixing right now and that is worth not forgetting, a short note under
`docs/reported/<short-slug>.md` is welcome (one-line summary + severity, a
minimal repro, root cause with file:line when known, fix directions). But
this is an aid, not a gate -- do not let it stop you from pressing on toward
v1.

Every file in `docs/reported/` needs a row in `docs/reported/README.md`. CI's
"Docs lint" job checks that (and the r7rs SRFI table) via
`tests/check-docs-lint.sh`, which runs in well under a second. A Claude Code
`PreToolUse` hook (`.claude/hooks/docs-lint-before-push.sh`) runs the same
script before any `git push` of this repo and blocks the push while it fails.
It reads the working tree, so commit the fix before pushing again.

### Test suites -- `bash tests/run.sh`

`bash tests/run.sh` (the by-value HKT path) is the suite. To run one fixture,
a slice, or another harness, see
[docs/guides/running-tests-guide.md](docs/guides/running-tests-guide.md) --
e.g. `TUR_TEST_FILTER='^name$' bash tests/run.sh`. Do not guess variable
names: an unknown one is silently ignored and the whole suite runs. The legacy
`TUR_M7_HKT=0` carrier path has been retired -- there is no longer a second
suite. It is a signal, not a gate: run it when it is useful, read what it
tells you, and use your judgement about what to do with red -- a red suite
does not block landing or opening a PR.

### Archiving resolved reports -- STRICT RULE

`docs/reported/` holds **open** findings only. When a report is resolved,
**move it into the archive** -- never leave it parked in `docs/reported/`,
and **never create a `history/` subdirectory under `docs/reported/`**:

- `docs/archive/<slug>.md` -- the resolved report itself.
- `docs/archive/history/<slug>.md` -- the per-fix paper trail / older
  resolved bug reports.

`docs/reported/history/` is **forbidden**. A Claude Code `PreToolUse` hook
(in `.claude/settings.json`) blocks it at the source: any `Write`/`Edit`
whose path falls under `docs/reported/history/`, and any `Bash` command that
references that path, is denied before it runs. No git config or per-clone
setup is required -- the hook ships with the repo.

If you find a resolved report sitting in `docs/reported/` (or a stray
`docs/reported/history/`), relocate it to `docs/archive/` (or
`docs/archive/history/`); do not duplicate an already-archived report.

## Test Suite Timeout -- STRICT RULE

**Every single test suite run, NO EXCEPTIONS, MUST be invoked with a 12
minute (720000ms) timeout.** This applies to `bash tests/run.sh`, any
ctest invocation, any spice-side suite, any subset run, every time.

- `Bash` tool: pass `timeout: 720000`.
- Shell directly: wrap in `timeout 720 ...` (or platform equivalent).

If a test suite run is invoked without a 12-minute timeout, you have
failed the rule -- stop, cancel, and re-run with the timeout. No
exceptions for "quick" runs, no exceptions for "just one fixture", no
exceptions for "I'll be fast." Always 12 minutes.

## No Lazy `:int` Stand-Ins -- STRICT RULE

Typing a request handle, response handle, ctx, server, connection, file,
socket, callback function pointer, option value, result value, cons cell,
HAMT map, vector, struct handle, or any other distinct kind of thing as
`:int` because "it's a pointer under the hood" is **not allowed**. The
language has `defopaque`, `defstruct`, `:ptr<T>`, `:fn`, `option<T>`,
`result<T,E>`, parametric types, and typeclasses for a reason. Reaching for
`:int` is throwing all of that away and exporting an API the type checker
cannot help anyone use correctly.

This is the same defect the `tour-tourist` middleware shipped --
`(fn [req : int] : int)` instead of `(fn [ctx : Ctx] : option<Response>)` --
and the kind of thing that gets noticed downstream months later when
something tries to actually compose against it. **Do not ship more of it.**

If you are about to write `:int` for a parameter or return type, **stop**
and answer:

- Is this value an opaque handle to a thing of a specific kind? -> `defopaque`
  newtype, or a `:ptr<TheStruct>`. Never `:int`.
- Is it a callback / function pointer? -> spell out the function type, e.g.
  `(fn [Request] option<Response>)`. Never `:int`.
- Is it a boolean? -> `:bool`. Never `:int` with 0/1 convention.
- Is it nullable / optional? -> `option<T>`. Never `:int` with a 0 sentinel.
- Is it a tagged union? -> `result<T,E>` or a real ADT. Never `:int` with a
  status-code convention.
- Is it genuinely a machine integer with no further structure (length,
  count, byte, port, file descriptor we cannot wrap right now)? -> `:int` is
  fine. Default the other way.

The **only** legitimate `:int` is "this really is a number, not a stand-in
for something else." Everything else gets a real type.

Returning `option<T>` / `result<T,E>` from an **inline-C** body is
first-class -- there is no "inline-C can't easily build a result, so I'll
return `:int`" escape hatch. The codegen preamble carries typed builders
(`tur_ok_ptr` / `tur_err_int` / `tur_some_ptr` / `tur_none`, and the
carrier-level `tur_box_*`) that construct the canonical Result/Option
layout, so a fallible C constructor hands back a real `(Result Handle E)`
/ `(Option Handle)` with no struct hand-rolling and no sentinel integer.
The box those builders malloc is **owned by the caller**: a body whose
declared return type is `option`/`result` transfers it, and the compiler frees
it when the value is read back. So return a FRESH box, and do not declare
`option`/`result` for a box something else owns (a container's element) --
give that a borrow-shaped signature instead, as `vec-get` does.
See [docs/guides/inline-c-results-guide.md](docs/guides/inline-c-results-guide.md).

### When you notice this in existing code

Prefer a real type up front when you are writing new API surface -- it is
cheap then and saves a downstream caller from a loosely-typed footgun. But on
the one track to v1 this is a preference, not a gate: if an existing API leans
on `:int` as a type-eraser, you may match it, wrap it, or tighten it later as
the work toward the finish line dictates -- whatever keeps the track moving.

## Testing Float Behavior -- STRICT RULE

When investigating anything involving floats (type coercion, arithmetic,
truncation, printing, codegen), **always pick a literal with a non-zero
fractional part** as your first probe -- e.g. `7.1`, `3.25`, not `7.0` and
never the integer `7`. An integer literal cannot show truncation, rounding,
or int/float divergence, so starting with `7` produces a misleading "looks
fine" result and forces a second round with `7.1` to actually see the bug.
Skip the double-take: lead with `7.1`.

A float probe only shows the shapes someone thought to write. The mechanism
check does not depend on that: `python3 tests/check-emitted-float-conversions.py
FILE.tur` (ctest `tur_emitted_float_conversions`, and inside both source
fuzzers) reports every float<->int VALUE conversion in emitted C. The corpus
is at zero, so any finding is new. If you make the emitter convert on
purpose, spell it `TUR_AS(T, x)`; everything else it reports is a bug. See
[value-representations-guide](docs/guides/value-representations-guide.md#detecting-the-mechanism-not-the-shape).

## Locating Referenced Files -- STRICT RULE

When asked to act on a doc, plan, or file by path, **look before concluding it
is missing, and fetch before giving up**:

1. Check the working tree (`ls`/`Read` the path). If it is there, proceed.
2. If it is missing, run `git fetch origin main` (and/or inspect
   `origin/main:<path>`), then re-check. The container is cloned fresh and
   pinned to the feature branch at session start, so a doc merged to `main`
   after the branch diverged genuinely will not be in the tree until fetched.
3. Only after both steps come up empty may you report the file as not found.

Reading any file and fetching any branch are **always allowed** -- these are
read operations. The "develop on branch X" rule constrains only where commits
and pushes land; it never forbids reading or fetching `main` (or any other
ref). Do not rules-lawyer a write-side instruction into a reason to skip a
read. "We're on a branch, not main" is never a reason to avoid `git fetch`.

## Fixture Snapshots

`tests/fixtures/*/expected.c` are codegen snapshots. When your change settles,
regenerate them if you want them to match -- a "codegen mismatch" reflects the
codegen moving, not a separate bug. Carry as many red fixtures as you like, for
as long as you like: forward progress is the priority, and a failing suite --
intermediate or not -- never blocks a commit, a PR, or calling a change done.
Regenerate snapshots and reconcile failures if and when it serves the work, on
your own judgement, not because a gate demands it.

**When the codegen changes** (e.g. `main` signature, new preamble, new boilerplate):

1. Regenerate all snapshots:
   ```sh
   TUR=./build/tur
   for dir in tests/fixtures/*/; do
     if [ -f "$dir/expected.c" ]; then
       input="$dir/input.tur"
       [ -f "$input" ] || input="$dir/$(basename $dir).tur"
       [ -f "$input" ] && "$TUR" emit-c "$input" > "$dir/expected.c" 2>/dev/null
     fi
   done
   ```
2. If useful, look at what moved: `bash tests/run.sh 2>&1 | grep "^FAIL"`
3. Commit the updated snapshots alongside the codegen change -- never in a separate PR

Running `bash tests/run.sh` before a PR is optional; `FAIL` lines do not block
opening one.

### Fixture churn is not a deferral reason

Small codegen cleanups regenerate snapshots **in the same PR** -- never punt
the regen to a follow-up. "Fixture churn" on its own is not a reason to defer
a fix; it may be a reason to coordinate timing (don't land two large regens
on the same day, give in-flight branches time to rebase), but not to shelve
the work indefinitely. Only batch a cleanup into a coordinated regen window
if it is itself large enough to warrant coordination (touches >500 fixtures,
or carries interacting semantic changes that benefit from a single regen).

### Leak detection (ASan/LSan) policy

The Debug build compiles `tur` with `-fsanitize=address,undefined`; on Linux
ASan ships LeakSanitizer enabled. The compiler/codegen path is leak-clean, so
`bash tests/run.sh` runs **with leak detection ON** -- a genuine leak in the
`tur build`/`emit-c` path will fail the suite (this is intended; do not
suppress it).

**That covers `tur` itself, NOT the programs it emits.** Fixture programs are
compiled without any sanitizer and run with `detect_leaks=0`, so a leak in
EMITTED code passes `run.sh` silently -- the suite only compares printed
output. Use `tests/run-leak-check.sh` (opt in per fixture with a
`requires.leak-check` marker) when that is what you are chasing. The full
coverage map, and two traps that have produced wrong answers here, are in
[docs/guides/test-suite-portability-guide.md](docs/guides/test-suite-portability-guide.md#7a-leak-checking----what-is-covered-and-what-is-not). The tree-walking turi/eval **interpreter** intentionally never
frees its closures/registered natives (process-lifetime), so the harnesses
that exercise it (`run-turi.sh`, `run-flags.sh`) and their ctest targets
default to `ASAN_OPTIONS=detect_leaks=0`. Override with
`ASAN_OPTIONS=detect_leaks=1 bash tests/<harness>.sh` to opt back in. See
[docs/asan-debug-leaks-plan.md](docs/asan-debug-leaks-plan.md).

#### macOS startup hang -- outdated ASan runtime

On an OS-ahead-of-toolchain pairing the clang ASan runtime can **deadlock at
startup** -- it spins forever in a spinlock inside `InitializeShadowMemory` while
walking the dyld shared cache, *before* `main()` runs. The symptom is that
**every** `tur` invocation hangs, including `tur --version`. This is a
toolchain/OS runtime bug, not a bug in `tur`. See
[docs/reported/macos-asan-runtime-deadlocks-at-startup.md](docs/reported/macos-asan-runtime-deadlocks-at-startup.md),
which carries the measured mechanism and whether it is currently live.

**Check that this is actually your problem before working around it.** It does
not reproduce on a matched pairing, and it needs no `tur` at all to test:

```sh
printf 'int main(void){return 0;}\n' > bare.c
cc -fsanitize=address -g -o bare-asan bare.c
perl -e 'alarm 15; exec @ARGV' ./bare-asan     # hangs only when live
```

(`perl -e 'alarm N; exec @ARGV'` rather than `timeout N`: macOS ships no
coreutils `timeout`, and this is the form CI uses.) A hang that survives that
passing is something else -- do not reach for `-DTUR_DEBUG_SANITIZE=OFF`. The
variable to compare is **the Command Line Tools version against the OS
version**, not the Darwin version against a threshold.

On macOS the runtime is **not** linked into the binary: an absolute,
version-pinned rpath (`.../CommandLineTools/usr/lib/clang/<N>/lib/darwin`) makes
it a dylib loaded at every startup, which is why remedy 1 below fixes binaries
you already built. Runtimes are not interchangeable either -- the binary imports
a guard symbol naming the compiler that built it, so forcing a foreign runtime
under it is rejected, not silently used.

`TUR_DEBUG_SANITIZE` defaults **ON on every platform** (macOS included) -- we do
not auto-disable anywhere, because a silent, permanent loss of sanitizer
coverage in CI is worse than a loud hang (the `tur --version` CI smoke check
catches a real deadlock). Three ways to deal with it locally, cheapest first:

- **Update the Command Line Tools.** The only remedy that needs no rebuild,
  because the runtime is loaded from the toolchain rather than linked in:

  ```sh
  softwareupdate --list        # look for a Command Line Tools update
  softwareupdate --install "Command Line Tools for Xcode-<version>"
  ```

  Verify with `DYLD_PRINT_LIBRARIES=1 ./build/tur --version` that the dylib now
  loading is the new one.

- **Keeps ASan/UBSan coverage, but read the next section first:** build with a
  current LLVM whose ASan runtime understands the current dyld cache -- e.g.
  Homebrew LLVM. This is the route that then makes **every** fixture fail on
  `___asan_version_mismatch_check_v8` unless you pin the fixture compiler to the
  same toolchain; that is the same guard symbol described above, and
  "macOS: building fixtures against a sanitized `libturi.a`" below has the fix:

  ```sh
  brew install llvm
  cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug \
        -DCMAKE_C_COMPILER="$(brew --prefix llvm)/bin/clang"
  ```

- **Escape hatch (drops leak/UB detection):** explicitly opt out. This strips
  `-fsanitize=address,undefined` from the whole Debug build (and the
  `eval_import` test's fixture compile):

  ```sh
  cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DTUR_DEBUG_SANITIZE=OFF
  ```

The Release build never carries the sanitizers, so `tur --version` on a Release
build always works regardless -- the quickest way to tell this apart from a hang
in `tur` itself.

#### macOS: building fixtures against a sanitized `libturi.a`

A first fixture-suite run on macOS can produce dozens of failures that are a
**toolchain mismatch, not a product regression**. Two traps, both of which the
harness reports as `build failed`:

- **Mixed toolchains.** If `tur` is built with Homebrew LLVM (remedy 2 of the
  section above) but fixtures link with Apple's system `cc`, every fixture that
  pulls in the ASan-instrumented `libturi.a` fails to link with
  `Undefined symbols ... ___asan_version_mismatch_check_v8`. Either pin the
  fixture compiler to the same toolchain (`CC=/opt/homebrew/opt/llvm/bin/clang
  bash tests/run-jit.sh`) or -- better -- build unsanitized with Apple clang,
  which sidesteps both this and the startup deadlock:

  ```sh
  cmake -S . -B build-nosan -DCMAKE_BUILD_TYPE=Debug -DTUR_JIT=ON \
        -DTUR_DEBUG_SANITIZE=OFF
  cmake --build build-nosan -j
  ```

- **`--target tur` is not enough.** The `tur` executable links `tur_core`
  objects and (under `-DTUR_JIT=ON`) `tur_mir`; it never links `turi`. So
  `cmake --build <dir> --target tur` does not produce `libturi.a`, and every
  fixture then dies with `ld: library 'turi' not found`. Build all targets.

## CLI Argument Parsing -- STRICT RULE

Reading CLI arguments via any mechanism other than `*args*` or `stdlib/args.tur` is
**strictly forbidden** in this codebase. This means:

- **Allowed**: `*args*` (the pre-declared global cons list), `head`/`tail`/`cstr->parse-int`
  to walk it, or `args/parse` from `stdlib/args.tur` for structured parsing.
- **Forbidden**: `parse-first-arg`, `parse-arg`, or any inline-C that directly reads
  `g_tur_args` via a raw `struct __tur_cons` cast. These patterns have been purged from
  the codebase. Do not reintroduce them.

If you need to read argument N in a self-contained benchmark file that does not import
stdlib, define local `head`/`tail`/`cstr->parse-int` stubs as inline-C at the top of
the file -- the interpreter will override them with stdlib natives automatically.

## Experimental Compiler Features -- STRICT RULE

New **in-flight** compiler features (half-built, semantics in flux, or
carrying a known cost we are not ready to impose on everyone) ship behind
`--enable=<name>`, **never gateless until graduation**. Concretely:

- Add one row to `EXPERIMENTS[]` in `src/runtime/experiments.c` with **every
  descriptor field** populated -- `name`, `summary`, `plan_path`,
  `introduced`, `expires_at`, `lifecycle`, and an `opt_global` pointing at a
  `g_opt_<name>` bool the feature's elaboration reads. No partial rows.
- Write a plan in `docs/upcoming/` and point `plan_path` at it.
- Call `experiment_warn_if_used("<name>")` from the feature's elaboration
  entry point so the lifecycle warning (TUR-W0060/W0061) fires.
- `expires_at` is **advisory and NEVER blocks a release.** The release-cut
  skills surface an expiring row and proceed; the author then graduates it,
  shelves it, or bumps `expires_at` with a one-line rationale. It is a
  **deadline, not an earliest date** -- graduating early is routine.

  **Do not refuse a version bump because a row is at or past its expiry**, and
  do not reconstruct such a gate from older prose. No registry check has ever
  existed in `cut-*-release.md`; believing it did has stranded two releases.

Enable sources, in ascending precedence: user file
(`~/.config/turmeric/experiments.tur` `:enable [...]`), project manifest
(`build.tur` `:experiments`), CLI (`--enable=<name>`). Any `:experiments`
key in the project manifest -- including the empty list -- fully suppresses
the user file; CLI always wins.

This does **not** apply to diagnostic strictness (`--strict-effects`, ...),
codegen/operator knobs (`--dump-*`, `--emit-abi-trace`), build-system options
(`--build-dir`, `-I`), or already-shipping partial features that went
always-on at their current level. See
[docs/guides/experimental-flags-guide.md](docs/guides/experimental-flags-guide.md).

## Region Store Hooks -- STRICT RULE

Regions (RM3, on by default) rewind a generation only when nothing outside it
still points in. The compiler proves that with two locks: the static walk over
a bracket's result type, and the runtime **escape note** -- and the note is
only as complete as the set of stores that carry it. So:

**Any primitive that writes a caller's word into memory that can outlive a
`with-region` / `bt-scope` bracket MUST note that word.** In stdlib inline-C
that is one line before the store, `TUR_REGION_NOTE(word);` (words of a
by-value aggregate: `TUR_REGION_NOTE_WORDS(ptr, nbytes);`). The macros are
always defined in the emitted preamble and expand to `((void)0)` under
`TUR_REGIONS=0`, so the same text compiles on both arms. In the emitter, call
`emit_region_note_lvalue` after the store. Host-runtime C (hamt.c) calls
`tur_region_note_escape` directly.

The hooked set today: `vec-push!`/`vec-set-o!`, `mutmap-set!`, the HAMT
setters, `bt-set!`/`g-set!`, `atomic-store!/swap!/cas!`, `gen-arr-push!`,
`grid-set!`, `rcvec-push!`, `set!` on a global or shared cell, `set-field!`,
`set-deref!`, the closure-env fill, the malloc'd ctor boxes, the element-box
helpers, `rc/of`, every erasing ascription (including one written as a call
argument, and the implicit typed-node -> `:int` argument of an inline-C callee
or -- for a `:heap` node, constructors excepted -- of a Turmeric-bodied one,
which the elaborator makes one), a node handed to an inline-C callee's
type-variable parameter (noted at the call; `region-escape-via-wrapper`), and
the `#lang r7rs` `call/cc`
stack image (`TUR_REGION_NOTE_WORDS` over the copy; `region-escape-via-callcc`). A new `-set!`/`-push!`/insert
primitive, a new boxing site, or a new cell type joins the list in the same
change, and `tests/fixtures/region-escape-via-store` gets the case. A missed
hook is a silent use-after-rewind on the default build -- exactly what
`docs/archive/region-escape-through-unhooked-stores.md` documents -- not a
lost saving. See `docs/guides/gc-guide.md` (regions) for the reasoning.

**A TYPED node parameter of an inline-C body needs no hand-written note**: the
emitter notes it at body entry, because an inline-C body is opaque and may
retain it (`region-escape-via-inline-c`). The manual `TUR_REGION_NOTE` is
still what a body needs when the word arrives ERASED -- as `:int`, `ptr<void>`
or `Any` -- since the type no longer says it is a node. That is why the stdlib
hooks above are written the way they are: their `val` is a carrier word, not a
`Link`.

## `#lang` takes a base dialect and nothing else

`#lang <language>[/<reader>]` selects one mutually-exclusive base dialect:
`turmeric`, `saffron`, and each over `curly-infix`, `neoteric` or `sweet`.
That is the whole grammar -- a trailing token is a hard error (TUR-E0330).
`tur dialects` lists the set.

There is **no layer axis**, so there is no table to add a row to. When you
want new per-file syntax or a per-file gate, it goes in one of three places:

- A one-off syntax convenience -> a `#use-reader-macros` file.
- A `#`-dispatch everyone should have -> unconditional, in
  `reader_macros_install_builtins` (`src/compiler/reader_macros.c`).
- A semantic gate -> an `EXPERIMENTS[]` row behind `--enable=<name>`, which
  carries the lifecycle (TUR-W0060/W0061) and `expires_at`.

A new base dialect is a different thing and still legitimate; it is
slash-namespaced and lives in `lang_base_from_name`.

## Dev server port -- STRICT RULE

**Port 3000 is the developer's own Try Turmeric dev server** (`just web-dev` /
`tur run web-dev`). **Never use it.** Do not open it in a browser, `curl` it,
point a test at it, start a server on it, run the `web-dev` recipe, or stop or
restart whatever is listening there. A test run pointed at it tests whatever
checkout that server came from, not your branch, and it gets in the way of the
developer's own session.

Use a different port every time:

- **Browser tests:** run `npx playwright test` from `web/`. It picks a free
  port, starts its own server there, and never reuses a running one
  (`web/playwright.config.js`). `TRY_TEST_PORT=<port>` pins the port, and
  3000 is refused. Do not change the config back to a fixed port or turn on
  `reuseExistingServer`.
- **A server to poke at by hand:** from `web/`, run
  `npx vite --port <free port> --strictPort`, and stop it when you are done.
- **Checking whether the developer's server is up** is fine and read-only:
  `lsof -nP -iTCP:3000 -sTCP:LISTEN`. Then leave it alone.

A `PreToolUse` hook (`.claude/hooks/no-dev-server-port.sh`, registered in
`.claude/settings.json`) denies Bash commands that address port 3000, start a
server on it, run `web-dev`, or start vite without an explicit `--port`.

## Build System

The main turmeric compiler is built with CMake directly. Once `tur` is on
`PATH`, project-level recipes (build, test, docs, wasm, web-dev) are invoked
via `tur run`, which reads the same Justfile.

### Manifest filename: `build.tur` or `build.tur.sweet`

Wherever the manifest is read (walk-up discovery, workspace member
resolution, transitive `:spices` deps, cwd-relative `tur add`/`tur fetch`),
both `build.tur` (plain s-expr) and `build.tur.sweet` (sweet-exp syntax)
are accepted. When both exist in the same directory the plain `build.tur`
wins. `pkg_manifest_read` selects the reader from the extension, so a
`.tur.sweet` manifest goes through the sweet-exp preprocessor without any
caller change. `tur init --sweet` scaffolds the sweet variant; bare
`tur init` still emits `build.tur`.

### Bootstrap (compile `tur` from source)

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DCMAKE_POLICY_VERSION_MINIMUM=3.5
cmake --build build -j --config Debug
bash tests/run.sh                                                                # compiled-fixture suite
```

The built compiler lands at `./build/tur`. It carries the MIR JIT engine
(`tur jit`): `TUR_JIT` defaults ON on 64-bit x86-64/arm64, compiling the MIR
sources vendored under `external/mir/` -- no configure reaches the network.
Never edit `external/mir/` by hand; `external/mir/VENDORED.md` says how to
change MIR.

### Test suite size and runtime -- READ BEFORE ASSUMING A HANG

`bash tests/run.sh` runs **~1442 fixtures** and a **clean** run completes in
**~4-5 minutes** (≈265s measured on a 4-core box; it parallelizes across
`nproc`). Expect roughly this number and this duration.

- A wall-clock far above ~5 min (e.g. 15-20 min) almost always means **CPU
  contention** -- another suite run, build, or sweep competing for the same
  cores -- **not** a hang. Check for concurrent `tur`/`run.sh` processes before
  concluding anything is stuck, and don't launch overlapping full-suite runs.
- Each fixture's *run* phase has a 10s timeout (`expected.timeout` overrides),
  so a genuine runtime infinite loop surfaces as a **FAIL**, not an indefinite
  stall. An indefinite stall would have to be in an *untimed* compile phase
  (`emit-c`/`build`); those are rare and worth pinpointing with a timeout sweep.
- The expected result is `summary: 1442 passed, 0 failed` (the exact count
  shifts as fixtures are added/removed -- treat it as "~1440", not a hard gate).

### Overlapping runs cause false FAILURES, not just slowness -- READ THIS TOO

The point above is about wall-clock. The same overlap also produces **failures
that look exactly like product bugs**, and they are the more expensive trap
because the failure text never mentions timing. Two distinct causes, both
observed:

- **A rebuild landing mid-run.** Fixtures exec `./build/tur` directly out of
  the build tree, so a concurrent `cmake --build` swaps the compiler underneath
  the suite. During the link window the file exists but is not yet executable,
  and everything dispatched in that window dies with `Permission denied` --
  which `tests/run.sh` reports as `build failed`. A batch of those reads as a
  compiler regression. It has also been seen as
  `FAIL rp6-watch-with-help -- expected help output to mention 'tur repl'`,
  where `./build/tur repl --help` prints the expected text perfectly well when
  run by hand a moment later.

  `tests/run.sh` now stamps the binary at startup and re-checks at the end,
  printing a `WARNING: ... changed while this run was in progress` and exiting
  2 if anything failed. Other harnesses do not, so recognize the shape:
  **assertions that pass when you run them directly were probably never really
  run.**

- **`ctest -jN` oversubscribing the box.** `tests/run.sh` and
  `tests/run-turi.sh` each fan out across `nproc` internally, so `-j4` is not
  four tests sharing a machine but `4 x nproc` processes on it. Their
  per-fixture timeouts (10s compiled, 15s interpreted) then expire on work that
  would otherwise finish comfortably. Both targets are marked `RUN_SERIAL` so
  ctest gives them the machine, which is what they already assumed.

- **Memory, not CPU, inside a single turi run.** The tree-walking interpreter
  retains roughly 4 KiB per step of a trampolined loop -- its closures and
  continuations are process-lifetime by design -- so a fixture's step count is
  a *memory* multiplier under `--interpret` and nothing at all compiled. A
  1e6-step fixture peaks at ~3.5 GiB RSS; two of them co-scheduled by the
  harness's own `xargs -P nproc` is memory pressure on a 16 GiB runner, and the
  interpreter's wall clock goes superlinear once RSS passes ~2 GiB (5e5 -> 1e6
  steps costs 7.6x the time, not 2x). CPU parallelism is not the variable:
  `nproc` workers on `nproc` cores measured no slower than idle. When an
  `--interpret` fixture times out, check its peak RSS
  (`/proc/<pid>/status` `VmHWM`) before reaching for a bigger timeout. See
  [docs/archive/ci-cps-tramp-turi-timeouts-under-load.md](docs/archive/ci-cps-tramp-turi-timeouts-under-load.md).
  **Since 2026-10-05** an activation's call frame, its let / match-arm frames
  and their bindings are handed back when nothing captured them
  ([turi-call-frames-never-reclaimed](docs/archive/turi-call-frames-never-reclaimed.md)),
  so ordinary loops no longer grow per step. A closure capture keeps its
  frames, a re-entrant `call/cc` turns reclamation off for the rest of the
  run, and a by-value struct argument of a type the program writes a field of
  is still copied and kept (since 2026-10-07 nothing else is copied:
  [turi-immutable-struct-args-copied-per-call](docs/archive/turi-immutable-struct-args-copied-per-call.md)).
  On the Debug `tur`,
  ASan's free quarantine (256 MB by default) adds to every peak.

The rule of thumb: **before diagnosing a test failure, check whether anything
else was building or testing at the same time.** If it was, re-run alone before
believing the result. Do not launch a build and a suite concurrently.

Both harnesses report a per-fixture timeout as `timed out (>Ns)`. They used to
diff stdout first, so a killed fixture's partial output surfaced as a
`stdout mismatch` -- a claim about the answer, not the clock. If you are reading
an older CI log, treat a `stdout mismatch` on a long-running fixture as
possibly a timeout.

Release build:

```sh
cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release -DCMAKE_POLICY_VERSION_MINIMUM=3.5
cmake --build build-release -j
```

### Build output directory

`tur build <dir>`, `tur build --shared <dir>`, and `tur emit-c --build-dir`
route every intermediate `.c`/`.h`/`.o`, the `.tur-abi-cache/`, and the final
executable / shared library into a single build directory laid out as
`<build-dir>/{obj,bin,lib}/`. The dir is created on first use and gets a
`.gitignore` containing `*` so the contents never get accidentally committed.

Precedence (highest first):

1. `--build-dir <dir>` / `-B <dir>` on the command line.
2. `TUR_BUILD_DIR=<dir>` environment variable.
3. `:build-dir "<path>"` in the nearest `build.tur` (relative to the manifest dir).
4. Default: `<project-root>/build/` when a manifest exists, else `<cwd>/build/`.

Single-file `tur build <file.tur>` still uses `/tmp/tur-build/` for its
intermediate `.c` (already off-tree) and lands the binary wherever `-o` says;
when no `-o` is given inside a manifest-rooted single-main project, the
output is anchored under `<build-dir>/bin/`. To restore the legacy
"artifact in cwd" layout for a one-off, pass `--build-dir .`.

### Project tasks (`tur run`)

```sh
tur run build    # debug build
tur run test     # build + run tests
tur run release  # release build
tur run docs     # generate API documentation
tur run wasm     # build WebAssembly module (runs docs first)
tur run web-dev  # run web dev server
```

`tur run` is built into the compiler -- no extra binary required. The
upstream `just` binary still works against the same Justfile if you prefer it,
but it is not required. See
[docs/guides/tur-run-guide.md](docs/guides/tur-run-guide.md).

## Per-file Commands Inside a Spice

`tur check`, `tur emit-c`, `tur emit-h`, and `tur run <file>` walk up
from the input file looking for an enclosing `build.tur`. When they
find one, the spice's `src/` is added to the module-resolution search
path, and every `:spices` dep declared in the manifest contributes its
`src/` too -- including `:path`-based local deps and workspace siblings
resolved via the parent `:members` list. The result is that intra-spice
imports (like `(import frame/schema)`) resolve without per-spice `-I`
configuration in your editor, LSP, or format-on-save hook.

`tur fetch` is only needed for `:url`-backed deps. Local-source deps
(`:path` entries and workspace siblings) resolve with no fetch step and
produce no `tur.lock` entries.

- Explicit `-I <dir>` flags still work and win on name collisions.
- `--no-auto-spice` (global flag, before the subcommand) opts out.
- `tur build <file>` (single-file build) does **not** auto-discover --
  use `tur run <file>` for the same convenience, or pass `-I` explicitly.
- `tur build <dir>` and `tur run` (project mode) configure themselves
  from `build.tur`; auto-discovery is a no-op there. When `<dir>` holds a
  `build.tur`, the build descends into `src/` (recursively, including
  nested `src/<pkg>/`), skips the manifest itself, resolves the include
  path from the project's own `src/` plus each `:spices` dep's `src/`, and
  validates that every declared `:exports` module has a backing source
  file. See [docs/manifest-driven-build-descent-plan.md](docs/manifest-driven-build-descent-plan.md).

See [docs/guides/developing-spices-guide.md](docs/guides/developing-spices-guide.md#per-file-commands-inside-a-spice)
for the full rules.

## REPL Auto-Discovery (spice-repl-plan)

`tur repl` also walks up from cwd looking for `build.tur`. When it
finds one, the spice tree is AOT-compiled into a shared library
under `<root>/.tur-repl-cache/`, dlopened, and every exported defn is
bound as a callable native at the prompt (both bare `add42` and
qualified `mod/add42`). Source edits propagate via the `(reload)`
form, or automatically with `tur repl --watch`.

- `TUR_NO_AUTO_SPICE=1` opts out (the REPL behaves as before).
- `TUR_BIN=<path>` overrides the subprocess executable; tests set
  this to point at the in-tree build.
- `.tur-repl-cache/` is appended to `.gitignore` on first creation
  (if a `.gitignore` already exists).

See [docs/guides/repl.md](docs/guides/repl.md#working-with-spices-in-the-repl)
for the full workflow + cache layout + troubleshooting guide.

## Docstring Standard (`;;;`)

Use `;;;` (triple-semicolon) as the doc-comment marker. A docstring block
immediately precedes a `defn`, `defmacro`, `defstruct`, or `definstance`.

### Module docstrings

A contiguous `;;;` block that appears **before the first real definition**
(`defn`, `defmacro`, `defstruct`, `definstance`, `defopaque`) in a file
becomes the *module docstring*. Place it at the very top of the file,
followed by a `;;` comment line (which acts as the separator):

```turmeric
;;; tur/list -- untyped singly-linked Cons/nil list.
;;;
;;; Legacy list implementation; prefer tur/list for new code.
;;;
;;; Since: Phase B1
;; List type for Turmeric        <- ;; line terminates the module block
(defstruct Cons ...)
```

The `tools/gendocs.py` parser captures this block as `module['docstring']`
and renders it as a description paragraph on the per-module HTML page.
It also registers the module name as a key in the doc-lookup table so
`(doc 'tur/list)` returns the summary.

The separator can also be any non-comment, non-blank, non-definition form
(e.g. `(defmodule ...)`, `(export ...)`, `(extern-c ...)`).

### Format

```turmeric
;;; cons -- prepend a value to a list.
;;;
;;; Parameters:
;;;   value -- the element to prepend
;;;   next  -- the existing list (or nil-value for empty)
;;;
;;; Returns:
;;;   A new Cons cell pointing to next.
;;;
;;; Example:
;;;   (cons 1 (cons 2 (nil-value)))  ; => (1 2)
;;;
;;; Since: Phase B1
(defn cons [value next] : int
  ...)
```

### Required Fields

| Field | Required | Notes |
|-------|----------|-------|
| One-line summary (first `;;;` line) | Yes | `;;; name -- brief description` |
| `Parameters:` block | If non-zero arity | One `;;;   name -- desc` line per param |
| `Returns:` | Yes, unless `:void` | Describe the return value |
| `Example:` | Yes | At least one usage example |
| `Since:` | When known | Phase tag, e.g. `Phase B1` |

### Conventions

- First line: `;;; name -- brief summary` (name repeated for greppability)
- Blank `;;;` lines separate sections
- Examples use `; => result` to show expected output
- Internal helpers (e.g. `tur-contract-check`, `__functor_*`) get a shorter
  one-liner only -- no Parameters/Returns/Example blocks needed
- **ASCII only** -- use `--` (double hyphen), never em dashes (`--`)
- A non-`;;;` line resets the docstring buffer; the `;;;` block must be
  immediately above the definition it documents

### Docstring Levels

- **Exported / public API**: full docstring (summary + params + returns + example + since)
- **Internal helpers**: single-line `;;; name -- what it does`
- **Typeclass instances** (`definstance`): single-line summary

## Generated Docs

Run `tur run docs` (or `python3 tools/gendocs.py stdlib/ --out docs/api/`) to
regenerate the HTML API reference from `;;;` docstrings. Also pass
`--emit-tur stdlib/docstrings.tur` to rebuild the runtime lookup table.

The generated files live in `docs/api/` -- do not edit them by hand.

## Fixture Files

Test fixture files (`tests/fixtures/**/*.tur`) must be ASCII-only. The Turmeric
parser hangs on non-ASCII bytes (e.g. UTF-8 em dashes). Always use `--` instead
of `--`.

### `requires.*` skip markers

A fixture directory can carry a marker file that causes `tests/run.sh` to
PASS-skip it under certain conditions:

| Marker | Skips when ... |
| --- | --- |
| `requires.tsan` | `TUR_TSAN` is not `1` |
| `requires.stress` | `TUR_STRESS` is not `1`; the fixture is the full-size (e.g. 1e7) twin of a per-PR fixture, run only by `nightly-arm64.yml`. Its skip prints as `PASS <name> (stress-skipped)` |
| `requires.interp` | (override) forces the interpreter path even under non-TSan |
| `requires.interp-only` | always under `run.sh` (happy path and `errors/` alike); the fixture asserts a `tur --interpret` behaviour and is owned by `tests/run-turi.sh` |
| `requires.dedicated-runner` | always under `run.sh`; the fixture is owned by its own ctest target (e.g. `tur_eval_import`) |
| `requires.posix-apis` | the host is producing Windows binaries (`TUR_HOST_WINDOWS=1`) |
| `requires.spices` | the sibling `../turmeric-spices/` checkout is absent |
| `requires.musttail` | the fixture compiler (`$CC`) does not honour `TUR_MUSTTAIL` -- probed once per run with the same gate the emitter writes; today only clang on x86-64/aarch64 passes it |
| `requires.posix-apis` | `TUR_HOST_WINDOWS=1` (an MSYS2 `MSYSTEM`); the fixture's inline-C needs a POSIX API MinGW lacks -- `pipe()`, `fork()`, `getppid()`. Applies to negative fixtures too |

`tests/run-turi.sh` honours the same six markers (`requires.compiled`,
`requires.tur-only`, `requires.dedicated-runner`, `requires.spices`,
`requires.tsan`, `requires.stress`) on both its positive pass and its `errors/` pass, through one
helper, and counts every skip: its summary reads `P passed, F failed, S
skipped of D discovered` and the run fails if those do not add up. It also
PASS-skips any fixture whose program (own file or one `load` deep) contains
a user inline-C block -- see
[docs/guides/test-suite-portability-guide.md](docs/guides/test-suite-portability-guide.md#7d-what-run-turish-does-not-run----and-how-it-says-so).

Note `requires.interp` and `requires.interp-only` are near-homographs that do
opposite things: the first keeps the fixture in `run.sh` (routing it through
the compiling `tur run` path), the second removes it from `run.sh` entirely so
`run-turi.sh` can assert `--interpret` behaviour the compiled path does not
share. An `errors/` fixture whose `expected.diag` is an interpreter diagnostic
wants `requires.interp-only`.

A fixture may also carry `expected.xfail` (not a skip marker either): a
**named failing test**. `expected.stdout` holds the answer the spec requires
and the marker's contents say why the implementation does not produce it
yet; the stdout mismatch then PASSES as `(xfail)` in both `run.sh` and
`run-turi.sh`, and a match FAILS with "delete expected.xfail" -- the gap it
recorded has closed. Only the stdout diff is excused; a build failure, crash
or timeout still fails. Use it for a gap a plan wants on record as a test
rather than as prose (first use: r7rs-lang-plan D5's referential-transparency
gap, since closed and its marker deleted), never to park a regression.

A fixture may also carry `known.fnsan` (not a skip marker): its compiled
program is a KNOWN trap under clang's `-fsanitize=function`, and the marker's
first line names the open report.  `tests/run-fnsan.sh` (the `fnsan` CI job)
leaves it out of the gate run, then runs it alone and FAILS if it no longer
traps -- delete the marker then.  Never use it to park a new trap.

A fixture may also carry `requires.no-leak-check` (not a skip marker): the
compiled binary then runs with `ASAN_OPTIONS=detect_leaks=0`. Reserve it for
fixtures whose program intentionally registers process-lifetime closures the
caller never frees (e.g. reactor callbacks). The compiler/codegen path
(`emit-c`/`build`) is still leak-checked; only the spawned program opts out.

## Optional dependencies

Some fixtures depend on the sibling repo `../turmeric-spices/`. When present,
fixtures tagged `requires.spices` run as normal; when absent they auto-skip.
To enable them, clone the repo next to this one:

```sh
git clone https://github.com/turmeric-lang/turmeric-spices/ ../turmeric-spices
```

## Sweet-Expression Style

Turmeric files can opt into sweet-expression syntax with a `#lang` directive or
a `.tur.sweet` extension. Prefer the full sweet-exp style over plain s-expressions
when writing new `.tur.sweet` files.

```
#lang sweet-exp
```

Sweet-exp gives three tools. Use all three, choosing whichever reduces noise for
a given expression:

| Tool | Syntax | Use when |
|------|--------|----------|
| Indentation (t-expr) | leading whitespace replaces outer `(...)` | top-level forms and multi-line bodies |
| Neoteric | `f(x y)` replaces `(f x y)` | inline calls, especially single-result expressions |
| Rest-of-line | `$ expr` replaces the surrounding outer `(...)` | a single nested call that would otherwise need wrapping parens |

### Sweet-Exp Indentation -- no outer parens for forms and bodies

The primary rule: drop the outer `(...)` of any form whose body can be expressed
as an indented block.

```turmeric
#lang sweet-exp

defn square [x : float] : float
  *(x x)

defn classify [x : float] : cstr
  if >(x 0.0)
    "positive"
    if <(x 0.0)
      "negative"
      "zero"

let [x compute-x()
     y compute-y()]
  +(x y)

while not(window-should-close?(w))
  do
    clear()
    render-frame()
    swap-buffers(w)
    poll-events()
```

### Sweet-Exp Neoteric -- `f(x)` for inline function calls

Use `f(args...)` wherever a function call is embedded inside another expression
and the neoteric form is more readable than the equivalent s-expression.

```turmeric
; Inline call as an argument
println(name)
let [v normalize(cross(a b))]

; Nested construction
let [proj mat4-perspective(0.785 {800.0 / 600.0} 0.1 100.0)]

; Operator calls -- neoteric keeps the symbol in prefix position
+(x y)
*(a b)
not(done?())
```

Prefer neoteric over traditional parens for any call that fits on one line and
has a clear, short argument list.

### Sweet-Exp `$` -- rest-of-line as argument

Use `$` to avoid one level of wrapping parens when a line has a single nested
call as its only argument.

```turmeric
; Without $:
println(str-concat("Hello, " name))

; With $:
println $ str-concat "Hello, " name

; Chained:
println $ normalize $ vec3(1.0 0.0 0.0)
```

Prefer `$` over neoteric when the outer call takes exactly one argument that is
itself a call with multiple space-separated arguments.

`$` wraps the rest of the line in one pair of parens only when the rest needs
it -- a bare token sequence. When the rest is *already* one complete delimited
expression (a neoteric call `g(7)`, a parenthesised form `(g 7)`, a curly-infix
group `{a + b}`, a data literal), the wrap is suppressed, so `$` composes with
neoteric and curly-infix rather than double-applying them. A bare atom is the
one exception: `f $ g` is `(f (g))` per SRFI-110 -- a zero-argument call, not
`(f g)`.

### Curly-infix -- `{a + b}` for arithmetic

Curly-infix is enabled in every dialect (plain s-expression and sweet-exp
alike), so `{...}` arithmetic is always available. Use it for arithmetic
expressions to make operator precedence visual. Contract types, which used
to share bare-brace syntax, live behind `#refine{var : T | pred}` now.

```turmeric
let [area {width * height}]
let [hyp sqrt({*(a a) + *(b b)})]
```

### Data literals -- `#map{...}`, `#set{...}`, `[...]`

Map/vec/set construction has a literal syntax that works transparently
inside sweet-exp files (the reader dispatch sits below the sweet-exp
layer). Slot values are ordinary expressions, so neoteric and curly-infix
compose inside the literal:

```turmeric
let [m #map{:name name :age {age + 1} :active 1}]
let [pts [make-point(0 0) make-point(1 1) origin]]
let [tags #set{1 2 3}]
```

`[...]` lowers to `(vec-of ...)` only in expression position; in `defn`/`let`
binding position it stays a binding spec. `#map{...}` keys must be a keyword,
string, or int literal. See
[docs/guides/data-literals-guide.md](docs/guides/data-literals-guide.md).

### What still uses traditional parens

A few forms are cleaner in traditional syntax:

- **`import` / `export`** -- short enough that indentation adds no value
- **`cons` lists** -- `(cons x (cons y 0))` reads clearly; neoteric
  `cons(x cons(y 0))` is harder to scan
- **`fn` lambdas** -- always `(fn [x : int] : int {x * 10})`, never neoteric
  `fn([x : int] : int ...)` and never the indented head-line form. A lambda is
  almost always an *argument* to something else, so the sweet forms bury the
  one delimiter that tells you where the lambda ends:

  ```turmeric
  ; Good -- the lambda is visibly one argument of three
  over(px (fn [v : int] : int {v * 10}) p)

  ; Bad -- neoteric: `fn(` looks like a call to a function named fn
  over(px fn([v : int] : int {v * 10}) p)
  ```

  Curly-infix and neoteric still apply *inside* the body; it is only the `fn`
  head that keeps its parens.

  One consequence bites, so state it plainly: **a `(...)` form switches the
  indentation layer off for everything inside it** -- indentation *and* `$`.
  Only delimiter-based syntax survives (s-expr parens, neoteric `f(x)`,
  curly-infix `{a + b}`). A wrapped body must therefore be
  delimiter-complete:

  ```turmeric
  ; BROKEN -- `when` was relying on indentation the parens just took away;
  ; this parses as four flat elements, with no error.
  (fn []
    when {read-tvar(x) < 10}
      perform(Retry()))

  ; TUR-E0332 -- `$` inside brackets is rejected (it used to be inert, and
  ; silently survived as a bare `$` symbol). `tur explain` has the long form.
  (fn [msg : cstr] : unit
    log/error $ str("a" msg))

  ; Correct
  (fn []
    (when {read-tvar(x) < 10}
      perform(Retry())))
  (fn [msg : cstr] : unit
    log/error(str("a" msg)))
  ```
- **`handle` blocks** -- see below
- **Inline C blocks** -- the ` ```c ... ``` ` fence body always stays as-is.
  The enclosing `defn` can be either form: indented sweet-exp (body closes by
  dedent, no trailing paren) **or** traditional `(defn ...)` with the
  same-line ` ```) ` close. Prefer the indented form in new sweet-exp code:

  ```turmeric
  defn answer [] : int
    ```c
    return 42;
    ```
  ```
- **Single-form expressions** that fit on one line and are already minimal:
  `(nil-value)`, `(ok-val r)`, etc.

### `handle` -- keep the clause and its body on one line

`handle` takes a *flat* argument list that pairs up two at a time:
`(handle expr (Eff [p] k) body (Eff2 [p] k) body ...)`. Sweet-exp
indentation gives every element its own line, which splits each clause from
the body that answers it -- so the pairing, the only thing a reader needs, is
exactly what the layout destroys:

```turmeric
; Bad -- which body belongs to which clause? Count lines and hope.
println
  handle compute()
    (Add [x] k)
    resume(k {x + 10})
    (Mul [x] k)
    resume(k {x * 2})
```

Write `handle` in traditional parens, one clause per line:

```turmeric
; Good -- the pairing is on the page
println
  (handle (compute)
    (Add [x] k) (resume k {x + 10})   ; 3+10 = 13
    (Mul [x] k) (resume k {x * 2}))   ; 4*2  =  8
```

**When a clause body shares the line with its clause**, write it in
s-expression form. Curly-infix `{a + b}` stays; it is more readable for
arithmetic and is legal in every dialect. Mixing neoteric into a packed
one-liner makes it a two- or three-syntax hybrid that scans worse than either
pure form:

```turmeric
; Bad -- neoteric, s-expr and a neoteric zero-arg call on one line
(Log [msg] k) (do println(msg) (resume k nil-value()))

; Good
(Log [msg] k) (do (println msg) (resume k (nil-value)))
```

**A multi-line body under a `;;` comment is ordinary code** -- neoteric and
curly-infix read fine there, same as in a `(fn ...)` body. The one-liner rule is
about line density, not about banning neoteric inside `handle`:

```turmeric
(handle
  (let [new-state update-state(state)]
    draw(new-state)
    game-loop(new-state))
  ;; Render is forwarded to the backend, then the loop continues.
  (Render [obj] k)
    (do handle-render(obj)
        (resume k)))
```

`match` has the same flat pair-up-two-at-a-time shape as `handle`, so it gets
the same treatment: one pattern per line with the body it answers.

When a body is too long to share the line, give the clause its own `;;`
comment and indent the body under it -- punctuate the block rather than
letting the clauses run together:

```turmeric
(handle (serve req)
  ;; Log, then continue with the untouched request.
  (Log [msg] k)
    (do (println msg)
        (resume k nil))
  ;; A missing session is not fatal -- resume with the anonymous user.
  (Session [] k)
    (resume k (anonymous-user)))
```

### Complete example

```turmeric
#lang sweet-exp

import opengl/window :refer [make-window destroy-window window-should-close?
                              poll-events swap-buffers set-clear-color clear]
import opengl/shaders :refer [compile-shader shader-program use-program]

defn make-program [vert-src : cstr frag-src : cstr] : int
  shader-program
    compile-shader(":vertex"   vert-src)
    compile-shader(":fragment" frag-src)

defn main [] : int
  let [w make-window(800 600 "Demo")]
    set-clear-color(0.1 0.1 0.1 1.0)
    while not(window-should-close?(w))
      clear()
      swap-buffers(w)
      poll-events()
    destroy-window(w)
    0
```

## Indentation Style

Follow Clojure-style indentation rules:

### Regular function calls -- align args under the first arg

When a call spans multiple lines, every argument after the first is indented to
the column of the first argument (one past the opening `(`).

```turmeric
(some-long-fn arg1
              arg2
              arg3)

(println (str-concat "Hello, "
                     name))
```

### Special forms and macros -- 2-space body indent

`defn`, `fn`, `let`, `if`, `when`, `do`, `cond`, `for`, `while`, and similar
forms use a fixed 2-space indent for their bodies, regardless of column position.

```turmeric
(defn greet [name : cstr] : void
  (println name))

(fn [x]
  (* x x))

(if condition
  then-branch
  else-branch)

(do
  (step-a)
  (step-b))
```

### Binding vectors -- align bindings under each other

In `let`, `loop`, etc., each binding pair is aligned so the names line up.
**Keep each binding pair on its own line** -- never split a name and its value
(or a name, type, and value triple) across separate lines.

```turmeric
(let [x   1
      y   2
      foo (+ x y)]
  foo)
```

Good -- one pair per line:

```turmeric
(let [x   1
      y   2
      foo (+ x y)]
  foo)
```

Bad -- name and value (or name/type/value) split across lines:

```turmeric
(let [x
      1
      y
      2]
  ...)
```

If a single binding's value is too long to fit on one line, indent the
continuation under the value's starting column rather than breaking before
the value:

```turmeric
(let [x   1
      foo (some-long-fn arg1
                        arg2
                        arg3)
      y   2]
  ...)
```

### Nesting -- rules compose

Each sub-expression follows its own rule at its own column.

```turmeric
(let [f (fn []
          (println "Hello"))]  ; fn body: 2 past the ( of (fn
  ...)

(foo (bar a
          b)   ; bar args: aligned with a
     c)        ; foo args: aligned with (bar ...
```

## Inline C Block Style Rule

Always place the closing ` ``` ` and its enclosing `)` on the same line
(` ```) `). Placing ` ``` ` on its own line causes Markdown renderers to
interpret it as the end of any surrounding code fence, breaking rendered
documentation. Example:

```turmeric
(defn file-size [f] : int
  ```c
  FILE* file = (FILE*)f;
  return (int)ftell(file);
  ```)
```

## Stdlib Layout

```
stdlib/
  list.tur      -- singly-linked list (Cons/nil)
  option.tur    -- optional values (some/none)
  result.tur    -- error handling (ok/err)
  pair.tur      -- generic two-element pair
  str.tur       -- UTF-8 string view
  vec.tur       -- growable array
  macros.tur    -- core macros (cond, when, for, do-m, doc, ...)
  contract.tur  -- runtime contracts (assert!, require!, ensure!, ...)
  hamt.tur      -- persistent hash-array-mapped trie
  map.tur       -- map operations (delegates to hamt)
  fix.tur       -- Fix type (fixed-point of a functor); cata/ana
  free.tur      -- Free monad; free-pure/lift/bind/fmap/run
  sym.tur       -- interned runtime symbols (:Sym); sym->str/sym=?
  sym-dynamic.tur -- opt-in runtime str->sym interning (load on demand)
  ...           -- concurrency, effects, typeclass, I/O, etc.
  docstrings.tur -- AUTO-GENERATED by gendocs.py --emit-tur; do not edit
```

## Web / WASM

The web REPL lives in `web/`. It uses Monaco Editor and the Emscripten WASM
build of libturi (`web/turmeric.js`).

The doc panel in the web REPL calls `turi_doc_lookup(name)` (exported from
`src/wasm_glue.c`) to retrieve doc strings without printing to the console.

## Function Arity Style Guide

### Parameter count

There is **no hard cap** on positional parameters -- a function may declare an
arbitrary number, bounded only by `uint32_t`, matching the emitted C (which has
no limit of its own). Functions with more than ~5 positional parameters are
still a code smell, and declaring more than **16** emits the `TUR-W0041` lint
nudge back to this guide. The high ceiling is an escape hatch for generated
code, macro expansions, and wide interop shims, not a target -- prefer a
`defstruct` options value or a `& rest :type` variadic. (`MAX_FN_ARITY` survives
in the source only as the default size of a few internal codegen fast-path
buffers, which fall back gracefully for wider functions; it is no longer an
arity limit.)

### More than 5 params -- reach for `defstruct`

When a function needs many named, independent inputs, pack them into a
struct and pass a single options value:

```turmeric
(defstruct CsvOpts
  [delim       : int   ;; field separator (e.g. 44 = ',')
   quote       : int   ;; quote char (e.g. 34 = '"')
   has-header  : int   ;; 1 = first row is header
   infer-rows  : int   ;; rows to sample for type inference
   null-str    : cstr  ;; string that represents NULL (e.g. "")
  ])

(defn read-csv [src : cstr opts : CsvOpts] : int
  ...)
```

**Default values via partial application** (Haskell-style idiom):

```turmeric
(def default-csv-opts (CsvOpts 44 34 1 100 ""))

;; read-csv-fast already has opts baked in; call it with just the filename.
(def read-csv-fast (read-csv default-csv-opts))

(read-csv-fast "data.csv")
```

This composes cleanly with currying: `(read-csv default-csv-opts)` returns a
closure `(fn [src :cstr] :int ...)` that already has the defaults locked in.

### Genuine variadic interfaces -- use `& rest :type`

When a function takes an *unknown number of values of the same type*
(e.g., `println`, `format`, aggregation column lists), use a variadic rest
parameter:

```turmeric
(defn println-all [first : cstr & rest : cstr] : void
  (println first)
  ;; rest is a cons-list of :cstr; walk it with head/tail helpers
  ...)

(println-all "hello")              ;; rest = nil
(println-all "a" "b" "c")         ;; rest = cons("b", cons("c", 0))
```

The rest type is **fully type-checked** -- not just primitives. User-defined
types (`defopaque` newtypes, structs, ADTs, type applications) are resolved to
their full type and each rest argument is checked by identity at the call site:

```turmeric
(defopaque Route :int)
(defopaque Middleware :int)

(defn launch [& routes : Route] : int ...)

(launch (route!) (route!))     ;; OK -- all Route
(launch (route!) (make-mw))    ;; ERROR: rest arg 1 (expected Route, got Middleware)
```

Because of this, the old workaround "declare the rest as `:int` and cast the
opaque handles back inside the body" is **no longer needed** -- write the real
type. A bare `:int` rest now also rejects opaque/struct/ADT values; pass the
declared type instead. For a mix of distinct handle types, prefer two explicit
`:list<T>` parameters over a single untyped rest.

Rules for `& rest`:

- **One `&` per parameter list** -- the rest parameter must be last.
- **Type annotation required** -- `& rest :int`, `& rest :Route`, etc. An
  unknown type name is a hard error (it is never silently demoted to `:int`).
- **Typed by the declared element type** -- primitive rest uses a fast
  TypeKind compare; a user-defined rest type compares full type identity.
  A declared type parameter (`(defn f [A] [& xs :A] ...)`) is a polymorphic
  rest: it accepts any element type, but `A` is a single type variable, so
  all rest args in one call must unify to the same type (a mixed-type call
  is a `TUR-E0001` on the offending arg). This homogeneity is what backs
  `vec-of`/`hamt-of` element checking.
- **Nil when absent** -- calling with zero rest args passes `rest = 0`.
- **No inline-C in variadic bodies** -- inline-C blocks declare fixed C
  signatures; wrap the inline-C in a fixed-arity helper and call it from
  the variadic body.
- **Not auto-curried** -- variadic `defn` does not produce a curried entry
  point. You can still under-saturate up to the required positional params
  (which returns a variadic closure), but you cannot partially apply into
  the rest slot.

### Cons-list manipulation in `#fx{Unsafe}` code

The rest parameter is a `int64_t` holding a pointer to a linked list of
`__tur_cons_cell { int64_t head; int64_t tail; }` cells, or `0` (nil).
Inline-C helpers that walk it look like:

```turmeric
(defn cons-list-sum [lst : int] #fx{Unsafe} : int
  ```c
  typedef struct { int64_t head; int64_t tail; } __tur_cons_cell;
  int64_t acc = 0;
  __tur_cons_cell *p = (__tur_cons_cell *)(intptr_t)lst;
  while (p) { acc += p->head; p = (__tur_cons_cell *)(intptr_t)p->tail; }
  return acc;
  ```)
```

Or use a pure tail-recursive helper:

```turmeric
(defn list-sum-acc [lst : int acc : int] #fx{Unsafe} : int
  (if (= lst 0)
    acc
    (list-sum-acc (cons-tail lst) (+ acc (cons-head lst)))))
```

### Quick decision guide

| Situation | Reach for |
|---|---|
| >5 named, independent params | `defstruct` options value |
| Default values + currying | `defstruct` + `(def fast (f defaults))` |
| Unknown number of same-type values | `& rest :type` variadic |
| Recursive accumulator threading context | closure-capture for context; fixed-arity for changing args |
| Genuinely >16 params | Something is wrong -- split the function |
