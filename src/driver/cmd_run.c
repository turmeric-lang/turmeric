/* cmd_run.c -- `tur run <file>` (and a bare `tur run` in a Justfile-less
 * project), `tur test <dir>` and `tur check <dir>`.
 *
 * Part of the compiler driver (src/driver/); driver.h says how the files
 * share declarations. */

#include "driver.h"
#include "commands.h"

#ifndef _WIN32
#include <sys/wait.h>
#endif

#include "prof.h"         /* profiling-plan C0+C1: --timings */
#include "global.h"   /* tur_installed_spice_dir, use_json_output */
#include "globals.h"

static int decode_exit_status(int status) {
    if (status == -1) return 127;
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
    return 1;
}

int cmd_run(int argc, char **argv) {
    /* tur run [-I <dir>...] [--release] [--offline] [<file>] [-- <args>...] */
    bool        release           = false;
    bool        offline           = false;
    const char *explicit_file     = NULL;
    const char *engine_flag       = NULL;   /* engine-selection-plan E1 */
    int         passthrough_start = -1;

    /* SC2: collect -I flags up front (stops scanning at `--`, since
     * everything after is passthrough to the spawned program). */
    int  scan_end = argc;
    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "--") == 0) { scan_end = i; break; }
    }
    char **user_inc = NULL;
    int    n_user_inc = drv_parse_include_flags(scan_end, argv, 2, &user_inc);
    if (n_user_inc < 0) { free(user_inc); return drv_usage_error(usage_run); }

    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "--") == 0) {
            passthrough_start = i + 1;
            break;
        }
        int c;
        if (drv_is_include_flag(argc, argv, i, &c)) { i += c - 1; continue; }
        if (strcmp(argv[i], "--release") == 0) {
            release = true;
        } else if (strcmp(argv[i], "--offline") == 0) {
            offline = true;
        } else if (strcmp(argv[i], "--engine") == 0 && i + 1 < scan_end) {
            /* engine-selection-plan E1: CLI rung of the ladder. */
            engine_flag = argv[++i];
        } else if (strncmp(argv[i], "--engine=", 9) == 0) {
            engine_flag = argv[i] + 9;
        } else if (argv[i][0] != '-' || strcmp(argv[i], "-") == 0) {
            if (!explicit_file) explicit_file = argv[i];
        }
    }
    /* Read where cmd_build_once picks its default cc flags.  A non-cc engine
     * (tur jit, the interpreter) has no cc step for it to change. */
    g_cc_release = release;

    /* spice_inc_dirs: populated below during project-mode setup.
     * RUN_ENTRY captures these via the enclosing scope.  User-supplied
     * -I flags (user_inc above) are merged in with priority over
     * project-inferred dirs (first match wins in the elaborator's
     * include search). */
    const char **spice_inc_dirs = NULL;
    int          n_spice_inc_dirs = 0;

    /* RM4: reader-macro definition files preloaded for the entry file.
     * Populated below during project-mode setup from the manifest's
     * `:reader-macros [...]` entry; stays NULL/0 in single-file mode.
     * `rm_paths_owned` is the heap-allocated backing storage (freed at
     * RUN_ENTRY's exit and at all early-return paths via the macro). */
    char       **rm_paths_owned = NULL;
    const char **rm_paths       = NULL;
    int          n_rm_paths     = 0;

    /* LS2 workspace-resolver ctx: populated by the explicit-file
     * auto-append below.  Stays zero-init for project mode (the gate in
     * elab_toplevel.c then matches no slots and the warning path is
     * inert). */
    Ls2ResolverCtx run_ls2 = {0};

    /* Helper: build 'entry', exec with optional passthrough args.
     * Cleans up user_inc, spice_inc_dirs, and auto_src_run on every exit. */
#define RUN_ENTRY(entry_path) do {                                       \
        char out_path[512]; \
        snprintf(out_path, sizeof(out_path), "%s/tur-run-XXXXXX", tur_temp_dir()); \
        int _fd = mkstemp(out_path);                                     \
        if (_fd < 0) { fprintf(stderr, "tur: mkstemp failed\n"); free(user_inc); free_reader_macro_paths(rm_paths_owned, n_rm_paths); for (int _i = 0; _i < n_auto_run_owned; _i++) free(auto_run_owned[_i]); \
        free(auto_run_owned); ls2_resolver_ctx_dispose(&run_ls2); return 2; } \
        close(_fd);                                                      \
        tur_exe_path(out_path, sizeof(out_path));                        \
        int _n_inc = n_user_inc + n_spice_inc_dirs;                      \
        const char **_inc = NULL;                                        \
        if (_n_inc > 0) {                                                \
            _inc = (const char **)malloc((size_t)_n_inc * sizeof(char *)); \
            for (int _i = 0; _i < n_user_inc; _i++) _inc[_i] = user_inc[_i]; \
            for (int _i = 0; _i < n_spice_inc_dirs; _i++) _inc[n_user_inc + _i] = spice_inc_dirs[_i]; \
        }                                                                \
        ls2_resolver_ctx_set(&run_ls2);                                  \
        int _rc = cmd_build((entry_path), out_path, _inc, _n_inc, NULL, rm_paths, n_rm_paths); \
        ls2_resolver_ctx_set(NULL);                                      \
        free(_inc);                                                      \
        if (_rc != 0) { unlink(out_path); for (int _i = 0; _i < n_spice_inc_dirs; _i++) free((char *)spice_inc_dirs[_i]); free(spice_inc_dirs); free(user_inc); free_reader_macro_paths(rm_paths_owned, n_rm_paths); for (int _i = 0; _i < n_auto_run_owned; _i++) free(auto_run_owned[_i]); \
        free(auto_run_owned); ls2_resolver_ctx_dispose(&run_ls2); return _rc; } \
        Buf _cmd; buf_init(&_cmd);                                       \
        bool _q = drv_buf_put_quoted(&_cmd, out_path);                       \
        if (passthrough_start >= 0) {                                    \
            for (int _i = passthrough_start; _i < argc; _i++) {          \
                buf_putc(&_cmd, ' ');                                    \
                _q = drv_buf_put_quoted(&_cmd, argv[_i]) && _q;              \
            }                                                            \
        }                                                                \
        buf_putc(&_cmd, '\0');                                           \
        /* tur-run-dump-output-interleaves-program-output: a --dump-* \
         * report still in tur's stdout buffer would land after the     \
         * program's own output, mid-line, once stdout is a file. */    \
        fflush(NULL);                                                    \
        prof_enter(PROF_RUN);                                            \
        int _sys = _q ? system(_cmd.data) : 2;                           \
        prof_exit(PROF_RUN);                                             \
        if (!_q) fprintf(stderr, "tur run: an argument was too long to quote\n"); \
        buf_free(&_cmd);                                                  \
        unlink(out_path);                                                \
        for (int _i = 0; _i < n_spice_inc_dirs; _i++) free((char *)spice_inc_dirs[_i]); \
        free(spice_inc_dirs);                                            \
        free(user_inc);                                                  \
        free_reader_macro_paths(rm_paths_owned, n_rm_paths);             \
        for (int _i = 0; _i < n_auto_run_owned; _i++) free(auto_run_owned[_i]); \
        free(auto_run_owned);                                              \
        ls2_resolver_ctx_dispose(&run_ls2);                              \
        return decode_exit_status(_sys);                                 \
    } while (0)

    /* SC4+SC5: in explicit-file mode, auto-discover the file's enclosing
     * spice and add its `src/` AND every :spices dep's src/ to the
     * include path so intra-spice and cross-spice imports resolve
     * without explicit -I (matching `tur check`).  Project mode below
     * sets up its own spice_inc_dirs from build.tur, so auto-discovery
     * is skipped there to avoid double-adding the same paths. */
    char **auto_run_owned = NULL;
    int    n_auto_run_owned = 0;
    if (explicit_file && strcmp(explicit_file, "-") != 0) {
        auto_append_spice_includes(explicit_file, &user_inc, &n_user_inc,
                                   &auto_run_owned, &n_auto_run_owned,
                                   &run_ls2);
    }

    /* Single-file mode: explicit file provided, skip project lookup. */
    if (explicit_file) {
        /* E6: treat "-" as stdin -- buffer it into a temp .tur file first. */
        if (strcmp(explicit_file, "-") == 0) {
            char src_tmp[512];
            snprintf(src_tmp, sizeof(src_tmp), "%s/tur-stdin-XXXXXX.tur", tur_temp_dir());
            int src_fd = mkstemps(src_tmp, 4);
            if (src_fd < 0) { fprintf(stderr, "tur: mkstemp failed\n"); free(user_inc); return 2; }
            char ibuf[4096]; size_t nr;
            int ok = 1;
            while ((nr = fread(ibuf, 1, sizeof(ibuf), stdin)) > 0)
                if ((size_t)write(src_fd, ibuf, nr) != nr) { ok = 0; break; }
            close(src_fd);
            if (!ok) {
                unlink(src_tmp);
                fprintf(stderr, "tur: error reading stdin\n");
                free(user_inc);
                return 2;
            }
            char out_path[512];
            snprintf(out_path, sizeof(out_path), "%s/tur-run-XXXXXX", tur_temp_dir());
            int out_fd = mkstemp(out_path);
            if (out_fd < 0) { unlink(src_tmp); fprintf(stderr, "tur: mkstemp failed\n"); free(user_inc); return 2; }
            close(out_fd);
            tur_exe_path(out_path, sizeof(out_path));
            /* SC2: stdin mode never enters project setup, so spice_inc_dirs
             * is empty here; we can pass user_inc straight through. */
            int brc = cmd_build(src_tmp, out_path,
                                (const char **)user_inc, n_user_inc, NULL,
                                NULL, 0);
            unlink(src_tmp);
            if (brc != 0) { unlink(out_path); free(user_inc); return brc; }
            Buf run_cmd; buf_init(&run_cmd);
            bool rq = drv_buf_put_quoted(&run_cmd, out_path);
            if (passthrough_start >= 0)
                for (int i = passthrough_start; i < argc; i++) {
                    buf_putc(&run_cmd, ' ');
                    rq = drv_buf_put_quoted(&run_cmd, argv[i]) && rq;
                }
            buf_putc(&run_cmd, '\0');
            if (!rq)
                fprintf(stderr, "tur run: an argument was too long to quote\n");
            fflush(NULL);   /* as RUN_ENTRY: a dump first, then the program */
            prof_enter(PROF_RUN);
            int sys = rq ? system(run_cmd.data) : 2;
            prof_exit(PROF_RUN);
            buf_free(&run_cmd);
            unlink(out_path);
            free(user_inc);
            return decode_exit_status(sys);
        }
        /* RM4: in explicit-file mode, walk up from the file to discover an
         * enclosing build.tur and apply its `:reader-macros [...]` if any.
         * Mirrors the auto-include discovery a few lines above.  The shared
         * helper also applies the manifest's `:experiments` (and the user
         * experiments file), which a hand-rolled copy of this walk used to
         * skip -- so `tur check f.tur` honoured a project's gates and
         * `tur run f.tur` silently compiled with them off. */
        rm_paths_owned = discover_manifest_reader_macros(explicit_file,
                                                         &n_rm_paths);
        rm_paths = (const char **)rm_paths_owned;
        /* engine-selection-plan E3: a resolved non-cc engine delegates to
         * that engine's own arm; "cc" continues into RUN_ENTRY unchanged. */
        {
            const char *eng = drv_resolve_engine(explicit_file, engine_flag);
            if (!eng) {
                free(user_inc);
                free_reader_macro_paths(rm_paths_owned, n_rm_paths);
                ls2_resolver_ctx_dispose(&run_ls2);
                return 2;
            }
            if (strcmp(eng, "cc") != 0) {
                int drc = run_delegate_engine(eng, explicit_file,
                                              user_inc, n_user_inc,
                                              argc, argv, passthrough_start);
                free(user_inc);
                for (int _i = 0; _i < n_auto_run_owned; _i++)
                    free(auto_run_owned[_i]);
                free(auto_run_owned);
                free_reader_macro_paths(rm_paths_owned, n_rm_paths);
                ls2_resolver_ctx_dispose(&run_ls2);
                return drc;
            }
        }
        RUN_ENTRY(explicit_file);
    }

    /* Project mode: walk up to find build.tur. */
    char cwd[4096];
    if (!getcwd(cwd, sizeof(cwd))) {
        fprintf(stderr, "tur run: cannot get current directory\n");
        return 2;
    }

    char *root = drv_find_project_root(cwd);
    if (!root) {
        fprintf(stderr,
            "tur run: no build.tur found (searched up from '%s')\n"
            "  Create a project with `tur new <name>`, "
            "or pass a file directly: tur run src/main.tur\n",
            cwd);
        return 1;
    }

    /* Parse manifest for entry point configuration. */
    char manifest_path[4096];
    if (!pkg_resolve_manifest_path(root, manifest_path,
                                   sizeof(manifest_path))) {
        free(root);
        return 1;
    }
    PkgManifest m;
    memset(&m, 0, sizeof(m));
    if (!pkg_manifest_read(manifest_path, &m)) { free(root); return 1; }

    /* RM4: resolve manifest reader-macros against the spice root and
     * thread them through RUN_ENTRY/cmd_build below. The allocated
     * string array is owned by cmd_run for the remainder of this call;
     * release sites already free user_inc / spice_inc_dirs and now also
     * free this. */
    rm_paths_owned = resolve_manifest_reader_macros(root, &m, &n_rm_paths);
    rm_paths       = (const char **)rm_paths_owned;

    /* The manifest is already in hand, so apply its `:experiments` (and the
     * user experiments file, which it suppresses when it declares the key)
     * here rather than through discover_manifest_reader_macros.  Without this
     * `tur run` in a project compiled with the project's gates off, where
     * `tur run src/main.tur` had them on. */
    apply_user_config_experiments(&m);
    apply_manifest_experiments(&m);

    /* Read lock file. */
    char lock_path[4096];
    snprintf(lock_path, sizeof(lock_path), "%s/tur.lock", root);
    PkgLockFile lock;
    memset(&lock, 0, sizeof(lock));
    lock.format_version = 1;
    pkg_lock_read(lock_path, &lock);

    /* Spice dependency handling. */
    if (m.n_spices > 0) {
        char spices_dir[4096];
        snprintf(spices_dir, sizeof(spices_dir), "%s/spices", root);

        if (offline) {
            /* Verify all required spices exist on disk. */
            bool missing = false;
            for (int i = 0; i < m.n_spices; i++) {
                const PkgSpice *s = &m.spices[i];
                if (s->path) continue; /* local path -- always present */
                /* LS4: workspace-sibling :spices entries are local-source
                 * (the sibling lives under the enclosing workspace), so
                 * they cannot be "missing" the way a never-fetched URL
                 * dep can be.  Skip them here. */
                if (pkg_is_workspace_member(root, s->name)) continue;
                char dep_dir[4096];
                if (s->ref)
                    snprintf(dep_dir, sizeof(dep_dir), "%s/%s-%s",
                             spices_dir, s->name, s->ref);
                else
                    snprintf(dep_dir, sizeof(dep_dir), "%s/%s",
                             spices_dir, s->name);
                struct stat _dstat;
                if (stat(dep_dir, &_dstat) != 0 || !S_ISDIR(_dstat.st_mode)) {
                    fprintf(stderr,
                        "tur run: --offline: spice '%s' not found at '%s'\n",
                        s->name, dep_dir);
                    missing = true;
                }
            }
            if (missing) {
                pkg_lock_free(&lock);
                pkg_manifest_free(&m);
                free(root);
                return 1;
            }
        } else {
            /* Fetch any missing spices; verify SHA-256 of already-fetched ones. */
            bool need_fetch = false;
            for (int i = 0; i < m.n_spices; i++) {
                const PkgSpice *s = &m.spices[i];
                if (s->path) continue;
                /* LS4: workspace siblings never trigger a remote fetch and
                 * have no lockfile row to check; the resolver picks them
                 * up via the workspace walk below. */
                if (pkg_is_workspace_member(root, s->name)) continue;
                char dep_dir[4096];
                if (s->ref)
                    snprintf(dep_dir, sizeof(dep_dir), "%s/%s-%s",
                             spices_dir, s->name, s->ref);
                else
                    snprintf(dep_dir, sizeof(dep_dir), "%s/%s",
                             spices_dir, s->name);
                struct stat _dstat;
                if (stat(dep_dir, &_dstat) != 0 || !S_ISDIR(_dstat.st_mode))
                    need_fetch = true;
            }
            /* C-3: the per-dep hash comparison used to be open-coded right
             * here and existed nowhere else in the tree, so `tur build` --
             * the command that actually compiles a dependency's code --
             * checked nothing at all.  One implementation, three callers. */
            if (!pkg_verify_locked_spices(root, &m, &lock, "tur run")) {
                pkg_lock_free(&lock);
                pkg_manifest_free(&m);
                free(root);
                return 1;
            }
            if (need_fetch) {
                /* LS5: partial-fetch isolation -- a single broken URL dep
                 * must not poison the search path for healthy ones.
                 * pkg_fetch_all continues past per-dep failures (sets ok
                 * false but doesn't break), so the on-disk state for
                 * successfully-fetched deps is consistent.  Write whatever
                 * lock entries we have and proceed; if the entry file
                 * actually imports a symbol from the missing dep, the
                 * elaborator's "module not found" diagnostic fires for
                 * that import alone.  Files that only reference healthy
                 * deps keep compiling. */
                bool fetch_ok = pkg_fetch_all(root, &m, &lock, false, NULL);
                pkg_lock_write(lock_path, &lock);
                if (!fetch_ok) {
                    fprintf(stderr,
                        "tur run: one or more dependencies failed to "
                        "fetch; continuing with healthy deps on disk. "
                        "Imports referencing the missing dep(s) will "
                        "fail with 'module not found'.\n");
                }
            }
        }

        /* Build the spice include-path array from the manifest via the
         * shared resolver. `tur run` project mode does not add the project's
         * own `src/` here -- the entry file lives inside `src/`, so the
         * resolver already searches it -- whereas `tur test` / `tur build
         * <dir>` request it (their inputs sit outside `src/`). */
        resolve_include_dirs_from_manifest(root, &m, /*include_own_src=*/false,
                                           &spice_inc_dirs, &n_spice_inc_dirs);
    }

    /* CMake dependency handling: generate and build if cmake-deps present.
     * Walk the enclosing manifest's :spices block transitively so a declared
     * dependency's :cmake-deps participate in this TU's build --
     * see docs/archive/history/transitive-cmake-deps-plan.md. */
    PkgCmakeDep *closure_deps = NULL;
    int          n_closure_deps = 0;
    if (!pkg_collect_transitive_cmake_deps(root, &m,
                                           pkg_workspace_wide_cmake_deps(),
                                           &closure_deps, &n_closure_deps)) {
        fprintf(stderr, "tur run: transitive cmake-deps resolution failed\n");
        pkg_lock_free(&lock);
        pkg_manifest_free(&m);
        for (int _i = 0; _i < n_spice_inc_dirs; _i++) free((char *)spice_inc_dirs[_i]);
        free(spice_inc_dirs);
        free(root);
        return 1;
    }
    if (n_closure_deps > 0) {
        /* Build a synthetic manifest aliasing `m`'s other fields and
         * swapping in the unioned cmake_deps so pkg_gen_cmake_deps and
         * pkg_cmake_build see the full set. */
        PkgManifest mu = m;
        mu.cmake_deps   = closure_deps;
        mu.n_cmake_deps = n_closure_deps;

        /* tur-run-cmake-marker-skips-after-failed-configure: keyed on the dep
         * manifest the compile reads, as `tur build <dir>` is.  The shared
         * cmake/CMakeLists.txt is written BEFORE the configure that can fail,
         * so after one failure every later `tur run` skipped the dep build
         * and compiled with no -I/-L/-l at all, failing far from the cause.
         * CMake writes the manifest only on a successful configure. */
        char cmake_marker[4096];
        snprintf(cmake_marker, sizeof(cmake_marker), "%s/cmake/%s", root,
                 pkg_cmake_manifest_name(NULL));
        struct stat _cmst;
        bool cmake_built = (stat(cmake_marker, &_cmst) == 0);
        if (!cmake_built) {
            if (!pkg_gen_cmake_deps(root, &mu) ||
                !pkg_cmake_build(root, &mu, &lock, NULL)) {
                /* A configure that succeeded wrote the manifest even though
                 * the build after it failed; drop it so the next run retries. */
                (void)remove(cmake_marker);
                fprintf(stderr, "tur run: cmake dependency build failed\n");
                pkg_cmake_deps_free(closure_deps, n_closure_deps);
                pkg_lock_free(&lock);
                pkg_manifest_free(&m);
                for (int _i = 0; _i < n_spice_inc_dirs; _i++) free((char *)spice_inc_dirs[_i]);
                free(spice_inc_dirs);
                free(root);
                return 1;
            }
            pkg_lock_write(lock_path, &lock);
        }
    }
    pkg_cmake_deps_free(closure_deps, n_closure_deps);

    pkg_lock_free(&lock);

    /* Entry point resolution (from the plan):
     *   1. :entry key in build.tur
     *   2. src/main.tur
     *   3. single .tur file in src/ */
    char entry[4096];
    struct stat _st;
    if (m.entry && *m.entry) {
        /* A manifest-declared entry is authoritative: if it does not resolve
         * we report THAT rather than silently falling through to src/main.tur,
         * which would run a different program than the manifest asked for. */
        if (m.entry[0] == '/')
            snprintf(entry, sizeof(entry), "%s", m.entry);
        else
            snprintf(entry, sizeof(entry), "%s/%s", root, m.entry);
        if (stat(entry, &_st) != 0) {
            fprintf(stderr,
                "tur run: :entry \"%s\" in build.tur does not name a file\n"
                "  Looked for %s\n",
                m.entry, entry);
            pkg_manifest_free(&m);
            free(root);
            for (int _i = 0; _i < n_spice_inc_dirs; _i++)
                free((char *)spice_inc_dirs[_i]);
            free(spice_inc_dirs);
            free(user_inc);
            return 1;
        }
        goto entry_resolved;
    }
    snprintf(entry, sizeof(entry), "%s/src/main.tur", root);
    if (stat(entry, &_st) != 0) {
        char src_dir[4096];
        snprintf(src_dir, sizeof(src_dir), "%s/src", root);
        int n_files = 0;
        char **files = collect_tur_files(src_dir, &n_files);
        if (n_files == 1) {
            strncpy(entry, files[0], sizeof(entry) - 1);
            entry[sizeof(entry) - 1] = '\0';
        } else {
            fprintf(stderr,
                "tur run: cannot determine entry point\n"
                "  Expected %s/src/main.tur, "
                "or exactly one .tur file in %s/src/,\n"
                "  or an :entry \"<path>\" key in build.tur\n",
                root, root);
            free_tur_files(files, n_files);
            pkg_manifest_free(&m);
            free(root);
            for (int _i = 0; _i < n_spice_inc_dirs; _i++)
                free((char *)spice_inc_dirs[_i]);
            free(spice_inc_dirs);
            free(user_inc);
            return 1;
        }
        free_tur_files(files, n_files);
    }

entry_resolved:
    pkg_manifest_free(&m);
    free(root);

    /* engine-selection-plan E3: project-mode engine dispatch.  The entry is
     * now known, so a manifest `:engine "interp"` makes a bare `tur run`
     * tree-walk this project (and --engine / TUR_ENGINE override it). */
    {
        const char *eng = drv_resolve_engine(entry, engine_flag);
        if (!eng) {
            for (int _i = 0; _i < n_spice_inc_dirs; _i++)
                free((char *)spice_inc_dirs[_i]);
            free(spice_inc_dirs);
            free(user_inc);
            free_reader_macro_paths(rm_paths_owned, n_rm_paths);
            return 2;
        }
        if (strcmp(eng, "cc") != 0) {
            int drc = run_delegate_engine(eng, entry, user_inc, n_user_inc,
                                          argc, argv, passthrough_start);
            for (int _i = 0; _i < n_spice_inc_dirs; _i++)
                free((char *)spice_inc_dirs[_i]);
            free(spice_inc_dirs);
            free(user_inc);
            free_reader_macro_paths(rm_paths_owned, n_rm_paths);
            return drc;
        }
    }

    RUN_ENTRY(entry);
#undef RUN_ENTRY
}

/* Per-test directives, read from a test file's leading comment lines:
 *
 *   ;; tur-test-flags: --strict-refine     -- extra compile flags for THIS test
 *   ;; tur-test-expect-error: TUR-W0372    -- this test must FAIL to compile and
 *                                             its diagnostics must contain this
 *                                             text; the run phase is skipped.
 *
 * A file with no directive behaves exactly as before.  This is what lets a
 * refined ecs test enforce its proof (`--strict-refine`, so an unproven crossing
 * is a hard error, not a warning) and lets an expected-fail negative be a real
 * test rather than documentation.  `--enable=refined` needs no flag here: the
 * file's `#lang turmeric refined` line enables it for that compile. */
typedef struct TestDirectives {
    bool strict_refine;
    char expect_error[128];   /* empty => not an expect-error test */
} TestDirectives;

static void parse_test_directives(const char *path, TestDirectives *out) {
    out->strict_refine = false;
    out->expect_error[0] = '\0';
    FILE *f = fopen(path, "r");
    if (!f) return;
    char line[512];
    int scanned = 0;
    while (scanned < 40 && fgets(line, sizeof(line), f)) {
        scanned++;
        const char *d;
        if ((d = strstr(line, "tur-test-flags:")) != NULL) {
            if (strstr(d, "--strict-refine")) out->strict_refine = true;
        }
        if ((d = strstr(line, "tur-test-expect-error:")) != NULL) {
            d += strlen("tur-test-expect-error:");
            while (*d == ' ' || *d == '\t') d++;
            size_t k = 0;
            while (*d && *d != '\n' && *d != '\r' && k + 1 < sizeof(out->expect_error))
                out->expect_error[k++] = *d++;
            while (k > 0 && (out->expect_error[k-1] == ' ' || out->expect_error[k-1] == '\t')) k--;
            out->expect_error[k] = '\0';
        }
    }
    fclose(f);
}

int cmd_test(const char *dir) {
    int n_files = 0;
    char **tur_files = collect_tur_files_deep(dir, &n_files);
    if (!tur_files || n_files == 0) {
        fprintf(stderr, "tur: no .tur files found in '%s'\n", dir);
        free_tur_files(tur_files, n_files);
        return 1;
    }

    qsort(tur_files, (size_t)n_files, sizeof(char *), drv_compare_cstr_ptrs);

    /* Bug 3 fix: derive `-I` include paths from the nearest build.tur so
     * test files can `(import test/suite ...)`, `(import plutovg/surface ...)`,
     * etc. without the caller having to pass `-I` flags manually.
     *
     * Includes:
     *   - the project's own `src/` directory (so e.g. plutovg tests can
     *     import plutovg/surface);
     *   - for each `:spices` entry in build.tur, the resolved spice's
     *     `src/` directory (so plutovg can pull in test/assert, etc.).
     *
     * The cmake-deps include/link flags are already wired up inside
     * `cmd_build` via `pkg_cmake_manifest_read`, so we don't redo that.
     */
    const char **spice_inc_dirs = NULL;
    int          n_spice_inc_dirs = 0;
    resolve_project_include_dirs(dir, &spice_inc_dirs, &n_spice_inc_dirs);

    int passed = 0;
    int failed = 0;
    char **failed_files = (char **)calloc((size_t)n_files, sizeof(char *));
    if (!failed_files) {
        fprintf(stderr, "tur: oom\n");
        free_tur_files(tur_files, n_files);
        for (int j = 0; j < n_spice_inc_dirs; j++) free((char *)spice_inc_dirs[j]);
        free(spice_inc_dirs);
        return 2;
    }

    for (int i = 0; i < n_files; i++) {
        char out_path[512];
        snprintf(out_path, sizeof(out_path), "%s/tur-test-XXXXXX", tur_temp_dir());
        int fd = mkstemp(out_path);
        if (fd < 0) {
            fprintf(stderr, "tur: mkstemp failed for %s\n", tur_files[i]);
            failed_files[failed++] = tur_files[i];
            putchar('F');
            continue;
        }
        close(fd);
        tur_exe_path(out_path, sizeof(out_path));

        TestDirectives td;
        parse_test_directives(tur_files[i], &td);
        bool prev_strict = g_strict_refine;
        if (td.strict_refine) g_strict_refine = true;
        bool expect_fail = td.expect_error[0] != '\0';

        /* For an expect-error test, capture stderr so we can confirm the
         * front-end emitted the named diagnostic (not some unrelated failure). */
        int stderr_bak = -1;
        char cap_path[512] = {0};
        if (expect_fail) {
            snprintf(cap_path, sizeof(cap_path), "%s/tur-test-err-XXXXXX", tur_temp_dir());
            int cfd = mkstemp(cap_path);
            if (cfd >= 0) {
                fflush(stderr);
                stderr_bak = dup(fileno(stderr));
                dup2(cfd, fileno(stderr));
                close(cfd);
            } else {
                cap_path[0] = '\0';
            }
        }

        int rm_n = 0;
        char **rm_p = discover_manifest_reader_macros(tur_files[i], &rm_n);
        int build_rc = cmd_build(tur_files[i], out_path,
                                  spice_inc_dirs, n_spice_inc_dirs, NULL,
                                  (const char **)rm_p, rm_n);
        free_reader_macro_paths(rm_p, rm_n);

        if (expect_fail && stderr_bak >= 0) {
            fflush(stderr);
            dup2(stderr_bak, fileno(stderr));
            close(stderr_bak);
        }
        g_strict_refine = prev_strict;

        if (expect_fail) {
            /* Pass iff the build FAILED and the captured diagnostics name the
             * expected error -- so a negative test cannot pass on an unrelated
             * compile error. The run phase is skipped. */
            char *cap = NULL;
            bool found = false;
            if (cap_path[0]) {
                FILE *cf = fopen(cap_path, "r");
                if (cf) {
                    cap = (char *)malloc(65536);
                    size_t got = cap ? fread(cap, 1, 65535, cf) : 0;
                    if (cap) cap[got] = '\0';
                    fclose(cf);
                }
                unlink(cap_path);
            }
            if (cap) found = strstr(cap, td.expect_error) != NULL;
            unlink(out_path);
            if (build_rc != 0 && found) {
                passed++;
                putchar('.');
            } else {
                failed_files[failed++] = tur_files[i];
                putchar('F');
                fprintf(stderr,
                        "\nEXPECT-ERROR %s: wanted diagnostic '%s' with a failed "
                        "build; got build_rc=%d, diagnostic %s\n",
                        tur_files[i], td.expect_error, build_rc,
                        found ? "found" : "NOT found");
                if (cap && !found && cap[0]) fprintf(stderr, "%s\n", cap);
            }
            free(cap);
            continue;
        }

        int run_rc = 1;
        if (build_rc == 0) {
            /* Quoted: the temp dir is user-controlled (TMP/TMPDIR) and may
             * contain spaces -- "C:\Users\Foo Bar\AppData\Local\Temp" is a
             * perfectly ordinary Windows path. */
            Buf test_cmd; buf_init(&test_cmd);
            bool tq = drv_buf_put_quoted(&test_cmd, out_path);
            buf_putc(&test_cmd, '\0');
            prof_enter(PROF_RUN);   /* --timings: the test binary's own run */
            int status = tq ? system(test_cmd.data) : 2;
            prof_exit(PROF_RUN);
            buf_free(&test_cmd);
            run_rc = decode_exit_status(status);
        }
        unlink(out_path);

        if (build_rc == 0 && run_rc == 0) {
            passed++;
            putchar('.');
        } else {
            failed_files[failed++] = tur_files[i];
            putchar('F');
        }
    }

    if (use_json_output) {
        printf("{\"total\":%d,\"passed\":%d,\"failed\":%d,\"tests\":[",
               n_files, passed, failed);
        bool first = true;
        for (int i = 0; i < n_files; i++) {
            bool is_fail = false;
            for (int j = 0; j < failed; j++) {
                if (failed_files[j] == tur_files[i]) { is_fail = true; break; }
            }
            if (!first) printf(",");
            first = false;
            char esc[1024];
            drv_json_escape(tur_files[i], esc, sizeof(esc));
            printf("{\"file\":\"%s\",\"status\":\"%s\"}", esc, is_fail ? "fail" : "pass");
        }
        printf("]}\n");
    } else {
        putchar('\n');
        printf("%d tests, %d passed, %d failed\n", n_files, passed, failed);
        for (int i = 0; i < failed; i++) {
            printf("FAIL %s\n", failed_files[i]);
        }
    }

    free(failed_files);
    free_tur_files(tur_files, n_files);
    for (int j = 0; j < n_spice_inc_dirs; j++) free((char *)spice_inc_dirs[j]);
    free(spice_inc_dirs);
    return failed == 0 ? 0 : 1;
}

/* Type-check every .tur file under a directory (no codegen kept).
 * Mirrors cmd_test's discovery so `tur check src/` works inside a spice
 * without a per-file path. Resolves intra-spice include dirs from the
 * nearest build.tur, exactly like cmd_test. Returns 0 iff all files pass. */
int cmd_check_dir(const char *dir) {
    int n_files = 0;
    char **tur_files = collect_tur_files_deep(dir, &n_files);
    if (!tur_files || n_files == 0) {
        fprintf(stderr, "tur: no .tur files found in '%s'\n", dir);
        free_tur_files(tur_files, n_files);
        return 1;
    }
    qsort(tur_files, (size_t)n_files, sizeof(char *), drv_compare_cstr_ptrs);

    const char **inc = NULL;
    int          n_inc = 0;
    resolve_project_include_dirs(dir, &inc, &n_inc);

    int failed = 0;
    if (use_json_output) diag_lsp_begin();
    for (int i = 0; i < n_files; i++) {
        int rm_n = 0;
        char **rm_p = discover_manifest_reader_macros(tur_files[i], &rm_n);
        Buf out;
        buf_init(&out);
        int rc = compile_to_c(tur_files[i], &out, inc, n_inc,
                              (const char **)rm_p, rm_n);
        buf_free(&out);
        free_reader_macro_paths(rm_p, rm_n);
        if (rc != 0) failed++;
    }
    if (use_json_output) { diag_lsp_flush(stdout); diag_lsp_end(); }

    free_tur_files(tur_files, n_files);
    for (int j = 0; j < n_inc; j++) free((char *)inc[j]);
    free(inc);
    return failed == 0 ? 0 : 1;
}
