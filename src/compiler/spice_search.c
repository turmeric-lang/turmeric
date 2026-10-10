/* spice_search.c -- the module search path a file inside a spice gets.
 *
 * Moved out of src/main.c (find_spice_root / auto_append_spice_includes) so
 * that libturi embedders resolve `(import ...)` the way the per-file commands
 * do.  main.c keeps its LS2 bookkeeping on top, through SpiceSearchHooks.
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
 * main.c's dir_of_path, '\\' included on Windows. */
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
 * so a '/'-only walk-up stops on its first step there (main.c's
 * last_path_sep says how that hid). */
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

int spice_search_append(const char *input,
                        char ***inc, int *n_inc,
                        char ***owned, int *n_owned,
                        const SpiceSearchHooks *hooks) {
    if (!input) return 0;
    char *root = spice_find_root(input);
    if (!root) return 0;
    int rc = 0;

    /* SC4: own src/ (always preferred over deps on name collision). */
    char own_src[4096];
    int n = snprintf(own_src, sizeof(own_src), "%s/src", root);
    if (n > 0 && (size_t)n < sizeof(own_src) && ss_is_dir(own_src)) {
        if (ss_add(own_src, NULL, inc, n_inc, owned, n_owned, hooks) != 0) rc = -1;
    }

    /* SC5: parse the manifest and append every fetched `:spices` dep's
     * src/.  Layout mirrors the convention used by cmd_run's project
     * mode: spices/<name>-<ref>/src/  (preferred), or spices/<name>/src/
     * for unversioned, or <root>/<s->path>/src/ for local-path deps.
     * If a `:subdir` is set (monorepo sub-package), descend into it
     * first, then look for src/.  If no src/ exists, fall back to the
     * dep dir itself so the user gets *some* search path. */
    char manifest_path[4096];
    if (rc == 0 && pkg_resolve_manifest_path(root, manifest_path, sizeof(manifest_path))) {
        PkgManifest m;
        memset(&m, 0, sizeof(m));
        if (pkg_manifest_read(manifest_path, &m)) {
            char spices_dir[4096];
            snprintf(spices_dir, sizeof(spices_dir), "%s/spices", root);
            for (int i = 0; i < m.n_spices && rc == 0; i++) {
                const PkgSpice *s = &m.spices[i];
                char dep_dir[4096];
                /* global-spice-library-consumption: `#{:global true}` resolves
                 * from the `tur install` registry, not from <root>/spices. */
                bool from_global = false;
                if (s->is_global) {
                    if (!tur_installed_spice_dir(s->name, dep_dir,
                                                 sizeof(dep_dir), NULL, NULL))
                        continue;
                    from_global = true;
                } else if (s->path) {
                    snprintf(dep_dir, sizeof(dep_dir), "%s/%s", root, s->path);
                } else if (s->ref) {
                    snprintf(dep_dir, sizeof(dep_dir), "%s/%s-%s",
                             spices_dir, s->name, s->ref);
                } else {
                    snprintf(dep_dir, sizeof(dep_dir), "%s/%s",
                             spices_dir, s->name);
                }
                if (s->subdir && !from_global) {
                    char joined[4096];
                    snprintf(joined, sizeof(joined), "%s/%s", dep_dir, s->subdir);
                    strncpy(dep_dir, joined, sizeof(dep_dir) - 1);
                    dep_dir[sizeof(dep_dir) - 1] = '\0';
                }
                char dep_src[4096];
                snprintf(dep_src, sizeof(dep_src), "%s/src", dep_dir);
                const char *chosen = ss_is_dir(dep_src) ? dep_src : dep_dir;
                /* Only add the path if it actually exists on disk.  A
                 * missing fetched dep (offline run, etc.) shouldn't
                 * pollute the include path with bogus dirs. */
                if (ss_is_dir(chosen)) {
                    if (ss_add(chosen, NULL, inc, n_inc, owned, n_owned, hooks) != 0)
                        rc = -1;
                }
            }
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
