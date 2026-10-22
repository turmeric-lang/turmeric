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
    int    n_bin;
    char  *installed_at;  /* ISO-8601 timestamp */
    char  *built_with;    /* tur version used at install time */
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

/* global-spice-library-consumption: the source dir of an INSTALLED spice,
 * looked up by name in state.tur -- what a `#{:global true}` manifest dep
 * resolves to.  Returns false when it is not installed (or its directory is
 * gone).  `out_version` / `out_resolved` optionally receive freshly allocated
 * copies of the recorded version and resolved SHA (NULL for a --path
 * install); the caller frees them. */
bool tur_installed_spice_dir(const char *name, char *out, size_t out_sz,
                             char **out_version, char **out_resolved);

/* E14: the --json flag.  When true, `tur doc` and `tur test` print JSON and
 * `tur check` writes its diagnostics as JSON.  The storage lives in install.c,
 * not the driver, so libturi (which links tur_core but not the driver)
 * resolves it; the driver's flag parsing sets it. */
extern bool use_json_output;

#endif /* TUR_GLOBAL_H */
