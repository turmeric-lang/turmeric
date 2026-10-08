#include "lsp_util.h"

#include <string.h>
#include <stdio.h>

static int is_ident_char(char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
           (c >= '0' && c <= '9') ||
           c == '-' || c == '?' || c == '!' || c == '*' ||
           c == '/' || c == '>' || c == '<' || c == '+' ||
           c == '=' || c == '_';
}

int lsp_word_at_pos(const char *text, size_t text_len,
                    int line_1based, int col_1based,
                    char *out_name, size_t name_cap) {
    if (!text || text_len == 0 || !out_name || name_cap == 0) return 0;
    out_name[0] = '\0';

    /* Walk to the start of the requested line */
    const char *p   = text;
    const char *end = text + text_len;
    int cur_line = 1;
    while (p < end && cur_line < line_1based) {
        if (*p == '\n') cur_line++;
        p++;
    }
    if (p >= end) return 0;

    /* Advance to the requested column (1-based) */
    const char *line_start = p;
    int cur_col = 1;
    while (p < end && *p != '\n' && cur_col < col_1based) {
        p++;
        cur_col++;
    }
    if (p >= end || *p == '\n') return 0;

    /* If cursor is not on an identifier character, try scanning right one step
     * (handles "cursor is just before the word" edge case). */
    if (!is_ident_char(*p)) {
        if (p + 1 < end && is_ident_char(p[1]))
            p++;
        else
            return 0;
    }

    /* Scan left to find word start */
    const char *word_start = p;
    while (word_start > line_start && is_ident_char(*(word_start - 1)))
        word_start--;

    /* Scan right to find word end */
    const char *word_end = p;
    while (word_end < end && *word_end != '\n' && is_ident_char(*word_end))
        word_end++;

    size_t len = (size_t)(word_end - word_start);
    if (len == 0 || len >= name_cap) return 0;

    memcpy(out_name, word_start, len);
    out_name[len] = '\0';
    return 1;
}

size_t lsp_offset_at_pos(const char *text, size_t text_len,
                         int line_1based, int col_1based) {
    if (!text || text_len == 0) return 0;
    size_t off = 0;
    int cur_line = 1;
    while (off < text_len && cur_line < line_1based) {
        if (text[off] == '\n') cur_line++;
        off++;
    }
    int cur_col = 1;
    while (off < text_len && text[off] != '\n' && cur_col < col_1based) {
        off++;
        cur_col++;
    }
    return off;
}

size_t lsp_prefix_at_pos(const char *text, size_t text_len,
                         int line_1based, int col_1based,
                         char *out_prefix, size_t cap) {
    if (!out_prefix || cap == 0) return 0;
    out_prefix[0] = '\0';
    if (!text || text_len == 0) return 0;

    size_t off   = lsp_offset_at_pos(text, text_len, line_1based, col_1based);
    size_t start = off;
    while (start > 0 && is_ident_char(text[start - 1])) start--;

    /* An identifier longer than the buffer is pathological; keep the head,
     * since that is the part every prefix comparison actually reads. */
    size_t len = off - start;
    if (len >= cap) len = cap - 1;
    memcpy(out_prefix, text + start, len);
    out_prefix[len] = '\0';
    return len;
}

/* One open `(` on the scan stack. */
typedef struct {
    char   name[128];  /* head symbol, "" until the first form is seen */
    int    forms;      /* forms seen at this level, head included */
    size_t last_end;   /* byte offset just past the most recent form */
} CallFrame;

#define LSP_CALL_DEPTH_MAX 64

int lsp_enclosing_call(const char *text, size_t text_len,
                       size_t cursor_off,
                       char *out_name, size_t name_cap,
                       int *active_param_out) {
    if (out_name && name_cap) out_name[0] = '\0';
    if (active_param_out) *active_param_out = 0;
    if (!text || text_len == 0) return 0;
    if (cursor_off > text_len) cursor_off = text_len;

    CallFrame stack[LSP_CALL_DEPTH_MAX];
    int depth = 0;
    /* Depth beyond the stack is still tracked as a count so the matching
     * closers pop back to a frame we do have, instead of unwinding into
     * someone else's. */
    int overflow = 0;

    size_t i = 0;
    while (i < cursor_off) {
        char c = text[i];

        if (c == ';') {                        /* comment to end of line */
            while (i < cursor_off && text[i] != '\n') i++;
            continue;
        }
        if (c == '"') {                        /* string literal is one form */
            i++;
            while (i < cursor_off && text[i] != '"') {
                if (text[i] == '\\') i++;
                i++;
            }
            if (i < cursor_off) i++;           /* past the closing quote */
            if (depth > 0 && !overflow) {
                stack[depth - 1].forms++;
                stack[depth - 1].last_end = i;
            }
            continue;
        }
        if (c == '(' || c == '[' || c == '{') {
            if (depth > 0 && !overflow) stack[depth - 1].forms++;
            if (c == '(' && depth < LSP_CALL_DEPTH_MAX && !overflow) {
                stack[depth].name[0]  = '\0';
                stack[depth].forms    = 0;
                stack[depth].last_end = i + 1;
                depth++;
            } else {
                /* `[` and `{` are not calls, and a `(` past the depth cap has
                 * nowhere to live -- either way, count the nesting so the
                 * closer pops the right thing. */
                overflow++;
            }
            i++;
            continue;
        }
        if (c == ')' || c == ']' || c == '}') {
            if (overflow > 0) overflow--;
            else if (depth > 0) {
                depth--;
                if (depth > 0) stack[depth - 1].last_end = i + 1;
            }
            i++;
            continue;
        }
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == ',') {
            i++;
            continue;
        }

        /* Bare token: an identifier, number, keyword, or operator run. */
        size_t tok_start = i;
        while (i < cursor_off) {
            char t = text[i];
            if (t == ' ' || t == '\t' || t == '\n' || t == '\r' || t == ',' ||
                t == '(' || t == ')' || t == '[' || t == ']' ||
                t == '{' || t == '}' || t == '"' || t == ';')
                break;
            i++;
        }
        if (depth > 0 && !overflow) {
            CallFrame *f = &stack[depth - 1];
            if (f->forms == 0) {
                size_t n = i - tok_start;
                if (n >= sizeof(f->name)) n = sizeof(f->name) - 1;
                memcpy(f->name, text + tok_start, n);
                f->name[n] = '\0';
            }
            f->forms++;
            f->last_end = i;
        }
    }

    if (depth == 0 || overflow) return 0;
    CallFrame *f = &stack[depth - 1];
    if (!f->name[0] || f->forms == 0) return 0;

    /* A form that ends exactly at the cursor is the one being typed, so it is
     * the active argument rather than a completed one. */
    int active = f->forms - 1;
    if (f->last_end == cursor_off) active--;
    if (active < 0) {
        /* Still typing the head itself -- there is no call yet. */
        if (f->forms <= 1) return 0;
        active = 0;
    }

    if (out_name && name_cap) {
        size_t n = strlen(f->name);
        if (n >= name_cap) n = name_cap - 1;
        memcpy(out_name, f->name, n);
        out_name[n] = '\0';
    }
    if (active_param_out) *active_param_out = active;
    return 1;
}

int lsp_ident_range_at(const char *text, size_t text_len, size_t off,
                       size_t *start, size_t *end) {
    if (!text || text_len == 0 || !start || !end) return 0;
    if (off > text_len) off = text_len;
    /* A caret just past the last character of a name still belongs to it. */
    if ((off >= text_len || !is_ident_char(text[off])) &&
        off > 0 && is_ident_char(text[off - 1]))
        off--;
    if (off >= text_len || !is_ident_char(text[off])) return 0;

    size_t s = off, e = off;
    while (s > 0 && is_ident_char(text[s - 1])) s--;
    while (e < text_len && is_ident_char(text[e])) e++;
    *start = s;
    *end   = e;
    return 1;
}

/* A drive-qualified Windows path: C:\dir or c:/dir. */
/* Does `path` end in the path components `tok` (len `tlen`) plus `ext`? */
static int path_ends_with(const char *path, const char *tok, size_t tlen,
                          const char *ext) {
    size_t plen = strlen(path), elen = strlen(ext), clen = tlen + elen;
    if (clen == 0 || clen > plen) return 0;
    const char *tail = path + plen - clen;
    if (memcmp(tail, tok, tlen) != 0 || memcmp(tail + tlen, ext, elen) != 0)
        return 0;
    return tail == path || tail[-1] == '/' || tail[-1] == '\\';
}

static int token_names_path(const char *tok, size_t tlen, const char *path) {
    if (tlen >= 2 && tok[0] == '.' && tok[1] == '/') { tok += 2; tlen -= 2; }
    static const char *const exts[] = { "", ".tur", ".tur.sweet", ".scm" };
    for (size_t i = 0; i < sizeof(exts) / sizeof(exts[0]); i++)
        if (path_ends_with(path, tok, tlen, exts[i])) return 1;
    return 0;
}

int lsp_reference_anchor(const char *text, size_t text_len,
                         const char *foreign_path, unsigned *line0,
                         unsigned *col_start0, unsigned *col_end0) {
    if (!text || !foreign_path || !*foreign_path) return 0;
    unsigned line = 0;
    size_t ls = 0;
    while (ls < text_len) {
        size_t le = ls;
        while (le < text_len && text[le] != '\n') le++;

        /* The head: `(load ...` in s-expression source, `load ...` in
         * sweet-exp, either way the first token of the line. */
        size_t i = ls;
        while (i < le && (text[i] == ' ' || text[i] == '\t')) i++;
        if (i < le && text[i] == '(') i++;
        size_t hs = i;
        while (i < le && text[i] != ' ' && text[i] != '\t' &&
               text[i] != '(' && text[i] != ')' && text[i] != '"') i++;
        size_t hl = i - hs;
        int is_ref = (hl == 4 && memcmp(text + hs, "load", 4) == 0) ||
                     (hl == 6 && memcmp(text + hs, "import", 6) == 0) ||
                     (hl == 7 && memcmp(text + hs, "include", 7) == 0);

        while (is_ref && i < le) {
            char c = text[i];
            if (c == ' ' || c == '\t' || c == '(' || c == ')' ||
                c == '[' || c == ']' || c == ';') {
                if (c == ';') break;            /* comment to end of line */
                i++;
                continue;
            }
            size_t ts = i, cs, cl;
            if (c == '"') {
                i++;
                cs = i;
                while (i < le && text[i] != '"') i++;
                cl = i - cs;
                if (i < le) i++;                /* past the closing quote */
            } else {
                cs = i;
                while (i < le && text[i] != ' ' && text[i] != '\t' &&
                       text[i] != '(' && text[i] != ')' &&
                       text[i] != '[' && text[i] != ']') i++;
                cl = i - cs;
            }
            /* `:refer`, `:as` and friends are options, not module names. */
            if (cl > 0 && text[cs] != ':' &&
                token_names_path(text + cs, cl, foreign_path)) {
                *line0      = line;
                *col_start0 = (unsigned)(ts - ls);
                *col_end0   = (unsigned)(i - ls);
                return 1;
            }
        }
        line++;
        ls = le + 1;
    }
    return 0;
}

static int lsp_has_drive(const char *p) {
    return p && ((p[0] >= 'A' && p[0] <= 'Z') || (p[0] >= 'a' && p[0] <= 'z'))
           && p[1] == ':' && (p[2] == '/' || p[2] == '\\');
}

/* Filesystem path -> `file:` URI.
 *
 * On Windows this used to percent-encode the drive colon and every backslash,
 * emitting file://C%3A%5Cdir%5Cx.tur.  That round-trips through this file's own
 * decoder, so every internal comparison agreed and the server looked correct --
 * but no editor spells a path that way, so a `textDocument/definition` answer
 * naming another file was a URI the client could not match to any document it
 * had.  Go-to-definition landed nowhere.
 *
 * The spelling every LSP client uses is file:///C:/dir/x.tur: an empty
 * authority, a literal drive colon, forward slashes.  POSIX paths are
 * unchanged -- they already start with '/', so file:// + /dir gives the same
 * three slashes. */
char *lsp_path_to_uri(const char *path, char *dest, size_t dest_cap) {
    if (!path || !dest || dest_cap < 8) return dest;
    size_t di = 0;
    const char *prefix = "file://";
    while (*prefix && di < dest_cap - 1) dest[di++] = *prefix++;
    /* file:///C:/... -- the slash before the drive letter is part of the path
     * component, and the authority is empty. */
    if (lsp_has_drive(path) && di < dest_cap - 1) dest[di++] = '/';
    /* Encode path: percent-encode characters outside unreserved + '/'.
     * A backslash becomes '/' rather than %5C, and the drive colon is kept
     * literal -- both are what clients send and expect back. */
    for (const char *p = path; *p && di + 3 < dest_cap; p++) {
        unsigned char c = (unsigned char)*p;
#ifdef _WIN32
        /* Windows only: a backslash IS a separator there.  On POSIX it is a
         * legal filename character, so rewriting it to '/' would invent a
         * directory boundary -- it keeps its %5C escape. */
        if (c == '\\') { dest[di++] = '/'; continue; }
#endif
        if (c == ':' && p == path + 1 && lsp_has_drive(path)) {
            dest[di++] = ':';
            continue;
        }
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '/' || c == '-' ||
            c == '_' || c == '.' || c == '~') {
            dest[di++] = (char)c;
        } else {
            di += (size_t)snprintf(dest + di, dest_cap - di, "%%%02X", c);
        }
    }
    dest[di] = '\0';
    return dest;
}

/* -------------------------------------------------------------------------
 * Occurrence scan (try-turmeric-navigation-and-minimap-plan, M5)
 * --------------------------------------------------------------------- */

void lsp_scan_occurrences(const char *text, size_t text_len,
                          const char *name,
                          LspOccurrenceFn fn, void *user) {
    if (!text || text_len == 0 || !name || !*name || !fn) return;
    size_t nlen = strlen(name);
    if (nlen == 0 || nlen > text_len) return;

    int    line0      = 0;
    size_t line_start = 0;
    size_t i          = 0;

    /* Advance one byte, keeping the line counter honest. Every skip below
     * goes through here rather than doing `i++`, because a comment or a
     * string can span lines and a lost newline shifts every position after
     * it -- silently, in the direction that makes a highlight land on the
     * wrong row. */
    #define ADVANCE() do { \
        if (text[i] == '\n') { line0++; line_start = i + 1; } \
        i++; \
    } while (0)

    while (i < text_len) {
        char c = text[i];

        if (c == ';') {                       /* line comment */
            while (i < text_len && text[i] != '\n') i++;
            continue;                          /* the '\n' itself falls through */
        }
        if (c == '"') {                       /* string literal */
            ADVANCE();
            while (i < text_len && text[i] != '"') {
                if (text[i] == '\\' && i + 1 < text_len) ADVANCE();
                ADVANCE();
            }
            if (i < text_len) ADVANCE();       /* closing quote */
            continue;
        }
        if (c == '#' && i + 1 < text_len && text[i + 1] == '|') {
            int depth = 0;                     /* block comment, nesting */
            while (i < text_len) {
                if (text[i] == '#' && i + 1 < text_len && text[i + 1] == '|') {
                    depth++;
                    ADVANCE(); ADVANCE();
                    continue;
                }
                if (text[i] == '|' && i + 1 < text_len && text[i + 1] == '#') {
                    depth--;
                    ADVANCE(); ADVANCE();
                    if (depth <= 0) break;
                    continue;
                }
                ADVANCE();
            }
            continue;
        }
        if (c == '`' && i + 2 < text_len &&
            text[i + 1] == '`' && text[i + 2] == '`') {
            ADVANCE(); ADVANCE(); ADVANCE();   /* opening fence */
            while (i + 2 < text_len &&
                   !(text[i] == '`' && text[i + 1] == '`' && text[i + 2] == '`')) {
                ADVANCE();
            }
            if (i + 2 < text_len) { ADVANCE(); ADVANCE(); ADVANCE(); }
            else { while (i < text_len) ADVANCE(); }
            continue;
        }

        if (!is_ident_char(c)) { ADVANCE(); continue; }

        /* On an identifier. Consume the whole of it, then compare -- a
         * substring match inside a longer name is not an occurrence, and
         * testing the boundary after a memcmp would still have to find the
         * end to know where to resume. */
        size_t start = i;
        while (i < text_len && is_ident_char(text[i])) i++;
        size_t len = i - start;
        if (len == nlen && memcmp(text + start, name, nlen) == 0) {
            fn(start, line0, (int)(start - line_start), (int)nlen, user);
        }
    }
    #undef ADVANCE
}
