/*
 * install.c -- `tur install` / `tur uninstall` (global spice install).
 *
 * Pipeline (`tur install`):
 *   1. Ensure the XDG layout exists.
 *   2. Resolve the source tree (git fetch into spices/<name>-<ref>/ or
 *      reference a local --path checkout in place).
 *   3. Read build.tur from the source tree; require :bin to be non-empty.
 *   4. Build each declared binary by shelling out to `tur build`.
 *   5. Symlink each binary into the user's bin dir (~/.local/bin/), with
 *      conflict detection.
 *   6. Update state.tur.
 *
 * `tur uninstall` is the inverse: drop the symlinks, drop the source tree
 * (for non-path installs), and remove the state.tur entry.
 */

#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif
#if defined(__APPLE__)
#  ifndef _DARWIN_C_SOURCE
#    define _DARWIN_C_SOURCE
#  endif
#elif !defined(_WIN32)
/* Windows is excluded deliberately: MinGW reads _POSIX_C_SOURCE as "hide the
 * Win32 CRT names", which un-declares mkdir/getcwd and hides _finddata_t --
 * which in turn breaks <dirent.h> itself.  glibc needs this macro to EXPOSE
 * those declarations; on Windows it does the exact opposite. */
#  ifndef _POSIX_C_SOURCE
#    define _POSIX_C_SOURCE 200809L
#  endif
#endif

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>
#ifndef _WIN32
#include <sys/wait.h>
#endif

#ifdef __APPLE__
#  include <mach-o/dyld.h>
#endif

#include "buf.h"
#include "global.h"
#include "pkg.h"
#include "spice_search.h"  /* resolve_include_dirs_from_manifest */
#include "platform_fs.h"
#include "platform_proc.h"
#ifdef _WIN32
#include <windows.h>  /* GetModuleFileNameA */
#endif

/* Storage for the --json global, declared in global.h.  The driver sets it
 * from argv. */
bool use_json_output = false;

/* ------------------------------------------------------------------ */
/* Local utilities                                                     */
/* ------------------------------------------------------------------ */

/* Resolve the absolute path of the running tur executable. Returns true
 * on success. Falls back to "tur" (PATH resolution at exec time) when no
 * platform API succeeds. */
static bool inst_self_exe(char *out, size_t cap) {
#if defined(__APPLE__)
    uint32_t sz = (uint32_t)cap;
    if (_NSGetExecutablePath(out, &sz) == 0) {
        char real[4096];
        if (realpath(out, real)) {
            size_t rl = strlen(real);
            if (rl < cap) { memcpy(out, real, rl + 1); return true; }
        }
        return true;
    }
#elif defined(_WIN32)
    /* No /proc on Windows.  GetModuleFileNameA(NULL, ...) yields the running
     * image's full path, which is what readlink("/proc/self/exe") gives us on
     * Linux.  It does not NUL-terminate on truncation, so treat a full-buffer
     * return as failure rather than reading past the end. */
    DWORD n = GetModuleFileNameA(NULL, out, (DWORD)cap);
    if (n > 0 && n < cap) { out[n] = '\0'; return true; }
#else
    ssize_t n = readlink("/proc/self/exe", out, cap - 1);
    if (n > 0) { out[n] = '\0'; return true; }
#endif
    snprintf(out, cap, "tur");
    return true;
}

/* Walk up from self_exe to find a directory containing `src/runtime/hamt.c`
 * (the dev layout the autolink markers in stdlib expect). Returns true and
 * writes the path into `out` on success. */
static bool inst_find_turmeric_root(const char *self_exe,
                                     char *out, size_t cap) {
    char dir[4096];
    snprintf(dir, sizeof(dir), "%s", self_exe);
    char *slash = strrchr(dir, '/');
    if (!slash) return false;
    *slash = '\0';

    for (int i = 0; i < 8; i++) {
        char probe[4096];
        snprintf(probe, sizeof(probe), "%s/src/runtime/hamt.c", dir);
        struct stat st;
        if (stat(probe, &st) == 0 && S_ISREG(st.st_mode)) {
            snprintf(out, cap, "%s", dir);
            return true;
        }
        slash = strrchr(dir, '/');
        if (!slash || slash == dir) return false;
        *slash = '\0';
    }
    return false;
}

static bool inst_mkdirp(const char *path) {
    struct stat st;
    if (stat(path, &st) == 0) return S_ISDIR(st.st_mode);
    if (mkdir(path, 0755) == 0) return true;
    if (errno == EEXIST) return true;

    char tmp[4096];
    snprintf(tmp, sizeof(tmp), "%s", path);
    size_t len = strlen(tmp);
    if (len && tmp[len - 1] == '/') tmp[--len] = '\0';
    for (size_t i = 1; i < len; i++) {
        if (tmp[i] == '/') {
            tmp[i] = '\0';
            if (mkdir(tmp, 0755) != 0 && errno != EEXIST) return false;
            tmp[i] = '/';
        }
    }
    return mkdir(path, 0755) == 0 || errno == EEXIST;
}

static const char *inst_iso_now(void) {
    static char buf[32];
    time_t t = time(NULL);
    struct tm *tm = gmtime(&t);
    strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", tm);
    return buf;
}

/* Derive a sensible spice name from a git url or local path. */
static void inst_derive_name(const char *url_or_path, char *out, size_t cap) {
    const char *last = strrchr(url_or_path, '/');
    const char *base = last ? last + 1 : url_or_path;
    snprintf(out, cap, "%s", base);
    size_t n = strlen(out);
    if (n > 4 && strcmp(out + n - 4, ".git") == 0) out[n - 4] = '\0';
    if (strncmp(out, "tur-", 4) == 0) memmove(out, out + 4, strlen(out) - 3);
}

/* Is `path` a symlink (POSIX) or a reparse point (Windows)?
 *
 * A recursive delete must never descend through one: the link is what belongs
 * to us, its target does not.  `rm -rf` got this right and the in-process walk
 * below has to as well, or removing a spice tree containing a symlinked
 * directory would take out the directory it points AT. */
static bool inst_is_link(const char *path) {
#ifdef _WIN32
    DWORD a = GetFileAttributesA(path);
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_REPARSE_POINT);
#else
    struct stat st;
    return lstat(path, &st) == 0 && S_ISLNK(st.st_mode);
#endif
}

/* Depth-first unlink.  False if anything could not be removed. */
static bool inst_rm_tree(const char *path) {
    if (inst_is_link(path)) return remove(path) == 0;

    struct stat st;
    if (stat(path, &st) != 0) return false;
    if (!S_ISDIR(st.st_mode)) {
#ifdef _WIN32
        /* git marks everything under .git/objects read-only, and DeleteFile
         * refuses a read-only file -- the classic reason `rm -rf` of a clone
         * fails on Windows.  Drop the attribute before unlinking. */
        chmod(path, S_IREAD | S_IWRITE);
#endif
        return remove(path) == 0;
    }

    bool ok = true;
    DIR *d = opendir(path);
    if (!d) return false;
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0)
            continue;
        char child[4096];
        int n = snprintf(child, sizeof(child), "%s/%s", path, ent->d_name);
        if (n < 0 || n >= (int)sizeof(child)) { ok = false; continue; }
        ok = inst_rm_tree(child) && ok;
    }
    closedir(d);
    if (!ok) return false;
    return rmdir(path) == 0;
}

/* Recursive delete.  Refuses to remove paths that don't contain a '/' (no
 * delete of a bare ".") to limit blast radius -- the same contract the shell
 * version had.
 *
 * Was `system("rm -rf -- '%s'")`, which failed twice on Windows: cmd.exe has
 * no `rm`, and it does not treat '...' as quoting, so the path arrived with
 * the quotes still attached.  A tree walk needs neither a shell nor a quoting
 * rule.  docs/archive/windows-spice-fetch-shell-quoting.md */
bool pkg_rm_rf(const char *path) {
    if (!path || !*path) return false;
    if (!strchr(path, '/')) return false;
    if (strstr(path, "..")) return false; /* defensive */
    return inst_rm_tree(path);
}

/* What installing one :bin does to bin_dir/<bin>. */
typedef enum {
    BIN_LINK,     /* place (or replace) the link: the bare name is ours */
    BIN_SHADOW,   /* build it, but leave bin_dir alone: `tur <spice>/<bin>` */
    BIN_REFUSE,   /* a file tur did not install is in the way (reported) */
} BinPlan;

/* namespaced-spice-binaries D1/D2: decide what installing `bin_name` as part
 * of spice `install_name` does to `target` (bin_dir/<bin>).
 *
 * The registry answers first, on every platform: it records which installed
 * spice holds each bare name (`:linked`), which a symlink's target cannot say
 * for a --path install (it points into the user's checkout) and a Windows
 * copy cannot say at all.
 *   - another spice holds it: shadow (D2), or with `take` (--force, --link)
 *     link over it and report the holder in *out_holder;
 *   - this spice already has it: keep its flag (an upgrade or reinstall
 *     never changes who holds a name, D5), unless `take`;
 *   - nobody tur installed claims it: link it if the path is free, else it
 *     is the user's own file, which only --force overwrites. */
static BinPlan inst_plan_bin(const char *verb, const char *target,
                             const char *bin_name, const char *install_name,
                             TurState *state, const char *spices_dir,
                             const char *self_install_dir,
                             bool take, bool force, const char **out_holder) {
    *out_holder = NULL;
    TurStateEntry *self = tur_state_find(state, install_name);
    int own = tur_state_entry_bin_index(self, bin_name);
    bool own_linked = own >= 0 && (!self->bin_linked || self->bin_linked[own]);
    TurStateEntry *holder = tur_state_bin_holder(state, bin_name, install_name);
    if (holder) {
        *out_holder = holder->name;
        return take ? BIN_LINK : BIN_SHADOW;
    }
    if (own_linked) return BIN_LINK;
    if (own >= 0 && !take) return BIN_SHADOW;

    struct stat st;
    if (lstat(target, &st) != 0) {
        if (errno == ENOENT) return BIN_LINK;
        fprintf(stderr, "tur %s: cannot stat '%s': %s\n",
                verb, target, strerror(errno));
        return BIN_REFUSE;
    }
    if (force) return BIN_LINK;
    if (S_ISLNK(st.st_mode)) {
        char buf[4096];
        ssize_t n = readlink(target, buf, sizeof(buf) - 1);
        if (n > 0) {
            buf[n] = '\0';
            if (self_install_dir && strncmp(buf, self_install_dir,
                                            strlen(self_install_dir)) == 0)
                return BIN_LINK; /* our own earlier install, registry lost */
            if (strncmp(buf, spices_dir, strlen(spices_dir)) == 0) {
                fprintf(stderr,
                    "tur %s: '%s' links into %s (-> %s), but no installed "
                    "spice claims it.\n"
                    "  Remove it, or pass --force.\n",
                    verb, target, spices_dir, buf);
                return BIN_REFUSE;
            }
        }
    }
    fprintf(stderr,
        "tur %s: refusing to overwrite existing '%s' "
        "(not owned by tur). Use --force to override.\n", verb, target);
    return BIN_REFUSE;
}

/* `tur <spice>/<bin>`'s spelling of a bin, for the messages below. */
static void inst_qualified(const char *spice, const char *bin, char *out, size_t cap) {
    snprintf(out, cap, "%s/%s", spice, tur_bin_short_name(bin));
}

/* The FILE NAME a :bin entry takes inside bin_dir.
 *
 * Windows resolves a bare command name through PATHEXT, so an extensionless
 * file on the PATH is not runnable as a command -- an installed `tur-sample`
 * would never be found.  `tur build -o` emits no suffix, so the installed entry
 * adds one.
 *
 * The suffix is a PROPERTY OF THE PATH, not of the name: state.tur keeps the
 * logical `tur-sample` on every platform (so a registry written here reads the
 * same as one written on Linux, and `tur list --json` does not vary), and every
 * filesystem path is built through this. */
static void inst_bin_name(const char *bin, char *out, size_t cap) {
#ifdef _WIN32
    snprintf(out, cap, "%s.exe", bin);
#else
    snprintf(out, cap, "%s", bin);
#endif
}

#ifdef _WIN32
/* Say once per run that the installed binaries are copies rather than links. */
static void inst_note_copy_fallback(void) {
    static bool said = false;
    if (said) return;
    said = true;
    fprintf(stderr,
        "tur install: note: installing a COPY, not a link.  Creating a symlink "
        "needs administrator rights or Developer Mode"
        " (Settings > System > For developers).\n"
        "  Rebuilding the spice will not update the installed command until you "
        "run `tur upgrade`.\n");
}
#endif

/* Place `src` at `target`, atomically: build it beside the target, then move it
 * over in one step.
 *
 * POSIX: a symlink, as it has always been -- rebuilding the spice then updates
 * the installed command for free.
 *
 * Windows: CreateSymbolicLink needs administrator rights or Developer Mode, so
 * try it and fall back to a copy.  Under Developer Mode -- which most developer
 * machines have on -- the semantics are identical to POSIX; otherwise the copy
 * costs the live-update property, which inst_note_copy_fallback says out loud
 * rather than leaving the user to discover.
 *
 * platform_fs.h's symlink() shim reports ENOSYS and is deliberately not used
 * here: its warning is against a shim that succeeds only when elevated, and the
 * fallback is what answers it.  See
 * docs/archive/windows-install-binary-placement.md */
static bool inst_place_bin(const char *src, const char *target) {
    char tmp[4096];
    snprintf(tmp, sizeof(tmp), "%s.tmp.%d", target, (int)getpid());
    unlink(tmp);
#ifdef _WIN32
    /* Not in every MinGW winnt.h yet; the value is stable ABI. */
#  ifndef SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE
#    define SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE 0x2
#  endif
    if (!CreateSymbolicLinkA(tmp, src,
                             SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE)) {
        if (!CopyFileA(src, tmp, FALSE)) {
            fprintf(stderr,
                "tur install: cannot place '%s': neither a symlink nor a copy "
                "of '%s' could be created (error %lu)\n",
                tmp, src, (unsigned long)GetLastError());
            return false;
        }
        inst_note_copy_fallback();
    }
    /* rename() refuses an existing target on Windows; MoveFileEx replaces it,
     * which is what the POSIX rename above has always done. */
    if (!MoveFileExA(tmp, target, MOVEFILE_REPLACE_EXISTING)) {
        fprintf(stderr, "tur install: cannot move '%s' -> '%s' (error %lu)\n",
                tmp, target, (unsigned long)GetLastError());
        unlink(tmp);
        return false;
    }
    return true;
#else
    if (symlink(src, tmp) != 0) {
        fprintf(stderr, "tur install: symlink('%s' -> '%s') failed: %s\n",
                tmp, src, strerror(errno));
        return false;
    }
    if (rename(tmp, target) != 0) {
        fprintf(stderr, "tur install: rename '%s' -> '%s' failed: %s\n",
                tmp, target, strerror(errno));
        unlink(tmp);
        return false;
    }
    return true;
#endif
}

/* ------------------------------------------------------------------ */
/* tur install                                                         */
/* ------------------------------------------------------------------ */

static int install_usage(void) {
    fprintf(stderr,
        "usage:\n"
        "  tur install <url> [--ref <ref>] [--subdir <path>] [--name <name>] [--force] [--link]\n"
        "  tur install <path> --path [--name <name>] [--force] [--link]\n"
        "  tur install --link <spice>[/<bin>]\n"
        "  tur install --print-path-snippet\n"
        "\n"
        "  --force   take every bin name the spice declares, overwriting any file\n"
        "  --link    take every bin name from other installed spices (never a\n"
        "            file tur did not install); given an installed <spice>/<bin>\n"
        "            it only moves that one name, without fetching or building\n");
    return 1;
}

/* GS-M5: print the shell snippets that put bin_dir on $PATH and exit.
 * Mirrors the post-install hint shown by do_install_spec when bin_dir is
 * missing from $PATH, but standalone for users who dismissed the first-run
 * message. Always exits 0 (the snippet is correct regardless of current
 * PATH state — telling the user "you don't need this" is unhelpful when
 * they just asked for it). */
static int install_print_path_snippet(void) {
    char bin_dir[4096];
    if (!tur_global_bin_dir(bin_dir, sizeof(bin_dir))) {
        fprintf(stderr,
            "tur install: cannot resolve global bin dir\n");
        return 1;
    }
    printf("# bash/zsh: add to ~/.bashrc or ~/.zshrc\n");
    printf("export PATH=\"%s:$PATH\"\n", bin_dir);
    printf("\n");
    printf("# fish: add to ~/.config/fish/config.fish\n");
    printf("set -gx PATH %s $PATH\n", bin_dir);
    return 0;
}

/* Parameters for a single install/upgrade pipeline run. */
typedef struct InstallSpec {
    const char *src;          /* url or local path */
    const char *ref;          /* git ref (NULL for path / default HEAD) */
    const char *subdir;       /* monorepo subdir under repo root */
    const char *name_alias;   /* --name override */
    const char *install_dir;  /* upgrade: the recorded :dir to fetch into */
    bool        is_path;
    bool        force;
    bool        link_all;     /* --link: take every bin name (D4) */
    bool        upgrade;      /* `tur upgrade`: module conflicts only warn */
    const char *verb;         /* "install" or "upgrade" — used in messages */
} InstallSpec;

/* installed-spice-libraries D4: set the installed root up the way `tur fetch`
 * sets up a project -- its `:spices` closure into `<root>/spices/` with a
 * tur.lock, and its `:cmake-deps` (its own and its deps') configured and
 * built under `<root>/cmake/`.  It runs `tur fetch` in that directory, so an
 * installed root is exactly a fetched project and every project-mode resolver
 * (the include walk, collect_spice_aux_c, the cmake deps manifest, the REPL
 * image's --bundle-deps) finds its deps where it already looks.
 *
 * The bespoke walk this replaced fetched only the first level, into the
 * SHARED spices/ directory next to the installed spices, and never carried a
 * dep's :cmake-deps or :c-sources into the build.  A spice with neither
 * :spices nor :cmake-deps has nothing to fetch and is left untouched.
 * Exit 1 from `tur fetch` means only `:optional` deps failed; that installs. */
/* installed-spice-libraries D4: set the installed root up the way `tur fetch`
 * sets up a project -- its `:spices` closure into `<root>/spices/` with a
 * tur.lock, and its `:cmake-deps` (its own and its deps') configured and
 * built under `<root>/cmake/`.  It runs `tur fetch` in that directory, so an
 * installed root is exactly a fetched project and every project-mode resolver
 * (the include walk, collect_spice_aux_c, the cmake deps manifest, the REPL
 * image's --bundle-deps) finds its deps where it already looks.
 *
 * The bespoke walk this replaced fetched only the first level, into the
 * SHARED spices/ directory next to the installed spices, and never carried a
 * dep's :cmake-deps or :c-sources into the build.  A spice with neither
 * :spices nor :cmake-deps has nothing to fetch and is left untouched.
 * Exit 1 from `tur fetch` means only `:optional` deps failed; that installs. */
static bool inst_fetch_root(const char *verb, const char *self_exe,
                            const char *src_dir, const PkgManifest *m) {
    if (m->n_spices == 0 && m->n_cmake_deps == 0) return true;
    Buf cmd;
    buf_init(&cmd);
    buf_puts(&cmd, TUR_CD " ");
    bool ok = pkg_cmd_arg(&cmd, src_dir);
    buf_puts(&cmd, " && ");
    ok = pkg_cmd_arg(&cmd, self_exe) && ok;
    buf_puts(&cmd, " fetch");
    buf_putc(&cmd, '\0');
    char wrapped[16384];
    if (ok && tur_shell_command(cmd.data, wrapped, sizeof(wrapped)) != 0)
        ok = false;
    fprintf(stderr, "tur %s: fetching dependencies into %s ...\n", verb, src_dir);
    int rc = ok ? system(wrapped) : -1;
    buf_free(&cmd);
    if (rc != -1 && WIFEXITED(rc)) rc = WEXITSTATUS(rc);
    if (rc == 0 || rc == 1) return true;
    fprintf(stderr, "tur %s: fetching dependencies failed (rc=%d)\n", verb, rc);
    return false;
}

/* The same setup for a caller whose stdout is not its own -- a script-dep
 * cache entry is built while `tur run` is about to run the script, so the
 * child `tur fetch`'s progress goes to stderr with the rest of the notes. */
bool pkg_setup_fetched_project(const char *who, const char *src_dir,
                               const PkgManifest *m) {
    if (m->n_spices == 0 && m->n_cmake_deps == 0) return true;
    char self_exe[4096];
    if (!inst_self_exe(self_exe, sizeof(self_exe))) return false;
    Buf cmd;
    buf_init(&cmd);
    buf_puts(&cmd, TUR_CD " ");
    bool ok = pkg_cmd_arg(&cmd, src_dir);
    buf_puts(&cmd, " && ");
    ok = pkg_cmd_arg(&cmd, self_exe) && ok;
    buf_puts(&cmd, " fetch 1>&2");
    buf_putc(&cmd, '\0');
    char wrapped[16384];
    if (ok && tur_shell_command(cmd.data, wrapped, sizeof(wrapped)) != 0)
        ok = false;
    fprintf(stderr, "tur: %s: fetching dependencies into %s ...\n", who, src_dir);
    int rc = ok ? system(wrapped) : -1;
    buf_free(&cmd);
    if (rc != -1 && WIFEXITED(rc)) rc = WEXITSTATUS(rc);
    if (rc == 0 || rc == 1) return true;
    fprintf(stderr, "tur: %s: fetching dependencies failed (rc=%d)\n", who, rc);
    return false;
}

static int inst_cmp_str(const void *a, const void *b) {
    return strcmp(*(const char *const *)a, *(const char *const *)b);
}

/* installed-spice-libraries D6: every installed spice's src/ shares one
 * namespace in a manifest-less context, so a module path the new spice
 * provides must not already come from another installed spice.  Returns the
 * number of conflicts found (each reported on stderr); `force` turns them
 * into warnings and the result into 0. */
static int inst_check_module_conflicts(const char *verb, const char *install_name,
                                       const char *src_dir, char **mods, int n_mods,
                                       const TurState *state, bool force) {
    char self_real[4096];
    if (!realpath(src_dir, self_real)) snprintf(self_real, sizeof(self_real), "%s", src_dir);
    int n_conflicts = 0;
    for (int i = 0; i < state->n_entries; i++) {
        const TurStateEntry *e = &state->entries[i];
        if (!e->name || strcmp(e->name, install_name) == 0) continue;
        char other[4096];
        if (!tur_state_entry_src_dir(e, other, sizeof(other))) continue;
        char other_real[4096];
        if (realpath(other, other_real) && strcmp(other_real, self_real) == 0) continue;
        char **theirs = NULL;
        int n_theirs = tur_spice_modules(other, &theirs);
        for (int j = 0; j < n_mods; j++) {
            if (!bsearch(&mods[j], theirs, (size_t)n_theirs, sizeof(char *),
                         inst_cmp_str))
                continue;
            fprintf(stderr, "tur %s: %s: module '%s' is already provided by "
                            "installed spice '%s'\n",
                    verb, force ? "warning" : "error", mods[j], e->name);
            n_conflicts++;
        }
        tur_spice_modules_free(theirs, n_theirs);
    }
    if (n_conflicts > 0 && !force) {
        fprintf(stderr,
            "tur %s: refusing to install '%s': outside a project every installed\n"
            "  spice's modules share one namespace, so the import would be ambiguous.\n"
            "  `tur uninstall` the other spice, or pass --force (the spice whose name\n"
            "  sorts first then wins).\n", verb, install_name);
        return n_conflicts;
    }
    return 0;
}

bool pkg_build_spice_bin(const char *who, const char *src_dir,
                         const PkgManifest *m, const char *bin_name,
                         const char *entry, const char *out_path) {
    struct stat st;
    char entry_abs[4096];
    if (entry[0] == '/')
        snprintf(entry_abs, sizeof(entry_abs), "%s", entry);
    else
        snprintf(entry_abs, sizeof(entry_abs), "%s/%s", src_dir, entry);
    if (stat(entry_abs, &st) != 0) {
        fprintf(stderr, "%s: :bin entrypoint '%s' for binary '%s' not found.\n",
                who, entry_abs, bin_name);
        return false;
    }

    char self_exe[4096];
    inst_self_exe(self_exe, sizeof(self_exe));
    /* `tur build` invokes cc with autolink markers like
     * `src/runtime/hamt.c` that are interpreted relative to cwd. Find the
     * turmeric source root from the exe path and cd there before the
     * sub-build so those relative paths resolve. */
    char tur_root[4096];
    bool have_root = inst_find_turmeric_root(self_exe, tur_root, sizeof(tur_root));

    /* The include list project mode computes for this root: its own src/,
     * then the transitive :spices closure now on disk under <root>/spices/.
     * The link inputs (:c-sources, the cmake/ deps manifest) are found by
     * `tur build`'s own walk up from the entry file to this root. */
    const char **inc_dirs = NULL;
    int n_inc_dirs = 0;
    resolve_include_dirs_from_manifest(src_dir, m, /*include_own_src=*/true,
                                       &inc_dirs, &n_inc_dirs);

    Buf cmd;
    bool cmd_ok = true;
    buf_init(&cmd);
    if (have_root) {
        /* `cd` alone does not change DRIVE on Windows: from C:, `cd D:\x`
         * silently succeeds and leaves you on C:.  /d is the switch that
         * makes it mean what this code intends. */
        buf_puts(&cmd, TUR_CD " ");
        cmd_ok = pkg_cmd_arg(&cmd, tur_root) && cmd_ok;
        buf_puts(&cmd, " && ");
    }
    cmd_ok = pkg_cmd_arg(&cmd, self_exe) && cmd_ok;
    buf_puts(&cmd, " build ");
    cmd_ok = pkg_cmd_arg(&cmd, entry_abs) && cmd_ok;
    for (int j = 0; j < n_inc_dirs; j++) {
        buf_puts(&cmd, " -I ");
        cmd_ok = pkg_cmd_arg(&cmd, inc_dirs[j]) && cmd_ok;
    }
    buf_puts(&cmd, " -o ");
    cmd_ok = pkg_cmd_arg(&cmd, out_path) && cmd_ok;
    buf_putc(&cmd, '\0');
    for (int j = 0; j < n_inc_dirs; j++) free((void *)inc_dirs[j]);
    free((void *)inc_dirs);

    /* This command STARTS with a quoted program path (or with `cd` when
     * have_root), which is the case cmd.exe's "strip the first and last
     * quote" rule tears in half.  tur_shell_command adds the enclosing
     * pair it eats; on POSIX it is a copy. */
    char wrapped[16384];
    if (cmd_ok && tur_shell_command(cmd.data, wrapped, sizeof(wrapped)) != 0)
        cmd_ok = false;

    fprintf(stderr, "%s: building %s ...\n", who, bin_name);
    int rc = cmd_ok ? system(wrapped) : -1;
    buf_free(&cmd);
    if (rc != 0) {
        fprintf(stderr, "%s: build failed for '%s' (rc=%d)\n", who, bin_name, rc);
        return false;
    }
    if (stat(out_path, &st) != 0) {
        fprintf(stderr, "%s: build produced no output at '%s'\n", who, out_path);
        return false;
    }
    return true;
}

static int do_install_spec(const InstallSpec *spec) {
    const char *src         = spec->src;
    const char *ref         = spec->ref;
    const char *subdir      = spec->subdir;
    const char *name_alias  = spec->name_alias;
    bool        is_path     = spec->is_path;
    bool        force       = spec->force;
    const char *verb        = spec->verb ? spec->verb : "install";

    if (!tur_global_ensure_layout()) {
        fprintf(stderr, "tur %s: failed to create install layout\n", verb);
        return 1;
    }

    char spices_dir[4096];
    char bin_dir[4096];
    char state_path[4096];
    if (!tur_global_spices_dir(spices_dir, sizeof(spices_dir))) return 1;
    if (!tur_global_bin_dir(bin_dir, sizeof(bin_dir))) return 1;
    if (!tur_global_state_path(state_path, sizeof(state_path))) return 1;

    /* ---------------------------------------------------------------- */
    /* 1. Resolve source: clone (git) or reference in-place (--path).   */
    /* ---------------------------------------------------------------- */
    char install_dir[4096] = "";  /* under spices/ (empty for --path) */
    char src_dir[4096];           /* directory containing build.tur */
    char *resolved_sha = NULL;
    char derived_name[256];

    if (is_path) {
        char abs[4096];
        if (!realpath(src, abs)) {
            fprintf(stderr, "tur %s: cannot resolve '%s': %s\n",
                    verb, src, strerror(errno));
            return 1;
        }
        snprintf(src_dir, sizeof(src_dir), "%s", abs);
        /* derive name from build.tur :name later */
        inst_derive_name(abs, derived_name, sizeof(derived_name));
    } else {
        /* derive a temp name so we can pick the install dir */
        if (name_alias)
            snprintf(derived_name, sizeof(derived_name), "%s", name_alias);
        else
            inst_derive_name(src, derived_name, sizeof(derived_name));

        if (spec->install_dir && *spec->install_dir) {
            /* upgrade: fetch into the directory the entry records (D5), not
             * one re-derived from its name, which can differ. */
            snprintf(install_dir, sizeof(install_dir), "%s", spec->install_dir);
        } else {
            char ref_tag[128];
            snprintf(ref_tag, sizeof(ref_tag), "%s",
                     ref ? ref : "HEAD");
            /* sanitize ref_tag: replace '/' with '-' */
            for (char *p = ref_tag; *p; p++) if (*p == '/') *p = '-';

            snprintf(install_dir, sizeof(install_dir), "%s/%s-%s",
                     spices_dir, derived_name, ref_tag);
        }

        resolved_sha = pkg_git_fetch(src, ref, install_dir);
        if (!resolved_sha) {
            fprintf(stderr, "tur %s: git fetch failed\n", verb);
            return 1;
        }

        if (subdir)
            snprintf(src_dir, sizeof(src_dir), "%s/%s",
                     install_dir, subdir);
        else
            snprintf(src_dir, sizeof(src_dir), "%s", install_dir);
    }

    /* ---------------------------------------------------------------- */
    /* 2. Read build.tur (or build.tur.sweet)                            */
    /* ---------------------------------------------------------------- */
    char build_path[4096];
    if (!pkg_resolve_manifest_path(src_dir, build_path, sizeof(build_path))) {
        fprintf(stderr, "tur %s: no build.tur or build.tur.sweet at '%s'\n",
                verb, src_dir);
        free(resolved_sha);
        return 1;
    }

    struct stat st;
    PkgManifest m;
    memset(&m, 0, sizeof(m));
    if (!pkg_manifest_read(build_path, &m)) {
        fprintf(stderr, "tur %s: cannot parse '%s'\n", verb, build_path);
        free(resolved_sha);
        return 1;
    }

    /* installed-spice-libraries D3: a spice with modules is a library, with or
     * without :bin entries.  Only a spice with neither has nothing to offer. */
    char own_src[4096];
    snprintf(own_src, sizeof(own_src), "%s/src", src_dir);
    bool has_src = (stat(own_src, &st) == 0 && S_ISDIR(st.st_mode));
    if (m.n_bins == 0 && !has_src) {
        fprintf(stderr,
            "tur %s: spice '%s' declares no :bin entries and has no src/; "
            "nothing to install.\n",
            verb, m.name ? m.name : derived_name);
        pkg_manifest_free(&m);
        free(resolved_sha);
        return 1;
    }

    /* Pick the canonical install name (priority: --name > manifest :name > derived) */
    const char *install_name =
        name_alias ? name_alias :
        (m.name && *m.name) ? m.name : derived_name;

    /* ---------------------------------------------------------------- */
    /* 3. Pre-flight conflict checks: bin_dir/<name> entries, and the    */
    /*    module paths this spice adds to the installed namespace (D6). */
    /* ---------------------------------------------------------------- */
    TurState pre_state;
    tur_state_read(state_path, &pre_state);
    /* D2: per bin, link it or leave it shadowed; `displaced` names the spice
     * a taken name came from (--force / --link). */
    BinPlan *plans = m.n_bins ? (BinPlan *)calloc((size_t)m.n_bins, sizeof(BinPlan)) : NULL;
    char **displaced = m.n_bins ? (char **)calloc((size_t)m.n_bins, sizeof(char *)) : NULL;
    char **holders   = m.n_bins ? (char **)calloc((size_t)m.n_bins, sizeof(char *)) : NULL;
    bool refused = m.n_bins && (!plans || !displaced || !holders);
    for (int i = 0; i < m.n_bins && !refused; i++) {
        char binfile[4096], target[4096];
        inst_bin_name(m.bin_names[i], binfile, sizeof(binfile));
        snprintf(target, sizeof(target), "%s/%s", bin_dir, binfile);
        const char *holder = NULL;
        plans[i] = inst_plan_bin(verb, target, m.bin_names[i], install_name,
                                 &pre_state, spices_dir,
                                 install_dir[0] ? install_dir : src_dir,
                                 force || spec->link_all, force, &holder);
        if (plans[i] == BIN_REFUSE) refused = true;
        else if (holder && plans[i] == BIN_LINK) displaced[i] = tur_strdup(holder);
        else if (holder) holders[i] = tur_strdup(holder);
    }
    /* An upgrade drops the bins the manifest no longer declares: the bare
     * name goes only if this spice held it (D5). */
    char **dropped = NULL;
    int n_dropped = 0;
    TurStateEntry *prev = tur_state_find(&pre_state, install_name);
    for (int j = 0; prev && j < prev->n_bin && !refused; j++) {
        bool kept = false;
        for (int i = 0; i < m.n_bins && !kept; i++)
            kept = strcmp(prev->bin[j], m.bin_names[i]) == 0;
        if (kept || (prev->bin_linked && !prev->bin_linked[j])) continue;
        char **nd = (char **)realloc(dropped, (size_t)(n_dropped + 1) * sizeof(char *));
        if (!nd) continue;
        dropped = nd;
        dropped[n_dropped++] = tur_strdup(prev->bin[j]);
    }
    int n_mod_conflicts = 0;
    if (!refused) {
        char **mods = NULL;
        int n_mods = tur_spice_modules(src_dir, &mods);
        n_mod_conflicts = inst_check_module_conflicts(verb, install_name, src_dir,
                                                      mods, n_mods, &pre_state,
                                                      force || spec->upgrade);
        tur_spice_modules_free(mods, n_mods);
    }
    tur_state_free(&pre_state);
    int rc = 1;
    TurState state;
    memset(&state, 0, sizeof(state));
    if (refused || n_mod_conflicts > 0) goto done;

    char self_exe[4096];
    inst_self_exe(self_exe, sizeof(self_exe));

    /* ---------------------------------------------------------------- */
    /* 4. Set the root up as a fetched project (D4).                    */
    /* ---------------------------------------------------------------- */
    if (!inst_fetch_root(verb, self_exe, src_dir, &m)) goto done;

    /* ---------------------------------------------------------------- */
    /* 5. Build each declared binary -- a shadowed one too: it runs as  */
    /*    `tur <spice>/<bin>`.  Output goes to <src_dir>/build/<bin>.   */
    /* ---------------------------------------------------------------- */
    char build_out_dir[4096];
    snprintf(build_out_dir, sizeof(build_out_dir), "%s/build", src_dir);
    if (m.n_bins > 0 && !inst_mkdirp(build_out_dir)) {
        fprintf(stderr, "tur %s: cannot create '%s'\n", verb, build_out_dir);
        goto done;
    }

    for (int i = 0; i < m.n_bins; i++) {
        char bin_out[4096];
        snprintf(bin_out, sizeof(bin_out), "%s/%s",
                 build_out_dir, m.bin_names[i]);
        char who[64];
        snprintf(who, sizeof(who), "tur %s", verb);
        if (!pkg_build_spice_bin(who, src_dir, &m, m.bin_names[i],
                                 m.bin_paths[i], bin_out))
            goto done;
    }

    /* ---------------------------------------------------------------- */
    /* 6. Link each binary this spice holds into bin_dir.               */
    /* ---------------------------------------------------------------- */
    for (int i = 0; i < m.n_bins; i++) {
        if (plans[i] != BIN_LINK) continue;
        char binfile[4096], target[4096], source[4096];
        inst_bin_name(m.bin_names[i], binfile, sizeof(binfile));
        snprintf(target, sizeof(target), "%s/%s", bin_dir, binfile);
        /* The BUILD artifact keeps the bare name -- `tur build -o` writes
         * exactly the path it is given -- so only the installed entry gets the
         * platform suffix. */
        snprintf(source, sizeof(source), "%s/%s",
                 build_out_dir, m.bin_names[i]);
        if (!inst_place_bin(source, target)) goto done;
    }
    for (int k = 0; k < n_dropped; k++) {
        char binfile[4096], target[4096];
        inst_bin_name(dropped[k], binfile, sizeof(binfile));
        snprintf(target, sizeof(target), "%s/%s", bin_dir, binfile);
        struct stat dst;
        if (lstat(target, &dst) == 0 && unlink(target) == 0)
            fprintf(stderr, "tur %s: removed %s (no longer declared)\n",
                    verb, target);
    }

    /* ---------------------------------------------------------------- */
    /* 7. Update state.tur                                              */
    /* ---------------------------------------------------------------- */
    tur_state_read(state_path, &state);
    TurStateEntry *e = tur_state_upsert(&state, install_name);
    if (!e) {
        fprintf(stderr, "tur %s: oom updating state.tur\n", verb);
        goto done;
    }
    if (is_path)        e->path     = tur_strdup(src_dir);
    if (src && !is_path)e->url      = tur_strdup(src);
    if (ref)            e->ref      = tur_strdup(ref);
    if (subdir)         e->subdir   = tur_strdup(subdir);
    if (resolved_sha)   e->resolved = tur_strdup(resolved_sha);
    if (m.version)      e->version  = tur_strdup(m.version);
    /* D5: record where the install lives, so no reader has to guess it from
     * the entry name (which differs from the clone's name whenever the
     * manifest's :name is not the URL's last segment). */
    if (!is_path) {
        e->dir = tur_strdup(install_dir);
        if (subdir) e->src_dir = tur_strdup(src_dir);
    }
    e->installed_at = tur_strdup(inst_iso_now());
#ifdef TUR_VERSION
    e->built_with = tur_strdup(TUR_VERSION);
#endif
    for (int i = 0; i < m.n_bins; i++)
        tur_state_entry_add_bin(e, m.bin_names[i], plans[i] == BIN_LINK);
    /* A name taken from another spice leaves that spice shadowed. */
    for (int i = 0; i < m.n_bins; i++) {
        if (!displaced[i]) continue;
        TurStateEntry *h = tur_state_find(&state, displaced[i]);
        int j = tur_state_entry_bin_index(h, m.bin_names[i]);
        if (j >= 0 && h->bin_linked) h->bin_linked[j] = false;
    }

    if (!tur_state_write(state_path, &state)) goto done;

    /* ---------------------------------------------------------------- */
    /* Output + PATH-check                                              */
    /* ---------------------------------------------------------------- */
    const char *past = (strcmp(verb, "upgrade") == 0) ? "upgraded" : "installed";
    fprintf(stderr, "tur %s: %s '%s'", verb, past, install_name);
    if (m.version) fprintf(stderr, " (%s)", m.version);
    if (m.n_bins == 0) fprintf(stderr, " [library]");
    fprintf(stderr, "\n");
    int n_linked = 0;
    for (int i = 0; i < m.n_bins; i++) {
        if (plans[i] != BIN_LINK) continue;
        n_linked++;
        char binfile[4096];
        inst_bin_name(m.bin_names[i], binfile, sizeof(binfile));
        fprintf(stderr, "  %s/%s", bin_dir, binfile);
        if (displaced[i]) {
            char q[512];
            inst_qualified(displaced[i], m.bin_names[i], q, sizeof(q));
            fprintf(stderr, "  (taken from '%s'; `tur %s` still runs theirs)",
                    displaced[i], q);
        }
        fprintf(stderr, "\n");
    }
    /* D2: one warning per shadowed bin. */
    for (int i = 0; i < m.n_bins; i++) {
        if (plans[i] != BIN_SHADOW) continue;
        char q[512];
        inst_qualified(install_name, m.bin_names[i], q, sizeof(q));
        if (holders[i])
            fprintf(stderr,
                "tur %s: warning: '%s' is already provided by installed spice '%s';\n"
                "  %s's copy is installed but not linked. Run it as `tur %s`,\n"
                "  or make it the default with `tur install --link %s`.\n",
                verb, m.bin_names[i], holders[i], install_name, q, q);
        else
            fprintf(stderr,
                "tur %s: note: %s's '%s' is installed but not linked.\n"
                "  Run it as `tur %s`, or make it the default with "
                "`tur install --link %s`.\n",
                verb, install_name, m.bin_names[i], q, q);
    }
    if (m.n_bins == 0) {
        fprintf(stderr, "  modules: %s/src\n"
                        "  importable from any file outside a project\n",
                src_dir);
    }

    if (n_linked > 0 && !tur_global_bin_on_path(bin_dir)) {
        fprintf(stderr,
            "\nNote: %s is not on $PATH.\n"
            "  bash/zsh: export PATH=\"%s:$PATH\"\n"
            "  fish:     set -gx PATH %s $PATH\n",
            bin_dir, bin_dir, bin_dir);
    }
    rc = 0;

done:
    for (int k = 0; k < m.n_bins; k++) {
        if (displaced) free(displaced[k]);
        if (holders)   free(holders[k]);
    }
    for (int k = 0; k < n_dropped; k++) free(dropped[k]);
    free(plans);
    free(displaced);
    free(holders);
    free(dropped);
    tur_state_free(&state);
    pkg_manifest_free(&m);
    free(resolved_sha);
    return rc;
}

/* namespaced-spice-binaries D4: `tur install --link <spice>[/<bin>]` for an
 * INSTALLED spice -- point bin_dir/<bin> at its build and flip the registry
 * flags.  No fetch, no build.  Without `/<bin>` every bin name the spice
 * declares moves.  Like --link on a source install it takes names from other
 * installed spices only: a file no installed spice claims is the user's own
 * and is left alone. */
static int inst_link_move(TurState *state, TurStateEntry *e, const char *bin,
                          const char *state_path) {
    char bin_dir[4096];
    if (!tur_global_bin_dir(bin_dir, sizeof(bin_dir))) return 1;
    if (e->n_bin == 0) {
        fprintf(stderr, "tur install: installed spice '%s' provides no binaries; "
                        "there is nothing to link\n", e->name);
        return 1;
    }
    int only = -1;
    if (bin) {
        char want[512];
        if (strncmp(bin, "tur-", 4) == 0) snprintf(want, sizeof(want), "%s", bin);
        else                              snprintf(want, sizeof(want), "tur-%s", bin);
        only = tur_state_entry_bin_index(e, want);
        if (only < 0) {
            fprintf(stderr, "tur install: installed spice '%s' has no binary '%s'; "
                            "it provides:\n", e->name, want);
            for (int j = 0; j < e->n_bin; j++)
                fprintf(stderr, "  %s\n", e->bin[j]);
            return 1;
        }
    }
    char src_dir[4096];
    if (!tur_state_entry_src_dir(e, src_dir, sizeof(src_dir))) {
        fprintf(stderr, "tur install: the source tree of '%s' is missing (%s) -- "
                        "re-run `tur install`\n", e->name, src_dir);
        return 1;
    }
    int failures = 0;
    for (int j = 0; j < e->n_bin; j++) {
        if (only >= 0 && j != only) continue;
        const char *b = e->bin[j];
        char binfile[4096], target[4096], source[4096];
        inst_bin_name(b, binfile, sizeof(binfile));
        snprintf(target, sizeof(target), "%s/%s", bin_dir, binfile);
        snprintf(source, sizeof(source), "%s/build/%s", src_dir, b);
        struct stat st;
        bool linked = !e->bin_linked || e->bin_linked[j];
        if (linked && lstat(target, &st) == 0) {
            fprintf(stderr, "tur install: '%s' already runs %s's\n", b, e->name);
            continue;
        }
        TurStateEntry *holder = tur_state_bin_holder(state, b, e->name);
        if (!holder && !linked && lstat(target, &st) == 0) {
            fprintf(stderr,
                "tur install: refusing to overwrite existing '%s' (not owned by "
                "tur). Remove it first.\n", target);
            failures++;
            continue;
        }
        if (stat(source, &st) != 0) {
            fprintf(stderr, "tur install: the installed '%s' has no built %s at %s "
                            "-- `tur upgrade %s` rebuilds it\n",
                    e->name, b, source, e->name);
            failures++;
            continue;
        }
        if (!inst_place_bin(source, target)) { failures++; continue; }
        if (e->bin_linked) e->bin_linked[j] = true;
        if (holder) {
            int hj = tur_state_entry_bin_index(holder, b);
            if (hj >= 0 && holder->bin_linked) holder->bin_linked[hj] = false;
            char q[512];
            inst_qualified(holder->name, b, q, sizeof(q));
            fprintf(stderr, "tur install: '%s' now runs %s's (was %s's; "
                            "`tur %s` still runs that one)\n",
                    b, e->name, holder->name, q);
        } else {
            fprintf(stderr, "tur install: '%s' now runs %s's\n", b, e->name);
        }
    }
    if (!tur_state_write(state_path, state)) return 1;
    return failures == 0 ? 0 : 1;
}

/* `beta` or `beta/foo` naming an installed spice: the move-only form of
 * --link.  Returns -1 when `arg` names no installed spice (it is a source). */
static int inst_try_link_installed(const char *arg) {
    char name[512];
    snprintf(name, sizeof(name), "%s", arg);
    char *slash = strchr(name, '/');
    const char *bin = NULL;
    if (slash) {
        if (strchr(slash + 1, '/') || !slash[1] || slash == name) return -1;
        *slash = '\0';
        bin = slash + 1;
    }
    char state_path[4096];
    if (!tur_global_state_path(state_path, sizeof(state_path))) return -1;
    TurState state;
    tur_state_read(state_path, &state);
    TurStateEntry *e = tur_state_find_spice(&state, name);
    int rc = e ? inst_link_move(&state, e, bin, state_path) : -1;
    tur_state_free(&state);
    return rc;
}

/* Thin argv parser; builds an InstallSpec and runs the pipeline. */
int cmd_pkg_install(int argc, char **argv) {
    /* --print-path-snippet short-circuits before InstallSpec parsing: it
     * takes no other flags and never touches state.tur. */
    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "--print-path-snippet") == 0)
            return install_print_path_snippet();
    }

    InstallSpec spec = {0};
    spec.verb = "install";
    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0)
            return install_usage();
        else if (strcmp(argv[i], "--ref")    == 0 && i + 1 < argc) spec.ref        = argv[++i];
        else if (strcmp(argv[i], "--subdir") == 0 && i + 1 < argc) spec.subdir     = argv[++i];
        else if (strcmp(argv[i], "--name")   == 0 && i + 1 < argc) spec.name_alias = argv[++i];
        else if (strcmp(argv[i], "--path")   == 0) spec.is_path = true;
        else if (strcmp(argv[i], "--force")  == 0) spec.force   = true;
        else if (strcmp(argv[i], "--link")   == 0) spec.link_all = true;
        else if (argv[i][0] != '-') {
            if (spec.src) {
                fprintf(stderr, "tur install: unexpected argument '%s'\n", argv[i]);
                return 1;
            }
            spec.src = argv[i];
        } else {
            fprintf(stderr, "tur install: unknown flag '%s'\n", argv[i]);
            return install_usage();
        }
    }
    if (!spec.src) return install_usage();
    /* D4: `--link` on something already installed only moves names.  A
     * --path argument is always a source; a URL never names an install. */
    if (spec.link_all && !spec.is_path && !spec.ref && !spec.subdir &&
        !spec.name_alias && !strstr(spec.src, "://") &&
        strncmp(spec.src, "git@", 4) != 0) {
        int rc = inst_try_link_installed(spec.src);
        if (rc >= 0) return rc;
        struct stat sst;
        if (strchr(spec.src, '/') && !strchr(spec.src, '.') &&
            !strchr(strchr(spec.src, '/') + 1, '/') && stat(spec.src, &sst) != 0) {
            char nm[512];
            snprintf(nm, sizeof(nm), "%.*s",
                     (int)(strchr(spec.src, '/') - spec.src), spec.src);
            fprintf(stderr, "tur install: no installed spice named '%s'\n", nm);
            return 1;
        }
    }
    return do_install_spec(&spec);
}

/* ------------------------------------------------------------------ */
/* tur uninstall                                                       */
/* ------------------------------------------------------------------ */

static int uninstall_usage(void) {
    fprintf(stderr, "usage: tur uninstall <name>\n");
    return 1;
}

int cmd_pkg_uninstall(int argc, char **argv) {
    const char *name = NULL;
    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0)
            return uninstall_usage();
        if (argv[i][0] != '-') {
            if (name) return uninstall_usage();
            name = argv[i];
        } else {
            return uninstall_usage();
        }
    }
    if (!name) return uninstall_usage();

    char spices_dir[4096];
    char bin_dir[4096];
    char state_path[4096];
    if (!tur_global_spices_dir(spices_dir, sizeof(spices_dir))) return 1;
    if (!tur_global_bin_dir(bin_dir, sizeof(bin_dir))) return 1;
    if (!tur_global_state_path(state_path, sizeof(state_path))) return 1;

    TurState state;
    tur_state_read(state_path, &state);
    TurStateEntry *e = tur_state_find(&state, name);

    /* Fallback: older list output rendered `name-version` with no separator,
     * so users sometimes copy the whole string. If the exact name misses and
     * the input ends in `-<digit>...`, try the prefix. */
    char *stripped = NULL;
    if (!e) {
        const char *dash = strrchr(name, '-');
        if (dash && dash != name && isdigit((unsigned char)dash[1])) {
            size_t pre_len = (size_t)(dash - name);
            stripped = (char *)malloc(pre_len + 1);
            if (stripped) {
                memcpy(stripped, name, pre_len);
                stripped[pre_len] = '\0';
                TurStateEntry *e2 = tur_state_find(&state, stripped);
                if (e2 && e2->version &&
                    strcmp(e2->version, dash + 1) == 0) {
                    e = e2;
                    name = stripped;
                }
            }
        }
    }

    if (!e) {
        fprintf(stderr, "tur uninstall: '%s' is not installed\n", name);
        free(stripped);
        tur_state_free(&state);
        return 1;
    }

    /* Remove bin_dir/<name> for each bin this spice HOLDS (D1's :linked; a
     * shadowed bin's name belongs to another spice).  The symlink check below
     * is defensive, not the ownership decision -- the registry has said this
     * spice owns these names.  Where a symlink was not available the entry is
     * a copy (inst_place_bin), which readlink cannot vouch for, so a plain
     * file at a path the registry claims is removed too.
     *
     * namespaced-spice-binaries D5: nothing is promoted into a freed name.
     * The user chose which tool it meant; uninstall cannot know which of the
     * remaining providers they would pick, so it says what is left. */
    for (int i = 0; i < e->n_bin; i++) {
        if (e->bin_linked && !e->bin_linked[i]) continue;
        char binfile[4096], target[4096];
        inst_bin_name(e->bin[i], binfile, sizeof(binfile));
        snprintf(target, sizeof(target), "%s/%s", bin_dir, binfile);
        struct stat lst;
        if (lstat(target, &lst) != 0) continue;
        bool ours = false;
        if (S_ISLNK(lst.st_mode)) {
            char rb[4096];
            ssize_t n = readlink(target, rb, sizeof(rb) - 1);
            if (n > 0) {
                rb[n] = '\0';
                ours = strncmp(rb, spices_dir, strlen(spices_dir)) == 0 ||
                       (e->path && strncmp(rb, e->path, strlen(e->path)) == 0);
            }
        } else {
            ours = true;   /* a copy we placed; the registry is the record */
        }
        if (!ours) continue;
        if (unlink(target) != 0) {
            fprintf(stderr, "tur uninstall: cannot remove '%s': %s\n",
                    target, strerror(errno));
            continue;
        }
        fprintf(stderr, "tur uninstall: removed %s\n", target);
        for (int k = 0; k < state.n_entries; k++) {
            const TurStateEntry *o = &state.entries[k];
            if (o == e || tur_state_entry_bin_index(o, e->bin[i]) < 0) continue;
            char q[512];
            inst_qualified(o->name, e->bin[i], q, sizeof(q));
            fprintf(stderr,
                "tur uninstall: note: '%s' is now unlinked; %s still provides it.\n"
                "  Run it as `tur %s`, or make it the default with\n"
                "  `tur install --link %s`.\n",
                e->bin[i], o->name, q, q);
        }
    }

    /* Remove the source tree (non-path installs only).
     *
     * installed-spice-libraries D5: exactly the root the entry records (or,
     * for an entry that predates `:dir`, the one reconstructed from its name)
     * -- never every `<name>-*` directory under spices/, which also took
     * `plot-extras-HEAD/` with `tur uninstall plot`.  The root must be a
     * direct child of spices/: a hand-edited `:dir` cannot point the delete
     * anywhere else. */
    char root[4096];
    if (tur_state_entry_root_dir(e, root, sizeof(root))) {
        size_t sl = strlen(spices_dir);
        const char *rest = root + sl;
        bool inside = strncmp(root, spices_dir, sl) == 0 && rest[0] == '/' &&
                      rest[1] && !strchr(rest + 1, '/') &&
                      strcmp(rest + 1, "..") != 0 && strcmp(rest + 1, ".") != 0;
        struct stat rst;
        if (!inside) {
            fprintf(stderr, "tur uninstall: not removing '%s': it is not an "
                            "install root under %s\n", root, spices_dir);
        } else if (stat(root, &rst) == 0 && S_ISDIR(rst.st_mode)) {
            if (pkg_rm_rf(root))
                fprintf(stderr, "tur uninstall: removed %s\n", root);
        }
    }

    tur_state_remove(&state, name);
    if (!tur_state_write(state_path, &state)) {
        tur_state_free(&state);
        free(stripped);
        return 1;
    }
    fprintf(stderr, "tur uninstall: '%s' removed\n", name);
    tur_state_free(&state);
    free(stripped);
    return 0;
}

/* ------------------------------------------------------------------ */
/* tur list                                                            */
/* ------------------------------------------------------------------ */

/* The source dir on disk for an installed entry (D5: the recorded one,
 * else the pre-D5 reconstruction).  Returns true if the dir exists. */
static bool inst_entry_src_dir(const TurStateEntry *e,
                                const char *spices_dir,
                                char *out, size_t out_sz) {
    (void)spices_dir;
    return tur_state_entry_src_dir(e, out, out_sz);
}

static int list_usage(void) {
    fprintf(stderr,
        "usage: tur list [--verbose|--json|--outdated]\n");
    return 1;
}

/* Forward decls: defined below. */
static char *upgrade_ls_remote(const char *url, const char *ref);
static void  inst_json_str(Buf *b, const char *s);

/* GS-M5: --outdated mode. Walk every non-path state entry, query the
 * remote SHA for its url@ref, and flag entries where the remote moved
 * past the resolved SHA on disk. --path entries are skipped (there is no
 * "remote" to compare against). Always exits 0 — this command is
 * informational; callers script around grep, not exit codes. */
static int list_print_outdated(const TurState *state, bool as_json) {
    int n_outdated = 0;
    int n_checked  = 0;
    int n_skipped  = 0;

    if (as_json) {
        Buf b; buf_init(&b);
        buf_puts(&b, "{\n  \"outdated\": [");
        bool first = true;
        for (int i = 0; i < state->n_entries; i++) {
            const TurStateEntry *e = &state->entries[i];
            if (e->path || !e->url) { n_skipped++; continue; }
            char *sha = upgrade_ls_remote(e->url, e->ref);
            n_checked++;
            if (!sha) continue;
            if (e->resolved && strcmp(sha, e->resolved) == 0) {
                free(sha);
                continue;
            }
            if (!first) buf_putc(&b, ',');
            first = false;
            buf_puts(&b, "\n    {\"name\": \"");
            inst_json_str(&b, e->name); buf_puts(&b, "\", \"url\": \"");
            inst_json_str(&b, e->url); buf_puts(&b, "\", \"ref\": \"");
            inst_json_str(&b, e->ref); buf_puts(&b, "\", \"resolved\": \"");
            inst_json_str(&b, e->resolved); buf_puts(&b, "\", \"remote\": \"");
            inst_json_str(&b, sha); buf_puts(&b, "\"}");
            n_outdated++;
            free(sha);
        }
        buf_printf(&b, "\n  ],\n  \"checked\": %d,\n  \"skipped\": %d\n}\n",
                   n_checked, n_skipped);
        buf_putc(&b, '\0');
        fputs(b.data, stdout);
        buf_free(&b);
        return 0;
    }

    for (int i = 0; i < state->n_entries; i++) {
        const TurStateEntry *e = &state->entries[i];
        if (e->path || !e->url) { n_skipped++; continue; }
        char *sha = upgrade_ls_remote(e->url, e->ref);
        n_checked++;
        if (!sha) {
            printf("  %-20s %s @ %s: unable to query remote\n",
                   e->name ? e->name : "(unnamed)",
                   e->url, e->ref ? e->ref : "HEAD");
            continue;
        }
        if (e->resolved && strcmp(sha, e->resolved) == 0) {
            free(sha);
            continue;
        }
        printf("  %-20s %.12s -> %.12s   (%s @ %s)\n",
               e->name ? e->name : "(unnamed)",
               e->resolved ? e->resolved : "(unknown)",
               sha,
               e->url, e->ref ? e->ref : "HEAD");
        n_outdated++;
        free(sha);
    }
    if (n_outdated == 0) {
        printf("All %d checked spice%s up to date",
               n_checked, n_checked == 1 ? "" : "s");
        if (n_skipped) printf(" (%d skipped: --path or no url)", n_skipped);
        printf(".\n");
    } else {
        printf("\n%d spice%s outdated (%d checked, %d skipped).\n",
               n_outdated, n_outdated == 1 ? "" : "s",
               n_checked, n_skipped);
        printf("Run `tur upgrade --all` to update.\n");
    }
    return 0;
}

/* JSON-escape a string into buf (no surrounding quotes). NULL becomes
 * empty. */
static void inst_json_str(Buf *b, const char *s) {
    if (!s) return;
    for (const char *p = s; *p; p++) {
        unsigned char c = (unsigned char)*p;
        if (c == '"' || c == '\\') { buf_putc(b, '\\'); buf_putc(b, c); }
        else if (c == '\n') { buf_puts(b, "\\n"); }
        else if (c == '\r') { buf_puts(b, "\\r"); }
        else if (c == '\t') { buf_puts(b, "\\t"); }
        else if (c < 0x20) { buf_printf(b, "\\u%04x", c); }
        else { buf_putc(b, c); }
    }
}

static void list_print_json(const TurState *state,
                             const char *bin_dir,
                             const char *spices_dir,
                             bool path_active) {
    Buf b; buf_init(&b);
    buf_puts(&b, "{\n");
    buf_printf(&b, "  \"bin_dir\": \"");
    inst_json_str(&b, bin_dir); buf_puts(&b, "\",\n");
    buf_printf(&b, "  \"spices_dir\": \"");
    inst_json_str(&b, spices_dir); buf_puts(&b, "\",\n");
    buf_printf(&b, "  \"path_active\": %s,\n",
               path_active ? "true" : "false");
    buf_puts(&b, "  \"installed\": [");
    for (int i = 0; i < state->n_entries; i++) {
        const TurStateEntry *e = &state->entries[i];
        char src_dir[4096];
        bool have_src = inst_entry_src_dir(e, spices_dir,
                                            src_dir, sizeof(src_dir));
        if (i) buf_putc(&b, ',');
        buf_puts(&b, "\n    {\n");
        buf_printf(&b, "      \"name\": \"");
        inst_json_str(&b, e->name); buf_puts(&b, "\",\n");
        buf_printf(&b, "      \"version\": \"");
        inst_json_str(&b, e->version); buf_puts(&b, "\",\n");
        buf_printf(&b, "      \"url\": \"");
        inst_json_str(&b, e->url); buf_puts(&b, "\",\n");
        buf_printf(&b, "      \"ref\": \"");
        inst_json_str(&b, e->ref); buf_puts(&b, "\",\n");
        buf_printf(&b, "      \"resolved\": \"");
        inst_json_str(&b, e->resolved); buf_puts(&b, "\",\n");
        buf_printf(&b, "      \"subdir\": \"");
        inst_json_str(&b, e->subdir); buf_puts(&b, "\",\n");
        buf_printf(&b, "      \"path\": \"");
        inst_json_str(&b, e->path); buf_puts(&b, "\",\n");
        buf_printf(&b, "      \"installed_at\": \"");
        inst_json_str(&b, e->installed_at); buf_puts(&b, "\",\n");
        buf_printf(&b, "      \"source_dir\": \"");
        if (have_src) inst_json_str(&b, src_dir);
        buf_puts(&b, "\",\n");
        buf_puts(&b, "      \"bin\": [");
        for (int j = 0; j < e->n_bin; j++) {
            if (j) buf_putc(&b, ',');
            buf_putc(&b, '"');
            inst_json_str(&b, e->bin[j]);
            buf_putc(&b, '"');
        }
        buf_puts(&b, "],\n");
        /* namespaced-spice-binaries D4: who holds each bare name. */
        buf_puts(&b, "      \"bins\": [");
        for (int j = 0; j < e->n_bin; j++) {
            bool linked = !e->bin_linked || e->bin_linked[j];
            if (j) buf_putc(&b, ',');
            buf_puts(&b, "{\"name\": \"");
            inst_json_str(&b, e->bin[j]);
            buf_printf(&b, "\", \"linked\": %s", linked ? "true" : "false");
            const TurStateEntry *h = linked ? NULL
                : tur_state_bin_holder((TurState *)state, e->bin[j], e->name);
            if (h) {
                buf_puts(&b, ", \"held_by\": \"");
                inst_json_str(&b, h->name);
                buf_putc(&b, '"');
            }
            buf_putc(&b, '}');
        }
        buf_puts(&b, "],\n");
        /* installed-spice-libraries IL1: a "tool" has :bin entries (and is
         * a library too -- its modules are importable); a "library" has
         * modules only. */
        buf_printf(&b, "      \"kind\": \"%s\",\n",
                   e->n_bin > 0 ? "tool" : "library");
        buf_puts(&b, "      \"modules\": [");
        if (have_src) {
            char **mods = NULL;
            int n_mods = tur_spice_modules(src_dir, &mods);
            for (int j = 0; j < n_mods; j++) {
                if (j) buf_putc(&b, ',');
                buf_putc(&b, '"');
                inst_json_str(&b, mods[j]);
                buf_putc(&b, '"');
            }
            tur_spice_modules_free(mods, n_mods);
        }
        buf_puts(&b, "],\n");
        buf_puts(&b, "      \"exports\": [");
        if (have_src) {
            char manifest[4096];
            PkgManifest m; memset(&m, 0, sizeof(m));
            if (pkg_resolve_manifest_path(src_dir, manifest, sizeof(manifest)) &&
                pkg_manifest_read(manifest, &m)) {
                for (int j = 0; j < m.n_exports; j++) {
                    if (j) buf_putc(&b, ',');
                    buf_putc(&b, '"');
                    inst_json_str(&b, m.exports[j]);
                    buf_putc(&b, '"');
                }
                pkg_manifest_free(&m);
            }
        }
        buf_puts(&b, "]\n    }");
    }
    buf_puts(&b, "\n  ]\n}\n");
    buf_putc(&b, '\0');
    fputs(b.data, stdout);
    buf_free(&b);
}

/* Print a comma-separated list of strings, wrapping at ~70 chars. The
 * first line is prefixed by `first_pfx`; continuation lines by
 * `cont_pfx`. */
static void list_print_wrapped(const char *first_pfx, const char *cont_pfx,
                                char **items, int n) {
    if (n == 0) { printf("%s(none)\n", first_pfx); return; }
    int col = 0;
    const char *pfx = first_pfx;
    for (int i = 0; i < n; i++) {
        const char *s = items[i];
        int slen = (int)strlen(s);
        const char *sep = (i + 1 < n) ? ", " : "";
        if (col == 0) {
            printf("%s", pfx);
            col = (int)strlen(pfx);
            pfx = cont_pfx;
        } else if (col + slen + 2 > 78) {
            printf("\n%s", cont_pfx);
            col = (int)strlen(cont_pfx);
        }
        printf("%s%s", s, sep);
        col += slen + (int)strlen(sep);
    }
    printf("\n");
}

int cmd_pkg_list(int argc, char **argv) {
    bool verbose   = false;
    /* `--json` is a global flag (stripped from argv by the driver); honor it. */
    bool as_json   = use_json_output;
    bool installed = false; /* --installed alias accepted, no-op */
    bool outdated  = false;
    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0)
            return list_usage();
        else if (strcmp(argv[i], "--verbose")   == 0) verbose = true;
        else if (strcmp(argv[i], "--installed") == 0) installed = true;
        else if (strcmp(argv[i], "--outdated")  == 0) outdated = true;
        else return list_usage();
    }
    (void)installed;
    if (verbose && as_json) {
        fprintf(stderr,
            "tur list: --verbose and --json are mutually exclusive\n");
        return 1;
    }
    if (outdated && verbose) {
        fprintf(stderr,
            "tur list: --outdated and --verbose are mutually exclusive\n");
        return 1;
    }

    char spices_dir[4096];
    char bin_dir[4096];
    char state_path[4096];
    if (!tur_global_spices_dir(spices_dir, sizeof(spices_dir))) return 1;
    if (!tur_global_bin_dir(bin_dir, sizeof(bin_dir))) return 1;
    if (!tur_global_state_path(state_path, sizeof(state_path))) return 1;

    TurState state;
    if (!tur_state_read(state_path, &state)) {
        fprintf(stderr, "tur list: cannot read state.tur\n");
        return 1;
    }

    bool path_active = tur_global_bin_on_path(bin_dir);

    if (outdated) {
        int rc = list_print_outdated(&state, as_json);
        tur_state_free(&state);
        return rc;
    }

    if (as_json) {
        list_print_json(&state, bin_dir, spices_dir, path_active);
        tur_state_free(&state);
        return 0;
    }

    printf("%s/\n", spices_dir);

    int n_with_bin = 0;
    for (int i = 0; i < state.n_entries; i++) {
        const TurStateEntry *e = &state.entries[i];
        bool is_last = (i + 1 == state.n_entries);
        const char *branch = is_last ? "\xe2\x94\x94" "\xe2\x94\x80" "\xe2\x94\x80"  /* └── */
                                     : "\xe2\x94\x9c" "\xe2\x94\x80" "\xe2\x94\x80"; /* ├── */
        const char *bar    = is_last ? "    " : "\xe2\x94\x82" "   "; /* │    or 4 spaces */

        /* Header line: name + version + origin.
         * Use a space between name and version so kebab-cased names like
         * `tur-notebook` are visually distinct from the version suffix —
         * otherwise `tur-notebook-0.1.0` reads as a single identifier and
         * users try to pass it to `tur uninstall`. */
        printf("%s %s", branch, e->name ? e->name : "(unnamed)");
        if (e->version) printf(" %s", e->version);
        if (e->n_bin == 0) printf(" [library]");
        if (e->path)               printf("       (%s) [path]", e->path);
        else if (e->url && e->ref) printf("       (%s @ %s)", e->url, e->ref);
        else if (e->url)           printf("       (%s)", e->url);
        printf("\n");

        if (e->n_bin > 0) n_with_bin++;

        char src_dir[4096] = "";
        bool have_src = inst_entry_src_dir(e, spices_dir,
                                            src_dir, sizeof(src_dir));

        PkgManifest m; memset(&m, 0, sizeof(m));
        bool have_manifest = false;
        if (have_src) {
            char manifest[4096];
            if (pkg_resolve_manifest_path(src_dir, manifest, sizeof(manifest)) &&
                pkg_manifest_read(manifest, &m)) have_manifest = true;
        }

        /* binaries line */
        char prefix1[256];
        snprintf(prefix1, sizeof(prefix1),
                 "%s\xe2\x94\x9c" "\xe2\x94\x80" "\xe2\x94\x80 binaries: ",
                 bar);
        char prefix_cont[256];
        snprintf(prefix_cont, sizeof(prefix_cont),
                 "%s\xe2\x94\x82                ", bar);
        if (e->n_bin > 0) {
            /* D4: a shadowed bin says whose the bare name is and how to
             * reach this one. */
            char **shown = (char **)calloc((size_t)e->n_bin, sizeof(char *));
            for (int j = 0; shown && j < e->n_bin; j++) {
                if (!e->bin_linked || e->bin_linked[j]) {
                    shown[j] = tur_strdup(e->bin[j]);
                    continue;
                }
                const TurStateEntry *h =
                    tur_state_bin_holder(&state, e->bin[j], e->name);
                char q[512], line[1024];
                inst_qualified(e->name, e->bin[j], q, sizeof(q));
                if (h)
                    snprintf(line, sizeof(line), "%s (shadowed by %s; tur %s)",
                             e->bin[j], h->name, q);
                else
                    snprintf(line, sizeof(line), "%s (unlinked; tur %s)",
                             e->bin[j], q);
                shown[j] = tur_strdup(line);
            }
            list_print_wrapped(prefix1, prefix_cont,
                               shown ? shown : e->bin, e->n_bin);
            for (int j = 0; shown && j < e->n_bin; j++) free(shown[j]);
            free(shown);
        } else
            printf("%s(library -- no binaries)\n", prefix1);

        /* exports line(s) */
        if (have_manifest) {
            char ep1[256];
            snprintf(ep1, sizeof(ep1),
                     "%s\xe2\x94\x94" "\xe2\x94\x80" "\xe2\x94\x80 exports:  ",
                     bar);
            char ep_cont[256];
            snprintf(ep_cont, sizeof(ep_cont),
                     "%s                 ", bar);
            if (verbose && m.n_exports > 0) {
                printf("%s\n", ep1);
                for (int j = 0; j < m.n_exports; j++)
                    printf("%s  %s\n", ep_cont, m.exports[j]);
            } else {
                list_print_wrapped(ep1, ep_cont, m.exports, m.n_exports);
            }
        } else {
            printf("%s\xe2\x94\x94" "\xe2\x94\x80" "\xe2\x94\x80 (source tree missing — re-run `tur install`?)\n",
                   bar);
        }
        pkg_manifest_free(&m);
    }

    printf("\n");
    printf("%d %s installed (%d with binaries, %d library-only).\n",
           state.n_entries,
           state.n_entries == 1 ? "spice" : "spices",
           n_with_bin, state.n_entries - n_with_bin);
    printf("PATH entry: %s (%s)\n",
           bin_dir,
           path_active ? "active" : "NOT on PATH -- run 'tur install --print-path-snippet'");

    tur_state_free(&state);
    return 0;
}

/* ------------------------------------------------------------------ */
/* tur upgrade                                                         */
/* ------------------------------------------------------------------ */

static int upgrade_usage(void) {
    fprintf(stderr,
        "usage:\n"
        "  tur upgrade <name>... [--dry-run] [--force]\n"
        "  tur upgrade --all [--dry-run] [--force]\n");
    return 1;
}

/* Resolve the remote HEAD SHA for url@ref without touching the working
 * copy. Returns a heap-allocated 40-char SHA or NULL. */
static char *upgrade_ls_remote(const char *url, const char *ref) {
    if (!url) return NULL;
    Buf cmd; buf_init(&cmd);
    buf_puts(&cmd, "git ls-remote ");
    bool ok = pkg_cmd_arg(&cmd, url);
    /* WP2 (D-6): `--` before the positional ref, same reason as
     * pkg_git_fetch's update path -- shell-quoting does not stop git from
     * reading a leading `--` as an option. */
    buf_puts(&cmd, " -- ");
    ok = pkg_cmd_arg(&cmd, ref ? ref : "HEAD") && ok;
    buf_puts(&cmd, " 2>" TUR_DEVNULL);
    buf_putc(&cmd, '\0');
    FILE *f = ok ? popen(cmd.data, "r") : NULL;
    buf_free(&cmd);
    if (!f) return NULL;
    char line[256] = {0};
    if (!fgets(line, sizeof(line), f)) { pclose(f); return NULL; }
    pclose(f);
    /* Output: "<sha>\t<refname>". Take leading 40 hex chars. */
    char *sha = (char *)malloc(64);
    if (!sha) return NULL;
    int n = 0;
    for (const char *p = line; *p && n < 40; p++) {
        if ((*p >= '0' && *p <= '9') ||
            (*p >= 'a' && *p <= 'f') ||
            (*p >= 'A' && *p <= 'F'))
            sha[n++] = *p;
        else break;
    }
    sha[n] = '\0';
    if (n != 40) { free(sha); return NULL; }
    return sha;
}

int cmd_pkg_upgrade(int argc, char **argv) {
    bool all     = false;
    bool dry_run = false;
    bool force   = false;
    const char *names[64];
    int n_names = 0;

    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0)
            return upgrade_usage();
        else if (strcmp(argv[i], "--all")     == 0) all = true;
        else if (strcmp(argv[i], "--dry-run") == 0) dry_run = true;
        else if (strcmp(argv[i], "--force")   == 0) force = true;
        else if (argv[i][0] != '-') {
            if (n_names < (int)(sizeof(names) / sizeof(names[0])))
                names[n_names++] = argv[i];
        } else {
            fprintf(stderr, "tur upgrade: unknown flag '%s'\n", argv[i]);
            return upgrade_usage();
        }
    }
    if (!all && n_names == 0) return upgrade_usage();
    if (all && n_names > 0) {
        fprintf(stderr,
            "tur upgrade: --all and explicit names are mutually exclusive\n");
        return 1;
    }

    char state_path[4096];
    if (!tur_global_state_path(state_path, sizeof(state_path))) return 1;

    TurState state;
    if (!tur_state_read(state_path, &state)) {
        fprintf(stderr, "tur upgrade: cannot read state.tur\n");
        return 1;
    }
    if (state.n_entries == 0) {
        fprintf(stderr, "tur upgrade: no spices installed\n");
        tur_state_free(&state);
        return 1;
    }

    /* Build the list of entries to upgrade (snapshot pointers; we won't
     * mutate state until the actual install runs). */
    TurStateEntry *targets[64];
    int n_targets = 0;
    if (all) {
        for (int i = 0; i < state.n_entries && n_targets < 64; i++)
            targets[n_targets++] = &state.entries[i];
    } else {
        for (int i = 0; i < n_names; i++) {
            TurStateEntry *e = tur_state_find(&state, names[i]);
            if (!e) {
                fprintf(stderr,
                    "tur upgrade: '%s' is not installed\n", names[i]);
                tur_state_free(&state);
                return 1;
            }
            targets[n_targets++] = e;
        }
    }

    if (dry_run) {
        for (int i = 0; i < n_targets; i++) {
            const TurStateEntry *e = targets[i];
            if (e->path) {
                printf("would rebuild '%s' from %s\n",
                       e->name, e->path);
            } else if (e->url) {
                char *sha = upgrade_ls_remote(e->url, e->ref);
                if (!sha) {
                    printf("'%s' (%s @ %s): unable to query remote\n",
                           e->name, e->url, e->ref ? e->ref : "HEAD");
                } else if (e->resolved && strcmp(sha, e->resolved) == 0) {
                    printf("'%s' (%s @ %s): already at %.12s\n",
                           e->name, e->url, e->ref ? e->ref : "HEAD",
                           sha);
                } else {
                    printf("'%s' (%s @ %s): %.12s -> %.12s\n",
                           e->name, e->url, e->ref ? e->ref : "HEAD",
                           e->resolved ? e->resolved : "(unknown)",
                           sha);
                    free(sha);
                }
                if (sha) free(sha);
            } else {
                printf("'%s': no source recorded\n", e->name);
            }
        }
        tur_state_free(&state);
        return 0;
    }

    /* Snapshot entry data — do_install_spec will mutate state.tur and we
     * read entry fields by value here. We can't keep pointers into state
     * after do_install_spec runs because it re-reads state.tur internally
     * and reallocates the entries array. */
    typedef struct {
        char *name;
        char *url;
        char *ref;
        char *subdir;
        char *path;
        char *dir;
    } Snapshot;
    Snapshot *snaps = (Snapshot *)calloc((size_t)n_targets, sizeof(Snapshot));
    for (int i = 0; i < n_targets; i++) {
        const TurStateEntry *e = targets[i];
        snaps[i].name   = e->name   ? tur_strdup(e->name)   : NULL;
        snaps[i].url    = e->url    ? tur_strdup(e->url)    : NULL;
        snaps[i].ref    = e->ref    ? tur_strdup(e->ref)    : NULL;
        snaps[i].subdir = e->subdir ? tur_strdup(e->subdir) : NULL;
        snaps[i].path   = e->path   ? tur_strdup(e->path)   : NULL;
        snaps[i].dir    = e->dir    ? tur_strdup(e->dir)    : NULL;
    }
    tur_state_free(&state);

    int failures = 0;
    for (int i = 0; i < n_targets; i++) {
        InstallSpec spec = {0};
        spec.verb       = "upgrade";
        /* D5: an upgrade replaces the links this spice holds and keeps
         * every bin's :linked flag (inst_plan_bin); it never takes a name.
         * Module conflicts only warn, as they always have here. */
        spec.upgrade    = true;
        (void)force;
        spec.name_alias = snaps[i].name;
        spec.subdir     = snaps[i].subdir;
        if (snaps[i].path) {
            spec.is_path = true;
            spec.src     = snaps[i].path;
        } else {
            spec.is_path = false;
            spec.src     = snaps[i].url;
            spec.ref     = snaps[i].ref;
            /* D5: re-fetch into the root the entry records. */
            spec.install_dir = snaps[i].dir;
        }
        if (!spec.src) {
            fprintf(stderr,
                "tur upgrade: state entry '%s' has neither :url nor :path; "
                "skipping.\n", snaps[i].name);
            failures++;
            continue;
        }
        int rc = do_install_spec(&spec);
        if (rc != 0) failures++;
    }

    for (int i = 0; i < n_targets; i++) {
        free(snaps[i].name);
        free(snaps[i].url);
        free(snaps[i].ref);
        free(snaps[i].subdir);
        free(snaps[i].path);
        free(snaps[i].dir);
    }
    free(snaps);

    return failures == 0 ? 0 : 1;
}
