/* jit_prune -- see jit_prune.h for what and why. */
#include "jit_prune.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rt_split_embed.h"

/* ------------------------------------------------------------------------ */
/* A lexer just good enough for emitted C: it only has to know when it is in */
/* code (as opposed to a comment or a string/char literal), the brace and   */
/* paren depth, and where identifiers are.                                   */
/* ------------------------------------------------------------------------ */

static int is_id_start(int c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}
static int is_id_char(int c) { return is_id_start(c) || (c >= '0' && c <= '9'); }

/* Skip a comment or literal starting at s[i]; returns the index just past it,
 * or i when s[i] does not start one. */
static size_t skip_noncode(const char *s, size_t n, size_t i) {
    if (i + 1 < n && s[i] == '/' && s[i + 1] == '/') {
        while (i < n && s[i] != '\n') i++;
        return i;
    }
    if (i + 1 < n && s[i] == '/' && s[i + 1] == '*') {
        i += 2;
        while (i + 1 < n && !(s[i] == '*' && s[i + 1] == '/')) i++;
        return i + 1 < n ? i + 2 : n;
    }
    if (s[i] == '"' || s[i] == '\'') {
        char q = s[i++];
        while (i < n && s[i] != q) {
            if (s[i] == '\\' && i + 1 < n) i++;
            else if (s[i] == '\n') break;   /* unterminated: stop at the line */
            i++;
        }
        return i < n ? i + 1 : n;
    }
    return i;
}

/* If s[i] is the `#` of a preprocessor line whose identifiers name nothing a
 * program uses -- a conditional (`#if`, `#ifdef`, `#elif`, ...), `#undef`,
 * `#include`, `#pragma`, `#error` -- the index past the line and its
 * continuations; else 0.  `#  undef min` in the prefix named the stdlib's
 * `min`, and so kept it (and `max` beside it) in every program.  A `#define`
 * is read: its body names what an expansion will use. */
static size_t pp_names_nothing(const char *s, size_t n, size_t i) {
    if (s[i] != '#') return 0;
    size_t b = i;
    while (b > 0 && (s[b - 1] == ' ' || s[b - 1] == '\t')) b--;
    if (b > 0 && s[b - 1] != '\n') return 0;          /* not first on its line */
    size_t j = i + 1;
    while (j < n && (s[j] == ' ' || s[j] == '\t')) j++;
    size_t k = j;
    while (k < n && is_id_char((unsigned char)s[k])) k++;
    static const char *const skip[] = { "if", "ifdef", "ifndef", "elif", "else", "endif",
                                        "undef", "include", "pragma", "error", "warning",
                                        "line", NULL };
    int hit = 0;
    for (int q = 0; skip[q] && !hit; q++)
        hit = strlen(skip[q]) == k - j && memcmp(s + j, skip[q], k - j) == 0;
    if (!hit) return 0;
    while (k < n) {
        if (s[k] == '\n') {
            size_t e = k;
            while (e > i && (s[e - 1] == ' ' || s[e - 1] == '\t' || s[e - 1] == '\r')) e--;
            if (e > i && s[e - 1] == '\\') { k++; continue; }
            return k;
        }
        k++;
    }
    return n;
}

/* Call `fn` for every identifier of s[0..n) that is in code. */
typedef void (*id_fn)(const char *id, size_t len, void *ud);
static void for_each_ident(const char *s, size_t n, id_fn fn, void *ud) {
    size_t i = 0;
    while (i < n) {
        size_t j = skip_noncode(s, n, i);
        if (j != i) { i = j; continue; }
        size_t pp = pp_names_nothing(s, n, i);
        if (pp) { i = pp; continue; }
        if (is_id_start((unsigned char)s[i]) &&
            (i == 0 || !is_id_char((unsigned char)s[i - 1]))) {
            size_t k = i;
            while (k < n && is_id_char((unsigned char)s[k])) k++;
            fn(s + i, k - i, ud);
            i = k;
            continue;
        }
        /* A number like 0x1f or 1e10: skip it whole so its letters are not
         * read as an identifier. */
        if (s[i] >= '0' && s[i] <= '9') {
            while (i < n && (is_id_char((unsigned char)s[i]) || s[i] == '.')) i++;
            continue;
        }
        i++;
    }
}

/* ------------------------------------------------------------------------ */
/* A string -> small-int-list map for names.                                 */
/* ------------------------------------------------------------------------ */

typedef struct NameEnt {
    const char *name;    /* points into the source; not NUL-terminated */
    size_t      len;
    int         live;
    unsigned   *chunks;  /* the node chunks this name defines */
    unsigned    n_chunks, cap_chunks;
} NameEnt;

typedef struct NameMap {
    NameEnt *ents;
    size_t   cap;        /* power of two */
    size_t   n;
} NameMap;

static uint64_t name_hash(const char *s, size_t n) {
    uint64_t h = 1469598103934665603ull;
    for (size_t i = 0; i < n; i++) { h ^= (unsigned char)s[i]; h *= 1099511628211ull; }
    return h;
}

static NameEnt *nm_find(NameMap *m, const char *s, size_t n, int create) {
    if (m->cap == 0) {
        if (!create) return NULL;
        m->cap = 1024;
        m->ents = (NameEnt *)calloc(m->cap, sizeof(NameEnt));
        if (!m->ents) { fprintf(stderr, "tur: oom\n"); abort(); }
    }
    if (create && (m->n + 1) * 2 > m->cap) {
        NameEnt *old = m->ents;
        size_t oc = m->cap;
        m->cap *= 2;
        m->ents = (NameEnt *)calloc(m->cap, sizeof(NameEnt));
        if (!m->ents) { fprintf(stderr, "tur: oom\n"); abort(); }
        for (size_t i = 0; i < oc; i++) {
            if (!old[i].name) continue;
            size_t k = (size_t)name_hash(old[i].name, old[i].len) & (m->cap - 1);
            while (m->ents[k].name) k = (k + 1) & (m->cap - 1);
            m->ents[k] = old[i];
        }
        free(old);
    }
    size_t k = (size_t)name_hash(s, n) & (m->cap - 1);
    while (m->ents[k].name) {
        if (m->ents[k].len == n && memcmp(m->ents[k].name, s, n) == 0)
            return &m->ents[k];
        k = (k + 1) & (m->cap - 1);
    }
    if (!create) return NULL;
    m->ents[k].name = s;
    m->ents[k].len = n;
    m->n++;
    return &m->ents[k];
}

static void nm_free(NameMap *m) {
    for (size_t i = 0; i < m->cap; i++) free(m->ents[i].chunks);
    free(m->ents);
}

static void ent_add_chunk(NameEnt *e, unsigned c) {
    if (e->n_chunks == e->cap_chunks) {
        e->cap_chunks = e->cap_chunks ? e->cap_chunks * 2 : 2;
        e->chunks = (unsigned *)realloc(e->chunks, e->cap_chunks * sizeof(unsigned));
        if (!e->chunks) { fprintf(stderr, "tur: oom\n"); abort(); }
    }
    e->chunks[e->n_chunks++] = c;
}

/* ------------------------------------------------------------------------ */
/* Top-level chunks of the program half.                                      */
/* ------------------------------------------------------------------------ */

typedef struct Chunk {
    size_t   start, end;       /* [start, end) in the source */
    NameEnt *defines;          /* NULL: a root */
    int      live;
    int      is_pp;
    int      is_macro;         /* a `#define NAME`: kept, read only when NAME is */
} Chunk;

typedef struct ChunkVec { Chunk *v; size_t n, cap; } ChunkVec;

static void cv_push(ChunkVec *cv, size_t start, size_t end, int is_pp) {
    if (end <= start) return;
    if (cv->n == cv->cap) {
        cv->cap = cv->cap ? cv->cap * 2 : 256;
        cv->v = (Chunk *)realloc(cv->v, cv->cap * sizeof(Chunk));
        if (!cv->v) { fprintf(stderr, "tur: oom\n"); abort(); }
    }
    Chunk *c = &cv->v[cv->n++];
    memset(c, 0, sizeof(*c));
    c->start = start;
    c->end = end;
    c->is_pp = is_pp;
}

/* Split s[0..n) into top-level entities.  A new entity starts at a line that
 * begins in column 0 with brace and paren depth 0, provided the previous one
 * is complete (its last code character was `;`, `}` or `)`, or it was a
 * preprocessor line that did not continue).  So `static void\nfoo(void) {`
 * and a `{` on its own line stay with the header above them.
 *
 * A `)` that closes a top-level `__attribute__((...))` does NOT complete one:
 * the emitter writes `__attribute__((constructor))` on its own line above the
 * definition it decorates.  Split there, the attribute was a root and the
 * definition an unreferenced static, so the pruned TU ended in a dangling
 * attribute.  Linux's c2mir let it through (and the constructor was silently
 * gone); macOS's rejected it -- "syntax error on 317 (expected
 * '<declarator>')" -- and every program paid the full-TU retry. */
static void split_chunks(const char *s, size_t n, size_t base, ChunkVec *cv) {
    size_t i = 0, cur = 0;
    int depth = 0, paren = 0;
    char last = 0;          /* last code character of the current chunk */
    int cur_pp = 0, pp_continues = 0, have_code = 0;
    int at_line_start = 1;
    int in_attr = 0;        /* inside a top-level __attribute__((...)) */
    int last_attr = 0;      /* ...and `last` is the `)` that closed it */
    while (i < n) {
        if (at_line_start) {
            at_line_start = 0;
            char c = s[i];
            int complete = !have_code ||
                           (cur_pp ? !pp_continues
                                   : (last == ';' || last == '}' ||
                                      (last == ')' && !last_attr)));
            if (depth == 0 && paren == 0 && complete && i > cur &&
                c != ' ' && c != '\t' && c != '\n' && c != '\r' &&
                c != '{' && c != '}') {
                cv_push(cv, base + cur, base + i, cur_pp);
                cur = i; have_code = 0; cur_pp = 0; last = 0;
                in_attr = 0; last_attr = 0;
            }
            if (depth == 0 && c == '#' && !cur_pp) {
                if (have_code && i > cur) {
                    cv_push(cv, base + cur, base + i, cur_pp);
                    cur = i;
                }
                cur_pp = 1; have_code = 1;
            }
            if (cur_pp && c != '#' && !pp_continues && depth == 0) {
                /* the preprocessor line ended on the previous line */
            }
        }
        size_t j = skip_noncode(s, n, i);
        if (j != i) {
            /* A comment may run across lines; a literal cannot (much). */
            for (size_t k = i; k < j; k++)
                if (s[k] == '\n') at_line_start = 1;
            if (s[i] == '"' || s[i] == '\'') { last = s[i]; have_code = 1; }
            i = j;
            continue;
        }
        char c = s[i];
        if (c == '\n') {
            if (cur_pp) {
                /* continued when the line ends in a backslash */
                size_t k = i;
                while (k > cur && (s[k - 1] == ' ' || s[k - 1] == '\t' || s[k - 1] == '\r')) k--;
                pp_continues = (k > cur && s[k - 1] == '\\');
            }
            at_line_start = 1;
            i++;
            continue;
        }
        if (c == ' ' || c == '\t' || c == '\r') { i++; continue; }
        if (!cur_pp && is_id_start((unsigned char)c)) {
            size_t e = i;
            while (e < n && is_id_char((unsigned char)s[e])) e++;
            if (depth == 0 && paren == 0) {
                in_attr = (e - i == 13 && memcmp(s + i, "__attribute__", 13) == 0);
                last_attr = 0;
            }
            last = s[e - 1];
            have_code = 1;
            i = e;
            continue;
        }
        if (!cur_pp) {
            if (c == '{') depth++;
            else if (c == '}') { if (depth > 0) depth--; }
            else if (c == '(') paren++;
            else if (c == ')') { if (paren > 0) paren--; }
            if (depth == 0 && paren == 0) {
                last_attr = (c == ')' && in_attr);
                if (c != '(') in_attr = 0;
            }
        }
        last = c;
        have_code = 1;
        i++;
    }
    cv_push(cv, base + cur, base + n, cur_pp);
}

/* Header tokens of a chunk: identifiers and single punctuation characters in
 * code, up to (and including) the first `{` or `;` at paren depth 0. */
typedef struct Tok { const char *p; size_t len; } Tok;

static size_t header_tokens(const char *s, size_t n, Tok *out, size_t max) {
    size_t i = 0, k = 0;
    int paren = 0;
    while (i < n && k < max) {
        size_t j = skip_noncode(s, n, i);
        if (j != i) {
            if (s[i] == '"' || s[i] == '\'') { out[k].p = s + i; out[k].len = j - i; k++; }
            i = j;
            continue;
        }
        char c = s[i];
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') { i++; continue; }
        if (is_id_start((unsigned char)c)) {
            size_t e = i;
            while (e < n && is_id_char((unsigned char)s[e])) e++;
            out[k].p = s + i; out[k].len = e - i; k++;
            i = e;
            continue;
        }
        if (c >= '0' && c <= '9') {
            size_t e = i;
            while (e < n && (is_id_char((unsigned char)s[e]) || s[e] == '.')) e++;
            out[k].p = s + i; out[k].len = e - i; k++;
            i = e;
            continue;
        }
        out[k].p = s + i; out[k].len = 1; k++;
        if (c == '(') paren++;
        else if (c == ')') { if (paren > 0) paren--; }
        else if ((c == '{' || c == ';') && paren == 0) break;
        i++;
    }
    return k;
}

static int tok_is(const Tok *t, const char *w) {
    size_t n = strlen(w);
    return t->len == n && memcmp(t->p, w, n) == 0;
}
static int tok_is_ident(const Tok *t) { return t->len > 0 && is_id_start((unsigned char)t->p[0]); }

/* Index just past the bracket matching the one at s[i] (`{` or `(`), reading
 * code only; n when it never closes. */
static size_t match_close(const char *s, size_t n, size_t i) {
    char open = s[i], close = open == '{' ? '}' : ')';
    int d = 0;
    while (i < n) {
        size_t j = skip_noncode(s, n, i);
        if (j != i) { i = j; continue; }
        if (s[i] == open) d++;
        else if (s[i] == close && --d == 0) return i + 1;
        i++;
    }
    return n;
}

/* Is s[i..n) free of code, give or take stray `;`? */
static int rest_is_blank(const char *s, size_t n, size_t i) {
    while (i < n) {
        size_t j = skip_noncode(s, n, i);
        if (j != i) {
            if (s[i] == '"' || s[i] == '\'') return 0;
            i = j;
            continue;
        }
        char c = s[i];
        if (c != ' ' && c != '\t' && c != '\n' && c != '\r' && c != ';') return 0;
        i++;
    }
    return 1;
}

/* The name a removable chunk defines, or len 0 for a root.  Removable:
 *   static <type> name(<params>) { ... }     function definition
 *   static <type> name(<params>);            prototype
 *   static <type> name [= ...] / [N] ...;    object
 *   TUR_FATBOX_DEF(name, ...)                static fat box
 *   extern <type> name(<params>);            extern declaration (no body,
 *   extern <type> name;                      no initializer)
 * Anything unrecognised -- including a function-pointer declarator, a
 * `static struct X {` definition, and anything carrying a constructor,
 * destructor or `used` attribute -- is a root.  So is a chunk that holds more
 * than that one entity: an inline-C block pasted with its indentation keeps
 * `static` definitions on indented lines, which split_chunks does not start
 * a chunk at, and only the first of them is named here.  Dropping such a
 * chunk for its first name would drop the others with it (their names are in
 * no map, so nothing could keep them). */
static Tok chunk_defines(const char *s, size_t n) {
    Tok none = { NULL, 0 };
    Tok t[512];
    size_t k = header_tokens(s, n, t, sizeof t / sizeof t[0]);
    if (k == 0 || k == sizeof t / sizeof t[0]) return none;
    size_t i = 0;
    for (size_t q = 0; q < k; q++) {
        if (tok_is(&t[q], "constructor") || tok_is(&t[q], "destructor") ||
            tok_is(&t[q], "used") || tok_is(&t[q], "__used__") ||
            tok_is(&t[q], "asm") || tok_is(&t[q], "__asm__"))
            return none;
    }
    /* A box's forward declaration goes with its definition: as a root it
     * named every box, and so every function value's shim and function. */
    if (tok_is(&t[0], "TUR_FATBOX_DECL")) {
        if (k >= 4 && tok_is(&t[1], "(") && tok_is_ident(&t[2]) && tok_is(&t[3], ")") &&
            rest_is_blank(s, n, match_close(s, n, (size_t)(t[1].p - s))))
            return t[2];
        return none;
    }
    if (tok_is(&t[0], "TUR_FATBOX_DEF")) {
        if (k >= 3 && tok_is(&t[1], "(") && tok_is_ident(&t[2]) &&
            rest_is_blank(s, n, match_close(s, n, (size_t)(t[1].p - s))))
            return t[2];
        return none;
    }
    /* leading __attribute__((...)) groups */
    while (i < k && tok_is(&t[i], "__attribute__")) {
        i++;
        int d = 0;
        do {
            if (i >= k) return none;
            if (tok_is(&t[i], "(")) d++;
            else if (tok_is(&t[i], ")")) d--;
            i++;
        } while (d > 0);
    }
    /* `static` definitions and declarations, and `extern` DECLARATIONS
     * (stdlib's extern-c prototypes: 130 of them in `(println 42)`, and what
     * kept hamt.h alive).  An extern with a body or an initializer is a
     * definition some other unit may link against, so it stays a root. */
    int is_extern = 0;
    if (i < k && tok_is(&t[i], "extern")) is_extern = 1;
    else if (i >= k || !tok_is(&t[i], "static")) return none;
    i++;
    /* first `(`, `=`, `[` at paren depth 0, and the terminator */
    size_t lp = 0, eq = 0, lb = 0, term = 0;
    int d = 0;
    for (size_t q = i; q < k; q++) {
        if (tok_is(&t[q], "(")) { if (d == 0 && !lp) lp = q; d++; }
        else if (tok_is(&t[q], ")")) { if (d > 0) d--; }
        else if (d == 0 && tok_is(&t[q], "=") && !eq) eq = q;
        else if (d == 0 && tok_is(&t[q], "[") && !lb) lb = q;
        else if (d == 0 && (tok_is(&t[q], "{") || tok_is(&t[q], ";"))) { term = q; break; }
    }
    if (!term) return none;
    if (is_extern && (eq || tok_is(&t[term], "{"))) return none;
    /* Where the entity ends: past its `;`, its function body, or the `;`
     * after a brace initializer. */
    size_t tpos = (size_t)(t[term].p - s), end;
    if (tok_is(&t[term], ";")) {
        end = tpos + 1;
    } else {
        end = match_close(s, n, tpos);
        if (eq && eq < term) {
            while (end < n) {
                size_t j = skip_noncode(s, n, end);
                if (j != end) { end = j; continue; }
                if (s[end] == ';') { end++; break; }
                if (s[end] == '{' || s[end] == '(') { end = match_close(s, n, end); continue; }
                end++;
            }
        }
    }
    if (!rest_is_blank(s, n, end)) return none;
    if (lp && (!eq || lp < eq) && (!lb || lp < lb)) {
        /* A function-pointer object, `static R (*name)(params) [attrs]
         * [= init];`: removable like any static object.  The emitter writes
         * one to keep a function cc would warn is unused
         * (`__tur_any_drop_keep = __tur_any_drop`); as a root it kept that
         * function, and what it calls, in every program. */
        if (!is_extern && lp + 4 < k && tok_is(&t[lp + 1], "*") && tok_is_ident(&t[lp + 2]) &&
            tok_is(&t[lp + 3], ")") && tok_is(&t[lp + 4], "(") && tok_is(&t[term], ";"))
            return t[lp + 2];
        /* a function: `name (` where name is an identifier, and not a
         * pointer declarator `(*name)` */
        if (lp < 1 || !tok_is_ident(&t[lp - 1])) return none;
        /* at least one type token before the name */
        if (lp - 1 <= i) return none;
        if (lp + 1 < k && (tok_is(&t[lp + 1], "*") || tok_is(&t[lp + 1], "^")))
            return none;
        if (tok_is(&t[lp - 1], "__attribute__")) return none;
        /* `MACRO(a, b)(params)`: the name is built by a macro, so what
         * precedes the first `(` is not it. */
        int pd = 0;
        for (size_t q = lp; q < k; q++) {
            if (tok_is(&t[q], "(")) pd++;
            else if (tok_is(&t[q], ")") && --pd == 0) {
                if (q + 1 < k && tok_is(&t[q + 1], "(")) return none;
                break;
            }
        }
        return t[lp - 1];
    }
    /* an object: the identifier before the first `=`, `[` or `;` */
    size_t stop = term;
    if (eq && eq < stop) stop = eq;
    if (lb && lb < stop) stop = lb;
    if (tok_is(&t[term], "{") && !(eq && eq < term)) return none;  /* static struct X { */
    if (stop < 1 || !tok_is_ident(&t[stop - 1])) return none;
    return t[stop - 1];
}

/* The name a `#define NAME ...` chunk defines, or len 0 for any other
 * preprocessor line.  A define is not a root: its body names something only
 * once NAME itself is named.  The emitter aliases every ADT constructor
 * (`#define ctor_Tuple2 ctor_Tuple2_Tuple2`), and as roots those aliases kept
 * every stdlib constructor in every program -- `Tuple2`..`Tuple8`, `Grid`,
 * `Schema`, `Slice`, `Pair`, `Option`, `Result` in `(println 1)`.  The line
 * itself is always written out: an unused macro costs nothing, and a dropped
 * one could only fail. */
static Tok pp_define_name(const char *s, size_t n) {
    Tok none = { NULL, 0 };
    size_t i = 0;
    if (i >= n || s[i] != '#') return none;
    i++;
    while (i < n && (s[i] == ' ' || s[i] == '\t')) i++;
    if (n - i < 6 || memcmp(s + i, "define", 6) != 0) return none;
    i += 6;
    if (i >= n || (s[i] != ' ' && s[i] != '\t')) return none;
    while (i < n && (s[i] == ' ' || s[i] == '\t')) i++;
    if (i >= n || !is_id_start((unsigned char)s[i])) return none;
    size_t k = i;
    while (k < n && is_id_char((unsigned char)s[k])) k++;
    Tok t = { s + i, k - i };
    return t;
}

/* `TUR_FATBOX_FILL(box, shim, fn)` fills a file-scope box at startup where a
 * pointer is narrower than its slots (wasm32); elsewhere it is `((void)0)`.
 * `__tur_fatbox_init` holds one per box, and as a live chunk it named every
 * box's shim and function, so no function value was ever dropped.  The box's
 * own TUR_FATBOX_DEF already names both, so a fill of a box this TU defines
 * names nothing (mark_chunk), and is written out only while that box is live
 * (write_chunk).  A box the map does not know -- one declared inside a
 * function -- is read as before. */
static int is_fill_at(const char *s, size_t n, size_t i) {
    return n - i >= 15 && memcmp(s + i, "TUR_FATBOX_FILL", 15) == 0 &&
           (i + 15 >= n || !is_id_char((unsigned char)s[i + 15])) &&
           (i == 0 || !is_id_char((unsigned char)s[i - 1]));
}

/* For a fill at s[i]: its box's name, and the end of the statement (past the
 * `;` after the closing paren).  Returns 0 when it is not a well-formed fill. */
static int fill_parse(const char *s, size_t n, size_t i, Tok *box, size_t *end) {
    size_t j = i + 15;
    while (j < n && (s[j] == ' ' || s[j] == '\t')) j++;
    if (j >= n || s[j] != '(') return 0;
    size_t close = match_close(s, n, j);
    size_t b = j + 1;
    while (b < close && (s[b] == ' ' || s[b] == '\t')) b++;
    if (b >= close || !is_id_start((unsigned char)s[b])) return 0;
    size_t e = b;
    while (e < close && is_id_char((unsigned char)s[e])) e++;
    box->p = s + b;
    box->len = e - b;
    size_t k = close;
    while (k < n && (s[k] == ' ' || s[k] == '\t')) k++;
    if (k < n && s[k] == ';') k++;
    *end = k;
    return 1;
}

/* ------------------------------------------------------------------------ */
/* Liveness                                                                   */
/* ------------------------------------------------------------------------ */

typedef struct MarkCtx {
    NameMap  *map;
    Chunk    *chunks;
    unsigned *work;
    size_t    n_work, cap_work;
    const NameEnt *self;
} MarkCtx;

static void push_work(MarkCtx *m, unsigned c) {
    if (m->n_work == m->cap_work) {
        m->cap_work = m->cap_work ? m->cap_work * 2 : 256;
        m->work = (unsigned *)realloc(m->work, m->cap_work * sizeof(unsigned));
        if (!m->work) { fprintf(stderr, "tur: oom\n"); abort(); }
    }
    m->work[m->n_work++] = c;
}

static void mark_ident(const char *id, size_t len, void *ud) {
    MarkCtx *m = (MarkCtx *)ud;
    NameEnt *e = nm_find(m->map, id, len, 0);
    if (!e || e == m->self || e->live || e->n_chunks == 0) return;
    e->live = 1;
    for (unsigned q = 0; q < e->n_chunks; q++) push_work(m, e->chunks[q]);
}

/* ------------------------------------------------------------------------ */
/* Heavy includes of the decls region                                        */
/* ------------------------------------------------------------------------ */

/* A header in the committed decls region, and the identifiers (exact, or a
 * prefix when the entry ends in `*`) that mean a program uses it.  Only the
 * PROGRAM's own text is scanned: the decls region declares its one hamt
 * function (tur_hamt_hash_xxh64) itself and names socket types only under
 * _WIN32, where these lines are not the ones included.  If the decls region
 * ever starts needing one of these headers on its own, the run-jit.sh smoke
 * test (a trivial program must run natively) fails first. */
typedef struct HeavyInclude {
    const char *line;
    const char *const *markers;
    const char *stand_in;   /* written in its place when dropped, or NULL */
} HeavyInclude;

static const char *const k_regex[] = {
    "regex_t", "regmatch_t", "regoff_t", "regcomp", "regexec", "regfree",
    "regerror", "REG_*", NULL };
static const char *const k_inet[] = {
    "inet_*", "htons", "htonl", "ntohs", "ntohl", "in_addr*", "in6_addr",
    "sockaddr_in*", "in_port_t", NULL };
static const char *const k_netin[] = {
    "sockaddr_in*", "in_addr*", "in6_addr", "in_port_t", "IPPROTO_*",
    "INADDR_*", "IN6ADDR_*", "IPV6_*", "IP_*", "htons", "htonl", "ntohs",
    "ntohl", "inet_*", NULL };
static const char *const k_socket[] = {
    "socket", "bind", "listen", "accept", "accept4", "connect", "setsockopt",
    "getsockopt", "send", "sendto", "sendmsg", "recv", "recvfrom", "recvmsg",
    "shutdown", "socketpair", "getpeername", "getsockname", "sockaddr*",
    "socklen_t", "msghdr", "cmsghdr", "iovec", "AF_*", "PF_*", "SOCK_*",
    "SOL_*", "SO_*", "MSG_*", "SHUT_*", "SOMAXCONN", "CMSG_*", NULL };
static const char *const k_select[] = {
    "select", "pselect", "fd_set", "FD_*", "timeval", "suseconds_t", NULL };
static const char *const k_hamt[] = {
    "tur_hamt_*", "Hamt*", "HAMT_*", "TUR_HAMT_H", NULL };

/* hamt.h is the decls region's FIRST include, so its own system includes are
 * where the TU's first system header comes from.  Dropped outright, the first
 * system header would be <ucontext.h>, under the region's `#define
 * _XOPEN_SOURCE 700`, and some libcs fix their feature level once, on that
 * first inclusion (macOS's <sys/cdefs.h> would settle on the POSIX level).  No
 * failure has been traced to that -- a CI run compiled ~800 programs on macOS
 * with hamt.h dropped -- so this is a precaution: leaving hamt.h's three
 * includes behind keeps the header order ahead of that block what it was
 * unpruned.  The other entries sit after it and carry no such role. */
static const HeavyInclude k_heavy[] = {
    { "#include <regex.h>",      k_regex,  NULL },
    { "#include <arpa/inet.h>",  k_inet,   NULL },
    { "#include <netinet/in.h>", k_netin,  NULL },
    { "#include <sys/socket.h>", k_socket, NULL },
    { "#include <sys/select.h>", k_select, NULL },
    { "#include \"hamt.h\"",     k_hamt,
      "#include <stdint.h>\n#include <stdbool.h>\n#include <stdio.h>\n" },
};
#define N_HEAVY (sizeof k_heavy / sizeof k_heavy[0])

typedef struct UseCtx { int used[N_HEAVY]; } UseCtx;

static int marker_match(const char *m, const char *id, size_t len) {
    size_t ml = strlen(m);
    if (ml && m[ml - 1] == '*')
        return len >= ml - 1 && memcmp(id, m, ml - 1) == 0;
    return len == ml && memcmp(id, m, ml) == 0;
}

static void use_ident(const char *id, size_t len, void *ud) {
    UseCtx *u = (UseCtx *)ud;
    for (size_t h = 0; h < N_HEAVY; h++) {
        if (u->used[h]) continue;
        for (const char *const *m = k_heavy[h].markers; *m; m++)
            if (marker_match(*m, id, len)) { u->used[h] = 1; break; }
    }
}

/* Is s[a..b) (one line, no newline) exactly `line`, give or take trailing
 * whitespace? */
static int line_is(const char *s, size_t a, size_t b, const char *line) {
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r')) b--;
    size_t n = strlen(line);
    return b - a == n && memcmp(s + a, line, n) == 0;
}

/* Copy s[a..b) to out, leaving out whole lines that are a dropped include. */
static unsigned copy_without_includes(Buf *out, const char *s, size_t a, size_t b,
                                      const UseCtx *u) {
    unsigned dropped = 0;
    size_t i = a;
    while (i < b) {
        size_t e = i;
        while (e < b && s[e] != '\n') e++;
        const HeavyInclude *drop = NULL;
        if (s[i] == '#') {
            for (size_t h = 0; h < N_HEAVY && !drop; h++)
                if (!u->used[h] && line_is(s, i, e, k_heavy[h].line)) drop = &k_heavy[h];
        }
        size_t next = e < b ? e + 1 : e;
        if (drop) {
            dropped++;
            if (drop->stand_in) buf_puts(out, drop->stand_in);
        } else {
            buf_write(out, s + i, next - i);
        }
        i = next;
    }
    return dropped;
}

/* ------------------------------------------------------------------------ */

static size_t find_bytes(const char *hay, size_t n, const char *needle, size_t m) {
    if (m == 0 || m > n) return (size_t)-1;
    for (size_t i = 0; i + m <= n; i++)
        if (hay[i] == needle[0] && memcmp(hay + i, needle, m) == 0) return i;
    return (size_t)-1;
}

static const char *const *g_extra_roots;
static size_t g_n_extra_roots;

void jit_prune_set_extra_roots(const char *const *names, size_t n) {
    g_extra_roots = n ? names : NULL;
    g_n_extra_roots = names ? n : 0;
}

/* Is the function chunk s[0..n) one of g_extra_roots?  Read from the chunk's
 * own text: its NameEnt may have moved since (the map grows as it fills). */
static int is_extra_root(const char *s, size_t n) {
    Tok nm = chunk_defines(s, n);
    for (size_t i = 0; nm.len && i < g_n_extra_roots; i++)
        if (strlen(g_extra_roots[i]) == nm.len && memcmp(g_extra_roots[i], nm.p, nm.len) == 0)
            return 1;
    return 0;
}

static int prune_disabled(void) {
    const char *off = getenv("TUR_JIT_NO_PRUNE");
    return off && *off && strcmp(off, "0") != 0;
}

/* Is `box` a file-scope fat box this TU defines (a TUR_FATBOX_DEF node)? */
static NameEnt *fill_box_known(NameMap *map, Tok box) {
    NameEnt *e = nm_find(map, box.p, box.len, 0);
    return (e && e->n_chunks) ? e : NULL;
}

/* mark_ident over a chunk, reading a fill of a known box as naming nothing. */
static void mark_chunk(const char *s, size_t n, MarkCtx *m) {
    size_t i = 0, from = 0;
    while (i < n) {
        size_t j = skip_noncode(s, n, i);
        if (j != i) { i = j; continue; }
        Tok box;
        size_t end;
        if (is_fill_at(s, n, i) && fill_parse(s, n, i, &box, &end) &&
            fill_box_known(m->map, box)) {
            for_each_ident(s + from, i - from, mark_ident, m);
            i = from = end;
            continue;
        }
        i++;
    }
    for_each_ident(s + from, n - from, mark_ident, m);
}

/* Write a live chunk, leaving out the fills of boxes that were dropped. */
static void write_chunk(Buf *out, const char *s, size_t n, NameMap *map) {
    size_t i = 0, from = 0;
    while (i < n) {
        size_t j = skip_noncode(s, n, i);
        if (j != i) { i = j; continue; }
        Tok box;
        size_t end;
        NameEnt *e;
        if (is_fill_at(s, n, i) && fill_parse(s, n, i, &box, &end) &&
            (e = fill_box_known(map, box)) != NULL && !e->live) {
            buf_write(out, s + from, i - from);
            i = from = end;
            continue;
        }
        i++;
    }
    buf_write(out, s + from, n - from);
}

/* Prune the program half s[pstart..n).  s[0..pstart) is all roots; the heavy
 * includes of s[dpos..pstart) (the committed decls region, empty for a full
 * TU) are dropped when unused. */
static void prune_program_half(Buf *src, size_t dpos, size_t pstart,
                               JitPruneStats *stats) {
    const char *s = src->data;
    size_t n = src->len;
    ChunkVec cv = {0};
    split_chunks(s + pstart, n - pstart, pstart, &cv);

    NameMap map = {0};
    unsigned n_nodes = 0;
    for (size_t c = 0; c < cv.n; c++) {
        if (cv.v[c].is_pp) {
            Tok mn = pp_define_name(s + cv.v[c].start, cv.v[c].end - cv.v[c].start);
            if (mn.len) {
                NameEnt *e = nm_find(&map, mn.p, mn.len, 1);
                ent_add_chunk(e, (unsigned)c);
                cv.v[c].defines = e;
                cv.v[c].is_macro = 1;
            }
            continue;
        }
        Tok nm = chunk_defines(s + cv.v[c].start, cv.v[c].end - cv.v[c].start);
        if (!nm.len) continue;
        NameEnt *e = nm_find(&map, nm.p, nm.len, 1);
        ent_add_chunk(e, (unsigned)c);
        cv.v[c].defines = e;
        n_nodes++;
    }

    MarkCtx m = { &map, cv.v, NULL, 0, 0, NULL };
    /* Roots: everything before the program half (the hoisted prefix and the
     * decls region, whose macros may name program-half entities), and every
     * program-half chunk that is not a removable definition or declaration. */
    for_each_ident(s, pstart, mark_ident, &m);
    for (size_t c = 0; c < cv.n; c++)
        if (!cv.v[c].defines ||
            (!cv.v[c].is_macro && g_n_extra_roots &&
             is_extra_root(s + cv.v[c].start, cv.v[c].end - cv.v[c].start)))
            push_work(&m, (unsigned)c);
    while (m.n_work) {
        unsigned c = m.work[--m.n_work];
        if (cv.v[c].live) continue;
        cv.v[c].live = 1;
        m.self = cv.v[c].defines;
        mark_chunk(s + cv.v[c].start, cv.v[c].end - cv.v[c].start, &m);
        m.self = NULL;
    }

    /* Which heavy includes the program's own text still needs. */
    UseCtx use;
    memset(&use, 0, sizeof use);
    for_each_ident(s, dpos, use_ident, &use);
    for (size_t c = 0; c < cv.n; c++)
        if (cv.v[c].live)
            for_each_ident(s + cv.v[c].start, cv.v[c].end - cv.v[c].start, use_ident, &use);

    Buf out;
    buf_init(&out);
    unsigned inc_dropped = 0;
    buf_write(&out, s, dpos);
    inc_dropped += copy_without_includes(&out, s, dpos, pstart, &use);
    unsigned dropped = 0;
    for (size_t c = 0; c < cv.n; c++) {
        if (!cv.v[c].live && !cv.v[c].is_macro) { dropped++; continue; }
        if (cv.v[c].is_pp)
            inc_dropped += copy_without_includes(&out, s, cv.v[c].start, cv.v[c].end, &use);
        else
            write_chunk(&out, s + cv.v[c].start, cv.v[c].end - cv.v[c].start, &map);
    }

    if (stats) {
        stats->bytes_before = n;
        stats->bytes_after = out.len;
        stats->nodes = n_nodes;
        stats->nodes_dropped = dropped;
        stats->includes_dropped = inc_dropped;
    }
    free(m.work);
    nm_free(&map);
    free(cv.v);
    buf_free(src);
    *src = out;
}

bool jit_prune_split_source(Buf *src, JitPruneStats *stats) {
    if (prune_disabled()) return false;
    if (!src || !src->data || tur_rt_split_decls_len == 0) return false;

    /* [prefix][decls][program half] -- the decls region is copied verbatim
     * by jit_try_split_preamble, so it is found by its own text. */
    size_t probe = tur_rt_split_decls_len < 256 ? tur_rt_split_decls_len : 256;
    size_t dpos = find_bytes(src->data, src->len, tur_rt_split_decls, probe);
    if (dpos == (size_t)-1 || dpos + tur_rt_split_decls_len > src->len ||
        memcmp(src->data + dpos, tur_rt_split_decls, tur_rt_split_decls_len) != 0)
        return false;

    /* Only the program half is pruned.  Pruning the decls region's ~500
     * prototypes too was measured at 4 ms of c2mir's 67 (unsanitized, best of
     * five, `(println 42)`): what remains there is the system headers it
     * genuinely needs, and splitting the committed region is not worth it. */
    prune_program_half(src, dpos, dpos + tur_rt_split_decls_len, stats);
    return true;
}

bool jit_prune_full_source(Buf *src, JitPruneStats *stats) {
    if (prune_disabled()) return false;
    if (!src || !src->data) return false;
    static const char pre_end[] =
        "/* ==== tur: end of fixed runtime preamble ==== */\n";
    size_t m = sizeof pre_end - 1;
    size_t pe = find_bytes(src->data, src->len, pre_end, m);
    if (pe == (size_t)-1) return false;
    /* The full preamble is all roots and keeps every include: its own
     * runtime uses them. */
    prune_program_half(src, pe + m, pe + m, stats);
    return true;
}
