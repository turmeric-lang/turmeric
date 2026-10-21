/* wasm_lsp.c -- the LSP server, reachable from JavaScript.
 *
 * `tur lsp` already implements completion, hover, diagnostics, go-to-
 * definition, document symbols, and signature help, and after the
 * lsp-client-gaps work it does so at a quality a real editor shipped
 * against. None of that is re-implemented here. What a browser lacks is a
 * *transport*: lsp_server_run() is a blocking read(2)/write(2) loop and there
 * is no descriptor to block on.
 *
 * src/lsp/lsp_session.c supplies the loop-free entry point. This file supplies
 * the two things that are specific to running it inside the WASM bundle:
 *
 *   1. The JS bridge -- one JSON string in, one JSON string out.
 *   2. tur_collect_symbols, the analysis backend. The native one lives in
 *      the driver and runs the whole compile_to_c pipeline; the driver is a
 *      CLI and is not part of the WASM source set, so the browser gets its own front end
 *      over the same shared pieces (the stdlib autoload in
 *      compiler/stdlib_autoload.c, the binding walk in lsp/lsp_collect.c).
 *
 * The browser's front end stops after elaboration. Everything the LSP reports
 * -- names, types, spans, type errors -- exists by the end of that pass; the
 * passes after it (effect lowering, CPS, borrow check) exist to produce a
 * binary, which nobody in a browser is asking for. Skipping them makes an
 * analysis roughly a parse-and-typecheck, which is the difference between
 * completion feeling instant and completion feeling like a build.
 *
 * The cost is stated rather than hidden: a borrow-check or lifetime error will
 * not appear as a marker in the playground. Those are the diagnostics a user
 * hits after the code already type-checks, and the playground's Run button
 * still reports them.
 */

#include <stdlib.h>
#include <string.h>

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#define TUR_WASM_EXPORT EMSCRIPTEN_KEEPALIVE
#else
#define TUR_WASM_EXPORT
#endif

#include "arena.h"
#include "buf.h"
#include "diag.h"
#include "platform_fs.h"  /* setenv on Windows (this TU also builds natively
                           * for the tur_lsp_wasm_backend_unit test) */
#include "elab.h"
#include "expr.h"
#include "forms.h"
#include "reader.h"
#include "stdlib_autoload.h"
#include "lang_dialects.h"     /* r7rs-lang-plan R9: lang_traits */
#include "runtime/globals.h"   /* g_lang_prelude */
#include "symbols.h"

#include "lsp/lsp_collect.h"
#include "lsp/lsp_scope.h"
#include "lsp/lsp_session.h"
#include "lsp/lsp_sym.h"

/* Where the Emscripten link step mounts the host `stdlib/` tree
 * (`--embed-file <src>/stdlib@/stdlib`). Absolute rather than the eval path's
 * cwd-relative "stdlib": the LSP compares symbol file paths against this root
 * to decide what is stdlib surface, and a relative prefix would stop matching
 * the moment anything changed the working directory. */
#define WASM_LSP_STDLIB_DIR "/stdlib"

/* The mount point is the default, not a constant. lsp.c's stdlib cache and the
 * elaborator's import resolution both read TUR_STDLIB_DIR, so an embedder that
 * has already set it wins -- which is also what lets this backend be exercised
 * against the in-tree stdlib by a native test, rather than only ever running
 * where nothing can check it. */
static const char *lsp_stdlib_dir(void) {
    const char *env = getenv("TUR_STDLIB_DIR");
    return (env && *env) ? env : WASM_LSP_STDLIB_DIR;
}

static int initialized_ = 0;

static void wasm_lsp_init(void) {
    if (initialized_) return;
    initialized_ = 1;
    setenv("TUR_STDLIB_DIR", lsp_stdlib_dir(), 1);
    diag_init(false);
}

/* -------------------------------------------------------------------------
 * Analysis backend
 * --------------------------------------------------------------------- */

static int read_whole_file(const char *path, char **out, size_t *out_len) {
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return -1; }
    long size = ftell(f);
    if (size < 0) { fclose(f); return -1; }
    if (fseek(f, 0, SEEK_SET) != 0) { fclose(f); return -1; }
    char *buf = (char *)malloc((size_t)size + 1);
    if (!buf) { fclose(f); return -1; }
    size_t n = fread(buf, 1, (size_t)size, f);
    fclose(f);
    if (n != (size_t)size) { free(buf); return -1; }
    buf[size] = '\0';
    *out = buf;
    *out_len = (size_t)size;
    return 0;
}

/* Read, parse, and elaborate `source_path`, recording every global binding.
 *
 * Mirrors the front half of the driver's compile_to_c, including the `#lang`
 * sweep -- without it a `#lang turmeric/sweet` buffer (which is what the
 * playground opens with) would be handed to the s-expression reader and come
 * back as a wall of syntax errors that describe nothing the user did wrong.
 *
 * Returns 0 when elaboration succeeded. A non-zero return still leaves behind
 * whatever symbols were collected before the failure: a type error after a
 * clean parse has a complete binding table, and throwing it away would take
 * completion to zero at the moment it is most wanted.
 */
int tur_collect_symbols(const char *source_path, const char *logical_path,
                        LspSymbol *out, int cap, int *count_out) {
    /* The playground has no spice tree to walk up into -- every buffer is a
     * tab, and the module search path is the bundled stdlib. */
    (void)logical_path;
    wasm_lsp_init();
    lsp_collect_begin(out, cap, count_out);

    char  *src = NULL;
    size_t len = 0;
    if (read_whole_file(source_path, &src, &len) != 0) {
        lsp_collect_end();
        return 2;
    }

    const char  *src_adj = src;
    size_t       len_adj = len;
    const char  *bad     = NULL;
    size_t       bad_len = 0;
    /* r7rs-lang-plan R9: detect_lang_DIALECT, not the plain detect_lang --
     * the latter reports only the reader axis, so a `#lang saffron` or
     * `#lang r7rs` buffer was elaborated as Turmeric (no `any` default, no
     * Scheme lowering, no prelude) and reported errors that were not there.
     * The R0 READER_TURMERIC sweep named this site; this is the driver's
     * detect_and_adjust_lang, minus its exit paths. */
    LangDialect dialect = LANG_TURMERIC;
    ReaderType lang_type = detect_lang_dialect(src, len, &src_adj, &len_adj,
                                               &bad, &bad_len, &dialect);
    (void)bad; (void)bad_len;
    if (lang_type == READER_UNKNOWN || lang_type == (ReaderType)-1) {
        lang_type = READER_TURMERIC;
        dialect   = LANG_TURMERIC;
    }
    /* The language's prelude joins the stdlib autoload list for this buffer
     * only -- set every call, so a Scheme tab cannot license it for the next
     * Turmeric one. */
    g_lang_prelude = lang_traits(dialect)->prelude;
    /* A trailing token is a hard error in the CLI (TUR-E0330, via exit(1)).
     * Killing the WASM module over a typo in a `#lang` line is not an option
     * -- it is the browser tab. Fall back to the base reader and let the
     * compile report whatever it reports. */
    ReaderType ext_type = reader_type_from_extension(source_path);
    ReaderType reader_type = (ext_type != READER_TURMERIC) ? ext_type : lang_type;
    if (!reader_type_is_implemented(reader_type)) reader_type = READER_TURMERIC;

    diag_reset();

    SourceFile file = {0};
    file.path        = source_path;
    file.src         = src_adj;
    file.len         = len_adj;
    file.head_offset = (size_t)(src_adj - src);
    file.file_id     = 0;
    file.reader_type = reader_type;
    file.lang        = dialect;
    diag_register_file(&file);

    Arena arena;
    arena_init(&arena, 0);
    SymbolTable st;
    symtab_init(&st, &arena);

    uint32_t nforms = 0;
    Form **forms = read_all(&arena, &st, &file, &nforms);

    int rc = 0;
    if (!forms || diag_had_error()) {
        rc = 1;
    } else {
        uint8_t  file_id = 1;
        uint32_t stdlib_prefix =
            tur_stdlib_prepend_forms(&arena, &st, lsp_stdlib_dir(),
                                     source_path, /*no_auto_stdlib=*/false,
                                     &forms, &nforms, &file_id);

        Expr *prog = elaborate_program(&arena, &st, forms, nforms,
                                       stdlib_prefix,
                                       /*module_base_dir=*/".",
                                       /*separate_compilation=*/false,
                                       /*sandboxed=*/false,
                                       /*out_tc_env=*/NULL,
                                       /*include_dirs=*/NULL,
                                       /*n_include_dirs=*/0,
                                       /*out_n_file_scope_defs=*/NULL,
                                       /*user_macros=*/NULL);
        /* Collect before judging: elaboration reports type errors on a tree it
         * still fully built, and that tree is exactly what hover and completion
         * want to describe. */
        if (prog) lsp_collect_program(prog);
        if (prog && lsp_scope_active()) lsp_scope_program(prog);
        if (!prog || diag_had_error()) rc = 1;
    }

    symtab_free(&st);
    arena_free(&arena);
    free(src);
    lsp_collect_end();
    return rc;
}

/* lsp_sym.h declares this for the MCP server's checks. Nothing the session
 * reaches calls it; the definition exists so every reference resolves. */
int tur_check_only(const char *path) {
    (void)path;
    return 1;
}

/* -------------------------------------------------------------------------
 * JS bridge
 * --------------------------------------------------------------------- */

/* Feed one JSON-RPC message to the server.
 *
 * Returns a malloc'd JSON array of every message the server produced -- the
 * response, plus any publishDiagnostics notifications the analysis emitted
 * along the way. Caller frees with turi_wasm_free_string / _free.
 *
 * An array rather than Content-Length framed bytes: the JS side is going to
 * JSON.parse this immediately, so framing it would only be work spent on
 * something the receiver strips back off.
 *
 * Returns "[]" (never NULL) when the client sends `exit`; the session is torn
 * down but the module stays alive, because the process here is a browser tab
 * and ending it is not a language server's call. */
TUR_WASM_EXPORT
char *turi_wasm_lsp_request(const char *json) {
    if (!json) return NULL;
    wasm_lsp_init();

    Buf out;
    lsp_session_handle(json, strlen(json), &out);

    /* Hand back a plain malloc'd C string: `out` is a Buf, and JS has no way
     * to free one. */
    char *ret = (char *)malloc(out.len + 1);
    if (ret) {
        memcpy(ret, out.data ? out.data : "", out.len);
        ret[out.len] = '\0';
    }
    buf_free(&out);
    return ret;
}

/* Analyze every document with pending edits and return the resulting
 * publishDiagnostics notifications, same JSON-array shape.
 *
 * The stdio server runs this off a poll() timeout on its input descriptor.
 * The browser has no descriptor and its own event timing, so the adapter's
 * trailing debounce calls this when typing goes quiet. */
TUR_WASM_EXPORT
char *turi_wasm_lsp_flush(void) {
    wasm_lsp_init();

    Buf out;
    lsp_session_flush(&out);

    char *ret = (char *)malloc(out.len + 1);
    if (ret) {
        memcpy(ret, out.data ? out.data : "", out.len);
        ret[out.len] = '\0';
    }
    buf_free(&out);
    return ret;
}

/* Drop every open document and the cached stdlib surface. The playground calls
 * this when a project is loaded wholesale, where re-opening each tab into a
 * session that still holds the previous project's files would leave
 * workspace/symbol answering with names from a workspace the user closed. */
TUR_WASM_EXPORT
void turi_wasm_lsp_reset(void) {
    lsp_session_reset();
}

/* -------------------------------------------------------------------------
 * Stdlib source reader (try-turmeric-navigation-and-minimap-plan, M4)
 * --------------------------------------------------------------------- */

/* True when `path` names a file inside the stdlib mount and nothing else.
 *
 * The alternative to this export was adding `FS` to EXPORTED_RUNTIME_METHODS,
 * which is one CMake word and hands the page a general filesystem. This is the
 * same amount of work and keeps the exported surface a list of named
 * operations, so the capability the page gains is "show me where `map` is
 * defined" rather than "read any path in the module".
 *
 * Three things are checked, and the third is the one that matters:
 *
 *   - the path starts with the stdlib root, and the next character is a
 *     separator (so `/stdlibx/secrets` is not inside `/stdlib`);
 *   - it ends in `.tur` -- the only thing a definition can land in;
 *   - it contains no `..` segment, so a prefix match cannot be walked back
 *     out of the tree it was supposed to confine the caller to.
 */
static int is_stdlib_source_path(const char *path) {
    if (!path || !*path) return 0;

    const char *root = lsp_stdlib_dir();
    size_t rlen = strlen(root);
    if (rlen == 0) return 0;
    /* A root written with a trailing slash still names the same directory. */
    while (rlen > 1 && root[rlen - 1] == '/') rlen--;
    if (strncmp(path, root, rlen) != 0) return 0;
    if (path[rlen] != '/') return 0;
    if (path[rlen + 1] == '\0') return 0;

    size_t plen = strlen(path);
    if (plen < 4 || strcmp(path + plen - 4, ".tur") != 0) return 0;

    for (const char *p = path; *p; p++) {
        if (p[0] != '.' || p[1] != '.') continue;
        int at_start = (p == path) || (p[-1] == '/');
        int at_end   = (p[2] == '\0') || (p[2] == '/');
        if (at_start && at_end) return 0;
    }
    return 1;
}

/* Read one stdlib source file out of the module's virtual filesystem.
 *
 * Returns a malloc'd NUL-terminated string the caller frees with _free, or
 * NULL for a path outside the stdlib, a file that does not exist, or a read
 * that fails. NULL is the only failure signal on purpose: the page's answer to
 * all three is the same -- do not open a tab -- and distinguishing them would
 * only tell a caller which paths exist.
 *
 * Shape matches every other export here (one string in, one malloc'd string
 * out), so the worker's callStringToString helper drives it with no new
 * marshaling code. */
TUR_WASM_EXPORT
char *turi_wasm_read_file(const char *path) {
    if (!path) return NULL;
    wasm_lsp_init();   /* fixes TUR_STDLIB_DIR before is_stdlib_source_path reads it */
    if (!is_stdlib_source_path(path)) return NULL;

    char  *text = NULL;
    size_t len  = 0;
    if (read_whole_file(path, &text, &len) != 0) return NULL;
    return text;   /* read_whole_file NUL-terminates; caller frees */
}
