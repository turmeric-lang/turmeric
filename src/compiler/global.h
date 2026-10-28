#ifndef TUR_GLOBAL_H
#define TUR_GLOBAL_H

/*
 * global.h -- per-user (global) spice install support.
 *
 * Layout (XDG-compliant; overridable via $TUR_HOME or $XDG_DATA_HOME):
 *
 *   ~/.local/share/turmeric/
 *     spices/<name>-<ver>/      -- one tree per installed spice
 *     cache/                    -- shared git fetch cache
 *     state.tur                 -- installed-spice registry
 *
 *   ~/.local/bin/
 *     tur-<cmd>                 -- symlinks into each spice's build/
 *
 * The functions in this header resolve paths and create the layout
 * lazily on first use. They never touch the network.
 */

#include <stdbool.h>
#include <stddef.h>

/* ------------------------------------------------------------------ */
/* Path resolution                                                     */
/* ------------------------------------------------------------------ */

/* Fill `out` with the global-spice data root.
 *
 * Resolution order:
 *   1. $TUR_HOME (fully isolated sandbox; overrides everything)
 *   2. $XDG_DATA_HOME/turmeric
 *   3. $HOME/.local/share/turmeric
 *
 * Returns true on success. Path has no trailing slash. */
bool tur_global_data_dir(char *out, size_t out_sz);

/* Fill `out` with the global-spice bin dir.
 *
 *   1. $TUR_HOME/bin (under sandbox)
 *   2. $HOME/.local/bin
 *
 * Returns true on success. */
bool tur_global_bin_dir(char *out, size_t out_sz);

/* Fill `out` with the spices/ subdir (data_dir + "/spices"). */
bool tur_global_spices_dir(char *out, size_t out_sz);

/* Fill `out` with the cache/ subdir. */
bool tur_global_cache_dir(char *out, size_t out_sz);

/* Fill `out` with the state.tur path. */
bool tur_global_state_path(char *out, size_t out_sz);

/* Create the layout (data_dir/{spices,cache}, bin_dir) if it does not yet
 * exist. Idempotent. Returns true on success. */
bool tur_global_ensure_layout(void);

/* True if `bin_dir` (typically ~/.local/bin) is on the current $PATH. */
bool tur_global_bin_on_path(const char *bin_dir);

/* ------------------------------------------------------------------ */
/* state.tur registry                                                  */
/* ------------------------------------------------------------------ */

typedef struct TurStateEntry {
    char  *name;          /* installed-as name (e.g. "notebook") */
    char  *url;           /* source url; NULL for --path installs */
    char  *ref;           /* git ref as given; NULL for --path */
    char  *subdir;        /* monorepo subdir; NULL for repo root */
    char  *path;          /* local source path; set only for --path installs */
    char  *resolved;      /* resolved git SHA; NULL for --path */
    char  *version;       /* version string from build.tur :version */
    char **bin;           /* binary names this spice provides */
    /* namespaced-spice-binaries D1: parallel to `bin` -- whether this spice
     * holds the bare name (bin_dir/<bin> is its link).  A shadowed bin is
     * built but reachable only as `tur <spice>/<bin>`.  An old registry's
     * plain string list reads as all-linked. */
    bool  *bin_linked;
    int    n_bin;
    char  *installed_at;  /* ISO-8601 timestamp */
    char  *built_with;    /* tur version used at install time */
    /* installed-spice-libraries D5: where the install lives on disk.  `dir`
     * is the install root (the clone under spices/; for a --path install the
     * checkout itself), `src_dir` the directory holding the spice's build.tur
     * when `:subdir` puts it below `dir`.  NULL for an entry written by a tur
     * that predates the fields -- readers then fall back to reconstructing
     * the path from the entry name (tur_state_entry_src_dir). */
    char  *dir;
    char  *src_dir;
} TurStateEntry;

typedef struct TurState {
    int             format_version;
    TurStateEntry  *entries;
    int             n_entries;
} TurState;

/* Read state.tur into `out`. Missing file is not an error; `out` ends up
 * empty with format_version == 1. */
bool tur_state_read(const char *path, TurState *out);

/* Atomically write state.tur (temp file + rename). */
bool tur_state_write(const char *path, const TurState *st);

void tur_state_free(TurState *st);

/* Find an entry by name. Returns NULL if not found. */
TurStateEntry *tur_state_find(TurState *st, const char *name);

/* Add or replace an entry (takes ownership of any heap fields). Returns
 * pointer to the entry. */
TurStateEntry *tur_state_upsert(TurState *st, const char *name);

/* Remove an entry by name. Returns true if an entry was removed. */
bool tur_state_remove(TurState *st, const char *name);

/* namespaced-spice-binaries D3: find an installed spice the way a user names
 * it -- exactly, else with its `tur-` prefix dropped or added. */
TurStateEntry *tur_state_find_spice(TurState *st, const char *name);

/* Append a bin (with its D1 flag) to an entry. */
void tur_state_entry_add_bin(TurStateEntry *e, const char *bin, bool linked);

/* The index of `bin` in an entry's list, or -1. */
int tur_state_entry_bin_index(const TurStateEntry *e, const char *bin);

/* D1: the entry that holds the bare name `bin` (its :linked flag is set),
 * skipping the entry named `except` (may be NULL).  NULL when nobody does. */
TurStateEntry *tur_state_bin_holder(TurState *st, const char *bin,
                                    const char *except);

/* `tur-foo` -> `foo`: the short spelling `tur <spice>/<bin>` takes. */
const char *tur_bin_short_name(const char *bin);

/* installed-spice-libraries D5: the directory holding an entry's build.tur.
 * Prefers the recorded `:src-dir` / `:dir`; for an entry that predates them,
 * a --path install is its `:path` and a git install is reconstructed as
 * `spices/<name>-<sanitized-ref>[/<subdir>]`.  Writes the path to `out` even
 * when it does not exist; returns whether it is a directory. */
bool tur_state_entry_src_dir(const TurStateEntry *e, char *out, size_t out_sz);

/* The install ROOT an entry owns under spices/ -- what `tur uninstall` and
 * `tur upgrade` operate on.  The recorded `:dir`, else the reconstruction
 * above without `:subdir`.  Returns false for a --path install (its tree is
 * the user's checkout, never ours to delete) and when no root can be named. */
bool tur_state_entry_root_dir(const TurStateEntry *e, char *out, size_t out_sz);

/* global-spice-library-consumption: the source dir of an INSTALLED spice,
 * looked up by name in state.tur -- what a `#{:global true}` manifest dep
 * resolves to.  Returns false when it is not installed (or its directory is
 * gone).  `out_version` / `out_resolved` optionally receive freshly allocated
 * copies of the recorded version and resolved SHA (NULL for a --path
 * install); the caller frees them. */
bool tur_installed_spice_dir(const char *name, char *out, size_t out_sz,
                             char **out_version, char **out_resolved);

/* installed-spice-libraries D6: the module paths a spice provides -- every
 * `.tur` file under `<spice_dir>/src/`, relative to it, without the
 * extension (`json/decode`), in byte-wise order.  Dot entries are skipped.
 * Returns the count (0 when there is no src/); *out is a heap array of heap
 * strings, freed with tur_spice_modules_free. */
int  tur_spice_modules(const char *spice_dir, char ***out);
void tur_spice_modules_free(char **mods, int n);

/* namespaced-spice-binaries D6: the modules a spice's `:bin` entry files
 * would be imported as (`src/main.tur` -> `main`) -- exactly the set
 * tur_spice_modules leaves out, so the conflict check and the search path
 * cannot disagree.  Same ownership as tur_spice_modules; unsorted. */
int  tur_spice_bin_modules(const char *spice_dir, char ***out);

/* ------------------------------------------------------------------ */
/* script-spice-deps-plan SD1: the script-dep cache                    */
/* ------------------------------------------------------------------ */

/* `<data>/cache/script-spices` -- one immutable tree per (url, subdir,
 * commit), never registered in state.tur.  Writes the path even when the
 * directory does not exist yet; false only when no data root can be named. */
bool tur_script_cache_dir(char *out, size_t out_sz);

/* `index.tur` in that directory remembers which commit a tag or branch
 * `:ref` resolved to, so a later run of any script naming the same
 * (url, ref) needs no network until `--update`. */
typedef struct TurScriptRef {
    char *url;
    char *ref;
    char *resolved;     /* full commit id */
    char *fetched_at;   /* ISO-8601 */
} TurScriptRef;

typedef struct TurScriptIndex {
    TurScriptRef *refs;
    int           n_refs;
} TurScriptIndex;

/* Missing or unreadable reads as empty (true).  Leaves the caller's
 * diagnostic state alone, as tur_state_read does. */
bool tur_script_index_read(const char *path, TurScriptIndex *out);
bool tur_script_index_write(const char *path, const TurScriptIndex *ix);
/* The recorded commit for (url, ref), or NULL. */
const char *tur_script_index_lookup(const TurScriptIndex *ix,
                                    const char *url, const char *ref);
/* Record (or replace) the commit for (url, ref). */
void tur_script_index_set(TurScriptIndex *ix, const char *url, const char *ref,
                          const char *resolved);
void tur_script_index_free(TurScriptIndex *ix);

/* E14: the --json flag.  When true, `tur doc` and `tur test` print JSON and
 * `tur check` writes its diagnostics as JSON.  The storage lives in install.c,
 * not the driver, so libturi (which links tur_core but not the driver)
 * resolves it; the driver's flag parsing sets it. */
extern bool use_json_output;

#endif /* TUR_GLOBAL_H */
