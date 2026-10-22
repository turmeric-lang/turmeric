/*
 * global.c -- per-user (global) spice install support.
 *
 * See global.h for the public API.
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

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "arena.h"
#include "platform_proc.h"
#include "source_literal.h"
#include "buf.h"
#include "diag.h"
#include "forms.h"
#include "global.h"
#include "reader.h"
#include "symbols.h"
#include "platform_fs.h"
#include "pkg.h"           /* tur_spice_modules reads :bin */

/* ------------------------------------------------------------------ */
/* Local helpers                                                       */
/* ------------------------------------------------------------------ */

static char *gs_ss_dup(StrSlice s) {
    char *out = (char *)malloc(s.len + 1);
    if (!out) return NULL;
    memcpy(out, s.p, s.len);
    out[s.len] = '\0';
    return out;
}

__attribute__((unused))
static const char *gs_iso_now(void) {
    static char buf[32];
    time_t t = time(NULL);
    struct tm *tm = gmtime(&t);
    strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", tm);
    return buf;
}

/* Recursive mkdir. */
static bool gs_mkdirp(const char *path) {
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

/* ------------------------------------------------------------------ */
/* Path resolution                                                     */
/* ------------------------------------------------------------------ */

bool tur_global_data_dir(char *out, size_t out_sz) {
    const char *th = getenv("TUR_HOME");
    if (th && th[0]) {
        snprintf(out, out_sz, "%s", th);
        return true;
    }
    const char *xdg = getenv("XDG_DATA_HOME");
    if (xdg && xdg[0]) {
        snprintf(out, out_sz, "%s/turmeric", xdg);
        return true;
    }
    const char *home = getenv("HOME");
    if (!home || !home[0]) {
        fprintf(stderr, "tur: $HOME is not set; cannot locate data dir\n");
        return false;
    }
    snprintf(out, out_sz, "%s/.local/share/turmeric", home);
    return true;
}

bool tur_global_bin_dir(char *out, size_t out_sz) {
    const char *th = getenv("TUR_HOME");
    if (th && th[0]) {
        snprintf(out, out_sz, "%s/bin", th);
        return true;
    }
    const char *home = getenv("HOME");
    if (!home || !home[0]) {
        fprintf(stderr, "tur: $HOME is not set; cannot locate bin dir\n");
        return false;
    }
    snprintf(out, out_sz, "%s/.local/bin", home);
    return true;
}

bool tur_global_spices_dir(char *out, size_t out_sz) {
    char data[4096];
    if (!tur_global_data_dir(data, sizeof(data))) return false;
    snprintf(out, out_sz, "%s/spices", data);
    return true;
}

bool tur_global_cache_dir(char *out, size_t out_sz) {
    char data[4096];
    if (!tur_global_data_dir(data, sizeof(data))) return false;
    snprintf(out, out_sz, "%s/cache", data);
    return true;
}

bool tur_global_state_path(char *out, size_t out_sz) {
    char data[4096];
    if (!tur_global_data_dir(data, sizeof(data))) return false;
    snprintf(out, out_sz, "%s/state.tur", data);
    return true;
}

bool tur_global_ensure_layout(void) {
    char p[4096];
    if (!tur_global_data_dir(p, sizeof(p)) || !gs_mkdirp(p)) return false;
    if (!tur_global_spices_dir(p, sizeof(p)) || !gs_mkdirp(p)) return false;
    if (!tur_global_cache_dir(p, sizeof(p)) || !gs_mkdirp(p)) return false;
    if (!tur_global_bin_dir(p, sizeof(p)) || !gs_mkdirp(p)) return false;
    return true;
}

bool tur_global_bin_on_path(const char *bin_dir) {
    const char *path = getenv("PATH");
    if (!path || !bin_dir) return false;
    size_t blen = strlen(bin_dir);
    const char *p = path;
    while (*p) {
        const char *sep = strchr(p, TUR_PATH_LIST_SEP);
        size_t seg = sep ? (size_t)(sep - p) : strlen(p);
        /* tur_path_seg_eq, not strncmp: on Windows this same directory is
         * spelled `C:/...` here and `C:\\...` in the PATH the shell exports. */
        if (tur_path_seg_eq(p, seg, bin_dir, blen)) return true;
        if (!sep) break;
        p = sep + 1;
    }
    return false;
}

/* ------------------------------------------------------------------ */
/* state.tur reader/writer                                             */
/* ------------------------------------------------------------------ */

static const Form *gs_map_get_kw(const Form *map, const char *kw) {
    if (!map || map->tag != F_MAP) return NULL;
    const FormList *fl = &map->as.list;
    for (uint32_t i = 0; i + 1 < fl->len; i += 2) {
        const Form *key = fl->items[i];
        if (key->tag == F_KEYWORD && strcmp(key->as.sym->name, kw) == 0)
            return fl->items[i + 1];
    }
    return NULL;
}

static char *gs_form_str_dup(const Form *f) {
    if (!f || f->tag != F_STR) return NULL;
    return gs_ss_dup(f->as.s);
}

static void gs_parse_str_vec(const Form *f, char ***out, int *n_out) {
    *out = NULL;
    *n_out = 0;
    if (!f) return;
    if (f->tag != F_VEC && f->tag != F_LIST) return;
    const FormList *fl = &f->as.list;
    if (fl->len == 0) return;
    *out = (char **)malloc(fl->len * sizeof(char *));
    if (!*out) return;
    for (uint32_t i = 0; i < fl->len; i++) {
        if (fl->items[i]->tag == F_STR)
            (*out)[(*n_out)++] = gs_ss_dup(fl->items[i]->as.s);
    }
}

bool tur_state_read(const char *path, TurState *out) {
    memset(out, 0, sizeof(*out));
    out->format_version = 1;

    FILE *f = fopen(path, "rb");
    if (!f) return true; /* missing is OK */

    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    rewind(f);
    if (sz <= 0) { fclose(f); return true; }
    char *src = (char *)malloc((size_t)sz + 1);
    if (!src) { fclose(f); return false; }
    if (fread(src, 1, (size_t)sz, f) != (size_t)sz) {
        fclose(f); free(src); return false;
    }
    fclose(f);
    src[sz] = '\0';

    Arena arena;
    arena_init(&arena, 0);
    SymbolTable st;
    symtab_init(&st, &arena);

    char *src_arena = (char *)arena_alloc(&arena, (size_t)sz + 1);
    memcpy(src_arena, src, (size_t)sz + 1);
    free(src);
    src = src_arena;

    SourceFile file = {0};
    file.path = path;
    file.src  = src;
    file.len  = (uint32_t)sz;
    diag_register_file(&file);

    uint32_t nforms = 0;
    Form **forms = read_all(&arena, &st, &file, &nforms);
    if (!forms || diag_had_error() || nforms == 0) {
        symtab_free(&st);
        arena_free(&arena);
        return true; /* empty / unparseable state -> treat as empty */
    }

    Form *ds = NULL;
    for (uint32_t i = 0; i < nforms; i++) {
        Form *frm = forms[i];
        if (frm->tag != F_LIST || frm->as.list.len < 1) continue;
        Form *head = frm->as.list.items[0];
        if (head->tag == F_SYM &&
            strcmp(head->as.sym->name, "defstate") == 0) {
            ds = frm;
            break;
        }
    }
    if (!ds) {
        symtab_free(&st);
        arena_free(&arena);
        return true;
    }

    const FormList *fl = &ds->as.list;
    for (uint32_t i = 1; i + 1 < fl->len; i += 2) {
        const Form *kf = fl->items[i];
        const Form *vf = fl->items[i + 1];
        if (kf->tag != F_KEYWORD) { i -= 1; continue; }
        const char *kw = kf->as.sym->name;
        if (strcmp(kw, "format-version") == 0) {
            if (vf->tag == F_INT) out->format_version = (int)vf->as.i;
        } else if (strcmp(kw, "installed") == 0) {
            if (!vf || vf->tag != F_MAP) continue;
            const FormList *mfl = &vf->as.list;
            for (uint32_t j = 0; j + 1 < mfl->len; j += 2) {
                const Form *nf = mfl->items[j];
                const Form *ef = mfl->items[j + 1];
                if (nf->tag != F_STR) continue;
                if (!ef || ef->tag != F_MAP) continue;

                out->entries = (TurStateEntry *)realloc(out->entries,
                    (out->n_entries + 1) * sizeof(TurStateEntry));
                if (!out->entries) {
                    symtab_free(&st); arena_free(&arena); return false;
                }
                TurStateEntry *e = &out->entries[out->n_entries++];
                memset(e, 0, sizeof(*e));
                e->name         = gs_ss_dup(nf->as.s);
                e->url          = gs_form_str_dup(gs_map_get_kw(ef, "url"));
                e->ref          = gs_form_str_dup(gs_map_get_kw(ef, "ref"));
                e->subdir       = gs_form_str_dup(gs_map_get_kw(ef, "subdir"));
                e->path         = gs_form_str_dup(gs_map_get_kw(ef, "path"));
                e->resolved     = gs_form_str_dup(gs_map_get_kw(ef, "resolved"));
                e->version      = gs_form_str_dup(gs_map_get_kw(ef, "version"));
                e->installed_at = gs_form_str_dup(gs_map_get_kw(ef, "installed-at"));
                e->built_with   = gs_form_str_dup(gs_map_get_kw(ef, "built-with"));
                e->dir          = gs_form_str_dup(gs_map_get_kw(ef, "dir"));
                e->src_dir      = gs_form_str_dup(gs_map_get_kw(ef, "src-dir"));
                gs_parse_str_vec(gs_map_get_kw(ef, "bin"),
                                 &e->bin, &e->n_bin);
            }
        }
    }

    symtab_free(&st);
    arena_free(&arena);
    return true;
}

/* state.tur is Turmeric source that tur writes and tur reads back, so every
 * value it emits has to survive the reader ESCAPE PROCESSING.  A Windows path
 * does not survive unescaped: a :path value written verbatim comes back as
 * `unknown string escape` on its first directory separator, and the WHOLE
 * registry then fails to load -- so the second `tur install` on a Windows box
 * could not see the first.  The same hazard exists on POSIX for any path
 * containing a double quote.
 *
 * tur_source_literal_escape is the same helper the generated-source paths use;
 * see its header for why escaping rather than rewriting separators. */
static void state_put_str(FILE *f, const char *s) {
    char esc[8192];
    tur_source_literal_escape(s ? s : "", esc, sizeof esc);
    fprintf(f, "\"%s\"", esc);
}

static void state_put_kv(FILE *f, const char *key, const char *val) {
    if (!val) return;
    fprintf(f, ":%s ", key);
    state_put_str(f, val);
    fprintf(f, " ");
}

bool tur_state_write(const char *path, const TurState *st) {
    char tmp_path[4096];
    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp.%d", path, (int)getpid());
    FILE *f = fopen(tmp_path, "w");
    if (!f) {
        fprintf(stderr, "tur: cannot write '%s': %s\n",
                path, strerror(errno));
        return false;
    }

    fprintf(f, ";;; state.tur -- installed-spice registry. Managed by `tur install`.\n");
    fprintf(f, "(defstate\n");
    fprintf(f, "  :format-version %d\n", st->format_version > 0 ? st->format_version : 1);
    fprintf(f, "  :installed #{\n");
    for (int i = 0; i < st->n_entries; i++) {
        const TurStateEntry *e = &st->entries[i];
        fprintf(f, "    ");
        state_put_str(f, e->name ? e->name : "");
        fprintf(f, " #{");
        state_put_kv(f, "url",          e->url);
        state_put_kv(f, "ref",          e->ref);
        state_put_kv(f, "subdir",       e->subdir);
        state_put_kv(f, "path",         e->path);
        state_put_kv(f, "resolved",     e->resolved);
        state_put_kv(f, "version",      e->version);
        state_put_kv(f, "installed-at", e->installed_at);
        state_put_kv(f, "built-with",   e->built_with);
        state_put_kv(f, "dir",          e->dir);
        state_put_kv(f, "src-dir",      e->src_dir);
        if (e->n_bin > 0) {
            fprintf(f, ":bin [");
            for (int j = 0; j < e->n_bin; j++) {
                if (j) fprintf(f, " ");
                state_put_str(f, e->bin[j]);
            }
            fprintf(f, "] ");
        }
        fprintf(f, "}\n");
    }
    fprintf(f, "  })\n");

    fclose(f);
    if (rename(tmp_path, path) != 0) {
        fprintf(stderr, "tur: rename failed: %s\n", strerror(errno));
        unlink(tmp_path);
        return false;
    }
    return true;
}

static void gs_entry_free(TurStateEntry *e) {
    free(e->name);
    free(e->url);
    free(e->ref);
    free(e->subdir);
    free(e->path);
    free(e->resolved);
    free(e->version);
    free(e->installed_at);
    free(e->built_with);
    free(e->dir);
    free(e->src_dir);
    for (int j = 0; j < e->n_bin; j++) free(e->bin[j]);
    free(e->bin);
    memset(e, 0, sizeof(*e));
}

void tur_state_free(TurState *st) {
    for (int i = 0; i < st->n_entries; i++) gs_entry_free(&st->entries[i]);
    free(st->entries);
    memset(st, 0, sizeof(*st));
}

TurStateEntry *tur_state_find(TurState *st, const char *name) {
    for (int i = 0; i < st->n_entries; i++) {
        if (st->entries[i].name && strcmp(st->entries[i].name, name) == 0)
            return &st->entries[i];
    }
    return NULL;
}

TurStateEntry *tur_state_upsert(TurState *st, const char *name) {
    TurStateEntry *e = tur_state_find(st, name);
    if (e) {
        /* Wipe existing scalar fields; caller will re-populate. */
        free(e->url);          e->url = NULL;
        free(e->ref);          e->ref = NULL;
        free(e->subdir);       e->subdir = NULL;
        free(e->path);         e->path = NULL;
        free(e->resolved);     e->resolved = NULL;
        free(e->version);      e->version = NULL;
        free(e->installed_at); e->installed_at = NULL;
        free(e->built_with);   e->built_with = NULL;
        free(e->dir);          e->dir = NULL;
        free(e->src_dir);      e->src_dir = NULL;
        for (int j = 0; j < e->n_bin; j++) free(e->bin[j]);
        free(e->bin);
        e->bin = NULL;
        e->n_bin = 0;
        return e;
    }
    st->entries = (TurStateEntry *)realloc(st->entries,
        (st->n_entries + 1) * sizeof(TurStateEntry));
    if (!st->entries) return NULL;
    e = &st->entries[st->n_entries++];
    memset(e, 0, sizeof(*e));
    e->name = tur_strdup(name);
    return e;
}

bool tur_state_remove(TurState *st, const char *name) {
    for (int i = 0; i < st->n_entries; i++) {
        if (st->entries[i].name && strcmp(st->entries[i].name, name) == 0) {
            gs_entry_free(&st->entries[i]);
            for (int j = i; j < st->n_entries - 1; j++)
                st->entries[j] = st->entries[j + 1];
            st->n_entries--;
            return true;
        }
    }
    return false;
}

static bool gs_is_dir(const char *p) {
    struct stat ds;
    return p[0] && stat(p, &ds) == 0 && S_ISDIR(ds.st_mode);
}

/* The pre-D5 reconstruction of a git install's root:
 * spices/<name>-<sanitized-ref>.  It names the wrong directory whenever the
 * clone was made under a different name than the entry's (a monorepo
 * `--subdir` install without `--name`), which is why new entries record
 * `:dir`; it survives only for entries written before that. */
static bool gs_reconstructed_root(const TurStateEntry *e, char *out, size_t out_sz) {
    char spices_dir[4096];
    if (!tur_global_spices_dir(spices_dir, sizeof(spices_dir))) return false;
    char ref_tag[128];
    snprintf(ref_tag, sizeof(ref_tag), "%s", e->ref ? e->ref : "HEAD");
    for (char *p = ref_tag; *p; p++) if (*p == '/') *p = '-';
    snprintf(out, out_sz, "%s/%s-%s", spices_dir,
             e->name ? e->name : "unknown", ref_tag);
    return true;
}

bool tur_state_entry_root_dir(const TurStateEntry *e, char *out, size_t out_sz) {
    if (!e || !out || out_sz == 0) return false;
    out[0] = '\0';
    if (e->path) return false;
    if (e->dir) { snprintf(out, out_sz, "%s", e->dir); return true; }
    return gs_reconstructed_root(e, out, out_sz);
}

bool tur_state_entry_src_dir(const TurStateEntry *e, char *out, size_t out_sz) {
    if (!e || !out || out_sz == 0) return false;
    out[0] = '\0';
    if (e->src_dir) {
        snprintf(out, out_sz, "%s", e->src_dir);
    } else if (e->path) {
        snprintf(out, out_sz, "%s", e->path);
    } else {
        char base[4096];
        if (!tur_state_entry_root_dir(e, base, sizeof(base))) return false;
        if (e->subdir) snprintf(out, out_sz, "%s/%s", base, e->subdir);
        else           snprintf(out, out_sz, "%s", base);
    }
    return gs_is_dir(out);
}

/* global-spice-library-consumption: resolve an INSTALLED spice's source
 * directory by name, for a manifest dep declared `#{:global true}`.
 *
 * The directory comes from tur_state_entry_src_dir: the recorded `:src-dir` /
 * `:dir` (installed-spice-libraries D5), or the reconstruction for an entry
 * that predates them.  Returns false when the spice is not installed or its
 * directory is gone, so callers can report "install it first" rather than
 * falling through to a project-local guess.
 *
 * `out_version` / `out_resolved`, when non-NULL, receive the state entry's
 * recorded version and resolved SHA (borrowed from the caller-owned state, so
 * they are copied out here).  A `--path` install has no SHA; *out_resolved is
 * left NULL then. */
bool tur_installed_spice_dir(const char *name, char *out, size_t out_sz,
                             char **out_version, char **out_resolved) {
    if (out_version)  *out_version  = NULL;
    if (out_resolved) *out_resolved = NULL;
    if (!name || !*name || !out || out_sz == 0) return false;

    char state_path[4096];
    if (!tur_global_state_path(state_path, sizeof(state_path))) return false;
    TurState st; memset(&st, 0, sizeof(st));
    if (!tur_state_read(state_path, &st)) return false;

    bool found = false;
    TurStateEntry *e = tur_state_find(&st, name);
    char dir[4096];
    if (e && tur_state_entry_src_dir(e, dir, sizeof(dir))) {
        snprintf(out, out_sz, "%s", dir);
        if (out_version && e->version)   *out_version  = tur_strdup(e->version);
        if (out_resolved && e->resolved) *out_resolved = tur_strdup(e->resolved);
        found = true;
    }
    tur_state_free(&st);
    return found;
}

/* ------------------------------------------------------------------ */
/* Module listing (installed-spice-libraries D6)                       */
/* ------------------------------------------------------------------ */

static void gs_push_mod(char ***out, int *n, int *cap, const char *rel) {
    if (*n >= *cap) {
        int nc = *cap ? *cap * 2 : 16;
        char **nv = (char **)realloc(*out, (size_t)nc * sizeof(char *));
        if (!nv) return;
        *out = nv; *cap = nc;
    }
    (*out)[(*n)++] = tur_strdup(rel);
}

static void gs_walk_mods(const char *dir, const char *rel,
                         char ***out, int *n, int *cap, int depth) {
    if (depth > 32) return;
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (ent->d_name[0] == '.') continue;
        char path[4096], sub[4096];
        snprintf(path, sizeof(path), "%s/%s", dir, ent->d_name);
        if (rel[0]) snprintf(sub, sizeof(sub), "%s/%s", rel, ent->d_name);
        else        snprintf(sub, sizeof(sub), "%s", ent->d_name);
        struct stat st;
        if (stat(path, &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) {
            gs_walk_mods(path, sub, out, n, cap, depth + 1);
        } else if (S_ISREG(st.st_mode)) {
            size_t sl = strlen(sub);
            if (sl > 4 && strcmp(sub + sl - 4, ".tur") == 0) {
                sub[sl - 4] = '\0';
                gs_push_mod(out, n, cap, sub);
            }
        }
    }
    closedir(d);
}

static int gs_cmp_str(const void *a, const void *b) {
    return strcmp(*(const char *const *)a, *(const char *const *)b);
}

/* `src/main.tur` / `./src/main.tur` -> `main`; NULL when the :bin path is
 * not under src/ (then it cannot shadow a module anyway). */
static bool gs_bin_module(const char *bin_path, char *out, size_t cap) {
    const char *p = bin_path;
    while (p[0] == '.' && p[1] == '/') p += 2;
    if (strncmp(p, "src/", 4) != 0) return false;
    p += 4;
    size_t l = strlen(p);
    if (l <= 4 || strcmp(p + l - 4, ".tur") != 0 || l - 4 >= cap) return false;
    memcpy(out, p, l - 4);
    out[l - 4] = '\0';
    return true;
}

int tur_spice_modules(const char *spice_dir, char ***out) {
    *out = NULL;
    if (!spice_dir) return 0;
    char src[4096];
    snprintf(src, sizeof(src), "%s/src", spice_dir);
    int n = 0, cap = 0;
    gs_walk_mods(src, "", out, &n, &cap, 0);

    /* A :bin entrypoint is a program, not a library module: nobody imports
     * a tool's `main`, and counting it would make every two tools that keep
     * their entry at src/main.tur "provide the same module". */
    char mp[4096];
    PkgManifest m;
    memset(&m, 0, sizeof(m));
    if (pkg_resolve_manifest_path(spice_dir, mp, sizeof(mp)) &&
        pkg_manifest_read(mp, &m)) {
        for (int b = 0; b < m.n_bins; b++) {
            char bm[4096];
            if (!m.bin_paths[b] || !gs_bin_module(m.bin_paths[b], bm, sizeof(bm)))
                continue;
            for (int i = 0; i < n; i++) {
                if (strcmp((*out)[i], bm) != 0) continue;
                free((*out)[i]);
                (*out)[i] = (*out)[--n];
                break;
            }
        }
    }
    pkg_manifest_free(&m);
    if (n > 1) qsort(*out, (size_t)n, sizeof(char *), gs_cmp_str);
    return n;
}

void tur_spice_modules_free(char **mods, int n) {
    for (int i = 0; i < n; i++) free(mods[i]);
    free(mods);
}
