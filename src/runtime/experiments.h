#ifndef TUR_EXPERIMENTS_H
#define TUR_EXPERIMENTS_H
/* experiments.h -- the experimental-feature-flag registry.
 *
 * Successor to the retired `-X<name>` surface (drop-x-flags-plan).  A
 * genuinely-in-flight compiler feature opts in with `--enable=<name>` on the
 * CLI or `:experiments [...]` in build.tur; every such feature is declared
 * once in the EXPERIMENTS[] table in experiments.c with all of its metadata
 * filled in, including an `expires_at` soft deadline: on that version's cut,
 * review the row and either graduate, shelve, or bump it.
 *
 * See docs/archive/history/experimental-flag-mechanism-plan.md and
 * docs/guides/experimental-flags-guide.md. */
#include <stdbool.h>
#include <stddef.h>

/* Where in its lifecycle an experiment sits.  Drives which warning code the
 * use-site emits (TUR-W0060 vs TUR-W0061). */
typedef enum ExperimentLifecycle {
    XF_LIFECYCLE_PROTOTYPE,  /* core algorithm/surface still in flux */
    XF_LIFECYCLE_BETA,       /* surface frozen, soaking before graduation */
} ExperimentLifecycle;

/* One row of the registry.  Every field is mandatory -- no flag may be added
 * without all of them populated (the release-cut skills surface expires_at as a
 * soft deadline to review, the docs site renders the rest). */
typedef struct ExperimentDescriptor {
    const char          *name;        /* kebab-case, no leading '-' */
    const char          *summary;     /* one-line, shown in `tur experiments` */
    const char          *plan_path;   /* docs/upcoming/... */
    const char          *introduced;  /* version string, e.g. "0.25.0" */
    const char          *expires_at;  /* version string, e.g. "0.28.0" */
    ExperimentLifecycle  lifecycle;
    bool                *opt_global;   /* points at g_opt_<name> (the enable bit) */
} ExperimentDescriptor;

/* --- Table iteration (for `tur experiments` and the release-cut skills) --- */
size_t                      experiment_count(void);
const ExperimentDescriptor *experiment_at(size_t i);
const ExperimentDescriptor *experiment_lookup(const char *name);

/* How an experiment came to be enabled, for the `tur experiments` source
 * column.  Numerically ascending == precedence lowest-to-highest, so
 * "higher-numbered source wins" (see experiment_enable): CLI beats manifest
 * beats user-config beats not-yet-set. */
typedef enum ExperimentSource {
    XF_SRC_NONE = 0,
    XF_SRC_USER_CONFIG,   /* ~/.config/turmeric/experiments.tur      */
    XF_SRC_MANIFEST,      /* project build.tur :experiments          */
    XF_SRC_CLI,           /* --enable=<name> or LSP force-enable-all */
} ExperimentSource;

/* Turn an experiment on.  Returns false if `name` is not a known experiment
 * (the caller then emits TUR-E0310).  `src` records the origin; a
 * higher-numbered source overrides a lower-numbered one (CLI beats manifest
 * beats user-config), but never the reverse. */
bool experiment_enable(const char *name, ExperimentSource src);

/* True iff the named experiment is known and currently enabled. */
bool experiment_is_enabled(const char *name);

/* The source that enabled experiment index `i` (XF_SRC_NONE if disabled). */
ExperimentSource experiment_source_at(size_t i);

/* Emit the lifecycle warning (TUR-W0060 prototype / TUR-W0061 beta) for the
 * named experiment, at most once per compile.  No-op if the name is unknown
 * or the experiment is not enabled.  Each gated feature calls this from its
 * own elaboration entry point. */
void experiment_warn_if_used(const char *name);

/* Reset the once-per-compile warning dedup state.  Called at the start of
 * each compile (alongside diag_reset). */
void experiment_reset_warnings(void);

/* Count the named experiment's lifecycle warning as already emitted.  For a
 * compiled program that embeds the compiler (r7rs-lang-plan T4's `eval`):
 * the compile that built it warned, and the program's own elaboration of the
 * same language should not warn again on its stderr. */
void experiment_mark_warned(const char *name);

/* UC-2 (user-config-experiments-plan): read the user-level experiments file
 * -- $XDG_CONFIG_HOME/turmeric/experiments.tur (fallback
 * $HOME/.config/turmeric/experiments.tur, or %APPDATA%\turmeric\experiments.tur
 * on Windows) -- and enable every name in its `:enable [...]` list at
 * XF_SRC_USER_CONFIG.  A no-op that returns false when the file is absent or
 * no home/config directory can be resolved.  An unknown *experiment name*
 * exits the process with TUR-E0310 (the file path is included in the
 * message), matching the manifest path's "typos surface immediately"
 * contract.  An unknown *key* emits TUR-W0062 and is otherwise ignored
 * (forward-compatible with future keys).
 *
 * Returns true iff a file was actually read.  Suppression -- skipping the
 * read when a project manifest declares its own :experiments -- is decided
 * by the caller before invoking this; see apply_user_config_experiments in
 * src/driver/spice.c. */
bool experiments_read_user_config(void);

#endif /* TUR_EXPERIMENTS_H */
