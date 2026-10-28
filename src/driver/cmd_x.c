/* cmd_x.c -- `tur x <source> [-- args]`: run a spice's `:bin` without
 * installing it (project-less-tools-plan PT3, PT-D5).
 *
 * The source grammar is `tur install`'s:
 *
 *   an installed name   exec the binary `tur install` already built;
 *   a path              build the `:bin` into <checkout>/build/ (where
 *                       `tur install` puts it), again whenever a source
 *                       under the checkout is newer than the binary;
 *   a URL               fetch into the script-dep cache -- the same entry a
 *                       script header naming that commit gets -- and build
 *                       the `:bin` into the entry once.
 *
 * Then exec it with the words after `--`.  Nothing is registered: no
 * state.tur row, no link in the bin dir, nothing on the project-less search
 * path.  A build.tur in cwd means nothing to it.
 *
 * Part of the compiler driver (src/driver/); driver.h says how the files
 * share declarations. */

#include "driver.h"
#include "commands.h"

#include <dirent.h>

#include "global.h"
#include "pkg.h"
#include "script_deps.h"

static int x_usage(FILE *o, int rc) {
    fprintf(o,
        "usage:\n"
        "  tur x <name>  [--bin <b>] [-- <args...>]   an installed spice's binary\n"
        "  tur x <name>/<bin> [-- <args...>]          the same; also `tur <name>/<bin>`\n"
        "  tur x <path>  [--bin <b>] [-- <args...>]   a local checkout, built in place\n"
        "  tur x <url> [--ref <ref>] [--subdir <dir>] [--bin <b>] [-- <args...>]\n"
        "                                             fetched and built into a cache\n"
        "\n"
        "Runs a spice's :bin without installing it.  The first run of a URL\n"
        "fetches and builds; later runs use the cache and need no network.\n"
        "\n"
        "flags:\n"
        "  --ref <ref>       tag, branch or commit of a URL (default: its default branch)\n"
        "  --subdir <dir>    the spice's directory inside the repository\n"
        "  --bin <name>      which :bin to run, when the spice declares several\n"
        "  --refresh         resolve a branch ref again (a URL), or rebuild (a path)\n"
        "  --offline         never fetch; a URL not in the cache is an error\n"
        "  --                everything after it is the program's\n");
    return rc;
}

static bool x_is_url(const char *s) {
    return strstr(s, "://") != NULL || strncmp(s, "git@", 4) == 0;
}

static bool x_is_path(const char *s) {
    return strchr(s, '/') != NULL ||
#ifdef _WIN32
           strchr(s, '\\') != NULL ||
#endif
           s[0] == '.' || s[0] == '~';
}

/* The cache entry's name for a URL source: the last segment of the subdir,
 * else of the URL without a trailing `.git`, kept to [A-Za-z0-9._-]. */
static void x_url_name(const char *url, const char *subdir, char *out, size_t cap) {
    char tmp[1024];
    snprintf(tmp, sizeof(tmp), "%s", subdir && *subdir ? subdir : url);
    size_t n = strlen(tmp);
    while (n > 0 && (tmp[n - 1] == '/' || tmp[n - 1] == '\\')) tmp[--n] = '\0';
    if (n > 4 && strcmp(tmp + n - 4, ".git") == 0) tmp[n -= 4] = '\0';
    const char *seg = tmp;
    for (const char *p = tmp; *p; p++)
        if (*p == '/' || *p == '\\' || *p == ':') seg = p + 1;
    size_t o = 0;
    for (const char *p = seg; *p && o + 1 < cap; p++) {
        char c = *p;
        out[o++] = (isalnum((unsigned char)c) || c == '.' || c == '_' || c == '-')
                   ? c : '-';
    }
    out[o] = '\0';
    if (o == 0) snprintf(out, cap, "spice");
}

/* The newest modification time of anything under `dir` that a build reads:
 * the build output, the fetched deps and dot-directories are left out. */
static time_t x_newest_source(const char *dir, int depth) {
    time_t newest = 0;
    DIR *d = opendir(dir);
    if (!d) return 0;
    struct dirent *de;
    while ((de = readdir(d))) {
        const char *nm = de->d_name;
        if (nm[0] == '.') continue;
        if (depth == 0 && (strcmp(nm, "build") == 0 || strcmp(nm, "spices") == 0 ||
                           strcmp(nm, "cmake") == 0))
            continue;
        char p[4096];
        snprintf(p, sizeof(p), "%s/%s", dir, nm);
        struct stat st;
        if (stat(p, &st) != 0) continue;
        time_t t = st.st_mtime;
        if (S_ISDIR(st.st_mode) && depth < 32) {
            time_t sub = x_newest_source(p, depth + 1);
            if (sub > t) t = sub;
        }
        if (t > newest) newest = t;
    }
    closedir(d);
    return newest;
}

/* Pick the :bin to run.  Returns its index, or -1 after printing why.  A
 * wanted name may leave off its `tur-` prefix (`foo` is `tur-foo`), as the
 * qualified form `tur <spice>/foo` does. */
static int x_pick_bin(const char *who, const PkgManifest *m, const char *want,
                      const char *what) {
    if (m->n_bins == 0) {
        fprintf(stderr, "%s: %s declares no :bin entries; there is nothing "
                        "to run\n", who, what);
        return -1;
    }
    if (want) {
        for (int i = 0; i < m->n_bins; i++)
            if (strcmp(m->bin_names[i], want) == 0) return i;
        for (int i = 0; i < m->n_bins; i++)
            if (strncmp(m->bin_names[i], "tur-", 4) == 0 &&
                strcmp(m->bin_names[i] + 4, want) == 0)
                return i;
        fprintf(stderr, "%s: %s has no :bin named '%s'; it declares:\n",
                who, what, want);
    } else if (m->n_bins == 1) {
        return 0;
    } else {
        fprintf(stderr, "%s: %s declares several binaries; pick one with "
                        "--bin:\n", who, what);
    }
    for (int i = 0; i < m->n_bins; i++)
        fprintf(stderr, "  %s\n", m->bin_names[i]);
    return -1;
}

static bool x_read_manifest(const char *dir, PkgManifest *m) {
    memset(m, 0, sizeof(*m));
    char mp[4096];
    if (!pkg_resolve_manifest_path(dir, mp, sizeof(mp))) {
        fprintf(stderr, "tur x: no build.tur or build.tur.sweet in '%s'\n", dir);
        return false;
    }
    if (!pkg_manifest_read(mp, m)) {
        fprintf(stderr, "tur x: cannot parse '%s'\n", mp);
        return false;
    }
    return true;
}

/* An installed spice's built binary: `<src_dir>/build/<bin>`, what `tur
 * install` built and links (or, shadowed, leaves unlinked).  Never builds.
 * The spice is named as `tur install` / `tur list` show it, with or without
 * its `tur-` prefix.  Returns 0 with `exe` filled, -1 when no installed spice
 * has that name (nothing printed: the callers word that differently), or 1/2
 * after printing why. */
static int x_installed_exe(const char *who, const char *spice, const char *bin,
                           char *exe, size_t cap) {
    char state_path[4096];
    if (!tur_global_state_path(state_path, sizeof(state_path))) return -1;
    TurState state;
    tur_state_read(state_path, &state);
    TurStateEntry *e = tur_state_find_spice(&state, spice);
    char root[4096];
    char name[256];
    snprintf(name, sizeof(name), "%s", e && e->name ? e->name : spice);
    bool have = e && tur_state_entry_src_dir(e, root, sizeof(root));
    tur_state_free(&state);
    if (!have) return -1;
    PkgManifest m;
    if (!x_read_manifest(root, &m)) return 1;
    char what[300];
    snprintf(what, sizeof(what), "installed spice '%s'", name);
    int bi = x_pick_bin(who, &m, bin, what);
    if (bi < 0) { pkg_manifest_free(&m); return 2; }
    snprintf(exe, cap, "%s/build/%s", root, m.bin_names[bi]);
    struct stat st;
#ifdef _WIN32
    if (stat(exe, &st) != 0) {
        char alt[4096];
        snprintf(alt, sizeof(alt), "%s.exe", exe);
        if (stat(alt, &st) == 0) snprintf(exe, cap, "%s", alt);
    }
#endif
    if (stat(exe, &st) != 0) {
        fprintf(stderr, "%s: the installed '%s' has no built %s at %s "
                        "-- `tur upgrade %s` rebuilds it\n",
                who, name, m.bin_names[bi], exe, name);
        pkg_manifest_free(&m);
        return 1;
    }
    pkg_manifest_free(&m);
    return 0;
}

bool drv_qualified_bin_shaped(const char *w) {
    const char *slash = w ? strchr(w, '/') : NULL;
    if (!slash || slash == w || !slash[1] || strchr(slash + 1, '/')) return false;
    for (const char *p = w; *p; p++) {
        if (p == slash) continue;
        unsigned char c = (unsigned char)*p;
        /* `@` and `.` only on the spice side, for D7's `beta@0.2.0/foo`,
         * which drv_run_qualified_bin then turns away by name. */
        bool spice_side = p < slash;
        if (!(isalnum(c) || c == '-' || c == '_' ||
              (spice_side && (c == '@' || c == '.'))))
            return false;
    }
    return w[0] != '.';
}

int drv_run_qualified_bin(const char *who, const char *word,
                          char **prog_argv, int prog_argc) {
    const char *slash = strchr(word, '/');
    char spice[256];
    snprintf(spice, sizeof(spice), "%.*s", (int)(slash - word), word);
    const char *bin = slash + 1;
    if (strchr(spice, '@')) {
        fprintf(stderr, "%s: '%s': versioned selection (<spice>@<version>/<bin>) "
                        "is not supported yet\n", who, word);
        return 2;
    }
    if (strchr(spice, '.')) {
        fprintf(stderr, "%s: '%s' is not <spice>/<bin>\n", who, word);
        return 2;
    }
    char exe[4096];
    int rc = x_installed_exe(who, spice, bin, exe, sizeof(exe));
    if (rc < 0) {
        fprintf(stderr, "%s: no installed spice named '%s' (`tur list` shows "
                        "them)\n", who, spice);
        return 1;
    }
    if (rc > 0) return rc;
    return drv_exec_program(exe, prog_argv, prog_argc);
}

static bool x_mkdir(const char *p) {
    return mkdir(p, 0755) == 0 || errno == EEXIST;
}

int cmd_x(int argc, char **argv) {
    const char *source = NULL, *ref = NULL, *subdir = NULL, *bin = NULL;
    bool refresh = false, offline = false;
    int dd = argc;
    for (int i = 2; i < argc; i++) {
        const char *a = argv[i];
        if (strcmp(a, "--") == 0) { dd = i; break; }
        if (strcmp(a, "-h") == 0 || strcmp(a, "--help") == 0)
            return x_usage(stdout, 0);
        const char **slot = NULL;
        const char *val = NULL;
        if      (strncmp(a, "--ref=", 6) == 0)    { slot = &ref;    val = a + 6; }
        else if (strncmp(a, "--subdir=", 9) == 0) { slot = &subdir; val = a + 9; }
        else if (strncmp(a, "--bin=", 6) == 0)    { slot = &bin;    val = a + 6; }
        else if (strcmp(a, "--ref") == 0)         slot = &ref;
        else if (strcmp(a, "--subdir") == 0)      slot = &subdir;
        else if (strcmp(a, "--bin") == 0)         slot = &bin;
        if (slot) {
            if (!val) {
                if (i + 1 >= argc || strcmp(argv[i + 1], "--") == 0) {
                    fprintf(stderr, "tur x: %s takes a value\n", a);
                    return x_usage(stderr, 2);
                }
                val = argv[++i];
            }
            *slot = val;
        } else if (strcmp(a, "--refresh") == 0) {
            refresh = true;
        } else if (strcmp(a, "--offline") == 0) {
            offline = true;
        } else if (a[0] == '-') {
            fprintf(stderr, "tur x: unknown flag '%s' (the program's own flags "
                            "go after `--`)\n", a);
            return x_usage(stderr, 2);
        } else if (!source) {
            source = a;
        } else {
            fprintf(stderr, "tur x: unexpected '%s' (the program's arguments go "
                            "after `--`)\n", a);
            return x_usage(stderr, 2);
        }
    }
    if (!source) return x_usage(stderr, 2);
    char **prog_argv = dd < argc ? argv + dd + 1 : NULL;
    int    prog_argc = dd < argc ? argc - dd - 1 : 0;

    /* namespaced-spice-binaries D3: `tur x beta/foo` is `tur beta/foo` --
     * unless a directory of that name is here, which stays a path. */
    struct stat qst;
    if (!x_is_url(source) && drv_qualified_bin_shaped(source) &&
        stat(source, &qst) != 0) {
        if (bin) {
            fprintf(stderr, "tur x: '%s' already names its binary; drop --bin\n",
                    source);
            return 2;
        }
        return drv_run_qualified_bin("tur x", source, prog_argv, prog_argc);
    }

    bool is_url = x_is_url(source);
    if (!is_url && (ref || subdir)) {
        fprintf(stderr, "tur x: --ref and --subdir name a commit of a URL; "
                        "'%s' is not one\n", source);
        return 2;
    }

    PkgManifest m;
    char root[4096];
    char exe[4096];

    if (is_url) {
        char name[256];
        x_url_name(source, subdir, name, sizeof(name));
        if (script_deps_cache_entry("tur x", name, source, ref, subdir,
                                    refresh, offline, root, sizeof(root)) != 1)
            return 1;
        if (!x_read_manifest(root, &m)) return 1;
        int bi = x_pick_bin("tur x", &m, bin, source);
        if (bi < 0) { pkg_manifest_free(&m); return 2; }
        /* Built once per entry, then immutable.  The build lands under a
         * private name first, so a concurrent first run never execs half a
         * binary. */
        char bindir[4096];
        snprintf(bindir, sizeof(bindir), "%s/.tur-x-bin", root);
        snprintf(exe, sizeof(exe), "%s/%s", bindir, m.bin_names[bi]);
        struct stat st;
        if (stat(exe, &st) != 0) {
            if (!x_mkdir(bindir)) {
                fprintf(stderr, "tur x: cannot create %s: %s\n", bindir,
                        strerror(errno));
                pkg_manifest_free(&m);
                return 1;
            }
            char tmp[4096];
            snprintf(tmp, sizeof(tmp), "%s/.%s-%ld", bindir, m.bin_names[bi],
                     (long)getpid());
            if (!pkg_build_spice_bin("tur x", root, &m, m.bin_names[bi],
                                     m.bin_paths[bi], tmp)) {
                unlink(tmp);
                pkg_manifest_free(&m);
                return 1;
            }
            if (rename(tmp, exe) != 0 && stat(exe, &st) != 0) {
                fprintf(stderr, "tur x: cannot place %s: %s\n", exe,
                        strerror(errno));
                unlink(tmp);
                pkg_manifest_free(&m);
                return 1;
            }
            unlink(tmp);
        }
    } else if (x_is_path(source)) {
        char path[4096];
        if (source[0] == '~' && (source[1] == '/' || source[1] == '\0') &&
            getenv("HOME"))
            snprintf(path, sizeof(path), "%s%s", getenv("HOME"), source + 1);
        else
            snprintf(path, sizeof(path), "%s", source);
        if (!realpath(path, root)) {
            fprintf(stderr, "tur x: cannot resolve '%s': %s\n", source,
                    strerror(errno));
            return 1;
        }
        if (!x_read_manifest(root, &m)) return 1;
        int bi = x_pick_bin("tur x", &m, bin, root);
        if (bi < 0) { pkg_manifest_free(&m); return 2; }
        char bindir[4096];
        snprintf(bindir, sizeof(bindir), "%s/build", root);
        snprintf(exe, sizeof(exe), "%s/%s", bindir, m.bin_names[bi]);
        struct stat st;
        bool stale = refresh || stat(exe, &st) != 0 ||
                     x_newest_source(root, 0) > st.st_mtime;
        if (stale) {
            if (!x_mkdir(bindir)) {
                fprintf(stderr, "tur x: cannot create %s: %s\n", bindir,
                        strerror(errno));
                pkg_manifest_free(&m);
                return 1;
            }
            if (!offline && !pkg_setup_fetched_project("x", root, &m)) {
                pkg_manifest_free(&m);
                return 1;
            }
            if (!pkg_build_spice_bin("tur x", root, &m, m.bin_names[bi],
                                     m.bin_paths[bi], exe)) {
                pkg_manifest_free(&m);
                return 1;
            }
        }
    } else {
        /* An installed name: exec what `tur install` built; never build. */
        int rc = x_installed_exe("tur x", source, bin, exe, sizeof(exe));
        if (rc < 0) {
            fprintf(stderr,
                    "tur x: no installed spice named '%s' -- give a path "
                    "(./%s) or a URL, or `tur install` it\n", source, source);
            return 1;
        }
        if (rc > 0) return rc;
        return drv_exec_program(exe, prog_argv, prog_argc);
    }
    pkg_manifest_free(&m);
    return drv_exec_program(exe, prog_argv, prog_argc);
}
