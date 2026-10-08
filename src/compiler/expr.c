#include "expr.h"
#include "typeclass.h"  /* Phase 15 */

#include <stdio.h>
#include <string.h>

Expr *expr_new(Arena *a, ExprKind k, Type t, Span span) {
    Expr *e = (Expr *)arena_alloc(a, sizeof(Expr));
    memset(e, 0, sizeof(*e));
    e->kind = k;
    e->type = t;
    e->span = span;
    return e;
}

const char *binding_fn_describe(const Binding *b, char *buf, size_t cap) {
    if (!buf || cap == 0) return "";
    if (!b || !b->name)
        snprintf(buf, cap, "anonymous function");
    else if (b->is_synthesized && b->diag_label)
        snprintf(buf, cap, "%s", b->diag_label);
    else if (b->is_synthesized)
        /* A mint site that has not set a label yet: still never the gensym. */
        snprintf(buf, cap, "compiler-generated function");
    else
        snprintf(buf, cap, "function '%s'", b->name->name);
    return buf;
}

/* Minimal debug printer; not critical to phase 1, but useful when codegen
 * misbehaves. */
void expr_print(Buf *b, const Expr *e) {
    switch (e->kind) {
        /* saffron-lang-plan S3: a runtime-resolved builtin. */
        case EX_DYN_OP:
            buf_puts(b, "(dyn-op ");
            buf_puts(b, e->as.dyn_op_.op ? e->as.dyn_op_.op->name : "?");
            buf_puts(b, " ...)");
            break;
        /* saffron-lang-plan S4: a call through a dynamic callee. */
        case EX_DYN_CALL: buf_puts(b, "(dyn-call ...)"); break;
        case EX_DYN_FIELD:
            buf_puts(b, "(dyn-field .");
            buf_puts(b, e->as.dyn_field_.field ? e->as.dyn_field_.field->name : "?");
            buf_puts(b, ")");
            break;
        /* saffron-lang-plan S9 (D8 piece 4): the class is known, the instance
         * is not -- so show the class, which is the part a reader can act on. */
        case EX_DYN_METHOD:
            buf_puts(b, "(dyn-method ");
            buf_puts(b, e->as.dyn_method_.tc && e->as.dyn_method_.tc->name
                            ? e->as.dyn_method_.tc->name->name : "?");
            buf_puts(b, ")");
            break;
        case EX_NIL_LIT:  buf_puts(b, "nil"); break;
        case EX_BOOL_LIT: buf_puts(b, e->as.b ? "true" : "false"); break;
        case EX_INT_LIT:  buf_printf(b, "%lld", (long long)e->as.i); break;
        case EX_FLOAT_LIT: buf_printf(b, "%g", e->as.f); break;
        case EX_CSTR_LIT: buf_putc(b, '"'); buf_write(b, e->as.s.p, e->as.s.len); buf_putc(b, '"'); break;
        case EX_SYM_LIT:  buf_putc(b, ':'); if (e->as.sym_lit_.sym) buf_write(b, e->as.sym_lit_.sym->name, e->as.sym_lit_.sym->len); break;
        case EX_DEFAULT_OF: buf_puts(b, "(default-of ?)"); break;
        case EX_CPS_CONT_APP:
            buf_puts(b, "(cps-apply ");
            expr_print(b, e->as.cps_cont_app_.cont);
            buf_putc(b, ' ');
            expr_print(b, e->as.cps_cont_app_.value);
            buf_putc(b, ')');
            break;
        case EX_VAR:      buf_write(b, e->as.var.binding->name->name, e->as.var.binding->name->len); break;
        case EX_CALLCC:
            buf_puts(b, e->as.callcc_.is_escape ? "(escape " : "(call/cc ");
            expr_print(b, e->as.callcc_.fn);
            buf_putc(b, ')');
            break;
        case EX_LETREC:
        case EX_LET:
            buf_puts(b, "(let [");
            for (uint32_t i = 0; i < e->as.let_.n; i++) {
                if (i) buf_putc(b, ' ');
                Binding *bd = e->as.let_.bindings[i].binding;
                buf_write(b, bd->name->name, bd->name->len);
                buf_putc(b, ' ');
                expr_print(b, e->as.let_.bindings[i].init);
            }
            buf_puts(b, "] ");
            expr_print(b, e->as.let_.body);
            buf_putc(b, ')');
            break;
        case EX_IF:
            buf_puts(b, "(if ");
            expr_print(b, e->as.if_.cond);
            buf_putc(b, ' ');
            expr_print(b, e->as.if_.then_);
            if (e->as.if_.else_or_null) {
                buf_putc(b, ' ');
                expr_print(b, e->as.if_.else_or_null);
            }
            buf_putc(b, ')');
            break;
        case EX_DO:
            buf_puts(b, "(do");
            for (uint32_t i = 0; i < e->as.do_.n; i++) {
                buf_putc(b, ' ');
                expr_print(b, e->as.do_.items[i]);
            }
            buf_putc(b, ')');
            break;
        case EX_WHILE:
            buf_puts(b, "(while ");
            expr_print(b, e->as.while_.cond);
            buf_putc(b, ' ');
            expr_print(b, e->as.while_.body);
            buf_putc(b, ')');
            break;
        case EX_SET:
            buf_puts(b, "(set! ");
            buf_write(b, e->as.set_.target->name->name, e->as.set_.target->name->len);
            buf_putc(b, ' ');
            expr_print(b, e->as.set_.value);
            buf_putc(b, ')');
            break;
        case EX_SET_DEREF:
            buf_puts(b, "(set! (@ ");
            expr_print(b, e->as.set_deref_.ref);
            buf_puts(b, ") ");
            expr_print(b, e->as.set_deref_.value);
            buf_putc(b, ')');
            break;
        case EX_DEF:
            buf_puts(b, "(def ");
            buf_write(b, e->as.def_.binding->name->name, e->as.def_.binding->name->len);
            buf_putc(b, ' ');
            expr_print(b, e->as.def_.init);
            buf_putc(b, ')');
            break;
        case EX_BUILTIN:
            buf_putc(b, '(');
            buf_puts(b, "builtin");
            for (uint32_t i = 0; i < e->as.builtin.n; i++) {
                buf_putc(b, ' ');
                expr_print(b, e->as.builtin.args[i]);
            }
            buf_putc(b, ')');
            break;
        case EX_PROGRAM:
            for (uint32_t i = 0; i < e->as.program.n; i++) {
                if (i) buf_putc(b, '\n');
                expr_print(b, e->as.program.items[i]);
            }
            break;
        /* Phase 2: not yet implemented */
        case EX_FN:
            buf_puts(b, "<fn>"); break;
        case EX_CALL:
            buf_puts(b, "<call>"); break;
        case EX_FN_DEF:
            buf_puts(b, "<fn-def>"); break;
        case EX_EXTERN_C:
            buf_puts(b, "<extern-c>"); break;
        case EX_INLINE_C:
            buf_puts(b, "<inline-c>"); break;
        /* Phase 3 */
        case EX_CLOSURE:
            buf_puts(b, "<closure>"); break;
        /* Phase 4 */
        case EX_DEFER:
            buf_puts(b, "(defer ");
            expr_print(b, e->as.defer_.body);
            buf_putc(b, ')');
            break;
        case EX_RETURN:
            buf_puts(b, "(return");
            if (e->as.return_.value) {
                buf_putc(b, ' ');
                expr_print(b, e->as.return_.value);
            }
            buf_putc(b, ')');
            break;
        /* Phase 5 */
        case EX_REF:
            buf_puts(b, "(ref ");
            expr_print(b, e->as.ref_.expr);
            buf_putc(b, ')');
            break;
        case EX_DEREF:
            buf_puts(b, "(@ ");
            expr_print(b, e->as.deref_.expr);
            buf_putc(b, ')');
            break;
        /* Phase 9: rc<T> + weak<T> */
        case EX_RC_OF:
            buf_puts(b, "(rc/of ");
            expr_print(b, e->as.rc_of_.expr);
            buf_putc(b, ')');
            break;
        case EX_RC_CLONE:
            buf_puts(b, "(rc/clone ");
            expr_print(b, e->as.rc_clone_.expr);
            buf_putc(b, ')');
            break;
        case EX_RC_DROP:
            buf_puts(b, "(rc/drop ");
            expr_print(b, e->as.rc_drop_.expr);
            buf_putc(b, ')');
            break;
        case EX_RC_PTR:
            buf_puts(b, "(rc->ptr ");
            expr_print(b, e->as.rc_ptr_.expr);
            buf_putc(b, ')');
            break;
        case EX_RC_COUNT:
            buf_puts(b, "(rc/strong-count ");
            expr_print(b, e->as.rc_count_.expr);
            buf_putc(b, ')');
            break;
        case EX_RC_FROM_REF:
            buf_puts(b, "(rc/from-ref ");
            expr_print(b, e->as.rc_from_ref_.expr);
            buf_putc(b, ')');
            break;
        case EX_REF_FROM_RC:
            buf_puts(b, "(ref/from-rc ");
            expr_print(b, e->as.ref_from_rc_.expr);
            buf_putc(b, ')');
            break;
        case EX_WEAK:
            buf_puts(b, "(weak ");
            expr_print(b, e->as.weak_.expr);
            buf_putc(b, ')');
            break;
        case EX_WEAK_UPGRADE:
            buf_puts(b, "(upgrade ");
            expr_print(b, e->as.weak_upgrade_.expr);
            buf_putc(b, ')');
            break;
        case EX_WEAK_PRED:
            buf_puts(b, "(weak? ");
            expr_print(b, e->as.weak_pred_.expr);
            buf_putc(b, ')');
            break;
        case EX_REF_PRED:
            buf_puts(b, "(ref? ");
            expr_print(b, e->as.ref_pred_.expr);
            buf_putc(b, ')');
            break;
        case EX_CONT_PRED:
            buf_puts(b, "(cont? ");
            expr_print(b, e->as.cont_pred_.expr);
            buf_putc(b, ')');
            break;
        /* Phase T21-F: async/await */
        case EX_ASYNC:
            buf_puts(b, "(async ");
            expr_print(b, e->as.async_.fn_expr);
            buf_putc(b, ')');
            break;
        case EX_AWAIT:
            buf_puts(b, "(await ");
            expr_print(b, e->as.await_.fut_expr);
            buf_putc(b, ')');
            break;
        /* Phase SEL1: fair multi-channel select */
        case EX_SELECT:
            buf_puts(b, "(select ...)");
            break;
        /* Phase N: numeric cast */
        case EX_CAST:
            buf_puts(b, "(as ");
            buf_puts(b, typekind_to_string(e->as.cast_.target_kind));
            buf_putc(b, ' ');
            expr_print(b, e->as.cast_.expr);
            buf_putc(b, ')');
            break;
        case EX_REINTERPRET:
            buf_puts(b, "(reinterpret ");
            buf_puts(b, typekind_to_string(e->as.reinterpret_.source_kind));
            buf_puts(b, " -> ");
            buf_puts(b, typekind_to_string(e->as.reinterpret_.target_kind));
            buf_putc(b, ' ');
            expr_print(b, e->as.reinterpret_.expr);
            buf_putc(b, ')');
            break;
        /* Phase H §1: dictionary passing */
        case EX_DICT:
            buf_printf(b, "(dict %s)", e->as.dict_.dict_name);
            break;
        /* Phase 12: Borrow traits */
        case EX_BORROW_IMMUT:
            buf_puts(b, "(& ");
            expr_print(b, e->as.borrow_immut_.expr);
            buf_putc(b, ')');
            break;
        case EX_BORROW_MUT:
            buf_puts(b, "(&mut ");
            expr_print(b, e->as.borrow_mut_.expr);
            buf_putc(b, ')');
            break;
        /* Phase 15: Typeclasses */
        case EX_TYPECLASS_DEF:
            buf_puts(b, "(defclass ");
            buf_puts(b, e->as.typeclass_def_.typeclass->name->name);
            buf_putc(b, ')');
            break;
        case EX_INSTANCE_DEF:
            buf_puts(b, "(definstance ");
            buf_puts(b, e->as.instance_def_.instance->typeclass->name->name);
            buf_putc(b, ')');
            break;
        /* Phase R2: Panic */
        case EX_PANIC:
            buf_puts(b, "(panic ");
            expr_print(b, e->as.panic_.payload);
            buf_putc(b, ')');
            break;
        case EX_PANIC_WITH:
            buf_puts(b, "(panic-with ");
            expr_print(b, e->as.panic_with_.payload);
            buf_putc(b, ')');
            break;
        case EX_CATCH_UNWIND:
            buf_puts(b, "(catch-unwind ");
            expr_print(b, e->as.catch_unwind_.thunk);
            buf_putc(b, ')');
            break;
        case EX_CATCH_PANIC_OF:
            buf_puts(b, "(catch-panic-of ");
            buf_printf(b, "%s ", typekind_to_string(e->as.catch_panic_of_.type_kind));
            expr_print(b, e->as.catch_panic_of_.thunk);
            buf_putc(b, ')');
            break;
        case EX_PANIC_PAYLOAD_TYPE:
            buf_puts(b, "(panic-payload-type ");
            expr_print(b, e->as.panic_payload_type_.payload);
            buf_putc(b, ')');
            break;
        case EX_PANIC_PAYLOAD_VALUE:
            buf_puts(b, "(panic-payload-value ");
            expr_print(b, e->as.panic_payload_value_.payload);
            buf_putc(b, ')');
            break;
        case EX_PANIC_PAYLOAD_FILE:
            buf_puts(b, "(panic-payload-file ");
            expr_print(b, e->as.panic_payload_file_.payload);
            buf_putc(b, ')');
            break;
        case EX_PANIC_PAYLOAD_LINE:
            buf_puts(b, "(panic-payload-line ");
            expr_print(b, e->as.panic_payload_line_.payload);
            buf_putc(b, ')');
            break;
        case EX_PANIC_PAYLOAD_DOWNS:
            buf_puts(b, "(panic-payload-downcast ");
            expr_print(b, e->as.panic_payload_downs_.payload);
            buf_printf(b, " %s)", typekind_to_string(e->as.panic_payload_downs_.target_type));
            break;
        /* Phase 18: Delimited continuations */
        case EX_RESET:
            buf_puts(b, "(reset ");
            expr_print(b, e->as.reset_.body);
            buf_putc(b, ')');
            break;
        case EX_SHIFT:
            buf_puts(b, "(shift ");
            expr_print(b, e->as.shift_.k_fn);
            buf_puts(b, " ");
            expr_print(b, e->as.shift_.body);
            buf_putc(b, ')');
            break;
        case EX_SHIFT0:
            buf_puts(b, "(shift0 ");
            expr_print(b, e->as.shift0_.k_fn);
            buf_puts(b, " ");
            expr_print(b, e->as.shift0_.body);
            buf_putc(b, ')');
            break;
        /* Phase B2: Cloneable continuations */
        case EX_CLONEABLE_RESET:
            buf_puts(b, "(cloneable-reset ");
            expr_print(b, e->as.cloneable_reset_.body);
            buf_putc(b, ')');
            break;
        case EX_CLONEABLE_SHIFT:
            buf_puts(b, "(cloneable-shift ");
            expr_print(b, e->as.cloneable_shift_.k_fn);
            buf_puts(b, " ");
            expr_print(b, e->as.cloneable_shift_.body);
            buf_putc(b, ')');
            break;
        /* Phase 19: Algebraic effects */
        case EX_DEFECT:
            buf_puts(b, "(defeffect ");
            buf_puts(b, e->as.effect_def_.def->name->name);
            buf_putc(b, ')');
            break;
        /* DV0-DV1: Dynamic vars */
        case EX_DEFDYNAMIC:
            buf_puts(b, "(defdynamic ");
            buf_puts(b, e->as.defdynamic_.entry->name->name);
            buf_putc(b, ')');
            break;
        case EX_DYNVAR_READ:
            buf_puts(b, e->as.dynvar_read_.entry->name->name);
            break;
        case EX_DYNVAR_BINDING:
            buf_puts(b, "(binding [");
            for (uint32_t i = 0; i < e->as.dynvar_binding_.n_pairs; i++) {
                if (i > 0) buf_putc(b, ' ');
                buf_puts(b, e->as.dynvar_binding_.pairs[i].entry->name->name);
                buf_putc(b, ' ');
                expr_print(b, e->as.dynvar_binding_.pairs[i].override_expr);
            }
            buf_puts(b, "] ");
            expr_print(b, e->as.dynvar_binding_.body);
            buf_putc(b, ')');
            break;
        case EX_DYNVAR_SET:
            buf_puts(b, "(set! ");
            buf_puts(b, e->as.dynvar_set_.entry->name->name);
            buf_putc(b, ' ');
            expr_print(b, e->as.dynvar_set_.value);
            buf_putc(b, ')');
            break;
        case EX_PERFORM:
            buf_puts(b, "(perform (");
            buf_puts(b, e->as.perform_.perform->effect_name->name);
            for (uint32_t i = 0; i < e->as.perform_.perform->n_args; i++) {
                buf_putc(b, ' ');
                expr_print(b, e->as.perform_.perform->args[i]);
            }
            buf_puts(b, "))");
            break;
        case EX_HANDLE:
            buf_puts(b, "(handle ");
            expr_print(b, e->as.handle_.handle->body);
            for (uint8_t i = 0; i < e->as.handle_.handle->n_cases; i++) {
                buf_puts(b, " (");
                buf_puts(b, e->as.handle_.handle->cases[i].effect_name->name);
                buf_puts(b, " [");
                for (uint32_t j = 0; j < e->as.handle_.handle->cases[i].n_params; j++) {
                    if (j > 0) buf_putc(b, ' ');
                    buf_puts(b, e->as.handle_.handle->cases[i].param_names[j]->name);
                }
                buf_puts(b, "] ");
                buf_puts(b, e->as.handle_.handle->cases[i].k_name->name);
                buf_puts(b, ") ");
                expr_print(b, e->as.handle_.handle->cases[i].body);
            }
            buf_putc(b, ')');
            break;
        case EX_HANDLER_LIT:
            buf_puts(b, "(handler ...)");
            break;
        case EX_WITH_HANDLER:
            buf_puts(b, "(with-handler ");
            expr_print(b, e->as.with_handler_.handler);
            buf_putc(b, ' ');
            expr_print(b, e->as.with_handler_.body);
            buf_putc(b, ')');
            break;
        case EX_COMPOSE_HANDLERS:
            buf_puts(b, "(compose-handlers ");
            expr_print(b, e->as.compose_handlers_.h1);
            buf_putc(b, ' ');
            expr_print(b, e->as.compose_handlers_.h2);
            buf_putc(b, ')');
            break;
        case EX_RESUME:
            buf_puts(b, "(resume ");
            expr_print(b, e->as.resume_.resume->k);
            buf_puts(b, " ");
            expr_print(b, e->as.resume_.resume->value);
            buf_putc(b, ')');
            break;
        case EX_DISCONTINUE:
            buf_puts(b, "(discontinue ");
            expr_print(b, e->as.discontinue_.discontinue->k);
            buf_puts(b, " ");
            expr_print(b, e->as.discontinue_.discontinue->exception);
            buf_putc(b, ')');
            break;
        case EX_MAKE_STRUCT:
            buf_puts(b, "<make-struct>");
            break;
        case EX_GET_FIELD:
            buf_puts(b, "<get-field>");
            break;
        case EX_SET_FIELD:
            buf_puts(b, "<set-field>");
            break;
        /* Phase 20: Software Transactional Memory */
        case EX_STM:
            buf_puts(b, "<stm>");
            break;
        case EX_ATOMICALLY:
            buf_puts(b, "<atomically>");
            break;
        case EX_RETRY:
            buf_puts(b, "<retry>");
            break;
        case EX_CHECK:
            buf_puts(b, "<check>");
            break;
        case EX_OR_ELSE:
            buf_puts(b, "<or-else>");
            break;
        case EX_TVAR_NEW:
            buf_puts(b, "<TVar::new>");
            break;
        case EX_TVAR_READ:
            buf_puts(b, "<TVar::read>");
            break;
        case EX_TVAR_WRITE:
            buf_puts(b, "<TVar::write>");
            break;
        case EX_TVAR_MODIFY:
            buf_puts(b, "<TVar::modify>");
            break;
        case EX_TVAR_SWAP:
            buf_puts(b, "<TVar::swap>");
            break;
        case EX_TVAR_CAS:
            buf_puts(b, "<TVar::cas>");
            break;
        /* Phase M0: Module system */
        case EX_DEFMODULE:
            buf_printf(b, "<defmodule %s>", e->as.defmodule_.mod->name->name);
            break;
        /* Phase 21: Serializable continuations */
        case EX_SERIAL_RESET:
            buf_puts(b, "(serial-reset ");
            expr_print(b, e->as.serial_reset_.body);
            buf_putc(b, ')');
            break;
        case EX_SERIAL_SHIFT:
            buf_puts(b, "(serial-shift ");
            expr_print(b, e->as.serial_shift_.k_fn);
            buf_putc(b, ' ');
            expr_print(b, e->as.serial_shift_.body);
            buf_putc(b, ')');
            break;
        case EX_SET_LIT:
            buf_puts(b, "#s(");
            for (uint32_t i = 0; i < e->as.set_lit_.n; i++) {
                if (i > 0) buf_putc(b, ' ');
                expr_print(b, e->as.set_lit_.items[i]);
            }
            buf_putc(b, ')');
            break;
        /* EX_CAST handled above with full type info */
        /* Phase HRT1 */
        case EX_POLY_WRAP:
            buf_puts(b, "<poly-wrap>");
            break;
        case EX_FN_TO_FAT:
            buf_puts(b, "(fn->fat ");
            expr_print(b, e->as.fn_to_fat_.inner);
            buf_putc(b, ')');
            break;
        case EX_POLY_TO_FAT:
            buf_puts(b, "(poly->fat ");
            expr_print(b, e->as.poly_to_fat_.inner);
            buf_putc(b, ')');
            break;
        case EX_ASCRIBE:
            buf_puts(b, "(:: ");
            expr_print(b, e->as.ascribe_.inner);
            buf_putc(b, ')');
            break;
        /* Phase HRT2 */
        case EX_EXISTS_PACK:
            buf_puts(b, "(pack ");
            expr_print(b, e->as.exists_pack_.value);
            buf_putc(b, ')');
            break;
        case EX_EXISTS_OPEN:
            buf_puts(b, "(open ");
            expr_print(b, e->as.exists_open_.packed);
            buf_puts(b, " [_ ");
            buf_write(b, e->as.exists_open_.var_binding->name->name,
                         e->as.exists_open_.var_binding->name->len);
            buf_puts(b, "] ");
            expr_print(b, e->as.exists_open_.body);
            buf_putc(b, ')');
            break;
        case EX_EXISTS_DISPATCH:
            buf_printf(b, "(.%s",
                       e->as.exists_dispatch_.typeclass
                           ->methods[e->as.exists_dispatch_.method_idx].name->name);
            for (uint32_t i = 0; i < e->as.exists_dispatch_.n_args; i++) {
                buf_putc(b, ' ');
                expr_print(b, e->as.exists_dispatch_.args[i]);
            }
            buf_putc(b, ')');
            break;
        /* Phase G0: ADTs */
        case EX_DEFDATA:
            buf_printf(b, "(defdata %s)", e->as.defdata_.def->name);
            break;
        /* Phase G1: GADTs */
        case EX_DEFGADT:
            buf_printf(b, "(defgadt %s)", e->as.defgadt_.def->name);
            break;
        case EX_MATCH:
            buf_puts(b, "(match ");
            expr_print(b, e->as.match_.scrutinee);
            buf_putc(b, ')');
            break;
        /* IT4: Tagged union injection */
        case EX_UNION_INJECT:
            buf_printf(b, "(union-inject %lld ", (long long)e->as.union_inject_.tag_idx);
            expr_print(b, e->as.union_inject_.value);
            buf_putc(b, ')');
            break;
        /* IT4 gradual typing */
        case EX_ANY_TYPE_OF:
            buf_puts(b, "(type-of ");
            expr_print(b, e->as.any_type_of_.value);
            buf_putc(b, ')');
            break;
        case EX_ANY_CAST:
            buf_printf(b, "(cast %s ", type_name(e->type));
            expr_print(b, e->as.any_cast_.value);
            buf_putc(b, ')');
            break;
        case EX_ANY_IS:
            buf_puts(b, "(is? ");
            expr_print(b, e->as.any_is_.value);
            buf_printf(b, " tag=%lld)", (long long)e->as.any_is_.test_tag);
            break;
        /* GF1: Generator forms */
        case EX_GEN:
            buf_puts(b, "(gen [])");
            break;
        case EX_YIELD:
            buf_puts(b, "(yield ");
            if (e->as.yield_.value) expr_print(b, e->as.yield_.value);
            buf_putc(b, ')');
            break;
        case EX_GEN_NEXT:
            buf_puts(b, "(gen-next ");
            if (e->as.gen_next_.gen_expr) expr_print(b, e->as.gen_next_.gen_expr);
            buf_putc(b, ')');
            break;
        case EX_GEN_UNWRAP:
            buf_puts(b, "(gen-unwrap ");
            if (e->as.gen_unwrap_.ptr_expr) expr_print(b, e->as.gen_unwrap_.ptr_expr);
            buf_putc(b, ')');
            break;
        case EX_GEN_DONE:
            buf_puts(b, "(gen-done? ");
            if (e->as.gen_done_.gen_expr) expr_print(b, e->as.gen_done_.gen_expr);
            buf_putc(b, ')');
            break;
        case EX_CONS_LIST:
            buf_puts(b, "(cons-list");
            for (uint32_t i = 0; i < e->as.cons_list_.n; i++) {
                buf_putc(b, ' ');
                expr_print(b, e->as.cons_list_.items[i]);
            }
            buf_putc(b, ')');
            break;
    }
}
