/* preamble_split.c -- the preamble and prelude splits and their caches: the
 * prelude's compiled pieces, the r7rs library-unit cache
 * (r7rs-programs-compile-slowly), the swap of the fixed runtime preamble for
 * the committed decls region (cc-path-preamble-split-plan), and `tur
 * emit-rt-split`.  The plain cc path uses all of it, which is why none of it
 * lives in jit.c.
 *
 * Part of the compiler driver (src/driver/); driver.h says how the files
 * share declarations. */

#include "driver.h"
#include "commands.h"

#ifndef _WIN32
#include <sys/time.h>
#endif

#include "compiler/emit_split.h" /* r7rs-programs-compile-slowly */
#include "runtime/hamt.h" /* S2: tur_hamt_hash_xxh64 for the split-artifact hash */
#include "runtime/rt_split_embed.h" /* S2: committed decls region + hash (TUR_JIT) */
#include "globals.h"

/* r7rs-split-units-each-compile-the-runtime: the library unit's emission,
 * cached.  On a warm cache the unit's text is already compiled (its object is
 * keyed by that text), but emitting it again was still ~40% of `tur`'s work
 * for a one-line program.  So the first build that emits it also writes the
 * renamed text and everything the emission leaves for the later units
 * (emit_split_carry_save) to <tur-build>/prelude/<key>.lib, and a later build
 * with the same key reads that back instead of emitting.  The key has to be
 * known BEFORE emission, so it covers everything that can reach the unit's
 * text (split_lib_cache_init): the compiler binary itself, the command line,
 * TUR_* in the environment, the experiments and emission switches, the
 * whole-program feature flags, and the path and content of every stdlib file
 * the unit's items come from.  The program's own code reaches none of it: the
 * unit writes only the stdlib's items.  A program whose items would land there
 * (an `extern-c`), or that reads a module of its own (a Turmeric module, a
 * Scheme library or `include` in another file), does not use the cache.  Nor
 * do its imports fork the key: an imported library's items are a later
 * unit's, and every r7rs fixture -- `(scheme eval)`, the SRFIs, the
 * `(turmeric ...)` seam -- emits one and the same library unit.
 *
 * What a wrong key could do is caught at three points, all falling back to
 * emitting the unit: the record names a binding this program does not have
 * (emit_split_carry_load), a later unit declares a library function with
 * another signature (split_check_sigs), or the units fail to compile or link
 * (cmd_build retries with the cache off).  TUR_SPLIT_LIB_CACHE=verify emits
 * the unit anyway and says when the entry differs from it;
 * tests/check-r7rs-split-lib-cache.sh runs it (TUR_SPLIT_LIB_CACHE_ALL=1: over
 * every r7rs fixture).  TUR_SPLIT_LIB_CACHE=0 turns the cache off. */
/* cmd_build: emit the library unit, whatever the cache holds (a retry after
 * a split build whose library unit came from it did not build); the entry is
 * still written, replacing one that differs. */
bool g_split_lib_cache_off = false;

/* r7rs-prelude-library-cold-compile: how many pieces the library unit is
 * compiled in (emit_split_pieces), at once.  The unit is ~1,200 functions at
 * -O2 and its parse is a small part of the compile, so the wall time falls
 * with the pieces: a cold build took 9.1 s whole and takes 4.6 s in four on
 * a 4-core box (docs/archive/r7rs-prelude-library-cold-compile.md).  One per
 * online CPU up to eight; TUR_PRELUDE_JOBS=<n> overrides, and 1 compiles the
 * unit whole.  Whole on Windows, where cmd.exe has no `&`/`wait`. */
#define PRELUDE_PIECE_DUP_MAX 1200   /* bytes of body: a copy per piece, for inlining */
int prelude_pieces(void) {
#ifdef _WIN32
    return 1;
#else
    const char *e = getenv("TUR_PRELUDE_JOBS");
    long n = (e && *e) ? strtol(e, NULL, 10) : sysconf(_SC_NPROCESSORS_ONLN);
    if (n < 1) n = 1;
    if (n > 8) n = 8;
    return (int)n;
#endif
}

/* Compile the library unit `lib_c` as `np` pieces at once and link them into
 * the one relocatable object `out_obj`.  Nonzero when anything fails -- the
 * caller then compiles the unit whole, so a piece that does not compile costs
 * time, never a build.  `stem` names the temporaries. */
static int prelude_compile_pieces(const Buf *lib_c, const char *cc, const char *flags,
                                  const char *stem, int np, const char *piece_prefix,
                                  const char *out_obj) {
    Buf pieces[8];
    for (int k = 0; k < np; k++) buf_init(&pieces[k]);
    int rc = emit_split_pieces(lib_c->data, lib_c->len, np, PRELUDE_PIECE_DUP_MAX,
                               piece_prefix, pieces);
    char src[8][1100], obj[8][1100];
    int written = 0;
    for (int k = 0; rc == 0 && k < np; k++) {
        snprintf(src[k], sizeof src[k], "%s.p%d.c", stem, k);
        snprintf(obj[k], sizeof obj[k], "%s.p%d.o", stem, k);
        FILE *f = drv_fopen_nofollow_w(src[k]);
        if (!f || fwrite(pieces[k].data, 1, pieces[k].len, f) != pieces[k].len) rc = 2;
        if (f) fclose(f);
        written = k + 1;
    }
    for (int k = 0; k < np; k++) buf_free(&pieces[k]);
    bool show = getenv("TUR_SHOW_CC") != NULL;
    if (rc == 0) {
        /* One shell: every compile in the background, then each one's
         * status by its pid, so a failure in any of them is seen. */
        /* Each piece carries every static helper and read-only table of
         * the unit and uses a share of them: the rest are unused, which is
         * no news. */
        Buf cmd; buf_init(&cmd);
        bool pq_ok = true;
        for (int k = 0; k < np; k++) {
            buf_printf(&cmd, "%s %s -Wno-unused-function -Wno-unused-variable "
                             "-Wno-unused-const-variable -c -o ", cc, flags);
            pq_ok = drv_buf_put_quoted(&cmd, obj[k]) && pq_ok;
            buf_putc(&cmd, ' ');
            pq_ok = drv_buf_put_quoted(&cmd, src[k]) && pq_ok;
            buf_printf(&cmd, " & p%d=$!; ", k);
        }
        if (!pq_ok) { buf_free(&cmd); return 2; }
        buf_puts(&cmd, "ok=0;");
        for (int k = 0; k < np; k++) buf_printf(&cmd, " wait $p%d || ok=1;", k);
        buf_puts(&cmd, " exit $ok");
        buf_putc(&cmd, '\0');
        if (show) fprintf(stderr, "CC: %s\n", cmd.data);
        rc = cc_system(cmd.data) != 0;
        buf_free(&cmd);
    }
    if (rc == 0) {
        Buf cmd; buf_init(&cmd);
        bool pq_ok = true;
        buf_printf(&cmd, "%s -r -nostdlib -o ", cc);
        pq_ok = drv_buf_put_quoted(&cmd, out_obj) && pq_ok;
        for (int k = 0; k < np; k++) {
            buf_putc(&cmd, ' ');
            pq_ok = drv_buf_put_quoted(&cmd, obj[k]) && pq_ok;
        }
        buf_putc(&cmd, '\0');
        if (!pq_ok) { buf_free(&cmd); return 2; }
        if (show) fprintf(stderr, "CC: %s\n", cmd.data);
        rc = cc_system(cmd.data) != 0;
        buf_free(&cmd);
    }
    for (int k = 0; k < written; k++) {
        if (!show) unlink(src[k]);
        unlink(obj[k]);
    }
    if (rc != 0) unlink(out_obj);
    return rc;
}

/* WP2 (D-4): may we reuse this cached prelude object?
 *
 * The test used to be `stat(obj) == 0 && st_size > 0` -- existence and
 * non-emptiness -- for a file whose name is a hash an attacker can compute and
 * whose contents go straight onto the link line.  Now it must also be a
 * regular file (lstat: not a symlink into someone else's object, not a fifo)
 * that we own and that nobody else can write. */
static bool prelude_object_usable(const char *obj, struct stat *st) {
    if (lstat(obj, st) != 0) return false;
    if (!S_ISREG(st->st_mode)) return false;
    if (st->st_size <= 0) return false;
#ifndef _WIN32
    if (st->st_uid != geteuid()) return false;
    if (st->st_mode & (S_IWGRP | S_IWOTH)) return false;
#endif
    return true;
}

/* r7rs-prelude-library-object-varies-with-the-program, fix direction 4: the
 * prelude cache is pruned least-recently-used first.  A reused object has its
 * mtime stamped (prelude_object_reuse); after compiling a new one, objects
 * beyond PRELUDE_CACHE_KEEP are removed oldest first -- but only those no
 * build has used for PRELUDE_CACHE_IDLE_SECS, so an object a concurrent build
 * has just chosen (and stamped) is never pulled from under its link.  Every
 * new `tur` whose library text changed adds an object; nothing removed them,
 * and a day of work left 42 (89 MB) in one directory. */
#define PRELUDE_CACHE_KEEP      24
#define PRELUDE_CACHE_IDLE_SECS (24 * 60 * 60)

static int prelude_object_reuse(const char *obj) {
#ifndef _WIN32
    (void)utimes(obj, NULL);
#else
    (void)obj;
#endif
    return 0;
}

typedef struct { char name[32]; time_t mtime; } PreludeCacheEnt;

static int prelude_cache_ent_cmp(const void *a, const void *b) {
    time_t x = ((const PreludeCacheEnt *)a)->mtime, y = ((const PreludeCacheEnt *)b)->mtime;
    return x < y ? -1 : x > y ? 1 : 0;
}

/* Prune the cache's own `<16 hex><suffix>` files beyond `keep_n`, least
 * recently used first, sparing any used in the last `idle_secs`. */
static void prelude_cache_prune_ext(const char *dir, const char *keep, const char *suffix,
                                    size_t keep_n, double idle_secs) {
#ifndef _WIN32
    DIR *d = opendir(dir);
    if (!d) return;
    PreludeCacheEnt *ents = NULL;
    size_t n = 0, cap = 0;
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        const char *nm = de->d_name;
        size_t l = strlen(nm);
        /* Only the cache's own `<16 hex><suffix>`: never a lock, a kept unit,
         * or a private `<hash>.<pid>.o` another build is still writing. */
        if (l != 16 + strlen(suffix) || strcmp(nm + 16, suffix) != 0) continue;
        bool hex = true;
        for (size_t i = 0; i < 16; i++) if (!isxdigit((unsigned char)nm[i])) hex = false;
        if (!hex) continue;
        char path[1100];
        snprintf(path, sizeof path, "%s/%s", dir, nm);
        if (keep && strcmp(path, keep) == 0) continue;
        struct stat st;
        if (lstat(path, &st) != 0 || !S_ISREG(st.st_mode)) continue;
        if (n == cap) {
            cap = cap ? cap * 2 : 32;
            PreludeCacheEnt *ne = realloc(ents, cap * sizeof *ents);
            if (!ne) break;
            ents = ne;
        }
        memcpy(ents[n].name, nm, l + 1);
        ents[n].mtime = st.st_mtime;
        n++;
    }
    closedir(d);
    if (n + 1 > keep_n) {
        qsort(ents, n, sizeof *ents, prelude_cache_ent_cmp);
        size_t excess = n + 1 - keep_n;
        time_t now = time(NULL);
        for (size_t i = 0; i < n && excess > 0; i++, excess--) {
            if (difftime(now, ents[i].mtime) < idle_secs) break;
            char path[1100];
            snprintf(path, sizeof path, "%s/%s", dir, ents[i].name);
            unlink(path);
        }
    }
    free(ents);
#else
    (void)dir; (void)keep; (void)suffix; (void)keep_n; (void)idle_secs;
#endif
}

static void prelude_cache_prune(const char *dir, const char *keep) {
    prelude_cache_prune_ext(dir, keep, ".o", PRELUDE_CACHE_KEEP, PRELUDE_CACHE_IDLE_SECS);
}

/* r7rs-prelude-library-object-varies-with-the-program, cause 3: could a
 * build's `-I <dir>` change the library unit's compile?  Only by supplying a
 * header, so a directory with no C header in it (a Turmeric module path, the
 * usual `-I`, or `-I .` in a project of `.tur` files) cannot, and it stays
 * out of the cache key -- it gave `tur build -I . a.tur` and `tur build
 * a.tur` two objects of byte-identical text, a cold compile each.  A header
 * is a file named like one (.h, .hh, .hpp, .hxx, .inc, .def), looked for up
 * to three levels down; a directory too big to walk counts as supplying one.
 * The `-I` is still passed to the compile either way. */
static bool dir_may_supply_headers_rec(const char *dir, int depth, int *budget) {
#ifndef _WIN32
    DIR *d = opendir(dir);
    if (!d) return false;
    bool found = false;
    struct dirent *de;
    while (!found && (de = readdir(d)) != NULL) {
        if (--*budget < 0) { found = true; break; }
        const char *nm = de->d_name;
        if (nm[0] == '.') continue;
        char path[2048];
        if ((size_t)snprintf(path, sizeof path, "%s/%s", dir, nm) >= sizeof path) continue;
        struct stat st;
        if (stat(path, &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) {
            if (depth < 3 && dir_may_supply_headers_rec(path, depth + 1, budget)) found = true;
        } else {
            static const char *const hdr_ext[] = { ".h", ".hh", ".hpp", ".hxx", ".inc", ".def" };
            const char *dot = strrchr(nm, '.');
            for (size_t k = 0; dot && k < sizeof hdr_ext / sizeof hdr_ext[0]; k++)
                if (strcmp(dot, hdr_ext[k]) == 0) { found = true; break; }
        }
    }
    closedir(d);
    return found;
#else
    (void)dir; (void)depth; (void)budget;
    return true;
#endif
}
static bool dir_may_supply_headers(const char *dir) {
    int budget = 4000;
    return dir_may_supply_headers_rec(dir, 0, &budget);
}

/* r7rs-programs-compile-slowly: the object of a split build's library unit
 * (emit_split.h), compiled once and cached by a hash of everything that goes
 * into it -- its text, the compiler and every flag -- under
 * <tur-build>/prelude/<hash>.o.  A cached object is reused as is; a missing
 * one is compiled to a private name and renamed into place, so parallel
 * builds (the fixture suite) never read a half-written object.  Returns 0
 * with `obj` filled, nonzero if the unit would not compile. */
int prelude_split_object(const Buf *lib_c, const char *cc, const char *cc_flags,
                         const Buf *aux_includes, const Buf *autolink,
                         bool needs_asan, const Buf *cmake_flags,
                         const char **include_dirs, int n_include_dirs,
                         const char *piece_prefix, int np, bool accept_quoted,
                         char *obj, size_t obj_cap) {
    /* The flags that reach a compile: the build's own, plus every -I/-D the
     * link line would pass; nothing it links.  And the link's sanitizers: a
     * program that links the ASan libturi (`eval`) is compiled with ASan, and
     * the library unit must be too -- it is where the prelude's
     * `__asan_default_options` lives, which turns off the use-after-return
     * fake stack a conservative collector cannot scan. */
    Buf flags; buf_init(&flags);
    buf_printf(&flags, "%s" TUR_EMITTED_C_CC_FLAGS, cc_flags);
    if (needs_asan) buf_puts(&flags, " -fsanitize=address,undefined");
    const Buf *srcs[3] = { aux_includes, autolink, cmake_flags };
    for (int k = 0; k < 3; k++) {
        const Buf *b = srcs[k];
        if (!b || b->len == 0 || !b->data) continue;
        const char *q = b->data, *lim = b->data + b->len;
        while (q < lim && *q) {
            while (q < lim && *q == ' ') q++;
            const char *tok = q;
            /* A shell-quoted token is one token, spaces and all: `(scheme
             * eval)`'s `'-DTUR_R7RS_STDLIB="<root>"'` (its @TUR_STDLIB_ROOT@)
             * is how the stdlib root reaches its C -- which, imported, is
             * compiled here (r7rs-imported-libraries-recompiled-every-build).
             * The compile runs through system(), so the shell unquotes it as
             * it does on the link line.  The imported-library unit's only: no
             * auto-loaded file writes one, and in the library unit's key it
             * would only fork that object for nothing. */
            bool squote = accept_quoted && (*q == '\'');
            if (squote) {
                q++;
                while (q < lim && *q && *q != '\'') q++;
                if (q < lim && *q == '\'') q++;
            }
            while (q < lim && *q && *q != ' ') q++;
            size_t tl = (size_t)(q - tok);
            const char *t = squote ? tok + 1 : tok;
            if (tl > (squote ? 4u : 2u) && t[0] == '-' && (t[1] == 'I' || t[1] == 'D')) {
                buf_putc(&flags, ' ');
                buf_write(&flags, tok, tl);
            }
        }
    }
    /* The key's flags are the compile's, less an `-I` that can supply no
     * header (dir_may_supply_headers). */
    Buf keyflags; buf_init(&keyflags);
    buf_write(&keyflags, flags.data, flags.len);
    for (int i = 0; i < n_include_dirs; i++) {
        if (include_dirs[i] && include_dirs[i][0]) {
            buf_puts(&flags, " -I");
            if (!drv_buf_put_quoted(&flags, include_dirs[i])) {
                buf_free(&flags);
                buf_free(&keyflags);
                return 2;
            }
            if (dir_may_supply_headers(include_dirs[i])) {
                buf_puts(&keyflags, " -I");
                (void)drv_buf_put_quoted(&keyflags, include_dirs[i]);
            }
        }
    }
    buf_putc(&flags, '\0');
    buf_putc(&keyflags, '\0');

    Buf key; buf_init(&key);
    buf_printf(&key, "%s\n%s\n%s\n", TUR_VERSION, cc, keyflags.data);
    buf_free(&keyflags);
    buf_write(&key, lib_c->data, lib_c->len);
    uint64_t h = tur_hamt_hash_xxh64(key.data, key.len);
    buf_free(&key);

    char dir[1024];
    snprintf(dir, sizeof dir, "%sprelude", drv_stable_c_prefix());
    if (mkdir(dir, 0700) != 0 && errno != EEXIST) {
        fprintf(stderr, "tur: cannot create '%s': %s\n", dir, strerror(errno));
        buf_free(&flags);
        return 2;
    }
    /* WP2 (D-4): the cached object is LINKED INTO the user's binary, so the
     * directory holding it has to be one we own -- drv_stable_c_prefix already
     * refuses a parent we do not, and this is the same check one level down,
     * because the prelude dir may predate that rule. */
    if (!drv_tmp_dir_is_ours(dir)) {
        fprintf(stderr,
            "tur: refusing to use the prelude cache in '%s' -- it is not a "
            "directory this user owns and controls\n", dir);
        buf_free(&flags);
        return 2;
    }
    snprintf(obj, obj_cap, "%s/%016llx.o", dir, (unsigned long long)h);
    struct stat st;
    if (prelude_object_usable(obj, &st)) { buf_free(&flags); return prelude_object_reuse(obj); }

    /* One compile per library.  The unit exports every stdlib definition, so
     * cc cannot drop the ones no program reaches, and the compile costs more
     * than a whole one-unit build did: builds that start together on a cold
     * cache (the fixture suite, `make -j`) wait for the first one's object
     * instead of each compiling their own.  A lock older than the wait (its
     * holder killed) is ignored, and a waiter that gives up compiles its own
     * copy -- the rename below makes racing writers agree. */
    char lock[1100];
    snprintf(lock, sizeof lock, "%s/%016llx.lock", dir, (unsigned long long)h);
    int lock_fd = open(lock, O_CREAT | O_EXCL | O_WRONLY, 0600);
    if (lock_fd < 0 && errno == EEXIST) {
        for (int tick = 0; tick < 1200; tick++) {   /* 100 ms ticks, 120 s */
            if (prelude_object_usable(obj, &st)) { buf_free(&flags); return prelude_object_reuse(obj); }
            if (stat(lock, &st) != 0) break;
            if (difftime(time(NULL), st.st_mtime) > 120) break;
            usleep(100000);
        }
        if (prelude_object_usable(obj, &st)) { buf_free(&flags); return prelude_object_reuse(obj); }
    }

    char src[1100], tmp_obj[1100];
    snprintf(src, sizeof src, "%s/%016llx.%ld.c", dir, (unsigned long long)h, (long)getpid());
    snprintf(tmp_obj, sizeof tmp_obj, "%s/%016llx.%ld.o", dir, (unsigned long long)h, (long)getpid());
    FILE *f = drv_fopen_nofollow_w(src);
    if (!f || fwrite(lib_c->data, 1, lib_c->len, f) != lib_c->len) {
        if (f) fclose(f);
        buf_free(&flags);
        if (lock_fd >= 0) { close(lock_fd); unlink(lock); }
        return 2;
    }
    fclose(f);
    int rc = 1;
    if (np > 1) {
        char stem[1100];
        snprintf(stem, sizeof stem, "%s/%016llx.%ld", dir, (unsigned long long)h, (long)getpid());
        rc = prelude_compile_pieces(lib_c, cc, flags.data, stem, np, piece_prefix, tmp_obj);
        if (rc != 0 && getenv("TUR_SHOW_CC"))
            fprintf(stderr, "tur: the library unit did not compile in %d pieces; compiling it whole\n", np);
    }
    if (rc != 0) {
        Buf cmd; buf_init(&cmd);
        bool pq_ok = true;
        buf_printf(&cmd, "%s %s -c -o ", cc, flags.data);
        pq_ok = drv_buf_put_quoted(&cmd, tmp_obj) && pq_ok;
        buf_putc(&cmd, ' ');
        pq_ok = drv_buf_put_quoted(&cmd, src) && pq_ok;
        buf_putc(&cmd, '\0');
        if (!pq_ok) { buf_free(&cmd); buf_free(&flags);
                      if (lock_fd >= 0) { close(lock_fd); unlink(lock); }
                      return 2; }
        if (getenv("TUR_SHOW_CC")) fprintf(stderr, "CC: %s\n", cmd.data);
        rc = cc_system(cmd.data);
        buf_free(&cmd);
    }
    buf_free(&flags);
    /* TUR_SHOW_CC keeps the unit's text beside its object, for a look at
     * what the cache key covered. */
    if (getenv("TUR_SHOW_CC")) {
        char keep[1100];
        snprintf(keep, sizeof keep, "%s/%016llx.c", dir, (unsigned long long)h);
        rename(src, keep);
    } else {
        unlink(src);
    }
    int ret = 0;
    if (rc != 0 || rename(tmp_obj, obj) != 0) { unlink(tmp_obj); ret = 2; }
    if (lock_fd >= 0) { close(lock_fd); unlink(lock); }
    if (ret == 0) prelude_cache_prune(dir, obj);
    return ret;
}

/* ---- the library unit's emission, cached (SplitLibCache) ---------------- */

/* An entry is a library unit's carried state (~120 KB) and the hash of its
 * renamed text (~1.6 MB, kept once per text).  There is a key for each `tur`
 * binary, stdlib and set of switches in use, and a text for fewer; one
 * nobody reads is only a cold emission away. */
#define SPLIT_LIB_CACHE_KEEP 32
#define SPLIT_LIB_TEXT_KEEP  8
#define SPLIT_LIB_MAGIC "tur-split-lib 1\n"

#ifndef _WIN32
extern char **environ;
#endif

static int split_lib_str_cmp(const void *a, const void *b) {
    return strcmp(*(const char *const *)a, *(const char *const *)b);
}

/* tur_hamt_hash_xxh64 reads 8-byte words, so it wants an aligned start; a
 * source file's text begins wherever its `#lang` line ends. */
static uint64_t split_lib_hash(const char *p, size_t n) {
    if (((uintptr_t)p & 7u) == 0) return tur_hamt_hash_xxh64(p, n);
    Buf a; buf_init(&a);
    buf_write(&a, p, n);
    uint64_t h = tur_hamt_hash_xxh64(a.data, n);
    buf_free(&a);
    return h;
}

/* One key field: its length, then its bytes, so no two field lists hash the
 * same text. */
static void split_lib_key_put(Buf *k, const char *s) {
    buf_printf(k, "%zu:%s\n", strlen(s), s);
}

void split_lib_cache_init(SplitLibCache *c, const Expr *prog, const char *input) {
    memset(c, 0, sizeof *c);
    const char *mode = getenv("TUR_SPLIT_LIB_CACHE");
    if (mode && strcmp(mode, "0") == 0) return;
#ifdef _WIN32
    (void)prog; (void)input;
#else
    const bool show = getenv("TUR_SHOW_CC") != NULL;
    /* A program that reads a module of its own (a Turmeric one, or a Scheme
     * library or `include` in another file) is left out, beyond what the
     * items below show: its code can do more than the program file's. */
    for (uint32_t f = 1; f < DIAG_MAX_FILES; f++) {
        const SourceFile *sf = diag_source_file((uint16_t)f);
        if (!sf || f < diag_autoload_file_ids_end() || diag_file_is_imported_stdlib((uint16_t)f))
            continue;
        if (show)
            fprintf(stderr, "tur: library unit not cached: the program reads %s, which is not the stdlib's\n",
                    sf->path ? sf->path : "?");
        return;
    }
    /* The files the library unit is written from: the auto-loaded ones and
     * any stdlib module they bring in.  An imported library's (`(srfi 13)`,
     * `(scheme read)`) are not among them -- its items are a later unit's --
     * so a program's imports do not fork the key. */
    bool lib_files[DIAG_MAX_FILES] = { false };
    const Expr *foreign = emit_split_lib_files(prog, lib_files, DIAG_MAX_FILES);
    if (foreign) {
        if (show) {
            const char *fp = diag_file_path(foreign->span.file_id);
            fprintf(stderr, "tur: library unit not cached: it would carry the program's item at %s:%u\n",
                    fp ? fp : "?", (unsigned)foreign->span.line);
        }
        return;
    }
    char exe[4096];
    struct stat est;
    if (drv_get_exe_path(exe, sizeof exe) != 0 || stat(exe, &est) != 0) return;

    Buf k; buf_init(&k);
    buf_puts(&k, SPLIT_LIB_MAGIC);
    split_lib_key_put(&k, TUR_VERSION);
    /* The compiler: a rebuilt `tur` is a new file, whatever its version says. */
    split_lib_key_put(&k, exe);
#if defined(__APPLE__)
    long mt_ns = (long)est.st_mtimespec.tv_nsec;
#else
    long mt_ns = (long)est.st_mtim.tv_nsec;
#endif
    buf_printf(&k, "exe %llu %llu %lld %lld.%09ld\n",
               (unsigned long long)est.st_dev, (unsigned long long)est.st_ino,
               (long long)est.st_size, (long long)est.st_mtime, mt_ns);
    /* The command line, less what names this program's files and outputs:
     * a flag that changes the emitted C changes the key.  An `-I` changes
     * only which files are read, and those are hashed below. */
    for (int i = 1; i < g_main_argc && g_main_argv; i++) {
        const char *a = g_main_argv[i];
        if (strcmp(a, "--") == 0) break;
        if (input && strcmp(a, input) == 0) continue;
        if ((strcmp(a, "-o") == 0 || strcmp(a, "--output") == 0 || strcmp(a, "-I") == 0 ||
             strcmp(a, "-B") == 0 || strcmp(a, "--build-dir") == 0) && i + 1 < g_main_argc) {
            i++;
            continue;
        }
        if (strncmp(a, "-I", 2) == 0 || strncmp(a, "--build-dir=", 12) == 0 ||
            strncmp(a, "--output=", 9) == 0)
            continue;
        buf_puts(&k, "arg ");
        split_lib_key_put(&k, a);
    }
    /* The environment's TUR_* switches (sorted; only the cache's own and
     * the build log's are left out). */
    size_t ne = 0, ecap = 0;
    const char **ev = NULL;
    for (char **e = environ; e && *e; e++) {
        if (strncmp(*e, "TUR_", 4) != 0 || strncmp(*e, "TUR_SHOW_CC=", 12) == 0 ||
            strncmp(*e, "TUR_SPLIT_LIB_CACHE=", 20) == 0)
            continue;
        if (ne == ecap) {
            ecap = ecap ? ecap * 2 : 32;
            const char **nv = (const char **)realloc(ev, ecap * sizeof *ev);
            if (!nv) { free(ev); buf_free(&k); return; }
            ev = nv;
        }
        ev[ne++] = *e;
    }
    if (ne) qsort(ev, ne, sizeof *ev, split_lib_str_cmp);
    for (size_t i = 0; i < ne; i++) { buf_puts(&k, "env "); split_lib_key_put(&k, ev[i]); }
    free(ev);
    /* Experiments, from whichever source enabled them. */
    for (size_t i = 0; i < experiment_count(); i++) {
        const ExperimentDescriptor *d = experiment_at(i);
        buf_printf(&k, "xp %s %d\n", d->name, d->opt_global ? (int)*d->opt_global : -1);
    }
    /* The emission's switches and the whole-program feature flags the
     * preamble reads (globals.h).  A new global that changes the emitted C
     * belongs here; `TUR_SPLIT_LIB_CACHE=verify` over the r7rs fixtures is
     * the check that the list is enough. */
    extern bool g_needs_regex_h;
#define SLK(g) buf_printf(&k, "g " #g " %lld\n", (long long)(g))
    SLK(g_unsafe_max_lines); SLK(g_unsafe_warn_nested); SLK(g_unsafe_require_safety);
    SLK(g_lint_unsafe_enabled); SLK(g_lint_inline_c_unsafe); SLK(g_panic_abort);
    SLK(g_warn_unused_result); SLK(g_lint_panic); SLK(g_no_contracts);
    SLK(g_emit_debug_lines); SLK(g_backtrack_depth); SLK(g_cps_path);
    SLK(g_needs_hamt); SLK(g_needs_regex_h); SLK(g_needs_dlfcn); SLK(g_has_variadics);
    SLK(g_uses_cons); SLK(g_gadt_enabled); SLK(g_data_literals_enabled);
    SLK(g_json_reader_enabled); SLK(g_schema_reader_enabled); SLK(g_sized_types_enabled);
    SLK(g_strict_effects); SLK(g_emit_abi_trace); SLK(g_linear_enabled);
    SLK(g_unique_enabled); SLK(g_substructural_enabled); SLK(g_union_types_enabled);
    SLK(g_intersection_types_enabled); SLK(g_effect_types_enabled);
    SLK(g_contracts_enabled); SLK(g_keep_contracts_in_release); SLK(g_sessions_enabled);
    SLK(g_dynvar_enabled); SLK(g_callcc_enabled); SLK(g_symbols_enabled);
    SLK(g_interpret_mode); SLK(g_elab_toplevel_imports); SLK(g_macro_caps_io);
    SLK(g_no_proc_macros); SLK(g_turi_stdlib_preload); SLK(g_adt_slab);
    SLK(g_sr1_sum_byvalue); SLK(g_sr2_app_sum_byvalue); SLK(g_trail_autoloaded);
    SLK(g_opt_option_niche); SLK(g_opt_regions); SLK(g_opt_dynamic_any);
    SLK(g_opt_saffron_gc); SLK(g_opt_r7rs); SLK(g_opt_reflected_measures);
    SLK(g_opt_repl_jit_inline_c); SLK(g_opt_r7rs_gc); SLK(g_strict_refine);
    SLK(g_emit_for_link); SLK(g_emit_ffi_export_shims); SLK(g_runtime_mode);
    SLK(emit_rcgc_from_archive());
#undef SLK
    split_lib_key_put(&k, g_lang_prelude ? g_lang_prelude : "");
    /* Those files, by path and content. */
    for (uint32_t f = 1; f < DIAG_MAX_FILES; f++) {
        const SourceFile *sf = diag_source_file((uint16_t)f);
        if (!sf || !sf->src || !(f < diag_autoload_file_ids_end() || lib_files[f])) continue;
        buf_printf(&k, "file %zu %016llx ", sf->len,
                   (unsigned long long)split_lib_hash(sf->src, sf->len));
        split_lib_key_put(&k, sf->path ? sf->path : "");
    }
    c->key = split_lib_hash(k.data, k.len);
    buf_free(&k);
    c->on = true;
    c->verify = mode && strcmp(mode, "verify") == 0;
    c->no_restore = g_split_lib_cache_off;
    if (show) fprintf(stderr, "tur: library unit cache key %016llx\n", (unsigned long long)c->key);
#endif
}

/* <tur-build>/prelude/<name><ext>, or false when the directory is not one we
 * own (the text is compiled into the user's program: same rule as the
 * objects beside it, prelude_split_object). */
static bool split_lib_cache_file(uint64_t name, const char *ext, char *out, size_t cap) {
    char dir[1024];
    snprintf(dir, sizeof dir, "%sprelude", drv_stable_c_prefix());
    if (mkdir(dir, 0700) != 0 && errno != EEXIST) return false;
    if (!drv_tmp_dir_is_ours(dir)) return false;
    snprintf(out, cap, "%s/%016llx%s", dir, (unsigned long long)name, ext);
    return true;
}

/* All of a cache file, or false when it is missing or not ours alone. */
static bool split_lib_cache_slurp(const char *path, Buf *out) {
    struct stat st;
    if (!prelude_object_usable(path, &st)) return false;
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    char chunk[65536];
    size_t n;
    while ((n = fread(chunk, 1, sizeof chunk, f)) > 0) buf_write(out, chunk, n);
    bool ok = !ferror(f);
    fclose(f);
    return ok;
}

/* Write `n` bytes to `path` whole or not at all (a private name, renamed). */
static bool split_lib_cache_put_file(const char *path, const char *head,
                                     const char *data, size_t n) {
    char tmp[1200];
    snprintf(tmp, sizeof tmp, "%s.%ld.tmp", path, (long)getpid());
    FILE *f = drv_fopen_nofollow_w(tmp);
    if (!f) return false;
    bool ok = (!head || fputs(head, f) >= 0) && fwrite(data, 1, n, f) == n;
    if (fclose(f) != 0) ok = false;
    if (!ok || rename(tmp, path) != 0) { unlink(tmp); return false; }
    return true;
}

/* The entry for `c->key`: <key>.lib holds the carried state and names the
 * text by its hash; <text hash>.libtext holds the text, once for every key
 * that has it.  False when either is missing or not wholly as written. */
static bool split_lib_cache_read(const SplitLibCache *c, Buf *text, Buf *carry) {
    char path[1100];
    Buf d; buf_init(&d);
    bool ok = false;
    if (split_lib_cache_file(c->key, ".lib", path, sizeof path) &&
        split_lib_cache_slurp(path, &d)) {
        /* "<magic><key>\n<text hash> <text len> <carry len>\n<carry>" */
        size_t ml = sizeof SPLIT_LIB_MAGIC - 1;
        const char *nl1 = d.len > ml && memcmp(d.data, SPLIT_LIB_MAGIC, ml) == 0
                          ? memchr(d.data + ml, '\n', d.len - ml) : NULL;
        const char *nl2 = nl1 ? memchr(nl1 + 1, '\n', (size_t)(d.data + d.len - nl1 - 1)) : NULL;
        unsigned long long key = 0, th = 0;
        size_t tl = 0, cl = 0;
        if (nl2 && sscanf(d.data + ml, "%16llx", &key) == 1 && key == c->key &&
            sscanf(nl1 + 1, "%16llx %zu %zu", &th, &tl, &cl) == 3 &&
            (size_t)(d.data + d.len - (nl2 + 1)) == cl) {
            char tpath[1100];
            size_t before = text->len;
            if (split_lib_cache_file((uint64_t)th, ".libtext", tpath, sizeof tpath) &&
                split_lib_cache_slurp(tpath, text) && text->len - before == tl &&
                split_lib_hash(text->data + before, tl) == (uint64_t)th) {
                buf_write(carry, nl2 + 1, cl);
                ok = true;
            } else {
                text->len = before;
            }
        }
    }
    buf_free(&d);
    return ok;
}

static void split_lib_cache_write(const SplitLibCache *c, const Buf *text, const Buf *carry) {
    uint64_t th = split_lib_hash(text->data, text->len);
    char tpath[1100], path[1100], dir[1024];
    if (!split_lib_cache_file(th, ".libtext", tpath, sizeof tpath) ||
        !split_lib_cache_file(c->key, ".lib", path, sizeof path))
        return;
    snprintf(dir, sizeof dir, "%sprelude", drv_stable_c_prefix());
    Buf have; buf_init(&have);
    bool text_there = split_lib_cache_slurp(tpath, &have) && have.len == text->len &&
                      memcmp(have.data, text->data, text->len) == 0;
    buf_free(&have);
    if (!text_there) {
        if (!split_lib_cache_put_file(tpath, NULL, text->data, text->len)) return;
        prelude_cache_prune_ext(dir, tpath, ".libtext", SPLIT_LIB_TEXT_KEEP, 0);
    }
    char head[128];
    snprintf(head, sizeof head, "%s%016llx\n%016llx %zu %zu\n", SPLIT_LIB_MAGIC,
             (unsigned long long)c->key, (unsigned long long)th, text->len, carry->len);
    if (split_lib_cache_put_file(path, head, carry->data, carry->len))
        prelude_cache_prune_ext(dir, path, ".lib", SPLIT_LIB_CACHE_KEEP, 0);
}

/* In place of the library unit's emission: its cached text (appended to
 * `lib_r`) and carried state, put back.  False -- having put back nothing --
 * when there is no entry or it does not fit this program; emit it then. */
bool split_lib_cache_restore(SplitLibCache *c, const Expr *prog, Buf *lib_r) {
    if (!c->on || c->verify || c->no_restore) return false;
    Buf text, carry;
    buf_init(&text);
    buf_init(&carry);
    bool ok = split_lib_cache_read(c, &text, &carry);
    if (ok && !emit_split_carry_load(carry.data, carry.len, prog)) {
        if (getenv("TUR_SHOW_CC"))
            fprintf(stderr, "tur: the cached library unit %016llx names bindings this program "
                            "does not have; emitting it\n", (unsigned long long)c->key);
        c->no_restore = true;
        ok = false;
    }
    if (ok) {
        buf_write(lib_r, text.data, text.len);
        /* Most recently used last, for the pruning. */
        char path[1100];
        if (split_lib_cache_file(c->key, ".lib", path, sizeof path))
            (void)prelude_object_reuse(path);
        if (split_lib_cache_file(split_lib_hash(text.data, text.len), ".libtext",
                                 path, sizeof path))
            (void)prelude_object_reuse(path);
        if (getenv("TUR_SHOW_CC"))
            fprintf(stderr, "tur: library unit from the cache (%016llx)\n", (unsigned long long)c->key);
    }
    buf_free(&text);
    buf_free(&carry);
    return ok;
}

/* After the library unit's emission: write its entry, unless an equal one is
 * there.  One that differs is replaced -- and, under
 * TUR_SPLIT_LIB_CACHE=verify, reported and kept, since then the key failed
 * to tell two different units apart. */
void split_lib_cache_record(SplitLibCache *c, const Expr *prog, const Buf *lib_r) {
    if (getenv("TUR_SHOW_CC"))
        fprintf(stderr, "tur: library unit emitted (text %016llx)\n",
                (unsigned long long)split_lib_hash(lib_r->data, lib_r->len));
    if (!c->on) return;
    Buf carry; buf_init(&carry);
    emit_split_carry_save(&carry, prog);
    Buf old_text, old_carry;
    buf_init(&old_text);
    buf_init(&old_carry);
    bool had = split_lib_cache_read(c, &old_text, &old_carry);
    bool same_text = had && old_text.len == lib_r->len &&
                     memcmp(old_text.data, lib_r->data, lib_r->len) == 0;
    bool same_carry = had && old_carry.len == carry.len &&
                      memcmp(old_carry.data, carry.data, carry.len) == 0;
    if (had && (!same_text || !same_carry)) {
        if (c->verify || getenv("TUR_SHOW_CC"))
            fprintf(stderr, "tur: split library cache: entry %016llx differs from the emitted "
                            "library unit (%s%s%s)\n", (unsigned long long)c->key,
                    same_text ? "" : "text", !same_text && !same_carry ? ", " : "",
                    same_carry ? "" : "carried state");
    } else if (had && c->verify && getenv("TUR_SHOW_CC")) {
        fprintf(stderr, "tur: split library cache: entry %016llx verified\n",
                (unsigned long long)c->key);
    }
    if (!had || (!c->verify && (!same_text || !same_carry)))
        split_lib_cache_write(c, lib_r, &carry);
    buf_free(&old_text);
    buf_free(&old_carry);
    buf_free(&carry);
}

/* Report WHY the split did not engage, under TUR_JIT_TIMING=1.
 *
 * The split fails closed -- a rejected hash just means the full preamble is
 * emitted, which is correct but slow -- so a disengage is invisible in every
 * observable except wall-clock.  That is how a hoisted inline-C `#include`
 * disabled the fast path for the entire httpd-*, image-*, and C-binding-spice
 * corpus without anyone noticing.  Note that the absence of TUR-W0071 is NOT
 * evidence the split engaged: W0071 means engaged-then-failed-to-compile, and a
 * never-engaged build is silent.  See
 * docs/archive/jit-s2-split-disengages-on-hoisted-inline-c-include.md. */
static bool jit_split_reject(const char *why) {
    const char *v = getenv("TUR_JIT_TIMING");
    if (v && *v && strcmp(v, "0") != 0)
        fprintf(stderr, "TUR_JIT_TIMING\tsplit\tdisengaged\t%s\n", why);
    return false;
}

/* cc-path-preamble-split-plan: `tur build --runtime=split` calls this as well
 * as the JIT, so the two paths cannot drift. */
/* NOT under `#ifdef TUR_HAVE_JIT`, deliberately.  These two started as a JIT
 * probe -- the name still says so -- but `--runtime=split` made the CC path
 * call jit_try_split_preamble from cmd_build, and cmd_build exists in every
 * build.  Leaving the definition inside the engine's guard while the
 * declaration and two of the three call sites sat outside it compiled fine in
 * any -DTUR_JIT=ON tree (which is every tree this was developed in) and failed
 * every non-JIT build with
 * `jit_try_split_preamble used but never defined [-Werror]`.
 * The declarations image they read moved to tur_core for the same reason. */
/* S2 (findings 25): swap an emitted TU's fixed preamble for the committed
 * declarations region when this compiler still matches the committed
 * artifacts.  Returns true and fills `out` with
 * [hoisted user prefix][committed decls][program half]; false (out
 * untouched) on hash mismatch, missing marker, or TUR_JIT_NO_SPLIT=1.
 * Shared by cmd_jit and the J2 REPL image build.  See cmd_jit's call site
 * for the full rationale. */
bool jit_try_split_preamble(Buf *csrc, Buf *out) {
    const char *no_split = getenv("TUR_JIT_NO_SPLIT");
    if (no_split && *no_split && strcmp(no_split, "0") != 0)
        return jit_split_reject("TUR_JIT_NO_SPLIT set");
    if (!csrc->data) return jit_split_reject("no emitted source");
    Buf probe;
    buf_init(&probe);
    emit_rt_split_source(&probe);
    uint64_t cur = tur_hamt_hash_xxh64(probe.data, probe.len);
    const char *dbg = getenv("TUR_JIT_SPLIT_DEBUG");
    if (dbg) {
        fprintf(stderr, "split-debug: probe=%016llx len=%zu committed=%016llx\n",
                (unsigned long long)cur, probe.len,
                (unsigned long long)tur_rt_split_hash);
        if (dbg[0] == '/' || (dbg[0] && dbg[1] == ':')) {
            FILE *pf = fopen(dbg, "wb");
            if (pf) { fwrite(probe.data, 1, probe.len, pf); fclose(pf); }
        }
    }
    buf_free(&probe);
    if (cur != tur_rt_split_hash)
        return jit_split_reject("preamble hash != committed artifact "
                                "(regenerate with tools/gen-runtime-split.py)");
    buf_putc(csrc, '\0');
    csrc->len--;   /* NUL-terminate for strstr, keep logical len */
    static const char pre_start[] = "/* generated by tur (phase 2) */\n";
    static const char pre_end[] =
        "/* ==== tur: end of fixed runtime preamble ==== */\n";
    const char *ps = strstr(csrc->data, pre_start);
    const char *pe = ps ? strstr(ps, pre_end) : NULL;
    if (!ps || !pe) return jit_split_reject("preamble markers not found");
    const char *after = pe + sizeof(pre_end) - 1;
    buf_write(out, csrc->data, (size_t)(ps - csrc->data));
    buf_write(out, tur_rt_split_decls, tur_rt_split_decls_len);
    /* Carry THIS program's own project includes across the swap.
     *
     * The preamble is not actually fixed across programs, which is the premise
     * the whole split rests on and the one place it is false: the emitter
     * writes EITHER `#include "hamt.h"` (a program the compiler lowers
     * directly onto the hamt API) OR loose `extern void *tur_hamt_new();`
     * declarations from stdlib's extern-c -- never both, because gcc rejects
     * the combination outright:
     *
     *   error: conflicting types for 'tur_hamt_new'; have 'void *()'
     *   note: previous declaration ... with type 'Hamt *(void)'
     *
     * The committed decls region is generated from ONE canonical emission, so
     * whichever shape that program happened to use was frozen for everybody.
     * Carrying the include unconditionally broke every loose-extern program;
     * guarding it out (`#ifndef TUR_RT_SPLIT_HOSTED`, still in the generated
     * header) broke every program that needs the header, which is what took
     * `hamt-lowering-basic` down with a screenful of implicit declarations.
     *
     * Re-emitting the includes the REPLACED REGION actually contained gives
     * each program exactly what it emitted, which is what the monolithic build
     * does -- so neither shape has to lose. Header guards make a duplicate
     * harmless if the decls region already pulled the same file in.
     *
     * Quoted includes only: `<...>` system headers are the decls region's
     * business, and on Windows re-emitting them is actively harmful (the JIT
     * cannot digest the MinGW SDK headers -- see
     * docs/archive/jit-windows-support-spike.md). */
    for (const char *p = ps; p < pe; ) {
        const char *eol = (const char *)memchr(p, '\n', (size_t)(pe - p));
        size_t len = eol ? (size_t)(eol - p + 1) : (size_t)(pe - p);
        if (len > sizeof("#include \"") - 1 &&
            strncmp(p, "#include \"", sizeof("#include \"") - 1) == 0)
            buf_write(out, p, len);
        if (!eol) break;
        p = eol + 1;
    }
    buf_write(out, after, csrc->len - (size_t)(after - csrc->data));
    /* TUR_JIT_DUMP_C=<path>: the exact text handed to c2mir.  A c2mir
     * diagnostic names <tur-jit>:LINE:COL, and until this existed there was no
     * way to see what was on that line: the source exists only in memory, it is
     * not `tur emit-c` output (the preamble has been swapped for the decls
     * image), and reconstructing it by hand gets the line numbers wrong.
     * TUR_JIT_SPLIT_DEBUG=<path> dumps the preamble PROBE, which is a different
     * text and cannot answer this. */
    const char *dump = getenv("TUR_JIT_DUMP_C");
    if (dump && *dump) {
        FILE *df = fopen(dump, "wb");
        if (df) { fwrite(out->data, 1, out->len, df); fclose(df); }
    }
    return true;
}

/* S2 (jit-engine-plan, findings 19.4): dump the feature-complete single-file
 * runtime preamble -- every program-gated block on, rc/GC in archive mode,
 * ending at the preamble marker -- for the split-generation tool
 * (tools/gen-runtime-split.py), or with --hash just the xxHash64 of that
 * text.  The hash spelling here IS the JIT-time compare: both sides call
 * tur_hamt_hash_xxh64 over the same emission, so the recorded artifact hash
 * and cmd_jit's probe can never disagree on hash function or input framing.
 *
 * Archive mode is hard-set rather than probed so the generated text is a
 * property of the compiler, not of which build tree generated it; cmd_jit's
 * probe runs under real process state, and a JIT invocation with no runtime
 * archive (rc/GC emitted as definitions) therefore hashes differently and
 * falls back to full-preamble emission -- the guard is self-enforcing. */
int cmd_emit_rt_split(int argc, char **argv) {
    bool hash_only = false;
    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "--hash") == 0) { hash_only = true; continue; }
        fprintf(stderr, "usage: tur emit-rt-split [--hash]\n");
        return 2;
    }
    emit_set_rcgc_from_archive(true);
    Buf out; buf_init(&out);
    emit_rt_split_source(&out);
    if (hash_only) {
        printf("%016llx\n",
               (unsigned long long)tur_hamt_hash_xxh64(out.data, out.len));
    } else {
        fwrite(out.data, 1, out.len, stdout);
    }
    buf_free(&out);
    return 0;
}
