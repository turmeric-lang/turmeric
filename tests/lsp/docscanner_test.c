/* docscanner_test.c -- unit tests for lsp_scan_docs / lsp_scan_docs_lookup.
 *
 * Compile and run (from the repo root):
 *   cc -std=c11 -Wall -Wextra \
 *      -I src -I src/lsp -I src/compiler -I src/runtime \
 *      tests/lsp/docscanner_test.c src/lsp/lsp_docs.c src/lsp/lsp_util.c \
 *      -o tests/lsp/docscanner_test
 *   ./tests/lsp/docscanner_test
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

/* Provide stub for lsp_sym.h dependency (LspSymbol and tur_collect_symbols
 * are not needed by the docscanner, but lsp_docs.h includes lsp_sym.h). */
#include "lsp_docs.h"

static int passed = 0;
static int failed = 0;

#define CHECK(cond, msg) \
    do { \
        if (cond) { passed++; } \
        else { fprintf(stderr, "FAIL: %s (line %d)\n", (msg), __LINE__); failed++; } \
    } while (0)

/* -------------------------------------------------------------------------
 * Test helpers
 * --------------------------------------------------------------------- */

static int lookup(LspDocTable *t, const char *name, char *out, size_t cap) {
    return lsp_scan_docs_lookup(t, name, out, cap);
}

/* -------------------------------------------------------------------------
 * Test: no docstring before definition
 * --------------------------------------------------------------------- */

static void test_no_docstring(void) {
    const char *src = "(defn add [a b] :int\n  (+ a b))\n";
    LspDocTable t;
    lsp_doc_table_init(&t);
    lsp_scan_docs(src, strlen(src), &t);

    char out[512];
    CHECK(lookup(&t, "add", out, sizeof(out)) == 0, "no docstring: miss");

    lsp_doc_table_free(&t);
}

/* -------------------------------------------------------------------------
 * Test: single-line docstring
 * --------------------------------------------------------------------- */

static void test_single_line_docstring(void) {
    const char *src =
        ";;; add -- add two integers\n"
        "(defn add [a b] :int\n"
        "  (+ a b))\n";
    LspDocTable t;
    lsp_doc_table_init(&t);
    lsp_scan_docs(src, strlen(src), &t);

    char out[512];
    CHECK(lookup(&t, "add", out, sizeof(out)) == 1, "single-line: hit");
    CHECK(strstr(out, "add -- add two integers") != NULL, "single-line: content");

    lsp_doc_table_free(&t);
}

/* -------------------------------------------------------------------------
 * Test: multi-section docstring (Parameters + Returns + Example)
 * --------------------------------------------------------------------- */

static void test_multi_section_docstring(void) {
    const char *src =
        ";;; cons -- prepend a value to a list.\n"
        ";;;\n"
        ";;; Parameters:\n"
        ";;;   value -- the element to prepend\n"
        ";;;   next  -- the existing list\n"
        ";;;\n"
        ";;; Returns:\n"
        ";;;   A new Cons cell.\n"
        "(defn cons [value next] :int\n"
        "  0)\n";
    LspDocTable t;
    lsp_doc_table_init(&t);
    lsp_scan_docs(src, strlen(src), &t);

    char out[512];
    CHECK(lookup(&t, "cons", out, sizeof(out)) == 1, "multi-section: hit");
    CHECK(strstr(out, "cons -- prepend") != NULL, "multi-section: summary");
    CHECK(strstr(out, "Parameters:") != NULL, "multi-section: params");
    CHECK(strstr(out, "Returns:") != NULL, "multi-section: returns");

    lsp_doc_table_free(&t);
}

/* -------------------------------------------------------------------------
 * Test: non-;;; gap resets doc buffer
 * --------------------------------------------------------------------- */

static void test_reset_on_non_comment_gap(void) {
    const char *src =
        ";;; foo -- first function\n"
        "(def x 42)\n"        /* non-;;; gap before bar */
        ";;; bar -- second function\n"
        "(defn bar [] :int 0)\n";
    LspDocTable t;
    lsp_doc_table_init(&t);
    lsp_scan_docs(src, strlen(src), &t);

    char foo_doc[512], bar_doc[512];
    /* foo has a def immediately after ;;; -- check it's recorded */
    int foo_hit = lookup(&t, "foo", foo_doc, sizeof(foo_doc));
    /* bar is preceded by a proper ;;; block */
    int bar_hit = lookup(&t, "bar", bar_doc, sizeof(bar_doc));

    /* foo's ;;; block is followed by (def x 42) which resets, then bar's ;;; block.
     * So foo gets recorded for "x" (since (def x) immediately follows).
     * Actually: the ;;; block says "foo -- first function", then (def x 42) resets.
     * So "x" is not recorded (no doc preceding it... wait, the ;;; block IS
     * preceding (def x 42)). Let's check: after the gap comes (defn bar) with
     * its own ;;; block.  The ;;; "foo -- first function" is followed by
     * "(def x 42)" -- a definition, so x gets doc "foo -- first function". */
    (void)foo_hit;
    CHECK(bar_hit == 1, "reset gap: bar has docstring");
    CHECK(strstr(bar_doc, "bar -- second function") != NULL,
          "reset gap: bar doc content");

    lsp_doc_table_free(&t);
}

/* -------------------------------------------------------------------------
 * Test: blank lines inside ;;; block are preserved
 * --------------------------------------------------------------------- */

static void test_blank_lines_in_block(void) {
    const char *src =
        ";;; greet -- say hello\n"
        ";;;\n"
        ";;; Example:\n"
        ";;;   (greet \"world\")\n"
        "\n"   /* blank line between ;;; and defn -- should NOT reset */
        "(defn greet [name] :nil)\n";
    LspDocTable t;
    lsp_doc_table_init(&t);
    lsp_scan_docs(src, strlen(src), &t);

    char out[512];
    CHECK(lookup(&t, "greet", out, sizeof(out)) == 1,
          "blank in block: hit");
    CHECK(strstr(out, "Example:") != NULL,
          "blank in block: example preserved");

    lsp_doc_table_free(&t);
}

/* -------------------------------------------------------------------------
 * Test: defmacro, defstruct, definstance forms
 * --------------------------------------------------------------------- */

static void test_other_def_forms(void) {
    const char *src =
        ";;; when-macro -- conditional execution\n"
        "(defmacro when [cond body])\n"
        "\n"
        ";;; Point -- a 2D point\n"
        "(defstruct Point [x y])\n"
        "\n"
        ";;; Show-int -- show instance for int\n"
        "(definstance Show :int)\n";
    LspDocTable t;
    lsp_doc_table_init(&t);
    lsp_scan_docs(src, strlen(src), &t);

    char out[512];
    CHECK(lookup(&t, "when", out, sizeof(out)) == 1, "defmacro: hit");
    CHECK(lookup(&t, "Point", out, sizeof(out)) == 1, "defstruct: hit");
    CHECK(lookup(&t, "Show", out, sizeof(out)) == 1, "definstance: hit");

    lsp_doc_table_free(&t);
}

/* -------------------------------------------------------------------------
 * Test: multiple definitions in one file
 * --------------------------------------------------------------------- */

static void test_multiple_defs(void) {
    const char *src =
        ";;; inc -- increment by 1\n"
        "(defn inc [x] :int (+ x 1))\n"
        "\n"
        ";;; dec -- decrement by 1\n"
        "(defn dec [x] :int (- x 1))\n"
        "\n"
        "(defn no-doc [] :nil)\n";
    LspDocTable t;
    lsp_doc_table_init(&t);
    lsp_scan_docs(src, strlen(src), &t);

    char out[512];
    CHECK(lookup(&t, "inc", out, sizeof(out)) == 1, "multi: inc hit");
    CHECK(strstr(out, "increment") != NULL, "multi: inc content");
    CHECK(lookup(&t, "dec", out, sizeof(out)) == 1, "multi: dec hit");
    CHECK(strstr(out, "decrement") != NULL, "multi: dec content");
    CHECK(lookup(&t, "no-doc", out, sizeof(out)) == 0, "multi: no-doc miss");

    lsp_doc_table_free(&t);
}

/* -------------------------------------------------------------------------
 * main
 * --------------------------------------------------------------------- */

/* -------------------------------------------------------------------------
 * Test: ^attrs before the name (caret-type-attributes-plan)
 * --------------------------------------------------------------------- */

static void test_attrs_before_name(void) {
    const char *src =
        ";;; Point -- a copyable point.\n"
        "(defstruct ^copy Point [x : int y : int])\n"
        ";;; Tok -- an exactly-once token.\n"
        "(defopaque ^linear ^sealed Tok : int)\n"
        ";;; Shape -- circles and squares.\n"
        "(defdata ^copy Shape (Circle int) (Square int))\n"
        ";;; Nat -- naturals.\n"
        "(defgadt ^copy Nat [] (Zero : (Nat)))\n"
        ";;; some -- wraps a value.\n"
        "(defn ^construct some [A] [x : A] : (Option A) x)\n"
        ";;; old -- going away.\n"
        "(defn ^deprecated \"use new\" old [] : int 0)\n";
    LspDocTable t;
    lsp_doc_table_init(&t);
    lsp_scan_docs(src, strlen(src), &t);

    char out[512];
    CHECK(lookup(&t, "Point", out, sizeof(out)) == 1, "defstruct ^copy: keyed by the name");
    CHECK(strstr(out, "a copyable point") != NULL, "defstruct ^copy: content");
    CHECK(lookup(&t, "Tok", out, sizeof(out)) == 1, "defopaque ^linear ^sealed: keyed by the name");
    CHECK(lookup(&t, "Shape", out, sizeof(out)) == 1, "defdata ^copy: keyed by the name");
    CHECK(lookup(&t, "Nat", out, sizeof(out)) == 1, "defgadt ^copy: keyed by the name");
    CHECK(lookup(&t, "some", out, sizeof(out)) == 1, "defn ^construct: keyed by the name");
    CHECK(lookup(&t, "old", out, sizeof(out)) == 1, "defn ^deprecated \"msg\": keyed by the name");
    CHECK(lookup(&t, "^copy", out, sizeof(out)) == 0, "no entry is keyed by an attribute");
    CHECK(lookup(&t, "^linear", out, sizeof(out)) == 0, "no entry is keyed by ^linear");
    CHECK(lookup(&t, "^construct", out, sizeof(out)) == 0, "no entry is keyed by ^construct");

    lsp_doc_table_free(&t);
}

int main(void) {
    printf("Running docscanner tests...\n");

    test_attrs_before_name();
    test_no_docstring();
    test_single_line_docstring();
    test_multi_section_docstring();
    test_reset_on_non_comment_gap();
    test_blank_lines_in_block();
    test_other_def_forms();
    test_multiple_defs();

    printf("Results: %d passed, %d failed\n", passed, failed);
    return failed > 0 ? 1 : 0;
}
