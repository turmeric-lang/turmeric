#include "lsp_docs.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdint.h>

/* -------------------------------------------------------------------------
 * LspDocTable -- docstring re-scanner
 * --------------------------------------------------------------------- */

void lsp_doc_table_init(LspDocTable *t) {
    t->entries = NULL;
    t->count   = 0;
    t->cap     = 0;
}

void lsp_doc_table_free(LspDocTable *t) {
    free(t->entries);
    t->entries = NULL;
    t->count = t->cap = 0;
}

/* Append one (name, doc) pair to the table, growing if needed. */
static void table_push(LspDocTable *t, const char *name, size_t nlen,
                       const char *doc, size_t dlen) {
    if (t->count >= t->cap) {
        t->cap = t->cap ? t->cap * 2 : 256;
        t->entries = realloc(t->entries, (size_t)t->cap * sizeof(LspDocEntry));
    }
    LspDocEntry *e = &t->entries[t->count++];
    size_t n = nlen < sizeof(e->name) - 1 ? nlen : sizeof(e->name) - 1;
    memcpy(e->name, name, n);
    e->name[n] = '\0';
    size_t d = dlen < sizeof(e->doc) - 1 ? dlen : sizeof(e->doc) - 1;
    memcpy(e->doc, doc, d);
    e->doc[d] = '\0';
}

static int is_blank_line(const char *p, size_t len) {
    for (size_t i = 0; i < len; i++)
        if (p[i] != ' ' && p[i] != '\t' && p[i] != '\r') return 0;
    return 1;
}

void lsp_scan_docs(const char *text, size_t len, LspDocTable *out) {
    char   doc_buf[512];
    size_t doc_len = 0;
    doc_buf[0] = '\0';

    const char *p   = text;
    const char *end = text + len;

    while (p < end) {
        /* Find end of line */
        const char *line_start = p;
        while (p < end && *p != '\n') p++;
        size_t raw_len = (size_t)(p - line_start);
        if (p < end) p++; /* skip '\n' */

        /* Trim trailing \r */
        const char *lend = line_start + raw_len;
        while (lend > line_start && *(lend - 1) == '\r') lend--;
        size_t line_len = (size_t)(lend - line_start);

        /* Skip leading whitespace */
        const char *lp = line_start;
        while (lp < lend && (*lp == ' ' || *lp == '\t')) lp++;
        size_t trimmed = (size_t)(lend - lp);

        /* Blank line: don't reset doc_buf */
        if (is_blank_line(line_start, line_len)) continue;

        /* ;;; comment line: accumulate into doc_buf */
        if (trimmed >= 3 && lp[0] == ';' && lp[1] == ';' && lp[2] == ';') {
            const char *comment = lp + 3;
            if (comment < lend && *comment == ' ') comment++;
            size_t clen = (size_t)(lend - comment);
            if (doc_len > 0 && doc_len < sizeof(doc_buf) - 1)
                doc_buf[doc_len++] = '\n';
            if (doc_len + clen < sizeof(doc_buf)) {
                memcpy(doc_buf + doc_len, comment, clen);
                doc_len += clen;
                doc_buf[doc_len] = '\0';
            }
            continue;
        }

        /* Check for a definition form: (defn|defmacro|defstruct|definstance NAME */
        if (trimmed >= 2 && lp[0] == '(') {
            const char *kw = lp + 1;
            while (kw < lend && (*kw == ' ' || *kw == '\t')) kw++;

            static const struct { const char *kw; size_t klen; } defs[] = {
                { "defn",        4 },
                { "defmacro",    8 },
                { "defstruct",   9 },
                { "definstance", 11 },
                { "def",         3 },
                { "defdata",     7 },
                { "defgadt",     7 },
                { "defopaque",   9 },
                { NULL, 0 },
            };

            const char *name_start = NULL;
            for (int di = 0; defs[di].kw; di++) {
                size_t kl = defs[di].klen;
                if ((size_t)(lend - kw) >= kl &&
                    memcmp(kw, defs[di].kw, kl) == 0 &&
                    (kw + kl >= lend || kw[kl] == ' ' || kw[kl] == '\t')) {
                    name_start = kw + kl;
                    break;
                }
            }

            if (name_start) {
                /* Step over the `^attr`s that precede the name --
                 * `(defstruct ^copy Point ...)`, `(defn ^construct some ...)` --
                 * and the message a `^deprecated` takes.  Without this the
                 * entry was keyed `^copy`. */
                for (;;) {
                    while (name_start < lend && (*name_start == ' ' || *name_start == '\t'))
                        name_start++;
                    if (name_start >= lend || *name_start != '^') break;
                    bool deprecated = (size_t)(lend - name_start) >= 11 &&
                                      memcmp(name_start, "^deprecated", 11) == 0;
                    while (name_start < lend && *name_start != ' ' && *name_start != '\t')
                        name_start++;
                    if (!deprecated) continue;
                    while (name_start < lend && (*name_start == ' ' || *name_start == '\t'))
                        name_start++;
                    if (name_start < lend && *name_start == '"') {
                        name_start++;
                        while (name_start < lend && *name_start != '"') name_start++;
                        if (name_start < lend) name_start++;
                    }
                }
                const char *name_end = name_start;
                while (name_end < lend && *name_end != ' ' && *name_end != '\t' &&
                       *name_end != ')' && *name_end != '[')
                    name_end++;
                size_t nlen = (size_t)(name_end - name_start);
                if (nlen > 0 && nlen < sizeof(((LspDocEntry *)0)->name)) {
                    if (doc_len > 0)
                        table_push(out, name_start, nlen, doc_buf, doc_len);
                }
                /* Definition found: always reset doc_buf */
                doc_buf[0] = '\0';
                doc_len = 0;
                continue;
            }
        }

        /* Non-blank, non-;;; line that is not a definition: reset */
        doc_buf[0] = '\0';
        doc_len = 0;
    }
}

int lsp_scan_docs_lookup(const LspDocTable *t, const char *name,
                         char *out, size_t cap) {
    for (int i = 0; i < t->count; i++) {
        if (strcmp(t->entries[i].name, name) == 0) {
            size_t n = strlen(t->entries[i].doc);
            if (n >= cap) n = cap - 1;
            memcpy(out, t->entries[i].doc, n);
            out[n] = '\0';
            return 1;
        }
    }
    if (cap > 0) out[0] = '\0';
    return 0;
}

/* -------------------------------------------------------------------------
 * Open-addressing hash map: URI string -> LspDoc*
 * --------------------------------------------------------------------- */

#define DOCS_INIT_CAP 16

typedef struct DocsSlot {
    LspDoc *doc;  /* NULL = empty */
} DocsSlot;

static DocsSlot *slots_    = NULL;
static size_t    capacity_ = 0;
static size_t    count_    = 0;

/* FNV-1a 32-bit */
static uint32_t fnv1a(const char *s, size_t len) {
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < len; i++) {
        h ^= (uint8_t)s[i];
        h *= 16777619u;
    }
    return h;
}

static void rehash(void) {
    size_t new_cap = capacity_ ? capacity_ * 2 : DOCS_INIT_CAP;
    DocsSlot *new_slots = calloc(new_cap, sizeof(DocsSlot));
    for (size_t i = 0; i < capacity_; i++) {
        if (!slots_[i].doc) continue;
        LspDoc *d = slots_[i].doc;
        uint32_t h = fnv1a(d->uri, strlen(d->uri)) % (uint32_t)new_cap;
        while (new_slots[h].doc) h = (h + 1) % (uint32_t)new_cap;
        new_slots[h].doc = d;
    }
    free(slots_);
    slots_ = new_slots;
    capacity_ = new_cap;
}

static size_t find_slot(const char *uri, size_t uri_len) {
    if (!capacity_) return (size_t)-1;
    uint32_t h = fnv1a(uri, uri_len) % (uint32_t)capacity_;
    for (size_t probe = 0; probe < capacity_; probe++) {
        size_t idx = (h + probe) % capacity_;
        if (!slots_[idx].doc) return (size_t)-1;
        if (strncmp(slots_[idx].doc->uri, uri, uri_len) == 0 &&
            slots_[idx].doc->uri[uri_len] == '\0')
            return idx;
    }
    return (size_t)-1;
}

void lsp_docs_init(void) {
    slots_    = NULL;
    capacity_ = 0;
    count_    = 0;
}

void lsp_docs_free(void) {
    for (size_t i = 0; i < capacity_; i++) {
        if (!slots_[i].doc) continue;
        LspDoc *d = slots_[i].doc;
        free(d->uri);
        free(d->path);
        free(d->text);
        free(d->symbols);
        free(d->bindings);
        free(d);
    }
    free(slots_);
    slots_ = NULL;
    capacity_ = count_ = 0;
}

/* `file:` URI -> filesystem path.
 *
 * Accepts both spellings a Windows client might send: the standard
 * file:///C:/dir/x.tur, whose path component begins with a slash BEFORE the
 * drive letter, and the file://C:/dir/x.tur some tooling emits.  Stripping a
 * bare "file://" and stopping there left "/C:/dir/x.tur", which no Windows API
 * will open -- so a correctly-formed request from a conforming editor referred
 * to a file the server could not read.
 *
 * POSIX is untouched: there is no drive letter, so the leading slash stays. */
char *lsp_uri_to_path(const char *uri, char *dest, size_t dest_cap) {
    const char *src = uri;
    if (strncmp(src, "file://", 7) == 0) src += 7;
    /* "/C:/..." -> "C:/..." */
    if (src[0] == '/' &&
        ((src[1] >= 'A' && src[1] <= 'Z') || (src[1] >= 'a' && src[1] <= 'z')) &&
        (src[2] == ':' || (src[2] == '%' && src[3] == '3' &&
                            (src[4] == 'A' || src[4] == 'a'))))
        src += 1;
    size_t di = 0;
    for (; *src && di < dest_cap - 1; src++) {
        if (*src == '%' && src[1] && src[2]) {
            char hex[3] = { src[1], src[2], '\0' };
            dest[di++] = (char)strtol(hex, NULL, 16);
            src += 2;
        } else {
            dest[di++] = *src;
        }
    }
    dest[di] = '\0';
    return dest;
}

void lsp_doc_free_symbols(LspDoc *doc) {
    if (!doc) return;
    free(doc->symbols);
    doc->symbols       = NULL;
    doc->symbol_count  = 0;
    doc->symbol_cap    = 0;
    doc->symbols_stale = 0;
    /* ever_analyzed is deliberately NOT cleared: this is called to swap one
     * index for another, and the fact that an analysis once succeeded is a
     * property of the document, not of the array being replaced. */
}

void lsp_doc_free_bindings(LspDoc *doc) {
    if (!doc) return;
    free(doc->bindings);
    doc->bindings           = NULL;
    doc->binding_count      = 0;
    doc->binding_cap        = 0;
    doc->bindings_truncated = 0;
}

static LspDoc *make_doc(const char *uri, size_t uri_len,
                        const char *text, size_t text_len) {
    LspDoc *d = calloc(1, sizeof(LspDoc));
    d->uri  = malloc(uri_len + 1);
    memcpy(d->uri, uri, uri_len);
    d->uri[uri_len] = '\0';

    d->path = malloc(uri_len + 1);
    lsp_uri_to_path(d->uri, d->path, uri_len + 1);

    d->text = malloc(text_len + 1);
    memcpy(d->text, text, text_len);
    d->text[text_len] = '\0';
    d->text_len = text_len;
    return d;
}

LspDoc *lsp_doc_open(const char *uri, size_t uri_len,
                     const char *text, size_t text_len) {
    size_t existing = find_slot(uri, uri_len);
    if (existing != (size_t)-1) {
        LspDoc *d = slots_[existing].doc;
        free(d->text);
        d->text = malloc(text_len + 1);
        memcpy(d->text, text, text_len);
        d->text[text_len] = '\0';
        d->text_len = text_len;
        d->dirty = 1;
        return d;
    }

    if (!capacity_ || (count_ + 1) * 4 >= capacity_ * 3)
        rehash();

    LspDoc *d = make_doc(uri, uri_len, text, text_len);
    d->dirty = 1;
    uint32_t h = fnv1a(uri, uri_len) % (uint32_t)capacity_;
    while (slots_[h].doc) h = (h + 1) % (uint32_t)capacity_;
    slots_[h].doc = d;
    count_++;
    return d;
}

void lsp_doc_change(const char *uri, size_t uri_len,
                    const char *text, size_t text_len) {
    size_t idx = find_slot(uri, uri_len);
    if (idx == (size_t)-1) {
        lsp_doc_open(uri, uri_len, text, text_len);
        return;
    }
    LspDoc *d = slots_[idx].doc;
    free(d->text);
    d->text = malloc(text_len + 1);
    memcpy(d->text, text, text_len);
    d->text[text_len] = '\0';
    d->text_len = text_len;
    d->dirty = 1;
}

void lsp_doc_close(const char *uri, size_t uri_len) {
    size_t idx = find_slot(uri, uri_len);
    if (idx == (size_t)-1) return;
    LspDoc *d = slots_[idx].doc;
    lsp_doc_free_symbols(d);
    lsp_doc_free_bindings(d);
    free(d->uri); free(d->path); free(d->text); free(d);
    slots_[idx].doc = NULL;
    count_--;
}

LspDoc *lsp_doc_get(const char *uri, size_t uri_len) {
    size_t idx = find_slot(uri, uri_len);
    return idx != (size_t)-1 ? slots_[idx].doc : NULL;
}

void lsp_docs_iterate(void (*cb)(const LspDoc *doc, void *ctx), void *ctx) {
    for (size_t i = 0; i < capacity_; i++) {
        if (slots_[i].doc)
            cb(slots_[i].doc, ctx);
    }
}

void lsp_docs_iterate_mut(void (*cb)(LspDoc *doc, void *ctx), void *ctx) {
    for (size_t i = 0; i < capacity_; i++) {
        if (slots_[i].doc)
            cb(slots_[i].doc, ctx);
    }
}

int lsp_docs_any_dirty(void) {
    for (size_t i = 0; i < capacity_; i++) {
        if (slots_[i].doc && slots_[i].doc->dirty) return 1;
    }
    return 0;
}
