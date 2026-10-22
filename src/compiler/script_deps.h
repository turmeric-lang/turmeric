#ifndef TUR_SCRIPT_DEPS_H
#define TUR_SCRIPT_DEPS_H

/* script_deps.h -- the spices a single-file script declares in its
 * `(defscript :spices #map{...})` header (script-spice-deps-plan).
 *
 * The header is read by pkg_script_header_read (pkg.c).  This file turns it
 * into spice roots on disk:
 *
 *   - a `:url` dep is fetched into the script-dep cache,
 *     `<data>/cache/script-spices/<name>-<sha12>[-<subdir hash>]`, one
 *     immutable tree per (url, subdir, commit), set up as a fetched project
 *     (its own :spices in its spices/, its :cmake-deps in its cmake/).  A tag
 *     or branch `:ref` is resolved once and remembered in index.tur;
 *   - a `:path` dep is a directory relative to the script, never cached;
 *   - a `:global true` dep is the installed spice of that name.
 *
 * When `<script>.lock` exists every `:url` dep resolves from it and every
 * tree is hash-checked against it.
 *
 * spice_search_append (spice_search.c) calls script_deps_resolve for every
 * per-file command, so the header is honored in one place. */

#include <stdbool.h>

/* `--offline`: resolve from the cache only; a missing entry is TUR-E0634. */
extern bool g_script_deps_offline;
/* `--update`: resolve tag and branch refs again instead of reading index.tur
 * (a lock, when present, still wins: `tur fetch --script f --lock --update`
 * is how a locked script moves). */
extern bool g_script_deps_update;
/* The editor and MCP paths: never fetch, never print, never stamp an
 * entry's last use; a dep that is not cached yet contributes nothing. */
extern bool g_script_deps_editor;

typedef struct ScriptDep {
    char *name;        /* the header's key */
    char *dir;         /* the spice root: the directory holding its build.tur */
    /* `:url` deps only (NULL otherwise): what a lock row records. */
    char *url;
    char *ref;
    char *resolved;    /* full commit id */
    char *sha256;      /* tree hash of `dir`, setup output left out */
} ScriptDep;

typedef struct ScriptDeps {
    ScriptDep *deps;
    int        n;
} ScriptDeps;

/* Resolve `script`'s header.  `in_project` says a build.tur sits above it
 * (a header there is TUR-E0632).  Returns 0 when there is nothing to do --
 * no header, or --enable=script-deps is off (the elaborator then reports
 * TUR-E0633) -- 1 with `out` filled, or -1 after printing why not.
 * `ignore_lock` resolves as if there were no <script>.lock (what writing a
 * fresh one wants). */
int  script_deps_resolve(const char *script, bool in_project, bool ignore_lock,
                         ScriptDeps *out);
void script_deps_free(ScriptDeps *sd);

/* `tur fetch --script <file> [--update] [--lock]`: warm the cache, and with
 * --lock write (or refresh) <file>.lock.  Returns a process exit status. */
int  script_deps_fetch_cmd(const char *script, bool lock);

/* `tur add --script <file> <url> [--ref R] [--subdir D] [--name N]` and
 * `tur add --script <file> <path> --path [--name N]`: create the header, or
 * add the entry to it, written back through the formatter.  `argv` holds the
 * words after `--script <file>`.  Returns a process exit status. */
int  script_deps_add_cmd(const char *script, int argc, char **argv);

/* `tur cache list` / `tur cache clean [--older-than <days>]`. */
int  cmd_cache(int argc, char **argv);

#endif /* TUR_SCRIPT_DEPS_H */
