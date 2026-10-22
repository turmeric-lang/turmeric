#include "borrow_check.h"

#include <stdio.h>

#include "diag.h"
#include "types.h"
#include "lifetime_elision.h"

void borrow_check_ctx_init(BorrowCheckCtx *ctx, const LifetimeContext *lifetime_ctx, const struct Scope *scope) {
    ctx->lifetime_ctx = lifetime_ctx;
    ctx->scope = scope;
    ctx->error_count = 0;
    ctx->handler_case = NULL;
    ctx->only_handler_captures = false;
}

/* Check if a type is a borrow type (&T or &mut T) */
static bool type_is_borrow(Type t) {
    return t.kind == TY_REF_IMMUT || t.kind == TY_REF_MUT;
}

/* Check if a type is a reference type (ref<T>, rc<T>, weak<T>) */
static bool type_is_reference(Type t) __attribute__((unused));
static bool type_is_reference(Type t) {
    return t.kind == TY_REF || t.kind == TY_RC || t.kind == TY_WEAK;
}

/* Check if an expression is a borrow expression */
static bool expr_is_borrow(const Expr *e) {
    return e->kind == EX_BORROW_IMMUT || e->kind == EX_BORROW_MUT;
}

/* Recursively check an expression for borrow violations */
static bool borrow_check_expr_recursive(BorrowCheckCtx *ctx, const Expr *e);

/* Check a function call for borrow validity */
static bool borrow_check_call(BorrowCheckCtx *ctx, const Expr *e) {
    /* For now, we just validate the types - full inter-procedural checking deferred */
    
    /* Check each argument */
    for (uint32_t i = 0; i < e->as.call_.n_args; i++) {
        const Expr *arg = e->as.call_.args[i];
        if (!borrow_check_expr_recursive(ctx, arg)) {
            return false;
        }
        
        /* Check if argument is a borrow and the function accepts it */
        if (expr_is_borrow(arg) || type_is_borrow(arg->type)) {
            /* The function should have a parameter type that accepts borrows */
            /* This validation is deferred until full type system integration */
        }
    }
    
    return true;
}

/* Global flag to enable/disable borrow checking (default: disabled for Phase 14) */
static bool borrow_check_enabled = false;

void borrow_check_set_enabled(bool enabled) {
    borrow_check_enabled = enabled;
}

bool borrow_check_is_enabled(void) {
    return borrow_check_enabled;
}

/* Run borrow checker on an entire program */
bool borrow_check_program(const Expr *program) {
    if (!borrow_check_is_enabled()) {
        return true;  /* Borrow checking disabled - skip */
    }
    
    if (program->kind != EX_PROGRAM) {
        return true;  /* Not a program - skip */
    }
    
    /* Create a borrow check context */
    BorrowCheckCtx ctx;
    borrow_check_ctx_init(&ctx, NULL, NULL);
    
    /* Check each top-level expression */
    for (uint32_t i = 0; i < program->as.program.n; i++) {
        const Expr *item = program->as.program.items[i];
        if (!borrow_check_expr(&ctx, item)) {
            return false;
        }
    }
    
    return ctx.error_count == 0;
}

/* Phase P19-7: Always-on check for borrow captures in effect handler case
 * bodies.  Handler case bodies are emitted as top-level C static functions
 * with no access to the enclosing stack frame, so any borrow-typed variable
 * from the outer scope referenced inside a handler body is an error.
 * This function runs unconditionally (not gated by borrow_check_set_enabled). */
bool borrow_check_effect_handler_captures(const Expr *program) {
    if (!program || program->kind != EX_PROGRAM) {
        return true;
    }
    BorrowCheckCtx ctx;
    borrow_check_ctx_init(&ctx, NULL, NULL);
    ctx.only_handler_captures = true;
    for (uint32_t i = 0; i < program->as.program.n; i++) {
        borrow_check_expr_recursive(&ctx, program->as.program.items[i]);
    }
    return ctx.error_count == 0;
}

/* TY4.1/TY4.2: always-on lifetime pass.  For every top-level function, run
 * lifetime elision over its signature (binding fresh lifetimes onto borrow
 * params and flowing them to elided borrow returns) and reject any function
 * whose resulting outlives-constraint graph contains a cycle.  Today the
 * surface language attaches no `'a` annotations to types, so elision typically
 * finds nothing; the pass is wired and ready so that lifetime-annotated
 * signatures are checked the moment that syntax lands.  The elision + solver
 * logic is unit-tested directly in tests/lifetime_unit.c. */
/* LS3: a borrow Type that is a nested borrow carries its inner lifetime in
 * lifetimes[1] (the head's own lifetime is lifetimes[0]).  &'a &'b T means the
 * outer reference ('a) points at an inner reference ('b), so 'b must outlive 'a:
 * record that edge ('b: 'a).  Derived purely from the Type so it is robust even
 * when a FnDef's parse-time lifetime context was never populated. */
static void collect_borrow_constraints(LifetimeContext *lc, const Type *t) {
    if (!t) return;
    if ((t->kind == TY_REF_IMMUT || t->kind == TY_REF_MUT)
            && t->n_lifetimes >= 2
            && t->lifetimes[0] != LIFETIME_NONE
            && t->lifetimes[1] != LIFETIME_NONE) {
        lifetime_context_add_constraint(lc, t->lifetimes[1], t->lifetimes[0]);
    }
}

static bool lifetime_check_fn_def(FnDef *fn) {
    if (!fn || fn->n_params == 0 || !fn->param_types) return true;

    /* LS2: use the full declared return Type, which preserves borrow lifetime
     * IDs (&'a T).  The TY_FN binding only carries result_kind, so reconstructing
     * from it would silently drop the programmer's output lifetime.  A FnDef
     * synthesised outside the surface parser (typeclass methods, partial-app
     * wrappers) records TY_NIL here -- no borrow lifetimes to check. */
    Type return_type = fn->return_type;
    if (return_type.kind == TY_UNKNOWN) {
        return_type = type_simple(TY_NIL, CK_COPY);
        if (fn->binding && fn->binding->type.kind == TY_FN) {
            return_type.kind = fn->binding->type.as.fn.result_kind;
        }
    }

    /* Rebuild the explicit-lifetime outlives graph directly from the borrow
     * Types (params + return).  This does not trust fn->lifetime_ctx, which may
     * be an uninitialised value for a synthetic FnDef. */
    LifetimeContext lc;
    lifetime_context_init(&lc);
    for (uint32_t i = 0; i < fn->n_params; i++) {
        collect_borrow_constraints(&lc, &fn->param_types[i]);
    }
    collect_borrow_constraints(&lc, &return_type);

    /* Run elision on a scratch context for the elided path (it binds fresh
     * lifetimes onto elided borrow params and flows them to elided borrow
     * returns, mutating the Types in place). */
    LifetimeContext scratch;
    lifetime_elision_apply(&scratch, fn->param_types, fn->n_params,
                           &return_type);

    /* Leave a clean, correct lifetime context on the FnDef for downstream
     * borrow checking. */
    fn->lifetime_ctx = lc;

    LifetimeId a = LIFETIME_NONE, b = LIFETIME_NONE;
    /* Reject a cyclic explicit signature (TUR-E0106) as well as any cycle the
     * elided path produced (defensive; elision cannot manufacture one today). */
    bool cyclic = lifetime_has_cycle(&lc, &a, &b);
    if (!cyclic) cyclic = lifetime_has_cycle(&scratch, &a, &b);
    if (cyclic) {
        Span span = fn->binding ? fn->binding->span : SPAN_UNKNOWN;
        diag_emit_with_code(DIAG_ERROR, span, TUR_E0106_CYCLIC_LIFETIME,
            "function '%s': lifetime constraints form a cycle "
            "('%c and '%c each required to outlive the other)",
            (fn->binding && fn->binding->name) ? fn->binding->name->name : "?",
            'a' + (a ? a - 1 : 0), 'a' + (b ? b - 1 : 0));
        return false;
    }
    return true;
}

bool lifetime_check_program(const Expr *program) {
    if (!program || program->kind != EX_PROGRAM) return true;
    bool ok = true;
    for (uint32_t i = 0; i < program->as.program.n; i++) {
        const Expr *item = program->as.program.items[i];
        if (item && item->kind == EX_FN_DEF && item->as.fn_def_.fn) {
            if (!lifetime_check_fn_def(item->as.fn_def_.fn)) ok = false;
        }
    }
    return ok;
}

/* Check a variable reference */
static bool borrow_check_var(BorrowCheckCtx *ctx, const Expr *e) {
    const Binding *b = e->as.var.binding;
    
    /* Check if this is a moved binding (skip in handler-captures-only mode,
     * as elab.c already emits use-after-move diagnostics). */
    if (!ctx->only_handler_captures && b->is_moved) {
        diag_emit(DIAG_ERROR, e->span,
                  "use-after-move of '%s' - value was moved", b->name->name);
        ctx->error_count++;
        return false;
    }
    
    /* Phase P19-7: Check borrow capture into effect handler case body.
     * Handler case bodies are emitted as top-level C static functions with no
     * access to the enclosing stack frame.  A binding with a borrow type that
     * originates outside the handler case (i.e. is not one of its own
     * param_bindings or k_binding) cannot be used inside the case body. */
    if (ctx->handler_case && type_is_borrow(b->type)) {
        const HandleCase *hc = ctx->handler_case;
        bool is_local = false;
        if (hc->k_binding && b == hc->k_binding) {
            is_local = true;
        }
        for (uint32_t i = 0; i < hc->n_params && !is_local; i++) {
            if (hc->param_bindings && hc->param_bindings[i] == b) {
                is_local = true;
            }
        }
        if (!is_local) {
            diag_emit(DIAG_ERROR, e->span,
                      "borrow '%s' cannot be captured by an effect handler case: "
                      "handler cases are emitted as separate functions and have no "
                      "access to the enclosing stack frame",
                      b->name->name);
            ctx->error_count++;
            return false;
        }
    }

    /* Check if this is a borrow type */
    if (type_is_borrow(e->type)) {
        /* The borrow should still be valid in the current scope */
        /* This is tracked by the existing Phase 12 scope-based borrow tracking */
    }
    
    return true;
}

/* Recursively check an expression */
static bool borrow_check_expr_recursive(BorrowCheckCtx *ctx, const Expr *e) {
    switch (e->kind) {
        /* saffron-lang-plan S3: a dynamic operator borrows nothing itself, but
         * its arguments are ordinary expressions and must still be walked --
         * skipping them would silently exempt a Saffron program from borrow
         * checking inside `(+ x (deref r))`. */
        case EX_DYN_OP:
            for (uint32_t i = 0; i < e->as.dyn_op_.n_args; i++)
                if (!borrow_check_expr_recursive(ctx, e->as.dyn_op_.args[i]))
                    return false;
            return true;
        /* saffron-lang-plan S4: the callee is an expression too, so it is
         * walked alongside the arguments. */
        /* saffron-lang-plan S4: a dynamic field READ borrows nothing, but its
         * receiver is an expression and must still be walked. */
        case EX_DYN_FIELD:
            return !e->as.dyn_field_.obj ||
                   borrow_check_expr_recursive(ctx, e->as.dyn_field_.obj);
        /* saffron-lang-plan S9: the receiver and every extra argument are
         * ordinary expressions; the dispatch itself borrows nothing. */
        case EX_DYN_METHOD:
            if (e->as.dyn_method_.obj &&
                !borrow_check_expr_recursive(ctx, e->as.dyn_method_.obj))
                return false;
            for (uint32_t i = 0; i < e->as.dyn_method_.n_args; i++)
                if (e->as.dyn_method_.args[i] &&
                    !borrow_check_expr_recursive(ctx, e->as.dyn_method_.args[i]))
                    return false;
            return true;
        case EX_DYN_CALL:
            if (e->as.dyn_call_.fn &&
                !borrow_check_expr_recursive(ctx, e->as.dyn_call_.fn))
                return false;
            for (uint32_t i = 0; i < e->as.dyn_call_.n_args; i++)
                if (!borrow_check_expr_recursive(ctx, e->as.dyn_call_.args[i]))
                    return false;
            return true;
        case EX_NIL_LIT:
        case EX_BOOL_LIT:
        case EX_INT_LIT:
        case EX_FLOAT_LIT:
        case EX_CSTR_LIT:
        case EX_SYM_LIT:
        case EX_DEFAULT_OF:
            return true;
        case EX_CPS_CONT_APP:
            /* CPS2: walk continuation and value subexpressions */
            return borrow_check_expr_recursive(ctx, e->as.cps_cont_app_.cont) &&
                   borrow_check_expr_recursive(ctx, e->as.cps_cont_app_.value);
        case EX_CAST:
            return borrow_check_expr_recursive(ctx, e->as.cast_.expr);
        case EX_CALLCC:   /* call-cc-completion: check f (the receiver) */
            return borrow_check_expr_recursive(ctx, e->as.callcc_.fn);
        case EX_REINTERPRET:
            return borrow_check_expr_recursive(ctx, e->as.reinterpret_.expr);
            
        case EX_VAR:
            return borrow_check_var(ctx, e);
            
        case EX_LET:
        case EX_LETREC:
            /* Check init expressions */
            for (uint32_t i = 0; i < e->as.let_.n; i++) {
                if (!borrow_check_expr_recursive(ctx, e->as.let_.bindings[i].init)) {
                    return false;
                }
            }
            /* Check body */
            return borrow_check_expr_recursive(ctx, e->as.let_.body);
            
        case EX_IF:
            if (!borrow_check_expr_recursive(ctx, e->as.if_.cond)) return false;
            if (!borrow_check_expr_recursive(ctx, e->as.if_.then_)) return false;
            if (e->as.if_.else_or_null && !borrow_check_expr_recursive(ctx, e->as.if_.else_or_null)) {
                return false;
            }
            return true;
            
        case EX_DO:
            for (uint32_t i = 0; i < e->as.do_.n; i++) {
                if (!borrow_check_expr_recursive(ctx, e->as.do_.items[i])) {
                    return false;
                }
            }
            return true;
            
        case EX_WHILE:
            if (!borrow_check_expr_recursive(ctx, e->as.while_.cond)) return false;
            if (!borrow_check_expr_recursive(ctx, e->as.while_.body)) return false;
            return true;
            
        case EX_SET:
            if (!borrow_check_expr_recursive(ctx, e->as.set_.value)) return false;
            /* Check if target is moved */
            if (!ctx->only_handler_captures && e->as.set_.target->is_moved) {
                diag_emit(DIAG_ERROR, e->span,
                          "use-after-move of '%s' in set!", e->as.set_.target->name->name);
                ctx->error_count++;
                return false;
            }
            return true;

        case EX_SET_DEREF:
            /* (set! (@ r) value) - mutation through mutable borrow */
            if (!borrow_check_expr_recursive(ctx, e->as.set_deref_.ref)) return false;
            if (!borrow_check_expr_recursive(ctx, e->as.set_deref_.value)) return false;
            return true;

        case EX_SET_FIELD:
            /* DS3: (set! (.field s) v) -- struct field write. */
            if (!borrow_check_expr_recursive(ctx, e->as.set_field_.receiver)) return false;
            if (!borrow_check_expr_recursive(ctx, e->as.set_field_.value)) return false;
            return true;
            
        case EX_DEF:
            if (e->as.def_.init && !borrow_check_expr_recursive(ctx, e->as.def_.init)) {
                return false;
            }
            return true;
            
        case EX_CALL:
            return borrow_check_call(ctx, e);
            
        case EX_FN_DEF:
            /* Check the function body with a new context */
            {
                BorrowCheckCtx fn_ctx;
                borrow_check_ctx_init(&fn_ctx, &e->as.fn_def_.fn->lifetime_ctx, ctx->scope);
                fn_ctx.only_handler_captures = ctx->only_handler_captures;
                if (!borrow_check_fn(&fn_ctx, e->as.fn_def_.fn)) {
                    ctx->error_count += fn_ctx.error_count;
                    return false;
                }
            }
            return true;
        case EX_FN:
            /* Function expression - check the fn_def */
            {
                FnDef *fn_def = e->as.fn_.fn;
                BorrowCheckCtx fn_ctx;
                borrow_check_ctx_init(&fn_ctx, &fn_def->lifetime_ctx, ctx->scope);
                fn_ctx.only_handler_captures = ctx->only_handler_captures;
                if (!borrow_check_fn(&fn_ctx, fn_def)) {
                    ctx->error_count += fn_ctx.error_count;
                    return false;
                }
            }
            return true;
            
        case EX_BORROW_IMMUT:
        case EX_BORROW_MUT:
            /* Borrow expressions are always valid at their creation point */
            if (!borrow_check_expr_recursive(ctx, e->as.borrow_immut_.expr)) {
                return false;
            }
            return true;
            
        case EX_CLOSURE: {
            /* HKT-P3: Validate all captured bindings at closure-creation site.
             * Iterate every binding in closure->captures and verify that none
             * of them has already been moved before the closure is formed. */
            const struct Closure *cl = e->as.closure_.closure;
            bool capture_ok = true;
            if (cl) {
                for (uint8_t ci = 0; ci < cl->n_captures; ci++) {
                    const Binding *b = cl->captures[ci];
                    if (!ctx->only_handler_captures && b && b->is_moved) {
                        diag_emit(DIAG_ERROR, e->span,
                                  "capture-after-move of '%s' in closure - "
                                  "value was already moved",
                                  b->name->name);
                        ctx->error_count++;
                        capture_ok = false;
                    }
                }
            }
            /* Check closure body */
            if (cl && cl->fn) {
                BorrowCheckCtx closure_ctx;
                borrow_check_ctx_init(&closure_ctx, &cl->fn->lifetime_ctx, ctx->scope);
                closure_ctx.only_handler_captures = ctx->only_handler_captures;
                if (!borrow_check_fn(&closure_ctx, cl->fn)) {
                    ctx->error_count += closure_ctx.error_count;
                    return false;
                }
            }
            return capture_ok;
        }
            
        case EX_DEFER:
            /* Check defer body */
            return borrow_check_expr_recursive(ctx, e->as.defer_.body);
        case EX_RETURN:
            /* Check return value */
            if (e->as.return_.value) {
                return borrow_check_expr_recursive(ctx, e->as.return_.value);
            }
            return true;
            
        case EX_REF:
        case EX_DEREF:
        case EX_RC_OF:
        case EX_RC_CLONE:
        case EX_RC_DROP:
        case EX_RC_PTR:
        case EX_RC_COUNT:
        case EX_RC_FROM_REF:
        case EX_REF_FROM_RC:
        case EX_WEAK:
        case EX_WEAK_PRED:
        case EX_REF_PRED:
        case EX_CONT_PRED:
            /* These are reference operations - check the inner expression */
            if (e->kind == EX_REF) {
                return borrow_check_expr_recursive(ctx, e->as.ref_.expr);
            } else if (e->kind == EX_DEREF) {
                return borrow_check_expr_recursive(ctx, e->as.deref_.expr);
            } else if (e->kind == EX_RC_OF) {
                return borrow_check_expr_recursive(ctx, e->as.rc_of_.expr);
            } else if (e->kind == EX_RC_CLONE) {
                return borrow_check_expr_recursive(ctx, e->as.rc_clone_.expr);
            } else if (e->kind == EX_RC_DROP) {
                return borrow_check_expr_recursive(ctx, e->as.rc_drop_.expr);
            } else if (e->kind == EX_RC_PTR) {
                return borrow_check_expr_recursive(ctx, e->as.rc_ptr_.expr);
            } else if (e->kind == EX_RC_COUNT) {
                return borrow_check_expr_recursive(ctx, e->as.rc_count_.expr);
            } else if (e->kind == EX_RC_FROM_REF) {
                return borrow_check_expr_recursive(ctx, e->as.rc_from_ref_.expr);
            } else if (e->kind == EX_REF_FROM_RC) {
                return borrow_check_expr_recursive(ctx, e->as.ref_from_rc_.expr);
            } else if (e->kind == EX_WEAK) {
                return borrow_check_expr_recursive(ctx, e->as.weak_.expr);
            } else if (e->kind == EX_WEAK_PRED) {
                return borrow_check_expr_recursive(ctx, e->as.weak_pred_.expr);
            } else if (e->kind == EX_REF_PRED) {
                return borrow_check_expr_recursive(ctx, e->as.ref_pred_.expr);
            } else if (e->kind == EX_CONT_PRED) {
                return borrow_check_expr_recursive(ctx, e->as.cont_pred_.expr);
            }
            return true;
        
        case EX_ASYNC: {
            /* AW-011B-2: Move tracking for async blocks.
             * When a value is captured by an async block, it is moved into the async context.
             * Mark all captured bindings as moved. */
            Expr *fn_expr = e->as.async_.fn_expr;
            if (fn_expr->kind == EX_FN || fn_expr->kind == EX_CLOSURE) {
                const struct Closure *cl = NULL;
                if (fn_expr->kind == EX_CLOSURE) {
                    cl = fn_expr->as.closure_.closure;
                } else if (fn_expr->kind == EX_FN) {
                    cl = fn_expr->as.fn_.fn->closure;
                }
                if (cl) {
                    for (uint8_t i = 0; i < cl->n_captures; i++) {
                        Binding *cap = cl->captures[i];
                        if (cap && !cap->is_moved) {
                            cap->is_moved = true;
                            cap->moved_at = e->span;
                        }
                    }
                }
            }
            return borrow_check_expr_recursive(ctx, e->as.async_.fn_expr);
        }
        
        case EX_AWAIT: {
            /* AW-012 / AW-011B-1: Enforce Send for values live across await points.
             * When we reach an await point, all currently live bindings (captured
             * by the enclosing async closure and not yet moved) must be Send.
             * Walk the current scope's bindings and check.
             */
            /* For now, just recurse into the future expression.
             * Full Send checking across await points requires tracking which
             * specific bindings cross each await, which needs Phase 23.
             * The Send check at async boundary (AW-012 task 1) covers the
             * common case where non-Send values are captured at all.
             */
            return borrow_check_expr_recursive(ctx, e->as.await_.fut_expr);
        }
        case EX_SELECT: {
            /* Phase SEL1: recurse into channel expressions and clause bodies */
            for (uint32_t ci = 0; ci < e->as.select_.n_clauses; ci++) {
                const SelectClauseEntry *cl = &e->as.select_.clauses[ci];
                if (!borrow_check_expr_recursive(ctx, cl->chan)) return false;
                if (cl->send_val && !borrow_check_expr_recursive(ctx, cl->send_val)) return false;
                if (!borrow_check_expr_recursive(ctx, cl->body)) return false;
            }
            if (e->as.select_.default_body)
                return borrow_check_expr_recursive(ctx, e->as.select_.default_body);
            return true;
        }
        /* Phase N: numeric cast — handled at line 169 in the literals group */

        case EX_BUILTIN:
            /* Check builtin arguments */
            for (uint32_t i = 0; i < e->as.builtin.n; i++) {
                if (!borrow_check_expr_recursive(ctx, e->as.builtin.args[i])) {
                    return false;
                }
            }
            return true;
            
        case EX_TYPECLASS_DEF:
        case EX_INSTANCE_DEF:
        case EX_DICT:
            /* Typeclass definitions and dictionary references are compile-time only */
            return true;
            
        case EX_EXTERN_C:
        case EX_INLINE_C:
        case EX_MAKE_STRUCT:
        case EX_SET_LIT:
            /* External/inline C and struct/set literals are trusted */
            return true;

        case EX_GET_FIELD:
            return borrow_check_expr_recursive(ctx, e->as.get_field_.struct_expr);

        case EX_DEFMODULE: {
            DefModule *mod = e->as.defmodule_.mod;
            for (uint32_t i = 0; i < mod->n_body; i++) {
                if (!borrow_check_expr_recursive(ctx, mod->body[i])) return false;
            }
            return true;
        }

        case EX_PROGRAM:
            for (uint32_t i = 0; i < e->as.program.n; i++) {
                if (!borrow_check_expr_recursive(ctx, e->as.program.items[i])) {
                    return false;
                }
            }
            return true;
        /* Phase R2: Panic */
        case EX_PANIC:
            return borrow_check_expr_recursive(ctx, e->as.panic_.payload);
        case EX_PANIC_WITH:
            return borrow_check_expr_recursive(ctx, e->as.panic_with_.payload);
        case EX_CATCH_UNWIND:
            return borrow_check_expr_recursive(ctx, e->as.catch_unwind_.thunk);
        case EX_CATCH_PANIC_OF:
            return borrow_check_expr_recursive(ctx, e->as.catch_panic_of_.thunk);
        case EX_PANIC_PAYLOAD_TYPE:
        case EX_PANIC_PAYLOAD_VALUE:
        case EX_PANIC_PAYLOAD_FILE:
        case EX_PANIC_PAYLOAD_LINE:
            return borrow_check_expr_recursive(ctx, e->as.panic_payload_type_.payload);
        case EX_PANIC_PAYLOAD_DOWNS:
            return borrow_check_expr_recursive(ctx, e->as.panic_payload_downs_.payload);
        /* Phase 18: Delimited continuations */
        case EX_RESET:
            /* Check the reset body */
            return borrow_check_expr_recursive(ctx, e->as.reset_.body);
        case EX_SHIFT:
            /* Check k function and body */
            if (!borrow_check_expr_recursive(ctx, e->as.shift_.k_fn)) return false;
            if (!borrow_check_expr_recursive(ctx, e->as.shift_.body)) return false;
            return true;
        case EX_SHIFT0:
            /* Check k function and body */
            if (!borrow_check_expr_recursive(ctx, e->as.shift0_.k_fn)) return false;
            if (!borrow_check_expr_recursive(ctx, e->as.shift0_.body)) return false;
            return true;
        /* Phase B2: Cloneable continuations */
        case EX_CLONEABLE_RESET:
            /* Check the cloneable-reset body */
            return borrow_check_expr_recursive(ctx, e->as.cloneable_reset_.body);
        case EX_CLONEABLE_SHIFT:
            /* Check k function and body */
            if (!borrow_check_expr_recursive(ctx, e->as.cloneable_shift_.k_fn)) return false;
            if (!borrow_check_expr_recursive(ctx, e->as.cloneable_shift_.body)) return false;
            return true;
        /* Phase 19: Algebraic effects */
        case EX_DEFECT:
            /* Effect definitions don't have borrows to check at this level */
            return true;
        /* DV0-DV1: Dynamic vars */
        case EX_DEFDYNAMIC:
            /* Declaration-only node; no borrows to check */
            return true;
        case EX_DYNVAR_READ:
            return true;
        case EX_DYNVAR_SET:
            return borrow_check_expr_recursive(ctx, e->as.dynvar_set_.value);
        case EX_DYNVAR_BINDING: {
            for (uint32_t i = 0; i < e->as.dynvar_binding_.n_pairs; i++) {
                if (!borrow_check_expr_recursive(ctx, e->as.dynvar_binding_.pairs[i].override_expr))
                    return false;
            }
            return borrow_check_expr_recursive(ctx, e->as.dynvar_binding_.body);
        }
        case EX_PERFORM:
            /* Check perform arguments */
            for (uint32_t i = 0; i < e->as.perform_.perform->n_args; i++) {
                if (!borrow_check_expr_recursive(ctx, e->as.perform_.perform->args[i])) {
                    return false;
                }
            }
            return true;
        case EX_HANDLE:
            /* Check the handled body (no special capture restrictions) */
            if (!borrow_check_expr_recursive(ctx, e->as.handle_.handle->body)) {
                return false;
            }
            /* Phase P19-7: Check each handler case body in a sub-context that
             * marks it as being inside a handler case.  Any variable reference
             * whose type is a borrow (&T / &mut T) and whose binding originates
             * outside the case (not in param_bindings or k_binding) is illegal
             * because handler case bodies are emitted as top-level C static
             * functions with no access to the enclosing stack frame. */
            for (uint8_t i = 0; i < e->as.handle_.handle->n_cases; i++) {
                const HandleCase *hc = &e->as.handle_.handle->cases[i];
                const HandleCase *prev_hc = ctx->handler_case;
                ctx->handler_case = hc;
                bool ok = borrow_check_expr_recursive(ctx, hc->body);
                ctx->handler_case = prev_hc;
                if (!ok) {
                    return false;
                }
            }
            return true;
        case EX_HANDLER_LIT:
            /* FH2: handler literal -- check each case body inside handler-case ctx. */
            for (uint8_t i = 0; i < e->as.handler_lit_.handle->n_cases; i++) {
                const HandleCase *hc = &e->as.handler_lit_.handle->cases[i];
                const HandleCase *prev_hc = ctx->handler_case;
                ctx->handler_case = hc;
                bool ok = borrow_check_expr_recursive(ctx, hc->body);
                ctx->handler_case = prev_hc;
                if (!ok) return false;
            }
            return true;
        case EX_WITH_HANDLER:
            /* FH3: check the handler value and the handled body. */
            if (!borrow_check_expr_recursive(ctx, e->as.with_handler_.handler)) return false;
            if (!borrow_check_expr_recursive(ctx, e->as.with_handler_.body)) return false;
            return true;
        case EX_COMPOSE_HANDLERS:
            /* FH5: check both handler values. */
            if (!borrow_check_expr_recursive(ctx, e->as.compose_handlers_.h1)) return false;
            if (!borrow_check_expr_recursive(ctx, e->as.compose_handlers_.h2)) return false;
            return true;
        case EX_RESUME:
            /* Check continuation and value */
            if (!borrow_check_expr_recursive(ctx, e->as.resume_.resume->k)) return false;
            if (!borrow_check_expr_recursive(ctx, e->as.resume_.resume->value)) return false;
            return true;
        case EX_DISCONTINUE:
            /* Check continuation and exception */
            if (!borrow_check_expr_recursive(ctx, e->as.discontinue_.discontinue->k)) return false;
            if (!borrow_check_expr_recursive(ctx, e->as.discontinue_.discontinue->exception)) return false;
            return true;
        /* Phase 20: Software Transactional Memory */
        case EX_STM: {
            /* Check all body expressions */
            for (uint32_t i = 0; i < e->as.stm_.n_body; i++) {
                if (!borrow_check_expr_recursive(ctx, e->as.stm_.body[i])) {
                    return false;
                }
            }
            return true;
        }
        case EX_ATOMICALLY:
            return borrow_check_expr_recursive(ctx, e->as.atomically_.stm_expr);
        case EX_RETRY:
            /* retry has no children */
            return true;
        case EX_CHECK:
            return borrow_check_expr_recursive(ctx, e->as.check_.cond);
        case EX_OR_ELSE:
            if (!borrow_check_expr_recursive(ctx, e->as.or_else_.stm1)) return false;
            if (!borrow_check_expr_recursive(ctx, e->as.or_else_.stm2)) return false;
            return true;
        case EX_TVAR_NEW:
            return borrow_check_expr_recursive(ctx, e->as.tvar_new_.init);
        case EX_TVAR_READ:
            return borrow_check_expr_recursive(ctx, e->as.tvar_read_.tvar);
        case EX_TVAR_WRITE:
            if (!borrow_check_expr_recursive(ctx, e->as.tvar_write_.tvar)) return false;
            if (!borrow_check_expr_recursive(ctx, e->as.tvar_write_.value)) return false;
            return true;
        case EX_TVAR_MODIFY:
            if (!borrow_check_expr_recursive(ctx, e->as.tvar_modify_.tvar)) return false;
            if (!borrow_check_expr_recursive(ctx, e->as.tvar_modify_.fn)) return false;
            return true;
        case EX_TVAR_SWAP:
            if (!borrow_check_expr_recursive(ctx, e->as.tvar_swap_.tvar)) return false;
            if (!borrow_check_expr_recursive(ctx, e->as.tvar_swap_.new_val)) return false;
            return true;
        case EX_TVAR_CAS:
            if (!borrow_check_expr_recursive(ctx, e->as.tvar_cas_.tvar)) return false;
            if (!borrow_check_expr_recursive(ctx, e->as.tvar_cas_.old_val)) return false;
            if (!borrow_check_expr_recursive(ctx, e->as.tvar_cas_.new_val)) return false;
            return true;
        /* Phase 21: Serializable continuations — borrow rules same as reset/shift */
        case EX_SERIAL_RESET:
            return borrow_check_expr_recursive(ctx, e->as.serial_reset_.body);
        case EX_SERIAL_SHIFT:
            if (!borrow_check_expr_recursive(ctx, e->as.serial_shift_.k_fn)) return false;
            return borrow_check_expr_recursive(ctx, e->as.serial_shift_.body);
        /* Phase HRT1: poly wrap is pure (no borrows), ascribe just delegates */
        case EX_POLY_WRAP:
            return borrow_check_expr_recursive(ctx, e->as.poly_wrap_.inner);
        case EX_FN_TO_FAT:
            return borrow_check_expr_recursive(ctx, e->as.fn_to_fat_.inner);
        case EX_POLY_TO_FAT:
            return borrow_check_expr_recursive(ctx, e->as.poly_to_fat_.inner);
        case EX_ASCRIBE:
            return borrow_check_expr_recursive(ctx, e->as.ascribe_.inner);
        /* Phase HRT2: existential pack/open */
        case EX_EXISTS_PACK:
            return borrow_check_expr_recursive(ctx, e->as.exists_pack_.value);
        case EX_EXISTS_OPEN:
            if (!borrow_check_expr_recursive(ctx, e->as.exists_open_.packed)) return false;
            return borrow_check_expr_recursive(ctx, e->as.exists_open_.body);
        case EX_EXISTS_DISPATCH:
            for (uint32_t i = 0; i < e->as.exists_dispatch_.n_args; i++) {
                if (!borrow_check_expr_recursive(ctx, e->as.exists_dispatch_.args[i]))
                    return false;
            }
            return true;
        /* Phase G0/G1: ADTs */
        case EX_DEFDATA:
            return true;
        case EX_MATCH: {
            if (!borrow_check_expr_recursive(ctx, e->as.match_.scrutinee)) return false;
            for (uint32_t i = 0; i < e->as.match_.n_arms; i++) {
                if (!borrow_check_expr_recursive(ctx, e->as.match_.arms[i].body)) return false;
            }
            return true;
        }
        /* IT4: Tagged union injection — delegate to inner value */
        case EX_UNION_INJECT:
            return borrow_check_expr_recursive(ctx, e->as.union_inject_.value);
        /* IT4 gradual typing — delegate to inner value */
        case EX_ANY_TYPE_OF:
            return borrow_check_expr_recursive(ctx, e->as.any_type_of_.value);
        case EX_ANY_CAST:
            return borrow_check_expr_recursive(ctx, e->as.any_cast_.value);
        case EX_ANY_IS:
            return borrow_check_expr_recursive(ctx, e->as.any_is_.value);
        /* GF1: Generator forms -- check sub-expressions */
        case EX_GEN:
            return e->as.gen_.def && e->as.gen_.def->body
                ? borrow_check_expr_recursive(ctx, e->as.gen_.def->body) : true;
        case EX_YIELD:
            return e->as.yield_.value
                ? borrow_check_expr_recursive(ctx, e->as.yield_.value) : true;
        case EX_GEN_NEXT:
            return borrow_check_expr_recursive(ctx, e->as.gen_next_.gen_expr);
        case EX_GEN_UNWRAP:
            return borrow_check_expr_recursive(ctx, e->as.gen_unwrap_.ptr_expr);
        case EX_GEN_DONE:
            return borrow_check_expr_recursive(ctx, e->as.gen_done_.gen_expr);
        case EX_CONS_LIST: {
            /* AR8: check each item in the rest-list construction */
            bool ok = true;
            for (uint32_t i = 0; i < e->as.cons_list_.n; i++) {
                if (!borrow_check_expr_recursive(ctx, e->as.cons_list_.items[i])) ok = false;
            }
            return ok;
        }
    }

    return true;
}

bool borrow_check_expr(BorrowCheckCtx *ctx, const Expr *e) {
    return borrow_check_expr_recursive(ctx, e);
}

bool borrow_check_fn(BorrowCheckCtx *ctx, const FnDef *fn) {
    /* Check function signature first */
    if (!borrow_check_fn_signature(fn)) {
        ctx->error_count++;
        return false;
    }
    
    /* Check return type */
    if (!borrow_check_return_type(fn)) {
        ctx->error_count++;
        return false;
    }
    
    /* Check function body */
    BorrowCheckCtx body_ctx;
    borrow_check_ctx_init(&body_ctx, &fn->lifetime_ctx, ctx->scope);
    body_ctx.only_handler_captures = ctx->only_handler_captures;
    
    if (fn->body && !borrow_check_expr(&body_ctx, fn->body)) {
        ctx->error_count += body_ctx.error_count;
        return false;
    }
    
    return true;
}

bool borrow_check_fn_signature(const FnDef *fn) {
    /* Validate that all borrow parameters have valid types */
    Span fn_span = fn->binding ? fn->binding->span : SPAN_UNKNOWN;
    for (uint32_t i = 0; i < fn->n_params; i++) {
        Type param_type = fn->param_types[i];
        
        /* Check if parameter type is valid */
        if (param_type.kind == TY_UNKNOWN) {
            diag_emit(DIAG_ERROR, fn_span,
                      "function '%s': parameter %u has unknown type", 
                      fn->binding->name->name, i + 1);
            return false;
        }
        
        /* Check for dangling references in parameters */
        if (type_is_borrow(param_type)) {
            /* Borrow parameters are valid as long as they have a lifetime */
            if (param_type.n_lifetimes == 0) {
                /* No explicit lifetime - elision rules should have applied */
                /* This is OK - elision rules assign implicit lifetimes */
            }
        }
    }
    
    return true;
}

bool borrow_check_return_type(const FnDef *fn) {
    Span fn_span = fn->binding ? fn->binding->span : SPAN_UNKNOWN;
    Type return_type = fn->binding->type;
    
    /* If return type is not a function type, get the actual return type */
    if (return_type.kind == TY_FN) {
        return_type.kind = return_type.as.fn.result_kind;
    }
    
    /* Check for dangling references in return type */
    if (type_is_borrow(return_type) && return_type.n_lifetimes == 0) {
        /* Return type is a borrow but has no lifetime annotation */
        /* This means it borrows from one of the input parameters (via elision) */
        /* For now, we allow this - the elision rules should have assigned a lifetime */
    }
    
    /* Check that all lifetimes in the return type are valid */
    if (type_has_any_lifetime(&return_type)) {
        /* Validate that each lifetime in the return type corresponds to a valid input lifetime */
        for (uint8_t i = 0; i < return_type.n_lifetimes; i++) {
            LifetimeId lid = return_type.lifetimes[i];
            if (lid == LIFETIME_NONE) continue;
            
            /* Check if this lifetime appears in any input parameter */
            bool found = false;
            for (uint32_t j = 0; j < fn->n_params; j++) {
                Type param_type = fn->param_types[j];
                LifetimeId param_lifetimes[MAX_TYPE_LIFETIMES];
                uint8_t n_param = 0;
                type_collect_lifetimes(&param_type, param_lifetimes, &n_param, MAX_TYPE_LIFETIMES);
                
                for (uint8_t k = 0; k < n_param; k++) {
                    if (param_lifetimes[k] == lid) {
                        found = true;
                        break;
                    }
                }
                if (found) break;
            }
            
            if (!found) {
                diag_emit(DIAG_ERROR, fn_span,
                          "function '%s': return type lifetime '%c' does not appear in any input parameter",
                          fn->binding->name->name, 'a' + (lid - 1));
                return false;
            }
        }
    }
    
    return true;
}
