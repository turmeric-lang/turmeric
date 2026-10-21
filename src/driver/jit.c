/* jit.c -- the driver's half of the MIR JIT: `tur jit`, the SDK include
 * probes, the REPL's in-process spice build and inline-C JIT, and
 * run_delegate_engine, which `tur run --engine` uses.
 *
 * Part of the compiler driver (src/driver/); driver.h says how the files
 * share declarations. */

#include "driver.h"
#include "commands.h"

#include "jit_prune.h"    /* JIT: drop the prelude a program never reaches */
#include "compiler/stack_guard.h" /* tur_run_on_big_stack */
#include "prof.h"         /* profiling-plan C0+C1: --timings */
#include "refine_discharge.h" /* RT3: per-compile refinement stats reset */
#include "globals.h"
#include "jit_engine.h"   /* J1: tur jit (TUR_HAVE_JIT builds only) */
#include "mono_specs.h"   /* VBM1: --dump-mono-specs registry dump */

/* All of this file is the engine's.  A build configured -DTUR_JIT=OFF (or on
 * a host the default leaves off) compiles only the cmd_jit stub at the end.
 * The split helpers the cc path calls in every build -- jit_try_split_preamble
 * and cmd_emit_rt_split -- live in preamble_split.c; when they sat inside the
 * JIT code, one of them landed under this guard and broke every JIT-less
 * build. */
#ifdef TUR_HAVE_JIT

#ifdef _WIN32
/* c2mir carries baked-in system-header paths for Linux and macOS
 * (/usr/include and friends) and nothing for Windows, so on MinGW every
 * `#include <winsock2.h>` in the emitted C fails with "error in opening
 * file" before parsing even starts.  Caller-supplied include_dirs are added
 * to c2mir's SYSTEM header search too, so the fix belongs here, not in MIR:
 * hand it the UCRT include dir of the same toolchain the cc fallback would
 * use, found by walking PATH for cc.exe and taking <bindir>/../include.
 * TUR_JIT_SYS_INCLUDE overrides the probe outright.  Returns false when
 * neither yields a directory -- the engine then fails to open the header and
 * falls back to cc, exactly as before this function existed. */
static bool jit_win_sys_include_dir(char *out, size_t cap) {
    const char *env = getenv("TUR_JIT_SYS_INCLUDE");
    struct stat st;
    if (env && *env) {
        snprintf(out, cap, "%s", env);
        return stat(out, &st) == 0;
    }
    const char *path = getenv("PATH");
    if (!path) return false;
    while (*path) {
        const char *sep = strchr(path, ';');
        size_t len = sep ? (size_t)(sep - path) : strlen(path);
        if (len > 0 && len < 3800) {
            char probe[4096];
            snprintf(probe, sizeof(probe), "%.*s/cc.exe", (int)len, path);
            if (stat(probe, &st) == 0) {
                snprintf(out, cap, "%.*s/../include", (int)len, path);
                return stat(out, &st) == 0;
            }
        }
        if (!sep) break;
        path = sep + 1;
    }
    return false;
}
#endif

/* The engine's `#include "hamt.h"` (et al.) include path: the Turmeric tree
 * (dev checkout, walking up from the executable) or the installed SDK.
 * On Windows also the toolchain's system include dir (see
 * jit_win_sys_include_dir).  Fills inc0/inc1/inc2 (caller-owned, >= 4096
 * each) and incs[0..2]; returns the count (0 when nothing was found). */
static int jit_sdk_include_dirs(char *inc0, size_t cap0,
                                char *inc1, size_t cap1,
                                char *inc2, size_t cap2,
                                const char *incs[3]) {
    int n = 0;
    char root[4096] = "";
    const char *env = getenv("TUR_SDK_ROOT");
    if (env && *env) {
        snprintf(root, sizeof(root), "%s/share/turmeric", env);
        struct stat st;
        char probe[4200];
        snprintf(probe, sizeof(probe), "%s/src/runtime/hamt.h", root);
        if (stat(probe, &st) != 0) snprintf(root, sizeof(root), "%s", env);
    } else {
        char exe[4096] = "";
        if (drv_get_exe_path(exe, sizeof(exe)) == 0) {
            char dir[4096];
            drv_dir_of_path(exe, dir, sizeof(dir));
            for (int d = 0; d < 8; d++) {
                char probe[4200];
                struct stat st;
                snprintf(probe, sizeof(probe), "%s/src/runtime/hamt.h", dir);
                if (stat(probe, &st) == 0) { snprintf(root, sizeof(root), "%s", dir); break; }
                snprintf(probe, sizeof(probe),
                         "%s/share/turmeric/src/runtime/hamt.h", dir);
                if (stat(probe, &st) == 0) {
                    snprintf(root, sizeof(root), "%s/share/turmeric", dir);
                    break;
                }
                char *sl = strrchr(dir, '/');
#ifdef _WIN32
                /* GetModuleFileName hands back backslashes; a '/'-only split
                 * ended the walk at depth 0 and the JIT lost hamt.h. */
                {
                    char *bs = strrchr(dir, '\\');
                    if (bs && (!sl || bs > sl)) sl = bs;
                }
#endif
                if (!sl || sl == dir) break;
                *sl = '\0';
            }
        }
    }
    if (root[0]) {
        snprintf(inc0, cap0, "%s/src", root);
        snprintf(inc1, cap1, "%s/src/runtime", root);
        incs[n++] = inc0;
        incs[n++] = inc1;
    }
#ifdef _WIN32
    if (jit_win_sys_include_dir(inc2, cap2)) {
        incs[n++] = inc2;
    }
#else
    (void)inc2; (void)cap2;
#endif
    return n;
}

/* J1 (docs/archive/jit-engine-plan.md section 3.2): `tur jit <file>` --
 * compile and execute in process via c2mir + MIR-gen, no cc subprocess, no
 * disk artifacts.  Front half is cmd_build's exactly: run_core_passes via
 * compile_to_c, then the same in-memory post-passes.  The back half hands the
 * buffer to tur_jit_execute (src/jit_engine.c, built under -DTUR_JIT=ON).
 *
 * Fallback (plan step 6): any engine-level failure -- c2mir rejecting a GNU
 * construct in user inline-C is the expected case -- prints a TUR-W and
 * delegates to cmd_run, which recompiles through cc.  Never a hard stop.
 *
 * Usage: tur jit [-I <dir>...] <file> [-- <args>...]  */
int cmd_jit(int argc, char **argv) {
    /* engine-selection-plan P0: locate the input FIRST, before anything that
     * depends on the enclosing manifest.  The original reason was the
     * `:experiments [jit]` gate (which graduated away 2026-08-17); the
     * surviving one is `:reader-macros`, which was ignored by the same
     * ordering bug and is restored here. */
    const char *input = NULL;
    /* B4 (post-jit-benchmark-resurrection-plan): --timing-json <path> writes
     * {"compile_ms","run_ms","engine"} after the run, so a benchmark harness
     * can separate compile from run (chart A) and detect the cc fallback
     * instead of averaging it in.  The JIT knows both numbers exactly;
     * recovering them from outside is guesswork. */
    const char *timing_json = NULL;
    int passthrough_start = -1;
    int scan_end = argc;
    for (int i = 2; i < argc; i++)
        if (strcmp(argv[i], "--") == 0) { scan_end = i; break; }
    char **user_inc = NULL;
    int n_user_inc = drv_parse_include_flags(scan_end, argv, 2, &user_inc);
    if (n_user_inc < 0) { free(user_inc); return 2; }
    for (int i = 2; i < scan_end; i++) {
        int c;
        if (drv_is_include_flag(argc, argv, i, &c)) { i += c - 1; continue; }
        if (strcmp(argv[i], "--timing-json") == 0 && i + 1 < scan_end) {
            timing_json = argv[++i];
            continue;
        }
        if (strncmp(argv[i], "--timing-json=", 14) == 0) {
            timing_json = argv[i] + 14;
            continue;
        }
        if (argv[i][0] != '-' && !input) input = argv[i];
    }
    for (int i = 2; i < argc; i++)
        if (strcmp(argv[i], "--") == 0) { passthrough_start = i + 1; break; }
    if (!input) {
        fprintf(stderr, "usage: tur jit [-I <dir>...] <file> [-- <args>...]\n");
        free(user_inc);
        return 2;
    }
    int jit_rm_n = 0;
    char **jit_rm = discover_manifest_reader_macros(input, &jit_rm_n);
    /* The `jit` experiment GRADUATED 2026-08-17 -- `tur jit` no longer needs
     * `--enable=jit`.  The BUILD-TIME gate is the one that remains: a build
     * with no engine compiles the stub at the end of this file instead. */

    refine_discharge_reset();
    g_emit_for_link = true;
    Buf csrc;
    buf_init(&csrc);
    struct timespec _tj0, _tj1;
    clock_gettime(CLOCK_MONOTONIC, &_tj0);
    /* tur-jit-skips-spice-include-discovery: the enclosing spice's `src/` and
     * its deps' go on the search path, as for every other per-file command
     * (`tur run --engine jit` resolved what `tur jit` could not).
     * --no-auto-spice is honoured inside. */
    char **jit_owned = NULL;
    int    n_jit_owned = 0;
    Ls2ResolverCtx jit_ls2 = {0};
    if (strcmp(input, "-") != 0)
        auto_append_spice_includes(input, &user_inc, &n_user_inc,
                                   &jit_owned, &n_jit_owned, &jit_ls2);
    ls2_resolver_ctx_set(&jit_ls2);
    int rc = compile_to_c(input, &csrc, (const char **)user_inc, n_user_inc,
                          (const char **)jit_rm, jit_rm_n);
    ls2_resolver_ctx_set(NULL);
    ls2_resolver_ctx_dispose(&jit_ls2);
    for (int i = 0; i < n_jit_owned; i++) free(jit_owned[i]);
    free(jit_owned);
    clock_gettime(CLOCK_MONOTONIC, &_tj1);
    /* Turmeric front-end time (read -> elaborate -> emit C); the engine's
     * own c2mir+link time is added below so compile_ms is invocation-to-
     * entry, the quantity chart A subtracts. */
    double front_ms = ((double)_tj1.tv_sec - (double)_tj0.tv_sec) * 1000.0 +
                      ((double)_tj1.tv_nsec - (double)_tj0.tv_nsec) / 1.0e6;
    free_reader_macro_paths(jit_rm, jit_rm_n);
    if (rc != 0) { buf_free(&csrc); free(user_inc); return rc; }
    hoist_tur_include_directives(&csrc);
    Buf autolink;
    buf_init(&autolink);
    if (!scan_autolink_markers(&csrc, &autolink)) {
        buf_free(&csrc); buf_free(&autolink); free(user_inc); return 2;
    }

    /* S2 (findings 19.4 item 3): swap the fixed preamble for the committed
     * declarations region when this compiler still matches the committed
     * artifacts.  The runtime then stays host-resident (tur_rt_split.c is
     * linked into this executable and exported for the engine's dlsym
     * resolver), and c2mir compiles ~60% less fixed text per program.
     *
     * The guard: re-emit the all-gates preamble under CURRENT process state
     * and hash it against the recorded artifact hash.  Emitter drift and knob
     * drift (--backtrack-depth, experiments) change that text, fail the
     * compare, and keep the full preamble -- never wrong, just slower.
     * Per-host facts (the runtime archive, trail autoload, the g_needs_*
     * gates) are forced to canonical values inside emit_rt_split_source, so
     * they cannot.  Probe cost is one in-memory emission,
     * microseconds against the ~90ms c2mir spend it can save.  Runs AFTER
     * compile_to_c: emit_rt_split_source resets per-TU codegen registries,
     * which must not disturb the program's own emission.
     *
     * The TU layout post-hoist is [hoisted user includes/code][preamble ...
     * marker][program half]; the splice replaces exactly the preamble
     * span, keeping any hoisted prefix.  TUR_JIT_NO_SPLIT=1 opts out. */
    Buf split_src;
    buf_init(&split_src);
    /* --timings: the split swap and the prune below rework the emitted C --
     * "emit"; c2mir and the link are "jit" (tur_jit_execute). */
    prof_enter(PROF_EMIT);
    bool split_used = jit_try_split_preamble(&csrc, &split_src);
    /* jit-suite-pays-for-the-whole-prelude: c2mir compiles every definition
     * it is handed, and every program carries the whole auto-loaded prelude
     * (and, split, the prelude's heavy system headers).  Drop what the
     * program never reaches (src/compiler/jit_prune.h).  A TU the split
     * declined is pruned as a COPY, so the full TU stays for the W0071 retry
     * below exactly as a split one does.  Not for the REPL image: it must
     * keep every exported function for later calls. */
    bool reduced_used = split_used;
    {
        JitPruneStats ps;
        memset(&ps, 0, sizeof ps);
        bool pruned;
        if (split_used) {
            pruned = jit_prune_split_source(&split_src, &ps);
        } else {
            buf_write(&split_src, csrc.data, csrc.len);
            pruned = jit_prune_full_source(&split_src, &ps);
            if (pruned) reduced_used = true;
            else { buf_free(&split_src); buf_init(&split_src); }
        }
        if (pruned) {
            const char *tv = getenv("TUR_JIT_TIMING");
            if (tv && *tv && strcmp(tv, "0") != 0)
                fprintf(stderr,
                        "TUR_JIT_TIMING\tprune\tbytes\t%zu -> %zu (%u of %u "
                        "definitions, %u includes dropped)\n",
                        ps.bytes_before, ps.bytes_after, ps.nodes_dropped,
                        ps.nodes, ps.includes_dropped);
        }
        /* TUR_JIT_DUMP_C: the text c2mir is handed first, so a <tur-jit>:LINE
         * diagnostic reads against it (the no-reduction path dumps below). */
        const char *dump = getenv("TUR_JIT_DUMP_C");
        if (reduced_used && dump && *dump) buf_to_path(&split_src, dump);
    }
    prof_exit(PROF_EMIT);

    /* Program argv: argv[0] = the source path (matches what a compiled binary
     * would see as its own name closely enough for *args*), then everything
     * after `--`. */
    int prog_argc = 1 + (passthrough_start > 0 ? argc - passthrough_start : 0);
    char **prog_argv = (char **)malloc(((size_t)prog_argc + 1) * sizeof(char *));
    if (!prog_argv) {
        buf_free(&csrc); buf_free(&split_src); buf_free(&autolink);
        free(user_inc); return 2;
    }
    prog_argv[0] = (char *)input;
    for (int i = 0; i < prog_argc - 1; i++)
        prog_argv[i + 1] = argv[passthrough_start + i];
    prog_argv[prog_argc] = NULL;

    /* Include path for `#include "hamt.h"` et al. */
    static char jit_inc0[4096], jit_inc1[4096], jit_inc2[4096];
    const char *jit_incs[3];
    int n_jit_incs = jit_sdk_include_dirs(jit_inc0, sizeof(jit_inc0),
                                          jit_inc1, sizeof(jit_inc1),
                                          jit_inc2, sizeof(jit_inc2),
                                          jit_incs);
    if (getenv("TUR_JIT_SPLIT_DEBUG")) {
        fprintf(stderr, "jit-debug: n_incs=%d", n_jit_incs);
        for (int di = 0; di < n_jit_incs; di++)
            fprintf(stderr, " [%s]", jit_incs[di]);
        fprintf(stderr, "\n");
    }

    int prog_rc = 0;
    int jrc;
    /* jit-cc-fallback-reentry-inherits-monomorph-state: decline the engine
     * deliberately, so the step-6 cc fallback can be exercised on demand.
     *
     * That path is otherwise unreachable on the fixtures that used to break it
     * -- with __auto_type gone from the emitter the engine no longer declines
     * them -- which is exactly the condition under which a latent bug gets
     * rediscovered the hard way.
     *
     * Declines BEFORE tur_jit_execute rather than after, so the program is not
     * run twice: the real shape is a front end that has already elaborated
     * (and populated the monomorph registry) handing over to a cc compile that
     * never executed anything. Forcing it post-execution would measure a
     * different thing and double the program's output.
     *
     * Test-only; not in --help. */
    const char *force_fb = getenv("TUR_JIT_FORCE_FALLBACK");
    if (force_fb && *force_fb && strcmp(force_fb, "0") != 0) {
        jrc = TUR_JIT_ERR_COMPILE;
    } else if (reduced_used) {
        /* Reduced TU (split and/or pruned) first; if it fails to COMPILE or
         * LINK, retry the full TU in the engine before conceding to cc -- the
         * hash guard covers emitter drift but not, e.g., an export the host
         * build dropped, nor a reference the pruner missed, and the full TU is
         * self-contained against both.  tests/run-jit.sh fails a fixture that
         * passes only on this retry.  A RUN failure is the program's own (a
         * panic aborts identically either way), so it is not retried. */
        jrc = tur_jit_execute(split_src.data, split_src.len,
                              autolink.len ? autolink.data : NULL,
                              jit_incs, n_jit_incs,
                              prog_argc, prog_argv, &prog_rc);
        if (jrc == TUR_JIT_ERR_COMPILE || jrc == TUR_JIT_ERR_LINK) {
            fprintf(stderr,
                    "tur: warning: TUR-W0071: %s failed to %s; retrying "
                    "with the full %s\n",
                    split_used ? "split-runtime path" : "pruned program",
                    jrc == TUR_JIT_ERR_COMPILE ? "compile" : "link",
                    split_used ? "preamble" : "program");
            jrc = tur_jit_execute(csrc.data, csrc.len,
                                  autolink.len ? autolink.data : NULL,
                                  jit_incs, n_jit_incs,
                                  prog_argc, prog_argv, &prog_rc);
        }
    } else {
        /* TUR_JIT_DUMP_C also covers the no-split path: the text handed to
         * c2mir is the full TU here, and a diagnostic's <tur-jit>:LINE is
         * against it, not against `tur emit-c` output (g_emit_for_link and
         * the include hoist both move lines). */
        const char *dump_full = getenv("TUR_JIT_DUMP_C");
        if (dump_full && *dump_full) {
            FILE *df = fopen(dump_full, "wb");
            if (df) { fwrite(csrc.data, 1, csrc.len, df); fclose(df); }
        }
        jrc = tur_jit_execute(csrc.data, csrc.len,
                              autolink.len ? autolink.data : NULL,
                              jit_incs, n_jit_incs,
                              prog_argc, prog_argv, &prog_rc);
    }
    free(prog_argv);
    buf_free(&csrc);
    buf_free(&split_src);
    buf_free(&autolink);
    free(user_inc);
    if (jrc == TUR_JIT_OK) {
        if (timing_json) {
            double eng_compile_ms = 0.0, run_ms = 0.0;
            tur_jit_last_timings(&eng_compile_ms, &run_ms);
            FILE *tf = fopen(timing_json, "w");
            if (tf) {
                fprintf(tf,
                        "{\"compile_ms\": %.3f, \"run_ms\": %.3f, "
                        "\"engine\": \"jit\"}\n",
                        front_ms + eng_compile_ms, run_ms);
                fclose(tf);
            }
        }
        return prog_rc;
    }

    /* Plan step 6: clean per-program fallback to the cc path.  cmd_run parses
     * the same argv shape (it never looks at argv[1]), so delegate whole. */
    fprintf(stderr,
            "tur: warning: TUR-W0070: jit engine could not %s this program "
            "(see diagnostics above); falling back to the cc path\n",
            jrc == TUR_JIT_ERR_COMPILE ? "compile"
            : jrc == TUR_JIT_ERR_LINK  ? "link"
                                       : "run");
    if (timing_json) {
        /* B4 / plan section 3.3: a benchmark that silently falls back is
         * measuring `tur build` with extra steps -- make it detectable. */
        FILE *tf = fopen(timing_json, "w");
        if (tf) {
            fprintf(tf, "{\"engine\": \"cc-fallback\"}\n");
            fclose(tf);
        }
    }
    /* jit-cc-fallback-reentry-inherits-monomorph-state: the engine attempt
     * already ran the front end, and the van-Laarhoven monomorph registry
     * (mono_specs.c) is file-scope state that survives into the re-entry.
     * cmd_run then elaborates the program a second time against a registry
     * that already believes those specs were requested, so the emitted C
     * CALLS `over_px__lens_<hash>` while nothing emits its definition:
     *
     *   error: call to undeclared function 'over_px__lens_89e16e7f8669ca4e'
     *
     * `tur run` on the same file is clean, which is what identified the
     * carried-over state rather than the program. mono_specs_reset() has
     * existed since the registry was added and was never called; this is the
     * call site it was written for. */
    mono_specs_reset();
    return cmd_run(argc, argv);
}

/* ------------------------------------------------------------------ */
/* J2 (jit-engine-plan 3.3): the REPL's in-process spice build.        */
/* ------------------------------------------------------------------ */
/* Replaces the `tur build --shared` subprocess + dlopen with the MIR
 * engine, via the TurSpiceJitHook the loader consults.  The whole spice
 * is compiled as ONE in-memory TU through the well-tested single-file
 * path: a synthetic root module imports every source module, so imports
 * dedupe through the ordinary module machinery (a load-based root
 * duplicated any module that was also imported intra-spice).
 *
 * Module names come from each file's `(defmodule <name>` (filename stem
 * when absent), and files whose module name does not match their
 * filename are made importable through a SHADOW DIR of symlinks under
 * .tur-repl-cache/jit-mods/ (hard links or copies on Windows, see
 * repl_jit_shadow_entry) -- module resolution is filename-based, and
 * on the --shared path a mismatched file was reachable only because each
 * file was compiled separately.
 *
 * v1 limit (recorded, not silent): transitive :spices deps are not
 * auto-appended (single-spice projects only -- the subprocess path
 * remains the default and handles them). */

/* Peek a source file's defmodule name into out (cap bytes).  Textual scan
 * of the first non-comment occurrence -- both `(defmodule x` and sweet-exp
 * `defmodule x` spellings.  Returns false when the file has none. */
static bool repl_jit_peek_module_name(const char *path, char *out, size_t cap) {
    FILE *f = fopen(path, "r");
    if (!f) return false;
    char line[4096];
    bool found = false;
    while (!found && fgets(line, sizeof(line), f)) {
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == ';') continue;
        if (*p == '(') { p++; while (*p == ' ') p++; }
        if (strncmp(p, "defmodule", 9) != 0) continue;
        p += 9;
        if (*p != ' ' && *p != '\t') continue;
        while (*p == ' ' || *p == '\t') p++;
        size_t o = 0;
        while (*p && *p != ' ' && *p != '\t' && *p != ')' && *p != '\n'
               && *p != '\r' && o + 1 < cap)
            out[o++] = *p++;
        out[o] = '\0';
        found = o > 0;
    }
    fclose(f);
    return found;
}

struct repl_jit_mod {
    char *src_path;   /* absolute source file path */
    char *mod_name;   /* module name (may contain '/') */
};

static int repl_jit_scan_dir(const char *dir, struct repl_jit_mod **mods,
                             uint32_t *n, uint32_t *cap) {
    DIR *d = opendir(dir);
    if (!d) return 0;   /* unreadable subdir: skip, the build would too */
    struct dirent *e;
    int rc = 0;
    while (rc == 0 && (e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        char path[4600];
        snprintf(path, sizeof(path), "%s/%s", dir, e->d_name);
        struct stat st;
        if (stat(path, &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) {
            rc = repl_jit_scan_dir(path, mods, n, cap);
            continue;
        }
        size_t nl = strlen(e->d_name);
        if (nl < 5 || strcmp(e->d_name + nl - 4, ".tur") != 0) continue;
        char modname[512];
        if (!repl_jit_peek_module_name(path, modname, sizeof(modname))) {
            snprintf(modname, sizeof(modname), "%.*s",
                     (int)(nl - 4), e->d_name);
        }
        if (*n == *cap) {
            uint32_t nc = *cap ? *cap * 2 : 8;
            struct repl_jit_mod *na =
                realloc(*mods, nc * sizeof(**mods));
            if (!na) { rc = -1; break; }
            *mods = na;
            *cap = nc;
        }
        (*mods)[*n].src_path = strdup(path);
        (*mods)[*n].mod_name = strdup(modname);
        (*n)++;
    }
    closedir(d);
    return rc;
}

static void repl_jit_mods_free(struct repl_jit_mod *mods, uint32_t n) {
    for (uint32_t i = 0; i < n; i++) {
        free(mods[i].src_path);
        free(mods[i].mod_name);
    }
    free(mods);
}

/* mkdir -p for the directory part of shadow/<modname>.tur. */
static void repl_jit_mkdirs_for(const char *shadow, const char *modname) {
    char path[4800];
    snprintf(path, sizeof(path), "%s/%s", shadow, modname);
    for (char *p = path + strlen(shadow) + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            mkdir(path, 0755);
            *p = '/';
        }
    }
}

/* One shadow entry: `link` (shadow/<modname>.tur) names the source file.
 *
 * POSIX: a symlink.  Windows: symlink() is a deliberate ENOSYS stub
 * (platform_fs.h) -- a real one needs Developer Mode or elevation, and the
 * header leaves the fallback to the caller, which knows what it can live
 * with.  This caller can live with any file holding the same bytes: the
 * entry is unlinked and recreated on every build, (reload) included, and
 * nothing reads it between builds, so it can never be staler than the
 * compile that reads it.  So: a HARD link first -- no privilege needed on
 * NTFS, and it is the same file, so a diagnostic naming the shadow path
 * still names what the user edits -- then a COPY for what a hard link
 * cannot span (another volume, FAT).  Returns 0, or -1 after printing why. */
static int repl_jit_shadow_entry(const char *src, const char *link) {
#ifdef _WIN32
    if (CreateHardLinkA(link, src, NULL)) return 0;
    DWORD link_err = GetLastError();
    if (CopyFileA(src, link, FALSE)) return 0;
    fprintf(stderr,
            "tur repl: jit: cannot shadow %s as %s (hard link: Windows "
            "error %lu, copy: Windows error %lu)\n",
            src, link, (unsigned long)link_err,
            (unsigned long)GetLastError());
    return -1;
#else
    if (symlink(src, link) == 0) return 0;
    fprintf(stderr, "tur repl: jit: symlink %s: %s\n", link, strerror(errno));
    return -1;
#endif
}

/* The image compile the REPL's two in-process builds share (the spice
 * build below, and C1's inline-C defns): swap in the S2 split preamble when
 * its hash matches, compile, and on a failed split attempt retry with the
 * full preamble -- the same ladder as cmd_jit, because the full TU is
 * self-contained against a hole the hash guard cannot see.
 *
 * `prune` drops what nothing live names, as cmd_jit does
 * (src/compiler/jit_prune.h).  Only for a TU whose every entry point is a
 * non-static definition: a static export the manifest names but nothing in the
 * TU calls would be dropped.  The spice build passes false for that reason;
 * C1's one function is reached through its non-static __ffi shim. */
static int repl_jit_compile_image(Buf *csrc, const char *autolink, bool prune,
                                  TurJitImage **out) {
    Buf split_src;
    buf_init(&split_src);
    bool split_used = jit_try_split_preamble(csrc, &split_src);
    bool reduced_used = split_used;
    if (prune) {
        JitPruneStats ps;
        memset(&ps, 0, sizeof ps);
        if (split_used) {
            (void)jit_prune_split_source(&split_src, &ps);
        } else {
            buf_write(&split_src, csrc->data, csrc->len);
            if (jit_prune_full_source(&split_src, &ps)) {
                reduced_used = true;
            } else {
                buf_free(&split_src);
                buf_init(&split_src);
            }
        }
    }

    static char jinc0[4096], jinc1[4096], jinc2[4096];
    const char *jincs[3];
    int n_jincs = jit_sdk_include_dirs(jinc0, sizeof(jinc0),
                                       jinc1, sizeof(jinc1),
                                       jinc2, sizeof(jinc2), jincs);

    const Buf *use = reduced_used ? &split_src : csrc;
    int jrc = tur_jit_compile_image(use->data, use->len, autolink,
                                    jincs, n_jincs, out);
    if (jrc != TUR_JIT_OK && reduced_used) {
        fprintf(stderr,
                "tur: warning: TUR-W0071: split-runtime path failed; "
                "retrying with the full preamble\n");
        jrc = tur_jit_compile_image(csrc->data, csrc->len, autolink,
                                    jincs, n_jincs, out);
    }
    buf_free(&split_src);
    return jrc;
}

static int repl_jit_build(const char *build_dir, void **out_image,
                          char **out_manifest) {
    *out_image = NULL;
    *out_manifest = NULL;

    /* Spice root: the dir holding build.tur -- build_dir's parent when the
     * conventional src/ layout is in play, else build_dir itself. */
    char rootd[4300];
    snprintf(rootd, sizeof(rootd), "%s", build_dir);
    size_t rl = strlen(rootd);
    if (rl > 4 && strcmp(rootd + rl - 4, "/src") == 0) rootd[rl - 4] = '\0';

    struct repl_jit_mod *mods = NULL;
    uint32_t n_mods = 0, cap_mods = 0;
    if (repl_jit_scan_dir(build_dir, &mods, &n_mods, &cap_mods) != 0
        || n_mods == 0) {
        fprintf(stderr, "tur repl: jit: no .tur sources under %s\n",
                build_dir);
        repl_jit_mods_free(mods, n_mods);
        return -1;
    }

    /* Shadow dir: module-name -> file symlinks + the synthetic root. */
    char shadow[4400];
    snprintf(shadow, sizeof(shadow), "%s/.tur-repl-cache", rootd);
    mkdir(shadow, 0755);
    snprintf(shadow, sizeof(shadow), "%s/.tur-repl-cache/jit-mods", rootd);
    mkdir(shadow, 0755);

    Buf root_src;
    buf_init(&root_src);
    buf_puts(&root_src, "(defmodule __jit-root\n");
    int rc = 0;
    for (uint32_t i = 0; i < n_mods && rc == 0; i++) {
        repl_jit_mkdirs_for(shadow, mods[i].mod_name);
        char link[4900];
        snprintf(link, sizeof(link), "%s/%s.tur", shadow, mods[i].mod_name);
        unlink(link);
        if (repl_jit_shadow_entry(mods[i].src_path, link) != 0) {
            rc = -1;
            break;
        }
        buf_printf(&root_src, "  (import %s)\n", mods[i].mod_name);
    }
    buf_puts(&root_src, ")\n");
    repl_jit_mods_free(mods, n_mods);

    char root_file[4600];
    snprintf(root_file, sizeof(root_file), "%s/__jit_root.tur", shadow);
    if (rc == 0) {
        FILE *rf = fopen(root_file, "w");
        if (!rf || fwrite(root_src.data, 1, root_src.len, rf) != root_src.len) {
            fprintf(stderr, "tur repl: jit: cannot write %s\n", root_file);
            rc = -1;
        }
        if (rf) fclose(rf);
    }
    buf_free(&root_src);
    if (rc != 0) return -1;

    /* Compile the whole spice as one TU, capturing the exports manifest
     * from the same program.  Same emission posture as cmd_jit -- which
     * means COMPILED-mode elaboration: the REPL process runs with
     * g_interpret_mode=true (turi_env_new sets it), and inheriting that
     * here would select `#?(:turi ...)` branches into native code and
     * demote unknown names from hard errors to W0040 runtime-dispatch
     * warnings.  The subprocess build never saw the flag; clear it for
     * exactly the compile. */
    refine_discharge_reset();
    bool saved_efl = g_emit_for_link;
    bool saved_interp = g_interpret_mode;
    g_emit_for_link = true;
    g_interpret_mode = false;
    Buf csrc, manifest;
    buf_init(&csrc);
    buf_init(&manifest);
    const char *incs[1] = { shadow };
    g_manifest_sink = &manifest;
    g_emit_ffi_export_shims = true;   /* high-arity exports need __ffi shims */
    rc = compile_to_c(root_file, &csrc, incs, 1, NULL, 0);
    g_emit_ffi_export_shims = false;
    g_manifest_sink = NULL;
    g_emit_for_link = saved_efl;
    g_interpret_mode = saved_interp;
    if (rc != 0) {
        /* Same actionable wording as the subprocess path (run_build): the
         * user story -- fix the source, (reload) -- is identical. */
        fprintf(stderr,
                "tur repl: spice rebuild failed; fix the error above, then "
                "type (reload) at the prompt to retry.\n");
        buf_free(&csrc);
        buf_free(&manifest);
        return -1;
    }
    hoist_tur_include_directives(&csrc);
    Buf autolink;
    buf_init(&autolink);
    if (!scan_autolink_markers(&csrc, &autolink)) {
        buf_free(&csrc); buf_free(&autolink); buf_free(&manifest); return -1;
    }

    /* ffi-spices-integration-plan S1: the subprocess build injects the
     * spice's cmake-dep and :link-libs flags into cc's link line; the
     * in-process image's equivalent is the autolink string, whose -L/-l
     * entries jit_load_autolink dlopens RTLD_GLOBAL before MIR_link
     * resolves.  Without this, a spice that declares its C dependency the
     * recommended way (:cmake-deps / :link-libs, no __tur_autolink__
     * marker anywhere) compiled fine in-process and then failed to resolve
     * the library's symbols.  Vendored :c-sources cannot be MIR-linked
     * in-process at all -- fail the hook so the loader falls back to the
     * subprocess path, which compiles them into the .so. */
    {
        Buf cmk, auxi, auxs;
        buf_init(&cmk);
        buf_init(&auxi);
        buf_init(&auxs);
        char probe[4400];
        snprintf(probe, sizeof(probe), "%s/build.tur", rootd);
        drv_collect_build_aux(probe, &cmk, &auxi, &auxs, NULL);
        bool have_aux_sources = auxs.len > 0;
        if (cmk.len > 0) {
            if (autolink.len > 0) {
                /* scan_autolink_markers NUL-terminated it; reopen. */
                autolink.len--;
            }
            buf_write(&autolink, cmk.data, cmk.len);
            buf_putc(&autolink, '\0');
        }
        buf_free(&cmk);
        buf_free(&auxi);
        buf_free(&auxs);
        if (have_aux_sources) {
            fprintf(stderr,
                    "tur repl: jit: this spice vendors C sources "
                    "(:c-sources), which the in-process engine cannot "
                    "link; using the subprocess build instead.\n");
            buf_free(&csrc);
            buf_free(&autolink);
            buf_free(&manifest);
            return -1;
        }
    }

    /* S2: same hash-gated preamble swap as cmd_jit -- a REPL reload is
     * exactly the loop the split exists for. */
    TurJitImage *img = NULL;
    int jrc = repl_jit_compile_image(&csrc,
                                     autolink.len ? autolink.data : NULL,
                                     /*prune=*/false, &img);
    buf_free(&csrc);
    buf_free(&autolink);
    if (jrc != TUR_JIT_OK) {
        fprintf(stderr,
                "tur repl: jit: engine could not %s the spice; fix the "
                "error above and type (reload) to retry.\n",
                jrc == TUR_JIT_ERR_COMPILE ? "compile" : "link");
        buf_free(&manifest);
        return -1;
    }

    buf_putc(&manifest, '\0');
    *out_manifest = strdup(manifest.data);
    buf_free(&manifest);
    *out_image = img;
    return 0;
}

static void *repl_jit_hook_sym(void *image, const char *mangled) {
    return tur_jit_image_sym((TurJitImage *)image, mangled);
}
static void repl_jit_hook_free(void *image) {
    tur_jit_image_free((TurJitImage *)image);
}
const TurSpiceJitHook g_repl_jit_hook = {
    repl_jit_build, repl_jit_hook_sym, repl_jit_hook_free,
};

/* aot-compiled-repl-plan C1: compile ONE inline-C defn the interpreter cannot
 * run (turi/inline_c_jit.c builds the program text and does the calling).
 *
 * Unlike repl_jit_build this runs in the MIDDLE of an evaluation -- inside the
 * call that needed the function -- so it puts back everything compile_to_c
 * resets that the interpreter's turn still uses: the diagnostic file registry
 * (the turn's own source is id 0, and its loaded files keep their ids), the
 * had-error flag, and the emission flags repl_jit_build also saves.  The
 * scratch file is the program compile_to_c reads; it is named after the
 * function so a diagnostic says which defn it is about, and removed once
 * read.  The image is never freed: the interpreter calls into it for the rest
 * of the process. */
static int repl_inline_c_jit_build_here(const char *name, const char *src,
                                        size_t len, void **out_image,
                                        char **out_manifest) {
    *out_image = NULL;
    *out_manifest = NULL;

    /* TEMP/TMP are what a Windows host sets; /tmp need not exist there. */
    const char *tmp = getenv("TMPDIR");
    if (!tmp || !*tmp) tmp = getenv("TEMP");
    if (!tmp || !*tmp) tmp = getenv("TMP");
    if (!tmp || !*tmp) tmp = "/tmp";
    char stem[64];
    size_t o = 0;
    for (const char *p = name; *p && o + 1 < sizeof stem; p++)
        stem[o++] = (isalnum((unsigned char)*p) || *p == '-' || *p == '_')
                    ? *p : '_';
    stem[o] = '\0';
    char path[4300];
    snprintf(path, sizeof path, "%s/tur-repl-jit-%s-XXXXXX.tur", tmp, stem);
    int fd = mkstemps(path, 4);
    if (fd < 0) {
        fprintf(stderr, "tur: repl-jit-inline-c: cannot create %s: %s\n",
                path, strerror(errno));
        return -1;
    }
    FILE *f = fdopen(fd, "w");
    bool wrote = f && fwrite(src, 1, len, f) == len;
    if (f) fclose(f); else close(fd);
    if (!wrote) {
        fprintf(stderr, "tur: repl-jit-inline-c: cannot write %s\n", path);
        unlink(path);
        return -1;
    }

    size_t cap = diag_files_capacity();
    const SourceFile **saved_files =
        (const SourceFile **)calloc(cap ? cap : 1, sizeof *saved_files);
    if (!saved_files) { unlink(path); return -1; }
    size_t n_saved = diag_files_save(saved_files, cap);
    bool had_error = diag_had_error();

    bool saved_efl = g_emit_for_link;
    bool saved_interp = g_interpret_mode;
    bool saved_shims = g_emit_ffi_export_shims;
    Buf *saved_sink = g_manifest_sink;
    Buf csrc, manifest;
    buf_init(&csrc);
    buf_init(&manifest);
    /* COMPILED-mode elaboration, as in repl_jit_build: inheriting the
     * interpreter's g_interpret_mode would select `#?(:turi ...)` branches
     * into native code.  The __ffi shim is what the interpreter calls. */
    g_emit_for_link = true;
    g_interpret_mode = false;
    g_emit_ffi_export_shims = true;
    g_manifest_sink = &manifest;
    int rc = compile_to_c(path, &csrc, NULL, 0, NULL, 0);
    g_manifest_sink = saved_sink;
    g_emit_ffi_export_shims = saved_shims;
    g_interpret_mode = saved_interp;
    g_emit_for_link = saved_efl;
    unlink(path);

    TurJitImage *img = NULL;
    if (rc == 0) {
        hoist_tur_include_directives(&csrc);
        /* The image runs __tur_static_init by name, but it is static and, in
         * a TU with no main, nothing calls it -- so the prune below would drop
         * it.  One non-static caller keeps it (and what it initializes). */
        buf_puts(&csrc, "\nvoid __tur_repl_jit_keep_init(void) "
                        "{ __tur_static_init(); }\n");
        Buf autolink;
        buf_init(&autolink);
        /* c2mir's system-header warnings are noise at a prompt; its errors
         * (why a body was refused) still print. */
        bool was_quiet = tur_jit_set_quiet_warnings(true);
        if (!scan_autolink_markers(&csrc, &autolink)) {
            rc = -1;
        } else if (repl_jit_compile_image(&csrc,
                                          autolink.len ? autolink.data : NULL,
                                          /*prune=*/true, &img) != TUR_JIT_OK) {
            rc = -1;
        }
        tur_jit_set_quiet_warnings(was_quiet);
        buf_free(&autolink);
    }
    buf_free(&csrc);

    /* Put the turn's diagnostic state back exactly as it was. */
    diag_reset();
    diag_files_replace(saved_files, n_saved);
    free(saved_files);
    if (had_error) diag_force_had_error();

    if (rc != 0 || !img) {
        buf_free(&manifest);
        return -1;
    }
    buf_putc(&manifest, '\0');
    *out_manifest = strdup(manifest.data);
    buf_free(&manifest);
    if (!*out_manifest) return -1;
    *out_image = img;
    return 0;
}

/* The build runs on a stack of its own.  It is reached in the middle of an
 * evaluation, which may be on one of turi's small makecontext stacks (a
 * generator, effect or async body): seq-builders-unfold compiles from a
 * generator, and elaboration and emit overflowed that stack under ASan. */
typedef struct {
    const char *name, *src;
    size_t len;
    void **out_image;
    char **out_manifest;
} ReplInlineCJitJob;

static int repl_inline_c_jit_job(void *p) {
    ReplInlineCJitJob *j = (ReplInlineCJitJob *)p;
    return repl_inline_c_jit_build_here(j->name, j->src, j->len,
                                        j->out_image, j->out_manifest);
}

static int repl_inline_c_jit_build(const char *name, const char *src,
                                   size_t len, void **out_image,
                                   char **out_manifest) {
    ReplInlineCJitJob j = { name, src, len, out_image, out_manifest };
    return tur_run_on_big_stack(repl_inline_c_jit_job, &j);
}

const TuriInlineCJitHook g_repl_inline_c_jit_hook = {
    repl_inline_c_jit_build, repl_jit_hook_sym,
};
#else /* !TUR_HAVE_JIT */

int cmd_jit(int argc, char **argv) {
    (void)argc; (void)argv;
    fprintf(stderr,
            "tur: this build carries no JIT engine; reconfigure with "
            "-DTUR_JIT=ON\n"
            "     (the default on 64-bit x86-64/arm64; MIR is vendored under "
            "external/mir)\n");
    return 2;
}

#endif /* TUR_HAVE_JIT */
