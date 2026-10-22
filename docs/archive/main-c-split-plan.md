# Split `src/main.c` into `src/driver/`

> **Status:** done, 2026-10-21 (branch `claude/main-c-split`). Every phase
> landed. `src/main.c` is the entry point only (~190 lines); the driver is 16
> files under `src/driver/`, declared through `driver.h` and `commands.h` and
> checked by `tests/check-driver-headers.py`. Dispatch reads one command table
> (`COMMANDS[]`) and one global-flag table (`GLOBAL_FLAGS[]`) in
> `driver/cli.c`, pinned by the `tests/run-flags.sh` goldens. Where the work
> departed from the text below: `run_delegate_engine` went to `cmd_run.c`, its
> only caller, since its interpreter branch is not JIT-only; and Phase 4
> changed two outputs, both stated in its commit (the completion scripts now
> list every command, and `tur check --dump-refine=json` prints beside
> `tur check` in `--help`). The original plan follows, measured on `main`
> 9ac3cd43.

## Why

`src/main.c` is 14,942 lines and holds 259 functions, with about 25
file-scope globals and 26 forward declarations scattered through the file. It
contains every `tur` subcommand, the pass pipeline, the cc invocation and
autolink, the preamble and split-lib caches, the JIT and REPL-JIT builds, the
test-suite worker, the docs server and the SMT front end. The largest
functions:

| Function | Lines |
|---|---|
| `tur_main_inner` (global flags + command dispatch) | 1,643 |
| `cmd_build_multi_files` | 666 |
| `cmd_run` | 509 |
| `cmd_build_once` | 471 |
| `cmd_build_project` | 401 |
| `cmd_eval_h` | 378 |

The size has a cost beyond scrolling:

- **Comments drift from what they describe.**
  [stale-comments-build-driver](stale-comments-build-driver.md)
  found a doc comment on a forward declaration 40 lines from its definition,
  which contradicts the definition's own comment. It also found a "how to add
  a pass" recipe that misses one of the four places it has to name.
- **Lists that should be one list have drifted apart.** `tur_main_inner`
  dispatches on 46 command names. `CANONICAL_COMMANDS[]` drives prefix
  resolution, and `list_external_subcommands`' `builtins[]` hides a `tur-<name>`
  on `$PATH` that a built-in shadows. These three disagree:
  - `builtins[]` is missing `demangle`, `dialects`, `experiments`,
    `image-info`, `image-verify`, `interpret`, `lsp`, `mcp` and `new`. So a
    `tur-lsp` on `$PATH` is listed under "External commands" even though
    `tur lsp` never reaches it, which is the misleading listing the function's
    comment says it prevents.
  - `jit`, `completion` and `emit-rt-split` are dispatched but are not in
    `CANONICAL_COMMANDS[]`. That may be deliberate for `emit-rt-split`, but
    nothing says so.
- **The `#ifdef` guard has already broken builds.** `jit_try_split_preamble`
  sits in the middle of the JIT code but must build without the JIT. Leaving it
  inside the guard broke every `-DTUR_JIT=OFF` build once (`main.c:5664`).
- **The file has been split before, piece by piece.**
  - `src/cli/` (completion, demangle, lsp-lite) moved out;
  - `stdlib_autoload.c` moved out;
  - `src/turi/preload.c` moved out;
  - the interpreter natives moved out.

  What is left is the part that is hard to split, because it shares state.
  This plan is how to finish the job.

## Non-goals

- **No behaviour change and no CLI change**, except in Phase 4, which says
  exactly what it changes.
- **Not splitting the big command functions themselves** (`cmd_build_multi_files`,
  `cmd_run`, `cmd_build_once`, `cmd_eval_h`). That is follow-up work, and it
  is easier once each one sits in its own file.
- **Nothing moves into `tur_core`.** See constraint 1 below.
- **`src/runtime/region.h` is not split.** It is 188 lines. Its cost is being
  pasted verbatim into emitted programs, and the decided fix for that is
  stripping comments at embed time
  ([stale-comments-regions](stale-comments-regions.md), item 13).

## Constraints to know before touching anything

1. **The new files belong to the `tur` executable, not `tur_core`.**
   `tur_core` is consumed by eighteen targets, including `libturi_wasm` and
   the unit tests. `src/cli/*.c` is compiled into it (`src/CMakeLists.txt:354`).
   Driver code calls `system()`, `fork()`, the cc toolchain and the JIT engine.
   Putting it in `tur_core` would drag all of that into the WASM build, the
   same failure `tur_jit_obj` was created to avoid. So the new directory is
   `src/driver/`, listed in `add_executable(tur main.c lsp/mcp.c ...)` through
   a `TUR_DRIVER_SOURCES` variable. Target-wide definitions (`TUR_VERSION`,
   `TUR_HAVE_JIT`, `TURI_HAVE_EDITLINE`) then reach the new files with no
   extra work.
2. **`tur` exports its symbols.** Under `TUR_JIT`, `tur` links with
   `ENABLE_EXPORTS`, and with `--export-all-symbols` on MinGW. That is so the
   JIT's `dlsym(RTLD_DEFAULT)` resolver can find the runtime. A function that
   loses `static` therefore becomes visible to JIT-compiled programs. Every
   non-static `tur_core` function is already visible, so this adds no new
   *kind* of risk. But generic names (`usage`, `cmd_run`, `read_entire_file`,
   `basename_of`, `ensure_dir`, `is_directory`) are names that user inline C
   could plausibly reference. **Rule:** a helper stays `static` in its new
   file unless another file needs it. A cross-file helper with a generic name
   gets a `drv_` prefix in Phase 1. Command entry points can keep their
   `cmd_*` names, since they are unlikely in user code.
3. **Two functions are already called from outside `main.c`.**
   - `tur_check_only` is declared ad hoc with `extern` in `lsp/lsp.c:50` and
     `lsp/mcp.c:97`.
   - `tur_collect_symbols` is declared in `lsp/lsp_sym.h`, and again ad hoc in
     `mcp.c`.

   `web/wasm_lsp.c` defines its own copies for the WASM build, and
   `tests/fuzz/fuzz_link_stub.c` stubs `tur_collect_symbols`. Keep both names
   and signatures. Move them together into `pipeline.c`, and declare
   `tur_check_only` in `lsp_sym.h` beside its sibling so the ad hoc `extern`s
   go away.
4. **Snapshot invariant.** Emitted C does not depend on which file the driver
   code lives in. **No `tests/fixtures/*/expected.c` may change in any PR of
   this series.** A snapshot diff means behaviour changed, and that PR is not
   a move.
5. **Move-only PRs.** Phases 2 and 3 move code and nothing else: no renames,
   no edits. The only allowed changes are dropping `static` where a
   declaration now lives in a header, and adding `#include`s. Reviewers read
   the diff with `git diff --color-moved=dimmed-zebra`. History stays
   traceable with `git blame -C -C`, which follows code across files. Any real
   change, even a typo fix, goes in its own PR.

## Target layout

Sizes come from the current function map. The final membership of each file
is decided in the PR that creates it.

| File | ~Lines | Contents |
|---|---|---|
| `main.c` | 1,700, then about 300 after Phase 4 | `main`, `tur_main_job` (big-stack trampoline), `__asan_default_options`, the dispatcher |
| `driver/cli.c` | 960 | usage text, `resolve_command` and the command lists, global-flag parsers (`parse_no_color` ... `parse_lint_panic`, `parse_timings_flag`), include-flag parsing, `-X` deprecation, external `tur-<name>` lookup |
| `driver/paths.c` | 540 | `read_entire_file*`, `dir_of_path`, `get_exe_path`, stdlib-root resolution, `detect_and_adjust_lang`, `cc_system`, `decode_exit_status`, `json_escape` |
| `driver/pipeline.c` | 1,260 | `core_passes[]`, `pass_zone`, `run_core_pass(es)`, the stdlib prepend / project prelude, `compile_to_c/h/implementation`, `tur_check_only`, `tur_collect_symbols`, the split appliers (`prelude_split_applies`, `resolve_rcgc_from_archive`, `preamble_split_auto_applies`) |
| `driver/spice.c` | 800 | manifest reader macros, experiments from manifest and user config, spice-root and include-dir discovery, `collect_tur_files*`, `collect_dep_dirs_recursive`, `collect_spice_aux_c` |
| `driver/cc.c` | 1,190 | `hoist_tur_include_directives`, autolink markers and flags, `locate_runtime_lib`, `apply_runtime_lib_mode`, `resolve_autolink_flags`, `link_command_run`, the stable temp-path helpers |
| `driver/preamble_split.c` | 940 | prelude split objects and their cache, the r7rs split-lib cache, `jit_try_split_preamble` and `jit_split_reject`, `cmd_emit_rt_split`. These are used **without** the JIT, which is why they are not in `jit.c` |
| `driver/cmd_emit.c` | 460 | `cmd_emit_c`, `cmd_emit_c_to_dir`, `cmd_emit_h`, `resolve_build_dir`, `default_output_name` |
| `driver/cmd_build.c` | 520 | `cmd_build`, `cmd_build_once` |
| `driver/project.c` | 1,300 | `cmd_build_multi(_files)`, `cmd_build_project`, module-name derivation, `^used` module collection |
| `driver/compile_link.c` | 300 | `cmd_compile`, `cmd_link`, the `.link` sidecar |
| `driver/cmd_run.c` | 800 | `cmd_run`, `cmd_test`, `cmd_check_dir` |
| `driver/jit.c` | 980 | `cmd_jit`, the SDK include probes, the REPL JIT and inline-C JIT builds, `run_delegate_engine`. **Wholly** inside `#ifdef TUR_HAVE_JIT`, with the non-JIT stubs beside it |
| `driver/cmd_format.c` | 780 | `cmd_format`, `cmd_parse_check`, `cmd_fmt` and the directory walk |
| `driver/cmd_eval.c` | 690 | `cmd_eval_h`, `cmd_eval_expr`, `cmd_repl`, `cmd_dap`, `cmd_trace(_dump)` |
| `driver/worker.c` | 620 | `cmd_worker` and the `wk_*` fixture evaluator |
| `driver/cmd_misc.c` | 970 | `image-info` / `image-verify`, `doc` / `docs` (with the docs server), `smt`, `experiments`, `dialects`, `explain`. Split further if it grows |

### Headers, and keeping them honest

Under this layout, 135 of the 259 functions are called from another file.
Most of those are command entry points and usage functions, which only the
dispatcher calls. So use two headers, not one:

- **`driver/driver.h`:** the shared helpers (about 60 functions from `paths`,
  `pipeline`, `spice`, `cc`, `preamble_split`) and the shared state (the
  globals, each declared `extern` and defined in the file that owns its
  logic).
- **`driver/commands.h`:** the `cmd_*` / `usage_*` entry points the
  dispatcher calls.

Both are sectioned by defining file. **Add a check that each section's
prototypes really are defined in that file.** It is about 30 lines of Python,
wired into ctest and into `tests/check-docs-lint.sh`'s neighbour list. The
stale-comments audit found that `emit_internal.h`, which claims the same
grouping, has about 133 of 239 prototypes in the wrong section
([stale-comments-emitter](../reported/stale-comments-emitter.md), item 10). A
header this plan creates should not start down that road. The same check can
then be pointed at `emit_internal.h`.

## Phases

### Phase 0: tidy in place (no moves)

These are small, independent changes. Each one makes the later moves cleaner.

- Land the `main.c` items of
  [stale-comments-build-driver](stale-comments-build-driver.md):
  - the comment fixes (items 1-3, 7, 8, 10, 11);
  - `begin_compile_unit()` for the three identical reset sequences (item 12);
  - deleting the two dead `needs_hamt` fields (item 9).

  Moved code should arrive already correct.
- Gather the 26 forward declarations into one block after the includes. Move
  the mid-file `#include`s (`runtime/hamt.h`, `runtime/image.h` at `:10917`,
  the socket headers at `:12206`) to the top, keeping their `#ifdef`s.
- Delete the orphan banner "Native HAMT wrappers for the interpreter (Tier 3)"
  (`:10912`). Its code moved out long ago; the image commands sit under it.
- Make `list_external_subcommands` filter on `CANONICAL_COMMANDS[]` plus
  `COMMAND_ALIASES[]`, and delete `builtins[]`. This fixes the shadowed
  `tur-<name>` listing above, and it can land at any time. Separately, either
  add `jit` and `completion` to `CANONICAL_COMMANDS[]` or comment why they are
  hidden.
- Wire `src/driver/` into CMake (`TUR_DRIVER_SOURCES`, initially empty) with
  empty `driver.h` and `commands.h`, plus the section check.

### Phase 1: expose the core, still in `main.c`

This is one PR. Drop `static` from the functions that will cross file lines
(the measured set of 135, minus those that end up sharing a file with all
their callers), and declare them in `driver.h` / `commands.h`. Apply the
`drv_` prefix from constraint 2 to the generic names. **This is the series'
only rename.** It is mechanical (`sed` plus a build). After it, every move in
Phases 2 and 3 is a pure cut-and-paste.

### Phase 2: move the leaves

Move the files that touch few of the shared globals first. One PR per file,
or two small ones together:

| Order | File | Shared globals it touches |
|---|---|---|
| 1 | `cmd_format.c` | 0 |
| 2 | `cmd_eval.c` + `worker.c` | 0 |
| 3 | `cmd_misc.c` | 0 (it uses the global-flag parsers through `driver.h`) |
| 4 | `compile_link.c` | 0 |
| 5 | `cmd_run.c` | 2 (`g_cc_release`, `g_build_bundle_deps`) |
| 6 | `jit.c` | 3 (`g_no_auto_spice`, `g_emit_for_link`, `g_manifest_sink`) |

The `jit.c` PR also finishes the guard cleanup. Once `jit_try_split_preamble`
and `cmd_emit_rt_split` live in `preamble_split.c`, everything in `jit.c` is
JIT-only. Its `#ifdef` can then wrap the whole file, with the two
"reconfigure with `-DTUR_JIT=ON`" stubs, which are now inside `cmd_jit` and
`run_delegate_engine`, as the `#else` branch. Build it with `TUR_JIT` both ON
and OFF.

### Phase 3: move the core

`paths.c`, then `spice.c`, `pipeline.c`, `cc.c`, `preamble_split.c`,
`cmd_emit.c`, `cmd_build.c` and `project.c`. Each global moves with the file
that owns its logic, and is declared `extern` in `driver.h`:

- `g_stdlib_root*` and `g_argv0` go to `paths.c`;
- the split sinks and caches go to `preamble_split.c` / `pipeline.c`;
- `g_runtime_mode`, `g_emit_for_link` and `g_cc_release` go to `cc.c`;
- the `--no-auto-*` flags go to `cli.c`, which parses them.

`cc.c` and `pipeline.c` touch the most shared state (12 and about 20
globals), so they go last. By then everything they talk to is already
declared.

### Phase 4: one command table and one flag table (behaviour-sensitive)

This is the only phase that changes the shape of the code, not just where it
lives. What remains in `tur_main_inner` is about 113 `strcmp(argv[i], ...)`
flag tests and 46 `strcmp(cmd, ...)` dispatch arms, plus 13 `parse_*` helpers
that each rescan `argv`.

- **A command table** (name, aliases, handler, usage function, hidden flag)
  that drives dispatch, `resolve_command`, the external-command filter, the
  usage listing and, where possible, `src/cli/completion.c`'s lists. The
  three disagreeing lists described under "Why" become one.
- **A global-flag table** (spelling, target variable or handler, whether it
  takes an argument, whether it is stripped from `argv`). It replaces the
  separate `parse_*` scans and the stripping loop. Precedence and stripping
  semantics must be kept exactly. Today some flags are read anywhere in
  `argv` by their scanner and are *also* stripped by the loop, and the table
  must reproduce that.

**Before starting Phase 4,** add golden tests in `tests/run-flags.sh` for
`tur --help`, every `tur <cmd> --help`, the "External commands" listing (with
a fake `tur-foo` on `PATH`), and `tur completion zsh|bash`. Today only "exit 0
and contains `usage:`" is checked, so a regression in the help text would go
unnoticed. Any output the golden tests catch changing is then a decision the
PR states, not an accident.

### Phase 5: docs, carried along, not batched

Each move PR updates the citations of the functions it moved:

- **Guides:** 79 `main.c` references across 19 guides. The source map in
  [build-driver-internals-guide](../guides/build-driver-internals-guide.md)
  ("`src/main.c`: everything below unless noted") gets rewritten when Phase 3
  ends.
- **Source comments:** 102 `main.c` references in 46 files under `src/`.
- **Tools:** `tools/check_turi_native_parity.py` defines an unused `MAIN_C`;
  `tests/fuzz/fuzz_link_stub.c`'s comment names `main.c`.

Cite functions, not paths, where the guide's own maintenance rule asks for
that. Then the next move costs nothing.

## Every PR in the series

- `bash tests/run.sh` with the 12-minute timeout. The summary should match the
  pre-PR baseline, with zero `expected.c` changes (constraint 4).
- ctest, including `tests/run-flags.sh` and `tests/run-turi.sh`.
- Debug (ASan) and Release builds, with `-DTUR_JIT=ON` and `OFF`. CI covers
  Windows (MSYS2) and macOS.
- For move-only PRs, review with `git diff --color-moved` and confirm nothing
  is highlighted as changed.
- No other change bundled in.

## Coordination

A move PR conflicts with any open branch that edits the code it moves. Land
moves one file at a time, at a quiet moment, and say so in the PR so open
branches can merge `main` promptly. Open plans whose next steps touch
`main.c`:

- [cc-path-preamble-split-plan](../upcoming/cc-path-preamble-split-plan.md): the macOS
  flip, in `preamble_split_auto_applies`;
- [profiling-plan](../upcoming/profiling-plan.md);
- [security-audit-plan](../upcoming/security-audit-plan.md);
- [aot-compiled-repl-plan](../upcoming/aot-compiled-repl-plan.md): REPL JIT;
- [wasm-spices-plan](../upcoming/wasm-spices-plan.md);
- [ffi-spices-integration-plan](../upcoming/ffi-spices-integration-plan.md);
- [windows-remaining-plan](../upcoming/windows-remaining-plan.md).

Prefer moving a region right after such a plan's step lands, not while one is
in flight.

## Risks

- **Merge conflicts.** Mitigated by the move-only discipline: a conflict in a
  pure move resolves by re-applying the other branch's hunk in the new file.
- **Hidden same-TU assumptions**, such as a static helper defined twice under
  different `#ifdef`s, or a function relying on file-scope initialisation
  order. The compiler finds these at the move, not at runtime.
- **Exported-symbol growth** (constraint 2). Bounded by keeping helpers
  `static` by default.
- **Phase 4 changes the CLI's internals.** Gated by the golden tests added
  first.

## Done when

- `main.c` holds `main`, the trampoline, the ASan options hook and a
  table-driven dispatcher, at about 300 lines.
- No file in `src/driver/` is over about 1,300 lines. No function is over
  about 700 lines (`cmd_build_multi_files` is the follow-up).
- There is one command list, and the help, completion and external-command
  output is pinned by golden tests.
- `driver.h` and `commands.h` pass their section check.
- The build-driver guide's source map describes `src/driver/`.
- Then move this plan to `docs/archive/`.
