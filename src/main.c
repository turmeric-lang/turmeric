/* tur: the Turmeric compiler driver (phase 2). */

#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif

/* _DARWIN_C_SOURCE exposes mkstemps and DT_REG on macOS without suppressing
 * POSIX extensions; on Linux _DEFAULT_SOURCE covers both. */
#if defined(__APPLE__)
#  ifndef _DARWIN_C_SOURCE
#    define _DARWIN_C_SOURCE
#  endif
#elif !defined(_WIN32)
/* Windows is excluded deliberately: MinGW reads _POSIX_C_SOURCE as "hide the
 * Win32 CRT names", which un-declares mkdir/getcwd and hides _finddata_t --
 * which in turn breaks <dirent.h> itself.  glibc needs this macro to EXPOSE
 * those declarations; on Windows it does the exact opposite. */
#  ifndef _POSIX_C_SOURCE
#    define _POSIX_C_SOURCE 200809L
#  endif
#endif

#include <ctype.h>
#include "source_literal.h"
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <math.h>
#ifndef _WIN32
/* poll(2) has no Windows counterpart for CRT file descriptors; the only user is
 * the fork-based fixture worker, which is compiled out below. */
#include <poll.h>
#endif
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/stat.h>
#include <time.h>
#include <dirent.h>
#include "platform_proc.h"
#include "tur_argcheck.h"
#include "platform_fs.h"  /* realpath/mkdir/setenv/mkstemps/... on Windows */
#ifdef _WIN32
#include <io.h>       /* _setmode, _fileno */
#endif
#ifndef _WIN32
/* waitpid() is only used by the fork-based fixture worker, which is compiled out
 * on Windows.  The WIFEXITED/WEXITSTATUS uses that remain are applied to a
 * system() return value, and platform_fs.h defines those there. */
#include <sys/wait.h>
#include <sys/time.h>   /* utimes: the prelude cache's LRU stamp */
/* The docs server (cmd_docs_serve), which Windows does not build. */
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#endif
#ifdef __APPLE__
#include <mach-o/dyld.h>     /* SN1: _NSGetExecutablePath for stdlib resolution */
#endif

#include "arena.h"
#include "assert.h"
#include "buf.h"
#include "borrow_check.h"  /* Phase 14 */
#include "cps.h"          /* Phase 18: CPS transformation */
#include "srfi_prune.h"   /* r7rs-srfi-plan S3: unreached SRFI definitions */
#include "jit_prune.h"    /* JIT: drop the prelude a program never reaches */
#include "cps_ir.h"       /* CPS2: ANF/CPS IR (--dump-cps) */
#include "diag.h"
#include "effect_check.h" /* Phase P19-2: effect-row inference */
#include "effect.h"       /* built-in effect registration */
#include "kind_check.h"   /* Phase HKT H0: kind inference pass */
#include "compiler/refine_smtlib.h" /* SX8a: `tur smt` -- the SMT-LIB2 seam */
#include "compiler/refine_solver.h" /* SX8a: the S0..S3 chain `tur smt` runs */
#include "elab.h"
#include "emit.h"
#include "compiler/emit_split.h" /* r7rs-programs-compile-slowly */
#include "compiler/stack_guard.h" /* tur_run_on_big_stack -- see the note at EOF */
#include "runtime/hamt.h" /* S2: tur_hamt_hash_xxh64 for the split-artifact hash */
#include "runtime/image.h" /* AI6: tur image-info / image-verify */
#include "runtime/rt_split_embed.h" /* S2: committed decls region + hash (TUR_JIT) */
#include "turi/spice_loader.h" /* J2: the REPL's in-process jit hook */
#include "turi/jit_ffi.h"      /* jit-ffi-c2mir-plan: dynamic-FFI provider */
#include "turi/inline_c_jit.h"  /* aot-compiled-repl-plan C1 */
#include "effect_lower.h" /* Phase 19: Effect lowering */
#include "expr.h"
#include "fmt.h"
#include "forms.h"
#include "pass.h"         /* Phase P19-1: pass scheduling */
#include "prof.h"         /* profiling-plan C0+C1: --timings */
#include "reader.h"
#include "reader_macros.h"
#include "lang_dialects.h"  /* `tur dialects`: the `#lang` base axis */
#include "refine_discharge.h" /* RT3: per-compile refinement stats reset */
#include "span_audit.h"  /* debugger Phase 1: breakpoint-span coverage audit */
#include "symbols.h"
/* Phase S0: eval API for tur repl */
#include "turi/eval.h"
#include "turi/collections_native.h"
#include "turi/interpreter_natives.h"
#include "turi/docstrings.h"
#include "turi/preload.h"
/* Phase S1: REPL with libedit, multi-line input, :type/:doc/:reload */
#include "turi/repl.h"
/* Phase PKG-1: Spice package manager */
#include "pkg.h"
#include "global.h"   /* tur_installed_spice_dir: :global spice deps */
#include "spice_search.h"  /* find_spice_root / auto_append_spice_includes */
/* RN0-RN7: Justfile-compatible task runner */
#include "justrun.h"
/* Global configuration variables — defined in globals.c */
#include "globals.h"
#include "experiments.h"  /* XF1: --enable=<name> experimental-flag registry */
#include "jit_engine.h"   /* J1: tur jit (TUR_HAVE_JIT builds only) */
#include "mono_specs.h"   /* VBM1: --dump-mono-specs registry dump */
/* LSP server */
#include "lsp/lsp.h"
#include "lsp/lsp_sym.h"
#include "lsp/lsp_collect.h"
#include "lsp/lsp_scope.h"
#include "stdlib_autoload.h"
#include "lsp/lsp_docs.h"
/* MCP server */
#include "lsp/mcp.h"
/* DAP server (debugger Phase 3) */
#include "turi/dap.h"
#include "turi/trace.h"
/* lsp-lite: lightweight completion/calltip backend for editors */
#include "cli/lsp_lite.h"
/* tur completion <zsh|bash>: emit a shell completion script */
#include "cli/completion.h"
#include "cli/demangle.h"
/* The driver's shared declarations (main-c-split-plan). */
#include "driver/driver.h"
#include "driver/commands.h"

/* -------------------------------------------------------------------------
 * Forward declarations: the functions and globals used above their
 * definitions.  Each one is described at its definition.
 * ---------------------------------------------------------------------- */

static int cmd_build_once(const char *input, const char *out_path,
                          const char **include_dirs, int n_include_dirs,
                          const char *target,
                          const char **reader_macro_paths,
                          int n_reader_macro_paths,
                          bool allow_split, bool *retry_whole, bool *three_units);

/* cli-usage-error-paths-exit-zero: a `usage_*` helper prints the subcommand's
 * usage and returns 0 -- right for `--help`, which is a successful request
 * for help.  The ERROR paths (an unknown flag, a missing or duplicated input)
 * print the same text but must exit nonzero, or a script cannot tell a typo
 * from a successful build: `tur build --typo || exit 1` used to succeed.
 * They route through here.  2 is the subcommand-level usage status (`tur docs`
 * and `tur expand` already used it); 64 (EX_USAGE) stays with the top-level
 * dispatcher's unknown-subcommand path. */
int drv_usage_error(int (*usage)(void)) {
    (void)usage();
    return 2;
}

/* True when a `--help` / `-h` appears among the subcommand's arguments.  For
 * the subcommands whose argv loop has no explicit help arm (emit-c, emit-h),
 * so that `--help` keeps exiting 0 rather than falling into the unknown-flag
 * arm, which now exits 2. */
static bool has_help_flag(int argc, char **argv) {
    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) return true;
    }
    return false;
}

/* Generate _main.c that includes all .h files and has main(). */
static int generate_main_c(Buf *out, const char **h_files, int n_files, const char *output_name) {
    buf_printf(out, "/* generated by tur (phase 2) - _main.c */\n");
    /* Feature-test macros must precede any system #include.  _main.c pulls in
     * <stdio.h> below (and tur_runtime.h transitively), and the first system
     * header locks in <features.h>'s feature set -- so a later
     * `#define _DEFAULT_SOURCE` inside tur_runtime.h arrives too late and
     * leaves POSIX symbols (strdup, clock_gettime/CLOCK_MONOTONIC, nanosleep)
     * unprototyped/undeclared.  Emit it first so it covers every include. */
    buf_puts(out, "#define _DEFAULT_SOURCE 1\n");
    buf_puts(out, "#include <stdio.h>\n");
    buf_puts(out, "#include <stdint.h>\n");
    buf_puts(out, "#include <stdbool.h>\n\n");

    for (int i = 0; i < n_files; i++) {
        buf_printf(out, "#include \"%s\"\n", h_files[i]);
    }
    buf_puts(out, "\n");

    /* For now, just declare main in _main.c if no module has it.
     * In the future, we'll detect which module has main and only include that.
     * For phase 2, we assume the user provides main in one of the modules. */
    buf_puts(out, "/* main() should be defined in one of the included modules */\n");
    return 0;
}

/* target: NULL for native build, "wasm" to compile with emcc. */
int cmd_build(const char *input, const char *out_path,
              const char **include_dirs, int n_include_dirs,
              const char *target,
              const char **reader_macro_paths,
              int n_reader_macro_paths) {
    /* r7rs-programs-compile-slowly: try the split build; if either unit fails
     * to compile or the two fail to link, build the program as one unit --
     * slower, never wrong. */
    bool retry = false;
    bool three = false;
    g_import_split_failed = false;
    g_split_lib_cache_off = false;
    int rc = cmd_build_once(input, out_path, include_dirs, n_include_dirs, target,
                            reader_macro_paths, n_reader_macro_paths, true, &retry, &three);
    /* r7rs-split-units-each-compile-the-runtime: units that did not build
     * around a library unit from the cache are tried again around an emitted
     * one first (which replaces the entry if it differs). */
    if (retry && g_split_lib_from_cache) {
        if (getenv("TUR_SHOW_CC"))
            fprintf(stderr, "tur: split build failed with the cached library unit; emitting it\n");
        g_split_lib_cache_off = true;
        rc = cmd_build_once(input, out_path, include_dirs, n_include_dirs, target,
                            reader_macro_paths, n_reader_macro_paths, true, &retry, &three);
    }
    /* r7rs-imported-libraries-recompiled-every-build: three units that did
     * not build try two (the imported libraries back in the program's)
     * before one. */
    if (retry && three) {
        if (getenv("TUR_SHOW_CC"))
            fprintf(stderr, "tur: imported-library split build failed; building as two units\n");
        g_import_split_failed = true;
        rc = cmd_build_once(input, out_path, include_dirs, n_include_dirs, target,
                            reader_macro_paths, n_reader_macro_paths, true, &retry, &three);
        g_import_split_failed = false;
    }
    if (!retry) return rc;
    if (getenv("TUR_SHOW_CC"))
        fprintf(stderr, "tur: prelude split build failed; building as one unit\n");
    return cmd_build_once(input, out_path, include_dirs, n_include_dirs, target,
                          reader_macro_paths, n_reader_macro_paths, false, &retry, &three);
}

static int cmd_build_once(const char *input, const char *out_path,
                          const char **include_dirs, int n_include_dirs,
                          const char *target,
                          const char **reader_macro_paths,
                          int n_reader_macro_paths,
                          bool allow_split, bool *retry_whole, bool *three_units) {
    *retry_whole = false;
    *three_units = false;
    g_split_lib_from_cache = false;
    /* RT3: reset per-compile refinement state, like the check/run/emit-c entry
     * points. The memo caches VC pointers into the per-compile arena; a process
     * that builds >1 file (`tur test`, LSP) otherwise keeps stale pointers and
     * memo_lookup dereferences them on a fingerprint collision. Partial fix for
     * the strict-refine multi-compile crash (see docs/reported). */
    refine_discharge_reset();
    /* DEDUP-4b: `tur` links this output, so the rc<T>/GC runtime may come
     * from the archive rather than being replicated into the preamble. */
    g_emit_for_link = true;
    Buf csrc;
    buf_init(&csrc);
    /* used-attr-whole-program: this single-file/whole-program path inlines only
     * the entry's Turmeric import closure, so a #[used] defn in a sibling
     * module reached only via a raw mangled C symbol (no `(import)`) would be
     * dropped and dangle at link time -- unlike `tur build <project>`, which
     * falls back to separate compilation.  Scan the -I search dirs for such
     * modules and publish them so elaborate_program force-loads (and thus
     * emits) them.  Scoped to the build path (not check/emit-c) so inspection
     * output and snapshots are unaffected. */
    int n_used_mods = 0;
    char **used_mods = collect_used_attr_modules(input, include_dirs,
                                                 n_include_dirs, &n_used_mods);
    UsedModulesCtx used_ctx = { (const char **)used_mods, n_used_mods };
    if (n_used_mods > 0) used_modules_ctx_set(&used_ctx);
    /* r7rs-programs-compile-slowly: a native, whole-preamble build of a
     * `#lang r7rs` program may come back as two units (prelude_split_applies
     * decides; compile_to_c leaves `split_lib` empty when it declines). */
    Buf split_lib;
    buf_init(&split_lib);
    /* r7rs-imported-libraries-recompiled-every-build: and the imported
     * libraries' unit, when the program imports any. */
    Buf split_imp;
    buf_init(&split_imp);
    bool try_split = allow_split && !(target && strcmp(target, "wasm") == 0) &&
                     g_runtime_mode != TUR_RT_SPLIT;
    if (try_split) { g_split_lib_sink = &split_lib; g_split_imp_sink = &split_imp; }
    int rc = compile_to_c(input, &csrc, include_dirs, n_include_dirs,
                          reader_macro_paths, n_reader_macro_paths);
    g_split_lib_sink = NULL;
    g_split_imp_sink = NULL;
    if (n_used_mods > 0) used_modules_ctx_set(NULL);
    free_tur_files(used_mods, n_used_mods);
    if (rc != 0) { buf_free(&csrc); buf_free(&split_lib); buf_free(&split_imp); return rc; }
    const bool prelude_split = split_lib.len > 0;
    if (!prelude_split) split_imp.len = 0;
    const bool import_split = split_imp.len > 0;
    *three_units = import_split;
    if (prelude_split) hoist_tur_include_directives(&split_lib);
    if (import_split) hoist_tur_include_directives(&split_imp);

    /* Write generated C to a deterministic path so ccache can cache the result
     * across repeated builds of the same .tur file.  Fall back to a random
     * temp if the stable path cannot be constructed (e.g. path too long). */
    char tmpl[1024];
    drv_stable_c_path(input, tmpl, sizeof(tmpl));
    FILE *tf = tmpl[0] ? drv_fopen_nofollow_w(tmpl) : NULL;
    if (!tf) {
        /* fallback: random temp */
        char fallback[512];
        snprintf(fallback, sizeof(fallback), "%s/tur-XXXXXX.c", tur_temp_dir());
        int fd = mkstemps(fallback, 2);
        if (fd < 0) {
            fprintf(stderr, "tur: cannot create temp file\n");
            buf_free(&csrc);
            buf_free(&split_lib);
            buf_free(&split_imp);
            return 2;
        }
        tf = fdopen(fd, "wb");
        memcpy(tmpl, fallback, sizeof(fallback));
    }
    bool wasm_target = target && strcmp(target, "wasm") == 0;

    /* cc-path-preamble-split-plan: swap the fixed runtime preamble for the
     * decls region and link libturt_preamble.a instead, so the preamble is
     * compiled once rather than once per program.  The default whenever
     * preamble_split_auto_applies() holds; --runtime=split forces it.
     *
     * Same swap the JIT does (jit_try_split_preamble), including its hash
     * guard: if the emitted preamble no longer matches the committed
     * artifact the swap DECLINES and we fall back to the full preamble, which
     * is slower but always correct.  A silent disengage is the documented
     * failure mode on the JIT side, so an explicit --runtime=split says so
     * rather than quietly producing a build that is not what was asked for.
     * (CI's engage probe runs with TUR_JIT_TIMING=1, which reports the
     * default's declines too.)
     *
     * Never for wasm (emcc has no libturt_preamble.a), and not on top of an
     * r7rs prelude split, whose program unit is already the small half. */
    Buf split_c;
    buf_init(&split_c);
    bool used_split = false;
    /* --timings: the swap re-emits the preamble to hash it -- emission. */
    prof_enter(PROF_EMIT);
    if (g_runtime_mode == TUR_RT_SPLIT) {
        if (wasm_target)
            fprintf(stderr,
                    "tur build: --runtime=split declined for --target wasm "
                    "(there is no wasm libturt_preamble.a); using the full "
                    "preamble\n");
        else if (!(used_split = jit_try_split_preamble(&csrc, &split_c)))
            fprintf(stderr,
                    "tur build: --runtime=split declined (the emitted preamble "
                    "does not match the committed split artifact -- regenerate "
                    "with tools/gen-runtime-split.py); using the full "
                    "preamble\n");
    } else if (g_runtime_mode == TUR_RT_AUTO && !wasm_target && !prelude_split &&
               preamble_split_auto_applies()) {
        used_split = jit_try_split_preamble(&csrc, &split_c);
    }
    prof_exit(PROF_EMIT);
    Buf *emit_c = used_split ? &split_c : &csrc;

    /* Phase S2: scan for __tur_include__ directives and inject them at the
     * top of the generated C so stdlib modules can add file-level includes.
     *
     * AFTER the split swap, not before.  Hoisting lands these near the top of
     * the TU, which is INSIDE the fixed-preamble region -- so hoisting first
     * meant the swap deleted them, and a program that reaches stdlib/hamt lost
     * the `#include "hamt.h"` it depends on and failed with a wall of implicit
     * declarations.  Swapping first puts them above the decls region instead,
     * where they survive and mean the same thing. */
    hoist_tur_include_directives(emit_c);

    /* performance-audit C-1 (fix #7): gcc spends most of a small build PARSING
     * prelude definitions the program never reaches, and only then drops
     * them -- 0.14 s of `triangle/fib`'s 0.16 s cc
     * (docs/artifacts/perf-audit-2026-10/wp1-compile-latency.md).  Hand it the
     * unit jit_prune already makes for c2mir: the program half keeps only what
     * something live names (src/compiler/jit_prune.h), so cc parses what it
     * compiles.  The prune runs after the include hoist: the hoisted prefix
     * sits before the decls region, and everything before the program half is
     * a root.
     *
     * A reference the scan misses cannot pass silently.  The compile fails,
     * the full unit is compiled instead under TUR-W0071 (after the link
     * below), and tests/run.sh fails any fixture that needed that retry.
     *
     * Not for wasm (emcc), --debug (a debugging session reads the unit whole)
     * or an r7rs prelude split (its program unit is already the small half).
     * TUR_CC_NO_PRUNE=1 opts out, as TUR_JIT_NO_PRUNE=1 does for both. */
    Buf cc_full;
    buf_init(&cc_full);
    bool cc_pruned = false;
    {
        const char *off = getenv("TUR_CC_NO_PRUNE");
        bool disabled = off && *off && strcmp(off, "0") != 0;
        if (!disabled && !wasm_target && !prelude_split && !g_emit_debug_lines) {
            prof_enter(PROF_EMIT);
            buf_write(&cc_full, emit_c->data, emit_c->len);
            JitPruneStats ps;
            memset(&ps, 0, sizeof ps);
            cc_pruned = used_split ? jit_prune_split_source(emit_c, &ps)
                                   : jit_prune_full_source(emit_c, &ps);
            if (!cc_pruned) {
                buf_free(&cc_full);
                buf_init(&cc_full);
            } else if (getenv("TUR_SHOW_CC")) {
                fprintf(stderr,
                        "tur: pruned the C unit: %zu -> %zu bytes (%u of %u "
                        "definitions, %u includes dropped)\n",
                        ps.bytes_before, ps.bytes_after, ps.nodes_dropped,
                        ps.nodes, ps.includes_dropped);
            }
            prof_exit(PROF_EMIT);
        }
    }

    if (!tf || fwrite(emit_c->data, 1, emit_c->len, tf) != emit_c->len) {
        fprintf(stderr, "tur: write failed\n");
        if (tf) fclose(tf);
        buf_free(&split_c);
        buf_free(&csrc);
        buf_free(&cc_full);
        buf_free(&split_lib);
        buf_free(&split_imp);
        return 2;
    }
    fclose(tf);
    buf_free(&split_c);

    /* Phase S2 / tur-link-and-build-split-plan Phase 3: scan the generated C
     * for __tur_autolink__ comments (shared helper). */
    Buf autolink;
    buf_init(&autolink);
    if (prelude_split) {
        /* The library unit's markers count too: a runtime source or library
         * its stdlib code needs is needed by the linked program. */
        Buf both; buf_init(&both);
        buf_write(&both, csrc.data, csrc.len);
        buf_putc(&both, '\n');
        buf_write(&both, split_lib.data, split_lib.len);
        if (import_split) {
            buf_putc(&both, '\n');
            buf_write(&both, split_imp.data, split_imp.len);
        }
        bool al_ok = scan_autolink_markers(&both, &autolink);
        buf_free(&both);
        if (!al_ok) { buf_free(&csrc); buf_free(&cc_full); buf_free(&autolink); buf_free(&split_lib); buf_free(&split_imp); return 2; }
    } else {
        /* The UNPRUNED text: a pruned non-split unit has lost the markers of
         * the definitions it dropped, and the link line stays what it was
         * before the prune existed.  Split, csrc was never pruned. */
        const Buf *al_src = (cc_pruned && !used_split) ? &cc_full : &csrc;
        if (!scan_autolink_markers(al_src, &autolink)) {
            buf_free(&csrc); buf_free(&cc_full); buf_free(&autolink); return 2;
        }
    }
    buf_free(&csrc);

    char chosen_out[1024];
    if (!out_path) {
        drv_default_output_name(input, chosen_out, sizeof(chosen_out));
        /* emcc outputs <name>.js + <name>.wasm; use .js as the primary output */
        if (wasm_target) {
            size_t n = strlen(chosen_out);
            if (n + 3 < sizeof(chosen_out)) { chosen_out[n++] = '.'; chosen_out[n++] = 'j'; chosen_out[n++] = 's'; chosen_out[n] = '\0'; }
        }
        out_path = chosen_out;
    }

    const char *cc;
    if (wasm_target) {
        cc = "emcc";
    } else {
        cc = getenv("CC");
        if (!cc || !*cc) cc = "cc";
    }

    /* TUR_CC_FLAGS overrides the default compiler flags.  Useful for test runs
     * where -O0 is fast enough and ccache benefits from consistent flags.
     * -fno-strict-aliasing: emitted code and inline C blocks routinely pun
     * pointers through int64_t, which GCC's TBAA miscompiles at -O2. */
    const char *cc_flags = getenv("TUR_CC_FLAGS");
    if (!cc_flags || !*cc_flags) {
        if (wasm_target)
            cc_flags = "-O2 -std=c99 -Wall -fno-strict-aliasing -s WASM=1";
        else if (g_emit_debug_lines)
            /* Debugger Phase 4 (--debug): -g for DWARF, -Og for a debugging-
             * friendly optimization level that still maps cleanly onto the
             * `#line`-anchored `.tur` source.  -O0 is NOT used: a self-contained
             * single-file build relies on the optimizer's dead-code elimination
             * to drop preloaded stdlib defns (e.g. contract handlers) that
             * reference libturi-only symbols this build does not link; -O0 keeps
             * them and the link fails.  -Og keeps that DCE while preserving
             * line-accurate stepping. */
            cc_flags = "-g -Og -std=c99 -Wall -fno-strict-aliasing";
        else if (g_cc_release)
            cc_flags = "-O3 -std=c99 -Wall -fno-strict-aliasing";
        else
            cc_flags = "-O2 -std=c99 -Wall -fno-strict-aliasing";
    }

    /* Collect cmake dep flags from cmake/spice-deps-manifest.json if present */
    Buf cmake_flags;
    buf_init(&cmake_flags);
    /* spices-c-sources-plan: the enclosing spice's :c-includes (as -I, visible
     * to inline-C in the generated TU) and :c-sources (vendored .c, compiled
     * as extra translation units and linked in). Resolved relative to the
     * manifest dir found by walking up from the input file. */
    Buf aux_includes; buf_init(&aux_includes);
    Buf aux_sources;  buf_init(&aux_sources);
    drv_collect_build_aux(input, &cmake_flags, &aux_includes, &aux_sources,
                          target);
    /* `:web` -> emcc flags.  Appended to cmake_flags because that Buf is
     * already spliced verbatim into the link command. */
    if (wasm_target && !drv_collect_web_link_flags(input, &cmake_flags)) {
        buf_free(&cmake_flags); buf_free(&aux_includes); buf_free(&aux_sources);
        buf_free(&autolink);
        buf_free(&cc_full);
        return 2;
    }

    /* tur-link-and-build-split-plan Phase 2/3c: under --runtime=lib, swap bare
     * runtime .c autolink sources for a link against the prebuilt libturi.a
     * (native builds only -- emcc/wasm has no libturi.a). */
    if (!wasm_target) apply_runtime_lib_mode(&autolink);
    autolink_regions_off(&autolink);

    /* cc-path-preamble-split-plan: the program half was emitted with the decls
     * region in place of the preamble, so the definitions have to come from
     * libturt_preamble.a.  Linked all-or-nothing and only here -- it is NOT a
     * member of libturt_runtime.a, because a normal program defines these
     * symbols itself and would collide with any member the linker pulled in.
     *
     * TUR_RT_SPLIT_HOSTED tells the decls region it is being compiled for a
     * host link rather than the JIT: it takes `extern int
     * tur_closure_headers_enabled;` instead of the definition (the runtime
     * archive has the definition), and skips the project-header includes whose
     * strict prototypes conflict with the loose externs the program emits.
     *
     * --gc-sections, because "all-or-nothing" is literal: the archive is ONE
     * object, so any reference pulled in the whole preamble, where the inline
     * preamble had let -O2 drop every static function the program never
     * called.  A one-line program went from 23 KB to 93 KB stripped.  The
     * archive is built with -ffunction-sections/-fdata-sections, so the linker
     * can drop the same functions the compiler used to (19 KB stripped -- a
     * little under the inline build).
     *
     * -dead_strip is the Mach-O spelling, and it is needed there for the same
     * reason: a one-line program measured 58,800 -> 171,896 bytes without it,
     * and 53,312 with (macOS 27 / Apple clang 21, arm64).  Mach-O strips at
     * atom granularity, so the archive's section flags are already enough.
     * Windows is the one arm still linking without it: MinGW's ld has
     * --gc-sections, but no Windows run has checked a PE link with it. */
    Buf split_flags;
    buf_init(&split_flags);
    if (used_split) {
        char libdir[4096], libname[128];
        if (locate_runtime_lib(libdir, sizeof(libdir), libname, sizeof(libname))) {
            Buf inj;
            buf_init(&inj);
            buf_printf(&inj, "-lturt_preamble -L%s", libdir);
#if defined(__APPLE__)
            buf_puts(&inj, " -Wl,-dead_strip");
#elif !defined(_WIN32)
            buf_puts(&inj, " -Wl,--gc-sections");
#endif
            if (autolink.len > 1) {
                buf_putc(&inj, ' ');
                buf_puts(&inj, autolink.data);
            }
            buf_putc(&inj, '\0');
            buf_free(&autolink);
            autolink = inj;
        } else {
            fprintf(stderr,
                    "tur build: --runtime=split could not locate the runtime "
                    "archive directory; the link will need a -L in "
                    "TUR_CC_FLAGS\n");
        }
        buf_printf(&split_flags, "%s -DTUR_RT_SPLIT_HOSTED", cc_flags);
        buf_putc(&split_flags, '\0');
        cc_flags = split_flags.data;
    }

    /* tur-link-and-build-split-plan Phase 1: resolve the raw autolink flags
     * (SDK anchoring, ASan autodetect, tree-relative path anchoring, and the
     * -lturi bare-.c filter) via the shared helper -- the same resolution
     * `tur link` runs on flags read from a `.link` sidecar. */
    bool autolink_needs_asan = false;
    resolve_autolink_flags(&autolink, cc_flags, &autolink_needs_asan);

    /* tur-link-and-build-split-plan Phase 1: assemble + run the link `cc`
     * command via the shared helper.  Inputs is the single generated `.c`, so
     * this is the byte-identical monolithic compile+link -- the same helper
     * `tur link` calls with `.o` inputs. */
    /* r7rs-programs-compile-slowly: the program unit links the library
     * unit's cached object; a failure at either step sends cmd_build back to
     * a one-unit build. */
    /* Quoted per path: drv_link_command_run splices this in verbatim, so each
     * element carries its own quoting (D-1). */
    char inputs[4600];
    {
        Buf _in; buf_init(&_in);
        if (!drv_buf_put_quoted(&_in, tmpl)) { buf_free(&_in); inputs[0] = '\0'; }
        else { buf_putc(&_in, '\0'); snprintf(inputs, sizeof inputs, "%s", _in.data); buf_free(&_in); }
    }
    if (prelude_split) {
        char obj[1024], imp_obj[1024];
        imp_obj[0] = '\0';
        /* r7rs-imported-libraries-recompiled-every-build: the imported
         * libraries' unit is cached the same way, by its own text. */
        if (prelude_split_object(&split_lib, cc, cc_flags, &aux_includes, &autolink,
                                 autolink_needs_asan, &cmake_flags,
                                 include_dirs, n_include_dirs,
                                 EMIT_SPLIT_PIECE_PREFIX, prelude_pieces(), false,
                                 obj, sizeof obj) != 0 ||
            (import_split &&
             prelude_split_object(&split_imp, cc, cc_flags, &aux_includes, &autolink,
                                  autolink_needs_asan, &cmake_flags,
                                  include_dirs, n_include_dirs,
                                  EMIT_SPLIT_IMP_PIECE_PREFIX, import_split_pieces(), true,
                                  imp_obj, sizeof imp_obj) != 0)) {
            *retry_whole = true;
            buf_free(&split_lib);
            buf_free(&split_imp);
            buf_free(&cc_full);
            buf_free(&aux_includes);
            buf_free(&aux_sources);
            buf_free(&autolink);
            buf_free(&cmake_flags);
            buf_free(&split_flags);
            return 2;
        }
        {
            Buf _in; buf_init(&_in);
            bool _ok = drv_buf_put_quoted(&_in, tmpl);
            buf_putc(&_in, ' ');
            _ok = drv_buf_put_quoted(&_in, obj) && _ok;
            if (imp_obj[0]) {
                buf_putc(&_in, ' ');
                _ok = drv_buf_put_quoted(&_in, imp_obj) && _ok;
            }
            buf_putc(&_in, '\0');
            if (_ok) snprintf(inputs, sizeof inputs, "%s", _in.data);
            else inputs[0] = '\0';
            buf_free(&_in);
        }
    }
    buf_free(&split_lib);
    buf_free(&split_imp);
    /* A pruned unit is compiled first with cc's stderr held back.  If that
     * fails, the unit as emitted decides:
     *   - it fails too: the program itself is wrong, and only ITS errors are
     *     shown, exactly as without the prune;
     *   - it builds: the prune dropped something live, a bug in jit_prune.
     *     TUR-W0071 says so with the pruned attempt's errors, and
     *     tests/run.sh fails a fixture that hits it. */
    char cc_err[1100];
    cc_err[0] = '\0';
    if (cc_pruned && tmpl[0] &&
        (size_t)snprintf(cc_err, sizeof cc_err, "%s.err", tmpl) >= sizeof cc_err)
        cc_err[0] = '\0';
    int link_rc = drv_link_command_run(cc, cc_flags, inputs, &aux_includes,
                                       &aux_sources, &autolink, autolink_needs_asan,
                                       &cmake_flags, include_dirs, n_include_dirs,
                                       out_path, cc_err[0] ? cc_err : NULL);
    if (link_rc == 0 && cc_err[0]) {
        drv_replay_to_stderr(cc_err);   /* its warnings are the user's */
    } else if (link_rc != 0 && cc_pruned) {
        FILE *rf = drv_fopen_nofollow_w(tmpl);
        if (rf && fwrite(cc_full.data, 1, cc_full.len, rf) == cc_full.len) {
            fclose(rf);
            link_rc = drv_link_command_run(cc, cc_flags, inputs, &aux_includes,
                                           &aux_sources, &autolink, autolink_needs_asan,
                                           &cmake_flags, include_dirs, n_include_dirs,
                                           out_path, NULL);
            if (link_rc == 0) {
                fprintf(stderr,
                        "tur: warning: TUR-W0071: the pruned C unit failed to "
                        "compile where the full unit builds, so the full unit "
                        "was used. This is a bug in the prune "
                        "(TUR_CC_NO_PRUNE=1 skips it); the pruned unit's cc "
                        "output was:\n");
                if (cc_err[0]) drv_replay_to_stderr(cc_err);
            }
        } else {
            if (rf) fclose(rf);
            if (cc_err[0]) drv_replay_to_stderr(cc_err);   /* no retry: show why */
        }
    }
    if (cc_err[0]) unlink(cc_err);
    buf_free(&cc_full);
    if (link_rc != 0 && prelude_split) *retry_whole = true;
    buf_free(&aux_includes);
    buf_free(&aux_sources);
    buf_free(&autolink);
    buf_free(&cmake_flags);
    buf_free(&split_flags);   /* cc_flags may point into this; done with it now */
    /* Leave the stable temp file for ccache; only unlink random fallbacks.
     * Tested against the real prefix rather than a literal "/tmp/tur-build/":
     * on Windows the path starts with a drive letter, so the old check was
     * never true there and every fallback temp file would have been left
     * behind. */
    {
        const char *sp = drv_stable_c_prefix();
        if (tmpl[0] && strncmp(tmpl, sp, strlen(sp)) != 0) {
            unlink(tmpl);
        }
    }
    return link_rc;
}

/* Build a project from multiple .tur files. Generates .h and .c for each,
 * plus a _main.c that includes all headers. */
/* RP0: `shared` selects shared-library build (skip _main.c, link with
 * -shared, default output `lib<dir>.so` -- `<dir>.dll` on Windows, see
 * TUR_SHLIB_PREFIX/TUR_SHLIB_EXT in platform_fs.h). The host can then
 * dlopen the result and dlsym exported defns.
 * RP1: `manifest_path` overrides the default exports.manifest location
 * (`<out_path>.manifest`). NULL means use the default. Ignored unless
 * `shared` is true. */
/* Core multi-file build.  Takes ownership of `tur_files` (frees via
 * free_tur_files) and compiles every file to .h/.c, links them (plus a
 * generated _main.c in executable mode), and writes the ABI cache under
 * `dir`.  `inc`/`n_inc` are include search dirs threaded into every
 * compile_to_h / compile_to_implementation call so cross-module imports
 * resolve; they are borrowed (not freed here). */
/* Mangle a module name for use as a C header/impl base name.
 * '/' -> "__", '-' -> '_', other non-identifier chars -> '_'.
 *
 * Deliberately keeps the LEGACY (lossy) fold for on-disk filenames:
 * injectivity matters for linker-visible C symbols but not for filesystem
 * paths -- and over-long "_hy"/"_xHH" expansions hurt readability.
 * Binding symbols in the generated C use the injective scheme (via
 * raw_name_for_binding / tur_mangle_append); only the header/impl base
 * names produced here stay legacy. See docs/guides/name-mangling-guide.md. */
static void mangle_mod_basename(const char *name, char *out, size_t cap) {
    size_t k = 0;
    for (size_t i = 0; name[i] && k + 2 < cap; i++) {
        char c = name[i];
        if (c == '/') { out[k++] = '_'; out[k++] = '_'; }
        else if (c == '-') { out[k++] = '_'; }
        else if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                 (c >= '0' && c <= '9') || c == '_') { out[k++] = c; }
        else { out[k++] = '_'; }
    }
    out[k] = '\0';
}

/* Derive a module name for one source file.  When `src_root` is non-NULL and
 * the file lives under it, the qualified path relative to src_root (minus
 * `.tur`) is used -- e.g. <root>/src + src/app/util.tur -> "app/util" -- which
 * matches the conventional `(defmodule app/util ...)` name so cross-module
 * imports and the generated header filenames line up.  Otherwise the bare
 * filename basename (minus `.tur`) is used. */
static char *derive_module_name(const char *file, const char *src_root) {
    if (src_root && *src_root) {
        size_t rlen = strlen(src_root);
        if (strncmp(file, src_root, rlen) == 0) {
            const char *rel = file + rlen;
            while (*rel == '/') rel++;
            size_t len = strlen(rel);
            len = drv_source_stem_len(rel, len);
            char *m = (char *)malloc(len + 1);
            memcpy(m, rel, len);
            m[len] = '\0';
            return m;
        }
    }
    const char *base = drv_basename_of(file);
    size_t len = strlen(base);
    len = drv_source_stem_len(base, len);
    char *m = (char *)malloc(len + 1);
    memcpy(m, base, len);
    m[len] = '\0';
    return m;
}

/* Return true if `path` contains a top-level `(defn main` definition (an
 * executable entry point), false otherwise.  Treats a sweet-expr bare
 * `defn main` form as matching too.  Used to auto-detect library mode. */
static bool file_has_main_defn(const char *path) {
    char *src = NULL;
    size_t len = 0;
    if (drv_read_entire_file_quiet(path, &src, &len) != 0) return false;
    bool found = false;
    /* r7rs-lang-plan R9: an R7RS PROGRAM is its top-level forms -- it has no
     * `(defn main`, and the lowering folds its expressions into one.  So a
     * `#lang r7rs` file is an entry point unless it is a library (a
     * `(define-library` at the start of a line).  Without this `tur build .`
     * on a Scheme program produced a shared library and no binary. */
    {
        const char *body = NULL; size_t body_len = 0;
        LangDialect dl = LANG_TURMERIC;
        (void)detect_lang_dialect(src, len, &body, &body_len, NULL, NULL, &dl);
        if (dl == LANG_R7RS) {
            bool is_library = false;
            for (size_t i = 0; i + 15 <= len && !is_library; i++)
                if ((i == 0 || src[i - 1] == '\n') && strncmp(src + i, "(define-library", 15) == 0)
                    is_library = true;
            free(src);
            return !is_library;
        }
    }
    for (size_t i = 0; i + 4 < len && !found; i++) {
        /* Match "(defn main" or "defn main" (sweet-exp) at a word boundary */
        bool sexpr = (strncmp(src + i, "(defn main", 10) == 0);
        bool sweet  = (i == 0 || src[i-1] == '\n') &&
                      strncmp(src + i, "defn main", 9) == 0;
        if (sexpr || sweet) {
            size_t skip = sexpr ? 10 : 9;
            char next = (i + skip < len) ? src[i + skip] : '\0';
            if (next == ' ' || next == '\t' || next == '\n' || next == '[')
                found = true;
        }
    }
    free(src);
    return found;
}

/* Return true if `path` carries a `#[used]` attribute -- a defn explicitly
 * retained with external C linkage because it is reached only through its
 * mangled C symbol (cross-module inline-C bridge or by-address C-ABI callback).
 * The reader admits whitespace between `#[` and the name, so accept that too.
 * Used to disqualify the single-main whole-program build shortcut: that path
 * inlines only the entry module's transitive *Turmeric* import closure, so a
 * sibling module reachable solely via raw extern would be dropped and its
 * symbol would dangle at link time.  Falling through to separate compilation
 * compiles and links every project module instead. */
static bool file_has_used_attr(const char *path) {
    char *src = NULL;
    size_t len = 0;
    if (drv_read_entire_file_quiet(path, &src, &len) != 0) return false;
    bool found = false;
    for (size_t i = 0; i + 6 < len && !found; i++) {
        if (src[i] != '#' || src[i + 1] != '[') continue;
        size_t j = i + 2;
        while (j < len && (src[j] == ' ' || src[j] == '\t')) j++;
        if (j + 4 <= len && strncmp(src + j, "used", 4) == 0) {
            char next = (j + 4 < len) ? src[j + 4] : '\0';
            if (next == ' ' || next == '\t' || next == ']') found = true;
        }
    }
    free(src);
    return found;
}

/* used-attr-whole-program: scan each -I module-search dir for `.tur` modules
 * that carry a #[used] attribute and return their slash-separated module names
 * (path relative to the include dir, sans ".tur").  cmd_build force-loads
 * these on the single-file/whole-program path so a #[used] defn reached only
 * via a raw mangled C symbol (no `(import)`) is still emitted -- the same
 * retention `tur build <project>` gets from its separate-compilation fallback.
 *
 * The entry file's own module is excluded (it is elaborated as the program's
 * top-level forms; force-loading it as a module would double-define it).
 * #[used] is rare, so the common case is a cheap read-and-no-match per `.tur`
 * file; realpath is only paid for the few files that actually match.
 * Heap array; free via free_tur_files. */
char **collect_used_attr_modules(const char *entry_path,
                                 const char **include_dirs,
                                 int n_include_dirs, int *n_out) {
    *n_out = 0;
    if (!include_dirs || n_include_dirs <= 0) return NULL;
    char *entry_real = (entry_path && entry_path[0]) ? realpath(entry_path, NULL)
                                                     : NULL;
    char **names = NULL;
    int n = 0, cap = 0;
    for (int i = 0; i < n_include_dirs; i++) {
        const char *dir = include_dirs[i];
        if (!dir || !*dir) continue;
        char **files = NULL;
        int nf = 0, capf = 0;
        collect_tur_recursive(dir, &files, &nf, &capf);
        size_t dlen = strlen(dir);
        for (int j = 0; j < nf; j++) {
            if (!file_has_used_attr(files[j])) continue;
            /* Exclude the entry's own module (compare canonical paths). */
            if (entry_real) {
                char *fr = realpath(files[j], NULL);
                bool is_entry = (fr && strcmp(fr, entry_real) == 0);
                free(fr);
                if (is_entry) continue;
            }
            /* Module name = path relative to the include dir, minus ".tur". */
            const char *rel = files[j];
            if (strncmp(rel, dir, dlen) == 0 && rel[dlen] == '/')
                rel += dlen + 1;
            else
                rel = drv_basename_of(files[j]);
            size_t rlen = strlen(rel);
            if (rlen > 4) rlen = drv_source_stem_len(rel, rlen);
            if (rlen == 0) continue;
            char *mod = (char *)malloc(rlen + 1);
            if (!mod) { fprintf(stderr, "tur: oom\n"); abort(); }
            memcpy(mod, rel, rlen);
            mod[rlen] = '\0';
            bool dup = false;
            for (int k = 0; k < n; k++)
                if (strcmp(names[k], mod) == 0) { dup = true; break; }
            if (dup) { free(mod); continue; }
            if (n >= cap) {
                cap = cap ? cap * 2 : 4;
                names = (char **)realloc(names, (size_t)cap * sizeof(char *));
                if (!names) { fprintf(stderr, "tur: oom\n"); abort(); }
            }
            names[n++] = mod;
        }
        free_tur_files(files, nf);
    }
    free(entry_real);
    *n_out = n;
    return names;
}

/* n_own: number of files at the front of tur_files that belong to the
 * project itself and should be linked into the output.  Dep modules beyond
 * n_own are compiled only to generate headers (so importers can #include
 * them) but their .c files are excluded from the link step.  Pass n_files
 * (== n_own) when there are no dep-only files. */
/* `tur build --shared --bundle-deps`: link each :spices dep's modules into
 * the library (see cmd_build_project's n_own). */
bool g_build_bundle_deps = false;

/* The per-module borrow specs pass 1 collects, and the arrays holding them.
 * Every exit frees them: an early one used to free only the outer arrays, so a
 * module failing after another had compiled leaked that module's specs, and
 * the Debug tur's LeakSanitizer turned the compile error's exit 1 into 141. */
static void free_borrow_specs(BorrowSpecInfo **all, uint32_t *n_all, int n_files) {
    for (int i = 0; all && n_all && i < n_files; i++) {
        for (uint32_t bi = 0; bi < n_all[i]; bi++) {
            free(all[i][bi].clone_name);
            free(all[i][bi].owning_module);
            free(all[i][bi].fn_symbol);
        }
        free(all[i]);
    }
    free(all);
    free(n_all);
}

static int cmd_build_multi_files(char **tur_files, int n_files,
                                 int n_own, int n_exported,
                                 const char *dir, const char *src_root,
                                 const char **file_src_roots,
                                 const char *out_path,
                                 bool shared, const char *manifest_path,
                                 const char **inc, int n_inc,
                                 const char *build_dir) {
    /* DEDUP-4b: an EXECUTABLE links here, so the rc<T>/GC runtime may come from
     * the archive rather than being replicated into every module TU.
     *
     * A `--shared` build must not: the archive is never injected into a shared
     * library's link line, so declaring-without-defining would leave the .so
     * with undefined rc_cb_alloc / rc_cb_alloc_struct -- a shared object
     * tolerates unresolved symbols at link time and only fails, or silently
     * binds to whatever the host exports, at dlopen.  A .so stays
     * self-contained on the DEDUP-3 owner-TU replica (already one instance per
     * library), which is also the right shape: a dlopened .so carrying its own
     * collector must not half-share one with its host.
     *
     * Caught by build-shared-rc-runtime in tests/run-build-project.sh, which
     * asserts exactly one owning definition of gc_all_blocks in the .so. */
    g_emit_for_link = !shared;
    /* build-output-directory-plan: every intermediate (.c/.h/_main.c/
     * tur_runtime.{c,h}) lands under `<build_dir>/obj/`; the final exe goes
     * to `<build_dir>/bin/<name>` and shared libs to `<build_dir>/lib/lib<name>.so`.
     * build_dir is required: callers must call drv_resolve_build_dir() first. */
    char obj_dir[PATH_MAX + 8];
    snprintf(obj_dir, sizeof(obj_dir), "%s/obj", build_dir);

    char chosen_out[PATH_MAX + 32];
    if (!out_path) {
        /* Prefer the manifest's :name over the directory basename so a
         * workspace member built from `.` gets `lib<pkg-name>.so` rather
         * than `lib<workspace-cwd>.so` -- and so a single-spice build
         * from `.` cannot regress to `lib..so` if drv_default_output_name's
         * realpath fallback ever fails. */
        char manifest_base[1024] = {0};
        {
            char *proj_root = drv_find_project_root(dir);
            if (proj_root) {
                char mp[4096];
                (void)pkg_resolve_manifest_path(proj_root, mp, sizeof(mp));
                PkgManifest mm;
                memset(&mm, 0, sizeof(mm));
                if (pkg_manifest_read(mp, &mm) && mm.name && mm.name[0]) {
                    snprintf(manifest_base, sizeof(manifest_base),
                             "%s", mm.name);
                }
                pkg_manifest_free(&mm);
                free(proj_root);
            }
        }
        char base[1024];
        if (shared) {
            if (manifest_base[0]) {
                snprintf(base, sizeof(base), "%s", manifest_base);
            } else {
                drv_default_output_name(dir, base, sizeof(base));
            }
            snprintf(chosen_out, sizeof(chosen_out),
                     "%s/lib/" TUR_SHLIB_PREFIX "%s" TUR_SHLIB_EXT,
                     build_dir, base);
        } else {
            if (manifest_base[0]) {
                snprintf(base, sizeof(base), "%s", manifest_base);
            } else {
                drv_default_output_name(dir, base, sizeof(base));
            }
            snprintf(chosen_out, sizeof(chosen_out),
                     "%s/bin/%s", build_dir, base);
        }
        out_path = chosen_out;
    }

    /* Allocate arrays for .h and .c filenames */
    char **h_files = (char **)malloc(n_files * sizeof(char *));
    char **c_files = (char **)malloc(n_files * sizeof(char *));
    char **mod_names = (char **)malloc(n_files * sizeof(char *));
    if (!h_files || !c_files || !mod_names) { fprintf(stderr, "tur: oom\n"); return 2; }

    /* RP1: in shared mode, accumulate one exports.manifest line per
     * exported defn across all compiled modules, then write the file
     * once after the link succeeds. NULL in executable mode -- nothing
     * dlopens an executable, so the manifest would be unused. */
    Buf manifest;
    buf_init(&manifest);
    Buf *manifest_ptr = shared ? &manifest : NULL;

    /* Generate module names + the .h/.c basenames they map to.  In project
     * mode (src_root set) the module name is the qualified path under src/
     * (e.g. "app/util") and the generated files are named by its mangled form
     * ("app__util.h") so cross-module `#include "app__util.h"` resolves. */
    for (int i = 0; i < n_files; i++) {
        /* T3: cross-spice dep modules are named relative to their own dep
         * src/ (via file_src_roots[i]) so e.g. sib/api maps to sib__api.h,
         * matching the importer's generated `#include "sib__api.h"`.  Project
         * modules fall back to the single src_root. */
        const char *sr = (file_src_roots && file_src_roots[i])
                             ? file_src_roots[i] : src_root;
        mod_names[i] = derive_module_name(tur_files[i], sr);
        char mangled[512];
        mangle_mod_basename(mod_names[i], mangled, sizeof(mangled));
        size_t mlen = strlen(mangled);
        size_t prefix_len = strlen(obj_dir) + 1;  /* "obj_dir/" */
        size_t need = prefix_len + mlen + 3;
        h_files[i] = (char *)malloc(need);
        c_files[i] = (char *)malloc(need);
        snprintf(h_files[i], need, "%s/%s.h", obj_dir, mangled);
        snprintf(c_files[i], need, "%s/%s.c", obj_dir, mangled);
    }

    /* J6: Two-pass ABI specialization.
     * Pass 1: compile all modules with no forced specs; collect borrow specs.
     * Pass 2: recompile modules that own borrow specs from other modules. */
    BorrowSpecInfo **all_borrow = (BorrowSpecInfo **)calloc(n_files, sizeof(BorrowSpecInfo *));
    uint32_t *all_n_borrow = (uint32_t *)calloc(n_files, sizeof(uint32_t));
    if (!all_borrow || !all_n_borrow) { fprintf(stderr, "tur: oom\n"); return 2; }

    /* Pass 1: Compile each .tur file to .h and .c (no forced specs yet). */
    for (int i = 0; i < n_files; i++) {
        int rm_n = 0;
        char **rm_p = discover_manifest_reader_macros(tur_files[i], &rm_n);

        Buf h_out;
        buf_init(&h_out);
        if (compile_to_h(tur_files[i], &h_out, mod_names[i], inc, n_inc,
                         (const char **)rm_p, rm_n, NULL, 0) != 0) {
            fprintf(stderr, "tur: failed to compile %s to header\n", tur_files[i]);
            buf_free(&h_out);
            free_reader_macro_paths(rm_p, rm_n);
            for (int j = 0; j < n_files; j++) { free(h_files[j]); free(c_files[j]); free(mod_names[j]); }
            free(h_files); free(c_files); free(mod_names);
            free_borrow_specs(all_borrow, all_n_borrow, n_files);
            free_tur_files(tur_files, n_files);
            return 1;
        }
        if (buf_to_path(&h_out, h_files[i]) != 0) {
            fprintf(stderr, "tur: failed to write %s\n", h_files[i]);
            buf_free(&h_out);
            free_reader_macro_paths(rm_p, rm_n);
            for (int j = 0; j < n_files; j++) { free(h_files[j]); free(c_files[j]); free(mod_names[j]); }
            free(h_files); free(c_files); free(mod_names);
            free_borrow_specs(all_borrow, all_n_borrow, n_files);
            free_tur_files(tur_files, n_files);
            return 1;
        }
        buf_free(&h_out);

        Buf c_out;
        buf_init(&c_out);
        if (compile_to_implementation(tur_files[i], &c_out, mod_names[i],
                                      inc, n_inc,
                                      (const char **)rm_p, rm_n,
                                      i < n_exported ? manifest_ptr : NULL,
                                      NULL, 0,
                                      &all_borrow[i], &all_n_borrow[i]) != 0) {
            fprintf(stderr, "tur: failed to compile %s to implementation\n", tur_files[i]);
            buf_free(&c_out);
            free_reader_macro_paths(rm_p, rm_n);
            for (int j = 0; j < n_files; j++) { free(h_files[j]); free(c_files[j]); free(mod_names[j]); }
            free(h_files); free(c_files); free(mod_names);
            free_borrow_specs(all_borrow, all_n_borrow, n_files);
            free_tur_files(tur_files, n_files);
            return 1;
        }
        /* Hoist `__tur_include__` directives to file scope (same treatment
         * as the single-file cmd_build path) -- required for spice modules
         * that include headers with top-level `static inline` declarations
         * such as mbedTLS. */
        hoist_tur_include_directives(&c_out);
        if (buf_to_path(&c_out, c_files[i]) != 0) {
            fprintf(stderr, "tur: failed to write %s\n", c_files[i]);
            buf_free(&c_out);
            free_reader_macro_paths(rm_p, rm_n);
            for (int j = 0; j < n_files; j++) { free(h_files[j]); free(c_files[j]); free(mod_names[j]); }
            free(h_files); free(c_files); free(mod_names);
            free_borrow_specs(all_borrow, all_n_borrow, n_files);
            free_tur_files(tur_files, n_files);
            return 1;
        }
        buf_free(&c_out);
        free_reader_macro_paths(rm_p, rm_n);
    }

    /* J6: Post-pass 1: build forced spec lists for each owner module. */
    ForcedAbiSpec **forced_for = (ForcedAbiSpec **)calloc(n_files, sizeof(ForcedAbiSpec *));
    uint32_t *n_forced_for = (uint32_t *)calloc(n_files, sizeof(uint32_t));
    uint32_t *cap_forced_for = (uint32_t *)calloc(n_files, sizeof(uint32_t));
    if (!forced_for || !n_forced_for || !cap_forced_for) { fprintf(stderr, "tur: oom\n"); return 2; }

    for (int i = 0; i < n_files; i++) {
        for (uint32_t bi = 0; bi < all_n_borrow[i]; bi++) {
            BorrowSpecInfo *bsi = &all_borrow[i][bi];
            if (!bsi->owning_module || !bsi->fn_symbol) continue;
            /* Find the owner module index. */
            int owner = -1;
            for (int j = 0; j < n_files; j++) {
                if (strcmp(mod_names[j], bsi->owning_module) == 0) { owner = j; break; }
            }
            if (owner < 0) continue;
            /* Check if this clone_name is already forced for owner. */
            bool dup = false;
            for (uint32_t fi = 0; fi < n_forced_for[owner]; fi++) {
                if (strcmp(forced_for[owner][fi].clone_name, bsi->clone_name) == 0) {
                    dup = true; break;
                }
            }
            if (dup) continue;
            /* Grow the forced array for the owner. */
            if (n_forced_for[owner] >= cap_forced_for[owner]) {
                uint32_t nc = cap_forced_for[owner] ? cap_forced_for[owner] * 2 : 4;
                ForcedAbiSpec *nf = (ForcedAbiSpec *)realloc(forced_for[owner],
                                                              nc * sizeof(ForcedAbiSpec));
                if (!nf) { fprintf(stderr, "tur: oom\n"); return 2; }
                forced_for[owner] = nf;
                cap_forced_for[owner] = nc;
            }
            ForcedAbiSpec *fs = &forced_for[owner][n_forced_for[owner]++];
            fs->clone_name = bsi->clone_name; /* borrow (valid until all_borrow freed) */
            fs->fn_symbol  = bsi->fn_symbol;
            fs->result_kind = bsi->result_kind;
            fs->n_args = bsi->n_args;
            for (uint8_t ai = 0; ai < bsi->n_args; ai++)
                fs->arg_kinds[ai] = bsi->arg_kinds[ai];
        }
    }

    /* J6: Pass 2: recompile owner modules that have forced specs. */
    for (int i = 0; i < n_files; i++) {
        if (n_forced_for[i] == 0) continue;

        int rm_n = 0;
        char **rm_p = discover_manifest_reader_macros(tur_files[i], &rm_n);

        Buf h_out;
        buf_init(&h_out);
        if (compile_to_h(tur_files[i], &h_out, mod_names[i], inc, n_inc,
                         (const char **)rm_p, rm_n,
                         forced_for[i], n_forced_for[i]) != 0) {
            fprintf(stderr, "tur: failed to recompile %s to header (pass 2)\n", tur_files[i]);
            buf_free(&h_out);
            free_reader_macro_paths(rm_p, rm_n);
            for (int j = 0; j < n_files; j++) { free(h_files[j]); free(c_files[j]); free(mod_names[j]); }
            free(h_files); free(c_files); free(mod_names);
            free(forced_for[i]); free(forced_for); free(n_forced_for); free(cap_forced_for);
            free_borrow_specs(all_borrow, all_n_borrow, n_files);
            free_tur_files(tur_files, n_files);
            return 1;
        }
        if (buf_to_path(&h_out, h_files[i]) != 0) {
            fprintf(stderr, "tur: failed to write %s (pass 2)\n", h_files[i]);
            buf_free(&h_out);
            free_reader_macro_paths(rm_p, rm_n);
            for (int j = 0; j < n_files; j++) { free(h_files[j]); free(c_files[j]); free(mod_names[j]); }
            free(h_files); free(c_files); free(mod_names);
            free(forced_for[i]); free(forced_for); free(n_forced_for); free(cap_forced_for);
            free_borrow_specs(all_borrow, all_n_borrow, n_files);
            free_tur_files(tur_files, n_files);
            return 1;
        }
        buf_free(&h_out);

        Buf c_out;
        buf_init(&c_out);
        if (compile_to_implementation(tur_files[i], &c_out, mod_names[i],
                                      inc, n_inc,
                                      (const char **)rm_p, rm_n,
                                      i < n_exported ? manifest_ptr : NULL,
                                      forced_for[i], n_forced_for[i],
                                      NULL, NULL) != 0) {
            fprintf(stderr, "tur: failed to recompile %s to implementation (pass 2)\n", tur_files[i]);
            buf_free(&c_out);
            free_reader_macro_paths(rm_p, rm_n);
            for (int j = 0; j < n_files; j++) { free(h_files[j]); free(c_files[j]); free(mod_names[j]); }
            free(h_files); free(c_files); free(mod_names);
            free(forced_for[i]); free(forced_for); free(n_forced_for); free(cap_forced_for);
            free_borrow_specs(all_borrow, all_n_borrow, n_files);
            free_tur_files(tur_files, n_files);
            return 1;
        }
        hoist_tur_include_directives(&c_out);
        if (buf_to_path(&c_out, c_files[i]) != 0) {
            fprintf(stderr, "tur: failed to write %s (pass 2)\n", c_files[i]);
            buf_free(&c_out);
            free_reader_macro_paths(rm_p, rm_n);
            for (int j = 0; j < n_files; j++) { free(h_files[j]); free(c_files[j]); free(mod_names[j]); }
            free(h_files); free(c_files); free(mod_names);
            free(forced_for[i]); free(forced_for); free(n_forced_for); free(cap_forced_for);
            free_borrow_specs(all_borrow, all_n_borrow, n_files);
            free_tur_files(tur_files, n_files);
            return 1;
        }
        buf_free(&c_out);
        free_reader_macro_paths(rm_p, rm_n);
    }

    /* J5: Write .tur-abi-cache/index with ownership information.
     * J6: skipped entirely when the cache is disabled (--no-abi-cache /
     * TUR_NO_ABI_CACHE); the build is still correct without it. */
    if (!g_no_abi_cache) {
        /* build-output-directory-plan: .tur-abi-cache/ lives inside the build
         * dir; the dir's own auto-.gitignore (`*`) already covers it, so no
         * separate gitignore append is needed. */
        char cache_dir[PATH_MAX + 32];
        snprintf(cache_dir, sizeof(cache_dir), "%s/.tur-abi-cache", build_dir);
        struct stat st_cache;
        if (stat(cache_dir, &st_cache) != 0) mkdir(cache_dir, 0755);
        char idx_tmp[PATH_MAX + 48];
        snprintf(idx_tmp, sizeof(idx_tmp), "%s/index.tmp", cache_dir);
        FILE *cf = fopen(idx_tmp, "w");
        if (cf) {
            for (int i = 0; i < n_files; i++) {
                for (uint32_t bi = 0; bi < all_n_borrow[i]; bi++) {
                    BorrowSpecInfo *bsi = &all_borrow[i][bi];
                    if (!bsi->owning_module || !bsi->fn_symbol) continue;
                    /* clone_name, owning_module, fn_symbol, result_kind, n_args, arg_kinds... */
                    fprintf(cf, "%s\t%s\t%s\t%d\t%d",
                            bsi->clone_name, bsi->owning_module, bsi->fn_symbol,
                            (int)bsi->result_kind, (int)bsi->n_args);
                    for (uint8_t ai = 0; ai < bsi->n_args; ai++)
                        fprintf(cf, "\t%d", (int)bsi->arg_kinds[ai]);
                    fprintf(cf, "\n");
                }
            }
            fclose(cf);
            char idx_path[PATH_MAX + 48];
            snprintf(idx_path, sizeof(idx_path), "%s/index", cache_dir);
            rename(idx_tmp, idx_path);
        }
    }

    /* Free borrow and forced spec arrays. */
    for (int i = 0; i < n_files; i++) free(forced_for[i]);
    free_borrow_specs(all_borrow, all_n_borrow, n_files);
    free(forced_for); free(n_forced_for); free(cap_forced_for);

    /* build-output-directory-plan: _main.c / tur_runtime.{c,h} all land in
     * `<build_dir>/obj/` next to the per-module .c/.h pairs. */
    char main_c_path[PATH_MAX + 32];
    char rt_c_path[PATH_MAX + 32];
    char rt_h_path[PATH_MAX + 32];
    snprintf(main_c_path, sizeof(main_c_path), "%s/_main.c", obj_dir);
    snprintf(rt_c_path,   sizeof(rt_c_path),   "%s/tur_runtime.c", obj_dir);
    snprintf(rt_h_path,   sizeof(rt_h_path),   "%s/tur_runtime.h", obj_dir);

    /* Generate _main.c (skipped in shared-library mode: the .so has no
     * entry point; the host invokes exported defns directly via dlsym). */
    if (!shared) {
        Buf main_c;
        buf_init(&main_c);
        if (generate_main_c(&main_c, (const char **)h_files, n_files, out_path) != 0) {
            fprintf(stderr, "tur: failed to generate _main.c\n");
            buf_free(&main_c);
            for (int j = 0; j < n_files; j++) { free(h_files[j]); free(c_files[j]); }
            free(h_files); free(c_files);
            free_tur_files(tur_files, n_files);
            return 1;
        }
        if (buf_to_path(&main_c, main_c_path) != 0) {
            fprintf(stderr, "tur: failed to write %s\n", main_c_path);
            buf_free(&main_c);
            for (int j = 0; j < n_files; j++) { free(h_files[j]); free(c_files[j]); }
            free(h_files); free(c_files);
            free_tur_files(tur_files, n_files);
            return 1;
        }
        buf_free(&main_c);
    }

    /* project-mode-rc-runtime-preamble-missing (owner-TU design): every module
     * .h now #includes "tur_runtime.h" for the shared runtime (RcControlBlock,
     * rc_cb_alloc, tur_frame, the GC, ...).  Generate that header plus a single
     * owner TU tur_runtime.c -- it alone #defines TUR_RT_OWNER, so it alone
     * defines the runtime's file-scope globals (one GC registry / free queue /
     * panic + scheduler state) while every module TU carries static replicas of
     * the runtime functions that operate on those shared globals.
     *
     * DEDUP-3 (docs/archive/gc-cycle-collection-plan.md): the rc<T>/GC family is no longer
     * replicated -- its definitions sit inside the same TUR_RT_OWNER guard as
     * the globals, so the owner TU carries the one externally-linked copy of
     * the collector and the other module TUs see only prototypes.
     *
     * Linked into the output below; cleaned up alongside _main.c. */
    {
        Buf rt_h; buf_init(&rt_h);
        resolve_rcgc_from_archive();   /* DEDUP-4b: before emitting the header */
        emit_shared_runtime_header(&rt_h);
        Buf rt_c; buf_init(&rt_c);
        buf_puts(&rt_c, "/* generated by tur -- shared runtime owner TU */\n");
        buf_puts(&rt_c, "#define TUR_RT_OWNER 1\n");
        buf_puts(&rt_c, "#include \"tur_runtime.h\"\n");
        int rt_ok = (buf_to_path(&rt_h, rt_h_path) == 0 &&
                     buf_to_path(&rt_c, rt_c_path) == 0);
        buf_free(&rt_h); buf_free(&rt_c);
        if (!rt_ok) {
            fprintf(stderr, "tur: failed to write tur_runtime.{h,c}\n");
            for (int j = 0; j < n_files; j++) { free(h_files[j]); free(c_files[j]); }
            free(h_files); free(c_files);
            free_tur_files(tur_files, n_files);
            return 1;
        }
    }

    /* Compile everything together with cc */
    const char *cc = getenv("CC");
    if (!cc || !*cc) cc = "cc";

    /* See the note on -fno-strict-aliasing and the GCC-14 warning policy above. */
    const char *cc_flags = getenv("TUR_CC_FLAGS");
    if (!cc_flags || !*cc_flags)
        cc_flags = g_cc_release ? "-O3 -std=c99 -Wall -fno-strict-aliasing"
                                : "-O2 -std=c99 -Wall -fno-strict-aliasing";

    /* Collect cmake dep flags from cmake/spice-deps-manifest.json if present,
     * then the project's own `:build-opts :link-libs` / `:link-flags`.
     *
     * The second half was missing here. This block is a hand-copied twin of
     * the cmake-manifest read inside drv_collect_build_aux, taken before
     * ffi-spices-integration-plan S1 taught THAT function to also honour
     * `:build-opts` -- so the fix landed in one copy and not the other, and
     * `tur build --shared <dir>` silently ignored a spice's declared link
     * libraries while `tur build <file>` honoured them.  Found from the
     * Godot AOT stager on Windows, where the staged library must link against
     * the extension DLL (PE resolves every import at link time; there is no
     * -undefined dynamic_lookup) and the manifest entry it wrote for that
     * never reached the cc line:
     *
     *     ld.exe: undefined reference to `godot_println'
     *
     * Both paths now call the same helper, which is the only arrangement that
     * cannot drift again. */
    Buf cmake_flags;
    buf_init(&cmake_flags);
    {
        char *proj_root = drv_find_project_root(dir);
        if (proj_root) {
            char manifest_path[4096];
            snprintf(manifest_path, sizeof(manifest_path),
                     "%s/cmake/%s", proj_root,
                     pkg_cmake_manifest_name(NULL));
            PkgCmakeManifest cmake_manifest;
            if (pkg_cmake_manifest_read(manifest_path, &cmake_manifest)) {
                pkg_cmake_manifest_append_cc_flags(&cmake_manifest, &cmake_flags);
                pkg_cmake_manifest_free(&cmake_manifest);
            }
            append_manifest_link_flags(proj_root, &cmake_flags);
            free(proj_root);
        }
    }

    /* spices-c-sources-plan: collect the project manifest's :c-includes (as
     * -I flags, visible to both the aux .c compile and the spice's inline-C)
     * and :c-sources (vendored hand-written C, compiled as additional
     * translation units and linked into this binary). Paths are resolved
     * relative to the manifest directory. */
    Buf aux_includes; buf_init(&aux_includes);
    Buf aux_sources;  buf_init(&aux_sources);
    {
        char *proj_root = drv_find_project_root(dir);
        if (proj_root) {
            collect_spice_aux_c(proj_root, &aux_includes, &aux_sources);
            free(proj_root);
        }
    }

    Buf cmd;
    buf_init(&cmd);
    buf_printf(&cmd, "%s %s" TUR_EMITTED_C_CC_FLAGS, cc, cc_flags);
    /* Vendored C include dirs come early so both the generated module .c
     * (carrying inline-C) and the aux .c sources can find their headers. */
    if (aux_includes.len > 0) buf_puts(&cmd, aux_includes.data);
    /* RP0: shared-library link gets -fPIC -shared and skips _main.c.
     * Position-independent code is required on Linux/macOS for symbols
     * loaded via dlopen; -shared tells the driver to emit a .so/.dylib
     * instead of an executable. On macOS, -undefined dynamic_lookup allows
     * cross-library symbols (e.g. httpd in tourist) to resolve at load time. */
    if (shared) {
#ifdef _WIN32
        /* No -fPIC on PE: every Win32 image is position-independent already, so
         * MinGW's gcc answers the flag with a "-fPIC ignored for target"
         * warning on every single shared build.  Suppressing the flag is not a
         * behaviour change -- it is dropping a no-op that only produces noise. */
        buf_puts(&cmd, " -shared");
#else
        buf_puts(&cmd, " -fPIC -shared");
#  ifdef __APPLE__
        buf_puts(&cmd, " -undefined dynamic_lookup");
#  endif
#endif
    }
    bool mm_quoted_ok = true;
    buf_puts(&cmd, " -o ");
    mm_quoted_ok = drv_buf_put_quoted(&cmd, out_path) && mm_quoted_ok;
    /* build-output-directory-plan: every generated .c includes its sibling .h
     * by bare name (`#include "modname.h"`), so the obj dir must be on the
     * include path even when it's not cwd. */
    buf_puts(&cmd, " -I");
    mm_quoted_ok = drv_buf_put_quoted(&cmd, obj_dir) && mm_quoted_ok;
    /* Add _main.c first (executable mode only) */
    if (!shared) {
        buf_putc(&cmd, ' ');
        mm_quoted_ok = drv_buf_put_quoted(&cmd, main_c_path) && mm_quoted_ok;
    }
    /* The shared-runtime owner TU (defines the runtime globals once). */
    buf_putc(&cmd, ' ');
    mm_quoted_ok = drv_buf_put_quoted(&cmd, rt_c_path) && mm_quoted_ok;
    /* Add own .c files only (dep-only files beyond n_own supply headers but
     * are not linked into this output -- they ship as separate libraries). */
    for (int i = 0; i < n_own; i++) {
        buf_putc(&cmd, ' ');
        mm_quoted_ok = drv_buf_put_quoted(&cmd, c_files[i]) && mm_quoted_ok;
    }
    /* spices-c-sources-plan: vendored .c sources compiled alongside the
     * spice's own translation units, before -lm so they can resolve math
     * symbols against it. */
    if (aux_sources.len > 0) buf_puts(&cmd, aux_sources.data);
    buf_free(&aux_includes);
    buf_free(&aux_sources);
    /* Append cmake dep flags (-I/-L/-l). */
    if (cmake_flags.len > 0) buf_puts(&cmd, cmake_flags.data);
    buf_free(&cmake_flags);
    /* Always link libm last. Pure spices can reference libm symbols
     * (sqrt/fabs/sin/cos/...) directly from inline-C, and static spice deps
     * (e.g. plutovg) reference them without carrying -lm themselves. GNU ld
     * resolves archives left-to-right, so -lm must come AFTER any
     * -l<staticlib> flags or the math symbols go unresolved -- hence it is
     * appended last. On Linux an unused -lm is a harmless no-op; on macOS
     * libm lives in libSystem so -lm is a no-op there too. Linking it
     * unconditionally means libm is usable from a pure spice without forcing
     * a fake cmake-dep just to pull it in. */
    /* GCC 14 / Apple clang 15+ promoted -Wincompatible-pointer-types and
     * -Wint-conversion from warnings to hard errors.  The generated C used to
     * trip both -- carrier<->concrete representation straddles (void*<->int64_t)
     * -- tracked under docs/archive/history/codegen-gcc14-permerrors.md and
     * docs/archive/macos-clang-int-conversion-hard-error.md.  Every straddle is
     * now bridged at emit time (String returns, cloneable-frame call args,
     * cps->direct spawn/void* params, closure-env void* fields, and __ps_N
     * binder-init crossings), and the whole fixture tree emits 0
     * -Wint-conversion / -Wincompatible-pointer-types hard errors, so the two
     * downgrades are removed -- a NEW straddle now fails the build (as intended)
     * instead of hiding behind the macOS CI red.
     *
     * -Wno-error=implicit-function-declaration used to be appended here and at
     * the two sibling cc invocations.  It is gone from all three: the two things
     * it was covering are fixed at the source -- the undeclared
     * `tur_hamt_hash_xxh64` call (now declared in the preamble) and the
     * `__tur_include__` hoist emitting code payloads ahead of directive payloads
     * (hoist_tur_include_directives now buckets the two).  An implicit
     * declaration is UB, and suppressing it here is what let a wrong-code bug
     * ride all the way to a second backend on another platform; it now fails the
     * build, as intended.
     *
     * Do NOT re-add it to paper over a new implicit declaration -- fix the
     * declaration.  And do not verify any change in this area with `emit-c`
     * output or a plain `tests/run.sh`: neither exercises the hoist path, and
     * both reported a false all-clear on exactly this question.  Use
     * tools/jit-spike/sweep-turjit.sh.  See findings 21.2/21.3 and
     * docs/archive/history/hoisted-inline-c-precedes-includes.md. */
    /* DEDUP-4b, the project half.  resolve_rcgc_from_archive (called above,
     * before tur_runtime.h was emitted) may have decided that libturt_runtime.a
     * supplies the rc<T>/GC runtime -- and, since regions graduated, the region
     * runtime -- in which case the generated header carries prototypes only
     * and this link line has to actually name the archive.  It never did: the
     * single-file path routes through apply_runtime_lib_mode, but this
     * multi-module link was assembled by hand and stopped at -lm.  That went
     * unnoticed only because the rc<T>/GC references the project fixtures
     * left after -O2 dead-code elimination happened to be none; the region
     * graduation made the gap unconditional, since every module's static init
     * takes the address of tur_region_shutdown for atexit: `undefined
     * reference to tur_region_shutdown` on every multi-module executable
     * build.  Re-probe rather than cache: locate_runtime_lib is a pure
     * filesystem check and this is the same answer the emitter saw.  Ahead of
     * -lm so a static archive member that needs libm still resolves.  Shared
     * builds never take the archive posture (g_emit_for_link is false there)
     * and stay self-contained. */
    if (!shared && emit_rcgc_from_archive()) {
        char rt_libdir[4096], rt_libname[128];
        if (locate_runtime_lib(rt_libdir, sizeof(rt_libdir),
                               rt_libname, sizeof(rt_libname)))
            buf_printf(&cmd, " -l%s -L%s", rt_libname, rt_libdir);
    }
    buf_puts(&cmd, " -lm");
#ifdef _WIN32
    /* The emitted runtime uses pthread_mutex_t/pthread_cond_t and select().
     * On glibc both live in libc; MinGW puts pthreads in winpthreads and
     * select() in Winsock, so they must be linked explicitly.  This is a LINK
     * decision about the host toolchain, not codegen, so keying it off the
     * host is correct -- the generated C itself stays portable. */
    buf_puts(&cmd, " -lpthread -lws2_32 -lshlwapi");
#endif
    /* Ensure null termination before passing to system(). */
    buf_putc(&cmd, '\0');
    if (!mm_quoted_ok) {
        fprintf(stderr, "tur build: a path on the link line was too long to "
                        "quote; refusing to run a truncated cc command\n");
        buf_free(&cmd);
        for (int i = 0; i < n_files; i++) { free(h_files[i]); free(c_files[i]); free(mod_names[i]); }
        free(h_files); free(c_files); free(mod_names);
        free_tur_files(tur_files, n_files);
        return 2;
    }
    if (getenv("TUR_SHOW_CC")) fprintf(stderr, "CC: %s\n", cmd.data);
    int sys_rc = cc_system(cmd.data);
    buf_free(&cmd);

    /* Clean up temp files */
    for (int i = 0; i < n_files; i++) { free(h_files[i]); free(c_files[i]); free(mod_names[i]); }
    free(h_files); free(c_files); free(mod_names);
    free_tur_files(tur_files, n_files);
    /* build-output-directory-plan: leave the intermediates in <build_dir>/obj/
     * so re-runs can ccache them and so users can inspect generated C. The
     * build dir is .gitignore'd on creation, so they don't leak into VCS. */
    (void)main_c_path; (void)rt_c_path; (void)rt_h_path;

    /* Windows: the linker may have appended ".exe" to a -o with no extension.
     * Put the binary back where the caller asked for it.  (Shared builds pass
     * an out_path that already carries an extension, so this is inert there.) */
    if (sys_rc == 0 && tur_settle_exe_output(out_path) != 0) {
        fprintf(stderr, "tur: could not place the linked binary at '%s'\n", out_path);
        return 2;
    }

    if (sys_rc != 0) {
        buf_free(&manifest);
        fprintf(stderr, "tur: cc invocation failed (status %d)\n", sys_rc);
        return 2;
    }

    /* RP1: write exports.manifest alongside the .so. Default location is
     * `<out_path>.manifest` (lives next to the library); --manifest <path>
     * overrides. The host (REPL / FFI dispatcher) reads this file to map
     * `<module>/<defn>` -> mangled C symbol + type signature. */
    if (shared) {
        char default_mp[2048];
        if (!manifest_path) {
            snprintf(default_mp, sizeof(default_mp), "%s.manifest", out_path);
            manifest_path = default_mp;
        }
        FILE *mf = fopen(manifest_path, "wb");
        if (!mf) {
            fprintf(stderr, "tur: cannot open %s for write: %s\n",
                    manifest_path, strerror(errno));
            buf_free(&manifest);
            return 2;
        }
        if (manifest.len > 0 &&
            fwrite(manifest.data, 1, manifest.len, mf) != manifest.len) {
            fprintf(stderr, "tur: failed writing %s\n", manifest_path);
            fclose(mf);
            buf_free(&manifest);
            return 2;
        }
        fclose(mf);
    }
    buf_free(&manifest);
    return 0;
}

/* Bare-directory build: collect every `.tur` under the directory,
 * recursively (collect_tur_files_deep), and build them.  Used for ad-hoc
 * multi-file directories with no build.tur manifest. */
int cmd_build_multi(const char *dir, const char *out_path, bool shared,
                    const char *manifest_path, const char *cli_build_dir) {
    int n_files;
    char **tur_files = collect_tur_files_deep(dir, &n_files);
    if (!tur_files || n_files == 0) {
        fprintf(stderr, "tur: no .tur files found in '%s'\n", dir);
        free_tur_files(tur_files, n_files);
        return 1;
    }
    char *build_dir = drv_resolve_build_dir(dir, cli_build_dir);
    if (!build_dir) { free_tur_files(tur_files, n_files); return 2; }
    /* `dir` is the module root for the files just collected, so put it on the
     * include path: now that the walk is recursive, `src/demo/lib.tur` is in
     * the set, and the sibling that does `(import demo/lib)` has to be able to
     * resolve it.  Finding the files and being unable to link them is only half
     * a fix -- and `tur build src/` is what the `module not found` diagnostic
     * tells the user to run.  Project mode passes its own resolved path here;
     * this is the bare-directory equivalent. */
    const char *self_inc[1] = { dir };
    int rc = cmd_build_multi_files(tur_files, n_files, n_files, n_files, dir, dir, NULL,
                                   out_path, shared, manifest_path, self_inc, 1,
                                   build_dir);
    free(build_dir);
    return rc;
}

/* Manifest-driven project build: `tur build <dir>` where <dir>/build.tur
 * exists.  Collects the project's module files (recursively under `src/`),
 * resolves the include search path from the manifest (own src/ + each
 * `:spices` dep's src/), merges in any user `-I` dirs (which take priority),
 * and builds the whole module set.  Mirrors the project resolution `tur run`
 * and the test runner already perform. */
int cmd_build_project(const char *root_in, const char *out_path,
                      bool shared, const char *manifest_path,
                      const char **user_inc, int n_user_inc,
                      const char *cli_build_dir,
                      const char *target) {
    /* Resolve `root_in` to an absolute path so transitive-dep walking can
     * climb out via `:path "../sibling"` references.  When `tur build .` is
     * run from a spice directory, `root_in` is "." and the dep-resolution
     * fallback walks up cannot reach the actual sibling.  realpath() fixes
     * that with no behavioral change for already-absolute callers. */
    char root_buf[4096];
    const char *root = root_in;
    if (root_in && root_in[0] != '/') {
        if (realpath(root_in, root_buf)) root = root_buf;
    }

    int n_files = 0;
    char **tur_files = collect_project_src_files(root, &n_files);
    if (!tur_files || n_files == 0) {
        fprintf(stderr,
                "tur: no .tur source files found under '%s/src' (or '%s')\n",
                root, root);
        free_tur_files(tur_files, n_files);
        return 1;
    }
    /* Deterministic order so generated artifacts and the ABI cache are
     * reproducible across runs. */
    qsort(tur_files, (size_t)n_files, sizeof(char *), drv_compare_cstr_ptrs);

    /* spices-c-sources-plan: validate the manifest up front. A manifest that
     * fails to parse -- e.g. an invalid :c-sources entry (missing file, wrong
     * extension, absolute path) -- is a hard build error, not a silently
     * ignored read. Doing this here means the parser's diagnostics translate
     * into a failed build (non-zero exit) instead of the build proceeding as
     * though the offending keys were absent. */
    {
        char mpath[4096];
        if (pkg_resolve_manifest_path(root, mpath, sizeof(mpath))) {
            PkgManifest vm; memset(&vm, 0, sizeof(vm));
            bool vok = pkg_manifest_read(mpath, &vm);
            pkg_manifest_free(&vm);
            if (!vok) {
                fprintf(stderr, "tur: invalid manifest '%s'\n", mpath);
                free_tur_files(tur_files, n_files);
                return 1;
            }
        }
    }

    /* Transitive cmake-deps autobuild.  Walks the manifest's `:spices`
     * closure (but NOT every workspace sibling -- see
     * docs/archive/history/tur-build-cmake-deps-workspace-overreach.md) and gen +
     * builds the union of their `:cmake-deps` into `<root>/cmake/`, mirroring
     * what cmd_run does for `tur run`.  Without this, a spice that imports
     * the json modules (which pull in yyjson) generates headers fine but
     * fails at cc with `yyjson.h not found`.  Skipped when
     * `cmake/CMakeLists.txt` already exists so reruns are cheap. */
    {
        char mpath[4096];
        if (pkg_resolve_manifest_path(root, mpath, sizeof(mpath))) {
            PkgManifest dm; memset(&dm, 0, sizeof(dm));
            if (pkg_manifest_read(mpath, &dm)) {
                /* C-3: `tur build` verifies the spice trees it is about to
                 * compile, exactly as `tur run` does.  It never did, which was
                 * the sharper half of the gap: `tur run` interprets, while
                 * this is the path that turns a dependency's source into a
                 * binary you keep and ship. */
                {
                    char lpath[4096];
                    snprintf(lpath, sizeof(lpath), "%s/tur.lock", root);
                    PkgLockFile vlock;
                    memset(&vlock, 0, sizeof(vlock));
                    vlock.format_version = 1;
                    if (pkg_lock_read(lpath, &vlock)) {
                        bool vok = pkg_verify_locked_spices(root, &dm, &vlock,
                                                            "tur build");
                        pkg_lock_free(&vlock);
                        if (!vok) {
                            pkg_manifest_free(&dm);
                            free_tur_files(tur_files, n_files);
                            return 1;
                        }
                    }
                }

                PkgCmakeDep *closure = NULL;
                int n_closure = 0;
                if (pkg_collect_transitive_cmake_deps(
                        root, &dm,
                        /*include_workspace_siblings=*/false,
                        &closure, &n_closure)
                    && n_closure > 0) {
                    /* Keyed on the ARM's own dep manifest, not on
                     * cmake/CMakeLists.txt.  The generated CMakeLists is
                     * shared by both arms, so statting it reported "already
                     * built" for a wasm build whose deps had only ever been
                     * configured natively -- the dep build was skipped and the
                     * link then failed on missing wasm libraries.  The
                     * manifest is the artifact the link step actually
                     * consumes, and there is one per arm. */
                    char cmake_marker[4096];
                    snprintf(cmake_marker, sizeof(cmake_marker), "%s/cmake/%s",
                             root, pkg_cmake_manifest_name(target));
                    struct stat _cmst;
                    bool already_built = (stat(cmake_marker, &_cmst) == 0);
                    if (!already_built) {
                        char lock_path[4096];
                        snprintf(lock_path, sizeof(lock_path),
                                 "%s/tur.lock", root);
                        PkgLockFile lock;
                        memset(&lock, 0, sizeof(lock));
                        lock.format_version = 1;
                        pkg_lock_read(lock_path, &lock);
                        PkgManifest mu = dm;
                        mu.cmake_deps   = closure;
                        mu.n_cmake_deps = n_closure;
                        if (pkg_gen_cmake_deps(root, &mu)
                            && pkg_cmake_build(root, &mu, &lock, target)) {
                            pkg_lock_write(lock_path, &lock);
                        } else {
                            /* The configure may have written the manifest
                             * before the build failed; drop it so the next
                             * build retries rather than skipping the deps
                             * (tur-run-cmake-marker-skips-after-failed-configure). */
                            (void)remove(cmake_marker);
                            fprintf(stderr,
                                "tur build: cmake dependency build failed\n");
                            pkg_cmake_deps_free(closure, n_closure);
                            pkg_lock_free(&lock);
                            pkg_manifest_free(&dm);
                            free_tur_files(tur_files, n_files);
                            return 1;
                        }
                        pkg_lock_free(&lock);
                    }
                }
                pkg_cmake_deps_free(closure, n_closure);
                pkg_manifest_free(&dm);
            }
        }
    }

    /* Auto-detect library mode: if no source file defines a `main` entry
     * point, build as a shared library (.so) rather than an executable.
     * This lets `tur build <spice-root>` work without an explicit --shared
     * flag -- spices are pure libraries and have no main. */
    if (!shared) {
        bool has_main = false;
        for (int i = 0; !has_main && i < n_files; i++)
            has_main = file_has_main_defn(tur_files[i]);
        if (!has_main) shared = true;
    }

    /* src_root used to derive qualified module names ("app/util") from file
     * paths; matches the prefix collect_project_src_files builds paths from. */
    char src_root[4096];
    snprintf(src_root, sizeof(src_root), "%s/src", root);

    /* Validate that every module declared in the manifest's :exports has a
     * backing source file. The build set itself stays the full src/ scan (so
     * internal, non-exported modules still compile); this check only catches
     * manifest/source drift -- a declared export with no file -- and fails
     * loudly rather than silently shipping an incomplete library. */
    {
        char mpath[4096];
        (void)pkg_resolve_manifest_path(root, mpath, sizeof(mpath));
        PkgManifest em;
        if (pkg_manifest_read(mpath, &em)) {
            int missing = 0;
            for (int i = 0; i < em.n_exports; i++) {
                const char *e = em.exports[i];
                size_t el = strlen(e);
                char cand[4096];
                if (el >= 4 && strcmp(e + el - 4, ".tur") == 0)
                    snprintf(cand, sizeof(cand), "%s/%s", root, e);
                else
                    snprintf(cand, sizeof(cand), "%s/%s.tur", src_root, e);
                struct stat es;
                if (stat(cand, &es) != 0 || !S_ISREG(es.st_mode)) {
                    fprintf(stderr,
                            "tur: build.tur declares export '%s' but no source "
                            "file exists at '%s'\n", e, cand);
                    missing++;
                }
            }
            pkg_manifest_free(&em);
            if (missing) {
                free_tur_files(tur_files, n_files);
                return 1;
            }
        }
    }

    const char **proj_inc = NULL;
    int          n_proj_inc = 0;
    resolve_project_include_dirs(root, &proj_inc, &n_proj_inc);

    /* Merge user -I dirs (priority, first) with project-inferred dirs. */
    int n_inc = n_user_inc + n_proj_inc;
    const char **inc = NULL;
    if (n_inc > 0) {
        inc = (const char **)malloc((size_t)n_inc * sizeof(char *));
        if (!inc) {
            fprintf(stderr, "tur: oom\n");
            for (int i = 0; i < n_proj_inc; i++) free((char *)proj_inc[i]);
            free(proj_inc);
            free_tur_files(tur_files, n_files);
            return 2;
        }
        for (int i = 0; i < n_user_inc; i++) inc[i] = user_inc[i];
        for (int i = 0; i < n_proj_inc; i++) inc[n_user_inc + i] = proj_inc[i];
    }

    /* Whole-program executable route (project-mode-rc-runtime-preamble-missing).
     * Separate compilation (cmd_build_multi_files, below) emits one TU per
     * module and omits the inline C runtime, so rc<T>/frame/effects fail to
     * link.  For an executable there is exactly one reachable entry, so we can
     * instead build that entry module *single-file*: compile_to_c -> emit_program
     * inlines every transitively-imported module into one TU carrying the full
     * runtime (the same path `tur build <file>` and the whole test suite
     * exercise).  Shared-library (.so) builds keep separate compilation -- they
     * have no single entry and link their deps separately. */
    if (!shared) {
        const char *entry = NULL;
        int n_main = 0;
        for (int i = 0; i < n_files; i++) {
            if (file_has_main_defn(tur_files[i])) { entry = tur_files[i]; n_main++; }
        }
        /* #[used] disqualifies the shortcut: a non-entry module retained for C
         * linkage (raw-extern bridge / by-address callback) is, by construction,
         * NOT in the entry's Turmeric import closure, so whole-program inlining
         * would drop it and its mangled symbol would dangle.  Fall through to
         * separate compilation, which compiles and links every project module. */
        bool nonentry_used = false;
        if (n_main == 1) {
            for (int i = 0; i < n_files; i++) {
                if (tur_files[i] != entry && file_has_used_attr(tur_files[i])) {
                    nonentry_used = true;
                    break;
                }
            }
        }
        /* Reroute only the unambiguous single-main case; a project with more
         * than one `main` falls through to the existing separate-compilation
         * path unchanged (no behavioral regression). */
        if (n_main == 1 && !nonentry_used) {
            int    n_rm = 0;
            char **rm   = discover_manifest_reader_macros(entry, &n_rm);
            /* build-output-directory-plan: when no -o was given, anchor the
             * executable inside the project's build dir so the single-main
             * fast path matches the separate-compilation default. */
            char *bd = NULL;
            char synth_out[PATH_MAX + 32];
            const char *eff_out = out_path;
            if (!out_path) {
                bd = drv_resolve_build_dir(root, cli_build_dir);
                if (bd) {
                    char base[1024];
                    char mp[4096];
                    (void)pkg_resolve_manifest_path(root, mp, sizeof(mp));
                    PkgManifest mm; memset(&mm, 0, sizeof(mm));
                    if (pkg_manifest_read(mp, &mm) && mm.name && mm.name[0]) {
                        snprintf(base, sizeof(base), "%s", mm.name);
                    } else {
                        drv_default_output_name(root, base, sizeof(base));
                    }
                    pkg_manifest_free(&mm);
                    snprintf(synth_out, sizeof(synth_out),
                             "%s/bin/%s", bd, base);
                    eff_out = synth_out;
                }
            }
            int rc = cmd_build(entry, eff_out, inc, n_inc, NULL,
                               (const char **)rm, n_rm);
            free(bd);
            free_reader_macro_paths(rm, n_rm);
            free(inc);
            for (int i = 0; i < n_proj_inc; i++) free((char *)proj_inc[i]);
            free(proj_inc);
            free_tur_files(tur_files, n_files);
            return rc;
        }
    }

    /* T3: pull each resolved :spices dep's modules into the build set so a
     * cross-spice `(import dep/mod)` links under separate compilation.  The
     * include path (proj_inc) already lets the importer type-check against the
     * dep's source, but without also compiling+linking the dep's modules the
     * generated `#include "dep__mod.h"` has no backing file and cc fails.
     *
     * Dep modules are named relative to their own dep `src/` (via the per-file
     * src-root array) so e.g. `sib/api` maps to `sib__api.h`, matching the
     * importer's emitted include.  A dep module whose qualified name collides
     * with one already in the set (a project module, or an earlier dep) is
     * skipped, keeping the project's own copy.
     *
     * Skipped in shared-library mode: a `.so` links its deps separately and
     * must not absorb their modules or accumulate their exports into the
     * library's manifest.  Transitive deps-of-deps are out of scope (the
     * resolved include path only covers this project's direct `:spices`). */
    char       **all_files = tur_files;  /* realloc'd as dep modules append */
    int          n_all     = n_files;
    int          cap_all   = n_files;
    const char **all_roots = (const char **)malloc(
        (size_t)(cap_all > 0 ? cap_all : 1) * sizeof(char *));
    const char **dep_dirs  = NULL;
    int          n_dep_dirs = 0;
    if (!all_roots) {
        fprintf(stderr, "tur: oom\n");
        free(inc);
        for (int i = 0; i < n_proj_inc; i++) free((char *)proj_inc[i]);
        free(proj_inc);
        free_tur_files(tur_files, n_files);
        return 2;
    }
    for (int i = 0; i < n_files; i++) all_roots[i] = src_root;

    /* Always collect dep modules: in executable mode they are linked in;
     * in shared-library mode they are compiled to headers only (so the
     * generated #include "dep__mod.h" in the project's C files resolves)
     * but their .c files are NOT linked into the output -- n_files tracks
     * the boundary.  Transitive deps-of-deps remain out of scope. */
    {
        char mpath[4096];
        (void)pkg_resolve_manifest_path(root, mpath, sizeof(mpath));
        PkgManifest dm;
        if (pkg_manifest_read(mpath, &dm)) {
            resolve_include_dirs_from_manifest(root, &dm, /*include_own_src=*/false,
                                               &dep_dirs, &n_dep_dirs);
            pkg_manifest_free(&dm);
        }

        /* seen-set of qualified module names already in the build set, seeded
         * with the project's own modules so dep modules can't shadow them. */
        int    n_seen = 0, cap_seen = n_files > 0 ? n_files : 1;
        char **seen = (char **)malloc((size_t)cap_seen * sizeof(char *));
        if (seen) {
            for (int i = 0; i < n_files; i++)
                seen[n_seen++] = derive_module_name(tur_files[i], src_root);
        }

        for (int k = 0; seen && k < n_dep_dirs; k++) {
            char **df = NULL; int dn = 0, dc = 0;
            collect_tur_recursive(dep_dirs[k], &df, &dn, &dc);
            qsort(df, (size_t)dn, sizeof(char *), drv_compare_cstr_ptrs);
            for (int j = 0; j < dn; j++) {
                char *mn = derive_module_name(df[j], dep_dirs[k]);
                bool dup = false;
                for (int s = 0; s < n_seen; s++)
                    if (strcmp(seen[s], mn) == 0) { dup = true; break; }
                if (dup) { free(mn); free(df[j]); continue; }
                if (n_seen >= cap_seen) {
                    cap_seen *= 2;
                    char **ns = (char **)realloc(seen, (size_t)cap_seen * sizeof(char *));
                    if (!ns) { free(mn); free(df[j]); continue; }
                    seen = ns;
                }
                seen[n_seen++] = mn;
                if (n_all >= cap_all) {
                    cap_all = cap_all > 0 ? cap_all * 2 : 8;
                    all_files = (char **)realloc(all_files,
                                                 (size_t)cap_all * sizeof(char *));
                    all_roots = (const char **)realloc(all_roots,
                                                 (size_t)cap_all * sizeof(char *));
                }
                all_files[n_all]  = df[j];        /* transfer ownership */
                all_roots[n_all]  = dep_dirs[k];  /* borrowed until build returns */
                n_all++;
            }
            free(df);  /* shell only; strings transferred or freed above */
        }
        for (int s = 0; s < n_seen; s++) free(seen[s]);
        free(seen);
    }

    /* n_own = project's own modules (link these); n_all - n_own = dep modules
     * compiled only for header generation (shared mode skips their link).
     * --bundle-deps links them into the library too -- the self-contained
     * image `tur repl` and a notebook dlopen, which have no other copy of a
     * dep's code to resolve against.  Either way only the project's own
     * modules are the library's exports: a dep's are not in its manifest. */
    int n_own = (shared && !g_build_bundle_deps) ? n_files : n_all;
    char *build_dir = drv_resolve_build_dir(root, cli_build_dir);
    int rc;
    if (!build_dir) {
        rc = 2;
    } else {
        rc = cmd_build_multi_files(all_files, n_all, n_own, n_files, root, src_root,
                                   all_roots, out_path, shared, manifest_path,
                                   inc, n_inc, build_dir);
        free(build_dir);
    }

    free(all_roots);
    for (int k = 0; k < n_dep_dirs; k++) free((char *)dep_dirs[k]);
    free(dep_dirs);
    free(inc);
    for (int i = 0; i < n_proj_inc; i++) free((char *)proj_inc[i]);
    free(proj_inc);
    return rc;
}

/* drop-x-flags-plan: every -X<name> below is an accept-and-warn no-op as
 * of v0.24.0.  The flags are still recognized for one full minor line so
 * downstream build.tur files keep compiling unchanged; each one emits
 * TUR-W0050 instead of enabling anything.  The features themselves are
 * unconditionally on (see src/runtime/globals.c). */
bool is_known_deprecated_x_flag(const char *s) {
    static const char *const names[] = {
        "-Xlinear", "-Xsubstructural", "-Xunique-types", "-Xgadt",
        "-Xunion-types", "-Xintersection-types", "-Xeffect-types",
        "-Xcontracts", "-Xsessions", "-Xdynamic-vars", "-Xcallcc",
        "-Xsized-types", "-Xdata-literals", "-Xjson-reader",
        "-Xschema-reader", "-Xsymbols",
    };
    for (size_t k = 0; k < sizeof(names) / sizeof(names[0]); k++) {
        if (strcmp(s, names[k]) == 0) return true;
    }
    return false;
}

void warn_deprecated_x_flag(const char *name) {
    fprintf(stderr, "warning [TUR-W0050]: %s is no longer needed; "
                    "the feature is on by default\n", name);
}

/* XF1 (experimental-flag-mechanism-plan): enable each comma-separated name in
 * a `--enable=<a,b,c>` value (or a manifest :experiments list).  An unknown
 * name is a hard error (TUR-E0310) so typos surface immediately.  Returns
 * true on success, false (after printing TUR-E0310) on the first unknown
 * name. `src` records the origin for the `tur experiments` source column. */
bool enable_experiment_list(const char *list, ExperimentSource src) {
    if (!list || !*list) return true;
    char copy[1024];
    strncpy(copy, list, sizeof(copy) - 1);
    copy[sizeof(copy) - 1] = '\0';
    char *tok = strtok(copy, ",");
    while (tok) {
        /* trim leading whitespace */
        while (*tok == ' ' || *tok == '\t') tok++;
        if (*tok) {
            if (!experiment_enable(tok, src)) {
                fprintf(stderr,
                        "error [TUR-E0310]: unknown experiment '%s'; "
                        "run 'tur experiments' for the list\n", tok);
                return false;
            }
        }
        tok = strtok(NULL, ",");
    }
    return true;
}

/* profiling-plan C0+C1: does `--timings <arg>` mean <arg> is the JSON
 * destination?  The two-token form cannot otherwise be told apart from
 * `--timings <input-file>`, so it takes the next argument only when that is
 * unmistakably an output path: one ending in `.json`, or a /dev/ node
 * (/dev/stdout, /dev/stderr, /dev/fd/N).  `--timings=<path>` takes any path,
 * and `--timings=-` means stdout. */
static bool timings_path_like(const char *a) {
    if (!a || !*a) return false;
    if (strncmp(a, "/dev/", 5) == 0) return true;
    size_t n = strlen(a);
    return n > 5 && strcmp(a + n - 5, ".json") == 0;
}

/* --timings / --timings=<path> / --timings <path>: start the zone timer and
 * strip the flag (and its path) from argv.  On any command -- the zones are
 * where the compiler is, so a command that compiles nothing reports "other".
 * Stops at `--`: what follows is the program's own argv. */
static void parse_timings_flag(int *argc_io, char **argv) {
    int argc = *argc_io;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--") == 0) break;
        int take = 0;
        const char *path = NULL;
        if (strcmp(argv[i], "--timings") == 0) {
            take = 1;
            if (i + 1 < argc && timings_path_like(argv[i + 1])) {
                path = argv[i + 1];
                take = 2;
            }
        } else if (strncmp(argv[i], "--timings=", 10) == 0) {
            take = 1;
            path = argv[i] + 10;
        }
        if (take == 0) continue;
        prof_start(argc, argv, path);   /* the command as typed, flag and all */
        for (int j = i; j + take < argc; j++) argv[j] = argv[j + take];
        argc -= take;
        i--;
    }
    *argc_io = argc;
}

/* Top-level subcommands, in canonical spelling. Kept in one place so
 * resolve_command() can prefix-match against them; the actual dispatch
 * chain in main() still uses strcmp against these same names. */
static const char *const CANONICAL_COMMANDS[] = {
    "emit-c", "emit-h", "emit-cmake", "check", "expand", "audit-spans",
    "lsp", "mcp", "dap", "trace", "lsp-lite",
    "build", "compile", "link", "run", "repl", "worker", "interpret", "debug",
    "eval", "doc", "docs", "image-info", "image-verify", "explain",
    "format", "fmt", "parse-check", "test", "demangle",
    "new", "init", "add", "add-cmake", "fetch", "audit",
    "install", "uninstall", "list", "upgrade", "experiments", "dialects",
    "smt",
    NULL,
};

/* Built-in subcommands that dispatch on their exact spelling only.  They stay
 * out of CANONICAL_COMMANDS[] because prefix resolution would change under
 * them: `completion` would make `tur comp` (today `compile`) ambiguous, and
 * `jit` would do the same to `tur j` (today the `just` alias of `run`).
 * `emit-rt-split` is the generator tools/gen-runtime-split.py runs, not a
 * command anyone types.  They still shadow a `tur-<name>` on $PATH. */
static const char *const HIDDEN_COMMANDS[] = {
    "jit", "completion", "emit-rt-split",
    NULL,
};

/* Alternate spellings that dispatch to a canonical command.  `tur just` is a
 * synonym for `tur run`: the task-runner mode reads a Justfile, so `just` is
 * the name muscle memory reaches for.  Aliases participate in the same
 * prefix resolution as canonical names, and resolve straight to the
 * canonical spelling so every downstream dispatch arm sees one name. */
static const struct { const char *alias; const char *canonical; }
COMMAND_ALIASES[] = {
    { "just", "run" },
    { NULL,   NULL  },
};

/* Sentinel: `tok` is a prefix of more than one canonical command. */
static const char *const COMMAND_AMBIGUOUS = (const char *)(intptr_t)-1;

/* True when `name` is a built-in subcommand in any spelling dispatch accepts:
 * canonical, hidden, or an alias. */
static bool is_builtin_command(const char *name) {
    for (int i = 0; CANONICAL_COMMANDS[i] != NULL; i++)
        if (strcmp(name, CANONICAL_COMMANDS[i]) == 0) return true;
    for (int i = 0; HIDDEN_COMMANDS[i] != NULL; i++)
        if (strcmp(name, HIDDEN_COMMANDS[i]) == 0) return true;
    for (int i = 0; COMMAND_ALIASES[i].alias != NULL; i++)
        if (strcmp(name, COMMAND_ALIASES[i].alias) == 0) return true;
    return false;
}

/* GS-M2: scan $PATH for `tur-*` executables and print them as an
 * "External commands:" block beneath the built-in usage. Built-in
 * subcommands always win at dispatch time, so we filter any `tur-<name>`
 * that shadows a built-in to avoid misleading the user. Within $PATH,
 * the first hit for each name wins (matching exec() resolution). */
static void list_external_subcommands(void) {
    const char *path_env = getenv("PATH");
    if (!path_env || !*path_env) return;

    char **names = NULL;
    int n_names = 0, cap_names = 0;

    const char *p = path_env;
    while (*p) {
        const char *colon = strchr(p, ':');
        size_t seg = colon ? (size_t)(colon - p) : strlen(p);
        if (seg > 0 && seg < 3500) {
            char dir[4096];
            snprintf(dir, sizeof(dir), "%.*s", (int)seg, p);
            DIR *d = opendir(dir);
            if (d) {
                struct dirent *de;
                while ((de = readdir(d)) != NULL) {
                    if (strncmp(de->d_name, "tur-", 4) != 0) continue;
                    const char *cmd = de->d_name + 4;
                    if (!*cmd) continue;
                    for (const char *q = cmd; *q; q++) {
                        if (!(isalnum((unsigned char)*q) || *q == '-' || *q == '_'))
                            goto skip;
                    }
                    if (is_builtin_command(cmd)) continue;
                    {
                        char full[8192];
                        snprintf(full, sizeof(full), "%s/%s", dir, de->d_name);
                        if (access(full, X_OK) != 0) continue;
                    }
                    {
                        bool dup = false;
                        for (int i = 0; i < n_names; i++) {
                            if (strcmp(names[i], cmd) == 0) { dup = true; break; }
                        }
                        if (dup) continue;
                    }
                    if (n_names == cap_names) {
                        int nc = cap_names ? cap_names * 2 : 8;
                        char **nn = (char **)realloc(names, (size_t)nc * sizeof(char *));
                        if (!nn) { closedir(d); goto cleanup; }
                        names = nn;
                        cap_names = nc;
                    }
                    names[n_names++] = strdup(cmd);
                  skip: ;
                }
                closedir(d);
            }
        }
        if (!colon) break;
        p = colon + 1;
    }

    if (n_names == 0) goto cleanup;

    for (int i = 1; i < n_names; i++) {
        char *cur = names[i];
        int j = i;
        while (j > 0 && strcmp(names[j - 1], cur) > 0) {
            names[j] = names[j - 1];
            j--;
        }
        names[j] = cur;
    }

    fprintf(stderr, "\nexternal commands (tur-* on $PATH):\n");
    for (int i = 0; i < n_names; i++)
        fprintf(stderr, "  tur %s\n", names[i]);

cleanup:
    for (int i = 0; i < n_names; i++) free(names[i]);
    free(names);
}

/* Resolve a bare argv[1] token to a canonical command name.
 *   - exact match wins outright.
 *   - unique prefix -> canonical.
 *   - >1 prefix hits -> COMMAND_AMBIGUOUS.
 *   - flags (leading '-') and 0 hits -> NULL. */
static const char *resolve_command(const char *tok) {
    if (!tok || !*tok || tok[0] == '-') return NULL;
    for (int i = 0; CANONICAL_COMMANDS[i] != NULL; i++) {
        if (strcmp(tok, CANONICAL_COMMANDS[i]) == 0) return CANONICAL_COMMANDS[i];
    }
    for (int i = 0; COMMAND_ALIASES[i].alias != NULL; i++) {
        if (strcmp(tok, COMMAND_ALIASES[i].alias) == 0)
            return COMMAND_ALIASES[i].canonical;
    }
    size_t n = strlen(tok);
    const char *hit = NULL;
    for (int i = 0; CANONICAL_COMMANDS[i] != NULL; i++) {
        if (strncmp(tok, CANONICAL_COMMANDS[i], n) == 0) {
            if (hit != NULL) return COMMAND_AMBIGUOUS;
            hit = CANONICAL_COMMANDS[i];
        }
    }
    for (int i = 0; COMMAND_ALIASES[i].alias != NULL; i++) {
        if (strncmp(tok, COMMAND_ALIASES[i].alias, n) == 0) {
            /* An alias and its own canonical name are not two commands, so
             * `tur ru` stays `run` rather than becoming ambiguous. */
            if (hit != NULL && strcmp(hit, COMMAND_ALIASES[i].canonical) != 0)
                return COMMAND_AMBIGUOUS;
            hit = COMMAND_ALIASES[i].canonical;
        }
    }
    return hit;
}

static int usage(void) {
    fprintf(stderr,
        "tur: the Turmeric compiler (v" TUR_VERSION ")\n"
        "\n"
        "usage:\n"
        "  tur build <file.tur> [-o <out>]    build a single file\n"
        "  tur build <dir> [-o <out>]         build all .tur files in directory\n"
        "  tur compile <file.tur> -o <out.o>  lower a .tur to an object (+ .link sidecar)\n"
        "  tur link <obj/src>... -o <out>     link objects (+ .link sidecars) into an exe\n"
        "  tur emit-c <input.tur>            print the generated C to stdout\n"
        "  tur emit-h <input.tur>            print the generated header to stdout\n"
        "  tur run <input.tur>               build + execute a single file\n"
        "  tur run <recipe>                  run a Justfile recipe (`tur just` is a synonym)\n"
        "  tur repl                          interactive REPL (Phase S1)\n"
        "  tur worker                        persistent fixture evaluator (Tier 3, reads dirs from stdin)\n"
        "  tur interpret [-I dir] <file.tur> run a file through the tree-walking interpreter\n"
        "  tur debug [-I dir] <file.tur>     run a file under the interactive debugger\n"
        "  tur dap                           Debug Adapter Protocol server (JSON-RPC/stdio) for editors\n"
        "  tur trace <file.tur> [-o f]       record an interpreted run; --dump reads one back\n"
        "  tur lsp-lite                      lightweight completion/calltip/doc backend (NDJSON/stdio)\n"
        "  tur eval '<expr>'                 evaluate an inline expression\n"
        "  tur doc <symbol>                  print documentation for a builtin, special form, or stdlib definition\n"
        "  tur docs [--open|--serve]         locate, open, or serve the rendered guides and API reference\n"
        "  tur explain <TUR-E####|snippet>   explain a diagnostic code or snippet errors\n"
        "  tur test <dir>                    run all .tur files in a directory\n"
        "  tur check <input.tur>             type-check only, no codegen (phase 8)\n"
        "  tur expand <input.tur>            trace macro expansions to stdout while checking\n"
        "  tur format [--check|--diff] [file.tur]   format source (stdin if no file given)\n"
        "  tur fmt [--check|--diff|--dry-run] [paths...]  format in place with dir walking\n"
        "  tur parse-check <a> <b>           exit 0 if both files read to the same AST\n"
        "  tur demangle [<name>...]          decode mangled C symbols back to Turmeric names\n"
        "                                    (filters stdin when given no names, so\n"
        "                                     `perf script | tur demangle` reads clean)\n"
        "  tur smt <file.smt2>               run an SMT-LIB2 script through the refinement solver\n"
        "  tur check --dump-refine=json <f>  print one JSON record per refinement obligation\n"
        "  tur experiments                   list experimental features (--enable=<name>)\n"
        "  tur dialects                      list the base dialects a `#lang` line may name\n"
        "  tur completion <zsh|bash>         print a shell completion script\n"
        "\n"
        "package management (Spice, Phase PKG-1):\n"
        "  tur init [--bin|--lib] <name>     create a new project\n"
        "  tur add <url> [--ref <tag>]       add a Turmeric spice\n"
        "  tur add <path> --path             add a local spice\n"
        "  tur add --workspace <name>        assert a workspace sibling (no manifest entry)\n"
        "  tur add-cmake <url> [--ref <tag>] add a C/CMake dependency\n"
        "  tur fetch [--update|--frozen|--dry-run|--refetch]  download/update spices (--frozen: fail if tur.lock would change; --refetch bypasses system pkgs)\n"
        "  tur audit                         list every origin the build fetches code from\n"
        "  tur emit-cmake [--output-dir <d>] generate CMakeLists.txt + config for CMake consumers\n"
        "  tur install <url> [--ref <ref>]   install a spice binary globally\n"
        "  tur install <path> --path         install a local spice binary globally\n"
        "  tur uninstall <name>              remove a globally installed spice\n"
        "  tur list [--verbose|--outdated]   list globally installed spices\n"
        "  tur upgrade <name>...|--all       upgrade globally installed spices\n");
    fprintf(stderr,
        "\n"
        "emit-c flags:\n"
        "  tur emit-c <file>                 compile a .tur file to C (stdout)\n"
        "  tur emit-c --output-dir <dir> <files...>  compile each .tur to <dir>/<mod>.h + .c\n"
        "\n"
        "global flags:\n"
        "  --enable=<name>[,<name>...]      turn on an experimental feature; see 'tur experiments'\n"
        "  --macro-caps=io                  grant macro-time (defmacro*) code I/O; default deny\n"
        "  --no-proc-macros                 refuse defmacro* instead of running it (untrusted trees)\n"
        "  --no-color                       disable colored diagnostics\n"
        "  --json                           structured JSON output (tur doc, tur test, tur check)\n"
        "  --json-diagnostics               output diagnostics as JSON (phase 8)\n"
        "  --explain <TUR-E####>            print explanation for a diagnostic code (HKT-P5)\n"
        "  --explain <snippet>              compile code snippet and explain errors (phase 8)\n"
        "  --timings[=<path>]               time each compiler phase (read, elaborate, emit, cc, ...):\n"
        "                                   a table on stderr; with a path, tur-profile/1 JSON there too\n"
        "                                   (`--timings <path>` when it ends in .json or is /dev/...)\n"
        "  --dump-kinds                     dump kind annotations after kind-check (HKT-P6)\n"
        "  --strict-effects                 warn on unannotated effectful functions (ER1)\n"
        "  -Werror=strict-effects           make the --strict-effects warnings errors (implies it)\n"
        "  --strict-refine                  hard-fail refinement obligations the solver cannot prove\n"
        "  --dump-effects                   print inferred effect row for each defn (ER6)\n"
        "  --dump-write-frames              print the checked verdict for each `#writes` frame (G1)\n"
        "  --dump-read-frames               print the verification verdict for each `#reads` frame (R4)\n"
        "  --dump-cps-coloring              print whole-program may-capture coloring per defn (CPS1)\n"
        "  --dump-cps                       print the ANF/CPS IR for each colored defn (CPS2)\n"
        "  --lint-effects                   deprecated alias for --strict-effects (TUR-W0050)\n"
        "  --backtrack-depth <N>            cap run-backtrack at N results (0=unlimited) (Phase B5)\n"
        "  --dump-clone-plan                dump cloneable capture plan after CPS (Phase B5)\n"
        "  --dump-cps-coloring              dump CPS coloring (colored/uncolored) per top-level defn (CPS1)\n"
        "  --cps-path                       emit CPS wrappers for colored functions (CPS3)\n"
        "  --emit-abi-trace                 print the resolved ABI path per call site during emit-c (Phase I)\n"
        "  --no-abi-cache                   disable the persistent cross-module ABI cache (.tur-abi-cache/) (Phase J6)\n"
        "  --no-r7rs-gc                     build a #lang r7rs program without its collector (TUR_R7RS_GC=0; for a program that starts threads)\n"
        "  --no-saffron-gc                  build a #lang saffron program without the collector (TUR_SAFFRON_GC=0)\n"
        "  --panic-abort                   all panics call abort() directly (Phase R5)\n"
        "  --warn-unused-result             warn on discarded result values (Phase R6)\n"
        "  --no-warn-unused-result          disable --warn-unused-result (Phase R6)\n"
        "  --lint-panic                     lint panic/must! usage (Phase R6)\n"
        "  --no-contracts                   strip contract checks; predicates not evaluated (Phase C2)\n"
        "  --dump-sizes                     print inferred size index per sized-GADT constructor (SZ8)\n"
        "  --keep-contracts                 retain contract checks in release builds (CT3)\n"
        "  -X<name>                         recognized for backwards compatibility; all language\n"
        "                                   features previously gated by -X flags are now on by default.\n"
        "                                   Each -X<name> emits TUR-W0050 and is otherwise ignored.\n"
        "                                   See docs/guides/compiler-flags-guide.md for the removal list.\n");
    list_external_subcommands();
    return 64;
}

/* E1: per-subcommand help strings */
int usage_build(void) {
    fprintf(stderr,
        "usage:\n"
        "  tur build [-I <dir>...] <file.tur> [-o <out>]   build a single file\n"
        "  tur build <dir> [-o <out>]                       build all .tur in dir\n"
        "  tur build --shared <dir> [-o <out>] [--manifest <p>]  build a shared library\n"
        "  tur compile [-I <dir>...] <file.tur> -o <out.o>  lower a .tur to an object + .link\n"
        "  tur link [--shared] <obj/src>... -o <out>        link objects (+ .link) into an exe/.so\n"
        "  tur emit-c [-I <dir>...] <file.tur>              emit C to stdout\n"
        "  tur emit-c [-I <dir>...] --build-dir <dir> <files...>  emit per-module .h/.c\n"
        "  tur emit-h [-I <dir>...] <file.tur>              emit header to stdout\n"
        "\n"
        "flags:\n"
        "  -o <out>          output file path\n"
        "  -I <dir>          add include directory for module resolution\n"
        "                    (repeat to add multiple; intra-spice imports usually\n"
        "                    want `-I src` from the spice root)\n"
        "  --release         compile at -O3 (the default is -O2; TUR_CC_FLAGS\n"
        "                    replaces either)\n"
        "  -B, --build-dir <d>  route generated .c/.h/.o + final artifact into <d>\n"
        "                    (subdirs: obj/, bin/, lib/). Defaults to\n"
        "                    <project-root>/build or <cwd>/build. Override layers:\n"
        "                    CLI flag > TUR_BUILD_DIR env > build.tur :build-dir.\n"
        "  --shared          build a shared library (`-fPIC -shared`, no main;\n"
        "                    Windows drops the -fPIC, which PE ignores anyway).\n"
        "                    Requires a directory argument. Exported defns are\n"
        "                    callable via dlopen/dlsym as `<module>__<name>`.\n"
        "                    Default output is <build-dir>/lib/lib<name>.so, or\n"
        "                    <name>.dll on Windows.\n"
        "  --split-build     build a single file as `compile` + `link` (cacheable\n"
        "                    `cc -c` object compiles + a link). Native builds only.\n"
        "  --no-split-build  force the monolithic single-`cc` build (the default).\n"
        "  --runtime=auto    (default) link the lean non-ASan libturt_runtime.a when\n"
        "                    locatable, else recompile the bare src/runtime autolink\n"
        "                    sources -- never links the ASan libturi.a on its own, so\n"
        "                    a default build matches the old source path.\n"
        "  --runtime=lib     force the archive link (lean preferred, else libturi.a).\n"
        "                    Set TUR_RUNTIME_LIB to point at the archive if not found.\n"
        "  --runtime=source  force recompiling the runtime sources.\n"
        "  --runtime=split   insist on the preamble split that auto takes on Linux\n"
        "                    and Windows when libturt_preamble.a is present: the\n"
        "                    fixed runtime preamble is linked, not recompiled per\n"
        "                    program.  TUR_PREAMBLE_SPLIT=0 keeps it inline.\n"
        "                    TUR_RUNTIME=auto|lib|source|split seeds the default.\n"
        "  --link-flags <f>  (tur link) extra linker flags, e.g. \"-L<dir> -lfoo\"\n"
        "  --manifest <p>    (with --shared) write exports.manifest to <p>\n"
        "                    (defaults to `<out>.manifest`). Lists each export\n"
        "                    as `<mod>/<defn> -> <mangled> :: (:args) -> :ret`.\n"
        "  --bundle-deps     (with --shared, on a project) link each :spices dep's\n"
        "                    modules into the library as well, so it loads with\n"
        "                    nothing else beside it; the manifest still lists only\n"
        "                    the project's own exports.\n"
        "  --target wasm     compile to WebAssembly via emcc (requires Emscripten)\n"
        "  --debug           emit `#line` directives mapping the generated C back\n"
        "                    to `.tur` source, and compile single-file builds with\n"
        "                    `-g -O0` so gdb/lldb step through Turmeric source.\n"
        "  --timings[=<p>]   print where the compile's time went, per phase\n"
        "                    (exclusive time; phases sum to the wall clock), and\n"
        "                    with <p> write it as tur-profile/1 JSON.  Also on\n"
        "                    check, emit-c, run and jit.  Profile a Release tur.\n"
        "\n"
        "Try 'tur --help' for global options.\n");
    return 0;
}

int usage_run(void) {
    fprintf(stderr,
        "usage:\n"
        "  tur run [-I <dir>...] <file.tur> [-- <args...>]\n"
        "                                  build and execute a single file\n"
        "  tur run [-I <dir>...] - [-- <args...>]\n"
        "                                  read source from stdin, build and execute\n"
        "\n"
        "flags:\n"
        "  -I <dir>    add an include directory for module resolution\n"
        "  --release   compile at -O3 (the default is -O2)\n"
        "  --offline   skip dependency fetch\n"
        "  --          pass remaining arguments to the program\n"
        "\n"
        "Try 'tur --help' for global options.\n");
    return 0;
}

int usage_expand(void) {
    fprintf(stderr,
        "usage:\n"
        "  tur expand [-I <dir>...] [--no-auto-spice] [--no-auto-stdlib] <file.tur>\n"
        "                                       elaborate the file and print each macro\n"
        "                                       expansion to stdout as it happens\n"
        "\n"
        "Each expansion prints as a `;; <macro-name> @ <line>:<col>` header (the\n"
        "call site) followed by the expanded form.  Nested expansions print too,\n"
        "inner-first, in elaboration order.  Diagnostics still go to stderr, so\n"
        "stdout is suitable for golden-file comparison.\n"
        "\n"
        "flags: same as `tur check` (see `tur check --help`)\n");
    return 0;
}

int usage_check(void) {
    fprintf(stderr,
        "usage:\n"
        "  tur check [-I <dir>...] [--no-auto-spice] [--no-auto-stdlib] <file.tur>\n"
        "                                       type-check only, no codegen\n"
        "\n"
        "flags:\n"
        "  -I <dir>             add an include directory for module resolution\n"
        "                       (repeat to add multiple)\n"
        "  --no-auto-spice      don't auto-discover the enclosing spice's src/\n"
        "                       (default behavior walks up from the file looking\n"
        "                       for build.tur and adds <spice>/src to the path)\n"
        "  --no-auto-stdlib     when the checked file is one of the auto-loaded\n"
        "                       stdlib files, skip auto-loading it and all\n"
        "                       subsequent stdlib files; earlier stdlib files\n"
        "                       still load so the file's own deps resolve.\n"
        "                       Use when type-checking auto-loaded stdlib files\n"
        "                       in isolation to avoid duplicate-definition errors.\n"
        "\n"
        "Try 'tur --help' for global options.\n");
    return 0;
}

/* SC1: is argv[i] the start of a `-I <path>` or `-I<path>` flag?
 * On true, writes the number of argv positions it consumes into *consumed
 * (1 for "-Ifoo", 2 for "-I foo").  Callers loop over argv looking for
 * non-include flags and skip these positions. */
bool drv_is_include_flag(int argc, char **argv, int i, int *consumed) {
    const char *a = argv[i];
    if (strcmp(a, "-I") == 0 && i + 1 < argc) {
        *consumed = 2;
        return true;
    }
    if (strncmp(a, "-I", 2) == 0 && a[2] != '\0') {
        *consumed = 1;
        return true;
    }
    return false;
}

/* SC1: collect every -I flag from argv[start..argc-1] into a heap-allocated
 * array.  The array of pointers is malloc'd (caller frees); the strings it
 * points to are borrowed from argv and live as long as argv does.
 *
 * Returns the number of include dirs collected, or -1 if a bare `-I` was
 * encountered without a following path argument.  Non-`-I` arguments are
 * ignored -- callers handle them in their own pass (using drv_is_include_flag
 * to skip the positions consumed here). */
int drv_parse_include_flags(int argc, char **argv, int start, char ***out_dirs) {
    int  cap = 4;
    int  n   = 0;
    char **dirs = (char **)malloc((size_t)cap * sizeof(char *));
    if (!dirs) { *out_dirs = NULL; return 0; }
    for (int i = start; i < argc; i++) {
        const char *a = argv[i];
        if (strcmp(a, "-I") == 0) {
            if (i + 1 >= argc) { free(dirs); *out_dirs = NULL; return -1; }
            if (n >= cap) {
                cap *= 2;
                char **bigger = (char **)realloc(dirs, (size_t)cap * sizeof(char *));
                if (!bigger) { free(dirs); *out_dirs = NULL; return n; }
                dirs = bigger;
            }
            dirs[n++] = argv[++i];
        } else if (strncmp(a, "-I", 2) == 0 && a[2] != '\0') {
            if (n >= cap) {
                cap *= 2;
                char **bigger = (char **)realloc(dirs, (size_t)cap * sizeof(char *));
                if (!bigger) { free(dirs); *out_dirs = NULL; return n; }
                dirs = bigger;
            }
            dirs[n++] = argv[i] + 2;
        }
    }
    *out_dirs = dirs;
    return n;
}

/* The interpreter arms (`tur --interpret`, `tur debug`): collect the `-I`
 * flags that come BEFORE the file path, starting at argv[start].  Everything
 * after the path is the program's *args*, untouched -- a `-I` there belongs to
 * the program.  Returns the index of the file path (argc when there is none),
 * or -1 after reporting a bare trailing `-I`.  *out_dirs is malloc'd (caller
 * frees); its strings are borrowed from argv. */
static int interp_leading_include_flags(int argc, char **argv, int start,
                                        char ***out_dirs, int *n_out) {
    *out_dirs = NULL;
    *n_out = 0;
    int i = start;
    while (i < argc) {
        int consumed = 0;
        if (!drv_is_include_flag(argc, argv, i, &consumed)) {
            if (strcmp(argv[i], "-I") == 0) {
                free(*out_dirs);
                *out_dirs = NULL;
                *n_out = 0;
                fprintf(stderr, "tur: -I requires a directory argument\n");
                return -1;
            }
            break;
        }
        char **bigger = (char **)realloc(*out_dirs,
                                         (size_t)(*n_out + 1) * sizeof(char *));
        if (!bigger) break;
        *out_dirs = bigger;
        (*out_dirs)[(*n_out)++] = consumed == 2 ? argv[i + 1] : argv[i] + 2;
        i += consumed;
    }
    return i;
}

int usage_eval(void) {
    fprintf(stderr,
        "usage:\n"
        "  tur eval '<expr>'          evaluate an inline expression and print the result\n"
        "  tur eval --file <file.tur> run a .tur file through the interpreter\n"
        "\n"
        "Try 'tur --help' for global options.\n");
    return 0;
}

int usage_doc(void) {
    fprintf(stderr,
        "usage:\n"
        "  tur doc <symbol>          print documentation for a builtin, special\n"
        "                            form, or stdlib definition\n"
        "  tur doc --json <symbol>   print documentation as JSON\n"
        "\n"
        "Reads the docstring table shipped alongside the binary\n"
        "(<stdlib>/docstrings.tur); no network access is involved.\n"
        "For rendered guides and API pages offline, see `tur docs`.\n"
        "\n"
        "Try 'tur --help' for global options.\n");
    return 0;
}

int usage_format(void) {
    fprintf(stderr,
        "usage:\n"
        "  tur format [file.tur]          format a source file (stdin if no file given)\n"
        "  tur format --check [file.tur]  exit 1 if formatting would change the file\n"
        "  tur format --diff [file.tur]   print unified diff of formatting changes\n"
        "  tur format --lang <dialect>    read with that dialect (as `tur fmt --lang`);\n"
        "                                 default: the file's extension and #lang line\n"
        "\n"
        "  Prefer `tur fmt`, which also formats in place.\n"
        "\n"
        "Try 'tur --help' for global options.\n");
    return 0;
}

int usage_parse_check(void) {
    fprintf(stderr,
        "usage:\n"
        "  tur parse-check <a> <b>   exit 0 if both files read to the same AST\n"
        "\n"
        "  <a> is read as turmeric, <b> as sweet-exp, unless an explicit\n"
        "  #lang directive overrides. Exit codes: 0 equal, 1 mismatch, 2 error.\n"
        "\n"
        "Try 'tur --help' for global options.\n");
    return 0;
}

int usage_test(void) {
    fprintf(stderr,
        "usage:\n"
        "  tur test <dir>   run all .tur test files in a directory\n"
        "\n"
        "Each test compiles and runs; it passes iff both succeed (exit 0).\n"
        "A test file may carry directives in its leading comment lines:\n"
        "  ;; tur-test-flags: --strict-refine   extra compile flags for this test\n"
        "  ;; tur-test-expect-error: TUR-W0372  this test must FAIL to compile and\n"
        "                                       its diagnostics must contain the\n"
        "                                       text; the run phase is skipped\n"
        "\n"
        "Try 'tur --help' for global options.\n");
    return 0;
}

int usage_repl(void) {
    fprintf(stderr,
        "usage:\n"
        "  tur repl [--watch] [--lang <dialect>] [--engine <name>]\n"
        "                  start the interactive REPL\n"
        "\n"
        "flags:\n"
        "  --lang <d>      start in a language dialect: \"turmeric\" (default),\n"
        "                  \"saffron\" (unannotated params default to any),\n"
        "                  \"r7rs\" (Scheme), or any base `tur dialects` lists.\n"
        "                  Equivalent to typing `#lang <d>` at the prompt.\n"
        "  --watch         auto-reload the enclosing spice between prompts\n"
        "                  when any source .tur file's mtime advances\n"
        "                  (RP6; equivalent to typing (reload) each turn)\n"
        "  --engine <name> engine for building the enclosing spice:\n"
        "                  \"cc\" (default -- build --shared subprocess) or\n"
        "                  \"jit\" (in-process MIR, needs -DTUR_JIT=ON).\n"
        "                  Precedence: --engine > TUR_ENGINE > build.tur\n"
        "                  :engine > \"cc\"\n"
        "\n"
        "REPL commands:\n"
        "  :help           print help\n"
        "  :quit / :q      exit the REPL\n"
        "  :doc <sym>      look up documentation for a symbol\n"
        "  :type <expr>    print the inferred type of an expression\n"
        "  :reload <file>  reload a source file\n"
        "  :reset          clear session and start fresh\n"
        "  :tutorial       start the interactive tutorial\n"
        "\n"
        "REPL forms:\n"
        "  (reload)        rebuild the loaded spice and refresh bindings\n"
        "\n"
        "Try 'tur --help' for global options.\n");
    return 0;
}

int usage_explain(void) {
    fprintf(stderr,
        "usage:\n"
        "  tur explain <TUR-E####>    print explanation for a diagnostic code\n"
        "  tur explain '<snippet>'   compile a snippet and explain errors\n"
        "\n"
        "examples:\n"
        "  tur explain TUR-E0042\n"
        "  tur explain '(+ 1 \"x\")'\n"
        "\n"
        "Try 'tur --help' for global options.\n");
    return 0;
}

/* Phase 8: Handle --no-color flag */
static bool parse_no_color(int argc, char **argv) {
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--no-color") == 0) {
            return true;
        }
    }
    return false;
}

/* SC4: --no-auto-spice disables the per-file subcommand walk-up that
 * adds the enclosing spice's `src/` to the include path.  Inspected by
 * auto_append_spice_includes(). */
bool g_no_auto_spice = false;

/* --no-auto-stdlib (tur check only): suffix-skip -- when the file being
 * checked IS one of the auto-loaded stdlib files, skip auto-loading it
 * and all subsequent stdlib files.  Earlier entries still load so the
 * file's transitive dependencies resolve.  This prevents duplicate-
 * definition errors when type-checking auto-loaded stdlib files in isolation. */
bool g_no_auto_stdlib = false;

static bool parse_no_auto_spice(int argc, char **argv) {
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--no-auto-spice") == 0) return true;
    }
    return false;
}

/* J6: --no-abi-cache (or TUR_NO_ABI_CACHE=1) disables the persistent
 * cross-module ABI specialization cache under <build-root>/.tur-abi-cache/.
 * The cache is a build-time optimization; disabling it must always still
 * produce a correct build (clones are recomputed per invocation). */
bool g_no_abi_cache = false;

static bool parse_no_abi_cache(int argc, char **argv) {
    const char *env = getenv("TUR_NO_ABI_CACHE");
    if (env && *env && strcmp(env, "0") != 0) return true;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--no-abi-cache") == 0) return true;
    }
    return false;
}

/* r7rs-gc (graduated 2026-09-25): the collector is the allocator of every
 * compiled single-unit `#lang r7rs` program (g_opt_r7rs_gc defaults true).
 * `TUR_R7RS_GC=0` or `--no-r7rs-gc` builds one without it -- the way out for
 * a program that starts threads, which the collector refuses at the start
 * site (docs/archive/r7rs-gc-plan.md).  `TUR_R7RS_GC=1` is the default
 * spelled out (tests/run-r7rs-gc.sh's "with" arm). */
/* any-widen-stored-in-an-adt-field-has-no-owner: the same collector is the
 * allocator of every compiled single-unit `#lang saffron` program
 * (g_opt_saffron_gc defaults true); `TUR_SAFFRON_GC=0` or `--no-saffron-gc`
 * builds one on plain malloc/free, where its `any` boxes are owned only as far
 * as the static drops reach. */
static bool parse_no_saffron_gc(int argc, char **argv) {
    bool off = false;
    const char *env = getenv("TUR_SAFFRON_GC");
    if (env && env[0] == '0') off = true;
    else if (env && env[0] == '1') off = false;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--no-saffron-gc") == 0) off = true;
    }
    return off;
}

static bool parse_no_r7rs_gc(int argc, char **argv) {
    bool off = false;
    const char *env = getenv("TUR_R7RS_GC");
    if (env && env[0] == '0') off = true;
    else if (env && env[0] == '1') off = false;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--no-r7rs-gc") == 0) off = true;
    }
    return off;
}

/* Phase R5: Handle --panic-abort flag */
static bool parse_panic_abort(int argc, char **argv) {
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--panic-abort") == 0) {
            return true;
        }
    }
    return false;
}

/* Debugger Phase 4: --debug emits `#line` source-map directives into the
 * generated C and builds single-file targets with `-g -O0`. */
static bool parse_debug_build(int argc, char **argv) {
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--debug") == 0) {
            return true;
        }
    }
    return false;
}

/* Phase R6: Handle --warn-unused-result flag */
static bool parse_warn_unused_result(int argc, char **argv) {
    bool enabled = false;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--warn-unused-result") == 0) {
            enabled = true;
        } else if (strcmp(argv[i], "--no-warn-unused-result") == 0) {
            enabled = false;
        }
    }
    return enabled;
}

/* F4 (cross-plan-followups): --Werror=deprecated -- what it promotes is
 * listed at g_werror_deprecated (runtime/globals.c). */
static bool parse_werror_deprecated(int argc, char **argv) {
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--Werror=deprecated") == 0 ||
            strcmp(argv[i], "-Werror=deprecated") == 0) {
            return true;
        }
    }
    return false;
}

/* Phase C: --Werror=inline-c-narrow-params promotes narrow-param-in-inline-C
 * warnings to hard errors so a strict build can gate against unannotated
 * narrow parameters reaching inline-C bodies. */
static bool parse_werror_inline_c_narrow_params(int argc, char **argv) {
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--Werror=inline-c-narrow-params") == 0 ||
            strcmp(argv[i], "-Werror=inline-c-narrow-params") == 0) {
            return true;
        }
    }
    return false;
}

/* -Werror=strict-effects promotes the --strict-effects lints (TUR-W0030,
 * TUR-W0032) to errors.  It implies --strict-effects; main applies that. */
static bool parse_werror_strict_effects(int argc, char **argv) {
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--Werror=strict-effects") == 0 ||
            strcmp(argv[i], "-Werror=strict-effects") == 0) {
            return true;
        }
    }
    return false;
}

/* Phase R6: Handle --lint-panic flag */
static bool parse_lint_panic(int argc, char **argv) {
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--lint-panic") == 0) {
            return true;
        }
    }
    return false;
}

/* Phase C2: Handle --no-contracts flag */
static bool parse_no_contracts(int argc, char **argv) {
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--no-contracts") == 0) {
            return true;
        }
    }
    return false;
}

/* Phase 8: Handle --json-diagnostics flag */
static bool use_json_diagnostics = false;

/* GS-M2: subcommand fallthrough. When `tur <cmd>` doesn't match a built-in,
 * search $PATH for `tur-<cmd>` and exec it with the remaining argv. Modeled
 * after git-foo / git foo. On success this does not return. On failure
 * prints a diagnostic and returns 1. */
static int try_external_subcommand(int argc, char **argv) {
    if (argc < 2) return usage();
    const char *cmd = argv[1];
    if (!cmd || !*cmd || cmd[0] == '-') return usage();
    /* Reject anything that's clearly a path or contains shell-unsafe bits. */
    for (const char *p = cmd; *p; p++) {
        if (!(isalnum((unsigned char)*p) || *p == '-' || *p == '_')) return usage();
    }
    char ext_name[256];
    snprintf(ext_name, sizeof(ext_name), "tur-%s", cmd);

    /* The suffixes a command name can carry.  Windows resolves a bare name
     * through PATHEXT, and `tur install` writes `tur-foo.exe`, so probing only
     * the bare name finds nothing there. */
#ifdef _WIN32
    static const char *const EXTS[] = { ".exe", "" };
#else
    static const char *const EXTS[] = { "" };
#endif

    const char *path_env = getenv("PATH");
    if (path_env) {
        const char *p = path_env;
        while (*p) {
            /* TUR_PATH_LIST_SEP, not ':': on Windows a ':' split tears every
             * entry at its drive colon and no candidate is ever well formed. */
            const char *sep = strchr(p, TUR_PATH_LIST_SEP);
            size_t seg = sep ? (size_t)(sep - p) : strlen(p);
            for (size_t e = 0; seg > 0 && seg < 3500
                               && e < sizeof(EXTS) / sizeof(EXTS[0]); e++) {
                char candidate[4096];
                snprintf(candidate, sizeof(candidate), "%.*s/%s%s",
                         (int)seg, p, ext_name, EXTS[e]);
                if (access(candidate, X_OK) != 0) continue;
                char **nv = (char **)calloc((size_t)argc, sizeof(char *));
                if (!nv) return 1;
                nv[0] = (char *)ext_name;
                for (int i = 2; i < argc; i++) nv[i - 1] = argv[i];
                nv[argc - 1] = NULL;
#ifdef _WIN32
                /* _execv does not REPLACE the process on Windows: the parent
                 * exits immediately and a calling shell sees the command
                 * finish before the child has.  Wait and pass the status on. */
                intptr_t rc = _spawnv(_P_WAIT, candidate, (const char *const *)nv);
                if (rc >= 0) { free(nv); return (int)rc; }
#else
                execv(candidate, nv);
#endif
                fprintf(stderr, "tur: failed to exec '%s': %s\n",
                        candidate, strerror(errno));
                free(nv);
                return 1;
            }
            if (!sep) break;
            p = sep + 1;
        }
    }
    fprintf(stderr,
        "tur: '%s' is not a tur command. See 'tur --help'.\n", cmd);
    return 1;
}

static int tur_main_inner(int argc, char **argv) {
    /* profiling-plan C0+C1: --timings first, so the clock starts before
     * anything it should measure, and stripped before g_main_argv is taken
     * below -- the split build's cache key hashes the command line, and a
     * profiler that changed the key would profile a cold cache. */
    parse_timings_flag(&argc, argv);
    /* TUR_ADT_SLAB=1: bump-allocate never-freed multi-variant ADT boxes.
     * Env-only measurement seam (see
     * docs/archive/multi-variant-adts-always-heap-allocate.md), deliberately
     * not a CLI flag -- and SHELVED 2026-08-25, so it is not a shipping
     * feature and is not on its way to becoming one. */
    {
        const char *__slab = getenv("TUR_ADT_SLAB");
        if (__slab && __slab[0] == '1') g_adt_slab = true;
        /* SR1 is ON by default; the env var is a two-way override so a
         * suspected representation bug can be bisected against the old int64
         * carrier without rebuilding.  `=0` opts out, `=1` is a no-op that
         * keeps existing invocations working. */
        const char *__sr1 = getenv("TUR_SR1_SUM_BYVALUE");
        if (__sr1 && __sr1[0] == '0') g_sr1_sum_byvalue = false;
        else if (__sr1 && __sr1[0] == '1') g_sr1_sum_byvalue = true;
        /* SR2a is ON by default too, since its graduation out of
         * --enable=parametric-sum-byvalue.  Same two-way shape: `=0` restores
         * the int64 carrier for a parametric sum monomorph, `=1` is the no-op
         * that keeps the measurement harnesses' existing invocations working.
         *
         * `=0` also takes SR3's Option niche down with it, in sr3_option_niche
         * (types.c) rather than here -- the niche narrows a BY-VALUE Option to
         * its payload pointer, so it cannot outlive the representation it
         * narrows.  Setting this alone is therefore a complete switch; it did
         * not used to be, and the failure mode was a compiler abort two layers
         * from the flag (docs/archive/sr2-carrier-seam-rotted.md). */
        const char *__sr2 = getenv("TUR_SR2_APP_SUM_BYVALUE");
        if (__sr2 && __sr2[0] == '0') g_sr2_app_sum_byvalue = false;
        else if (__sr2 && __sr2[0] == '1') g_sr2_app_sum_byvalue = true;
        /* SR3 slice B (the Option niche) is ON by default since its graduation
         * out of --enable=option-niche (2026-09-03).  `=0` restores the tagged
         * 16-byte Option monomorph and the boxed Vec slot for bisection and
         * for tests/run-option-niche-seam.sh, which keeps that path green.
         *
         * One-directional: this turns the niche off without disturbing SR2,
         * whereas TUR_SR2_APP_SUM_BYVALUE=0 turns BOTH off (the niche needs a
         * by-value Option to narrow).  So `=1` here cannot force the niche back
         * on under a disabled SR2 -- sr3_option_niche refuses it, deliberately,
         * because that combination is the one that aborts the compiler. */
        const char *__sr3 = getenv("TUR_OPTION_NICHE");
        if (__sr3 && __sr3[0] == '0') g_opt_option_niche = false;
        else if (__sr3 && __sr3[0] == '1') g_opt_option_niche = true;
        /* RM3 regions are ON by default since graduation out of
         * --enable=regions (2026-09-05).  `=0` restores plain malloc at the
         * routed ctor sites, no bracket around bt-scope / with-region, and no
         * atexit shutdown -- the pre-graduation default build -- for bisection
         * and for tests/run-regions-seam.sh, which keeps that path green.
         * Independent of the SR2/SR3 hatches above: a region is a lifetime
         * over the arena, not a representation, so it composes with either. */
        const char *__rgn = getenv("TUR_REGIONS");
        if (__rgn && __rgn[0] == '0') g_opt_regions = false;
        else if (__rgn && __rgn[0] == '1') g_opt_regions = true;
        /* The interpreter's inline-C JIT is ON by default since graduation
         * out of --enable=repl-jit-inline-c (2026-10-17).  `=0` restores the
         * "inline-C not supported" error -- the pre-graduation default -- for
         * bisection and for the harnesses that test the pattern executor on
         * its own (run-interp-*-inline-c*.sh); tests/turi/repl-jit-inline-c.sh
         * keeps that path covered. */
        const char *__ijit = getenv("TUR_REPL_JIT_INLINE_C");
        if (__ijit && __ijit[0] == '0') g_opt_repl_jit_inline_c = false;
        else if (__ijit && __ijit[0] == '1') g_opt_repl_jit_inline_c = true;
    }

#ifdef _WIN32
    /*
     * Put stdout/stderr in binary mode so a newline stays one byte.
     *
     * Windows opens them in text mode by default, which silently rewrites every
     * '\n' to "\r\n" on the way out.  Three things here break on that, and two
     * of them break silently:
     *
     *   - `tur emit-c` writes generated C to stdout, so every line would gain a
     *     \r and stop matching the tests/fixtures/<name>/expected.c snapshots.
     *   - The LSP and DAP servers frame messages with a Content-Length byte
     *     count computed BEFORE the text-mode expansion -- so the count would
     *     understate the bytes actually written and desynchronise the protocol.
     *   - `tur fmt --stdout` would rewrite the file's line endings as a side
     *     effect of printing it.
     *
     * Binary mode makes output byte-exact, matching every other platform.
     */
    _setmode(_fileno(stdout), _O_BINARY);
    _setmode(_fileno(stderr), _O_BINARY);
#endif
    /* SN1: stash argv[0] for exe-path fallback in resolve_stdlib_root().
     * Platform APIs (_NSGetExecutablePath / /proc/self/exe) are tried
     * first; argv[0] is the last-resort path source. */
    g_argv0 = (argc > 0) ? argv[0] : NULL;
    g_main_argc = argc;
    g_main_argv = (const char *const *)argv;
    /* Resolve the stdlib root once at startup so TUR_STDLIB_DIR is
     * propagated into the process env before any subsystem (elaborator,
     * worker, interpreter) reads it. */
    (void)resolve_stdlib_root();

#ifdef TUR_HAVE_JIT
    /* jit-ffi-c2mir-plan: install the c2mir-backed dynamic-FFI provider so
     * the interpreter's extern-c registration, call-ptr routing, and the
     * spice FFI ladder can synthesize call thunks at runtime.  JIT builds
     * only; without it every consumer keeps the non-JIT fallback behavior. */
    tur_jit_ffi_install();
    /* aot-compiled-repl-plan C1: the interpreter's compiler for inline-C
     * defns it cannot run.  Inert under TUR_REPL_JIT_INLINE_C=0. */
    turi_set_inline_c_jit_hook(&g_repl_inline_c_jit_hook);
#endif

    /* global-flags-parsed-after-double-dash: everything from the first `--`
     * on belongs to the program, so the global flags are parsed and stripped
     * from the words before it only.  `argc` is cut there for the parse, and
     * the tail (the `--` included, which the commands that forward arguments
     * look for) is put back after the strip loop below.  The strip only moves
     * words below the cut, so the tail is never touched in between. */
    int dd_at = argc;
    for (int i = 1; i < argc; i++)
        if (strcmp(argv[i], "--") == 0) { dd_at = i; break; }
    const int dd_tail = argc - dd_at;
    argc = dd_at;

    /* Phase 8: Check for global flags before command */
    bool no_color = parse_no_color(argc, argv);
    bool explain_mode = false;
    const char *explain_code = NULL;
    g_panic_abort = parse_panic_abort(argc, argv);
    g_warn_unused_result = parse_warn_unused_result(argc, argv);
    g_lint_panic = parse_lint_panic(argc, argv);
    /* Phase C2: --no-contracts strips contract checks (release builds). */
    g_no_contracts = parse_no_contracts(argc, argv);
    /* Debugger Phase 4: --debug emits `#line` directives + builds with -g -O0. */
    g_emit_debug_lines = parse_debug_build(argc, argv);
    /* F4: --Werror=deprecated -- see g_werror_deprecated (runtime/globals.c) */
    g_werror_deprecated = parse_werror_deprecated(argc, argv);
    /* Phase C: --Werror=inline-c-narrow-params promotes narrow-param warnings */
    g_werror_inline_c_narrow_params = parse_werror_inline_c_narrow_params(argc, argv);
    /* -Werror=strict-effects: promote the effect-annotation lints to errors. */
    g_werror_strict_effects = parse_werror_strict_effects(argc, argv);
    if (g_werror_strict_effects) g_strict_effects = true;
    /* SC4: --no-auto-spice disables enclosing-spice auto-discovery in
     * per-file subcommands (check/emit-c/emit-h/run). */
    g_no_auto_spice = parse_no_auto_spice(argc, argv);
    /* J6: --no-abi-cache / TUR_NO_ABI_CACHE disables the persistent
     * cross-module ABI specialization cache (.tur-abi-cache/). */
    g_no_abi_cache = parse_no_abi_cache(argc, argv);
    /* r7rs-gc: TUR_R7RS_GC=0 / --no-r7rs-gc builds a Scheme program without
     * the collector (a program that starts threads). */
    if (parse_no_r7rs_gc(argc, argv)) g_opt_r7rs_gc = false;
    if (parse_no_saffron_gc(argc, argv)) g_opt_saffron_gc = false;
    /* tur-link-and-build-split-plan Phase 2/3c/6: TUR_RUNTIME overrides the
     * default runtime-linkage mode (auto).  A CLI --runtime= flag, parsed
     * later, still wins. */
    {
        const char *rt = getenv("TUR_RUNTIME");
        if (rt) {
            if      (strcmp(rt, "lib") == 0)    g_runtime_mode = TUR_RT_LIB;
            else if (strcmp(rt, "source") == 0) g_runtime_mode = TUR_RT_SOURCE;
            else if (strcmp(rt, "auto") == 0)   g_runtime_mode = TUR_RT_AUTO;
            else if (strcmp(rt, "split") == 0)  g_runtime_mode = TUR_RT_SPLIT;
        }
    }

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--panic-abort") == 0) {
            /* Already parsed, remove from argv */
            for (int j = i; j < argc - 1; j++) {
                argv[j] = argv[j + 1];
            }
            argc--;
            i--;
        } else if (strcmp(argv[i], "--no-auto-spice") == 0) {
            /* SC4: already parsed into g_no_auto_spice; strip from argv. */
            for (int j = i; j < argc - 1; j++) {
                argv[j] = argv[j + 1];
            }
            argc--;
            i--;
        } else if (strcmp(argv[i], "--no-r7rs-gc") == 0 ||
                   strcmp(argv[i], "--no-saffron-gc") == 0) {
            /* r7rs-gc: already parsed into g_opt_r7rs_gc / g_opt_saffron_gc;
             * strip from argv. */
            for (int j = i; j < argc - 1; j++) {
                argv[j] = argv[j + 1];
            }
            argc--;
            i--;
        } else if (strcmp(argv[i], "--panic-trace") == 0) {
            /* Retired (panic-location-names-the-runtime-not-the-call-site):
             * it printed the defer-frame chain as raw pointers, which named
             * no function and no line.  A panic now names its own .tur file
             * and line, and `--debug` builds for a debugger's backtrace.
             * Still accepted so existing scripts keep working; TUR-W0050
             * says so. */
            fprintf(stderr, "warning [TUR-W0050]: --panic-trace is retired and "
                            "does nothing; a panic already names its .tur file "
                            "and line, and a --debug build gives a debugger "
                            "backtrace (`tur debug`)\n");
            for (int j = i; j < argc - 1; j++) {
                argv[j] = argv[j + 1];
            }
            argc--;
            i--;
        } else if (strcmp(argv[i], "--warn-unused-result") == 0 ||
                   strcmp(argv[i], "--no-warn-unused-result") == 0) {
            /* Already parsed, remove from argv */
            for (int j = i; j < argc - 1; j++) {
                argv[j] = argv[j + 1];
            }
            argc--;
            i--;
        } else if (strcmp(argv[i], "--Werror=deprecated") == 0 ||
                   strcmp(argv[i], "-Werror=deprecated") == 0) {
            /* F4: already parsed into g_werror_deprecated; remove from argv. */
            for (int j = i; j < argc - 1; j++) {
                argv[j] = argv[j + 1];
            }
            argc--;
            i--;
        } else if (strcmp(argv[i], "--Werror=inline-c-narrow-params") == 0 ||
                   strcmp(argv[i], "-Werror=inline-c-narrow-params") == 0) {
            /* Phase C: already parsed into g_werror_inline_c_narrow_params; remove. */
            for (int j = i; j < argc - 1; j++) {
                argv[j] = argv[j + 1];
            }
            argc--;
            i--;
        } else if (strcmp(argv[i], "--Werror=strict-effects") == 0 ||
                   strcmp(argv[i], "-Werror=strict-effects") == 0) {
            /* Already parsed into g_werror_strict_effects; remove. */
            for (int j = i; j < argc - 1; j++) {
                argv[j] = argv[j + 1];
            }
            argc--;
            i--;
        } else if (strcmp(argv[i], "--lint-panic") == 0) {
            /* Already parsed, remove from argv */
            for (int j = i; j < argc - 1; j++) {
                argv[j] = argv[j + 1];
            }
            argc--;
            i--;
        } else if (strcmp(argv[i], "--no-contracts") == 0) {
            /* Phase C2: already parsed into g_no_contracts; remove from argv. */
            for (int j = i; j < argc - 1; j++) {
                argv[j] = argv[j + 1];
            }
            argc--;
            i--;
        } else if (strcmp(argv[i], "--debug") == 0) {
            /* Debugger Phase 4: already parsed into g_emit_debug_lines; strip
             * so the per-command flag parsers (build/emit-c) don't reject it. */
            for (int j = i; j < argc - 1; j++) {
                argv[j] = argv[j + 1];
            }
            argc--;
            i--;
        } else if (strcmp(argv[i], "--json-diagnostics") == 0) {
            use_json_diagnostics = true;
            for (int j = i; j < argc - 1; j++) {
                argv[j] = argv[j + 1];
            }
            argc--;
            i--;
        } else if (strcmp(argv[i], "--json") == 0) {
            /* E14: structured JSON output — implies --json-diagnostics for check */
            use_json_output = true;
            for (int j = i; j < argc - 1; j++) {
                argv[j] = argv[j + 1];
            }
            argc--;
            i--;
        } else if (strcmp(argv[i], "--explain") == 0 && i + 1 < argc) {
            explain_mode = true;
            explain_code = argv[i + 1];
            /* Remove --explain and code from argv */
            for (int j = i; j < argc - 2; j++) {
                argv[j] = argv[j + 2];
            }
            argc -= 2;
            i--;
        } else if (strcmp(argv[i], "--lint-unsafe") == 0) {
            g_lint_unsafe_enabled = true;
            g_unsafe_warn_nested = true;
            /* Remove from argv for command parsing */
            for (int j = i; j < argc - 1; j++) {
                argv[j] = argv[j + 1];
            }
            argc--;
            i--;
        } else if (strncmp(argv[i], "--lint-unsafe-max-lines=", 24) == 0) {
            g_lint_unsafe_enabled = true;
            g_unsafe_max_lines = (uint32_t)atoi(argv[i] + 24);
            /* Remove from argv for command parsing */
            for (int j = i; j < argc - 1; j++) {
                argv[j] = argv[j + 1];
            }
            argc--;
            i--;
        } else if (strcmp(argv[i], "--require-unsafe-docs") == 0) {
            g_lint_unsafe_enabled = true;
            g_unsafe_require_safety = true;
            /* Remove from argv for command parsing */
            for (int j = i; j < argc - 1; j++) {
                argv[j] = argv[j + 1];
            }
            argc--;
            i--;
        } else if (strcmp(argv[i], "--unsafe-stats") == 0) {
            g_lint_unsafe_enabled = true;
            g_unsafe_stats_enabled = true;
            /* Remove from argv for command parsing */
            for (int j = i; j < argc - 1; j++) {
                argv[j] = argv[j + 1];
            }
            argc--;
            i--;
        } else if (strcmp(argv[i], "--lint-inline-c-unsafe") == 0) {
            g_lint_inline_c_unsafe = true;
            for (int j = i; j < argc - 1; j++) {
                argv[j] = argv[j + 1];
            }
            argc--;
            i--;
        } else if (strcmp(argv[i], "--dump-kinds") == 0) {
            /* Phase HKT-P6: enable kind-annotation dump after kind-check */
            g_dump_kinds = true;
            for (int j = i; j < argc - 1; j++) {
                argv[j] = argv[j + 1];
            }
            argc--;
            i--;
        } else if (strcmp(argv[i], "--strict-refine") == 0) {
            /* RT3: upgrade every undecided/refuted refinement obligation from
             * "keep the runtime check" to a hard compile error.  A
             * diagnostic-strictness knob, NOT an experiment -- it changes how
             * loudly the compiler reports, not what it compiles. */
            g_strict_refine = true;
            for (int j = i; j < argc - 1; j++) {
                argv[j] = argv[j + 1];
            }
            argc--;
            i--;
        } else if (strcmp(argv[i], "--strict-effects") == 0) {
            /* ER1: enforce unannotated effectful functions as warnings */
            g_strict_effects = true;
            for (int j = i; j < argc - 1; j++) {
                argv[j] = argv[j + 1];
            }
            argc--;
            i--;
        } else if (strcmp(argv[i], "--dump-reflect") == 0) {
            /* reflected-measures RF5: print the totality verdict for each
             * `^reflect` site */
            g_dump_reflect = true;
            for (int j = i; j < argc - 1; j++) {
                argv[j] = argv[j + 1];
            }
            argc--;
            i--;
        } else if (strcmp(argv[i], "--dump-write-frames") == 0) {
            /* G1: print the checked verdict for each declared `#writes` frame */
            g_dump_write_frames = true;
            for (int j = i; j < argc - 1; j++) {
                argv[j] = argv[j + 1];
            }
            argc--;
            i--;
        } else if (strcmp(argv[i], "--dump-read-frames") == 0) {
            /* R4 slice 2: print the verification verdict for each `#reads` frame */
            g_dump_read_frames = true;
            for (int j = i; j < argc - 1; j++) {
                argv[j] = argv[j + 1];
            }
            argc--;
            i--;
        } else if (strcmp(argv[i], "--dump-effects") == 0) {
            /* ER6: print inferred effect row for each defn after inference */
            g_dump_effects = true;
            for (int j = i; j < argc - 1; j++) {
                argv[j] = argv[j + 1];
            }
            argc--;
            i--;
        } else if (strcmp(argv[i], "--dump-refine=json") == 0) {
            /* SX8a: one JSON record per refinement obligation, on stdout.
             * Stripped here rather than in a per-command loop so it works
             * with `check` and `emit-c` alike -- the records come from the
             * elaboration both share, and forcing a codegen run just to read
             * a report would be the wrong shape. */
            g_dump_refine_json = true;
            for (int j = i; j < argc - 1; j++) {
                argv[j] = argv[j + 1];
            }
            argc--;
            i--;
        } else if (strcmp(argv[i], "--dump-cps-coloring") == 0) {
            /* CPS1: print the whole-program may-capture coloring per defn */
            g_dump_cps_coloring = true;
            for (int j = i; j < argc - 1; j++) {
                argv[j] = argv[j + 1];
            }
            argc--;
            i--;
        } else if (strcmp(argv[i], "--dump-cps") == 0) {
            /* CPS2: print the ANF/CPS IR for each colored defn */
            g_dump_cps = true;
            for (int j = i; j < argc - 1; j++) {
                argv[j] = argv[j + 1];
            }
            argc--;
            i--;
        } else if (strcmp(argv[i], "--dump-mono-specs") == 0) {
            /* VBM1 (van-laarhoven-monomorphization-plan): print the by-value HKT
             * monomorphization spec registry after elaboration. */
            g_dump_mono_specs = true;
            for (int j = i; j < argc - 1; j++) {
                argv[j] = argv[j + 1];
            }
            argc--;
            i--;
        } else if (strcmp(argv[i], "--dump-cps-mono") == 0) {
            /* G1 (cps-backend-generic-monomorph-classification-plan): report CPS-
             * subset admissibility of each colored-generic monomorph. */
            g_dump_cps_mono = true;
            for (int j = i; j < argc - 1; j++) {
                argv[j] = argv[j + 1];
            }
            argc--;
            i--;
        } else if (strcmp(argv[i], "--dump-sizes") == 0) {
            /* SZ8: print inferred size index per sized-GADT constructor */
            g_dump_sizes = true;
            for (int j = i; j < argc - 1; j++) {
                argv[j] = argv[j + 1];
            }
            argc--;
            i--;
        } else if (strcmp(argv[i], "--lint-effects") == 0) {
            /* Retired: it was a byte-identical copy of --strict-effects
             * (docs/archive/strict-effects-and-lint-effects-are-
             * indistinguishable.md).  Still accepted, as an alias, so
             * existing scripts keep working; TUR-W0050 says so. */
            fprintf(stderr, "warning [TUR-W0050]: --lint-effects is deprecated; "
                            "use --strict-effects, which emits the same "
                            "TUR-W0030 warnings (-Werror=strict-effects makes "
                            "them errors)\n");
            g_strict_effects = true;
            for (int j = i; j < argc - 1; j++) {
                argv[j] = argv[j + 1];
            }
            argc--;
            i--;
        } else if (strncmp(argv[i], "--backtrack-depth=", 18) == 0) {
            /* Phase B5: cap run-backtrack at N results (0 = unlimited) */
            g_backtrack_depth = (int64_t)atoll(argv[i] + 18);
            for (int j = i; j < argc - 1; j++) {
                argv[j] = argv[j + 1];
            }
            argc--;
            i--;
        } else if (strcmp(argv[i], "--backtrack-depth") == 0 && i + 1 < argc) {
            g_backtrack_depth = (int64_t)atoll(argv[i + 1]);
            for (int j = i; j < argc - 2; j++) {
                argv[j] = argv[j + 2];
            }
            argc -= 2;
            i--;
        } else if (strcmp(argv[i], "--dump-clone-plan") == 0) {
            /* Phase B5: dump cloneable capture plan after CPS */
            g_dump_clone_plan = true;
            for (int j = i; j < argc - 1; j++) {
                argv[j] = argv[j + 1];
            }
            argc--;
            i--;
        } else if (strcmp(argv[i], "--dump-cps-coloring") == 0) {
            /* CPS1: dump CPS coloring (colored/uncolored) after cps_transform */
            g_dump_cps_coloring = true;
            for (int j = i; j < argc - 1; j++) {
                argv[j] = argv[j + 1];
            }
            argc--;
            i--;
        } else if (strcmp(argv[i], "--cps-path") == 0) {
            /* CPS3: emit CPS wrappers for colored functions */
            g_cps_path = true;
            for (int j = i; j < argc - 1; j++) {
                argv[j] = argv[j + 1];
            }
            argc--;
            i--;
        } else if (strcmp(argv[i], "--emit-abi-trace") == 0) {
            /* Phase I: trace the resolved ABI path for each call site during emit */
            g_emit_abi_trace = true;
            for (int j = i; j < argc - 1; j++) {
                argv[j] = argv[j + 1];
            }
            argc--;
            i--;
        } else if (is_known_deprecated_x_flag(argv[i])) {
            /* drop-x-flags-plan: every -X<name> is an accept-and-warn no-op
             * as of v0.24.0.  TUR-W0050 emitted; the underlying feature is
             * always on (see src/runtime/globals.c). */
            warn_deprecated_x_flag(argv[i]);
            for (int j = i; j < argc - 1; j++) {
                argv[j] = argv[j + 1];
            }
            argc--;
            i--;
        } else if (strcmp(argv[i], "--keep-contracts") == 0) {
            /* CT3: keep contract checks in release builds */
            g_keep_contracts_in_release = true;
            for (int j = i; j < argc - 1; j++) {
                argv[j] = argv[j + 1];
            }
            argc--;
            i--;
        } else if (strncmp(argv[i], "--enable=", 9) == 0) {
            /* XF1: opt in to one or more experimental features (comma list).
             * An unknown name is a hard TUR-E0310 error. */
            if (!enable_experiment_list(argv[i] + 9, XF_SRC_CLI)) return 2;
            for (int j = i; j < argc - 1; j++) {
                argv[j] = argv[j + 1];
            }
            argc--;
            i--;
        } else if (strncmp(argv[i], "--macro-caps=", 13) == 0) {
            /* Stage 3 (macro-system-direction-plan): grant the macro-time
             * env extra capabilities.  Only `io` exists; anything else --
             * including a plea for ffi/unsafe -- is a hard error, because
             * those are never offered at macro time. */
            if (strcmp(argv[i] + 13, "io") != 0) {
                fprintf(stderr,
                        "error: --macro-caps only accepts 'io' "
                        "(got '%s'); ffi/unsafe/inline-c/async are never "
                        "available at macro-expansion time\n",
                        argv[i] + 13);
                return 2;
            }
            g_macro_caps_io = true;
            for (int j = i; j < argc - 1; j++) {
                argv[j] = argv[j + 1];
            }
            argc--;
            i--;
        } else if (strcmp(argv[i], "--no-proc-macros") == 0) {
            /* security-audit-plan WP3: no defmacro* body runs. */
            g_no_proc_macros = true;
            for (int j = i; j < argc - 1; j++) {
                argv[j] = argv[j + 1];
            }
            argc--;
            i--;
        } else if (strcmp(argv[i], "--allow-experimental") == 0) {
            /* UC-4 (user-config-experiments-plan): --allow-experimental was
             * retired.  Enabling an experiment (via --enable=<name>,
             * build.tur, or ~/.config/turmeric/experiments.tur) is now itself
             * the acknowledgment; the separate gate added nothing.  Reject it
             * with a targeted message for one release rather than silently
             * accepting or emitting a generic "unknown flag". */
            fprintf(stderr,
                    "error: --allow-experimental was retired in v" TUR_VERSION
                    "; enabling an experiment (via --enable=<name>, build.tur, "
                    "or ~/.config/turmeric/experiments.tur) is now the "
                    "acknowledgment. Remove the flag.\n");
            return 2;
        } else if (strcmp(argv[i], "--no-color") == 0) {
            /* no-color-flag-rejected-everywhere: already parsed into
             * `no_color`; it was never stripped, so a trailing one reached
             * the command's parser as an unknown flag and a leading one was
             * taken for the command name. */
            for (int j = i; j < argc - 1; j++) {
                argv[j] = argv[j + 1];
            }
            argc--;
            i--;
        }
    }
    /* Put the program's words back (see dd_at above). */
    if (dd_tail > 0 && argc != dd_at)
        memmove(&argv[argc], &argv[dd_at], (size_t)dd_tail * sizeof(argv[0]));
    argc += dd_tail;
    argv[argc] = NULL;
    
        /* Initialize diagnostics - use color unless --no-color or --json-diagnostics specified */
        /* JSON output disables color */
        if (use_json_output) use_json_diagnostics = true;
        diag_init(!no_color && !use_json_diagnostics && stderr_is_tty());
        diag_set_json_output(use_json_diagnostics);
    
    if (explain_mode) {
        if (!explain_code) {
            fprintf(stderr, "tur: --explain requires a code snippet argument\n");
            return usage();
        }
        return cmd_explain(explain_code);
    }
    
    /* E2: --version / -V / -v */
    if (argc >= 2 && (strcmp(argv[1], "--version") == 0
                   || strcmp(argv[1], "-V") == 0
                   || strcmp(argv[1], "-v") == 0)) {
        printf("tur: the Turmeric compiler (v" TUR_VERSION ")\n");
        printf("Run `tur --help` for more commands.\n");
        return 0;
    }

    /* E1: --help / -h at top level */
    if (argc >= 2 && (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-h") == 0)) {
        usage();
        return 0;
    }

    if (argc < 2) return usage();
    const char *cmd = argv[1];

    {
        const char *resolved = resolve_command(cmd);
        if (resolved == COMMAND_AMBIGUOUS) return usage();
        if (resolved != NULL) cmd = resolved;
    }

    if (strcmp(cmd, "emit-c") == 0) {
        /* SC2: collect -I flags up front so both emit-c forms see them. */
        char **emit_inc = NULL;
        int    n_emit_inc = drv_parse_include_flags(argc, argv, 2, &emit_inc);
        if (n_emit_inc < 0) { free(emit_inc); return drv_usage_error(usage_build); }
        if (has_help_flag(argc, argv)) { free(emit_inc); return usage_build(); }

        /* tur emit-c [-I <dir>...] [--output-dir <dir> | --build-dir <dir> | -B <dir>]
         *            <file1> [<file2> ...]
         * build-output-directory-plan: --build-dir / -B are the new spellings;
         * --output-dir stays as a deprecated alias. */
        int od_idx = -1;
        for (int i = 2; i < argc; i++) {
            int c;
            if (drv_is_include_flag(argc, argv, i, &c)) { i += c - 1; continue; }
            if (strcmp(argv[i], "--output-dir") == 0 ||
                strcmp(argv[i], "--build-dir")  == 0 ||
                strcmp(argv[i], "-B")            == 0) { od_idx = i; break; }
        }
        if (od_idx >= 0) {
            if (od_idx + 1 >= argc) { free(emit_inc); return drv_usage_error(usage_build); }
            const char *out_dir = argv[od_idx + 1];
            /* Inputs are every non-flag arg that isn't --output-dir or its
             * value or a -I value.  Build a clean inputs list. */
            char **inputs = (char **)malloc((size_t)argc * sizeof(char *));
            int    n_inputs = 0;
            for (int i = 2; i < argc; i++) {
                int c;
                if (drv_is_include_flag(argc, argv, i, &c)) { i += c - 1; continue; }
                if (i == od_idx) { i++; continue; }   /* skip --output-dir and its value */
                if (strcmp(argv[i], "--no-abi-cache") == 0) continue; /* J6: global, skip */
                if (strcmp(argv[i], "--no-r7rs-gc") == 0) continue;   /* r7rs-gc: global, skip */
                if (strcmp(argv[i], "--no-saffron-gc") == 0) continue;
                if (argv[i][0] == '-') { free(inputs); free(emit_inc); return drv_usage_error(usage_build); }
                inputs[n_inputs++] = argv[i];
            }
            /* SC4+SC5: auto-discover spice src + cross-spice deps for the
             * FIRST input only.  For the multi-file --output-dir form,
             * that input typically lives inside the same spice as the
             * others; if it doesn't, an explicit -I is the right escape
             * hatch. */
            char **md_owned = NULL;
            int    n_md_owned = 0;
            Ls2ResolverCtx md_ls2 = {0};
            if (n_inputs > 0) {
                auto_append_spice_includes(inputs[0], &emit_inc, &n_emit_inc,
                                           &md_owned, &n_md_owned, &md_ls2);
            }
            ls2_resolver_ctx_set(&md_ls2);
            int rc = cmd_emit_c_to_dir(out_dir, inputs, n_inputs,
                                       (const char **)emit_inc, n_emit_inc);
            ls2_resolver_ctx_set(NULL);
            ls2_resolver_ctx_dispose(&md_ls2);
            for (int i = 0; i < n_md_owned; i++) free(md_owned[i]);
            free(md_owned);
            free(inputs);
            free(emit_inc);
            return rc;
        }

        /* tur emit-c [-I <dir>...] <file>   single file to stdout (legacy) */
        const char *input = NULL;
        for (int i = 2; i < argc; i++) {
            int c;
            if (drv_is_include_flag(argc, argv, i, &c)) { i += c - 1; continue; }
            if (argv[i][0] != '-') {
                if (input) { free(emit_inc); return drv_usage_error(usage_build); }
                input = argv[i];
                continue;
            }
            free(emit_inc); return drv_usage_error(usage_build);
        }
        if (!input) { free(emit_inc); return drv_usage_error(usage_build); }
        char **ec_owned = NULL; int n_ec_owned = 0;
        Ls2ResolverCtx ec_ls2 = {0};
        auto_append_spice_includes(input, &emit_inc, &n_emit_inc,
                                   &ec_owned, &n_ec_owned, &ec_ls2);
        ls2_resolver_ctx_set(&ec_ls2);
        int rc = cmd_emit_c(input, (const char **)emit_inc, n_emit_inc);
        ls2_resolver_ctx_set(NULL);
        ls2_resolver_ctx_dispose(&ec_ls2);
        for (int i = 0; i < n_ec_owned; i++) free(ec_owned[i]);
        free(ec_owned);
        free(emit_inc);
        return rc;
    }
    if (strcmp(cmd, "emit-h") == 0) {
        /* SC2: accept -I just like emit-c / check / build.
         * SC4+SC5: auto-discover enclosing spice src/ and dep src/. */
        char **eh_inc = NULL;
        int    n_eh_inc = drv_parse_include_flags(argc, argv, 2, &eh_inc);
        if (n_eh_inc < 0) { free(eh_inc); return drv_usage_error(usage_build); }
        if (has_help_flag(argc, argv)) { free(eh_inc); return usage_build(); }
        const char *input = NULL;
        for (int i = 2; i < argc; i++) {
            int c;
            if (drv_is_include_flag(argc, argv, i, &c)) { i += c - 1; continue; }
            if (argv[i][0] != '-') {
                if (input) { free(eh_inc); return drv_usage_error(usage_build); }
                input = argv[i];
                continue;
            }
            free(eh_inc); return drv_usage_error(usage_build);
        }
        if (!input) { free(eh_inc); return drv_usage_error(usage_build); }
        char **eh_owned = NULL; int n_eh_owned = 0;
        Ls2ResolverCtx eh_ls2 = {0};
        auto_append_spice_includes(input, &eh_inc, &n_eh_inc,
                                   &eh_owned, &n_eh_owned, &eh_ls2);
        ls2_resolver_ctx_set(&eh_ls2);
        int rc = cmd_emit_h(input, (const char **)eh_inc, n_eh_inc);
        ls2_resolver_ctx_set(NULL);
        ls2_resolver_ctx_dispose(&eh_ls2);
        for (int i = 0; i < n_eh_owned; i++) free(eh_owned[i]);
        free(eh_owned);
        free(eh_inc);
        return rc;
    }
    if (strcmp(cmd, "check") == 0) {
        /* Phase 8: tur check subcommand - type-check only, no codegen
         * SC1: now accepts -I <dir> flags so per-file checks inside a spice
         * resolve their intra-spice imports the same way `tur build` does.
         * SC4: walks up to find an enclosing build.tur and auto-adds its
         * `src/` so editors / format-on-save don't need explicit -I. */
        char       **check_inc = NULL;
        int          n_check_inc = drv_parse_include_flags(argc, argv, 2, &check_inc);
        if (n_check_inc < 0) { free(check_inc); return drv_usage_error(usage_check); }
        const char *input = NULL;
        for (int i = 2; i < argc; i++) {
            if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
                free(check_inc); return usage_check();
            }
            int c;
            if (drv_is_include_flag(argc, argv, i, &c)) { i += c - 1; continue; }
            if (strcmp(argv[i], "--no-auto-spice") == 0) continue;
            if (strcmp(argv[i], "--no-auto-stdlib") == 0) {
                g_no_auto_stdlib = true;
                continue;
            }
            if (argv[i][0] != '-') {
                if (input) { free(check_inc); return drv_usage_error(usage_check); }
                input = argv[i];
                continue;
            }
            free(check_inc); return drv_usage_error(usage_check);
        }
        if (!input) { free(check_inc); return drv_usage_error(usage_check); }
        /* A directory argument checks every .tur file under it (project /
         * spice mode), mirroring `tur test <dir>`. */
        if (drv_is_directory(input)) {
            free(check_inc);
            return cmd_check_dir(input);
        }
        char **ck_owned = NULL; int n_ck_owned = 0;
        Ls2ResolverCtx ck_ls2 = {0};
        auto_append_spice_includes(input, &check_inc, &n_check_inc,
                                   &ck_owned, &n_ck_owned, &ck_ls2);
        ls2_resolver_ctx_set(&ck_ls2);
        int rc;
        int rm_n = 0;
        char **rm_p = discover_manifest_reader_macros(input, &rm_n);
        if (use_json_output) {
            diag_lsp_begin();
            Buf out;
            buf_init(&out);
            compile_to_c(input, &out, (const char **)check_inc, n_check_inc,
                         (const char **)rm_p, rm_n);
            buf_free(&out);
            diag_lsp_flush(stdout);
            diag_lsp_end();
            rc = diag_had_error() ? 1 : 0;
        } else {
            Buf out;
            buf_init(&out);
            rc = compile_to_c(input, &out, (const char **)check_inc, n_check_inc,
                              (const char **)rm_p, rm_n);
            buf_free(&out);
        }
        ls2_resolver_ctx_set(NULL);
        ls2_resolver_ctx_dispose(&ck_ls2);
        free_reader_macro_paths(rm_p, rm_n);
        for (int i = 0; i < n_ck_owned; i++) free(ck_owned[i]);
        free(ck_owned);
        free(check_inc);
        return rc;
    }
    if (strcmp(cmd, "expand") == 0) {
        /* `tur expand`: type-check like `tur check`, but with the expansion
         * trace on -- every macro expansion outside the stdlib preload prints
         * to stdout (see the g_dump_expansion site in elab_call.c).  Same
         * include-path plumbing as `tur check`. */
        char       **exp_inc = NULL;
        int          n_exp_inc = drv_parse_include_flags(argc, argv, 2, &exp_inc);
        if (n_exp_inc < 0) { free(exp_inc); return drv_usage_error(usage_expand); }
        const char *input = NULL;
        for (int i = 2; i < argc; i++) {
            if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
                free(exp_inc); return usage_expand();
            }
            int c;
            if (drv_is_include_flag(argc, argv, i, &c)) { i += c - 1; continue; }
            if (strcmp(argv[i], "--no-auto-spice") == 0) continue;
            if (strcmp(argv[i], "--no-auto-stdlib") == 0) {
                g_no_auto_stdlib = true;
                continue;
            }
            if (argv[i][0] != '-') {
                if (input) { free(exp_inc); return drv_usage_error(usage_expand); }
                input = argv[i];
                continue;
            }
            free(exp_inc); return drv_usage_error(usage_expand);
        }
        if (!input) { free(exp_inc); return drv_usage_error(usage_expand); }
        char **exp_owned = NULL; int n_exp_owned = 0;
        Ls2ResolverCtx exp_ls2 = {0};
        auto_append_spice_includes(input, &exp_inc, &n_exp_inc,
                                   &exp_owned, &n_exp_owned, &exp_ls2);
        ls2_resolver_ctx_set(&exp_ls2);
        int rm_n = 0;
        char **rm_p = discover_manifest_reader_macros(input, &rm_n);
        g_dump_expansion = true;
        Buf out;
        buf_init(&out);
        int rc = compile_to_c(input, &out, (const char **)exp_inc, n_exp_inc,
                              (const char **)rm_p, rm_n);
        buf_free(&out);
        g_dump_expansion = false;
        ls2_resolver_ctx_set(NULL);
        ls2_resolver_ctx_dispose(&exp_ls2);
        free_reader_macro_paths(rm_p, rm_n);
        for (int i = 0; i < n_exp_owned; i++) free(exp_owned[i]);
        free(exp_owned);
        free(exp_inc);
        return rc;
    }
    if (strcmp(cmd, "audit-spans") == 0) {
        /* debugger Phase 1: elaborate <file> and report breakpoint-eligible
         * AST nodes (top-level forms, defns, let forms, call sites) that lack
         * a usable source span.  Auto-discovers the enclosing spice src/ and
         * -I flags exactly like `tur check`, so intra-spice imports resolve.
         * Exit codes: 0 = clean, 3 = holes found, 1 = could not elaborate,
         * 2 = file error. */
        char       **as_inc = NULL;
        int          n_as_inc = drv_parse_include_flags(argc, argv, 2, &as_inc);
        if (n_as_inc < 0) { free(as_inc); return drv_usage_error(usage_check); }
        const char *input = NULL;
        for (int i = 2; i < argc; i++) {
            if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
                free(as_inc); return usage_check();
            }
            int c;
            if (drv_is_include_flag(argc, argv, i, &c)) { i += c - 1; continue; }
            if (strcmp(argv[i], "--no-auto-spice") == 0) continue;
            if (strcmp(argv[i], "--no-auto-stdlib") == 0) {
                g_no_auto_stdlib = true;
                continue;
            }
            if (argv[i][0] != '-') {
                if (input) { free(as_inc); return drv_usage_error(usage_check); }
                input = argv[i];
                continue;
            }
            free(as_inc); return drv_usage_error(usage_check);
        }
        if (!input) { free(as_inc); return drv_usage_error(usage_check); }
        char **as_owned = NULL; int n_as_owned = 0;
        Ls2ResolverCtx as_ls2 = {0};
        auto_append_spice_includes(input, &as_inc, &n_as_inc,
                                   &as_owned, &n_as_owned, &as_ls2);
        ls2_resolver_ctx_set(&as_ls2);
        int rm_n = 0;
        char **rm_p = discover_manifest_reader_macros(input, &rm_n);
        Buf out;
        buf_init(&out);
        g_audit_spans = true;
        g_audit_span_holes = 0;
        int rc = compile_to_c(input, &out, (const char **)as_inc, n_as_inc,
                              (const char **)rm_p, rm_n);
        g_audit_spans = false;
        buf_free(&out);
        ls2_resolver_ctx_set(NULL);
        ls2_resolver_ctx_dispose(&as_ls2);
        free_reader_macro_paths(rm_p, rm_n);
        for (int i = 0; i < n_as_owned; i++) free(as_owned[i]);
        free(as_owned);
        free(as_inc);
        return rc;
    }
    if (strcmp(cmd, "lsp") == 0) {
        diag_init(false);   /* no color -- stdout is reserved for JSON-RPC */
        for (int i = 2; i < argc; i++) {
            if (strcmp(argv[i], "--rename-exports") == 0)
                lsp_set_rename_exports(true);
        }
        lsp_server_run(STDIN_FILENO, STDOUT_FILENO);
        return 0;
    }
    if (strcmp(cmd, "mcp") == 0) {
        diag_init(false);   /* no color -- stdout is reserved for JSON-RPC */
        mcp_server_run(STDIN_FILENO, STDOUT_FILENO);
        return 0;
    }
    /* Debugger Phase 3: DAP server over the interpreter (JSON-RPC / stdio). */
    if (strcmp(cmd, "dap") == 0) {
        return cmd_dap();
    }
    /* Track T1: record an interpreted run, or read a recording back. */
    if (strcmp(cmd, "trace") == 0) {
        const char *file = NULL, *out_path = NULL;
        uint32_t    max_steps = 0;
        bool        dump = false;
        TurTraceGrain grain = TUR_TRACE_GRAIN_NODE;
        char      **prog_argv = NULL;
        int         prog_argc = 0;
        for (int i = 2; i < argc; i++) {
            if (strcmp(argv[i], "--dump") == 0) { dump = true; continue; }
            if (strcmp(argv[i], "-o") == 0 && i + 1 < argc) {
                out_path = argv[++i];
                continue;
            }
            if (strncmp(argv[i], "--max-steps=", 12) == 0) {
                max_steps = (uint32_t)strtoul(argv[i] + 12, NULL, 10);
                continue;
            }
            /* The old granularity, for a program too big to record per
             * expression under the cap.  Not the default: see TurTraceGrain. */
            if (strcmp(argv[i], "--lines") == 0) {
                grain = TUR_TRACE_GRAIN_LINE; continue;
            }
            if (!file) { file = argv[i]; continue; }
            /* Everything after the program is the program's own argv. */
            prog_argv = &argv[i];
            prog_argc = argc - i;
            break;
        }
        if (!file) {
            fprintf(stderr,
                "usage: tur trace <file.tur> [-o out.turtrace] "
                "[--max-steps=N] [--lines] [-- args...]\n"
                "       tur trace --dump <file.turtrace>\n"
                "\n"
                "  A step is one expression.  --lines records one step per\n"
                "  source line instead, which is coarser: nested calls on one\n"
                "  line collapse into a single step.\n");
            return 1;
        }
        if (dump) return cmd_trace_dump(file);
        return cmd_trace(file, out_path, prog_argv, prog_argc, max_steps, grain);
    }
    /* lsp-lite: completion/calltip/doc backend for lightweight editors.
     * Newline-delimited JSON over stdio; stdout is reserved for protocol
     * traffic. */
    if (strcmp(cmd, "lsp-lite") == 0) {
        diag_init(false);
        resolve_stdlib_root();   /* sets TUR_STDLIB_DIR so lsp_lite can find docstrings.tur */
        return lsp_lite_run(STDIN_FILENO, STDOUT_FILENO);
    }
    if (strcmp(cmd, "build") == 0) {
        const char *input = NULL;
        const char *out = NULL;
        const char *build_target = NULL;
        bool        shared = false;  /* RP0: --shared selects shared-library build */
        const char *manifest_out = NULL; /* RP1: --manifest <path> override */
        const char *cli_build_dir = NULL; /* build-output-directory-plan: --build-dir/-B */
        /* tur-link-and-build-split-plan Phase 3b/4: opt in to the compile+link
         * split for a single-file build.  Default stays the monolithic `cc`
         * call so output is byte-identical until the split is proven; the two
         * flags let a build force either path during rollout. */
        int split_build = 0;  /* 0 = default(monolithic), 1 = split, -1 = forced monolithic */
        /* SC1: collect -I flags once via the shared helper, then walk argv
         * a second time for the build-specific flags (`-o`, `--target`)
         * and the positional input. */
        char  **build_inc = NULL;
        int     n_build_inc = drv_parse_include_flags(argc, argv, 2, &build_inc);
        if (n_build_inc < 0) { free(build_inc); return drv_usage_error(usage_build); }
        for (int i = 2; i < argc; i++) {
            int c;
            if (drv_is_include_flag(argc, argv, i, &c)) { i += c - 1; continue; }
            if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
                free(build_inc); return usage_build();
            } else if (strcmp(argv[i], "-o") == 0 && i + 1 < argc) {
                out = argv[++i];
            } else if (strcmp(argv[i], "--shared") == 0) {
                shared = true;
            } else if (strcmp(argv[i], "--bundle-deps") == 0) {
                g_build_bundle_deps = true;
            } else if (strcmp(argv[i], "--release") == 0) {
                g_cc_release = true;
            } else if (strcmp(argv[i], "--split-build") == 0) {
                split_build = 1;
            } else if (strcmp(argv[i], "--no-split-build") == 0) {
                split_build = -1;
            } else if (strncmp(argv[i], "--runtime=", 10) == 0 ||
                       (strcmp(argv[i], "--runtime") == 0 && i + 1 < argc)) {
                const char *mode = argv[i][9] == '=' ? argv[i] + 10 : argv[++i];
                if (strcmp(mode, "lib") == 0) g_runtime_mode = TUR_RT_LIB;
                else if (strcmp(mode, "source") == 0) g_runtime_mode = TUR_RT_SOURCE;
                else if (strcmp(mode, "auto") == 0) g_runtime_mode = TUR_RT_AUTO;
                else if (strcmp(mode, "split") == 0) g_runtime_mode = TUR_RT_SPLIT;
                else {
                    fprintf(stderr, "tur build: unknown --runtime '%s' "
                            "(supported: auto, lib, source, split)\n", mode);
                    free(build_inc); return 1;
                }
            } else if (strcmp(argv[i], "--no-abi-cache") == 0) {
                /* J6: consumed globally by parse_no_abi_cache; no-op here. */
            } else if (strcmp(argv[i], "--no-r7rs-gc") == 0 ||
                       strcmp(argv[i], "--no-saffron-gc") == 0) {
                /* r7rs-gc: consumed globally by parse_no_r7rs_gc /
                 * parse_no_saffron_gc; no-op here. */
            } else if (strcmp(argv[i], "--manifest") == 0 && i + 1 < argc) {
                manifest_out = argv[++i];
            } else if ((strcmp(argv[i], "--build-dir") == 0 ||
                        strcmp(argv[i], "-B") == 0) && i + 1 < argc) {
                cli_build_dir = argv[++i];
            } else if (strcmp(argv[i], "--target") == 0 && i + 1 < argc) {
                build_target = argv[++i];
                if (strcmp(build_target, "wasm") != 0) {
                    fprintf(stderr, "tur build: unknown target '%s' (supported: wasm)\n", build_target);
                    free(build_inc); return 1;
                }
            } else if (argv[i][0] != '-') {
                if (input) { free(build_inc); return drv_usage_error(usage_build); }
                input = argv[i];
            } else {
                free(build_inc); return drv_usage_error(usage_build);
            }
        }
        if (!input) { free(build_inc); return drv_usage_error(usage_build); }
        /* RP0: --shared only supports directory input -- the single-file
         * build path emits a `static`-by-default function and a main(), so
         * dlsym wouldn't find anything useful. Directory mode runs through
         * compile_to_implementation in separate-compilation mode, which
         * gives exported defns extern linkage. */
        if (shared && !drv_is_directory(input)) {
            fprintf(stderr, "tur build: --shared requires a directory argument "
                            "(single-file builds emit static symbols)\n");
            free(build_inc); return 1;
        }
        if (shared && build_target) {
            fprintf(stderr, "tur build: --shared and --target are mutually exclusive\n");
            free(build_inc); return 1;
        }
        if (manifest_out && !shared) {
            fprintf(stderr, "tur build: --manifest requires --shared\n");
            free(build_inc); return 1;
        }
        if (g_build_bundle_deps && !shared) {
            fprintf(stderr, "tur build: --bundle-deps requires --shared\n");
            free(build_inc); return 1;
        }
        /* Check if input is a directory - use multi-file build */
        int rc;
        if (drv_is_directory(input)) {
            /* Manifest-driven project build when the directory carries a
             * build.tur: read the manifest, descend into src/, and resolve
             * the include path (own src/ + each :spices dep). Otherwise fall
             * back to the bare-directory glob. */
            char proj_manifest[4096];
            snprintf(proj_manifest, sizeof(proj_manifest),
                     "%s/build.tur", input);
            struct stat mst;
            if (stat(proj_manifest, &mst) == 0 && S_ISREG(mst.st_mode)) {
                rc = cmd_build_project(input, out, shared, manifest_out,
                                       (const char **)build_inc, n_build_inc,
                                       cli_build_dir, build_target);
            } else {
                rc = cmd_build_multi(input, out, shared, manifest_out,
                                     cli_build_dir);
            }
        } else {
            /* RM4: auto-discover the spice manifest containing `input` so
             * `:reader-macros [...]` entries apply when building a single
             * file from inside a project. No-op when there is no manifest.
             * The shared helper also applies the manifest's `:experiments`
             * (and the user experiments file): a hand-rolled copy of this
             * walk skipped them, so `tur build f.tur` compiled with a
             * project's gates off where `tur check f.tur` had them on. */
            char **b_rm  = NULL;
            int    b_n   = 0;
            b_rm = discover_manifest_reader_macros(input, &b_n);
            /* Mirror emit-c / emit-h / check / run: widen the include
             * path with the enclosing spice's src/ and every declared
             * `:spices` dep's src/. Short-circuits when --no-auto-spice
             * is set or there is no enclosing build.tur. */
            char **b_owned = NULL; int n_b_owned = 0;
            Ls2ResolverCtx b_ls2 = {0};
            auto_append_spice_includes(input, &build_inc, &n_build_inc,
                                       &b_owned, &n_b_owned, &b_ls2);
            ls2_resolver_ctx_set(&b_ls2);
            if (split_build == 1 && !build_target) {
                /* tur-link-and-build-split-plan Phase 3b: `tur build` defined as
                 * `tur compile` + `tur link` composed -- the exact subcommand
                 * code paths, so the three can never drift.  The generated
                 * object + `.link` sidecar land under the build temp dir. */
                char base[512];
                drv_default_output_name(input, base, sizeof(base));
                char objp[1200];
                snprintf(objp, sizeof(objp), "%s%s.o", drv_stable_c_prefix(), base);
                rc = cmd_compile(input, objp, (const char **)build_inc,
                                 n_build_inc, (const char **)b_rm, b_n);
                if (rc == 0) {
                    const char *outp = out;
                    char outbuf[1024];
                    if (!outp) {
                        drv_default_output_name(input, outbuf, sizeof(outbuf));
                        outp = outbuf;
                    }
                    const char *lk[1] = { objp };
                    rc = cmd_link(outp, lk, 1, false, NULL);
                }
                /* Drop the intermediate .o/.c/.link (ccache keyed on content
                 * already caches the underlying object compile). */
                char scrap[1300];
                unlink(objp);
                drv_obj_sibling_path(objp, ".c",    scrap, sizeof(scrap)); unlink(scrap);
                drv_obj_sibling_path(objp, ".link", scrap, sizeof(scrap)); unlink(scrap);
            } else {
                rc = cmd_build(input, out, (const char **)build_inc, n_build_inc,
                               build_target, (const char **)b_rm, b_n);
            }
            ls2_resolver_ctx_set(NULL);
            ls2_resolver_ctx_dispose(&b_ls2);
            for (int i = 0; i < n_b_owned; i++) free(b_owned[i]);
            free(b_owned);
            free_reader_macro_paths(b_rm, b_n);
        }
        free(build_inc);
        return rc;
    }
    /* tur-link-and-build-split-plan Phase 3a-0: tur compile <in.tur> -o <out.o> */
    if (strcmp(cmd, "compile") == 0) {
        const char *input = NULL;
        const char *out = NULL;
        const char *cli_build_dir = NULL;
        char  **comp_inc = NULL;
        int     n_comp_inc = drv_parse_include_flags(argc, argv, 2, &comp_inc);
        if (n_comp_inc < 0) { free(comp_inc); return drv_usage_error(usage_build); }
        for (int i = 2; i < argc; i++) {
            int c;
            if (drv_is_include_flag(argc, argv, i, &c)) { i += c - 1; continue; }
            if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
                free(comp_inc); return usage_build();
            } else if (strcmp(argv[i], "-o") == 0 && i + 1 < argc) {
                out = argv[++i];
            } else if ((strcmp(argv[i], "--build-dir") == 0 ||
                        strcmp(argv[i], "-B") == 0) && i + 1 < argc) {
                cli_build_dir = argv[++i];
            } else if (strncmp(argv[i], "--runtime=", 10) == 0 ||
                       (strcmp(argv[i], "--runtime") == 0 && i + 1 < argc)) {
                const char *mode = argv[i][9] == '=' ? argv[i] + 10 : argv[++i];
                if (strcmp(mode, "lib") == 0) g_runtime_mode = TUR_RT_LIB;
                else if (strcmp(mode, "source") == 0) g_runtime_mode = TUR_RT_SOURCE;
                else if (strcmp(mode, "auto") == 0) g_runtime_mode = TUR_RT_AUTO;
                else {
                    fprintf(stderr, "tur compile: unknown --runtime '%s' "
                            "(supported: auto, lib, source)\n", mode);
                    free(comp_inc); return 1;
                }
            } else if (strcmp(argv[i], "--no-abi-cache") == 0 ||
                       strcmp(argv[i], "--no-r7rs-gc") == 0 ||
                       strcmp(argv[i], "--no-saffron-gc") == 0) {
                /* global, consumed elsewhere */
            } else if (argv[i][0] != '-') {
                if (input) { free(comp_inc); return drv_usage_error(usage_build); }
                input = argv[i];
            } else {
                free(comp_inc); return drv_usage_error(usage_build);
            }
        }
        if (!input) { free(comp_inc); return drv_usage_error(usage_build); }
        if (drv_is_directory(input)) {
            fprintf(stderr, "tur compile: expects a single .tur file, not a directory\n");
            free(comp_inc); return 1;
        }
        /* Default the object under <build-dir>/obj/ when --build-dir/-B (or a
         * manifest :build-dir) applies and no explicit -o was given. */
        char obj_default[1200];
        if (!out) {
            char *bd = drv_resolve_build_dir(input, cli_build_dir);
            if (bd) {
                char base[512];
                drv_default_output_name(input, base, sizeof(base));
                snprintf(obj_default, sizeof(obj_default), "%s/obj/%s.o", bd, base);
                out = obj_default;
                free(bd);
            }
        }
        /* Reader macros, experiments + spice include auto-discovery,
         * mirroring cmd_build. */
        char **c_rm = NULL; int c_n = 0;
        c_rm = discover_manifest_reader_macros(input, &c_n);
        char **c_owned = NULL; int n_c_owned = 0;
        Ls2ResolverCtx c_ls2 = {0};
        auto_append_spice_includes(input, &comp_inc, &n_comp_inc,
                                   &c_owned, &n_c_owned, &c_ls2);
        ls2_resolver_ctx_set(&c_ls2);
        int rc = cmd_compile(input, out, (const char **)comp_inc, n_comp_inc,
                             (const char **)c_rm, c_n);
        ls2_resolver_ctx_set(NULL);
        ls2_resolver_ctx_dispose(&c_ls2);
        for (int i = 0; i < n_c_owned; i++) free(c_owned[i]);
        free(c_owned);
        free_reader_macro_paths(c_rm, c_n);
        free(comp_inc);
        return rc;
    }
    /* tur-link-and-build-split-plan Phase 3a: tur link <obj/src>... -o <out> */
    if (strcmp(cmd, "link") == 0) {
        const char *out = NULL;
        bool shared = false;
        const char *link_flags = NULL;
        const char **inputs = (const char **)malloc((size_t)argc * sizeof(char *));
        int n_inputs = 0;
        for (int i = 2; i < argc; i++) {
            if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
                free(inputs); return usage_build();
            } else if (strcmp(argv[i], "-o") == 0 && i + 1 < argc) {
                out = argv[++i];
            } else if (strcmp(argv[i], "--shared") == 0) {
                shared = true;
            } else if (strcmp(argv[i], "--link-flags") == 0 && i + 1 < argc) {
                link_flags = argv[++i];
            } else if (argv[i][0] != '-') {
                inputs[n_inputs++] = argv[i];
            } else {
                fprintf(stderr, "tur link: unknown option '%s'\n", argv[i]);
                free(inputs); return drv_usage_error(usage_build);
            }
        }
        if (n_inputs == 0) { free(inputs); return drv_usage_error(usage_build); }
        int rc = cmd_link(out, inputs, n_inputs, shared, link_flags);
        free(inputs);
        return rc;
    }
    if (strcmp(cmd, "jit") == 0) {
        return cmd_jit(argc, argv);
    }
    if (strcmp(cmd, "emit-rt-split") == 0) {
        return cmd_emit_rt_split(argc, argv);
    }
    if (strcmp(cmd, "run") == 0) {
        /* Disambiguate: if the first non-flag argument ends in .tur or
         * .tur.sweet, use the classic compile-and-run path; if --release /
         * -I flags appear (compile-only flags), also use classic path.
         * Otherwise dispatch to the Justfile task runner (RN0-RN7). */
        bool use_classic = false;
        for (int i = 2; i < argc; i++) {
            if (strcmp(argv[i], "--") == 0) break;
            if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
                /* Default help to the task runner; it documents both modes. */
                return usage_justrun();
            }
            if (strcmp(argv[i], "--release") == 0 ||
                strcmp(argv[i], "--offline") == 0  ||
                strcmp(argv[i], "--engine") == 0   ||
                strncmp(argv[i], "--engine=", 9) == 0 ||
                strncmp(argv[i], "-I", 2) == 0) {
                /* --engine is a classic-path (compile/run) flag; without this
                 * arm its VALUE would be taken as a Justfile task name. */
                use_classic = true;
                break;
            }
            if (argv[i][0] != '-' || strcmp(argv[i], "-") == 0) {
                /* Check if it looks like a .tur file path */
                const char *a = argv[i];
                size_t an = strlen(a);
                if ((an > 4  && strcmp(a + an - 4,  ".tur")      == 0) ||
                    (an > 10 && strcmp(a + an - 10, ".tur.sweet") == 0) ||
                    (an > 4  && strcmp(a + an - 4,  ".scm")      == 0) ||
                    strcmp(a, "-") == 0) {
                    use_classic = true;
                }
                break;
            }
        }
        if (use_classic)
            return cmd_run(argc, argv);
        /* engine-selection-plan E3: a BARE `tur run` (no recipe, no file) in
         * a Justfile-less build.tur project takes the classic project-run
         * path -- which resolves the manifest's `:engine` -- instead of the
         * historical hard 127.  A named recipe keeps the task-runner error:
         * a typo'd task silently compiling-and-running would be worse. */
        {
            bool bare = true;
            for (int i = 2; i < argc; i++) {
                if (strcmp(argv[i], "--") == 0) break;
                if (argv[i][0] != '-') { bare = false; break; }
                if (strcmp(argv[i], "--justfile") == 0 ||
                    strcmp(argv[i], "--chdir") == 0) i++;   /* skip value */
            }
            if (bare && !justrun_finds_justfile()) {
                char _cwd[4096];
                if (getcwd(_cwd, sizeof(_cwd))) {
                    char *_root = drv_find_project_root(_cwd);
                    if (_root) {
                        free(_root);
                        return cmd_run(argc, argv);
                    }
                }
            }
        }
        return cmd_justrun(argc, argv);
    }
    if (strcmp(cmd, "repl") == 0) {
        /* Phase S0: interactive REPL.
         * RP6: --watch enables auto-reload between prompts when a
         * spice source file changes mtime. No background thread --
         * the freshness check runs synchronously each turn. */
        bool watch_mode = false;
        const char *repl_engine_flag = NULL;
        /* saffron-lang-plan S8: start the session in a dialect, rather than
         * making the user type `#lang saffron` as their first line.  Both
         * routes land on the same env switch. */
        const char *repl_lang_flag = NULL;
        for (int i = 2; i < argc; i++) {
            if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
                return usage_repl();
            }
            if (strcmp(argv[i], "--watch") == 0) {
                watch_mode = true;
                continue;
            }
            if (strcmp(argv[i], "--lang") == 0 && i + 1 < argc) {
                repl_lang_flag = argv[++i];
                continue;
            }
            if (strncmp(argv[i], "--lang=", 7) == 0) {
                repl_lang_flag = argv[i] + 7;
                continue;
            }
            if (strcmp(argv[i], "--engine") == 0 && i + 1 < argc) {
                repl_engine_flag = argv[++i];
                continue;
            }
            if (strncmp(argv[i], "--engine=", 9) == 0) {
                repl_engine_flag = argv[i] + 9;
                continue;
            }
            fprintf(stderr, "tur repl: unknown option '%s'\n", argv[i]);
            return drv_usage_error(usage_repl);
        }
        /* J2 (jit-engine-plan 3.3): spice auto-discovery can build in process
         * through the MIR engine instead of the `tur build --shared`
         * subprocess + dlopen.  This used to hang off `--enable=jit`; that
         * experiment graduated 2026-08-17, so it hangs off ENGINE SELECTION
         * now -- `--engine jit`, `TUR_ENGINE=jit`, or `:engine "jit"` in the
         * enclosing build.tur, the same ladder `tur run` resolves.
         *
         * Graduating the experiment deliberately did NOT make this the
         * default: unset, the engine resolves to "cc" and the subprocess path
         * is byte-for-byte what it was. */
        {
            const char *eng = drv_resolve_engine(".", repl_engine_flag);
            if (!eng) return 2;    /* TUR-E0311 already printed */
            if (strcmp(eng, "jit") == 0) {
#ifdef TUR_HAVE_JIT
                tur_spice_set_jit_hook(&g_repl_jit_hook);
#else
                fprintf(stderr,
                        "tur repl: engine \"jit\" is configured, but this "
                        "build carries no JIT engine\n"
                        "     reconfigure with -DTUR_JIT=ON (the default on "
                        "64-bit x86-64/arm64),\n"
                        "     or override the engine: --engine cc / "
                        "TUR_ENGINE=cc\n");
                return 2;
#endif
            }
        }
        /* Validated HERE rather than inside cmd_repl, so a typo is a usage
         * error before the banner prints and the stdlib preloads. */
        if (repl_lang_flag) {
            /* r7rs-lang-plan R9: any base `tur dialects` lists, resolved
             * through the same table the `#lang` line uses -- it was a
             * hard-coded "turmeric"/"saffron" pair, so `--lang r7rs` was
             * refused. */
            LangDialect ld;
            ReaderType lr;
            if (!lang_base_lookup(repl_lang_flag, strlen(repl_lang_flag), &ld, &lr)) {
                fprintf(stderr,
                        "tur repl: unknown --lang '%s' "
                        "(expected a base `tur dialects` lists, e.g. "
                        "\"turmeric\", \"saffron\" or \"r7rs\")\n",
                        repl_lang_flag);
                return drv_usage_error(usage_repl);
            }
            g_repl_start_lang = repl_lang_flag;
        }
        return cmd_repl(watch_mode);
    }
    /* Tier 3: persistent fixture worker for the test suite. */
    if (strcmp(cmd, "worker") == 0) {
        return cmd_worker();
    }
    if (strcmp(cmd, "interpret") == 0 || strcmp(cmd, "--interpret") == 0) {
        char **inc = NULL;
        int    n_inc = 0;
        int    fi = interp_leading_include_flags(argc, argv, 2, &inc, &n_inc);
        if (fi < 0) return 2;
        if (fi >= argc) {
            free(inc);
            fprintf(stderr, "tur: %s requires a file argument\n", cmd);
            return usage();
        }
        int rc = cmd_eval_inc(argv[fi], !no_color && stderr_is_tty(),
                              inc, n_inc, argv + fi + 1, argc - fi - 1,
                              /*debug=*/false);
        free(inc);
        return rc;
    }
    /* Debugger Phase 2: `tur debug <file.tur> [args...]` -- run a file through
     * the tree-walking interpreter under the interactive debugger.  Drops into
     * a command REPL at program entry; commands are read from stdin (so a
     * script can drive it).  See docs/archive/history/debugger-plan.md (Phase 2). */
    if (strcmp(cmd, "debug") == 0) {
        if (argc < 3 || strcmp(argv[2], "--help") == 0 || strcmp(argv[2], "-h") == 0) {
            fprintf(stderr,
                "usage:\n  tur debug <file.tur> [args...]\n"
                "\nDrops into an interactive debugger (break/step/next/finish/\n"
                "backtrace/locals/print/list/continue). Type 'help' at the\n"
                "(tur-dbg) prompt for the full command list.\n");
            return argc < 3 ? 1 : 0;
        }
        char **inc = NULL;
        int    n_inc = 0;
        int    fi = interp_leading_include_flags(argc, argv, 2, &inc, &n_inc);
        if (fi < 0) return 2;
        if (fi >= argc) {
            free(inc);
            fprintf(stderr, "tur: debug requires a file argument\n");
            return 1;
        }
        int rc = cmd_eval_inc(argv[fi], !no_color && stderr_is_tty(),
                              inc, n_inc, argv + fi + 1, argc - fi - 1,
                              /*debug=*/true);
        free(inc);
        return rc;
    }
    /* E3: tur eval '<expr>' or tur eval --file <file> */
    if (strcmp(cmd, "eval") == 0) {
        if (argc < 3) return drv_usage_error(usage_eval);
        bool is_file = false;
        const char *src = NULL;
        for (int i = 2; i < argc; i++) {
            if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0)
                return usage_eval();
            int consumed = 0;
            if (drv_is_include_flag(argc, argv, i, &consumed)) {
                i += consumed - 1;
                continue;
            }
            if (strcmp(argv[i], "--file") == 0 && i + 1 < argc) {
                is_file = true;
                src = argv[++i];
            } else if (argv[i][0] != '-') {
                src = argv[i];
            }
        }
        if (!src) return drv_usage_error(usage_eval);
        bool use_color = !no_color && stderr_is_tty();
        if (is_file) {
            char **inc = NULL;
            int    n_inc = drv_parse_include_flags(argc, argv, 2, &inc);
            if (n_inc < 0) {
                fprintf(stderr, "tur: -I requires a directory argument\n");
                return 2;
            }
            int rc = cmd_eval_inc(src, use_color, inc, n_inc, NULL, 0,
                                  /*debug=*/false);
            free(inc);
            return rc;
        }
        return cmd_eval_expr(src, use_color);
    }
    /* E5: tur doc <symbol> */
    if (strcmp(cmd, "doc") == 0) {
        if (argc == 3 && (strcmp(argv[2], "--help") == 0 || strcmp(argv[2], "-h") == 0))
            return usage_doc();
        if (argc != 3) return drv_usage_error(usage_doc);
        return cmd_doc_cli(argv[2]);
    }
    /* OD4: tur docs -- locate/open/serve the rendered guides and API pages. */
    if (strcmp(cmd, "docs") == 0)
        return cmd_docs(argc, argv);
    /* AI6: tur image-info <image> -- print header without resuming. */
    if (strcmp(cmd, "image-info") == 0) {
        if (argc != 3 || strcmp(argv[2], "--help") == 0 || strcmp(argv[2], "-h") == 0) {
            fprintf(stderr, "usage:\n  tur image-info <image>\n");
            return argc == 3 ? 0 : 1;
        }
        return cmd_image_info(argv[2]);
    }
    /* AI6: tur image-verify <image> [binary] -- validate header (+ stamp). */
    if (strcmp(cmd, "image-verify") == 0) {
        if (argc < 3 || argc > 4 || strcmp(argv[2], "--help") == 0 || strcmp(argv[2], "-h") == 0) {
            fprintf(stderr, "usage:\n  tur image-verify <image> [loader-binary]\n");
            return (argc >= 3 && strcmp(argv[2], "--help") != 0) ? 2 : 0;
        }
        return cmd_image_verify(argv[2], argc == 4 ? argv[3] : NULL);
    }
    /* E13: tur explain — first-class subcommand wrapping --explain */
    if (strcmp(cmd, "explain") == 0) {
        if (argc == 3 && (strcmp(argv[2], "--help") == 0 || strcmp(argv[2], "-h") == 0))
            return usage_explain();
        if (argc < 3) return drv_usage_error(usage_explain);
        return cmd_explain(argv[2]);
    }
    if (strcmp(cmd, "format") == 0) {
        bool check_only = false;
        bool diff_mode  = false;
        const char *fmt_input = NULL;
        bool        lang_set  = false;
        ReaderType  force_lang = READER_TURMERIC;
        for (int i = 2; i < argc; i++) {
            if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0)
                return usage_format();
            if (strcmp(argv[i], "--check") == 0) {
                check_only = true;
            } else if (strcmp(argv[i], "--diff") == 0) {
                diff_mode = true;
            } else if (strcmp(argv[i], "--lang") == 0) {
                if (i + 1 >= argc) return drv_usage_error(usage_format);
                const char *lang = argv[++i];
                if (!fmt_reader_from_lang_name(lang, &force_lang)) {
                    fprintf(stderr,
                            "tur format: unknown dialect '%s' (expected a "
                            "reader name or a base `tur dialects` lists)\n",
                            lang);
                    return 2;
                }
                lang_set = true;
            } else if (argv[i][0] != '-') {
                if (fmt_input) return drv_usage_error(usage_format);
                fmt_input = argv[i];
            } else {
                return drv_usage_error(usage_format);
            }
        }
        if (check_only && diff_mode) return drv_usage_error(usage_format);
        return cmd_format(fmt_input, check_only, diff_mode, lang_set,
                          force_lang);
    }
    if (strcmp(cmd, "fmt") == 0)
        return cmd_fmt(argc, argv);
    if (strcmp(cmd, "parse-check") == 0) {
        if (argc == 3 && (strcmp(argv[2], "--help") == 0 ||
                          strcmp(argv[2], "-h") == 0))
            return usage_parse_check();
        if (argc != 4) return drv_usage_error(usage_parse_check);
        return cmd_parse_check(argv[2], argv[3]);
    }
    if (strcmp(cmd, "demangle") == 0)
        return cmd_demangle(argc, argv);
    if (strcmp(cmd, "test") == 0) {
        if (argc == 3 && (strcmp(argv[2], "--help") == 0 || strcmp(argv[2], "-h") == 0))
            return usage_test();
        if (argc != 3) return drv_usage_error(usage_test);
        return cmd_test(argv[2]);
    }
    /* Phase PKG-1: Spice package manager commands */
    if (strcmp(cmd, "new") == 0)
        return cmd_pkg_new(argc, argv);
    if (strcmp(cmd, "init") == 0)
        return cmd_pkg_init(argc, argv);
    if (strcmp(cmd, "add") == 0)
        return cmd_pkg_add(argc, argv);
    if (strcmp(cmd, "add-cmake") == 0)
        return cmd_pkg_add_cmake(argc, argv);
    if (strcmp(cmd, "fetch") == 0)
        return cmd_pkg_fetch(argc, argv);
    if (strcmp(cmd, "audit") == 0)
        return cmd_pkg_audit(argc, argv);
    if (strcmp(cmd, "emit-cmake") == 0)
        return cmd_pkg_emit_cmake(argc, argv);
    /* GS-M2: global spice install commands */
    if (strcmp(cmd, "install") == 0)
        return cmd_pkg_install(argc, argv);
    if (strcmp(cmd, "uninstall") == 0)
        return cmd_pkg_uninstall(argc, argv);
    /* GS-M3 / GS-M4 */
    if (strcmp(cmd, "list") == 0)
        return cmd_pkg_list(argc, argv);
    if (strcmp(cmd, "upgrade") == 0)
        return cmd_pkg_upgrade(argc, argv);
    /* SX8a: run an SMT-LIB2 script through the refinement solver */
    if (strcmp(cmd, "smt") == 0)
        return cmd_smt(argc, argv);
    /* XF3: experimental-feature registry listing */
    if (strcmp(cmd, "experiments") == 0)
        return cmd_experiments(argc, argv);
    /* `#lang` base-dialect listing */
    if (strcmp(cmd, "dialects") == 0)
        return cmd_dialects(argc, argv);
    /* Shell completion scripts (zsh/bash) */
    if (strcmp(cmd, "completion") == 0)
        return cmd_completion(argc, argv);

    /* GS-M2: subcommand fallthrough — `tur foo bar` execs `tur-foo bar`
     * from $PATH when "foo" isn't a built-in. Built-ins always win.
     * If exec succeeds, this does not return. */
    return try_external_subcommand(argc, argv);
}

/* ---------------------------------------------------------------------------
 * The compiler runs on a stack sized for its own recursion.
 *
 * Every phase that walks the AST -- reader, elaborator, emitter -- recurses
 * once per level of expression nesting, and how deep that goes is a property
 * of the INPUT.  A macro that expands into a nested chain can want more levels
 * than a default 8 MiB thread stack holds, and on a Debug+ASan build each
 * frame is ~40x its usual size.
 *
 * Rationing it with a per-phase depth constant was tried and failed in both
 * directions: the constant stopped being a ceiling as soon as a frame grew (an
 * ASan stack-overflow abort where a diagnostic should have printed), and it
 * rejected legitimate macro-generated code that no hand-written source would
 * reach.  See docs/archive/emit-depth-guard-loses-race-with-asan-stack.md.
 *
 * The emitter is where that surfaced, but it is not the only offender: an
 * ~400-level nested expression aborts in elab_call -> elab_form, which has no
 * depth guard at all.  So the stack is sized once, underneath the whole
 * driver, rather than per phase.  TUR_STACK_MB tunes it; jit_engine.c has done
 * the same for a JIT'd program's entry stack (TUR_JIT_STACK_MB) all along.
 *
 * emit_program and friends ALSO trampoline (emit_module.c).  That is not
 * redundant: tur_run_on_big_stack is re-entrant-safe, so it is a no-op when
 * reached from here, and it keeps the guarantee for an embedder that links
 * libturi and calls the emitter without going through this main.
 * ------------------------------------------------------------------------ */

typedef struct { int argc; char **argv; } TurMainArgs;

static int tur_main_job(void *p) {
    TurMainArgs *a = (TurMainArgs *)p;
    return tur_main_inner(a->argc, a->argv);
}

/* r7rs-lang-plan T5: `#lang r7rs`'s call/cc copies the C stack under
 * `tur --interpret` (src/turi/interpreter_natives.c).  ASan's use-after-return
 * detection moves address-taken locals onto a heap "fake stack" that a copy of
 * the real stack cannot see, so a sanitized `tur` runs with it off -- the
 * same default the prelude sets for a sanitized compiled Scheme program.
 * ASAN_OPTIONS still overrides it. */
#if defined(__SANITIZE_ADDRESS__)
const char *__asan_default_options(void);
const char *__asan_default_options(void) { return "detect_stack_use_after_return=0"; }
#endif

#ifdef _WIN32
/* A crash in tur.exe is SILENT on Windows: an access violation ends the
 * process with status 0xC0000005 and nothing on stderr, where POSIX at least
 * reports the signal.  That is how `tur mcp` came to die mid-session on CI
 * three times with no evidence at all
 * (docs/archive/mcp-server-exits-mid-session-on-windows.md).
 *
 * Name the exception and where it happened, as an offset into tur.exe --
 * `addr2line -e tur.exe <ImageBase + offset>` resolves it against the same
 * build (ImageBase from `objdump -p`, 0x140000000 by default) -- then carry on
 * exactly as before: EXCEPTION_CONTINUE_SEARCH leaves the default handling --
 * and so the exit status -- untouched.  Only an exception nothing else
 * handled reaches this, so it prints only on a process that is already dying.
 * Kept to a stack buffer and _write: after a stack overflow there is very
 * little stack left to report with. */
static LONG WINAPI tur_win_report_crash(EXCEPTION_POINTERS *ep) {
    char msg[160];
    uintptr_t at   = (uintptr_t)ep->ExceptionRecord->ExceptionAddress;
    uintptr_t base = (uintptr_t)GetModuleHandleW(NULL);
    int n = snprintf(msg, sizeof msg,
                     "tur: fatal exception 0x%08lx at %p (tur.exe+0x%llx)\n",
                     (unsigned long)ep->ExceptionRecord->ExceptionCode,
                     (void *)at, (unsigned long long)(at - base));
    if (n > 0) (void)_write(2, msg, (unsigned)n);
    return EXCEPTION_CONTINUE_SEARCH;
}
#endif

int main(int argc, char **argv) {
#ifdef _WIN32
    SetUnhandledExceptionFilter(tur_win_report_crash);
#endif
    TurMainArgs a = { argc, argv };
    return tur_run_on_big_stack(tur_main_job, &a);
}
