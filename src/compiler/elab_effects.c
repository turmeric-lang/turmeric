/* elab_effects.c -- delimited continuations and algebraic effects. */
#include "elab_internal.h"
#include "lang_dialects.h"   /* saffron-perform-argument-skips-the-any-seam */

/* E3a (owning-cloneable-capture, cps-backend-owning-env-teardown): an owning
 * value captured ^borrow into a genuinely multi-shot cloneable continuation that
 * lacks a Clone instance is admitted -- rather than rejected with TUR-E0014 --
 * when the `owning-cloneable-capture` experiment is on AND the owning kind is a
 * ONE-WORD handle the cloneable frame env can carry:
 *   - `rc<T>`   (TY_RC): a reference-counted handle;
 *   - a `:heap` ADT / struct carrier handle (a one-word typed pointer).
 * Both ride the frame env by a bare pointer copy; for a ^borrow capture the
 * frame never drops the handle, so the shallow-shared env is read-only-correct
 * across resumes and the owner drops it once (see the borrow teardown in
 * build_marshal_reset).  An owning BY-VALUE aggregate (multi-word) does not fit
 * the one-word env and is not admitted here -- it needs a boxed / widened env
 * captured by a pointer to the owner's by-value local (see the cloneable emit),
 * so it too is admitted -- it fits the one-word env by ADDRESS. */
static bool owning_byvalue_agg(const Type *t) {
    if (!t) return false;
    const AdtDef *def = NULL;
    if (t->kind == TY_ADT) {
        def = t->as.adt_.def;
        if (!def || !adt_is_byvalue_product(def)) return false;
    } else if (t->kind == TY_APP) {
        def = type_adt_app_def((Type *)t);
        if (!def || !adt_app_is_byvalue_product(*(Type *)t)) return false;
    } else {
        return false;
    }
    return def->needs_drop_glue && !def->is_heap && def->n_ctors == 1;
}
static bool owning_multishot_admissible(const Type *t) {
    if (!t) return false;
    return t->kind == TY_RC
        || type_is_heap_adt(*(Type *)t)
        || type_is_heap_struct(*(Type *)t)
        || owning_byvalue_agg(t);
}

/* ---- file-local helper forward declarations ---- */
static void check_cloneable_capture_precise(Elab *e, Span span,
                                            const Expr *reset_body);
static void check_serializable_capture_precise(Elab *e, Span span,
                                               const Expr *reset_body);
static bool is_effect_handled(Elab *e, const Symbol *name);
static void push_handled_effect(Elab *e, const Symbol *name);
static bool elab_effect_is_referred(const Elab *e, const Effect *eff);
static void cont_mark_consumed(Expr *k);

/* fx-row-syntax-rename-plan Phase 2: warn-once helper for legacy effect-row
 * spellings.  Call sites pass an F_MAP form they are about to consume as an
 * effect row; if the form's reader-set provenance is PROV_FX_LEGACY (bare
 * `#{...}`) or PROV_FX_AT_LEGACY (`@{...}`), emit the corresponding
 * deprecation warning and clear the provenance so a single source location
 * warns at most once.  `#fx{...}` (PROV_FX_EXPLICIT) and non-effect-row uses
 * are silent. */
void warn_legacy_fx_row(Form *f) {
    if (!f || f->tag != F_MAP) return;
    DiagCode code;
    const char *spelling;
    switch ((FxProvenance)f->fx_prov) {
        case PROV_FX_LEGACY:
            code = TUR_D0002_FX_ROW_LEGACY_HASH;
            spelling = "#{...}";
            break;
        case PROV_FX_AT_LEGACY:
            code = TUR_D0003_FX_ROW_LEGACY_AT;
            spelling = "@{...}";
            break;
        default:
            return;
    }
    diag_emit_with_code(DIAG_WARNING, f->span, code,
        "%s effect row is deprecated; prefer #fx{...} "
        "(run tools/migrate-fx-rows.py to rewrite)", spelling);
    f->fx_prov = (uint8_t)PROV_FX_EXPLICIT;
}

/* Phase 18: Delimited continuations */

/* CF2: Derive the delimited-continuation result type for shift/shift0.
 *
 * Typing rule (v1 semantics: `(shift f body)` evaluates to `(f body)`):
 *   f : A -> B          -- the continuation receiver (1 value argument)
 *   body : A            -- the value handed to the receiver
 *   ------------------------------------------------------------------
 *   (shift f body) : B  -- the receiver's *result* type, not body's type
 *
 * The earlier placeholder reused `body->type` (the receiver's *domain* A),
 * which only coincides with B when f's domain equals its codomain.  This walks
 * f to recover both A (domain) and B (codomain).  A captured-closure receiver
 * carries the env as parameter 0, so the value parameter is at index 1.
 * Returns false when f's function type is not statically available. */
static bool shift_fn_domain_codomain(const Expr *k, Type *domain, Type *codomain) {
    const Type *ft = NULL;
    uint8_t val_idx = 0;
    if (k->kind == EX_CLOSURE) {
        struct Closure *c = k->as.closure_.closure;
        if (c && c->fn && c->fn->binding && c->fn->binding->type.kind == TY_FN) {
            ft = &c->fn->binding->type;
            val_idx = 1;  /* env occupies parameter 0 */
        }
    } else if (k->type.kind == TY_FN) {
        ft = &k->type;
    } else if (k->kind == EX_VAR && k->as.var.binding &&
               k->as.var.binding->type.kind == TY_FN) {
        ft = &k->as.var.binding->type;
    }
    if (!ft || ft->as.fn.arity <= val_idx) return false;

    if (ft->as.fn.arg_full_types && ft->as.fn.arg_full_types[val_idx]) {
        *domain = *ft->as.fn.arg_full_types[val_idx];
    } else {
        TypeKind dk = ft->as.fn.arg_kinds[val_idx];
        *domain = type_simple(dk, typekind_default_copy_kind(dk));
    }
    if (ft->as.fn.result_full_type) {
        *codomain = *ft->as.fn.result_full_type;
    } else {
        TypeKind rk = ft->as.fn.result_kind;
        *codomain = type_simple(rk, typekind_default_copy_kind(rk));
    }
    return true;
}

/* cps-backend-n6 cross-function resume: the Type of a shift receiver's
 * continuation parameter, read from the receiver's own param binding (which
 * preserves the cont flavor and copy_kind that `shift_fn_domain_codomain`'s
 * domain fallback loses).  Works for an inline lambda (EX_FN), a capturing
 * closure (EX_CLOSURE, env at param 0), and a named-fn reference (EX_VAR ->
 * source_fn_def).  Returns NULL if the receiver's continuation param is not
 * reachable (e.g. a forward-referenced named fn not yet elaborated). */
static const Type *receiver_cont_param_type(const Expr *k_expr) {
    const FnDef *fn = NULL;
    uint8_t idx = 0;
    if (k_expr->kind == EX_FN) {
        fn = k_expr->as.fn_.fn; idx = 0;
    } else if (k_expr->kind == EX_CLOSURE && k_expr->as.closure_.closure) {
        fn = k_expr->as.closure_.closure->fn; idx = 1;  /* env occupies param 0 */
    } else if (k_expr->kind == EX_VAR && k_expr->as.var.binding) {
        fn = k_expr->as.var.binding->source_fn_def; idx = 0;
    }
    if (!fn || fn->n_params <= idx || !fn->params || !fn->params[idx]) return NULL;
    return &fn->params[idx]->type;
}

/* CF2: shared shift/shift0 result-typing.  Verifies the body matches the
 * receiver's domain (rejecting a mistyped shift) and returns the receiver's
 * codomain as the expression's type.  Falls back to `body->type` only when the
 * receiver's type is not statically known (or its codomain is unresolved), so
 * the placeholder behavior is preserved exactly where no better type exists.
 * `form` is "shift" or "shift0" for diagnostics; `body_span` locates the body.
 * On a type error emits TUR-E0001 and sets *ok = false. */
static Type shift_result_type(const Expr *k, const Expr *body,
                              const char *form, Span body_span, bool *ok) {
    *ok = true;
    Type domain, codomain;
    if (!shift_fn_domain_codomain(k, &domain, &codomain))
        return body->type;  /* receiver type unknown -- preserve prior behavior */

    /* Reject a mistyped shift: the body is the value passed to the continuation
     * receiver, so it must match the receiver's parameter type. */
    if (domain.kind != TY_UNKNOWN && body->type.kind != TY_UNKNOWN &&
        !type_eq(body->type, domain)) {
        diag_emit_with_code(DIAG_ERROR, body_span, TUR_E0001_TYPE_MISMATCH,
            "%s: body type mismatch -- the continuation receiver expects %s, "
            "but the body has type %s",
            form, type_name(domain), type_name(body->type));
        *ok = false;
        return body->type;
    }

    return codomain.kind != TY_UNKNOWN ? codomain : body->type;
}

/* (reset body) - Establish a continuation boundary.
 */
static Expr *elab_cont_shift_core(Elab *e, const Form *call, Expr *k_expr);  /* fwd */
static Form *reflavor_shift_receiver(Elab *e, Form *recv);                   /* fwd */
static Effect *elab_get_shift_effect(Elab *e);                               /* fwd */

/* cps-backend-n6 cross-function resume: record a reset node so the gated
 * post-elaboration pass can wrap it in a __Shift handler (see Elab field docs). */
static void record_reset_node(Elab *e, Expr *node) {
    if (e->n_pending_reset_nodes >= e->cap_pending_reset_nodes) {
        e->cap_pending_reset_nodes =
            e->cap_pending_reset_nodes ? e->cap_pending_reset_nodes * 2 : 16;
        e->pending_reset_nodes = (Expr **)realloc(
            e->pending_reset_nodes,
            e->cap_pending_reset_nodes * sizeof(Expr *));
    }
    e->pending_reset_nodes[e->n_pending_reset_nodes++] = node;
}

Expr *elab_reset(Elab *e, const Form *call) {
    /* The body is VARIADIC: `(reset a b)` used to be
     * "(reset body) requires exactly one argument", so a multi-step delimited
     * computation paid an explicit `(do ...)`.  Folding the `do` in changes no
     * semantics -- the delimiter delimits exactly the same extent. */
    if (call->as.list.len < 2) {
        diag_emit(DIAG_ERROR, call->span,
                  "(reset body ...) requires at least one body expression");
        return NULL;
    }
    /* Item B (resuming-shift plan): a plain `reset` is the abortive delimiter
     * (EX_RESET) UNLESS a resuming (resume-k) shift binds to it -- then it
     * promotes to the reified delimiter (EX_CLONEABLE_RESET), so one `reset`
     * keyword serves both.  Bumping cloneable_reset_depth lets a resuming shift
     * inside satisfy its lexical-scope requirement; the per-depth flag records
     * whether one actually bound here.  A reset with only abortive shifts / shift0
     * (which need EX_RESET -- proven by the reset-alias experiment) stays EX_RESET. */
    int d = ++e->cloneable_reset_depth;
    bool track = d >= 0 && d < 64;
    if (track) { e->reified_shift_at_depth[d] = false;
                 e->reified_serial_at_depth[d] = false;
                 /* A plain `reset` is flavor-flexible -- not pinned cloneable. */
                 e->pinned_cloneable_at_depth[d] = false; }
    Expr *body = elab_implicit_do(e, call, 1);
    bool reified = track && e->reified_shift_at_depth[d];
    bool serial  = track && e->reified_serial_at_depth[d];
    e->cloneable_reset_depth--;
    if (!body) return NULL;
    if (reified && serial) {
        /* Capability-folding item 1: a resuming shift with a `serial-cont`
         * receiver bound here, so this plain `reset` IS the serial delimiter --
         * lower it exactly like `serial-reset` (EX_SERIAL_RESET), including the
         * Serializable-capture check on the reified continuation body. */
        check_serializable_capture_precise(e, call->span, body);
        Expr *out = expr_new(e->arena, EX_SERIAL_RESET, body->type, call->span);
        out->as.serial_reset_.body = body;
        return out;
    }
    if (reified) {
        /* CPS-CL10 / E4: a resuming shift bound here -- verify the captures. */
        check_cloneable_capture_precise(e, call->span, body);
        Expr *out = expr_new(e->arena, EX_CLONEABLE_RESET, body->type, call->span);
        out->as.cloneable_reset_.body = body;
        record_reset_node(e, out);
        return out;
    }
    Expr *out = expr_new(e->arena, EX_RESET, body->type, call->span);
    out->as.reset_.body = body;
    record_reset_node(e, out);
    return out;
}

/* (shift k body) - Capture the current continuation.
 */
Expr *elab_shift(Elab *e, const Form *call) {
    if (call->as.list.len != 3) {
        diag_emit(DIAG_ERROR, call->span,
                  "(shift k body) requires exactly two arguments");
        return NULL;
    }
    /* cps-backend-n6 cross-function resume: a `shift` with no lexically-enclosing
     * reset (cloneable_reset_depth == 0) whose receiver is a `cont`-param lambda
     * cannot use the reified (lexically-scoped) path -- it is either an abortive
     * ignore-k shift or a cross-function resuming shift that lowers onto the
     * __Shift effect.  Reflavor the receiver's `cont` param to `effect-cont` BEFORE
     * elaborating (so `(k v)` in the receiver body lowers to EX_RESUME, slice A),
     * and elaborate that copy ONCE -- avoiding a dead cloneable-flavored lift that
     * would reference an unlinked cloneable-resume runtime.  Inside a reset (depth
     * > 0) the reified path is used, so the receiver stays cloneable-flavored. */
    Form *recv_to_elab = call->as.list.items[1];
    if (e->cloneable_reset_depth == 0) {
        Form *reflav = reflavor_shift_receiver(e, recv_to_elab);
        if (reflav) recv_to_elab = reflav;
    }
    Expr *k_expr = elab_form(e, recv_to_elab);
    if (!k_expr) return NULL;
    
    /* Check if k_expr is a function, closure, or a var referencing a function */
    bool is_function = false;
    if (k_expr->kind == EX_FN || k_expr->kind == EX_CLOSURE) {
        is_function = true;
    } else if (k_expr->kind == EX_VAR) {
        /* Check if the binding is a function */
        Binding *b = k_expr->as.var.binding;
        if (b && (b->type.kind == TY_FN || b->closure_fn_binding)) {
            is_function = true;
        }
    }
    
    if (!is_function) {
        diag_emit(DIAG_ERROR, call->as.list.items[1]->span,
                  "shift requires a function as first argument");
        return NULL;
    }

    /* Item B (resuming-shift plan): keyword collapse.  A `shift` whose receiver
     * takes a `cont` uses the continuation-passing convention -- route
     * it to the unified core (resume-k reified, or ignore-k dynamic abort).  A
     * receiver whose parameter is a plain value keeps the abortive convention
     * below (receiver applied to the body value).  This makes one `shift` keyword
     * serve both conventions, dispatched by the receiver's parameter type; every
     * existing abortive `shift` (non-`cont` receiver) is unchanged. */
    {
        Type dom, cod;
        if (shift_fn_domain_codomain(k_expr, &dom, &cod) && dom.kind == TY_CONT)
            return elab_cont_shift_core(e, call, k_expr);
    }

    Expr *body = elab_form(e, call->as.list.items[2]);
    if (!body) return NULL;
    /* CF2: the result type of (shift f body) is f's codomain (the result of
     * calling f with body's value), not body's type.  Also rejects a body whose
     * type does not match f's parameter type. */
    bool ok = true;
    Type result_type = shift_result_type(k_expr, body, "shift",
                                         call->as.list.items[2]->span, &ok);
    if (!ok) return NULL;
    Expr *out = expr_new(e->arena, EX_SHIFT, result_type, call->span);
    out->as.shift_.k_fn = k_expr;
    out->as.shift_.body = body;
    return out;
}

/* (shift0 k body) - One-shot shift.
 */
Expr *elab_shift0(Elab *e, const Form *call) {
    if (call->as.list.len != 3) {
        diag_emit(DIAG_ERROR, call->span,
                  "(shift0 k body) requires exactly two arguments");
        return NULL;
    }
    Expr *k_expr = elab_form(e, call->as.list.items[1]);
    if (!k_expr) return NULL;
    
    /* Check if k_expr is a function, closure, or a var referencing a function */
    bool is_function = false;
    if (k_expr->kind == EX_FN || k_expr->kind == EX_CLOSURE) {
        is_function = true;
    } else if (k_expr->kind == EX_VAR) {
        /* Check if the binding is a function */
        Binding *b = k_expr->as.var.binding;
        if (b && (b->type.kind == TY_FN || b->closure_fn_binding)) {
            is_function = true;
        }
    }
    
    if (!is_function) {
        diag_emit(DIAG_ERROR, call->as.list.items[1]->span,
                  "shift0 requires a function as first argument");
        return NULL;
    }
    
    Expr *body = elab_form(e, call->as.list.items[2]);
    if (!body) return NULL;
    /* CF2: result type is f's codomain (see elab_shift).  shift0 differs from
     * shift only in delimiter behavior at runtime, not in this local typing. */
    bool ok = true;
    Type result_type = shift_result_type(k_expr, body, "shift0",
                                         call->as.list.items[2]->span, &ok);
    if (!ok) return NULL;
    Expr *out = expr_new(e->arena, EX_SHIFT0, result_type, call->span);
    out->as.shift0_.k_fn = k_expr;
    out->as.shift0_.body = body;
    return out;
}

/* Phase B2: Cloneable continuations */

/* CPS-CL10 / E4 (elab-time): emit TUR-E0014 for a binding CAPTURED into a
 * cloneable-shift's multi-shot continuation that lacks a Clone instance.
 *
 * This runs once per reset, on the full reset body (the delimited context the
 * continuation reifies), so it knows exactly which fn-local bindings the
 * continuation actually references -- the free variables of that body.  The
 * old per-shift check ran before the body existed and had to over-approximate
 * to EVERY in-scope binding; that spuriously rejected an owning value (an `rc`,
 * a non-Clone struct, ...) merely being *in scope* at a cloneable-shift, even
 * when the continuation never touches it (E4).  An owning value that is not
 * free in the continuation is provably not captured, so it needs no Clone; one
 * that IS free (genuinely captured owning) stays rejected -- the native
 * multi-shot env cannot own a reference without the E3 env clone/drop teardown,
 * which is unbuilt.  fn-local bindings only (walk stops at
 * fn_entry_outer_scope): an ENCLOSING-fn binding is not captured into the
 * continuation env (CF7.3) and a top-level def is global, so neither is a
 * capture candidate. */
static void check_cloneable_capture_precise(Elab *e, Span span,
                                            const Expr *reset_body) {
    const Symbol *clone_sym = intern_cstr(e->st, "Clone");
    TypeClass *clone_tc = typeclass_env_lookup_typeclass(&e->typeclass_env, clone_sym);
    if (!clone_tc) return; /* No Clone typeclass in scope; nothing to check */
    if (!reset_body) return;

    /* The free variables of the reifiable continuation context.  A binding not
     * in this set is not referenced by the continuation and so is never cloned
     * on resume -- it does not need Clone regardless of its type. */
    uint32_t n_fv = 0;
    Binding **fvs = collect_free_vars(reset_body, NULL, 0, NULL, 0, &n_fv);

    Scope *stop = e->fn_entry_outer_scope ? e->fn_entry_outer_scope : &e->global;
    for (Scope *s = e->scope; s != NULL && s != stop; s = s->parent) {
        for (uint32_t i = 0; i < s->n; i++) {
            Binding *b = s->bindings[i];
            if (!b || !b->name) continue;
            Type t = b->type;
            /* Primitive/function/continuation types are always safe to capture */
            if (t.kind == TY_NIL || t.kind == TY_FN ||
                t.kind == TY_CLONEABLE_CONT) continue;
            /* Only a binding the continuation actually references is captured. */
            bool captured = false;
            for (uint32_t j = 0; j < n_fv; j++)
                if (fvs[j] == b) { captured = true; break; }
            if (!captured) continue;
            TypeClassInstance *inst =
                typeclass_env_lookup_instance(&e->typeclass_env, clone_tc, &t, 1);
            if (!inst) {
                /* E3a (graduated): an owning kind we can emit multi-shot env
                 * teardown for is ADMITTED instead of rejected -- the cloneable
                 * codegen gives its captured frame env clone glue so each resume
                 * owns its own +1. */
                if (owning_multishot_admissible(&t))
                    continue;
                diag_emit_with_code(DIAG_ERROR, span,
                                    TUR_E0014_NOT_CLONE,
                                    "captured binding '%s' does not implement Clone "
                                    "(required by cloneable-shift)", b->name->name);
            }
        }
    }
    if (fvs) free(fvs);
}

/* (cloneable-reset body) - Establish a continuation boundary with cloneable captures.
 * Similar to reset, but all captured values must implement Clone.
 * The body can use cloneable-shift to capture a cloneable continuation. */
Expr *elab_cloneable_reset(Elab *e, const Form *call) {
    /* Variadic body -- see elab_reset. */
    if (call->as.list.len < 2) {
        diag_emit(DIAG_ERROR, call->span,
                  "(cloneable-reset body ...) requires at least one body expression");
        return NULL;
    }
    /* CPS-CL7: track nesting depth so cloneable-shift can detect missing reset */
    int d = ++e->cloneable_reset_depth;
    /* Capability-folding item 1: this depth is pinned cloneable by the keyword, so
     * a `serial-cont` plain-`shift` bound here is rejected with a clear message. */
    if (d >= 0 && d < 64) e->pinned_cloneable_at_depth[d] = true;
    Expr *body = elab_implicit_do(e, call, 1);
    e->cloneable_reset_depth--;
    if (!body) return NULL;
    /* CPS-CL10 / E4: verify captures of the reified continuation body. */
    check_cloneable_capture_precise(e, call->span, body);
    Expr *out = expr_new(e->arena, EX_CLONEABLE_RESET, body->type, call->span);
    out->as.cloneable_reset_.body = body;
    record_reset_node(e, out);
    return out;
}

/* (cloneable-shift k body) - Capture the current cloneable continuation.
 * Similar to shift, but the continuation passed to k is cloneable (multi-shot).
 * All captured environment values must implement Clone.
 * k is a function that receives the cloneable continuation as its first argument.
 */
/* Unification (resuming-shift plan): does this shift receiver PROVABLY ignore
 * its captured continuation?  A receiver `(fn [k] EXPR)` whose body never
 * references `k` discards the continuation -- semantically an abortive shift,
 * which can lower via the dynamic-abort path (cross-function, any context)
 * instead of the reified-context path (lexically scoped, subset-restricted).
 * Conservative: returns true ONLY for a lambda/closure receiver we can see
 * through and prove `k` unused; every other case returns false and keeps the
 * reified lowering, so the routing is purely additive. */
static bool receiver_ignores_continuation(const Expr *k_expr) {
    const FnDef *fn = NULL;
    /* `val_idx` is the parameter index the shift passes the continuation to,
     * matching shift_fn_domain_codomain: a bare lambda / named fn takes it at 0;
     * a closure thunk has its env prepended at 0, so the continuation is at 1. */
    uint8_t val_idx = 0;
    if (k_expr->kind == EX_FN) {
        fn = k_expr->as.fn_.fn; val_idx = 0;
    } else if (k_expr->kind == EX_CLOSURE && k_expr->as.closure_.closure) {
        fn = k_expr->as.closure_.closure->fn; val_idx = 1;
    } else if (k_expr->kind == EX_VAR && k_expr->as.var.binding) {
        /* A named top-level fn receiver: source_fn_def gives its FnDef/body
         * (set during elaboration).  NULL for a forward reference not yet
         * elaborated -- conservatively keep the reified path in that case. */
        fn = k_expr->as.var.binding->source_fn_def; val_idx = 0;
    }
    /* Only a single-continuation receiver: the continuation must be the LAST
     * param (no trailing params), so `params[val_idx]` really is the one the
     * shift binds.  A multi-param fn is not a valid one-arg receiver. */
    if (!fn || !fn->body || fn->n_params != (uint8_t)(val_idx + 1)) return false;
    const Binding *kparam = fn->params[val_idx];
    if (!kparam) return false;
    uint32_t n_out = 0;
    Binding **fvs = collect_free_vars(fn->body, NULL, 0, NULL, 0, &n_out);
    bool uses_k = false;
    for (uint32_t i = 0; i < n_out; i++)
        if (fvs[i] == kparam) { uses_k = true; break; }
    if (fvs) free(fvs);
    return !uses_k;
}

/* cps-backend-n6 cross-function resume.  Deep-clone a form, replacing every
 * continuation-flavor annotation naming `from` with `to` (e.g. cont ->
 * effect-cont).  Recurses through lists / vecs / type-annotations so it catches
 * the annotation however the reader spelled it (`[k : cont]` -> a keyword,
 * `[k (cont R)]` -> a list head, a bare `cont` symbol).  Non-matching atoms are
 * shared (returned as-is); only the spine that changed is rebuilt. */
static Form *reflavor_cont_sym(Elab *e, Form *f,
                               const Symbol *from, const Symbol *to) {
    if (!f) return f;
    if ((f->tag == F_SYM || f->tag == F_KEYWORD) && f->as.sym == from) {
        return (f->tag == F_KEYWORD) ? form_keyword(e->arena, f->span, to)
                                     : form_sym(e->arena, f->span, to);
    }
    if (f->tag == F_TYPE_ANN && f->as.list.len >= 1) {
        Form *inner = reflavor_cont_sym(e, f->as.list.items[0], from, to);
        if (inner == f->as.list.items[0]) return f;
        return form_type_ann(e->arena, f->span, inner);
    }
    if (f->tag == F_LIST || f->tag == F_VEC) {
        uint32_t n = f->as.list.len;
        Form **items = NULL;
        for (uint32_t i = 0; i < n; i++) {
            Form *ni = reflavor_cont_sym(e, f->as.list.items[i], from, to);
            if (ni != f->as.list.items[i] && !items) {
                items = (Form **)arena_alloc(e->arena, n * sizeof(Form *));
                for (uint32_t j = 0; j < i; j++) items[j] = f->as.list.items[j];
            }
            if (items) items[i] = ni;
        }
        if (!items) return f;  /* nothing changed */
        return (f->tag == F_VEC) ? form_vec(e->arena, f->span, items, n)
                                 : form_list(e->arena, f->span, items, n);
    }
    return f;
}

/* cps-backend-n6 cross-function resume.  Reflavor a shift receiver's `cont`
 * parameter to `multishot-effect-cont`, so `(k v)` inside it resumes the effect
 * continuation multi-shot (CONT_EFFECT + CK_MULTISHOT -> snapshot EX_RESUME)
 * rather than a cloneable one.  Multi-shot is the general case: a receiver that
 * resumes `k` once behaves identically (snapshot once, resume once), and one that
 * resumes it more than once (e.g. (+ (k 1) (k 2))) now works, matching the
 * ^multishot handler the reset-wrap installs.  Only a lambda-literal receiver
 * `(fn [k : cont] ...)` is rewritable from the call site; a named-fn receiver
 * returns NULL (the caller keeps the E0016 error).  Only the parameter vector is
 * reflavored -- the body is left untouched, so a body reference to something
 * coincidentally named `cont` is not disturbed. */
static Form *reflavor_shift_receiver(Elab *e, Form *recv) {
    if (!recv || recv->tag != F_LIST || recv->as.list.len < 2) return NULL;
    Form *head = recv->as.list.items[0];
    if (head->tag != F_SYM || head->as.sym != e->sym_fn) return NULL;
    Form *params = recv->as.list.items[1];
    if (params->tag != F_VEC && params->tag != F_LIST) return NULL;
    const Symbol *cont_sym = intern_cstr(e->st, "cont");
    const Symbol *eff_sym  = intern_cstr(e->st, "multishot-effect-cont");
    Form *new_params = reflavor_cont_sym(e, params, cont_sym, eff_sym);
    if (new_params == params) return NULL;  /* no `cont` annotation to reflavor */
    uint32_t fn_n = recv->as.list.len;
    Form **new_fn = (Form **)arena_alloc(e->arena, fn_n * sizeof(Form *));
    for (uint32_t i = 0; i < fn_n; i++) new_fn[i] = recv->as.list.items[i];
    new_fn[1] = new_params;
    return form_list(e->arena, recv->span, new_fn, fn_n);
}

/* Does form `f` (recursively) contain a CONTINUATION-RESUME of `kname` --
 * `(resume kname ...)` OR the `(kname ...)` application sugar (k applied as a
 * function)?  Either proves kname is a continuation. */
static bool form_resumes_sym(const Form *f, const Symbol *resume_sym, const Symbol *kname) {
    if (!f || (f->tag != F_LIST && f->tag != F_VEC)) return false;
    if (f->as.list.len >= 1) {
        const Form *h = f->as.list.items[0];
        if (h->tag == F_SYM) {
            /* (k ...) -- k applied as a function (resume sugar). */
            if (h->as.sym == kname) return true;
            /* (resume k ...) */
            if (h->as.sym == resume_sym && f->as.list.len >= 2) {
                const Form *a = f->as.list.items[1];
                if (a->tag == F_SYM && a->as.sym == kname) return true;
            }
        }
    }
    for (uint32_t i = 0; i < f->as.list.len; i++)
        if (form_resumes_sym(f->as.list.items[i], resume_sym, kname)) return true;
    return false;
}

/* cps-dk-multishot-user-effects (Phase C): is `payload` a lambda `(fn [k ...] body)`
 * whose body RESUMES its first param -- `(resume k ...)` or the `(k ...)` sugar?
 * The reliable signal that a raw-`int`-typed payload param is actually a
 * continuation (its type carries no cont flavor), so a `(fn [int] R)` payload can
 * be reflavored to the DK cloneable substrate WITHOUT disturbing a genuine
 * non-continuation `(fn [int] R)` payload (which never resumes its param). */
static bool form_lambda_resumes_first_param(Elab *e, const Form *payload) {
    if (!payload || payload->tag != F_LIST || payload->as.list.len < 3) return false;
    const Form *head = payload->as.list.items[0];
    if (head->tag != F_SYM || head->as.sym != e->sym_fn) return false;
    const Form *params = payload->as.list.items[1];
    if ((params->tag != F_VEC && params->tag != F_LIST) || params->as.list.len < 1)
        return false;
    const Form *p0 = params->as.list.items[0];
    if (p0->tag != F_SYM) return false;
    const Symbol *kname = p0->as.sym;
    const Symbol *resume_sym = intern_cstr(e->st, "resume");
    for (uint32_t i = 2; i < payload->as.list.len; i++)
        if (form_resumes_sym(payload->as.list.items[i], resume_sym, kname)) return true;
    return false;
}

/* cps-dk-multishot-user-effects (Phase A): reflavor a USER effect's fn PAYLOAD
 * lambda's `effect-cont` param to `multishot-effect-cont` BEFORE elaboration, so
 * `(k v)` inside the payload lowers to the DK-backed `tur_cloneable_cont_resume`
 * (CK_MULTISHOT snapshot resume) rather than the fiber `tur_effect_cont_resume`.
 * This is the user-effect analogue of reflavor_shift_receiver: it lets a
 * `(perform (E (fn [k : effect-cont] (k v))))` performer + its `(E [f] k) (f k)`
 * handler CPS-emit through the same cloneable-cont substrate __Shift uses, instead
 * of co-evicting to fiber.  Multishot is the sound generalization of one-shot
 * (resume-once behaves identically -- snapshot once, resume once), so upgrading a
 * one-shot `effect-cont` payload does not change observable behaviour on any
 * backend.  Only a lambda-literal payload `(fn [k : effect-cont] ...)` is
 * rewritable from the perform site; anything else returns NULL (unchanged).  Only
 * the parameter vector is reflavored -- the body is untouched. */
static Form *reflavor_effect_payload(Elab *e, Form *payload) {
    if (!payload || payload->tag != F_LIST || payload->as.list.len < 2) return NULL;
    Form *head = payload->as.list.items[0];
    if (head->tag != F_SYM || head->as.sym != e->sym_fn) return NULL;
    Form *params = payload->as.list.items[1];
    if (params->tag != F_VEC && params->tag != F_LIST) return NULL;
    const Symbol *to = intern_cstr(e->st, "multishot-effect-cont");
    /* Reflavor `effect-cont` (the annotated case) OR a raw `int` continuation
     * handle (cps-dk-multishot Phase C) -- the latter only when the payload body
     * actually resumes its first param (form_lambda_resumes_first_param), so a
     * genuine `(fn [int] R)` non-continuation payload is never disturbed.  Both
     * upgrade to `multishot-effect-cont` so `(k v)`/`resume` lowers to the DK
     * cloneable substrate.  The int->cont reflavor type-checks: a TY_CONT payload
     * unifies with the effect's declared `(fn [int] R)` param (int carrier). */
    Form *new_params = reflavor_cont_sym(e, params, intern_cstr(e->st, "effect-cont"), to);
    if (new_params == params && form_lambda_resumes_first_param(e, payload))
        new_params = reflavor_cont_sym(e, params, intern_cstr(e->st, "int"), to);
    if (new_params == params) return NULL;  /* nothing to reflavor */
    uint32_t fn_n = payload->as.list.len;
    Form **new_fn = (Form **)arena_alloc(e->arena, fn_n * sizeof(Form *));
    for (uint32_t i = 0; i < fn_n; i++) new_fn[i] = payload->as.list.items[i];
    new_fn[1] = new_params;
    return form_list(e->arena, payload->span, new_fn, fn_n);
}

/* cps-dk-multishot-user-effects (Phase A): does effect `eff` declare a RESUMABLE
 * fn PAYLOAD -- a param `(fn [<cont>] R)` whose first arg is a continuation
 * (`effect-cont` / `multishot-effect-cont`, TY_CONT)?  Such an effect is resumed
 * THROUGH the payload (`(E [f] k) (f k)`): the payload's `(k v)` resumes the
 * handler continuation.  For the CPS/DK backend to emit performer + handler
 * through the shared DK-backed cloneable-cont substrate (the __Shift bridge,
 * generalized), the payload's cont param is reflavored to multishot at the
 * perform site (reflavor_effect_payload) AND the handler's `k` is auto-upgraded to
 * CK_MULTISHOT (below) so both halves agree on the cloneable substrate -- matching
 * a hand-written `^multishot` handler.  Returns the index of the resumable payload
 * param, or -1. */
static int effect_resumable_payload_param(const Effect *eff) {
    if (!eff || !eff->constructor) return -1;
    return eff->constructor->resumable_payload_param;
}

/* Does a param TYPE form denote a resumable fn payload -- `(fn [effect-cont] R)`
 * / `(fn [multishot-effect-cont] R)`?  The `effect-cont` cont flavor collapses to
 * its TY_INT carrier in the stored Type (arg_kinds), so the FORM is the reliable
 * signal.  Scans for the flavor symbol as the fn's first param annotation (a bare
 * symbol, keyword, or type-annotation head) inside the type form's spine. */
static bool form_type_is_cont_payload(Elab *e, const Form *f) {
    if (!f) return false;
    const Symbol *ec  = intern_cstr(e->st, "effect-cont");
    const Symbol *mec = intern_cstr(e->st, "multishot-effect-cont");
    if ((f->tag == F_SYM || f->tag == F_KEYWORD)
        && (f->as.sym == ec || f->as.sym == mec))
        return true;
    if ((f->tag == F_LIST || f->tag == F_VEC || f->tag == F_TYPE_ANN)) {
        for (uint32_t i = 0; i < f->as.list.len; i++)
            if (form_type_is_cont_payload(e, f->as.list.items[i])) return true;
    }
    return false;
}

/* cps-backend-n6 cross-function resume.  Lazily register the synthetic __Shift
 * effect, once per program.  It carries the shift's receiver as a boxed fn
 * payload `(fn [effect-cont] int)` (blocker 3: a one-word {thunk,env} closure so
 * a capturing receiver fits the effect slot) and returns int (the delimited
 * result).  Subsequent calls return the cached Effect. */
static Effect *elab_get_shift_effect(Elab *e) {
    const Symbol *name = intern_cstr(e->st, "__Shift");
    Effect *eff = effect_env_lookup(e->effect_env, name);
    if (eff) return eff;
    const Symbol **pnames = arena_alloc(e->arena, sizeof(const Symbol *));
    pnames[0] = intern_cstr(e->st, "recv");
    TypeKind *ptypes = arena_alloc(e->arena, sizeof(TypeKind));
    ptypes[0] = TY_FN;
    eff = effect_env_register(e->effect_env, e->arena, name, pnames, ptypes, 1,
                              TY_INT, e->current_module_name, false);
    if (!eff || !eff->constructor) return NULL;
    /* Preserve the full boxed fn payload type so `(recv k)` is callable in the
     * synthesized handler (a def-less TY_FN degrades to a 0-arity uncallable fn).
     * The single arg is the int64-carried effect continuation. */
    TypeKind argk = TY_INT;
    Type *fnt = arena_alloc(e->arena, sizeof(Type));
    *fnt = type_fn(&argk, 1, TY_INT);
    fnt->as.fn.boxed = true;
    const Type **full = arena_alloc(e->arena, sizeof(const Type *));
    full[0] = fnt;
    eff->constructor->param_full_types = full;
    return eff;
}

/* cps-backend-n6 cross-function resume: specialize a named-fn receiver whose
 * continuation parameter is a plain `cont` (CONT_CLONEABLE) into a renamed copy
 * whose param is `multishot-effect-cont`, so it can drive a cross-function resume
 * exactly like an inline-lambda receiver.  Reuses the retained `defn_form` +
 * file-scope re-elaboration path (like bare-fat monomorphization): clone the
 * defn form, rename it (`NAME$xfn`), reflavor its param vector, elaborate at file
 * scope, and register the clone for emission.  Deduped by name -- a second shift
 * on the same receiver finds the already-built clone in global scope.  Returns
 * the clone binding, or NULL when it cannot be built (no retained form, no `cont`
 * param to reflavor, elaboration failed). */
static Binding *elab_specialize_cont_receiver(Elab *e, Binding *callee) {
    if (!callee || !callee->defn_form || !callee->name) return NULL;
    const Form *df = callee->defn_form;
    if (df->tag != F_LIST || df->as.list.len < 3) return NULL;

    /* Fresh name; dedup by looking it up first (so we neither re-elaborate nor
     * double-register on a second cross-function shift of the same receiver). */
    char nbuf[300];
    int wn = snprintf(nbuf, sizeof(nbuf), "%s$xfn", callee->name->name);
    if (wn <= 0 || wn >= (int)sizeof(nbuf)) return NULL;
    const Symbol *mname = symtab_intern(e->st, strslice(nbuf, (uint32_t)wn));
    Binding *existing = scope_lookup(&e->global, mname);
    if (existing) return existing;

    /* Locate the name symbol (first non-caret F_SYM after `defn`) and the
     * parameter vector (first F_VEC/F_LIST), mirroring elab_defn's own parse. */
    uint32_t n = df->as.list.len;
    int name_idx = -1, params_idx = -1;
    for (uint32_t i = 1; i < n; i++) {
        const Form *it = df->as.list.items[i];
        if (name_idx < 0 && it->tag == F_SYM && it->as.sym->name
            && it->as.sym->name[0] != '^') { name_idx = (int)i; continue; }
        if (name_idx >= 0 && (it->tag == F_VEC || it->tag == F_LIST)) {
            params_idx = (int)i; break;
        }
    }
    if (name_idx < 0 || params_idx < 0) return NULL;

    /* Reflavor the param vec cont -> multishot-effect-cont; bail if there is no
     * `cont` annotation to reflavor (nothing to specialize). */
    Form *rp = reflavor_cont_sym(e, df->as.list.items[params_idx],
                                 intern_cstr(e->st, "cont"),
                                 intern_cstr(e->st, "multishot-effect-cont"));
    if (rp == df->as.list.items[params_idx]) return NULL;

    Form **items = (Form **)arena_alloc(e->arena, n * sizeof(Form *));
    for (uint32_t i = 0; i < n; i++) items[i] = df->as.list.items[i];
    items[name_idx]   = form_sym(e->arena, df->as.list.items[name_idx]->span, mname);
    items[params_idx] = rp;
    Form *spec_form = form_list(e->arena, df->span, items, n);

    /* Elaborate the clone at file scope (its params must not capture caller
     * locals), then register it for file-scope emission. */
    Scope *saved = e->scope;
    e->scope = &e->global;
    Expr *def = elab_defn(e, spec_form);
    e->scope = saved;
    if (!def) return NULL;
    if (def->kind == EX_FN_DEF) elab_register_file_def(e, def);
    return scope_lookup(&e->global, mname);
}

/* cps-backend-n6 cross-function resume: wrap a reset body B in a __Shift handler
 *   B  ->  (handle B (__Shift [recv] k) (recv k))
 * so a callee's `(perform (__Shift recv))` is caught here and the receiver is
 * applied to the delimited continuation k.  A lexical abortive/reified shift
 * bypasses this handler (it is EX_SHIFT / EX_CLONEABLE_SHIFT, never a perform) and
 * reaches the reset's own lowering unchanged -- only a cross-function resuming
 * shift performs __Shift and is caught.  The handler body `(recv k)` is built via
 * the real elaborator in a temp scope binding recv (the boxed fn payload) and k
 * (an is_continuation binding), so the boxed-payload application and continuation
 * passing are constructed exactly as a hand-written handler would be. */
static Expr *wrap_reset_body_with_shift_handler(Elab *e, Expr *body_B, Span span) {
    Effect *sheff = elab_get_shift_effect(e);
    if (!sheff || !sheff->constructor || !sheff->constructor->param_full_types)
        return body_B;
    /* Names are pure lowercase letters (no underscore, no sigil): a boxed-payload
     * handler param is DECLARED by its verbatim name but USED through the
     * injective mangler, which rewrites a literal '_' as "_un" -- so any name with
     * a byte the mangler touches desyncs the declaration from the use.  These live
     * in a fresh handler scope, so they cannot collide with user code. */
    const Symbol *recv_name = intern_cstr(e->st, "xshiftrecv");
    const Symbol *k_name    = intern_cstr(e->st, "xshiftkont");

    Scope hs;
    scope_init(&hs, e->scope);
    Scope *saved = e->scope;
    e->scope = &hs;

    Type recv_type = *sheff->constructor->param_full_types[0];
    Binding *recv_b = binding_new(e, recv_name, recv_type, false, false, span);
    scope_add(&hs, recv_b);
    /* k is the delimited continuation, carried as an int64 handle.  Multi-shot
     * discipline (mirrors elab_handle's CK_MULTISHOT case): the captured
     * continuation is a snapshot-capable tur_cloneable_cont, so a receiver that
     * resumes it more than once runs an independent copy each time.  Single-resume
     * receivers behave identically (one snapshot, one resume). */
    Binding *k_b = binding_new(e, k_name, TYPE_INT, false, false, span);
    k_b->is_continuation = true;
    k_b->type.copy_kind = CK_MULTISHOT;
    scope_add(&hs, k_b);

    /* (recv k) */
    Form **ci = (Form **)arena_alloc(e->arena, 2 * sizeof(Form *));
    ci[0] = form_sym(e->arena, span, recv_name);
    ci[1] = form_sym(e->arena, span, k_name);
    Form *callf = form_list(e->arena, span, ci, 2);
    Expr *hbody = elab_form(e, callf);

    e->scope = saved;
    scope_free(&hs);
    if (!hbody) return body_B;

    HandleCase *cases = (HandleCase *)arena_alloc(e->arena, sizeof(HandleCase));
    /* Arena memory is not zeroed, and cps_ir.c copies every HandleCase field
     * unconditionally (`cs[ci].resumable_payload = c->resumable_payload` was
     * UBSan's "load of value N, which is not a valid value for type '_Bool'"
     * on the macOS sanitizer gate).  Zero first, then set what this site
     * means -- the same rule as the HandleExpr below. */
    memset(cases, 0, sizeof(HandleCase));
    cases[0].effect_name    = sheff->name;
    cases[0].n_params       = 1;
    cases[0].param_names    = (const Symbol **)arena_alloc(e->arena, sizeof(const Symbol *));
    cases[0].param_names[0] = recv_name;
    cases[0].param_bindings = (Binding **)arena_alloc(e->arena, sizeof(Binding *));
    cases[0].param_bindings[0] = recv_b;
    cases[0].k_name         = k_name;
    cases[0].k_binding      = k_b;
    cases[0].cont_kind      = CK_MULTISHOT;
    cases[0].body           = hbody;

    HandleExpr *h = (HandleExpr *)arena_alloc(e->arena, sizeof(HandleExpr));
    /* Arena memory is not zeroed, and every field of this struct is read
     * unconditionally at emit time -- `shallow` was left uninitialized here,
     * which UBSan catches as "load of value 190, which is not a valid value
     * for type '_Bool'".  Zero first, then set what this site means. */
    memset(h, 0, sizeof(HandleExpr));
    h->body            = body_B;
    h->cases           = cases;
    h->n_cases         = 1;
    h->is_unsafe_marker = false;

    Expr *out = expr_new(e->arena, EX_HANDLE, body_B->type, span);
    out->as.handle_.handle = h;
    return out;
}

/* cps-backend-n6 cross-function resume: the gated whole-program pass.  When the
 * program contains a cross-function resuming shift (uses_crossfn_resume), wrap
 * every recorded reset node's body in a __Shift handler; otherwise a no-op, so a
 * program with no such shift keeps byte-for-byte-identical reset codegen.  Runs
 * post-elaboration (a same-elaboration flag would not do -- a reset can be
 * elaborated before the callee's shift sets the flag). */
void elab_wrap_resets_for_crossfn_resume(Elab *e) {
    if (!e || !e->uses_crossfn_resume) return;
    /* Only a PLAIN reset (EX_RESET) is wrappable into a __Shift handler; an
     * EX_CLONEABLE_RESET is skipped below (its reified lowering cannot host a
     * handler).  So the handler-installing capacity of the program is its count of
     * plain resets.  If that is zero, a performed __Shift can never be caught --
     * whether the program has no reset at all, or only reified (cloneable) resets
     * (e.g. a lexical resuming shift whose receiver transitively performs a
     * cross-function __Shift).  Reject up front (TUR-E0016) rather than let it
     * reach a runtime "Unhandled effect: __Shift" abort. */
    uint32_t n_plain_resets = 0;
    for (uint32_t i = 0; i < e->n_pending_reset_nodes; i++) {
        Expr *node = e->pending_reset_nodes[i];
        if (node && node->kind == EX_RESET) n_plain_resets++;
    }
    if (n_plain_resets == 0) {
        diag_emit_with_code(DIAG_ERROR, e->crossfn_resume_span,
            TUR_E0016_CLONEABLE_SHIFT_OUTSIDE_RESET,
            "a resuming shift (its receiver invokes the continuation) has no "
            "plain enclosing reset to capture the continuation up to -- there is no "
            "delimiter that can catch it\n"
            "  = note: a `reset` that also reifies a *lexical* resuming shift cannot "
            "additionally catch a *cross-function* one (the two lowerings are "
            "incompatible in a single delimiter)\n"
            "  = help: wrap the computation that (transitively) reaches this shift "
            "in a dedicated (reset ...) that has no lexical resuming shift of its "
            "own");
        return;
    }
    for (uint32_t i = 0; i < e->n_pending_reset_nodes; i++) {
        Expr *node = e->pending_reset_nodes[i];
        if (!node) continue;
        /* Only wrap a PLAIN reset (EX_RESET).  An EX_CLONEABLE_RESET has a lexical
         * reified (resuming/cloneable) shift bound to it, and reifying that shift
         * walks the reset body under the narrow build_cloneable grammar -- which a
         * `(handle ...)` wrapper falls outside of (TUR-E0710).  A cross-function
         * resuming shift's delimiter is always a plain reset (the shift is not
         * lexically inside it), so wrapping only EX_RESET catches every
         * cross-function perform while leaving reified resets intact.  (A single
         * reset that both reifies a lexical shift AND must catch a cross-function
         * one is unsupported -- rare, and it would need the reified lowering to
         * tolerate a handler in its delimited context.) */
        if (node->kind == EX_RESET) {
            node->as.reset_.body = wrap_reset_body_with_shift_handler(
                e, node->as.reset_.body, node->span);
        }
    }
}

/* Shared core for the continuation-passing (k-convention) shift: cloneable-shift
 * and a `shift` whose receiver is `cont`-typed (item B).  Takes the
 * already-elaborated receiver `k_expr` so `elab_shift` can dispatch here without
 * re-elaborating.  Routes an ignore-k receiver to the abortive path, else emits
 * the reified EX_CLONEABLE_SHIFT (and records that a resuming shift bound to the
 * enclosing reset, so a plain `reset` promotes itself to a reified delimiter). */
static Expr *elab_cont_shift_core(Elab *e, const Form *call, Expr *k_expr) {
    /* Check if k_expr is a function, closure, or a var referencing a function */
    bool is_function = false;
    if (k_expr->kind == EX_FN || k_expr->kind == EX_CLOSURE) {
        is_function = true;
    } else if (k_expr->kind == EX_VAR) {
        Binding *b = k_expr->as.var.binding;
        if (b && (b->type.kind == TY_FN || b->closure_fn_binding)) {
            is_function = true;
        }
    }
    if (!is_function) {
        diag_emit(DIAG_ERROR, call->as.list.items[1]->span,
                  "cloneable-shift requires a function as first argument");
        return NULL;
    }

    /* Unification: a shift whose receiver provably IGNORES its continuation is an
     * abortive shift.  Lower it via the dynamic-abort path (an abortive EX_SHIFT),
     * which works cross-function and under arbitrary contexts -- no lexical
     * cloneable-reset (TUR-E0016) or build_cloneable-subset context (TUR-E0710).
     * The receiver expects a `cont` but ignores it, so we hand it a null (0)
     * continuation of the right type.  NOT applied to the literal `cloneable-shift`
     * keyword (which keeps its exact reified semantics); applied to
     * a `cont`-typed `shift`. */
    bool abort_route = call->as.list.items[0]->tag == F_SYM
                       && call->as.list.items[0]->as.sym == e->sym_shift;
    Type kdomain, kcodomain;
    if (abort_route && receiver_ignores_continuation(k_expr)
        && shift_fn_domain_codomain(k_expr, &kdomain, &kcodomain)) {
        Expr *zero = expr_new(e->arena, EX_INT_LIT, kdomain, call->span);
        zero->as.i = 0;
        Expr *out = expr_new(e->arena, EX_SHIFT, kcodomain, call->span);
        out->as.shift_.k_fn = k_expr;
        out->as.shift_.body = zero;
        return out;
    }

    /* CPS-CL7: a RESUMING shift must be inside a reset (the reified-context path
     * is lexically scoped).  A plain `reset` counts too (item B): it promotes
     * itself to a reified delimiter when a resuming shift binds to it. */
    if (e->cloneable_reset_depth == 0) {
        /* Capability-folding symmetric twin (cps-cloneable-shift-under-serial-
         * reset-misleading-e0016): a RESUMING cloneable (`cont`) shift with no
         * enclosing plain/cloneable reset but lexically inside a `serial-reset`
         * is NOT a cross-function resume -- there IS an enclosing delimiter, just
         * a serial one whose marshal substrate cannot host an in-memory
         * multi-shot continuation.  Reject with the real flavor-mismatch cause
         * (mirror of the serial-cont-under-cloneable-reset TUR-E0019 above)
         * instead of falling into the cross-function __Shift desugar, which post-
         * elaboration finds no plain EX_RESET and emits the MISLEADING TUR-E0016
         * "no enclosing reset / cross-function resume".  Sound because a plain
         * `reset` and `cloneable-reset` BOTH bump cloneable_reset_depth, so
         * reaching this branch means none intervenes: serial_reset_depth > 0 here
         * => the NEAREST delimiter is the serial-reset (a nested
         * `reset`-in-`serial-reset` bumps cloneable_reset_depth and never reaches
         * here, so it keeps working).  Abortive (ignore-k) shifts return via the
         * abort route above and never reach this branch.  The genuine
         * cross-function case (no enclosing delimiter anywhere) has
         * serial_reset_depth == 0 and keeps its TUR-E0016 path. */
        if (e->serial_reset_depth > 0) {
            diag_emit_with_code(DIAG_ERROR, call->span,
                TUR_E0016_CLONEABLE_SHIFT_OUTSIDE_RESET,
                "a `cont` (cloneable) shift receiver needs a cloneable-capable "
                "delimiter, but the nearest enclosing reset is a `serial-reset` "
                "(whose marshal substrate cannot host an in-memory multi-shot "
                "continuation)\n"
                "  = help: use a plain `reset` (it adopts the receiver's flavor) "
                "or `cloneable-reset` to delimit a cloneable continuation");
            return NULL;
        }
        /* For the `shift` surface, tailor the message: the common cause
         * now is a resuming shift whose reset is in a CALLER (cross-function
         * resume, unsupported -- only cross-function ABORT works).  Keep the exact
         * legacy wording for the literal `cloneable-shift` keyword (error fixture
         * cloneable-shift-outside-reset pins it). */
        bool shift_kw = call->as.list.items[0]->tag == F_SYM
                        && call->as.list.items[0]->as.sym == e->sym_shift;
        /* cps-backend-n6 cross-function resume: a resuming `shift` with no lexical
         * reset lowers onto the synthetic __Shift effect instead of erroring --
         * (shift RECV BODY) -> (perform (__Shift RECV)).  RECV's continuation param
         * must be `multishot-effect-cont` (CONT_EFFECT + CK_MULTISHOT) so `(k v)`
         * inside it resumes -- via the snapshot path -- the delimited continuation
         * the enclosing reset's whole-program-wrapped `^multishot` __Shift handler
         * carries (slice C).  BODY is discarded (the receiver drives the
         * continuation; matching the target encoding).
         *
         * The flavor is read from the receiver's own param binding, so BOTH an
         * inline lambda (elab_shift reflavored its `cont` param before elaboration)
         * AND a named-fn receiver the user annotated `multishot-effect-cont` are
         * accepted.  A plain-`cont` named receiver (CONT_CLONEABLE) or a one-shot
         * `effect-cont` one falls through to a tailored E0016 below. */
        const Type *kpt = receiver_cont_param_type(k_expr);
        bool kpt_effect = kpt && kpt->kind == TY_CONT
                          && (ContFlavor)kpt->as.cont.flavor == CONT_EFFECT;
        bool kpt_multishot = kpt_effect && kpt->copy_kind == CK_MULTISHOT;

        /* A NAMED receiver written with a plain `cont` param (CONT_CLONEABLE)
         * cannot be reflavored in place (its body was elaborated against a
         * cloneable continuation), but its retained source form can be
         * re-elaborated into a `multishot-effect-cont` specialization.  Redirect
         * the receiver to that clone, then fall through to the perform below --
         * this makes a named-fn resuming receiver work cross-function without the
         * user having to annotate its param. */
        if (shift_kw && !kpt_multishot && k_expr->kind == EX_VAR
            && k_expr->as.var.binding && kpt && kpt->kind == TY_CONT
            && (ContFlavor)kpt->as.cont.flavor == CONT_CLONEABLE) {
            Binding *spec = elab_specialize_cont_receiver(e, k_expr->as.var.binding);
            if (spec) {
                Expr *nv = expr_new(e->arena, EX_VAR, spec->type, call->span);
                nv->as.var.binding = spec;
                k_expr = nv;
                kpt = receiver_cont_param_type(k_expr);
                kpt_effect = kpt && kpt->kind == TY_CONT
                             && (ContFlavor)kpt->as.cont.flavor == CONT_EFFECT;
                kpt_multishot = kpt_effect && kpt->copy_kind == CK_MULTISHOT;
            }
        }
        if (shift_kw && kpt_multishot) {
            Effect *sheff = elab_get_shift_effect(e);
            if (sheff && sheff->constructor) {
                /* Box the receiver as the one-word {thunk,env} closure the effect
                 * slot expects (mirrors elab_perform's fn-payload boxing), so a
                 * capturing receiver rides along.  A capturing closure is already
                 * boxed; a bare fn pointer is wrapped via EX_FN_TO_FAT. */
                Expr *recv = k_expr;
                /* Mark a capturing-closure receiver so the CPS backend delegates
                 * its build (CT_LETRAW) even though it captures -- it is only ever
                 * invoked as `(recv k)` in the bridge-wrapping __Shift handler
                 * case, never indirect-called elsewhere.  Scoped to __Shift. */
                if (recv->kind == EX_CLOSURE && recv->as.closure_.closure)
                    recv->as.closure_.closure->is_shift_receiver = true;
                if (recv->type.kind == TY_FN && !recv->type.as.fn.boxed) {
                    Type *bt = (Type *)arena_alloc(e->arena, sizeof(Type));
                    *bt = recv->type;
                    bt->as.fn.boxed = true;
                    Expr *shim = expr_new(e->arena, EX_FN_TO_FAT, *bt,
                                          call->as.list.items[1]->span);
                    shim->as.fn_to_fat_.inner = recv;
                    recv = shim;
                }
                PerformExpr *perform = arena_alloc(e->arena, sizeof(PerformExpr));
                perform->effect_name = sheff->name;
                Expr **pargs = (Expr **)arena_alloc(e->arena, sizeof(Expr *));
                pargs[0] = recv;
                perform->args = pargs;
                perform->n_args = 1;
                perform->resumable_payload = false;  /* __Shift: own admission path */
                Type rt = sheff->constructor->result_full_type
                        ? *sheff->constructor->result_full_type
                        : type_from_kind(sheff->constructor->result_type);
                Expr *out = expr_new(e->arena, EX_PERFORM, rt, call->span);
                out->as.perform_.perform = perform;
                if (!e->uses_crossfn_resume) e->crossfn_resume_span = call->span;
                e->uses_crossfn_resume = true;
                return out;
            }
        }
        if (shift_kw && kpt_effect && !kpt_multishot) {
            /* The receiver's continuation param is a one-shot `effect-cont`, but the
             * synthesized __Shift handler is `^multishot` -- the two disciplines
             * must match (a `^multishot` handler captures a snapshot-capable
             * cloneable cont; a one-shot `(k v)` would resume it as a fiber). */
            diag_emit_with_code(DIAG_ERROR, call->span,
                TUR_E0016_CLONEABLE_SHIFT_OUTSIDE_RESET,
                "a cross-function resuming shift receiver must use a "
                "`multishot-effect-cont` continuation, not a one-shot `effect-cont`\n"
                "  = help: declare the receiver's continuation parameter "
                "`multishot-effect-cont` (single-resume receivers work through it "
                "too), or inline the receiver as (fn [k : cont] ...) to have it "
                "reflavored automatically");
            return NULL;
        }
        if (shift_kw) {
            /* Reached for a resuming receiver whose continuation param is NOT
             * CONT_EFFECT -- e.g. a named-fn receiver with a plain `cont` param,
             * which cannot be reflavored from the call site.  An inline-lambda
             * receiver is reflavored automatically (cross-function RESUME above);
             * a named receiver can opt in by declaring its param
             * `multishot-effect-cont`. */
            diag_emit_with_code(DIAG_ERROR, call->span,
                TUR_E0016_CLONEABLE_SHIFT_OUTSIDE_RESET,
                "a resuming shift (its receiver invokes the continuation) must sit "
                "inside a lexically-enclosing reset\n"
                "  = note: cross-function RESUME (the reset in a caller) works when "
                "the receiver is written inline as (fn [k : cont] ...), or is a "
                "named function whose continuation parameter is declared "
                "`multishot-effect-cont`\n"
                "  = help: inline the receiver as a lambda, declare a named "
                "receiver's continuation param `multishot-effect-cont`, move the "
                "reset to enclose the shift lexically, or use algebraic effects "
                "(perform / handle / resume) directly");
            return NULL;
        }
        diag_emit_with_code(DIAG_ERROR, call->span,
                            TUR_E0016_CLONEABLE_SHIFT_OUTSIDE_RESET,
                            "cloneable-shift used outside of any cloneable-reset boundary");
        return NULL;
    }

    Expr *body = elab_form(e, call->as.list.items[2]);
    if (!body) return NULL;

    /* Capability-folding item 1: preserve the continuation's flavor from the
     * receiver's `cont` capability annotation.  A `serial-cont` receiver
     * (CONT_SERIAL) yields a serial continuation -- lower this shift exactly like
     * `serial-shift` (EX_SERIAL_SHIFT) and mark the enclosing plain `reset` to
     * promote to EX_SERIAL_RESET.  Any other cont flavor (plain `cont` /
     * `cloneable-cont`, CONT_CLONEABLE) keeps the multi-shot cloneable lowering.
     * NOT applied to the literal `cloneable-shift` keyword (which pins the
     * cloneable flavor regardless of the receiver's annotation). */
    bool shift_kw = call->as.list.items[0]->tag == F_SYM
                    && call->as.list.items[0]->as.sym == e->sym_shift;
    const Type *kpt = receiver_cont_param_type(k_expr);
    bool serial_route = shift_kw && kpt && kpt->kind == TY_CONT
                        && (ContFlavor)kpt->as.cont.flavor == CONT_SERIAL;
    if (serial_route) {
        /* Capability-folding item 1: the nearest enclosing delimiter is the
         * literal `cloneable-reset` keyword, which pins the cloneable flavor and
         * cannot host a serial continuation.  Reject here with the actual fix
         * ("use a plain `reset` or `serial-reset`") instead of letting the
         * flavor-mismatched EX_SERIAL_SHIFT reach the downstream serial-context
         * lowering, which fails with the misleading TUR-E0706 "context not
         * capturable" (it points at the context shape, not the real cause). */
        int cd = e->cloneable_reset_depth;
        if (cd >= 0 && cd < 64 && e->pinned_cloneable_at_depth[cd]) {
            diag_emit_with_code(DIAG_ERROR, call->span,
                TUR_E0019_SERIAL_SHIFT_OUTSIDE_RESET,
                "a `serial-cont` shift receiver needs a serial-capable delimiter, "
                "but the nearest enclosing reset is a `cloneable-reset` (which pins "
                "the multi-shot cloneable flavor)\n"
                "  = help: use a plain `reset` (it adopts the receiver's flavor) or "
                "`serial-reset` to delimit a serial continuation");
            return NULL;
        }
        Expr *out = expr_new(e->arena, EX_SERIAL_SHIFT, body->type, call->span);
        out->as.serial_shift_.k_fn = k_expr;
        out->as.serial_shift_.body = body;
        if (e->cloneable_reset_depth >= 0 && e->cloneable_reset_depth < 64) {
            e->reified_shift_at_depth[e->cloneable_reset_depth] = true;
            e->reified_serial_at_depth[e->cloneable_reset_depth] = true;
        }
        return out;
    }

    Expr *out = expr_new(e->arena, EX_CLONEABLE_SHIFT, body->type, call->span);
    out->as.cloneable_shift_.k_fn = k_expr;
    out->as.cloneable_shift_.body = body;
    out->as.cloneable_shift_.cont_body = NULL;
    /* Item B: mark the enclosing reset (at this depth) as needing the reified
     * delimiter, so a plain `reset` becomes EX_CLONEABLE_RESET. */
    if (e->cloneable_reset_depth >= 0 && e->cloneable_reset_depth < 64)
        e->reified_shift_at_depth[e->cloneable_reset_depth] = true;
    /* CPS-CL10 / E4: the Clone-capture check now runs once per enclosing reset
     * (check_cloneable_capture_precise), on the full reset body, so it can tell
     * a genuinely-captured owning value from one merely in scope.  See the
     * enclosing reset elaborators (elab_reset / elab_cloneable_reset). */
    return out;
}

Expr *elab_cloneable_shift(Elab *e, const Form *call) {
    if (call->as.list.len != 3) {
        diag_emit(DIAG_ERROR, call->span,
                  "(cloneable-shift k body) requires exactly two arguments");
        return NULL;
    }
    Expr *k_expr = elab_form(e, call->as.list.items[1]);
    if (!k_expr) return NULL;
    /* The literal `cloneable-shift` keyword never routes onto the __Shift effect
     * (it keeps its exact reified semantics); its receiver is a CONT_CLONEABLE
     * `cont`, so the CONT_EFFECT perform route in the core never fires for it. */
    return elab_cont_shift_core(e, call, k_expr);
}

/* (call/cc* f) - multi-shot call/cc that produces a cloneable continuation.
 *
 * CPS-CL8 / cps-transform-plan (CPS11): the continuation f receives is the one
 * captured up to the nearest *enclosing* cloneable-reset:
 *
 *   - Inside a cloneable-reset (cloneable_reset_depth > 0): desugar to a BARE
 *     (cloneable-shift f 0) that binds to that enclosing reset. The captured
 *     continuation is the real delimited context between the call/cc* and its
 *     reset -- e.g. (cloneable-reset (+ 10 (call/cc* f))) hands f a continuation
 *     that computes (+ 10 []) and is replayable multi-shot (CPS9 lowers it onto
 *     the DK machine).
 *   - With no enclosing cloneable-reset: desugar to (cloneable-reset
 *     (cloneable-shift f 0)) -- a freshly-installed delimiter whose captured
 *     continuation is empty/trivial. This is the original sugar and keeps
 *     call/cc* usable on its own (no reset required), matching the existing
 *     call-cc-star semantics (k is the identity continuation). */
Expr *elab_call_cc_star(Elab *e, const Form *call) {
    if (call->as.list.len != 2) {
        diag_emit(DIAG_ERROR, call->span,
                  "(call/cc* f) requires exactly one argument");
        return NULL;
    }
    Expr *f_expr = elab_form(e, call->as.list.items[1]);
    if (!f_expr) return NULL;

    /* The shift emitter allocates a continuation and passes it to k_fn (= f),
     * so f receives the continuation directly -- no wrapper needed. */

    /* Determine f's return type */
    Type call_result_type = TYPE_INT;
    if (f_expr->kind == EX_VAR && f_expr->as.var.binding
        && f_expr->as.var.binding->type.kind == TY_FN) {
        call_result_type = type_from_kind(
            f_expr->as.var.binding->type.as.fn.result_kind);
    }

    /* Build EX_INT_LIT for the default value 0 */
    Expr *zero = expr_new(e->arena, EX_INT_LIT, TYPE_INT, call->span);
    zero->as.i = 0;

    /* Build EX_CLONEABLE_SHIFT: (cloneable-shift f 0) */
    Expr *shift = expr_new(e->arena, EX_CLONEABLE_SHIFT, call_result_type, call->span);
    shift->as.cloneable_shift_.k_fn = f_expr;
    shift->as.cloneable_shift_.body = zero;
    shift->as.cloneable_shift_.live_captures = NULL;
    shift->as.cloneable_shift_.n_live_captures = 0;
    shift->as.cloneable_shift_.cont_body = NULL;

    /* Inside an enclosing cloneable-reset: capture up to it (real call/cc*
     * semantics). The bare shift binds to that reset's prompt. */
    if (e->cloneable_reset_depth > 0) {
        return shift;
    }

    /* No enclosing reset: install a fresh delimiter (trivial continuation). */
    Expr *reset = expr_new(e->arena, EX_CLONEABLE_RESET, call_result_type, call->span);
    reset->as.cloneable_reset_.body = shift;
    return reset;
}

/* Phase 21: Serializable continuations */

/* E4a (mirrors check_cloneable_capture_precise): emit TUR-E0018 for a binding
 * CAPTURED into a serial-shift's continuation that lacks a Serializable
 * instance.  Runs once per serial-reset, on the full reset body, and flags only
 * the bindings that are FREE in that body -- the ones the continuation actually
 * captures.  The old per-shift check ran before the body existed and had to
 * over-approximate to EVERY in-scope binding, so a non-Serializable value merely
 * being in scope at a serial-shift (never captured) was spuriously rejected.
 * The scope walk keeps its original reach (to &e->global -- serial captures may
 * include enclosing-fn bindings); only the free-variable gate is new. */
static void check_serializable_capture_precise(Elab *e, Span span,
                                               const Expr *reset_body) {
    const Symbol *ser_sym = intern_cstr(e->st, "Serializable");
    TypeClass *ser_tc = typeclass_env_lookup_typeclass(&e->typeclass_env, ser_sym);
    if (!ser_tc) return; /* Serializable not yet in scope; defer to a later pass */
    if (!reset_body) return;

    /* Free variables of the reifiable continuation context -- a binding not in
     * this set is never captured, so it needs no Serializable instance. */
    uint32_t n_fv = 0;
    Binding **fvs = collect_free_vars(reset_body, NULL, 0, NULL, 0, &n_fv);

    for (Scope *s = e->scope; s != NULL && s != &e->global; s = s->parent) {
        for (uint32_t i = 0; i < s->n; i++) {
            Binding *b = s->bindings[i];
            if (!b || !b->name) continue;
            Type t = b->type;
            /* Primitive types that are always serializable */
            if (t.kind == TY_NIL || t.kind == TY_FN ||
                t.kind == TY_CLONEABLE_CONT || t.kind == TY_CONT) continue;
            /* Only a binding the continuation actually references is captured. */
            bool captured = false;
            for (uint32_t j = 0; j < n_fv; j++)
                if (fvs[j] == b) { captured = true; break; }
            if (!captured) continue;
            TypeClassInstance *inst =
                typeclass_env_lookup_instance(&e->typeclass_env, ser_tc, &t, 1);
            if (!inst) {
                diag_emit_with_code(DIAG_ERROR, span,
                                    TUR_E0018_NOT_SERIALIZABLE,
                                    "captured binding '%s' does not implement Serializable "
                                    "(required by serial-shift)\n"
                                    "  = help: use serial-reset outside non-serializable "
                                    "resources, or implement Serializable for the type",
                                    b->name->name);
            }
        }
    }
    if (fvs) free(fvs);
}

/* (serial-reset body) - Establish a serializable continuation boundary.
 * Like reset, but marks the region so serial-shift can capture it. */
Expr *elab_serial_reset(Elab *e, const Form *call) {
    /* Variadic body -- see elab_reset. */
    if (call->as.list.len < 2) {
        diag_emit(DIAG_ERROR, call->span,
                  "(serial-reset body ...) requires at least one body expression");
        return NULL;
    }
    e->serial_reset_depth++;
    Expr *body = elab_implicit_do(e, call, 1);
    e->serial_reset_depth--;
    if (!body) return NULL;
    /* E4a: verify captures of the reified serial continuation body. */
    check_serializable_capture_precise(e, call->span, body);
    Expr *out = expr_new(e->arena, EX_SERIAL_RESET, body->type, call->span);
    out->as.serial_reset_.body = body;
    return out;
}

/* (serial-shift k body) - Capture the current continuation as a serializable one.
 * k is a function that receives the serial-continuation as its argument.
 * All captured environment values must implement Serializable. */
Expr *elab_serial_shift(Elab *e, const Form *call) {
    if (call->as.list.len != 3) {
        diag_emit(DIAG_ERROR, call->span,
                  "(serial-shift k body) requires exactly two arguments");
        return NULL;
    }

    if (e->serial_reset_depth == 0) {
        diag_emit_with_code(DIAG_ERROR, call->span,
                            TUR_E0019_SERIAL_SHIFT_OUTSIDE_RESET,
                            "serial-shift used outside of any serial-reset boundary");
        return NULL;
    }

    Expr *k_expr = elab_form(e, call->as.list.items[1]);
    if (!k_expr) return NULL;

    bool is_function = false;
    if (k_expr->kind == EX_FN || k_expr->kind == EX_CLOSURE) {
        is_function = true;
    } else if (k_expr->kind == EX_VAR) {
        Binding *b = k_expr->as.var.binding;
        if (b && (b->type.kind == TY_FN || b->closure_fn_binding)) {
            is_function = true;
        }
    }
    if (!is_function) {
        diag_emit(DIAG_ERROR, call->as.list.items[1]->span,
                  "serial-shift requires a function as first argument");
        return NULL;
    }

    Expr *body = elab_form(e, call->as.list.items[2]);
    if (!body) return NULL;

    Expr *out = expr_new(e->arena, EX_SERIAL_SHIFT, body->type, call->span);
    out->as.serial_shift_.k_fn = k_expr;
    out->as.serial_shift_.body = body;

    /* E4a: the Serializable-capture check now runs once per enclosing serial-
     * reset (check_serializable_capture_precise), on the full reset body, so a
     * value merely in scope but not captured is no longer flagged.  See
     * elab_serial_reset. */
    return out;
}

/* (try-with body (EffectName [params] k) handler ...)
 * Sugar: identical to (handle body ...).
 * Provided for OCaml/algebraic-effects familiarity.
 */
Expr *elab_try_with(Elab *e, const Form *call) {
    /* try-with has the same surface syntax as handle; delegate directly. */
    return elab_handle(e, call);
}

/* (defeffect Name [param1 : T1, param2 : T2, ...] : R)
 * (defeffect ^private Name [param1 : T1, param2 : T2, ...] : R)
 * Declares a new algebraic effect with parameters and a result type.
 * The optional ^private annotation restricts the effect to the defining module.
 */
Expr *elab_defeffect(Elab *e, const Form *call) {
    /* Minimum: (defeffect Name [params...] :ret-type)
     * Or with visibility: (defeffect ^private Name [params...] :ret-type) */
    if (call->as.list.len < 4) {
        diag_emit(DIAG_ERROR, call->span,
                  "defeffect requires (defeffect Name [params...] result-type)");
        return NULL;
    }

    /* Phase P19-6: Parse optional ^private visibility annotation */
    bool is_private = false;
    uint32_t name_idx = 1;  /* index of the effect name form */
    if (call->as.list.len >= 5) {
        Form *maybe_private = call->as.list.items[1];
        if (maybe_private->tag == F_SYM
            && maybe_private->as.sym == e->sym_caret_private) {
            is_private = true;
            name_idx = 2;
        }
    }

    /* Parse effect name */
    Form *name_f = call->as.list.items[name_idx];
    if (name_f->tag != F_SYM) {
        diag_emit(DIAG_ERROR, name_f->span, "defeffect: effect name must be a symbol");
        return NULL;
    }
    const Symbol *name = name_f->as.sym;
    
    /* Check if effect already exists.  PS4: an earlier REPL/playground turn's
     * effect is redefined in place (below, once the new signature is parsed) --
     * re-running the shipped effects example must not fail on the effect the
     * previous run declared.  The builtin Unsafe is never redefinable. */
    bool redefining = false;
    /* effect-row-honesty-plan W2: IO / FS / Net / Proc / Rand are
     * compiler-known.  A declaration of one is accepted when it declares
     * exactly the built-in -- that is what stdlib/effects.tur does, so its
     * docs stay where they are -- and checked against it once the signature
     * is parsed, below. */
    const char *builtin_cap_parent = NULL;
    bool declaring_builtin_cap =
        effect_builtin_capability(name->name, &builtin_cap_parent);
    if (declaring_builtin_cap) {
        /* fall through: validated after the signature is parsed */
    } else if (effect_env_contains(e->effect_env, name)) {
        if (name != e->sym_effect_unsafe && elab_prior_turn_effect(e, name)) {
            redefining = true;
        } else {
            diag_emit(DIAG_ERROR, name_f->span,
                      "defeffect: '%s' is already defined", name->name);
            return NULL;
        }
    }
    
    /* Parse parameter list (index shifts by 1 when ^private is present) */
    Form *params_f = call->as.list.items[name_idx + 1];
    if (params_f->tag != F_LIST && params_f->tag != F_VEC) {
        diag_emit(DIAG_ERROR, params_f->span,
                  "defeffect: expected parameter list, got %s",
                  form_tag_name(params_f->tag));
        return NULL;
    }
    
    /* Parse parameter list.
     * Accepts:
     *   []          — no params
     *   [x y]       — untyped params (default to TY_INT)
     *   [x :int y :cstr] — typed params (name :type pairs)
     */
    uint8_t raw_n = (uint8_t)params_f->as.list.len;
    /* Count actual params (skip type keyword items) */
    uint8_t n_params = 0;
    for (uint8_t i = 0; i < raw_n; i++) {
        Form *f = params_f->as.list.items[i];
        if (f->tag == F_SYM) n_params++;
        /* F_KEYWORD items are type annotations, not params */
    }
    const Symbol **param_names = arena_alloc(e->arena, n_params * sizeof(const Symbol *));
    TypeKind *param_types = arena_alloc(e->arena, n_params * sizeof(TypeKind));
    /* Tier C: the full param Types, when they are aggregates whose bare TypeKind
     * loses the def.  Entries stay NULL for scalars; the whole array is NULL if no
     * param is an aggregate (set after the loop). */
    const Type **param_full = arena_alloc(e->arena, (n_params ? n_params : 1) * sizeof(const Type *));
    for (uint32_t j = 0; j < n_params; j++) param_full[j] = NULL;
    bool any_agg_param = false;
    int resumable_payload = -1;   /* index of a `(fn [effect-cont] R)` payload */

    {
        uint8_t p = 0;
        for (uint8_t i = 0; i < raw_n; i++) {
            Form *param_f = params_f->as.list.items[i];
            if (param_f->tag == F_KEYWORD || param_f->tag == F_TYPE_ANN) {
                /* Type annotation for preceding param — already handled below */
                continue;
            }
            if (param_f->tag != F_SYM) {
                diag_emit(DIAG_ERROR, param_f->span,
                          "defeffect: parameter name must be a symbol");
                return NULL;
            }
            param_names[p] = param_f->as.sym;
            /* Check if the next item is a type keyword or F_TYPE_ANN */
            /* saffron-defeffect-params-default-to-int: an unannotated parameter
             * takes the file's own default -- `any` under `#lang saffron`, `int`
             * otherwise -- the same one a `defn` parameter takes.  It used to be
             * a bare TY_INT here, so `(defeffect Log [msg] : int)` in a Saffron
             * file silently typed `msg` as `int` and `(perform (Log "hi"))`
             * printed the cstr's address.  The RESULT type is a hard error when
             * missing, so only this side ever defaulted. */
            TypeKind pk = saffron_default_param_kind(param_f->span);
            Type *ann = NULL;
            Form *type_form = NULL;   /* the param's type form, for cont-payload scan */
            if (i + 1 < raw_n) {
                Form *next = params_f->as.list.items[i + 1];
                type_form = next;
                if (next->tag == F_KEYWORD) {
                    pk = typekind_from_symbol(next->as.sym->name);
                    if (pk == TY_UNKNOWN) {
                        /* Tier C: a user type name (by-value struct/ADT). */
                        Form sym_view = *next;
                        sym_view.tag = F_SYM;
                        ann = type_expr_from_form(e, &sym_view, NULL, NULL, NULL, 0);
                        pk = ann ? ann->kind : TY_INT;
                    }
                    i++; /* Consume the type keyword */
                } else if (next->tag == F_TYPE_ANN) {
                    if (next->as.list.len > 0) {
                        ann = type_expr_from_form(e, next->as.list.items[0], NULL, NULL, NULL, 0);
                        if (ann) pk = ann->kind;
                    }
                    i++; /* Consume the type annotation */
                }
            }
            param_types[p] = pk;
            /* Preserve the FULL param Type when the bare TypeKind loses structure
             * the handler needs: an aggregate (ADT/APP/STRUCT -- keeps the def for
             * field access) or a function (TY_FN -- keeps arity + param/result
             * types, so a fn-valued payload `recv` is callable as `(recv k)` in the
             * handler; without this it degrades to a 0-arity uncallable fn). */
            if (ann && (ann->kind == TY_ADT || ann->kind == TY_APP
                        || ann->kind == TY_STRUCT || ann->kind == TY_FN)) {
                Type *stored = arena_alloc(e->arena, sizeof(Type));
                *stored = *ann;
                /* A fn payload is carried as a BOXED closure (a one-word `void *`
                 * to the heap `{thunk, env}` box), so a CAPTURING receiver's env
                 * rides along and fits the one-word effect slot -- unlike the fat
                 * `tur_poly_fn_t` (two words) or the bare pointer (loses the env). */
                if (stored->kind == TY_FN) stored->as.fn.boxed = true;
                param_full[p] = stored;
                any_agg_param = true;
                /* cps-dk-multishot-user-effects (Phase A): a `(fn [effect-cont] R)`
                 * payload marks this a resumable-payload effect (resumed through the
                 * payload).  Detect from the type FORM -- the cont flavor collapses
                 * to TY_INT in the stored Type's arg_kinds. */
                if (stored->kind == TY_FN && resumable_payload < 0
                    && form_type_is_cont_payload(e, type_form))
                    resumable_payload = (int)p;
            }
            p++;
        }
    }
    
    /* Parse return type.
     * v1 accepts both historical symbol syntax and keyword syntax:
     *   (defeffect E [] int)
     *   (defeffect E [] :int)
     */
    Form *ret_f = call->as.list.items[name_idx + 2];
    if (ret_f->tag != F_SYM && ret_f->tag != F_KEYWORD && ret_f->tag != F_TYPE_ANN) {
        diag_emit(DIAG_ERROR, ret_f->span,
                  "defeffect: return type annotation must be a symbol or keyword like :int");
        return NULL;
    }

    TypeKind result_type;
    Type *result_ann = NULL;   /* Tier C: full result Type when resolved */
    if (ret_f->tag == F_TYPE_ANN) {
        result_ann = (ret_f->as.list.len > 0)
            ? type_expr_from_form(e, ret_f->as.list.items[0], NULL, NULL, NULL, 0)
            : NULL;
        result_type = result_ann ? result_ann->kind : TY_UNKNOWN;
    } else {
        result_type = typekind_from_symbol(ret_f->as.sym->name);
        if (result_type == TY_UNKNOWN) {
            /* Tier C: not a builtin type keyword -- try resolving it as a
             * user type name (a by-value struct/ADT).  A keyword `:Pr` and a
             * bare symbol `Pr` both carry the name in as.sym; type_expr_from_form
             * resolves an F_SYM, so present a symbol view of the form. */
            Form sym_view = *ret_f;
            sym_view.tag = F_SYM;
            result_ann = type_expr_from_form(e, &sym_view, NULL, NULL, NULL, 0);
            if (result_ann) result_type = result_ann->kind;
        }
    }
    if (result_type == TY_UNKNOWN) {
        diag_emit(DIAG_ERROR, ret_f->span,
                  "defeffect: unknown return type '%s'", ret_f->as.sym->name);
        return NULL;
    }
    
    /* ET4: Parse optional ^extends ParentName after the return type.
     * stdlib-effect-rows: also parse the standalone ^capability flag, which may
     * appear anywhere among the trailing attributes (it takes no argument). */
    const Symbol *parent_name = NULL;
    bool is_capability = false;
    uint32_t extends_start = name_idx + 3; /* items after (defeffect [^private] Name [params] :ret) */
    for (uint32_t xi = extends_start; xi < (uint32_t)call->as.list.len; xi++) {
        Form *f = call->as.list.items[xi];
        if (f->tag == F_SYM && f->as.sym == e->sym_caret_capability) {
            is_capability = true;
            continue;
        }
        if (f->tag == F_SYM && f->as.sym == e->sym_caret_extends) {
            if (xi + 1 >= (uint32_t)call->as.list.len) {
                diag_emit(DIAG_ERROR, f->span,
                          "defeffect: expected effect name after ^extends");
                return NULL;
            }
            Form *pname_f = call->as.list.items[xi + 1];
            if (pname_f->tag != F_SYM) {
                diag_emit(DIAG_ERROR, pname_f->span,
                          "defeffect: expected effect name after ^extends");
                return NULL;
            }
            parent_name = pname_f->as.sym;
            xi++; /* consume the parent name */
        }
    }

    if (declaring_builtin_cap) {
        bool parent_ok = builtin_cap_parent
            ? (parent_name && strcmp(parent_name->name, builtin_cap_parent) == 0)
            : (parent_name == NULL);
        if (n_params != 0 || result_type != TY_NIL || !is_capability ||
            !parent_ok || is_private) {
            char want[96];
            snprintf(want, sizeof(want), "(defeffect %s [] :nil%s%s ^capability)",
                     name->name, builtin_cap_parent ? " ^extends " : "",
                     builtin_cap_parent ? builtin_cap_parent : "");
            diag_emit(DIAG_ERROR, name_f->span,
                      "defeffect: '%s' is a compiler-known capability effect; "
                      "a declaration of it must match the built-in exactly, "
                      "%s -- or leave it out, since '%s' needs no declaration",
                      name->name, want, name->name);
            return NULL;
        }
    }
    if (redefining)
        effect_redefine(effect_env_lookup(e->effect_env, name), param_names,
                        param_types, n_params, result_type,
                        e->current_module_name, is_private);
    /* Register the effect — pass module visibility info (Phase P19-6) */
    Effect *effect = effect_env_register(e->effect_env, e->arena, name,
                                          param_names, param_types, n_params, result_type,
                                          e->current_module_name, is_private);
    if (!effect) return NULL;
    effect->is_capability = is_capability;
    /* saffron-effect-row-lost-through-unannotated-call: this unit declares an
     * effect, so the dynamic-node operand hoist's CALL half is live for it.
     * Set here rather than counted off the effect env, which always holds the
     * built-ins.
     *
     * NOT for a defeffect read during the stdlib autoload: `stdlib/trail.tur`
     * declares one, and it is autoloaded into EVERY program, so an ungarded
     * flag was set unconditionally and the gate gated nothing.  That is not a
     * theoretical looseness -- it is what regressed `any-widen-frame-box` and
     * `saffron-dyn-field` under the leak harness, by applying the hoist to
     * programs (typed ones, with no effect of their own) that had no use for
     * it.  `in_stdlib_load` is the same window that stamps `is_from_stdlib`. */
    if (!e->in_stdlib_load) e->unit_has_user_effect = true;
    /* Tier C: record the full result Type when it is a by-value aggregate whose
     * bare TypeKind loses the def -- perform reads it so the perform result
     * carries the real monomorphized type. */
    if (effect->constructor && result_ann &&
        (result_ann->kind == TY_ADT || result_ann->kind == TY_APP ||
         result_ann->kind == TY_STRUCT)) {
        Type *stored = arena_alloc(e->arena, sizeof(Type));
        *stored = *result_ann;
        effect->constructor->result_full_type = stored;
    }
    /* Tier C (P5): record the full parameter Types when any param is an aggregate,
     * so the handler-case param binding carries the real def (a def-less TY_ADT
     * would fail field access in the case body). */
    if (effect->constructor && any_agg_param)
        effect->constructor->param_full_types = param_full;
    /* cps-dk-multishot-user-effects (Phase A): record a resumable fn-payload param
     * so perform reflavors the payload cont to multishot and handle upgrades `k`. */
    if (effect->constructor)
        effect->constructor->resumable_payload_param = resumable_payload;

    /* ET4: Resolve parent effect if ^extends was specified */
    if (parent_name) {
        Effect *parent_eff = effect_env_lookup(e->effect_env, parent_name);
        if (!parent_eff) {
            diag_emit(DIAG_ERROR, name_f->span,
                      "defeffect: unknown parent effect '%s' in ^extends clause",
                      parent_name->name);
            return NULL;
        }
        effect->parent = parent_eff;
    }

    /* Create the effect definition expression */
    EffectDef *def = arena_alloc(e->arena, sizeof(EffectDef));
    def->name = name;
    def->param_names = param_names;
    def->param_types = param_types;
    def->n_params = n_params;
    def->result_type = result_type;
    def->is_private = is_private;
    def->defining_module_name = e->current_module_name;
    def->parent_name = parent_name;  /* ET4: ^extends parent effect name */
    def->is_capability = is_capability;  /* stdlib-effect-rows */

    Expr *out = expr_new(e->arena, EX_DEFECT, TYPE_NIL, call->span);
    out->as.effect_def_.def = def;
    return out;
}

/* Phase 19 TUR-E0008: Helpers for unhandled-effect scope tracking. */
static bool is_effect_handled(Elab *e, const Symbol *name) {
    for (uint32_t i = 0; i < e->n_handled_effects; i++) {
        if (e->handled_effect_names[i] == name) return true;
    }
    return false;
}

static void push_handled_effect(Elab *e, const Symbol *name) {
    if (e->n_handled_effects >= e->cap_handled_effects) {
        e->cap_handled_effects = e->cap_handled_effects ? e->cap_handled_effects * 2 : 8;
        e->handled_effect_names = (const Symbol **)realloc(
            e->handled_effect_names,
            e->cap_handled_effects * sizeof(const Symbol *));
    }
    e->handled_effect_names[e->n_handled_effects++] = name;
}

/* PR5-3-D: Check if an effect was explicitly imported via :refer [(effect Name)]. */
static bool elab_effect_is_referred(const Elab *e, const Effect *eff) {
    for (uint32_t i = 0; i < e->n_referred_effects; i++) {
        if (e->referred_effects[i] == eff) return true;
    }
    return false;
}

/* (perform (EffectName arg1 arg2 ...))
 * Perform an algebraic effect with arguments.
 */
/* perform-does-not-typecheck-its-arguments: the memory-safety half of the
 * general check.
 *
 * `elab_perform` elaborated each argument and stored it in the effect slot
 * without ever consulting the effect's declared parameter types, so
 * `(defeffect Log [msg : cstr] : int)` performed as `(perform (Log 42))`
 * compiled clean -- `tur check` and `tur emit-c` both exit 0 -- and segfaulted
 * when the handler dereferenced the integer 42 as a `char *`.
 *
 * The GENERAL check is not a one-liner and is not attempted here: an ordinary
 * call decides argument compatibility across ~300 lines of `elab_call_fn_inner`
 * (implicit coercions, borrows, type variables, HKT carriers, by-value
 * aggregates, the `any` seam), threaded through a mutable `arg_ok` with
 * coercion side effects, and a naive `kind ==` at the perform site would reject
 * a large amount of code that is correct today.  Factoring that decision into a
 * predicate both sites can share is the real fix and is still worth doing.
 *
 * What this closes is the shape the segfault takes and the one shape that
 * admits no legitimate program: a SCALAR word (an integer, a bool, a float)
 * arriving at a POINTER-shaped parameter, where the handler's only possible
 * reading of the slot is as an address.  Every other combination is left to the
 * general check, so this cannot reject anything that works today.
 *
 * Deliberately NOT pointer-shaped here: TY_STRUCT / TY_ADT (by-value aggregates
 * whose slot is not an address), TY_FN (a fn-typed param takes the boxing shim
 * a few lines below), TY_TYVAR / TY_ANY / TY_UNKNOWN (no declared shape to
 * disagree with).  And NOT scalar: TY_NIL, which an ordinary call already
 * accepts for a pointer parameter, and TY_NEVER.
 *
 * ...and the argument must be a LITERAL, which is narrower than the filed fix
 * direction proposed ("reject a scalar argument reaching a pointer-shaped
 * parameter ... admits no legitimate program").  It does admit one, and the
 * fixture corpus found it in seven places: an UNANNOTATED lambda parameter
 * defaults to `int` at elaboration and is refined later, so
 * `(fn [msg] (perform (Log msg)))` against `Log [msg : cstr]` reads as an int
 * argument at this point and is perfectly correct --
 * `tests/fixtures/effect-type-alias` and `cps-backend-fn-param-effectful` are
 * two of them.  A literal's type is never provisional, so restricting to one
 * gives a rule with no false positives at all, at the cost of missing a
 * mismatch that arrives through a variable.  Widening this wants the same
 * factored-out `arg_ok` predicate the general check wants -- not a better
 * guess here.
 *
 * `is_literal_arg` peels the representation wrappers elaboration may already
 * have put on the literal, so `(perform (Log 42))` is still recognised when
 * the 42 arrives boxed or ascribed. */
static bool perform_param_is_pointer_shaped(const Type *t) {
    if (!t) return false;
    switch (t->kind) {
        case TY_CSTR:
        case TY_PTR_VOID:
        case TY_REF:
        case TY_RC:
        case TY_WEAK:
        case TY_REF_IMMUT:
        case TY_REF_MUT:
            return true;
        default:
            return false;
    }
}

/* Did this argument's type come from a parameter the author never annotated --
 * the carrier default (`saffron_default_param_kind`: TY_INT in a typed file)
 * whose TY_INT means "one machine word", not "an integer"?
 *
 * This used to SUPPRESS the check, so that `(fn [msg] (perform (Log msg)))`
 * against `Log [msg : cstr]` compiled.  It no longer does: an ordinary call
 * rejects the very same value (`function 'takes-cstr' arg 1: expected cstr, got
 * int`), and a `perform` that accepted it was the last place the effect ABI was
 * laxer than the language around it.
 *
 * What the flag is for now is the DIAGNOSTIC.  A mismatch reported on a
 * carrier-defaulted parameter is not fixed by changing the argument -- the
 * value is already the right thing at run time -- but by writing the annotation
 * that was missing, which is a different instruction from the one an ordinary
 * type error gives.  So the check fires either way and this only decides
 * whether the message says so.
 *
 * An un-annotated `let` bound directly to such a parameter inherits the
 * provenance (elab_let), so `(let [m msg] (perform (Log m)))` gets the same
 * advice one indirection along. */
static bool perform_arg_is_unannotated_carrier(const Expr *a) {
    while (a) {
        switch (a->kind) {
            case EX_VAR:
                return a->as.var.binding &&
                       a->as.var.binding->type_is_carrier_default;
            case EX_ASCRIBE:     a = a->as.ascribe_.inner;    break;
            case EX_CAST:        a = a->as.cast_.expr;        break;
            case EX_REINTERPRET: a = a->as.reinterpret_.expr; break;
            default:             return false;
        }
    }
    return false;
}

/* perform-does-not-typecheck-its-arguments, second pass: the PRIMITIVE half of
 * the general check, which turned out not to need the `arg_ok` factoring at all.
 *
 * The filing assumed a perform-site check "has to agree with what an ordinary
 * call already accepts", and that agreeing meant reproducing ~300 lines of
 * `elab_call_fn_inner`.  Measuring what a call actually accepts for PRIMITIVES
 * shows those 300 lines are about type variables, HKT carriers, by-value
 * aggregates, borrows and fn values -- not scalars.  Between two plain
 * primitives a call is simply exact TypeKind equality, with two alias pairs and
 * no implicit widening whatsoever:
 *
 *     (defn f [n : int] ...)      (f 7.25)      rejected: expected int, got float
 *     (defn f [x : float] ...)    (f 7)         rejected: expected float, got int
 *     (defn f [n : int] ...)      (f "hi")      rejected: expected int, got cstr
 *     (defn f [n : int64] ...)    int arg       ACCEPTED   (int   == int64)
 *     (defn f [x : float64] ...)  float arg     ACCEPTED   (float == float64)
 *     (defn f [n : int] ...)      int8 arg      rejected
 *     (defn f [n : int] ...)      bool arg      rejected
 *
 * So `perform` can have the same rule for the same shapes, verifiably rather
 * than by imitation.  `perform_primitive_norm` returns the normalized kind for
 * a plain primitive and TY_UNKNOWN for everything else, so the check fires only
 * when BOTH sides are plain primitives and stays silent everywhere the call
 * path has a coercion arm.
 *
 * Deliberately NOT plain primitives: TY_PTR_VOID (a call accepts TY_FN and
 * TY_NIL for it), TY_NIL (accepted for pointer parameters), TY_SYM, TY_NEVER,
 * and every aggregate / fn / tyvar / any.  Those keep the narrower
 * scalar-into-pointer rule below, or no rule at all. */
static TypeKind perform_primitive_norm(TypeKind k) {
    switch (k) {
        case TY_INT:   case TY_INT64:   return TY_INT;
        case TY_FLOAT: case TY_FLOAT64: return TY_FLOAT;
        case TY_BOOL:
        case TY_CSTR:
        case TY_INT8:  case TY_INT16:  case TY_INT32:
        case TY_UINT8: case TY_UINT16: case TY_UINT32: case TY_UINT64:
        case TY_FLOAT32:
            return k;
        default:
            return TY_UNKNOWN;
    }
}

static bool perform_arg_is_scalar_word(const Type *t) {
    if (!t) return false;
    switch (t->kind) {
        case TY_BOOL:
        case TY_INT:
        case TY_INT8:  case TY_INT16:  case TY_INT32:  case TY_INT64:
        case TY_UINT8: case TY_UINT16: case TY_UINT32: case TY_UINT64:
        case TY_FLOAT: case TY_FLOAT32: case TY_FLOAT64:
            return true;
        default:
            return false;
    }
}

/* perform-does-not-typecheck-its-arguments, third pass: the AGGREGATE and
 * POINTER shapes the primitive rule above deliberately left alone.  Measured
 * against an ordinary call the same way the primitive pass was, one shape per
 * program, so each row below is a call's verdict and not an imitation of
 * `arg_ok`:
 *
 *     param             argument              call
 *     Point             Other / 5 / "hi" / cb / nil   rejected
 *     Point             (Point 1 2)           accepted
 *     (Option int)      5 / (some "x")        rejected
 *     (Option int)      (some 1)              accepted
 *     H (opaque)        ptr<void> / 5         rejected
 *     ptr<void>         "hi" / Point / (some 1) / H / :Sym / bool
 *                                             rejected
 *     ptr<void>         cb / nil / ptr<int>   accepted
 *     ptr<int>          "hi"                  rejected
 *     cstr              cb / nil / Point      rejected
 *     float / bool      Point                 rejected
 *     int               Point / (Circle 1)    ACCEPTED (the carrier word)
 *
 * A `perform` accepted every rejected row silently: `(perform (E (Other 1)))`
 * against `E [p : Point]` read `Other`'s first field as `.x` and printed it.
 * So the rule here is exactly that table.  It stays silent wherever the call
 * path has an inference arm this site cannot reproduce -- an argument or
 * parameter that still mentions a type variable (a generic body's `x : A`,
 * the W2 `(vec-new)` into `(Vec int)` unification), `any` (unboxed above),
 * `!`, unknown -- and for an `int` parameter, which is the carrier word and
 * accepts an aggregate at a call too. */
static bool perform_type_mentions_named_tyvar(const Type *t) {
    if (!t) return false;
    switch (t->kind) {
        case TY_TYVAR: return t->as.tyvar_.name != NULL;
        case TY_APP:
            return perform_type_mentions_named_tyvar(t->as.app.fn) ||
                   perform_type_mentions_named_tyvar(t->as.app.arg);
        default: return false;
    }
}
static bool perform_type_is_aggregate(const Type *t) {
    if (!t) return false;
    switch (t->kind) {
        case TY_STRUCT: return true;
        case TY_ADT:    return t->as.adt_.def != NULL;
        case TY_APP:    return type_adt_app_def(t) != NULL;
        default:        return false;
    }
}
static bool perform_arg_shape_mismatch(const Type *want, const Type *got) {
    if (!want || !got) return false;
    switch (got->kind) {
        case TY_TYVAR: case TY_ANY: case TY_UNKNOWN: case TY_NEVER:
            return false;                       /* an inference arm's business */
        default: break;
    }
    if (perform_type_mentions_named_tyvar(want) ||
        perform_type_mentions_named_tyvar(got))
        return false;
    bool got_agg = perform_type_is_aggregate(got);
    if (perform_type_is_aggregate(want)) {
        /* Nominal identity, as the call's same-kind check: the SAME type, not
         * merely an aggregate. */
        if (!got_agg) return true;
        return !type_eq(*got, *want);
    }
    switch (want->kind) {
        case TY_PTR_VOID:
            /* A call admits a fn value, nil, and any pointer; nothing else. */
            return !(got->kind == TY_PTR_VOID || got->kind == TY_FN ||
                     got->kind == TY_NIL);
        case TY_CSTR:
            return got->kind == TY_FN || got->kind == TY_NIL || got_agg;
        case TY_FLOAT: case TY_FLOAT32: case TY_FLOAT64: case TY_BOOL:
        case TY_INT8: case TY_INT16: case TY_INT32:
        case TY_UINT8: case TY_UINT16: case TY_UINT32: case TY_UINT64:
            return got_agg;
        default:
            return false;                       /* int (carrier), fn, sym, ... */
    }
}

Expr *elab_perform(Elab *e, const Form *call) {
    if (call->as.list.len < 2) {
        diag_emit(DIAG_ERROR, call->span,
                  "perform requires (perform (EffectName arg1 ...))");
        return NULL;
    }
    
    /* Parse effect call form: (EffectName arg1 arg2 ...) */
    Form *effect_call_f = call->as.list.items[1];
    if (effect_call_f->tag != F_LIST) {
        diag_emit(DIAG_ERROR, effect_call_f->span,
                  "perform: expected effect call as list, got %s",
                  form_tag_name(effect_call_f->tag));
        return NULL;
    }
    
    if (effect_call_f->as.list.len < 1) {
        diag_emit(DIAG_ERROR, effect_call_f->span,
                  "perform: effect call must have at least an effect name");
        return NULL;
    }
    
    /* Parse effect name */
    Form *name_f = effect_call_f->as.list.items[0];
    if (name_f->tag != F_SYM) {
        diag_emit(DIAG_ERROR, name_f->span,
                  "perform: effect name must be a symbol");
        return NULL;
    }
    const Symbol *effect_name = name_f->as.sym;
    
    /* Check if effect exists */
    Effect *effect = effect_env_lookup(e->effect_env, effect_name);
    if (!effect) {
        diag_emit(DIAG_ERROR, name_f->span,
                  "perform: unknown effect '%s'", effect_name->name);
        return NULL;
    }

    /* Phase P19-6 / ER5: Enforce effect visibility — private effects cannot be
     * performed from outside their defining module (TUR-E0021). */
    if (!effect->is_exported
        && effect->defining_module_name != NULL
        && effect->defining_module_name != e->current_module_name
        && !elab_effect_is_referred(e, effect)) {
        diag_emit_with_code(DIAG_ERROR, name_f->span,
                            TUR_E0021_PRIVATE_EFFECT,
                            "effect '%s' is private to module '%s'",
                            effect_name->name, effect->defining_module_name->name);
        diag_emit(DIAG_NOTE, name_f->span,
                  "remove ^private from the defeffect declaration to make it public");
        return NULL;
    }

    /* Phase 19 TUR-E0008: At top level (not inside a defn/fn body), every
     * perform must be wrapped by an enclosing handle expression. Inside a
     * defn/fn the caller is expected to provide the handler, so we skip. */
    if (e->fn_body_depth == 0 && !is_effect_handled(e, effect_name)) {
        diag_emit(DIAG_ERROR, name_f->span,
                  "[TUR-E0008]: effect '%s' is performed but has no enclosing handler",
                  effect_name->name);
        diag_emit(DIAG_NOTE, name_f->span,
                  "wrap the expression with a (handle ...) block or a handler macro");
        return NULL;
    }

    /* Parse arguments */
    uint8_t n_args = effect_call_f->as.list.len - 1;

    /* perform-does-not-typecheck-its-arguments: arity, which was unchecked in
     * BOTH directions and is a memory-safety hole in one of them.
     *
     * The effect slot array is sized by the DECLARATION, so too few arguments
     * left the tail slots uninitialised and the handler read whatever was
     * there -- `(defeffect Two [a : int b : int] : int)` performed as
     * `(perform (Two 1))` segfaulted.  Too many silently dropped the surplus
     * (the seams below are all written `if (i < n_params)`), so
     * `(perform (One 1 2 3))` printed a garbage word.
     *
     * A strict equality check is right here because an effect constructor has
     * no arity flexibility at all: `defeffect` counts bare `F_SYM` items and
     * has no `& rest` form and no default VALUES.  (The "default defeffect
     * params" work this report grew out of defaults a parameter's TYPE when the
     * annotation is omitted, which does not change how many arguments a
     * `perform` must supply -- worth stating, because that is the reason arity
     * was left out of the first pass at this report.) */
    if (n_args != effect->constructor->n_params) {
        diag_emit_with_code(DIAG_ERROR, effect_call_f->span,
            TUR_E0002_ARITY_MISMATCH,
            "effect '%s' declares %u parameter%s, but this 'perform' supplies "
            "%u argument%s",
            effect_name->name,
            (unsigned)effect->constructor->n_params,
            effect->constructor->n_params == 1 ? "" : "s",
            (unsigned)n_args, n_args == 1 ? "" : "s");
        return NULL;
    }

    Expr **args = arena_alloc(e->arena, n_args * sizeof(Expr *));
    for (uint32_t i = 0; i < n_args; i++) {
        /* cps-dk-multishot-user-effects (Phase A): reflavor a resumable fn-payload
         * lambda's `effect-cont` param to `multishot-effect-cont` before elaborating
         * (see reflavor_effect_payload) so its `(k v)` resumes through the DK-backed
         * cloneable-cont substrate the CPS handler produces.  A non-lambda / non-
         * effect-cont arg is returned unchanged. */
        Form *arg_form = effect_call_f->as.list.items[i + 1];
        Form *reflav = reflavor_effect_payload(e, arg_form);
        if (reflav) {
            arg_form = reflav;
            /* cps-dk-multishot-user-effects (Phase C): a reflavored RAW-INT payload
             * (form_lambda_resumes_first_param -- the body resumes its param, but
             * the `(fn [int] R)` declaration carries no cont flavor for defeffect to
             * detect) marks the effect resumable-payload here, on the shared effect
             * object, so the enclosing handler (elaborated after the performer)
             * upgrades its `k` and takes the cloneable-cont wrap too.  Order note:
             * this relies on the performer being elaborated before the handler
             * (top-level defns elaborate in source order; the annotated
             * effect-cont/multishot-effect-cont case is order-independent, detected
             * at defeffect).  Skips the annotated case (already set). */
            if (effect->constructor && effect->constructor->resumable_payload_param < 0)
                effect->constructor->resumable_payload_param = (int)i;
        }
        args[i] = elab_form(e, arg_form);
        if (!args[i]) return NULL;
        /* saffron-perform-argument-skips-the-any-seam: an `any` argument
         * reaching a CONCRETE effect parameter takes the same checked unbox an
         * ordinary call's argument takes (elab_call.c's seam).  Without it the
         * 16-byte `tur_tagged_t` box was stored into the one-word effect slot
         * and the handler read the box address as the payload -- an empty line
         * for a `cstr` parameter, silently, on both back ends.
         *
         * `elab_any_unbox_to` is the node `(cast x T)` lowers to, so a genuine
         * mismatch panics with the ordinary `cast: any holds ...` message
         * rather than reinterpreting the word -- the contract the call-site
         * seam already has, and the reason `cast` was built checked.
         *
         * Gated on the dialect like its twin: in a typed file an `any` here is
         * a static question, not a runtime one.  Both the argument's span and
         * the perform's are consulted, for the macro-expanded-argument reason
         * M10 records at the call seam. */
        if (i < effect->constructor->n_params &&
            args[i]->type.kind == TY_ANY &&
            (lang_span_is_dynamic(args[i]->span) ||
             lang_span_is_dynamic(call->span) || e->toplevel_dynamic)) {
            const Type *want_full = effect->constructor->param_full_types
                ? effect->constructor->param_full_types[i] : NULL;
            Type want = want_full ? *want_full
                                  : type_from_kind(effect->constructor->param_types[i]);
            if (want.kind != TY_ANY && want.kind != TY_TYVAR &&
                want.kind != TY_UNKNOWN) {
                Expr *unboxed = elab_any_unbox_to(e, args[i], want, args[i]->span);
                if (unboxed) args[i] = unboxed;
            }
        }
        /* ...and the other direction, which the same hole covered: a CONCRETE
         * argument reaching an `any` effect parameter needs the widen every
         * other `any` slot gets (IT4, `A <: any`), or the raw word is stored
         * where the handler expects a two-word tagged box and reads a garbage
         * tag.  Unlike the unbox above this is not dialect-specific -- an `any`
         * parameter means the same thing in either dialect -- and
         * `elab_coerce_to_any` is a no-op on a value that is already `any`, so
         * it is safe to apply whenever the slot asks for one. */
        if (i < effect->constructor->n_params &&
            effect->constructor->param_types[i] == TY_ANY &&
            args[i]->type.kind != TY_ANY) {
            Expr *widened = elab_coerce_to_any(e, args[i]);
            if (widened) args[i] = widened;
        }
        /* perform-does-not-typecheck-its-arguments: the declared parameter type
         * is right there in the defeffect, and until now nothing at this site
         * read it.  Run the shape check AFTER both seams above, so an `any`
         * argument that was just unboxed to the concrete parameter type is
         * judged on what it became, not on what it arrived as. */
        if (i < effect->constructor->n_params) {
            const Type *want_full = effect->constructor->param_full_types
                ? effect->constructor->param_full_types[i] : NULL;
            Type want = want_full ? *want_full
                                  : type_from_kind(effect->constructor->param_types[i]);
            TypeKind wnorm = perform_primitive_norm(want.kind);
            TypeKind gnorm = perform_primitive_norm(args[i]->type.kind);
            bool primitive_mismatch =
                (wnorm != TY_UNKNOWN && gnorm != TY_UNKNOWN && wnorm != gnorm);
            bool scalar_into_pointer =
                perform_param_is_pointer_shaped(&want) &&
                perform_arg_is_scalar_word(&args[i]->type);
            bool shape_mismatch =
                perform_arg_shape_mismatch(&want, &args[i]->type);
            if (primitive_mismatch || scalar_into_pointer || shape_mismatch) {
                const Symbol *pn = effect->constructor->param_names
                    ? effect->constructor->param_names[i] : NULL;
                char pbuf[96];
                if (pn && pn->name) snprintf(pbuf, sizeof(pbuf), "'%s'", pn->name);
                else                snprintf(pbuf, sizeof(pbuf), "%u", (unsigned)(i + 1));
                diag_emit_with_code(DIAG_ERROR,
                    effect_call_f->as.list.items[i + 1]->span,
                    TUR_E0001_TYPE_MISMATCH,
                    "effect '%s' declares parameter %s as '%s', but this "
                    "argument has type '%s'%s",
                    effect_name->name, pbuf, type_name(want),
                    type_name(args[i]->type),
                    scalar_into_pointer
                        ? " -- the handler would read the value as an address"
                        : "");
                /* The carrier case: the value is already the right thing at run
                 * time and the argument does not want changing -- the missing
                 * annotation does.  Say that, rather than leaving the author to
                 * read a plain type error against a parameter they never
                 * typed. */
                if (perform_arg_is_unannotated_carrier(args[i]))
                    diag_emit(DIAG_NOTE,
                        effect_call_f->as.list.items[i + 1]->span,
                        "this parameter has no type annotation, so it took the "
                        "default '%s' carrier; annotate it (e.g. "
                        "'[%.*s : %s]') to give it the declared type",
                        type_name(args[i]->type),
                        (int)(args[i]->kind == EX_VAR && args[i]->as.var.binding
                              && args[i]->as.var.binding->name
                                  ? args[i]->as.var.binding->name->len : 1),
                        (args[i]->kind == EX_VAR && args[i]->as.var.binding
                         && args[i]->as.var.binding->name
                             ? args[i]->as.var.binding->name->name : "x"),
                        type_name(want));
                return NULL;
            }
        }
        /* cps-dk-multishot-user-effects (Phase A): mark a CAPTURING closure payload
         * of the resumable-payload param `is_effect_payload` so the CPS backend
         * delegates its build (is_delegatable_value) even though it captures --
         * paired with the handler-case boxed-env reap.  A capture-free payload
         * (EX_FN / EX_FN_TO_FAT) needs no flag (already delegatable). */
        if ((int)i == effect_resumable_payload_param(effect)
            && args[i]->kind == EX_CLOSURE && args[i]->as.closure_.closure
            && args[i]->as.closure_.closure->n_captures > 0)
            args[i]->as.closure_.closure->is_effect_payload = true;
        /* An fn-value payload is carried as a BOXED closure (one-word `void *` to
         * the heap `{thunk, env}` box; defeffect marks the param `boxed`), so a
         * capturing receiver's env rides along in the one-word effect slot.  A
         * capturing closure is already a boxed value; a NON-capturing fn is a bare
         * pointer, so box it here (EX_FN_TO_FAT) to keep the representation uniform
         * -- otherwise the handler's boxed-closure dispatch reads garbage. */
        if (args[i]->type.kind == TY_FN && !args[i]->type.as.fn.boxed
            && args[i]->type.as.fn.arity >= 1 && args[i]->type.as.fn.arity <= 5) {
            Type *bt = (Type *)arena_alloc(e->arena, sizeof(Type));
            *bt = args[i]->type;
            bt->as.fn.boxed = true;
            Expr *shim = expr_new(e->arena, EX_FN_TO_FAT, *bt,
                                  effect_call_f->as.list.items[i + 1]->span);
            shim->as.fn_to_fat_.inner = args[i];
            args[i] = shim;
        }
    }

    /* Create the perform expression */
    PerformExpr *perform = arena_alloc(e->arena, sizeof(PerformExpr));
    perform->effect_name = effect_name;
    perform->args = args;
    perform->n_args = n_args;
    /* cps-dk-multishot-user-effects (Phase A): flag a resumed-through-payload
     * effect so the CPS/DK perform-arg gate admits the boxed-fn payload atom. */
    perform->resumable_payload = effect_resumable_payload_param(effect) >= 0;
    
    /* The return type of perform is the result type of the effect.  Tier C: when
     * the effect declares a by-value aggregate result, use the full Type (which
     * carries the real def) rather than type_from_kind (which would yield a
     * def-less TY_ADT). */
    Type result_type = effect->constructor->result_full_type
        ? *effect->constructor->result_full_type
        : type_from_kind(effect->constructor->result_type);

    Expr *out = expr_new(e->arena, EX_PERFORM, result_type, call->span);
    out->as.perform_.perform = perform;
    return out;
}

/* (handle expr case1 case2 ...)
 * Handle algebraic effects with cases.
 * Each case: (EffectName [param1 param2 ...] k) body ...
 */
/* Shared body for `handle` (deep) and `handle-shallow` (shallow).  The two forms
 * differ only in the `shallow` bit stamped on the resulting HandleExpr -- parse,
 * pre-scan, typecheck, and case elaboration are identical (deep vs shallow is
 * type-transparent). */
static Expr *elab_handle_impl(Elab *e, const Form *call, bool shallow) {
    if (call->as.list.len < 2) {
        diag_emit(DIAG_ERROR, call->span,
                  "handle requires (handle expr case1 case2 ...)");
        return NULL;
    }

    
    /* Phase 19 TUR-E0008: Pre-scan case headers to push effect names onto the
     * handled-effects scope BEFORE elaborating the body.  This ensures that
     * perform calls inside the body see the enclosing handle as a handler,
     * without changing the case elaboration logic below. */
    uint32_t n_case_forms_pre = call->as.list.len >= 2 ? call->as.list.len - 2 : 0;
    uint8_t n_cases_pre = (uint8_t)((n_case_forms_pre / 2));
    uint32_t saved_n_handled = e->n_handled_effects;
    for (uint8_t i = 0; i < n_cases_pre; i++) {
        Form *case_f = call->as.list.items[2 + (i * 2)];
        if (case_f->tag == F_LIST && case_f->as.list.len >= 1 &&
            case_f->as.list.items[0]->tag == F_SYM) {
            push_handled_effect(e, case_f->as.list.items[0]->as.sym);
        }
    }

    /* Parse the body to be handled */
    Expr *body = elab_form(e, call->as.list.items[1]);

    /* Pop the pre-scanned handlers after body elaboration */
    e->n_handled_effects = saved_n_handled;

    if (!body) return NULL;
    
    /* Parse handle cases.
     * Surface syntax follows effects-plan.md §4.3:
     *   (handle expr
     *     (Effect [params...] k) body
     *     ...)
     * so cases are provided as header/body pairs.
     */
    uint32_t n_case_forms = call->as.list.len - 2;
    if ((n_case_forms & 1U) != 0U) {
        diag_emit(DIAG_ERROR, call->span,
                  "handle expects pairs of (case-header body)");
        return NULL;
    }

    uint8_t n_cases = (uint8_t)(n_case_forms / 2);
    HandleCase *cases = arena_alloc(e->arena, n_cases * sizeof(HandleCase));
    
    for (uint8_t i = 0; i < n_cases; i++) {
        Form *case_f = call->as.list.items[2 + (i * 2)];
        if (case_f->tag != F_LIST) {
            diag_emit(DIAG_ERROR, case_f->span,
                      "handle: expected case as list, got %s",
                      form_tag_name(case_f->tag));
            return NULL;
        }
        
        /* Case header format: (EffectName [params...] k)
         * LC0: Optional annotation before k:
         *   (EffectName [params...] ^linear k)    -- CK_LINEAR: exactly-once
         *   (EffectName [params...] ^multishot k) -- CK_MULTISHOT: safe multi-shot */
        uint32_t hdr_len = case_f->as.list.len;
        if (hdr_len != 3 && hdr_len != 4) {
            diag_emit(DIAG_ERROR, case_f->span,
                      "handle case header requires (EffectName [params...] k) "
                      "or (EffectName [params...] ^annotation k)");
            return NULL;
        }

        /* Parse effect name */
        Form *name_f = case_f->as.list.items[0];
        if (name_f->tag != F_SYM) {
            diag_emit(DIAG_ERROR, name_f->span,
                      "handle case: effect name must be a symbol");
            return NULL;
        }
        cases[i].effect_name = name_f->as.sym;

        /* Parse parameter list */
        Form *params_f = case_f->as.list.items[1];
        if (params_f->tag != F_LIST && params_f->tag != F_VEC) {
            diag_emit(DIAG_ERROR, params_f->span,
                      "handle case: expected parameter list, got %s",
                      form_tag_name(params_f->tag));
            return NULL;
        }

        cases[i].n_params = params_f->as.list.len;

        /* perform-does-not-typecheck-its-arguments: the HANDLER half of the
         * same arity hole, and the more dangerous direction is the opposite one
         * from the perform side.  A clause declaring MORE parameters than the
         * effect has reads past the end of the slot array -- `(Two [a] k)`
         * against a one-parameter effect printed a garbage word, and
         * `(One [a b c] k)` against it segfaulted.
         *
         * Skipped (not an error) when the effect is not registered: an unknown
         * effect name is diagnosed on its own terms elsewhere, and reporting an
         * arity mismatch against a declaration we could not find would be a
         * confusing second error on the same line. */
        {
            Effect *hef = effect_env_lookup(e->effect_env, cases[i].effect_name);
            if (hef && hef->constructor &&
                cases[i].n_params != hef->constructor->n_params) {
                diag_emit_with_code(DIAG_ERROR, params_f->span,
                    TUR_E0002_ARITY_MISMATCH,
                    "handler clause for '%s' binds %u parameter%s, but the "
                    "effect declares %u",
                    cases[i].effect_name->name,
                    (unsigned)cases[i].n_params,
                    cases[i].n_params == 1 ? "" : "s",
                    (unsigned)hef->constructor->n_params);
                return NULL;
            }
        }
        cases[i].param_names = arena_alloc(e->arena, cases[i].n_params * sizeof(const Symbol *));
        cases[i].param_bindings = arena_alloc(e->arena, cases[i].n_params * sizeof(Binding *));
        for (uint32_t j = 0; j < cases[i].n_params; j++) {
            Form *param_f = params_f->as.list.items[j];
            if (param_f->tag != F_SYM) {
                diag_emit(DIAG_ERROR, param_f->span,
                          "handle case: parameter name must be a symbol");
                return NULL;
            }
            cases[i].param_names[j] = param_f->as.sym;
        }

        /* LC0: Parse optional cont_kind annotation and continuation name.
         * 3 items: (Effect [params...] k)             -> CK_UNIQUE (default)
         * 4 items: (Effect [params...] ^annotation k) -> CK_LINEAR or CK_COPY */
        cases[i].cont_kind = CK_UNIQUE;
        cases[i].resumable_payload = false;
        Form *k_f;
        if (hdr_len == 4) {
            Form *ann_f = case_f->as.list.items[2];
            k_f = case_f->as.list.items[3];
            if (ann_f->tag == F_SYM && ann_f->as.sym == e->sym_caret_linear) {
                cases[i].cont_kind = CK_LINEAR;
            } else if (ann_f->tag == F_SYM &&
                       ann_f->as.sym == e->sym_caret_multishot) {
                cases[i].cont_kind = CK_MULTISHOT;
            } else {
                diag_emit(DIAG_ERROR, ann_f->span,
                          "handle case: expected ^linear or ^multishot "
                          "before continuation name, got '%s'",
                          ann_f->tag == F_SYM ? ann_f->as.sym->name : "<non-symbol>");
                return NULL;
            }
        } else {
            k_f = case_f->as.list.items[2];
        }

        /* cps-dk-multishot-user-effects (Phase A): auto-upgrade an UN-annotated
         * handler `k` to CK_MULTISHOT when this effect is resumed THROUGH a fn
         * payload whose cont param the perform site reflavored to multishot
         * (reflavor_effect_payload).  Both halves must agree on the cloneable
         * substrate: the payload's `(k v)` lowers to `tur_cloneable_cont_resume`,
         * so the handler must provide a cloneable `k` (a `^multishot`-equivalent
         * continuation), not a one-shot fiber cont.  An EXPLICIT annotation always
         * wins (the user asked for ^linear/^multishot deliberately). */
        {
            const Effect *ceff = effect_env_lookup(e->effect_env, cases[i].effect_name);
            if (ceff && effect_resumable_payload_param(ceff) >= 0) {
                cases[i].resumable_payload = true;
                /* Auto-upgrade an UN-annotated `k` to multishot (an explicit
                 * ^linear/^multishot always wins). */
                if (cases[i].cont_kind == CK_UNIQUE)
                    cases[i].cont_kind = CK_MULTISHOT;
            }
        }

        if (k_f->tag != F_SYM) {
            diag_emit(DIAG_ERROR, k_f->span,
                      "handle case: continuation name must be a symbol");
            return NULL;
        }
        cases[i].k_name = k_f->as.sym;
        
        /* Look up effect definition to get param types */
        Effect *eff = effect_env_lookup(e->effect_env, cases[i].effect_name);

        /* Phase P19-6 / ER5: Enforce effect visibility in handler declarations
         * (TUR-E0021). */
        if (eff && !eff->is_exported
            && eff->defining_module_name != NULL
            && eff->defining_module_name != e->current_module_name
            && !elab_effect_is_referred(e, eff)) {
            diag_emit_with_code(DIAG_ERROR, name_f->span,
                                TUR_E0021_PRIVATE_EFFECT,
                                "effect '%s' is private to module '%s'",
                                cases[i].effect_name->name, eff->defining_module_name->name);
            diag_emit(DIAG_NOTE, name_f->span,
                      "remove ^private from the defeffect declaration to make it public");
            return NULL;
        }

        /* Create a handler scope with bindings for params and k */
        Scope handler_scope;
        scope_init(&handler_scope, e->scope);
        Scope *saved_scope = e->scope;
        e->scope = &handler_scope;
        
        /* Create bindings for each parameter */
        for (uint32_t j = 0; j < cases[i].n_params; j++) {
            /* Use the effect's declared param type if available, else TY_INT.
             * Tier C: prefer the full param Type (carries the real aggregate def)
             * so field access on the param binding elaborates. */
            Type ptype;
            if (eff && j < eff->constructor->n_params
                && eff->constructor->param_full_types
                && eff->constructor->param_full_types[j]) {
                ptype = *eff->constructor->param_full_types[j];
            } else {
                TypeKind pk = (eff && j < eff->constructor->n_params)
                    ? eff->constructor->param_types[j] : TY_INT;
                ptype = type_from_kind(pk);
            }
            Binding *pb = binding_new(e, cases[i].param_names[j], ptype, false, false, params_f->span);
            cases[i].param_bindings[j] = pb;
            scope_add(&handler_scope, pb);
        }
        
        /* LC0: Create binding for k; apply cont_kind annotation.
         * is_continuation lets the async-escape check (T25) detect continuation capture. */
        Binding *kb = binding_new(e, cases[i].k_name, TYPE_INT, false, false, k_f->span);
        switch (cases[i].cont_kind) {
        case CK_LINEAR:
            /* ^linear k: exactly one resume/discontinue required.
             * LC2: is_affine + is_relevant wire k into the ST0-ST1 usage-tracking
             * machinery so that usage_state is maintained via elab_var. */
            kb->is_linear = true;
            kb->type.copy_kind = CK_LINEAR;
            kb->is_affine    = true;  /* no duplication */
            kb->is_relevant  = true;  /* must be consumed */
            break;
        case CK_MULTISHOT:
            /* ^multishot k: MS1: safe multi-shot via snapshot semantics; no ownership warning. */
            kb->type.copy_kind = CK_MULTISHOT;
            break;
        default: /* CK_UNIQUE */
            /* Default: affine (at most once).
             * LC2: is_affine wires k into the ST0-ST1 usage-tracking machinery. */
            kb->type.copy_kind = CK_MOVE;
            kb->is_affine = true;  /* no duplication */
            break;
        }
        kb->is_continuation = true;
        cases[i].k_binding = kb;
        scope_add(&handler_scope, kb);
        
        /* Parse handler body inside the handler scope */
        Form *body_f = call->as.list.items[3 + (i * 2)];
        cases[i].body = elab_form(e, body_f);

        /* MS2: For ^multishot handlers, check all captured free variables.
         * CK_UNIQUE (move-only) and CK_LINEAR captures are forbidden because
         * the handler body may run multiple times -- each resume takes a
         * snapshot of k, so the body executes N times and must not consume a
         * linear or unique value on each execution. */
        if (cases[i].cont_kind == CK_MULTISHOT && cases[i].body) {
            /* Build the "local params" list: effect params + k (not free vars) */
            uint8_t n_hparams = (uint8_t)(cases[i].n_params + 1);
            Binding **hparams = arena_alloc(e->arena, n_hparams * sizeof(Binding *));
            for (uint32_t j = 0; j < cases[i].n_params; j++)
                hparams[j] = cases[i].param_bindings[j];
            hparams[cases[i].n_params] = kb;
            uint32_t n_caps = 0;
            Binding **caps = collect_free_vars(cases[i].body, hparams, n_hparams, NULL, 0, &n_caps);
            for (uint32_t ci = 0; ci < n_caps; ci++) {
                CopyKind ck = caps[ci]->type.copy_kind;
                if (ck == CK_UNIQUE || ck == CK_LINEAR) {
                    diag_emit_with_code(DIAG_ERROR, cases[i].body->span,
                        TUR_E0500_MULTISHOT_UNIQUE_CAPTURE,
                        "^multishot handler captures '%s' which is %s -- "
                        "cannot be safely captured in a multi-shot handler",
                        caps[ci]->name->name,
                        ck == CK_UNIQUE ? "unique (move-only)" : "linear");
                }
            }
            free(caps);
        }

        /* LC1/LC2: ^linear k must be consumed (resumed or discontinued) in the handler body.
         * Check immediately after body elaboration while the binding is still in scope.
         * Uses is_linear_consumed (flow-sensitive via elab_if linear-state machinery). */
        if (cases[i].cont_kind == CK_LINEAR && kb && !kb->is_linear_consumed) {
            diag_emit_with_code(DIAG_ERROR, k_f->span,
                TUR_E0100_LINEAR_DROPPED,
                "linear continuation '%s' was not resumed or discontinued",
                kb->name->name);
        }

        /* Restore outer scope */
        e->scope = saved_scope;
        scope_free(&handler_scope);

        if (!cases[i].body) return NULL;

        /* ET3-C: Check that the handler clause result type matches the body type.
         * Skip the check when either is TY_UNKNOWN or TY_NEVER, or when the clause
         * body ends in a resume expression (resume returns the value type passed to
         * the continuation, not the overall handle result type). */
        {
            /* Helper: check if an expression ends in a resume.
             * Handles bare (resume ...) and (do ... (resume ...)) patterns. */
            const Expr *cb = cases[i].body;
            bool ends_in_resume = (cb->kind == EX_RESUME);
            if (!ends_in_resume && cb->kind == EX_DO && cb->as.do_.n > 0) {
                const Expr *last = cb->as.do_.items[cb->as.do_.n - 1];
                ends_in_resume = (last->kind == EX_RESUME);
            }

            if (!ends_in_resume && i == 0 && body != NULL
                && cases[i].body->type.kind != TY_UNKNOWN
                && cases[i].body->type.kind != TY_NEVER
                && body->type.kind != TY_UNKNOWN
                && body->type.kind != TY_NEVER
                && cases[i].body->type.kind != body->type.kind) {
                diag_emit_with_code(DIAG_ERROR, body_f->span,
                    TUR_E0252_HANDLER_RESULT_MISMATCH,
                    "handler clause result type '%s' does not match handle expression type '%s'",
                    type_name(cases[i].body->type), type_name(body->type));
            } else if (!ends_in_resume && i > 0 && cases[0].body != NULL) {
                /* Also check if case 0's body ends in resume before comparing */
                const Expr *cb0 = cases[0].body;
                bool c0_ends_resume = (cb0->kind == EX_RESUME);
                if (!c0_ends_resume && cb0->kind == EX_DO && cb0->as.do_.n > 0) {
                    const Expr *last0 = cb0->as.do_.items[cb0->as.do_.n - 1];
                    c0_ends_resume = (last0->kind == EX_RESUME);
                }
                if (!c0_ends_resume
                    && cases[i].body->type.kind != TY_UNKNOWN
                    && cases[i].body->type.kind != TY_NEVER
                    && cases[0].body->type.kind != TY_UNKNOWN
                    && cases[0].body->type.kind != TY_NEVER
                    && cases[i].body->type.kind != cases[0].body->type.kind) {
                    diag_emit_with_code(DIAG_ERROR, body_f->span,
                        TUR_E0252_HANDLER_RESULT_MISMATCH,
                        "handler clause result type '%s' does not match other clauses' type '%s'",
                        type_name(cases[i].body->type), type_name(cases[0].body->type));
                }
            }
        }
    }

    /* Create the handle expression */
    HandleExpr *handle = arena_alloc(e->arena, sizeof(HandleExpr));
    memset(handle, 0, sizeof(HandleExpr));   /* arena memory is not zeroed */
    handle->body = body;
    handle->cases = cases;
    handle->n_cases = n_cases;
    handle->is_unsafe_marker = false;
    handle->shallow = shallow;

    /* The return type of handle is the same as the body's type */
    Expr *out = expr_new(e->arena, EX_HANDLE, body->type, call->span);
    out->as.handle_.handle = handle;
    return out;
}

/* `handle` -- deep effect handler (re-installed on resume). */
Expr *elab_handle(Elab *e, const Form *call) {
    return elab_handle_impl(e, call, false);
}

/* `handle-shallow` -- shallow effect handler (F2): NOT re-installed on resume,
 * the effect-side analogue of `shift0`. */
Expr *elab_handle_shallow(Elab *e, const Form *call) {
    return elab_handle_impl(e, call, true);
}

/* FH2: (handler (E [params] k) body) -- a single-effect handler value literal.
 * Parses one handle case (identical surface syntax to a (handle ...) case) and
 * builds a HandleExpr with body == NULL.  The result has type TY_HANDLER
 * carrying the effect name, value/result kinds, and continuation discipline.
 */
Expr *elab_handler_lit(Elab *e, const Form *call) {
    /* (handler (E [params] k) body) -- head + header + body = 3 items. */
    if (call->as.list.len != 3) {
        diag_emit(DIAG_ERROR, call->span,
                  "(handler (E [params] k) body) requires a case header and a body");
        return NULL;
    }

    HandleCase *cases = arena_alloc(e->arena, sizeof(HandleCase));
    memset(cases, 0, sizeof(HandleCase));

    Form *case_f = call->as.list.items[1];
    if (case_f->tag != F_LIST) {
        diag_emit(DIAG_ERROR, case_f->span,
                  "handler literal: expected case header (E [params] k), got %s",
                  form_tag_name(case_f->tag));
        return NULL;
    }
    uint32_t hdr_len = case_f->as.list.len;
    if (hdr_len != 3 && hdr_len != 4) {
        diag_emit(DIAG_ERROR, case_f->span,
                  "handler literal header requires (EffectName [params...] k) "
                  "or (EffectName [params...] ^annotation k)");
        return NULL;
    }

    Form *name_f = case_f->as.list.items[0];
    if (name_f->tag != F_SYM) {
        diag_emit(DIAG_ERROR, name_f->span, "handler literal: effect name must be a symbol");
        return NULL;
    }
    cases[0].effect_name = name_f->as.sym;

    Form *params_f = case_f->as.list.items[1];
    if (params_f->tag != F_LIST && params_f->tag != F_VEC) {
        diag_emit(DIAG_ERROR, params_f->span,
                  "handler literal: expected parameter list, got %s",
                  form_tag_name(params_f->tag));
        return NULL;
    }
    cases[0].n_params = params_f->as.list.len;
    cases[0].param_names = arena_alloc(e->arena, cases[0].n_params * sizeof(const Symbol *));
    cases[0].param_bindings = arena_alloc(e->arena, cases[0].n_params * sizeof(Binding *));
    for (uint32_t j = 0; j < cases[0].n_params; j++) {
        Form *param_f = params_f->as.list.items[j];
        if (param_f->tag != F_SYM) {
            diag_emit(DIAG_ERROR, param_f->span, "handler literal: parameter name must be a symbol");
            return NULL;
        }
        cases[0].param_names[j] = param_f->as.sym;
    }

    /* Optional cont_kind annotation + continuation name. */
    cases[0].cont_kind = CK_UNIQUE;
    Form *k_f;
    if (hdr_len == 4) {
        Form *ann_f = case_f->as.list.items[2];
        k_f = case_f->as.list.items[3];
        if (ann_f->tag == F_SYM && ann_f->as.sym == e->sym_caret_linear) {
            cases[0].cont_kind = CK_LINEAR;
        } else if (ann_f->tag == F_SYM && ann_f->as.sym == e->sym_caret_multishot) {
            cases[0].cont_kind = CK_MULTISHOT;
        } else {
            diag_emit(DIAG_ERROR, ann_f->span,
                      "handler literal: expected ^linear or ^multishot "
                      "before continuation name");
            return NULL;
        }
    } else {
        k_f = case_f->as.list.items[2];
    }
    if (k_f->tag != F_SYM) {
        diag_emit(DIAG_ERROR, k_f->span, "handler literal: continuation name must be a symbol");
        return NULL;
    }
    cases[0].k_name = k_f->as.sym;

    Effect *eff = effect_env_lookup(e->effect_env, cases[0].effect_name);
    if (eff && !eff->is_exported
        && eff->defining_module_name != NULL
        && eff->defining_module_name != e->current_module_name
        && !elab_effect_is_referred(e, eff)) {
        diag_emit_with_code(DIAG_ERROR, name_f->span, TUR_E0021_PRIVATE_EFFECT,
                            "effect '%s' is private to module '%s'",
                            cases[0].effect_name->name, eff->defining_module_name->name);
        return NULL;
    }

    /* Handler scope: bind params + k, then elaborate the body. */
    Scope handler_scope;
    scope_init(&handler_scope, e->scope);
    Scope *saved_scope = e->scope;
    e->scope = &handler_scope;

    for (uint32_t j = 0; j < cases[0].n_params; j++) {
        TypeKind pk = (eff && j < eff->constructor->n_params)
            ? eff->constructor->param_types[j] : TY_INT;
        Type ptype = type_from_kind(pk);
        Binding *pb = binding_new(e, cases[0].param_names[j], ptype, false, false, params_f->span);
        cases[0].param_bindings[j] = pb;
        scope_add(&handler_scope, pb);
    }

    Binding *kb = binding_new(e, cases[0].k_name, TYPE_INT, false, false, k_f->span);
    switch (cases[0].cont_kind) {
    case CK_LINEAR:
        kb->is_linear = true; kb->type.copy_kind = CK_LINEAR;
        kb->is_affine = true; kb->is_relevant = true;
        break;
    case CK_MULTISHOT:
        kb->type.copy_kind = CK_MULTISHOT;
        break;
    default:
        kb->type.copy_kind = CK_MOVE; kb->is_affine = true;
        break;
    }
    kb->is_continuation = true;
    cases[0].k_binding = kb;
    scope_add(&handler_scope, kb);

    Form *body_f = call->as.list.items[2];
    cases[0].body = elab_form(e, body_f);

    /* MS2: a ^multishot handler literal may re-run its body per resume, so it
     * must not capture a unique (move-only) or linear free variable (TUR-E0500).
     * Mirrors the inline-handle check in elab_handle. */
    if (cases[0].cont_kind == CK_MULTISHOT && cases[0].body) {
        uint8_t n_hparams = (uint8_t)(cases[0].n_params + 1);
        Binding **hparams = arena_alloc(e->arena, n_hparams * sizeof(Binding *));
        for (uint32_t j = 0; j < cases[0].n_params; j++)
            hparams[j] = cases[0].param_bindings[j];
        hparams[cases[0].n_params] = kb;
        uint32_t n_caps = 0;
        Binding **caps = collect_free_vars(cases[0].body, hparams, n_hparams, NULL, 0, &n_caps);
        for (uint32_t ci = 0; ci < n_caps; ci++) {
            CopyKind ck = caps[ci]->type.copy_kind;
            if (ck == CK_UNIQUE || ck == CK_LINEAR) {
                diag_emit_with_code(DIAG_ERROR, cases[0].body->span,
                    TUR_E0500_MULTISHOT_UNIQUE_CAPTURE,
                    "^multishot handler captures '%s' which is %s -- "
                    "cannot be safely captured in a multi-shot handler",
                    caps[ci]->name->name,
                    ck == CK_UNIQUE ? "unique (move-only)" : "linear");
            }
        }
        free(caps);
    }

    if (cases[0].cont_kind == CK_LINEAR && kb && !kb->is_linear_consumed) {
        diag_emit_with_code(DIAG_ERROR, k_f->span, TUR_E0100_LINEAR_DROPPED,
            "linear continuation '%s' was not resumed or discontinued", kb->name->name);
    }

    e->scope = saved_scope;
    scope_free(&handler_scope);
    if (!cases[0].body) return NULL;

    HandleExpr *handle = arena_alloc(e->arena, sizeof(HandleExpr));
    memset(handle, 0, sizeof(HandleExpr));   /* arena memory is not zeroed */
    handle->body = NULL;          /* literal: detached from any body (FH design) */
    handle->cases = cases;
    handle->n_cases = 1;
    handle->is_unsafe_marker = false;
    handle->shallow = false;      /* handler-value literals are deep (F2) */

    /* Build the TY_HANDLER value type. */
    Type htype;
    memset(&htype, 0, sizeof(Type));
    htype.kind = TY_HANDLER;
    htype.copy_kind = CK_COPY;
    htype.hkt_kind = KIND_STAR;
    htype.as.handler_.effect_name = cases[0].effect_name->name;
    {
        /* FH4.1: single-element handled row (unresolved name-set). */
        const Symbol *one[1] = { cases[0].effect_name };
        htype.as.handler_.handled_row =
            effect_row_unresolved(e->arena, one, 1, call->span);
    }
    htype.as.handler_.value_kind  = (eff && eff->constructor->n_params > 0)
        ? eff->constructor->param_types[0] : TY_INT;
    htype.as.handler_.result_kind = cases[0].body->type.kind;
    htype.as.handler_.cont_kind   = cases[0].cont_kind;

    Expr *out = expr_new(e->arena, EX_HANDLER_LIT, htype, call->span);
    out->as.handler_lit_.handle = handle;
    return out;
}

/* FH3: (with-handler hv body) -- apply a handler value to a body.
 * The body runs with hv's dispatch table installed; the result type is the
 * body's type (the answer type T).  hv must be a TY_HANDLER value.
 */
Expr *elab_with_handler(Elab *e, const Form *call) {
    /* hv first so we know which effects to mark handled while elaborating body. */
    Expr *hv = elab_form(e, call->as.list.items[1]);
    if (!hv) return NULL;
    if (hv->type.kind != TY_HANDLER) {
        diag_emit(DIAG_ERROR, call->as.list.items[1]->span,
                  "(with-handler): first argument must be a handler value, got '%s'",
                  type_name(hv->type));
        return NULL;
    }

    /* Push the handler's effect(s) onto the handled-effects scope so perform
     * calls in the body see them as handled (mirrors elab_handle's pre-scan). */
    uint32_t saved_n_handled = e->n_handled_effects;
    if (hv->type.as.handler_.effect_name != NULL) {
        /* Intern the effect name back to a Symbol for push_handled_effect. */
        const Symbol *eff_sym = intern_cstr(e->st, hv->type.as.handler_.effect_name);
        push_handled_effect(e, eff_sym);
    }
    Expr *body = elab_form(e, call->as.list.items[2]);
    e->n_handled_effects = saved_n_handled;
    if (!body) return NULL;

    /* `(with-handler (handler (E [p] k) case-body) body)` IS
     * `(handle body (E [p] k) case-body)`: the literal is deep, its cases are
     * the handle's, and building it has no effect of its own.  Emit the
     * `handle`, so the literal form lowers wherever `handle` does.  As an
     * EX_WITH_HANDLER it was refused in positions a `handle` lowers in -- as
     * a builtin's operand (`(println (with-handler ...))` left `main`
     * uncolored) and anywhere inside a capturing lambda (the lambda's body
     * tainted its effect through the enclosing `main`).  Only when the case
     * body is answer-typed the way elab_handle_impl would accept it (it
     * resumes, diverges, or has the body's type); anything else keeps the
     * with-handler node and its existing lowering. */
    if (hv->kind == EX_HANDLER_LIT && hv->as.handler_lit_.handle
        && hv->as.handler_lit_.handle->n_cases > 0) {
        const HandleExpr *lit = hv->as.handler_lit_.handle;
        bool answer_ok = true;
        for (uint8_t ci = 0; ci < lit->n_cases && answer_ok; ci++) {
            const Expr *cb = lit->cases[ci].body;
            if (!cb) { answer_ok = false; break; }
            const Expr *last = cb;
            if (last->kind == EX_DO && last->as.do_.n > 0)
                last = last->as.do_.items[last->as.do_.n - 1];
            answer_ok = last->kind == EX_RESUME
                     || cb->type.kind == TY_NEVER
                     || cb->type.kind == body->type.kind;
        }
        if (answer_ok) {
            HandleExpr *h = arena_alloc(e->arena, sizeof(HandleExpr));
            *h = *lit;                 /* cases, n_cases, shallow = false */
            h->body = body;
            Expr *out = expr_new(e->arena, EX_HANDLE, body->type, call->span);
            out->as.handle_.handle = h;
            return out;
        }
    }

    Expr *out = expr_new(e->arena, EX_WITH_HANDLER, body->type, call->span);
    out->as.with_handler_.handler = hv;
    out->as.with_handler_.body = body;
    return out;
}

/* ET3-E / FH5: (compose-handlers h1 h2)
 * Compose two handler values that must handle different effects (TUR_E0251 on
 * overlap).  Produces a TY_HANDLER value whose runtime table is the
 * concatenation of h1's and h2's tables (h1 outer, per FH0.1).
 */
Expr *elab_compose_handlers(Elab *e, const Form *call) {
    if (call->as.list.len != 3) {
        diag_emit(DIAG_ERROR, call->span,
                  "(compose-handlers h1 h2) requires exactly two arguments");
        return NULL;
    }
    Expr *h1 = elab_form(e, call->as.list.items[1]);
    if (!h1) return NULL;
    Expr *h2 = elab_form(e, call->as.list.items[2]);
    if (!h2) return NULL;

    /* ET3: Both arguments should have TY_HANDLER type */
    if (h1->type.kind != TY_HANDLER) {
        diag_emit(DIAG_ERROR, call->as.list.items[1]->span,
                  "(compose-handlers): first argument must be a handler value, got '%s'",
                  type_name(h1->type));
        return NULL;
    }
    if (h2->type.kind != TY_HANDLER) {
        diag_emit(DIAG_ERROR, call->as.list.items[2]->span,
                  "(compose-handlers): second argument must be a handler value, got '%s'",
                  type_name(h2->type));
        return NULL;
    }

    /* FH4.1: handlers must handle disjoint effect *sets* (TUR-E0251).  Compare
     * the handled rows by name so the check also covers composed (multi-effect)
     * handlers, not just the single effect_name. */
    {
        const Symbol *n1[32]; uint8_t c1 = 0;
        const Symbol *n2[32]; uint8_t c2 = 0;
        effect_row_collect_names(h1->type.as.handler_.handled_row, n1, &c1, 32);
        effect_row_collect_names(h2->type.as.handler_.handled_row, n2, &c2, 32);
        for (uint8_t i = 0; i < c1; i++)
            for (uint8_t j = 0; j < c2; j++)
                if (n1[i] == n2[j]) {
                    diag_emit_with_code(DIAG_ERROR, call->span,
                        TUR_E0251_HANDLER_OVERLAP,
                        "composed handlers both handle effect '%s'; overlapping "
                        "effects are not allowed", n1[i]->name);
                    return NULL;
                }
    }
    /* Fallback for legacy types without a handled_row: compare effect_name. */
    if ((!h1->type.as.handler_.handled_row || !h2->type.as.handler_.handled_row)
        && h1->type.as.handler_.effect_name != NULL
        && h2->type.as.handler_.effect_name != NULL
        && h1->type.as.handler_.effect_name == h2->type.as.handler_.effect_name) {
        diag_emit_with_code(DIAG_ERROR, call->span,
            TUR_E0251_HANDLER_OVERLAP,
            "composed handlers both handle effect '%s'; overlapping effects are not allowed",
            h1->type.as.handler_.effect_name);
        return NULL;
    }

    /* FH5: runtime composition is implemented -- the CF3 TUR-E0704 gate is
     * removed.  The static checks above (TUR-E0251 overlap, handler-value
     * typing) still fire first.  Composition lowers to table concatenation
     * (emit_effects_compose_handlers); see docs/first-class-handlers-semantics.md. */

    /* FH0.3: composed handlers must agree on the answer type T.  In v1 a
     * detached handler literal does not carry the answer type -- its
     * result_kind records the case body's own type, which for a resume-
     * terminated body is the resumed value's type, not T.  Enforcing strict T
     * equality from that field would misfire, so the check is deferred to the
     * application site (the body type of with-handler is the real T).  The
     * disjoint-effect rule (TUR-E0251) above is still enforced. */

    /* FH5: produce a composed handler value.  Its runtime table is the
     * concatenation of h1's and h2's tables (h1 outer, per FH0.1).  The
     * value type is TY_HANDLER; the single effect_name field is meaningful
     * only for single-effect handlers, so a composed value sets it to NULL
     * (the row is carried implicitly by the two children at codegen time). */
    Type ctype;
    memset(&ctype, 0, sizeof(Type));
    ctype.kind = TY_HANDLER;
    ctype.copy_kind = CK_COPY;
    ctype.hkt_kind = KIND_STAR;
    ctype.as.handler_.effect_name = NULL;   /* composed: multi-effect row */
    /* FH4.1: the composed handled set is the union of the two rows. */
    ctype.as.handler_.handled_row = effect_row_union(e->arena,
        h1->type.as.handler_.handled_row, h2->type.as.handler_.handled_row);
    ctype.as.handler_.value_kind  = TY_UNKNOWN;
    ctype.as.handler_.result_kind = (h1->type.as.handler_.result_kind != TY_UNKNOWN)
        ? h1->type.as.handler_.result_kind : h2->type.as.handler_.result_kind;
    ctype.as.handler_.cont_kind   = CK_COPY;

    Expr *out = expr_new(e->arena, EX_COMPOSE_HANDLERS, ctype, call->span);
    out->as.compose_handlers_.h1 = h1;
    out->as.compose_handlers_.h2 = h2;
    return out;
}

/* LC1/LC2: Pre-check a continuation binding for double-use before elaborating k.
 * Uses is_linear_consumed (CK_LINEAR) and is_moved (CK_UNIQUE) — both are
 * snapshot/restored by elab_if, making them flow-sensitive across branches.
 * Emits the appropriate error and returns true if the use should be rejected. */
bool cont_check_double_use(Elab *e, const Form *k_form) {
    if (k_form->tag != F_SYM) return false;
    Binding *kb = scope_lookup(e->scope, k_form->as.sym);
    if (!kb || !kb->is_continuation) return false;
    /* CK_COPY / CK_MULTISHOT: multi-shot allowed -- no double-use restriction. */
    if (kb->type.copy_kind == CK_COPY || kb->type.copy_kind == CK_MULTISHOT) return false;

    if (kb->type.copy_kind == CK_LINEAR) {
        if (kb->is_linear_consumed) {
            diag_emit_with_code(DIAG_ERROR, k_form->span,
                TUR_E0101_LINEAR_USE_AFTER_CONSUME,
                "linear continuation '%s' has already been resumed or discontinued",
                kb->name->name);
            return true;
        }
    } else {
        /* CK_UNIQUE: is_moved is flow-sensitive (elab_if restores it between branches). */
        if (kb->is_moved) {
            diag_emit_with_code(DIAG_ERROR, k_form->span,
                TUR_E0201_UNIQUE_COPY,
                "continuation '%s' has already been resumed or discontinued",
                kb->name->name);
            return true;
        }
    }
    return false;
}

/* LC1/LC2: Mark a continuation binding consumed after resume/discontinue.
 * Sets is_linear_consumed (CK_LINEAR), is_moved (CK_UNIQUE) — both flow-sensitive.
 * Also sets usage_state for ST0-ST1 interop. */
static void cont_mark_consumed(Expr *k) {
    if (!k || k->kind != EX_VAR) return;
    Binding *kb = k->as.var.binding;
    if (!kb->is_continuation) return;
    /* LC2: also set usage_state for ST0-ST1 informational tracking. */
    kb->usage_state = USAGE_USED_ONCE;
    if (kb->type.copy_kind == CK_LINEAR) {
        kb->is_linear_consumed = true;
    } else if (type_is_move(kb->type)) {
        /* CK_UNIQUE: mark moved so elab_if's move-state machinery stays consistent. */
        binding_mark_moved(kb, k->span);
    }
    /* CK_COPY / CK_MULTISHOT: usage_state updated but no ownership enforcement. */
}

/* Build an EX_RESUME from an already-elaborated continuation `k` and `value`,
 * with the shared consumption + multishot-in-atomically checks.  Used by
 * `elab_resume` (the `(resume k v)` form) and by the `(k v)` application sugar
 * for effect handler continuations (elab_call.c) -- so both spellings resume
 * identically.  Caller runs cont_check_double_use before elaborating `k`. */
Expr *elab_make_resume(Elab *e, Expr *k, Expr *value, Span span) {
    /* LC1: Mark continuation consumed per its cont_kind. */
    cont_mark_consumed(k);

    /* MS2: Resuming a ^multishot continuation inside atomically is unsafe --
     * the handler body may be re-executed by STM retry, causing the continuation
     * to be resumed more than once in unexpected ways. */
    if (k->kind == EX_VAR && k->as.var.binding &&
        k->as.var.binding->type.copy_kind == CK_MULTISHOT &&
        elab_in_atomically) {
        diag_emit_with_code(DIAG_ERROR, span,
            TUR_E0502_MULTISHOT_RESUME_IN_ATOMIC,
            "cannot resume a '^multishot' continuation inside 'atomically' -- "
            "STM retry may cause the continuation to be resumed multiple times unexpectedly");
        return NULL;
    }

    ResumeExpr *resume = arena_alloc(e->arena, sizeof(ResumeExpr));
    resume->k = k;
    resume->value = value;

    Expr *out = expr_new(e->arena, EX_RESUME, value->type, span);
    out->as.resume_.resume = resume;
    return out;
}

/* (resume k value)
 * Resume a captured continuation with a value.
 */
Expr *elab_resume(Elab *e, const Form *call) {
    if (call->as.list.len != 3) {
        diag_emit(DIAG_ERROR, call->span,
                  "(resume k value) requires exactly two arguments");
        return NULL;
    }

    /* LC1: Check for double-use before elaborating k (would emit E0005 otherwise). */
    if (cont_check_double_use(e, call->as.list.items[1])) return NULL;

    Expr *k = elab_form(e, call->as.list.items[1]);
    if (!k) return NULL;

    Expr *value = elab_form(e, call->as.list.items[2]);
    if (!value) return NULL;

    return elab_make_resume(e, k, value, call->span);
}

/* (discontinue k exception)
 * Discontinue a captured continuation with an exception.
 */
Expr *elab_discontinue(Elab *e, const Form *call) {
    if (call->as.list.len != 3) {
        diag_emit(DIAG_ERROR, call->span,
                  "(discontinue k exception) requires exactly two arguments");
        return NULL;
    }
    
    /* LC1: Check for double-use before elaborating k. */
    if (cont_check_double_use(e, call->as.list.items[1])) return NULL;

    Expr *k = elab_form(e, call->as.list.items[1]);
    if (!k) return NULL;

    Expr *exception = elab_form(e, call->as.list.items[2]);
    if (!exception) return NULL;

    /* LC1: Mark continuation consumed per its cont_kind. */
    cont_mark_consumed(k);

    DiscontinueExpr *discontinue = arena_alloc(e->arena, sizeof(DiscontinueExpr));
    discontinue->k = k;
    discontinue->exception = exception;

    Expr *out = expr_new(e->arena, EX_DISCONTINUE, TYPE_NIL, call->span);
    out->as.discontinue_.discontinue = discontinue;
    return out;
}

/* Phase 19: (cont? k) — check if a Phase 18 continuation has not been consumed.
 * For Phase 19 algebraic-effect continuations (which are CK_MOVE int64_t dummies),
 * the static one-shot check already catches double-resume at compile time.
 * For Phase 18 tur_cont* continuations, this checks tur_cont_consumed at runtime.
 * Returns: bool
 */
Expr *elab_cont_pred(Elab *e, const Form *call) {
    if (call->as.list.len != 2) {
        diag_emit(DIAG_ERROR, call->span,
                  "(cont? k) requires exactly one argument");
        return NULL;
    }
    Expr *inner = elab_form(e, call->as.list.items[1]);
    if (!inner) return NULL;

    /* Accepts TY_CONT (Phase 18 tur_cont*) or TY_INT (Phase 19 CK_MOVE dummy k) */
    if (inner->type.kind != TY_CONT && inner->type.kind != TY_INT) {
        diag_emit(DIAG_ERROR, call->span,
                  "(cont? k) requires a continuation argument");
        return NULL;
    }

    Expr *out = expr_new(e->arena, EX_CONT_PRED, TYPE_BOOL, call->span);
    out->as.cont_pred_.expr = inner;
    return out;
}

/* CC4.2: default an unannotated continuation parameter to the escape flavor.
 * (call/cc f) / (escape f) hand `f` an undelimited continuation captured against
 * the implicit root prompt; resuming it lowers to tur_escape_resume.  When `f`
 * is written inline as (fn [k] ...) with a single *unannotated* trailing param,
 * rewrite the param list to [k :escape-cont] so the (k v) application sugar
 * dispatches to the escape resume runtime instead of defaulting `k` to :int.
 * (This is the local exception to the :int-by-default lambda-param rule, scoped
 * to the call/cc / escape receiver.)  Caret markers (^linear, ^unique, ...) are
 * '^'-prefixed and always precede their param, so an unannotated trailing param
 * is simply a non-'^' F_SYM in the last vec slot -- ^linear k still gets the
 * annotation appended (preserving OQ3's opt-in linearity). */
static Form *callcc_default_cont_param(Elab *e, Form *f_form) {
    if (!f_form || f_form->tag != F_LIST || f_form->as.list.len < 2) return f_form;
    const Form *head = f_form->as.list.items[0];
    if (head->tag != F_SYM || head->as.sym != e->sym_fn) return f_form;
    Form *params = f_form->as.list.items[1];
    if (params->tag != F_VEC || params->as.list.len == 0) return f_form;
    const Form *last = params->as.list.items[params->as.list.len - 1];
    /* Already annotated (keyword / `: T` / type-list) -- leave it alone. */
    if (last->tag != F_SYM || last->as.sym->name[0] == '^') return f_form;

    /* saffron-dynamic-surface-pass (low): a receiver annotated `: any` --
     * `(fn [k] : any (k 42))` -- gets a VALUE-TYPED continuation,
     * `(escape-cont any any)`, so `(k v)` widens `v` to a box and the
     * landing reads a box back.  With the bare `:escape-cont` the resume
     * value stayed a raw word: `(k 42)` delivered an int64 and the landing
     * assigned it to a `tur_tagged_t` binder ("conversion to non-scalar type
     * requested").  Detected on the FORM, since the receiver has not been
     * elaborated yet; only the exact `: any` spelling qualifies. */
    bool ret_any = false;
    if (f_form->as.list.len >= 4) {
        const Form *ra = f_form->as.list.items[2];
        const Form *inner = (ra->tag == F_TYPE_ANN && ra->as.list.len == 1)
                                ? ra->as.list.items[0] : ra;
        if ((inner->tag == F_SYM || inner->tag == F_KEYWORD) &&
            inner->as.sym->len == 3 && memcmp(inner->as.sym->name, "any", 3) == 0 &&
            (ra->tag == F_TYPE_ANN || ra->tag == F_KEYWORD))
            ret_any = true;
    }
    /* Rebuild the param vec with :escape-cont appended. */
    uint32_t pn = params->as.list.len;
    Form **new_params = (Form **)arena_alloc(e->arena, sizeof(Form *) * (pn + 1));
    for (uint32_t i = 0; i < pn; i++) new_params[i] = params->as.list.items[i];
    if (ret_any) {
        const Symbol *anys = intern_cstr(e->st, "any");
        Form *ct[3] = { form_sym(e->arena, last->span, intern_cstr(e->st, "escape-cont")),
                        form_sym(e->arena, last->span, anys),
                        form_sym(e->arena, last->span, anys) };
        new_params[pn] = form_type_ann(e->arena, last->span,
                                       form_list(e->arena, last->span, ct, 3));
    } else {
        new_params[pn] = form_keyword(e->arena, last->span,
                                      intern_cstr(e->st, "escape-cont"));
    }
    Form *new_vec = form_vec(e->arena, params->span, new_params, pn + 1);

    /* Rebuild the fn form with the rewritten param vec. */
    uint32_t fn_n = f_form->as.list.len;
    Form **new_fn = (Form **)arena_alloc(e->arena, sizeof(Form *) * fn_n);
    for (uint32_t i = 0; i < fn_n; i++) new_fn[i] = f_form->as.list.items[i];
    new_fn[1] = new_vec;
    return form_list(e->arena, f_form->span, new_fn, fn_n);
}

/* call-cc-completion: build the shared EX_CALLCC node for (call/cc f) and
 * (escape f).  The result type is f's codomain (CF2 typing): calling f with the
 * captured continuation yields a value of f's return type. */
static Expr *callcc_node(Elab *e, const Form *call, bool is_escape) {
    Form *f_form = callcc_default_cont_param(e, call->as.list.items[1]);
    bool saved_ccr = e->in_callcc_receiver;
    e->in_callcc_receiver = true;
    Expr *f_expr = elab_form(e, f_form);
    e->in_callcc_receiver = saved_ccr;
    if (!f_expr) return NULL;

    /* f's codomain becomes the call/cc result type; fall back to int. */
    Type result_type = TYPE_INT;
    if (f_expr->type.kind == TY_FN) {
        result_type = f_expr->type.as.fn.result_full_type
            ? *f_expr->type.as.fn.result_full_type
            : type_from_kind(f_expr->type.as.fn.result_kind);
    } else if (f_expr->kind == EX_VAR && f_expr->as.var.binding
               && f_expr->as.var.binding->type.kind == TY_FN) {
        result_type = type_from_kind(
            f_expr->as.var.binding->type.as.fn.result_kind);
    }

    Expr *out = expr_new(e->arena, EX_CALLCC, result_type, call->span);
    out->as.callcc_.fn = f_expr;
    out->as.callcc_.is_escape = is_escape;
    return out;
}

/* call-cc-completion (CC1): (call/cc f) - undelimited continuation capture
 * against the implicit program-wide prompt.  f receives a real continuation
 * handle; invoking it (tur_escape_resume) returns its argument at the call/cc
 * site (one-shot, upward).  No enclosing reset required; unbounded depth. */
Expr *elab_call_cc(Elab *e, const Form *call) {
    if (call->as.list.len != 2) {
        diag_emit(DIAG_ERROR, call->span,
                  "(call/cc f) requires exactly one argument");
        return NULL;
    }
    /* call-cc-completion (CC5): the -Xcallcc gate (and TUR-E0700) are retired.
     * The CPS substrate (cps-transform-plan CPS5.3/CPS6) supplies the implicit
     * program-wide prompt and unbounded capture, so call/cc is now sound and
     * ungated.  -Xcallcc is accepted as a deprecated no-op for one release. */
    /* call-cc-completion (CC1): real undelimited capture against the implicit
     * program-wide prompt.  Build an EX_CALLCC node; codegen establishes a
     * setjmp landing at the call/cc site and hands f the landing as the
     * continuation handle.  f receives it (as int64_t); invoking it via
     * (tur_escape_resume k v) returns v at this site (one-shot, upward escape),
     * with no enclosing reset required and no 16-frame ceiling.  The result
     * type is f's codomain (CF2). */
    return callcc_node(e, call, /*is_escape=*/false);
}

/* call-cc-completion (CC3): (escape f) - one-shot undelimited early-exit.
 * `f` receives a real continuation captured against the implicit root prompt;
 * invoking it (tur_escape_resume) unwinds to this site without re-installing a
 * prompt (the shift0-style abort flavor). */
Expr *elab_escape(Elab *e, const Form *call) {
    if (call->as.list.len != 2) {
        diag_emit(DIAG_ERROR, call->span,
                  "(escape f) requires exactly one argument");
        return NULL;
    }
    /* call-cc-completion (CC5): the -Xcallcc gate (and TUR-E0701) are retired --
     * see elab_call_cc.  escape now has real early-exit semantics on the CPS
     * substrate; -Xcallcc is a deprecated no-op for one release. */
    /* call-cc-completion (CC3): escape is the one-shot abort flavor.  Same
     * undelimited capture as call/cc; invoking the continuation unwinds to this
     * site without re-installing a prompt.  For the one-shot upward use both
     * flavors behave identically (the captured context is abandoned either
     * way); is_escape is recorded for the eventual shift-vs-shift0 distinction. */
    return callcc_node(e, call, /*is_escape=*/true);
}
