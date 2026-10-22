/* spice_search.c -- the module search path a file inside a spice gets.
 *
 * Moved out of src/main.c (find_spice_root / auto_append_spice_includes) so
 * that libturi embedders resolve `(import ...)` the way the per-file commands
 * do.  The driver keeps its LS2 bookkeeping on top, through SpiceSearchHooks.
 * See spice_search.h and docs/archive/notebook-eval-no-module-base-dir.md. */

/* realpath / strdup under -std=c11; the same preamble as pkg.c (Windows is
 * left out on purpose -- see the note there). */
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif
#if defined(__APPLE__)
#  ifndef _DARWIN_C_SOURCE
#    define _DARWIN_C_SOURCE
#  endif
#elif !defined(_WIN32)
#  ifndef _POSIX_C_SOURCE
#    define _POSIX_C_SOURCE 200809L
#  endif
#endif

#include "spice_search.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "global.h"   /* tur_installed_spice_dir: :global spice deps */
#include "pkg.h"
#include "platform_fs.h"  /* realpath on Windows */

/* The directory part of `path`, or "." when it has none.  The same split as
 * the driver's drv_dir_of_path, '\\' included on Windows. */
static void ss_dir_of_path(const char *path, char *out, size_t cap) {
    const char *last_slash = strrchr(path, '/');
#ifdef _WIN32
    const char *last_bs = strrchr(path, '\\');
    if (last_bs && (!last_slash || last_bs > last_slash)) last_slash = last_bs;
#endif
    if (!last_slash) {
        out[0] = '.'; out[1] = '\0';
    } else {
        size_t n = (size_t)(last_slash - path);
        if (n == 0) n = 1; /* "/foo" -> "/" */
        if (n >= cap) n = cap - 1;
        memcpy(out, path, n);
        out[n] = '\0';
    }
}

/* The last separator in `p`.  realpath() hands back backslashes on Windows,
 * so a '/'-only walk-up stops on its first step there (the driver's
 * drv_last_path_sep says how that hid). */
static char *ss_last_path_sep(char *p) {
    char *slash = strrchr(p, '/');
#ifdef _WIN32
    char *bs = strrchr(p, '\\');
    if (bs && (!slash || bs > slash)) slash = bs;
#endif
    return slash;
}

char *spice_find_root(const char *file_path) {
    if (!file_path) return NULL;

    char raw_dir[4096];
    ss_dir_of_path(file_path, raw_dir, sizeof(raw_dir));

    /* Canonicalize.  realpath() requires the path to exist; for a file the
     * caller is about to read, the directory always exists, so this should
     * generally succeed.  If it fails (e.g. permissions), fall back to
     * cwd-prefixing for a relative path. */
    char dir[4096];
    if (realpath(raw_dir, dir) == NULL) {
        if (raw_dir[0] == '/') {
            strncpy(dir, raw_dir, sizeof(dir) - 1);
            dir[sizeof(dir) - 1] = '\0';
        } else {
            char cwd[4096];
            if (!getcwd(cwd, sizeof(cwd))) return NULL;
            int n;
            if (raw_dir[0] == '.' && raw_dir[1] == '\0') {
                n = snprintf(dir, sizeof(dir), "%s", cwd);
            } else {
                n = snprintf(dir, sizeof(dir), "%s/%s", cwd, raw_dir);
            }
            if (n < 0 || (size_t)n >= sizeof(dir)) return NULL;
        }
    }

    /* `dir` came through realpath() just above, which on Windows returns a
     * backslash path even when the caller passed forward slashes -- so a
     * '/'-only step never advanced past depth 0, spice_search_append then
     * contributed no include paths at all, and every `(import sibling)` inside
     * a spice went unresolved.  `tur check` hid it (the importing file's own
     * directory is already on the search path); the LSP, which analyses a
     * scratch copy in the temp directory, could not. */
    for (int steps = 0; steps < TUR_SPICE_WALK_MAX; steps++) {
        char candidate[4096];
        if (pkg_resolve_manifest_path(dir, candidate, sizeof(candidate))) {
            size_t dl = strlen(dir);
            char *res = (char *)malloc(dl + 1);
            if (res) memcpy(res, dir, dl + 1);
            return res;
        }
        char *slash = ss_last_path_sep(dir);
        if (!slash || slash == dir) break;
        *slash = '\0';
    }
    return NULL;
}

/* Append a strdup'd copy of `s` to both the include list and the owned
 * ledger.  Returns 0, or -1 on allocation failure. */
static int ss_append_owned(const char *s,
                           char ***inc, int *n_inc,
                           char ***owned, int *n_owned) {
    char *copy = strdup(s);
    if (!copy) return -1;
    char **bigger_inc = (char **)realloc(*inc, (size_t)(*n_inc + 1) * sizeof(char *));
    if (!bigger_inc) { free(copy); return -1; }
    *inc = bigger_inc;
    char **bigger_own = (char **)realloc(*owned, (size_t)(*n_owned + 1) * sizeof(char *));
    if (!bigger_own) { free(copy); return -1; }
    *owned = bigger_own;
    (*inc)[(*n_inc)++]     = copy;
    (*owned)[(*n_owned)++] = copy;
    return 0;
}

static bool ss_is_dir(const char *p) {
    struct stat st;
    return stat(p, &st) == 0 && S_ISDIR(st.st_mode);
}

static int ss_add(const char *dir, const char *producer,
                  char ***inc, int *n_inc, char ***owned, int *n_owned,
                  const SpiceSearchHooks *hooks) {
    if (ss_append_owned(dir, inc, n_inc, owned, n_owned) != 0) return -1;
    if (hooks && hooks->on_add) hooks->on_add(hooks->ud, *n_inc, dir, producer);
    return 0;
}

/* The transitive :spices walk's visited set; see visited_roots_mark.
 * (installed-spice-libraries IL0: this walk lived in the driver's spice.c,
 * where `tur install`, the project-less fallback and libturi could not reach
 * it.) */
typedef struct { char **paths; int n; int cap; } VisitedRoots;

static void collect_dep_dirs_recursive(const char *root, const char *top_root,
                                       const PkgManifest *m,
                                       const char ***dirs, int *n, int *cap,
                                       VisitedRoots *visited);

/* Build a project's include search path from an already-loaded manifest.
 * `root` is the project root (the dir holding build.tur); `m` its manifest.
 * When `include_own_src` is set, `root/src` is added first (so intra-spice
 * imports resolve for callers whose entry/test files live outside `src/`).
 * Each `:spices` dep is resolved in priority order:
 *   workspace-member sibling -> explicit `:path` -> fetched `:ref` dir
 *   -> `spices/<name>`; then `:subdir`, a `src/` preference, and a
 *   monorepo-sibling fallback when the resolved dir is absent.
 * *out_dirs is a heap array of strdup'd dirs (*out_n entries); the caller
 * frees each string and the array.  Shared by `tur run`, `tur test`, and
 * `tur build <dir>` so all three resolve the include path identically. */
/* spice-include-dirs-transitive: recurse one level into each dep's manifest
 * and add its :spices' src/ dirs too.  Walking only direct deps left tourist
 * unable to resolve `thread-pool/pool` (httpd's dep), even with the workspace
 * `:members` declaring it -- imports from one spice into another silently
 * dropped at the first hop.  Capacity grows as we go since transitive width is
 * not bounded by m->n_spices.
 *
 * Two separate dedups are at work, and they are not interchangeable:
 *   - include_dir_seen / push_include_dir dedupe the OUTPUT list, so a dep
 *     pulled in by several parents contributes one -I.
 *   - VisitedRoots below dedupes the WALK, so a cycle terminates.
 * This comment used to claim the first also did the second; it does not, and
 * a manifest cycle recursed until something else gave out.  See
 * docs/archive/history/spice-cycle-include-path-blowup.md. */
static bool include_dir_seen(const char **dirs, int n, const char *cand) {
    for (int i = 0; i < n; i++)
        if (dirs[i] && strcmp(dirs[i], cand) == 0) return true;
    return false;
}

static void grow_include_dirs(const char ***dirs, int *cap, int need) {
    if (need <= *cap) return;
    int new_cap = *cap ? *cap * 2 : 8;
    while (new_cap < need) new_cap *= 2;
    *dirs = (const char **)realloc(*dirs, (size_t)new_cap * sizeof(char *));
    *cap = new_cap;
}

/* Append `cand` to the include list unless an equivalent entry is already
 * there.  The path is canonicalized first (realpath, falling back to the raw
 * spelling when it fails): a transitively-resolved dep dir arrives as
 * `a/../b/src`, which the textual dedup would otherwise treat as distinct from
 * the `b/src` a different parent contributed.  Canonicalizing keeps the `-I`
 * list short and caps the damage from any future unbounded walk independently
 * of the visited set. */
static void push_include_dir(const char ***dirs, int *n, int *cap,
                             const char *cand) {
    char canon_buf[4096];
    const char *canon = cand;
    if (realpath(cand, canon_buf)) canon = canon_buf;
    if (include_dir_seen(*dirs, *n, canon)) return;
    grow_include_dirs(dirs, cap, *n + 1);
    if (!*dirs) return;
    (*dirs)[(*n)++] = strdup(canon);
}

/* spice-cycle-include-path-blowup: visited set for the transitive :spices walk,
 * keyed on each package root's canonical (realpath'd) path.  Without it a
 * manifest cycle (A declares B, B declares A) recurses forever: each lap
 * resolves through one more `../` hop, so the textual paths never repeat and
 * the output-dir dedup below never fires.  The symptom was either a
 * multi-kilobyte `-I` argument that cc rejected with "File name too long" on an
 * unrelated system header, or -- on a sanitized build -- a stack overflow.
 * Mirrors the keying pkg_collect_transitive_cmake_deps already uses for the
 * same shape of walk. */
/* True if `dir` was already visited; otherwise records it and returns false. */
static bool visited_roots_mark(VisitedRoots *v, const char *dir) {
    char canon_buf[4096];
    const char *canon = dir;
    if (realpath(dir, canon_buf)) canon = canon_buf;
    for (int i = 0; i < v->n; i++)
        if (strcmp(v->paths[i], canon) == 0) return true;
    if (v->n >= v->cap) {
        int nc = v->cap ? v->cap * 2 : 8;
        char **np = (char **)realloc(v->paths, (size_t)nc * sizeof(char *));
        if (!np) return true;   /* OOM: treat as visited so the walk terminates */
        v->paths = np;
        v->cap   = nc;
    }
    v->paths[v->n++] = strdup(canon);
    return false;
}

static void visited_roots_free(VisitedRoots *v) {
    for (int i = 0; i < v->n; i++) free(v->paths[i]);
    free(v->paths);
}

void resolve_include_dirs_from_manifest(const char *root,
                                        const PkgManifest *m,
                                        bool include_own_src,
                                        const char ***out_dirs,
                                        int *out_n) {
    *out_dirs = NULL;
    *out_n    = 0;

    int cap = 0;
    const char **dirs = NULL;
    int n = 0;
    grow_include_dirs(&dirs, &cap, (include_own_src ? 1 : 0) + m->n_spices + 8);
    if (!dirs) return;

    if (include_own_src) {
        char own_src[4096];
        snprintf(own_src, sizeof(own_src), "%s/src", root);
        struct stat ss;
        if (stat(own_src, &ss) == 0 && S_ISDIR(ss.st_mode))
            push_include_dir(&dirs, &n, &cap, own_src);
    }

    /* Seed the visited set with the root itself, so a dep that declares the
     * root back (the minimal A -> B -> A cycle) stops at the second hop. */
    VisitedRoots visited; memset(&visited, 0, sizeof(visited));
    visited_roots_mark(&visited, root);
    collect_dep_dirs_recursive(root, root, m, &dirs, &n, &cap, &visited);
    visited_roots_free(&visited);

    *out_dirs = dirs;
    *out_n    = n;
}

/* Walk every :spices entry in m, resolve its on-disk dir, push its src/ (or
 * dep_dir) into *dirs (deduped), then recurse into the dep's own manifest. */
/* `top_root` is the project the walk started from: `tur fetch` puts the
 * whole :spices closure in ITS spices/, flat, so a fetched dep of a fetched
 * dep is looked for there when its parent's own spices/ lacks it. */
static void collect_dep_dirs_recursive(const char *root, const char *top_root,
                                       const PkgManifest *m,
                                       const char ***dirs, int *n, int *cap,
                                       VisitedRoots *visited) {
    char spices_dir[4096];
    snprintf(spices_dir, sizeof(spices_dir), "%s/spices", root);
    for (int i = 0; i < m->n_spices; i++) {
        const PkgSpice *s = &m->spices[i];
        char dep_dir[4096];
        /* global-spice-library-consumption: a `:global` dep resolves from the
         * `tur install` registry and nowhere else -- a project-local fallback
         * would silently use a different spice than the manifest asked for.
         * Not installed: skipped, like any other dep whose dir is absent. */
        bool from_global = false;
        if (s->is_global) {
            if (!tur_installed_spice_dir(s->name, dep_dir, sizeof(dep_dir),
                                         NULL, NULL))
                continue;
            from_global = true;
        }
        /* LS4: a :spices entry that is a workspace sibling resolves to the
         * sibling's on-disk dir, ignoring any :url/:ref declared for
         * external publication. */
        char *ws_path = (s->path || from_global)
                            ? NULL
                            : pkg_workspace_member_path(root, s->name);
        if (from_global) {
            /* dep_dir is already the installed package root. */
        } else if (ws_path) {
            strncpy(dep_dir, ws_path, sizeof(dep_dir) - 1);
            dep_dir[sizeof(dep_dir) - 1] = '\0';
            free(ws_path);
        } else if (s->path) {
            snprintf(dep_dir, sizeof(dep_dir), "%s/%s", root, s->path);
        } else {
            if (s->ref)
                snprintf(dep_dir, sizeof(dep_dir), "%s/%s-%s",
                         spices_dir, s->name, s->ref);
            else
                snprintf(dep_dir, sizeof(dep_dir), "%s/%s", spices_dir, s->name);
            struct stat fs;
            if ((stat(dep_dir, &fs) != 0 || !S_ISDIR(fs.st_mode)) &&
                top_root && strcmp(top_root, root) != 0) {
                if (s->ref)
                    snprintf(dep_dir, sizeof(dep_dir), "%s/spices/%s-%s",
                             top_root, s->name, s->ref);
                else
                    snprintf(dep_dir, sizeof(dep_dir), "%s/spices/%s",
                             top_root, s->name);
            }
        }
        if (s->subdir && !from_global) {
            char tmp[4096];
            snprintf(tmp, sizeof(tmp), "%s/%s", dep_dir, s->subdir);
            strncpy(dep_dir, tmp, sizeof(dep_dir) - 1);
            dep_dir[sizeof(dep_dir) - 1] = '\0';
        }
        char src_sub[4096];
        snprintf(src_sub, sizeof(src_sub), "%s/src", dep_dir);
        struct stat ss;
        const char *chosen = NULL;
        if (stat(src_sub, &ss) == 0 && S_ISDIR(ss.st_mode))
            chosen = src_sub;
        else if (stat(dep_dir, &ss) == 0 && S_ISDIR(ss.st_mode))
            chosen = dep_dir;

        /* `resolved_root` is the dep's true on-disk package root, used both
         * for manifest lookup (the recursion's `root`) and for choosing which
         * include path to push when src/ is not directly present. */
        char resolved_root[4096];
        resolved_root[0] = '\0';
        if (chosen) {
            /* dep_dir/src exists OR dep_dir itself exists; the package root is
             * dep_dir.  If chosen == src_sub, strip the trailing "/src" so the
             * manifest read targets dep_dir, not its src/. */
            strncpy(resolved_root, dep_dir, sizeof(resolved_root) - 1);
            resolved_root[sizeof(resolved_root) - 1] = '\0';
        }
        bool fallback_added = false;
        if (!chosen && s->subdir) {
            char ancestor[4096];
            strncpy(ancestor, root, sizeof(ancestor) - 1);
            ancestor[sizeof(ancestor) - 1] = '\0';
            for (int up = 0; up < 4 && !fallback_added; up++) {
                char *slash = strrchr(ancestor, '/');
                if (!slash || slash == ancestor) break;
                *slash = '\0';
                char sib_src[4096];
                snprintf(sib_src, sizeof(sib_src),
                         "%s/%s/src", ancestor, s->subdir);
                if (stat(sib_src, &ss) == 0 && S_ISDIR(ss.st_mode)) {
                    push_include_dir(dirs, n, cap, sib_src);
                    snprintf(resolved_root, sizeof(resolved_root),
                             "%s/%s", ancestor, s->subdir);
                    chosen = NULL;
                    fallback_added = true;
                    break;
                }
                char sib_dir[4096];
                snprintf(sib_dir, sizeof(sib_dir),
                         "%s/%s", ancestor, s->subdir);
                if (stat(sib_dir, &ss) == 0 && S_ISDIR(ss.st_mode)) {
                    push_include_dir(dirs, n, cap, sib_dir);
                    strncpy(resolved_root, sib_dir, sizeof(resolved_root) - 1);
                    resolved_root[sizeof(resolved_root) - 1] = '\0';
                    chosen = NULL;
                    fallback_added = true;
                    break;
                }
            }
        }
        if (chosen) push_include_dir(dirs, n, cap, chosen);

        /* Recurse into the dep's own manifest so its :spices contribute their
         * src/ too.  Use `resolved_root` (the real on-disk package root) for
         * manifest lookup; `dep_dir` may have :subdir appended to a :path,
         * yielding a non-existent path when both are set together. */
        if (!resolved_root[0]) continue;
        /* Stop on re-entry. A diamond (two parents pulling the same dep) is
         * deduped for free by the same check -- its src/ was already pushed on
         * the first visit. */
        if (visited_roots_mark(visited, resolved_root)) continue;
        char dmp[4096];
        if (!pkg_resolve_manifest_path(resolved_root, dmp, sizeof(dmp))) continue;
        PkgManifest dm; memset(&dm, 0, sizeof(dm));
        if (!pkg_manifest_read(dmp, &dm)) continue;
        collect_dep_dirs_recursive(resolved_root, top_root, &dm, dirs, n, cap,
                                   visited);
        pkg_manifest_free(&dm);
    }
}

/* ------------------------------------------------------------------ */
/* installed-spice-libraries: the project-less fallback (D2, D7, D8)   */
/* ------------------------------------------------------------------ */

bool g_no_global_spices = false;

bool spice_search_global_disabled(void) {
    if (g_no_global_spices) return true;
    const char *env = getenv("TUR_NO_GLOBAL_SPICES");
    return env && env[0] && strcmp(env, "0") != 0;
}

/* Provenance of every include slot the fallback added, keyed by the exact
 * directory string it appended: which installed spice it belongs to (a
 * dependency's slot belongs to the top-level installed spice that pulled it
 * in) and that spice's root.  Process-wide rather than threaded through the
 * elaborator's state, because the roots it records are read back AFTER the
 * compile, by the driver's link step (drv_collect_build_aux), when the
 * per-compile resolver context is already gone. */
typedef struct {
    char *dir;
    char *name;
    char *root;
    bool  is_dep;
} InstalledSlot;

static InstalledSlot *g_islots;
static int            g_n_islots, g_cap_islots;
static char         **g_used_roots;
static int            g_n_used, g_cap_used;

static const InstalledSlot *ss_find_slot(const char *dir) {
    if (!dir) return NULL;
    for (int i = 0; i < g_n_islots; i++)
        if (strcmp(g_islots[i].dir, dir) == 0) return &g_islots[i];
    return NULL;
}

static void ss_record_slot(const char *dir, const char *name, const char *root,
                           bool is_dep) {
    if (ss_find_slot(dir)) return;
    if (g_n_islots >= g_cap_islots) {
        int nc = g_cap_islots ? g_cap_islots * 2 : 16;
        InstalledSlot *ns = (InstalledSlot *)realloc(g_islots,
                                                     (size_t)nc * sizeof(*ns));
        if (!ns) return;
        g_islots = ns; g_cap_islots = nc;
    }
    InstalledSlot *sl = &g_islots[g_n_islots];
    sl->dir  = strdup(dir);
    sl->name = strdup(name);
    sl->root = strdup(root);
    sl->is_dep = is_dep;
    if (!sl->dir || !sl->name || !sl->root) {
        free(sl->dir); free(sl->name); free(sl->root);
        return;
    }
    g_n_islots++;
}

const char *spice_search_installed_slot(const char *dir, bool *out_is_dep) {
    const InstalledSlot *sl = ss_find_slot(dir);
    if (out_is_dep) *out_is_dep = sl ? sl->is_dep : false;
    return sl ? sl->name : NULL;
}

void spice_search_note_import_dir(const char *dir) {
    const InstalledSlot *sl = ss_find_slot(dir);
    if (!sl) return;
    for (int i = 0; i < g_n_used; i++)
        if (strcmp(g_used_roots[i], sl->root) == 0) return;
    if (g_n_used >= g_cap_used) {
        int nc = g_cap_used ? g_cap_used * 2 : 8;
        char **nu = (char **)realloc(g_used_roots, (size_t)nc * sizeof(char *));
        if (!nu) return;
        g_used_roots = nu; g_cap_used = nc;
    }
    char *copy = strdup(sl->root);
    if (copy) g_used_roots[g_n_used++] = copy;
}

int spice_search_used_installed_roots(const char *const **out) {
    *out = (const char *const *)g_used_roots;
    return g_n_used;
}

/* Is `dir` (or the directory it resolves to) already on the list?  The
 * fallback runs after the -I dirs, so an -I that names an installed spice's
 * src/ keeps its slot and its plain `(-I ...)` provenance. */
static bool ss_already_listed(char **inc, int n_inc, const char *dir) {
    char want[4096];
    const char *w = realpath(dir, want) ? want : dir;
    for (int i = 0; i < n_inc; i++) {
        if (!inc[i]) continue;
        if (strcmp(inc[i], dir) == 0) return true;
        char have[4096];
        if (realpath(inc[i], have) && strcmp(have, w) == 0) return true;
    }
    return false;
}

typedef struct { char *name; char *dir; } SsInstalled;

static int ss_cmp_installed(const void *a, const void *b) {
    return strcmp(((const SsInstalled *)a)->name, ((const SsInstalled *)b)->name);
}

/* The installed spices whose source tree is on disk, sorted by install name
 * (byte-wise: neither the locale nor state.tur's entry order may decide which
 * of two spices wins).  Heap array; free with ss_installed_free. */
static int ss_installed_list(SsInstalled **out) {
    *out = NULL;
    char state_path[4096];
    if (!tur_global_state_path(state_path, sizeof(state_path))) return 0;
    TurState st;
    memset(&st, 0, sizeof(st));
    if (!tur_state_read(state_path, &st) || st.n_entries == 0) {
        tur_state_free(&st);
        return 0;
    }
    SsInstalled *v = (SsInstalled *)calloc((size_t)st.n_entries, sizeof(*v));
    int n = 0;
    for (int i = 0; v && i < st.n_entries; i++) {
        char dir[4096];
        if (!st.entries[i].name ||
            !tur_state_entry_src_dir(&st.entries[i], dir, sizeof(dir)))
            continue;
        v[n].name = strdup(st.entries[i].name);
        v[n].dir  = strdup(dir);
        if (!v[n].name || !v[n].dir) { free(v[n].name); free(v[n].dir); continue; }
        n++;
    }
    tur_state_free(&st);
    if (n > 1) qsort(v, (size_t)n, sizeof(*v), ss_cmp_installed);
    *out = v;
    return n;
}

static void ss_installed_free(SsInstalled *v, int n) {
    for (int i = 0; i < n; i++) { free(v[i].name); free(v[i].dir); }
    free(v);
}

int spice_search_installed_names(char ***out_names, char ***out_dirs) {
    SsInstalled *v = NULL;
    int n = ss_installed_list(&v);
    *out_names = n ? (char **)calloc((size_t)n, sizeof(char *)) : NULL;
    if (out_dirs) *out_dirs = n ? (char **)calloc((size_t)n, sizeof(char *)) : NULL;
    for (int i = 0; i < n; i++) {
        if (*out_names) { (*out_names)[i] = v[i].name; v[i].name = NULL; }
        if (out_dirs && *out_dirs) { (*out_dirs)[i] = v[i].dir; v[i].dir = NULL; }
    }
    ss_installed_free(v, n);
    return n;
}

int spice_search_append_spice(const char *name, const char *spice_dir,
                              char ***inc, int *n_inc,
                              char ***owned, int *n_owned,
                              const SpiceSearchHooks *hooks) {
    int rc = 0;
    char own_src[4096];
    snprintf(own_src, sizeof(own_src), "%s/src", spice_dir);
    if (ss_is_dir(own_src) && !ss_already_listed(*inc, *n_inc, own_src)) {
        if (ss_add(own_src, NULL, inc, n_inc, owned, n_owned, hooks) != 0) return -1;
        ss_record_slot(own_src, name, spice_dir, false);
    }
    char mp[4096];
    if (!pkg_resolve_manifest_path(spice_dir, mp, sizeof(mp))) return rc;
    PkgManifest m;
    memset(&m, 0, sizeof(m));
    if (!pkg_manifest_read(mp, &m)) return rc;
    const char **dirs = NULL;
    int nd = 0;
    resolve_include_dirs_from_manifest(spice_dir, &m, false, &dirs, &nd);
    for (int i = 0; i < nd; i++) {
        if (rc == 0 && !ss_already_listed(*inc, *n_inc, dirs[i])) {
            if (ss_add(dirs[i], NULL, inc, n_inc, owned, n_owned, hooks) != 0)
                rc = -1;
            else
                ss_record_slot(dirs[i], name, spice_dir, true);
        }
        free((void *)dirs[i]);
    }
    free((void *)dirs);
    pkg_manifest_free(&m);
    return rc;
}

/* D2 steps 4-5: every installed spice's own src/, in install-name order, then
 * each one's transitive :spices closure in the same order.  All top-level
 * spices come before any dependency, so the spice the user installed BY NAME
 * beats another version of it that some other installed spice depends on. */
static int ss_append_installed(char ***inc, int *n_inc,
                               char ***owned, int *n_owned,
                               const SpiceSearchHooks *hooks) {
    SsInstalled *v = NULL;
    int n = ss_installed_list(&v);
    int rc = 0;
    for (int i = 0; i < n && rc == 0; i++) {
        char own_src[4096];
        snprintf(own_src, sizeof(own_src), "%s/src", v[i].dir);
        if (!ss_is_dir(own_src) || ss_already_listed(*inc, *n_inc, own_src)) continue;
        if (ss_add(own_src, NULL, inc, n_inc, owned, n_owned, hooks) != 0) rc = -1;
        else ss_record_slot(own_src, v[i].name, v[i].dir, false);
    }
    for (int i = 0; i < n && rc == 0; i++) {
        char mp[4096];
        if (!pkg_resolve_manifest_path(v[i].dir, mp, sizeof(mp))) continue;
        PkgManifest m;
        memset(&m, 0, sizeof(m));
        if (!pkg_manifest_read(mp, &m)) continue;
        const char **dirs = NULL;
        int nd = 0;
        resolve_include_dirs_from_manifest(v[i].dir, &m, false, &dirs, &nd);
        for (int j = 0; j < nd; j++) {
            if (rc == 0 && !ss_already_listed(*inc, *n_inc, dirs[j])) {
                if (ss_add(dirs[j], NULL, inc, n_inc, owned, n_owned, hooks) != 0)
                    rc = -1;
                else
                    ss_record_slot(dirs[j], v[i].name, v[i].dir, true);
            }
            free((void *)dirs[j]);
        }
        free((void *)dirs);
        pkg_manifest_free(&m);
    }
    ss_installed_free(v, n);
    return rc;
}

int spice_search_append(const char *input,
                        char ***inc, int *n_inc,
                        char ***owned, int *n_owned,
                        const SpiceSearchHooks *hooks) {
    if (!input) return 0;
    char *root = spice_find_root(input);
    if (!root) {
        /* installed-spice-libraries D1: a manifest-less context -- no
         * build.tur above the anchor -- searches the installed spices after
         * everything it already searches.  A project never does: its
         * manifest says what it depends on. */
        if (spice_search_global_disabled()) return 0;
        return ss_append_installed(inc, n_inc, owned, n_owned, hooks);
    }
    int rc = 0;

    /* SC4: own src/ (always preferred over deps on name collision). */
    char own_src[4096];
    int n = snprintf(own_src, sizeof(own_src), "%s/src", root);
    if (n > 0 && (size_t)n < sizeof(own_src) && ss_is_dir(own_src)) {
        if (ss_add(own_src, NULL, inc, n_inc, owned, n_owned, hooks) != 0) rc = -1;
    }

    /* SC5: parse the manifest and append the src/ of every `:spices` dep
     * across the transitive closure (resolve_include_dirs_from_manifest:
     * workspace sibling -> :path -> fetched spices/<name>-<ref> ->
     * spices/<name>, :subdir, a `src/` preference, a `:global` dep from the
     * install registry).  It used to read one level of `:spices` only, so a
     * file inside a spice whose dep has a dep of its own could not resolve
     * the grand-dep; walking the closure only fills such gaps.  A dep that
     * is not on disk (offline, never fetched) contributes nothing. */
    char manifest_path[4096];
    if (rc == 0 && pkg_resolve_manifest_path(root, manifest_path, sizeof(manifest_path))) {
        PkgManifest m;
        memset(&m, 0, sizeof(m));
        if (pkg_manifest_read(manifest_path, &m)) {
            const char **dirs = NULL;
            int nd = 0;
            resolve_include_dirs_from_manifest(root, &m, false, &dirs, &nd);
            for (int i = 0; i < nd; i++) {
                if (rc == 0 && ss_add(dirs[i], NULL, inc, n_inc, owned, n_owned, hooks) != 0)
                    rc = -1;
                free((void *)dirs[i]);
            }
            free((void *)dirs);
            if (hooks && hooks->on_manifest) hooks->on_manifest(hooks->ud, &m);
            pkg_manifest_free(&m);
        }
    }

    /* LS2 (local-spice-dev-workflow-plan): workspace member auto-resolution.
     *
     * Walk ancestors of `root` looking for any directory containing a
     * `build.tur` whose `:members [...]` list names our spice (matched by
     * comparing the resolved absolute path of `<workspace>/<member>` to
     * `root`).  When found, add every *other* member's `src/` to the
     * include path.  Workspace membership is the consent boundary:
     * sibling members can import each other without an explicit :spices
     * entry; external publication still uses URL deps. */
    char anc[4096];
    size_t rlen = strlen(root);
    if (rc == 0 && rlen + 1 < sizeof(anc)) {
        memcpy(anc, root, rlen + 1);
        for (int up = 0; up < TUR_SPICE_WALK_MAX && rc == 0; up++) {
            char *last = ss_last_path_sep(anc);
            if (!last || last == anc) break;
            *last = '\0';

            char ws_manifest[4096];
            if (!pkg_resolve_manifest_path(anc, ws_manifest, sizeof(ws_manifest)))
                continue;

            PkgManifest wm;
            memset(&wm, 0, sizeof(wm));
            bool ok = pkg_manifest_read(ws_manifest, &wm);
            if (ok && wm.n_members > 0) {
                /* Self-detection: locate the member entry whose absolute
                 * path matches `root`.  Only proceed if the current spice
                 * is itself a listed member of this candidate workspace. */
                const char *self_member_path = NULL;
                for (int i = 0; i < wm.n_members; i++) {
                    char mp[4096];
                    int mn = snprintf(mp, sizeof(mp), "%s/%s", anc, wm.members[i]);
                    if (mn <= 0 || (size_t)mn >= sizeof(mp)) continue;
                    char real_mp[4096];
                    if (realpath(mp, real_mp) == NULL) continue;
                    if (strcmp(real_mp, root) == 0) {
                        self_member_path = wm.members[i];
                        break;
                    }
                }
                if (self_member_path) {
                    for (int i = 0; i < wm.n_members && rc == 0; i++) {
                        if (strcmp(wm.members[i], self_member_path) == 0) continue;
                        char sib_src[4096];
                        int sn = snprintf(sib_src, sizeof(sib_src), "%s/%s/src",
                                          anc, wm.members[i]);
                        if (sn <= 0 || (size_t)sn >= sizeof(sib_src)) continue;
                        if (ss_is_dir(sib_src)) {
                            if (ss_add(sib_src, wm.members[i], inc, n_inc,
                                       owned, n_owned, hooks) != 0)
                                rc = -1;
                        }
                    }
                    pkg_manifest_free(&wm);
                    break;
                }
            }
            pkg_manifest_free(&wm);
            /* Any build.tur (workspace or not) terminates the walk so we
             * don't accidentally treat a non-workspace ancestor's project
             * as the enclosing workspace. */
            if (ok) break;
        }
    }

    free(root);
    return rc;
}
