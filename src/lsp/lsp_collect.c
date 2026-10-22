#include "lsp_collect.h"

#include "diag.h"
#include "expr.h"
#include "types.h"

#include <string.h>

/* Destination of the collection currently in flight, or NULL. */
static LspSymbol *out_      = NULL;
static int        cap_      = 0;
static int       *count_    = NULL;

static void collect_items(const Expr **items, uint32_t n);

static void collect_binding(const Binding *b, LspSymKind kind) {
    if (!b || !b->name || !b->is_global) return;
    /* Names the elaborator minted -- lifted lambdas (`__fn_774`) and instance
     * methods (`__inst_Eq_eq_qu_int`) -- are not part of the program a person
     * is editing.  Dropping them here rather than in on_completion is
     * deliberate: hover, go-to-definition, documentSymbol and
     * workspace/symbol all read this one index, and every one of them was
     * showing the mangled names.  There is nothing to navigate to and no
     * prefix a user would type, so there is no reader for whom keeping them
     * is better. */
    if (b->is_synthesized) return;
    if (!out_ || !count_) return;
    if (*count_ >= cap_) return;
    LspSymbol *sym = &out_[(*count_)++];
    memset(sym, 0, sizeof(*sym));
    sym->kind = kind;
    size_t nlen = strlen(b->name->name);
    if (nlen >= sizeof(sym->name)) nlen = sizeof(sym->name) - 1;
    memcpy(sym->name, b->name->name, nlen);
    const char *tn = type_name(b->type);
    if (tn) {
        size_t tlen = strlen(tn);
        if (tlen >= sizeof(sym->type_str)) tlen = sizeof(sym->type_str) - 1;
        memcpy(sym->type_str, tn, tlen);
    }
    sym->line      = (int)b->span.line;
    sym->col_start = (int)b->span.col_start;
    sym->col_end   = (int)b->span.col_end;
    const char *fp = diag_file_path(b->span.file_id);
    if (fp) {
        size_t flen = strlen(fp);
        if (flen >= sizeof(sym->file_path)) flen = sizeof(sym->file_path) - 1;
        memcpy(sym->file_path, fp, flen);
    }
}

/* Record kind, for an ADT binding.
 *
 * `defstruct` lowers to a `defdata` before it gets here, so the surface form
 * is no longer recoverable -- but the shape it lowered to is, and that is the
 * distinction a reader wants anyway: one constructor is a record ("type" with
 * fields), several are a sum ("type" with variants). A type whose constructors
 * refine its parameters is a sum too, whatever keyword spelled it; one whose
 * annotations all return the same instantiation is judged by its shape. */
static LspSymKind adt_kind(const AdtDef *def) {
    if (!def) return LSP_KIND_STRUCT;
    if (def->is_refined) return LSP_KIND_ENUM;
    return def->n_ctors == 1 ? LSP_KIND_STRUCT : LSP_KIND_ENUM;
}

static void collect_items(const Expr **items, uint32_t n) {
    for (uint32_t i = 0; i < n; i++) {
        const Expr *item = items[i];
        if (!item) continue;
        switch (item->kind) {
            case EX_FN_DEF:
                collect_binding(item->as.fn_def_.fn ? item->as.fn_def_.fn->binding : NULL,
                                LSP_KIND_FUNCTION);
                break;
            case EX_DEF:
                collect_binding(item->as.def_.binding, LSP_KIND_VALUE);
                break;
            case EX_DEFDATA:
                collect_binding(item->as.defdata_.binding,
                                adt_kind(item->as.defdata_.def));
                break;
            case EX_DEFMODULE:
                if (item->as.defmodule_.mod)
                    collect_items((const Expr **)item->as.defmodule_.mod->body,
                                  item->as.defmodule_.mod->n_body);
                break;
            default:
                break;
        }
    }
}

void lsp_collect_begin(LspSymbol *out, int cap, int *count_out) {
    out_   = out;
    cap_   = cap;
    count_ = count_out;
    if (count_) *count_ = 0;
}

bool lsp_collect_active(void) {
    return out_ != NULL;
}

void lsp_collect_program(const struct Expr *prog_) {
    const Expr *prog = (const Expr *)prog_;
    if (!out_ || !prog || prog->kind != EX_PROGRAM) return;
    collect_items((const Expr **)prog->as.program.items, prog->as.program.n);
}

void lsp_collect_end(void) {
    out_   = NULL;
    cap_   = 0;
    count_ = NULL;
}
