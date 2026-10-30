/* wasm_backend_test.c -- exercise the browser's analysis backend natively.
 *
 * src/web/wasm_lsp.c is the front end the WASM playground uses instead of
 * main.c's compile_to_c: read, `#lang` sweep, stdlib autoload, elaborate,
 * collect. It normally only ever runs inside a wasm module, where nothing in
 * CI can look at it -- so a mistake in it would surface as "the playground's
 * completion is empty", found by a person, months later.
 *
 * Nothing in that file is actually wasm-specific: the Emscripten build differs
 * only in where the stdlib is mounted, and that is a TUR_STDLIB_DIR lookup.
 * Pointing it at the in-tree stdlib/ and driving the same lsp_session_handle
 * the bridge drives runs the real thing, against the real compiler, here.
 *
 * What this does NOT cover, and cannot: MEMFS temp-file creation (mkstemps
 * under Emscripten), the embedded /stdlib mount, and wasm bundle size. Those
 * need an emcc build.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>  /* mkdir */
#include <unistd.h>    /* rmdir */

#include "buf.h"
#include "lsp_session.h"
#include "web/wasm_glue.h"  /* turi_wasm_add_include_dir, turi_wasm_put_spice_file */
#include "platform_fs.h"  /* setenv on Windows */

static int passed = 0;
static int failed = 0;

#define CHECK(cond, msg) \
    do { \
        if (cond) { passed++; } \
        else { fprintf(stderr, "FAIL: %s (line %d)\n", (msg), __LINE__); failed++; } \
    } while (0)

static Buf send_msg(const char *json) {
    Buf out;
    lsp_session_handle(json, strlen(json), &out);
    return out;
}

static int contains(const Buf *b, const char *needle) {
    return b->data && strstr(b->data, needle) != NULL;
}

/* JSON-escape a source string into a didOpen and send it. */
static void open_doc(const char *uri, const char *text) {
    Buf b;
    buf_init(&b);
    buf_puts(&b, "{\"jsonrpc\":\"2.0\",\"method\":\"textDocument/didOpen\","
                 "\"params\":{\"textDocument\":{\"uri\":\"");
    buf_puts(&b, uri);
    buf_puts(&b, "\",\"text\":\"");
    for (const char *p = text; *p; p++) {
        switch (*p) {
            case '"':  buf_puts(&b, "\\\""); break;
            case '\\': buf_puts(&b, "\\\\"); break;
            case '\n': buf_puts(&b, "\\n");  break;
            default:   buf_putc(&b, *p);     break;
        }
    }
    buf_puts(&b, "\"}}}");
    buf_putc(&b, '\0');
    Buf out = send_msg(b.data);
    buf_free(&out);
    buf_free(&b);
}

static void fresh(void) {
    lsp_session_reset();
    Buf out = send_msg("{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\","
                       "\"params\":{}}");
    buf_free(&out);
}

/* -------------------------------------------------------------------------
 * The stdlib is genuinely in scope
 *
 * The playground's analysis has to prepend the same stdlib the compiler does.
 * Without it every buffer would report `cons` and `Option` as unknown -- code
 * the compiler accepts, flagged as broken by the editor sitting next to it.
 * --------------------------------------------------------------------- */

static void test_stdlib_is_autoloaded(void) {
    fresh();
    open_doc("file:///project/main.tur",
             "(defn twice [x : int] : int (* x 2))\nvec-\n");

    /* Query with a `vec-` prefix rather than an empty one. The response is
     * capped at 200 items and the stdlib's typeclass instances alone overrun
     * that, so an unfiltered query proves nothing about whether list.tur or
     * vec.tur were loaded -- it only proves the cap works. */
    Buf out = send_msg(
        "{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"textDocument/completion\","
        "\"params\":{\"textDocument\":{\"uri\":\"file:///project/main.tur\"},"
        "\"position\":{\"line\":1,\"character\":4}}}");

    CHECK(contains(&out, "\"label\":\"vec-new\"") ||
          contains(&out, "\"label\":\"vec-push\""),
          "stdlib symbols are in scope without an explicit import");
    CHECK(!contains(&out, "\"label\":\"twice\""),
          "the prefix filter is real, not a raw dump");
    buf_free(&out);
}

static void test_document_local_symbols_come_first(void) {
    fresh();
    open_doc("file:///project/main.tur",
             "(defn twice [x : int] : int (* x 2))\n");

    Buf out = send_msg(
        "{\"jsonrpc\":\"2.0\",\"id\":22,\"method\":\"textDocument/completion\","
        "\"params\":{\"textDocument\":{\"uri\":\"file:///project/main.tur\"},"
        "\"position\":{\"line\":1,\"character\":0}}}");

    /* With the cap in play this is not a nicety: the buffer's own definitions
     * are exactly the ones that get dropped if collection order wins. */
    const char *items = out.data ? strstr(out.data, "\"items\":[") : NULL;
    CHECK(items && strncmp(items, "\"items\":[{\"label\":\"twice\"", 25) == 0,
          "the document's own definition is the first item offered");
    buf_free(&out);
}

static void test_hover_reports_a_real_type(void) {
    fresh();
    open_doc("file:///project/main.tur",
             "(defn twice [x : int] : int (* x 2))\n(twice 21)\n");

    Buf out = send_msg(
        "{\"jsonrpc\":\"2.0\",\"id\":3,\"method\":\"textDocument/hover\","
        "\"params\":{\"textDocument\":{\"uri\":\"file:///project/main.tur\"},"
        "\"position\":{\"line\":1,\"character\":2}}}");

    CHECK(contains(&out, "twice"), "hover finds the symbol under the cursor");
    /* The rendered type comes from the elaborator, not from re-reading the
     * source, so this is the assertion that says elaboration really ran. */
    CHECK(contains(&out, "int"), "hover carries the inferred type");
    buf_free(&out);
}

/* -------------------------------------------------------------------------
 * Diagnostics
 * --------------------------------------------------------------------- */

static void test_type_error_becomes_a_diagnostic(void) {
    fresh();
    Buf b;
    buf_init(&b);
    buf_puts(&b, "{\"jsonrpc\":\"2.0\",\"method\":\"textDocument/didOpen\","
                 "\"params\":{\"textDocument\":{\"uri\":\"file:///project/bad.tur\","
                 "\"text\":\"(defn f [] : int (undefined-name 1))\"}}}");
    buf_putc(&b, '\0');
    Buf out = send_msg(b.data);

    CHECK(contains(&out, "textDocument/publishDiagnostics"),
          "an erroring buffer publishes diagnostics");
    CHECK(contains(&out, "\"severity\":1"),
          "the error is reported at severity 1");
    CHECK(contains(&out, "file:///project/bad.tur"),
          "the diagnostic is attributed to the document");
    CHECK(!contains(&out, "tur_lsp_"),
          "the analysis temp path never reaches the client");
    buf_free(&out);
    buf_free(&b);
}

static void test_clean_buffer_publishes_no_diagnostics(void) {
    fresh();
    open_doc("file:///project/ok.tur", "(defn f [] : int 1)\n");

    /* Re-analyse by editing, so the assertion is about a real compile rather
     * than about nothing having happened yet. */
    Buf out = send_msg(
        "{\"jsonrpc\":\"2.0\",\"method\":\"textDocument/didChange\","
        "\"params\":{\"textDocument\":{\"uri\":\"file:///project/ok.tur\"},"
        "\"contentChanges\":[{\"text\":\"(defn f [] : int 2)\\n\"}]}}");

    CHECK(contains(&out, "\"diagnostics\":[]"),
          "a buffer that compiles reports no markers");
    buf_free(&out);
}

/* -------------------------------------------------------------------------
 * #lang handling
 *
 * Try Turmeric's default buffer opens with `#lang turmeric/sweet`. If the
 * analysis handed that to the s-expression reader, the very first thing every
 * visitor saw would be a buffer covered in syntax errors describing nothing
 * they did.
 * --------------------------------------------------------------------- */

static void test_sweet_exp_buffer_is_read_correctly(void) {
    fresh();
    Buf b;
    buf_init(&b);
    buf_puts(&b, "{\"jsonrpc\":\"2.0\",\"method\":\"textDocument/didOpen\","
                 "\"params\":{\"textDocument\":{\"uri\":\"file:///project/sweet.tur\","
                 "\"text\":\"#lang turmeric/sweet\\n\\n"
                 "defn twice [x : int] : int\\n  *(x 2)\\n\"}}}");
    buf_putc(&b, '\0');
    Buf out = send_msg(b.data);

    CHECK(contains(&out, "\"diagnostics\":[]"),
          "a #lang turmeric/sweet buffer analyses without syntax errors");
    buf_free(&out);
    buf_free(&b);

    Buf syms = send_msg(
        "{\"jsonrpc\":\"2.0\",\"id\":4,\"method\":\"textDocument/documentSymbol\","
        "\"params\":{\"textDocument\":{\"uri\":\"file:///project/sweet.tur\"}}}");
    CHECK(contains(&syms, "\"name\":\"twice\""),
          "sweet-exp definitions land in the symbol index");
    buf_free(&syms);
}

/* -------------------------------------------------------------------------
 * A dynamic dialect is analysed as itself
 *
 * r7rs-lang-plan R9: the backend read the directive with the plain
 * detect_lang, which reports only the READER axis, and never set the file's
 * language -- so a `#lang saffron` or `#lang r7rs` tab was elaborated as
 * Turmeric (int parameter defaults, no Scheme lowering, no prelude) and the
 * editor flagged working code.  The broken r7rs buffer is the control: a probe
 * that cannot see a real error would pass the clean ones on no evidence.
 * --------------------------------------------------------------------- */

static void test_dynamic_dialect_buffers_analyse_as_themselves(void) {
    fresh();
    open_doc("file:///project/broken.tur",
             "#lang r7rs\n(import (scheme base) (scheme write))\n"
             "(display (undefined-fn-xyz 1))\n");
    Buf out = send_msg("{\"jsonrpc\":\"2.0\",\"method\":\"textDocument/didSave\","
                       "\"params\":{\"textDocument\":{\"uri\":\"file:///project/broken.tur\"}}}");
    buf_free(&out);

    fresh();
    Buf b; buf_init(&b);
    buf_puts(&b, "{\"jsonrpc\":\"2.0\",\"method\":\"textDocument/didOpen\","
                 "\"params\":{\"textDocument\":{\"uri\":\"file:///project/bad.scm.tur\","
                 "\"text\":\"#lang r7rs\\n(import (scheme base) (scheme write))\\n"
                 "(display (undefined-fn-xyz 1))\\n\"}}}");
    buf_putc(&b, '\0');
    out = send_msg(b.data);
    CHECK(contains(&out, "undefined-fn-xyz"),
          "control: a broken #lang r7rs buffer reports its unknown name");
    buf_free(&out); buf_free(&b);

    fresh();
    buf_init(&b);
    buf_puts(&b, "{\"jsonrpc\":\"2.0\",\"method\":\"textDocument/didOpen\","
                 "\"params\":{\"textDocument\":{\"uri\":\"file:///project/ok.scm.tur\","
                 "\"text\":\"#lang r7rs\\n(import (scheme base) (scheme write))\\n"
                 "(define (add a b) (+ a b))\\n(display (add 1.5 2.25))\\n"
                 "(write (car (quote (a #t #\\\\x))))\\n(newline)\\n\"}}}");
    buf_putc(&b, '\0');
    out = send_msg(b.data);
    CHECK(contains(&out, "\"diagnostics\":[]"),
          "a valid #lang r7rs buffer is lowered, gets the prelude, and is clean");
    buf_free(&out); buf_free(&b);

    fresh();
    buf_init(&b);
    buf_puts(&b, "{\"jsonrpc\":\"2.0\",\"method\":\"textDocument/didOpen\","
                 "\"params\":{\"textDocument\":{\"uri\":\"file:///project/ok.saf.tur\","
                 "\"text\":\"#lang saffron\\n(defn add [a b] (+ a b))\\n"
                 "(defn main [] (println (add 1.5 2.25)) 0)\\n\"}}}");
    buf_putc(&b, '\0');
    out = send_msg(b.data);
    CHECK(contains(&out, "\"diagnostics\":[]"),
          "a valid #lang saffron buffer keeps its any parameters (a float argument is fine)");
    buf_free(&out); buf_free(&b);

    /* And the language does not leak into the next Turmeric tab. */
    fresh();
    open_doc("file:///project/plain.tur",
             "(defn add [a : int b : int] : int (+ a b))\n"
             "(defn main [] : int (println (add 1 2)) 0)\n");
    buf_init(&b);
    buf_puts(&b, "{\"jsonrpc\":\"2.0\",\"method\":\"textDocument/didOpen\","
                 "\"params\":{\"textDocument\":{\"uri\":\"file:///project/plain2.tur\","
                 "\"text\":\"(defn main [] : int (println (add2 1)) 0)\\n\"}}}");
    buf_putc(&b, '\0');
    out = send_msg(b.data);
    CHECK(contains(&out, "add2"),
          "a Turmeric tab after the dynamic ones still reports its own errors");
    buf_free(&out); buf_free(&b);
}

/* -------------------------------------------------------------------------
 * Cross-tab workspace symbols
 *
 * workspace/symbol covers open documents only -- recorded as a gap against
 * native clients, where most of a project is on disk and unopened. In the
 * playground every tab IS an open document, so the gap does not exist here.
 * --------------------------------------------------------------------- */

static void test_workspace_symbol_spans_tabs(void) {
    fresh();
    open_doc("file:///project/a.tur", "(defn alpha-fn [] : int 1)\n");
    open_doc("file:///project/b.tur", "(defn beta-fn [] : int 2)\n");

    Buf out = send_msg(
        "{\"jsonrpc\":\"2.0\",\"id\":5,\"method\":\"workspace/symbol\","
        "\"params\":{\"query\":\"-fn\"}}");

    CHECK(contains(&out, "\"name\":\"alpha-fn\""), "tab a contributes symbols");
    CHECK(contains(&out, "\"name\":\"beta-fn\""),  "tab b contributes symbols");
    CHECK(contains(&out, "file:///project/a.tur"),
          "each result names the tab it came from");
    buf_free(&out);
}

/* -------------------------------------------------------------------------
 * Definition across tabs
 * --------------------------------------------------------------------- */

static void test_definition_within_a_tab(void) {
    fresh();
    open_doc("file:///project/main.tur",
             "(defn helper [] : int 7)\n(defn use-it [] : int (helper))\n");

    Buf out = send_msg(
        "{\"jsonrpc\":\"2.0\",\"id\":6,\"method\":\"textDocument/definition\","
        "\"params\":{\"textDocument\":{\"uri\":\"file:///project/main.tur\"},"
        "\"position\":{\"line\":1,\"character\":23}}}");

    CHECK(contains(&out, "file:///project/main.tur"),
          "definition resolves to the document's own uri");
    CHECK(contains(&out, "\"line\":0"),
          "definition points at the defining line, 0-based");
    buf_free(&out);
}

/* -------------------------------------------------------------------------
 * Definitions that land in the stdlib, and the reader that opens them
 * (try-turmeric-navigation-and-minimap-plan, M4)
 *
 * Measured before the work started (plan §4.1): the server does return a real
 * stdlib Location, which is what made the read-only-tab half of M4 worth
 * building rather than shrinking to a keybinding. This pins that answer, and
 * the export the page reads the file back through.
 * --------------------------------------------------------------------- */

/* Exported from wasm_lsp.c for the worker; no header, since the only other
 * caller is a JS bridge. */
extern char *turi_wasm_read_file(const char *path);

static void test_definition_reaches_the_stdlib(void) {
    fresh();
    open_doc("file:///project/main.tur",
             "(def p (pair 1 2))\n");

    Buf out = send_msg(
        "{\"jsonrpc\":\"2.0\",\"id\":20,\"method\":\"textDocument/definition\","
        "\"params\":{\"textDocument\":{\"uri\":\"file:///project/main.tur\"},"
        "\"position\":{\"line\":0,\"character\":9}}}");

    CHECK(contains(&out, "stdlib/pair.tur"),
          "a stdlib name resolves to its defining file, not to null");
    CHECK(contains(&out, "\"uri\":\"file://"),
          "the location is a file:// uri the client can look up");
    buf_free(&out);
}

static void test_read_file_serves_a_stdlib_source(void) {
    char path[1024];
    const char *root = getenv("TUR_STDLIB_DIR");
    snprintf(path, sizeof(path), "%s/pair.tur", root ? root : "stdlib");

    char *text = turi_wasm_read_file(path);
    CHECK(text != NULL, "the reader returns a stdlib source file");
    if (text) {
        CHECK(strstr(text, "pair") != NULL,
              "the text is the file, not an empty buffer");
        free(text);
    }
}

static void test_read_file_refuses_everything_else(void) {
    char path[1024];
    const char *root = getenv("TUR_STDLIB_DIR");
    if (!root) root = "stdlib";

    /* The whole point of exporting a named operation instead of adding FS to
     * EXPORTED_RUNTIME_METHODS is that the capability the page gains is "show
     * me where `map` is defined", not "read any path in the module". Each of
     * these is a way that could have leaked. */
    char *outside = turi_wasm_read_file("/etc/passwd");
    CHECK(outside == NULL, "a path outside the stdlib is refused");
    free(outside);

    snprintf(path, sizeof(path), "%s/../etc/passwd", root);
    char *traversal = turi_wasm_read_file(path);
    CHECK(traversal == NULL, "a `..` segment cannot walk out of the tree");
    free(traversal);

    /* A sibling directory whose name merely starts with the root's. */
    snprintf(path, sizeof(path), "%s-elsewhere/secret.tur", root);
    char *prefix = turi_wasm_read_file(path);
    CHECK(prefix == NULL, "a prefix match is not a containment check");
    free(prefix);

    snprintf(path, sizeof(path), "%s/pair.c", root);
    char *wrong_ext = turi_wasm_read_file(path);
    CHECK(wrong_ext == NULL, "only .tur sources are readable");
    free(wrong_ext);

    char *root_itself = turi_wasm_read_file(root);
    CHECK(root_itself == NULL, "the root directory is not a file to read");
    free(root_itself);

    char *nothing = turi_wasm_read_file(NULL);
    CHECK(nothing == NULL, "a null path is refused rather than crashing");

    snprintf(path, sizeof(path), "%s/no-such-module.tur", root);
    char *missing = turi_wasm_read_file(path);
    CHECK(missing == NULL, "a file that is not there reads as a refusal");
    free(missing);
}

/* -------------------------------------------------------------------------
 * A buffer that has never parsed still completes
 * --------------------------------------------------------------------- */

static void test_broken_buffer_falls_back_to_stdlib(void) {
    fresh();
    /* Opened already-unbalanced: there is no earlier revision to fall back on,
     * so the stdlib surface is the only honest answer -- and it is a correct
     * one, because Turmeric auto-loads it. */
    open_doc("file:///project/broken.tur", "(defn f [] : int (vec-\n");

    Buf out = send_msg(
        "{\"jsonrpc\":\"2.0\",\"id\":7,\"method\":\"textDocument/completion\","
        "\"params\":{\"textDocument\":{\"uri\":\"file:///project/broken.tur\"},"
        "\"position\":{\"line\":0,\"character\":22}}}");

    CHECK(contains(&out, "\"label\":\"vec-new\"") ||
          contains(&out, "\"label\":\"vec-push\""),
          "a never-parsed buffer still completes stdlib names");
    CHECK(!contains(&out, "\"label\":\"f\""),
          "the fallback carries no document-local symbols");
    buf_free(&out);
}

/* -------------------------------------------------------------------------
 * Elaborator-minted names never reach a human-facing listing
 *
 * Anonymous lambdas lift to `__fn_774` globals and typeclass instances
 * elaborate to `__inst_Eq_eq_qu_int` methods. Both used to land in the symbol
 * index, so completion offered ~68 of them inside a 200-item cap and the
 * outline view listed the mangled instance methods.
 *
 * The filter is on Binding.is_synthesized, not on the `__` spelling: the
 * stdlib uses that prefix for its own hand-written internal helpers, which
 * have real source and are worth hovering. `__buffer-helper` below stands for
 * that class -- it is written in the buffer, so it must survive.
 * --------------------------------------------------------------------- */

static void test_synthesized_names_are_not_offered(void) {
    fresh();
    open_doc("file:///project/main.tur",
             "(defn __buffer-helper [x : int] : int (* x 2))\n"
             "(defn apply-twice [n : int] : int\n"
             "  ((fn [y : int] : int (__buffer-helper y)) n))\n"
             "\n");

    /* An empty prefix, deliberately: this is the query the cap truncates, and
     * the one where the mangled names were crowding out real ones. */
    Buf out = send_msg(
        "{\"jsonrpc\":\"2.0\",\"id\":30,\"method\":\"textDocument/completion\","
        "\"params\":{\"textDocument\":{\"uri\":\"file:///project/main.tur\"},"
        "\"position\":{\"line\":3,\"character\":0}}}");

    CHECK(!contains(&out, "\"label\":\"__inst_"),
          "instance methods are not completion candidates");
    CHECK(!contains(&out, "\"label\":\"__fn_"),
          "lifted lambdas are not completion candidates");
    CHECK(contains(&out, "\"label\":\"__buffer-helper\""),
          "a hand-written __-prefixed name is still offered");
    buf_free(&out);
}

static void test_outline_omits_lifted_lambdas(void) {
    fresh();
    open_doc("file:///project/outline.tur",
             "(defn apply-twice [n : int] : int\n"
             "  ((fn [y : int] : int y) n))\n");

    Buf out = send_msg(
        "{\"jsonrpc\":\"2.0\",\"id\":31,\"method\":\"textDocument/documentSymbol\","
        "\"params\":{\"textDocument\":{\"uri\":\"file:///project/outline.tur\"}}}");

    /* documentSymbol already restricts itself to this file, which is exactly
     * why the lifted lambda showed up here: its span is the buffer's own. */
    CHECK(contains(&out, "\"name\":\"apply-twice\""),
          "the outline lists the function the user wrote");
    CHECK(!contains(&out, "\"name\":\"__fn_"),
          "the outline does not list the lambda the elaborator lifted");
    buf_free(&out);
}

/* -------------------------------------------------------------------------
 * main
 * --------------------------------------------------------------------- */

/* -------------------------------------------------------------------------
 * The page's mounted spices (raylib-web-playground-plan P2)
 *
 * The raylib playground mounts spice packages and puts each `src/` on the
 * include-dir list; the analysis has to resolve imports through that list,
 * or every `(import raylib/core ...)` the playground runs fine is flagged as
 * a missing module.  A defscript header is read as the Compile engine reads
 * it, not reported as needing --enable=script-deps.
 * --------------------------------------------------------------------- */

static void test_import_resolves_through_added_include_dir(void) {
    /* A directory of its own under the working directory (no mkdtemp: MinGW
     * has none), created through platform_fs.h's mkdir. */
    const char *dir = "tur-lsp-wasm-backend-inc";
    mkdir(dir, 0777);
    char sub[300], path[340];
    snprintf(sub, sizeof sub, "%s/arcadetest", dir);
    mkdir(sub, 0777);
    snprintf(path, sizeof path, "%s/thing.tur", sub);
    FILE *f = fopen(path, "w");
    if (!f) { CHECK(0, "write the module"); return; }
    fputs("(defmodule arcadetest/thing\n"
          "  (export twice)\n"
          "  (defn twice [x : int] : int (* 2 x)))\n", f);
    fclose(f);

    /* Control: with nothing mounted (the list starts empty) the import is an
     * error.  The list is never cleared here: turi_wasm_clear_include_dirs
     * keeps its strings for the environments that borrow them, which LSan
     * would report. */
    fresh();
    Buf b;
    buf_init(&b);
    buf_puts(&b, "{\"jsonrpc\":\"2.0\",\"method\":\"textDocument/didOpen\","
                 "\"params\":{\"textDocument\":{\"uri\":\"file:///project/imp.tur\","
                 "\"text\":\"(defmodule m (export) (import arcadetest/thing :refer [twice]) "
                 "(defn main [] : int (twice 21)))\"}}}");
    buf_putc(&b, '\0');
    Buf out = send_msg(b.data);
    CHECK(contains(&out, "\"severity\":1"),
          "control: an import of an unmounted module is an error");
    buf_free(&out);

    /* Mounted: the same buffer is clean. */
    turi_wasm_add_include_dir(dir);
    fresh();
    out = send_msg(b.data);
    CHECK(contains(&out, "\"diagnostics\":[]"),
          "an import resolves through a directory the page added");
    buf_free(&out);
    buf_free(&b);

    remove(path);
    rmdir(sub);
    rmdir(dir);
}

static void test_defscript_header_is_read_not_refused(void) {
    fresh();
    open_doc("file:///project/hdr.tur", "(defn f [] : int 1)\n");
    Buf out = send_msg(
        "{\"jsonrpc\":\"2.0\",\"method\":\"textDocument/didChange\","
        "\"params\":{\"textDocument\":{\"uri\":\"file:///project/hdr.tur\"},"
        "\"contentChanges\":[{\"text\":\"(defscript :spices #map{\\\"math\\\" "
        "#map{:path \\\"../math\\\"}})\\n(defn f [] : int 2)\\n\"}]}}");
    CHECK(contains(&out, "\"diagnostics\":[]"),
          "a defscript header raises no TUR-E0633 in the editor");
    buf_free(&out);
}

static void test_put_spice_file_refuses_paths_outside_spices(void) {
    CHECK(turi_wasm_put_spice_file("/etc/passwd.tur", "") == -1,
          "a path outside /spices/ is refused");
    CHECK(turi_wasm_put_spice_file("/spices/a/src/../../../tmp/x.tur", "") == -1,
          "a path with .. is refused");
    CHECK(turi_wasm_put_spice_file("/spices/a/lib/x.tur", "") == -1,
          "a path outside a spice's src/ is refused");
    CHECK(turi_wasm_put_spice_file("/spices/a/src/x.c", "") == -1,
          "a file that is not Turmeric source is refused");
    CHECK(turi_wasm_put_spice_file(NULL, "") == -1 &&
          turi_wasm_put_spice_file("/spices/a/src/x.tur", NULL) == -1,
          "a missing path or text is refused");
}

int main(int argc, char **argv) {
    /* The wasm build finds the stdlib at its MEMFS mount point; here it is
     * wherever the harness says, defaulting to the in-tree copy. */
    const char *stdlib_dir = (argc > 1) ? argv[1] : "stdlib";
    setenv("TUR_STDLIB_DIR", stdlib_dir, 1);

    printf("Running WASM LSP backend tests (stdlib=%s)...\n", stdlib_dir);

    test_stdlib_is_autoloaded();
    test_document_local_symbols_come_first();
    test_hover_reports_a_real_type();
    test_type_error_becomes_a_diagnostic();
    test_clean_buffer_publishes_no_diagnostics();
    test_sweet_exp_buffer_is_read_correctly();
    test_dynamic_dialect_buffers_analyse_as_themselves();
    test_workspace_symbol_spans_tabs();
    test_definition_within_a_tab();
    test_definition_reaches_the_stdlib();
    test_read_file_serves_a_stdlib_source();
    test_read_file_refuses_everything_else();
    test_broken_buffer_falls_back_to_stdlib();
    test_synthesized_names_are_not_offered();
    test_outline_omits_lifted_lambdas();
    test_import_resolves_through_added_include_dir();
    test_defscript_header_is_read_not_refused();
    test_put_spice_file_refuses_paths_outside_spices();

    lsp_session_reset();

    printf("Results: %d passed, %d failed\n", passed, failed);
    return failed > 0 ? 1 : 0;
}
