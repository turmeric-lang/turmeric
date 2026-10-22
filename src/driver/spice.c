/* spice.c -- the enclosing spice: finding the project and spice roots, the
 * manifest's reader macros and experiments (and the user experiments file),
 * the include path the `:spices` deps contribute, collecting a tree's .tur
 * files, and engine selection.
 *
 * Part of the compiler driver (src/driver/); driver.h says how the files
 * share declarations. */

#include "driver.h"

#include "global.h"   /* tur_installed_spice_dir */
#include "spice_search.h"  /* find_spice_root / auto_append_spice_includes */

/* RM4: collect the manifest's `:reader-macros [...]` entries as absolute
 * paths suitable for compile_to_c. `root_dir` is the spice root (where
 * build.tur lives); each manifest entry is joined onto it unless it is
 * already absolute. Returns a heap-allocated `char **` (each element also
 * heap-allocated) and writes the count to `*n_out`. Caller frees with
 * `free_reader_macro_paths`. Returns NULL with `*n_out = 0` if the
 * manifest has no entries. */
char **resolve_manifest_reader_macros(const char *root_dir,
                                      const PkgManifest *m,
                                      int *n_out) {
    *n_out = 0;
    if (!m || m->n_reader_macros == 0) return NULL;
    char **out = (char **)malloc(sizeof(char *) * (size_t)m->n_reader_macros);
    if (!out) return NULL;
    for (int i = 0; i < m->n_reader_macros; ++i) {
        const char *rel = m->reader_macros[i];
        char buf[4096];
        if (rel[0] == '/') {
            snprintf(buf, sizeof(buf), "%s", rel);
        } else {
            snprintf(buf, sizeof(buf), "%s/%s", root_dir, rel);
        }
        out[i] = (char *)malloc(strlen(buf) + 1);
        if (!out[i]) {
            for (int j = 0; j < i; ++j) free(out[j]);
            free(out);
            return NULL;
        }
        strcpy(out[i], buf);
    }
    *n_out = m->n_reader_macros;
    return out;
}

void free_reader_macro_paths(char **paths, int n) {
    if (!paths) return;
    for (int i = 0; i < n; ++i) free(paths[i]);
    free(paths);
}

/* Walk up from 'start' to find a directory containing build.tur.
 * Returns malloc'd path string or NULL if not found. Caller must free(). */
char *drv_find_project_root(const char *start) {
    char dir[4096];
    strncpy(dir, start, sizeof(dir) - 1);
    dir[sizeof(dir) - 1] = '\0';
    for (;;) {
        char candidate[4096];
        if (pkg_resolve_manifest_path(dir, candidate, sizeof(candidate))) {
            char *res = (char *)malloc(strlen(dir) + 1);
            if (res) strcpy(res, dir);
            return res;
        }
        char *slash = drv_last_path_sep(dir);
        if (!slash || slash == dir) break;
        *slash = '\0';
    }
    return NULL;
}

/* SC3: walk up from `file_path`'s directory to the enclosing `build.tur`.
 * Lives in tur_core (src/compiler/spice_search.c) so that libturi embedders
 * share it; see spice_find_root for the contract. */
static char *find_spice_root(const char *file_path) {
    return spice_find_root(file_path);
}

/* XF1 (experimental-flag-mechanism-plan): merge an enclosing spice's
 * :experiments [...] into the active experiment set.  CLI --enable= has
 * already run (and wins on conflict).  An unknown name in the manifest is a
 * hard configuration error: emit TUR-E0310 and abort, the same "typos surface
 * immediately" contract the CLI path enforces. */
void apply_manifest_experiments(const PkgManifest *m) {
    for (int i = 0; i < m->n_experiments; i++) {
        if (!experiment_enable(m->experiments[i], XF_SRC_MANIFEST)) {
            fprintf(stderr,
                    "error [TUR-E0310]: unknown experiment '%s' in build.tur "
                    ":experiments; run 'tur experiments' for the list\n",
                    m->experiments[i]);
            exit(2);
        }
    }
}

/* UC-3 (user-config-experiments-plan): whether the user-level experiments
 * file has already been consulted this process (or deliberately bypassed, as
 * the LSP hot path does).  The read happens at most once; suppression by a
 * project manifest is decided on that first consult. */
bool g_user_config_experiments_done = false;

/* UC-3 (user-config-experiments-plan): consult the user-level experiments
 * file ($XDG_CONFIG_HOME/turmeric/experiments.tur) at most once per process.
 * Suppressed entirely when the current compile runs inside a manifested
 * project that declared its own :experiments list -- even the empty
 * `:experiments []` (Goal 2): the project owner's stated intent governs, and
 * user preferences do not silently union in.  `m_or_null` is the resolved
 * project manifest, or NULL for a scratch file / non-project invocation.
 * CLI --enable= (applied earlier at XF_SRC_CLI) still wins over whatever this
 * turns on, because a higher-numbered source always beats a lower one. */
void apply_user_config_experiments(const PkgManifest *m_or_null) {
    if (g_user_config_experiments_done) return;
    g_user_config_experiments_done = true;
    if (m_or_null && m_or_null->has_experiments_key) return;  /* suppressed */
    experiments_read_user_config();
}

/* Find the spice enclosing `input_path`, read its manifest and apply the
 * experiments that go with it: the user-level file (suppressed if the manifest
 * declares :experiments) and the manifest's own list.  CLI --enable= ran
 * earlier and wins.  Returns true with `*m` filled and `*root_out` set (the
 * caller frees both); false when there is no readable manifest, in which case
 * the user-level file has been applied and nothing is owned.
 *
 * Every command that compiles or evaluates a file reaches the project's
 * experiments through here (directly, or via discover_manifest_reader_macros),
 * so a project's `:experiments` means the same thing to all of them. */
static bool load_manifest_apply_experiments(const char *input_path,
                                            char **root_out,
                                            PkgManifest *m) {
    *root_out = NULL;
    memset(m, 0, sizeof(*m));
    if (!input_path) return false;
    char *sroot = find_spice_root(input_path);
    if (!sroot) {
        /* No enclosing project: the user-level experiments file applies
         * unconditionally (nothing to suppress it). */
        apply_user_config_experiments(NULL);
        return false;
    }
    char mp[4096];
    if (!pkg_resolve_manifest_path(sroot, mp, sizeof(mp))) {
        apply_user_config_experiments(NULL);
        free(sroot);
        return false;
    }
    if (!pkg_manifest_read(mp, m)) {
        /* Manifest present but unreadable -- treat as no project key. */
        apply_user_config_experiments(NULL);
        pkg_manifest_free(m);
        free(sroot);
        return false;
    }
    /* User file first (suppressed if the manifest declares :experiments),
     * then the manifest's own list, then -- earlier -- CLI --enable=.
     * Precedence is by source rank, so the read order does not matter for
     * the source column; we read user-config first only to skip it when
     * the manifest carries the key. */
    apply_user_config_experiments(m);
    apply_manifest_experiments(m);
    *root_out = sroot;
    return true;
}

/* The experiments half on its own, for a command that has no use for the
 * manifest's reader macros (the interpreter file-eval arms). */
void apply_enclosing_manifest_experiments(const char *input_path) {
    char *sroot = NULL;
    PkgManifest m;
    if (load_manifest_apply_experiments(input_path, &sroot, &m)) {
        pkg_manifest_free(&m);
        free(sroot);
    }
}

/* RM4 follow-up: convenience wrapper around find_spice_root +
 * pkg_manifest_read + resolve_manifest_reader_macros. Walks up from
 * `input_path` to find an enclosing build.tur, then returns the resolved
 * absolute reader-macro paths. Returns NULL/0 when there is no manifest
 * or no entries. Caller frees with free_reader_macro_paths. Used by
 * every compile entry point that takes a single input file path. */
char **discover_manifest_reader_macros(const char *input_path,
                                       int *n_out) {
    *n_out = 0;
    char *sroot = NULL;
    PkgManifest m;
    if (!load_manifest_apply_experiments(input_path, &sroot, &m)) return NULL;
    char **out = resolve_manifest_reader_macros(sroot, &m, n_out);
    pkg_manifest_free(&m);
    free(sroot);
    return out;
}

/* SC4+SC5: auto-discover the enclosing spice and append both its own
 * `src/` (SC4) and every `:spices` dep's `src/` (SC5) to the include
 * list.  Each appended path is strdup'd; the returned `*owned` array
 * (size in `*n_owned`) is the strdup'd pointers in insertion order so
 * the caller can free them after the compile call.
 *
 * Order matters: explicit `-I` flags from the user are already at the
 * front of `*inc`, so the elaborator's first-match-wins behavior keeps
 * them as the highest priority.  Within the auto-appended block, the
 * enclosing spice's own src/ comes before fetched-dep src/ dirs (so a
 * local module shadows a vendored one of the same name).
 *
 * Skipped (returns 0 with no entries) when:
 *   - g_no_auto_spice is set, OR
 *   - no build.tur is found within TUR_SPICE_WALK_MAX ancestors, OR
 *   - both checks (own src/, manifest deps) yield nothing.
 *
 * The caller must own/free the returned pointer array even when empty:
 *
 *     char **owned = NULL;
 *     int    n_owned = 0;
 *     auto_append_spice_includes(input, &inc, &n_inc, &owned, &n_owned);
 *     // ...compile...
 *     for (int i = 0; i < n_owned; i++) free(owned[i]);
 *     free(owned);
 */
/* LS2: grow the producer-per-include parallel array by one entry, copying
 * `producer` (may be NULL) onto the end. Length is tracked via *n_inc by
 * the caller; this is called immediately after a successful
 * append_inc_owned to keep the arrays aligned. */
static int ls2_push_producer(Ls2ResolverCtx *ls2, int new_n_inc,
                             const char *producer) {
    if (!ls2) return 0;
    const char **bigger = (const char **)realloc(
        (void *)ls2->producer_per_inc, (size_t)new_n_inc * sizeof(char *));
    if (!bigger) return -1;
    ls2->producer_per_inc = bigger;
    /* The caller passes new_n_inc = old_n_inc + 1, so the new slot is at
     * new_n_inc - 1.  Any pre-existing slots stay as they were. */
    char *copy = producer ? strdup(producer) : NULL;
    if (producer && !copy) return -1;
    bigger[new_n_inc - 1] = copy;
    return 0;
}

/* LS2: when the LS2 context is active, initialize the leading slots of
 * producer_per_inc to NULL so the per-include indices line up.  Called
 * once at the top of auto_append_spice_includes with the count of -I
 * entries the caller already prepended. */
static int ls2_prime_producer_array(Ls2ResolverCtx *ls2, int n_existing) {
    if (!ls2 || n_existing <= 0) return 0;
    const char **arr = (const char **)calloc((size_t)n_existing, sizeof(char *));
    if (!arr) return -1;
    ls2->producer_per_inc = arr;
    return 0;
}

/* LS2: the SpiceSearchHooks auto_append_spice_includes hands the walk.
 * `ud` is the caller's Ls2ResolverCtx, or NULL when it keeps none. */
static void ls2_on_search_add(void *ud, int n_inc, const char *dir,
                              const char *producer) {
    Ls2ResolverCtx *ls2 = (Ls2ResolverCtx *)ud;
    (void)ls2_push_producer(ls2, n_inc, producer);
    if (producer && ls2 && ls2->debug_resolver) {
        fprintf(stderr,
                "tur: resolver: added workspace sibling '%s' src/ -> %s\n",
                producer, dir);
    }
}

/* LS2: snapshot the consumer's declared :spices names so the elaborator can
 * suppress the warning when a workspace sibling is already redeclared via
 * :spices. */
static void ls2_on_search_manifest(void *ud, const PkgManifest *m) {
    Ls2ResolverCtx *ls2 = (Ls2ResolverCtx *)ud;
    if (!ls2 || m->n_spices <= 0) return;
    ls2->declared_spices = (const char **)calloc((size_t)m->n_spices, sizeof(char *));
    if (!ls2->declared_spices) return;
    for (int i = 0; i < m->n_spices; i++)
        ls2->declared_spices[i] = strdup(m->spices[i].name);
    ls2->n_declared_spices = m->n_spices;
}

/* SC4+SC5+SC6: widen the include path with the enclosing spice's src/ and
 * every declared `:spices` dep's src/.  Used from tur_check_only (called by
 * the LSP server) and the per-file dispatchers.
 *
 * LS2: when out_ls2 is non-NULL, the helper also populates a parallel
 * provenance array (workspace-sibling producer path per include dir),
 * the consumer's declared :spices map keys, and the TUR_DEBUG_RESOLVER
 * flag. Caller is responsible for calling ls2_resolver_ctx_dispose
 * after compile_to_c returns. Pass NULL to skip the LS2 bookkeeping. */
int auto_append_spice_includes(const char *input,
                               char ***inc, int *n_inc,
                               char ***owned, int *n_owned,
                               Ls2ResolverCtx *out_ls2) {
    *owned = NULL;
    *n_owned = 0;
    if (out_ls2) {
        memset(out_ls2, 0, sizeof(*out_ls2));
        const char *dbg = getenv("TUR_DEBUG_RESOLVER");
        out_ls2->debug_resolver = (dbg && dbg[0] == '1');
        /* Pre-existing entries are user-supplied -I flags; mark them with
         * NULL producer so the parallel array aligns with the final
         * include list. */
        if (ls2_prime_producer_array(out_ls2, *n_inc) != 0) return -1;
    }
    if (g_no_auto_spice || !input) return 0;

    /* script-spice-deps-plan SD-D7: whether a script header is honored
     * depends on --enable=script-deps, which can come from the user
     * experiments file -- read further down every command's path, after this
     * walk.  A file outside any project has nothing to suppress that file, so
     * apply it now (once per process). */
    if (!g_user_config_experiments_done) {
        char *proot = find_spice_root(input);
        if (!proot) apply_user_config_experiments(NULL);
        free(proot);
    }

    /* SC4 + SC5 + LS2: the walk itself is spice_search_append
     * (src/compiler/spice_search.c), shared with libturi embedders; the LS2
     * bookkeeping rides on its hooks. */
    SpiceSearchHooks hooks = {
        .on_add      = ls2_on_search_add,
        .on_manifest = ls2_on_search_manifest,
        .ud          = out_ls2,
    };
    int src = spice_search_append(input, inc, n_inc, owned, n_owned, &hooks);
    /* A script header that cannot be honored (TUR-E063x, already reported)
     * stops the command: compiling on would only add a "module not found"
     * for every import the header was there to satisfy. */
    if (src == SPICE_SEARCH_SCRIPT_ERROR) exit(1);

    /* LS2: finalize ctx — set length to current *n_inc and allocate the
     * per-include warned[] dedup array so the elaborator can mark slots
     * after firing the first warning. */
    if (out_ls2) {
        out_ls2->n_inc = *n_inc;
        if (*n_inc > 0) {
            out_ls2->warned_per_inc =
                (bool *)calloc((size_t)*n_inc, sizeof(bool));
            if (!out_ls2->warned_per_inc) return -1;
        }
    }
    return 0;
}

/* LS2: free the parallel arrays owned by an Ls2ResolverCtx populated by
 * auto_append_spice_includes. Idempotent; safe to call on a zero-init
 * struct. */
void ls2_resolver_ctx_dispose(Ls2ResolverCtx *ctx) {
    if (!ctx) return;
    if (ctx->producer_per_inc) {
        for (int i = 0; i < ctx->n_inc; i++) {
            if (ctx->producer_per_inc[i])
                free((void *)ctx->producer_per_inc[i]);
        }
        free((void *)ctx->producer_per_inc);
    }
    free(ctx->warned_per_inc);
    if (ctx->declared_spices) {
        for (int i = 0; i < ctx->n_declared_spices; i++) {
            if (ctx->declared_spices[i])
                free((void *)ctx->declared_spices[i]);
        }
        free((void *)ctx->declared_spices);
    }
    memset(ctx, 0, sizeof(*ctx));
}

/* Collect all .tur files in a directory. Returns malloc'd array, sets *n_out. */
char **collect_tur_files(const char *dir, int *n_out) {
    DIR *d = opendir(dir);
    if (!d) return NULL;

    char **files = NULL;
    int cap = 0;
    int n = 0;

    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (!tur_dirent_is_reg(dir, ent)) continue;
        size_t len = strlen(ent->d_name);
        if (len >= 4 && strcmp(ent->d_name + len - 4, ".tur") == 0) {
            if (n >= cap) {
                cap = cap ? cap * 2 : 8;
                files = (char **)realloc(files, cap * sizeof(char *));
            }
            files[n] = (char *)malloc(strlen(dir) + 1 + len + 1);
            snprintf(files[n], strlen(dir) + 1 + len + 1, "%s/%s", dir, ent->d_name);
            n++;
        }
    }
    closedir(d);
    *n_out = n;
    return files;
}

/* Free the array returned by collect_tur_files. */
void free_tur_files(char **files, int n) {
    for (int i = 0; i < n; i++) free(files[i]);
    free(files);
}

int drv_compare_cstr_ptrs(const void *a, const void *b) {
    const char *const *sa = (const char *const *)a;
    const char *const *sb = (const char *const *)b;
    return strcmp(*sa, *sb);
}

/* Recursively collect `*.tur` files under `dir` into a growing array.
 * Skips the package manifest (`build.tur`) and any dotfile entry (so
 * `.git/`, `.tur-cache/`, `.tur-abi-cache/`, etc. are not descended into).
 * Paths are heap-allocated; the array is freed via free_tur_files. */
void collect_tur_recursive(const char *dir,
                           char ***files, int *n, int *cap) {
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (ent->d_name[0] == '.') continue; /* ., .., and dot subtrees */
        char path[4096];
        snprintf(path, sizeof(path), "%s/%s", dir, ent->d_name);
        struct stat st;
        if (stat(path, &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) {
            collect_tur_recursive(path, files, n, cap);
        } else if (S_ISREG(st.st_mode)) {
            size_t len = strlen(ent->d_name);
            if (len < 4 || strcmp(ent->d_name + len - 4, ".tur") != 0) continue;
            if (pkg_is_manifest_name(ent->d_name)) continue; /* manifest */
            if (*n >= *cap) {
                *cap = *cap ? *cap * 2 : 8;
                *files = (char **)realloc(*files, (size_t)*cap * sizeof(char *));
            }
            (*files)[(*n)++] = strdup(path);
        }
    }
    closedir(d);
}

/* Collect every `.tur` file under `dir`, recursively.
 *
 * `tur build <dir>`, `tur check <dir>`, and `tur test <dir>` used the FLAT
 * collect_tur_files, which sees only files sitting directly in `dir`.  A spice
 * whose modules live one level down -- `src/demo/lib.tur`, the layout
 * `:exports "demo/lib"` implies and the guides use -- therefore reported
 * `tur: no .tur files found in 'src/'`, on the invocation the `module not
 * found` diagnostic itself recommends.  Project mode already recursed
 * (collect_project_src_files), so the two spellings of "build this spice"
 * disagreed, with nothing in the output to say which one you got.
 *
 * Same shape as free_tur_files' input, so the caller frees it unchanged.
 * See docs/archive/tur-build-nested-src-dir-finds-no-files.md. */
char **collect_tur_files_deep(const char *dir, int *n_out) {
    char **files = NULL;
    int n = 0, cap = 0;
    collect_tur_recursive(dir, &files, &n, &cap);
    *n_out = n;
    return files;
}

/* Collect a spice/library project's module files for a directory build.
 * Prefers a recursive walk of `<root>/src` (the conventional layout used by
 * every first-party spice, including nested `src/<pkg>/` trees); when there
 * is no `src/`, falls back to a shallow scan of `root` with the manifest
 * filtered out.  Heap array; free via free_tur_files. */
char **collect_project_src_files(const char *root, int *n_out) {
    *n_out = 0;
    char **files = NULL;
    int n = 0, cap = 0;

    char src_dir[4096];
    snprintf(src_dir, sizeof(src_dir), "%s/src", root);
    struct stat st;
    if (stat(src_dir, &st) == 0 && S_ISDIR(st.st_mode)) {
        collect_tur_recursive(src_dir, &files, &n, &cap);
    } else {
        /* No src/ tree: shallow scan of the root, skipping build.tur. */
        int rn = 0;
        char **raw = collect_tur_files(root, &rn);
        for (int i = 0; i < rn; i++) {
            if (pkg_is_manifest_name(drv_basename_of(raw[i]))) {
                free(raw[i]);
                continue;
            }
            if (n >= cap) {
                cap = cap ? cap * 2 : 8;
                files = (char **)realloc(files, (size_t)cap * sizeof(char *));
            }
            files[n++] = raw[i];
        }
        free(raw);
    }
    *n_out = n;
    return files;
}

/* spices-c-sources-plan: collect a project's vendored C build inputs into two
 * command-line fragments (caller inits/frees the Bufs):
 *   - `includes`: -I flags. The project's OWN :c-includes (private to the
 *     spice) plus, for each :spices dep, that dep's :c-includes -- the latter
 *     are required so the dep's vendored .c can find its own headers when
 *     compiled in the same cc invocation. Dep includes are an implementation
 *     detail; consumers should not rely on them being visible to their .tur
 *     inline-C, even though source-level aggregation makes them so here.
 *   - `sources`: absolute paths to the project's own :c-sources AND each
 *     :spices dep's :c-sources, so a consumer of a vendor spice links the
 *     vendor's hand-written C into the final binary.
 * g_no_auto_spice disables manifest discovery entirely (matching every other
 * auto-spice convenience). */
/* c-sources-propagate-only-one-level: gather every :spices dep's vendored
 * `:c-sources` / `:c-includes` across the WHOLE closure, the way
 * pkg_collect_transitive_cmake_deps already does for :cmake-deps -- a `.c`
 * declared two spices away used to be dropped, and the failure was an
 * undefined symbol in the consumer's link with nothing pointing at the
 * manifest that owns it.  Worklist + visited set (realpath-keyed, so a
 * diamond terminates), and every source is emitted ONCE by resolved path:
 * the same spice reached by two routes, or two deps vendoring the same
 * third-party file, must not turn a missing-symbol failure into a
 * duplicate-symbol one. */
static bool aux_c_seen(char ***seen, int *n, int *cap, const char *key) {
    for (int i = 0; i < *n; i++) if (strcmp((*seen)[i], key) == 0) return true;
    if (*n >= *cap) {
        int nc = *cap ? *cap * 2 : 8;
        char **ns = (char **)realloc(*seen, (size_t)nc * sizeof(char *));
        if (!ns) return true;   /* oom: treat as seen (drop) */
        *seen = ns; *cap = nc;
    }
    (*seen)[(*n)++] = strdup(key);
    return false;
}
void collect_spice_aux_c(const char *root, Buf *includes, Buf *sources) {
    if (!root) return;
    collect_spice_aux_c_roots(&root, 1, includes, sources);
}

/* installed-spice-libraries D7: the same closure walk seeded with several
 * roots at once -- a project's own root plus the installed spices a program
 * imported -- so a dep two of them share, or the project and an installed
 * spice both vendor, is compiled ONCE.  Each work item remembers the root it
 * was reached from, which is the project whose `tur fetch` put the fetched
 * deps in its spices/ (pkg_resolve_spice_dep_dir_in). */
void collect_spice_aux_c_roots(const char *const *roots, int n_roots,
                               Buf *includes, Buf *sources) {
    if (g_no_auto_spice || !roots || n_roots <= 0) return;
    char **work = NULL; const char **work_top = NULL; int n_work = 0, cap_work = 0;
    char **visited = NULL; int n_vis = 0, cap_vis = 0;
    char **src_seen = NULL; int n_src = 0, cap_src = 0;
    char **inc_seen = NULL; int n_inc = 0, cap_inc = 0;
    #define AUX_PUSH(d, top) do { if (n_work >= cap_work) { int nc = cap_work ? cap_work * 2 : 8; \
        char **nw = (char **)realloc(work, (size_t)nc * sizeof(char *)); \
        if (!nw) { free(d); break; } work = nw; \
        const char **nt = (const char **)realloc(work_top, (size_t)nc * sizeof(char *)); \
        if (!nt) { free(d); break; } work_top = nt; cap_work = nc; } \
        work_top[n_work] = (top); work[n_work++] = (d); } while (0)
    /* A stack: push in reverse so the first root is walked first. */
    for (int r = n_roots - 1; r >= 0; r--)
        if (roots[r]) AUX_PUSH(strdup(roots[r]), roots[r]);
    while (n_work > 0) {
        --n_work;
        char *dir = work[n_work];
        const char *top = work_top[n_work];
        char canon[4096];
        const char *key = realpath(dir, canon) ? canon : dir;
        if (aux_c_seen(&visited, &n_vis, &cap_vis, key)) { free(dir); continue; }
        char mp[4096];
        if (!pkg_resolve_manifest_path(dir, mp, sizeof(mp))) { free(dir); continue; }
        PkgManifest m; memset(&m, 0, sizeof(m));
        if (!pkg_manifest_read(mp, &m)) { free(dir); continue; }
        for (int i = 0; i < m.n_c_includes; i++) {
            char path[4096];
            snprintf(path, sizeof(path), "%s/%s", dir, m.c_includes[i]);
            char ic[4096];
            const char *ikey = realpath(path, ic) ? ic : path;
            if (aux_c_seen(&inc_seen, &n_inc, &cap_inc, ikey)) continue;
            /* D-1: `path` is <spice dir>/<manifest's :c-includes entry>.  The
             * entry is grammar-checked at manifest-parse time; the spice dir
             * is the user's own path and only needs quoting. */
            buf_puts(includes, " -I");
            if (!drv_buf_put_quoted(includes, path))
                fprintf(stderr, "tur: skipping :c-includes path (too long to "
                                "quote): %s\n", path);
        }
        for (int i = 0; i < m.n_c_sources; i++) {
            char path[4096];
            snprintf(path, sizeof(path), "%s/%s", dir, m.c_sources[i]);
            char sc[4096];
            const char *skey = realpath(path, sc) ? sc : path;
            if (aux_c_seen(&src_seen, &n_src, &cap_src, skey)) continue;
            buf_putc(sources, ' ');
            if (!drv_buf_put_quoted(sources, path))
                fprintf(stderr, "tur: skipping :c-sources path (too long to "
                                "quote): %s\n", path);
        }
        /* Enqueue this manifest's own :spices for the next level.  The
         * resolver is the one the :cmake-deps walk uses, so both closures
         * agree on what a dependency is (workspace sibling, :path, fetched,
         * :global); a dep that is not on disk is skipped silently, exactly
         * like a missing fetched dep before. */
        for (int i = 0; i < m.n_spices; i++) {
            char *dep_dir = pkg_resolve_spice_dep_dir_in(dir, top, &m.spices[i]);
            if (!dep_dir) continue;
            AUX_PUSH(dep_dir, top);
        }
        pkg_manifest_free(&m);
        free(dir);
    }
    #undef AUX_PUSH
    for (int i = 0; i < n_work; i++) free(work[i]);
    free(work);
    free((void *)work_top);
    for (int i = 0; i < n_vis; i++) free(visited[i]);
    free(visited);
    for (int i = 0; i < n_src; i++) free(src_seen[i]);
    free(src_seen);
    for (int i = 0; i < n_inc; i++) free(inc_seen[i]);
    free(inc_seen);
}

/* Resolve a project's include search path by walking up from `dir` to find
 * the enclosing build.tur, then delegating to
 * resolve_include_dirs_from_manifest with the project's own `src/` included.
 * No-op (leaves *out_n = 0) when there is no manifest.  Used by `tur test`
 * and `tur build <dir>`. */
void resolve_project_include_dirs(const char *dir,
                                  const char ***out_dirs, int *out_n) {
    *out_dirs = NULL;
    *out_n    = 0;

    char abs_dir[4096];
    if (!realpath(dir, abs_dir)) {
        strncpy(abs_dir, dir, sizeof(abs_dir) - 1);
        abs_dir[sizeof(abs_dir) - 1] = '\0';
    }
    char *proj_root = drv_find_project_root(abs_dir);
    if (!proj_root) return;

    char manifest_path[4096];
    if (!pkg_resolve_manifest_path(proj_root, manifest_path,
                                   sizeof(manifest_path))) {
        free(proj_root);
        return;
    }
    PkgManifest m;
    if (!pkg_manifest_read(manifest_path, &m)) {
        free(proj_root);
        return;
    }

    resolve_include_dirs_from_manifest(proj_root, &m, /*include_own_src=*/true,
                                       out_dirs, out_n);

    pkg_manifest_free(&m);
    free(proj_root);
}

/* engine-selection-plan E1: resolve the execution engine for `tur run`.
 * Precedence (highest first), mirroring drv_resolve_build_dir's ladder:
 *   1. --engine <name> on the command line.
 *   2. TUR_ENGINE env var.
 *   3. The nearest `build.tur`'s `:engine` (walking up from input_or_root).
 *   4. "cc" -- compile via the C emitter and run the binary (the reference).
 *
 * Returns a static string ("cc" | "jit" | "interp"), or NULL after printing
 * TUR-E0311 for an unknown CLI/env value.  A manifest value is validated at
 * parse time (pkg.c, same code), so an invalid manifest never reaches here.
 * The engines differ in SEMANTICS, not just speed (#?(:tur ... :turi ...),
 * inline-C carve-outs, c2mir divergences), which is why an unknown value is
 * a hard error and never a fallback. */
const char *drv_resolve_engine(const char *input_or_root,
                               const char *cli_flag) {
    const char *cand = NULL;
    const char *from = NULL;
    if (cli_flag && *cli_flag) {
        cand = cli_flag;
        from = "--engine";
    } else if (getenv("TUR_ENGINE") && *getenv("TUR_ENGINE")) {
        cand = getenv("TUR_ENGINE");
        from = "TUR_ENGINE";
    }
    if (!cand) {
        const char *start = (input_or_root && *input_or_root) ? input_or_root
                                                              : ".";
        /* find_spice_root handles both file and directory inputs, relative
         * or absolute -- the same walker the RM4 manifest discovery uses. */
        char *root = find_spice_root(start);
        if (root) {
            char mp[4096];
            (void)pkg_resolve_manifest_path(root, mp, sizeof(mp));
            PkgManifest m;
            memset(&m, 0, sizeof(m));
            const char *resolved = NULL;
            if (pkg_manifest_read(mp, &m) && m.engine && *m.engine) {
                if (strcmp(m.engine, "jit") == 0)         resolved = "jit";
                else if (strcmp(m.engine, "interp") == 0) resolved = "interp";
                else                                      resolved = "cc";
            }
            pkg_manifest_free(&m);
            free(root);
            if (resolved) return resolved;
        }
        return "cc";
    }
    if (strcmp(cand, "cc") == 0)     return "cc";
    if (strcmp(cand, "jit") == 0)    return "jit";
    if (strcmp(cand, "interp") == 0) return "interp";
    fprintf(stderr,
            "tur: error TUR-E0311: unknown engine '%s' (from %s); expected "
            "\"cc\", \"jit\", or \"interp\"\n"
            "     precedence: --engine > TUR_ENGINE > build.tur :engine > "
            "\"cc\"; see `tur explain TUR-E0311`\n",
            cand, from);
    return NULL;
}
