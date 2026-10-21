/* elab_memory.c -- ref/lref/deref/drop, rc and weak references, and GC primitives. */
#include "elab_internal.h"
#include <string.h>     /* CG6: strlen for the stat readers */

/* Phase 5: ref — (ref expr)
 * Creates an owning reference to a heap-allocated value.
 * The compiler injects a defer (drop! r) at the binding site for auto-cleanup.
 * 
 * Grammar: (ref expr)
 * Returns: ref<T> where T is the type of expr
 * 
 * Move semantics: Once a ref binding is moved (assigned to another binding),
 * the source is poisoned and cannot be used again.
 */
Expr *elab_ref(Elab *e, const Form *call) {
    /* Minimum: (ref expr) */
    if (call->as.list.len < 2) {
        diag_emit(DIAG_ERROR, call->span,
                  "ref requires an expression: (ref expr)");
        return NULL;
    }
    
    /* Elaborate the inner expression */
    Expr *inner = elab_form(e, call->as.list.items[1]);
    if (!inner) return NULL;
    
    /* ref<T> where T is the inner expression's type */
    Type ref_type = type_ref(inner->type.kind);

    /* Create EX_REF expression */
    Expr *out = expr_new(e->arena, EX_REF, ref_type, call->span);
    out->as.ref_.expr = inner;
    return out;
}

/* LT3: lref/new — (lref/new expr)
 * Heap-allocates expr and returns an lref<T> (linear owning pointer).
 * The caller must consume the returned lref<T> exactly once.
 *
 * Grammar: (lref/new expr)
 * Returns: lref<T>
 */
Expr *elab_lref_new(Elab *e, const Form *call) {
    if (call->as.list.len != 2) {
        diag_emit(DIAG_ERROR, call->span,
                  "lref/new requires an expression: (lref/new expr)");
        return NULL;
    }

    Expr *inner = elab_form(e, call->as.list.items[1]);
    if (!inner) return NULL;

    /* lref<T> where T is the inner expression's type */
    Type lref_type = type_lref(inner->type.kind);

    /* Create EX_REF expression — lref/new lowers identically to ref at C level */
    Expr *out = expr_new(e->arena, EX_REF, lref_type, call->span);
    out->as.ref_.expr = inner;
    return out;
}

/* Phase 5: deref — (@ expr)
 * Dereferences a ref<T> or ptr<T>, returning T.
 * 
 * Grammar: (@ expr)
 * expr must have type ref<T>, rc<T>, or ptr<T>
 * Returns: T
 */
Expr *elab_deref(Elab *e, const Form *call) {
    /* Minimum: (@ expr) */
    if (call->as.list.len < 2) {
        diag_emit(DIAG_ERROR, call->span,
                  "@ requires an expression: (@ expr)");
        return NULL;
    }
    
    /* Elaborate the inner expression */
    Expr *inner = elab_form(e, call->as.list.items[1]);
    if (!inner) return NULL;
    
    /* Check that inner is ref<T>, lref<T>, rc<T>, ptr<T>, &T, or &mut T */
    if (inner->type.kind != TY_REF && inner->type.kind != TY_LREF
        && inner->type.kind != TY_RC && inner->type.kind != TY_PTR_VOID
        && inner->type.kind != TY_REF_IMMUT && inner->type.kind != TY_REF_MUT) {
        diag_emit(DIAG_ERROR, call->span,
                  "@ requires ref<T>, lref<T>, rc<T>, ptr<T>, &T, or &mut T, got %s",
                  type_name(inner->type));
        return NULL;
    }

    /* Return type is the inner type */
    Type result_type;
    if (inner->type.kind == TY_REF || inner->type.kind == TY_LREF) {
        result_type = type_from_kind(inner->type.as.ref.inner);
    } else if (inner->type.kind == TY_RC) {
        /* rc-deref-emits-control-block-pointer: an rc over an ADT carries its
         * AdtDef (elab_rc_of), so hand the reader the full ADT type rather than
         * a def-less TY_ADT that no field access or match can resolve. */
        if (inner->type.as.rc.inner == TY_ADT && inner->type.as.rc.adt_def)
            result_type = type_adt(inner->type.as.rc.adt_def);
        else
            result_type = type_from_kind(inner->type.as.rc.inner);
    } else if (inner->type.kind == TY_REF_IMMUT || inner->type.kind == TY_REF_MUT) {
        /* Phase 12: &T and &mut T dereference to T.
         * borrowed-aggregate-key-skips-the-key-check: T is the borrow's full
         * target when it recorded one (`&Pt` derefs to Pt, not a bare ADT
         * kind), and a `(& K)` derefs to the NAMED tyvar K, so a spec at
         * K = any resolves the read to the tagged box. */
        if (inner->type.as.ref_borrow.target_full) {
            result_type = *inner->type.as.ref_borrow.target_full;
        } else if (inner->type.as.ref_borrow.target == TY_TYVAR &&
                   inner->type.as.ref_borrow.target_tyvar) {
            memset(&result_type, 0, sizeof result_type);
            result_type.kind = TY_TYVAR;
            result_type.as.tyvar_.name = inner->type.as.ref_borrow.target_tyvar;
        } else {
            result_type = type_from_kind(inner->type.as.ref_borrow.target);
        }
    } else {
        /* ptr<void> derefs to void* for now - could be more precise */
        result_type = TYPE_PTR_VOID;
    }
    
    /* Theme 1 (ref<T> deref/auto-drop): deref of a ref<T> is NON-consuming.
     * The generic EX_VAR walk (elab_toplevel.c) marks a linear binding consumed
     * on every use; for a ref<T> we read *through* the handle without taking
     * ownership, so undo that mark.  Ownership is discharged exactly once at
     * scope exit -- either by an explicit (drop! r)/(rc/drop ...)/(ref/from-rc
     * ...) or by the auto-drop injected in elab_let.  Leaving deref consuming
     * forced the must-consume obligation to be met by the read itself, which
     * (a) leaked the allocation and (b) made a following (drop! r) a
     * use-after-consume error.  Restricted to TY_REF so lref<T> borrows and
     * &T/&mut T references keep their existing (consuming) behavior. */
    if (inner->kind == EX_VAR &&
        inner->as.var.binding &&
        inner->as.var.binding->is_linear &&
        !inner->as.var.binding->is_nonowning_ref &&
        inner->type.kind == TY_REF) {
        inner->as.var.binding->is_linear_consumed = false;
    }

    /* Create EX_DEREF expression */
    Expr *out = expr_new(e->arena, EX_DEREF, result_type, call->span);
    out->as.deref_.expr = inner;
    return out;
}

/* Phase 5: drop! — (drop! expr)
 * Explicitly drops a ref<T>, freeing the underlying allocation.
 * Grammar: (drop! expr)
 * Returns: nil
 * Note: This is used by auto-injected defers for ref bindings.
 * Move semantics: After drop!, the ref should not be used (enforced at codegen time
 * by the elaborator not tracking moves yet - future work). */
Expr *elab_drop(Elab *e, const Form *call) {
    /* Minimum: (drop! expr) */
    if (call->as.list.len < 2) {
        diag_emit(DIAG_ERROR, call->span,
                  "drop! requires an expression: (drop! expr)");
        return NULL;
    }
    if (call->as.list.len > 2) {
        diag_emit(DIAG_ERROR, call->span,
                  "drop! takes exactly 1 argument: (drop! expr)");
        return NULL;
    }

    Expr *inner = elab_form(e, call->as.list.items[1]);
    if (!inner) return NULL;

    /* drop! works on ref<T> */
    if (inner->type.kind != TY_REF) {
        diag_emit(DIAG_ERROR, call->span,
                  "drop! requires ref<T>, got %s", type_name(inner->type));
        return NULL;
    }

    /* Look up the drop! builtin spec and create a BUILTIN expression */
    /* drop! is a unary operator on ref<T> that returns nil */
    const BuiltinSpec *spec = builtin_lookup(e->sym_drop, inner->type, 1);
    if (!spec) {
        diag_emit(DIAG_ERROR, call->span,
                  "internal error: drop! builtin not found");
        return NULL;
    }

    /* Create a BUILTIN expression */
    Expr *out = expr_new(e->arena, EX_BUILTIN, TYPE_NIL, call->span);
    out->as.builtin.spec = spec;
    out->as.builtin.n = 1;
    out->as.builtin.args = (Expr **)arena_alloc(e->arena, sizeof(Expr *));
    out->as.builtin.args[0] = inner;

    return out;
}

/* Phase 9: rc<T> operations */

/* (rc/of x) - Create a new rc<T> with x as the value.
 * Allocates a control block, copies/moves x into it.
 * Returns: rc<T>
 */
Expr *elab_rc_of(Elab *e, const Form *call) {
    if (call->as.list.len != 2) {
        diag_emit(DIAG_ERROR, call->span,
                  "(rc/of x) requires exactly one argument");
        return NULL;
    }

    Expr *inner = elab_form(e, call->as.list.items[1]);
    if (!inner) return NULL;

    /* LT1: Reject wrapping a linear value in rc<T> */
    if (inner->type.copy_kind == CK_LINEAR) {
        const char *val_name = (inner->kind == EX_VAR)
            ? inner->as.var.binding->name->name : "linear value";
        diag_emit_with_code(DIAG_ERROR, call->span,
                            TUR_E0103_LINEAR_IN_RC,
                            "cannot wrap linear value '%s' in rc<T> -- "
                            "shared ownership violates linearity",
                            val_name);
        return NULL;
    }

    /* UT1: Reject wrapping a unique value in rc<T> */
    if (inner->kind == EX_VAR &&
        inner->as.var.binding->is_unique) {
        diag_emit_with_code(DIAG_ERROR, call->span,
                            TUR_E0202_UNIQUE_IN_RC,
                            "cannot wrap unique value '%s' in rc<T> -- "
                            "shared ownership violates uniqueness",
                            inner->as.var.binding->name->name);
        return NULL;
    }

    /* rc<T> where T is the inner expression's type.  CONV-S1 (slice 2): when
     * wrapping a single-variant record ADT (every lowered `defstruct` is one),
     * carry its AdtDef so `(.field rc-of-adt)` / `(set! (.field ..) v)` can
     * auto-deref through the rc and resolve fields.
     * structdef-retirement DS-C: the former `TY_STRUCT` arm (carrying a
     * `StructDef` on the rc) is dead -- structs lower to record ADTs, so `inner`
     * is never `TY_STRUCT`; a former struct wraps through the `TY_ADT` arm. */
    Type rc_type;
    if (inner->type.kind == TY_ADT && inner->type.as.adt_.def) {
        rc_type = type_rc_adt(inner->type.as.adt_.def);
    } else {
        rc_type = type_rc(inner->type.kind);
    }

    /* Create EX_RC_OF expression */
    Expr *out = expr_new(e->arena, EX_RC_OF, rc_type, call->span);
    out->as.rc_of_.expr = inner;
    return out;
}

/* (rc/clone r) - Increment strong count, return new rc<T> pointing to same value.
 * Returns: rc<T>
 *
 * EXG4-2: Also accepts a constrained existential (`(exists [a] [(C a) ...] T)`)
 * value, since at runtime that is an rc-managed pointer.  The result preserves
 * the same existential type so the cloned reference is open-able just like the
 * original.
 */
Expr *elab_rc_clone(Elab *e, const Form *call) {
    if (call->as.list.len != 2) {
        diag_emit(DIAG_ERROR, call->span,
                  "(rc/clone r) requires exactly one argument");
        return NULL;
    }

    Expr *inner = elab_form(e, call->as.list.items[1]);
    if (!inner) return NULL;

    /* Argument must be rc<T> or a constrained existential. */
    bool is_rc = (inner->type.kind == TY_RC);
    bool is_constrained_exists = (inner->type.kind == TY_EXISTS
                                  && inner->type.as.forall_.n_constraints > 0);
    if (!is_rc && !is_constrained_exists) {
        diag_emit(DIAG_ERROR, call->span,
                  "rc/clone requires rc<T> or a constrained existential, got %s",
                  type_name(inner->type));
        return NULL;
    }

    /* rc/clone returns the same type as input */
    Type rc_type = inner->type;

    /* Create EX_RC_CLONE expression */
    Expr *out = expr_new(e->arena, EX_RC_CLONE, rc_type, call->span);
    out->as.rc_clone_.expr = inner;
    out->as.rc_clone_.elide = false;  /* Phase 9 follow-up: elision pass may set true */
    return out;
}

/* (rc/drop r) - Decrement strong count.
 * Returns: nil
 */
Expr *elab_rc_drop(Elab *e, const Form *call) {
    if (call->as.list.len != 2) {
        diag_emit(DIAG_ERROR, call->span,
                  "(rc/drop r) requires exactly one argument");
        return NULL;
    }

    Expr *inner = elab_form(e, call->as.list.items[1]);
    if (!inner) return NULL;

    /* Argument must be rc<T> or a constrained existential.
     * EXG4-2: constrained existentials are rc-managed at runtime, so the
     * same decrement applies. */
    bool is_rc = (inner->type.kind == TY_RC);
    bool is_constrained_exists = (inner->type.kind == TY_EXISTS
                                  && inner->type.as.forall_.n_constraints > 0);
    if (!is_rc && !is_constrained_exists) {
        diag_emit(DIAG_ERROR, call->span,
                  "rc/drop requires rc<T> or a constrained existential, got %s",
                  type_name(inner->type));
        return NULL;
    }

    /* Create EX_RC_DROP expression */
    Expr *out = expr_new(e->arena, EX_RC_DROP, TYPE_NIL, call->span);
    out->as.rc_drop_.expr = inner;
    out->as.rc_drop_.elide = false;  /* Phase 9 follow-up: elision pass may set true */
    return out;
}

/* (rc->ptr r) - Borrow a ptr<T> from an rc<T>.
 * Returns: ptr<void> (for now; could be more precise with generics)
 */
Expr *elab_rc_ptr(Elab *e, const Form *call) {
    if (call->as.list.len != 2) {
        diag_emit(DIAG_ERROR, call->span,
                  "(rc->ptr r) requires exactly one argument");
        return NULL;
    }

    Expr *inner = elab_form(e, call->as.list.items[1]);
    if (!inner) return NULL;

    /* Argument must be rc<T> */
    if (inner->type.kind != TY_RC) {
        diag_emit(DIAG_ERROR, call->span,
                  "rc->ptr requires rc<T>, got %s", type_name(inner->type));
        return NULL;
    }

    /* Returns ptr<void> for now */
    Type result_type = TYPE_PTR_VOID;

    /* Create EX_RC_PTR expression */
    Expr *out = expr_new(e->arena, EX_RC_PTR, result_type, call->span);
    out->as.rc_ptr_.expr = inner;
    return out;
}

/* (rc/strong-count r) - Get the strong count for debugging.
 * Returns: int
 */
Expr *elab_rc_strong_count(Elab *e, const Form *call) {
    if (call->as.list.len != 2) {
        diag_emit(DIAG_ERROR, call->span,
                  "(rc/strong-count r) requires exactly one argument");
        return NULL;
    }

    Expr *inner = elab_form(e, call->as.list.items[1]);
    if (!inner) return NULL;

    /* Argument must be rc<T> or a constrained existential.
     * EXG4-2: constrained existentials are rc-managed at runtime, so the
     * same strong-count read applies. */
    bool is_rc = (inner->type.kind == TY_RC);
    bool is_constrained_exists = (inner->type.kind == TY_EXISTS
                                  && inner->type.as.forall_.n_constraints > 0);
    if (!is_rc && !is_constrained_exists) {
        diag_emit(DIAG_ERROR, call->span,
                  "rc/strong-count requires rc<T> or a constrained existential, got %s",
                  type_name(inner->type));
        return NULL;
    }

    /* Create EX_RC_COUNT expression */
    Expr *out = expr_new(e->arena, EX_RC_COUNT, TYPE_INT, call->span);
    out->as.rc_count_.expr = inner;
    return out;
}

/* (rc/from-ref r) - Move a ref<T> into rc<T>.
 * Returns: rc<T>
 */
Expr *elab_rc_from_ref(Elab *e, const Form *call) {
    if (call->as.list.len != 2) {
        diag_emit(DIAG_ERROR, call->span,
                  "(rc/from-ref r) requires exactly one argument");
        return NULL;
    }

    Expr *inner = elab_form(e, call->as.list.items[1]);
    if (!inner) return NULL;

    if (inner->type.kind != TY_REF) {
        diag_emit(DIAG_ERROR, call->span,
                  "rc/from-ref requires ref<T>, got %s", type_name(inner->type));
        return NULL;
    }

    if (inner->kind == EX_VAR) {
        binding_mark_moved(inner->as.var.binding, inner->span);
    }

    Type rc_type = type_rc(inner->type.as.ref.inner);
    Expr *out = expr_new(e->arena, EX_RC_FROM_REF, rc_type, call->span);
    out->as.rc_from_ref_.expr = inner;
    return out;
}

/* (ref/from-rc r) - Extract a ref<T> from a unique rc<T>.
 * Requires strong-count == 1 and no weak observers at runtime.
 * Returns: ref<T>
 */
Expr *elab_ref_from_rc(Elab *e, const Form *call) {
    if (call->as.list.len != 2) {
        diag_emit(DIAG_ERROR, call->span,
                  "(ref/from-rc r) requires exactly one argument");
        return NULL;
    }

    Expr *inner = elab_form(e, call->as.list.items[1]);
    if (!inner) return NULL;

    if (inner->type.kind != TY_RC) {
        diag_emit(DIAG_ERROR, call->span,
                  "ref/from-rc requires rc<T>, got %s", type_name(inner->type));
        return NULL;
    }

    if (inner->kind == EX_VAR) {
        binding_mark_moved(inner->as.var.binding, inner->span);
    }

    Type ref_type = type_ref(inner->type.as.rc.inner);
    Expr *out = expr_new(e->arena, EX_REF_FROM_RC, ref_type, call->span);
    out->as.ref_from_rc_.expr = inner;
    return out;
}

/* Phase 9: weak<T> operations */

/* (weak r) - Create a weak<T> from an rc<T>.
 * Returns: weak<T>
 */
Expr *elab_weak(Elab *e, const Form *call) {
    if (call->as.list.len != 2) {
        diag_emit(DIAG_ERROR, call->span,
                  "(weak r) requires exactly one argument");
        return NULL;
    }

    Expr *inner = elab_form(e, call->as.list.items[1]);
    if (!inner) return NULL;

    /* Argument must be rc<T> */
    if (inner->type.kind != TY_RC) {
        diag_emit(DIAG_ERROR, call->span,
                  "weak requires rc<T>, got %s", type_name(inner->type));
        return NULL;
    }

    /* weak<T> where T is the inner type of the rc */
    Type weak_type = type_weak(inner->type.as.rc.inner);

    /* Create EX_WEAK expression */
    Expr *out = expr_new(e->arena, EX_WEAK, weak_type, call->span);
    out->as.weak_.expr = inner;
    return out;
}

/* (upgrade w) -- retired (ref-weak-builtins-stubbed).  It returned the
 * option as a `ptr<void>` that no `Option` function accepts, and the strong
 * handle it minted on success could never be released.  `weak/upgrade`
 * (tur/weak) is the same operation with the right type, `(Option rc<T>)`,
 * so the builtin points there rather than keeping a second spelling. */
Expr *elab_weak_upgrade(Elab *e, const Form *call) {
    diag_emit(DIAG_ERROR, call->span,
              "`upgrade` was retired: use `weak/upgrade` from tur/weak, which "
              "returns an `(Option rc<T>)` -- and `weak/unwrap` and `rc/drop` "
              "for the strong handle it hands back");
    return NULL;
}

/* `ref?` / `weak?` only look at their argument's type: like `deref` of a
 * ref<T>, a bare linear variable passed to one is read, not consumed, so the
 * generic EX_VAR walk's consumed mark is undone (see elab_deref). */
static void elab_pred_read_is_not_a_move(Expr *inner) {
    if (inner->kind == EX_VAR && inner->as.var.binding &&
        inner->as.var.binding->is_linear &&
        !inner->as.var.binding->is_nonowning_ref &&
        (inner->type.kind == TY_REF || inner->type.kind == TY_WEAK))
        inner->as.var.binding->is_linear_consumed = false;
}

/* (weak? w) - Check if w is a weak<T>.
 * Returns: bool
 */
Expr *elab_weak_pred(Elab *e, const Form *call) {
    if (call->as.list.len != 2) {
        diag_emit(DIAG_ERROR, call->span,
                  "(weak? w) requires exactly one argument");
        return NULL;
    }

    Expr *inner = elab_form(e, call->as.list.items[1]);
    if (!inner) return NULL;
    elab_pred_read_is_not_a_move(inner);

    /* Create EX_WEAK_PRED expression -- answered from inner's static type
     * (emit_expr.c, eval.c). */
    Expr *out = expr_new(e->arena, EX_WEAK_PRED, TYPE_BOOL, call->span);
    out->as.weak_pred_.expr = inner;
    return out;
}

/* (ref? x) - Check if x is a ref<T>.
 * Returns: bool
 */
Expr *elab_ref_pred(Elab *e, const Form *call) {
    if (call->as.list.len != 2) {
        diag_emit(DIAG_ERROR, call->span,
                  "(ref? x) requires exactly one argument");
        return NULL;
    }

    Expr *inner = elab_form(e, call->as.list.items[1]);
    if (!inner) return NULL;
    elab_pred_read_is_not_a_move(inner);

    /* Create EX_REF_PRED expression -- true iff inner's static type is
     * ref<T> (emit_expr.c, eval.c). */
    Expr *out = expr_new(e->arena, EX_REF_PRED, TYPE_BOOL, call->span);
    out->as.ref_pred_.expr = inner;
    return out;
}

/* Phase 10: GC builtins */

/* (gc!) - Force a garbage collection cycle. Returns nil. */
Expr *elab_gc_force(Elab *e, const Form *call) {
    if (call->as.list.len != 1) {
        diag_emit(DIAG_ERROR, call->span,
                  "(gc!) takes no arguments");
        return NULL;
    }
    /* For Phase 10 v1: emit as inline-C with gc_force() call */
    InlineC *ic = (InlineC *)arena_alloc(e->arena, sizeof(InlineC));
    memset(ic, 0, sizeof(InlineC));
    ic->code = strslice("gc_force();", 11);
    ic->return_type = TYPE_NIL;
    ic->captures = NULL;
    ic->n_captures = 0;
    ic->val_exprs = NULL;
    ic->n_val_exprs = 0;
    Expr *out = expr_new(e->arena, EX_INLINE_C, TYPE_NIL, call->span);
    out->as.inline_c_.inline_c = ic;
    return out;
}

/* (gc-enable!) - Enable cycle collection. Returns nil. */
Expr *elab_gc_enable(Elab *e, const Form *call) {
    if (call->as.list.len != 1) {
        diag_emit(DIAG_ERROR, call->span,
                  "(gc-enable!) takes no arguments");
        return NULL;
    }
    InlineC *ic = (InlineC *)arena_alloc(e->arena, sizeof(InlineC));
    memset(ic, 0, sizeof(InlineC));
    ic->code = strslice("gc_enable();", 12);
    ic->return_type = TYPE_NIL;
    ic->captures = NULL;
    ic->n_captures = 0;
    ic->val_exprs = NULL;
    ic->n_val_exprs = 0;
    Expr *out = expr_new(e->arena, EX_INLINE_C, TYPE_NIL, call->span);
    out->as.inline_c_.inline_c = ic;
    return out;
}

/* (gc-disable!) - Disable cycle collection. Returns nil. */
Expr *elab_gc_disable(Elab *e, const Form *call) {
    if (call->as.list.len != 1) {
        diag_emit(DIAG_ERROR, call->span,
                  "(gc-disable!) takes no arguments");
        return NULL;
    }
    InlineC *ic = (InlineC *)arena_alloc(e->arena, sizeof(InlineC));
    memset(ic, 0, sizeof(InlineC));
    ic->code = strslice("gc_disable();", 13);
    ic->return_type = TYPE_NIL;
    ic->captures = NULL;
    ic->n_captures = 0;
    ic->val_exprs = NULL;
    ic->n_val_exprs = 0;
    Expr *out = expr_new(e->arena, EX_INLINE_C, TYPE_NIL, call->span);
    out->as.inline_c_.inline_c = ic;
    return out;
}

/* CG5: (gc-auto!) -- switch the collector to GC_AUTO, where collections run
 * automatically at allocation checkpoints (see gc_on_alloc_checkpoint in
 * src/runtime/gc.c).  Returns nil.
 *
 * CG8 GRADUATED 2026-08-17: this is an ordinary call form now, no `--enable`.
 * Note what that does and does not mean.  It ungates the CALL; it does not make
 * automatic collection a default, and never will -- a program that does not
 * call this runs the pure-RC path with no collector overhead, exactly as
 * before, and the AUTO-only alloc-path cost (the rc_cb_alloc_kinded payload
 * zeroing) is conditional on GC_AUTO mode at RUN time.  See
 * docs/archive/gc-cycle-collection-followup-plan.md. */
Expr *elab_gc_auto(Elab *e, const Form *call) {
    if (call->as.list.len != 1) {
        diag_emit(DIAG_ERROR, call->span,
                  "(gc-auto!) takes no arguments");
        return NULL;
    }

    InlineC *ic = (InlineC *)arena_alloc(e->arena, sizeof(InlineC));
    memset(ic, 0, sizeof(InlineC));
    ic->code = strslice("gc_auto();", 10);
    ic->return_type = TYPE_NIL;
    ic->captures = NULL;
    ic->n_captures = 0;
    ic->val_exprs = NULL;
    ic->n_val_exprs = 0;
    Expr *out = expr_new(e->arena, EX_INLINE_C, TYPE_NIL, call->span);
    out->as.inline_c_.inline_c = ic;
    return out;
}


/* CG6: the four GC statistics readers.
 *
 * Four separate readers rather than one `(gc-stats)` returning a record: every
 * one of these is a plain count, so `:int` is the honest type, not an `:int`
 * standing in for something structured (see CLAUDE.md).  A record would have to
 * be built and boxed by an inline-C body for no gain -- and callers that want
 * one can define it in Turmeric over these.
 *
 * Ungated, unlike (gc-auto!): reading a counter changes no behaviour, and the
 * whole point of CG6 is to make the collector's work visible -- including to
 * someone deciding whether the experiment is worth enabling. */
static Expr *elab_gc_stat_reader(Elab *e, const Form *call,
                                 const char *form_name, const char *c_call) {
    if (call->as.list.len != 1) {
        diag_emit(DIAG_ERROR, call->span, "(%s) takes no arguments", form_name);
        return NULL;
    }
    /* An EX_INLINE_C in VALUE position is spliced as a C expression, not as a
     * statement block (emit_expr.c, case EX_INLINE_C) -- so `c_call` must be a
     * bare expression.  Writing `return ...;` here produced
     * `printf("%lld", (long long)(return ...;))`. */
    InlineC *ic = (InlineC *)arena_alloc(e->arena, sizeof(InlineC));
    memset(ic, 0, sizeof(InlineC));
    ic->code = strslice(c_call, (uint32_t)strlen(c_call));
    ic->return_type = TYPE_INT;
    ic->captures = NULL;
    ic->n_captures = 0;
    ic->val_exprs = NULL;
    ic->n_val_exprs = 0;
    Expr *out = expr_new(e->arena, EX_INLINE_C, TYPE_INT, call->span);
    out->as.inline_c_.inline_c = ic;
    return out;
}

/* (gc-collections) -- collections run so far. */
Expr *elab_gc_collections(Elab *e, const Form *call) {
    return elab_gc_stat_reader(e, call, "gc-collections",
                               "(int64_t)gc_stat_collections()");
}

/* (gc-objects-freed) -- control blocks the collector has reclaimed. */
Expr *elab_gc_objects_freed(Elab *e, const Form *call) {
    return elab_gc_stat_reader(e, call, "gc-objects-freed",
                               "(int64_t)gc_stat_objects_freed()");
}

/* (gc-live-blocks) -- rc blocks currently registered. */
Expr *elab_gc_live_blocks(Elab *e, const Form *call) {
    return elab_gc_stat_reader(e, call, "gc-live-blocks",
                               "(int64_t)gc_stat_live_blocks()");
}

/* (gc-candidate-high-water) -- peak candidate-buffer occupancy.  The one that
 * answers "is the collector keeping up?": gc_suspect_count is instantaneous and
 * sits near zero right after any collection, so sampling it tells you little. */
Expr *elab_gc_candidate_high_water(Elab *e, const Form *call) {
    return elab_gc_stat_reader(e, call, "gc-candidate-high-water",
                               "(int64_t)gc_stat_candidate_high_water()");
}
