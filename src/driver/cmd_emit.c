/* cmd_emit.c -- `tur emit-c` and `tur emit-h`, and the build-directory and
 * output-name resolution the build commands share.
 *
 * Part of the compiler driver (src/driver/); driver.h says how the files
 * share declarations. */

#include "driver.h"
#include "commands.h"

int cmd_emit_c(const char *path,
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
int cmd_emit_c_to_dir(const char *out_dir, char **inputs, int n_inputs,
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
int cmd_emit_h(const char *path,
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
