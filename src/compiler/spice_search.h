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

#include <stdbool.h>

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

/* SC4+SC5+LS2: append the enclosing spice's `src/`, then the `src/` of every
 * `:spices` dep across the transitive closure that exists on disk, then the
 * workspace siblings' `src/`, to `*inc`.
 *
 * installed-spice-libraries D1/D2: when `input` has NO enclosing `build.tur`
 * (a manifest-less context), it appends the installed spices instead -- each
 * one's own `src/` in install-name order, then each one's dependency closure
 * -- unless spice_search_global_disabled().  Entries already on the list
 * (the `-I` dirs) are not appended twice, and nothing the caller put in
 * front moves, so an installed spice only fills a gap.  Each appended path is a strdup'd copy also recorded in `*owned`
 * (insertion order) for the caller to free.  Entries already in `*inc` stay in
 * front, so the elaborator's first-match-wins keeps them highest priority.
 * Appends nothing when `input` is NULL.
 *
 * script-spice-deps-plan SD2: when `input` carries a `(defscript ...)` header
 * (and --enable=script-deps is on), the header's deps -- fetched into the
 * script-dep cache on first use -- and their closures are appended INSTEAD,
 * and nothing else is: no installed-spice fallback.
 *
 * Returns 0, -1 on allocation failure, or SPICE_SEARCH_SCRIPT_ERROR when the
 * header could not be honored (already reported; the command should stop). */
#define SPICE_SEARCH_SCRIPT_ERROR (-2)
int spice_search_append(const char *input,
                        char ***inc, int *n_inc,
                        char ***owned, int *n_owned,
                        const SpiceSearchHooks *hooks);

/* installed-spice-libraries D8: the off switch for the project-less
 * fallback.  `--no-global-spices` sets the flag (the driver's
 * `--no-auto-spice` never reaches spice_search_append at all, so it implies
 * it); `TUR_NO_GLOBAL_SPICES=1` is the environment spelling, which the test
 * harnesses and libturi embedders use. */
extern bool g_no_global_spices;
bool spice_search_global_disabled(void);

/* Append one spice's own `src/` and its dependency closure, recording them
 * as installed-spice slots under `name` (the REPL's `:spice`; D10).  Dirs
 * already on the list are skipped.  Returns 0, or -1 on allocation failure. */
int spice_search_append_spice(const char *name, const char *spice_dir,
                              char ***inc, int *n_inc,
                              char ***owned, int *n_owned,
                              const SpiceSearchHooks *hooks);

/* The installed spices whose source tree is on disk, sorted by name:
 * parallel heap arrays of heap strings (`out_dirs` may be NULL).  Returns the
 * count; the caller frees every string and both arrays. */
int spice_search_installed_names(char ***out_names, char ***out_dirs);

/* D7 provenance.  Every slot the fallback (or spice_search_append_spice)
 * added is recorded, process-wide, with the installed spice it belongs to.
 *   - spice_search_installed_slot: that spice's name for an include dir
 *     (exactly as appended), or NULL; *out_is_dep says whether the dir is
 *     one of its dependencies rather than its own src/.
 *   - spice_search_note_import_dir: the elaborator reports the include dir
 *     that satisfied an import; an installed slot marks its spice's root as
 *     used.
 *   - spice_search_used_installed_roots: the roots marked so far, for the
 *     link step -- only spices a program imports contribute link inputs. */
const char *spice_search_installed_slot(const char *dir, bool *out_is_dep);
/* namespaced-spice-binaries D6: is `<dir>/<module>.tur` a `:bin` entry file
 * of the dependency or installed spice whose src/ `dir` is?  Those are
 * programs, not modules, and resolve as `module not found`.  `dir` is the
 * include dir exactly as appended; the project's own src/ hides nothing. */
bool        spice_search_module_hidden(const char *dir, const char *module);
void        spice_search_note_import_dir(const char *dir);
int         spice_search_used_installed_roots(const char *const **out);

/* Build a project's include search path from an already-loaded manifest:
 * `root/src` first when `include_own_src` is set, then the src/ of every
 * `:spices` dep across the transitive closure (workspace sibling -> `:path`
 * -> fetched `<root>/spices/<name>-<ref>` -> `<root>/spices/<name>`, a
 * `:global` dep from the install registry), deduplicated by real path.
 * *out_dirs is a heap array of strdup'd dirs (*out_n entries); the caller
 * frees each string and the array.  Shared by `tur run`, `tur test`,
 * `tur build <dir>`, `tur install` and the project-less fallback. */
void resolve_include_dirs_from_manifest(const char *root,
                                        const PkgManifest *m,
                                        bool include_own_src,
                                        const char ***out_dirs,
                                        int *out_n);

#endif /* TUR_SPICE_SEARCH_H */
