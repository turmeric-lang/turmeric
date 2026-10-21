#include "effect_lower.h"

#include <string.h> /* For strlen */
#include "arena.h"
#include "diag.h"
#include "expr.h"
#include "effect.h"

/* Phase 19: Effect lowering pass
 *
 * Despite the name, nothing is lowered here.  effect_lower returns its input
 * when the program contains no effect node; otherwise it rebuilds the tree arm
 * by arm and returns EX_PERFORM, EX_HANDLE and EX_RESUME unchanged.  The
 * CPS/DK backend lowers effects.  The PASS_EFFECT_LOWER arm's real work is
 * building the EffectEnv that later passes read.
 *
 * The rebuild is not quite an identity: lower_discontinue rebuilds
 * EX_DISCONTINUE with TYPE_NIL rather than the node's own type.  Confirm
 * nothing downstream reads that type before deleting the pass.
 */

/* Check if an expression contains perform or handle */
static bool expr_contains_effects(const Expr *e) {
    if (!e) return false;
    
    switch (e->kind) {
        case EX_PERFORM:
        case EX_HANDLE:
        case EX_RESUME:
        case EX_DISCONTINUE:
            return true;
        case EX_LET:
            for (uint32_t i = 0; i < e->as.let_.n; i++) {
                if (expr_contains_effects(e->as.let_.bindings[i].init)) return true;
            }
            return expr_contains_effects(e->as.let_.body);
        case EX_IF:
            if (expr_contains_effects(e->as.if_.cond)) return true;
            if (expr_contains_effects(e->as.if_.then_)) return true;
            if (e->as.if_.else_or_null && expr_contains_effects(e->as.if_.else_or_null)) return true;
            return false;
        case EX_DO:
            for (uint32_t i = 0; i < e->as.do_.n; i++) {
                if (expr_contains_effects(e->as.do_.items[i])) return true;
            }
            return false;
        case EX_WHILE:
            return expr_contains_effects(e->as.while_.cond) ||
                   expr_contains_effects(e->as.while_.body);
        case EX_SET:
            return expr_contains_effects(e->as.set_.value);
        case EX_DEF:
            return e->as.def_.init && expr_contains_effects(e->as.def_.init);
        case EX_BUILTIN:
            for (uint32_t i = 0; i < e->as.builtin.n; i++) {
                if (expr_contains_effects(e->as.builtin.args[i])) return true;
            }
            return false;
        case EX_FN_DEF:
            return expr_contains_effects(e->as.fn_def_.fn->body);
        case EX_FN:
            return false; /* Function literals don't contain effects at this level */
        case EX_CALL:
            for (uint32_t i = 0; i < e->as.call_.n_args; i++) {
                if (expr_contains_effects(e->as.call_.args[i])) return true;
            }
            return false;
        case EX_CLOSURE:
            return expr_contains_effects(e->as.closure_.closure->fn->body);
        case EX_DEFER:
            return expr_contains_effects(e->as.defer_.body);
        case EX_RETURN:
            return e->as.return_.value && expr_contains_effects(e->as.return_.value);
        case EX_PANIC:
            return expr_contains_effects(e->as.panic_.payload);
        case EX_PANIC_WITH:
            return expr_contains_effects(e->as.panic_with_.payload);
        case EX_CATCH_UNWIND:
            return expr_contains_effects(e->as.catch_unwind_.thunk);
        case EX_CATCH_PANIC_OF:
            return expr_contains_effects(e->as.catch_panic_of_.thunk);
        case EX_PANIC_PAYLOAD_TYPE:
        case EX_PANIC_PAYLOAD_VALUE:
        case EX_PANIC_PAYLOAD_FILE:
        case EX_PANIC_PAYLOAD_LINE:
            return expr_contains_effects(e->as.panic_payload_type_.payload);
        case EX_PANIC_PAYLOAD_DOWNS:
            return expr_contains_effects(e->as.panic_payload_downs_.payload);
        case EX_RESET:
        case EX_SHIFT:
        case EX_SHIFT0:
        case EX_CLONEABLE_RESET:
        case EX_CLONEABLE_SHIFT:
            /* These are already lowered or primitive - no further lowering */
            return false;
        default:
            return false;
    }
}

/* Helper: transform a single expression, recursively lowering effects */
static Expr *lower_expr(Arena *a, SymbolTable *st, Expr *e, EffectEnv *effect_env);

/* Helper: transform a discontinue expression */
static Expr *lower_discontinue(Arena *a, SymbolTable *st, DiscontinueExpr *dc, Span span) {
    Expr *new_k = lower_expr(a, st, dc->k, NULL);
    Expr *new_exception = lower_expr(a, st, dc->exception, NULL);
    
    DiscontinueExpr *new_dc = arena_alloc(a, sizeof(DiscontinueExpr));
    new_dc->k = new_k;
    new_dc->exception = new_exception;
    
    Expr *out = expr_new(a, EX_DISCONTINUE, TYPE_NIL, span);
    out->as.discontinue_.discontinue = new_dc;
    return out;
}

/* Helper: transform a single expression, recursively lowering effects */
static Expr *lower_expr(Arena *a, SymbolTable *st, Expr *e, EffectEnv *effect_env) {
    if (!e) return NULL;
    
    switch (e->kind) {
        case EX_NIL_LIT:
        case EX_BOOL_LIT:
        case EX_INT_LIT:
        case EX_FLOAT_LIT:
        case EX_CSTR_LIT:
        case EX_VAR:
            return e;
        
        case EX_LET: {
            LetBinding *new_bindings = arena_alloc(a, e->as.let_.n * sizeof(LetBinding));
            for (uint32_t i = 0; i < e->as.let_.n; i++) {
                new_bindings[i].binding = e->as.let_.bindings[i].binding;
                new_bindings[i].init = lower_expr(a, st, e->as.let_.bindings[i].init, effect_env);
            }
            Expr *new_body = lower_expr(a, st, e->as.let_.body, effect_env);
            Expr *out = expr_new(a, EX_LET, e->type, e->span);
            out->as.let_.bindings = new_bindings;
            out->as.let_.n = e->as.let_.n;
            out->as.let_.body = new_body;
            return out;
        }
        
        case EX_IF: {
            Expr *new_cond = lower_expr(a, st, e->as.if_.cond, effect_env);
            Expr *new_then = lower_expr(a, st, e->as.if_.then_, effect_env);
            Expr *new_else = e->as.if_.else_or_null ? 
                lower_expr(a, st, e->as.if_.else_or_null, effect_env) : NULL;
            Expr *out = expr_new(a, EX_IF, e->type, e->span);
            out->as.if_.cond = new_cond;
            out->as.if_.then_ = new_then;
            out->as.if_.else_or_null = new_else;
            return out;
        }
        
        case EX_DO: {
            Expr **new_items = arena_alloc(a, e->as.do_.n * sizeof(Expr *));
            for (uint32_t i = 0; i < e->as.do_.n; i++) {
                new_items[i] = lower_expr(a, st, e->as.do_.items[i], effect_env);
            }
            Expr *out = expr_new(a, EX_DO, e->type, e->span);
            out->as.do_.items = new_items;
            out->as.do_.n = e->as.do_.n;
            return out;
        }
        
        case EX_WHILE: {
            Expr *new_cond = lower_expr(a, st, e->as.while_.cond, effect_env);
            Expr *new_body = lower_expr(a, st, e->as.while_.body, effect_env);
            Expr *out = expr_new(a, EX_WHILE, e->type, e->span);
            out->as.while_.cond = new_cond;
            out->as.while_.body = new_body;
            return out;
        }
        
        case EX_SET: {
            Expr *new_value = lower_expr(a, st, e->as.set_.value, effect_env);
            Expr *out = expr_new(a, EX_SET, e->type, e->span);
            out->as.set_.target = e->as.set_.target;
            out->as.set_.value = new_value;
            return out;
        }
        
        case EX_DEF:
            if (e->as.def_.init) {
                Expr *new_init = lower_expr(a, st, e->as.def_.init, effect_env);
                Expr *out = expr_new(a, EX_DEF, e->type, e->span);
                out->as.def_.binding = e->as.def_.binding;
                out->as.def_.init = new_init;
                return out;
            }
            return e;
        
        case EX_BUILTIN: {
            Expr **new_args = arena_alloc(a, e->as.builtin.n * sizeof(Expr *));
            for (uint32_t i = 0; i < e->as.builtin.n; i++) {
                new_args[i] = lower_expr(a, st, e->as.builtin.args[i], effect_env);
            }
            Expr *out = expr_new(a, EX_BUILTIN, e->type, e->span);
            out->as.builtin.spec = e->as.builtin.spec;
            out->as.builtin.args = new_args;
            out->as.builtin.n = e->as.builtin.n;
            return out;
        }
        
        case EX_FN_DEF: {
            FnDef *fd = e->as.fn_def_.fn;
            FnDef *new_fd = arena_alloc(a, sizeof(FnDef));
            *new_fd = *fd;
            new_fd->body = lower_expr(a, st, fd->body, effect_env);
            Expr *out = expr_new(a, EX_FN_DEF, e->type, e->span);
            out->as.fn_def_.fn = new_fd;
            return out;
        }
        
        case EX_FN: {
            FnDef *new_fn = arena_alloc(a, sizeof(FnDef));
            *new_fn = *e->as.fn_.fn;
            new_fn->body = lower_expr(a, st, new_fn->body, effect_env);
            Expr *out = expr_new(a, EX_FN, e->type, e->span);
            out->as.fn_.fn = new_fn;
            return out;
        }
        
        case EX_CALL: {
            Expr **new_args = arena_alloc(a, e->as.call_.n_args * sizeof(Expr *));
            for (uint32_t i = 0; i < e->as.call_.n_args; i++) {
                new_args[i] = lower_expr(a, st, e->as.call_.args[i], effect_env);
            }
            Expr *out = expr_new(a, EX_CALL, e->type, e->span);
            out->as.call_.fn_binding = e->as.call_.fn_binding;
            out->as.call_.fn_expr = e->as.call_.fn_expr
                ? lower_expr(a, st, e->as.call_.fn_expr, effect_env) : NULL;
            out->as.call_.args = new_args;
            out->as.call_.n_args = e->as.call_.n_args;
            return out;
        }
        
        case EX_CLOSURE: {
            struct Closure *closure = e->as.closure_.closure;
            FnDef *new_fn = arena_alloc(a, sizeof(FnDef));
            *new_fn = *closure->fn;
            new_fn->body = lower_expr(a, st, closure->fn->body, effect_env);
            
            struct Closure *new_closure = arena_alloc(a, sizeof(struct Closure));
            *new_closure = *closure;
            new_closure->fn = new_fn;
            
            Expr *out = expr_new(a, EX_CLOSURE, e->type, e->span);
            out->as.closure_.closure = new_closure;
            return out;
        }
        
        case EX_RETURN: {
            Expr *new_value = e->as.return_.value ? 
                lower_expr(a, st, e->as.return_.value, effect_env) : NULL;
            Expr *out = expr_new(a, EX_RETURN, e->type, e->span);
            out->as.return_.value = new_value;
            return out;
        }
        
        case EX_PANIC: {
            Expr *new_payload = lower_expr(a, st, e->as.panic_.payload, effect_env);
            Expr *out = expr_new(a, EX_PANIC, e->type, e->span);
            out->as.panic_.payload = new_payload;
            return out;
        }
        
        case EX_DEFER: {
            Expr *new_body = lower_expr(a, st, e->as.defer_.body, effect_env);
            Binding **new_captures = NULL;
            if (e->as.defer_.n_captures > 0) {
                new_captures = arena_alloc(a, e->as.defer_.n_captures * sizeof(Binding *));
                for (uint8_t i = 0; i < e->as.defer_.n_captures; i++) {
                    new_captures[i] = e->as.defer_.captures[i];
                }
            }
            
            Expr *out = expr_new(a, EX_DEFER, e->type, e->span);
            out->as.defer_.body = new_body;
            out->as.defer_.captures = new_captures;
            out->as.defer_.n_captures = e->as.defer_.n_captures;
            return out;
        }
        
        case EX_REF:
        case EX_DEREF:
        case EX_RC_OF:
        case EX_RC_CLONE:
        case EX_RC_DROP:
        case EX_RC_PTR:
        case EX_RC_COUNT:
        case EX_WEAK:
        case EX_WEAK_UPGRADE:
        case EX_WEAK_PRED:
        case EX_REF_PRED:
        case EX_BORROW_IMMUT:
        case EX_BORROW_MUT:
        case EX_SET_DEREF:
        case EX_TYPECLASS_DEF:
        case EX_INSTANCE_DEF:
        case EX_DICT:
        case EX_EXTERN_C:
        case EX_INLINE_C:
        case EX_MAKE_STRUCT:
        case EX_SET_LIT:
            return e;
        
        /* Phase 18: Delimited continuations */
        case EX_RESET: {
            Expr *new_body = lower_expr(a, st, e->as.reset_.body, effect_env);
            Expr *out = expr_new(a, EX_RESET, e->type, e->span);
            out->as.reset_.body = new_body;
            return out;
        }
        
        case EX_SHIFT: {
            Expr *new_k_fn = lower_expr(a, st, e->as.shift_.k_fn, effect_env);
            Expr *new_body = lower_expr(a, st, e->as.shift_.body, effect_env);
            Expr *out = expr_new(a, EX_SHIFT, e->type, e->span);
            out->as.shift_.k_fn = new_k_fn;
            out->as.shift_.body = new_body;
            return out;
        }
        
        case EX_SHIFT0: {
            Expr *new_k_fn = lower_expr(a, st, e->as.shift0_.k_fn, effect_env);
            Expr *new_body = lower_expr(a, st, e->as.shift0_.body, effect_env);
            Expr *out = expr_new(a, EX_SHIFT0, e->type, e->span);
            out->as.shift0_.k_fn = new_k_fn;
            out->as.shift0_.body = new_body;
            return out;
        }
        
        /* Phase B2: Cloneable continuations */
        case EX_CLONEABLE_RESET: {
            Expr *new_body = lower_expr(a, st, e->as.cloneable_reset_.body, effect_env);
            Expr *out = expr_new(a, EX_CLONEABLE_RESET, e->type, e->span);
            out->as.cloneable_reset_.body = new_body;
            return out;
        }
        
        case EX_CLONEABLE_SHIFT: {
            Expr *new_k_fn = lower_expr(a, st, e->as.cloneable_shift_.k_fn, effect_env);
            Expr *new_body = lower_expr(a, st, e->as.cloneable_shift_.body, effect_env);
            Expr *out = expr_new(a, EX_CLONEABLE_SHIFT, e->type, e->span);
            out->as.cloneable_shift_.k_fn = new_k_fn;
            out->as.cloneable_shift_.body = new_body;
            return out;
        }
        
        /* Phase 19: Algebraic effects
         * EX_PERFORM, EX_HANDLE, and EX_RESUME are emitted directly by emit.c
         * using the runtime effect handler chain (global_effect_handler_chain).
         * We pass them through unchanged here instead of converting to shift/reset stubs.
         */
        case EX_DEFECT:
            return e;
        /* DV1: Dynamic var nodes pass through unchanged */
        case EX_DEFDYNAMIC:
        case EX_DYNVAR_READ:
        case EX_DYNVAR_SET:
            return e;
        case EX_DYNVAR_BINDING: {
            for (uint32_t i = 0; i < e->as.dynvar_binding_.n_pairs; i++) {
                e->as.dynvar_binding_.pairs[i].override_expr =
                    lower_expr(a, st, e->as.dynvar_binding_.pairs[i].override_expr, effect_env);
            }
            e->as.dynvar_binding_.body = lower_expr(a, st, e->as.dynvar_binding_.body, effect_env);
            return e;
        }

        case EX_PERFORM:
            return e;

        case EX_HANDLE:
            return e;
        
        case EX_RESUME:
            return e;
        
        case EX_DISCONTINUE: {
            return lower_discontinue(a, st, e->as.discontinue_.discontinue, e->span);
        }
        
        case EX_PROGRAM: {
            Expr **new_items = arena_alloc(a, e->as.program.n * sizeof(Expr *));
            for (uint32_t i = 0; i < e->as.program.n; i++) {
                new_items[i] = lower_expr(a, st, e->as.program.items[i], effect_env);
            }
            Expr *out = expr_new(a, EX_PROGRAM, e->type, e->span);
            out->as.program.items = new_items;
            out->as.program.n = e->as.program.n;
            return out;
        }
        
        default:
            return e;
    }
}

/* Main entry point: lower all effect expressions in a program */
Expr *effect_lower(Arena *a, SymbolTable *st, Expr *program, EffectEnv *effect_env) {
    if (!program) return NULL;
    
    /* Check if the program contains any effects */
    if (!expr_contains_effects(program)) {
        return program;
    }
    
    /* Transform the entire program */
    return lower_expr(a, st, program, effect_env);
}
