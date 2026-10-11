#include "stdlib_autoload.h"

#include "diag.h"
#include "reader.h"
#include "runtime/globals.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Auto-loaded stdlib files.  Shared by compile_to_c (single-file) and
 * compile_to_h / compile_to_implementation (project-mode multi-file) so
 * spice code in `tur build .` sees `Cons`, `tnil?`, `Option`, etc. without
 * explicit imports -- matching single-file semantics.  Each TU embeds its
 * own static copy of stdlib defns; emit_module.c's
 * `is_from_stdlib`-aware paths static-ify them per TU so multi-TU links
 * don't see duplicate symbols. */
static const char *const autoload_files_[] = {
    "macros.tur",
    "safe.tur",
    "contract.tur",
    "hamt.tur",
    "typeclass-eq.tur",
    "typeclass-ord.tur",
    "typeclass-functor.tur",
    "typeclass-clone.tur",
    "typeclass-drop.tur",
    "typeclass-hash.tur",
    "typeclass-applicative.tur",
    "typeclass-alternative.tur",
    "typeclass-monad.tur",
    "typeclass-monaderror.tur",
    "typeclass-bifunctor.tur",
    /* parsec-guide-plan P1/P2: the Char type a `#\A` literal elaborates to.
     * After the Eq/Ord stubs, whose classes it instantiates. */
    "char.tur",
    "map.tur",
    "vec.tur",
    "slice.tur",
    "option.tur",
    "result.tur",
    "pair.tur",
    "tuple.tur",
    "list.tur",
    "grid.tur",
    "zipper.tur",
    "set.tur",
    "mutmap.tur",
    "json.tur",
    "schema.tur",
    "sym.tur",
    "unique.tur",
    /* SX1: backtrackable state.  Was gated behind the `backtrackable-state`
     * experiment until it graduated 2026-08-29; an ordinary list member now. */
    "trail.tur",
    /* RM3 (regions): `with-region`, the lifetime-only bracket.  Loaded after
     * trail.tur since it is the trail's sibling in the region/trail split.  The
     * body is a bare forwarder, so with `--enable=regions` off the defn is an
     * ordinary identity call and adds no behaviour; the emitter opens a region
     * around a call to it only when the flag is on. */
    "region.tur",
    NULL
};

/* SX1 (solver-extension-plan) GRADUATED 2026-08-29.  stdlib/trail.tur used to
 * be spliced in here only when the `backtrackable-state` experiment was on --
 * gating the autoload rather than a call site, so that with the experiment off
 * the module was simply absent and `bt-set!` was an unknown function like any
 * other.  The experiment is gone and trail.tur is an ordinary member of
 * `autoload_files_` above, so this is now a plain accessor.
 *
 * Kept as a function rather than collapsed into the array because both front
 * ends (the CLI and src/web/wasm_lsp.c) go through it, and because
 * tur_stdlib_prepend_forms reads the list through this accessor -- reading the
 * raw array there once made the single-file path disagree with project mode
 * about what was in scope. */
/* saffron-lang-plan S6 / r7rs-lang-plan R1: when the ENTRY file's language
 * has a prelude (`g_lang_prelude`, from LangTraits.prelude), the same list
 * with that prelude appended.
 *
 * COPIED from `autoload_files_` at first use rather than written out a second
 * time, so a file added to the base list is picked up here with no second edit
 * -- the drift this file already guards against in the other direction (reading
 * the raw array instead of the accessor made the single-file path disagree with
 * project mode about what was in scope).
 *
 * The prelude goes LAST so every stdlib name it adapts -- `Vec`, `Map`,
 * `Option`, the typeclasses -- is already in scope when it is read.
 *
 * The copy is rebuilt only when the prelude it was built for changes (the
 * REPL can switch languages mid-process), so what a compile loads depends on
 * the current entry file alone, never on what ran before it. */
static const char *autoload_files_prelude_[64];
static const char *autoload_files_prelude_for_ = NULL;

const char *const *tur_stdlib_autoload_files(void) {
    if (!g_lang_prelude) return autoload_files_;
    if (autoload_files_prelude_for_ != g_lang_prelude) {
        size_t n = 0;
        while (autoload_files_[n] != NULL &&
               n + 2 < sizeof(autoload_files_prelude_) /
                       sizeof(autoload_files_prelude_[0])) {
            autoload_files_prelude_[n] = autoload_files_[n];
            n++;
        }
        autoload_files_prelude_[n++] = g_lang_prelude;
        autoload_files_prelude_[n]   = NULL;
        autoload_files_prelude_for_  = g_lang_prelude;
    }
    return (const char *const *)autoload_files_prelude_;
}

void tur_source_file_apply_lang_header(SourceFile *sf, const char *whole_src,
                                       size_t whole_len) {
    if (!sf || !whole_src) return;
    const char  *rest    = whole_src;
    size_t       rest_len = whole_len;
    LangDialect  dl       = LANG_TURMERIC;
    ReaderType   rt = detect_lang_dialect(whole_src, whole_len, &rest, &rest_len,
                                          NULL, NULL, &dl);
    if (rest == whole_src || !reader_type_is_implemented(rt)) return;
    sf->src         = (char *)rest;
    sf->len         = rest_len;
    sf->head_offset = (size_t)(rest - whole_src);
    sf->reader_type = rt;
    sf->lang        = dl;
}

static const char *basename_of(const char *path) {
    const char *slash = strrchr(path, '/');
#ifdef _WIN32
    const char *back = strrchr(path, '\\');
    if (back && (!slash || back > slash)) slash = back;
#endif
    return slash ? slash + 1 : path;
}

/* Read a whole file without printing on failure -- a missing autoload file is
 * expected during --no-auto-stdlib builds and must not become stderr noise. */
static int read_file_quiet(const char *path, char **out, size_t *out_len) {
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

uint32_t tur_stdlib_prepend_forms(Arena *arena, SymbolTable *st,
                                  const char *stdlib_dir,
                                  const char *entry_path,
                                  bool no_auto_stdlib,
                                  Form ***forms_in_out,
                                  uint32_t *nforms_in_out,
                                  uint8_t *file_id_in_out) {
    if (!stdlib_dir || !*stdlib_dir) stdlib_dir = "stdlib";

    /* Through the accessor, not the raw array: that is where the SX1
     * experiment gate adds trail.tur, and reading `autoload_files_` directly
     * here meant the single-file path silently ignored it while project mode
     * honoured it -- the same program compiling two different ways. */
    const char *const *files = tur_stdlib_autoload_files();

    int no_stdlib_skip_from = -1;
    if (no_auto_stdlib) {
        const char *input_base = basename_of(entry_path);
        for (int j = 0; files[j] != NULL; j++) {
            if (strcmp(input_base, files[j]) == 0) {
                no_stdlib_skip_from = j;
                break;
            }
        }
    }

    /* SX1: recomputed per call, never accumulated.  The REPL and the harnesses
     * run several compiles in one process, and a sticky flag would let a compile
     * that loaded trail.tur license the guard in one that did not. */
    g_trail_autoloaded = false;

    uint32_t total = 0;
    Form **all = NULL;
    for (int i = 0; files[i] != NULL; i++) {
        if (no_stdlib_skip_from >= 0 && i >= no_stdlib_skip_from) continue;
        char path_buf[4096];
        int pn = snprintf(path_buf, sizeof(path_buf), "%s/%s",
                          stdlib_dir, files[i]);
        if (pn < 0 || (size_t)pn >= sizeof(path_buf)) {
            fprintf(stderr, "tur: stdlib path too long for '%s' (dir='%s')\n",
                    files[i], stdlib_dir);
            continue;
        }
        char *stdlib_src = NULL;
        size_t stdlib_len = 0;
        if (read_file_quiet(path_buf, &stdlib_src, &stdlib_len) != 0)
            continue;

        /* Set only after the read SUCCEEDS: a missing stdlib/trail.tur is
         * skipped silently above, and its autolink marker would be missing from
         * the output too, so the guard must not be emitted for it either. */
        if (strcmp(files[i], "trail.tur") == 0) g_trail_autoloaded = true;

        char *src_copy = (char *)arena_alloc(arena, stdlib_len);
        memcpy(src_copy, stdlib_src, stdlib_len);
        char *path_copy = (char *)arena_alloc(arena, strlen(path_buf) + 1);
        memcpy(path_copy, path_buf, strlen(path_buf) + 1);

        SourceFile *stdlib_file = (SourceFile *)arena_alloc(arena, sizeof(SourceFile));
        *stdlib_file = (SourceFile){0};
        stdlib_file->path = path_copy;
        stdlib_file->src = src_copy;
        stdlib_file->len = stdlib_len;
        stdlib_file->file_id = (*file_id_in_out)++;
        stdlib_file->reader_type = READER_TURMERIC;
        /* saffron-lang-plan S6: honour a `#lang` line in a stdlib file.  The
         * Saffron prelude has one -- its own bodies use D4 truthiness, a hard
         * error under the Turmeric dialect.  Shared with load_project_prelude;
         * see the helper's comment for why it is shared. */
        tur_source_file_apply_lang_header(stdlib_file, src_copy, stdlib_len);
        diag_register_file(stdlib_file);

        uint32_t n = 0;
        Form **fs = read_all(arena, st, stdlib_file, &n);
        if (fs && n > 0) {
            Form **new_all = (Form **)arena_alloc(arena,
                (total + n) * sizeof(Form *));
            for (uint32_t j = 0; j < total; j++) new_all[j] = all[j];
            for (uint32_t j = 0; j < n; j++) new_all[total + j] = fs[j];
            all = new_all;
            total += n;
        }
        free(stdlib_src);
    }

    if (all && total > 0) {
        Form **out = (Form **)arena_alloc(arena,
            (*nforms_in_out + total) * sizeof(Form *));
        for (uint32_t i = 0; i < total; i++) out[i] = all[i];
        for (uint32_t i = 0; i < *nforms_in_out; i++)
            out[total + i] = (*forms_in_out)[i];
        *forms_in_out = out;
        *nforms_in_out += total;
    }
    return total;
}
