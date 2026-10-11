/* fmt.c — Turmeric source code pretty-printer.
 *
 * Architecture:
 *   fmt_print()       public entry point; iterates top-level forms
 *   fmt_form()        dispatches on FormTag
 *   fmt_list()        handles F_LIST: tries inline, falls back to special or
 *                     generic layout
 *   fmt_form_flat()   renders a form compactly onto a Buf (for measuring)
 *   fmt_measure()     returns flat width of a form (UINT32_MAX if it has \n)
 *
 * Comment handling (option a from plan):
 *   The source text is re-scanned between form spans to extract comments.
 *   If opts.src is NULL, comments are dropped and one blank line is emitted
 *   between top-level forms.
 */

#include "fmt.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stdint.h>

/* ---------------------------------------------------------------------------
 * Internal state
 * ---------------------------------------------------------------------------
 */

typedef struct FmtState {
    Buf       *buf;
    uint32_t   col;   /* current column (0-based) */
    FmtOptions opts;
} FmtState;

/* ---------------------------------------------------------------------------
 * Low-level emit helpers
 * ---------------------------------------------------------------------------
 */

static void fs_putc(FmtState *s, char c) {
    buf_putc(s->buf, c);
    if (c == '\n') s->col = 0;
    else s->col++;
}

static void fs_write(FmtState *s, const char *p, size_t n) {
    for (size_t i = 0; i < n; i++) fs_putc(s, p[i]);
}

static void fs_puts(FmtState *s, const char *str) {
    fs_write(s, str, strlen(str));
}

/* Emit a newline then `col` spaces. */
static void fs_newline_indent(FmtState *s, uint32_t col) {
    fs_putc(s, '\n');
    for (uint32_t i = 0; i < col; i++) fs_putc(s, ' ');
}

/* ---------------------------------------------------------------------------
 * Symbol comparison
 * ---------------------------------------------------------------------------
 */

static bool sym_eq(const Symbol *sym, const char *name) {
    size_t n = strlen(name);
    return (size_t)sym->len == n && memcmp(sym->name, name, n) == 0;
}

/* ---------------------------------------------------------------------------
 * Flat printer — renders a form inline (no line breaks) into a Buf.
 * Used for measuring and for emitting forms that fit on the current line.
 * ---------------------------------------------------------------------------
 */

static void fmt_form_flat(Buf *b, const Form *f);

/* Phase N: map a numeric literal's type suffix back to its source spelling.
 * Without this the formatter silently drops the suffix (e.g. 0i8 -> 0),
 * changing the literal's type. */
static const char *lit_suffix_str(LiteralSuffix suf) {
    switch (suf) {
        case LIT_SUF_I8:  return "i8";
        case LIT_SUF_I16: return "i16";
        case LIT_SUF_I32: return "i32";
        case LIT_SUF_I64: return "i64";
        case LIT_SUF_U8:  return "u8";
        case LIT_SUF_U16: return "u16";
        case LIT_SUF_U32: return "u32";
        case LIT_SUF_U64: return "u64";
        case LIT_SUF_F32: return "f32";
        case LIT_SUF_F64: return "f64";
        case LIT_SUF_NONE:
        default:          return "";
    }
}

/* Render an F_INT / F_FLOAT literal, preserving its type suffix and full
 * value. For floats, plain "%g" defaults to 6 significant digits -- which
 * silently truncated high-precision literals (e.g. PI 3.14159265358979323846
 * -> 3.14159) -- so search for the shortest "%.*g" precision that strtod
 * recovers exactly (17 sig figs always round-trips an IEEE-754 double).
 * "%g" also turns 0.0 into "0", which would re-parse as an int, so append
 * ".0" when the formatted value carries no '.', exponent, or inf/nan marker. */
static void fmt_num_literal(Buf *b, const Form *f) {
    if (f->tag == F_FLOAT) {
        double v = f->as.f;
        char tmp[64];
        int n = -1;
        for (int prec = 1; prec <= 17; prec++) {
            n = snprintf(tmp, sizeof(tmp), "%.*g", prec, v);
            if (n > 0 && (size_t)n < sizeof(tmp) && strtod(tmp, NULL) == v) break;
        }
        if (n < 0 || (size_t)n >= sizeof(tmp)) {
            n = snprintf(tmp, sizeof(tmp), "%.17g", v);
        }
        bool floaty = false;
        for (int i = 0; i < n; i++) {
            char c = tmp[i];
            if (c == '.' || c == 'e' || c == 'E' || c == 'n' || c == 'i') {
                floaty = true;
                break;
            }
        }
        buf_write(b, tmp, (size_t)n);
        if (!floaty) buf_puts(b, ".0");
    } else if (f->lit_suffix == LIT_SUF_CHAR) {
        /* parsec-guide-plan P2: a `#\A` literal reads back as an F_INT; write
         * it as the character it was rather than its code, which would also
         * change its type from Char to int.  Names match the reader's table. */
        int64_t v = f->as.i;
        const char *name = NULL;
        switch (v) {
            case 32:  name = "space";     break;
            case 10:  name = "newline";   break;
            case 9:   name = "tab";       break;
            case 13:  name = "return";    break;
            case 0:   name = "null";      break;
            case 8:   name = "backspace"; break;
            case 127: name = "delete";    break;
            case 27:  name = "escape";    break;
            case 7:   name = "alarm";     break;
            default:  break;
        }
        if (name)                 buf_printf(b, "#\\%s", name);
        else if (v > 32 && v < 127) buf_printf(b, "#\\%c", (char)v);
        else                      buf_printf(b, "#\\u%llx", (unsigned long long)v);
        return;
    } else {
        buf_printf(b, "%lld", (long long)f->as.i);
    }
    buf_puts(b, lit_suffix_str(f->lit_suffix));
}

static void print_str_escaped_b(Buf *b, StrSlice s) {
    buf_putc(b, '"');
    for (uint32_t i = 0; i < s.len; i++) {
        char c = s.p[i];
        switch (c) {
            case '"':  buf_puts(b, "\\\""); break;
            case '\\': buf_puts(b, "\\\\"); break;
            case '\n': buf_puts(b, "\\n");  break;
            case '\t': buf_puts(b, "\\t");  break;
            case '\r': buf_puts(b, "\\r");  break;
            default:
                if ((unsigned char)c < 0x20)
                    buf_printf(b, "\\x%02x", (unsigned char)c);
                else
                    buf_putc(b, c);
        }
    }
    buf_putc(b, '"');
}

/* fx-row-syntax-rename-plan: an F_MAP form carries its source spelling in
 * `fx_prov`.  `#fx{...}` (PROV_FX_EXPLICIT) is the preferred effect-row
 * spelling and MUST round-trip as `#fx{`; the legacy `#{...}`/`@{...}` and
 * plain maps print as `#{`.  Returns the opening delimiter to emit. */
static const char *fx_map_open(const Form *f) {
    return (f->fx_prov == PROV_FX_EXPLICIT) ? "#fx{" : "#{";
}

/* WF1 / C2: `#reads` and `#writes` read as the lists `(reads ...)` and
 * `(writes ...)`, stamped PROV_READS / PROV_WRITES (reader.c).  Neither list
 * has a paren spelling -- `reads` and `writes` are not functions, so a
 * formatted `(reads v)` is a hard `unknown function or operator 'reads'` the
 * next time the file is compiled.  Print them back as the annotation sugar,
 * for the same reason the `&mut x` borrow sugar is special-cased below.
 *
 * Measured before this existed: `tur fmt` turned a clean one-line
 * `(defn vlen [^borrow v : (Vec int)] #reads v : int (vec-len v))` into a file
 * that does not compile -- silently, exit 0.
 *
 * The frame's BRACKETS are kept whenever a bare name would not round-trip:
 * `#writes []` is the meaningful "writes nothing" claim and reads as a
 * one-element `(writes)`, which must not come back as a frameless `#writes`.
 * A single-name frame normalizes to the unbracketed spelling, which is what
 * the guides use and what re-reads identically. */
static bool fmt_frame_annot(Buf *b, const Form *f) {
    const char *kw = NULL;
    if (f->fx_prov == (uint8_t)PROV_READS)       kw = "#reads";
    else if (f->fx_prov == (uint8_t)PROV_WRITES) kw = "#writes";
    if (!kw || f->tag != F_LIST || f->as.list.len == 0) return false;
    buf_puts(b, kw);
    if (f->as.list.len == 2) {
        buf_putc(b, ' ');
        fmt_form_flat(b, f->as.list.items[1]);
        return true;
    }
    buf_puts(b, " [");
    for (uint32_t i = 1; i < f->as.list.len; i++) {
        if (i > 1) buf_putc(b, ' ');
        fmt_form_flat(b, f->as.list.items[i]);
    }
    buf_putc(b, ']');
    return true;
}

static void fmt_form_flat(Buf *b, const Form *f) {
    switch (f->tag) {
        case F_NIL:   buf_puts(b, "nil"); break;
        case F_BOOL:  buf_puts(b, f->as.b ? "true" : "false"); break;
        case F_INT:
        case F_FLOAT: fmt_num_literal(b, f); break;
        case F_STR:   print_str_escaped_b(b, f->as.s); break;
        case F_SYM:
            buf_write(b, f->as.sym->name, f->as.sym->len);
            break;
        case F_KEYWORD:
            buf_putc(b, ':');
            buf_write(b, f->as.sym->name, f->as.sym->len);
            break;
        case F_LIST:
            if (fmt_frame_annot(b, f)) break;
            /* fmt-drops-comments-in-handle-and-binding-modifier-gaps: the
             * reader's `&mut x` borrow sugar reads as the list `(&mut x)`, and
             * that list has no paren spelling -- `(&mut x)` written out reads
             * as `((&mut x))` (reader.c: "the explicit form must be written
             * as &mut x").  Print it back as the sugar, or a second format
             * pass wraps it in another pair of parens. */
            if (f->as.list.len == 2 && f->as.list.items[0]->tag == F_SYM
                && f->as.list.items[0]->as.sym->len == 4
                && memcmp(f->as.list.items[0]->as.sym->name, "&mut", 4) == 0) {
                buf_puts(b, "&mut ");
                fmt_form_flat(b, f->as.list.items[1]);
                break;
            }
            buf_putc(b, '(');
            for (uint32_t i = 0; i < f->as.list.len; i++) {
                if (i) buf_putc(b, ' ');
                fmt_form_flat(b, f->as.list.items[i]);
            }
            buf_putc(b, ')');
            break;
        case F_VEC:
            buf_putc(b, '[');
            for (uint32_t i = 0; i < f->as.list.len; i++) {
                if (i) buf_putc(b, ' ');
                fmt_form_flat(b, f->as.list.items[i]);
            }
            buf_putc(b, ']');
            break;
        case F_MAP:
            buf_puts(b, fx_map_open(f));
            for (uint32_t i = 0; i < f->as.list.len; i++) {
                if (i) buf_putc(b, ' ');
                fmt_form_flat(b, f->as.list.items[i]);
            }
            buf_putc(b, '}');
            break;
        case F_SET:
            buf_puts(b, "#s(");
            for (uint32_t i = 0; i < f->as.list.len; i++) {
                if (i) buf_putc(b, ' ');
                fmt_form_flat(b, f->as.list.items[i]);
            }
            buf_putc(b, ')');
            break;
        case F_CBLOCK:
            buf_puts(b, "```c ");
            buf_write(b, f->as.cblock.p, f->as.cblock.len);
            buf_puts(b, "```");
            break;
        case F_QUOTE:
            buf_putc(b, '\'');
            if (f->as.list.len > 0) fmt_form_flat(b, f->as.list.items[0]);
            break;
        case F_QUASIQUOTE:
            buf_putc(b, '`');
            if (f->as.list.len > 0) fmt_form_flat(b, f->as.list.items[0]);
            break;
        case F_UNQUOTE:
            buf_putc(b, '~');
            if (f->as.list.len > 0) fmt_form_flat(b, f->as.list.items[0]);
            break;
        case F_UNQUOTE_SPLICING:
            buf_puts(b, "~@");
            if (f->as.list.len > 0) fmt_form_flat(b, f->as.list.items[0]);
            break;
        case F_TYPE_ANN:
            buf_puts(b, ": ");
            if (f->as.list.len > 0) fmt_form_flat(b, f->as.list.items[0]);
            break;
        /* CT0: Contract type #refine{ var : T | pred }.  The tag is not
         * optional: bare `{...}` reads as curly-infix now (read_contract_type
         * is reachable only from read_refine_literal), so printing the brace
         * body alone round-trips a refinement into an arithmetic form. */
        case F_CONTRACT_TYPE:
            buf_puts(b, "#refine{ ");
            for (uint32_t _i = 0; _i < f->as.list.len; _i++) {
                if (_i) buf_putc(b, ' ');
                fmt_form_flat(b, f->as.list.items[_i]);
            }
            buf_puts(b, " }");
            break;
        case F_READER_COND:
            buf_puts(b, "#?(");
            for (uint32_t i = 0; i < f->as.list.len; i++) {
                if (i) buf_putc(b, ' ');
                fmt_form_flat(b, f->as.list.items[i]);
            }
            buf_putc(b, ')');
            break;
        /* RR3: Range literal variable annotation -- format the desugared range form */
        case F_RANGE_VAR:
            if (f->as.list.len > 1) fmt_form_flat(b, f->as.list.items[1]);
            break;
        /* DL0: data literals */
        case F_MAP_LITERAL:
            buf_puts(b, "#map{");
            for (uint32_t i = 0; i < f->as.list.len; i++) {
                if (i) buf_putc(b, ' ');
                fmt_form_flat(b, f->as.list.items[i]);
            }
            buf_putc(b, '}');
            break;
        case F_SET_LITERAL:
            buf_puts(b, "#set{");
            for (uint32_t i = 0; i < f->as.list.len; i++) {
                if (i) buf_putc(b, ' ');
                fmt_form_flat(b, f->as.list.items[i]);
            }
            buf_putc(b, '}');
            break;
        case F_ROW_LITERAL:
            buf_puts(b, "#row{");
            for (uint32_t i = 0; i < f->as.list.len; i++) {
                if (i) buf_putc(b, ' ');
                fmt_form_flat(b, f->as.list.items[i]);
            }
            buf_putc(b, '}');
            break;
    }
}

static bool form_contains_multipair_let(const Form *f);

/* Measure the flat width of a form.
 * Returns UINT32_MAX if the form contains a literal newline character, or if it
 * contains a multi-pair `let`/`loop` -- such a form must never be emitted flat
 * (the house style keeps each binding pair on its own line), so reporting it as
 * unmeasurable forces every enclosing inline check to break and recurse down to
 * fmt_let. */
static uint32_t fmt_measure(const Form *f) {
    if (form_contains_multipair_let(f)) return UINT32_MAX;
    Buf tmp;
    buf_init(&tmp);
    fmt_form_flat(&tmp, f);
    bool has_nl = false;
    for (size_t i = 0; i < tmp.len; i++) {
        if (tmp.data[i] == '\n') { has_nl = true; break; }
    }
    uint32_t w = has_nl ? UINT32_MAX : (uint32_t)tmp.len;
    buf_free(&tmp);
    return w;
}

/* Emit a form via the flat printer into the state (always inline). */
static void fmt_emit_inline(FmtState *s, const Form *f) {
    Buf tmp;
    buf_init(&tmp);
    fmt_form_flat(&tmp, f);
    fs_write(s, tmp.data, tmp.len);
    buf_free(&tmp);
}

/* ---------------------------------------------------------------------------
 * Special-form classification
 * ---------------------------------------------------------------------------
 */

typedef enum SpecialForm {
    SF_NONE,
    SF_DEFPACKAGE,          /* (defpackage ...) and (deflockfile ...) */
    SF_DEFN, SF_DEFMACRO,
    SF_FN,
    SF_LET, SF_LOOP,
    SF_IF,
    SF_WHEN, SF_UNLESS,
    SF_DO,
    SF_CASE,
    SF_COND,
    SF_HANDLE,
    SF_DEFCLASS,
    SF_DEFINSTANCE,
    SF_DEFEFFECT,
} SpecialForm;

static const Symbol *list_head_sym(const Form *f) {
    if (f->tag != F_LIST || f->as.list.len == 0) return NULL;
    const Form *h = f->as.list.items[0];
    if (h->tag != F_SYM) return NULL;
    return h->as.sym;
}

static SpecialForm classify_list(const Form *f) {
    const Symbol *h = list_head_sym(f);
    if (!h) return SF_NONE;
    if (sym_eq(h, "defpackage"))  return SF_DEFPACKAGE;
    if (sym_eq(h, "deflockfile")) return SF_DEFPACKAGE;
    if (sym_eq(h, "defn"))        return SF_DEFN;
    if (sym_eq(h, "defmacro"))    return SF_DEFMACRO;
    if (sym_eq(h, "fn"))          return SF_FN;
    if (sym_eq(h, "let"))         return SF_LET;
    if (sym_eq(h, "let*"))        return SF_LET;
    if (sym_eq(h, "if"))          return SF_IF;
    if (sym_eq(h, "when"))        return SF_WHEN;
    if (sym_eq(h, "unless"))      return SF_UNLESS;
    if (sym_eq(h, "do"))          return SF_DO;
    if (sym_eq(h, "case"))        return SF_CASE;
    if (sym_eq(h, "cond"))        return SF_COND;
    if (sym_eq(h, "loop"))        return SF_LOOP;
    if (sym_eq(h, "handle"))      return SF_HANDLE;
    if (sym_eq(h, "handle-shallow")) return SF_HANDLE;  /* F2: same layout as handle */
    if (sym_eq(h, "defclass"))    return SF_DEFCLASS;
    if (sym_eq(h, "definstance")) return SF_DEFINSTANCE;
    if (sym_eq(h, "defeffect"))   return SF_DEFEFFECT;
    return SF_NONE;
}

/* True if f is -- or contains anywhere in its subtree -- a `let`/`let*`/`loop`
 * whose binding vector has two or more pairs (len > 2).  Used by fmt_measure to
 * mark such forms unmeasurable so they are never joined onto one line. */
static bool form_contains_multipair_let(const Form *f) {
    if (!f) return false;
    if (f->tag == F_LIST) {
        const Symbol *h = list_head_sym(f);
        if (h && (sym_eq(h, "let") || sym_eq(h, "let*") || sym_eq(h, "loop")) &&
            f->as.list.len >= 2) {
            const Form *b = f->as.list.items[1];
            if (b->tag == F_VEC && b->as.list.len > 2) return true;
        }
    }
    /* Recurse into the child forms of list/vector nodes (a multi-pair let can be
     * nested inside a call, an if-branch, or a binding value). */
    if (f->tag == F_LIST || f->tag == F_VEC) {
        for (uint32_t i = 0; i < f->as.list.len; i++)
            if (form_contains_multipair_let(f->as.list.items[i])) return true;
    }
    return false;
}

/* ---------------------------------------------------------------------------
 * Pretty-printer — forward declarations
 * ---------------------------------------------------------------------------
 */

static void fmt_form(FmtState *s, const Form *f);
static void fmt_list(FmtState *s, const Form *f);
static uint32_t emit_comments_indented(FmtState *s, uint32_t from_off,
                                       uint32_t to_off, uint32_t col,
                                       bool *must_break);
static bool span_has_comment(FmtState *s, const Form *f);
static void emit_trailing_gap_comments(FmtState *s, uint32_t prev_end,
                                       const Form *f, uint32_t col);

/* fmt_measure, plus: a form whose source span carries a comment is reported as
 * unmeasurable.  The flat printer has no way to re-emit a comment (the parsed
 * AST carries no comment nodes), so any inline collapse of such a form deletes
 * it from the rewritten file -- silently, since `tur fmt` writes in place.
 *
 * Reporting UINT32_MAX rather than checking `span_has_comment` at each call
 * site is deliberate: it makes the form unmeasurable for *enclosing* inline
 * checks too, so a comment inside a vector nested in a form that would itself
 * fit cannot be flattened away one level up.  Every caller that decides
 * "inline vs. break" must use this, not fmt_measure; fmt_measure stays raw for
 * the column-alignment width computations, which never emit anything. */
static uint32_t fmt_measure_src(FmtState *s, const Form *f) {
    if (span_has_comment(s, f)) return UINT32_MAX;
    return fmt_measure(f);
}

/* ---------------------------------------------------------------------------
 * Body-form emission with interior-comment preservation
 * ---------------------------------------------------------------------------
 */

/* Emit f's tail forms (indices [start, len)) one per line at body_col,
 * re-emitting any source comments that sit in the gaps between consecutive
 * forms (and between the last header item and the first body form).  This is
 * what keeps comments that live *inside* a form -- e.g. a ';;' note between a
 * defn signature and its body -- from being dropped, since the parsed AST
 * carries no comment nodes. */
static void fmt_body_forms(FmtState *s, const Form *f, uint32_t start,
                           uint32_t body_col) {
    uint32_t n = f->as.list.len;
    bool have_src = (s->opts.src != NULL) && (start >= 1) && (start <= n);
    uint32_t prev_end = have_src ? f->as.list.items[start - 1]->span.off_end : 0;
    for (uint32_t i = start; i < n; i++) {
        const Form *child = f->as.list.items[i];
        if (have_src) emit_comments_indented(s, prev_end, child->span.off_start, body_col, NULL);
        fs_newline_indent(s, body_col);
        fmt_form(s, child);
        if (have_src) prev_end = child->span.off_end;
    }
    /* fmt-drops-comments-in-handle-and-binding-modifier-gaps: a comment
     * after the LAST body form, before the closing paren
     * (`(println @rm)))  ; deref` with the `)` on the next line), sits in
     * no inter-form gap and used to be dropped.  Re-emit it; the closing
     * paren then lands on its own line, which is where the source had it. */
    if (have_src) emit_trailing_gap_comments(s, prev_end, f, body_col);
}

/* Offset one past a collection's opening delimiter (`[`, `#{`, `#map{`, `#s(`,
 * `#fx{`, ...), so a comment sitting between the delimiter and the first
 * element is inside the scanned gap.  Returns off_start when there is no
 * source text or no delimiter is found, which makes the gap empty. */
static uint32_t collection_body_start(FmtState *s, const Form *f) {
    const char *src = s->opts.src;
    if (!src) return 0;
    uint32_t a = f->span.off_start;
    uint32_t b = f->span.off_end;
    if (b > (uint32_t)s->opts.src_len) b = (uint32_t)s->opts.src_len;
    for (uint32_t i = a; i < b; i++) {
        if (src[i] == '[' || src[i] == '{' || src[i] == '(') return i + 1;
    }
    return a;
}

/* Re-emit any source comments sitting in src[prev_end, to).
 *
 * This is what keeps comments that live *inside* a bracket vector, map, set,
 * call, or cond arm from being dropped: the parsed AST has no comment nodes,
 * so the only record of them is the source gap between two element spans.
 *
 * A ';' comment that sat on the *same source line* as the preceding element is
 * re-emitted there rather than on a line of its own.  That placement is not
 * cosmetic: a trailing `; first field` pushed down a line lands above the
 * *next* field and now reads as a comment about that one instead, which is a
 * different claim about the code than the author made.  Comments that had
 * their own source line, and block comments, keep the own-line layout.
 *
 * Returns whether the caller MUST start a new line before whatever it emits
 * next -- which is not the same as "did anything get emitted".  A ';' or block
 * comment runs to end of line, so an element appended after one is swallowed
 * by it; a single-line `#;datum` is self-delimiting and stays inline, leaving
 * the caller free to write its normal separating space. */
static bool emit_gap_comments_range(FmtState *s, uint32_t prev_end,
                                    uint32_t to, uint32_t col) {
    if (!s->opts.src) return false;
    uint32_t len = (uint32_t)s->opts.src_len;
    if (to > len) to = len;
    bool must_break = false;
    emit_comments_indented(s, prev_end, to, col, &must_break);
    return must_break;
}

static bool emit_gap_comments_before(FmtState *s, uint32_t prev_end,
                                     const Form *child, uint32_t col) {
    return emit_gap_comments_range(s, prev_end, child->span.off_start, col);
}

/* Re-emit comments sitting between the last element and the closing delimiter.
 * The scan stops at the first non-comment character, so passing the form's end
 * offset is safe -- the ']' / '}' / ')' terminates it.
 *
 * The line break afterwards is mandatory, not cosmetic: output ends inside a
 * ';' comment, so a closing bracket written straight after it would be *inside
 * the comment* in the reformatted file -- turning silent comment loss into a
 * syntax error. */
static void emit_trailing_gap_comments(FmtState *s, uint32_t prev_end,
                                       const Form *f, uint32_t col) {
    if (emit_gap_comments_range(s, prev_end, f->span.off_end, col)) {
        fs_newline_indent(s, col);
    }
}

/* ---------------------------------------------------------------------------
 * Special-form layouts
 * ---------------------------------------------------------------------------
 */

/* A parameter-vector element that annotates the *preceding* parameter and so
 * must stay on the same line as it (rather than being pushed to its own line
 * by the generic vector breaker).  Covers spaced `: T` (F_TYPE_ANN), fused
 * `:T` (F_KEYWORD), contract types `{v : T | p}`, and complex type forms
 * written as a list/vector -- matching how elaboration reads param annotations
 * (see elab_fns.c). */
static bool param_is_annotation(const Form *f) {
    switch (f->tag) {
        case F_TYPE_ANN:
        case F_KEYWORD:
        case F_CONTRACT_TYPE:
        case F_LIST:
        case F_VEC:
            return true;
        default:
            return false;
    }
}

/* A leading modifier that prefixes the *following* parameter name and so keeps
 * that name on the same line: `&` (rest) or any `^...` metadata symbol
 * (^fat, ^mut, ^linear, ...). */
static bool param_is_leading_modifier(const Form *f) {
    if (f->tag != F_SYM) return false;
    if (f->as.sym->len >= 1 && f->as.sym->name[0] == '^') return true;
    if (f->as.sym->len == 1 && f->as.sym->name[0] == '&') return true;
    return false;
}

/* [name : T\n name2 : T\n ^fat name3] -- one parameter (with its annotation)
 * per line.  Unlike fmt_vec_broken this never splits a `name`/`: type` pair
 * across lines, honoring the CLAUDE.md rule against splitting name/type/value
 * triples. */
static void fmt_vec_params_broken(FmtState *s, const Form *f) {
    uint32_t inner = s->col + 1; /* one past '[' */
    uint32_t n = f->as.list.len;
    uint32_t prev_end = collection_body_start(s, f);
    fs_putc(s, '[');
    for (uint32_t i = 0; i < n; i++) {
        const Form *cur = f->as.list.items[i];
        bool had_comment = emit_gap_comments_before(s, prev_end, cur, inner);
        if (had_comment) {
            /* A comment ran to end of line; the parameter starts a fresh one. */
            fs_newline_indent(s, inner);
        } else if (i == 0) {
            /* first element sits right after '[' */
        } else if (param_is_annotation(cur)
                   || param_is_leading_modifier(f->as.list.items[i - 1])) {
            fs_putc(s, ' ');
        } else {
            fs_newline_indent(s, inner);
        }
        fmt_form(s, cur);
        prev_end = cur->span.off_end;
    }
    emit_trailing_gap_comments(s, prev_end, f, inner);
    fs_putc(s, ']');
}

/* Format a defn/fn parameter vector: inline if it fits, else one parameter
 * per line via fmt_vec_params_broken.  The measure is comment-aware, so a
 * vector carrying an interior comment always takes the broken path (which
 * re-emits it) rather than being flattened, which would delete it. */
static void fmt_param_vec(FmtState *s, const Form *vec) {
    uint32_t w = fmt_measure_src(s, vec);
    if (w != UINT32_MAX && s->col + w <= s->opts.line_width) {
        fmt_emit_inline(s, vec);
    } else {
        fmt_vec_params_broken(s, vec);
    }
}

/* Index of the parameter vector in a defn/defmacro form.
 *
 * A defn may carry a type-parameter vector, and optionally a class-constraint
 * vector, ahead of its parameters:
 *
 *   (defn name [params] :ret body)                          -> 2
 *   (defn name [A B] [params] :ret body)                    -> 3
 *   (defn name [A] [(Class A) ...] [params] :ret body)       -> 4
 *
 * This mirrors the elaborator's detection in elab_fns.c
 * (`elab_defn`, the name_idx + 1 / + 2 / + 3 checks) *exactly*, and must keep
 * doing so. The two-consecutive-vectors test is genuinely ambiguous in the
 * language -- `(defn f [x] [1 2 3])` reads as a type-parameterized defn, not
 * as one returning a vector literal -- so a formatter that disambiguated it
 * differently would silently reprint code as something that means something
 * else. Agreeing with the elaborator is the only safe rule, whatever one
 * thinks of the ambiguity itself.
 *
 * Getting this wrong is what kept `stdlib/rcvec.tur` un-self-formatted: with
 * params assumed at index 2, the real parameter vector and the return
 * annotation of a `(defn f [A] [params] : ret ...)` fell past the header and
 * were laid out as body forms, one per line. */
static uint32_t defn_params_index(const Form *f) {
    uint32_t n = f->as.list.len;
    Form *const *it = f->as.list.items;

    if (!(n > 3 && it[2]->tag == F_VEC && it[3]->tag == F_VEC))
        return 2;                     /* no type-parameter vector */

    /* items[2] is the type-parameter vector; params are at 3 unless a
     * constraint vector sits between them. A constraint vector is non-empty
     * and every element is `(ClassName TyVar ...)`. */
    if (n > 4 && it[4]->tag == F_VEC) {
        const Form *maybe = it[3];
        bool looks_like_constraints = (maybe->as.list.len > 0);
        for (uint32_t i = 0; looks_like_constraints && i < maybe->as.list.len; i++) {
            const Form *cf = maybe->as.list.items[i];
            if (cf->tag != F_LIST || cf->as.list.len < 1 ||
                cf->as.list.items[0]->tag != F_SYM)
                looks_like_constraints = false;
        }
        if (looks_like_constraints) return 4;
    }
    return 3;
}

/* fmt-drops-comments-in-handle-and-binding-modifier-gaps: emit header items
 * [0, end) of a special form on the opening line, re-emitting any source
 * comment sitting in the gap between two of them instead of writing a bare
 * ' ' over it (the `handle` scrutinee / first-arm gap, a `;;` between a defn
 * name and its params, ...).  A gap that carried a comment ends inside that
 * comment, so the next item starts a fresh line at body_col -- the same
 * shape fmt_call's head/first-arg pair uses.  `params_idx` (or UINT32_MAX)
 * names the parameter vector, printed through fmt_param_vec. */
static void fmt_header_items(FmtState *s, const Form *f, uint32_t end,
                             uint32_t body_col, uint32_t params_idx) {
    uint32_t n = f->as.list.len;
    uint32_t prev_end = 0;
    for (uint32_t i = 0; i < end && i < n; i++) {
        const Form *cur = f->as.list.items[i];
        if (i) {
            if (emit_gap_comments_before(s, prev_end, cur, body_col))
                fs_newline_indent(s, body_col);
            else
                fs_putc(s, ' ');
        }
        if (i == params_idx && cur->tag == F_VEC) fmt_param_vec(s, cur);
        else                                       fmt_form(s, cur);
        prev_end = cur->span.off_end;
    }
}

/* (defn name [params] :ret\n  body...) */
static void fmt_defn(FmtState *s, const Form *f) {
    uint32_t n = f->as.list.len;
    uint32_t paren_col = s->col;
    uint32_t body_col  = paren_col + s->opts.indent_width;

    fs_putc(s, '(');

    /* Header items: defn/defmacro, name, any type-parameter and constraint
     * vectors, params, optional :ret keyword or type annotation */
    uint32_t params_idx = defn_params_index(f);
    uint32_t header_end = params_idx + 1;
    if (n > header_end && (f->as.list.items[header_end]->tag == F_KEYWORD
                        || f->as.list.items[header_end]->tag == F_TYPE_ANN))
        header_end++;

    fmt_header_items(s, f, header_end, body_col, params_idx);

    fmt_body_forms(s, f, header_end, body_col);

    fs_putc(s, ')');
}

/* (fn [params] [:ret]\n  body...) */
static void fmt_fn(FmtState *s, const Form *f) {
    uint32_t n = f->as.list.len;
    uint32_t paren_col = s->col;
    uint32_t body_col  = paren_col + s->opts.indent_width;

    fs_putc(s, '(');

    /* Header: fn, params, optional :ret keyword or type annotation */
    uint32_t header_end = 2;
    if (n > 2 && (f->as.list.items[2]->tag == F_KEYWORD
               || f->as.list.items[2]->tag == F_TYPE_ANN)) header_end = 3;

    fmt_header_items(s, f, header_end, body_col, 1);

    fmt_body_forms(s, f, header_end, body_col);

    fs_putc(s, ')');
}

/* [name1 val1\n name2 val2\n ...]
 * Pair-per-line layout for let/loop binding vectors. Names are naturally
 * aligned at the inner column; values are aligned at a common column based
 * on the widest name (so consecutive `name val` pairs read like a table).
 * If any value is itself multi-line, it wraps starting at that column. */
static void fmt_vec_let_bindings_broken(FmtState *s, const Form *f) {
    uint32_t inner = s->col + 1; /* one past '[' */
    uint32_t n = f->as.list.len;

    /* Compute the widest flat name across all pairs so values align in a
     * single column. Single-line names are common; if a name itself spans
     * lines (unusual), skip it from the width calculation -- the value on
     * that row will just sit one space after the name's final column. */
    /* A leading modifier (`^mut y 0`, `^fat f ...`) prefixes the NAME slot:
     * it and the name print together (`^mut y`) and take one pair position,
     * exactly as fmt_vec_params_broken treats it.  Consuming it as a pair
     * element desynchronised the walk (`^mut`=name, `y`=value, `0`=name),
     * which split the binding from its value and made the layout
     * non-idempotent. */
    uint32_t max_name = 0;
    for (uint32_t i = 0; i < n; i += 2) {
        uint32_t w = 0;
        if (param_is_leading_modifier(f->as.list.items[i]) && i + 1 < n) {
            uint32_t mw = fmt_measure(f->as.list.items[i]);
            if (mw == UINT32_MAX) continue;
            w = mw + 1;
            i++;
        }
        if (i >= n) break;
        uint32_t nw = fmt_measure(f->as.list.items[i]);
        if (nw == UINT32_MAX) continue;
        w += nw;
        if (w > max_name) max_name = w;
    }
    uint32_t value_col = inner + max_name + 1;

    uint32_t prev_end = collection_body_start(s, f);
    fs_putc(s, '[');
    uint32_t i = 0;
    while (i < n) {
        const Form *name = f->as.list.items[i];
        if (emit_gap_comments_before(s, prev_end, name, inner) || i) {
            fs_newline_indent(s, inner);
        }
        if (param_is_leading_modifier(name) && i + 1 < n) {
            fmt_form(s, name); i++;
            prev_end = name->span.off_end;
            name = f->as.list.items[i];
            if (emit_gap_comments_before(s, prev_end, name, inner))
                fs_newline_indent(s, inner);
            else
                fs_putc(s, ' ');
        }
        fmt_form(s, name); i++;
        prev_end = name->span.off_end;
        if (i < n) {
            const Form *val = f->as.list.items[i];
            /* A comment between a binding's name and its value cannot stay in
             * place without splitting the pair across lines, which the house
             * style forbids.  Emit it above the pair's value column instead of
             * dropping it -- the pair itself stays intact below. */
            if (emit_gap_comments_before(s, prev_end, val, inner)) {
                fs_newline_indent(s, inner);
                for (uint32_t k = 0; k < max_name + 1; k++) fs_putc(s, ' ');
            } else {
                /* Pad to the shared value column; at least one space. */
                uint32_t pad = (value_col > s->col) ? (value_col - s->col) : 1;
                for (uint32_t k = 0; k < pad; k++) fs_putc(s, ' ');
            }
            fmt_form(s, val); i++;
            prev_end = val->span.off_end;
        }
    }
    emit_trailing_gap_comments(s, prev_end, f, inner);
    fs_putc(s, ']');
}

static uint32_t prev_end_head(const Form *f) {
    return f->as.list.len ? f->as.list.items[0]->span.off_end : 0;
}

/* (let [bindings]\n  body...) — also used for loop */
static void fmt_let(FmtState *s, const Form *f) {
    uint32_t n = f->as.list.len;
    uint32_t paren_col = s->col;
    uint32_t body_col  = paren_col + s->opts.indent_width;

    fs_putc(s, '(');

    /* Head: 'let' / 'loop' */
    if (n >= 1) fmt_form(s, f->as.list.items[0]);

    /* Bindings vector: a single pair (or empty vector) may sit inline when it
     * fits; a vector with two or more pairs is always broken pair-per-line, per
     * the house style ("keep each binding pair on its own line").  Overflow of a
     * single pair also falls to the pair-per-line formatter rather than letting
     * the generic vector formatter split every element onto its own line. */
    if (n >= 2) {
        const Form *bindings = f->as.list.items[1];
        /* A comment between the head and the bindings (a `loop` whose first
         * body form follows a `; note`, examples/guestbook) used to be
         * dropped by the bare ' ' here. */
        if (emit_gap_comments_before(s, prev_end_head(f), bindings, body_col))
            fs_newline_indent(s, body_col);
        else
            fs_putc(s, ' ');
        if (bindings->tag == F_VEC) {
            uint32_t w = fmt_measure_src(s, bindings);
            bool single_pair = bindings->as.list.len <= 2;
            if (single_pair && w != UINT32_MAX &&
                s->col + w <= s->opts.line_width) {
                fmt_emit_inline(s, bindings);
            } else {
                fmt_vec_let_bindings_broken(s, bindings);
            }
        } else {
            fmt_form(s, bindings);
        }
    }

    fmt_body_forms(s, f, 2, body_col);

    fs_putc(s, ')');
}

/* (if test\n  then\n  else) */
static void fmt_if(FmtState *s, const Form *f) {
    (void)f->as.list.len;
    uint32_t paren_col = s->col;
    uint32_t body_col  = paren_col + s->opts.indent_width;

    fs_putc(s, '(');

    /* "if" and test on the same first line */
    fmt_header_items(s, f, 2, body_col, UINT32_MAX);

    /* then and optional else on separate indented lines */
    fmt_body_forms(s, f, 2, body_col);

    fs_putc(s, ')');
}

/* (when test\n  body...) — also for unless */
static void fmt_when(FmtState *s, const Form *f) {
    (void)f->as.list.len;
    uint32_t paren_col = s->col;
    uint32_t body_col  = paren_col + s->opts.indent_width;

    fs_putc(s, '(');

    fmt_header_items(s, f, 2, body_col, UINT32_MAX);

    fmt_body_forms(s, f, 2, body_col);

    fs_putc(s, ')');
}

/* (do\n  form...) */
static void fmt_do(FmtState *s, const Form *f) {
    uint32_t n = f->as.list.len;
    uint32_t paren_col = s->col;
    uint32_t body_col  = paren_col + s->opts.indent_width;

    fs_putc(s, '(');

    if (n >= 1) fmt_form(s, f->as.list.items[0]); /* "do" */

    fmt_body_forms(s, f, 1, body_col);

    fs_putc(s, ')');
}

/* (case expr\n  pat result\n  pat result...) */
static void fmt_case(FmtState *s, const Form *f) {
    uint32_t n = f->as.list.len;
    uint32_t paren_col = s->col;
    uint32_t body_col  = paren_col + s->opts.indent_width;

    fs_putc(s, '(');

    /* "case" + expr on first line */
    fmt_header_items(s, f, 2, body_col, UINT32_MAX);

    /* Arms: each pat+result pair on its own line.  A comment before an arm
     * (an arm's trailing `;; note`, or one between the scrutinee and the
     * first arm) is re-emitted from the gap, as fmt_cond does; a comment
     * between a pattern and its result goes above the result rather than
     * being dropped. */
    uint32_t prev_end = (n >= 2) ? f->as.list.items[1]->span.off_end
                                 : (n >= 1 ? f->as.list.items[0]->span.off_end : 0);
    uint32_t i = 2;
    while (i < n) {
        const Form *pat = f->as.list.items[i];
        emit_gap_comments_before(s, prev_end, pat, body_col);
        fs_newline_indent(s, body_col);
        fmt_form(s, pat);
        i++;
        prev_end = pat->span.off_end;
        if (i < n) {
            const Form *res = f->as.list.items[i];
            if (emit_gap_comments_before(s, prev_end, res, body_col))
                fs_newline_indent(s, body_col + s->opts.indent_width);
            else
                fs_putc(s, ' ');
            fmt_form(s, res);
            i++;
            prev_end = res->span.off_end;
        }
    }
    emit_trailing_gap_comments(s, prev_end, f, body_col);

    fs_putc(s, ')');
}

/* (cond test1 body1\n      test2 body2\n      :else  bodyN) — clause table.
 * The head `cond` and the first test share the opening line; every subsequent
 * test aligns under the first test's column, and all bodies align at a common
 * column one space past the widest single-line test, so the test/body pairs
 * read like a table (the documented Clojure-style cond layout).  A body that is
 * itself multi-line wraps from the shared body column.  The whole-form-fits
 * inline case is handled by fmt_list before this runs. */
static void fmt_cond(FmtState *s, const Form *f) {
    uint32_t n = f->as.list.len;

    fs_putc(s, '(');
    if (n >= 1) fmt_form(s, f->as.list.items[0]);   /* 'cond' */

    /* The first test sits one space after the head; later tests align there. */
    uint32_t test_col = s->col + 1;

    /* Widest single-line test, so every body shares one column.  A test that is
     * itself multi-line (rare) is skipped from the width like fmt_let does. */
    uint32_t max_test = 0;
    for (uint32_t i = 1; i + 1 < n; i += 2) {
        uint32_t w = fmt_measure(f->as.list.items[i]);
        if (w != UINT32_MAX && w > max_test) max_test = w;
    }
    uint32_t value_col = test_col + max_test + 1;

    /* A `cond` arm's trailing comment (`(= x 1) 1  ;; the base case`) sits in
     * the gap before the NEXT arm's test, so it is re-emitted from there --
     * same shape as the body-form and vector-element gaps. */
    uint32_t prev_end = (n >= 1) ? f->as.list.items[0]->span.off_end : 0;
    uint32_t i = 1;
    while (i < n) {
        const Form *test = f->as.list.items[i];
        bool had_comment = emit_gap_comments_before(s, prev_end, test, test_col);
        if (i == 1 && !had_comment) fs_putc(s, ' ');
        else                        fs_newline_indent(s, test_col);
        fmt_form(s, test); i++;
        prev_end = test->span.off_end;
        if (i < n) {
            const Form *body = f->as.list.items[i];
            /* A comment between a test and its body would orphan the body; put
             * it above the arm instead of dropping it. */
            if (emit_gap_comments_before(s, prev_end, body, test_col)) {
                fs_newline_indent(s, value_col);
            } else {
                uint32_t pad = (value_col > s->col) ? (value_col - s->col) : 1;
                for (uint32_t k = 0; k < pad; k++) fs_putc(s, ' ');
            }
            fmt_form(s, body); i++;
            prev_end = body->span.off_end;
        }
    }
    emit_trailing_gap_comments(s, prev_end, f, test_col);

    fs_putc(s, ')');
}

/* (handle expr\n  arm...) */
static void fmt_handle(FmtState *s, const Form *f) {
    (void)f->as.list.len;
    uint32_t paren_col = s->col;
    uint32_t body_col  = paren_col + s->opts.indent_width;

    fs_putc(s, '(');

    fmt_header_items(s, f, 2, body_col, UINT32_MAX);

    fmt_body_forms(s, f, 2, body_col);

    fs_putc(s, ')');
}

/* (defclass Name [params]\n  method...) */
static void fmt_defclass(FmtState *s, const Form *f) {
    (void)f->as.list.len;
    uint32_t paren_col = s->col;
    uint32_t body_col  = paren_col + s->opts.indent_width;

    fs_putc(s, '(');

    fmt_header_items(s, f, 3, body_col, UINT32_MAX);

    fmt_body_forms(s, f, 3, body_col);

    fs_putc(s, ')');
}

/* (definstance Class [Type]\n  impl...) — the header is the 3 items
 * `definstance`, the class name, and the `[Type]` vector; every method
 * implementation goes on its own indented body line (2-space body indent,
 * matching the documented special-form style). */
static void fmt_definstance(FmtState *s, const Form *f) {
    (void)f->as.list.len;
    uint32_t paren_col = s->col;
    uint32_t body_col  = paren_col + s->opts.indent_width;

    fs_putc(s, '(');

    fmt_header_items(s, f, 3, body_col, UINT32_MAX);

    fmt_body_forms(s, f, 3, body_col);

    fs_putc(s, ')');
}

/* Generic function call:
 *   (f a b c)          — if inline fits
 *   (f a               — if not: head+first-arg on same line,
 *     b                   remaining args indented by indent_width
 *     c)
 */
static void fmt_call(FmtState *s, const Form *f) {
    uint32_t n = f->as.list.len;
    uint32_t paren_col = s->col;
    uint32_t body_col  = paren_col + s->opts.indent_width;

    fs_putc(s, '(');

    /* Head + first arg on the opening line.  fmt_body_forms covers the gaps
     * between the *remaining* args, but not the one between the head and the
     * first arg -- which is where a `#;` datum comment on the first argument
     * lives, and where it used to be deleted. */
    uint32_t on_first_line = (n > 2) ? 2 : n;
    uint32_t prev_end = 0;
    for (uint32_t i = 0; i < on_first_line; i++) {
        const Form *cur = f->as.list.items[i];
        if (i) {
            if (emit_gap_comments_before(s, prev_end, cur, body_col)) {
                fs_newline_indent(s, body_col);
            } else {
                fs_putc(s, ' ');
            }
        }
        fmt_form(s, cur);
        prev_end = cur->span.off_end;
    }

    /* Remaining args on indented lines */
    fmt_body_forms(s, f, on_first_line, body_col);

    fs_putc(s, ')');
}

/* ---------------------------------------------------------------------------
 * Collection layouts (vector, map, set)
 * ---------------------------------------------------------------------------
 */

/* [a\n b\n c] — 1-space indent from '['
 *
 * A spaced `: T` annotation (F_TYPE_ANN) stays on the line of the element it
 * annotates.  This is the same rule fmt_vec_params_broken applies to parameter
 * vectors, and it is what a `defstruct` field vector needs: without it a field
 * list that overruns the line width is reprinted with every name and every
 * type on its own line, which splits the `name : type` pair the house style
 * says to keep together. */
static void fmt_vec_broken(FmtState *s, const Form *f) {
    uint32_t inner = s->col + 1; /* one past '[' */
    uint32_t n = f->as.list.len;
    uint32_t prev_end = collection_body_start(s, f);
    fs_putc(s, '[');
    for (uint32_t i = 0; i < n; i++) {
        const Form *cur = f->as.list.items[i];
        bool had_comment = emit_gap_comments_before(s, prev_end, cur, inner);
        if (had_comment) {
            fs_newline_indent(s, inner);
        } else if (i == 0) {
            /* first element sits right after '[' */
        } else if (cur->tag == F_TYPE_ANN) {
            fs_putc(s, ' ');
        } else {
            fs_newline_indent(s, inner);
        }
        fmt_form(s, cur);
        prev_end = cur->span.off_end;
    }
    emit_trailing_gap_comments(s, prev_end, f, inner);
    fs_putc(s, ']');
}

/* #{k v\n  k v} */
static void fmt_map_broken(FmtState *s, const Form *f) {
    /* `#map{...}` (F_MAP_LITERAL) shares this layout -- it is the canonical
     * manifest spelling, and a `:spices #map{...}` with several deps is exactly
     * the case that overruns the line width. */
    const char *open = (f->tag == F_MAP_LITERAL) ? "#map{" : fx_map_open(f);
    uint32_t inner = s->col + (uint32_t)strlen(open); /* past the '#{'/'#fx{' */
    uint32_t n = f->as.list.len;
    uint32_t prev_end = collection_body_start(s, f);
    fs_puts(s, open);
    uint32_t i = 0;
    while (i < n) {
        const Form *key = f->as.list.items[i];
        if (emit_gap_comments_before(s, prev_end, key, inner) || i) {
            fs_newline_indent(s, inner);
        }
        fmt_form(s, key); i++;
        prev_end = key->span.off_end;
        if (i < n) {
            const Form *val = f->as.list.items[i];
            /* A comment between a key and its value goes above the pair rather
             * than between them, which would leave the value orphaned. */
            if (emit_gap_comments_before(s, prev_end, val, inner)) {
                fs_newline_indent(s, inner);
            } else {
                fs_putc(s, ' ');
            }
            fmt_form(s, val); i++;
            prev_end = val->span.off_end;
        }
    }
    emit_trailing_gap_comments(s, prev_end, f, inner);
    fs_putc(s, '}');
}

/* #s(a\n   b) */
static void fmt_set_broken(FmtState *s, const Form *f) {
    uint32_t inner = s->col + 3; /* three past '#s(' */
    uint32_t prev_end = collection_body_start(s, f);
    fs_puts(s, "#s(");
    for (uint32_t i = 0; i < f->as.list.len; i++) {
        const Form *cur = f->as.list.items[i];
        if (emit_gap_comments_before(s, prev_end, cur, inner) || i) {
            fs_newline_indent(s, inner);
        }
        fmt_form(s, cur);
        prev_end = cur->span.off_end;
    }
    emit_trailing_gap_comments(s, prev_end, f, inner);
    fs_putc(s, ')');
}

/* Emit a non-empty F_MAP in block style:
 *   #{
 *     k v           ← at entry_col
 *     k v
 *   }               ← closing at close_col
 * Used by fmt_defpackage for :spices / :cmake-deps values. */
static void fmt_map_block(FmtState *s, const Form *f,
                           uint32_t entry_col, uint32_t close_col) {
    uint32_t n = f->as.list.len;
    fs_puts(s, "#{");
    uint32_t prev_end = collection_body_start(s, f);
    uint32_t i = 0;
    while (i < n) {
        const Form *key = f->as.list.items[i];
        emit_gap_comments_before(s, prev_end, key, entry_col);
        fs_newline_indent(s, entry_col);
        fmt_form(s, key); i++;
        prev_end = key->span.off_end;
        if (i < n) {
            const Form *val = f->as.list.items[i];
            if (emit_gap_comments_before(s, prev_end, val, entry_col))
                fs_newline_indent(s, entry_col + s->opts.indent_width);
            else
                fs_putc(s, ' ');
            fmt_form(s, val); i++;
            prev_end = val->span.off_end;
        }
    }
    emit_gap_comments_range(s, prev_end, f->span.off_end, entry_col);
    fs_newline_indent(s, close_col);
    fs_putc(s, '}');
}

/* (defpackage name        also handles (deflockfile ...)
 *   :key1 val1
 *   :spices #{
 *     "name" #{...}
 *   }) */
static void fmt_defpackage(FmtState *s, const Form *f) {
    uint32_t n = f->as.list.len;
    uint32_t paren_col = s->col;
    uint32_t body_col  = paren_col + s->opts.indent_width;

    fs_putc(s, '(');

    /* head (defpackage/deflockfile) + package name */
    fmt_header_items(s, f, 2, body_col, UINT32_MAX);

    /* keyword-value pairs starting at index 2, re-emitting any comment in
     * the gaps (a `;; why this dep` above a :spices entry). */
    uint32_t prev_end = (n >= 2) ? f->as.list.items[1]->span.off_end
                                 : (n >= 1 ? f->as.list.items[0]->span.off_end : 0);
    uint32_t i = 2;
    while (i < n) {
        const Form *kw = f->as.list.items[i];
        emit_gap_comments_before(s, prev_end, kw, body_col);
        fs_newline_indent(s, body_col);
        fmt_form(s, kw); /* keyword */
        i++;
        prev_end = kw->span.off_end;
        if (i < n) {
            const Form *val = f->as.list.items[i];
            if (emit_gap_comments_before(s, prev_end, val, body_col))
                fs_newline_indent(s, body_col + s->opts.indent_width);
            else
                fs_putc(s, ' ');
            /* Non-empty map value: try inline, fall back to block */
            if (val->tag == F_MAP && val->as.list.len > 0) {
                uint32_t w = fmt_measure_src(s, val);
                if (w != UINT32_MAX && s->col + w <= s->opts.line_width) {
                    fmt_emit_inline(s, val);
                } else {
                    fmt_map_block(s, val,
                                  body_col + s->opts.indent_width,
                                  body_col);
                }
            } else {
                fmt_form(s, val);
            }
            i++;
            prev_end = val->span.off_end;
        }
    }
    emit_trailing_gap_comments(s, prev_end, f, body_col);

    fs_putc(s, ')');
}

/* ---------------------------------------------------------------------------
 * Main list dispatcher
 * ---------------------------------------------------------------------------
 */

static void fmt_list(FmtState *s, const Form *f) {
    /* Always try inline first -- but never collapse a form whose source span
     * contains a comment, since the flat printer has no way to re-emit it and
     * the comment would be silently dropped.  fmt_measure_src reports such a
     * form as unmeasurable, which is what makes the check below decline. */
    uint32_t w = fmt_measure_src(s, f);
    if (w != UINT32_MAX && s->col + w <= s->opts.line_width) {
        fmt_emit_inline(s, f);
        return;
    }

    switch (classify_list(f)) {
        case SF_DEFN:
        case SF_DEFMACRO:    fmt_defn(s, f);      break;
        case SF_FN:          fmt_fn(s, f);         break;
        case SF_LET:
        case SF_LOOP:        fmt_let(s, f);        break;
        case SF_IF:          fmt_if(s, f);         break;
        case SF_WHEN:
        case SF_UNLESS:      fmt_when(s, f);       break;
        case SF_DO:          fmt_do(s, f);         break;
        case SF_CASE:        fmt_case(s, f);       break;
        case SF_COND:        fmt_cond(s, f);       break;
        case SF_HANDLE:      fmt_handle(s, f);     break;
        case SF_DEFPACKAGE:  fmt_defpackage(s, f);  break;
        case SF_DEFCLASS:    fmt_defclass(s, f);   break;
        case SF_DEFINSTANCE: fmt_definstance(s, f);break;
        /* defeffect is usually short; if it doesn't fit, use generic layout */
        case SF_DEFEFFECT:
        default:             fmt_call(s, f);       break;
    }
}

/* ---------------------------------------------------------------------------
 * Main form dispatcher
 * ---------------------------------------------------------------------------
 */

static void fmt_form(FmtState *s, const Form *f) {
    switch (f->tag) {
        case F_NIL:   fs_puts(s, "nil");                               break;
        case F_BOOL:  fs_puts(s, f->as.b ? "true" : "false");         break;
        case F_INT:
        case F_FLOAT: {
            Buf nb; buf_init(&nb);
            fmt_num_literal(&nb, f);
            fs_write(s, nb.data, nb.len);
            buf_free(&nb);
            break;
        }
        case F_STR:   fmt_emit_inline(s, f);                           break;
        case F_SYM:
            fs_write(s, f->as.sym->name, f->as.sym->len);
            break;
        case F_KEYWORD:
            fs_putc(s, ':');
            fs_write(s, f->as.sym->name, f->as.sym->len);
            break;
        case F_CBLOCK: {
            const char *cp = f->as.cblock.p;
            size_t clen = f->as.cblock.len;
            bool multiline = false;
            for (size_t i = 0; i < clen; i++) {
                if (cp[i] == '\n') { multiline = true; break; }
            }
            if (multiline) {
                /* Put the opening fence on its own line so the first code
                 * line is not glued onto ```c (which a Markdown renderer
                 * would otherwise swallow as the fence info string). The
                 * block's trailing "\n<indent>" already positions the
                 * closing fence. */
                uint32_t cb_indent = s->col;
                fs_puts(s, "```c");
                fs_newline_indent(s, cb_indent);
                fs_write(s, cp, clen);
                fs_puts(s, "```");
            } else {
                fs_puts(s, "```c ");
                fs_write(s, cp, clen);
                fs_puts(s, "```");
            }
            break;
        }
        case F_QUOTE:
            fs_putc(s, '\'');
            if (f->as.list.len > 0) fmt_form(s, f->as.list.items[0]);
            break;
        case F_QUASIQUOTE:
            fs_putc(s, '`');
            if (f->as.list.len > 0) fmt_form(s, f->as.list.items[0]);
            break;
        case F_UNQUOTE:
            fs_putc(s, '~');
            if (f->as.list.len > 0) fmt_form(s, f->as.list.items[0]);
            break;
        case F_UNQUOTE_SPLICING:
            fs_puts(s, "~@");
            if (f->as.list.len > 0) fmt_form(s, f->as.list.items[0]);
            break;
        case F_TYPE_ANN:
            fs_puts(s, ": ");
            if (f->as.list.len > 0) fmt_form(s, f->as.list.items[0]);
            break;
        /* CT0: Contract type { var : T | pred } — format inline */
        case F_CONTRACT_TYPE: {
            uint32_t w = fmt_measure(f);
            if (s->col + w <= s->opts.line_width) fmt_emit_inline(s, f);
            else fmt_emit_inline(s, f); /* always inline for now */
            break;
        }
        /* The `w != UINT32_MAX` guard is load-bearing on all three: `s->col + w`
         * is uint32 arithmetic, so an unmeasurable form (one with an interior
         * newline, or -- since fmt_measure_src -- an interior comment) wraps
         * around to a tiny number at any column > 0 and would be inlined. */
        case F_VEC: {
            uint32_t w = fmt_measure_src(s, f);
            if (w != UINT32_MAX && s->col + w <= s->opts.line_width) fmt_emit_inline(s, f);
            else fmt_vec_broken(s, f);
            break;
        }
        case F_MAP: {
            uint32_t w = fmt_measure_src(s, f);
            if (w != UINT32_MAX && s->col + w <= s->opts.line_width) fmt_emit_inline(s, f);
            else fmt_map_broken(s, f);
            break;
        }
        case F_SET: {
            uint32_t w = fmt_measure_src(s, f);
            if (w != UINT32_MAX && s->col + w <= s->opts.line_width) fmt_emit_inline(s, f);
            else fmt_set_broken(s, f);
            break;
        }
        case F_LIST:
            /* A `#reads`/`#writes` frame has a sugar spelling and no paren
             * spelling (fmt_frame_annot), so it goes out flat rather than
             * through the list breaker, which would print it as a call. */
            if (f->fx_prov == (uint8_t)PROV_READS ||
                f->fx_prov == (uint8_t)PROV_WRITES) {
                fmt_emit_inline(s, f);
                break;
            }
            fmt_list(s, f);
            break;
        case F_READER_COND:
            fmt_emit_inline(s, f);
            break;
        /* RR3: Range literal variable annotation -- format the desugared range form */
        case F_RANGE_VAR:
            if (f->as.list.len > 1) fmt_form(s, f->as.list.items[1]);
            break;
        /* DL0: data literals.  `#map{...}` breaks across lines when it does not
         * fit, exactly as `#{...}` does: it is the canonical spelling for a
         * manifest's :spices / :cmake-deps map, and squashing one of those onto
         * a single 200-column line is not a formatting outcome anyone wants.
         * #set{...} / #row{...} stay inline -- their elements are short and
         * their line-per-element layout has no established shape. */
        case F_MAP_LITERAL: {
            uint32_t w = fmt_measure_src(s, f);
            if (w != UINT32_MAX && s->col + w <= s->opts.line_width) fmt_emit_inline(s, f);
            else fmt_map_broken(s, f);
            break;
        }
        case F_SET_LITERAL:
        case F_ROW_LITERAL:
            fmt_emit_inline(s, f);
            break;
    }
}

/* ---------------------------------------------------------------------------
 * Comment and blank-line extraction from source text
 * ---------------------------------------------------------------------------
 */

static uint32_t count_blank_lines(const char *src, uint32_t from_off,
                                  uint32_t to_off, uint32_t max);

/* Terminate the current line, then emit `blanks` empty lines. */
static void fs_break_and_blank(FmtState *s, uint32_t blanks) {
    if (s->col > 0) fs_putc(s, '\n');
    for (uint32_t i = 0; i < blanks; i++) fs_putc(s, '\n');
}

/* Emit any ';' comments found in src[from_off .. to_off).
 *
 * Comments are placed on their own lines, and the gap's blank-line structure is
 * reproduced around them rather than relocated: blank lines before the first
 * comment and between successive comments are preserved as-is (capped at 2).
 * `min_lead` is a floor on the blanks emitted before the first comment, for
 * callers that want a comment forced away from whatever preceded it; every
 * caller currently passes 0, because forcing a blank in front of a comment is
 * what moved a docstring's separating blank to the wrong side of it.  Blank
 * lines *after* the last comment are the caller's business -- see fmt_print,
 * which keeps a `;;;` docstring flush against the definition it documents.
 *
 * `*out_end`, when non-NULL, receives the offset just past the last comment
 * emitted (or `from_off` when the gap held none).
 *
 * Returns true if anything was emitted. */
static bool emit_comments_in_gap(FmtState *s, uint32_t from_off, uint32_t to_off,
                                 uint32_t min_lead, uint32_t *out_end) {
    const char *src = s->opts.src;
    if (out_end) *out_end = from_off;
    if (!src || from_off >= to_off) return false;

    const char *p   = src + from_off;
    const char *end = src + to_off;
    bool emitted = false;
    uint32_t prev_end = from_off; /* offset just past the last comment emitted */

    /* Break the line and emit the blanks that separated `prev_end` from the
     * comment starting at `cstart`. */
    #define EMIT_GAP_BLANKS(cstart)                                            \
        do {                                                                   \
            uint32_t _n = count_blank_lines(src, prev_end, (cstart), 2);       \
            if (!emitted && _n < min_lead) _n = min_lead;                      \
            fs_break_and_blank(s, _n);                                         \
        } while (0)

    while (p < end) {
        if (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') { p++; continue; }

        /* Line comment */
        if (*p == ';') {
            const char *line_end = p;
            while (line_end < end && *line_end != '\n') line_end++;
            /* A comment that trailed the preceding top-level form on its own
             * line stays there.  Breaking it onto the next line drops it in
             * front of the *following* form, where it reads as a comment about
             * that one -- which is how a last struct field's `; unix timestamp`
             * ends up parked outside the defstruct describing whatever comes
             * next. Only the first comment in the gap can qualify, and only
             * when there is a populated line to trail. */
            bool same_line = !emitted && min_lead == 0 && s->col > 0
                && memchr(src + from_off, '\n',
                          (size_t)(p - (src + from_off))) == NULL;
            if (same_line) fs_putc(s, ' ');
            else           EMIT_GAP_BLANKS((uint32_t)(p - src));
            fs_write(s, p, (size_t)(line_end - p));
            emitted = true;
            p = line_end;
            prev_end = (uint32_t)(p - src);
            continue;
        }

        /* Block comment #| ... |# */
        if (*p == '#' && p + 1 < end && p[1] == '|') {
            const char *blk = p;
            p += 2;
            while (p + 1 < end && !(p[0] == '|' && p[1] == '#')) p++;
            if (p + 1 < end) p += 2;
            EMIT_GAP_BLANKS((uint32_t)(blk - src));
            fs_write(s, blk, (size_t)(p - blk));
            emitted = true;
            prev_end = (uint32_t)(p - src);
            continue;
        }

        /* Datum comment #;datum -- re-emit verbatim */
        if (*p == '#' && p + 1 < end && p[1] == ';') {
            const char *blk = p;
            p += 2;
            /* skip whitespace between #; and the datum */
            while (p < end && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')) p++;
            if (p < end) {
                if (*p == '(' || *p == '[' || *p == '{') {
                    char open = *p;
                    char close = (open == '(') ? ')' : (open == '[') ? ']' : '}';
                    int depth = 0;
                    while (p < end) {
                        if (*p == '"') {
                            p++;
                            while (p < end && *p != '"') {
                                if (*p == '\\' && p + 1 < end) p++;
                                p++;
                            }
                            if (p < end) p++;
                        } else if (*p == open) {
                            depth++; p++;
                        } else if (*p == close) {
                            depth--; p++;
                            if (depth == 0) break;
                        } else {
                            p++;
                        }
                    }
                } else if (*p == '"') {
                    p++;
                    while (p < end && *p != '"') {
                        if (*p == '\\' && p + 1 < end) p++;
                        p++;
                    }
                    if (p < end) p++;
                } else {
                    while (p < end && *p != ' ' && *p != '\t' && *p != '\r'
                           && *p != '\n' && *p != '(' && *p != ')' && *p != '['
                           && *p != ']' && *p != '{' && *p != '}') {
                        p++;
                    }
                }
            }
            EMIT_GAP_BLANKS((uint32_t)(blk - src));
            fs_write(s, blk, (size_t)(p - blk));
            emitted = true;
            prev_end = (uint32_t)(p - src);
            continue;
        }

        /* Anything else is part of a form — stop */
        break;
    }
    #undef EMIT_GAP_BLANKS
    if (out_end) *out_end = prev_end;
    return emitted;
}

/* Emit ';' line / '#| |#' block / '#;' datum comments found in
 * src[from_off .. to_off), indented to `col`.  Used for comments that live
 * *inside* a form (between body sub-forms, or between the elements of a
 * bracket vector / map / set), which the top-level emit_comments_in_gap never
 * sees.  Returns the number emitted.
 *
 * A ';' comment that shared a source line with whatever precedes the gap is
 * emitted as a trailing comment on the current output line; everything else
 * gets a line of its own at `col`.  That distinction is not cosmetic: a
 * trailing `; the sum` pushed onto the next line lands above the *following*
 * form and now reads as a comment about that one, which is a different claim
 * about the code than the author made. */
static uint32_t emit_comments_indented(FmtState *s, uint32_t from_off,
                                       uint32_t to_off, uint32_t col,
                                       bool *must_break) {
    const char *src = s->opts.src;
    if (must_break) *must_break = false;
    if (!src || from_off >= to_off) return 0;

    const char *p   = src + from_off;
    const char *end = src + to_off;
    uint32_t count = 0;
    /* True until the scan crosses a newline: a comment found before then sat on
     * the same source line as the preceding form.  Emitting one mid-line is
     * only possible when there *is* a current line to trail (s->col > 0). */
    bool same_line = (s->col > 0);

    while (p < end) {
        if (*p == '\n') { same_line = false; p++; continue; }
        if (*p == ' ' || *p == '\t' || *p == '\r') { p++; continue; }

        /* Line comment */
        if (*p == ';') {
            const char *line_end = p;
            while (line_end < end && *line_end != '\n') line_end++;
            const char *trim_end = line_end;
            while (trim_end > p && trim_end[-1] == '\r') trim_end--;
            if (same_line) {
                fs_putc(s, ' ');
                same_line = false;
            } else {
                fs_newline_indent(s, col);
            }
            fs_write(s, p, (size_t)(trim_end - p));
            count++;
            if (must_break) *must_break = true;
            p = line_end;
            continue;
        }

        /* Block comment #| ... |# (written verbatim, including any newlines).
         * Always own-line: the body may span lines, so trailing it onto a
         * populated line would reflow the code around it. */
        if (*p == '#' && p + 1 < end && p[1] == '|') {
            const char *blk = p;
            p += 2;
            while (p + 1 < end && !(p[0] == '|' && p[1] == '#')) p++;
            if (p + 1 < end) p += 2;
            fs_newline_indent(s, col);
            same_line = false;
            count++;
            fs_write(s, blk, (size_t)(p - blk));
            continue;
        }

        /* Datum comment #;datum -- re-emit verbatim */
        if (*p == '#' && p + 1 < end && p[1] == ';') {
            const char *blk = p;
            p += 2;
            while (p < end && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')) p++;
            if (p < end) {
                if (*p == '(' || *p == '[' || *p == '{') {
                    char open = *p;
                    char close = (open == '(') ? ')' : (open == '[') ? ']' : '}';
                    int depth = 0;
                    while (p < end) {
                        if (*p == '"') {
                            p++;
                            while (p < end && *p != '"') {
                                if (*p == '\\' && p + 1 < end) p++;
                                p++;
                            }
                            if (p < end) p++;
                        } else if (*p == open) {
                            depth++; p++;
                        } else if (*p == close) {
                            depth--; p++;
                            if (depth == 0) break;
                        } else {
                            p++;
                        }
                    }
                } else if (*p == '"') {
                    p++;
                    while (p < end && *p != '"') {
                        if (*p == '\\' && p + 1 < end) p++;
                        p++;
                    }
                    if (p < end) p++;
                } else {
                    while (p < end && *p != ' ' && *p != '\t' && *p != '\r'
                           && *p != '\n' && *p != '(' && *p != ')' && *p != '['
                           && *p != ']' && *p != '{' && *p != '}') {
                        p++;
                    }
                }
            }
            /* A single-line `#;datum` keeps its place in the argument list;
             * one that spans lines goes on its own, since it is written
             * verbatim and would otherwise carry its newlines into the middle
             * of a line the printer is still tracking a column for. */
            bool datum_multiline = (memchr(blk, '\n', (size_t)(p - blk)) != NULL);
            if (same_line && !datum_multiline) {
                fs_putc(s, ' ');
                if (must_break) *must_break = false;
            } else {
                fs_newline_indent(s, col);
                same_line = false;
                if (must_break) *must_break = true;
            }
            fs_write(s, blk, (size_t)(p - blk));
            count++;
            /* A datum comment is self-delimiting, so unlike a ';' comment the
             * caller does NOT have to break the line after one -- but it must
             * still be told something was emitted, and the shared "did we
             * emit" contract is a break.  Leaving same_line set lets a second
             * datum comment on the same line follow this one inline. */
            continue;
        }

        /* Anything else is part of a form — stop */
        break;
    }
    return count;
}

/* True if f's source span contains a line/block/datum comment (ignoring any
 * ';' or '#' that appears inside a string or an inline-C ```...``` block).
 * Used to keep fmt_list from collapsing such a form onto one line, which would
 * drop the comment. */
static bool span_has_comment(FmtState *s, const Form *f) {
    const char *src = s->opts.src;
    if (!src) return false;
    uint32_t a = f->span.off_start;
    uint32_t b = f->span.off_end;
    if (b > (uint32_t)s->opts.src_len) b = (uint32_t)s->opts.src_len;
    if (a >= b) return false;

    const char *p   = src + a;
    const char *end = src + b;
    while (p < end) {
        char c = *p;
        if (c == '\\' && p + 1 < end) { p += 2; continue; } /* escaped char */
        if (c == '"') {                                     /* string literal */
            p++;
            while (p < end && *p != '"') {
                if (*p == '\\' && p + 1 < end) p++;
                p++;
            }
            if (p < end) p++;
            continue;
        }
        if (c == '`' && p + 2 < end && p[1] == '`' && p[2] == '`') { /* ```...``` */
            p += 3;
            while (p + 2 < end && !(p[0] == '`' && p[1] == '`' && p[2] == '`')) p++;
            if (p + 2 < end) p += 3; else p = end;
            continue;
        }
        if (c == ';') return true;
        if (c == '#' && p + 1 < end && (p[1] == '|' || p[1] == ';')) return true;
        p++;
    }
    return false;
}

/* Count blank lines in src[from_off .. to_off), capped at max. */
static uint32_t count_blank_lines(const char *src, uint32_t from_off,
                                   uint32_t to_off, uint32_t max) {
    if (!src || from_off >= to_off) return 0;
    const char *p = src + from_off;
    const char *end = src + to_off;
    uint32_t blanks = 0;
    bool prev_was_nl = false;
    while (p < end && blanks < max) {
        if (*p == '\n') {
            if (prev_was_nl) blanks++;
            prev_was_nl = true;
        } else if (*p != ' ' && *p != '\t' && *p != '\r') {
            prev_was_nl = false;
        }
        p++;
    }
    return blanks < max ? blanks : max;
}

/* ---------------------------------------------------------------------------
 * Public entry point
 * ---------------------------------------------------------------------------
 */

int fmt_print(Buf *buf, Form **forms, uint32_t count, FmtOptions opts) {
    if (!buf || (!forms && count > 0)) return -1;

    FmtState s = {0};
    s.buf  = buf;
    s.col  = 0;
    s.opts = opts;

    if (s.opts.indent_width == 0) s.opts.indent_width = 2;
    if (s.opts.line_width   == 0) s.opts.line_width   = 80;

    uint32_t prev_end = 0; /* byte offset after the last emitted form */

    for (uint32_t i = 0; i < count; i++) {
        const Form *f = forms[i];

        if (i == 0) {
            /* Emit any leading comments before the first form */
            if (s.opts.src && f->span.off_start > 0) {
                emit_comments_in_gap(&s, 0, f->span.off_start, 0, NULL);
                if (s.col > 0) fs_putc(&s, '\n');
            }
        } else {
            uint32_t gap_start = prev_end;
            uint32_t gap_end   = f->span.off_start;

            /* Emit the gap's comments, reproducing the blank lines that led up
             * to them exactly as the source had them. */
            uint32_t comments_end = gap_start;
            bool had_comment =
                emit_comments_in_gap(&s, gap_start, gap_end, 0, &comments_end);

            /* Determine how many blank lines to insert before the form. */
            uint32_t blanks = 0;
            if (had_comment) {
                /* Preserve the source's blanks between the last comment and the
                 * form -- exactly, with no minimum.  A `;;;` docstring block
                 * must stay flush against the definition it documents: any
                 * intervening blank resets the docstring buffer, so injecting
                 * one here would silently detach every docstring in the file
                 * from its `defn`/`deftype` (tools/gendocs.py and `(doc ...)`
                 * both read the block immediately above the definition). */
                if (s.opts.src) {
                    blanks = count_blank_lines(s.opts.src, comments_end,
                                               gap_end, 2);
                }
            } else {
                if (s.opts.src) {
                    blanks = count_blank_lines(s.opts.src, gap_start, gap_end, 2);
                }
                /* At least one blank line between adjacent top-level forms */
                if (blanks == 0) blanks = 1;
            }

            fs_break_and_blank(&s, blanks);
        }

        fmt_form(&s, f);
        prev_end = f->span.off_end;
    }

    /* Trailing comments after the last form */
    if (s.opts.src && prev_end < (uint32_t)s.opts.src_len) {
        emit_comments_in_gap(&s, prev_end, (uint32_t)s.opts.src_len, 0, NULL);
    }

    /* Ensure exactly one trailing newline */
    if (buf->len == 0 || buf->data[buf->len - 1] != '\n') {
        buf_putc(buf, '\n');
    }

    return 0;
}

/* ---------------------------------------------------------------------------
 * Whole-buffer entry point
 * ---------------------------------------------------------------------------
 */

#include "arena.h"
#include "reader.h"
#include "reader_macros.h"
#include "symbols.h"


/* ===========================================================================
 * r7rs-lang-plan R9: `tur fmt` for `#lang r7rs` -- re-indent, never reprint.
 *
 * The form printer cannot format Scheme faithfully, because the R7RS reader
 * desugars lexemes that have no Form of their own: `#\x` reads as a call to
 * the char constructor, `#u8(...)` as `(bytevector ...)`, `#e1.5e2` as 150,
 * `|two words|` as a symbol whose name has a space, and `#t` as the boolean
 * the printer spells `true`.  Printed back, a Scheme file came out as a
 * different program (measured: every one of those, plus `,` as `~`).
 *
 * So a Scheme file keeps every token exactly as written, and every line
 * break; the formatter recomputes only each line's LEADING whitespace, strips
 * trailing whitespace, and ends the file with one newline -- which is what a
 * Lisp editor's indent-region does.  Lines that begin inside a string, a
 * `|symbol|`, a `#| |#` comment or an inline-C fence are left untouched.
 *
 * The indentation rules, per open bracket (innermost first):
 *   - a line that starts with a closer aligns with its opener;
 *   - quoted data (`'(`, `` `( ``, `#(`, `#u8(`, and everything nested in
 *     one) and a `[...]` binding vector align under the first element;
 *   - a body form -- `define`, `lambda`, the `let` family, `if`, `when`,
 *     `case`, `syntax-rules`, ... and the prelude's Turmeric
 *     `defn`/`fn`/`def...` shapes (CLAUDE.md: special forms take a 2-space
 *     body) -- indents its body two past the opener;
 *   - a call aligns later arguments under its first argument when that
 *     argument shares the head's line, else under the head;
 *   - a list whose head is itself a list (a `let` binding list) aligns under
 *     that head.
 * The pass is idempotent: its output only depends on columns it sets itself.
 * ======================================================================== */

typedef struct {
    int  col;          /* column of the opening bracket */
    int  head_col;     /* column of element 0, -1 before it */
    int  arg_col;      /* column of element 1 when on element 0's line, else -1 */
    int  head_line;    /* line of element 0 */
    int  n_elems;
    bool head_is_sym;
    bool body_form;
    bool data;         /* quoted data or a [...] vector */
    bool data_inherit; /* children inherit `data` (quoted data, not [...]) */
} SchemeFrame;

static bool scheme_delim(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '(' || c == ')' ||
           c == '[' || c == ']' || c == '"' || c == ';' || c == '\0';
}

static bool scheme_body_head(const char *t, size_t n) {
    static const char *const FORMS[] = {
        "lambda", "case-lambda", "let", "let*", "letrec", "letrec*", "let-values",
        "let*-values", "let-syntax", "letrec-syntax", "syntax-rules",
        "when", "unless", "begin", "case", "if", "guard", "parameterize",
        "delay", "delay-force", "make-promise", "with-exception-handler",
        "dynamic-wind", "call-with-port", "call-with-input-file",
        "call-with-output-file", "with-input-from-file", "with-output-to-file",
        "fn", "loop", "while", "for", "match", "handle",
        NULL };
    if (n >= 3 && strncmp(t, "def", 3) == 0) return true;   /* define*, defn, defstruct, ... */
    for (int i = 0; FORMS[i]; i++)
        if (strlen(FORMS[i]) == n && strncmp(FORMS[i], t, n) == 0) return true;
    return false;
}

int fmt_scheme_reindent(const char *src, size_t len, Buf *out) {
    enum { MAXD = 1024 };
    SchemeFrame *st = (SchemeFrame *)calloc(MAXD, sizeof(SchemeFrame));
    if (!st) return -1;
    int depth = 0;
    bool in_str = false, in_bar = false, in_fence = false;
    int block = 0;                     /* #| |# nesting */
    bool pending_prefix = false;       /* a ' ` , ,@ #; waiting for its datum */
    bool prefix_quotes = false;        /* ...and it quotes (' or `) */
    int line_no = 0;
    size_t pos = 0;
    Buf line; buf_init(&line);
    int rc = 0;

    while (pos < len) {
        size_t e = pos;
        while (e < len && src[e] != '\n') e++;
        const char *raw = src + pos;
        size_t rawn = e - pos;
        pos = (e < len) ? e + 1 : e;
        line_no++;

        /* A line that begins inside a string, bar symbol, block comment or
         * inline-C fence is not ours to touch. */
        bool verbatim = in_str || in_bar || block > 0 || in_fence;
        size_t lead = 0;
        while (lead < rawn && (raw[lead] == ' ' || raw[lead] == '\t')) lead++;
        const char *body = raw + lead;
        size_t bodyn = rawn - lead;

        line.len = 0;
        if (verbatim) {
            buf_write(&line, raw, rawn);
        } else {
            while (bodyn > 0 && (body[bodyn - 1] == ' ' || body[bodyn - 1] == '\t' ||
                                 body[bodyn - 1] == '\r')) bodyn--;
            if (bodyn > 0) {
                int ind = 0;
                if (depth > 0) {
                    SchemeFrame *f = &st[depth - 1];
                    if (body[0] == ')' || body[0] == ']') ind = f->col;
                    else if (f->data) ind = f->col + 1;
                    else if (f->head_col < 0) ind = f->col + 1;
                    else if (f->body_form) ind = f->col + 2;
                    else if (f->arg_col >= 0) ind = f->arg_col;
                    else ind = f->head_col;
                }
                /* An inline-C fence opener keeps the author's column for the
                 * fence body's sake only when it is at top level; inside a
                 * form it is an element like any other. */
                for (int k = 0; k < ind; k++) buf_putc(&line, ' ');
                buf_write(&line, body, bodyn);
            }
        }

        /* Scan the line as emitted, to advance the state. */
        const char *L = line.data ? line.data : "";
        size_t n = line.len;
        size_t i = 0;
        if (in_fence) {
            /* The fence closes at a line whose content starts with ```; the
             * rest of that line (`)` usually) is ordinary source. */
            size_t k = 0;
            while (k < n && (L[k] == ' ' || L[k] == '\t')) k++;
            if (k + 3 <= n && strncmp(L + k, "```", 3) == 0) { in_fence = false; i = k + 3; }
            else i = n;
        }
        while (i < n) {
            char c = L[i];
            if (in_str) {
                if (c == '\\') { i += 2; continue; }
                if (c == '"') in_str = false;
                i++; continue;
            }
            if (in_bar) {
                if (c == '\\') { i += 2; continue; }
                if (c == '|') in_bar = false;
                i++; continue;
            }
            if (block > 0) {
                if (c == '|' && i + 1 < n && L[i + 1] == '#') { block--; i += 2; continue; }
                if (c == '#' && i + 1 < n && L[i + 1] == '|') { block++; i += 2; continue; }
                i++; continue;
            }
            if (c == ' ' || c == '\t' || c == '\r') { i++; continue; }
            if (c == ';') break;
            if (c == '#' && i + 1 < n && L[i + 1] == '|') { block++; i += 2; continue; }
            if (c == '`' && i + 2 < n && L[i + 1] == '`' && L[i + 2] == '`') {
                /* inline-C fence: an element of the enclosing form */
                if (depth > 0) {
                    SchemeFrame *f = &st[depth - 1];
                    if (f->n_elems == 1 && f->head_line == line_no && f->arg_col < 0) f->arg_col = (int)i;
                    f->n_elems++;
                }
                in_fence = true;
                pending_prefix = false;
                i = n;   /* the opener line (```c) holds nothing else */
                continue;
            }
            if (c == ')' || c == ']') {
                if (depth > 0) depth--;
                pending_prefix = false;
                i++; continue;
            }
            /* Quote-like prefixes attach to the next datum. */
            bool is_prefix = (c == '\'' || c == '`' || c == ',') ||
                             (c == '#' && i + 1 < n && L[i + 1] == ';');
            int elem_col = (int)i;
            bool counted = false;
            if (depth > 0 && !pending_prefix) {
                SchemeFrame *f = &st[depth - 1];
                if (f->n_elems == 0) { f->head_col = elem_col; f->head_line = line_no; }
                else if (f->n_elems == 1 && f->head_line == line_no) f->arg_col = elem_col;
                f->n_elems++;
                counted = true;
            }
            (void)counted;
            if (is_prefix) {
                if (!pending_prefix) prefix_quotes = false;
                if (c == '\'' || c == '`') prefix_quotes = true;
                if (c == ',') prefix_quotes = false;
                pending_prefix = true;
                if (c == '#') i += 2;
                else if (c == ',' && i + 1 < n && L[i + 1] == '@') i += 2;
                else i++;
                continue;
            }
            bool quoted = pending_prefix && prefix_quotes;
            bool unquoted = pending_prefix && !prefix_quotes;
            pending_prefix = false;
            /* Openers: ( [ #( #u8( */
            size_t open_at = n;
            bool vec_data = false;
            if (c == '(' || c == '[') open_at = i;
            else if (c == '#' && i + 1 < n && L[i + 1] == '(') { open_at = i + 1; vec_data = true; }
            else if (c == '#' && i + 3 < n && L[i + 1] == 'u' && L[i + 2] == '8' && L[i + 3] == '(') { open_at = i + 3; vec_data = true; }
            if (open_at < n) {
                if (depth >= MAXD) { rc = -1; break; }
                SchemeFrame *parent = depth > 0 ? &st[depth - 1] : NULL;
                SchemeFrame *f = &st[depth++];
                memset(f, 0, sizeof *f);
                f->col = (int)open_at;
                f->head_col = -1;
                f->arg_col = -1;
                bool inherited = parent && parent->data_inherit && !unquoted;
                f->data_inherit = quoted || vec_data || inherited;
                f->data = f->data_inherit || L[open_at] == '[';
                /* Peek the head token for the body-form test. */
                size_t h = open_at + 1;
                while (h < n && (L[h] == ' ' || L[h] == '\t')) h++;
                size_t he = h;
                while (he < n && !scheme_delim(L[he])) he++;
                f->head_is_sym = he > h && L[h] != '"' && L[h] != '#' && L[h] != '\'';
                f->body_form = f->head_is_sym && scheme_body_head(L + h, he - h);
                i = open_at + 1;
                continue;
            }
            if (c == '"') { in_str = true; i++; continue; }
            if (c == '|') { in_bar = true; i++; continue; }
            if (c == '#' && i + 1 < n && L[i + 1] == '\\') {
                /* #\x: the character itself may be a delimiter, e.g. #\( */
                i += 3;
                while (i < n && !scheme_delim(L[i])) i++;
                continue;
            }
            /* An ordinary token. */
            while (i < n && !scheme_delim(L[i]) && L[i] != '|') i++;
        }
        if (rc != 0) break;
        /* No leading blank lines (the body usually starts with the newline
         * that ended the `#lang` line, which the caller re-emits). */
        if (out->len == 0 && line.len == 0) continue;
        buf_write(out, line.data ? line.data : "", line.len);
        if (e < len) buf_putc(out, '\n');
    }
    buf_free(&line);
    free(st);
    if (rc != 0) return rc;
    /* Exactly one trailing newline; no trailing blank lines. */
    while (out->len > 0 && (out->data[out->len - 1] == '\n' || out->data[out->len - 1] == ' '))
        out->len--;
    buf_putc(out, '\n');
    return 0;
}

int fmt_format_buffer(const char *path_label, const char *src, size_t len,
                      ReaderType rtype, Buf *out) {
    /* Reset BEFORE registering: diag_reset() clears the file registry, so
     * registering first (as this used to) wiped this file's entry and left
     * any format-time parse-error diagnostic without a source snippet
     * (files_[0] == NULL -> "<unknown>"). */
    diag_reset();

    SourceFile file = {0};
    file.path        = path_label;
    file.src         = src;
    file.len         = len;
    file.file_id     = 0;
    file.reader_type = rtype;
    diag_register_file(&file);

    Arena arena;
    arena_init(&arena, 0);
    SymbolTable st;
    symtab_init(&st, &arena);

    ReaderMacroRegistry rmreg;
    reader_macros_init(&rmreg, &arena);
    rmreg.strict = true;
    /* Keep `(reader-macros/define ...)` directives in the form stream so the
     * formatter emits them -- stripping them (the compiler default) would make
     * `tur fmt` silently delete the definition. */
    rmreg.keep_define_forms = true;

    uint32_t nforms = 0;
    Form **forms = read_all_with_registry(&arena, &st, &file, &rmreg, &nforms);

    int rc = 0;
    if (!forms || diag_had_error()) {
        rc = -1;
    } else if (rtype == READER_R7RS_SWEET || rtype == READER_SWEET) {
        /* r7rs-sweet-base-dialect-missing: the parse above is the syntax
         * check, and the layout IS the syntax, so the text is kept as
         * written -- re-indenting it as Scheme would change its meaning.
         * fmt-reprints-sweet-as-s-expressions: the same holds for
         * turmeric/sweet and saffron/sweet.  Handing their forms to
         * fmt_print reprinted the buffer as s-expressions -- still legal
         * sweet-exp, but a different syntax from the one the file was
         * written in, under a header that still announced sweet.
         * Only the ends are normalized, as fmt_scheme_reindent does: no
         * leading blank lines (the body starts with the newline that ended
         * the `#lang` line, which the caller re-emits) and exactly one
         * trailing newline, so the pass is idempotent. */
        size_t a = 0, z = len;
        for (size_t k = 0; k < len; k++) {
            if (src[k] == '\n') a = k + 1;
            else if (src[k] != ' ' && src[k] != '\t' && src[k] != '\r') break;
        }
        while (z > a && (src[z - 1] == '\n' || src[z - 1] == ' ' ||
                         src[z - 1] == '\t' || src[z - 1] == '\r'))
            z--;
        buf_init(out);
        buf_write(out, src + a, z - a);
        buf_putc(out, '\n');
    } else if (rtype == READER_R7RS) {
        /* r7rs-lang-plan R9: the parse above is the syntax check; the
         * output is the ORIGINAL text re-indented (fmt_scheme_reindent). */
        buf_init(out);
        if (fmt_scheme_reindent(src, len, out) != 0) {
            buf_free(out);
            rc = -1;
        }
    } else {
        FmtOptions opts = {0};
        opts.indent_width = 2;
        opts.line_width   = 80;
        opts.src          = src;
        opts.src_len      = len;
        buf_init(out);
        if (fmt_print(out, forms, nforms, opts) != 0) {
            buf_free(out);
            rc = -1;
        }
    }

    symtab_free(&st);
    arena_free(&arena);
    return rc;
}

/* r7rs-lang-plan R9 (moved from main.c): format a document that may carry a
 * `#lang` directive -- the directive verbatim, the body by its reader.  See
 * fmt.h. */
int fmt_format_document(const char *path_label, const char *src, size_t len,
                              ReaderType rtype, Buf *out) {
    const char *body = src;
    size_t body_len = len;
    LangDialect dl = LANG_TURMERIC;
    ReaderType lang_rt = detect_lang_dialect(src, len, &body, &body_len,
                                             NULL, NULL, &dl);
    size_t head_len = (size_t)(body - src);
    if (head_len == 0) return fmt_format_buffer(path_label, src, len, rtype, out);

    if (rtype == READER_TURMERIC && reader_type_is_implemented(lang_rt))
        rtype = lang_rt;

    Buf body_out;
    int rc = fmt_format_buffer(path_label, body, body_len, rtype, &body_out);
    if (rc != 0) return rc;

    buf_init(out);
    buf_write(out, src, head_len);
    /* The reader hands back the position after the directive TEXT, which may or
     * may not include its newline, and the printer strips leading blank lines
     * from the body -- so without this the two ran together as
     * `#lang saffron;;; ...`.  Normalising to exactly one newline also keeps
     * the pass idempotent, which `fmt-idempotence-stdlib` checks. */
    if (head_len == 0 || src[head_len - 1] != '\n') buf_putc(out, '\n');
    buf_write(out, body_out.data, body_out.len);
    buf_free(&body_out);
    return 0;
}
