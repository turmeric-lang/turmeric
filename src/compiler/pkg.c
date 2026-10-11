/*
 * pkg.c -- Spice: the Turmeric package manager (Phase PKG-1)
 *
 * See pkg.h for the public API.
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

#include "platform_proc.h"
#include "tur_argcheck.h"
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "arena.h"
#include "buf.h"
#include "diag.h"
#include "fmt.h"
#include "forms.h"
#include "pkg.h"
#include "global.h"
#include "reader.h"
#include "symbols.h"
#include "platform_fs.h"
#include "runtime/sha256.h"

/* ================================================================== */
/* Internal helpers                                                     */
/* ================================================================== */

/* Copy a StrSlice into a heap-allocated NUL-terminated string. */
static char *ss_dup(StrSlice s) {
    char *out = (char *)malloc(s.len + 1);
    if (!out) return NULL;
    memcpy(out, s.p, s.len);
    out[s.len] = '\0';
    return out;
}

/* Run a command and capture stdout into a heap-allocated string.
 * Returns NULL on failure.  Caller must free(). */
static char *run_capture(const char *cmd) {
    FILE *f = popen(cmd, "r");
    if (!f) return NULL;
    Buf b;
    buf_init(&b);
    char line[256];
    while (fgets(line, sizeof(line), f))
        buf_puts(&b, line);
    int rc = pclose(f);
    if (rc != 0) {
        buf_free(&b);
        return NULL;
    }
    /* NUL-terminate and strip trailing newline */
    if (b.len == 0) { buf_free(&b); return NULL; }
    buf_putc(&b, '\0');
    /* trim trailing whitespace */
    char *p = b.data + b.len - 2; /* last char before NUL */
    while (p >= b.data && (*p == '\n' || *p == '\r' || *p == ' '))
        *p-- = '\0';
    char *result = tur_strdup(b.data);
    buf_free(&b);
    return result;
}

/* Return ISO-8601 UTC timestamp in a static buffer (thread-unsafe but fine
 * here since package operations are single-threaded). */
static const char *iso_now(void) {
    static char buf[32];
    time_t t = time(NULL);
    struct tm *tm = gmtime(&t);
    strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", tm);
    return buf;
}

/* Ensure a directory exists (creates intermediate parents). */
static bool mkdirp(const char *path) {
    struct stat st;
    if (stat(path, &st) == 0) return true;
    /* Try to create it */
    if (mkdir(path, 0755) == 0) return true;
    if (errno == EEXIST) return true;
    /* Walk up and recurse */
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

/* ================================================================== */
/* :tur-version is validated inline in the defpackage key loop (below) so the
 * caret lands on the range the user wrote; defined further down beside the
 * version-range helpers it depends on. */
static void pkg_check_tur_version_span(const char *range, Span span);

/* Form-walking helpers for defpackage parsing                         */
/* ================================================================== */

/* Get a value from an F_MAP by keyword name (without colon).
 * Returns NULL if not found. */
static const Form *map_get_kw(const Form *map, const char *kw) {
    if (!map || (map->tag != F_MAP && map->tag != F_MAP_LITERAL)) return NULL;
    const FormList *fl = &map->as.list;
    for (uint32_t i = 0; i + 1 < fl->len; i += 2) {
        const Form *key = fl->items[i];
        if (key->tag == F_KEYWORD && strcmp(key->as.sym->name, kw) == 0)
            return fl->items[i + 1];
    }
    return NULL;
}

/* Emit a diagnostic when a build.tur keyword expected a map but got something
 * else. The most common mistake by far is writing a bare `{...}` instead of
 * `#map{...}`. Without this, parse_cmake_deps and parse_spices would silently
 * skip everything and write empty lockfiles.
 *
 * docs/archive/spice-guides-bare-brace-manifest-syntax.md: the hint used to be
 * gated on `got->tag == F_CONTRACT_TYPE`, from when a bare `{...}` read as a
 * contract-type annotation. Contract types moved to `#refine{...}` and bare
 * `{` is now unconditionally SRFI-105 curly-infix (reader.c), so that tag never
 * appears in a manifest slot and the one diagnostic that could teach the fix
 * had gone dead -- leaving a bare ":spices must be a map" with no hint of what
 * a map looks like. Suggest the spelling unconditionally: the hint is useful
 * for ANY wrong shape here, and it makes every stale copy of the docs
 * self-correcting. Curly-infix gets the extra sentence naming what the reader
 * actually saw, since that is the case a user is overwhelmingly in. */
static void report_non_map(const Form *got, const char *what) {
    if (!got) return;
    /* A bare `{a b c}` reads as curly-infix, which the reader lowers to an
     * ordinary call form -- there is no distinguishing tag left by the time it
     * reaches here, so key on the source text at the span instead. */
    const SourceFile *sf = diag_source_file(got->span.file_id);
    bool bare_brace = sf && sf->src && got->span.off_start < sf->len &&
                      sf->src[got->span.off_start] == '{';
    diag_emit(DIAG_ERROR, got->span,
              "build.tur: %s must be a map -- use `#map{...}`%s", what,
              bare_brace
                ? " (a bare `{...}` is curly-infix arithmetic, not a map)"
                : "");
}

/* Reject an effect-row literal sitting in a manifest slot that means "map".
 *
 * The reader gives `#{...}`, `#fx{...}`, and `@{...}` all the same F_MAP tag
 * and distinguishes them only by `fx_prov`, so a slot that checks the tag
 * alone silently accepts an effect row as a map.  `parse_exports` has guarded
 * against this since exports-map-syntax-tighten-plan; this helper is that
 * plan's follow-up audit, shared by every other map-shaped manifest slot.
 *
 * `alt` names an additional accepted shape (e.g. "a vector of source paths")
 * or is NULL.  Returns true when `f` is an effect row -- a diagnostic has
 * been emitted and the caller should bail. */
static bool reject_fx_row(const Form *f, const char *what, const char *alt) {
    if (!f || f->tag != F_MAP) return false;
    const char *spelling =
        (f->fx_prov == (uint8_t)PROV_FX_EXPLICIT)  ? "#fx{...}" :
        (f->fx_prov == (uint8_t)PROV_FX_AT_LEGACY) ? "@{...}"   : NULL;
    if (!spelling) return false;
    diag_emit(DIAG_ERROR, f->span,
              "TUR-E0620: build.tur: %s expects a map (`#{...}` or "
              "`#map{...}`)%s%s; got an effect-row literal (`%s`).  Effect "
              "rows are the spelling used in function type annotations "
              "(e.g. `#fx{Net}`), not manifest maps.",
              what, alt ? " or " : "", alt ? alt : "", spelling);
    return true;
}

/* True when `f` is usable as a manifest map -- `#{...}` (F_MAP, non-effect
 * provenance) or `#map{...}` (F_MAP_LITERAL).  Emits the appropriate
 * diagnostic and returns false otherwise. */
static bool expect_map(const Form *f, const char *what) {
    if (reject_fx_row(f, what, NULL)) return false;
    if (f && (f->tag == F_MAP || f->tag == F_MAP_LITERAL)) return true;
    report_non_map(f, what);
    return false;
}

/* Extract a string from an F_STR form, or NULL. */
static char *form_str_dup(const Form *f) {
    if (!f || f->tag != F_STR) return NULL;
    return ss_dup(f->as.s);
}

/* Extract a bool from an F_BOOL form. */
static bool form_bool_val(const Form *f) {
    if (!f || f->tag != F_BOOL) return false;
    return f->as.b;
}

/* Parse the :spices map: #{"name" #{:url "..." :ref "..."} ...} */
static bool parse_spices(const Form *map, PkgManifest *m) {
    if (!map) return true; /* missing keyword is OK */
    if (!expect_map(map, ":spices")) return false;
    const FormList *fl = &map->as.list;
    int cap = 4;
    m->spices = (PkgSpice *)malloc(cap * sizeof(PkgSpice));
    if (!m->spices) return false;
    m->n_spices = 0;

    for (uint32_t i = 0; i + 1 < fl->len; i += 2) {
        const Form *key = fl->items[i];
        const Form *val = fl->items[i + 1];
        if (key->tag != F_STR) continue;
        if (!val) continue;
        if (!expect_map(val, "entry in :spices")) continue;

        if (m->n_spices >= cap) {
            cap *= 2;
            m->spices = (PkgSpice *)realloc(m->spices,
                                             cap * sizeof(PkgSpice));
            if (!m->spices) return false;
        }
        PkgSpice *s = &m->spices[m->n_spices++];
        memset(s, 0, sizeof(*s));
        s->name     = ss_dup(key->as.s);
        s->url      = form_str_dup(map_get_kw(val, "url"));
        s->ref      = form_str_dup(map_get_kw(val, "ref"));
        s->path     = form_str_dup(map_get_kw(val, "path"));
        s->subdir   = form_str_dup(map_get_kw(val, "subdir"));
        /* WP2 (D-5, D-6): `name` and `:ref` are joined into the directory
         * `tur fetch` CREATES -- `spices/<name>-<ref>` -- and `:ref` is also
         * handed to git as a positional argument.  Neither was checked, so a
         * transitive manifest could choose a write location outside `spices/`
         * with a `..`, or hand git an option with a leading `-`.  A spice name
         * is an identifier and a git ref has git's own rules; both are path
         * segments here, which is the tightest thing that is also true. */
        if (!tur_path_segment_ok(s->name)) {
            diag_emit(DIAG_ERROR, key->span,
                "build.tur: spice name '%s' is not a usable directory name -- "
                "letters, digits, '.', '_', '-' and '+' only, not starting "
                "with '-' or '.'", s->name ? s->name : "");
            return false;
        }
        if (s->ref && !tur_git_ref_ok(s->ref)) {
            diag_emit(DIAG_ERROR, val->span,
                "build.tur: spice \"%s\" has an unusable :ref '%s' -- a ref "
                "may hold letters, digits, '.', '_', '-', '+' and '/', must "
                "not start with '-', '.' or '/', and must not contain '..'",
                s->name, s->ref);
            return false;
        }
        if (s->url && !tur_url_ok(s->url)) {
            diag_emit(DIAG_ERROR, val->span,
                "build.tur: spice \"%s\" has an unusable :url '%s' -- it must "
                "not start with '-' or contain whitespace or a character the "
                "shell would read as syntax", s->name, s->url);
            return false;
        }
        /* `:subdir` names a directory INSIDE the fetched repo, so it is
         * contained.  `:path`, by contrast, is documented as a sibling or
         * monorepo pointer and legitimately climbs (`../leaf`,
         * `../../turmeric-spices/spices/zlib`) -- it is a place we READ, never
         * a place we create, so it gets shell-safety and no containment. */
        if (s->subdir && !tur_rel_path_ok(s->subdir)) {
            diag_emit(DIAG_ERROR, val->span,
                "build.tur: spice \"%s\" has an unusable :subdir '%s' -- it "
                "must be a relative path inside the fetched repository (no "
                "'..', no leading '/', no shell metacharacters)",
                s->name, s->subdir);
            return false;
        }
        if (s->path && (!tur_arg_is_shell_safe(s->path) || s->path[0] == '-')) {
            diag_emit(DIAG_ERROR, val->span,
                "build.tur: spice \"%s\" has an unusable :path '%s' -- it must "
                "not start with '-' or contain a character the shell would "
                "read as syntax", s->name, s->path);
            return false;
        }
        const Form *opt_f = map_get_kw(val, "optional");
        s->optional = form_bool_val(opt_f);
        /* global-spice-library-consumption: `#{:global true}` resolves through
         * the `tur install` registry rather than <root>/spices. */
        s->is_global = form_bool_val(map_get_kw(val, "global"));
        if (s->is_global && (s->url || s->path)) {
            /* diag_emit, not fprintf: pkg_manifest_read judges the read by
             * diag_had_error(), so a bare stderr write would leave the
             * manifest ACCEPTED with a message nobody acted on. */
            diag_emit(DIAG_ERROR, val->span,
                "spice \"%s\" declares :global true together with %s -- a "
                "global dep resolves from the installed-spice registry, so it "
                "takes neither",
                s->name ? s->name : "?", s->url ? ":url" : ":path");
            return false;
        }
    }
    return true;
}

/* Forward declaration (parse_str_vec is defined after parse_cmake_deps). */
static bool parse_str_vec(const Form *f, char ***out, int *n_out);

/* Parse a single cmake dep options map: #{:KEY "VAL" ...}.  `label` names the
 * key being parsed so a shape error points at `:options` or `:wasm-options`
 * rather than always the former. */
static bool parse_cmake_opts(const Form *map, const char *label,
                              PkgCmakeOpt **out_opts, int *out_n) {
    *out_opts = NULL;
    *out_n    = 0;
    if (!map) return true;
    if (!expect_map(map, label)) return false;
    const FormList *fl = &map->as.list;
    int cap = 4;
    *out_opts = (PkgCmakeOpt *)malloc(cap * sizeof(PkgCmakeOpt));
    if (!*out_opts) return false;
    for (uint32_t i = 0; i + 1 < fl->len; i += 2) {
        const Form *k = fl->items[i];
        const Form *v = fl->items[i + 1];
        if (k->tag != F_KEYWORD) continue;
        if (!v || v->tag != F_STR) continue;
        if (*out_n >= cap) {
            cap *= 2;
            *out_opts = (PkgCmakeOpt *)realloc(*out_opts,
                                                cap * sizeof(PkgCmakeOpt));
            if (!*out_opts) return false;
        }
        PkgCmakeOpt *o = &(*out_opts)[(*out_n)++];
        o->key = tur_strdup(k->as.sym->name);
        o->val = ss_dup(v->as.s);
    }
    return true;
}

/* Parse the :cmake-deps map */
static bool parse_cmake_deps(const Form *map, PkgManifest *m) {
    if (!map) return true; /* missing keyword is OK */
    if (!expect_map(map, ":cmake-deps")) return false;
    const FormList *fl = &map->as.list;
    int cap = 4;
    m->cmake_deps = (PkgCmakeDep *)malloc(cap * sizeof(PkgCmakeDep));
    if (!m->cmake_deps) return false;
    m->n_cmake_deps = 0;

    for (uint32_t i = 0; i + 1 < fl->len; i += 2) {
        const Form *key = fl->items[i];
        const Form *val = fl->items[i + 1];
        if (key->tag != F_STR) continue;
        if (!val) continue;
        if (!expect_map(val, "entry in :cmake-deps")) continue;

        if (m->n_cmake_deps >= cap) {
            cap *= 2;
            m->cmake_deps = (PkgCmakeDep *)realloc(m->cmake_deps,
                                                     cap * sizeof(PkgCmakeDep));
            if (!m->cmake_deps) return false;
        }
        PkgCmakeDep *d = &m->cmake_deps[m->n_cmake_deps++];
        memset(d, 0, sizeof(*d));
        d->name       = ss_dup(key->as.s);
        d->url        = form_str_dup(map_get_kw(val, "url"));
        d->ref        = form_str_dup(map_get_kw(val, "ref"));
        d->path       = form_str_dup(map_get_kw(val, "path"));
        d->cmake_name = form_str_dup(map_get_kw(val, "cmake-name"));
        d->prefer_system  = form_bool_val(map_get_kw(val, "prefer-system"));
        d->cmake_version  = form_str_dup(map_get_kw(val, "cmake-version"));
        /* WP2 (D-9): every one of these is written into the generated
         * cmake/CMakeLists.txt, which cmake then runs.  The name and
         * cmake-name land in bare positions (`FetchContent_Declare(<name>`,
         * `find_package(<cmake-name>`) where even quoting would not save them,
         * so they must be identifiers; the url and ref are quoted at the write
         * site and need only be free of `$`, `"` and `;`. */
        if (!tur_cmake_ident_ok(d->name)) {
            diag_emit(DIAG_ERROR, key->span,
                "build.tur: cmake-dep name '%s' must be a cmake identifier "
                "(letters, digits, '_' and '-')", d->name ? d->name : "");
            return false;
        }
        if (d->cmake_name && !tur_cmake_ident_ok(d->cmake_name)) {
            diag_emit(DIAG_ERROR, val->span,
                "build.tur: cmake-dep '%s': :cmake-name '%s' must be a cmake "
                "identifier", d->name, d->cmake_name);
            return false;
        }
        if (d->cmake_version && !tur_cmake_value_ok(d->cmake_version)) {
            diag_emit(DIAG_ERROR, val->span,
                "build.tur: cmake-dep '%s': :cmake-version '%s' is not a "
                "usable version string", d->name, d->cmake_version);
            return false;
        }
        if (d->url && !tur_url_ok(d->url)) {
            diag_emit(DIAG_ERROR, val->span,
                "build.tur: cmake-dep '%s': :url '%s' must not start with '-' "
                "or contain whitespace or a character cmake or the shell would "
                "read as syntax", d->name, d->url);
            return false;
        }
        if (d->ref && !tur_git_ref_ok(d->ref)) {
            diag_emit(DIAG_ERROR, val->span,
                "build.tur: cmake-dep '%s': :ref '%s' is not a usable git ref",
                d->name, d->ref);
            return false;
        }
        if (d->path && !tur_arg_is_shell_safe(d->path)) {
            diag_emit(DIAG_ERROR, val->span,
                "build.tur: cmake-dep '%s': :path '%s' contains a character "
                "cmake or the shell would read as syntax", d->name, d->path);
            return false;
        }
        parse_str_vec(map_get_kw(val, "targets"), &d->targets, &d->n_targets);
        /* :link-libs overrides the -l name derived from the target name; the
         * empty list is a meaningful value ("link nothing"), so record key
         * presence separately from the count. */
        const Form *ll_f = map_get_kw(val, "link-libs");
        d->has_link_libs = (ll_f != NULL);
        parse_str_vec(ll_f, &d->link_libs, &d->n_link_libs);
        parse_str_vec(map_get_kw(val, "link-flags"),
                      &d->link_flags, &d->n_link_flags);
        const Form *opts_f = map_get_kw(val, "options");
        parse_cmake_opts(opts_f, ":options", &d->opts, &d->n_opts);
        parse_cmake_opts(map_get_kw(val, "wasm-options"), ":wasm-options",
                         &d->wasm_opts, &d->n_wasm_opts);

        /* :prefer-system needs a :cmake-name to know what to find_package. */
        if (d->prefer_system && !d->cmake_name) {
            diag_emit(DIAG_ERROR, key->span,
                      "build.tur: cmake-dep '%s': :prefer-system true "
                      "requires :cmake-name", d->name);
        }
    }
    return true;
}

/* Parse a string-vector form [a b c] or just a plain F_STR */
static bool parse_str_vec(const Form *f, char ***out, int *n_out) {
    *out   = NULL;
    *n_out = 0;
    if (!f) return true;
    if (f->tag == F_STR) {
        *out    = (char **)malloc(sizeof(char *));
        (*out)[0] = ss_dup(f->as.s);
        *n_out  = 1;
        return true;
    }
    if (f->tag != F_VEC && f->tag != F_LIST) return true;
    const FormList *fl = &f->as.list;
    *out = (char **)malloc(fl->len * sizeof(char *));
    if (!*out) return false;
    *n_out = 0;
    for (uint32_t i = 0; i < fl->len; i++) {
        if (fl->items[i]->tag == F_STR)
            (*out)[(*n_out)++] = ss_dup(fl->items[i]->as.s);
    }
    return true;
}

/* XF1 (experimental-flag-mechanism-plan): parse a :experiments [...] list.
 * Entries may be keywords (:fancy-rows), symbols (fancy-rows), or strings
 * ("fancy-rows"); the bare kebab-case name (no leading ':') is stored. */
static bool parse_experiments(const Form *f, char ***out, int *n_out) {
    *out   = NULL;
    *n_out = 0;
    if (!f) return true;
    if (f->tag != F_VEC && f->tag != F_LIST) {
        report_non_map(f, ":experiments"); /* close enough: "expected a vector" */
        return false;
    }
    const FormList *fl = &f->as.list;
    *out = (char **)malloc(fl->len * sizeof(char *));
    if (!*out) return false;
    for (uint32_t i = 0; i < fl->len; i++) {
        const Form *it = fl->items[i];
        if (it->tag == F_KEYWORD || it->tag == F_SYM)
            (*out)[(*n_out)++] = tur_strdup(it->as.sym->name);
        else if (it->tag == F_STR)
            (*out)[(*n_out)++] = ss_dup(it->as.s);
    }
    return true;
}

/* spices-c-sources-plan: parse + validate a :c-sources / :c-includes vector.
 * Each entry must be a manifest-relative path (absolute paths rejected); for
 * sources (require_c_ext) the extension must be .c/.cc/.cpp and the file must
 * exist, for includes the directory must exist. Any failing entry emits a
 * DIAG_ERROR carrying that entry's span. Valid entries are stored verbatim
 * (as written in build.tur) so the build can re-resolve them relative to the
 * manifest dir. */
/* WP2 (D-1): a `:build-opts` entry that reaches the link line.
 *
 * `:link-libs` entries become `-l<name>`; `:link-flags` entries are spliced in
 * VERBATIM, which is documented (pkg.h) as the only way to spell
 * `-framework Cocoa` -- so one entry may be several space-separated tokens and
 * each is checked on its own.  Before this, a transitive spice could put
 * `-lfoo; touch pwned` in either and the shell ran it while building a project
 * that merely depended on that spice. */
static bool link_entry_tokens_ok(const char *s) {
    if (!s || !*s) return false;
    const char *prev = NULL;
    char prev_buf[256];
    for (const char *q = s; *q; ) {
        while (*q == ' ') q++;
        if (!*q) break;
        const char *t = q;
        while (*q && *q != ' ') q++;
        size_t n = (size_t)(q - t);
        char tok[1024];
        if (n >= sizeof(tok)) return false;
        memcpy(tok, t, n);
        tok[n] = '\0';
        if (!tur_link_token_ok(tok, prev)) return false;
        snprintf(prev_buf, sizeof(prev_buf), "%s", tok);
        prev = prev_buf;
    }
    return true;
}

/* Grammar-check a parsed `:link-libs` / `:link-flags` vector in place, after
 * parse_str_vec has filled it.  Reports through diag so the manifest read
 * fails the way every other bad slot does; `span` is the :build-opts map's,
 * which is the closest span parse_str_vec leaves us. */
static bool check_link_vec(char **vec, int n, const char *what,
                           bool bare_lib_names, Span span) {
    bool ok = true;
    for (int i = 0; i < n; i++) {
        if (!vec[i]) continue;
        bool good = bare_lib_names
            ? (vec[i][0] != '-' && tur_link_value_ok(vec[i]))
            : link_entry_tokens_ok(vec[i]);
        if (!good) {
            diag_emit(DIAG_ERROR, span,
                "build.tur: %s entry '%s' is not a link token this build will "
                "pass to the C compiler.\n"
                "  Allowed: -l<name>, -L<dir>, -I<dir>, -D<key>[=<val>], "
                "-framework <name>, -Wl,<...>, a source or object path, or a "
                "bare toolchain flag -- and nothing the shell reads as syntax.",
                what, vec[i]);
            ok = false;
        }
    }
    return ok;
}

static bool parse_c_path_vec(const Form *f, const char *manifest_dir,
                             const char *what, bool require_c_ext,
                             char ***out, int *n_out) {
    *out   = NULL;
    *n_out = 0;
    if (!f) return true;
    if (f->tag != F_VEC && f->tag != F_LIST && f->tag != F_STR) {
        report_non_map(f, what); /* close enough: "expected a vector" */
        return false;
    }
    /* Normalise a bare string to a one-element list view. */
    const Form *single[1] = { f };
    const Form *const *items;
    uint32_t n;
    if (f->tag == F_STR) {
        items = single;
        n     = 1;
    } else {
        items = (const Form *const *)f->as.list.items;
        n     = f->as.list.len;
    }
    *out = (char **)malloc((n ? n : 1) * sizeof(char *));
    if (!*out) return false;
    for (uint32_t i = 0; i < n; i++) {
        const Form *entry = items[i];
        if (!entry || entry->tag != F_STR) continue;
        char *p = ss_dup(entry->as.s);
        if (!p) continue;
        bool ok = true;
        if (p[0] == '/') {
            diag_emit(DIAG_ERROR, entry->span,
                      "build.tur: %s entry '%s' must be a relative path "
                      "(absolute paths are not allowed)", what, p);
            ok = false;
        }
        /* WP2 (D-1, D-5): the entry is joined onto the manifest's directory
         * and then spliced into the cc command string, so it has to be both
         * CONTAINED (no `..` climbing out of the spice) and free of anything
         * the shell reads as syntax.  The absolute-path check above was the
         * only guard, and `../../etc` passed it. */
        if (ok && !tur_rel_path_ok(p)) {
            diag_emit(DIAG_ERROR, entry->span,
                      "build.tur: %s entry '%s' must stay inside the spice "
                      "directory and contain no shell metacharacters "
                      "(no '..', no backslash, no ; & | ` $ quotes)", what, p);
            ok = false;
        }
        if (ok && require_c_ext) {
            const char *dot = strrchr(p, '.');
            if (!dot || !(strcmp(dot, ".c")   == 0 ||
                          strcmp(dot, ".cc")  == 0 ||
                          strcmp(dot, ".cpp") == 0)) {
                diag_emit(DIAG_ERROR, entry->span,
                          "build.tur: %s entry '%s' must have a .c, .cc, or "
                          ".cpp extension", what, p);
                ok = false;
            }
        }
        if (ok && manifest_dir) {
            char full[4096];
            snprintf(full, sizeof(full), "%s/%s", manifest_dir, p);
            struct stat stbuf;
            bool exists = (stat(full, &stbuf) == 0);
            bool right_kind = exists && (require_c_ext ? S_ISREG(stbuf.st_mode)
                                                       : S_ISDIR(stbuf.st_mode));
            if (!right_kind) {
                diag_emit(DIAG_ERROR, entry->span,
                          "build.tur: %s entry '%s' not found (resolved to '%s')",
                          what, p, full);
                ok = false;
            }
        }
        if (ok) {
            (*out)[(*n_out)++] = p;
        } else {
            free(p);
        }
    }
    return true;
}

/* Parse the :exports field. Accepts either form:
 *   - a map literal `#map{ "mod/name" [sym ...] ... }` -- the canonical
 *     spice form; the module-name keys are captured (the per-module symbol
 *     vectors are not stored here -- consumers that need them re-read the
 *     manifest form).  Also accepts a bare `#{...}` legacy map for
 *     back-compat with spices that predate `#map{...}`.
 *   - a vector ["src/lib.tur" ...] or a bare string -- legacy/path form,
 *     delegated to parse_str_vec.
 *
 * `#fx{...}` (an effect-row literal) is REJECTED here with TUR-E0620.  The
 * reader tags all three of `#{...}`, `#fx{...}`, and (implicitly) `#map{...}`
 * with related F_MAP-family shapes, but effect rows are never a valid
 * `:exports` value.  See docs/archive/exports-map-syntax-tighten-plan.md;
 * the shared reject_fx_row()/expect_map() helpers extend the same guard to
 * every other map-shaped manifest slot.
 *
 * Storing the keys lets the build driver validate declared exports against
 * on-disk sources and lets `tur emit-cmake` enumerate the modules. */
static bool parse_exports(const Form *f, char ***out, int *n_out) {
    *out   = NULL;
    *n_out = 0;
    if (!f) return true;

    /* Reject `#fx{...}` / `@{...}` -- effect rows masquerading as a map. */
    if (reject_fx_row(f, ":exports", "a vector of source paths")) return false;

    if (f->tag != F_MAP && f->tag != F_MAP_LITERAL)
        return parse_str_vec(f, out, n_out);

    const FormList *fl = &f->as.list;
    *out = (char **)malloc((fl->len / 2 + 1) * sizeof(char *));
    if (!*out) return false;
    for (uint32_t i = 0; i + 1 < fl->len; i += 2) {
        const Form *key = fl->items[i];
        if (key->tag == F_STR)
            (*out)[(*n_out)++] = ss_dup(key->as.s);
    }
    return true;
}

/* ================================================================== */
/* Manifest filename resolution (build.tur, build.tur.sweet)            */
/* ================================================================== */

bool pkg_is_manifest_name(const char *name) {
    if (!name) return false;
    return strcmp(name, "build.tur") == 0
        || strcmp(name, "build.tur.sweet") == 0;
}

bool pkg_resolve_manifest_path(const char *dir, char *out, size_t cap) {
    if (!dir || !out || cap == 0) return false;
    struct stat st;
    int n = snprintf(out, cap, "%s/build.tur", dir);
    if (n > 0 && (size_t)n < cap
        && stat(out, &st) == 0 && S_ISREG(st.st_mode))
        return true;
    n = snprintf(out, cap, "%s/build.tur.sweet", dir);
    if (n > 0 && (size_t)n < cap
        && stat(out, &st) == 0 && S_ISREG(st.st_mode))
        return true;
    out[0] = '\0';
    return false;
}

bool pkg_resolve_manifest_cwd(char *out, size_t cap) {
    if (!out || cap == 0) return false;
    struct stat st;
    if (cap > strlen("build.tur")) {
        strcpy(out, "build.tur");
        if (stat(out, &st) == 0 && S_ISREG(st.st_mode)) return true;
    }
    if (cap > strlen("build.tur.sweet")) {
        strcpy(out, "build.tur.sweet");
        if (stat(out, &st) == 0 && S_ISREG(st.st_mode)) return true;
    }
    out[0] = '\0';
    return false;
}

/* ================================================================== */
/* pkg_manifest_read                                                    */
/* ================================================================== */

/* Sticky record that SOME manifest was found and rejected this process.
 *
 * Same problem and same shape as g_tv_rejected below: the manifest is read
 * before compilation, and every compile entry point then calls diag_reset() to
 * clear `had_error_`, which wiped the manifest's own error.  TUR-E0620 (and
 * friends) therefore printed at `error:` severity and the command exited 0.
 * The verdict is recorded here and re-asserted after each diag_reset(). */
static bool g_manifest_malformed = false;
static char g_manifest_malformed_path[4096];
static char g_manifest_malformed_slot[64];   /* ":spices", ... or "" */
/* Report at most once per process: a manifest is read several times per
 * invocation (walk-up for build-dir, again for reader macros, again per
 * dependency), and repeating one verdict per read is pure noise. */
static bool g_manifest_malformed_reported = false;

bool        pkg_manifest_malformed(void) { return g_manifest_malformed; }
const char *pkg_manifest_malformed_path(void) {
    return g_manifest_malformed ? g_manifest_malformed_path : NULL;
}

void pkg_manifest_reassert(void) {
    if (!g_manifest_malformed) return;
    /* Re-emitted per compile, deliberately: had_error_ must be set for THIS
     * compile to fail, and there is no diag API to mark an error without
     * printing.  The brief form keeps the repeat cheap to read. */
    if (g_manifest_malformed_slot[0])
        diag_emit(DIAG_ERROR, SPAN_UNKNOWN,
                  "TUR-E0624: refusing to continue: '%s' could not be read -- "
                  "its %s is malformed (see the error above).  Nothing the "
                  "manifest declares is in effect, including this spice's own "
                  "src/ on the module search path.",
                  g_manifest_malformed_path, g_manifest_malformed_slot);
    else
        diag_emit(DIAG_ERROR, SPAN_UNKNOWN,
                  "TUR-E0624: refusing to continue: '%s' could not be read "
                  "(see the error above).  Nothing the manifest declares is in "
                  "effect, including this spice's own src/ on the module "
                  "search path.",
                  g_manifest_malformed_path);
}

void pkg_manifest_malformed_reset(void) {
    g_manifest_malformed = false;
    g_manifest_malformed_reported = false;
    g_manifest_malformed_path[0] = '\0';
    g_manifest_malformed_slot[0] = '\0';
}

/* Record a found-but-broken manifest.  Idempotent per process: the FIRST
 * rejection is the one reported, since later reads are usually of the same
 * file (walk-up probes) or of deps that failed because of it. */
static void pkg_manifest_mark_malformed_slot(const char *path,
                                             const char *slot) {
    if (!g_manifest_malformed_reported) {
        snprintf(g_manifest_malformed_path, sizeof(g_manifest_malformed_path),
                 "%s", path ? path : "build.tur");
        snprintf(g_manifest_malformed_slot, sizeof(g_manifest_malformed_slot),
                 "%s", slot ? slot : "");
        g_manifest_malformed_reported = true;
    }
    g_manifest_malformed = true;
}

static void pkg_manifest_mark_malformed(const char *path) {
    pkg_manifest_mark_malformed_slot(path, NULL);
}

bool pkg_manifest_read(const char *path, PkgManifest *out) {
    return pkg_manifest_read_status(path, out, NULL);
}

/* Release one manifest slot and reset it to empty.  pkg_manifest_free uses
 * them, and so does the manifest reader when a key appears twice: the later
 * value replaces the earlier one, as it always has, and the earlier one's
 * allocations are released instead of leaked (found by
 * tests/fuzz/fuzz_manifest, security-audit-plan WP4). */
static void str_vec_clear(char ***v, int *n) {
    for (int i = 0; i < *n; i++) free((*v)[i]);
    free(*v);
    *v = NULL;
    *n = 0;
}

static void spices_clear(PkgManifest *m) {
    for (int i = 0; i < m->n_spices; i++) {
        free(m->spices[i].name);
        free(m->spices[i].url);
        free(m->spices[i].ref);
        free(m->spices[i].path);
        free(m->spices[i].subdir);
    }
    free(m->spices);
    m->spices = NULL;
    m->n_spices = 0;
}

static void cmake_deps_clear(PkgManifest *m) {
    for (int i = 0; i < m->n_cmake_deps; i++) {
        free(m->cmake_deps[i].name);
        free(m->cmake_deps[i].url);
        free(m->cmake_deps[i].ref);
        free(m->cmake_deps[i].path);
        free(m->cmake_deps[i].cmake_name);
        free(m->cmake_deps[i].cmake_version);
        for (int j = 0; j < m->cmake_deps[i].n_targets; j++)
            free(m->cmake_deps[i].targets[j]);
        free(m->cmake_deps[i].targets);
        for (int j = 0; j < m->cmake_deps[i].n_link_libs; j++)
            free(m->cmake_deps[i].link_libs[j]);
        free(m->cmake_deps[i].link_libs);
        for (int j = 0; j < m->cmake_deps[i].n_link_flags; j++)
            free(m->cmake_deps[i].link_flags[j]);
        free(m->cmake_deps[i].link_flags);
        for (int j = 0; j < m->cmake_deps[i].n_opts; j++) {
            free(m->cmake_deps[i].opts[j].key);
            free(m->cmake_deps[i].opts[j].val);
        }
        free(m->cmake_deps[i].opts);
        for (int j = 0; j < m->cmake_deps[i].n_wasm_opts; j++) {
            free(m->cmake_deps[i].wasm_opts[j].key);
            free(m->cmake_deps[i].wasm_opts[j].val);
        }
        free(m->cmake_deps[i].wasm_opts);
    }
    free(m->cmake_deps);
    m->cmake_deps = NULL;
    m->n_cmake_deps = 0;
}

static void bins_clear(PkgManifest *m) {
    for (int i = 0; i < m->n_bins; i++) {
        free(m->bin_names[i]);
        free(m->bin_paths[i]);
    }
    free(m->bin_names);
    free(m->bin_paths);
    m->bin_names = NULL;
    m->bin_paths = NULL;
    m->n_bins = 0;
    pkg_web_opts_free(&m->web);
}

bool pkg_manifest_read_status(const char *path, PkgManifest *out,
                              PkgManifestStatus *status) {
    memset(out, 0, sizeof(*out));
    /* `:web`'s defaults are not all zero (ES3, a 128 MB heap), so they have to
     * be installed before the slot parser runs -- a manifest with no `:web`
     * key still describes a buildable web target. */
    pkg_web_opts_defaults(&out->web);
    if (status) *status = PKG_MANIFEST_ABSENT;

    /* Read the file into memory */
    FILE *f = fopen(path, "rb");
    if (!f) {
        /* ABSENT, not malformed: "there is no manifest here" is the normal
         * case for every walk-up probe, and must stay quiet. */
        fprintf(stderr, "spice: cannot open '%s': %s\n", path, strerror(errno));
        return false;
    }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    rewind(f);
    char *src = (char *)malloc(sz + 1);
    /* Past this point the file EXISTS, so any failure is MALFORMED (or a
     * read error on a real file) -- never "no manifest here". */
    if (!src) {
        fclose(f);
        if (status) *status = PKG_MANIFEST_MALFORMED;
        pkg_manifest_mark_malformed(path);
        return false;
    }
    if (fread(src, 1, sz, f) != (size_t)sz) {
        fclose(f); free(src);
        if (status) *status = PKG_MANIFEST_MALFORMED;
        pkg_manifest_mark_malformed(path);
        return false;
    }
    fclose(f);
    src[sz] = '\0';

    /* Parse with the Turmeric reader */
    Arena arena;
    arena_init(&arena, 0);
    SymbolTable st;
    symtab_init(&st, &arena);

    /* Copy src into the arena so the registered SourceFile.src stays valid
     * for the lifetime of any diagnostics emitted from later parsing. */
    char *src_arena = (char *)arena_alloc(&arena, (size_t)sz + 1);
    memcpy(src_arena, src, (size_t)sz + 1);
    free(src);
    src = src_arena;

    /* Strip a leading "#lang ..." directive (if any) before parsing.  The
     * sweet-exp preprocessor and the s-expr reader both choke on the bare
     * '#' of an unhandled "#lang" line; main.c's single-file paths run the
     * same detect_lang sweep before reading.
     *
     * A trailing token after the base name is diagnosed HERE too, rather than
     * silently tolerated because this caller only wants the strip.  A manifest
     * is source like any other file, and `#lang` taking a base and nothing
     * else has to mean the same thing in build.tur and build.tur.sweet as it
     * does in a .tur -- a token rejected everywhere except the one file that
     * configures the build is the kind of split nobody discovers until it
     * matters. */
    const char *src_eff = src;
    size_t      len_eff = (size_t)sz;
    {
        const char *src_rest = src;
        size_t      len_rest = (size_t)sz;
        const char *bad      = NULL;
        size_t      bad_len  = 0;
        ReaderType  lang_type = detect_lang(src, (size_t)sz, &src_rest, &len_rest,
                                            &bad, &bad_len);
        if (bad && lang_type != READER_UNKNOWN) {
            fprintf(stderr,
                    "spice: error [TUR-E0330]: `#lang` takes a single base "
                    "dialect; unexpected trailing token '%.*s' in %s\n",
                    (int)bad_len, bad, path);
            symtab_free(&st);   /* its buckets are heap, not arena */
            arena_free(&arena);
            if (status) *status = PKG_MANIFEST_MALFORMED;
            pkg_manifest_mark_malformed(path);
            return false;
        }
        src_eff = src_rest;
        len_eff = len_rest;
    }

    SourceFile file = {0};
    file.path    = path;
    file.src     = src_eff;
    file.len     = (uint32_t)len_eff;
    file.file_id = 0;
    file.reader_type = reader_type_from_extension(path);
    diag_register_file(&file);

    uint32_t nforms = 0;
    Form **forms = read_all(&arena, &st, &file, &nforms);

    if (!forms || diag_had_error() || nforms == 0) {
        symtab_free(&st);
        arena_free(&arena);
        return false;
    }

    /* Find the (defpackage ...) form */
    Form *dp = NULL;
    for (uint32_t i = 0; i < nforms; i++) {
        Form *frm = forms[i];
        if (frm->tag != F_LIST || frm->as.list.len < 2) continue;
        Form *head = frm->as.list.items[0];
        if (head->tag == F_SYM &&
            strcmp(head->as.sym->name, "defpackage") == 0) {
            dp = frm;
            break;
        }
    }
    if (!dp) {
        fprintf(stderr, "spice: no (defpackage ...) form found in '%s'\n", path);
        symtab_free(&st);
        arena_free(&arena);
        return false;
    }

    /* items[1] is the package name symbol or string */
    Form *name_form = dp->as.list.items[1];
    if (name_form->tag == F_SYM)
        out->name = tur_strdup(name_form->as.sym->name);
    else if (name_form->tag == F_STR)
        out->name = ss_dup(name_form->as.s);

    /* Walk keyword-value pairs starting at index 2 */
    const char *bad_slot = NULL;   /* first slot whose parser reported failure */
    const FormList *fl = &dp->as.list;
    for (uint32_t i = 2; i + 1 < fl->len; i += 2) {
        const Form *kf = fl->items[i];
        const Form *vf = fl->items[i + 1];
        if (kf->tag != F_KEYWORD) { i -= 1; continue; }
        const char *kw = kf->as.sym->name;

        if (strcmp(kw, "name") == 0) {
            free(out->name);
            out->name = form_str_dup(vf);
        } else if (strcmp(kw, "version") == 0) {
            /* A key repeated in the manifest replaces the earlier value, so
             * each scalar slot frees what it held -- only :name did, and the
             * rest leaked (found by tests/fuzz/fuzz_manifest). */
            free(out->version);
            out->version = form_str_dup(vf);
        } else if (strcmp(kw, "tur-version") == 0) {
            free(out->tur_version);
            out->tur_version = form_str_dup(vf);
            /* Checked here rather than after the loop so the caret lands on the
             * range the user wrote. */
            pkg_check_tur_version_span(out->tur_version, vf->span);
        } else if (strcmp(kw, "description") == 0) {
            free(out->description);
            out->description = form_str_dup(vf);
        } else if (strcmp(kw, "license") == 0) {
            free(out->license);
            out->license = form_str_dup(vf);
        } else if (strcmp(kw, "repository") == 0) {
            free(out->repository);
            out->repository = form_str_dup(vf);
        } else if (strcmp(kw, "homepage") == 0) {
            free(out->homepage);
            out->homepage = form_str_dup(vf);
        } else if (strcmp(kw, "authors") == 0) {
            str_vec_clear(&out->authors, &out->n_authors);
            if (!parse_str_vec(vf, &out->authors, &out->n_authors))
                bad_slot = bad_slot ? bad_slot : ":authors";
        } else if (strcmp(kw, "spices") == 0) {
            /* The slot parsers all return bool and none of these used to be
             * checked, so a broken slot was visible only in the trailing
             * diag_had_error() sweep -- all-or-nothing, with no record of WHICH
             * slot broke.  Keep the first failing slot name for the sticky
             * verdict; parsing still continues so the user sees every slot's
             * diagnostic in one pass rather than one per edit-compile cycle. */
            spices_clear(out);
            if (!parse_spices(vf, out))
                bad_slot = bad_slot ? bad_slot : ":spices";
        } else if (strcmp(kw, "cmake-deps") == 0) {
            cmake_deps_clear(out);
            if (!parse_cmake_deps(vf, out))
                bad_slot = bad_slot ? bad_slot : ":cmake-deps";
        } else if (strcmp(kw, "exports") == 0) {
            str_vec_clear(&out->exports, &out->n_exports);
            if (!parse_exports(vf, &out->exports, &out->n_exports))
                bad_slot = bad_slot ? bad_slot : ":exports";
        } else if (strcmp(kw, "members") == 0) {
            /* LS2: workspace member spice directories, relative to this
             * manifest. A non-empty list makes this manifest a workspace
             * root; the resolver auto-links sibling members. */
            str_vec_clear(&out->members, &out->n_members);
            parse_str_vec(vf, &out->members, &out->n_members);
            /* WP2 (D-5): a member path is resolved against the workspace root
             * and then built, so it has to stay inside the workspace. */
            for (int mi = 0; mi < out->n_members; mi++) {
                if (out->members[mi] && !tur_rel_path_ok(out->members[mi]))
                    diag_emit(DIAG_ERROR, vf->span,
                        "build.tur: :members entry '%s' must be a relative "
                        "path inside the workspace (no '..', no leading '/', "
                        "no shell metacharacters)", out->members[mi]);
            }
        } else if (strcmp(kw, "build-dir") == 0) {
            /* build-output-directory-plan: relative path for build artifacts. */
            free(out->build_dir);
            out->build_dir = form_str_dup(vf);
            /* WP2 (D-5): "relative path" was a comment, not a rule -- an
             * absolute or climbing :build-dir chose where the build WROTE. */
            if (out->build_dir && !tur_rel_path_ok(out->build_dir)) {
                diag_emit(DIAG_ERROR, vf->span,
                    "build.tur: :build-dir '%s' must be a relative path inside "
                    "the project (no '..', no leading '/', no shell "
                    "metacharacters)", out->build_dir);
                free(out->build_dir);
                out->build_dir = NULL;
            }
        } else if (strcmp(kw, "entry") == 0) {
            /* Entry-point module for project-mode `tur run`, relative to the
             * manifest dir. Existence is checked by the caller (main.c), which
             * is the only place that knows the resolved project root. */
            free(out->entry);
            out->entry = form_str_dup(vf);
        } else if (strcmp(kw, "engine") == 0) {
            /* engine-selection-plan E1: default execution engine for
             * `tur run`.  An unknown VALUE is a hard error -- `:engine
             * "jitt"` silently running under cc is exactly the failure mode
             * the plan exists to prevent (unknown KEYS stay silently
             * ignored, which is the documented compatibility story). */
            free(out->engine);
            out->engine = form_str_dup(vf);
            if (out->engine && strcmp(out->engine, "cc") != 0 &&
                strcmp(out->engine, "jit") != 0 &&
                strcmp(out->engine, "interp") != 0) {
                diag_emit_with_code(DIAG_ERROR, vf->span,
                                    TUR_E0311_UNKNOWN_ENGINE,
                                    "build.tur: unknown :engine value '%s' "
                                    "(expected \"cc\", \"jit\", or "
                                    "\"interp\")",
                                    out->engine);
            }
        } else if (strcmp(kw, "experiments") == 0) {
            /* XF1: opt-in experimental features for this spice. */
            str_vec_clear(&out->experiments, &out->n_experiments);
            parse_experiments(vf, &out->experiments, &out->n_experiments);
            /* UC-3: record that the key was present even when the list is
             * empty -- an empty :experiments [] still suppresses the
             * user-level experiments file. */
            out->has_experiments_key = true;
        } else if (strcmp(kw, "reader-macros") == 0) {
            /* RM4: vector of paths to reader-macro definition files. */
            str_vec_clear(&out->reader_macros, &out->n_reader_macros);
            parse_str_vec(vf, &out->reader_macros, &out->n_reader_macros);
        } else if (strcmp(kw, "bin") == 0) {
            /* GS-M1: :bin #{ "tur-foo" "src/main.tur" ... } */
            if (!vf) continue;
            if (!expect_map(vf, ":bin")) continue;
            bins_clear(out);
            const FormList *bfl = &vf->as.list;
            int cap = (int)(bfl->len / 2 + 1);
            out->bin_names = (char **)malloc(cap * sizeof(char *));
            out->bin_paths = (char **)malloc(cap * sizeof(char *));
            if (!out->bin_names || !out->bin_paths) continue;
            for (uint32_t bi = 0; bi + 1 < bfl->len; bi += 2) {
                const Form *bk = bfl->items[bi];
                const Form *bv = bfl->items[bi + 1];
                if (bk->tag != F_STR) continue;
                if (!bv || bv->tag != F_STR) {
                    diag_emit(DIAG_ERROR, bv ? bv->span : bk->span,
                              "build.tur: :bin entry value must be a "
                              "string entrypoint path");
                    continue;
                }
                char *bname = ss_dup(bk->as.s);
                char *bpath = ss_dup(bv->as.s);
                if (!bname || !bpath) { free(bname); free(bpath); continue; }
                if (strncmp(bname, "tur-", 4) != 0 || bname[4] == '\0') {
                    diag_emit(DIAG_ERROR, bk->span,
                              "build.tur: :bin name '%s' must start with "
                              "'tur-' (e.g. tur-nb)", bname);
                    free(bname); free(bpath);
                    continue;
                }
                out->bin_names[out->n_bins] = bname;
                out->bin_paths[out->n_bins] = bpath;
                out->n_bins++;
            }
        } else if (strcmp(kw, "web") == 0) {
            if (vf && expect_map(vf, ":web")) {
                out->web.present = true;
                const Form *gf = map_get_kw(vf, "gl");
                if (gf && gf->tag == F_KEYWORD && gf->as.sym &&
                    gf->as.sym->name) {
                    const char *g = gf->as.sym->name;
                    if (strcmp(g, "es2") == 0)      out->web.gl = PKG_WEB_GL_ES2;
                    else if (strcmp(g, "es3") == 0) out->web.gl = PKG_WEB_GL_ES3;
                    else
                        diag_emit(DIAG_ERROR, gf->span,
                                  "build.tur: :web :gl must be :es2 or :es3, "
                                  "got :%s", g);
                }
                out->web.canvas  = form_bool_val(map_get_kw(vf, "canvas"));
                out->web.audio   = form_bool_val(map_get_kw(vf, "audio"));
                out->web.threads = form_bool_val(map_get_kw(vf, "threads"));
                const Form *hf = map_get_kw(vf, "heap");
                if (hf) {
                    if (hf->tag == F_INT && hf->as.i >= 0)
                        out->web.heap = (long)hf->as.i;
                    else
                        diag_emit(DIAG_ERROR, hf->span,
                                  "build.tur: :web :heap must be a "
                                  "non-negative byte count (0 selects "
                                  "-sALLOW_MEMORY_GROWTH)");
                }
                const Form *mf = map_get_kw(vf, "main-loop");
                if (mf && mf->tag == F_KEYWORD && mf->as.sym &&
                    mf->as.sym->name) {
                    const char *ml = mf->as.sym->name;
                    if (strcmp(ml, "callback") == 0 ||
                        strcmp(ml, "asyncify") == 0 ||
                        strcmp(ml, "none") == 0) {
                        free(out->web.main_loop);
                        out->web.main_loop = tur_strdup(ml);
                    } else {
                        diag_emit(DIAG_ERROR, mf->span,
                                  "build.tur: :web :main-loop must be "
                                  ":callback, :asyncify or :none, got :%s", ml);
                    }
                }
            }
        } else if (strcmp(kw, "build-opts") == 0) {
            if (vf && expect_map(vf, ":build-opts")) {
                const Form *cf = map_get_kw(vf, "c-flags");
                const Form *lf = map_get_kw(vf, "link-libs");
                const Form *nf = map_get_kw(vf, "no-stdlib");
                const Form *sf = map_get_kw(vf, "c-sources");
                const Form *if_ = map_get_kw(vf, "c-includes");
                str_vec_clear(&out->c_flags, &out->n_c_flags);
                str_vec_clear(&out->link_libs, &out->n_link_libs);
                str_vec_clear(&out->link_flags, &out->n_link_flags);
                str_vec_clear(&out->c_sources, &out->n_c_sources);
                str_vec_clear(&out->c_includes, &out->n_c_includes);
                parse_str_vec(cf, &out->c_flags,   &out->n_c_flags);
                parse_str_vec(lf, &out->link_libs,  &out->n_link_libs);
                parse_str_vec(map_get_kw(vf, "link-flags"),
                              &out->link_flags, &out->n_link_flags);
                check_link_vec(out->link_libs, out->n_link_libs,
                               ":link-libs", true, vf->span);
                check_link_vec(out->link_flags, out->n_link_flags,
                               ":link-flags", false, vf->span);
                check_link_vec(out->c_flags, out->n_c_flags,
                               ":c-flags", false, vf->span);
                out->no_stdlib = form_bool_val(nf);
                /* spices-c-sources-plan: validate vendored sources/includes
                 * against the manifest directory (the dir holding build.tur). */
                char mdir[4096];
                snprintf(mdir, sizeof(mdir), "%s", path);
                char *slash = strrchr(mdir, '/');
                if (slash) *slash = '\0'; else snprintf(mdir, sizeof(mdir), ".");
                parse_c_path_vec(sf,  mdir, ":c-sources",  true,
                                 &out->c_sources,  &out->n_c_sources);
                parse_c_path_vec(if_, mdir, ":c-includes", false,
                                 &out->c_includes, &out->n_c_includes);
            }
        }
    }

    bool ok = !diag_had_error();
    symtab_free(&st);
    arena_free(&arena);

    if (!ok) {
        /* A manifest that EXISTS and failed to parse.  Two things follow.
         *
         * First, never hand back a partially-parsed manifest: the slot parsers
         * run to completion past a broken slot, so `out` is populated with
         * whatever DID parse.  Every caller takes the `if (!read) continue;`
         * branch and drops it on the floor, which leaked (ASan caught
         * parse_exports/ss_dup allocations escaping this way).  Free and zero
         * so the failure branch owns nothing. */
        pkg_manifest_free(out);
        memset(out, 0, sizeof(*out));
        /* Second, remember it.  diag_reset() at the next compile entry point
         * would otherwise erase the only evidence, leaving an `error:` on
         * stderr and a 0 exit status. */
        pkg_manifest_mark_malformed_slot(path, bad_slot);
        if (status) *status = PKG_MANIFEST_MALFORMED;
    } else if (status) {
        *status = PKG_MANIFEST_OK;
    }
    return ok;
}

/* ================================================================== */
/* :tur-version enforcement                                             */
/* ================================================================== */

/* The compiler's own version, injected by CMake on tur_core.  Guarded because
 * pkg.c had no reason to know it before this check existed. */
#ifndef TUR_VERSION
#define TUR_VERSION "unknown"
#endif

/* Sticky record of a REJECTING :tur-version verdict, surviving diag_reset().
 *
 * The manifest is read once, before compilation; every compile entry point then
 * calls diag_reset() to clear `had_error_` so a batch driver
 * (`tur check <dir>`) does not mark later files failed because an earlier one
 * was.  That reset is correct, and it also wiped this check's error -- so a
 * floor violation printed an "error" and then exited 0, which is not an error
 * at all.  (The pre-existing TUR-E0620 manifest error has the same shape.)
 *
 * So the verdict is recorded here and re-asserted after each diag_reset(), next
 * to experiment_reset_warnings() which handles the same once-per-compile
 * problem from the other direction. */
static bool g_tv_rejected = false;
static char g_tv_reject_brief[320];
/* Report the verdict at most once per process: the manifest is read more than
 * once per invocation (walk-up for build-dir, again for reader macros), and
 * repeating the same verdict per read is pure noise. */
static bool g_tv_reported = false;

void pkg_tur_version_reassert(void) {
    if (!g_tv_rejected) return;
    /* Re-emitted per compile, deliberately: had_error_ must be set for THIS
     * compile to fail, and there is no diag API to mark an error without
     * printing.  The brief form keeps the repeat cheap to read. */
    diag_emit(DIAG_ERROR, SPAN_UNKNOWN, "%s", g_tv_reject_brief);
}

void pkg_tur_version_reset(void) {
    g_tv_rejected = false;
    g_tv_reported = false;
    g_tv_reject_brief[0] = '\0';
}

/* Check `range` (the :tur-version value) against the running compiler.  `span`
 * is the value form's own span, so the caret lands on the range the user wrote
 * rather than on the top of the file.
 *
 */
static void pkg_check_tur_version_span(const char *range, Span span) {
    if (!range || !*range) return;

    if (!pkg_version_range_valid(range)) {
        if (!g_tv_reported)
            diag_emit(DIAG_ERROR, span,
                      "TUR-E0622: :tur-version \"%s\" is not a valid version "
                      "range.  Expected comma-separated comparators or a "
                      "caret, e.g. \">=0.32.2\", \">=0.32.2, <0.35.0\", or "
                      "\"^0.32\".  (`~`, `*` and `||` are not supported.)",
                      range);
        g_tv_reported = true;
        g_tv_rejected = true;
        snprintf(g_tv_reject_brief, sizeof(g_tv_reject_brief),
                 "TUR-E0622: refusing to compile: build.tur declares "
                 ":tur-version \"%s\", which is not a valid version range",
                 range);
        return;
    }

    /* A build that cannot report its own version cannot honour a range, and
     * failing closed would break every non-release build.  Stay silent. */
    if (strcmp(TUR_VERSION, "unknown") == 0) return;

    bool below_floor = false;
    if (pkg_version_range_match(range, TUR_VERSION, &below_floor)) return;

    if (below_floor) {
        /* The spice needs syntax / experiments / manifest keys this compiler
         * does not have.  Stop rather than let the real failure surface later as
         * an error about perfectly valid source -- which is the whole reason
         * this key exists. */
        if (!g_tv_reported)
            diag_emit(DIAG_ERROR, span,
                      "TUR-E0621: this spice requires tur %s, but this is tur "
                      "%s.  Upgrade the compiler, or use a spice revision that "
                      "supports %s.",
                      range, TUR_VERSION, TUR_VERSION);
        g_tv_reported = true;
        g_tv_rejected = true;
        snprintf(g_tv_reject_brief, sizeof(g_tv_reject_brief),
                 "TUR-E0621: refusing to compile: build.tur requires tur %s, "
                 "this is tur %s", range, TUR_VERSION);
    } else {
        /* Above a declared ceiling: the author never tested this combination,
         * which is usually still fine.  A hard error would mean every compiler
         * release breaks every spice until each author bumps a number, so this
         * stays advisory. */
        if (!g_tv_reported)
            diag_emit(DIAG_WARNING, span,
                      "TUR-W0623: this spice declares tur %s, but this is tur "
                      "%s -- newer than the author tested against.  "
                      "Continuing; if something breaks, report it upstream "
                      "rather than assuming it is your build.",
                      range, TUR_VERSION);
        g_tv_reported = true;
    }
}

/* ================================================================== */
/* pkg_manifest_write                                                   */
/* ================================================================== */

bool pkg_manifest_write(const char *path, const PkgManifest *m) {
    FILE *f = fopen(path, "w");
    if (!f) {
        fprintf(stderr, "spice: cannot write '%s': %s\n", path, strerror(errno));
        return false;
    }

    fprintf(f, ";;; build.tur -- project manifest for \"%s\"\n",
            m->name ? m->name : "unnamed");
    fprintf(f, "(defpackage %s\n", m->name ? m->name : "unnamed");
    if (m->name)        fprintf(f, "  :name        \"%s\"\n", m->name);
    if (m->version)     fprintf(f, "  :version     \"%s\"\n", m->version);
    if (m->tur_version) fprintf(f, "  :tur-version \"%s\"\n", m->tur_version);
    if (m->description) fprintf(f, "  :description \"%s\"\n", m->description);
    if (m->license)     fprintf(f, "  :license     \"%s\"\n", m->license);
    if (m->repository)  fprintf(f, "  :repository  \"%s\"\n", m->repository);
    if (m->homepage)    fprintf(f, "  :homepage    \"%s\"\n", m->homepage);

    if (m->n_authors > 0) {
        fprintf(f, "  :authors     [");
        for (int i = 0; i < m->n_authors; i++) {
            if (i) fprintf(f, " ");
            fprintf(f, "\"%s\"", m->authors[i]);
        }
        fprintf(f, "]\n");
    }

    if (m->n_spices > 0) {
        fprintf(f, "\n  :spices #{\n");
        for (int i = 0; i < m->n_spices; i++) {
            const PkgSpice *s = &m->spices[i];
            if (s->path) {
                fprintf(f, "    \"%s\" #{:path \"%s\"", s->name, s->path);
            } else {
                fprintf(f, "    \"%s\" #{:url \"%s\"", s->name,
                        s->url ? s->url : "");
                if (s->ref) fprintf(f, " :ref \"%s\"", s->ref);
            }
            if (s->optional) fprintf(f, " :optional true");
            fprintf(f, "}\n");
        }
        fprintf(f, "  }\n");
    }

    if (m->n_cmake_deps > 0) {
        fprintf(f, "\n  :cmake-deps #{\n");
        for (int i = 0; i < m->n_cmake_deps; i++) {
            const PkgCmakeDep *d = &m->cmake_deps[i];
            if (d->path) {
                fprintf(f, "    \"%s\" #{:path \"%s\"", d->name, d->path);
            } else {
                fprintf(f, "    \"%s\" #{:url \"%s\"",
                        d->name, d->url ? d->url : "");
                if (d->ref) fprintf(f, " :ref \"%s\"", d->ref);
            }
            if (d->cmake_name) fprintf(f, " :cmake-name \"%s\"", d->cmake_name);
            if (d->prefer_system) fprintf(f, " :prefer-system true");
            if (d->cmake_version)
                fprintf(f, " :cmake-version \"%s\"", d->cmake_version);
            if (d->n_targets > 0) {
                fprintf(f, " :targets [");
                for (int j = 0; j < d->n_targets; j++) {
                    if (j) fprintf(f, " ");
                    fprintf(f, "\"%s\"", d->targets[j]);
                }
                fprintf(f, "]");
            }
            /* has_link_libs, not the count: `:link-libs []` is a value that
             * has to survive the round trip. */
            if (d->has_link_libs) {
                fprintf(f, " :link-libs [");
                for (int j = 0; j < d->n_link_libs; j++) {
                    if (j) fprintf(f, " ");
                    fprintf(f, "\"%s\"", d->link_libs[j]);
                }
                fprintf(f, "]");
            }
            if (d->n_link_flags > 0) {
                fprintf(f, " :link-flags [");
                for (int j = 0; j < d->n_link_flags; j++) {
                    if (j) fprintf(f, " ");
                    fprintf(f, "\"%s\"", d->link_flags[j]);
                }
                fprintf(f, "]");
            }
            if (d->n_opts > 0) {
                fprintf(f, " :options #{");
                for (int j = 0; j < d->n_opts; j++) {
                    if (j) fprintf(f, " ");
                    fprintf(f, ":%s \"%s\"",
                            d->opts[j].key, d->opts[j].val);
                }
                fprintf(f, "}");
            }
            if (d->n_wasm_opts > 0) {
                fprintf(f, " :wasm-options #{");
                for (int j = 0; j < d->n_wasm_opts; j++) {
                    if (j) fprintf(f, " ");
                    fprintf(f, ":%s \"%s\"",
                            d->wasm_opts[j].key, d->wasm_opts[j].val);
                }
                fprintf(f, "}");
            }
            fprintf(f, "}\n");
        }
        fprintf(f, "  }\n");
    }

    /* `:web` round-trips only when the manifest actually carried it: the
     * defaults are not all zero, so writing them unconditionally would put a
     * `:web` block into every manifest `tur add` touches. */
    if (m->web.present) {
        fprintf(f, "\n  :web #{\n");
        fprintf(f, "    :gl :%s\n",
                m->web.gl == PKG_WEB_GL_ES2 ? "es2" : "es3");
        if (m->web.canvas)  fprintf(f, "    :canvas true\n");
        if (m->web.audio)   fprintf(f, "    :audio true\n");
        if (m->web.threads) fprintf(f, "    :threads true\n");
        fprintf(f, "    :heap %ld\n", m->web.heap);
        if (m->web.main_loop)
            fprintf(f, "    :main-loop :%s\n", m->web.main_loop);
        fprintf(f, "  }\n");
    }

    if (m->n_c_flags > 0 || m->n_link_libs > 0 || m->n_link_flags > 0 ||
        m->no_stdlib || m->n_c_sources > 0 || m->n_c_includes > 0) {
        fprintf(f, "\n  :build-opts #{\n");
        if (m->n_c_flags > 0) {
            fprintf(f, "    :c-flags [");
            for (int i = 0; i < m->n_c_flags; i++) {
                if (i) fprintf(f, " ");
                fprintf(f, "\"%s\"", m->c_flags[i]);
            }
            fprintf(f, "]\n");
        }
        if (m->n_c_includes > 0) {
            fprintf(f, "    :c-includes [");
            for (int i = 0; i < m->n_c_includes; i++) {
                if (i) fprintf(f, " ");
                fprintf(f, "\"%s\"", m->c_includes[i]);
            }
            fprintf(f, "]\n");
        }
        if (m->n_c_sources > 0) {
            fprintf(f, "    :c-sources [");
            for (int i = 0; i < m->n_c_sources; i++) {
                if (i) fprintf(f, " ");
                fprintf(f, "\"%s\"", m->c_sources[i]);
            }
            fprintf(f, "]\n");
        }
        if (m->n_link_libs > 0) {
            fprintf(f, "    :link-libs [");
            for (int i = 0; i < m->n_link_libs; i++) {
                if (i) fprintf(f, " ");
                fprintf(f, "\"%s\"", m->link_libs[i]);
            }
            fprintf(f, "]\n");
        }
        if (m->n_link_flags > 0) {
            fprintf(f, "    :link-flags [");
            for (int i = 0; i < m->n_link_flags; i++) {
                if (i) fprintf(f, " ");
                fprintf(f, "\"%s\"", m->link_flags[i]);
            }
            fprintf(f, "]\n");
        }
        if (m->no_stdlib) fprintf(f, "    :no-stdlib true\n");
        fprintf(f, "  }\n");
    }

    if (m->n_exports > 0) {
        fprintf(f, "\n  :exports [");
        for (int i = 0; i < m->n_exports; i++) {
            if (i) fprintf(f, " ");
            fprintf(f, "\"%s\"", m->exports[i]);
        }
        fprintf(f, "]\n");
    }

    if (m->build_dir)
        fprintf(f, "  :build-dir   \"%s\"\n", m->build_dir);
    if (m->entry && *m->entry)
        fprintf(f, "  :entry       \"%s\"\n", m->entry);
    if (m->engine && *m->engine)
        fprintf(f, "  :engine      \"%s\"\n", m->engine);

    if (m->n_reader_macros > 0) {
        fprintf(f, "\n  :reader-macros [");
        for (int i = 0; i < m->n_reader_macros; i++) {
            if (i) fprintf(f, " ");
            fprintf(f, "\"%s\"", m->reader_macros[i]);
        }
        fprintf(f, "]\n");
    }

    if (m->n_experiments > 0) {
        /* XF1: write experiment names as keywords, the canonical manifest form. */
        fprintf(f, "\n  :experiments [");
        for (int i = 0; i < m->n_experiments; i++) {
            if (i) fprintf(f, " ");
            fprintf(f, ":%s", m->experiments[i]);
        }
        fprintf(f, "]\n");
    }

    if (m->n_bins > 0) {
        fprintf(f, "\n  :bin #{\n");
        for (int i = 0; i < m->n_bins; i++) {
            fprintf(f, "    \"%s\" \"%s\"\n",
                    m->bin_names[i], m->bin_paths[i]);
        }
        fprintf(f, "  }\n");
    }

    fprintf(f, ")\n");
    fclose(f);
    return true;
}

/* ================================================================== */
/* pkg_manifest_free                                                    */
/* ================================================================== */

void pkg_manifest_free(PkgManifest *m) {
    free(m->name);
    free(m->version);
    free(m->tur_version);
    free(m->description);
    free(m->license);
    free(m->repository);
    free(m->homepage);
    str_vec_clear(&m->authors, &m->n_authors);
    spices_clear(m);
    cmake_deps_clear(m);
    str_vec_clear(&m->exports, &m->n_exports);
    str_vec_clear(&m->c_flags, &m->n_c_flags);
    str_vec_clear(&m->link_flags, &m->n_link_flags);
    str_vec_clear(&m->link_libs, &m->n_link_libs);
    str_vec_clear(&m->c_sources, &m->n_c_sources);
    str_vec_clear(&m->c_includes, &m->n_c_includes);
    str_vec_clear(&m->reader_macros, &m->n_reader_macros);
    bins_clear(m);
    str_vec_clear(&m->members, &m->n_members);
    str_vec_clear(&m->experiments, &m->n_experiments);
    free(m->build_dir);
    free(m->entry);
    free(m->engine);
    memset(m, 0, sizeof(*m));
}

/* ================================================================== */
/* Lock file (Turmeric S-expression format)                            */
/* ================================================================== */

bool pkg_lock_read(const char *path, PkgLockFile *out) {
    memset(out, 0, sizeof(*out));
    out->format_version = 1;

    /* "rb", not "r": on Windows a text-mode handle strips CR on the way in, so
     * fread returns FEWER bytes than the ftell size above and the `!= sz` check
     * below rejects the file -- silently, because the caller reads the result as
     * "there is no lock file yet". Every tur.lock tur itself writes has CRLF
     * line endings, so on Windows the lockfile was never read AT ALL: the
     * integrity check never ran, and the pinned :resolved refs never applied.
     * The Turmeric reader treats CR as whitespace, so the bytes are fine.
     * docs/archive/windows-text-mode-read-rejects-own-files.md */
    FILE *f = fopen(path, "rb");
    if (!f) return false; /* not an error -- no lock file yet */

    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    rewind(f);
    char *src = (char *)malloc(sz + 1);
    if (!src) { fclose(f); return false; }
    if (fread(src, 1, sz, f) != (size_t)sz) { fclose(f); free(src); return false; }
    fclose(f);
    src[sz] = '\0';

    Arena arena;
    arena_init(&arena, 0);
    SymbolTable st;
    symtab_init(&st, &arena);

    SourceFile file = {0};
    file.path    = path;
    file.src     = src;
    file.len     = (uint32_t)sz;
    file.file_id = 0;
    diag_register_file(&file);

    uint32_t nforms = 0;
    Form **forms = read_all(&arena, &st, &file, &nforms);
    free(src);

    if (!forms || diag_had_error() || nforms == 0) {
        symtab_free(&st);
        arena_free(&arena);
        return false;
    }

    /* Find (deflockfile ...) */
    Form *dlf = NULL;
    for (uint32_t i = 0; i < nforms; i++) {
        Form *frm = forms[i];
        if (frm->tag != F_LIST || frm->as.list.len < 1) continue;
        Form *head = frm->as.list.items[0];
        if (head->tag == F_SYM &&
            strcmp(head->as.sym->name, "deflockfile") == 0) {
            dlf = frm;
            break;
        }
    }
    if (!dlf) {
        /* Empty or unrecognised format -- return empty lock */
        symtab_free(&st);
        arena_free(&arena);
        return true;
    }

    /* Walk keyword-value pairs starting at index 1 */
    const FormList *fl = &dlf->as.list;
    for (uint32_t i = 1; i + 1 < fl->len; i += 2) {
        const Form *kf = fl->items[i];
        const Form *vf = fl->items[i + 1];
        if (kf->tag != F_KEYWORD) { i -= 1; continue; }
        const char *kw = kf->as.sym->name;

        if (strcmp(kw, "format-version") == 0) {
            if (vf->tag == F_INT) out->format_version = (int)vf->as.i;
        } else if (strcmp(kw, "spices") == 0 ||
                   strcmp(kw, "cmake-deps") == 0) {
            bool is_cmake = (strcmp(kw, "cmake-deps") == 0);
            if (!vf || vf->tag != F_MAP) continue;
            const FormList *mfl = &vf->as.list;
            for (uint32_t j = 0; j + 1 < mfl->len; j += 2) {
                const Form *nf = mfl->items[j];
                const Form *ef = mfl->items[j + 1];
                if (nf->tag != F_STR) continue;
                if (!ef || ef->tag != F_MAP) continue;

                out->entries = (PkgLockEntry *)realloc(out->entries,
                    (out->n_entries + 1) * sizeof(PkgLockEntry));
                if (!out->entries) {
                    symtab_free(&st); arena_free(&arena); return false;
                }
                PkgLockEntry *e = &out->entries[out->n_entries++];
                memset(e, 0, sizeof(*e));
                e->name       = ss_dup(nf->as.s);
                e->is_cmake   = is_cmake;
                e->url        = form_str_dup(map_get_kw(ef, "url"));
                e->ref        = form_str_dup(map_get_kw(ef, "ref"));
                e->resolved   = form_str_dup(map_get_kw(ef, "resolved"));
                e->sha256     = form_str_dup(map_get_kw(ef, "sha256"));
                e->fetched_at = form_str_dup(map_get_kw(ef, "fetched-at"));
                e->resolved_via   = form_str_dup(map_get_kw(ef, "resolved-via"));
                e->system_version = form_str_dup(map_get_kw(ef, "system-version"));

                /* transitive: ["name@ref" ...] */
                const Form *tr = map_get_kw(ef, "transitive");
                if (tr && (tr->tag == F_VEC || tr->tag == F_LIST)) {
                    const FormList *tfl = &tr->as.list;
                    if (tfl->len > 0) {
                        e->transitive = (char **)malloc(
                            tfl->len * sizeof(char *));
                        for (uint32_t k = 0; k < tfl->len; k++) {
                            if (tfl->items[k]->tag == F_STR)
                                e->transitive[e->n_transitive++] =
                                    ss_dup(tfl->items[k]->as.s);
                        }
                    }
                }
            }
        }
    }

    symtab_free(&st);
    arena_free(&arena);
    return true;
}

bool pkg_lock_write(const char *path, const PkgLockFile *lock) {
    /* Write to a temp file then rename for atomicity */
    char tmp_path[4096];
    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp.%d", path, (int)getpid());
    FILE *f = fopen(tmp_path, "w");
    if (!f) {
        fprintf(stderr, "spice: cannot write lock file '%s': %s\n",
                path, strerror(errno));
        return false;
    }

    fprintf(f, ";;; tur.lock -- generated by tur. Do not edit by hand.\n");
    fprintf(f, ";;; Commit this file to version control for reproducible builds.\n\n");
    fprintf(f, "(deflockfile\n");
    fprintf(f, "  :format-version %d\n", lock->format_version);

    /* Spices section -- use #{} map syntax (standard Turmeric, no #lang needed) */
    fprintf(f, "  :spices #{\n");
    for (int i = 0; i < lock->n_entries; i++) {
        const PkgLockEntry *e = &lock->entries[i];
        if (e->is_cmake) continue;
        fprintf(f, "    \"%s\" #{", e->name);
        if (e->url)        fprintf(f, ":url \"%s\" ", e->url);
        if (e->ref)        fprintf(f, ":ref \"%s\" ", e->ref);
        if (e->resolved)   fprintf(f, ":resolved \"%s\" ", e->resolved);
        if (e->sha256)     fprintf(f, ":sha256 \"%s\" ", e->sha256);
        if (e->fetched_at) fprintf(f, ":fetched-at \"%s\" ", e->fetched_at);
        if (e->n_transitive > 0) {
            fprintf(f, ":transitive [");
            for (int j = 0; j < e->n_transitive; j++) {
                if (j) fprintf(f, " ");
                fprintf(f, "\"%s\"", e->transitive[j]);
            }
            fprintf(f, "] ");
        }
        fprintf(f, "}\n");
    }
    fprintf(f, "  }\n");

    /* cmake-deps section */
    fprintf(f, "  :cmake-deps #{\n");
    for (int i = 0; i < lock->n_entries; i++) {
        const PkgLockEntry *e = &lock->entries[i];
        if (!e->is_cmake) continue;
        fprintf(f, "    \"%s\" #{", e->name);
        if (e->resolved_via && strcmp(e->resolved_via, "system") == 0) {
            /* System-resolved: record only the resolution provenance. There
             * is no git SHA to pin -- the system package manager owns it. */
            fprintf(f, ":resolved-via \"system\" ");
            if (e->system_version)
                fprintf(f, ":system-version \"%s\" ", e->system_version);
        } else {
            if (e->url)        fprintf(f, ":url \"%s\" ", e->url);
            if (e->ref)        fprintf(f, ":ref \"%s\" ", e->ref);
            if (e->resolved)   fprintf(f, ":resolved \"%s\" ", e->resolved);
            if (e->sha256)     fprintf(f, ":sha256 \"%s\" ", e->sha256);
            if (e->fetched_at) fprintf(f, ":fetched-at \"%s\" ", e->fetched_at);
            if (e->resolved_via)
                fprintf(f, ":resolved-via \"%s\" ", e->resolved_via);
        }
        fprintf(f, "}\n");
    }
    fprintf(f, "  })\n");

    fclose(f);
    if (rename(tmp_path, path) != 0) {
        fprintf(stderr, "spice: rename failed: %s\n", strerror(errno));
        unlink(tmp_path);
        return false;
    }
    return true;
}

void pkg_lock_free(PkgLockFile *lock) {
    for (int i = 0; i < lock->n_entries; i++) {
        PkgLockEntry *e = &lock->entries[i];
        free(e->name);
        free(e->url);
        free(e->ref);
        free(e->resolved);
        free(e->sha256);
        free(e->fetched_at);
        free(e->resolved_via);
        free(e->system_version);
        for (int j = 0; j < e->n_transitive; j++) free(e->transitive[j]);
        free(e->transitive);
    }
    free(lock->entries);
    memset(lock, 0, sizeof(*lock));
}

PkgLockEntry *pkg_lock_find(PkgLockFile *lock,
                             const char *name, bool is_cmake) {
    for (int i = 0; i < lock->n_entries; i++) {
        PkgLockEntry *e = &lock->entries[i];
        if (e->is_cmake == is_cmake && strcmp(e->name, name) == 0)
            return e;
    }
    return NULL;
}

/* Add (or update) an entry in the lock file. */
static PkgLockEntry *lock_upsert(PkgLockFile *lock,
                                  const char *name, bool is_cmake) {
    PkgLockEntry *e = pkg_lock_find(lock, name, is_cmake);
    if (e) return e;
    /* append */
    lock->entries = (PkgLockEntry *)realloc(lock->entries,
        (lock->n_entries + 1) * sizeof(PkgLockEntry));
    if (!lock->entries) return NULL;
    e = &lock->entries[lock->n_entries++];
    memset(e, 0, sizeof(*e));
    e->name     = tur_strdup(name);
    e->is_cmake = is_cmake;
    return e;
}

/* LS3: drop any lock entry matching (name, is_cmake).  Returns true if
 * an entry was removed.  Used by `tur fetch` when a previously locked
 * dep has been switched to a local-source declaration (`:path` or a
 * workspace member) so the lockfile records only URL deps. */
static bool lock_remove(PkgLockFile *lock, const char *name, bool is_cmake) {
    for (int i = 0; i < lock->n_entries; i++) {
        PkgLockEntry *e = &lock->entries[i];
        if (e->is_cmake != is_cmake || strcmp(e->name, name) != 0) continue;
        free(e->name);
        free(e->url);
        free(e->ref);
        free(e->resolved);
        free(e->sha256);
        free(e->fetched_at);
        free(e->resolved_via);
        free(e->system_version);
        for (int j = 0; j < e->n_transitive; j++) free(e->transitive[j]);
        free(e->transitive);
        for (int j = i; j < lock->n_entries - 1; j++)
            lock->entries[j] = lock->entries[j + 1];
        lock->n_entries--;
        return true;
    }
    return false;
}

/* ================================================================== */
/* SHA-256                                                              */
/* ================================================================== */

bool pkg_sha256_file(const char *path, char out[65]) {
    uint8_t digest[TUR_SHA256_DIGEST_LEN];
    if (!tur_sha256_file(path, digest)) return false;
    tur_sha256_hex(digest, out);
    return true;
}

/* --- pkg_hash_dir: a content hash of a directory tree ------------------- *
 *
 * This used to be `tar -c <dir> | sha256sum`, which had four problems and
 * only the last one was visible:
 *
 *   1. It hashed `.git`.  A shallow clone's `.git/index` records mtimes, so
 *      running any git command inside a fetched spice changed its hash and
 *      `tur run` then reported "integrity check failed" for a tree nobody had
 *      touched.
 *   2. `tar -c /abs/path` puts the ABSOLUTE PATH in the member headers, so the
 *      digest depended on where the project happened to live on disk.
 *   3. tar output is not reproducible across implementations -- bsdtar and GNU
 *      tar disagree on default format, and both record uid/gid/mtime -- so a
 *      lockfile written on macOS could not verify on Linux.
 *   4. No Windows host can run it.  MinGW ships neither `shasum` nor
 *      `sha256sum`, and popen there runs cmd.exe, where the '%s' quoting is
 *      not quoting at all.  The failure was silent: pkg_fetch_all stored the
 *      git SHA instead and `tur run`'s check quietly never ran.
 *
 * So: hash the CONTENT, in process.  For each regular file, in ascending byte
 * order of its relative path:
 *
 *     "F\0" <relpath> "\0" <decimal size> "\0" <bytes>
 *
 * Separators are normalized to '/', the length prefix keeps a path and the
 * bytes that follow it unambiguous, and the sort makes readdir order
 * irrelevant.  File modes are deliberately absent: Windows cannot represent
 * the executable bit, and a hash that disagreed across platforms would be
 * worse than one that ignores it.
 *
 * Reproducible for a given checkout.  Not guaranteed byte-identical across
 * platforms for a tree containing symlinks or CRLF-translated files -- git
 * itself materializes those differently -- which is why a mismatch is only
 * ever reported for a hash this same algorithm produced (PKG_TREE_HASH_TAG).
 * See docs/archive/pkg-hash-shells-out-to-sha256sum.md. */

/* Directory names never descended into.  `.git` because it is VCS metadata
 * rather than content (see 1 above); the rest because they are tur's own build
 * outputs, which appear inside a spice only after someone builds in it and are
 * never part of what was fetched. */
static bool hash_skip_dir(const char *name) {
    return strcmp(name, ".git") == 0
        || strcmp(name, "build") == 0
        || strcmp(name, ".tur-abi-cache") == 0
        || strcmp(name, ".tur-repl-cache") == 0;
}

/* Bounds a symlinked cycle; a source tree this deep is already pathological. */
#define PKG_HASH_MAX_DEPTH 64

typedef struct {
    char **paths;      /* relative, '/'-separated */
    int    n, cap;
    bool   failed;
} HashWalk;

static void hash_walk_push(HashWalk *w, const char *rel) {
    if (w->n == w->cap) {
        int cap = w->cap ? w->cap * 2 : 64;
        char **p = (char **)realloc(w->paths, (size_t)cap * sizeof(char *));
        if (!p) { w->failed = true; return; }
        w->paths = p;
        w->cap = cap;
    }
    w->paths[w->n] = tur_strdup(rel);
    if (!w->paths[w->n]) { w->failed = true; return; }
    w->n++;
}

static void hash_walk(HashWalk *w, const char *root, const char *rel, int depth) {
    if (w->failed || depth > PKG_HASH_MAX_DEPTH) return;
    char abs[4096];
    if (*rel) snprintf(abs, sizeof(abs), "%s/%s", root, rel);
    else      snprintf(abs, sizeof(abs), "%s", root);

    DIR *d = opendir(abs);
    if (!d) { w->failed = true; return; }
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) continue;
        char child[4096];
        if (*rel) snprintf(child, sizeof(child), "%s/%s", rel, ent->d_name);
        else      snprintf(child, sizeof(child), "%s", ent->d_name);
        char child_abs[4096];
        snprintf(child_abs, sizeof(child_abs), "%s/%s", root, child);
        struct stat st;
        if (stat(child_abs, &st) != 0) continue;   /* dangling link: not content */
        if (S_ISDIR(st.st_mode)) {
            if (hash_skip_dir(ent->d_name)) continue;
            hash_walk(w, root, child, depth + 1);
        } else if (S_ISREG(st.st_mode)) {
            hash_walk_push(w, child);
        }
        if (w->failed) break;
    }
    closedir(d);
}

static int hash_path_cmp(const void *a, const void *b) {
    return strcmp(*(const char *const *)a, *(const char *const *)b);
}

bool pkg_hash_dir(const char *dir, char out[PKG_HASH_MAX]) {
    HashWalk w;
    memset(&w, 0, sizeof w);
    hash_walk(&w, dir, "", 0);

    bool ok = !w.failed;
    if (ok) {
        qsort(w.paths, (size_t)w.n, sizeof(char *), hash_path_cmp);

        TurSha256 ctx;
        tur_sha256_init(&ctx);
        for (int i = 0; i < w.n && ok; i++) {
            char abs[4096];
            snprintf(abs, sizeof(abs), "%s/%s", dir, w.paths[i]);
            FILE *f = fopen(abs, "rb");
            if (!f) { ok = false; break; }
            if (fseek(f, 0, SEEK_END) != 0) { fclose(f); ok = false; break; }
            long sz = ftell(f);
            if (sz < 0) { fclose(f); ok = false; break; }
            rewind(f);

            char hdr[64];
            int hn = snprintf(hdr, sizeof(hdr), "%ld", sz);
            tur_sha256_update(&ctx, "F", 2);              /* 'F' and its NUL */
            tur_sha256_update(&ctx, w.paths[i], strlen(w.paths[i]) + 1);
            tur_sha256_update(&ctx, hdr, (size_t)hn + 1);

            uint8_t buf[64 * 1024];
            for (;;) {
                size_t got = fread(buf, 1, sizeof buf, f);
                if (got > 0) tur_sha256_update(&ctx, buf, got);
                if (got < sizeof buf) break;
            }
            if (ferror(f)) ok = false;
            fclose(f);
        }
        if (ok) {
            uint8_t digest[TUR_SHA256_DIGEST_LEN];
            tur_sha256_final(&ctx, digest);
            memcpy(out, PKG_TREE_HASH_TAG, sizeof(PKG_TREE_HASH_TAG) - 1);
            tur_sha256_hex(digest, out + sizeof(PKG_TREE_HASH_TAG) - 1);
        }
    }

    for (int i = 0; i < w.n; i++) free(w.paths[i]);
    free(w.paths);
    return ok;
}

bool pkg_hash_comparable(const char *recorded) {
    return recorded != NULL
        && strncmp(recorded, PKG_TREE_HASH_TAG, sizeof(PKG_TREE_HASH_TAG) - 1) == 0;
}

/* ================================================================== */
/* Semver                                                               */
/* ================================================================== */

bool pkg_semver_parse(const char *v,
                      int *major, int *minor, int *patch,
                      char **pre) {
    if (!v) return false;
    if (*v == 'v') v++;
    if (*v < '0' || *v > '9') return false;   /* strtol would accept " +1", "-3" */
    /* parse major.minor.patch */
    char *end;
    long ma = strtol(v, &end, 10);
    if (*end != '.') return false;
    v = end + 1;
    if (*v < '0' || *v > '9') return false;
    long mi = strtol(v, &end, 10);
    if (*end != '.' && *end != '\0' && *end != '-') return false;
    v = (*end == '.') ? end + 1 : end;
    long pa = 0;
    if (*end == '.') {
        if (*v < '0' || *v > '9') return false;
        pa = strtol(v, &end, 10);
    }
    /* Reject trailing garbage.  "0.32.2junk" used to parse as 0.32.2, which is
     * tolerable for a lenient sort but wrong for validating a user-authored
     * constraint -- a typo in a `:tur-version` range must be an error, not a
     * silently different range.  Only end-of-string or a `-<pre>` suffix is
     * accepted; `+build` metadata is deliberately unsupported (nothing in the
     * toolchain emits it, and accepting it would imply we compare it). */
    if (*end != '\0' && *end != '-') return false;
    if (*end == '-' && end[1] == '\0') return false;  /* dangling '-' */
    *major = (int)ma;
    *minor = (int)mi;
    *patch = (int)pa;
    if (pre) {
        if (*end == '-')
            *pre = tur_strdup(end + 1);
        else
            *pre = NULL;
    }
    return true;
}

/* Compare one dot-separated pre-release identifier pair, semver rules:
 * all-numeric identifiers compare numerically, others lexically, and a numeric
 * identifier always ranks LOWER than an alphanumeric one. */
static int semver_pre_ident_cmp(const char *a, size_t alen,
                                const char *b, size_t blen) {
    bool a_num = alen > 0, b_num = blen > 0;
    for (size_t i = 0; i < alen; i++) if (a[i] < '0' || a[i] > '9') { a_num = false; break; }
    for (size_t i = 0; i < blen; i++) if (b[i] < '0' || b[i] > '9') { b_num = false; break; }
    if (a_num && b_num) {
        /* Length-then-lexical rather than strtol: no overflow on a long
         * identifier, and valid semver has no leading zeros anyway. */
        while (alen > 1 && *a == '0') { a++; alen--; }
        while (blen > 1 && *b == '0') { b++; blen--; }
        if (alen != blen) return alen < blen ? -1 : 1;
        int d = memcmp(a, b, alen);
        return d < 0 ? -1 : (d > 0 ? 1 : 0);
    }
    if (a_num != b_num) return a_num ? -1 : 1;   /* numeric < alphanumeric */
    size_t n = alen < blen ? alen : blen;
    int d = memcmp(a, b, n);
    if (d != 0) return d < 0 ? -1 : 1;
    if (alen != blen) return alen < blen ? -1 : 1;
    return 0;
}

/* Compare two pre-release strings ("rc1", "alpha.2").  NULL means "no
 * pre-release", which ranks HIGHER than any pre-release: 1.0.0 > 1.0.0-rc1. */
static int semver_pre_cmp(const char *a, const char *b) {
    if (!a && !b) return 0;
    if (!a) return  1;    /* release beats pre-release */
    if (!b) return -1;
    for (;;) {
        if (!*a && !*b) return 0;
        /* A shorter identifier list is lower when all preceding fields match. */
        if (!*a) return -1;
        if (!*b) return  1;
        const char *ae = strchr(a, '.'); size_t alen = ae ? (size_t)(ae - a) : strlen(a);
        const char *be = strchr(b, '.'); size_t blen = be ? (size_t)(be - b) : strlen(b);
        int d = semver_pre_ident_cmp(a, alen, b, blen);
        if (d != 0) return d;
        a = ae ? ae + 1 : a + alen;
        b = be ? be + 1 : b + blen;
    }
}

int pkg_semver_compare(const char *a, const char *b) {
    int ma, mi, pa, mb, mib, pb;
    char *prea = NULL, *preb = NULL;
    bool oka = pkg_semver_parse(a, &ma, &mi, &pa, &prea);
    bool okb = pkg_semver_parse(b, &mb, &mib, &pb, &preb);
    if (!oka && !okb) { free(prea); free(preb); return strcmp(a, b); }
    if (!oka) { free(prea); free(preb); return -1; }
    if (!okb) { free(prea); free(preb); return  1; }
    int d = (ma != mb) ? ma - mb : (mi != mib) ? mi - mib : pa - pb;
    /* Pre-release is a TIE-BREAKER, not ignored.  It used to be parsed and then
     * freed unread, so 0.33.0-rc1 and 0.33.0 compared EQUAL -- exactly the case
     * a version floor has to get right during a release cycle. */
    if (d == 0) d = semver_pre_cmp(prea, preb);
    free(prea);
    free(preb);
    return d;
}

/* ------------------------------------------------------------------ */
/* Version ranges (`:tur-version`)                                     */
/*                                                                     */
/* Grammar, Cargo-flavoured -- comma-separated conjuncts, each either a */
/* comparator or a caret:                                              */
/*                                                                     */
/*   range    := conjunct ("," conjunct)*                              */
/*   conjunct := (">=" | "<=" | ">" | "<" | "=")? version              */
/*             | "^" version                                           */
/*                                                                     */
/* A bare version means "=".  `^X.Y.Z` is the compatible-update range:  */
/* >=X.Y.Z and < the next version that could break it -- which for a   */
/* 0.x version is the next MINOR (0.32.2 -> <0.33.0), because pre-1.0  */
/* minors are breaking by convention.  That 0.x rule is the one people  */
/* get wrong, so it is spelled out in the guide.                        */
/*                                                                     */
/* Deliberately NOT supported: `~`, `*`, `||` disjunction, wildcards.   */
/* Each is easy to add later and none is needed to express a floor, a   */
/* ceiling, or a compatible range -- which is all this key is for.      */
/* ------------------------------------------------------------------ */

typedef enum { RG_GE, RG_GT, RG_LE, RG_LT, RG_EQ } RangeOp;

/* Skip ASCII spaces/tabs. */
static const char *rg_skip_ws(const char *s) {
    while (*s == ' ' || *s == '\t') s++;
    return s;
}

/* Parse one conjunct at *pp, advancing it past the conjunct.  On success writes
 * through `op` and `ver` (the latter a malloc'd version string the caller
 * frees) and returns true.  `is_caret` reports a `^` conjunct, which expands to
 * TWO bounds rather than one. */
static bool rg_parse_conjunct(const char **pp, RangeOp *op, char **ver,
                              bool *is_caret) {
    const char *s = rg_skip_ws(*pp);
    *is_caret = false;
    *op = RG_EQ;
    if (s[0] == '^')                       { *is_caret = true; s += 1; }
    else if (s[0] == '>' && s[1] == '=')   { *op = RG_GE; s += 2; }
    else if (s[0] == '<' && s[1] == '=')   { *op = RG_LE; s += 2; }
    else if (s[0] == '>')                  { *op = RG_GT; s += 1; }
    else if (s[0] == '<')                  { *op = RG_LT; s += 1; }
    else if (s[0] == '=')                  { *op = RG_EQ; s += 1; }
    s = rg_skip_ws(s);
    const char *start = s;
    while (*s && *s != ',' && *s != ' ' && *s != '\t') s++;
    if (s == start) return false;
    size_t n = (size_t)(s - start);
    char *v = (char *)malloc(n + 1);
    if (!v) return false;
    memcpy(v, start, n);
    v[n] = '\0';
    /* Validate eagerly: a malformed conjunct must be an error rather than a
     * silently different constraint. */
    int ma, mi, pa; char *pre = NULL;
    if (!pkg_semver_parse(v, &ma, &mi, &pa, &pre)) { free(pre); free(v); return false; }
    free(pre);
    *ver = v;
    *pp = rg_skip_ws(s);
    return true;
}

bool pkg_version_range_valid(const char *range) {
    if (!range || !*range) return false;
    const char *p = range;
    int n_conjuncts = 0;
    for (;;) {
        RangeOp op; char *ver = NULL; bool caret = false;
        if (!rg_parse_conjunct(&p, &op, &ver, &caret)) return false;
        free(ver);
        n_conjuncts++;
        p = rg_skip_ws(p);
        if (*p == ',') { p++; continue; }
        if (*p == '\0') break;
        return false;                       /* junk between conjuncts */
    }
    return n_conjuncts > 0;
}

/* Upper bound implied by `^v`: next minor for 0.x, next major otherwise. */
static void rg_caret_bound(const char *v, int *maj, int *min) {
    int ma = 0, mi = 0, pa = 0; char *pre = NULL;
    pkg_semver_parse(v, &ma, &mi, &pa, &pre);
    free(pre);
    if (ma == 0) { *maj = 0;      *min = mi + 1; }
    else         { *maj = ma + 1; *min = 0; }
}

bool pkg_version_range_match(const char *range, const char *version,
                             bool *out_below_floor) {
    if (out_below_floor) *out_below_floor = false;
    if (!range || !version) return true;
    const char *p = range;
    bool ok = true;
    for (;;) {
        RangeOp op; char *ver = NULL; bool caret = false;
        if (!rg_parse_conjunct(&p, &op, &ver, &caret)) return true;  /* invalid: caller validates */
        if (caret) {
            /* >= ver */
            if (pkg_semver_compare(version, ver) < 0) {
                ok = false;
                if (out_below_floor) *out_below_floor = true;
            } else {
                int bmaj, bmin;
                rg_caret_bound(ver, &bmaj, &bmin);
                char bound[64];
                snprintf(bound, sizeof(bound), "%d.%d.0", bmaj, bmin);
                if (pkg_semver_compare(version, bound) >= 0) ok = false;
            }
        } else {
            int c = pkg_semver_compare(version, ver);
            bool pass = (op == RG_GE) ? (c >= 0)
                      : (op == RG_GT) ? (c >  0)
                      : (op == RG_LE) ? (c <= 0)
                      : (op == RG_LT) ? (c <  0)
                      :                 (c == 0);
            if (!pass) {
                ok = false;
                /* A lower-bound conjunct the version fails is a FLOOR miss --
                 * the code genuinely predates what the spice needs.  Failing an
                 * upper bound only means untested-against, which the caller
                 * downgrades to a warning. */
                if ((op == RG_GE || op == RG_GT || op == RG_EQ) && c < 0 &&
                    out_below_floor)
                    *out_below_floor = true;
            }
        }
        free(ver);
        p = rg_skip_ws(p);
        if (*p == ',') { p++; continue; }
        break;
    }
    return ok;
}

/* ================================================================== */
/* Git operations                                                       */
/* ================================================================== */

/* Append one argument to a shell command, quoted for the platform's shell.
 *
 * Every command in this file used to interpolate `'%s'`, which cmd.exe does not
 * treat as quoting at all -- git received a path with the quotes still attached
 * and reported `could not create leading directories of ''./spices/demo''`.
 * That made `tur fetch` unable to fetch anything on Windows.  Returns false if
 * the argument does not fit; a truncated command string is worse than none, so
 * callers must fail rather than run it.
 * docs/reported/windows-spice-fetch-shell-quoting.md */
bool pkg_cmd_arg(Buf *cmd, const char *arg) {
    char q[8192];
    if (tur_shell_quote(arg, q, sizeof q) != 0) return false;
    buf_puts(cmd, q);
    return true;
}

char *pkg_git_resolve(const char *repo_dir) {
    Buf cmd;
    buf_init(&cmd);
    buf_puts(&cmd, "git -C ");
    bool ok = pkg_cmd_arg(&cmd, repo_dir);
    buf_puts(&cmd, " rev-parse HEAD 2>" TUR_DEVNULL);
    buf_putc(&cmd, '\0');
    char *sha = ok ? run_capture(cmd.data) : NULL;
    buf_free(&cmd);
    return sha;
}

char *pkg_git_fetch(const char *url, const char *ref, const char *dest_dir) {
    struct stat st;
    bool already_cloned = (stat(dest_dir, &st) == 0 && S_ISDIR(st.st_mode));

    Buf cmd;
    buf_init(&cmd);

    bool ok = true;
    if (!already_cloned) {
        /* Fresh clone */
        buf_puts(&cmd, "git clone --depth 1 ");
        if (ref) {
            buf_puts(&cmd, "--branch ");
            ok = pkg_cmd_arg(&cmd, ref) && ok;
            buf_putc(&cmd, ' ');
        }
        buf_puts(&cmd, "-- ");
        ok = pkg_cmd_arg(&cmd, url) && ok;
        buf_putc(&cmd, ' ');
        ok = pkg_cmd_arg(&cmd, dest_dir) && ok;
        buf_puts(&cmd, " 2>&1");
    } else {
        /* Fetch and checkout the desired ref */
        buf_puts(&cmd, "git -C ");
        ok = pkg_cmd_arg(&cmd, dest_dir) && ok;
        /* WP2 (D-6): `--` before the positional ref.  pkg_cmd_arg already
         * shell-quotes it, so `a; id` was never a shell injection -- but git
         * reads a quoted `--upload-pack=<cmd>` as an OPTION all the same, and
         * a ref comes out of a transitive manifest.  The clone branch above
         * always had its `--`; this one did not. */
        buf_puts(&cmd, " fetch --depth 1 origin -- ");
        ok = pkg_cmd_arg(&cmd, ref ? ref : "HEAD") && ok;
        buf_puts(&cmd, " 2>&1 && git -C ");
        ok = pkg_cmd_arg(&cmd, dest_dir) && ok;
        buf_puts(&cmd, " checkout FETCH_HEAD 2>&1");
    }
    buf_putc(&cmd, '\0');
    int rc = ok ? system(cmd.data) : -1;
    buf_free(&cmd);

    if (rc != 0) {
        fprintf(stderr, "spice: git failed for '%s' ref '%s' in '%s'\n",
                url, ref ? ref : "(default)", dest_dir);
        if (!ok)
            fprintf(stderr, "  (a path in that command was too long to quote)\n");
        if (already_cloned) {
            fprintf(stderr,
                "  hint: the cached clone has uncommitted changes or a "
                "conflicting state.\n"
                "        inspect with: git -C '%s' status\n"
                "        discard with: rm -rf '%s'\n",
                dest_dir, dest_dir);
        }
        return NULL;
    }
    return pkg_git_resolve(dest_dir);
}

/* A commit id as tur.lock records it: 40 hex digits (SHA-1) or 64 (SHA-256
 * repositories).  The value comes out of a file that travels with the
 * project, so it is checked before it reaches a git command line. */
static bool pkg_is_commit_id(const char *s) {
    if (!s) return false;
    size_t n = 0;
    for (; s[n]; n++)
        if (!((s[n] >= '0' && s[n] <= '9') || (s[n] >= 'a' && s[n] <= 'f')))
            return false;
    return n == 40 || n == 64;
}

static int pkg_git_in(const char *dest_dir, const char *args, const char *arg) {
    Buf cmd; buf_init(&cmd);
    buf_puts(&cmd, "git -C ");
    bool ok = pkg_cmd_arg(&cmd, dest_dir);
    buf_putc(&cmd, ' ');
    buf_puts(&cmd, args);
    if (arg) { buf_putc(&cmd, ' '); ok = pkg_cmd_arg(&cmd, arg) && ok; }
    buf_puts(&cmd, " >" TUR_DEVNULL " 2>&1");
    buf_putc(&cmd, '\0');
    int rc = ok ? system(cmd.data) : -1;
    buf_free(&cmd);
    return rc;
}

/* lock-tracks-ref-not-resolved-commit: tur.lock records the commit a fetch
 * resolved to, but the clone used to be built from the manifest's `:ref`
 * alone -- so on a fresh checkout a branch-shaped `:ref` took whatever the
 * branch pointed at NOW, and the only way past the integrity check was
 * `tur fetch --update`, which re-pins to the new commit.  The recorded commit
 * was the one thing `tur fetch` would not fetch.
 *
 * Clone as before, then move to the pinned commit.  `git clone --branch`
 * cannot take a SHA, hence the second step.  A depth-1 fetch of the bare SHA
 * is cheapest, but servers without `uploadpack.allowReachableSHA1InWant` (a
 * plain `git daemon`, a file:// remote) refuse it; then the branch's history
 * is fetched in full and the commit looked for there.  Neither step falls back
 * to the branch tip.  If the commit is gone (history rewritten), that is an
 * error and the clone is removed, so a later run cannot mistake it for the
 * pinned tree. */
char *pkg_git_fetch_pinned(const char *url, const char *ref, const char *pinned,
                           const char *dest_dir) {
    char *sha = pkg_git_fetch(url, ref, dest_dir);
    if (!sha || !pinned) return sha;
    if (!pkg_is_commit_id(pinned)) {
        fprintf(stderr,
            "spice: tur.lock pins '%s' to '%s', which is not a commit id;\n"
            "  refusing to fetch it.  Re-pin deliberately with: tur fetch --update\n",
            url, pinned);
        free(sha);
        (void)pkg_rm_rf(dest_dir);
        return NULL;
    }
    if (strcmp(sha, pinned) == 0) return sha;
    free(sha);
    const char *co = "-c advice.detachedHead=false checkout -q --detach";
    bool got = pkg_git_in(dest_dir, "fetch -q --depth 1 origin --", pinned) == 0 &&
               pkg_git_in(dest_dir, co, pinned) == 0;
    if (!got) {
        /* Full history of what was cloned; a non-shallow clone just fetches. */
        if (pkg_git_in(dest_dir, "fetch -q --unshallow origin", NULL) != 0)
            (void)pkg_git_in(dest_dir, "fetch -q origin", NULL);
        got = pkg_git_in(dest_dir, co, pinned) == 0;
    }
    sha = got ? pkg_git_resolve(dest_dir) : NULL;
    if (sha && strcmp(sha, pinned) == 0) return sha;
    free(sha);
    fprintf(stderr,
        "spice: tur.lock pins %s (ref: %s) to commit %s,\n"
        "  and that commit cannot be fetched from it any more -- the history\n"
        "  was rewritten, or the server will not serve it.  Not falling back to\n"
        "  the branch: that would silently undo the pin.  If the change is\n"
        "  expected, re-pin deliberately:\n"
        "      tur fetch --update\n",
        url, ref ? ref : "(default)", pinned);
    (void)pkg_rm_rf(dest_dir);
    return NULL;
}

/* ================================================================== */
/* pkg_fetch_all -- BFS transitive resolution                          */
/* ================================================================== */

/* A pending fetch item */
typedef struct FetchItem {
    char *name;
    char *url;
    char *ref;
    char *path;   /* NULL = git dep */
    char *subdir; /* NULL = repo root; set for monorepo sub-packages */
    bool  is_global; /* `#{:global true}` -- owned by `tur install`, never fetched */
    bool  is_cmake;
    bool  from_root; /* LS3: true iff this item came from the root manifest */
    /* tur-fetch-exit-code-optional-vs-required: `:optional true` on the
     * declaring manifest.  A failed fetch of an optional dep is reported and
     * skipped rather than failing the run, and `tur fetch` says so in its
     * exit status (1 = only optional deps failed, 2 = a required one did). */
    bool  optional;
    /* origin for error reporting */
    char *from;
} FetchItem;

/* LS3: how deep to walk looking for an enclosing workspace manifest.
 * Must accommodate worktrees and nested checkouts; mirrors the resolver's
 * TUR_SPICE_WALK_MAX in main.c. */
#define PKG_WORKSPACE_WALK_MAX 16

/* LS3: discover the names of *other* members of the workspace enclosing
 * `project_dir`, if any.  Walks parents of `project_dir` looking for a
 * `build.tur` that declares :members containing `project_dir`.  The
 * member's name is read from its own `build.tur` `:name`, falling back
 * to the basename of the member path when the manifest is unreadable or
 * has no `:name`.
 *
 * Returns a NULL-terminated, heap-allocated array of strings (and sets
 * *out_n to the count) on success.  Returns NULL with *out_n = 0 when
 * `project_dir` is not part of any workspace.  Caller frees each string
 * and the array.
 *
 * The workspace member list is used by `tur fetch` to skip any `:spices`
 * entry whose dep name matches a sibling member -- the workspace is the
 * dep source, not the remote URL. */
static char **collect_workspace_member_names(const char *project_dir,
                                              int *out_n) {
    *out_n = 0;
    if (!project_dir) return NULL;

    char real_root[4096];
    if (realpath(project_dir, real_root) == NULL) return NULL;

    char anc[4096];
    size_t rlen = strlen(real_root);
    if (rlen + 1 > sizeof(anc)) return NULL;
    memcpy(anc, real_root, rlen + 1);

    for (int up = 0; up < PKG_WORKSPACE_WALK_MAX; up++) {
        char *last = strrchr(anc, '/');
        if (!last || last == anc) break;
        *last = '\0';

        char ws_manifest[4096];
        if (!pkg_resolve_manifest_path(anc, ws_manifest,
                                       sizeof(ws_manifest)))
            continue;

        PkgManifest wm;
        memset(&wm, 0, sizeof(wm));
        if (!pkg_manifest_read(ws_manifest, &wm)) continue;

        if (wm.n_members <= 0) {
            /* Any build.tur (workspace or not) terminates the walk so
             * we don't accidentally treat a non-workspace ancestor's
             * project as the enclosing workspace. */
            pkg_manifest_free(&wm);
            break;
        }

        /* Confirm project_dir is itself a listed member of this
         * candidate workspace. */
        bool is_member = false;
        for (int i = 0; i < wm.n_members; i++) {
            char mp[4096];
            int mn = snprintf(mp, sizeof(mp), "%s/%s",
                              anc, wm.members[i]);
            if (mn <= 0 || (size_t)mn >= sizeof(mp)) continue;
            char real_mp[4096];
            if (realpath(mp, real_mp) == NULL) continue;
            if (strcmp(real_mp, real_root) == 0) {
                is_member = true;
                break;
            }
        }
        if (!is_member) {
            pkg_manifest_free(&wm);
            break;
        }

        char **names = (char **)calloc((size_t)wm.n_members + 1,
                                        sizeof(char *));
        if (!names) { pkg_manifest_free(&wm); return NULL; }
        int n = 0;
        for (int i = 0; i < wm.n_members; i++) {
            char member_dir[4096];
            int mn = snprintf(member_dir, sizeof(member_dir),
                              "%s/%s", anc, wm.members[i]);
            char member_manifest[4096];
            const char *resolved_name = NULL;
            PkgManifest mm;
            memset(&mm, 0, sizeof(mm));
            bool mm_ok = false;
            if (mn > 0 && (size_t)mn < sizeof(member_dir)
                && pkg_resolve_manifest_path(member_dir, member_manifest,
                                             sizeof(member_manifest))
                && pkg_manifest_read(member_manifest, &mm)) {
                mm_ok = true;
                if (mm.name && mm.name[0]) resolved_name = mm.name;
            }
            if (!resolved_name) {
                /* Fall back to the basename of the member path. */
                const char *slash = strrchr(wm.members[i], '/');
                resolved_name = slash ? slash + 1 : wm.members[i];
            }
            if (resolved_name && resolved_name[0])
                names[n++] = tur_strdup(resolved_name);
            if (mm_ok) pkg_manifest_free(&mm);
        }
        names[n] = NULL;
        *out_n = n;
        pkg_manifest_free(&wm);
        return names;
    }
    return NULL;
}

/* LS3: case-sensitive lookup of `name` in a names[]/n_names list. */
static bool name_in_list(const char *name, char **names, int n_names) {
    if (!name || !names) return false;
    for (int i = 0; i < n_names; i++) {
        if (names[i] && strcmp(names[i], name) == 0) return true;
    }
    return false;
}

/* LS4: thin wrapper over collect_workspace_member_names so cmd_run (and
 * any other resolution-time consumer) can ask "is this :spices entry a
 * workspace sibling?" without re-implementing the workspace walk.
 * Returns true iff `project_dir` is itself a member of some enclosing
 * workspace AND that workspace lists a sibling member whose name (per
 * its own build.tur `:name`, or basename fallback) matches `name`. */
bool pkg_is_workspace_member(const char *project_dir, const char *name) {
    if (!project_dir || !name || !name[0]) return false;
    int n = 0;
    char **names = collect_workspace_member_names(project_dir, &n);
    bool hit = name_in_list(name, names, n);
    if (names) {
        for (int i = 0; i < n; i++) free(names[i]);
        free(names);
    }
    return hit;
}

/* LS4: resolve a workspace-sibling `:spices` entry to the absolute path
 * of the sibling's directory.  Walks parents of `project_dir` looking
 * for a `build.tur` whose `:members [...]` list contains `project_dir`
 * itself; in that workspace, returns the first member whose declared
 * `:name` (or basename fallback) matches `dep_name`.  Returns NULL when
 * no enclosing workspace lists a matching sibling.  Caller frees. */
char *pkg_workspace_member_path(const char *project_dir, const char *dep_name) {
    if (!project_dir || !dep_name || !dep_name[0]) return NULL;

    char real_root[4096];
    if (realpath(project_dir, real_root) == NULL) return NULL;

    char anc[4096];
    size_t rlen = strlen(real_root);
    if (rlen + 1 > sizeof(anc)) return NULL;
    memcpy(anc, real_root, rlen + 1);

    for (int up = 0; up < PKG_WORKSPACE_WALK_MAX; up++) {
        char *last = strrchr(anc, '/');
        if (!last || last == anc) break;
        *last = '\0';

        char ws_manifest[4096];
        if (!pkg_resolve_manifest_path(anc, ws_manifest,
                                       sizeof(ws_manifest)))
            continue;

        PkgManifest wm;
        memset(&wm, 0, sizeof(wm));
        if (!pkg_manifest_read(ws_manifest, &wm)) continue;

        if (wm.n_members <= 0) {
            pkg_manifest_free(&wm);
            break;
        }

        bool is_member = false;
        for (int i = 0; i < wm.n_members; i++) {
            char mp[4096];
            int mn = snprintf(mp, sizeof(mp), "%s/%s",
                              anc, wm.members[i]);
            if (mn <= 0 || (size_t)mn >= sizeof(mp)) continue;
            char real_mp[4096];
            if (realpath(mp, real_mp) == NULL) continue;
            if (strcmp(real_mp, real_root) == 0) { is_member = true; break; }
        }
        if (!is_member) { pkg_manifest_free(&wm); break; }

        char *found = NULL;
        for (int i = 0; i < wm.n_members && !found; i++) {
            char member_dir[4096];
            int mn = snprintf(member_dir, sizeof(member_dir),
                              "%s/%s", anc, wm.members[i]);
            char member_manifest[4096];
            const char *resolved_name = NULL;
            PkgManifest mm;
            memset(&mm, 0, sizeof(mm));
            bool mm_ok = false;
            if (mn > 0 && (size_t)mn < sizeof(member_dir)
                && pkg_resolve_manifest_path(member_dir, member_manifest,
                                             sizeof(member_manifest))
                && pkg_manifest_read(member_manifest, &mm)) {
                mm_ok = true;
                if (mm.name && mm.name[0]) resolved_name = mm.name;
            }
            if (!resolved_name) {
                const char *slash = strrchr(wm.members[i], '/');
                resolved_name = slash ? slash + 1 : wm.members[i];
            }
            if (resolved_name && strcmp(resolved_name, dep_name) == 0) {
                char mp[4096];
                snprintf(mp, sizeof(mp), "%s/%s", anc, wm.members[i]);
                found = tur_strdup(mp);
            }
            if (mm_ok) pkg_manifest_free(&mm);
        }
        pkg_manifest_free(&wm);
        return found;
    }
    return NULL;
}

/* Conflict table entry */
typedef struct ConflictEntry {
    char *name;
    char *url;
    char *ref;
} ConflictEntry;

bool pkg_fetch_all(const char *project_dir,
                   const PkgManifest *manifest,
                   PkgLockFile *lock,
                   bool update,
                   bool *out_optional_failed) {
    if (out_optional_failed) *out_optional_failed = false;
    /* Create spices/ directory */
    char spices_dir[4096];
    snprintf(spices_dir, sizeof(spices_dir), "%s/spices", project_dir);
    if (!mkdirp(spices_dir)) {
        fprintf(stderr, "spice: cannot create '%s': %s\n",
                spices_dir, strerror(errno));
        return false;
    }

    /* BFS queue */
    int q_cap = 32;
    int q_len = 0;
    int q_head = 0;
    FetchItem *queue = (FetchItem *)malloc(q_cap * sizeof(FetchItem));
    if (!queue) return false;

    /* Conflict table (name → url@ref) */
    int cf_cap = 16;
    int n_cf   = 0;
    ConflictEntry *conflicts = (ConflictEntry *)malloc(cf_cap * sizeof(ConflictEntry));
    if (!conflicts) { free(queue); return false; }

    /* LS3: collect the set of workspace-sibling member names so any
     * `:spices` entry that refers to a sibling is skipped (no fetch,
     * no lock row).  The workspace is the dep source, not the URL. */
    int n_ws_members = 0;
    char **ws_member_names = collect_workspace_member_names(project_dir,
                                                             &n_ws_members);

    /* Seed the queue from the manifest's direct spices */
    for (int i = 0; i < manifest->n_spices; i++) {
        const PkgSpice *s = &manifest->spices[i];
        if (q_len >= q_cap) {
            q_cap *= 2;
            queue = (FetchItem *)realloc(queue, q_cap * sizeof(FetchItem));
            if (!queue) return false;
        }
        FetchItem *it = &queue[q_len++];
        it->name     = tur_strdup(s->name);
        it->url      = s->url    ? tur_strdup(s->url)    : NULL;
        it->ref      = s->ref    ? tur_strdup(s->ref)    : NULL;
        it->path     = s->path   ? tur_strdup(s->path)   : NULL;
        it->subdir   = s->subdir ? tur_strdup(s->subdir) : NULL;
        it->is_global = s->is_global;
        it->is_cmake = false;
        it->from_root = true;
        it->optional = s->optional;
        it->from     = tur_strdup("(root)");
    }

    bool ok = true;

    while (q_head < q_len) {
        FetchItem *it = &queue[q_head++];

        if (it->is_cmake) {
            /* cmake deps are handled in pkg_gen_cmake_deps, not fetched here */
            free(it->name); free(it->url); free(it->ref);
            free(it->path); free(it->subdir); free(it->from);
            continue;
        }

        /* global-spice-library-consumption: a `:global` dep is owned by
         * `tur install`; there is nothing to fetch and no lock row to keep
         * (its checkout is the registry's, recorded in state.tur).  A direct
         * (root) dep that is not installed is a hard error, symmetric with a
         * `:path` dep whose directory is missing -- the alternative is a
         * "module not found" a hundred lines later with no hint that a spice
         * was never installed. */
        if (it->is_global) {
            if (it->from_root) {
                char gdir[4096];
                if (!tur_installed_spice_dir(it->name, gdir, sizeof(gdir),
                                             NULL, NULL)) {
                    fprintf(stderr,
                        "spice: '%s' declares :global true but no such spice "
                        "is installed -- run `tur install <source>` first "
                        "(from %s)\n",
                        it->name, it->from);
                    ok = false;
                }
            }
            (void)lock_remove(lock, it->name, false);
            free(it->name); free(it->url); free(it->ref);
            free(it->path); free(it->subdir); free(it->from);
            continue;
        }

        /* LS3: Local path dep — skip the fetch entirely, drop any stale
         * lock row.  The resolver picks the source up directly from
         * `<:path>/src` at compile time, so reproducibility for local
         * deps is owned by git, not the lockfile.  Direct (root)
         * `:path` deps that fail to exist on disk are a hard error,
         * symmetric with how missing URL refs behave. */
        if (it->path) {
            if (it->from_root) {
                char abs_path[4096];
                int an = (it->path[0] == '/')
                    ? snprintf(abs_path, sizeof(abs_path), "%s", it->path)
                    : snprintf(abs_path, sizeof(abs_path), "%s/%s",
                               project_dir, it->path);
                bool path_ok = false;
                if (an > 0 && (size_t)an < sizeof(abs_path)) {
                    struct stat pst;
                    if (stat(abs_path, &pst) == 0 && S_ISDIR(pst.st_mode)) {
                        char mp[4096];
                        int mn = snprintf(mp, sizeof(mp), "%s/build.tur",
                                          abs_path);
                        if (mn > 0 && (size_t)mn < sizeof(mp)) {
                            struct stat mst;
                            if (stat(mp, &mst) == 0 && S_ISREG(mst.st_mode))
                                path_ok = true;
                        }
                    }
                }
                if (!path_ok) {
                    fprintf(stderr,
                        "spice: '%s' declares :path \"%s\" but the "
                        "directory does not exist or contains no "
                        "build.tur (from %s)\n",
                        it->name, it->path, it->from);
                    ok = false;
                }
            }
            (void)lock_remove(lock, it->name, false);
            free(it->name); free(it->url); free(it->ref);
            free(it->path); free(it->subdir); free(it->from);
            continue;
        }

        /* LS3: workspace-member dep — the workspace resolver already
         * supplies sibling members at compile time; `tur fetch` should
         * not try to fetch a URL for them, and the lockfile should not
         * record them. */
        if (it->from_root && name_in_list(it->name, ws_member_names,
                                          n_ws_members)) {
            (void)lock_remove(lock, it->name, false);
            free(it->name); free(it->url); free(it->ref);
            free(it->path); free(it->subdir); free(it->from);
            continue;
        }

        /* Check for conflicts with previously seen deps */
        bool conflict = false;
        for (int c = 0; c < n_cf; c++) {
            if (strcmp(conflicts[c].name, it->name) == 0) {
                /* Same name — check if same url+ref */
                bool same_url = (!conflicts[c].url && !it->url) ||
                                (conflicts[c].url && it->url &&
                                 strcmp(conflicts[c].url, it->url) == 0);
                bool same_ref = (!conflicts[c].ref && !it->ref) ||
                                (conflicts[c].ref && it->ref &&
                                 strcmp(conflicts[c].ref, it->ref) == 0);
                if (!same_url || !same_ref) {
                    fprintf(stderr,
                        "spice: conflict! '%s' required as '%s@%s' "
                        "(from %s) but already resolved as '%s@%s'\n",
                        it->name,
                        it->url ? it->url : "(local)",
                        it->ref ? it->ref : "(default)",
                        it->from,
                        conflicts[c].url ? conflicts[c].url : "(local)",
                        conflicts[c].ref ? conflicts[c].ref : "(default)");
                    ok = false;
                }
                conflict = true;
                break;
            }
        }
        if (conflict) {
            free(it->name); free(it->url); free(it->ref);
            free(it->path); free(it->subdir); free(it->from);
            continue;
        }

        /* Register in conflict table */
        if (n_cf >= cf_cap) {
            cf_cap *= 2;
            conflicts = (ConflictEntry *)realloc(conflicts,
                cf_cap * sizeof(ConflictEntry));
            if (!conflicts) { ok = false; break; }
        }
        ConflictEntry *ce = &conflicts[n_cf++];
        ce->name = tur_strdup(it->name);
        ce->url  = it->url ? tur_strdup(it->url) : NULL;
        ce->ref  = it->ref ? tur_strdup(it->ref) : NULL;

        /* Build dest path: spices/<name>-<ref> or spices/<name>.  Computed
         * BEFORE the lock check, which has to know whether it is on disk. */
        char dest[4096];
        if (it->ref)
            snprintf(dest, sizeof(dest), "%s/%s-%s",
                     spices_dir, it->name, it->ref);
        else
            snprintf(dest, sizeof(dest), "%s/%s", spices_dir, it->name);

        /* In the lock AND actually present?  Then there is nothing to do.
         *
         * C-3: the stat() is the load-bearing half, and it was missing.  A
         * lock row on its own used to satisfy this, so on the shape that
         * matters most -- a fresh clone, tur.lock committed, spices/
         * gitignored and therefore absent -- `tur fetch` printed
         * "spice: using cached '<name>' @ <sha>" and downloaded NOTHING,
         * leaving the build to fail afterwards with "module not found".
         * Measured against 82ccdc555 before the fix.
         *
         * It also meant the recorded :sha256 and :resolved were never
         * consulted on the one path where a comparison is possible. They were
         * printed, never checked. */
        struct stat dest_st;
        PkgLockEntry *le = pkg_lock_find(lock, it->name, false);
        bool dest_present = (stat(dest, &dest_st) == 0 && S_ISDIR(dest_st.st_mode));
        if (le && !update && dest_present) {
            fprintf(stderr, "spice: using cached '%s' @ %s\n",
                    it->name, le->resolved ? le->resolved : le->ref);
            free(it->name); free(it->url); free(it->ref);
            free(it->path); free(it->subdir); free(it->from);
            continue;
        }

        /* C-3: what the lock says this tree hashed to last time, captured
         * before the fetch below overwrites it.  Only a hash THIS algorithm
         * produced can be compared -- an older tur wrote a `tar -c | sha256sum`
         * digest or the git-SHA fallback, and pkg_hash_comparable rejects
         * those rather than reporting a format change as tampering.  NULL
         * under --update: re-pinning is the user asking for the new content. */
        char *prev_sha = (le && !update && pkg_hash_comparable(le->sha256))
                       ? tur_strdup(le->sha256) : NULL;

        fprintf(stderr, "spice: fetching '%s' from %s (ref: %s) ...\n",
                it->name, it->url, it->ref ? it->ref : "(default)");

        /* Check out the commit the lock recorded, unless the consumer asked
         * for new content or changed what the dep points at. */
        const char *pin = NULL;
        if (le && !update && le->resolved &&
            ((!le->url && !it->url) || (le->url && it->url && strcmp(le->url, it->url) == 0)) &&
            ((!le->ref && !it->ref) || (le->ref && it->ref && strcmp(le->ref, it->ref) == 0)))
            pin = le->resolved;
        char *resolved = pkg_git_fetch_pinned(it->url, it->ref, pin, dest);
        if (!resolved) {
            if (it->optional) {
                /* tur-fetch-exit-code-optional-vs-required: an `:optional`
                 * dep is EXPECTED to be unfetchable sometimes (a test-only
                 * spice, a platform the consumer does not have).  Say so,
                 * drop its stale lock row, and carry on -- but report it
                 * through the out-param so the CLI can exit 1 rather than
                 * 0, distinct from the 2 a required failure earns. */
                fprintf(stderr,
                        "spice: failed to fetch optional '%s' -- skipped "
                        "(from %s)\n", it->name, it->from);
                (void)lock_remove(lock, it->name, false);
                if (out_optional_failed) *out_optional_failed = true;
            } else {
                fprintf(stderr, "spice: failed to fetch '%s'\n", it->name);
                ok = false;
            }
            free(prev_sha);
            free(it->name); free(it->url); free(it->ref);
            free(it->path); free(it->subdir); free(it->from);
            continue;
        }

        /* Update lock entry */
        le = lock_upsert(lock, it->name, false);
        if (le) {
            free(le->url);       le->url       = it->url ? tur_strdup(it->url) : NULL;
            free(le->ref);       le->ref       = it->ref ? tur_strdup(it->ref) : NULL;
            free(le->resolved);  le->resolved  = resolved;
            free(le->fetched_at);le->fetched_at= tur_strdup(iso_now());
            /* Content hash of the fetched tree, for `tur run`'s integrity
             * check.  The git-SHA fallback is provenance only: it does not
             * carry PKG_TREE_HASH_TAG, so pkg_hash_comparable rejects it and
             * the check is skipped rather than failing against a value this
             * algorithm never produced. */
            char dir_sha[PKG_HASH_MAX];
            bool hashed = pkg_hash_dir(dest, dir_sha);

            /* C-3: COMPARE, do not merely record.
             *
             * This is the one moment upstream drift is detectable: the lock
             * says what this tree hashed to when it was pinned, and a
             * freshly-fetched tree is right here to check it against.  The
             * old code overwrote the recorded hash unconditionally with
             * whatever had just been downloaded, so it could only ever catch
             * a LOCAL edit made after a fetch -- never the upstream moving
             * under a branch-shaped :ref, which is the case the consuming
             * guide warns about.
             *
             * `tur fetch --update` is the escape hatch and the diagnostic says
             * so: taking new content is a decision the consumer makes on
             * purpose, not a default. */
            if (prev_sha && hashed && strcmp(dir_sha, prev_sha) != 0) {
                fprintf(stderr,
                    "spice: integrity check FAILED for '%s'\n"
                    "  tur.lock pinned: %s\n"
                    "  just fetched:    %s\n"
                    "  The content at %s (ref: %s) is not what this lockfile\n"
                    "  recorded. A branch-shaped :ref moves under you; a tag or\n"
                    "  a commit does not. If the change is expected, take it\n"
                    "  deliberately:\n"
                    "      tur fetch --update\n",
                    it->name, prev_sha, dir_sha,
                    it->url, it->ref ? it->ref : "(default)");
                ok = false;
                /* The lock row is left alone on purpose.  Rewriting the hash
                 * here would make the very next command agree with the drift
                 * and report nothing -- the failure would last exactly one
                 * run, which is the same as not having it. */
            } else {
                free(le->sha256);
                if (hashed) le->sha256 = tur_strdup(dir_sha);
                else        le->sha256 = tur_strdup(resolved); /* git SHA */
            }
            free(prev_sha);
        } else {
            free(prev_sha);
            free(resolved);
        }

        /* Read transitive deps from this spice's build.tur.
         * If :subdir is set, the manifest lives inside the monorepo subdir. */
        char sub_dir[4096];
        if (it->subdir)
            snprintf(sub_dir, sizeof(sub_dir), "%s/%s", dest, it->subdir);
        else
            snprintf(sub_dir, sizeof(sub_dir), "%s", dest);
        char sub_build[4096];
        if (pkg_resolve_manifest_path(sub_dir, sub_build, sizeof(sub_build))) {
            PkgManifest sub;
            if (pkg_manifest_read(sub_build, &sub)) {
                /* Record transitive deps in lock entry */
                if (le) {
                    for (int j = 0; j < le->n_transitive; j++)
                        free(le->transitive[j]);
                    free(le->transitive);
                    le->transitive   = NULL;
                    le->n_transitive = 0;
                    int tr_cap = sub.n_spices;
                    if (tr_cap > 0)
                        le->transitive = (char **)malloc(tr_cap * sizeof(char *));
                }
                /* Enqueue sub-spices */
                for (int j = 0; j < sub.n_spices; j++) {
                    const PkgSpice *ss = &sub.spices[j];
                    if (q_len >= q_cap) {
                        q_cap *= 2;
                        queue = (FetchItem *)realloc(queue,
                            q_cap * sizeof(FetchItem));
                        if (!queue) { ok = false; break; }
                    }
                    FetchItem *nit = &queue[q_len++];
                    nit->name     = tur_strdup(ss->name);
                    nit->url      = ss->url    ? tur_strdup(ss->url)    : NULL;
                    nit->ref      = ss->ref    ? tur_strdup(ss->ref)    : NULL;
                    nit->path     = ss->path   ? tur_strdup(ss->path)   : NULL;
                    nit->subdir   = ss->subdir ? tur_strdup(ss->subdir) : NULL;
                    nit->is_global = ss->is_global;
                    nit->is_cmake = false;
                    nit->from_root = false;
                    nit->optional = ss->optional;
                    char from_buf[256];
                    snprintf(from_buf, sizeof(from_buf), "%s@%s",
                             it->name, it->ref ? it->ref : "HEAD");
                    nit->from = tur_strdup(from_buf);

                    /* record in transitive list */
                    if (le && le->transitive) {
                        char tr_str[512];
                        snprintf(tr_str, sizeof(tr_str), "%s@%s",
                                 ss->name, ss->ref ? ss->ref : "HEAD");
                        le->transitive[le->n_transitive++] = tur_strdup(tr_str);
                    }
                }
                pkg_manifest_free(&sub);
            }
        }

        free(it->name); free(it->url); free(it->ref);
        free(it->path); free(it->subdir); free(it->from);
    }

    /* Free remaining unprocessed items */
    while (q_head < q_len) {
        FetchItem *it = &queue[q_head++];
        free(it->name); free(it->url); free(it->ref);
        free(it->path); free(it->subdir); free(it->from);
    }
    free(queue);

    /* Free conflict table */
    for (int i = 0; i < n_cf; i++) {
        free(conflicts[i].name);
        free(conflicts[i].url);
        free(conflicts[i].ref);
    }
    free(conflicts);

    /* LS3: free workspace-member name list */
    if (ws_member_names) {
        for (int i = 0; i < n_ws_members; i++) free(ws_member_names[i]);
        free(ws_member_names);
    }

    return ok;
}

/* ================================================================== */
/* CMake dependency file generation                                    */
/* ================================================================== */

/* Return the link-lib name to use for a cmake dep.
 * Rules (in order):
 *   1. If :targets is set, use the last component of the first target after "::".
 *   2. If :cmake-name is set, use cmake-name.
 *   3. Use the dep name.
 * The returned pointer points into existing strings; do not free. */
static const char *cmake_dep_link_lib(const PkgCmakeDep *d) {
    if (d->n_targets > 0) {
        const char *t = d->targets[0];
        const char *sep = strrchr(t, ':');
        return (sep && sep[1]) ? sep + 1 : t;
    }
    if (d->cmake_name) return d->cmake_name;
    return d->name;
}

/* Strip a "Namespace::" prefix off a CMake target name.
 *   "MbedTLS::mbedtls" -> "mbedtls"
 *   "mbedtls"          -> "mbedtls"
 * Returns a pointer into the input string; do not free. */
static const char *cmake_target_basename(const char *target) {
    const char *sep = strrchr(target, ':');
    return (sep && sep[1]) ? sep + 1 : target;
}

/* Resolve a :cmake-deps `:path` to a directory, given the project being built.
 *
 * Two forms coexist in one manifest array, which is the whole reason this
 * helper exists. A dep declared by the project being built carries `:path` as
 * written -- relative to that manifest. A dep collected **transitively** from a
 * workspace sibling has already been absolutized against its origin dir by
 * append_cmake_dep_with_conflict_check, because the sibling's relative path is
 * meaningless from here.
 *
 * Prefixing the absolute form produces `<project_dir>//abs/path`, or
 * `.//abs/path` when project_dir is "." (the `tur fetch` path). CMake reads
 * both as relative and `add_subdirectory` fails with "given source ... which is
 * not an existing directory", so no dependency builds at all. The `:spices`
 * transitive walk already makes exactly this leading-'/' test; the
 * `:cmake-deps` side did not. */
static void cmake_dep_path_dir(const PkgCmakeDep *d, const char *project_dir,
                               char *out, size_t cap) {
    const char *p = d->path ? d->path : "";
    if (p[0] == '/')
        snprintf(out, cap, "%s", p);
    else
        /* Fallback only. append_cmake_dep_with_conflict_check absolutizes
         * every :path dep it collects, so in practice the branch above is the
         * one that runs; this covers a dep that reached here without passing
         * through the collector. */
        snprintf(out, cap, "%s/cmake/%s", project_dir, p);
}

/* Emit the CMake that computes _spice_<name>_inc / _spice_<name>_bld from a
 * FetchContent-built dependency's SOURCE_DIR / BINARY_DIR. Shared by the
 * plain (fetch-only) deps and the fetch fallback branch of :prefer-system
 * deps. */
static void emit_fetch_inc_bld(FILE *f, const char *name) {
    fprintf(f, "FetchContent_GetProperties(%s)\n", name);
    fprintf(f, "if(EXISTS \"${%s_SOURCE_DIR}/include\")\n", name);
    fprintf(f, "  set(_spice_%s_inc \"${%s_SOURCE_DIR}/include\")\n",
            name, name);
    fprintf(f, "else()\n");
    fprintf(f, "  set(_spice_%s_inc \"${%s_SOURCE_DIR}\")\n", name, name);
    fprintf(f, "endif()\n");
    fprintf(f, "set(_spice_%s_bld \"${%s_BINARY_DIR}\")\n", name, name);
}

/* Emit the JSON "include_dirs" line for one manifest entry.
 *   from_targets -- when true (the system find_package branch), build the
 *     array from each target's INTERFACE_INCLUDE_DIRECTORIES. That property
 *     is a CMake ;-list, so a single target may carry several dirs; the
 *     $<JOIN:...,", "> genexpr rewrites the ; separators into JSON array
 *     element boundaries at file(GENERATE) time, so a multi-dir system
 *     package lands as ["d1", "d2"] instead of a single "d1;d2" string that
 *     would produce a bogus -Id1;d2 flag downstream. When false (the fetch
 *     branch, path deps, or non-prefer-system deps), emit the single
 *     precomputed ${_spice_<name>_inc} dir. */
static void emit_include_dirs_line(FILE *f, const PkgCmakeDep *d,
                                   bool from_targets) {
    if (from_targets && d->n_targets > 0) {
        fprintf(f, "  \"    \\\"include_dirs\\\": [");
        for (int j = 0; j < d->n_targets; j++) {
            fprintf(f,
                "%s\\\"$<JOIN:$<TARGET_PROPERTY:%s,"
                "INTERFACE_INCLUDE_DIRECTORIES>,\\\", \\\">\\\"",
                j ? ", " : "", d->targets[j]);
        }
        fprintf(f, "],\\n\"\n");
    } else {
        fprintf(f, "  \"    \\\"include_dirs\\\": "
                   "[\\\"${_spice_%s_inc}\\\"],\\n\"\n", d->name);
    }
}

/* Emit the JSON "link_dirs"/"link_libs"/"link_flags" lines for one manifest
 * entry.
 *   use_full_targets -- when true, the $<TARGET_*:...> genexprs are keyed off
 *     the fully-qualified target name (e.g. "MbedTLS::mbedtls"), which is what
 *     a system find_package() imports. When false, the namespace-stripped
 *     basename ("mbedtls") is used, which is what the FetchContent build
 *     exports. When :targets is empty, both branches fall back to the dep's
 *     BINARY_DIR var and the single cmake_dep_link_lib() name.
 *
 * With :targets declared, the link line is asked of CMake rather than
 * reconstructed from the target's *name*:
 *
 *   - the artifact is $<TARGET_FILE:t>, the exact file the target produces.
 *     That resolves OUTPUT_NAME (glfw's target is `glfw`, its archive is
 *     `libglfw3.a`), namespace aliasing (PostgreSQL::PostgreSQL -> libpq), and
 *     static-vs-shared preference (zlib builds both `libz.a` and `libz.1.dylib`
 *     into one directory, where a `-lz` resolves to the dylib and then fails at
 *     load with "Library not loaded: @rpath/libz.1.dylib -- no LC_RPATH's
 *     found"). A name-derived `-l` reaches none of the three.
 *
 *   - the transitive requirements are INTERFACE_LINK_LIBRARIES, which is where
 *     `-framework Cocoa` / `-framework IOKit` live for the Objective-C macOS
 *     backends of glfw and raylib. A framework cannot be spelled as `-l` at
 *     all, so before this there was no link line that worked on macOS -- the
 *     failure was `ld: symbol(s) not found for architecture arm64` with
 *     `_objc_retain` missing, which reads as a toolchain problem rather than a
 *     missing flag. This is the sibling property of the
 *     INTERFACE_INCLUDE_DIRECTORIES that emit_include_dirs_line already reads.
 *
 * $<TARGET_FILE:...> is an error on an INTERFACE library (a header-only dep
 * has no artifact), so it is wrapped in a TYPE guard. CMake does not evaluate
 * the false arm of a $<cond:...>, so the guard is safe and yields an empty
 * element, which the consumer skips.
 *
 * A dep's own `:link-libs` overrides all of that -- including `:link-libs []`,
 * which links nothing (raygui is header-only; glad is a code generator that
 * builds no library), the one thing no genexpr can express. */
static void emit_link_lines(FILE *f, const PkgCmakeDep *d,
                            const char *link_lib, bool use_full_targets) {
    /* link_dirs */
    if (d->n_targets > 0) {
        fprintf(f, "  \"    \\\"link_dirs\\\":    [");
        for (int j = 0; j < d->n_targets; j++) {
            const char *t = use_full_targets
                                ? d->targets[j]
                                : cmake_target_basename(d->targets[j]);
            fprintf(f, "%s\\\"$<TARGET_FILE_DIR:%s>\\\"", j ? ", " : "", t);
        }
        fprintf(f, "],\\n\"\n");
    } else {
        fprintf(f, "  \"    \\\"link_dirs\\\":    [\\\"${_spice_%s_bld}\\\"],\\n\"\n",
                d->name);
    }

    /* link_libs -- the explicit override, else the derived name, else nothing
     * (because :targets deps link by artifact path via link_flags below). */
    fprintf(f, "  \"    \\\"link_libs\\\":    [");
    if (d->has_link_libs) {
        for (int j = 0; j < d->n_link_libs; j++)
            fprintf(f, "%s\\\"%s\\\"", j ? ", " : "", d->link_libs[j]);
    } else if (d->n_targets == 0) {
        fprintf(f, "\\\"%s\\\"", link_lib);
    }
    fprintf(f, "],\\n\"\n");

    /* link_flags -- computed by the prelude into ${_tur_lf_json}. */
    fprintf(f, "  \"    \\\"link_flags\\\":   [${_tur_lf_json}]\\n\"\n");
}

/* The constant CMake helper that turns one target's INTERFACE_LINK_LIBRARIES
 * into link-line tokens. Emitted once per generated CMakeLists.
 *
 * This runs at *configure* time, where `if(TARGET ...)` and
 * get_target_property are available -- which is the whole point. A generator
 * expression can read the property but cannot iterate it, and the raw entries
 * cannot be classified by string inspection downstream:
 *
 *   - a bare entry may be a library name (`m` -> -lm) or a CMake target name
 *     (raylib's INTERFACE_LINK_LIBRARIES lists `glfw`). Identical shape,
 *     opposite handling, and guessing "library" emits a `-lglfw` that does not
 *     exist.
 *   - a target may have no linkable artifact at all. raylib vendors glfw as an
 *     OBJECT_LIBRARY whose objects are already inside libraylib.a, so the
 *     correct action is to drop it -- and $<TARGET_FILE:...> on it is a hard
 *     generate-time error ("Target glfw is not an executable or library").
 *   - $<LINK_ONLY:...> survives one level of $<TARGET_PROPERTY:...> expansion
 *     as literal text, so it has to be unwrapped here rather than downstream. */
static void emit_link_iface_helper(FILE *f) {
    fputs(
    "# Resolve one target's INTERFACE_LINK_LIBRARIES into link-line tokens.\n"
    "set(_TUR_LINKABLE STATIC_LIBRARY SHARED_LIBRARY MODULE_LIBRARY\n"
    "                  UNKNOWN_LIBRARY EXECUTABLE)\n"
    "# Recursive: a target with no artifact of its own still carries\n"
    "# requirements. raylib vendors glfw as an OBJECT_LIBRARY -- its objects\n"
    "# are already inside libraylib.a, so it contributes no -l, but the Cocoa\n"
    "# and IOKit frameworks its macOS backend needs live on *its*\n"
    "# INTERFACE_LINK_LIBRARIES and are reachable no other way. `_seen` guards\n"
    "# against link cycles.\n"
    "function(_tur_walk_iface tgt acc_var seen_var)\n"
    "  set(_acc \"${${acc_var}}\")\n"
    "  set(_seen \"${${seen_var}}\")\n"
    "  if(NOT \"${tgt}\" IN_LIST _seen)\n"
    "    list(APPEND _seen \"${tgt}\")\n"
    "    if(TARGET \"${tgt}\")\n"
    "      get_target_property(_ill \"${tgt}\" INTERFACE_LINK_LIBRARIES)\n"
    "      if(_ill)\n"
    "        foreach(_e IN LISTS _ill)\n"
    "          string(REGEX REPLACE \"^\\\\$<LINK_ONLY:(.*)>$\" \"\\\\1\" _e \"${_e}\")\n"
    "          if(NOT _e MATCHES \"^\\\\$<\")\n"
    "            if(TARGET \"${_e}\")\n"
    "              get_target_property(_ty \"${_e}\" TYPE)\n"
    "              if(_ty IN_LIST _TUR_LINKABLE)\n"
    "                list(APPEND _acc \"$<TARGET_FILE:${_e}>\")\n"
    "                if(_ty STREQUAL SHARED_LIBRARY)\n"
    "                  list(APPEND _acc \"-Wl,-rpath,$<TARGET_FILE_DIR:${_e}>\")\n"
    "                endif()\n"
    "              else()\n"
    "                set(_a \"${_acc}\")\n"
    "                set(_s \"${_seen}\")\n"
    "                _tur_walk_iface(\"${_e}\" _a _s)\n"
    "                set(_acc \"${_a}\")\n"
    "                set(_seen \"${_s}\")\n"
    "              endif()\n"
    "            else()\n"
    "              list(APPEND _acc \"${_e}\")\n"
    "            endif()\n"
    "          endif()\n"
    "        endforeach()\n"
    "      endif()\n"
    "    endif()\n"
    "  endif()\n"
    "  set(${acc_var} \"${_acc}\" PARENT_SCOPE)\n"
    "  set(${seen_var} \"${_seen}\" PARENT_SCOPE)\n"
    "endfunction()\n"
    "\n"
    "function(_tur_resolve_link_iface tgt out_var)\n"
    "  set(_acc \"\")\n"
    "  set(_seen \"\")\n"
    "  _tur_walk_iface(\"${tgt}\" _acc _seen)\n"
    "  set(${out_var} \"${_acc}\" PARENT_SCOPE)\n"
    "endfunction()\n"
    "\n"
    "# Append one target's own artifact (and an rpath if it is shared).\n"
    "function(_tur_append_artifact tgt out_var)\n"
    "  set(_out ${${out_var}})\n"
    "  if(TARGET \"${tgt}\")\n"
    "    get_target_property(_ty \"${tgt}\" TYPE)\n"
    "    if(_ty IN_LIST _TUR_LINKABLE)\n"
    "      list(APPEND _out \"$<TARGET_FILE:${tgt}>\")\n"
    "      if(_ty STREQUAL SHARED_LIBRARY)\n"
    "        list(APPEND _out \"-Wl,-rpath,$<TARGET_FILE_DIR:${tgt}>\")\n"
    "      endif()\n"
    "    endif()\n"
    "  endif()\n"
    "  set(${out_var} \"${_out}\" PARENT_SCOPE)\n"
    "endfunction()\n\n", f);
}

/* Emit the CMake that computes ${_tur_lf_json} for one dep: the JSON body of
 * its "link_flags" array. Must precede the string(APPEND _spice_manifest ...)
 * block that references it, and must sit inside the same CMake `if` branch,
 * because a :prefer-system dep's targets differ per branch. */
static void emit_link_flags_prelude(FILE *f, const PkgCmakeDep *d,
                                    bool use_full_targets) {
    fprintf(f, "set(_tur_lf \"\")\n");
    /* The dep's own artifacts, unless :link-libs took over the link. */
    if (!d->has_link_libs) {
        for (int j = 0; j < d->n_targets; j++) {
            const char *t = use_full_targets
                                ? d->targets[j]
                                : cmake_target_basename(d->targets[j]);
            fprintf(f, "_tur_append_artifact(\"%s\" _tur_lf)\n", t);
        }
    }
    /* Transitive requirements -- where -framework Cocoa lives. */
    for (int j = 0; j < d->n_targets; j++) {
        const char *t = use_full_targets
                            ? d->targets[j]
                            : cmake_target_basename(d->targets[j]);
        fprintf(f, "_tur_resolve_link_iface(\"%s\" _tur_ill)\n", t);
        fprintf(f, "list(APPEND _tur_lf ${_tur_ill})\n");
    }
    /* The dep's verbatim escape hatch, last. */
    for (int j = 0; j < d->n_link_flags; j++)
        fprintf(f, "list(APPEND _tur_lf \"%s\")\n", d->link_flags[j]);
    /* JSON-ify: "a", "b". Quotes are written into the manifest text, so they
     * are escaped once here and land literal. */
    fprintf(f,
        "set(_tur_lf_json \"\")\n"
        "foreach(_e IN LISTS _tur_lf)\n"
        "  if(NOT _tur_lf_json STREQUAL \"\")\n"
        "    string(APPEND _tur_lf_json \", \")\n"
        "  endif()\n"
        "  string(APPEND _tur_lf_json \"\\\"${_e}\\\"\")\n"
        "endforeach()\n");
}

/* ================================================================== */
/* Transitive :cmake-deps resolution                                   */
/* ================================================================== */

static char *dup_cstr_or_null(const char *s) {
    return s ? tur_strdup(s) : NULL;
}

static bool deep_copy_cmake_dep(PkgCmakeDep *dst, const PkgCmakeDep *src) {
    memset(dst, 0, sizeof(*dst));
    dst->name          = dup_cstr_or_null(src->name);
    dst->url           = dup_cstr_or_null(src->url);
    dst->ref           = dup_cstr_or_null(src->ref);
    dst->path          = dup_cstr_or_null(src->path);
    dst->cmake_name    = dup_cstr_or_null(src->cmake_name);
    dst->cmake_version = dup_cstr_or_null(src->cmake_version);
    dst->prefer_system = src->prefer_system;
    if (src->n_targets > 0) {
        dst->targets   = (char **)calloc((size_t)src->n_targets, sizeof(char *));
        if (!dst->targets) return false;
        for (int i = 0; i < src->n_targets; i++)
            dst->targets[i] = dup_cstr_or_null(src->targets[i]);
        dst->n_targets = src->n_targets;
    }
    dst->has_link_libs = src->has_link_libs;
    if (src->n_link_libs > 0) {
        dst->link_libs = (char **)calloc((size_t)src->n_link_libs, sizeof(char *));
        if (!dst->link_libs) return false;
        for (int i = 0; i < src->n_link_libs; i++)
            dst->link_libs[i] = dup_cstr_or_null(src->link_libs[i]);
        dst->n_link_libs = src->n_link_libs;
    }
    if (src->n_link_flags > 0) {
        dst->link_flags = (char **)calloc((size_t)src->n_link_flags, sizeof(char *));
        if (!dst->link_flags) return false;
        for (int i = 0; i < src->n_link_flags; i++)
            dst->link_flags[i] = dup_cstr_or_null(src->link_flags[i]);
        dst->n_link_flags = src->n_link_flags;
    }
    if (src->n_opts > 0) {
        dst->opts = (PkgCmakeOpt *)calloc((size_t)src->n_opts, sizeof(PkgCmakeOpt));
        if (!dst->opts) return false;
        for (int i = 0; i < src->n_opts; i++) {
            dst->opts[i].key = dup_cstr_or_null(src->opts[i].key);
            dst->opts[i].val = dup_cstr_or_null(src->opts[i].val);
        }
        dst->n_opts = src->n_opts;
    }
    if (src->n_wasm_opts > 0) {
        dst->wasm_opts = (PkgCmakeOpt *)calloc((size_t)src->n_wasm_opts,
                                               sizeof(PkgCmakeOpt));
        if (!dst->wasm_opts) return false;
        for (int i = 0; i < src->n_wasm_opts; i++) {
            dst->wasm_opts[i].key = dup_cstr_or_null(src->wasm_opts[i].key);
            dst->wasm_opts[i].val = dup_cstr_or_null(src->wasm_opts[i].val);
        }
        dst->n_wasm_opts = src->n_wasm_opts;
    }
    return true;
}

static void free_one_cmake_dep(PkgCmakeDep *d) {
    free(d->name);
    free(d->url);
    free(d->ref);
    free(d->path);
    free(d->cmake_name);
    free(d->cmake_version);
    for (int i = 0; i < d->n_targets; i++) free(d->targets[i]);
    free(d->targets);
    for (int i = 0; i < d->n_link_libs; i++) free(d->link_libs[i]);
    free(d->link_libs);
    for (int i = 0; i < d->n_link_flags; i++) free(d->link_flags[i]);
    free(d->link_flags);
    for (int i = 0; i < d->n_opts; i++) {
        free(d->opts[i].key);
        free(d->opts[i].val);
    }
    free(d->opts);
    for (int i = 0; i < d->n_wasm_opts; i++) {
        free(d->wasm_opts[i].key);
        free(d->wasm_opts[i].val);
    }
    free(d->wasm_opts);
    memset(d, 0, sizeof(*d));
}

bool pkg_workspace_wide_cmake_deps(void) {
    const char *e = getenv("TUR_CMAKE_DEPS_WORKSPACE_WIDE");
    return e && *e && strcmp(e, "0") != 0;
}

void pkg_cmake_deps_free(PkgCmakeDep *deps, int n) {
    if (!deps) return;
    for (int i = 0; i < n; i++) free_one_cmake_dep(&deps[i]);
    free(deps);
}

static bool str_eq_or_both_null(const char *a, const char *b) {
    if (a == b) return true;
    if (!a || !b) return false;
    return strcmp(a, b) == 0;
}

/* Resolve a :spices entry to its on-disk directory, mirroring the
 * priority used in main.c's resolve_include_dirs_from_manifest:
 *   workspace-sibling -> :path -> <root>/spices/<name>-<ref> -> <root>/spices/<name>
 * Returns a heap-allocated absolute (or root-relative) path.  Returns
 * NULL when the resolved directory does not exist (best-effort silent
 * skip, matching pkg_fetch_all's "continuing with healthy deps" policy).
 * Caller frees. */
char *pkg_resolve_spice_dep_dir(const char *root_project_dir,
                                const PkgSpice *s) {
    if (!root_project_dir || !s || !s->name) return NULL;
    char dep_dir[4096];
    /* global-spice-library-consumption: an explicitly `:global` dep resolves
     * from the install registry and nowhere else -- falling back to a
     * project-local guess would silently use a different spice than the one
     * the manifest asked for. */
    if (s->is_global) {
        if (!tur_installed_spice_dir(s->name, dep_dir, sizeof(dep_dir), NULL, NULL))
            return NULL;
        return tur_strdup(dep_dir);
    }
    char *ws = s->path ? NULL
                       : pkg_workspace_member_path(root_project_dir, s->name);
    bool from_path = false;
    if (ws) {
        snprintf(dep_dir, sizeof(dep_dir), "%s", ws);
        free(ws);
    } else if (s->path) {
        snprintf(dep_dir, sizeof(dep_dir), "%s/%s", root_project_dir, s->path);
        from_path = true;
    } else if (s->ref) {
        snprintf(dep_dir, sizeof(dep_dir), "%s/spices/%s-%s",
                 root_project_dir, s->name, s->ref);
    } else {
        snprintf(dep_dir, sizeof(dep_dir), "%s/spices/%s",
                 root_project_dir, s->name);
    }
    /* `:subdir` describes the sub-path inside a URL-fetched repo (e.g. a
     * monorepo's `spices/<name>`).  When the entry is `:path`-based the
     * path already points at the on-disk package root, so appending the
     * subdir lands somewhere that does not exist (the same shape bit
     * resolve_include_dirs_from_manifest had to grow ancestor-walking to
     * work around).  Only join when we did NOT come from `:path`. */
    if (s->subdir && !from_path) {
        char tmp[4096];
        snprintf(tmp, sizeof(tmp), "%s/%s", dep_dir, s->subdir);
        snprintf(dep_dir, sizeof(dep_dir), "%s", tmp);
    }
    struct stat ss;
    if (stat(dep_dir, &ss) != 0 || !S_ISDIR(ss.st_mode)) return NULL;
    return tur_strdup(dep_dir);
}

/* Append `dep` (deep-copied) into *out_deps, applying conflict-detection
 * keyed on `dep->name`.  `origin_dir` is reported in conflict diagnostics
 * (typically the manifest directory the dep was read from). */
static bool append_cmake_dep_with_conflict_check(PkgCmakeDep **out_deps,
                                                  int *out_n,
                                                  int *out_cap,
                                                  const PkgCmakeDep *dep,
                                                  const char *origin_dir,
                                                  const char **origins) {
    for (int i = 0; i < *out_n; i++) {
        if (!str_eq_or_both_null((*out_deps)[i].name, dep->name)) continue;
        /* Same name -- require :url and :ref to match (path-form deps
         * are compared by :path).  Mismatched :cmake-name or :options
         * is silently kept on the existing copy. */
        bool url_match  = str_eq_or_both_null((*out_deps)[i].url,  dep->url);
        bool ref_match  = str_eq_or_both_null((*out_deps)[i].ref,  dep->ref);
        bool path_match = str_eq_or_both_null((*out_deps)[i].path, dep->path);
        if (url_match && ref_match && path_match) return true; /* dup */
        fprintf(stderr,
            "tur: conflicting :cmake-deps for \"%s\":\n"
            "  %s: url=%s, ref=%s, path=%s\n"
            "  %s: url=%s, ref=%s, path=%s\n",
            dep->name,
            origins[i] ? origins[i] : "<unknown>",
            (*out_deps)[i].url  ? (*out_deps)[i].url  : "(none)",
            (*out_deps)[i].ref  ? (*out_deps)[i].ref  : "(none)",
            (*out_deps)[i].path ? (*out_deps)[i].path : "(none)",
            origin_dir ? origin_dir : "<unknown>",
            dep->url  ? dep->url  : "(none)",
            dep->ref  ? dep->ref  : "(none)",
            dep->path ? dep->path : "(none)");
        return false;
    }
    if (*out_n >= *out_cap) {
        int new_cap = *out_cap ? *out_cap * 2 : 4;
        PkgCmakeDep *nd = (PkgCmakeDep *)realloc(
            *out_deps, (size_t)new_cap * sizeof(PkgCmakeDep));
        if (!nd) return false;
        *out_deps = nd;
        *out_cap  = new_cap;
        /* parallel origins[] array grows in the caller */
    }
    if (!deep_copy_cmake_dep(&(*out_deps)[*out_n], dep)) return false;
    /* Absolutize :path-form deps so a sibling's relative path still resolves
     * from here.
     *
     * The base is `<origin_dir>/cmake`, NOT `origin_dir`. The generated
     * CMakeLists lives in `<spice>/cmake/`, so a relative add_subdirectory
     * argument resolves from there, and every spice writes its `:path`
     * accordingly -- `"../cmake-deps/raygui"` means
     * `<spice>/cmake/../cmake-deps/raygui`, i.e. `<spice>/cmake-deps/raygui`.
     * Joining against `origin_dir` instead lands one directory too high
     * (`spices/cmake-deps/raygui`), which does not exist.
     *
     * The result is always absolute, which is what lets cmake_dep_path_dir
     * stay a pure passthrough: `origin_dir` is itself relative under
     * `tur fetch` (it is "."), so joining alone would leave a relative path
     * that the emitter would then join against project_dir a second time,
     * inserting `cmake/` twice. realpath() settles both the absoluteness and
     * the `..` normalization; if the directory does not exist there is
     * nothing to resolve, so the unnormalized join is kept and CMake reports
     * it verbatim, which is the more useful error. */
    if ((*out_deps)[*out_n].path && (*out_deps)[*out_n].path[0] != '/'
        && origin_dir) {
        char joined[4096];
        char cwd[4096];
        if (origin_dir[0] == '/') {
            snprintf(joined, sizeof(joined), "%s/cmake/%s",
                     origin_dir, (*out_deps)[*out_n].path);
        } else if (getcwd(cwd, sizeof(cwd))) {
            /* origin_dir is "." under `tur fetch`, which is relative to the
             * process cwd -- anchor it there rather than leaving a relative
             * result the emitter would join a second time. */
            snprintf(joined, sizeof(joined), "%s/%s/cmake/%s",
                     cwd, origin_dir, (*out_deps)[*out_n].path);
        } else {
            snprintf(joined, sizeof(joined), "%s/cmake/%s",
                     origin_dir, (*out_deps)[*out_n].path);
        }
        char resolved[4096];
        const char *final = realpath(joined, resolved) ? resolved : joined;
        free((*out_deps)[*out_n].path);
        (*out_deps)[*out_n].path = tur_strdup(final);
    }
    (*out_n)++;
    return true;
}

/* Discover the on-disk directories of the *other* members of the workspace
 * enclosing `project_dir`, if any.  Walks parents looking for a `build.tur`
 * that declares `:members` containing `project_dir` itself, then returns the
 * absolute path of every sibling member (excluding `project_dir`).
 *
 * This mirrors the workspace-sibling `src/` resolution in main.c's
 * auto_append_spice_includes: any sibling member's modules are importable
 * from `project_dir` without an explicit `:spices` entry, so any sibling's
 * native (`:cmake-deps`) contributions must participate in the build too.
 *
 * Returns a heap-allocated array of `char *` (caller frees each entry and the
 * array) and sets *out_n; returns NULL with *out_n = 0 when `project_dir` is
 * not part of any workspace. */
/* WP2 (D-9): one `:options` pair on its way into a generated
 * `set(<key> "<val>" CACHE <type> "" FORCE)` line.  The key is unquotable (it is
 * a cmake variable name) and the value is a bare token, so both are
 * narrow; a pair that fails is skipped with a diagnostic rather than written,
 * because a cmake dep configured without one of its options fails visibly
 * while a `$\{...}` in a value does not. */
void pkg_web_opts_defaults(PkgWebOpts *w) {
    if (!w) return;
    memset(w, 0, sizeof(*w));
    /* ES3/WebGL2 is the floor.  ES2 gives a WebGL1 context, whose NPOT
     * restriction ("no-mipmaps, no-repeat" on any texture not a power of two)
     * is a real constraint on a real game; WebGL2 has been in every evergreen
     * browser for years. `:gl :es2` remains available for old mobile. */
    w->gl        = PKG_WEB_GL_ES3;
    /* 128 MB, matching raylib's own BUILD_WEB_HEAP_SIZE default.  A fixed heap
     * beats -sALLOW_MEMORY_GROWTH when the working set is known: growth
     * invalidates cached heap views and copies at every grow, which is a
     * frame-time spike exactly when a level loads.  `:heap 0` opts into
     * growth for a program whose working set is not known at link time. */
    w->heap      = 134217728L;
    w->threads   = false;   /* -pthread needs COOP/COEP, which GitHub Pages
                             * cannot serve, and a droppable static site is
                             * the point of this target. */
    w->main_loop = NULL;    /* NULL reads as "callback" */
}

void pkg_web_opts_free(PkgWebOpts *w) {
    if (!w) return;
    free(w->main_loop);
    w->main_loop = NULL;
}

void pkg_web_compose_link_flags(const PkgWebOpts *w, Buf *out) {
    if (!w || !out) return;

    /* Kept alive across main() returning: in callback mode the frame callback
     * fires after main has unwound, and with the default the runtime would
     * tear down first and the callback fire into a dead module. */
    buf_printf(out, " -sEXIT_RUNTIME=0 -sMODULARIZE=1 -sEXPORT_NAME=TurmericApp");

    if (w->heap > 0)
        buf_printf(out, " -sINITIAL_MEMORY=%ld", w->heap);
    else
        buf_printf(out, " -sALLOW_MEMORY_GROWTH=1");

    if (w->canvas) {
        /* raylib and the opengl spice both reach the canvas through the
         * Emscripten GLFW port. GL_ENABLE_GET_PROC_ADDRESS is insurance: a
         * minimal program runs without it, but a raylib build that resolves
         * GL entry points through glGetProcAddress dies at InitWindow(). */
        buf_printf(out, " -sUSE_GLFW=3 -sGL_ENABLE_GET_PROC_ADDRESS");
        if (w->gl == PKG_WEB_GL_ES3)
            buf_printf(out, " -sMAX_WEBGL_VERSION=2");
    }

    if (w->audio) {
        /* ccall per upstream; HEAPF32 because miniaudio -- what raylib's
         * raudio is built on -- reads the heap as `Module.HEAPF32.buffer`
         * from inside its ScriptProcessorNode callback, and Emscripten no
         * longer hangs heap views off Module by default.  Naming it is also
         * what makes updateMemoryViews() re-publish the view after a heap
         * grow, so it matters doubly under `:heap 0`.  Omitting it is a
         * TypeError thrown out of the audio callback -- and only once the
         * AudioContext is actually running, so it hides until someone
         * gestures to unlock audio. */
        buf_printf(out, " -sEXPORTED_RUNTIME_METHODS=ccall,HEAPF32");
    }

    if (w->threads)
        buf_printf(out, " -pthread -sPTHREAD_POOL_SIZE_STRICT=0");

    /* Asyncify lets an existing blocking `while (not (window-should-close))`
     * loop yield to the browser with no source change.  The cost is a fixed
     * ~16ms wait ADDED to frame time (measured: 59.5fps at ~0ms of frame work,
     * 40.0fps at 8ms, against a flat 60.0 in callback mode), so it is a
     * porting mode for a game with frame time to spare, not the default. */
    if (w->main_loop && strcmp(w->main_loop, "asyncify") == 0)
        buf_printf(out, " -sASYNCIFY");
}

bool pkg_web_check_gl_agreement(const PkgWebOpts *w,
                                const PkgCmakeDep *deps, int n_deps) {
    if (!w || !deps) return true;
    for (int i = 0; i < n_deps; i++) {
        const PkgCmakeDep *d = &deps[i];
        for (int j = 0; j < d->n_wasm_opts; j++) {
            const char *val = d->wasm_opts[j].val;
            if (!val) continue;
            /* Only the two spellings that actually pin a level are checked;
             * anything else in :wasm-options is none of this function's
             * business. */
            bool dep_es3 = strstr(val, "OPENGL_ES3") != NULL;
            bool dep_es2 = strstr(val, "OPENGL_ES2") != NULL;
            if (!dep_es3 && !dep_es2) continue;
            bool web_es3 = (w->gl == PKG_WEB_GL_ES3);
            if (dep_es3 == web_es3) continue;
            fprintf(stderr,
                "build.tur: cmake-dep '%s' :wasm-options sets %s \"%s\", which "
                "needs a WebGL%d context, but :web :gl is :%s (WebGL%d).\n"
                "  These are set by different tools and neither checks the "
                "other, so a mismatch links fine and then dies in the browser "
                "on `attachShader ... parameter 2 is not of type "
                "'WebGLShader'`.\n"
                "  Set :web :gl to :%s, or change the dep's option.\n",
                d->name, d->wasm_opts[j].key, val,
                dep_es3 ? 2 : 1,
                web_es3 ? "es3" : "es2", web_es3 ? 2 : 1,
                dep_es3 ? "es3" : "es2");
            return false;
        }
    }
    return true;
}

/* The cache type to declare an option with.  Every `:options` value in the
 * spice tree today is ON/OFF, which is why this was hardcoded `BOOL`; but a
 * dep's web backend is typically selected by a STRING (raylib's
 * `PLATFORM`/`GRAPHICS`), and declaring that BOOL both mistypes the cache
 * entry and fights the dep's own `set(... CACHE STRING)`.  Booleans keep BOOL
 * so nothing in the existing tree changes shape. */
static const char *cmake_opt_cache_type(const char *val) {
    if (!val) return "STRING";
    static const char *bools[] = {
        "ON", "OFF", "TRUE", "FALSE", "YES", "NO", "Y", "N", "1", "0"
    };
    for (size_t i = 0; i < sizeof(bools) / sizeof(bools[0]); i++) {
        const char *b = bools[i];
        size_t n = strlen(b);
        if (strlen(val) != n) continue;
        size_t k = 0;
        for (; k < n; k++) {
            char a = val[k], c = b[k];
            if (a >= 'a' && a <= 'z') a = (char)(a - 'a' + 'A');
            if (a != c) break;
        }
        if (k == n) return "BOOL";
    }
    return "STRING";
}

static bool cmake_opt_ok(const PkgCmakeDep *d, const PkgCmakeOpt *o);

/* Emit the `set(<K> "<V>" CACHE <T> "" FORCE)` lines for one option list.
 * `indent` matches the surrounding block; `wasm_only` wraps the lines in
 * `if(EMSCRIPTEN)` so the same generated CMakeLists serves both arms. */
static void emit_cmake_opts(FILE *f, const PkgCmakeDep *d,
                            const PkgCmakeOpt *opts, int n,
                            const char *indent, bool wasm_only) {
    if (n <= 0) return;
    /* Count what will actually be written: an all-skipped list must not emit
     * an empty `if(EMSCRIPTEN)/endif()` pair. */
    int live = 0;
    for (int j = 0; j < n; j++) if (cmake_opt_ok(d, &opts[j])) live++;
    if (live == 0) return;
    if (wasm_only) fprintf(f, "%sif (EMSCRIPTEN)\n", indent);
    const char *in2 = wasm_only ? "    " : "";
    for (int j = 0; j < n; j++) {
        if (!cmake_opt_ok(d, &opts[j])) continue;
        fprintf(f, "%s%sset(%s \"%s\" CACHE %s \"\" FORCE)\n",
                indent, in2, opts[j].key, opts[j].val,
                cmake_opt_cache_type(opts[j].val));
    }
    if (wasm_only) fprintf(f, "%sendif()\n", indent);
}

static bool cmake_opt_ok(const PkgCmakeDep *d, const PkgCmakeOpt *o) {
    if (!tur_cmake_ident_ok(o->key)) {
        fprintf(stderr, "spice: cmake-dep '%s': skipping :options key '%s' -- "
                        "not a cmake identifier\n", d->name, o->key ? o->key : "");
        return false;
    }
    if (!tur_cmake_value_ok(o->val)) {
        fprintf(stderr, "spice: cmake-dep '%s': skipping :options value for "
                        "'%s' -- '%s' contains a character cmake would read as "
                        "syntax\n", d->name, o->key, o->val ? o->val : "");
        return false;
    }
    return true;
}

static char **collect_workspace_sibling_dirs(const char *project_dir,
                                             int *out_n) {
    *out_n = 0;
    if (!project_dir) return NULL;

    char real_root[4096];
    if (realpath(project_dir, real_root) == NULL) return NULL;

    char anc[4096];
    size_t rlen = strlen(real_root);
    if (rlen + 1 > sizeof(anc)) return NULL;
    memcpy(anc, real_root, rlen + 1);

    for (int up = 0; up < PKG_WORKSPACE_WALK_MAX; up++) {
        char *last = strrchr(anc, '/');
        if (!last || last == anc) break;
        *last = '\0';

        char ws_manifest[4096];
        if (!pkg_resolve_manifest_path(anc, ws_manifest, sizeof(ws_manifest)))
            continue;

        PkgManifest wm;
        memset(&wm, 0, sizeof(wm));
        if (!pkg_manifest_read(ws_manifest, &wm)) continue;

        if (wm.n_members <= 0) {
            /* Any build.tur (workspace or not) terminates the walk. */
            pkg_manifest_free(&wm);
            break;
        }

        /* Confirm project_dir is itself a listed member of this candidate
         * workspace before treating its siblings as contributors. */
        const char *self_member = NULL;
        for (int i = 0; i < wm.n_members; i++) {
            char mp[4096];
            int mn = snprintf(mp, sizeof(mp), "%s/%s", anc, wm.members[i]);
            if (mn <= 0 || (size_t)mn >= sizeof(mp)) continue;
            char real_mp[4096];
            if (realpath(mp, real_mp) == NULL) continue;
            if (strcmp(real_mp, real_root) == 0) {
                self_member = wm.members[i];
                break;
            }
        }
        if (!self_member) {
            pkg_manifest_free(&wm);
            break;
        }

        char **dirs = (char **)calloc((size_t)wm.n_members, sizeof(char *));
        if (!dirs) { pkg_manifest_free(&wm); return NULL; }
        int n = 0;
        for (int i = 0; i < wm.n_members; i++) {
            if (strcmp(wm.members[i], self_member) == 0) continue;
            char member_dir[4096];
            int mn = snprintf(member_dir, sizeof(member_dir),
                              "%s/%s", anc, wm.members[i]);
            if (mn <= 0 || (size_t)mn >= sizeof(member_dir)) continue;
            struct stat ss;
            if (stat(member_dir, &ss) != 0 || !S_ISDIR(ss.st_mode)) continue;
            dirs[n++] = tur_strdup(member_dir);
        }
        pkg_manifest_free(&wm);
        if (n == 0) { free(dirs); return NULL; }
        *out_n = n;
        return dirs;
    }
    return NULL;
}

bool pkg_collect_transitive_cmake_deps(const char        *root_project_dir,
                                       const PkgManifest *root_manifest,
                                       bool               include_workspace_siblings,
                                       PkgCmakeDep      **out_deps,
                                       int               *out_n) {
    *out_deps = NULL;
    *out_n    = 0;
    if (!root_project_dir || !root_manifest) return true;

    PkgCmakeDep *deps     = NULL;
    int          n_deps   = 0;
    int          cap_deps = 0;
    const char **origins  = NULL;
    int          n_origins = 0;
    int          cap_origins = 0;

#define GROW_ORIGINS_TO(n_target) do {                                       \
    if ((n_target) > cap_origins) {                                          \
        int nc = cap_origins ? cap_origins * 2 : 4;                          \
        while (nc < (n_target)) nc *= 2;                                     \
        const char **no = (const char **)realloc(                            \
            origins, (size_t)nc * sizeof(char *));                           \
        if (!no) goto fail;                                                  \
        origins = no;                                                        \
        cap_origins = nc;                                                    \
    }                                                                        \
} while (0)

    /* Root manifest's own deps go first. */
    for (int i = 0; i < root_manifest->n_cmake_deps; i++) {
        GROW_ORIGINS_TO(n_origins + 1);
        if (!append_cmake_dep_with_conflict_check(
                &deps, &n_deps, &cap_deps,
                &root_manifest->cmake_deps[i],
                root_project_dir, origins))
            goto fail;
        origins[n_origins++] = root_project_dir;
    }

    /* Multi-level walk over :spices entries.  The worklist holds
     * absolute-real-path directories of spices whose `build.tur` we
     * still need to inspect; the visited set is keyed on those same
     * paths so cycles and diamond shapes terminate.  Each visit
     * unions the spice's :cmake-deps and appends its own :spices to
     * the worklist. */
    char **worklist  = NULL;
    int    n_work    = 0;
    int    cap_work  = 0;
    char **visited   = NULL;
    int    n_visited = 0;
    int    cap_vis   = 0;

#define GROW_WORK_TO(n_target) do {                                         \
    if ((n_target) > cap_work) {                                            \
        int nc = cap_work ? cap_work * 2 : 4;                               \
        while (nc < (n_target)) nc *= 2;                                    \
        char **nw = (char **)realloc(worklist, (size_t)nc * sizeof(char *));\
        if (!nw) goto walk_fail;                                            \
        worklist = nw;                                                      \
        cap_work = nc;                                                      \
    }                                                                       \
} while (0)
#define GROW_VIS_TO(n_target) do {                                          \
    if ((n_target) > cap_vis) {                                             \
        int nc = cap_vis ? cap_vis * 2 : 4;                                 \
        while (nc < (n_target)) nc *= 2;                                    \
        char **nv = (char **)realloc(visited, (size_t)nc * sizeof(char *)); \
        if (!nv) goto walk_fail;                                            \
        visited = nv;                                                       \
        cap_vis = nc;                                                       \
    }                                                                       \
} while (0)

    /* Helper: canonical key for the visited set.  realpath() if the dir
     * exists; otherwise the input. */
    /* Seed: the root's own :spices entries. */
    for (int i = 0; i < root_manifest->n_spices; i++) {
        char *sib_dir = pkg_resolve_spice_dep_dir(root_project_dir,
                                              &root_manifest->spices[i]);
        if (!sib_dir) continue;
        GROW_WORK_TO(n_work + 1);
        worklist[n_work++] = sib_dir;
    }

    /* Seed: the enclosing workspace's *other* members.  A sibling member's
     * modules are importable from the root spice without an explicit :spices
     * entry (see auto_append_spice_includes in main.c), so its :cmake-deps
     * must participate in the build too.  Each seeded dir is walked exactly
     * like a :spices entry (its build.tur read, :cmake-deps unioned, its own
     * :spices enqueued); the visited set dedups any overlap with the :spices
     * seeds above.
     *
     * Skipped when `include_workspace_siblings` is false (the `tur build .`
     * caller), since `tur build` does not transparently link in workspace
     * siblings the way `tur run` does -- it sticks to the declared :spices
     * closure.  See docs/archive/history/tur-build-cmake-deps-workspace-overreach.md. */
    if (include_workspace_siblings) {
        int    n_sib   = 0;
        char **sib_dirs = collect_workspace_sibling_dirs(root_project_dir,
                                                         &n_sib);
        for (int i = 0; i < n_sib; i++) {
            GROW_WORK_TO(n_work + 1);
            worklist[n_work++] = sib_dirs[i];  /* ownership moves to worklist */
        }
        free(sib_dirs);
    }

    while (n_work > 0) {
        char *sib_dir = worklist[--n_work];

        /* Visited check (canonical key via realpath when available). */
        char canon_buf[4096];
        const char *canon = sib_dir;
        if (realpath(sib_dir, canon_buf)) canon = canon_buf;
        bool seen = false;
        for (int v = 0; v < n_visited; v++) {
            if (strcmp(visited[v], canon) == 0) { seen = true; break; }
        }
        if (seen) { free(sib_dir); continue; }
        GROW_VIS_TO(n_visited + 1);
        visited[n_visited++] = tur_strdup(canon);

        char sib_manifest[4096];
        if (!pkg_resolve_manifest_path(sib_dir, sib_manifest,
                                       sizeof(sib_manifest))) {
            free(sib_dir);
            continue;
        }
        PkgManifest sm;
        memset(&sm, 0, sizeof(sm));
        if (!pkg_manifest_read(sib_manifest, &sm)) {
            free(sib_dir);
            continue;
        }

        for (int j = 0; j < sm.n_cmake_deps; j++) {
            GROW_ORIGINS_TO(n_origins + 1);
            if (!append_cmake_dep_with_conflict_check(
                    &deps, &n_deps, &cap_deps,
                    &sm.cmake_deps[j], sib_dir, origins)) {
                pkg_manifest_free(&sm);
                free(sib_dir);
                goto walk_fail;
            }
            origins[n_origins++] = tur_strdup(sib_dir);
        }

        /* Enqueue this sibling's own :spices for the next level. */
        for (int j = 0; j < sm.n_spices; j++) {
            char *grand_dir = pkg_resolve_spice_dep_dir(sib_dir, &sm.spices[j]);
            if (!grand_dir) continue;
            GROW_WORK_TO(n_work + 1);
            worklist[n_work++] = grand_dir;
        }

        pkg_manifest_free(&sm);
        free(sib_dir);
    }

    for (int v = 0; v < n_visited; v++) free(visited[v]);
    free(visited);
    free(worklist);
#undef GROW_WORK_TO
#undef GROW_VIS_TO

    /* origins[] entries i >= root_manifest->n_cmake_deps are
     * heap-allocated copies; free them now that conflict diagnostics
     * have been emitted (or not). */
    for (int i = root_manifest->n_cmake_deps; i < n_origins; i++)
        free((char *)origins[i]);
    free(origins);

#undef GROW_ORIGINS_TO

    *out_deps = deps;
    *out_n    = n_deps;
    return true;

fail:
    for (int i = root_manifest->n_cmake_deps; i < n_origins; i++)
        free((char *)origins[i]);
    free(origins);
    pkg_cmake_deps_free(deps, n_deps);
    return false;

walk_fail:
    for (int i = 0; i < n_work; i++) free(worklist[i]);
    free(worklist);
    for (int v = 0; v < n_visited; v++) free(visited[v]);
    free(visited);
    for (int i = root_manifest->n_cmake_deps; i < n_origins; i++)
        free((char *)origins[i]);
    free(origins);
    pkg_cmake_deps_free(deps, n_deps);
    return false;
}

bool pkg_gen_cmake_deps(const char *project_dir,
                        const PkgManifest *manifest) {
    if (manifest->n_cmake_deps == 0) return true;

    char cmake_dir[4096];
    snprintf(cmake_dir, sizeof(cmake_dir), "%s/cmake", project_dir);
    if (!mkdirp(cmake_dir)) {
        fprintf(stderr, "spice: cannot create '%s'\n", cmake_dir);
        return false;
    }

    /* Write cmake/CMakeLists.txt (the entrypoint for cmake -S cmake/) */
    char out_path[4096];
    snprintf(out_path, sizeof(out_path), "%s/CMakeLists.txt", cmake_dir);
    FILE *f = fopen(out_path, "w");
    if (!f) {
        fprintf(stderr, "spice: cannot write '%s': %s\n",
                out_path, strerror(errno));
        return false;
    }

    fprintf(f, "# cmake/CMakeLists.txt -- AUTO-GENERATED by `tur fetch`. Do not edit.\n");
    fprintf(f, "cmake_minimum_required(VERSION 3.20)\n");
    fprintf(f, "project(SpiceDeps)\n\n");
    fprintf(f, "include(FetchContent)\n\n");
    /* A static dep is linked into the spice's `tur build --shared` image as
     * well as into executables -- the image `tur repl` and a notebook cell
     * (turi_env_attach_spice) load -- and an archive built without -fPIC
     * cannot be: plot's image failed on plutovg's ("relocation R_X86_64_TPOFF32
     * ... recompile with -fPIC").  A cache default, so a dep's :options can
     * still turn it off. */
    fprintf(f, "set(CMAKE_POSITION_INDEPENDENT_CODE ON CACHE BOOL "
               "\"Static deps link into tur build --shared images too\")\n\n");
    /* SF3: when set, :prefer-system deps skip find_package and always fetch
     * from source. `tur fetch --refetch` passes -DTUR_FETCH_FORCE_FETCH=ON. */
    fprintf(f, "option(TUR_FETCH_FORCE_FETCH "
               "\"Bypass system find_package for :prefer-system deps\" OFF)\n\n");

    emit_link_iface_helper(f);

    /* Declare and make available each dep */
    for (int i = 0; i < manifest->n_cmake_deps; i++) {
        const PkgCmakeDep *d = &manifest->cmake_deps[i];
        fprintf(f, "# %s\n", d->name);
        if (d->path) {
            /* Local path dep -- use add_subdirectory with absolute path */
            char abs_path[4096];
            cmake_dep_path_dir(d, project_dir, abs_path, sizeof(abs_path));
            char build_subdir[4096];
            snprintf(build_subdir, sizeof(build_subdir),
                     "${CMAKE_BINARY_DIR}/_local/%s-build", d->name);
            /* Options have to be in the cache before the subdirectory is
             * added, which is the only point at which the dep reads them.
             * This branch used to emit none at all, so a `:path` dep silently
             * ignored every option it declared -- including the
             * `:wasm-options` a local raylib checkout needs to pick its web
             * backend. */
            emit_cmake_opts(f, d, d->opts, d->n_opts, "", false);
            emit_cmake_opts(f, d, d->wasm_opts, d->n_wasm_opts, "", true);
            fprintf(f, "add_subdirectory(\"%s\" \"%s\")\n", abs_path,
                    build_subdir);
            fprintf(f, "set(_%s_resolved_via \"path\" CACHE INTERNAL \"\")\n\n",
                    d->name);
        } else if (d->prefer_system) {
            /* System-first: try find_package, fall back to FetchContent.
             * The fallback block is emitted verbatim inside the
             * `if (NOT <cmake-name>_FOUND)` guard so a system copy short-
             * circuits the clone + source build entirely. */
            const char *cn = d->cmake_name; /* guaranteed non-NULL (SF0) */
            /* Options go in the cache BEFORE find_package, not only in the
             * fallback: a find module reads its hints from there.  Homebrew
             * keeps libpq keg-only, so FindPostgreSQL finds nothing on macOS
             * unless `PostgreSQL_ROOT` points at the keg -- and written only
             * in the fallback, that hint arrived after the search it was for.
             * The fallback still sees them: they are cache entries. */
            emit_cmake_opts(f, d, d->opts, d->n_opts, "", false);
            emit_cmake_opts(f, d, d->wasm_opts, d->n_wasm_opts, "", true);
            /* No :url means system-only (libpq has no repo to build from), so
             * there is nothing for --refetch to prefer over the system copy. */
            if (d->url) fprintf(f, "if (NOT TUR_FETCH_FORCE_FETCH)\n");
            const char *in = d->url ? "    " : "";
            if (d->cmake_version)
                fprintf(f, "%sfind_package(%s %s QUIET)\n", in, cn,
                        d->cmake_version);
            else
                fprintf(f, "%sfind_package(%s QUIET)\n", in, cn);
            if (d->url) fprintf(f, "endif()\n");
            fprintf(f, "if (NOT %s_FOUND)\n", cn);
            if (d->url) {
                fprintf(f, "    FetchContent_Declare(%s\n", d->name);
                fprintf(f, "      GIT_REPOSITORY \"%s\"\n", d->url);
                if (d->ref) fprintf(f, "      GIT_TAG        \"%s\"\n", d->ref);
                fprintf(f, "    )\n");
                fprintf(f, "    FetchContent_MakeAvailable(%s)\n", d->name);
                fprintf(f, "    set(_%s_resolved_via \"fetch\" CACHE INTERNAL \"\")\n",
                        d->name);
            } else {
                /* An empty FetchContent_Declare used to stand here, and the
                 * error it produced ("No download info given for
                 * '<name>-populate'") named neither the package nor the fact
                 * that it was looked for on the system. */
                fprintf(f, "    message(FATAL_ERROR \"spice: cmake-dep '%s': "
                           "find_package(%s) found no system copy, and the dep "
                           "declares no :url to build one from. Install it with "
                           "the system package manager, or set the %s_ROOT "
                           "environment variable to its prefix.\")\n",
                        d->name, cn, cn);
            }
            fprintf(f, "else()\n");
            fprintf(f, "    set(_%s_resolved_via \"system\" CACHE INTERNAL \"\")\n",
                    d->name);
            fprintf(f, "    message(STATUS \"spice: %s resolved via system "
                       "find_package(%s)\")\n", d->name, cn);
            fprintf(f, "endif()\n\n");
        } else {
            fprintf(f, "FetchContent_Declare(%s\n", d->name);
            if (d->url) fprintf(f, "  GIT_REPOSITORY \"%s\"\n", d->url);
            if (d->ref) fprintf(f, "  GIT_TAG        \"%s\"\n", d->ref);
            fprintf(f, ")\n");
            emit_cmake_opts(f, d, d->opts, d->n_opts, "", false);
            emit_cmake_opts(f, d, d->wasm_opts, d->n_wasm_opts, "", true);
            fprintf(f, "FetchContent_MakeAvailable(%s)\n", d->name);
            fprintf(f, "set(_%s_resolved_via \"fetch\" CACHE INTERNAL \"\")\n\n",
                    d->name);
        }
    }

    /* Generate spice-deps-manifest.json at cmake configure time.
     *
     * The filename is arm-dependent, and CMake picks it rather than tur: the
     * generated CMakeLists is shared by both arms (see the `if(EMSCRIPTEN)`
     * option blocks above), and the two arms resolve every dep to different
     * -I/-L/-l paths.  With one shared filename a wasm configure silently
     * overwrote the native manifest, so the next native link picked up wasm
     * library paths. */
    fprintf(f, "# --- Generate spice-deps-manifest.json ---\n");
    fprintf(f, "if (EMSCRIPTEN)\n");
    fprintf(f, "    set(_spice_manifest_name \"%s\")\n",
            PKG_CMAKE_MANIFEST_WASM);
    fprintf(f, "else()\n");
    fprintf(f, "    set(_spice_manifest_name \"%s\")\n",
            PKG_CMAKE_MANIFEST_NATIVE);
    fprintf(f, "endif()\n");
    fprintf(f, "set(_spice_manifest_path "
               "\"${CMAKE_CURRENT_SOURCE_DIR}/${_spice_manifest_name}\")\n");
    fprintf(f, "set(_spice_manifest \"{\\n\")\n");
    fprintf(f, "set(_spice_first TRUE)\n\n");

    for (int i = 0; i < manifest->n_cmake_deps; i++) {
        const PkgCmakeDep *d = &manifest->cmake_deps[i];
        const char *link_lib = cmake_dep_link_lib(d);

        fprintf(f, "# manifest entry: %s\n", d->name);
        fprintf(f, "if(NOT _spice_first)\n");
        fprintf(f, "  string(APPEND _spice_manifest \",\\n\")\n");
        fprintf(f, "endif()\n");
        fprintf(f, "set(_spice_first FALSE)\n");

        if (d->path) {
            /* Local path dep: derive paths from the path field */
            char abs_path[4096];
            cmake_dep_path_dir(d, project_dir, abs_path, sizeof(abs_path));
            fprintf(f, "set(_spice_%s_inc \"%s/include\")\n",
                    d->name, abs_path);
            fprintf(f, "if(NOT EXISTS \"${_spice_%s_inc}\")\n", d->name);
            fprintf(f, "  set(_spice_%s_inc \"%s\")\n", d->name, abs_path);
            fprintf(f, "endif()\n");
            fprintf(f, "set(_spice_%s_bld "
                       "\"${CMAKE_BINARY_DIR}/_local/%s-build\")\n",
                    d->name, d->name);
        } else if (d->prefer_system) {
            /* System-first: when find_package() resolved the dep, pull the
             * include dir from the first target's INTERFACE_INCLUDE_DIRECTORIES
             * (generator expression evaluated at file(GENERATE) time). The
             * fetch fallback uses the usual SOURCE_DIR/BINARY_DIR layout.
             * The system branch links via the namespaced targets, so it sets
             * empty _bld; link_dirs come from $<TARGET_FILE_DIR:...> below. */
            fprintf(f, "if(_%s_resolved_via STREQUAL \"system\")\n", d->name);
            if (d->n_targets > 0) {
                fprintf(f, "  set(_spice_%s_inc "
                           "\"$<TARGET_PROPERTY:%s,INTERFACE_INCLUDE_DIRECTORIES>\")\n",
                        d->name, d->targets[0]);
            } else {
                fprintf(f, "  set(_spice_%s_inc \"\")\n", d->name);
            }
            fprintf(f, "  set(_spice_%s_bld \"\")\n", d->name);
            fprintf(f, "else()\n");
            emit_fetch_inc_bld(f, d->name);
            fprintf(f, "endif()\n");
        } else {
            emit_fetch_inc_bld(f, d->name);
        }

        /* Header: open brace + resolved_via (+ system_version for
         * :prefer-system deps). The include_dirs/link_* lines follow; for
         * :prefer-system they are emitted per-branch below because the
         * system and fetch paths key off different target names and include
         * sources. */
        fprintf(f, "string(APPEND _spice_manifest\n");
        fprintf(f, "  \"  \\\"%s\\\": {\\n\"\n", d->name);
        fprintf(f, "  \"    \\\"resolved_via\\\": \\\"${_%s_resolved_via}\\\",\\n\"\n",
                d->name);
        /* SF3: capture the find_package version (set by find_package on the
         * system path) so the lockfile can record :system-version. */
        if (d->prefer_system)
            fprintf(f, "  \"    \\\"system_version\\\": \\\"${%s_VERSION}\\\",\\n\"\n",
                    d->cmake_name);
        fprintf(f, ")\n");

        /* include_dirs + link_dirs/link_libs. For :prefer-system deps the
         * genexpr target key differs by branch (namespaced "MbedTLS::mbedtls"
         * for the system import vs the unaliased "mbedtls" the FetchContent
         * build exports), and the system branch sources include dirs from the
         * targets' INTERFACE_INCLUDE_DIRECTORIES, so the two branches are
         * emitted under a runtime CMake `if`. */
        if (d->prefer_system) {
            fprintf(f, "if(_%s_resolved_via STREQUAL \"system\")\n", d->name);
            emit_link_flags_prelude(f, d, /*use_full_targets=*/true);
            fprintf(f, "string(APPEND _spice_manifest\n");
            emit_include_dirs_line(f, d, /*from_targets=*/true);
            emit_link_lines(f, d, link_lib, /*use_full_targets=*/true);
            fprintf(f, ")\n");
            fprintf(f, "else()\n");
            emit_link_flags_prelude(f, d, /*use_full_targets=*/false);
            fprintf(f, "string(APPEND _spice_manifest\n");
            emit_include_dirs_line(f, d, /*from_targets=*/false);
            emit_link_lines(f, d, link_lib, /*use_full_targets=*/false);
            fprintf(f, ")\n");
            fprintf(f, "endif()\n");
        } else {
            /* :targets present -- derive link_dirs from $<TARGET_FILE_DIR:tgt>
             * (one per target, deduped at the linker level) and link_libs from
             * the target basenames. Generator expressions evaluate at
             * file(GENERATE) time, so the real .a paths land in the JSON even
             * when the dep's build dir layout is nested (e.g. mbedTLS puts its
             * .a files in <BINARY_DIR>/library/, not <BINARY_DIR>/ directly).
             * With no :targets, fall back to the dep's BINARY_DIR + a single
             * link_lib from cmake_dep_link_lib().
             *
             * Include dirs: when :targets is declared, source them from the
             * first target's INTERFACE_INCLUDE_DIRECTORIES rather than the
             * ${SOURCE_DIR}/{include,} heuristic in emit_fetch_inc_bld --
             * the heuristic misses libraries (e.g. yyjson) that publish their
             * public header from a non-standard subdir like ${SOURCE_DIR}/src.
             * Falls back to the heuristic when no :targets are declared. */
            emit_link_flags_prelude(f, d, /*use_full_targets=*/false);
            fprintf(f, "string(APPEND _spice_manifest\n");
            emit_include_dirs_line(f, d, /*from_targets=*/d->n_targets > 0);
            emit_link_lines(f, d, link_lib, /*use_full_targets=*/false);
            fprintf(f, ")\n");
        }

        fprintf(f, "string(APPEND _spice_manifest \"  }\")\n\n");
    }

    fprintf(f, "string(APPEND _spice_manifest \"\\n}\\n\")\n");
    /* file(GENERATE) evaluates generator expressions in CONTENT at generation
     * time -- required for $<TARGET_FILE_DIR:...> to expand to a real path. */
    fprintf(f, "file(GENERATE OUTPUT \"${_spice_manifest_path}\" "
               "CONTENT \"${_spice_manifest}\")\n");
    fprintf(f, "message(STATUS \"spice: will write cmake/spice-deps-manifest.json at generate time\")\n");

    fclose(f);
    fprintf(stderr, "spice: generated %s\n", out_path);
    return true;
}

/* ================================================================== */
/* cmake build invocation                                              */
/* ================================================================== */

/* Major version of the `cmake` on PATH, or 0 if it cannot be determined.
 * Probed once and cached -- pkg_cmake_build can run several times per
 * invocation (fetch, then build). */
static int cmake_major_version(void) {
    static int cached = -1;
    if (cached >= 0) return cached;
    cached = 0;
    FILE *p = popen("cmake --version 2>" TUR_DEVNULL, "r");
    if (p) {
        char line[256];
        if (fgets(line, sizeof(line), p)) {
            /* "cmake version 4.3.3" */
            const char *v = strstr(line, "version ");
            if (v) cached = atoi(v + strlen("version "));
        }
        pclose(p);
    }
    return cached;
}

bool pkg_cmake_build(const char *project_dir,
                     const PkgManifest *manifest,
                     PkgLockFile *lock,
                     const char *target) {
    if (manifest->n_cmake_deps == 0) return true;

    bool wasm = target && strcmp(target, "wasm") == 0;

    char cmake_src[4096];
    snprintf(cmake_src, sizeof(cmake_src), "%s/cmake", project_dir);

    /* Per-arm build dir.  A native and a wasm configure cannot share one:
     * the toolchain file, compiler and every cached path differ, and CMake
     * refuses (or worse, silently reuses) a tree configured for the other. */
    char cmake_bld[4096];
    snprintf(cmake_bld, sizeof(cmake_bld), "%s/cmake/%s", project_dir,
             wasm ? "build-wasm" : "build");

    if (!mkdirp(cmake_bld)) {
        fprintf(stderr, "spice: cannot create '%s'\n", cmake_bld);
        return false;
    }

    /* Configure */
    Buf cmd;
    bool cmake_ok = true;
    buf_init(&cmd);
    buf_puts(&cmd, wasm ? "emcmake cmake -S " : "cmake -S ");
    cmake_ok = pkg_cmd_arg(&cmd, cmake_src) && cmake_ok;
    buf_puts(&cmd, " -B ");
    cmake_ok = pkg_cmd_arg(&cmd, cmake_bld) && cmake_ok;
    /* CMake 4 removed compatibility with `cmake_minimum_required` floors below
     * 3.5, so a dependency that has not raised its floor (hiredis, and plenty
     * of other stable C libraries) aborts the whole configure and *nothing*
     * builds -- including the deps that were fine. The spice author does not
     * control that floor, so failing here punishes the wrong person.
     *
     * Pass the escape hatch CMake itself recommends in that error, which is
     * also what turmeric's own bootstrap already does. Only on CMake >= 4: on
     * 3.x the variable goes unused and CMake reports it under
     * "Manually-specified variables were not used by the project", which is
     * noise on every single configure. TUR_CMAKE_NO_POLICY_MIN=1 opts out for
     * a project that wants the strict floor enforced.
     *
     * This used to carry a `!wasm` conjunct, which made the reasoning above
     * apply to native builds only: the identical manifest configured natively
     * and died under emcmake with "Compatibility with CMake < 3.5 has been
     * removed from CMake."  Nothing about a low `cmake_minimum_required` floor
     * is arm-specific, and the CMake 3.x noise the guard worried about is
     * already handled by the arm-independent `cmake_major_version() >= 4`
     * test.  raylib hid the bug -- its own floor is exactly 3.5, the lowest
     * CMake 4 still accepts -- so it only showed up on the second cmake dep.
     * docs/reported/wasm-arm-suppresses-cmake-policy-min.md. */
    if (cmake_major_version() >= 4 && !getenv("TUR_CMAKE_NO_POLICY_MIN"))
        buf_printf(&cmd, " -DCMAKE_POLICY_VERSION_MINIMUM=3.5");
    /* SF3: honor `tur fetch --refetch` (sets TUR_FETCH_FORCE_FETCH) by
     * disabling the system find_package short-circuit. */
    if (getenv("TUR_FETCH_FORCE_FETCH"))
        buf_printf(&cmd, " -DTUR_FETCH_FORCE_FETCH=ON");
    buf_putc(&cmd, '\0');
    fprintf(stderr, "spice: cmake configure%s ...\n", wasm ? " (wasm)" : "");
    int rc = cmake_ok ? system(cmd.data) : -1;
    buf_free(&cmd);
    if (rc != 0) {
        fprintf(stderr, "spice: cmake configure failed (status %d)\n", rc);
        return false;
    }

    /* Build */
    buf_init(&cmd);
    buf_puts(&cmd, "cmake --build ");
    cmake_ok = pkg_cmd_arg(&cmd, cmake_bld) && cmake_ok;
    buf_putc(&cmd, '\0');
    fprintf(stderr, "spice: cmake build ...\n");
    rc = cmake_ok ? system(cmd.data) : -1;
    buf_free(&cmd);
    if (rc != 0) {
        fprintf(stderr, "spice: cmake build failed (status %d)\n", rc);
        return false;
    }

    /* SF3: read which resolution path each :prefer-system dep actually took
     * (recorded as "resolved_via" in the generated manifest JSON). */
    PkgCmakeManifest cmkman;
    memset(&cmkman, 0, sizeof(cmkman));
    char manifest_json[4096];
    snprintf(manifest_json, sizeof(manifest_json), "%s/%s", cmake_src,
             wasm ? PKG_CMAKE_MANIFEST_WASM : PKG_CMAKE_MANIFEST_NATIVE);
    pkg_cmake_manifest_read(manifest_json, &cmkman);

    /* Update tur.lock cmake-dep entries with resolved git SHAs */
    for (int i = 0; i < manifest->n_cmake_deps; i++) {
        const PkgCmakeDep *d = &manifest->cmake_deps[i];
        if (!d->url) continue; /* local path deps are not locked */

        /* Determine which path this dep resolved through. */
        const char *via = NULL;
        const char *sysver = NULL;
        for (int k = 0; k < cmkman.n_entries; k++) {
            if (strcmp(cmkman.entries[k].name, d->name) == 0) {
                via    = cmkman.entries[k].resolved_via;
                sysver = cmkman.entries[k].system_version;
                break;
            }
        }

        PkgLockEntry *le = lock_upsert(lock, d->name, true);
        if (!le) continue;

        if (via && strcmp(via, "system") == 0) {
            /* System-resolved: record provenance only, drop any stale
             * fetch fields so the lock writer emits the system shape. */
            free(le->url);          le->url          = NULL;
            free(le->ref);          le->ref          = NULL;
            free(le->resolved);     le->resolved     = NULL;
            free(le->sha256);       le->sha256       = NULL;
            free(le->fetched_at);   le->fetched_at   = NULL;
            free(le->resolved_via); le->resolved_via = tur_strdup("system");
            free(le->system_version);
            le->system_version = (sysver && sysver[0]) ? tur_strdup(sysver)
                                                       : NULL;
            continue;
        }

        free(le->url); le->url = tur_strdup(d->url);
        free(le->ref); le->ref = d->ref ? tur_strdup(d->ref) : NULL;
        free(le->fetched_at); le->fetched_at = tur_strdup(iso_now());
        if (d->prefer_system) {
            free(le->resolved_via); le->resolved_via = tur_strdup("fetch");
        }

        /* Read git HEAD SHA from cmake's fetched source directory */
        char dep_src[4096];
        snprintf(dep_src, sizeof(dep_src), "%s/_deps/%s-src", cmake_bld, d->name);
        char *sha = pkg_git_resolve(dep_src);
        if (sha) {
            free(le->resolved); le->resolved = sha;
            free(le->sha256);   le->sha256   = tur_strdup(sha);
        }
    }

    pkg_cmake_manifest_free(&cmkman);
    return true;
}

/* C-3: verify every PRESENT, lock-pinned spice tree against its recorded hash.
 *
 * Absent directories are deliberately not an error here -- the caller decides
 * whether a missing dep means "fetch it" (tur run) or "you have not fetched
 * yet" (tur build) -- and a hash an older tur wrote is skipped rather than
 * reported as tampering, which is what pkg_hash_comparable is for.
 *
 * This used to be open-coded inside `tur run` and existed NOWHERE else, so
 * `tur build` -- the command that actually compiles the dependency's code --
 * never checked anything. One copy, three callers (run, build, audit). */
bool pkg_verify_locked_spices(const char *project_dir,
                              const PkgManifest *manifest,
                              const PkgLockFile *lock,
                              const char *cmd) {
    if (!manifest || !lock) return true;

    char spices_dir[4096];
    snprintf(spices_dir, sizeof(spices_dir), "%s/spices", project_dir);

    bool ok = true;
    for (int i = 0; i < manifest->n_spices; i++) {
        const PkgSpice *s = &manifest->spices[i];
        if (s->path) continue;                                  /* local source */
        if (pkg_is_workspace_member(project_dir, s->name)) continue;

        char dep_dir[4096];
        if (s->ref)
            snprintf(dep_dir, sizeof(dep_dir), "%s/%s-%s", spices_dir, s->name, s->ref);
        else
            snprintf(dep_dir, sizeof(dep_dir), "%s/%s", spices_dir, s->name);

        struct stat st;
        if (stat(dep_dir, &st) != 0 || !S_ISDIR(st.st_mode)) continue;

        PkgLockEntry *le = pkg_lock_find((PkgLockFile *)lock, s->name, false);
        if (!le || !pkg_hash_comparable(le->sha256)) continue;

        char actual[PKG_HASH_MAX];
        if (!pkg_hash_dir(dep_dir, actual)) continue;
        if (strcmp(actual, le->sha256) == 0) continue;

        fprintf(stderr,
            "%s: integrity check FAILED for spice '%s'\n"
            "  tur.lock pinned: %s\n"
            "  on disk:         %s\n"
            "  %s has changed since it was fetched. If you edited it, that is\n"
            "  why; otherwise re-download it:\n"
            "      tur fetch --update\n",
            cmd, s->name, le->sha256, actual, dep_dir);
        ok = false;
    }
    return ok;
}

bool pkg_cmake_verify_lock(const char *project_dir,
                            const PkgLockFile *lock) {
    char cmake_bld[4096];
    snprintf(cmake_bld, sizeof(cmake_bld), "%s/cmake/build", project_dir);

    bool ok = true;
    for (int i = 0; i < lock->n_entries; i++) {
        const PkgLockEntry *le = &lock->entries[i];
        if (!le->is_cmake || !le->url || !le->resolved) continue;

        char dep_src[4096];
        snprintf(dep_src, sizeof(dep_src), "%s/_deps/%s-src", cmake_bld, le->name);

        struct stat st;
        if (stat(dep_src, &st) != 0) continue; /* not yet fetched -- skip */

        char *sha = pkg_git_resolve(dep_src);
        if (!sha) continue;

        if (strcmp(sha, le->resolved) != 0) {
            fprintf(stderr,
                "spice: cmake dep '%s' SHA mismatch!\n"
                "  lock: %s\n"
                "  disk: %s\n"
                "  Run `tur fetch` to resync.\n",
                le->name, le->resolved, sha);
            ok = false;
        }
        free(sha);
    }
    return ok;
}

/* ================================================================== */
/* spice-deps-manifest.json parsing                                    */
/* ================================================================== */

static const char *json_skip_ws(const char *p) {
    while (p && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) p++;
    return p;
}

/* Parse a quoted JSON string. Advances *pp past the closing quote.
 * Returns heap-allocated C string, or NULL on parse error. */
static char *json_parse_str(const char **pp) {
    const char *p = json_skip_ws(*pp);
    if (!p || *p != '"') return NULL;
    p++;
    const char *s = p;
    while (*p && *p != '"') {
        if (*p == '\\') { p++; if (*p) p++; } /* skip escaped char */
        else p++;
    }
    if (*p != '"') return NULL;
    size_t n = (size_t)(p - s);
    char *r = (char *)malloc(n + 1);
    if (!r) return NULL;
    memcpy(r, s, n);
    r[n] = '\0';
    *pp = p + 1;
    return r;
}

/* Parse a JSON array of strings ["a","b",...].
 * Advances *pp past the closing ]. */
static char **json_parse_str_arr(const char **pp, int *n_out) {
    *n_out = 0;
    const char *p = json_skip_ws(*pp);
    if (!p || *p != '[') return NULL;
    p++;
    int cap = 4;
    char **arr = (char **)malloc(cap * sizeof(char *));
    if (!arr) return NULL;
    while (1) {
        p = json_skip_ws(p);
        if (!*p || *p == ']') break;
        if (*p == ',') { p++; continue; }
        char *s = json_parse_str(&p);
        if (!s) { if (*p) p++; continue; }
        if (*n_out >= cap) {
            cap *= 2;
            arr = (char **)realloc(arr, cap * sizeof(char *));
            if (!arr) { free(s); return NULL; }
        }
        arr[(*n_out)++] = s;
    }
    if (*p == ']') p++;
    *pp = p;
    return arr;
}

bool pkg_cmake_manifest_read(const char *path, PkgCmakeManifest *out) {
    memset(out, 0, sizeof(*out));

    /* "rb": see pkg_lock_read -- a text-mode read is short of the ftell size on
     * Windows and the check below then rejects tur's own file. */
    FILE *f = fopen(path, "rb");
    if (!f) return true; /* not present is not an error */

    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    rewind(f);
    char *src = (char *)malloc((size_t)sz + 1);
    if (!src) { fclose(f); return false; }
    if (fread(src, 1, (size_t)sz, f) != (size_t)sz) {
        fclose(f); free(src); return false;
    }
    fclose(f);
    src[sz] = '\0';

    const char *p = src;
    p = json_skip_ws(p);
    if (*p != '{') { free(src); return false; }
    p++;

    int cap = 8;
    out->entries = (PkgCmakeManifestEntry *)malloc(
        cap * sizeof(PkgCmakeManifestEntry));
    if (!out->entries) { free(src); return false; }

    while (1) {
        p = json_skip_ws(p);
        if (!*p || *p == '}') break;
        if (*p == ',') { p++; continue; }

        /* Parse dep name */
        char *name = json_parse_str(&p);
        if (!name) { if (*p) p++; continue; }

        p = json_skip_ws(p);
        if (*p != ':') { free(name); if (*p) p++; continue; }
        p++;
        p = json_skip_ws(p);
        if (*p != '{') { free(name); if (*p) p++; continue; }
        p++;

        if (out->n_entries >= cap) {
            cap *= 2;
            out->entries = (PkgCmakeManifestEntry *)realloc(
                out->entries, cap * sizeof(PkgCmakeManifestEntry));
            if (!out->entries) { free(name); free(src); return false; }
        }
        PkgCmakeManifestEntry *e = &out->entries[out->n_entries++];
        memset(e, 0, sizeof(*e));
        e->name = name;

        /* Parse the entry object */
        while (1) {
            p = json_skip_ws(p);
            if (!*p || *p == '}') break;
            if (*p == ',') { p++; continue; }

            char *key = json_parse_str(&p);
            if (!key) { if (*p) p++; continue; }
            p = json_skip_ws(p);
            if (*p != ':') { free(key); if (*p) p++; continue; }
            p++;

            if (strcmp(key, "resolved_via") == 0) {
                p = json_skip_ws(p);
                e->resolved_via = json_parse_str(&p);
            } else if (strcmp(key, "system_version") == 0) {
                p = json_skip_ws(p);
                e->system_version = json_parse_str(&p);
            } else if (strcmp(key, "include_dirs") == 0) {
                e->include_dirs = json_parse_str_arr(&p, &e->n_include_dirs);
            } else if (strcmp(key, "link_dirs") == 0) {
                e->link_dirs = json_parse_str_arr(&p, &e->n_link_dirs);
            } else if (strcmp(key, "link_libs") == 0) {
                e->link_libs = json_parse_str_arr(&p, &e->n_link_libs);
            } else if (strcmp(key, "link_flags") == 0) {
                e->link_flags = json_parse_str_arr(&p, &e->n_link_flags);
            } else {
                /* skip unknown value */
                p = json_skip_ws(p);
                if (*p == '"') { char *tmp = json_parse_str(&p); free(tmp); }
                else if (*p == '[') {
                    int dummy = 0;
                    char **tmp = json_parse_str_arr(&p, &dummy);
                    for (int k = 0; k < dummy; k++) free(tmp[k]);
                    free(tmp);
                } else {
                    while (*p && *p != ',' && *p != '}') p++;
                }
            }
            free(key);
        }
        if (*p == '}') p++;
    }

    free(src);
    return true;
}

void pkg_cmake_manifest_free(PkgCmakeManifest *m) {
    for (int i = 0; i < m->n_entries; i++) {
        PkgCmakeManifestEntry *e = &m->entries[i];
        free(e->name);
        free(e->resolved_via);
        free(e->system_version);
        for (int j = 0; j < e->n_include_dirs; j++) free(e->include_dirs[j]);
        free(e->include_dirs);
        for (int j = 0; j < e->n_link_dirs; j++) free(e->link_dirs[j]);
        free(e->link_dirs);
        for (int j = 0; j < e->n_link_libs; j++) free(e->link_libs[j]);
        free(e->link_libs);
        for (int j = 0; j < e->n_link_flags; j++) free(e->link_flags[j]);
        free(e->link_flags);
    }
    free(m->entries);
    memset(m, 0, sizeof(*m));
}

/* Classify one "link_flags" token and append it to the cc link line.
 *
 * The tokens come from a CMake target's INTERFACE_LINK_LIBRARIES (see
 * emit_link_lines) and from a dep's `:link-flags`, so they arrive in whatever
 * shape CMake records them rather than in a shape tur chose:
 *
 *   "-framework Cocoa"        a linker flag -- verbatim (this is the whole
 *                             point: a framework cannot be spelled as -l)
 *   "/abs/path/libfoo.a"      an artifact path (what $<TARGET_FILE:...>
 *                             yields) -- verbatim, which is what picks a
 *                             specific archive instead of letting -l prefer
 *                             a sibling .dylib
 *   "$<LINK_ONLY:Threads::Threads>"
 *                             an *unevaluated* genex. file(GENERATE) expands
 *                             $<TARGET_PROPERTY:...> one level and does not
 *                             re-evaluate what comes back, so nested genexes
 *                             survive into the JSON as literal text. Skipped.
 *   "Foo::Bar"                a namespaced CMake target name. Only CMake can
 *                             say what artifact that is, and by this point
 *                             CMake is no longer in the loop. Skipped.
 *   "m"                       a bare library name -- -lm.
 *
 * The two skips are not silent losses relative to the old behavior: before
 * link_flags existed, INTERFACE_LINK_LIBRARIES was not read at all, so every
 * one of these entries was dropped. */
static void append_link_flag_token(Buf *buf, const char *tok) {
    if (!tok || !tok[0]) return;
    if (strstr(tok, "$<")) return;          /* unevaluated genex */
    if (strstr(tok, "::")) return;          /* namespaced target name */
    /* An Apple framework arrives from INTERFACE_LINK_LIBRARIES as an absolute
     * path to the .framework *directory* (raylib reports OpenGL that way).
     * That cannot be passed through as a link input -- ld rejects it with
     * "file cannot be mmap()ed, errno=22", because a .framework is a directory
     * and not an object. It has to be respelled as `-framework <name>`. */
    size_t n = strlen(tok);
    const char *sfx = ".framework";
    size_t sn = strlen(sfx);
    if (tok[0] == '/' && n > sn && strcmp(tok + n - sn, sfx) == 0) {
        const char *slash = strrchr(tok, '/');
        const char *base  = slash ? slash + 1 : tok;
        buf_printf(buf, " -framework %.*s", (int)((n - sn) - (size_t)(base - tok)),
                   base);
        return;
    }
    if (tok[0] == '-' || tok[0] == '/')
        buf_printf(buf, " %s", tok);        /* flag or absolute path */
    else
        buf_printf(buf, " -l%s", tok);      /* bare library name */
}

/* WP2 (D-1): one entry of cmake's spice-deps-manifest.json, on its way into a
 * shell command string.
 *
 * Unlike a manifest's `:link-flags`, these are PATHS and library names that
 * cmake computed on this machine, not text a manifest author typed -- so the
 * check is for shell syntax only, not for shape, and a path with a space is
 * left alone (it is already broken by the space-joined contract, and narrowing
 * it here would reject working machines for no security gain).  A token that
 * fails is dropped with a diagnostic rather than silently spliced: the link
 * error that follows names the missing library, which is a better outcome than
 * running whatever the token said. */
static bool cmake_manifest_token_ok(const char *tok, const char *what) {
    if (!tok || !*tok) return false;
    if (tur_arg_is_shell_safe(tok)) return true;
    fprintf(stderr,
            "spice: dropping %s '%s' from cmake/spice-deps-manifest.json -- it "
            "contains a character the shell would read as syntax\n", what, tok);
    return false;
}

void pkg_cmake_manifest_append_cc_flags(const PkgCmakeManifest *m, Buf *buf) {
    for (int i = 0; i < m->n_entries; i++) {
        const PkgCmakeManifestEntry *e = &m->entries[i];
        for (int j = 0; j < e->n_include_dirs; j++) {
            if (e->include_dirs[j] && e->include_dirs[j][0] &&
                cmake_manifest_token_ok(e->include_dirs[j], "include dir"))
                buf_printf(buf, " -I%s", e->include_dirs[j]);
        }
        for (int j = 0; j < e->n_link_dirs; j++) {
            if (e->link_dirs[j] && e->link_dirs[j][0] &&
                cmake_manifest_token_ok(e->link_dirs[j], "link dir"))
                buf_printf(buf, " -L%s", e->link_dirs[j]);
        }
        for (int j = 0; j < e->n_link_libs; j++) {
            if (e->link_libs[j] && e->link_libs[j][0] &&
                cmake_manifest_token_ok(e->link_libs[j], "link lib"))
                buf_printf(buf, " -l%s", e->link_libs[j]);
        }
        /* After link_libs: a dep's own artifact must precede the transitive
         * libraries it depends on for static archive resolution. */
        for (int j = 0; j < e->n_link_flags; j++)
            if (cmake_manifest_token_ok(e->link_flags[j], "link flag"))
                append_link_flag_token(buf, e->link_flags[j]);
    }
}

/* ================================================================== */
/* CLI: tur new / tur init -- shared helpers                           */
/* ================================================================== */

/* Reserved spice names that collide with built-in subcommands / build
 * directives and would confuse module resolution (NW0). */
static bool is_reserved_project_name(const char *name) {
    static const char *const reserved[] = { "tur", "build", "test" };
    for (size_t i = 0; i < sizeof(reserved) / sizeof(reserved[0]); i++) {
        if (strcmp(name, reserved[i]) == 0) return true;
    }
    return false;
}

/* Validate project name (NW0): must match [a-z][a-z0-9-]*, be 2-64
 * characters, lead with a letter, and not be a reserved name. */
static bool valid_project_name(const char *name) {
    if (!name || !*name) return false;
    size_t len = strlen(name);
    if (len < 2 || len > 64) return false;
    if (!islower((unsigned char)*name)) return false;
    for (const char *p = name + 1; *p; p++) {
        if (!islower((unsigned char)*p) &&
            !isdigit((unsigned char)*p) &&
            *p != '-')
            return false;
    }
    if (is_reserved_project_name(name)) return false;
    return true;
}

/* Scaffold a new project inside 'dir' (must already exist).
 * 'name' is the project name string used in generated files.
 * Returns 0 on success. */
/* ================================================================== */
/* Scaffold options (NW0-NW5)                                          */
/* ================================================================== */

typedef struct {
    const char *dir;         /* target directory */
    const char *name;        /* spice name */
    bool        is_bin;      /* --kind bin vs lib */
    bool        no_git;      /* --no-git */
    bool        no_ci;       /* --no-ci  */
    bool        no_justfile; /* --no-justfile */
    bool        dry_run;     /* --dry-run */
    bool        here;        /* --here (scaffold into cwd) */
    bool        sweet;       /* --sweet (emit build.tur.sweet instead of build.tur) */
    const char *author;      /* --author "Name <email>" */
    const char *license;     /* --license MIT|Apache-2.0|BSD-3-Clause|none */
    bool        force;       /* --force: overwrite files that already exist */
    /* saffron-lang-plan S8: --saffron scaffolds a `#lang saffron` SOURCE tree.
     * The manifest is untouched -- a manifest is a manifest in either dialect,
     * and `:experiments` is not needed because the `#lang` line enables the
     * experiment by itself. */
    bool        saffron;
    /* r7rs-lang-plan R9: --r7rs scaffolds `#lang r7rs` sources -- a binary is
     * a top-level Scheme program, a library a `define-library`.  Same
     * manifest, for the same reason as --saffron. */
    bool        r7rs;
} ScaffoldOpts;

/* Write a file and print it in the scaffold summary.
 * Returns true on success; false on I/O error. */
/* tur-init-help-scaffolds-and-overwrites, defect 2: scaffolding OVERWROTE
 * existing files without refusing or asking.  Run in a checkout of this repo it
 * replaced a 209-line `.gitignore` with an 8-line template and clobbered
 * `README.md`; both were tracked so git recovered them, but uncommitted edits
 * to either would simply be gone.
 *
 * The refusal is PRE-FLIGHT rather than per-file, so a collision cannot leave a
 * half-scaffolded tree -- and it reuses the real scaffold walk in a probe pass
 * rather than re-listing the dozen target paths, which would drift the first
 * time a scaffold file is added.  `scaffold_write` is the single chokepoint
 * every target goes through, so the probe sees exactly what a real run would
 * write.
 *
 * There WAS a refusal, but it keyed on a MANIFEST being present
 * (`build.tur`/`build.tur.sweet`), not on the files about to be clobbered -- so
 * a project checkout with no manifest at its root sailed straight past it. */
static bool g_scaffold_probe = false;
static char g_scaffold_hits[16][512];
static int  g_scaffold_n_hits = 0;

/* Record a probe collision for a target that does NOT go through
 * scaffold_write.  The manifest, tur.lock, the Justfile and ci.yml each write
 * through their own writer (pkg_manifest_write / pkg_lock_write /
 * justrun_write_template), so the chokepoint below never sees them -- and the
 * first version of this fix therefore left all four still overwritable, and
 * printed their names during the silent probe.  Returns true in probe mode so
 * the caller skips its real work. */
static bool scaffold_probe_note(const char *path) {
    if (!g_scaffold_probe) return false;
    FILE *ex = fopen(path, "r");
    if (ex) {
        fclose(ex);
        if (g_scaffold_n_hits < (int)(sizeof(g_scaffold_hits) /
                                      sizeof(g_scaffold_hits[0])))
            snprintf(g_scaffold_hits[g_scaffold_n_hits++],
                     sizeof(g_scaffold_hits[0]), "%s", path);
    }
    return true;
}

static bool scaffold_write(const char *path, const char *contents, bool dry_run) {
    if (g_scaffold_probe) {
        (void)contents; (void)dry_run;
        FILE *ex = fopen(path, "r");
        if (ex) {
            fclose(ex);
            if (g_scaffold_n_hits < (int)(sizeof(g_scaffold_hits) /
                                          sizeof(g_scaffold_hits[0])))
                snprintf(g_scaffold_hits[g_scaffold_n_hits++],
                         sizeof(g_scaffold_hits[0]), "%s", path);
        }
        return true;
    }
    printf("  %s\n", path);
    if (dry_run) return true;
    FILE *f = fopen(path, "w");
    if (!f) {
        fprintf(stderr, "tur new: cannot write '%s': %s\n", path, strerror(errno));
        return false;
    }
    fputs(contents, f);
    fclose(f);
    return true;
}

/* Determine author string: --author > git config > env > placeholder. */
static void resolve_author(const ScaffoldOpts *opts, char *out, size_t cap) {
    if (opts->author && *opts->author) {
        strncpy(out, opts->author, cap - 1);
        out[cap - 1] = '\0';
        return;
    }
    /* Try GIT_AUTHOR_NAME / GIT_AUTHOR_EMAIL env vars */
    const char *env_name  = getenv("GIT_AUTHOR_NAME");
    const char *env_email = getenv("GIT_AUTHOR_EMAIL");
    if (env_name && env_email && *env_name && *env_email) {
        snprintf(out, cap, "%s <%s>", env_name, env_email);
        return;
    }
    /* Try git config */
    FILE *p = popen("git config user.name 2>" TUR_DEVNULL, "r");
    char git_name[128] = {0};
    char git_email[128] = {0};
    if (p) { if (fgets(git_name, sizeof(git_name), p)) {
        char *nl = strchr(git_name, '\n'); if (nl) *nl = '\0'; } pclose(p); }
    p = popen("git config user.email 2>" TUR_DEVNULL, "r");
    if (p) { if (fgets(git_email, sizeof(git_email), p)) {
        char *nl = strchr(git_email, '\n'); if (nl) *nl = '\0'; } pclose(p); }
    if (git_name[0] && git_email[0])
        snprintf(out, cap, "%s <%s>", git_name, git_email);
    else if (git_name[0])
        strncpy(out, git_name, cap - 1);
    else {
        strncpy(out, "Anonymous <unknown@local>", cap - 1);
        fprintf(stderr, "tur new: git config user.name/user.email not set; "
                "using placeholder author\n");
    }
    out[cap - 1] = '\0';
}

/* NW2: extended scaffold -- single canonical implementation. */
int scaffold_project_ext(const ScaffoldOpts *opts) {
    const char *dir  = opts->dir;
    const char *name = opts->name;
    char path[4096];
    char buf[16384];
    char author[256];

    /* tur-init-help-scaffolds-and-overwrites: refuse before writing ANYTHING
     * if a target already exists.  Runs the whole scaffold once in probe mode
     * (no printing, no mkdir, no writes) to collect collisions from the same
     * walk a real run takes -- see scaffold_write.  Guarded on !g_scaffold_probe
     * so the probe pass does not recurse, and skipped for --dry-run (which
     * writes nothing) and --force (the explicit opt-in). */
    if (!opts->dry_run && !opts->force && !g_scaffold_probe) {
        ScaffoldOpts probe = *opts;
        probe.dry_run = true;
        g_scaffold_n_hits = 0;
        g_scaffold_probe  = true;
        (void)scaffold_project_ext(&probe);
        g_scaffold_probe  = false;
        if (g_scaffold_n_hits > 0) {
            fprintf(stderr, "tur: refusing to overwrite existing file%s:\n",
                    g_scaffold_n_hits == 1 ? "" : "s");
            for (int i = 0; i < g_scaffold_n_hits; i++)
                fprintf(stderr, "  %s\n", g_scaffold_hits[i]);
            fprintf(stderr,
                    "  scaffold into an empty directory, or pass --force to "
                    "overwrite\n");
            return 1;
        }
    }

    resolve_author(opts, author, sizeof(author));
    const char *license = opts->license ? opts->license : "none";

    if (g_scaffold_probe) {
        /* silent */
    } else if (opts->dry_run)
        printf("tur new: (dry-run) would create:\n");
    else
        printf("Creating %s spice '%s'\n",
               opts->is_bin ? "binary" : "library", name);

    /* ---- src/ ---- */
    if (!opts->dry_run) {
        snprintf(path, sizeof(path), "%s/src", dir);
        if (!mkdirp(path)) {
            fprintf(stderr, "tur: cannot create '%s'\n", path);
            return 1;
        }
    }

    /* ---- tests/ ---- */
    if (!opts->dry_run) {
        snprintf(path, sizeof(path), "%s/tests", dir);
        if (!mkdirp(path)) {
            fprintf(stderr, "tur: cannot create '%s'\n", path);
            return 1;
        }
    }

    /* ---- build.tur (or build.tur.sweet) ---- */
    {
        const char *manifest_name = opts->sweet ? "build.tur.sweet" : "build.tur";
        snprintf(path, sizeof(path), "%s/%s", dir, manifest_name);
        if (scaffold_probe_note(path)) { /* probe: recorded, nothing written */ }
        else if (!opts->dry_run) {
            if (opts->sweet) {
                /* Sweet-exp scaffold: simple defpackage in t-expr form. */
                char sweet_buf[1024];
                snprintf(sweet_buf, sizeof(sweet_buf),
                         "#lang sweet-exp\n"
                         "defpackage \"%s\"\n"
                         "  :version \"0.1.0\"\n",
                         name);
                FILE *f = fopen(path, "w");
                if (!f) {
                    fprintf(stderr, "tur: cannot write '%s': %s\n",
                            path, strerror(errno));
                    return 1;
                }
                fputs(sweet_buf, f);
                fclose(f);
            } else {
                PkgManifest m;
                memset(&m, 0, sizeof(m));
                m.name    = (char *)name;
                m.version = "0.1.0";
                if (!pkg_manifest_write(path, &m)) return 1;
            }
            printf("  %s\n", manifest_name);
        } else {
            printf("  %s\n", manifest_name);
        }
    }

    /* ---- tur.lock ---- */
    {
        snprintf(path, sizeof(path), "%s/tur.lock", dir);
        if (scaffold_probe_note(path)) { /* probe: recorded, nothing written */ }
        else if (!opts->dry_run) {
            PkgLockFile lock;
            memset(&lock, 0, sizeof(lock));
            lock.format_version = 1;
            if (!pkg_lock_write(path, &lock)) return 1;
            printf("  tur.lock\n");
        } else {
            printf("  tur.lock\n");
        }
    }

    /* ---- src/<name>.tur (lib) or src/main.tur (bin) ---- */
    {
        /* Convert hyphens to underscores for module/function names */
        char mod_name[256];
        strncpy(mod_name, name, sizeof(mod_name) - 1);
        mod_name[sizeof(mod_name) - 1] = '\0';
        for (char *q = mod_name; *q; q++) if (*q == '-') *q = '_';

        if (opts->is_bin && opts->r7rs) {
            /* An R7RS program is its top-level forms: no `main` (the build
             * treats a `#lang r7rs` file without a define-library as an entry
             * point, file_has_main_defn). */
            snprintf(path, sizeof(path), "%s/src/main.tur", dir);
            snprintf(buf, sizeof(buf),
                "#lang r7rs\n"
                ";;; %s -- entry point.\n"
                ";;\n"
                "(import (scheme base) (scheme write))\n"
                "\n"
                "(display \"Hello from %s!\")\n"
                "(newline)\n",
                name, name);
        } else if (opts->r7rs) {
            snprintf(path, sizeof(path), "%s/src/%s.tur", dir, mod_name);
            snprintf(buf, sizeof(buf),
                "#lang r7rs\n"
                ";;; %s -- library module.\n"
                ";;;\n"
                ";;; Since: 0.1.0\n"
                ";;\n"
                "(define-library (%s)\n"
                "  (export add)\n"
                "  (import (scheme base))\n"
                "  (begin\n"
                "    (define (add a b) (+ a b))))\n",
                name, mod_name);
        } else if (opts->is_bin && opts->saffron) {
            /* No `:int` on main, and no annotations anywhere: that is the whole
             * point of the dialect, and a scaffold that annotated would teach
             * the opposite of what the reader asked for. */
            snprintf(path, sizeof(path), "%s/src/main.tur", dir);
            snprintf(buf, sizeof(buf),
                "#lang saffron\n"
                ";;; %s -- entry point.\n"
                ";;\n"
                "(defn main []\n"
                "  (println \"Hello from %s!\")\n"
                "  0)\n",
                name, name);
        } else if (opts->is_bin) {
            snprintf(path, sizeof(path), "%s/src/main.tur", dir);
            snprintf(buf, sizeof(buf),
                ";;; %s -- entry point.\n"
                ";;\n"
                "(defn main [] :int\n"
                "  (println \"Hello from %s!\")\n"
                "  0)\n",
                name, name);
        } else if (opts->saffron) {
            snprintf(path, sizeof(path), "%s/src/%s.tur", dir, mod_name);
            snprintf(buf, sizeof(buf),
                "#lang saffron\n"
                ";;; %s -- library module.\n"
                ";;;\n"
                ";;; Since: 0.1.0\n"
                ";;\n"
                "(defmodule %s (export add) "
                    "(defn add [a b] (+ a b)))\n",
                name, mod_name);
        } else {
            snprintf(path, sizeof(path), "%s/src/%s.tur", dir, mod_name);
            /* The template is emitted in the exact shape `tur fmt` produces so
             * a fresh scaffold passes `tur fmt --check` unchanged (NW6 / B4):
             * a module `;;;` docstring, a `;;` separator, then a `defmodule`
             * with `export` (so the test module can `(import ...)` it).
             * Per-defn `;;;` docstrings inside a `defmodule` are stripped by
             * fmt today, so the body carries none. The body uses only builtin
             * operators (`+`); stdlib functions are not linked into a fresh
             * spice build, so the old string `greet`/`str` template never
             * compiled (B3). */
            snprintf(buf, sizeof(buf),
                ";;; %s -- library module.\n"
                ";;;\n"
                ";;; Since: 0.1.0\n"
                ";;\n"
                "(defmodule %s (export add) "
                    "(defn add [a :int b :int] :int (+ a b)))\n",
                name, mod_name);
        }
        if (!scaffold_write(path, buf, opts->dry_run)) return 1;
    }

    /* ---- tests/<name>_test.tur ---- */
    {
        char mod_name[256];
        strncpy(mod_name, name, sizeof(mod_name) - 1);
        mod_name[sizeof(mod_name) - 1] = '\0';
        for (char *q = mod_name; *q; q++) if (*q == '-') *q = '_';

        snprintf(path, sizeof(path), "%s/tests/%s_test.tur", dir, mod_name);
        if (opts->is_bin && opts->r7rs) {
            /* r7rs-srfi-plan S6: an SRFI 64 suite.  Its default runner prints
             * the summary and, after a failure, exits 1 from the outermost
             * test-end -- which is what `tur test` counts. */
            snprintf(buf, sizeof(buf),
                "#lang r7rs\n"
                ";;; %s_test -- tests for %s, in SRFI 64.\n"
                ";;\n"
                "(import (scheme base) (srfi 64))\n"
                "\n"
                "(test-begin \"%s\")\n"
                "(test-equal \"arithmetic\" 4 (+ 2 2))\n"
                "(test-end \"%s\")\n",
                mod_name, name, name, name);
        } else if (opts->r7rs) {
            /* A Scheme program importing the library under test, as an SRFI
             * 64 suite (see the binary scaffold). */
            snprintf(buf, sizeof(buf),
                "#lang r7rs\n"
                ";;; %s_test -- unit tests for %s, in SRFI 64.\n"
                ";;\n"
                "(import (scheme base) (srfi 64) (%s))\n"
                "\n"
                "(test-begin \"%s\")\n"
                "(test-equal \"add\" 5 (add 2 3))\n"
                "(test-end \"%s\")\n",
                mod_name, name, mod_name, name, name);
        } else if (opts->is_bin && opts->saffron) {
            snprintf(buf, sizeof(buf),
                "#lang saffron\n"
                ";;; %s_test -- smoke test for %s.\n"
                ";;\n"
                "(defn main []\n"
                "  (println \"tests: ok\")\n"
                "  0)\n",
                mod_name, name);
        } else if (opts->is_bin) {
            snprintf(buf, sizeof(buf),
                ";;; %s_test -- smoke test for %s.\n"
                ";;\n"
                "(defn main [] :int\n"
                "  (println \"tests: ok\")\n"
                "  0)\n",
                mod_name, name);
        } else {
            /* fmt-canonical (see the lib template note): a `defmodule` wrapper
             * so the `(import ...)` is legal (top-level import is rejected),
             * and a real assertion over builtin `=`. Both `if` branches return
             * `:int` (the `0`/`1` exit codes) so the `main` body type-checks. */
            snprintf(buf, sizeof(buf),
                ";;; %s_test -- unit tests for %s.\n"
                ";;\n"
                "(defmodule %s_test\n"
                "  (import %s :refer [add])\n"
                "  (defn main [] :int\n"
                "    (if (= (add 2 3) 5)\n"
                "      (do (println \"tests: ok\") 0)\n"
                "      (do (println \"tests: FAIL\") 1))))\n",
                mod_name, name, mod_name, mod_name);
        }
        if (!scaffold_write(path, buf, opts->dry_run)) return 1;
    }

    /* ---- .gitignore ---- */
    {
        snprintf(path, sizeof(path), "%s/.gitignore", dir);
        snprintf(buf, sizeof(buf),
            "build/\nspices/\n.tur-cache/\n.tur-repl-cache/\n"
            "cmake/CMakeLists.txt\ncmake/build/\n"
            "cmake/spice-deps-manifest.json\n*.o\n");
        if (!scaffold_write(path, buf, opts->dry_run)) return 1;
    }

    /* ---- README.md ---- */
    {
        snprintf(path, sizeof(path), "%s/README.md", dir);
        snprintf(buf, sizeof(buf),
            "# %s\n\n"
            "A Turmeric %s spice.\n\n"
            "## Getting started\n\n"
            "```sh\n"
            "tur run --list   # see available tasks\n"
            "tur run build    # build the spice\n"
            "tur run test     # run the test suite\n"
            "```\n",
            name, opts->is_bin ? "binary" : "library");
        if (!scaffold_write(path, buf, opts->dry_run)) return 1;
    }

    /* ---- LICENSE ---- */
    if (license && strcmp(license, "none") != 0) {
        snprintf(path, sizeof(path), "%s/LICENSE", dir);
        /* Simple stub -- full text would be very long; point to SPDX. */
        snprintf(buf, sizeof(buf),
            "SPDX-License-Identifier: %s\n"
            "\n"
            "See https://spdx.org/licenses/%s.html for the full license text.\n",
            license, license);
        if (!scaffold_write(path, buf, opts->dry_run)) return 1;
    }

    /* ---- Justfile ---- */
    if (!opts->no_justfile) {
        snprintf(path, sizeof(path), "%s/Justfile", dir);
        if (scaffold_probe_note(path)) { /* probe: recorded, nothing written */ }
        else if (opts->dry_run) {
            printf("  Justfile\n");
        } else {
            /* justrun_write_template is defined in justrun.c */
            extern int justrun_write_template(const char *dir,
                                               const char *spice_name,
                                               int force);
            if (justrun_write_template(dir, name, 0) != 0) return 1;
        }
    }

    /* ---- .github/workflows/ci.yml ---- */
    if (!opts->no_ci) {
        if (g_scaffold_probe) {
            char wf_probe[4096];
            snprintf(wf_probe, sizeof(wf_probe), "%s/.github/workflows/ci.yml", dir);
            (void)scaffold_probe_note(wf_probe);
        } else if (!opts->dry_run) {
            char wf_dir[4096];
            snprintf(wf_dir, sizeof(wf_dir), "%s/.github/workflows", dir);
            if (!mkdirp(wf_dir)) {
                fprintf(stderr, "tur new: cannot create '%s'\n", wf_dir);
                /* Non-fatal -- CI is optional */
            } else {
                snprintf(path, sizeof(path), "%s/ci.yml", wf_dir);
                snprintf(buf, sizeof(buf),
                    "name: CI\n"
                    "on: [push, pull_request]\n"
                    "jobs:\n"
                    "  build:\n"
                    "    strategy:\n"
                    "      matrix:\n"
                    "        os: [ubuntu-latest, macos-latest]\n"
                    "    runs-on: ${{ matrix.os }}\n"
                    "    steps:\n"
                    "      - uses: actions/checkout@v4\n"
                    "      - name: Install Turmeric\n"
                    "        run: |\n"
                    "          curl -fsSL https://turmeric-lang.com/install.sh | sh\n"
                    "      - name: Run CI\n"
                    "        run: tur run ci\n");
                if (!scaffold_write(path, buf, false)) {
                    /* Non-fatal */
                }
                printf("  .github/workflows/ci.yml\n");
            }
        } else {
            printf("  .github/workflows/ci.yml\n");
        }
    }

    /* ---- Git init ---- */
    if (!opts->no_git && !opts->dry_run) {
        char cmd[4096], qdir[2048];
        /* The directory is shell-quoted for the PLATFORM's shell, and the null
         * device is spelled the platform's way.  Both were POSIX-only before:
         * cmd.exe does not treat ' as a quote character at all, so git received
         * a literal 'C:\path' and failed, and 2>/dev/null is an unresolvable
         * path there ("The system cannot find the path specified.").  The net
         * effect was a scaffolded project with no git repository and exit 0.
         *
         * The commit message is spelled with the platform quoting too, rather
         * than relying on git to be forgiving about stray single quotes. */
        if (tur_shell_quote(dir, qdir, sizeof(qdir)) != 0) {
            fprintf(stderr, "tur new: project path too long to quote for the "
                    "shell; skipping git init\n");
            goto after_git;
        }
        snprintf(cmd, sizeof(cmd),
            "git -C %s init -q 2>" TUR_DEVNULL " && "
            "git -C %s add -A 2>" TUR_DEVNULL " && "
            "git -C %s commit -q -m \"Initial scaffold from tur new\" 2>" TUR_DEVNULL,
            qdir, qdir, qdir);
        if (system(cmd) != 0) {
            fprintf(stderr, "tur new: git init failed (non-fatal); "
                    "project is on disk without a git repository\n");
        }
    after_git: ;
    }

    if (!opts->dry_run) {
        printf("\nRun:\n");
        if (strcmp(dir, ".") != 0) printf("  cd %s && ", name);
        printf("tur run\n");
    }
    return 0;
}

/* ================================================================== */
/* CLI: tur new                                                         */
/* ================================================================== */

int cmd_pkg_new(int argc, char **argv) {
    /* Usage: tur new <name> [--kind lib|bin] [--bin|--lib] [--no-git]
     *        [--no-ci] [--no-justfile] [--author "..."] [--license MIT]
     *        [--dry-run] [--here] */
    ScaffoldOpts opts;
    memset(&opts, 0, sizeof(opts));
    opts.license = "none";
    opts.is_bin  = false; /* default: library */

    bool show_help = false;
    const char *name = NULL;

    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            show_help = true; break;
        }
        if (strcmp(argv[i], "--bin") == 0)           opts.is_bin      = true;
        else if (strcmp(argv[i], "--lib") == 0)       opts.is_bin      = false;
        else if (strcmp(argv[i], "--no-git") == 0)    opts.no_git      = true;
        else if (strcmp(argv[i], "--no-ci") == 0)     opts.no_ci       = true;
        else if (strcmp(argv[i], "--no-justfile") == 0) opts.no_justfile= true;
        else if (strcmp(argv[i], "--dry-run") == 0)   opts.dry_run     = true;
        else if (strcmp(argv[i], "--here") == 0)      opts.here        = true;
        else if (strcmp(argv[i], "--sweet") == 0)     opts.sweet       = true;
        else if (strcmp(argv[i], "--kind") == 0 && i + 1 < argc) {
            i++;
            if (strcmp(argv[i], "bin") == 0)       opts.is_bin = true;
            else if (strcmp(argv[i], "lib") == 0)  opts.is_bin = false;
            else {
                fprintf(stderr, "tur new: unknown --kind '%s' (use 'lib' or 'bin')\n",
                        argv[i]);
                return 2;
            }
        } else if (strcmp(argv[i], "--author") == 0 && i + 1 < argc) {
            opts.author = argv[++i];
        } else if (strcmp(argv[i], "--license") == 0 && i + 1 < argc) {
            opts.license = argv[++i];
        } else if (argv[i][0] != '-') {
            if (name) {
                fprintf(stderr, "tur new: unexpected argument '%s'\n", argv[i]);
                return 1;
            }
            name = argv[i];
        } else {
            fprintf(stderr, "tur new: unknown option '%s'\n", argv[i]);
            return 2;
        }
    }

    if (show_help) {
        fprintf(stderr,
            "usage: tur new <name> [options]\n"
            "       tur new --here [options]\n"
            "\n"
            "options:\n"
            "  --kind lib|bin     library (default) or binary spice\n"
            "  --bin, --lib       shorthand for --kind\n"
            "  --author NAME      author string (default: git config)\n"
            "  --license SPDX     write LICENSE (MIT, Apache-2.0, "
                                 "BSD-3-Clause, none)\n"
            "  --no-git           skip git init and initial commit\n"
            "  --no-ci            skip .github/workflows/ci.yml\n"
            "  --no-justfile      skip the standard Justfile template\n"
            "  --dry-run          print files that would be created; "
                                 "do not write\n"
            "  --here             scaffold into the current directory\n"
            "  --sweet            emit build.tur.sweet (sweet-exp syntax)\n"
            "\n"
            "generated layout:\n"
            "  build.tur, tur.lock, src/<name>.tur, tests/<name>_test.tur\n"
            "  .gitignore, README.md, Justfile, .github/workflows/ci.yml\n");
        return 0;
    }

    /* Validate license */
    if (opts.license && strcmp(opts.license, "none") != 0 &&
        strcmp(opts.license, "MIT") != 0 &&
        strcmp(opts.license, "Apache-2.0") != 0 &&
        strcmp(opts.license, "BSD-3-Clause") != 0) {
        fprintf(stderr,
            "tur new: unknown license '%s'\n"
            "  Supported: MIT, Apache-2.0, BSD-3-Clause, none\n",
            opts.license);
        return 2;
    }

    if (opts.here) {
        /* Scaffold into cwd; derive name from directory basename */
        if (name) {
            fprintf(stderr, "tur new: --here and a name argument are mutually "
                    "exclusive\n");
            return 2;
        }
        char cwd[4096];
        if (!getcwd(cwd, sizeof(cwd))) {
            fprintf(stderr, "tur new: cannot get current directory\n");
            return 1;
        }
        /* Check the dir is empty (allow hidden files / .git) */
        {
            char found[64];
            if (pkg_resolve_manifest_cwd(found, sizeof(found))) {
                fprintf(stderr, "tur new: %s already exists in cwd "
                        "(already initialised?)\n", found);
                return 1;
            }
        }
        const char *base = strrchr(cwd, '/');
        opts.name = base ? base + 1 : cwd;
        opts.dir  = ".";
        if (!valid_project_name(opts.name)) {
            fprintf(stderr,
                "tur new: directory name '%s' is not a valid spice name\n"
                "  Names must match [a-z][a-z0-9-]* "
                "(lowercase letters, digits, hyphens)\n",
                opts.name);
            return 1;
        }
        return scaffold_project_ext(&opts);
    }

    if (!name) {
        fprintf(stderr, "usage: tur new <name> [--kind lib|bin] [options]\n"
                "       tur new --help\n");
        return 1;
    }

    if (!valid_project_name(name)) {
        fprintf(stderr,
            "tur new: invalid spice name '%s'\n"
            "  Names must match [a-z][a-z0-9-]* "
            "(lowercase letters, digits, hyphens), be 2-64 characters,\n"
            "  start with a letter, and not be reserved (tur, build, test)\n",
            name);
        return 1;
    }

    opts.name = name;
    opts.dir  = name;

    if (!opts.dry_run) {
        struct stat st;
        if (stat(name, &st) == 0) {
            fprintf(stderr, "tur new: '%s' already exists\n", name);
            return 1;
        }
        if (!mkdirp(name)) {
            fprintf(stderr, "tur new: cannot create directory '%s': %s\n",
                    name, strerror(errno));
            return 1;
        }
    }

    return scaffold_project_ext(&opts);
}

/* ================================================================== */
/* CLI: tur init                                                        */
/* ================================================================== */

int cmd_pkg_init(int argc, char **argv) {
    /* Usage: tur init [--bin|--lib] [--no-git] [--sweet] [<name>] */
    bool is_bin = true;
    bool no_git = false;
    bool sweet  = false;
    bool saffron = false;
    bool r7rs = false;
    const char *name = NULL;

    bool force = false;

    for (int i = 2; i < argc; i++) {
        /* tur-init-help-scaffolds-and-overwrites, defect 1: `--help` was not
         * handled.  It starts with '-', so it fell past the name slot too, and
         * with no name given the CURRENT DIRECTORY's basename was used -- so
         * `tur init --help` in a checkout called `turmeric` scaffolded a
         * project called `turmeric` over the working tree.  Every other
         * subcommand handles it; this one did not. */
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            printf("usage: tur init [options] [<name>]\n"
                   "\n"
                   "Scaffold a spice in the CURRENT directory.  With no <name>,\n"
                   "the current directory's basename is used.\n"
                   "\n"
                   "options:\n"
                   "  --bin, --lib   binary (default) or library spice\n"
                   "  --sweet        write build.tur.sweet (sweet-exp manifest)\n"
                   "  --saffron      scaffold `#lang saffron` sources (no annotations)\n"
                   "  --r7rs         scaffold `#lang r7rs` sources (a Scheme program,\n"
                   "                 or a define-library with --lib)\n"
                   "  --no-git       skip git init\n"
                   "  --force        overwrite files that already exist\n"
                   "  -h, --help     show this help\n");
            return 0;
        }
        if (strcmp(argv[i], "--bin") == 0)         is_bin = true;
        else if (strcmp(argv[i], "--lib") == 0)    is_bin = false;
        else if (strcmp(argv[i], "--no-git") == 0) no_git = true;
        else if (strcmp(argv[i], "--sweet") == 0)  sweet  = true;
        else if (strcmp(argv[i], "--saffron") == 0) saffron = true;
        else if (strcmp(argv[i], "--r7rs") == 0)   r7rs = true;
        else if (strcmp(argv[i], "--force") == 0)  force  = true;
        /* An UNKNOWN flag was silently IGNORED, which is the same accident with
         * a different spelling: a flag that does not exist scaffolded just as
         * `--help` did.  Refuse instead.  (`--saffron` was the example this
         * comment used to name; it exists now, one line above.) */
        else if (argv[i][0] == '-') {
            fprintf(stderr, "tur init: unknown option '%s'\n"
                            "  run `tur init --help` for usage\n", argv[i]);
            return 1;
        }
        else {
            if (name) {
                fprintf(stderr, "tur init: unexpected argument '%s'\n", argv[i]);
                return 1;
            }
            name = argv[i];
        }
    }

    /* Derive name from current directory basename if not provided */
    char pwd_name[256];
    if (!name) {
        char cwd[4096];
        if (!getcwd(cwd, sizeof(cwd))) {
            fprintf(stderr, "tur init: cannot get current directory\n");
            return 1;
        }
        const char *base = strrchr(cwd, '/');
        strncpy(pwd_name, base ? base + 1 : cwd, sizeof(pwd_name) - 1);
        pwd_name[sizeof(pwd_name) - 1] = '\0';
        name = pwd_name;
    }

    if (!valid_project_name(name)) {
        fprintf(stderr,
            "tur init: invalid project name '%s'\n"
            "  Names must match [a-z][a-z0-9-]* "
            "(lowercase letters, digits, hyphens), be 2-64 characters,\n"
            "  start with a letter, and not be reserved (tur, build, test)\n",
            name);
        return 1;
    }

    /* Refuse if build.tur or build.tur.sweet already exists */
    {
        char found[64];
        if (pkg_resolve_manifest_cwd(found, sizeof(found))) {
            fprintf(stderr, "tur init: %s already exists\n", found);
            return 1;
        }
    }

    if (saffron && r7rs) {
        fprintf(stderr, "tur init: --saffron and --r7rs pick different languages; give one\n");
        return 1;
    }

    ScaffoldOpts opts;
    memset(&opts, 0, sizeof(opts));
    opts.dir     = ".";
    opts.name    = name;
    opts.is_bin  = is_bin;
    opts.no_git  = no_git;
    opts.sweet   = sweet;
    opts.saffron = saffron;
    opts.r7rs    = r7rs;
    opts.license = "none";
    opts.force   = force;
    return scaffold_project_ext(&opts);
}

/* ================================================================== */
/* Form-based build.tur mutation (comment-preserving round-trip)        */
/* ================================================================== */

/* Intern a keyword symbol by C string name. */
static const Symbol *pkg_intern(SymbolTable *st, const char *name) {
    return symtab_intern(st, strslice(name, (uint32_t)strlen(name)));
}

/* Build a spice entry value map: #{ :url "..." :ref "..." }
 * or #{ :path "..." } for local deps. */
static Form *pkg_build_spice_val(Arena *a, SymbolTable *st,
                                  const char *url, const char *ref,
                                  const char *path, const char *subdir,
                                  bool optional, bool literal) {
    Form *items[10];
    uint32_t n = 0;
    if (path) {
        items[n++] = form_keyword(a, SPAN_UNKNOWN, pkg_intern(st, "path"));
        items[n++] = form_str(a, SPAN_UNKNOWN, path, (uint32_t)strlen(path));
    } else {
        items[n++] = form_keyword(a, SPAN_UNKNOWN, pkg_intern(st, "url"));
        items[n++] = form_str(a, SPAN_UNKNOWN, url ? url : "",
                              (uint32_t)strlen(url ? url : ""));
        if (ref) {
            items[n++] = form_keyword(a, SPAN_UNKNOWN, pkg_intern(st, "ref"));
            items[n++] = form_str(a, SPAN_UNKNOWN, ref, (uint32_t)strlen(ref));
        }
        if (subdir) {
            items[n++] = form_keyword(a, SPAN_UNKNOWN, pkg_intern(st, "subdir"));
            items[n++] = form_str(a, SPAN_UNKNOWN, subdir, (uint32_t)strlen(subdir));
        }
    }
    if (optional) {
        items[n++] = form_keyword(a, SPAN_UNKNOWN, pkg_intern(st, "optional"));
        items[n++] = form_bool(a, SPAN_UNKNOWN, true);
    }
    /* Match the spelling of the enclosing :spices map so one manifest does not
     * end up mixing `#map{...}` and `#{...}` after a `tur add`. */
    return literal ? form_map_literal(a, SPAN_UNKNOWN, items, n)
                   : form_map(a, SPAN_UNKNOWN, items, n);
}

/* Return a new defpackage Form (F_LIST) with a new spice entry appended.
 * If :spices already exists in dp, the new key/val are appended to the map.
 * If :spices is absent, a new :spices #{} map is added. */
static Form *pkg_defpackage_add_spice(Arena *a, SymbolTable *st,
                                       const Form *dp,
                                       const char *spice_name,
                                       const char *url, const char *ref,
                                       const char *path, const char *subdir,
                                       bool optional) {
    Form *entry_key = form_str(a, SPAN_UNKNOWN,
                               spice_name, (uint32_t)strlen(spice_name));
    uint32_t n = dp->as.list.len;

    /* Find :spices keyword index */
    int spices_val_idx = -1;
    for (uint32_t i = 0; i + 1 < n; i++) {
        const Form *kf = dp->as.list.items[i];
        if (kf->tag == F_KEYWORD &&
            strcmp(kf->as.sym->name, "spices") == 0) {
            spices_val_idx = (int)(i + 1);
            break;
        }
    }

    /* Preserve the original form's span so comment gap extraction works. */
    Span orig_span = dp->span;

    if (spices_val_idx >= 0) {
        /* Extend existing :spices map.
         *
         * Both spellings have to be handled here: `#{...}` is F_MAP and
         * `#map{...}` is F_MAP_LITERAL. Reading only F_MAP silently produced
         * map_n == 0, so `tur add` on a `#map{...}`-spelled manifest DROPPED
         * every dependency already declared and wrote back a map holding only
         * the new entry. `#map{...}` is the canonical spelling the guides now
         * use, so that path is the common one, not an exotic case.
         *
         * Rebuild with the tag the manifest already had, so adding a dep does
         * not silently respell the user's `#map{...}` as `#{...}`. */
        const Form *old_map = dp->as.list.items[spices_val_idx];
        bool is_literal = (old_map->tag == F_MAP_LITERAL);
        Form *entry_val = pkg_build_spice_val(a, st, url, ref, path, subdir,
                                              optional, is_literal);
        uint32_t map_n = (old_map->tag == F_MAP || is_literal)
                       ? old_map->as.list.len : 0;
        Form **new_map_items = (Form **)arena_alloc(
                a, (map_n + 2) * sizeof(Form *));
        if (map_n > 0)
            memcpy(new_map_items, old_map->as.list.items,
                   map_n * sizeof(Form *));
        new_map_items[map_n]     = entry_key;
        new_map_items[map_n + 1] = entry_val;
        Form *new_map = is_literal
            ? form_map_literal(a, SPAN_UNKNOWN, new_map_items, map_n + 2)
            : form_map(a, SPAN_UNKNOWN, new_map_items, map_n + 2);

        Form **new_dp = (Form **)arena_alloc(a, n * sizeof(Form *));
        memcpy(new_dp, dp->as.list.items, n * sizeof(Form *));
        new_dp[spices_val_idx] = new_map;
        return form_list(a, orig_span, new_dp, n);
    } else {
        /* Append :spices #{ entry } */
        Form *kw = form_keyword(a, SPAN_UNKNOWN, pkg_intern(st, "spices"));
        Form *entry_val = pkg_build_spice_val(a, st, url, ref, path, subdir,
                                              optional, false);
        Form *new_map_items[2] = { entry_key, entry_val };
        Form *new_map = form_map(a, SPAN_UNKNOWN, new_map_items, 2);

        Form **new_dp = (Form **)arena_alloc(a, (n + 2) * sizeof(Form *));
        memcpy(new_dp, dp->as.list.items, n * sizeof(Form *));
        new_dp[n]     = kw;
        new_dp[n + 1] = new_map;
        return form_list(a, orig_span, new_dp, n + 2);
    }
}

/* Read build.tur, add a spice entry, and write back preserving comments.
 * Returns true on success. */
static bool pkg_build_tur_add_spice(const char *build_path,
                                     const char *spice_name,
                                     const char *url, const char *ref,
                                     const char *path, const char *subdir,
                                     bool optional) {
    /* 1. Read raw source (keep for comment preservation). */
    FILE *f = fopen(build_path, "rb");
    if (!f) {
        fprintf(stderr, "tur add: cannot open '%s': %s\n",
                build_path, strerror(errno));
        return false;
    }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    rewind(f);
    char *src = (char *)malloc((size_t)sz + 1);
    if (!src) { fclose(f); return false; }
    if (fread(src, 1, (size_t)sz, f) != (size_t)sz) {
        fclose(f); free(src); return false;
    }
    fclose(f);
    src[sz] = '\0';

    /* 2. Parse into Form AST. */
    Arena arena;
    arena_init(&arena, 0);
    SymbolTable st;
    symtab_init(&st, &arena);

    SourceFile file = {0};
    file.path    = build_path;
    file.src     = src;
    file.len     = (uint32_t)sz;
    file.file_id = 0;
    diag_register_file(&file);
    diag_reset();

    uint32_t nforms = 0;
    Form **forms = read_all(&arena, &st, &file, &nforms);

    if (!forms || diag_had_error() || nforms == 0) {
        fprintf(stderr, "tur add: failed to parse '%s'\n", build_path);
        free(src);
        symtab_free(&st);
        arena_free(&arena);
        return false;
    }

    /* 3. Find the defpackage form and mutate it. */
    int dp_idx = -1;
    for (uint32_t i = 0; i < nforms; i++) {
        const Form *frm = forms[i];
        if (frm->tag == F_LIST && frm->as.list.len >= 2) {
            const Form *head = frm->as.list.items[0];
            if (head->tag == F_SYM &&
                strcmp(head->as.sym->name, "defpackage") == 0) {
                dp_idx = (int)i;
                break;
            }
        }
    }
    if (dp_idx < 0) {
        fprintf(stderr, "tur add: no (defpackage ...) form in '%s'\n",
                build_path);
        free(src);
        symtab_free(&st);
        arena_free(&arena);
        return false;
    }

    forms[dp_idx] = pkg_defpackage_add_spice(
            &arena, &st, forms[dp_idx],
            spice_name, url, ref, path, subdir, optional);

    /* 4. Re-emit with comment preservation. */
    Buf out;
    buf_init(&out);
    FmtOptions opts = {0};
    opts.indent_width = 2;
    opts.line_width   = 80;
    opts.src          = src;
    opts.src_len      = (size_t)sz;
    if (fmt_print(&out, forms, nforms, opts) != 0) {
        fprintf(stderr, "tur add: formatter failed\n");
        buf_free(&out);
        free(src);
        symtab_free(&st);
        arena_free(&arena);
        return false;
    }

    /* 5. Write atomically (temp file + rename). */
    char tmp_path[4096];
    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp.%d", build_path, (int)getpid());
    FILE *wf = fopen(tmp_path, "wb");
    if (!wf) {
        fprintf(stderr, "tur add: cannot write '%s': %s\n",
                tmp_path, strerror(errno));
        buf_free(&out);
        free(src);
        symtab_free(&st);
        arena_free(&arena);
        return false;
    }
    fwrite(out.data, 1, out.len, wf);
    fclose(wf);

    if (rename(tmp_path, build_path) != 0) {
        fprintf(stderr, "tur add: rename failed: %s\n", strerror(errno));
        unlink(tmp_path);
        buf_free(&out);
        free(src);
        symtab_free(&st);
        arena_free(&arena);
        return false;
    }

    buf_free(&out);
    free(src);
    symtab_free(&st);
    arena_free(&arena);
    return true;
}

/* ================================================================== */
/* CLI: tur add                                                         */
/* ================================================================== */

int cmd_pkg_add(int argc, char **argv) {
    /* Usage: tur add <url-or-path> [--ref <ref>] [--name <name>]
     *                              [--path] [--subdir <dir>] [--optional]
     *        tur add --workspace <name>
     *
     * LS6 (local-spice-dev-workflow): --workspace asserts that <name> is
     * a sibling member of the enclosing workspace and exits without
     * touching build.tur, since workspace siblings are auto-resolved by
     * the LS2/LS4 machinery and do not need a `:spices` entry. */
    const char *url_or_path   = NULL;
    const char *ref           = NULL;
    const char *name_override = NULL;
    const char *subdir        = NULL;
    bool        is_path       = false;
    bool        is_workspace  = false;
    bool        optional      = false;

    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "--ref") == 0 && i + 1 < argc) {
            ref = argv[++i];
        } else if (strcmp(argv[i], "--name") == 0 && i + 1 < argc) {
            name_override = argv[++i];
        } else if (strcmp(argv[i], "--path") == 0) {
            is_path = true;
        } else if (strcmp(argv[i], "--workspace") == 0) {
            is_workspace = true;
        } else if (strcmp(argv[i], "--subdir") == 0 && i + 1 < argc) {
            subdir = argv[++i];
        } else if (strcmp(argv[i], "--optional") == 0) {
            optional = true;
        } else if (argv[i][0] != '-') {
            if (url_or_path) {
                fprintf(stderr, "tur add: unexpected argument '%s'\n", argv[i]);
                return 1;
            }
            url_or_path = argv[i];
        }
    }
    if (!url_or_path) {
        fprintf(stderr, "usage: tur add <url> [--ref <ref>] [--name <name>] [--subdir <dir>]\n"
                        "       tur add <path> --path\n"
                        "       tur add --workspace <name>\n");
        return 1;
    }

    /* LS6: --workspace <name> -- assert membership and exit without
     * modifying build.tur.  Workspace siblings resolve implicitly via
     * the parent manifest's :members list, so no :spices entry is
     * needed for sibling access.  Mixing --workspace with --path /
     * --ref / --url-shaped args is an error. */
    if (is_workspace) {
        if (is_path || ref || subdir || optional || name_override) {
            fprintf(stderr,
                "tur add --workspace: --workspace is exclusive with "
                "--path / --ref / --subdir / --optional / --name\n");
            return 1;
        }
        /* Disallow obvious URL/path values; --workspace takes a bare name. */
        if (strchr(url_or_path, '/') || strchr(url_or_path, ':')) {
            fprintf(stderr,
                "tur add --workspace: expected a bare workspace member "
                "name, got '%s'\n",
                url_or_path);
            return 1;
        }
        {
            char ws_found[64];
            if (!pkg_resolve_manifest_cwd(ws_found, sizeof(ws_found))) {
                fprintf(stderr,
                    "tur add --workspace: no build.tur found in current "
                    "directory; run from inside a workspace member\n");
                return 1;
            }
        }
        if (!pkg_is_workspace_member(".", url_or_path)) {
            fprintf(stderr,
                "tur add --workspace: '%s' is not a member of the "
                "enclosing workspace.\n"
                "  Check the parent build.tur's :members list, or add "
                "'%s' as a member there first.\n",
                url_or_path, url_or_path);
            return 1;
        }
        printf("'%s' is a workspace sibling; no manifest entry needed.\n",
               url_or_path);
        printf("  workspace resolution adds its src/ to the include path "
               "automatically.\n");
        printf("  declare it in :spices later for external (URL) "
               "publication.\n");
        return 0;
    }

    /* Handle spice/<pkg> registry shorthand */
    if (strncmp(url_or_path, "spice/", 6) == 0) {
        fprintf(stderr,
            "The Spice registry is not yet available.\n"
            "Add the package directly with a Git URL:\n"
            "  tur add https://github.com/turmeric-spice/tur-%s --ref v0.1.0\n",
            url_or_path + 6);
        return 1;
    }

    /* Load existing manifest */
    char manifest_path[64];
    if (!pkg_resolve_manifest_cwd(manifest_path, sizeof(manifest_path))) {
        fprintf(stderr,
            "No build.tur found. Run `tur new <name>` to create a project.\n");
        return 1;
    }
    PkgManifest m;
    memset(&m, 0, sizeof(m));
    if (!pkg_manifest_read(manifest_path, &m)) return 1;

    /* Derive package name from URL/path or --name override */
    char name_buf[256];
    if (name_override) {
        strncpy(name_buf, name_override, sizeof(name_buf) - 1);
        name_buf[sizeof(name_buf) - 1] = '\0';
    } else {
        const char *last = strrchr(url_or_path, '/');
        const char *base = last ? last + 1 : url_or_path;
        strncpy(name_buf, base, sizeof(name_buf) - 1);
        name_buf[sizeof(name_buf) - 1] = '\0';
        /* strip .git suffix */
        size_t nl = strlen(name_buf);
        if (nl > 4 && strcmp(name_buf + nl - 4, ".git") == 0)
            name_buf[nl - 4] = '\0';
        /* strip tur- prefix for nicer names */
        if (strncmp(name_buf, "tur-", 4) == 0)
            memmove(name_buf, name_buf + 4, strlen(name_buf) - 3);
    }

    /* Check for duplicate */
    for (int i = 0; i < m.n_spices; i++) {
        if (strcmp(m.spices[i].name, name_buf) == 0) {
            /* There is no `tur update` subcommand -- CANONICAL_COMMANDS has
             * no "update" row (`tur upgrade` is the installed-tool pipeline,
             * a different thing). Point at the flow that actually exists:
             * edit the manifest, then re-resolve. */
            fprintf(stderr,
                "'%s' is already a dependency. "
                "To change its ref, edit the `:spices` entry for '%s' in "
                "build.tur, then run `tur fetch --update`.\n",
                name_buf, name_buf);
            pkg_manifest_free(&m);
            return 1;
        }
    }

    /* Warn when no --ref given for a git dep */
    if (!is_path && !ref) {
        fprintf(stderr,
            "Warning: no --ref specified; will resolve to HEAD.\n"
            "Pin with: tur add %s --ref <tag-or-sha>\n",
            url_or_path);
    }

    /* Write build.tur via form-based round-trip (preserves comments). */
    const char *spice_url  = is_path ? NULL : url_or_path;
    const char *spice_path = is_path ? url_or_path : NULL;
    if (!pkg_build_tur_add_spice(manifest_path, name_buf,
                                  spice_url, ref, spice_path,
                                  is_path ? NULL : subdir, optional)) {
        pkg_manifest_free(&m);
        return 1;
    }

    /* Also track in the manifest struct for the fetch step below. */
    m.spices = (PkgSpice *)realloc(m.spices,
                                    (m.n_spices + 1) * sizeof(PkgSpice));
    if (!m.spices) { pkg_manifest_free(&m); return 1; }
    PkgSpice *ns = &m.spices[m.n_spices++];
    memset(ns, 0, sizeof(*ns));
    ns->name     = tur_strdup(name_buf);
    ns->optional = optional;
    if (is_path) {
        ns->path = tur_strdup(url_or_path);
    } else {
        ns->url = tur_strdup(url_or_path);
        if (ref) ns->ref = tur_strdup(ref);
        if (subdir) ns->subdir = tur_strdup(subdir);
    }

    /* Fetch immediately and update tur.lock */
    if (!is_path) {
        PkgLockFile lock;
        memset(&lock, 0, sizeof(lock));
        lock.format_version = 1;
        pkg_lock_read("tur.lock", &lock);

        bool fetch_ok = pkg_fetch_all(".", &m, &lock, false, NULL);
        if (fetch_ok) {
            pkg_lock_write("tur.lock", &lock);
            /* If no ref was given, report what HEAD resolved to */
            if (!ref) {
                PkgLockEntry *le = pkg_lock_find(&lock, name_buf, false);
                if (le && le->resolved) {
                    fprintf(stderr,
                        "Warning: no --ref specified; resolved to HEAD (%s).\n"
                        "Pin with: tur add %s --ref %.12s\n",
                        le->resolved, url_or_path, le->resolved);
                }
            }
        } else {
            fprintf(stderr,
                "tur add: fetch failed -- build.tur was updated but "
                "tur.lock was not. Run `tur fetch` to retry.\n");
        }
        pkg_lock_free(&lock);
    }

    printf("Added '%s'", name_buf);
    if (ns->url) printf(" -> %s", ns->url);
    if (ns->ref) printf(" @ %s", ns->ref);
    if (ns->path) printf(" (local: %s)", ns->path);
    printf("\n");
    if (!is_path) {
        printf("  build.tur updated\n");
        printf("  tur.lock updated\n");
    }

    pkg_manifest_free(&m);
    return 0;
}

/* ================================================================== */
/* CLI: tur add-cmake                                                   */
/* ================================================================== */

int cmd_pkg_add_cmake(int argc, char **argv) {
    /* Usage: tur add-cmake <url> [--ref <ref>] [--opt KEY=VAL ...] */
    const char *url = NULL;
    const char *ref = NULL;
    /* Options as "KEY=VALUE" strings (max 16) */
    char *opts[16];
    int n_opts = 0;

    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "--ref") == 0 && i + 1 < argc) {
            ref = argv[++i];
        } else if (strcmp(argv[i], "--opt") == 0 && i + 1 < argc) {
            if (n_opts < 16) opts[n_opts++] = argv[++i];
        } else if (argv[i][0] != '-') {
            if (url) {
                fprintf(stderr, "tur add-cmake: unexpected '%s'\n", argv[i]);
                return 1;
            }
            url = argv[i];
        }
    }
    if (!url) {
        fprintf(stderr, "usage: tur add-cmake <url> [--ref <ref>] "
                        "[--opt KEY=VAL]\n");
        return 1;
    }

    char manifest_path[64];
    if (!pkg_resolve_manifest_cwd(manifest_path, sizeof(manifest_path))) {
        fprintf(stderr, "tur add-cmake: no build.tur; run `tur init` first\n");
        return 1;
    }

    PkgManifest m;
    memset(&m, 0, sizeof(m));
    if (!pkg_manifest_read(manifest_path, &m)) return 1;

    /* Derive name from URL */
    char name_buf[256];
    {
        const char *last = strrchr(url, '/');
        const char *base = last ? last + 1 : url;
        strncpy(name_buf, base, sizeof(name_buf) - 1);
        name_buf[sizeof(name_buf) - 1] = '\0';
        size_t nl = strlen(name_buf);
        if (nl > 4 && strcmp(name_buf + nl - 4, ".git") == 0)
            name_buf[nl - 4] = '\0';
    }

    /* Check for duplicate */
    for (int i = 0; i < m.n_cmake_deps; i++) {
        if (strcmp(m.cmake_deps[i].name, name_buf) == 0) {
            fprintf(stderr, "tur add-cmake: '%s' already in build.tur\n",
                    name_buf);
            pkg_manifest_free(&m);
            return 1;
        }
    }

    m.cmake_deps = (PkgCmakeDep *)realloc(m.cmake_deps,
        (m.n_cmake_deps + 1) * sizeof(PkgCmakeDep));
    if (!m.cmake_deps) { pkg_manifest_free(&m); return 1; }
    PkgCmakeDep *nd = &m.cmake_deps[m.n_cmake_deps++];
    memset(nd, 0, sizeof(*nd));
    nd->name = tur_strdup(name_buf);
    nd->url  = tur_strdup(url);
    if (ref) nd->ref = tur_strdup(ref);

    if (n_opts > 0) {
        nd->opts   = (PkgCmakeOpt *)malloc(n_opts * sizeof(PkgCmakeOpt));
        nd->n_opts = 0;
        for (int i = 0; i < n_opts; i++) {
            char *eq = strchr(opts[i], '=');
            if (!eq) continue;
            *eq = '\0';
            nd->opts[nd->n_opts].key = tur_strdup(opts[i]);
            nd->opts[nd->n_opts].val = tur_strdup(eq + 1);
            nd->n_opts++;
            *eq = '=';
        }
    }

    if (!pkg_manifest_write(manifest_path, &m)) {
        pkg_manifest_free(&m);
        return 1;
    }

    printf("Added cmake dep '%s' -> %s", name_buf, url);
    if (ref) printf(" @ %s", ref);
    printf("\n");
    /* The generated file is cmake/CMakeLists.txt; `SpiceDeps` is the name of
     * the cmake project declared inside it, which is what this message used
     * to report as the filename. */
    printf("Run `tur fetch` to generate cmake/CMakeLists.txt.\n");

    pkg_manifest_free(&m);
    return 0;
}

/* ================================================================== */
/* CLI: tur fetch                                                       */
/* ================================================================== */

static int lock_sig_cmp(const void *a, const void *b) {
    return strcmp(*(char *const *)a, *(char *const *)b);
}

/* `tur fetch --frozen`: everything a lock row PINS -- not when it was
 * fetched -- as one string, entries sorted, so a before/after comparison says
 * whether the fetch would have changed the lockfile.  Caller frees. */
static char *lock_pin_signature(const PkgLockFile *lock) {
    int n = lock ? lock->n_entries : 0;
    char **rows = (char **)calloc(n ? (size_t)n : 1, sizeof(char *));
    if (!rows) { fprintf(stderr, "tur: oom\n"); abort(); }
    for (int i = 0; i < n; i++) {
        const PkgLockEntry *e = &lock->entries[i];
        Buf b; buf_init(&b);
        buf_printf(&b, "%s|%d|%s|%s|%s|%s|%s|%s", e->name ? e->name : "",
                   e->is_cmake ? 1 : 0, e->url ? e->url : "", e->ref ? e->ref : "",
                   e->resolved ? e->resolved : "", e->sha256 ? e->sha256 : "",
                   e->resolved_via ? e->resolved_via : "",
                   e->system_version ? e->system_version : "");
        for (int t = 0; t < e->n_transitive; t++)
            buf_printf(&b, "|%s", e->transitive[t] ? e->transitive[t] : "");
        rows[i] = tur_strdup(b.data ? b.data : "");
        buf_free(&b);
    }
    qsort(rows, (size_t)n, sizeof(char *), lock_sig_cmp);
    Buf all; buf_init(&all);
    for (int i = 0; i < n; i++) { buf_puts(&all, rows[i]); buf_putc(&all, '\n'); free(rows[i]); }
    free(rows);
    char *r = tur_strdup(all.data ? all.data : "");
    buf_free(&all);
    return r;
}

int cmd_pkg_fetch(int argc, char **argv) {
    bool update  = false;
    bool dry_run = false;
    bool refetch = false;
    bool frozen  = false;
    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "--update") == 0) update = true;
        else if (strcmp(argv[i], "--dry-run") == 0) dry_run = true;
        else if (strcmp(argv[i], "--refetch") == 0) refetch = true;
        else if (strcmp(argv[i], "--frozen") == 0) frozen = true;
    }
    /* --frozen (the `npm ci` / `cargo --locked` shape): fetch exactly what
     * tur.lock pins and fail if anything would change it -- a dep with no row,
     * a moved :ref, a different commit or tree.  The lock is never written.
     * What CI should run.  lock-tracks-ref-not-resolved-commit. */
    if (frozen && update) {
        fprintf(stderr, "tur fetch: --frozen and --update contradict each other\n");
        return 2;
    }
    /* SF3: --refetch forces the source-build path for :prefer-system deps,
     * bypassing any system find_package copy. pkg_cmake_build reads this env
     * var and passes -DTUR_FETCH_FORCE_FETCH=ON to the cmake configure. */
    if (refetch) setenv("TUR_FETCH_FORCE_FETCH", "1", 1);

    char manifest_path[64];
    if (!pkg_resolve_manifest_cwd(manifest_path, sizeof(manifest_path))) {
        fprintf(stderr, "tur fetch: no build.tur found in current directory\n");
        return 2;
    }

    PkgManifest m;
    memset(&m, 0, sizeof(m));
    if (!pkg_manifest_read(manifest_path, &m)) return 2;

    /* LS6 (local-spice-dev-workflow): --dry-run classifies each direct
     * dep without performing any fetches or touching tur.lock.  Useful
     * for "did I wire this workspace up right?" sanity checks. */
    if (dry_run) {
        printf("spice: dry-run (no fetches will be performed, "
               "tur.lock untouched)\n");
        int n_url = 0, n_path = 0, n_ws = 0, n_cmake = 0;
        for (int i = 0; i < m.n_spices; i++) {
            const PkgSpice *s = &m.spices[i];
            if (s->path) {
                printf("  skip   :path             %-20s %s\n",
                       s->name, s->path);
                n_path++;
            } else if (pkg_is_workspace_member(".", s->name)) {
                char *wp = pkg_workspace_member_path(".", s->name);
                printf("  skip   workspace member  %-20s %s\n",
                       s->name, wp ? wp : "(unknown path)");
                free(wp);
                n_ws++;
            } else {
                printf("  fetch  URL               %-20s %s%s%s\n",
                       s->name,
                       s->url ? s->url : "(no url)",
                       s->ref ? " @ "    : "",
                       s->ref ? s->ref   : "");
                n_url++;
            }
        }
        for (int i = 0; i < m.n_cmake_deps; i++) {
            const PkgCmakeDep *d = &m.cmake_deps[i];
            printf("  fetch  cmake             %-20s %s%s%s\n",
                   d->name,
                   d->url ? d->url : "(no url)",
                   d->ref ? " @ "    : "",
                   d->ref ? d->ref   : "");
            n_cmake++;
        }
        printf("summary: %d to fetch (%d URL, %d cmake), "
               "%d skipped (%d :path, %d workspace member)\n",
               n_url + n_cmake, n_url, n_cmake,
               n_path + n_ws, n_path, n_ws);
        pkg_manifest_free(&m);
        return 0;
    }

    PkgLockFile lock;
    memset(&lock, 0, sizeof(lock));
    lock.format_version = 1;
    /* Load existing lock (if any) */
    bool had_lock = pkg_lock_read("tur.lock", &lock);
    if (frozen && !had_lock) {
        fprintf(stderr, "tur fetch --frozen: no tur.lock to hold the fetch to\n");
        pkg_lock_free(&lock);
        pkg_manifest_free(&m);
        return 2;
    }
    char *frozen_sig = frozen ? lock_pin_signature(&lock) : NULL;

    /* tur-fetch-exit-code-optional-vs-required: exit status contract --
     *   0  everything fetched (or cached);
     *   1  only `:optional` deps failed -- the lock is written and the
     *      project is buildable without them;
     *   2  a REQUIRED dep or step failed (a :spices entry, a :cmake-deps
     *      build, the lock write, the manifest itself).
     * CI can then warn-and-continue on 1 and fail on 2 instead of guessing. */
    bool optional_failed = false;
    bool ok = pkg_fetch_all(".", &m, &lock, update, &optional_failed);

    /* cmake deps: generate cmake/CMakeLists.txt, then configure+build.
     * transitive-cmake-deps-plan: union the enclosing manifest's :cmake-deps
     * with any declared transitively through its :spices closure, so
     * `tur fetch` mirrors `tur run`'s view of the dep set. Both use the
     * declared closure -- see pkg_workspace_wide_cmake_deps(). */
    PkgCmakeDep *fetch_deps   = NULL;
    int          n_fetch_deps = 0;
    if (!pkg_collect_transitive_cmake_deps(".", &m,
                                           pkg_workspace_wide_cmake_deps(),
                                           &fetch_deps, &n_fetch_deps)) {
        fprintf(stderr,
                "spice: transitive cmake-deps resolution failed\n");
        ok = false;
    } else if (n_fetch_deps > 0) {
        /* Alias `m` and swap in the unioned cmake_deps so the existing
         * generator/builder pair sees the full set. */
        PkgManifest mu = m;
        mu.cmake_deps   = fetch_deps;
        mu.n_cmake_deps = n_fetch_deps;
        /* On re-fetch without --update, verify existing SHAs first */
        if (!update) {
            if (!pkg_cmake_verify_lock(".", &lock)) {
                fprintf(stderr,
                    "spice: SHA mismatch detected -- run `tur fetch --update`"
                    " to re-fetch cmake deps\n");
                ok = false;
            }
        }
        if (ok) {
            if (!pkg_gen_cmake_deps(".", &mu)) {
                ok = false;
            } else if (!pkg_cmake_build(".", &mu, &lock, NULL)) {
                ok = false;
            }
        }
    }
    pkg_cmake_deps_free(fetch_deps, n_fetch_deps);

    if (frozen) {
        char *after = lock_pin_signature(&lock);
        if (strcmp(after, frozen_sig) != 0) {
            fprintf(stderr,
                "tur fetch --frozen: tur.lock would change.  A dependency has no\n"
                "  row, or its :url / :ref / commit / tree differs from the pin.\n"
                "  Run `tur fetch` (or `tur fetch --update`) and commit the lock.\n");
            ok = false;
        }
        free(after);
        free(frozen_sig);
    } else
    /* Write updated lock file */
    if (!pkg_lock_write("tur.lock", &lock)) ok = false;

    pkg_lock_free(&lock);
    pkg_manifest_free(&m);

    if (!ok) {
        fprintf(stderr, "spice: fetch completed with errors\n");
        return 2;
    }
    printf(frozen ? "spice: matches tur.lock (frozen)\n"
                  : "spice: lock file written to tur.lock\n");
    if (optional_failed) {
        fprintf(stderr,
                "spice: fetch completed; one or more optional deps could not "
                "be fetched (exit 1)\n");
        return 1;
    }
    return 0;
}

/* ================================================================== */
/* CLI: tur emit-cmake (Phase B)                                       */
/* ================================================================== */

/* FindTurmeric.cmake -- distributed with tur, placed in cmake/ by emit-cmake */
static const char FIND_TURMERIC_CMAKE[] =
"# FindTurmeric.cmake -- distributed with the Turmeric toolchain.\n"
"#\n"
"# Imported targets:\n"
"#   Turmeric::Compiler -- the tur binary as an imported executable\n"
"#\n"
"# Variables set:\n"
"#   Turmeric_FOUND, Turmeric_VERSION, Turmeric_COMPILER\n"
"\n"
"find_program(Turmeric_COMPILER NAMES tur\n"
"  PATHS ENV PATH /usr/local/bin /opt/homebrew/bin\n"
"  DOC \"Path to the Turmeric compiler\"\n"
")\n"
"\n"
"if(Turmeric_COMPILER)\n"
"  execute_process(\n"
"    COMMAND \"${Turmeric_COMPILER}\" --version\n"
"    OUTPUT_VARIABLE Turmeric_VERSION_STRING\n"
"    OUTPUT_STRIP_TRAILING_WHITESPACE\n"
"    ERROR_QUIET\n"
"  )\n"
"  string(REGEX MATCH \"[0-9]+\\\\.[0-9]+\\\\.[0-9]+\" Turmeric_VERSION\n"
"         \"${Turmeric_VERSION_STRING}\")\n"
"endif()\n"
"\n"
"include(FindPackageHandleStandardArgs)\n"
"find_package_handle_standard_args(Turmeric\n"
"  REQUIRED_VARS Turmeric_COMPILER\n"
"  VERSION_VAR   Turmeric_VERSION\n"
")\n"
"\n"
"if(Turmeric_FOUND AND NOT TARGET Turmeric::Compiler)\n"
"  add_executable(Turmeric::Compiler IMPORTED)\n"
"  set_target_properties(Turmeric::Compiler PROPERTIES\n"
"    IMPORTED_LOCATION \"${Turmeric_COMPILER}\"\n"
"  )\n"
"endif()\n";

/* AddTurmericTarget.cmake -- distributed with tur, placed in cmake/ by emit-cmake */
static const char ADD_TURMERIC_TARGET_CMAKE[] =
"# AddTurmericTarget.cmake -- helper functions for CMake projects using Turmeric.\n"
"#\n"
"# Functions:\n"
"#   add_turmeric_library(<name> SOURCES <...> [DEPENDS <targets...>])\n"
"#   add_turmeric_executable(<name> SOURCES <...> [LIBRARIES <targets...>])\n"
"\n"
"include_guard(GLOBAL)\n"
"find_package(Turmeric REQUIRED)\n"
"\n"
"# add_turmeric_library(<name> SOURCES <files...> [DEPENDS <targets...>])\n"
"function(add_turmeric_library name)\n"
"  cmake_parse_arguments(ATL \"\" \"\" \"SOURCES;DEPENDS\" ${ARGN})\n"
"  if(NOT ATL_SOURCES)\n"
"    message(FATAL_ERROR \"add_turmeric_library: SOURCES is required\")\n"
"  endif()\n"
"\n"
"  # Derive the list of expected .h and .c outputs\n"
"  set(_out_h_files)\n"
"  set(_out_c_files)\n"
"  foreach(_src IN LISTS ATL_SOURCES)\n"
"    get_filename_component(_mod \"${_src}\" NAME_WE)\n"
"    list(APPEND _out_h_files \"${CMAKE_CURRENT_BINARY_DIR}/${_mod}.h\")\n"
"    list(APPEND _out_c_files \"${CMAKE_CURRENT_BINARY_DIR}/${_mod}.c\")\n"
"  endforeach()\n"
"\n"
"  add_custom_command(\n"
"    OUTPUT  ${_out_h_files} ${_out_c_files}\n"
"    COMMAND Turmeric::Compiler emit-c\n"
"            --output-dir \"${CMAKE_CURRENT_BINARY_DIR}\"\n"
"            ${ATL_SOURCES}\n"
"    DEPENDS ${ATL_SOURCES}\n"
"    COMMENT \"Compiling Turmeric library ${name}\"\n"
"  )\n"
"\n"
"  add_library(${name} STATIC ${_out_c_files})\n"
"  target_include_directories(${name} PUBLIC \"${CMAKE_CURRENT_BINARY_DIR}\")\n"
"\n"
"  if(ATL_DEPENDS)\n"
"    target_link_libraries(${name} PUBLIC ${ATL_DEPENDS})\n"
"  endif()\n"
"\n"
"  add_library(${name}::${name} ALIAS ${name})\n"
"endfunction()\n"
"\n"
"# add_turmeric_executable(<name> SOURCES <files...> [LIBRARIES <targets...>])\n"
"function(add_turmeric_executable name)\n"
"  cmake_parse_arguments(ATE \"\" \"\" \"SOURCES;LIBRARIES\" ${ARGN})\n"
"  if(NOT ATE_SOURCES)\n"
"    message(FATAL_ERROR \"add_turmeric_executable: SOURCES is required\")\n"
"  endif()\n"
"\n"
"  set(_out_h_files)\n"
"  set(_out_c_files)\n"
"  foreach(_src IN LISTS ATE_SOURCES)\n"
"    get_filename_component(_mod \"${_src}\" NAME_WE)\n"
"    list(APPEND _out_h_files \"${CMAKE_CURRENT_BINARY_DIR}/${_mod}.h\")\n"
"    list(APPEND _out_c_files \"${CMAKE_CURRENT_BINARY_DIR}/${_mod}.c\")\n"
"  endforeach()\n"
"\n"
"  add_custom_command(\n"
"    OUTPUT  ${_out_h_files} ${_out_c_files}\n"
"    COMMAND Turmeric::Compiler emit-c\n"
"            --output-dir \"${CMAKE_CURRENT_BINARY_DIR}\"\n"
"            ${ATE_SOURCES}\n"
"    DEPENDS ${ATE_SOURCES}\n"
"    COMMENT \"Compiling Turmeric executable ${name}\"\n"
"  )\n"
"\n"
"  add_executable(${name} ${_out_c_files})\n"
"\n"
"  if(ATE_LIBRARIES)\n"
"    target_link_libraries(${name} PRIVATE ${ATE_LIBRARIES})\n"
"  endif()\n"
"endfunction()\n";

/* Collect .tur files from src/ when :exports is absent.
 * Returns a malloc'd array of heap-alloc'd paths; *n_out = count.
 * Caller must free each string and the array. */
static char **collect_exports_from_src(const char *project_dir, int *n_out) {
    *n_out = 0;
    char src_dir[4096];
    snprintf(src_dir, sizeof(src_dir), "%s/src", project_dir);

    DIR *d = opendir(src_dir);
    if (!d) return NULL;

    int cap = 8;
    char **files = (char **)malloc(cap * sizeof(char *));
    if (!files) { closedir(d); return NULL; }

    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
#if defined(_DIRENT_HAVE_D_TYPE) || defined(__APPLE__) || defined(__FreeBSD__)
        if (ent->d_type != DT_REG && ent->d_type != DT_UNKNOWN) continue;
#endif
        size_t len = strlen(ent->d_name);
        if (len < 4 || strcmp(ent->d_name + len - 4, ".tur") != 0) continue;

        if (*n_out >= cap) {
            cap *= 2;
            files = (char **)realloc(files, cap * sizeof(char *));
            if (!files) { closedir(d); return NULL; }
        }
        char full[4096];
        snprintf(full, sizeof(full), "src/%s", ent->d_name);
        files[(*n_out)++] = tur_strdup(full);
    }
    closedir(d);
    return files;
}

/* Write a text file atomically.  Returns true on success. */
static bool write_file_atomic(const char *path, const char *content) {
    char tmp[4096];
    snprintf(tmp, sizeof(tmp), "%s.tmp.%d", path, (int)getpid());
    FILE *f = fopen(tmp, "w");
    if (!f) {
        fprintf(stderr, "tur emit-cmake: cannot write '%s': %s\n",
                path, strerror(errno));
        return false;
    }
    fputs(content, f);
    fclose(f);
    if (rename(tmp, path) != 0) {
        fprintf(stderr, "tur emit-cmake: rename failed: %s\n", strerror(errno));
        unlink(tmp);
        return false;
    }
    return true;
}

/* ------------------------------------------------------------------ */
/* CLI: tur audit                                                       */
/* ------------------------------------------------------------------ */

/* Look up a dep by name in tur.lock.  Returns NULL when the lock has no
 * entry -- which is normal for :path and workspace deps (they resolve from
 * local source and never produce a lock row) and a finding for a :url dep
 * (it means the tree has never been fetched, so nothing is pinned). */
static const PkgLockEntry *audit_lock_entry(const PkgLockFile *lock,
                                            const char *name, bool cmake) {
    if (!lock) return NULL;
    for (int i = 0; i < lock->n_entries; i++) {
        if (lock->entries[i].is_cmake != cmake) continue;
        if (lock->entries[i].name && strcmp(lock->entries[i].name, name) == 0)
            return &lock->entries[i];
    }
    return NULL;
}

/* Print one origin row plus, when it exists, the pin from tur.lock. */
static void audit_print_origin(const char *kind, const char *name,
                               const char *url, const char *ref,
                               const char *path, const char *subdir,
                               const PkgLockEntry *le, bool *any_unpinned) {
    if (path) {
        /* Local source: no download, no hash, and nothing to pin. The trust
         * boundary is the filesystem, which the user already controls. */
        printf("  %-6s %-22s local path  %s\n", kind, name, path);
        return;
    }
    printf("  %-6s %-22s %s", kind, name, url ? url : "(no url)");
    if (subdir) printf("  subdir=%s", subdir);
    printf("\n");
    printf("         %-22s ref %s\n", "", ref ? ref : "(unpinned)");
    if (le) {
        if (le->resolved)
            printf("         %-22s commit %s\n", "", le->resolved);
        if (le->sha256)
            printf("         %-22s sha256 %s\n", "", le->sha256);
        if (le->resolved_via && strcmp(le->resolved_via, "system") == 0)
            printf("         %-22s via system package%s%s\n", "",
                   le->system_version ? " " : "",
                   le->system_version ? le->system_version : "");
    } else {
        printf("         %-22s NOT IN tur.lock -- run `tur fetch` to pin it\n", "");
        if (any_unpinned) *any_unpinned = true;
    }
}

/* `tur audit` -- list every origin this project will fetch code from.
 *
 * The security section of consuming-spices-guide.md promised this. Its point
 * is stated there: a :cmake-deps entry is a trust decision equivalent to
 * executing build scripts from that repository. This command answers "what am
 * I trusting?" in one place, rather than making the reader reconstruct it from
 * build.tur and tur.lock by hand.
 *
 * It deliberately does NOT verify anything. The guide's original wording also
 * promised maintainer GPG keys; there is no key infrastructure in the tree to
 * check against, and printing an unverified key would be worse than printing
 * none. What this reports is exactly what the manifest and lock say. */
int cmd_pkg_audit(int argc, char **argv) {
    (void)argc; (void)argv;

    char manifest_path[64];
    if (!pkg_resolve_manifest_cwd(manifest_path, sizeof(manifest_path))) {
        fprintf(stderr, "tur audit: no build.tur found in current directory\n");
        return 1;
    }

    PkgManifest m;
    memset(&m, 0, sizeof(m));
    if (!pkg_manifest_read(manifest_path, &m)) return 1;

    PkgLockFile lock;
    memset(&lock, 0, sizeof(lock));
    bool have_lock = pkg_lock_read("tur.lock", &lock);

    printf("audit: %s\n", m.name ? m.name : "(unnamed package)");
    if (!have_lock)
        printf("  (no tur.lock -- nothing is pinned yet; run `tur fetch`)\n");

    bool any_unpinned = false;

    printf("\nTurmeric spices (%d):\n", m.n_spices);
    if (m.n_spices == 0) printf("  (none)\n");
    for (int i = 0; i < m.n_spices; i++) {
        const PkgSpice *sp = &m.spices[i];
        audit_print_origin("spice", sp->name, sp->url, sp->ref, sp->path,
                           sp->subdir,
                           audit_lock_entry(have_lock ? &lock : NULL,
                                            sp->name, false),
                           &any_unpinned);
    }

    printf("\nCMake dependencies (%d) -- each executes build scripts from its "
           "repository:\n", m.n_cmake_deps);
    if (m.n_cmake_deps == 0) printf("  (none)\n");
    for (int i = 0; i < m.n_cmake_deps; i++) {
        const PkgCmakeDep *cd = &m.cmake_deps[i];
        if (cd->prefer_system && !cd->url && !cd->path) {
            /* System-only: no download and no build script from anywhere but
             * the system, so nothing for tur.lock to pin -- reporting it as
             * unpinned would nag about a step `tur fetch` can never take. */
            printf("  %-6s %-22s system only  find_package(%s)\n", "cmake",
                   cd->name, cd->cmake_name);
            continue;
        }
        audit_print_origin("cmake", cd->name, cd->url, cd->ref, cd->path, NULL,
                           audit_lock_entry(have_lock ? &lock : NULL,
                                            cd->name, true),
                           &any_unpinned);
        if (cd->prefer_system)
            printf("         %-22s prefer-system: a system copy is used when "
                   "found, bypassing the pin above\n", "");
    }

    if (any_unpinned)
        printf("\nSome origins are not pinned in tur.lock. Run `tur fetch` so "
               "each resolves to a recorded commit and hash.\n");

    /* C-3: audit VERIFIES now.  It used to close with "this lists origins; it
     * verifies nothing", which the consuming guide reproduced -- so the one
     * command named "audit" was the one that checked least.  It re-hashes
     * every spice tree that is present and pinned, and compares. */
    printf("\nIntegrity:\n");
    bool spices_ok = have_lock
                   ? pkg_verify_locked_spices(".", &m, &lock, "tur audit")
                   : true;
    bool cmake_ok  = have_lock ? pkg_cmake_verify_lock(".", &lock) : true;
    if (!have_lock) {
        printf("  (no tur.lock -- nothing to verify against)\n");
    } else if (spices_ok && cmake_ok) {
        printf("  every present, pinned dependency matches tur.lock.\n");
    } else {
        printf("  MISMATCH -- see above.\n");
    }

    printf("\nWhat this does and does not prove: the hashes above say the trees\n"
           "on disk are the ones tur.lock recorded when they were fetched. They\n"
           "are trust-on-first-use, not a signature -- nothing here authenticates\n"
           "an origin, and `tur.lock` pins content rather than checking out a\n"
           "recorded commit (:ref is what a clone tracks). Prefer a tag over a\n"
           "branch for :ref, and read a new spice before adding it.\n");

    pkg_lock_free(&lock);
    pkg_manifest_free(&m);
    return 0;
}

int cmd_pkg_emit_cmake(int argc, char **argv) {
    /* Usage: tur emit-cmake [--output-dir <dir>]
     * Reads build.tur in the current directory and generates:
     *   CMakeLists.txt
     *   TurmericConfig.cmake
     *   cmake/FindTurmeric.cmake
     *   cmake/AddTurmericTarget.cmake */
    const char *output_dir = ".";

    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "--output-dir") == 0 && i + 1 < argc) {
            output_dir = argv[++i];
        } else if (argv[i][0] == '-') {
            fprintf(stderr, "tur emit-cmake: unknown flag '%s'\n", argv[i]);
            return 1;
        }
    }

    char manifest_path[64];
    if (!pkg_resolve_manifest_cwd(manifest_path, sizeof(manifest_path))) {
        fprintf(stderr,
            "tur emit-cmake: no build.tur found in current directory\n"
            "  Run `tur init --lib <name>` to create a library project.\n");
        return 1;
    }

    PkgManifest m;
    memset(&m, 0, sizeof(m));
    if (!pkg_manifest_read(manifest_path, &m)) return 1;

    if (!m.name) {
        fprintf(stderr, "tur emit-cmake: build.tur has no :name field\n");
        pkg_manifest_free(&m);
        return 1;
    }

    const char *version = m.version ? m.version : "0.0.0";

    /* Collect source files from :exports or scan src/ */
    char **sources = NULL;
    int    n_sources = 0;
    bool   sources_heap = false;

    if (m.n_exports > 0) {
        /* :exports entries are either module names ("scscm/lexer", from the
         * canonical map form) or source paths ("src/lib.tur", legacy vector
         * form). Normalize both to a source path so the generated CMake
         * references real files. */
        sources      = (char **)malloc((size_t)m.n_exports * sizeof(char *));
        sources_heap = true;
        n_sources    = 0;
        if (sources) {
            for (int i = 0; i < m.n_exports; i++) {
                const char *e = m.exports[i];
                size_t el = strlen(e);
                char buf[4096];
                if (el >= 4 && strcmp(e + el - 4, ".tur") == 0)
                    snprintf(buf, sizeof(buf), "%s", e);
                else
                    snprintf(buf, sizeof(buf), "src/%s.tur", e);
                sources[n_sources++] = tur_strdup(buf);
            }
        }
    } else {
        sources       = collect_exports_from_src(".", &n_sources);
        sources_heap  = true;
        if (!sources || n_sources == 0) {
            fprintf(stderr,
                "tur emit-cmake: no source files found.\n"
                "  Add a :exports [\"src/lib.tur\"] block to build.tur or\n"
                "  create .tur files in src/.\n");
            pkg_manifest_free(&m);
            return 1;
        }
    }

    /* Ensure cmake/ subdirectory exists */
    char cmake_subdir[4096];
    snprintf(cmake_subdir, sizeof(cmake_subdir), "%s/cmake", output_dir);
    if (!mkdirp(cmake_subdir)) {
        fprintf(stderr, "tur emit-cmake: cannot create '%s'\n", cmake_subdir);
        if (sources_heap) {
            for (int i = 0; i < n_sources; i++) free(sources[i]);
            free(sources);
        }
        pkg_manifest_free(&m);
        return 1;
    }

    /* Build the CMakeLists.txt content */
    Buf cml;
    buf_init(&cml);
    buf_printf(&cml,
        "# CMakeLists.txt -- AUTO-GENERATED by `tur emit-cmake`. Do not edit.\n"
        "# Re-generate with: tur emit-cmake\n"
        "cmake_minimum_required(VERSION 3.20)\n"
        "project(%s VERSION %s LANGUAGES C)\n"
        "\n"
        "list(APPEND CMAKE_MODULE_PATH \"${CMAKE_CURRENT_SOURCE_DIR}/cmake\")\n"
        "find_package(Turmeric REQUIRED)\n"
        "\n",
        m.name, version);

    /* Collect output file lists for the custom command */
    Buf out_files, dep_files, src_args;
    buf_init(&out_files);
    buf_init(&dep_files);
    buf_init(&src_args);

    for (int i = 0; i < n_sources; i++) {
        /* Extract module name (basename without .tur) */
        const char *src = sources[i];
        const char *base = strrchr(src, '/');
        base = base ? base + 1 : src;
        size_t blen = strlen(base);
        char mod[256];
        size_t mlen = (blen >= 4 && strcmp(base + blen - 4, ".tur") == 0)
                      ? blen - 4 : blen;
        if (mlen >= sizeof(mod)) mlen = sizeof(mod) - 1;
        memcpy(mod, base, mlen);
        mod[mlen] = '\0';

        buf_printf(&out_files,
            "          \"${CMAKE_CURRENT_BINARY_DIR}/%s.h\"\n"
            "          \"${CMAKE_CURRENT_BINARY_DIR}/%s.c\"\n",
            mod, mod);

        buf_printf(&dep_files,
            "          \"${CMAKE_CURRENT_SOURCE_DIR}/%s\"\n", src);

        buf_printf(&src_args,
            "              \"${CMAKE_CURRENT_SOURCE_DIR}/%s\"\n", src);
    }

    /* add_custom_command block */
    buf_printf(&cml,
        "add_custom_command(\n"
        "  OUTPUT\n"
        "%s"
        "  COMMAND Turmeric::Compiler emit-c\n"
        "              --output-dir \"${CMAKE_CURRENT_BINARY_DIR}\"\n"
        "%s"
        "  DEPENDS\n"
        "%s"
        "  COMMENT \"Compiling Turmeric library %s\"\n"
        ")\n\n",
        out_files.data, src_args.data, dep_files.data, m.name);

    buf_free(&out_files);
    buf_free(&dep_files);

    /* add_library: list all .c files */
    buf_printf(&cml, "add_library(%s STATIC\n", m.name);
    for (int i = 0; i < n_sources; i++) {
        const char *src = sources[i];
        const char *base = strrchr(src, '/');
        base = base ? base + 1 : src;
        size_t blen = strlen(base);
        char mod[256];
        size_t mlen = (blen >= 4 && strcmp(base + blen - 4, ".tur") == 0)
                      ? blen - 4 : blen;
        if (mlen >= sizeof(mod)) mlen = sizeof(mod) - 1;
        memcpy(mod, base, mlen);
        mod[mlen] = '\0';
        buf_printf(&cml,
            "  \"${CMAKE_CURRENT_BINARY_DIR}/%s.c\"\n", mod);
    }
    buf_free(&src_args);

    buf_printf(&cml,
        ")\n"
        "target_include_directories(%s\n"
        "  PUBLIC\n"
        "    \"$<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}>\"\n"
        "    \"$<INSTALL_INTERFACE:include>\"\n"
        ")\n"
        "add_library(%s::all ALIAS %s)\n"
        "\n"
        "# Install rules\n"
        "install(TARGETS %s\n"
        "  EXPORT %s-targets\n"
        "  ARCHIVE DESTINATION lib\n"
        ")\n",
        m.name, m.name, m.name, m.name, m.name);

    /* install headers */
    for (int i = 0; i < n_sources; i++) {
        const char *src = sources[i];
        const char *base = strrchr(src, '/');
        base = base ? base + 1 : src;
        size_t blen = strlen(base);
        char mod[256];
        size_t mlen = (blen >= 4 && strcmp(base + blen - 4, ".tur") == 0)
                      ? blen - 4 : blen;
        if (mlen >= sizeof(mod)) mlen = sizeof(mod) - 1;
        memcpy(mod, base, mlen);
        mod[mlen] = '\0';
        buf_printf(&cml,
            "install(FILES \"${CMAKE_CURRENT_BINARY_DIR}/%s.h\"\n"
            "  DESTINATION include\n"
            ")\n", mod);
    }

    buf_printf(&cml,
        "install(EXPORT %s-targets\n"
        "  FILE %s-targets.cmake\n"
        "  NAMESPACE %s::\n"
        "  DESTINATION lib/cmake/%s\n"
        ")\n"
        "install(FILES cmake/FindTurmeric.cmake cmake/AddTurmericTarget.cmake\n"
        "  DESTINATION lib/cmake/%s\n"
        ")\n",
        m.name, m.name, m.name, m.name, m.name);

    /* Write CMakeLists.txt */
    char cml_path[4096];
    snprintf(cml_path, sizeof(cml_path), "%s/CMakeLists.txt", output_dir);
    buf_putc(&cml, '\0');
    if (!write_file_atomic(cml_path, cml.data)) {
        buf_free(&cml);
        if (sources_heap) {
            for (int i = 0; i < n_sources; i++) free(sources[i]);
            free(sources);
        }
        pkg_manifest_free(&m);
        return 1;
    }
    buf_free(&cml);
    printf("  %s\n", cml_path);

    /* Build TurmericConfig.cmake content */
    Buf cfg;
    buf_init(&cfg);
    buf_printf(&cfg,
        "# %sConfig.cmake -- AUTO-GENERATED by `tur emit-cmake`. Do not edit.\n"
        "set(%s_VERSION %s)\n"
        "set(%s_FOUND   TRUE)\n"
        "include(\"${CMAKE_CURRENT_LIST_DIR}/%s-targets.cmake\")\n",
        m.name, m.name, version, m.name, m.name);
    buf_putc(&cfg, '\0');

    char cfg_path[4096];
    snprintf(cfg_path, sizeof(cfg_path), "%s/%sConfig.cmake", output_dir, m.name);
    if (!write_file_atomic(cfg_path, cfg.data)) {
        buf_free(&cfg);
        if (sources_heap) {
            for (int i = 0; i < n_sources; i++) free(sources[i]);
            free(sources);
        }
        pkg_manifest_free(&m);
        return 1;
    }
    buf_free(&cfg);
    printf("  %s\n", cfg_path);

    /* Write cmake/FindTurmeric.cmake */
    char find_path[4096];
    snprintf(find_path, sizeof(find_path), "%s/FindTurmeric.cmake", cmake_subdir);
    if (!write_file_atomic(find_path, FIND_TURMERIC_CMAKE)) {
        if (sources_heap) {
            for (int i = 0; i < n_sources; i++) free(sources[i]);
            free(sources);
        }
        pkg_manifest_free(&m);
        return 1;
    }
    printf("  %s\n", find_path);

    /* Write cmake/AddTurmericTarget.cmake */
    char add_path[4096];
    snprintf(add_path, sizeof(add_path),
             "%s/AddTurmericTarget.cmake", cmake_subdir);
    if (!write_file_atomic(add_path, ADD_TURMERIC_TARGET_CMAKE)) {
        if (sources_heap) {
            for (int i = 0; i < n_sources; i++) free(sources[i]);
            free(sources);
        }
        pkg_manifest_free(&m);
        return 1;
    }
    printf("  %s\n", add_path);

    /* Save copies before freeing manifest */
    char *pkg_name    = tur_strdup(m.name);
    char *pkg_version = tur_strdup(version);

    if (sources_heap) {
        for (int i = 0; i < n_sources; i++) free(sources[i]);
        free(sources);
    }
    pkg_manifest_free(&m);

    printf("\nGenerated cmake files for '%s' v%s.\n", pkg_name, pkg_version);
    printf("\nTo use from a CMake project via CPM:\n");
    printf("  CPMAddPackage(\n");
    printf("    NAME    %s\n", pkg_name);
    printf("    URL     https://github.com/<user>/%s/archive/refs/tags/v%s.tar.gz\n",
           pkg_name, pkg_version);
    printf("    VERSION %s\n", pkg_version);
    printf("  )\n");
    printf("  target_link_libraries(my_target PRIVATE %s::all)\n", pkg_name);

    free(pkg_name);
    free(pkg_version);
    return 0;
}
