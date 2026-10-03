/* elab_forms.c -- control-flow and basic expression forms (let/if/do/while/case/...). */
#include "elab_internal.h"
#include "lang_dialects.h"   /* saffron-lang-plan S3: lang_span_is_dynamic */
#include "experiments.h"     /* loop-invariants-plan: experiment_warn_if_used */

/* ---- file-local helper forward declarations ---- */
static Expr *elab_set_deref(Elab *e, const Form *call, const Form *deref_form);

/* byvalue-struct-field-leak: resolve a let-binding type to the by-value ADT
 * (single-variant record product) whose owned rc/ref fields need a scope-exit
 * release, or NULL when the type is not such a product.
 *
 * A bare by-value struct/record local carrying an `rc`/`ref`/`weak` field
 * (needs_drop_glue) never invokes drop glue on its own -- only the
 * `rc/of`-wrapped path does, through the control block's drop_fn.  So its owned
 * fields leak at scope exit.  This gate mirrors the emit-side predicate for
 * `emit_adt_byval_drop_glue`: a non-:heap, drop-gluey, single-ctor product laid
 * out by value (adt_is_byvalue_product for a bare TY_ADT, or
 * adt_app_is_byvalue_product for a concrete TY_APP monomorph).  A :heap ADT is a
 * typed pointer whose fields are released elsewhere, so it is excluded. */
static const AdtDef *elab_byval_drop_adt(Type t) {
    const AdtDef *def = NULL;
    if (t.kind == TY_ADT) {
        def = t.as.adt_.def;
        if (!def || !adt_is_byvalue_product(def)) return NULL;
    } else if (t.kind == TY_APP) {
        def = type_adt_app_def(&t);
        if (!def || !adt_app_is_byvalue_product(t)) return NULL;
    } else {
        return NULL;
    }
    if (!def->needs_drop_glue || def->is_heap) return NULL;
    if (def->n_ctors != 1) return NULL;
    return def;
}

/* byvalue-recursive-adt-boxes-are-never-freed: resolve a let-binding type to the
 * by-value ADT whose recursive SPINE needs a scope-exit free, or NULL.
 *
 * The twin of elab_byval_drop_adt above and deliberately not a relaxation of it:
 * that one refuses a multi-variant ADT because its callers index `ctors[0]`
 * directly, and a recursive ADT is a sum by construction (it needs a base case),
 * so every type this exists for is multi-variant.  The recursive-field marking
 * (elab_structs.c) has already applied the `:copy` and `:heap` exclusions and
 * the direct-self-reference test -- a field pointing `drop_inner_def` at its own
 * def is the whole condition, so it is not restated here. */
bool localowned_binding_is_confined(const Expr *body, const Binding *b,
                                    bool result_cannot_carry);

const AdtDef *elab_byval_localowned_adt(Type t) {
    const AdtDef *def = NULL;
    if (t.kind == TY_ADT)      def = t.as.adt_.def;
    else if (t.kind == TY_APP) def = type_adt_app_def(&t);
    else                       return NULL;
    if (!def || def->is_heap || !def->needs_drop_glue) return NULL;
    /* A parametric monomorph's glue carries a mangled per-instantiation name;
     * threading that through is separate work, as the sibling nested-aggregate
     * rule already records. */
    if (def->n_type_params != 0) return NULL;
    for (uint32_t ci = 0; ci < def->n_ctors; ci++)
        for (uint32_t fi = 0; fi < def->ctors[ci]->n_fields; fi++)
            if (def->ctors[ci]->fields[fi].drop_inner_def == def ||
                def->ctors[ci]->fields[fi].kind == TY_ANY)
                return def;
    return NULL;
}

/* rc-field-read-into-var-double-free: peel type ascriptions and return the
 * EX_GET_FIELD when `init` reads an owning `rc<T>` FIELD directly (e.g.
 * `(.r o)` / `(:: (.r o) rc<int>)`), or NULL otherwise.
 *
 * Reading an `rc` field is a shared-ownership borrow: the source struct still
 * owns that field (a by-value local releases it via its scope-exit field
 * auto-drop, an `rc/of`-wrapped struct via its control-block drop glue, a
 * borrowed parameter via its caller).  Binding the raw word into a new
 * rc-managed local therefore aliases the control block WITHOUT incrementing the
 * strong count, yet that new binding gets its own scope-exit decrement -- two
 * decrements against one +1 count => double free.  The caller wraps such an init
 * in EX_RC_CLONE so the read clones (increments) the count, making the new
 * binding a genuine second owner that balances its own decrement. */
static Expr *elab_rc_field_read_init(Expr *init) {
    Expr *cur = init;
    while (cur && cur->kind == EX_ASCRIBE)
        cur = cur->as.ascribe_.inner;
    if (cur && cur->kind == EX_GET_FIELD && cur->type.kind == TY_RC)
        return cur;
    return NULL;
}

/* byvalue-recursive-shared-copies-leak: the struct-sized twin of the rule
 * above.  A by-value product with an `rc` field -- `(defstruct S [r : rc<int>
 * n : int])` -- is released field by field when a let-local of it leaves scope
 * (the auto-drop in elab_let), so every such local must hold its own +1 on each
 * rc field.  A copy of a value someone else owns holds none, and the move
 * checker lets one be taken from three places that keep owning it:
 *
 *   (defn id-b [^borrow x : S] : S x)           ; out of a ^borrow
 *   (defn get-g [] : S g)                       ; out of a global
 *   (vec-get v 0)                               ; out of a container
 *
 * plus a match binder of any of those.  Each local initialised from one (or
 * from a call returning one) decremented a count it never took: ASan
 * heap-use-after-free in rc_strong_decrement, all four shapes.
 *
 * The fix is at the copy: where a SHARED VIEW flows into a position that owns
 * -- a let-local, a function's result, a `return` -- it is cloned, taking a +1
 * on each rc field, so the owner it becomes is a real one.  A function's result
 * is therefore always owned, which is the convention every caller already
 * assumes.  Only non-generic products: a monomorph's field layout is named per
 * instantiation, and nothing here has needed one yet. */
static const AdtDef *elab_byval_product_with(Type t, TypeKind k) {
    const AdtDef *ad = elab_byval_drop_adt(t);
    if (!ad || t.kind != TY_ADT || ad->n_type_params != 0) return NULL;
    const CtorDef *ctor = ad->ctors[0];
    for (uint32_t fi = 0; fi < ctor->n_fields; fi++)
        if (ctor->fields[fi].kind == k) return ad;
    return NULL;
}

const AdtDef *elab_rc_product_adt(Type t) {
    return elab_byval_product_with(t, TY_RC);
}

/* A container read hands back a copy of an element the container still
 * holds -- the same list own_carry_for_result retains an `rc` result for. */
static bool elab_call_reads_container_element(const Expr *x) {
    const Binding *fb = x->as.call_.fn_binding;
    if (x->as.call_.fn_expr || !fb || !fb->name) return false;
    const char *nm = fb->name->name;
    return strcmp(nm, "vec-get") == 0 || strcmp(nm, "vec-get-byval") == 0 ||
           strcmp(nm, "vec-get-checked") == 0 || strcmp(nm, "map-get-eq-o") == 0;
}

bool elab_expr_is_shared_view(const Expr *x) {
    while (x) {
        switch (x->kind) {
            case EX_ASCRIBE:   x = x->as.ascribe_.inner; continue;
            /* A field of a shared view is shared.  A field of an OWNED value is
             * not claimed either way: the parent's drop releases only its own
             * direct rc/ref fields, so a nested product's are not doubled. */
            case EX_GET_FIELD: x = x->as.get_field_.struct_expr; continue;
            case EX_VAR: {
                const Binding *b = x->as.var.binding;
                return b && (b->is_borrow || b->is_global || b->shared_view);
            }
            case EX_CALL:
                return elab_call_reads_container_element(x);
            default:
                return false;
        }
    }
    return false;
}

/* (let [__shv v] (do (rc/clone (.r1 __shv)) ... __shv)) */
static Expr *elab_clone_rc_fields(Elab *e, Expr *v, const AdtDef *ad) {
    LetBinding *lb = (LetBinding *)arena_alloc(e->arena, sizeof(LetBinding));
    char nm[48];
    snprintf(nm, sizeof nm, "__shv_%u", elab_fresh_id(e));
    const Symbol *sym = symtab_intern(e->st, strslice(nm, (uint32_t)strlen(nm)));
    Binding *tb = binding_new(e, sym, v->type, false, false, v->span);
    memset(lb, 0, sizeof(*lb));
    lb->binding = tb;
    lb->init = v;

    const CtorDef *ctor = ad->ctors[0];
    uint32_t n_rc = 0;
    for (uint32_t fi = 0; fi < ctor->n_fields; fi++)
        if (ctor->fields[fi].kind == TY_RC) n_rc++;
    Expr **items = (Expr **)arena_alloc(e->arena, (n_rc + 1) * sizeof(Expr *));
    uint32_t j = 0;
    for (uint32_t fi = 0; fi < ctor->n_fields; fi++) {
        if (ctor->fields[fi].kind != TY_RC) continue;
        Type fld_ty = ctor->fields[fi].full_type ? *ctor->fields[fi].full_type
                                                 : type_from_kind(TY_RC);
        Expr *tv = expr_new(e->arena, EX_VAR, v->type, v->span);
        tv->as.var.binding = tb;
        Expr *gf = expr_new(e->arena, EX_GET_FIELD, fld_ty, v->span);
        gf->as.get_field_.struct_expr = tv;
        gf->as.get_field_.field_idx = fi;
        gf->as.get_field_.adt_def = ad;
        gf->as.get_field_.adt_ctor = ctor;
        Expr *cl = expr_new(e->arena, EX_RC_CLONE, fld_ty, v->span);
        cl->as.rc_clone_.expr = gf;
        cl->as.rc_clone_.elide = false;
        items[j++] = cl;
    }
    Expr *res = expr_new(e->arena, EX_VAR, v->type, v->span);
    res->as.var.binding = tb;
    items[j++] = res;
    Expr *d = expr_new(e->arena, EX_DO, v->type, v->span);
    d->as.do_.items = items;
    d->as.do_.n = j;
    Expr *let = expr_new(e->arena, EX_LET, v->type, v->span);
    let->as.let_.bindings = lb;
    let->as.let_.n = 1;
    let->as.let_.body = d;
    return let;
}

/* Clone every shared-view LEAF of `v` -- the value positions of an if / match /
 * do / let are followed, so `(if c x (S ...))` clones only the `x` branch and a
 * fresh value is never counted twice.  Sets *had_shared when anything was. */
Expr *elab_clone_shared_rc_product(Elab *e, Expr *v, bool *had_shared) {
    if (!v) return v;
    const AdtDef *ad = elab_rc_product_adt(v->type);
    if (!ad) return v;
    switch (v->kind) {
        case EX_IF:
            v->as.if_.then_ = elab_clone_shared_rc_product(e, v->as.if_.then_, had_shared);
            if (v->as.if_.else_or_null)
                v->as.if_.else_or_null =
                    elab_clone_shared_rc_product(e, v->as.if_.else_or_null, had_shared);
            return v;
        case EX_MATCH:
            for (uint32_t i = 0; i < v->as.match_.n_arms; i++)
                v->as.match_.arms[i].body =
                    elab_clone_shared_rc_product(e, v->as.match_.arms[i].body, had_shared);
            return v;
        case EX_DO:
            if (v->as.do_.n > 0)
                v->as.do_.items[v->as.do_.n - 1] = elab_clone_shared_rc_product(
                    e, v->as.do_.items[v->as.do_.n - 1], had_shared);
            return v;
        case EX_LET:
        case EX_LETREC:
            v->as.let_.body = elab_clone_shared_rc_product(e, v->as.let_.body, had_shared);
            return v;
        default:
            if (!elab_expr_is_shared_view(v)) return v;
            if (had_shared) *had_shared = true;
            return elab_clone_rc_fields(e, v, ad);
    }
}

/* The first value position of `v` that produces a shared view, or NULL.  (The
 * ref-field question: a `ref` cannot be cloned, so a value that may hold one
 * it does not own must leave that field to its owner.) */
static const Expr *elab_shared_view_leaf(const Expr *v) {
    if (!v) return NULL;
    const Expr *r = NULL;
    switch (v->kind) {
        case EX_IF:
            r = elab_shared_view_leaf(v->as.if_.then_);
            return r ? r : elab_shared_view_leaf(v->as.if_.else_or_null);
        case EX_MATCH:
            for (uint32_t i = 0; i < v->as.match_.n_arms && !r; i++)
                r = elab_shared_view_leaf(v->as.match_.arms[i].body);
            return r;
        case EX_DO:
            return v->as.do_.n > 0
                ? elab_shared_view_leaf(v->as.do_.items[v->as.do_.n - 1]) : NULL;
        case EX_LET:
        case EX_LETREC:
            return elab_shared_view_leaf(v->as.let_.body);
        default:
            return elab_expr_is_shared_view(v) ? v : NULL;
    }
}

/* The let-init / result hook: clone the rc fields of a shared view where it
 * becomes an owner.  A `ref` field has no count to take, so a let-local that
 * may hold a borrowed one is marked shared_view (its auto-drop leaves the
 * field to the real owner), and a RESULT that may hold one -- the caller would
 * free it, and so would the owner -- is TUR-E0108, Rust's "cannot move out of
 * a borrow". */
Expr *elab_own_byval_copy(Elab *e, Expr *v, Binding *local) {
    if (!v) return v;
    if (elab_byval_product_with(v->type, TY_REF) ||
        elab_byval_product_with(v->type, TY_LREF)) {
        const Expr *leaf = elab_shared_view_leaf(v);
        if (leaf && local) {
            local->shared_view = true;
        } else if (leaf) {
            const AdtDef *ad = elab_byval_drop_adt(v->type);
            diag_emit_with_code(
                DIAG_ERROR, leaf->span, TUR_E0108_REF_FIELD_MOVED_OUT_OF_BORROW,
                "returns a copy of a '%s' it does not own (a ^borrow, a global, "
                "a container element, or a binder of one): the caller would free "
                "its owning `ref` field, and so would the value's real owner. "
                "Return a new value (copy the box with `(ref (deref ...))`), or take "
                "the parameter by value",
                ad && ad->name ? ad->name : "struct");
        }
    }
    return elab_clone_shared_rc_product(e, v, NULL);
}

/* local-struct-drop: does field `fi` own a BOXED fn-field?  Such a field holds a
 * heap fat-closure handle (`{shim, fn}` box, or a capturing env) that the struct
 * drop glue frees via `free((void *)f)`.  A by-value local carrying one is freed
 * at scope exit by the direct emitter (Binding.drops_fn_fields), NOT an injected
 * defer -- a `(drop! (.fn o))` defer reads a fat-fn field the CPS backend's
 * continuation-capture admission rejects, evicting a colored fn.  An UNboxed
 * fn-field is a bare fn pointer (not heap) and is skipped. */
static bool elab_field_is_boxed_fnfield(const CtorDef *ctor, uint32_t fi) {
    return ctor->fields[fi].kind == TY_FN && ctor->fields[fi].full_type &&
           ctor->fields[fi].full_type->kind == TY_FN &&
           ctor->fields[fi].full_type->as.fn.boxed;
}

/* struct-temporary-fn-field-box-leaks: does a by-value value of type `t`
 * own a BOXED fn-field -- the case local-struct-drop frees at a `let`'s scope
 * exit?  Exported for the field-call path, which hoists an unbound owning
 * receiver into a `let` so the same release reaches it. */
bool elab_type_owns_boxed_fnfield(Type t) {
    const AdtDef *ad = elab_byval_drop_adt(t);
    if (!ad) return false;
    const CtorDef *ctor = ad->ctors[0];
    for (uint32_t fi = 0; fi < ctor->n_fields; fi++)
        if (elab_field_is_boxed_fnfield(ctor, fi)) return true;
    return false;
}

/* ---- internal define splicing ---- */

/* splice_body_def_head -- is `f` a body-position binding form, and under which
 * spelling was it written?  Returns the head symbol (`define` or `def`) so the
 * caller's diagnostics can quote what the user actually typed, or NULL.
 *
 * def/define consolidation D1: `def` in a body position means what `define`
 * means -- a binding scoped over the rest of the body.  At the top level `def`
 * keeps its global-binding meaning, so a body window that IS the global scope
 * (a top-level `(do ...)`) leaves `def` alone and lets elab_def see it. */
static const Symbol *splice_body_def_head(Elab *e, const Form *f) {
    if (f->tag != F_LIST || f->as.list.len < 1) return NULL;
    Form *head = f->as.list.items[0];
    if (head->tag != F_SYM) return NULL;
    const Symbol *s = head->as.sym;
    if (s == e->sym_define) return s;
    if (s == e->sym_def && e->scope != &e->global) return s;
    return NULL;
}

/* splice_internal_defines -- rewrite a body window that may contain
 * (define name init) / (def name init) forms into nested let expressions.
 *
 * Returns NULL when no such form is present so callers can keep their existing
 * fast path unchanged (true no-op guarantee -- zero codegen drift).
 *
 * When defines are present, always returns a non-NULL Form* (a let or do)
 * that the caller should elaborate with elab_form.  On parse error, emits a
 * diagnostic and returns form_nil; the caller still calls elab_form on it,
 * which returns a typed nil (preventing double-error from fallthrough).
 *
 * Desugaring:
 *   (define x 1)         =>  (let [x 1]        <rest>)
 *   (define ^mut x 1)    =>  (let [^mut x 1]   <rest>)
 *   (def x : float 7.1)  =>  (let [x : float 7.1] <rest>)
 */
Form *splice_internal_defines(Elab *e, Form **items, uint32_t n, Span span) {
    /* Pre-scan: bail out fast when no defines are present. */
    bool has_define = false;
    for (uint32_t i = 0; i < n; i++) {
        if (splice_body_def_head(e, items[i])) { has_define = true; break; }
    }
    if (!has_define) return NULL;

    /* Find the first define and build (let [^ann... name init] <tail>). */
    for (uint32_t i = 0; i < n; i++) {
        Form *f = items[i];
        const Symbol *head_sym = splice_body_def_head(e, f);
        if (!head_sym) continue;
        const char *kw = head_sym->name;  /* quote the spelling the user wrote */

        /* Parse: (define [^ann...] name [: type] init) */
        Form **dargs  = f->as.list.items + 1;  /* skip the head */
        uint32_t dlen = f->as.list.len - 1;
        if (dlen < 2) {
            diag_emit(DIAG_ERROR, f->span,
                      "%s requires (%s name init)", kw, kw);
            return form_nil(e->arena, f->span);
        }

        /* Consume annotation symbols before the binding name.
         * Mirrors the annotation loop in elab_let (elab_forms.c).
         * D3/§3.5(c): ^persistent and ^deprecated are static-storage /
         * top-level concepts with no local meaning.  ^persistent used to be
         * accepted here and quietly demoted to an ordinary let binding --
         * i.e. it silently did not do what it said.  Both are now named
         * rejections. */
        uint32_t ann_end = 0;
        {
            uint32_t j = 0;
            while (j < dlen) {
                Form *cur = dargs[j];
                if (cur->tag != F_SYM) break;
                const Symbol *s = cur->as.sym;
                if (s == e->sym_caret_persistent || s == e->sym_caret_deprecated) {
                    diag_emit(DIAG_ERROR, cur->span,
                              "%s: '%s' has no meaning on a body binding "
                              "(it is a top-level `def` annotation); "
                              "move the binding to the top level or drop the annotation",
                              kw, s->name);
                    return form_nil(e->arena, f->span);
                }
                if (s == e->sym_caret_mut       ||
                    s == e->sym_caret_linear     ||
                    s == e->sym_caret_unique     ||
                    s == e->sym_caret_affine     ||
                    s == e->sym_caret_relevant) {
                    j++;
                } else {
                    break;
                }
            }
            ann_end = j;
        }
        uint32_t name_idx = ann_end;
        if (name_idx >= dlen) {
            diag_emit(DIAG_ERROR, f->span,
                      "%s requires (%s name init)", kw, kw);
            return form_nil(e->arena, f->span);
        }
        Form *name_form = dargs[name_idx];
        if (name_form->tag != F_SYM) {
            diag_emit(DIAG_ERROR, name_form->span,
                      "%s: binding name must be a symbol", kw);
            return form_nil(e->arena, f->span);
        }

        /* D3/§3.5(a): optional `: type` / `:type` ascription between the name
         * and the init, matching top-level `def` and the `[name : type init]`
         * shape elab_let already accepts.  Only a type when something follows
         * it -- otherwise `(define x :some-keyword)` would lose its init. */
        Form *type_form = NULL;
        uint32_t init_idx = name_idx + 1;
        if (init_idx + 1 < dlen) {
            Form *maybe_ann = dargs[init_idx];
            if (maybe_ann->tag == F_KEYWORD || maybe_ann->tag == F_TYPE_ANN) {
                type_form = maybe_ann;
                init_idx++;
            }
        }
        if (init_idx >= dlen) {
            diag_emit(DIAG_ERROR, f->span,
                      "%s requires an initial value", kw);
            return form_nil(e->arena, f->span);
        }
        if (init_idx != dlen - 1) {
            diag_emit(DIAG_ERROR, f->span,
                      "%s: expected (%s name [: type] init); got extra forms",
                      kw, kw);
            return form_nil(e->arena, f->span);
        }

        /* Build the let binding vector: [^ann... name [: type] init] */
        uint32_t bvec_len = ann_end + (type_form ? 3 : 2);
        Form **bvec_items = (Form **)arena_alloc(e->arena, bvec_len * sizeof(Form *));
        uint32_t bvi = 0;
        for (uint32_t a = 0; a < ann_end; a++) bvec_items[bvi++] = dargs[a];
        bvec_items[bvi++] = name_form;
        if (type_form) bvec_items[bvi++] = type_form;
        bvec_items[bvi++] = dargs[init_idx];
        Form *bvec = form_vec(e->arena, f->span, bvec_items, bvec_len);

        /* Build body from remaining items, recursively splicing. */
        uint32_t tail_n = n - (i + 1);
        Form   **tail   = items + i + 1;
        Form    *body_form;
        if (tail_n == 0) {
            body_form = form_nil(e->arena, span);
        } else {
            Form *spliced_tail = splice_internal_defines(e, tail, tail_n, span);
            if (spliced_tail) {
                body_form = spliced_tail;
            } else if (tail_n == 1) {
                body_form = tail[0];
            } else {
                Form **do_items = (Form **)arena_alloc(e->arena, (tail_n + 1) * sizeof(Form *));
                do_items[0] = form_sym(e->arena, span, e->sym_do);
                for (uint32_t k = 0; k < tail_n; k++) do_items[k + 1] = tail[k];
                body_form = form_list(e->arena, span, do_items, tail_n + 1);
            }
        }

        /* (let [bvec] body_form). */
        Form *let_items[3];
        let_items[0] = form_sym(e->arena, f->span, e->sym_let);
        let_items[1] = bvec;
        let_items[2] = body_form;
        Form *let_form = form_list(e->arena, f->span, let_items, 3);

        /* Items BEFORE the first define are still part of the body and must
         * still run.  Dropping them (the pre-D1 behaviour) silently deleted
         * every statement above the first internal define -- e.g. the
         * (println "before") in
         *   (defn main [] : int (println "before") (define x 1) (println x) 0)
         * never executed. */
        if (i == 0) return let_form;
        Form **pre_items = (Form **)arena_alloc(e->arena, (i + 2) * sizeof(Form *));
        pre_items[0] = form_sym(e->arena, span, e->sym_do);
        for (uint32_t k = 0; k < i; k++) pre_items[k + 1] = items[k];
        pre_items[i + 1] = let_form;
        return form_list(e->arena, span, pre_items, i + 2);
    }

    /* has_define was true but we processed no defines -- shouldn't happen.
     * Return NULL to take the safe caller path. */
    return NULL;
}

/* closure-drop-glue (automatic R3a): a let-binding whose type is a MOVE-ONLY
 * (non-Clone) Drop-instance OPAQUE newtype -- e.g. httpd's `Handler` -- carries a
 * heap fat-closure onion it solely owns.  Such a binding gets the same scope-exit
 * auto-drop the TY_REF (rc/ref) path gets, but dispatched through the type's Drop
 * instance (which routes to TUR_CLOSURE_DROP) instead of the `drop!`->free
 * builtin (a bare free of the past-header fat pointer is the interior-free abort).
 *
 * Restricted, in the conservative "only ever greenlight a drop" spirit, to:
 *   - an opaque ADT with a Drop instance and NO Clone instance (sole-owner, so a
 *     single scope-exit release is correct -- a Clone/refcounted type like String
 *     balances via capture retain/release, not a let scope-exit drop), and
 *   - an initializer that is a genuine fresh-producing CALL (optionally ascribed),
 *     so the binding owns a fresh value rather than aliasing a borrowed handle.
 * The per-binding move/consume filtering (is_moved / is_linear_consumed /
 * is_binding_consumed) is applied by the injection loops exactly as for TY_REF, so
 * a Handler handed to a consumer (server constructor / httpd-call) that moves it is
 * NOT double-dropped.  Returns the resolved Drop instance (for the method binding)
 * or NULL when the binding is not a closure-drop opaque. */
static struct TypeClassInstance *binding_closure_drop_inst(Elab *e, Binding *b,
                                                           Expr *init) {
    if (!b || !init) return NULL;
    /* opaque ADT newtype only */
    if (b->type.kind != TY_ADT || !b->type.as.adt_.def ||
        !b->type.as.adt_.def->is_opaque)
        return NULL;
    /* fresh-producing call initializer (peel ascriptions) */
    const Expr *pinit = init;
    while (pinit && pinit->kind == EX_ASCRIBE) pinit = pinit->as.ascribe_.inner;
    if (!pinit || pinit->kind != EX_CALL) return NULL;
    /* Drop instance present, Clone instance absent (move-only sole owner) */
    const Symbol *drop_name  = intern_cstr(e->st, "Drop");
    const Symbol *clone_name = intern_cstr(e->st, "Clone");
    TypeClass *drop_tc  = typeclass_env_lookup_typeclass(&e->typeclass_env, drop_name);
    TypeClass *clone_tc = typeclass_env_lookup_typeclass(&e->typeclass_env, clone_name);
    if (!drop_tc) return NULL;
    struct TypeClassInstance *di =
        typeclass_env_lookup_instance(&e->typeclass_env, drop_tc, &b->type, 1);
    if (!di || di->n_method_impls == 0 || !di->method_impls[0] ||
        !di->method_impls[0]->binding)
        return NULL;
    if (clone_tc &&
        typeclass_env_lookup_instance(&e->typeclass_env, clone_tc, &b->type, 1))
        return NULL;   /* Clone == shared refcount owner; not a sole-owner drop */
    return di;
}

/* ---- special forms ---- */

/* The parametric ADT a form constructs, when the form is literally a
 * constructor call of one -- `(Empty)`, `(none)`, `(Some x)`.  NULL otherwise. */
static const AdtDef *ctor_call_parametric_adt(Elab *e, const Form *f) {
    if (!e || !f || f->tag != F_LIST || f->as.list.len < 1) return NULL;
    if (f->as.list.items[0]->tag != F_SYM) return NULL;
    CtorDef *c = elab_lookup_ctor(e, f->as.list.items[0]->as.sym);
    if (!c || !c->adt || c->adt->n_type_params == 0) return NULL;
    return c->adt;
}

/* Use-site look-ahead for an UNANNOTATED let binding whose initializer is a
 * bare parametric constructor.
 *
 * `(let [e (Empty)] ... (getv e))` against `getv [b : (Box int)]` has no
 * expected type at the initializer and no annotation to supply one, so
 * `(Empty)` defaults to the bare TY_ADT and emits the carrier `ctor_Empty()`
 * while every consumer reads the `(Box int)` monomorph.  Annotating the binding
 * already fixes it; this finds the same answer without the annotation.
 *
 * Search `f` for a call that passes `name` in a parameter slot declared as a
 * concrete application of `adt`, and return that parameter's type.  Bounded on
 * purpose: syntactic, depth-capped, confined to the let's own text, and gated
 * on the ADT matching, so the type it finds can only ground the constructor to
 * a family it already belongs to.  Same shape as the sibling-argument
 * look-ahead in elab_call.c (poly-hof-constrained-arg-baked-carrier), which
 * resolves a param's tyvars by elaborating siblings early. */
static const Type *let_use_site_app_type(Elab *e, const Form *f,
                                         const Symbol *name, const AdtDef *adt,
                                         uint32_t depth) {
    if (!f || depth == 0) return NULL;
    if (f->tag == F_LIST && f->as.list.len >= 1 &&
        f->as.list.items[0]->tag == F_SYM) {
        Binding *fb = scope_lookup(e->scope, f->as.list.items[0]->as.sym);
        if (fb && fb->type.kind == TY_FN && fb->type.as.fn.arg_full_types) {
            for (uint32_t k = 0; k + 1 < f->as.list.len; k++) {
                const Form *a = f->as.list.items[k + 1];
                if (a->tag != F_SYM || a->as.sym != name) continue;
                uint32_t idx = fb->closure_fn_binding ? k + 1 : k;
                if (idx >= fb->type.as.fn.arity) continue;
                const Type *pt = fb->type.as.fn.arg_full_types[idx];
                if (pt && pt->kind == TY_APP && type_adt_app_def(pt) == adt &&
                    type_app_is_concrete_adt(pt))
                    return pt;
            }
        }
    }
    if (f->tag == F_LIST || f->tag == F_VEC) {
        for (uint32_t k = 0; k < f->as.list.len; k++) {
            const Type *r = let_use_site_app_type(e, f->as.list.items[k], name,
                                                  adt, depth - 1);
            if (r) return r;
        }
    }
    return NULL;
}

/* any-struct-box-leak-per-widen (the early-exit case): move an `any` binding's
 * drop from scope exit to its single consuming call.
 *
 * The scope-exit drop is a TRAILING free -- emitted after the body -- so a
 * `return` or a TCO'd tail call in that body jumps straight past it and the box
 * leaks.  (The closure-env and catch-box frees have the same shape and the same
 * hole; this does not fix those.)  But when the binding is used EXACTLY ONCE,
 * and that use is an argument a callee neither retains nor outlives, the drop
 * does not need the scope at all: the box is dead the moment that call returns,
 * which is a point every path through the body reaches before it can exit.
 *
 * `catch_box_binding_escapes_except` does the counting.  Excluding the one use
 * we found, the binding must not appear anywhere else -- so "exactly once" is
 * established by the same conservative walk that decides escape, not by a
 * separate occurrence counter that could disagree with it.
 *
 * The binding is then flagged so the scope-exit rule skips it; both firing
 * would free the box twice. */
bool any_box_binding_escapes_except(const Expr *e, const Binding *b,
                                    const Expr *ignore);
bool expr_is_fresh_any_closure(const Expr *x);

static const Expr *any_find_sole_drop_use(const Expr *e, const Binding *b);

static void any_let_move_drop_to_use(Expr *let_e) {
    if (!let_e || !let_e->as.let_.bindings) return;
    for (uint32_t i = 0; i < let_e->as.let_.n; i++) {
        LetBinding *lb = &let_e->as.let_.bindings[i];
        if (!lb->binding || !lb->init) continue;
        if (lb->binding->type.kind != TY_ANY) continue;
        if (!any_expr_is_owned_temp(lb->init, 8)) continue;
        /* dynamic-returned-closure-env-is-never-freed: a fresh closure's drop
         * releases its env through the header, which only the scope-exit
         * channel spells (let_binding_widen_drop_stmt); the at-use drop is
         * `__tur_any_drop`, which a "fn" payload passes through untouched.
         * Leave it at scope exit, where early exits fire it too. */
        if (expr_is_fresh_any_closure(lb->init)) continue;
        const Expr *use = any_find_sole_drop_use(let_e->as.let_.body, lb->binding);
        if (!use) continue;
        if (any_box_binding_escapes_except(let_e->as.let_.body, lb->binding, use))
            continue;
        /* A sibling binding's initializer could also reach it. */
        bool other = false;
        for (uint32_t j = 0; j < let_e->as.let_.n && !other; j++)
            if (j != i && any_box_binding_escapes_except(
                              let_e->as.let_.bindings[j].init, lb->binding, NULL))
                other = true;
        if (other) continue;
        ((Expr *)use)->any_drop_after = true;
        lb->binding->any_dropped_at_use = true;
    }
}

/* The first argument position holding a bare reference to `b` whose parameter is
 * droppable -- non-retaining and effect-free, the same pair every other `any`
 * drop rule uses. */
static const Expr *any_find_sole_drop_use(const Expr *e, const Binding *b) {
    if (!e) return NULL;
    const Expr *r = NULL;
    switch (e->kind) {
        case EX_CALL: {
            const Binding *fb = e->as.call_.fn_binding;
            if (fb && !e->as.call_.fn_expr && fb->type.kind == TY_FN
                && effect_row_is_empty(fb->type.as.fn.effect_row)) {
                for (uint32_t i = 0; i < e->as.call_.n_args && i < 32; i++) {
                    const Expr *a = e->as.call_.args[i];
                    while (a && a->kind == EX_ASCRIBE) a = a->as.ascribe_.inner;
                    /* Return the ASCRIBE-PEELED node: it is what the escape
                     * walk encounters, so it is what `ignore` must match, and
                     * it is what emit_value's hook fires on. */
                    if (a && a->kind == EX_VAR && a->as.var.binding == b
                        && (fb->nonretain_ptr_param_mask & (1u << i)))
                        return a;
                }
            }
            for (uint32_t i = 0; i < e->as.call_.n_args; i++)
                if ((r = any_find_sole_drop_use(e->as.call_.args[i], b))) return r;
            return any_find_sole_drop_use(e->as.call_.fn_expr, b);
        }
        case EX_BUILTIN:
            for (uint32_t i = 0; i < e->as.builtin.n; i++)
                if ((r = any_find_sole_drop_use(e->as.builtin.args[i], b))) return r;
            return NULL;
        case EX_IF:
            /* Only the CONDITION is unconditional.  A use inside an arm runs on
             * one path and not the other, so moving the drop there would leak on
             * the path that skips it -- and the scope-exit rule has already been
             * suppressed by then.  The drop has to dominate every exit, which is
             * what restricting the search to unconditional positions buys. */
            return any_find_sole_drop_use(e->as.if_.cond, b);
        case EX_DO:
            for (uint32_t i = 0; i < e->as.do_.n; i++)
                if ((r = any_find_sole_drop_use(e->as.do_.items[i], b))) return r;
            return NULL;
        case EX_LET:
        case EX_LETREC:
            for (uint32_t i = 0; i < e->as.let_.n; i++)
                if ((r = any_find_sole_drop_use(e->as.let_.bindings[i].init, b))) return r;
            return any_find_sole_drop_use(e->as.let_.body, b);
        case EX_REINTERPRET: return any_find_sole_drop_use(e->as.reinterpret_.expr, b);
        case EX_ASCRIBE: return any_find_sole_drop_use(e->as.ascribe_.inner, b);
        case EX_RETURN:  return any_find_sole_drop_use(e->as.return_.value, b);
        default: return NULL;
    }
}

/* let-bound-generic-call-result-in-generic-truncates: inside a generic body, a
 * call to a generic whose declared result is a bare type variable, instantiated
 * to one of the ENCLOSING signature's own type variables -- `(gid x)` with
 * `x : A` inside `(defn wrap [A] ...)` -- is typed as the int64 carrier by
 * elab_call.c (it collapses a bare-tyvar result to `int` plus a reinterpret
 * back, and a reinterpret cannot size a tyvar, so none is added).  Passed
 * straight to another call that is harmless: emit re-targets what it can see.
 * Bound by a `let`, the binding took the `int`, so in a float spec the double
 * the spec returned was VALUE-converted into an int64_t (9.75 -> 9), and at a
 * by-value aggregate the C did not compile.
 *
 * Give the binding the tyvar instead, through an explicit reinterpret typed
 * `A` that emit lowers per clone (emit_expr.c, the EX_REINTERPRET tyvar arm).
 * The binding then behaves exactly like an `A`-typed parameter, a path every
 * consumer already handles.  Only the binding changes: typing the CALL itself
 * `A` was tried and broke emitter paths that rely on the carrier `int` for a
 * call in argument position (the report records which).
 *
 * The tyvar must be one the enclosing signature quantifies (`sig_tyvars`):
 * outside a generic, `(unwrap-or (none) 42)` also binds its result to a type
 * variable -- the unbound one `(none)` minted -- and that must stay `int`. */
static Expr *let_bridge_sig_tyvar_result(Elab *e, Expr *init) {
    if (!init || init->kind != EX_CALL || init->type.kind != TY_INT)
        return init;
    const Binding *fb = init->as.call_.fn_binding;
    if (!fb || fb->type.kind != TY_FN) return init;
    const Type *rft = fb->type.as.fn.result_full_type;
    if (!rft || rft->kind != TY_TYVAR || !rft->as.tyvar_.name) return init;
    for (uint32_t k = 0; k < init->as.call_.n_abi_bindings; k++) {
        const AbiTypeBinding *ab = &init->as.call_.abi_bindings[k];
        if (!ab->name || strcmp(ab->name, rft->as.tyvar_.name) != 0) continue;
        if (ab->type.kind != TY_TYVAR || !ab->type.as.tyvar_.name) return init;
        bool in_sig = false;
        for (uint8_t si = 0; si < e->n_sig_tyvars && !in_sig; si++)
            in_sig = e->sig_tyvars[si] &&
                     strcmp(e->sig_tyvars[si], ab->type.as.tyvar_.name) == 0;
        if (!in_sig) return init;
        Expr *r = expr_new(e->arena, EX_REINTERPRET, ab->type, init->span);
        r->as.reinterpret_.expr = init;
        r->as.reinterpret_.source_kind = TY_INT;
        r->as.reinterpret_.target_kind = TY_TYVAR;
        r->as.reinterpret_.retain = false;
        return r;
    }
    return init;
}

/* ---- compiled-closure-copies-a-captured-mut --------------------------------
 *
 * A compiled closure COPIES each variable it captures into its env when the
 * closure is built; the interpreter's closures share the frame.  So a `^mut`
 * that a lambda captures and something then `set!`s gave two answers:
 *
 *   (let [^mut acc 0]
 *     (apply3 (fn [x : int] : nil (set! acc (+ acc x))))
 *     acc)                       ; compiled 0, interpreted 6
 *
 * and a lambda that only WRITES the variable did not capture it at all (a cc
 * error naming an undeclared local).  Shared capture is the semantics the
 * interpreter, `set!`, and `#lang r7rs` (scheme_lower.c's R10 assignment
 * conversion) all agree on, so the compiled path is brought to it here, for
 * every dialect: such a variable lives in a heap cell (`TurMutCell`,
 * stdlib/pair.tur) bound under a hidden name, and the variable's own name
 * becomes an alias whose reads elaborate as `(.v <cell>)` and whose `set!`s
 * as field writes.  Each closure then captures the cell POINTER -- an
 * ordinary heap pointer every capture path already carries -- and all of them
 * see one location.
 *
 * Which variables: a pure `^mut` (no other substructural annotation) whose
 * name appears inside a `fn` in the rest of the `let` -- a later initializer
 * or the body -- and whose type is a scalar (numbers, bool, cstr, sym) or an
 * `any`.  The scan is syntactic and over-approximates (a shadowing binding
 * inside the lambda still counts), which only costs a cell.  A lambda a MACRO
 * introduces is not seen, and keeps the copying behaviour; so do aggregate,
 * pointer, rc and linear types, whose scope-exit ownership a cell would have
 * to take over. */
static bool cell_form_is_listy(const Form *f) {
    return f && (f->tag == F_LIST || f->tag == F_VEC || f->tag == F_MAP ||
                 f->tag == F_SET || f->tag == F_MAP_LITERAL ||
                 f->tag == F_SET_LITERAL);
}

static bool cell_vec_binds(const Form *v, const Symbol *n) {
    if (!v || v->tag != F_VEC) return false;
    for (uint32_t k = 0; k < v->as.list.len; k++)
        if (v->as.list.items[k]->tag == F_SYM && v->as.list.items[k]->as.sym == n)
            return true;
    return false;
}

/* Does `n` occur free inside a `fn` somewhere in `f`? */
static bool cell_fn_mentions(Elab *e, const Form *f, const Symbol *n,
                             bool in_fn, int depth) {
    if (!f || depth > 256) return false;
    if (f->tag == F_SYM) return in_fn && f->as.sym == n;
    if (!cell_form_is_listy(f)) return false;
    uint32_t len = f->as.list.len;
    if (f->tag == F_LIST && len >= 1 && f->as.list.items[0]->tag == F_SYM) {
        const Symbol *h = f->as.list.items[0]->as.sym;
        if (h == e->sym_quote) return false;
        if (h == e->sym_fn || h == e->sym_lambda) {
            for (uint32_t k = 1; k < len && k <= 2; k++) {
                if (f->as.list.items[k]->tag == F_VEC) {
                    if (cell_vec_binds(f->as.list.items[k], n)) return false;
                    break;
                }
            }
            in_fn = true;
        }
    }
    for (uint32_t k = 0; k < len; k++)
        if (cell_fn_mentions(e, f->as.list.items[k], n, in_fn, depth + 1))
            return true;
    return false;
}

static bool cell_elem_type_ok(Type t) {
    switch (t.kind) {
        case TY_INT: case TY_INT8: case TY_INT16: case TY_INT32: case TY_INT64:
        case TY_UINT8: case TY_UINT16: case TY_UINT32: case TY_UINT64:
        case TY_FLOAT: case TY_FLOAT32: case TY_FLOAT64:
        case TY_BOOL: case TY_CSTR: case TY_SYM: case TY_ANY:
            return true;
        default:
            return false;
    }
}

/* The `(.v <cell>)` read every use of an aliased name elaborates as. */
Form *elab_mut_cell_read_form(Elab *e, const Binding *alias, Span span) {
    Form **items = (Form **)arena_alloc(e->arena, 2 * sizeof(Form *));
    items[0] = form_sym(e->arena, span, intern_cstr(e->st, ".v"));
    items[1] = form_sym(e->arena, span, alias->cell_hidden_sym);
    return form_list(e->arena, span, items, 2);
}

/* After binding `b` (the `^mut` NAME) has been bound to its initializer as an
 * ordinary let binding, turn it into the cell form: `b` keeps the initial
 * value (no longer mutable, and shadowed), a hidden binding holds `(TurMutCell
 * <name>)`, and the name is re-bound as the alias.  False when the cell type
 * is not in scope (a :no-stdlib build) -- the binding then stays as it was. */
static bool elab_let_mut_to_cell(Elab *e, Scope *inner, LetBinding **binds,
                                 bool **moved, uint32_t *n_binds, uint32_t *cap,
                                 Binding *b, const Symbol *name, Span name_span,
                                 bool *ok) {
    *ok = true;
    const Symbol *cell_ty = intern_cstr(e->st, "TurMutCell");
    if (!scope_lookup(e->scope, cell_ty)) return false;
    char hidden[48];
    snprintf(hidden, sizeof hidden, "__tur_mutcell_%u", b->id);
    const Symbol *hidden_sym = intern_cstr(e->st, hidden);
    Form **items = (Form **)arena_alloc(e->arena, 2 * sizeof(Form *));
    items[0] = form_sym(e->arena, name_span, cell_ty);
    items[1] = form_sym(e->arena, name_span, name);
    Expr *cell_init = elab_form(e, form_list(e->arena, name_span, items, 2));
    if (!cell_init) { *ok = false; return false; }
    b->is_mut = false;
    Binding *cb = binding_new(e, hidden_sym, cell_init->type, false, false, name_span);
    cb->is_mut_cell = true;
    scope_add(inner, cb);
    if (*n_binds == *cap) {
        *cap = *cap ? *cap * 2 : 4;
        *binds = (LetBinding *)realloc(*binds, *cap * sizeof(LetBinding));
        *moved = (bool *)realloc(*moved, *cap * sizeof(bool));
        if (!*binds || !*moved) { fprintf(stderr, "tur: oom\n"); abort(); }
    }
    (*binds)[*n_binds].binding = cb;
    (*binds)[*n_binds].init = cell_init;
    (*moved)[*n_binds] = false;
    (*n_binds)++;
    Binding *alias = binding_new(e, name, b->type, true, false, name_span);
    alias->cell_hidden_sym = hidden_sym;
    scope_add(inner, alias);
    return true;
}

Expr *elab_let(Elab *e, const Form *call) {
    /* (let [b1 i1 b2 i2 ...] body...)
     * Named-let: (let name [p1 v1 ...] body...) -- desugar to letrec */
    if (call->as.list.len < 2) {
        diag_emit(DIAG_ERROR, call->span, "let requires a binding vector");
        return NULL;
    }
    /* Dispatch to named-let when item[1] is a symbol and item[2] is a vector. */
    if (call->as.list.len >= 4 &&
        call->as.list.items[1]->tag == F_SYM &&
        call->as.list.items[2]->tag == F_VEC) {
        return elab_named_let(e, call);
    }
    Form *bindings_form = call->as.list.items[1];
    if (bindings_form->tag != F_VEC) {
        diag_emit(DIAG_ERROR, bindings_form->span,
                  "let bindings must be a vector [name init ...]");
        return NULL;
    }

    /* Parse bindings: walk left-to-right, optional ^mut / ^persistent prefix per entry. */
    LetBinding *binds = NULL;
    bool       *binding_moved_during_init = NULL; /* tracks moves of preceding bindings during each init elaboration */
    uint32_t    n_binds = 0, cap = 0;
    Scope       inner;
    scope_init(&inner, e->scope);
    e->scope = &inner;

    int rc = 0;
    uint32_t i = 0;
    while (i < bindings_form->as.list.len) {
        Form *cur = bindings_form->as.list.items[i];
        bool is_mut = false;
        bool is_persistent = false;
        bool is_linear_ann = false;
        bool is_unique_ann = false;
        bool is_affine_ann = false;   /* ST0 */
        bool is_relevant_ann = false; /* ST0 */
        bool is_fat_ann = false;      /* vec-get-typed-fat-closure-readback */
        /* Phase P3 / LT0 / UT0 / UT2 / ST0: Consume binding annotations in any order.
         * Accepted: ^mut, ^persistent, ^linear, ^unique, ^affine, ^relevant, ^fat
         * -- may appear in any combination before the binding name,
         * e.g. [^affine v ...]. */
        {
            bool keep_going = true;
            while (keep_going && cur->tag == F_SYM) {
                if (cur->as.sym == e->sym_caret_mut) {
                    is_mut = true;
                } else if (cur->as.sym == e->sym_caret_persistent) {
                    is_persistent = true;
                } else if (cur->as.sym == e->sym_caret_linear) {
                    is_linear_ann = true;
                } else if (cur->as.sym == e->sym_caret_unique) {
                    is_unique_ann = true;
                } else if (cur->as.sym == e->sym_caret_affine) {
                    is_affine_ann = true;
                } else if (cur->as.sym == e->sym_caret_relevant) {
                    is_relevant_ann = true;
                } else if (cur->as.sym == e->sym_caret_fat) {
                    is_fat_ann = true;
                } else if (cur->as.sym == e->sym_caret_multishot) {
                    /* MS2: ^multishot is only valid on handler continuation bindings */
                    diag_emit_with_code(DIAG_ERROR, cur->span,
                        TUR_E0501_MULTISHOT_ANN_OUTSIDE_HANDLER,
                        "'^multishot' annotation is only valid on a handler continuation, "
                        "not on a let binding");
                    rc = -1; keep_going = false; break;
                } else {
                    break; /* Not an annotation -- this is the binding name */
                }
                i++;
                if (i >= bindings_form->as.list.len) {
                    diag_emit(DIAG_ERROR, cur->span,
                              "trailing annotation '%s' with no binding name",
                              cur->as.sym->name);
                    rc = -1; keep_going = false; break;
                }
                cur = bindings_form->as.list.items[i];
            }
        }
        /* SS0b: Vector destructuring: (let [[a b] init] ...) */
        if (cur->tag == F_VEC) {
            Form *vec_pat = cur;
            Span  vec_span = cur->span;
            i++;
            if (i >= bindings_form->as.list.len) {
                diag_emit(DIAG_ERROR, vec_span,
                          "let vector destructuring is missing its initializer");
                rc = -1; break;
            }
            Form *init_form_v = bindings_form->as.list.items[i++];
            Expr *init_v = elab_form(e, init_form_v);
            if (!init_v) { rc = -1; break; }
            /* Create one binding per name in the pattern vector with placeholder types.
             * The actual types will be verified by codegen (SS2); for SS0b the
             * type-level operations (send/recv/offer/choose-*) already emit their
             * own diagnostics before returning. */
            uint32_t n_elems = vec_pat->as.list.len;
            /* SS2: For session pair types we track the first binding so vi=1 can
             * reference vi=0's C name via __TUR_CAP_0__. */
            uint32_t first_bind_idx = n_binds;
            for (uint32_t vi = 0; vi < n_elems; vi++) {
                Form *vname_f = vec_pat->as.list.items[vi];
                if (vname_f->tag != F_SYM) {
                    diag_emit(DIAG_ERROR, vname_f->span,
                              "vector destructuring elements must be symbols");
                    rc = -1; break;
                }
                /* Determine the type for this element. */
                Type elem_type;
                if (init_v->type.kind == TY_SESSION_PAIR ||
                    init_v->type.kind == TY_SESSION_RECV_PAIR) {
                    Type *fst_t = init_v->type.as.session_.fst;
                    /* SS8: N-role make-protocol pairs: get elem_type from global_t directly */
                    if (init_v->type.kind == TY_SESSION_PAIR && fst_t && fst_t->kind == TY_ROLE) {
                        Type *global_t = fst_t->as.role_.global_type;
                        if ((int)vi < global_t->as.global_.n_roles) {
                            elem_type = type_role(global_t, global_t->as.global_.roles[vi],
                                                  global_t->as.global_.body);
                            elem_type.copy_kind = CK_LINEAR;
                        } else {
                            elem_type = type_from_kind(TY_INT);
                        }
                    } else if (vi == 0 && fst_t) {
                        elem_type = *fst_t;
                    } else if (vi == 1 && init_v->type.as.session_.snd) {
                        elem_type = *init_v->type.as.session_.snd;
                    } else {
                        elem_type = type_from_kind(TY_INT);
                    }
                } else {
                    elem_type = (vi == 0) ? init_v->type : type_from_kind(TY_INT);
                }
                Binding *vb = binding_new(e, vname_f->as.sym, elem_type, false, false, vname_f->span);
                if (elem_type.copy_kind == CK_LINEAR) {
                    vb->is_linear = true;
                }
                scope_add(&inner, vb);
                if (n_binds == cap) {
                    cap = cap ? cap * 2 : 4;
                    binds = (LetBinding *)realloc(binds, cap * sizeof(LetBinding));
                    if (!binds) { fprintf(stderr, "tur: oom\n"); abort(); }
                    binding_moved_during_init = (bool *)realloc(binding_moved_during_init, cap * sizeof(bool));
                    if (!binding_moved_during_init) { fprintf(stderr, "tur: oom\n"); abort(); }
                }
                /* SS2: For session pair types, create separate EX_INLINE_C inits
                 * so each binding gets its own expression (avoids double evaluation). */
                Expr *elem_init = init_v; /* default: share the original init */
                if (init_v->type.kind == TY_SESSION_PAIR) {
                    Type *fst = init_v->type.as.session_.fst;
                    bool is_role_pair = (fst && fst->kind == TY_ROLE);
                    Expr *ic_expr = expr_new(e->arena, EX_INLINE_C, elem_type, vec_span);
                    InlineC *ic = (InlineC *)arena_alloc(e->arena, sizeof(InlineC));
                    memset(ic, 0, sizeof(InlineC));
                    ic->return_type = elem_type;
                    ic->val_exprs = NULL; ic->n_val_exprs = 0;
                    ic->captures = NULL; ic->n_captures = 0;
                    if (is_role_pair) {
                        /* SS7: make-protocol destructuring:
                         *   vi=0: tur_make_roles(N, 0) -- allocates router + role 0
                         *   vi=k: tur_get_role(__TUR_VAL_0__, k) -- peer role on same router */
                        int n_roles = fst->as.role_.global_type->as.global_.n_roles;
                        if (vi == 0) {
                            Buf code_buf; buf_init(&code_buf);
                            buf_printf(&code_buf, "tur_make_roles(%d, 0)", n_roles);
                            char *code_str = (char *)arena_alloc(e->arena, code_buf.len + 1);
                            memcpy(code_str, code_buf.data, code_buf.len);
                            code_str[code_buf.len] = '\0';
                            ic->code = strslice(code_str, (uint32_t)code_buf.len);
                            buf_free(&code_buf);
                        } else {
                            /* vi > 0: tur_get_role(__TUR_VAL_0__, vi) referencing vi=0's binding */
                            Buf code_buf; buf_init(&code_buf);
                            buf_printf(&code_buf, "tur_get_role(__TUR_VAL_0__, %d)", (int)vi);
                            char *code_str = (char *)arena_alloc(e->arena, code_buf.len + 1);
                            memcpy(code_str, code_buf.data, code_buf.len);
                            code_str[code_buf.len] = '\0';
                            ic->code = strslice(code_str, (uint32_t)code_buf.len);
                            buf_free(&code_buf);
                            /* Reference vi=0's binding as __TUR_VAL_0__ */
                            ic->val_exprs = (Expr **)arena_alloc(e->arena, sizeof(Expr *));
                            Expr *r0_var = expr_new(e->arena, EX_VAR,
                                                    binds[first_bind_idx].binding->type, vec_span);
                            r0_var->as.var.binding = binds[first_bind_idx].binding;
                            ic->val_exprs[0] = r0_var;
                            ic->n_val_exprs = 1;
                        }
                    } else {
                        /* Existing make-session logic */
                        if (vi == 0) {
                            /* SS2: Pass protocol name for debug tag: tur_session_new(TUR_DBGPROTO("...")) */
                            Buf code_buf;
                            buf_init(&code_buf);
                            buf_puts(&code_buf, "tur_session_new(TUR_DBGPROTO(\"");
                            /* init_v->type is TY_SESSION_PAIR; fst is Session[P]; fst->fst is P */
                            Type *sess_p = init_v->type.as.session_.fst;
                            if (sess_p && sess_p->as.session_.fst) {
                                type_print(&code_buf, *sess_p->as.session_.fst);
                            } else {
                                buf_puts(&code_buf, "?");
                            }
                            buf_puts(&code_buf, "\"))");
                            char *code_str = (char *)arena_alloc(e->arena, code_buf.len + 1);
                            memcpy(code_str, code_buf.data, code_buf.len);
                            code_str[code_buf.len] = '\0';
                            ic->code = strslice(code_str, (uint32_t)code_buf.len);
                            buf_free(&code_buf);
                        } else {
                            static const char ref_code[] = "__TUR_VAL_0__";
                            ic->code = strslice(ref_code, sizeof(ref_code) - 1);
                            ic->val_exprs = (Expr **)arena_alloc(e->arena, sizeof(Expr *));
                            Expr *chan_var = expr_new(e->arena, EX_VAR,
                                                      binds[first_bind_idx].binding->type, vec_span);
                            chan_var->as.var.binding = binds[first_bind_idx].binding;
                            ic->val_exprs[0] = chan_var;
                            ic->n_val_exprs = 1;
                        }
                    }
                    ic_expr->as.inline_c_.inline_c = ic;
                    elem_init = ic_expr;
                } else if (init_v->type.kind == TY_SESSION_RECV_PAIR) {
                    /* recv / recv-timeout / recv-from destructuring.
                     * Case A -- plain recv (EX_INLINE_C, code is recv-pair sentinel):
                     *   vi=0: tur_session_recv(__TUR_VAL_0__)
                     *   vi=1: __TUR_VAL_0__ (channel ptr)
                     * Case B -- recv-timeout Left arm (init_v is EX_VAR of type RecvPair):
                     *   vi=0: tur__rtv_ (value stashed by tur_session_recv_timeout)
                     *   vi=1: __TUR_VAL_0__ (the arm var = channel ptr)
                     * Case C -- recv-timeout inline_c
                     *   (code starts with "tur_session_recv_timeout"):
                     *   vi=0: tur__rtv_
                     *   vi=1: __TUR_VAL_0__ (channel)
                     * Case D -- SS7 recv-from (EX_INLINE_C, snd type is TY_ROLE):
                     *   vi=0: tur_router_recv(__TUR_VAL_0__, from_idx)
                     *   vi=1: __TUR_VAL_0__ (role channel ptr) */
                    Type *recv_snd = init_v->type.as.session_.snd;
                    bool is_role_recv = (recv_snd && recv_snd->kind == TY_ROLE
                                         && init_v->kind == EX_INLINE_C);
                    bool is_recv_timeout_var = (!is_role_recv && init_v->kind == EX_VAR);
                    bool is_recv_timeout_ic = false;
                    Binding *chan_b = NULL;
                    if (!is_role_recv && !is_recv_timeout_var && init_v->kind == EX_INLINE_C) {
                        InlineC *orig_ic = init_v->as.inline_c_.inline_c;
                        is_recv_timeout_ic = (orig_ic->code.len > 24 &&
                            memcmp(orig_ic->code.p, "tur_session_recv_timeout", 24) == 0);
                        chan_b = (orig_ic->n_val_exprs > 0 && orig_ic->val_exprs[0]
                            && orig_ic->val_exprs[0]->kind == EX_VAR)
                            ? orig_ic->val_exprs[0]->as.var.binding : NULL;
                    }
                    bool is_recv_timeout = is_recv_timeout_var || is_recv_timeout_ic;
                    Expr *ic_expr = expr_new(e->arena, EX_INLINE_C, elem_type, vec_span);
                    InlineC *ic = (InlineC *)arena_alloc(e->arena, sizeof(InlineC));
                    memset(ic, 0, sizeof(InlineC));
                    ic->return_type = elem_type;
                    ic->captures = NULL; ic->n_captures = 0;
                    if (is_role_recv) {
                        /* SS7: recv-from destructuring:
                         *   vi=0: the tur_router_recv(...) code embedded by elab_recv_from
                         *   vi=1: __TUR_VAL_0__ (same role channel pointer) */
                        InlineC *orig_ic = init_v->as.inline_c_.inline_c;
                        Binding *role_b = (orig_ic->n_val_exprs > 0 && orig_ic->val_exprs[0]
                            && orig_ic->val_exprs[0]->kind == EX_VAR)
                            ? orig_ic->val_exprs[0]->as.var.binding : NULL;
                        if (vi == 0) {
                            /* session-payloads-are-int64-only: convert the
                             * router's int64 word back to the payload type. */
                            const char *wrapped = session_payload_from_word(
                                e, elem_type, orig_ic->code.p, orig_ic->code.len);
                            ic->code = strslice(wrapped, (uint32_t)strlen(wrapped));
                            if (role_b) {
                                ic->val_exprs = (Expr **)arena_alloc(e->arena, sizeof(Expr *));
                                Expr *role_var = expr_new(e->arena, EX_VAR, role_b->type, vec_span);
                                role_var->as.var.binding = role_b;
                                ic->val_exprs[0] = role_var;
                                ic->n_val_exprs = 1;
                            } else { ic->val_exprs = NULL; ic->n_val_exprs = 0; }
                        } else {
                            /* vi=1: same void* role channel pointer */
                            static const char ref_role[] = "__TUR_VAL_0__";
                            ic->code = strslice(ref_role, sizeof(ref_role) - 1);
                            if (role_b) {
                                ic->val_exprs = (Expr **)arena_alloc(e->arena, sizeof(Expr *));
                                Expr *role_var = expr_new(e->arena, EX_VAR, role_b->type, vec_span);
                                role_var->as.var.binding = role_b;
                                ic->val_exprs[0] = role_var;
                                ic->n_val_exprs = 1;
                            } else { ic->val_exprs = NULL; ic->n_val_exprs = 0; }
                        }
                    } else if (vi == 0) {
                        if (is_recv_timeout) {
                            /* SS3c: value is in tur__rtv_ thread-local;
                             * converted back from the int64 word to the payload
                             * type (session-payloads-are-int64-only). */
                            static const char rtv_code[] = "tur__rtv_";
                            const char *wrapped = session_payload_from_word(
                                e, elem_type, rtv_code, sizeof(rtv_code) - 1);
                            ic->code = strslice(wrapped, (uint32_t)strlen(wrapped));
                            ic->val_exprs = NULL; ic->n_val_exprs = 0;
                        } else {
                            static const char recv_code[] = "tur_session_recv(__TUR_VAL_0__)";
                            /* Converted back from the int64 word to the
                             * payload type (session-payloads-are-int64-only):
                             * a double is bit-reinterpreted, a pointer cast
                             * through intptr_t.  The interpreter peels the
                             * wrapper before its prefix match. */
                            const char *wrapped = session_payload_from_word(
                                e, elem_type, recv_code, sizeof(recv_code) - 1);
                            ic->code = strslice(wrapped, (uint32_t)strlen(wrapped));
                            if (chan_b) {
                                ic->val_exprs = (Expr **)arena_alloc(e->arena, sizeof(Expr *));
                                Expr *chan_var = expr_new(e->arena, EX_VAR, chan_b->type, vec_span);
                                chan_var->as.var.binding = chan_b;
                                ic->val_exprs[0] = chan_var;
                                ic->n_val_exprs = 1;
                            } else { ic->val_exprs = NULL; ic->n_val_exprs = 0; }
                        }
                    } else {
                        /* vi=1: the channel pointer */
                        static const char ref_code2[] = "__TUR_VAL_0__";
                        ic->code = strslice(ref_code2, sizeof(ref_code2) - 1);
                        if (is_recv_timeout_var) {
                            /* init_v IS the arm variable (a void* channel pointer) */
                            ic->val_exprs = (Expr **)arena_alloc(e->arena, sizeof(Expr *));
                            ic->val_exprs[0] = init_v;  /* EX_VAR for the arm binding */
                            ic->n_val_exprs = 1;
                        } else if (chan_b) {
                            ic->val_exprs = (Expr **)arena_alloc(e->arena, sizeof(Expr *));
                            Expr *chan_var = expr_new(e->arena, EX_VAR, chan_b->type, vec_span);
                            chan_var->as.var.binding = chan_b;
                            ic->val_exprs[0] = chan_var;
                            ic->n_val_exprs = 1;
                        } else { ic->val_exprs = NULL; ic->n_val_exprs = 0; }
                    }
                    ic_expr->as.inline_c_.inline_c = ic;
                    elem_init = ic_expr;
                }
                binds[n_binds].binding = vb;
                binds[n_binds].init = elem_init;
                binding_moved_during_init[n_binds] = false;
                n_binds++;
            }
            continue;
        }
        if (cur->tag != F_SYM) {
            diag_emit(DIAG_ERROR, cur->span,
                      "let binding name must be a symbol, got %s",
                      cur->tag == F_INT ? "an integer" :
                      cur->tag == F_STR ? "a string"   :
                      cur->tag == F_KEYWORD ? "a keyword" : "non-symbol");
            rc = -1; break;
        }
        const Symbol *name = cur->as.sym;
        Span name_span = cur->span;
        i++;
        /* Optional type annotation between name and init:
         *   fused:  (let [x :int 5] ...)
         *   spaced: (let [x : int 5] ...)
         * Spaced (F_TYPE_ANN) is unambiguous and always consumed.  Fused
         * (F_KEYWORD) overlaps with keyword *values* (e.g. a macro template
         * `(let [__k ~k] ...)` where `k` is bound to `:foo`), so only consume
         * a bare keyword when it resolves to a known TypeKind.  Anything else
         * is treated as the initializer. */
        const Form *type_ann_form = NULL;
        if (i < bindings_form->as.list.len) {
            Form *maybe_ann = bindings_form->as.list.items[i];
            if (maybe_ann->tag == F_TYPE_ANN) {
                type_ann_form = maybe_ann;
                i++;
            } else if (maybe_ann->tag == F_KEYWORD &&
                       maybe_ann->as.sym != NULL &&
                       typekind_from_symbol(maybe_ann->as.sym->name) != TY_UNKNOWN) {
                type_ann_form = maybe_ann;
                i++;
            }
        }
        if (i >= bindings_form->as.list.len) {
            diag_emit(DIAG_ERROR, cur->span,
                      "let binding for '%s' is missing its initializer",
                      name->name);
            rc = -1; break;
        }
        Form *init_form = bindings_form->as.list.items[i++];

        /* Task 1 (Prereq 1): Snapshot move-state of preceding bindings before elaborating this init.
         * This lets us detect which preceding bindings are moved during this init's elaboration. */
        bool *moved_snapshot = NULL;
        if (n_binds > 0) {
            moved_snapshot = (bool *)malloc(n_binds * sizeof(bool));
            if (!moved_snapshot) { fprintf(stderr, "tur: oom\n"); abort(); }
            for (uint32_t j = 0; j < n_binds; j++) {
                moved_snapshot[j] = binds[j].binding->is_moved;
            }
        }

        /* Phase P3: flag ^persistent RHS so map-new can lower to hamt/new */
        bool prev_in_persistent_let = e->in_persistent_let;
        if (is_persistent) e->in_persistent_let = true;
        /* generic-return-type-not-inferred-from-context: when the binding
         * carries a type annotation, push it onto the expected-type channel
         * so a generic `(call ...)` whose `[A]` lives only on the result
         * can bind A to the annotation.  Skip ^fat (it re-types the binding
         * after the fact and the annotation is not a return-position type). */
        Type *prev_expected = e->expected_type;
        Type *let_init_expected = NULL;
        if (type_ann_form && !is_fat_ann) {
            let_init_expected = fn_type_from_form(e, type_ann_form, NULL, NULL, 0);
            if (let_init_expected) e->expected_type = let_init_expected;
        }
        /* No annotation, and the initializer is a bare parametric constructor:
         * look ahead to a use in this let's own text for the family.  See
         * let_use_site_app_type. */
        if (!let_init_expected && !is_fat_ann) {
            const AdtDef *cadt = ctor_call_parametric_adt(e, init_form);
            if (cadt) {
                const Type *found = NULL;
                for (uint32_t bi = i; !found && bi < bindings_form->as.list.len; bi++)
                    found = let_use_site_app_type(e, bindings_form->as.list.items[bi],
                                                  name, cadt, 8);
                for (uint32_t bi = 2; !found && bi < call->as.list.len; bi++)
                    found = let_use_site_app_type(e, call->as.list.items[bi],
                                                  name, cadt, 8);
                if (found) e->expected_type = (Type *)found;
            }
        }
        Expr *init = elab_form(e, init_form);
        e->expected_type = prev_expected;
        e->in_persistent_let = prev_in_persistent_let;

        /* Task 2 (Prereq 1): Capture which preceding bindings were newly moved during this init elaboration. */
        if (moved_snapshot) {
            for (uint32_t j = 0; j < n_binds; j++) {
                if (!moved_snapshot[j] && binds[j].binding->is_moved) {
                    binding_moved_during_init[j] = true;
                }
            }
            free(moved_snapshot);
        }

        if (!init) { rc = -1; break; }

        /* fat-let-of-thin-fn-stores-code-pointer: `^fat` promises a fat
         * `{ thunk, env }` handle, and the binding is fat-dispatched and
         * handed to fat sinks as one -- but a captureless lambda or a named
         * defn is a bare code pointer, and was stored as it was.  The first
         * fat call read the function's own machine code as slot 0: a SIGSEGV
         * with no diagnostic, where the interpreter answered.  Box it here,
         * as the `^fat` argument and `^mut` fn-cell sites already do.  Only a
         * value that is provably thin: a lifted captureless lambda or a defn
         * referenced by name. */
        if (is_fat_ann && init->kind == EX_VAR && init->as.var.binding) {
            const Binding *ib = init->as.var.binding;
            if (ib->is_global && !ib->is_fat && !ib->is_poly_fn &&
                (ib->is_lifted_lambda || ib->source_fn_def) &&
                init->type.kind == TY_FN && !init->type.as.fn.boxed &&
                !init->type.as.fn.cfnptr)
                init = elab_fn_value_to_fat(e, init);
        }

        /* let-binding-void-call-emits-invalid-c: a `:void` init has no value to
         * name.  Left to run, the emitter writes `void x = ...;` -- "variable
         * has incomplete type 'void'", a cc error with no .tur attribution,
         * several frames from anything the author wrote.  EVERY :void init
         * fails that way today (checked: void defn call, while, println, and a
         * non-first binding), so rejecting here breaks nothing that currently
         * compiles -- it only moves the report to the binding that caused it.
         *
         * Sequencing a side effect in a binding list is a natural thing to
         * reach for when the `let` body must end in a particular value (an
         * `it` body that has to yield a bool, say), so the message names `do`
         * as the fix rather than only stating the rule. */
        /* r7rs-lang-plan R8: in a dynamic file every expression has a value --
         * Scheme's `(let ((r (display x))) ...)` binds the unspecified value
         * (and `guard` lowers its body to exactly that binding).  Widen the
         * nil to `any`, the same widen a nil argument in value position
         * already gets, rather than refusing the binding. */
        if (init->type.kind == TY_NIL && lang_span_is_dynamic(init_form->span)) {
            Expr *w = elab_coerce_to_any(e, init);
            if (w) init = w;
        }
        if (init->type.kind == TY_NIL) {
            diag_emit_with_code(DIAG_ERROR, init_form->span,
                TUR_E0023_BIND_VOID_EXPRESSION,
                "cannot bind '%s' to an expression of type :void; "
                "sequence it with `do` instead of binding it",
                name && name->name ? name->name : "_");
            rc = -1; break;
        }

        /* Verify the optional type annotation against the elaborated init type.
         * For now we compare TypeKind for the common primitive cases (int,
         * float, bool, cstr, nil/void, ptr) -- enough to reject obvious
         * mistakes like `(let [x : int "hello"] ...)`.  Complex types
         * (structs, ADTs, arrows) parse but skip the equality check;
         * downstream typing rules still apply to the init expression. */
        bool ann_is_any = false;
        if (type_ann_form) {
            Type *ann_ty = fn_type_from_form(e, type_ann_form, NULL, NULL, 0);
            if (ann_ty) {
                ann_is_any = (ann_ty->kind == TY_ANY);
                /* bare-fat-param-non-int-result (Phase A4): a declared-typed
                 * binding whose init's tail is a bare-^fat int64 call carries no
                 * recorded result type; infer it from the annotation and re-stamp
                 * the call before the kind-match check below.  See
                 * docs/archive/history/bare-fat-result-type-inference-plan.md. */
                if (kind_is_non_int_register_class(ann_ty->kind) &&
                    retype_bare_fat_tail_calls(init, ann_ty->kind) &&
                    init->type.kind == TY_INT) {
                    init->type = type_from_kind(ann_ty->kind);
                }
                TypeKind ak = ann_ty->kind, ik = init->type.kind;
                bool primitive = (ak == TY_INT || ak == TY_FLOAT ||
                                  ak == TY_BOOL || ak == TY_CSTR ||
                                  ak == TY_NIL || ak == TY_PTR_VOID);
                if (primitive && ak != ik) {
                    diag_emit(DIAG_ERROR, type_ann_form->span,
                        "let binding '%s': type annotation does not match "
                        "initializer (annotated %s, got %s)",
                        name->name,
                        typekind_to_string(ak), typekind_to_string(ik));
                    rc = -1; break;
                }
                /* committed-applied-let-annotation: an applied annotation was
                 * parsed and then not compared at all, so `[o : (Option int)
                 * (some 7.1)]`, `[o : (Vec int) (some 1)]` and `[o : (Option
                 * int) 7]` were accepted -- the binding silently took the
                 * initializer's type, or an int where an Option was claimed. */
                if (!primitive && applied_type_conflict(*ann_ty, init->type)) {
                    Buf ab; buf_init(&ab); type_print(&ab, *ann_ty); buf_putc(&ab, '\0');
                    Buf ib; buf_init(&ib); type_print(&ib, init->type); buf_putc(&ib, '\0');
                    diag_emit_with_code(DIAG_ERROR, type_ann_form->span,
                        TUR_E0001_TYPE_MISMATCH,
                        "let binding '%s': type annotation does not match "
                        "initializer (annotated %s, got %s)",
                        name->name, ab.data, ib.data);
                    buf_free(&ab); buf_free(&ib);
                    rc = -1; break;
                }
            }
            /* gadt-length-index-not-enforced: an annotated GADT index is a
             * claim about the initializer -- `[v : (Vec (Succ Zero)) (VNil)]`
             * is false. */
            const Form *got = NULL;
            if (gadt_claim_disagrees(e, type_ann_form, init, &got)) {
                const Form *ann = type_ann_form;
                while (ann && ann->tag == F_TYPE_ANN && ann->as.list.len == 1)
                    ann = ann->as.list.items[0];
                Buf cb; buf_init(&cb); form_print(&cb, ann); buf_putc(&cb, '\0');
                Buf gb; buf_init(&gb); form_print(&gb, got); buf_putc(&gb, '\0');
                diag_emit_with_code(DIAG_ERROR, type_ann_form->span,
                    TUR_E0001_TYPE_MISMATCH,
                    "let binding '%s': annotated %s, but the initializer has type %s",
                    name->name, cb.data, gb.data);
                buf_free(&cb); buf_free(&gb);
                rc = -1; break;
            }
        }

        /* Phase 11: Move tracking - if init is a CK_MOVE binding reference, poison it */
        if (init->kind == EX_VAR && type_is_move(init->as.var.binding->type)) {
            binding_mark_moved(init->as.var.binding, init_form->span);
        }

        /* UT1: Alias tracking.
         * (a) When a CK_COPY (non-unique) binding is copied into a new binding,
         *     mark the source as AS_ALIASED so that passing it as ^unique later
         *     is caught (TUR_E0200).
         * (b) When a ^unique binding is transferred (moved), propagate is_unique
         *     to the destination so uniqueness is preserved through rebinding. */
        if (init->kind == EX_VAR) {
            Binding *src = init->as.var.binding;
            if (!src->is_unique && type_is_copy(src->type)) {
                /* CK_COPY binding copied into new name — record alias */
                src->alias_state = AS_ALIASED;
                src->alias_name  = name;
            }
        }

        /* any-coercion-not-driven-by-expected-type: an `: any` annotation is a
         * WIDENING REQUEST, not just a claim to check.
         *
         * The binding below takes `init->type`, and the annotation above is only
         * consulted for a mismatch check that skips non-primitives -- so
         * `(let [x : any 42] ...)` bound `x` at `int` and the annotation did
         * nothing at all.  `type-of` then rejected `x` for not being an `any`,
         * which reads as a type error when it is really a missing coercion.
         *
         * Widen here, AFTER the move/alias tracking above: those inspect
         * `init->kind == EX_VAR`, and coercing earlier would wrap the init in
         * EX_UNION_INJECT and silently switch both off for every `: any`
         * binding. */
        if (ann_is_any && init->type.kind != TY_ANY &&
            init->type.kind != TY_NEVER) {
            init = elab_coerce_to_any(e, init);
            if (!init) { rc = -1; break; }
        }
        /* fn-cell-set-with-capturing-closure-segfaults: a `^mut` FUNCTION cell
         * is fat from the start.  A cell initialised with a non-capturing
         * lambda was spelled as a thin function pointer, and a later `set!` of
         * a capturing closure -- a fat `{thunk, env}` box, whose declared fn
         * type is equal to the cell's -- stored the box's address where a code
         * pointer was expected; the next call jumped into the box.  Both
         * representations are legitimate values of the cell's type, so the
         * cell takes the one that can hold either: its init is shimmed to fat
         * (a link-time constant for a lifted lambda, so no allocation) and the
         * binding is marked boxed, which is what makes every call through it
         * dispatch through slot 0 and every later `set!` normalise its value
         * the same way.  An immutable binding is never re-pointed and keeps
         * the thin spelling. */
        if (is_mut && init->type.kind == TY_FN && !init->type.as.fn.boxed) {
            /* A fn PARAMETER is already fat by the parameter protocol
             * (fn_param_type_is_fat_normalized) although its type is not
             * marked boxed; shimming it again would box a box.  Such an init
             * keeps today's spelling (see
             * docs/reported/let-alias-of-fn-param-call-undeclared.md for what
             * that spelling does). */
            const Expr *ip = init;
            while (ip && ip->kind == EX_ASCRIBE) ip = ip->as.ascribe_.inner;
            bool init_is_fat_param = ip && ip->kind == EX_VAR && ip->as.var.binding &&
                (ip->as.var.binding->is_fat ||
                 (ip->as.var.binding->is_param &&
                  fn_param_type_is_fat_normalized(&ip->as.var.binding->type)));
            if (!init_is_fat_param) {
                Expr *fat = elab_fn_value_to_fat(e, init);
                if (fat != init) init = fat;
            }
        }

        /* let-bound-generic-call-result-in-generic-truncates: see
         * let_bridge_sig_tyvar_result.  After every init rewrite above, so an
         * annotation-driven widen or fn shim is never second-guessed. */
        init = let_bridge_sig_tyvar_result(e, init);

        Binding *b = binding_new(e, name, init->type, is_mut, false, name_span);
        /* perform-does-not-typecheck-its-arguments: an UNANNOTATED `let` bound
         * directly to a carrier-defaulted parameter inherits that provenance --
         * its TY_INT is the same "one untyped word", one indirection along.
         * Without this, `(fn [msg] (let [m msg] (perform (Log m))))` would look
         * like a declared int and be reported, while the `msg` spelling one
         * line up is correctly declined.  An annotated binding declares its own
         * type and never inherits. */
        if (!type_ann_form) {
            const Expr *ip = init;
            while (ip) {
                if (ip->kind == EX_ASCRIBE)          ip = ip->as.ascribe_.inner;
                else if (ip->kind == EX_CAST)        ip = ip->as.cast_.expr;
                else if (ip->kind == EX_REINTERPRET) ip = ip->as.reinterpret_.expr;
                else break;
            }
            if (ip && ip->kind == EX_VAR && ip->as.var.binding &&
                ip->as.var.binding->type_is_carrier_default)
                b->type_is_carrier_default = true;
        }
        /* SZ8 projection-size recovery: retain a type-annotation Form so a
         * downstream call passing `b` (or a field projection of it) can recover
         * its static size index.  Prefer an explicit binding annotation; else
         * inherit the initializer call's declared return-type Form (e.g.
         * `a (mk-2)` where `mk-2 : (SizedBuf (Static 2))`). */
        if (type_ann_form) {
            b->decl_type_form = type_ann_form;
        } else {
            /* declared-size-index-never-checked-against-value (B): route
             * through the shared recovery so an ASCRIBED init
             * (`b (:: (mk) (SizedBuf (Static 4)))`) pins the binding's index
             * exactly as a call init does. */
            b->decl_type_form = sz_recover_type_form(e, init);
        }
        /* TY4: borrow-escape at a let binding.  If the init is a borrow of a
         * referent that lives in a deeper (shorter-lived) scope than this
         * binding, the borrow would outlive the value it points to. */
        {
            const Binding *ref = borrow_referent_binding(init);
            if (ref && !ref->is_global && ref->scope_depth > b->scope_depth) {
                diag_emit_with_code(DIAG_ERROR, init->span,
                    TUR_E0105_BORROW_ESCAPES_SCOPE,
                    "`%s` borrows `%s`, which does not live long enough "
                    "(the borrow outlives the value it points to)",
                    name->name, ref->name->name);
                rc = -1;
                break;
            }
        }
        b->is_persistent = is_persistent;
        /* LT0: Mark binding as linear if annotated with ^linear or if initializer
         * type has CK_LINEAR (e.g., returned from a function returning lref<T>). */
        if (is_linear_ann || init->type.copy_kind == CK_LINEAR) {
            b->is_linear = true;
            /* Upgrade copy_kind to CK_LINEAR on the binding's type */
            b->type.copy_kind = CK_LINEAR;
        }
        /* UT0: Mark binding as unique if annotated with ^unique.
         *
         * closure-capture-escapes-linearity: also when the initializer is a
         * CLOSURE that inherited CK_UNIQUE from a captured unique value it
         * consumes (elab_fns.c).  Deliberately narrower than the CK_LINEAR case
         * above, which accepts any initializer type: CK_UNIQUE is carried by
         * ordinary `ref<T>` values too, so inferring from it in general would
         * silently make every `(let [r (ref 7)] ...)` unique -- a much larger
         * behaviour change than this report calls for.  A TY_FN is the only
         * shape that can pick up CK_UNIQUE the new way. */
        if (is_unique_ann
            || (init->type.kind == TY_FN && init->type.copy_kind == CK_UNIQUE)) {
            b->is_unique = true;
            b->type.copy_kind = CK_UNIQUE;
        }
        /* ST0: Mark binding as affine if annotated with ^affine */
        if (is_affine_ann) {
            b->is_affine = true;
            b->type.substruct = SK_AFFINE;
        }
        /* ST0: Mark binding as relevant if annotated with ^relevant */
        if (is_relevant_ann) {
            b->is_relevant = true;
            b->type.substruct = SK_RELEVANT;
        }
        /* vec-get-typed-fat-closure-readback: ^fat re-types a :ptr<void> fat-box
         * init (the standard shape produced by a fat-closure-returning helper,
         * e.g. an SF-fold's (chain-loop ...) result) into a directly-callable
         * fat closure.  The fn-type annotation supplies the call signature;
         * is_fat routes (out args) through the fat-dispatch path -- exactly the
         * syntax the "declare it as a fat closure parameter (^fat out :(fn ...))"
         * help text recommends.  Without the annotation, a bare ^fat keeps the
         * init's :ptr<void> type but is still fat-callable. */
        if (is_fat_ann) {
            b->is_fat = true;
            if (type_ann_form) {
                Type *fat_ty = fn_type_from_form(e, type_ann_form, NULL, NULL, 0);
                if (fat_ty && fat_ty->kind == TY_FN) {
                    b->type = *fat_ty;
                }
            }
        }
        /* UT1: Propagate is_unique through ownership transfer (let [y x] where x is ^unique) */
        if (!is_unique_ann &&
            init->kind == EX_VAR && init->as.var.binding->is_unique) {
            b->is_unique = true;
            /* copy_kind is already CK_UNIQUE, inherited from init->type */
        }
        /* UT2: Infer uniqueness from the initializer's type.  When the RHS is a
         * uniquely-owned value (CK_UNIQUE -- e.g. a ref<T> factory result or a
         * call whose return type is ^unique) and the binding carries no explicit
         * ^unique annotation, promote the type-level uniqueness signal onto the
         * binding so the UT1 alias / use-after-consume checks (TUR-E0200 /
         * TUR-E0201) apply.  This makes hand-annotating ^unique unnecessary in
         * the common "let-bind a unique factory result" shape.
         *
         * A ref obtained from `ref/from-rc` is excluded.  The original reason
         * given -- "it shares the rc's payload ... a non-owning view, not a
         * unique owner" -- is FALSE: tur_ref_from_rc destroys the control block
         * and hands the payload over, so the ref is its sole owner.  Believing
         * otherwise at the sibling site suppressed the scope-exit auto-drop and
         * leaked the payload (fixed; see
         * docs/archive/history/ref-from-rc-orphans-the-payload.md).
         *
         * The exclusion is kept here anyway, and the stated cost -- "marking it
         * unique would reject legitimate re-reads" -- is also wrong: a re-read
         * is already rejected as use-after-move (TUR-E0005), uniqueness or not.
         * It is kept because removing it is not currently observable. Checked:
         * the full suite is 2694/0 either way, aliasing is caught by
         * move-checking with or without it, an explicit `(drop! r)` does not
         * double-free, and a `^unique ^mut` parameter accepts the ref both
         * ways.  `type_ref` sets CK_UNIQUE, so the branch IS reachable -- the
         * binding simply disagrees with its own type's copy_kind.  Since
         * `is_unique` only gates additional alias diagnostics, the exclusion
         * can at worst MISS a diagnostic, never invent one.  Left as-is rather
         * than changed on a premise nobody can currently test. */
        if (!is_unique_ann && !b->is_unique &&
            ty_is_unique(init->type) &&
            init->kind != EX_REF_FROM_RC) {
            b->is_unique = true;
            b->type.copy_kind = CK_UNIQUE;
        }
        /* ST1: Propagate affine/relevant through ownership transfer (let [y x] where x is ^affine/^relevant) */
        if (!is_affine_ann && !is_relevant_ann &&
                init->kind == EX_VAR) {
            Binding *src = init->as.var.binding;
            if (src->is_affine) {
                b->is_affine = true;
                b->type.substruct = SK_AFFINE;
            }
            if (src->is_relevant) {
                b->is_relevant = true;
                b->type.substruct = SK_RELEVANT;
            }
        }
        /* ST3: Propagate affine/relevant through expression type (e.g. from must-use macro
         * returning an EX_LET whose type carries SK_RELEVANT). Only fires when not already
         * set by the EX_VAR propagation above. */
        if (!is_affine_ann && !is_relevant_ann &&
                !b->is_affine && !b->is_relevant && init->kind != EX_VAR) {
            if (init->type.substruct == SK_RELEVANT) {
                b->is_relevant = true;
                b->type.substruct = SK_RELEVANT;
            } else if (init->type.substruct == SK_AFFINE) {
                b->is_affine = true;
                b->type.substruct = SK_AFFINE;
            }
        }
        /* ST2: ref<T> bindings are inferred as SK_LINEAR unless an explicit
         * substructural annotation is already present. */
        if (!is_linear_ann && !is_affine_ann && !is_relevant_ann
                && !b->is_linear && init && init->type.kind == TY_REF) {
            b->is_linear = true;
            b->type.substruct = SK_LINEAR;
        }
        scope_add(&inner, b);

        if (n_binds == cap) {
            cap = cap ? cap * 2 : 4;
            binds = (LetBinding *)realloc(binds, cap * sizeof(LetBinding));
            if (!binds) { fprintf(stderr, "tur: oom\n"); abort(); }
            binding_moved_during_init = (bool *)realloc(binding_moved_during_init, cap * sizeof(bool));
            if (!binding_moved_during_init) { fprintf(stderr, "tur: oom\n"); abort(); }
        }
        
        /* RT1: a `let` bound directly to a lambda literal inherits that
         * lambda's contract parameters, so `(let [f (fn [x : Pos] ...)] (f 0))`
         * checks its argument the way a call to a named function does.  Read
         * straight off the init expression's FnDef rather than chasing the
         * closure-binding graph: a non-capturing lambda has no closure box, so
         * closure_fn_binding is not set for it and that route would miss
         * exactly the simplest case. */
        {
            /* A non-capturing lambda is returned as an EX_VAR naming its lifted
             * thunk binding; a capturing one as an EX_FN_DEF.  Both carry the
             * contract parameters on the FnDef's binding. */
            const Binding *lam = NULL;
            if (init && init->kind == EX_VAR)
                lam = init->as.var.binding;
            else if (init && init->kind == EX_FN_DEF && init->as.fn_def_.fn)
                lam = init->as.fn_def_.fn->binding;
            if (lam && lam->refine_param_preds) {
                b->refine_param_preds = lam->refine_param_preds;
                b->refine_param_vars  = lam->refine_param_vars;
                b->refine_param_names = lam->refine_param_names;
                b->n_refine_params    = lam->n_refine_params;
            }
        }

        /* Propagate closure metadata through lets so a binding produced by a
         * closure literal or a closure-returning call remains callable with the
         * underlying thunk signature.
         *
         * fat-let-of-thin-fn-stores-code-pointer: not through a `^fat x : (fn
         * ...)` binding.  The annotation IS the call signature (it re-types
         * the binding above), and a call through it is a fat dispatch at that
         * signature.  Recording the producing lambda instead made `(h 7.1)`
         * call the lambda directly at the GENERIC lambda's types -- `(>>> f
         * g)`'s `(fn [x : A] : C ...)` -- so 7.1 was passed as its carrier
         * bits to the spec clone's `double` parameter and the result printed
         * as an int (-9223372036854775808, where the interpreter said 16.7). */
        bool fat_ann_typed = is_fat_ann && type_ann_form && b->type.kind == TY_FN;
        if (init && !fat_ann_typed) {
            /* curried-fn-typed-param: distinguish "init *is* a closure value"
             * from "init names a *function* that returns a closure".  When the
             * init is an EX_VAR naming a plain function (TY_FN, no
             * closure_fn_binding of its own) that merely *returns* a closure,
             * the alias is still that function -- propagate
             * returns_closure_fn_binding, NOT closure_fn_binding.  Marking the
             * alias as a closure value would make elab_call_fn swap in the
             * inner thunk's type and subtract a hidden env param from its
             * arity, so (f 1) on a curried (fn [int] (fn [int] int)) would
             * wrongly resolve to the inner result kind instead of (fn [int]
             * int). */
            if (init->kind == EX_VAR && init->as.var.binding &&
                init->as.var.binding->type.kind == TY_FN &&
                !init->as.var.binding->closure_fn_binding &&
                init->as.var.binding->returns_closure_fn_binding) {
                b->returns_closure_fn_binding =
                    init->as.var.binding->returns_closure_fn_binding;
            } else {
                Binding *closure_b = expr_closure_fn_binding(init);
                if (closure_b) {
                    /* let-bound SF (let-bound-sf-loses-outer-arg-type): the same
                     * "is a closure value" vs "returns a closure" distinction the
                     * EX_VAR branch above makes also applies to a *call* init.
                     * When init is a call to a function whose return *value* is a
                     * thin (non-boxed) function pointer that itself returns a
                     * closure -- e.g. (make-sf) returning the outer
                     * (fn [sig] (fn [t] ...)) lambda -- closure_b describes what
                     * that thin fn *returns* when called, not what the call result
                     * *is*.  Recording it as closure_fn_binding would make
                     * elab_call_fn swap in the inner env+arg thunk type for `b`,
                     * dropping b's real first parameter (the outer `sig`) and
                     * reading the inner arg in its place.  Route it to
                     * returns_closure_fn_binding instead so a chained call
                     * (sf input) still sees its result as a closure while `b`
                     * keeps its declared outer signature.  A call whose callee
                     * returns a genuine fat closure *box* (e.g. (adder 10), where
                     * adder's body is a capturing lambda) still flows to
                     * closure_fn_binding -- the call result IS the closure value. */
                    if (init->kind == EX_CALL && init->as.call_.fn_binding &&
                        !init->as.call_.fn_binding->returns_boxed_closure) {
                        b->returns_closure_fn_binding = closure_b;
                    } else {
                        b->closure_fn_binding = closure_b;
                        /* generic-closure-return-type-app (Defect B): remember
                         * WHICH call produced the closure value, so the invoke
                         * can target the producing spec's inner-body clone
                         * instead of the shared generic base thunk -- same
                         * stash as the call-head temp in elab_call.c. */
                        if (init->kind == EX_CALL)
                            b->closure_head_init = init;
                    }
                }
            }
        }
        /* Phase HRT4: propagate poly fn metadata through let-bindings.
         * (let [g f] ...) where f is is_poly_fn → g inherits is_poly_fn.
         * (let [g id] ...) where id is a global TY_FN → g.source_binding = id. */
        if (init && init->kind == EX_VAR) {
            Binding *init_b = init->as.var.binding;
            /* closure-representation-unification (Phase 0): propagate the ^fat
             * marker through a let alias so (let [gv g] ... (gv x)) on a
             * fn-typed ^fat parameter still fat-dispatches instead of taking the
             * thin ER2 path (which casts the fat box to a bare fn pointer and
             * crashes).  A bare ^fat alias is :ptr<void> and fat-dispatches
             * regardless, but carrying is_fat keeps the two forms consistent. */
            /* fn-value-carrier-fat-seam-residuals (nominal-param alias): the
             * marker also has to carry for a param that is fat by NORMALIZATION
             * rather than by annotation.  `f : (fn [int] Pair2)` holds a fat
             * handle at runtime -- the callee's invoke dispatches fat, keyed on
             * `fn_param_type_is_fat_normalized` -- but `is_fat` is false on it,
             * because that flag records the `^fat` ANNOTATION.
             *
             * Without this, `(let [g f] ... (h g ...))` re-shims the alias into
             * a SECOND `__tur_fatshim` box, and the consumer reads the inner
             * handle's first word as code:
             *
             *   (defn use3 [f : (fn [int] Pair2)] : int
             *     (let [g f] (.a (apply2 g 5))))        ;; SEGV; without the
             *                                           ;; alias it is fine
             *
             * The call-site pass-through already knows about both kinds -- its
             * comment even says "a ^fat parameter (or a let-alias of one)" --
             * but its normalized arm requires `is_param`, which an alias is not.
             * Carrying the fact on the alias fixes it for every guard keyed on
             * `is_fat` rather than for that one call site.
             *
             * The question "is this parameter a fat handle?" is asked of the
             * representation chokepoint, not re-derived here: repr_of_binding
             * is the function that decides the PARAM's own representation, and
             * an alias has to agree with its param by construction -- that
             * disagreement is exactly the re-shim above.  (The repr-decision
             * ratchet pins the re-derivation sites for the same reason.) */
            if (init_b->is_fat ||
                (init_b->is_param &&
                 repr_of_binding(init_b, REPR_POS_PARAM) == REPR_FAT_HANDLE)) {
                b->is_fat = true;
            }
            if (init_b->is_poly_fn) {
                b->is_poly_fn = true;
                b->poly_type  = init_b->poly_type;
            } else if (init_b->type.kind == TY_FN) {
                /* Follow any existing source chain to the root global fn. */
                Binding *root = init_b->source_binding ? init_b->source_binding : init_b;
                /* pr-386 regression fix (docs/archive/history/pr-386-source-binding-
                 * alias-breaks-closure-and-with-resource.md): never chain to a
                 * lifted-lambda __fn_N helper.  source_binding means "the user
                 * typed a global function name as the init"; a captureless
                 * closure-returning lambda is callable only through the
                 * closure-dispatch protocol on this let binding, and chaining
                 * to __fn_N makes (f x) emit a direct call whose result is the
                 * int64 carrier rather than a function pointer. */
                if (root->is_global && !root->is_lifted_lambda) b->source_binding = root;
                /* any-fn-widen-through-local-binding-leaks: the widen-only
                 * alias, which DOES chain to a lifted lambda (see the field's
                 * note in expr.h).  Immutable bindings only. */
                if (!b->is_mut) {
                    Binding *wroot = init_b->widen_fn_alias ? init_b->widen_fn_alias
                                                            : init_b;
                    if (wroot->is_global && !wroot->closure_fn_binding &&
                        !wroot->is_param && !wroot->is_poly_fn && !wroot->is_fat)
                        b->widen_fn_alias = wroot;
                }
            }
        }
        
        /* A `ref/from-rc` result used to be marked is_nonowning_ref here, on the
         * reasoning that it "shares the rc payload and cannot auto-drop".  The
         * runtime contract is the opposite: rc.h documents tur_ref_from_rc as
         * DESTROYING the control block, and its body nulls `cb->value` before
         * `free(cb)`, so the payload survives with the returned ref as its only
         * owner.  Nothing shared it and nothing freed it --
         * docs/reported/rc-ref-conversion-and-weak-upgrade-leak.md.
         *
         * It is therefore an ordinary owning ref, exactly like a fresh
         * `(ref ...)`: deref is non-consuming and the scope-exit auto-drop
         * below discharges the obligation.  The fixture guarding this path says
         * the same thing in its own header -- "ownership transfers to the
         * caller ... the caller owns the resulting ref" -- and what it asserts
         * is that the CONSUMED rc gets no auto-drop, which is a different
         * binding and still holds. */

        /* async-await-payload-is-int64-only: a binding initialised by an
         * `(async ..)` remembers the thunk's payload type for its `await`.
         * generator-yield-payload-is-int64-only: one initialised by a
         * `(gen-next g)` remembers the element kind for its `gen-unwrap`. */
        {
            const Expr *ai = init;
            while (ai && ai->kind == EX_ASCRIBE) ai = ai->as.ascribe_.inner;
            if (ai && ai->kind == EX_ASYNC) b->async_payload = &ai->as.async_.payload;
            if (ai && ai->kind == EX_GEN_NEXT && ai->as.gen_next_.gen_expr &&
                ai->as.gen_next_.gen_expr->type.kind == TY_GENERATOR) {
                b->gen_elem_kind = ai->as.gen_next_.gen_expr->type.as.generator_.element_kind;
                b->gen_elem_tyvar = ai->as.gen_next_.gen_expr->type.as.generator_.element_tyvar;
                b->gen_elem_set  = true;
            }
        }
        /* byvalue-recursive-shared-copies-leak: a local is an owner, so a
         * shared view it is initialised from gets its own counts. */
        init = elab_own_byval_copy(e, init, b);
        binds[n_binds].binding = b;
        binds[n_binds].init = init;
        binding_moved_during_init[n_binds] = false; /* new binding, not yet moved during init */
        n_binds++;

        /* compiled-closure-copies-a-captured-mut: a `^mut` a lambda in the
         * rest of this let captures moves into a shared heap cell. */
        if (is_mut && !is_persistent && !is_linear_ann && !is_unique_ann &&
            !is_affine_ann && !is_relevant_ann && !is_fat_ann &&
            !b->is_linear && !b->is_unique && !b->is_global &&
            cell_elem_type_ok(b->type)) {
            bool captured = false;
            for (uint32_t bi = i; !captured && bi < bindings_form->as.list.len; bi++)
                captured = cell_fn_mentions(e, bindings_form->as.list.items[bi],
                                            name, false, 0);
            for (uint32_t bi = 2; !captured && bi < call->as.list.len; bi++)
                captured = cell_fn_mentions(e, call->as.list.items[bi], name,
                                            false, 0);
            bool ok = true;
            if (captured)
                (void)elab_let_mut_to_cell(e, &inner, &binds,
                                           &binding_moved_during_init, &n_binds,
                                           &cap, b, name, name_span, &ok);
            if (!ok) { rc = -1; break; }
        }
    }

    /* Phase 5: Check if any binding is a ref and needs auto-defer drop.
     * Theme 1 (ref<T> deref/auto-drop): linear refs participate in auto-drop
     * too -- the injected (defer (drop! r)) is the single ownership discharge
     * at scope exit.  The precise per-binding filtering (moved / explicitly
     * consumed / consumed-by-use) happens in the count + injection loops below
     * where the elaborated body is available; here we only decide whether to
     * wrap the body in a `do` so defers can be appended, so over-detecting a
     * ref that ends up not needing a drop is harmless (n_refs == 0 -> no-op). */
    /* `(upgrade w)` returns its option<rc<T>> as a heap box minted in EMIT
     * (emit_expr.c EX_WEAK_UPGRADE) and typed `ptr<void>`, so no binding owned
     * it and nothing freed it -- bug 2 of
     * docs/reported/rc-ref-conversion-and-weak-upgrade-leak.md.
     *
     * It cannot be typed `ref<...>` instead (that is the obvious fix and it
     * fails): every consumer takes the result as a `ptr<void>` parameter, and
     * `ref<ptr<void>>` does not coerce to one -- all five in-tree callers get
     * TUR-E0001.  So the binding keeps its `ptr<void>` type and the ownership
     * is keyed on the INIT instead.  The disposal is the same `drop!` builtin
     * the ref path uses, which is BS_PREFIX_UNARY_FREE -- it emits a plain
     * `free`, which is exactly right for this malloc.  (The "drop! requires
     * ref<T>" check lives in elab_drop, the surface form; building the builtin
     * directly here bypasses it, which is what makes this work.) */
    #define binding_owns_upgrade_box(bp) \
        ((bp)->init && (bp)->init->kind == EX_WEAK_UPGRADE)

    bool has_ref_bindings = false;
    for (uint32_t k = 0; k < n_binds; k++) {
        if (binds[k].binding->type.kind == TY_REF ||
            binding_owns_upgrade_box(&binds[k])) {
            has_ref_bindings = true;
            break;
        }
        /* closure-drop-glue (automatic R3a): a move-only Drop-instance opaque
         * (Handler) also needs the do-wrap so its scope-exit drop can be
         * appended.  Over-detecting is harmless (the injection loop re-checks). */
        if (binding_closure_drop_inst(e, binds[k].binding, binds[k].init)) {
            has_ref_bindings = true;
            break;
        }
    }

    /* The binding_moved_during_init array (built during the binding loop) records which
     * bindings were moved during init elaboration of subsequent bindings in this let form.
     * Combined with is_moved (which also captures body-phase moves), this gives complete
     * move-state tracking across all elaboration phases. */

    Expr *body = NULL;
    if (rc == 0) {
        uint32_t body_count = call->as.list.len - 2;
        if (body_count == 0) {
            body = e_nil(e, call->span);
        } else {
            /* Internal defines in let body: splice into nested let forms. */
            Form *spliced = splice_internal_defines(e,
                                call->as.list.items + 2, body_count, call->span);
            if (spliced) {
                body = elab_form(e, spliced);
                if (!body) rc = -1;
            } else if (body_count == 1) {
                body = elab_form(e, call->as.list.items[2]);
                if (!body) rc = -1;
            } else {
                Expr **items = (Expr **)arena_alloc(e->arena, body_count * sizeof(Expr *));
                for (uint32_t k = 0; k < body_count; k++) {
                    items[k] = elab_form(e, call->as.list.items[2 + k]);
                    if (!items[k]) { rc = -1; break; }
                }
                if (rc == 0) {
                    body = expr_new(e->arena, EX_DO, items[body_count - 1]->type, call->span);
                    body->as.do_.items = items;
                    body->as.do_.n = body_count;
                }
            }
        }

        /* EXG4-1/EXG4-4: ownership transfer through let-tail position.
         *
         * If the body's tail expression is a direct EX_VAR of one of this
         * let's own bindings, the let's result IS that binding's value.
         * Whoever consumes the let's result (an outer let init, a function
         * return, a call arg, etc.) takes ownership.  Mark the binding as
         * moved so the auto-drop pass below skips it -- the consumer is
         * responsible for the eventual drop.
         *
         * Without this, the inner let auto-drops the rc-block before its
         * value escapes, producing use-after-free at the consumer site
         * (e.g. `(let [outer (let [e (pack ...)] e)] ...)`).  Only fires
         * for move-typed bindings; copy types are unaffected. */
        if (rc == 0 && body && n_binds > 0) {
            Expr *cur = body;
            while (cur) {
                if (cur->kind == EX_VAR) {
                    Binding *bv = cur->as.var.binding;
                    for (uint32_t k = 0; k < n_binds; k++) {
                        if (binds[k].binding == bv &&
                            type_is_move(bv->type) &&
                            !bv->is_moved) {
                            binding_mark_moved(bv, cur->span);
                            break;
                        }
                    }
                    break;
                } else if (cur->kind == EX_DO) {
                    if (cur->as.do_.n == 0) break;
                    cur = cur->as.do_.items[cur->as.do_.n - 1];
                } else if (cur->kind == EX_LET) {
                    cur = cur->as.let_.body;
                } else {
                    break;
                }
            }
        }

        /* Phase 5: If we have ref bindings and the body is a single expression
         * (not a do), wrap it in a do so we can add defers */
        if (has_ref_bindings && body && body->kind != EX_DO) {
            Expr **items = (Expr **)arena_alloc(e->arena, 1 * sizeof(Expr *));
            items[0] = body;
            body = expr_new(e->arena, EX_DO, body->type, call->span);
            body->as.do_.items = items;
            body->as.do_.n = 1;
        }
        
        /* Phase 5: Inject defers for ref bindings into the do body */
        if (has_ref_bindings && body && body->kind == EX_DO) {
            /* We need to add defer expressions to the do body */
            /* First, collect all ref binding names that need drops (excluding moved ones) */
            uint32_t n_refs = 0;
            for (uint32_t k = 0; k < n_binds; k++) {
                /* Skip refs that were moved during init or body elaboration - avoid use-after-move defer */
                /* Theme 1: a linear ref consumed by a non-drop path (e.g. `(return r)`
                 * or a tail move) has is_linear_consumed set and must NOT also auto-drop
                 * -- that would double-free / free an escaping ref.  A deref'd ref has
                 * been un-marked (see elab_deref) so it still auto-drops here.
                 *
                 * `ref/from-rc` results USED to be excluded here, on the reasoning
                 * that "they don't own the data".  That is backwards: rc.h documents
                 * tur_ref_from_rc as destroying the control block, and its body nulls
                 * `cb->value` before `free(cb)` -- so the payload survives and the
                 * returned ref is its only owner.  Excluding it meant nothing ever
                 * freed the payload (rc-ref-conversion-and-weak-upgrade-leak).  The
                 * fixture guarding this path agrees in its own header: "ownership
                 * transfers to the caller ... the caller owns the resulting ref" --
                 * what it asserts is that the consumed RC gets no auto-drop, which is
                 * a different binding and still holds. */
                if ((binds[k].binding->type.kind == TY_REF ||
                     binding_owns_upgrade_box(&binds[k])) &&
                    !binding_moved_during_init[k] &&
                    !binds[k].binding->is_moved &&
                    !binds[k].binding->is_linear_consumed &&
                    !is_binding_consumed(body, binds[k].binding)) {
                    n_refs++;
                }
                /* closure-drop-glue (automatic R3a): count a move-only Drop opaque
                 * (Handler) binding not moved/consumed by scope exit -- same move
                 * filters as the ref path, so a handed-off Handler is not dropped. */
                else if (binding_closure_drop_inst(e, binds[k].binding, binds[k].init) &&
                         !binding_moved_during_init[k] &&
                         !binds[k].binding->is_moved &&
                         !is_binding_consumed(body, binds[k].binding)) {
                    n_refs++;
                }
            }
            
            if (n_refs > 0) {
                /* Create new items array with space for defers */
                uint32_t new_n = body->as.do_.n + n_refs;
                Expr **new_items = (Expr **)arena_alloc(e->arena, new_n * sizeof(Expr *));
                
                /* Copy existing items */
                memcpy(new_items, body->as.do_.items, body->as.do_.n * sizeof(Expr *));
                
                /* Add defer expressions for each ref binding at the end */
                /* Note: defers execute in LIFO order, so we add them in order and they'll
                 * fire in reverse order. But since we're adding them at the end of the
                 * items array, they'll be after the actual body expressions, which is
                 * what we want for scope-exit behavior. */
                uint32_t defer_idx = body->as.do_.n;
                for (uint32_t k = 0; k < n_binds; k++) {
                    /* Skip refs moved during init or body elaboration - avoid use-after-move defer */
                    /* Theme 1: mirror the count-loop guard (see above), including the
                     * dropped ref/from-rc exclusion. */
                    if ((binds[k].binding->type.kind == TY_REF ||
                         binding_owns_upgrade_box(&binds[k])) &&
                        !binding_moved_during_init[k] &&
                        !binds[k].binding->is_moved &&
                        !binds[k].binding->is_linear_consumed &&
                        !is_binding_consumed(body, binds[k].binding)) {
                        /* Theme 1: the injected auto-drop IS this linear ref's single
                         * ownership discharge -- mark it consumed so the LT1 scope-exit
                         * check (below) treats the must-consume obligation as satisfied
                         * rather than reporting TUR-E0100. */
                        binds[k].binding->is_linear_consumed = true;
                        /* Create (defer (drop! binding_name)) expression */
                        /* Create a variable reference to the binding */
                        Expr *var_expr = expr_new(e->arena, EX_VAR, binds[k].binding->type, call->span);
                        var_expr->as.var.binding = binds[k].binding;
                        
                        /* Look up the drop! builtin spec and create a BUILTIN expression */
                        const BuiltinSpec *spec = builtin_lookup(e->sym_drop, binds[k].binding->type, 1);
                        if (!spec) {
                            diag_emit(DIAG_ERROR, call->span,
                                      "internal error: drop! builtin not found for ref<T>");
                            rc = -1;
                            break;
                        }
                        
                        /* Create the drop! builtin call */
                        Expr *drop_call = expr_new(e->arena, EX_BUILTIN, TYPE_NIL, call->span);
                        drop_call->as.builtin.spec = spec;
                        drop_call->as.builtin.n = 1;
                        drop_call->as.builtin.args = (Expr **)arena_alloc(e->arena, sizeof(Expr *));
                        drop_call->as.builtin.args[0] = var_expr;
                        
                        /* Create the defer expression */
                        Expr *defer_expr = expr_new(e->arena, EX_DEFER, TYPE_NIL, call->span);
                        defer_expr->as.defer_.body = drop_call;
                        /* Capture analysis for defer body */
                        /* Collect free variables in the defer body (the drop! call references the binding) */
                        uint32_t n_free = 0;
                        Binding **free_vars = collect_free_vars(drop_call, NULL, 0, NULL, 0, &n_free);
                        
                        Binding **captures = NULL;
                        uint8_t n_captures = 0;
                        if (n_free > 0) {
                            captures = (Binding **)arena_alloc(e->arena, n_free * sizeof(Binding *));
                            memcpy(captures, free_vars, n_free * sizeof(Binding *));
                            n_captures = (uint8_t)n_free;
                        }
                        free(free_vars);
                        
                        defer_expr->as.defer_.captures = captures;
                        defer_expr->as.defer_.n_captures = n_captures;
                        defer_expr->as.defer_.is_drop_glue = true;

                        new_items[defer_idx++] = defer_expr;
                    }
                    /* closure-drop-glue (automatic R3a): a move-only Drop opaque
                     * (Handler) -- inject `(defer (<Drop.drop> b))`, dispatching
                     * through the Drop instance's method (TUR_CLOSURE_DROP) rather
                     * than drop!->free (a bare free of a headered onion aborts). */
                    else {
                        struct TypeClassInstance *di =
                            binding_closure_drop_inst(e, binds[k].binding, binds[k].init);
                        if (di && !binding_moved_during_init[k] &&
                            !binds[k].binding->is_moved &&
                            !is_binding_consumed(body, binds[k].binding)) {
                            Binding *dm = di->method_impls[0]->binding;
                            Expr *var_expr = expr_new(e->arena, EX_VAR,
                                                      binds[k].binding->type, call->span);
                            var_expr->as.var.binding = binds[k].binding;

                            Expr *drop_call = expr_new(e->arena, EX_CALL, TYPE_NIL, call->span);
                            drop_call->as.call_.fn_binding = dm;
                            drop_call->as.call_.n_args = 1;
                            drop_call->as.call_.args =
                                (Expr **)arena_alloc(e->arena, sizeof(Expr *));
                            drop_call->as.call_.args[0] = var_expr;

                            Expr *defer_expr = expr_new(e->arena, EX_DEFER, TYPE_NIL, call->span);
                            defer_expr->as.defer_.body = drop_call;
                            uint32_t n_free = 0;
                            Binding **free_vars =
                                collect_free_vars(drop_call, NULL, 0, NULL, 0, &n_free);
                            Binding **captures = NULL;
                            uint8_t n_captures = 0;
                            if (n_free > 0) {
                                captures = (Binding **)arena_alloc(
                                    e->arena, n_free * sizeof(Binding *));
                                memcpy(captures, free_vars, n_free * sizeof(Binding *));
                                n_captures = (uint8_t)n_free;
                            }
                            free(free_vars);
                            defer_expr->as.defer_.captures = captures;
                            defer_expr->as.defer_.n_captures = n_captures;
                            defer_expr->as.defer_.is_drop_glue = true;

                            new_items[defer_idx++] = defer_expr;
                        }
                    }
                }

                /* Update the body with new items */
                body->as.do_.items = new_items;
                body->as.do_.n = new_n;
            }
        }
    }

    /* Phase 5b / EXG1-5: RC auto-drop injection with consumption detection.
     * Inject (defer (rc/drop x)) for let-bound rc/of values that are:
     * 1. Not consumed by ref/from-rc (which would transfer ownership and cause double-free)
     * 2. Not explicitly dropped via (rc/drop x)
     * 3. Not moved to another binding
     *
     * EXG1-5: constrained existentials (`(exists [a] [(C a) ...] T)`) are
     * allocated through rc_cb_alloc by emit_expr.c, so their bindings need
     * the same scope-exit decrement.  We piggy-back on the same EX_RC_DROP
     * mechanism: at codegen time the binding's value is a void* that
     * implicitly converts to RcControlBlock* for rc_strong_decrement.
     * Unconstrained existentials (no witnesses) are unchanged — they do
     * not allocate, so they need no drop. */
    bool has_rc_bindings = false;
    for (uint32_t k = 0; k < n_binds; k++) {
        Type bt = binds[k].binding->type;
        /* EXG6: linear existentials use the plain malloc emit path and
         * are freed at the open site, so they do NOT participate in the
         * rc-based scope-exit auto-drop. */
        bool is_rc_managed = bt.kind == TY_RC ||
            (bt.kind == TY_EXISTS && bt.as.forall_.n_constraints > 0
             && !bt.as.forall_.is_linear);
        if (is_rc_managed) {
            has_rc_bindings = true;
            break;
        }
    }

    /* If we have rc bindings, wrap body in do if needed and inject defers */
    if (has_rc_bindings && body && body->kind != EX_DO) {
        Expr **items = (Expr **)arena_alloc(e->arena, 1 * sizeof(Expr *));
        items[0] = body;
        body = expr_new(e->arena, EX_DO, body->type, call->span);
        body->as.do_.items = items;
        body->as.do_.n = 1;
    }

    if (has_rc_bindings && body && body->kind == EX_DO) {
        /* rc-field-read-into-var-double-free: BEFORE injecting the scope-exit
         * auto-drops below (which mutate `body` to add `(defer (rc/drop x))` and
         * would then read as consumption), clone-on-read every rc binding whose
         * init borrows an `rc` field (`saved (.r o)`).  Reading an rc field is a
         * shared-ownership borrow: the source struct still releases that field (a
         * by-value local via its field auto-drop, an rc/of struct via its
         * control-block drop glue, a borrowed parameter via its caller), so the
         * raw word copy aliases the control block WITHOUT a strong-count bump --
         * yet the new binding is disposed exactly once (its scope-exit auto-drop,
         * an explicit `(rc/drop saved)`, or a move into a consumer), double-freeing
         * the block.  Wrapping the init in EX_RC_CLONE makes the read increment the
         * count, so the binding is a genuine second owner whose +1 balances that
         * one disposal.  This is orthogonal to the auto-drop's moved/consumed
         * filter: a consumed or moved binding is still disposed once and still
         * needs the clone.  The ONE exception is when the source field is itself
         * explicitly moved out (`(rc/drop (.f o))` / `(drop! (.f o))`), which
         * suppresses the source-side release -- cloning then would over-count, so
         * skip it. */
        for (uint32_t k = 0; k < n_binds; k++) {
            Type bt = binds[k].binding->type;
            bool is_rc_managed = bt.kind == TY_RC ||
                (bt.kind == TY_EXISTS && bt.as.forall_.n_constraints > 0
                 && !bt.as.forall_.is_linear);
            if (!is_rc_managed) continue;
            Expr *fld = elab_rc_field_read_init(binds[k].init);
            if (!fld) continue;
            /* Skip when the source field is explicitly moved out: the source no
             * longer releases it, so the raw copy is already the sole owner. */
            const Expr *recv = fld->as.get_field_.struct_expr;
            if (recv && recv->kind == EX_VAR && recv->as.var.binding &&
                is_field_consumed(body, recv->as.var.binding,
                                  fld->as.get_field_.field_idx))
                continue;
            Expr *clone = expr_new(e->arena, EX_RC_CLONE, binds[k].init->type,
                                   binds[k].init->span);
            clone->as.rc_clone_.expr = binds[k].init;
            clone->as.rc_clone_.elide = false;
            binds[k].init = clone;
        }

        /* Count rc bindings that need auto-drop (excluding consumed/moved ones) */
        uint32_t n_rc_drops = 0;
        for (uint32_t k = 0; k < n_binds; k++) {
            Type bt = binds[k].binding->type;
            bool is_rc_managed = bt.kind == TY_RC ||
                (bt.kind == TY_EXISTS && bt.as.forall_.n_constraints > 0
                 && !bt.as.forall_.is_linear);
            if (is_rc_managed &&
                !binding_moved_during_init[k] &&
                !binds[k].binding->is_moved &&
                !is_binding_consumed(body, binds[k].binding)) {
                n_rc_drops++;
                /* set-bang-rc-release: this binding owns a continuous +1 from
                 * its init to the auto-drop injected below, so every `(set! b v)`
                 * in the body must release what it overwrites -- otherwise only
                 * the FINAL value is ever released and each assignment leaks a
                 * block.  Gated on exactly the auto-drop predicate above so the
                 * two can never disagree: a hand-managed binding gets neither. */
                elab_set_rc_release(e->arena, body, binds[k].binding);
            }
        }

        if (n_rc_drops > 0) {
            /* Create new items array with space for rc drop defers */
            uint32_t new_n = body->as.do_.n + n_rc_drops;
            Expr **new_items = (Expr **)arena_alloc(e->arena, new_n * sizeof(Expr *));

            /* Copy existing items */
            memcpy(new_items, body->as.do_.items, body->as.do_.n * sizeof(Expr *));

            /* Add defer expressions for each unconsumed rc binding */
            uint32_t defer_idx = body->as.do_.n;
            for (uint32_t k = 0; k < n_binds; k++) {
                /* Skip RC bindings that are moved or consumed */
                Type bt = binds[k].binding->type;
                bool is_rc_managed = bt.kind == TY_RC ||
                    (bt.kind == TY_EXISTS && bt.as.forall_.n_constraints > 0
                     && !bt.as.forall_.is_linear);
                if (is_rc_managed &&
                    !binding_moved_during_init[k] &&
                    !binds[k].binding->is_moved &&
                    !is_binding_consumed(body, binds[k].binding)) {
                    
                    /* Create a variable reference to the rc binding */
                    Expr *var_expr = expr_new(e->arena, EX_VAR, binds[k].binding->type, call->span);
                    var_expr->as.var.binding = binds[k].binding;
                    
                    /* Create the rc/drop expression */
                    Expr *rc_drop_expr = expr_new(e->arena, EX_RC_DROP, TYPE_NIL, call->span);
                    rc_drop_expr->as.rc_drop_.expr = var_expr;
                    
                    /* Create the defer expression wrapping the rc/drop */
                    Expr *defer_expr = expr_new(e->arena, EX_DEFER, TYPE_NIL, call->span);
                    defer_expr->as.defer_.body = rc_drop_expr;
                    
                    /* Capture analysis */
                    uint32_t n_free = 0;
                    Binding **free_vars = collect_free_vars(rc_drop_expr, NULL, 0, NULL, 0, &n_free);
                    
                    Binding **captures = NULL;
                    uint8_t n_captures = 0;
                    if (n_free > 0) {
                        captures = (Binding **)arena_alloc(e->arena, n_free * sizeof(Binding *));
                        memcpy(captures, free_vars, n_free * sizeof(Binding *));
                        n_captures = (uint8_t)n_free;
                    }
                    free(free_vars);
                    
                    defer_expr->as.defer_.captures = captures;
                    defer_expr->as.defer_.n_captures = n_captures;
                    defer_expr->as.defer_.is_drop_glue = true;
                    
                    new_items[defer_idx++] = defer_expr;
                }
            }
            
            /* Update the body with new items */
            body->as.do_.items = new_items;
            body->as.do_.n = new_n;
        }
    }

    /* byvalue-struct-field-leak: a *bare* by-value ADT/record local carrying
     * owning rc/ref fields is never released at scope exit -- the drop glue that
     * frees those fields (`drop_glue_tur_adt_<T>`) is only wired up on the
     * `rc/of`-wrapped path, through the control block's drop_fn.  Mirror the
     * rc-drop injection above and emit one scope-exit defer per owning field:
     * `(defer (rc/drop (.f o)))` for an rc field, `(defer (drop! (.f o)))` for a
     * ref field.  Guarded by the same moved / consumed filters so a local that
     * escapes (returned, moved into a call, or explicitly consumed) is not
     * double-dropped.  This path is disjoint from the rc-binding path above: a
     * value wrapped in `rc/of` binds at type TY_RC and is handled there.
     *
     * `weak` fields are intentionally left to leak their control-block weak
     * count: a weak reference is non-owning (it holds no payload), there is no
     * scope-exit `weak`-decrement primitive to reuse here, and the reported
     * defect is specifically about *owning* fields.  The rc/of drop-glue path
     * still decrements weak fields via rc_weak_decrement. */
    /* Reject the unsound consume-in-handler-case shape BEFORE injecting any
     * per-field auto-drop.  A handler case that drops an owning field of one of
     * these captured by-value locals (`(rc/drop (.f o))` / `(drop! (.f o))`
     * inside a `handle` here) would double-release the field against the
     * scope-exit auto-drop below (and a multi-shot resume would drop it N
     * times).  is_field_consumed handles the straight-line case (suppress the
     * auto-drop); the handler-case case cannot be balanced locally, so it is a
     * hard error (TUR-E0107) rather than a silent double-drop / eviction to a
     * miscompiling fallback. */
    if (rc == 0) {
        for (uint32_t k = 0; k < n_binds; k++) {
            const AdtDef *ad = elab_byval_drop_adt(binds[k].binding->type);
            if (!ad) continue;
            if (binding_moved_during_init[k] || binds[k].binding->is_moved ||
                is_binding_consumed(body, binds[k].binding))
                continue;
            const CtorDef *ctor = ad->ctors[0];
            for (uint32_t fi = 0; fi < ctor->n_fields; fi++) {
                TypeKind fk = ctor->fields[fi].kind;
                if (fk != TY_RC && fk != TY_REF && fk != TY_LREF)
                    continue;
                if (is_field_consumed_in_handler(body, binds[k].binding, fi)) {
                    const char *fname = ctor->fields[fi].name
                                            ? ctor->fields[fi].name : "<field>";
                    const char *oname = binds[k].binding->name
                                            ? binds[k].binding->name->name : "<local>";
                    diag_emit_with_code(
                        DIAG_ERROR, binds[k].binding->span,
                        TUR_E0107_CAPTURED_FIELD_CONSUMED_IN_HANDLER,
                        "handler case consumes owning field '.%s' of captured "
                        "by-value value '%s'; the field would be dropped both by "
                        "the handler case and by '%s's scope-exit auto-drop (and a "
                        "multi-shot resume would drop it more than once). Borrow "
                        "the field instead (read it, do not drop it), or move "
                        "ownership out of '%s' before the handle.",
                        fname, oname, oname, oname);
                    rc = -1;
                }
            }
        }
    }

    bool has_byval_drop_bindings = false;
    for (uint32_t k = 0; k < n_binds; k++) {
        if (elab_byval_drop_adt(binds[k].binding->type)) {
            has_byval_drop_bindings = true;
            break;
        }
    }

    if (has_byval_drop_bindings && body && body->kind != EX_DO) {
        Expr **items = (Expr **)arena_alloc(e->arena, 1 * sizeof(Expr *));
        items[0] = body;
        body = expr_new(e->arena, EX_DO, body->type, call->span);
        body->as.do_.items = items;
        body->as.do_.n = 1;
    }

    /* local-struct-drop (fn-field): flag every eligible by-value local that owns
     * a boxed fn-field so the DIRECT emitter frees that box at scope exit.  Same
     * moved/consumed/escape guards as the rc/ref auto-drop injection below, so a
     * struct that escapes (returned / moved / consumed) is never flagged (no
     * double-free).  Deliberately NOT injected as a defer -- see
     * Binding.drops_fn_fields -- so a colored fn stays CPS-admissible. */
    for (uint32_t k = 0; k < n_binds; k++) {
        const AdtDef *ad = elab_byval_drop_adt(binds[k].binding->type);
        if (!ad) continue;
        if (binding_moved_during_init[k] || binds[k].binding->is_moved ||
            is_binding_consumed(body, binds[k].binding))
            continue;
        const CtorDef *ctor = ad->ctors[0];
        for (uint32_t fi = 0; fi < ctor->n_fields; fi++) {
            if (!elab_field_is_boxed_fnfield(ctor, fi)) continue;
            if (is_field_consumed(body, binds[k].binding, fi)) continue;
            binds[k].binding->drops_fn_fields = true;
            break;
        }
    }

    /* byvalue-recursive-adt-boxes-are-never-freed: the same flagging for a
     * by-value RECURSIVE local, whose spine of boxes is freed at scope exit.
     *
     * A separate loop rather than a branch in the one above, because it cannot
     * share `elab_byval_drop_adt`: that helper refuses `n_ctors != 1` (its
     * callers read `ctors[0]` directly) and a recursive ADT is a sum by
     * construction -- it needs a base case -- so every type this exists for
     * would be refused.  The guards are the same three, and they are what makes
     * the free sound: a local that was moved into a call, moved during its own
     * initialisation, or explicitly consumed has handed ownership on, and the
     * new owner's scope frees the spine instead. */
    for (uint32_t k = 0; k < n_binds; k++) {
        const AdtDef *ad = elab_byval_localowned_adt(binds[k].binding->type);
        if (!ad) continue;
        /* Residue 1: a local whose only "move" was a LEND -- every use that
         * poisoned it passed it to a callee proven non-retaining -- still owns
         * its spine here, and nothing else will free it.  A real move anywhere
         * (moved_owning is sticky across branches) keeps the old answer. */
        Binding *lb = binds[k].binding;
        bool moved_for_real = lb->is_moved &&
                              !(lb->lent_to_nonretaining && !lb->moved_owning);
        if (binding_moved_during_init[k] || moved_for_real ||
            is_binding_consumed(body, lb))
            continue;
        /* fn-cell-set-with-capturing-closure-segfaults, found while closing
         * it: the guards above see the LOCAL, not its aliases.  A match
         * binder of `xs` carries a pointer into the spine, and if the body
         * stores it -- `(set! g-fn (fn [] (llen t)))`, a closure capturing
         * `t` into a global cell -- the drop at scope exit frees what the
         * closure will read (measured: the call printed 1 for a 2-link
         * tail).  Ask the strict alias-aware walk the parameter inference
         * already uses: every alias must stay confined to the body, and the
         * scope's value may carry one out only if it is a non-pointer scalar.
         * Refusing costs a leak; the old answer cost a use-after-free. */
        {
            TypeKind bk = body ? body->type.kind : TY_UNKNOWN;
            bool scalar_result;
            switch (bk) {
                case TY_NIL: case TY_BOOL: case TY_INT: case TY_FLOAT: case TY_SYM:
                case TY_INT8: case TY_INT16: case TY_INT32: case TY_INT64:
                case TY_UINT8: case TY_UINT16: case TY_UINT32: case TY_UINT64:
                case TY_FLOAT32: case TY_FLOAT64:
                    scalar_result = true; break;
                default:
                    scalar_result = false; break;
            }
            if (!localowned_binding_is_confined(body, lb, scalar_result))
                continue;
        }
        binds[k].binding->drops_local_owned = true;
    }

    if (has_byval_drop_bindings && body && body->kind == EX_DO) {
        /* Count owning fields across every eligible by-value local. */
        uint32_t n_field_drops = 0;
        for (uint32_t k = 0; k < n_binds; k++) {
            const AdtDef *ad = elab_byval_drop_adt(binds[k].binding->type);
            if (!ad) continue;
            if (binding_moved_during_init[k] || binds[k].binding->is_moved ||
                is_binding_consumed(body, binds[k].binding))
                continue;
            const CtorDef *ctor = ad->ctors[0];
            for (uint32_t fi = 0; fi < ctor->n_fields; fi++) {
                TypeKind fk = ctor->fields[fi].kind;
                if (fk != TY_RC && fk != TY_REF && fk != TY_LREF)
                    continue;
                /* byvalue-recursive-shared-copies-leak: a `ref` field of a
                 * shared view belongs to its real owner (it cannot be cloned
                 * the way an rc field is). */
                if (fk != TY_RC && binds[k].binding->shared_view)
                    continue;
                /* Skip a field the body already drops explicitly -- the
                 * scope-exit auto-drop would double-free it. */
                if (is_field_consumed(body, binds[k].binding, fi))
                    continue;
                n_field_drops++;
            }
        }

        if (n_field_drops > 0) {
            uint32_t new_n = body->as.do_.n + n_field_drops;
            Expr **new_items =
                (Expr **)arena_alloc(e->arena, new_n * sizeof(Expr *));
            memcpy(new_items, body->as.do_.items,
                   body->as.do_.n * sizeof(Expr *));
            uint32_t defer_idx = body->as.do_.n;

            for (uint32_t k = 0; k < n_binds; k++) {
                const AdtDef *ad = elab_byval_drop_adt(binds[k].binding->type);
                if (!ad) continue;
                if (binding_moved_during_init[k] || binds[k].binding->is_moved ||
                    is_binding_consumed(body, binds[k].binding))
                    continue;
                const CtorDef *ctor = ad->ctors[0];
                for (uint32_t fi = 0; fi < ctor->n_fields; fi++) {
                    TypeKind fk = ctor->fields[fi].kind;
                    if (fk != TY_RC && fk != TY_REF && fk != TY_LREF)
                        continue;
                    if (fk != TY_RC && binds[k].binding->shared_view)
                        continue;   /* must match the counting loop above */
                    /* Skip a field the body already drops explicitly (must match
                     * the counting loop above, or new_items over/underflows). */
                    if (is_field_consumed(body, binds[k].binding, fi))
                        continue;

                    /* (.f o) -- read the owning field off the by-value local. */
                    Type fld_ty = ctor->fields[fi].full_type
                                      ? *ctor->fields[fi].full_type
                                      : type_from_kind(fk);
                    Expr *var_expr = expr_new(e->arena, EX_VAR,
                                              binds[k].binding->type, call->span);
                    var_expr->as.var.binding = binds[k].binding;
                    Expr *get_field =
                        expr_new(e->arena, EX_GET_FIELD, fld_ty, call->span);
                    get_field->as.get_field_.struct_expr = var_expr;
                    get_field->as.get_field_.field_idx = fi;
                    get_field->as.get_field_.adt_def = ad;
                    get_field->as.get_field_.adt_ctor = ctor;

                    /* rc field -> (rc/drop (.f o)); ref field -> (drop! (.f o)).
                     * Both mirror exactly what drop_glue_tur_adt_<T> emits for the
                     * field (rc_strong_decrement + drain, or free). */
                    Expr *drop_body = NULL;
                    if (fk == TY_RC) {
                        drop_body =
                            expr_new(e->arena, EX_RC_DROP, TYPE_NIL, call->span);
                        drop_body->as.rc_drop_.expr = get_field;
                    } else {
                        const BuiltinSpec *spec =
                            builtin_lookup(e->sym_drop, fld_ty, 1);
                        if (!spec) {
                            diag_emit(DIAG_ERROR, call->span,
                                      "internal error: drop! builtin not found "
                                      "for by-value ADT owning field");
                            rc = -1;
                            break;
                        }
                        drop_body = expr_new(e->arena, EX_BUILTIN, TYPE_NIL,
                                             call->span);
                        drop_body->as.builtin.spec = spec;
                        drop_body->as.builtin.n = 1;
                        drop_body->as.builtin.args =
                            (Expr **)arena_alloc(e->arena, sizeof(Expr *));
                        drop_body->as.builtin.args[0] = get_field;
                    }

                    Expr *defer_expr =
                        expr_new(e->arena, EX_DEFER, TYPE_NIL, call->span);
                    defer_expr->as.defer_.body = drop_body;

                    /* Capture analysis: the drop body references the local. */
                    uint32_t n_free = 0;
                    Binding **free_vars =
                        collect_free_vars(drop_body, NULL, 0, NULL, 0, &n_free);
                    Binding **captures = NULL;
                    uint8_t n_captures = 0;
                    if (n_free > 0) {
                        captures = (Binding **)arena_alloc(
                            e->arena, n_free * sizeof(Binding *));
                        memcpy(captures, free_vars, n_free * sizeof(Binding *));
                        n_captures = (uint8_t)n_free;
                    }
                    free(free_vars);
                    defer_expr->as.defer_.captures = captures;
                    defer_expr->as.defer_.n_captures = n_captures;
                    defer_expr->as.defer_.is_drop_glue = true;

                    new_items[defer_idx++] = defer_expr;
                }
                if (rc != 0) break;
            }

            body->as.do_.items = new_items;
            body->as.do_.n = new_n;
        }
    }

    /* LT1: At scope exit, verify all linear bindings were consumed */
    if (rc == 0) {
        for (uint32_t k = 0; k < n_binds; k++) {
            Binding *lb = binds[k].binding;
            if (lb->is_linear && !lb->is_linear_consumed && !lb->is_moved) {
                diag_emit_with_code(DIAG_ERROR, lb->span,
                                    TUR_E0100_LINEAR_DROPPED,
                                    "linear value '%s' dropped without being consumed",
                                    lb->name->name);
                rc = -1;
            }
        }
    }

    /* ST1: At scope exit, verify all relevant bindings were used at least once */
    if (rc == 0) {
        for (uint32_t k = 0; k < n_binds; k++) {
            Binding *lb = binds[k].binding;
            if (lb->is_relevant && lb->usage_state == USAGE_UNUSED && !lb->is_moved) {
                diag_emit_with_code(DIAG_ERROR, lb->span,
                                    TUR_E0151_RELEVANT_DROPPED,
                                    "relevant value '%s' dropped without being used",
                                    lb->name->name);
                rc = -1;
            }
        }
    }

    /* Pop scope before returning. */
    e->scope = inner.parent;
    scope_free(&inner);

    /* Clean up move-state tracking memory */
    if (binding_moved_during_init) {
        free(binding_moved_during_init);
    }

    if (rc != 0) { free(binds); return NULL; }

    Expr *out = expr_new(e->arena, EX_LET, body->type, call->span);
    LetBinding *bcopy = NULL;
    if (n_binds > 0) {
        bcopy = (LetBinding *)arena_alloc(e->arena, n_binds * sizeof(LetBinding));
        memcpy(bcopy, binds, n_binds * sizeof(LetBinding));
    }
    free(binds);
    out->as.let_.bindings = bcopy;
    out->as.let_.n = n_binds;
    out->as.let_.body = body;
    any_let_move_drop_to_use(out);
    return out;
}

Expr *elab_do(Elab *e, const Form *call) {
    /* (do body...) — value of last expr; (do) is nil. */
    uint32_t n = call->as.list.len - 1;
    if (n == 0) return e_nil(e, call->span);

    /* Internal defines: splice (define name init) into nested let forms. */
    {
        Form *spliced = splice_internal_defines(e, call->as.list.items + 1, n, call->span);
        if (spliced) return elab_form(e, spliced);
    }

    Expr **items = (Expr **)arena_alloc(e->arena, n * sizeof(Expr *));
    for (uint32_t i = 0; i < n; i++) {
        items[i] = elab_form(e, call->as.list.items[1 + i]);
        if (!items[i]) return NULL;
    }
    
    /* Phase R6: Warn on discarded result values */
    if (g_warn_unused_result) {
        /* Check if this is an ignore! pattern: (do <expr> nil) */
        bool is_ignore_pattern = (n == 2 && items[1]->kind == EX_NIL_LIT);
        
        for (uint32_t i = 0; i < n - 1; i++) {
            /* All items except the last have their values discarded */
            if (items[i]->type.kind == TY_PTR_VOID) {
                /* This is a ptr<void> value being discarded - likely a Result */
                /* Skip warning if this is the ignore! pattern: (do expr nil) */
                if (!is_ignore_pattern || i != 0) {
                    diag_emit(DIAG_WARNING, items[i]->span,
                              "discarded result value of type ptr<void>; use ignore! to suppress this warning");
                }
            }
        }
    }
    
    Expr *out = expr_new(e->arena, EX_DO, items[n - 1]->type, call->span);
    out->as.do_.items = items;
    out->as.do_.n = n;
    return out;
}

/* elab_letstar -- (let* [b1 i1 b2 i2 ...] body...)
 *
 * Sequential-binding let: each binding sees the bindings that precede it.
 * Desugars to a right-nested chain of single-binding `let` forms and delegates
 * to elab_let, so all of let's machinery (annotations, vector destructuring,
 * move/alias tracking, typed inits) carries over unchanged.
 *
 *   (let* [a 1 b (+ a 1)] body)
 *     => (let [a 1] (let [b (+ a 1)] body))
 *
 * A single binding "unit" is: zero or more `^`-prefixed annotation symbols,
 * followed by a name symbol OR a vector destructuring pattern, followed by one
 * initializer form.  This mirrors how elab_let parses each entry.
 */
Expr *elab_letstar(Elab *e, const Form *call) {
    if (call->as.list.len < 2) {
        diag_emit(DIAG_ERROR, call->span, "let* requires a binding vector");
        return NULL;
    }
    Form *bindings = call->as.list.items[1];
    if (bindings->tag != F_VEC) {
        diag_emit(DIAG_ERROR, bindings->span,
                  "let* bindings must be a vector [name init ...]");
        return NULL;
    }
    Span     sp     = call->span;
    uint32_t n_body = call->as.list.len - 2;
    Form   **body   = call->as.list.items + 2;
    uint32_t blen   = bindings->as.list.len;

    /* Split the binding vector into units, recording (start,len) ranges. */
    typedef struct { uint32_t start; uint32_t len; } LsUnit;
    LsUnit  *units   = (blen == 0) ? NULL
                       : (LsUnit *)arena_alloc(e->arena, blen * sizeof(LsUnit));
    uint32_t n_units = 0;
    uint32_t i = 0;
    while (i < blen) {
        uint32_t start = i;
        /* Consume leading `^`-prefixed annotation symbols (^mut, ^linear, ...). */
        while (i < blen &&
               bindings->as.list.items[i]->tag == F_SYM &&
               bindings->as.list.items[i]->as.sym->len > 0 &&
               bindings->as.list.items[i]->as.sym->name[0] == '^') {
            i++;
        }
        if (i >= blen) {
            diag_emit(DIAG_ERROR, bindings->as.list.items[start]->span,
                      "let* trailing annotation with no binding name");
            return NULL;
        }
        i++; /* the name symbol or destructuring pattern */
        /* Optional type annotation between name and init (mirrors elab_let).
         * F_TYPE_ANN is unambiguous; F_KEYWORD only consumed if it names a
         * known type (otherwise it's a keyword value, e.g. macro-expanded). */
        if (i < blen) {
            Form *maybe_ann = bindings->as.list.items[i];
            if (maybe_ann->tag == F_TYPE_ANN) {
                i++;
            } else if (maybe_ann->tag == F_KEYWORD &&
                       maybe_ann->as.sym != NULL &&
                       typekind_from_symbol(maybe_ann->as.sym->name) != TY_UNKNOWN) {
                i++;
            }
        }
        if (i >= blen) {
            diag_emit(DIAG_ERROR, bindings->as.list.items[i - 1]->span,
                      "let* binding is missing its initializer");
            return NULL;
        }
        i++; /* the initializer form */
        units[n_units].start = start;
        units[n_units].len   = i - start;
        n_units++;
    }

    Form *sym_let_f = form_sym(e->arena, sp, e->sym_let);

    /* No bindings: (let* [] body...) == (let [] body...). */
    if (n_units == 0) {
        Form  *empty_bv  = form_vec(e->arena, sp, NULL, 0);
        uint32_t len     = 2 + n_body;
        Form **items     = (Form **)arena_alloc(e->arena, len * sizeof(Form *));
        items[0] = sym_let_f;
        items[1] = empty_bv;
        for (uint32_t k = 0; k < n_body; k++) items[2 + k] = body[k];
        return elab_let(e, form_list(e->arena, sp, items, len));
    }

    /* Build the nested let chain from the innermost unit outward. */
    Form *inner = NULL;
    for (int u = (int)n_units - 1; u >= 0; u--) {
        uint32_t ustart = units[u].start;
        uint32_t ulen   = units[u].len;
        Form **bv_items = (Form **)arena_alloc(e->arena, ulen * sizeof(Form *));
        for (uint32_t k = 0; k < ulen; k++) {
            bv_items[k] = bindings->as.list.items[ustart + k];
        }
        Form *bv = form_vec(e->arena, sp, bv_items, ulen);

        uint32_t  let_len;
        Form    **let_items;
        if (u == (int)n_units - 1) {
            /* Innermost let carries the original body forms. */
            let_len   = 2 + n_body;
            let_items = (Form **)arena_alloc(e->arena, let_len * sizeof(Form *));
            for (uint32_t k = 0; k < n_body; k++) let_items[2 + k] = body[k];
        } else {
            /* Outer lets wrap the next-inner let as their single body form. */
            let_len      = 3;
            let_items    = (Form **)arena_alloc(e->arena, let_len * sizeof(Form *));
            let_items[2] = inner;
        }
        let_items[0] = sym_let_f;
        let_items[1] = bv;
        inner = form_list(e->arena, sp, let_items, let_len);
    }
    return elab_let(e, inner);
}

/* ---- letrec and named let ---- */

/* elab_letrec -- (letrec [f1 i1 f2 i2 ...] body...)
 *
 * Two-pass binding: all names are pre-registered in the inner scope before
 * any initializer is elaborated, enabling self-recursion and mutual recursion
 * between fn-valued bindings.  Non-fn inits that reference their own name will
 * see the TY_UNKNOWN placeholder and type-check against it, producing a
 * type-mismatch error rather than silent undefined behaviour.
 *
 * Restrictions in v1:
 *   - Binding annotations (^linear, ^unique, ^affine, ^relevant, ^mut) are
 *     rejected.  Substructural recursive locals can be top-level defns.
 *   - Vector destructuring is not supported.
 */
/* Is `f` a `(fn [...] ...)` / `(lambda [...] ...)` literal? */
static bool letrec_form_is_fn_literal(Elab *e, const Form *f) {
    if (!f || f->tag != F_LIST || f->as.list.len < 3) return false;
    const Form *h = f->as.list.items[0];
    return h->tag == F_SYM && (h->as.sym == e->sym_fn || h->as.sym == e->sym_lambda) &&
           f->as.list.items[1]->tag == F_VEC;
}

/* Walk a member's init form for symbol references.  Sets *refs_local when a
 * symbol resolves to a local outside the group (a parameter or let binding of
 * the enclosing function), and refs[j] when it names group member j.  Purely
 * syntactic, so it over-approximates: a name an inner binder shadows still
 * counts -- see letrec_predict_closures for why that is safe. */
static void letrec_scan_refs(Elab *e, const Form *f, Binding **group, uint32_t n,
                             const Symbol *const *own_params, uint32_t n_own,
                             bool *refs_local, bool *refs) {
    if (!f) return;
    switch (f->tag) {
        case F_SYM: {
            for (uint32_t i = 0; i < n_own; i++)
                if (own_params[i] == f->as.sym) return;
            Binding *b = scope_lookup(e->scope, f->as.sym);
            if (!b || b->is_global) return;
            for (uint32_t j = 0; j < n; j++) {
                if (group[j] == b) { refs[j] = true; return; }
            }
            *refs_local = true;
            return;
        }
        case F_QUOTE: case F_TYPE_ANN: case F_KEYWORD: case F_CBLOCK:
            return;
        case F_LIST: case F_VEC: case F_MAP: case F_SET: case F_MAP_LITERAL:
        case F_SET_LITERAL: case F_READER_COND: case F_RANGE_VAR:
        case F_QUASIQUOTE: case F_UNQUOTE: case F_UNQUOTE_SPLICING:
        case F_CONTRACT_TYPE:
            for (uint32_t i = 0; i < f->as.list.len; i++)
                letrec_scan_refs(e, f->as.list.items[i], group, n, own_params,
                                 n_own, refs_local, refs);
            return;
        default:
            return;
    }
}

/* letrec-mutual-recursion-between-capturing-closures: mark each `fn` member
 * predicted to capture (letrec_predicted_closure).  A member captures when its
 * init names a local outside the group, names a NON-fn member (a value member
 * is an ordinary local, always captured), or calls a member that captures --
 * the last to a fixpoint, since capturing a closure makes the caller one.
 *
 * Over-approximation is the safe direction.  A member predicted to capture that
 * ends up captureless is lifted to a global, and its sibling's capture of it is
 * skipped at the env struct and at the fill (the Edge-1 is_global skip in
 * EX_CLOSURE emission).  Under-approximation is the bug this fixes. */
static void letrec_predict_closures(Elab *e, Binding **group, Form **inits, uint32_t n) {
    bool *is_fn   = (bool *)calloc(n, sizeof(bool));
    bool *local   = (bool *)calloc(n, sizeof(bool));
    bool *refs    = (bool *)calloc((size_t)n * n, sizeof(bool));
    if (!is_fn || !local || !refs) { fprintf(stderr, "tur: oom\n"); abort(); }
    for (uint32_t k = 0; k < n; k++) is_fn[k] = letrec_form_is_fn_literal(e, inits[k]);
    for (uint32_t k = 0; k < n; k++) {
        if (!is_fn[k]) continue;
        /* The lambda's own parameter names are not references out of it. */
        const Form *pv = inits[k]->as.list.items[1];
        const Symbol **own = (const Symbol **)calloc(pv->as.list.len + 1, sizeof(Symbol *));
        if (!own) { fprintf(stderr, "tur: oom\n"); abort(); }
        uint32_t n_own = 0;
        for (uint32_t i = 0; i < pv->as.list.len; i++)
            if (pv->as.list.items[i]->tag == F_SYM) own[n_own++] = pv->as.list.items[i]->as.sym;
        for (uint32_t i = 2; i < inits[k]->as.list.len; i++)
            letrec_scan_refs(e, inits[k]->as.list.items[i], group, n, own, n_own,
                             &local[k], &refs[(size_t)k * n]);
        free(own);
        for (uint32_t j = 0; j < n; j++)
            if (j != k && refs[(size_t)k * n + j] && !is_fn[j]) local[k] = true;
    }
    for (bool changed = true; changed; ) {
        changed = false;
        for (uint32_t k = 0; k < n; k++) {
            if (!is_fn[k] || local[k]) continue;
            for (uint32_t j = 0; j < n; j++) {
                if (j != k && refs[(size_t)k * n + j] && local[j]) {
                    local[k] = true; changed = true; break;
                }
            }
        }
    }
    for (uint32_t k = 0; k < n; k++)
        if (is_fn[k] && local[k]) group[k]->letrec_predicted_closure = true;
    free(is_fn); free(local); free(refs);
}

Expr *elab_letrec(Elab *e, const Form *call) {
    if (call->as.list.len < 3) {
        diag_emit(DIAG_ERROR, call->span,
                  "letrec requires a binding vector and a body: (letrec [name init ...] body...)");
        return NULL;
    }
    Form *bvec = call->as.list.items[1];
    if (bvec->tag != F_VEC) {
        diag_emit(DIAG_ERROR, bvec->span,
                  "letrec bindings must be a vector [name init ...]");
        return NULL;
    }
    uint32_t blen = bvec->as.list.len;
    if (blen % 2 != 0) {
        diag_emit(DIAG_ERROR, bvec->span,
                  "letrec binding vector must have an even number of forms (name init pairs)");
        return NULL;
    }
    uint32_t n_entries = blen / 2;

    /* Temporary per-binding metadata, freed before return. */
    typedef struct { const Symbol *name; Span span; Form *init_form; } LrEntry;
    LrEntry *entries = (LrEntry *)malloc(n_entries * sizeof(LrEntry));
    if (!entries) { fprintf(stderr, "tur: oom\n"); abort(); }
    Binding **pre_b = (Binding **)malloc(n_entries * sizeof(Binding *));
    if (!pre_b) { fprintf(stderr, "tur: oom\n"); abort(); }

    Scope inner;
    scope_init(&inner, e->scope);
    e->scope = &inner;
    int rc = 0;

    /* Parse binding vector: reject annotations, collect name/init pairs. */
    for (uint32_t k = 0; k < n_entries && rc == 0; k++) {
        Form *name_f = bvec->as.list.items[k * 2];
        Form *init_f = bvec->as.list.items[k * 2 + 1];
        /* Reject substructural / mutability annotations that make no sense in
         * a pre-registered binding group. */
        if (name_f->tag == F_SYM && (
            name_f->as.sym == e->sym_caret_mut       ||
            name_f->as.sym == e->sym_caret_linear    ||
            name_f->as.sym == e->sym_caret_unique    ||
            name_f->as.sym == e->sym_caret_affine    ||
            name_f->as.sym == e->sym_caret_relevant  ||
            name_f->as.sym == e->sym_caret_persistent)) {
            diag_emit(DIAG_ERROR, name_f->span,
                      "letrec does not support binding annotations ('^%s'); "
                      "use a top-level defn for annotated recursive bindings",
                      name_f->as.sym->name);
            rc = -1; break;
        }
        if (name_f->tag != F_SYM) {
            diag_emit(DIAG_ERROR, name_f->span,
                      "letrec binding name must be a symbol");
            rc = -1; break;
        }
        /* Duplicate name check. */
        for (uint32_t j = 0; j < k; j++) {
            if (entries[j].name == name_f->as.sym) {
                diag_emit(DIAG_ERROR, name_f->span,
                          "letrec: '%s' is bound twice in the same binding group",
                          name_f->as.sym->name);
                rc = -1; break;
            }
        }
        if (rc != 0) break;
        entries[k].name      = name_f->as.sym;
        entries[k].span      = name_f->span;
        entries[k].init_form = init_f;
    }

    /* Pass A -- pre-register all names with placeholder types.
     * If the init is a (fn ...) literal, peek at arity and return type to build
     * a TY_FN stub so callers see the right arity during init elaboration.
     * Otherwise fall back to TY_UNKNOWN. */
    for (uint32_t k = 0; k < n_entries && rc == 0; k++) {
        Form *init_f = entries[k].init_form;
        Type placeholder = TYPE_UNKNOWN;
        if (init_f->tag == F_LIST && init_f->as.list.len >= 3) {
            Form *ih = init_f->as.list.items[0];
            if (ih->tag == F_SYM &&
                (ih->as.sym == e->sym_fn || ih->as.sym == e->sym_lambda)) {
                Form *params_f = init_f->as.list.items[1];
                if (params_f->tag == F_VEC) {
                    /* Same `^`-marker-aware scan as the defmodule/top-level
                     * forward-decl pre-pass: a `(fn [^fat g ...] ...)` literal
                     * must not count its ^fat marker as a parameter slot. */
                    TypeKind *arg_kinds = NULL;
                    uint32_t arity = fwd_decl_scan_params(e->arena, params_f, &arg_kinds);
                    /* Peek at the return-type keyword at index 2 (fn [params] :ret body).
                     *
                     * r7rs-lang-plan R6 (docs/archive/r7rs-compiled-dynamic-shapes.md
                     * section 1): in a DYNAMIC file the unannotated default is
                     * `any`, exactly as elab_defn pins a Saffron signature.  The
                     * `int` placeholder made the recursive call inside a named
                     * let / `do` / letrec lambda type `int` while the lambda
                     * itself returned `any`, and the emitter wrapped the tagged
                     * result in an int tag ("aggregate value used where an
                     * integer was expected" from cc).  Pass B below pins the
                     * lambda's own return to match. */
                    TypeKind ret_kind = lang_span_is_dynamic(init_f->span) ? TY_ANY : TY_INT;
                    if (init_f->as.list.len >= 4) {
                        Form *ret_f = init_f->as.list.items[2];
                        /* Accept spaced `: T` (F_TYPE_ANN{F_SYM/F_KEYWORD}) too. */
                        if (ret_f->tag == F_TYPE_ANN && ret_f->as.list.len == 1 &&
                            (ret_f->as.list.items[0]->tag == F_SYM ||
                             ret_f->as.list.items[0]->tag == F_KEYWORD)) {
                            ret_f = ret_f->as.list.items[0];
                        } else if (ret_f->tag == F_TYPE_ANN && ret_f->as.list.len == 1 &&
                                   ret_f->as.list.items[0]->tag == F_NIL) {
                            /* forward-referenced-nil-call-bound-to-auto-type: a
                             * bare `nil` in type position reads as F_NIL, so the
                             * unwrap above misses it and `: nil` collapsed to the
                             * TY_INT placeholder.  Same gap as the defmodule
                             * pre-pass (elab_module.c). */
                            ret_kind = TY_NIL;
                            ret_f = NULL;
                        }
                        if (ret_f && (ret_f->tag == F_KEYWORD || ret_f->tag == F_SYM)) {
                            const char *rn = ret_f->as.sym->name;
                            uint32_t   rl = ret_f->as.sym->len;
                            if      (rl == 3 && memcmp(rn, "int",  3) == 0) ret_kind = TY_INT;
                            else if (rl == 4 && memcmp(rn, "bool", 4) == 0) ret_kind = TY_BOOL;
                            else if (rl == 4 && memcmp(rn, "void", 4) == 0) ret_kind = TY_NIL;
                            else if (rl == 3 && memcmp(rn, "nil",  3) == 0) ret_kind = TY_NIL;
                            else if (rl == 4 && memcmp(rn, "cstr", 4) == 0) ret_kind = TY_CSTR;
                            /* W1 follow-on (float carrier): `float` is an inline
                             * carrier scalar, not a struct/app/ADT, so the override
                             * block below (which only fires for those kinds) never
                             * reached it -- a `:float` self-recursive letrec
                             * collapsed to the int64 carrier exactly like a Box did.
                             * The self-call then typed `int` (spurious then=float
                             * else=int mismatch); ascribing the call to `:float` to
                             * silence that produced an int->double union truncation
                             * at the carrier-decode site (emit_core.c), so it
                             * type-checked but returned 0.0 at runtime.  Resolve the
                             * scalar here, matching the top-level forward-decl pass
                             * (elab_module.c) and #460's RR1.  `float64` is an alias
                             * for `float`.  `float32` lives in a separate register
                             * class (xmm vs gp) and is still policed by the E0707
                             * guard, so it is left out of this scalar fast-path. */
                            else if (rl == 5 && memcmp(rn, "float",   5) == 0) ret_kind = TY_FLOAT;
                            else if (rl == 7 && memcmp(rn, "float64", 7) == 0) ret_kind = TY_FLOAT;
                            /* Stage 2 (macro-system-direction-plan): Syntax is
                             * a simple scalar-class kind (a copyable Form*
                             * handle); without this a `: Syntax` self-recursive
                             * letrec fn -- the canonical defmacro* field-walker
                             * -- collapses to the int carrier and the self-call
                             * trips a spurious then=Syntax else=int mismatch. */
                            else if (rl == 6 && memcmp(rn, "Syntax",  6) == 0) ret_kind = TY_SYNTAX;
                            else if (rl == 3 && memcmp(rn, "any",     3) == 0) ret_kind = TY_ANY;
                        }
                    }
                    placeholder = type_fn(arg_kinds, (uint8_t)arity, ret_kind);
                    /* r7rs-lang-plan R7: a variadic lambda's rest shape. */
                    fwd_decl_apply_variadic(e, e->arena, &placeholder, params_f);
                    /* W1 (letrec self-recursion): the scalar ret_kind peek
                     * above resolves only int/bool/void/nil/cstr and collapses
                     * every other declared return -- a :copy struct, a (Vec T),
                     * an ADT -- to the int64 carrier (TY_INT).  A self-recursive
                     * `fn` bound here reads that carrier placeholder when its own
                     * call inside the body is type-checked (Pass B, below), so the
                     * recursive call comes back typed `int`; an `if` whose other
                     * arm is the real return type then fails with a spurious
                     * branch mismatch (then=Box else=int).  Resolve the declared
                     * return form fully and stamp the carrier-lowered result onto
                     * the placeholder, mirroring #460's RR1 fix for top-level
                     * defn.  Only carrier kinds (struct / type-app / ADT) override
                     * the scalar fast-path; a bare F_LIST at index 2 is a body
                     * form (e.g. `(go ...)`), not a return annotation, so the
                     * F_KEYWORD/F_SYM/F_TYPE_ANN gate skips it. */
                    if (init_f->as.list.len >= 4) {
                        Form *ret_form = init_f->as.list.items[2];
                        if (ret_form->tag == F_KEYWORD ||
                            ret_form->tag == F_SYM ||
                            ret_form->tag == F_TYPE_ANN) {
                            Type *rft = fn_type_from_form(e, ret_form,
                                                          NULL, NULL, 0);
                            /* structdef-retirement DS-C: the TY_STRUCT disjunct
                             * is dead -- a return type is never TY_STRUCT. */
                            if (rft && (rft->kind == TY_APP ||
                                        rft->kind == TY_ADT)) {
                                placeholder.as.fn.result_kind      = rft->kind;
                                placeholder.as.fn.result_full_type = rft;
                            }
                        }
                    }
                }
            }
        }
        Binding *b = binding_new(e, entries[k].name, placeholder,
                                 false, false, entries[k].span);
        /* Edge 1: mark every letrec/named-let group member so collect_free_vars
         * recognizes a call to it (gate accept) and the active self-exclude
         * group can keep a direct self/mutual recursive call out of the capture
         * set while a nested-closure reference is captured. */
        b->is_letrec_binding = true;
        scope_add(&inner, b);
        pre_b[k] = b;
    }

    /* letrec-mutual-recursion-between-capturing-closures: predict, before any
     * init is elaborated, which `fn` members will CAPTURE.  A call to a group
     * member from a sibling's top-level body is left to the recursion
     * machinery only while that member is not known to be a closure -- and a
     * LATER member has no init yet when an earlier one is elaborated, so a call
     * to it was never captured and the lifted body named an undeclared local.
     * With the prediction, collect_free_vars captures it, and the emitter ties
     * the knot (the earlier env's slot is patched once the later member is
     * bound).  Over-predicting is harmless: a member that turns out captureless
     * is lifted to a global, and a capture of a global member is skipped at
     * both the env struct and its fill (Edge 1). */
    if (rc == 0 && n_entries > 1) {
        Form **inits = (Form **)malloc(n_entries * sizeof(Form *));
        if (!inits) { fprintf(stderr, "tur: oom\n"); abort(); }
        for (uint32_t k = 0; k < n_entries; k++) inits[k] = entries[k].init_form;
        letrec_predict_closures(e, pre_b, inits, n_entries);
        free(inits);
    }

    /* Pass B -- elaborate each init inside the inner scope, then patch the
     * pre-registered binding's type with the actual elaborated type. */
    LetBinding *binds = NULL;
    uint32_t n_binds = 0;
    if (rc == 0) {
        binds = (LetBinding *)malloc(n_entries * sizeof(LetBinding));
        if (!binds) { fprintf(stderr, "tur: oom\n"); abort(); }
    }
    for (uint32_t k = 0; k < n_entries && rc == 0; k++) {
        /* Edge 1: publish the whole group as the self-exclude set just for this
         * init's elaboration.  elab_fn snapshots+clears it on entry (so only the
         * init's own top-level lambda excludes the group; nested closures see an
         * empty group).  pre_b is the group array and stays live through Pass B.
         * Cleared right after so it never leaks into the body (Pass C) or into a
         * sibling value init -- a lambda there must capture group members. */
        e->letrec_self_group   = pre_b;
        e->letrec_self_group_n = n_entries;
        /* r7rs-lang-plan R6: pin an unannotated lambda init's return to `any`
         * in a dynamic file, so the lambda agrees with the `any` placeholder
         * Pass A gave its callers.  Without this a nil-tailed body (the loop
         * that ends in `display`) inferred `nil` while the group's other
         * members called it as `any`.  The rewrite is at the Form level:
         * `(fn [params] body...)` becomes `(fn [params] : any body...)`. */
        Form *init_form = entries[k].init_form;
        if (init_form->tag == F_LIST && init_form->as.list.len >= 3 &&
            lang_span_is_dynamic(init_form->span)) {
            Form *ih = init_form->as.list.items[0];
            Form *rf = init_form->as.list.items[2];
            bool annotated = (rf->tag == F_TYPE_ANN || rf->tag == F_KEYWORD ||
                              (rf->tag == F_SYM && rf->as.sym->name[0] == ':'));
            if (ih->tag == F_SYM &&
                (ih->as.sym == e->sym_fn || ih->as.sym == e->sym_lambda) &&
                init_form->as.list.items[1]->tag == F_VEC && !annotated) {
                uint32_t len = init_form->as.list.len;
                Form **items = (Form **)arena_alloc(e->arena, (len + 1) * sizeof(Form *));
                items[0] = init_form->as.list.items[0];
                items[1] = init_form->as.list.items[1];
                Form *any_sym = form_sym(e->arena, init_form->span,
                                         symtab_intern(e->st, strslice("any", 3)));
                items[2] = form_type_ann(e->arena, init_form->span, any_sym);
                for (uint32_t j = 2; j < len; j++) items[j + 1] = init_form->as.list.items[j];
                init_form = form_list(e->arena, init_form->span, items, len + 1);
            }
        }
        pre_b[k]->letrec_elaborating = true;
        Expr *init = elab_form(e, init_form);
        pre_b[k]->letrec_elaborating = false;
        e->letrec_self_group   = NULL;
        e->letrec_self_group_n = 0;
        if (!init) { rc = -1; break; }
        /* Patch the pre-registered binding with the real type. */
        pre_b[k]->type = init->type;
        /* For fn-valued bindings, mark as global so the emitter uses direct
         * calls (C function name) rather than cast-and-call through a local
         * pointer -- enabling self-recursion and mutual recursion. */
        /* For no-capture fn bindings (EX_VAR pointing to a file-scope static fn),
         * mark as global so callers emit direct C function calls, enabling
         * self-recursion and mutual recursion without closure overhead. */
        if (init->kind == EX_VAR && init->type.kind == TY_FN) {
            Binding *fn_b = init->as.var.binding;
            char *fn_c_name = elab_mangle_binding_name(fn_b);
            pre_b[k]->is_global = true;
            pre_b[k]->c_export_name = arena_strdup(e->arena, fn_c_name, strlen(fn_c_name));
            free(fn_c_name);
        }
        /* Propagate closure metadata (mirrors elab_let). */
        Binding *cl = expr_closure_fn_binding(init);
        if (cl) pre_b[k]->closure_fn_binding = cl;
        /* Phase HRT4: poly fn metadata propagation. */
        if (init->kind == EX_VAR) {
            Binding *ib = init->as.var.binding;
            if (ib->is_poly_fn) {
                pre_b[k]->is_poly_fn = true;
                pre_b[k]->poly_type  = ib->poly_type;
            } else if (ib->type.kind == TY_FN) {
                Binding *root = ib->source_binding ? ib->source_binding : ib;
                if (root->is_global) pre_b[k]->source_binding = root;
            }
        }
        binds[n_binds].binding = pre_b[k];
        binds[n_binds].init    = init;
        n_binds++;
    }

    /* Pass C -- elaborate body. */
    Expr *body = NULL;
    if (rc == 0) {
        uint32_t body_count = call->as.list.len - 2;
        if (body_count == 0) {
            body = e_nil(e, call->span);
        } else {
            Form *spliced = splice_internal_defines(e,
                                call->as.list.items + 2, body_count, call->span);
            if (spliced) {
                body = elab_form(e, spliced);
                if (!body) rc = -1;
            } else if (body_count == 1) {
                body = elab_form(e, call->as.list.items[2]);
                if (!body) rc = -1;
            } else {
                Expr **items = (Expr **)arena_alloc(e->arena, body_count * sizeof(Expr *));
                for (uint32_t k = 0; k < body_count && rc == 0; k++) {
                    items[k] = elab_form(e, call->as.list.items[2 + k]);
                    if (!items[k]) rc = -1;
                }
                if (rc == 0) {
                    body = expr_new(e->arena, EX_DO, items[body_count-1]->type, call->span);
                    body->as.do_.items = items;
                    body->as.do_.n = body_count;
                }
            }
        }
    }

    e->scope = inner.parent;
    scope_free(&inner);
    free(entries);
    free(pre_b);

    if (rc != 0) {
        free(binds);
        return NULL;
    }

    Expr *out = expr_new(e->arena, EX_LETREC, body->type, call->span);
    LetBinding *bcopy = (LetBinding *)arena_alloc(e->arena, n_binds * sizeof(LetBinding));
    memcpy(bcopy, binds, n_binds * sizeof(LetBinding));
    free(binds);
    out->as.let_.bindings = bcopy;
    out->as.let_.n        = n_binds;
    out->as.let_.body     = body;
    return out;
}

/* elab_named_let -- (let name [p1 v1 p2 v2 ...] body...)
 *
 * Desugars to:
 *   (letrec [name (fn [p1 p2 ...] body...)]
 *     (name v1 v2 ...))
 *
 * Type annotations carry through verbatim: [n :int 10 ...] becomes (fn [n :int] ...).
 */
Expr *elab_named_let(Elab *e, const Form *call) {
    /* call: (let <sym> [p1 v1 ...] body...) */
    Form *loop_name_f = call->as.list.items[1]; /* F_SYM checked by caller */
    const Symbol *loop_sym = loop_name_f->as.sym;
    Form *bvec = call->as.list.items[2]; /* F_VEC checked by caller */
    Span sp = call->span;
    Arena *a = e->arena;

    if (call->as.list.len < 4) {
        diag_emit(DIAG_ERROR, sp,
                  "named let requires a body: (let %s [...] body...)", loop_sym->name);
        return NULL;
    }

    /* Walk the binding vector, separating param specs from initial values.
     * Format: [name1 [:type1] init1  name2 [:type2] init2 ...]
     * fn_param_items: the items that go inside (fn [...] ...)
     * init_items:     the initial call arguments for (name v1 v2 ...) */
    uint32_t blen = bvec->as.list.len;
    Form **fn_param_items = (Form **)arena_alloc(a, blen * sizeof(Form *));
    Form **init_items     = (Form **)arena_alloc(a, blen * sizeof(Form *));
    uint32_t n_params = 0, n_inits = 0;

    uint32_t bi = 0;
    while (bi < blen) {
        Form *cur = bvec->as.list.items[bi];
        if (cur->tag != F_SYM) {
            diag_emit(DIAG_ERROR, cur->span,
                      "named let: binding name must be a symbol");
            return NULL;
        }
        fn_param_items[n_params++] = cur;  /* the param name */
        bi++;
        /* Optional :type annotation -- accept both fused `:type` (F_KEYWORD)
         * and spaced `: type` (F_TYPE_ANN); fn elaboration handles both. */
        if (bi < blen &&
            (bvec->as.list.items[bi]->tag == F_KEYWORD ||
             bvec->as.list.items[bi]->tag == F_TYPE_ANN)) {
            fn_param_items[n_params++] = bvec->as.list.items[bi++];
        }
        /* Mandatory initializer */
        if (bi >= blen) {
            diag_emit(DIAG_ERROR, cur->span,
                      "named let: missing initializer for parameter '%s'", cur->as.sym->name);
            return NULL;
        }
        init_items[n_inits++] = bvec->as.list.items[bi++];
    }

    /* Build (fn [fn_param_items...] body...) */
    uint32_t body_count = call->as.list.len - 3; /* items after the bvec */
    uint32_t fn_len = 2 + body_count;             /* fn + params_vec + body... */
    Form **fn_items_arr = (Form **)arena_alloc(a, fn_len * sizeof(Form *));
    fn_items_arr[0] = form_sym(a, sp, e->sym_fn);
    fn_items_arr[1] = form_vec(a, sp, fn_param_items, n_params);
    for (uint32_t i = 0; i < body_count; i++)
        fn_items_arr[2 + i] = call->as.list.items[3 + i];
    Form *fn_form = form_list(a, sp, fn_items_arr, fn_len);

    /* Build letrec binding vector [loop_sym fn_form] */
    Form *lr_bvec_arr[2];
    lr_bvec_arr[0] = form_sym(a, sp, loop_sym);
    lr_bvec_arr[1] = fn_form;
    Form *lr_bvec = form_vec(a, sp, lr_bvec_arr, 2);

    /* Build call form (loop_sym init_items...) */
    uint32_t call_len = 1 + n_inits;
    Form **call_items_arr = (Form **)arena_alloc(a, call_len * sizeof(Form *));
    call_items_arr[0] = form_sym(a, sp, loop_sym);
    for (uint32_t i = 0; i < n_inits; i++) call_items_arr[1 + i] = init_items[i];
    Form *call_form = form_list(a, sp, call_items_arr, call_len);

    /* Build (letrec lr_bvec call_form) */
    Form *lr_arr[3];
    lr_arr[0] = form_sym(a, sp, e->sym_letrec);
    lr_arr[1] = lr_bvec;
    lr_arr[2] = call_form;
    Form *letrec_form = form_list(a, sp, lr_arr, 3);

    return elab_form(e, letrec_form);
}

/* TY3: recognize a flow-narrowing guard in an `if` condition Form.
 *
 * Two supported shapes, both yielding (variable symbol, type-name symbol):
 *   (is? x T)              -- the dedicated type-test predicate
 *   (= (type-of x) "T")    -- type-of compared against a string literal
 *
 * On a match, *out_var receives x's Symbol and *out_type receives T's Symbol
 * (interned from the string literal in the type-of shape), and returns true.
 * Only direct, single-variable tests narrow; negation/conjunction do not (see
 * TY3.3).  Recognition is purely syntactic on the un-elaborated Form. */
static bool if_guard_narrowing(Elab *e, const Form *cond,
                               const Symbol **out_var, Form **out_type) {
    if (!cond || cond->tag != F_LIST || cond->as.list.len < 1) return false;
    Form *head = cond->as.list.items[0];
    if (head->tag != F_SYM) return false;

    /* Shape 1: (is? x T), where T is a bare name OR an APPLIED type form.
     *
     * The applied form used to be refused here (`tf->tag == F_SYM` only), so
     * `(if (is? x (Option float)) ...)` tested the tag and then narrowed
     * nothing -- the body still saw `x : any` and a method call on it was a
     * "cannot dispatch on an `any` receiver" diagnostic. The author had to
     * repeat the target in an explicit `(cast x (Option float))` that the
     * guard had already proved. `(is? x Circle)` narrowed on the same line,
     * so the two shapes behaved differently with nothing saying why.
     *
     * `is?` and `cast` have shared one target resolver since
     * any-narrowing-broken-for-parametric-receivers was fixed, so an applied
     * target already resolves for both; passing the FORM through rather than a
     * Symbol is all that was missing. */
    if (head->as.sym == e->sym_is_q && cond->as.list.len == 3) {
        Form *xf = cond->as.list.items[1];
        Form *tf = cond->as.list.items[2];
        if (xf->tag == F_SYM && (tf->tag == F_SYM || tf->tag == F_LIST)) {
            *out_var  = xf->as.sym;
            *out_type = tf;
            return true;
        }
        return false;
    }

    /* Shape 2: (= (type-of x) "T") */
    const Symbol *sym_eq = symtab_intern(e->st, strslice("=", 1));
    if (head->as.sym == sym_eq && cond->as.list.len == 3) {
        Form *lhs = cond->as.list.items[1];
        Form *rhs = cond->as.list.items[2];
        /* lhs must be (type-of x) with x a bare symbol */
        if (lhs->tag != F_LIST || lhs->as.list.len != 2) return false;
        Form *lhead = lhs->as.list.items[0];
        Form *xf    = lhs->as.list.items[1];
        if (lhead->tag != F_SYM || lhead->as.sym != e->sym_type_of) return false;
        if (xf->tag != F_SYM) return false;
        /* rhs must be a string literal naming the type */
        if (rhs->tag != F_STR) return false;
        *out_var  = xf->as.sym;
        /* Shape 2's target is a STRING naming the type; rebuild it as the
         * symbol form the cast expects, so both shapes hand back a Form. */
        *out_type = form_sym(e->arena, rhs->span,
                             symtab_intern(e->st, rhs->as.s));
        return true;
    }

    return false;
}

/* TY3: wrap a branch Form so the narrowed variable is rebound to the unboxed
 * value: <branch>  =>  (let [x (cast x T)] <branch>).  Reuses the TY2 checked
 * cast, so a use of x at type T inside the branch type-checks and the runtime
 * tag is verified.  Returns the original branch if any piece cannot be built. */
static Form *if_narrow_branch(Elab *e, Form *branch,
                              const Symbol *var, Form *type_form, Span sp) {
    Arena *a = e->arena;
    /* (cast x T) -- T spliced as a FORM, so an applied target like
     * `(Option float)` survives; it used to be re-synthesised from a Symbol,
     * which is why only a bare name could reach here. */
    Form *cast_items[3];
    cast_items[0] = form_sym(a, sp, e->sym_cast);
    cast_items[1] = form_sym(a, sp, var);
    cast_items[2] = type_form;
    Form *cast_f = form_list(a, sp, cast_items, 3);
    /* binding vector [x (cast x T)] */
    Form *bvec_items[2] = { form_sym(a, sp, var), cast_f };
    Form *bvec = form_vec(a, sp, bvec_items, 2);
    /* (let [x (cast x T)] branch) */
    Form *let_items[3] = { form_sym(a, sp, e->sym_let), bvec, branch };
    return form_list(a, sp, let_items, 3);
}

/* vec-get-typed-fat-closure-readback: is this expression a fat-closure value --
 * i.e. a directly-callable ^fat fn binding, or a boxed TY_FN closure value?
 * Such a value is representationally a void* fat box, so it unifies with a
 * :ptr<void> branch in an `if`.  A thin (unboxed) bare fn pointer is NOT a fat
 * box, so it is excluded to preserve the raw-pointer-vs-closure split. */
static bool expr_is_fat_closure_value(const Expr *x) {
    if (!x) return false;
    if (x->kind == EX_VAR && x->as.var.binding && x->as.var.binding->is_fat)
        return true;
    if (x->type.kind == TY_FN && x->type.as.fn.boxed)
        return true;
    return false;
}

/* fn-value-carrier-fat-seam-residuals (cell 2): is this expression a VAR of a
 * carrier (tur_poly_fn_t) fn param -- the by-value poly carrier, whose elab
 * type is spelled ptr<void>?  Peeks through ascriptions.  Used by the
 * if-branch unifier to admit carrier-vs-boxed-fn joins by inserting the
 * poly-to-fat conversion on this arm. */
static bool expr_is_poly_carrier_fn_var(const Expr *x) {
    while (x && x->kind == EX_ASCRIBE) x = x->as.ascribe_.inner;
    return x && x->kind == EX_VAR && x->as.var.binding &&
           x->as.var.binding->is_poly_fn && x->as.var.binding->is_param;
}

/* M2b: if-branch tyvar tolerance.
 *
 * The strict `type_eq` used for if-branch parity treats `(Option A)` (A bare
 * tyvar) and `(Option int)` as distinct.  After M2b polymorphized `(none)` /
 * `(some x)` over A, the natural pattern
 *
 *   (if cond (none) (some x))
 *
 * fails: then-branch is `(Option A)`, else-branch is `(Option int)`, and they
 * don't unify even though A := int trivially makes them agree.
 *
 * Walk both types structurally; whenever one side carries a bare TY_TYVAR
 * where the other side has a concrete kind, accept it.  When both sides
 * agree everywhere else, pick the more-concrete side as the if result
 * (so downstream callers see `Option int`, not `Option A`).  Returns false
 * if the types disagree in any non-tyvar position.
 *
 * Scope: only used inside if-branch comparison; the rest of the elaborator
 * keeps strict `type_eq` semantics.  Mirrors the spec-resolver pattern used
 * in elab_make_struct for unbound result tyvars. */
static bool type_eq_tyvar_tolerant(Type a, Type b, Type *out_concrete) {
    if (a.kind == TY_TYVAR && b.kind != TY_TYVAR) {
        if (out_concrete) *out_concrete = b;
        return true;
    }
    if (b.kind == TY_TYVAR && a.kind != TY_TYVAR) {
        if (out_concrete) *out_concrete = a;
        return true;
    }
    if (a.kind != b.kind) return false;
    if (a.kind == TY_APP) {
        if (!a.as.app.fn || !b.as.app.fn || !a.as.app.arg || !b.as.app.arg)
            return false;
        Type fn_concrete = *a.as.app.fn;
        Type arg_concrete = *a.as.app.arg;
        if (!type_eq_tyvar_tolerant(*a.as.app.fn, *b.as.app.fn, &fn_concrete))
            return false;
        if (!type_eq_tyvar_tolerant(*a.as.app.arg, *b.as.app.arg, &arg_concrete))
            return false;
        if (out_concrete) *out_concrete = a;  /* shape preserved, args refined */
        /* Rebuild a fresh TY_APP with the concrete sub-types woven in. */
        if (out_concrete) {
            Type out = a;
            /* Best-effort: drop the recovered args back through static buffers
             * the same way type_app does at elab time would be cleaner, but
             * for the if-result we just preserve `a`'s shape — the use site
             * only consults the result type's outermost kind for ABI
             * decisions, and the inner tyvar gets resolved per call site
             * downstream through ordinary spec resolution. */
            out.as.app.fn = a.as.app.fn;
            out.as.app.arg = a.as.app.arg;
            (void)fn_concrete;
            (void)arg_concrete;
            *out_concrete = out;
        }
        return true;
    }
    if (a.kind == TY_TYVAR) {
        /* Both sides are tyvars; accept (existing type_eq already does). */
        if (out_concrete) *out_concrete = a;
        return true;
    }
    return type_eq(a, b);
}

static bool if_branches_unify_via_tyvar(Type then_ty, Type else_ty, Type *out) {
    Type result = then_ty;
    /* fmap-over-underdetermined-constructor-is-a-defless-shell: a branch
     * whose constructor left a parameter OPEN is the carrier, as the bare ADT
     * it used to be typed as was.  Two such branches over one ADT --
     * `(if c (Ok 1.5) (Err 3))` -- join to that bare ADT, as they did.  An
     * open branch beside a concrete one was a mismatch against the bare ADT
     * and stays one: the tolerant walk below would read the open slot as a
     * wildcard and keep the carrier shape over a by-value peer. */
    if (type_has_open_slot(&then_ty) || type_has_open_slot(&else_ty)) {
        AdtDef *td = (then_ty.kind == TY_APP) ? type_adt_app_def(&then_ty)
                   : (then_ty.kind == TY_ADT) ? then_ty.as.adt_.def : NULL;
        AdtDef *ed = (else_ty.kind == TY_APP) ? type_adt_app_def(&else_ty)
                   : (else_ty.kind == TY_ADT) ? else_ty.as.adt_.def : NULL;
        bool then_open = then_ty.kind == TY_ADT || type_has_open_slot(&then_ty);
        bool else_open = else_ty.kind == TY_ADT || type_has_open_slot(&else_ty);
        if (td && td == ed && then_open && else_open) {
            if (out) *out = type_adt(td);
            return true;
        }
        return false;
    }
    if (!type_eq_tyvar_tolerant(then_ty, else_ty, &result)) return false;
    /* Prefer the side with no bare tyvars at the outermost position. */
    if (then_ty.kind == TY_TYVAR && else_ty.kind != TY_TYVAR) result = else_ty;
    else if (else_ty.kind == TY_TYVAR && then_ty.kind != TY_TYVAR) result = then_ty;
    else result = then_ty;
    if (out) *out = result;
    return true;
}

/* saffron-lang-plan D7: refuse a feature whose whole content is a static
 * guarantee, in a file whose default type is `any`.
 *
 * `feature` names it as the source spells it (`with-region`, `defgadt`,
 * `^linear`); `why` is the one clause that says what the guarantee rests on,
 * so the message is specific rather than a shared "not supported here".
 *
 * Returns true when it rejected, so a caller can bail on the same line.  False
 * outside a Saffron file, which is every existing call path -- a Turmeric
 * module in the same project keeps all of these.
 *
 * Deliberately NOT a surface scan of the form tree.  Each call site sits where
 * the compiler has already RESOLVED what it is looking at (a binding, a
 * special-form dispatch, a parsed parameter attribute), so a local named
 * `with-region` or a macro that expands to one is judged by what it elaborates
 * to, not by how it is spelled. */
bool saffron_reject_static_only(Elab *e, Span span, const char *feature,
                                const char *why) {
    (void)e;
    if (!lang_span_is_dynamic(span)) return false;
    diag_emit_with_code(DIAG_ERROR, span, TUR_E0312_SAFFRON_STATIC_ONLY,
                        "`%s` is not available in a `#lang saffron` file: %s",
                        feature, why);
    diag_emit(DIAG_NOTE, span,
              "the restriction is per file -- move this into a Turmeric module "
              "and call across the boundary; see `tur explain TUR-E0312`");
    return true;
}

/* saffron-lang-plan D4: wrap an `any`-typed expression in the truthiness
 * operator so it can feed a C-level `bool` slot.
 *
 * Only nil and `false` are falsy; everything else -- 0, "", an empty vector --
 * is truthy, which is why this is a runtime tag decision and not a comparison.
 * Lowered as a dynamic operator rather than a second node kind: it is exactly
 * what EX_DYN_OP is for.  The reserved name is not a builtin, so the
 * interpreter answers it before consulting the builtin table.
 *
 * Returns the expression unchanged when it is not an `any` in a Saffron file,
 * so callers can apply it unconditionally.
 *
 * SHARED, because there is more than one bool slot a Saffron `any` can reach:
 * an `if`/`when` condition, and a `#refine{...}` contract predicate, whose
 * `tur-contract-check` takes a `bool`.  The contract path had no wrap, so a
 * refinement over `any` emitted `tur_contract_check(<tur_tagged_t>, ...)` --
 * uncompilable C, on a path the interpreter ran correctly. */
Expr *elab_saffron_truthy(Elab *e, Expr *cond, Span site) {
    if (!cond || cond->type.kind != TY_ANY) return cond;
    /* saffron-dynamic-surface-pass M10: the FORM's span counts too -- a
     * macro-expanded condition (`(if (map-get m k) ...)`) carries the
     * stdlib's span, and was "if condition must be bool, got any" even in a
     * Saffron file. */
    if (!lang_span_is_dynamic(cond->span) && !lang_span_is_dynamic(site) &&
        !e->toplevel_dynamic) return cond;
    /* r7rs-lang-plan R2: a Scheme file decides by Scheme's rule -- only `#f`
     * is false -- and the two rules disagree on exactly one value, `nil`.
     * Same three-way test as the dynamic gate above, for the same
     * macro-expansion reason. */
    bool scheme = lang_span_is_scheme(cond->span) || lang_span_is_scheme(site) ||
                  e->toplevel_scheme;
    const char *opn = scheme ? SCHEME_TRUTHY_OP : SAFFRON_TRUTHY_OP;
    Expr **targs = (Expr **)arena_alloc(e->arena, sizeof(Expr *));
    targs[0] = cond;
    Expr *t = expr_new(e->arena, EX_DYN_OP, TYPE_BOOL, cond->span);
    t->as.dyn_op_.op     = symtab_intern(e->st, strslice(opn, (uint32_t)strlen(opn)));
    t->as.dyn_op_.args   = targs;
    t->as.dyn_op_.n_args = 1;
    return elab_hoist_control_operands(e, t);
}

/* True when `f` is a call whose head names a return-only-dispatch typeclass
 * method with no shadowing binding (e.g. `(pure x)`, `(empty)`) -- a method
 * whose instance can only be selected from an expected result type.  Used by
 * elab_if to let a concrete sibling arm supply that type. */
static bool if_form_is_return_dispatch(Elab *e, const Form *f) {
    if (!f || f->tag != F_LIST || f->as.list.len < 1) return false;
    const Form *head = f->as.list.items[0];
    if (head->tag != F_SYM) return false;
    return elab_symbol_is_return_dispatch_method(e, head->as.sym);
}

Expr *elab_if(Elab *e, const Form *call) {
    if (call->as.list.len != 3 && call->as.list.len != 4) {
        diag_emit(DIAG_ERROR, call->span,
                  "if expects (if cond then) or (if cond then else); got %u argument(s)",
                  call->as.list.len - 1);
        return NULL;
    }

    /* TY3: flow-sensitive narrowing.  Detect a type-test guard on the raw
     * condition Form *before* elaborating it.  Two effects:
     *   1. The `(= (type-of x) "T")` shape is rewritten to `(is? x T)` so the
     *      condition elaborates to a tag comparison (plain `=` has no cstr
     *      overload).  The `(is? x T)` shape is already in that form.
     *   2. When x is `any`-typed, the then-branch is wrapped in
     *      (let [x (cast x T)] ...) so a use of x at type T type-checks
     *      without an explicit cast; the TY2 checked cast verifies the runtime
     *      tag on entry to the branch.
     * (TY3.3: only the direct then-branch on an `any` variable narrows; the
     * else-complement is left to a future phase -- the `any` complement is not
     * a single type, and union variables already narrow via `match`.) */
    Form *cond_form = call->as.list.items[1];
    Form *then_form = call->as.list.items[2];
    Form *else_form = (call->as.list.len == 4) ? call->as.list.items[3] : NULL;
    {
        const Symbol *gv = NULL; Form *gt = NULL;
        if (if_guard_narrowing(e, cond_form, &gv, &gt)) {
            /* Rewrite the condition to the canonical (is? x T) test form. */
            Form *is_items[3] = { form_sym(e->arena, call->span, e->sym_is_q),
                                  form_sym(e->arena, call->span, gv),
                                  gt };
            cond_form = form_list(e->arena, call->span, is_items, 3);
            Binding *vb = scope_lookup(e->scope, gv);
            if (vb && vb->type.kind == TY_ANY) {
                then_form = if_narrow_branch(e, then_form, gv, gt, call->span);
            }
        }
    }

    Expr *cond = elab_form(e, cond_form);
    if (!cond) return NULL;
    /* saffron-lang-plan S3/D4: truthiness.  A Saffron `if` accepts a dynamic
     * condition and decides it at runtime, where Turmeric requires a `bool`.
     *
     * The rule D4 settles: **`false` and `nil` are falsy; `0`, `""` and the
     * empty container are truthy.**  That is the Lisp/Clojure convention rather
     * than the C one, and it is the right default here for a specific reason --
     * Turmeric's `if` already demands a bool, so there is no legacy
     * int-truthiness to stay compatible with, and the C rule would silently
     * turn `(if (vec-len v) ...)` into a bug on an empty vector.
     *
     * Lowered as a dynamic operator rather than a second node kind: it is
     * exactly what EX_DYN_OP is for, a decision the value's own tag makes at
     * runtime.  The reserved name is not a builtin, so the interpreter answers
     * it before consulting the builtin table. */
    cond = elab_saffron_truthy(e, cond, call->span);
    /* r7rs-lang-plan R2: in a Scheme file a condition of any STATIC type is
     * legal, and its truth is known at compile time -- only `#f` is false, so
     * a statically-int (or -string, or -Cons) condition is always true, and a
     * statically-nil one never is.  Saffron's local inference keeps a literal
     * concrete (D3), which is why `(let ((x 5)) (if x ...))` reaches here with
     * an int; the condition still runs, for its effects. */
    if (!type_eq(cond->type, TYPE_BOOL) &&
        (lang_span_is_scheme(cond->span) || lang_span_is_scheme(call->span) ||
         e->toplevel_scheme)) {
        Expr *lit = expr_new(e->arena, EX_BOOL_LIT, TYPE_BOOL, cond->span);
        lit->as.b = (cond->type.kind != TY_NIL);
        Expr **seq = (Expr **)arena_alloc(e->arena, 2 * sizeof(Expr *));
        seq[0] = cond;
        seq[1] = lit;
        Expr *d = expr_new(e->arena, EX_DO, TYPE_BOOL, cond->span);
        d->as.do_.items = seq;
        d->as.do_.n = 2;
        d->as.do_.tail_drop_hoist = false;
        cond = d;
    }
    if (!type_eq(cond->type, TYPE_BOOL)) {
        diag_emit(DIAG_ERROR, cond->span,
                  "if condition must be bool, got %s", type_name(cond->type));
        return NULL;
    }

    /* return-directed-methods-pure-empty-inference (fix direction #2): with no
     * outer expected type and exactly one branch a return-directed method call
     * (its typeclass instance is selectable only from an expected result type,
     * e.g. `(pure x)` / `(empty)`), probe the concrete sibling branch to
     * discover the join type and thread it as the expected type for both arms.
     * Without this `(if c (pure 1) known-opt)` cannot ground `pure` even though
     * the sibling arm pins the container.  The probe runs under a diag capture
     * frame with move/linear state snapshot+restore, so a failed or
     * side-effecting probe leaves no trace. */
    Type *saved_if_expected = e->expected_type;
    Type sibling_ty = TYPE_UNKNOWN;
    bool have_sibling_ty = false;
    if (!e->expected_type && else_form) {
        bool then_rd = if_form_is_return_dispatch(e, then_form);
        bool else_rd = if_form_is_return_dispatch(e, else_form);
        Form *probe = NULL;
        if (then_rd && !else_rd)      probe = else_form;
        else if (else_rd && !then_rd) probe = then_form;
        if (probe) {
            Binding **pmb = NULL; bool *pbs = NULL;
            uint32_t pnmb = move_state_snapshot_bindings(e->scope, &pmb, &pbs);
            Binding **plb = NULL; bool *plbf = NULL;
            uint32_t pnl = linear_state_snapshot_bindings(e->scope, &plb, &plbf);
            diag_push_capture();
            Expr *pe = elab_form(e, probe);
            uint32_t perr = diag_pop_capture();
            move_state_restore(pmb, pbs, pnmb);
            linear_state_restore(plb, plbf, pnl);
            free(pmb); free(pbs); free(plb); free(plbf);
            if (pe && perr == 0 && pe->type.kind != TY_UNKNOWN &&
                pe->type.kind != TY_NEVER && pe->type.kind != TY_NIL) {
                sibling_ty = pe->type;
                have_sibling_ty = true;
            }
        }
    }

    Binding **move_bindings = NULL;
    bool *before_states = NULL;
    uint32_t n_move_bindings = move_state_snapshot_bindings(e->scope, &move_bindings, &before_states);

    /* LT1: Snapshot linear consumption state before branches. */
    Binding **lin_bindings = NULL;
    bool *lin_before = NULL;
    uint32_t n_lin = 0;
    n_lin = linear_state_snapshot_bindings(e->scope, &lin_bindings, &lin_before);

    if (have_sibling_ty) e->expected_type = &sibling_ty;
    Expr *then_ = elab_form(e, then_form);
    if (have_sibling_ty) e->expected_type = saved_if_expected;
    if (!then_) {
        free(move_bindings);
        free(before_states);
        free(lin_bindings);
        free(lin_before);
        return NULL;
    }
    bool *then_states = move_state_capture_current(move_bindings, n_move_bindings);

    /* LT1: Capture linear consumption state after then-branch. */
    bool *lin_then = NULL;
    if (n_lin > 0) {
        lin_then = linear_state_capture_current(lin_bindings, n_lin);
    }

    /* Rewind to pre-branch move-state before elaborating else branch. */
    move_state_restore(move_bindings, before_states, n_move_bindings);

    /* LT1: Rewind linear consumption state before elaborating else branch. */
    if (n_lin > 0) {
        linear_state_restore(lin_bindings, lin_before, n_lin);
    }

    Expr *else_ = NULL;
    Type result_t = TYPE_NIL;
    if (call->as.list.len == 4) {
        if (have_sibling_ty) e->expected_type = &sibling_ty;
        else_ = elab_form(e, else_form);
        if (have_sibling_ty) e->expected_type = saved_if_expected;
        if (!else_) {
            move_state_restore(move_bindings, before_states, n_move_bindings);
            free(then_states);
            free(move_bindings);
            free(before_states);
            free(lin_then);
            free(lin_bindings);
            free(lin_before);
            return NULL;
        }
        bool *else_states = move_state_capture_current(move_bindings, n_move_bindings);

        /* A move is guaranteed after if/else only if it was present before,
         * or both branches moved the binding. */
        for (uint32_t i = 0; i < n_move_bindings; i++) {
            move_bindings[i]->is_moved = before_states[i] || (then_states[i] && else_states[i]);
        }
        free(else_states);

        /* A branch that diverges — its top-level expression is a return,
         * throw, panic, panic-with, or has `!` (TYPE_NEVER) type — is
         * compatible with any other branch.  The if's result type is then
         * the non-diverging branch's type.  This lets `(? expr)` lower to
         * `(if cond (return ...) ok-val)` even when the two branch types
         * differ. */
        bool then_div = (then_->type.kind == TY_NEVER) ||
                        (then_->kind == EX_RETURN) ||
                        (then_->kind == EX_PANIC)  ||
                        (then_->kind == EX_PANIC_WITH);
        bool else_div = (else_->type.kind == TY_NEVER) ||
                        (else_->kind == EX_RETURN) ||
                        (else_->kind == EX_PANIC)  ||
                        (else_->kind == EX_PANIC_WITH);

        /* LT1: Capture linear state after else, check for branch mismatch, merge. */
        if (n_lin > 0) {
            bool *lin_else = linear_state_capture_current(lin_bindings, n_lin);
            bool lin_ok = true;
            for (uint32_t i = 0; i < n_lin; i++) {
                if (lin_before[i]) continue; /* already consumed before if; fine */
                /* Skip mismatch check for diverging branches. */
                if (!then_div && !else_div && lin_then[i] != lin_else[i]) {
                    diag_emit_with_code(DIAG_ERROR, call->span,
                                        TUR_E0104_LINEAR_BRANCH_MISMATCH,
                                        "linear value '%s' consumed in one branch but not the other"
                                        " -- consume it in both branches or neither",
                                        lin_bindings[i]->name->name);
                    lin_ok = false;
                }
                /* Merge: use surviving branch's consumed state. */
                bool merged;
                if (then_div && else_div) {
                    merged = false; /* both diverge; treat as unconsumed */
                } else if (then_div) {
                    merged = lin_else[i];
                } else if (else_div) {
                    merged = lin_then[i];
                } else {
                    merged = lin_then[i] && lin_else[i];
                }
                lin_bindings[i]->is_linear_consumed = merged;
            }
            free(lin_else);
            if (!lin_ok) {
                free(then_states);
                free(move_bindings);
                free(before_states);
                free(lin_then);
                free(lin_bindings);
                free(lin_before);
                return NULL;
            }
        }

        if (then_div && else_div) {
            result_t = then_->type;  /* both diverge; pick either */
        } else if (then_div) {
            result_t = else_->type;
        } else if (else_div) {
            result_t = then_->type;
        } else if (then_->type.kind == TY_ANY || else_->type.kind == TY_ANY ||
                   (e->expected_type && e->expected_type->kind == TY_ANY &&
                    !type_eq(then_->type, else_->type))) {
            /* TY2.2: branch widening to `any`.  When one branch is `any`, box
             * the other (a narrower subtype) so both arms share the tagged
             * representation and the if yields `any`.
             *
             * any-coercion-not-driven-by-expected-type: an `any` EXPECTATION
             * does the same.  Only a branch already carrying `any` used to
             * trigger this, so `(defn f [b] : any (if b (Some 7.1) 42))` fell
             * through to the mismatch diagnostic below -- reported as a type
             * error when both arms would widen to the very type the signature
             * declares.  The union guide describes widening as happening at "an
             * `if` branch facing an `any` sibling", and that clause was doing
             * all the work; the declared return is just as good a target.
             *
             * Gated on the arms actually DISAGREEING: when they already share a
             * type there is nothing to reconcile here, and the return-position
             * widen boxes the result once at the tail rather than once per arm
             * -- so this stays inert for programs that were already fine. */
            then_ = elab_coerce_to_any(e, then_);
            else_ = elab_coerce_to_any(e, else_);
            result_t = then_->type;
        } else if (!type_eq(then_->type, else_->type) &&
                   ((then_->type.kind == TY_PTR_VOID &&
                     expr_is_poly_carrier_fn_var(then_) &&
                     else_->type.kind == TY_FN &&
                     (else_->type.as.fn.boxed ||
                      fn_param_type_is_fat_normalized(&else_->type))) ||
                    (else_->type.kind == TY_PTR_VOID &&
                     expr_is_poly_carrier_fn_var(else_) &&
                     then_->type.kind == TY_FN &&
                     (then_->type.as.fn.boxed ||
                      fn_param_type_is_fat_normalized(&then_->type))))) {
            /* fn-value-carrier-fat-seam-residuals (cell 2): one arm is a
             * CARRIER fn param (a by-value tur_poly_fn_t, spelled ptr<void>),
             * the other a fat-normalized fn result of the same family --
             * e.g. `(if (= n 0) v (f3 (- n 1) v))` where the recursion's
             * result is the stage-2 boxed fn type.  The values are
             * convertible (the poly-to-fat bridge exists); a reject here is
             * the unifier not knowing that.  Insert the conversion on the
             * carrier arm AT THE JOIN -- context-independent, so the fix
             * holds whether or not this if sits in a tail the stage-2
             * normalizer would visit -- and adopt the fn type for the if.
             * Scoped to a carrier-param VAR arm against a boxed or
             * fat-normalizable (concrete, effect-free) fn type; tyvar and
             * effect-row'd signatures keep their conventions and still
             * mismatch loudly. */
            bool carrier_is_then = then_->type.kind == TY_PTR_VOID;
            Expr **carm = carrier_is_then ? &then_ : &else_;
            Type ft = carrier_is_then ? else_->type : then_->type;
            ft.as.fn.boxed = true;
            Type *sink = (Type *)arena_alloc(e->arena, sizeof(Type));
            *sink = ft;
            Expr *conv = expr_new(e->arena, EX_POLY_TO_FAT, TYPE_PTR_VOID,
                                  (*carm)->span);
            conv->as.poly_to_fat_.inner = *carm;
            conv->as.poly_to_fat_.sink_fn_type = sink;
            *carm = conv;
            result_t = ft;
        } else if ((expr_is_fat_closure_value(then_) && else_->type.kind == TY_PTR_VOID) ||
                   (expr_is_fat_closure_value(else_) && then_->type.kind == TY_PTR_VOID)) {
            /* vec-get-typed-fat-closure-readback: a fat-closure value (a directly
             * callable ^fat fn binding, representationally a void* box) and a
             * :ptr<void> are the same representation.  This is the natural shape
             * of an SF-fold base case: `(if done sig (loop ... (apply ...)))`
             * where `sig` is a ^fat parameter (TY_FN) and the recursive arm
             * returns the threaded :ptr<void> fat box.  Widen both arms to
             * :ptr<void> so the if elaborates instead of reporting a spurious
             * "then=(fn ...) else=ptr<void>" mismatch; the caller re-types the
             * result with `^fat` (or `::`) to call it again.  Calling a raw
             * :ptr<void> directly stays an error (CRU B-4). */
            result_t = TYPE_PTR_VOID;
        } else if (then_->type.kind == TY_FN && else_->type.kind == TY_FN &&
                   then_->type.as.fn.boxed != else_->type.as.fn.boxed &&
                   type_eq(then_->type, else_->type)) {
            /* if-joins-closure-and-thin-fn: one arm a capturing closure (a fat
             * `{ thunk, env }` box), the other a thin code pointer (a
             * non-capturing lambda, a defn by name).  Same function type, two
             * representations, and the join's C slot holds either -- so the
             * thin one, called through the fat protocol, read its code as an
             * env and crashed (`((if c (lambda () c) (lambda () 0)))` in
             * `#lang r7rs`, SRFI 48's format with a port).  Box the thin arm
             * (elab_fn_value_to_fat, a static box) and adopt the fat type. */
            then_ = elab_fn_value_to_fat(e, then_);
            else_ = elab_fn_value_to_fat(e, else_);
            result_t = then_->type.as.fn.boxed ? then_->type : else_->type;
        } else if (!type_eq(then_->type, else_->type) &&
                   (lang_span_is_dynamic(call->span) || e->toplevel_dynamic) &&
                   !if_branches_unify_via_tyvar(then_->type, else_->type, &result_t)) {
            /* saffron-dynamic-surface-pass (low): in a Saffron file an `if`
             * whose arms disagree JOINS TO `any` -- `(defn pick [c] (if c 1
             * "one"))` is the ordinary dynamic-language shape, and it was a
             * static "then=int else=cstr" because the widen above fires only
             * when an arm or the expectation already IS `any`; an unannotated
             * Saffron defn pins its return to `any` after the body is
             * elaborated, so the expectation was not there to see.  Placed
             * after every other reconciliation (the fn-carrier seams, the
             * tyvar unify) so those keep their typed answers, and keyed on
             * the dialect the same way the truthiness rule is (the form's own
             * span, or the top-level form's dialect when a macro hides it).
             * Typed Turmeric keeps the diagnostic below. */
            then_ = elab_coerce_to_any(e, then_);
            else_ = elab_coerce_to_any(e, else_);
            result_t = then_->type;
        } else if (!type_eq(then_->type, else_->type)
                   && !if_branches_unify_via_tyvar(then_->type, else_->type, &result_t)) {
            free(then_states);
            free(move_bindings);
            free(before_states);
            free(lin_then);
            free(lin_bindings);
            free(lin_before);
            /* type_name() heap-allocates (strdup) for composite kinds (fn,
             * handler, union, ...) and no one frees it -- a LeakSanitizer-visible
             * leak on every if-branch mismatch involving a function type. Print
             * into owned buffers we free here, matching the leak-clean pattern in
             * elab_call.c's arg-mismatch diagnostic. */
            Buf then_buf; buf_init(&then_buf);
            type_print(&then_buf, then_->type);
            buf_putc(&then_buf, '\0');
            Buf else_buf; buf_init(&else_buf);
            type_print(&else_buf, else_->type);
            buf_putc(&else_buf, '\0');
            diag_emit(DIAG_ERROR, call->span,
                      "if branches have mismatched types: then=%s else=%s",
                      then_buf.data, else_buf.data);
            buf_free(&then_buf);
            buf_free(&else_buf);
            return NULL;
        } else {
            result_t = then_->type;
            /* UT2 (U3): phi downgrade at an if/match join.  The join of two
             * arms that agree on type but disagree on uniqueness is *shared*:
             * unique ∧ unique → unique, but unique ∧ shared → shared (a safe
             * weakening -- the result may be aliased through the shared arm, so
             * it cannot be treated as uniquely owned).  Adopt the non-unique
             * arm's copy_kind so a downstream let does not infer uniqueness
             * (UT2).  The user can re-annotate `^unique` to force the stricter
             * discipline. */
            {
                bool then_uniq = ty_is_unique(then_->type);
                bool else_uniq = ty_is_unique(else_->type);
                if (then_uniq && !else_uniq)
                    result_t.copy_kind = else_->type.copy_kind;
                else if (!then_uniq && else_uniq)
                    result_t.copy_kind = then_->type.copy_kind;
            }
        }
    } else {
        /* Without else, then-branch moves are not guaranteed after the if. */
        move_state_restore(move_bindings, before_states, n_move_bindings);
        /* LT1: Without else, then-branch linear consumption is not guaranteed;
         * restore to the pre-if state so scope-exit checking fires if needed. */
        if (n_lin > 0) {
            linear_state_restore(lin_bindings, lin_before, n_lin);
        }
    }

    free(then_states);
    free(move_bindings);
    free(before_states);
    free(lin_then);
    free(lin_bindings);
    free(lin_before);

    /* If no else, the if is a statement-style branch with type nil
     * (matches Clojure's behavior of returning nil for a missing else). */
    Expr *out = expr_new(e->arena, EX_IF, result_t, call->span);
    out->as.if_.cond = cond;
    out->as.if_.then_ = then_;
    out->as.if_.else_or_null = else_;
    return out;
}

/* Phase 6: Threading macro ->  */
/* (-> x (f a) (g b)) expands to (g (f x a) b) */
Expr *elab_thread(Elab *e, const Form *call) {
    if (call->as.list.len < 2) {
        diag_emit(DIAG_ERROR, call->span,
                  "-> requires at least one argument");
        return NULL;
    }
    
    /* Start with the initial value as a Form */
    Form *current = call->as.list.items[1];
    
    /* Process remaining forms */
    for (uint32_t i = 2; i < call->as.list.len; i++) {
        Form *form = call->as.list.items[i];
        if (form->tag == F_SYM) {
            /* (-> x f) -> (f x) */
            current = form_list(e->arena, call->span,
                (Form *[]){form, current}, 2);
        } else if (form->tag == F_LIST) {
            /* (-> x (f a b)) -> (f x a b) */
            /* Prepend current to the list arguments */
            uint32_t n = form->as.list.len;
            Form **new_items = (Form **)arena_alloc(e->arena, (n + 1) * sizeof(Form *));
            new_items[0] = form->as.list.items[0]; /* function name */
            new_items[1] = current; /* insert current as first arg */
            for (uint32_t j = 1; j < n; j++) {
                new_items[j + 1] = form->as.list.items[j];
            }
            current = form_list(e->arena, call->span, new_items, n + 1);
        } else {
            diag_emit(DIAG_ERROR, form->span,
                      "-> expected symbol or list, got %s",
                      form->tag == F_VEC ? "vector" : "other");
            return NULL;
        }
    }
    
    /* Elaborate the final form */
    return elab_form(e, current);
}

/* Phase 6: Threading macro ->>  */
/* (->> x (f a) (g b)) expands to (g b (f x a)) - value as last arg */
Expr *elab_thread_last(Elab *e, const Form *call) {
    if (call->as.list.len < 2) {
        diag_emit(DIAG_ERROR, call->span,
                  "->> requires at least one argument");
        return NULL;
    }
    
    /* Start with the initial value as a Form */
    Form *current = call->as.list.items[1];
    
    /* Process remaining forms */
    for (uint32_t i = 2; i < call->as.list.len; i++) {
        Form *form = call->as.list.items[i];
        if (form->tag == F_SYM) {
            /* (->> x f) -> (f x) - same as -> for single arg */
            current = form_list(e->arena, call->span,
                (Form *[]){form, current}, 2);
        } else if (form->tag == F_LIST) {
            /* (->> x (f a b)) -> (f a b x) - append current as last arg */
            uint32_t n = form->as.list.len;
            Form **new_items = (Form **)arena_alloc(e->arena, (n + 1) * sizeof(Form *));
            for (uint32_t j = 0; j < n; j++) {
                new_items[j] = form->as.list.items[j];
            }
            new_items[n] = current; /* append current as last arg */
            current = form_list(e->arena, call->span, new_items, n + 1);
        } else {
            diag_emit(DIAG_ERROR, form->span,
                      "->> expected symbol or list, got %s",
                      form->tag == F_VEC ? "vector" : "other");
            return NULL;
        }
    }
    
    /* Elaborate the final form */
    return elab_form(e, current);
}

/* Phase DS3: (set! (.field s) v) - struct field write.
 *
 * Accepts two receiver shapes:
 *   - TY_STRUCT (direct struct value): the receiver binding must be `^mut`.
 *   - TY_RC where struct_def is set (rc<Struct>): no `^mut` required
 *     (interior mutability through the rc wrapper).
 *
 * For rc-typed fields, the prior value's strong count is decremented before
 * the new pointer is stored (the new value already carries its own +1).
 * For move-only non-rc values the source binding is marked moved. */
static Expr *elab_set_field(Elab *e, const Form *call, Form *target) {
    /* target shape: (.field receiver-form) */
    Form *head_form = target->as.list.items[0];
    const char *fname = head_form->as.sym->name + 1; /* skip leading '.' */

    Expr *receiver = elab_form(e, target->as.list.items[1]);
    if (!receiver) return NULL;

    /* Resolve the receiver's record ADT -- directly or through the rc wrapper.
     * CONV-S1 seam 4 / structdef-retirement DS-C: a defstruct receiver is a
     * single-variant record ADT, never a StructDef. */
    const AdtDef *adt = NULL;
    const CtorDef *adt_ctor = NULL;
    bool receiver_is_rc = false;
    /* end-to-end-monomorphization: a :heap receiver is a typed pointer to a
     * shared heap header -- mutation through it is interior (like rc), so no
     * `^mut` on the local binding is required, and the field write derefs. */
    bool receiver_is_heap = type_is_heap_struct(receiver->type) ||
                            type_is_heap_adt(receiver->type);
    Type rt = receiver->type;
    /* structdef-retirement DS-C: the TY_STRUCT receiver arm, the TY_APP
     * struct_defs scan, and the rc<Struct> (rc.struct_def) arm are all dead --
     * a struct receiver is a record ADT (or an ADT app / rc<ADT>) now.  `def`
     * (StructDef*) stays NULL; every receiver resolves through `adt`. */
    if (rt.kind == TY_ADT && rt.as.adt_.def) {
        adt = rt.as.adt_.def;
    } else if (rt.kind == TY_APP) {
        /* CONV-S1 seam 4: a lowered parametric record struct is a record ADT app
         * (`(Box int)`); resolve its base AdtDef. */
        const Type *adt_base = &rt;
        while (adt_base && adt_base->kind == TY_APP && adt_base->as.app.fn)
            adt_base = adt_base->as.app.fn;
        if (adt_base && adt_base->kind == TY_ADT && adt_base->as.adt_.def)
            adt = adt_base->as.adt_.def;
    } else if (rt.kind == TY_RC && rt.as.rc.adt_def) {
        /* CONV-S1 seam 4: rc<Name> where Name is a lowered single-variant
         * record ADT (a defstruct-as-defadt struct).  Resolve the field through
         * the rc wrapper's adt_def mirror, the by-value analog of the
         * struct_def branch above. */
        adt = rt.as.rc.adt_def;
        receiver_is_rc = true;
    } else if (rt.kind == TY_REF_MUT) {
        /* &mut Struct -- field write through a mutable borrow is not yet
         * supported (structdef-retirement DS-C: the dead TY_STRUCT probe here is
         * removed). */
        diag_emit(DIAG_ERROR, target->span,
                  "set! through &mut Struct is not yet supported -- "
                  "use a ^mut struct binding or rc<Struct> instead");
        return NULL;
    } else {
        diag_emit(DIAG_ERROR, target->span,
                  "set! (.field s): receiver must be a struct or rc<Struct>, got %s",
                  type_name(rt));
        return NULL;
    }

    /* CONV-S1 seam 4: a lowered receiver resolved to a record ADT, not a struct.
     * It must be a single-variant record product (the shape a defstruct lowers
     * to); find the field in its sole record constructor. */
    if (adt) {
        if (!(adt->n_ctors == 1 && adt->ctors[0]->is_record)) {
            diag_emit(DIAG_ERROR, target->span,
                      "set! (.field s): receiver must be a struct or rc<Struct>, got %s",
                      type_name(rt));
            return NULL;
        }
        adt_ctor = adt->ctors[0];
    } else {
        diag_emit(DIAG_ERROR, target->span,
                  "set! (.field s): receiver must be a struct or rc<Struct>, got %s",
                  type_name(rt));
        return NULL;
    }

    /* Find the field by name on the lowered record-ADT constructor. */
    uint32_t fi = 0;
    uint32_t n_fields = adt_ctor->n_fields;
    for (; fi < n_fields; fi++) {
        const char *fn = adt_ctor->fields[fi].name;
        if (fn && strcmp(fn, fname) == 0) break;
    }
    if (fi >= n_fields) {
        diag_emit(DIAG_ERROR, head_form->span,
                  "set! (.%s s): type '%s' has no field '%s'",
                  fname, adt->name, fname);
        return NULL;
    }

    /* Direct-struct receivers require ^mut on the binding.  Through-rc and
     * through a :heap typed pointer are interior-mutable so any binding is fine. */
    if (!receiver_is_rc && !receiver_is_heap && receiver->kind == EX_VAR) {
        Binding *rb = receiver->as.var.binding;
        if (rb->is_moved) {
            diag_emit_with_code(DIAG_ERROR, target->span, TUR_E0005_USE_AFTER_MOVE,
                                "use-after-move: cannot set! field of '%s' because it was moved",
                                rb->name->name);
            return NULL;
        }
        if (!rb->is_mut) {
            diag_emit(DIAG_ERROR, target->span,
                      "set! (.%s ...): '%s' is immutable; use ^mut at the binding site",
                      fname, rb->name->name);
            return NULL;
        }
    }

    /* Record-ADT field type, with type-arg substitution for a parametric
     * receiver (`(Box int)` -> field A becomes int), mirroring the read side
     * (elab_typeclasses.c get-field).  structdef-retirement DS-C: the former
     * TY_STRUCT `def` path (elab_struct_field_use_type) is dead. */
    Type expected_field;
    {
        const CtorField *cf = &adt_ctor->fields[fi];
        expected_field = cf->full_type ? *cf->full_type
                                       : type_from_kind(cf->kind);
        if (cf->full_type && adt->n_type_params > 0 && rt.kind == TY_APP) {
            Type *type_args = (Type *)arena_alloc(e->arena,
                                  adt->n_type_params * sizeof(Type));
            if (elab_adt_type_extract_args(&rt, adt, type_args))
                expected_field = adt_field_instantiate_type(e, adt,
                                     cf->full_type, type_args);
        }
    }
    /* Elaborate the value and type-check against the field.  r7rs-lang-plan
     * R3 (a Saffron gap too): an `any` field takes any value, so a concrete
     * one is widened through `(:: v any)` in a dynamic file, exactly as a
     * `set!` into an `any` cell is. */
    Form *value_form = call->as.list.items[2];
    if (expected_field.kind == TY_ANY &&
        (lang_span_is_dynamic(call->span) || e->toplevel_dynamic)) {
        Form **asc = (Form **)arena_alloc(e->arena, 3 * sizeof(Form *));
        asc[0] = form_sym(e->arena, value_form->span, e->sym_ascribe);
        asc[1] = value_form;
        asc[2] = form_sym(e->arena, value_form->span,
                          symtab_intern(e->st, strslice("any", 3)));
        value_form = form_list(e->arena, value_form->span, asc, 3);
    }
    Expr *value = elab_form(e, value_form);
    if (!value) return NULL;
    if (value->type.kind != TY_PTR_VOID && !type_eq(value->type, expected_field)) {
        diag_emit(DIAG_ERROR, value->span,
                  "set! (.%s ...): value type %s does not match field type %s",
                  fname, type_name(value->type), type_name(expected_field));
        return NULL;
    }

    /* Move-at-set for rc-managed payloads, mirroring the DS2 scan
     * in elab_make_struct. */
    if (value->kind == EX_VAR && value->as.var.binding) {
        TypeKind vk = value->type.kind;
        if (vk == TY_RC || vk == TY_WEAK || vk == TY_EXISTS) {
            (void)binding_mark_moved(value->as.var.binding, value->span);
        }
    }

    /* set-bang-rc-release: an rc field write RELEASES the field's previous
     * pointer (see emit_set_field_stmt), so the incoming value has to carry its
     * own +1 or the field ends up owning a reference nobody took.  The move-at-set
     * above covers a bare variable, and `(rc/of ...)` / `(rc/clone ...)` carry one
     * by construction -- but a bare rc FIELD READ carries nothing:
     * `(set! (.next a) (.next b))` copied the word straight across, leaving
     * `a.next` and `b.next` aliasing one block that BOTH would later release.
     * Wrap it, exactly as the let-init and `(set! var ...)` paths do for the same
     * borrow shape. */
    if (expected_field.kind == TY_RC) {
        Expr *inner = value;
        while (inner && inner->kind == EX_ASCRIBE)
            inner = inner->as.ascribe_.inner;
        if (inner && inner->kind == EX_GET_FIELD && inner->type.kind == TY_RC) {
            Expr *clone = expr_new(e->arena, EX_RC_CLONE, value->type, value->span);
            clone->as.rc_clone_.expr = value;
            clone->as.rc_clone_.elide = false;
            value = clone;
        }
    }

    Expr *out = expr_new(e->arena, EX_SET_FIELD, TYPE_NIL, call->span);
    out->as.set_field_.receiver = receiver;
    out->as.set_field_.value = value;
    out->as.set_field_.field_idx = fi;
    out->as.set_field_.adt_def = adt;
    out->as.set_field_.adt_ctor = adt_ctor;
    out->as.set_field_.receiver_is_rc = receiver_is_rc;
    return out;
}

Expr *elab_set(Elab *e, const Form *call) {
    if (call->as.list.len != 3) {
        diag_emit(DIAG_ERROR, call->span, "set! takes (set! name value)");
        return NULL;
    }
    Form *target = call->as.list.items[1];

    /* Phase 12: Handle (set! (@ r) value) - mutation through mutable borrow */
    if (target->tag == F_LIST && target->as.list.len == 2
        && target->as.list.items[0]->tag == F_SYM
        && target->as.list.items[0]->as.sym == e->sym_deref) {
        return elab_set_deref(e, call, target);
    }

    /* Phase DS3: (set! (.field s) v) -- struct field write. */
    if (target->tag == F_LIST && target->as.list.len == 2
        && target->as.list.items[0]->tag == F_SYM
        && target->as.list.items[0]->as.sym->len > 1
        && target->as.list.items[0]->as.sym->name[0] == '.') {
        return elab_set_field(e, call, target);
    }

    if (target->tag != F_SYM) {
        diag_emit(DIAG_ERROR, target->span,
                  "set! target must be a symbol, (@ borrow), or (.field struct)");
        return NULL;
    }
    Binding *b = scope_lookup(e->scope, target->as.sym);
    /* compiled-closure-copies-a-captured-mut: a `^mut` moved into a shared
     * cell is written through the cell's field. */
    if (b && b->cell_hidden_sym) {
        Form *ftarget = elab_mut_cell_read_form(e, b, target->span);
        Form **items = (Form **)arena_alloc(e->arena, 3 * sizeof(Form *));
        items[0] = call->as.list.items[0];
        items[1] = ftarget;
        items[2] = call->as.list.items[2];
        Form *fcall = form_list(e->arena, call->span, items, 3);
        return elab_set_field(e, fcall, ftarget);
    }
    if (!b) {
        diag_emit(DIAG_ERROR, target->span,
                  "set!: '%s' is not bound", target->as.sym->name);
        return NULL;
    }

    /* DV1: If the target is a dynamic var, dispatch to dynvar set path */
    if (b->is_dynvar && b->dynvar_entry) {
        DynVarEntry *entry = b->dynvar_entry;

        /* TUR-E0605: set! on dynvar inside atomically is disallowed */
        if (elab_in_atomically) {
            diag_emit_with_code(DIAG_ERROR, target->span, TUR_E0605_DYNVAR_SET_IN_ATOMIC,
                                "set!: cannot mutate dynamic var '%s' inside an "
                                "atomically block (mutation cannot be rolled back on retry)",
                                entry->name->name);
            return NULL;
        }

        /* TUR-E0601: no active binding frame for this var */
        bool frame_active = false;
        for (uint32_t i = 0; i < e->n_active_dynvar_bindings; i++) {
            if (e->active_dynvar_bindings[i] == entry->name) {
                frame_active = true;
                break;
            }
        }
        if (!frame_active) {
            diag_emit_with_code(DIAG_ERROR, target->span, TUR_E0601_DYNVAR_SET_NO_BINDING,
                                "set!: '%s' has no active binding frame on this thread; "
                                "wrap in (binding [%s ...] ...) first",
                                entry->name->name, entry->name->name);
            return NULL;
        }

        Expr *value = elab_form(e, call->as.list.items[2]);
        if (!value) return NULL;

        /* TUR-E0602: type must match declared value type */
        if (value->type.kind != entry->value_type.kind) {
            diag_emit_with_code(DIAG_ERROR, value->span, TUR_E0602_DYNVAR_TYPE_MISMATCH,
                                "set!: '%s' declared as '%s' but value has type '%s'",
                                entry->name->name, type_name(entry->value_type),
                                type_name(value->type));
            return NULL;
        }

        Expr *out = expr_new(e->arena, EX_DYNVAR_SET, TYPE_NIL, call->span);
        out->as.dynvar_set_.entry = entry;
        out->as.dynvar_set_.value = value;
        return out;
    }

    /* Phase 11: Check if target binding has been moved */
    if (b->is_moved) {
        diag_emit_with_code(DIAG_ERROR, target->span, TUR_E0005_USE_AFTER_MOVE,
                            "use-after-move: cannot set! '%s' because it was moved",
                            b->name->name);
        return NULL;
    }
    if (!b->is_mut) {
        diag_emit(DIAG_ERROR, target->span,
                  "set!: '%s' is immutable; use ^mut at the binding site to allow it",
                  b->name->name);
        return NULL;
    }
    /* G3 (mutable-globals-plan §4.3): an exported global is READ-ONLY outside
     * its defining module.  A module that exports a counter for reading should
     * not thereby export it for writing -- the same argument `:sealed` makes
     * about an opaque's representation.
     *
     * Only bites across a real module boundary: a global with no defining
     * module (every single-file program) and a write from inside the owning
     * module are both untouched.  The permission lives at the definition site,
     * `(export (mut g))`, so the decision sits with the code that owns the
     * invariant rather than with whoever wants to write it. */
    if (b->is_global && !b->is_export_mut &&
        b->defining_module_name != NULL &&
        b->defining_module_name != e->current_module_name) {
        diag_emit(DIAG_ERROR, target->span,
                  "set!: '%s' is owned by module '%s' and is exported read-only; "
                  "call a setter that module exports, or have it export the global "
                  "as `(export (mut %s))`",
                  b->name->name, b->defining_module_name->name, b->name->name);
        return NULL;
    }
    /* r7rs-lang-plan R2 (a Saffron gap too): a `^mut x : any` cell takes any
     * value, so a concrete one is widened through the same `(:: v any)`
     * ascription a call argument gets, instead of "value type cstr does not
     * match binding type any".  Only in a dynamic file, and only into an
     * `any` cell -- a typed cell keeps the exact check below. */
    Form *value_form = call->as.list.items[2];
    if (b->type.kind == TY_ANY &&
        (lang_span_is_dynamic(call->span) || e->toplevel_dynamic)) {
        Form **asc = (Form **)arena_alloc(e->arena, 3 * sizeof(Form *));
        asc[0] = form_sym(e->arena, value_form->span, e->sym_ascribe);
        asc[1] = value_form;
        asc[2] = form_sym(e->arena, value_form->span,
                          symtab_intern(e->st, strslice("any", 3)));
        value_form = form_list(e->arena, value_form->span, asc, 3);
    }
    Expr *value = elab_form(e, value_form);
    if (!value) return NULL;
    /* let-bound-generic-call-result-in-generic-truncates: a `^mut` cell whose
     * init the `let` retyped to the enclosing signature's own `A` takes a later
     * generic-call value through the same bridge, or `(set! y (gid x))` would
     * compare the call's carrier `int` against the cell's `A` and refuse it. */
    if (b->type.kind == TY_TYVAR)
        value = let_bridge_sig_tyvar_result(e, value);
    if (!type_eq(value->type, b->type)) {
        diag_emit(DIAG_ERROR, value->span,
                  "set!: value type %s does not match binding type %s",
                  type_name(value->type), type_name(b->type));
        return NULL;
    }
    /* fn-cell-set-with-capturing-closure-segfaults: `type_eq` does not see the
     * representation.  A fat cell (a `^mut` fn cell is one, see the `let`
     * path) takes a thin value through the same shim its init took; a cell
     * that stayed thin -- initialised from a thin fn VALUE the `let` could
     * not re-spell, such as a parameter -- cannot hold a capturing closure,
     * and the store is refused here rather than segfaulting at the call. */
    if (b->is_poly_fn && value->type.kind == TY_FN) {
        /* let-alias-of-fn-param-call-undeclared: a cell that ALIASES a
         * polymorphic fn parameter is a `tur_poly_fn_t` value -- a
         * representation neither a lambda literal nor a closure box has (the
         * store was `incompatible types when assigning to type
         * 'tur_poly_fn_t'`, thin and capturing alike).  Refuse it statically;
         * a cell meant to be re-pointed is bound from a lambda literal. */
        diag_emit(DIAG_ERROR, value->span,
                  "set!: '%s' aliases a polymorphic function parameter and "
                  "cannot be re-pointed; bind a `^mut` cell from a lambda "
                  "literal instead",
                  b->name->name);
        return NULL;
    }
    if (b->type.kind == TY_FN && value->type.kind == TY_FN) {
        if (b->type.as.fn.boxed && !value->type.as.fn.boxed) {
            value = elab_fn_value_to_fat(e, value);
        } else if (!b->type.as.fn.boxed && value->type.as.fn.boxed) {
            diag_emit(DIAG_ERROR, value->span,
                      "set!: '%s' holds a bare function pointer and cannot take a "
                      "capturing closure; initialise it with a capturing lambda, "
                      "or bind it with `^mut` from a lambda literal so it is a "
                      "closure cell",
                      b->name->name);
            return NULL;
        }
    }

    /* Phase 11: Move tracking - if value is a CK_MOVE binding reference, poison it */
    if (value->kind == EX_VAR && type_is_move(value->as.var.binding->type)) {
        binding_mark_moved(value->as.var.binding, value->span);
    }

    Expr *out = expr_new(e->arena, EX_SET, TYPE_NIL, call->span);
    out->as.set_.target = b;
    out->as.set_.value = value;
    return out;
}

/* loop-invariants-plan: is `f` the `:invariant` keyword? */
static bool while_is_invariant_kw(const Elab *e, const Form *f) {
    return f && f->tag == F_KEYWORD && f->as.sym == e->kw_invariant;
}

/* The source location a loop-invariant panic names, as `file:line:col`.
 * Basename only: the message is program output, and an absolute build path
 * would make it differ between checkouts. */
static const char *while_loc_str(Elab *e, Span sp) {
    Span t = diag_translate_span(sp);
    const char *path = diag_file_path(t.file_id);
    const char *base = path ? path : "<input>";
    for (const char *q = base; *q; q++)
        if (*q == '/' || *q == '\\') base = q + 1;
    char buf[256];
    snprintf(buf, sizeof(buf), "%s:%u:%u", base, (unsigned)t.line,
             (unsigned)t.col_start);
    return arena_strdup(e->arena, buf, strlen(buf));
}

Expr *elab_while(Elab *e, const Form *call) {
    if (call->as.list.len < 2) {
        diag_emit(DIAG_ERROR, call->span, "while requires a condition");
        return NULL;
    }
    Expr *cond = elab_form(e, call->as.list.items[1]);
    if (!cond) return NULL;
    /* saffron-dynamic-surface-pass (low): `while` is the THIRD bool slot a
     * Saffron `any` can reach, after the `if`/`when` condition and the
     * `#refine{...}` predicate.  It had no wrap, so `if/when/and/or` applied
     * truthiness and `while` alone answered "while condition must be bool,
     * got any" -- a loop could not be driven by a dynamic value in a dialect
     * whose every value is one. */
    cond = elab_saffron_truthy(e, cond, call->span);
    if (!type_eq(cond->type, TYPE_BOOL)) {
        diag_emit(DIAG_ERROR, cond->span,
                  "while condition must be bool, got %s", type_name(cond->type));
        return NULL;
    }

    /* loop-invariants-plan LI0: `(while c :invariant p body...)`.  The keyword
     * is accepted only DIRECTLY after the condition and at most once -- the
     * `:pre`/`:post` scan shape, narrowed to one slot.  Before this, a stray
     * keyword in a `while` body was an ordinary (ignored) value statement, so
     * `:invariant p` would have silently evaluated `p` each iteration and
     * checked nothing; rejecting it anywhere but the annotation slot is what
     * keeps a misplaced one from reading as a working invariant.
     *
     * The annotation always PARSES and is always VALIDATED (a pure `bool`); the
     * `loop-invariants` gate withholds only the acting. */
    uint32_t body_start = 2;
    const Form *inv_form = NULL;
    Expr *inv_e = NULL;
    if (call->as.list.len > 2 && while_is_invariant_kw(e, call->as.list.items[2])) {
        if (call->as.list.len < 4) {
            diag_emit(DIAG_ERROR, call->as.list.items[2]->span,
                      "while: `:invariant` must be followed by a predicate");
            return NULL;
        }
        inv_form = call->as.list.items[3];
        body_start = 4;
    }
    for (uint32_t k = body_start; k < call->as.list.len; k++) {
        if (!while_is_invariant_kw(e, call->as.list.items[k])) continue;
        diag_emit(DIAG_ERROR, call->as.list.items[k]->span,
                  inv_form && k == body_start
                      ? "while: at most one `:invariant` per loop -- combine the "
                        "predicates with `and`"
                      : "while: `:invariant` must directly follow the loop "
                        "condition");
        return NULL;
    }
    if (inv_form) {
        inv_e = elab_loop_invariant_pred(e, inv_form, inv_form->span);
        if (!inv_e) return NULL;
    }
    bool li_on = inv_form && g_opt_loop_invariants;
    if (li_on) experiment_warn_if_used("loop-invariants");

    uint32_t n = call->as.list.len - body_start;
    Expr *body;
    if (n == 0) body = e_nil(e, call->span);
    else if (n == 1) {
        body = elab_form(e, call->as.list.items[body_start]);
        if (!body) return NULL;
    } else {
        Expr **items = (Expr **)arena_alloc(e->arena, n * sizeof(Expr *));
        for (uint32_t i = 0; i < n; i++) {
            items[i] = elab_form(e, call->as.list.items[body_start + i]);
            if (!items[i]) return NULL;
        }
        body = expr_new(e->arena, EX_DO, TYPE_NIL, call->span);
        body->as.do_.items = items;
        body->as.do_.n = n;
    }

    /* LI1: the invariant is a contract first.  Checked on entry and as the
     * last statement of every iteration (re-establishment), injected as
     * ordinary Exprs so the compiled and interpreted paths agree for free.
     * A static proof (LI2, decided by the enclosing defn) overwrites the slot
     * with nil -- the same elision a proved `:pre`/`:post` gets. */
    Expr **entry_slot = NULL, **body_slot = NULL;
    Expr *entry_do = NULL;
    if (li_on && rt_contracts_emitted()) {
        const char *loc = while_loc_str(e, call->span);
        char msg[384];
        snprintf(msg, sizeof(msg), "Loop invariant failed on entry to the while "
                 "loop at %s", loc);
        Expr *entry_chk = li_contract_check(
            e, inv_e, arena_strdup(e->arena, msg, strlen(msg)), inv_form->span);
        Expr *body_pred = entry_chk ? elab_loop_invariant_pred(e, inv_form,
                                                               inv_form->span)
                                    : NULL;
        snprintf(msg, sizeof(msg), "Loop invariant not re-established by the "
                 "body of the while loop at %s", loc);
        Expr *body_chk = body_pred ? li_contract_check(
            e, body_pred, arena_strdup(e->arena, msg, strlen(msg)),
            inv_form->span) : NULL;
        if (entry_chk && body_chk) {
            Expr **bi = (Expr **)arena_alloc(e->arena, 2 * sizeof(Expr *));
            bi[0] = body;
            bi[1] = body_chk;
            Expr *nb = expr_new(e->arena, EX_DO, TYPE_NIL, call->span);
            nb->as.do_.items = bi;
            nb->as.do_.n = 2;
            body = nb;
            body_slot = &bi[1];

            Expr **ei = (Expr **)arena_alloc(e->arena, 2 * sizeof(Expr *));
            ei[0] = entry_chk;
            entry_do = expr_new(e->arena, EX_DO, TYPE_NIL, call->span);
            entry_do->as.do_.items = ei;
            entry_do->as.do_.n = 2;
            entry_slot = &ei[0];
        }
    }

    Expr *out = expr_new(e->arena, EX_WHILE, TYPE_NIL, call->span);
    out->as.while_.cond = cond;
    out->as.while_.body = body;
    out->as.while_.invariant = inv_form;
    if (li_on) {
        LoopInvSite *site = li_register_site(e, call, call->as.list.items[1],
                                             inv_form, body_start, call->span);
        if (site) {
            site->entry_check = entry_slot;
            site->body_check  = body_slot;
        }
    }
    if (entry_do) {
        entry_do->as.do_.items[1] = out;
        return entry_do;
    }
    return out;
}

/* case desugars to (let [g disc] (if (= g v1) e1 (if (= g v2) e2 ... eN))).
 * Syntax: (case val v1 e1 v2 e2 ... :else eN) */
Expr *elab_case(Elab *e, const Form *call) {
    if (call->as.list.len < 2) {
        diag_emit(DIAG_ERROR, call->span,
                  "case requires (case val v1 e1 ...), got empty call");
        return NULL;
    }
    uint32_t n = call->as.list.len - 2; /* clause forms after discriminant */
    if ((n & 1) != 0) {
        diag_emit(DIAG_ERROR, call->span,
                  "case expects pairs of (value expr) after discriminant; got %u clause forms", n);
        return NULL;
    }

    /* Generate a gensym for the discriminant to avoid multiple evaluation. */
    char disc_name[32];
    snprintf(disc_name, sizeof(disc_name), "case_disc_%u", e->next_gensym_id++);
    const Symbol *disc_sym = symtab_intern(e->st, strslice(disc_name, (uint32_t)strlen(disc_name)));
    Form *disc_sym_f = form_sym(e->arena, call->span, disc_sym);

    /* Intern = symbol for building (= g vi) tests */
    const Symbol *sym_eq = symtab_intern(e->st, strslice("=", 1));
    Form *sym_eq_f = form_sym(e->arena, call->span, sym_eq);

    /* Intern 'if' and ':else' for building the nested if tree */
    Form *sym_if_f  = form_sym(e->arena, call->span, e->sym_if);

    /* Build the nested if tree right-to-left */
    Form *acc = form_nil(e->arena, call->span);
    for (int i = (int)n - 2; i >= 0; i -= 2) {
        Form *val  = call->as.list.items[2 + i];
        Form *body = call->as.list.items[2 + i + 1];

        /* The fallback clause is spelled `:else` or bare `else` -- one clause,
         * two spellings, matching `cond`.  A bare `else` used to reach the
         * `(= disc else)` test below and die as TUR-E0003 `unbound symbol
         * 'else'`, so no program that compiled before changes meaning. */
        if ((val->tag == F_KEYWORD || val->tag == F_SYM) && val->as.sym == e->kw_else) {
            acc = body;
            continue;
        }

        /* Build (= disc_sym val) */
        Form *eq_items[3] = { sym_eq_f, disc_sym_f, val };
        Form *test_f = form_list(e->arena, val->span, eq_items, 3);

        /* Build (if test body acc) */
        Form *if_items[4] = { sym_if_f, test_f, body, acc };
        acc = form_list(e->arena, val->span, if_items, 4);
    }

    /* Wrap in (let [disc_sym disc_expr] acc) */
    Form *disc_f = call->as.list.items[1];
    Form *bind_vec_items[2] = { disc_sym_f, disc_f };
    Form *bind_vec = form_vec(e->arena, call->span, bind_vec_items, 2);
    Form *sym_let_f = form_sym(e->arena, call->span, e->sym_let);
    Form *let_items[3] = { sym_let_f, bind_vec, acc };
    Form *let_form = form_list(e->arena, call->span, let_items, 3);

    return elab_form(e, let_form);
}

/* Elaborate forms [start..len) of `call` as one implicit body block.
 * A single form elaborates to itself; two or more are wrapped in an EX_DO so
 * every consumer that wants one `Expr *` (capture analysis, the reset/defer
 * lowerings) keeps working unchanged. */
Expr *elab_implicit_do(Elab *e, const Form *call, uint32_t start) {
    uint32_t n = call->as.list.len - start;
    if (n == 0) return e_nil(e, call->span);
    if (n == 1) return elab_form(e, call->as.list.items[start]);
    Expr **items = (Expr **)arena_alloc(e->arena, n * sizeof(Expr *));
    for (uint32_t i = 0; i < n; i++) {
        items[i] = elab_form(e, call->as.list.items[start + i]);
        if (!items[i]) return NULL;
    }
    Expr *out = expr_new(e->arena, EX_DO, items[n - 1]->type, call->span);
    out->as.do_.items = items;
    out->as.do_.n     = n;
    return out;
}

/* Phase 4: defer — (defer body ...)
 * Records a body to be evaluated at scope exit, in LIFO order.
 * For now, only valid inside let/do/defn/while bodies.
 * The body is elaborated but its value is discarded (defer always evaluates to nil).
 * v1 lowering (effects-plan.md §6.10): performs capture analysis for thunk lifting.
 * Nested defers are not yet supported.
 *
 * The body is VARIADIC.  It used to read items[1] and nothing else, so
 * `(defer a b)` compiled clean and silently dropped `b` on both the compiled
 * path and under `--interpret`.  Forms 2..n now join an implicit `do`, which
 * collect_free_vars below walks exactly as it walked a hand-written one.
 */
Expr *elab_defer(Elab *e, const Form *call) {
    if (call->as.list.len < 2) {
        diag_emit(DIAG_ERROR, call->span, "defer requires an expression");
        return NULL;
    }
    /* Phase M5: module-level defer (top-level scope) is allowed.
     * The body runs at process exit via atexit(). No captures — at global
     * scope all referenced names are already global bindings. */
    if (e->scope == &e->global) {
        Expr *body = elab_implicit_do(e, call, 1);
        if (!body) return NULL;
        Expr *out = expr_new(e->arena, EX_DEFER, TYPE_NIL, call->span);
        out->as.defer_.body       = body;
        out->as.defer_.captures   = NULL;
        out->as.defer_.n_captures = 0;
        return out;
    }
    /* Elaborate the body (forms 1..n as one implicit do) */
    Expr *body = elab_implicit_do(e, call, 1);
    if (!body) return NULL;
    
    /* v1 lowering: Collect free variables (captures) for thunk lifting.
     * Per effects-plan.md §6.10.1, defers are entries in a list-on-frame.
     * For defers that reference local variables, we need to capture them
     * in an env struct (same pattern as closures in Phase 3).
     * 
     * For defer thunks (unlike closure thunks), there are no "params" — all
     * non-global bindings referenced in the defer body need to be captured
     * since the thunk is a separate function at file scope.
     * We pass empty params to collect_free_vars so all non-global bindings
     * are treated as free variables (captures). */
    Binding **captures = NULL;
    uint8_t n_captures = 0;
    
    /* Collect free variables in the defer body - pass empty params
     * so all non-global bindings are captured */
    uint32_t n_free = 0;
    Binding **free_vars = collect_free_vars(body, NULL, 0, NULL, 0, &n_free);
    
    if (n_free > 0) {
        /* Store captures in the defer expression (arena-allocated, lives for compilation) */
        captures = (Binding **)arena_alloc(e->arena, n_free * sizeof(Binding *));
        memcpy(captures, free_vars, n_free * sizeof(Binding *));
        n_captures = (uint8_t)n_free;
    }
    
    free(free_vars);
    
    /* Create EX_DEFER expression with capture info */
    Expr *out = expr_new(e->arena, EX_DEFER, TYPE_NIL, call->span);
    out->as.defer_.body = body;
    out->as.defer_.captures = captures;
    out->as.defer_.n_captures = n_captures;
    return out;
}

/* Phase 3/4: return — (return) or (return expr)
 * Early return from a function, firing all defers in the scope chain.
 * 
 * Grammar: (return) or (return expr)
 * The return value type must match the function's return type.
 * 
 * Per effects-plan.md §6.10: "return: walk every enclosing frame, fire defers 
 * per frame, then function-exit." The codegen emits tur_frame_fire_chain to 
 * walk the parent chain and fire all defers before returning.
 */
/* proper-tail-calls T1 (docs/archive/proper-tail-calls-plan.md, T-D1):
 * `(^tailcall <call>)`, or its prefix spelling `^tailcall <call>`.
 *
 * The annotation carries no semantics -- it elaborates to the call itself,
 * unchanged -- it only turns "did this become a tail call?" into a question the
 * compiler answers.  The check lives in the emitter (emit_fns.c), next to the
 * `tco_mark` / `emit_tail` pair that decides it, so the annotation and the
 * lowering cannot drift apart; here we only mark the node.
 *
 * An ascription between the marker and the call is transparent: `^tailcall`
 * binds to the call underneath it, which is where the tail position is. */
Expr *elab_tailcall(Elab *e, const Form *call) {
    if (call->as.list.len != 2) {
        diag_emit_with_code(DIAG_ERROR, call->span, TUR_E0716_TAILCALL_NOT_TAIL,
                            "`^tailcall` takes exactly one call: "
                            "`^tailcall (f x)`");
        return NULL;
    }
    Expr *inner = elab_form(e, call->as.list.items[1]);
    if (!inner) return NULL;

    Expr *target = inner;
    while (target->kind == EX_ASCRIBE) target = target->as.ascribe_.inner;
    /* T6 (T-D6): a DYNAMIC call -- a call through an `any` in the dynamic
     * dialect -- is a call too, and in tail position it bounces to a
     * trampoline, so the annotation asks the same question of it. */
    if (target->kind == EX_DYN_CALL) {
        target->as.dyn_call_.wants_tailcall = true;
        return inner;
    }
    if (target->kind != EX_CALL) {
        diag_emit_with_code(DIAG_ERROR, call->span, TUR_E0716_TAILCALL_NOT_TAIL,
                            "`^tailcall` must annotate a function call; "
                            "a tail call is a call, and there is nothing here "
                            "to place in tail position");
        return NULL;
    }
    target->as.call_.wants_tailcall = true;
    return inner;
}

Expr *elab_return(Elab *e, const Form *call) {
    /* return is only valid inside function bodies */
    if (e->scope == &e->global) {
        diag_emit(DIAG_ERROR, call->span,
                  "return is not allowed at module top level");
        return NULL;
    }
    
    /* Check number of arguments */
    if (call->as.list.len > 2) {
        diag_emit(DIAG_ERROR, call->span,
                  "return takes at most one argument: (return) or (return expr)");
        return NULL;
    }
    
    Expr *value = NULL;
    if (call->as.list.len == 2) {
        /* (return expr) */
        value = elab_form(e, call->as.list.items[1]);
        if (!value) return NULL;

        /* Phase 11: returning a move-only binding transfers ownership. */
        if (value->kind == EX_VAR && type_is_move(value->as.var.binding->type)) {
            binding_mark_moved(value->as.var.binding, value->span);
        }
        /* byvalue-recursive-shared-copies-leak: a result is owned by the
         * caller, so a shared view returned here is cloned. */
        value = elab_own_byval_copy(e, value, NULL);
        /* The enclosing function's `:post` / refined return, which the
         * whole-body wrap cannot see a `return` reach. */
        value = rt_check_returned_value(e, value, call->span);
    }
    
    /* Create EX_RETURN expression */
    /* The type will be determined by the function's return type during type checking.
     * For now, use the value's type or NIL if no value. */
    Type return_type = value ? value->type : TYPE_NIL;
    Expr *out = expr_new(e->arena, EX_RETURN, return_type, call->span);
    out->as.return_.value = value;
    return out;
}

/* Phase R1: ? operator — (? expr)
 *
 * Lowers to:
 *   (let [__q_N expr]
 *     (if (err? __q_N)
 *         (return (err (err-val __q_N)))
 *         (ok-val __q_N)))
 *
 * Requires `err?`, `err`, `err-val`, and `ok-val` to be in scope (the fixture
 * defines these alongside the result-shaped type).  Must appear inside a
 * function body.  The if branches have types `!` (from `return`) and the
 * unwrapped ok type; `elab_if` accepts NEVER as compatible with any type and
 * propagates the other branch's type as the result.
 */
Expr *elab_question(Elab *e, const Form *call) {
    if (e->fn_body_depth == 0) {
        diag_emit(DIAG_ERROR, call->span,
                  "? operator is only allowed inside a function body");
        return NULL;
    }
    if (call->as.list.len != 2) {
        diag_emit(DIAG_ERROR, call->span,
                  "? operator requires exactly one argument: (? expr)");
        return NULL;
    }

    Form *inner = call->as.list.items[1];
    Span span = call->span;

    /* R1: reject a `?` applied to a literal scalar up front.  A literal can
     * never be a Result value, and -- being a literal -- it can never be a
     * moved-from binding, so inspecting it here is side-effect free (no risk
     * of double-evaluating an owned operand).  Computed non-Result operands
     * are still caught downstream by the TUR-E0001 arg-type check on
     * __tur-q-is-err?; this branch just gives the canonical mistake a clearer,
     * `?`-specific message. */
    switch (inner->tag) {
        case F_INT: case F_FLOAT: case F_BOOL:
        case F_NIL: case F_STR: case F_KEYWORD: {
            const char *what =
                inner->tag == F_INT     ? "int"     :
                inner->tag == F_FLOAT   ? "float"   :
                inner->tag == F_BOOL    ? "bool"    :
                inner->tag == F_STR     ? "cstr"    :
                inner->tag == F_KEYWORD ? "keyword" : "nil";
            diag_emit_with_code(DIAG_ERROR, inner->span, TUR_E0001_TYPE_MISMATCH,
                                "? operator requires a Result value, got %s", what);
            return NULL;
        }
        default:
            break;
    }

    /* Fresh symbol __q_N to avoid multiple evaluation of expr. */
    char q_name[32];
    snprintf(q_name, sizeof(q_name), "__q_%u", e->next_gensym_id++);
    const Symbol *q_sym = symtab_intern(e->st,
        strslice(q_name, (uint32_t)strlen(q_name)));
    Form *q_sym_f = form_sym(e->arena, span, q_sym);

    /* The ? lowering routes through two stdlib helpers --
     * __tur-q-is-err? and __tur-q-ok-val (defined in stdlib/result.tur) --
     * that wrap the unsafe err? / ok-val calls inside (unsafe ...). This
     * keeps the unsafe audit surface for ? to a single pair of named
     * functions and means user call sites of ? do not need their own
     * (unsafe ...) wrapper. Update both lowering and the stdlib helpers
     * together if the internal result representation changes. */
    const Symbol *sym_is_err = symtab_intern(e->st,
        strslice("__tur-q-is-err?", 15));
    const Symbol *sym_ok_val = symtab_intern(e->st,
        strslice("__tur-q-ok-val", 14));

    Form *is_err_f  = form_sym(e->arena, span, sym_is_err);
    Form *ok_val_f  = form_sym(e->arena, span, sym_ok_val);
    Form *if_f      = form_sym(e->arena, span, e->sym_if);
    Form *let_f     = form_sym(e->arena, span, e->sym_let);
    Form *return_f  = form_sym(e->arena, span, e->sym_return);

    /* (__tur-q-is-err? __q) */
    Form *test_items[2] = { is_err_f, q_sym_f };
    Form *test_form = form_list(e->arena, span, test_items, 2);

    /* (return __q) -- __q is already the err Result; propagate it as-is. */
    Form *return_items[2] = { return_f, q_sym_f };
    Form *return_form = form_list(e->arena, span, return_items, 2);

    /* (__tur-q-ok-val __q) */
    Form *ok_val_items[2] = { ok_val_f, q_sym_f };
    Form *ok_val_form = form_list(e->arena, span, ok_val_items, 2);

    /* (if (__tur-q-is-err? __q) (return __q) (__tur-q-ok-val __q)) */
    Form *if_items[4] = { if_f, test_form, return_form, ok_val_form };
    Form *if_form = form_list(e->arena, span, if_items, 4);

    /* (let [__q expr] if_form) */
    Form *bind_vec_items[2] = { q_sym_f, inner };
    Form *bind_vec = form_vec(e->arena, span, bind_vec_items, 2);
    Form *let_items[3] = { let_f, bind_vec, if_form };
    Form *let_form = form_list(e->arena, span, let_items, 3);

    return elab_form(e, let_form);
}

/* Phase 12: Borrow traits */

/* Elaborate (set! (@ r) value) - mutation through a mutable borrow.
 *
 * Called when elab_set detects a deref-assignment target: (set! (@ r) value).
 * Only &mut T is allowed as the borrow; &T produces an immutable-borrow diagnostic.
 */
static Expr *elab_set_deref(Elab *e, const Form *call, const Form *deref_form) {
    /* Elaborate the borrow expression r */
    Expr *ref = elab_form(e, deref_form->as.list.items[1]);
    if (!ref) return NULL;

    /* Must be &mut T */
    if (ref->type.kind == TY_REF_IMMUT) {
        diag_emit(DIAG_ERROR, deref_form->span,
                  "cannot assign through immutable borrow; use `&mut T` for mutation");
        return NULL;
    }
    if (ref->type.kind != TY_REF_MUT) {
        diag_emit(DIAG_ERROR, deref_form->span,
                  "set! via @ requires a &mut T borrow, got %s",
                  type_name(ref->type));
        return NULL;
    }

    /* Elaborate the value */
    Expr *value = elab_form(e, call->as.list.items[2]);
    if (!value) return NULL;

    /* Type-check: value must match the inner type */
    Type inner_type = type_from_kind(ref->type.as.ref_borrow.target);
    if (!type_eq(value->type, inner_type)) {
        diag_emit(DIAG_ERROR, value->span,
                  "set! type mismatch: cannot assign %s through &mut %s borrow",
                  type_name(value->type), type_name(inner_type));
        return NULL;
    }

    Expr *out = expr_new(e->arena, EX_SET_DEREF, TYPE_NIL, call->span);
    out->as.set_deref_.ref = ref;
    out->as.set_deref_.value = value;
    return out;
}

/* GF1: Walk an elaborated expression tree and collect all EX_LET bindings.
 * Used to find yield-live variables for generator struct field promotion. */
static void collect_let_bindings_walk(const Expr *body,
                                      Binding ***out_arr, uint32_t *n_out,
                                      uint32_t *cap_out) {
    if (!body) return;
    const Expr **stk = (const Expr **)malloc(256 * sizeof(const Expr *));
    int sp = 0;
    stk[sp++] = body;
    while (sp > 0) {
        const Expr *cur = stk[--sp];
        if (!cur) continue;
        switch (cur->kind) {
            case EX_LET:
                for (uint32_t i = 0; i < cur->as.let_.n; i++) {
                    Binding *b = cur->as.let_.bindings[i].binding;
                    if (b) {
                        if (*n_out >= *cap_out) {
                            *cap_out = *cap_out ? *cap_out * 2 : 8;
                            *out_arr = (Binding **)realloc(*out_arr, *cap_out * sizeof(Binding *));
                        }
                        (*out_arr)[(*n_out)++] = b;
                        if (cur->as.let_.bindings[i].init)
                            stk[sp++] = cur->as.let_.bindings[i].init;
                    }
                }
                if (cur->as.let_.body) stk[sp++] = cur->as.let_.body;
                break;
            case EX_IF:
                if (cur->as.if_.cond)         stk[sp++] = cur->as.if_.cond;
                if (cur->as.if_.then_)        stk[sp++] = cur->as.if_.then_;
                if (cur->as.if_.else_or_null) stk[sp++] = cur->as.if_.else_or_null;
                break;
            case EX_DO:
                for (uint32_t i = 0; i < cur->as.do_.n; i++)
                    stk[sp++] = cur->as.do_.items[i];
                break;
            case EX_WHILE:
                stk[sp++] = cur->as.while_.cond;
                stk[sp++] = cur->as.while_.body;
                break;
            case EX_YIELD:
                if (cur->as.yield_.value) stk[sp++] = cur->as.yield_.value;
                break;
            case EX_CALL:
                for (uint32_t i = 0; i < cur->as.call_.n_args; i++)
                    stk[sp++] = cur->as.call_.args[i];
                break;
            case EX_RETURN:
                if (cur->as.return_.value) stk[sp++] = cur->as.return_.value;
                break;
            default:
                break;
        }
    }
    free(stk);
}

/* CF5: recursively scan a Form tree for any reference to a given symbol. */
static bool form_contains_sym(const Form *f, const Symbol *sym) {
    if (!f) return false;
    if (f->tag == F_SYM) return f->as.sym == sym;
    if (f->tag == F_LIST || f->tag == F_VEC || f->tag == F_MAP || f->tag == F_SET) {
        for (uint32_t i = 0; i < f->as.list.len; i++)
            if (form_contains_sym(f->as.list.items[i], sym)) return true;
    }
    return false;
}

/* GF1: (gen [] body...) -- create a generator expression.
 *
 * The form (gen [] body) compiles to a heap-allocated C state-machine struct
 * with a _next function.  All let bindings in the body are promoted to struct
 * fields (conservative yield-live analysis).  Captures (free variables) are
 * passed to the _create function and stored in the struct. */
Expr *elab_gen(Elab *e, const Form *call) {
    if (call->as.list.len < 3) {
        diag_emit(DIAG_ERROR, call->span,
                  "gen requires a capture vector and at least one body form: (gen [] body)");
        return NULL;
    }
    /* v1: no nested generators */
    if (e->gen_ctx) {
        diag_emit(DIAG_ERROR, call->span,
                  "nested generators are not supported in v1");
        return NULL;
    }

    Form *cap_vec = call->as.list.items[1];
    if (cap_vec->tag != F_VEC) {
        diag_emit(DIAG_ERROR, cap_vec->span,
                  "gen second argument must be an empty capture vector []");
        return NULL;
    }
    if (cap_vec->as.list.len != 0) {
        diag_emit(DIAG_ERROR, cap_vec->span,
                  "gen capture vector must be empty in v1 (captures are computed automatically)");
        return NULL;
    }

    /* Generate unique struct/fn names */
    uint32_t gen_id = e->gen_counter++;
    const char *fn_name_raw = e->current_fn_name ? e->current_fn_name->name : "top";
    /* Mangle fn_name to a valid C identifier: replace hyphens/slashes/dots with '_'. */
    size_t fn_name_len = strlen(fn_name_raw);
    char *fn_name_buf = (char *)alloca(fn_name_len + 1);
    for (size_t _i = 0; _i < fn_name_len; _i++) {
        char _c = fn_name_raw[_i];
        fn_name_buf[_i] = ((_c >= 'a' && _c <= 'z') || (_c >= 'A' && _c <= 'Z') ||
                           (_c >= '0' && _c <= '9') || _c == '_') ? _c : '_';
    }
    fn_name_buf[fn_name_len] = '\0';
    const char *fn_name = fn_name_buf;

    GenDef *def = (GenDef *)arena_alloc(e->arena, sizeof(GenDef));
    memset(def, 0, sizeof(GenDef));
    snprintf(def->struct_name, sizeof(def->struct_name), "__gen_%s_%u_t", fn_name, gen_id);
    snprintf(def->next_fn,     sizeof(def->next_fn),    "__gen_%s_%u_next", fn_name, gen_id);
    snprintf(def->create_fn,   sizeof(def->create_fn),  "__gen_%s_%u_create", fn_name, gen_id);

    /* CF5: detect recursive generator -- enclosing function calls itself in body. */
    bool gen_is_recursive = false;
    if (e->current_fn_name) {
        for (uint32_t i = 2; i < call->as.list.len; i++) {
            if (form_contains_sym(call->as.list.items[i], e->current_fn_name)) {
                gen_is_recursive = true;
                break;
            }
        }
    }

    /* Push gen context */
    GenContext gctx;
    memset(&gctx, 0, sizeof(gctx));
    gctx.parent = e->gen_ctx;
    gctx.is_recursive = gen_is_recursive;
    e->gen_ctx = &gctx;

    /* Push a new scope for the gen body */
    Scope body_scope;
    scope_init(&body_scope, e->scope);
    e->scope = &body_scope;

    /* Elaborate body forms */
    uint32_t n_body = call->as.list.len - 2;
    Expr *body_expr = NULL;
    if (n_body == 1) {
        body_expr = elab_form(e, call->as.list.items[2]);
    } else {
        Expr **items = (Expr **)arena_alloc(e->arena, n_body * sizeof(Expr *));
        for (uint32_t i = 0; i < n_body; i++) {
            items[i] = elab_form(e, call->as.list.items[i + 2]);
            if (!items[i]) {
                e->scope = body_scope.parent;
                e->gen_ctx = gctx.parent;
                return NULL;
            }
        }
        body_expr = expr_new(e->arena, EX_DO, items[n_body-1]->type, call->span);
        body_expr->as.do_.items = items;
        body_expr->as.do_.n     = n_body;
    }
    if (!body_expr) {
        e->scope = body_scope.parent;
        e->gen_ctx = gctx.parent;
        return NULL;
    }

    /* Pop scope and gen context */
    e->scope   = body_scope.parent;
    e->gen_ctx = gctx.parent;

    def->n_yield_points = gctx.n_yields;
    def->element_kind   = gctx.element_kind_set ? gctx.element_kind : TY_INT;
    def->body           = body_expr;

    /* Collect free variables (captures) from the elaborated body */
    uint32_t n_caps = 0;
    Binding **caps = collect_free_vars(body_expr, NULL, 0, NULL, 0, &n_caps);
    def->captures   = caps ? (Binding **)arena_alloc(e->arena, n_caps * sizeof(Binding *)) : NULL;
    if (def->captures && caps)
        memcpy(def->captures, caps, n_caps * sizeof(Binding *));
    free(caps);
    def->n_captures = n_caps;

    /* Collect all let bindings for struct field promotion */
    Binding **let_binds  = NULL;
    uint32_t  n_let      = 0;
    uint32_t  cap_let    = 0;
    collect_let_bindings_walk(body_expr, &let_binds, &n_let, &cap_let);

    /* Build struct_bindings = captures ++ let_bindings (de-duplicated) */
    uint32_t total = n_caps + n_let;
    Binding **sb = NULL;
    uint32_t n_sb = 0;
    if (total > 0) {
        sb = (Binding **)arena_alloc(e->arena, total * sizeof(Binding *));
        /* Add captures first */
        for (uint32_t i = 0; i < n_caps; i++) sb[n_sb++] = def->captures[i];
        /* Add let bindings (skip if already in captures) */
        for (uint32_t i = 0; i < n_let; i++) {
            bool dup = false;
            for (uint32_t j = 0; j < n_caps; j++) {
                if (def->captures[j] == let_binds[i]) { dup = true; break; }
            }
            if (!dup) sb[n_sb++] = let_binds[i];
        }
    }
    free(let_binds);
    def->struct_bindings   = sb;
    def->n_struct_bindings = n_sb;

    /* Build result type: TY_GENERATOR */
    Type gen_type;
    memset(&gen_type, 0, sizeof(gen_type));
    gen_type.kind = TY_GENERATOR;
    gen_type.copy_kind = CK_COPY;
    gen_type.hkt_kind  = KIND_STAR;
    gen_type.as.generator_.element_kind = def->element_kind;
    gen_type.as.generator_.element_tyvar = gctx.element_tyvar;

    Expr *out = expr_new(e->arena, EX_GEN, gen_type, call->span);
    out->as.gen_.def = def;
    return out;
}

/* GF1: (yield expr) -- yield a value inside a gen body. */
Expr *elab_yield(Elab *e, const Form *call) {
    if (!e->gen_ctx) {
        diag_emit(DIAG_ERROR, call->span,
                  "yield is only valid inside a gen body");
        return NULL;
    }
    /* CF5 (TUR-E0702): yield inside a match arm is unsupported in v1. */
    if (e->in_match_arm) {
        diag_emit_with_code(DIAG_ERROR, call->span, TUR_E0702_YIELD_IN_MATCH_ARM,
            "'yield' is not supported inside a 'match' arm (1.0 limitation); "
            "this requires the post-1.0 CPS pass.");
        return NULL;
    }
    /* CF5 (TUR-E0703): yield inside a recursive generator is unsupported in v1. */
    if (e->gen_ctx->is_recursive) {
        diag_emit_with_code(DIAG_ERROR, call->span, TUR_E0703_YIELD_IN_RECURSIVE_GEN,
            "'yield' is not supported inside a recursive generator (1.0 limitation); "
            "this requires the post-1.0 CPS pass.");
        return NULL;
    }
    if (call->as.list.len != 2) {
        diag_emit(DIAG_ERROR, call->span,
                  "yield requires exactly one argument: (yield expr)");
        return NULL;
    }
    Expr *value = elab_form(e, call->as.list.items[1]);
    if (!value) return NULL;

    /* Record element kind on first yield */
    if (!e->gen_ctx->element_kind_set) {
        e->gen_ctx->element_kind     = value->type.kind;
        e->gen_ctx->element_kind_set = true;
        /* generator-in-generic: remember WHICH tyvar, when it is one the
         * enclosing signature quantifies -- the kind alone collapsed the
         * element to `int` at gen-unwrap. */
        if (value->type.kind == TY_TYVAR && value->type.as.tyvar_.name)
            for (uint8_t si = 0; si < e->n_sig_tyvars; si++)
                if (e->sig_tyvars[si] &&
                    strcmp(e->sig_tyvars[si], value->type.as.tyvar_.name) == 0) {
                    e->gen_ctx->element_tyvar = value->type.as.tyvar_.name;
                    break;
                }
    }

    uint32_t yid = ++e->gen_ctx->n_yields;

    Expr *out = expr_new(e->arena, EX_YIELD, TYPE_NIL, call->span);
    out->as.yield_.value    = value;
    out->as.yield_.yield_id = yid;
    return out;
}

/* GF1: (gen-next g) -- advance a generator; returns ptr<void> (some/none). */
Expr *elab_gen_next(Elab *e, const Form *call) {
    if (call->as.list.len != 2) {
        diag_emit(DIAG_ERROR, call->span,
                  "gen-next requires exactly one argument: (gen-next g)");
        return NULL;
    }
    Expr *gen_expr = elab_form(e, call->as.list.items[1]);
    if (!gen_expr) return NULL;
    if (gen_expr->type.kind != TY_GENERATOR) {
        diag_emit(DIAG_ERROR, gen_expr->span,
                  "gen-next expects a generator value, got %s",
                  type_name(gen_expr->type));
        return NULL;
    }

    Type ptr_type;
    memset(&ptr_type, 0, sizeof(ptr_type));
    ptr_type.kind      = TY_PTR_VOID;
    ptr_type.copy_kind = CK_COPY;
    ptr_type.hkt_kind  = KIND_STAR;

    Expr *out = expr_new(e->arena, EX_GEN_NEXT, ptr_type, call->span);
    out->as.gen_next_.gen_expr = gen_expr;
    out->as.gen_next_.def      = gen_expr->as.gen_.def;
    return out;
}

/* generator-yield-payload-is-int64-only: (gen-unwrap p) -- read the yielded
 * value out of a gen-next result, at the generator's element type.
 *
 * `gen-unwrap` was a stdlib inline-C reader declared `: int`, so the slot's
 * type reached the SIGNATURE: a yielded 7.25 printed 7, a cstr printed its
 * address, and nothing objected.  The element kind is on the generator's type
 * (`TY_GENERATOR.element_kind`, recorded at the first `yield`), and the read
 * reaches it through the `(gen-next g)` it consumes or the let binding that
 * holds one -- the same provenance rule the await uses, and the shape every
 * stdlib macro (`gen-for-each`, `gen-collect`, `yield*`) expands to.  A
 * word-shaped element (a scalar class, cstr, ptr<void>, :Sym) is read at its
 * type; anything else, or an opaque pointer, keeps the `int` read the stdlib
 * reader gave.  The slot holds the value's BITS (emit_stmt's EX_YIELD stores
 * a float's IEEE-754 pattern, not an int conversion), so the read is a
 * reinterpret, never a value conversion. */
Expr *elab_gen_unwrap(Elab *e, const Form *call) {
    Expr *p = elab_form(e, call->as.list.items[1]);
    if (!p) return NULL;
    TypeKind elem = TY_INT;
    bool have = false;
    const Expr *src = p;
    while (src && src->kind == EX_ASCRIBE) src = src->as.ascribe_.inner;
    const char *elem_tyvar = NULL;
    if (src && src->kind == EX_GEN_NEXT && src->as.gen_next_.gen_expr &&
        src->as.gen_next_.gen_expr->type.kind == TY_GENERATOR) {
        elem = src->as.gen_next_.gen_expr->type.as.generator_.element_kind;
        elem_tyvar = src->as.gen_next_.gen_expr->type.as.generator_.element_tyvar;
        have = true;
    } else if (src && src->kind == EX_VAR && src->as.var.binding &&
               src->as.var.binding->gen_elem_set) {
        elem = src->as.var.binding->gen_elem_kind;
        elem_tyvar = src->as.var.binding->gen_elem_tyvar;
        have = true;
    }
    /* generator-in-generic: an element typed with the enclosing signature's
     * own tyvar reads as that tyvar -- the emitter bridges it per clone and
     * the interpreter hands back the yielded value -- instead of collapsing
     * to the `int` read below (7.1's bits printed as 4.61968e+18). */
    if (have && elem == TY_TYVAR && elem_tyvar) {
        Expr *tout = expr_new(e->arena, EX_GEN_UNWRAP,
                              type_tyvar_named(elem_tyvar), call->span);
        tout->as.gen_unwrap_.ptr_expr = p;
        tout->as.gen_unwrap_.elem     = TY_TYVAR;
        return tout;
    }
    if (have) {
        switch (elem) {
            case TY_BOOL: case TY_FLOAT: case TY_FLOAT64: case TY_FLOAT32:
            case TY_CSTR: case TY_PTR_VOID: case TY_SYM:
            case TY_INT: case TY_INT64: case TY_UINT64: case TY_INT32:
            case TY_UINT32: case TY_INT16: case TY_UINT16: case TY_INT8:
            case TY_UINT8:
                break;
            default:
                elem = TY_INT;
                break;
        }
    }
    Expr *out = expr_new(e->arena, EX_GEN_UNWRAP, type_from_kind(elem), call->span);
    out->as.gen_unwrap_.ptr_expr = p;
    out->as.gen_unwrap_.elem     = elem;
    return out;
}

/* GF1: (gen-done? g) -- true if the generator is exhausted. */
Expr *elab_gen_done(Elab *e, const Form *call) {
    if (call->as.list.len != 2) {
        diag_emit(DIAG_ERROR, call->span,
                  "gen-done? requires exactly one argument: (gen-done? g)");
        return NULL;
    }
    Expr *gen_expr = elab_form(e, call->as.list.items[1]);
    if (!gen_expr) return NULL;
    if (gen_expr->type.kind != TY_GENERATOR) {
        diag_emit(DIAG_ERROR, gen_expr->span,
                  "gen-done? expects a generator value, got %s",
                  type_name(gen_expr->type));
        return NULL;
    }

    Type bool_type;
    memset(&bool_type, 0, sizeof(bool_type));
    bool_type.kind      = TY_BOOL;
    bool_type.copy_kind = CK_COPY;
    bool_type.hkt_kind  = KIND_STAR;

    Expr *out = expr_new(e->arena, EX_GEN_DONE, bool_type, call->span);
    out->as.gen_done_.gen_expr = gen_expr;
    return out;
}
