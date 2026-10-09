/* effect_check.c — Effect-row inference pass (Phase P19-2). */

#include "effect_check.h"

#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "buf.h"
#include "builtins.h"
#include "diag.h"
#include "effect.h"
#include "cps.h"            /* cps_visit_children */
#include "expr.h"
#include "globals.h"
#include "typeclass.h"

/* ---------------------------------------------------------------------------
 * Binding → FnDef index
 * An open-addressed hash on the binding pointer, used to look up the FnDef for
 * a callee binding during propagation.  It grows: a callee that is not in the
 * index contributes only its declared row, so a fixed cap that dropped entries
 * (the old 1024-slot array did) silently loses every inferred effect of the
 * functions past it -- and a `#lang r7rs` program indexes well over 1024
 * (tests/fixtures/errors/effect-row-past-1024-fns).
 * --------------------------------------------------------------------------- */

typedef struct {
    const Binding *binding;
    FnDef         *fn;
} FnEntry;

typedef struct {
    FnEntry  *slots;   /* cap entries; binding == NULL marks an empty slot */
    uint32_t  n;
    uint32_t  cap;     /* a power of two, or 0 before the first add */
} FnIndex;

/* The env's effect named `name` (a C string), or NULL. */
static Effect *find_effect_named(EffectEnv *env, const char *name) {
    if (!env || !name) return NULL;
    for (uint32_t i = 0; i < env->n_effects; i++) {
        Effect *eff = env->effects[i];
        if (eff && eff->name && eff->name->name && strcmp(eff->name->name, name) == 0)
            return eff;
    }
    return NULL;
}

/* effect-row-honesty-plan W4: a builtin's declared capability (IO on
 * `println`) enters the caller's inferred row, exactly as a callee's declared
 * capability does for an ordinary call. */
static EffectRow *merge_builtin_effect(Arena *a, EffectRow *row, EffectEnv *env,
                                       const char *effect_name) {
    Effect *eff = find_effect_named(env, effect_name);
    if (!eff) return row;
    return effect_row_merge(a, row, effect_row_single(a, eff));
}

static Effect *find_unsafe_effect(EffectEnv *env) {
    if (!env) return NULL;
    for (uint32_t i = 0; i < env->n_effects; i++) {
        Effect *eff = env->effects[i];
        if (eff && eff->name && eff->name->name &&
            strcmp(eff->name->name, EFFECT_NAME_UNSAFE) == 0) {
            return eff;
        }
    }
    return NULL;
}

/* Returns true if e contains any EX_INLINE_C node (shallow: only checks the
 * immediate expression, not inside nested function literals). */
static bool expr_has_inline_c(Expr *e) {
    if (!e) return false;
    if (e->kind == EX_INLINE_C) return true;
    if (e->kind == EX_DO) {
        for (uint32_t i = 0; i < e->as.do_.n; i++)
            if (expr_has_inline_c(e->as.do_.items[i])) return true;
    }
    return false;
}

static uint32_t fn_index_slot(const FnEntry *slots, uint32_t cap,
                              const Binding *b) {
    uint64_t h = (uint64_t)(uintptr_t)b;
    h ^= h >> 33;
    h *= 0xff51afd7ed558ccdULL;
    h ^= h >> 33;
    uint32_t mask = cap - 1;
    uint32_t i = (uint32_t)h & mask;
    while (slots[i].binding && slots[i].binding != b) i = (i + 1) & mask;
    return i;
}

/* The first FnDef added for a binding wins; a later add is ignored. */
static void fn_index_add(FnIndex *idx, const Binding *b, FnDef *fn) {
    if (!b) return;
    if ((idx->n + 1) * 2 > idx->cap) {
        uint32_t cap = idx->cap ? idx->cap * 2 : 256;
        FnEntry *grown = calloc(cap, sizeof(FnEntry));
        if (!grown) { fprintf(stderr, "tur: oom\n"); abort(); }
        for (uint32_t i = 0; i < idx->cap; i++) {
            if (!idx->slots[i].binding) continue;
            grown[fn_index_slot(grown, cap, idx->slots[i].binding)] = idx->slots[i];
        }
        free(idx->slots);
        idx->slots = grown;
        idx->cap = cap;
    }
    uint32_t i = fn_index_slot(idx->slots, idx->cap, b);
    if (idx->slots[i].binding) return;
    idx->slots[i].binding = b;
    idx->slots[i].fn      = fn;
    idx->n++;
}

static FnDef *fn_index_lookup(const FnIndex *idx, const Binding *b) {
    if (!b || idx->cap == 0) return NULL;
    return idx->slots[fn_index_slot(idx->slots, idx->cap, b)].fn;
}

/* ---------------------------------------------------------------------------
 * Single-function effect-row inference
 * --------------------------------------------------------------------------- */

/* ER5 / PR5-2: Module currently being analysed during effect-row inference.
 * Set by effect_check_pass before each function's fixed-point iteration so
 * that private effects of other modules can be filtered at call sites.
 * NULL during analysis of top-level (non-module) code. */
static const Symbol *s_current_analysis_module = NULL;

/* ER5 / PR5-2: Strip private effects of other modules from a concrete row.
 * When A calls B and B's row contains private effects of B, those must not
 * leak into A's inferred row (A cannot name, perform, or handle them). */
static EffectRow *filter_cross_module_private(Arena *a, EffectRow *row) {
    if (!row || row->kind != ERK_CONCRETE || !s_current_analysis_module)
        return row;
    uint8_t n = row->as.concrete.n_effects;
    Effect **filtered = arena_alloc(a, n * sizeof(Effect *));
    uint8_t k = 0;
    for (uint8_t i = 0; i < n; i++) {
        Effect *eff = row->as.concrete.effects[i];
        if (eff && eff->is_private
            && eff->defining_module_name
            && eff->defining_module_name != s_current_analysis_module)
            continue; /* strip: private to a different module */
        filtered[k++] = eff;
    }
    if (k == n) return row;
    if (k == 0) return effect_row_empty(a);
    return effect_row_concrete(a, filtered, k);
}

/* saffron-effect-row-lost-through-unannotated-call: the operand lists of the
 * four Saffron dynamic nodes, in ONE place.
 *
 * Every walk in this file is structural, and none of them had an arm for
 * EX_DYN_OP / EX_DYN_CALL / EX_DYN_FIELD / EX_DYN_METHOD -- so each fell to its
 * `default` and stopped, and a `perform`, an effectful call, or a nested
 * `handle` reachable only THROUGH a dynamic node was invisible to the whole
 * pass.  `(+ (g) 1)` is enough: the `+` is dynamic in a Saffron file, so the
 * call to `g` sat under a node the walk did not descend into, the caller's
 * inferred row came back empty, and an enclosing handler was reported
 * unreachable (TUR-W0033) before the program aborted `unhandled effect`.
 *
 * Same species as `collect_free_vars` missing these arms
 * (saffron-dynamic-surface-pass H1), and the reason it is a shared accessor
 * rather than four copies: four transcriptions of the same shape knowledge is
 * how the next node kind gets added to three of them.
 *
 * Note the report's own hypothesis -- that `any` carries no effect row, so the
 * row was lost through the unannotated RETURN -- was wrong, and measurably so:
 * the same program with the dynamic operator removed (`(defn f [] (g))`)
 * propagates the row and prints the right answer with every signature still
 * unannotated.  The `any` return was never involved. */
static uint32_t saffron_dyn_child_count(const Expr *e) {
    if (!e) return 0;
    switch (e->kind) {
    case EX_DYN_OP:     return e->as.dyn_op_.n_args;
    case EX_DYN_CALL:   return 1 + e->as.dyn_call_.n_args;
    case EX_DYN_FIELD:  return 1;
    case EX_DYN_METHOD: return 1 + e->as.dyn_method_.n_args;
    /* ...and the `any` WIDEN and readers, which the first round of arms missed
     * for the reason the same omission was missed in cps.c's walk: nothing then
     * put one between a defn's body and its effectful call.  The RETURN-position
     * widen does, on every unannotated Saffron defn whose body is not already an
     * `any` -- so `(defn tc [] (do (g) 1))` had its whole body inside an
     * EX_UNION_INJECT, the walk stopped there, and the row came back empty while
     * `(defn tc [] (> (g) 0))` -- no widen -- inferred correctly two lines away.
     * That divergence is what identified them. */
    case EX_UNION_INJECT:
    case EX_ANY_CAST:
    case EX_ANY_IS:
    case EX_ANY_TYPE_OF: return 1;
    default:            return 0;
    }
}

static Expr *saffron_dyn_child(const Expr *e, uint32_t i) {
    if (!e || i >= saffron_dyn_child_count(e)) return NULL;
    switch (e->kind) {
    case EX_DYN_OP:     return e->as.dyn_op_.args[i];
    case EX_DYN_CALL:   return i == 0 ? e->as.dyn_call_.fn
                                      : e->as.dyn_call_.args[i - 1];
    case EX_DYN_FIELD:  return e->as.dyn_field_.obj;
    case EX_DYN_METHOD: return i == 0 ? e->as.dyn_method_.obj
                                      : e->as.dyn_method_.args[i - 1];
    case EX_UNION_INJECT: return e->as.union_inject_.value;
    case EX_ANY_CAST:     return e->as.any_cast_.value;
    case EX_ANY_IS:       return e->as.any_is_.value;
    case EX_ANY_TYPE_OF:  return e->as.any_type_of_.value;
    default:            return NULL;
    }
}

/* Walk an expression tree and collect into *row all effects that are directly
 * performed (EX_PERFORM) or transitively performed (EX_CALL to a known defn
 * whose inferred row is already populated).
 *
 * Phase P19-4: `subst` is an effect-row substitution used to resolve row
 * variables when a polymorphic callee is instantiated at a call site.  Pass a
 * fresh empty EffectRowSubst for each top-level function analysis. */
/* FH4.2: remove the effect(s) handled by a handler value from a row.  Recurses
 * into handler literals (their case effect names) and compositions.  For a
 * handler bound to a variable, the handled effect name is only available as a
 * C-string on the TY_HANDLER type (not a Symbol*), which effect_row_remove
 * cannot consume here; that case is left conservative (the handled effect may
 * over-propagate as a TUR-W0030 warning rather than being silently dropped). */
static EffectRow *remove_handler_effects(EffectRow *row, Expr *hv, Arena *a) {
    if (!hv) return row;
    if (hv->kind == EX_HANDLER_LIT) {
        HandleExpr *h = hv->as.handler_lit_.handle;
        for (uint8_t i = 0; i < h->n_cases; i++)
            row = effect_row_remove(row, h->cases[i].effect_name, a);
    } else if (hv->kind == EX_COMPOSE_HANDLERS) {
        row = remove_handler_effects(row, hv->as.compose_handlers_.h1, a);
        row = remove_handler_effects(row, hv->as.compose_handlers_.h2, a);
    }
    return row;
}

static EffectRow *collect_effects_in_expr(Arena *a, Expr *e,
                                          EffectRow *row,
                                          const FnIndex *idx,
                                          EffectEnv *env,
                                          EffectRowSubst *subst) {
    if (!e) return row;

    switch (e->kind) {
    case EX_PERFORM: {
        /* Look up the effect by name in the environment. */
        const PerformExpr *p = e->as.perform_.perform;
        Effect *eff = effect_env_lookup(env, p->effect_name);
        if (eff) {
            EffectRow *single = effect_row_single(a, eff);
            row = effect_row_merge(a, row, single);
        }
        /* Also recurse into arguments. */
        for (uint32_t i = 0; i < p->n_args; i++) {
            row = collect_effects_in_expr(a, p->args[i], row, idx, env, subst);
        }
        return row;
    }

    case EX_CALL: {
        /* Phase P19-4: propagate from a known callee's already-inferred row,
         * applying any row-variable substitutions before merging. */

        /* Phase 16 v2: indirect capability field call — fn_expr is EX_GET_FIELD.
         * Propagate the effect-row annotation on the :fn struct field (if any). */
        if (e->as.call_.fn_expr &&
            e->as.call_.fn_expr->kind == EX_GET_FIELD) {            Expr *gf = e->as.call_.fn_expr;
            uint32_t fidx = gf->as.get_field_.field_idx;
            EffectRow *field_effect_row = NULL;
            /* structdef-retirement slice 5 A1 / DS-D: a `defstruct` is a
             * single-variant record ADT, so the capability field's effect row
             * lives on the CtorField.  Read it off adt_ctor so `(.run v)` on a
             * lowered `[run : fn #fx{Eff}]` field still contributes its effect --
             * without this the lowering would silently drop effect tracking. */
            const struct CtorDef *ac = gf->as.get_field_.adt_ctor;
            if (ac && fidx < ac->n_fields)
                field_effect_row = ac->fields[fidx].effect_row;
            if (field_effect_row) {
                EffectRow *field_row = effect_row_apply_subst(
                    field_effect_row, subst, a);
                row = effect_row_merge(a, row, field_row);
            }
            /* Also recurse into the struct expression and arguments. */
            row = collect_effects_in_expr(a, gf->as.get_field_.struct_expr, row, idx, env, subst);
            for (uint32_t i = 0; i < e->as.call_.n_args; i++) {
                row = collect_effects_in_expr(a, e->as.call_.args[i], row, idx, env, subst);
            }
            return row;
        }

        /* Phase H §1: dict-dispatch call -- fn_expr is EX_DICT (static singleton
         * load).  ER3: Propagate the typeclass method's declared effect row to the
         * caller's inferred row so that effectful typeclass methods are tracked. */
        if (e->as.call_.fn_expr &&
            e->as.call_.fn_expr->kind == EX_DICT) {
            /* ER3: Propagate the typeclass method's declared effect row. */
            Expr *d = e->as.call_.fn_expr;
            TypeClassInstance *inst = d->as.dict_.instance;
            if (inst && inst->typeclass) {
                const char *dict_mname = d->as.dict_.method_name;
                for (uint8_t mi = 0; mi < inst->typeclass->n_methods; mi++) {
                    TypeClassMethod *meth = &inst->typeclass->methods[mi];
                    if (!meth->name || !meth->effect_row) continue;
                    /* Compare sanitized method name with dict_mname. */
                    const char *orig = meth->name->name;
                    bool match = true;
                    uint32_t k = 0;
                    for (; orig[k] || dict_mname[k]; k++) {
                        char a_ch = orig[k], b_ch = dict_mname[k];
                        if (!isalnum((unsigned char)a_ch) && a_ch != '_') a_ch = '_';
                        if (a_ch != b_ch) { match = false; break; }
                    }
                    if (match) {
                        EffectRow *mrow = effect_row_apply_subst(meth->effect_row, subst, a);
                        row = effect_row_merge(a, row, mrow);
                        break;
                    }
                }
            }
            for (uint32_t i = 0; i < e->as.call_.n_args; i++) {
                row = collect_effects_in_expr(a, e->as.call_.args[i], row, idx, env, subst);
            }
            return row;
        }

        FnDef *callee = fn_index_lookup(idx, e->as.call_.fn_binding);

        /* ER2: Row-variable unification at call sites.
         * For each function-typed parameter that carries a row variable,
         * unify that variable with the actual argument's inferred effect row,
         * then apply the updated substitution when merging the callee's row. */
        if (callee && callee->params && callee->n_params > 0 &&
            e->as.call_.n_args > 0) {
            uint32_t n_check = callee->n_params < (uint8_t)e->as.call_.n_args
                                ? callee->n_params : (uint8_t)e->as.call_.n_args;
            for (uint8_t pi = 0; pi < n_check; pi++) {
                Binding *param = callee->params[pi];
                if (!param || param->type.kind != TY_FN) continue;
                EffectRow *param_row = param->type.as.fn.effect_row;
                if (!param_row || param_row->kind != ERK_VAR) continue;

                /* Determine the actual argument's effect row.  Peel the
                 * fat-normalization shim first (EX_FN_TO_FAT boxes a bare-fn
                 * or lambda argument into a fat handle) and erased
                 * ascriptions: the row belongs to the wrapped value.  Without
                 * the peel, row-variable unification never runs for a
                 * normalized effectful slot -- which silently disarmed both
                 * the occurs check (TUR-E0254 negatives) and the row-var
                 * mismatch inference (TUR-E0009). */
                Expr *actual = e->as.call_.args[pi];
                while (actual &&
                       (actual->kind == EX_ASCRIBE ||
                        actual->kind == EX_FN_TO_FAT))
                    actual = (actual->kind == EX_ASCRIBE)
                                 ? actual->as.ascribe_.inner
                                 : actual->as.fn_to_fat_.inner;
                EffectRow *actual_row = NULL;
                if (actual && actual->kind == EX_CLOSURE) {
                    /* Closure literal: collect its body effects with a fresh subst. */
                    EffectRowSubst *arg_subst = effect_row_subst_new(a);
                    actual_row = collect_effects_in_expr(
                        a, actual->as.closure_.closure->fn->body,
                        effect_row_empty(a), idx, env, arg_subst);
                } else if (actual && actual->kind == EX_VAR &&
                           actual->as.var.binding) {
                    /* Named function: prefer the declared row for occurs-check
                     * purposes -- the inferred row may have already resolved
                     * row variables to concrete effects, hiding the open variable.
                     * Fall back to the inferred row if no declaration exists. */
                    FnDef *arg_fn = fn_index_lookup(idx, actual->as.var.binding);
                    EffectRow *decl_row = actual->as.var.binding->type.kind == TY_FN
                                         ? actual->as.var.binding->type.as.fn.effect_row
                                         : NULL;
                    if (decl_row && (decl_row->kind == ERK_VAR ||
                                     decl_row->kind == ERK_UNION)) {
                        /* Use the declared row: it may contain open row variables. */
                        actual_row = decl_row;
                    } else if (arg_fn && arg_fn->inferred_effect_row) {
                        actual_row = arg_fn->inferred_effect_row;
                    } else {
                        actual_row = decl_row;
                    }
                }
                if (actual_row) {
                    if (!effect_row_unify(param_row, actual_row, subst, a)) {
                        diag_emit_with_code(DIAG_ERROR, e->span,
                            TUR_E0254_INFINITE_EFFECT_ROW,
                            "occurs-check failure: unifying effect row variable '%s' with "
                            "row containing itself would produce an infinite effect row",
                            param_row->as.var.var_name->name);
                    }
                }
            }
        }

        /* w0033-unreachable-clause-false-positive-through-fn-value: a fn-typed
         * parameter with NO row annotation admits an effectful argument (the
         * CPS coloring threads it, and the callee calls it), but nothing
         * above carries that argument's effects: ER2 unifies only a
         * row-VARIABLE param, and a call through an un-annotated param adds
         * nothing to the callee's own row.  So `(use1 ask-plus 1)` read as
         * performing nothing -- a handler around it was reported unreachable
         * (TUR-W0033), and a fn value of a function that only performs that
         * way was built with no CPS entry, aborting `unhandled effect` when
         * called through.  Charge the argument's row to this call, as the
         * callee may invoke it.  Conservative for a callee that only stores
         * the value: its effect still happens when something calls it. */
        if (callee && callee->params && callee->n_params > 0 &&
            e->as.call_.n_args > 0) {
            uint32_t n_open = callee->n_params < e->as.call_.n_args
                                ? callee->n_params : e->as.call_.n_args;
            for (uint32_t pi = 0; pi < n_open; pi++) {
                Binding *param = callee->params[pi];
                if (!param) continue;
                /* An un-annotated `(fn [int] int)` in parameter position is
                 * a rank-2 poly-fn param (ptr<void> at the C level, its
                 * signature on poly_type); a bare TY_FN param counts too.
                 * Either is "open" exactly when no row is written on it. */
                const Type *pfn = NULL;
                if (param->type.kind == TY_FN) pfn = &param->type;
                else if (param->is_poly_fn) {
                    pfn = param->poly_type;          /* NULL: a bare `: fn` */
                    while (pfn && pfn->kind == TY_FORALL) pfn = pfn->as.forall_.body;
                    if (param->poly_type && (!pfn || pfn->kind != TY_FN)) continue;
                } else continue;
                /* A written row (`#fx{}` included) is ER2's or TUR-E0009's --
                 * except a bare row VARIABLE, `(fn [int] #fx{e} int)`: ER2
                 * binds `e` to the argument's row above, but a call through
                 * the parameter charges nothing to the callee's own inferred
                 * row, so unless the callee also declares `#fx{e}` the
                 * binding reached no one -- the same false TUR-W0033 on a
                 * handler around the call, found 2026-10-09 writing
                 * generic-hof-effectful-callback.  Charge it here too. */
                if (pfn && pfn->as.fn.effect_row
                    && pfn->as.fn.effect_row->kind != ERK_VAR) continue;
                /* Peel the shims a fn-value argument rides in: an erased
                 * ascription, the fat normalization, and the poly-fn wrapper
                 * an un-annotated `(fn [int] int)` slot builds. */
                Expr *actual = e->as.call_.args[pi];
                while (actual &&
                       (actual->kind == EX_ASCRIBE ||
                        actual->kind == EX_FN_TO_FAT ||
                        actual->kind == EX_POLY_WRAP))
                    actual = actual->kind == EX_ASCRIBE  ? actual->as.ascribe_.inner
                           : actual->kind == EX_FN_TO_FAT ? actual->as.fn_to_fat_.inner
                           : actual->as.poly_wrap_.inner;
                EffectRow *actual_row = NULL;
                if (actual && actual->kind == EX_CLOSURE &&
                    actual->as.closure_.closure &&
                    actual->as.closure_.closure->fn) {
                    EffectRowSubst *arg_subst = effect_row_subst_new(a);
                    actual_row = collect_effects_in_expr(
                        a, actual->as.closure_.closure->fn->body,
                        effect_row_empty(a), idx, env, arg_subst);
                } else if (actual && actual->kind == EX_VAR &&
                           actual->as.var.binding) {
                    FnDef *arg_fn = fn_index_lookup(idx, actual->as.var.binding);
                    if (arg_fn && arg_fn->inferred_effect_row)
                        actual_row = arg_fn->inferred_effect_row;
                    else if (actual->as.var.binding->type.kind == TY_FN)
                        actual_row = actual->as.var.binding->type.as.fn.effect_row;
                }
                if (actual_row)
                    row = effect_row_merge(a, row,
                                           effect_row_apply_subst(actual_row, subst, a));
            }
        }

        if (callee && callee->inferred_effect_row) {
            /* Apply substitution to resolve any row variables in the callee's
             * inferred row (e.g., from a polymorphic higher-order parameter). */
            EffectRow *callee_row =
                effect_row_apply_subst(callee->inferred_effect_row, subst, a);
            /* ER5 / PR5-2: strip private effects of other modules. */
            callee_row = filter_cross_module_private(a, callee_row);
            row = effect_row_merge(a, row, callee_row);
            /* Explicit unsafe annotation on the callee should also propagate. */
            Effect *unsafe_eff = find_unsafe_effect(env);
            if (callee->binding &&
                callee->binding->type.kind == TY_FN &&
                callee->binding->type.as.fn.effect_row &&
                unsafe_eff &&
                effect_row_contains(callee->binding->type.as.fn.effect_row, unsafe_eff)) {
                row = effect_row_merge(a, row, effect_row_single(a, unsafe_eff));
            }
            /* stdlib-effect-rows: capability tags (e.g. #{FS}) are never inferred
             * from a callee's body, so propagate them from the callee's *declared*
             * row into the caller's inferred row.  This makes a #{} (or
             * mis-tagged) caller of an FS-tagged function fail row-checking. */
            if (callee->binding &&
                callee->binding->type.kind == TY_FN) {
                EffectRow *cdecl = callee->binding->type.as.fn.effect_row;
                if (cdecl && cdecl->kind == ERK_CONCRETE) {
                    for (uint8_t ci = 0; ci < cdecl->as.concrete.n_effects; ci++) {
                        Effect *ceff = cdecl->as.concrete.effects[ci];
                        if (ceff && ceff->is_capability) {
                            row = effect_row_merge(a, row, effect_row_single(a, ceff));
                        }
                    }
                }
            }
        } else if (e->as.call_.fn_binding) {
            /* Callee not in the top-level index (e.g., a higher-order param).
             * If the binding's declared effect row has been resolved via the
             * current substitution, contribute those effects.  Guard on TY_FN:
             * a higher-order param may be a rank-N poly value (TY_FORALL), whose
             * union member is not the fn record -- reading .as.fn.effect_row off
             * a non-fn type yields a garbage pointer (surfaced when the Type
             * union shrank under out-of-line fn arg storage). */
            EffectRow *decl_row = e->as.call_.fn_binding->type.kind == TY_FN
                ? e->as.call_.fn_binding->type.as.fn.effect_row
                : NULL;
            if (decl_row) {
                EffectRow *resolved = effect_row_apply_subst(decl_row, subst, a);
                if (resolved && resolved->kind == ERK_CONCRETE) {
                    row = effect_row_merge(a, row, resolved);
                }
            }
            /* Note: extern-c calls are NOT automatically inferred as Unsafe here.
             * Stdlib wrapper functions (e.g. hamt.tur) wrap unsafe C and present
             * a safe interface to callers; adding Unsafe inference here would
             * cause TUR-W0030 under --strict-effects for those unannotated wrappers. */
        }

        /* ER2: Apply substitution to the callee's declared row.
         * When the callee declares #{e} and we just bound e -> {Ask}, this
         * merges {Ask} into the caller's row even if the callee's inferred
         * row is empty (e.g., because calling a row-variable param was not
         * tracked by the fixed-point). */
        if (callee && callee->binding) {
            EffectRow *callee_decl = callee->binding->type.as.fn.effect_row;
            if (callee_decl && callee_decl->kind == ERK_VAR) {
                EffectRow *resolved = effect_row_apply_subst(callee_decl, subst, a);
                if (resolved && resolved->kind != ERK_VAR) {
                    row = effect_row_merge(a, row, resolved);
                }
            }
        }

        /* Recurse into arguments. */
        for (uint32_t i = 0; i < e->as.call_.n_args; i++) {
            row = collect_effects_in_expr(a, e->as.call_.args[i], row, idx, env, subst);
        }
        return row;
    }

    case EX_LET: {
        for (uint32_t i = 0; i < e->as.let_.n; i++) {
            row = collect_effects_in_expr(a, e->as.let_.bindings[i].init, row, idx, env, subst);
        }
        return collect_effects_in_expr(a, e->as.let_.body, row, idx, env, subst);
    }

    case EX_DO: {
        for (uint32_t i = 0; i < e->as.do_.n; i++) {
            row = collect_effects_in_expr(a, e->as.do_.items[i], row, idx, env, subst);
        }
        return row;
    }

    case EX_IF:
        row = collect_effects_in_expr(a, e->as.if_.cond,       row, idx, env, subst);
        row = collect_effects_in_expr(a, e->as.if_.then_,      row, idx, env, subst);
        row = collect_effects_in_expr(a, e->as.if_.else_or_null, row, idx, env, subst);
        return row;

    case EX_WHILE:
        row = collect_effects_in_expr(a, e->as.while_.cond, row, idx, env, subst);
        row = collect_effects_in_expr(a, e->as.while_.body, row, idx, env, subst);
        return row;

    case EX_SET:
        return collect_effects_in_expr(a, e->as.set_.value, row, idx, env, subst);

    case EX_DEF:
        return collect_effects_in_expr(a, e->as.def_.init, row, idx, env, subst);

    case EX_BUILTIN:
        for (uint32_t i = 0; i < e->as.builtin.n; i++) {
            row = collect_effects_in_expr(a, e->as.builtin.args[i], row, idx, env, subst);
        }
        if (e->as.builtin.spec && e->as.builtin.spec->effect)
            row = merge_builtin_effect(a, row, env, e->as.builtin.spec->effect);
        return row;

    case EX_RETURN:
        return collect_effects_in_expr(a, e->as.return_.value, row, idx, env, subst);

    case EX_PANIC:
        return collect_effects_in_expr(a, e->as.panic_.payload, row, idx, env, subst);
    case EX_PANIC_WITH:
        return collect_effects_in_expr(a, e->as.panic_with_.payload, row, idx, env, subst);
    case EX_CATCH_UNWIND:
        return collect_effects_in_expr(a, e->as.catch_unwind_.thunk, row, idx, env, subst);
    case EX_CATCH_PANIC_OF:
        return collect_effects_in_expr(a, e->as.catch_panic_of_.thunk, row, idx, env, subst);
    case EX_PANIC_PAYLOAD_TYPE:
    case EX_PANIC_PAYLOAD_VALUE:
    case EX_PANIC_PAYLOAD_FILE:
    case EX_PANIC_PAYLOAD_LINE:
        return collect_effects_in_expr(a, e->as.panic_payload_type_.payload, row, idx, env, subst);
    case EX_PANIC_PAYLOAD_DOWNS:
        return collect_effects_in_expr(a, e->as.panic_payload_downs_.payload, row, idx, env, subst);



    case EX_DEFER:
        return collect_effects_in_expr(a, e->as.defer_.body, row, idx, env, subst);

    case EX_RESET:
        return collect_effects_in_expr(a, e->as.reset_.body, row, idx, env, subst);

    case EX_SHIFT:
        row = collect_effects_in_expr(a, e->as.shift_.k_fn, row, idx, env, subst);
        return collect_effects_in_expr(a, e->as.shift_.body, row, idx, env, subst);

    case EX_SHIFT0:
        row = collect_effects_in_expr(a, e->as.shift0_.k_fn, row, idx, env, subst);
        return collect_effects_in_expr(a, e->as.shift0_.body, row, idx, env, subst);

    /* Phase B2: Cloneable continuations */
    case EX_CLONEABLE_RESET:
        return collect_effects_in_expr(a, e->as.cloneable_reset_.body, row, idx, env, subst);

    case EX_CLONEABLE_SHIFT:
        row = collect_effects_in_expr(a, e->as.cloneable_shift_.k_fn, row, idx, env, subst);
        return collect_effects_in_expr(a, e->as.cloneable_shift_.body, row, idx, env, subst);

    case EX_HANDLE: {
        /* Effects performed inside a handle body are handled (absorbed) by the
         * handler when a matching case exists; only effects NOT covered by a
         * case bubble up to the caller.
         *
         * Effect re-opening: a handler case body may itself perform effects
         * (e.g., it calls (perform Write ...) while handling a Read effect).
         * Those effects propagate to the enclosing function.
         *
         * Algorithm:
         *   1. Collect the body's effect row into body_row.
         *   2. For each handler case: remove its handled effect from body_row
         *      (it is absorbed here) and collect the case body's effects into
         *      the caller's row (they are re-opened / propagated outward).
         *   3. Merge any unhandled body effects into the caller's row. */
        HandleExpr *h = e->as.handle_.handle;
        EffectRow *body_row = collect_effects_in_expr(
            a, h->body, effect_row_empty(a), idx, env, subst);
        for (uint8_t i = 0; i < h->n_cases; i++) {
            /* Absorb the handled effect from the body row -- but only for a DEEP
             * handler.  A shallow handler (handle-shallow, F2) handles the first
             * perform and then removes itself, so a re-perform in the resumed
             * continuation escapes to the enclosing handler; the effect stays in
             * the row (and in the enclosing fn's inferred row), so an outer
             * handler for it is correctly seen as reachable (no false TUR-W0033). */
            if (!h->shallow)
                body_row = effect_row_remove(body_row, h->cases[i].effect_name, a);
            /* Propagate effects performed inside the handler case body. */
            row = collect_effects_in_expr(a, h->cases[i].body, row, idx, env, subst);
        }
        /* Any body effects that were not covered by a case propagate upward. */
        row = effect_row_merge(a, row, body_row);
        return row;
    }

    case EX_HANDLER_LIT: {
        /* FH2/FH4: a handler value's case bodies may re-open effects.  The
         * literal itself handles nothing in place (it is detached from a body),
         * so it only contributes the effects performed inside its case bodies. */
        HandleExpr *h = e->as.handler_lit_.handle;
        for (uint8_t i = 0; i < h->n_cases; i++)
            row = collect_effects_in_expr(a, h->cases[i].body, row, idx, env, subst);
        return row;
    }

    case EX_COMPOSE_HANDLERS:
        /* FH5: re-opened effects from both composed handlers' case bodies. */
        row = collect_effects_in_expr(a, e->as.compose_handlers_.h1, row, idx, env, subst);
        row = collect_effects_in_expr(a, e->as.compose_handlers_.h2, row, idx, env, subst);
        return row;

    case EX_WITH_HANDLER: {
        /* FH4.2: applying a handler discharges its handled effect(s) from the
         * body's row; leftover (unhandled) body effects and effects re-opened
         * by the handler's case bodies propagate upward -- mirroring EX_HANDLE. */
        Expr *hv = e->as.with_handler_.handler;
        EffectRow *body_row = collect_effects_in_expr(
            a, e->as.with_handler_.body, effect_row_empty(a), idx, env, subst);
        body_row = remove_handler_effects(body_row, hv, a);
        /* Re-opened effects performed inside the handler's case bodies. */
        row = collect_effects_in_expr(a, hv, row, idx, env, subst);
        row = effect_row_merge(a, row, body_row);
        return row;
    }

    case EX_RESUME:
        row = collect_effects_in_expr(a, e->as.resume_.resume->k, row, idx, env, subst);
        return collect_effects_in_expr(a, e->as.resume_.resume->value, row, idx, env, subst);

    case EX_REF:
        return collect_effects_in_expr(a, e->as.ref_.expr, row, idx, env, subst);
    case EX_DEREF:
        if (e->as.deref_.expr && e->as.deref_.expr->type.kind == TY_PTR_VOID) {
            Effect *unsafe_eff = find_unsafe_effect(env);
            if (unsafe_eff) row = effect_row_merge(a, row, effect_row_single(a, unsafe_eff));
        }
        return collect_effects_in_expr(a, e->as.deref_.expr, row, idx, env, subst);

    case EX_BORROW_IMMUT:
        if (e->as.borrow_immut_.expr &&
            e->as.borrow_immut_.expr->type.kind == TY_PTR_VOID) {
            Effect *unsafe_eff = find_unsafe_effect(env);
            if (unsafe_eff) row = effect_row_merge(a, row, effect_row_single(a, unsafe_eff));
        }
        return collect_effects_in_expr(a, e->as.borrow_immut_.expr, row, idx, env, subst);
    case EX_BORROW_MUT:
        if (e->as.borrow_mut_.expr &&
            e->as.borrow_mut_.expr->type.kind == TY_PTR_VOID) {
            Effect *unsafe_eff = find_unsafe_effect(env);
            if (unsafe_eff) row = effect_row_merge(a, row, effect_row_single(a, unsafe_eff));
        }
        return collect_effects_in_expr(a, e->as.borrow_mut_.expr, row, idx, env, subst);

    case EX_RC_OF:
        return collect_effects_in_expr(a, e->as.rc_of_.expr, row, idx, env, subst);
    case EX_RC_CLONE:
        return collect_effects_in_expr(a, e->as.rc_clone_.expr, row, idx, env, subst);
    case EX_RC_DROP:
        return collect_effects_in_expr(a, e->as.rc_drop_.expr, row, idx, env, subst);
    case EX_RC_PTR:
        return collect_effects_in_expr(a, e->as.rc_ptr_.expr, row, idx, env, subst);
    case EX_RC_COUNT:
        return collect_effects_in_expr(a, e->as.rc_count_.expr, row, idx, env, subst);
    case EX_RC_FROM_REF:
        return collect_effects_in_expr(a, e->as.rc_from_ref_.expr, row, idx, env, subst);
    case EX_REF_FROM_RC:
        return collect_effects_in_expr(a, e->as.ref_from_rc_.expr, row, idx, env, subst);
    case EX_WEAK:
        return collect_effects_in_expr(a, e->as.weak_.expr, row, idx, env, subst);
    case EX_WEAK_UPGRADE:
        return collect_effects_in_expr(a, e->as.weak_upgrade_.expr, row, idx, env, subst);
    case EX_WEAK_PRED:
        return collect_effects_in_expr(a, e->as.weak_pred_.expr, row, idx, env, subst);
    case EX_REF_PRED:
        return collect_effects_in_expr(a, e->as.ref_pred_.expr, row, idx, env, subst);

    case EX_CLOSURE:
        return collect_effects_in_expr(a, e->as.closure_.closure->fn->body, row, idx, env, subst);

    case EX_FN_DEF:
        /* Inner defn: do not recurse — that defn is processed separately. */
        return row;

    case EX_MAKE_STRUCT:
        for (uint32_t i = 0; i < e->as.make_struct_.n_fields; i++) {
            row = collect_effects_in_expr(a, e->as.make_struct_.field_values[i], row, idx, env, subst);
        }
        return row;

    case EX_SET_LIT:
        for (uint32_t i = 0; i < e->as.set_lit_.n; i++) {
            row = collect_effects_in_expr(a, e->as.set_lit_.items[i], row, idx, env, subst);
        }
        return row;

    case EX_GET_FIELD:
        return collect_effects_in_expr(a, e->as.get_field_.struct_expr, row, idx, env, subst);

    case EX_CONT_PRED:
        return collect_effects_in_expr(a, e->as.cont_pred_.expr, row, idx, env, subst);

    case EX_SET_DEREF:
        row = collect_effects_in_expr(a, e->as.set_deref_.ref,   row, idx, env, subst);
        return collect_effects_in_expr(a, e->as.set_deref_.value, row, idx, env, subst);

    case EX_DISCONTINUE:
        row = collect_effects_in_expr(a, e->as.discontinue_.discontinue->k,         row, idx, env, subst);
        return collect_effects_in_expr(a, e->as.discontinue_.discontinue->exception, row, idx, env, subst);

    case EX_DYN_OP:
    case EX_DYN_CALL:
    case EX_DYN_FIELD:
    case EX_DYN_METHOD:
    case EX_UNION_INJECT:
    case EX_ANY_CAST:
    case EX_ANY_IS:
    case EX_ANY_TYPE_OF:
        /* saffron-effect-row-lost-through-unannotated-call: descend into the
         * Saffron dynamic nodes' operands.  Exactly the reasoning the
         * EX_ASCRIBE arm below states, for the nodes a `#lang saffron` file is
         * mostly MADE of -- without it the inferred row omits an effect
         * performed under any dynamic operator, call, field read or method
         * dispatch. */
        for (uint32_t i = 0; i < saffron_dyn_child_count(e); i++)
            row = collect_effects_in_expr(a, saffron_dyn_child(e, i), row, idx, env, subst);
        /* A dynamic `println` (Saffron, an `any` argument) is the same builtin,
         * dispatched at run time: it carries the same declared effect. */
        if (e->kind == EX_DYN_OP && e->as.dyn_op_.op && e->as.dyn_op_.op->name) {
            const char *bfx = builtin_effect_for_name(e->as.dyn_op_.op->name);
            if (bfx) row = merge_builtin_effect(a, row, env, bfx);
        }
        return row;

    case EX_ASCRIBE:
        /* A type ascription `(:: <inner> T)` is erased at codegen; descend into
         * the inner expression so a `perform` (or effectful call) that appears
         * ONLY inside an ascription is still tracked.  Without this the
         * function's inferred row omits the effect -- causing a false
         * TUR-W0033 "handler clause is unreachable" and under-inferring the
         * declared-row check (TUR-E0009). */
        return collect_effects_in_expr(a, e->as.ascribe_.inner, row, idx, env, subst);

    default:
        return row;
    }
}

/* ---------------------------------------------------------------------------
 * effect_row_check_declared
 * --------------------------------------------------------------------------- */

int effect_row_check_declared(FnDef *fd, Arena *a) {
    if (!fd) return 0;

    /* Declared row lives in the function type. */
    EffectRow *declared = fd->binding
                            ? fd->binding->type.as.fn.effect_row
                            : NULL;

    /* Unannotated functions are unconstrained in v1 — skip the check. */
    if (!declared) return 0;

    EffectRow *inferred = fd->inferred_effect_row;
    if (!inferred) inferred = effect_row_empty(a);

    if (effect_row_is_subset(inferred, declared)) return 0;

    /* Inferred row is not a subset of the declared row: emit TUR-E0009 for each
     * violating effect so the user knows exactly which effect is undeclared. */
    int rc = 0;
    Span fn_span = fd->binding ? fd->binding->span : (Span){0, 0, 0, 0, 0, 0};

    if (inferred->kind == ERK_CONCRETE) {
        for (uint8_t i = 0; i < inferred->as.concrete.n_effects; i++) {
            Effect *eff = inferred->as.concrete.effects[i];
            if (!effect_row_contains(declared, eff)) {
                /* Build a human-readable declared-row string. */
                Buf decl_str;
                buf_init(&decl_str);
                effect_row_print(&decl_str, declared);

                char who[256];
                diag_emit_with_code(DIAG_ERROR, fn_span,
                    TUR_E0009_EFFECT_ROW_MISMATCH,
                    "%s performs effect '%s' but its declared row %.*s "
                    "does not include it",
                    binding_fn_describe(fd->binding, who, sizeof(who)),
                    eff->name->name,
                    (int)decl_str.len, decl_str.data);
                buf_free(&decl_str);
                rc = 1;
            }
        }
    } else {
        /* Non-concrete inferred row: emit a generic mismatch. */
        Buf inferred_str, decl_str;
        buf_init(&inferred_str);
        buf_init(&decl_str);
        effect_row_print(&inferred_str, inferred);
        effect_row_print(&decl_str, declared);

        char who[256];
        diag_emit_with_code(DIAG_ERROR, fn_span,
            TUR_E0009_EFFECT_ROW_MISMATCH,
            "%s: inferred effect row %.*s is not a subset of declared row %.*s",
            binding_fn_describe(fd->binding, who, sizeof(who)),
            (int)inferred_str.len, inferred_str.data,
            (int)decl_str.len,    decl_str.data);
        buf_free(&inferred_str);
        buf_free(&decl_str);
        rc = 1;
    }
    return rc;
}

/* ER1: Emit TUR-W0031 for each effect declared but never inferred (over-annotation).
 * Returns 0 always -- this is a warning, not an error. */
static int effect_row_check_over_annotated(FnDef *fd, Arena *a) {
    if (!fd) return 0;
    EffectRow *declared = fd->binding
                            ? fd->binding->type.as.fn.effect_row
                            : NULL;
    /* Only annotated functions with concrete declared rows */
    if (!declared || declared->kind != ERK_CONCRETE) return 0;
    /* An instance method's row is copied from its class method (Step 0b): it
     * is what the class allows every instance, not a claim this instance
     * made, and an instance that performs less than the class allows is
     * normal.  The instance cannot change the row, so the warning would have
     * nothing for its reader to do. */
    if (fd->binding && fd->binding->is_instance_method) return 0;
    EffectRow *inferred = fd->inferred_effect_row;
    if (!inferred) inferred = effect_row_empty(a);

    Span fn_span = fd->binding ? fd->binding->span : (Span){0, 0, 0, 0, 0, 0};
    for (uint8_t i = 0; i < declared->as.concrete.n_effects; i++) {
        Effect *eff = declared->as.concrete.effects[i];
        /* stdlib-effect-rows: a capability tag (e.g. #{FS}) marks a function's
         * authority, not an effect it `perform`s, so it is justified by its
         * annotation alone -- never flag it as over-annotated. */
        if (eff->is_capability) continue;
        if (!effect_row_contains(inferred, eff)) {
            /* ET4: Also suppress if the inferred row contains a subeffect of `eff`.
             * A function declaring #{IO} that performs #{Write} (where Write extends IO)
             * is not over-annotated -- Write's presence covers IO. */
            bool covered_by_subeffect = false;
            if (inferred->kind == ERK_CONCRETE) {
                for (uint8_t j = 0; j < inferred->as.concrete.n_effects; j++) {
                    if (effect_is_subeffect(inferred->as.concrete.effects[j], eff)) {
                        covered_by_subeffect = true;
                        break;
                    }
                }
            }
            if (!covered_by_subeffect) {
                /* Suppress W0031 for #{Unsafe} when the body contains inline C:
                 * inline C is inherently unsafe but the effect system does not
                 * infer Unsafe from it, so the annotation is always justified. */
                bool suppress = false;
                if (strcmp(eff->name->name, EFFECT_NAME_UNSAFE) == 0 &&
                    expr_has_inline_c(fd->body)) {
                    suppress = true;
                }
                if (!suppress) {
                    char who[256];
                    diag_emit_with_code(DIAG_WARNING, fn_span,
                        TUR_W0031_EFFECT_OVER_ANNOTATED,
                        "%s declares effect '%s' but never performs it",
                        binding_fn_describe(fd->binding, who, sizeof(who)),
                        eff->name->name);
                }
            }
        }
    }
    return 0;
}

/* The level of a --strict-effects lint: a warning, or an error under
 * -Werror=strict-effects. */
static DiagLevel strict_effects_level(void) {
    return g_werror_strict_effects ? DIAG_ERROR : DIAG_WARNING;
}

/* ER2: Under --strict-effects, warn when a function declares a row variable
 * (e.g., #{e}) but the inferred row is always a concrete effect set.
 * Suggests replacing the variable with the concrete row. */
static int effect_row_check_var_always_concrete(FnDef *fd, Arena *a) {
    (void)a;
    if (!fd) return 0;
    EffectRow *declared = fd->binding ? fd->binding->type.as.fn.effect_row : NULL;
    if (!declared || declared->kind != ERK_VAR) return 0;
    /* Capability tags (IO from a `println`, FS, Bt) do not count: they say
     * what the body may touch, not what flows through the row variable, so a
     * row-polymorphic function that also prints is not "always concrete". */
    EffectRow *inferred = effect_row_without_capabilities(a, fd->inferred_effect_row);
    if (!inferred || inferred->kind != ERK_CONCRETE) return 0;
    if (inferred->as.concrete.n_effects == 0) return 0;

    Span fn_span = fd->binding ? fd->binding->span : (Span){0, 0, 0, 0, 0, 0};
    Buf inferred_str;
    buf_init(&inferred_str);
    effect_row_print(&inferred_str, inferred);
    char who[256];
    diag_emit_with_code(strict_effects_level(), fn_span,
        TUR_W0032_ROW_VAR_ALWAYS_CONCRETE,
        "%s declares row variable '#fx{%s}' but always performs "
        "concrete effects #fx%.*s; consider replacing '#fx{%s}' with '#fx%.*s'",
        binding_fn_describe(fd->binding, who, sizeof(who)),
        declared->as.var.var_name->name,
        (int)inferred_str.len, inferred_str.data,
        declared->as.var.var_name->name,
        (int)inferred_str.len, inferred_str.data);
    buf_free(&inferred_str);
    return g_werror_strict_effects ? 1 : 0;
}

/* ER1: --strict-effects: TUR-W0030 on a function that performs effects but
 * declares no row.  The one place this is emitted -- --lint-effects used to
 * carry a byte-identical copy, which is how the two flags came to be the same
 * flag under two names (docs/archive/strict-effects-and-lint-effects-are-
 * indistinguishable.md).  Returns 1 when -Werror=strict-effects made it an
 * error. */
static int effect_row_check_unannotated(FnDef *fd, EffectRow *declared,
                                        EffectRow *inferred) {
    if (!fd || declared || !inferred || effect_row_is_empty(inferred)) return 0;
    /* A rank-2 forwarding wrapper performs exactly what it wraps, and the
     * wrapped function gets its own TUR-W0030 (or is annotated); the wrapper
     * itself has no source form to annotate. */
    if (fd->binding && fd->binding->synth_kind == SYNTH_FORWARDING_WRAPPER) return 0;
    Span fn_span = fd->binding ? fd->binding->span : SPAN_UNKNOWN;
    Buf inferred_str;
    buf_init(&inferred_str);
    effect_row_print(&inferred_str, inferred);
    char who[256];
    /* An instance method's row, and a class default body's, is the class
     * method's, so that is where the annotation goes; everything else takes
     * it after its parameter vector. */
    bool from_class = fd->binding &&
                      (fd->binding->is_instance_method ||
                       fd->binding->synth_kind == SYNTH_DEFAULT_METHOD);
    diag_emit_with_code(strict_effects_level(), fn_span,
        TUR_W0030_STRICT_EFFECTS_UNANNOTATED,
        "%s performs effects %.*s but has no effect-row annotation "
        "(add #fx%.*s %s, or handle those effects inside it)",
        binding_fn_describe(fd->binding, who, sizeof(who)),
        (int)inferred_str.len, inferred_str.data,
        (int)inferred_str.len, inferred_str.data,
        from_class ? "to the method's signature in its defclass"
                   : "after its parameter vector");
    buf_free(&inferred_str);
    return g_werror_strict_effects ? 1 : 0;
}

/* ER1: Recursively find EX_CLOSURE nodes with declared effect rows and check them.
 * Collect each closure body's effects and validate against its declared row. */
static int check_closures_in_expr(Arena *a, Expr *e,
                                   const FnIndex *idx,
                                   EffectEnv *env) {
    if (!e) return 0;
    int rc = 0;

    switch (e->kind) {
    case EX_CLOSURE: {
        FnDef *fd = e->as.closure_.closure->fn;
        EffectRow *declared = fd->binding
                                ? fd->binding->type.as.fn.effect_row
                                : NULL;
        /* Only check closures with an explicit annotation that has been resolved. */
        if (declared && declared->kind != ERK_UNRESOLVED) {
            EffectRowSubst *subst = effect_row_subst_new(a);
            EffectRow *body_row = collect_effects_in_expr(
                a, fd->body, effect_row_empty(a), idx, env, subst);
            fd->inferred_effect_row = body_row;
            rc |= effect_row_check_declared(fd, a);
        }
        /* Recurse into the closure body to catch nested closures. */
        rc |= check_closures_in_expr(a, fd->body, idx, env);
        return rc;
    }

    case EX_LET:
        for (uint32_t i = 0; i < e->as.let_.n; i++)
            rc |= check_closures_in_expr(a, e->as.let_.bindings[i].init, idx, env);
        return rc | check_closures_in_expr(a, e->as.let_.body, idx, env);

    case EX_DO:
        for (uint32_t i = 0; i < e->as.do_.n; i++)
            rc |= check_closures_in_expr(a, e->as.do_.items[i], idx, env);
        return rc;

    case EX_IF:
        return check_closures_in_expr(a, e->as.if_.cond, idx, env)
             | check_closures_in_expr(a, e->as.if_.then_, idx, env)
             | check_closures_in_expr(a, e->as.if_.else_or_null, idx, env);

    case EX_WHILE:
        return check_closures_in_expr(a, e->as.while_.cond, idx, env)
             | check_closures_in_expr(a, e->as.while_.body, idx, env);

    case EX_CALL: {
        Expr *fn_expr = e->as.call_.fn_expr;
        rc |= check_closures_in_expr(a, fn_expr, idx, env);
        for (uint32_t i = 0; i < e->as.call_.n_args; i++)
            rc |= check_closures_in_expr(a, e->as.call_.args[i], idx, env);
        return rc;
    }

    case EX_DYN_OP:
    case EX_DYN_CALL:
    case EX_DYN_FIELD:
    case EX_DYN_METHOD:
    case EX_UNION_INJECT:
    case EX_ANY_CAST:
    case EX_ANY_IS:
    case EX_ANY_TYPE_OF:
        for (uint32_t i = 0; i < saffron_dyn_child_count(e); i++)
            rc |= check_closures_in_expr(a, saffron_dyn_child(e, i), idx, env);
        return rc;

    case EX_SET:
        return check_closures_in_expr(a, e->as.set_.value, idx, env);

    case EX_DEF:
        return check_closures_in_expr(a, e->as.def_.init, idx, env);

    case EX_RETURN:
        return check_closures_in_expr(a, e->as.return_.value, idx, env);

    case EX_HANDLE: {
        HandleExpr *h = e->as.handle_.handle;
        rc |= check_closures_in_expr(a, h->body, idx, env);
        for (uint8_t i = 0; i < h->n_cases; i++)
            rc |= check_closures_in_expr(a, h->cases[i].body, idx, env);
        return rc;
    }

    case EX_HANDLER_LIT: {
        HandleExpr *h = e->as.handler_lit_.handle;
        for (uint8_t i = 0; i < h->n_cases; i++)
            rc |= check_closures_in_expr(a, h->cases[i].body, idx, env);
        return rc;
    }
    case EX_WITH_HANDLER:
        rc |= check_closures_in_expr(a, e->as.with_handler_.handler, idx, env);
        rc |= check_closures_in_expr(a, e->as.with_handler_.body, idx, env);
        return rc;
    case EX_COMPOSE_HANDLERS:
        rc |= check_closures_in_expr(a, e->as.compose_handlers_.h1, idx, env);
        rc |= check_closures_in_expr(a, e->as.compose_handlers_.h2, idx, env);
        return rc;

    case EX_BUILTIN:
        for (uint32_t i = 0; i < e->as.builtin.n; i++)
            rc |= check_closures_in_expr(a, e->as.builtin.args[i], idx, env);
        return rc;

    case EX_MAKE_STRUCT:
        for (uint32_t i = 0; i < e->as.make_struct_.n_fields; i++)
            rc |= check_closures_in_expr(a, e->as.make_struct_.field_values[i], idx, env);
        return rc;

    case EX_DEFER:
        return check_closures_in_expr(a, e->as.defer_.body, idx, env);

    case EX_RESET:
        return check_closures_in_expr(a, e->as.reset_.body, idx, env);

    case EX_SHIFT:
        return check_closures_in_expr(a, e->as.shift_.k_fn, idx, env)
             | check_closures_in_expr(a, e->as.shift_.body, idx, env);

    case EX_SHIFT0:
        return check_closures_in_expr(a, e->as.shift0_.k_fn, idx, env)
             | check_closures_in_expr(a, e->as.shift0_.body, idx, env);

    case EX_FN_DEF:
        /* Inner defn: processed separately at the top-level. */
        return 0;

    case EX_ASCRIBE:
        /* Ascription is erased at codegen; descend into the inner expression. */
        return check_closures_in_expr(a, e->as.ascribe_.inner, idx, env);

    default:
        return 0;
    }
}

/* ER4: Walk an expression tree and check function-argument effect-row subtyping.
 * At each EX_CALL, for each function-typed parameter with a concrete declared
 * row, verify that the actual argument's inferred row is a subset.  Emits
 * TUR-E0009 on violation. */
static int check_call_site_rows_in_expr(Arena *a, Expr *e,
                                         const FnIndex *idx,
                                         EffectEnv *env) {
    if (!e) return 0;
    int rc = 0;

    switch (e->kind) {
    case EX_CALL: {
        /* First, check the call site itself. */
        FnDef *callee = fn_index_lookup(idx, e->as.call_.fn_binding);
        if (callee && callee->params && callee->n_params > 0 &&
            e->as.call_.n_args > 0) {
            uint32_t n_check = (callee->n_params < (uint8_t)e->as.call_.n_args)
                                ? callee->n_params : (uint8_t)e->as.call_.n_args;
            for (uint8_t pi = 0; pi < n_check; pi++) {
                Binding *param = callee->params[pi];
                if (!param || param->type.kind != TY_FN) continue;
                EffectRow *param_row = param->type.as.fn.effect_row;
                Expr *actual = e->as.call_.args[pi];
                /* Only check concrete declared rows (not row variables). */
                if (!param_row ||
                    (param_row->kind != ERK_CONCRETE &&
                     param_row->kind != ERK_EMPTY)) continue;

                /* Determine actual argument's inferred effect row.  Peel the
                 * fat-normalization shims first (EX_FN_TO_FAT wraps a bare-fn
                 * or lambda argument into a fat handle; ascription is erased
                 * at codegen): the row belongs to the wrapped value, and
                 * matching the wrapper here is how the five errors/effect-*
                 * negatives silently stopped diagnosing when the effect-row
                 * exclusion was probe-lifted (2026-08-16 re-measurement). */
                while (actual &&
                       (actual->kind == EX_ASCRIBE ||
                        actual->kind == EX_FN_TO_FAT))
                    actual = (actual->kind == EX_ASCRIBE)
                                 ? actual->as.ascribe_.inner
                                 : actual->as.fn_to_fat_.inner;
                EffectRow *actual_row = NULL;
                if (actual && actual->kind == EX_CLOSURE) {
                    EffectRowSubst *arg_subst = effect_row_subst_new(a);
                    actual_row = collect_effects_in_expr(
                        a, actual->as.closure_.closure->fn->body,
                        effect_row_empty(a), idx, env, arg_subst);
                } else if (actual && actual->kind == EX_VAR &&
                           actual->as.var.binding) {
                    FnDef *arg_fn = fn_index_lookup(idx, actual->as.var.binding);
                    if (arg_fn) {
                        actual_row = arg_fn->inferred_effect_row;
                    } else if (actual->as.var.binding->type.kind == TY_FN) {
                        actual_row = actual->as.var.binding->type.as.fn.effect_row;
                    }
                }

                if (actual_row && !effect_row_is_empty(actual_row) &&
                    !effect_row_is_subset(actual_row, param_row)) {
                    Buf actual_str, param_str;
                    buf_init(&actual_str);
                    buf_init(&param_str);
                    effect_row_print(&actual_str, actual_row);
                    effect_row_print(&param_str, param_row);
                    diag_emit_with_code(DIAG_ERROR, e->span,
                        TUR_E0009_EFFECT_ROW_MISMATCH,
                        "effect-row subtype violation: argument's row %.*s is not a "
                        "subset of parameter's declared row %.*s",
                        (int)actual_str.len, actual_str.data,
                        (int)param_str.len, param_str.data);
                    buf_free(&actual_str);
                    buf_free(&param_str);
                    rc = 1;
                }
            }
        }
        /* Recurse into all sub-expressions. */
        if (e->as.call_.fn_expr)
            rc |= check_call_site_rows_in_expr(a, e->as.call_.fn_expr, idx, env);
        for (uint32_t i = 0; i < e->as.call_.n_args; i++)
            rc |= check_call_site_rows_in_expr(a, e->as.call_.args[i], idx, env);
        return rc;
    }

    case EX_DYN_OP:
    case EX_DYN_CALL:
    case EX_DYN_FIELD:
    case EX_DYN_METHOD:
    case EX_UNION_INJECT:
    case EX_ANY_CAST:
    case EX_ANY_IS:
    case EX_ANY_TYPE_OF: {
        /* An ER4-checkable call reachable only through a dynamic node was
         * never reached, so its row subtyping went unchecked. */
        for (uint32_t i = 0; i < saffron_dyn_child_count(e); i++)
            rc |= check_call_site_rows_in_expr(a, saffron_dyn_child(e, i), idx, env);
        return rc;
    }

    case EX_LET:
        for (uint32_t i = 0; i < e->as.let_.n; i++)
            rc |= check_call_site_rows_in_expr(a, e->as.let_.bindings[i].init, idx, env);
        return rc | check_call_site_rows_in_expr(a, e->as.let_.body, idx, env);

    case EX_DO:
        for (uint32_t i = 0; i < e->as.do_.n; i++)
            rc |= check_call_site_rows_in_expr(a, e->as.do_.items[i], idx, env);
        return rc;

    case EX_IF:
        return check_call_site_rows_in_expr(a, e->as.if_.cond, idx, env)
             | check_call_site_rows_in_expr(a, e->as.if_.then_, idx, env)
             | check_call_site_rows_in_expr(a, e->as.if_.else_or_null, idx, env);

    case EX_WHILE:
        return check_call_site_rows_in_expr(a, e->as.while_.cond, idx, env)
             | check_call_site_rows_in_expr(a, e->as.while_.body, idx, env);

    case EX_SET:
        return check_call_site_rows_in_expr(a, e->as.set_.value, idx, env);

    case EX_DEF:
        return check_call_site_rows_in_expr(a, e->as.def_.init, idx, env);

    case EX_RETURN:
        return check_call_site_rows_in_expr(a, e->as.return_.value, idx, env);

    case EX_HANDLE: {
        HandleExpr *h = e->as.handle_.handle;
        rc |= check_call_site_rows_in_expr(a, h->body, idx, env);
        for (uint8_t i = 0; i < h->n_cases; i++)
            rc |= check_call_site_rows_in_expr(a, h->cases[i].body, idx, env);
        return rc;
    }

    case EX_HANDLER_LIT: {
        HandleExpr *h = e->as.handler_lit_.handle;
        for (uint8_t i = 0; i < h->n_cases; i++)
            rc |= check_call_site_rows_in_expr(a, h->cases[i].body, idx, env);
        return rc;
    }
    case EX_WITH_HANDLER:
        rc |= check_call_site_rows_in_expr(a, e->as.with_handler_.handler, idx, env);
        rc |= check_call_site_rows_in_expr(a, e->as.with_handler_.body, idx, env);
        return rc;
    case EX_COMPOSE_HANDLERS:
        rc |= check_call_site_rows_in_expr(a, e->as.compose_handlers_.h1, idx, env);
        rc |= check_call_site_rows_in_expr(a, e->as.compose_handlers_.h2, idx, env);
        return rc;

    case EX_BUILTIN:
        for (uint32_t i = 0; i < e->as.builtin.n; i++)
            rc |= check_call_site_rows_in_expr(a, e->as.builtin.args[i], idx, env);
        return rc;

    case EX_MAKE_STRUCT:
        for (uint32_t i = 0; i < e->as.make_struct_.n_fields; i++)
            rc |= check_call_site_rows_in_expr(a, e->as.make_struct_.field_values[i], idx, env);
        return rc;

    case EX_CLOSURE:
        return check_call_site_rows_in_expr(a, e->as.closure_.closure->fn->body, idx, env);

    case EX_DEFER:
        return check_call_site_rows_in_expr(a, e->as.defer_.body, idx, env);

    case EX_RESET:
        return check_call_site_rows_in_expr(a, e->as.reset_.body, idx, env);

    case EX_SHIFT:
        return check_call_site_rows_in_expr(a, e->as.shift_.k_fn, idx, env)
             | check_call_site_rows_in_expr(a, e->as.shift_.body, idx, env);

    case EX_FN_DEF:
        return 0;

    case EX_ASCRIBE:
        /* Ascription is erased at codegen; descend into the inner expression. */
        return check_call_site_rows_in_expr(a, e->as.ascribe_.inner, idx, env);

    default:
        return 0;
    }
}

typedef struct { Arena *a; FnIndex *idx; EffectEnv *env; } UnreachUd;
static bool unreach_visit(const Expr *c, void *ud);

/* ---------------------------------------------------------------------------
 * ET1-C: check_unreachable_handlers_in_expr
 * Walk an expression tree; for each EX_HANDLE, recompute the body's inferred
 * effect row and emit TUR-W0033 for any handler clause whose effect is absent
 * from that row (the clause can never be triggered).
 * --------------------------------------------------------------------------- */

static void check_unreachable_handlers_in_expr(
        Arena *a, Expr *e, FnIndex *idx, EffectEnv *env) {
    if (!e) return;
    switch (e->kind) {
    case EX_HANDLE: {
        HandleExpr *h = e->as.handle_.handle;
        /* unsafe-block-w0033-on-raw-builtins (docs/archive/): the `(unsafe
         * ...)` desugar's Unsafe clause is compiler-generated and the block
         * is MANDATORY for the gated raw builtins, which by design never
         * perform the Unsafe effect (it is a compile-time marker; see the
         * is_unsafe_marker comment in expr.h).  So "this clause is
         * unreachable" was both always true and never actionable --
         * contradictory advice on a required form.  Skip the clause check;
         * a USER-written handle for a never-performed effect still warns
         * (errors/effect-handle-unreachable pins that), and nested user
         * handles inside the unsafe body are still swept below. */
        if (h->is_unsafe_marker) {
            check_unreachable_handlers_in_expr(a, h->body, idx, env);
            for (uint8_t i = 0; i < h->n_cases; i++)
                check_unreachable_handlers_in_expr(a, h->cases[i].body, idx, env);
            return;
        }
        EffectRowSubst *subst = effect_row_subst_new(a);
        /* What the body PERFORMS: a capability tag in its row (IO from a
         * `println`) is never performed, and since IO is the parent of
         * Write/Read, leaving it in would make every Write clause over a body
         * that merely prints look reachable. */
        EffectRow *body_row = effect_row_without_capabilities(a,
            collect_effects_in_expr(a, h->body, effect_row_empty(a), idx, env, subst));
        for (uint8_t i = 0; i < h->n_cases; i++) {
            const Symbol *eff_name = h->cases[i].effect_name;
            if (!eff_name) continue;
            Effect *eff = effect_env_lookup(env, eff_name);
            if (!eff) continue;
            if (!effect_row_contains(body_row, eff)) {
                Span span = h->cases[i].body
                            ? h->cases[i].body->span
                            : (Span){0, 0, 0, 0, 0, 0};
                diag_emit_with_code(DIAG_WARNING, span,
                    TUR_W0033_UNREACHABLE_HANDLER,
                    "handler clause for '%s' is unreachable: "
                    "the body does not perform '%s'",
                    eff_name->name, eff_name->name);
            }
        }
        check_unreachable_handlers_in_expr(a, h->body, idx, env);
        for (uint8_t i = 0; i < h->n_cases; i++)
            check_unreachable_handlers_in_expr(a, h->cases[i].body, idx, env);
        return;
    }
    case EX_LET:
        for (uint32_t i = 0; i < e->as.let_.n; i++)
            check_unreachable_handlers_in_expr(
                a, e->as.let_.bindings[i].init, idx, env);
        check_unreachable_handlers_in_expr(a, e->as.let_.body, idx, env);
        return;
    case EX_DO:
        for (uint32_t i = 0; i < e->as.do_.n; i++)
            check_unreachable_handlers_in_expr(a, e->as.do_.items[i], idx, env);
        return;
    case EX_IF:
        check_unreachable_handlers_in_expr(a, e->as.if_.cond, idx, env);
        check_unreachable_handlers_in_expr(a, e->as.if_.then_, idx, env);
        check_unreachable_handlers_in_expr(a, e->as.if_.else_or_null, idx, env);
        return;
    case EX_WHILE:
        check_unreachable_handlers_in_expr(a, e->as.while_.cond, idx, env);
        check_unreachable_handlers_in_expr(a, e->as.while_.body, idx, env);
        return;
    case EX_CALL:
        check_unreachable_handlers_in_expr(a, e->as.call_.fn_expr, idx, env);
        for (uint32_t i = 0; i < e->as.call_.n_args; i++)
            check_unreachable_handlers_in_expr(a, e->as.call_.args[i], idx, env);
        return;
    case EX_DYN_OP:
    case EX_DYN_CALL:
    case EX_DYN_FIELD:
    case EX_DYN_METHOD:
    case EX_UNION_INJECT:
    case EX_ANY_CAST:
    case EX_ANY_IS:
    case EX_ANY_TYPE_OF:
        /* A nested `handle` reachable only through a dynamic node was never
         * visited, so its own clauses went unchecked. */
        for (uint32_t i = 0; i < saffron_dyn_child_count(e); i++)
            check_unreachable_handlers_in_expr(a, saffron_dyn_child(e, i), idx, env);
        return;
    case EX_CLOSURE:
        check_unreachable_handlers_in_expr(
            a, e->as.closure_.closure->fn->body, idx, env);
        return;
    case EX_SET:
        check_unreachable_handlers_in_expr(a, e->as.set_.value, idx, env);
        return;
    case EX_RETURN:
        check_unreachable_handlers_in_expr(a, e->as.return_.value, idx, env);
        return;
    case EX_ASCRIBE:
        /* Ascription is erased at codegen; descend so a nested handle inside an
         * ascription is still checked. */
        check_unreachable_handlers_in_expr(a, e->as.ascribe_.inner, idx, env);
        return;
    default: {
        /* Every other node: descend through the shared operand enumeration,
         * so a `handle` inside a builtin's argument (`(println (handle
         * ...))`), a match arm, or any node without an arm above is still
         * checked.  This walk used to stop here, which is why the same
         * unreachable clause warned when let-bound and not when printed. */
        UnreachUd u = { a, idx, env };
        cps_visit_children(e, unreach_visit, &u);
        return;
    }
    }
}

static bool unreach_visit(const Expr *c, void *ud) {
    UnreachUd *u = (UnreachUd *)ud;
    check_unreachable_handlers_in_expr(u->a, (Expr *)c, u->idx, u->env);
    return false;
}

/* ---------------------------------------------------------------------------
 * TUR-E0026: an effect name no defeffect declares.
 *
 * effect_row_resolve drops a name it cannot find -- there is nothing to
 * resolve it to -- and for years that was the whole story: `#fx{IO}` in a
 * file that never loaded stdlib/effects.tur checked as `#fx{}`, and a
 * caller's `#fx{}` then passed a TUR-E0009 check it should have failed.
 * Every declared row now comes through resolve_declared_row, which reports
 * the names first.  (docs/archive/capability-effect-tag-silently-resolves-
 * to-empty-row.md)
 * --------------------------------------------------------------------------- */

/* The effects stdlib/effects.tur declares that the compiler does not know
 * already (IO / FS / Net / Proc / Rand are compiler-known since W2, so they
 * are never unknown).  That module is not autoloaded, so these are the names
 * most likely to be written before it is loaded, and for them the hint can
 * say exactly what to load.  A name missing from this list still gets the
 * general hint; it does not change what is an error. */
static const char *const k_effects_tur_names[] = {
    "Write", "Fail", "Read", "GetEnv", "Log", "Abort", "Async", "Await",
};

static bool is_effects_tur_name(const Symbol *name) {
    for (size_t i = 0; i < sizeof(k_effects_tur_names) / sizeof(k_effects_tur_names[0]); i++)
        if (strcmp(name->name, k_effects_tur_names[i]) == 0) return true;
    return false;
}

/* The compiler attributes that used to be written inside `#fx{...}`
 * (effect-row-honesty-plan W0) and their spellings now.  Left in a row they
 * are TUR-E0026 like any undeclared name; this only makes the message say
 * where they went. */
static const char *moved_attr_spelling(const Symbol *name) {
    if (strcmp(name->name, "Construct") == 0)     return "^construct";
    if (strcmp(name->name, "ByVal") == 0)         return "^byval";
    if (strcmp(name->name, "NonExhaustive") == 0) return "^non-exhaustive";
    return NULL;
}

static const char *moved_attr_example(const Symbol *name) {
    if (strcmp(name->name, "NonExhaustive") == 0)
        return "(match ^non-exhaustive x ...)";
    if (strcmp(name->name, "ByVal") == 0)
        return "(defn ^byval name [A] [...] : A ...)";
    return "(defn ^construct name [A] [...] : (Option A) ...)";
}

/* The declared effect closest to `name` by edit distance, for a typo hint;
 * NULL when nothing is close enough to be a plausible misspelling. */
static const Symbol *closest_declared_effect(const Symbol *name, EffectEnv *env) {
    const Symbol *best = NULL;
    int best_d = 3;   /* accept distance <= 2 */
    for (uint32_t i = 0; i < env->n_effects; i++) {
        const Effect *eff = env->effects[i];
        if (!eff || !eff->name) continue;
        int d = sym_levenshtein_distance(name, eff->name);
        if (d < best_d && d < (int)name->len) { best_d = d; best = eff->name; }
    }
    return best;
}

/* Report the unknown effect names in `row` (once per row object), then
 * resolve it.  `fallback` is used when the row carries no span of its own.
 * Sets *rc to 1 if anything was reported. */
static EffectRow *resolve_declared_row(EffectRow *row, EffectEnv *env, Arena *a,
                                       Span fallback, int *rc) {
    if (!row || row->kind != ERK_UNRESOLVED) return row;
    if (!row->as.unresolved.unknown_reported) {
        row->as.unresolved.unknown_reported = true;
        const Symbol *unknown[16];
        uint8_t n = effect_row_unknown_names(row, env, unknown, 16);
        if (n > 16) n = 16;
        Span span = row->as.unresolved.span;
        if (span.line == 0) span = fallback;
        for (uint8_t i = 0; i < n; i++) {
            const Symbol *name = unknown[i];
            const Symbol *near = closest_declared_effect(name, env);
            if (!(name->name[0] >= 'A' && name->name[0] <= 'Z')) {
                /* `#fx{|e}` (Koka's row-tail spelling) and other names that
                 * start with neither case of letter. */
                diag_emit_with_code(DIAG_ERROR, span, TUR_E0026_UNKNOWN_EFFECT_IN_ROW,
                    "'%s' in effect row is neither an effect nor a row variable "
                    "(an effect name starts with an uppercase letter, a row "
                    "variable with a lowercase one: #fx{IO e})",
                    name->name);
            } else if (moved_attr_spelling(name)) {
                /* effect-row-honesty-plan W0: an attribute that used to
                 * borrow the row brackets. */
                diag_emit_with_code(DIAG_ERROR, span, TUR_E0026_UNKNOWN_EFFECT_IN_ROW,
                    "'%s' in effect row is not an effect: it is the attribute %s "
                    "now, e.g. %s",
                    name->name, moved_attr_spelling(name), moved_attr_example(name));
            } else if (is_effects_tur_name(name)) {
                diag_emit_with_code(DIAG_ERROR, span, TUR_E0026_UNKNOWN_EFFECT_IN_ROW,
                    "unknown effect '%s' in effect row: no defeffect declares it "
                    "('%s' is declared in stdlib/effects.tur, which is not "
                    "autoloaded; add (load \"stdlib/effects.tur\"))",
                    name->name, name->name);
            } else if (near) {
                diag_emit_with_code(DIAG_ERROR, span, TUR_E0026_UNKNOWN_EFFECT_IN_ROW,
                    "unknown effect '%s' in effect row: no defeffect declares it "
                    "(did you mean '%s'?)",
                    name->name, near->name);
            } else {
                diag_emit_with_code(DIAG_ERROR, span, TUR_E0026_UNKNOWN_EFFECT_IN_ROW,
                    "unknown effect '%s' in effect row: no defeffect declares it "
                    "(did you load the module that declares it?)",
                    name->name);
            }
            *rc = 1;
        }
    }
    return effect_row_resolve(row, env, a);
}

/* ---------------------------------------------------------------------------
 * The items the pass sees: every top-level item, with `(defmodule ...)` bodies
 * and top-level `(do ...)` forms spread in place, recursively -- the same
 * flattening the emitter does (emit_core.c, flatten_program_items).  A module
 * member's EX_FN_DEF lives in EX_DEFMODULE's `mod->body`, and a macro's
 * `(do (defn ...) (defn ...))` keeps its defns inside the EX_DO; a walk over
 * `program->as.program.items` alone never resolved, inferred or checked
 * either, so a member's `#fx{}` was a promise nothing read.  The array is
 * malloc'd; the caller frees it.
 * --------------------------------------------------------------------------- */

static void effect_items_push(Expr *e, Expr ***arr, uint32_t *n, uint32_t *cap) {
    if (!e) return;
    if (e->kind == EX_DEFMODULE && e->as.defmodule_.mod) {
        DefModule *mod = e->as.defmodule_.mod;
        for (uint32_t j = 0; j < mod->n_body; j++)
            effect_items_push(mod->body[j], arr, n, cap);
        return;
    }
    if (e->kind == EX_DO) {
        for (uint32_t j = 0; j < e->as.do_.n; j++)
            effect_items_push(e->as.do_.items[j], arr, n, cap);
        return;
    }
    if (*n == *cap) {
        *cap = *cap ? *cap * 2 : 64;
        Expr **grown = realloc(*arr, *cap * sizeof(Expr *));
        if (!grown) { fprintf(stderr, "tur: oom\n"); abort(); }
        *arr = grown;
    }
    (*arr)[(*n)++] = e;
}

static Expr **effect_check_items(Expr *program, uint32_t *out_n) {
    Expr **arr = NULL;
    uint32_t n = 0, cap = 0;
    for (uint32_t i = 0; i < program->as.program.n; i++)
        effect_items_push(program->as.program.items[i], &arr, &n, &cap);
    *out_n = n;
    return arr;
}

/* ---------------------------------------------------------------------------
 * effect_check_pass
 * --------------------------------------------------------------------------- */

int effect_check_pass(Arena *a, Expr *program, EffectEnv *env) {
    if (!program || program->kind != EX_PROGRAM) return 0;
    if (!env) return 0;

    uint32_t n_items = 0;
    Expr **items = effect_check_items(program, &n_items);

    /* --- Step -1: Populate the effect env from EX_DEFECT nodes.
     * PASS_EFFECT_LOWER creates a fresh empty env; we scan the elaborated AST
     * for (defeffect ...) nodes and register each one so the env is populated
     * before ERK_UNRESOLVED resolution and inference run. --- */
    for (uint32_t i = 0; i < n_items; i++) {
        Expr *item = items[i];
        if (!item || item->kind != EX_DEFECT || !item->as.effect_def_.def) continue;
        EffectDef *def = item->as.effect_def_.def;
        /* Re-registering takes the node's declaration.  In one program that is
         * the same declaration again; in a long-lived interpreter env, whose
         * EffectEnv outlives each turn, it is a later turn redefining the
         * effect (PS4), and the older signature must not stay behind. */
        Effect *have = effect_env_lookup(env, def->name);
        if (have) {
            effect_redefine(have, def->param_names, def->param_types,
                            def->n_params, def->result_type,
                            def->defining_module_name, def->is_private);
        } else {
            effect_env_register(env, a, def->name,
                                def->param_names, def->param_types,
                                def->n_params, def->result_type,
                                def->defining_module_name, def->is_private);
        }
    }
    /* ET4: Second pass to set parent pointers from ^extends declarations.
     * Must happen after all effects are registered so parent lookup works. */
    for (uint32_t i = 0; i < n_items; i++) {
        Expr *item = items[i];
        if (!item || item->kind != EX_DEFECT || !item->as.effect_def_.def) continue;
        EffectDef *def = item->as.effect_def_.def;
        if (!def->parent_name) continue;
        Effect *child_eff = effect_env_lookup(env, def->name);
        Effect *parent_eff = effect_env_lookup(env, def->parent_name);
        if (child_eff && parent_eff) {
            child_eff->parent = parent_eff;
        }
    }
    /* stdlib-effect-rows: propagate the ^capability flag onto each registered
     * effect.  (The fresh env populated above does not carry it through
     * effect_env_register, which predates the flag.) */
    for (uint32_t i = 0; i < n_items; i++) {
        Expr *item = items[i];
        if (!item || item->kind != EX_DEFECT || !item->as.effect_def_.def) continue;
        EffectDef *def = item->as.effect_def_.def;
        if (!def->is_capability) continue;
        Effect *eff = effect_env_lookup(env, def->name);
        if (eff) eff->is_capability = true;
    }

    /* --- Step 0: Resolve ERK_UNRESOLVED declared effect rows.
     * Effect row annotations are parsed during elaboration as ERK_UNRESOLVED
     * (symbolic names).  Now that PASS_EFFECT_LOWER has populated the effect
     * environment, we can resolve each annotation to ERK_CONCRETE/ERK_VAR.
     * Every resolution goes through resolve_declared_row, which reports a
     * name no defeffect declares (TUR-E0026) instead of dropping it. --- */
    int rc = 0;
    for (uint32_t i = 0; i < n_items; i++) {
        Expr *item = items[i];
        if (!item || item->kind != EX_FN_DEF || !item->as.fn_def_.fn) continue;
        FnDef *fn = item->as.fn_def_.fn;
        if (fn->binding && fn->binding->type.as.fn.effect_row) {
            EffectRow *decl = fn->binding->type.as.fn.effect_row;
            if (decl->kind == ERK_UNRESOLVED) {
                fn->binding->type.as.fn.effect_row =
                    resolve_declared_row(decl, env, a, fn->binding->span, &rc);
            }
        }
        /* ER2: Also resolve effect rows on TY_FN parameter types.
         * When a parameter is annotated with :(fn [] #{e} :T), its type carries
         * an ERK_UNRESOLVED effect row that must be resolved here so that the
         * row-variable unification in collect_effects_in_expr can see ERK_VAR. */
        for (uint32_t pi = 0; pi < fn->n_params; pi++) {
            Binding *param = fn->params[pi];
            if (!param || param->type.kind != TY_FN) continue;
            EffectRow *pr = param->type.as.fn.effect_row;
            if (pr && pr->kind == ERK_UNRESOLVED) {
                param->type.as.fn.effect_row =
                    resolve_declared_row(pr, env, a, param->span, &rc);
            }
        }
    }

    /* --- Step 0a2: Resolve ERK_UNRESOLVED effect rows on record-ADT capability
     * fields (`[run : fn #fx{Eff}]`).  elab_structs.c parses the trailing
     * `#fx{...}` on a fn-typed field as ERK_UNRESOLVED (the effect env is not yet
     * built at elaboration time); resolve it IN PLACE on the CtorField now that
     * `env` is populated.  Without this the row stays symbolic, so
     * `effect_row_contains` (which returns false for ERK_UNRESOLVED) never sees
     * the effect: a `(.run v)` capability call is dropped from the enclosing
     * body's inferred row (spurious TUR-W0033) and the CPS coloring cannot match
     * the field call against the handler.  Both effect_check and the CPS pass
     * (cps_ir.c expr_fn_effect_row) read the same CtorField.effect_row, so an
     * in-place resolve fixes both. */
    for (uint32_t i = 0; i < n_items; i++) {
        Expr *item = items[i];
        if (!item) continue;
        AdtDef *adt = NULL;
        if (item->kind == EX_DEFDATA) adt = item->as.defdata_.def;
        else if (item->kind == EX_DEFGADT) adt = item->as.defgadt_.def;
        if (!adt) continue;
        for (uint32_t ci = 0; ci < adt->n_ctors; ci++) {
            CtorDef *ctor = adt->ctors[ci];
            if (!ctor) continue;
            for (uint32_t fi = 0; fi < ctor->n_fields; fi++) {
                EffectRow *fr = ctor->fields[fi].effect_row;
                if (fr && fr->kind == ERK_UNRESOLVED)
                    ctor->fields[fi].effect_row =
                        resolve_declared_row(fr, env, a, item->span, &rc);
            }
        }
    }

    /* --- Step 0b: Resolve typeclass method effect rows + update instance method bindings. ---
     * Pass 1: Resolve all ERK_UNRESOLVED effect rows on typeclass method signatures. */
    for (uint32_t i = 0; i < n_items; i++) {
        Expr *item = items[i];
        if (!item || item->kind != EX_TYPECLASS_DEF) continue;
        TypeClass *tc = item->as.typeclass_def_.typeclass;
        if (!tc) continue;
        for (uint8_t mi = 0; mi < tc->n_methods; mi++) {
            TypeClassMethod *meth = &tc->methods[mi];
            if (meth->effect_row && meth->effect_row->kind == ERK_UNRESOLVED) {
                meth->effect_row = resolve_declared_row(meth->effect_row, env, a,
                                                        item->span, &rc);
            }
        }
    }
    /* Pass 2: Update instance method impl bindings with the resolved method effect rows. */
    for (uint32_t i = 0; i < n_items; i++) {
        Expr *item = items[i];
        if (!item || item->kind != EX_INSTANCE_DEF) continue;
        TypeClassInstance *inst = item->as.instance_def_.instance;
        if (!inst || !inst->typeclass) continue;
        uint8_t n_min = inst->typeclass->n_methods < inst->n_method_impls
                        ? inst->typeclass->n_methods : inst->n_method_impls;
        for (uint8_t mi = 0; mi < n_min; mi++) {
            TypeClassMethod *meth = &inst->typeclass->methods[mi];
            FnDef *impl = inst->method_impls[mi];
            if (!impl || !impl->binding) continue;
            if (meth->effect_row) {
                impl->binding->type.as.fn.effect_row = meth->effect_row;
            }
        }
    }

    /* ER3 Pass 3: Propagate resolved effect_row to default method FnDef bindings.
     * Default method bodies are registered as EX_FN_DEF nodes, but their binding
     * types are set up before typeclass method effect rows are resolved.  Copy
     * the now-resolved effect_row so effect_check_pass validates them correctly. */
    for (uint32_t i = 0; i < n_items; i++) {
        Expr *item = items[i];
        if (!item || item->kind != EX_TYPECLASS_DEF) continue;
        TypeClass *tc = item->as.typeclass_def_.typeclass;
        if (!tc) continue;
        for (uint8_t mi = 0; mi < tc->n_methods; mi++) {
            TypeClassMethod *meth = &tc->methods[mi];
            if (!meth->default_fn_expr || !meth->effect_row) continue;
            FnDef *def_fn = meth->default_fn_expr->as.fn_def_.fn;
            if (!def_fn || !def_fn->binding) continue;
            def_fn->binding->type.as.fn.effect_row = meth->effect_row;
        }
    }

    /* structdef-retirement DS-D: the former Step 0c resolved ERK_UNRESOLVED
     * effect rows off EX_DEF struct fields (def_.struct_def).  A defstruct is now
     * a record ADT and its capability-field effect rows resolve on the CtorField
     * along the defdata path, so this StructDef-keyed pre-pass is dead. */

    /* --- Step 1: Collect every FnDef (top-level and module members) into an index. --- */
    FnIndex idx;
    memset(&idx, 0, sizeof(idx));

    for (uint32_t i = 0; i < n_items; i++) {
        Expr *item = items[i];
        if (item && item->kind == EX_FN_DEF && item->as.fn_def_.fn) {
            FnDef *fn = item->as.fn_def_.fn;
            if (fn->binding) {
                fn_index_add(&idx, fn->binding, fn);
            }
        }
    }

    /* --- Step 2: Fixed-point iteration.
     * On each sweep, recompute every FnDef's inferred row.  Repeat until no
     * row grows (monotone — rows only ever gain effects).               --- */
    bool changed = true;
    while (changed) {
        changed = false;

        for (uint32_t i = 0; i < n_items; i++) {
            Expr *item = items[i];
            if (!item || item->kind != EX_FN_DEF || !item->as.fn_def_.fn) continue;

            FnDef *fn = item->as.fn_def_.fn;

            /* ER5 / PR5-2: set the module context so cross-module private
             * effects are filtered when merging callee rows. */
            s_current_analysis_module =
                fn->binding ? fn->binding->defining_module_name : NULL;

            /* Phase P19-4: create a fresh substitution for each function analysis.
             * This allows row-variable bindings to be computed per-invocation. */
            EffectRowSubst *subst = effect_row_subst_new(a);

            /* Start with the empty row for this iteration. */
            EffectRow *fresh = collect_effects_in_expr(
                a, fn->body, effect_row_empty(a), &idx, env, subst);

            /* Check if the row grew compared to the previous iteration. */
            EffectRow *prev = fn->inferred_effect_row;
            if (!prev || !effect_row_is_subset(fresh, prev) ||
                         !effect_row_is_subset(prev, fresh)) {
                fn->inferred_effect_row = fresh;
                changed = true;
            }
        }
    }

    /* Reset module context after fixed-point so it doesn't affect validation. */
    s_current_analysis_module = NULL;

    /* --- Step 3: Validate each function against its declared row. --- */
    for (uint32_t i = 0; i < n_items; i++) {
        Expr *item = items[i];
        if (!item || item->kind != EX_FN_DEF || !item->as.fn_def_.fn) continue;
        FnDef *fn = item->as.fn_def_.fn;
        EffectRow *declared = fn->binding ? fn->binding->type.as.fn.effect_row : NULL;
        EffectRow *inferred = fn->inferred_effect_row;

        /* ER1: check declared vs inferred (TUR-E0009). */
        if (effect_row_check_declared(fn, a) != 0) rc = 1;

        /* ER1: over-annotation warning (TUR-W0031). */
        effect_row_check_over_annotated(fn, a);

        /* ER1/ER2: --strict-effects lints (TUR-W0030, TUR-W0032). */
        if (g_strict_effects) {
            if (effect_row_check_var_always_concrete(fn, a) != 0) rc = 1;
            if (effect_row_check_unannotated(fn, declared, inferred) != 0) rc = 1;
        }
    }

    /* ER1: Check closures with declared effect rows. */
    for (uint32_t i = 0; i < n_items; i++) {
        Expr *item = items[i];
        if (!item || item->kind != EX_FN_DEF || !item->as.fn_def_.fn) continue;
        if (check_closures_in_expr(a, item->as.fn_def_.fn->body, &idx, env) != 0)
            rc = 1;
    }

    /* --- Step 3b: ER4 -- Call-site effect-row subtype checking.
     * For each top-level function, walk its body and verify that any function
     * value passed to a concrete-row parameter has a compatible (subset) row. */
    for (uint32_t i = 0; i < n_items; i++) {
        Expr *item = items[i];
        if (!item || item->kind != EX_FN_DEF || !item->as.fn_def_.fn) continue;
        if (check_call_site_rows_in_expr(a, item->as.fn_def_.fn->body,
                                          &idx, env) != 0)
            rc = 1;
    }

    /* --- Step 3c: ET1-C -- Unreachable handler clause warnings (TUR-W0033).
     * For each top-level function, walk its body and warn when a handler clause
     * names an effect that the handled body does not actually perform. */
    for (uint32_t i = 0; i < n_items; i++) {
        Expr *item = items[i];
        if (!item || item->kind != EX_FN_DEF || !item->as.fn_def_.fn) continue;
        check_unreachable_handlers_in_expr(a, item->as.fn_def_.fn->body, &idx, env);
    }

    free(idx.slots);
    free(items);
    return rc;
}

/* ER6: --dump-effects — print each top-level defn's inferred effect row.
 * ET4: When a declared annotation is present, prefer it over the inferred row
 * so that row variables (e.g. #{e}) appear in the output for polymorphic defns. */
void effect_check_dump_effects(Expr *program, FILE *out) {
    if (!program || program->kind != EX_PROGRAM) return;
    uint32_t n_items = 0;
    Expr **items = effect_check_items(program, &n_items);
    for (uint32_t i = 0; i < n_items; i++) {
        Expr *item = items[i];
        if (!item || item->kind != EX_FN_DEF || !item->as.fn_def_.fn) continue;
        FnDef *fn = item->as.fn_def_.fn;
        if (!fn->binding || !fn->binding->name) continue;

        /* Prefer the declared annotation; fall back to inferred. */
        EffectRow *declared = NULL;
        if (fn->binding->type.kind == TY_FN)
            declared = fn->binding->type.as.fn.effect_row;

        Buf row_str;
        buf_init(&row_str);
        EffectRow *row = declared ? declared : fn->inferred_effect_row;
        if (!row) {
            buf_puts(&row_str, "#{}");
        } else {
            buf_putc(&row_str, '#');
            effect_row_print(&row_str, row);
        }
        fprintf(out, "defn %s : %.*s\n",
                fn->binding->name->name,
                (int)row_str.len, row_str.data);
        buf_free(&row_str);
    }
    free(items);
}
