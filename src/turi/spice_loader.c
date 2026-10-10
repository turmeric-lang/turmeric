/* spice_loader.c -- see spice_loader.h.
 *
 * Responsibilities:
 *   1. Walk up from cwd to find an enclosing `build.tur`.
 *   2. Compare every .tur file's mtime to the cached library's mtime,
 *      and rebuild via `tur build --shared` if any source is newer.
 *      (sha256-based `build.manifest` from the plan is deferred; mtime
 *      is sufficient for the REPL's "did anything change?" question
 *      and avoids touching every byte on cold starts.)
 *   3. dlopen the library and dlsym each export listed in
 *      exports.manifest, materialising a TurSpiceExport row per defn.
 *
 * The dispatcher class encoding (`i`/`f`/`v`) is shared with
 * src/runtime/ffi_dispatch.h; the binding layer in RP4 picks the
 * matching `tur_ffi_call_<ret>_<args>` trampoline based on it.
 */

#include "platform_proc.h"
#include "spice_loader.h"
#include "platform_fs.h"  /* realpath/mkdir/getline on Windows */

#include <dirent.h>
#ifdef _WIN32
#  include "platform_dl.h"  /* dlopen/dlsym/dlclose over LoadLibrary */
#else
#  include <dlfcn.h>
#endif
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

/* ------------------------------------------------------------------ */
/* Image                                                              */
/* ------------------------------------------------------------------ */

struct TurSpiceImage {
    char           *root;       /* absolute path to dir containing build.tur */
    char           *build_dir;  /* `<root>/src` if present, else == root */
    char           *lib_path;   /* path to the loaded .so (under .tur-repl-cache) */
    void           *handle;     /* dlopen result (NULL on the J2 jit path) */
    /* J2: in-process image from the jit hook (NULL on the dlopen path).
     * Symbols resolve through g_jit_hook->sym instead of dlsym, and
     * freshness is source mtime vs build_stamp_ns (there is no .so). */
    void           *jit_image;
    int64_t         build_stamp_ns;
    TurSpiceExport *exports;
    uint32_t        n_exports;
    uint32_t        cap_exports;
};

/* J2: the installed in-process JIT hook, or NULL for the subprocess path. */
static const TurSpiceJitHook *g_jit_hook = NULL;

void tur_spice_set_jit_hook(const TurSpiceJitHook *hook) {
    g_jit_hook = hook;
}

static void spice_export_free(TurSpiceExport *e) {
    free(e->module);
    free(e->name);
    free(e->mangled);
    free(e->arg_classes);
}

void tur_spice_image_free(TurSpiceImage *img) {
    if (!img) return;
    for (uint32_t i = 0; i < img->n_exports; i++) {
        spice_export_free(&img->exports[i]);
    }
    free(img->exports);
    if (img->handle) dlclose(img->handle);
    if (img->jit_image && g_jit_hook && g_jit_hook->free_image)
        g_jit_hook->free_image(img->jit_image);
    free(img->lib_path);
    free(img->build_dir);
    free(img->root);
    free(img);
}

const char *tur_spice_image_root(const TurSpiceImage *img) {
    return img ? img->root : NULL;
}
const char *tur_spice_image_lib(const TurSpiceImage *img) {
    return img ? img->lib_path : NULL;
}
uint32_t tur_spice_image_count(const TurSpiceImage *img) {
    return img ? img->n_exports : 0;
}
const TurSpiceExport *tur_spice_image_at(const TurSpiceImage *img,
                                          uint32_t i) {
    if (!img || i >= img->n_exports) return NULL;
    return &img->exports[i];
}
const TurSpiceExport *tur_spice_image_find(const TurSpiceImage *img,
                                            const char *module,
                                            const char *name) {
    if (!img || !module || !name) return NULL;
    for (uint32_t i = 0; i < img->n_exports; i++) {
        const TurSpiceExport *e = &img->exports[i];
        if (strcmp(e->module, module) == 0 && strcmp(e->name, name) == 0) {
            return e;
        }
    }
    return NULL;
}

/* Forward decls: defined below alongside needs_rebuild. */
static bool needs_rebuild(const char *root, const char *lib_path,
                          const char *tur_bin);
static int64_t newest_tur_mtime(const char *dir, int64_t acc);

bool tur_spice_image_is_fresh(const TurSpiceImage *img) {
    if (!img) return false;
    /* J2: no .so on the in-process path -- compare sources against the
     * image's build timestamp instead of a library mtime. */
    if (img->jit_image) {
        return newest_tur_mtime(img->build_dir, 0) <= img->build_stamp_ns;
    }
    return !needs_rebuild(img->build_dir, img->lib_path, NULL);
}

/* ------------------------------------------------------------------ */
/* Discovery: walk up to build.tur                                    */
/* ------------------------------------------------------------------ */

/* Same upper bound as the compiler's find_spice_root: bounded walk
 * keeps stat() noise contained on filesystems with no build.tur all
 * the way to /. */
#define SPICE_WALK_MAX 16

static char *find_build_tur_root(const char *start) {
    char dir[4096];
    if (!start || !*start) start = ".";
    if (!realpath(start, dir)) {
        /* Fall back to start as-is; realpath fails when the path
         * contains a non-existent component. */
        snprintf(dir, sizeof(dir), "%s", start);
    }
    for (int i = 0; i < SPICE_WALK_MAX; i++) {
        char candidate[4200];
        snprintf(candidate, sizeof(candidate), "%s/build.tur", dir);
        struct stat st;
        if (stat(candidate, &st) == 0 && S_ISREG(st.st_mode)) {
            char *out = strdup(dir);
            return out;
        }
        char *slash = strrchr(dir, '/');
        if (!slash || slash == dir) break;
        *slash = '\0';
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/* Freshness: any .tur newer than the .so?                            */
/* ------------------------------------------------------------------ */

/* Modification time in nanoseconds.  Whole-second `st_mtime` is too coarse
 * for the --watch reload check: on macOS CI the `tur build --shared` rebuild
 * is slow enough that the cached .so lands in the same whole second as a
 * subsequent source edit, so a strict `source > lib` second comparison
 * misses the edit and the auto-reload never fires (the edit genuinely
 * happens after the build in wall-clock time, so sub-second resolution
 * resolves it).  APFS and ext4 both record nanosecond mtimes; filesystems
 * without sub-second resolution report tv_nsec == 0 and degrade cleanly to
 * the previous second-granularity behavior. */
static int64_t stat_mtime_ns(const struct stat *st) {
#if defined(__APPLE__)
    long nsec = st->st_mtimespec.tv_nsec;
#elif defined(st_mtime) || defined(__linux__) || defined(_POSIX_C_SOURCE)
    long nsec = st->st_mtim.tv_nsec;
#else
    long nsec = 0;
#endif
    return (int64_t)st->st_mtime * 1000000000LL + (int64_t)nsec;
}

static int64_t newest_tur_mtime(const char *dir, int64_t acc) {
    DIR *d = opendir(dir);
    if (!d) return acc;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue;       /* skip dotfiles + . / .. */
        char path[4200];
        snprintf(path, sizeof(path), "%s/%s", dir, e->d_name);
        struct stat st;
        if (stat(path, &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) {
            acc = newest_tur_mtime(path, acc);
            continue;
        }
        if (!S_ISREG(st.st_mode)) continue;
        size_t nl = strlen(e->d_name);
        if (nl < 5 || strcmp(e->d_name + nl - 4, ".tur") != 0) continue;
        int64_t m = stat_mtime_ns(&st);
        if (m > acc) acc = m;
    }
    closedir(d);
    return acc;
}

#ifndef TUR_VERSION
#define TUR_VERSION "unknown"
#endif

/* WP2 (D-3): the provenance stamp written beside a cached `.so`.
 *
 * `tur repl` walks up from the cwd, AOT-builds the tree it finds and
 * `dlopen`s the result.  Whether to rebuild was decided by ONE mtime
 * comparison, so a `.tur-repl-cache/lib-N.so` COMMITTED to a repository with a
 * timestamp newer than the sources was loaded into the process without a build
 * -- opening a checkout and typing `tur repl` ran it.  The `.gitignore`
 * handshake in ensure_cache_dir does nothing against a repo that commits the
 * object deliberately.
 *
 * The fix is to require the library to be one THIS tur built: a sidecar
 * recording the compiler's version, path, size and mtime.  An attacker can
 * write the `.so`, and can write the stamp too -- but not one that matches the
 * `tur` binary on the machine they are attacking, which they do not know.  The
 * check is "did I build this", not "is this file trustworthy". */
static void repl_cache_stamp_path(const char *lib_path, char *out, size_t cap) {
    snprintf(out, cap, "%s.built-by", lib_path);
}

static void repl_cache_stamp_text(const char *tur_bin, char *out, size_t cap) {
    struct stat es;
    long long sz = 0, mt = 0;
    if (tur_bin && stat(tur_bin, &es) == 0) {
        sz = (long long)es.st_size;
        mt = (long long)es.st_mtime;
    }
    snprintf(out, cap, "tur-repl-cache-v1\n%s\n%s\n%lld\n%lld\n",
             TUR_VERSION, tur_bin ? tur_bin : "", sz, mt);
}

static bool repl_cache_stamp_matches(const char *lib_path, const char *tur_bin) {
    char sp[4200];
    repl_cache_stamp_path(lib_path, sp, sizeof(sp));
    FILE *f = fopen(sp, "rb");
    if (!f) return false;
    char have[8192];
    size_t n = fread(have, 1, sizeof(have) - 1, f);
    fclose(f);
    have[n] = '\0';
    char want[8192];
    repl_cache_stamp_text(tur_bin, want, sizeof(want));
    return strcmp(have, want) == 0;
}

static void repl_cache_stamp_write(const char *lib_path, const char *tur_bin) {
    char sp[4200];
    repl_cache_stamp_path(lib_path, sp, sizeof(sp));
    FILE *f = fopen(sp, "wb");
    if (!f) return;
    char txt[8192];
    repl_cache_stamp_text(tur_bin, txt, sizeof(txt));
    fputs(txt, f);
    fclose(f);
}

/* Returns true if `lib_path` is missing, was not built by THIS tur (D-3), OR
 * any .tur under `root` is newer than the library. Conservative: treats
 * unreadable libs and stat() failures as "rebuild needed" so the next
 * invocation tries again rather than serving stale code. */
static bool needs_rebuild(const char *root, const char *lib_path,
                          const char *tur_bin) {
    struct stat lib_st;
    if (stat(lib_path, &lib_st) != 0) return true;
    /* `tur_bin` NULL means the caller is only asking about SOURCE freshness
     * (tur_spice_image_is_fresh, for an image already loaded and vetted). */
    if (tur_bin && !repl_cache_stamp_matches(lib_path, tur_bin)) return true;
    int64_t lib_mtime = stat_mtime_ns(&lib_st);
    int64_t newest = newest_tur_mtime(root, 0);
    return newest > lib_mtime;
}

/* ------------------------------------------------------------------ */
/* Cache layout + .gitignore handshake                                */
/* ------------------------------------------------------------------ */

static int ensure_cache_dir(const char *root, char *out, size_t cap) {
    snprintf(out, cap, "%s/.tur-repl-cache", root);
    struct stat st;
    if (stat(out, &st) == 0 && S_ISDIR(st.st_mode)) return 0;
    if (mkdir(out, 0755) != 0 && errno != EEXIST) {
        fprintf(stderr, "tur repl: cannot create %s: %s\n",
                out, strerror(errno));
        return -1;
    }
    /* Best-effort .gitignore handshake: if the project already has a
     * .gitignore, append `.tur-repl-cache/` when not already listed.
     * Don't *create* a .gitignore when one is absent -- that would be
     * surprising in non-git projects. */
    char gi[4200];
    snprintf(gi, sizeof(gi), "%s/.gitignore", root);
    FILE *gf = fopen(gi, "r");
    if (!gf) return 0;
    bool seen = false;
    char line[256];
    while (fgets(line, sizeof(line), gf)) {
        /* trim trailing whitespace */
        size_t ll = strlen(line);
        while (ll > 0 && (line[ll-1] == '\n' || line[ll-1] == '\r'
                          || line[ll-1] == ' ' || line[ll-1] == '\t')) {
            line[--ll] = '\0';
        }
        if (strcmp(line, ".tur-repl-cache") == 0
            || strcmp(line, ".tur-repl-cache/") == 0
            || strcmp(line, "/.tur-repl-cache") == 0
            || strcmp(line, "/.tur-repl-cache/") == 0) {
            seen = true;
            break;
        }
    }
    fclose(gf);
    if (!seen) {
        FILE *af = fopen(gi, "a");
        if (af) {
            fputs("\n# Added by `tur repl` (RP3): cached spice .so + manifest.\n"
                  ".tur-repl-cache/\n", af);
            fclose(af);
        }
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Subprocess invocation: tur build --shared                          */
/* ------------------------------------------------------------------ */

/* Quoting and the null device come from platform_proc.h.  The local
 * shell_quote here single-quoted its arguments, which cmd.exe does not
 * recognise as quoting at all -- the spice build subprocess received a
 * literal 'C:\path' for every argument and could not run. */

static int run_build(const char *tur_bin, const char *root,
                     const char *lib_path, const char *manifest_path) {
    char q_bin[1024], q_root[4400], q_lib[4400], q_mf[4400];
    if (tur_shell_quote(tur_bin,       q_bin,  sizeof(q_bin))  != 0
     || tur_shell_quote(root,          q_root, sizeof(q_root)) != 0
     || tur_shell_quote(lib_path,      q_lib,  sizeof(q_lib))  != 0
     || tur_shell_quote(manifest_path, q_mf,   sizeof(q_mf))   != 0) {
        fprintf(stderr, "tur repl: build command too long to quote\n");
        return -1;
    }
    /* First attempt: silent. If it fails, retry with output visible so
     * the user sees the actual diagnostic from the compiler. */
    char cmd[20000];
    snprintf(cmd, sizeof(cmd),
             "%s build --shared --bundle-deps %s -o %s --manifest %s >" TUR_DEVNULL " 2>&1",
             q_bin, q_root, q_lib, q_mf);
    /* cmd.exe eats the outer quote pair of a /c string, so give it one of
     * ours to eat -- otherwise a quoted program plus quoted arguments comes
     * back as "The filename, directory name, or volume label syntax is
     * incorrect." and no build runs. */
    char wrapped[20008];
    if (tur_shell_command(cmd, wrapped, sizeof(wrapped)) != 0) {
        fprintf(stderr, "tur repl: build command too long\n");
        return -1;
    }
    int rc = system(wrapped);
    if (rc == 0) return 0;
    fprintf(stderr,
            "tur repl: spice rebuild failed; replaying with full output:\n");
    snprintf(cmd, sizeof(cmd),
             "%s build --shared --bundle-deps %s -o %s --manifest %s",
             q_bin, q_root, q_lib, q_mf);
    if (tur_shell_command(cmd, wrapped, sizeof(wrapped)) != 0) return -1;
    int _sys_ret = system(wrapped); (void)_sys_ret;
    /* RP7: tell the user what to do next. (reload) re-runs the same
     * build subprocess after they've fixed the source, so they don't
     * have to restart the REPL on every compile error. */
    fprintf(stderr,
            "tur repl: fix the error above, then type (reload) at the "
            "prompt to retry.\n");
    return -1;
}

/* ------------------------------------------------------------------ */
/* Manifest parsing                                                   */
/* ------------------------------------------------------------------ */

static char class_for_tag(const char *tag, bool is_return) {
    if (strcmp(tag, ":int")    == 0
     || strcmp(tag, ":bool")   == 0
     || strcmp(tag, ":cstr")   == 0
     || strcmp(tag, ":ptr")    == 0
     || strcmp(tag, ":int8")   == 0
     || strcmp(tag, ":int16")  == 0
     || strcmp(tag, ":int32")  == 0
     || strcmp(tag, ":int64")  == 0
     || strcmp(tag, ":uint8")  == 0
     || strcmp(tag, ":uint16") == 0
     || strcmp(tag, ":uint32") == 0
     || strcmp(tag, ":uint64") == 0
     || strcmp(tag, ":any")    == 0
     || strcmp(tag, ":never")  == 0) {
        return 'i';
    }
    /* A by-value record: its bytes cross through a pointer (ffi_thunk.c). */
    if (strcmp(tag, ":record") == 0) return 's';
    if (strcmp(tag, ":float")   == 0
     || strcmp(tag, ":float32") == 0
     || strcmp(tag, ":float64") == 0) {
        return 'f';
    }
    if (is_return && strcmp(tag, ":void") == 0) return 'v';
    return '?';
}

/* Skip whitespace; return pointer to first non-space char (or end). */
static char *skip_ws(char *p) {
    while (*p == ' ' || *p == '\t') p++;
    return p;
}

/* Append an export row; resizes the array as needed. Takes ownership of
 * the three char* fields (frees them on failure). */
/* Takes ownership of `module`, `name`, `mangled`, and the heap-allocated
 * `arg_classes` (length n_args; may be NULL when n_args == 0). */
static int append_export(TurSpiceImage *img, char *module, char *name,
                         char *mangled, char ret_class,
                         char *arg_classes, uint32_t n_args,
                         bool is_variadic, char rest_class, void *fn_ptr,
                         TurFfiShimFn ffi_shim) {
    if (img->n_exports == img->cap_exports) {
        uint32_t nc = img->cap_exports ? img->cap_exports * 2 : 8;
        TurSpiceExport *na = realloc(img->exports, nc * sizeof(*na));
        if (!na) {
            free(module); free(name); free(mangled); free(arg_classes);
            return -1;
        }
        img->exports = na;
        img->cap_exports = nc;
    }
    TurSpiceExport *e = &img->exports[img->n_exports++];
    e->module = module;
    e->name = name;
    e->mangled = mangled;
    e->ret_class = ret_class;
    e->ret_tag = 0;
    e->n_args = n_args;
    e->is_variadic = is_variadic;
    e->rest_class = rest_class;
    e->fn_ptr = fn_ptr;
    e->ffi_shim = ffi_shim;
    e->arg_classes = arg_classes;  /* owned; freed in spice_export_free */
    return 0;
}

/* Parse `<mod>/<name> -> <mangled> :: (<tags>) -> <ret>` into one
 * export row and dlsym the symbol. Mutates the line buffer in place.
 * Return codes:
 *    0 -- success.
 *   -1 -- malformed line; caller prints the original (pre-mutation)
 *         line so the user sees what was wrong.
 *   -2 -- line parsed cleanly but dlsym failed. This path already
 *         printed the detailed "stale exports.manifest" diagnostic;
 *         caller should propagate without adding noise. */
static int parse_manifest_line(TurSpiceImage *img, char *line) {
    char *p = skip_ws(line);
    if (*p == '\0' || *p == '#') return 0;     /* blank / comment */
    /* `<module>/<name> -> ...`, where a module name nests (`stats/rng`):
     * the name is what follows the LAST slash before the arrow.  Splitting
     * at the first one bound `stats/rng/rng-make` as module `stats`, name
     * `rng/rng-make`, so no export of a nested module had its own name. */
    char *arrow = strstr(p, " -> ");
    if (!arrow) return -1;
    char *slash = NULL;
    for (char *q = p; q < arrow; q++) if (*q == '/') slash = q;
    if (!slash) return -1;
    *slash = '\0';
    char *module = strdup(p);
    p = skip_ws(slash + 1);
    /* name (up to ` -> `) */
    *arrow = '\0';
    /* defn name may have trailing whitespace */
    char *end = arrow;
    while (end > p && (*(end - 1) == ' ' || *(end - 1) == '\t')) end--;
    *end = '\0';
    char *name = strdup(p);
    p = skip_ws(arrow + 4);
    /* mangled symbol (up to ` :: `) */
    char *colons = strstr(p, " :: ");
    if (!colons) { free(module); free(name); return -1; }
    *colons = '\0';
    end = colons;
    while (end > p && (*(end - 1) == ' ' || *(end - 1) == '\t')) end--;
    *end = '\0';
    char *mangled = strdup(p);
    p = skip_ws(colons + 4);
    /* `(<tags>) -> <ret>` */
    if (*p != '(') { free(module); free(name); free(mangled); return -1; }
    p++;
    char *close = strchr(p, ')');
    if (!close) { free(module); free(name); free(mangled); return -1; }
    *close = '\0';
    /* arg_classes is grown on demand to the (arbitrary) argument count; no
     * fixed-arity cap.  Ownership transfers to the export via append_export. */
    char    *arg_classes = NULL;
    uint32_t n_args = 0;
    uint32_t cap_args = 0;
    bool is_variadic = false;
    char rest_class = 'i';
    #define ARG_PUSH(cls)                                                     \
        do {                                                                  \
            if (n_args == cap_args) {                                         \
                cap_args = cap_args ? cap_args * 2 : 8;                        \
                char *_na = (char *)realloc(arg_classes, cap_args);           \
                if (!_na) { free(arg_classes); free(module); free(name);      \
                            free(mangled); return -1; }                       \
                arg_classes = _na;                                            \
            }                                                                 \
            arg_classes[n_args++] = (cls);                                    \
        } while (0)
    char *tok = strtok(p, " \t");
    while (tok) {
        if (strcmp(tok, "&") == 0) {
            is_variadic = true;
            /* The next token is the rest-arg's element tag.  The callee
             * receives a single cons-list pointer (int64_t) regardless, so
             * the SLOT class is 'i' -- the ELEMENT class only decides how
             * the REPL marshaller packs each cons head (S2: floats go in as
             * IEEE-754 bit patterns, mirroring the compiled call sites'
             * union reinterpret).
             *
             * The rest formal is already in the emitted positional tags --
             * fd->n_params includes it, so a variadic line reads e.g.
             * `(:int :int & :int)` for one positional + rest.  The slot was
             * therefore pushed by the loop above; OVERWRITE its class with
             * 'i' rather than pushing a second one (the old push-again
             * over-counted the arity by one, latent while variadic exports
             * were rejected outright, and would also have classed a :float
             * rest slot 'f' when the callee takes a pointer). */
            char *rt = strtok(NULL, " \t");
            if (rt) rest_class = class_for_tag(rt, /*is_return=*/false);
            if (rest_class != 'f') rest_class = 'i';  /* unknown/poly -> 'i' */
            if (n_args > 0) arg_classes[n_args - 1] = 'i';
            else ARG_PUSH('i');   /* defensive: foreign manifest shape */
            break;
        }
        ARG_PUSH(class_for_tag(tok, /*is_return=*/false));
        tok = strtok(NULL, " \t");
    }
    #undef ARG_PUSH
    p = skip_ws(close + 1);
    if (strncmp(p, "->", 2) != 0) {
        free(module); free(name); free(mangled); free(arg_classes); return -1;
    }
    p = skip_ws(p + 2);
    /* The ret tag runs to whitespace or end. */
    char *ret_end = p;
    while (*ret_end && *ret_end != ' ' && *ret_end != '\t'
           && *ret_end != '\n' && *ret_end != '\r') ret_end++;
    *ret_end = '\0';
    char ret_class = class_for_tag(p, /*is_return=*/true);
    char ret_tag = strcmp(p, ":cstr") == 0 ? 'c'
                 : strcmp(p, ":bool") == 0 ? 'b' : 0;
    /* Resolve the symbol -- J2 in-process images resolve through the jit
     * hook's MIR item lookup; the dlopen path keeps dlsym.  Either way a
     * miss is a hard error: the manifest and the image have drifted out of
     * sync. RP7: the message names the artifact so the user knows what to
     * discard, and points at the two fixes (one keeps the session alive,
     * the other forces a full clean rebuild). */
    void *fn_ptr = NULL;
    const char *derr = NULL;
    if (img->jit_image && g_jit_hook && g_jit_hook->sym) {
        fn_ptr = g_jit_hook->sym(img->jit_image, mangled);
    } else {
        dlerror();
        fn_ptr = dlsym(img->handle, mangled);
        derr = dlerror();
    }
    if (!fn_ptr || derr) {
        fprintf(stderr,
                "tur repl: stale exports.manifest -- it lists symbol '%s'\n"
                "          but it is not present in %s\n"
                "          (%s)\n"
                "          Fix: type (reload) at the prompt, or run\n"
                "               `rm -rf .tur-repl-cache` and restart the REPL.\n",
                mangled,
                img->jit_image ? "the in-process jit image" : img->lib_path,
                derr ? derr : "symbol not found");
        free(module); free(name); free(mangled); free(arg_classes);
        return -2;  /* RP7: caller skips the generic "malformed" message */
    }
    /* interpreter-arbitrary-arity-ffi (Phase 2): probe for the per-export
     * `<mangled>__ffi` shim.  Its absence is expected and benign -- a spice
     * built before shim emission has no such symbol -- so a NULL result is
     * not an error; the call path falls back to the generated shape table. */
    TurFfiShimFn ffi_shim = NULL;
    {
        size_t shim_len = strlen(mangled) + 5 + 1;  /* "__ffi" + NUL */
        char  *shim_sym = (char *)malloc(shim_len);
        if (shim_sym) {
            snprintf(shim_sym, shim_len, "%s__ffi", mangled);
            void *sym = NULL;
            if (img->jit_image && g_jit_hook && g_jit_hook->sym) {
                sym = g_jit_hook->sym(img->jit_image, shim_sym);
            } else {
                dlerror();
                sym = dlsym(img->handle, shim_sym);
                (void)dlerror();  /* clear; a missing shim is not reported */
            }
            /* The shim is `void(*)(const int64_t*, const double*, int64_t*,
             * double*)`; the lookup hands back a `void *`, which is not
             * portably convertible to a function pointer by a direct cast
             * (ISO C), so round-trip through memcpy. */
            memcpy(&ffi_shim, &sym, sizeof ffi_shim);
            free(shim_sym);
        }
    }
    /* Ownership of arg_classes transfers to the export. */
    int arc = append_export(img, module, name, mangled, ret_class,
                            arg_classes, n_args, is_variadic, rest_class,
                            fn_ptr, ffi_shim);
    if (arc == 0) img->exports[img->n_exports - 1].ret_tag = ret_tag;
    return arc;
}

/* J2: parse manifest TEXT (the jit hook returns it in memory; there is no
 * exports.manifest file on the in-process path).  Same per-line semantics
 * as load_manifest below. */
static int load_manifest_text(TurSpiceImage *img, const char *text) {
    int rc = 0;
    const char *p = text;
    while (*p) {
        const char *nl = strchr(p, '\n');
        size_t ll = nl ? (size_t)(nl - p) : strlen(p);
        char *line = (char *)malloc(ll + 1);
        if (!line) return -1;
        memcpy(line, p, ll);
        line[ll] = '\0';
        char *snapshot = strdup(line);
        int prc = parse_manifest_line(img, line);
        if (prc == -1) {
            fprintf(stderr, "tur repl: malformed manifest line: %s\n",
                    snapshot ? snapshot : "(out of memory)");
            rc = -1;
        }
        if (prc == -2) rc = -1;  /* detailed diagnostic already printed */
        free(line);
        free(snapshot);
        if (rc != 0) return rc;
        if (!nl) break;
        p = nl + 1;
    }
    return rc;
}

static int load_manifest(TurSpiceImage *img, const char *manifest_path) {
    FILE *f = fopen(manifest_path, "r");
    if (!f) {
        fprintf(stderr, "tur repl: cannot open %s: %s\n",
                manifest_path, strerror(errno));
        return -1;
    }
    /* getline grows `line` as needed, so a manifest row for a very high-arity
     * export (whose arg-tag list can run to many KB) is not truncated. */
    char   *line = NULL;
    size_t  line_cap = 0;
    char   *snapshot = NULL;
    int rc = 0;
    ssize_t nread;
    while ((nread = getline(&line, &line_cap, f)) != -1) {
        /* strip trailing newline */
        size_t ll = (size_t)nread;
        while (ll > 0 && (line[ll-1] == '\n' || line[ll-1] == '\r')) {
            line[--ll] = '\0';
        }
        /* RP7: parse_manifest_line mutates `line` in place via *p='\0'
         * trimmers, so we snapshot the original for diagnostic display
         * before parsing. */
        free(snapshot);
        snapshot = strdup(line);
        int prc = parse_manifest_line(img, line);
        if (prc == -1) {
            fprintf(stderr, "tur repl: malformed manifest line: %s\n",
                    snapshot ? snapshot : "(out of memory)");
            rc = -1;
            break;
        }
        if (prc == -2) {
            /* dlsym failure: parse_manifest_line already printed the
             * detailed "stale exports.manifest" diagnostic. */
            rc = -1;
            break;
        }
    }
    free(line);
    free(snapshot);
    fclose(f);
    return rc;
}

/* ------------------------------------------------------------------ */
/* Entry point                                                        */
/* ------------------------------------------------------------------ */

int tur_spice_image_load(const char *start_dir, const char *tur_bin,
                         TurSpiceImage **out_image) {
    if (out_image) *out_image = NULL;
    if (!start_dir) start_dir = ".";
    if (!tur_bin || !*tur_bin) tur_bin = "tur";

    char *root = find_build_tur_root(start_dir);
    if (!root) return 1;  /* no project here */

    char cache_dir[4200];
    if (ensure_cache_dir(root, cache_dir, sizeof(cache_dir)) != 0) {
        free(root);
        return -1;
    }
    /* RP5: bump a process-monotonic generation each load so the .so
     * sits at a unique path every time. Critical on macOS, where
     * dlopen reuses the existing handle when re-opened by the same
     * absolute path -- which would silently keep the previous version
     * of every spice export resident and make (reload) a no-op even
     * after a successful rebuild. Linux has the same issue once
     * RTLD_NOLOAD is involved; unique paths sidestep both. */
    static unsigned int generation_counter = 0;
    unsigned int gen = generation_counter++;
    char lib_path[4400], manifest_path[4400];
    snprintf(lib_path, sizeof(lib_path), "%s/lib-%u" TUR_SHLIB_EXT,
             cache_dir, gen);
    snprintf(manifest_path, sizeof(manifest_path),
             "%s/exports.manifest", cache_dir);

    /* Sources live under `<root>/src/` by spice convention. Fall back to
     * `<root>` itself for the flat-layout fixtures used by some tests.
     * The freshness check also watches the chosen source dir so we
     * don't scan build.tur, the cache, or sibling docs/tests dirs. */
    char src_dir[4300];
    snprintf(src_dir, sizeof(src_dir), "%s/src", root);
    struct stat src_st;
    const char *build_dir = root;
    if (stat(src_dir, &src_st) == 0 && S_ISDIR(src_st.st_mode)) {
        build_dir = src_dir;
    }

    /* J2 (jit-engine-plan 3.3): with the in-process hook installed, skip
     * the subprocess + .so + dlopen entirely.  Every load compiles fresh
     * in memory -- that IS the point; there is no cached artifact to be
     * stale.  Freshness for (reload)/--watch is the image's build stamp
     * (tur_spice_image_is_fresh). */
    if (g_jit_hook && g_jit_hook->build) {
        void *jimg = NULL;
        char *manifest = NULL;
        int64_t stamp = newest_tur_mtime(build_dir, 0);
        if (g_jit_hook->build(build_dir, &jimg, &manifest) != 0) {
            /* ffi-spices-integration-plan S1: a hook failure used to fail
             * the whole load, stranding spices the engine cannot handle
             * (vendored :c-sources it cannot MIR-link, a static-only cmake
             * dep it cannot dlopen).  The subprocess + .so + dlopen path
             * handles all of those; take it instead.  A genuine compile
             * error in the spice will fail again below with the same
             * diagnostic, which is the same end state as before, one build
             * slower. */
            fprintf(stderr,
                    "tur repl: in-process jit load failed; falling back to "
                    "the subprocess build.\n");
            goto subprocess_path;
        }
        TurSpiceImage *img = calloc(1, sizeof(*img));
        if (!img) {
            if (g_jit_hook->free_image) g_jit_hook->free_image(jimg);
            free(manifest); free(root);
            return -1;
        }
        img->root           = root;
        img->build_dir      = strdup(build_dir);
        img->lib_path       = strdup("<in-process jit image>");
        img->jit_image      = jimg;
        img->build_stamp_ns = stamp;
        int mrc = manifest ? load_manifest_text(img, manifest) : -1;
        free(manifest);
        if (mrc != 0) {
            tur_spice_image_free(img);
            return -1;
        }
        *out_image = img;
        return 0;
    }

subprocess_path:
    if (needs_rebuild(build_dir, lib_path, tur_bin)) {
        /* Build the ROOT, with its :spices deps linked in.  Given `<root>/src`
         * -- a directory with no build.tur -- `tur build` never read the
         * manifest, so a spice whose sources import a dep (notebook's tui.tur
         * imports ansi/term) failed "module not found"; and a plain --shared
         * build of the root leaves the deps' code out, which an image dlopen'd
         * on its own cannot do without.  build_dir stays the freshness root. */
        if (run_build(tur_bin, root, lib_path, manifest_path) != 0) {
            free(root);
            return -1;
        }
        repl_cache_stamp_write(lib_path, tur_bin);
    }

    void *handle = dlopen(lib_path, RTLD_NOW | RTLD_LOCAL);
    if (!handle) {
        fprintf(stderr, "tur repl: dlopen(%s) failed: %s\n",
                lib_path, dlerror());
        free(root);
        return -1;
    }

    TurSpiceImage *img = calloc(1, sizeof(*img));
    if (!img) {
        dlclose(handle); free(root); return -1;
    }
    img->root      = root;
    img->build_dir = strdup(build_dir);
    img->lib_path  = strdup(lib_path);
    img->handle    = handle;

    if (load_manifest(img, manifest_path) != 0) {
        tur_spice_image_free(img);
        return -1;
    }
    *out_image = img;
    return 0;
}
