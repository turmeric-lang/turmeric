#ifndef TUR_FORMS_H
#define TUR_FORMS_H

#include <stdint.h>
#include <stdbool.h>

#include "runtime/arena.h"
#include "compiler/symbols.h"
#include "runtime/buf.h"

typedef struct Span {
    uint16_t file_id;
    uint32_t line;       /* 1-based */
    uint32_t col_start;  /* 1-based, inclusive */
    uint32_t col_end;    /* 1-based, exclusive */
    uint32_t off_start;  /* byte offset, inclusive */
    uint32_t off_end;    /* byte offset, exclusive */
} Span;

/* Sentinel for unknown/missing spans (Phase 8: diagnostics polish) */
#define SPAN_UNKNOWN ((Span){0})

/* Check if a span is unknown */
static inline bool span_is_unknown(Span s) {
    return s.file_id == 0 && s.line == 0 && s.off_start == 0 && s.off_end == 0;
}

/* Create a span from byte offsets */
static inline Span span_from_offsets(uint16_t file_id, uint32_t off_start, uint32_t off_end) {
    return (Span){file_id, 0, 0, 0, off_start, off_end};
}

/* Phase N: Literal suffix tag for typed integer/float literals (42i8, 3.14f32, etc.)
 * This mirrors TypeKind for numeric types but lives in forms.h to avoid a
 * circular dependency (types.h already includes forms.h for Span). */
typedef enum LiteralSuffix {
    LIT_SUF_NONE = 0,  /* unsuffixed — default type (int64 / double) */
    LIT_SUF_I8,        /* i8  → int8   */
    LIT_SUF_I16,       /* i16 → int16  */
    LIT_SUF_I32,       /* i32 → int32  */
    LIT_SUF_I64,       /* i64 → int64  */
    LIT_SUF_U8,        /* u8  → uint8  */
    LIT_SUF_U16,       /* u16 → uint16 */
    LIT_SUF_U32,       /* u32 → uint32 */
    LIT_SUF_U64,       /* u64 → uint64 */
    LIT_SUF_F32,       /* f32 → float32 */
    LIT_SUF_F64,       /* f64 → float64 */
    /* parsec-guide-plan P2: not a spelled suffix but the mark a `#\A`
     * character literal leaves on its F_INT, so the elaborator types it as
     * `Char` (stdlib/char.tur) and the formatter prints it back as `#\A`. */
    LIT_SUF_CHAR,
} LiteralSuffix;

typedef enum FormTag {
    F_NIL  = 0,
    F_BOOL,
    F_INT,
    F_FLOAT,       /* Phase 1: Float literals (1.0, 1.5e3) */
    F_STR,
    F_SYM,
    F_KEYWORD,    /* :foo */
    F_LIST,       /* (a b c) */
    F_VEC,        /* [a b c]  — same payload as F_LIST */
    F_MAP,        /* #{k1 v1 k2 v2} — same payload as F_LIST */
    F_SET,        /* #s(a b c) — same payload as F_LIST */
    F_CBLOCK,     /* ```c ... ``` C code block (Phase 2) */
    /* Phase 6: quote/quasiquote */
    F_QUOTE,      /* (quote x) - literal expression */
    F_QUASIQUOTE, /* (quasiquote x) - template literal with unquote */
    F_UNQUOTE,    /* (unquote x) - unquote in quasiquote */
    F_UNQUOTE_SPLICING, /* (unquote-splicing x) - splice unquoted list */
    F_TYPE_ANN,         /* `: type-expr` — compound type annotation wrapper produced by reader */
    /* CT0: Contract type { x : T | p } — refinement type annotation */
    F_CONTRACT_TYPE,    /* { var : base-type | predicate } */
    /* INT-1: Reader conditional #?(:tur expr :turi expr) */
    F_READER_COND,     /* #?(:tur <tur-form> :turi <turi-form>) */
    /* RR3: Range literal with variable name preserved for shadowing check */
    F_RANGE_VAR,       /* [var_sym_form, desugared_range_form] */
    /* DL0: Data literals -- payload identical to F_LIST, distinguished from
     * F_MAP (effect rows) and F_SET (#s(...) set literal) so the elaborator
     * can lower them to hamt-of / set-of calls. Gated behind -Xdata-literals. */
    F_MAP_LITERAL,     /* #map{k1 v1 k2 v2 ...} -- same payload as F_LIST */
    F_SET_LITERAL,     /* #set{e1 e2 e3 ...}    -- same payload as F_LIST */
    /* Variadic HKT rows: #row{T1 T2 ...} type-level row literal -- same payload
     * as F_LIST. Distinct from F_VEC (which means a TupleN type) and from the
     * value-level data literals above: a row is TYPE-ONLY and is lowered to a
     * TY_TYPEROW in type position; in value position it is an error. */
    F_ROW_LITERAL,
} FormTag;

/* fx-row-syntax-rename-plan: provenance tag on F_MAP forms that originate
 * as an effect row.  Drives the TUR-D0002/D0003 deprecation warnings emitted
 * when elaboration consumes an F_MAP as an effect row.  PROV_FX_NONE is the
 * zero default so memset(0) of a Form leaves provenance unset. */
typedef enum FxProvenance {
    PROV_FX_NONE = 0,        /* not an effect row, or originated as #fx{...} */
    PROV_FX_EXPLICIT,        /* #fx{...} -- preferred spelling, no warning */
    PROV_FX_LEGACY,          /* bare #{...} -- TUR-D0002 */
    PROV_FX_AT_LEGACY,       /* @{...}     -- TUR-D0003 */
    /* C2 / #reads: stamps the `(reads <sym>)` F_LIST produced by `#reads <sym>`
     * so the defn signature walk tells a read-frame annotation apart from a
     * body expression.  A read-frame is not an effect row; see
     * docs/guides/stateful-refinements-guide.md. */
    PROV_READS,              /* (reads <sym>) from #reads <sym> */
    /* WF1 / #writes: stamps the `(writes <sym>...)` F_LIST produced by
     * `#writes <sym>` or `#writes [<sym>...]`.  Same role as PROV_READS -- it
     * is what lets the defn signature walk tell a write-frame annotation apart
     * from a body expression -- but the frame is a SET of parameters, not one,
     * because a function may legitimately write more than one argument.  See
     * docs/archive/checked-write-frames-plan.md (WF1). */
    PROV_WRITES,             /* (writes <sym>...) from #writes <sym>|[<sym>...] */
    /* r7rs-lang-plan R7: stamps an F_VEC the Scheme reader read from `#(...)`
     * (a self-evaluating vector datum), as opposed to a Turmeric `[...]`
     * binding vector in the same file.  The Scheme lowering rewrites only the
     * stamped ones into `(vector ...)`. */
    PROV_SCHEME_VECTOR,
    /* r7rs-lang-plan R10: stamps the F_NIL / F_BOOL the Scheme reader made
     * from the WORDS `nil`, `true` and `false`.  In code they keep their
     * Turmeric meaning (the prelude is written with them); as quoted data
     * they are the symbols a Scheme program means -- `(symbol? 'nil)`. */
    PROV_SCHEME_WORD,
} FxProvenance;

struct Form;
typedef struct Form Form;

typedef struct FormList {
    Form    **items;
    uint32_t  len;
} FormList;

struct Form {
    FormTag       tag;
    Span          span;
    LiteralSuffix lit_suffix; /* Phase N: type suffix for F_INT/F_FLOAT; LIT_SUF_NONE otherwise */
    uint8_t       fx_prov;    /* FxProvenance: only meaningful for F_MAP forms */
    union {
        bool          b;
        int64_t       i;
        double        f;        /* Phase 1: Float literal value */
        StrSlice      s;        /* string literal contents (escapes resolved) */
        const Symbol *sym;
        FormList      list;
        StrSlice      cblock;   /* raw C code for F_CBLOCK */
    } as;
};

Form *form_new(Arena *a, FormTag tag, Span span);
Form *form_nil    (Arena *a, Span span);
Form *form_bool   (Arena *a, Span span, bool b);
Form *form_int    (Arena *a, Span span, int64_t i);
Form *form_float  (Arena *a, Span span, double f);
Form *form_str    (Arena *a, Span span, const char *p, uint32_t len);
Form *form_sym    (Arena *a, Span span, const Symbol *sym);
Form *form_keyword(Arena *a, Span span, const Symbol *sym);
Form *form_list   (Arena *a, Span span, Form **items, uint32_t len);
Form *form_vec    (Arena *a, Span span, Form **items, uint32_t len);
Form *form_map    (Arena *a, Span span, Form **items, uint32_t len);
Form *form_set    (Arena *a, Span span, Form **items, uint32_t len);
/* DL0: data-literal constructors (payload identical to F_LIST) */
Form *form_map_literal(Arena *a, Span span, Form **items, uint32_t len);
Form *form_set_literal(Arena *a, Span span, Form **items, uint32_t len);
/* Variadic HKT rows: #row{...} type-row literal (payload identical to F_LIST). */
Form *form_row_literal(Arena *a, Span span, Form **items, uint32_t len);
Form *form_cblock (Arena *a, Span span, StrSlice code);
/* Phase 6 */
Form *form_quote  (Arena *a, Span span, Form *quoted);
Form *form_type_ann(Arena *a, Span span, Form *inner);
/* CT0: Contract type { var : T | pred } */
Form *form_contract_type(Arena *a, Span span, Form **items, uint32_t len);
/* INT-1: Reader conditional form. items = [tur_key, tur_form, turi_key, turi_form] */
Form *form_reader_cond(Arena *a, Span span, Form **items, uint32_t len);
/* RR3: Range literal variable annotation. items = [var_sym_form, range_form] */
Form *form_range_var(Arena *a, Span span, const Symbol *var_sym, Form *range);
Form *form_quasiquote  (Arena *a, Span span, Form *quoted);
Form *form_unquote  (Arena *a, Span span, Form *quoted);
Form *form_unquote_splicing  (Arena *a, Span span, Form *quoted);

void  form_print(Buf *b, const Form *f);

/* Return the name of a FormTag as a string */
const char *form_tag_name(FormTag tag);

/* Deep structural equality over two forms.  Symbols/keywords compare by
 * interned pointer identity; strings/C-blocks by content; sequence forms
 * recursively.  Spans are ignored.  Shared by the compile-time macro
 * evaluator's `=` builtin and the interpreter's TURI_SYNTAX `=`. */
bool form_equal(const Form *a, const Form *b);

#endif
