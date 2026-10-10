#ifndef TUR_SPICE_SEARCH_H
#define TUR_SPICE_SEARCH_H

/* spice_search -- the module search path a file inside a spice gets.
 *
 * `tur check` / `tur run <file>` / `tur --interpret` and every other per-file
 * command walk up from the input file to the enclosing `build.tur` and search,
 * after the importing file's own directory and the stdlib: the spice's own
 * `src/`, each `:spices` dep's `src/`, and the other members of a workspace
 * that lists the spice in `:members`.  This is that walk, in tur_core so that
 * libturi embedders get the same list through turi_env_set_search_path_for
 * (src/turi/eval.h) instead of re-deriving it.
 * See docs/archive/notebook-eval-no-module-base-dir.md */

#include "pkg.h"

/* SC3: maximum number of parent directories spice_find_root walks before
 * giving up.  The deepest known intra-spice file is `<root>/src/<mod>/x.tur`
 * (3 levels above the spice root); 16 leaves ample headroom for worktrees,
 * nested temp checkouts, and `node_modules`-style nesting without ever
 * climbing all the way to `/` on a filesystem that has no `build.tur`. */
#define TUR_SPICE_WALK_MAX 16

/* SC3: walk up from `file_path`'s directory looking for a `build.tur` (or
 * `build.tur.sweet`).  Returns a heap-allocated absolute path to the directory
 * holding the manifest (the spice root), or NULL when none is found within
 * TUR_SPICE_WALK_MAX steps. */
char *spice_find_root(const char *file_path);

/* What spice_search_append reports as it goes.  Every hook may be NULL. */
typedef struct SpiceSearchHooks {
    /* After each directory `dir` is appended.  `n_inc` is the list's new
     * length; `producer` names the workspace member that supplied it (a
     * sibling's `src/`) and is NULL for the spice's own `src/` and its
     * `:spices` deps. */
    void (*on_add)(void *ud, int n_inc, const char *dir, const char *producer);
    /* Once, with the spice's own manifest, after its `:spices` deps. */
    void (*on_manifest)(void *ud, const PkgManifest *m);
    void *ud;
} SpiceSearchHooks;

/* SC4+SC5+LS2: append the enclosing spice's `src/`, then every `:spices`
 * dep's `src/` that exists on disk, then the workspace siblings' `src/`, to
 * `*inc`.  Each appended path is a strdup'd copy also recorded in `*owned`
 * (insertion order) for the caller to free.  Entries already in `*inc` stay in
 * front, so the elaborator's first-match-wins keeps them highest priority.
 * Appends nothing when `input` is NULL or has no enclosing `build.tur`.
 * Returns 0, or -1 on allocation failure. */
int spice_search_append(const char *input,
                        char ***inc, int *n_inc,
                        char ***owned, int *n_owned,
                        const SpiceSearchHooks *hooks);

#endif /* TUR_SPICE_SEARCH_H */
