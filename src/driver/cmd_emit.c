/* cmd_emit.c -- `tur emit-c` and `tur emit-h`, and the build-directory and
 * output-name resolution the build commands share.
 *
 * Part of the compiler driver (src/driver/); driver.h says how the files
 * share declarations. */

#include "driver.h"
#include "commands.h"

#include "global.h"       /* use_json_output */
#include "globals.h"      /* g_dump_expansion */

static int cmd_emit_c(const char *path,
                      const char **include_dirs, int n_include_dirs) {
    Buf out;
    buf_init(&out);
    int n_rm = 0;
    char **rm = discover_manifest_reader_macros(path, &n_rm);
    int rc = compile_to_c(path, &out, include_dirs, n_include_dirs,
                          (const char **)rm, n_rm);
    free_reader_macro_paths(rm, n_rm);
    if (rc == 0) buf_to_file(&out, stdout);
    buf_free(&out);
    return rc;
}

/* Phase B: emit per-module .h and .c files to a directory.
 * Usage: tur emit-c --output-dir <dir> <file1.tur> [<file2.tur> ...]
 * Each input produces <dir>/<module>.h and <dir>/<module>.c.
 * J7: uses the same two-pass ABI specialization as cmd_build_multi. */
static int cmd_emit_c_to_dir(const char *out_dir, char **inputs, int n_inputs,
                             const char **include_dirs, int n_include_dirs) {
    if (n_inputs < 1) {
        fprintf(stderr, "tur emit-c --output-dir: at least one input required\n");
        return 1;
    }

    /* Create output directory if it doesn't exist */
    struct stat st_od;
    if (stat(out_dir, &st_od) != 0) {
        if (mkdir(out_dir, 0755) != 0 && errno != EEXIST) {
            fprintf(stderr, "tur emit-c: cannot create '%s': %s\n",
                    out_dir, strerror(errno));
            return 2;
        }
    }

    /* Pre-compute module names. */
    char **ecd_mod_names = (char **)malloc(n_inputs * sizeof(char *));
    char **ecd_h_paths   = (char **)malloc(n_inputs * sizeof(char *));
    char **ecd_c_paths   = (char **)malloc(n_inputs * sizeof(char *));
    if (!ecd_mod_names || !ecd_h_paths || !ecd_c_paths) {
        fprintf(stderr, "tur: oom\n"); return 2;
    }
    for (int i = 0; i < n_inputs; i++) {
        const char *base = drv_basename_of(inputs[i]);
        size_t base_len = strlen(base);
        size_t n = drv_source_stem_len(base, base_len);
        ecd_mod_names[i] = (char *)malloc(n + 1);
        memcpy(ecd_mod_names[i], base, n);
        ecd_mod_names[i][n] = '\0';
        size_t path_sz = strlen(out_dir) + n + 4;
        ecd_h_paths[i] = (char *)malloc(path_sz);
        ecd_c_paths[i] = (char *)malloc(path_sz);
        snprintf(ecd_h_paths[i], path_sz, "%s/%s.h", out_dir, ecd_mod_names[i]);
        snprintf(ecd_c_paths[i], path_sz, "%s/%s.c", out_dir, ecd_mod_names[i]);
    }

    /* J7 Pass 1: compile all with no forced specs; collect borrow specs. */
    BorrowSpecInfo **ecd_borrow = (BorrowSpecInfo **)calloc(n_inputs, sizeof(BorrowSpecInfo *));
    uint32_t *ecd_n_borrow = (uint32_t *)calloc(n_inputs, sizeof(uint32_t));
    if (!ecd_borrow || !ecd_n_borrow) { fprintf(stderr, "tur: oom\n"); return 2; }

    int rc = 0;
    for (int i = 0; i < n_inputs && rc == 0; i++) {
        int rm_n = 0;
        char **rm_p = discover_manifest_reader_macros(inputs[i], &rm_n);
        Buf h_buf, c_buf;
        buf_init(&h_buf); buf_init(&c_buf);
        int h_rc = compile_to_h(inputs[i], &h_buf, ecd_mod_names[i],
                                include_dirs, n_include_dirs,
                                (const char **)rm_p, rm_n, NULL, 0);
        int c_rc = compile_to_implementation(inputs[i], &c_buf, ecd_mod_names[i],
                                             include_dirs, n_include_dirs,
                                             (const char **)rm_p, rm_n, NULL,
                                             NULL, 0,
                                             &ecd_borrow[i], &ecd_n_borrow[i]);
        free_reader_macro_paths(rm_p, rm_n);
        if (h_rc != 0 || c_rc != 0) {
            rc = h_rc != 0 ? h_rc : c_rc;
        } else if (buf_to_path(&h_buf, ecd_h_paths[i]) != 0 ||
                   buf_to_path(&c_buf, ecd_c_paths[i]) != 0) {
            fprintf(stderr, "tur emit-c: failed to write output for '%s'\n", inputs[i]);
            rc = 2;
        }
        buf_free(&h_buf); buf_free(&c_buf);
    }

    /* J7 Pass 2: recompile owners with forced specs. */
    if (rc == 0) {
        ForcedAbiSpec **ecd_forced = (ForcedAbiSpec **)calloc(n_inputs, sizeof(ForcedAbiSpec *));
        uint32_t *ecd_n_forced = (uint32_t *)calloc(n_inputs, sizeof(uint32_t));
        uint32_t *ecd_cap_forced = (uint32_t *)calloc(n_inputs, sizeof(uint32_t));
        if (!ecd_forced || !ecd_n_forced || !ecd_cap_forced) { fprintf(stderr, "tur: oom\n"); rc = 2; }

        if (rc == 0) {
            for (int i = 0; i < n_inputs; i++) {
                for (uint32_t bi = 0; bi < ecd_n_borrow[i]; bi++) {
                    BorrowSpecInfo *bsi = &ecd_borrow[i][bi];
                    if (!bsi->owning_module || !bsi->fn_symbol) continue;
                    int owner = -1;
                    for (int j = 0; j < n_inputs; j++) {
                        if (strcmp(ecd_mod_names[j], bsi->owning_module) == 0) { owner = j; break; }
                    }
                    if (owner < 0) continue;
                    bool dup = false;
                    for (uint32_t fi = 0; fi < ecd_n_forced[owner]; fi++) {
                        if (strcmp(ecd_forced[owner][fi].clone_name, bsi->clone_name) == 0) { dup = true; break; }
                    }
                    if (dup) continue;
                    if (ecd_n_forced[owner] >= ecd_cap_forced[owner]) {
                        uint32_t nc = ecd_cap_forced[owner] ? ecd_cap_forced[owner] * 2 : 4;
                        ForcedAbiSpec *nf = (ForcedAbiSpec *)realloc(ecd_forced[owner], nc * sizeof(ForcedAbiSpec));
                        if (!nf) { fprintf(stderr, "tur: oom\n"); rc = 2; break; }
                        ecd_forced[owner] = nf; ecd_cap_forced[owner] = nc;
                    }
                    ForcedAbiSpec *fs = &ecd_forced[owner][ecd_n_forced[owner]++];
                    fs->clone_name = bsi->clone_name;
                    fs->fn_symbol  = bsi->fn_symbol;
                    fs->result_kind = bsi->result_kind;
                    fs->n_args = bsi->n_args;
                    for (uint8_t ai = 0; ai < bsi->n_args; ai++) fs->arg_kinds[ai] = bsi->arg_kinds[ai];
                }
            }
            for (int i = 0; i < n_inputs && rc == 0; i++) {
                if (ecd_n_forced[i] == 0) continue;
                int rm_n = 0;
                char **rm_p = discover_manifest_reader_macros(inputs[i], &rm_n);
                Buf h_buf, c_buf;
                buf_init(&h_buf); buf_init(&c_buf);
                int h_rc = compile_to_h(inputs[i], &h_buf, ecd_mod_names[i],
                                        include_dirs, n_include_dirs,
                                        (const char **)rm_p, rm_n,
                                        ecd_forced[i], ecd_n_forced[i]);
                int c_rc = compile_to_implementation(inputs[i], &c_buf, ecd_mod_names[i],
                                                     include_dirs, n_include_dirs,
                                                     (const char **)rm_p, rm_n, NULL,
                                                     ecd_forced[i], ecd_n_forced[i],
                                                     NULL, NULL);
                free_reader_macro_paths(rm_p, rm_n);
                if (h_rc != 0 || c_rc != 0) rc = h_rc != 0 ? h_rc : c_rc;
                else if (buf_to_path(&h_buf, ecd_h_paths[i]) != 0 ||
                         buf_to_path(&c_buf, ecd_c_paths[i]) != 0) rc = 2;
                buf_free(&h_buf); buf_free(&c_buf);
            }
        }
        for (int i = 0; i < n_inputs; i++) free(ecd_forced[i]);
        free(ecd_forced); free(ecd_n_forced); free(ecd_cap_forced);
    }

    /* J5: Write .tur-abi-cache/index for emit-c --output-dir.
     * J6: skipped entirely when the cache is disabled (--no-abi-cache /
     * TUR_NO_ABI_CACHE); the build is still correct without it. */
    if (rc == 0 && !g_no_abi_cache) {
        char cache_dir[4096];
        snprintf(cache_dir, sizeof(cache_dir), "%s/.tur-abi-cache", out_dir);
        struct stat st_cd;
        bool cache_dir_new = (stat(cache_dir, &st_cd) != 0);
        if (cache_dir_new) mkdir(cache_dir, 0755);
        char idx_tmp[4096];
        snprintf(idx_tmp, sizeof(idx_tmp), "%s/index.tmp", cache_dir);
        FILE *cf = fopen(idx_tmp, "w");
        if (cf) {
            for (int i = 0; i < n_inputs; i++) {
                for (uint32_t bi = 0; bi < ecd_n_borrow[i]; bi++) {
                    BorrowSpecInfo *bsi = &ecd_borrow[i][bi];
                    if (!bsi->owning_module || !bsi->fn_symbol) continue;
                    fprintf(cf, "%s\t%s\t%s\t%d\t%d",
                            bsi->clone_name, bsi->owning_module, bsi->fn_symbol,
                            (int)bsi->result_kind, (int)bsi->n_args);
                    for (uint8_t ai = 0; ai < bsi->n_args; ai++)
                        fprintf(cf, "\t%d", (int)bsi->arg_kinds[ai]);
                    fprintf(cf, "\n");
                }
            }
            fclose(cf);
            char idx_path[4096];
            snprintf(idx_path, sizeof(idx_path), "%s/index", cache_dir);
            rename(idx_tmp, idx_path);
        }
    }

    for (int i = 0; i < n_inputs; i++) {
        for (uint32_t bi = 0; bi < ecd_n_borrow[i]; bi++) {
            free(ecd_borrow[i][bi].clone_name);
            free(ecd_borrow[i][bi].owning_module);
            free(ecd_borrow[i][bi].fn_symbol);
        }
        free(ecd_borrow[i]);
        free(ecd_mod_names[i]); free(ecd_h_paths[i]); free(ecd_c_paths[i]);
    }
    free(ecd_borrow); free(ecd_n_borrow);
    free(ecd_mod_names); free(ecd_h_paths); free(ecd_c_paths);
    return rc;
}

/* Phase M3: emit the .h for a single module file to stdout. */
static int cmd_emit_h(const char *path,
                      const char **include_dirs, int n_include_dirs) {
    const char *base = drv_basename_of(path);
    size_t base_len = strlen(base);
    char mod_name[256];
    size_t n = drv_source_stem_len(base, base_len);
    if (n >= sizeof(mod_name)) n = sizeof(mod_name) - 1;
    memcpy(mod_name, base, n);
    mod_name[n] = '\0';

    Buf out;
    buf_init(&out);
    int n_rm = 0;
    char **rm = discover_manifest_reader_macros(path, &n_rm);
    int rc = compile_to_h(path, &out, mod_name, include_dirs, n_include_dirs,
                          (const char **)rm, n_rm, NULL, 0);
    free_reader_macro_paths(rm, n_rm);
    if (rc == 0) buf_to_file(&out, stdout);
    buf_free(&out);
    return rc;
}

/* Choose an output executable name from the input path: foo.tur -> foo.
 *
 * For inputs that resolve to "the current directory" (".", "./", "" after
 * basename), drv_basename_of() returns "." or "" -- which would yield artifact
 * names like "lib..so" downstream. Resolve to an absolute path via realpath()
 * so the cwd's real basename is used instead. */
void drv_default_output_name(const char *input, char *out, size_t cap) {
    const char *base = drv_basename_of(input);
    char resolved[PATH_MAX];
    /* True when base came from resolving a "current directory" input to its
     * real path: the basename is then a directory name, which has no
     * extension to strip (a "." in "my.project" is part of the name). */
    bool resolved_dir = false;
    if (base[0] == '\0' || (base[0] == '.' && base[1] == '\0')) {
        if (realpath(input && input[0] ? input : ".", resolved)) {
            base = drv_basename_of(resolved);
            if (base[0] == '\0') base = "root";  /* realpath("/") -> "/" */
            resolved_dir = true;
        }
    }
    size_t n = strlen(base);
    if (n >= cap) n = cap - 1;
    memcpy(out, base, n);
    out[n] = '\0';
    /* Only strip extension if this looks like a file (has a dot that's not at
     * the start) -- never for a resolved directory name. */
    if (!resolved_dir && n > 0 && out[n-1] != '/') {
        char *dot = strrchr(out, '.');
        if (dot && dot != out) { *dot = '\0'; }
    }
}

/* build-output-directory-plan: ensure_dir(path) creates a directory tree with
 * mkdir -p semantics.  Returns 0 on success (or if the directory already
 * exists), -1 with errno set on failure.  Walks `path` component-wise so a
 * fresh `<root>/build/obj` works even when only `<root>` exists. */
static int ensure_dir(const char *path) {
    if (!path || !*path) return -1;
    char tmp[4096];
    size_t n = strlen(path);
    if (n >= sizeof(tmp)) { errno = ENAMETOOLONG; return -1; }
    memcpy(tmp, path, n + 1);
    /* Strip trailing slashes (except a bare "/"). */
    while (n > 1 && tmp[n - 1] == '/') tmp[--n] = '\0';
    /* Skip leading slash (or "./") so the loop starts on the first real component. */
    char *p = tmp;
    if (*p == '/') p++;
    while (*p) {
        while (*p && *p != '/') p++;
        if (!*p) break;
        *p = '\0';
        if (mkdir(tmp, 0755) != 0 && errno != EEXIST) return -1;
        *p++ = '/';
    }
    if (mkdir(tmp, 0755) != 0 && errno != EEXIST) return -1;
    return 0;
}

/* build-output-directory-plan: resolve the absolute build directory for a
 * compile.  Precedence (highest first):
 *   1. cli_flag (--build-dir / -B / TUR's emit-c --build-dir).
 *   2. TUR_BUILD_DIR env var.
 *   3. The nearest `build.tur`'s `:build-dir` (walking up from input_or_root),
 *      resolved relative to that manifest's directory.
 *   4. `<project-root>/build` when a manifest exists; otherwise `<cwd>/build`.
 *
 * On success returns a heap-allocated absolute path with the directory plus
 * `obj/`, `bin/`, `lib/` subdirs created, and a `.gitignore` containing
 * `*\n` dropped in on first creation.  Returns NULL with a diagnostic on
 * failure.  Caller frees with free(). */
char *drv_resolve_build_dir(const char *input_or_root, const char *cli_flag) {
    /* Step 1: pick the raw (unresolved) path string. */
    char raw[4096] = {0};
    char manifest_dir[4096] = {0};

    if (cli_flag && *cli_flag) {
        snprintf(raw, sizeof(raw), "%s", cli_flag);
    } else if (getenv("TUR_BUILD_DIR") && *getenv("TUR_BUILD_DIR")) {
        snprintf(raw, sizeof(raw), "%s", getenv("TUR_BUILD_DIR"));
    } else {
        const char *start = (input_or_root && *input_or_root) ? input_or_root : ".";
        char start_dir[4096];
        struct stat st;
        if (stat(start, &st) == 0 && S_ISDIR(st.st_mode)) {
            snprintf(start_dir, sizeof(start_dir), "%s", start);
        } else {
            drv_dir_of_path(start, start_dir, sizeof(start_dir));
        }
        char *root = drv_find_project_root(start_dir);
        if (root) {
            snprintf(manifest_dir, sizeof(manifest_dir), "%s", root);
            char mp[4096];
            (void)pkg_resolve_manifest_path(root, mp, sizeof(mp));
            PkgManifest m;
            memset(&m, 0, sizeof(m));
            if (pkg_manifest_read(mp, &m) && m.build_dir && *m.build_dir) {
                if (m.build_dir[0] == '/')
                    snprintf(raw, sizeof(raw), "%s", m.build_dir);
                else
                    snprintf(raw, sizeof(raw), "%s/%s", root, m.build_dir);
            } else {
                snprintf(raw, sizeof(raw), "%s/build", root);
            }
            pkg_manifest_free(&m);
            free(root);
        } else {
            snprintf(raw, sizeof(raw), "./build");
        }
    }

    /* Step 2: ensure the directory exists, then canonicalize. */
    if (ensure_dir(raw) != 0) {
        fprintf(stderr, "tur: cannot create build directory '%s': %s\n",
                raw, strerror(errno));
        return NULL;
    }

    char abs[PATH_MAX];
    if (!realpath(raw, abs)) {
        fprintf(stderr, "tur: cannot resolve build directory '%s': %s\n",
                raw, strerror(errno));
        return NULL;
    }

    /* Create obj/, bin/, lib/ subdirs. */
    char sub[PATH_MAX + 16];
    const char *subs[] = {"obj", "bin", "lib"};
    for (size_t i = 0; i < sizeof(subs) / sizeof(*subs); i++) {
        snprintf(sub, sizeof(sub), "%s/%s", abs, subs[i]);
        if (mkdir(sub, 0755) != 0 && errno != EEXIST) {
            fprintf(stderr, "tur: cannot create '%s': %s\n", sub, strerror(errno));
            return NULL;
        }
    }

    /* Drop a `.gitignore` containing `*` on first creation so the build dir
     * is never accidentally committed even if it gets tracked. Skip if one
     * already exists -- the user may have customized it. */
    char gi[PATH_MAX + 16];
    snprintf(gi, sizeof(gi), "%s/.gitignore", abs);
    struct stat gst;
    if (stat(gi, &gst) != 0) {
        FILE *gf = fopen(gi, "w");
        if (gf) { fprintf(gf, "*\n"); fclose(gf); }
    }

    (void)manifest_dir;  /* reserved for future workspace-member resolution */

    /* Return heap copy. */
    size_t n = strlen(abs);
    char *res = (char *)malloc(n + 1);
    if (!res) return NULL;
    memcpy(res, abs, n + 1);
    return res;
}

/* -------------------------------------------------------------------------
 * Entry points: the command table in cli.c runs these for `tur <command>`.
 * ---------------------------------------------------------------------- */

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

int cmd_emit_c_main(int argc, char **argv) {
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

int cmd_emit_h_main(int argc, char **argv) {
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

int cmd_check_main(int argc, char **argv) {
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

int cmd_expand_main(int argc, char **argv) {
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

int cmd_audit_spans_main(int argc, char **argv) {
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
