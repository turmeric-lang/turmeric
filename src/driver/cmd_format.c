/* cmd_format.c -- `tur format`, `tur fmt` and `tur parse-check`: the source
 * formatter's front ends, and the AST comparison that checks a sweet-exp file
 * against its s-expression twin.
 *
 * Part of the compiler driver (src/driver/); driver.h says how the files
 * share declarations. */

#include "driver.h"
#include "commands.h"

#include "arena.h"
#include "fmt.h"
#include "forms.h"
#include "reader.h"
#include "reader_macros.h"
#include "lang_dialects.h"  /* `tur dialects`: the `#lang` base axis */
#include "symbols.h"

/* The `--lang` vocabulary shared by `tur fmt` and `tur format`: a reader
 * name, or (lang-flags-take-different-vocabularies) any base `tur dialects`
 * lists, resolved to its reader -- formatting is a reader concern, so
 * `saffron` formats as `turmeric` and `saffron/sweet` as `sweet`.  False for
 * a name neither table knows. */
static bool fmt_reader_from_lang_name(const char *lang, ReaderType *out) {
    if (strcmp(lang, "turmeric") == 0 || strcmp(lang, "tur") == 0) {
        *out = READER_TURMERIC;
    } else if (strcmp(lang, "sweet-exp") == 0 || strcmp(lang, "sweet") == 0 || strcmp(lang, "tursweet") == 0) {
        *out = READER_SWEET;
    } else if (strcmp(lang, "curly-infix") == 0) {
        *out = READER_CURLY_INFIX;
    } else if (strcmp(lang, "neoteric") == 0) {
        *out = READER_NEOTERIC;
    } else if (strcmp(lang, "r7rs") == 0) {
        /* r7rs-lang-plan R9: a Scheme buffer with no `#lang` line
         * (an editor selection) -- re-indented, never reprinted. */
        *out = READER_R7RS;
    } else if (strcmp(lang, "r7rs/sweet") == 0) {
        /* Checked, and kept as written (fmt_format_buffer). */
        *out = READER_R7RS_SWEET;
    } else {
        LangDialect base_dl = LANG_TURMERIC;
        ReaderType  base_rt = READER_TURMERIC;
        if (!lang_base_lookup(lang, strlen(lang), &base_dl, &base_rt))
            return false;
        (void)base_dl;
        *out = base_rt;
    }
    return true;
}

/* The tail `tur format` shares across readers: print `out`, or with --check /
 * --diff compare it against the original `src` instead.  Returns the exit
 * code. */
static int format_emit(const char *path, const char *src, size_t len,
                       Buf *out_buf, bool check_only, bool diff_mode) {
    int rc = 0;
    Buf out = *out_buf;
    if (check_only) {
        /* Exit 1 if already-formatted output differs from input */
        bool same = (out.len == len) && (memcmp(out.data, src, len) == 0);
        if (!same) {
            if (path) fprintf(stderr, "tur: %s is not formatted\n", path);
            rc = 1;
        }
    } else if (diff_mode) {
        bool same = (out.len == len) && (memcmp(out.data, src, len) == 0);
        if (!same) {
            /* Write original and formatted to temp files, run diff -u. */
            char orig_tmp[512];
            snprintf(orig_tmp, sizeof(orig_tmp), "%s/tur-fmt-orig-XXXXXX", tur_temp_dir());
            int orig_fd = mkstemp(orig_tmp);
            if (orig_fd >= 0) {
                ssize_t _wr1 = write(orig_fd, src, len); (void)_wr1;
                close(orig_fd);
            }
            char new_tmp[512];
            snprintf(new_tmp, sizeof(new_tmp), "%s/tur-fmt-new-XXXXXX", tur_temp_dir());
            int new_fd = mkstemp(new_tmp);
            if (new_fd >= 0) {
                ssize_t _wr2 = write(new_fd, out.data, out.len); (void)_wr2;
                close(new_fd);
            }
            const char *label = path ? path : "<stdin>";
            /* WP2 (D-7): `'%s'` is not quoting -- a `'` in the path ends
             * the argument, and cmd.exe does not read `'` as a quote at
             * all.  tur_shell_quote is the one that handles both. */
            Buf diff_cmd; buf_init(&diff_cmd);
            bool dq = true;
            buf_puts(&diff_cmd, "diff -u -L ");
            dq = drv_buf_put_quoted(&diff_cmd, label) && dq;
            buf_puts(&diff_cmd, " -L ");
            dq = drv_buf_put_quoted(&diff_cmd, label) && dq;
            buf_putc(&diff_cmd, ' ');
            dq = drv_buf_put_quoted(&diff_cmd, orig_tmp) && dq;
            buf_putc(&diff_cmd, ' ');
            dq = drv_buf_put_quoted(&diff_cmd, new_tmp) && dq;
            buf_putc(&diff_cmd, '\0');
            int diff_rc = dq ? system(diff_cmd.data) : 2;
            buf_free(&diff_cmd);
            unlink(orig_tmp);
            unlink(new_tmp);
            /* diff exits 1 when files differ, 0 when same */
            if (diff_rc != 0) rc = 1;
        }
    } else {
        buf_to_file(&out, stdout);
    }
    return rc;
}

/* tur format [--check|--diff] [--lang <dialect>] [file]
 * Read source from file (or stdin if no file given), format it, and write to
 * stdout.  --check: exit 1 if file is not already formatted (no output).
 * --diff: print unified diff if file would change; exit 1 if changed.
 *
 * format-subcommand-shreds-a-sweet-buffer: the reader comes from `--lang`,
 * else the file's extension, and a `#lang` line is honoured either way.
 * Anything that is not plain Turmeric goes through fmt_format_document --
 * the same path as `tur fmt` -- instead of being read under the Turmeric
 * reader, which printed a sweet buffer one token per line. */
static int cmd_format(const char *path, bool check_only, bool diff_mode,
                      bool lang_set, ReaderType force_lang) {
    char  *src = NULL;
    size_t len = 0;

    if (path) {
        if (drv_read_entire_file(path, &src, &len) != 0) return 2;
    } else {
        /* Read from stdin */
        size_t cap = 4096;
        src = (char *)malloc(cap);
        if (!src) { fprintf(stderr, "tur: oom\n"); return 2; }
        len = 0;
        int c;
        while ((c = fgetc(stdin)) != EOF) {
            if (len + 1 >= cap) {
                cap *= 2;
                char *tmp = (char *)realloc(src, cap);
                if (!tmp) { free(src); fprintf(stderr, "tur: oom\n"); return 2; }
                src = tmp;
            }
            src[len++] = (char)c;
        }
        src[len] = '\0';
    }

    ReaderType rtype = lang_set ? force_lang : reader_type_from_extension(path);
    {
        const char *body = src;
        size_t body_len = len;
        LangDialect dl = LANG_TURMERIC;
        (void)detect_lang_dialect(src, len, &body, &body_len, NULL, NULL, &dl);
        if (rtype != READER_TURMERIC || body != src) {
            Buf out;
            int rc;
            if (fmt_format_document(path ? path : "<stdin>", src, len,
                                    rtype, &out) != 0) {
                rc = 1;
            } else {
                rc = format_emit(path, src, len, &out, check_only, diff_mode);
                buf_free(&out);
            }
            free(src);
            return rc;
        }
    }

    SourceFile file = {0};
    file.path        = path ? path : "<stdin>";
    file.src         = src;
    file.len         = len;
    file.file_id     = 0;
    file.reader_type = READER_TURMERIC;
    diag_register_file(&file);

    Arena arena;
    arena_init(&arena, 0);
    SymbolTable st;
    symtab_init(&st, &arena);

    /* RM4 follow-up: preload manifest reader-macros so `tur format`
     * doesn't fail to parse files that use `#foo{...}` invocations
     * registered in the spice's build.tur. Stdin-mode (path == NULL)
     * has no enclosing file to walk up from -- skip. */
    ReaderMacroRegistry reader_macros_reg;
    reader_macros_init(&reader_macros_reg, &arena);
    /* Transitive-RM decision #2: batch compile is strict -- duplicate
     * `(reader-macros/define ...)` is a hard error. REPL leaves this
     * flag false on env->reader_macros for iterative-redefinition UX. */
    reader_macros_reg.strict = true;
    if (path) {
        int rm_n = 0;
        char **rm_p = discover_manifest_reader_macros(path, &rm_n);
        for (int i = 0; i < rm_n; ++i) {
            if (reader_macros_load_file(&arena, &st, rm_p[i],
                                        &reader_macros_reg) != 0) {
                free_reader_macro_paths(rm_p, rm_n);
                symtab_free(&st); arena_free(&arena); free(src);
                return 1;
            }
        }
        free_reader_macro_paths(rm_p, rm_n);
    }

    uint32_t nforms = 0;
    Form **forms = read_all_with_registry(&arena, &st, &file,
                                          &reader_macros_reg, &nforms);

    int rc = 0;
    if (!forms || diag_had_error()) {
        rc = 1;
    } else {
        FmtOptions opts = {0};
        opts.indent_width = 2;
        opts.line_width   = 80;
        opts.src          = src;
        opts.src_len      = len;

        Buf out;
        buf_init(&out);
        if (fmt_print(&out, forms, nforms, opts) != 0) {
            fprintf(stderr, "tur: fmt_print failed\n");
            rc = 1;
        } else {
            rc = format_emit(path, src, len, &out, check_only, diff_mode);
        }
        buf_free(&out);
    }

    symtab_free(&st);
    arena_free(&arena);
    free(src);
    return rc;
}

/* tur parse-check <a> <b>
 * Read two files and compare the form trees they produce. The first file is
 * read as turmeric s-expressions, the second as sweet-exp, unless an explicit
 * `#lang` directive on either file overrides the forced reader. Used by
 * tools/check-guide-pairs.py to verify a guide's turmeric+sweet-exp toggle
 * pair reads to the same AST.
 *
 * Exit codes: 0 = ASTs equal, 1 = mismatch, 2 = read/parse error. */

/* parse-check canonicalization: a simple type annotation reads as an
 * `F_TYPE_ANN` wrapping the type when written spaced (`: int`) but as a plain
 * `F_KEYWORD` when written unspaced (`:int`). Both elaborate identically, so
 * for AST-equality purposes fold `F_TYPE_ANN(sym)` down to `F_KEYWORD(sym)`.
 * Compound annotations (`: (fn [:int] :int)`, `: list<int>`) keep their
 * `F_TYPE_ANN` wrapper -- both toggle siblings produce it, so they still match.
 * Applied to both sides before form_print, so a guide's traditional `[] : int`
 * and its sweet-exp `[] :int` compare equal. */
static void parse_check_canon(SymbolTable *st, Form *f) {
    if (!f) return;
    switch (f->tag) {
        case F_LIST: case F_VEC: case F_MAP: case F_SET:
        case F_QUOTE: case F_QUASIQUOTE: case F_UNQUOTE:
        case F_UNQUOTE_SPLICING: case F_TYPE_ANN: case F_CONTRACT_TYPE:
        case F_READER_COND: case F_RANGE_VAR:
        case F_MAP_LITERAL: case F_SET_LITERAL: case F_ROW_LITERAL:
            for (uint32_t i = 0; i < f->as.list.len; i++)
                parse_check_canon(st, f->as.list.items[i]);
            break;
        default:
            break;
    }
    /* Fold `: T` (F_TYPE_ANN wrapping a single atom) to the `:T` keyword form.
     * The inner is F_SYM for ordinary type names, but a literal for the built-in
     * type names nil/true/false, which the reader produces as F_NIL/F_BOOL. */
    if (f->tag == F_TYPE_ANN && f->as.list.len == 1) {
        const Form  *inner = f->as.list.items[0];
        const Symbol *kw   = NULL;
        switch (inner->tag) {
            case F_SYM:  kw = inner->as.sym; break;
            case F_NIL:  kw = symtab_intern(st, strslice("nil", 3)); break;
            case F_BOOL: kw = inner->as.b ? symtab_intern(st, strslice("true", 4))
                                          : symtab_intern(st, strslice("false", 5));
                         break;
            default:     break;
        }
        if (kw) {
            f->tag    = F_KEYWORD;
            f->as.sym = kw;
        }
    }
}

/* Read `path`, select the reader (forced unless a `#lang` directive is
 * present), parse all top-level forms, and serialize each with form_print into
 * `out` (one per line). Returns 0 on success, non-zero on read/parse error. */
static int parse_check_read(const char *path, ReaderType forced, Buf *out) {
    char  *src = NULL;
    size_t len = 0;
    if (drv_read_entire_file(path, &src, &len) != 0) return 2;

    /* Honour an explicit `#lang` directive; otherwise force by position.
     * detect_lang leaves out_rest == src when no directive is present, and
     * otherwise advances past the directive so the reader does not choke on
     * the leading '#'. */
    const char *rest = src;
    size_t      rest_len = len;
    ReaderType  detected = detect_lang(src, len, &rest, &rest_len, NULL, NULL);
    ReaderType  reader   = forced;
    if (rest != src) {
        /* A `#lang` directive was present -- honour it and use the
         * directive-stripped source. */
        if (!reader_type_is_implemented(detected)) {
            fprintf(stderr, "tur: %s: unknown or unimplemented #lang directive\n",
                    path);
            free(src);
            return 2;
        }
        reader = detected;
    } else {
        /* No directive: read the whole source with the forced reader. */
        rest     = src;
        rest_len = len;
    }

    /* Fresh diagnostic slate per file -- see begin_compile_unit.
     * cmd_parse_check reads two files in one process, so a stale `had_error_`
     * from the first must not leak into the second's `diag_had_error()` gate.
     * diag_reset alone, not begin_compile_unit, on purpose: this only reads,
     * so the per-compile reasserts and refinement stats do not apply. */
    diag_reset();

    SourceFile file = {0};
    file.path        = path;
    file.src         = rest;
    file.len         = rest_len;
    file.head_offset = (size_t)(rest - src);
    file.file_id     = 0;
    file.reader_type = reader;
    diag_register_file(&file);

    Arena arena;
    arena_init(&arena, 0);
    SymbolTable st;
    symtab_init(&st, &arena);

    uint32_t nforms = 0;
    Form **forms = read_all_with_registry(&arena, &st, &file, NULL, &nforms);

    int rc = 0;
    if (!forms || diag_had_error()) {
        rc = 2;
    } else {
        for (uint32_t i = 0; i < nforms; i++) {
            parse_check_canon(&st, forms[i]);
            form_print(out, forms[i]);
            buf_putc(out, '\n');
        }
    }

    symtab_free(&st);
    arena_free(&arena);
    free(src);
    return rc;
}

static int cmd_parse_check(const char *path_a, const char *path_b) {
    Buf sa, sb;
    buf_init(&sa);
    buf_init(&sb);

    int rc;
    if (parse_check_read(path_a, READER_TURMERIC, &sa) != 0 ||
        parse_check_read(path_b, READER_SWEET, &sb) != 0) {
        rc = 2;
    } else if (sa.len == sb.len &&
               (sa.len == 0 || memcmp(sa.data, sb.data, sa.len) == 0)) {
        rc = 0;
    } else {
        fprintf(stderr, "tur: parse-check mismatch between %s and %s\n",
                path_a, path_b);
        fprintf(stderr, "--- %s ---\n%.*s", path_a, (int)sa.len,
                sa.data ? sa.data : "");
        fprintf(stderr, "--- %s ---\n%.*s", path_b, (int)sb.len,
                sb.data ? sb.data : "");
        rc = 1;
    }

    buf_free(&sa);
    buf_free(&sb);
    return rc;
}

/* ---------------------------------------------------------------------------
 * tur fmt -- formatter with directory walking and in-place writes
 * ---------------------------------------------------------------------------
 *
 * Modes:
 *   default (no --check/--diff/--stdout/--stdin):
 *     format files in-place; print names of changed files to stderr
 *   --check / --dry-run:
 *     exit 1 if any file would change; print names to stderr
 *   --diff:
 *     print unified diff for each file that would change; exit 1 if any
 *   --stdout <file>:
 *     format one file and write to stdout; do not modify the file
 *   --stdin [--lang <lang>]:
 *     read from stdin, format, write to stdout
 *
 * Exit codes: 0=ok, 1=would-change (--check) or I/O error, 2=CLI/parse error
 */

/* Directories to skip when walking */
static bool fmt_skip_dir(const char *name) {
    return strcmp(name, "build")          == 0
        || strcmp(name, ".git")           == 0
        || strcmp(name, ".tur-cache")     == 0
        || strcmp(name, ".turnb-cache")   == 0
        || strcmp(name, ".tur-repl-cache")== 0;
}

static bool fmt_is_tur_file(const char *name) {
    size_t n = strlen(name);
    if (n >= 4 && strcmp(name + n - 4, ".tur") == 0) return true;
    if (n >= 10 && strcmp(name + n - 10, ".tur.sweet") == 0) return true;
    return false;
}


/* Core: read src, parse, format, return formatted Buf.
 * Returns 0 on success with *out populated, -1 on error.
 *
 * The pipeline itself lives in fmt.c so the LSP's textDocument/formatting
 * handler -- which is linked into tur_core, not into main.c -- can reach the
 * same code instead of shelling out to this binary. */
/* saffron-lang-plan S8 (formatter half): format a source that may carry a
 * `#lang` directive.
 *
 * The formatter fed the WHOLE buffer to the reader, and the reader has already
 * had the directive stripped by every other entry point -- so a `#lang` line
 * reached it as source and came back
 * "error: unexpected character '#' (0x23)".  That made EVERY `#lang` file
 * unformattable, Saffron and `sweet-exp` alike: `tur fmt` on any of the
 * saffron-* fixtures failed the same way, so the dialect had no formatter at
 * all.  It surfaced only when a `#lang` file first appeared in stdlib (the
 * Saffron prelude), because `fmt-bootstrap-stdlib` formats stdlib and nothing
 * else formatted a `#lang` file.
 *
 * The directive is preserved VERBATIM rather than reprinted: it is not an
 * s-expression, the reader hands back only where it ended, and re-emitting it
 * from a parse would be inventing a canonical spelling for something with no
 * formatter rules of its own.
 *
 * The header also selects the reader when the caller had no better answer --
 * a `#lang turmeric/sweet` file must format under the sweet-exp printer, and
 * the extension alone does not say so. */
static int fmt_format_source(const char *path_label, const char *src, size_t len,
                              ReaderType rtype, Buf *out) {
    /* r7rs-lang-plan R9: the body moved to fmt.c (fmt_format_document) so the
     * LSP's textDocument/formatting -- linked into tur_core, not main.c --
     * formats a `#lang` document too; it handed the directive to the reader
     * and got a parse error, i.e. "no edits", for every `#lang` buffer. */
    return fmt_format_document(path_label, src, len, rtype, out);
}

typedef enum {
    FMT_MODE_INPLACE,
    FMT_MODE_CHECK,
    FMT_MODE_DIFF,
    FMT_MODE_STDOUT,
} FmtMode;

/* Process one file. Returns 0=unchanged, 1=changed/would-change, -1=error. */
static int fmt_process_file(const char *path, ReaderType force_lang,
                             FmtMode mode) {
    char  *src = NULL;
    size_t len = 0;
    if (drv_read_entire_file(path, &src, &len) != 0) return -1;

    ReaderType rtype = reader_type_from_extension(path);

    Buf out;
    int rc = fmt_format_source(path, src, len, rtype, &out);
    if (rc != 0) { free(src); return -1; }

    bool changed = (out.len != len) || (memcmp(out.data, src, len) != 0);

    if (!changed) {
        free(src);
        buf_free(&out);
        return 0;
    }

    /* File would change */
    switch (mode) {
        case FMT_MODE_CHECK:
            fprintf(stderr, "tur fmt: %s\n", path);
            free(src);
            buf_free(&out);
            return 1;

        case FMT_MODE_DIFF: {
            char orig_tmp[512];
            snprintf(orig_tmp, sizeof(orig_tmp), "%s/tur-fmt-orig-XXXXXX", tur_temp_dir());
            char new_tmp[512];
            snprintf(new_tmp, sizeof(new_tmp), "%s/tur-fmt-new-XXXXXX", tur_temp_dir());
            int ofd = mkstemp(orig_tmp);
            if (ofd >= 0) {
                ssize_t _w1 = write(ofd, src, len); (void)_w1;
                close(ofd);
            }
            free(src);
            int nfd = mkstemp(new_tmp);
            if (nfd >= 0) {
                ssize_t _w2 = write(nfd, out.data, out.len); (void)_w2;
                close(nfd);
            }
            Buf diff_cmd; buf_init(&diff_cmd);
            bool dq = true;
            buf_puts(&diff_cmd, "diff -u -L ");
            dq = drv_buf_put_quoted(&diff_cmd, path) && dq;
            buf_puts(&diff_cmd, " -L ");
            dq = drv_buf_put_quoted(&diff_cmd, path) && dq;
            buf_putc(&diff_cmd, ' ');
            dq = drv_buf_put_quoted(&diff_cmd, orig_tmp) && dq;
            buf_putc(&diff_cmd, ' ');
            dq = drv_buf_put_quoted(&diff_cmd, new_tmp) && dq;
            buf_putc(&diff_cmd, '\0');
            int diff_rc = dq ? system(diff_cmd.data) : 2;
            buf_free(&diff_cmd);
            unlink(orig_tmp);
            unlink(new_tmp);
            buf_free(&out);
            return (diff_rc != 0) ? 1 : 0;
        }

        case FMT_MODE_STDOUT:
            free(src);
            buf_to_file(&out, stdout);
            buf_free(&out);
            return 0;

        case FMT_MODE_INPLACE:
            free(src);
            if (buf_to_path(&out, path) != 0) {
                fprintf(stderr, "tur fmt: cannot write '%s': %s\n",
                        path, strerror(errno));
                buf_free(&out);
                return -1;
            }
            fprintf(stderr, "tur fmt: reformatted %s\n", path);
            buf_free(&out);
            return 1;
    }
    free(src);
    buf_free(&out);
    return 0;
}

/* Walk a directory, processing all .tur/.tur.sweet files.
 * Returns count of changed files, or -1 if an error occurred. */
static int fmt_walk(const char *path, ReaderType force_lang, FmtMode mode,
                    int *err_count) {
    struct stat st;
    if (stat(path, &st) != 0) {
        fprintf(stderr, "tur fmt: cannot stat '%s': %s\n", path, strerror(errno));
        (*err_count)++;
        return 0;
    }

    int changed = 0;

    if (S_ISREG(st.st_mode)) {
        if (!fmt_is_tur_file(path)) return 0;
        int r = fmt_process_file(path, force_lang, mode);
        if (r < 0) (*err_count)++;
        else if (r > 0) changed++;
        return changed;
    }

    if (!S_ISDIR(st.st_mode)) return 0;

    DIR *d = opendir(path);
    if (!d) {
        fprintf(stderr, "tur fmt: cannot open dir '%s': %s\n", path, strerror(errno));
        (*err_count)++;
        return 0;
    }

    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (ent->d_name[0] == '.') continue; /* skip hidden (incl. .git) */
        if (fmt_skip_dir(ent->d_name)) continue;

        char child[4096];
        snprintf(child, sizeof(child), "%s/%s", path, ent->d_name);

        /* tur_dirent_is_* fold in the stat() fallback for filesystems (and
         * Windows) that do not report d_type, so the DT_UNKNOWN arm this used
         * to carry -- a verbatim copy of both branches below -- is gone. */
        if (tur_dirent_is_dir(path, ent)) {
            changed += fmt_walk(child, force_lang, mode, err_count);
        } else if (tur_dirent_is_reg(path, ent) && fmt_is_tur_file(ent->d_name)) {
            int r = fmt_process_file(child, force_lang, mode);
            if (r < 0) (*err_count)++;
            else if (r > 0) changed++;
        }
    }
    closedir(d);
    return changed;
}

static int usage_fmt(void) {
    fprintf(stderr,
        "usage:\n"
        "  tur fmt [paths...]                   format .tur/.tur.sweet files in place\n"
        "  tur fmt --check [paths...]           exit 1 if any file would change\n"
        "  tur fmt --dry-run [paths...]         alias for --check\n"
        "  tur fmt --diff [paths...]            print unified diff of changes\n"
        "  tur fmt --stdout <file>              format file and print to stdout\n"
        "  tur fmt --stdin [--lang <dialect>]   format stdin and print to stdout\n"
        "\n"
        "  Paths may be files or directories.  Defaults to current directory.\n"
        "  Skips:  build/  .git/  .tur-cache/  .turnb-cache/  .tur-repl-cache/\n"
        "\n"
        "  Dialects for --lang:  turmeric (default)  sweet-exp  curly-infix  neoteric  r7rs\n"
        "  r7rs/sweet, or any base `tur dialects` lists (e.g. saffron, saffron/sweet),\n"
        "  which formats with that base's reader.\n"
        "  (tursweet is a deprecated alias for sweet-exp)\n"
        "\n"
        "Exit codes:\n"
        "  0   all files already formatted (or successfully written)\n"
        "  1   --check: at least one file would change; or I/O error\n"
        "  2   CLI / parse error\n"
        "\n"
        "Try 'tur --help' for global options.\n");
    return 0;
}

int cmd_fmt(int argc, char **argv) {
    bool check_only   = false;
    bool diff_mode    = false;
    bool stdin_mode   = false;
    const char *stdout_file = NULL;
    ReaderType  force_lang  = READER_TURMERIC;
    bool        lang_set    = false;

    /* Collect non-flag arguments as paths */
    char **paths  = NULL;
    int    npaths = 0, cap_paths = 0;

    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0)
            return usage_fmt();

        if (strcmp(argv[i], "--check") == 0 || strcmp(argv[i], "--dry-run") == 0) {
            check_only = true;
        } else if (strcmp(argv[i], "--diff") == 0) {
            diff_mode = true;
        } else if (strcmp(argv[i], "--stdin") == 0) {
            stdin_mode = true;
        } else if (strcmp(argv[i], "--stdout") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "tur fmt: --stdout requires a file argument\n");
                return 2;
            }
            stdout_file = argv[++i];
        } else if (strcmp(argv[i], "--lang") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "tur fmt: --lang requires a dialect argument\n");
                return 2;
            }
            const char *lang = argv[++i];
            if (!fmt_reader_from_lang_name(lang, &force_lang)) {
                fprintf(stderr,
                        "tur fmt: unknown dialect '%s' (expected a reader "
                        "name or a base `tur dialects` lists)\n", lang);
                return 2;
            }
            lang_set = true;
        } else if (argv[i][0] == '-') {
            fprintf(stderr, "tur fmt: unknown flag '%s'\n", argv[i]);
            return 2;
        } else {
            if (npaths >= cap_paths) {
                cap_paths = cap_paths ? cap_paths * 2 : 8;
                paths = (char **)realloc(paths, (size_t)cap_paths * sizeof(char *));
                if (!paths) { fprintf(stderr, "tur: oom\n"); return 1; }
            }
            paths[npaths++] = argv[i];
        }
    }

    /* Validate flag combinations */
    if ((check_only && diff_mode) || (check_only && stdin_mode) ||
        (diff_mode  && stdin_mode)) {
        fprintf(stderr, "tur fmt: incompatible flags\n");
        free(paths);
        return 2;
    }
    if (stdout_file && (check_only || diff_mode || stdin_mode || npaths > 0)) {
        fprintf(stderr, "tur fmt: --stdout cannot be combined with other modes or paths\n");
        free(paths);
        return 2;
    }
    if (lang_set && !stdin_mode && !stdout_file) {
        fprintf(stderr, "tur fmt: --lang is only meaningful with --stdin or --stdout\n");
        free(paths);
        return 2;
    }

    /* --stdin mode: read from stdin, format, write to stdout */
    if (stdin_mode) {
        size_t cap = 4096;
        char *src = (char *)malloc(cap);
        if (!src) { fprintf(stderr, "tur: oom\n"); free(paths); return 1; }
        size_t len = 0;
        int c;
        while ((c = fgetc(stdin)) != EOF) {
            if (len + 1 >= cap) {
                cap *= 2;
                char *tmp = (char *)realloc(src, cap);
                if (!tmp) { free(src); fprintf(stderr, "tur: oom\n"); free(paths); return 1; }
                src = tmp;
            }
            src[len++] = (char)c;
        }
        src[len] = '\0';

        Buf out;
        int rc = fmt_format_source("<stdin>", src, len, force_lang, &out);
        free(src);
        free(paths);
        if (rc != 0) return 1;
        buf_to_file(&out, stdout);
        buf_free(&out);
        return 0;
    }

    /* --stdout <file> mode: format one file, write to stdout */
    if (stdout_file) {
        char *src = NULL; size_t len = 0;
        if (drv_read_entire_file(stdout_file, &src, &len) != 0) { free(paths); return 1; }
        ReaderType rtype = lang_set ? force_lang : reader_type_from_extension(stdout_file);
        Buf out;
        int rc = fmt_format_source(stdout_file, src, len, rtype, &out);
        free(src);
        free(paths);
        if (rc != 0) return 1;
        buf_to_file(&out, stdout);
        buf_free(&out);
        return 0;
    }

    /* Default to cwd when no paths given */
    if (npaths == 0) {
        paths = (char **)realloc(paths, sizeof(char *));
        paths[0] = ".";
        npaths = 1;
    }

    FmtMode mode = check_only ? FMT_MODE_CHECK
                 : diff_mode  ? FMT_MODE_DIFF
                 :              FMT_MODE_INPLACE;

    int changed = 0, errors = 0;
    for (int i = 0; i < npaths; i++)
        changed += fmt_walk(paths[i], force_lang, mode, &errors);

    free(paths);

    if (errors > 0) return 1;
    if (changed > 0 && check_only) return 1;
    if (changed > 0 && diff_mode)  return 1;
    return 0;
}

/* -------------------------------------------------------------------------
 * Entry points: the command table in cli.c runs these for `tur <command>`.
 * ---------------------------------------------------------------------- */

int cmd_format_main(int argc, char **argv) {
    bool check_only = false;
    bool diff_mode  = false;
    const char *fmt_input = NULL;
    bool        lang_set  = false;
    ReaderType  force_lang = READER_TURMERIC;
    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0)
            return usage_format();
        if (strcmp(argv[i], "--check") == 0) {
            check_only = true;
        } else if (strcmp(argv[i], "--diff") == 0) {
            diff_mode = true;
        } else if (strcmp(argv[i], "--lang") == 0) {
            if (i + 1 >= argc) return drv_usage_error(usage_format);
            const char *lang = argv[++i];
            if (!fmt_reader_from_lang_name(lang, &force_lang)) {
                fprintf(stderr,
                        "tur format: unknown dialect '%s' (expected a "
                        "reader name or a base `tur dialects` lists)\n",
                        lang);
                return 2;
            }
            lang_set = true;
        } else if (argv[i][0] != '-') {
            if (fmt_input) return drv_usage_error(usage_format);
            fmt_input = argv[i];
        } else {
            return drv_usage_error(usage_format);
        }
    }
    if (check_only && diff_mode) return drv_usage_error(usage_format);
    return cmd_format(fmt_input, check_only, diff_mode, lang_set,
                      force_lang);
}

int cmd_parse_check_main(int argc, char **argv) {
    if (argc == 3 && (strcmp(argv[2], "--help") == 0 ||
                      strcmp(argv[2], "-h") == 0))
        return usage_parse_check();
    if (argc != 4) return drv_usage_error(usage_parse_check);
    return cmd_parse_check(argv[2], argv[3]);
}
