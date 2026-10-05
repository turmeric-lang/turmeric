/* lsp_lite.c -- minimal completion/calltip/doc helper for Turmeric editors.
 *
 * Protocol: newline-delimited JSON over stdio. One request per line, one
 * response per line. Pretty-printing / Content-Length framing intentionally
 * avoided -- consumers read byte streams with line buffering, and the
 * lightweight framing keeps client code trivial.
 *
 * Request shape:
 *   {"id":<int>,"method":"<name>","params":{...}}
 *
 * Methods:
 *   "complete" {"prefix":"co","uri":"file://..."}  -- prefix match against
 *       stdlib docstrings + per-URI scanned defns. Result: array of strings.
 *   "calltip"  {"name":"cons"}                    -- first non-empty line of
 *       the doc for `name`. Result: string ("" if unknown).
 *   "doc"      {"name":"cons"}                    -- full doc text.
 *   "update"   {"uri":"file://...","text":"..."} -- replace per-URI text
 *       buffer and re-scan its ;;; docstrings. Result: "ok".
 *   "shutdown" {}                                  -- exit 0.
 *
 * Response shape:
 *   {"id":<int>,"result":<value>}
 *   {"id":<int>,"error":"<msg>"}
 *
 * The "lite" in the name is load-bearing: this is not a full LSP server.
 * It is a one-process completion/doc backend that the editor spawns once
 * per session and talks to via stdin/stdout.
 */

#include "lsp_lite.h"

#include "../lsp/lsp_docs.h"
#include "../lsp/lsp_json.h"
#include "../runtime/buf.h"
#include "../turi/repl.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* ------------------------------------------------------------------------- */

typedef struct {
    char *name;   /* heap, NUL-terminated */
    char *doc;    /* heap, NUL-terminated */
} LiteEntry;

typedef struct {
    LiteEntry *items;
    int        count;
    int        cap;
} LiteTable;

static void lite_table_push(LiteTable *t, const char *name, size_t nlen,
                            const char *doc, size_t dlen) {
    if (t->count == t->cap) {
        t->cap = t->cap ? t->cap * 2 : 256;
        t->items = (LiteEntry *)realloc(t->items, (size_t)t->cap * sizeof *t->items);
    }
    LiteEntry *e = &t->items[t->count++];
    e->name = (char *)malloc(nlen + 1);
    memcpy(e->name, name, nlen);
    e->name[nlen] = '\0';
    e->doc = (char *)malloc(dlen + 1);
    memcpy(e->doc, doc, dlen);
    e->doc[dlen] = '\0';
}

static void lite_table_free(LiteTable *t) {
    for (int i = 0; i < t->count; i++) { free(t->items[i].name); free(t->items[i].doc); }
    free(t->items);
    t->items = NULL;
    t->count = t->cap = 0;
}

/* ---------------------------------------------------------------------------
 * Parse stdlib/docstrings.tur entries on startup.
 *
 * Each row in the generated table looks like:
 *   {"<name>", "<doc with \\n escapes>"},
 * Parse with a simple state machine -- avoids pulling in a full reader.
 * --------------------------------------------------------------------------- */

/* Unescape a C-string literal body (between the surrounding quotes) into dest.
 * Handles \", \\, \n, \r, \t, \xHH and ignores other escapes. */
static size_t unescape(const char *src, size_t len, char *dest, size_t cap) {
    size_t o = 0;
    for (size_t i = 0; i < len && o + 1 < cap; i++) {
        char c = src[i];
        if (c == '\\' && i + 1 < len) {
            char n = src[++i];
            switch (n) {
            case 'n':  dest[o++] = '\n'; break;
            case 'r':  dest[o++] = '\r'; break;
            case 't':  dest[o++] = '\t'; break;
            case '"':  dest[o++] = '"';  break;
            case '\\': dest[o++] = '\\'; break;
            default:   dest[o++] = n;    break;
            }
        } else {
            dest[o++] = c;
        }
    }
    dest[o] = '\0';
    return o;
}

/* Scan a C-string literal starting at *p (which points at the opening quote).
 * Advances *p past the closing quote. Fills out [start, end) of the body.
 * Returns 1 on success, 0 if the literal is malformed. */
static int scan_cstr(const char **p, const char *end,
                     const char **body_start, size_t *body_len) {
    if (*p >= end || **p != '"') return 0;
    (*p)++;
    const char *s = *p;
    while (*p < end && **p != '"') {
        if (**p == '\\' && *p + 1 < end) (*p)++;
        (*p)++;
    }
    if (*p >= end) return 0;
    *body_start = s;
    *body_len = (size_t)(*p - s);
    (*p)++;
    return 1;
}

static void load_stdlib_docstrings(LiteTable *out, const char *path) {
    /* "rb": a text-mode read on Windows is short of the ftell size, and the
     * strict check below then drops the whole table without a word. See
     * docs/archive/windows-text-mode-read-rejects-own-files.md */
    FILE *fp = fopen(path, "rb");
    if (!fp) return;
    fseek(fp, 0, SEEK_END);
    long sz = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    if (sz <= 0) { fclose(fp); return; }
    char *buf = (char *)malloc((size_t)sz + 1);
    if (!buf) { fclose(fp); return; }
    if (fread(buf, 1, (size_t)sz, fp) != (size_t)sz) { free(buf); fclose(fp); return; }
    buf[sz] = '\0';
    fclose(fp);

    const char *p = buf, *end = buf + sz;
    /* Look for `    {"key", "val"},` rows -- skip the closing `{NULL, NULL}`. */
    while (p < end) {
        while (p < end && *p != '{') p++;
        if (p >= end) break;
        p++;  /* past `{` */
        while (p < end && isspace((unsigned char)*p)) p++;
        if (p >= end || *p != '"') continue;
        const char *ns; size_t nl;
        if (!scan_cstr(&p, end, &ns, &nl)) continue;
        while (p < end && (isspace((unsigned char)*p) || *p == ',')) p++;
        if (p >= end || *p != '"') continue;
        const char *vs; size_t vl;
        if (!scan_cstr(&p, end, &vs, &vl)) continue;
        if (nl == 0 || nl > 200 || vl == 0) continue;
        /* Decode the escape sequences before storing. */
        char nbuf[256];
        size_t ndec = unescape(ns, nl, nbuf, sizeof nbuf);
        char *dbuf = (char *)malloc(vl + 1);
        if (!dbuf) continue;
        size_t ddec = unescape(vs, vl, dbuf, vl + 1);
        lite_table_push(out, nbuf, ndec, dbuf, ddec);
        free(dbuf);
    }

    free(buf);
}

/* The stdlib root is exposed by the main binary via TUR_STDLIB_DIR (set by
 * resolve_stdlib_root in src/main.c before any subcommand runs). Use the env
 * var to stay decoupled from main.c's static state. */
static void resolve_docstrings_path(char *out, size_t cap) {
    const char *root = getenv("TUR_STDLIB_DIR");
    if (root && *root) {
        snprintf(out, cap, "%s/docstrings.tur", root);
        return;
    }
    snprintf(out, cap, "stdlib/docstrings.tur");
}

/* ---------------------------------------------------------------------------
 * Per-URI scanned doc table -- one slot keyed on the most recently updated
 * URI. The editor only cares about the active buffer; multi-URI tracking is
 * future work.
 * --------------------------------------------------------------------------- */

static LspDocTable g_buf_docs;
static char        g_buf_uri[1024];

static void rescan_buffer(const char *text, size_t len, const char *uri) {
    lsp_doc_table_free(&g_buf_docs);
    lsp_doc_table_init(&g_buf_docs);
    lsp_scan_docs(text, len, &g_buf_docs);
    snprintf(g_buf_uri, sizeof g_buf_uri, "%s", uri ? uri : "");
}

/* ---------------------------------------------------------------------------
 * JSON helpers
 * --------------------------------------------------------------------------- */

static void emit_escaped(Buf *b, const char *s, size_t n) {
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        switch (c) {
        case '"':  buf_puts(b, "\\\""); break;
        case '\\': buf_puts(b, "\\\\"); break;
        case '\n': buf_puts(b, "\\n");  break;
        case '\r': buf_puts(b, "\\r");  break;
        case '\t': buf_puts(b, "\\t");  break;
        default:
            if (c < 0x20) buf_printf(b, "\\u%04x", c);
            else          buf_putc(b, (char)c);
        }
    }
}

static void emit_string(Buf *b, const char *s) {
    buf_putc(b, '"');
    if (s) emit_escaped(b, s, strlen(s));
    buf_putc(b, '"');
}

static void write_line(int out_fd, Buf *b) {
    buf_putc(b, '\n');
    ssize_t n = write(out_fd, b->data, b->len);
    (void)n;
}

static void respond_str(int out_fd, int64_t id, const char *result) {
    Buf b; buf_init(&b);
    buf_printf(&b, "{\"id\":%lld,\"result\":", (long long)id);
    emit_string(&b, result);
    buf_putc(&b, '}');
    write_line(out_fd, &b);
    buf_free(&b);
}

static void respond_error(int out_fd, int64_t id, const char *msg) {
    Buf b; buf_init(&b);
    buf_printf(&b, "{\"id\":%lld,\"error\":", (long long)id);
    emit_string(&b, msg);
    buf_putc(&b, '}');
    write_line(out_fd, &b);
    buf_free(&b);
}

/* ---------------------------------------------------------------------------
 * Builtin name enumeration -- the REPL keeps its docstrings in a private
 * table (turi_doc_lookup_builtin); since there is no enumeration API, list
 * the names we care about here. This is the same canonical set surfaced by
 * `tur doc` for special forms / built-in operators.
 * --------------------------------------------------------------------------- */

static const char *const BUILTIN_NAMES[] = {
    "defn", "defmacro", "defstruct", "defopaque", "definstance", "defclass",
    "defmodule", "def", "fn", "let", "loop", "recur",
    "if", "when", "unless", "cond", "do", "for", "while",
    "match", "case", "and", "or", "not",
    "import", "export", "extern-c", "extern", "ns",
    "true", "false", "nil", "nil-value",
    "cons", "car", "cdr", "head", "tail",
    "println", "print", "eprintln", "eprint", "format",
    /* Surface spellings only. `builtins.c` maps these to C operators for
     * codegen (`mod` -> `%`, `=` -> `==`, `not=` -> `!=`); completing the C
     * spelling offers the user an operator the compiler rejects. */
    "+", "-", "*", "/", "mod", "<", ">", "<=", ">=", "=", "not=",
    NULL
};

/* ---------------------------------------------------------------------------
 * Request handlers
 * --------------------------------------------------------------------------- */

static const char *find_doc(LiteTable *stdlib_t, const char *name) {
    for (int i = 0; i < stdlib_t->count; i++)
        if (strcmp(stdlib_t->items[i].name, name) == 0)
            return stdlib_t->items[i].doc;
    char tmp[1024];
    if (lsp_scan_docs_lookup(&g_buf_docs, name, tmp, sizeof tmp)) {
        /* Buffer doc is scanned each update; the table owns the storage. */
        for (int i = 0; i < g_buf_docs.count; i++)
            if (strcmp(g_buf_docs.entries[i].name, name) == 0)
                return g_buf_docs.entries[i].doc;
    }
    const char *bi = turi_doc_lookup_builtin(name);
    return bi;
}

static void handle_complete(int out_fd, int64_t id, LiteTable *stdlib_t,
                            const char *prefix, size_t plen) {
    Buf b; buf_init(&b);
    buf_printf(&b, "{\"id\":%lld,\"result\":[", (long long)id);
    int first = 1;

    /* Use an inline "seen" filter to avoid duplicates between the three name
     * sources. ~1500 stdlib entries -- a linear scan per emit is fine. */
    const char **seen = NULL;
    int seen_n = 0, seen_cap = 0;
    int seen_has;
    (void)seen_has;

    /* 1. stdlib */
    for (int i = 0; i < stdlib_t->count; i++) {
        const char *n = stdlib_t->items[i].name;
        if (plen == 0 || strncmp(n, prefix, plen) == 0) {
            seen_has = 0;
            for (int j = 0; j < seen_n; j++) if (strcmp(seen[j], n) == 0) { seen_has = 1; break; }
            if (seen_has) continue;
            if (seen_n == seen_cap) {
                seen_cap = seen_cap ? seen_cap * 2 : 64;
                seen = (const char **)realloc(seen, (size_t)seen_cap * sizeof *seen);
            }
            seen[seen_n++] = n;
            if (!first) buf_putc(&b, ',');
            emit_string(&b, n);
            first = 0;
        }
    }
    /* 2. builtins */
    for (int i = 0; BUILTIN_NAMES[i]; i++) {
        const char *n = BUILTIN_NAMES[i];
        if (plen == 0 || strncmp(n, prefix, plen) == 0) {
            seen_has = 0;
            for (int j = 0; j < seen_n; j++) if (strcmp(seen[j], n) == 0) { seen_has = 1; break; }
            if (seen_has) continue;
            if (seen_n == seen_cap) {
                seen_cap = seen_cap ? seen_cap * 2 : 64;
                seen = (const char **)realloc(seen, (size_t)seen_cap * sizeof *seen);
            }
            seen[seen_n++] = n;
            if (!first) buf_putc(&b, ',');
            emit_string(&b, n);
            first = 0;
        }
    }
    /* 3. per-URI scanned defns */
    for (int i = 0; i < g_buf_docs.count; i++) {
        const char *n = g_buf_docs.entries[i].name;
        if (plen == 0 || strncmp(n, prefix, plen) == 0) {
            seen_has = 0;
            for (int j = 0; j < seen_n; j++) if (strcmp(seen[j], n) == 0) { seen_has = 1; break; }
            if (seen_has) continue;
            if (seen_n == seen_cap) {
                seen_cap = seen_cap ? seen_cap * 2 : 64;
                seen = (const char **)realloc(seen, (size_t)seen_cap * sizeof *seen);
            }
            seen[seen_n++] = n;
            if (!first) buf_putc(&b, ',');
            emit_string(&b, n);
            first = 0;
        }
    }
    free(seen);

    buf_puts(&b, "]}");
    write_line(out_fd, &b);
    buf_free(&b);
}

static void handle_calltip(int out_fd, int64_t id, LiteTable *stdlib_t,
                           const char *name) {
    const char *doc = find_doc(stdlib_t, name);
    if (!doc) { respond_str(out_fd, id, ""); return; }
    /* First non-empty line. */
    const char *nl = strchr(doc, '\n');
    char line[512];
    size_t llen = nl ? (size_t)(nl - doc) : strlen(doc);
    if (llen >= sizeof line) llen = sizeof line - 1;
    memcpy(line, doc, llen);
    line[llen] = '\0';
    respond_str(out_fd, id, line);
}

static void handle_doc(int out_fd, int64_t id, LiteTable *stdlib_t,
                       const char *name) {
    const char *doc = find_doc(stdlib_t, name);
    respond_str(out_fd, id, doc ? doc : "");
}

/* ---------------------------------------------------------------------------
 * Line reader -- newline-delimited input. Grows the buffer in 4KiB chunks
 * to handle long "update" payloads with embedded escape sequences.
 * --------------------------------------------------------------------------- */

static char *read_line(int in_fd, size_t *out_len) {
    size_t cap = 4096, len = 0;
    char *buf = (char *)malloc(cap);
    if (!buf) return NULL;
    for (;;) {
        if (len + 1 >= cap) {
            cap *= 2;
            char *nb = (char *)realloc(buf, cap);
            if (!nb) { free(buf); return NULL; }
            buf = nb;
        }
        char c;
        ssize_t n = read(in_fd, &c, 1);
        if (n <= 0) {
            if (len == 0) { free(buf); return NULL; }
            break;
        }
        if (c == '\n') break;
        buf[len++] = c;
    }
    buf[len] = '\0';
    if (out_len) *out_len = len;
    return buf;
}

/* ---------------------------------------------------------------------------
 * Main loop
 * --------------------------------------------------------------------------- */

int lsp_lite_run(int in_fd, int out_fd) {
    LiteTable stdlib_t = {0};
    char path[4096];
    resolve_docstrings_path(path, sizeof path);
    load_stdlib_docstrings(&stdlib_t, path);

    lsp_doc_table_init(&g_buf_docs);

    for (;;) {
        size_t llen;
        char *line = read_line(in_fd, &llen);
        if (!line) break;
        if (llen == 0) { free(line); continue; }

        int64_t id = lsp_json_int(line, "id");
        size_t mlen;
        const char *m = lsp_json_str(line, "method", &mlen);
        if (!m) { respond_error(out_fd, id, "missing method"); free(line); continue; }

        char method[32];
        size_t cp = mlen < sizeof method - 1 ? mlen : sizeof method - 1;
        memcpy(method, m, cp); method[cp] = '\0';

        if (strcmp(method, "shutdown") == 0) {
            respond_str(out_fd, id, "bye");
            free(line);
            break;
        }
        size_t plen;
        const char *params = lsp_json_raw(line, "params", &plen);
        const char *p = params ? params : "{}";

        if (strcmp(method, "complete") == 0) {
            size_t pflen;
            const char *pf = lsp_json_str(p, "prefix", &pflen);
            char prefix[128] = "";
            if (pf) {
                size_t cc = pflen < sizeof prefix - 1 ? pflen : sizeof prefix - 1;
                memcpy(prefix, pf, cc); prefix[cc] = '\0';
            }
            handle_complete(out_fd, id, &stdlib_t, prefix, strlen(prefix));
        } else if (strcmp(method, "calltip") == 0) {
            char name[256] = "";
            lsp_json_str_copy(p, "name", name, sizeof name);
            handle_calltip(out_fd, id, &stdlib_t, name);
        } else if (strcmp(method, "doc") == 0) {
            char name[256] = "";
            lsp_json_str_copy(p, "name", name, sizeof name);
            handle_doc(out_fd, id, &stdlib_t, name);
        } else if (strcmp(method, "update") == 0) {
            size_t tlen;
            const char *t = lsp_json_str(p, "text", &tlen);
            char uri[1024] = "";
            lsp_json_str_copy(p, "uri", uri, sizeof uri);
            if (t) {
                /* Unescape the JSON string into a temp buffer before scanning,
                 * since lsp_scan_docs wants raw source. */
                char *decoded = (char *)malloc(tlen + 1);
                size_t dl = unescape(t, tlen, decoded, tlen + 1);
                rescan_buffer(decoded, dl, uri);
                free(decoded);
                respond_str(out_fd, id, "ok");
            } else {
                respond_error(out_fd, id, "missing text");
            }
        } else {
            respond_error(out_fd, id, "unknown method");
        }
        free(line);
    }

    lite_table_free(&stdlib_t);
    lsp_doc_table_free(&g_buf_docs);
    return 0;
}
