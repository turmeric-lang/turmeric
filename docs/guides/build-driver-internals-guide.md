---
title: The Build Driver
category: Compiler Internals
description: How the `tur` driver turns a command line into a binary -- command dispatch and the global-flag parsers, the compile entry points and the real pass schedule, how `cc` is chosen and invoked, `tur run` and project mode, every cache and what invalidates it, the build-directory ladder, and the driver's environment variables
tags:
  - build-system
  - caching
  - compiler-flags
---

# The Build Driver (internals)

This guide is for contributors working on the `tur` driver -- `src/main.c`
and `src/driver/` -- and the small helpers in `src/cli/`. It follows one command from `argv` to a
linked executable and names the function that does each step. It describes
the code as it stands.

The pipeline itself (reader, elaborator, emitter) has its own guides; start
from the [compiler internals map](compiler-internals.md). The user-facing
side of the commands covered here is in
[tur compile / tur link](tur-compile-link-guide.md),
[compiler flags](compiler-flags-guide.md),
[the JIT](jit-guide.md) and
[developing spices](developing-spices-guide.md).

> **Trust the code over the comments.** The driver is large and many of its
> comments predate a rework of the thing they describe. Where a comment and
> this guide disagree, read the code. If the code disagrees with this guide,
> fix the guide.

---

## The short version

1. `main` moves the whole driver onto a large stack
   (`tur_run_on_big_stack`) and calls `tur_main_inner`.
2. `tur_main_inner` reads a few environment seams, then parses **global
   flags** out of `argv` into `g_*` globals and strips them (the flag table,
   `GLOBAL_FLAGS[]` in `driver/cli.c`).
3. `drv_find_command` looks `argv[1]` up in the command table (`COMMANDS[]`,
   same file: an exact spelling, an alias, or a unique prefix) and the
   dispatcher runs that row's handler. An unknown name is exec'd as
   `tur-<name>` from `$PATH`.
4. Per-file commands widen the include path from the enclosing `build.tur`
   (`auto_append_spice_includes`) and load its reader macros and
   experiments (`discover_manifest_reader_macros`).
5. `compile_to_c` (whole program, one C unit) or
   `compile_to_h`/`compile_to_implementation` (separate compilation) read
   the source, prepend the stdlib, run `run_core_passes`, and emit C.
6. `cmd_build_once` writes the C to `$TMPDIR/tur-build/<name>.c`, decides
   how the runtime is linked, and runs **one** `cc` command that compiles
   and links (`drv_link_command_run`).
7. `tur run <file>` is `cmd_build` into a temp executable, then `system()`
   on it, then `unlink`.

---

## Source map

| File | Role |
|---|---|
| `src/main.c` | `main`, the big-stack trampoline, the ASan options hook, and `tur_main_inner`: startup and the table-driven dispatcher |
| `src/driver/driver.h`, `commands.h` | What the driver files share: helpers and state, and the `cmd_*` / `usage_*` entry points. Sectioned by defining file; `tests/check-driver-headers.py` checks each section |
| `src/driver/cli.c` | The command table, the global-flag table and its parser, `--timings`, the environment switches, `tur --help` and the per-command usage text, `-I` parsing, the external-command fallback |
| `src/driver/paths.c` | Reading files, the executable's path, the stdlib root, `#lang` detection, `cc_system` |
| `src/driver/spice.c` | Manifest and spice discovery: reader macros, experiments, the `:spices` include walk, `.tur` collection, engine selection |
| `src/driver/pipeline.c` | `core_passes[]` and the pass runner, the three compile entry points, the stdlib and project prelude, the split switches, `tur_check_only` / `tur_collect_symbols` |
| `src/driver/cc.c` | `#include` hoisting, autolink markers and flags, the runtime library and its mode, `drv_link_command_run`, the stable temp paths |
| `src/driver/preamble_split.c` | The prelude pieces and their cache, the r7rs library-unit cache, the preamble swap (`jit_try_split_preamble`), `tur emit-rt-split` |
| `src/driver/cmd_emit.c` | `emit-c`, `emit-h`, `check`, `expand`, `audit-spans`; the build-directory ladder |
| `src/driver/cmd_build.c` | `tur build <file>` (`cmd_build`, `cmd_build_once`) |
| `src/driver/project.c` | `tur build <dir>`: `cmd_build_project`, `cmd_build_multi`, separate compilation |
| `src/driver/compile_link.c` | `tur compile`, `tur link`, the `.link` sidecar |
| `src/driver/cmd_run.c` | `tur run <file>`, `tur test`, `tur check <dir>`, engine delegation |
| `src/driver/jit.c` | `tur jit` and the REPL's JIT builds -- the whole file is `#ifdef TUR_HAVE_JIT` |
| `src/driver/cmd_eval.c` | The interpreter commands: `interpret`, `debug`, `eval`, `repl`, `dap`, `trace` |
| `src/driver/cmd_format.c` | `format`, `fmt`, `parse-check` |
| `src/driver/worker.c` | `tur worker` |
| `src/driver/cmd_misc.c` | `doc`, `docs`, `image-info` / `image-verify`, `smt`, `experiments`, `dialects`, `explain`, `lsp`, `mcp`, `lsp-lite` |
| `src/runtime/pass.h` | `PassKind`, `PassContext` |
| `src/runtime/globals.c`, `globals.h` | Most `g_*` option globals the passes and emitter read |
| `src/runtime/experiments.c` | `EXPERIMENTS[]`, `enable_experiment_list` (see the [experimental flags guide](experimental-flags-guide.md)) |
| `src/compiler/spice_search.c` | `spice_find_root`, `spice_search_append` -- the walk-up behind auto-spice discovery |
| `src/compiler/stdlib_autoload.c` | `tur_stdlib_prepend_forms`, the stdlib auto-load list |
| `src/compiler/stack_guard.c` | `tur_run_on_big_stack` (`TUR_STACK_MB`) |
| `src/compiler/prof.c` | `--timings` zones (`prof_enter`/`prof_exit`) |
| `src/compiler/justrun.c` | `tur run <recipe>`, the Justfile task runner (see the [tur run guide](tur-run-guide.md)) |
| `src/compiler/pkg.c` | Manifest reading, fetch, cmake deps (see the [package manager internals](package-manager-internals-guide.md)) |
| `src/turi/spice_loader.c` | The REPL's `.tur-repl-cache/` |
| `src/platform_fs.h` | `tur_temp_dir` |
| `src/cli/completion.c` | `tur completion zsh\|bash` -- embedded scripts; the command lists come from the command table |
| `src/cli/demangle.c`, `lsp_lite.c` | `tur demangle`, `tur lsp-lite` |

---

## 1. Startup and command dispatch

### Before any flag is read

`main` installs a crash reporter on Windows, then runs `tur_main_inner` on a
stack sized by `TUR_STACK_MB` (default 256 MiB, `stack_guard.h`). Every AST
walk recurses once per nesting level of the *input*, so the stack is sized
once for the whole driver rather than rationed per phase.

`tur_main_inner` then, in order:

1. `drv_parse_timings_flag` strips `--timings[=path]` and starts the
   profiler, before anything it should measure and before `g_main_argv` is
   taken (the split build's cache key hashes the command line).
2. `drv_apply_env_overrides` reads the representation and memory seams
   (`TUR_ADT_SLAB`, `TUR_SR1_SUM_BYVALUE`, `TUR_SR2_APP_SUM_BYVALUE`,
   `TUR_OPTION_NICHE`, `TUR_REGIONS`), the interpreter's inline-C JIT switch
   (`TUR_REPL_JIT_INLINE_C`) and `TUR_RUNTIME`. See
   [section 8](#8-environment-variables).
3. Records `g_argv0`, `g_main_argc`, `g_main_argv`, and calls
   `resolve_stdlib_root`, which `setenv`s `TUR_STDLIB_DIR` (without
   overwriting) so every later subsystem sees one stdlib root.
4. On JIT builds, installs the c2mir FFI provider and the REPL inline-C hook.

### Global flags: one table

`GLOBAL_FLAGS[]` in `driver/cli.c` lists every global flag: its spelling
(and a second one, as for `-Werror=` / `--Werror=`), the global it sets or
the action it runs, and how it is read. `drv_parse_global_flags` reads the
table in two passes, as the hand-written scanners and strip loop it
replaced did -- and the difference is observable:

1. **Scan.** Every `GF_SCAN` row (`--no-color`, `--panic-abort`,
   `--warn-unused-result` and its `--no-` twin, `--lint-panic`,
   `--no-contracts`, `--debug`, the three `-Werror=`s, `--no-auto-spice`,
   `--no-abi-cache`, `--no-r7rs-gc`, `--no-saffron-gc`) is applied wherever it
   appears, before anything is removed -- so `--debug` counts even as the word
   after `--explain`. The environment's half of three of them
   (`TUR_NO_ABI_CACHE`, `TUR_R7RS_GC=0`, `TUR_SAFFRON_GC=0`) is read first.
2. **Strip.** Left to right, each word is matched against the table in
   table order; a row that is not a scan row is applied (`--dump-*`,
   `--strict-*`, `--lint-*`, `--json`, `--explain`, `--enable=`,
   `--macro-caps=`, `-X<name>`, ...), and every matched row is removed from
   `argv` except a `GF_KEEP` one. A row whose action fails (an unknown
   `--enable=` name, `--macro-caps=` other than `io`,
   `--allow-experimental`) stops the parse; the dispatcher exits with its
   status. A `GF_VALUE` row (`--explain`, `--backtrack-depth N`) with no word
   after it is not that flag: it stays in `argv` for the subcommand to reject.

Per-command flags (`-I`, `-o`, `--build-dir`/`-B`, `--runtime=`, `--shared`,
`--engine`, ...) are parsed by each command's `cmd_<name>_main`.

Because of the strip pass, a global flag is accepted before **or** after the
subcommand: `tur --strict-effects check f.tur` and
`tur check f.tur --strict-effects` are the same. Two details:

- **Both passes stop at `--`.** `argc` is cut at the first `--` while they
  run, and the tail (the `--` included) is put back afterwards, so
  `tur run f.tur -- a --strict-effects b` compiles with the default flags and
  hands the program all three words. Until 2026-10-17 neither stopped there
  ([global-flags-parsed-after-double-dash](https://github.com/turmeric-lang/turmeric/blob/main/docs/archive/global-flags-parsed-after-double-dash.md)).
- **`--no-color` is read and stripped** like the others. It used to stay in
  `argv`, so a trailing one was an unknown flag to the subcommand (exit 2) and
  a leading one was taken for the subcommand (exit 64)
  ([no-color-flag-rejected-everywhere](https://github.com/turmeric-lang/turmeric/blob/main/docs/archive/no-color-flag-rejected-everywhere.md)).

`--no-abi-cache` is the one `GF_KEEP` row: read, but left in `argv`, so a
subcommand parser that rejects unknown dashes has to skip it by name. The
`build`, `compile` and `emit-c --output-dir` handlers do; the single-file
`emit-c`, `check` and `expand` ones do not, so `tur emit-c f.tur
--no-abi-cache` is a usage error. (The same handlers still skip
`--no-auto-spice`, `--no-r7rs-gc` and `--no-saffron-gc` by name, which is
dead code now that the strip pass removes them.)

After the parse: `diag_init` (color off under `--json`/`--json-diagnostics`
or a non-tty stderr), then `--explain`, `--version`/`-V`/`-v`, and
`--help`/`-h`, which are handled before command resolution.
`tests/run-flags.sh` pins the stripping and the errors in its goldens
(`tests/flags-golden/flags-*.txt`).

### The worker's flag-string parser

`wk_apply_flags` is a **second, independent** flag parser. It reads a
fixture's `flags` file as a space-separated string and sets the same
globals, for `tur worker` (a persistent fixture evaluator that reads fixture
directories from stdin and forks per fixture). Its list has drifted from
`GLOBAL_FLAGS[]` in both directions:

- missing: `--no-contracts`, `--keep-contracts`, `--panic-abort`, `--debug`,
  `--lint-panic`, `--warn-unused-result`, `--dump-kinds`,
  `--dump-clone-plan`, `--cps-path`, `--backtrack-depth`;
- extra: `--lint-unsafe-doc`, `--lint-unsafe-nested`, which the table
  does not know.

No script under `tests/` starts `tur worker` today (`tests/run.sh` does
not use it), so the drift is latent. If you
revive the worker, reconcile the two lists first.

### Resolving and dispatching the subcommand

`COMMANDS[]` in `driver/cli.c` is the one list of subcommands: each row is
the name, its handler, its flags, its line in the completion scripts and its
lines in `tur --help`. It drives the dispatcher, prefix resolution, the
`tur --help` listing (`drv_usage`, in table order), the filter that keeps a
`tur-<name>` a built-in shadows out of the "external commands" listing, and
the command lists of both completion scripts. Until main-c-split-plan there
were three lists, and they disagreed.

`drv_find_command` maps `argv[1]` to a row:

1. An exact spelling of a command or an alias (`COMMAND_ALIASES[]`: `just`
   -> `run`, and `--interpret` -> `interpret`) wins.
2. Otherwise a unique prefix of a command or alias resolves (`tur bui` ->
   `build`); two or more hits print the usage and exit 64. A token that
   starts with `-` is never prefix-resolved.
3. No hit returns NULL.

`jit`, `emit-rt-split` and `completion` are `DRV_CMD_EXACT` rows: they
dispatch on their exact spelling only, because listing them for prefix
resolution would make `tur comp` and `tur j` ambiguous. A token no row
matches falls to `drv_try_external_subcommand`, which `execv`s the first
executable `tur-<name>` on `$PATH` with the remaining arguments (on Windows,
`_spawnv` and wait).

A handler takes the whole `argc`/`argv`, global flags already stripped:
`argv[1]` is the command word as typed and its arguments start at
`argv[2]`. Most are a `cmd_<name>_main` in the file for their area; a few
rows point straight at a `cmd_*` that already took `(argc, argv)`.

The main commands and their handlers:

| Command | Handler | Path through the compiler |
|---|---|---|
| `emit-c <file>` | `cmd_emit_c` | `compile_to_c`, C to stdout |
| `emit-c -B <dir> <files...>` | `cmd_emit_c_to_dir` | per module `compile_to_h` + `compile_to_implementation` |
| `emit-h <file>` | `cmd_emit_h` | `compile_to_h` |
| `check <file>` / `<dir>` | `cmd_check_main` / `cmd_check_dir` | `compile_to_c`, output discarded |
| `expand <file>` | `cmd_expand_main` | `compile_to_c` with `g_dump_expansion` |
| `audit-spans <file>` | `cmd_audit_spans_main` | `compile_to_c` with `g_audit_spans` (stops after elaboration) |
| `build <file>` | `cmd_build` (or `cmd_compile` + `cmd_link` under `--split-build`) | `compile_to_c` + one `cc` |
| `build <dir>` with `build.tur` | `cmd_build_project` | see [section 4](#4-directory-and-project-builds) |
| `build <dir>` without one | `cmd_build_multi` | separate compilation |
| `compile` / `link` | `cmd_compile` / `cmd_link` | see the [compile/link guide](tur-compile-link-guide.md) |
| `run <file>` or bare `run` in a Justfile-less project | `cmd_run` | `cmd_build` + `system()` |
| `run <recipe>` | `cmd_justrun` | the task runner |
| `jit <file>` | `cmd_jit` | `compile_to_c` + MIR in process (see the [JIT guide](jit-guide.md)) |
| `emit-rt-split [--hash]` | `cmd_emit_rt_split` | prints the canonical split-runtime text (`emit_rt_split_source`) |
| `interpret`, `debug`, `eval`, `repl`, `trace`, `dap` | `cmd_eval_inc`, `cmd_eval_expr`, `cmd_repl`, `cmd_trace`, `cmd_dap` | the `turi` interpreter |
| `test <dir>` | `cmd_test` | `cmd_build` per file + `system()` |
| `lsp`, `mcp` | `lsp_server_run`, `mcp_server_run` | `tur_check_only` (every experiment on) / `tur_collect_symbols`, both `compile_to_c` |
| `lsp-lite` | `lsp_lite_run` (`src/cli/lsp_lite.c`) | docstrings only, no compile |
| `new`, `init`, `add`, `fetch`, `install`, ... | `cmd_pkg_*` | the package manager |

---

## 2. The compile entry points

Three functions turn one `.tur` file into C. They share the front half and
differ in what they prepend and what they emit.

| | `compile_to_c` | `compile_to_h` | `compile_to_implementation` |
|---|---|---|---|
| Used by | `emit-c`, `check`, `expand`, `audit-spans`, `build <file>`, `run`, `jit`, `test`, LSP/MCP | `emit-h`, separate compilation | separate compilation |
| Stdlib prepended | `prepend_stdlib_forms` -> `tur_stdlib_prepend_forms` (honours `--no-auto-stdlib`) | `load_project_prelude` (every auto-load file) | `load_project_prelude` |
| `ctx.separate_compilation` | false | true | true |
| After the passes | `resolve_rcgc_from_archive`, `srfi_prune_program`, optional r7rs split, `emit_program`, optional exports manifest | `emit_header` | `emit_implementation`, optional exports manifest |

`load_project_prelude_impl` loads the full auto-load list
(`tur_stdlib_autoload_files`), the same files `compile_to_c` gets.

**Shared front half**, in order:

1. `drv_read_entire_file`, then `detect_and_adjust_lang` for the `#lang` line.
2. Per-unit resets, in `begin_compile_unit`: `diag_reset`,
   `experiment_reset_warnings`, `pkg_tur_version_reassert`,
   `pkg_manifest_reassert`, `refine_discharge_reset`. These exist because
   `tur test`, `tur check <dir>`, project builds and the LSP loop over files
   in one process; a stale `had_error_` would fail every later file.
3. `diag_register_file`, a fresh `Arena` and `SymbolTable`.
4. Reader macros from the manifest are loaded into a **strict** registry
   (a duplicate `reader-macros/define` is an error here, not in the REPL).
5. `read_all_with_registry`, then the stdlib prepend.
6. `run_core_passes`.

**`tur check` emits C.** Its usage text says "type-check only, no codegen",
but it calls `compile_to_c` with a throwaway buffer, so the whole emitter
runs (about 44% of a `tur --timings check` on a small file). That is also
why an emission-time error -- for example "this effect operation has no
lowering here" -- shows up under `tur check`. `tur_check_only` (the LSP's
diagnostics) and `tur_collect_symbols` do the same, the latter with
`g_logical_entry_path` set so imports resolve from where the editor buffer
lives rather than from its scratch copy.

### The pass schedule

`core_passes[]` lists six passes; `run_core_passes` runs them in order,
charging each to its `--timings` zone (`pass_zone`). `run_core_pass`
returns 0 to continue, 1 to fail, 2 to stop successfully.

| Pass | What it actually runs |
|---|---|
| `PASS_ELABORATE` | `elaborate_program` (macro expansion, typeclasses and refinement discharge happen inside and have their own zones). Then, **outside the list but inside this arm**: `mono_specs_resolve_program` (zone `mono`), `--dump-mono-specs` (flushed), and under `audit-spans` `span_audit_program`, which returns 2 and ends the pipeline |
| `PASS_KIND_CHECK` | `kind_check_pass`; `--dump-kinds` |
| `PASS_EFFECT_ENV` | Builds the `EffectEnv` (builtin `Unsafe`, the capabilities); the tree is untouched. The CPS/DK backend lowers effects |
| `PASS_EFFECT_ROW_INFER` | `effect_check_pass`, the effect-row checker; `--dump-effects` |
| `PASS_CPS` | the `--dump-cps-coloring` (stdout) and `--dump-cps` dumps, `cps_transform`, then `--dump-clone-plan` and the stderr half of `--dump-cps-coloring` |
| `PASS_BORROW_CHECK` | `borrow_check_program`, then `borrow_check_effect_handler_captures`, then `lifetime_check_program` (the TY4 lifetime pass lives here, not in a pass of its own) |

In builds without `NDEBUG`, every arm also asserts `kind_verify_program`,
and that time is charged to the pass.

Work that runs **after** the list, in `compile_to_c` only:

- `resolve_rcgc_from_archive` decides whether the preamble declares or
  defines the rc/GC runtime (see the
  [runtime preamble guide](runtime-preamble-and-linkage-guide.md));
- `srfi_prune_program` drops SRFI definitions nothing reaches;
- the r7rs prelude split (`prelude_split_applies`), which emits up to three
  units -- see [section 7](#7-caches-and-what-invalidates-them);
- `emit_program`, during which the CPS coloring and classification that the
  backend actually uses run (`ensure_S`), not in `PASS_CPS`. See
  [CPS and the DK machine](cps-dk-internals-guide.md#pipeline-position).

The emitter is covered in the
[emitter architecture guide](emitter-architecture-guide.md), the elaborator
in the [elaborator internals guide](elaborator-internals-guide.md), and the
r7rs split units in the
[r7rs implementation guide](r7rs-implementation-guide.md).

---

## 3. Single-file builds: invoking `cc`

`cmd_build` is a retry ladder around `cmd_build_once`:

1. Build with every split allowed.
2. If the split units failed and the library unit came from the cache,
   retry with `g_split_lib_cache_off`.
3. If three units failed, retry as two (`g_import_split_failed`).
4. If it still failed, build as one unit (`allow_split = false`).

`TUR_SHOW_CC=1` prints each step down the ladder. A failure that only
happens in the split shape is therefore slow, never wrong.

`cmd_build_once`, step by step:

1. Sets `g_emit_for_link = true`. This is the one difference between the C
   `tur build` compiles and the C `tur emit-c` prints: `emit-c` keeps a
   self-contained preamble, `build` may declare runtime pieces an archive
   supplies.
2. `collect_used_attr_modules` force-loads `^used` modules from the `-I`
   dirs, so a symbol only C reaches is still emitted.
3. `compile_to_c`, with `g_split_lib_sink`/`g_split_imp_sink` set unless the
   target is wasm or `--runtime=split`.
4. Writes the C to `drv_stable_c_path(input)`: `$TMPDIR/tur-build/` plus the
   input path **as typed**, every byte outside `[A-Za-z0-9-]` turned into
   `_` (`m.tur` -> `m_tur.c`). It is stable so ccache can hit. If the
   directory is not ours (`drv_tmp_dir_is_ours`: a real directory, owned by
   us, not group/other-writable) it falls back to
   `$TMPDIR/tur-build-<uid>/`. The file is opened with `O_NOFOLLOW`.
5. The preamble swap: under `TUR_RT_SPLIT`, or `TUR_RT_AUTO` when
   `preamble_split_auto_applies`, `jit_try_split_preamble` replaces the
   fixed preamble with the committed decls region. It declines on a hash
   mismatch (loudly only under `--runtime=split`).
6. `hoist_tur_include_directives` lifts `__tur_include__` payloads to file
   scope -- directives first, then code -- **after** the swap, or the swap
   would delete them.
7. `scan_autolink_markers` collects `__tur_autolink__` link flags, checking
   every token against the grammar in `src/tur_argcheck.h` (they reach a
   shell).
8. Picks the output: `-o`, else `drv_default_output_name` in the **current
   directory** (`foo.tur` -> `foo`; `.js` appended for wasm). A
   single-file build never consults the build directory.
9. Picks the compiler and flags (below), collects the spice's
   `:c-includes`/`:c-sources` and cmake-dep flags (`drv_collect_build_aux`),
   and for wasm the `:web` emcc flags.
10. `apply_runtime_lib_mode` swaps bare runtime `.c` sources for an archive;
    `resolve_autolink_flags` anchors SDK and tree-relative paths and probes
    for a sanitized `libturi.a`.
11. Under the r7rs split, `prelude_split_object` compiles (or reuses) the
    library and imported-library objects.
12. `drv_link_command_run` assembles and runs the one `cc` command.
13. Leaves the stable `.c` in place for ccache; only a random fallback temp
    is unlinked.

### The `cc` command

`drv_link_command_run` builds:

```
$CC <cc_flags> -Wno-misleading-indentation -fwrapv -o '<out>' '<tmp>.c'
    [lib/imp objects] <:c-includes> <:c-sources> <autolink>
    [-fsanitize=address,undefined] <cmake flags> -I'<dir>'... -lm
    [Windows: -lpthread -lws2_32 -lshlwapi]
```

and runs it through `cc_system` (`system()` wrapped in the `cc` timing
zone). With `TUR_SHOW_CC=1`, the command is printed first:

```
CC: cc -O2 -std=c99 -Wall -fno-strict-aliasing -Wno-misleading-indentation
    -fwrapv -o 'h1' '/tmp/tur-build/hello_tur-<hash>.c' .../src/runtime/hamt.c ... -lm
```

| Piece | Where it comes from |
|---|---|
| `$CC` | `getenv("CC")`, default `cc`; always `emcc` for `--target wasm` |
| `cc_flags` | `TUR_CC_FLAGS` replaces the default **entirely**. Defaults: `-O2 -std=c99 -Wall -fno-strict-aliasing`; under `--debug` `-g -Og ...` (not `-O0`: the build relies on dead-code elimination to drop preloaded stdlib functions that reference libturi-only symbols); wasm adds `-s WASM=1` |
| `TUR_EMITTED_C_CC_FLAGS` | `-Wno-misleading-indentation -fwrapv`, appended **after** `cc_flags` so an override cannot drop them. `-fwrapv` is semantics: Turmeric `int` arithmetic wraps |
| `-fsanitize=address,undefined` | added only when `resolve_autolink_flags` finds `__asan_init` in a `libturi.a` on a `-L` path (`nm` probe). A sanitized `tur` does **not** sanitize the programs it builds |
| runtime archive / sources | `apply_runtime_lib_mode`; full detail in the [runtime preamble and linkage guide](runtime-preamble-and-linkage-guide.md) and the [compile/link guide](tur-compile-link-guide.md#tur-build---runtime----how-the-runtime-gets-linked) |

`--runtime=` (or `TUR_RUNTIME`) sets `g_runtime_mode`: `auto` (default),
`lib`, `source`, `split`. The preamble swap is skipped by `auto` when
`--debug` is on, when `TUR_CC_FLAGS` contains `-fsanitize` (the archive is
not sanitized), when there is no `libturt_runtime.a` + `libturt_preamble.a`
pair, and on macOS unless `TUR_PREAMBLE_SPLIT=1`.

To see exactly what was compiled, set `TUR_SHOW_CC=1` and open the `.c` it
names: that file is post-swap and post-hoist, unlike `tur emit-c` output.

---

## 4. Directory and project builds

`tur build <dir>` takes one of two paths:

- **`<dir>/build.tur` exists** -> `cmd_build_project`.
- **Otherwise** -> `cmd_build_multi`, which collects every `.tur` under
  the directory, recursively (`collect_tur_files_deep`), and calls
  `cmd_build_multi_files`.

`cmd_build_project`, in order:

1. `realpath`s the root, collects `src/**.tur`
   (`collect_project_src_files`) and sorts them.
2. Validates the manifest (an invalid one fails the build), verifies locked
   spices (`pkg_verify_locked_spices`), and builds the transitive
   `:cmake-deps` closure unless `cmake/<arm manifest>` already exists.
3. **Library autodetect:** if no file defines `main`, the build is
   `--shared`.
4. Checks every `:exports` entry has a source file.
5. Builds the include path: user `-I` first, then
   `resolve_project_include_dirs` (own `src/` plus each `:spices` dep's).
6. **Single-main fast path:** an executable with exactly one `main` and no
   non-entry `^used` module is built as a **single file**:
   `cmd_build(entry, ...)`, output `<build-dir>/bin/<:name>`. This is the
   whole-program route, carrying the full runtime in one unit.
7. Otherwise, separate compilation: dep modules are added to the build set
   (compiled for headers; linked only for executables or `--bundle-deps`),
   and `cmd_build_multi_files` runs.

`cmd_build_multi_files` is two-pass (J6):

1. **Pass 1:** `compile_to_h` + `compile_to_implementation` per module,
   collecting `BorrowSpecInfo` clones.
2. Forced spec lists per owning module.
3. **Pass 2:** recompile owners that have forced specs.
4. Writes `_main.c` (executables only), the runtime owner TU
   `tur_runtime.{c,h}`, and links everything with one `cc`. Its flags are
   `TUR_CC_FLAGS` or `-O2 -std=c99 -Wall -fno-strict-aliasing`, plus
   `TUR_EMITTED_C_CC_FLAGS`; `--shared` adds `-fPIC -shared` (`-shared`
   alone on Windows). This path does not switch to `-g -Og` under
   `--debug`.

Separate compilation itself is covered in the
[separate compilation internals guide](separate-compilation-internals-guide.md).

---

## 5. `tur run` and per-file discovery

### Picking the mode

The `run` arm scans its arguments up to `--`:

- a `.tur`, `.tur.sweet` or `.scm` path, `-`, or any of `--release`,
  `--offline`, `--engine`, `-I` -> `cmd_run` (the classic path);
- a bare `tur run` with no Justfile but an enclosing `build.tur` ->
  `cmd_run` in project mode;
- anything else -> `cmd_justrun`, the task runner.

### `cmd_run`

1. `-I` is collected only up to `--`; everything after `--` is the
   program's `argv`. `--release` sets `g_cc_release`, which makes the cc
   step default to `-O3` instead of `-O2` (`tur build --release` sets it
   too); `TUR_CC_FLAGS` still replaces the defaults, and a non-`cc` engine
   has no cc step for it to change.
2. **Single-file mode** (`<file>` given): auto-spice includes, then
   `discover_manifest_reader_macros`, then `drv_resolve_engine`. A non-`cc`
   engine goes to `run_delegate_engine` (`cmd_jit` or the interpreter).
   `-` reads stdin into `$TMPDIR/tur-stdin-XXXXXX.tur` first.
3. **Project mode:** `drv_find_project_root` from the cwd; fetch or verify
   `:spices` (`--offline` only checks they exist); build the include path
   from the manifest; build the transitive cmake deps; pick the entry
   (`:entry`, else `src/main.tur`, else the only file in `src/`).
4. `RUN_ENTRY`: `mkstemp` `$TMPDIR/tur-run-XXXXXX`, `cmd_build` into it,
   run it with `system()` (arguments quoted with `drv_buf_put_quoted`),
   `unlink` it, and return `decode_exit_status` of the child.

The engine ladder (`drv_resolve_engine`) is `--engine` > `TUR_ENGINE` >
`build.tur :engine` > `cc`. An unknown CLI or environment value is
TUR-E0311, never a fallback.

### Why stdout must be flushed

The compile runs **in this process** and the program runs in a child that
shares stdout. Anything the compiler printed to a buffered stdout -- a
`--dump-*` report -- is still in the parent's buffer when the child writes.
On a terminal stdout is line-buffered, so the order comes out right.
Redirected, it is fully buffered: the child's output lands at whatever
buffer boundary the parent's writes had reached, and the rest of the dump
followed when the parent exited: `tur --dump-effects run f.tur > out` put the
program's `hi` in the middle of a dump line.

So `RUN_ENTRY` (and the stdin branch of `cmd_run`) calls `fflush(NULL)`
before `system()`, which covers every dump at once, and `cc_system` does the
same before each toolchain subprocess. A new compile-time report that prints
to stdout needs no flush of its own. Until 2026-10-17 only
`--dump-mono-specs` flushed
([tur-run-dump-output-interleaves-program-output](https://github.com/turmeric-lang/turmeric/blob/main/docs/archive/tur-run-dump-output-interleaves-program-output.md)).

### Auto-spice discovery

Per-file commands find the enclosing project themselves (CLAUDE.md "Per-file
Commands Inside a Spice"). Two helpers do it:

| Helper | What it adds |
|---|---|
| `auto_append_spice_includes` -> `spice_search_append` | the enclosing spice's `src/` and every `:spices` dep's `src/`, after the user's `-I` (first match wins). Walks at most `TUR_SPICE_WALK_MAX` (16) ancestors. Records LS2 provenance for the workspace-sibling advisory; `TUR_DEBUG_RESOLVER=1` prints each addition |
| `discover_manifest_reader_macros` | the manifest's `:reader-macros` paths, **and** as a side effect applies the user experiments file and the manifest's `:experiments` (`apply_user_config_experiments`, `apply_manifest_experiments`) |

| Command | Spice includes | Reader macros | Manifest + user experiments |
|---|---|---|---|
| `check`, `expand`, `audit-spans`, `emit-c`, `emit-h` | yes | yes | yes |
| `run <file>` | yes | yes | yes |
| `build <file>`, `compile <file>` | yes | yes (the arm reads the manifest itself) | **no** |
| `jit <file>` | yes | yes | yes |
| `build <dir>` | from the manifest | yes (`discover_manifest_reader_macros` per module) | yes |
| `run` (project mode) | from the manifest | from the manifest | **no** |

The "no" cells in the experiments column are arms that hand-roll the
manifest read (`pkg_manifest_read` + `resolve_manifest_reader_macros`)
instead of calling `discover_manifest_reader_macros`, which is the only
place `apply_manifest_experiments` and `apply_user_config_experiments` run.
`cmd_run`'s single-file branch carries a comment about exactly this mistake.
So a project with `:experiments [reflected-measures]` gets the gate under
`tur check src/main.tur`, `tur run src/main.tur` and `tur build .`, but
`tur build src/main.tur`, `tur compile src/main.tur` and a bare `tur run`
compile with it off (and say "`^reflect` is ignored").

`cmd_jit` walks for spice includes as the `run` arm does before delegating
to it; until 2026-10-17 it did not, so `tur jit <file>` missed intra-spice
imports `tur run --engine jit <file>` resolved
([tur-jit-skips-spice-include-discovery](https://github.com/turmeric-lang/turmeric/blob/main/docs/archive/tur-jit-skips-spice-include-discovery.md)).

`--no-auto-spice` sets `g_no_auto_spice`, which turns off the include walk
only; reader macros and experiments still apply. CLAUDE.md's statement that
single-file `tur build` does not auto-discover is out of date: it does, and
`--no-auto-spice` turns it off.

---

## 6. The build directory

`drv_resolve_build_dir(input_or_root, cli_flag)` decides where project builds
put their output. Precedence, highest first:

1. `--build-dir <dir>` / `-B <dir>`.
2. `TUR_BUILD_DIR`.
3. `:build-dir` in the nearest manifest (`drv_find_project_root`, unbounded
   walk-up; relative to the manifest's directory unless absolute).
4. `<project-root>/build` when a manifest exists, else `./build`.

The result is `realpath`ed; `obj/`, `bin/` and `lib/` are created, and a
`.gitignore` containing `*` is written if none exists.

Who uses it:

| Command | Uses `drv_resolve_build_dir` | Output when no `-o` |
|---|---|---|
| `build <file>` | **no** | `./<stem>` in the cwd; intermediate C in `$TMPDIR/tur-build/` |
| `build <dir>` (single-main fast path) | yes | `<bd>/bin/<:name>` |
| `build <dir>` (separate compilation) | yes | `<bd>/bin/<:name>` or `<bd>/lib/lib<:name>.so`; intermediates in `<bd>/obj/` |
| `compile <file>` | yes, for the default object | `<bd>/obj/<stem>.o` |
| `emit-c --build-dir <dir>` | **no** | `<dir>/<mod>.{h,c}` flat, plus `<dir>/.tur-abi-cache/` |

So CLAUDE.md's "Build output directory" section is right about the
precedence but overstates the layout: `emit-c --build-dir` writes no
`obj/`/`bin/`/`lib/`, no `.gitignore`, and ignores `TUR_BUILD_DIR`; and the
"anchored under `<build-dir>/bin/`" rule applies to `tur build <dir>`, not
to `tur build <file>` run inside a project.

---

## 7. Caches and what invalidates them

| Cache | Location | Key | Invalidated by |
|---|---|---|---|
| Intermediate C | `$TMPDIR/tur-build/<sanitized base name>-<hash>.c` | xxHash64 of the input's real path | overwritten every build; kept for ccache |
| r7rs library-unit object | `$TMPDIR/tur-build/prelude/<hash>.o` | xxHash64 of `TUR_VERSION`, `$CC`, the compile flags (less `-I` dirs that hold no headers, `dir_may_supply_headers`) and the unit's text | a different text or flags; LRU-pruned past 24 objects unused for 24 h (`prelude_cache_prune`) |
| r7rs library-unit emission | `$TMPDIR/tur-build/prelude/<key>.lib` | `split_lib_cache_init`: the `tur` binary (path, dev, inode, size, mtime), the command line less inputs/outputs/`-I`, every `TUR_*` variable except `TUR_SHOW_CC` and `TUR_SPLIT_LIB_CACHE`, the experiments, the emission globals, and the path and content of every stdlib file the unit is written from | any of those changing; a program that reads a module of its own or would put its own item in the unit is never cached; pruned past 32 entries. `TUR_SPLIT_LIB_CACHE=0` disables, `=verify` re-emits and compares |
| ABI clone index | `<build-dir>/.tur-abi-cache/index` (or `<emit-c dir>/.tur-abi-cache/`) | -- | rewritten by every separate-compilation build. **Nothing reads it back**: clones are recomputed each build, so `--no-abi-cache`/`TUR_NO_ABI_CACHE` only skips the write |
| REPL spice image | `<root>/.tur-repl-cache/lib-N.so` + `.built-by` | the `.built-by` stamp: `TUR_VERSION`, the `tur` path, size and mtime | a `.tur` under the root newer than the library, or a stamp written by another `tur` (`needs_rebuild` in `spice_loader.c`) |
| cmake deps | `<root>/cmake/` | presence of the arm's dep manifest (`pkg_cmake_manifest_name`) | deleting it; see the [package manager internals](package-manager-internals-guide.md) |

Three properties of the intermediate-C cache are worth knowing:

- **It is keyed on the file, not the path string.** The name carries a hash
  of the input's real path (`m_tur-<16 hex>.c`), so two concurrent
  `tur build m.tur` from different directories write different files. Until
  2026-10-17 it was the path as typed, and either `cc` could compile the
  other one's program
  ([tur-build-intermediate-c-path-collides](https://github.com/turmeric-lang/turmeric/blob/main/docs/archive/tur-build-intermediate-c-path-collides.md)).
  Two concurrent builds of the *same* file still share one name.
- The same prefix holds the object, `.c` and `.link` sidecar of a
  `tur build --split-build` (`<stem>.o`), which the `build` arm deletes
  after the link. They are keyed on the stem alone.
- `$TMPDIR` is read once (`tur_temp_dir`); on Windows it is `TMP`, `TEMP`,
  then `USERPROFILE`.

---

## 8. Environment variables

The driver (`src/main.c`, `src/driver/`) reads these (grep `getenv` to
refresh the list). Variables read only elsewhere are noted.

**Toolchain and linkage**

| Variable | Effect |
|---|---|
| `CC` | the C compiler (default `cc`) |
| `TUR_CC_FLAGS` | replaces the default `cc` flags; `-fsanitize` in it disables the preamble swap |
| `TUR_RUNTIME` | `auto`/`lib`/`source`/`split`; `--runtime=` wins |
| `TUR_RUNTIME_LIB` | directory or archive path for `locate_runtime_lib` |
| `TUR_SDK_ROOT` | the SDK prefix for `-lturi` anchoring |
| `TUR_RCGC_FROM_ARCHIVE` | `0` keeps the emitted rc/GC replica; `1` takes the archive posture even for `emit-c`, when an archive is found |
| `TUR_PREAMBLE_SPLIT` | `0` off; `1` opts in on platforms where `auto` does not |
| `TUR_PRELUDE_SPLIT`, `TUR_IMPORT_SPLIT` | `0` turns off the r7rs two- and three-unit builds |
| `TUR_PRELUDE_JOBS` | pieces the library unit is compiled in (1-8; default one per CPU) |
| `TUR_SPLIT_LIB_CACHE` | `0` off, `verify` re-emit and compare |
| `TMPDIR` (`TMP`, `TEMP`, `USERPROFILE` on Windows) | the temp root |

**Paths and discovery**

| Variable | Effect |
|---|---|
| `TUR_STDLIB_DIR` | the stdlib root; set by `resolve_stdlib_root` if unset |
| `TUR_BUILD_DIR` | rung 2 of the build-dir ladder |
| `TUR_ENGINE` | rung 2 of the engine ladder |
| `TUR_NO_ABI_CACHE` | same as `--no-abi-cache` |
| `TUR_DOCS_DIR` | where `tur docs` looks |
| `TUR_NO_AUTO_SPICE`, `TUR_BIN` | the REPL's discovery opt-out and subprocess binary (read in `src/turi/`) |
| `TUR_STACK_MB` | the driver's stack (read in `stack_guard.c`) |
| `TUR_REPL_JIT_INLINE_C=0` | the interpreter refuses an inline-C defn it cannot run instead of JIT-compiling it (the `repl-jit-inline-c` graduation's off switch; see the [REPL guide](repl.md#inline-c-at-the-prompt)) |

**Representation and memory seams** (two-way, `0`/`1`; for bisection, not
features): `TUR_SR1_SUM_BYVALUE`, `TUR_SR2_APP_SUM_BYVALUE`,
`TUR_OPTION_NICHE`, `TUR_REGIONS`, `TUR_R7RS_GC`, `TUR_SAFFRON_GC`, and the
shelved `TUR_ADT_SLAB`. See the
[value representations guide](value-representations-guide.md).

**Tracing**

| Variable | Prints |
|---|---|
| `TUR_SHOW_CC` | every `cc` command, and every split/cache decision (19 sites) |
| `TUR_VERBOSE` | the engine `tur run` delegated to |
| `TUR_DEBUG_RESOLVER=1` | each workspace-sibling include added |
| `TUR_JIT_TIMING`, `TUR_JIT_DUMP_C`, `TUR_JIT_SPLIT_DEBUG`, `TUR_JIT_NO_SPLIT`, `TUR_JIT_FORCE_FALLBACK`, `TUR_JIT_SYS_INCLUDE` | see the [JIT guide](jit-guide.md) |
| `TUR_TSAN`, `RESULTS_DIR` | `tur worker` only |

The CPS and emitter traces (`TUR_TRACE_EVICT`, `TUR_CPS_CENSUS`, ...) are
read by the passes, not the driver; see
[CPS and the DK machine](cps-dk-internals-guide.md#debugging-and-testing)
and [diagnosing the compiler](diagnosing-the-compiler-guide.md).

---

## 9. Adding a subcommand or a global flag

### A new subcommand

1. Write its handler, `int cmd_<name>_main(int argc, char **argv)`, in the
   `src/driver/` file for its area, and declare it in that file's section of
   `driver/commands.h`. `argv[2]` is its first argument.
2. Add a row to `COMMANDS[]` in `driver/cli.c`: the name, the handler, its
   flags, a one-line summary for the completion scripts, and its lines for
   `tur --help` (NULL to list none). The row's position is its place in the
   `--help` listing. That is all dispatch, prefix resolution, the
   external-command filter and the completion scripts' command lists need.
   Check the new name does not make an existing prefix ambiguous; a name that
   would (as `completion` would for `tur comp`) gets `DRV_CMD_EXACT`.
3. Write `usage_<name>` in `driver/cli.c`: `--help` returns 0 through it;
   error paths return `drv_usage_error(usage_<name>)` (exit 2).
4. If it compiles a file, call `auto_append_spice_includes` and
   `discover_manifest_reader_macros`, set the LS2 context around the
   compile (`ls2_resolver_ctx_set`), and dispose of both afterwards. Copy
   an existing handler (`cmd_emit_h_main` is the shortest).
5. If it takes arguments worth completing, add an arm for it to the
   per-command `case` in both scripts in `src/cli/completion.c`.
6. Rewrite the CLI goldens (`TUR_FLAGS_GOLDEN_UPDATE=1 bash
   tests/run-flags.sh`), add the new `help-<name>` golden to the list there,
   and review the diff.
7. Document it in the user guide for its area, and in
   [compiler-flags-guide.md](compiler-flags-guide.md) if it takes flags.

### A new global flag

1. Add the `g_<name>` global (in `src/runtime/globals.c` + `globals.h` if a
   pass or the emitter reads it; in the driver file that owns it, declared
   in its `driver.h` section, if only the driver does).
2. Add a row to `GLOBAL_FLAGS[]` in `driver/cli.c`: the spelling, the
   global it sets (or a `gf_*` action), and how it is read. A plain row is
   applied and stripped by the strip pass; `GF_SCAN` applies it wherever it
   appears, even as another flag's value; `GF_VALUE` / `GF_PREFIX` take a
   value; `GF_KEEP` leaves it in `argv`.
3. Add the same token to `wk_apply_flags`, the worker's parser.
4. If it changes the emitted C, add it to the key in `split_lib_cache_init`
   (`SLK(...)`), or the r7rs library-unit cache will serve stale text.
   `TUR_SPLIT_LIB_CACHE=verify` is the check.
5. Add it to the "global flags" block of `drv_usage`, to both completion
   scripts (`_arguments` in zsh, `__tur_global_flags` in bash), and to
   [compiler-flags-guide.md](compiler-flags-guide.md). Rewrite the CLI
   goldens and add it to the `flags-stripped` golden's command line.
6. If it is an in-flight feature rather than a diagnostic or codegen knob,
   it is an experiment instead: an `EXPERIMENTS[]` row behind `--enable=`
   (see the [experimental flags guide](experimental-flags-guide.md)).

---

## Debugging the driver

| Question | Tool |
|---|---|
| What `cc` ran, with what flags? | `TUR_SHOW_CC=1 tur build f.tur` |
| What C was actually compiled? | the `.c` path in that `CC:` line (post-swap, post-hoist) -- not `tur emit-c` |
| Where did the time go? | `tur --timings build f.tur` (a sanitized Debug `tur` says its numbers are not representative) |
| Did the split or the cache engage? | `TUR_SHOW_CC=1`, which prints every decline and cache decision |
| Which engine did `tur run` use? | `TUR_VERBOSE=1` |
| Which include dirs came from where? | `TUR_DEBUG_RESOLVER=1` |
| Is it the split's fault? | rerun with `TUR_PRELUDE_SPLIT=0`, `TUR_PREAMBLE_SPLIT=0`, or `--runtime=source` |

---

## Bugs found while writing this guide

Each was reproduced against the compiler this guide describes and filed in
[`docs/reported/`](https://github.com/turmeric-lang/turmeric/blob/main/docs/reported/README.md). When one is fixed and archived,
update the passage above that describes it and drop it from this list.

- [build-file-and-run-project-ignore-manifest-experiments](https://github.com/turmeric-lang/turmeric/blob/main/docs/archive/build-file-and-run-project-ignore-manifest-experiments.md) -- Some commands ignore the manifest's `:experiments`

## Maintenance

Update this guide in the same PR when you:

- add, remove or rename a subcommand, a global flag, or a flag-parsing
  route;
- change `core_passes[]` or move work into or out of `run_core_pass`;
- change how `cc` is chosen, the default flags, or `drv_link_command_run`'s
  command shape;
- add a cache, change a cache key, or change the build-directory ladder;
- add an environment variable the driver reads.

Refresh the environment table with `grep -oh 'getenv("[A-Z_0-9]*")'
src/main.c src/driver/*.c | sort -u`. Cite functions, not line numbers.
