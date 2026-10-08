/* session_test.c -- unit tests for the transport-free LSP session
 * (src/lsp/lsp_session.c + the dispatch lifted out of src/lsp/lsp.c).
 *
 * The point of the transport split is that the server's behaviour no longer
 * depends on there being a pipe. This harness is the proof: it drives scripted
 * JSON-RPC messages straight through lsp_session_handle() and asserts on the
 * JSON array that comes back. No file descriptors, no subprocess, no editor --
 * which also makes it the cheapest place to pin the browser path's behaviour,
 * since the browser path IS this function.
 *
 * tur_collect_symbols is stubbed rather than real. Running the actual compiler
 * would make every assertion here a hostage to whatever the stdlib currently
 * exports; a fixed two-symbol index (one document-local, one "stdlib") makes
 * the ordering, filtering, and path-remapping rules testable by inspection.
 * tests/lsp/mcp_lsp_test.py covers the server against the real compiler.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "buf.h"
#include "diag.h"
#include "lsp_session.h"
#include "lsp_scope.h"
#include "lsp_sym.h"
#include "platform_fs.h"  /* setenv on Windows */

static int passed = 0;
static int failed = 0;

#define CHECK(cond, msg) \
    do { \
        if (cond) { passed++; } \
        else { fprintf(stderr, "FAIL: %s (line %d)\n", (msg), __LINE__); failed++; } \
    } while (0)

/* -------------------------------------------------------------------------
 * Stub analysis backend
 *
 * Answers as if the buffer defined `local-fn`, and as if the stdlib defined
 * `cons`. The caller hands us the temp path it wrote the document to, and the
 * real collector reports that path on the document's own symbols -- so the
 * stub does too, which is what run_doc_analysis's remap-to-doc-path step keys
 * off.
 * --------------------------------------------------------------------- */

#define STUB_STDLIB_DIR "/stub-stdlib"

/* Set by a test to make the next analysis report a compile error. */
static int stub_emit_error = 0;
/* Set by a test to make the next analysis yield nothing (the "buffer does not
 * parse" state, which is the normal state while typing). */
static int stub_yield_nothing = 0;

static void stub_fill_scope(void);

static void stub_sym(LspSymbol *s, const char *name, const char *type,
                     const char *file, int line, int c0, int c1,
                     LspSymKind kind) {
    memset(s, 0, sizeof(*s));
    snprintf(s->name, sizeof(s->name), "%s", name);
    snprintf(s->type_str, sizeof(s->type_str), "%s", type);
    snprintf(s->file_path, sizeof(s->file_path), "%s", file);
    s->line = line;
    s->col_start = c0;
    s->col_end = c1;
    s->kind = kind;
}

int tur_collect_symbols(const char *source_path, const char *logical_path,
                        LspSymbol *out, int cap, int *count_out) {
    (void)logical_path;   /* the stub has no spice tree to resolve against */
    *count_out = 0;

    /* The stdlib prime analyses an empty scratch file, which always compiles
     * -- so "this buffer does not parse" must not make the prime fail too, or
     * the fallback would be untestable. Recognise it by name, the way the
     * server names it. */
    int is_prime = strstr(source_path, "tur_lsp_std_") != NULL;
    if (is_prime) {
        if (cap >= 1)
            stub_sym(&out[(*count_out)++], "cons", "(fn [int int] : int)",
                     STUB_STDLIB_DIR "/list.tur", 12, 7, 11,
                     LSP_KIND_FUNCTION);
        return 0;
    }

    if (stub_yield_nothing) return 1;

    /* Locals ride the same call the globals do, because in the real collector
     * they ride the same walk (main.c's one elaboration hook). */
    stub_fill_scope();

    if (cap >= 1)
        stub_sym(&out[(*count_out)++], "local-fn", "(fn [int int] : int)",
                 source_path, 3, 7, 15, LSP_KIND_FUNCTION);
    if (cap >= 2)
        stub_sym(&out[(*count_out)++], "cons", "(fn [int int] : int)",
                 STUB_STDLIB_DIR "/list.tur", 12, 7, 11, LSP_KIND_FUNCTION);
    /* A record type, so the kind mapping has something to get wrong. Its
     * type_str is deliberately not a function type *and* not a struct-shaped
     * one: before the kind field, this and a plain `def` were the same entry,
     * because the only question anyone could ask was "does the rendered type
     * start with (fn". */
    if (cap >= 3)
        stub_sym(&out[(*count_out)++], "Point", "Point",
                 source_path, 5, 12, 17, LSP_KIND_STRUCT);

    if (stub_emit_error) {
        /* Diagnostics are reported against the temp file the server wrote, and
         * run_doc_analysis remaps that path back to the document's. Register
         * the file so the span has something to resolve to, exactly as a real
         * compile would. */
        static char path_copy[512];
        snprintf(path_copy, sizeof(path_copy), "%s", source_path);
        SourceFile f = {0};
        f.path = path_copy;
        f.src = "";
        f.len = 0;
        f.file_id = 0;
        diag_register_file(&f);
        Span span = {0};
        span.file_id = 0;
        span.line = 2;
        span.col_start = 3;
        span.col_end = 8;
        diag_emit(DIAG_ERROR, span, "stub: undefined name `nope`");
        return 1;
    }
    return 0;
}

/* lsp.c declares this for the `tur check` path; nothing in the session
 * reaches it, but the declaration is enough for a linker to want a body. */
int tur_check_only(const char *path) {
    (void)path;
    return 0;
}

/* -------------------------------------------------------------------------
 * Harness helpers
 * --------------------------------------------------------------------- */

/* Send one message and return the reply array. Caller frees with buf_free. */
static Buf send_msg(const char *json) {
    Buf out;
    lsp_session_handle(json, strlen(json), &out);
    return out;
}

static int contains(const Buf *b, const char *needle) {
    return b->data && strstr(b->data, needle) != NULL;
}

/* Byte offset of `needle` in the reply, or -1. Used to assert ordering
 * between two items in one array. */
static long index_of(const Buf *b, const char *needle) {
    if (!b->data) return -1;
    const char *p = strstr(b->data, needle);
    return p ? (long)(p - b->data) : -1;
}

static void session_open(const char *uri, const char *text) {
    Buf b;
    buf_init(&b);
    buf_printf(&b, "{\"jsonrpc\":\"2.0\",\"method\":\"textDocument/didOpen\","
                   "\"params\":{\"textDocument\":{\"uri\":\"%s\",\"text\":\"%s\"}}}",
               uri, text);
    buf_putc(&b, '\0');
    Buf out = send_msg(b.data);
    buf_free(&out);
    buf_free(&b);
}

static void fresh_session(void) {
    stub_emit_error = 0;
    stub_yield_nothing = 0;
    lsp_session_reset();
    Buf out = send_msg("{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\","
                       "\"params\":{}}");
    buf_free(&out);
}

/* -------------------------------------------------------------------------
 * initialize
 * --------------------------------------------------------------------- */

static void test_initialize(void) {
    lsp_session_reset();
    Buf out = send_msg("{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\","
                       "\"params\":{}}");

    CHECK(out.data && out.data[0] == '[', "initialize reply is a JSON array");
    CHECK(contains(&out, "\"id\":1"), "initialize echoes the request id");
    CHECK(contains(&out, "\"positionEncoding\":\"utf-8\""),
          "initialize declares utf-8 position encoding");
    CHECK(contains(&out, "\"completionProvider\""),
          "initialize advertises completion");
    CHECK(contains(&out, "\"signatureHelpProvider\""),
          "initialize advertises signature help");
    /* Exactly one message: nothing is open, so no diagnostics ride along. */
    CHECK(!contains(&out, "publishDiagnostics"),
          "initialize alone publishes no diagnostics");
    buf_free(&out);
}

/* -------------------------------------------------------------------------
 * didOpen publishes diagnostics without anyone asking
 *
 * The stdio server defers analysis until poll() says the client went quiet.
 * A caller with no descriptor to poll gets the flush at the end of the
 * message instead -- without it, an edited buffer's diagnostics would not
 * appear until some unrelated request happened to need symbols.
 * --------------------------------------------------------------------- */

static void test_did_open_publishes_diagnostics(void) {
    fresh_session();
    Buf b;
    buf_init(&b);
    buf_puts(&b, "{\"jsonrpc\":\"2.0\",\"method\":\"textDocument/didOpen\","
                 "\"params\":{\"textDocument\":"
                 "{\"uri\":\"file:///project/main.tur\",\"text\":\"(defn f [] 1)\"}}}");
    buf_putc(&b, '\0');
    Buf out = send_msg(b.data);

    CHECK(contains(&out, "textDocument/publishDiagnostics"),
          "didOpen flushes analysis and publishes diagnostics");
    CHECK(contains(&out, "file:///project/main.tur"),
          "publishDiagnostics names the document uri");
    CHECK(contains(&out, "\"diagnostics\":[]"),
          "a clean stub compile publishes an empty diagnostic list");
    buf_free(&out);
    buf_free(&b);
}

static void test_diagnostics_reach_the_client(void) {
    fresh_session();
    stub_emit_error = 1;
    Buf b;
    buf_init(&b);
    buf_puts(&b, "{\"jsonrpc\":\"2.0\",\"method\":\"textDocument/didOpen\","
                 "\"params\":{\"textDocument\":"
                 "{\"uri\":\"file:///project/bad.tur\",\"text\":\"(nope)\"}}}");
    buf_putc(&b, '\0');
    Buf out = send_msg(b.data);

    CHECK(contains(&out, "stub: undefined name"),
          "a compile error reaches publishDiagnostics");
    CHECK(contains(&out, "\"severity\":1"),
          "an error maps to LSP severity 1");
    CHECK(contains(&out, "file:///project/bad.tur"),
          "the diagnostic is attributed to the document, not the temp file");
    CHECK(!contains(&out, "tur_lsp_"),
          "the server's temp path never leaks to the client");
    buf_free(&out);
    buf_free(&b);
    stub_emit_error = 0;
}

/* -------------------------------------------------------------------------
 * completion
 * --------------------------------------------------------------------- */

static void test_completion_orders_local_first(void) {
    fresh_session();
    session_open("file:///project/main.tur", "(defn local-fn [a b] : int a)");

    Buf out = send_msg(
        "{\"jsonrpc\":\"2.0\",\"id\":7,\"method\":\"textDocument/completion\","
        "\"params\":{\"textDocument\":{\"uri\":\"file:///project/main.tur\"},"
        "\"position\":{\"line\":0,\"character\":0}}}");

    CHECK(contains(&out, "\"isIncomplete\":false"),
          "completion answers with a CompletionList");
    CHECK(contains(&out, "\"label\":\"local-fn\""),
          "completion offers the document's own definition");
    CHECK(contains(&out, "\"label\":\"cons\""),
          "completion offers stdlib symbols too");
    long local = index_of(&out, "\"label\":\"local-fn\"");
    long std_  = index_of(&out, "\"label\":\"cons\"");
    CHECK(local >= 0 && std_ >= 0 && local < std_,
          "document-local symbols come before stdlib ones");
    buf_free(&out);
}

static void test_completion_filters_by_prefix(void) {
    fresh_session();
    session_open("file:///project/main.tur", "(co");

    /* Cursor at the end of "(co" -- the prefix is what precedes it. */
    Buf out = send_msg(
        "{\"jsonrpc\":\"2.0\",\"id\":8,\"method\":\"textDocument/completion\","
        "\"params\":{\"textDocument\":{\"uri\":\"file:///project/main.tur\"},"
        "\"position\":{\"line\":0,\"character\":3}}}");

    CHECK(contains(&out, "\"label\":\"cons\""),
          "prefix `co` keeps cons");
    CHECK(!contains(&out, "\"label\":\"local-fn\""),
          "prefix `co` drops local-fn");
    buf_free(&out);
}

/* -------------------------------------------------------------------------
 * hover / definition / documentSymbol / signatureHelp
 * --------------------------------------------------------------------- */

static void test_hover(void) {
    fresh_session();
    session_open("file:///project/main.tur", "(cons 1 2)");

    Buf out = send_msg(
        "{\"jsonrpc\":\"2.0\",\"id\":9,\"method\":\"textDocument/hover\","
        "\"params\":{\"textDocument\":{\"uri\":\"file:///project/main.tur\"},"
        "\"position\":{\"line\":0,\"character\":2}}}");

    CHECK(contains(&out, "\"kind\":\"markdown\""),
          "hover honours the 3.17 markdown default");
    CHECK(contains(&out, "(cons : (fn [int int] : int))"),
          "hover renders the symbol's type");
    buf_free(&out);
}

static void test_definition_reports_document_path(void) {
    fresh_session();
    session_open("file:///project/main.tur", "(local-fn 1 2)");

    Buf out = send_msg(
        "{\"jsonrpc\":\"2.0\",\"id\":10,\"method\":\"textDocument/definition\","
        "\"params\":{\"textDocument\":{\"uri\":\"file:///project/main.tur\"},"
        "\"position\":{\"line\":0,\"character\":2}}}");

    /* The stub reports the temp path; run_doc_analysis rewrites it to the
     * document's own path before the client ever sees it. A client that
     * followed the temp path would open a file that has already been
     * unlinked. */
    CHECK(contains(&out, "file:///project/main.tur"),
          "definition points at the document, not the analysis temp file");
    CHECK(contains(&out, "\"line\":2"),
          "definition converts the 1-based span to a 0-based LSP position");
    buf_free(&out);
}

static void test_document_symbol_is_file_scoped(void) {
    fresh_session();
    session_open("file:///project/main.tur", "(defn local-fn [a b] : int a)");

    Buf out = send_msg(
        "{\"jsonrpc\":\"2.0\",\"id\":11,\"method\":\"textDocument/documentSymbol\","
        "\"params\":{\"textDocument\":{\"uri\":\"file:///project/main.tur\"}}}");

    CHECK(contains(&out, "\"name\":\"local-fn\""),
          "documentSymbol lists the document's definitions");
    CHECK(!contains(&out, "\"name\":\"cons\""),
          "documentSymbol excludes symbols defined elsewhere");
    buf_free(&out);
}

/* -------------------------------------------------------------------------
 * Symbol kinds, documentHighlight, builtin fallback
 * (try-turmeric-navigation-and-minimap-plan, M5)
 * --------------------------------------------------------------------- */

static void test_document_symbol_reports_real_kinds(void) {
    fresh_session();
    session_open("file:///project/main.tur", "(defn local-fn [a b] : int a)");

    Buf out = send_msg(
        "{\"jsonrpc\":\"2.0\",\"id\":30,\"method\":\"textDocument/documentSymbol\","
        "\"params\":{\"textDocument\":{\"uri\":\"file:///project/main.tur\"}}}");

    /* 12 = Function, 23 = Struct. The old mapping could only answer 12 or 13,
     * so a defstruct and a def were indistinguishable in an outline -- which
     * is most of what makes an outline scannable. */
    CHECK(contains(&out, "\"name\":\"local-fn\",\"kind\":12"),
          "a function reports SymbolKind.Function");
    CHECK(contains(&out, "\"name\":\"Point\",\"kind\":23"),
          "a record type reports SymbolKind.Struct, not Variable");
    buf_free(&out);
}

static void test_completion_kind_follows_the_symbol_kind(void) {
    fresh_session();
    session_open("file:///project/main.tur", "(P");

    Buf out = send_msg(
        "{\"jsonrpc\":\"2.0\",\"id\":31,\"method\":\"textDocument/completion\","
        "\"params\":{\"textDocument\":{\"uri\":\"file:///project/main.tur\"},"
        "\"position\":{\"line\":0,\"character\":2}}}");

    /* CompletionItemKind is a *different* enum from SymbolKind for the same
     * distinction: 22 = Struct here, not 23. */
    CHECK(contains(&out, "\"label\":\"Point\",\"kind\":22"),
          "a type completes with a type icon, not a variable one");
    buf_free(&out);
}

static void test_document_highlight_skips_comments_and_strings(void) {
    fresh_session();
    /* Four textual occurrences of `local-fn`, two of them real. The other two
     * -- one in a comment, one inside a string literal -- are the exact cases
     * a regular expression over the source cannot tell apart from a use.
     *
     * The definition sits at 0-based line 2, column 6, which is where the stub
     * collector claims it is (line 3, col_start 7, both 1-based). That
     * agreement is what the Write/Text distinction is keyed on. */
    session_open("file:///project/main.tur",
                 "(local-fn 1 2) ; local-fn again\\n"
                 "(def s \\\"local-fn\\\")\\n"
                 "(defn local-fn [a b] : int a)");

    Buf out = send_msg(
        "{\"jsonrpc\":\"2.0\",\"id\":32,\"method\":\"textDocument/documentHighlight\","
        "\"params\":{\"textDocument\":{\"uri\":\"file:///project/main.tur\"},"
        "\"position\":{\"line\":0,\"character\":2}}}");

    CHECK(contains(&out, "\"range\""), "documentHighlight answers ranges");
    /* Exactly two ranges: count the range objects. */
    int n = 0;
    for (const char *p = out.data; (p = strstr(p, "\"range\":")) != NULL; p++) n++;
    CHECK(n == 2, "the comment and the string literal are not occurrences");
    CHECK(contains(&out,
        "{\"start\":{\"line\":0,\"character\":1},"
         "\"end\":{\"line\":0,\"character\":9}},\"kind\":1}"),
          "a use is reported as text");
    /* The definition is Write, not Text -- "which of these is the definition"
     * is the question a reader scanning a column of marks actually has. */
    CHECK(contains(&out,
        "{\"start\":{\"line\":2,\"character\":6},"
         "\"end\":{\"line\":2,\"character\":14}},\"kind\":3}"),
          "the defining occurrence is reported as a write");
    buf_free(&out);
}

static void test_document_highlight_off_a_word_is_null(void) {
    fresh_session();
    session_open("file:///project/main.tur", "(local-fn 1 2)   ");

    /* Character 15 is a space with a space after it. Not 9, the space before
     * `1`: lsp_word_at_pos steps one character right when the cursor is not on
     * an identifier, and a digit is an identifier character -- so that
     * position resolves to the word "1", which is a different test. */
    Buf out = send_msg(
        "{\"jsonrpc\":\"2.0\",\"id\":33,\"method\":\"textDocument/documentHighlight\","
        "\"params\":{\"textDocument\":{\"uri\":\"file:///project/main.tur\"},"
        "\"position\":{\"line\":0,\"character\":15}}}");

    /* `null` rather than `[]` so a client keeps what it was showing instead of
     * flickering as the caret crosses whitespace. */
    CHECK(contains(&out, "\"result\":null"),
          "a position that is not on a word answers null");
    buf_free(&out);
}

static void test_hover_falls_back_to_the_builtin_table(void) {
    fresh_session();
    session_open("file:///project/main.tur", "(println 1)");

    Buf out = send_msg(
        "{\"jsonrpc\":\"2.0\",\"id\":34,\"method\":\"textDocument/hover\","
        "\"params\":{\"textDocument\":{\"uri\":\"file:///project/main.tur\"},"
        "\"position\":{\"line\":0,\"character\":3}}}");

    /* The index is built from Bindings and a compiler builtin has none, so
     * this used to be `{"contents":""}` -- silent on the name a first-time
     * visitor types first. */
    CHECK(contains(&out, "(println : (fn [int] #fx{IO} : nil))"),
          "hover on a builtin renders its signature, effect row included");
    CHECK(contains(&out, "built-in operator"),
          "hover says the answer came from the operator table, not analysis");
    buf_free(&out);
}

static void test_signature_help_falls_back_to_the_builtin_table(void) {
    fresh_session();
    session_open("file:///project/main.tur", "(println ");

    Buf out = send_msg(
        "{\"jsonrpc\":\"2.0\",\"id\":35,\"method\":\"textDocument/signatureHelp\","
        "\"params\":{\"textDocument\":{\"uri\":\"file:///project/main.tur\"},"
        "\"position\":{\"line\":0,\"character\":9}}}");

    CHECK(contains(&out, "\"signatures\""),
          "signatureHelp answers for a builtin callee");
    CHECK(contains(&out, "(println : (fn [int] #fx{IO} : nil))"),
          "the builtin's signature reaches the label");
    buf_free(&out);
}

static void test_definition_on_a_builtin_is_still_null(void) {
    fresh_session();
    session_open("file:///project/main.tur", "(println 1)");

    Buf out = send_msg(
        "{\"jsonrpc\":\"2.0\",\"id\":36,\"method\":\"textDocument/definition\","
        "\"params\":{\"textDocument\":{\"uri\":\"file:///project/main.tur\"},"
        "\"position\":{\"line\":0,\"character\":3}}}");

    /* A builtin has no source location. Inventing one -- pointing at
     * builtins.c, or at the call site -- would be worse than answering
     * nothing, because the editor would actually go there. */
    CHECK(contains(&out, "\"result\":null"),
          "a builtin has nowhere to go to");
    buf_free(&out);
}

static void test_signature_help(void) {
    fresh_session();
    session_open("file:///project/main.tur", "(cons 1 ");

    Buf out = send_msg(
        "{\"jsonrpc\":\"2.0\",\"id\":12,\"method\":\"textDocument/signatureHelp\","
        "\"params\":{\"textDocument\":{\"uri\":\"file:///project/main.tur\"},"
        "\"position\":{\"line\":0,\"character\":8}}}");

    CHECK(contains(&out, "\"signatures\""), "signatureHelp answers a signature");
    CHECK(contains(&out, "(cons : (fn [int int] : int))"),
          "the signature label carries the rendered type");
    CHECK(contains(&out, "\"activeParameter\":1"),
          "the cursor after the first argument selects parameter 1");
    buf_free(&out);
}

/* -------------------------------------------------------------------------
 * didChange re-analyses; the last-good index survives a broken buffer
 * --------------------------------------------------------------------- */

static void test_did_change_republishes(void) {
    fresh_session();
    session_open("file:///project/main.tur", "(defn local-fn [a b] : int a)");

    stub_emit_error = 1;
    Buf out = send_msg(
        "{\"jsonrpc\":\"2.0\",\"method\":\"textDocument/didChange\","
        "\"params\":{\"textDocument\":{\"uri\":\"file:///project/main.tur\"},"
        "\"contentChanges\":[{\"text\":\"(nope\"}]}}");

    CHECK(contains(&out, "textDocument/publishDiagnostics"),
          "didChange republishes diagnostics without a separate request");
    CHECK(contains(&out, "stub: undefined name"),
          "the new diagnostics describe the new text");
    buf_free(&out);
    stub_emit_error = 0;
}

static void test_stale_index_survives_unparseable_text(void) {
    fresh_session();
    session_open("file:///project/main.tur", "(defn local-fn [a b] : int a)");

    /* Mid-edit the buffer does not parse and yields no symbols at all. The
     * index from the previous revision has to be retained -- dropping it takes
     * completion to zero answers at precisely the moment it is wanted. */
    stub_yield_nothing = 1;
    Buf changed = send_msg(
        "{\"jsonrpc\":\"2.0\",\"method\":\"textDocument/didChange\","
        "\"params\":{\"textDocument\":{\"uri\":\"file:///project/main.tur\"},"
        "\"contentChanges\":[{\"text\":\"(local-\"}]}}");
    buf_free(&changed);

    Buf out = send_msg(
        "{\"jsonrpc\":\"2.0\",\"id\":13,\"method\":\"textDocument/completion\","
        "\"params\":{\"textDocument\":{\"uri\":\"file:///project/main.tur\"},"
        "\"position\":{\"line\":0,\"character\":7}}}");

    CHECK(contains(&out, "\"label\":\"local-fn\""),
          "an unparseable buffer keeps serving the last good index");
    buf_free(&out);
    stub_yield_nothing = 0;
}

/* -------------------------------------------------------------------------
 * didChange batching: Full sync means the last change wins
 * --------------------------------------------------------------------- */

static void test_did_change_takes_the_last_element(void) {
    fresh_session();
    session_open("file:///project/main.tur", "(defn local-fn [a b] : int a)");

    Buf changed = send_msg(
        "{\"jsonrpc\":\"2.0\",\"method\":\"textDocument/didChange\","
        "\"params\":{\"textDocument\":{\"uri\":\"file:///project/main.tur\"},"
        "\"contentChanges\":[{\"text\":\"(co\"},{\"text\":\"(local-\"}]}}");
    buf_free(&changed);

    /* If element 0 had won, the prefix at column 7 would be past the end of
     * "(co" and the filter would behave differently. */
    Buf out = send_msg(
        "{\"jsonrpc\":\"2.0\",\"id\":14,\"method\":\"textDocument/completion\","
        "\"params\":{\"textDocument\":{\"uri\":\"file:///project/main.tur\"},"
        "\"position\":{\"line\":0,\"character\":7}}}");

    CHECK(contains(&out, "\"label\":\"local-fn\""),
          "the last contentChanges element is the one adopted");
    CHECK(!contains(&out, "\"label\":\"cons\""),
          "prefix `local-` drops cons");
    buf_free(&out);
}

/* -------------------------------------------------------------------------
 * Protocol edges
 * --------------------------------------------------------------------- */

static void test_unknown_method_is_answered(void) {
    fresh_session();
    /* Something the server does not implement and, per the follow-through
     * plan's out-of-scope list, is not about to. `textDocument/rename` used to
     * stand here and stopped being unknown the day rename shipped. */
    Buf out = send_msg("{\"jsonrpc\":\"2.0\",\"id\":42,\"method\":\"textDocument/inlayHint\","
                       "\"params\":{}}");
    CHECK(contains(&out, "-32601"), "an unknown request gets Method not found");
    CHECK(contains(&out, "\"id\":42"), "the error echoes the request id");
    buf_free(&out);
}

static void test_unknown_notification_is_silent(void) {
    fresh_session();
    /* No id means a notification, and a notification must never be answered. */
    Buf out = send_msg("{\"jsonrpc\":\"2.0\",\"method\":\"$/setTrace\","
                       "\"params\":{\"value\":\"off\"}}");
    CHECK(out.data && strcmp(out.data, "[]") == 0,
          "an unknown notification produces no reply at all");
    buf_free(&out);
}

static void test_cancel_on_the_session_path_is_a_noop(void) {
    fresh_session();
    session_open("file:///project/main.tur", "(cons 1 2)");

    Buf cancel = send_msg("{\"jsonrpc\":\"2.0\",\"method\":\"$/cancelRequest\","
                          "\"params\":{\"id\":15}}");
    CHECK(cancel.data && strcmp(cancel.data, "[]") == 0,
          "$/cancelRequest itself is never answered");
    buf_free(&cancel);

    /* There is no descriptor to drain, so a cancel that arrives before the
     * request it names cannot strand that id: request 15 must still be
     * answered normally rather than with -32800. */
    Buf out = send_msg(
        "{\"jsonrpc\":\"2.0\",\"id\":15,\"method\":\"textDocument/hover\","
        "\"params\":{\"textDocument\":{\"uri\":\"file:///project/main.tur\"},"
        "\"position\":{\"line\":0,\"character\":2}}}");
    CHECK(!contains(&out, "-32800"),
          "a cancel with nothing to cancel does not poison a later id");
    CHECK(contains(&out, "\"contents\""), "the request is answered normally");
    buf_free(&out);
}

static void test_formatting_skips_the_analysis_flush(void) {
    fresh_session();
    session_open("file:///project/main.tur", "(defn f [] 1)");

    Buf changed = send_msg(
        "{\"jsonrpc\":\"2.0\",\"method\":\"textDocument/didChange\","
        "\"params\":{\"textDocument\":{\"uri\":\"file:///project/main.tur\"},"
        "\"contentChanges\":[{\"text\":\"(defn  f  []  1)\"}]}}");
    buf_free(&changed);

    Buf out = send_msg(
        "{\"jsonrpc\":\"2.0\",\"id\":16,\"method\":\"textDocument/formatting\","
        "\"params\":{\"textDocument\":{\"uri\":\"file:///project/main.tur\"},"
        "\"options\":{\"tabSize\":2,\"insertSpaces\":true}}}");

    CHECK(contains(&out, "newText"), "formatting returns a full-document edit");
    CHECK(contains(&out, "(defn f [] 1)"),
          "formatting reads doc->text, so it sees the edit without an analysis");
    buf_free(&out);
}

static void test_formatting_with_no_params_errors(void) {
    fresh_session();
    Buf out = send_msg("{\"jsonrpc\":\"2.0\",\"id\":17,"
                       "\"method\":\"textDocument/formatting\"}");
    CHECK(contains(&out, "-32602"), "a request without params gets Invalid params");
    buf_free(&out);
}

/* -------------------------------------------------------------------------
 * Lifecycle: exit tears the session down, it does not kill the process
 * --------------------------------------------------------------------- */

static void test_exit_ends_the_session_without_exiting(void) {
    fresh_session();
    session_open("file:///project/main.tur", "(cons 1 2)");

    Buf out;
    bool alive = lsp_session_handle("{\"jsonrpc\":\"2.0\",\"method\":\"exit\"}",
                                    strlen("{\"jsonrpc\":\"2.0\",\"method\":\"exit\"}"),
                                    &out);
    CHECK(!alive, "exit reports the session is over");
    CHECK(out.data && strcmp(out.data, "[]") == 0, "exit emits no message");
    buf_free(&out);

    /* Reaching this line at all is most of the assertion: the stdio loop
     * calls _exit() here, and a browser tab is not something to terminate on
     * a client's say-so. The session must also be genuinely fresh afterwards. */
    Buf reopened = send_msg(
        "{\"jsonrpc\":\"2.0\",\"id\":18,\"method\":\"textDocument/documentSymbol\","
        "\"params\":{\"textDocument\":{\"uri\":\"file:///project/main.tur\"}}}");
    CHECK(contains(&reopened, "\"result\":[]"),
          "documents opened before exit are gone afterwards");
    buf_free(&reopened);
}

/* The stdlib fallback has to survive a reset.
 *
 * The prime used to latch on a function-local static, which on stdio is
 * indistinguishable from correct -- one session is one process there. In a
 * browser the module outlives the session: the latch stayed set while the
 * cache it guarded was freed, so from the second session onward a buffer
 * opened with a syntax error already in it had completion dead for the life
 * of the page, which is exactly the case the fallback exists for. */
static void test_stdlib_fallback_survives_a_reset(void) {
    for (int round = 0; round < 2; round++) {
        fresh_session();
        stub_yield_nothing = 1;
        session_open("file:///project/never-parsed.tur", "(cons");

        Buf out = send_msg(
            "{\"jsonrpc\":\"2.0\",\"id\":21,\"method\":\"textDocument/completion\","
            "\"params\":{\"textDocument\":{\"uri\":\"file:///project/never-parsed.tur\"},"
            "\"position\":{\"line\":0,\"character\":5}}}");

        CHECK(contains(&out, "\"label\":\"cons\""),
              round == 0 ? "a never-parsed buffer completes from the stdlib"
                         : "and still does after the session was reset");
        buf_free(&out);
        stub_yield_nothing = 0;
    }
}

static void test_flush_with_nothing_dirty_is_empty(void) {
    fresh_session();
    session_open("file:///project/main.tur", "(cons 1 2)");

    /* didOpen already flushed, so an explicit flush has nothing to do. A
     * client polling its debounce timer must not get a fresh compile (and a
     * fresh marker repaint) out of an idle buffer. */
    Buf out;
    lsp_session_flush(&out);
    CHECK(out.data && strcmp(out.data, "[]") == 0,
          "flushing a clean session publishes nothing");
    buf_free(&out);
}

static void test_did_close_drops_the_document(void) {
    fresh_session();
    session_open("file:///project/main.tur", "(cons 1 2)");

    Buf closed = send_msg(
        "{\"jsonrpc\":\"2.0\",\"method\":\"textDocument/didClose\","
        "\"params\":{\"textDocument\":{\"uri\":\"file:///project/main.tur\"}}}");
    buf_free(&closed);

    Buf out = send_msg(
        "{\"jsonrpc\":\"2.0\",\"id\":19,\"method\":\"textDocument/hover\","
        "\"params\":{\"textDocument\":{\"uri\":\"file:///project/main.tur\"},"
        "\"position\":{\"line\":0,\"character\":2}}}");
    CHECK(contains(&out, "\"result\":null"),
          "hover on a closed document answers null");
    buf_free(&out);
}

/* -------------------------------------------------------------------------
 * Text decoding
 * --------------------------------------------------------------------- */

static void test_escapes_are_decoded(void) {
    fresh_session();
    /* é has to become the two UTF-8 bytes, not the five literal
     * characters -- otherwise every byte offset after it in the buffer shifts
     * and the positions in every later answer are wrong. */
    session_open("file:///project/main.tur",
                 "(def caf\\u00e9 1)\\n(local-fn 1 2)");

    Buf out = send_msg(
        "{\"jsonrpc\":\"2.0\",\"id\":20,\"method\":\"textDocument/hover\","
        "\"params\":{\"textDocument\":{\"uri\":\"file:///project/main.tur\"},"
        "\"position\":{\"line\":1,\"character\":2}}}");
    CHECK(contains(&out, "local-fn"),
          "a decoded \\uXXXX escape keeps later line/column offsets correct");
    buf_free(&out);
}

/* -------------------------------------------------------------------------
 * Scope, rename and references
 * (editor-intelligence-follow-through-plan, S1/A1/A2/A3)
 *
 * The assertions here are about JSON shapes -- a WorkspaceEdit's `changes`
 * map, a refusal's message -- which is what §6 of the plan says belongs in
 * this harness rather than in a fixture: a fixture can only compare a
 * program's printed output, and none of this produces any.
 *
 * The stub fills the binding table the same way it fills the symbol index.
 * Offsets are computed from SCOPE_DOC with strstr rather than written down,
 * so editing the document text cannot silently decouple what the stub claims
 * from what the server scans.
 * --------------------------------------------------------------------- */

/* line 0:  (def total 9)
 * line 1:  (defn f [n]
 * line 2:    (let [total 1]
 * line 3:      (+ total n)))          */
#define SCOPE_DOC \
    "(def total 9)\n(defn f [n]\n  (let [total 1]\n    (+ total n)))\n"

#define SCOPE_URI "file:///project/scope.tur"

/* Which locals the next analysis reports: none, the real two, one whose
 * binder has no span (macro-introduced), or more than the table can hold. */
static enum { SCOPE_NONE, SCOPE_REAL, SCOPE_MACRO, SCOPE_FLOOD } stub_scope =
    SCOPE_NONE;

/* Byte offset of the `nth` occurrence of `needle` in SCOPE_DOC. */
static uint32_t doc_off(const char *needle, int nth) {
    const char *p = SCOPE_DOC;
    for (;;) {
        p = strstr(p, needle);
        if (!p) { fprintf(stderr, "FAIL: SCOPE_DOC has no %s\n", needle);
                  failed++; return 0; }
        if (nth-- == 0) return (uint32_t)(p - SCOPE_DOC);
        p += strlen(needle);
    }
}

static void stub_bind(const char *name, LspBindKind kind, int depth,
                      uint32_t line, uint32_t col_start,
                      uint32_t def_start, uint32_t def_end,
                      uint32_t scope_start, uint32_t scope_end) {
    LspBinding b;
    memset(&b, 0, sizeof(b));
    snprintf(b.name, sizeof(b.name), "%s", name);
    b.kind            = kind;
    b.depth           = depth;
    b.def_line        = line;
    b.def_col_start   = col_start;
    b.def_col_end     = col_start + (uint32_t)strlen(name);
    b.def_off_start   = def_start;
    b.def_off_end     = def_end;
    b.scope_start_off = scope_start;
    b.scope_end_off   = scope_end;
    lsp_scope_record(&b);
}

/* The two locals SCOPE_DOC really has, in the coordinates the real collector
 * would report: `n`, the parameter of `f`, and `total`, the `let` binding
 * that shadows the global of the same name. */
static void stub_fill_scope(void) {
    uint32_t doc_len   = (uint32_t)strlen(SCOPE_DOC);
    uint32_t n_off     = doc_off("n]", 0);
    uint32_t total_off = doc_off("total 1", 0);
    /* The `let` binding is live from the end of its own init, not from its
     * binder -- which is what keeps `(let [x (+ x 1)] ...)` honest. */
    uint32_t total_scope = doc_off("1]", 0) + 1;

    switch (stub_scope) {
    case SCOPE_NONE:
        break;
    case SCOPE_REAL:
        stub_bind("n", LSP_BIND_PARAM, 1, 2, 10,
                  n_off, n_off + 1, n_off + 1, doc_len);
        stub_bind("total", LSP_BIND_LET, 2, 3, 9,
                  total_off, total_off + 5, total_scope, doc_len);
        break;
    case SCOPE_MACRO:
        /* def_line 0 and no def range: a binder whose position is a point in
         * expanded source that does not exist in this file. */
        stub_bind("total", LSP_BIND_LET, 2, 0, 0, 0, 0, total_scope, doc_len);
        break;
    case SCOPE_FLOOD: {
        /* Past the table's cap, so lsp_scope_truncated() latches. */
        for (int i = 0; i < 6000; i++)
            stub_bind("filler", LSP_BIND_LET, 1, 3, 1, 1, 2, 2, 3);
        break;
    }
    }
}

/* Escape a raw document into a JSON string body and open it. */
static void session_open_raw(const char *uri, const char *raw) {
    Buf esc;
    buf_init(&esc);
    for (const char *p = raw; *p; p++) {
        if (*p == '\n')      buf_puts(&esc, "\\n");
        else if (*p == '"')  buf_puts(&esc, "\\\"");
        else if (*p == '\\') buf_puts(&esc, "\\\\");
        else                 buf_putc(&esc, *p);
    }
    buf_putc(&esc, '\0');
    session_open(uri, esc.data);
    buf_free(&esc);
}

static Buf scope_request(const char *method, int line, int ch,
                         const char *extra) {
    Buf b;
    buf_init(&b);
    buf_printf(&b, "{\"jsonrpc\":\"2.0\",\"id\":9,\"method\":\"%s\","
                   "\"params\":{\"textDocument\":{\"uri\":\"%s\"},"
                   "\"position\":{\"line\":%d,\"character\":%d}%s}}",
               method, SCOPE_URI, line, ch, extra ? extra : "");
    buf_putc(&b, '\0');
    Buf out = send_msg(b.data);
    buf_free(&b);
    return out;
}

static void scope_session(void) {
    fresh_session();
    session_open_raw(SCOPE_URI, SCOPE_DOC);
}

static void test_initialize_advertises_rename_and_references(void) {
    lsp_session_reset();
    Buf out = send_msg("{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\","
                       "\"params\":{}}");
    CHECK(contains(&out, "\"renameProvider\":{\"prepareProvider\":true}"),
          "rename is advertised in the prepare form, not as a bare boolean");
    CHECK(contains(&out, "\"referencesProvider\":true"),
          "initialize advertises references");
    buf_free(&out);
}

static void test_highlight_local_stops_at_its_scope(void) {
    stub_scope = SCOPE_REAL;
    scope_session();
    /* The caret on the `let`-bound `total` (line 2). */
    Buf out = scope_request("textDocument/documentHighlight", 2, 9, NULL);
    CHECK(contains(&out, "\"line\":2"), "the binder is reported");
    CHECK(contains(&out, "\"line\":3"), "the use inside the let is reported");
    CHECK(!contains(&out, "\"line\":0"),
          "the global `total` on line 0 is a different variable");
    CHECK(contains(&out, "\"kind\":3"),
          "the binder is marked Write, not Text");
    buf_free(&out);
    stub_scope = SCOPE_NONE;
}

static void test_highlight_global_skips_the_shadowed_region(void) {
    stub_scope = SCOPE_REAL;
    scope_session();
    /* The caret on the global `total` (line 0). Its uses are the whole file
     * MINUS the region the `let` covers -- the half that, missing, makes
     * renaming the outer name rewrite the inner one. */
    Buf out = scope_request("textDocument/documentHighlight", 0, 5, NULL);
    CHECK(contains(&out, "\"line\":0"), "the global's own definition");
    CHECK(!contains(&out, "\"line\":2"),
          "the shadowing binder is not a use of the global");
    CHECK(!contains(&out, "\"line\":3"),
          "the shadowed use is not a use of the global");
    buf_free(&out);
    stub_scope = SCOPE_NONE;
}

static void test_prepare_rename_answers_with_the_range(void) {
    stub_scope = SCOPE_REAL;
    scope_session();
    Buf out = scope_request("textDocument/prepareRename", 2, 9, NULL);
    CHECK(contains(&out, "\"placeholder\":\"total\""),
          "prepareRename echoes the current name as the placeholder");
    CHECK(contains(&out, "\"character\":8"),
          "prepareRename reports the identifier's own start column");
    buf_free(&out);
    stub_scope = SCOPE_NONE;
}

static void test_rename_local_leaves_the_global_alone(void) {
    stub_scope = SCOPE_REAL;
    scope_session();
    Buf out = scope_request("textDocument/rename", 2, 9,
                            ",\"newName\":\"acc\"");
    CHECK(contains(&out, "\"changes\""), "rename answers with a WorkspaceEdit");
    CHECK(contains(&out, SCOPE_URI), "the edit names the document uri");
    CHECK(contains(&out, "\"newText\":\"acc\""), "the new name is carried");
    CHECK(!contains(&out, "\"line\":0"),
          "the global `total` on line 0 is not rewritten");
    buf_free(&out);
    stub_scope = SCOPE_NONE;
}

static void test_prepare_rename_refuses_a_macro_binding(void) {
    stub_scope = SCOPE_MACRO;
    scope_session();
    Buf out = scope_request("textDocument/prepareRename", 3, 8, NULL);
    CHECK(contains(&out, "macro-introduced"),
          "a binder with no span in this file refuses, with a reason");
    CHECK(!contains(&out, "\"result\""),
          "a refusal is an error, never a range the client can act on");
    buf_free(&out);
    stub_scope = SCOPE_NONE;
}

static void test_rename_refuses_a_macro_binding_too(void) {
    stub_scope = SCOPE_MACRO;
    scope_session();
    /* A client that skipped prepareRename must still not get an edit. */
    Buf out = scope_request("textDocument/rename", 3, 8,
                            ",\"newName\":\"acc\"");
    CHECK(contains(&out, "macro-introduced"),
          "rename repeats the refusal rather than trusting the client asked");
    CHECK(!contains(&out, "\"changes\""), "and produces no edit");
    buf_free(&out);
    stub_scope = SCOPE_NONE;
}

static void test_prepare_rename_refuses_a_truncated_table(void) {
    stub_scope = SCOPE_FLOOD;
    scope_session();
    Buf out = scope_request("textDocument/prepareRename", 0, 5, NULL);
    CHECK(contains(&out, "too large to rename safely"),
          "an overflowed binding table refuses rather than guessing");
    buf_free(&out);
    stub_scope = SCOPE_NONE;
}

static void test_prepare_rename_refuses_a_stdlib_symbol(void) {
    stub_scope = SCOPE_NONE;
    fresh_session();
    session_open_raw(SCOPE_URI, "(cons 1 2)\n");
    Buf out = scope_request("textDocument/prepareRename", 0, 2, NULL);
    CHECK(contains(&out, "stdlib symbol"),
          "a name defined under the stdlib root refuses");
    buf_free(&out);
}

static void test_references_are_scope_bounded(void) {
    stub_scope = SCOPE_REAL;
    scope_session();
    Buf out = scope_request("textDocument/references", 2, 9,
                            ",\"context\":{\"includeDeclaration\":true}");
    CHECK(contains(&out, "\"uri\":\"" SCOPE_URI "\""),
          "references answer with Locations, uri and all");
    CHECK(contains(&out, "\"line\":2"), "the declaration is included");
    CHECK(!contains(&out, "\"line\":0"),
          "the global of the same name is not a reference to this local");
    buf_free(&out);

    out = scope_request("textDocument/references", 2, 9,
                        ",\"context\":{\"includeDeclaration\":false}");
    CHECK(!contains(&out, "\"line\":2"),
          "includeDeclaration:false drops the declaration");
    CHECK(contains(&out, "\"line\":3"), "and keeps the use");
    buf_free(&out);
    stub_scope = SCOPE_NONE;
}

/* -------------------------------------------------------------------------
 * main
 * --------------------------------------------------------------------- */

int main(void) {
    printf("Running LSP session tests...\n");

    /* The stdlib cache is keyed off this; the stub reports `cons` as living
     * under it so the never-analyzed-document fallback has something to serve. */
    setenv("TUR_STDLIB_DIR", STUB_STDLIB_DIR, 1);
    diag_init(false);

    test_initialize();
    test_did_open_publishes_diagnostics();
    test_diagnostics_reach_the_client();
    test_completion_orders_local_first();
    test_completion_filters_by_prefix();
    test_hover();
    test_definition_reports_document_path();
    test_document_symbol_is_file_scoped();
    test_document_symbol_reports_real_kinds();
    test_completion_kind_follows_the_symbol_kind();
    test_document_highlight_skips_comments_and_strings();
    test_document_highlight_off_a_word_is_null();
    test_hover_falls_back_to_the_builtin_table();
    test_signature_help_falls_back_to_the_builtin_table();
    test_definition_on_a_builtin_is_still_null();
    test_signature_help();
    test_did_change_republishes();
    test_stale_index_survives_unparseable_text();
    test_did_change_takes_the_last_element();
    test_unknown_method_is_answered();
    test_unknown_notification_is_silent();
    test_cancel_on_the_session_path_is_a_noop();
    test_formatting_skips_the_analysis_flush();
    test_formatting_with_no_params_errors();
    test_exit_ends_the_session_without_exiting();
    test_stdlib_fallback_survives_a_reset();
    test_flush_with_nothing_dirty_is_empty();
    test_did_close_drops_the_document();
    test_escapes_are_decoded();
    test_initialize_advertises_rename_and_references();
    test_highlight_local_stops_at_its_scope();
    test_highlight_global_skips_the_shadowed_region();
    test_prepare_rename_answers_with_the_range();
    test_rename_local_leaves_the_global_alone();
    test_prepare_rename_refuses_a_macro_binding();
    test_rename_refuses_a_macro_binding_too();
    test_prepare_rename_refuses_a_truncated_table();
    test_prepare_rename_refuses_a_stdlib_symbol();
    test_references_are_scope_bounded();

    lsp_session_reset();

    printf("Results: %d passed, %d failed\n", passed, failed);
    return failed > 0 ? 1 : 0;
}
