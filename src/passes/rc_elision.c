#include "rc_elision.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* -----------------------------------------------------------------------
 * Forward declarations
 * --------------------------------------------------------------------- */
static void analyze_expr(Expr *e);

typedef enum ElideDecision {
    ELIDE_OK = 0,
    ELIDE_REJECT_MULTI_USE,
    ELIDE_REJECT_NON_DROP_USE,
    ELIDE_REJECT_USE_NOT_FOUND,
    ELIDE_REJECT_BARRIER_BEFORE_USE,
} ElideDecision;

static bool rc_elision_debug_enabled(void) {
#if TUR_DEBUG
    static int cached = -1;
    if (cached < 0) {
        const char *env = getenv("TUR_RC_ELISION_DEBUG");
        cached = (env && *env && strcmp(env, "0") != 0) ? 1 : 0;
    }
    return cached == 1;
#else
    return false;
#endif
}

static const char *decision_str(ElideDecision d) {
    switch (d) {
        case ELIDE_OK:
            return "accepted (single-use clone with no prior barrier)";
        case ELIDE_REJECT_MULTI_USE:
            return "rejected: clone binding has != 1 use";
        case ELIDE_REJECT_NON_DROP_USE:
            return "rejected: single use is not rc/drop (could change observable semantics)";
        case ELIDE_REJECT_USE_NOT_FOUND:
            return "rejected: single-use index not found in do-block";
        case ELIDE_REJECT_BARRIER_BEFORE_USE:
            return "rejected: barrier appears before use (call/builtin/loop/escape)";
        default:
            return "rejected: unknown";
    }
}

/* -----------------------------------------------------------------------
 * count_uses
 *
 * Count the number of EX_VAR nodes in 'e' that reference binding 'b'.
 * We deliberately cross function/closure boundaries so that captured
 * uses are also counted — which prevents unsafe elision when a binding
 * escapes into a closure or defer body.
 * --------------------------------------------------------------------- */
static int count_uses(const Expr *e, const Binding *b) {
    if (!e) return 0;
    int n = 0;
    switch (e->kind) {
        case EX_VAR:
            return (e->as.var.binding == b) ? 1 : 0;

        case EX_LET: {
            for (uint32_t i = 0; i < e->as.let_.n; i++)
                n += count_uses(e->as.let_.bindings[i].init, b);
            n += count_uses(e->as.let_.body, b);
            return n;
        }
        case EX_IF:
            n += count_uses(e->as.if_.cond, b);
            n += count_uses(e->as.if_.then_, b);
            n += count_uses(e->as.if_.else_or_null, b);
            return n;
        case EX_DO:
            for (uint32_t i = 0; i < e->as.do_.n; i++)
                n += count_uses(e->as.do_.items[i], b);
            return n;
        case EX_WHILE:
            n += count_uses(e->as.while_.cond, b);
            n += count_uses(e->as.while_.body, b);
            return n;
        case EX_SET:
            return count_uses(e->as.set_.value, b);
        case EX_DEF:
            return count_uses(e->as.def_.init, b);
        case EX_BUILTIN:
            for (uint32_t i = 0; i < e->as.builtin.n; i++)
                n += count_uses(e->as.builtin.args[i], b);
            return n;
        case EX_CALL:
            for (uint32_t i = 0; i < e->as.call_.n_args; i++)
                n += count_uses(e->as.call_.args[i], b);
            return n;
        case EX_FN_DEF:
            return count_uses(e->as.fn_def_.fn->body, b);
        case EX_FN:
            return count_uses(e->as.fn_.fn->body, b);
        case EX_CLOSURE: {
            /* Closure captures are uses of the outer binding */
            for (uint8_t i = 0; i < e->as.closure_.closure->n_captures; i++) {
                if (e->as.closure_.closure->captures[i] == b) n++;
            }
            return n + count_uses(e->as.closure_.closure->fn->body, b);
        }
        case EX_RETURN:
            return count_uses(e->as.return_.value, b);
        case EX_DEFER:
            for (uint8_t i = 0; i < e->as.defer_.n_captures; i++) {
                if (e->as.defer_.captures[i] == b) n++;
            }
            n += count_uses(e->as.defer_.body, b);
            return n;
        case EX_REF:      return count_uses(e->as.ref_.expr, b);
        case EX_DEREF:    return count_uses(e->as.deref_.expr, b);
        case EX_RC_OF:    return count_uses(e->as.rc_of_.expr, b);
        case EX_RC_CLONE: return count_uses(e->as.rc_clone_.expr, b);
        case EX_RC_DROP:  return count_uses(e->as.rc_drop_.expr, b);
        case EX_RC_PTR:   return count_uses(e->as.rc_ptr_.expr, b);
        case EX_RC_COUNT: return count_uses(e->as.rc_count_.expr, b);
        case EX_RC_FROM_REF:  return count_uses(e->as.rc_from_ref_.expr, b);
        case EX_REF_FROM_RC:  return count_uses(e->as.ref_from_rc_.expr, b);
        case EX_WEAK:         return count_uses(e->as.weak_.expr, b);
        case EX_WEAK_PRED:    return count_uses(e->as.weak_pred_.expr, b);
        case EX_REF_PRED:     return count_uses(e->as.ref_pred_.expr, b);
        case EX_BORROW_IMMUT: return count_uses(e->as.borrow_immut_.expr, b);
        case EX_BORROW_MUT:   return count_uses(e->as.borrow_mut_.expr, b);
        case EX_SET_DEREF:    return count_uses(e->as.set_deref_.ref, b)
                                   + count_uses(e->as.set_deref_.value, b);
        case EX_PANIC:        return count_uses(e->as.panic_.payload, b);
        case EX_PANIC_WITH:   return count_uses(e->as.panic_with_.payload, b);
        case EX_CATCH_UNWIND: return count_uses(e->as.catch_unwind_.thunk, b);
        case EX_CATCH_PANIC_OF: return count_uses(e->as.catch_panic_of_.thunk, b);
        case EX_PANIC_PAYLOAD_TYPE:
        case EX_PANIC_PAYLOAD_VALUE:
        case EX_PANIC_PAYLOAD_FILE:
        case EX_PANIC_PAYLOAD_LINE:
            return count_uses(e->as.panic_payload_type_.payload, b);
        case EX_PANIC_PAYLOAD_DOWNS:
            return count_uses(e->as.panic_payload_downs_.payload, b);
        case EX_RESET:  return count_uses(e->as.reset_.body, b);
        case EX_SHIFT:
            n += count_uses(e->as.shift_.k_fn, b);
            n += count_uses(e->as.shift_.body, b);
            return n;
        case EX_SHIFT0:
            n += count_uses(e->as.shift0_.k_fn, b);
            n += count_uses(e->as.shift0_.body, b);
            return n;
        case EX_PERFORM:
            for (uint32_t i = 0; i < e->as.perform_.perform->n_args; i++)
                n += count_uses(e->as.perform_.perform->args[i], b);
            return n;
        case EX_HANDLE: {
            n += count_uses(e->as.handle_.handle->body, b);
            for (uint8_t i = 0; i < e->as.handle_.handle->n_cases; i++)
                n += count_uses(e->as.handle_.handle->cases[i].body, b);
            return n;
        }
        case EX_RESUME:
            n += count_uses(e->as.resume_.resume->k, b);
            n += count_uses(e->as.resume_.resume->value, b);
            return n;
        case EX_DISCONTINUE:
            n += count_uses(e->as.discontinue_.discontinue->k, b);
            n += count_uses(e->as.discontinue_.discontinue->exception, b);
            return n;
        case EX_INLINE_C: {
            for (uint8_t i = 0; i < e->as.inline_c_.inline_c->n_captures; i++) {
                if (e->as.inline_c_.inline_c->captures[i] == b) n++;
            }
            return n;
        }
        case EX_MAKE_STRUCT: {
            for (uint32_t i = 0; i < e->as.make_struct_.n_fields; i++) {
                n += count_uses(e->as.make_struct_.field_values[i], b);
            }
            return n;
        }
        case EX_SET_LIT: {
            for (uint32_t i = 0; i < e->as.set_lit_.n; i++) {
                n += count_uses(e->as.set_lit_.items[i], b);
            }
            return n;
        }
        /* Leaf / no-child nodes */
        default:
            return 0;
    }
}

/* -----------------------------------------------------------------------
 * has_barrier
 *
 * Returns true if expression 'e' (or any sub-expression) could cause a
 * side-effect that invalidates last-use assumptions: function calls,
 * builtins with side effects, loops, closures, defer, throw, try.
 *
 * This is conservative: any EX_CALL or EX_BUILTIN is treated as a barrier
 * regardless of purity, to avoid unsound elision.
 * --------------------------------------------------------------------- */
static bool has_barrier(const Expr *e) {
    if (!e) return false;
    switch (e->kind) {
        /* Direct barrier nodes */
        case EX_CALL:
        case EX_BUILTIN:
        case EX_WHILE:
        case EX_PANIC:
        case EX_PANIC_WITH:
        case EX_CATCH_UNWIND:
        case EX_CATCH_PANIC_OF:
        case EX_CLOSURE:
        case EX_DEFER:
        case EX_RC_COUNT:
        case EX_REF_FROM_RC:
        case EX_RESET:
        case EX_SHIFT:
        case EX_SHIFT0:
        case EX_PERFORM:
        case EX_HANDLE:
            return true;

        /* Transparent / pass-through to children */
        case EX_LET: {
            for (uint32_t i = 0; i < e->as.let_.n; i++) {
                if (has_barrier(e->as.let_.bindings[i].init)) return true;
            }
            return has_barrier(e->as.let_.body);
        }
        case EX_IF:
            return has_barrier(e->as.if_.cond) ||
                   has_barrier(e->as.if_.then_) ||
                   has_barrier(e->as.if_.else_or_null);
        case EX_DO:
            for (uint32_t i = 0; i < e->as.do_.n; i++) {
                if (has_barrier(e->as.do_.items[i])) return true;
            }
            return false;
        case EX_SET:
            return has_barrier(e->as.set_.value);
        case EX_DEF:
            return has_barrier(e->as.def_.init);
        case EX_RETURN:
            return has_barrier(e->as.return_.value);
        case EX_REF:           return has_barrier(e->as.ref_.expr);
        case EX_DEREF:         return has_barrier(e->as.deref_.expr);
        case EX_RC_OF:         return has_barrier(e->as.rc_of_.expr);
        case EX_RC_CLONE:      return has_barrier(e->as.rc_clone_.expr);
        case EX_RC_DROP:       return has_barrier(e->as.rc_drop_.expr);
        case EX_RC_PTR:        return has_barrier(e->as.rc_ptr_.expr);
        case EX_RC_FROM_REF:   return has_barrier(e->as.rc_from_ref_.expr);
        case EX_WEAK:          return has_barrier(e->as.weak_.expr);
        case EX_WEAK_PRED:     return has_barrier(e->as.weak_pred_.expr);
        case EX_REF_PRED:      return has_barrier(e->as.ref_pred_.expr);
        case EX_BORROW_IMMUT:  return has_barrier(e->as.borrow_immut_.expr);
        case EX_BORROW_MUT:    return has_barrier(e->as.borrow_mut_.expr);
        case EX_SET_DEREF:     return has_barrier(e->as.set_deref_.ref)
                                   || has_barrier(e->as.set_deref_.value);
        case EX_MAKE_STRUCT: {
            for (uint32_t i = 0; i < e->as.make_struct_.n_fields; i++) {
                if (has_barrier(e->as.make_struct_.field_values[i])) return true;
            }
            return false;
        }
        case EX_SET_LIT: {
            for (uint32_t i = 0; i < e->as.set_lit_.n; i++) {
                if (has_barrier(e->as.set_lit_.items[i])) return true;
            }
            return false;
        }
        /* Leaf nodes */
        default:
            return false;
    }
}

/* Count occurrences of (rc/drop <binding>) for binding b. */
static int count_drop_uses(const Expr *e, const Binding *b) {
    if (!e) return 0;
    int n = 0;
    switch (e->kind) {
        case EX_RC_DROP:
            if (e->as.rc_drop_.expr &&
                e->as.rc_drop_.expr->kind == EX_VAR &&
                e->as.rc_drop_.expr->as.var.binding == b) {
                n++;
            }
            return n + count_drop_uses(e->as.rc_drop_.expr, b);
        case EX_LET:
            for (uint32_t i = 0; i < e->as.let_.n; i++)
                n += count_drop_uses(e->as.let_.bindings[i].init, b);
            n += count_drop_uses(e->as.let_.body, b);
            return n;
        case EX_IF:
            n += count_drop_uses(e->as.if_.cond, b);
            n += count_drop_uses(e->as.if_.then_, b);
            n += count_drop_uses(e->as.if_.else_or_null, b);
            return n;
        case EX_DO:
            for (uint32_t i = 0; i < e->as.do_.n; i++)
                n += count_drop_uses(e->as.do_.items[i], b);
            return n;
        case EX_WHILE:
            n += count_drop_uses(e->as.while_.cond, b);
            n += count_drop_uses(e->as.while_.body, b);
            return n;
        case EX_SET:    return count_drop_uses(e->as.set_.value, b);
        case EX_DEF:    return count_drop_uses(e->as.def_.init, b);
        case EX_RETURN: return count_drop_uses(e->as.return_.value, b);
        case EX_BUILTIN:
            for (uint32_t i = 0; i < e->as.builtin.n; i++)
                n += count_drop_uses(e->as.builtin.args[i], b);
            return n;
        case EX_CALL:
            for (uint32_t i = 0; i < e->as.call_.n_args; i++)
                n += count_drop_uses(e->as.call_.args[i], b);
            return n;
        default:
            return 0;
    }
}

/* -----------------------------------------------------------------------
 * mark_drop_elision
 *
 * Walk expression 'e' looking for (EX_RC_DROP (EX_VAR b)) patterns and
 * mark them elide=true.  Called when the corresponding EX_RC_CLONE for
 * binding 'b' has already been marked for elision.
 * --------------------------------------------------------------------- */
static void mark_drop_elision(Expr *e, const Binding *b) {
    if (!e) return;
    switch (e->kind) {
        case EX_RC_DROP:
            if (e->as.rc_drop_.expr &&
                e->as.rc_drop_.expr->kind == EX_VAR &&
                e->as.rc_drop_.expr->as.var.binding == b) {
                e->as.rc_drop_.elide = true;
            } else {
                mark_drop_elision(e->as.rc_drop_.expr, b);
            }
            return;
        case EX_LET: {
            for (uint32_t i = 0; i < e->as.let_.n; i++)
                mark_drop_elision(e->as.let_.bindings[i].init, b);
            mark_drop_elision(e->as.let_.body, b);
            return;
        }
        case EX_IF:
            mark_drop_elision(e->as.if_.cond, b);
            mark_drop_elision(e->as.if_.then_, b);
            mark_drop_elision(e->as.if_.else_or_null, b);
            return;
        case EX_DO:
            for (uint32_t i = 0; i < e->as.do_.n; i++)
                mark_drop_elision(e->as.do_.items[i], b);
            return;
        case EX_WHILE:
            mark_drop_elision(e->as.while_.cond, b);
            mark_drop_elision(e->as.while_.body, b);
            return;
        case EX_SET:   mark_drop_elision(e->as.set_.value, b);   return;
        case EX_DEF:   mark_drop_elision(e->as.def_.init, b);    return;
        case EX_RETURN: mark_drop_elision(e->as.return_.value, b); return;
        case EX_BUILTIN:
            for (uint32_t i = 0; i < e->as.builtin.n; i++)
                mark_drop_elision(e->as.builtin.args[i], b);
            return;
        case EX_CALL:
            for (uint32_t i = 0; i < e->as.call_.n_args; i++)
                mark_drop_elision(e->as.call_.args[i], b);
            return;
        default:
            return;
    }
}

/* -----------------------------------------------------------------------
 * is_elision_eligible
 *
 * Given the let body 'body' and a binding 'b' whose init is EX_RC_CLONE,
 * return true if:
 *   (a) 'b' is used exactly once in 'body', AND
 *   (b) no barrier appears before the first use when 'body' is a do-block.
 *
 * If 'body' is a single expression (not a do-block), condition (b) is
 * trivially satisfied since there are no prior statements.
 * --------------------------------------------------------------------- */
static ElideDecision is_elision_eligible(const Expr *body, const Binding *b) {
    /* Condition (a): exactly one use total */
    if (count_uses(body, b) != 1) return ELIDE_REJECT_MULTI_USE;

    /* Condition (a.1): the single use must be an explicit rc/drop.
     * This keeps elision semantics-preserving by only removing a no-op pair
     * (increment followed by decrement) with no observable intermediate use. */
    if (count_drop_uses(body, b) != 1) return ELIDE_REJECT_NON_DROP_USE;

    /* Condition (b): no barrier before the single use in a do-block */
    if (!body || body->kind != EX_DO) {
        /* Single-expression let body: require a direct (rc/drop <binding>) use.
         * Reject nested/conditional contexts (e.g. if/while/try), which are harder
         * to prove equivalent for clone/drop elision. */
        if (body->kind == EX_RC_DROP &&
            body->as.rc_drop_.expr &&
            body->as.rc_drop_.expr->kind == EX_VAR &&
            body->as.rc_drop_.expr->as.var.binding == b) {
            return ELIDE_OK;
        }
        return ELIDE_REJECT_NON_DROP_USE;
    }

    /* do-block: find the statement index that contains the use */
    int use_idx = -1;
    for (uint32_t i = 0; i < body->as.do_.n; i++) {
        if (count_uses(body->as.do_.items[i], b) > 0) {
            use_idx = (int)i;
            break;
        }
    }
    if (use_idx < 0) return ELIDE_REJECT_USE_NOT_FOUND;

        /* Require the use statement itself to be a direct rc/drop of the cloned
         * binding. This rejects conditional/nested drops inside if/while/try, which
         * could otherwise appear as single-use but are not guaranteed to execute in
         * an unconditional straight-line way. */
        const Expr *use_stmt = body->as.do_.items[use_idx];
        if (!(use_stmt &&
                    use_stmt->kind == EX_RC_DROP &&
                    use_stmt->as.rc_drop_.expr &&
                    use_stmt->as.rc_drop_.expr->kind == EX_VAR &&
                    use_stmt->as.rc_drop_.expr->as.var.binding == b)) {
                return ELIDE_REJECT_NON_DROP_USE;
        }

    /* Check if any statement before use_idx has a barrier */
    for (int i = 0; i < use_idx; i++) {
        if (has_barrier(body->as.do_.items[i])) return ELIDE_REJECT_BARRIER_BEFORE_USE;
    }
    return ELIDE_OK;
}

/* -----------------------------------------------------------------------
 * analyze_let
 *
 * Check each binding in an EX_LET node.  If the binding's init is an
 * EX_RC_CLONE and the binding is eligible, mark clone + any matching drop.
 * Then recurse into let inits and body.
 * --------------------------------------------------------------------- */
static void analyze_let(Expr *e) {
    if (!e || e->kind != EX_LET) return;

    for (uint32_t i = 0; i < e->as.let_.n; i++) {
        LetBinding *lb = &e->as.let_.bindings[i];
        if (!lb->init || lb->init->kind != EX_RC_CLONE) continue;

        const Binding *b = lb->binding;
        ElideDecision decision = is_elision_eligible(e->as.let_.body, b);
        if (decision == ELIDE_OK) {
            /* Mark the clone for elision */
            lb->init->as.rc_clone_.elide = true;
            /* Also mark any corresponding explicit rc/drop for the same binding */
            mark_drop_elision(e->as.let_.body, b);
        }

        if (rc_elision_debug_enabled()) {
            fprintf(stderr,
                    "rc-elision: %s binding=%s id=%u\n",
                    decision_str(decision),
                    b && b->name ? b->name->name : "<anon>",
                    b ? b->id : 0u);
        }

        /* Recursively analyze the clone's source expression */
        analyze_expr(lb->init->as.rc_clone_.expr);
    }

    /* Recurse into the let body */
    analyze_expr(e->as.let_.body);
}

/* -----------------------------------------------------------------------
 * analyze_expr
 *
 * Walk the expression tree, calling analyze_let for each EX_LET node and
 * recursing into all reachable sub-expressions (including function bodies).
 * --------------------------------------------------------------------- */
static void analyze_expr(Expr *e) {
    if (!e) return;
    switch (e->kind) {
        case EX_LET:
            analyze_let(e);
            return;  /* analyze_let recurses */
        case EX_IF:
            analyze_expr(e->as.if_.cond);
            analyze_expr(e->as.if_.then_);
            analyze_expr(e->as.if_.else_or_null);
            return;
        case EX_DO:
            for (uint32_t i = 0; i < e->as.do_.n; i++)
                analyze_expr(e->as.do_.items[i]);
            return;
        case EX_WHILE:
            analyze_expr(e->as.while_.cond);
            analyze_expr(e->as.while_.body);
            return;
        case EX_SET:    analyze_expr(e->as.set_.value);  return;
        case EX_DEF:    analyze_expr(e->as.def_.init);   return;
        case EX_RETURN: analyze_expr(e->as.return_.value); return;
        case EX_BUILTIN:
            for (uint32_t i = 0; i < e->as.builtin.n; i++)
                analyze_expr(e->as.builtin.args[i]);
            return;
        case EX_CALL:
            for (uint32_t i = 0; i < e->as.call_.n_args; i++)
                analyze_expr(e->as.call_.args[i]);
            return;
        case EX_FN_DEF:  analyze_expr(e->as.fn_def_.fn->body);  return;
        case EX_FN:      analyze_expr(e->as.fn_.fn->body);       return;
        case EX_CLOSURE: analyze_expr(e->as.closure_.closure->fn->body); return;
        case EX_DEFER:   analyze_expr(e->as.defer_.body); return;
        case EX_REF:           analyze_expr(e->as.ref_.expr);           return;
        case EX_DEREF:         analyze_expr(e->as.deref_.expr);         return;
        case EX_RC_OF:         analyze_expr(e->as.rc_of_.expr);         return;
        case EX_RC_CLONE:      analyze_expr(e->as.rc_clone_.expr);      return;
        case EX_RC_DROP:       analyze_expr(e->as.rc_drop_.expr);       return;
        case EX_RC_PTR:        analyze_expr(e->as.rc_ptr_.expr);        return;
        case EX_RC_COUNT:      analyze_expr(e->as.rc_count_.expr);      return;
        case EX_RC_FROM_REF:   analyze_expr(e->as.rc_from_ref_.expr);   return;
        case EX_REF_FROM_RC:   analyze_expr(e->as.ref_from_rc_.expr);   return;
        case EX_WEAK:          analyze_expr(e->as.weak_.expr);          return;
        case EX_WEAK_PRED:     analyze_expr(e->as.weak_pred_.expr);     return;
        case EX_REF_PRED:      analyze_expr(e->as.ref_pred_.expr);      return;
        case EX_BORROW_IMMUT:  analyze_expr(e->as.borrow_immut_.expr);  return;
        case EX_BORROW_MUT:    analyze_expr(e->as.borrow_mut_.expr);    return;
        case EX_SET_DEREF:     analyze_expr(e->as.set_deref_.ref);
                               analyze_expr(e->as.set_deref_.value);    return;
        case EX_PANIC:         analyze_expr(e->as.panic_.payload);      return;
        case EX_PANIC_WITH:    analyze_expr(e->as.panic_with_.payload); return;
        case EX_CATCH_UNWIND:  analyze_expr(e->as.catch_unwind_.thunk); return;
        case EX_CATCH_PANIC_OF: analyze_expr(e->as.catch_panic_of_.thunk); return;
        case EX_PANIC_PAYLOAD_TYPE:
        case EX_PANIC_PAYLOAD_VALUE:
        case EX_PANIC_PAYLOAD_FILE:
        case EX_PANIC_PAYLOAD_LINE:
            analyze_expr(e->as.panic_payload_type_.payload); return;
        case EX_PANIC_PAYLOAD_DOWNS:
            analyze_expr(e->as.panic_payload_downs_.payload); return;
        case EX_RESET:  analyze_expr(e->as.reset_.body); return;
        case EX_SHIFT:
            analyze_expr(e->as.shift_.k_fn);
            analyze_expr(e->as.shift_.body);
            return;
        case EX_SHIFT0:
            analyze_expr(e->as.shift0_.k_fn);
            analyze_expr(e->as.shift0_.body);
            return;
        case EX_PERFORM:
            for (uint32_t i = 0; i < e->as.perform_.perform->n_args; i++)
                analyze_expr(e->as.perform_.perform->args[i]);
            return;
        case EX_HANDLE: {
            analyze_expr(e->as.handle_.handle->body);
            for (uint8_t i = 0; i < e->as.handle_.handle->n_cases; i++)
                analyze_expr(e->as.handle_.handle->cases[i].body);
            return;
        }
        case EX_RESUME:
            analyze_expr(e->as.resume_.resume->k);
            analyze_expr(e->as.resume_.resume->value);
            return;
        case EX_DISCONTINUE:
            analyze_expr(e->as.discontinue_.discontinue->k);
            analyze_expr(e->as.discontinue_.discontinue->exception);
            return;
        case EX_MAKE_STRUCT:
            for (uint32_t i = 0; i < e->as.make_struct_.n_fields; i++)
                analyze_expr(e->as.make_struct_.field_values[i]);
            return;
        case EX_SET_LIT:
            for (uint32_t i = 0; i < e->as.set_lit_.n; i++)
                analyze_expr(e->as.set_lit_.items[i]);
            return;
        /* Leaf nodes: nothing to recurse */
        default:
            return;
    }
}

/* -----------------------------------------------------------------------
 * Public API
 * --------------------------------------------------------------------- */

/*
 * Analyze the body of a single function definition for last-use elision
 * opportunities.  Marks eligible EX_RC_CLONE and EX_RC_DROP nodes with
 * elide=true.  The emitter checks these flags before generating
 * rc_strong_increment / rc_strong_decrement + rc_free_queue_drain calls.
 */
void rc_elision_analyze_fn(Expr *fn_body) {
    analyze_expr(fn_body);
}
