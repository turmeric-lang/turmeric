#include "cps.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "arena.h"
#include "expr.h"
#include "typeclass.h"
#include "effect.h"   /* effect_row_is_empty */
#include "globals.h"

/* Phase 18: CPS transformation for delimited continuations
 * 
 * For v1, we use a simplified approach:
 * - Functions containing shift/reset are marked as "may_capture" 
 * - No full CPS transformation is performed
 * - The emitter handles shift/reset using runtime continuation support
 * 
 * Full CPS transformation (where every function call becomes a tail call
 * into a continuation) will be implemented in a future phase.
 */

/* Check if an expression contains shift or shift0 */
bool cps_expr_contains_shift(const Expr *e) {
    if (!e) return false;
    
    switch (e->kind) {
        case EX_RESET:
        case EX_SHIFT:
        case EX_SHIFT0:
        case EX_SERIAL_RESET:
        case EX_SERIAL_SHIFT:
        case EX_CLONEABLE_RESET:
        case EX_CLONEABLE_SHIFT:
            return true;
        case EX_LET:
            for (uint32_t i = 0; i < e->as.let_.n; i++) {
                if (cps_expr_contains_shift(e->as.let_.bindings[i].init)) return true;
            }
            return cps_expr_contains_shift(e->as.let_.body);
        case EX_IF:
            if (cps_expr_contains_shift(e->as.if_.cond)) return true;
            if (cps_expr_contains_shift(e->as.if_.then_)) return true;
            if (e->as.if_.else_or_null && cps_expr_contains_shift(e->as.if_.else_or_null)) return true;
            return false;
        case EX_DO:
            for (uint32_t i = 0; i < e->as.do_.n; i++) {
                if (cps_expr_contains_shift(e->as.do_.items[i])) return true;
            }
            return false;
        case EX_WHILE:
            return cps_expr_contains_shift(e->as.while_.cond) || 
                   cps_expr_contains_shift(e->as.while_.body);
        case EX_SET:
            return cps_expr_contains_shift(e->as.set_.value);
        case EX_DEF:
            return e->as.def_.init && cps_expr_contains_shift(e->as.def_.init);
        case EX_BUILTIN:
            for (uint32_t i = 0; i < e->as.builtin.n; i++) {
                if (cps_expr_contains_shift(e->as.builtin.args[i])) return true;
            }
            return false;
        case EX_FN_DEF:
            return cps_fn_needs_transform(e->as.fn_def_.fn);
        case EX_FN:
            return cps_fn_needs_transform(e->as.fn_.fn);
        case EX_CALL:
            for (uint32_t i = 0; i < e->as.call_.n_args; i++) {
                if (cps_expr_contains_shift(e->as.call_.args[i])) return true;
            }
            return false;
        case EX_CLOSURE:
            return cps_fn_needs_transform(e->as.closure_.closure->fn);
        case EX_DEFER:
            return cps_expr_contains_shift(e->as.defer_.body);
        case EX_RETURN:
            return e->as.return_.value && cps_expr_contains_shift(e->as.return_.value);
        case EX_CALLCC:
            /* call-cc-completion: descend into the receiver so a shift nested
             * inside (call/cc f)/(escape f) is still found. */
            return cps_expr_contains_shift(e->as.callcc_.fn);
        case EX_PANIC:
            return cps_expr_contains_shift(e->as.panic_.payload);
        case EX_PANIC_WITH:
            return cps_expr_contains_shift(e->as.panic_with_.payload);
        case EX_CATCH_UNWIND:
            return cps_expr_contains_shift(e->as.catch_unwind_.thunk);
        case EX_CATCH_PANIC_OF:
            return cps_expr_contains_shift(e->as.catch_panic_of_.thunk);
        case EX_PANIC_PAYLOAD_TYPE:
        case EX_PANIC_PAYLOAD_VALUE:
        case EX_PANIC_PAYLOAD_FILE:
        case EX_PANIC_PAYLOAD_LINE:
            return cps_expr_contains_shift(e->as.panic_payload_type_.payload);
        case EX_PANIC_PAYLOAD_DOWNS:
            return cps_expr_contains_shift(e->as.panic_payload_downs_.payload);
        /* Phase 19: Algebraic effects - these lower to shift/reset */
        case EX_DEFECT:
            return false; /* Effect definitions don't contain shift */
        /* DV1: Dynamic var nodes */
        case EX_DEFDYNAMIC:
        case EX_DYNVAR_READ:
        case EX_DYNVAR_SET:
            return false;
        case EX_DYNVAR_BINDING:
            for (uint32_t i = 0; i < e->as.dynvar_binding_.n_pairs; i++) {
                if (cps_expr_contains_shift(e->as.dynvar_binding_.pairs[i].override_expr))
                    return true;
            }
            return cps_expr_contains_shift(e->as.dynvar_binding_.body);
        case EX_AWAIT:
            /* cps-async (graduated 2026-07-19): `await` lowers to a dk_shift (or,
             * inside a handler case, delegates to the fiber path), so a function
             * containing it must always be CPS-colored. */
            return true;
        case EX_PERFORM:
            /* perform lowers to shift - always needs CPS marking */
            return true;
        case EX_HANDLE:
            /* handle lowers to reset - always needs CPS marking */
            if (cps_expr_contains_shift(e->as.handle_.handle->body)) return true;
            for (uint8_t i = 0; i < e->as.handle_.handle->n_cases; i++) {
                if (cps_expr_contains_shift(e->as.handle_.handle->cases[i].body)) return true;
            }
            return true; /* handle itself needs marking even if body doesn't */
        case EX_RESUME:
            /* resume is a tail call into a continuation - needs marking */
            return true;
        case EX_DISCONTINUE:
            /* discontinue is like throw but with a continuation - needs marking */
            return cps_expr_contains_shift(e->as.discontinue_.discontinue->exception);
        case EX_PROGRAM:
            for (uint32_t i = 0; i < e->as.program.n; i++) {
                if (cps_expr_contains_shift(e->as.program.items[i])) return true;
            }
            return false;
        case EX_REINTERPRET:
            return cps_expr_contains_shift(e->as.reinterpret_.expr);
        case EX_ASCRIBE:
            /* Type ascription is erased at codegen; a control op reachable only
             * through (:: <control-op> T) must still be seen by coloring. */
            return cps_expr_contains_shift(e->as.ascribe_.inner);
        default:
            return false;
    }
}

/* Check if a function definition needs (one-shot) CPS transformation */
bool cps_fn_needs_transform(const FnDef *fd) {
    if (!fd) return false;
    return cps_expr_contains_shift(fd->body);
}

/* SR2b: does this expression contain an actual SUSPENSION op -- a shift /
 * shift0 (any flavor), perform, or await?  Unlike cps_expr_contains_shift,
 * a `handle` or `reset` whose body is effect-free does NOT count, and a
 * handler case's own `resume` does not either (it only runs if the body
 * performs).  `(unsafe ...)` desugars to exactly such a handle -- a
 * discharge whose case is `(resume k nil)` -- and classifying every
 * unsafe-bodied fn as effectful minted ABI clones for plain inline-C
 * instance methods (typeclass-unsafe-passbyptr-struct-arg broke on the
 * clone's by-value param vs the body's pass-by-pointer reads).  Read by
 * emit_cps_ir_colored_fn_needs_mono, which wants "this fn's own body
 * suspends", not "this fn touches the effect system". */
bool cps_expr_contains_effect_op(const Expr *e) {
    if (!e) return false;
    switch (e->kind) {
        case EX_SHIFT:
        case EX_SHIFT0:
        case EX_SERIAL_SHIFT:
        case EX_CLONEABLE_SHIFT:
        case EX_PERFORM:
        case EX_AWAIT:
            return true;
        case EX_RESET:
            return cps_expr_contains_effect_op(e->as.reset_.body);
        case EX_SERIAL_RESET:
            return cps_expr_contains_effect_op(e->as.serial_reset_.body);
        case EX_CLONEABLE_RESET:
            return cps_expr_contains_effect_op(e->as.cloneable_reset_.body);
        case EX_HANDLE:
            if (cps_expr_contains_effect_op(e->as.handle_.handle->body)) return true;
            for (uint8_t i = 0; i < e->as.handle_.handle->n_cases; i++) {
                if (cps_expr_contains_effect_op(e->as.handle_.handle->cases[i].body)) return true;
            }
            return false;
        case EX_LET:
            for (uint32_t i = 0; i < e->as.let_.n; i++) {
                if (cps_expr_contains_effect_op(e->as.let_.bindings[i].init)) return true;
            }
            return cps_expr_contains_effect_op(e->as.let_.body);
        case EX_IF:
            return cps_expr_contains_effect_op(e->as.if_.cond) ||
                   cps_expr_contains_effect_op(e->as.if_.then_) ||
                   (e->as.if_.else_or_null &&
                    cps_expr_contains_effect_op(e->as.if_.else_or_null));
        case EX_DO:
            for (uint32_t i = 0; i < e->as.do_.n; i++) {
                if (cps_expr_contains_effect_op(e->as.do_.items[i])) return true;
            }
            return false;
        case EX_WHILE:
            return cps_expr_contains_effect_op(e->as.while_.cond) ||
                   cps_expr_contains_effect_op(e->as.while_.body);
        case EX_SET:
            return cps_expr_contains_effect_op(e->as.set_.value);
        case EX_DEF:
            return e->as.def_.init && cps_expr_contains_effect_op(e->as.def_.init);
        case EX_BUILTIN:
            for (uint32_t i = 0; i < e->as.builtin.n; i++) {
                if (cps_expr_contains_effect_op(e->as.builtin.args[i])) return true;
            }
            return false;
        case EX_FN_DEF:
            return e->as.fn_def_.fn &&
                   cps_expr_contains_effect_op(e->as.fn_def_.fn->body);
        case EX_FN:
            return e->as.fn_.fn && cps_expr_contains_effect_op(e->as.fn_.fn->body);
        case EX_CLOSURE:
            return e->as.closure_.closure->fn &&
                   cps_expr_contains_effect_op(e->as.closure_.closure->fn->body);
        case EX_CALL:
            for (uint32_t i = 0; i < e->as.call_.n_args; i++) {
                if (cps_expr_contains_effect_op(e->as.call_.args[i])) return true;
            }
            return false;
        case EX_DEFER:
            return cps_expr_contains_effect_op(e->as.defer_.body);
        case EX_RETURN:
            return e->as.return_.value &&
                   cps_expr_contains_effect_op(e->as.return_.value);
        case EX_CALLCC:
            return cps_expr_contains_effect_op(e->as.callcc_.fn);
        case EX_PANIC:
            return cps_expr_contains_effect_op(e->as.panic_.payload);
        case EX_PANIC_WITH:
            return cps_expr_contains_effect_op(e->as.panic_with_.payload);
        case EX_CATCH_UNWIND:
            return cps_expr_contains_effect_op(e->as.catch_unwind_.thunk);
        case EX_CATCH_PANIC_OF:
            return cps_expr_contains_effect_op(e->as.catch_panic_of_.thunk);
        case EX_PANIC_PAYLOAD_TYPE:
        case EX_PANIC_PAYLOAD_VALUE:
        case EX_PANIC_PAYLOAD_FILE:
        case EX_PANIC_PAYLOAD_LINE:
            return cps_expr_contains_effect_op(e->as.panic_payload_type_.payload);
        case EX_PANIC_PAYLOAD_DOWNS:
            return cps_expr_contains_effect_op(e->as.panic_payload_downs_.payload);
        case EX_DYNVAR_BINDING:
            for (uint32_t i = 0; i < e->as.dynvar_binding_.n_pairs; i++) {
                if (cps_expr_contains_effect_op(
                        e->as.dynvar_binding_.pairs[i].override_expr))
                    return true;
            }
            return cps_expr_contains_effect_op(e->as.dynvar_binding_.body);
        case EX_RESUME:
        case EX_DISCONTINUE:
            /* Only reachable inside a handler case; the case runs only when
             * the handle's body suspends, which the body walk above finds. */
            return false;
        case EX_REINTERPRET:
            return cps_expr_contains_effect_op(e->as.reinterpret_.expr);
        case EX_ASCRIBE:
            return cps_expr_contains_effect_op(e->as.ascribe_.inner);
        default:
            return false;
    }
}

/* Phase B2: Check if an expression contains cloneable-shift or cloneable-reset. */
bool cps_expr_contains_cloneable_shift(const Expr *e) {
    if (!e) return false;

    switch (e->kind) {
        case EX_CLONEABLE_RESET:
        case EX_CLONEABLE_SHIFT:
            return true;
        case EX_LET:
            for (uint32_t i = 0; i < e->as.let_.n; i++) {
                if (cps_expr_contains_cloneable_shift(e->as.let_.bindings[i].init))
                    return true;
            }
            return cps_expr_contains_cloneable_shift(e->as.let_.body);
        case EX_IF:
            if (cps_expr_contains_cloneable_shift(e->as.if_.cond)) return true;
            if (cps_expr_contains_cloneable_shift(e->as.if_.then_)) return true;
            return e->as.if_.else_or_null &&
                   cps_expr_contains_cloneable_shift(e->as.if_.else_or_null);
        case EX_DO:
            for (uint32_t i = 0; i < e->as.do_.n; i++) {
                if (cps_expr_contains_cloneable_shift(e->as.do_.items[i]))
                    return true;
            }
            return false;
        case EX_WHILE:
            return cps_expr_contains_cloneable_shift(e->as.while_.cond) ||
                   cps_expr_contains_cloneable_shift(e->as.while_.body);
        case EX_FN_DEF:
            return cps_fn_needs_cloneable_transform(e->as.fn_def_.fn);
        case EX_FN:
            return cps_fn_needs_cloneable_transform(e->as.fn_.fn);
        case EX_CALL:
            for (uint32_t i = 0; i < e->as.call_.n_args; i++) {
                if (cps_expr_contains_cloneable_shift(e->as.call_.args[i]))
                    return true;
            }
            return cps_expr_contains_cloneable_shift(e->as.call_.fn_expr);
        case EX_CLOSURE:
            return cps_fn_needs_cloneable_transform(e->as.closure_.closure->fn);
        case EX_RETURN:
            return e->as.return_.value &&
                   cps_expr_contains_cloneable_shift(e->as.return_.value);
        case EX_PROGRAM:
            for (uint32_t i = 0; i < e->as.program.n; i++) {
                if (cps_expr_contains_cloneable_shift(e->as.program.items[i]))
                    return true;
            }
            return false;
        /* Control / value forms that can nest a cloneable-reset/-shift.  Omitting
         * any left the tur_cloneable_cont prelude ungated when the cloneable form
         * hid under it -- e.g. `(+ 1 (cloneable-reset ...))` -- an "unknown type
         * name 'tur_cloneable_cont'" build error.  Additive: this only ever emits
         * the prelude when a cloneable form is actually present. */
        case EX_BUILTIN:
            for (uint32_t i = 0; i < e->as.builtin.n; i++)
                if (cps_expr_contains_cloneable_shift(e->as.builtin.args[i])) return true;
            return false;
        case EX_MATCH:
            if (cps_expr_contains_cloneable_shift(e->as.match_.scrutinee)) return true;
            for (uint32_t i = 0; i < e->as.match_.n_arms; i++)
                if (cps_expr_contains_cloneable_shift(e->as.match_.arms[i].guard) ||
                    cps_expr_contains_cloneable_shift(e->as.match_.arms[i].body)) return true;
            return false;
        case EX_RESET:  return cps_expr_contains_cloneable_shift(e->as.reset_.body);
        case EX_SHIFT:  return cps_expr_contains_cloneable_shift(e->as.shift_.k_fn) ||
                               cps_expr_contains_cloneable_shift(e->as.shift_.body);
        case EX_SHIFT0: return cps_expr_contains_cloneable_shift(e->as.shift0_.k_fn) ||
                               cps_expr_contains_cloneable_shift(e->as.shift0_.body);
        case EX_SERIAL_RESET: return cps_expr_contains_cloneable_shift(e->as.serial_reset_.body);
        case EX_SERIAL_SHIFT: return cps_expr_contains_cloneable_shift(e->as.serial_shift_.k_fn) ||
                                     cps_expr_contains_cloneable_shift(e->as.serial_shift_.body);
        case EX_CALLCC: return cps_expr_contains_cloneable_shift(e->as.callcc_.fn);
        case EX_PERFORM:
            if (e->as.perform_.perform)
                for (uint32_t i = 0; i < e->as.perform_.perform->n_args; i++)
                    if (cps_expr_contains_cloneable_shift(e->as.perform_.perform->args[i])) return true;
            return false;
        case EX_HANDLE:
        case EX_HANDLER_LIT:
            if (e->as.handle_.handle) {
                HandleExpr *h = e->as.handle_.handle;
                if (cps_expr_contains_cloneable_shift(h->body)) return true;
                for (uint8_t i = 0; i < h->n_cases; i++)
                    if (cps_expr_contains_cloneable_shift(h->cases[i].body)) return true;
            }
            return false;
        case EX_RESUME:
            return e->as.resume_.resume &&
                   (cps_expr_contains_cloneable_shift(e->as.resume_.resume->k) ||
                    cps_expr_contains_cloneable_shift(e->as.resume_.resume->value));
        case EX_DISCONTINUE:
            return e->as.discontinue_.discontinue &&
                   (cps_expr_contains_cloneable_shift(e->as.discontinue_.discontinue->k) ||
                    cps_expr_contains_cloneable_shift(e->as.discontinue_.discontinue->exception));
        case EX_WITH_HANDLER: return cps_expr_contains_cloneable_shift(e->as.with_handler_.handler) ||
                                     cps_expr_contains_cloneable_shift(e->as.with_handler_.body);
        case EX_ASYNC:  return cps_expr_contains_cloneable_shift(e->as.async_.fn_expr);
        case EX_AWAIT:  return cps_expr_contains_cloneable_shift(e->as.await_.fut_expr);
        case EX_SET:    return cps_expr_contains_cloneable_shift(e->as.set_.value);
        case EX_DEF:    return e->as.def_.init && cps_expr_contains_cloneable_shift(e->as.def_.init);
        case EX_DEFER:  return cps_expr_contains_cloneable_shift(e->as.defer_.body);
        case EX_MAKE_STRUCT:
            for (uint32_t i = 0; i < e->as.make_struct_.n_fields; i++)
                if (cps_expr_contains_cloneable_shift(e->as.make_struct_.field_values[i])) return true;
            return false;
        case EX_GET_FIELD: return cps_expr_contains_cloneable_shift(e->as.get_field_.struct_expr);
        case EX_SET_FIELD: return cps_expr_contains_cloneable_shift(e->as.set_field_.receiver) ||
                                  cps_expr_contains_cloneable_shift(e->as.set_field_.value);
        case EX_REF:          return cps_expr_contains_cloneable_shift(e->as.ref_.expr);
        case EX_DEREF:        return cps_expr_contains_cloneable_shift(e->as.deref_.expr);
        case EX_BORROW_IMMUT: return cps_expr_contains_cloneable_shift(e->as.borrow_immut_.expr);
        case EX_BORROW_MUT:   return cps_expr_contains_cloneable_shift(e->as.borrow_mut_.expr);
        case EX_ASCRIBE:      return cps_expr_contains_cloneable_shift(e->as.ascribe_.inner);
        case EX_REINTERPRET:  return cps_expr_contains_cloneable_shift(e->as.reinterpret_.expr);
        case EX_CAST:         return cps_expr_contains_cloneable_shift(e->as.cast_.expr);
        case EX_FN_TO_FAT:    return cps_expr_contains_cloneable_shift(e->as.fn_to_fat_.inner);
        case EX_POLY_TO_FAT:  return cps_expr_contains_cloneable_shift(e->as.poly_to_fat_.inner);
        case EX_POLY_WRAP:    return cps_expr_contains_cloneable_shift(e->as.poly_wrap_.inner);
        default:
            return false;
    }
}

/* Phase B2: Check if a function definition needs cloneable CPS transformation. */
bool cps_fn_needs_cloneable_transform(const FnDef *fd) {
    if (!fd) return false;
    return cps_expr_contains_cloneable_shift(fd->body);
}

/* =========================================================================
 * CPS1 (cps-transform-plan): whole-program "may-capture" coloring analysis.
 *
 * See CPS0.1 for the ratified rule. We build a call graph over the top-level
 * functions, seed the colored set with functions whose body directly contains a
 * control-op node, conservatively color any function that makes an unresolved
 * (indirect/extern/local-value) call, and propagate backward to a fixed point.
 * Calls to nested lambdas are unresolved and hence covered by the conservative
 * rule, so the node set can be restricted to top-level functions while staying
 * sound for the top-level coloring this exposes.
 * ========================================================================= */

/* ---------------------------------------------------------------------------
 * effect-row-lost-through-a-constructor-argument: generic child enumeration.
 *
 * The two whole-program walks below -- `cps_directly_uses_control` (which SEEDS
 * a function that itself holds a control op) and `cps_collect_calls` (which
 * builds the call-graph EDGES the coloring fixpoint propagates along) -- were
 * each written as a switch over the node kinds that existed at the time, with
 * `default:` meaning "no children".  Every node kind added since has therefore
 * been a silent hole: the subtree under it is not walked, so a `perform` or a
 * call to an effectful defn hidden below it does not color its function, the
 * handler's DK is never threaded through, and the program aborts with
 * `tur: unhandled effect (tag N)` -- having type-checked clean, because
 * `collect_effects_in_expr` (src/passes/effect_check.c) has the arm and infers
 * the row correctly.  `--dump-effects` says `#{Ask}` while
 * `--dump-cps-coloring` says `uncolored`; that disagreement IS the bug.
 *
 * This has now been filed three separate times, each as one more missing arm:
 * cps-coloring-walk-has-no-arm-for-union-inject,
 * saffron-any-return-defeats-the-frame-box-rule, and
 * saffron-effect-row-lost-through-unannotated-call.  The shape that opened THIS
 * report -- `(.v (Box (g)))`, an effectful call as a constructor argument -- is
 * the same hole seen through `EX_MAKE_STRUCT` and `EX_GET_FIELD`, neither of
 * which either walk had ever had an arm for.
 *
 * So rather than add two more arms and wait for the fourth report, both walks
 * now fall back to this one enumeration, which covers every kind that carries
 * an evaluated operand.  Adding an arm to a walk is now a choice about
 * NON-uniform treatment (a call-graph edge, a boundary, an unconditional seed),
 * not a prerequisite for the subtree being visited at all.
 *
 * `visit` returns true to stop the walk early; the enumerator returns whether
 * it stopped.  Nested function definitions (EX_FN_DEF / EX_FN / EX_CLOSURE) are
 * call-graph boundaries and are NOT enumerated here -- both walks keep their
 * own explicit arms for them, and the enumerator agrees by omission.
 * Declaration nodes (defdata / defgadt / defmodule / typeclass / instance /
 * extern-c / inline-c) likewise have no evaluated operand in the enclosing
 * body.
 * ------------------------------------------------------------------------- */
bool cps_visit_children(const Expr *e,
                        bool (*visit)(const Expr *, void *),
                        void *ud) {
    if (!e) return false;

#define V(x)  do { const Expr *c_ = (x); if (c_ && visit(c_, ud)) return true; } while (0)
#define VN(arr, n) do { for (uint32_t i_ = 0; i_ < (uint32_t)(n); i_++) V((arr)[i_]); } while (0)

    switch (e->kind) {
        /* --- binding / sequencing ------------------------------------- */
        case EX_LET:
        case EX_LETREC:
            for (uint32_t i = 0; i < e->as.let_.n; i++)
                V(e->as.let_.bindings[i].init);
            V(e->as.let_.body);
            return false;
        case EX_IF:
            V(e->as.if_.cond); V(e->as.if_.then_); V(e->as.if_.else_or_null);
            return false;
        case EX_DO:      VN(e->as.do_.items, e->as.do_.n);     return false;
        case EX_PROGRAM: VN(e->as.program.items, e->as.program.n); return false;
        case EX_WHILE:   V(e->as.while_.cond); V(e->as.while_.body); return false;
        case EX_SET:     V(e->as.set_.value);  return false;
        case EX_DEF:     V(e->as.def_.init);   return false;
        case EX_RETURN:  V(e->as.return_.value); return false;
        case EX_DEFER:   V(e->as.defer_.body); return false;

        /* --- calls ------------------------------------------------------ */
        case EX_CALL:
            V(e->as.call_.fn_expr);
            VN(e->as.call_.args, e->as.call_.n_args);
            return false;
        case EX_BUILTIN: VN(e->as.builtin.args, e->as.builtin.n); return false;
        case EX_CONS_LIST: VN(e->as.cons_list_.items, e->as.cons_list_.n); return false;

        /* --- aggregates and field access -------------------------------
         * The hole this report was opened for.  `(Box (g))` is an
         * EX_MAKE_STRUCT whose field value is the effectful call, and the
         * `.v` reading it back is an EX_GET_FIELD. */
        case EX_MAKE_STRUCT:
            VN(e->as.make_struct_.field_values, e->as.make_struct_.n_fields);
            return false;
        case EX_GET_FIELD: V(e->as.get_field_.struct_expr); return false;
        case EX_SET_FIELD: V(e->as.set_field_.receiver); V(e->as.set_field_.value); return false;
        case EX_SET_LIT:   VN(e->as.set_lit_.items, e->as.set_lit_.n); return false;

        /* --- match ------------------------------------------------------ */
        case EX_MATCH:
            V(e->as.match_.scrutinee);
            for (uint32_t i = 0; i < e->as.match_.n_arms; i++) {
                V(e->as.match_.arms[i].body);
                V(e->as.match_.arms[i].guard);
            }
            return false;

        /* --- effects and delimited control ------------------------------ */
        case EX_PERFORM:
            VN(e->as.perform_.perform->args, e->as.perform_.perform->n_args);
            return false;
        case EX_HANDLE: {
            const HandleExpr *h = e->as.handle_.handle;
            V(h->body);
            for (uint8_t i = 0; i < h->n_cases; i++) V(h->cases[i].body);
            return false;
        }
        case EX_HANDLER_LIT: {
            const HandleExpr *h = e->as.handler_lit_.handle;
            for (uint8_t i = 0; i < h->n_cases; i++) V(h->cases[i].body);
            return false;
        }
        case EX_WITH_HANDLER:     V(e->as.with_handler_.handler); V(e->as.with_handler_.body); return false;
        case EX_COMPOSE_HANDLERS: V(e->as.compose_handlers_.h1); V(e->as.compose_handlers_.h2); return false;
        case EX_RESUME:           V(e->as.resume_.resume->k); V(e->as.resume_.resume->value); return false;
        case EX_DISCONTINUE:      V(e->as.discontinue_.discontinue->k);
                                  V(e->as.discontinue_.discontinue->exception); return false;
        case EX_CONT_PRED:        V(e->as.cont_pred_.expr); return false;
        case EX_RESET:            V(e->as.reset_.body); return false;
        case EX_SHIFT:            V(e->as.shift_.k_fn); V(e->as.shift_.body); return false;
        case EX_SHIFT0:           V(e->as.shift0_.k_fn); V(e->as.shift0_.body); return false;
        case EX_CALLCC:           V(e->as.callcc_.fn); return false;
        case EX_CLONEABLE_RESET:  V(e->as.cloneable_reset_.body); return false;
        case EX_CLONEABLE_SHIFT:  V(e->as.cloneable_shift_.k_fn);
                                  V(e->as.cloneable_shift_.body); return false;
        case EX_SERIAL_RESET:     V(e->as.serial_reset_.body); return false;
        case EX_SERIAL_SHIFT:     V(e->as.serial_shift_.k_fn); V(e->as.serial_shift_.body); return false;
        case EX_CPS_CONT_APP:     V(e->as.cps_cont_app_.cont); V(e->as.cps_cont_app_.value); return false;

        /* --- concurrency ------------------------------------------------ */
        case EX_ASYNC:      V(e->as.async_.fn_expr);  return false;
        case EX_AWAIT:      V(e->as.await_.fut_expr); return false;
        case EX_SELECT:
            for (uint32_t i = 0; i < e->as.select_.n_clauses; i++) {
                V(e->as.select_.clauses[i].chan);
                V(e->as.select_.clauses[i].send_val);
                V(e->as.select_.clauses[i].body);
            }
            V(e->as.select_.default_body);
            return false;
        case EX_STM:        VN(e->as.stm_.body, e->as.stm_.n_body); return false;
        case EX_ATOMICALLY: V(e->as.atomically_.stm_expr); return false;
        case EX_CHECK:      V(e->as.check_.cond); return false;
        case EX_OR_ELSE:    V(e->as.or_else_.stm1); V(e->as.or_else_.stm2); return false;
        case EX_TVAR_NEW:    V(e->as.tvar_new_.init);  return false;
        case EX_TVAR_READ:   V(e->as.tvar_read_.tvar); return false;
        case EX_TVAR_WRITE:  V(e->as.tvar_write_.tvar);  V(e->as.tvar_write_.value);   return false;
        case EX_TVAR_MODIFY: V(e->as.tvar_modify_.tvar); V(e->as.tvar_modify_.fn);     return false;
        case EX_TVAR_SWAP:   V(e->as.tvar_swap_.tvar);   V(e->as.tvar_swap_.new_val);  return false;
        case EX_TVAR_CAS:    V(e->as.tvar_cas_.tvar);    V(e->as.tvar_cas_.old_val);
                             V(e->as.tvar_cas_.new_val); return false;

        /* --- generators -------------------------------------------------- */
        case EX_YIELD:    V(e->as.yield_.value);        return false;
        case EX_GEN_NEXT: V(e->as.gen_next_.gen_expr);  return false;
        case EX_GEN_UNWRAP: V(e->as.gen_unwrap_.ptr_expr); return false;
        case EX_GEN_DONE: V(e->as.gen_done_.gen_expr);  return false;

        /* --- dynamic vars ------------------------------------------------ */
        case EX_DEFDYNAMIC: V(e->as.defdynamic_.root_expr); return false;
        case EX_DYNVAR_SET: V(e->as.dynvar_set_.value);     return false;
        case EX_DYNVAR_BINDING:
            for (uint32_t i = 0; i < e->as.dynvar_binding_.n_pairs; i++)
                V(e->as.dynvar_binding_.pairs[i].override_expr);
            V(e->as.dynvar_binding_.body);
            return false;

        /* --- references, rc, borrows ------------------------------------- */
        case EX_REF:          V(e->as.ref_.expr);           return false;
        case EX_DEREF:        V(e->as.deref_.expr);         return false;
        case EX_SET_DEREF:    V(e->as.set_deref_.ref); V(e->as.set_deref_.value); return false;
        case EX_RC_OF:        V(e->as.rc_of_.expr);         return false;
        case EX_RC_CLONE:     V(e->as.rc_clone_.expr);      return false;
        case EX_RC_DROP:      V(e->as.rc_drop_.expr);       return false;
        case EX_RC_PTR:       V(e->as.rc_ptr_.expr);        return false;
        case EX_RC_COUNT:     V(e->as.rc_count_.expr);      return false;
        case EX_RC_FROM_REF:  V(e->as.rc_from_ref_.expr);   return false;
        case EX_REF_FROM_RC:  V(e->as.ref_from_rc_.expr);   return false;
        case EX_WEAK:         V(e->as.weak_.expr);          return false;
        case EX_WEAK_UPGRADE: V(e->as.weak_upgrade_.expr);  return false;
        case EX_WEAK_PRED:    V(e->as.weak_pred_.expr);     return false;
        case EX_REF_PRED:     V(e->as.ref_pred_.expr);      return false;
        case EX_BORROW_IMMUT: V(e->as.borrow_immut_.expr);  return false;
        case EX_BORROW_MUT:   V(e->as.borrow_mut_.expr);    return false;

        /* --- panics ------------------------------------------------------ */
        case EX_PANIC:                V(e->as.panic_.payload);                return false;
        case EX_PANIC_WITH:           V(e->as.panic_with_.payload);           return false;
        case EX_CATCH_UNWIND:         V(e->as.catch_unwind_.thunk);           return false;
        case EX_CATCH_PANIC_OF:       V(e->as.catch_panic_of_.thunk);         return false;
        case EX_PANIC_PAYLOAD_TYPE:   V(e->as.panic_payload_type_.payload);   return false;
        case EX_PANIC_PAYLOAD_VALUE:  V(e->as.panic_payload_value_.payload);  return false;
        case EX_PANIC_PAYLOAD_FILE:   V(e->as.panic_payload_file_.payload);   return false;
        case EX_PANIC_PAYLOAD_LINE:   V(e->as.panic_payload_line_.payload);   return false;
        case EX_PANIC_PAYLOAD_DOWNS:  V(e->as.panic_payload_downs_.payload);  return false;

        /* --- erased / representation wrappers ---------------------------- */
        case EX_ASCRIBE:     V(e->as.ascribe_.inner);     return false;
        case EX_CAST:        V(e->as.cast_.expr);         return false;
        case EX_REINTERPRET: V(e->as.reinterpret_.expr);  return false;
        case EX_POLY_WRAP:   V(e->as.poly_wrap_.inner);   return false;
        case EX_FN_TO_FAT:   V(e->as.fn_to_fat_.inner);   return false;
        case EX_POLY_TO_FAT: V(e->as.poly_to_fat_.inner); return false;

        /* --- existentials ------------------------------------------------ */
        case EX_EXISTS_PACK:     V(e->as.exists_pack_.value); return false;
        case EX_EXISTS_OPEN:     V(e->as.exists_open_.packed); V(e->as.exists_open_.body); return false;
        case EX_EXISTS_DISPATCH: VN(e->as.exists_dispatch_.args, e->as.exists_dispatch_.n_args);
                                 return false;

        /* --- Saffron dynamic surface ------------------------------------- */
        case EX_UNION_INJECT: V(e->as.union_inject_.value);  return false;
        case EX_ANY_CAST:     V(e->as.any_cast_.value);      return false;
        case EX_ANY_IS:       V(e->as.any_is_.value);        return false;
        case EX_ANY_TYPE_OF:  V(e->as.any_type_of_.value);   return false;
        case EX_DYN_OP:       VN(e->as.dyn_op_.args, e->as.dyn_op_.n_args); return false;
        case EX_DYN_CALL:     V(e->as.dyn_call_.fn);
                              VN(e->as.dyn_call_.args, e->as.dyn_call_.n_args); return false;
        case EX_DYN_FIELD:    V(e->as.dyn_field_.obj); return false;
        case EX_DYN_METHOD:   V(e->as.dyn_method_.obj);
                              VN(e->as.dyn_method_.args, e->as.dyn_method_.n_args); return false;

        /* Leaves: literals, variables, dict singletons, `(retry)`, dynvar
         * reads, generator values, and every declaration form.  Nested fn
         * definitions are boundaries, not leaves -- see the header comment. */
        default:
            return false;
    }
#undef VN
#undef V
}

/* True iff e's subtree DIRECTLY contains a control-op node, WITHOUT descending
 * into nested function definitions (each nested fn is colored on its own merits;
 * reaching it is modeled as an unresolved call by the caller). This mirrors the
 * trusted enumeration in cps_expr_contains_shift, plus the serial operators. */
static bool cps_directly_uses_control(const Expr *e);
bool cps_expr_uses_control(const Expr *e) { return cps_directly_uses_control(e); }

/* Visitor thunk for the enumerator's default arm: "stop" means "found one". */
static bool cps_control_visit(const Expr *c, void *ud) {
    (void)ud;
    return cps_directly_uses_control(c);
}

static bool cps_directly_uses_control(const Expr *e) {
    if (!e) return false;
    switch (e->kind) {
        /* Seeds: any of these IR nodes is a control operator. */
        case EX_RESET:
        case EX_SHIFT:
        case EX_SHIFT0:
        case EX_CLONEABLE_RESET:
        case EX_CLONEABLE_SHIFT:
        case EX_SERIAL_RESET:
        case EX_SERIAL_SHIFT:
        case EX_PERFORM:
        case EX_HANDLE:
        case EX_RESUME:
        case EX_DISCONTINUE:
        /* (call/cc f) / (escape f): the native CT-IR path (build_callcc /
         * emit_callcc) lowers these on the DK-threaded backend, so a function that
         * uses them is a CPS candidate -- color it so ensure_S classifies it and it
         * emits natively instead of wholly direct-emitting through emit_cps_callcc. */
        case EX_CALLCC:
            return true;
        /* cps-async (graduated 2026-07-19): `await` lowers to a dk_shift (or, in a
         * handler case, delegates to the fiber path), so it is always a control
         * seed that colors its function. */
        case EX_AWAIT:
            return true;
        /* Structural recursion (no descent into nested fn bodies). */
        case EX_LET:
            for (uint32_t i = 0; i < e->as.let_.n; i++)
                if (cps_directly_uses_control(e->as.let_.bindings[i].init)) return true;
            return cps_directly_uses_control(e->as.let_.body);
        case EX_IF:
            return cps_directly_uses_control(e->as.if_.cond)
                || cps_directly_uses_control(e->as.if_.then_)
                || cps_directly_uses_control(e->as.if_.else_or_null);
        case EX_DO:
            for (uint32_t i = 0; i < e->as.do_.n; i++)
                if (cps_directly_uses_control(e->as.do_.items[i])) return true;
            return false;
        case EX_WHILE:
            return cps_directly_uses_control(e->as.while_.cond)
                || cps_directly_uses_control(e->as.while_.body);
        case EX_SET:
            return cps_directly_uses_control(e->as.set_.value);
        case EX_DEF:
            return cps_directly_uses_control(e->as.def_.init);
        case EX_BUILTIN:
            for (uint32_t i = 0; i < e->as.builtin.n; i++)
                if (cps_directly_uses_control(e->as.builtin.args[i])) return true;
            return false;
        case EX_CALL:
            if (cps_directly_uses_control(e->as.call_.fn_expr)) return true;
            for (uint32_t i = 0; i < e->as.call_.n_args; i++)
                if (cps_directly_uses_control(e->as.call_.args[i])) return true;
            return false;
        case EX_DEFER:
            return cps_directly_uses_control(e->as.defer_.body);
        case EX_RETURN:
            return cps_directly_uses_control(e->as.return_.value);
        case EX_PANIC:
            return cps_directly_uses_control(e->as.panic_.payload);
        case EX_PANIC_WITH:
            return cps_directly_uses_control(e->as.panic_with_.payload);
        case EX_PANIC_PAYLOAD_TYPE:
        case EX_PANIC_PAYLOAD_VALUE:
        case EX_PANIC_PAYLOAD_FILE:
        case EX_PANIC_PAYLOAD_LINE:
            return cps_directly_uses_control(e->as.panic_payload_type_.payload);
        case EX_PANIC_PAYLOAD_DOWNS:
            return cps_directly_uses_control(e->as.panic_payload_downs_.payload);
        case EX_CATCH_UNWIND:
            return cps_directly_uses_control(e->as.catch_unwind_.thunk);
        case EX_CATCH_PANIC_OF:
            return cps_directly_uses_control(e->as.catch_panic_of_.thunk);
        case EX_DYNVAR_BINDING:
            for (uint32_t i = 0; i < e->as.dynvar_binding_.n_pairs; i++)
                if (cps_directly_uses_control(e->as.dynvar_binding_.pairs[i].override_expr))
                    return true;
            return cps_directly_uses_control(e->as.dynvar_binding_.body);
        case EX_REINTERPRET:
            return cps_directly_uses_control(e->as.reinterpret_.expr);
        case EX_ASCRIBE:
            /* Ascription is erased at codegen; seed on a control op that is
             * only reachable through (:: <control-op> T). */
            return cps_directly_uses_control(e->as.ascribe_.inner);
        /* cps-coloring-walk-has-no-arm-for-union-inject: the same reasoning as
         * EX_ASCRIBE above, for the four nodes a `#lang saffron` file wraps its
         * expressions in.  Without these arms each falls to `default: return
         * false`, so a function whose only control op sits under one is never
         * COLORED -- and the candidate loop in cps_color_program is gated on
         * `cps_colored`, so the signature gate is never even consulted.  The
         * op then reaches the direct emitter, which has no lowering for one,
         * and aborts:
         *
         *   tur: emit: EX_CALLCC reached the direct emitter
         *
         * EX_UNION_INJECT is the return-position widen an `any` return type
         * inserts, which made `(defn go [] (call/cc ...))` -- an unannotated
         * Saffron defn, i.e. the ordinary case -- abort while the same function
         * annotated `: int` compiled.  Three of the four have a fixture
         * (saffron-callcc-under-any-return, -under-dyn-op, -under-dyn-call).
         *
         * EX_DYN_FIELD has NO fixture: a dyn field's only child is its
         * receiver, and every attempt to give it an `any`-typed control op as
         * that receiver was refused earlier, by the field resolver ("no
         * typeclass method found for 'n'").  The arm is here by construction --
         * the child is an expression and can structurally hold a control op --
         * not because a repro was found.  If one is ever built, add the
         * fixture; do not take this comment as evidence the shape reaches here.
         *
         * Same class as docs/archive/saffron-any-return-defeats-the-frame-box-
         * rule.md, where `expr_subtree_has_inline_c` was missing arms for these
         * same nodes and silently skipped every Saffron function. */
        case EX_UNION_INJECT:
            return cps_directly_uses_control(e->as.union_inject_.value);
        case EX_DYN_OP:
            for (uint32_t i = 0; i < e->as.dyn_op_.n_args; i++)
                if (cps_directly_uses_control(e->as.dyn_op_.args[i])) return true;
            return false;
        case EX_DYN_CALL:
            if (cps_directly_uses_control(e->as.dyn_call_.fn)) return true;
            for (uint32_t i = 0; i < e->as.dyn_call_.n_args; i++)
                if (cps_directly_uses_control(e->as.dyn_call_.args[i])) return true;
            return false;
        case EX_DYN_FIELD:
            return cps_directly_uses_control(e->as.dyn_field_.obj);
        /* saffron-lang-plan S9: the dispatch node, same shape as the three
         * above -- receiver plus arguments, any of which can hold a control op. */
        case EX_DYN_METHOD:
            if (cps_directly_uses_control(e->as.dyn_method_.obj)) return true;
            for (uint32_t i = 0; i < e->as.dyn_method_.n_args; i++)
                if (cps_directly_uses_control(e->as.dyn_method_.args[i])) return true;
            return false;
        /* ...and the `any` READERS, which the first round of arms missed
         * because nothing then put one between a defn's body and its control
         * op.  The return-position seam does: a concrete return annotation over
         * a dynamic body now narrows through `EX_ANY_CAST`, so `(defn go [x] :
         * int (+ x (call/cc ...)))` had its only control op behind a cast, fell
         * to `default: return false`, and went UNCOLORED -- the exact failure
         * this walk's other arms exist to prevent, reintroduced by a node added
         * later.  A reader is transparent to control the same way an ascription
         * is. */
        case EX_ANY_CAST:
            return cps_directly_uses_control(e->as.any_cast_.value);
        case EX_ANY_IS:
            return cps_directly_uses_control(e->as.any_is_.value);
        case EX_ANY_TYPE_OF:
            return cps_directly_uses_control(e->as.any_type_of_.value);
        /* B3 (cps-tramp-resume): a `(with-handler hv body)` / `(compose-handlers
         * ...)` is a delimited handler install -- a control seed under the flag,
         * so a `main`/fn whose only control op is a first-class handler value is
         * colored and its inner `perform` is not hidden.  build_with_handler
         * DK-lowers a literal (or compose-of-literals) handler value; a dynamic
         * handler value evicts and falls back gracefully.  Unconditional since
         * cps-tramp-resume graduated (2026-07-19); the fiber-lowered
         * with-handler path it used to preserve is gone. */
        case EX_WITH_HANDLER:
        case EX_COMPOSE_HANDLERS:
            return true;
        /* A control op (perform / shift / handle / ...) inside a `match` arm colors
         * its function just like one in an `if` branch -- without this recursion a
         * `(defn pick [b] (match b (Full v) (+ v (perform (Choose))) ...))` reads as
         * uncolored and fiber-performs the effect, tainting it for the enclosing
         * DK handler (cps-backend-effect-under-match).  Unconditional since
         * cps-tramp-resume graduated (2026-07-19). */
        case EX_MATCH:
            if (cps_directly_uses_control(e->as.match_.scrutinee)) return true;
            for (uint32_t i = 0; i < e->as.match_.n_arms; i++) {
                if (cps_directly_uses_control(e->as.match_.arms[i].body)) return true;
                if (cps_directly_uses_control(e->as.match_.arms[i].guard)) return true;
            }
            return false;
        /* Nested function definitions are call-graph boundaries. */
        case EX_FN_DEF:
        case EX_FN:
        case EX_CLOSURE:
            return false;
        /* effect-row-lost-through-a-constructor-argument: every other node kind
         * walks its evaluated operands through the shared enumerator, instead of
         * reading as "no control here" because this switch was written before the
         * node existed.  `(.v (Box (perform (Ask))))` is the shape that was
         * escaping -- neither EX_GET_FIELD nor EX_MAKE_STRUCT had an arm. */
        default:
            return cps_visit_children(e, cps_control_visit, NULL);
    }
}

/* One call-graph node, one per top-level function. */
typedef struct CpsNode {
    FnDef    *fd;
    bool      colored;
    bool      has_indirect;   /* makes an unresolved call -> conservatively colored */
    uint32_t *edges;          /* indices of resolved top-level callees */
    uint32_t  n_edges, cap_edges;
} CpsNode;

static void cps_node_add_edge(CpsNode *self, uint32_t target) {
    for (uint32_t i = 0; i < self->n_edges; i++)
        if (self->edges[i] == target) return; /* dedup */
    if (self->n_edges == self->cap_edges) {
        self->cap_edges = self->cap_edges ? self->cap_edges * 2 : 4;
        self->edges = realloc(self->edges, self->cap_edges * sizeof(uint32_t));
    }
    self->edges[self->n_edges++] = target;
}

/* The C symbol a binding resolves to, for call-target identity.  A captureless
 * letrec lambda -- the named-let `(let go [...] ...)` idiom -- binds `go` to a
 * DIFFERENT Binding object than the lifted `__fn_N`'s own FnDef binding, and
 * records the lifted function's C name in `c_export_name`.  That is precisely
 * how the emitter resolves the call (raw_name_for_binding consults
 * c_export_name first, and emit_fns.c's self-TCO check compares the same
 * mangled names), so it is the identity the coloring analysis must use too. */
static const char *cps_binding_c_symbol(const Binding *b) {
    if (!b) return NULL;
    if (b->c_export_name) return b->c_export_name;
    return b->name ? b->name->name : NULL;
}

/* The C symbol a CALL through `b` names, for the lookup side of the fallback
 * below.  A local or a parameter names a function only through c_export_name
 * (the named-let alias above).  Otherwise it is a value: `(f x)` through a
 * parameter `f` calls whatever the caller passed, never the top-level `f` that
 * happens to share its name.  Matching it by its bare name made `__cons-fmap`'s
 * callback call an edge to a program's own pure `(define f ...)`, which left a
 * function that calls an unknown procedure uncolored -- an effectful callback
 * then had "no lowering here", and the r7rs library unit's text (so its cached
 * object) depended on whether the program named something `f`. */
static const char *cps_call_c_symbol(const Binding *b) {
    if (!b) return NULL;
    if (b->c_export_name) return b->c_export_name;
    if (!b->is_global) return NULL;
    return b->name ? b->name->name : NULL;
}

/* Find the top-level node whose function binding == b, or -1.
 *
 * The pointer compare is the fast, exact case.  The C-symbol fallback resolves
 * the alias above: without it a named let's self-call read as an UNRESOLVED
 * call, which set `has_indirect` and conservatively colored the loop -- so a
 * plain `(let go [i 0 acc 0] ... (go ...))` with no control operator anywhere
 * was CPS-emitted, and its self-call then re-entered the function's own DK
 * entry wrapper (a dk_prompt malloc + setjmp) once per iteration, overflowing
 * the stack on every engine.  Two bindings that carry the SAME C symbol are the
 * same C function by construction (the emitter emits one definition), so the
 * fallback resolves a real edge rather than widening anything: the callee's own
 * seed still colors it if it uses control, and the fixpoint still propagates
 * that to callers.  See
 * docs/archive/history/cps-colored-noncapture-named-let-recurses-through-entry.md. */
/* r7rs-conformance-program-emits-megabytes-of-c: cps_color_program asks this
 * once per call edge, and each answer scanned every node (twice on a miss) --
 * quadratic in the number of functions.  While cps_color_program runs, its node
 * array is indexed by binding and by C symbol, each slot holding the FIRST node
 * the scans below would have found. */
static struct {
    const CpsNode  *nodes;
    uint32_t        n, cap;
    const Binding **bkey;  int *bval;     /* binding -> first node */
    const char    **skey;  int *sval;     /* C symbol -> first node */
} g_cps_nidx;

static uint32_t cps_nidx_ptr_hash(const void *p) {
    uint64_t x = (uint64_t)(uintptr_t)p;
    x ^= x >> 33; x *= 0xff51afd7ed558ccdULL; x ^= x >> 33;
    return (uint32_t)x;
}
static uint32_t cps_nidx_str_hash(const char *s) {
    uint32_t h = 2166136261u;
    for (; *s; s++) { h ^= (uint8_t)*s; h *= 16777619u; }
    return h;
}

static void cps_nidx_free(void) {
    free((void *)g_cps_nidx.bkey); free(g_cps_nidx.bval);
    free((void *)g_cps_nidx.skey); free(g_cps_nidx.sval);
    memset(&g_cps_nidx, 0, sizeof g_cps_nidx);
}

static const char *cps_binding_c_symbol(const Binding *b);

static void cps_nidx_build(const CpsNode *nodes, uint32_t n) {
    cps_nidx_free();
    uint32_t cap = 16;
    while (cap < 2 * n + 2) cap <<= 1;
    g_cps_nidx.bkey = (const Binding **)calloc(cap, sizeof *g_cps_nidx.bkey);
    g_cps_nidx.bval = (int *)calloc(cap, sizeof *g_cps_nidx.bval);
    g_cps_nidx.skey = (const char **)calloc(cap, sizeof *g_cps_nidx.skey);
    g_cps_nidx.sval = (int *)calloc(cap, sizeof *g_cps_nidx.sval);
    if (!g_cps_nidx.bkey || !g_cps_nidx.bval || !g_cps_nidx.skey || !g_cps_nidx.sval) {
        cps_nidx_free();   /* OOM: cps_find_node scans */
        return;
    }
    for (uint32_t i = 0; i < n; i++) {
        const Binding *b = nodes[i].fd->binding;
        if (!b) continue;
        uint32_t h = cps_nidx_ptr_hash(b) & (cap - 1);
        while (g_cps_nidx.bkey[h] && g_cps_nidx.bkey[h] != b) h = (h + 1) & (cap - 1);
        if (!g_cps_nidx.bkey[h]) { g_cps_nidx.bkey[h] = b; g_cps_nidx.bval[h] = (int)i; }
        const char *sym = cps_binding_c_symbol(b);
        if (!sym) continue;
        h = cps_nidx_str_hash(sym) & (cap - 1);
        while (g_cps_nidx.skey[h] && strcmp(g_cps_nidx.skey[h], sym) != 0) h = (h + 1) & (cap - 1);
        if (!g_cps_nidx.skey[h]) { g_cps_nidx.skey[h] = sym; g_cps_nidx.sval[h] = (int)i; }
    }
    g_cps_nidx.nodes = nodes;
    g_cps_nidx.n = n;
    g_cps_nidx.cap = cap;
}

static int cps_find_node(CpsNode *nodes, uint32_t n, const Binding *b) {
    if (!b) return -1;
    if (g_cps_nidx.cap && g_cps_nidx.nodes == nodes && g_cps_nidx.n == n) {
        uint32_t m = g_cps_nidx.cap - 1, h = cps_nidx_ptr_hash(b) & m;
        while (g_cps_nidx.bkey[h]) {
            if (g_cps_nidx.bkey[h] == b) return g_cps_nidx.bval[h];
            h = (h + 1) & m;
        }
        const char *bsym = cps_call_c_symbol(b);
        if (!bsym || !*bsym) return -1;
        h = cps_nidx_str_hash(bsym) & m;
        while (g_cps_nidx.skey[h]) {
            if (strcmp(g_cps_nidx.skey[h], bsym) == 0) return g_cps_nidx.sval[h];
            h = (h + 1) & m;
        }
        return -1;
    }
    for (uint32_t i = 0; i < n; i++)
        if (nodes[i].fd->binding == b) return (int)i;
    const char *bsym = cps_call_c_symbol(b);
    if (!bsym || !*bsym) return -1;
    for (uint32_t i = 0; i < n; i++) {
        const char *nsym = cps_binding_c_symbol(nodes[i].fd->binding);
        if (nsym && strcmp(nsym, bsym) == 0) return (int)i;
    }
    return -1;
}

/* Walk a function body collecting call edges and the indirect flag, WITHOUT
 * descending into nested function bodies (separate nodes). */
static void cps_collect_calls(const Expr *e, CpsNode *nodes, uint32_t n_nodes,
                              CpsNode *self);

typedef struct { CpsNode *nodes; uint32_t n_nodes; CpsNode *self; } CpsCollectUd;
static bool cps_collect_visit(const Expr *c, void *ud) {
    CpsCollectUd *u = (CpsCollectUd *)ud;
    cps_collect_calls(c, u->nodes, u->n_nodes, u->self);
    return false;
}

static void cps_collect_calls(const Expr *e, CpsNode *nodes, uint32_t n_nodes,
                              CpsNode *self) {
    if (!e) return;
    switch (e->kind) {
        case EX_CALL: {
            int idx = cps_find_node(nodes, n_nodes, e->as.call_.fn_binding);
            /* cps-evicts-handle-in-operand-positions (fuzz seed 3333 case 121):
             * a lambda LITERAL applied on the spot -- `((fn [] b))` -- has
             * exactly one callee, its own lifted FnDef, which is a node like
             * any top-level fn.  Counting it as an unresolved call colored
             * every pure function that did this (a generic whose body was
             * `(let [b x] ((fn [] b)))`), and a colored callee that then
             * SIG-REJECTs still reads as cps->cps to its callers: their join
             * over its fat-closure result is not slot-representable, so a
             * `main` that also held a `handle` was evicted whole ("no lowering
             * here").  An edge to the lambda's node is exact: a lambda that
             * does use control still colors its applier through it. */
            /* The elaborator hoists the head into a compiler temp --
             * `(let [__call_head_N (fn [] b)] (__call_head_N))` -- which
             * records its init in closure_head_init and is never assigned;
             * see through it too.  (A user `let` stores only a CALL there,
             * which the literal test below never matches.) */
            const Expr *h = NULL;
            if (idx < 0 && !e->as.call_.fn_binding)
                h = e->as.call_.fn_expr;
            else if (idx < 0)
                h = e->as.call_.fn_binding->closure_head_init;
            if (h) {
                for (;;) {
                    if (h->kind == EX_ASCRIBE) h = h->as.ascribe_.inner;
                    else if (h->kind == EX_FN_TO_FAT) h = h->as.fn_to_fat_.inner;
                    else if (h->kind == EX_POLY_TO_FAT) h = h->as.poly_to_fat_.inner;
                    else break;
                    if (!h) break;
                }
                const FnDef *lf = NULL;
                if (h && h->kind == EX_CLOSURE && h->as.closure_.closure)
                    lf = h->as.closure_.closure->fn;
                else if (h && h->kind == EX_FN)
                    lf = h->as.fn_.fn;
                if (lf && lf->binding)
                    idx = cps_find_node(nodes, n_nodes, lf->binding);
            }
            if (idx >= 0) {
                cps_node_add_edge(self, (uint32_t)idx);
            } else {
                /* Unresolved: indirect call, call through a local value, or an
                 * extern/builtin not in the top-level set. Conservatively treat
                 * as possibly reaching a control op (CPS0.1 rule 3).
                 *
                 * Exception: a CONSTRUCTOR call cannot reach a control op -- it
                 * stores its (independently-checked) argument values into a fresh
                 * aggregate and invokes nothing.  Coloring on it needlessly
                 * colors every pure struct/ADT builder (Option/Result/Pair/map
                 * constructors and user `make-struct` helpers), which then cannot
                 * be delegated as a cps->direct call.  Any control op in an
                 * argument is still caught by the seed scan (cps_directly_uses_
                 * control) and the arg recursion below, so skipping the call
                 * itself is sound.  See
                 * docs/archive/history/cps-coloring-overcolors-nonnode-calls.md.
                 *
                 * Same reasoning for a RESOLVED call whose callee's body is
                 * inline-C (`body_is_inline_c`): an inline-C body is opaque C with
                 * NO Turmeric control op, so it can never reach a perform/handle/
                 * shift -- it is a leaf exactly like a constructor.  This stops a
                 * contract macro (`require-msg!` -> `tur-contract-check`, an
                 * embedded inline-C `defn` absent from the node set) from
                 * spuriously coloring an otherwise-pure function, which then
                 * SIG-REJECTs and cascades its callers onto the fiber path (the
                 * sized-bitvec/matrix `main`s tail-call such a contract helper). */
                /* perform-inside-loop-has-no-lowering (examples/snake): an
                 * `extern-c` callee is the same leaf -- a foreign C function
                 * with no Turmeric control op in it.  Treating it as an
                 * indirect call colored every helper that touched the FFI
                 * (`render-frame`, `show-game-over`), and a colored helper
                 * called from a handler clause is a heap join the clause
                 * grammar does not admit, so the whole game loop evicted. */
                bool callee_inline_c = e->as.call_.fn_binding
                    && (e->as.call_.fn_binding->body_is_inline_c
                        || e->as.call_.fn_binding->is_extern_c);
                if (e->as.call_.ctor == NULL && !callee_inline_c)
                    self->has_indirect = true;
            }
            cps_collect_calls(e->as.call_.fn_expr, nodes, n_nodes, self);
            for (uint32_t i = 0; i < e->as.call_.n_args; i++)
                cps_collect_calls(e->as.call_.args[i], nodes, n_nodes, self);
            return;
        }
        case EX_LET:
            for (uint32_t i = 0; i < e->as.let_.n; i++)
                cps_collect_calls(e->as.let_.bindings[i].init, nodes, n_nodes, self);
            cps_collect_calls(e->as.let_.body, nodes, n_nodes, self);
            return;
        case EX_IF:
            cps_collect_calls(e->as.if_.cond, nodes, n_nodes, self);
            cps_collect_calls(e->as.if_.then_, nodes, n_nodes, self);
            cps_collect_calls(e->as.if_.else_or_null, nodes, n_nodes, self);
            return;
        case EX_DO:
            for (uint32_t i = 0; i < e->as.do_.n; i++)
                cps_collect_calls(e->as.do_.items[i], nodes, n_nodes, self);
            return;
        case EX_WHILE:
            cps_collect_calls(e->as.while_.cond, nodes, n_nodes, self);
            cps_collect_calls(e->as.while_.body, nodes, n_nodes, self);
            return;
        case EX_SET:
            cps_collect_calls(e->as.set_.value, nodes, n_nodes, self);
            return;
        case EX_DEF:
            cps_collect_calls(e->as.def_.init, nodes, n_nodes, self);
            return;
        case EX_BUILTIN:
            for (uint32_t i = 0; i < e->as.builtin.n; i++)
                cps_collect_calls(e->as.builtin.args[i], nodes, n_nodes, self);
            return;
        case EX_DEFER:
            cps_collect_calls(e->as.defer_.body, nodes, n_nodes, self);
            return;
        case EX_RETURN:
            cps_collect_calls(e->as.return_.value, nodes, n_nodes, self);
            return;
        case EX_RESET:
            cps_collect_calls(e->as.reset_.body, nodes, n_nodes, self);
            return;
        case EX_SHIFT:
            cps_collect_calls(e->as.shift_.k_fn, nodes, n_nodes, self);
            cps_collect_calls(e->as.shift_.body, nodes, n_nodes, self);
            return;
        case EX_SHIFT0:
            cps_collect_calls(e->as.shift0_.k_fn, nodes, n_nodes, self);
            cps_collect_calls(e->as.shift0_.body, nodes, n_nodes, self);
            return;
        case EX_CLONEABLE_RESET:
            cps_collect_calls(e->as.cloneable_reset_.body, nodes, n_nodes, self);
            return;
        case EX_CLONEABLE_SHIFT:
            cps_collect_calls(e->as.cloneable_shift_.k_fn, nodes, n_nodes, self);
            cps_collect_calls(e->as.cloneable_shift_.body, nodes, n_nodes, self);
            return;
        case EX_DYNVAR_BINDING:
            for (uint32_t i = 0; i < e->as.dynvar_binding_.n_pairs; i++)
                cps_collect_calls(e->as.dynvar_binding_.pairs[i].override_expr,
                                  nodes, n_nodes, self);
            cps_collect_calls(e->as.dynvar_binding_.body, nodes, n_nodes, self);
            return;
        case EX_CATCH_UNWIND:
            cps_collect_calls(e->as.catch_unwind_.thunk, nodes, n_nodes, self);
            return;
        case EX_CATCH_PANIC_OF:
            cps_collect_calls(e->as.catch_panic_of_.thunk, nodes, n_nodes, self);
            return;
        case EX_ASCRIBE:
            /* Ascription is erased at codegen; descend so a call inside
             * (:: <expr> T) still contributes a call-graph edge. */
            cps_collect_calls(e->as.ascribe_.inner, nodes, n_nodes, self);
            return;
        /* saffron-effect-row-lost-through-unannotated-call: the Saffron nodes
         * and the `any` widen / readers, the same set `cps_directly_uses_control`
         * carries -- and missing here for the same reason it was missing there,
         * one step removed.
         *
         * That walk SEEDS a function that itself holds a control op; this one
         * builds the call-graph EDGES the fixpoint then propagates along.  With
         * no arms, an edge whose call sits inside a dynamic operand was never
         * recorded, so a caller of an effectful function went uncolored: the
         * handler's DK was not threaded through it, and the callee's `perform`
         * searched a root with no handler -- `tur: unhandled effect (tag N)` at
         * run time, on a program the interpreter ran correctly.
         *
         * `(defn f [] (+ (g) 1))` is the whole repro, because `+` on an `any`
         * IS a dynamic op -- which is to say, most of the arithmetic in most
         * Saffron programs. */
        case EX_UNION_INJECT:
            cps_collect_calls(e->as.union_inject_.value, nodes, n_nodes, self);
            return;
        case EX_DYN_OP:
            for (uint32_t i = 0; i < e->as.dyn_op_.n_args; i++)
                cps_collect_calls(e->as.dyn_op_.args[i], nodes, n_nodes, self);
            return;
        case EX_DYN_CALL:
            /* The callee is an expression resolved at run time, so no edge can
             * be named: conservatively indirect, exactly as the EX_CALL arm
             * treats an unresolved callee. */
            self->has_indirect = true;
            cps_collect_calls(e->as.dyn_call_.fn, nodes, n_nodes, self);
            for (uint32_t i = 0; i < e->as.dyn_call_.n_args; i++)
                cps_collect_calls(e->as.dyn_call_.args[i], nodes, n_nodes, self);
            return;
        case EX_DYN_FIELD:
            cps_collect_calls(e->as.dyn_field_.obj, nodes, n_nodes, self);
            return;
        case EX_DYN_METHOD:
            cps_collect_calls(e->as.dyn_method_.obj, nodes, n_nodes, self);
            for (uint32_t i = 0; i < e->as.dyn_method_.n_args; i++)
                cps_collect_calls(e->as.dyn_method_.args[i], nodes, n_nodes, self);
            return;
        case EX_ANY_CAST:
            cps_collect_calls(e->as.any_cast_.value, nodes, n_nodes, self);
            return;
        case EX_ANY_IS:
            cps_collect_calls(e->as.any_is_.value, nodes, n_nodes, self);
            return;
        /* Nested function definitions are call-graph boundaries. */
        case EX_FN_DEF:
        case EX_FN:
        case EX_CLOSURE:
            return;
        /* effect-row-lost-through-a-constructor-argument: the aggregate family,
         * which is where this report's defect lived -- `(Box (g))` is an
         * EX_MAKE_STRUCT whose field value is the effectful call, and the `.v`
         * reading it back is an EX_GET_FIELD.  Neither had an arm, so the call
         * edge under them was dropped and the coloring fixpoint never reached
         * the caller.
         *
         * cps-edge-walk-misses-nodes-and-colored-frames-leak (2026-09-16): the
         * `default:` below now falls back to the shared `cps_visit_children`
         * enumerator, the same one `cps_directly_uses_control` uses.  It was
         * held back deliberately for a while: this walk sets `has_indirect`
         * for an unresolved callee (CPS0.1 rule 3), so descending into a node
         * it never descended into before colors a function that merely calls
         * a callback there -- on `tests/fixtures/typed/result-basic` the
         * enumerator colored 13 more functions (`result-map`, `option-map`,
         * `option-eq?`, six typeclass instances), because EX_MATCH had no arm
         * and every stdlib HOF calls its callback inside a `match`.  That
         * coloring is right; what made it unshippable was that a newly
         * colored function LEAKED the fresh sum box it handed to a
         * non-retaining callee (the direct emitter's argument-hoist drain has
         * no CPS twin).  The CPS emitter now carries that drain -- see the
         * deferred-drop table in emit_cps_ir.c -- so the widening lands with
         * it, gated by tests/run-leak-check.sh rather than run.sh alone. */
        case EX_MAKE_STRUCT:
            for (uint32_t i = 0; i < e->as.make_struct_.n_fields; i++)
                cps_collect_calls(e->as.make_struct_.field_values[i], nodes, n_nodes, self);
            return;
        case EX_GET_FIELD:
            cps_collect_calls(e->as.get_field_.struct_expr, nodes, n_nodes, self);
            return;
        default: {
            /* cps-edge-walk-misses-nodes-and-colored-frames-leak: every other
             * kind that carries an evaluated operand falls back to the shared
             * enumerator, so a call under a `match` arm (or any later node
             * kind) contributes its edge. */
            CpsCollectUd ud = { nodes, n_nodes, self };
            cps_visit_children(e, cps_collect_visit, &ud);
            return;
        }
    }
}

/* E2 (cps-tramp-resume): peel a fn-value ARG's wrappers to the EX_VAR naming a
 * (lifted-lambda) binding. */
static const Expr *cps_peel_fnvalue_arg(const Expr *e) {
    while (e) {
        switch (e->kind) {
            case EX_ASCRIBE:     e = e->as.ascribe_.inner;     break;
            case EX_FN_TO_FAT:   e = e->as.fn_to_fat_.inner;   break;
            case EX_POLY_TO_FAT: e = e->as.poly_to_fat_.inner; break;
            case EX_POLY_WRAP:   e = e->as.poly_wrap_.inner;   break;
            case EX_CAST:        e = e->as.cast_.expr;         break;
            case EX_REINTERPRET: e = e->as.reinterpret_.expr;  break;
            default:             return e;
        }
    }
    return e;
}

/* E2 (cps-tramp-resume): force-color a lifted lambda passed as an arg to an
 * effectful TY_FN param, so a PURE such lambda still gets a __cps entry the HOF's
 * registry thread needs (effect-subtype cluster).  Best-effort recursion. */
static bool cps_force_color_eff_fnval_args(const Expr *e, CpsNode *nodes, uint32_t n) {
    if (!e) return false;
    bool ch = false;
    switch (e->kind) {
        case EX_CALL: {
            if (e->as.call_.fn_binding) {
                int ci = cps_find_node(nodes, n, e->as.call_.fn_binding);
                if (ci >= 0) {
                    FnDef *cfd = nodes[ci].fd;
                    uint32_t np = cfd->n_params;
                    for (uint32_t p = 0; p < np && p < e->as.call_.n_args; p++) {
                        const Binding *pb = cfd->params ? cfd->params[p] : NULL;
                        if (!pb || pb->type.kind != TY_FN) continue;
                        EffectRow *pr = (EffectRow *)pb->type.as.fn.effect_row;
                        if (effect_row_is_empty(pr)) continue;
                        const Expr *arg = cps_peel_fnvalue_arg(e->as.call_.args[p]);
                        if (arg && arg->kind == EX_VAR && arg->as.var.binding) {
                            int li = cps_find_node(nodes, n, arg->as.var.binding);
                            if (li >= 0 && !nodes[li].colored) { nodes[li].colored = true; ch = true; }
                            /* A let temp of a capturing closure records the
                             * lifted lambda in closure_fn_binding; a `^borrow`
                             * hoist temp in hoist_closure_fn_binding. */
                            const Binding *lam = arg->as.var.binding->closure_fn_binding
                                ? arg->as.var.binding->closure_fn_binding
                                : arg->as.var.binding->hoist_closure_fn_binding;
                            if (lam) {
                                li = cps_find_node(nodes, n, lam);
                                if (li >= 0 && !nodes[li].colored) { nodes[li].colored = true; ch = true; }
                            }
                        }
                        /* fn-value-fat-normalization (effect-row increment): a
                         * CAPTURING closure literal in the same position -- its
                         * lifted FnDef (env as param[0]) needs the same __cps
                         * entry for the registry's slot-0 (env-taking) dispatch. */
                        if (arg && arg->kind == EX_CLOSURE && arg->as.closure_.closure
                            && arg->as.closure_.closure->fn
                            && arg->as.closure_.closure->fn->binding) {
                            int li = cps_find_node(nodes, n,
                                arg->as.closure_.closure->fn->binding);
                            if (li >= 0 && !nodes[li].colored) { nodes[li].colored = true; ch = true; }
                        }
                    }
                }
            }
            /* E2c: a `(make-struct S ...)` lowers to a CONSTRUCTOR call.  A fn-value
             * stored in an EFFECTFUL capability field (`[run : fn #fx{E}]`) is
             * threaded via the registry at each `(.run obj)` call, which needs even
             * a PURE such fn-value force-colored so it gets a __cps entry to
             * register (effect-subtype-capability). */
            if (e->as.call_.ctor) {
                const CtorDef *ctor = e->as.call_.ctor;
                for (uint32_t p = 0; p < e->as.call_.n_args && p < ctor->n_fields; p++) {
                    if (effect_row_is_empty(ctor->fields[p].effect_row)) continue;
                    const Expr *arg = cps_peel_fnvalue_arg(e->as.call_.args[p]);
                    if (arg && arg->kind == EX_VAR && arg->as.var.binding) {
                        int li = cps_find_node(nodes, n, arg->as.var.binding);
                        if (li >= 0 && !nodes[li].colored) { nodes[li].colored = true; ch = true; }
                    }
                    if (arg && arg->kind == EX_CLOSURE && arg->as.closure_.closure
                        && arg->as.closure_.closure->fn
                        && arg->as.closure_.closure->fn->binding) {
                        int li = cps_find_node(nodes, n,
                            arg->as.closure_.closure->fn->binding);
                        if (li >= 0 && !nodes[li].colored) { nodes[li].colored = true; ch = true; }
                    }
                }
            }
            if (cps_force_color_eff_fnval_args(e->as.call_.fn_expr, nodes, n)) ch = true;
            for (uint32_t i = 0; i < e->as.call_.n_args; i++)
                if (cps_force_color_eff_fnval_args(e->as.call_.args[i], nodes, n)) ch = true;
            return ch;
        }
        case EX_LET:
            for (uint32_t i = 0; i < e->as.let_.n; i++)
                if (cps_force_color_eff_fnval_args(e->as.let_.bindings[i].init, nodes, n)) ch = true;
            if (cps_force_color_eff_fnval_args(e->as.let_.body, nodes, n)) ch = true;
            return ch;
        case EX_IF:
            if (cps_force_color_eff_fnval_args(e->as.if_.cond, nodes, n)) ch = true;
            if (cps_force_color_eff_fnval_args(e->as.if_.then_, nodes, n)) ch = true;
            if (cps_force_color_eff_fnval_args(e->as.if_.else_or_null, nodes, n)) ch = true;
            return ch;
        case EX_DO:
            for (uint32_t i = 0; i < e->as.do_.n; i++)
                if (cps_force_color_eff_fnval_args(e->as.do_.items[i], nodes, n)) ch = true;
            return ch;
        case EX_BUILTIN:
            for (uint32_t i = 0; i < e->as.builtin.n; i++)
                if (cps_force_color_eff_fnval_args(e->as.builtin.args[i], nodes, n)) ch = true;
            return ch;
        case EX_WHILE:
            if (cps_force_color_eff_fnval_args(e->as.while_.cond, nodes, n)) ch = true;
            if (cps_force_color_eff_fnval_args(e->as.while_.body, nodes, n)) ch = true;
            return ch;
        case EX_SET:    return cps_force_color_eff_fnval_args(e->as.set_.value, nodes, n);
        case EX_RETURN: return cps_force_color_eff_fnval_args(e->as.return_.value, nodes, n);
        case EX_DEFER:  return cps_force_color_eff_fnval_args(e->as.defer_.body, nodes, n);
        case EX_DEF:    return cps_force_color_eff_fnval_args(e->as.def_.init, nodes, n);
        case EX_REINTERPRET:return cps_force_color_eff_fnval_args(e->as.reinterpret_.expr, nodes, n);
        case EX_ASCRIBE:return cps_force_color_eff_fnval_args(e->as.ascribe_.inner, nodes, n);
        case EX_RESET:  return cps_force_color_eff_fnval_args(e->as.reset_.body, nodes, n);
        case EX_HANDLE: {
            HandleExpr *h = e->as.handle_.handle;
            if (!h) return false;
            if (cps_force_color_eff_fnval_args(h->body, nodes, n)) ch = true;
            for (uint8_t i = 0; i < h->n_cases; i++)
                if (cps_force_color_eff_fnval_args(h->cases[i].body, nodes, n)) ch = true;
            return ch;
        }
        default: return false;
    }
}

/* E2 (cps-tramp-resume): does `e` contain a `with-handler` anywhere (structural,
 * no descent into nested fn bodies)?  Used to color a fn that discharges one
 * effect via with-handler but leaves a LEFTOVER (fh-discharge-row's do-work) --
 * gated additionally on the fn having a non-empty effect row, so the top-level
 * with-handler mains that discharge EVERYTHING (empty row) are NOT colored and
 * keep their existing DK-lowering. */
static bool cps_body_has_with_handler(const Expr *e) {
    if (!e) return false;
    switch (e->kind) {
        case EX_WITH_HANDLER: {
            /* Only a LITERAL handler is translatable (build_with_handler); a dynamic
             * handler value (a `handler`-typed param, e.g. run-with's `h`, or a
             * compose-handlers) would evict -- do NOT color those, so they keep
             * their existing lowering. */
            const Expr *hv = e->as.with_handler_.handler;
            while (hv && hv->kind == EX_ASCRIBE) hv = hv->as.ascribe_.inner;
            return hv && hv->kind == EX_HANDLER_LIT;
        }
        case EX_LET:
            for (uint32_t i = 0; i < e->as.let_.n; i++)
                if (cps_body_has_with_handler(e->as.let_.bindings[i].init)) return true;
            return cps_body_has_with_handler(e->as.let_.body);
        case EX_DO:
            for (uint32_t i = 0; i < e->as.do_.n; i++)
                if (cps_body_has_with_handler(e->as.do_.items[i])) return true;
            return false;
        case EX_IF:
            return cps_body_has_with_handler(e->as.if_.cond)
                || cps_body_has_with_handler(e->as.if_.then_)
                || cps_body_has_with_handler(e->as.if_.else_or_null);
        case EX_REINTERPRET: return cps_body_has_with_handler(e->as.reinterpret_.expr);
        case EX_ASCRIBE: return cps_body_has_with_handler(e->as.ascribe_.inner);
        case EX_RETURN:  return cps_body_has_with_handler(e->as.return_.value);
        case EX_HANDLE: {
            HandleExpr *h = e->as.handle_.handle;
            if (!h) return false;
            if (cps_body_has_with_handler(h->body)) return true;
            for (uint8_t i = 0; i < h->n_cases; i++)
                if (cps_body_has_with_handler(h->cases[i].body)) return true;
            return false;
        }
        default: return false;
    }
}

/* The fn's declared/inferred effect row is non-empty -- it has a LEFTOVER effect
 * that escapes (not fully discharged by an internal with-handler). */
static bool cps_fn_has_leftover_effect(const FnDef *fd) {
    if (!fd) return false;
    if (fd->inferred_effect_row && !effect_row_is_empty(fd->inferred_effect_row))
        return true;
    if (fd->binding && fd->binding->type.kind == TY_FN
        && !effect_row_is_empty(fd->binding->type.as.fn.effect_row))
        return true;
    return false;
}

/* await-through-fn-value-parks-only-the-callee: does `e` suspend on an
 * `await` -- one of its own, or a named callee marked `may_await`?  Nested
 * function definitions are boundaries (cps_visit_children does not enter
 * them), so an await inside a lambda or an `(async (fn [] ...))` body is that
 * lambda's, not the enclosing function's -- but a CALL that passes such a
 * function as an argument may await, since its callee may call it
 * (await-parks-only-to-the-nearest-c-frame: `(apply3 level3 x)` with an
 * awaiting `level3`). */
static bool cps_awaits_visit(const Expr *c, void *ud);
static bool cps_expr_awaits(const Expr *e);
static bool cps_binding_may_await(const Binding *b) {
    if (!b) return false;
    if (b->source_binding) b = b->source_binding;
    return b->source_fn_def && b->source_fn_def->may_await;
}
static bool cps_fnvalue_arg_awaits(const Expr *arg) {
    arg = cps_peel_fnvalue_arg(arg);
    if (!arg) return false;
    if (arg->kind == EX_VAR && arg->as.var.binding) {
        const Binding *b = arg->as.var.binding;
        return cps_binding_may_await(b)
            || cps_binding_may_await(b->closure_fn_binding)
            || cps_binding_may_await(b->hoist_closure_fn_binding);
    }
    if (arg->kind == EX_CLOSURE && arg->as.closure_.closure && arg->as.closure_.closure->fn) {
        const FnDef *lfd = arg->as.closure_.closure->fn;
        return lfd->may_await || cps_expr_awaits(lfd->body);
    }
    return false;
}
static bool cps_expr_awaits(const Expr *e) {
    if (!e) return false;
    if (e->kind == EX_AWAIT) return true;
    if (e->kind == EX_CALL) {
        if (cps_binding_may_await(e->as.call_.fn_binding)) return true;
        for (uint32_t i = 0; i < e->as.call_.n_args; i++)
            if (cps_fnvalue_arg_awaits(e->as.call_.args[i])) return true;
    }
    return cps_visit_children(e, cps_awaits_visit, NULL);
}
static bool cps_awaits_visit(const Expr *c, void *ud) {
    (void)ud;
    return cps_expr_awaits(c);
}

bool cps_fn_may_await(const FnDef *fd) {
    if (!fd) return false;
    return fd->may_await || cps_expr_awaits(fd->body);
}

/* Does this function's own body install a `handle` (not an `(unsafe ...)`
 * marker, and not inside a nested function definition)?  Evicted from the CPS
 * backend, such a function takes every effect it handles off the backend with
 * it (a non-candidate's effect set counts what it handles), and a perform of
 * one of those effects anywhere then has no lowering
 * (cps-capturing-closure-with-handle-returned-refused). */
static bool cps_installs_handle_visit(const Expr *c, void *ud);
static bool cps_expr_installs_handle(const Expr *e) {
    if (!e) return false;
    if (e->kind == EX_HANDLE && e->as.handle_.handle
        && !e->as.handle_.handle->is_unsafe_marker) return true;
    return cps_visit_children(e, cps_installs_handle_visit, NULL);
}
static bool cps_installs_handle_visit(const Expr *c, void *ud) {
    (void)ud;
    return cps_expr_installs_handle(c);
}
bool cps_fn_installs_handle(const FnDef *fd) {
    return fd && cps_expr_installs_handle(fd->body);
}

void cps_color_program(Arena *a, Expr *program) {
    (void)a;
    if (!program || program->kind != EX_PROGRAM) return;

    /* Collect top-level function nodes -- AND the function members nested inside
     * `(defmodule ...)` bodies.  A module member never appears as a top-level
     * EX_FN_DEF (it lives in EX_DEFMODULE's `mod->body`), so without descending
     * here an effectful module `defn` (e.g. a `run` that installs a `handle`) is
     * never colored and silently falls to the fiber effect runtime -- the same
     * self-contained handle DK-lowers fine as a bare top-level defn.  Only
     * control-using members become colored (cps_directly_uses_control); pure
     * module members stay uncolored, so the blast radius is effectful code. */
    uint32_t cap = 16, n = 0;
    CpsNode *nodes = calloc(cap, sizeof(CpsNode));
    #define CPS_ADD_FN_NODE(FDEXPR) do {                                        \
        FnDef *_fd = (FDEXPR)->as.fn_def_.fn;                                   \
        if (_fd) {                                                              \
            _fd->cps_colored = false; /* reset (idempotent) */                 \
            if (n == cap) { cap *= 2; nodes = realloc(nodes, cap * sizeof(CpsNode)); } \
            memset(&nodes[n], 0, sizeof(CpsNode));                              \
            nodes[n].fd = _fd;                                                  \
            n++;                                                               \
        }                                                                      \
    } while (0)
    for (uint32_t i = 0; i < program->as.program.n; i++) {
        Expr *item = program->as.program.items[i];
        if (!item) continue;
        if (item->kind == EX_FN_DEF && item->as.fn_def_.fn) {
            CPS_ADD_FN_NODE(item);
        } else if (item->kind == EX_DEFMODULE && item->as.defmodule_.mod) {
            /* Descending into module members colors fns a top-level-only walk
             * would miss.  Unconditional since cps-tramp-resume graduated. */
            DefModule *m = item->as.defmodule_.mod;
            for (uint32_t j = 0; j < m->n_body; j++) {
                Expr *mb = m->body[j];
                if (mb && mb->kind == EX_FN_DEF && mb->as.fn_def_.fn)
                    CPS_ADD_FN_NODE(mb);
            }
        }
    }
    #undef CPS_ADD_FN_NODE
    cps_nidx_build(nodes, n);

    /* Seed + build edges. */
    for (uint32_t i = 0; i < n; i++) {
        nodes[i].colored = cps_directly_uses_control(nodes[i].fd->body);
        /* E2 (cps-tramp-resume): a fn that discharges an effect via `with-handler`
         * but leaves a LEFTOVER (non-empty effect row) must be colored so its
         * leftover threads to the enclosing DK handler instead of fiber-emitting
         * (fh-discharge-row's do-work).  Narrow: only fires for a genuine-leftover
         * fn, so a with-handler main / helper that discharges EVERYTHING (empty
         * row) is untouched and keeps its existing lowering. */
        if (!nodes[i].colored
            && cps_fn_has_leftover_effect(nodes[i].fd)
            && cps_body_has_with_handler(nodes[i].fd->body))
            nodes[i].colored = true;
        cps_collect_calls(nodes[i].fd->body, nodes, n, &nodes[i]);
        if (nodes[i].has_indirect) nodes[i].colored = true;
    }

    /* Backward propagation to a least fixed point: a function is colored if any
     * resolved callee is colored. */
    bool changed = true;
    while (changed) {
        changed = false;
        for (uint32_t i = 0; i < n; i++) {
            if (nodes[i].colored) continue;
            for (uint32_t e = 0; e < nodes[i].n_edges; e++) {
                if (nodes[nodes[i].edges[e]].colored) {
                    nodes[i].colored = true;
                    changed = true;
                    break;
                }
            }
        }
    }

    /* Force-color effectful fn-value arguments, then re-propagate to a fixed
     * point.  Unconditional since cps-tramp-resume graduated (2026-07-19). */
    bool fc = true;
    while (fc) {
        fc = false;
        for (uint32_t i = 0; i < n; i++)
            if (cps_force_color_eff_fnval_args(nodes[i].fd->body, nodes, n)) fc = true;
    }
    changed = true;
    while (changed) {
        changed = false;
        for (uint32_t i = 0; i < n; i++) {
            if (nodes[i].colored) continue;
            for (uint32_t e = 0; e < nodes[i].n_edges; e++)
                if (nodes[nodes[i].edges[e]].colored) { nodes[i].colored = true; changed = true; break; }
        }
    }

    /* await-through-fn-value-parks-only-the-callee: `may_await` to a fixed
     * point -- a function whose body awaits, or calls a function that may.
     * Reset first, like cps_colored, so a re-run starts clean. */
    for (uint32_t i = 0; i < n; i++) nodes[i].fd->may_await = false;
    changed = true;
    while (changed) {
        changed = false;
        for (uint32_t i = 0; i < n; i++) {
            if (nodes[i].fd->may_await) continue;
            if (cps_expr_awaits(nodes[i].fd->body)) {
                nodes[i].fd->may_await = true;
                changed = true;
            }
        }
    }

    /* Write results back as additive IR metadata, then free scratch. */
    for (uint32_t i = 0; i < n; i++) {
        nodes[i].fd->cps_colored = nodes[i].colored;
        free(nodes[i].edges);
    }
    cps_nidx_free();
    free(nodes);
}

void cps_dump_coloring(Arena *a, Expr *program, FILE *out) {
    if (!program || program->kind != EX_PROGRAM || !out) return;
    cps_color_program(a, program);
    for (uint32_t i = 0; i < program->as.program.n; i++) {
        Expr *item = program->as.program.items[i];
        if (!item || item->kind != EX_FN_DEF || !item->as.fn_def_.fn) continue;
        FnDef *fd = item->as.fn_def_.fn;
        if (!fd->binding || !fd->binding->name) continue;
        /* Only report user-level top-level defns (not stdlib module functions),
         * so the partition is legible. */
        if (fd->binding->defining_module_name) continue;
        fprintf(out, "cps-coloring: %s %s\n",
                fd->binding->name->name,
                fd->cps_colored ? "COLORED" : "uncolored");
    }
}

/* -------------------------------------------------------------------------
 * CPS-CL1: Liveness analysis for cloneable-shift sites
 * -------------------------------------------------------------------------
 * Conservative approach: collect every local (non-global) Binding referenced
 * via EX_VAR anywhere in the enclosing function body.  This over-captures but
 * is always correct; the env struct may be slightly larger than necessary.
 * -------------------------------------------------------------------------*/

/* Add b to *out (malloc'd, grown as needed) if it is local and not yet present. */
static void add_unique_local(Binding *b, Binding ***out, uint32_t *n, uint32_t *cap) {
    if (!b || b->is_global) return;
    for (uint32_t i = 0; i < *n; i++) {
        if ((*out)[i] == b) return; /* already present */
    }
    if (*n == *cap) {
        *cap = *cap ? *cap * 2 : 8;
        *out = realloc(*out, *cap * sizeof(Binding *));
    }
    (*out)[(*n)++] = b;
}

/* Recursively walk e, collecting unique local bindings into *out. */
static void collect_local_vars_expr(const Expr *e,
                                    Binding ***out, uint32_t *n, uint32_t *cap) {
    if (!e) return;
    switch (e->kind) {
        case EX_VAR:
            add_unique_local(e->as.var.binding, out, n, cap);
            return;
        case EX_LET:
            for (uint32_t i = 0; i < e->as.let_.n; i++)
                collect_local_vars_expr(e->as.let_.bindings[i].init, out, n, cap);
            collect_local_vars_expr(e->as.let_.body, out, n, cap);
            return;
        case EX_IF:
            collect_local_vars_expr(e->as.if_.cond, out, n, cap);
            collect_local_vars_expr(e->as.if_.then_, out, n, cap);
            collect_local_vars_expr(e->as.if_.else_or_null, out, n, cap);
            return;
        case EX_DO:
            for (uint32_t i = 0; i < e->as.do_.n; i++)
                collect_local_vars_expr(e->as.do_.items[i], out, n, cap);
            return;
        case EX_WHILE:
            collect_local_vars_expr(e->as.while_.cond, out, n, cap);
            collect_local_vars_expr(e->as.while_.body, out, n, cap);
            return;
        case EX_SET:
            collect_local_vars_expr(e->as.set_.value, out, n, cap);
            return;
        case EX_DEF:
            collect_local_vars_expr(e->as.def_.init, out, n, cap);
            return;
        case EX_BUILTIN:
            for (uint32_t i = 0; i < e->as.builtin.n; i++)
                collect_local_vars_expr(e->as.builtin.args[i], out, n, cap);
            return;
        case EX_CALL:
            collect_local_vars_expr(e->as.call_.fn_expr, out, n, cap);
            for (uint32_t i = 0; i < e->as.call_.n_args; i++)
                collect_local_vars_expr(e->as.call_.args[i], out, n, cap);
            return;
        case EX_RETURN:
            collect_local_vars_expr(e->as.return_.value, out, n, cap);
            return;
        case EX_PANIC:
            collect_local_vars_expr(e->as.panic_.payload, out, n, cap);
            return;
        case EX_DEFER:
            collect_local_vars_expr(e->as.defer_.body, out, n, cap);
            return;
        case EX_RESET:
            collect_local_vars_expr(e->as.reset_.body, out, n, cap);
            return;
        case EX_SHIFT:
            collect_local_vars_expr(e->as.shift_.k_fn, out, n, cap);
            collect_local_vars_expr(e->as.shift_.body, out, n, cap);
            return;
        case EX_SHIFT0:
            collect_local_vars_expr(e->as.shift0_.k_fn, out, n, cap);
            collect_local_vars_expr(e->as.shift0_.body, out, n, cap);
            return;
        case EX_CLONEABLE_RESET:
            collect_local_vars_expr(e->as.cloneable_reset_.body, out, n, cap);
            return;
        case EX_CLONEABLE_SHIFT:
            collect_local_vars_expr(e->as.cloneable_shift_.k_fn, out, n, cap);
            collect_local_vars_expr(e->as.cloneable_shift_.body, out, n, cap);
            return;
        /* Closures/fns: do NOT recurse into nested fn bodies — they have their
         * own capture analysis.  The closure itself may reference locals via
         * EX_VAR in the capture list, but EX_CLOSURE doesn't expose that here.
         * Fine for the conservative over-approximation. */
        default:
            return;
    }
}

/* Recursively patch all EX_CLONEABLE_SHIFT nodes in e with the given locals
 * (forward-declared so cps_compute_live_at_shift can call it). */
static void patch_cloneable_shifts(Arena *a, Expr *e,
                                   Binding **locals, uint32_t n_locals);

/* Populate live_captures on every EX_CLONEABLE_SHIFT inside fn_body.
 * Conservative: all local vars in the entire fn body become live_captures. */
static void cps_compute_live_at_shift(Arena *a, Expr *fn_body) {
    if (!fn_body) return;
    if (!cps_expr_contains_cloneable_shift(fn_body)) return;

    Binding **locals = NULL;
    uint32_t n_locals = 0, cap_locals = 0;
    collect_local_vars_expr(fn_body, &locals, &n_locals, &cap_locals);

    if (n_locals == 0) {
        free(locals);
        return;
    }

    /* Copy into arena (shared across all shift sites in this fn). */
    Binding **arena_locals = arena_alloc(a, n_locals * sizeof(Binding *));
    memcpy(arena_locals, locals, n_locals * sizeof(Binding *));
    free(locals);

    patch_cloneable_shifts(a, fn_body, arena_locals, n_locals);
}

/* Recursively patch all EX_CLONEABLE_SHIFT nodes in e with the given locals. */
static void patch_cloneable_shifts(Arena *a, Expr *e,
                                   Binding **locals, uint32_t n_locals) {
    if (!e) return;
    (void)a; /* arena already used to allocate locals */
    switch (e->kind) {
        case EX_CLONEABLE_SHIFT:
            e->as.cloneable_shift_.live_captures  = locals;
            e->as.cloneable_shift_.n_live_captures = n_locals;
            patch_cloneable_shifts(a, e->as.cloneable_shift_.k_fn,  locals, n_locals);
            patch_cloneable_shifts(a, e->as.cloneable_shift_.body,  locals, n_locals);
            return;
        case EX_CLONEABLE_RESET:
            patch_cloneable_shifts(a, e->as.cloneable_reset_.body, locals, n_locals);
            return;
        case EX_LET:
            for (uint32_t i = 0; i < e->as.let_.n; i++)
                patch_cloneable_shifts(a, e->as.let_.bindings[i].init, locals, n_locals);
            patch_cloneable_shifts(a, e->as.let_.body, locals, n_locals);
            return;
        case EX_IF:
            patch_cloneable_shifts(a, e->as.if_.cond, locals, n_locals);
            patch_cloneable_shifts(a, e->as.if_.then_, locals, n_locals);
            patch_cloneable_shifts(a, e->as.if_.else_or_null, locals, n_locals);
            return;
        case EX_DO:
            for (uint32_t i = 0; i < e->as.do_.n; i++)
                patch_cloneable_shifts(a, e->as.do_.items[i], locals, n_locals);
            return;
        case EX_WHILE:
            patch_cloneable_shifts(a, e->as.while_.cond, locals, n_locals);
            patch_cloneable_shifts(a, e->as.while_.body, locals, n_locals);
            return;
        case EX_RETURN:
            patch_cloneable_shifts(a, e->as.return_.value, locals, n_locals);
            return;
        case EX_DEFER:
            patch_cloneable_shifts(a, e->as.defer_.body, locals, n_locals);
            return;
        case EX_CALL:
            patch_cloneable_shifts(a, e->as.call_.fn_expr, locals, n_locals);
            for (uint32_t i = 0; i < e->as.call_.n_args; i++)
                patch_cloneable_shifts(a, e->as.call_.args[i], locals, n_locals);
            return;
        default:
            return;
    }
}

/* Helper: mark a function as may_capture (for future one-shot CPS) */
static void mark_fn_may_capture(FnDef *fd) {
    if (fd) {
        fd->may_capture = true;
    }
}

/* Phase B2: mark a function as needing cloneable CPS.
 * Sets may_capture so the emitter knows the function interacts with
 * continuations, even before the full cloneable CPS pass is implemented. */
static void mark_fn_needs_cloneable_cps(FnDef *fd) {
    if (fd) {
        fd->may_capture = true;  /* subsumes may_capture for now */
    }
}

/* Helper: transform an expression, marking functions that contain shift */
static Expr *cps_mark_expr(Arena *a, Expr *e) {
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
                new_bindings[i].init = cps_mark_expr(a, e->as.let_.bindings[i].init);
            }
            Expr *new_body = cps_mark_expr(a, e->as.let_.body);
            Expr *out = expr_new(a, EX_LET, e->type, e->span);
            out->as.let_.bindings = new_bindings;
            out->as.let_.n = e->as.let_.n;
            out->as.let_.body = new_body;
            return out;
        }
        
        case EX_IF: {
            Expr *new_cond = cps_mark_expr(a, e->as.if_.cond);
            Expr *new_then = cps_mark_expr(a, e->as.if_.then_);
            Expr *new_else = e->as.if_.else_or_null ? 
                cps_mark_expr(a, e->as.if_.else_or_null) : NULL;
            Expr *out = expr_new(a, EX_IF, e->type, e->span);
            out->as.if_.cond = new_cond;
            out->as.if_.then_ = new_then;
            out->as.if_.else_or_null = new_else;
            return out;
        }
        
        case EX_DO: {
            Expr **new_items = arena_alloc(a, e->as.do_.n * sizeof(Expr *));
            for (uint32_t i = 0; i < e->as.do_.n; i++) {
                new_items[i] = cps_mark_expr(a, e->as.do_.items[i]);
            }
            Expr *out = expr_new(a, EX_DO, e->type, e->span);
            out->as.do_.items = new_items;
            out->as.do_.n = e->as.do_.n;
            return out;
        }
        
        case EX_WHILE: {
            Expr *new_cond = cps_mark_expr(a, e->as.while_.cond);
            Expr *new_body = cps_mark_expr(a, e->as.while_.body);
            Expr *out = expr_new(a, EX_WHILE, e->type, e->span);
            out->as.while_.cond = new_cond;
            out->as.while_.body = new_body;
            return out;
        }
        
        case EX_SET: {
            Expr *new_value = cps_mark_expr(a, e->as.set_.value);
            Expr *out = expr_new(a, EX_SET, e->type, e->span);
            out->as.set_.target = e->as.set_.target;
            out->as.set_.value = new_value;
            return out;
        }
        
        case EX_DEF:
            if (e->as.def_.init) {
                Expr *new_init = cps_mark_expr(a, e->as.def_.init);
                Expr *out = expr_new(a, EX_DEF, e->type, e->span);
                out->as.def_.binding = e->as.def_.binding;
                out->as.def_.init = new_init;
                return out;
            }
            return e;
        
        case EX_BUILTIN: {
            Expr **new_args = arena_alloc(a, e->as.builtin.n * sizeof(Expr *));
            for (uint32_t i = 0; i < e->as.builtin.n; i++) {
                new_args[i] = cps_mark_expr(a, e->as.builtin.args[i]);
            }
            Expr *out = expr_new(a, EX_BUILTIN, e->type, e->span);
            out->as.builtin.spec = e->as.builtin.spec;
            out->as.builtin.args = new_args;
            out->as.builtin.n = e->as.builtin.n;
            return out;
        }
        
        case EX_FN_DEF: {
            FnDef *fd = e->as.fn_def_.fn;
            if (cps_fn_needs_transform(fd)) {
                mark_fn_may_capture(fd);
            }
            /* Phase B2: also mark functions needing cloneable CPS */
            if (cps_fn_needs_cloneable_transform(fd)) {
                mark_fn_needs_cloneable_cps(fd);
            }
            FnDef *new_fd = arena_alloc(a, sizeof(FnDef));
            *new_fd = *fd;
            new_fd->body = cps_mark_expr(a, fd->body);
            /* This clone replaces `fd` in the program tree the backend emits, so
             * keep the binding's canonical-defn link pointing at it.  Downstream
             * passes gate on `binding->source_fn_def == fd` to recognize the
             * canonical top-level defn -- notably the stackless catch-unwind
             * eligibility in emit_fns.c (gs_basic_ok).  Left stale, the clone
             * fails that check, so every catch-unwind function in an async
             * program silently loses its flat-stack trampoline and recurses
             * natively -> fiber stack overflow (fiber-rec).  See
             * docs/archive/fiber-rec-async-fiber-segfault.md. */
            if (new_fd->binding && new_fd->binding->source_fn_def == fd) {
                new_fd->binding->source_fn_def = new_fd;
            }
            /* CPS-CL1: populate live_captures at each cloneable-shift site */
            if (cps_fn_needs_cloneable_transform(new_fd)) {
                cps_compute_live_at_shift(a, new_fd->body);
            }
            Expr *out = expr_new(a, EX_FN_DEF, e->type, e->span);
            out->as.fn_def_.fn = new_fd;
            return out;
        }

        case EX_FN: {
            FnDef *fn = e->as.fn_.fn;
            if (cps_fn_needs_transform(fn)) {
                mark_fn_may_capture(fn);
            }
            /* Phase B2: also mark functions needing cloneable CPS */
            if (cps_fn_needs_cloneable_transform(fn)) {
                mark_fn_needs_cloneable_cps(fn);
            }
            FnDef *new_fn = arena_alloc(a, sizeof(FnDef));
            *new_fn = *fn;
            new_fn->body = cps_mark_expr(a, fn->body);
            /* CPS-CL1: populate live_captures at each cloneable-shift site */
            if (cps_fn_needs_cloneable_transform(new_fn)) {
                cps_compute_live_at_shift(a, new_fn->body);
            }
            Expr *out = expr_new(a, EX_FN, e->type, e->span);
            out->as.fn_.fn = new_fn;
            return out;
        }
        
        case EX_CALL: {
            Expr **new_args = arena_alloc(a, e->as.call_.n_args * sizeof(Expr *));
            for (uint32_t i = 0; i < e->as.call_.n_args; i++) {
                new_args[i] = cps_mark_expr(a, e->as.call_.args[i]);
            }
            Expr *out = expr_new(a, EX_CALL, e->type, e->span);
            /* Struct-assign the whole call_ member first so every field --
             * abi_bindings / n_abi_bindings (the named-tyvar substitution emit
             * uses to mint ABI specializations), dict_arg, is_poly_call,
             * poly_arg_mask -- survives the CPS tree rebuild.  Previously this
             * field-copied only fn_binding/fn_expr/args/n_args, silently
             * dropping abi_bindings: any call inside a CPS-transformed function
             * lost its by-value specialization (carrier base called with a
             * by-value struct -> cc type error).  See
             * docs/archive/history/m5-eq-vec-byval-rewrite-drops-sibling-specs.md. */
            out->as.call_ = e->as.call_;
            out->as.call_.fn_expr = e->as.call_.fn_expr
                ? cps_mark_expr(a, e->as.call_.fn_expr) : NULL;
            out->as.call_.args = new_args;
            out->as.call_.n_args = e->as.call_.n_args;
            return out;
        }
        
        case EX_CLOSURE: {
            struct Closure *closure = e->as.closure_.closure;
            if (cps_fn_needs_transform(closure->fn)) {
                mark_fn_may_capture(closure->fn);
            }
            FnDef *new_fn = arena_alloc(a, sizeof(FnDef));
            *new_fn = *closure->fn;
            new_fn->body = cps_mark_expr(a, closure->fn->body);
            
            struct Closure *new_closure = arena_alloc(a, sizeof(struct Closure));
            *new_closure = *closure;
            new_closure->fn = new_fn;
            
            Expr *out = expr_new(a, EX_CLOSURE, e->type, e->span);
            out->as.closure_.closure = new_closure;
            return out;
        }
        
        case EX_RETURN: {
            Expr *new_value = e->as.return_.value ? 
                cps_mark_expr(a, e->as.return_.value) : NULL;
            Expr *out = expr_new(a, EX_RETURN, e->type, e->span);
            out->as.return_.value = new_value;
            return out;
        }
        
        case EX_PANIC: {
            Expr *new_payload = cps_mark_expr(a, e->as.panic_.payload);
            Expr *out = expr_new(a, EX_PANIC, e->type, e->span);
            out->as.panic_.payload = new_payload;
            return out;
        }
        
        case EX_DEFER: {
            Expr *new_body = cps_mark_expr(a, e->as.defer_.body);
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
        
        /* Orthogonal features - pass through */
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
        
        /* Phase 18: Delimited continuations - pass through but mark */
        case EX_RESET: {
            Expr *new_body = cps_mark_expr(a, e->as.reset_.body);
            Expr *out = expr_new(a, EX_RESET, e->type, e->span);
            out->as.reset_.body = new_body;
            return out;
        }
        
        case EX_SHIFT: {
            Expr *new_k_fn = cps_mark_expr(a, e->as.shift_.k_fn);
            Expr *new_body = cps_mark_expr(a, e->as.shift_.body);
            Expr *out = expr_new(a, EX_SHIFT, e->type, e->span);
            out->as.shift_.k_fn = new_k_fn;
            out->as.shift_.body = new_body;
            return out;
        }
        
        case EX_SHIFT0: {
            Expr *new_k_fn = cps_mark_expr(a, e->as.shift0_.k_fn);
            Expr *new_body = cps_mark_expr(a, e->as.shift0_.body);
            Expr *out = expr_new(a, EX_SHIFT0, e->type, e->span);
            out->as.shift0_.k_fn = new_k_fn;
            out->as.shift0_.body = new_body;
            return out;
        }

        case EX_SERIAL_RESET: {
            Expr *new_body = cps_mark_expr(a, e->as.serial_reset_.body);
            Expr *out = expr_new(a, EX_SERIAL_RESET, e->type, e->span);
            out->as.serial_reset_.body = new_body;
            return out;
        }

        case EX_SERIAL_SHIFT: {
            Expr *new_k_fn = cps_mark_expr(a, e->as.serial_shift_.k_fn);
            Expr *new_body = cps_mark_expr(a, e->as.serial_shift_.body);
            Expr *out = expr_new(a, EX_SERIAL_SHIFT, e->type, e->span);
            out->as.serial_shift_.k_fn = new_k_fn;
            out->as.serial_shift_.body = new_body;
            return out;
        }
        
        /* Phase B2: Cloneable continuations - pass through but mark */
        case EX_CLONEABLE_RESET: {
            Expr *new_body = cps_mark_expr(a, e->as.cloneable_reset_.body);
            Expr *out = expr_new(a, EX_CLONEABLE_RESET, e->type, e->span);
            out->as.cloneable_reset_.body = new_body;
            return out;
        }
        
        case EX_CLONEABLE_SHIFT: {
            Expr *new_k_fn = cps_mark_expr(a, e->as.cloneable_shift_.k_fn);
            Expr *new_body = cps_mark_expr(a, e->as.cloneable_shift_.body);
            Expr *out = expr_new(a, EX_CLONEABLE_SHIFT, e->type, e->span);
            out->as.cloneable_shift_.k_fn = new_k_fn;
            out->as.cloneable_shift_.body = new_body;
            /* live_captures will be filled by cps_compute_live_at_shift */
            out->as.cloneable_shift_.live_captures   = NULL;
            out->as.cloneable_shift_.n_live_captures = 0;
            /* CPS-CL3: cont_body is set later if needed for full continuation splitting */
            out->as.cloneable_shift_.cont_body = NULL;
            return out;
        }
        
        /* Phase 19: Algebraic effects */
        case EX_DEFECT:
            return e;
        /* DV1: Dynamic var nodes pass through CPS transform unchanged */
        case EX_DEFDYNAMIC:
        case EX_DYNVAR_READ:
        case EX_DYNVAR_SET:
            return e;
        case EX_DYNVAR_BINDING:
            return e;

        case EX_PERFORM:
            /* perform is already lowered to shift by effect_lower pass */
            /* Just mark the expression */
            return e;

        case EX_HANDLE:
            /* handle is already lowered to reset by effect_lower pass */
            /* Just mark the expression */
            return e;
        
        case EX_RESUME: {
            Expr *new_k = cps_mark_expr(a, e->as.resume_.resume->k);
            Expr *new_value = cps_mark_expr(a, e->as.resume_.resume->value);
            ResumeExpr *new_resume = arena_alloc(a, sizeof(ResumeExpr));
            new_resume->k = new_k;
            new_resume->value = new_value;
            Expr *out = expr_new(a, EX_RESUME, e->type, e->span);
            out->as.resume_.resume = new_resume;
            return out;
        }
        
        case EX_DISCONTINUE: {
            Expr *new_k = cps_mark_expr(a, e->as.discontinue_.discontinue->k);
            Expr *new_exception = cps_mark_expr(a, e->as.discontinue_.discontinue->exception);
            DiscontinueExpr *new_dc = arena_alloc(a, sizeof(DiscontinueExpr));
            new_dc->k = new_k;
            new_dc->exception = new_exception;
            Expr *out = expr_new(a, EX_DISCONTINUE, e->type, e->span);
            out->as.discontinue_.discontinue = new_dc;
            return out;
        }
        
        case EX_PROGRAM: {
            Expr **new_items = arena_alloc(a, e->as.program.n * sizeof(Expr *));
            for (uint32_t i = 0; i < e->as.program.n; i++) {
                new_items[i] = cps_mark_expr(a, e->as.program.items[i]);
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

/* CPS-CL10 moved to src/elab.c (check_cloneable_capture) for earlier diagnosis. */

/* CPS-CL11: Per-binding clone/drop function name resolution.
 * --------------------------------------------------------------------------
 * For every EX_CLONEABLE_SHIFT, allocate parallel arrays capture_clone_fns[]
 * and capture_drop_fns[] of length n_live_captures.  For each captured
 * binding b, look up the Clone instance for b->type and record the C name
 * of its `clone` method (typically `inst->method_impls[0]->binding->name`).
 *
 * Slots are NULL when no Clone instance is found; emit.c handles NULL by
 * falling back to a bitwise field copy.  The strings stored point into the
 * symbol table (which lives in the same arena as the rest of the program),
 * so they are stable for the remainder of compilation.
 * --------------------------------------------------------------------------*/
static void cps_emit_capture_environment_expr(Arena *a, Expr *e,
                                              TypeClassEnv *tc_env,
                                              TypeClass *clone_tc) {
    if (!e) return;
    switch (e->kind) {
        case EX_CLONEABLE_SHIFT: {
            uint32_t n = e->as.cloneable_shift_.n_live_captures;
            if (n > 0 && !e->as.cloneable_shift_.capture_clone_fns) {
                const char **clone_fns = arena_alloc(a, n * sizeof(const char *));
                const char **drop_fns  = arena_alloc(a, n * sizeof(const char *));
                memset(clone_fns, 0, n * sizeof(const char *));
                memset(drop_fns,  0, n * sizeof(const char *));
                for (uint32_t i = 0; i < n; i++) {
                    Binding *b = e->as.cloneable_shift_.live_captures[i];
                    if (!b || !clone_tc) continue;
                    Type t = b->type;
                    TypeClassInstance *inst =
                        typeclass_env_lookup_instance(tc_env, clone_tc, &t, 1);
                    if (!inst) continue;
                    /* Clone has exactly one method (`clone`), at index 0. */
                    if (inst->n_method_impls > 0 && inst->method_impls[0]
                        && inst->method_impls[0]->binding
                        && inst->method_impls[0]->binding->name) {
                        clone_fns[i] = inst->method_impls[0]->binding->name->name;
                    }
                    /* CF7.2: Drop handled by type-kind dispatch in emit_effects.c;
                     * drop_fns slot left NULL (no formal Drop typeclass yet). */
                }
                e->as.cloneable_shift_.capture_clone_fns = clone_fns;
                e->as.cloneable_shift_.capture_drop_fns  = drop_fns;
            }
            cps_emit_capture_environment_expr(a, e->as.cloneable_shift_.k_fn, tc_env, clone_tc);
            cps_emit_capture_environment_expr(a, e->as.cloneable_shift_.body, tc_env, clone_tc);
            return;
        }
        case EX_CLONEABLE_RESET:
            cps_emit_capture_environment_expr(a, e->as.cloneable_reset_.body, tc_env, clone_tc);
            return;
        case EX_LET:
            for (uint32_t i = 0; i < e->as.let_.n; i++)
                cps_emit_capture_environment_expr(a, e->as.let_.bindings[i].init, tc_env, clone_tc);
            cps_emit_capture_environment_expr(a, e->as.let_.body, tc_env, clone_tc);
            return;
        case EX_IF:
            cps_emit_capture_environment_expr(a, e->as.if_.cond,         tc_env, clone_tc);
            cps_emit_capture_environment_expr(a, e->as.if_.then_,        tc_env, clone_tc);
            cps_emit_capture_environment_expr(a, e->as.if_.else_or_null, tc_env, clone_tc);
            return;
        case EX_DO:
            for (uint32_t i = 0; i < e->as.do_.n; i++)
                cps_emit_capture_environment_expr(a, e->as.do_.items[i], tc_env, clone_tc);
            return;
        case EX_WHILE:
            cps_emit_capture_environment_expr(a, e->as.while_.cond, tc_env, clone_tc);
            cps_emit_capture_environment_expr(a, e->as.while_.body, tc_env, clone_tc);
            return;
        case EX_RETURN:
            cps_emit_capture_environment_expr(a, e->as.return_.value, tc_env, clone_tc);
            return;
        case EX_CALL:
            cps_emit_capture_environment_expr(a, e->as.call_.fn_expr, tc_env, clone_tc);
            for (uint32_t i = 0; i < e->as.call_.n_args; i++)
                cps_emit_capture_environment_expr(a, e->as.call_.args[i], tc_env, clone_tc);
            return;
        case EX_FN_DEF:
            if (e->as.fn_def_.fn)
                cps_emit_capture_environment_expr(a, e->as.fn_def_.fn->body, tc_env, clone_tc);
            return;
        case EX_FN:
            if (e->as.fn_.fn)
                cps_emit_capture_environment_expr(a, e->as.fn_.fn->body, tc_env, clone_tc);
            return;
        case EX_PROGRAM:
            for (uint32_t i = 0; i < e->as.program.n; i++)
                cps_emit_capture_environment_expr(a, e->as.program.items[i], tc_env, clone_tc);
            return;
        default:
            return;
    }
}

void cps_emit_capture_environment(Arena *a, Expr *program, TypeClassEnv *tc_env) {
    if (!program || !tc_env) return;
    TypeClass *clone_tc = NULL;
    for (TypeClass *tc = tc_env->typeclasses; tc != NULL; tc = tc->next) {
        if (tc->name && strcmp(tc->name->name, "Clone") == 0) {
            clone_tc = tc;
            break;
        }
    }
    /* If Clone isn't defined we still walk so capture_*_fns gets nulled out. */
    cps_emit_capture_environment_expr(a, program, tc_env, clone_tc);
}

/* CPS1: Transitive call-graph coloring support. ==============================
 *
 * After cps_mark_expr has directly marked functions containing control
 * operators, we propagate "may_capture" backward through the call graph:
 * if a callee is colored, its caller is colored too.  We do this as a
 * simple fixed-point iteration over the top-level FnDef list.
 */

/* Walk an expression and check if it calls any binding in the colored set.
 * `colored` is an array of `n` Binding pointers known to be colored.
 * Returns true if the expression (or any sub-expression) calls one of them,
 * or if it makes any indirect call (conservative over-approximation). */
static bool cps_body_calls_colored(const Expr *e,
                                   Binding **colored, uint32_t n) {
    if (!e) return false;
    switch (e->kind) {
        case EX_CALL: {
            if (e->as.call_.fn_binding) {
                for (uint32_t i = 0; i < n; i++) {
                    if (e->as.call_.fn_binding == colored[i]) return true;
                }
            } else {
                /* Indirect call: conservatively treat as calling colored */
                return true;
            }
            for (uint32_t i = 0; i < e->as.call_.n_args; i++)
                if (cps_body_calls_colored(e->as.call_.args[i], colored, n))
                    return true;
            if (e->as.call_.fn_expr)
                return cps_body_calls_colored(e->as.call_.fn_expr, colored, n);
            return false;
        }
        case EX_LET:
            for (uint32_t i = 0; i < e->as.let_.n; i++)
                if (cps_body_calls_colored(e->as.let_.bindings[i].init, colored, n))
                    return true;
            return cps_body_calls_colored(e->as.let_.body, colored, n);
        case EX_IF:
            return cps_body_calls_colored(e->as.if_.cond, colored, n) ||
                   cps_body_calls_colored(e->as.if_.then_, colored, n) ||
                   cps_body_calls_colored(e->as.if_.else_or_null, colored, n);
        case EX_DO:
            for (uint32_t i = 0; i < e->as.do_.n; i++)
                if (cps_body_calls_colored(e->as.do_.items[i], colored, n))
                    return true;
            return false;
        case EX_WHILE:
            return cps_body_calls_colored(e->as.while_.cond, colored, n) ||
                   cps_body_calls_colored(e->as.while_.body, colored, n);
        case EX_SET:
            return cps_body_calls_colored(e->as.set_.value, colored, n);
        case EX_RETURN:
            return cps_body_calls_colored(e->as.return_.value, colored, n);
        case EX_FN_DEF:
            return e->as.fn_def_.fn
                ? cps_body_calls_colored(e->as.fn_def_.fn->body, colored, n)
                : false;
        case EX_FN:
            return e->as.fn_.fn
                ? cps_body_calls_colored(e->as.fn_.fn->body, colored, n)
                : false;
        case EX_CLOSURE:
            return e->as.closure_.closure && e->as.closure_.closure->fn
                ? cps_body_calls_colored(e->as.closure_.closure->fn->body, colored, n)
                : false;
        case EX_BUILTIN:
            for (uint32_t i = 0; i < e->as.builtin.n; i++)
                if (cps_body_calls_colored(e->as.builtin.args[i], colored, n))
                    return true;
            return false;
        case EX_RESET:
            return cps_body_calls_colored(e->as.reset_.body, colored, n);
        case EX_SHIFT:
            return cps_body_calls_colored(e->as.shift_.k_fn, colored, n) ||
                   cps_body_calls_colored(e->as.shift_.body, colored, n);
        case EX_SHIFT0:
            return cps_body_calls_colored(e->as.shift0_.k_fn, colored, n) ||
                   cps_body_calls_colored(e->as.shift0_.body, colored, n);
        case EX_SERIAL_RESET:
            return cps_body_calls_colored(e->as.serial_reset_.body, colored, n);
        case EX_SERIAL_SHIFT:
            return cps_body_calls_colored(e->as.serial_shift_.k_fn, colored, n) ||
                   cps_body_calls_colored(e->as.serial_shift_.body, colored, n);
        /* saffron-effect-row-lost-through-unannotated-call: the Saffron nodes
         * and the `any` widen/readers, exactly the set `cps_directly_uses_control`
         * above already carries -- and for the same reason, one step removed.
         *
         * That walk asks "is a control op UNDER this node"; this one asks "is a
         * call to a COLORED function under it", and the backward fixpoint below
         * is what turns the second into coloring.  With no arms here, a caller
         * whose only effectful call sits inside a dynamic operand fell to
         * `default: return false` and was never colored -- so the handler's DK
         * was not threaded through it and the callee's `perform` searched a
         * root that had no handler: `tur: unhandled effect (tag N)` at run time,
         * on a program the interpreter ran correctly.
         *
         * `(+ (g) 1)` in a Saffron file is enough, since `+` on an `any` IS a
         * dynamic op -- which is to say, most of the arithmetic in most Saffron
         * programs. */
        case EX_UNION_INJECT:
            return cps_body_calls_colored(e->as.union_inject_.value, colored, n);
        case EX_DYN_OP:
            for (uint32_t i = 0; i < e->as.dyn_op_.n_args; i++)
                if (cps_body_calls_colored(e->as.dyn_op_.args[i], colored, n)) return true;
            return false;
        case EX_DYN_CALL:
            /* The callee is an expression, so this is an indirect call -- the
             * EX_CALL arm above treats one as conservatively colored, and the
             * same reasoning applies here. */
            return true;
        case EX_DYN_FIELD:
            return cps_body_calls_colored(e->as.dyn_field_.obj, colored, n);
        case EX_DYN_METHOD:
            if (cps_body_calls_colored(e->as.dyn_method_.obj, colored, n)) return true;
            for (uint32_t i = 0; i < e->as.dyn_method_.n_args; i++)
                if (cps_body_calls_colored(e->as.dyn_method_.args[i], colored, n)) return true;
            return false;
        case EX_ANY_CAST:
            return cps_body_calls_colored(e->as.any_cast_.value, colored, n);
        case EX_ANY_IS:
            return cps_body_calls_colored(e->as.any_is_.value, colored, n);
        default:
            return false;
    }
}

/* CPS1: Safely read a FnDef's may_capture field; arena-allocated FnDefs that
 * were never touched by cps_mark_expr may have uninitialized bytes. */
static bool fn_is_colored(const FnDef *fd) {
    uint8_t raw;
    memcpy(&raw, &fd->may_capture, 1);
    return raw != 0;
}

/* Propagate may_capture transitively through the call graph of the given
 * program (which must be EX_PROGRAM).  Runs as a fixed-point iteration. */
static void cps_propagate_coloring(Expr *program) {
    if (!program || program->kind != EX_PROGRAM) return;

    uint32_t n = program->as.program.n;

    /* Collect all top-level FnDef entries */
    FnDef **fns     = malloc(n * sizeof(FnDef *));
    Binding **binds = malloc(n * sizeof(Binding *));
    uint32_t nfns   = 0;
    for (uint32_t i = 0; i < n; i++) {
        Expr *item = program->as.program.items[i];
        if (!item || item->kind != EX_FN_DEF) continue;
        FnDef *fd = item->as.fn_def_.fn;
        if (!fd || !fd->binding) continue;
        /* Normalize may_capture: arena memory may be uninitialised; treat
         * any non-zero byte as true so UBSAN is not triggered. */
        fd->may_capture = fn_is_colored(fd);
        fns[nfns]     = fd;
        binds[nfns++] = fd->binding;
    }

    /* Collect initially colored bindings */
    Binding **colored = malloc(nfns * sizeof(Binding *));
    uint32_t ncolored = 0;
    for (uint32_t i = 0; i < nfns; i++) {
        if (fns[i]->may_capture) colored[ncolored++] = binds[i];
    }

    /* Fixed-point: propagate colored → callers */
    bool changed = true;
    while (changed) {
        changed = false;
        for (uint32_t i = 0; i < nfns; i++) {
            if (fns[i]->may_capture) continue; /* already colored */
            if (!fns[i]->body) continue;
            if (cps_body_calls_colored(fns[i]->body, colored, ncolored)) {
                fns[i]->may_capture = true;
                colored[ncolored++] = binds[i];
                changed = true;
            }
        }
    }

    /* CPS2: mirror may_capture into is_cps for all top-level functions */
    for (uint32_t i = 0; i < nfns; i++) {
        fns[i]->is_cps = fns[i]->may_capture;
    }

    free(fns);
    free(binds);
    free(colored);
}

/* CPS1: --dump-cps-coloring
 * Walk the top-level FnDef list and print each function with its coloring. */
void cps_dump_cps_coloring(const Expr *program, FILE *out) {
    if (!program || program->kind != EX_PROGRAM) return;
    fprintf(out, "=== cps coloring ===\n");
    for (uint32_t i = 0; i < program->as.program.n; i++) {
        const Expr *item = program->as.program.items[i];
        if (!item || item->kind != EX_FN_DEF) continue;
        const FnDef *fd = item->as.fn_def_.fn;
        if (!fd || !fd->binding || !fd->binding->name) continue;
        fprintf(out, "%s: %s\n",
                fd->binding->name->name,
                fd->may_capture ? "colored" : "uncolored");
    }
    fprintf(out, "=== end cps coloring ===\n");
}

/* CPS3: Normalize may_capture and is_cps for FnDef nodes nested inside
 * EX_DEFMODULE items (stdlib/imported modules).  These are never processed
 * by cps_mark_expr (which returns EX_DEFMODULE unchanged) or by
 * cps_propagate_coloring (which only iterates top-level EX_FN_DEF items), so
 * their arena-allocated may_capture/is_cps bytes can be uninitialized garbage.
 * We use cps_fn_needs_transform (body scan) rather than the raw-byte read used
 * by fn_is_colored, because arena memory is not zeroed and a garbage non-zero
 * byte would incorrectly color an innocent stdlib function. */
static void cps_normalize_module_fndefs(Expr *program) {
    if (!program || program->kind != EX_PROGRAM) return;
    for (uint32_t i = 0; i < program->as.program.n; i++) {
        Expr *item = program->as.program.items[i];
        if (!item || item->kind != EX_DEFMODULE) continue;
        DefModule *mod = item->as.defmodule_.mod;
        if (!mod) continue;
        for (uint32_t j = 0; j < mod->n_body; j++) {
            Expr *body_item = mod->body[j];
            if (!body_item || body_item->kind != EX_FN_DEF) continue;
            FnDef *fd = body_item->as.fn_def_.fn;
            if (!fd) continue;
            fd->may_capture = cps_fn_needs_transform(fd);
            fd->is_cps = fd->may_capture;
        }
    }
}

/* Main entry point: mark functions that contain shift for CPS transformation */
Expr *cps_transform(Arena *a, Expr *program, TypeClassEnv *tc_env) {
    if (!program) {
        fprintf(stderr, "cps: NULL program\n");
        return NULL;
    }

    if (program->kind != EX_PROGRAM) {
        fprintf(stderr, "cps: expected EX_PROGRAM, got %d\n", program->kind);
        return NULL;
    }

    /* Always normalize module-nested FnDefs before any emitter reads is_cps.
     * This must run even when the program has no shift (early-return path)
     * because the emitter iterates over all items (including module items) when
     * --cps-path is active, and uninitialized arena bytes trigger UBSAN. */
    cps_normalize_module_fndefs(program);

    /* Check if the program contains any shift expressions */
    if (!cps_expr_contains_shift(program)) {
        return program; /* No CPS transformation needed */
    }

    /* Mark all functions that directly contain shift/reset and populate live_captures */
    Expr *result = cps_mark_expr(a, program);

    /* CPS1: Propagate may_capture transitively through the call graph.
     * A function that calls a colored function is itself colored. */
    cps_propagate_coloring(result);

    /* CPS-CL10: Clone-instance checking is now performed by check_cloneable_capture
     * in src/elab.c at elaboration time.  The CPS-CL11 pass below still resolves
     * per-binding clone/drop function names for emit.c. */

    /* CPS-CL11: record per-binding clone/drop fn names so emit.c can produce
     * field-by-field deep-clone code instead of the v1 bitwise copy. */
    if (result && tc_env) cps_emit_capture_environment(a, result, tc_env);

    return result;
}

/* Phase B5: --dump-clone-plan
 * Walk the program and print a summary of every EX_CLONEABLE_SHIFT site. */
static void cps_dump_clone_plan_expr(const Expr *e, FILE *out, int depth) {
    if (!e) return;
    switch (e->kind) {
        case EX_CLONEABLE_SHIFT: {
            fprintf(out, "[shift %d] %u captured bindings:\n",
                    (int)e->span.line,
                    e->as.cloneable_shift_.n_live_captures);
            for (uint32_t i = 0; i < e->as.cloneable_shift_.n_live_captures; i++) {
                Binding *b = e->as.cloneable_shift_.live_captures[i];
                const char *clone_fn =
                    (e->as.cloneable_shift_.capture_clone_fns &&
                     e->as.cloneable_shift_.capture_clone_fns[i])
                    ? e->as.cloneable_shift_.capture_clone_fns[i]
                    : "(bitwise copy)";
                const char *bname = (b && b->name) ? b->name->name : "?";
                fprintf(out, "  [%u] %s -> %s\n", i, bname, clone_fn);
            }
            cps_dump_clone_plan_expr(e->as.cloneable_shift_.k_fn, out, depth + 1);
            cps_dump_clone_plan_expr(e->as.cloneable_shift_.body, out, depth + 1);
            break;
        }
        case EX_CLONEABLE_RESET:
            cps_dump_clone_plan_expr(e->as.cloneable_reset_.body, out, depth + 1);
            break;
        case EX_PROGRAM:
            for (uint32_t i = 0; i < e->as.program.n; i++)
                cps_dump_clone_plan_expr(e->as.program.items[i], out, depth);
            break;
        case EX_FN_DEF:
            if (e->as.fn_def_.fn && e->as.fn_def_.fn->body)
                cps_dump_clone_plan_expr(e->as.fn_def_.fn->body, out, depth);
            break;
        case EX_FN:
            if (e->as.fn_.fn && e->as.fn_.fn->body)
                cps_dump_clone_plan_expr(e->as.fn_.fn->body, out, depth);
            break;
        case EX_LET:
            for (uint32_t i = 0; i < e->as.let_.n; i++)
                cps_dump_clone_plan_expr(e->as.let_.bindings[i].init, out, depth);
            cps_dump_clone_plan_expr(e->as.let_.body, out, depth);
            break;
        case EX_DO:
            for (uint32_t i = 0; i < e->as.do_.n; i++)
                cps_dump_clone_plan_expr(e->as.do_.items[i], out, depth);
            break;
        case EX_IF:
            cps_dump_clone_plan_expr(e->as.if_.cond, out, depth);
            cps_dump_clone_plan_expr(e->as.if_.then_, out, depth);
            cps_dump_clone_plan_expr(e->as.if_.else_or_null, out, depth);
            break;
        case EX_CALL:
            cps_dump_clone_plan_expr(e->as.call_.fn_expr, out, depth);
            for (uint32_t i = 0; i < e->as.call_.n_args; i++)
                cps_dump_clone_plan_expr(e->as.call_.args[i], out, depth);
            break;
        default:
            break;
    }
    (void)depth;
}

void cps_dump_clone_plan(const Expr *program, FILE *out) {
    if (!program) return;
    fprintf(out, "=== clone plan ===\n");
    cps_dump_clone_plan_expr(program, out, 0);
    fprintf(out, "=== end clone plan ===\n");
}
