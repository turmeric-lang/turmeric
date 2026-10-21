/* span_audit.c -- breakpoint-span coverage audit (debugger Phase 1).
 *
 * See span_audit.h and docs/archive/history/debugger-plan.md (Phase 1).
 *
 * The traversal mirrors the per-kind child extraction used by the effect-row
 * pass (src/passes/effect_check.c) so coverage of the Expr union stays in
 * step with the rest of the compiler.  A node is "breakpoint-eligible" when it
 * is a category a debugger would let you set a breakpoint on or surface as a
 * stack frame: a top-level form, a defn, a let form, or a call site.  Such a
 * node is a "hole" when its span is SPAN_UNKNOWN or carries no line (line == 0),
 * because neither resolves to a source location the debugger can stop at.
 */

#include "span_audit.h"

#include "diag.h"
#include "forms.h"

/* True when a span cannot back a breakpoint: no file/offset (SPAN_UNKNOWN) or
 * no source line.  A debugger keys breakpoints on (file, line), so a missing
 * line is just as useless as a fully-unknown span even if byte offsets exist. */
static bool span_has_no_line(Span s) {
    return span_is_unknown(s) || s.line == 0;
}

/* Map a node kind to its breakpoint category, or NULL when the kind is not
 * itself a breakpoint target (it may still hold children that are). */
static const char *node_category(const Expr *e) {
    switch (e->kind) {
    case EX_CALL:   return "call";
    case EX_FN_DEF: return "defn";
    case EX_LET:
    case EX_LETREC: return "let";
    default:        return NULL;
    }
}

static void check_node(const Expr *e, const char *category,
                       SpanAuditResult *r, FILE *out) {
    r->n_checked++;
    if (!span_has_no_line(e->span)) return;
    r->n_holes++;
    const char *path = diag_file_path(e->span.file_id);
    fprintf(out, "  %s:%u:%u  %s  (kind=%d, span=%s)\n",
            path ? path : "<unknown-file>",
            e->span.line, e->span.col_start, category, (int)e->kind,
            span_is_unknown(e->span) ? "SPAN_UNKNOWN" : "line==0");
}

/* Forward decl: the recursive worker.  `is_top` is true only for direct
 * top-level forms (children of EX_PROGRAM / a defmodule body), which are
 * breakpoint-eligible regardless of their kind. */
static void audit_expr(const Expr *e, bool is_top,
                       SpanAuditResult *r, FILE *out);

#define REC(child)      audit_expr((child), false, r, out)
#define REC_TOP(child)  audit_expr((child), true,  r, out)

static void audit_fn_body(const FnDef *fn, SpanAuditResult *r, FILE *out) {
    if (fn && fn->body) REC(fn->body);
}

static void audit_expr(const Expr *e, bool is_top,
                       SpanAuditResult *r, FILE *out) {
    if (!e) return;

    /* Self-check: the node's own breakpoint category wins; a top-level form of
     * an otherwise-uncategorised kind is still checked as a top-level form. */
    const char *cat = node_category(e);
    if (!cat && is_top) cat = "top-level form";
    if (cat) check_node(e, cat, r, out);

    /* Recurse into children.  Arms with no Expr children fall through to the
     * default and stop. */
    switch (e->kind) {
    case EX_PROGRAM:
        for (uint32_t i = 0; i < e->as.program.n; i++)
            REC_TOP(e->as.program.items[i]);
        return;
    case EX_DEFMODULE: {
        const DefModule *m = e->as.defmodule_.mod;
        if (m)
            for (uint32_t i = 0; i < m->n_body; i++) REC_TOP(m->body[i]);
        return;
    }

    case EX_LET:
    case EX_LETREC:
        for (uint32_t i = 0; i < e->as.let_.n; i++)
            REC(e->as.let_.bindings[i].init);
        REC(e->as.let_.body);
        return;

    case EX_IF:
        REC(e->as.if_.cond);
        REC(e->as.if_.then_);
        REC(e->as.if_.else_or_null);
        return;

    case EX_DO:
        for (uint32_t i = 0; i < e->as.do_.n; i++) REC(e->as.do_.items[i]);
        return;

    case EX_WHILE:
        REC(e->as.while_.cond);
        REC(e->as.while_.body);
        return;

    case EX_SET:
        REC(e->as.set_.value);
        return;

    case EX_DEF:
        REC(e->as.def_.init);
        return;

    case EX_BUILTIN:
        for (uint32_t i = 0; i < e->as.builtin.n; i++) REC(e->as.builtin.args[i]);
        return;

    case EX_FN:
        audit_fn_body(e->as.fn_.fn, r, out);
        return;
    case EX_FN_DEF:
        audit_fn_body(e->as.fn_def_.fn, r, out);
        return;
    case EX_CLOSURE:
        if (e->as.closure_.closure) audit_fn_body(e->as.closure_.closure->fn, r, out);
        return;

    case EX_CALL:
        if (e->as.call_.fn_expr) REC(e->as.call_.fn_expr);
        if (e->as.call_.dict_arg) REC(e->as.call_.dict_arg);
        for (uint32_t i = 0; i < e->as.call_.n_args; i++)
            REC(e->as.call_.args[i]);
        return;

    case EX_RETURN:
        REC(e->as.return_.value);
        return;

    case EX_DEFER:
        REC(e->as.defer_.body);
        return;

    case EX_REF:           REC(e->as.ref_.expr); return;
    case EX_DEREF:         REC(e->as.deref_.expr); return;
    case EX_RC_OF:         REC(e->as.rc_of_.expr); return;
    case EX_RC_CLONE:      REC(e->as.rc_clone_.expr); return;
    case EX_RC_DROP:       REC(e->as.rc_drop_.expr); return;
    case EX_RC_PTR:        REC(e->as.rc_ptr_.expr); return;
    case EX_RC_COUNT:      REC(e->as.rc_count_.expr); return;
    case EX_RC_FROM_REF:   REC(e->as.rc_from_ref_.expr); return;
    case EX_REF_FROM_RC:   REC(e->as.ref_from_rc_.expr); return;
    case EX_WEAK:          REC(e->as.weak_.expr); return;
    case EX_WEAK_PRED:     REC(e->as.weak_pred_.expr); return;
    case EX_REF_PRED:      REC(e->as.ref_pred_.expr); return;
    case EX_BORROW_IMMUT:  REC(e->as.borrow_immut_.expr); return;
    case EX_BORROW_MUT:    REC(e->as.borrow_mut_.expr); return;
    case EX_CONT_PRED:     REC(e->as.cont_pred_.expr); return;

    case EX_SET_DEREF:
        REC(e->as.set_deref_.ref);
        REC(e->as.set_deref_.value);
        return;

    case EX_PANIC:       REC(e->as.panic_.payload); return;
    case EX_PANIC_WITH:  REC(e->as.panic_with_.payload); return;
    case EX_CATCH_UNWIND: REC(e->as.catch_unwind_.thunk); return;
    case EX_CATCH_PANIC_OF: REC(e->as.catch_panic_of_.thunk); return;
    case EX_PANIC_PAYLOAD_TYPE:
    case EX_PANIC_PAYLOAD_VALUE:
    case EX_PANIC_PAYLOAD_FILE:
    case EX_PANIC_PAYLOAD_LINE:
        REC(e->as.panic_payload_type_.payload); return;
    case EX_PANIC_PAYLOAD_DOWNS:
        REC(e->as.panic_payload_downs_.payload); return;

    case EX_RESET:           REC(e->as.reset_.body); return;
    case EX_SERIAL_RESET:    REC(e->as.serial_reset_.body); return;
    case EX_CLONEABLE_RESET: REC(e->as.cloneable_reset_.body); return;

    case EX_SHIFT:
        REC(e->as.shift_.k_fn); REC(e->as.shift_.body); return;
    case EX_SHIFT0:
        REC(e->as.shift0_.k_fn); REC(e->as.shift0_.body); return;
    case EX_SERIAL_SHIFT:
        REC(e->as.serial_shift_.k_fn); REC(e->as.serial_shift_.body); return;
    case EX_CLONEABLE_SHIFT:
        REC(e->as.cloneable_shift_.k_fn); REC(e->as.cloneable_shift_.body); return;

    case EX_HANDLE: {
        const HandleExpr *h = e->as.handle_.handle;
        if (h) {
            REC(h->body);
            for (uint8_t i = 0; i < h->n_cases; i++) REC(h->cases[i].body);
        }
        return;
    }
    case EX_HANDLER_LIT: {
        const HandleExpr *h = e->as.handler_lit_.handle;
        if (h) for (uint8_t i = 0; i < h->n_cases; i++) REC(h->cases[i].body);
        return;
    }
    case EX_WITH_HANDLER:
        REC(e->as.with_handler_.handler);
        REC(e->as.with_handler_.body);
        return;
    case EX_COMPOSE_HANDLERS:
        REC(e->as.compose_handlers_.h1);
        REC(e->as.compose_handlers_.h2);
        return;

    case EX_PERFORM: {
        const PerformExpr *p = e->as.perform_.perform;
        if (p) for (uint32_t i = 0; i < p->n_args; i++) REC(p->args[i]);
        return;
    }
    case EX_RESUME:
        if (e->as.resume_.resume) {
            REC(e->as.resume_.resume->k);
            REC(e->as.resume_.resume->value);
        }
        return;
    case EX_DISCONTINUE:
        if (e->as.discontinue_.discontinue) {
            REC(e->as.discontinue_.discontinue->k);
            REC(e->as.discontinue_.discontinue->exception);
        }
        return;

    case EX_ASYNC: REC(e->as.async_.fn_expr); return;
    case EX_AWAIT: REC(e->as.await_.fut_expr); return;

    case EX_SELECT:
        for (uint32_t i = 0; i < e->as.select_.n_clauses; i++) {
            REC(e->as.select_.clauses[i].chan);
            REC(e->as.select_.clauses[i].send_val);
            REC(e->as.select_.clauses[i].body);
        }
        REC(e->as.select_.default_body);
        return;

    case EX_STM:
        for (uint32_t i = 0; i < e->as.stm_.n_body; i++) REC(e->as.stm_.body[i]);
        return;
    case EX_ATOMICALLY: REC(e->as.atomically_.stm_expr); return;
    case EX_CHECK:      REC(e->as.check_.cond); return;
    case EX_OR_ELSE:
        REC(e->as.or_else_.stm1); REC(e->as.or_else_.stm2); return;
    case EX_TVAR_NEW:    REC(e->as.tvar_new_.init); return;
    case EX_TVAR_READ:   REC(e->as.tvar_read_.tvar); return;
    case EX_TVAR_WRITE:
        REC(e->as.tvar_write_.tvar); REC(e->as.tvar_write_.value); return;
    case EX_TVAR_MODIFY:
        REC(e->as.tvar_modify_.tvar); REC(e->as.tvar_modify_.fn); return;
    case EX_TVAR_SWAP:
        REC(e->as.tvar_swap_.tvar); REC(e->as.tvar_swap_.new_val); return;
    case EX_TVAR_CAS:
        REC(e->as.tvar_cas_.tvar);
        REC(e->as.tvar_cas_.old_val);
        REC(e->as.tvar_cas_.new_val);
        return;

    case EX_CAST:        REC(e->as.cast_.expr); return;
    case EX_REINTERPRET: REC(e->as.reinterpret_.expr); return;

    case EX_MAKE_STRUCT:
        for (uint32_t i = 0; i < e->as.make_struct_.n_fields; i++)
            REC(e->as.make_struct_.field_values[i]);
        return;
    case EX_GET_FIELD: REC(e->as.get_field_.struct_expr); return;
    case EX_SET_FIELD:
        REC(e->as.set_field_.receiver); REC(e->as.set_field_.value); return;

    case EX_SET_LIT:
        for (uint32_t i = 0; i < e->as.set_lit_.n; i++) REC(e->as.set_lit_.items[i]);
        return;

    case EX_POLY_WRAP:  REC(e->as.poly_wrap_.inner); return;
    case EX_FN_TO_FAT:  REC(e->as.fn_to_fat_.inner); return;
    case EX_POLY_TO_FAT: REC(e->as.poly_to_fat_.inner); return;
    case EX_ASCRIBE:    REC(e->as.ascribe_.inner); return;

    case EX_EXISTS_PACK: REC(e->as.exists_pack_.value); return;
    case EX_EXISTS_OPEN:
        REC(e->as.exists_open_.packed); REC(e->as.exists_open_.body); return;
    case EX_EXISTS_DISPATCH:
        for (uint32_t i = 0; i < e->as.exists_dispatch_.n_args; i++)
            REC(e->as.exists_dispatch_.args[i]);
        return;

    case EX_MATCH:
        REC(e->as.match_.scrutinee);
        for (uint32_t i = 0; i < e->as.match_.n_arms; i++) {
            REC(e->as.match_.arms[i].body);
            REC(e->as.match_.arms[i].guard);
        }
        return;

    case EX_UNION_INJECT: REC(e->as.union_inject_.value); return;
    case EX_ANY_TYPE_OF:  REC(e->as.any_type_of_.value); return;
    case EX_ANY_IS:       REC(e->as.any_is_.value); return;
    case EX_ANY_CAST:     REC(e->as.any_cast_.value); return;

    case EX_DEFDYNAMIC: REC(e->as.defdynamic_.root_expr); return;
    case EX_DYNVAR_BINDING:
        for (uint32_t i = 0; i < e->as.dynvar_binding_.n_pairs; i++)
            REC(e->as.dynvar_binding_.pairs[i].override_expr);
        REC(e->as.dynvar_binding_.body);
        return;
    case EX_DYNVAR_SET: REC(e->as.dynvar_set_.value); return;

    case EX_GEN:
        if (e->as.gen_.def) REC(e->as.gen_.def->body);
        return;
    case EX_YIELD:    REC(e->as.yield_.value); return;
    case EX_GEN_NEXT: REC(e->as.gen_next_.gen_expr); return;
    case EX_GEN_UNWRAP: REC(e->as.gen_unwrap_.ptr_expr); return;
    case EX_GEN_DONE: REC(e->as.gen_done_.gen_expr); return;

    case EX_CONS_LIST:
        for (uint32_t i = 0; i < e->as.cons_list_.n; i++) REC(e->as.cons_list_.items[i]);
        return;

    case EX_CPS_CONT_APP:
        REC(e->as.cps_cont_app_.cont); REC(e->as.cps_cont_app_.value); return;

    default:
        /* Leaf or no-Expr-child kind (literals, EX_VAR, EX_DICT, EX_INLINE_C,
         * EX_EXTERN_C, EX_DEFDATA/EX_DEFGADT, EX_TYPECLASS_DEF, definstance,
         * effect defs, EX_SYM_LIT, EX_DEFAULT_OF, EX_RETRY, ...). */
        return;
    }
}

#undef REC
#undef REC_TOP

int span_audit_program(const Expr *prog, FILE *out) {
    SpanAuditResult r = {0, 0};
    if (prog) audit_expr(prog, /*is_top=*/true, &r, out);
    fprintf(out, "span-audit: %d hole(s) in %d breakpoint-eligible node(s)\n",
            r.n_holes, r.n_checked);
    return r.n_holes;
}
