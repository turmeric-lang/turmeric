/* script_deps.c -- the spices a single-file script declares in its
 * `(defscript ...)` header: the dep cache, the opt-in <script>.lock, and the
 * `tur fetch --script` / `tur add --script` / `tur cache` commands.
 * See script_deps.h and docs/upcoming/script-spice-deps-plan.md. */

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

#include "script_deps.h"

#include <dirent.h>
#include <errno.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#ifdef _WIN32
#include <io.h>
#endif

#include "buf.h"
#include "diag.h"
#include "fmt.h"
#include "global.h"
#include "pkg.h"
#include "platform_fs.h"
#include "runtime/experiments.h"
#include "runtime/globals.h"   /* g_opt_script_deps */
#include "spice_search.h"     /* spice_find_root */

bool g_script_deps_offline = false;
bool g_script_deps_update  = false;
bool g_script_deps_editor  = false;

/* What an entry's spice dir grows when it is set up as a fetched project, the
 * bookkeeping files at an entry's root, and the binaries `tur x` builds into
 * the entry (project-less-tools-plan PT3): none of it is fetched content, so
 * the tree hash leaves it out. */
static const char *const SD_HASH_SKIP[] = {
    "spices", "cmake", "tur.lock", ".tur-script-entry", ".tur-last-used",
    ".tur-x-bin", NULL,
};

#define SD_META    ".tur-script-entry"
#define SD_STAMP   ".tur-last-used"

/* ------------------------------------------------------------------ */
/* Small helpers                                                       */
/* ------------------------------------------------------------------ */

static const char *sd_base(const char *p) {
    const char *s = strrchr(p, '/');
#ifdef _WIN32
    const char *b = strrchr(p, '\\');
    if (b && (!s || b > s)) s = b;
#endif
    return s ? s + 1 : p;
}

static bool sd_is_dir(const char *p) {
    struct stat st;
    return stat(p, &st) == 0 && S_ISDIR(st.st_mode);
}

static bool sd_exists(const char *p) {
    struct stat st;
    return stat(p, &st) == 0;
}

static bool sd_mkdirp(const char *path) {
    char tmp[4096];
    snprintf(tmp, sizeof(tmp), "%s", path);
    for (char *p = tmp + 1; *p; p++) {
        if (*p != '/') continue;
        *p = '\0';
        if (mkdir(tmp, 0755) != 0 && errno != EEXIST) return false;
        *p = '/';
    }
    return mkdir(tmp, 0755) == 0 || errno == EEXIST;
}

static bool sd_is_commit_id(const char *s) {
    if (!s) return false;
    size_t n = 0;
    for (; s[n]; n++)
        if (!((s[n] >= '0' && s[n] <= '9') || (s[n] >= 'a' && s[n] <= 'f')))
            return false;
    return n == 40 || n == 64;
}

static char *sd_dup(const char *s) { return s ? strdup(s) : NULL; }

/* The current time, ISO-8601 UTC, into the caller's buffer. */
#define SD_NOW_MAX 32
static void sd_now(char buf[SD_NOW_MAX]) {
    time_t t = time(NULL);
    struct tm tmv;
#ifdef _WIN32
    gmtime_s(&tmv, &t);
#else
    gmtime_r(&t, &tmv);
#endif
    strftime(buf, SD_NOW_MAX, "%Y-%m-%dT%H:%M:%SZ", &tmv);
}

/* FNV-1a, for the short tag that keeps two `:subdir`s of one commit apart. */
static uint32_t sd_fnv(const char *s) {
    uint32_t h = 2166136261u;
    for (; s && *s; s++) { h ^= (uint8_t)*s; h *= 16777619u; }
    return h;
}

/* An error about a dep, in the shape the driver prints configuration errors
 * (`error [TUR-E0310]: ...`).  The header's own errors have spans and go
 * through diag_emit; these are about the cache, the network and the lock. */
static void sd_error(const char *script, const char *fmt, ...)
    TUR_PRINTF_FMT(2, 3);
static void sd_error(const char *script, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "%s: error [TUR-E0634]: ", script);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
}

/* `git` and the child `tur fetch` write progress to stdout; a script's stdout
 * is its own.  Point fd 1 at stderr while the cache is being filled. */
typedef struct { int saved; } SdStdout;

static void sd_stdout_to_stderr(SdStdout *s) {
    fflush(stdout);
    s->saved = dup(1);
    if (s->saved >= 0) dup2(2, 1);
}

static void sd_stdout_restore(SdStdout *s) {
    if (s->saved < 0) return;
    fflush(stdout);
    dup2(s->saved, 1);
    close(s->saved);
    s->saved = -1;
}

/* pkg_lock_read registers its file in the diagnostic table and judges the
 * read by diag_had_error; run it on a clean slate, as tur_state_read does. */
static bool sd_lock_read(const char *path, PkgLockFile *out) {
    const SourceFile *saved[DIAG_MAX_FILES];
    size_t n_saved = diag_files_save(saved, sizeof(saved) / sizeof(saved[0]));
    bool had_error = diag_had_error();
    diag_reset();
    bool ok = pkg_lock_read(path, out);
    diag_reset();
    diag_files_replace(saved, n_saved);
    if (had_error) diag_force_had_error();
    return ok;
}

/* ------------------------------------------------------------------ */
/* Cache entries                                                       */
/* ------------------------------------------------------------------ */

/* An entry's metadata, `.tur-script-entry`: `key value` lines. */
typedef struct {
    char *name, *url, *ref, *subdir, *resolved, *sha256, *created;
} SdMeta;

static void sd_meta_free(SdMeta *m) {
    free(m->name); free(m->url); free(m->ref); free(m->subdir);
    free(m->resolved); free(m->sha256); free(m->created);
    memset(m, 0, sizeof(*m));
}

static bool sd_meta_read(const char *entry, SdMeta *m) {
    memset(m, 0, sizeof(*m));
    char p[4096];
    snprintf(p, sizeof(p), "%s/" SD_META, entry);
    FILE *f = fopen(p, "r");
    if (!f) return false;
    char line[4096];
    while (fgets(line, sizeof(line), f)) {
        size_t n = strlen(line);
        while (n && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = '\0';
        char *sp = strchr(line, ' ');
        if (!sp) continue;
        *sp = '\0';
        const char *v = sp + 1;
        char **slot = strcmp(line, "name") == 0     ? &m->name
                    : strcmp(line, "url") == 0      ? &m->url
                    : strcmp(line, "ref") == 0      ? &m->ref
                    : strcmp(line, "subdir") == 0   ? &m->subdir
                    : strcmp(line, "resolved") == 0 ? &m->resolved
                    : strcmp(line, "sha256") == 0   ? &m->sha256
                    : strcmp(line, "created") == 0  ? &m->created : NULL;
        if (slot) { free(*slot); *slot = strdup(v); }
    }
    fclose(f);
    return m->url != NULL && m->resolved != NULL;
}

static bool sd_meta_write(const char *entry, const SdMeta *m) {
    char p[4096];
    snprintf(p, sizeof(p), "%s/" SD_META, entry);
    FILE *f = fopen(p, "w");
    if (!f) return false;
    fprintf(f, "name %s\nurl %s\n", m->name, m->url);
    if (m->ref)    fprintf(f, "ref %s\n", m->ref);
    if (m->subdir) fprintf(f, "subdir %s\n", m->subdir);
    fprintf(f, "resolved %s\n", m->resolved);
    if (m->sha256) fprintf(f, "sha256 %s\n", m->sha256);
    char now[SD_NOW_MAX];
    sd_now(now);
    fprintf(f, "created %s\n", m->created ? m->created : now);
    return fclose(f) == 0;
}

static void sd_stamp(const char *entry) {
    if (g_script_deps_editor) return;
    char p[4096];
    snprintf(p, sizeof(p), "%s/" SD_STAMP, entry);
    FILE *f = fopen(p, "w");
    char now[SD_NOW_MAX];
    sd_now(now);
    if (f) { fprintf(f, "%s\n", now); fclose(f); }
}

/* The entry name for (name, subdir, commit). */
static void sd_entry_name(const char *name, const char *subdir, const char *sha,
                          char *out, size_t cap) {
    if (subdir && *subdir)
        snprintf(out, cap, "%s-%.12s-%06x", name, sha,
                 (unsigned)(sd_fnv(subdir) & 0xffffffu));
    else
        snprintf(out, cap, "%s-%.12s", name, sha);
}

/* A complete entry is the visible name (a symlink to the tree on POSIX, the
 * tree itself on Windows) whose metadata file exists: the metadata is the
 * last thing written. */
static bool sd_entry_complete(const char *entry) {
    char p[4096];
    snprintf(p, sizeof(p), "%s/" SD_META, entry);
    return sd_exists(p);
}

static void sd_spice_dir(const char *entry, const char *subdir,
                         char *out, size_t cap) {
    if (subdir && *subdir) snprintf(out, cap, "%s/%s", entry, subdir);
    else                   snprintf(out, cap, "%s", entry);
}

/* Seed a not-yet-set-up spice dir's tur.lock with the lock's rows for this
 * dep's own dependencies (`<dep>/<name>`), so the `tur fetch` that sets it up
 * checks out what the lock pinned. */
static void sd_seed_lock(const char *spice_dir, const PkgLockFile *lock,
                         const char *dep) {
    if (!lock) return;
    size_t dl = strlen(dep);
    PkgLockFile sub;
    memset(&sub, 0, sizeof(sub));
    sub.format_version = 1;
    for (int i = 0; i < lock->n_entries; i++) {
        const PkgLockEntry *e = &lock->entries[i];
        if (e->is_cmake || strncmp(e->name, dep, dl) != 0 || e->name[dl] != '/')
            continue;
        PkgLockEntry *ne = (PkgLockEntry *)realloc(sub.entries,
            (size_t)(sub.n_entries + 1) * sizeof(PkgLockEntry));
        if (!ne) break;
        sub.entries = ne;
        PkgLockEntry *c = &sub.entries[sub.n_entries++];
        memset(c, 0, sizeof(*c));
        c->name       = strdup(e->name + dl + 1);
        c->url        = sd_dup(e->url);
        c->ref        = sd_dup(e->ref);
        c->resolved   = sd_dup(e->resolved);
        c->sha256     = sd_dup(e->sha256);
        c->fetched_at = sd_dup(e->fetched_at);
    }
    if (sub.n_entries > 0) {
        char p[4096];
        snprintf(p, sizeof(p), "%s/tur.lock", spice_dir);
        (void)pkg_lock_write(p, &sub);
    }
    pkg_lock_free(&sub);
}

/* Fetch (url, ref) -- at `pin` when one is known -- and build a complete
 * entry for it.  `entry_out` receives the entry path.  The tree is built
 * under a private name and only then given its visible one, so a concurrent
 * run either finds the finished tree or loses the race harmlessly, and an
 * interrupted fetch leaves nothing under a valid name. */
static bool sd_build_entry(const char *script, const char *cache,
                           const PkgSpice *s, const char *pin,
                           const PkgLockFile *lock,
                           char *entry_out, size_t cap,
                           char **out_sha) {
    *out_sha = NULL;
    static int counter = 0;
    char tmp[4096];
    snprintf(tmp, sizeof(tmp), "%s/.build-%s-%ld-%d", cache, s->name,
             (long)getpid(), counter++);
    (void)pkg_rm_rf(tmp);

    fprintf(stderr, "tur: fetching %s (%s) for %s\n", s->name,
            s->ref ? s->ref : pin ? pin : "default branch", sd_base(script));
    SdStdout so;
    sd_stdout_to_stderr(&so);
    /* A commit id is not something `git clone --branch` takes: clone the
     * default branch and move to the commit. */
    const char *clone_ref = sd_is_commit_id(s->ref) ? NULL : s->ref;
    char *sha = pin ? pkg_git_fetch_pinned(s->url, clone_ref, pin, tmp)
                    : pkg_git_fetch(s->url, clone_ref, tmp);
    sd_stdout_restore(&so);
    if (!sha) {
        (void)pkg_rm_rf(tmp);
        sd_error(script, "cannot fetch spice \"%s\" from %s (ref %s)",
                 s->name, s->url, s->ref ? s->ref : "default branch");
        return false;
    }

    char ename[512];
    sd_entry_name(s->name, s->subdir, sha, ename, sizeof(ename));
    snprintf(entry_out, cap, "%s/%s", cache, ename);
    if (sd_entry_complete(entry_out)) {
        /* Another script (or run) already built this commit. */
        (void)pkg_rm_rf(tmp);
        *out_sha = sha;
        return true;
    }

    /* The tree is the content, not the repository. */
    char gitdir[4096];
    snprintf(gitdir, sizeof(gitdir), "%s/.git", tmp);
    (void)pkg_rm_rf(gitdir);

    char spice_dir[4096];
    sd_spice_dir(tmp, s->subdir, spice_dir, sizeof(spice_dir));
    if (!sd_is_dir(spice_dir)) {
        (void)pkg_rm_rf(tmp);
        sd_error(script, "spice \"%s\": %s has no directory '%s' at %s",
                 s->name, s->url, s->subdir ? s->subdir : ".",
                 s->ref ? s->ref : sha);
        free(sha);
        return false;
    }
    SdMeta meta;
    memset(&meta, 0, sizeof(meta));
    char hash[PKG_HASH_MAX];
    meta.name     = s->name;
    meta.url      = s->url;
    meta.ref      = s->ref;
    meta.subdir   = s->subdir;
    meta.resolved = sha;
    meta.sha256   = pkg_hash_dir_excluding(spice_dir, SD_HASH_SKIP, hash) ? hash : NULL;
    char now[SD_NOW_MAX];
    sd_now(now);
    meta.created  = now;

    /* Set the tree up as a fetched project, the way `tur install` sets up an
     * installed root, so every project-mode resolver works on it unchanged. */
    char mp[4096];
    if (pkg_resolve_manifest_path(spice_dir, mp, sizeof(mp))) {
        PkgManifest m;
        memset(&m, 0, sizeof(m));
        if (pkg_manifest_read(mp, &m)) {
            sd_seed_lock(spice_dir, lock, s->name);
            sd_stdout_to_stderr(&so);
            bool ok = pkg_setup_fetched_project("script deps", spice_dir, &m);
            sd_stdout_restore(&so);
            pkg_manifest_free(&m);
            if (!ok) {
                (void)pkg_rm_rf(tmp);
                sd_error(script, "spice \"%s\": fetching its own dependencies "
                         "failed", s->name);
                free(sha);
                return false;
            }
        }
    }
    if (!sd_meta_write(tmp, &meta)) {
        (void)pkg_rm_rf(tmp);
        sd_error(script, "cannot write the cache entry for \"%s\" under %s",
                 s->name, cache);
        free(sha);
        return false;
    }

#ifdef _WIN32
    /* No dependable symlinks: rename the tree into place.  Paths a
     * :cmake-deps build recorded under the temporary name go stale; a
     * header dep with native deps should be fetched once more if so. */
    if (rename(tmp, entry_out) != 0) {
        (void)pkg_rm_rf(tmp);
        if (!sd_entry_complete(entry_out)) {
            sd_error(script, "cannot move the cache entry for \"%s\" into "
                     "place: %s", s->name, strerror(errno));
            free(sha);
            return false;
        }
    }
#else
    /* The tree keeps the name it was built under -- a :cmake-deps build
     * records absolute paths into it -- and the visible name is a symlink,
     * created atomically: whoever creates it first wins, and the loser's
     * tree is removed. */
    char real_name[4096];
    snprintf(real_name, sizeof(real_name), ".tree-%s-%ld-%d", ename,
             (long)getpid(), counter++);
    char real_path[4096];
    snprintf(real_path, sizeof(real_path), "%s/%s", cache, real_name);
    bool placed = rename(tmp, real_path) == 0;
    if (placed && symlink(real_name, entry_out) != 0) {
        /* A filesystem without symlinks: rename, as on Windows. */
        placed = errno != EEXIST && rename(real_path, entry_out) == 0;
    }
    if (!placed) {
        bool lost = sd_entry_complete(entry_out);
        (void)pkg_rm_rf(sd_exists(real_path) ? real_path : tmp);
        if (!lost) {
            sd_error(script, "cannot move the cache entry for \"%s\" into "
                     "place: %s", s->name, strerror(errno));
            free(sha);
            return false;
        }
    }
#endif
    *out_sha = sha;
    return true;
}

/* Remove an entry: on POSIX the visible symlink and the tree it names. */
static bool sd_entry_remove(const char *cache, const char *name) {
    char p[4096];
    snprintf(p, sizeof(p), "%s/%s", cache, name);
#ifndef _WIN32
    char target[4096];
    ssize_t n = readlink(p, target, sizeof(target) - 1);
    if (n > 0) {
        target[n] = '\0';
        char tp[4096];
        if (target[0] == '/') snprintf(tp, sizeof(tp), "%s", target);
        else                  snprintf(tp, sizeof(tp), "%s/%s", cache, target);
        if (unlink(p) != 0) return false;
        return pkg_rm_rf(tp);
    }
#endif
    return pkg_rm_rf(p);
}

/* ------------------------------------------------------------------ */
/* Resolution                                                          */
/* ------------------------------------------------------------------ */

static void sd_dep_free(ScriptDep *d) {
    free(d->name); free(d->dir); free(d->url); free(d->ref);
    free(d->resolved); free(d->sha256);
}

void script_deps_free(ScriptDeps *sd) {
    for (int i = 0; i < sd->n; i++) sd_dep_free(&sd->deps[i]);
    free(sd->deps);
    memset(sd, 0, sizeof(*sd));
}

static ScriptDep *sd_push(ScriptDeps *sd, const char *name, const char *dir) {
    ScriptDep *nd = (ScriptDep *)realloc(sd->deps,
                                         (size_t)(sd->n + 1) * sizeof(ScriptDep));
    if (!nd) return NULL;
    sd->deps = nd;
    ScriptDep *d = &sd->deps[sd->n++];
    memset(d, 0, sizeof(*d));
    d->name = strdup(name);
    d->dir  = strdup(dir);
    return d;
}

/* With a lock: every row this dep's tree carries must be the one the lock
 * pinned, and every tree must still hash to what the lock recorded. */
static bool sd_verify_locked(const char *script, const char *lock_path,
                             const PkgLockFile *lock, const PkgLockEntry *row,
                             const char *spice_dir, const char *dep) {
    char hash[PKG_HASH_MAX];
    if (pkg_hash_comparable(row->sha256) &&
        (!pkg_hash_dir_excluding(spice_dir, SD_HASH_SKIP, hash) ||
         strcmp(hash, row->sha256) != 0)) {
        sd_error(script, "the cached tree of \"%s\" (%s) does not match %s:\n"
                 "  lock:   %s\n  cached: %s\n"
                 "  It was modified after it was fetched.  Remove it with "
                 "`tur cache clean` and run again.",
                 dep, spice_dir, sd_base(lock_path), row->sha256, hash);
        return false;
    }
    size_t dl = strlen(dep);
    PkgLockFile own;
    char own_path[4096];
    snprintf(own_path, sizeof(own_path), "%s/tur.lock", spice_dir);
    bool have_own = sd_lock_read(own_path, &own);
    bool ok = true;
    for (int i = 0; ok && i < lock->n_entries; i++) {
        const PkgLockEntry *e = &lock->entries[i];
        if (e->is_cmake || strncmp(e->name, dep, dl) != 0 || e->name[dl] != '/')
            continue;
        const char *sub = e->name + dl + 1;
        PkgLockEntry *oe = have_own ? pkg_lock_find(&own, sub, false) : NULL;
        if (!oe || !oe->resolved || !e->resolved ||
            strcmp(oe->resolved, e->resolved) != 0) {
            sd_error(script, "the cached \"%s\" was set up with %s at %s, but "
                     "%s pins %s.  Remove the entry (`tur cache clean`) and "
                     "run again.", dep, sub,
                     oe && oe->resolved ? oe->resolved : "(nothing)",
                     sd_base(lock_path), e->resolved ? e->resolved : "?");
            ok = false;
            break;
        }
        char tdir[4096];
        if (e->ref) snprintf(tdir, sizeof(tdir), "%s/spices/%s-%s", spice_dir, sub, e->ref);
        else        snprintf(tdir, sizeof(tdir), "%s/spices/%s", spice_dir, sub);
        if (pkg_hash_comparable(e->sha256) && sd_is_dir(tdir) &&
            (!pkg_hash_dir(tdir, hash) || strcmp(hash, e->sha256) != 0)) {
            sd_error(script, "the cached dependency %s of \"%s\" (%s) does not "
                     "match %s.  Remove the entry (`tur cache clean`) and run "
                     "again.", sub, dep, tdir, sd_base(lock_path));
            ok = false;
        }
    }
    if (have_own) pkg_lock_free(&own);
    return ok;
}

int script_deps_resolve(const char *script, bool in_project, bool ignore_lock,
                        ScriptDeps *out) {
    memset(out, 0, sizeof(*out));
    if (!script || !g_opt_script_deps) return 0;

    PkgManifest hdr;
    PkgScriptStatus st = pkg_script_header_read(script, in_project, &hdr);
    if (st == PKG_SCRIPT_NONE) return 0;
    if (st == PKG_SCRIPT_ERROR) return -1;
    experiment_warn_if_used("script-deps");

    char script_abs[4096];
    if (!realpath(script, script_abs))
        snprintf(script_abs, sizeof(script_abs), "%s", script);
    char script_dir[4096];
    snprintf(script_dir, sizeof(script_dir), "%s", script_abs);
    char *slash = strrchr(script_dir, '/');
    if (slash && slash != script_dir) *slash = '\0';
    else snprintf(script_dir, sizeof(script_dir), "%s", slash ? "/" : ".");

    char lock_path[4096];
    snprintf(lock_path, sizeof(lock_path), "%s.lock", script_abs);
    PkgLockFile lock;
    memset(&lock, 0, sizeof(lock));
    bool have_lock = !ignore_lock && sd_exists(lock_path) &&
                     sd_lock_read(lock_path, &lock);

    char cache[4096] = "";
    char index_path[4096] = "";
    TurScriptIndex ix;
    memset(&ix, 0, sizeof(ix));
    bool ix_dirty = false;
    if (tur_script_cache_dir(cache, sizeof(cache))) {
        snprintf(index_path, sizeof(index_path), "%s/index.tur", cache);
        (void)tur_script_index_read(index_path, &ix);
    }

    int rc = 1;
    for (int i = 0; rc == 1 && i < hdr.n_spices; i++) {
        const PkgSpice *s = &hdr.spices[i];
        char dir[4096];

        if (s->is_global) {
            if (!tur_installed_spice_dir(s->name, dir, sizeof(dir), NULL, NULL)) {
                if (g_script_deps_editor) continue;
                sd_error(script, "spice \"%s\" is declared `:global true` but is "
                         "not installed -- `tur install` it, or give it a :url "
                         "and :ref", s->name);
                rc = -1;
                break;
            }
            if (!sd_push(out, s->name, dir)) rc = -1;
            continue;
        }

        if (s->path) {
            if (s->path[0] == '/') snprintf(dir, sizeof(dir), "%s", s->path);
            else snprintf(dir, sizeof(dir), "%s/%s", script_dir, s->path);
            if (s->subdir) {
                size_t l = strlen(dir);
                snprintf(dir + l, sizeof(dir) - l, "/%s", s->subdir);
            }
            if (!sd_is_dir(dir)) {
                if (g_script_deps_editor) continue;
                sd_error(script, "spice \"%s\": :path '%s' is not a directory "
                         "(resolved against the script's directory to %s)",
                         s->name, s->path, dir);
                rc = -1;
                break;
            }
            char canon[4096];
            if (!sd_push(out, s->name, realpath(dir, canon) ? canon : dir)) rc = -1;
            continue;
        }

        /* A :url dep (pkg_script_header_read guarantees a :ref).  Its pin,
         * in order: the lock, a commit-id :ref, index.tur (unless --update),
         * else resolve by fetching. */
        if (!cache[0]) {
            sd_error(script, "no data directory to cache \"%s\" in -- set "
                     "$HOME or $TUR_HOME", s->name);
            rc = -1;
            break;
        }
        const PkgLockEntry *row = NULL;
        const char *pin = NULL;
        if (have_lock) {
            row = pkg_lock_find(&lock, s->name, false);
            if (!row || !row->resolved ||
                !row->url || strcmp(row->url, s->url) != 0 ||
                !row->ref || strcmp(row->ref, s->ref) != 0) {
                sd_error(script, "%s does not pin spice \"%s\" as the header "
                         "declares it -- the lock is stale; run "
                         "`tur fetch --script %s --lock`",
                         sd_base(lock_path), s->name, script);
                rc = -1;
                break;
            }
            pin = row->resolved;
        } else if (sd_is_commit_id(s->ref)) {
            pin = s->ref;
        } else if (!g_script_deps_update) {
            pin = tur_script_index_lookup(&ix, s->url, s->ref);
        }

        char entry[4096] = "";
        char *resolved = NULL;
        if (pin) {
            char ename[512];
            sd_entry_name(s->name, s->subdir, pin, ename, sizeof(ename));
            snprintf(entry, sizeof(entry), "%s/%s", cache, ename);
            SdMeta meta;
            if (sd_entry_complete(entry) && sd_meta_read(entry, &meta)) {
                bool same = strcmp(meta.url, s->url) == 0 &&
                            strcmp(meta.resolved, pin) == 0 &&
                            ((!meta.subdir && !s->subdir) ||
                             (meta.subdir && s->subdir &&
                              strcmp(meta.subdir, s->subdir) == 0));
                sd_meta_free(&meta);
                if (!same) {
                    sd_error(script, "cache entry %s holds a different spice "
                             "than \"%s\" (%s) -- remove it with "
                             "`tur cache clean`", entry, s->name, s->url);
                    rc = -1;
                    break;
                }
                resolved = strdup(pin);
            }
        }
        if (!resolved) {
            if (g_script_deps_editor) continue;
            if (g_script_deps_offline) {
                sd_error(script, "spice \"%s\" (%s at %s) is not in the "
                         "script-dep cache, and --offline forbids fetching it "
                         "-- run once online, or `tur fetch --script %s`",
                         s->name, s->url, s->ref, script);
                rc = -1;
                break;
            }
            if (!sd_mkdirp(cache)) {
                sd_error(script, "cannot create %s: %s", cache, strerror(errno));
                rc = -1;
                break;
            }
            if (!sd_build_entry(script, cache, s, pin, have_lock ? &lock : NULL,
                                entry, sizeof(entry), &resolved)) {
                rc = -1;
                break;
            }
        }
        if (!sd_is_commit_id(s->ref) && !have_lock) {
            const char *known = tur_script_index_lookup(&ix, s->url, s->ref);
            if (!known || strcmp(known, resolved) != 0) {
                tur_script_index_set(&ix, s->url, s->ref, resolved);
                ix_dirty = true;
            }
        }

        char spice_dir[4096];
        sd_spice_dir(entry, s->subdir, spice_dir, sizeof(spice_dir));
        if (have_lock &&
            !sd_verify_locked(script, lock_path, &lock, row, spice_dir, s->name)) {
            free(resolved);
            rc = -1;
            break;
        }
        sd_stamp(entry);
        char canon[4096];
        ScriptDep *d = sd_push(out, s->name,
                               realpath(spice_dir, canon) ? canon : spice_dir);
        if (!d) { free(resolved); rc = -1; break; }
        d->url      = strdup(s->url);
        d->ref      = strdup(s->ref);
        d->resolved = resolved;
        SdMeta meta;
        if (sd_meta_read(entry, &meta)) {
            d->sha256 = meta.sha256 ? strdup(meta.sha256) : NULL;
            sd_meta_free(&meta);
        }
    }

    if (ix_dirty && index_path[0] && !g_script_deps_editor)
        (void)tur_script_index_write(index_path, &ix);
    tur_script_index_free(&ix);
    if (have_lock) pkg_lock_free(&lock);
    pkg_manifest_free(&hdr);
    if (rc < 0) script_deps_free(out);
    return rc;
}

/* ------------------------------------------------------------------ */
/* One entry, outside any header: `tur x <url>`                        */
/* ------------------------------------------------------------------ */

int script_deps_cache_entry(const char *who, const char *name,
                            const char *url, const char *ref,
                            const char *subdir, bool update, bool offline,
                            char *spice_dir_out, size_t cap) {
    char cache[4096];
    if (!tur_script_cache_dir(cache, sizeof(cache))) {
        sd_error(who, "no data directory to cache \"%s\" in -- set $HOME or "
                 "$TUR_HOME", name);
        return -1;
    }
    char index_path[4096];
    snprintf(index_path, sizeof(index_path), "%s/index.tur", cache);
    TurScriptIndex ix;
    memset(&ix, 0, sizeof(ix));
    (void)tur_script_index_read(index_path, &ix);
    /* index.tur keys a ref; no ref is the default branch, kept as HEAD. */
    const char *ix_ref = ref ? ref : "HEAD";

    const char *pin = NULL;
    if (sd_is_commit_id(ref))  pin = ref;
    else if (!update)          pin = tur_script_index_lookup(&ix, url, ix_ref);

    PkgSpice s;
    memset(&s, 0, sizeof(s));
    s.name   = (char *)name;
    s.url    = (char *)url;
    s.ref    = (char *)ref;
    s.subdir = (char *)subdir;

    int rc = 1;
    char entry[4096] = "";
    char *resolved = NULL;
    if (pin) {
        char ename[512];
        sd_entry_name(name, subdir, pin, ename, sizeof(ename));
        snprintf(entry, sizeof(entry), "%s/%s", cache, ename);
        SdMeta meta;
        if (sd_entry_complete(entry) && sd_meta_read(entry, &meta)) {
            bool same = strcmp(meta.url, url) == 0 &&
                        strcmp(meta.resolved, pin) == 0 &&
                        ((!meta.subdir && !subdir) ||
                         (meta.subdir && subdir && strcmp(meta.subdir, subdir) == 0));
            sd_meta_free(&meta);
            if (!same) {
                sd_error(who, "cache entry %s holds a different spice than %s "
                         "-- remove it with `tur cache clean`", entry, url);
                rc = -1;
            } else {
                resolved = strdup(pin);
            }
        }
    }
    if (rc == 1 && !resolved) {
        if (offline) {
            sd_error(who, "%s (%s) is not in the cache, and --offline forbids "
                     "fetching it -- run once without --offline", url,
                     ref ? ref : "default branch");
            rc = -1;
        } else if (!sd_mkdirp(cache)) {
            sd_error(who, "cannot create %s: %s", cache, strerror(errno));
            rc = -1;
        } else if (!sd_build_entry(who, cache, &s, pin, NULL,
                                   entry, sizeof(entry), &resolved)) {
            rc = -1;
        }
    }
    if (rc == 1 && !sd_is_commit_id(ref)) {
        const char *known = tur_script_index_lookup(&ix, url, ix_ref);
        if (!known || strcmp(known, resolved) != 0) {
            tur_script_index_set(&ix, url, ix_ref, resolved);
            (void)tur_script_index_write(index_path, &ix);
        }
    }
    if (rc == 1) {
        sd_stamp(entry);
        char spice_dir[4096];
        sd_spice_dir(entry, subdir, spice_dir, sizeof(spice_dir));
        char canon[4096];
        snprintf(spice_dir_out, cap, "%s",
                 realpath(spice_dir, canon) ? canon : spice_dir);
    }
    free(resolved);
    tur_script_index_free(&ix);
    return rc;
}

/* ------------------------------------------------------------------ */
/* tur fetch --script                                                  */
/* ------------------------------------------------------------------ */

static bool sd_require_flag(const char *who) {
    if (g_opt_script_deps) return true;
    fprintf(stderr,
            "%s: error [TUR-E0633]: script headers are experimental -- run "
            "with --enable=script-deps (e.g. `tur --enable=script-deps %s`)\n",
            who, who);
    return false;
}

int script_deps_fetch_cmd(const char *script, bool lock) {
    if (!sd_require_flag("fetch --script")) return 2;
    if (!sd_exists(script)) {
        fprintf(stderr, "tur fetch --script: no such file '%s'\n", script);
        return 2;
    }
    char *root = spice_find_root(script);
    bool in_project = root != NULL;
    free(root);
    ScriptDeps sd;
    int rc = script_deps_resolve(script, in_project, lock, &sd);
    if (rc < 0) return 1;
    if (rc == 0) {
        fprintf(stderr, "tur fetch --script: '%s' has no `(defscript ...)` "
                "header -- `tur add --script %s <url> --ref <ref>` writes one\n",
                script, script);
        return 1;
    }
    for (int i = 0; i < sd.n; i++)
        fprintf(stderr, "spice: %-16s %s%s%s\n", sd.deps[i].name, sd.deps[i].dir,
                sd.deps[i].resolved ? " @ " : "",
                sd.deps[i].resolved ? sd.deps[i].resolved : "");
    if (!lock) {
        script_deps_free(&sd);
        return 0;
    }

    /* The lock: one row per :url dep, then one per dependency of each, named
     * `<dep>/<name>` and copied from the entry's own tur.lock. */
    PkgLockFile lk;
    memset(&lk, 0, sizeof(lk));
    lk.format_version = 1;
    bool ok = true;
    for (int i = 0; ok && i < sd.n; i++) {
        const ScriptDep *d = &sd.deps[i];
        if (!d->url) continue;
        PkgLockEntry *ne = (PkgLockEntry *)realloc(lk.entries,
            (size_t)(lk.n_entries + 1) * sizeof(PkgLockEntry));
        if (!ne) { ok = false; break; }
        lk.entries = ne;
        PkgLockEntry *e = &lk.entries[lk.n_entries++];
        memset(e, 0, sizeof(*e));
        e->name       = strdup(d->name);
        e->url        = strdup(d->url);
        e->ref        = strdup(d->ref);
        e->resolved   = strdup(d->resolved);
        e->sha256     = sd_dup(d->sha256);
        char now[SD_NOW_MAX];
        sd_now(now);
        e->fetched_at = strdup(now);

        char own_path[4096];
        snprintf(own_path, sizeof(own_path), "%s/tur.lock", d->dir);
        PkgLockFile own;
        if (!sd_exists(own_path) || !sd_lock_read(own_path, &own)) continue;
        for (int j = 0; j < own.n_entries; j++) {
            const PkgLockEntry *oe = &own.entries[j];
            if (oe->is_cmake) continue;
            ne = (PkgLockEntry *)realloc(lk.entries,
                (size_t)(lk.n_entries + 1) * sizeof(PkgLockEntry));
            if (!ne) { ok = false; break; }
            lk.entries = ne;
            PkgLockEntry *t = &lk.entries[lk.n_entries++];
            memset(t, 0, sizeof(*t));
            size_t nl = strlen(d->name) + strlen(oe->name) + 2;
            t->name = (char *)malloc(nl);
            if (t->name) snprintf(t->name, nl, "%s/%s", d->name, oe->name);
            t->url        = sd_dup(oe->url);
            t->ref        = sd_dup(oe->ref);
            t->resolved   = sd_dup(oe->resolved);
            t->sha256     = sd_dup(oe->sha256);
            t->fetched_at = sd_dup(oe->fetched_at);
        }
        pkg_lock_free(&own);
    }
    char script_abs[4096];
    if (!realpath(script, script_abs))
        snprintf(script_abs, sizeof(script_abs), "%s", script);
    char lock_path[4096];
    snprintf(lock_path, sizeof(lock_path), "%s.lock", script_abs);
    if (ok) ok = pkg_lock_write(lock_path, &lk);
    if (ok) fprintf(stderr, "spice: lock file written to %s\n", lock_path);
    pkg_lock_free(&lk);
    script_deps_free(&sd);
    return ok ? 0 : 1;
}

/* ------------------------------------------------------------------ */
/* tur add --script                                                    */
/* ------------------------------------------------------------------ */

static void sd_put_str(Buf *b, const char *s) {
    buf_putc(b, '"');
    for (; *s; s++) {
        if (*s == '"' || *s == '\\') buf_putc(b, '\\');
        buf_putc(b, *s);
    }
    buf_putc(b, '"');
}

int script_deps_add_cmd(const char *script, int argc, char **argv) {
    if (!sd_require_flag("add --script")) return 2;
    const char *source = NULL, *ref = NULL, *subdir = NULL, *name = NULL;
    bool is_path = false;
    for (int i = 0; i < argc; i++) {
        const char *a = argv[i];
        if (strcmp(a, "--ref") == 0 && i + 1 < argc)         ref = argv[++i];
        else if (strcmp(a, "--subdir") == 0 && i + 1 < argc) subdir = argv[++i];
        else if (strcmp(a, "--name") == 0 && i + 1 < argc)   name = argv[++i];
        else if (strcmp(a, "--path") == 0)                   is_path = true;
        else if (a[0] != '-' && !source)                     source = a;
        else {
            fprintf(stderr, "tur add --script: unexpected argument '%s'\n", a);
            return 2;
        }
    }
    if (!source) {
        fprintf(stderr,
            "usage: tur add --script <file> <url> --ref <ref> [--subdir <dir>] [--name <n>]\n"
            "       tur add --script <file> <path> --path [--name <n>]\n");
        return 2;
    }
    if (!is_path && !ref) {
        fprintf(stderr, "tur add --script: a fetched dep needs --ref -- a script "
                "header pins every dep (a tag, a branch or a commit id)\n");
        return 2;
    }
    char derived[256];
    if (!name) {
        const char *b = subdir ? sd_base(subdir) : sd_base(source);
        snprintf(derived, sizeof(derived), "%s", b);
        size_t l = strlen(derived);
        while (l && derived[l - 1] == '/') derived[--l] = '\0';
        if (l > 4 && strcmp(derived + l - 4, ".git") == 0) derived[l - 4] = '\0';
        name = derived;
    }

    /* The file as it is, and its header (if any). */
    FILE *f = fopen(script, "rb");
    if (!f) {
        fprintf(stderr, "tur add --script: cannot open '%s': %s\n", script,
                strerror(errno));
        return 2;
    }
    Buf src;
    buf_init(&src);
    char chunk[8192];
    size_t got;
    while ((got = fread(chunk, 1, sizeof(chunk), f)) > 0) buf_write(&src, chunk, got);
    fclose(f);
    buf_putc(&src, '\0');
    size_t len = src.len - 1;

    PkgManifest hdr;
    PkgScriptStatus st = pkg_script_header_read(script, false, &hdr);
    if (st == PKG_SCRIPT_ERROR) { buf_free(&src); return 1; }

    const char *rest = src.data;
    size_t rest_len = len;
    ReaderType rt = detect_lang(src.data, len, &rest, &rest_len, NULL, NULL);
    if (rest == src.data) rt = reader_type_from_extension(script);
    size_t off = (size_t)(rest - src.data);
    size_t h_start = 0, h_end = 0;
    if (st == PKG_SCRIPT_OK &&
        pkg_script_header_locate(rest, rest_len, (int)rt, &h_start, &h_end)) {
        h_start += off;
        h_end   += off;
    } else {
        /* No header: it goes where one has to be -- after the preamble, at
         * the start of the line the first form is on. */
        size_t p = pkg_preamble_end(rest, rest_len);
        while (p > 0 && rest[p - 1] != '\n') p--;
        h_start = h_end = off + p;
    }

    /* The new entry list: the header's, with this one added or replaced. */
    Buf hb;
    buf_init(&hb);
    buf_puts(&hb, "(defscript :spices #map{");
    bool replaced = false;
    for (int i = 0; i <= hdr.n_spices; i++) {
        PkgSpice add = {0};
        const PkgSpice *s;
        if (i < hdr.n_spices) {
            s = &hdr.spices[i];
            if (strcmp(s->name, name) == 0) { replaced = true; continue; }
        } else {
            add.name   = (char *)name;
            add.url    = is_path ? NULL : (char *)source;
            add.path   = is_path ? (char *)source : NULL;
            add.ref    = is_path ? NULL : (char *)ref;
            add.subdir = (char *)subdir;
            s = &add;
        }
        sd_put_str(&hb, s->name);
        buf_puts(&hb, " #map{");
        bool first = true;
#define SD_KV(k, v) do { if (v) { if (!first) buf_putc(&hb, ' ');           \
                              buf_puts(&hb, k " "); sd_put_str(&hb, v);       \
                              first = false; } } while (0)
        SD_KV(":url", s->url);
        SD_KV(":ref", s->ref);
        SD_KV(":subdir", s->subdir);
        SD_KV(":path", s->path);
#undef SD_KV
        if (s->is_global) { buf_puts(&hb, first ? ":global true" : " :global true"); first = false; }
        if (s->optional)  { buf_puts(&hb, first ? ":optional true" : " :optional true"); }
        buf_puts(&hb, "} ");
    }
    buf_puts(&hb, "})\n");
    pkg_manifest_free(&hdr);

    Buf formatted;
    const char *htext = hb.data;
    size_t hlen = hb.len;
    bool have_fmt = fmt_format_buffer(script, hb.data, hb.len, READER_TURMERIC,
                                      &formatted) == 0;
    if (have_fmt) { htext = formatted.data; hlen = formatted.len; }

    Buf outb;
    buf_init(&outb);
    buf_write(&outb, src.data, h_start);
    buf_write(&outb, htext, hlen);
    if (hlen && htext[hlen - 1] != '\n') buf_putc(&outb, '\n');
    size_t tail = h_end;
    /* A replaced header keeps what followed it; a new one gets a blank line
     * before the rest of the file. */
    if (st != PKG_SCRIPT_OK) buf_putc(&outb, '\n');
    else while (tail < len && (src.data[tail] == '\n' || src.data[tail] == '\r')) tail++;
    if (st == PKG_SCRIPT_OK && tail < len) buf_putc(&outb, '\n');
    buf_write(&outb, src.data + tail, len - tail);

    char tmp[4096];
    snprintf(tmp, sizeof(tmp), "%s.tmp.%ld", script, (long)getpid());
    FILE *w = fopen(tmp, "wb");
    bool ok = w && fwrite(outb.data, 1, outb.len, w) == outb.len;
    if (w) ok = (fclose(w) == 0) && ok;
    struct stat sst;
    if (ok && stat(script, &sst) == 0) (void)chmod(tmp, sst.st_mode & 07777);
    if (ok && rename(tmp, script) != 0) ok = false;
    if (!ok) {
        unlink(tmp);
        fprintf(stderr, "tur add --script: cannot rewrite '%s'\n", script);
    } else {
        fprintf(stderr, "spice: %s \"%s\" %s the header of %s\n",
                replaced ? "updated" : "added", name,
                replaced ? "in" : "to", script);
    }
    if (have_fmt) buf_free(&formatted);
    buf_free(&hb);
    buf_free(&outb);
    buf_free(&src);
    return ok ? 0 : 1;
}

/* ------------------------------------------------------------------ */
/* tur cache                                                           */
/* ------------------------------------------------------------------ */

static void sd_du(const char *p, unsigned long long *bytes) {
    struct stat st;
    if (lstat(p, &st) != 0) return;
    if (S_ISLNK(st.st_mode)) return;
    if (!S_ISDIR(st.st_mode)) { *bytes += (unsigned long long)st.st_size; return; }
    DIR *d = opendir(p);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;
        char c[4096];
        snprintf(c, sizeof(c), "%s/%s", p, e->d_name);
        sd_du(c, bytes);
    }
    closedir(d);
}

/* When an entry was last used: its stamp's mtime, else its metadata's. */
static time_t sd_last_used(const char *entry) {
    char p[4096];
    struct stat st;
    snprintf(p, sizeof(p), "%s/" SD_STAMP, entry);
    if (stat(p, &st) == 0) return st.st_mtime;
    snprintf(p, sizeof(p), "%s/" SD_META, entry);
    if (stat(p, &st) == 0) return st.st_mtime;
    return 0;
}

static int sd_cmp_str(const void *a, const void *b) {
    return strcmp(*(const char *const *)a, *(const char *const *)b);
}

/* The visible entries (and, with `stale`, the leftovers of an interrupted
 * build: `.build-*`, and on POSIX a `.tree-*` no entry points at). */
static int sd_list(const char *cache, char ***out) {
    *out = NULL;
    DIR *d = opendir(cache);
    if (!d) return 0;
    int n = 0, cap = 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.') continue;
        if (strcmp(e->d_name, "index.tur") == 0) continue;
        if (n == cap) {
            cap = cap ? cap * 2 : 16;
            char **nv = (char **)realloc(*out, (size_t)cap * sizeof(char *));
            if (!nv) break;
            *out = nv;
        }
        (*out)[n++] = strdup(e->d_name);
    }
    closedir(d);
    if (n > 1) qsort(*out, (size_t)n, sizeof(char *), sd_cmp_str);
    return n;
}

static int cache_usage(void) {
    fprintf(stderr,
        "usage:\n"
        "  tur cache list                         the script-dep cache's entries\n"
        "  tur cache clean [--older-than <days>]  remove entries (not used for <days>)\n"
        "\n"
        "The cache holds the spices script headers declare (`defscript`);\n"
        "`tur install` and `tur fetch` never read it.\n");
    return 2;
}

int cmd_cache(int argc, char **argv) {
    /* argv[0] is "tur", argv[1] "cache". */
    if (argc < 3) return cache_usage();
    const char *sub = argv[2];
    char cache[4096];
    if (!tur_script_cache_dir(cache, sizeof(cache))) {
        fprintf(stderr, "tur cache: no data directory (set $HOME or $TUR_HOME)\n");
        return 1;
    }
    if (strcmp(sub, "--help") == 0 || strcmp(sub, "-h") == 0) {
        (void)cache_usage();
        return 0;
    }
    if (strcmp(sub, "list") == 0) {
        char **names = NULL;
        int n = sd_list(cache, &names);
        if (n == 0) printf("(the script-dep cache at %s is empty)\n", cache);
        for (int i = 0; i < n; i++) {
            char entry[4096];
            snprintf(entry, sizeof(entry), "%s/%s", cache, names[i]);
            SdMeta m;
            bool have = sd_meta_read(entry, &m);
            unsigned long long bytes = 0;
            char real[4096];
            sd_du(realpath(entry, real) ? real : entry, &bytes);
            time_t lu = sd_last_used(entry);
            char when[32] = "?";
            if (lu) {
                struct tm tmv;
#ifdef _WIN32
                localtime_s(&tmv, &lu);
#else
                localtime_r(&lu, &tmv);
#endif
                strftime(when, sizeof(when), "%Y-%m-%d %H:%M", &tmv);
            }
            printf("%-32s %8.1f KiB  last used %s  %s%s%s%s%s\n", names[i],
                   (double)bytes / 1024.0, when,
                   have ? m.url : "(incomplete)",
                   have && m.subdir ? " " : "", have && m.subdir ? m.subdir : "",
                   have && m.ref ? " @ " : "", have && m.ref ? m.ref : "");
            if (have) sd_meta_free(&m);
            free(names[i]);
        }
        free(names);
        return 0;
    }
    if (strcmp(sub, "clean") == 0) {
        long days = -1;
        for (int i = 3; i < argc; i++) {
            if (strcmp(argv[i], "--older-than") == 0 && i + 1 < argc) {
                char *end = NULL;
                days = strtol(argv[++i], &end, 10);
                if (!end || *end || days < 0) {
                    fprintf(stderr, "tur cache clean: --older-than takes a "
                            "number of days, not '%s'\n", argv[i]);
                    return 2;
                }
            } else {
                fprintf(stderr, "tur cache clean: unexpected argument '%s'\n", argv[i]);
                return cache_usage();
            }
        }
        time_t now = time(NULL);
        char **names = NULL;
        int n = sd_list(cache, &names);
        int removed = 0, failed = 0;
        for (int i = 0; i < n; i++) {
            char entry[4096];
            snprintf(entry, sizeof(entry), "%s/%s", cache, names[i]);
            if (days >= 0) {
                time_t lu = sd_last_used(entry);
                if (lu && difftime(now, lu) < (double)days * 86400.0) {
                    free(names[i]);
                    continue;
                }
            }
            if (sd_entry_remove(cache, names[i])) {
                printf("removed %s\n", names[i]);
                removed++;
            } else {
                fprintf(stderr, "tur cache clean: cannot remove %s\n", entry);
                failed++;
            }
            free(names[i]);
        }
        free(names);
        if (days < 0) {
            /* A full clean also drops the index and any leftover build. */
            char ip[4096];
            snprintf(ip, sizeof(ip), "%s/index.tur", cache);
            (void)unlink(ip);
            DIR *d = opendir(cache);
            struct dirent *e;
            while (d && (e = readdir(d)) != NULL) {
                if (strncmp(e->d_name, ".build-", 7) != 0 &&
                    strncmp(e->d_name, ".tree-", 6) != 0) continue;
                char p[4096];
                snprintf(p, sizeof(p), "%s/%s", cache, e->d_name);
                (void)pkg_rm_rf(p);
            }
            if (d) closedir(d);
        }
        printf("tur cache clean: removed %d entr%s\n", removed,
               removed == 1 ? "y" : "ies");
        return failed ? 1 : 0;
    }
    fprintf(stderr, "tur cache: unknown subcommand '%s'\n", sub);
    return cache_usage();
}
