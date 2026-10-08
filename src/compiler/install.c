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

#ifdef __APPLE__
#  include <mach-o/dyld.h>
#endif

#include "buf.h"
#include "global.h"
#include "pkg.h"
#include "platform_fs.h"
#include "platform_proc.h"
#ifdef _WIN32
#include <windows.h>  /* GetModuleFileNameA */
#endif

/* Storage for the --json global. main.c sets this from argv parsing; we
 * (and other TUs) read it via extern declaration. */
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
 * rule.  docs/reported/windows-spice-fetch-shell-quoting.md */
bool pkg_rm_rf(const char *path) {
    if (!path || !*path) return false;
    if (!strchr(path, '/')) return false;
    if (strstr(path, "..")) return false; /* defensive */
    return inst_rm_tree(path);
}

/* Walk the bin_dir/<name>:
 *   - returns true if it's missing, or if it's a symlink pointing into the
 *     spices/ tree (i.e. owned by an earlier tur install), or if force is
 *     set. Otherwise prints a diagnostic and returns false. */
static bool inst_check_bin_target(const char *target, const char *bin_name,
                                   const char *install_name,
                                   const TurState *state,
                                   const char *spices_dir,
                                   const char *self_install_dir,
                                   bool force) {
    struct stat st;
    if (lstat(target, &st) != 0) {
        if (errno == ENOENT) return true;
        fprintf(stderr, "tur install: cannot stat '%s': %s\n",
                target, strerror(errno));
        return false;
    }
    if (force) return true;

    /* Where a symlink is not available the file cannot say who put it there,
     * so ask the registry, which is the actual record of what tur installed.
     * (S_ISLNK is a compile-time 0 on Windows -- platform_fs.h.) */
    if (!S_ISLNK(st.st_mode) && state != NULL) {
        for (int i = 0; i < state->n_entries; i++) {
            const TurStateEntry *se = &state->entries[i];
            for (int j = 0; j < se->n_bin; j++) {
                if (strcmp(se->bin[j], bin_name) != 0) continue;
                if (install_name && strcmp(se->name, install_name) == 0)
                    return true;   /* our own previous install */
                fprintf(stderr,
                    "tur install: '%s' is already provided by installed spice "
                    "'%s'.\n"
                    "  Run `tur uninstall %s` first, or pass --force.\n",
                    target, se->name, se->name);
                return false;
            }
        }
    }

    if (S_ISLNK(st.st_mode)) {
        char buf[4096];
        ssize_t n = readlink(target, buf, sizeof(buf) - 1);
        if (n > 0) {
            buf[n] = '\0';
            if (self_install_dir && strncmp(buf, self_install_dir,
                                            strlen(self_install_dir)) == 0)
                return true; /* overwriting our own previous install */
            if (strncmp(buf, spices_dir, strlen(spices_dir)) == 0) {
                fprintf(stderr,
                    "tur install: '%s' is already provided by another "
                    "installed spice (-> %s).\n"
                    "  Run `tur uninstall` first, or pass --force.\n",
                    target, buf);
                return false;
            }
        }
    }
    fprintf(stderr,
        "tur install: refusing to overwrite existing '%s' "
        "(not owned by tur). Use --force to override.\n", target);
    return false;
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
        "  tur install <url> [--ref <ref>] [--subdir <path>] [--name <name>] [--force]\n"
        "  tur install <path> --path [--name <name>] [--force]\n"
        "  tur install --print-path-snippet\n");
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
    bool        is_path;
    bool        force;
    const char *verb;         /* "install" or "upgrade" — used in messages */
} InstallSpec;

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

        char ref_tag[128];
        snprintf(ref_tag, sizeof(ref_tag), "%s",
                 ref ? ref : "HEAD");
        /* sanitize ref_tag: replace '/' with '-' */
        for (char *p = ref_tag; *p; p++) if (*p == '/') *p = '-';

        snprintf(install_dir, sizeof(install_dir), "%s/%s-%s",
                 spices_dir, derived_name, ref_tag);

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
    /* 2. Read build.tur                                                */
    /* ---------------------------------------------------------------- */
    char build_path[4096];
    snprintf(build_path, sizeof(build_path), "%s/build.tur", src_dir);

    struct stat st;
    if (stat(build_path, &st) != 0) {
        fprintf(stderr, "tur %s: no build.tur at '%s'\n", verb, src_dir);
        free(resolved_sha);
        return 1;
    }

    PkgManifest m;
    memset(&m, 0, sizeof(m));
    if (!pkg_manifest_read(build_path, &m)) {
        fprintf(stderr, "tur %s: cannot parse '%s'\n", verb, build_path);
        free(resolved_sha);
        return 1;
    }

    if (m.n_bins == 0) {
        fprintf(stderr,
            "tur %s: spice '%s' declares no :bin entries; "
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
    /* 3. Pre-flight conflict check on bin_dir/<name> entries.          */
    /* ---------------------------------------------------------------- */
    TurState pre_state;
    tur_state_read(state_path, &pre_state);
    for (int i = 0; i < m.n_bins; i++) {
        char binfile[4096], target[4096];
        inst_bin_name(m.bin_names[i], binfile, sizeof(binfile));
        snprintf(target, sizeof(target), "%s/%s", bin_dir, binfile);
        if (!inst_check_bin_target(target, m.bin_names[i], install_name,
                                    &pre_state, spices_dir,
                                    install_dir[0] ? install_dir : src_dir,
                                    force)) {
            tur_state_free(&pre_state);
            pkg_manifest_free(&m);
            free(resolved_sha);
            return 1;
        }
    }
    tur_state_free(&pre_state);

    /* ---------------------------------------------------------------- */
    /* 4. Build each declared binary.                                   */
    /*    Output goes to <src_dir>/build/<bin_name>.                    */
    /* ---------------------------------------------------------------- */
    char build_out_dir[4096];
    snprintf(build_out_dir, sizeof(build_out_dir), "%s/build", src_dir);
    if (!inst_mkdirp(build_out_dir)) {
        fprintf(stderr, "tur %s: cannot create '%s'\n", verb, build_out_dir);
        pkg_manifest_free(&m);
        free(resolved_sha);
        return 1;
    }

    char self_exe[4096];
    inst_self_exe(self_exe, sizeof(self_exe));

    /* `tur build` invokes cc with autolink markers like
     * `src/runtime/hamt.c` that are interpreted relative to cwd. Find the
     * turmeric source root from the exe path and cd there before each
     * sub-build so those relative paths resolve. */
    char tur_root[4096];
    bool have_root = inst_find_turmeric_root(self_exe, tur_root, sizeof(tur_root));

    /* ---------------------------------------------------------------- */
    /* 4a. Fetch :spices deps and collect their src/ dirs for -I.       */
    /* ---------------------------------------------------------------- */
#define INST_MAX_DEPS 64
    char *dep_srcs[INST_MAX_DEPS];
    int   n_dep_srcs = 0;
    bool  deps_ok = true;

    for (int j = 0; j < m.n_spices && n_dep_srcs < INST_MAX_DEPS; j++) {
        const PkgSpice *dep = &m.spices[j];
        char dep_dest[4096];

        if (dep->path) {
            /* Local path dep: resolve relative to src_dir. */
            if (dep->path[0] == '/')
                snprintf(dep_dest, sizeof(dep_dest), "%s", dep->path);
            else
                snprintf(dep_dest, sizeof(dep_dest), "%s/%s",
                         src_dir, dep->path);
        } else if (dep->url) {
            /* Git dep: fetch into global spices dir if not already present. */
            char ref_tag[128];
            snprintf(ref_tag, sizeof(ref_tag), "%s",
                     dep->ref ? dep->ref : "HEAD");
            for (char *p = ref_tag; *p; p++) if (*p == '/') *p = '-';

            if (dep->ref)
                snprintf(dep_dest, sizeof(dep_dest), "%s/%s-%s",
                         spices_dir, dep->name, ref_tag);
            else
                snprintf(dep_dest, sizeof(dep_dest), "%s/%s",
                         spices_dir, dep->name);

            struct stat dep_st;
            if (stat(dep_dest, &dep_st) != 0 || !S_ISDIR(dep_st.st_mode)) {
                fprintf(stderr, "tur %s: fetching dep '%s' ...\n",
                        verb, dep->name);
                char *sha = pkg_git_fetch(dep->url, dep->ref, dep_dest);
                if (!sha) {
                    if (dep->optional) {
                        fprintf(stderr,
                            "tur %s: optional dep '%s' unavailable, skipping\n",
                            verb, dep->name);
                        continue;
                    }
                    fprintf(stderr,
                        "tur %s: failed to fetch required dep '%s'\n",
                        verb, dep->name);
                    deps_ok = false;
                    break;
                }
                free(sha);
            }
        } else {
            continue;
        }

        /* Compute the src/ dir, accounting for :subdir. */
        char dep_src[4096];
        if (dep->subdir)
            snprintf(dep_src, sizeof(dep_src), "%s/%s/src",
                     dep_dest, dep->subdir);
        else
            snprintf(dep_src, sizeof(dep_src), "%s/src", dep_dest);

        struct stat src_st;
        if (stat(dep_src, &src_st) == 0 && S_ISDIR(src_st.st_mode))
            dep_srcs[n_dep_srcs++] = tur_strdup(dep_src);
    }

    if (!deps_ok) {
        for (int j = 0; j < n_dep_srcs; j++) free(dep_srcs[j]);
        pkg_manifest_free(&m);
        free(resolved_sha);
        return 1;
    }

    for (int i = 0; i < m.n_bins; i++) {
        const char *bin_name = m.bin_names[i];
        const char *entry    = m.bin_paths[i];

        char entry_abs[4096];
        if (entry[0] == '/')
            snprintf(entry_abs, sizeof(entry_abs), "%s", entry);
        else
            snprintf(entry_abs, sizeof(entry_abs), "%s/%s", src_dir, entry);

        if (stat(entry_abs, &st) != 0) {
            fprintf(stderr,
                "tur %s: :bin entrypoint '%s' for binary '%s' "
                "not found.\n", verb, entry_abs, bin_name);
            for (int j = 0; j < n_dep_srcs; j++) free(dep_srcs[j]);
            pkg_manifest_free(&m);
            free(resolved_sha);
            return 1;
        }

        char bin_out[4096];
        snprintf(bin_out, sizeof(bin_out), "%s/%s",
                 build_out_dir, bin_name);

        char src_subdir[4096];
        snprintf(src_subdir, sizeof(src_subdir), "%s/src", src_dir);
        bool has_src = (stat(src_subdir, &st) == 0 && S_ISDIR(st.st_mode));

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
        if (has_src) {
            buf_puts(&cmd, " -I ");
            cmd_ok = pkg_cmd_arg(&cmd, src_subdir) && cmd_ok;
        }
        for (int j = 0; j < n_dep_srcs; j++) {
            buf_puts(&cmd, " -I ");
            cmd_ok = pkg_cmd_arg(&cmd, dep_srcs[j]) && cmd_ok;
        }
        buf_puts(&cmd, " -o ");
        cmd_ok = pkg_cmd_arg(&cmd, bin_out) && cmd_ok;
        buf_putc(&cmd, '\0');

        /* This command STARTS with a quoted program path (or with `cd` when
         * have_root), which is the case cmd.exe's "strip the first and last
         * quote" rule tears in half.  tur_shell_command adds the enclosing
         * pair it eats; on POSIX it is a copy. */
        char wrapped[16384];
        if (cmd_ok && tur_shell_command(cmd.data, wrapped, sizeof(wrapped)) != 0)
            cmd_ok = false;

        fprintf(stderr, "tur %s: building %s ...\n", verb, bin_name);
        int rc = cmd_ok ? system(wrapped) : -1;
        buf_free(&cmd);
        if (rc != 0) {
            fprintf(stderr,
                "tur %s: build failed for '%s' (rc=%d)\n",
                verb, bin_name, rc);
            for (int j = 0; j < n_dep_srcs; j++) free(dep_srcs[j]);
            pkg_manifest_free(&m);
            free(resolved_sha);
            return 1;
        }
        if (stat(bin_out, &st) != 0) {
            fprintf(stderr,
                "tur %s: build produced no output at '%s'\n",
                verb, bin_out);
            for (int j = 0; j < n_dep_srcs; j++) free(dep_srcs[j]);
            pkg_manifest_free(&m);
            free(resolved_sha);
            return 1;
        }
    }
    for (int j = 0; j < n_dep_srcs; j++) free(dep_srcs[j]);

    /* ---------------------------------------------------------------- */
    /* 5. Symlink each binary into bin_dir.                             */
    /* ---------------------------------------------------------------- */
    for (int i = 0; i < m.n_bins; i++) {
        char binfile[4096], target[4096], source[4096];
        inst_bin_name(m.bin_names[i], binfile, sizeof(binfile));
        snprintf(target, sizeof(target), "%s/%s", bin_dir, binfile);
        /* The BUILD artifact keeps the bare name -- `tur build -o` writes
         * exactly the path it is given -- so only the installed entry gets the
         * platform suffix. */
        snprintf(source, sizeof(source), "%s/%s",
                 build_out_dir, m.bin_names[i]);
        if (!inst_place_bin(source, target)) {
            pkg_manifest_free(&m);
            free(resolved_sha);
            return 1;
        }
    }

    /* ---------------------------------------------------------------- */
    /* 6. Update state.tur                                              */
    /* ---------------------------------------------------------------- */
    TurState state;
    tur_state_read(state_path, &state);
    TurStateEntry *e = tur_state_upsert(&state, install_name);
    if (!e) {
        fprintf(stderr, "tur %s: oom updating state.tur\n", verb);
        tur_state_free(&state);
        pkg_manifest_free(&m);
        free(resolved_sha);
        return 1;
    }
    if (is_path)        e->path     = tur_strdup(src_dir);
    if (src && !is_path)e->url      = tur_strdup(src);
    if (ref)            e->ref      = tur_strdup(ref);
    if (subdir)         e->subdir   = tur_strdup(subdir);
    if (resolved_sha)   e->resolved = tur_strdup(resolved_sha);
    if (m.version)      e->version  = tur_strdup(m.version);
    e->installed_at = tur_strdup(inst_iso_now());
#ifdef TUR_VERSION
    e->built_with = tur_strdup(TUR_VERSION);
#endif
    e->n_bin = m.n_bins;
    e->bin   = (char **)malloc(m.n_bins * sizeof(char *));
    for (int i = 0; i < m.n_bins; i++)
        e->bin[i] = tur_strdup(m.bin_names[i]);

    if (!tur_state_write(state_path, &state)) {
        tur_state_free(&state);
        pkg_manifest_free(&m);
        free(resolved_sha);
        return 1;
    }

    /* ---------------------------------------------------------------- */
    /* Output + PATH-check                                              */
    /* ---------------------------------------------------------------- */
    const char *past = (strcmp(verb, "upgrade") == 0) ? "upgraded" : "installed";
    fprintf(stderr, "tur %s: %s '%s'", verb, past, install_name);
    if (m.version) fprintf(stderr, " (%s)", m.version);
    fprintf(stderr, "\n");
    for (int i = 0; i < m.n_bins; i++) {
        char binfile[4096];
        inst_bin_name(m.bin_names[i], binfile, sizeof(binfile));
        fprintf(stderr, "  %s/%s\n", bin_dir, binfile);
    }

    if (!tur_global_bin_on_path(bin_dir)) {
        fprintf(stderr,
            "\nNote: %s is not on $PATH.\n"
            "  bash/zsh: export PATH=\"%s:$PATH\"\n"
            "  fish:     set -gx PATH %s $PATH\n",
            bin_dir, bin_dir, bin_dir);
    }

    tur_state_free(&state);
    pkg_manifest_free(&m);
    free(resolved_sha);
    return 0;
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

    /* Remove bin_dir/<name> for each bin we own.  The symlink check below is
     * defensive, not the ownership decision -- we are already inside
     * `if (e)`, so the registry has said this spice owns these names.  Where a
     * symlink was not available the entry is a copy (inst_place_bin), which
     * readlink cannot vouch for, so a plain file at a path the registry claims
     * is removed too. */
    for (int i = 0; i < e->n_bin; i++) {
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
        } else {
            fprintf(stderr, "tur uninstall: removed %s\n", target);
        }
    }

    /* Remove the source tree (non-path installs only). */
    if (!e->path) {
        /* Find <spices_dir>/<name>-<something>/ — match by prefix. */
        DIR *d = opendir(spices_dir);
        if (d) {
            struct dirent *ent;
            size_t nl = strlen(name);
            while ((ent = readdir(d)) != NULL) {
                if (ent->d_name[0] == '.') continue;
                size_t el = strlen(ent->d_name);
                if (el <= nl + 1) continue;
                if (strncmp(ent->d_name, name, nl) != 0) continue;
                if (ent->d_name[nl] != '-') continue;
                char tree[4096];
                snprintf(tree, sizeof(tree), "%s/%s",
                         spices_dir, ent->d_name);
                if (pkg_rm_rf(tree)) {
                    fprintf(stderr, "tur uninstall: removed %s\n", tree);
                }
            }
            closedir(d);
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

/* Reconstruct the source dir on disk for an installed entry. For path
 * installs this is the local checkout; for git installs we look in
 * spices/<name>-<sanitized-ref>/<subdir>?. Returns true if the dir
 * exists. */
static bool inst_entry_src_dir(const TurStateEntry *e,
                                const char *spices_dir,
                                char *out, size_t out_sz) {
    if (e->path) {
        snprintf(out, out_sz, "%s", e->path);
        struct stat st;
        return stat(out, &st) == 0 && S_ISDIR(st.st_mode);
    }
    char ref_tag[128];
    snprintf(ref_tag, sizeof(ref_tag), "%s", e->ref ? e->ref : "HEAD");
    for (char *p = ref_tag; *p; p++) if (*p == '/') *p = '-';
    char base[4096];
    snprintf(base, sizeof(base), "%s/%s-%s", spices_dir,
             e->name ? e->name : "unknown", ref_tag);
    if (e->subdir)
        snprintf(out, out_sz, "%s/%s", base, e->subdir);
    else
        snprintf(out, out_sz, "%s", base);
    struct stat st;
    return stat(out, &st) == 0 && S_ISDIR(st.st_mode);
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
        buf_puts(&b, "      \"exports\": [");
        if (have_src) {
            char manifest[4096];
            snprintf(manifest, sizeof(manifest),
                     "%s/build.tur", src_dir);
            PkgManifest m; memset(&m, 0, sizeof(m));
            if (pkg_manifest_read(manifest, &m)) {
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
    /* `--json` is a global flag (stripped from argv by main.c); honor it. */
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
            snprintf(manifest, sizeof(manifest), "%s/build.tur", src_dir);
            if (pkg_manifest_read(manifest, &m)) have_manifest = true;
        }

        /* binaries line */
        char prefix1[256];
        snprintf(prefix1, sizeof(prefix1),
                 "%s\xe2\x94\x9c" "\xe2\x94\x80" "\xe2\x94\x80 binaries: ",
                 bar);
        char prefix_cont[256];
        snprintf(prefix_cont, sizeof(prefix_cont),
                 "%s\xe2\x94\x82                ", bar);
        list_print_wrapped(prefix1, prefix_cont, e->bin, e->n_bin);

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
    printf("%d %s installed (%d with binaries).\n",
           state.n_entries,
           state.n_entries == 1 ? "spice" : "spices",
           n_with_bin);
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
    } Snapshot;
    Snapshot *snaps = (Snapshot *)calloc((size_t)n_targets, sizeof(Snapshot));
    for (int i = 0; i < n_targets; i++) {
        const TurStateEntry *e = targets[i];
        snaps[i].name   = e->name   ? tur_strdup(e->name)   : NULL;
        snaps[i].url    = e->url    ? tur_strdup(e->url)    : NULL;
        snaps[i].ref    = e->ref    ? tur_strdup(e->ref)    : NULL;
        snaps[i].subdir = e->subdir ? tur_strdup(e->subdir) : NULL;
        snaps[i].path   = e->path   ? tur_strdup(e->path)   : NULL;
    }
    tur_state_free(&state);

    int failures = 0;
    for (int i = 0; i < n_targets; i++) {
        InstallSpec spec = {0};
        spec.verb       = "upgrade";
        spec.force      = true; /* upgrade always replaces its own symlinks */
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
    }
    free(snaps);

    return failures == 0 ? 0 : 1;
}
