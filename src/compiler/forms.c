#include "forms.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

Form *form_new(Arena *a, FormTag tag, Span span) {
    Form *f = (Form *)arena_alloc(a, sizeof(Form));
    memset(f, 0, sizeof(*f));
    f->tag = tag;
    f->span = span;
    return f;
}

Form *form_nil(Arena *a, Span span) {
    return form_new(a, F_NIL, span);
}

Form *form_bool(Arena *a, Span span, bool b) {
    Form *f = form_new(a, F_BOOL, span);
    f->as.b = b;
    return f;
}

Form *form_int(Arena *a, Span span, int64_t i) {
    Form *f = form_new(a, F_INT, span);
    f->as.i = i;
    return f;
}

Form *form_float(Arena *a, Span span, double f) {
    Form *form = form_new(a, F_FLOAT, span);
    form->as.f = f;
    return form;
}

Form *form_str(Arena *a, Span span, const char *p, uint32_t len) {
    Form *f = form_new(a, F_STR, span);
    f->as.s.p = arena_strdup(a, p, len);
    f->as.s.len = len;
    return f;
}

Form *form_sym(Arena *a, Span span, const Symbol *sym) {
    Form *f = form_new(a, F_SYM, span);
    f->as.sym = sym;
    return f;
}

Form *form_keyword(Arena *a, Span span, const Symbol *sym) {
    Form *f = form_new(a, F_KEYWORD, span);
    f->as.sym = sym;
    return f;
}

static Form *form_seq(Arena *a, FormTag tag, Span span, Form **items, uint32_t len) {
    Form *f = form_new(a, tag, span);
    if (len) {
        Form **copy = (Form **)arena_alloc(a, sizeof(Form *) * len);
        memcpy(copy, items, sizeof(Form *) * len);
        f->as.list.items = copy;
    } else {
        f->as.list.items = NULL;
    }
    f->as.list.len = len;
    return f;
}

Form *form_list(Arena *a, Span span, Form **items, uint32_t len) {
    return form_seq(a, F_LIST, span, items, len);
}

Form *form_vec(Arena *a, Span span, Form **items, uint32_t len) {
    return form_seq(a, F_VEC, span, items, len);
}

Form *form_map(Arena *a, Span span, Form **items, uint32_t len) {
    return form_seq(a, F_MAP, span, items, len);
}

Form *form_set(Arena *a, Span span, Form **items, uint32_t len) {
    return form_seq(a, F_SET, span, items, len);
}

/* DL0: data-literal constructors -- payload identical to F_LIST */
Form *form_map_literal(Arena *a, Span span, Form **items, uint32_t len) {
    return form_seq(a, F_MAP_LITERAL, span, items, len);
}

Form *form_set_literal(Arena *a, Span span, Form **items, uint32_t len) {
    return form_seq(a, F_SET_LITERAL, span, items, len);
}

Form *form_row_literal(Arena *a, Span span, Form **items, uint32_t len) {
    return form_seq(a, F_ROW_LITERAL, span, items, len);
}

Form *form_cblock(Arena *a, Span span, StrSlice code) {
    Form *f = form_new(a, F_CBLOCK, span);
    /* The code slice points into the source file; we need to copy it */
    f->as.cblock.p = arena_strdup(a, code.p, code.len);
    f->as.cblock.len = code.len;
    return f;
}

/* Phase 6 */
Form *form_quote(Arena *a, Span span, Form *quoted) {
    Form *f = form_new(a, F_QUOTE, span);
    /* Allocate a proper list with one item */
    Form **items = (Form **)arena_alloc(a, sizeof(Form *));
    items[0] = quoted;
    f->as.list.items = items;
    f->as.list.len = 1;
    return f;
}

Form *form_quasiquote(Arena *a, Span span, Form *quoted) {
    Form *f = form_new(a, F_QUASIQUOTE, span);
    Form **items = (Form **)arena_alloc(a, sizeof(Form *));
    items[0] = quoted;
    f->as.list.items = items;
    f->as.list.len = 1;
    return f;
}

Form *form_unquote(Arena *a, Span span, Form *quoted) {
    Form *f = form_new(a, F_UNQUOTE, span);
    Form **items = (Form **)arena_alloc(a, sizeof(Form *));
    items[0] = quoted;
    f->as.list.items = items;
    f->as.list.len = 1;
    return f;
}

Form *form_unquote_splicing(Arena *a, Span span, Form *quoted) {
    Form *f = form_new(a, F_UNQUOTE_SPLICING, span);
    Form **items = (Form **)arena_alloc(a, sizeof(Form *));
    items[0] = quoted;
    f->as.list.items = items;
    f->as.list.len = 1;
    return f;
}

Form *form_type_ann(Arena *a, Span span, Form *inner) {
    Form *f = form_new(a, F_TYPE_ANN, span);
    Form **items = (Form **)arena_alloc(a, sizeof(Form *));
    items[0] = inner;
    f->as.list.items = items;
    f->as.list.len = 1;
    return f;
}

/* CT0: Contract type { var : T | pred } — stores items as a list */
Form *form_contract_type(Arena *a, Span span, Form **items, uint32_t len) {
    return form_seq(a, F_CONTRACT_TYPE, span, items, len);
}

/* INT-1: Reader conditional #?(:tur expr :turi expr) */
Form *form_reader_cond(Arena *a, Span span, Form **items, uint32_t len) {
    Form *f = form_new(a, F_READER_COND, span);
    Form **copy = (Form **)arena_alloc(a, len * sizeof(Form *));
    for (uint32_t i = 0; i < len; i++) copy[i] = items[i];
    f->as.list.items = copy;
    f->as.list.len = len;
    return f;
}

/* RR3: Range literal with variable name for shadowing check.
 * items[0] = var_sym form, items[1] = desugared range form. */
Form *form_range_var(Arena *a, Span span, const Symbol *var_sym, Form *range) {
    Form *f = form_new(a, F_RANGE_VAR, span);
    Form **items = (Form **)arena_alloc(a, 2 * sizeof(Form *));
    items[0] = form_sym(a, span, var_sym);
    items[1] = range;
    f->as.list.items = items;
    f->as.list.len = 2;
    return f;
}

/* Return the name of a FormTag as a string */
const char *form_tag_name(FormTag tag) {
    switch (tag) {
        case F_NIL: return "nil";
        case F_BOOL: return "bool";
        case F_INT: return "int";
        case F_FLOAT: return "float";
        case F_STR: return "string";
        case F_SYM: return "symbol";
        case F_KEYWORD: return "keyword";
        case F_LIST: return "list";
        case F_VEC: return "vector";
        case F_MAP: return "map";
        case F_SET: return "set";
        case F_CBLOCK: return "c-block";
        case F_QUOTE: return "quote";
        case F_QUASIQUOTE: return "quasiquote";
        case F_UNQUOTE: return "unquote";
        case F_UNQUOTE_SPLICING: return "unquote-splicing";
        case F_TYPE_ANN: return "type-ann";
        case F_CONTRACT_TYPE: return "contract-type";
        case F_READER_COND: return "reader-cond";
        case F_RANGE_VAR: return "range-var";
        case F_MAP_LITERAL: return "map-literal";
        case F_SET_LITERAL: return "set-literal";
        case F_ROW_LITERAL: return "row-literal";
        default: return "unknown";
    }
}

static void print_str_escaped(Buf *b, StrSlice s) {
    buf_putc(b, '"');
    for (uint32_t i = 0; i < s.len; i++) {
        char c = s.p[i];
        switch (c) {
            case '"':  buf_puts(b, "\\\""); break;
            case '\\': buf_puts(b, "\\\\"); break;
            case '\n': buf_puts(b, "\\n"); break;
            case '\t': buf_puts(b, "\\t"); break;
            case '\r': buf_puts(b, "\\r"); break;
            default:
                if ((unsigned char)c < 0x20) {
                    buf_printf(b, "\\x%02x", (unsigned char)c);
                } else {
                    buf_putc(b, c);
                }
        }
    }
    buf_putc(b, '"');
}

/* The shortest text that reads back to the same double, with a decimal
 * point or an exponent so that it reads back as a FLOAT.  defmacro* and the
 * inline-C JIT re-parse this output, and `%g` handed `2.0` back as the int 2
 * and 0.30000000000000004 as 0.3; a contract message printed `(>= r 0.0)` as
 * `(>= r 0)`.  A non-finite value keeps `%g` (there is no literal for it
 * outside #lang r7rs). */
static void print_float(Buf *b, double d) {
    if (!isfinite(d)) { buf_printf(b, "%g", d); return; }
    char tmp[40];
    for (int prec = 1; prec <= 17; prec++) {
        snprintf(tmp, sizeof tmp, "%.*g", prec, d);
        if (strtod(tmp, NULL) == d) break;
    }
    buf_puts(b, tmp);
    if (!strpbrk(tmp, ".eE")) buf_puts(b, ".0");
}

void form_print(Buf *b, const Form *f) {
    switch (f->tag) {
        case F_NIL:  buf_puts(b, "nil"); break;
        case F_BOOL: buf_puts(b, f->as.b ? "true" : "false"); break;
        case F_INT:  buf_printf(b, "%lld", (long long)f->as.i); break;
        case F_FLOAT: print_float(b, f->as.f); break;
        case F_STR:  print_str_escaped(b, f->as.s); break;
        case F_SYM:
            buf_write(b, f->as.sym->name, f->as.sym->len);
            break;
        case F_KEYWORD:
            buf_putc(b, ':');
            buf_write(b, f->as.sym->name, f->as.sym->len);
            break;
        case F_LIST:
            buf_putc(b, '(');
            for (uint32_t i = 0; i < f->as.list.len; i++) {
                if (i) buf_putc(b, ' ');
                form_print(b, f->as.list.items[i]);
            }
            buf_putc(b, ')');
            break;
        case F_VEC:
            buf_putc(b, '[');
            for (uint32_t i = 0; i < f->as.list.len; i++) {
                if (i) buf_putc(b, ' ');
                form_print(b, f->as.list.items[i]);
            }
            buf_putc(b, ']');
            break;
        case F_MAP:
            buf_puts(b, "#{");
            for (uint32_t i = 0; i < f->as.list.len; i++) {
                if (i) buf_putc(b, ' ');
                form_print(b, f->as.list.items[i]);
            }
            buf_putc(b, '}');
            break;
        case F_SET:
            buf_puts(b, "#s(");
            for (uint32_t i = 0; i < f->as.list.len; i++) {
                if (i) buf_putc(b, ' ');
                form_print(b, f->as.list.items[i]);
            }
            buf_putc(b, ')');
            break;
        case F_CBLOCK:
            buf_puts(b, "```c ");
            buf_write(b, f->as.cblock.p, f->as.cblock.len);
            buf_puts(b, "```");
            break;
        /* Phase 6 */
        case F_QUOTE:
            buf_puts(b, "(quote ");
            if (f->as.list.len > 0) {
                form_print(b, f->as.list.items[0]);
            }
            buf_puts(b, ")");
            break;
        case F_QUASIQUOTE:
            buf_puts(b, "(quasiquote ");
            if (f->as.list.len > 0) {
                form_print(b, f->as.list.items[0]);
            }
            buf_puts(b, ")");
            break;
        case F_UNQUOTE:
            buf_puts(b, "(unquote ");
            if (f->as.list.len > 0) {
                form_print(b, f->as.list.items[0]);
            }
            buf_puts(b, ")");
            break;
        case F_UNQUOTE_SPLICING:
            buf_puts(b, "(unquote-splicing ");
            if (f->as.list.len > 0) {
                form_print(b, f->as.list.items[0]);
            }
            buf_puts(b, ")");
            break;
        case F_TYPE_ANN:
            buf_puts(b, ": ");
            if (f->as.list.len > 0) {
                form_print(b, f->as.list.items[0]);
            }
            break;
        /* CT0: Contract type { var : T | pred } */
        case F_CONTRACT_TYPE:
            buf_puts(b, "{ ");
            for (uint32_t i = 0; i < f->as.list.len; i++) {
                if (i) buf_putc(b, ' ');
                form_print(b, f->as.list.items[i]);
            }
            buf_puts(b, " }");
            break;
        /* INT-1: Reader conditional */
        case F_READER_COND:
            buf_puts(b, "#?(");
            for (uint32_t i = 0; i < f->as.list.len; i++) {
                if (i) buf_putc(b, ' ');
                form_print(b, f->as.list.items[i]);
            }
            buf_putc(b, ')');
            break;
        /* RR3: Range literal variable annotation -- print the desugared range form */
        case F_RANGE_VAR:
            if (f->as.list.len > 1) form_print(b, f->as.list.items[1]);
            break;
        /* DL0: data literals */
        case F_MAP_LITERAL:
            buf_puts(b, "#map{");
            for (uint32_t i = 0; i < f->as.list.len; i++) {
                if (i) buf_putc(b, ' ');
                form_print(b, f->as.list.items[i]);
            }
            buf_putc(b, '}');
            break;
        case F_SET_LITERAL:
            buf_puts(b, "#set{");
            for (uint32_t i = 0; i < f->as.list.len; i++) {
                if (i) buf_putc(b, ' ');
                form_print(b, f->as.list.items[i]);
            }
            buf_putc(b, '}');
            break;
        case F_ROW_LITERAL:
            buf_puts(b, "#row{");
            for (uint32_t i = 0; i < f->as.list.len; i++) {
                if (i) buf_putc(b, ' ');
                form_print(b, f->as.list.items[i]);
            }
            buf_putc(b, '}');
            break;
    }
}

/* Deep structural equality (see forms.h).  Formerly elab_macros.c's static
 * ct_form_equal; hoisted so the interpreter's TURI_SYNTAX `=` shares the
 * exact semantics of the compile-time macro evaluator's `=`. */
bool form_equal(const Form *a, const Form *b) {
    if (a == b) return true;
    if (!a || !b) return false;
    if (a->tag != b->tag) return false;
    switch (a->tag) {
        case F_NIL: return true;
        case F_BOOL: return a->as.b == b->as.b;
        case F_INT: return a->as.i == b->as.i;
        case F_FLOAT: return a->as.f == b->as.f;
        case F_STR:
            return a->as.s.len == b->as.s.len &&
                   strncmp(a->as.s.p, b->as.s.p, a->as.s.len) == 0;
        case F_SYM:
        case F_KEYWORD:
            /* Name-based, not pointer-based: forms built in the macro-time
             * env intern into that env's own symbol table, so cross-symtab
             * pointer comparison would be always-false.  Within one table
             * this is exactly pointer identity (interning is unique). */
            if (a->as.sym == b->as.sym) return true;
            return a->as.sym && b->as.sym &&
                   a->as.sym->len == b->as.sym->len &&
                   memcmp(a->as.sym->name, b->as.sym->name, a->as.sym->len) == 0;
        case F_QUOTE:
        case F_QUASIQUOTE:
        case F_UNQUOTE:
        case F_UNQUOTE_SPLICING:
        case F_TYPE_ANN:
        /* CT0: Contract types compare structurally */
        case F_CONTRACT_TYPE:
        /* INT-1: Reader conditionals compare structurally */
        case F_READER_COND:
        /* RR3: Range literal variable annotation compares structurally */
        case F_RANGE_VAR:
        case F_LIST:
        case F_VEC:
        case F_MAP:
        case F_SET:
        case F_MAP_LITERAL:
        case F_SET_LITERAL:
        case F_ROW_LITERAL:
            if (a->as.list.len != b->as.list.len) return false;
            for (uint32_t i = 0; i < a->as.list.len; i++) {
                if (!form_equal(a->as.list.items[i], b->as.list.items[i])) return false;
            }
            return true;
        case F_CBLOCK:
            return a->as.cblock.len == b->as.cblock.len &&
                   strncmp(a->as.cblock.p, b->as.cblock.p, a->as.cblock.len) == 0;
    }
    return false;
}
