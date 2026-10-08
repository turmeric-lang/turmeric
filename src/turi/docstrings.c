/* docstrings.c -- read stdlib/docstrings.tur's doc table directly from C.
 * See docstrings.h.
 *
 * The file's shape is a contract we own on both sides: tools/gendocs.py's
 * emit_docstrings_tur writes
 *
 *     static const struct { const char *key; const char *val; } entries[] = {
 *       {"name", "docstring with \n escapes"},
 *       ...
 *     };
 *
 * so the scanner below only has to understand C string literals -- it does not
 * care about line layout, ordering, or how many entries there are.  It stops at
 * the end of that array, which keeps the separate doc-verified? table (a flat
 * list of bare names) from being mistaken for entries.
 *
 * The generated file is ~500 KB, so it is parsed once into a sorted table
 * rather than rescanned per lookup: the playground doc panel looks a name up
 * on every hover.
 */
#include "turi/docstrings.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    char *key;
    char *val;
} DocEntry;

static char     *g_path    = NULL;   /* the file the cache below was read from */
static DocEntry *g_entries = NULL;
static size_t    g_n       = 0;

/* Read one C string literal starting at `p` (which must point at the opening
 * quote), unescaping into `out`.  Returns a pointer just past the closing
 * quote, or NULL if the literal is unterminated.  `out` may be NULL to scan
 * without copying; `cap` then does not matter. */
static const char *scan_c_string(const char *p, char *out, size_t cap) {
    if (*p != '"') return NULL;
    p++;
    size_t i = 0;
    while (*p && *p != '"') {
        char c = *p++;
        if (c == '\\' && *p) {
            char e = *p++;
            switch (e) {
                case 'n':  c = '\n'; break;
                case 't':  c = '\t'; break;
                case 'r':  c = '\r'; break;
                case '0':  c = '\0'; break;
                case '\\': c = '\\'; break;
                case '"':  c = '"';  break;
                default:   c = e;    break;
            }
        }
        if (out && i + 1 < cap) out[i++] = c;
    }
    if (*p != '"') return NULL;
    if (out && cap) out[i] = '\0';
    return p + 1;
}

/* Copy the literal at `p` (ending just before `end`) into a fresh string. */
static char *dup_c_string(const char *p, const char *end) {
    size_t room = (size_t)(end - p) + 1;   /* unescaping only shrinks */
    char *s = (char *)malloc(room);
    if (s) scan_c_string(p, s, room);
    return s;
}

static char *read_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    long size = ftell(f);
    if (size < 0 || fseek(f, 0, SEEK_SET) != 0) { fclose(f); return NULL; }
    char *buf = (char *)malloc((size_t)size + 1);
    if (!buf) { fclose(f); return NULL; }
    size_t n = fread(buf, 1, (size_t)size, f);
    fclose(f);
    if (n != (size_t)size) { free(buf); return NULL; }
    buf[size] = '\0';
    return buf;
}

static int entry_cmp(const void *a, const void *b) {
    return strcmp(((const DocEntry *)a)->key, ((const DocEntry *)b)->key);
}

static void cache_clear(void) {
    for (size_t i = 0; i < g_n; i++) {
        free(g_entries[i].key);
        free(g_entries[i].val);
    }
    free(g_entries);
    free(g_path);
    g_entries = NULL;
    g_n       = 0;
    g_path    = NULL;
}

/* Parse `path` into the cache.  A missing or malformed file leaves an EMPTY
 * cache for that path, so a lookup against an absent stdlib is a cheap miss
 * rather than a re-read per call. */
static void cache_load(const char *path) {
    cache_clear();
    g_path = strdup(path);

    char *text = read_file(path);
    if (!text) return;

    const char *p = strstr(text, "entries[] = {");
    if (!p) { free(text); return; }
    p += strlen("entries[] = {");

    size_t cap = 0;

#define SKIP_WS_COMMA(q) \
    while (*(q) == ' ' || *(q) == '\t' || *(q) == '\n' || \
           *(q) == '\r' || *(q) == ',') (q)++

    for (;;) {
        SKIP_WS_COMMA(p);
        /* The array's own closing brace ends the table.  Each entry also ends
         * in `}`, which is consumed at the bottom of the loop -- leaving it
         * there would stop the scan after the first entry. */
        if (*p != '{') break;
        p++;
        SKIP_WS_COMMA(p);

        const char *key_start = p;
        const char *after_key = scan_c_string(p, NULL, 0);
        if (!after_key) break;
        p = after_key;
        SKIP_WS_COMMA(p);
        if (*p != '"') break;                 /* {NULL, NULL} sentinel, or malformed */

        const char *val_start = p;
        const char *after_val = scan_c_string(p, NULL, 0);
        if (!after_val) break;

        if (g_n == cap) {
            size_t ncap = cap ? cap * 2 : 1024;
            DocEntry *grown = (DocEntry *)realloc(g_entries, ncap * sizeof(DocEntry));
            if (!grown) break;
            g_entries = grown;
            cap = ncap;
        }
        g_entries[g_n].key = dup_c_string(key_start, after_key);
        g_entries[g_n].val = dup_c_string(val_start, after_val);
        if (!g_entries[g_n].key || !g_entries[g_n].val) {
            free(g_entries[g_n].key);
            free(g_entries[g_n].val);
            break;
        }
        g_n++;

        p = after_val;
        SKIP_WS_COMMA(p);
        if (*p == '}') p++;                   /* close this entry */
    }
#undef SKIP_WS_COMMA

    free(text);
    /* gendocs dedups keys (first definition wins), so a plain sort is enough
     * for bsearch to find the one entry per key. */
    if (g_n > 1) qsort(g_entries, g_n, sizeof(DocEntry), entry_cmp);
}

const char *tur_docstring_lookup_in(const char *path, const char *name) {
    if (!path || !name) return NULL;
    if (!g_path || strcmp(g_path, path) != 0) cache_load(path);
    if (g_n == 0) return NULL;
    DocEntry probe = { (char *)name, NULL };
    DocEntry *hit = (DocEntry *)bsearch(&probe, g_entries, g_n,
                                        sizeof(DocEntry), entry_cmp);
    return hit ? hit->val : NULL;
}

const char *tur_docstring_lookup(const char *name) {
    const char *sdir = getenv("TUR_STDLIB_DIR");
    if (!sdir || !*sdir) sdir = "stdlib";
    char path[4096];
    int n = snprintf(path, sizeof path, "%s/docstrings.tur", sdir);
    if (n < 0 || (size_t)n >= sizeof path) return NULL;
    return tur_docstring_lookup_in(path, name);
}

/* The builtin table lives here, not in repl.c, because the interpreter's
 * `doc-print` native uses it: anything interpreter_natives.c references is
 * linked into every libturi embedder, and repl.c would drag readline in with
 * it. */
const char *turi_doc_lookup_builtin(const char *sym) {
    static const struct { const char *name; const char *doc; } docs[] = {
        /* Arithmetic */
        {"+",        "(+ a b ...) -- add numbers"},
        {"-",        "(- a b ...) -- subtract numbers"},
        {"*",        "(* a b ...) -- multiply numbers"},
        {"/",        "(/ a b) -- divide numbers"},
        {"mod",      "(mod a b) -- integer remainder"},
        /* Comparison */
        {"=",        "(= a b) -- equality"},
        {"!=",       "(!= a b) -- inequality"},
        {"<",        "(< a b) -- less-than"},
        {">",        "(> a b) -- greater-than"},
        {"<=",       "(<= a b) -- less-than-or-equal"},
        {">=",       "(>= a b) -- greater-than-or-equal"},
        /* Logic */
        {"not",      "(not b) -- boolean negation"},
        {"and",      "(and a b ...) -- short-circuit logical and"},
        {"or",       "(or a b ...) -- short-circuit logical or"},
        /* I/O */
        {"println",  "(println x) -- print value with trailing newline; #fx{IO}"},
        {"print",    "(print x) -- print value without trailing newline"},
        {"eprintln", "(eprintln x) -- print value to stderr with trailing newline; #fx{IO}"},
        {"eprint",   "(eprint x) -- print value to stderr without trailing newline; #fx{IO}"},
        /* Core special forms */
        {"let",      "(let [x v ...] body) -- bind local variables in scope of body"},
        {"if",       "(if cond then else) -- conditional: evaluates then or else branch"},
        {"do",       "(do e1 e2 ...) -- evaluate expressions in sequence, return last"},
        {"defn",     "(defn name [p1 :T1 ...] :Ret body) -- define a named function"},
        {"fn",       "(fn [p1 :T1 ...] :Ret body) -- anonymous function (lambda)"},
        {"def",      "(def name [: type] value) -- bind name; a top-level binding at the top level, scoped over the rest of the body inside one"},
        {"define",   "(define name [: type] value) -- a spelling of def; same meaning in both positions"},
        {"while",    "(while cond body ...) -- loop while cond is true"},
        {"set!",     "(set! var value) -- mutate an existing variable binding"},
        {"quote",    "(quote x) -- return x unevaluated; shorthand: 'x"},
        {"return",   "(return value) -- early return from a function"},
        {"defer",    "(defer body ...) -- run body when current scope exits"},
        {"^tailcall","(^tailcall (f x)) -- assert this call is in tail position; TUR-E0716 with the reason if it is not. Prefix spelling: ^tailcall (f x)"},
        /* Pattern matching and data */
        {"match",    "(match val (Pattern body) ...) -- destructure and branch on value"},
        {"defstruct","(defstruct Name [field :Type ...]) -- define a named product type"},
        {"defdata",  "(defdata Name (Ctor) (Ctor :T) ...) -- define an algebraic data type"},
        {"defgadt",  "(defgadt Name [a] (Ctor :T) ...) -- define a generalized ADT"},
        {"deftype",  "(deftype Alias ActualType) -- define a type alias"},
        /* Macros */
        {"defmacro", "(defmacro name [args] body) -- define a syntax macro"},
        {"when",     "(when cond body ...) -- execute body if cond is true, else nil"},
        {"unless",   "(unless cond body ...) -- execute body if cond is false, else nil"},
        {"cond",     "(cond test expr ... else expr) -- multi-branch conditional; the fallback clause is `else` or `:else`"},
        {"for",      "(for [x seq] body) -- iterate over a sequence"},
        /* Modules */
        {"defmodule","(defmodule Name (export ...) body ...) -- define a module"},
        {"import",   "(import module/name :as alias) -- import a module (inside defmodule)"},
        /* Typeclasses */
        {"defclass", "(defclass Name [param] (method :Type) ...) -- define a typeclass"},
        {"definstance","(definstance ClassName TypeName method-impls ...) -- implement a typeclass"},
        /* Effects */
        {"defeffect","(defeffect Name (op :Type) ...) -- define an algebraic effect"},
        {"perform",  "(perform effect/op args ...) -- perform an effect operation"},
        {"handle",   "(handle expr (effect/op args k) body ...) -- handle effects"},
        {"resume",   "(resume k value) -- resume a delimited continuation"},
        /* Async */
        {"async",    "(async body) -- create an async computation"},
        {"await",    "(await future) -- wait for an async computation to complete"},
        /* Error handling.  try/catch/throw were deleted end-to-end in v0.25.0
         * (CHANGELOG.md:1974); the model is Result-returning functions plus
         * panic for the unrecoverable case.  Do not re-add them here. */
        {"panic",    "(panic msg) -- abort with an unrecoverable error"},
        {"panic-with","(panic-with value) -- panic carrying a typed payload"},
        {"catch-unwind","(catch-unwind thunk) -- run thunk, returning a Result whose err slot carries the Panic"},
        {"catch-panic-of","(catch-panic-of Type thunk) -- like catch-unwind, but re-raises panics whose payload is not Type"},
        /* Dynamic vars */
        {"defdynamic","(defdynamic *name* :Type init) -- define a dynamic (thread-local) variable"},
        {"let-dyn",  "(let-dyn [*name* val] body) -- bind dynamic variable within scope"},
        {NULL, NULL}
    };
    for (int i = 0; docs[i].name; i++) {
        if (strcmp(sym, docs[i].name) == 0)
            return docs[i].doc;
    }
    return NULL;
}
