/* paths.c -- files, paths and the process: reading files, the running
 * executable's path, the stdlib root (and the TUR_STDLIB_DIR guard), `#lang`
 * detection, and running a toolchain command (cc_system).
 *
 * Part of the compiler driver (src/driver/); driver.h says how the files
 * share declarations. */

#include "driver.h"

#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif

#include "prof.h"         /* profiling-plan C0+C1: --timings */
#include "lang_dialects.h"  /* `tur dialects`: the `#lang` base axis */
#include "globals.h"

/* Escape a string for JSON output (handles backslash, double-quote, newline). */
void drv_json_escape(const char *s, char *out, size_t cap) {
    size_t i = 0;
    while (*s && i + 4 < cap) {
        if (*s == '\\' || *s == '"') { out[i++] = '\\'; out[i++] = *s++; }
        else if (*s == '\n')          { out[i++] = '\\'; out[i++] = 'n'; s++; }
        else                          { out[i++] = *s++; }
    }
    out[i] = '\0';
}

/* Extract basename from a path.
 *
 * Windows accepts both separators, and realpath() on this platform hands back
 * the backslash spelling ("C:\\tmp\\proj"), so a '/'-only scan finds nothing
 * and returns the whole absolute path as the "basename".  That silently became
 * the output name -- `tur build --shared .` tried to link
 * `<build>/lib/C:\tmp\proj.dll` and ld failed with "Invalid argument".
 * Backslash is only a separator under _WIN32: it is a legal character in a
 * POSIX filename, so splitting on it there would be wrong. */
const char *drv_basename_of(const char *path) {
    const char *s = strrchr(path, '/');
#ifdef _WIN32
    const char *b = strrchr(path, '\\');
    if (b && (!s || b > s)) s = b;
#endif
    return s ? s + 1 : path;
}

/* The length of `name` without its source extension: `.tur` and `.scm`
 * (r7rs-lang-plan open question 4: a `.scm` file is a Scheme program or
 * library) are stripped for a module or output name; anything else is kept
 * whole.  `.tur.sweet` keeps its `.tur` stem on purpose, as before. */
size_t drv_source_stem_len(const char *name, size_t len) {
    if (len >= 4 && (strcmp(name + len - 4, ".tur") == 0 ||
                     strcmp(name + len - 4, ".scm") == 0)) return len - 4;
    return len;
}

/* Every toolchain subprocess the driver runs over emitted C -- cc, the link,
 * the parallel prelude pieces' shell, nm -- goes through here, so --timings
 * charges the wall time the driver spends waiting on it to "cc".  The time is
 * wall time: pieces compiled in parallel overlap, and their CPU is not summed. */
int cc_system(const char *cmd) {
    fflush(NULL);   /* the child's output must not overtake ours */
    prof_enter(PROF_CC);
    int rc = system(cmd);
    prof_exit(PROF_CC);
    return rc;
}

/* Helper to detect language and adjust source for #lang directive. */
ReaderType detect_and_adjust_lang(const char *path, char *src, size_t len,
                                 const char **out_src, size_t *out_len,
                                 LangDialect *out_dialect) {
    ReaderType ext_type = reader_type_from_extension(path);

    /* Always run detect_lang so any leading "#lang ..." line is stripped from
     * the source — otherwise the chosen reader would choke on the '#'. When
     * the extension already selected a non-default reader, the extension
     * still wins for the base; the directive is treated as an optional,
     * redundant hint. */
    const char *src_rest = src;
    size_t len_rest = len;
    const char *bad = NULL;
    size_t bad_len = 0;
    LangDialect dialect = LANG_TURMERIC;
    ReaderType lang_type = detect_lang_dialect(src, len, &src_rest, &len_rest,
                                               &bad, &bad_len, &dialect);

    if (bad) {
        /* lang-unknown-base-diagnostic-names-nothing: the detector hands an
         * unrecognised BASE out through the same slot as a trailing token;
         * the returned type says which.  Name what the user wrote either way. */
        if (lang_type == READER_UNKNOWN) {
            fprintf(stderr,
                    "tur: error [TUR-E0331]: unknown #lang base '%.*s' -- see `tur dialects` for the valid bases (in %s)\n",
                    (int)bad_len, bad, path);
            exit(1);
        }
        /* `#lang` takes a base dialect and nothing else -- hard error
         * (TUR-E0330), mirroring the unimplemented-base exit below. */
        fprintf(stderr,
                "tur: error [TUR-E0330]: `#lang` takes a single base dialect; "
                "unexpected trailing token '%.*s' in %s\n",
                (int)bad_len, bad, path);
        exit(1);
    }

    ReaderType detected_type = (ext_type != READER_TURMERIC) ? ext_type : lang_type;
    /* `.scm` selects the language as well as the reader (r7rs-lang-plan,
     * open question 4): a Scheme file needs no `#lang r7rs` line. */
    {
        LangDialect ext_dialect = lang_dialect_from_extension(path);
        if (ext_dialect != LANG_TURMERIC) dialect = ext_dialect;
    }

    /* Check if the reader is implemented */
    if (!reader_type_is_implemented(detected_type)) {
        fprintf(stderr, "tur: error: #lang %s is not yet implemented\n",
                reader_type_name(detected_type));
        exit(1);
    }

    *out_src = src_rest;
    *out_len = len_rest;
    /* saffron-lang-plan S1: the language axis rides out beside the reader.  The
     * extension override above is deliberately reader-only -- a `.tur.sweet`
     * file is sweet-exp Turmeric unless its `#lang` line says otherwise, and
     * there is no extension that means "Saffron". */
    if (out_dialect) *out_dialect = dialect;
    /* saffron-lang-plan S6: the language's prelude (LangTraits.prelude) joins
     * the stdlib autoload list when the ENTRY file has one.  Set here rather than at each caller
     * because this is the one function every CLI path that opens an entry file
     * goes through, and set unconditionally (not only when true) so a Saffron
     * compile cannot license the prelude for the next Turmeric one in the same
     * process -- see the note on the declaration. */
    g_lang_prelude = lang_traits(dialect)->prelude;
    return detected_type;
}

/* Compute the directory part of a file path (Phase M2: module base dir). */
void drv_dir_of_path(const char *path, char *out, size_t cap) {
    const char *last_slash = strrchr(path, '/');
#ifdef _WIN32
    /* GetModuleFileName / _fullpath return backslashed paths; a '/'-only
     * split sees no separator at all and answers ".", which silently broke
     * the exe-relative SDK walk (the JIT could not find hamt.h). */
    const char *last_bs = strrchr(path, '\\');
    if (last_bs && (!last_slash || last_bs > last_slash)) last_slash = last_bs;
#endif
    if (!last_slash) {
        out[0] = '.'; out[1] = '\0';
    } else {
        size_t n = (size_t)(last_slash - path);
        if (n == 0) n = 1; /* handle "/foo" → "/" */
        if (n >= cap) n = cap - 1;
        memcpy(out, path, n);
        out[n] = '\0';
    }
}

/* SN1: argv[0] stashed at startup as the last-resort exe-path hint.
 * Resolved via platform APIs in drv_get_exe_path() first; argv[0] is only used
 * if those fail. */
const char *g_argv0 = NULL;
/* The whole command line, for the split build's library-unit cache key
 * (split_lib_cache_init): a flag that changes the emitted C changes it. */
int g_main_argc = 0;
const char *const *g_main_argv = NULL;

/* SN1: write the absolute path of the running `tur` executable to out.
 * Returns 0 on success, -1 on failure (in which case out is unmodified). */
int drv_get_exe_path(char *out, size_t cap) {
#ifdef __APPLE__
    uint32_t sz = (uint32_t)cap;
    if (_NSGetExecutablePath(out, &sz) == 0) {
        /* _NSGetExecutablePath returns a path that may contain symlinks
         * or `..`; resolve to a canonical form so the walk-up sees the
         * real directory layout. */
        char real[4096];
        if (realpath(out, real)) {
            size_t rl = strlen(real);
            if (rl < cap) { memcpy(out, real, rl + 1); }
        }
        return 0;
    }
#elif defined(_WIN32)
    /* readlink("/proc/self/exe") has no Windows meaning (the platform_fs.h
     * stub always fails), so this used to fall through to the argv[0] guess
     * -- which loses when tur is invoked as a bare `tur` from PATH.
     * (windows.h is already in scope in this TU via the platform shims.) */
    unsigned long wn = GetModuleFileNameA(NULL, out, (unsigned long)cap);
    if (wn > 0 && wn < (unsigned long)cap) return 0;
#else
    ssize_t n = readlink("/proc/self/exe", out, cap - 1);
    if (n > 0) { out[n] = '\0'; return 0; }
#endif
    if (g_argv0 && strchr(g_argv0, '/')) {
        /* argv[0] contains a path component -- resolve it. */
        if (realpath(g_argv0, out)) return 0;
    }
    return -1;
}

/* SN1: resolved stdlib root directory.  Set lazily on first call to
 * resolve_stdlib_root(), reused thereafter.  Empty string means "we
 * tried and could not find one"; NULL means "not yet attempted". */
static char  g_stdlib_root[4096] = "";
static int   g_stdlib_root_state = 0;  /* 0=unresolved, 1=found, 2=not-found */

/* SN1: locate the stdlib root directory.  Precedence:
 *   1. $TUR_STDLIB_DIR (verbatim, no walk-up)
 *   2. walk up from exe directory looking for `stdlib/macros.tur`
 *      -- supports both the in-tree dev layout (sibling of `build/`) and
 *      a sibling `stdlib/` in any ancestor.
 *   3. `<exe_dir>/../share/turmeric/stdlib` -- prefix-style installed layout.
 *   4. literal "stdlib" -- last-resort fallback matching the legacy
 *      cwd-relative behavior so users running from a repo root still work.
 * Returns a pointer to a static buffer, or NULL if no directory was found
 * and the legacy fallback was not desired.  The current implementation
 * always returns non-NULL: in the worst case it returns the literal
 * "stdlib" so callers can still attempt the open. */
/* Locate the stdlib that ships beside this binary -- the walk-up half of
 * resolve_stdlib_root, factored out so the TUR_STDLIB_DIR branch can compare
 * against it. Returns true and fills `out` on success. */
static bool find_stdlib_beside_exe(char *out, size_t cap) {
    char exe[4096];
    if (drv_get_exe_path(exe, sizeof(exe)) != 0) return false;
    char dir[4096];
    drv_dir_of_path(exe, dir, sizeof(dir));

    /* Walk up to 8 levels looking for `<dir>/stdlib/macros.tur`.
     * macros.tur is the anchor: it's the first file every preload
     * loop touches, so if it's missing nothing else will resolve. */
    char probe[4096];
    for (int depth = 0; depth < 8; depth++) {
        int n = snprintf(probe, sizeof(probe), "%s/stdlib/macros.tur", dir);
        if (n > 0 && (size_t)n < sizeof(probe) && access(probe, R_OK) == 0) {
            int rn = snprintf(out, cap, "%s/stdlib", dir);
            if (rn > 0 && (size_t)rn < cap) return true;
        }
        /* Try installed prefix layout: `<dir>/share/turmeric/stdlib`. */
        n = snprintf(probe, sizeof(probe),
                     "%s/share/turmeric/stdlib/macros.tur", dir);
        if (n > 0 && (size_t)n < sizeof(probe) && access(probe, R_OK) == 0) {
            int rn = snprintf(out, cap, "%s/share/turmeric/stdlib", dir);
            if (rn > 0 && (size_t)rn < cap) return true;
        }
        /* Step up one level.  Backslashes too: GetModuleFileNameA hands back
         * `C:\...\bin\tur.exe`, so a '/'-only search finds no separator, breaks
         * out of the loop on the first iteration, and the walk-up never happens.
         * Only depth 0 is ever probed, which is why a FLAT tree (stdlib beside
         * the exe) resolved while the installed prefix layout -- <prefix>/bin/tur
         * with <prefix>/share/turmeric/stdlib one level up -- did not: `when` came
         * back "unknown function or operator", the program then elaborated without
         * macros or CPS, and the emitted C referenced DK without defining it.
         * drv_dir_of_path above already carries this fix and the comment explaining
         * it; this loop did not get it. */
        char *slash = strrchr(dir, '/');
#ifdef _WIN32
        char *bs = strrchr(dir, '\\');
        if (bs && (!slash || bs > slash)) slash = bs;
#endif
        if (!slash || slash == dir) break;
        *slash = '\0';
    }
    return false;
}

/* True when two directory paths denote the same directory. realpath() first
 * so a symlinked or trailing-slash spelling of the same dir does not read as
 * a mismatch; plain compare when either cannot be resolved. */
static bool same_dir(const char *a, const char *b) {
    char ra[4096], rb[4096];
    if (realpath(a, ra) && realpath(b, rb)) return strcmp(ra, rb) == 0;
    return strcmp(a, b) == 0;
}

/* stdlib-dir-guard-accepts-mismatched-stdlib: does this stdlib belong to THIS
 * compiler?  `<root>/VERSION` carries the release the stdlib shipped with,
 * written beside the top-level VERSION and kept in lockstep with it by
 * tests/check-stdlib-version-stamp.sh (ctest `tur_stdlib_version_stamp`).
 *
 * The older probe (a readable macros.tur) proves the directory IS a stdlib.  It
 * does not prove it is OURS, and a stdlib from another release is the more
 * common stale case -- a version manager keeps every install on disk.  The
 * mismatch then surfaces far downstream as something like "no member named
 * 'is_ok' in 'tur_result_box_t'", naming neither the stdlib nor the variable,
 * and reads as a codegen bug.  That cost a full investigation once; see
 * docs/archive/type-fuzz-src-red-on-clang-21.md. */
typedef enum {
    STDLIB_VER_MATCH,      /* stamped, and it is ours */
    STDLIB_VER_MISMATCH,   /* stamped, and it is someone else's */
    STDLIB_VER_UNKNOWN,    /* no stamp: predates the check, or an odd layout */
} StdlibVerdict;

/* Reads `<root>/VERSION`.  On MISMATCH, `out_ver` receives the stamp found. */
static StdlibVerdict stdlib_version_verdict(const char *root, char *out_ver,
                                            size_t cap) {
    char path[4096];
    int n = snprintf(path, sizeof path, "%s/VERSION", root);
    if (n <= 0 || (size_t)n >= sizeof path) return STDLIB_VER_UNKNOWN;

    FILE *f = fopen(path, "r");
    if (!f) return STDLIB_VER_UNKNOWN;
    char buf[256];
    char *got = fgets(buf, sizeof buf, f);
    fclose(f);
    if (!got) return STDLIB_VER_UNKNOWN;

    /* Strip trailing whitespace; the file is written with a trailing newline. */
    size_t len = strlen(buf);
    while (len > 0 && (buf[len-1] == '\n' || buf[len-1] == '\r' ||
                       buf[len-1] == ' '  || buf[len-1] == '\t')) {
        buf[--len] = '\0';
    }
    if (len == 0) return STDLIB_VER_UNKNOWN;
    if (strcmp(buf, TUR_VERSION) == 0) return STDLIB_VER_MATCH;
    snprintf(out_ver, cap, "%s", buf);
    return STDLIB_VER_MISMATCH;
}

const char *resolve_stdlib_root(void) {
    if (g_stdlib_root_state != 0) {
        return g_stdlib_root[0] ? g_stdlib_root : "stdlib";
    }
    g_stdlib_root_state = 2;  /* assume not-found until we succeed */

    /* TUR_STDLIB_DIR is honored, but no longer taken on faith.
     *
     * It is an ordinary environment variable, so it is inherited by anything
     * a tur process spawns and it outlives the install that set it. A stale
     * value therefore points a freshly built `tur` at a stdlib that has moved
     * or been deleted, and taking it verbatim meant the failure surfaced much
     * later as a wall of `load: cannot open .../macros.tur` errors with
     * nothing naming the variable that caused them.
     *
     * macros.tur is the anchor, matching the walk-up probe below: it is the
     * first file every preload touches, so if it is missing nothing else will
     * resolve either. A directory that fails the check is reported once and
     * then ignored, letting the walk-up find the stdlib shipped beside this
     * binary -- which is nearly always what the user actually wanted. */
    const char *env = getenv("TUR_STDLIB_DIR");
    if (env && *env) {
        size_t n = strlen(env);
        if (n < sizeof(g_stdlib_root)) {
            char probe[4096];
            int pn = snprintf(probe, sizeof(probe), "%s/macros.tur", env);
            if (pn > 0 && (size_t)pn < sizeof(probe) && access(probe, R_OK) == 0) {
                /* The probe above proves the directory *is* a stdlib. It does
                 * not prove it is THIS compiler's stdlib, and a stdlib from
                 * another release is the more common stale case -- a version
                 * manager keeps every install on disk. The mismatch surfaces
                 * far downstream as e.g. "no member named 'is_ok' in
                 * 'tur_result_box_t'", which names neither the stdlib nor the
                 * variable and reads as a codegen bug. Say so here instead.
                 *
                 * Only when a stdlib beside this binary exists and differs:
                 * with no walk-up result there is nothing to disagree with
                 * (the wasm/LSP embedder sets the variable deliberately and
                 * has no exe path), so that case stays silent. */
                char found[64];
                found[0] = '\0';
                StdlibVerdict v = stdlib_version_verdict(env, found, sizeof found);
                if (v == STDLIB_VER_MISMATCH) {
                    fprintf(stderr,
                        "tur: TUR_STDLIB_DIR=%s is turmeric %s, but this "
                        "compiler is v%s.\n"
                        "tur: a mismatched stdlib miscompiles in ways that name "
                        "neither the stdlib nor this variable; unset it if that "
                        "was not deliberate.\n", env, found, TUR_VERSION);
                } else if (v == STDLIB_VER_UNKNOWN) {
                    /* No stamp to compare, so fall back to the older heuristic:
                     * report only when a stdlib beside this binary exists AND
                     * differs.  With no walk-up result there is nothing to
                     * disagree with (the wasm/LSP embedder sets the variable
                     * deliberately and has no exe path), so that stays silent.
                     *
                     * This used to fire whenever the two directories differed,
                     * including when they were the same release -- "someone who
                     * deliberately points at another tree gets a notice they do
                     * not need", which is what the stamp is for.  A CONFIRMED
                     * match now says nothing at all. */
                    char beside[4096];
                    if (find_stdlib_beside_exe(beside, sizeof(beside))
                        && !same_dir(env, beside)) {
                        fprintf(stderr,
                            "tur: TUR_STDLIB_DIR=%s overrides the stdlib beside "
                            "this binary (%s), and carries no VERSION stamp to "
                            "check it against v%s.\n"
                            "tur: a stdlib from a different release will "
                            "miscompile against this compiler; unset it if that "
                            "was not deliberate.\n", env, beside, TUR_VERSION);
                    }
                }
                memcpy(g_stdlib_root, env, n + 1);
                g_stdlib_root_state = 1;
                return g_stdlib_root;
            }
            fprintf(stderr,
                    "tur: ignoring TUR_STDLIB_DIR=%s "
                    "(no readable macros.tur there); "
                    "falling back to the stdlib beside the binary\n", env);
            /* Drop it so every downstream reader -- elab_toplevel.c, the REPL
             * preload, lsp_lite.c -- agrees with the value resolved below
             * instead of re-reading the bad one out of the environment. */
            unsetenv("TUR_STDLIB_DIR");
        }
    }

    if (find_stdlib_beside_exe(g_stdlib_root, sizeof(g_stdlib_root))) {
        g_stdlib_root_state = 1;
        /* The stdlib beside the binary should always be ours.  Only a confirmed
         * MISMATCH is reported: an unstamped one here is an odd or partial
         * install layout, which should not nag on every invocation. */
        {
            char found[64];
            found[0] = '\0';
            if (stdlib_version_verdict(g_stdlib_root, found, sizeof found)
                    == STDLIB_VER_MISMATCH) {
                fprintf(stderr,
                    "tur: the stdlib at %s is turmeric %s, but this compiler is "
                    "v%s -- they must match.\n",
                    g_stdlib_root, found, TUR_VERSION);
            }
        }
        /* Propagate to TUR_STDLIB_DIR so the elaborator (elab_toplevel.c)
         * sees the same value without needing its own copy of this
         * resolver.  Use overwrite=0 so an explicit user override wins;
         * we already returned above if env was set. */
        setenv("TUR_STDLIB_DIR", g_stdlib_root, 0);
        return g_stdlib_root;
    }

    /* Legacy fallback: literal "stdlib" relative to cwd.  Preserved so
     * developers running from the repo root still get a working binary
     * even if the exe-walk failed (e.g. unusual sandbox).  Callers that
     * want a hard error on missing stdlib should check g_stdlib_root_state. */
    return "stdlib";
}

/* Resolve a stdlib basename like "macros.tur" to a full path.
 *
 * Order of precedence:
 *   1. $TUR_STDLIB_DIR/<basename>  -- explicit env-var override
 *   2. <exe_walk_up>/stdlib/<basename> -- walk up from the tur binary
 *   3. ./stdlib/<basename>         -- legacy cwd-relative fallback
 *
 * Returns a pointer to `out` (NUL-terminated). On overflow, the path is
 * truncated and a warning is emitted to stderr; callers still get a usable
 * pointer.
 */
const char *tur_stdlib_path(const char *basename,
                            char *out, size_t outlen) {
    const char *sdir = resolve_stdlib_root();
    int n = snprintf(out, outlen, "%s/%s", sdir, basename);
    if (n < 0 || (size_t)n >= outlen) {
        fprintf(stderr,
                "tur: stdlib path too long for '%s' (TUR_STDLIB_DIR='%s')\n",
                basename, sdir);
    }
    return out;
}

/* SN2: read a file without printing on failure.  Used by stdlib preload
 * loops where missing optional files should be silent.  Returns 0 on
 * success (out + out_len populated, caller frees out), -1 if the file
 * cannot be opened or read.  Never writes to stderr. */
int drv_read_entire_file_quiet(const char *path, char **out, size_t *out_len) {
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return -1; }
    long size = ftell(f);
    if (size < 0) { fclose(f); return -1; }
    if (fseek(f, 0, SEEK_SET) != 0) { fclose(f); return -1; }
    char *buf = (char *)malloc((size_t)size + 1);
    if (!buf) { fclose(f); return -1; }
    size_t nread = fread(buf, 1, (size_t)size, f);
    fclose(f);
    if (nread != (size_t)size) { free(buf); return -1; }
    buf[size] = '\0';
    *out = buf;
    *out_len = (size_t)size;
    return 0;
}

int drv_read_entire_file(const char *path, char **out, size_t *out_len) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "tur: cannot open '%s': %s\n", path, strerror(errno));
        return -1;
    }
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return -1; }
    long size = ftell(f);
    if (size < 0) { fclose(f); return -1; }
    if (fseek(f, 0, SEEK_SET) != 0) { fclose(f); return -1; }

    char *buf = (char *)malloc((size_t)size + 1);
    if (!buf) { fclose(f); fprintf(stderr, "tur: oom\n"); return -1; }
    size_t n = fread(buf, 1, (size_t)size, f);
    fclose(f);
    if (n != (size_t)size) { free(buf); return -1; }
    buf[size] = '\0';
    *out = buf;
    *out_len = (size_t)size;
    return 0;
}

int drv_is_directory(const char *path) {
    struct stat st;
    if (stat(path, &st) != 0) return 0;
    return S_ISDIR(st.st_mode);
}
