#include "reader.h"
#include "reader_macros.h"
#include "lang_dialects.h" /* the #lang BASE axis */
#include "types.h"   /* Phase N: TypeKind constants for literal suffixes */
#include "globals.h" /* DL0: g_data_literals_enabled */
#include "buf.h"     /* sweet-exp preprocessor */

#include <ctype.h>
#include <math.h>    /* r7rs-lang-plan R1: +inf.0 / +nan.0, #e on a float lexeme */
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

typedef struct Reader {
    const SourceFile *file;
    Arena            *arena;
    SymbolTable      *st;
    const char       *src;
    size_t            len;
    size_t            pos;
    uint32_t          line;
    uint32_t          col;
    bool              error;
    /* Phase S1: Curly-infix support */
    bool              curly_infix_enabled;
    /* Phase S2: Neoteric support */
    bool              neoteric_enabled;
    /* r7rs-lang-plan R1: the Scheme lexical layer (READER_R7RS).  A variant
     * of this reader, not a second one: `#t`/`#f`, `#\c` with the R7RS
     * names and `#\x<hex>`, `#(...)`, `#u8(...)`, `,`/`,@` as unquote (comma
     * is whitespace in every Turmeric reader -- the byte was never spoken
     * for, so this costs nothing), dotted pairs, `|sym|`, the
     * `#x`/`#o`/`#b`/`#d`/`#e`/`#i` numeric prefixes, `+5`/`.5`/`+inf.0`,
     * and the Scheme string escapes.  The Turmeric lexemes stay for the
     * Turmeric-shaped Scheme sources (the prelude, stdlib/r7rs/); in user
     * Scheme source (scheme_user_source) a leading `:` is an identifier,
     * `[...]` a list, `true`/`false`/`nil` and `^tailcall` identifiers, and
     * the `#` literals, inline C and `@` are refused
     * (docs/archive/r7rs-turmeric-syntax-leaks.md). */
    bool              scheme_enabled;
    /* `#!fold-case` / `#!no-fold-case` (R7RS 2.1), Scheme only. */
    bool              fold_case;
    /* > 0 while reading the inside of a Turmeric data literal (`#map{...}`,
     * `#set{...}`), which keeps Turmeric's lexemes in a Scheme source too
     * (its keys are keywords by its own grammar).  See scheme_user_source. */
    uint32_t          turmeric_literal_depth;
    /* RM0/RM1: User-defined #-dispatch macros. May be NULL (no user macros). */
    const ReaderMacroRegistry *user_macros;
    /* proper-tail-calls T1: true while reading the FIRST element of a `(...)`
     * list.  Consumed (and cleared) by read_form, which hands it to the symbol
     * reader so `^tailcall` keeps its prefix meaning everywhere EXCEPT list
     * head -- otherwise the explicit `(^tailcall (f x))` spelling would read
     * as a one-element list wrapping a second annotation. */
    bool at_list_head;
} Reader;

/* Forward declaration; the RM1 implementation lives further down, after the
 * basic Reader helpers (peek, advance, span_from_to, ...) it depends on. */
static Form *try_read_user_macro(Reader *r);
/* r7rs-lang-plan R1: the Scheme-only `#` dispatches and lexemes. */
static Form *try_read_scheme_hash(Reader *r);
static Form *read_piped_symbol(Reader *r);
static Form *read_number(Reader *r, int sign);
static bool  scheme_sym_extra(int c);
static bool  is_sym_cont(int c);
static int   hex_digit(int c);
static int   utf8_encode(uint32_t cp, char *out);

/* Forward declarations for neoteric support */
static int peek_neoteric_bracket(const Reader *r);
static Form *read_neoteric_bracket(Reader *r, Form *atom, int bracket);
static Form *read_seq(Reader *r, char open, char close, FormTag tag,
                      const char *unterminated_msg);
/* INT-1: Reader conditional */
static Form *read_reader_cond(Reader *r);
/* CT0: Contract-type reader. Body for `#refine{...}` and historically for
 * bare `{var : T | pred}` (now removed -- bare braces are curly-infix). */
static Form *read_contract_type(Reader *r);

static Span span_from_to(const Reader *r,
                         uint32_t start_line, uint32_t start_col,
                         size_t   start_off,  size_t end_off) {
    Span s;
    s.file_id = r->file->file_id;
    s.line = start_line;
    s.col_start = start_col;
    s.col_end = start_col + (uint32_t)(end_off - start_off);
    s.off_start = (uint32_t)start_off;
    s.off_end = (uint32_t)end_off;
    return s;
}

static Span span_point(const Reader *r) {
    Span s;
    s.file_id = r->file->file_id;
    s.line = r->line;
    s.col_start = r->col;
    s.col_end = r->col + 1;
    s.off_start = (uint32_t)r->pos;
    s.off_end = (uint32_t)r->pos + 1;
    return s;
}

static int peek(const Reader *r) {
    if (r->pos >= r->len) return -1;
    return (unsigned char)r->src[r->pos];
}

static int peek2(const Reader *r) {
    if (r->pos + 1 >= r->len) return -1;
    return (unsigned char)r->src[r->pos + 1];
}

static int peek3(const Reader *r) {
    if (r->pos + 2 >= r->len) return -1;
    return (unsigned char)r->src[r->pos + 2];
}

/* Phase N: Peek at character n positions ahead (0-based from current pos). */
static int peek_at(const Reader *r, size_t n) {
    if (r->pos + n >= r->len) return -1;
    return (unsigned char)r->src[r->pos + n];
}

static int advance(Reader *r) {
    if (r->pos >= r->len) return -1;
    int c = (unsigned char)r->src[r->pos++];
    if (c == '\n') {
        r->line++;
        r->col = 1;
    } else {
        r->col++;
    }
    return c;
}

/* Skip a #| ... |# block comment (supports nesting). */
static bool skip_block_comment(Reader *r) {
    uint32_t start_line = r->line;
    uint32_t start_col = r->col;
    size_t start_off = r->pos;

    /* Consume opening #| */
    advance(r);
    advance(r);

    int depth = 1;
    while (peek(r) != -1) {
        if (peek(r) == '#' && peek2(r) == '|') {
            advance(r);
            advance(r);
            depth++;
            continue;
        }
        if (peek(r) == '|' && peek2(r) == '#') {
            advance(r);
            advance(r);
            depth--;
            if (depth == 0) return true;
            continue;
        }
        advance(r);
    }

    diag_emit(DIAG_ERROR,
              span_from_to(r, start_line, start_col, start_off, r->pos),
              "unterminated block comment");
    r->error = true;
    return false;
}

static Form *read_form(Reader *r);
static void skip_ws_and_comments(Reader *r);

/* DC1/DC2: a datum comment `#;<datum>` at r->pos -- read and discard one
 * form.  False, with r->error set, when no form follows. */
static bool skip_datum_comment(Reader *r) {
    Span s = span_point(r);
    advance(r); advance(r);  /* consume '#' and ';' */
    skip_ws_and_comments(r);
    if (r->error) return false;
    int next = peek(r);
    if (next == -1) {
        diag_emit(DIAG_ERROR, s,
                  "datum comment #; requires a following form, "
                  "got end of input");
        r->error = true;
        return false;
    }
    if (next == ')' || next == ']' || next == '}') {
        diag_emit(DIAG_ERROR, s,
                  "datum comment #; requires a following form, got '%c'",
                  (char)next);
        r->error = true;
        return false;
    }
    Form *discarded = read_form(r);
    if (r->error) return false;
    if (!discarded) {
        diag_emit(DIAG_ERROR, s,
                  "datum comment #; requires a following form, "
                  "got end of input");
        r->error = true;
        return false;
    }
    return true;
}

static void skip_ws_and_comments(Reader *r) {
    for (;;) {
        if (r->error) return;
        int c = peek(r);
        if (c == -1) return;
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n' ||
            /* Comma is whitespace (a Clojure inheritance) in every Turmeric
             * reader; under Scheme it is unquote and read_form owns it. */
            (c == ',' && !r->scheme_enabled)) {
            advance(r);
        } else if (r->scheme_enabled && c == '#' && peek2(r) == '!' &&
                   ((r->pos + 11 <= r->len &&
                     memcmp(r->src + r->pos, "#!fold-case", 11) == 0) ||
                    (r->pos + 14 <= r->len &&
                     memcmp(r->src + r->pos, "#!no-fold-case", 14) == 0))) {
            /* R7RS 2.1: the case-folding directives are comments that flip a
             * reader flag for the rest of the file. */
            bool fold = (r->src[r->pos + 2] == 'f');
            size_t n = fold ? 11 : 14;
            if (r->pos + n < r->len && is_sym_cont((unsigned char)r->src[r->pos + n])) {
                return;   /* `#!fold-casex` -- not the directive; let read_form complain */
            }
            for (size_t i = 0; i < n; i++) advance(r);
            r->fold_case = fold;
        } else if (c == ';') {
            while ((c = peek(r)) != -1 && c != '\n') advance(r);
        } else if (c == '#' && peek2(r) == '|') {
            if (!skip_block_comment(r)) return;
        } else if (c == '#' && peek2(r) == ';') {
            /* R7RS 2.2: a datum comment is intertoken space, so it may be
             * the last thing in a list -- `(list 1 #;2)`.  Read as the
             * prefix of the next form instead, that was "unexpected ')'". */
            if (!skip_datum_comment(r)) return;
        } else {
            return;
        }
    }
}

static bool is_sym_start(int c) {
    if (c == -1) return false;
    if (isalpha(c)) return true;
    switch (c) {
        case '+': case '-': case '*': case '/':
        case '=': case '<': case '>':
        case '!': case '?':
        case '_': case '$': case '&':
        case '.':            /* Phase 15: enables .method for typeclass method calls */
        case '^':            /* enables ^mut, ^int as sym-shaped metadata */
        case '|':            /* enables |>, |||, and similar pipe-shaped operators */
        case 39:             /* single quote ' - enables lifetime annotations like 'a */
        /* UTF-8 leading bytes for λ (U+03BB: 0xCE 0xBB),
           ∀ (U+2200: 0xE2 0x88 0x80), ∃ (U+2203: 0xE2 0x88 0x83) */
        case 0xCE: case 0xE2:
            return true;
    }
    return false;
}

static bool is_sym_cont(int c) {
    if (c == -1) return false;
    if (isalnum(c)) return true;
    switch (c) {
        case '+': case '-': case '*': case '/':
        case '=': case '<': case '>':
        case '!': case '?':
        case '_': case '$': case '&':
        case '.': case '#': case '^':
        case '|':            /* enables |>, |||, and similar pipe-shaped operators */
        case 39:             /* single quote ' - allows ' in lifetime symbols like 'a */
        /* UTF-8 leading bytes */
        case 0xCE: case 0xE2:
        /* UTF-8 continuation bytes for λ (0xBB), ∀ (0x88, 0x80), ∃ (0x88, 0x83) */
        case 0x80: case 0x83: case 0x88: case 0xBB:
            return true;
    }
    return false;
}

static Form *read_form(Reader *r);
static Form *read_attribute(Reader *r);
/* `head_pos` is true when this symbol is the FIRST element of a `(...)`
 * list, which suppresses the `^tailcall` prefix sugar below. */
static Form *read_symbol_or_minus_at(Reader *r, bool head_pos);

static Form *read_string(Reader *r) {
    uint32_t start_line = r->line;
    uint32_t start_col = r->col;
    size_t start_off = r->pos;
    advance(r); /* consume opening " */

    /* Build into a temporary buffer; copy into arena at the end. */
    /* Two-pass: first scan to find end and unescaped length. */
    size_t scan = r->pos;
    size_t out_len = 0;
    bool   ok = false;
    while (scan < r->len) {
        char c = r->src[scan];
        if (c == '"') { ok = true; break; }
        if (c == '\\') {
            if (scan + 1 >= r->len) break;
            scan += 2;
            out_len++;
            continue;
        }
        scan++;
        out_len++;
    }
    if (!ok) {
        Span s = span_from_to(r, start_line, start_col, start_off, r->pos);
        diag_emit(DIAG_ERROR, s, "unterminated string literal");
        r->error = true;
        return NULL;
    }

    char *buf = (char *)arena_alloc_aligned(r->arena, out_len + 1, 1);
    size_t bi = 0;
    while (peek(r) != -1 && peek(r) != '"') {
        int c = advance(r);
        if (c == '\\') {
            int e = advance(r);
            char out;
            /* r7rs-lang-plan R1: the R7RS 6.7 escape set.  `\a`, `\b` and
             * `\|` are new; `\x<hex>;` takes any scalar value and is written
             * back as UTF-8; `\<intraline ws>*<newline><intraline ws>*` is
             * the line continuation and produces nothing.  `\0` is Turmeric's
             * and is not Scheme, so it falls through to the shared table
             * (accepting it costs nothing and keeps the reader a variant). */
            if (r->scheme_enabled) {
                if (e == 'a') { buf[bi++] = 7; continue; }
                if (e == 'b') { buf[bi++] = 8; continue; }
                if (e == '|') { buf[bi++] = '|'; continue; }
                if (e == 'x' || e == 'X') {
                    uint32_t cp = 0; int nd = 0, d;
                    while ((d = hex_digit(peek(r))) >= 0) {
                        if (nd < 8) cp = cp * 16 + (uint32_t)d;
                        advance(r); nd++;
                    }
                    if (nd == 0 || peek(r) != ';' || nd > 8 || cp > 0x10FFFF) {
                        diag_emit(DIAG_ERROR, span_point(r),
                                  "malformed '\\x<hex>;' string escape: expected 1-8 "
                                  "hex digits (at most 10FFFF) followed by ';'");
                        r->error = true;
                        return NULL;
                    }
                    advance(r); /* ';' */
                    bi += (size_t)utf8_encode(cp, buf + bi);
                    continue;
                }
                if (e == ' ' || e == '\t' || e == '\n' || e == '\r') {
                    /* Line continuation: optional intraline whitespace, the
                     * line ending, then the next line's leading whitespace. */
                    int w = e;
                    while (w == ' ' || w == '\t') w = advance(r);
                    if (w == '\r' && peek(r) == '\n') advance(r);
                    if (w != '\n' && w != '\r') {
                        diag_emit(DIAG_ERROR, span_point(r),
                                  "'\\' followed by whitespace must continue "
                                  "onto the next line (R7RS line continuation)");
                        r->error = true;
                        return NULL;
                    }
                    while (peek(r) == ' ' || peek(r) == '\t') advance(r);
                    continue;
                }
            }
            switch (e) {
                case 'n':  out = '\n'; break;
                case 't':  out = '\t'; break;
                case 'r':  out = '\r'; break;
                case '0':  out = '\0'; break;
                case '\\': out = '\\'; break;
                case '"':  out = '"';  break;
                default:
                    diag_emit(DIAG_ERROR, span_point(r), "unknown string escape '\\%c'", e);
                    r->error = true;
                    out = (char)e;
            }
            buf[bi++] = out;
        } else {
            buf[bi++] = (char)c;
        }
    }
    advance(r); /* closing " */
    buf[bi] = '\0';

    Span span = span_from_to(r, start_line, start_col, start_off, r->pos);
    Form *f = form_new(r->arena, F_STR, span);
    f->as.s.p = buf;
    f->as.s.len = (uint32_t)bi;
    
    /* Phase S2: Check for neoteric bracket immediately following string */
    if (r->neoteric_enabled) {
        int bracket = peek_neoteric_bracket(r);
        if (bracket != -1) {
            return read_neoteric_bracket(r, f, bracket);
        }
    }
    
    return f;
}

/* r7rs-lang-plan R1: write `cp` as UTF-8 into `out` (up to 4 bytes) and
 * return how many bytes were written.  Callers range-check `cp` first. */
static int utf8_encode(uint32_t cp, char *out) {
    if (cp < 0x80) { out[0] = (char)cp; return 1; }
    if (cp < 0x800) {
        out[0] = (char)(0xC0 | (cp >> 6));
        out[1] = (char)(0x80 | (cp & 0x3F));
        return 2;
    }
    if (cp < 0x10000) {
        out[0] = (char)(0xE0 | (cp >> 12));
        out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[2] = (char)(0x80 | (cp & 0x3F));
        return 3;
    }
    out[0] = (char)(0xF0 | (cp >> 18));
    out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
    out[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
    out[3] = (char)(0x80 | (cp & 0x3F));
    return 4;
}

static int hex_digit(int c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return 10 + c - 'a';
    if (c >= 'A' && c <= 'F') return 10 + c - 'A';
    return -1;
}

/* Accumulate one digit of a numeric literal into an unsigned magnitude.
 * The accumulation is unsigned so it is always well defined -- the old signed
 * `ival = ival * 10 + digit` was UB the moment a literal ran past int64, which
 * is also how an out-of-range literal used to wrap silently instead of being
 * diagnosed. Once `*ovf` is set the magnitude is pinned at UINT64_MAX and no
 * further digit changes it; the caller reports the range error after the type
 * suffix has been read. */
static void mag_push(uint64_t *mag, bool *ovf, uint64_t base, uint64_t digit) {
    if (*ovf) return;
    if (*mag > (UINT64_MAX - digit) / base) { *ovf = true; *mag = UINT64_MAX; return; }
    *mag = *mag * base + digit;
}

/* strtod over the delimited slice [start, end) of the source, arena-allocating
 * only when the slice does not fit the stack buffer. */
static double read_slice_double(Reader *r, size_t start, size_t end) {
    size_t num_len = end - start;
    char stackbuf[64];
    char *nb = stackbuf;
    if (num_len >= sizeof(stackbuf)) {
        nb = (char *)arena_alloc(r->arena, num_len + 1);
    }
    memcpy(nb, r->src + start, num_len);
    nb[num_len] = '\0';
    return strtod(nb, NULL);
}

static Form *read_number(Reader *r, int sign) {
    uint32_t start_line = r->line;
    uint32_t start_col = r->col;
    size_t start_off = r->pos;
    if (sign != 0) {
        /* sign already at peek; consume it for the lexeme range */
        advance(r);
    }

    bool is_float = false;
    double fval = 0.0;
    int64_t ival = 0;
    bool any = false;
    /* The literal's magnitude, accumulated unsigned and range-checked once the
     * type suffix is known. `sign` is applied at the very end, which is why
     * INT64_MIN's magnitude (INT64_MAX + 1) has to survive the accumulation. */
    uint64_t mag = 0;
    bool mag_overflow = false;
    /* A 0x / 0b literal is a bit pattern, not a magnitude: the full 64-bit
     * range is its own, so 0xFFFFFFFFFFFFFFFF keeps meaning -1. */
    bool bitpattern = false;
    /* The delimited decimal lexeme, for recovering a float value with strtod. */
    size_t num_start = 0, num_end = 0;

    /* 0x / 0b prefixes - these are always integers */
    if (peek(r) == '0' && (peek2(r) == 'x' || peek2(r) == 'X')) {
        advance(r); advance(r);
        bitpattern = true;
        int d;
        while ((d = hex_digit(peek(r))) >= 0) {
            mag_push(&mag, &mag_overflow, 16, (uint64_t)d);
            advance(r);
            any = true;
        }
    } else if (peek(r) == '0' && (peek2(r) == 'b' || peek2(r) == 'B')) {
        advance(r); advance(r);
        bitpattern = true;
        while (peek(r) == '0' || peek(r) == '1') {
            mag_push(&mag, &mag_overflow, 2, (uint64_t)(advance(r) - '0'));
            any = true;
        }
    } else {
        /* Decimal number. The loops below only *delimit* the lexeme (advance
         * the cursor, accumulate ival for the integer/suffix paths, and set
         * is_float). The float value is recovered with strtod over the matched
         * characters -- a hand-rolled digit accumulator drifts from the
         * correctly-rounded double and previously dropped negative exponents
         * outright (e.g. 1e-10 parsed as 1.0). See
         * docs/reader-float-parsing-plan.md. */
        num_start = r->pos;

        /* Parse integer part */
        while (peek(r) >= '0' && peek(r) <= '9') {
            int digit = advance(r) - '0';
            mag_push(&mag, &mag_overflow, 10, (uint64_t)digit);
            any = true;
        }

        /* Check for fractional part */
        if (peek(r) == '.') {
            is_float = true;
            advance(r);  /* consume '.' */
            while (peek(r) >= '0' && peek(r) <= '9') {
                advance(r);
                any = true;
            }
        }

        /* Check for exponent part */
        if ((peek(r) == 'e' || peek(r) == 'E') && any) {
            is_float = true;
            advance(r);  /* consume 'e' or 'E' */
            if (peek(r) == '+' || peek(r) == '-') {
                advance(r);
            }
            bool exp_any = false;
            while (peek(r) >= '0' && peek(r) <= '9') {
                advance(r);
                exp_any = true;
            }
            if (!exp_any) {
                Span s = span_point(r);
                diag_emit(DIAG_ERROR, s, "expected exponent digits in float literal");
                r->error = true;
                return NULL;
            }
        }

        /* Recover the float value from the delimited lexeme [num_start, pos).
         * The type suffix (f32/f64) has not been consumed yet, so the slice is
         * pure decimal-float syntax and strtod stops exactly at its end. The
         * leading sign is applied below via the `sign < 0` negation. */
        num_end = r->pos;
        if (is_float) {
            fval = read_slice_double(r, num_start, num_end);
        }
    }

    if (!any) {
        Span s = span_point(r);
        diag_emit(DIAG_ERROR, s, "expected digits in numeric literal");
        r->error = true;
        return NULL;
    }

    /* Phase N: Scan optional type suffix (i8, i16, i32, i64, u8, u16, u32, u64, f32, f64). */
    LiteralSuffix lit_suf = LIT_SUF_NONE;
    if (!is_float) {
        /* Integer suffixes */
        if      (peek(r) == 'i' && peek2(r) == '8'  && !is_sym_cont(peek_at(r, 2))) { advance(r); advance(r); lit_suf = LIT_SUF_I8; }
        else if (peek(r) == 'i' && peek2(r) == '1'  && peek3(r) == '6' && !is_sym_cont(peek_at(r, 3))) { advance(r); advance(r); advance(r); lit_suf = LIT_SUF_I16; }
        else if (peek(r) == 'i' && peek2(r) == '3'  && peek3(r) == '2' && !is_sym_cont(peek_at(r, 3))) { advance(r); advance(r); advance(r); lit_suf = LIT_SUF_I32; }
        else if (peek(r) == 'i' && peek2(r) == '6'  && peek3(r) == '4' && !is_sym_cont(peek_at(r, 3))) { advance(r); advance(r); advance(r); lit_suf = LIT_SUF_I64; }
        else if (peek(r) == 'u' && peek2(r) == '8'  && !is_sym_cont(peek_at(r, 2))) { advance(r); advance(r); lit_suf = LIT_SUF_U8; }
        else if (peek(r) == 'u' && peek2(r) == '1'  && peek3(r) == '6' && !is_sym_cont(peek_at(r, 3))) { advance(r); advance(r); advance(r); lit_suf = LIT_SUF_U16; }
        else if (peek(r) == 'u' && peek2(r) == '3'  && peek3(r) == '2' && !is_sym_cont(peek_at(r, 3))) { advance(r); advance(r); advance(r); lit_suf = LIT_SUF_U32; }
        else if (peek(r) == 'u' && peek2(r) == '6'  && peek3(r) == '4' && !is_sym_cont(peek_at(r, 3))) { advance(r); advance(r); advance(r); lit_suf = LIT_SUF_U64; }
        /* Float suffixes on integer-looking literals (e.g. 1f32) */
        /* `1f32` / `1f64`: an integer lexeme with a float suffix. Recover the
         * value with strtod over the delimited digits, not from `mag` -- a
         * magnitude past 64 bits saturates, and 99999999999999999999f64 is a
         * perfectly representable double. `0b1f32` has no decimal slice to
         * read (and `0x` swallows `f` as a digit), so a bit-pattern lexeme
         * falls back to its magnitude. */
        #define TUR_INT_LEXEME_AS_DOUBLE() \
            (num_end > num_start ? read_slice_double(r, num_start, num_end) : (double)mag)
        else if (peek(r) == 'f' && peek2(r) == '3'  && peek3(r) == '2' && !is_sym_cont(peek_at(r, 3))) { advance(r); advance(r); advance(r); lit_suf = LIT_SUF_F32; is_float = true; fval = TUR_INT_LEXEME_AS_DOUBLE(); }
        else if (peek(r) == 'f' && peek2(r) == '6'  && peek3(r) == '4' && !is_sym_cont(peek_at(r, 3))) { advance(r); advance(r); advance(r); lit_suf = LIT_SUF_F64; is_float = true; fval = TUR_INT_LEXEME_AS_DOUBLE(); }
        #undef TUR_INT_LEXEME_AS_DOUBLE
    } else {
        /* Float suffixes */
        if      (peek(r) == 'f' && peek2(r) == '3'  && peek3(r) == '2' && !is_sym_cont(peek_at(r, 3))) { advance(r); advance(r); advance(r); lit_suf = LIT_SUF_F32; }
        else if (peek(r) == 'f' && peek2(r) == '6'  && peek3(r) == '4' && !is_sym_cont(peek_at(r, 3))) { advance(r); advance(r); advance(r); lit_suf = LIT_SUF_F64; }
    }


    Span span = span_from_to(r, start_line, start_col, start_off, r->pos);
    Form *atom;

    if (is_float) {
        if (sign < 0) fval = -fval;
        atom = form_float(r->arena, span, fval);
    } else {
        /* Range-check the magnitude before it becomes a value. The sized
         * suffixes below check themselves against the (exact) int64 value, so
         * defer to them whenever the literal at least fits 64 bits -- for
         * `300i8`, "overflows int8 range" is the message that helps. What is
         * left is the default int64 path, which had no check at all and so
         * turned 9223372036854775808 into INT64_MIN without a word. */
        bool sized_suffix = (lit_suf != LIT_SUF_NONE && lit_suf != LIT_SUF_I64);
        bool unsigned_target = (lit_suf == LIT_SUF_U8  || lit_suf == LIT_SUF_U16 ||
                                lit_suf == LIT_SUF_U32 || lit_suf == LIT_SUF_U64);
        uint64_t mag_bound = (bitpattern || unsigned_target)
                             ? UINT64_MAX
                             : (sign < 0 ? (uint64_t)INT64_MAX + 1u : (uint64_t)INT64_MAX);
        if (mag_overflow || (!sized_suffix && mag > mag_bound)) {
            diag_emit(DIAG_ERROR, span,
                      "integer literal overflows int64 range "
                      "(-9223372036854775808..9223372036854775807)");
            r->error = true;
            return NULL;
        }
        /* Apply the sign without negating a signed int64: `~mag + 1` is the
         * two's-complement negation done in unsigned arithmetic, which is
         * defined for INT64_MIN's magnitude where `-ival` was UB. */
        ival = (sign < 0) ? (int64_t)(~mag + 1u) : (int64_t)mag;
        atom = form_int(r->arena, span, ival);
        /* Overflow checks for small integer types */
        if (lit_suf == LIT_SUF_I8 && (ival < -128 || ival > 127)) {
            diag_emit(DIAG_ERROR, span, "integer literal overflows int8 range (-128..127)");
            r->error = true;
            return NULL;
        }
        if (lit_suf == LIT_SUF_I16 && (ival < -32768 || ival > 32767)) {
            diag_emit(DIAG_ERROR, span, "integer literal overflows int16 range (-32768..32767)");
            r->error = true;
            return NULL;
        }
        if (lit_suf == LIT_SUF_I32 && (ival < -2147483648LL || ival > 2147483647LL)) {
            diag_emit(DIAG_ERROR, span, "integer literal overflows int32 range");
            r->error = true;
            return NULL;
        }
        if (lit_suf == LIT_SUF_U8 && (ival < 0 || ival > 255)) {
            diag_emit(DIAG_ERROR, span, "integer literal overflows uint8 range (0..255)");
            r->error = true;
            return NULL;
        }
        if (lit_suf == LIT_SUF_U16 && (ival < 0 || ival > 65535)) {
            diag_emit(DIAG_ERROR, span, "integer literal overflows uint16 range (0..65535)");
            r->error = true;
            return NULL;
        }
        if (lit_suf == LIT_SUF_U32 && (ival < 0 || ival > 4294967295LL)) {
            diag_emit(DIAG_ERROR, span, "integer literal overflows uint32 range (0..4294967295)");
            r->error = true;
            return NULL;
        }
        if (lit_suf == LIT_SUF_U64 && ival < 0) {
            diag_emit(DIAG_ERROR, span, "integer literal overflows uint64 range (must be non-negative)");
            r->error = true;
            return NULL;
        }
    }
    atom->lit_suffix = lit_suf;


    /* Phase S2: Check for neoteric bracket immediately following number */
    if (r->neoteric_enabled) {
        int bracket = peek_neoteric_bracket(r);
        if (bracket != -1) {
            return read_neoteric_bracket(r, atom, bracket);
        }
    }
    
    return atom;
}

/* A user Scheme source: read by the Scheme reader, and neither Turmeric-shaped
 * nor inside a Turmeric data literal.  Where this holds, two Turmeric lexemes
 * give way to Scheme's (docs/reported/r7rs-turmeric-syntax-leaks.md):
 *
 *   - a token that starts with `:` is an identifier (R7RS 7.1.1 makes `:` an
 *     <initial>), as the runtime `read` has always read it -- so `':x` is the
 *     symbol `:x`, `:::` can be a custom ellipsis, and SRFI 42's `:range` can
 *     be defined (docs/archive/r7rs-leading-colon-identifiers.md).  A Scheme
 *     program that passes a key to a Turmeric map writes the symbol `'k`,
 *     which is the same runtime value the keyword `:k` is;
 *   - `[...]` is a list, as Racket, Chez and Guile read it (R7RS reserves the
 *     brackets), not Turmeric's vector.
 *
 * The Turmeric-shaped Scheme sources keep Turmeric's lexemes: the prelude and
 * the on-demand library files under stdlib/r7rs/ (`(defstruct R7rsPair :heap
 * [a : any d : any])`, `(:: c :int)`), and a synthetic `<eval>` source up to
 * the REPL's pinned preload -- the same test as scheme_lower.c's
 * prelude_span.  So does the inside of a Turmeric data literal (`#map{:k 1}`),
 * whose keys are keywords by its own grammar. */
static bool scheme_user_source(const Reader *r) {
    if (!r->scheme_enabled || r->turmeric_literal_depth) return false;
    const char *p = r->file ? r->file->path : NULL;
    if (!p) return true;
    if (p[0] == '<') return g_synthetic_user_from_line && r->line >= g_synthetic_user_from_line;
    size_t n = strlen(p);
    static const char SUFFIX[] = "r7rs/prelude.tur";
    size_t m = sizeof SUFFIX - 1;
    if (n >= m && memcmp(p + n - m, SUFFIX, m) == 0) return false;
    return strstr(p, "stdlib/r7rs/") == NULL;
}

#define SCHEME_SEAM_HELP \
    "write Turmeric code in a Turmeric module and import it with (turmeric <module>)"
/* r7rs-turmeric-syntax-leaks items 2, 3 and 6: a Turmeric lexeme in user
 * Scheme source -- a `#map{...}`-family literal, `#?(...)`, inline C, `@` --
 * is refused, naming the Scheme spelling or the seam: Turmeric code belongs in
 * a Turmeric module the program imports with `(turmeric <module>)`.  Reports
 * at the lexeme's start and stops the read, as any reader error does. */
static Form *scheme_refuse_lexeme(Reader *r, const char *what, const char *instead) {
    diag_emit(DIAG_ERROR, span_point(r), "%s is Turmeric syntax, not Scheme; %s", what, instead);
    r->error = true;
    return NULL;
}
static bool scheme_is_delim(int c);
static Form *scheme_refuse_hash(Reader *r) {
    /* peek(r) == '#', and no Scheme `#` form (`#t`, `#(`, `#u8(`, `#x`...,
     * `#\`, `#;`) claimed it. */
    static const char SEAM[] = SCHEME_SEAM_HELP;
    if (peek2(r) == 'm' && peek3(r) == 'a' && peek_at(r, 3) == 'p' && peek_at(r, 4) == '{')
        return scheme_refuse_lexeme(r, "`#map{...}`",
            "build the map with map-assoc on (map-new), from (turmeric stdlib/map)");
    if (peek2(r) == 's' && peek3(r) == 'e' && peek_at(r, 3) == 't' && peek_at(r, 4) == '{')
        return scheme_refuse_lexeme(r, "`#set{...}`",
            "build the set with set-add on (set-new), from (turmeric stdlib/set)");
    if (peek2(r) == 'r' && peek3(r) == 'a' && peek_at(r, 3) == 't' && peek_at(r, 4) == '{')
        return scheme_refuse_lexeme(r, "`#rat{...}`",
            "R7RS writes an exact ratio as n/d, e.g. 3/4");
    if (peek2(r) == 'c' && peek3(r) == 'x' && peek_at(r, 3) == '{')
        return scheme_refuse_lexeme(r, "`#cx{...}`",
            "R7RS writes a complex number as a+bi, e.g. 1+2i, or (make-rectangular a b)");
    if (peek2(r) == '?')
        return scheme_refuse_lexeme(r, "the reader conditional `#?(...)`",
            "use cond-expand (R7RS 4.2.1)");
    char tag[24];
    size_t k = 1, t = 0;
    while (t + 1 < sizeof tag && peek_at(r, k) != -1 && !scheme_is_delim(peek_at(r, k)) &&
           peek_at(r, k) != '{' && peek_at(r, k) != '[')
        tag[t++] = (char)peek_at(r, k++);
    tag[t] = '\0';
    char what[48];
    snprintf(what, sizeof what, "`#%s`", tag);
    return scheme_refuse_lexeme(r, what, SEAM);
}

static Form *read_keyword(Reader *r) {
    uint32_t start_line = r->line;
    uint32_t start_col = r->col;
    size_t start_off = r->pos;
    advance(r); /* consume first ':' */

    /* Phase HRT1: '::' is the type ascription operator — emit as a symbol named "::" */
    if (peek(r) == ':') {
        advance(r); /* consume second ':' */
        size_t end = r->pos;
        Span span = span_from_to(r, start_line, start_col, start_off, end);
        StrSlice name = strslice(r->src + start_off, (uint32_t)(end - start_off)); /* "::" */
        const Symbol *sym = symtab_intern(r->st, name);
        return form_sym(r->arena, span, sym);
    }

    /* `: type-expr` — space-separated or fused-paren compound type annotation */
    {
        int c2 = peek(r);
        if (c2 == ' ' || c2 == '\t' || c2 == '\n' || c2 == '\r'
            || c2 == '(' || c2 == '[' || c2 == -1) {
            skip_ws_and_comments(r);
            if (peek(r) == -1) {
                Span s = span_from_to(r, start_line, start_col, start_off, r->pos);
                diag_emit(DIAG_ERROR, s, "expected type expression after ':'");
                r->error = true;
                return NULL;
            }
            Form *inner = read_form(r);
            if (!inner) return NULL;
            size_t end = r->pos;
            Span sp = span_from_to(r, start_line, start_col, start_off, end);
            return form_type_ann(r->arena, sp, inner);
        }
    }

    if (!is_sym_cont(peek(r)) && !isalpha(peek(r))) {
        /* Phase G1: bare ':' (not followed by an identifier) is a type-annotation
         * separator symbol used in defgadt constructor forms.  Emit it as F_SYM(":")
         * instead of an error so that `(CtorName field : return-type)` parses. */
        size_t end = r->pos;
        Span span = span_from_to(r, start_line, start_col, start_off, end);
        StrSlice name = strslice(r->src + start_off, (uint32_t)(end - start_off)); /* ":" */
        const Symbol *sym = symtab_intern(r->st, name);
        return form_sym(r->arena, span, sym);
    }
    while (is_sym_cont(peek(r))) advance(r);
    size_t end = r->pos;
    Span span = span_from_to(r, start_line, start_col, start_off, end);
    /* keyword name is the slice after the colon */
    StrSlice name = strslice(r->src + start_off + 1, (uint32_t)(end - start_off - 1));
    const Symbol *sym = symtab_intern(r->st, name);
    return form_keyword(r->arena, span, sym);
}

static Form *read_quote(Reader *r) {
    /* Phase 6: ('x) reader macro - read as (quote x) */
    uint32_t start_line = r->line;
    uint32_t start_col = r->col;
    size_t start_off = r->pos;
    advance(r); /* consume '\'' */
    skip_ws_and_comments(r);
    
    if (peek(r) == -1) {
        Span s = span_from_to(r, start_line, start_col, start_off, r->pos);
        diag_emit(DIAG_ERROR, s, "' requires an expression after it");
        r->error = true;
        return NULL;
    }
    
    Form *inner = read_form(r);
    if (!inner) return NULL;
    
    /* Create a quote form */
    return form_quote(r->arena, span_from_to(r, start_line, start_col, start_off, inner->span.off_end), inner);
}

/* Phase 6: Read unquote (~) or unquote-splicing (~@) - only valid inside
 * quasiquote.  r7rs-lang-plan R1: the Scheme reader spells the same two
 * forms `,` and `,@`; `prefix` is the character that introduced them, for
 * the diagnostics. */
static Form *read_unquote_with(Reader *r, char prefix) {
    uint32_t start_line = r->line;
    uint32_t start_col = r->col;
    size_t start_off = r->pos;
    advance(r); /* consume the prefix */
    
    /* Check for ~@ / ,@ (unquote-splicing) */
    if (peek(r) == '@') {
        advance(r); /* consume '@' */
        skip_ws_and_comments(r);
        
        if (peek(r) == -1) {
            Span s = span_from_to(r, start_line, start_col, start_off, r->pos);
            diag_emit(DIAG_ERROR, s, "%c@ requires an expression after it", prefix);
            r->error = true;
            return NULL;
        }
        
        Form *inner = read_form(r);
        if (!inner) return NULL;
        
        /* Create an unquote-splicing form */
        return form_unquote_splicing(r->arena, span_from_to(r, start_line, start_col, start_off, inner->span.off_end), inner);
    }
    
    skip_ws_and_comments(r);
    
    if (peek(r) == -1) {
        Span s = span_from_to(r, start_line, start_col, start_off, r->pos);
        diag_emit(DIAG_ERROR, s, "%c requires an expression after it", prefix);
        r->error = true;
        return NULL;
    }
    
    Form *inner = read_form(r);
    if (!inner) return NULL;
    
    /* Create an unquote form */
    return form_unquote(r->arena, span_from_to(r, start_line, start_col, start_off, inner->span.off_end), inner);
}

static Form *read_unquote(Reader *r) {
    return read_unquote_with(r, '~');
}

static Form *read_quasiquote(Reader *r) {
    /* Phase 6: (`x) reader macro - read as (quasiquote x) */
    uint32_t start_line = r->line;
    uint32_t start_col = r->col;
    size_t start_off = r->pos;
    advance(r); /* consume '`' */
    skip_ws_and_comments(r);
    
    if (peek(r) == -1) {
        Span s = span_from_to(r, start_line, start_col, start_off, r->pos);
        diag_emit(DIAG_ERROR, s, "` requires an expression after it");
        r->error = true;
        return NULL;
    }
    
    Form *inner = read_form(r);
    if (!inner) return NULL;
    
    /* Create a quasiquote form */
    return form_quasiquote(r->arena, span_from_to(r, start_line, start_col, start_off, inner->span.off_end), inner);
}

static Form *read_at(struct Reader *r) {
    /* '@' prefix has two roles:
     *   1) Deref sugar: @x / @ x -> (deref x)
     *   2) Effect-row annotation sugar: @{...} / @ {...} -> F_MAP {...}
     */
    uint32_t start_line = r->line;
    uint32_t start_col = r->col;
    size_t start_off = r->pos;
    advance(r); /* consume '@' */
    skip_ws_and_comments(r);

    /* Effect-row annotation sugar for signatures.
     * fx-row-syntax-rename-plan Phase 1: tag with PROV_FX_AT_LEGACY so elab
     * emits TUR-D0003 on first consumption. */
    if (peek(r) == '{') {
        Form *m = read_seq(r, '{', '}', F_MAP, "unterminated effect row (missing '}')");
        if (m && m->tag == F_MAP) m->fx_prov = (uint8_t)PROV_FX_AT_LEGACY;
        return m;
    }

    if (peek(r) == -1) {
        Span s = span_from_to(r, start_line, start_col, start_off, r->pos);
        diag_emit(DIAG_ERROR, s, "@ requires an expression after it");
        r->error = true;
        return NULL;
    }
    
    Form *inner = read_form(r);
    if (!inner) return NULL;
    
    /* Create a list: (deref inner). */
    Form **items = (Form **)arena_alloc(r->arena, 2 * sizeof(Form *));
    const Symbol *deref_sym = symtab_intern(r->st, strslice("deref", 5));
    items[0] = form_sym(r->arena, span_from_to(r, start_line, start_col, start_off, start_off + 1), deref_sym);
    items[1] = inner;
    
    Span span = span_from_to(r, start_line, start_col, start_off, inner->span.off_end);
    return form_list(r->arena, span, items, 2);
}

/* Phase 12: & / &mut borrow prefix sugar.
 *
 * Rules:
 *   '&' followed by whitespace/EOF/closer → bare '&' symbol (preserves (& x) form)
 *   '&' followed by 'm','u','t' + delimiter → (&mut <next-form>) sugar
 *   '&' followed by anything else           → (& <next-form>) sugar
 *
 * This means:
 *   (& x)    still works: '& ' has space → bare symbol, list reads normally
 *   &x       → (& x)
 *   &mut x   → (&mut x)   [note: (&mut x) explicit form must be written as &mut x]
 */
static bool is_borrow_no_sugar(int c) {
    /* Characters after '&' that suppress sugar and return a bare '&' symbol */
    if (c == -1) return true;
    return (c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == ',' ||
            c == ')' || c == ']' || c == '}' || c == ';' || c == '"');
}

static bool is_token_delim(int c) {
    /* Characters that end a token — used to check what follows 'mut' */
    if (c == -1) return true;
    return (c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == ',' ||
            c == '(' || c == ')' || c == '[' || c == ']' || c == '{' || c == '}' ||
            c == ';' || c == '"' || c == ':' || c == '@' || c == '`' || c == '~');
}

static Form *read_borrow(Reader *r) {
    uint32_t start_line = r->line;
    uint32_t start_col = r->col;
    size_t start_off = r->pos;
    advance(r); /* consume '&' */

    /* No-sugar: '&' followed by whitespace/closer → return bare '&' symbol.
     * This preserves the (& x) explicit call form. */
    if (is_borrow_no_sugar(peek(r))) {
        Span span = span_from_to(r, start_line, start_col, start_off, r->pos);
        const Symbol *sym = symtab_intern(r->st, strslice("&", 1));
        return form_sym(r->arena, span, sym);
    }

    /* Check for &mut sugar: next chars 'm','u','t' + token delimiter */
    int c1 = peek(r), c2 = peek2(r), c3 = peek3(r);
    int c4 = (r->pos + 3 < r->len) ? (unsigned char)r->src[r->pos + 3] : -1;
    bool is_mut = (c1 == 'm' && c2 == 'u' && c3 == 't' && is_token_delim(c4));

    const char *op_str;
    uint32_t op_len;
    if (is_mut) {
        advance(r); advance(r); advance(r); /* consume 'm','u','t' */
        op_str = "&mut";
        op_len = 4;
    } else {
        op_str = "&";
        op_len = 1;
    }
    Span op_span = span_from_to(r, start_line, start_col, start_off, r->pos);

    /* Allow whitespace between operator and operand (e.g. &mut x) */
    skip_ws_and_comments(r);

    /* LS1: Rust-style lifetime prefix on a borrow *type*, e.g. &'a int or
     * &mut 'a int.  An apostrophe here begins a lifetime symbol ('a, 'b, ...);
     * read it as a distinct token and thread it into the produced list as a
     * middle element -- (& 'a int) / (&mut 'a int).  The lifetime is only
     * meaningful in type-annotation position; elab_types.c interprets it there,
     * and the borrow-expression path ignores a stray lifetime element. */
    Form *lifetime = NULL;
    if (peek(r) == '\'') {
        uint32_t lt_line = r->line;
        uint32_t lt_col = r->col;
        size_t lt_off = r->pos;
        advance(r); /* consume the apostrophe */
        while (is_sym_cont(peek(r))) advance(r);
        Span lt_span = span_from_to(r, lt_line, lt_col, lt_off, r->pos);
        const Symbol *lt_sym = symtab_intern(r->st, strslice(r->src + lt_off, r->pos - lt_off));
        lifetime = form_sym(r->arena, lt_span, lt_sym);
        skip_ws_and_comments(r);
    }

    if (peek(r) == -1 || peek(r) == ')' || peek(r) == ']' || peek(r) == '}') {
        Span s = span_from_to(r, start_line, start_col, start_off, r->pos);
        diag_emit(DIAG_ERROR, s, "%s requires an expression after it", op_str);
        r->error = true;
        return NULL;
    }

    Form *inner = read_form(r);
    if (!inner) return NULL;

    const Symbol *op_sym = symtab_intern(r->st, strslice(op_str, op_len));
    if (lifetime != NULL) {
        Form **items = (Form **)arena_alloc(r->arena, 3 * sizeof(Form *));
        items[0] = form_sym(r->arena, op_span, op_sym);
        items[1] = lifetime;
        items[2] = inner;
        Span span = span_from_to(r, start_line, start_col, start_off, inner->span.off_end);
        return form_list(r->arena, span, items, 3);
    }
    Form **items = (Form **)arena_alloc(r->arena, 2 * sizeof(Form *));
    items[0] = form_sym(r->arena, op_span, op_sym);
    items[1] = inner;

    Span span = span_from_to(r, start_line, start_col, start_off, inner->span.off_end);
    return form_list(r->arena, span, items, 2);
}

static Form *read_symbol_or_minus_at(Reader *r, bool head_pos) {
    uint32_t start_line = r->line;
    uint32_t start_col = r->col;
    size_t start_off = r->pos;

    /* Special case: '-' followed by digit is a negative integer. */
    if (peek(r) == '-' && peek2(r) >= '0' && peek2(r) <= '9') {
        return read_number(r, -1);
    }
    /* r7rs-lang-plan R1: Scheme also has an explicit `+` sign (`+5`) and a
     * sign before a bare fraction (`-.5`, `+.5`); Turmeric reads `+5` as a
     * symbol, so these are Scheme-only. */
    if (r->scheme_enabled && (peek(r) == '+' || peek(r) == '-') &&
        ((peek2(r) >= '0' && peek2(r) <= '9') ||
         (peek2(r) == '.' && peek3(r) >= '0' && peek3(r) <= '9'))) {
        return read_number(r, peek(r) == '-' ? -1 : 1);
    }

    /* R3: `:` is an ordinary identifier character in Scheme once past the
     * first byte (`v:vec-new`, the conventional `(prefix ...)` spelling); at
     * token start it is still Turmeric's keyword, which read_form owns. */
    while (is_sym_cont(peek(r)) ||
           (r->scheme_enabled && (scheme_sym_extra(peek(r)) || peek(r) == ':'))) advance(r);
    size_t end = r->pos;
    if (end == start_off) {
        Span s = span_point(r);
        diag_emit(DIAG_ERROR, s, "unexpected character '%c'", (char)peek(r));
        r->error = true;
        advance(r);
        return NULL;
    }

    Span span = span_from_to(r, start_line, start_col, start_off, end);
    StrSlice name = strslice(r->src + start_off, (uint32_t)(end - start_off));

    if (r->scheme_enabled) {
        /* R7RS 6.2.5: the inexact infinities and NaN are peculiar
         * identifiers that read as numbers. */
        if (name.len == 6 && (memcmp(name.p, "+inf.0", 6) == 0 ||
                              memcmp(name.p, "-inf.0", 6) == 0))
            return form_float(r->arena, span, name.p[0] == '-' ? -INFINITY : INFINITY);
        if (name.len == 6 && (memcmp(name.p, "+nan.0", 6) == 0 ||
                              memcmp(name.p, "-nan.0", 6) == 0))
            return form_float(r->arena, span, NAN);
        /* `#!fold-case`: an identifier is read as if downcased.  `|...|`
         * symbols are exempt and never come through here. */
        if (r->fold_case) {
            char *lower = (char *)arena_alloc_aligned(r->arena, name.len + 1, 1);
            for (uint32_t i = 0; i < name.len; i++)
                lower[i] = (char)tolower((unsigned char)name.p[i]);
            lower[name.len] = '\0';
            name = strslice(lower, name.len);
        }
    }

    /* Recognize literal keywords -- except in user Scheme source, where
     * `true`, `false` and `nil` are ordinary identifiers (R7RS spells the
     * booleans `#t`/`#f` and has no nil; r7rs-turmeric-syntax-leaks item 5). */
    Form *word = NULL;
    if (scheme_user_source(r))
        ;
    else if (name.len == 3 && memcmp(name.p, "nil", 3) == 0)
        word = form_nil(r->arena, span);
    else if (name.len == 4 && memcmp(name.p, "true", 4) == 0)
        word = form_bool(r->arena, span, true);
    else if (name.len == 5 && memcmp(name.p, "false", 5) == 0)
        word = form_bool(r->arena, span, false);
    if (word) {
        if (r->scheme_enabled) word->fx_prov = PROV_SCHEME_WORD;   /* R10 */
        return word;
    }

    /* sweet-dollar-inside-brackets-is-a-silent-symbol: `$` is the sweet-exp
     * rest-of-line marker, and sweet_emit_content rewrites it only where the
     * indentation layer is live (`bd == 0`).  Inside `(...)`, `[...]` or
     * `{...}` it declines, and the token used to arrive here as an ordinary
     * symbol named `$` -- silently giving the enclosing form an extra element.
     * Every other decline path rewrites to something (`$` at EOL or before a
     * comment wraps an empty rest, `$x` is the symbol `$x`), so a BARE `$`
     * reaching the reader from a sweet file means exactly one thing. */
    if (r->file != NULL && (r->file->reader_type == READER_SWEET ||
                            r->file->reader_type == READER_R7RS_SWEET) &&
        name.len == 1 && name.p[0] == '$') {
        diag_emit_with_code(DIAG_ERROR, span, TUR_E0332_SWEET_DOLLAR_IN_BRACKETS,
                            "`$` has no meaning inside brackets -- the "
                            "rest-of-line marker only applies where "
                            "indentation is significant; write the call with a "
                            "delimiter instead, as `f(x)` or `(f x)`");
        r->error = true;
        return NULL;
    }

    /* proper-tail-calls T1 (docs/archive/proper-tail-calls-plan.md, T-D1):
     * `^tailcall` is a PREFIX annotation on the expression that follows it,
     * so `^tailcall (loop v)` reads as `(^tailcall (loop v))`.  It has to be
     * a prefix rather than an extra element because the shapes that most want
     * it -- a `match` or `handle` arm -- pair up two forms at a time, and an
     * extra element there silently re-pairs every clause after it.
     *
     * Suppressed in list-head position (`head_pos`), which is what lets the
     * explicit `(^tailcall (loop v))` spelling read as itself; the sweet-exp
     * indentation layer produces that spelling from a `^tailcall`-led line. */
    /* In user Scheme source `^tailcall` is an identifier (`^` is an R7RS
     * <initial>), and every call in tail position is a proper tail call
     * already (r7rs-turmeric-syntax-leaks item 4). */
    if (!head_pos && name.len == 9 && memcmp(name.p, "^tailcall", 9) == 0 &&
        !scheme_user_source(r)) {
        Span op_span = span;
        skip_ws_and_comments(r);
        if (peek(r) == -1 || peek(r) == ')' || peek(r) == ']' || peek(r) == '}') {
            diag_emit(DIAG_ERROR, op_span,
                      "`^tailcall` requires a call after it");
            r->error = true;
            return NULL;
        }
        Form *inner = read_form(r);
        if (!inner) return NULL;
        Form **items = (Form **)arena_alloc(r->arena, 2 * sizeof(Form *));
        items[0] = form_sym(r->arena, op_span,
                            symtab_intern(r->st, strslice("^tailcall", 9)));
        items[1] = inner;
        Span whole = span_from_to(r, start_line, start_col, start_off,
                                  inner->span.off_end);
        return form_list(r->arena, whole, items, 2);
    }

    const Symbol *sym = symtab_intern(r->st, name);
    Form *atom = form_sym(r->arena, span, sym);

    /* Phase S2: Check for neoteric bracket immediately following atom */
    if (r->neoteric_enabled) {
        int bracket = peek_neoteric_bracket(r);
        if (bracket != -1) {
            return read_neoteric_bracket(r, atom, bracket);
        }
    }
    
    return atom;
}

/* INT-1: Read #?(:tur expr :turi expr) reader conditional. */
static Form *read_reader_cond(Reader *r) {
    uint32_t start_line = r->line;
    uint32_t start_col = r->col;
    size_t start_off = r->pos;
    advance(r); /* consume '#' */
    advance(r); /* consume '?' */

    skip_ws_and_comments(r);
    if (peek(r) != '(') {
        Span s = span_from_to(r, start_line, start_col, start_off, r->pos);
        diag_emit(DIAG_ERROR, s, "#? must be followed by '('");
        r->error = true;
        return NULL;
    }
    advance(r); /* consume '(' */

    Form **items = NULL;
    size_t cap = 0, n = 0;

    for (;;) {
        skip_ws_and_comments(r);
        if (r->error) { free(items); return NULL; }
        if (peek(r) == ')') { advance(r); break; }
        if (peek(r) == -1) {
            Span s = span_from_to(r, start_line, start_col, start_off, r->pos);
            diag_emit(DIAG_ERROR, s, "unterminated reader conditional (missing ')')");
            r->error = true;
            free(items);
            return NULL;
        }
        Form *child = read_form(r);
        if (!child) { free(items); return NULL; }
        if (n == cap) {
            cap = cap ? cap * 2 : 4;
            items = (Form **)realloc(items, cap * sizeof(Form *));
        }
        items[n++] = child;
    }

    Span span = span_from_to(r, start_line, start_col, start_off, r->pos);
    Form **arena_items = (Form **)arena_alloc(r->arena, n * sizeof(Form *));
    for (size_t i = 0; i < n; i++) arena_items[i] = items[i];
    free(items);
    return form_reader_cond(r->arena, span, arena_items, (uint32_t)n);
}

static Form *read_seq(Reader *r, char open, char close, FormTag tag,
                      const char *unterminated_msg) {
    uint32_t start_line = r->line;
    uint32_t start_col = r->col;
    size_t start_off = r->pos;
    advance(r); /* consume opener */

    Form **items = NULL;
    size_t cap = 0, n = 0;

    for (;;) {
        skip_ws_and_comments(r);
        int c = peek(r);
        if (c == -1) {
            /* Anchor the caret at the opening delimiter only (a single
             * character), not the whole span from opener to EOF. On real
             * files an open form can span hundreds of lines, which used
             * to produce a multi-screen `^^^^^^...` ribbon that buried
             * the actual problem location. */
            Span s = span_from_to(r, start_line, start_col,
                                  start_off, start_off + 1);
            diag_emit(DIAG_ERROR, s, "%s", unterminated_msg);
            r->error = true;
            free(items);
            return NULL;
        }
        if (c == close) {
            advance(r);
            break;
        }
        /* Defensive: a stray opposite-bracket inside should be a clear error,
         * not silently consumed by our caller. */
        if ((open == '(' && c == ']') || (open == '[' && c == ')')) {
            Span s = span_point(r);
            diag_emit(DIAG_ERROR, s, "mismatched closer '%c' inside %c..%c",
                      (char)c, open, close);
            r->error = true;
            free(items);
            return NULL;
        }
        r->at_list_head = (tag == F_LIST && n == 0);
        Form *child = read_form(r);
        r->at_list_head = false;
        if (!child) {
            free(items);
            return NULL;
        }
        if (n == cap) {
            cap = cap ? cap * 2 : 4;
            items = (Form **)realloc(items, cap * sizeof(Form *));
            if (!items) { fprintf(stderr, "tur: oom\n"); abort(); }
        }
        items[n++] = child;

        /* r7rs-lang-plan R1: a dotted pair `(a . d)`.  The bare `.` is not
         * an identifier in Scheme, so a `.` symbol inside a list means
         * exactly one thing.  The reader VALIDATES the shape -- something
         * before the dot, exactly one datum after it, then the closer -- and
         * keeps the `.` symbol in place as the improper-tail marker: the
         * Form model has no improper list, and R3's datum lowering (and
         * R2's `(lambda (a . rest) ...)` formals) read the marker where it
         * sits.  Under the Turmeric reader `.` is an ordinary symbol and this
         * block never runs, which is why `(quote (a . b))` there is an
         * "unbound symbol" and not a pair. */
        if (r->scheme_enabled && tag == F_LIST && child->tag == F_SYM &&
            child->as.sym->len == 1 && child->as.sym->name[0] == '.') {
            Span dot = child->span;
            if (n == 1) {
                diag_emit(DIAG_ERROR, dot,
                          "malformed dotted pair: `.` needs a datum before it");
                r->error = true;
                free(items);
                return NULL;
            }
            skip_ws_and_comments(r);
            int nc = peek(r);
            if (r->error || nc == -1 || nc == close) {
                if (!r->error)
                    diag_emit(DIAG_ERROR, dot,
                              "malformed dotted pair: `.` needs a datum after it");
                r->error = true;
                free(items);
                return NULL;
            }
            Form *tail = read_form(r);
            if (!tail) { free(items); return NULL; }
            if (n == cap) {
                cap = cap ? cap * 2 : 4;
                items = (Form **)realloc(items, cap * sizeof(Form *));
                if (!items) { fprintf(stderr, "tur: oom\n"); abort(); }
            }
            items[n++] = tail;
            skip_ws_and_comments(r);
            if (r->error) { free(items); return NULL; }
            if (peek(r) != close) {
                diag_emit(DIAG_ERROR, dot,
                          "malformed dotted pair: exactly one datum may follow `.`");
                r->error = true;
                free(items);
                return NULL;
            }
            advance(r);
            break;
        }
    }

    Span span = span_from_to(r, start_line, start_col, start_off, r->pos);
    Form *seq;
    if (tag == F_LIST) {
        seq = form_list(r->arena, span, items, (uint32_t)n);
    } else if (tag == F_VEC) {
        seq = form_vec(r->arena, span, items, (uint32_t)n);
    } else if (tag == F_SET) {
        seq = form_set(r->arena, span, items, (uint32_t)n);
    } else if (tag == F_MAP_LITERAL) {
        seq = form_map_literal(r->arena, span, items, (uint32_t)n);
    } else if (tag == F_SET_LITERAL) {
        seq = form_set_literal(r->arena, span, items, (uint32_t)n);
    } else if (tag == F_ROW_LITERAL) {
        seq = form_row_literal(r->arena, span, items, (uint32_t)n);
    } else {
        seq = form_map(r->arena, span, items, (uint32_t)n);
    }
    free(items);
    return seq;
}

static Form *read_map(Reader *r) {
    uint32_t start_line = r->line;
    uint32_t start_col = r->col;
    size_t start_off = r->pos;

    advance(r); /* consume '#' */
    if (peek(r) != '{') {
        Span s = span_from_to(r, start_line, start_col, start_off, r->pos);
        diag_emit(DIAG_ERROR, s, "expected '{' after '#' for map literal");
        r->error = true;
        return NULL;
    }

    /* fx-row-syntax-rename-plan Phase 1: bare `#{...}` is the legacy
     * effect-row spelling.  Tag with PROV_FX_LEGACY so elab emits TUR-D0002
     * the first time the form is consumed as an effect row.  Non-effect-row
     * consumers (none in tree today, but the slot is reserved) ignore the
     * provenance. */
    Form *m = read_seq(r, '{', '}', F_MAP, "unterminated map (missing '}')");
    if (m && m->tag == F_MAP) m->fx_prov = (uint8_t)PROV_FX_LEGACY;
    return m;
}

/* fx-row-syntax-rename-plan Phase 1: `#fx{...}` is the explicit effect-row
 * spelling, distinguished from the legacy bare `#{...}` only by the
 * provenance tag.  Dispatched from read_form *before* the `#` + `{` map
 * branch so `#fx{` does not parse as the symbol `fx` followed by a map. */
static Form *read_fx_row(Reader *r) {
    advance(r); /* consume '#' */
    advance(r); /* consume 'f' */
    advance(r); /* consume 'x' */
    /* peek is '{' -- checked by caller */
    Form *m = read_seq(r, '{', '}', F_MAP, "unterminated #fx{...} effect row (missing '}')");
    if (m && m->tag == F_MAP) m->fx_prov = (uint8_t)PROV_FX_EXPLICIT;
    return m;
}

/* C2 / #reads: `#reads <sym>` or `#reads [<sym> ...]` names the ^borrow
 * parameters whose mutable state a measure reads.  It sits at the effect-row
 * position of a defn signature and is consumed by elab_defn.  Returns
 * `(reads <sym> ...)` stamped PROV_READS so the signature walk distinguishes it
 * from a body expression.  A read-frame is not an effect row; see
 * docs/guides/stateful-refinements-guide.md.
 *
 * The bracketed form mirrors `#writes` below, for the same reason its comment
 * gives: the annotation is followed by the return marker `:`, which reads as a
 * symbol, so a greedy symbol run could not tell where the frame ends.  Unlike
 * `#writes`, an EMPTY `#reads []` is rejected in elab_defn rather than here --
 * "reads nothing" is the absence of the annotation, and spelling it two ways
 * would give the solver two encodings of one claim. */
static Form *read_reads_annot(Reader *r) {
    uint32_t start_line = r->line;
    uint32_t start_col  = r->col;
    size_t   start_off  = r->pos;
    advance(r); advance(r); advance(r);   /* '#' 'r' 'e' */
    advance(r); advance(r); advance(r);   /* 'a' 'd' 's' */
    skip_ws_and_comments(r);
    Form *vec = NULL;
    Form *one = NULL;
    if (peek(r) == '[') {
        vec = read_seq(r, '[', ']', F_VEC,
                       "unterminated #reads frame (missing ']')");
        if (!vec) return NULL;               /* error already emitted */
    } else {
        one = read_symbol_or_minus_at(r, true);  /* the single parameter name */
        if (!one) return NULL;               /* error already emitted */
    }
    uint32_t n = vec ? vec->as.list.len : 1;
    Span span = span_from_to(r, start_line, start_col, start_off, r->pos);
    Form *head = form_sym(r->arena, span, symtab_intern(r->st, strslice("reads", 5)));
    Form **items = (Form **)arena_alloc(r->arena, (n + 1) * sizeof(Form *));
    items[0] = head;
    if (vec) {
        for (uint32_t i = 0; i < n; i++) items[i + 1] = vec->as.list.items[i];
    } else {
        items[1] = one;
    }
    Form *lst = form_list(r->arena, span, items, n + 1);
    lst->fx_prov = (uint8_t)PROV_READS;
    return lst;
}

/* WF1 / #writes: `#writes <sym>` or `#writes [<sym> ...]` names the parameters
 * whose mutable state this function's body may write.  Sits at the same
 * signature position as `#reads` (after `#fx{...}`, either order relative to
 * `#reads`) and is consumed by elab_defn.  Returns `(writes <sym>...)` stamped
 * PROV_WRITES.
 *
 * Why a bracketed list and not a greedy symbol run: the annotation is followed
 * by the return-type marker `:`, which reads as a symbol, so a greedy scan
 * could not tell where the frame ends.  The vector form is unambiguous, and it
 * gives `#writes []` -- "this body writes nothing" -- a spelling, which is the
 * frame WF2 most wants to check and which a single-symbol-only syntax could
 * not express.  See docs/archive/checked-write-frames-plan.md (WF1). */
static Form *read_writes_annot(Reader *r) {
    uint32_t start_line = r->line;
    uint32_t start_col  = r->col;
    size_t   start_off  = r->pos;
    advance(r); advance(r); advance(r);   /* '#' 'w' 'r' */
    advance(r); advance(r); advance(r);   /* 'i' 't' 'e' */
    advance(r);                           /* 's' */
    skip_ws_and_comments(r);
    Form  *vec = NULL;
    Form  *one = NULL;
    if (peek(r) == '[') {
        vec = read_seq(r, '[', ']', F_VEC,
                       "unterminated #writes frame (missing ']')");
        if (!vec) return NULL;            /* error already emitted */
    } else {
        one = read_symbol_or_minus_at(r, true); /* the single parameter name */
        if (!one) return NULL;            /* error already emitted */
    }
    uint32_t n = vec ? vec->as.list.len : 1;
    Span span = span_from_to(r, start_line, start_col, start_off, r->pos);
    Form *head = form_sym(r->arena, span, symtab_intern(r->st, strslice("writes", 6)));
    Form **items = (Form **)arena_alloc(r->arena, (n + 1) * sizeof(Form *));
    items[0] = head;
    if (vec) {
        for (uint32_t i = 0; i < n; i++) items[i + 1] = vec->as.list.items[i];
    } else {
        items[1] = one;
    }
    Form *lst = form_list(r->arena, span, items, n + 1);
    lst->fx_prov = (uint8_t)PROV_WRITES;
    return lst;
}

static Form *read_set(Reader *r) {
    advance(r); /* consume '#' */
    advance(r); /* consume 's' */
    return read_seq(r, '(', ')', F_SET, "unterminated set (missing ')')");
}

/* DL0: Is `f` a valid key form for a #map{...} literal?
 * Keys must be a keyword, string literal, or int literal. */
static bool dl_valid_map_key(const Form *f) {
    return f->tag == F_KEYWORD || f->tag == F_STR || f->tag == F_INT;
}

/* DL0: Read a #map{...} data literal -> F_MAP_LITERAL.
 * Slots are key/value pairs; keys must be keyword/string/int literals.
 *   TUR-E0280 -- odd slot count (unmatched key)
 *   TUR-E0281 -- unexpected EOF (delegated to read_seq's message)
 *   TUR-E0282 -- invalid key form */
static Form *read_map_literal(Reader *r) {
    advance(r); /* consume '#' */
    advance(r); /* consume 'm' */
    advance(r); /* consume 'a' */
    advance(r); /* consume 'p' */
    /* peek == '{' guaranteed by caller */
    r->turmeric_literal_depth++;
    Form *lit = read_seq(r, '{', '}', F_MAP_LITERAL,
                         "unterminated map literal (missing '}') (TUR-E0281)");
    r->turmeric_literal_depth--;
    if (!lit || r->error) return lit;

    uint32_t n = lit->as.list.len;
    /* quasiquote-splice-into-vector-unsupported: when a slot is an unquote
     * (`~x`) or unquote-splicing (`~@xs`), the literal sits inside a macro
     * template and its real key/value slots are only known after expansion.
     * A `~@` splice in particular changes the slot count.  Defer the
     * even-arity and key-form checks past macro expansion in that case. */
    bool has_unquote = false;
    for (uint32_t i = 0; i < n; i++) {
        FormTag t = lit->as.list.items[i]->tag;
        if (t == F_UNQUOTE || t == F_UNQUOTE_SPLICING) { has_unquote = true; break; }
    }
    if (has_unquote) return lit;
    if (n % 2 != 0) {
        diag_emit(DIAG_ERROR, lit->span,
                  "#map{...} requires an even number of slot forms "
                  "(key/value pairs); got %u (TUR-E0280)", n);
        r->error = true;
        return NULL;
    }
    for (uint32_t i = 0; i < n; i += 2) {
        Form *key = lit->as.list.items[i];
        if (!dl_valid_map_key(key)) {
            diag_emit(DIAG_ERROR, key->span,
                      "#map{...} key must be a keyword, string, or int literal "
                      "(TUR-E0282)");
            r->error = true;
            return NULL;
        }
    }
    return lit;
}

/* DL0: Read a #set{...} data literal -> F_SET_LITERAL.
 * Elements are arbitrary expressions; no key-form restriction.
 *   TUR-E0281 -- unexpected EOF (delegated to read_seq's message) */
static Form *read_set_literal(Reader *r) {
    advance(r); /* consume '#' */
    advance(r); /* consume 's' */
    advance(r); /* consume 'e' */
    advance(r); /* consume 't' */
    /* peek == '{' guaranteed by caller */
    r->turmeric_literal_depth++;
    Form *lit = read_seq(r, '{', '}', F_SET_LITERAL,
                         "unterminated set literal (missing '}') (TUR-E0281)");
    r->turmeric_literal_depth--;
    return lit;
}

/* Variadic HKT rows: read a #row{...} type-row literal -> F_ROW_LITERAL.
 * Elements are type forms (positional). The literal is only meaningful in
 * type-annotation position, where the elaborator lowers it to a TY_TYPEROW;
 * in value position the elaborator reports a type-only error.
 *   TUR-E0281 -- unexpected EOF (delegated to read_seq's message) */
static Form *read_row_literal(Reader *r) {
    advance(r); /* consume '#' */
    advance(r); /* consume 'r' */
    advance(r); /* consume 'o' */
    advance(r); /* consume 'w' */
    /* peek == '{' guaranteed by caller */
    return read_seq(r, '{', '}', F_ROW_LITERAL,
                    "unterminated row literal (missing '}') (TUR-E0281)");
}

/* Read a #refine{var : T | pred} contract-type data literal.  Consumes the
 * `#refine` tag, then delegates to read_contract_type for the `{...}` body,
 * producing the same F_CONTRACT_TYPE form bare braces used to. */
static Form *read_refine_literal(Reader *r) {
    advance(r); /* consume '#' */
    advance(r); /* consume 'r' */
    advance(r); /* consume 'e' */
    advance(r); /* consume 'f' */
    advance(r); /* consume 'i' */
    advance(r); /* consume 'n' */
    advance(r); /* consume 'e' */
    /* peek == '{' guaranteed by caller */
    return read_contract_type(r);
}

/* TCE (typed-container-elements / test-suite-idioms Phase E): a `:`-prefixed
 * element type fused to the closer of a vec, set, or map literal pins the
 * collection's type parameters.  `[]:int` lowers to `(:: [] (Vec int))` and
 * `#set{}:int` to `(:: #set{} (Set int))`, so an empty literal recovers its
 * element type without the verbose `(:: (vec-new) (Vec int))` ascription.
 * The element type form may itself be compound -- `#set{}:(Vec int)` pins
 * `Set[Vec[int]]`.
 *
 * `ctor` is the type-constructor name ("Vec", "Set", or "Map") and
 * `n_type_args` how many parameters it takes.  A two-parameter constructor
 * spells its suffix as a parenthesised pair -- `#map{}:(int cstr)` lowers to
 * `(:: #map{} (Map int cstr))` -- because the arity has to come from
 * somewhere and a bare `#map{}:int cstr` would swallow whatever token
 * followed the literal.  There is no ambiguity with the compound-element
 * reading above: which one applies is fixed by the literal kind, not by the
 * shape of the suffix.
 *
 * The suffix is recognized only when a single ':' is immediately adjacent to
 * the closer (no intervening whitespace) and is not the '::' ascription
 * operator; otherwise `lit` is returned unchanged so binding vectors
 * (`[x :int]`, where ']' is followed by space) and bare literals are
 * unaffected. */
static Form *maybe_container_type_suffix(Reader *r, Form *lit, const char *ctor,
                                         int n_type_args) {
    if (!lit || r->error) return lit;
    if (peek(r) != ':' || peek2(r) == ':') return lit;

    uint32_t s_line = r->line, s_col = r->col;
    size_t s_off = r->pos;
    advance(r); /* consume ':' */
    if (peek(r) == -1) {
        Span s = span_from_to(r, s_line, s_col, s_off, r->pos);
        diag_emit(DIAG_ERROR, s,
                  n_type_args == 2
                      ? "expected a (key value) type pair after the ':' "
                        "container-literal suffix (e.g. #map{}:(int cstr))"
                      : "expected an element type after the ':' "
                        "container-literal suffix (e.g. []:int)");
        r->error = true;
        return NULL;
    }
    Form *elem = read_form(r);
    if (!elem || r->error) return NULL;

    /* A two-parameter constructor takes its arguments as one parenthesised
     * list.  Reject anything else here rather than letting a mis-shaped
     * suffix reach the elaborator as a bogus type application, where the
     * error would point at a form the user never wrote. */
    if (n_type_args == 2 &&
        (elem->tag != F_LIST || elem->as.list.len != 2)) {
        Span s = span_from_to(r, s_line, s_col, s_off, r->pos);
        diag_emit(DIAG_ERROR, s,
                  "the ':' suffix on a #map{} literal takes exactly two type "
                  "arguments as a parenthesised pair -- write "
                  "#map{}:(<key> <value>), e.g. #map{}:(int cstr)");
        r->error = true;
        return NULL;
    }

    Span sp = lit->span;
    /* (Ctor arg...) -- the full container type. */
    const Symbol *ctor_sym =
        symtab_intern(r->st, strslice(ctor, (uint32_t)strlen(ctor)));
    size_t n_items = 1 + (size_t)n_type_args;
    Form **ctor_items = (Form **)arena_alloc(r->arena, n_items * sizeof(Form *));
    ctor_items[0] = form_sym(r->arena, sp, ctor_sym);
    if (n_type_args == 2) {
        ctor_items[1] = elem->as.list.items[0];
        ctor_items[2] = elem->as.list.items[1];
    } else {
        ctor_items[1] = elem;
    }
    Form *ctype = form_list(r->arena, sp, ctor_items, (uint32_t)n_items);
    /* (:: lit (Ctor elem)) -- erased at codegen; pins the static type. */
    const Symbol *asc_sym = symtab_intern(r->st, strslice("::", 2));
    Form **asc_items = (Form **)arena_alloc(r->arena, 3 * sizeof(Form *));
    asc_items[0] = form_sym(r->arena, sp, asc_sym);
    asc_items[1] = lit;
    asc_items[2] = ctype;
    return form_list(r->arena, sp, asc_items, 3);
}

/* N1 (numeric-tower-rational-complex-plan §5.1): parse a decimal int64 out of
 * the raw `#rat{...}` body, starting at *i and stopping at the first byte that
 * is not part of the number.  Returns false on a missing digit run or on
 * int64 overflow, so the caller can report a read-time error instead of
 * silently wrapping a literal.  Accepts a leading '+' or '-'. */
static char *read_raw_body(Reader *r, int open, int close, uint32_t *out_len);

static bool rat_parse_int(const char *body, uint32_t len, uint32_t *i,
                          int64_t *out) {
    uint32_t k = *i;
    bool neg = false;
    if (k < len && (body[k] == '-' || body[k] == '+')) {
        neg = (body[k] == '-');
        k++;
    }
    if (k >= len || body[k] < '0' || body[k] > '9') return false;
    /* Accumulate the magnitude in uint64 so the int64 minimum -- whose
     * magnitude is one past the positive range -- parses without overflowing
     * the accumulator it is being range-checked against. */
    uint64_t mag = 0;
    const uint64_t limit = neg ? 9223372036854775808ULL : 9223372036854775807ULL;
    while (k < len && body[k] >= '0' && body[k] <= '9') {
        uint64_t digit = (uint64_t)(body[k] - '0');
        if (mag > (limit - digit) / 10) return false;   /* would overflow */
        mag = mag * 10 + digit;
        k++;
    }
    *i = k;
    *out = neg ? (int64_t)(~mag + 1ULL) : (int64_t)mag;
    return true;
}

/* N1: gcd of two non-negative values, for read-time normalization. */
static uint64_t rat_gcd_u64(uint64_t a, uint64_t b) {
    while (b != 0) { uint64_t t = a % b; a = b; b = t; }
    return a;
}

/* N1: read a `#rat{n/d}` rational literal -> `(rat/of! n d)`, normalized at
 * read time (`#rat{6/8}` emits `(rat/of! 3 4)`).
 *
 * Bare `3/4` is not available as rational syntax: read_number stops at '/', so
 * `3/4` already reads as `3` followed by the symbol `/4`, and making it a
 * rational would collide with '/' as division and with module-qualified names
 * (`tur/list`).  Hence a '#'-dispatch alongside #map / #set / #json.
 *
 * The body is read RAW rather than as forms, because curly-infix is enabled in
 * every dialect: an ordinarily-read `{3 / 4}` would be infix division, not a
 * literal.  A missing denominator (`#rat{5}`) is the whole number 5/1.
 *
 *   TUR-E0284 -- malformed body, zero denominator, or out-of-int64-range part */
static Form *read_rat_literal(Reader *r) {
    uint32_t s_line = r->line, s_col = r->col;
    size_t   s_off  = r->pos;
    advance(r); /* consume '#' */
    advance(r); /* consume 'r' */
    advance(r); /* consume 'a' */
    advance(r); /* consume 't' */
    /* peek == '{' guaranteed by caller */
    uint32_t body_len = 0;
    char *body = read_raw_body(r, '{', '}', &body_len);
    if (!body || r->error) return NULL;
    Span sp = span_from_to(r, s_line, s_col, s_off, r->pos);

    uint32_t i = 0;
    while (i < body_len && (body[i] == ' ' || body[i] == '\t')) i++;
    int64_t n = 0, d = 1;
    if (!rat_parse_int(body, body_len, &i, &n)) {
        diag_emit(DIAG_ERROR, sp,
                  "malformed rational literal '#rat{%.*s}'; expected "
                  "'#rat{n/d}' with int64 parts (TUR-E0284)",
                  (int)body_len, body);
        r->error = true;
        return NULL;
    }
    while (i < body_len && (body[i] == ' ' || body[i] == '\t')) i++;
    if (i < body_len && body[i] == '/') {
        i++;
        while (i < body_len && (body[i] == ' ' || body[i] == '\t')) i++;
        if (!rat_parse_int(body, body_len, &i, &d)) {
            diag_emit(DIAG_ERROR, sp,
                      "malformed rational literal '#rat{%.*s}'; expected an "
                      "int64 denominator after '/' (TUR-E0284)",
                      (int)body_len, body);
            r->error = true;
            return NULL;
        }
    }
    while (i < body_len && (body[i] == ' ' || body[i] == '\t')) i++;
    if (i != body_len) {
        diag_emit(DIAG_ERROR, sp,
                  "trailing junk in rational literal '#rat{%.*s}'; expected "
                  "'#rat{n/d}' (TUR-E0284)",
                  (int)body_len, body);
        r->error = true;
        return NULL;
    }
    if (d == 0) {
        diag_emit(DIAG_ERROR, sp,
                  "rational literal '#rat{%.*s}' has a zero denominator "
                  "(TUR-E0284)",
                  (int)body_len, body);
        r->error = true;
        return NULL;
    }

    /* Normalize at read time so `#rat{6/8}` and `#rat{3/4}` are the same
     * literal, and so structural equality on the emitted value is
     * mathematical equality.  Magnitudes go through uint64 so the int64
     * minimum normalizes without a negation that would overflow. */
    bool neg  = ((n < 0) != (d < 0));
    uint64_t un = (n < 0) ? (~(uint64_t)n + 1ULL) : (uint64_t)n;
    uint64_t ud = (d < 0) ? (~(uint64_t)d + 1ULL) : (uint64_t)d;
    uint64_t g  = rat_gcd_u64(un, ud);
    if (g != 0) { un /= g; ud /= g; }
    if (ud > 9223372036854775807ULL ||
        un > (neg ? 9223372036854775808ULL : 9223372036854775807ULL)) {
        diag_emit(DIAG_ERROR, sp,
                  "normalized rational literal '#rat{%.*s}' does not fit in "
                  "int64 (TUR-E0284)",
                  (int)body_len, body);
        r->error = true;
        return NULL;
    }
    int64_t nn = neg ? (int64_t)(~un + 1ULL) : (int64_t)un;
    int64_t nd = (int64_t)ud;

    const Symbol *ctor = symtab_intern(r->st, strslice("rat/of!", 7));
    Form **items = (Form **)arena_alloc(r->arena, 3 * sizeof(Form *));
    items[0] = form_sym(r->arena, sp, ctor);
    items[1] = form_int(r->arena, sp, nn);
    items[2] = form_int(r->arena, sp, nd);
    return form_list(r->arena, sp, items, 3);
}

/* N2 (numeric-tower-rational-complex-plan §5.2): read a `#cx{re im}` complex
 * literal -> `(complex/of re im)`.
 *
 * `#cx` rather than `#c`: a single-letter dispatch is too scarce a name to
 * spend, and `#c` reads as "C" in a codebase full of inline-C blocks.  Unlike
 * `#rat{...}`, the two slots are read as ordinary FORMS, so a computed
 * component composes -- `#cx{3.25 {1.0 + 0.5}}` works, and the curly-infix
 * inner expression is exactly what it looks like.
 *
 *   TUR-E0285 -- wrong number of slot forms */
static Form *read_cx_literal(Reader *r) {
    uint32_t s_line = r->line, s_col = r->col;
    size_t   s_off  = r->pos;
    advance(r); /* consume '#' */
    advance(r); /* consume 'c' */
    advance(r); /* consume 'x' */
    /* peek == '{' guaranteed by caller */
    Form *seq = read_seq(r, '{', '}', F_LIST,
                         "unterminated complex literal (missing '}') "
                         "(TUR-E0285)");
    if (!seq || r->error) return NULL;
    Span sp = span_from_to(r, s_line, s_col, s_off, r->pos);
    if (seq->as.list.len != 2) {
        diag_emit(DIAG_ERROR, sp,
                  "#cx{...} requires exactly two slot forms (real and "
                  "imaginary part); got %u (TUR-E0285)", seq->as.list.len);
        r->error = true;
        return NULL;
    }
    const Symbol *ctor = symtab_intern(r->st, strslice("complex/of", 10));
    Form **items = (Form **)arena_alloc(r->arena, 3 * sizeof(Form *));
    items[0] = form_sym(r->arena, sp, ctor);
    items[1] = seq->as.list.items[0];
    items[2] = seq->as.list.items[1];
    return form_list(r->arena, sp, items, 3);
}

/* DL0: Try to read a #<tag>{...} data literal.  Returns NULL (without
 * consuming) when the input is not a data literal so the caller can fall
 * through to other '#'-dispatches.  Only invoked when -Xdata-literals is on.
 *
 * Recognizes #map{ and #set{ from a small dispatch table.  A #<ident>{ whose
 * tag is not in the table is reported as TUR-E0283 (unknown dispatch tag). */
static Form *try_read_data_literal(Reader *r) {
    /* peek(r) == '#' guaranteed by caller. */
    if (peek_at(r, 1) == 'm' && peek_at(r, 2) == 'a' &&
        peek_at(r, 3) == 'p' && peek_at(r, 4) == '{') {
        return maybe_container_type_suffix(r, read_map_literal(r), "Map", 2);
    }
    if (peek_at(r, 1) == 's' && peek_at(r, 2) == 'e' &&
        peek_at(r, 3) == 't' && peek_at(r, 4) == '{') {
        return maybe_container_type_suffix(r, read_set_literal(r), "Set", 1);
    }
    if (peek_at(r, 1) == 'r' && peek_at(r, 2) == 'o' &&
        peek_at(r, 3) == 'w' && peek_at(r, 4) == '{') {
        return read_row_literal(r);
    }
    if (peek_at(r, 1) == 'r' && peek_at(r, 2) == 'e' &&
        peek_at(r, 3) == 'f' && peek_at(r, 4) == 'i' &&
        peek_at(r, 5) == 'n' && peek_at(r, 6) == 'e' &&
        peek_at(r, 7) == '{') {
        return read_refine_literal(r);
    }
    /* N1/N2: the numeric-tower literals.  Always on, not a #lang layer -- a
     * core data-literal dispatch belongs here with #map / #set. */
    if (peek_at(r, 1) == 'r' && peek_at(r, 2) == 'a' &&
        peek_at(r, 3) == 't' && peek_at(r, 4) == '{') {
        return read_rat_literal(r);
    }
    if (peek_at(r, 1) == 'c' && peek_at(r, 2) == 'x' && peek_at(r, 3) == '{') {
        return read_cx_literal(r);
    }
    /* Detect a #<ident>{ shape with an unrecognized tag -> TUR-E0283.
     * Scan a run of identifier characters after '#'; if it is followed by
     * '{' and the run is non-empty, it looked like a data-literal dispatch. */
    size_t k = 1;
    while (is_sym_cont(peek_at(r, k))) k++;
    if (k > 1 && peek_at(r, k) == '{') {
        Span s = span_point(r);
        char tag[32];
        size_t tlen = k - 1 < sizeof(tag) - 1 ? k - 1 : sizeof(tag) - 1;
        for (size_t i = 0; i < tlen; i++) tag[i] = (char)peek_at(r, 1 + i);
        tag[tlen] = '\0';
        diag_emit(DIAG_ERROR, s,
                  "unknown data-literal dispatch tag '#%s{...}'; "
                  "expected '#map{...}', '#set{...}', '#row{...}', "
                  "'#rat{...}', or '#cx{...}' (TUR-E0283)", tag);
        r->error = true;
        return NULL;
    }
    return NULL;
}

/* ------------------------------------------------------------------------ */
/* JR0 (json-reader-macro-plan): #json(...) compile-time reader macro.       */
/*                                                                           */
/* Parses a verbatim JSON value at compile time and emits the equivalent     */
/* tur/json tagged-node constructor calls:                                   */
/*   object -> (json/object-put (json/object-put (json/object-new) k v) ...) */
/*   array  -> (json/array-push (json/array-push (json/array-new) e0) ...)   */
/*   string -> (json/string "..."),  int -> (json/int n)                     */
/*   float  -> (json/float f),       true/false -> (json/bool true|false)    */
/*   null   -> (json/null)                                                   */
/* Every node is a uniform :int handle, so heterogeneous and nested JSON     */
/* compose -- the type variation lives in each node's runtime tag, queryable */
/* via json/type + json/get-*.  This is the same node tree json/decode       */
/* produces, so #json(...) is a compile-time-validated json/decode. The      */
/* emitted forms are elaborated by the normal typechecker; json.tur is       */
/* auto-loaded so the constructors resolve.                                  */
/* Errors: TUR-E0270 (malformed JSON), TUR-E0271 (unexpected EOF).           */
/* ------------------------------------------------------------------------ */

static Form *json_read_value(Reader *r);

/* Strict JSON whitespace: space, tab, LF, CR (JSON has no comments). */
static void json_skip_ws(Reader *r) {
    for (;;) {
        int c = peek(r);
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') { advance(r); continue; }
        break;
    }
}

/* Build a (NAME arg0 arg1 ...) call form interning NAME as a symbol. */
static Form *json_call(Reader *r, Span span, const char *name,
                       Form **args, uint32_t nargs) {
    const Symbol *sym = symtab_intern(r->st, strslice(name, (uint32_t)strlen(name)));
    Form **items = (Form **)arena_alloc(r->arena, (nargs + 1) * sizeof(Form *));
    items[0] = form_sym(r->arena, span, sym);
    for (uint32_t i = 0; i < nargs; i++) items[i + 1] = args[i];
    return form_list(r->arena, span, items, nargs + 1);
}

/* UTF-8 encode a BMP code point into buf (1-3 bytes); returns byte count. */
static size_t json_utf8_encode(uint32_t cp, char *buf) {
    if (cp <= 0x7F) { buf[0] = (char)cp; return 1; }
    if (cp <= 0x7FF) {
        buf[0] = (char)(0xC0 | (cp >> 6));
        buf[1] = (char)(0x80 | (cp & 0x3F));
        return 2;
    }
    buf[0] = (char)(0xE0 | (cp >> 12));
    buf[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
    buf[2] = (char)(0x80 | (cp & 0x3F));
    return 3;
}

static int json_hex_digit(int c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* Read a JSON string starting at the opening '"'.  Emits a raw F_STR literal
 * (the :cstr payload).  json_read_value wraps it in (json/string ...) for a
 * string *value*; object keys use the raw F_STR directly (json/object-put
 * takes a :cstr key). */
static Form *json_read_string(Reader *r) {
    uint32_t sl = r->line, sc = r->col;
    size_t   so = r->pos;
    advance(r); /* consume opening '"' */

    char *buf = NULL;
    size_t cap = 0, n = 0;
#define JPUSH(ch) do {                                                  \
        if (n == cap) { cap = cap ? cap * 2 : 16;                       \
            buf = (char *)realloc(buf, cap);                            \
            if (!buf) { fprintf(stderr, "tur: oom\n"); abort(); } }     \
        buf[n++] = (char)(ch);                                          \
    } while (0)

    for (;;) {
        int c = peek(r);
        if (c == -1) {
            Span s = span_from_to(r, sl, sc, so, r->pos);
            diag_emit(DIAG_ERROR, s,
                      "#json: unexpected end of input inside string (TUR-E0271)");
            r->error = true; free(buf); return NULL;
        }
        if (c == '"') { advance(r); break; }
        if (c == '\\') {
            advance(r);
            int e = advance(r);
            switch (e) {
                case '"':  JPUSH('"');  break;
                case '\\': JPUSH('\\'); break;
                case '/':  JPUSH('/');  break;
                case 'b':  JPUSH('\b'); break;
                case 'f':  JPUSH('\f'); break;
                case 'n':  JPUSH('\n'); break;
                case 'r':  JPUSH('\r'); break;
                case 't':  JPUSH('\t'); break;
                case 'u': {
                    int h0 = json_hex_digit(advance(r));
                    int h1 = json_hex_digit(advance(r));
                    int h2 = json_hex_digit(advance(r));
                    int h3 = json_hex_digit(advance(r));
                    if (h0 < 0 || h1 < 0 || h2 < 0 || h3 < 0) {
                        diag_emit(DIAG_ERROR, span_point(r),
                                  "#json: invalid \\u escape (TUR-E0270)");
                        r->error = true; free(buf); return NULL;
                    }
                    uint32_t cp = (uint32_t)((h0 << 12) | (h1 << 8) | (h2 << 4) | h3);
                    char u8[4];
                    size_t k = json_utf8_encode(cp, u8);
                    for (size_t i = 0; i < k; i++) JPUSH(u8[i]);
                    break;
                }
                default:
                    diag_emit(DIAG_ERROR, span_point(r),
                              "#json: invalid string escape '\\%c' (TUR-E0270)",
                              (char)(e < 0 ? '?' : e));
                    r->error = true; free(buf); return NULL;
            }
        } else {
            advance(r);
            JPUSH(c);
        }
    }
#undef JPUSH

    Span span = span_from_to(r, sl, sc, so, r->pos);
    Form *f = form_str(r->arena, span, buf ? buf : "", (uint32_t)n);
    free(buf);
    return f;
}

/* Read a JSON number.  Emits (json/int n) for integers, (json/float f) when a
 * fraction or exponent is present. */
static Form *json_read_number(Reader *r) {
    uint32_t sl = r->line, sc = r->col;
    size_t   so = r->pos;
    bool is_float = false;
    bool saw_digit = false;

    if (peek(r) == '-') advance(r);
    while (peek(r) >= '0' && peek(r) <= '9') { advance(r); saw_digit = true; }
    if (peek(r) == '.') {
        is_float = true; advance(r);
        while (peek(r) >= '0' && peek(r) <= '9') { advance(r); saw_digit = true; }
    }
    if (peek(r) == 'e' || peek(r) == 'E') {
        is_float = true; advance(r);
        if (peek(r) == '+' || peek(r) == '-') advance(r);
        while (peek(r) >= '0' && peek(r) <= '9') advance(r);
    }

    size_t len = r->pos - so;
    char tmp[64];
    if (!saw_digit || len == 0 || len >= sizeof(tmp)) {
        Span s = span_from_to(r, sl, sc, so, r->pos);
        diag_emit(DIAG_ERROR, s, "#json: malformed number (TUR-E0270)");
        r->error = true; return NULL;
    }
    memcpy(tmp, r->src + so, len);
    tmp[len] = '\0';

    Span span = span_from_to(r, sl, sc, so, r->pos);
    if (is_float) {
        Form *lit = form_float(r->arena, span, strtod(tmp, NULL));
        return json_call(r, span, "json/float", &lit, 1);
    }
    Form *lit = form_int(r->arena, span, (int64_t)strtoll(tmp, NULL, 10));
    return json_call(r, span, "json/int", &lit, 1);
}

/* Read a JSON array starting at '['.  Left-folds into
 *   (json/array-push (json/array-push (json/array-new) e0) e1) ...
 * so each element is appended in source order. */
static Form *json_read_array(Reader *r) {
    uint32_t sl = r->line, sc = r->col;
    size_t   so = r->pos;
    advance(r); /* consume '[' */

    Span open_sp = span_from_to(r, sl, sc, so, r->pos);
    Form *acc = json_call(r, open_sp, "json/array-new", NULL, 0);

    json_skip_ws(r);
    if (peek(r) == ']') { advance(r); return acc; }
    for (;;) {
        Form *v = json_read_value(r);
        if (!v) return NULL;
        Form *push_args[2] = { acc, v };
        acc = json_call(r, v->span, "json/array-push", push_args, 2);
        json_skip_ws(r);
        int c = peek(r);
        if (c == ',') { advance(r); continue; }
        if (c == ']') { advance(r); break; }
        if (c == -1) {
            Span s = span_from_to(r, sl, sc, so, r->pos);
            diag_emit(DIAG_ERROR, s,
                      "#json: unexpected end of input inside array (TUR-E0271)");
            r->error = true; return NULL;
        }
        diag_emit(DIAG_ERROR, span_point(r),
                  "#json: expected ',' or ']' in array (TUR-E0270)");
        r->error = true; return NULL;
    }
    return acc;
}

/* Read a JSON object starting at '{'.  Left-folds into
 *   (json/object-put (json/object-put (json/object-new) "k0" v0) "k1" v1) ...
 * Keys are passed as raw :cstr (json/object-put takes a string key), so a
 * value is retrieved with (json/get node "key"). */
static Form *json_read_object(Reader *r) {
    uint32_t sl = r->line, sc = r->col;
    size_t   so = r->pos;
    advance(r); /* consume '{' */

    Span open_sp = span_from_to(r, sl, sc, so, r->pos);
    Form *acc = json_call(r, open_sp, "json/object-new", NULL, 0);

    json_skip_ws(r);
    if (peek(r) == '}') { advance(r); return acc; }
    for (;;) {
        json_skip_ws(r);
        if (peek(r) != '"') {
            if (peek(r) == -1) {
                Span s = span_from_to(r, sl, sc, so, r->pos);
                diag_emit(DIAG_ERROR, s,
                          "#json: unexpected end of input inside object (TUR-E0271)");
            } else {
                diag_emit(DIAG_ERROR, span_point(r),
                          "#json: object key must be a string (TUR-E0270)");
            }
            r->error = true; return NULL;
        }
        Form *key_str = json_read_string(r);
        if (!key_str) return NULL;
        json_skip_ws(r);
        if (peek(r) != ':') {
            diag_emit(DIAG_ERROR, span_point(r),
                      "#json: expected ':' after object key (TUR-E0270)");
            r->error = true; return NULL;
        }
        advance(r); /* consume ':' */
        Form *val = json_read_value(r);
        if (!val) return NULL;
        Form *put_args[3] = { acc, key_str, val };
        acc = json_call(r, val->span, "json/object-put", put_args, 3);
        json_skip_ws(r);
        int c = peek(r);
        if (c == ',') { advance(r); continue; }
        if (c == '}') { advance(r); break; }
        if (c == -1) {
            Span s = span_from_to(r, sl, sc, so, r->pos);
            diag_emit(DIAG_ERROR, s,
                      "#json: unexpected end of input inside object (TUR-E0271)");
            r->error = true; return NULL;
        }
        diag_emit(DIAG_ERROR, span_point(r),
                  "#json: expected ',' or '}' in object (TUR-E0270)");
        r->error = true; return NULL;
    }
    return acc;
}

/* Match a bare JSON keyword (true/false/null) at the cursor, consuming it on
 * success. */
static bool json_match_kw(Reader *r, const char *kw) {
    size_t klen = strlen(kw);
    for (size_t i = 0; i < klen; i++) {
        if (peek_at(r, i) != (int)(unsigned char)kw[i]) return false;
    }
    for (size_t i = 0; i < klen; i++) advance(r);
    return true;
}

/* Recursive-descent entry: read one JSON value and emit its Turmeric form. */
static Form *json_read_value(Reader *r) {
    json_skip_ws(r);
    int c = peek(r);
    uint32_t sl = r->line, sc = r->col;
    size_t   so = r->pos;
    switch (c) {
        case '{': return json_read_object(r);
        case '[': return json_read_array(r);
        case '"': {
            Form *str = json_read_string(r);
            if (!str) return NULL;
            return json_call(r, str->span, "json/string", &str, 1);
        }
        case 't':
            if (json_match_kw(r, "true")) {
                Span sp = span_from_to(r, sl, sc, so, r->pos);
                /* stdlib-int-stand-in-audit S3: json/bool takes :bool now,
                   so the reader emits a real boolean rather than 1/0. */
                Form *one = form_bool(r->arena, sp, true);
                return json_call(r, sp, "json/bool", &one, 1);
            }
            break;
        case 'f':
            if (json_match_kw(r, "false")) {
                Span sp = span_from_to(r, sl, sc, so, r->pos);
                Form *zero = form_bool(r->arena, sp, false);
                return json_call(r, sp, "json/bool", &zero, 1);
            }
            break;
        case 'n':
            /* JSON null becomes a real, type-distinct (json/null) node -- not a
             * 0 sentinel. */
            if (json_match_kw(r, "null"))
                return json_call(r, span_from_to(r, sl, sc, so, r->pos),
                                 "json/null", NULL, 0);
            break;
        case '-': case '0': case '1': case '2': case '3':
        case '4': case '5': case '6': case '7': case '8': case '9':
            return json_read_number(r);
        case -1:
            diag_emit(DIAG_ERROR, span_point(r),
                      "#json: unexpected end of input (TUR-E0271)");
            r->error = true; return NULL;
        default: break;
    }
    diag_emit(DIAG_ERROR, span_point(r),
              "#json: unexpected character '%c' in JSON value (TUR-E0270)",
              (char)(c < 0 ? '?' : c));
    r->error = true;
    return NULL;
}

/* JR0: try to read a #json(...) form.  peek(r) == '#' is guaranteed by the
 * caller.  Returns NULL (without consuming) when the input is not a #json
 * form so the dispatcher can fall through to other '#'-branches. */
/* RD1: build a type Form from the text between the < > of a #json<...> hint.
 * A single token (User) yields a symbol Form; whitespace-separated tokens
 * (Result User E) yield an application list (Result User E).  The hint text
 * never contains parens (the scanner stops at '('), so this simple split
 * covers the typed-decode cases. */
static Form *read_type_hint(Reader *r, const char *text, size_t len, Span span) {
    Form *toks[16];
    size_t ntoks = 0;
    size_t i = 0;
    while (i < len) {
        while (i < len && (text[i] == ' ' || text[i] == '\t')) i++;
        size_t start = i;
        while (i < len && text[i] != ' ' && text[i] != '\t') i++;
        if (i > start) {
            if (ntoks == 16) {
                diag_emit(DIAG_ERROR, span,
                          "#json<...>: type hint has too many tokens (TUR-E0270)");
                return NULL;
            }
            const Symbol *sym =
                symtab_intern(r->st, strslice(text + start, (uint32_t)(i - start)));
            toks[ntoks++] = form_sym(r->arena, span, sym);
        }
    }
    if (ntoks == 0) {
        diag_emit(DIAG_ERROR, span,
                  "#json<...>: expected a type name after '<' (TUR-E0270)");
        return NULL;
    }
    if (ntoks == 1) return toks[0];
    Form **items = (Form **)arena_alloc(r->arena, ntoks * sizeof(Form *));
    for (size_t k = 0; k < ntoks; k++) items[k] = toks[k];
    return form_list(r->arena, span, items, (uint32_t)ntoks);
}

static Form *try_read_json(Reader *r) {
    if (!(peek_at(r, 1) == 'j' && peek_at(r, 2) == 's' &&
          peek_at(r, 3) == 'o' && peek_at(r, 4) == 'n')) {
        return NULL;
    }
    /* The form is #json(...) or, with a type hint, #json<Type>(...). */
    if (peek_at(r, 5) != '(' && peek_at(r, 5) != '<') return NULL;

    uint32_t sl = r->line, sc = r->col;
    size_t   so = r->pos;
    advance(r); advance(r); advance(r); advance(r); advance(r); /* "#json" */

    /* RD1: optional <Type> hint.  When present, the emitted json node tree is
     * wrapped in an ascription (:: <node> Type) so return-type-directed
     * dispatch (Phase RT) and ordinary type checking can consume it.  The hint
     * accepts a single type name (User) or a whitespace-separated applied type
     * (Result User E), which becomes (Result User E). */
    Form *type_form = NULL;
    if (peek(r) == '<') {
        advance(r); /* consume '<' */
        size_t name_off = r->pos;
        while (peek(r) != '>' && peek(r) != '(' && peek(r) != ')' &&
               peek(r) != '\n' && peek(r) != -1) {
            advance(r);
        }
        size_t name_len = r->pos - name_off;
        if (name_len == 0) {
            diag_emit(DIAG_ERROR, span_point(r),
                      "#json<...>: expected a type name after '<' (TUR-E0270)");
            r->error = true; return NULL;
        }
        if (peek(r) != '>') {
            diag_emit(DIAG_ERROR, span_point(r),
                      "#json<...>: expected '>' to close the type hint (TUR-E0270)");
            r->error = true; return NULL;
        }
        Span tspan = span_from_to(r, sl, sc, name_off, r->pos);
        type_form = read_type_hint(r, r->src + name_off, name_len, tspan);
        if (!type_form) { r->error = true; return NULL; }
        advance(r); /* consume '>' */
    }

    if (peek(r) != '(') {
        Span s = span_from_to(r, sl, sc, so, r->pos);
        diag_emit(DIAG_ERROR, s,
                  "#json must be followed by '(' (TUR-E0270)");
        r->error = true; return NULL;
    }
    advance(r); /* consume '(' */

    Form *val = json_read_value(r);
    if (!val) return NULL;

    json_skip_ws(r);
    if (peek(r) != ')') {
        if (peek(r) == -1) {
            Span s = span_from_to(r, sl, sc, so, r->pos);
            diag_emit(DIAG_ERROR, s,
                      "#json: unexpected end of input, missing ')' (TUR-E0271)");
        } else {
            diag_emit(DIAG_ERROR, span_point(r),
                      "#json: expected ')' to close #json(...) (TUR-E0270)");
        }
        r->error = true;
        return NULL;
    }
    advance(r); /* consume ')' */

    /* RD1: wrap in (:: val Type) when a type hint was given. */
    if (type_form) {
        const Symbol *asc = symtab_intern(r->st, strslice("::", 2));
        Span s = span_from_to(r, sl, sc, so, r->pos);
        Form **items = (Form **)arena_alloc(r->arena, 3 * sizeof(Form *));
        items[0] = form_sym(r->arena, s, asc);
        items[1] = val;
        items[2] = type_form;
        return form_list(r->arena, s, items, 3);
    }
    return val;
}

/* RD2: #json-str<T>(expr) / #json-str?<T>(expr) -- typed decode of a runtime
 * JSON string, panicking or Result-returning.
 *
 *   #json-str<T>(e)   ==>  (:: (decode! (json/decode e)) T)
 *   #json-str?<T>(e)  ==>  (:: (catch-unwind
 *                               (fn [] : T (:: (decode! (json/decode e)) T)))
 *                             (Result T int))
 *
 * Unlike #json(...), the inner is an ordinary Turmeric expression (a :cstr at
 * runtime), read with the normal reader -- no JSON sub-parser is involved.
 *
 * The `?` form is the panicking one behind a catch boundary rather than a
 * second decode path: `HasSchema` has exactly one method and the schema lives
 * inside each instance's `decode!` body, so a reader macro has nothing else to
 * branch on.  Two things had to be true first, and both now are -- a schema
 * violation raises a catchable `panic` instead of calling `abort()`
 * (stdlib/schema.tur, schema-decode-abort), and `catch-unwind` over a thunk
 * returning a by-value aggregate no longer miscompiles (see
 * docs/archive/catch-unwind-aggregate-return-miscompiled.md); a typed decode
 * lands in a struct by definition, so it hit that every time.
 *
 * #json-file<T>(path) / #json-file?<T>(path) are the same family over
 * json/decode-file!, which panics on an unreadable path. */
static Form *try_read_json_str(Reader *r) {
    /* Two spellings share one grammar and one expansion shape:
     *   #json-str<T>(e)    decodes the JSON text e      -- (json/decode e)
     *   #json-file<T>(p)   decodes the JSON file at p   -- (json/decode-file! p)
     * The file form's own failure (an unreadable path) is a panic inside
     * json/decode-file!, so the `?` variant's catch boundary turns it into an
     * err exactly as it does a schema violation -- nothing here has to know. */
    static const char PFX_STR[]  = "#json-str";
    static const char PFX_FILE[] = "#json-file";
    const char *pfx = NULL; int plen = 0; const char *decoder = NULL;
    if (peek_at(r, 1) == 'j' && peek_at(r, 2) == 's' && peek_at(r, 3) == 'o' &&
        peek_at(r, 4) == 'n' && peek_at(r, 5) == '-') {
        if (peek_at(r, 6) == 's' && peek_at(r, 7) == 't' && peek_at(r, 8) == 'r') {
            pfx = PFX_STR; plen = 9; decoder = "json/decode";
        } else if (peek_at(r, 6) == 'f' && peek_at(r, 7) == 'i' && peek_at(r, 8) == 'l' &&
                   peek_at(r, 9) == 'e') {
            pfx = PFX_FILE; plen = 10; decoder = "json/decode-file!";
        }
    }
    if (!pfx) return NULL;
    int after = peek_at(r, plen);
    if (after != '<' && after != '?') return NULL;

    uint32_t sl = r->line, sc = r->col;
    size_t   so = r->pos;
    for (int k = 0; k < plen; k++) advance(r); /* the prefix */

    bool result_form = false;
    if (peek(r) == '?') {
        result_form = true;
        advance(r); /* consume '?' */
    }

    /* <Type> hint (required). */
    if (peek(r) != '<') {
        diag_emit(DIAG_ERROR, span_point(r),
                  "%s must be followed by a <Type> hint (TUR-E0270)", pfx);
        r->error = true; return NULL;
    }
    advance(r); /* consume '<' */
    size_t name_off = r->pos;
    while (peek(r) != '>' && peek(r) != '(' && peek(r) != ')' &&
           peek(r) != '\n' && peek(r) != -1) {
        advance(r);
    }
    size_t name_len = r->pos - name_off;
    if (name_len == 0) {
        diag_emit(DIAG_ERROR, span_point(r),
                  "%s<...>: expected a type name after '<' (TUR-E0270)", pfx);
        r->error = true; return NULL;
    }
    if (peek(r) != '>') {
        diag_emit(DIAG_ERROR, span_point(r),
                  "%s<...>: expected '>' to close the type hint (TUR-E0270)", pfx);
        r->error = true; return NULL;
    }
    Span tspan = span_from_to(r, sl, sc, name_off, r->pos);
    Form *type_form = read_type_hint(r, r->src + name_off, name_len, tspan);
    if (!type_form) { r->error = true; return NULL; }
    advance(r); /* consume '>' */

    if (peek(r) != '(') {
        diag_emit(DIAG_ERROR, span_point(r),
                  "%s<T> must be followed by '(' (TUR-E0270)", pfx);
        r->error = true; return NULL;
    }
    advance(r); /* consume '(' */

    skip_ws_and_comments(r);
    Form *expr = read_form(r);
    if (!expr || r->error) { r->error = true; return NULL; }
    skip_ws_and_comments(r);
    if (peek(r) != ')') {
        diag_emit(DIAG_ERROR, span_point(r),
                  "%s<T>(expr): expected ')' to close the expression (TUR-E0271)", pfx);
        r->error = true; return NULL;
    }
    advance(r); /* consume ')' */

    Span s = span_from_to(r, sl, sc, so, r->pos);
    /* (json/decode expr) -- or (json/decode-file! path) for the file form */
    Form *decode_arg = expr;
    Form *json_decode = json_call(r, s, decoder, &decode_arg, 1);
    /* (decode! (json/decode expr)) */
    Form *decoded = json_call(r, s, "decode!", &json_decode, 1);
    /* (:: (decode! (json/decode expr)) Type) */
    const Symbol *asc = symtab_intern(r->st, strslice("::", 2));
    Form **items = (Form **)arena_alloc(r->arena, 3 * sizeof(Form *));
    items[0] = form_sym(r->arena, s, asc);
    items[1] = decoded;
    items[2] = type_form;
    Form *ascribed = form_list(r->arena, s, items, 3);
    if (!result_form) return ascribed;

    /* (fn [] : T <ascribed>) -- the return annotation is not decoration: the
     * catch boundary reads the thunk's declared return type to decide whether
     * the value needs heap-boxing across the int64 result box. */
    Form **fnitems = (Form **)arena_alloc(r->arena, 4 * sizeof(Form *));
    fnitems[0] = form_sym(r->arena, s, symtab_intern(r->st, strslice("fn", 2)));
    fnitems[1] = form_vec(r->arena, s, NULL, 0);
    fnitems[2] = form_type_ann(r->arena, s, type_form);
    fnitems[3] = ascribed;
    Form *thunk = form_list(r->arena, s, fnitems, 4);

    /* (catch-unwind <thunk>) */
    Form **cuitems = (Form **)arena_alloc(r->arena, 2 * sizeof(Form *));
    cuitems[0] = form_sym(r->arena, s,
                          symtab_intern(r->st, strslice("catch-unwind", 12)));
    cuitems[1] = thunk;
    Form *caught = form_list(r->arena, s, cuitems, 2);

    /* (:: (catch-unwind ...) (Result T int)) -- the err slot carries the
     * panic payload handle, which `err?` / `panic-msg`-style readers consume;
     * `int` is the payload's carrier type at this surface. */
    Form **ritems = (Form **)arena_alloc(r->arena, 3 * sizeof(Form *));
    ritems[0] = form_sym(r->arena, s, symtab_intern(r->st, strslice("Result", 6)));
    ritems[1] = type_form;
    ritems[2] = form_sym(r->arena, s, symtab_intern(r->st, strslice("int", 3)));
    Form *result_ty = form_list(r->arena, s, ritems, 3);

    Form **aitems = (Form **)arena_alloc(r->arena, 3 * sizeof(Form *));
    aitems[0] = form_sym(r->arena, s, asc);
    aitems[1] = caught;
    aitems[2] = result_ty;
    return form_list(r->arena, s, aitems, 3);
}

/* RR: Is form an operator symbol (<, <=, >, >=, =)? */
static bool rr_is_op(const Form *f) {
    if (f->tag != F_SYM) return false;
    const char *n = f->as.sym->name;
    return (strcmp(n, "<") == 0 || strcmp(n, "<=") == 0 ||
            strcmp(n, ">") == 0 || strcmp(n, ">=") == 0 ||
            strcmp(n, "=") == 0);
}

/* RR: Is form a single-letter variable (a-z or A-Z, not an operator)? */
static bool rr_is_var(const Form *f) {
    if (f->tag != F_SYM) return false;
    const char *n = f->as.sym->name;
    return (n[1] == '\0' &&
            ((n[0] >= 'a' && n[0] <= 'z') || (n[0] >= 'A' && n[0] <= 'Z')));
}

/* RR0/RR1/RR2/RR4: Read a #r{...} range literal and desugar to a constructor call. */
static Form *read_range_literal(Reader *r) {
    uint32_t start_line = r->line;
    uint32_t start_col = r->col;
    size_t start_off = r->pos;

    advance(r); /* consume '#' */
    advance(r); /* consume 'r' */

    if (peek(r) != '{') {
        Span s = span_from_to(r, start_line, start_col, start_off, r->pos);
        diag_emit(DIAG_ERROR, s, "#r must be followed by '{' for range literal");
        r->error = true;
        return NULL;
    }
    advance(r); /* consume '{' */

    Form *toks[5];
    size_t ntoks = 0;

    for (;;) {
        skip_ws_and_comments(r);
        int c = peek(r);
        if (c == -1) {
            Span s = span_from_to(r, start_line, start_col, start_off, r->pos);
            diag_emit(DIAG_ERROR, s,
                      "#r{...}: unterminated range literal (missing '}')");
            r->error = true;
            return NULL;
        }
        if (c == '}') {
            advance(r);
            break;
        }
        if (ntoks == 5) {
            Span s = span_point(r);
            diag_emit(DIAG_ERROR, s,
                      "#r{...} expects 'var op form' or 'form op var op form', "
                      "got more than 5 tokens");
            r->error = true;
            return NULL;
        }
        Form *child = read_form(r);
        if (!child) return NULL;
        toks[ntoks++] = child;
    }

    Span span = span_from_to(r, start_line, start_col, start_off, r->pos);

    if (ntoks == 0) {
        diag_emit(DIAG_ERROR, span, "#r{} requires a range expression");
        r->error = true;
        return NULL;
    }
    if (ntoks != 3 && ntoks != 5) {
        diag_emit(DIAG_ERROR, span,
                  "#r{...} expects 'var op form' or 'form op var op form', "
                  "got %zu tokens", ntoks);
        r->error = true;
        return NULL;
    }

/* Intern a C string as an F_SYM using the whole range span. */
#define RR_SYM(name_cstr) \
    form_sym(r->arena, span, \
             symtab_intern(r->st, strslice((name_cstr), \
                                           (uint32_t)strlen(name_cstr))))

/* Promote an F_INT bound to F_FLOAT when use_float is set. */
#define RR_PROMOTE(fp) \
    ((use_float && (fp)->tag == F_INT) \
        ? form_float(r->arena, (fp)->span, (double)(fp)->as.i) \
        : (fp))

    if (ntoks == 3) {
        /* One-sided: var op form  |  form op var */
        Form *a = toks[0], *op_f = toks[1], *b = toks[2];

        if (!rr_is_op(op_f)) {
            diag_emit(DIAG_ERROR, op_f->span,
                      "#r{...}: expected a comparison operator, got '%s'",
                      op_f->tag == F_SYM ? op_f->as.sym->name : "<expr>");
            r->error = true;
            return NULL;
        }
        const char *op = op_f->as.sym->name;

        bool a_var = rr_is_var(a);
        bool b_var = rr_is_var(b);

        if (!a_var && !b_var) {
            diag_emit(DIAG_ERROR, span,
                      "#r{...}: expected a single-letter variable on one side");
            r->error = true;
            return NULL;
        }

        /* If both look like vars, treat left as var, right as the bound form. */
        Form *bound = a_var ? b : a;
        bool var_first = a_var;

        /* Canonicalize to var-op-form: flip operator when form is on the left. */
        if (!var_first) {
            if      (strcmp(op, ">")  == 0) op = "<";
            else if (strcmp(op, ">=") == 0) op = "<=";
            else if (strcmp(op, "<")  == 0) op = ">";
            else if (strcmp(op, "<=") == 0) op = ">=";
            /* "=" is symmetric -- no flip needed */
        }

        bool use_float = (bound->tag == F_FLOAT);
        bound = RR_PROMOTE(bound);

        const char *ctor;
        if      (strcmp(op, "=")  == 0) ctor = use_float ? "float-singleton-range"    : "singleton-range";
        else if (strcmp(op, "<")  == 0) ctor = use_float ? "float-less-than-range"    : "less-than-range";
        else if (strcmp(op, "<=") == 0) ctor = use_float ? "float-at-most-range"      : "at-most-range";
        else if (strcmp(op, ">")  == 0) ctor = use_float ? "float-greater-than-range" : "greater-than-range";
        else                             ctor = use_float ? "float-at-least-range"     : "at-least-range";

        const Symbol *var_sym = (a_var ? a : b)->as.sym;
        Form **out = (Form **)arena_alloc(r->arena, 2 * sizeof(Form *));
        out[0] = RR_SYM(ctor);
        out[1] = bound;
        Form *range = form_list(r->arena, span, out, 2);
        return form_range_var(r->arena, span, var_sym, range);
    }

    /* ntoks == 5: two-sided: left_f op1 var op2 right_f */
    Form *left_f  = toks[0];
    Form *op1_f   = toks[1];
    Form *var_f   = toks[2];
    Form *op2_f   = toks[3];
    Form *right_f = toks[4];

    if (!rr_is_op(op1_f)) {
        diag_emit(DIAG_ERROR, op1_f->span,
                  "#r{...}: expected a comparison operator, got '%s'",
                  op1_f->tag == F_SYM ? op1_f->as.sym->name : "<expr>");
        r->error = true;
        return NULL;
    }
    if (!rr_is_op(op2_f)) {
        diag_emit(DIAG_ERROR, op2_f->span,
                  "#r{...}: expected a comparison operator, got '%s'",
                  op2_f->tag == F_SYM ? op2_f->as.sym->name : "<expr>");
        r->error = true;
        return NULL;
    }
    if (!rr_is_var(var_f)) {
        diag_emit(DIAG_ERROR, var_f->span,
                  "#r{...}: expected a single-letter variable in the middle position");
        r->error = true;
        return NULL;
    }

    const char *op1 = op1_f->as.sym->name;
    const char *op2 = op2_f->as.sym->name;

    if (strcmp(op1, "=") == 0 || strcmp(op2, "=") == 0) {
        diag_emit(DIAG_ERROR, span, "#r{...}: '=' is only valid in a one-sided range");
        r->error = true;
        return NULL;
    }

    bool op1_fwd = (strcmp(op1, "<") == 0 || strcmp(op1, "<=") == 0);
    bool op2_fwd = (strcmp(op2, "<") == 0 || strcmp(op2, "<=") == 0);

    if (op1_fwd != op2_fwd) {
        diag_emit(DIAG_ERROR, span,
                  "#r{...}: cannot mix '<'/'<=' and '>'/>=' in a two-sided range");
        r->error = true;
        return NULL;
    }

    bool use_float = (left_f->tag == F_FLOAT || right_f->tag == F_FLOAT);

    /* Note when one literal bound is float but the other is a runtime expression. */
    if (use_float && (left_f->tag != F_INT && left_f->tag != F_FLOAT)) {
        diag_emit(DIAG_NOTE, span,
                  "#r{...}: mixed literal types; using float constructors -- "
                  "ensure the expression produces a float");
    } else if (use_float && (right_f->tag != F_INT && right_f->tag != F_FLOAT)) {
        diag_emit(DIAG_NOTE, span,
                  "#r{...}: mixed literal types; using float constructors -- "
                  "ensure the expression produces a float");
    }

    Form *ctor_lo, *ctor_hi;
    bool lo_incl, hi_incl;

    if (op1_fwd) {
        /* Left-to-right: left_f op1 var op2 right_f  (e.g. 0 <= n < 10) */
        ctor_lo = left_f;
        ctor_hi = right_f;
        lo_incl = (strcmp(op1, "<=") == 0);
        hi_incl = (strcmp(op2, "<=") == 0);
    } else {
        /* Right-to-left: left_f op1 var op2 right_f  (e.g. 10 > n >= 0)
         * left_f is the hi bound in the source; right_f is the lo bound.
         * Constructor always receives (ctor lo hi). */
        ctor_lo = right_f;
        ctor_hi = left_f;
        hi_incl = (strcmp(op1, ">=") == 0); /* op1: hi op1 var */
        lo_incl = (strcmp(op2, ">=") == 0); /* op2: var op2 lo */
    }

    /* Compile-time empty-range warning for integer literal bounds (RR1). */
    if (ctor_lo->tag == F_INT && ctor_hi->tag == F_INT) {
        int64_t lo_v = ctor_lo->as.i;
        int64_t hi_v = ctor_hi->as.i;
        bool empty = (lo_incl && hi_incl) ? (lo_v > hi_v) : (lo_v >= hi_v);
        if (empty) {
            diag_emit(DIAG_WARNING, span, "#r{...}: range is provably empty");
        }
    }

    ctor_lo = RR_PROMOTE(ctor_lo);
    ctor_hi = RR_PROMOTE(ctor_hi);

    const char *ctor;
    if      (lo_incl && hi_incl)   ctor = use_float ? "float-closed-range"      : "closed-range";
    else if (!lo_incl && !hi_incl)  ctor = use_float ? "float-open-range"        : "open-range";
    else if (lo_incl && !hi_incl)   ctor = use_float ? "float-closed-open-range" : "closed-open-range";
    else                             ctor = use_float ? "float-open-closed-range" : "open-closed-range";

    Form **out = (Form **)arena_alloc(r->arena, 3 * sizeof(Form *));
    out[0] = RR_SYM(ctor);
    out[1] = ctor_lo;
    out[2] = ctor_hi;
    Form *range = form_list(r->arena, span, out, 3);
    return form_range_var(r->arena, span, var_f->as.sym, range);

#undef RR_SYM
#undef RR_PROMOTE
}

/* Phase R5: Read an attribute form: #[...] */
static Form *read_attribute(Reader *r) {
    uint32_t start_line = r->line;
    uint32_t start_col = r->col;
    size_t start_off = r->pos;

    advance(r); /* consume '#' */
    if (peek(r) != '[') {
        Span s = span_from_to(r, start_line, start_col, start_off, r->pos);
        diag_emit(DIAG_ERROR, s, "expected '[' after '#' for attribute");
        r->error = true;
        return NULL;
    }
    advance(r); /* consume '[' */

    /* Read the attribute name */
    skip_ws_and_comments(r);
    Form *attr_name = read_form(r);
    if (!attr_name) {
        r->error = true;
        return NULL;
    }

    /* Check for closing ] */
    skip_ws_and_comments(r);
    if (peek(r) != ']') {
        Span s = span_from_to(r, start_line, start_col, start_off, r->pos);
        diag_emit(DIAG_ERROR, s, "expected ']' to close attribute");
        r->error = true;
        return NULL;
    }
    advance(r); /* consume ']' */

    Span span = span_from_to(r, start_line, start_col, start_off, r->pos);
    
    if (attr_name->tag != F_SYM) {
        diag_emit(DIAG_ERROR, attr_name->span, "attribute name must be a symbol");
        r->error = true;
        return NULL;
    }
    
    /* Create a symbol with "#" prefix: #no-unwind */
    char attr_name_buf[64];
    int written = snprintf(attr_name_buf, sizeof(attr_name_buf), "#%s", attr_name->as.sym->name);
    if (written >= (int)sizeof(attr_name_buf)) {
        diag_emit(DIAG_ERROR, span, "attribute name too long");
        r->error = true;
        return NULL;
    }
    StrSlice attr_slice = strslice(attr_name_buf, (uint32_t)written);
    const Symbol *attr_sym = symtab_intern(r->st, attr_slice);
    return form_sym(r->arena, span, attr_sym);
}

/* Read a C code block: ```c ... ``` or ``` c ... ``` */
static Form *read_cblock(Reader *r) {
    uint32_t start_line = r->line;
    uint32_t start_col = r->col;
    size_t start_off = r->pos;
    
    /* Expect ``` */
    if (peek(r) != '`') {
        Span s = span_point(r);
        diag_emit(DIAG_ERROR, s, "expected '`' for C code block");
        r->error = true;
        return NULL;
    }
    advance(r); /* consume '`' */
    if (peek(r) != '`') {
        Span s = span_point(r);
        diag_emit(DIAG_ERROR, s, "expected '`' for C code block");
        r->error = true;
        return NULL;
    }
    advance(r); /* consume '`' */
    if (peek(r) != '`') {
        Span s = span_point(r);
        diag_emit(DIAG_ERROR, s, "expected '`' for C code block");
        r->error = true;
        return NULL;
    }
    advance(r); /* consume '`' */
    
    /* Now expect 'c' (possibly after whitespace) */
    skip_ws_and_comments(r);
    if (peek(r) != 'c') {
        Span s = span_point(r);
        diag_emit(DIAG_ERROR, s, "expected 'c' for C code block (use ```c ... ```)");
        r->error = true;
        return NULL;
    }
    advance(r); /* consume 'c' */
    skip_ws_and_comments(r);
    
    /* Now read the C code until we find ``` */
    size_t code_start = r->pos;
    
    while (peek(r) != -1) {
        if (peek(r) == '`' && peek2(r) == '`' && peek3(r) == '`') {
            /* Found ``` */
            advance(r); advance(r); advance(r);
            break;
        }
        advance(r);
    }
    
    if (peek(r) == -1) {
        Span s = span_point(r);
        diag_emit(DIAG_ERROR, s, "unterminated C code block (missing ```)");
        r->error = true;
        return NULL;
    }
    
    size_t code_end = r->pos - 3; /* don't include the ``` */
    StrSlice code = { r->src + code_start, (uint32_t)(code_end - code_start) };
    Span span = span_from_to(r, start_line, start_col, start_off, r->pos);
    return form_cblock(r->arena, span, code);
}

/* Phase S2: Helper to check if neoteric bracket follows */
/* Returns the bracket char if found (with no whitespace), or -1 if not */
static int peek_neoteric_bracket(const Reader *r) {
    int c = peek(r);
    if (c == '(' || c == '{' || c == '[') {
        return c;
    }
    return -1;
}

static Form *read_curly_infix(Reader *r);

/* Phase S2: Read neoteric bracketed expression after an atom */
/* f(expr) -> (f expr), f{expr} -> (f expr), f[expr] -> (bracketapply f expr) */
static Form *read_neoteric_bracket(Reader *r, Form *atom, int bracket) {
    uint32_t start_line = r->line;
    uint32_t start_col = r->col;
    size_t start_off = r->pos;

    /* `#lang r7rs/sweet` (r7rs-sweet-base-dialect-missing) reads neoteric
     * as SRFI-105 specifies it for Scheme: `f{n - 1}` is `(f {n - 1})`, one
     * argument -- `(fact (- n 1))`, the SRFI's own example -- and `f{}` is
     * `(f)`.  `f[x]` is `f(x)`: Scheme's `[...]` are parens.  The Turmeric
     * readers keep their own reading of both, below. */
    if (r->scheme_enabled && bracket == '{') {
        Form *arg = read_curly_infix(r);
        if (!arg) return NULL;
        bool empty = arg->tag == F_LIST && arg->as.list.len == 0;
        Form **call_items = (Form **)arena_alloc(r->arena, 2 * sizeof(Form *));
        call_items[0] = atom;
        call_items[1] = arg;
        Span span = span_from_to(r, start_line, start_col, start_off, r->pos);
        Form *result = form_list(r->arena, span, call_items, empty ? 1 : 2);
        int next = peek_neoteric_bracket(r);
        if (next != -1) return read_neoteric_bracket(r, result, next);
        return result;
    }
    
    /* Consume the opening bracket */
    advance(r);
    
    /* Read the content inside the bracket */
    Form **items = NULL;
    size_t cap = 0, n = 0;
    char close_bracket;
    
    switch (bracket) {
        case '(': close_bracket = ')'; break;
        case '{': close_bracket = '}'; break;
        case '[': close_bracket = ']'; break;
        default: return atom; /* Shouldn't happen */
    }
    
    for (;;) {
        skip_ws_and_comments(r);
        int c = peek(r);
        if (c == -1) {
            Span s = span_from_to(r, start_line, start_col, start_off, r->pos);
            const char *bracket_name = (bracket == '(') ? "parentheses" : 
                                       (bracket == '{') ? "braces" : "brackets";
            diag_emit(DIAG_ERROR, s, "unterminated neoteric %s (missing '%c')", 
                      bracket_name, close_bracket);
            r->error = true;
            free(items);
            return NULL;
        }
        if (c == close_bracket) {
            advance(r);
            break;
        }
        Form *child = read_form(r);
        if (!child) {
            free(items);
            return NULL;
        }
        if (n == cap) {
            cap = cap ? cap * 2 : 4;
            items = (Form **)realloc(items, cap * sizeof(Form *));
            if (!items) { fprintf(stderr, "tur: oom\n"); abort(); }
        }
        items[n++] = child;
    }
    
    /* Create the final call form */
    Span span = span_from_to(r, start_line, start_col, start_off, r->pos);
    
    /* For neoteric: f(x y z) -> (f x y z), not (f (x y z)) */
    /* We need to spread the items as separate arguments */
    
    Form **call_items = (Form **)arena_alloc(r->arena, (n + 1) * sizeof(Form *));
    call_items[0] = atom;
    for (uint32_t i = 0; i < n; i++) {
        call_items[i + 1] = items[i];
    }
    free(items);
    
    Form *result;
    if (bracket == '[' && n == 1 && !r->scheme_enabled) {
        /* Special case: f[x] -> (bracketapply f x) */
        const Symbol *bracketapply_sym = symtab_intern(r->st, strslice("bracketapply", 12));
        Form **ba_items = (Form **)arena_alloc(r->arena, 3 * sizeof(Form *));
        ba_items[0] = form_sym(r->arena, span, bracketapply_sym);
        ba_items[1] = atom;
        ba_items[2] = call_items[1]; /* the single argument */
        /* call_items is arena memory: no free.  A free() here was an invalid
         * free of an arena slab interior -- any `f[x]` in a neoteric or
         * sweet-exp file crashed the reader (`tur check`, the LSP), found by
         * tests/fuzz/fuzz_reader (security-audit-plan WP4). */
        result = form_list(r->arena, span, ba_items, 3);
    } else {
        result = form_list(r->arena, span, call_items, n + 1);
    }

    /* Phase S2: neoteric chaining. Per SRFI-105, application chains:
     * f(x)(y) -> ((f x) y), not ((f x) (y)). If another neoteric bracket
     * immediately follows (no whitespace), apply the just-built form to it. */
    if (r->neoteric_enabled) {
        int next = peek_neoteric_bracket(r);
        if (next != -1) {
            return read_neoteric_bracket(r, result, next);
        }
    }

    return result;
}

/* Phase S1: Read curly-infix expression {a + b} -> (+ a b) */
static Form *read_curly_infix(Reader *r) {
    uint32_t start_line = r->line;
    uint32_t start_col = r->col;
    size_t start_off = r->pos;
    advance(r); /* consume '{' */

    Form **items = NULL;
    size_t cap = 0, n = 0;

    for (;;) {
        skip_ws_and_comments(r);
        int c = peek(r);
        if (c == -1) {
            Span s = span_from_to(r, start_line, start_col, start_off, r->pos);
            diag_emit(DIAG_ERROR, s, "unterminated curly-infix expression (missing '}')");
            r->error = true;
            free(items);
            return NULL;
        }
        if (c == '}') {
            advance(r);
            break;
        }
        Form *child = read_form(r);
        if (!child) {
            free(items);
            return NULL;
        }
        if (n == cap) {
            cap = cap ? cap * 2 : 4;
            items = (Form **)realloc(items, cap * sizeof(Form *));
            if (!items) { fprintf(stderr, "tur: oom\n"); abort(); }
        }
        items[n++] = child;
    }

    /* Handle special cases per SRFI-105 */
    if (n == 0) {
        /* Empty { } */
        Span span = span_from_to(r, start_line, start_col, start_off, r->pos);
        return form_list(r->arena, span, NULL, 0);
    } else if (n == 1) {
        /* {e} -> e */
        Form *only = items[0];
        free(items);
        return only;
    } else if (n == 2) {
        /* {e1 e2} -> (e1 e2) - function call */
        Span span = span_from_to(r, start_line, start_col, start_off, r->pos);
        Form *result = form_list(r->arena, span, items, (uint32_t)n);
        free(items);
        return result;
    }

    /* Per SRFI-105, a "simple" infix list {a op b op c ...} has odd length
     * with the operators at the odd indices (1, 3, 5, ...) and operands at
     * the even indices (0, 2, 4, ...). When every operator slot holds the
     * same symbol it lowers to a prefix call (op a b c ...); anything else
     * (even length, a non-symbol in an operator slot, or mixed operators)
     * falls back to the $nfx$ marker.
     *
     * Detecting operators positionally is essential: scanning for the first
     * F_SYM anywhere would mistake a symbol *operand* (e.g. the `x` in
     * {x * x} or the `width` in {width * height}) for the operator and
     * spuriously emit $nfx$. */
    bool simple_infix = (n % 2 == 1);
    const Symbol *first_op = NULL;

    if (simple_infix) {
        for (uint32_t i = 1; i < n; i += 2) {
            if (items[i]->tag != F_SYM) { simple_infix = false; break; }
            if (first_op == NULL) {
                first_op = items[i]->as.sym;
            } else if (items[i]->as.sym != first_op) {
                simple_infix = false;
                break;
            }
        }
    }

    Span span = span_from_to(r, start_line, start_col, start_off, r->pos);

    if (simple_infix && first_op != NULL) {
        /* Homogeneous simple infix: operands are the even-indexed items. */
        uint32_t op_count = (n + 1) / 2;
        Form **call_items = (Form **)arena_alloc(r->arena, (op_count + 1) * sizeof(Form *));
        call_items[0] = form_sym(r->arena, span, first_op);
        uint32_t k = 1;
        for (uint32_t i = 0; i < n; i += 2) {
            call_items[k++] = items[i];
        }

        free(items);
        return form_list(r->arena, span, call_items, op_count + 1);
    } else {
        /* Mixed operators or no operators - use $nfx$ macro */
        /* Create ($nfx$ a + b * c) */
        const Symbol *nfx_sym = symtab_intern(r->st, strslice("$nfx$", 5));
        Form **nfx_items = (Form **)arena_alloc(r->arena, (n + 1) * sizeof(Form *));
        nfx_items[0] = form_sym(r->arena, span, nfx_sym);
        for (uint32_t i = 0; i < n; i++) {
            nfx_items[i + 1] = items[i];
        }
        free(items);
        return form_list(r->arena, span, nfx_items, n + 1);
    }
}

/* CT0: Read a contract type annotation: { var : T | pred }
 * Produces an F_CONTRACT_TYPE form with items:
 *   [0] = F_SYM(var)
 *   [1] = F_TYPE_ANN(T)  (or F_KEYWORD(:T))
 *   [2] = F_SYM("|")
 *   [3] = predicate form (any expression)
 * The outer braces are consumed here. */
static Form *read_contract_type(Reader *r) {
    uint32_t start_line = r->line;
    uint32_t start_col = r->col;
    size_t start_off = r->pos;

    advance(r); /* consume '{' */

    /* Collect items until '}' */
    Form **items = NULL;
    size_t cap = 0, n = 0;

    for (;;) {
        skip_ws_and_comments(r);
        int c = peek(r);
        if (c == -1) {
            Span s = span_from_to(r, start_line, start_col, start_off, r->pos);
            diag_emit(DIAG_ERROR, s, "unterminated contract type (missing '}')");
            r->error = true;
            free(items);
            return NULL;
        }
        if (c == '}') {
            advance(r);
            break;
        }
        Form *child = read_form(r);
        if (!child) {
            free(items);
            return NULL;
        }
        if (n == cap) {
            cap = cap ? cap * 2 : 4;
            items = (Form **)realloc(items, cap * sizeof(Form *));
            if (!items) { fprintf(stderr, "tur: oom\n"); abort(); }
        }
        items[n++] = child;
    }

    Span span = span_from_to(r, start_line, start_col, start_off, r->pos);
    Form *f = form_contract_type(r->arena, span, items, (uint32_t)n);
    free(items);
    return f;
}

/* ------------------------------------------------------------------------
 * RM1: user-defined #-dispatch reader macros (template-only, raw bodies).
 * Placed here, below the basic Reader helpers (peek/advance/span_*), and
 * above read_form, which calls try_read_user_macro on the '#' branch.
 * ------------------------------------------------------------------------ */

/* Identifier char class for macro names. Per the plan's open question #2,
 * we delegate to the symbol reader so names like `#if+`, `#?some`,
 * `#<-bind` work, with one carve-out: `#` is *not* a continuation byte for
 * macro names — otherwise `#foo#bar` would parse as a single macro name
 * `foo#bar` instead of two adjacent `#`-dispatches. */
static bool is_macro_id_start(int c) {
    return is_sym_start(c);
}
static bool is_macro_id_cont(int c) {
    if (c == '#') return false;
    return is_sym_cont(c);
}

static int matching_close_for(int open) {
    switch (open) {
        case '(': return ')';
        case '[': return ']';
        case '{': return '}';
        default:  return 0;
    }
}

/* Read a raw (verbatim) macro body. The reader is positioned at the
 * open-delim; on success leaves it past the matched close-delim. Tracks
 * nesting and recognises `\X` escapes (the backslash is dropped, the next
 * byte is kept literally — including the close-delim, which does not
 * affect depth). Returns the body bytes (arena-allocated, NUL-terminated)
 * and sets *out_len. On unterminated input, emits a diagnostic and returns
 * NULL. */
static char *read_raw_body(Reader *r, int open, int close, uint32_t *out_len) {
    uint32_t open_line = r->line;
    uint32_t open_col  = r->col;
    size_t   open_off  = r->pos;
    advance(r); /* consume open delim */

    /* Two-pass: find the matched close + the unescaped length, then copy. */
    size_t scan      = r->pos;
    size_t scan_end  = 0;
    size_t out_n     = 0;
    int    depth     = 1;
    bool   ok        = false;
    while (scan < r->len) {
        char c = r->src[scan];
        if (c == '\\' && scan + 1 < r->len) {
            scan += 2;
            out_n += 1;
            continue;
        }
        if (c == open && open != close) {
            depth++; scan++; out_n++; continue;
        }
        if (c == close) {
            depth--;
            if (depth == 0) { ok = true; scan_end = scan; break; }
            scan++; out_n++; continue;
        }
        scan++; out_n++;
    }
    if (!ok) {
        diag_emit(DIAG_ERROR,
                  span_from_to(r, open_line, open_col, open_off, r->pos),
                  "unterminated reader macro body (missing '%c')",
                  (char)close);
        r->error = true;
        return NULL;
    }

    char *buf = (char *)arena_alloc_aligned(r->arena, out_n + 1, 1);
    size_t bi = 0;
    while (r->pos < scan_end) {
        int c = peek(r);
        if (c == '\\' && r->pos + 1 < r->len) {
            advance(r); /* drop the backslash */
            buf[bi++] = (char)advance(r);
            continue;
        }
        buf[bi++] = (char)advance(r);
    }
    buf[bi] = '\0';
    advance(r); /* consume close delim */
    *out_len = (uint32_t)bi;
    return buf;
}

/* Deep-clone a form tree, overriding every node's span with `span`. Used
 * by the macro expanders so that the expanded tree carries the call-site
 * span (e.g. the `#foo{...}` location), not the registration-site span
 * the template was originally parsed with. The payload (literal value,
 * symbol pointer, raw bytes, ...) is shared with the source form — only
 * the `Form` envelope and any child `Form *` slots are freshly allocated.
 */
static Form *form_clone_with_span(Arena *a, const Form *f, Span span) {
    if (!f) return NULL;
    Form *out = form_new(a, f->tag, span);
    out->lit_suffix = f->lit_suffix;
    memcpy(&out->as, &f->as, sizeof(f->as));
    switch (f->tag) {
        case F_LIST: case F_VEC: case F_MAP: case F_SET:
        case F_QUOTE: case F_QUASIQUOTE:
        case F_UNQUOTE: case F_UNQUOTE_SPLICING:
        case F_TYPE_ANN: case F_CONTRACT_TYPE:
        case F_READER_COND: case F_RANGE_VAR: {
            uint32_t n = f->as.list.len;
            Form **items = (Form **)arena_alloc(
                a, sizeof(Form *) * (n ? n : 1));
            for (uint32_t i = 0; i < n; ++i) {
                items[i] = form_clone_with_span(a, f->as.list.items[i], span);
            }
            out->as.list.items = items;
            out->as.list.len = n;
            break;
        }
        default:
            /* Scalar payload was copied by the memcpy above. */
            break;
    }
    return out;
}

/* RM2: parse a template splice marker `$N` (N >= 1). Returns N, or 0 if
 * `f` is not such a marker. Leading zeros are rejected so the syntax
 * stays unambiguous. */
static int try_dollar_index(const Form *f) {
    if (!f || f->tag != F_SYM || !f->as.sym) return 0;
    const Symbol *s = f->as.sym;
    if (s->len < 2 || s->name[0] != '$') return 0;
    if (s->name[1] < '1' || s->name[1] > '9') return 0;
    int n = 0;
    for (uint32_t i = 1; i < s->len; ++i) {
        char c = s->name[i];
        if (c < '0' || c > '9') return 0;
        n = n * 10 + (c - '0');
        if (n > 9999) return 0;
    }
    return n;
}

static bool is_dollar_body_form(const Form *f) {
    return f && f->tag == F_SYM && f->as.sym
        && f->as.sym->len == 5
        && memcmp(f->as.sym->name, "$body", 5) == 0;
}

/* Walk a template form tree, replacing the symbol `$body` with a string
 * literal of the body. Sub-trees are always rebuilt so that every node in
 * the result carries the call-site span (so diagnostics on the expansion
 * point at the `#foo{...}` use site, not the `(reader-macros/define ...)`
 * registration site). Only F_LIST and F_VEC are recursed into for
 * substitution — quoted / quasiquoted sub-trees are span-rewritten but
 * their symbols are left intact, matching the "templates are dumb"
 * stance in the plan. */
static Form *expand_raw_template(Reader *r, const Form *t,
                                 const char *body, uint32_t body_len,
                                 Span call_site) {
    if (!t) return NULL;
    if (t->tag == F_SYM && t->as.sym
        && t->as.sym->len == 5
        && memcmp(t->as.sym->name, "$body", 5) == 0) {
        return form_str(r->arena, call_site, body, body_len);
    }
    if (t->tag == F_LIST || t->tag == F_VEC) {
        uint32_t n = t->as.list.len;
        Form **items = (Form **)arena_alloc(
            r->arena, sizeof(Form *) * (n ? n : 1));
        for (uint32_t i = 0; i < n; ++i) {
            items[i] = expand_raw_template(r, t->as.list.items[i],
                                           body, body_len, call_site);
        }
        return (t->tag == F_LIST)
            ? form_list(r->arena, call_site, items, n)
            : form_vec(r->arena, call_site, items, n);
    }
    /* Anything else — including quoted sub-trees, keywords, literals —
     * gets a structural clone with the call-site span everywhere. */
    return form_clone_with_span(r->arena, t, call_site);
}

/* RM2: walk a template tree replacing splice markers with parsed body
 * forms. Two replacement rules apply inside any F_LIST/F_VEC child slot:
 *
 *   - the symbol `$body` is replaced by *all* body forms, spliced into
 *     the surrounding sequence (so `(foo $body)` with body `[a b c]`
 *     becomes `(foo a b c)`);
 *   - the symbol `$N` (N >= 1) is replaced in-place by the N-th body
 *     form (1-indexed). Referring past the body is a diagnostic.
 *
 * At the top level (template position itself), `$body` produces a
 * synthesized F_LIST of the items and `$N` produces a single item.
 *
 * Template-derived nodes are rebuilt with the call-site span; body forms
 * retain their original spans (they came from user code at the call site
 * so their existing parse-time spans already point at the right place). */
static Form *expand_datum_template(Reader *r, const Form *t,
                                   Form **items, uint32_t body_n,
                                   Span call_site) {
    if (!t) return NULL;
    if (is_dollar_body_form(t)) {
        return form_list(r->arena, call_site, items, body_n);
    }
    int idx;
    if ((idx = try_dollar_index(t)) > 0) {
        if ((uint32_t)idx > body_n) {
            diag_emit(DIAG_ERROR, t->span,
                      "reader macro template references $%d but body has "
                      "only %u form%s",
                      idx, (unsigned)body_n, body_n == 1 ? "" : "s");
            r->error = true;
            return NULL;
        }
        return items[idx - 1];  /* keep the body form's own span */
    }
    if (t->tag == F_LIST || t->tag == F_VEC) {
        uint32_t in_n = t->as.list.len;
        /* Worst case: every child is `$body` and there are body_n items. */
        uint32_t cap  = in_n + body_n + 1;
        Form  **out   = (Form **)arena_alloc(r->arena, sizeof(Form *) * cap);
        uint32_t out_n = 0;
        for (uint32_t i = 0; i < in_n; ++i) {
            const Form *child = t->as.list.items[i];
            if (is_dollar_body_form(child)) {
                if (out_n + body_n > cap) {
                    cap = (out_n + body_n) * 2;
                    Form **bigger = (Form **)arena_alloc(
                        r->arena, sizeof(Form *) * cap);
                    memcpy(bigger, out, sizeof(Form *) * out_n);
                    out = bigger;
                }
                for (uint32_t j = 0; j < body_n; ++j) out[out_n++] = items[j];
                continue;
            }
            int cidx = try_dollar_index(child);
            if (cidx > 0) {
                if ((uint32_t)cidx > body_n) {
                    diag_emit(DIAG_ERROR, child->span,
                              "reader macro template references $%d but "
                              "body has only %u form%s",
                              cidx, (unsigned)body_n,
                              body_n == 1 ? "" : "s");
                    r->error = true;
                    return NULL;
                }
                out[out_n++] = items[cidx - 1];
                continue;
            }
            Form *expanded =
                expand_datum_template(r, child, items, body_n, call_site);
            if (r->error) return NULL;
            out[out_n++] = expanded;
        }
        return (t->tag == F_LIST)
            ? form_list(r->arena, call_site, out, out_n)
            : form_vec(r->arena, call_site, out, out_n);
    }
    /* Anything else (atoms, quotes, keywords, ...) — clone with call_site. */
    return form_clone_with_span(r->arena, t, call_site);
}

/* `#dedent"""..."""` -- indent-aware, verbatim multi-line string literal.
 *
 * Reader is positioned just past `#dedent`, at the first `#` of an optional
 * hash run or at the opening `"""`.  `#dedent#"""..."""#` (any number of
 * hashes, matched on both ends) lets the body contain `"""`.  The body is
 * VERBATIM: no escapes, so code with `\n`, `{`, `}` and `"` needs nothing.
 *
 *   - the opening line must be blank; the text starts on the next line;
 *   - leading blank lines are dropped;
 *   - if the closing delimiter is alone on its line, ITS indentation is the
 *     amount stripped (Swift's rule) and the line break before it is not part
 *     of the string -- put a blank line before it for a final "\n";
 *   - otherwise (closer inline after text) the first non-blank line sets it;
 *   - every non-blank line must start with exactly that indent (an error
 *     otherwise); whitespace-only lines become empty;
 *   - a CR before a LF is dropped.
 * The result is an ordinary string literal, so it is a static cstr. */
static Form *read_dedent_literal(Reader *r, uint32_t call_line,
                                 uint32_t call_col, size_t call_off) {
    Span site = span_from_to(r, call_line, call_col, call_off, r->pos);
    const char *src = r->src;
    size_t p = r->pos;
    size_t hashes = 0;
    while (p < r->len && src[p] == '#') { hashes++; p++; }
    if (p + 3 > r->len || src[p] != '"' || src[p + 1] != '"'
        || src[p + 2] != '"') {
        diag_emit(DIAG_ERROR, site,
                  "#dedent expects '\"\"\"' (optionally after '#'s) "
                  "to open the string");
        r->error = true;
        return NULL;
    }
    size_t body_start = p + 3;
    size_t body_end = 0;
    bool   found = false;
    for (size_t q = body_start; q + 3 + hashes <= r->len; ++q) {
        if (src[q] != '"' || src[q + 1] != '"' || src[q + 2] != '"') continue;
        size_t h = 0;
        while (h < hashes && src[q + 3 + h] == '#') h++;
        if (h == hashes) { body_end = q; found = true; break; }
    }
    if (!found) {
        diag_emit(DIAG_ERROR, site,
                  "unterminated #dedent string (missing closing '\"\"\"' "
                  "followed by %zu '#')", hashes);
        r->error = true;
        return NULL;
    }
    /* The opening line is blank; the text starts after its newline. */
    size_t nl = body_start;
    while (nl < body_end && (src[nl] == ' ' || src[nl] == '\t'
                             || src[nl] == '\r')) nl++;
    if (nl >= body_end || src[nl] != '\n') {
        diag_emit(DIAG_ERROR, site,
                  "#dedent text must start on the line after the opening "
                  "'\"\"\"'");
        r->error = true;
        return NULL;
    }
    size_t text_start = nl + 1;

    /* The last line: alone with the closer, or the closer follows text. */
    size_t ls = body_end;
    while (ls > text_start && src[ls - 1] != '\n') ls--;
    bool own_line = true;
    for (size_t i = ls; i < body_end; ++i) {
        if (src[i] != ' ' && src[i] != '\t') { own_line = false; break; }
    }
    size_t region_end = body_end;
    if (own_line) region_end = (ls > text_start) ? ls - 1 : text_start;

    /* Indent: the closer's, else the first non-blank line's. */
    const char *indent = src + ls;
    size_t indent_len = own_line ? body_end - ls : 0;
    if (!own_line) {
        size_t a = text_start;
        while (a < region_end) {
            size_t e = a;
            while (e < region_end && src[e] != '\n') e++;
            size_t w = a;
            while (w < e && (src[w] == ' ' || src[w] == '\t')) w++;
            if (w < e && !(src[w] == '\r' && w + 1 == e)) {
                indent = src + a;
                indent_len = w - a;
                break;
            }
            a = e + 1;
        }
    }

    char  *buf = (char *)arena_alloc_aligned(r->arena, region_end - text_start + 1, 1);
    size_t o = 0;
    bool   started = false;
    size_t a = text_start;
    uint32_t lineno = 1;
    while (a <= region_end) {
        size_t e = a;
        while (e < region_end && src[e] != '\n') e++;
        size_t t = e;
        if (t > a && src[t - 1] == '\r') t--;
        size_t w = a;
        while (w < t && (src[w] == ' ' || src[w] == '\t')) w++;
        bool blank = (w == t);
        if (blank && !started) {
            /* a leading blank line is dropped */
        } else {
            if (started) buf[o++] = '\n';
            started = true;
            if (!blank) {
                if (t - a < indent_len || memcmp(src + a, indent, indent_len) != 0) {
                    diag_emit(DIAG_ERROR, site,
                              "#dedent line %u of the string is indented "
                              "less than the %s",
                              (unsigned)lineno,
                              own_line ? "closing '\"\"\"'" : "first line");
                    r->error = true;
                    return NULL;
                }
                memcpy(buf + o, src + a + indent_len, t - a - indent_len);
                o += t - a - indent_len;
            }
        }
        lineno++;
        if (e >= region_end) break;
        a = e + 1;
    }
    buf[o] = '\0';

    size_t stop = body_end + 3 + hashes;
    while (r->pos < stop) advance(r);
    return form_str(r->arena, span_from_to(r, call_line, call_col, call_off, r->pos),
                    buf, (uint32_t)o);
}

/* User-macro dispatch hook. Called from read_form when it sees a '#'.
 *
 * Returns:
 *   - a Form*  → consumed the input, produced an expansion
 *   - NULL with r->error=false → no user macro matched; rolled back to the
 *                                '#' so the caller can try built-ins
 *   - NULL with r->error=true  → matched but body was malformed
 *
 * In RM1 only RM_BODY_NONE (bare) and RM_BODY_RAW are honored end-to-end;
 * RM_BODY_DATUM lands in RM2. */
static Form *try_read_user_macro(Reader *r) {
    /* We always need to attempt parsing here because built-in named string
     * macros (currently `#rx"..."`) dispatch through this hook too — a
     * blanket "skip when registry empty" would miss them. */

    size_t   save_pos  = r->pos;
    uint32_t save_line = r->line;
    uint32_t save_col  = r->col;

    uint32_t call_line = r->line;
    uint32_t call_col  = r->col;
    size_t   call_off  = r->pos;

    advance(r); /* consume '#' */

    if (!is_macro_id_start(peek(r))) {
        r->pos = save_pos; r->line = save_line; r->col = save_col;
        return NULL;
    }
    size_t id_start = r->pos;
    while (is_macro_id_cont(peek(r))) advance(r);
    StrSlice name = strslice(r->src + id_start,
                             (uint32_t)(r->pos - id_start));

    int delim_char = peek(r);
    int try_delim  = 0;
    if (delim_char == '(' || delim_char == '[' || delim_char == '{'
        || delim_char == '"') {
        try_delim = delim_char;
    }

    /* Built-in `#dedent"""..."""` / `#dedent#"""..."""#` (see
     * read_dedent_literal).  `dedent` is reserved so no user macro shadows it. */
    if ((delim_char == '"' || delim_char == '#') && name.len == 6
        && memcmp(name.p, "dedent", 6) == 0) {
        return read_dedent_literal(r, call_line, call_col, call_off);
    }

    /* Built-in named string macro: #rx"..." → (re/compile "..."). The name
     * `rx` is reserved (see reader_macros.c::kReserved) so no user macro
     * can shadow it. */
    if (try_delim == '"' && name.len == 2
        && name.p[0] == 'r' && name.p[1] == 'x') {
        bool save_neo = r->neoteric_enabled;
        r->neoteric_enabled = false;
        Form *str = read_string(r);
        r->neoteric_enabled = save_neo;
        if (!str || r->error) return NULL;
        Span full_site =
            span_from_to(r, call_line, call_col, call_off, r->pos);
        const Symbol *re_compile =
            symtab_intern(r->st, strslice("re/compile", 10));
        Form *head = form_sym(r->arena, full_site, re_compile);
        Form **items = (Form **)arena_alloc(r->arena, sizeof(Form *) * 2);
        items[0] = head;
        items[1] = str;
        return form_list(r->arena, full_site, items, 2);
    }

    /* From here on we need a user-registry hit. */
    const ReaderMacroEntry *e = NULL;
    if (r->user_macros && r->user_macros->len > 0) {
        if (try_delim != 0) {
            e = reader_macros_lookup(r->user_macros, name, try_delim);
        }
        if (!e) {
            e = reader_macros_lookup(r->user_macros, name, 0);
        }
    }
    if (!e) {
        /* No exact (name, delim) match -- but this pair may be a RESERVED
         * built-in, which always wins.  Rewind so the caller's own dispatch
         * handles it, before either targeted diagnostic below can fire.
         *
         * `#s` is why this exists.  `#s(...)` is the set literal and `#s"..."`
         * is the owned-String literal: two different (name, delim) pairs that
         * share a name.  Once `#s"` is registered, the `lookup_any` branch
         * below sees a macro named "s", concludes the user meant it, and
         * reports `#s(1 2 3)` as "reader string macro '#s' expects string
         * body".  That was already true inside a `#lang turmeric stringed`
         * file -- a latent hole nobody hit, because almost nobody turned the
         * layer on.  Making `#s"` unconditional turns it into every set
         * literal in the language, so the rewind is part of that change.
         *
         * Checking reserved-ness rather than special-casing `s` is what keeps
         * this from recurring: a reserved pair cannot be registered at all
         * (reader_macros_register refuses it), so a registry hit on the NAME
         * can never be the right answer for it. */
        if (try_delim != 0 && reader_macros_is_reserved(name, try_delim)) {
            r->pos = save_pos; r->line = save_line; r->col = save_col;
            return NULL;
        }
        /* If a macro with this name exists under a different delimiter, the
         * user almost certainly meant it — emit a targeted diagnostic instead
         * of silently rewinding into the generic "unexpected character"
         * path. */
        const ReaderMacroEntry *any = (r->user_macros && r->user_macros->len > 0)
            ? reader_macros_lookup_any(r->user_macros, name) : NULL;
        if (any) {
            if (any->mode == RM_BODY_NONE) {
                diag_emit(DIAG_ERROR, span_point(r),
                          "reader macro '#%.*s' takes no body, "
                          "but a '%c' delimiter follows",
                          (int)name.len, name.p, (char)delim_char);
            } else if (any->mode == RM_BODY_STRING) {
                diag_emit(DIAG_ERROR, span_point(r),
                          "reader string macro '#%.*s' expects string body",
                          (int)name.len, name.p);
            } else {
                diag_emit(DIAG_ERROR, span_point(r),
                          "reader macro '#%.*s' expects '%c' body, got '%c'",
                          (int)name.len, name.p,
                          (char)any->delim,
                          delim_char == -1 ? '?' : (char)delim_char);
            }
            r->error = true;
            return NULL;
        }
        /* A `#name"..."` invocation that didn't match anything is almost
         * certainly an intended string macro the user forgot to register —
         * emit a targeted diagnostic rather than silently rewinding (which
         * would surface as the generic "unexpected character" later). */
        if (try_delim == '"') {
            diag_emit(DIAG_ERROR, span_point(r),
                      "unknown reader string macro '#%.*s'",
                      (int)name.len, name.p);
            r->error = true;
            return NULL;
        }
        /* Truly unknown — rewind so the caller's built-in checks (and the
         * final "unexpected character" fallback) still apply. */
        r->pos = save_pos; r->line = save_line; r->col = save_col;
        return NULL;
    }

    Span name_site = span_from_to(r, call_line, call_col, call_off, r->pos);

    if (e->mode == RM_BODY_NONE) {
        /* Bare form: any following delimiter is not part of this macro. */
        return expand_raw_template(r, e->template, "", 0, name_site);
    }

    if (try_delim == 0 || try_delim != e->delim) {
        if (e->mode == RM_BODY_STRING) {
            diag_emit(DIAG_ERROR, span_point(r),
                      "reader string macro '#%.*s' expects string body",
                      (int)name.len, name.p);
        } else {
            diag_emit(DIAG_ERROR, span_point(r),
                      "reader macro '#%.*s' expects '%c' body",
                      (int)name.len, name.p, (char)e->delim);
        }
        r->error = true;
        return NULL;
    }

    if (e->mode == RM_BODY_STRING) {
        bool save_neo = r->neoteric_enabled;
        r->neoteric_enabled = false;
        Form *str = read_string(r);
        r->neoteric_enabled = save_neo;
        if (!str || r->error) return NULL;
        Span full_site =
            span_from_to(r, call_line, call_col, call_off, r->pos);
        return expand_raw_template(r, e->template,
                                   str->as.s.p, str->as.s.len, full_site);
    }

    int close = matching_close_for(e->delim);

    if (e->mode == RM_BODY_RAW) {
        uint32_t body_len = 0;
        char *body = read_raw_body(r, e->delim, close, &body_len);
        if (r->error) return NULL;
        Span full_site =
            span_from_to(r, call_line, call_col, call_off, r->pos);
        return expand_raw_template(r, e->template, body, body_len, full_site);
    }

    /* RM_BODY_DATUM: recursively read forms until the matched close. */
    const char *unterm_msg;
    switch (e->delim) {
        case '(': unterm_msg =
            "unterminated reader macro body (missing ')')"; break;
        case '[': unterm_msg =
            "unterminated reader macro body (missing ']')"; break;
        case '{': unterm_msg =
            "unterminated reader macro body (missing '}')"; break;
        default:  unterm_msg = "unterminated reader macro body";
    }
    /* read_seq picks the underlying container type from `tag`; for the
     * purposes of splicing into the template we only need access to its
     * items + len, so F_LIST for paren/brace and F_VEC for bracket is a
     * reasonable choice (preserves the natural sequence shape). */
    FormTag body_tag = (e->delim == '[') ? F_VEC : F_LIST;
    Form *seq = read_seq(r, (char)e->delim, (char)close, body_tag, unterm_msg);
    if (!seq || r->error) return NULL;
    Span full_site = span_from_to(r, call_line, call_col, call_off, r->pos);
    return expand_datum_template(r, e->template,
                                 seq->as.list.items, seq->as.list.len,
                                 full_site);
}

/* Legible character literals: `#\a`, `#\space`, `#\u41`.  The reader emits a
 * plain :int literal whose value is the character's byte code -- no new type,
 * no ABI change, no runtime work.  See
 * docs/archive/legible-char-literals-plan.md. */
static bool char_lit_is_delim(int c) {
    /* A reader delimiter terminates a char literal: EOF, whitespace (including
     * the reader's comma-as-whitespace), and the closing/comment bytes. */
    return c == -1 || c == ' ' || c == '\t' || c == '\r' || c == '\n' ||
           c == ',' || c == ')' || c == ']' || c == '}' || c == ';';
}

/* Named-char table (CH1).  Hard-coded, reader-side; no runtime dependency. */
static bool char_lit_named(const char *name, int64_t *out) {
    if      (strcmp(name, "space")     == 0) { *out = 32;  return true; }
    else if (strcmp(name, "newline")   == 0) { *out = 10;  return true; }
    else if (strcmp(name, "tab")       == 0) { *out = 9;   return true; }
    else if (strcmp(name, "return")    == 0) { *out = 13;  return true; }
    else if (strcmp(name, "null")      == 0) { *out = 0;   return true; }
    else if (strcmp(name, "backspace") == 0) { *out = 8;   return true; }
    else if (strcmp(name, "delete")    == 0) { *out = 127; return true; }
    else if (strcmp(name, "escape")    == 0) { *out = 27;  return true; }
    else if (strcmp(name, "alarm")     == 0) { *out = 7;   return true; }  /* R7RS 6.6 */
    return false;
}

static Form *read_char_literal(Reader *r) {
    uint32_t start_line = r->line;
    uint32_t start_col = r->col;
    size_t start_off = r->pos;
    advance(r); /* consume '#' */
    advance(r); /* consume '\\' */

    int first = peek(r);
    if (first == -1) {
        Span s = span_from_to(r, start_line, start_col, start_off, r->pos);
        diag_emit(DIAG_ERROR, s,
                  "unterminated character literal: '#\\' at end of input");
        r->error = true;
        return NULL;
    }

    int64_t value;

    /* r7rs-lang-plan R1: R7RS 6.6 spells the hex form `#\x<hex>` (any scalar
     * value), and `#\<char>` may be a non-ASCII character, which is more
     * than one byte here; both Scheme-only, since under the Turmeric reader
     * `#\x41` is the ambiguous-literal error below and a lone `#\x` is the
     * letter x in both. */
    if (r->scheme_enabled && first == 'x' && hex_digit(peek2(r)) >= 0) {
        advance(r); /* consume 'x' */
        uint32_t v = 0; int nd = 0, d;
        while ((d = hex_digit(peek(r))) >= 0) {
            if (nd < 8) v = v * 16 + (uint32_t)d;
            advance(r); nd++;
        }
        if (nd > 8 || v > 0x10FFFF || !char_lit_is_delim(peek(r))) {
            Span s = span_from_to(r, start_line, start_col, start_off, r->pos);
            diag_emit(DIAG_ERROR, s,
                      "malformed '#\\x' character escape: expected hex digits "
                      "naming a Unicode scalar value (at most 10FFFF) followed "
                      "by a delimiter");
            r->error = true;
            return NULL;
        }
        value = (int64_t)v;
    }
    else if (r->scheme_enabled && first >= 0x80) {
        /* One UTF-8 encoded character: decode the lead byte's length. */
        int len = (first >= 0xF0) ? 4 : (first >= 0xE0) ? 3 : (first >= 0xC0) ? 2 : 0;
        uint32_t v = 0;
        if (len == 0) {
            Span s = span_from_to(r, start_line, start_col, start_off, r->pos);
            diag_emit(DIAG_ERROR, s, "malformed UTF-8 in character literal");
            r->error = true;
            return NULL;
        }
        v = (uint32_t)first & (0xFFu >> (len + 1));
        advance(r);
        for (int i = 1; i < len; i++) {
            int b = peek(r);
            if (b == -1 || (b & 0xC0) != 0x80) {
                Span s = span_from_to(r, start_line, start_col, start_off, r->pos);
                diag_emit(DIAG_ERROR, s, "malformed UTF-8 in character literal");
                r->error = true;
                return NULL;
            }
            v = (v << 6) | ((uint32_t)b & 0x3F);
            advance(r);
        }
        if (!char_lit_is_delim(peek(r))) {
            Span s = span_from_to(r, start_line, start_col, start_off, r->pos);
            diag_emit(DIAG_ERROR, s,
                      "ambiguous character literal: '#\\' names exactly one "
                      "character and must be followed by a delimiter");
            r->error = true;
            return NULL;
        }
        value = (int64_t)v;
    }
    /* Escape form (CH2): #\u<hex>, 1-4 hex digits, value 0..0xFF.  Checked
     * before the named form so `#\uf` reads as codepoint 0x0F rather than an
     * unknown two-letter name. */
    else if (first == 'u' && hex_digit(peek2(r)) >= 0) {
        advance(r); /* consume 'u' */
        int64_t v = 0;
        int ndigits = 0;
        int d;
        while (ndigits < 4 && (d = hex_digit(peek(r))) >= 0) {
            v = v * 16 + d;
            advance(r);
            ndigits++;
        }
        if (!char_lit_is_delim(peek(r))) {
            Span s = span_from_to(r, start_line, start_col, start_off, r->pos);
            diag_emit(DIAG_ERROR, s,
                      "malformed '#\\u' codepoint escape: expected 1-4 hex "
                      "digits followed by a delimiter");
            r->error = true;
            return NULL;
        }
        if (v > 0xFF) {
            Span s = span_from_to(r, start_line, start_col, start_off, r->pos);
            diag_emit(DIAG_ERROR, s,
                      "character codepoint escape '#\\u%llx' out of range "
                      "(0..0xFF)", (unsigned long long)v);
            r->error = true;
            return NULL;
        }
        value = v;
    }
    /* Named form (CH1): two consecutive ASCII letters enter name mode. */
    else if (isalpha(first) && isalpha(peek2(r))) {
        char name[16];
        size_t n = 0;
        while (isalpha(peek(r))) {
            if (n < sizeof(name) - 1) name[n] = (char)advance(r);
            else advance(r);
            n++;
        }
        if (n >= sizeof(name) || !char_lit_is_delim(peek(r))) {
            Span s = span_from_to(r, start_line, start_col, start_off, r->pos);
            diag_emit(DIAG_ERROR, s,
                      "malformed character-name literal '#\\...': a name must be "
                      "ASCII letters terminated by a delimiter");
            r->error = true;
            return NULL;
        }
        name[n] = '\0';
        if (!char_lit_named(name, &value)) {
            Span s = span_from_to(r, start_line, start_col, start_off, r->pos);
            diag_emit(DIAG_ERROR, s,
                      "unknown character name '#\\%s' (known: space, newline, "
                      "tab, return, null, backspace, delete, escape)", name);
            r->error = true;
            return NULL;
        }
    }
    /* Base form (CH0): exactly one character, which must be followed by a
     * delimiter.  A trailing non-delimiter (e.g. `#\a2`) is ambiguous and
     * rejected -- write `#\a` and `#\2` separately, or use an escape. */
    else {
        value = (int64_t)advance(r);
        if (!char_lit_is_delim(peek(r))) {
            Span s = span_from_to(r, start_line, start_col, start_off, r->pos);
            diag_emit(DIAG_ERROR, s,
                      "ambiguous character literal: '#\\' names exactly one "
                      "character and must be followed by a delimiter -- write "
                      "'#\\<char>' separately or use a '#\\u' escape");
            r->error = true;
            return NULL;
        }
    }

    Span span = span_from_to(r, start_line, start_col, start_off, r->pos);
    /* r7rs-lang-plan R3 / D3: under the Scheme reader a character is its own
     * type, distinct from an integer (`char?` must be disjoint from
     * `integer?`), and there is no character Form -- so the literal reads as
     * the call form `(r7rs-char__ <scalar value>)`, which the prelude's
     * constructor turns into an `R7rsChar`.  scheme_lower.c's datum walker
     * recognises the shape under `quote`. */
    if (r->scheme_enabled) {
        Form **items = (Form **)arena_alloc(r->arena, 2 * sizeof(Form *));
        items[0] = form_sym(r->arena, span, symtab_intern(r->st, strslice("r7rs-char__", 11)));
        items[1] = form_int(r->arena, span, value);
        return form_list(r->arena, span, items, 2);
    }
    /* parsec-guide-plan P2: still an F_INT, so every form-level consumer
     * (quote, patterns, macros) sees the code it always saw, but marked so
     * the elaborator types it as Char and the formatter writes `#\A`. */
    Form *lit = form_int(r->arena, span, value);
    lit->lit_suffix = LIT_SUF_CHAR;
    return lit;
}

/* ---------------------------------------------------------------------------
 * r7rs-lang-plan R1: the Scheme lexical layer (READER_R7RS)
 *
 * Everything below is reachable only with r->scheme_enabled.  It is a
 * VARIANT of the reader above: the s-expression machinery, spans, symbol
 * table and Form model are shared, and each helper handles one lexeme R7RS
 * spells differently from Turmeric.  No Scheme SEMANTICS live here -- `#t`
 * is an F_BOOL, `#\a` an F_INT code point, `#(1 2)` an F_VEC, `#u8(...)` a
 * `(bytevector ...)` call form, a dotted pair a list carrying its `.` --
 * which is exactly the plan's R1 contract: a `#lang r7rs` file reads, and
 * then elaborates as the same forms would under `#lang saffron`.
 * ------------------------------------------------------------------------- */

/* R7RS 7.1.1 <delimiter>: whitespace, `|`, `(`, `)`, `"`, `;` -- plus the
 * brackets and the quote characters, which end a token here as well. */
static bool scheme_is_delim(int c) {
    return c == -1 || c == ' ' || c == '\t' || c == '\r' || c == '\n' ||
           c == '(' || c == ')' || c == '[' || c == ']' || c == '{' || c == '}' ||
           c == '"' || c == ';' || c == '|' || c == '\'' || c == '`' || c == ',';
}

#include "r7rs_bignum.inc"     /* T1: a literal outside int64 */
#include "r7rs_numsyntax.inc"

/* r7rs-lang-plan T0: a number token, read whole.  The token runs to the next
 * delimiter and goes through the one R7RS number parser (r7rs_numsyntax.inc,
 * shared with `string->number` and `read`), so `1/2` and `3+4i` are ONE
 * token -- they used to split into `1` and the symbol `/2`, and `3`, `+4`
 * and `i`.  A value the tower holds reads as that value (`10/2` is 5); one
 * it cannot hold yet is an error naming the task that brings it.  Returns
 * NULL without consuming or setting r->error when the token is not number
 * syntax, so the caller's own path (a symbol, or its diagnostic) runs. */
static Form *try_read_scheme_number(Reader *r) {
    size_t n = 0;
    while (!scheme_is_delim(peek_at(r, n))) n++;
    if (n == 0) return NULL;
    r7rs_ns_result res;
    r7rs_ns_parse(r->src + r->pos, n, 10, &res);
    if (res.kind == R7NS_NONE) return NULL;
    uint32_t start_line = r->line;
    uint32_t start_col = r->col;
    size_t start_off = r->pos;
    for (size_t k = 0; k < n; k++) advance(r);
    Span span = span_from_to(r, start_line, start_col, start_off, r->pos);
    if (res.kind == R7NS_REFUSED) {
        diag_emit(DIAG_ERROR, span, "`%.*s`: %s", (int)n, r->src + start_off, res.why);
        r->error = true;
        return NULL;
    }
    if (res.kind == R7NS_BIG || res.kind == R7NS_RATIO || res.kind == R7NS_COMPLEX) {
        /* T1/T2: an exact integer outside int64 reads as the call form
         * `(r7rs-big__ "<decimal digits>")` and an exact non-integer as
         * `(r7rs-ratio__ "<n>/<d>")`, as a char reads as `(r7rs-char__ n)`;
         * the prelude builds the value, and the datum walker keeps the shape
         * under `quote`.  T6: a non-real complex number is
         * `(r7rs-complex__ "<re> <im>")`, each part a real's spelling. */
        const char *ctor = res.kind == R7NS_BIG ? "r7rs-big__"
                         : res.kind == R7NS_RATIO ? "r7rs-ratio__" : "r7rs-complex__";
        Form **items = (Form **)arena_alloc(r->arena, 2 * sizeof(Form *));
        items[0] = form_sym(r->arena, span, symtab_intern(r->st, strslice(ctor, (uint32_t)strlen(ctor))));
        size_t bl = strlen(res.big);
        char *digits = (char *)arena_alloc_aligned(r->arena, bl + 1, 1);
        memcpy(digits, res.big, bl + 1);
        free(res.big);
        items[1] = form_str(r->arena, span, digits, (uint32_t)bl);
        return form_list(r->arena, span, items, 2);
    }
    return res.kind == R7NS_INT ? form_int(r->arena, span, res.i)
                                : form_float(r->arena, span, res.f);
}

/* Identifier characters R7RS allows that Turmeric's is_sym_start/is_sym_cont
 * do not: `~` and `%` are <special initial>s; `@` is a <special subsequent>
 * (start position is handled by read_form, where `@` stays deref sugar). */
static bool scheme_sym_extra(int c) {
    return c == '~' || c == '%' || c == '@';
}

/* `|...|` -- a symbol whose spelling is taken verbatim, with the string
 * escapes (`\|`, `\\`, `\x<hex>;`, `\a`, `\b`, `\t`, `\n`, `\r`).  Never
 * case-folded (R7RS 2.1). */
static Form *read_piped_symbol(Reader *r) {
    uint32_t start_line = r->line;
    uint32_t start_col = r->col;
    size_t start_off = r->pos;
    advance(r); /* opening '|' */

    /* Upper bound on the decoded length: every escape decodes to at most as
     * many bytes as it occupies. */
    size_t scan = r->pos, cap = 0; bool ok = false;
    while (scan < r->len) {
        if (r->src[scan] == '|') { ok = true; break; }
        if (r->src[scan] == '\\') { if (scan + 1 >= r->len) break; scan += 2; cap += 2; continue; }
        scan++; cap++;
    }
    if (!ok) {
        Span s = span_from_to(r, start_line, start_col, start_off, start_off + 1);
        diag_emit(DIAG_ERROR, s, "unterminated |symbol| (missing '|')");
        r->error = true;
        return NULL;
    }
    char *buf = (char *)arena_alloc_aligned(r->arena, cap + 1, 1);
    size_t bi = 0;
    while (peek(r) != '|') {
        int c = advance(r);
        if (c != '\\') { buf[bi++] = (char)c; continue; }
        int e = advance(r);
        switch (e) {
            case '|':  buf[bi++] = '|';  break;
            case '\\': buf[bi++] = '\\'; break;
            /* R10: `\"` too, as in a string: other implementations accept
             * it, and chibi's suite writes `'|\"|`. */
            case '"':  buf[bi++] = '"';  break;
            case 'a':  buf[bi++] = 7;    break;
            case 'b':  buf[bi++] = 8;    break;
            case 't':  buf[bi++] = '\t'; break;
            case 'n':  buf[bi++] = '\n'; break;
            case 'r':  buf[bi++] = '\r'; break;
            case 'x': case 'X': {
                uint32_t cp = 0; int nd = 0, d;
                while ((d = hex_digit(peek(r))) >= 0) {
                    if (nd < 8) cp = cp * 16 + (uint32_t)d;
                    advance(r); nd++;
                }
                if (nd == 0 || nd > 8 || peek(r) != ';' || cp > 0x10FFFF) {
                    diag_emit(DIAG_ERROR, span_point(r),
                              "malformed '\\x<hex>;' escape in |symbol|");
                    r->error = true;
                    return NULL;
                }
                advance(r); /* ';' */
                bi += (size_t)utf8_encode(cp, buf + bi);
                break;
            }
            default:
                diag_emit(DIAG_ERROR, span_point(r),
                          "unknown escape '\\%c' in |symbol|", e);
                r->error = true;
                return NULL;
        }
    }
    advance(r); /* closing '|' */
    buf[bi] = '\0';
    Span span = span_from_to(r, start_line, start_col, start_off, r->pos);
    const Symbol *sym = symtab_intern(r->st, strslice(buf, (uint32_t)bi));
    return form_sym(r->arena, span, sym);
}

/* `#x`/`#o`/`#b`/`#d` radix and `#e`/`#i` exactness prefixes (R7RS 7.1.1
 * <number>), at most one of each, in either order.  A radix-10 body is the
 * ordinary read_number lexeme (so `#d1.5e3` and `#e#d10` work); another
 * radix is an integer in that base.  `#i` on an integer lexeme makes it a
 * float; `#e` on a float lexeme is honoured only when the value is integral
 * -- an exact non-integer needs the rationals R5 brings, and saying so beats
 * quietly reading `#e1.5` as 1.5. */
static Form *read_scheme_prefixed_number(Reader *r) {
    uint32_t start_line = r->line;
    uint32_t start_col = r->col;
    size_t start_off = r->pos;
    int radix = 10;   /* 0 == not yet given */
    int exact = 0;    /* 0 unspecified, 1 `#e`, -1 `#i` */
    bool radix_given = false, exact_given = false;

    while (peek(r) == '#') {
        int p = tolower(peek2(r));
        if ((p == 'x' || p == 'b' || p == 'o' || p == 'd') && !radix_given) {
            radix = (p == 'x') ? 16 : (p == 'b') ? 2 : (p == 'o') ? 8 : 10;
            radix_given = true;
        } else if ((p == 'e' || p == 'i') && !exact_given) {
            exact = (p == 'e') ? 1 : -1;
            exact_given = true;
        } else {
            Span s = span_from_to(r, start_line, start_col, start_off, r->pos + 2);
            diag_emit(DIAG_ERROR, s,
                      "malformed numeric prefix: at most one radix (#x #o #b #d) "
                      "and one exactness (#e #i) prefix");
            r->error = true;
            return NULL;
        }
        advance(r); advance(r);
    }

    int sign = 0;
    if (peek(r) == '-') sign = -1;
    else if (peek(r) == '+') sign = 1;

    Form *num;
    if (radix == 10) {
        num = read_number(r, sign);
        if (!num) return NULL;
    } else {
        if (sign != 0) advance(r);
        uint64_t mag = 0; bool ovf = false, any = false;
        for (;;) {
            int d = hex_digit(peek(r));
            if (d < 0 || d >= radix) break;
            mag_push(&mag, &ovf, (uint64_t)radix, (uint64_t)d);
            advance(r);
            any = true;
        }
        Span span = span_from_to(r, start_line, start_col, start_off, r->pos);
        if (!any || !scheme_is_delim(peek(r))) {
            diag_emit(DIAG_ERROR, span,
                      "expected base-%d digits after the radix prefix", radix);
            r->error = true;
            return NULL;
        }
        uint64_t bound = (sign < 0) ? (uint64_t)INT64_MAX + 1u : (uint64_t)INT64_MAX;
        if (ovf || mag > bound) {
            diag_emit(DIAG_ERROR, span,
                      "integer literal overflows int64 range "
                      "(-9223372036854775808..9223372036854775807)");
            r->error = true;
            return NULL;
        }
        int64_t ival = (sign < 0) ? (int64_t)(~mag + 1u) : (int64_t)mag;
        num = form_int(r->arena, span, ival);
    }

    if (exact == 1 && num->tag == F_FLOAT) {
        double f = num->as.f;
        if (isfinite(f) && f == floor(f) && fabs(f) < 9.2e18) {
            num->tag  = F_INT;
            num->as.i = (int64_t)f;
        } else {
            diag_emit(DIAG_ERROR, num->span,
                      "`#e` on a non-integral literal needs exact rationals, "
                      "which `#lang r7rs` does not have yet (r7rs-lang-plan "
                      "R5); write the exact integer or drop the prefix");
            r->error = true;
            return NULL;
        }
    } else if (exact == -1 && num->tag == F_INT) {
        num->tag  = F_FLOAT;
        num->as.f = (double)num->as.i;
        num->lit_suffix = LIT_SUF_NONE;
    }
    /* Span the whole lexeme, prefixes included. */
    num->span = span_from_to(r, start_line, start_col, start_off, r->pos);
    return num;
}

/* The Scheme-only `#` dispatches.  Returns NULL WITHOUT setting r->error
 * when the input is not one of them, so read_form falls through to the
 * Turmeric `#` literals it shares with every other dialect. */
static Form *try_read_scheme_hash(Reader *r) {
    uint32_t start_line = r->line;
    uint32_t start_col = r->col;
    size_t start_off = r->pos;
    int c2 = peek2(r);

    /* `#t` / `#f` / `#true` / `#false` (R7RS 6.3).  A delimiter must follow
     * the tag, which is what keeps `#fx{...}` (Turmeric's effect row) and any
     * other `#f<ident>` shape out of this branch. */
    if (c2 == 't' || c2 == 'f') {
        bool val = (c2 == 't');
        size_t n = 0;
        if (val) {
            if (r->pos + 5 <= r->len && memcmp(r->src + r->pos, "#true", 5) == 0 &&
                scheme_is_delim(peek_at(r, 5))) n = 5;
            else if (scheme_is_delim(peek_at(r, 2))) n = 2;
        } else {
            if (r->pos + 6 <= r->len && memcmp(r->src + r->pos, "#false", 6) == 0 &&
                scheme_is_delim(peek_at(r, 6))) n = 6;
            else if (scheme_is_delim(peek_at(r, 2))) n = 2;
        }
        if (n) {
            for (size_t i = 0; i < n; i++) advance(r);
            return form_bool(r->arena,
                             span_from_to(r, start_line, start_col, start_off, r->pos),
                             val);
        }
        return NULL;
    }

    /* `#(...)` -- a vector literal (R7RS 6.8), the same F_VEC `[...]` reads
     * as, so it lowers wherever `[...]` lowers. */
    if (c2 == '(') {
        advance(r); /* '#' */
        Form *v = read_seq(r, '(', ')', F_VEC, "unterminated vector (missing ')')");
        if (v) {
            v->span = span_from_to(r, start_line, start_col, start_off, r->pos);
            v->fx_prov = PROV_SCHEME_VECTOR;   /* R7: a datum, not a binding vector */
        }
        return v;
    }

    /* `#u8(...)` -- a bytevector literal (R7RS 6.9).  There is no bytevector
     * Form, so it reads as the call form `(bytevector e ...)`, which R3's
     * data stage gives a meaning; the elements are read as ordinary forms
     * and range-checked there, not here. */
    if (c2 == 'u' && peek3(r) == '8' && peek_at(r, 3) == '(') {
        advance(r); advance(r); advance(r); /* "#u8" */
        Form *l = read_seq(r, '(', ')', F_LIST, "unterminated bytevector (missing ')')");
        if (!l) return NULL;
        uint32_t n = l->as.list.len;
        Form **items = (Form **)malloc((n + 1) * sizeof(Form *));
        if (!items) { fprintf(stderr, "tur: oom\n"); abort(); }
        Span head = span_from_to(r, start_line, start_col, start_off, start_off + 3);
        items[0] = form_sym(r->arena, head, symtab_intern(r->st, strslice("bytevector", 10)));
        for (uint32_t i = 0; i < n; i++) items[i + 1] = l->as.list.items[i];
        Form *bv = form_list(r->arena,
                             span_from_to(r, start_line, start_col, start_off, r->pos),
                             items, n + 1);
        free(items);
        return bv;
    }

    /* Radix / exactness prefixes (R7RS 7.1.1). */
    if (c2 == 'x' || c2 == 'X' || c2 == 'b' || c2 == 'B' || c2 == 'o' || c2 == 'O' ||
        c2 == 'd' || c2 == 'D' || c2 == 'e' || c2 == 'E' || c2 == 'i' || c2 == 'I') {
        /* `#e`/`#i`/`#d`/`#b`/... only when what follows the prefix chain
         * can start a number; otherwise decline so `#d`-style user reader
         * macros and the Turmeric `#`-literals keep their meaning. */
        size_t k = 0;
        while (peek_at(r, k) == '#') {
            int p = tolower(peek_at(r, k + 1));
            if (p != 'x' && p != 'b' && p != 'o' && p != 'd' && p != 'e' && p != 'i') break;
            k += 2;
        }
        int n0 = peek_at(r, k);
        int n1 = peek_at(r, k + 1);
        bool numberish = (n0 >= '0' && n0 <= '9') ||
                         ((n0 == '+' || n0 == '-' || n0 == '.') &&
                          ((n1 >= '0' && n1 <= '9') || n1 == '.')) ||
                         /* T0: `#i+inf.0`, `#e+i` */
                         ((n0 == '+' || n0 == '-') &&
                          (n1 == 'i' || n1 == 'I' || n1 == 'n' || n1 == 'N')) ||
                         (n0 >= 'a' && n0 <= 'f') || (n0 >= 'A' && n0 <= 'F');
        if (!numberish) return NULL;
        Form *num = try_read_scheme_number(r);
        if (num || r->error) return num;
        /* Not number syntax: the prefix reader names what is wrong. */
        return read_scheme_prefixed_number(r);
    }

    return NULL;
}

static Form *read_form(Reader *r) {
    /* proper-tail-calls T1: consume the one-shot list-head flag before any
     * recursive read can see it. */
    bool head_pos = r->at_list_head;
    r->at_list_head = false;
    skip_ws_and_comments(r);
    if (r->error) return NULL;
    int c = peek(r);
    if (c == -1) return NULL;

    /* RM0: Try user-registered #-dispatch macros before built-ins. The
     * registry rejects any name that would shadow a built-in, so order
     * is correctness-neutral; user-first just keeps the common case fast.
     * In RM0 this always returns NULL (no registry / empty registry). */
    if (c == '#') {
        Form *m = try_read_user_macro(r);
        if (m || r->error) return m;
    }

    /* DC1/DC2: a datum comment `#;datum` is whitespace, and
     * skip_ws_and_comments above has already discarded it. */
    /* r7rs-lang-plan R1: the Scheme-only `#` dispatches -- `#t`/`#f`,
     * `#(...)`, `#u8(...)`, the numeric prefixes.  Before every Turmeric
     * `#` literal so that, e.g., `#f` is a boolean and not the start of
     * `#fx{...}`; the helper declines (NULL, no error) anything it does not
     * own, and each of its shapes needs a delimiter or an opener right after
     * the tag, which is what keeps `#fx{`, `#false{`-style collisions out. */
    if (c == '#' && r->scheme_enabled) {
        Form *m = try_read_scheme_hash(r);
        if (m || r->error) return m;
    }
    /* Legible character literals: `#\a`, `#\space`, `#\u41`.  The backslash
     * after `#` is unambiguous -- no other reader dispatch starts with `#\`.
     * Emits a plain :int literal (the byte code). */
    if (c == '#' && peek2(r) == '\\') {
        return read_char_literal(r);
    }
    /* r7rs-turmeric-syntax-leaks items 2 and 6: every `#` form left is
     * Turmeric's (`#map{`, `#set{`, `#rat{`, `#cx{`, `#?(`, `#fx{`, `#json`,
     * `#[`...).  A user reader macro was offered the `#` first, above. */
    if (c == '#' && scheme_user_source(r)) return scheme_refuse_hash(r);
    /* fx-row-syntax-rename-plan Phase 1: `#fx{...}` -- explicit effect row.
     * Must be checked BEFORE the generic `#` + `{` map dispatch, otherwise
     * the bare-map branch would never see `f` after `#`.  None of the
     * other reader literals (`#map{`, `#set{`, `#row{`, `#refine{`, `#r{`,
     * `#s(`, `#json...`) start with `#fx{`, so this prefix is unambiguous. */
    if (c == '#' && peek2(r) == 'f' && peek3(r) == 'x' && peek_at(r, 3) == '{') {
        return read_fx_row(r);
    }
    /* C2 / #reads: `#reads <sym>` read-frame annotation.  Checked here, before
     * try_read_data_literal (which claims `#<ident>{` shapes), and unambiguous:
     * `#reads` is followed by whitespace/a symbol, not `{`, and no other reader
     * literal spells `#reads`.  is_sym_cont(peek_at(6)) guards against a longer
     * identifier like `#readsx`. */
    if (c == '#' && peek2(r) == 'r' && peek3(r) == 'e' && peek_at(r, 3) == 'a' &&
        peek_at(r, 4) == 'd' && peek_at(r, 5) == 's' && !is_sym_cont(peek_at(r, 6))) {
        return read_reads_annot(r);
    }
    /* WF1 / #writes: `#writes <sym>` / `#writes [<sym> ...]` write-frame
     * annotation.  Same window and same reasoning as `#reads` above -- it must
     * precede try_read_data_literal, and no other reader literal spells
     * `#writes` (the `#w` prefix is otherwise unclaimed).  is_sym_cont at 7
     * guards a longer identifier like `#writesx`. */
    if (c == '#' && peek2(r) == 'w' && peek3(r) == 'r' && peek_at(r, 3) == 'i' &&
        peek_at(r, 4) == 't' && peek_at(r, 5) == 'e' && peek_at(r, 6) == 's' &&
        !is_sym_cont(peek_at(r, 7))) {
        return read_writes_annot(r);
    }
    if (c == '#' && peek2(r) == '{') {
        return read_map(r);
    }
    if (c == '#' && peek2(r) == 's' && peek3(r) == '(') {
        return read_set(r);
    }
    /* RR0: Range literal #r{...}.  Checked BEFORE try_read_data_literal: that
     * helper's unknown-tag detector (TUR-E0283) greedily claims any `#<ident>{`
     * shape not in its table, which includes `#r{`.  When data literals were an
     * opt-in -X flag this was harmless (the helper was unreachable by default),
     * but with the flag now always-on the range dispatch must win first. */
    if (c == '#' && peek2(r) == 'r' && peek3(r) == '{') {
        return read_range_literal(r);
    }
    /* DL0: data literals #map{...} / #set{...}.
     * Placed after #{ and #s( so effect rows and #s(...) sets win; the helper
     * returns NULL without consuming when the input is not a data literal. */
    if (c == '#') {
        Form *m = try_read_data_literal(r);
        if (m || r->error) return m;
    }
    /* JR0: #json(...) compile-time JSON reader macro.
     * Placed alongside the data-literal dispatch; the helper returns NULL
     * without consuming when the input is not a #json( form. */
    /* RD2: #json-str<T>(...) typed-decode reader family.  Checked before
     * #json so the more specific prefix wins.  Returns NULL without
     * consuming for non-#json-str input. */
    if (c == '#') {
        Form *m = try_read_json_str(r);
        if (m || r->error) return m;
    }
    if (c == '#') {
        Form *m = try_read_json(r);
        if (m || r->error) return m;
    }
    /* Phase R5: Attribute syntax #[...] */
    if (c == '#' && peek2(r) == '[') {
        return read_attribute(r);
    }
    /* INT-1: Reader conditional #?(:tur expr :turi expr) */
    if (c == '#' && peek2(r) == '?') {
        return read_reader_cond(r);
    }

    /* CT0 / Phase S1: Curly-brace handling.
     * Bare '{' is always SRFI-105 curly-infix in every dialect.  Contract
     * types use the explicit `#refine{var : T | pred}` data-literal form
     * (read_refine_literal), keeping the dispatch context-free. */
    if (c == '{') {
        return read_curly_infix(r);
    }
    
    if (c == '(') return read_seq(r, '(', ')', F_LIST, "unterminated list (missing ')')");
    if (c == '[') {
        /* r7rs-turmeric-syntax-leaks item 1: brackets are parentheses in a
         * user Scheme source. */
        if (scheme_user_source(r))
            return read_seq(r, '[', ']', F_LIST, "unterminated list (missing ']')");
        Form *v = read_seq(r, '[', ']', F_VEC, "unterminated vector (missing ']')");
        /* TCE: a fused `:T` element-type suffix (`[]:int`) pins the vec's
         * element type.  Binding vectors are unaffected because their ']' is
         * never immediately followed by ':'. */
        return maybe_container_type_suffix(r, v, "Vec", 1);
    }
    if (c == ')' || c == ']' || c == '}') {
        Span s = span_point(r);
        diag_emit(DIAG_ERROR, s, "unexpected '%c'", (char)c);
        r->error = true;
        advance(r);
        return NULL;
    }
    if (c == '"') return read_string(r);
    if (c == ':') {
        /* r7rs-leading-colon-identifiers: `:x`, `:::`, `::x` are R7RS
         * identifiers in a Scheme source.  A `:` or `::` standing alone
         * reads as before (Turmeric's annotation / ascription). */
        int c2 = peek2(r);
        /* A lone `:` is an identifier too (R7RS 7.1.1: `:` is an <initial>),
         * not Turmeric's annotation marker: SRFI 42 names its dispatching
         * generator `:`, as in `(list-ec (: i 3) i)` (r7rs-srfi-plan S7). */
        if (scheme_user_source(r) && (c2 == ' ' || c2 == '\t' || c2 == '\n' || c2 == '\r' ||
                                      c2 == '(' || c2 == ')' || c2 == '[' || c2 == ']' || c2 == -1)) {
            uint32_t start_line = r->line, start_col = r->col;
            size_t start_off = r->pos;
            advance(r);
            Span span = span_from_to(r, start_line, start_col, start_off, r->pos);
            return form_sym(r->arena, span, symtab_intern(r->st, strslice(":", 1)));
        }
        if (scheme_user_source(r) && c2 != ' ' && c2 != '\t' && c2 != '\n' &&
            c2 != '\r' && c2 != '(' && c2 != '[' && c2 != ')' && c2 != ']' && c2 != -1 &&
            !(c2 == ':' && !is_sym_cont(peek3(r)) && peek3(r) != ':'))
            return read_symbol_or_minus_at(r, head_pos);
        return read_keyword(r);
    }
    if (c == '`') {
        /* r7rs-turmeric-syntax-leaks item 3: an inline C fence in user Scheme
         * source.  (Three quasiquotes before a `c` would be legal R7RS; no
         * program writes them, and the fence is what this is.) */
        if (peek2(r) == '`' && peek3(r) == '`' && peek_at(r, 3) == 'c' &&
            (peek_at(r, 4) == '\n' || peek_at(r, 4) == '\r' || peek_at(r, 4) == ' ' ||
             peek_at(r, 4) == '\t') && scheme_user_source(r))
            return scheme_refuse_lexeme(r, "inline C", SCHEME_SEAM_HELP);
        /* Phase 6: Check for triple backtick (C block) vs single backtick (quasiquote) */
        if (peek2(r) == '`' && peek3(r) == '`') {
            return read_cblock(r); /* C code block ``` */
        } else {
            /* Single backtick - quasiquote */
            return read_quasiquote(r);
        }
    }
    /* r7rs-lang-plan T0: a Scheme number is read as one whole token. */
    if (r->scheme_enabled &&
        ((c >= '0' && c <= '9') || c == '+' || c == '-' ||
         (c == '.' && peek2(r) >= '0' && peek2(r) <= '9'))) {
        Form *num = try_read_scheme_number(r);
        if (num || r->error) return num;
    }
    if (c >= '0' && c <= '9') return read_number(r, 0);
    /* r7rs-lang-plan R1: Scheme lexemes that a Turmeric byte already owns.
     * `.5` is a number (Turmeric: the symbol `.5`); `,`/`,@` are unquote
     * (Turmeric: whitespace); `|sym|` is a delimited symbol (Turmeric: `|`
     * starts an operator such as `|>`); `~` and `&` are ordinary identifier
     * characters (Turmeric: unquote and borrow sugar). */
    if (r->scheme_enabled) {
        if (c == '.' && peek2(r) >= '0' && peek2(r) <= '9') return read_number(r, 0);
        if (c == ',') return read_unquote_with(r, ',');
        if (c == '|') return read_piped_symbol(r);
    }
    /* '@' as deref/effect-row prefix */
    /* r7rs-turmeric-syntax-leaks item 6: `@` cannot start an R7RS
     * identifier, and Turmeric's deref is not Scheme (`,@` is read above). */
    if (c == '@' && scheme_user_source(r))
        return scheme_refuse_lexeme(r, "`@` (Turmeric's deref)",
                                    "a Scheme program has no boxes of its own; " SCHEME_SEAM_HELP);
    if (c == '@') return read_at(r);
    /* Phase 6: ' as quote operator */
    if (c == '\'') return read_quote(r);
    /* Phase 6: ~ as unquote operator (only valid inside quasiquote) */
    if (c == '~' && !r->scheme_enabled) return read_unquote(r);
    /* Phase 12: & as borrow prefix sugar (&x → (& x), &mut x → (&mut x)) */
    if (c == '&' && !r->scheme_enabled) return read_borrow(r);
    if (is_sym_start(c) || (r->scheme_enabled && scheme_sym_extra(c)))
        return read_symbol_or_minus_at(r, head_pos);

    Span s = span_point(r);
    diag_emit(DIAG_ERROR, s, "unexpected character '%c' (0x%02x)", (char)c, c);
    r->error = true;
    advance(r);
    return NULL;
}

#include <stdio.h>

/* RM4: implementation backing both `#use-reader-macros "..."` and the
 * spice-manifest `:reader-macros [...]` preloader. Reads `abs_path` and
 * recursively reads its top-level forms into `registry`. Non-directive
 * forms are silently discarded. Returns 0 on success, -1 on failure
 * (diagnostic emitted at the global level). */
int reader_macros_load_file(Arena *arena, SymbolTable *st,
                            const char *abs_path,
                            struct ReaderMacroRegistry *registry) {
    FILE *fp = fopen(abs_path, "rb");
    if (!fp) {
        diag_emit(DIAG_ERROR, SPAN_UNKNOWN,
                  "reader-macros: cannot open '%s'", abs_path);
        return -1;
    }
    fseek(fp, 0, SEEK_END);
    long sz = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    if (sz < 0) {
        fclose(fp);
        diag_emit(DIAG_ERROR, SPAN_UNKNOWN,
                  "reader-macros: cannot stat '%s'", abs_path);
        return -1;
    }
    char *buf = (char *)arena_alloc(arena, (size_t)sz + 1);
    size_t got = fread(buf, 1, (size_t)sz, fp);
    buf[got] = '\0';
    fclose(fp);

    SourceFile *sf = (SourceFile *)arena_alloc(arena, sizeof(SourceFile));
    *sf = (SourceFile){0};  /* clear xform_map/orig_src so diag rendering is safe */
    sf->path        = arena_strdup(arena, abs_path, strlen(abs_path));
    sf->src         = buf;
    sf->len         = (uint32_t)got;
    /* Use a near-max file_id so we don't overwrite an existing slot: the
     * registry's last one (DIAG_MAX_FILES in diag.h). This is best-effort --
     * collisions across multiple preloads in the same compile only affect
     * diagnostic snippet rendering, not correctness. */
    sf->file_id     = DIAG_MAX_FILES - 1;
    sf->reader_type = READER_TURMERIC;
    diag_register_file(sf);

    uint32_t sub_nforms = 0;
    Form **sub_forms = read_all_with_registry(arena, st, sf,
                                              registry, &sub_nforms);
    (void)sub_forms;
    return diag_had_error() ? -1 : 0;
}

/* RM4: resolve a relative path against the directory containing
 * `base_path`. Absolute paths pass through unchanged. The result is
 * written into `out` (at most `out_sz` bytes including the NUL). */
static void rm_resolve_relative(const char *base_path, const char *rel,
                                char *out, size_t out_sz) {
    if (rel[0] == '/') {
        snprintf(out, out_sz, "%s", rel);
        return;
    }
    const char *slash = base_path ? strrchr(base_path, '/') : NULL;
    if (slash) {
        size_t dir_len = (size_t)(slash - base_path + 1);
        snprintf(out, out_sz, "%.*s%s", (int)dir_len, base_path, rel);
    } else {
        snprintf(out, out_sz, "%s", rel);
    }
}

/* RM4: attempt to consume a `#use-reader-macros "path"` directive at the
 * current reader position. Returns:
 *   - 1 if a directive was consumed (and processed; r->error may be set
 *     on failure to load / parse the named file)
 *   - 0 if the input doesn't look like the directive (reader untouched).
 *
 * Only meaningful at the top level — the caller (read_all_with_registry)
 * invokes this between top-level forms.
 *
 * The named file is read with its own sub-Reader against `reg`, so any
 * `(reader-macros/define ...)` directives in it register into the
 * caller's registry. Non-directive forms in the loaded file are silently
 * discarded — `#use-reader-macros` is a syntax-only mechanism. */
static int try_consume_use_directive(Reader *r,
                                     struct ReaderMacroRegistry *reg) {
    static const char kKeyword[] = "use-reader-macros";
    const size_t kKwLen = sizeof(kKeyword) - 1;

    if (peek(r) != '#') return 0;
    if (r->pos + 1 + kKwLen > r->len) return 0;
    if (memcmp(r->src + r->pos + 1, kKeyword, kKwLen) != 0) return 0;
    /* Make sure the keyword isn't a prefix of a longer identifier. */
    int after = (r->pos + 1 + kKwLen < r->len)
        ? (unsigned char)r->src[r->pos + 1 + kKwLen] : -1;
    if (after != -1 && is_macro_id_cont(after)) return 0;

    /* Commit: consume '#' + keyword bytes. */
    uint32_t kw_line = r->line;
    uint32_t kw_col  = r->col;
    size_t   kw_off  = r->pos;
    advance(r);
    for (size_t i = 0; i < kKwLen; ++i) advance(r);

    skip_ws_and_comments(r);
    if (peek(r) != '"') {
        diag_emit(DIAG_ERROR, span_point(r),
                  "#use-reader-macros: expected a string literal "
                  "(file path) after the directive");
        r->error = true;
        return 1;
    }

    Form *path_form = read_string(r);
    if (!path_form || r->error) {
        r->error = true;
        return 1;
    }

    const char *rel = path_form->as.s.p;
    char abs_path[4096];
    if (rel[0] != '/' && r->file && r->file->base_dir) {
        /* The reading file's path has no usable dirname (e.g. the synthetic
         * "<eval>" blob under --interpret); resolve the relative path against
         * the script's base_dir instead. */
        snprintf(abs_path, sizeof(abs_path), "%s/%s", r->file->base_dir, rel);
    } else {
        rm_resolve_relative(r->file->path, rel, abs_path, sizeof(abs_path));
    }

    /* Stdlib-path fallback (mirrors (load)'s "stdlib/<rest>" retry, see
     * elab_toplevel.c): a "stdlib/..." path that does not resolve against the
     * reading file's directory is retried against TUR_STDLIB_DIR (default
     * "stdlib" relative to cwd).  This lets a stdlib-shipped reader-macro file
     * be referenced by a stable path from anywhere -- e.g.
     * `#use-reader-macros "stdlib/string-reader.tur"` -- exactly like
     * `(load "stdlib/...")`, instead of only resolving next to the caller. */
    if (rel[0] != '/' && strncmp(rel, "stdlib/", 7) == 0) {
        FILE *probe = fopen(abs_path, "rb");
        if (probe) {
            fclose(probe);
        } else {
            const char *sdir = getenv("TUR_STDLIB_DIR");
            if (!sdir || !*sdir) sdir = "stdlib";
            char alt[4096];
            snprintf(alt, sizeof(alt), "%s/%s", sdir, rel + 7);
            FILE *ap = fopen(alt, "rb");
            if (ap) {
                fclose(ap);
                snprintf(abs_path, sizeof(abs_path), "%s", alt);
            }
        }
    }

    if (reader_macros_load_file(r->arena, r->st, abs_path, reg) != 0) {
        /* reader_macros_load_file emitted its own diagnostic; flag the
         * outer reader so the calling read_all bails out. The kw_*
         * captures stay around in case we want to re-anchor later. */
        (void)kw_line; (void)kw_col; (void)kw_off;
        r->error = true;
    }
    return 1;
}

/* ===========================================================================
 * Sweet-expression (SRFI-110 t-expression) preprocessor.
 *
 * Indent-sensitive sweet-exp source is transformed into plain s-expression
 * text by inserting implicit `(` and `)` around each indent-determined
 * grouping; the result is then fed through the regular reader with
 * curly-infix and neoteric enabled, so inline forms like `f(x)` and
 * `{a + b}` keep working unchanged.
 *
 * Algorithm:
 *   1. Walk the source and split it into logical lines.  A logical line
 *      may span several physical lines when:
 *        - bracket groups `(...)` `[...]` `{...}` cross newlines;
 *        - inside a string literal `"..."`;
 *        - inside a block comment `#| ... |#`.
 *
 *   2. For each logical line compute its indent column (leading
 *      whitespace expanded with tab stops every 8 columns) and content
 *      range.  Blank / comment-only lines are flagged and skipped during
 *      structural analysis.
 *
 *   3. Walk the line list recursively.  Each "node" is one non-blank
 *      logical line plus every subsequent line at strictly deeper
 *      indent.  An "element" is either a head token (counted from the
 *      line content honoring brackets, strings, comments and `$`) or
 *      one immediate child sub-node.  When a node has more than one
 *      element, a `(` is recorded for the head line and a matching `)`
 *      for the last physical line of the node's last descendant.
 *
 *   4. Emit transformed text by walking the line list and inserting the
 *      recorded `(` / `)` at the right positions.  Newlines are
 *      preserved so diagnostic line numbers stay accurate.  Column
 *      positions can shift by the number of inserted parens; this is
 *      best-effort and acceptable.
 *
 *   5. `$` rest-of-line: a `$` token at top level (not inside
 *      brackets/string/comment) wraps the rest of the line in `(` `)`,
 *      i.e. `f $ g x` becomes `f (g x)`.  Counts as a single element
 *      for wrap decisions.  The wrap is suppressed when the rest is
 *      already one complete delimited expression -- `f $ g(x)` emits
 *      `f g(x)`, not `f ((g x))`.  See
 *      sweet_dollar_rest_is_delimited().
 *
 * Unsupported (yet): `\\` group operator, explicit `\` line
 * continuation.  Block-comment lines are treated as regular
 * non-blank content; using them for indent grouping is undefined.
 * =========================================================================*/

/* Map builder shared by the emitter to record xform→original byte runs.
 * Runs grow from the arena (freed wholesale at arena_free) rather than the
 * heap, so the map -- which lives for the file's diagnostic lifetime and has
 * no dedicated teardown -- does not leak. */
static void sweet_map_add_run(Arena *arena, SweetMap *m, size_t xform_off,
                               size_t orig_off, size_t n) {
    if (n == 0 || !m) return;
    if (m->n_runs > 0) {
        SweetMapRun *last = &m->runs[m->n_runs - 1];
        if (last->xform_offset + last->length == xform_off &&
            last->orig_offset  + last->length == orig_off) {
            last->length += (uint32_t)n;
            return;
        }
    }
    if (m->n_runs == m->cap_runs) {
        size_t new_cap = m->cap_runs ? m->cap_runs * 2 : 64;
        SweetMapRun *grown = (SweetMapRun *)arena_alloc(arena,
                                  new_cap * sizeof *grown);
        if (!grown) return;
        if (m->runs && m->n_runs > 0)
            memcpy(grown, m->runs, m->n_runs * sizeof *grown);
        m->runs = grown;
        m->cap_runs = new_cap;
    }
    m->runs[m->n_runs].xform_offset = (uint32_t)xform_off;
    m->runs[m->n_runs].orig_offset  = (uint32_t)orig_off;
    m->runs[m->n_runs].length       = (uint32_t)n;
    m->n_runs++;
}

size_t sweet_map_translate_offset(const SweetMap *m, size_t xform_off) {
    if (!m || m->n_runs == 0) return xform_off;
    /* Binary search for the rightmost run with xform_offset <= xform_off. */
    size_t lo = 0, hi = m->n_runs;
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        if (m->runs[mid].xform_offset <= xform_off) lo = mid + 1;
        else hi = mid;
    }
    if (lo == 0) return m->runs[0].orig_offset;
    const SweetMapRun *r = &m->runs[lo - 1];
    if (xform_off < (size_t)r->xform_offset + r->length) {
        return (size_t)r->orig_offset + (xform_off - r->xform_offset);
    }
    return (size_t)r->orig_offset + r->length;
}

/* Emit helpers used by the preprocessor: emit_copy_ records a run in
 * the map; emit_insert_ does not (the bytes are reader-inserted). */
typedef struct SweetEmit {
    Buf      *out;
    SweetMap *map;
    Arena    *arena;
    bool      scheme;   /* r7rs/sweet: Scheme's lexemes (sweet_lexeme_end) */
} SweetEmit;

static void emit_copy_(SweetEmit *e, const char *src, size_t orig_off, size_t n) {
    if (n == 0) return;
    sweet_map_add_run(e->arena, e->map, e->out->len, orig_off, n);
    buf_write(e->out, src + orig_off, n);
}

static void emit_copy_char_(SweetEmit *e, const char *src, size_t orig_off) {
    sweet_map_add_run(e->arena, e->map, e->out->len, orig_off, 1);
    buf_putc(e->out, src[orig_off]);
}

static void emit_insert_char_(SweetEmit *e, char c) {
    buf_putc(e->out, c);
}

typedef struct SweetLine {
    size_t phys_start;     /* offset of first physical line start (col 0) */
    size_t content_start;  /* offset of first non-indent char */
    size_t content_end;    /* offset where logical line ends (at \n or EOF) */
    int    indent;         /* indent column (tab-expanded) */
    bool   blank;          /* blank or comment-only line */
    bool   is_group;       /* leading `\\` marker — line is transparent to parent */
    int    open_count;     /* `(` to emit before content */
    int    close_count;    /* `)` to emit after content */
    int    contributes_elems; /* elements this node contributes to its parent
                               * (1 for regular wrapped lines, >=0 for group
                               * lines where tokens + child contributions
                               * flatten into the parent's element list) */
} SweetLine;

static size_t sweet_indent_bytes(const char *s, size_t i, size_t end) {
    size_t start = i;
    while (i < end && (s[i] == ' ' || s[i] == '\t')) i++;
    return i - start;
}

static int sweet_indent_col(const char *s, size_t i, size_t end) {
    int col = 0;
    while (i < end) {
        char c = s[i];
        if (c == ' ') { col++; i++; }
        else if (c == '\t') { col += 8 - (col % 8); i++; }
        else break;
    }
    return col;
}

/* If src[i] is a `\` line-continuation marker (a `\` followed only by
 * optional spaces/tabs and then `\n`), return the position past the
 * newline and any leading whitespace of the next physical line.
 * Otherwise return i unchanged.  Caller is responsible for ensuring this
 * runs only at top level (bd == 0, !in_str, !in_bc). */
static size_t sweet_skip_line_cont(const char *src, size_t i, size_t end) {
    if (i >= end || src[i] != '\\') return i;
    size_t k = i + 1;
    while (k < end && (src[k] == ' ' || src[k] == '\t')) k++;
    if (k >= end || src[k] != '\n') return i;
    k++;
    while (k < end && (src[k] == ' ' || src[k] == '\t')) k++;
    return k;
}

/* Detect a leading GROUP marker.  Per SRFI-110, the marker is a pair of
 * backslashes (`\\` — two characters) immediately after the line's
 * indent, either alone on the line or followed by whitespace and more
 * content.  Two backslashes (instead of one) distinguish the GROUP
 * marker from the `\` line-continuation marker. */
static bool sweet_line_is_group(const char *s, size_t i, size_t end) {
    if (i + 1 >= end) return false;
    if (s[i] != '\\' || s[i + 1] != '\\') return false;
    if (i + 2 >= end) return true;
    char n = s[i + 2];
    return (n == ' ' || n == '\t' || n == '\n' || n == '\r');
}

/* Advance past the GROUP marker (`\\` and any trailing spaces/tabs).
 * The caller ensures the line is a group line.  Returns the position of
 * the first post-marker character (which may equal end). */
static size_t sweet_skip_group_marker(const char *s, size_t i, size_t end) {
    if (i + 1 < end && s[i] == '\\' && s[i + 1] == '\\') i += 2;
    while (i < end && (s[i] == ' ' || s[i] == '\t')) i++;
    return i;
}

/* Past a lexeme at s[i] that holds a byte the scanners below would take for
 * structure, or i itself when none starts there.  Called only outside
 * strings and comments.
 *
 * A character literal -- `#\(`, `#\;`, `#\"` -- in every dialect, since the
 * Turmeric reader reads `#\c` too.  Under `#lang r7rs/sweet`
 * (r7rs-sweet-base-dialect-missing) also Scheme's own: a `|delimited
 * symbol|`, which may hold spaces, brackets and semicolons, and the `#;`
 * datum-comment prefix, whose `;` does not start a line comment. */
static size_t sweet_lexeme_end(const char *s, size_t i, size_t end, bool scheme) {
    if (s[i] == '#' && i + 2 < end && s[i + 1] == '\\' && s[i + 2] != '\n')
        return i + 3;
    if (!scheme) return i;
    if (s[i] == '#' && i + 1 < end && s[i + 1] == ';') return i + 2;
    if (s[i] == '|') {
        size_t k = i + 1;
        while (k < end && s[k] != '|' && s[k] != '\n') {
            if (s[k] == '\\' && k + 1 < end) k++;
            k++;
        }
        return (k < end && s[k] == '|') ? k + 1 : i;
    }
    return i;
}

/* One step of a `$` rest-of-line scan, which stops at a `;` or a newline:
 * past a string (whose `;` is text), a lexeme (sweet_lexeme_end) or a `\`
 * line continuation, else one byte. */
static size_t sweet_rest_step(const char *s, size_t i, size_t end, bool scheme) {
    if (s[i] == '"') {
        i++;
        while (i < end && s[i] != '"') {
            if (s[i] == '\\' && i + 1 < end) i++;
            i++;
        }
        return i < end ? i + 1 : end;
    }
    size_t k = sweet_lexeme_end(s, i, end, scheme);
    if (k != i) return k;
    if (s[i] == '\\') {
        k = sweet_skip_line_cont(s, i, end);
        if (k != i) return k;
    }
    return i + 1;
}

static bool sweet_line_is_blank(const char *s, size_t i, size_t end) {
    size_t orig = i;
    while (i < end && (s[i] == ' ' || s[i] == '\t')) i++;
    if (i >= end) return true;
    char c = s[i];
    if (c == '\n' || c == '\r' || c == ';') return true;
    /* Shebang at file start is treated as a blank line; the preprocessor
     * emits it verbatim, and the regular reader skips it via the
     * `#!`-at-byte-0 check in read_all_with_registry. */
    if (orig == 0 && c == '#' && i + 1 < end && s[i + 1] == '!') return true;
    return false;
}

/* Count top-level whitespace-separated elements in [start, end).
 * Bracket groups, strings, and block comments are each one element.
 * Line comments end the count.  `$` followed by whitespace consumes
 * the rest of the line into a single element. */
/* True if a triple-backtick (```) inline-C fence opens/closes at offset i. */
static inline bool sweet_at_fence(const char *src, size_t i, size_t end) {
    return i + 2 < end && src[i] == '`' && src[i + 1] == '`' && src[i + 2] == '`';
}

static int sweet_count_elements(const char *src, size_t start, size_t end,
                                bool scheme) {
    int count = 0;
    bool in_tok = false;   /* mid-token at bd == 0 */
    int  bd = 0;
    bool in_str = false, in_bc = false, in_cb = false;
    size_t i = start;
    while (i < end) {
        char c = src[i];
        if (in_cb) {
            /* Inside a ```c ... ``` block: opaque, part of the current token. */
            if (sweet_at_fence(src, i, end)) { in_cb = false; i += 3; continue; }
            i++; continue;
        }
        if (!in_str && !in_bc && sweet_at_fence(src, i, end)) {
            if (bd == 0 && !in_tok) { count++; in_tok = true; }
            in_cb = true; i += 3; continue;
        }
        if (in_str) {
            if (c == '\\' && i + 1 < end) { i += 2; continue; }
            if (c == '"') {
                in_str = false;
                i++;
                if (bd == 0) in_tok = false;
                continue;
            }
            i++; continue;
        }
        if (in_bc) {
            if (c == '|' && i + 1 < end && src[i + 1] == '#') {
                in_bc = false; i += 2;
                if (bd == 0) in_tok = false;
                continue;
            }
            i++; continue;
        }
        if (bd > 0) {
            /* Inside an open bracket group: track structure only. */
            if (c == '"') { in_str = true; i++; continue; }
            if (c == '#' && i + 1 < end && src[i + 1] == '|') {
                in_bc = true; i += 2; continue;
            }
            {
                size_t k = sweet_lexeme_end(src, i, end, scheme);
                if (k != i) { i = k; continue; }
            }
            /* A line comment inside the group: its brackets are not
             * structure (the logical line runs on past it). */
            if (c == ';') {
                while (i < end && src[i] != '\n') i++;
                continue;
            }
            if (c == '(' || c == '[' || c == '{') { bd++; i++; continue; }
            if (c == ')' || c == ']' || c == '}') {
                bd--; i++;
                if (bd == 0) {
                    /* Neoteric chaining: f(x)(y) is a single element, so a
                     * bracket immediately following a closed group continues
                     * the same token rather than starting a new element. */
                    in_tok = (i < end && (src[i] == '(' ||
                                          src[i] == '[' || src[i] == '{'));
                }
                continue;
            }
            i++; continue;
        }
        /* bd == 0 */
        {
            size_t k = sweet_lexeme_end(src, i, end, scheme);
            if (k != i) {
                if (!in_tok) { count++; in_tok = true; }
                i = k; continue;
            }
        }
        if (c == ';') break;
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
            in_tok = false; i++; continue;
        }
        if (c == '\\') {
            size_t k = sweet_skip_line_cont(src, i, end);
            if (k != i) { in_tok = false; i = k; continue; }
        }
        if (c == '"') {
            if (!in_tok) { count++; in_tok = true; }
            in_str = true; i++; continue;
        }
        if (c == '#' && i + 1 < end && src[i + 1] == '|') {
            if (!in_tok) { count++; in_tok = true; }
            in_bc = true; i += 2; continue;
        }
        if (c == '(' || c == '[' || c == '{') {
            if (!in_tok) { count++; in_tok = true; }
            bd++; i++; continue;
        }
        if (c == ')' || c == ']' || c == '}') {
            /* Unmatched close at top level — treat as boundary. */
            in_tok = false; i++; continue;
        }
        if (c == '$' && (i + 1 >= end ||
                         src[i + 1] == ' ' || src[i + 1] == '\t')) {
            count++;
            i++;
            while (i < end && src[i] != ';' && src[i] != '\n') {
                i = sweet_rest_step(src, i, end, scheme);
            }
            in_tok = false;
            continue;
        }
        if (!in_tok) { count++; in_tok = true; }
        i++;
    }
    return count;
}

static SweetLine *sweet_collect_lines(const char *src, size_t len,
                                       size_t *out_n, bool scheme) {
    SweetLine *lines = NULL;
    size_t cap = 0, n = 0;

    size_t i = 0;
    while (i < len) {
        size_t phys_start = i;
        size_t indent_bytes = sweet_indent_bytes(src, i, len);
        int indent = sweet_indent_col(src, i, len);
        size_t content_start = phys_start + indent_bytes;
        bool blank = sweet_line_is_blank(src, phys_start, len);
        bool is_group = !blank &&
                        sweet_line_is_group(src, content_start, len);

        /* Scan to end of logical line.  When the line opens with a GROUP
         * marker (`\\`), step past it before the scanner runs so the bare
         * `\\` is not misread as a `\\`-line-continuation. */
        int bd = 0;
        bool in_str = false, in_bc = false, in_cb = false;
        size_t j = content_start;
        if (is_group) j = sweet_skip_group_marker(src, j, len);
        while (j < len) {
            char c = src[j];
            if (in_cb) {
                /* Inside a ```c ... ``` block: opaque, and newlines do not
                 * end the logical line (the fence spans physical lines). */
                if (sweet_at_fence(src, j, len)) { in_cb = false; j += 3; continue; }
                j++; continue;
            }
            if (!in_str && !in_bc && sweet_at_fence(src, j, len)) {
                in_cb = true; j += 3; continue;
            }
            if (in_str) {
                if (c == '\\' && j + 1 < len) { j += 2; continue; }
                if (c == '"') in_str = false;
                j++; continue;
            }
            if (in_bc) {
                if (c == '|' && j + 1 < len && src[j + 1] == '#') {
                    in_bc = false; j += 2; continue;
                }
                j++; continue;
            }
            if (c == '"') { in_str = true; j++; continue; }
            if (c == '#' && j + 1 < len && src[j + 1] == '|') {
                in_bc = true; j += 2; continue;
            }
            {
                size_t k = sweet_lexeme_end(src, j, len, scheme);
                if (k != j) { j = k; continue; }
            }
            if (c == ';') {
                while (j < len && src[j] != '\n') j++;
                continue;
            }
            if (bd == 0 && c == '\\') {
                size_t k = sweet_skip_line_cont(src, j, len);
                if (k != j) { j = k; continue; }
            }
            if (c == '(' || c == '[' || c == '{') { bd++; j++; continue; }
            if (c == ')' || c == ']' || c == '}') {
                if (bd > 0) bd--;
                j++; continue;
            }
            if (c == '\n') {
                if (bd == 0) break;
                j++; continue;
            }
            j++;
        }

        if (n == cap) {
            cap = cap ? cap * 2 : 32;
            SweetLine *grown = (SweetLine *)realloc(lines, cap * sizeof *grown);
            if (!grown) { free(lines); *out_n = 0; return NULL; }
            lines = grown;
        }
        SweetLine *L = &lines[n++];
        L->phys_start = phys_start;
        L->content_start = content_start;
        L->content_end = j;
        L->indent = indent;
        L->blank = blank;
        L->is_group = is_group;
        L->open_count = 0;
        L->close_count = 0;
        L->contributes_elems = 1;

        if (j < len && src[j] == '\n') j++;
        i = j;
    }

    *out_n = n;
    return lines;
}

/* Recursively annotate open/close counts.  Returns index of the last
 * line consumed by this node (so the caller can resume after it).
 *
 * Also sets H->contributes_elems to the number of elements this node
 * contributes to its parent's element list.  Normal (non-group) nodes
 * always contribute 1 (the bare-token or wrapped form).  GROUP nodes
 * contribute their post-`\\` head tokens plus the sum of their
 * children's contributions, flattening into the parent. */
static size_t sweet_analyze_node(SweetLine *lines, size_t n_lines,
                                  const char *src, size_t idx, bool scheme) {
    SweetLine *H = &lines[idx];
    int my_indent = H->indent;
    size_t head_start = H->content_start;
    if (H->is_group)
        head_start = sweet_skip_group_marker(src, head_start, H->content_end);
    int head_elems = sweet_count_elements(src, head_start, H->content_end, scheme);

    size_t last = idx;
    int child_contrib = 0;
    size_t j = idx + 1;
    while (j < n_lines) {
        if (lines[j].blank) { j++; continue; }
        if (lines[j].indent <= my_indent) break;
        size_t child_first = j;
        last = sweet_analyze_node(lines, n_lines, src, j, scheme);
        child_contrib += lines[child_first].contributes_elems;
        j = last + 1;
    }

    if (H->is_group) {
        /* GROUP line is transparent: do not wrap; flatten own tokens and
         * children's contributions into the parent. */
        H->contributes_elems = head_elems + child_contrib;
    } else {
        if (head_elems + child_contrib > 1) {
            H->open_count++;
            lines[last].close_count++;
        }
        H->contributes_elems = 1;
    }
    return last;
}

static void sweet_analyze_top(SweetLine *lines, size_t n_lines,
                               const char *src, bool scheme) {
    size_t i = 0;
    while (i < n_lines) {
        if (lines[i].blank) { i++; continue; }
        size_t last = sweet_analyze_node(lines, n_lines, src, i, scheme);
        i = last + 1;
    }
}

/* True when [start, end) is *already* exactly one complete, delimited
 * expression: an optional prefix run (an identifier for a neoteric call,
 * or reader/quote sigils like `#map`, `'`, `` ` ``) glued directly to one
 * balanced `(`/`[`/`{` group that closes at the end of the range.
 *
 * `$` means "wrap the rest of the line in one pair of parens", which is
 * right for a bare token sequence (`f $ g 1 2` => `(f (g 1 2))`) but adds a
 * second application layer when the rest is already one expression --
 * `f $ g(1)` would become `(f ((g 1)))`.  The `$` rewrite consults this to
 * suppress the redundant wrap.  A bare atom (`f $ g`) is deliberately NOT
 * covered: SRFI-110 specifies `(f (g))` there, and that is a call, not a
 * double application. */
static bool sweet_dollar_rest_is_delimited(const char *src,
                                            size_t start, size_t end,
                                            bool scheme) {
    while (start < end && (src[start] == ' ' || src[start] == '\t')) start++;
    while (end > start && (src[end - 1] == ' ' || src[end - 1] == '\t' ||
                           src[end - 1] == '\r' || src[end - 1] == '\n'))
        end--;
    if (start >= end) return false;

    /* Prefix: everything before the first opening delimiter.  Any
     * whitespace, string quote, stray closer, continuation or second `$`
     * in it means the rest is a sequence, not a single expression. */
    size_t p = start;
    while (p < end) {
        char c = src[p];
        if (c == '(' || c == '[' || c == '{') break;
        if (c == ' ' || c == '\t' || c == '"' || c == '\\' || c == '$' ||
            c == ')' || c == ']' || c == '}')
            return false;
        p++;
    }
    if (p >= end) return false;  /* no delimited group at all */

    /* The group must be balanced and must close exactly at `end`. */
    int bd = 0;
    bool in_str = false;
    size_t q = p;
    while (q < end) {
        char c = src[q];
        if (in_str) {
            if (c == '\\' && q + 1 < end) { q += 2; continue; }
            if (c == '"') in_str = false;
            q++; continue;
        }
        if (c == '"') { in_str = true; q++; continue; }
        {
            size_t k = sweet_lexeme_end(src, q, end, scheme);
            if (k != q) { q = k; continue; }
        }
        if (c == '(' || c == '[' || c == '{') { bd++; q++; continue; }
        if (c == ')' || c == ']' || c == '}') {
            bd--; q++;
            if (bd == 0) break;
            if (bd < 0) return false;
            continue;
        }
        q++;
    }
    if (in_str || bd != 0) return false;
    return q == end;
}

/* Emit the line content with `$` rewritten to `(<rest-of-line>)`.
 * Records xform→original byte runs in e->map (when non-NULL) so
 * diagnostic snippets can be rendered from the user's original source. */
static void sweet_emit_content(SweetEmit *e, const char *src,
                                size_t start, size_t end) {
    int bd = 0;
    bool in_str = false, in_bc = false, in_cb = false;
    size_t i = start;
    while (i < end) {
        char c = src[i];
        if (in_cb) {
            /* Inside a ```c ... ``` block: copy verbatim, no `$`/paren logic. */
            if (sweet_at_fence(src, i, end)) {
                emit_copy_char_(e, src, i);
                emit_copy_char_(e, src, i + 1);
                emit_copy_char_(e, src, i + 2);
                in_cb = false; i += 3; continue;
            }
            emit_copy_char_(e, src, i); i++; continue;
        }
        if (!in_str && !in_bc && sweet_at_fence(src, i, end)) {
            emit_copy_char_(e, src, i);
            emit_copy_char_(e, src, i + 1);
            emit_copy_char_(e, src, i + 2);
            in_cb = true; i += 3; continue;
        }
        if (in_str) {
            emit_copy_char_(e, src, i);
            if (c == '\\' && i + 1 < end) {
                emit_copy_char_(e, src, i + 1); i += 2; continue;
            }
            if (c == '"') in_str = false;
            i++; continue;
        }
        if (in_bc) {
            emit_copy_char_(e, src, i);
            if (c == '|' && i + 1 < end && src[i + 1] == '#') {
                emit_copy_char_(e, src, i + 1); in_bc = false; i += 2; continue;
            }
            i++; continue;
        }
        {
            size_t k = sweet_lexeme_end(src, i, end, e->scheme);
            if (k != i) { emit_copy_(e, src, i, k - i); i = k; continue; }
        }
        if (c == ';') {
            /* copy the comment through to EOL (inside a bracket group too:
             * its brackets are not structure) */
            size_t j = i;
            while (j < end && src[j] != '\n') j++;
            emit_copy_(e, src, i, j - i);
            i = j;
            continue;
        }
        if (c == '"') { in_str = true; emit_copy_char_(e, src, i); i++; continue; }
        if (c == '#' && i + 1 < end && src[i + 1] == '|') {
            in_bc = true; emit_copy_char_(e, src, i); emit_copy_char_(e, src, i + 1);
            i += 2; continue;
        }
        if (c == '(' || c == '[' || c == '{') { bd++; emit_copy_char_(e, src, i); i++; continue; }
        if (c == ')' || c == ']' || c == '}') {
            if (bd > 0) bd--;
            emit_copy_char_(e, src, i); i++; continue;
        }
        if (c == '\\') {
            size_t k = sweet_skip_line_cont(src, i, end);
            if (k != i) {
                /* Locate the `\n` (skip the optional spaces/tabs the
                 * line-cont allows between `\` and the newline). */
                size_t nl = i + 1;
                while (nl < end && (src[nl] == ' ' || src[nl] == '\t')) nl++;
                /* Emit `\n` (inserted) so transformed line count matches
                 * the user's source, then re-emit the next physical
                 * line's indent so token columns stay accurate.  The
                 * trailing indent IS from the original source — record
                 * it as a copy. */
                emit_insert_char_(e, '\n');
                size_t ind = (nl < end) ? nl + 1 : nl;
                if (ind < k) emit_copy_(e, src, ind, k - ind);
                i = k;
                continue;
            }
        }
        if (bd == 0 && c == '$' &&
            (i + 1 >= end || src[i + 1] == ' ' || src[i + 1] == '\t')) {
            /* Replace `$ <rest>` with `(<rest>)`. */
            i++;
            /* Skip the whitespace immediately after `$`. */
            while (i < end && (src[i] == ' ' || src[i] == '\t')) i++;
            /* Recurse on the remaining content of the line so a second
             * `$` further along still works (left-associative would also
             * be valid; SRFI-105/110 says `$` is right-associative inside
             * a line — recursing matches that). */
            size_t rest_end = i;
            while (rest_end < end && src[rest_end] != '\n' && src[rest_end] != ';')
                rest_end++;
            size_t rs = i;
            while (rs < end && src[rs] != '\n' && src[rs] != ';')
                rs = sweet_rest_step(src, rs, end, e->scheme);
            rest_end = rs;
            /* ... but when the rest is already one complete expression --
             * a neoteric call `g(7)`, a parenthesised form `(g 7)`, a
             * curly-infix group, a data literal -- wrapping it again would
             * apply the result as a function.  Emit it as-is. */
            bool wrap = !sweet_dollar_rest_is_delimited(src, i, rest_end, e->scheme);
            if (wrap) emit_insert_char_(e, '(');
            sweet_emit_content(e, src, i, rest_end);
            if (wrap) emit_insert_char_(e, ')');
            i = rest_end;
            continue;
        }
        emit_copy_char_(e, src, i);
        i++;
    }
}

/* Find the offset within [start, end) just past the last "code" byte --
 * i.e. trimming any trailing whitespace and any trailing `;` line comment.
 * Implicit closing parens must be emitted at this point, not at content_end,
 * otherwise a `)` lands after a trailing `; comment` and gets commented out
 * (producing a spurious "unterminated list"). Block comments and string
 * contents count as code (they self-terminate), so only `;` line comments
 * are trimmed. */
static size_t sweet_code_end(const char *src, size_t start, size_t end,
                             bool scheme) {
    size_t code_end = start;
    bool in_str = false, in_bc = false, in_cb = false;
    size_t i = start;
    while (i < end) {
        char c = src[i];
        if (in_cb) {
            /* ```c ... ``` block: opaque code (the fence self-terminates). */
            if (sweet_at_fence(src, i, end)) { code_end = i + 3; in_cb = false; i += 3; continue; }
            code_end = i + 1; i++; continue;
        }
        if (!in_str && !in_bc && sweet_at_fence(src, i, end)) {
            in_cb = true; code_end = i + 3; i += 3; continue;
        }
        if (in_str) {
            if (c == '\\' && i + 1 < end) { code_end = i + 2; i += 2; continue; }
            if (c == '"') in_str = false;
            code_end = i + 1; i++; continue;
        }
        if (in_bc) {
            if (c == '|' && i + 1 < end && src[i + 1] == '#') {
                in_bc = false; code_end = i + 2; i += 2; continue;
            }
            code_end = i + 1; i++; continue;
        }
        if (c == '"') { in_str = true; code_end = i + 1; i++; continue; }
        if (c == '#' && i + 1 < end && src[i + 1] == '|') {
            in_bc = true; code_end = i + 2; i += 2; continue;
        }
        {
            size_t k = sweet_lexeme_end(src, i, end, scheme);
            if (k != i) { code_end = k; i = k; continue; }
        }
        if (c == ';') {
            /* Line comment -- skip to EOL without advancing code_end. */
            while (i < end && src[i] != '\n') i++;
            continue;
        }
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') { i++; continue; }
        code_end = i + 1; i++;
    }
    return code_end;
}

static char *sweet_preprocess(Arena *arena, const char *src, size_t len,
                               size_t *out_len, SweetMap *out_map,
                               bool scheme) {
    size_t n_lines = 0;
    SweetLine *lines = sweet_collect_lines(src, len, &n_lines, scheme);
    if (!lines && n_lines == 0) {
        char *empty = (char *)arena_alloc(arena, 1);
        empty[0] = 0;
        *out_len = 0;
        return empty;
    }
    sweet_analyze_top(lines, n_lines, src, scheme);

    Buf b; buf_init(&b);
    SweetEmit emit = { .out = &b, .map = out_map, .arena = arena, .scheme = scheme };
    size_t cur = 0;
    for (size_t i = 0; i < n_lines; i++) {
        SweetLine *L = &lines[i];

        /* Copy anything between cur and phys_start verbatim (normally
         * empty — physical lines are contiguous). */
        if (cur < L->phys_start)
            emit_copy_(&emit, src, cur, L->phys_start - cur);

        /* Indent whitespace (verbatim copy from the original line). */
        if (L->content_start > L->phys_start)
            emit_copy_(&emit, src, L->phys_start,
                       L->content_start - L->phys_start);

        /* Implicit opens (inserted, no original counterpart). */
        for (int k = 0; k < L->open_count; k++) emit_insert_char_(&emit, '(');

        /* Content, with `$` rewrites.  For GROUP lines, skip the leading
         * `\\` marker so it does not appear in the output. */
        size_t cstart = L->content_start;
        if (L->is_group)
            cstart = sweet_skip_group_marker(src, cstart, L->content_end);

        /* Split off any trailing line comment so the implicit closes land
         * before it (a `)` after a `; comment` would be commented out). */
        size_t code_end = sweet_code_end(src, cstart, L->content_end, scheme);
        sweet_emit_content(&emit, src, cstart, code_end);

        /* Implicit closes (inserted). */
        for (int k = 0; k < L->close_count; k++) emit_insert_char_(&emit, ')');

        /* Trailing whitespace + line comment, copied verbatim after closes. */
        if (code_end < L->content_end)
            emit_copy_(&emit, src, code_end, L->content_end - code_end);

        /* Trailing newline (copied from original). */
        cur = L->content_end;
        if (cur < len && src[cur] == '\n') {
            emit_copy_char_(&emit, src, cur);
            cur++;
        }
    }
    if (cur < len) emit_copy_(&emit, src, cur, len - cur);

    char *result = (char *)arena_alloc(arena, b.len + 1);
    /* An empty result leaves b.data NULL, and memcpy's source must not be
     * NULL even for zero bytes (UBSan, tests/fuzz/fuzz_manifest). */
    if (b.len) memcpy(result, b.data, b.len);
    result[b.len] = 0;
    *out_len = b.len;
    buf_free(&b);
    free(lines);
    return result;
}

/* TR2 (turi-incremental-elaboration-design): offset-aware core.
 *
 * `start_offset` / `start_line` let the interpreter re-read ONLY the newly
 * appended tail of a long-lived eval session's accumulated source while still
 * passing the FULL accumulated blob as `file` -- so the Forms it produces carry
 * absolute spans into that blob and diagnostics render correctly, without
 * re-parsing the prefix every turn (the O(N^2) re-parse).
 *
 * `read_all_with_registry` is exactly this with (0, 1), so the compiler path is
 * byte-identical. Callers must not use a non-zero offset with READER_SWEET: the
 * t-expression preprocessor rewrites the whole buffer, so an offset expressed in
 * original coordinates is meaningless in transformed ones. The interpreter
 * guards on reader_type before opting in. */
Form **read_all_with_registry_from(Arena *arena, SymbolTable *st,
                                   const SourceFile *file,
                                   struct ReaderMacroRegistry *external_reg,
                                   uint32_t start_offset, uint32_t start_line,
                                   uint32_t *out_count) {
    /* For READER_SWEET, run the t-expression preprocessor first.  The
     * transformed source replaces the SourceFile in the diag registry so
     * that span offsets recorded in Forms (which index into r.src) match
     * what diagnostics print as the file's contents. */
    const SourceFile *eff_file = file;
    if (file->reader_type == READER_SWEET || file->reader_type == READER_R7RS_SWEET) {
        size_t xlen = 0;
        SweetMap *xmap = (SweetMap *)arena_alloc(arena, sizeof *xmap);
        xmap->runs = NULL;
        xmap->n_runs = 0;
        xmap->cap_runs = 0;
        char  *xsrc = sweet_preprocess(arena, file->src, file->len, &xlen, xmap,
                                       file->reader_type == READER_R7RS_SWEET);
        /* TUR_SWEET_DUMP=1 prints the preprocessed source for debugging. */
        if (getenv("TUR_SWEET_DUMP")) {
            fprintf(stderr, "==== sweet-exp preprocessed (%s) ====\n%.*s====\n",
                    file->path, (int)xlen, xsrc);
        }
        SourceFile *xfile = (SourceFile *)arena_alloc(arena, sizeof *xfile);
        *xfile = *file;
        xfile->src = xsrc;
        xfile->len = xlen;
        xfile->orig_src = file->src;
        xfile->orig_len = file->len;
        xfile->xform_map = xmap;
        diag_register_file(xfile);
        eff_file = xfile;
    }

    Reader r;
    r.file = eff_file;
    r.arena = arena;
    r.st = st;
    r.src = eff_file->src;
    r.len = eff_file->len;
    /* TR2: resume at the caller's offset (0 for every non-interpreter caller).
     * Clamped so a stale offset can never read past the buffer. */
    r.pos = (start_offset <= eff_file->len) ? start_offset : (uint32_t)eff_file->len;
    r.line = start_line ? start_line : 1;
    r.col = 1;
    r.error = false;
    r.at_list_head = false;
    /* Phase S1: Set syntax feature flags based on reader type from SourceFile.
     * curly-infix is on in every dialect now -- bare {a + b} reads as (+ a b)
     * regardless of #lang.  Contract types live behind explicit `#refine{...}`. */
    r.curly_infix_enabled = true;
    r.neoteric_enabled = false;
    r.scheme_enabled = false;
    r.fold_case = false;
    r.turmeric_literal_depth = 0;
    /* RM1: Reader-macro registry. If the caller supplied one, dispatch and
     * registration happen against it directly (REPL session semantics);
     * otherwise we keep a per-call local one (file semantics). */
    ReaderMacroRegistry local_reg;
    ReaderMacroRegistry *reg = external_reg;
    if (!reg) {
        reader_macros_init(&local_reg, arena);
        reg = &local_reg;
    }
    r.user_macros = reg;

    /* Install the built-in `#`-dispatch macros before the first form.  These
     * are unconditional -- no `#lang` token, no `#use-reader-macros` -- so
     * every file gets them regardless of its base dialect.  Idempotent, so a
     * persistent REPL/interp registry and a `(load)`-shared strict registry
     * are both safe; see reader_macros.c. */
    reader_macros_install_builtins(reg, arena, st);

    /* The LANGUAGE axis gets the same treatment, in the same place, because it
     * is the same decision one level up.  Since saffron graduated at 0.46.0
     * this cannot fail -- no dialect is gated -- but it is still where a
     * dynamic-language file records itself for the emitter (g_opt_dynamic_any).
     *
     * Here rather than at each detection site: every path that elaborates a
     * file -- compile, `--interpret`, an imported module, the REPL -- funnels
     * through this reader entry, so one call covers them all and none can
     * forget.  The detection sites' only job is to put the dialect on the
     * SourceFile. */
    (void)lang_dialect_apply(file->lang, file->path);

    switch (file->reader_type) {
        case READER_TURMERIC:
            /* Standard s-expression syntax only */
            break;
        case READER_CURLY_INFIX:
            r.curly_infix_enabled = true;
            break;
        case READER_NEOTERIC:
            r.curly_infix_enabled = true;
            r.neoteric_enabled = true;
            break;
        case READER_SWEET:
            r.curly_infix_enabled = true;
            r.neoteric_enabled = true;
            break;
        case READER_R7RS:
            /* r7rs-lang-plan R1: the Scheme lexical layer, on the same
             * s-expression reader.  Curly-infix stays on -- SRFI-105 is a
             * Scheme SRFI, and `{` is reserved in R7RS rather than spoken
             * for -- and neoteric stays off, since `f(x)` is not Scheme. */
            r.scheme_enabled = true;
            break;
        case READER_R7RS_SWEET:
            /* r7rs-sweet-base-dialect-missing: the Scheme lexemes over the
             * text the preprocessor above made, with SRFI-105's neoteric
             * `f(x)` -- which only ever follows a symbol, so `#(1 2)`,
             * `#u8(1)`, `'(a b)` and a number keep their Scheme reading. */
            r.scheme_enabled = true;
            r.neoteric_enabled = true;
            break;
        case READER_UNKNOWN:
            break;
    }

    /* Skip a Racket-style shebang line at file start (`#!/usr/bin/env tur`
     * etc.).  The shebang is recognized only at byte 0 and must look like
     * `#!` followed by `/`, whitespace, or EOL — leaving `#!fold-case`-style
     * future reader directives unaffected. */
    if (r.pos == 0 && r.len >= 2 && r.src[0] == '#' && r.src[1] == '!' &&
        (r.len < 3 || r.src[2] == '/' || r.src[2] == ' ' || r.src[2] == '\t' ||
         r.src[2] == '\n' || r.src[2] == '\r')) {
        while (r.pos < r.len && r.src[r.pos] != '\n') r.pos++;
        if (r.pos < r.len) { r.pos++; r.line++; r.col = 1; }
    }

    Form **forms = NULL;
    size_t cap = 0, n = 0;

    for (;;) {
        skip_ws_and_comments(&r);
        if (r.error) {
            free(forms);
            return NULL;
        }
        if (peek(&r) == -1) break;
        /* RM4: top-level `#use-reader-macros "path"` directive — read
         * macros from another file before continuing this one. */
        if (try_consume_use_directive(&r, reg)) {
            if (r.error) { free(forms); return NULL; }
            continue;
        }
        Form *f = read_form(&r);
        if (!f) {
            /* A NULL return with no error flag set means the reader hit EOF
             * after consuming whitespace, comments, or a datum-comment
             * `#;<form>` whose trailing form was the last meaningful token
             * in the file.  Treat as end-of-input rather than parse error. */
            if (!r.error) break;
            free(forms);
            return NULL;
        }
        /* RM1: top-level `(reader-macros/define ...)` is a directive: it
         * registers an entry in the registry that subsequent forms can
         * dispatch off, and is stripped from the output Form stream. */
        if (reader_macros_is_define_form(f)) {
            if (reader_macros_register_from_form(reg, f) != 0) {
                r.error = true;
                free(forms);
                return NULL;
            }
            /* Normally the directive is a pure read-time side effect and is
             * stripped from the output.  The formatter opts to keep it (via
             * reg->keep_define_forms) so `tur fmt` round-trips the source
             * instead of silently deleting the definition. */
            if (!reg || !reg->keep_define_forms)
                continue;
        }

        if (n == cap) {
            cap = cap ? cap * 2 : 4;
            forms = (Form **)realloc(forms, cap * sizeof(Form *));
            if (!forms) { fprintf(stderr, "tur: oom\n"); abort(); }
        }
        forms[n++] = f;
    }

    Form **out = (Form **)arena_alloc(arena, sizeof(Form *) * (n + 1));
    for (size_t i = 0; i < n; i++) out[i] = forms[i];
    out[n] = NULL;
    free(forms);

    *out_count = (uint32_t)n;
    return out;
}

/* Whole-file read: the offset-aware core starting at the top. Every compiler
 * path goes through here, so its behavior is unchanged by TR2. */
Form **read_all_with_registry(Arena *arena, SymbolTable *st,
                              const SourceFile *file,
                              struct ReaderMacroRegistry *external_reg,
                              uint32_t *out_count) {
    return read_all_with_registry_from(arena, st, file, external_reg,
                                       /*start_offset=*/0, /*start_line=*/1,
                                       out_count);
}

Form **read_all(Arena *arena, SymbolTable *st, const SourceFile *file,
                uint32_t *out_count) {
    return read_all_with_registry(arena, st, file, NULL, out_count);
}

/* #lang directive detection and reader type utilities */

/* Resolve a `#lang` base name (the first, possibly slash-namespaced, token)
 * to a ReaderType.  Returns (ReaderType)-1 for an unknown base.
 *
 * saffron-lang-plan D1: the base token now names a PAIR -- a language and a
 * reader -- so `out_dialect` receives the language half.  The two axes are
 * independent: `<lang>` alone means that language's default reader, and
 * `<lang>/<reader>` picks one explicitly, so every reader is spellable under
 * every language.  Callers that only want the reader pass NULL.
 *
 * `sweet-exp` stays a Turmeric-only legacy alias: it predates the language
 * axis, and giving it a Saffron spelling would invent a second way to say
 * something `saffron/sweet` already says. */
static ReaderType lang_base_from_name(const char *name, size_t len,
                                      LangDialect *out_dialect) {
    if (out_dialect) *out_dialect = LANG_TURMERIC;

    /* L3: `sweet-exp` is the legacy alias for `turmeric/sweet`, accepted
     * silently through v1 and deliberately not a LANG_BASES[] row, so it is
     * never generated and never listed. */
    if (len == 9 && memcmp(name, "sweet-exp", 9) == 0)
        return READER_SWEET;

    /* r7rs-lang-plan R1: every other spelling is a row of LANG_BASES[] in
     * lang_dialects.c -- the same table `tur dialects` prints and the
     * playground picker walks -- so a base that can be listed can be named
     * and vice versa, with no second copy to keep in step. */
    LangDialect d = LANG_TURMERIC;
    ReaderType  r = READER_TURMERIC;
    if (lang_base_lookup(name, len, &d, &r)) {
        if (out_dialect) *out_dialect = d;
        return r;
    }
    return (ReaderType)-1;
}

/* Tokens that used to be legal `#lang` layers.
 *
 * The layer axis was decommissioned in v0.49.0
 * (docs/archive/lang-layers-decommission-plan.md).  `stringed` -- the only
 * token that was ever legal -- is accepted and ignored for one minor line, so
 * a file that opted in per-file keeps compiling across the boundary, exactly
 * as GRADUATED[] in experiments.c does for `--enable`.  Age out at 0.50.0:
 * empty this list (or delete it with the warning below) and `#lang turmeric
 * stringed` becomes the same TUR-E0330 any other trailing token gets.
 *
 * Six lines and a flag rather than a registry: this is the migration window,
 * not a second axis to grow rows in. */
static const char *const RETIRED_LANG_TOKENS[] = { "stringed", NULL };

static bool g_lang_retired_warned = false;

static bool lang_token_is_retired(const char *name, size_t len) {
    if (!name) return false;
    for (size_t i = 0; RETIRED_LANG_TOKENS[i]; i++) {
        if (strlen(RETIRED_LANG_TOKENS[i]) == len &&
            memcmp(RETIRED_LANG_TOKENS[i], name, len) == 0) {
            if (!g_lang_retired_warned) {
                g_lang_retired_warned = true;
                fprintf(stderr,
                        "warning [TUR-W0064]: `#lang` layer '%.*s' was retired; "
                        "the token is accepted and ignored for one release, and "
                        "its behaviour is now unconditional\n",
                        (int)len, name);
            }
            return true;
        }
    }
    return false;
}

/* Parse a `#lang` directive: the base dialect, and nothing else.  See the
 * declaration in diag.h for the out-param contract. */
ReaderType detect_lang_dialect(const char *src, size_t len,
                               const char **out_rest, size_t *out_rest_len,
                               const char **out_bad, size_t *out_bad_len,
                               LangDialect *out_dialect) {
    const char *p = src;
    size_t remaining = len;

    if (out_dialect) *out_dialect = LANG_TURMERIC;
    if (out_bad)     *out_bad     = NULL;
    if (out_bad_len) *out_bad_len = 0;

    /* Racket-style shebang: if the file starts with `#!` (followed by `/` or
     * whitespace, to disambiguate from any future `#!`-dispatched form),
     * skip the whole shebang line so `#lang ...` on the next line is still
     * detected. */
    if (remaining >= 2 && p[0] == '#' && p[1] == '!' &&
        (remaining < 3 || p[2] == '/' || p[2] == ' ' || p[2] == '\t' ||
         p[2] == '\n' || p[2] == '\r')) {
        while (remaining > 0 && *p != '\n') { p++; remaining--; }
        if (remaining > 0) { p++; remaining--; }  /* past the newline */
    }

    /* Skip leading whitespace (but not newlines - #lang must be first line) */
    while (remaining > 0 && (p[0] == ' ' || p[0] == '\t')) {
        p++;
        remaining--;
    }

    /* Check for #lang */
    if (remaining >= 5 && p[0] == '#' && p[1] == 'l' && p[2] == 'a' &&
        p[3] == 'n' && p[4] == 'g') {
        p += 5;
        remaining -= 5;

        /* Skip whitespace after #lang */
        while (remaining > 0 && (p[0] == ' ' || p[0] == '\t')) {
            p++;
            remaining--;
        }

        /* Extract the base name (the first token; it can contain slashes). */
        const char *lang_start = p;
        size_t lang_len = 0;
        while (remaining > 0 && p[0] != ' ' && p[0] != '\t' &&
               p[0] != '\n' && p[0] != '\r') {
            p++;
            lang_len++;
            remaining--;
        }

        ReaderType base = lang_base_from_name(lang_start, lang_len, out_dialect);
        /* lang-unknown-base-diagnostic-names-nothing: an unrecognised BASE
         * used to come back as (ReaderType)-1 with its token dropped, so the
         * only thing a caller could print was reader_type_name(READER_UNKNOWN)
         * -- the literal word "unknown", as if it were what the user wrote.
         * Hand the token out through the same slot an unknown LAYER already
         * uses (callers tell the two apart by the returned type: a base
         * failure is READER_UNKNOWN, a layer failure leaves the base valid),
         * and return READER_UNKNOWN rather than -1 so every caller's
         * reader_type_is_implemented test sees one value. */
        if (base == (ReaderType)-1 || base == READER_UNKNOWN) {
            if (out_bad && *out_bad == NULL) {
                *out_bad = lang_start;
                if (out_bad_len) *out_bad_len = lang_len;
            }
            base = READER_UNKNOWN;
        }

        /* Classify each trailing token, then consume to end-of-line so no
         * token ever leaks into the body handed to the reader.  The
         * EOL-consumption is why the directive never leaks and is independent
         * of what the tokens mean, so it survived the layer axis.  `p` stops
         * AT the newline (matching the no-trailing-token path, where the
         * base-name loop already halts on `\n`/`\r`), leaving the terminator
         * in place so the body keeps its original line numbering -- the empty
         * line 1 the reader sees stands in for the stripped `#lang` line. */
        for (;;) {
            while (remaining > 0 && (p[0] == ' ' || p[0] == '\t')) {
                p++;
                remaining--;
            }
            if (remaining == 0 || p[0] == '\n' || p[0] == '\r') break;

            const char *tok = p;
            size_t tok_len = 0;
            while (remaining > 0 && p[0] != ' ' && p[0] != '\t' &&
                   p[0] != '\n' && p[0] != '\r') {
                p++;
                tok_len++;
                remaining--;
            }

            /* Two-way: a retired layer token (accepted, warned, ignored) or a
             * bad one.  `#lang` takes a base dialect and nothing else. */
            if (lang_token_is_retired(tok, tok_len)) continue;
            if (out_bad && *out_bad == NULL) {
                *out_bad = tok;              /* first trailing token */
                if (out_bad_len) *out_bad_len = tok_len;
            }
        }

        if (out_rest) *out_rest = p;
        if (out_rest_len) *out_rest_len = remaining;
        return base;   /* (ReaderType)-1 when the base name is unknown */
    }

    /* No #lang directive found */
    if (out_rest) *out_rest = src;
    if (out_rest_len) *out_rest_len = len;
    return READER_TURMERIC;
}

/* saffron-lang-plan S1: the pre-dialect entry point, now a wrapper.  Kept so
 * the callers that have no use for the language axis need no change -- the
 * LSP, the package reader, `tur parse-check`. */
ReaderType detect_lang(const char *src, size_t len,
                       const char **out_rest, size_t *out_rest_len,
                       const char **out_bad, size_t *out_bad_len) {
    return detect_lang_dialect(src, len, out_rest, out_rest_len,
                               out_bad, out_bad_len, NULL);
}

/* Get reader type from file extension.
 *
 * `.scm` is `#lang r7rs` without the line (r7rs-lang-plan, open question 4,
 * decided 2026-09-25): the Scheme reader, and -- through
 * lang_dialect_from_extension below -- the Scheme language.  A `.scm` file
 * that carries a `#lang` line keeps it as a redundant hint, as a `.tur.sweet`
 * file does. */
ReaderType reader_type_from_extension(const char *path) {
    if (!path) return READER_TURMERIC;
    size_t n = strlen(path);
    if (n >= 10 && strcmp(path + n - 10, ".tur.sweet") == 0) {
        return READER_SWEET;
    }
    if (n >= 4 && strcmp(path + n - 4, ".scm") == 0) {
        return READER_R7RS;
    }
    return READER_TURMERIC;
}

/* The language an extension selects on its own: LANG_R7RS for `.scm`,
 * LANG_TURMERIC (meaning "the directive decides, else Turmeric") for every
 * other spelling.  The reader-only override above is deliberately not
 * enough for `.scm`: a Scheme file read by the Scheme reader but elaborated
 * as Turmeric would have no prelude and no Scheme truthiness. */
LangDialect lang_dialect_from_extension(const char *path) {
    if (!path) return LANG_TURMERIC;
    size_t n = strlen(path);
    if (n >= 4 && strcmp(path + n - 4, ".scm") == 0) return LANG_R7RS;
    return LANG_TURMERIC;
}

/* Get reader type name as string.  Always the canonical slash-namespaced
 * spelling: the legacy `sweet-exp` alias is accepted on input
 * (lang_base_from_name) but never generated, so a round-trip through
 * detect_lang -> reader_type_name is stable. */
const char *reader_type_name(ReaderType type) {
    switch (type) {
        case READER_UNKNOWN: return "unknown";
        case READER_TURMERIC: return "turmeric";
        case READER_CURLY_INFIX: return "turmeric/curly-infix";
        case READER_NEOTERIC: return "turmeric/neoteric";
        case READER_SWEET: return "turmeric/sweet";
        /* r7rs-lang-plan R1: the Scheme reader is owned by one language and
         * has no `turmeric/` spelling; the base token IS the name. */
        case READER_R7RS: return "r7rs";
        case READER_R7RS_SWEET: return "r7rs/sweet";
        default: return "<invalid>";
    }
}

/* saffron-lang-plan S1: canonical dialect name, the sibling of
 * reader_type_name.  Used by `tur dialects` and by diagnostics that need to
 * say which language a file is in. */
const char *lang_dialect_name(LangDialect d) {
    switch (d) {
        case LANG_TURMERIC: return "turmeric";
        case LANG_SAFFRON:  return "saffron";
        case LANG_R7RS:     return "r7rs";
        default:            return "<invalid>";
    }
}

/* Check if a reader type is implemented */
bool reader_type_is_implemented(ReaderType type) {
    switch (type) {
        case READER_UNKNOWN:
            return false; /* Unknown language is not implemented */
        case READER_TURMERIC:
        case READER_CURLY_INFIX:
        case READER_NEOTERIC:
            return true; /* Phase S2: neoteric is now implemented */
        case READER_SWEET:
            return true; /* indent-sensitive t-expressions + curly-infix + neoteric */
        case READER_R7RS:
            return true; /* r7rs-lang-plan R1: the Scheme reader variant */
        case READER_R7RS_SWEET:
            return true; /* the sweet-exp preprocessor over Scheme's lexemes */
        default:
            return false;
    }
}

/* repl-continuation-counter-misreads-reader-syntax: how open `src` is, lexed
 * the way the reader lexes it -- the question a REPL asks before deciding
 * whether the lines typed so far are a whole form yet.
 *
 * The REPL used to answer it with a per-line bracket count that knew only `"`
 * strings and `;` comments, so a ```c body's brackets and semicolons were read
 * as Lisp (`for (i = 0; ...)` left the prompt open for good), a string's
 * brackets on its second line were structure, and so were those inside a
 * `#| |#` comment or a `#\(` literal.  This walks the WHOLE accumulated input
 * once, with the reader's lexemes:
 *
 *   - `"..."` strings, backslash escapes included, across lines;
 *   - ```c ... ``` fences (opaque up to the next ```, as read_cblock reads);
 *   - `#| ... |#` block comments, NESTED (skip_block_comment counts depth);
 *   - `;` line comments, and `#;` as a datum-comment prefix (every dialect --
 *     skip_ws_and_comments does not gate it on Scheme), whose `;` is not one;
 *   - `#\c` character literals, and Scheme's `|delimited symbols|`, via
 *     sweet_lexeme_end, the helper the sweet preprocessor already shares.
 *
 * Returns the count of brackets left open (negative when over-closed).  Input
 * that ends inside a string, fence or block comment is never complete: that
 * returns at least 1 and sets *in_lexeme, so the caller can also tell that a
 * blank line typed there is content rather than a request to cancel.
 * Reader macros (#use-reader-macros) are not modelled; their brackets count
 * as brackets. */
int reader_open_depth(const char *src, size_t len, ReaderType rt,
                      bool *in_lexeme) {
    bool scheme = (rt == READER_R7RS || rt == READER_R7RS_SWEET);
    int  depth = 0;
    int  block = 0;               /* #| |# nesting */
    bool in_str = false, in_cb = false;
    size_t i = 0;
    while (i < len) {
        char c = src[i];
        if (in_cb) {
            if (sweet_at_fence(src, i, len)) { in_cb = false; i += 3; }
            else i++;
            continue;
        }
        if (in_str) {
            if (c == '\\' && i + 1 < len) { i += 2; continue; }
            if (c == '"') in_str = false;
            i++;
            continue;
        }
        if (block > 0) {
            if (c == '#' && i + 1 < len && src[i + 1] == '|') { block++; i += 2; continue; }
            if (c == '|' && i + 1 < len && src[i + 1] == '#') { block--; i += 2; continue; }
            i++;
            continue;
        }
        if (sweet_at_fence(src, i, len)) { in_cb = true; i += 3; continue; }
        if (c == '"') { in_str = true; i++; continue; }
        if (c == '#' && i + 1 < len && src[i + 1] == '|') { block = 1; i += 2; continue; }
        if (c == '#' && i + 1 < len && src[i + 1] == ';') { i += 2; continue; }
        {
            size_t k = sweet_lexeme_end(src, i, len, scheme);
            if (k != i) { i = k; continue; }
        }
        if (c == ';') {
            while (i < len && src[i] != '\n') i++;
            continue;
        }
        if (c == '(' || c == '[' || c == '{') depth++;
        else if (c == ')' || c == ']' || c == '}') depth--;
        i++;
    }
    bool open_lexeme = in_str || in_cb || block > 0;
    if (in_lexeme) *in_lexeme = open_lexeme;
    if (open_lexeme && depth < 1) return 1;
    return depth;
}
