/* cmd_build.c -- `tur build <file>`: one file to one executable (cmd_build,
 * which retries a split build whole, and cmd_build_once).
 *
 * Part of the compiler driver (src/driver/); driver.h says how the files
 * share declarations. */

#include "driver.h"
#include "commands.h"

#include "jit_prune.h"    /* JIT: drop the prelude a program never reaches */
#include "compiler/emit_split.h" /* r7rs-programs-compile-slowly */
#include "prof.h"         /* profiling-plan C0+C1: --timings */
#include "refine_discharge.h" /* RT3: per-compile refinement stats reset */
#include "globals.h"

static int cmd_build_once(const char *input, const char *out_path,
                          const char **include_dirs, int n_include_dirs,
                          const char *target,
                          const char **reader_macro_paths,
                          int n_reader_macro_paths,
                          bool allow_split, bool *retry_whole, bool *three_units);

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
    g_emit_for_wasm = target && strcmp(target, "wasm") == 0;
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
    g_emit_for_wasm = false;
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
            /* wasm-target-stack-is-emcc-default: the linear stack overflows
             * into whatever sits below it unless the build checks; the check
             * is a bounds compare on each `sp` move, so every build but
             * --release pays it.  The stack size itself is composed from
             * `:web :stack` (pkg_web_compose_link_flags). */
            cc_flags = g_cc_release
                ? "-O3 -std=c99 -Wall -fno-strict-aliasing -s WASM=1"
                : "-O2 -std=c99 -Wall -fno-strict-aliasing -s WASM=1 "
                  "-sSTACK_OVERFLOW_CHECK=1";
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
    drv_collect_build_aux_with_installed(input, &cmake_flags, &aux_includes, &aux_sources,
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

/* -------------------------------------------------------------------------
 * Entry points: the command table in cli.c runs these for `tur <command>`.
 * ---------------------------------------------------------------------- */

int cmd_build_main(int argc, char **argv) {
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
