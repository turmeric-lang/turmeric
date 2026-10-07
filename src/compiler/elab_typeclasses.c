/* elab_typeclasses.c -- typeclass declarations, instances, and method-call dispatch. */
#include "elab_internal.h"
#include "lang_dialects.h"   /* saffron-lang-plan S4: lang_span_is_dynamic */
#include "refine_discharge.h"     /* RT1: instance/class refinement variance */
#include "refine_solver.h"        /* RT1: refine_model_search, for the variance witness */
#include "forms.h"
#include "mangle.h"

/* ---- file-local helper forward declarations ---- */
/* panic-location-names-the-runtime-not-the-call-site: an instance method's
 * name for a failed contract's message -- the head of its `(name [params]
 * body)` impl form. */
static const char *rt_impl_method_name(const Form *impl_form) {
    if (impl_form && impl_form->tag == F_LIST && impl_form->as.list.len > 0 &&
        impl_form->as.list.items[0]->tag == F_SYM)
        return impl_form->as.list.items[0]->as.sym->name;
    return NULL;
}

static TypeClassMethod *parse_typeclass_method(Elab *e, Form *method_form, Span span,
    uint32_t *out_body_start,
    const Symbol **class_type_params, uint8_t n_class_type_params,
    const Kind *class_type_param_kinds,
    const Symbol **assoc_type_names, uint8_t n_assoc_type_names);
static Expr *make_dict_expr(Elab *e, TypeClassInstance *inst, Span span);
static bool rt_type_mentions_tyvar(const Type *t, const char *name);

/* Phase RT: is class method `m` of class `c` a return-only-dispatch method --
 * one of the class's type parameters appears in the return type but in no
 * parameter type?  Such methods select their instance from the expected result
 * type, so their parameters must NOT be coerced to the instance type the way
 * an ordinary receiver-dispatched method's first int parameter is. */
static bool method_is_return_dispatch(const TypeClass *c, const TypeClassMethod *m) {
    for (uint8_t ti = 0; ti < c->n_type_params; ti++) {
        const Symbol *tp = c->type_params[ti];
        if (!tp) continue;
        if (!rt_type_mentions_tyvar(&m->return_type, tp->name)) continue;
        bool in_param = false;
        for (uint32_t pi = 0; pi < m->n_params; pi++) {
            if (rt_type_mentions_tyvar(&m->param_types[pi], tp->name)) {
                in_param = true;
                break;
            }
        }
        if (!in_param) return true;
    }
    return false;
}

/* KB-030: designated home file (basename) for a built-in primitive type that
 * has no StructDef of its own (e.g. `str`, `rc`).  The orphan-instance check
 * credits a user struct to its defining module via origin_file_id; built-ins
 * have no def and so were always flagged orphan.  This table gives each such
 * built-in a home stdlib file, so an instance declared in that file (e.g.
 * `(definstance Eq [str] ...)` in stdlib/str.tur) is treated as non-orphan,
 * mirroring the ownership rule for user types.  Returns NULL for names that
 * are not registered built-ins. */
static const char *builtin_type_home_basename(const char *type_name) {
    if (!type_name) return NULL;
    if (strcmp(type_name, "str")  == 0) return "str.tur";
    if (strcmp(type_name, "rc")   == 0) return "rc.tur";
    if (strcmp(type_name, "weak") == 0) return "rc.tur";
    return NULL;
}

/* KB-030/KB-027: as above, but keyed on a resolved built-in TypeKind.  Some
 * built-ins resolve to a dedicated TypeKind rather than an opaque-struct name,
 * so they carry no type_arg_syms entry; map those kinds to their home file
 * directly.  Two families:
 *   - rc<T>/weak<T> are data types with their own module -> rc.tur;
 *   - the bare scalar primitives (int, bool, cstr, the sized numeric kinds)
 *     have no data module of their own, so their canonical typeclass instances
 *     live in the comprehensive typeclass module -> typeclass.tur.  This lets
 *     typeclass.tur host `Clone [int]`, `Clone [uint8]`, ... without tripping
 *     the orphan check (Clone is declared in typeclass-clone.tur).
 * Returns NULL for kinds with no fixed home. */
static const char *builtin_kind_home_basename(TypeKind k) {
    switch (k) {
        case TY_RC:
        case TY_WEAK:
            return "rc.tur";
        case TY_INT:
        case TY_BOOL:
        case TY_CSTR:
        case TY_FLOAT:
        case TY_INT8:
        case TY_INT16:
        case TY_INT32:
        case TY_INT64:
        case TY_UINT8:
        case TY_UINT16:
        case TY_UINT32:
        case TY_UINT64:
        case TY_FLOAT32:
        case TY_FLOAT64:
            return "typeclass.tur";
        /* SYM3 (runtime-symbols-plan): the :Sym primitive has no data module of
         * its own; its canonical instances (Eq/Hash/MapKey[Sym]) live in
         * sym.tur, which is only auto-loaded under -Xsymbols.  Crediting Sym to
         * sym.tur keeps those instances non-orphan there. */
        case TY_SYM:
            return "sym.tur";
        default:
            return NULL;
    }
}

/* Return the final path component of `path` (the basename), or `path` itself
 * when it contains no '/'.  NULL-safe. */
static const char *tc_path_basename(const char *path) {
    if (!path) return NULL;
    const char *slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

/* F3-5 (cross-plan-followups): convert a runtime Type back to its
 * source-form representation so the dispatcher can synthesise inline
 * ascription forms.  Supports only the kinds the dispatch synthesis
 * actually needs: primitives + TY_STRUCT (named) + TY_APP.  Returns
 * NULL for unsupported kinds. */
Form *type_to_form(Elab *e, const Type *t, Span span) {
    if (!t) return NULL;
    const char *kw = NULL;
    switch (t->kind) {
        case TY_INT:      kw = "int";      break;
        case TY_BOOL:     kw = "bool";     break;
        case TY_CSTR:     kw = "cstr";     break;
        case TY_FLOAT:    kw = "float";    break;
        case TY_INT8:     kw = "int8";     break;
        case TY_INT16:    kw = "int16";    break;
        case TY_INT32:    kw = "int32";    break;
        case TY_UINT8:    kw = "uint8";    break;
        case TY_UINT16:   kw = "uint16";   break;
        case TY_UINT32:   kw = "uint32";   break;
        case TY_UINT64:   kw = "uint64";   break;
        case TY_FLOAT32:  kw = "float32";  break;
        case TY_PTR_VOID: kw = "ptr<void>"; break;
        case TY_SYM:      kw = "Sym";      break;
        default: break;
    }
    if (kw) {
        return form_keyword(e->arena, span,
            intern_cstr(e->st, kw));
    }
    /* CONV-S2: under defstruct-as-defadt a typed-collection element is a lowered
     * record ADT (`Vec`/`Map`/...), so its bare name and TY_APP head are TY_ADT,
     * not TY_STRUCT.  Mirror the struct cases so the comparator-synthesis
     * ascription form (`(:: a (Vec int))`) is built for an ADT element; without
     * it type_to_form returns NULL, the dispatch synthesis declines, and the
     * element comparator collapses to the wrong (int) instance. */
    if (t->kind == TY_ADT && t->as.adt_.def && t->as.adt_.def->name) {
        return form_sym(e->arena, span,
            intern_cstr(e->st, t->as.adt_.def->name));
    }
    if (t->kind == TY_APP) {
        /* Walk the TY_APP chain to collect [head, arg1, arg2, ...]
         * (left-associative -- arg1 is the innermost). */
        const Type *args[8];
        uint8_t n_args = 0;
        const Type *head = t;
        while (head && head->kind == TY_APP && n_args < 8) {
            if (head->as.app.arg) args[n_args++] = head->as.app.arg;
            head = head->as.app.fn;
        }
        const char *head_name =
            (head && head->kind == TY_ADT && head->as.adt_.def)
                ? head->as.adt_.def->name
          : NULL;
        if (!head_name) {
            return NULL;
        }
        /* Build (StructName arg1-form arg2-form ...) in original order. */
        uint32_t n_items = 1 + n_args;
        Form **items = (Form **)arena_alloc(e->arena, n_items * sizeof(Form *));
        items[0] = form_sym(e->arena, span, intern_cstr(e->st, head_name));
        /* args were collected innermost-first; reverse to original order. */
        for (uint32_t i = 0; i < n_args; i++) {
            Form *af = type_to_form(e, args[n_args - 1 - i], span);
            if (!af) return NULL;
            items[1 + i] = af;
        }
        return form_list(e->arena, span, items, n_items);
    }
    return NULL;
}

/* F3-5: map a typed-collection struct identity to its eq-helper symbol
 * and the number of element-comparator parameters the helper expects.
 *
 * Helpers split into two shapes:
 *   1-comparator: (helper m1 m2 elem-cmp)               -- Vec, Map,
 *                                                          Option, Cons
 *   2-comparator: (helper m1 m2 cmp-A cmp-B)            -- Pair, Result
 *
 * For Map's keys the helper relies on the HAMT's hash lookup -- key
 * equality is via the hash + pointer/memcmp, which is correct for
 * primitive keys.  Recursive Map[K (Vec V)] etc. only need the
 * V-comparator threaded recursively.
 *
 * Set is intentionally not listed: `set-eq?` takes no comparator
 * (relies on hash equality entirely), so even single-level dispatch
 * is wrong for non-primitive elements; fixing it requires changes to
 * the helper itself, not just the dispatcher.
 *
 * Returns NULL if the struct is not a recognised typed collection. */
static const Symbol *helper_eq_symbol_for_struct(Elab *e, const AdtDef *sd,
                                                  uint8_t *out_n_comparators) {
    if (!sd || !sd->name) return NULL;
    if (strcmp(sd->name, "Vec") == 0) {
        if (out_n_comparators) *out_n_comparators = 1;
        return intern_cstr(e->st, "vec-eq?");
    }
    if (strcmp(sd->name, "Map") == 0) {
        if (out_n_comparators) *out_n_comparators = 1;
        return intern_cstr(e->st, "map-eq?");
    }
    if (strcmp(sd->name, "Option") == 0) {
        if (out_n_comparators) *out_n_comparators = 1;
        return intern_cstr(e->st, "option-eq?");
    }
    if (strcmp(sd->name, "Cons") == 0) {
        if (out_n_comparators) *out_n_comparators = 1;
        return intern_cstr(e->st, "list-eq?");
    }
    if (strcmp(sd->name, "Pair") == 0) {
        if (out_n_comparators) *out_n_comparators = 2;
        return intern_cstr(e->st, "pair-eq?");
    }
    if (strcmp(sd->name, "Result") == 0) {
        if (out_n_comparators) *out_n_comparators = 2;
        return intern_cstr(e->st, "result-eq?");
    }
    if (strcmp(sd->name, "Set") == 0) {
        /* `set-eq?` itself takes no comparator (relies on HAMT hash
         * equality, wrong for non-primitive elements).  The synth path
         * routes to a comparator-taking variant `set-eq-cmp?` instead
         * so structural equality of Set[Vec[int]] etc. works
         * correctly. */
        if (out_n_comparators) *out_n_comparators = 1;
        return intern_cstr(e->st, "set-eq-cmp?");
    }
    return NULL;
}

/* F3-5: build a comparator lambda Form for a single type argument.
 * The lambda has shape `(fn [a b] (.eq? (:: a TYPE) (:: b TYPE)))`
 * where TYPE is the source-form of `elem_type`.  At elaboration time
 * the inner `.eq?` dispatches on the ascribed receiver type, which
 * (thanks to F3-7's sticky ascription) terminates the recursion at
 * the right level.  Returns NULL if `elem_type` cannot be
 * round-tripped to a Form. */
/* container-element-form-plan CE2 (default since option-niche graduated):
 * `slot_words` says the helper hands the comparator RAW Vec slot words
 * (`vec-eq?` reads `a->data[i]` straight out of the buffer).  A niche
 * `(Option P)` element sits in its slot as the payload pointer itself, so the
 * `(:: __cmp_slot_a (Option P))` bridge must reinterpret the word, never unbox
 * it as a carrier box.  The `__cmp_slot_` prefix is the mark: emit_slot_word_is
 * recognises it exactly as it does the hoist temp of a `vec-get`.  Every other
 * helper (map/option/list/pair/result-eq?) hands the comparator a value that
 * crossed the carrier boundary (boxed), so those keep the plain names. */
/* Which of the `*-eq?` helpers hand their comparator the STORED WORD, and
 * which hand it a value that has crossed the carrier boundary (a box).  The
 * split is not "Vec versus everything else" -- it is "walks its own payload
 * slots" versus "iterates a HAMT":
 *
 *   Vec     vec-eq?       `a->data[i]`               -- word
 *   Cons    list-eq?      `(list-head l)`            -- word
 *   Option  option-eq?    the `(Some v)` match binder -- word
 *   Result  result-eq?    the sum's payload slot      -- word
 *   Pair    pair-eq?      `a->fst` / `a->snd`         -- word
 *   Map     map-eq?       the HAMT value             -- box
 *   Set     set-eq-cmp?   the HAMT key               -- box
 *
 * For a niche `(Option P)` element the stored word is the payload pointer
 * itself, so a comparator fed a word must REINTERPRET it rather than unbox a
 * box that is not there.  This was `strcmp(sd->name, "Vec") == 0`, which made
 * `(eq? a b)` silently wrong for every OTHER word-passing container once the
 * niche graduated -- `(eq? (some (some "aa")) (some (some "bb")))` answered
 * true, because the inner comparator unboxed a String pointer as a box and
 * compared whatever it found.  Keep it in step with elab_call.c's
 * call_comparator_gets_slot_words, which answers the same question for a
 * comparator the USER writes at one of these calls. */
static bool container_helper_passes_slot_words(const char *sd_name) {
    if (!sd_name) return false;
    return strcmp(sd_name, "Vec") == 0 || strcmp(sd_name, "Cons") == 0 ||
           strcmp(sd_name, "Option") == 0 || strcmp(sd_name, "Result") == 0 ||
           strcmp(sd_name, "Pair") == 0;
}

static Form *build_comparator_lambda(Elab *e, const Type *elem_type, Span span,
                                     bool slot_words) {
    Form *elem_form = type_to_form(e, elem_type, span);
    if (!elem_form) return NULL;

    const Symbol *sym_a       = intern_cstr(e->st, slot_words ? "__cmp_slot_a" : "__cmp_a");
    const Symbol *sym_b       = intern_cstr(e->st, slot_words ? "__cmp_slot_b" : "__cmp_b");
    const Symbol *sym_dot_eq  = intern_cstr(e->st, ".eq?");

    Form **asc_a_items = (Form **)arena_alloc(e->arena, 3 * sizeof(Form *));
    asc_a_items[0] = form_sym(e->arena, span, e->sym_ascribe);
    asc_a_items[1] = form_sym(e->arena, span, sym_a);
    asc_a_items[2] = elem_form;
    Form *asc_a = form_list(e->arena, span, asc_a_items, 3);

    Form **asc_b_items = (Form **)arena_alloc(e->arena, 3 * sizeof(Form *));
    asc_b_items[0] = form_sym(e->arena, span, e->sym_ascribe);
    asc_b_items[1] = form_sym(e->arena, span, sym_b);
    asc_b_items[2] = elem_form;
    Form *asc_b = form_list(e->arena, span, asc_b_items, 3);

    Form **dot_eq_items = (Form **)arena_alloc(e->arena, 3 * sizeof(Form *));
    dot_eq_items[0] = form_sym(e->arena, span, sym_dot_eq);
    dot_eq_items[1] = asc_a;
    dot_eq_items[2] = asc_b;
    Form *dot_eq_call = form_list(e->arena, span, dot_eq_items, 3);

    Form **params_items = (Form **)arena_alloc(e->arena, 2 * sizeof(Form *));
    params_items[0] = form_sym(e->arena, span, sym_a);
    params_items[1] = form_sym(e->arena, span, sym_b);
    Form *params_vec = form_vec(e->arena, span, params_items, 2);

    Form **lambda_items = (Form **)arena_alloc(e->arena, 3 * sizeof(Form *));
    lambda_items[0] = form_sym(e->arena, span, e->sym_fn);
    lambda_items[1] = params_vec;
    lambda_items[2] = dot_eq_call;
    return form_list(e->arena, span, lambda_items, 3);
}

/* GHE5/#4: build `(mk-cmp (:: 0 K))` -- the MapKey[K] carrier comparator for
 * a concrete key type K.  mk-cmp ignores its argument value (it returns a
 * constant carrier-ABI function pointer), so the ascribed 0 only carries the
 * type K for dispatch.  Returns NULL if K cannot be round-tripped to a Form. */
static Form *build_mapkey_cmp_form(Elab *e, const Type *key_type, Span span) {
    Form *key_form = type_to_form(e, key_type, span);
    if (!key_form) return NULL;
    const Symbol *sym_mk_cmp = intern_cstr(e->st, "mk-cmp");

    Form **asc_items = (Form **)arena_alloc(e->arena, 3 * sizeof(Form *));
    asc_items[0] = form_sym(e->arena, span, e->sym_ascribe);
    asc_items[1] = form_int(e->arena, span, 0);
    asc_items[2] = key_form;
    Form *asc = form_list(e->arena, span, asc_items, 3);

    Form **mk_items = (Form **)arena_alloc(e->arena, 2 * sizeof(Form *));
    mk_items[0] = form_sym(e->arena, span, sym_mk_cmp);
    mk_items[1] = asc;
    return form_list(e->arena, span, mk_items, 2);
}

/* F3-5: synthesise the dispatcher rewrite for `(.eq? obj other)` when
 * `obj` has TY_APP receiver type AND the outer instance is a known
 * typed-collection whose element type(s) include at least one TY_APP
 * (recursive structural equality).  Returns NULL if conditions aren't
 * met or synthesis isn't applicable.
 *
 * 1-comparator helpers (Vec, Map, Option, Cons) synthesise:
 *   (<helper> obj other (fn [a b] (.eq? (:: a <elem>) (:: b <elem>))))
 *
 * 2-comparator helpers (Pair, Result) synthesise:
 *   (<helper> obj other
 *     (fn [a b] (.eq? (:: a <fst>) (:: b <fst>)))
 *     (fn [a b] (.eq? (:: a <snd>) (:: b <snd>))))
 *
 * The synthesised closures re-enter elab_method_call with `(:: a <T>)`
 * as the receiver, which (thanks to F3-7's sticky ascription) dispatches
 * to the right inner instance.  Recursion terminates at primitive
 * element types where F3-7's single-level path takes over. */
/* CRU B-3: box a synthesized comparator expr into a fat closure.  The
 * constrained-Eq synthesis dispatcher builds its helper call as an EX_CALL node
 * directly, bypassing elab_call's ^fat auto-shim -- so a captureless comparator
 * lambda would reach the (now ^fat) *-eq? value-comparator parameter as a bare
 * function pointer and be misread as a fat box (segfault, per
 * docs/archive/history/eq-synthesis-dispatcher-passes-bare-comparator-to-fat-sink.md).
 * Wrapping it in EX_FN_TO_FAT here boxes a captureless lambda via the
 * per-signature __tur_fatshim_*, and is a pass-through for an already-fat
 * (capturing) closure -- matching the fat dispatch the helper bodies now use.
 * Only *value/element* comparators are boxed; the MapKey `keyeq` carrier stays
 * thin (it is a constant carrier-ABI fn pointer, not a user closure). */
static Expr *box_synth_comparator(Elab *e, Expr *inner) {
    if (!inner) return NULL;
    Expr *shim = expr_new(e->arena, EX_FN_TO_FAT, TYPE_PTR_VOID, inner->span);
    shim->as.fn_to_fat_.inner = inner;
    return shim;
}

/* Coerce an argument bound for a fat-closure (is_fat) method parameter into the
 * fat-closure representation, mirroring the `^fat` auto-shim in elab_call.c.  A
 * bare (non-capturing) TY_FN reference is boxed via EX_FN_TO_FAT; a capturing
 * closure (boxed TY_FN), an already-fat :ptr<void> handle, or nil passes
 * through unchanged.  Used by arrow-instance dispatch so `(comp add1 dbl)`
 * feeds its function arguments to the arrow method's fat-closure parameters. */
static Expr *arrow_fat_shim(Elab *e, Expr *a) {
    if (!a) return a;
    if (a->type.kind == TY_FN && !a->type.as.fn.boxed) {
        /* fat-closure-var-passthrough: a ^fat let-binding or parameter whose
         * type was set to a concrete (fn ...) annotation is ALREADY a fat
         * closure carried as int64_t.  Re-type to ptr<void> (the natural fat
         * carrier) so it passes through without being double-boxed into a
         * __tur_fatshim_void___void__ wrapper.  That shim calls slot[1] as a
         * bare one-arg fn, but slot[1] here IS a fat closure whose thunk
         * expects two arguments (env + arg), causing a segfault.
         * Mirrors the is_fat guard in elab_call.c (two-level-sf-closure-
         * return-miscompiles-out-binding). */
        if (a->kind == EX_VAR && a->as.var.binding && a->as.var.binding->is_fat) {
            a->type = TYPE_PTR_VOID;
            return a;
        }
        Expr *shim = expr_new(e->arena, EX_FN_TO_FAT, TYPE_PTR_VOID, a->span);
        shim->as.fn_to_fat_.inner = a;
        return shim;
    }
    return a;
}

/* G10: a type argument is "concrete for instance discrimination" when it is a
 * named struct/ADT OR a concrete primitive (int/cstr/bool/float/...).  Two
 * instances over the same applied head differing only in a concrete primitive
 * element -- `Enc [(Option cstr)]` vs `Enc [(Option int)]` -- must be told apart;
 * the prior check considered only TY_STRUCT/TY_ADT concrete, so a primitive
 * element difference was ignored and both instances matched any `(Option X)`
 * receiver (the last-defined silently won).  A tyvar element stays a wildcard
 * (parametric instances match any element), so it is deliberately NOT concrete. */
static bool typeclass_type_arg_concrete(const Type *t) {
    if (!t) return false;
    switch (t->kind) {
        case TY_ADT:    return t->as.adt_.def != NULL;
        case TY_INT: case TY_INT8: case TY_INT16: case TY_INT32: case TY_INT64:
        case TY_UINT64: case TY_BOOL: case TY_CSTR: case TY_FLOAT:
        case TY_FLOAT32: case TY_FLOAT64: case TY_NIL: case TY_PTR_VOID:
            return true;
        default: return false;
    }
}

/* True when `tc` has an instance whose head is the function arrow (TY_FN).
 * Used to defer a structurally-return-dispatch method (e.g. `comp [f g] : a`,
 * whose untyped params do not mention the class variable) to argument-based
 * dispatch when the call supplies arrow arguments: the function argument's type
 * selects the arrow instance, so no return-type ascription is needed. */
static bool typeclass_has_arrow_instance(TypeClassEnv *env, const TypeClass *tc) {
    for (TypeClassInstance *inst = env->instances; inst; inst = inst->next) {
        if (inst->typeclass != tc) continue;
        if (inst->n_type_args > 0 && inst->type_args[0].kind == TY_FN) return true;
    }
    return false;
}

static Expr *try_synth_recursive_eq(Elab *e, TypeClassInstance *outer_inst,
                                     Expr *obj, Expr *other_arg, Span span) {
    if (!outer_inst || outer_inst->n_type_args == 0) return NULL;
    if (obj->type.kind != TY_APP || !obj->type.as.app.arg) return NULL;
    /* Look up the helper for this typed-collection. */
    const AdtDef *sd = (outer_inst->type_args[0].kind == TY_ADT)
                           ? outer_inst->type_args[0].as.adt_.def
                           : NULL;

    /* GHE5/#4: content-keyed structural equality for Map[K V].  For a Map the
     * outermost TY_APP arg is V and the inner is K (Map[K V] = app(app(Map,K),V)).
     * The generic Eq[Map] instance compares keys by carrier identity, which is
     * wrong for content keys (:cstr -- distinct pointers with equal text; a
     * heap-boxed struct -- distinct boxes with equal fields).  Since this
     * dispatch site is *concrete*, thread the per-K MapKey comparator -- exactly
     * what map-assoc/map-get do at their concrete call sites -- into the
     * content-aware map-eq-k? helper, alongside the recursive value comparator.
     *
     * Key witnesses to drive MapKey[K] dispatch (mk-cmp ignores its argument
     * value -- it returns a constant carrier-ABI comparator -- so any value of
     * type K serves):
     *   - :cstr        -> (mk-cmp (:: 0 cstr))          (pointer carrier; the
     *                                                    int->cstr ascription is
     *                                                    a no-op)
     *   - struct K     -> (mk-cmp (make-struct K 0 ...)) (a by-value zero struct,
     *                     matching mk-cmp[K]'s by-value ABI; (:: 0 K) would
     *                     instead deref a non-pointer aggregate).  Scoped to
     *                     all-:int-field structs so the 0 literals type-check.
     * Int/bool/float and opaque-over-int keys are inline values whose carrier
     * identity already coincides with content equality, so they are left to the
     * generic path. */
    if (sd && strcmp(sd->name, "Map") == 0 &&
        obj->type.as.app.fn && obj->type.as.app.fn->kind == TY_APP &&
        obj->type.as.app.fn->as.app.arg) {
        const Type *v_type = obj->type.as.app.arg;
        const Type *k_type = obj->type.as.app.fn->as.app.arg;
        Form *kf = NULL;
        if (k_type->kind == TY_CSTR) {
            kf = build_mapkey_cmp_form(e, k_type, span);
        }
        if (kf) {
            Form *vf = build_comparator_lambda(e, v_type, span, false);
            if (!vf) return NULL;
            Expr *kcmp = elab_form(e, kf);
            Expr *vcmp = elab_form(e, vf);
            if (!kcmp || !vcmp) return NULL;
            Binding *mek_b = scope_lookup(&e->global, intern_cstr(e->st, "map-eq-k?"));
            if (!mek_b) return NULL;
            Expr **ca = (Expr **)arena_alloc(e->arena, 4 * sizeof(Expr *));
            ca[0] = obj; ca[1] = other_arg; ca[2] = kcmp;
            ca[3] = box_synth_comparator(e, vcmp);   /* value comparator: ^fat */
            Expr *out = expr_new(e->arena, EX_CALL, TYPE_BOOL, span);
            out->as.call_.fn_binding = mek_b;
            out->as.call_.fn_expr    = NULL;
            out->as.call_.args       = ca;
            out->as.call_.n_args     = 4;
            out->as.call_.dict_arg   = NULL;
            return out;
        }
        /* Not an intercepted key type -- fall through to the generic path. */
    }

    uint8_t n_comparators = 0;
    const Symbol *helper_sym = helper_eq_symbol_for_struct(e, sd, &n_comparators);
    if (!helper_sym || (n_comparators != 1 && n_comparators != 2)) return NULL;
    Binding *helper_b = scope_lookup(&e->global, helper_sym);
    if (!helper_b) return NULL;

    /* Collect the type-arg(s) the helper's comparator(s) target.
     *
     * For 1-comparator helpers the comparator targets the OUTERMOST
     * arg of the TY_APP chain.  This matches every typed-collection
     * helper signature we currently support:
     *   Vec[A]     -> vec-eq?  with cmp for A     (only arg)
     *   Cons[A]    -> list-eq? with cmp for A     (only arg)
     *   Option[A]  -> option-eq? with cmp for A   (only arg)
     *   Map[K V]   -> map-eq?  with cmp for V     (outermost = V;
     *                                                K rides on HAMT hash)
     *
     * For 2-comparator helpers (Pair[A B], Result[A B]) the helpers
     * expect args in source order: (fst-cmp, snd-cmp) and
     * (ok-cmp, err-cmp).  TY_APP storage is innermost-first so we
     * reverse: source-order arg[0] = A (innermost in storage). */
    const Type *args_collected[2] = {0};
    uint8_t n_collected = 0;
    if (n_comparators == 1) {
        args_collected[0] = obj->type.as.app.arg;
        n_collected = 1;
    } else {
        const Type *raw[4];
        uint8_t n_raw = 0;
        for (const Type *tx = &obj->type;
             tx && tx->kind == TY_APP && n_raw < 4;
             tx = tx->as.app.fn) {
            if (tx->as.app.arg) raw[n_raw++] = tx->as.app.arg;
        }
        /* raw is innermost-first; reverse to source order. */
        for (uint8_t i = 0; i < n_raw && n_collected < 2; i++) {
            args_collected[n_collected++] = raw[n_raw - 1 - i];
        }
    }
    if (n_collected < n_comparators) return NULL;

    /* Only fire when at least one arg type is recursive (TY_APP).
     * If all the relevant args are primitive, the single-level F3-7
     * path already produces the right answer and we should not
     * intercept (the existing dispatch is potentially more
     * efficient via the static singleton vtable). */
    bool any_recursive = false;
    for (uint8_t i = 0; i < n_comparators; i++) {
        if (args_collected[i] && args_collected[i]->kind == TY_APP) {
            any_recursive = true;
            break;
        }
    }
    if (!any_recursive) return NULL;

    /* Build a comparator lambda per arg.  Elaborate them now so any
     * type errors surface before we commit to the synthesised call. */
    Expr *lambdas[2] = {0};
    for (uint8_t i = 0; i < n_comparators; i++) {
        Form *lf = build_comparator_lambda(e, args_collected[i], span,
                                           container_helper_passes_slot_words(sd->name));
        if (!lf) return NULL;
        lambdas[i] = elab_form(e, lf);
        if (!lambdas[i]) return NULL;
    }

    /* Build EX_CALL to helper with [obj, other, lambda0, ...]. */
    uint32_t total_args = 2 + (uint32_t)n_comparators;
    Expr **call_args = (Expr **)arena_alloc(e->arena, total_args * sizeof(Expr *));
    call_args[0] = obj;
    call_args[1] = other_arg;
    for (uint8_t i = 0; i < n_comparators; i++) {
        /* CRU B-3: the *-eq? carrier helpers now fat-dispatch their value/element
         * comparator(s); box each synthesized comparator so the captureless
         * lambda arrives as a fat closure rather than a bare pointer. */
        call_args[2 + i] = box_synth_comparator(e, lambdas[i]);
    }

    Expr *out = expr_new(e->arena, EX_CALL, TYPE_BOOL, span);
    out->as.call_.fn_binding = helper_b;
    out->as.call_.fn_expr    = NULL;
    out->as.call_.args       = call_args;
    out->as.call_.n_args     = total_args;
    out->as.call_.dict_arg   = NULL;
    return out;
}

/* Phase 15: Typeclasses */

/* Parse a single typeclass method definition from a Form.
 * Syntax: (method-name [param1 : type1, param2 : type2, ...] : return-type)
 * or: (method-name [param1 param2 ...] : return-type) - types inferred from usage
 */
/* Phase RT: resolve a keyword name to a class type-param index, returning the
 * matching type-param symbol (so a method return/param `:a` becomes TY_TYVAR a
 * when `a` is one of the class's type parameters). */
static const Symbol *class_type_param_match(const char *kw_name, uint32_t kw_len,
                                            const Symbol **class_type_params,
                                            uint8_t n_class_type_params) {
    for (uint8_t i = 0; i < n_class_type_params; i++) {
        const Symbol *tp = class_type_params[i];
        if (tp && tp->len == kw_len && memcmp(tp->name, kw_name, kw_len) == 0) {
            return tp;
        }
    }
    return NULL;
}

/* M7 HKT: is `sym` a candidate method-level
 * type variable -- a lowercase-leading symbol that is neither a primitive type
 * keyword, a special type-form head (fn/c-fn/void/any), nor a class type param?
 *
 * Rationale (corrected root cause, 2026-06-18): an HKT-applied method signature
 * such as `(gmap [container : (g a) f : (fn [a] b)] : (g b))` parses the HEAD
 * `g` to a NAMED TY_TYVAR (it is a class type param), but the element tyvars
 * `a`/`b` are NOT class params, so type_expr_from_form takes its "unknown ->
 * opaque struct" fallback and they become ANONYMOUS TY_STRUCT{def=NULL}.  That
 * is why `(g b)` resolves to `(type-app tyvar ?)` instead of `(type-app g b)`:
 * the element is anonymous, so layer-0 head substitution yields `(Option ?)`
 * with no named element to refine per call.  Collecting these implicit
 * method-level tyvars and threading them as additional type params makes them
 * resolve to NAMED tyvars, the prerequisite for per-call element refinement. */
static bool m7_is_method_tyvar_name(const Symbol *sym,
                                    const Symbol **class_tp, uint8_t n_class_tp) {
    if (!sym || sym->len == 0) return false;
    char c = sym->name[0];
    if (c < 'a' || c > 'z') return false;            /* tyvars are lowercase */
    if (typekind_from_symbol(sym->name) != TY_UNKNOWN) return false; /* primitive */
    if ((sym->len == 4 && memcmp(sym->name, "void", 4) == 0) ||
        (sym->len == 3 && memcmp(sym->name, "any",  3) == 0) ||
        (sym->len == 2 && memcmp(sym->name, "fn",   2) == 0) ||
        (sym->len == 4 && memcmp(sym->name, "c-fn", 4) == 0))
        return false;
    for (uint8_t i = 0; i < n_class_tp; i++)
        if (class_tp[i] == sym) return false;
    return true;
}

/* M7 HKT: walk a (type-position) form collecting de-duplicated method-level
 * tyvar symbols into `out` (capacity `max`, current count `*n_out`). */
static void m7_collect_form_tyvars(const Form *form,
                                   const Symbol **class_tp, uint8_t n_class_tp,
                                   const Symbol **out, uint8_t *n_out, uint8_t max) {
    if (!form) return;
    switch (form->tag) {
        case F_SYM:
            if (m7_is_method_tyvar_name(form->as.sym, class_tp, n_class_tp)) {
                for (uint8_t i = 0; i < *n_out; i++)
                    if (out[i] == form->as.sym) return;
                if (*n_out < max) out[(*n_out)++] = form->as.sym;
            }
            return;
        case F_TYPE_ANN:
        case F_LIST:
        case F_VEC:
            for (uint32_t i = 0; i < form->as.list.len; i++)
                m7_collect_form_tyvars(form->as.list.items[i], class_tp, n_class_tp,
                                       out, n_out, max);
            return;
        default:
            return;
    }
}

/* saffron-dyn-witness-fn-arity-defaults-unary: what an arity-less `fn`
 * class parameter means.  In typed Turmeric it is the Phase CCL poly-closure
 * carrier (`tur_poly_fn_t`, int64 in and out).  In a DYNAMIC dialect every
 * lambda is `(fn [any ..] any)` -- a tagged-value convention the int64 carrier
 * cannot speak -- so there the parameter is simply a function value that
 * arrives as `any` and is called dynamically (EX_DYN_CALL checks the arity
 * against the closure that arrived).  With the carrier, the instance body
 * called a Saffron lambda through `int64_t(*)(void*, int64_t)` and printed
 * the element's TYPE TAG (`4` for 2.5), and a binary `g` panicked at the
 * witness's unary cast. */
static void tc_fn_param_type(Span sp, Type *ty, bool *is_fn) {
    if (lang_span_is_dynamic(sp)) {
        *ty = type_simple(TY_ANY, CK_COPY);
        *is_fn = false;
    } else {
        *ty = TYPE_PTR_VOID;
        *is_fn = true;
    }
}

static TypeClassMethod *parse_typeclass_method(Elab *e, Form *method_form, Span span,
                                               uint32_t *out_body_start,
                                               const Symbol **class_type_params,
                                               uint8_t n_class_type_params,
                                               const Kind *class_type_param_kinds,
                                               const Symbol **assoc_type_names,
                                               uint8_t n_assoc_type_names) {
    if (method_form->tag != F_LIST || method_form->as.list.len < 3) {
        diag_emit(DIAG_ERROR, span,
                  "typeclass method requires (name [params...] : return-type)");
        return NULL;
    }
    
    /* Parse method name */
    Form *name_form = method_form->as.list.items[0];
    if (name_form->tag != F_SYM) {
        diag_emit(DIAG_ERROR, name_form->span,
                  "typeclass method name must be a symbol");
        return NULL;
    }
    const Symbol *name = name_form->as.sym;
    
    /* Parse parameter vector */
    Form *params_form = method_form->as.list.items[1];
    if (params_form->tag != F_VEC) {
        diag_emit(DIAG_ERROR, params_form->span,
                  "typeclass method parameter list must be a vector");
        return NULL;
    }
    
    /* M7 HKT (flag-gated): build the effective type-param array used for
     * type-expression resolution = the class type params plus any implicit
     * method-level tyvars collected from the param/return type forms.  When the
     * flag is OFF this is identical to (class_type_params, n_class_type_params),
     * so the path is byte-for-byte inert.  See m7_is_method_tyvar_name. */
    const Symbol **eff_tp = class_type_params;
    uint8_t n_eff_tp = n_class_type_params;
    /* Thread the class type-param kinds through to type-expression resolution so
     * an HKT param `^m` (kind `* -> *`) used in an APPLIED position inside a
     * method signature -- e.g. `bind`'s `k : (fn [a] (m b))` -- resolves to a
     * TY_TYVAR carrying KIND_ARROW, not the KIND_STAR default.  Without this,
     * `(m b)` reconstructed by call_instantiate_type at the instance call site
     * trips type_app's kind check (TUR-E0012).  The fmap shape (fn returns a
     * bare element `b`) never builds `(m _)` so it was unaffected; only the
     * monadic shapes (fn returning an applied HKT type) hit it.  See
     * docs/archive/history/m7-hkt-fn-returning-applied-type-kind-mismatch.md. */
    Kind *eff_kinds = (Kind *)class_type_param_kinds;
    /* M7: collecting method-level element tyvars is a PARSE concern.  A typed
     * HKT class signature (`(foldr [ta : (t a) ...] : b)`) parses and the
     * by-value emit paths pick up the element types.  Inert for existing
     * classes whose method signatures contain no method-level tyvars (eff_tp ==
     * class params). */
    {
        enum { M7_MAX_TP = 16 };
        const Symbol **buf =
            (const Symbol **)arena_alloc(e->arena, M7_MAX_TP * sizeof(const Symbol *));
        Kind *kbuf = (Kind *)arena_alloc(e->arena, M7_MAX_TP * sizeof(Kind));
        uint8_t n = 0;
        for (uint8_t i = 0; i < n_class_type_params && n < M7_MAX_TP; i++) {
            kbuf[n] = class_type_param_kinds ? class_type_param_kinds[i] : KIND_STAR;
            buf[n] = class_type_params[i];
            n++;
        }
        uint8_t n_class_in_buf = n;
        m7_collect_form_tyvars(params_form, class_type_params, n_class_type_params,
                               buf, &n, M7_MAX_TP);
        /* Locate the return form (index 2, or 3 when an effect-row #{...}
         * precedes it) and collect its element tyvars too. */
        {
            uint32_t ri = 2;
            if (method_form->as.list.len > ri &&
                method_form->as.list.items[ri]->tag == F_MAP) ri++;
            if (method_form->as.list.len > ri)
                m7_collect_form_tyvars(method_form->as.list.items[ri],
                                       class_type_params, n_class_type_params,
                                       buf, &n, M7_MAX_TP);
        }
        /* The method-level tyvars appended after the class params are ordinary
         * element variables of kind `*`. */
        for (uint8_t i = n_class_in_buf; i < n; i++) kbuf[i] = KIND_STAR;
        eff_tp = buf;
        eff_kinds = kbuf;
        n_eff_tp = n;
    }

    /* Parse parameters */
    uint8_t n_params = params_form->as.list.len;
    const Symbol **param_names = NULL;
    Type *param_types = NULL;
    /* Phase CCL: callable-param flag array — parallel to param_names/param_types.
     * param_is_fn[i] = true when the i-th param is declared with [name :fn] syntax,
     * marking it as a single-argument callable that should receive tur_poly_fn_t. */
    bool *param_is_fn = NULL;
    /* Prereq 4: per-param flag tracking whether the user wrote an explicit type
     * annotation. Without this, the elaborator can't tell `[v : int]` (explicit)
     * from `[v]` (default to int) -- both become TY_INT in param_types. The
     * substitution site at elab_definstance consults this flag to leave
     * explicit-`:int` params alone instead of rewriting them to the class tyvar. */
    bool *param_explicit_type = NULL;
    const Form **param_refine_preds = NULL;
    const char **param_refine_vars  = NULL;

    if (n_params > 0) {
        param_names = (const Symbol **)arena_alloc(e->arena, n_params * sizeof(const Symbol *));
        param_types = (Type *)arena_alloc(e->arena, n_params * sizeof(Type));
        param_is_fn = (bool *)arena_alloc(e->arena, n_params * sizeof(bool));
        param_explicit_type = (bool *)arena_alloc(e->arena, n_params * sizeof(bool));
        param_refine_preds = (const Form **)arena_alloc(e->arena, n_params * sizeof(Form *));
        param_refine_vars  = (const char **)arena_alloc(e->arena, n_params * sizeof(char *));
        for (uint8_t _i = 0; _i < n_params; _i++) {
            param_refine_preds[_i] = NULL;
            param_refine_vars[_i]  = NULL;
        }
        for (uint32_t i = 0; i < n_params; i++) {
            param_is_fn[i] = false;
            param_explicit_type[i] = false;
        }

        /* actual_p: number of real parameters encountered (keywords don't count). */
        uint8_t actual_p = 0;
        for (uint32_t i = 0; i < n_params; i++) {
            Form *p = params_form->as.list.items[i];
            /* ECS E2d-P6 (Issue 2 secondary): substructural / borrow caret
             * markers (^borrow, ^mut, ^unique, ^linear, ^affine, ^relevant,
             * ^fat) annotate the *next* parameter; they are not parameters
             * themselves.  Skip them so they do not consume a param slot --
             * otherwise `[^borrow s : S idx : int val : E]` mis-aligned the
             * parsed param types (treating `^borrow` as a param), so an
             * instance method inheriting those types saw the wrong type per
             * position.  (The borrow discipline itself is enforced on the
             * elaborated instance-method FnDefs and at call sites.) */
            if (p->tag == F_SYM &&
                (p->as.sym == e->sym_caret_borrow ||
                 p->as.sym == e->sym_caret_mut ||
                 p->as.sym == e->sym_caret_unique ||
                 p->as.sym == e->sym_caret_linear ||
                 p->as.sym == e->sym_caret_affine ||
                 p->as.sym == e->sym_caret_relevant ||
                 p->as.sym == e->sym_caret_fat)) {
                continue;
            }
            if (p->tag == F_SYM) {
                param_names[actual_p] = p->as.sym;
                /* Default to int for now - type inference for method params deferred */
                param_types[actual_p] = TYPE_INT;
                param_is_fn[actual_p] = false;
                actual_p++;
            } else if (p->tag == F_KEYWORD) {
                /* Inline type annotation for the previous parameter:
                 * e.g. [b :ptr<void>] where :ptr<void> annotates b */
                if (actual_p == 0) {
                    diag_emit(DIAG_ERROR, p->span,
                              "type annotation without preceding parameter");
                    return NULL;
                }
                uint8_t prev = actual_p - 1;
                /* Prereq 4: any explicit annotation -- :int, :bool, :cstr,
                 * :ptr<void>, :fn, or a class tyvar -- pins the param type
                 * and the elaborator must not rewrite it later. */
                param_explicit_type[prev] = true;
                const Symbol *kw = p->as.sym;
                if (kw->len == 3 && memcmp(kw->name, "int", 3) == 0) {
                    param_types[prev] = TYPE_INT;
                } else if (kw->len == 4 && memcmp(kw->name, "bool", 4) == 0) {
                    param_types[prev] = TYPE_BOOL;
                } else if (kw->len == 4 && memcmp(kw->name, "cstr", 4) == 0) {
                    param_types[prev] = TYPE_CSTR;
                } else if ((kw->len == 9 && memcmp(kw->name, "ptr<void>", 9) == 0) ||
                           (kw->len == 3 && memcmp(kw->name, "ptr", 3) == 0)) {
                    param_types[prev] = TYPE_PTR_VOID;
                } else if (kw->len == 2 && memcmp(kw->name, "fn", 2) == 0) {
                    tc_fn_param_type(p->span, &param_types[prev], &param_is_fn[prev]);
                } else {
                    /* Phase RT: a parameter typed `:a` naming a class type
                     * parameter becomes a TY_TYVAR -- this is the dispatch
                     * witness (e.g. schema-of [_ :a]). */
                    const Symbol *tp = class_type_param_match(kw->name, kw->len,
                                                              class_type_params,
                                                              n_class_type_params);
                    if (tp) {
                        param_types[prev] = type_tyvar_named(tp->name);
                    } else {
                        /* ECS E2d-P6 (Issue 1): a parameter typed by an associated
                         * type member (e.g. `val :Elem`) is an abstract projection
                         * resolved per instance; carry it as a named TY_TYVAR. */
                        const Symbol *am = class_type_param_match(kw->name, kw->len,
                                                                  assoc_type_names,
                                                                  n_assoc_type_names);
                        if (am) {
                            param_types[prev] = type_tyvar_named(am->name);
                        } else {
                            diag_emit(DIAG_ERROR, p->span,
                                      "unsupported type in typeclass method parameter");
                            return NULL;
                        }
                    }
                }
            } else if (p->tag == F_TYPE_ANN) {
                if (actual_p == 0) {
                    diag_emit(DIAG_ERROR, p->span,
                              "type annotation without preceding parameter");
                    return NULL;
                }
                /* Prereq 4: same as the F_KEYWORD branch -- an explicit
                 * spaced `: type` pins the param type. */
                param_explicit_type[actual_p - 1] = true;
                /* Phase CCL: `: fn` (F_TYPE_ANN wrapping F_SYM("fn")) is the
                 * spaced form of `:fn` -- the poly-closure carrier marker. */
                Form *inner_f = (p->as.list.len > 0) ? p->as.list.items[0] : NULL;
                if (inner_f &&
                    (inner_f->tag == F_SYM || inner_f->tag == F_KEYWORD) &&
                    inner_f->as.sym->len == 2 &&
                    memcmp(inner_f->as.sym->name, "fn", 2) == 0) {
                    tc_fn_param_type(p->span, &param_types[actual_p - 1],
                                     &param_is_fn[actual_p - 1]);
                } else if (inner_f &&
                           (inner_f->tag == F_SYM || inner_f->tag == F_KEYWORD) &&
                           class_type_param_match(inner_f->as.sym->name,
                                                  inner_f->as.sym->len,
                                                  assoc_type_names,
                                                  n_assoc_type_names)) {
                    /* ECS E2d-P6 (Issue 1): a spaced `: Elem` naming an associated
                     * type member is an abstract projection; carry it as a named
                     * TY_TYVAR (resolved per instance, like the keyword form). */
                    const Symbol *am = class_type_param_match(inner_f->as.sym->name,
                                                              inner_f->as.sym->len,
                                                              assoc_type_names,
                                                              n_assoc_type_names);
                    param_types[actual_p - 1] = type_tyvar_named(am->name);
                } else {
                    /* ECS E2d-P6 (Issue 2): resolve class type parameters used in
                     * a spaced `: S` (or parametric `: (Dense S)`) parameter
                     * annotation to a named TY_TYVAR.  Without the class type
                     * params here, `S` was parsed as an opaque/unknown type and
                     * the return-only-dispatch detector could not see that S
                     * appears in argument position, mis-classifying an
                     * argument-dispatchable method as return-only. */
                    Type *ft = inner_f
                        ? type_expr_from_form(e, inner_f, NULL,
                                              eff_tp, eff_kinds,
                                              n_eff_tp)
                        : NULL;
                    if (!ft) {
                        diag_emit(DIAG_ERROR, p->span,
                                  "unsupported type in typeclass method parameter");
                        return NULL;
                    }
                    /* CT0/RT1: a `k : #refine{...}` class parameter -- peel to
                     * the base type and remember the predicate.  This is the
                     * branch the spaced-colon form actually takes; the two
                     * nested-vector sites below handle `[k : T]`. */
                    param_types[actual_p - 1] = *rt_peel_contract(
                        ft, &param_refine_preds[actual_p - 1],
                        &param_refine_vars[actual_p - 1]);
                }
            } else if (p->tag == F_VEC && p->as.list.len >= 2) {
                /* [name : type] or [name :fn] nested vector syntax */
                Form *name_f = p->as.list.items[0];
                Form *type_f = p->as.list.items[1];
                if (name_f->tag != F_SYM) {
                    diag_emit(DIAG_ERROR, name_f->span,
                              "parameter name must be a symbol");
                    return NULL;
                }
                param_names[actual_p] = name_f->as.sym;
                param_is_fn[actual_p] = false;
                /* Parse type annotation */
                if (type_f->tag == F_KEYWORD) {
                    const Symbol *kw = type_f->as.sym;
                    if (kw->len == 3 && memcmp(kw->name, "int", 3) == 0) {
                        param_types[actual_p] = TYPE_INT;
                    } else if (kw->len == 4 && memcmp(kw->name, "bool", 4) == 0) {
                        param_types[actual_p] = TYPE_BOOL;
                    } else if (kw->len == 4 && memcmp(kw->name, "cstr", 4) == 0) {
                        param_types[actual_p] = TYPE_CSTR;
                    } else if ((kw->len == 9 && memcmp(kw->name, "ptr<void>", 9) == 0) ||
                               (kw->len == 3 && memcmp(kw->name, "ptr", 3) == 0)) {
                        param_types[actual_p] = TYPE_PTR_VOID;
                    } else if (kw->len == 2 && memcmp(kw->name, "fn", 2) == 0) {
                        /* Phase CCL: :fn marks this param as a single-argument
                         * callable; it will be passed as tur_poly_fn_t at call
                         * sites so that capturing closures work transparently. */
                        tc_fn_param_type(type_f->span, &param_types[actual_p],
                                         &param_is_fn[actual_p]);
                    } else {
                        diag_emit(DIAG_ERROR, type_f->span,
                                  "unsupported type in typeclass method parameter");
                        return NULL;
                    }
                } else if (type_f->tag == F_TYPE_ANN) {
                    /* `: type-expr` compound annotation; special-case `: fn`
                     * (the poly-closure carrier) before falling through to the
                     * generic type-expression parser. */
                    Form *ti = (type_f->as.list.len > 0) ? type_f->as.list.items[0] : NULL;
                    if (ti &&
                        (ti->tag == F_SYM || ti->tag == F_KEYWORD) &&
                        ti->as.sym->len == 2 &&
                        memcmp(ti->as.sym->name, "fn", 2) == 0) {
                        tc_fn_param_type(type_f->span, &param_types[actual_p],
                                         &param_is_fn[actual_p]);
                    } else {
                        Type *ft = ti
                            ? type_expr_from_form(e, ti, NULL,
                                                  eff_tp, eff_kinds,
                                                  n_eff_tp)
                            : NULL;
                        if (!ft) {
                            diag_emit(DIAG_ERROR, type_f->span,
                                      "unsupported type form in typeclass method parameter");
                            return NULL;
                        }
                        param_types[actual_p] = *rt_peel_contract(
                            ft, &param_refine_preds[actual_p],
                            &param_refine_vars[actual_p]);
                    }
                } else if (type_f->tag == F_LIST || type_f->tag == F_VEC) {
                    /* Phase HRT3: allow forall/exists type forms as parameter types */
                    Type *ft = type_expr_from_form(e, type_f, NULL,
                                                   eff_tp, eff_kinds,
                                                   n_eff_tp);
                    if (!ft) {
                        diag_emit(DIAG_ERROR, type_f->span,
                                  "unsupported type form in typeclass method parameter");
                        return NULL;
                    }
                    param_types[actual_p] = *rt_peel_contract(
                        ft, &param_refine_preds[actual_p],
                        &param_refine_vars[actual_p]);
                } else {
                    param_types[actual_p] = TYPE_INT; /* default */
                }
                actual_p++;
            } else {
                diag_emit(DIAG_ERROR, p->span,
                          "parameter must be a symbol or [name : type] vector");
                return NULL;
            }
        }
        n_params = actual_p;
    }
    
    /* Parse return type - must be after params */
    /* Syntax: (method [params] : return-type)
     *      or (method [params] : #{Effect...} return-type)  -- effect row annotation
     *      or (method [params] #{Effect...} : return-type)  -- effect row annotation alt
     *
     * ER3: #{...} (F_MAP) in return-type position is now parsed and stored on
     * the method so effect_check_pass can enforce it against instance method bodies. */
    EffectRow *method_effect_row = NULL;
    Type return_type = TYPE_NIL;
    const Form *return_refine_pred = NULL;   /* RT1: `: #refine{ r : T | q }` */
    const char *return_refine_var  = NULL;
    uint32_t ret_idx = 2;   /* first element after params vector */
    if (method_form->as.list.len > ret_idx) {
        Form *maybe_row = method_form->as.list.items[ret_idx];
        if (maybe_row->tag == F_MAP) {
            /* #{Effect...} effect-row annotation -- parse and store it. */
            warn_legacy_fx_row(maybe_row);
            uint8_t n_sym = (uint8_t)maybe_row->as.list.len;
            const Symbol **syms = (const Symbol **)arena_alloc(e->arena,
                                    (n_sym ? n_sym : 1) * sizeof(Symbol *));
            uint8_t n_valid = 0;
            for (uint32_t j = 0; j < maybe_row->as.list.len; j++) {
                Form *item = maybe_row->as.list.items[j];
                if (item->tag == F_SYM) {
                    syms[n_valid++] = item->as.sym;
                }
            }
            method_effect_row = effect_row_unresolved(e->arena, syms, n_valid,
                                                      maybe_row->span);
            ret_idx++;
        }
    }
    if (method_form->as.list.len > ret_idx) {
        Form *ret_form = method_form->as.list.items[ret_idx];
        /* Spaced `: T` over a single symbol/keyword: normalise to F_KEYWORD
         * so the class-type-param match below runs. */
        if (ret_form->tag == F_TYPE_ANN && ret_form->as.list.len == 1 &&
            (ret_form->as.list.items[0]->tag == F_KEYWORD ||
             ret_form->as.list.items[0]->tag == F_SYM)) {
            Form *inner = ret_form->as.list.items[0];
            Form *kf = (Form *)arena_alloc(e->arena, sizeof(Form));
            *kf = *inner;
            kf->tag = F_KEYWORD;
            ret_form = kf;
        }
        if (ret_form->tag == F_MAP) {
            /* another effect row or #{} after the params — skip silently */
            warn_legacy_fx_row(ret_form);
            /* (ignore the rest; return type stays TYPE_NIL) */
        } else if (ret_form->tag == F_KEYWORD) {
            const Symbol *kw = ret_form->as.sym;
            if (kw->len == 3 && memcmp(kw->name, "int", 3) == 0) {
                return_type = TYPE_INT;
            } else if (kw->len == 4 && memcmp(kw->name, "bool", 4) == 0) {
                return_type = TYPE_BOOL;
            } else if (kw->len == 4 && memcmp(kw->name, "cstr", 4) == 0) {
                return_type = TYPE_CSTR;
            } else if ((kw->len == 4 && memcmp(kw->name, "void", 4) == 0) ||
                       (kw->len == 3 && memcmp(kw->name, "nil", 3) == 0)) {
                return_type = TYPE_NIL;
            } else if ((kw->len == 9 && memcmp(kw->name, "ptr<void>", 9) == 0) ||
                       (kw->len == 3 && memcmp(kw->name, "ptr", 3) == 0)) {
                return_type = TYPE_PTR_VOID;
            } else {
                /* Phase RT: a return type `:a` naming a class type parameter
                 * becomes a TY_TYVAR.  This is the return-only dispatch case:
                 * the dispatch variable appears only in the result, so the
                 * instance must be selected from the call's expected type.
                 * M7 HKT (flag-gated): match against the EFFECTIVE type params
                 * (class params + method-level element tyvars) so a bare
                 * element return `: a` -- the Comonad `extract [w : (f a)] : a`
                 * / Foldable shape -- resolves to a named TY_TYVAR instead of
                 * erroring.  eff_tp == class_type_params when the flag is off,
                 * so this is byte-for-byte inert flag-off. */
                const Symbol *tp = class_type_param_match(kw->name, kw->len,
                                                          eff_tp, n_eff_tp);
                if (tp) {
                    return_type = type_tyvar_named(tp->name);
                } else {
                    /* ECS E2d-P6 (Issue 1): a return type naming an associated
                     * type member (e.g. `(type Elem : Type)` -> `: Elem`) is an
                     * abstract projection over the instance type, resolved per
                     * instance.  Represent it as a named TY_TYVAR carrying the
                     * associated-type name; elab_definstance substitutes it with
                     * the instance's `(type Elem = T)` binding, and a call site
                     * recovers the concrete result from the dispatched instance
                     * method (argument dispatch keys on the receiver type). */
                    const Symbol *am = class_type_param_match(kw->name, kw->len,
                                                              assoc_type_names,
                                                              n_assoc_type_names);
                    if (am) {
                        return_type = type_tyvar_named(am->name);
                    } else {
                        /* Bare nominal return type: a single-symbol `: T` naming
                         * an ordinary user-defined type (defopaque newtype or
                         * defstruct) is neither a builtin keyword, a class type
                         * param, nor an associated-type name -- but it is still a
                         * legitimate return type.  The applied-form path
                         * (`(Result T cstr)`, `(Vec T)`) already routes through
                         * type_expr_from_form; route the bare symbol through the
                         * same resolver before giving up.  Synthesize an F_SYM
                         * form (ret_form was normalized to F_KEYWORD above) so the
                         * nominal-type lookup runs. */
                        Form *sym_f = (Form *)arena_alloc(e->arena, sizeof(Form));
                        *sym_f = *ret_form;
                        sym_f->tag = F_SYM;
                        Type *ft = type_expr_from_form(e, sym_f, NULL,
                                                       eff_tp, eff_kinds,
                                                       n_eff_tp);
                        if (ft) {
                            return_type = *ft;
                        } else {
                            diag_emit(DIAG_ERROR, ret_form->span,
                                      "unsupported return type in typeclass method");
                            return NULL;
                        }
                    }
                }
            }
        } else if (ret_form->tag == F_TYPE_ANN) {
            /* `: type-expr` compound return type annotation */
            /* Prereq 5: pass the class's type parameters through so the return
             * type's `a` inside a parameterized form like `(Result a cstr)`
             * resolves to TY_TYVAR rather than an undefined opaque struct.
             * Without this, return-dispatch detection works (rt_type_mentions_tyvar
             * recurses through TY_APP) but the call-site unification can't extract
             * the `a`-position binding, so ascriptions like
             * `(:: (decode ...) (Result cstr cstr))` silently fall back to the
             * first instance. (Bare-`a` return type happens to work because the
             * raw `a` symbol resolves via the class_type_param_match path used
             * elsewhere in this function, not via type_expr_from_form.) */
            Type *ft = (ret_form->as.list.len > 0)
                ? type_expr_from_form(e, ret_form->as.list.items[0], NULL,
                                      eff_tp, eff_kinds,
                                      n_eff_tp)
                : NULL;
            if (!ft) {
                diag_emit(DIAG_ERROR, ret_form->span,
                          "unsupported return type form in typeclass method");
                return NULL;
            }
            /* RT1: `: #refine{ r : T | q }` on a class method result.  Peel to
             * the base type -- otherwise the contract type reaches codegen and
             * the method does not compile -- and KEEP the predicate, which is
             * the class's promise to callers.  Dropping it silently (which is
             * what happened before) produced a class signature that read like a
             * guarantee and enforced nothing. */
            return_type = *rt_peel_contract(ft, &return_refine_pred,
                                            &return_refine_var);
        } else if (ret_form->tag == F_LIST || ret_form->tag == F_VEC) {
            /* Phase HRT3: allow forall/exists type forms as return types.
             * Prereq 5: same as above -- pass class type params so a
             * nested `a` resolves to TY_TYVAR. */
            Type *ft = type_expr_from_form(e, ret_form, NULL,
                                           eff_tp, eff_kinds,
                                           n_eff_tp);
            if (!ft) {
                diag_emit(DIAG_ERROR, ret_form->span,
                          "unsupported return type form in typeclass method");
                return NULL;
            }
            return_type = *ft;
        } else {
            diag_emit(DIAG_ERROR, ret_form->span,
                      "typeclass method return type must be a keyword like :int");
            return NULL;
        }
    }
    
    /* ER3: Report where body forms start so elab_defclass can elaborate defaults.
     * body_start_idx is the index of the first form after the return type (or
     * method_form->as.list.len if there are no body forms). */
    uint32_t body_start_idx = ret_idx + 1;
    if (out_body_start) *out_body_start = body_start_idx;

    TypeClassMethod *method = (TypeClassMethod *)arena_alloc(e->arena, sizeof(TypeClassMethod));
    /* arena_alloc does not zero.  Zero the whole struct before the field
     * assignments so any member NOT set below (refine_class_binding was the
     * one that bit: the RT1 memo slot read junk from a recycled slab and a
     * later dynamic dispatch dereferenced it -- the refined multi-compile
     * SIGSEGV) -- and any field added later -- starts NULL instead of
     * whatever the recycled slab held. */
    memset(method, 0, sizeof(*method));
    method->name = name;
    method->param_names = param_names;
    method->param_types = param_types;
    method->param_is_fn = param_is_fn;
    method->param_refine_preds = param_refine_preds;
    method->param_refine_vars  = param_refine_vars;
    method->param_explicit_type = param_explicit_type;
    method->n_params = n_params;
    method->return_type = return_type;
    method->return_refine_pred = return_refine_pred;
    method->return_refine_var  = return_refine_var;
    method->effect_row = method_effect_row;  /* ER3: NULL if not annotated */
    method->default_fn_expr = NULL;          /* ER3: set by elab_defclass if body forms exist */
    method->default_method_form = NULL;      /* set by elab_defclass if body forms exist */
    return method;
}

/* Compare a freshly-parsed typeclass against an existing entry of the same
 * name. Returns true iff all observable signature surface matches: same
 * type-parameter count and kinds, same method count, and per-method the same
 * name, parameter count, parameter type-kinds, and return type-kind. Used to
 * accept idempotent stdlib pre-declarations while rejecting genuine
 * redefinitions. */
static bool typeclass_signatures_match(const TypeClass *existing,
                                       uint8_t n_type_params,
                                       const Kind *type_param_kinds,
                                       uint8_t n_methods,
                                       const TypeClassMethod *methods,
                                       uint8_t n_supers,
                                       const Form **super_forms,
                                       const uint8_t *super_n_args,
                                       const uint8_t *super_arg_idx) {
    if (existing->n_type_params != n_type_params) return false;
    if (existing->n_methods     != n_methods)     return false;
    /* class-superclasses SC1: the preamble is signature surface too.  Without
     * this a class seen through two import paths with differing preambles
     * would silently keep the first. */
    if (existing->n_supers != n_supers) return false;
    for (uint8_t i = 0; i < n_supers; i++) {
        const Form *ef = existing->super_forms ? existing->super_forms[i] : NULL;
        const Form *nf = super_forms[i];
        if (!ef || !nf) return false;
        if (ef->as.list.items[0]->as.sym != nf->as.list.items[0]->as.sym) return false;
        if (existing->super_n_args[i] != super_n_args[i]) return false;
        for (uint8_t k = 0; k < super_n_args[i]; k++)
            if (existing->super_arg_idx[i * TUR_SUPER_MAX_ARGS + k] !=
                super_arg_idx[i * TUR_SUPER_MAX_ARGS + k]) return false;
    }
    for (uint8_t i = 0; i < n_type_params; i++) {
        Kind a = existing->type_param_kinds ? existing->type_param_kinds[i] : KIND_STAR;
        Kind b = type_param_kinds          ? type_param_kinds[i]          : KIND_STAR;
        if (a != b) return false;
    }
    for (uint8_t i = 0; i < n_methods; i++) {
        const TypeClassMethod *em = &existing->methods[i];
        const TypeClassMethod *nm = &methods[i];
        if (em->name != nm->name) return false; /* symbols interned -- ptr eq */
        if (em->n_params != nm->n_params) return false;
        if (em->return_type.kind != nm->return_type.kind) return false;
        for (uint32_t j = 0; j < em->n_params; j++) {
            if (em->param_types[j].kind != nm->param_types[j].kind) return false;
        }
    }
    return true;
}

/* Elaborate (defclass Name [type-params...] (method1 ...) (method2 ...) ...)
 *
 * Defines a new typeclass with type parameters and methods.
 * Syntax: (defclass Eq [a] (eq? [x : a, y : a] : bool))
 *         (defclass Show [a] (show [x : a] : cstr))
 */

/* typeclass-default-methods-do-not-work: does a default body mention one of
 * the class's own methods -- `(.lt? x y)`, or bare `(lt? x y)`?  Such a body can
 * only be elaborated per instance. */
static bool form_mentions_sibling(const Form *f, const TypeClassMethod *methods,
                                  uint32_t n_methods) {
    if (!f) return false;
    if (f->tag == F_SYM && f->as.sym && f->as.sym->name) {
        const char *nm = f->as.sym->name;
        const char *bare = (nm[0] == '.') ? nm + 1 : nm;
        for (uint32_t i = 0; i < n_methods; i++)
            if (methods[i].name && strcmp(methods[i].name->name, bare) == 0) return true;
        return false;
    }
    if (f->tag == F_LIST || f->tag == F_VEC) {
        for (uint32_t i = 0; i < f->as.list.len; i++)
            if (form_mentions_sibling(f->as.list.items[i], methods, n_methods)) return true;
    }
    return false;
}
static bool default_body_mentions_sibling(const Form *method_form, uint32_t body_start,
                                          const TypeClassMethod *methods, uint32_t n_methods) {
    for (uint32_t k = body_start; k < method_form->as.list.len; k++)
        if (form_mentions_sibling(method_form->as.list.items[k], methods, n_methods)) return true;
    return false;
}

Expr *elab_defclass(Elab *e, const Form *call) {
    /* Minimum: (defclass Name) */
    if (call->as.list.len < 2) {
        diag_emit(DIAG_ERROR, call->span,
                  "defclass requires a name: (defclass Name [...])");
        return NULL;
    }
    
    /* Parse typeclass name */
    Form *name_form = call->as.list.items[1];
    if (name_form->tag != F_SYM) {
        diag_emit(DIAG_ERROR, name_form->span,
                  "defclass name must be a symbol");
        return NULL;
    }
    const Symbol *name = name_form->as.sym;

    /* Phase HKT H3: Functor, Applicative, Monad, Traversable, Foldable are now
     * defined (in stdlib/typeclass.tur), not reserved.  The only guard remaining
     * is the standard "already defined" check below. */

    /* Check if already defined. The decision (skip silently vs hard-error) is
     * deferred until after the new defclass is parsed so we can compare
     * signatures: identical signature = idempotent silent skip (preserves the
     * stdlib pre-declaration of Eq, Functor, etc.); different signature =
     * "typeclass 'Foo' is already defined" diagnostic. See
     * typeclass_signatures_match above. */
    TypeClass *existing = typeclass_env_lookup_typeclass(&e->typeclass_env, name);
    
    /* Parse type parameters (optional) */
    const Symbol **type_params = NULL;
    Kind         *type_param_kinds = NULL;
    uint8_t n_type_params = 0;
    uint32_t methods_start = 2;

    if (call->as.list.len >= 3) {
        Form *params_form = call->as.list.items[2];
        if (params_form->tag == F_VEC) {
            n_type_params = params_form->as.list.len;
            if (n_type_params > 0) {
                type_params = (const Symbol **)arena_alloc(e->arena,
                    n_type_params * sizeof(const Symbol *));
                /* Phase PTC2: Explicitly initialize all type_param_kinds to KIND_STAR.
                 * Note: arena_alloc does NOT zero memory, contrary to the old comment. */
                type_param_kinds = (Kind *)arena_alloc(e->arena,
                    n_type_params * sizeof(Kind));
                for (uint8_t i = 0; i < n_type_params; i++) {
                    type_param_kinds[i] = KIND_STAR;  /* Default kind for all params */
                }
                
                for (uint8_t i = 0; i < n_type_params; i++) {
                    Form *p = params_form->as.list.items[i];
                    /* Phase HKT H1: [f :kind] vector form — lowered to the same
                     * internal representation as '^f' (KIND_ARROW) or '^^f' (KIND_ARROW2).
                     * Accepted forms: [f :kind] and [f :kind2]. */
                    if (p->tag == F_VEC) {
                        if (p->as.list.len != 2 ||
                            p->as.list.items[0]->tag != F_SYM ||
                            p->as.list.items[1]->tag != F_KEYWORD) {
                            diag_emit(DIAG_ERROR, p->span,
                                      "[f :kind] form requires exactly two elements: "
                                      "a symbol and a kind keyword (:kind or :kind2)");
                            return NULL;
                        }
                        const Symbol *bare = p->as.list.items[0]->as.sym;
                        const char   *kw   = p->as.list.items[1]->as.sym->name;
                        Kind          kt;
                        if (strcmp(kw, "kind2") == 0) {
                            kt = KIND_ARROW2;
                        } else if (strcmp(kw, "kind") == 0) {
                            kt = KIND_ARROW;
                        } else {
                            diag_emit(DIAG_ERROR, p->span,
                                      "unknown kind keyword ':%s'; expected :kind or :kind2",
                                      kw);
                            return NULL;
                        }
                        if (bare->len == 0 || bare->name[0] < 'a' || bare->name[0] > 'z') {
                            diag_emit(DIAG_ERROR, p->span,
                                      "'%s' is not a valid type parameter; "
                                      "use a lowercase name in [name :kind]",
                                      bare->name);
                            return NULL;
                        }
                        type_params[i]      = bare;
                        type_param_kinds[i] = kt;
                        continue;
                    }
                    if (p->tag != F_SYM) {
                        diag_emit(DIAG_ERROR, p->span,
                                  "type parameter must be a symbol");
                        return NULL;
                    }
                    /* Phase HKT H1: '^name' prefix marks a kind * -> * (type constructor)
                     * parameter.  The canonical name stored is 'name' (without '^').
                     * Phase HKT H5: '^^name' prefix marks a kind * -> * -> * (binary
                     * type constructor) parameter. */
                    if (p->as.sym->len > 2 && p->as.sym->name[0] == '^' && p->as.sym->name[1] == '^') {
                        const char  *bare     = p->as.sym->name + 2;
                        uint32_t     bare_len = p->as.sym->len  - 2;
                        /* Only lowercase-leading names are kind variables. */
                        if (bare_len > 0 && bare[0] >= 'a' && bare[0] <= 'z') {
                            type_params[i]      = symtab_intern(e->st, strslice(bare, bare_len));
                            type_param_kinds[i] = KIND_ARROW2;
                        } else {
                            diag_emit(DIAG_ERROR, p->span,
                                      "'%s' is not a valid type parameter; "
                                      "use lowercase '^^name' for a kind '* -> * -> *' parameter",
                                      p->as.sym->name);
                            return NULL;
                        }
                    } else if (p->as.sym->len > 1 && p->as.sym->name[0] == '^') {
                        const char  *bare     = p->as.sym->name + 1;
                        uint32_t     bare_len = p->as.sym->len  - 1;
                        /* Only lowercase-leading names are kind variables. */
                        if (bare_len > 0 && bare[0] >= 'a' && bare[0] <= 'z') {
                            type_params[i]      = symtab_intern(e->st, strslice(bare, bare_len));
                            type_param_kinds[i] = KIND_ARROW;
                        } else {
                            /* Uppercase — treat as a constraint annotation in wrong place. */
                            diag_emit(DIAG_ERROR, p->span,
                                      "'%s' is not a valid type parameter; "
                                      "use lowercase '^name' for a kind '* -> *' parameter",
                                      p->as.sym->name);
                            return NULL;
                        }
                    } else {
                        type_params[i]      = p->as.sym;
                        type_param_kinds[i] = KIND_STAR;
                    }
                }
            }
            methods_start = 3;
        }
    }

    /* class-superclasses (docs/archive/typeclass-superclasses-plan.md, SC1):
     * optional constraint preamble `[(Super var...) ...]` right after the
     * type-param vector -- the same bracketed `(Class var)` vector `definstance`
     * and `defn` already spell constraints with, and in the same position
     * `defn` puts it (`(defn f [W] [(Foo W)] [params] ...)`).  It is
     * distinguished from what may follow by form tag alone: F_VEC is the
     * preamble, the bare symbol `|` is the fundep clause, F_LIST is a method,
     * so every existing defclass parses exactly as before.
     *
     * The elements are STORED, not resolved: a superclass may be declared
     * below its subclass, and the post-unit pass (elab_typeclass_superclasses_
     * finish) resolves, cycle-checks, and enforces the instance obligation once
     * every form in the unit is registered. */
    const Form **super_forms   = NULL;
    uint8_t     *super_n_args  = NULL;
    uint8_t     *super_arg_idx = NULL;
    uint8_t      n_supers      = 0;
    if (methods_start < call->as.list.len &&
        call->as.list.items[methods_start]->tag == F_VEC) {
        Form *sv = call->as.list.items[methods_start];
        if (sv->as.list.len == 0) {
            diag_emit_with_code(DIAG_ERROR, sv->span,
                TUR_E0390_CLASS_SUPERCLASS_PREAMBLE,
                "defclass '%s': empty superclass constraint vector; "
                "list at least one (Class var), or drop the vector",
                name->name);
            return NULL;
        }
        n_supers      = (uint8_t)sv->as.list.len;
        super_forms   = (const Form **)arena_alloc(e->arena, n_supers * sizeof(const Form *));
        super_n_args  = (uint8_t *)arena_alloc(e->arena, n_supers);
        super_arg_idx = (uint8_t *)arena_alloc(e->arena, n_supers * TUR_SUPER_MAX_ARGS);
        for (uint8_t i = 0; i < n_supers; i++) {
            Form *el = sv->as.list.items[i];
            if (el->tag != F_LIST || el->as.list.len < 2 ||
                el->as.list.items[0]->tag != F_SYM) {
                diag_emit_with_code(DIAG_ERROR, el->span,
                    TUR_E0390_CLASS_SUPERCLASS_PREAMBLE,
                    "defclass '%s': superclass constraint must be (Class var...), "
                    "e.g. [(Semigroup a)]", name->name);
                return NULL;
            }
            if (el->as.list.len - 1 > TUR_SUPER_MAX_ARGS) {
                diag_emit_with_code(DIAG_ERROR, el->span,
                    TUR_E0390_CLASS_SUPERCLASS_PREAMBLE,
                    "defclass '%s': superclass constraint names %u type "
                    "variables; at most %d are supported",
                    name->name, (unsigned)(el->as.list.len - 1), TUR_SUPER_MAX_ARGS);
                return NULL;
            }
            if (el->as.list.items[0]->as.sym == name) {
                diag_emit_with_code(DIAG_ERROR, el->span,
                    TUR_E0392_CLASS_SUPERCLASS_CYCLE,
                    "typeclass superclass cycle: '%s' lists itself as a superclass",
                    name->name);
                return NULL;
            }
            super_n_args[i] = (uint8_t)(el->as.list.len - 1);
            for (uint8_t k = 1; k < el->as.list.len; k++) {
                Form *v = el->as.list.items[k];
                int idx = -1;
                if (v->tag == F_SYM) {
                    for (uint8_t p = 0; p < n_type_params; p++)
                        if (type_params[p] && type_params[p] == v->as.sym) { idx = p; break; }
                }
                if (idx < 0) {
                    diag_emit_with_code(DIAG_ERROR, v->span,
                        TUR_E0390_CLASS_SUPERCLASS_PREAMBLE,
                        "defclass '%s': superclass constraint (%s ...) names '%s', "
                        "which is not one of this class's type parameters",
                        name->name, el->as.list.items[0]->as.sym->name,
                        v->tag == F_SYM ? v->as.sym->name : form_tag_name(v->tag));
                    return NULL;
                }
                super_arg_idx[i * TUR_SUPER_MAX_ARGS + (k - 1)] = (uint8_t)idx;
            }
            super_forms[i] = el;
        }
        methods_start += 1;
    }

    /* assoc-types-2 (Part A / MP2): optional functional-dependency clause
     * `| (from... -> to...)` immediately after the type-param vector.  The `|`
     * is a bare symbol; the following form is a parenthesized list with a `->`
     * separating the determining (from) names from the determined (to) names.
     * Example: (defclass Collect [c e] | (c -> e) ...).  A class with no `|`
     * clause has has_fundep == false (every parameter must be fixed at the
     * dispatch site). */
    bool     fundep_has       = false;
    uint16_t fundep_from_mask = 0;
    uint16_t fundep_to_mask   = 0;
    if (methods_start < call->as.list.len) {
        Form *bar = call->as.list.items[methods_start];
        if (bar->tag == F_SYM && strcmp(bar->as.sym->name, "|") == 0) {
            if (methods_start + 1 >= call->as.list.len ||
                call->as.list.items[methods_start + 1]->tag != F_LIST) {
                diag_emit(DIAG_ERROR, bar->span,
                          "functional dependency '|' must be followed by a "
                          "(from... -> to...) list");
                return NULL;
            }
            Form *fd = call->as.list.items[methods_start + 1];
            /* Locate the '->' separator within the fundep list. */
            int arrow_at = -1;
            for (uint32_t i = 0; i < fd->as.list.len; i++) {
                Form *t = fd->as.list.items[i];
                if (t->tag == F_SYM && strcmp(t->as.sym->name, "->") == 0) {
                    arrow_at = (int)i;
                    break;
                }
            }
            if (arrow_at < 0) {
                diag_emit(DIAG_ERROR, fd->span,
                          "functional dependency requires '->' (e.g. (c -> e))");
                return NULL;
            }
            /* Map a fundep name to its type-parameter index, setting the bit. */
            for (uint32_t i = 0; i < fd->as.list.len; i++) {
                if ((int)i == arrow_at) continue;
                Form *t = fd->as.list.items[i];
                if (t->tag != F_SYM) {
                    diag_emit(DIAG_ERROR, t->span,
                              "functional dependency entries must be type-parameter names");
                    return NULL;
                }
                int idx = -1;
                for (uint8_t p = 0; p < n_type_params; p++) {
                    if (type_params[p] && type_params[p] == t->as.sym) { idx = p; break; }
                }
                if (idx < 0) {
                    diag_emit(DIAG_ERROR, t->span,
                              "functional dependency names unknown type parameter '%s'",
                              t->as.sym->name);
                    return NULL;
                }
                if ((int)i < arrow_at) fundep_from_mask |= (uint16_t)(1u << idx);
                else                   fundep_to_mask   |= (uint16_t)(1u << idx);
            }
            if (fundep_from_mask == 0 || fundep_to_mask == 0) {
                diag_emit(DIAG_ERROR, fd->span,
                          "functional dependency needs at least one parameter on "
                          "each side of '->'");
                return NULL;
            }
            if (fundep_from_mask & fundep_to_mask) {
                diag_emit(DIAG_ERROR, fd->span,
                          "functional dependency 'from' and 'to' parameters must be disjoint");
                return NULL;
            }
            fundep_has = true;
            methods_start += 2;
        }
    }

    /* class-superclasses SC1: a vector HERE is either a preamble written after
     * the fundep clause (canonical order is preamble first) or a second
     * preamble.  Both get a dedicated diagnostic rather than the generic
     * "typeclass method requires ..." the method parser would otherwise blame
     * the whole defclass with. */
    if (methods_start < call->as.list.len &&
        call->as.list.items[methods_start]->tag == F_VEC) {
        Form *sv = call->as.list.items[methods_start];
        if (fundep_has && n_supers == 0) {
            diag_emit_with_code(DIAG_ERROR, sv->span,
                TUR_E0390_CLASS_SUPERCLASS_PREAMBLE,
                "defclass '%s': the superclass constraint vector must come BEFORE "
                "the '|' functional-dependency clause: "
                "(defclass %s [params] [(Super var)] | (from -> to) ...)",
                name->name, name->name);
        } else {
            diag_emit_with_code(DIAG_ERROR, sv->span,
                TUR_E0390_CLASS_SUPERCLASS_PREAMBLE,
                "defclass '%s': only one superclass constraint vector is allowed; "
                "list every superclass in it: [(A a) (B a)]",
                name->name);
        }
        return NULL;
    }

    /* Parse methods */
    TypeClassMethod *methods = NULL;
    uint8_t n_methods = 0;

    /* assoc-types-plan: a defclass body interleaves method declarations with
     * associated-type declarations `(type Name : Type)`.  Classify each body
     * form before parsing: a `type` head whose second element is a bare symbol
     * (not a [params] vector) is an associated-type member; everything else is
     * a method.  (A method literally named `type` carries a `[params]` vector
     * as its second element, so the discriminator never misfires.) */
    uint32_t n_body_forms = call->as.list.len - methods_start;
    uint32_t *method_form_idx = n_body_forms > 0
        ? (uint32_t *)arena_alloc(e->arena, n_body_forms * sizeof(uint32_t)) : NULL;
    const Symbol **assoc_type_names = n_body_forms > 0
        ? (const Symbol **)arena_alloc(e->arena, n_body_forms * sizeof(const Symbol *))
        : NULL;
    uint8_t n_assoc_types = 0;
    const Symbol *sym_type_kw = intern_cstr(e->st, "type");

    for (uint32_t i = methods_start; i < call->as.list.len; i++) {
        Form *bf = call->as.list.items[i];
        if (bf->tag == F_LIST && bf->as.list.len >= 2 &&
            bf->as.list.items[0]->tag == F_SYM &&
            bf->as.list.items[0]->as.sym == sym_type_kw &&
            bf->as.list.items[1]->tag == F_SYM) {
            assoc_type_names[n_assoc_types++] = bf->as.list.items[1]->as.sym;
            continue;
        }
        method_form_idx[n_methods++] = i;
    }

    if (n_methods == 0 && n_assoc_types == 0) {
        diag_emit(DIAG_ERROR, call->span,
                  "defclass requires at least one method or associated type");
        return NULL;
    }

    /* Allocate methods array */
    methods = n_methods > 0
        ? (TypeClassMethod *)arena_alloc(e->arena, n_methods * sizeof(TypeClassMethod))
        : NULL;
    /* Per-method body_start so the default-body elaboration can run as a
     * second pass after the redefinition check. */
    uint32_t *method_body_starts = n_methods > 0
        ? (uint32_t *)arena_alloc(e->arena, n_methods * sizeof(uint32_t)) : NULL;

    /* Second pass (signatures only): parse each method's signature.  Default
     * bodies are elaborated in a third pass below so that an idempotent
     * stdlib re-declare can short-circuit without registering orphan
     * __default_* file-level FnDefs. */
    for (uint32_t i = 0; i < n_methods; i++) {
        Form *method_form = call->as.list.items[method_form_idx[i]];
        uint32_t body_start = 0;
        TypeClassMethod *method = parse_typeclass_method(e, method_form, call->span, &body_start,
                                                         type_params, n_type_params,
                                                         type_param_kinds,
                                                         assoc_type_names, n_assoc_types);
        if (!method) return NULL;
        methods[i] = *method;
        method_body_starts[i] = body_start;
    }

    /* Redefinition check: same name already registered -> compare signatures.
     * Identical = idempotent silent skip (stdlib pre-declaration case);
     * different = hard error. */
    if (existing) {
        if (existing->n_assoc_types == n_assoc_types &&
            typeclass_signatures_match(existing, n_type_params,
                                       type_param_kinds, n_methods, methods,
                                       n_supers, super_forms, super_n_args,
                                       super_arg_idx)) {
            return e_nil(e, call->span);
        }
        diag_emit(DIAG_ERROR, call->span,
                  "typeclass '%s' is already defined", name->name);
        return NULL;
    }

    /* Third pass: RECORD default method bodies (if any).
     *
     * typeclass-default-methods-do-not-work: this used to elaborate the default
     * body here, as a synthetic `__default_<Class>_<method>` FnDef with its
     * parameters typed at the class's type variable.  That can never work for
     * the body a default exists to write -- `(or (.lt? x y) (= x y))` -- because
     * no instance of the class exists yet, so `.lt?` on a tyvar receiver
     * resolves to nothing; and it fired even when every instance implemented
     * the method, so merely WRITING a default broke the class.  The form is
     * kept instead and elab_definstance splices it in for an omitted method,
     * where it elaborates as an ordinary instance method against a concrete
     * receiver with its siblings resolvable. */
    for (uint32_t i = 0; i < n_methods; i++) {
        Form *method_form = call->as.list.items[method_form_idx[i]];
        uint32_t body_start = method_body_starts[i];
        if (body_start < method_form->as.list.len)
            methods[i].default_method_form = method_form;
    }

    /* ...and ALSO elaborate it at the class when that can succeed -- a default
     * body that calls no sibling method.  `errors/typeclass-effect-row-default-bad`
     * pins a class-level rule: a default whose body performs an effect beyond
     * the method's declared row is TUR-E0009 even when every instance overrides
     * it, and that check needs the body elaborated HERE (the spliced copy is
     * only checked where it is spliced).  A sibling-calling default is skipped
     * -- at the class there is nothing for `.lt?` to resolve against, which is
     * the failure this whole change exists to remove -- and gets its checking
     * per instance instead.  Decided syntactically, so it never depends on
     * whether a speculative elaboration happened to fail. */
    for (uint32_t i = 0; i < n_methods; i++) {
        Form *method_form = call->as.list.items[method_form_idx[i]];
        uint32_t body_start = method_body_starts[i];
        TypeClassMethod *method = &methods[i];

        /* ER3: If the method form has forms after the return type, elaborate
         * them as a default body.  This mirrors elab_definstance's method
         * elaboration so that effect_check_pass finds it as a normal FnDef. */
        if (body_start < method_form->as.list.len &&
            !default_body_mentions_sibling(method_form, body_start, methods, n_methods)) {
            /* Build a synthetic function name: __default_<TypeClass>_<method> */
            char default_name_buf[192];
            snprintf(default_name_buf, sizeof(default_name_buf),
                     "__default_%s_%s", name->name, method->name->name);
            const Symbol *default_sym = symtab_intern(e->st,
                strslice(default_name_buf, strlen(default_name_buf)));

            /* Build parameter bindings from the method signature */
            uint8_t n_mp = methods[i].n_params;
            Binding **mp = n_mp > 0
                ? (Binding **)arena_alloc(e->arena, n_mp * sizeof(Binding *)) : NULL;
            Type *mp_types = n_mp > 0
                ? (Type *)arena_alloc(e->arena, n_mp * sizeof(Type)) : NULL;

            /* Parse body parameter names from the method form's param vector */
            Form *pbody_params = method_form->as.list.items[1]; /* the [params] vector */
            uint8_t actual_p = 0;
            for (uint8_t j = 0; j < pbody_params->as.list.len && actual_p < n_mp; j++) {
                Form *pf = pbody_params->as.list.items[j];
                const Symbol *pname = NULL;
                Type ptype = methods[i].n_params > actual_p
                    ? methods[i].param_types[actual_p] : TYPE_INT;
                if (pf->tag == F_SYM) {
                    pname = pf->as.sym;
                } else if (pf->tag == F_VEC && pf->as.list.len >= 1
                           && pf->as.list.items[0]->tag == F_SYM) {
                    pname = pf->as.list.items[0]->as.sym;
                }
                if (!pname) continue;
                mp[actual_p] = binding_new(e, pname, ptype, false, false, pf->span);
                mp_types[actual_p] = ptype;
                actual_p++;
            }
            n_mp = actual_p;

            /* Push scope with parameters */
            Scope def_scope;
            scope_init(&def_scope, e->scope);
            e->scope = &def_scope;
            for (uint8_t j = 0; j < n_mp; j++)
                scope_add(&def_scope, mp[j]);
            e->fn_body_depth++;
            /* A default method is its own function (see Elab.ret_contract). */
            const RetContract *def_saved_ret_contract = e->ret_contract;
            e->ret_contract = NULL;

            /* Elaborate body forms */
            uint32_t n_body = method_form->as.list.len - body_start;
            Expr *def_body = e_nil(e, method_form->span);
            if (n_body == 1) {
                def_body = elab_form(e, method_form->as.list.items[body_start]);
            } else if (n_body > 1) {
                Expr **body_items = (Expr **)arena_alloc(e->arena, n_body * sizeof(Expr *));
                for (uint32_t k = 0; k < n_body; k++) {
                    body_items[k] = elab_form(e, method_form->as.list.items[body_start + k]);
                    if (!body_items[k]) {
                        e->fn_body_depth--;
                        e->ret_contract = def_saved_ret_contract;
                        e->scope = def_scope.parent;
                        scope_free(&def_scope);
                        return NULL;
                    }
                }
                def_body = expr_new(e->arena, EX_DO,
                    body_items[n_body - 1]->type, method_form->span);
                def_body->as.do_.items = body_items;
                def_body->as.do_.n = n_body;
            }

            e->fn_body_depth--;
            e->ret_contract = def_saved_ret_contract;
            e->scope = def_scope.parent;
            scope_free(&def_scope);

            /* Build FnDef and register it as a file-level function */
            TypeKind pk[MAX_FN_ARITY];
            for (uint8_t j = 0; j < n_mp; j++) pk[j] = mp_types[j].kind;
            Type fn_t = type_fn(pk, n_mp, methods[i].return_type.kind);

            FnDef *def_fd = (FnDef *)arena_alloc(e->arena, sizeof(FnDef));
            memset(def_fd, 0, sizeof(FnDef));
            Binding *def_b = binding_new(e, default_sym, fn_t, false, true,
                                          method_form->span);
            /* `__default_<Class>_<method>` is the elaborator's name for the
             * body the user wrote inside the defclass; keep it out of
             * symbol listings and diagnostics alike. */
            def_b->is_synthesized = true;
            def_b->synth_kind = SYNTH_DEFAULT_METHOD;
            {
                char lbl[192];
                int n = snprintf(lbl, sizeof(lbl),
                                 "default body of method '%s' in class %s",
                                 method->name->name, name->name);
                if (n > 0 && (size_t)n < sizeof(lbl))
                    def_b->diag_label = arena_strdup(e->arena, lbl, (size_t)n);
            }
            def_fd->binding        = def_b;
            def_fd->params         = mp;
            def_fd->n_params       = n_mp;
            def_fd->body           = def_body;
            def_fd->is_variadic    = false;
            def_fd->closure        = NULL;
            def_fd->param_types    = mp_types;
            def_fd->may_capture    = false;
            def_fd->inferred_effect_row = NULL;
            constraint_set_init(&def_fd->constraints);
            /* LS2/LS3: no surface borrow lifetimes on a synthesised default
             * method; give the lifetime pass a clean context + return Type. */
            lifetime_context_init(&def_fd->lifetime_ctx);
            def_fd->return_type = type_simple(TY_UNKNOWN, CK_COPY);

            scope_add(&e->global, def_b);
            Expr *def_expr = expr_new(e->arena, EX_FN_DEF, fn_t, method_form->span);
            def_expr->as.fn_def_.fn = def_fd;
            elab_register_file_def(e, def_expr);

            methods[i].default_fn_expr = def_expr;
        }
    }

    /* Register the typeclass in the environment */
    TypeClass *tc = typeclass_env_register_typeclass(&e->typeclass_env, name);
    if (!tc) {
        diag_emit(DIAG_ERROR, call->span,
                  "failed to register typeclass '%s'", name->name);
        return NULL;
    }
    
    tc->type_params       = type_params;
    tc->type_param_kinds  = type_param_kinds;
    tc->n_type_params     = n_type_params;
    tc->methods           = methods;
    tc->n_methods         = n_methods;
    tc->assoc_type_names  = n_assoc_types > 0 ? assoc_type_names : NULL;
    tc->n_assoc_types     = n_assoc_types;
    /* assoc-types-2 (MP2): record the functional dependency (if any). */
    tc->has_fundep        = fundep_has;
    tc->fundep_from_mask  = fundep_from_mask;
    tc->fundep_to_mask    = fundep_to_mask;
    /* class-superclasses SC1: the unresolved preamble; resolved post-unit. */
    tc->super_forms       = super_forms;
    tc->super_n_args      = super_n_args;
    tc->super_arg_idx     = super_arg_idx;
    tc->n_supers          = n_supers;
    tc->supers            = NULL;
    tc->decl_form         = call;
    /* Phase HKT-P4: record the file that defined this typeclass. */
    tc->origin_file_id    = call->span.file_id;
    /* method-vs-defn clash check: a class registered during stdlib auto-load is
     * "intentionally overridable" by a same-named user defn, so it is exempt
     * from the TUR-W0039 clash warning (see elab_toplevel.c). */
    /* interp-stdlib-class-method-shadows-user-defn: interpreter preload turns
     * load stdlib outside the in_stdlib_load bracket (stdlib_prefix == 0), so
     * OR in the preload flag -- otherwise MapKey/Show/... register as USER
     * classes under --interpret and a same-named user defn loses bare-call
     * resolution to the class method (compiled/interp divergence). */
    {
        extern bool g_turi_stdlib_preload;   /* runtime/globals.c */
        tc->from_stdlib   = e->in_stdlib_load || g_turi_stdlib_preload;
    }

    /* Create a TYPECLASS_DEF expression for codegen */
    Expr *tc_expr = expr_new(e->arena, EX_TYPECLASS_DEF, TYPE_NIL, call->span);
    tc_expr->as.typeclass_def_.typeclass = tc;
    elab_register_file_def(e, tc_expr);
    
    /* Create a nil expression as the result (defclass returns nothing) */
    return e_nil(e, call->span);
}

/* Build the codegen type-arg suffix for a typeclass instance, e.g. "_int",
 * "_option", or "_result_int".  This suffix is the discriminator baked into
 * every instance C symbol -- the method functions (__inst_<Class>_<method>SUFFIX)
 * and the dictionary struct/singleton (dict_<Class>SUFFIX).  It mirrors
 * emit_dict_name (emit_core.c) and is the single source of truth shared by both
 * the method-name builder below and the duplicate-instance guard, so the dedup
 * key can never drift from the names actually emitted.
 *
 * Two instances of the same typeclass collide in generated C iff their suffixes
 * are byte-equal.  Returns false on buffer overflow (caller emits the error). */
static bool build_inst_type_suffix(const Type *type_args,
                                   const Symbol **type_arg_syms,
                                   uint8_t n_type_args,
                                   char *out, size_t outlen) {
    size_t len = 0;
    if (outlen == 0) return false;
    out[0] = '\0';
    for (uint8_t j = 0; j < n_type_args; j++) {
        const char *type_component = NULL;
        char ctor_name_buf[32];  /* for TY_STRUCT/TY_APP constructor names (source) */
        char ctor_mangle_buf[128]; /* injective-mangled form of ctor_name_buf */
        switch (type_args[j].kind) {
            case TY_INT:     type_component = "int";     break;
            case TY_BOOL:    type_component = "bool";    break;
            case TY_CSTR:    type_component = "cstr";    break;
            case TY_NIL:     type_component = "nil";     break;
            case TY_PTR_VOID: type_component = "ptr_void"; break;
            case TY_INT8:    type_component = "int8";    break;
            case TY_INT16:   type_component = "int16";   break;
            case TY_INT32:   type_component = "int32";   break;
            case TY_UINT8:   type_component = "uint8";   break;
            case TY_UINT16:  type_component = "uint16";  break;
            case TY_UINT32:  type_component = "uint32";  break;
            case TY_UINT64:  type_component = "uint64";  break;
            case TY_FLOAT:   type_component = "float";   break;
            case TY_FLOAT32: type_component = "float32"; break;
            case TY_FLOAT64: type_component = "float64"; break;
            case TY_SYM:     type_component = "Sym";     break;
            case TY_FN:      type_component = "arrow";   break;
            case TY_TYVAR:
                /* structdef-retirement slice 5 B2 (P3): an unresolved instance
                 * head (an unknown name like `option`/`vec`) is now a named
                 * TY_TYVAR carried with its source symbol in type_arg_syms,
                 * exactly as the old def-less TY_STRUCT was.  Mangle by that
                 * name so two distinct unknown-name instances of the same class
                 * (TestFunctor[option] vs TestFunctor[vec]) get distinct
                 * suffixes -- without this both collapse to `_T` and the
                 * idempotent re-instance guard swallows the second. */
                if (type_arg_syms && type_arg_syms[j]) {
                    uint32_t sym_len = type_arg_syms[j]->len;
                    if (sym_len >= sizeof(ctor_name_buf))
                        sym_len = (uint32_t)(sizeof(ctor_name_buf) - 1);
                    memcpy(ctor_name_buf, type_arg_syms[j]->name, sym_len);
                    ctor_name_buf[sym_len] = '\0';
                    tur_mangle_ident(ctor_name_buf, ctor_mangle_buf, sizeof(ctor_mangle_buf));
                    type_component = ctor_mangle_buf;
                } else if (type_args[j].as.tyvar_.name) {
                    tur_mangle_ident(type_args[j].as.tyvar_.name,
                                     ctor_mangle_buf, sizeof(ctor_mangle_buf));
                    type_component = ctor_mangle_buf;
                } else {
                    type_component = "T";
                }
                break;
            case TY_ADT:
                /* CONV-S1 (defstruct-as-defadt): a record-ADT head -- a lowered
                 * `defstruct` or a hand-written single-variant `(defdata T ...)` --
                 * mangles by its constructor name exactly as TY_STRUCT does.
                 * Without this the switch fell through to `default: "T"`, so every
                 * non-parametric ADT-headed instance of a class collapsed to the
                 * same `_T` suffix and the idempotent re-instance guard silently
                 * swallowed all but the first (e.g. `Backend [CanvasBackend]`,
                 * `[SurfaceBackend]`, `[PngBackend]` -> one surviving instance).
                 * Flag-independent: also fixes hand-written record-ADT instances. */
                if (type_arg_syms && type_arg_syms[j]) {
                    uint32_t sym_len = type_arg_syms[j]->len;
                    if (sym_len >= sizeof(ctor_name_buf))
                        sym_len = (uint32_t)(sizeof(ctor_name_buf) - 1);
                    memcpy(ctor_name_buf, type_arg_syms[j]->name, sym_len);
                    ctor_name_buf[sym_len] = '\0';
                    tur_mangle_ident(ctor_name_buf, ctor_mangle_buf, sizeof(ctor_mangle_buf));
                    type_component = ctor_mangle_buf;
                } else if (type_args[j].as.adt_.def && type_args[j].as.adt_.def->name) {
                    type_component = type_args[j].as.adt_.def->name;
                } else {
                    type_component = "T";
                }
                break;
            case TY_APP: {
                /* Phase HKT §3: partial type application -- encode as "ctor_arg" */
                const char *ctor_part = "T";
                const char *arg_part  = "T";
                if (type_arg_syms && type_arg_syms[j]) {
                    ctor_part = type_arg_syms[j]->name;
                }
                if (type_args[j].as.app.arg) {
                    const Type *aarg = type_args[j].as.app.arg;
                    /* ECS E2d-P6: a parametric instance head's element is a
                     * NAMED TY_TYVAR (`A` in `(Dense A)`).  Render it honestly
                     * via type_name (`"tyvar"`); the C identifier only has to be
                     * unique per instance and stable, and the element name half
                     * is normalized so two declarations of the same parametric
                     * instance that pick different tyvar letters still mangle
                     * identically.  A concrete element keeps its real name. */
                    const char *n = type_name(*aarg);
                    if (n) arg_part = n;
                }
                char mctor[64], marg[64];
                tur_mangle_ident(ctor_part, mctor, sizeof(mctor));
                tur_mangle_ident(arg_part, marg, sizeof(marg));
                snprintf(ctor_mangle_buf, sizeof(ctor_mangle_buf), "%s_%s", mctor, marg);
                type_component = ctor_mangle_buf;
                break;
            }
            default: type_component = "T"; break;
        }
        int written = snprintf(out + len, outlen - len, "%s%s",
                               j == 0 ? "_" : "", type_component);
        if (written < 0 || (size_t)written >= outlen - len) return false;
        len += (size_t)written;
    }
    return true;
}

/* True when `t` still mentions a named type variable anywhere in its spine.
 * Used by the `@TypeName` pinned-dispatch path to tell a result type that is
 * fully resolved (`Buf`, `(Option int)`) from one that is still abstract. */
static bool tc_type_mentions_tyvar(const Type *t) {
    if (!t) return false;
    if (t->kind == TY_TYVAR) return true;
    if (t->kind == TY_APP)
        return tc_type_mentions_tyvar(t->as.app.fn) ||
               tc_type_mentions_tyvar(t->as.app.arg);
    return false;
}

/* ECS E2d-P6 (Issue 2 secondary): substitute a class method's parameter type --
 * which may reference the class's type parameters as named TY_TYVARs (e.g.
 * `val : E` parses to TY_TYVAR("E")) -- with the concrete instance type args, so
 * an instance method body whose params are unannotated (`[s idx val]`) inherits
 * the substituted types (S -> (Dense Pos), E -> Pos) instead of being left as
 * abstract tyvars.  Recurses through TY_APP so a parametric param type like
 * `(Dense E)` is rewritten too.  Returns the type unchanged when it mentions no
 * class type parameter. */
/* The function-arrow instance head: `(->)` in `(definstance C [(->)] ...)` is
 * recorded as an arity-0 TY_FN of kind * -> * -> * (see the is_arrow_head arm
 * of the definstance head parse). */
static bool tc_is_arrow_head_marker(const Type *t) {
    return t && t->kind == TY_FN && t->as.fn.arity == 0 &&
           t->hkt_kind == KIND_ARROW2;
}

static Type elab_subst_class_tyvars(Arena *arena, Type t,
                                    const Symbol **type_params,
                                    uint8_t n_type_params,
                                    const Type *type_args,
                                    uint8_t n_type_args);

/* arrow-instance-closure-erased-to-words: a class method written over a
 * binary class variable, `(>>> [f : (a b c) g : (a c d)] : (a b d))`, read at
 * the `(->)` head.  `(a X Y)` is `app(app(a, X), Y)`; with `a := (->)` it is
 * the function type `(fn [X] Y)`, carried as a fat closure (boxed) -- the
 * representation an arrow-head parameter already has.  X and Y keep their full
 * types, so a method whose element types are class-method type variables is a
 * generic impl the call site specializes, instead of a body fixed at erased
 * words.  Returns false when `t` is not that shape. */
static bool elab_subst_arrow_app(Arena *arena, const Type *t,
                                 const Symbol **type_params, uint8_t n_type_params,
                                 const Type *type_args, uint8_t n_type_args,
                                 Type *out) {
    if (t->kind != TY_APP || !t->as.app.fn || !t->as.app.arg) return false;
    const Type *inner = t->as.app.fn;
    if (inner->kind != TY_APP || !inner->as.app.fn || !inner->as.app.arg) return false;
    const Type *head = inner->as.app.fn;
    if (head->kind != TY_TYVAR || !head->as.tyvar_.name) return false;
    bool arrow = false;
    for (uint8_t k = 0; k < n_type_params && k < n_type_args; k++) {
        if (type_params[k] && strcmp(type_params[k]->name, head->as.tyvar_.name) == 0) {
            arrow = tc_is_arrow_head_marker(&type_args[k]);
            break;
        }
    }
    if (!arrow) return false;
    Type *x = (Type *)arena_alloc(arena, sizeof(Type));
    *x = elab_subst_class_tyvars(arena, *inner->as.app.arg, type_params,
                                 n_type_params, type_args, n_type_args);
    Type *y = (Type *)arena_alloc(arena, sizeof(Type));
    *y = elab_subst_class_tyvars(arena, *t->as.app.arg, type_params,
                                 n_type_params, type_args, n_type_args);
    /* A type variable rides as the int64 word at the C level, as `(-> A B)`
     * spells it; the full type says which variable. */
    TypeKind xk = x->kind == TY_TYVAR ? TY_INT : x->kind;
    TypeKind yk = y->kind == TY_TYVAR ? TY_INT : y->kind;
    Type fn = type_fn(&xk, 1, yk);
    fn.as.fn.arg_full_types = (Type **)arena_alloc(arena, sizeof(Type *));
    fn.as.fn.arg_full_types[0] = x;
    fn.as.fn.result_full_type = y;
    fn.as.fn.boxed = true;
    *out = fn;
    return true;
}

static Type elab_subst_class_tyvars(Arena *arena, Type t,
                                    const Symbol **type_params,
                                    uint8_t n_type_params,
                                    const Type *type_args,
                                    uint8_t n_type_args) {
    Type arrow_fn;
    if (elab_subst_arrow_app(arena, &t, type_params, n_type_params,
                             type_args, n_type_args, &arrow_fn))
        return arrow_fn;
    if (t.kind == TY_TYVAR && t.as.tyvar_.name) {
        for (uint8_t k = 0; k < n_type_params && k < n_type_args; k++) {
            if (type_params[k] &&
                strcmp(type_params[k]->name, t.as.tyvar_.name) == 0) {
                return type_args[k];
            }
        }
        return t;
    }
    if (t.kind == TY_APP) {
        if (t.as.app.fn) {
            Type *fn = (Type *)arena_alloc(arena, sizeof(Type));
            *fn = elab_subst_class_tyvars(arena, *t.as.app.fn, type_params,
                                          n_type_params, type_args, n_type_args);
            t.as.app.fn = fn;
        }
        if (t.as.app.arg) {
            Type *arg = (Type *)arena_alloc(arena, sizeof(Type));
            *arg = elab_subst_class_tyvars(arena, *t.as.app.arg, type_params,
                                           n_type_params, type_args, n_type_args);
            t.as.app.arg = arg;
        }
        return t;
    }
    return t;
}

/* partial-head-ap-calls-fat-closure-as-thin-pointer: the hole-aware twin of
 * elab_subst_class_tyvars, for an instance whose head is a hole-at-0 partial
 * application.  T4 stores `(Result _ B)` as `app(Result, B)` with
 * `partial_hole_pos == 0`, so the plain substitution turns the class method's
 * `(f X)` into `app(app(Result, B), X)` -- `(Result B X)`, the arms swapped.
 * Inside the instance body that typed `ap`'s `ff : (f (fn a b))` as
 * `(Result B (fn a b))`, so the `Ok` payload was the fixed arm `B`, not the
 * function: calling it was "not a function", and the ascription that worked
 * around that produced a thin call on what is really a fat closure box.
 *
 * Here an application whose head is a class parameter bound to a partial head
 * puts the applied argument in the HOLE slot instead: `(f X)` becomes
 * `app(app(Result, X), B)`, i.e. `(Result X B)`.  Every other shape defers to
 * the plain substitution, and so does every instance without a hole-at-0 head
 * (hole position 1 and a leftmost partial application `(Either E)` are
 * already correct under the plain substitution, since their free slot is the
 * outermost one). */
static Type elab_subst_class_tyvars_holed(Arena *arena, Type t,
                                          const Symbol **type_params,
                                          uint8_t n_type_params,
                                          const Type *type_args,
                                          uint8_t n_type_args,
                                          uint8_t hole_pos) {
    if (hole_pos != 0)
        return elab_subst_class_tyvars(arena, t, type_params, n_type_params,
                                       type_args, n_type_args);
    if (t.kind == TY_APP && t.as.app.fn && t.as.app.arg &&
        t.as.app.fn->kind == TY_TYVAR && t.as.app.fn->as.tyvar_.name) {
        for (uint8_t k = 0; k < n_type_params && k < n_type_args; k++) {
            if (!type_params[k] ||
                strcmp(type_params[k]->name, t.as.app.fn->as.tyvar_.name) != 0)
                continue;
            const Type *head = &type_args[k];
            /* A hole-at-0 head over a two-parameter constructor is exactly
             * `app(C, Fixed)`; anything else is not the shape T4 records. */
            if (head->kind != TY_APP || !head->as.app.fn || !head->as.app.arg ||
                head->as.app.fn->kind == TY_APP)
                break;
            Type *inner_arg = (Type *)arena_alloc(arena, sizeof(Type));
            *inner_arg = elab_subst_class_tyvars_holed(
                arena, *t.as.app.arg, type_params, n_type_params, type_args,
                n_type_args, hole_pos);
            Type *inner = (Type *)arena_alloc(arena, sizeof(Type));
            *inner = *head;                      /* app(C, Fixed) ... */
            inner->as.app.arg = inner_arg;       /* ... becomes app(C, X) */
            inner->hkt_kind = kind_apply_one(head->as.app.fn->hkt_kind);
            Type *fixed = (Type *)arena_alloc(arena, sizeof(Type));
            *fixed = *head->as.app.arg;
            Type out = *head;
            out.as.app.fn = inner;               /* app(app(C, X), Fixed) */
            out.as.app.arg = fixed;
            out.hkt_kind = kind_apply_one(inner->hkt_kind);
            return out;
        }
    }
    if (t.kind == TY_APP) {
        if (t.as.app.fn) {
            Type *fn = (Type *)arena_alloc(arena, sizeof(Type));
            *fn = elab_subst_class_tyvars_holed(arena, *t.as.app.fn, type_params,
                                                n_type_params, type_args,
                                                n_type_args, hole_pos);
            t.as.app.fn = fn;
        }
        if (t.as.app.arg) {
            Type *arg = (Type *)arena_alloc(arena, sizeof(Type));
            *arg = elab_subst_class_tyvars_holed(arena, *t.as.app.arg, type_params,
                                                 n_type_params, type_args,
                                                 n_type_args, hole_pos);
            t.as.app.arg = arg;
        }
        return t;
    }
    return elab_subst_class_tyvars(arena, t, type_params, n_type_params,
                                   type_args, n_type_args);
}

/* M7 fix direction 1 (flag-gated): a function value stored as an HKT container
 * element (e.g. the `(fn a b)` element of `(Option (fn a b))` in the
 * Applicative `ap` shape) is physically a fat closure box -- it was boxed at
 * the producer (the EX_FN_TO_FAT shim in elab_call.c) and extracted via the
 * carrier `value` field.  For the instance-method body to CALL it correctly
 * (`((.value ff) x)`), the element fn must be marked `boxed` so the application
 * dispatches through the fat-box thunk instead of a bare fn-pointer call (which
 * reads the box address as code and segfaults).  Walk a substituted body param
 * type and box any unboxed TY_FN that sits in HKT-element position. */
/* `inner_slots`: also box a fn in a constructor's INNER argument slots, not
 * just the outermost one.  Needed for a hole-at-0 partial head, where the
 * hole-aware substitution puts the element -- `ap`'s `(fn a b)` -- in the
 * innermost slot of `(Result (fn a b) B)`.  Off for every other instance, so
 * their body param types are unchanged. */
static Type m7_box_hkt_element_fns_ex(Arena *arena, Type t, bool inner_slots) {
    if (t.kind == TY_APP) {
        if (inner_slots && t.as.app.fn && t.as.app.fn->kind == TY_APP) {
            Type *fn = (Type *)arena_alloc(arena, sizeof(Type));
            *fn = m7_box_hkt_element_fns_ex(arena, *t.as.app.fn, inner_slots);
            t.as.app.fn = fn;
        }
        if (t.as.app.arg) {
            Type *arg = (Type *)arena_alloc(arena, sizeof(Type));
            *arg = m7_box_hkt_element_fns_ex(arena, *t.as.app.arg, inner_slots);
            t.as.app.arg = arg;
        }
        return t;
    }
    if (t.kind == TY_FN && !t.as.fn.boxed) {
        t.as.fn.boxed = true;
        return t;
    }
    return t;
}

/* M7 HKT layer-4 (flag-gated): is this instance-method body genuinely
 * by-value-constructible?  The emit-side per-(f, A) by-value spec only works
 * when the method body constructs its `(f b)` result IN-BODY via `^construct`
 * calls (`some`/`none`/`ok`/...) -- so its inner constructs recover by value.
 * A body that DELEGATES to a carrier helper (e.g. `Bifunctor [Result]`'s
 * `(result-bimap container ...)`, where `result-bimap` takes a `:int` carrier)
 * cannot, and must stay on the uniform-carrier dispatch until Phase 4.2 rewrites
 * the helper.  Recurse through if/do/let to the tail position.
 *
 * Monadic shapes (Monad `bind`): the tail of a branch may be a CALL to the
 * continuation `k` -- `(k (.value ma))` -- which returns the result family
 * `(m b)` BY VALUE (k is a fn-typed PARAMETER, not a global carrier helper).
 * Admit such a tail too, distinguished from a carrier-delegating helper by
 * (a) the callee is a local fn binding (`!is_global`), not a top-level defn,
 * and (b) the call's result type is the applied `(f b)` family (TY_APP). */
static bool m7_body_constructs_byvalue(const Expr *e) {
    if (!e) return false;
    switch (e->kind) {
        case EX_IF:
            return m7_body_constructs_byvalue(e->as.if_.then_) &&
                   m7_body_constructs_byvalue(e->as.if_.else_or_null);
        case EX_DO:
            return e->as.do_.n > 0 &&
                   m7_body_constructs_byvalue(e->as.do_.items[e->as.do_.n - 1]);
        case EX_LET:
            return m7_body_constructs_byvalue(e->as.let_.body);
        case EX_MATCH:
            /* HKT instance bodies are commonly a `match` over the receiver, with
             * each arm CONSTRUCTING the result family in-body -- e.g.
             * `(definstance Functor [ReF] (fmap [c g] (match c (EmptyF) (EmptyF)
             *  (AltF x y) (AltF (g x) (g y)) ...)))`.  Such a body is
             * by-value-constructible iff every arm's tail is.  Without this arm
             * the by-value spec is never minted and a sub-int64 / float result
             * element silently reads the wrong (carrier int64) ADT layout. */
            if (e->as.match_.n_arms == 0) return false;
            for (uint32_t i = 0; i < e->as.match_.n_arms; i++) {
                if (!e->as.match_.arms[i].body ||
                    !m7_body_constructs_byvalue(e->as.match_.arms[i].body))
                    return false;
            }
            return true;
        case EX_VAR:
            /* Selection / pass-through tail (Alternative `<|>` / `or-else`):
             * the body returns an EXISTING `(f a)` value -- a parameter or local
             * of the result applied family -- directly, e.g.
             * `(if (some? x) x y)`.  Under the by-value spec the param's type is
             * the by-value `Option__int`, so returning it is already by value;
             * no in-body `^construct` is needed.  Restrict to the applied
             * `(f b)` family (TY_APP) so a bare-element return (the `extract` /
             * Foldable shape, whose result is not an applied type) stays on the
             * uniform carrier path until its own probe hardens it. */
            return e->type.kind == TY_APP;
        case EX_CALL:
            if (e->as.call_.fn_binding &&
                e->as.call_.fn_binding->is_construct_template)
                return true;
            /* An ADT constructor call -- `(AltF (g x) (g y))`, `(EmptyF)` -- builds
             * the result family in-body.  For a by-value (`:copy`) defdata this
             * constructs the by-value ADT layout (`ctor_*__bool`) per element under
             * the active spec, so it is by-value-constructible.  This is the shape
             * of a `match`-bodied `Functor`/`Bifunctor` instance over a sum type. */
            if (e->as.call_.ctor)
                return true;
            /* Monadic continuation tail call: a local (parameter) fn returning
             * the applied `(f b)` family by value. */
            if (e->as.call_.fn_binding && !e->as.call_.fn_expr &&
                !e->as.call_.fn_binding->is_global &&
                e->type.kind == TY_APP)
                return true;
            return false;
        default:
            return false;
    }
}

/* M7 HKT layer-4 (flag-gated): is this instance-method body a by-value-safe
 * BARE-ELEMENT return?  The Comonad `extract [w : (f a)] : a` / Foldable shape
 * returns a bare element (`a`, grounding to a scalar/struct), not an applied
 * `(f b)` -- so there is no `^construct` to recover, and m7_body_constructs_
 * byvalue (which looks for one) correctly rejects it.  A bare-element body is
 * by-value-safe when its tail merely READS a scalar out of the (now by-value)
 * receiver -- a field access `(.value w)` -- or returns a bare element binding
 * directly.  It is NOT safe if it passes the whole `(f a)` receiver to a carrier
 * helper, so we admit only field reads / bare-element vars (recursing through
 * if/do/let to the tail), never a general call. */
static bool m7_body_returns_byvalue_element(const Expr *e) {
    if (!e) return false;
    switch (e->kind) {
        case EX_IF:
            return m7_body_returns_byvalue_element(e->as.if_.then_) &&
                   m7_body_returns_byvalue_element(e->as.if_.else_or_null);
        case EX_DO:
            return e->as.do_.n > 0 &&
                   m7_body_returns_byvalue_element(e->as.do_.items[e->as.do_.n - 1]);
        case EX_LET:
            return m7_body_returns_byvalue_element(e->as.let_.body);
        case EX_MATCH:
            /* A bare-element body may also `match` the receiver, each arm
             * READING a scalar element by value (Comonad/Foldable shapes). */
            if (e->as.match_.n_arms == 0) return false;
            for (uint32_t i = 0; i < e->as.match_.n_arms; i++) {
                if (!e->as.match_.arms[i].body ||
                    !m7_body_returns_byvalue_element(e->as.match_.arms[i].body))
                    return false;
            }
            return true;
        case EX_GET_FIELD:
            /* `(.value w)` -- a scalar field read off the by-value receiver. */
            return true;
        case EX_VAR:
            /* returns a bare element value (a param/local) directly, never the
             * applied `(f a)` receiver itself (that is the selection shape). */
            return e->type.kind != TY_APP;
        case EX_CALL:
            /* Foldable `foldr`: the tail folds via a fn PARAMETER --
             * `(g (.value t) z)` -- returning the bare element result by value.
             * Admit a local (parameter) fn callee whose result is a bare element
             * (non-applied), distinguished from a carrier-delegating global
             * helper by `!is_global`.  The receiver only ever reaches the callee
             * as an extracted scalar (`(.value t)`), never whole, so by value is
             * safe. */
            return e->as.call_.fn_binding && !e->as.call_.fn_expr &&
                   !e->as.call_.fn_binding->is_global &&
                   e->type.kind != TY_APP;
        case EX_ANY_CAST:
            /* The checked unbox a dynamic body takes under a `: a` result
             * (see the instance-body narrowing): the element read is under it. */
            return m7_body_returns_byvalue_element(e->as.any_cast_.value);
        case EX_DYN_CALL: {
            /* saffron-dyn-witness-fn-arity-defaults-unary: a Saffron `g : fn`
             * extra is an `any` called dynamically -- `(g (.l t) (.r t))`.
             * Like the Foldable fn-PARAMETER arm above, the receiver reaches
             * the callee only as extracted elements, each WIDENED to `any` on
             * the way in, and that widen is what needs the element's type.
             * Admit a local (parameter) callee whose every argument is itself
             * a by-value element read. */
            const Expr *fe = e->as.dyn_call_.fn;
            while (fe && fe->kind == EX_ASCRIBE) fe = fe->as.ascribe_.inner;
            if (!fe || fe->kind != EX_VAR || !fe->as.var.binding ||
                fe->as.var.binding->is_global)
                return false;
            for (uint32_t i = 0; i < e->as.dyn_call_.n_args; i++)
                if (!m7_body_returns_byvalue_element(e->as.dyn_call_.args[i]))
                    return false;
            return true;
        }
        case EX_UNION_INJECT: {
            /* erased-instance-body-tags-a-type-variable-widened-to-any: an
             * `: any` result is an element read WIDENED on the way out --
             * `(.v o)` under `(unbox1 [o] ...)`.  The widen is what needs the
             * element's concrete type (its tag), so the read under it decides.
             *
             * Except a call through an UNTYPED `g : fn` carrier: it takes and
             * returns the int64 word whatever the elements are, so a spec
             * whose elements are `any` or a double hands it a value it cannot
             * take (a cc error, `aggregate value used where an integer was
             * expected` -- the saffron-dyn-witness-fn-arity repro). */
            const Expr *v = e->as.union_inject_.value;
            if (v && v->kind == EX_CALL && v->as.call_.fn_binding &&
                v->as.call_.fn_binding->is_poly_fn &&
                !(v->as.call_.fn_binding->poly_type &&
                  v->as.call_.fn_binding->poly_type->kind == TY_FN))
                return false;
            return m7_body_returns_byvalue_element(v);
        }
        default:
            return false;
    }
}

/* M7 HKT layers 1+3 (flag-gated): unify a method's DECLARED parameter type
 * (carrying element tyvars, e.g. `(g a)` or `(fn [a] b)`) against the ACTUAL
 * call-site argument type, recording each tyvar -> concrete-type binding.  The
 * collected bindings feed elab_subst_class_tyvars to refine the HKT result
 * `(Option b)` to a ground `(Option int)` at the call site.  Names are interned
 * so they compare by Symbol identity the same way elab_subst_class_tyvars does. */
static void m7_collect_tyvar_bindings(Elab *e, Type decl, Type act,
                                      const Symbol **names, Type *types,
                                      uint8_t *n, uint8_t max) {
    switch (decl.kind) {
        case TY_TYVAR:
            /* A free-tyvar ACTUAL carries no grounding information (e.g. the
             * element of a bare `(none)` argument).  Skip it so a sibling
             * argument that DOES ground this tyvar wins -- the Alternative
             * `(alt2 (none) (some 42))` shape, where the element is recoverable
             * from the second arg.  Without this skip the "keep the first
             * binding" rule below would lock `a` to the free element of arg 0
             * and the result would never ground.  (This also leaves the genuine
             * `ap` caveat -- `(ap (none) (some 41))`, where NO arg grounds `b` --
             * un-grounded, so it still falls back to carrier dispatch.) */
            if (act.kind == TY_TYVAR) return;
            if (decl.as.tyvar_.name) {
                for (uint8_t i = 0; i < *n; i++)
                    if (names[i] && strcmp(names[i]->name, decl.as.tyvar_.name) == 0)
                        return;  /* keep the first binding for a given tyvar */
                if (*n < max) {
                    names[*n] = symtab_intern(e->st,
                        strslice(decl.as.tyvar_.name,
                                 (uint32_t)strlen(decl.as.tyvar_.name)));
                    types[*n] = act;
                    (*n)++;
                }
            }
            return;
        case TY_APP:
            /* arrow-instance-closure-erased-to-words: `(a X Y)` over a binary
             * class variable, met by a one-argument function -- the `(->)`
             * head.  X binds to the function's parameter, Y to its result
             * (the head variable itself is the caller's to bind). */
            if (act.kind == TY_FN && act.as.fn.arity == 1 &&
                decl.as.app.fn && decl.as.app.arg &&
                decl.as.app.fn->kind == TY_APP && decl.as.app.fn->as.app.arg &&
                decl.as.app.fn->as.app.fn &&
                decl.as.app.fn->as.app.fn->kind == TY_TYVAR) {
                Type aa = (act.as.fn.arg_full_types && act.as.fn.arg_full_types[0])
                          ? *act.as.fn.arg_full_types[0]
                          : type_from_kind(act.as.fn.arg_kinds[0]);
                Type ar = act.as.fn.result_full_type
                          ? *act.as.fn.result_full_type
                          : type_from_kind(act.as.fn.result_kind);
                m7_collect_tyvar_bindings(e, *decl.as.app.fn->as.app.arg, aa,
                                          names, types, n, max);
                m7_collect_tyvar_bindings(e, *decl.as.app.arg, ar,
                                          names, types, n, max);
                return;
            }
            if (act.kind == TY_APP) {
                if (decl.as.app.fn && act.as.app.fn)
                    m7_collect_tyvar_bindings(e, *decl.as.app.fn, *act.as.app.fn,
                                              names, types, n, max);
                if (decl.as.app.arg && act.as.app.arg)
                    m7_collect_tyvar_bindings(e, *decl.as.app.arg, *act.as.app.arg,
                                              names, types, n, max);
            }
            return;
        case TY_FN:
            if (act.kind == TY_FN) {
                uint32_t ar = decl.as.fn.arity < act.as.fn.arity
                             ? decl.as.fn.arity : act.as.fn.arity;
                for (uint8_t i = 0; i < ar; i++) {
                    Type da = (decl.as.fn.arg_full_types && decl.as.fn.arg_full_types[i])
                              ? *decl.as.fn.arg_full_types[i]
                              : type_from_kind(decl.as.fn.arg_kinds[i]);
                    Type aa = (act.as.fn.arg_full_types && act.as.fn.arg_full_types[i])
                              ? *act.as.fn.arg_full_types[i]
                              : type_from_kind(act.as.fn.arg_kinds[i]);
                    m7_collect_tyvar_bindings(e, da, aa, names, types, n, max);
                }
                Type dr = decl.as.fn.result_full_type
                          ? *decl.as.fn.result_full_type
                          : type_from_kind(decl.as.fn.result_kind);
                Type ar2 = act.as.fn.result_full_type
                           ? *act.as.fn.result_full_type
                           : type_from_kind(act.as.fn.result_kind);
                m7_collect_tyvar_bindings(e, dr, ar2, names, types, n, max);
            }
            return;
        default:
            return;
    }
}

/* hkt-carrier-result-loses-payload-types: a copy of the TY_APP chain `t` with
 * the argument `layers_from_outer` applications below the outermost replaced by
 * `newarg` (0 = the outermost application's argument).  The receiver `(f a)`
 * with its hole slot replaced by `b` IS `(f b)`, whatever the instance head's
 * erased spelling says.  Shares nothing with the head, so the arms cannot
 * swap.  Beyond the chain's depth, `t` is returned unchanged. */
static Type m7_app_replace_slot(Arena *arena, Type t, uint32_t layers_from_outer,
                                Type newarg) {
    if (t.kind != TY_APP || !t.as.app.fn || !t.as.app.arg) return t;
    Type out = t;
    if (layers_from_outer == 0) {
        Type *na = (Type *)arena_alloc(arena, sizeof(Type));
        *na = newarg;
        out.as.app.arg = na;
        return out;
    }
    Type *nf = (Type *)arena_alloc(arena, sizeof(Type));
    *nf = m7_app_replace_slot(arena, *t.as.app.fn, layers_from_outer - 1, newarg);
    out.as.app.fn = nf;
    return out;
}

/* hkt-carrier-result-loses-payload-types: the argument `layers_from_outer`
 * applications below the outermost of the TY_APP chain `t` (0 = the outermost
 * application's argument).  False when the chain is shallower than asked. */
static bool m7_app_slot_arg(Type t, uint32_t layers_from_outer, Type *out) {
    while (layers_from_outer > 0) {
        if (t.kind != TY_APP || !t.as.app.fn) return false;
        t = *t.as.app.fn;
        layers_from_outer--;
    }
    if (t.kind != TY_APP || !t.as.app.arg) return false;
    *out = *t.as.app.arg;
    return true;
}

/* hkt-carrier-result-loses-payload-types: does the tyvar `bname` occur inside
 * an application headed by the class variable `fvar` anywhere in `t`?  Such an
 * occurrence is where the class-variable unification reads a hole-at-0 head
 * reversed, so a binding of `bname` collected there cannot be trusted. */
static bool m7_tyvar_under_app_of(const Type *t, const char *fvar, const char *bname) {
    if (!t || !fvar || !bname) return false;
    switch (t->kind) {
        case TY_APP: {
            const Type *h = t;
            while (h && h->kind == TY_APP && h->as.app.fn) h = h->as.app.fn;
            bool under = h && h->kind == TY_TYVAR && h->as.tyvar_.name &&
                         strcmp(h->as.tyvar_.name, fvar) == 0;
            for (const Type *a = t; a && a->kind == TY_APP; a = a->as.app.fn) {
                const Type *arg = a->as.app.arg;
                if (!arg) continue;
                if (under && arg->kind == TY_TYVAR && arg->as.tyvar_.name &&
                    strcmp(arg->as.tyvar_.name, bname) == 0)
                    return true;
                if (m7_tyvar_under_app_of(arg, fvar, bname)) return true;
            }
            return false;
        }
        case TY_FN: {
            if (t->as.fn.arg_full_types)
                for (uint32_t i = 0; i < t->as.fn.arity; i++)
                    if (m7_tyvar_under_app_of(t->as.fn.arg_full_types[i], fvar, bname))
                        return true;
            return m7_tyvar_under_app_of(t->as.fn.result_full_type, fvar, bname);
        }
        default:
            return false;
    }
}

static bool m7_params_mention_under_app_of(const TypeClassMethod *cm,
                                           const char *fvar, const char *bname) {
    for (uint8_t p = 0; p < cm->n_params; p++)
        if (m7_tyvar_under_app_of(&cm->param_types[p], fvar, bname)) return true;
    return false;
}

/* M7 layer-4 guard: does a (post-substitution) result type still carry a named,
 * un-grounded element tyvar?  When the HKT by-value monomorphization cannot
 * recover a result element tyvar from the call args -- the Applicative `ap`
 * shape, whose result element `b` lives only inside a wrapped function value
 * that erases to `ptr<void>` (docs/archive/history/m7-hkt-ap-fn-element-carrier-
 * erasure.md) -- the substituted result `(Option b)` keeps `b` free.  Emitting
 * a by-value spec for it mints a half-by-value method (carrier `int64_t`
 * return) while the dispatch dict references a dropped carrier base, a hard cc
 * error.  Detecting the free tyvar lets the caller fall back to the uniform
 * carrier dispatch instead. */
static bool m7_type_has_free_tyvar(Type t) {
    switch (t.kind) {
        case TY_TYVAR:
            return t.as.tyvar_.name != NULL;
        case TY_APP:
            return (t.as.app.fn && m7_type_has_free_tyvar(*t.as.app.fn)) ||
                   (t.as.app.arg && m7_type_has_free_tyvar(*t.as.app.arg));
        case TY_FN: {
            if (t.as.fn.arg_full_types)
                for (uint32_t i = 0; i < t.as.fn.arity; i++)
                    if (t.as.fn.arg_full_types[i] &&
                        m7_type_has_free_tyvar(*t.as.fn.arg_full_types[i]))
                        return true;
            return t.as.fn.result_full_type &&
                   m7_type_has_free_tyvar(*t.as.fn.result_full_type);
        }
        default:
            return false;
    }
}

/* method-result-functor-inference: is this fully-ground applied result type
 * representationally the int64 carrier?  True for an opaque newtype head
 * (`(defopaque Box [a] :int)` -- n_ctors == 0, is_opaque) and a transparent
 * int-record newtype -- BOTH lower to `int64_t`, so committing the grounded
 * `(Box int)` as the call's static result type is ABI-identical to the
 * carrier the (carrier-bodied) instance method actually returns.  This lets a
 * downstream `(un-box r)` recover `f := Box` from the receiver's `(Box int)`
 * even when the instance body delegates to a carrier helper and so is NOT
 * by-value-constructible (m7_body_byvalue_ok == false).  Kept narrow to the
 * int64-carrier representations: a genuine by-value aggregate (Option/Result
 * with multiple fields) must still mint a by-value spec before its precise
 * result type may be committed, or the consumer reads the aggregate layout
 * off a carrier int64 (the carrier-vs-by-value mismatch). */
static bool m7_result_is_int_carrier(Type t) {
    if (type_is_transparent_int_newtype(t)) return true;
    if (t.kind == TY_APP) {
        AdtDef *adef = NULL;
        Type args[16];
        uint8_t na = 0;
        if (type_extract_adt_app(&t, &adef, args, &na) && adef)
            return adef->is_opaque;
    }
    if (t.kind == TY_ADT)
        return t.as.adt_.def && t.as.adt_.def->is_opaque;
    return false;
}

/* hkt-fmap-result-is-not-droppable: collapse an applied HKT result whose head is
 * a POINTER-FAMILY builtin type constructor -- `(type-app rc<?> int)` -- down to
 * the concrete `rc<int>` the rest of the compiler recognizes.
 *
 * An instance over the built-in `rc` constructor (stdlib/rc.tur's `Functor [rc]`)
 * gets its result substituted correctly to `(type-app rc<?> int)`, but nothing
 * consumed that shape: `rc/drop` and friends test for TY_RC, so the fmap result
 * could not be released and every `(fmap r f)` leaked a control block plus a
 * payload slot.  The head substitution builds a TY_APP because that is what an
 * HKT class result `(f b)` is; `rc<T>` is not spelled as a TY_APP anywhere else,
 * so the two representations have to be reconciled here.
 *
 * Sound to commit WITHOUT minting a by-value spec, for exactly the reason the
 * opaque-newtype arm beside it is: an rc/weak is an `RcControlBlock *` and a ref
 * is a plain pointer, so the by-value representation IS the int64 carrier the
 * method returns -- 8 bytes, same bits, no aggregate layout to misread.  (The
 * emitter already relies on this: TY_RC sits in emit_fns.c's typed-pointer
 * return escape-hatch list, pinned by tests/fixtures/inline-c-rc-return-typed.)
 *
 * Returns false for any other head, leaving the neighbouring arms untouched. */
static bool m7_app_to_ptr_family(Type t, Type *out) {
    if (t.kind != TY_APP || !t.as.app.fn || !t.as.app.arg) return false;
    TypeKind head = t.as.app.fn->kind;
    if (head != TY_RC && head != TY_WEAK && head != TY_REF && head != TY_LREF)
        return false;
    const Type *arg = t.as.app.arg;
    /* An aggregate element carries its def so field access through the handle
     * still resolves, mirroring type_rc_adt on the struct-field path. */
    if (head == TY_RC && arg->kind == TY_ADT && arg->as.adt_.def) {
        *out = type_rc_adt(arg->as.adt_.def);
        return true;
    }
    if (head == TY_WEAK && arg->kind == TY_ADT && arg->as.adt_.def) {
        *out = type_weak_adt(arg->as.adt_.def);
        return true;
    }
    switch (head) {
        case TY_RC:   *out = type_rc(arg->kind);   return true;
        case TY_WEAK: *out = type_weak(arg->kind); return true;
        case TY_REF:  *out = type_ref(arg->kind);  return true;
        case TY_LREF: *out = type_lref(arg->kind); return true;
        default:      return false;
    }
}

/* Parse a single instance-head argument form into a Type.  A primitive type
 * keyword (`int`, `cstr`, ...) or a known struct/ADT name resolves to its
 * concrete type; any other bare name is treated as a head *type variable*
 * (e.g. `V` in `(Map cstr V)`), recorded as a named TY_TYVAR so its identity
 * survives to the dispatch site where it unifies against the receiver's
 * matching slot.  Mirrors the inline arg parser in the single-`_` partial-
 * application path.  Returns false (after emitting a diagnostic) when the form
 * is not a symbol or keyword. */
static bool parse_instance_head_arg(Elab *e, const Form *f, Type *out) {
    if (f->tag != F_SYM && f->tag != F_KEYWORD) {
        diag_emit(DIAG_ERROR, f->span,
                  "type application argument must be a type keyword or symbol");
        return false;
    }
    const Symbol *akw = f->as.sym;
    if (akw->len == 3 && memcmp(akw->name, "int", 3) == 0) {
        *out = TYPE_INT; return true;
    }
    if (akw->len == 4 && memcmp(akw->name, "bool", 4) == 0) {
        *out = TYPE_BOOL; return true;
    }
    if (akw->len == 4 && memcmp(akw->name, "cstr", 4) == 0) {
        *out = TYPE_CSTR; return true;
    }
    if ((akw->len == 4 && memcmp(akw->name, "void", 4) == 0) ||
        (akw->len == 3 && memcmp(akw->name, "nil", 3) == 0)) {
        *out = TYPE_NIL; return true;
    }
    TypeKind ank = typekind_from_symbol(akw->name);
    if (ank != TY_UNKNOWN) {
        *out = type_simple(ank, CK_COPY); return true;
    }
    Binding *asb = scope_lookup(e->scope, akw);
    if (asb && asb->type.kind == TY_ADT && asb->type.as.adt_.def) {
        *out = asb->type; return true;
    }
    /* return-dispatched-sum-mint-in-constrained-instance-miscompiles (repro
     * 3): scope_lookup is value-preferring, so for a lowered defstruct (and
     * any `(defdata T ... (T ...))`) the name resolves to the constructor
     * FUNCTION and the fall-through below turned `Box` in `[(Option Box)]`
     * into a type VARIABLE named Box.  Ask the type namespace, as the
     * two-parameter head path (`elab_lookup_type_by_name`, below) already
     * does; only a name known to neither is a genuine head tyvar. */
    Type *aty = elab_lookup_type_by_name(e, akw);
    if (aty && aty->kind == TY_ADT && aty->as.adt_.def) {
        *out = *aty; return true;
    }
    *out = type_tyvar_named(akw->name);
    return true;
}

/* Resolve a PRIMITIVE type name appearing in a `definstance` constraint
 * (`[TC float]`, `[(TC cstr)]`).  Returns false for anything that is not a
 * built-in scalar so the caller can go on to try type parameters and
 * user-defined type names.
 *
 * This is the same name set `parse_instance_head_arg` accepts, factored out so
 * the constraint parsers cannot drift back to recognising a hand-written subset
 * of it -- they used to accept exactly `int`/`bool`/`cstr` and silently keep
 * their `TYPE_INT` initializer for every other spelling, which made `[TC float]`
 * mean `[TC int]`.  See docs/archive/definstance-constraint-type-defaults-to-int.md. */
static bool constraint_prim_type(const Symbol *kw, Type *out) {
    if (kw->len == 3 && memcmp(kw->name, "int", 3) == 0)  { *out = TYPE_INT;  return true; }
    if (kw->len == 4 && memcmp(kw->name, "bool", 4) == 0) { *out = TYPE_BOOL; return true; }
    if (kw->len == 4 && memcmp(kw->name, "cstr", 4) == 0) { *out = TYPE_CSTR; return true; }
    if ((kw->len == 4 && memcmp(kw->name, "void", 4) == 0) ||
        (kw->len == 3 && memcmp(kw->name, "nil", 3) == 0)) { *out = TYPE_NIL; return true; }
    if (kw->len == 9 && memcmp(kw->name, "ptr<void>", 9) == 0) { *out = TYPE_PTR_VOID; return true; }
    TypeKind nk = typekind_from_symbol(kw->name);
    if (nk != TY_UNKNOWN) { *out = type_simple(nk, CK_COPY); return true; }
    return false;
}

/* Resolve a USER-DEFINED type name appearing in a `definstance` constraint
 * (`[TC MyStruct]`).  Mirrors the instance head's own resolution: the type
 * namespace first (so the owning module is credited), then the value binding,
 * which is where a lowered `defstruct` whose constructor shadows the type name
 * is found.  Returns false when the name is not a known type -- the caller
 * reports that rather than defaulting. */
/* The ADT a `definstance` head argument is built from.  A bare head (`[Cons]`)
 * is the ADT itself; an applied head (`[(Option A)]`, `[(Map cstr V)]`) is a
 * TY_APP spine whose innermost `fn` is the constructor.  Used to decide whether
 * a constraint variable names one of the head's type parameters. */
static AdtDef *constraint_head_adt(const Type *t) {
    while (t && t->kind == TY_APP) t = t->as.app.fn;
    if (t && t->kind == TY_ADT) return t->as.adt_.def;
    return NULL;
}

static bool constraint_named_type(Elab *e, const Symbol *kw, Type *out) {
    Type *ty = elab_lookup_type_by_name(e, kw);
    if (ty && ty->kind == TY_ADT && ty->as.adt_.def) { *out = *ty; return true; }
    Binding *b = scope_lookup(e->scope, kw);
    if (b && b->type.kind == TY_ADT && b->type.as.adt_.def) { *out = b->type; return true; }
    return false;
}

/* Elaborate (definstance ClassName [type-args...] (method1 [args...] body...) ...)
 *
 * Defines an instance of a typeclass for concrete types.
 * Syntax: (definstance Eq int (eq? [x y] (== x y)))
 *         (definstance Show int (show [x] (int->str x)))
 */
static Expr *elab_definstance_inner(Elab *e, const Form *call);

/* typeclass-method-resolution-ignores-the-class: bracket the whole instance
 * elaboration so the unconstrained-method-call check can tell "inside a
 * definstance body" from "inside an ordinary defn".  A wrapper rather than
 * inc/dec at each return, because the inner function has many exit paths. */
Expr *elab_definstance(Elab *e, const Form *call) {
    e->definstance_depth++;
    Expr *r = elab_definstance_inner(e, call);
    if (e->definstance_depth > 0) e->definstance_depth--;
    return r;
}

static Expr *elab_definstance_inner(Elab *e, const Form *call) {
    /* Minimum: (definstance ClassName) */
    if (call->as.list.len < 2) {
        diag_emit(DIAG_ERROR, call->span,
                  "definstance requires a typeclass name: (definstance ClassName ...)");
        return NULL;
    }
    
    /* Parse typeclass name */
    Form *tc_form = call->as.list.items[1];
    if (tc_form->tag != F_SYM) {
        diag_emit(DIAG_ERROR, tc_form->span,
                  "definstance typeclass name must be a symbol");
        return NULL;
    }
    const Symbol *tc_name = tc_form->as.sym;

    /* Look up the typeclass */
    TypeClass *tc = typeclass_env_lookup_typeclass(&e->typeclass_env, tc_name);
    if (!tc) {
        diag_emit(DIAG_ERROR, tc_form->span,
                  "typeclass '%s' is not defined", tc_name->name);
        return NULL;
    }
    
    /* Parse type arguments (optional) */
    Type *type_args = NULL;
    /* Phase HKT H3: track original symbol for each type arg so method name
     * mangling can use "option", "vec", etc. instead of the generic "T".
     * Only allocated when needed (at least one unknown/constructor type arg). */
    const Symbol **type_arg_syms = NULL;
    uint8_t n_type_args = 0;
    uint32_t impls_start = 2;
    /* M7 partial-app wildcard head: the `_` hole slot index parsed from a
     * `(Ctor _ B)` / `(Ctor A _)` instance head, recorded onto the instance
     * so the by-value HKT grounding can fix the non-hole slots. 0xFF = none. */
    uint8_t hkt_hole_pos = 0xFF;
    
    if (call->as.list.len >= 3) {
        Form *args_form = call->as.list.items[2];
        if (args_form->tag == F_VEC) {
            n_type_args = args_form->as.list.len;
            if (n_type_args > 0) {
                type_args = (Type *)arena_alloc(e->arena, n_type_args * sizeof(Type));
                type_arg_syms = (const Symbol **)arena_alloc(e->arena,
                    n_type_args * sizeof(const Symbol *));
                for (uint8_t i = 0; i < n_type_args; i++) {
                    type_arg_syms[i] = NULL;  /* NULL means use default name */
                }
                for (uint8_t i = 0; i < n_type_args; i++) {
                    Form *arg = args_form->as.list.items[i];
                    /* Function-arrow instance head: `(->)` (a one-element list
                     * whose item is the `->` symbol) or a bare `->` symbol map
                     * to a dedicated function-arrow constructor of kind
                     * * -> * -> *, represented as a TY_FN marker (arity 0).
                     * This is distinct from the opaque-struct fallback below:
                     * a method parameter typed by the class variable then
                     * resolves to a callable fat closure (see the param-type
                     * substitution further down), so an `Arrow [(->)]` instance
                     * whose body composes/applies its arguments type-checks. */
                    bool is_arrow_head = false;
                    if (arg->tag == F_SYM && arg->as.sym == e->sym_arrow) {
                        is_arrow_head = true;
                    } else if (arg->tag == F_LIST && arg->as.list.len == 1 &&
                               arg->as.list.items[0]->tag == F_SYM &&
                               arg->as.list.items[0]->as.sym == e->sym_arrow) {
                        is_arrow_head = true;
                    }
                    if (is_arrow_head) {
                        memset(&type_args[i], 0, sizeof(type_args[i]));
                        type_args[i].kind = TY_FN;
                        type_args[i].copy_kind = CK_COPY;
                        type_args[i].hkt_kind = KIND_ARROW2;  /* * -> * -> * */
                        /* Stable, C-safe mangling component (the raw `->`
                         * symbol would sanitise to "__"). */
                        type_arg_syms[i] = intern_cstr(e->st, "arrow");
                        continue;
                    }
                    /* Parse type keywords or symbols */
                    if (arg->tag == F_KEYWORD || arg->tag == F_SYM) {
                        const Symbol *kw = arg->as.sym;
                        if (kw->len == 3 && memcmp(kw->name, "int", 3) == 0) {
                            type_args[i] = TYPE_INT;
                        } else if (kw->len == 4 && memcmp(kw->name, "bool", 4) == 0) {
                            type_args[i] = TYPE_BOOL;
                        } else if (kw->len == 4 && memcmp(kw->name, "cstr", 4) == 0) {
                            type_args[i] = TYPE_CSTR;
                        } else if ((kw->len == 4 && memcmp(kw->name, "void", 4) == 0) ||
                                   (kw->len == 3 && memcmp(kw->name, "nil", 3) == 0)) {
                            type_args[i] = TYPE_NIL;
                        } else if (kw->len == 9 && memcmp(kw->name, "ptr<void>", 9) == 0) {
                            type_args[i] = TYPE_PTR_VOID;
                        } else {
                            /* Phase N4: Try new numeric type names first. */
                            TypeKind nk = typekind_from_symbol(kw->name);
                            if (nk != TY_UNKNOWN) {
                                type_args[i] = type_simple(nk, CK_COPY);
                            } else {
                                /* Check if this name refers to a known struct type.
                                 * If so, preserve the StructDef pointer so the kind
                                 * system can distinguish concrete structs (kind *)
                                 * from opaque type constructors (kind * -> *). */
                                Binding *sb = scope_lookup(e->scope, kw);
                                /* The instance head names a TYPE.  scope_lookup is
                                 * value-preferring, and for a single-variant record
                                 * ADT whose constructor shares the type's name --
                                 * every lowered `defstruct`, and a hand-written
                                 * `(defdata T [A] (T [...]))` -- it returns the
                                 * constructor FUNCTION (TY_FN), shadowing the type.
                                 * Consult the authoritative type-namespace lookup
                                 * to recover the ADT/struct type in that case.  We
                                 * still prefer a struct binding from scope_lookup
                                 * first, preserving the GADT/struct coexistence
                                 * (MF4) struct-preference. */
                                /* structdef-retirement DS-C: the TY_STRUCT
                                 * instance-head branches (scope binding and type
                                 * lookup) are dead -- a defstruct head is a
                                 * TY_ADT now.  Resolve via the type namespace
                                 * first (a defdata/defgadt/lowered-defstruct type
                                 * constructor -- preferred so the orphan check
                                 * credits the owning module), then the value
                                 * binding, else an unresolved tyvar. */
                                Type *head_ty = elab_lookup_type_by_name(e, kw);
                                if (head_ty && head_ty->kind == TY_ADT &&
                                    head_ty->as.adt_.def) {
                                    type_args[i] = *head_ty;
                                    type_arg_syms[i] = kw;
                                } else if (sb && sb->type.kind == TY_ADT && sb->type.as.adt_.def) {
                                    type_args[i] = sb->type;
                                    type_arg_syms[i] = kw;
                                } else {
                                    /* Phase HKT H3 / P3: unknown name -- an
                                     * unresolved type variable.  Emit a named
                                     * TY_TYVAR marked as an opaque kind-'* -> *'
                                     * constructor (a genuine kind-* tyvar keeps
                                     * hkt_kind == KIND_STAR, so the kind checker
                                     * can tell them apart); the symbol is tracked
                                     * via type_arg_syms[i] for method mangling. */
                                    type_args[i] = type_tyvar_named(kw->name);
                                    type_args[i].copy_kind = CK_MOVE;
                                    type_args[i].hkt_kind = KIND_ARROW;
                                }
                                type_arg_syms[i] = kw;
                            }
                        }
                    } else if (arg->tag == F_LIST &&
                               (arg->as.list.len == 2 || arg->as.list.len == 3)) {
                        /* Phase HKT §3: (constructor arg) — partial type application.
                         * Parses `(result int)` in type position as TY_APP where
                         * fn = TY_STRUCT(constructor, KIND_ARROW2) and arg = concrete type.
                         * This allows `(definstance Functor [(result int)] ...)`.
                         *
                         * T4 (trailing-parameter head): the 3-element hole form
                         * `(Ctor _ B)` / `(Ctor A _)` marks the *free* parameter
                         * with exactly one `_` and fixes the other.  This lets a
                         * kind-(* -> *) class fix a *trailing* parameter -- e.g.
                         * `(Result _ B)` holds the err arm B and varies the ok arm,
                         * which leftmost-only application (`(Result A)`) cannot
                         * express.  The varying element arm is erased to the int64
                         * carrier, so downstream only needs the constructor identity
                         * (dispatch + orphan check) and a valid (* -> *) head, which
                         * is exactly what fixing the named arm produces.  See
                         * docs/archive/history/result-param-order-blocks-functor-monad.md. */
                        Form *ctor_form = arg->as.list.items[0];
                        Form *aarg_form;
                        if (arg->as.list.len == 2) {
                            aarg_form = arg->as.list.items[1];
                        } else {
                            const Symbol *us = intern_cstr(e->st, "_");
                            Form *h1 = arg->as.list.items[1];
                            Form *h2 = arg->as.list.items[2];
                            bool h1_hole = (h1->tag == F_SYM || h1->tag == F_KEYWORD)
                                           && h1->as.sym == us;
                            bool h2_hole = (h2->tag == F_SYM || h2->tag == F_KEYWORD)
                                           && h2->as.sym == us;
                            /* Fully-applied 2-parameter head `(Ctor a b)` with NO
                             * `_` hole: BOTH arguments are bound -- each a concrete
                             * type or a head type variable (e.g. `cstr` and `V` in
                             * `(Map cstr V)`).  Build a nested, fully-applied TY_APP
                             * -- app(app(Ctor, a), b) -- of kind *, so a kind-*
                             * class stays kind * while a head tyvar binds to the
                             * receiver's matching slot at dispatch
                             * (m7_collect_tyvar_bindings unifies the nested head
                             * against the concrete receiver).  This is the kind-*
                             * counterpart to the single-`_` partial-application
                             * path below, which serves kind-(* -> *) classes.  See
                             * docs/archive/history/kind-star-instance-two-param-type-cannot-bind-constraint-var.md */
                            if (!h1_hole && !h2_hole) {
                                if (ctor_form->tag != F_SYM &&
                                    ctor_form->tag != F_KEYWORD) {
                                    diag_emit(DIAG_ERROR, ctor_form->span,
                                              "type application constructor must be a symbol");
                                    return NULL;
                                }
                                const Symbol *ctor_sym2 = ctor_form->as.sym;
                                Type a0, a1;
                                if (!parse_instance_head_arg(e, h1, &a0)) return NULL;
                                if (!parse_instance_head_arg(e, h2, &a1)) return NULL;
                                /* Constructor fn type carries its real def +
                                 * arity-derived kind (so dispatch matching and the
                                 * orphan check see the right constructor identity);
                                 * an unresolved ctor falls back to an opaque binary
                                 * constructor. */
                                Type *fn_type = (Type *)arena_alloc(e->arena, sizeof(Type));
                                memset(fn_type, 0, sizeof(Type));
                                Binding *ctor_b2 = scope_lookup(e->scope, ctor_sym2);
                                /* CONV-S1 (byvalue-field-ascribed-carrier-receiver):
                                 * scope_lookup is value-preferring, so for a lowered
                                 * record ADT (and any `(defdata T [A] (T [...]))`)
                                 * the type name `Duo` resolves to the constructor
                                 * FUNCTION (TY_FN), shadowing the type.  The else
                                 * branch then built a def-less TY_STRUCT base, so
                                 * `(.snd x)` on a `(Duo cstr int)` receiver in the
                                 * instance body unwrapped the app to a def-less
                                 * struct and the field never resolved.  Recover the
                                 * ADT/struct type from the type namespace, mirroring
                                 * the single-arg keyword head path above (struct
                                 * from scope_lookup still wins first, preserving the
                                 * MF4 struct/GADT coexistence preference). */
                                Type *head_ty2 = elab_lookup_type_by_name(e, ctor_sym2);
                                if (ctor_b2 && ctor_b2->type.kind == TY_ADT) {
                                    *fn_type = ctor_b2->type;
                                    uint32_t ca = ctor_b2->type.as.adt_.def->n_type_params;
                                    fn_type->hkt_kind = (ca > 0)
                                        ? kind_for_arity(ca) : KIND_ARROW2;
                                } else if (head_ty2 && head_ty2->kind == TY_ADT &&
                                           head_ty2->as.adt_.def) {
                                    *fn_type = *head_ty2;
                                    uint32_t ca = head_ty2->as.adt_.def->n_type_params;
                                    fn_type->hkt_kind = (ca > 0)
                                        ? kind_for_arity(ca) : KIND_ARROW2;
                                } else {
                                    /* structdef-retirement slice 5 B2: an
                                     * unresolved partial-app constructor head is
                                     * a named TY_TYVAR (kind '* -> * -> *' in
                                     * hkt_kind) rather than a def-less
                                     * TY_STRUCT; the name is carried for dispatch
                                     * / orphan-check identity. */
                                    *fn_type = type_tyvar_named(ctor_sym2->name);
                                    fn_type->copy_kind = CK_MOVE;
                                    fn_type->hkt_kind = KIND_ARROW2;
                                }
                                /* inner = app(Ctor, a0);  outer = app(inner, a1).
                                 * Each application steps the kind one rung down the
                                 * ladder, so a binary (ARROW2) constructor applied
                                 * to two args lands at kind * -- keeping a kind-*
                                 * class from being promoted to higher kind. */
                                Type *a0p = (Type *)arena_alloc(e->arena, sizeof(Type));
                                *a0p = a0;
                                Type *a1p = (Type *)arena_alloc(e->arena, sizeof(Type));
                                *a1p = a1;
                                Type *inner = (Type *)arena_alloc(e->arena, sizeof(Type));
                                memset(inner, 0, sizeof(Type));
                                inner->kind = TY_APP;
                                inner->copy_kind = CK_MOVE;
                                inner->hkt_kind = kind_apply_one(fn_type->hkt_kind);
                                inner->as.app.fn = fn_type;
                                inner->as.app.arg = a0p;
                                memset(&type_args[i], 0, sizeof(type_args[i]));
                                type_args[i].kind = TY_APP;
                                type_args[i].copy_kind = CK_MOVE;
                                type_args[i].hkt_kind = kind_apply_one(inner->hkt_kind);
                                type_args[i].as.app.fn = inner;
                                type_args[i].as.app.arg = a1p;
                                type_arg_syms[i] = ctor_sym2;
                                continue;
                            }
                            if (h1_hole == h2_hole) {
                                diag_emit(DIAG_ERROR, arg->span,
                                          "instance head has two '_' holes; exactly "
                                          "one parameter may be left free (e.g. (Result _ B))");
                                return NULL;
                            }
                            /* The fixed (non-hole) arm is the type argument. */
                            aarg_form = h1_hole ? h2 : h1;
                            /* Record the hole slot index (0 = first ctor param,
                             * 1 = second) so the by-value HKT grounding fixes the
                             * other slot from the concrete receiver. */
                            hkt_hole_pos = h1_hole ? 0 : 1;
                        }
                        if (ctor_form->tag != F_SYM && ctor_form->tag != F_KEYWORD) {
                            diag_emit(DIAG_ERROR, ctor_form->span,
                                      "type application constructor must be a symbol");
                            return NULL;
                        }
                        const Symbol *ctor_sym = ctor_form->as.sym;
                        /* Parse the argument type (must be a primitive or known sym) */
                        Type app_arg_type;
                        if (aarg_form->tag == F_SYM || aarg_form->tag == F_KEYWORD) {
                            const Symbol *akw = aarg_form->as.sym;
                            if (akw->len == 3 && memcmp(akw->name, "int", 3) == 0) {
                                app_arg_type = TYPE_INT;
                            } else if (akw->len == 4 && memcmp(akw->name, "bool", 4) == 0) {
                                app_arg_type = TYPE_BOOL;
                            } else if (akw->len == 4 && memcmp(akw->name, "cstr", 4) == 0) {
                                app_arg_type = TYPE_CSTR;
                            } else if ((akw->len == 4 && memcmp(akw->name, "void", 4) == 0) ||
                                       (akw->len == 3 && memcmp(akw->name, "nil", 3) == 0)) {
                                app_arg_type = TYPE_NIL;
                            } else {
                                /* ECS E2d-P6 (Issue 2 secondary): resolve a known
                                 * struct/ADT symbol to its real def, mirroring the
                                 * top-level type-arg parser above.  Without this an
                                 * applied head like `(Dense Pos)` recorded `Pos`
                                 * as an opaque NULL-def struct, so substituting the
                                 * instance type arg into an inherited param type
                                 * (`s : S` -> `(Dense Pos)`) produced an imprecise
                                 * receiver that failed to bind the helper's `(Dense
                                 * A)` type variable.  A primitive numeric name also
                                 * resolves here. */
                                TypeKind ank = typekind_from_symbol(akw->name);
                                Binding *asb = scope_lookup(e->scope, akw);
                                /* return-dispatched-sum-mint-in-constrained-instance-
                                 * miscompiles (repro 3): scope_lookup is value-
                                 * preferring, so a lowered defstruct's name resolves
                                 * to its constructor FUNCTION and `Box` in
                                 * `[(Option Box)]` fell through to a type VARIABLE
                                 * named Box.  Ask the type namespace as well. */
                                Type *aty = elab_lookup_type_by_name(e, akw);
                                if (ank != TY_UNKNOWN) {
                                    app_arg_type = type_simple(ank, CK_COPY);
                                } else if (asb && asb->type.kind == TY_ADT &&
                                           asb->type.as.adt_.def) {
                                    app_arg_type = asb->type;
                                } else if (aty && aty->kind == TY_ADT &&
                                           aty->as.adt_.def) {
                                    app_arg_type = *aty;
                                } else {
                                    /* Unknown name in an applied instance head
                                     * (`A` in `(Dense A)`) is the instance's own
                                     * type parameter -- a type *variable*, not a
                                     * concrete opaque struct.  Record it as a
                                     * NAMED TY_TYVAR (the same representation
                                     * type_expr_from_form uses for class/sig type
                                     * params) so its identity survives to the
                                     * call site: the parametric associated-type
                                     * projection (`(type Elem = A)`) and the
                                     * abi-binding grounding both unify this head
                                     * tyvar against the concrete receiver
                                     * (`(Dense Pos)` => `A -> Pos`).  A nameless
                                     * null-def struct here would erase `A` and
                                     * carrier-collapse a struct element.  Many
                                     * dispatch sites already accept either shape
                                     * (the open-binder-skolems named-TYVAR
                                     * migration); a concrete head arg still
                                     * resolves to its real def via the branches
                                     * above. */
                                    app_arg_type = type_tyvar_named(akw->name);
                                }
                            }
                        } else {
                            diag_emit(DIAG_ERROR, aarg_form->span,
                                      "type application argument must be a type keyword or symbol");
                            return NULL;
                        }
                        /* Build fn type for the constructor being applied.
                         * If the constructor names a real defdata/defstruct in
                         * scope (e.g. `Either` in `(Either E)`), preserve its
                         * TY_ADT/TY_STRUCT identity -- including the def's
                         * origin_file_id -- so the orphan-instance check can
                         * credit the owning module.  Otherwise fall back to an
                         * opaque KIND_ARROW2 TY_STRUCT (e.g. `result`/`vec`). */
                        Type *fn_type = (Type *)arena_alloc(e->arena, sizeof(Type));
                        memset(fn_type, 0, sizeof(Type));
                        Binding *ctor_b = scope_lookup(e->scope, ctor_sym);
                        /* The applied head names a TYPE constructor.  scope_lookup
                         * is value-preferring and for a single-variant record ADT
                         * whose constructor shares the type's name (every lowered
                         * defstruct -- e.g. `(Result _ B)` once Result lowers)
                         * returns the constructor FUNCTION (TY_FN), so the
                         * TY_ADT/TY_STRUCT branch below would miss and fall to the
                         * opaque `<struct>` fallback -- erasing the ADT identity and
                         * breaking `.is-ok` field access in the method body.  Resolve
                         * the head type authoritatively via the type namespace,
                         * preferring a struct binding from scope_lookup first (MF4
                         * struct-preference). */
                        Type head_ct;
                        bool have_head_ct = false;
                        {
                            Type *ht = elab_lookup_type_by_name(e, ctor_sym);
                            if (ht && ht->kind == TY_ADT && ht->as.adt_.def) {
                                head_ct = *ht; have_head_ct = true;
                            } else if (ctor_b && ctor_b->type.kind == TY_ADT) {
                                head_ct = ctor_b->type; have_head_ct = true;
                            }
                        }
                        /* The constructor's kind follows its real arity: a unary
                         * constructor (Option : * -> *) applied to one arg is a
                         * fully-applied type of kind *, while a binary one
                         * (Result : * -> * -> *) applied to one arg is still a
                         * (* -> *) constructor.  Derive the kind from the def's
                         * n_type_params when known; fall back to the legacy binary
                         * assumption only for an opaque (def == NULL) constructor. */
                        if (have_head_ct) {
                            *fn_type = head_ct;
                            uint32_t ctor_arity = head_ct.as.adt_.def->n_type_params;
                            fn_type->hkt_kind = (ctor_arity > 0)
                                ? kind_for_arity(ctor_arity)
                                : KIND_ARROW2;
                        } else {
                            /* structdef-retirement slice 5 B2: an unresolved
                             * partial-app constructor head is a named TY_TYVAR
                             * (kind '* -> * -> *' in hkt_kind) rather than a
                             * def-less TY_STRUCT; carry the name for dispatch /
                             * orphan-check identity. */
                            *fn_type = type_tyvar_named(ctor_sym->name);
                            fn_type->copy_kind = CK_MOVE;
                            fn_type->hkt_kind = KIND_ARROW2;
                        }
                        /* Build arg type on arena */
                        Type *arg_type_ptr = (Type *)arena_alloc(e->arena, sizeof(Type));
                        *arg_type_ptr = app_arg_type;
                        /* Assemble TY_APP */
                        memset(&type_args[i], 0, sizeof(type_args[i]));
                        type_args[i].kind = TY_APP;
                        /* return-dispatched-sum-mint-in-constrained-instance-
                         * miscompiles (repro 3): a RESOLVED head takes the
                         * discipline `(Option Box)` has everywhere else (copy,
                         * or the opaque's own linear/affine lift); the blanket
                         * CK_MOVE made the instance method's own parameter
                         * move-only, so `(some? x)` consumed it and `(unwrap
                         * x)` was a use-after-move -- the same body as a defn
                         * is accepted.  An unresolved head keeps CK_MOVE. */
                        if (have_head_ct) {
                            type_args[i].copy_kind = CK_COPY;
                            propagate_app_discipline(&type_args[i], fn_type);
                        } else {
                            type_args[i].copy_kind = CK_MOVE;
                        }
                        /* Result kind = constructor kind with one arg applied
                         * (ARROW2 -> ARROW for a binary head; ARROW -> STAR for a
                         * fully-applied unary head). */
                        type_args[i].hkt_kind = kind_apply_one(fn_type->hkt_kind);
                        type_args[i].as.app.fn  = fn_type;
                        type_args[i].as.app.arg = arg_type_ptr;
                        /* Store constructor sym for name mangling (e.g. "result") */
                        type_arg_syms[i] = ctor_sym;
                    } else {
                        diag_emit(DIAG_ERROR, arg->span,
                                  "unsupported type argument in definstance");
                        return NULL;
                    }
                }
            }
            /* Phase HKT-P1: After parsing individual type arguments, combine consecutive
             * symbols into TY_APP for implicit type application syntax [result int].
             * This allows both [(result int)] (explicit) and [result int] (implicit). */
            if (n_type_args > 0) {
                for (uint8_t i = 0; i < n_type_args; ) {
                    if (i + 1 < n_type_args) {
                        /* Check if current is a tyvar placeholder (potential
                         * higher-kinded constructor head when applied to a next
                         * arg) and next is a type.  Accepts the legacy anonymous
                         * TY_STRUCT{def=NULL} shape and the named TY_TYVAR
                         * introduced by Direction A step 2a. */
                        if (type_args[i].kind == TY_TYVAR) {
                            Type *next_type = &type_args[i + 1];
                            /* Next can be any concrete type (primitive, TY_STRUCT, or TY_APP) */
                            if (next_type->kind != TY_UNKNOWN) {
                                /* Combine into TY_APP */
                                Type *fn_type = (Type *)arena_alloc(e->arena, sizeof(Type));
                                *fn_type = type_args[i];  /* Copy the constructor type */
                                fn_type->hkt_kind = KIND_ARROW2;  /* Assume binary constructor */
                                
                                Type *arg_type_ptr = (Type *)arena_alloc(e->arena, sizeof(Type));
                                *arg_type_ptr = type_args[i + 1];
                                
                                /* Create TY_APP */
                                type_args[i].kind = TY_APP;
                                type_args[i].copy_kind = CK_MOVE;
                                type_args[i].hkt_kind = KIND_ARROW;  /* ARROW2 applied to 1 arg */
                                type_args[i].as.app.fn = fn_type;
                                type_args[i].as.app.arg = arg_type_ptr;
                                /* Store constructor sym for name mangling if we have it */
                                /* type_arg_syms[i] already contains the constructor symbol */
                                
                                /* Remove the second type arg by shifting */
                                for (uint8_t j = i + 1; j < n_type_args - 1; j++) {
                                    type_args[j] = type_args[j + 1];
                                    if (type_arg_syms) {
                                        type_arg_syms[j] = type_arg_syms[j + 1];
                                    }
                                }
                                n_type_args--;
                                /* Don't advance i - recheck current position */
                                continue;
                            }
                        }
                    }
                    i++;
                }
            }
            impls_start = 3;
        }
    }
    
    /* Phase PTC1: Parse type parameter constraints (optional)
     * Syntax: (definstance Clone [Pair a b] [(Clone a) (Clone b)] (clone [x] ...))
     * Constraint vector is a vector of lists: [(Clone a) (Clone b)]
     * Each constraint is a list (Clone a) where Clone is the typeclass and a is the type param.
     * After type args at index 2, check for a constraint vector at index impls_start.
     */
    TypeConstraint *type_param_constraints = NULL;
    uint8_t n_type_param_constraints = 0;
    /* Names of the tyvars introduced by the constraint vector (e.g. `A` in
     * `[(Eq A)]`).  These must be in scope as type variables while elaborating
     * the instance method bodies so that a bare `A` in an ascription such as
     * `(:: t1 (Cons A))` resolves to the constraint tyvar rather than a
     * same-named global type.  Pushed onto e->sig_tyvars for pass 2.  See
     * docs/archive/history/m5-suite-residual-6-failures-2026-06-14.md (root cause A). */
    const Symbol *constraint_tyvar_syms[32];
    uint8_t n_constraint_tyvar_syms = 0;

    if (call->as.list.len > impls_start) {
        Form *next_form = call->as.list.items[impls_start];
        if (next_form->tag == F_VEC && next_form->as.list.len > 0) {
            /* Check if this is a constraint vector by looking at the first item */
            /* If the first item is a list (F_LIST), it's likely [(Clone a) (Clone b)] */
            /* If the first item is a symbol (F_SYM), it might be a flat [Clone a Clone b] vector */
            bool is_constraint_vector = false;
            if (next_form->as.list.len > 0) {
                Form *first_item = next_form->as.list.items[0];
                if (first_item->tag == F_LIST) {
                    /* Vector of lists format: [(Clone a) (Clone b)] */
                    is_constraint_vector = true;
                    n_type_param_constraints = next_form->as.list.len;
                } else if (next_form->as.list.len >= 2) {
                    /* Could be flat format [Clone a Clone b ...] */
                    /* Check if all items alternate between SYM (typeclass) and SYM/KEYWORD (type arg) */
                    is_constraint_vector = true;
                    n_type_param_constraints = next_form->as.list.len / 2;
                }
            }
            
            if (is_constraint_vector && n_type_param_constraints > 0) {
                type_param_constraints = (TypeConstraint *)arena_alloc(
                    e->arena, n_type_param_constraints * sizeof(TypeConstraint));
                
                Form *first_item = next_form->as.list.items[0];
                if (first_item->tag == F_LIST) {
                    /* Parse as vector of lists: [(Clone a) (Clone b)] */
                    for (uint8_t i = 0; i < n_type_param_constraints; i++) {
                        Form *constraint_form = next_form->as.list.items[i];
                        if (constraint_form->tag != F_LIST || constraint_form->as.list.len < 1) {
                            diag_emit(DIAG_ERROR, constraint_form->span,
                                      "definstance: constraint must be a list like (Clone a), got tag %d with %d items",
                                      constraint_form->tag, constraint_form->as.list.len);
                            return NULL;
                        }
                        
                        Form *tc_name_form = constraint_form->as.list.items[0];
                        if (tc_name_form->tag != F_SYM) {
                            diag_emit(DIAG_ERROR, tc_name_form->span,
                                      "definstance: constraint typeclass name must be a symbol");
                            return NULL;
                        }
                        
                        TypeClass *constraint_tc = typeclass_env_lookup_typeclass(
                            &e->typeclass_env, tc_name_form->as.sym);
                        if (!constraint_tc) {
                            diag_emit(DIAG_ERROR, tc_name_form->span,
                                      "definstance: constraint typeclass '%s' is not defined",
                                      tc_name_form->as.sym->name);
                            return NULL;
                        }
                        
                        /* Type argument being constrained (optional, at index 1) */
                        Type constrained_type = TYPE_INT; /* Default */
                        int8_t p_idx = -1;
                        const Symbol *ct_var = NULL;
                        if (constraint_form->as.list.len >= 2) {
                            Form *type_arg_form = constraint_form->as.list.items[1];
                            /* A keyword spelling (`[TC :cstr]`) names a type
                             * exactly as the instance head's own parser
                             * accepts it.  Anything else is not a type at
                             * all -- reported below rather than left on the
                             * `TYPE_INT` initializer. */
                            if (type_arg_form->tag == F_SYM ||
                                type_arg_form->tag == F_KEYWORD) {
                                const Symbol *type_param_name = type_arg_form->as.sym;
                                if (n_constraint_tyvar_syms < 32) {
                                    bool dup = false;
                                    for (uint8_t d = 0; d < n_constraint_tyvar_syms; d++) {
                                        if (constraint_tyvar_syms[d] == type_param_name) {
                                            dup = true; break;
                                        }
                                    }
                                    if (!dup)
                                        constraint_tyvar_syms[n_constraint_tyvar_syms++] =
                                            type_param_name;
                                }
                                /* M5 gap 4: also record the constraint var on the
                                 * TypeConstraint so the emit composition pass can
                                 * resolve it to its concrete element type. */
                                ct_var = type_param_name;
                                bool found = false;
                                for (uint8_t j = 0; j < n_type_args; j++) {
                                    if (type_arg_syms && type_arg_syms[j] &&
                                        type_arg_syms[j] == type_param_name) {
                                        constrained_type = type_args[j];
                                        found = true;
                                        break;
                                    }
                                }
                                if (!found)
                                    found = constraint_prim_type(type_param_name,
                                                                 &constrained_type);
                                /* CONV-S2: under defstruct-as-defadt the instance
                                 * head is a lowered record ADT (`[Cons]`/`[Vec]`),
                                 * so the constraint var (`A` in `[(Tag A)]`) is one
                                 * of the ADT's type params, not a struct's.  Mirror
                                 * the struct lookup so its param_idx is recorded;
                                 * without it p_idx stays -1 and the emit-side
                                 * constraint-var->element mapping (a helper call or
                                 * lifted closure inside the instance body) never
                                 * grounds A, baking the carrier representative. */
                                if (!found) {
                                    for (uint8_t j = 0; j < n_type_args && p_idx < 0; j++) {
                                        /* An APPLIED head (`[(Option A)]`) is a
                                         * TY_APP over the same ADT, and binds
                                         * its type params exactly as a bare one
                                         * does -- peel to the constructor so `A`
                                         * is recognised there too, instead of
                                         * falling through to the type-name
                                         * lookup and being reported unknown. */
                                        AdtDef *adef = constraint_head_adt(&type_args[j]);
                                        if (adef) {
                                            for (uint8_t k = 0; k < adef->n_type_params; k++) {
                                                if (adef->type_params[k] &&
                                                    strcmp(adef->type_params[k],
                                                           type_param_name->name) == 0) {
                                                    p_idx = (int8_t)k;
                                                    break;
                                                }
                                            }
                                        }
                                    }
                                }
                                /* A user-defined type name (`[TC MyStruct]`).
                                 * Resolved last so a name that is also a head
                                 * type arg or an ADT type parameter keeps
                                 * meaning the parameter, as it did before. */
                                if (!found && p_idx < 0)
                                    found = constraint_named_type(e, type_param_name,
                                                                  &constrained_type);
                                /* Never default silently: a constraint type we
                                 * cannot resolve used to keep the `TYPE_INT`
                                 * initializer, so the constraint was checked --
                                 * or silently satisfied -- against a type that
                                 * appears nowhere in the source. */
                                if (!found && p_idx < 0) {
                                    diag_emit(DIAG_ERROR, type_arg_form->span,
                                              "definstance: constraint type '%s' is not a known "
                                              "type or type parameter",
                                              type_param_name->name);
                                    return NULL;
                                }
                            } else {
                                diag_emit(DIAG_ERROR, type_arg_form->span,
                                          "definstance: constraint type must be a type name, "
                                          "got a %s literal",
                                          form_tag_name(type_arg_form->tag));
                                return NULL;
                            }
                        }

                        type_param_constraints[i] = (TypeConstraint){
                            .typeclass = constraint_tc,
                            .type_arg = constrained_type,
                            .param_idx = p_idx,
                            .tyvar = ct_var
                        };
                    }
                } else {
                    /* Parse as flat vector: [Clone a Clone b ...] */
                    uint8_t n_items = next_form->as.list.len;
                    if (n_items % 2 != 0) {
                        diag_emit(DIAG_ERROR, next_form->span,
                                  "definstance: flat constraint vector must have an even number of items");
                        return NULL;
                    }
                    n_type_param_constraints = n_items / 2;
                    /* Reallocate for flat format - old allocation will be GC'd with arena */
                    type_param_constraints = (TypeConstraint *)arena_alloc(
                        e->arena, n_type_param_constraints * sizeof(TypeConstraint));
                    
                    for (uint8_t i = 0; i < n_type_param_constraints; i++) {
                        uint8_t idx = i * 2;
                        Form *tc_name_form = next_form->as.list.items[idx];
                        if (tc_name_form->tag != F_SYM) {
                            diag_emit(DIAG_ERROR, tc_name_form->span,
                                      "definstance: constraint typeclass name must be a symbol");
                            return NULL;
                        }
                        
                        TypeClass *constraint_tc = typeclass_env_lookup_typeclass(
                            &e->typeclass_env, tc_name_form->as.sym);
                        if (!constraint_tc) {
                            diag_emit(DIAG_ERROR, tc_name_form->span,
                                      "definstance: constraint typeclass '%s' is not defined",
                                      tc_name_form->as.sym->name);
                            return NULL;
                        }
                        
                        Type constrained_type = TYPE_INT;
                        int8_t p_idx = -1;
                        const Symbol *ct_var = NULL;
                        if (idx + 1 < n_items) {
                            Form *type_arg_form = next_form->as.list.items[idx + 1];
                            /* A keyword spelling (`[TC :cstr]`) names a type
                             * exactly as the instance head's own parser
                             * accepts it.  Anything else is not a type at
                             * all -- reported below rather than left on the
                             * `TYPE_INT` initializer. */
                            if (type_arg_form->tag == F_SYM ||
                                type_arg_form->tag == F_KEYWORD) {
                                const Symbol *type_param_name = type_arg_form->as.sym;
                                ct_var = type_param_name;
                                bool found = false;
                                for (uint8_t j = 0; j < n_type_args; j++) {
                                    if (type_arg_syms && type_arg_syms[j] &&
                                        type_arg_syms[j] == type_param_name) {
                                        constrained_type = type_args[j];
                                        found = true;
                                        break;
                                    }
                                }
                                if (!found)
                                    found = constraint_prim_type(type_param_name,
                                                                 &constrained_type);
                                /* CONV-S2: lowered record ADT instance head -- see
                                 * the paren-format block above. */
                                if (!found) {
                                    for (uint8_t j = 0; j < n_type_args && p_idx < 0; j++) {
                                        /* An APPLIED head (`[(Option A)]`) is a
                                         * TY_APP over the same ADT, and binds
                                         * its type params exactly as a bare one
                                         * does -- peel to the constructor so `A`
                                         * is recognised there too, instead of
                                         * falling through to the type-name
                                         * lookup and being reported unknown. */
                                        AdtDef *adef = constraint_head_adt(&type_args[j]);
                                        if (adef) {
                                            for (uint8_t k = 0; k < adef->n_type_params; k++) {
                                                if (adef->type_params[k] &&
                                                    strcmp(adef->type_params[k],
                                                           type_param_name->name) == 0) {
                                                    p_idx = (int8_t)k;
                                                    break;
                                                }
                                            }
                                        }
                                    }
                                }
                                /* A user-defined type name (`[TC MyStruct]`).
                                 * Resolved last so a name that is also a head
                                 * type arg or an ADT type parameter keeps
                                 * meaning the parameter, as it did before. */
                                if (!found && p_idx < 0)
                                    found = constraint_named_type(e, type_param_name,
                                                                  &constrained_type);
                                /* Never default silently: a constraint type we
                                 * cannot resolve used to keep the `TYPE_INT`
                                 * initializer, so the constraint was checked --
                                 * or silently satisfied -- against a type that
                                 * appears nowhere in the source. */
                                if (!found && p_idx < 0) {
                                    diag_emit(DIAG_ERROR, type_arg_form->span,
                                              "definstance: constraint type '%s' is not a known "
                                              "type or type parameter",
                                              type_param_name->name);
                                    return NULL;
                                }
                            } else {
                                diag_emit(DIAG_ERROR, type_arg_form->span,
                                          "definstance: constraint type must be a type name, "
                                          "got a %s literal",
                                          form_tag_name(type_arg_form->tag));
                                return NULL;
                            }
                        }

                        type_param_constraints[i] = (TypeConstraint){
                            .typeclass = constraint_tc,
                            .type_arg = constrained_type,
                            .param_idx = p_idx,
                            .tyvar = ct_var
                        };
                    }
                }
                impls_start++; /* Skip past the constraint vector */
            }
        }
    }

    /* Phase PTC2: Validate type parameter constraints */
    if (type_param_constraints && n_type_param_constraints > 0) {
        for (uint8_t i = 0; i < n_type_param_constraints; i++) {
            /* PTC4/PTC6: constraints with param_idx >= 0 refer to struct type params
             * whose concrete types are not known at definstance time; skip PTC2. */
            if (type_param_constraints[i].param_idx >= 0) continue;
            TypeClass *constraint_tc = type_param_constraints[i].typeclass;
            Type constrained_type = type_param_constraints[i].type_arg;
            bool is_primitive = (constrained_type.kind == TY_INT ||
                                 constrained_type.kind == TY_BOOL ||
                                 constrained_type.kind == TY_CSTR ||
                                 constrained_type.kind == TY_NIL ||
                                 constrained_type.kind == TY_FLOAT ||
                                 constrained_type.kind == TY_PTR_VOID);
            /* A non-parametric user-defined type (`[TC MyStruct]`) names one
             * concrete type, so its instance can be looked up here on the same
             * terms as a primitive's.  A parametric one (`Vec<A>` named bare)
             * still defers -- the element type is not known yet. */
            if (!is_primitive && constrained_type.kind == TY_ADT &&
                constrained_type.as.adt_.def &&
                constrained_type.as.adt_.def->n_type_params == 0)
                is_primitive = true;

            /* PTC2: For primitive types, validate that a constraint instance exists.
             * For user-defined types (structs, etc.), defer validation to PTC3.
             * Phase B1: float is treated as a primitive for constraint purposes. */
            if (is_primitive) {
                Type lookup_type = constrained_type;
                if (constrained_type.kind == TY_BOOL) {
                    lookup_type = TYPE_BOOL;
                } else if (constrained_type.kind == TY_CSTR) {
                    lookup_type = TYPE_CSTR;
                } else if (constrained_type.kind == TY_NIL) {
                    lookup_type = TYPE_NIL;
                } else if (constrained_type.kind == TY_PTR_VOID) {
                    lookup_type = TYPE_PTR_VOID;
                } else if (constrained_type.kind == TY_FLOAT) {
                    lookup_type = TYPE_FLOAT;
                }
                
                TypeClassInstance *inst = typeclass_env_lookup_instance(
                    &e->typeclass_env, constraint_tc, &lookup_type, 1);
                if (!inst) {
                    diag_emit_with_code(DIAG_ERROR, call->span,
                        TUR_E0015_TYPECLASS_CONSTRAINT_NOT_SATISFIED,
                        "typeclass constraint not satisfied: no instance of '%s' for type '%s'",
                        constraint_tc->name->name, type_name(constrained_type));
                    return NULL;
                }
            }
            /* For user-defined types, the constraint is stored on the instance
             * but not validated here (deferred to PTC3 for constraint propagation). */
        }
    }

    /* Validate type argument count matches typeclass parameters */
    if (n_type_args != tc->n_type_params) {
        diag_emit(DIAG_ERROR, call->span,
                  "definstance: expected %d type arguments for '%s', got %d",
                  tc->n_type_params, tc_name->name, n_type_args);
        return NULL;
    }

    /* Phase HKT H1: Kind constraint validation.
     * If the typeclass has kind-annotated parameters (type_param_kinds != NULL),
     * verify that each type argument satisfies the expected kind.
     * Primitive types (int, bool, cstr, nil, float) have kind *.
     * Struct types and other user-defined types are treated as kind * -> *.
     * A definstance that supplies a primitive where kind '* -> *' is expected
     * is a compile-time error (TUR-E0012). */
    if (tc->type_param_kinds != NULL) {
        for (uint8_t i = 0; i < n_type_args; i++) {
            Kind expected = tc->type_param_kinds[i];
            if (expected == KIND_ARROW || expected == KIND_ARROW2) {
                TypeKind tk = type_args[i].kind;
                bool is_primitive = (tk == TY_INT  || tk == TY_BOOL  || tk == TY_CSTR ||
                                     tk == TY_NIL  || tk == TY_FLOAT || tk == TY_PTR_VOID);
                if (is_primitive) {
                    diag_emit_with_code(DIAG_ERROR, call->span, TUR_E0012_KIND_MISMATCH,
                        "kind mismatch (TUR-E0012): typeclass '%s' parameter %d expects kind "
                        "'%s' (a type constructor), but '%s' has kind '*'",
                        tc_name->name, (int)(i + 1),
                        kind_to_string(expected),
                        type_name(type_args[i]));
                    return NULL;
                }
            }
        }
    }

    /* Compute the instance's codegen type-arg suffix once -- it depends only on
     * the type args, not on any individual method, and is reused both for the
     * duplicate-instance guard just below and for every method name built later. */
    char inst_type_suffix[64];
    if (!build_inst_type_suffix(type_args, type_arg_syms, n_type_args,
                                inst_type_suffix, sizeof(inst_type_suffix))) {
        diag_emit(DIAG_ERROR, call->span,
                  "typeclass instance name is too long for '%s'", tc_name->name);
        return NULL;
    }

    /* Idempotent re-instance guard.  An instance whose (typeclass, type-arg
     * suffix) matches one already registered would re-emit the same dictionary
     * struct/singleton and __inst_* method functions, producing a hard C
     * "redefinition" ODR error.  This fires whenever the same instance is seen
     * twice -- e.g. a module that explicitly (load "stdlib/typeclass.tur")s
     * while an auto-loaded partial typeclass stub (typeclass-clone.tur, ...)
     * already supplied the same primitive instance.  The first definition wins;
     * the redundant one is a silent no-op, matching the include-guard mental
     * model for repeated loads.  See docs/archive/history/load-not-idempotent-typeclass.md. */
    for (TypeClassInstance **link = &e->typeclass_env.instances; *link; link = &(*link)->next) {
        TypeClassInstance *prev = *link;
        if (prev->typeclass != tc || prev->n_type_args != n_type_args) continue;
        char prev_suffix[64];
        if (!build_inst_type_suffix(prev->type_args, prev->type_arg_syms,
                                    prev->n_type_args, prev_suffix, sizeof(prev_suffix)))
            continue;
        if (strcmp(prev_suffix, inst_type_suffix) == 0) {
            /* duplicate-instance-silently-drops-a-user-definstance: the guard
             * above cannot tell "the same stdlib file loaded twice" (its
             * reason to exist, and rightly silent) from "a USER file defining
             * an instance the autoloaded stdlib already covers" -- where
             * silence meant `(definstance Eq [int] ...)` was accepted and had
             * no effect, and `(.eq? 3 3)` kept answering from the stdlib.  The
             * two differ by WHERE the second definition lives: a stdlib file
             * (autoloaded or explicitly `(load "stdlib/...")`-ed, which is why
             * `in_stdlib_load` alone is not the signal) stays silent; anything
             * else is REJECTED.  Decided 2026-09-11: an exact duplicate is an
             * error (what Haskell and Rust do with overlapping instances), not
             * a replacement -- replacing would change which code runs inside
             * the stdlib itself for a primitive type.  T1's "user shadows
             * stdlib" stays what it is: a tie-break among AMBIGUOUS candidates,
             * never a licence to redefine an exact one.  The override route is
             * a newtype, which the diagnostic's long text names. */
            const SourceFile *dup_sf = diag_source_file(call->span.file_id);
            bool in_stdlib_file = dup_sf && dup_sf->path && strstr(dup_sf->path, "stdlib/");
            /* PS4 (playground-session-hygiene-plan): an instance an earlier
             * REPL/playground turn defined is replaced -- unlinked here, then
             * registered afresh below -- so re-running a program does not fail
             * on the instance its previous run left.  The stdlib's own
             * instances keep the refusal above, for the reason given there. */
            if (!in_stdlib_file && !e->in_stdlib_load &&
                elab_prior_turn_instance(e, prev) &&
                !elab_file_is_stdlib(prev->origin_file_id)) {
                *link = prev->next;
                if (e->turn_start_instances == prev)
                    e->turn_start_instances = prev->next;
                break;
            }
            if (!in_stdlib_file && !e->in_stdlib_load) {
                diag_emit_with_code(DIAG_ERROR, call->span,
                    TUR_E0025_DUPLICATE_INSTANCE,
                    "instance %s [%s] is already defined%s; a class has one "
                    "instance per type -- to give this type different behaviour, "
                    "wrap it in a newtype (defopaque) and define the instance for that",
                    tc_name->name,
                    n_type_args > 0 ? type_name(type_args[0]) : "",
                    (prev->origin_file_id != call->span.file_id &&
                     diag_file_path(prev->origin_file_id) &&
                     strstr(diag_file_path(prev->origin_file_id), "stdlib/"))
                        ? " by the stdlib" : "");
                return NULL;
            }
            /* Already have this exact instance (a stdlib file loaded twice);
             * emit nothing further. */
            return e_nil(e, call->span);
        }
    }

    /* assoc-types-2 (Part A / MP5): functional-dependency coherence.  When the
     * class declares `| (from -> to)`, two instances that agree on every `from`
     * parameter must agree on every `to` parameter -- otherwise `to` is not
     * functionally determined by `from`.  Reject the new instance if an existing
     * one shares its `from` projection but disagrees on `to`.  (An exact
     * duplicate would already have been swallowed by the idempotent guard
     * above, so any match reaching here is a genuine conflict.) */
    if (tc->has_fundep && n_type_args == tc->n_type_params) {
        for (TypeClassInstance *prev = e->typeclass_env.instances; prev; prev = prev->next) {
            if (prev->typeclass != tc || prev->n_type_args != n_type_args) continue;
            bool from_eq = true;
            for (uint8_t i = 0; i < n_type_args; i++) {
                if (!(tc->fundep_from_mask & (uint16_t)(1u << i))) continue;
                if (!type_eq(prev->type_args[i], type_args[i])) { from_eq = false; break; }
            }
            if (!from_eq) continue;
            for (uint8_t i = 0; i < n_type_args; i++) {
                if (!(tc->fundep_to_mask & (uint16_t)(1u << i))) continue;
                if (!type_eq(prev->type_args[i], type_args[i])) {
                    diag_emit(DIAG_ERROR, call->span,
                              "functional dependency violated: '%s' already has an "
                              "instance with this determining type but a different "
                              "determined type",
                              tc_name->name);
                    return NULL;
                }
            }
        }
    }

    /* assoc-types-plan: an instance body interleaves method implementations
     * with associated-type bindings `(type Name = <type-expr>)`.  Classify the
     * forms after impls_start: a `type` head whose second element is a bare
     * symbol is an associated-type binding; everything else is a method impl.
     * Bindings are resolved after the instance is registered (so the type
     * expression may reference the instance's own type args). */
    uint32_t n_inst_body = call->as.list.len - impls_start;
    Form **method_impl_forms = n_inst_body > 0
        ? (Form **)arena_alloc(e->arena, n_inst_body * sizeof(Form *)) : NULL;
    uint32_t n_method_impl_forms = 0;
    const Symbol **assoc_bind_names = n_inst_body > 0
        ? (const Symbol **)arena_alloc(e->arena, n_inst_body * sizeof(const Symbol *))
        : NULL;
    Form **assoc_bind_forms = n_inst_body > 0
        ? (Form **)arena_alloc(e->arena, n_inst_body * sizeof(Form *)) : NULL;
    uint32_t n_assoc_binds = 0;
    {
        const Symbol *sym_type_kw = intern_cstr(e->st, "type");
        for (uint32_t bi = impls_start; bi < call->as.list.len; bi++) {
            Form *bf = call->as.list.items[bi];
            if (bf->tag == F_LIST && bf->as.list.len >= 2 &&
                bf->as.list.items[0]->tag == F_SYM &&
                bf->as.list.items[0]->as.sym == sym_type_kw &&
                bf->as.list.items[1]->tag == F_SYM) {
                /* (type Name = <type-expr>)  -- an optional `=` token may sit
                 * between the name and the type expression. */
                const Symbol *sym_eq = intern_cstr(e->st, "=");
                uint32_t te_idx = 2;
                if (bf->as.list.len > te_idx &&
                    bf->as.list.items[te_idx]->tag == F_SYM &&
                    bf->as.list.items[te_idx]->as.sym == sym_eq) {
                    te_idx++;
                }
                if (bf->as.list.len <= te_idx) {
                    diag_emit(DIAG_ERROR, bf->span,
                              "associated type binding requires a type: "
                              "(type %s = <type>)", bf->as.list.items[1]->as.sym->name);
                    return NULL;
                }
                assoc_bind_names[n_assoc_binds] = bf->as.list.items[1]->as.sym;
                assoc_bind_forms[n_assoc_binds] = bf->as.list.items[te_idx];
                n_assoc_binds++;
                continue;
            }
            method_impl_forms[n_method_impl_forms++] = bf;
        }
    }

    /* Parse method implementations */
    /* Each method impl is a function definition without the 'defn' keyword */
    /* Syntax: (method-name [param1 param2 ...] body...)
     * The number of methods must match the typeclass definition.
     */

    /* typeclass-default-methods-do-not-work: line the provided impls up in
     * CLASS ORDER by name, and fill a method the instance omits from the
     * class's default form.  The default form has the exact shape of an
     * instance method form -- `(name [params] : ret body...)` -- so from here on
     * it is indistinguishable from one the instance wrote, and elaborates with
     * the receiver at this instance's type and its siblings resolvable.  A
     * method with neither an impl nor a default is the error this used to
     * report as a bare count. */
    /* default-method-spliced-at-carrier-type: which slots were filled from the
     * class's default form rather than written by the instance.  The spliced
     * form carries the CLASS's annotations (`[x : a y : a] : a`), and those
     * elaborate literally -- `a` is a type variable, which lowers to the int64
     * carrier -- so the spliced copy's signature was `int64_t (int64_t,
     * int64_t)` while the explicit `(pick [x y] x)` an instance writes gets
     * `double (double, double)`.  Same dispatch, wrong signature: the
     * caller's doubles were converted on the way in, and `(g 2.5 7.1)`
     * through a constrained generic printed `2`.  The per-method loop below
     * treats a spliced default's annotations as the class's own (which they
     * are) and lets the parameters and result inherit the class signature
     * under the instance substitution, exactly like a bare-parameter method. */
    bool *impl_is_default_arr = tc->n_methods
        ? (bool *)arena_alloc(e->arena, tc->n_methods * sizeof(bool)) : NULL;
    if (impl_is_default_arr) memset(impl_is_default_arr, 0, tc->n_methods * sizeof(bool));
    {
        Form **ordered = tc->n_methods
            ? (Form **)arena_alloc(e->arena, tc->n_methods * sizeof(Form *)) : NULL;
        for (uint32_t k = 0; k < n_method_impl_forms; k++) {
            Form *pf = method_impl_forms[k];
            if (pf->tag != F_LIST || pf->as.list.len < 1 || pf->as.list.items[0]->tag != F_SYM)
                continue;   /* the per-method loop below reports the shape */
            bool known = false;
            for (uint8_t i = 0; i < tc->n_methods && !known; i++)
                known = (pf->as.list.items[0]->as.sym == tc->methods[i].name);
            if (!known) {
                diag_emit(DIAG_ERROR, pf->as.list.items[0]->span,
                          "method implementation name '%s' doesn't match any method of '%s'",
                          pf->as.list.items[0]->as.sym->name, tc_name->name);
                return NULL;
            }
        }
        for (uint8_t i = 0; i < tc->n_methods; i++) {
            Form *found = NULL;
            for (uint32_t k = 0; k < n_method_impl_forms && !found; k++) {
                Form *pf = method_impl_forms[k];
                if (pf->tag == F_LIST && pf->as.list.len >= 1 &&
                    pf->as.list.items[0]->tag == F_SYM &&
                    pf->as.list.items[0]->as.sym == tc->methods[i].name)
                    found = pf;
            }
            if (!found && tc->methods[i].default_method_form) {
                found = (Form *)tc->methods[i].default_method_form;
                impl_is_default_arr[i] = true;
            }
            if (!found) {
                diag_emit(DIAG_ERROR, call->span,
                          "definstance: missing method '%s' for '%s', and the class "
                          "declares no default for it",
                          tc->methods[i].name->name, tc_name->name);
                return NULL;
            }
            ordered[i] = found;
        }
        method_impl_forms = ordered;
        n_method_impl_forms = tc->n_methods;
    }
    if (n_method_impl_forms < tc->n_methods) {
        diag_emit(DIAG_ERROR, call->span,
                  "definstance: expected %d method implementations for '%s', got %d",
                  tc->n_methods, tc_name->name, n_method_impl_forms);
        return NULL;
    }
    
    /* For Phase 15 v1, we store method implementations as FnDef pointers.
     * In a full implementation, these would be stored in the instance and
     * codegen would generate dictionary structs. For now, we validate syntax.
     */
    FnDef **method_impls = NULL;
    if (tc->n_methods > 0) {
        method_impls = (FnDef **)arena_alloc(e->arena, tc->n_methods * sizeof(FnDef *));
        for (uint8_t i = 0; i < tc->n_methods; i++) method_impls[i] = NULL;
    }

    /* Intra-instance method dispatch: register the instance and wire up its
     * type args / method-impl slots BEFORE elaborating any method body, so that
     * a `(.sibling self ...)` call inside one method's body can resolve to
     * another method of the *same* instance.
     *
     * Elaboration is split into two passes so that *forward* references and
     * mutual recursion between siblings resolve too:
     *   Pass 1 parses each method's signature (name / params / return type) and
     *     creates its FnDef + binding shell, filling `method_impls[i]` and
     *     registering the file-scope def -- but leaves the body as a nil
     *     placeholder.  After pass 1 every sibling slot is non-NULL.
     *   Pass 2 elaborates each method body against the now-complete instance,
     *     so a call to a sibling defined *later* in the same `definstance`
     *     (or a mutually recursive pair) sees a populated slot.
     * The instance itself is registered on the env list up front so the
     * type-based `.method` dispatcher selects it.  Post-registration
     * bookkeeping (orphan check, INSTANCE_DEF expr) still happens after the
     * loops. */
    typedef struct {
        Form     *impl_form;        /* (method-name [params...] body...) */
        uint32_t  impl_body_start;  /* index of first body form within impl_form */
        Binding **method_params;    /* param bindings to push into the body scope */
        uint8_t   n_method_params;
        FnDef    *method_fd;        /* shell whose ->body pass 2 fills in */
        bool      arrow_return;     /* class return type is the (->) class var:
                                     * refine the method's result type from the
                                     * elaborated body (a callable closure) in
                                     * pass 2. */
        /* Gap 1 (instance-method-return-not-unified): the declared return's
         * concrete nominal def (after Phase RT substitution), so pass 2 can
         * reject a body that yields a different nominal type.  (structdef-
         * retirement DS-D: the former StructDef ret_struct is gone -- every
         * former struct is a record ADT.) */
        const AdtDef    *ret_adt;
        /* float-register-class-returns: the declared return's TypeKind (after
         * Phase RT substitution), so pass 2 can reject a float-vs-non-float
         * (xmm-vs-GP) register-class clash in the method body. */
        TypeKind         ret_kind;
        /* carrier-aware-return-unification Phase 0: the full declared return
         * Type (after Phase RT substitution), retained so the Phase 3 classifier
         * can tell a grounded concrete commit from a carrier-participating
         * (free-tyvar / applied) return.  ret_kind / ret_adt are
         * its decomposition; ret_full keeps the whole type for that future
         * carrier-vs-committed decision. */
        Type             ret_full;
        /* carrier-aware-return-unification Phase 3: true iff the method's
         * CLASS-DECLARATION return was the class type variable (substituted to
         * this instance's concrete type at Phase RT).  Only such a return is a
         * genuine per-instance commit -- a fixed concrete class-decl return
         * (e.g. `len : int`) is a carrier slot for every instance and must stay
         * tolerant.  Combined with a grounded ret_full, this gates
         * RET_CLASS_COMMITTED for the method. */
        bool             ret_was_class_var;
    } InstMethodPass;
    InstMethodPass *passes = NULL;
    if (tc->n_methods > 0) {
        passes = (InstMethodPass *)arena_alloc(e->arena,
            tc->n_methods * sizeof(InstMethodPass));
        memset(passes, 0, tc->n_methods * sizeof(InstMethodPass));
    }

    TypeClassInstance *inst = typeclass_env_register_instance(&e->typeclass_env, tc);
    if (!inst) {
        diag_emit(DIAG_ERROR, call->span,
                  "failed to register instance for '%s'", tc_name->name);
        return NULL;
    }
    inst->type_args = type_args;
    inst->n_type_args = n_type_args;
    inst->type_arg_syms = type_arg_syms;  /* Phase HKT §1: store for dict naming */
    inst->method_impls = method_impls;
    inst->n_method_impls = tc->n_methods;
    /* Phase PTC1: Store type parameter constraints */
    inst->type_param_constraints = type_param_constraints;
    inst->n_type_param_constraints = n_type_param_constraints;
    /* Phase HKT-P4: record the file that defined this instance. */
    inst->origin_file_id = call->span.file_id;
    /* class-superclasses SC4: the form, for the post-unit obligation check. */
    inst->decl_form = call;
    /* M7: record the partial-app wildcard hole slot (0xFF when absent). */
    inst->partial_hole_pos = hkt_hole_pos;
    /* S9 (D8 Q1): stamped BEFORE the method loop, because the minted methods'
     * bodies are `.m` calls on an `any` receiver and must already see this
     * instance as the dynamic stand-in it is (see dyn_any_minted). */
    inst->dyn_any_minted = e->minting_dyn_any;

    /* assoc-types-plan: resolve and store associated-type bindings.  Every
     * member the class declares must be bound exactly once; an unknown member
     * name or a missing binding is a hard error reported on the instance, so a
     * wrong/forgotten projection surfaces here rather than at a downstream use
     * site.
     *
     * ECS E2d-P6 (Issue 1): resolved up front -- before the method loop --
     * so an instance method body whose param/return type names an associated
     * member (e.g. `val : Elem` -> TY_TYVAR("Elem")) can substitute it with the
     * concrete binding (`(type Elem = Pos)` -> Pos). */
    if (tc->n_assoc_types > 0 || n_assoc_binds > 0) {
        Type *resolved = tc->n_assoc_types > 0
            ? (Type *)arena_alloc(e->arena, tc->n_assoc_types * sizeof(Type)) : NULL;
        bool *bound = tc->n_assoc_types > 0
            ? (bool *)arena_alloc(e->arena, tc->n_assoc_types * sizeof(bool)) : NULL;
        for (uint8_t k = 0; k < tc->n_assoc_types; k++) bound[k] = false;
        /* ECS E2d-P6 (parametric associated-type element): collect the free
         * tyvar names appearing in the instance head (e.g. `A` in `(Dense A)`)
         * so a binding RHS that names one (`(type Elem = A)`) resolves to that
         * NAMED TY_TYVAR rather than a nameless null-def struct.  Without this,
         * `A` is an unknown symbol to type_expr_from_form (no type_params), the
         * binding becomes an anonymous abstract struct, and the call-site
         * projection can never correlate `Elem` with the receiver's element. */
        const Symbol *head_tv_syms[16];
        Kind head_tv_kinds[16];
        uint8_t n_head_tv = 0;
        for (uint8_t ta = 0; ta < n_type_args && n_head_tv < 16; ta++) {
            const Type *sp = &type_args[ta];
            while (sp && sp->kind == TY_APP) {
                const Type *arg = sp->as.app.arg;
                if (arg && arg->kind == TY_TYVAR && arg->as.tyvar_.name) {
                    bool dup = false;
                    for (uint8_t d = 0; d < n_head_tv; d++)
                        if (head_tv_syms[d] &&
                            strcmp(head_tv_syms[d]->name, arg->as.tyvar_.name) == 0) {
                            dup = true; break;
                        }
                    if (!dup && n_head_tv < 16) {
                        head_tv_syms[n_head_tv] = symtab_intern(e->st,
                            strslice(arg->as.tyvar_.name,
                                     (uint32_t)strlen(arg->as.tyvar_.name)));
                        head_tv_kinds[n_head_tv] = KIND_STAR;
                        n_head_tv++;
                    }
                }
                sp = sp->as.app.fn;
            }
        }
        for (uint32_t bi = 0; bi < n_assoc_binds; bi++) {
            uint8_t idx = 0; bool found = false;
            for (uint8_t k = 0; k < tc->n_assoc_types; k++) {
                if (tc->assoc_type_names[k] == assoc_bind_names[bi]) {
                    idx = k; found = true; break;
                }
            }
            if (!found) {
                diag_emit(DIAG_ERROR, call->span,
                          "definstance: '%s' is not an associated type of typeclass '%s'",
                          assoc_bind_names[bi]->name, tc_name->name);
                return NULL;
            }
            Type *rt = type_expr_from_form(e, assoc_bind_forms[bi], NULL,
                                           n_head_tv > 0 ? head_tv_syms : NULL,
                                           n_head_tv > 0 ? head_tv_kinds : NULL,
                                           n_head_tv);
            if (!rt) {
                diag_emit(DIAG_ERROR, assoc_bind_forms[bi]->span,
                          "definstance: unsupported type for associated type '%s'",
                          assoc_bind_names[bi]->name);
                return NULL;
            }
            resolved[idx] = *rt;
            bound[idx] = true;
        }
        for (uint8_t k = 0; k < tc->n_assoc_types; k++) {
            if (!bound[k]) {
                diag_emit(DIAG_ERROR, call->span,
                          "definstance: missing binding for associated type '%s' of '%s'",
                          tc->assoc_type_names[k]->name, tc_name->name);
                return NULL;
            }
        }
        inst->assoc_types = resolved;
        inst->n_assoc_types = tc->n_assoc_types;
    }

    for (uint8_t i = 0; i < tc->n_methods; i++) {
        Form *impl_form = method_impl_forms[i];
        /* default-method-spliced-at-carrier-type: see the note at the splice. */
        bool impl_is_default = impl_is_default_arr && impl_is_default_arr[i];
        if (impl_form->tag != F_LIST || impl_form->as.list.len < 3) {
            diag_emit(DIAG_ERROR, impl_form->span,
                      "method implementation requires (name [params...] body...)");
            return NULL;
        }
        
        /* Parse the method implementation as a function */
        /* For now, we just validate the name matches */
        Form *impl_name_form = impl_form->as.list.items[0];
        if (impl_name_form->tag != F_SYM) {
            diag_emit(DIAG_ERROR, impl_name_form->span,
                      "method implementation name must be a symbol");
            return NULL;
        }
        
        if (impl_name_form->as.sym != tc->methods[i].name) {
            diag_emit(DIAG_ERROR, impl_name_form->span,
                      "method implementation name '%s' doesn't match typeclass method '%s'",
                      impl_name_form->as.sym->name, tc->methods[i].name->name);
            return NULL;
        }
        
        /* Elaborate the method implementation as a function */
        /* The form is (method-name [params...] body...) */

        /* Create a synthetic name for this method implementation */
        /* Format: __inst_<typeclass>_<method>_<typeargs> e.g. __inst_MyEq_eq_int */
        enum {
            MAX_INSTANCE_METHOD_NAME_LEN = 192,
            MAX_SANITIZED_METHOD_NAME_LEN = 64,
            MAX_INSTANCE_TYPE_SUFFIX_LEN = 64,
        };
        char method_name[MAX_INSTANCE_METHOD_NAME_LEN];
        
        /* Mangle method name into a C identifier via the shared mangler so
         * sigil method pairs (`>>>`/`<<<`) get distinct instance-function
         * names instead of colliding on `___`. */
        char sanitized_method_name[MAX_SANITIZED_METHOD_NAME_LEN];
        const char *method_name_str = tc->methods[i].name->name;
        tur_mangle_ident(method_name_str, sanitized_method_name,
                         sizeof(sanitized_method_name));

        /* Type arg suffix was computed once up front (inst_type_suffix) -- it is
         * identical for every method of this instance and shared with the
         * duplicate-instance guard, so the emitted names stay in lock-step. */
        int method_name_written =
            snprintf(method_name, sizeof(method_name), "__inst_%.*s_%s%s",
                     (int)tc_name->len, tc_name->name, sanitized_method_name,
                     inst_type_suffix);
        if (method_name_written < 0 || (size_t)method_name_written >= sizeof(method_name)) {
            diag_emit(DIAG_ERROR, impl_form->span,
                      "typeclass instance method name is too long");
            return NULL;
        }
        
        const Symbol *method_sym = symtab_intern(e->st, 
            strslice(method_name, (uint32_t)strlen(method_name)));
        
        /* Parse the method implementation form */
        /* impl_form is (method-name [params...] :return-type body...) */
        /* or (method-name [params...] body...) if no return type */
        Form *impl_params_form = impl_form->as.list.items[1];
        uint32_t impl_body_start = 2;
        Type return_type = tc->methods[i].return_type;  /* Default from typeclass */
        /* RT1: the class's promise about this method's result, if any. */
        const Form *m_class_ret_pred = tc->methods[i].return_refine_pred;
        const char *m_class_ret_var  = tc->methods[i].return_refine_var;
        /* carrier-aware-return-unification Phase 3: did the class-decl return name
         * the class type variable (so the substitution below grounds it to this
         * instance's concrete type)?  Only then is the method a genuine
         * per-instance commit; a fixed concrete class-decl return stays a carrier
         * slot.  An explicit instance annotation (further below) does NOT set
         * this -- that path keeps the conservative carrier classification. */
        bool ret_was_class_var = false;

        /* Phase RT: substitute a tyvar return type (the dispatch variable) with
         * the instance's concrete type argument, so the emitted impl returns
         * the instance type (e.g. (decode! [raw :int] : a) becomes : User for
         * HasSchema[User]).  An explicit annotation below still wins. */
        if (return_type.kind == TY_TYVAR && return_type.as.tyvar_.name) {
            bool subst = false;
            for (uint8_t ti = 0; ti < tc->n_type_params && ti < n_type_args; ti++) {
                if (tc->type_params[ti] &&
                    strcmp(tc->type_params[ti]->name,
                           return_type.as.tyvar_.name) == 0) {
                    return_type = type_args[ti];
                    subst = true;
                    ret_was_class_var = true;
                    break;
                }
            }
            /* ECS E2d-P6 (Issue 1): a return type naming an associated type
             * member (`: Elem`) substitutes with this instance's binding
             * (`(type Elem = Pos)` -> Pos), so the emitted impl returns the
             * concrete projected type. */
            for (uint8_t ak = 0; !subst && ak < inst->n_assoc_types; ak++) {
                if (tc->assoc_type_names[ak] &&
                    strcmp(tc->assoc_type_names[ak]->name,
                           return_type.as.tyvar_.name) == 0) {
                    return_type = inst->assoc_types[ak];
                    break;
                }
            }
        }
        /* M7 layer 0: an HKT-applied return like `(g b)` arrives as
         * TY_APP(TY_TYVAR g, TY_TYVAR b).  Substitute the HKT class param in the
         * application HEAD (`g`) with this instance's constructor (type_args[ti],
         * e.g. Option), leaving the element tyvar (`b`) abstract for per-call
         * refinement.  Result: `(Option b)`.  Only the outermost head is
         * rewritten (the common `(f a)` / `(f b)` shape). */
        else if (return_type.kind == TY_APP &&
                 return_type.as.app.fn &&
                 return_type.as.app.fn->kind == TY_TYVAR &&
                 return_type.as.app.fn->as.tyvar_.name) {
            const char *head = return_type.as.app.fn->as.tyvar_.name;
            for (uint8_t ti = 0; ti < tc->n_type_params && ti < n_type_args; ti++) {
                if (tc->type_params[ti] &&
                    strcmp(tc->type_params[ti]->name, head) == 0) {
                    /* M7 partial-app wildcard head (`(Result _ B)`): rebuild the
                     * full ctor application so the result element `b` lands in the
                     * HOLE slot and the fixed arm is a tyvar named after the
                     * struct param (grounded at the call site).  A naive head
                     * subst would give `((Result B) b)` -- wrong slot order, and
                     * the opaque fixed arm collapses the result to the carrier. */
                    if (hkt_hole_pos == 0) {
                        return_type = elab_subst_class_tyvars_holed(
                            e->arena, return_type, tc->type_params,
                            tc->n_type_params, type_args, n_type_args,
                            hkt_hole_pos);
                    } else {
                        Type *new_fn = (Type *)arena_alloc(e->arena, sizeof(Type));
                        *new_fn = type_args[ti];
                        return_type.as.app.fn = new_fn;
                    }
                    break;
                }
            }
        }
        /* saffron-applied-class-var-result-takes-one-instances-type: a
         * kind-* class's result that mentions the class variable INSIDE an
         * application -- `(wrap-self [x : a] : (Option a))` -- matched neither
         * branch above (not a bare tyvar, and its head `Option` is no class
         * parameter), so `Wrap [Pt]`'s impl kept returning `(Option a)`: an
         * open type the emitter lowers to the carrier, whose box every
         * dynamic dispatch then tagged with one unresolved `(Option a)` id,
         * whichever instance ran.  Substitute the class variables through the
         * application, so the impl returns `(Option Pt)`.  Three guards:
         *   - a class whose every parameter is kind-*: an HKT class's
         *     parameter is a constructor, handled by the head rewrite above;
         *   - a RECEIVER-dispatched method: a return-directed one (`(dec
         *     [seed : int] : (Result a cstr))`) is selected by its expected
         *     result and rides the uniform carrier every such dispatch reads;
         *   - GROUND instance types (a primitive or a non-parametric ADT): a
         *     parametric head (`Dec [Option] [(Dec A)]`) would substitute the
         *     bare constructor, `(Result Option cstr)`. */
        else if (return_type.kind == TY_APP &&
                 !method_is_return_dispatch(tc, &tc->methods[i])) {
            bool subst_ok = n_type_args > 0;
            if (tc->type_param_kinds)
                for (uint8_t ki = 0; ki < tc->n_type_params; ki++)
                    if (tc->type_param_kinds[ki] != KIND_STAR) { subst_ok = false; break; }
            for (uint8_t ti = 0; subst_ok && ti < n_type_args; ti++) {
                const Type *ta = &type_args[ti];
                if (ta->kind == TY_APP || ta->kind == TY_TYVAR || ta->kind == TY_UNKNOWN ||
                    (ta->kind == TY_ADT && ta->as.adt_.def &&
                     ta->as.adt_.def->n_type_params > 0))
                    subst_ok = false;
            }
            if (subst_ok)
                return_type = elab_subst_class_tyvars(e->arena, return_type,
                                                      tc->type_params, tc->n_type_params,
                                                      type_args, n_type_args);
        }
        /* Arrow head: a method whose declared return is the class variable
         * (e.g. `comp : a` under `Arrow [(->)]`) returns a callable closure.
         * The arrow marker carries no arity yet, so flag it as a boxed TY_FN
         * placeholder here and refine its full signature from the elaborated
         * body in pass 2 (the body's `(fn [x] ...)` carries the real arity). */
        bool arrow_return = false;
        if (return_type.kind == TY_FN && return_type.as.fn.arity == 0) {
            arrow_return = true;
            return_type.as.fn.boxed = true;
        }
        /* RT1: the refinement this impl promises about its RESULT.  Starts as
         * the class's promise, which an impl that writes a plain return type
         * INHERITS -- otherwise a class could declare a guarantee that no
         * instance ever enforces. */
        const Form *impl_ret_pred = m_class_ret_pred;
        const char *impl_ret_var  = m_class_ret_var;
        bool        impl_ret_pred_own = false;
        /* Check for return type annotation after params */
        if (impl_is_default && impl_form->as.list.len >= 3 &&
            (impl_form->as.list.items[2]->tag == F_KEYWORD ||
             impl_form->as.list.items[2]->tag == F_TYPE_ANN)) {
            /* default-method-spliced-at-carrier-type: the spliced default's
             * return annotation IS the class declaration's, already parsed
             * into `tc->methods[i].return_type` and substituted for this
             * instance above (`: a` -> the instance type, with
             * ret_was_class_var set).  Re-reading it here would elaborate the
             * class tyvar literally and land on the carrier.  Skip past it. */
            impl_body_start = 3;
        } else if (impl_form->as.list.len >= 3) {
            Form *ret_or_body = impl_form->as.list.items[2];
            /* Accept both fused `:T` (F_KEYWORD) and spaced `: T` (F_TYPE_ANN). */
            const Symbol *kw = NULL;
            if (ret_or_body->tag == F_KEYWORD) {
                kw = ret_or_body->as.sym;
            } else if (ret_or_body->tag == F_TYPE_ANN &&
                       ret_or_body->as.list.len == 1 &&
                       (ret_or_body->as.list.items[0]->tag == F_SYM ||
                        ret_or_body->as.list.items[0]->tag == F_KEYWORD)) {
                kw = ret_or_body->as.list.items[0]->as.sym;
            }
            /* RT1: `: #refine{ r : T | q }` on an impl result.  This branch did
             * not exist, so the annotation fell through to the body and came
             * back as "type annotation ': type' is only valid after a parameter
             * name or as a return type" -- a class could declare a result
             * refinement that an instance was syntactically forbidden to
             * restate.  Peel to the base type and keep the predicate. */
            if (!kw && ret_or_body->tag == F_TYPE_ANN &&
                ret_or_body->as.list.len == 1 &&
                ret_or_body->as.list.items[0]->tag == F_CONTRACT_TYPE) {
                Type *ft = type_expr_from_form(e, ret_or_body->as.list.items[0],
                                               NULL, NULL, NULL, 0);
                if (ft) {
                    ret_was_class_var = false;
                    const Form *rp = NULL; const char *rv = NULL;
                    return_type = *rt_peel_contract(ft, &rp, &rv);
                    if (rp) {
                        impl_ret_pred = rp;
                        impl_ret_var  = rv;
                        impl_ret_pred_own = true;
                    }
                    impl_body_start = 3;
                }
            }
            /* A compound result annotation, `: (Option float)`, `: (Vec int)`:
             * only a bare symbol reached the `kw` path, so this fell through to
             * the body and came back as "type annotation ': type' is only valid
             * after a parameter name" -- while the same form is accepted on a
             * method PARAMETER and on the class declaration.  Resolve it the
             * way a parameter annotation is resolved. */
            if (!kw && impl_body_start == 2 && ret_or_body->tag == F_TYPE_ANN &&
                ret_or_body->as.list.len == 1 &&
                ret_or_body->as.list.items[0]->tag != F_CONTRACT_TYPE) {
                Type *ft = type_expr_from_form(e, ret_or_body->as.list.items[0],
                                               NULL, NULL, NULL, 0);
                if (!ft) {
                    diag_emit(DIAG_ERROR, ret_or_body->span,
                              "unsupported type form in method return annotation");
                    return NULL;
                }
                ret_was_class_var = false;
                return_type = *ft;
                impl_body_start = 3;
            }
            if (kw) {
                /* carrier-aware-return-unification Phase 3: an explicit instance
                 * return annotation replaces the substituted class-var return, so
                 * it is no longer the "class type variable grounded for this
                 * instance" commit.  Conservatively drop back to the carrier
                 * classification rather than reason about annotation-vs-class-var
                 * agreement. */
                ret_was_class_var = false;
                if (kw->len == 3 && memcmp(kw->name, "int", 3) == 0) {
                    return_type = TYPE_INT;
                } else if (kw->len == 4 && memcmp(kw->name, "bool", 4) == 0) {
                    return_type = TYPE_BOOL;
                } else if (kw->len == 4 && memcmp(kw->name, "cstr", 4) == 0) {
                    return_type = TYPE_CSTR;
                } else if (kw->len == 5 && memcmp(kw->name, "float", 5) == 0) {
                    /* A :float instance return must carry the real register class
                     * (xmm0/double) all the way through the emitted impl signature
                     * and the monomorphic dispatch -- otherwise the result is
                     * numerically truncated at the int64-carrier return boundary
                     * (e.g. 6.5 -> 6).  See the typeclass-method return-type
                     * erasure follow-up. */
                    return_type = TYPE_FLOAT;
                } else if (kw->len == 7 && memcmp(kw->name, "float32", 7) == 0) {
                    return_type = TYPE_FLOAT32;
                } else if (kw->len == 7 && memcmp(kw->name, "float64", 7) == 0) {
                    return_type = TYPE_FLOAT64;
                } else if ((kw->len == 4 && memcmp(kw->name, "void", 4) == 0) ||
                           (kw->len == 3 && memcmp(kw->name, "nil", 3) == 0)) {
                    return_type = TYPE_NIL;
                } else if ((kw->len == 9 && memcmp(kw->name, "ptr<void>", 9) == 0) ||
                           (kw->len == 3 && memcmp(kw->name, "ptr", 3) == 0)) {
                    return_type = TYPE_PTR_VOID;
                } else {
                    /* Fall back to a full type expression so compound instance
                     * return annotations (e.g. `: (Vec int)`, a struct/ADT name)
                     * resolve instead of being silently dropped to the class
                     * carrier.  The keyword shortcuts above stay for the common
                     * primitives; type_expr_from_form covers the rest.  If it
                     * cannot resolve, leave return_type at the class default
                     * (back-compat with annotations the resolver does not know). */
                    Form *tf = (ret_or_body->tag == F_TYPE_ANN &&
                                ret_or_body->as.list.len >= 1)
                        ? ret_or_body->as.list.items[0] : ret_or_body;
                    Type *ft = type_expr_from_form(e, tf, NULL, NULL, NULL, 0);
                    if (ft) return_type = *ft;
                }
                impl_body_start = 3;
            }
        }
        
        /* Parse parameters */
        Binding **method_params = NULL;
        uint8_t n_method_params = 0;
        Type *method_param_types = NULL;
        /* CT0/CT1: contract-typed parameters of this instance method.  Collected
         * while the annotations are resolved, injected as entry checks once the
         * body exists -- the same treatment a `defn` or `fn` parameter gets. */
        const Form *m_ct_preds[MAX_FN_ARITY];
        const char *m_ct_vars[MAX_FN_ARITY];
        uint32_t    m_ct_param_idx[MAX_FN_ARITY];
        uint32_t    n_m_ct_preds = 0;
        /* RT1: which parameters the INSTANCE annotated for itself.  An
         * unannotated parameter inherits the class's refinement, exactly as an
         * unannotated result inherits the class's promise; writing an explicit
         * annotation -- including a bare `: int`, which carries no predicate --
         * is how an instance opts out and demands less.  Without this an
         * omitted annotation inherited NOTHING, so a class demand was enforced
         * only by an instance that happened to restate it. */
        bool m_param_annotated[MAX_FN_ARITY];
        memset(m_param_annotated, 0, sizeof(m_param_annotated));
        
        if (impl_params_form->tag == F_VEC) {
            uint8_t max_method_params = (uint8_t)impl_params_form->as.list.len;
            if (max_method_params > 0) {
                method_params = (Binding **)arena_alloc(e->arena, 
                    max_method_params * sizeof(Binding *));
                method_param_types = (Type *)arena_alloc(e->arena, 
                    max_method_params * sizeof(Type));
                
                for (uint8_t j = 0; j < max_method_params; j++) {
                    Form *p = impl_params_form->as.list.items[j];
                    /* typeclass-method-resolution-ignores-the-class, "Trap for
                     * anyone writing a repro": a substructural / borrow caret
                     * annotates the NEXT parameter and is not a parameter
                     * itself.  The defclass parser has skipped these since ECS
                     * E2d-P6 (see the identical guard in that loop); this one
                     * did not, so repeating the class's spelling in the impl
                     * -- `(foo-of [^borrow w] : int (.v w))` instead of
                     * `(foo-of [w] (.v w))` -- made `^borrow` consume a param
                     * slot and emitted a TWO-parameter C function.  `tur check`
                     * exited 0 and cc then rejected every call site, correct
                     * ones included, with "too few arguments to function call,
                     * expected 2, have 1".  A check/build divergence, which is
                     * the worst place for one.  (The borrow discipline itself
                     * is enforced on the elaborated FnDefs and at call sites,
                     * exactly as in the defclass parser.) */
                    if (p->tag == F_SYM &&
                        (p->as.sym == e->sym_caret_borrow ||
                         p->as.sym == e->sym_caret_mut ||
                         p->as.sym == e->sym_caret_unique ||
                         p->as.sym == e->sym_caret_linear ||
                         p->as.sym == e->sym_caret_affine ||
                         p->as.sym == e->sym_caret_relevant ||
                         p->as.sym == e->sym_caret_fat)) {
                        continue;
                    }
                    if (p->tag == F_KEYWORD || p->tag == F_TYPE_ANN) {
                        if (n_method_params == 0) {
                            diag_emit(DIAG_ERROR, p->span,
                                      "method parameter type annotation without a preceding parameter");
                            return NULL;
                        }
                        /* default-method-spliced-at-carrier-type: a spliced
                         * default's parameter annotations are the CLASS
                         * signature's, already in `tc->methods[i].param_types`
                         * (and its refinements / `param_explicit_type`), which
                         * the bare-parameter path below substitutes for this
                         * instance.  Reading `x : a` here literally would
                         * type the parameter at the tyvar carrier. */
                        if (impl_is_default) continue;
                        uint8_t prev = n_method_params - 1;
                        if (prev < MAX_FN_ARITY) m_param_annotated[prev] = true;
                        Type param_type = method_param_types[prev];
                        if (p->tag == F_TYPE_ANN) {
                            Type *ann = (p->as.list.len > 0)
                                ? type_expr_from_form(e, p->as.list.items[0], NULL, NULL, NULL, 0)
                                : NULL;
                            if (!ann) {
                                diag_emit(DIAG_ERROR, p->span,
                                          "unsupported type form in method parameter");
                                return NULL;
                            }
                            /* CT0: peel a contract annotation to its base type and
                             * keep the predicate for the entry check below.
                             * Without this the parameter's type stayed the
                             * contract type and the method body could not use
                             * the value at all. */
                            const Form *ct_pred = NULL;
                            const char *ct_var  = NULL;
                            ann = rt_peel_contract(ann, &ct_pred, &ct_var);
                            if (ct_pred && n_m_ct_preds < MAX_FN_ARITY) {
                                m_ct_preds[n_m_ct_preds]    = ct_pred;
                                m_ct_vars[n_m_ct_preds]     = ct_var;
                                m_ct_param_idx[n_m_ct_preds] = prev;
                                n_m_ct_preds++;
                            }
                            param_type = *ann;
                        } else {
                            const Symbol *kw = p->as.sym;
                            if (kw->len == 3 && memcmp(kw->name, "int", 3) == 0) {
                                param_type = TYPE_INT;
                            } else if (kw->len == 4 && memcmp(kw->name, "bool", 4) == 0) {
                                param_type = TYPE_BOOL;
                            } else if (kw->len == 4 && memcmp(kw->name, "cstr", 4) == 0) {
                                param_type = TYPE_CSTR;
                            } else if ((kw->len == 4 && memcmp(kw->name, "void", 4) == 0) ||
                                       (kw->len == 3 && memcmp(kw->name, "nil", 3) == 0)) {
                                param_type = TYPE_NIL;
                            } else if ((kw->len == 9 && memcmp(kw->name, "ptr<void>", 9) == 0) ||
                                       (kw->len == 3 && memcmp(kw->name, "ptr", 3) == 0)) {
                                param_type = TYPE_PTR_VOID;
                            } else {
                                diag_emit(DIAG_ERROR, p->span,
                                          "unsupported type in method parameter");
                                return NULL;
                            }
                        }
                        bool param_is_poly = (param_type.kind == TY_FORALL || param_type.kind == TY_EXISTS);
                        if (!param_is_poly
                            && tc->methods[i].param_is_fn
                            && prev < tc->methods[i].n_params
                            && tc->methods[i].param_is_fn[prev]) {
                            param_is_poly = true;
                            param_type = TYPE_PTR_VOID;
                        }
                        Type c_param_type = param_is_poly ? TYPE_PTR_VOID : param_type;
                        method_params[prev]->type = c_param_type;
                        method_params[prev]->is_poly_fn = param_is_poly;
                        method_params[prev]->poly_type = NULL;
                        if (param_is_poly) {
                            Type *pt = (Type *)arena_alloc(e->arena, sizeof(Type));
                            *pt = param_type;
                            method_params[prev]->poly_type = pt;
                        }
                        method_param_types[prev] = c_param_type;
                        continue;
                    }

                    Type param_type = TYPE_INT;
                    /* Arrow head: a class-variable-typed parameter becomes a
                     * callable fat closure (TY_PTR_VOID + is_fat), so a method
                     * body that applies it -- `(g (f x))` -- routes through the
                     * fat-dispatch path instead of erroring "not a function". */
                    bool param_is_fat = false;

                    /* Phase 15: Try to use type from typeclass method definition */
                    if (tc->methods[i].param_types && n_method_params < tc->methods[i].n_params) {
                        param_type = tc->methods[i].param_types[n_method_params];
                    }

                    /* ECS E2d-P6 (Issue 2 secondary): when the class method param
                     * type references the class's type parameters (e.g. `val : E`
                     * -> TY_TYVAR("E"), or `s : S` -> TY_TYVAR("S")), substitute
                     * them with this instance's concrete type args so an
                     * unannotated instance-body param (`[s idx val]`) inherits the
                     * right type instead of an abstract tyvar.  This is the
                     * general, multi-param-aware replacement for the legacy
                     * receiver-only `type_args[0]` rewrite below (which keyed on
                     * the param defaulting to TY_INT). */
                    bool param_was_tyvar_subst = false;
                    if (param_type.kind == TY_TYVAR || param_type.kind == TY_APP) {
                        for (uint8_t ck = 0; n_type_args > 0 && ck < tc->n_type_params; ck++) {
                            if (tc->type_params[ck] &&
                                rt_type_mentions_tyvar(&param_type,
                                                       tc->type_params[ck]->name)) {
                                param_was_tyvar_subst = true;
                                break;
                            }
                        }
                        /* ECS E2d-P6 (Issue 1): also substitute associated-type
                         * tyvars (`val : Elem`) with this instance's binding
                         * (`(type Elem = Pos)` -> Pos). */
                        for (uint8_t ak = 0; !param_was_tyvar_subst &&
                                 ak < inst->n_assoc_types; ak++) {
                            if (tc->assoc_type_names[ak] &&
                                rt_type_mentions_tyvar(&param_type,
                                                       tc->assoc_type_names[ak]->name)) {
                                param_was_tyvar_subst = true;
                                break;
                            }
                        }
                        if (param_was_tyvar_subst) {
                            if (n_type_args > 0) {
                                /* Hole-aware for a `(Result _ B)` head, so
                                 * `(f X)` lands X in the hole slot (see
                                 * elab_subst_class_tyvars_holed). */
                                param_type = elab_subst_class_tyvars_holed(
                                    e->arena, param_type,
                                    tc->type_params, tc->n_type_params,
                                    type_args, n_type_args, hkt_hole_pos);
                            }
                            if (inst->n_assoc_types > 0) {
                                param_type = elab_subst_class_tyvars(
                                    e->arena, param_type,
                                    tc->assoc_type_names, tc->n_assoc_types,
                                    inst->assoc_types, inst->n_assoc_types);
                            }
                        }
                    }

                    /* Phase 15: Substitute type variables with type args */
                    /* For v1: if the param type is TYPE_INT (default) and we have type args,
                     * use the first type arg */
                    /* CS1b: elab_param_type is the type used for scope bindings and body
                     * elaboration; param_type is the ABI type used for the emitted signature. */
                    Type elab_param_type = param_type;
                    /* Arrow head: under `Arrow [(->)]`, a method parameter typed
                     * by the class variable is the function arrow itself -- a
                     * callable closure.  Carry it as a fat-closure sink
                     * (:ptr<void> + is_fat), the same representation the
                     * bare-function arrow layer uses for its `^fat` parameters,
                     * so applying it (`(g (f x))`) dispatches through the fat
                     * protocol.  This applies even to a return-dispatch method
                     * (e.g. `comp [f g] : a`, whose untyped params default to the
                     * carrier): the arrow instance head makes the params arrows. */
                    if (param_was_tyvar_subst) {
                        /* M7 fix direction 1: box any fn that sits in
                         * HKT-element position of the body param type, so
                         * calling an HKT-wrapped function (`((.value ff) x)` in
                         * the Applicative `ap` shape) fat-dispatches through the
                         * box instead of bare-calling the box address. */
                        elab_param_type = m7_box_hkt_element_fns_ex(
                            e->arena, elab_param_type, hkt_hole_pos == 0);
                        /* The substituted full type (elab_param_type) is what the
                         * method body sees; lower the ABI/signature type to the
                         * int64 carrier for applied/parametric types, matching the
                         * dispatch ABI used for concrete instances elsewhere. */
                        /* hkt-foldable-rc-param: an instance over a pointer-family
                         * builtin sees its receiver as the applied `(t a)` ->
                         * `(type-app rc<?> tyvar 'a')`, which unifies with nothing
                         * -- so an instance body could not pass its own receiver to
                         * anything typed `rc<A>` ("expected rc<?>, got (type-app
                         * rc<?> tyvar 'a')") and had to reach for inline-C.
                         * Collapse it to the concrete `rc<a>` the rest of the
                         * compiler recognizes, the parameter-side mirror of the
                         * result-side collapse in the dispatch path.
                         *
                         * The ABI/signature type stays TYPE_INT: this changes only
                         * what the BODY sees, never the dict's uniform carrier
                         * calling convention. */
                        Type ptr_family_param;
                        if (m7_app_to_ptr_family(elab_param_type, &ptr_family_param)) {
                            elab_param_type = ptr_family_param;
                            param_type = TYPE_INT;
                        } else if (elab_param_type.kind == TY_APP) {
                            param_type = TYPE_INT;
                        } else {
                            param_type = elab_param_type;
                        }
                    }
                    else if (param_type.kind == TY_INT && n_type_args > 0 &&
                        type_args[0].kind == TY_FN) {
                        elab_param_type = TYPE_PTR_VOID;
                        param_type = TYPE_PTR_VOID;
                        param_is_fat = true;
                    }
                    /* Phase RT: for a return-only-dispatch method, parameters
                     * are genuine (concrete) inputs, not the dispatch receiver,
                     * so do not rewrite an int parameter to the instance type.
                     *
                     * Prereq 4: the `param_type.kind == TY_INT` test is broad --
                     * untyped params default to TY_INT, but an explicit `:int`
                     * annotation lands on the same kind. Skip the rewrite when
                     * the user explicitly annotated the param (tracked via
                     * `param_explicit_type[]`, populated by parse_typeclass_method).
                     * Without this, a class like
                     * `(defclass Decode [a] (decode [v : int] : (Result a cstr)))`
                     * would emit `__inst_Decode_decode_cstr(const char *)` for
                     * the cstr instance -- silently substituting cstr in where
                     * the user pinned int, and segfaulting at runtime. */
                    /* saffron-dyn-parametric-extra-read-as-class-var (fix
                     * direction 2): in a dynamic dialect, a bare EXTRA
                     * parameter of an instance on a PARAMETRIC head is `any`,
                     * not the head.  The rewrite below cannot tell `Eq [Vec]`'s
                     * `y` (another vector) from `Nth [Vec]`'s `n` (an index):
                     * both are bare in the class and the impl.  Retyped to the
                     * head, the index made the dynamic witness cast it to
                     * `(Vec any)` and panic.  As `any` the impl narrows it where
                     * it is used -- `(vec-get v n)` and `(vec-len y)` both pass
                     * it through the D5 seam, which casts to what the callee
                     * takes -- the dialect's own default for a bare parameter,
                     * and what spelling `n : any` already did.  The receiver
                     * (parameter 0) keeps the rewrite; a kind-* head, whose
                     * class variable IS a concrete type, keeps it too. */
                    else if (param_type.kind == TY_INT && n_method_params > 0 &&
                        n_type_args > 0 && type_args[0].kind == TY_ADT &&
                        type_args[0].as.adt_.def &&
                        type_args[0].as.adt_.def->n_type_params > 0 &&
                        lang_span_is_dynamic(p->span) &&
                        !method_is_return_dispatch(tc, &tc->methods[i]) &&
                        !(tc->methods[i].param_explicit_type &&
                          n_method_params < tc->methods[i].n_params &&
                          tc->methods[i].param_explicit_type[n_method_params])) {
                        memset(&elab_param_type, 0, sizeof(elab_param_type));
                        elab_param_type.copy_kind = CK_COPY;
                        elab_param_type.kind = TY_ANY;
                        param_type = elab_param_type;
                    }
                    else if (param_type.kind == TY_INT && n_type_args > 0 &&
                        !method_is_return_dispatch(tc, &tc->methods[i]) &&
                        !(tc->methods[i].param_explicit_type &&
                          n_method_params < tc->methods[i].n_params &&
                          tc->methods[i].param_explicit_type[n_method_params])) {
                        elab_param_type = type_args[0];
                        /* PTC4: KIND_ARROW struct type-constructors (have type params) are
                         * applied as TY_APP at call sites, which lowers to int64_t in C.
                         * Use int64_t so the method signature matches the dispatch ABI.
                         * CS1b: preserve the full struct type in elab_param_type so that
                         * field-access forms inside the method body can resolve correctly.
                         * T4: a partially-applied instance head (e.g. `(Result _ B)` /
                         * `(Either E)`) records the receiver type as a TY_APP, which also
                         * lowers to the int64_t carrier.  Force the carrier here too --
                         * otherwise a concrete by-value struct receiver (e.g. an ascribed
                         * `(Result int int)`) is marshalled by-address into the int64_t
                         * impl signature and emits invalid C. */
                        if (elab_param_type.kind == TY_APP) {
                            param_type = TYPE_INT;
                        } else {
                            param_type = elab_param_type;
                        }
                    }

                    /* Phase HRT3: if the param type is TY_FORALL, treat it as a poly fn param.
                     * Phase CCL: also treat :fn-annotated params (param_is_fn) as poly fn. */
                    bool param_is_poly = (param_type.kind == TY_FORALL || param_type.kind == TY_EXISTS);
                    if (!param_is_poly
                        && tc->methods[i].param_is_fn
                        && n_method_params < tc->methods[i].n_params
                        && tc->methods[i].param_is_fn[n_method_params]) {
                        param_is_poly = true;
                        param_type = TYPE_PTR_VOID;
                        elab_param_type = TYPE_PTR_VOID;
                    }
                    /* M7 capturing-closure gate: a typed `(fn [a] b)` element param
                     * (the mapper handed to fmap/bimap/ap/<|>) must use the
                     * `tur_poly_fn_t` {env, fn} carrier -- like the regular defn
                     * path -- so a CAPTURING closure's env survives `(g x)` instead
                     * of being dropped by a bare raw-fn-pointer call (segfault).
                     * SCOPED to element fns whose RESULT is a plain element (a bare
                     * tyvar `b`): a continuation returning an HKT-applied `(m b)`
                     * (Monad `bind`, Traversable `traverse`) unpacks its wrapped
                     * result through the carrier and regresses under the
                     * tur_poly_fn_t switch, so it stays as-is. */
                    if (!param_is_poly &&
                        param_type.kind == TY_FN) {
                        param_is_poly = true;
                    }
                    Type c_param_type = param_is_poly ? TYPE_PTR_VOID : param_type;
                    if (p->tag == F_SYM) {
                        /* Simple parameter name */
                        method_params[n_method_params] = binding_new(e, p->as.sym, elab_param_type, false, false, p->span);
                        method_params[n_method_params]->is_param = true;
                        if (param_is_poly) {
                            method_params[n_method_params]->is_poly_fn = true;
                            Type *pt = (Type *)arena_alloc(e->arena, sizeof(Type));
                            *pt = param_type;
                            method_params[n_method_params]->poly_type = pt;
                        }
                        if (param_is_fat) method_params[n_method_params]->is_fat = true;
                        method_param_types[n_method_params++] = c_param_type;
                    } else if (p->tag == F_VEC && p->as.list.len >= 1) {
                        /* Parameter with type annotation: [name : type] */
                        Form *name_f = p->as.list.items[0];
                        if (name_f->tag == F_SYM) {
                            /* Check for type annotation */
                            if (p->as.list.len >= 2 && p->as.list.items[1]->tag == F_KEYWORD) {
                                const Symbol *kw = p->as.list.items[1]->as.sym;
                                if (kw->len == 3 && memcmp(kw->name, "int", 3) == 0) {
                                    param_type = TYPE_INT;
                                } else if (kw->len == 4 && memcmp(kw->name, "bool", 4) == 0) {
                                    param_type = TYPE_BOOL;
                                } else if (kw->len == 4 && memcmp(kw->name, "cstr", 4) == 0) {
                                    param_type = TYPE_CSTR;
                                } else if ((kw->len == 4 && memcmp(kw->name, "void", 4) == 0) ||
                                           (kw->len == 3 && memcmp(kw->name, "nil", 3) == 0)) {
                                    param_type = TYPE_NIL;
                                }
                            } else if (p->as.list.len >= 2 && p->as.list.items[1]->tag == F_TYPE_ANN) {
                                Type *ann = (p->as.list.items[1]->as.list.len > 0)
                                    ? type_expr_from_form(e, p->as.list.items[1]->as.list.items[0], NULL, NULL, NULL, 0)
                                    : NULL;
                                if (!ann) {
                                    diag_emit(DIAG_ERROR, p->span,
                                              "unsupported type form in method parameter");
                                    return NULL;
                                }
                                param_type = *ann;
                            } else if (p->as.list.len >= 2
                                       && (p->as.list.items[1]->tag == F_LIST || p->as.list.items[1]->tag == F_VEC)) {
                                Type *ann = type_expr_from_form(e, p->as.list.items[1], NULL, NULL, NULL, 0);
                                if (!ann) {
                                    diag_emit(DIAG_ERROR, p->span,
                                              "unsupported type form in method parameter");
                                    return NULL;
                                }
                                param_type = *ann;
                            }
                            /* Phase CCL: :fn-annotated params also become poly fn */
                            if (!param_is_poly
                                && tc->methods[i].param_is_fn
                                && n_method_params < tc->methods[i].n_params
                                && tc->methods[i].param_is_fn[n_method_params]) {
                                param_is_poly = true;
                                param_type = TYPE_PTR_VOID;
                            }
                            param_is_poly = param_is_poly
                                || (param_type.kind == TY_FORALL || param_type.kind == TY_EXISTS);
                            c_param_type = param_is_poly ? TYPE_PTR_VOID : param_type;
                            method_params[n_method_params] = binding_new(e, name_f->as.sym, c_param_type, false, false, p->span);
                            method_params[n_method_params]->is_param = true;
                            if (param_is_poly) {
                                method_params[n_method_params]->is_poly_fn = true;
                                Type *pt = (Type *)arena_alloc(e->arena, sizeof(Type));
                                *pt = param_type;
                                method_params[n_method_params]->poly_type = pt;
                            }
                            method_param_types[n_method_params++] = c_param_type;
                        } else {
                            diag_emit(DIAG_ERROR, p->span,
                                      "method parameter name must be a symbol");
                            return NULL;
                        }
                    } else {
                        diag_emit(DIAG_ERROR, p->span,
                                  "method parameter must be a symbol or vector");
                        return NULL;
                    }
                }
            }
        }
        
        /* Pass 2 (below) elaborates the body once every sibling slot is wired
         * up.  For now the FnDef carries a nil placeholder body. */
        Expr *method_body = e_nil(e, impl_form->span);

        /* Create a proper function type for the method */
        TypeKind param_kinds[MAX_FN_ARITY];
        for (uint8_t j = 0; j < n_method_params; j++) {
            param_kinds[j] = method_param_types[j].kind;
        }
        Type fn_type = type_fn(param_kinds, n_method_params, return_type.kind);
        /* Arrow head: mirror each fat-closure parameter into the method's fn
         * type so call sites auto-shim a bare function argument into a fat box
         * (the same arg_fat plumbing the regular defn path uses for `^fat`). */
        for (uint8_t j = 0; j < n_method_params; j++) {
            if (method_params[j]->is_fat) FN_ARG_SET(fn_type.as.fn, j, FA_FAT, true);
        }
        /* Closure-returning instance methods: a method whose declared return
         * type is a function type (e.g. (arr-of [f] : (fn [:int] :int))) must
         * carry the full TY_FN through result_full_type, exactly like the
         * regular defn path (elab_fns.c "Issue 1b").  Without it, codegen falls
         * back to emit_type_from_kind(TY_FN) -- a zeroed fn shell whose result
         * kind is TY_UNKNOWN -- and the dict-field / impl-signature return type
         * lowers to an unknown-void carrier, silently dropping the returned fat
         * closure handle.  Attaching the full type makes type_c_name lower the
         * fn carrier to int64_t (the fat-closure handle the rest of the
         * language and TUR_APPLY* expect). */
        if (return_type.kind == TY_FN) {
            Type *rft = (Type *)arena_alloc(e->arena, sizeof(Type));
            *rft = return_type;
            fn_type.as.fn.result_full_type = rft;
        }
        /* ECS E2d-P6 (value-level projection): a method returning a *by-value*
         * struct or concrete ADT -- e.g. an instance whose return type
         * substituted to the by-value `Pos` (a class-var `: E`, an associated
         * member `: Elem`, or a direct `: Pos`) -- must carry the precise
         * return Type so the emitted impl, the dict slot, and the call site
         * agree on the by-value layout.  `type_from_kind(result_kind)` alone
         * drops the struct def, leaving `(.field (method ...))` unable to
         * resolve and a let-bound result mis-lowered against the carrier.
         *
         * Scope: genuinely by-value nominal types only.  Parametric structs and
         * applied/opaque heads (TY_APP, e.g. the receiver `(Dense Pos)`) keep
         * the documented int64 carrier ABI -- they are erased everywhere else,
         * and forcing them by-value here would diverge from the dispatch ABI. */
        else if (return_type.kind == TY_ADT && return_type.as.adt_.def &&
                 return_type.as.adt_.def->n_type_params == 0) {
            Type *rft = (Type *)arena_alloc(e->arena, sizeof(Type));
            *rft = return_type;
            fn_type.as.fn.result_full_type = rft;
        }
        /* M7 layer 2: carry an HKT-applied TY_APP return (`(Option b)` after
         * layer-0 head substitution) through result_full_type so the call site
         * receives the named applied head + element tyvar to refine, instead of
         * an anonymous `(type-app ? ?)`. */
        else if (return_type.kind == TY_APP) {
            Type *rft = (Type *)arena_alloc(e->arena, sizeof(Type));
            *rft = return_type;
            fn_type.as.fn.result_full_type = rft;
        }
        /* ECS E2d-P6 (parametric associated-type element): a PARAMETRIC instance
         * such as `(definstance StorageOps [(Dense A)] (type Elem = A) ...)`
         * projects the method's `: Elem` return to the instance head's own tyvar
         * `A` (Phase RT's assoc-type substitution above resolves `Elem -> A`, a
         * *named* TY_TYVAR -- not a concrete struct, so the by-value branch above
         * does not fire).  Carry that named tyvar through result_full_type so the
         * call site can recover the concrete element type by instantiating `A`
         * through the receiver's bindings (`(Dense Pos)` => `A -> Pos`), exactly
         * the way emit_abi_register_call's bare-tyvar-result recovery expects a
         * NAMED generic_result.  Without the name, result_kind=TY_TYVAR lowers to
         * an anonymous tyvar that no binding can substitute, so a struct element
         * stays carrier-collapsed.  Concrete instances (`Elem = Pos`) already took
         * the by-value branch; this only adds the parametric case. */
        else if (return_type.kind == TY_TYVAR && return_type.as.tyvar_.name) {
            Type *rft = (Type *)arena_alloc(e->arena, sizeof(Type));
            *rft = return_type;
            fn_type.as.fn.result_full_type = rft;
        }

        /* Create FnDef for the method implementation */
        FnDef *method_fd = (FnDef *)arena_alloc(e->arena, sizeof(FnDef));
        memset(method_fd, 0, sizeof(FnDef));
        Binding *method_binding = binding_new(e, method_sym, fn_type, false, true, impl_form->span);
        /* method_sym->name is ALREADY a mangled C identifier (built as
         * "__inst_<class>_<method>_<typesuffix>" above, with the method part run
         * through tur_mangle_ident). It must be emitted verbatim: re-mangling it
         * would double-encode every '_' as "_un" and desync the definition from
         * the use site (the dict initializer rebuilds the same name fresh). Pin
         * it via c_export_name, the documented "emit this C name as-is" bypass. */
        method_binding->c_export_name = method_sym->name;
        method_binding->is_instance_method = true;   /* B6: internal export, CPS-eligible */
        /* The user wrote `(definstance Eq int ...)`, not
         * `__inst_Eq_eq_qu_int` -- keep the mangled name out of every
         * human-facing symbol listing. */
        method_binding->is_synthesized = true;
        method_binding->synth_kind = SYNTH_INSTANCE_METHOD;
        /* ...and out of diagnostics, in the words the user did write. */
        {
            char args[96] = "";
            size_t used = 0;
            for (uint8_t ti = 0; ti < n_type_args && used < sizeof(args); ti++) {
                int w = snprintf(args + used, sizeof(args) - used, "%s%s",
                                 ti ? " " : "", type_name(type_args[ti]));
                if (w < 0) break;
                used += (size_t)w;
            }
            char lbl[224];
            int n = snprintf(lbl, sizeof(lbl), "method '%s' of instance %s [%s]",
                             method_name_str, tc_name->name, args);
            if (n > 0 && (size_t)n < sizeof(lbl))
                method_binding->diag_label = arena_strdup(e->arena, lbl, (size_t)n);
        }

        /* RT1 VARIANCE: an instance may accept MORE than its class signature
         * promises, never less.  The class signature is the contract callers
         * program against, so an instance that demands more would reject an
         * argument a generic caller was entitled to pass -- and find out at
         * run time, in the method's own entry check.
         *
         * The obligation is `class_pred(p) |- instance_pred(p)` over a fresh
         * parameter `p`, which is an ordinary query through the same seam.  A
         * class parameter with NO refinement promises nothing, so any instance
         * refinement on it is a strengthening unless the predicate is a
         * tautology -- and asking the solver is exactly how to tell those
         * apart.  Reported only on a REFUTATION; an undecidable pair keeps the
         * runtime check, as everywhere else. */
        if (n_m_ct_preds > 0) {
            for (uint32_t _ci = 0; _ci < n_m_ct_preds; _ci++) {
                uint32_t _pi = m_ct_param_idx[_ci];
                if (_pi >= n_method_params || !method_params[_pi] ||
                    !method_params[_pi]->name) continue;
                const char *pname = method_params[_pi]->name->name;

                const Form *cls_pred = NULL;
                const char *cls_var  = NULL;
                if (tc->methods[i].param_refine_preds &&
                    _pi < tc->methods[i].n_params) {
                    cls_pred = tc->methods[i].param_refine_preds[_pi];
                    cls_var  = tc->methods[i].param_refine_vars[_pi];
                }
                /* Identical predicates are the overwhelmingly common case
                 * (an instance restating its class signature); skip the
                 * solver entirely for them. */
                if (cls_pred == m_ct_preds[_ci]) continue;

                RefineEnv *venv = refine_env_new(e->arena);
                refine_env_set_resolver(venv, rt_refine_resolver(e), e);
                refine_env_declare(venv, pname,
                                   rt_sort_of_kind(method_params[_pi]->type.kind));
                if (cls_pred) refine_env_push(venv, cls_pred, cls_var, pname);

                Form *subj = form_sym(e->arena, impl_form->span,
                                      symtab_intern(e->st,
                                          strslice(pname, (uint32_t)strlen(pname))));
                char what[192];
                snprintf(what, sizeof(what),
                         "parameter '%s' of instance method '%s'", pname,
                         tc->methods[i].name ? tc->methods[i].name->name : "?");
                RefineObligation *vob = refine_collect_obligation(
                    &e->refine_obs, m_ct_preds[_ci], m_ct_vars[_ci], subj,
                    rt_sort_of_kind(method_params[_pi]->type.kind),
                    type_name(method_params[_pi]->type),
                    impl_form->span, venv,
                    arena_strdup(e->arena, what, strlen(what)), NULL);
                if (!vob) continue;
                /* Decide it silently, then ask separately for a witness.  The
                 * ordinary reporting path is wrong for this obligation: its
                 * failure is a declaration-vs-declaration inconsistency with
                 * its own diagnostic, not a `TUR-E0371` about a value. */
                vob->speculative = true;
                bool ok = refine_discharge_one(vob, e->arena);
                if (!ok && vob->vc && refine_model_search(vob->vc, e->arena)) {
                    diag_emit_with_code(DIAG_ERROR, impl_form->span,
                        TUR_E0374_REFINE_INSTANCE_STRONGER,
                        "instance method '%s' demands more of parameter '%s' "
                        "than the '%s' class signature promises",
                        tc->methods[i].name ? tc->methods[i].name->name : "?",
                        pname, tc->name ? tc->name->name : "?");
                    if (!cls_pred)
                        diag_emit(DIAG_NOTE, impl_form->span,
                                  "the class signature places no refinement on "
                                  "'%s', so callers may pass any value of its type",
                                  pname);
                }
            }
        }
        /* RT1: RESULT variance, which runs the OPPOSITE way to parameters.
         * A caller programming against the class signature relies on the
         * result predicate, so an instance must deliver at least as much as
         * the class promises: `instance_pred(r) |- class_pred(r)`.  (For
         * parameters it is `class_pred(p) |- instance_pred(p)` -- the class is
         * the hypothesis there and the goal here.)
         *
         * Only checked when the instance RESTATED a predicate.  An instance
         * that writes a plain return type inherits the class's, which cannot
         * be a weakening. */
        bool ret_variance_proved = false;
        if (impl_ret_pred_own && m_class_ret_pred &&
            impl_ret_pred != m_class_ret_pred) {
            const char *rvar = impl_ret_var ? impl_ret_var
                             : (m_class_ret_var ? m_class_ret_var : "r");
            RefineEnv *venv = refine_env_new(e->arena);
            refine_env_set_resolver(venv, rt_refine_resolver(e), e);
            refine_env_declare(venv, rvar, rt_sort_of_kind(return_type.kind));
            refine_env_push(venv, impl_ret_pred, impl_ret_var, rvar);

            Form *subj = form_sym(e->arena, impl_form->span,
                                  symtab_intern(e->st,
                                      strslice(rvar, (uint32_t)strlen(rvar))));
            char what[192];
            snprintf(what, sizeof(what), "the result of instance method '%s'",
                     tc->methods[i].name ? tc->methods[i].name->name : "?");
            RefineObligation *vob = refine_collect_obligation(
                &e->refine_obs, m_class_ret_pred, m_class_ret_var, subj,
                rt_sort_of_kind(return_type.kind), type_name(return_type),
                impl_form->span, venv,
                arena_strdup(e->arena, what, strlen(what)), NULL);
            if (vob) {
                vob->speculative = true;
                bool ok = refine_discharge_one(vob, e->arena);
                ret_variance_proved = ok;
                if (!ok && vob->vc && refine_model_search(vob->vc, e->arena)) {
                    diag_emit_with_code(DIAG_ERROR, impl_form->span,
                        TUR_E0374_REFINE_INSTANCE_STRONGER,
                        "instance method '%s' promises less about its result "
                        "than the '%s' class signature does",
                        tc->methods[i].name ? tc->methods[i].name->name : "?",
                        tc->name ? tc->name->name : "?");
                    diag_emit(DIAG_NOTE, impl_form->span,
                              "a caller programming against the class signature "
                              "relies on the class's result refinement, so an "
                              "instance must deliver at least as much");
                }
            }
        }
        /* CT0/RT1: carry this method's contract parameters on its binding.  The
         * parameters are resolved in THIS pass but the body is elaborated in
         * pass 2, so the binding is the record that spans both -- and it is
         * also where a dispatch site would look for them. */
        /* An UNANNOTATED parameter inherits the class's refinement, mirroring
         * the result direction below.  So the arrays are published whenever
         * either side has a predicate, not only when the instance restated
         * one -- otherwise inheritance would produce a predicate the entry
         * check (which reads exactly these arrays) never sees. */
        bool _inherits_any = false;
        if (tc->methods[i].param_refine_preds) {
            for (uint8_t _pi = 0; _pi < n_method_params &&
                                  _pi < tc->methods[i].n_params; _pi++) {
                if (_pi < MAX_FN_ARITY && !m_param_annotated[_pi] &&
                    tc->methods[i].param_refine_preds[_pi]) {
                    _inherits_any = true;
                    break;
                }
            }
        }
        if ((n_m_ct_preds > 0 || _inherits_any) && n_method_params > 0) {
            const Form **rp = (const Form **)arena_alloc(e->arena, n_method_params * sizeof(Form *));
            const char **rv = (const char **)arena_alloc(e->arena, n_method_params * sizeof(char *));
            const char **rn = (const char **)arena_alloc(e->arena, n_method_params * sizeof(char *));
            for (uint8_t _pi = 0; _pi < n_method_params; _pi++) {
                rp[_pi] = NULL;
                rv[_pi] = NULL;
                rn[_pi] = (method_params[_pi] && method_params[_pi]->name)
                        ? method_params[_pi]->name->name : NULL;
            }
            for (uint32_t _ci = 0; _ci < n_m_ct_preds; _ci++) {
                uint32_t _pi = m_ct_param_idx[_ci];
                if (_pi >= n_method_params) continue;
                rp[_pi] = m_ct_preds[_ci];
                rv[_pi] = m_ct_vars[_ci];
            }
            if (_inherits_any) {
                for (uint8_t _pi = 0; _pi < n_method_params &&
                                      _pi < tc->methods[i].n_params; _pi++) {
                    if (_pi >= MAX_FN_ARITY || m_param_annotated[_pi]) continue;
                    if (rp[_pi]) continue;   /* instance restated it: keep that */
                    rp[_pi] = tc->methods[i].param_refine_preds[_pi];
                    rv[_pi] = tc->methods[i].param_refine_vars
                            ? tc->methods[i].param_refine_vars[_pi] : NULL;
                }
            }
            method_binding->refine_param_preds = rp;
            method_binding->refine_param_vars  = rv;
            method_binding->refine_param_names = rn;
            method_binding->n_refine_params    = n_method_params;
        }
        /* RT1/RT4: and the result refinement -- its own if it restated one, the
         * class's otherwise.  The binding is the only record that reaches pass
         * 2, where the body exists and the check can be injected.
         *
         * Publishing it is gated on the check actually being emitted, matching
         * `rt_ret_guaranteed` on the `defn` path.  `refine_return_pred` is read
         * by RT4 as a FACT about the value a call produced, so a build that
         * strips contracts (`--no-contracts`, or a release build without
         * --keep-contracts) must not leave the fact behind after removing the
         * thing that enforced it.  Nothing reads this binding today -- a
         * dispatch does not resolve to it -- but the field's contract is
         * "published only when enforced", and a latent violation of it is a
         * trap for whoever wires the propagation up. */
        if (rt_contracts_emitted()) {
            method_binding->refine_return_pred = impl_ret_pred;
            method_binding->refine_return_var  = impl_ret_var;
            /* An instance that RESTATED its own promise only enforces that
             * one.  A dispatch site is handed the CLASS's promise (it cannot
             * know which instance runs), so the class predicate has to be
             * enforced here too -- unless the variance obligation actually
             * proved that the instance's implies it, in which case the
             * instance's own check already covers it.
             *
             * Reporting only on a refutation is right for the diagnostic and
             * not enough for this: an UNDECIDABLE pair emits no error, so
             * without the extra check the class promise would be enforced by
             * nothing while callers relied on it. */
            if (impl_ret_pred_own && m_class_ret_pred &&
                impl_ret_pred != m_class_ret_pred && !ret_variance_proved) {
                method_binding->refine_class_ret_pred = m_class_ret_pred;
                method_binding->refine_class_ret_var  = m_class_ret_var;
            }
        }
        method_fd->binding = method_binding;
        method_fd->params = method_params;
        method_fd->n_params = n_method_params;
        method_fd->body = method_body;
        method_fd->is_variadic = false;
        method_fd->closure = NULL;
        method_fd->param_types = method_param_types;
        method_fd->may_capture = false;
        method_fd->inferred_effect_row = NULL;  /* must be NULL; effect_check_pass reads this */
        constraint_set_init(&method_fd->constraints);
        /* LS2/LS3: instance methods carry no surface borrow lifetimes; give the
         * lifetime pass a clean context + return Type rather than garbage. */
        lifetime_context_init(&method_fd->lifetime_ctx);
        method_fd->return_type = type_simple(TY_UNKNOWN, CK_COPY);

        /* method_impls[i] must be populated now so a sibling `.method self`
         * dispatch (including forward / mutually-recursive references) resolves
         * during pass 2.  The file-scope registration and global binding are
         * deferred to pass 2, AFTER the body is elaborated, so they happen
         * after the body's lifted lambdas are registered -- preserving the
         * original `[body lambdas...][method def]` emit order.  Registering the
         * method def first would emit the method ahead of its closure's lifted
         * thunk; the closure's file-scope env struct (written while emitting the
         * method body) would then land textually inside the method, out of
         * scope for the later thunk -- an "undefined struct __env_N" miscompile. */
        method_impls[i] = method_fd;
        /* M4a: backlink the method FnDef to its owning instance so emit_module
         * can identify instance methods in O(1) and (when the class is non-HKT)
         * route them through the per-instantiation emit path.  See
         * docs/archive/m4-typeclass-per-method-abi-plan.md. */
        method_fd->owner_instance = inst;

        /* Stash what pass 2 needs to elaborate this method's body. */
        passes[i].impl_form       = impl_form;
        passes[i].impl_body_start = impl_body_start;
        passes[i].method_params   = method_params;
        passes[i].n_method_params = n_method_params;
        passes[i].method_fd       = method_fd;
        passes[i].arrow_return    = arrow_return;
        passes[i].ret_adt    = (return_type.kind == TY_ADT)    ? return_type.as.adt_.def    : NULL;
        passes[i].ret_kind   = return_type.kind;
        passes[i].ret_full   = return_type;  /* Phase 0: retain the whole type */
        passes[i].ret_was_class_var = ret_was_class_var;  /* Phase 3 */
    }

    /* Bring the constraint tyvars (e.g. `A` from `[(Eq A)]`) into the
     * signature-tyvar scope for the duration of pass 2's body elaboration, so a
     * bare `A` in an ascription resolves to the tyvar over a same-named global
     * type (root cause A). Saved/restored to keep enclosing scopes intact.
     * (M5 gap 4 reached the same goal via a separate `inst_body_type_params`
     * scope; main's `sig_tyvars` route subsumes it, so only `.tyvar` recording
     * above is kept for the emit-side composition pass.) */
    uint8_t saved_n_sig_tyvars = e->n_sig_tyvars;
    for (uint8_t ti = 0; ti < n_constraint_tyvar_syms; ti++) {
        if (e->n_sig_tyvars >= 32) break;
        const char *nm = constraint_tyvar_syms[ti]
            ? constraint_tyvar_syms[ti]->name : NULL;
        if (!nm) continue;
        bool dup = false;
        for (uint8_t s = 0; s < e->n_sig_tyvars; s++) {
            if (e->sig_tyvars[s] && strcmp(e->sig_tyvars[s], nm) == 0) {
                dup = true; break;
            }
        }
        if (!dup) {
            e->sig_tyvar_kinds[e->n_sig_tyvars] = KIND_STAR;
            e->sig_tyvars[e->n_sig_tyvars++] = nm;
        }
    }

    /* Pass 2: every sibling's FnDef shell and `method_impls` slot is now
     * populated, so elaborate each method body.  A call to a sibling defined
     * later in this same `definstance` -- or a mutually recursive pair --
     * resolves because the dispatcher finds a non-NULL slot for it. */
    for (uint8_t i = 0; i < tc->n_methods; i++) {
        InstMethodPass *mp = &passes[i];
        Form *impl_form = mp->impl_form;
        uint32_t impl_body_start = mp->impl_body_start;

        /* Elaborate the body - push a scope with method parameters */
        Scope method_scope;
        scope_init(&method_scope, e->scope);
        e->scope = &method_scope;

        /* Add method parameters to scope */
        for (uint8_t j = 0; j < mp->n_method_params; j++) {
            scope_add(&method_scope, mp->method_params[j]);
        }

        /* ER3: Increment fn_body_depth so that (perform ...) inside an instance
         * method body does not trigger TUR-E0008 (unhandled effect at top level).
         * The handler is expected to be provided at the call site. */
        e->fn_body_depth++;
        /* early-return-bypasses-return-refinement: a `return` in the method gets
         * the result checks the whole-body wrap below gives its tail value. */
        const RetContract *inst_saved_ret_contract = e->ret_contract;
        e->ret_contract = NULL;
        {
            const Binding *rb = mp->method_fd ? mp->method_fd->binding : NULL;
            if (rb && rb->refine_return_pred && rt_contracts_emitted()) {
                RetContract *rc = (RetContract *)arena_alloc(e->arena, sizeof(RetContract));
                memset(rc, 0, sizeof(*rc));
                rc->ret           = rb->refine_return_pred;
                rc->ret_var       = rb->refine_return_var;
                rc->class_ret     = rb->refine_class_ret_pred;
                rc->class_ret_var = rb->refine_class_ret_var;
                rc->subject       = rt_impl_method_name(mp->impl_form);
                e->ret_contract = rc;
            }
        }

        /* saffron-applied-class-var-result-takes-one-instances-type: push a
         * GROUND applied result (`(Option Pt)`, after the class-variable
         * substitution in pass 1) onto the expected-type channel, as elab_defn
         * does for its declared return.  In a dynamic file a generic
         * constructor call defers to that expectation instead of widening its
         * argument to `any`: `(some x)` builds the declared `(Option Pt)`, not
         * an `(Option any)` the impl's signature cannot return.  Kind-* classes
         * only, and only a result with no free type variable left -- an HKT
         * method's `(Option b)` keeps its element open on purpose. */
        Type *saved_body_expected = e->expected_type;
        {
            bool all_star = true;
            if (tc->type_param_kinds)
                for (uint8_t ki = 0; ki < tc->n_type_params; ki++)
                    if (tc->type_param_kinds[ki] != KIND_STAR) { all_star = false; break; }
            if (all_star && mp->ret_full.kind == TY_APP &&
                !m7_type_has_free_tyvar(mp->ret_full)) {
                Type *be = (Type *)arena_alloc(e->arena, sizeof(Type));
                *be = mp->ret_full;
                e->expected_type = be;
            }
        }

        Expr *method_body = e_nil(e, impl_form->span);
        uint32_t n_body = impl_form->as.list.len - impl_body_start;
        Type *body_expected = e->expected_type;
        /* loop-invariants-plan: the loops this body registers, decided below. */
        uint32_t li_start = e->n_loop_inv_sites;
        if (n_body > 0) {
            if (n_body == 1) {
                method_body = elab_form(e, impl_form->as.list.items[impl_body_start]);
                if (!method_body) { e->expected_type = saved_body_expected; e->fn_body_depth--; e->ret_contract = inst_saved_ret_contract; e->scope = method_scope.parent; scope_free(&method_scope); return NULL; }
            } else {
                Expr **items = (Expr **)arena_alloc(e->arena, n_body * sizeof(Expr *));
                for (uint32_t k = 0; k < n_body; k++) {
                    /* Only the tail form produces the result. */
                    e->expected_type = (k + 1 == n_body) ? body_expected : saved_body_expected;
                    items[k] = elab_form(e, impl_form->as.list.items[impl_body_start + k]);
                    if (!items[k]) { e->expected_type = saved_body_expected; e->fn_body_depth--; e->ret_contract = inst_saved_ret_contract; e->scope = method_scope.parent; scope_free(&method_scope); return NULL; }
                }
                method_body = expr_new(e->arena, EX_DO, items[n_body - 1]->type, impl_form->span);
                method_body->as.do_.items = items;
                method_body->as.do_.n = n_body;
            }
        }
        e->expected_type = saved_body_expected;

        e->fn_body_depth--;
        e->ret_contract = inst_saved_ret_contract;

        /* loop-invariants-plan: decide this body's `:invariant` loops the way
         * elab_defn does, before any contract wraps the body. */
        li_analyze_method_loops(e, li_start, mp->method_params,
                                mp->n_method_params,
                                mp->method_fd ? mp->method_fd->binding : NULL,
                                impl_form, impl_body_start);

        /* CT1: inject this instance method's parameter contract checks, while
         * the method scope is still current (the predicate elaborates in it).
         * Shared with `defn` and `fn` so a refined method parameter is enforced
         * rather than decorative. */
        {
            const Binding *mb = mp->method_fd ? mp->method_fd->binding : NULL;
            if (mb && mb->refine_param_preds && method_body && rt_contracts_emitted()) {
                /* The arrays are indexed by parameter, with NULL where there is
                 * no refinement, so the index list is just 0..n-1. */
                uint32_t idx[MAX_FN_ARITY];
                uint32_t n_idx = mb->n_refine_params < MAX_FN_ARITY
                               ? mb->n_refine_params : MAX_FN_ARITY;
                for (uint32_t _i = 0; _i < n_idx; _i++) idx[_i] = _i;
                Binding *m_check_fn = scope_lookup(&e->global, e->sym_tur_contract_check);
                method_body = rt_inject_param_checks(
                    e, method_body, m_check_fn,
                    mp->method_params, mp->n_method_params,
                    mb->refine_param_preds, mb->refine_param_vars,
                    idx, n_idx, rt_impl_method_name(impl_form), impl_form->span);
            }
        }

        /* RT1: check this method's RESULT against the refinement it promises --
         * its own if it restated one, otherwise the class's, which it inherits.
         * Before this, a `defclass` result refinement produced no check
         * anywhere: the identical predicate on a plain `defn` panicked, while a
         * class method returning -9 under `: #refine{ r : int | (>= r 0) }`
         * printed -9 and exited 0. */
        {
            const Binding *rb = mp->method_fd ? mp->method_fd->binding : NULL;
            if (rb && rb->refine_return_pred && method_body &&
                rt_contracts_emitted()) {
                Binding *m_check_fn =
                    scope_lookup(&e->global, e->sym_tur_contract_check);
                method_body = rt_wrap_return_check(
                    e, method_body, m_check_fn, rb->refine_return_pred,
                    rb->refine_return_var, "Return contract violated",
                    rt_impl_method_name(impl_form), impl_form->span);
                /* ...and the class's promise on top, when the instance's own
                 * was not proved to imply it.  See the comment where this is
                 * set: a dispatch site relies on the class predicate. */
                if (rb->refine_class_ret_pred)
                    method_body = rt_wrap_return_check(
                        e, method_body, m_check_fn, rb->refine_class_ret_pred,
                        rb->refine_class_ret_var,
                        "Class result contract violated",
                        rt_impl_method_name(impl_form), impl_form->span);
            }
        }

        /* S9: the class DECLARES the result, and `: any` is a widening target
         * for the body exactly as it is for a defn's (P6).  Nothing applied it
         * here, so `(size [x] 3)` under `(size [x : a] : any)` returned a raw
         * int64 from a function the class -- and every dispatch site -- types
         * as `tur_tagged_t` (cc: incompatible types when returning), in
         * plain Turmeric as much as in Saffron.  Widen while the method scope
         * is live, so a hoisted control temp binds inside it. */
        if (method_body && method_body->kind != EX_INLINE_C &&
            mp->ret_kind == TY_ANY && method_body->type.kind != TY_ANY &&
            method_body->type.kind != TY_NEVER) {
            Expr *widened = elab_coerce_to_any_return(e, method_body);
            if (widened) method_body = widened;
        }

        /* The inverse, for a DYNAMIC body under a type-variable result: a
         * Saffron `g : fn` extra is called dynamically, so `(g (.l t))` is an
         * `any` while the class declares `: a`.  Nothing narrowed it, and the
         * erased body returned a `tur_tagged_t` from an `int64_t` function
         * (saffron-dyn-witness-fn-arity-defaults-unary; the fuzzer's
         * `hofm_g_fn` + `hofm_r_a` legs).  The checked unbox -- the node
         * `(cast x T)` lowers to -- is the D5 seam a defn's concrete result
         * already takes; under a spec `a` resolves to the element's type, and
         * in the erased body an unresolved target fails loudly at runtime
         * rather than reinterpreting the box. */
        if (method_body && method_body->kind != EX_INLINE_C &&
            mp->ret_kind == TY_TYVAR && method_body->type.kind == TY_ANY &&
            lang_span_is_dynamic(method_body->span)) {
            Expr *unboxed = elab_any_unbox_to(e, method_body, mp->ret_full,
                                              method_body->span);
            if (unboxed) method_body = unboxed;
        }

        /* saffron-applied-class-var-result-takes-one-instances-type: with the
         * class variable substituted through an applied result, a body that
         * builds a DIFFERENT instantiation -- `(some 3)` under `Wrap [Pt]`'s
         * `(Option Pt)` -- is a type error the kind-only return check below
         * cannot see (both are TY_APP).  Unchecked, it reached cc as
         * "incompatible types when returning".  Both sides must be ground: an
         * open element is the carrier's business, not a mismatch. */
        if (method_body && method_body->kind != EX_INLINE_C &&
            mp->ret_full.kind == TY_APP && method_body->type.kind == TY_APP &&
            !m7_type_has_free_tyvar(mp->ret_full) &&
            !m7_type_has_free_tyvar(method_body->type) &&
            !type_eq(mp->ret_full, method_body->type)) {
            bool all_star = true;
            if (tc->type_param_kinds)
                for (uint8_t ki = 0; ki < tc->n_type_params; ki++)
                    if (tc->type_param_kinds[ki] != KIND_STAR) { all_star = false; break; }
            if (all_star) {
                Buf wb; buf_init(&wb); type_print(&wb, mp->ret_full); buf_putc(&wb, '\0');
                Buf gb; buf_init(&gb); type_print(&gb, method_body->type); buf_putc(&gb, '\0');
                const char *meth = tc->methods[i].name ? tc->methods[i].name->name : "?";
                diag_emit_with_code(DIAG_ERROR, method_body->span, TUR_E0001_TYPE_MISMATCH,
                    "instance method '%s' declares return type %s but its body "
                    "returns %s", meth, wb.data, gb.data);
                buf_free(&wb); buf_free(&gb);
                e->scope = method_scope.parent;
                scope_free(&method_scope);
                return NULL;
            }
        }

        /* Pop method scope */
        e->scope = method_scope.parent;
        scope_free(&method_scope);

        FnDef *method_fd = mp->method_fd;
        method_fd->body = method_body;

        /* constrained-generic-dispatch-tyvar-name-and-inlinec (Bug 2): stamp the
         * binding's `body_is_inline_c` flag here, the same way elab_fns.c does
         * for ordinary defns.  Instance-method FnDefs are built in this pass, so
         * without this the flag stays false on every instance method -- and the
         * call-site ABI logic that keys off it (the Phase D `&temp` pass-by-ptr
         * spill in emit_expr.c, and #439's emit_reresolved_receiver_is_by_ptr
         * bridge) would wrongly take the address of an inline-C instance's
         * by-value struct receiver, passing a `T *` to a by-value `T self`
         * formal -- a hard cc type error at both direct and generic call sites. */
        if (method_fd->binding) {
            method_fd->binding->body_is_inline_c =
                (method_body && method_body->kind == EX_INLINE_C);
            /* RM1: instance-method bodies are where the erased sum boxes the
             * leak sweep found actually come from (`ap`'s some(..) arms), so
             * the freshness flag matters most here -- and `alt-or`, which
             * returns an argument, is exactly what it must stay false for. */
            elab_stamp_sum_freshness(method_fd->binding, method_fd->params,
                                     method_fd->n_params, method_body);
            /* ... and the non-retaining parameter masks, so a statically
             * resolved dispatch site (`fn_binding` = this binding) gets the
             * same closure-env / sum-box drops a defn call site does.  `bind`
             * only CALLS its continuation, and a bind chain's envs were the
             * bulk of the RM1 residue. */
            elab_infer_nonretain_masks(method_fd->binding, method_fd->params,
                                       method_fd->n_params, method_body);
        }

        /* Arrow head: the method's declared return was the class variable (the
         * function arrow), flagged as a boxed-TY_FN placeholder in pass 1.  Now
         * that the body is elaborated, refine the result's full signature from
         * the body's actual closure type (arity, boxing) so a caller binding
         * the result -- `(let [h (comp f g)] (h 3))` -- sees a callable closure
         * with the right arity rather than an arity-0 shell. */
        if (mp->arrow_return && method_body && method_body->type.kind == TY_FN &&
            method_fd->binding->type.kind == TY_FN) {
            Type *rft = (Type *)arena_alloc(e->arena, sizeof(Type));
            *rft = method_body->type;
            /* Preserve the body's actual boxing: a *capturing* arrow body (e.g.
             * comp's `(fn [x] (g (f x)))`) is a boxed fat closure, so a caller
             * applying the result -- `(h 3)` -- uses the thunk convention; a
             * *non-capturing* body (e.g. Category ident's `(fn [x] x)`) is
             * lifted to a bare function pointer and must stay unboxed so `(i 41)`
             * emits a direct call (and arrow_fat_shim boxes it when fed to a fat
             * parameter).  Forcing boxed=true here mis-types the non-capturing
             * case and crashes the thunk dispatch on a code address. */
            method_fd->binding->type.as.fn.result_full_type = rft;
        }

        /* carrier-aware-return-unification: reject a genuine return-position
         * conflict between the method's declared return (after Phase RT
         * substitution) and its elaborated body via the shared
         * `return_position_conflict` dispatcher.  An instance method is normally a
         * CARRIER_METHOD position (the dispatcher only flags the float-COMMIT
         * direction, since a non-float-declared carrier method with a float
         * instance body is the deliberate per-instance bridge the typeclass ABI
         * resolves to the real register class) -- EXCEPT when Phase 3 classifies
         * it COMMITTED (see meth_cls below).  The arrow-head refinement above
         * already handled TY_FN results.  inline-C bodies (fiat TY_NIL) are
         * skipped; the int-literal -> float coercion is widened in place first.
         * e->scope and fn_body_depth are already restored here, so on error we
         * mirror the surrounding return-NULL. */
        if (method_body && method_body->kind != EX_INLINE_C) {
            rc_widen_int_literal_to_float_return(mp->ret_kind, method_body);
            /* carrier-aware-return-unification Phase 3: a method whose class-decl
             * return was the class type variable, grounded to a concrete
             * (free-tyvar-free) type for this instance, is a genuine per-instance
             * commit -- as strict as the equivalent defn.  Otherwise (a fixed
             * concrete class-decl slot, an explicit annotation, or a still-applied
             * HKT return like bind/ap's `(f b)` carrying a free element) the
             * method participates in the carrier and stays tolerant. */
            ReturnClass meth_cls =
                (mp->ret_was_class_var && !m7_type_has_free_tyvar(mp->ret_full))
                    ? RET_CLASS_COMMITTED
                    : RET_CLASS_CARRIER_METHOD;
            /* nil-tail-not-checked-against-declared-return: `false` keeps the
             * nil-body check OFF for instance methods for now.  A class-decl
             * return IS always written down, so `true` would be defensible --
             * but it is a separate blast radius from the defn case this closes,
             * and mixing them would make a regression here unattributable. */
            ReturnConflict rc = return_position_conflict(
                mp->ret_adt, mp->ret_kind, method_body->type,
                meth_cls, /*check_nil_body=*/false);
            if (rc != RET_CONFLICT_NONE) {
                const char *want = mp->ret_adt ? mp->ret_adt->name
                                 : typekind_to_string(mp->ret_kind);
                const char *meth = (tc->methods[i].name) ? tc->methods[i].name->name : "?";
                Buf gb; buf_init(&gb);
                type_print(&gb, method_body->type);
                buf_putc(&gb, '\0');
                switch (rc) {
                    case RET_CONFLICT_NOMINAL:
                        diag_emit_with_code(DIAG_ERROR, method_body->span,
                            TUR_E0001_TYPE_MISMATCH,
                            "instance method '%s' declares return type '%s' but "
                            "its body returns %s",
                            meth, want, gb.data);
                        break;
                    case RET_CONFLICT_REGISTER_CLASS:
                        diag_emit_with_code(DIAG_ERROR, method_body->span,
                            TUR_E0707_RETURN_REGISTER_CLASS_MISMATCH,
                            "instance method '%s' declares return type '%s' but "
                            "its body returns %s -- a float and a non-float live "
                            "in different register classes (xmm vs general-purpose),"
                            " so this is a register-class miscompile, not a "
                            "tolerable carrier bridge",
                            meth, want, gb.data);
                        break;
                    case RET_CONFLICT_POINTER_SCALAR:
                        diag_emit_with_code(DIAG_ERROR, method_body->span,
                            TUR_E0708_RETURN_POINTER_SCALAR_MISMATCH,
                            "instance method '%s' declares return type 'cstr' but "
                            "its body returns %s -- a bare integer is never a "
                            "valid string pointer, so this is a type-erasure bug, "
                            "not a tolerable carrier bridge",
                            meth, gb.data);
                        break;
                    /* TYPE_REVERSE / BOOL_INTEGER (TUR-E0709): reachable only when
                     * Phase 3 classified this method RET_CLASS_COMMITTED (its
                     * class-decl return was the class type variable, grounded to a
                     * concrete type for this instance), so a divergent concrete
                     * body is a real per-instance mismatch, not a carrier bridge. */
                    case RET_CONFLICT_TYPE_REVERSE:
                        diag_emit_with_code(DIAG_ERROR, method_body->span,
                            TUR_E0709_RETURN_TYPE_MISMATCH,
                            "instance method '%s' declares return type '%s' but "
                            "its body returns %s -- a string pointer is never a "
                            "valid integer, and this instance commits to the "
                            "class type variable's grounding, so there is no "
                            "carrier to bridge it",
                            meth, want, gb.data);
                        break;
                    case RET_CONFLICT_BOOL_INTEGER:
                        diag_emit_with_code(DIAG_ERROR, method_body->span,
                            TUR_E0709_RETURN_TYPE_MISMATCH,
                            "instance method '%s' declares return type '%s' but "
                            "its body returns %s -- bool and the integer family "
                            "are distinct types (boolean constants are "
                            "true/false, not 0/1), and this instance commits to "
                            "the class type variable's grounding, so there is no "
                            "carrier to bridge them",
                            meth, want, gb.data);
                        break;
                    case RET_CONFLICT_CARRIER_AGGREGATE:
                        diag_emit_with_code(DIAG_ERROR, method_body->span,
                            TUR_E0709_RETURN_TYPE_MISMATCH,
                            "instance method '%s' declares return type '%s' but "
                            "its body returns %s -- an aggregate is a real C "
                            "type (a struct, or a typed pointer to one), not the "
                            "int64 carrier, so there is no representation these "
                            "two share and nothing to bridge them",
                            meth, want, gb.data);
                        break;
                    case RET_CONFLICT_NIL_BODY:
                        /* Unreachable while the call above passes
                         * ret_annotated=false; present so the switch stays
                         * exhaustive and so turning that on is a one-line
                         * change with a diagnostic already waiting. */
                        break;
                    case RET_CONFLICT_NONE: break;  /* unreachable */
                }
                buf_free(&gb);
                return NULL;
            }
        }

        /* Register the method now -- after its body's lifted lambdas were
         * registered above -- so emit order is `[body lambdas...][method def]`,
         * matching the original single-pass behaviour. */
        scope_add(&e->global, method_fd->binding);
        Expr *method_def_expr = expr_new(e->arena, EX_FN_DEF,
                                         method_fd->binding->type, mp->impl_form->span);
        method_def_expr->as.fn_def_.fn = method_fd;
        elab_register_file_def(e, method_def_expr);
    }
    /* Restore the signature-tyvar scope now that every method body is done. */
    e->n_sig_tyvars = saved_n_sig_tyvars;

    /* Instance was registered and its fields wired up before the body loops
     * (see above) so intra-instance `.sibling self` dispatch resolves -- now
     * including forward references and mutual recursion.  The method_impls
     * slots are fully populated with elaborated bodies. */

    /* Phase HKT-P4: Orphan instance check.
     *
     * Rule: an instance is "orphan" when NEITHER the typeclass NOR any
     * struct-type type argument was defined in the current compilation unit.
     * In Rust terms: you may only define Foo<Bar> if you own Foo or Bar.
     *
     * Now a hard DIAG_ERROR since the module system (P19-6) has landed. */
    if (tc->origin_file_id != 0 && tc->origin_file_id != call->span.file_id &&
        !e->minting_dyn_any) {
        /* The typeclass is from a different file.
         * Check if any struct type-arg was defined here. */
        bool owns_a_type_arg = false;
        const char *cur_basename =
            tc_path_basename(diag_file_path(call->span.file_id));
        for (uint8_t i = 0; i < n_type_args && !owns_a_type_arg; i++) {
            /* An ADT (defdata/defgadt) type-arg, like Functor [Either], is owned
             * by the module that declares it. */
            if (type_args[i].kind == TY_ADT && type_args[i].as.adt_.def) {
                if (type_args[i].as.adt_.def->origin_file_id == call->span.file_id) {
                    owns_a_type_arg = true;
                }
                continue;
            }
            /* A partially-applied head, like Functor [(Either E)], is a TY_APP
             * whose fn carries the constructor's ADT/struct identity.  Credit
             * the instance to the constructor's owning module. */
            if (type_args[i].kind == TY_APP && type_args[i].as.app.fn) {
                const Type *fn = type_args[i].as.app.fn;
                if (fn->kind == TY_ADT && fn->as.adt_.def &&
                    fn->as.adt_.def->origin_file_id == call->span.file_id) {
                    owns_a_type_arg = true;
                }
                continue;
            }
            /* KB-030: a built-in primitive type (str, rc, ...) has no StructDef,
             * so it can never match origin_file_id.  Credit it to its designated
             * home file instead, so primitive instances can live in the natural
             * module without tripping the orphan check.  The home is found
             * either from the resolved TypeKind (rc/weak -> TY_RC/TY_WEAK) or,
             * for opaque-struct names with no dedicated kind (str), from the
             * recorded type-arg symbol. */
            const char *home = builtin_kind_home_basename(type_args[i].kind);
            if (!home && type_arg_syms && type_arg_syms[i]) {
                home = builtin_type_home_basename(type_arg_syms[i]->name);
            }
            if (home && cur_basename && strcmp(cur_basename, home) == 0) {
                owns_a_type_arg = true;
            }
        }
        if (!owns_a_type_arg) {
            diag_emit_with_code(DIAG_ERROR, call->span,
                      TUR_E0013_ORPHAN_INSTANCE,
                      "orphan instance: typeclass '%s' is defined in a different "
                      "module and none of the type arguments belong to this module; "
                      "move the instance to the module that defines the typeclass or "
                      "one of the type arguments",
                      tc_name->name);
        }
    }
    
    /* Create an INSTANCE_DEF expression for codegen */
    Expr *inst_expr = expr_new(e->arena, EX_INSTANCE_DEF, TYPE_NIL, call->span);
    inst_expr->as.instance_def_.instance = inst;
    elab_register_file_def(e, inst_expr);
    
    /* Create a nil expression as the result (definstance returns nothing) */
    return e_nil(e, call->span);
}

/* Phase 15: Elaborate (.method obj arg1 arg2 ...) - typeclass method call
 * 
 * Syntax: (.method obj arg1 arg2 ...)
 * Looks up the method in the typeclass for the type of obj, and generates a call.
 * For v1, we use direct method function calls (monomorphic only).
 * Full dictionary passing deferred to v2.
 */
/* Phase 12: EX_GET_FIELD — struct field access via (.fieldname s)
 *
 * Syntax: (.field s)  where s has type TY_STRUCT
 * Returns: the type of the named field
 * Also resolves immutable/mutable borrow of a field:
 *   (& (.field s))   → EX_BORROW_IMMUT wrapping EX_GET_FIELD
 *   (&mut (.field s)) → EX_BORROW_MUT wrapping EX_GET_FIELD
 */

/* Phase H §1: Build an EX_DICT node for a typeclass instance singleton.
 * The dict_name field is computed from the instance's typeclass and type args
 * using the same naming convention as emit.c (emit_dict_name / EX_INSTANCE_DEF).
 * Returns a TY_PTR_VOID-typed Expr that, when emitted, yields the address of
 * the global dictionary singleton cast to int64_t. */
/* RT1: the Binding standing for a class method's CLASS-LEVEL signature, used to
 * record a crossing at a dispatch that never resolved to an instance.
 *
 * Checking the caller's argument against the class predicate is sound by the
 * same variance argument that already licenses result propagation, run in the
 * other direction.  `TUR-E0374` rejects an instance that demands MORE than its
 * class, so every instance's parameter predicate is implied by the class's --
 * which makes the class predicate the strongest demand true of EVERY instance,
 * and an argument satisfying it acceptable to whichever instance runs.
 *
 * It is also strictly stronger than what a dynamic site could otherwise use.
 * With no resolved instance the dispatch falls back to an arbitrary
 * carrier-compatible one, and that instance's predicate is *weaker* than the
 * class's (or absent), so checking against it would demand less than the
 * contract while claiming to check the contract.
 *
 * Returns NULL -- meaning "record nothing" -- when no parameter carries a
 * refinement, which is the overwhelmingly common case. */
static const Binding *rt_class_method_refine_binding(Elab *e, TypeClass *tc,
                                                     uint8_t mi) {
    if (!e || !tc || mi >= tc->n_methods) return NULL;
    TypeClassMethod *m = &tc->methods[mi];
    if (m->refine_class_binding) return m->refine_class_binding;
    if (!m->param_refine_preds || m->n_params == 0) return NULL;

    bool any = false;
    for (uint8_t p = 0; p < m->n_params; p++)
        if (m->param_refine_preds[p]) { any = true; break; }
    if (!any) return NULL;

    Binding *b = (Binding *)arena_alloc(e->arena, sizeof(Binding));
    memset(b, 0, sizeof(*b));
    b->name      = m->name;
    b->is_global = true;

    /* Arg kinds matter: refine_resolve_call_sites falls back to TY_INT for a
     * parameter it cannot type, which would encode a float argument into the
     * wrong sort. */
    TypeKind kinds[MAX_FN_ARITY];
    uint32_t arity = m->n_params < MAX_FN_ARITY ? m->n_params : MAX_FN_ARITY;
    for (uint32_t p = 0; p < arity; p++)
        kinds[p] = m->param_types ? m->param_types[p].kind : TY_INT;
    b->type = type_fn(kinds, arity, m->return_type.kind);

    const Form **preds = (const Form **)arena_alloc(e->arena,
                              m->n_params * sizeof(const Form *));
    const char **vars  = (const char **)arena_alloc(e->arena,
                              m->n_params * sizeof(const char *));
    const char **names = (const char **)arena_alloc(e->arena,
                              m->n_params * sizeof(const char *));
    for (uint8_t p = 0; p < m->n_params; p++) {
        preds[p] = m->param_refine_preds[p];
        vars[p]  = m->param_refine_vars ? m->param_refine_vars[p] : NULL;
        names[p] = (m->param_names && m->param_names[p])
                 ? m->param_names[p]->name : NULL;
    }
    b->refine_param_preds = preds;
    b->refine_param_vars  = vars;
    b->refine_param_names = names;
    b->n_refine_params    = m->n_params;

    m->refine_class_binding = b;
    return b;
}

static Expr *make_dict_expr(Elab *e, TypeClassInstance *inst, Span span) {
    Expr *d = expr_new(e->arena, EX_DICT, type_from_kind(TY_PTR_VOID), span);
    d->as.dict_.instance = inst;

    /* Compute dict_name: "dict_<TypeClass>_<typearg>..." */
    const TypeClass *tc = inst->typeclass;
    char *dst = d->as.dict_.dict_name;
    size_t dstlen = sizeof(d->as.dict_.dict_name);
    char type_suffix[320] = "";  /* wide enough for the longest mangled component (<=259) */
    for (uint8_t i = 0; i < inst->n_type_args; i++) {
        if (i == 0) strncat(type_suffix, "_", sizeof(type_suffix) - strlen(type_suffix) - 1);
        const char *component = "T";
        switch (inst->type_args[i].kind) {
            case TY_INT:      component = "int";      break;
            case TY_BOOL:     component = "bool";     break;
            case TY_CSTR:     component = "cstr";     break;
            case TY_NIL:      component = "nil";      break;
            case TY_PTR_VOID: component = "ptr_void"; break;
            case TY_SYM:      component = "Sym";      break;
            case TY_ADT:
                /* CONV-S1 (defstruct-as-defadt): match emit_dict_name's TY_ADT arm
                 * so the DICT expr's dict_name agrees with the emitted struct. */
                if (inst->type_arg_syms && inst->type_arg_syms[i])
                    component = inst->type_arg_syms[i]->name;
                else if (inst->type_args[i].as.adt_.def &&
                         inst->type_args[i].as.adt_.def->name)
                    component = inst->type_args[i].as.adt_.def->name;
                break;
            default: break;
        }
        char comp_buf[128];
        tur_mangle_ident(component, comp_buf, sizeof(comp_buf));
        strncat(type_suffix, comp_buf, sizeof(type_suffix) - strlen(type_suffix) - 1);
    }
    snprintf(dst, dstlen, "dict_%s%s", tc->name->name, type_suffix);
    return d;
}

/* Phase RT: does `t` (or any nested type) reference the named type variable? */
static bool rt_type_mentions_tyvar(const Type *t, const char *name) {
    if (!t || !name) return false;
    switch (t->kind) {
        case TY_TYVAR:
            return t->as.tyvar_.name && strcmp(t->as.tyvar_.name, name) == 0;
        case TY_APP:
            return rt_type_mentions_tyvar(t->as.app.fn, name) ||
                   rt_type_mentions_tyvar(t->as.app.arg, name);
        default:
            return false;
    }
}

/* Phase RT: walk a method's declared return type in parallel with the expected
 * type, binding the dispatch tyvar `rv` to the corresponding subtree of the
 * expected type.  Handles the bare case (return == `a`, binds a := expected)
 * and structured returns ((Result a E) vs (Result User E), binds a := User).
 * Returns true and writes *out_bound when `rv` was located; false otherwise. */
static bool rt_unify_return(const Type *ret, const Type *expected,
                            const char *rv, Type *out_bound) {
    if (!ret || !expected) return false;
    if (ret->kind == TY_TYVAR && ret->as.tyvar_.name &&
        strcmp(ret->as.tyvar_.name, rv) == 0) {
        *out_bound = *expected;
        return true;
    }
    if (ret->kind == TY_APP && expected->kind == TY_APP) {
        if (rt_unify_return(ret->as.app.arg, expected->as.app.arg, rv, out_bound))
            return true;
        if (rt_unify_return(ret->as.app.fn, expected->as.app.fn, rv, out_bound))
            return true;
    }
    return false;
}

/* Shared callable-result helper (return-type-dispatch-nullary-arrow plan, T2).
 * A method whose binding return type is a *boxed* TY_FN carries a callable fat
 * closure value (the capturing arrow body recovered in pass 2 -- report
 * function-arrow-not-instantiable, fix #1).  Return its full signature so a
 * caller applying the result -- `(h 3)` -- sees the real arity and dispatches
 * through the fat (thunk) protocol instead of an arity-0 shell.  Otherwise fall
 * back to the supplied carrier type: the unboxed fn-handle ABI in
 * elab_method_call, or the unified/ascribed `bound` in return dispatch.
 *
 * Note the boxed gate: a regular closure-returning method may declare an
 * *unboxed* TY_FN return whose body nonetheless captures (so the runtime value
 * is a fat box, applied through TUR_APPLY1).  Typing that as a bare function
 * pointer would miscompile a direct call, so unboxed declared returns keep the
 * opaque carrier here.  Arrow methods, whose result_full_type is refined from
 * the *body* in pass 2 (accurate boxing), are handled directly in
 * elab_try_return_dispatch where the unboxed bare-fn case is wanted. */
static Type method_callable_result_type(const Binding *binding, Type fallback) {
    if (binding && binding->type.kind == TY_FN) {
        const Type *rft = binding->type.as.fn.result_full_type;
        if (rft && rft->kind == TY_FN && rft->as.fn.boxed) {
            return *rft;
        }
    }
    return fallback;
}

/* Predicate mirroring the method-finding phase of elab_try_return_dispatch:
 * true when `name` resolves to a return-only-dispatch typeclass method (a class
 * type param appears in the method's return type but in none of its parameter
 * types) and no ordinary binding shadows it.  Used by elab_if to recognise an
 * arm whose instance can only be picked from an expected result type, so a
 * concrete sibling arm can supply that type (return-directed-methods-pure-empty-
 * inference, fix direction #2). */
bool elab_symbol_is_return_dispatch_method(Elab *e, const Symbol *name) {
    if (!name) return false;
    /* A user/local defn of the same name wins (mirrors the `!fn_binding` gate
     * at the elab_try_return_dispatch call site); it is not return-directed. */
    if (scope_lookup(e->scope, name)) return false;

    TypeClassEnv *env = &e->typeclass_env;
    for (TypeClass *c = env->typeclasses; c != NULL; c = c->next) {
        for (uint8_t mi = 0; mi < c->n_methods; mi++) {
            const TypeClassMethod *m = &c->methods[mi];
            if (!(m->name->len == name->len &&
                  memcmp(m->name->name, name->name, name->len) == 0)) {
                continue;
            }
            /* Some class type param must appear ONLY in the return type. */
            bool any_tp_in_param = false;
            for (uint8_t ti = 0; ti < c->n_type_params && !any_tp_in_param; ti++) {
                const Symbol *tp = c->type_params[ti];
                if (!tp) continue;
                for (uint32_t pi = 0; pi < m->n_params; pi++) {
                    if (rt_type_mentions_tyvar(&m->param_types[pi], tp->name)) {
                        any_tp_in_param = true;
                        break;
                    }
                }
            }
            if (any_tp_in_param) continue;
            for (uint8_t ti = 0; ti < c->n_type_params; ti++) {
                const Symbol *tp = c->type_params[ti];
                if (!tp) continue;
                if (!rt_type_mentions_tyvar(&m->return_type, tp->name)) continue;
                bool in_param = false;
                for (uint32_t pi = 0; pi < m->n_params; pi++) {
                    if (rt_type_mentions_tyvar(&m->param_types[pi], tp->name)) {
                        in_param = true;
                        break;
                    }
                }
                if (in_param) continue;
                return true;
            }
        }
    }
    return false;
}

Expr *elab_try_return_dispatch(Elab *e, const Form *call, const Symbol *name,
                               bool *handled) {
    if (handled) *handled = false;
    if (!name || call->tag != F_LIST) return NULL;

    TypeClassEnv *env = &e->typeclass_env;

    /* Find a return-only-dispatch method named `name`: one of the class's type
     * parameters appears in the method's return type but in none of its
     * parameter types, so the instance can only be picked from the expected
     * result type. */
    TypeClass *tc = NULL;
    uint8_t midx = 0;
    const TypeClassMethod *meth = NULL;
    const char *disp_tv = NULL;
    for (TypeClass *c = env->typeclasses; c != NULL && !meth; c = c->next) {
        for (uint8_t mi = 0; mi < c->n_methods; mi++) {
            const TypeClassMethod *m = &c->methods[mi];
            if (!(m->name->len == name->len &&
                  memcmp(m->name->name, name->name, name->len) == 0)) {
                continue;
            }
            /* ECS E2d-P6 (Issue 2): multi-param dispatch resolvable from an
             * argument-position param.  When SOME class type parameter appears
             * in a parameter type, argument-based dispatch can pin the instance
             * (the receiver's static type selects it), and any return-only
             * param is then read off the matched instance.  Decline
             * return-only dispatch here so elab_method_call's argument dispatch
             * takes over -- otherwise a method like `(sop-get [s : S idx : int]
             * : E)` on `(defclass StorageOps [S E])` would key the lookup on E
             * (return-only) and fail to find the instance even though S is fully
             * known from the receiver.  A genuinely return-only method (every
             * class type param absent from the parameter list, e.g. Serializable
             * `(deserialize [b : ptr<void>] : a)`) still flows through below. */
            bool any_tp_in_param = false;
            for (uint8_t ti = 0; ti < c->n_type_params && !any_tp_in_param; ti++) {
                const Symbol *tp = c->type_params[ti];
                if (!tp) continue;
                for (uint32_t pi = 0; pi < m->n_params; pi++) {
                    if (rt_type_mentions_tyvar(&m->param_types[pi], tp->name)) {
                        any_tp_in_param = true;
                        break;
                    }
                }
            }
            if (any_tp_in_param) continue;
            for (uint8_t ti = 0; ti < c->n_type_params; ti++) {
                const Symbol *tp = c->type_params[ti];
                if (!tp) continue;
                if (!rt_type_mentions_tyvar(&m->return_type, tp->name)) continue;
                bool in_param = false;
                for (uint32_t pi = 0; pi < m->n_params; pi++) {
                    if (rt_type_mentions_tyvar(&m->param_types[pi], tp->name)) {
                        in_param = true;
                        break;
                    }
                }
                if (in_param) continue;
                tc = c; midx = mi; meth = m; disp_tv = tp->name;
                break;
            }
            if (meth) break;
        }
    }
    if (!meth) return NULL;  /* not a return-only-dispatch method */
    if (handled) *handled = true;

    /* Arrow head: if this class has a function-arrow instance and the call has
     * arguments, the arrow argument (a function) selects the instance via the
     * normal argument-based dispatcher -- so decline return-dispatch here and
     * let argument dispatch take over, rather than demanding a return-type
     * ascription.  Nullary arrow methods (no argument to dispatch on, e.g. an
     * ArrowZero `zero`) still fall through to expected-type return-dispatch. */
    if (call->as.list.len > 1 &&
        typeclass_has_arrow_instance(&e->typeclass_env, tc)) {
        if (handled) *handled = false;
        return NULL;
    }

    Type bound;
    TypeClassInstance *inst = NULL;
    bool abstract_return_dispatch = false;
    if (!e->expected_type) {
        /* Mechanism B (return-type-dispatch-nullary-arrow plan, T3): with no
         * expected type a nullary arrow method (e.g. Category `ident`) has
         * nothing to dispatch on.  Fall back to the instance set: if `tc`+`meth`
         * has exactly one implementing instance and its head is the function
         * arrow, select it unambiguously.  Gating to a *unique arrow* instance
         * keeps existing multi-instance return-only classes (default-of,
         * schema-of, decode!, ...) on the ascription-required path below. */
        TypeClassInstance *uniq = NULL;
        int n_impl = 0;
        for (TypeClassInstance *it = env->instances; it; it = it->next) {
            if (it->typeclass != tc) continue;
            if (midx >= it->n_method_impls || !it->method_impls[midx]) continue;
            n_impl++;
            uniq = it;
        }
        if (n_impl == 1 && uniq->n_type_args > 0 &&
            uniq->type_args[0].kind == TY_FN) {
            inst  = uniq;
            bound = uniq->type_args[0];
        } else {
            /* T4: genuinely ambiguous (no instance, or >1, or non-arrow head) --
             * keep requiring an ascription; never silently pick an instance. */
            diag_emit(DIAG_ERROR, call->span,
                      "cannot infer type for return-directed method '%s'; add a "
                      "type ascription, e.g. (:: (%s ...) T)",
                      name->name, name->name);
            return NULL;
        }
    } else {
        /* Bind the dispatch tyvar from the expected type (bare or structured). */
        if (!rt_unify_return(&meth->return_type, e->expected_type, disp_tv,
                             &bound)) {
            diag_emit(DIAG_ERROR, call->span,
                      "ascribed type does not match the result shape of '%s'",
                      name->name);
            return NULL;
        }
        /* return-dispatch-tyvar (docs/archive/history/return-dispatch-tyvar-silent-
         * misdispatch.md): when the ascription pins the result to an *abstract*
         * type variable -- e.g. `(:: (deserialize b) A)` inside a constrained
         * `(defn round [A] [(Serializable A)] ...)` -- `bound` is a TY_TYVAR
         * (or the TY_STRUCT-NULL-def "abstract tyvar" representation) and there
         * is no concrete instance to pick yet.  Mirror the receiver-dispatch
         * path (`obj_is_abstract_tyvar` in elab_method_call): select the
         * carrier-compatible `int` instance as the polymorphic-base
         * representative, and tag the call so the emit-side re-resolution
         * (emit_core.c) specializes it to the concrete A per monomorphization.
         * Without this we either silently mis-dispatched to the `ptr<void>`
         * instance (the original bug, back when `deserialize` returned `:int`
         * and the class var was absent from the signature) or hard-errored. */
        bool bound_is_abstract_tyvar =
            bound.kind == TY_TYVAR;
        if (bound_is_abstract_tyvar) {
            /* nullary-class-method-unresolvable-over-newtype-tyvar: this search
             * accepted ONLY a literal `TY_INT` head, so a class instanced solely
             * over `defopaque` newtypes -- the one idiom that gives a single
             * carrier several algebras, `(defopaque Sum :int)` beside
             * `(defopaque Product :int)` -- found no representative and
             * hard-errored with "no instance '<Class> tyvar'".
             *
             * A non-pointer opaque newtype IS the int64 carrier at runtime
             * (AdtDef.is_opaque with opaque_base_is_ptr false), so it is as
             * valid a polymorphic-base representative as a bare `int`, and
             * emit-side re-resolution specializes it per monomorphization --
             * PROVIDED there is a specialization to re-resolve into.
             *
             * That proviso is the gate.  Specs are split on argument types, and
             * `Sum` / `Product` are distinct `Type`s even though both render
             * int64, so a generic that takes one as a PARAMETER interns two
             * specs and Gap H names them `..._int64_t` / `..._int64_t__h1`.  A
             * generic whose class tyvar reaches no parameter
             * (`(defn zero-of [^Mo A] [] : A (mz))`) interns ONE spec for every
             * instantiation, so a representative picked here would be baked in
             * for all of them: `Sum` and `Product` would both answer with
             * whichever instance was chosen, silently.  Requiring the tyvar to
             * reach a parameter keeps that shape on its existing hard error,
             * which is the right answer until return-only specialization can
             * distinguish same-carrier newtypes.
             *
             * Prefer a real `int` head when one exists, so the representative
             * choice is unchanged for every class that has one.  The
             * receiver-directed twin in elab_method_call has had this two-tier
             * shape since constrained-generic-instance-element-dispatch; this is
             * the return-directed version, which never grew it. */
            bool tyvar_reaches_param = false;
            if (bound.as.tyvar_.name) {
                for (uint8_t ci = 0; ci < e->cur_fn_n_constraints && ci < 32; ci++) {
                    const TypeConstraint *con = &e->cur_fn_constraints[ci];
                    /* class-superclasses SC3: a superclass constraint reaches a
                     * parameter exactly when the subclass constraint that
                     * entails it does, so the mask bit is the entailing
                     * constraint's. */
                    if (!con || !typeclass_entails(env, con->typeclass, tc)) continue;
                    if (!con->tyvar || !con->tyvar->name) continue;
                    if (strcmp(con->tyvar->name, bound.as.tyvar_.name) != 0) continue;
                    if (e->cur_fn_constraint_param_mask & (1u << ci))
                        tyvar_reaches_param = true;
                    break;
                }
            }
            TypeClassInstance *carrier_opaque = NULL;
            for (TypeClassInstance *it = env->instances; it; it = it->next) {
                if (it->typeclass != tc) continue;
                if (midx >= it->n_method_impls || !it->method_impls[midx]) continue;
                if (it->n_type_args == 0) continue;
                if (it->type_args[0].kind == TY_INT) { inst = it; break; }
                if (!carrier_opaque && tyvar_reaches_param &&
                    it->type_args[0].kind == TY_ADT) {
                    const AdtDef *ad = it->type_args[0].as.adt_.def;
                    if (ad && ad->is_opaque && !ad->opaque_base_is_ptr)
                        carrier_opaque = it;
                }
            }
            if (!inst) inst = carrier_opaque;
            /* constrained-hkt-pure-and-byvalue-carriers (gap 1): the search above
             * looks for a kind-`*` `int`-headed representative, which a
             * higher-kinded class never has -- every `Applicative`/`Monad`
             * instance head is a type CONSTRUCTOR.  So `pure`/`empty` on an
             * abstract `m` inside a constrained poly fn found no representative
             * and hard-errored ("no instance 'Applicative tyvar'"), even though
             * the receiver-directed methods of the very same constraint (`ap`,
             * `bind`) resolve fine through the representative path in
             * elab_method_call.
             *
             * Mirror that path for the higher-kinded case: when the abstract
             * tyvar IS this function's constraint variable and `tc` is the
             * constraint's class, use the constraint's ambient representative
             * instance as the polymorphic base.  Emit-side re-resolution then
             * specializes it per monomorphization, exactly as for the kind-`*`
             * representative above.  Gated on the ambient constraint so an
             * unconstrained `(pure 42)` with no expected type still reports the
             * ascription-required diagnostic rather than silently picking an
             * instance.
             *
             * The gate is that `bound` names THIS body's abstract constraint
             * variable -- not that `tc` is the ambient class.  A body may
             * constrain several classes on one type constructor (`[^Monad m
             * ^Applicative m ...]`), and only the first is recorded as ambient;
             * keying on the tyvar covers the rest.  This is the same latitude
             * the receiver-directed path already takes, which picks a
             * representative for `ap`/`bind` without consulting the constraint
             * set at all. */
            if (!inst && e->cur_hkt_constraint_tyvar && bound.as.tyvar_.name &&
                strcmp(bound.as.tyvar_.name, e->cur_hkt_constraint_tyvar) == 0) {
                TypeClassInstance *repr =
                    (e->cur_hkt_dict_binding && e->cur_hkt_dict_binding->ambient_repr)
                        ? e->cur_hkt_dict_binding->ambient_repr : NULL;
                /* The ambient repr belongs to the FIRST constraint's class; it is
                 * only usable here when that is also `tc`. */
                if (repr && (repr->typeclass != tc ||
                             midx >= repr->n_method_impls || !repr->method_impls[midx]))
                    repr = NULL;
                if (!repr) {
                    for (TypeClassInstance *it = env->instances; it; it = it->next) {
                        if (it->typeclass != tc) continue;
                        if (midx >= it->n_method_impls || !it->method_impls[midx])
                            continue;
                        repr = it;
                        break;
                    }
                }
                inst = repr;
            }
            abstract_return_dispatch = (inst != NULL);
        }
        if (!inst) {
            inst = typeclass_env_lookup_instance(env, tc, &bound, 1);
        }
        if (!inst) {
            diag_emit(DIAG_ERROR, call->span,
                      "no instance '%s %s'", tc->name->name, type_name(bound));
            return NULL;
        }
    }
    FnDef *impl = inst->method_impls[midx];
    if (!impl || !impl->binding) {
        diag_emit(DIAG_ERROR, call->span,
                  "instance '%s %s' has no implementation for method '%s'",
                  tc->name->name, type_name(bound), name->name);
        return NULL;
    }

    /* Elaborate any arguments (clearing the expected-type channel so they are
     * synthesized normally). */
    uint32_t n_args = call->as.list.len - 1;
    Expr **args = (n_args == 0) ? NULL
        : (Expr **)arena_alloc(e->arena, n_args * sizeof(Expr *));
    Type *saved_expected = e->expected_type;
    e->expected_type = NULL;
    for (uint32_t i = 0; i < n_args; i++) {
        args[i] = elab_form(e, call->as.list.items[1 + i]);
        if (!args[i]) { e->expected_type = saved_expected; return NULL; }
    }
    e->expected_type = saved_expected;

    /* Thread the callable result type (plan T2/T3).  For an arrow method the
     * impl binding's result_full_type was refined from the *body* in pass 2,
     * so its boxing is accurate: a non-capturing body (Category ident's
     * `(fn [x] x)`) is an unboxed bare function pointer applied directly
     * (`(i 41)`), while a capturing body (comp's `(fn [x] (g (f x)))`) is a
     * boxed fat closure applied through the thunk protocol.  Use it whichever
     * way it is boxed so the result carries the real arity; non-arrow
     * return-dispatch methods (default-of, schema-of, ...) have no fn-typed
     * result_full_type and fall back to the unified/ascribed return type.
     *
     * Prereq 5: when an ascription pins the call's return type (the common
     * case for return-dispatch methods), use `*e->expected_type` rather than
     * the bare `bound` dispatch-tyvar value. Otherwise a method declared
     * `(decode [v : int] : (Result a cstr))` ascribed to `(Result cstr cstr)`
     * would set the call's elab type to `cstr` (just the `a`-binding) instead
     * of `(Result cstr cstr)` -- the carrier-to-by-value bridge at the
     * `ok-val` consumer's call site then misses because the elab type kind is
     * TY_CSTR rather than TY_APP, and the compile fails to compile when
     * passing the int64 carrier into a parameter expecting the by-value
     * struct. */
    Type result_type = e->expected_type ? *e->expected_type : bound;
    if (impl->binding && impl->binding->type.kind == TY_FN) {
        const Type *rft = impl->binding->type.as.fn.result_full_type;
        if (rft && rft->kind == TY_FN) {
            result_type = *rft;
        }
    }
    Expr *out = expr_new(e->arena, EX_CALL, result_type, call->span);
    out->as.call_.fn_binding = impl->binding;
    out->as.call_.fn_expr    = NULL;
    out->as.call_.args       = args;
    out->as.call_.n_args     = n_args;
    /* M4c Path A return-side
     * (docs/archive/m4c-path-a-result-side-needs-return-dispatch-elab-hook.md):
     * mirror the receiver-dispatch path's abi_bindings population (around
     * line 4047) so emit_abi_register_call mints a per-instantiation spec
     * for return-dispatch typeclass methods too.  `bound` here is the
     * call-site's pinned dispatch-tyvar value — for `(:: (dec 42) (Result
     * int cstr))` it's `int`, exactly what the spec system needs to
     * substitute the method's `(Result a cstr)` return into `(Result int
     * cstr)`.  HKT carve-out preserved. */
    if (inst && tc && out->as.call_.fn_binding != NULL) {
        bool is_hkt = false;
        if (tc->type_param_kinds) {
            for (uint8_t i = 0; i < tc->n_type_params; i++) {
                if (tc->type_param_kinds[i] != KIND_STAR) { is_hkt = true; break; }
            }
        }
        if (!is_hkt && tc->n_type_params == 1 && tc->type_params[0]) {
            /* nested-construct/constrained-instance return dispatch: besides
             * binding the class var (`a -> (Option cstr)`), also bind the matched
             * instance's OWN head tyvars by unifying its head (`(Option A)`)
             * against the pinned dispatch value (`(Option cstr)`) -> `A -> cstr`.
             * A constrained instance body (`(definstance Dec [Option] [(Dec A)]
             * ...)`) writes its nested construct/return-dispatch seams in terms
             * of `A`; without grounding it the spec collapses every element to the
             * int64-carrier representative (`dec_int`, `Option__int`) and a
             * cstr/float/struct consumer misreads the carrier int.  Mirrors the
             * receiver-dispatch path's head-tyvar collection. */
            /* The instance's constraint vars (`A` of `[(Dec A)]`) are NOT in the
             * bare head `Option`; recover them from the pinned dispatch value's
             * app args at each constraint's param_idx -- `a = (Option cstr)` ->
             * A = arg[0] = cstr. */
            Type barg_spine[8]; uint8_t n_barg = 0;
            {
                Type tcur = bound; Type stack[8]; uint8_t ns = 0;
                while (tcur.kind == TY_APP && tcur.as.app.fn && tcur.as.app.arg) {
                    if (ns < 8) stack[ns++] = *tcur.as.app.arg;
                    tcur = *tcur.as.app.fn;
                }
                for (int s = (int)ns - 1; s >= 0 && n_barg < 8; s--)
                    barg_spine[n_barg++] = stack[s];
            }
            const Symbol *hb_names[ABI_TYPE_BINDINGS_MAX];
            Type hb_types[ABI_TYPE_BINDINGS_MAX];
            uint8_t hb_n = 0;
            uint8_t neg_pos = 0;  /* positional index for standalone (param_idx<0) constraints */
            for (uint8_t ci = 0;
                 ci < inst->n_type_param_constraints &&
                 hb_n < ABI_TYPE_BINDINGS_MAX - 1; ci++) {
                const TypeConstraint *cstr = &inst->type_param_constraints[ci];
                if (!cstr->tyvar || !cstr->tyvar->name) continue;
                /* A constraint tied to a head type-param position uses param_idx;
                 * a STANDALONE constraint (`[Option] [(Dec A)]`, param_idx == -1)
                 * binds positionally against the dispatch value's app args -- the
                 * `[(Dec A)]` element corresponds to `(Option cstr)`'s arg[0]. */
                int pidx = cstr->param_idx >= 0 ? cstr->param_idx : (int)neg_pos;
                if (cstr->param_idx < 0) neg_pos++;
                if (pidx < 0 || pidx >= (int)n_barg) continue;
                hb_names[hb_n] = cstr->tyvar;
                hb_types[hb_n] = barg_spine[pidx];
                hb_n++;
            }
            uint8_t total = (uint8_t)(1 + hb_n);
            AbiTypeBinding *bindings = (AbiTypeBinding *)arena_alloc(
                e->arena, total * sizeof(AbiTypeBinding));
            bindings[0].name = tc->type_params[0]->name;
            bindings[0].type = bound;
            for (uint8_t k = 0; k < hb_n; k++) {
                bindings[1 + k].name = hb_names[k]->name;
                bindings[1 + k].type = hb_types[k];
            }
            out->as.call_.abi_bindings = bindings;
            out->as.call_.n_abi_bindings = total;
        }
        /* M7 layer-4: a return-directed HKT method -- Applicative `pure`/`wrap`,
         * `[x : a] : (f a)` -- has its class var `f` ONLY in the result, so the
         * instance is picked from the ascribed return type (`bound` = the
         * constructor, e.g. Option) and the element `a` is recovered from the
         * argument types.  Mirror the receiver-dispatch path's by-value spec
         * interning (elab_method_call) so the instance method emits a by-value
         * `(Option int)` return instead of the int64 carrier (which the by-value
         * consumer would then misread -> silent miscompile).  Gated on a
         * by-value-constructible body, exactly as the receiver path is. */
        if (is_hkt && tc->n_type_params >= 1 &&
            tc->type_params[0] && impl->body &&
            impl->body->kind != EX_INLINE_C &&
            m7_body_constructs_byvalue(impl->body)) {
            const Symbol *m7_names[16];
            Type m7_types[16];
            uint8_t m7_n = 0;
            for (uint32_t i = 0; i < n_args; i++) {
                if (i < meth->n_params && args[i])
                    m7_collect_tyvar_bindings(e, meth->param_types[i],
                                              args[i]->type, m7_names, m7_types,
                                              &m7_n, 16);
            }
            /* Bind the HKT class var to the CONSTRUCTOR HEAD of the ascribed
             * result (`Option`), not the full applied `(Option int)`, matching
             * the receiver-dispatch path (an in-body applied occurrence `(f a)`
             * then resolves to `(Option a)` -> `(Option int)` at emit). */
            Type hkt_head = bound;
            while (hkt_head.kind == TY_APP && hkt_head.as.app.fn)
                hkt_head = *hkt_head.as.app.fn;
            uint8_t total = (uint8_t)(1 + m7_n);
            if (total > ABI_TYPE_BINDINGS_MAX) total = ABI_TYPE_BINDINGS_MAX;
            AbiTypeBinding *bindings = (AbiTypeBinding *)arena_alloc(
                e->arena, total * sizeof(AbiTypeBinding));
            bindings[0].name = tc->type_params[0]->name;
            bindings[0].type = hkt_head;
            uint8_t bi = 1;
            for (uint8_t k = 0; k < m7_n && bi < total; k++) {
                bindings[bi].name = m7_names[k]->name;
                bindings[bi].type = m7_types[k];
                bi++;
            }
            out->as.call_.abi_bindings = bindings;
            out->as.call_.n_abi_bindings = bi;
        }
    }
    /* return-dispatch-tyvar: tag the abstract-tyvar return-dispatch call with a
     * dict_arg carrying the (representative int) instance + method name, so the
     * emit-side return-dispatch re-resolution can specialize it to the concrete
     * A per monomorphization (keyed on the call's result type, which is the
     * abstract tyvar here).  `out->type` stays the tyvar `bound`. */
    if (abstract_return_dispatch && inst) {
        Expr *dict_expr = make_dict_expr(e, inst, call->span);
        tur_mangle_ident(name->name, dict_expr->as.dict_.method_name,
                         sizeof(dict_expr->as.dict_.method_name));
        out->as.call_.dict_arg = dict_expr;
        out->type = bound;  /* keep the abstract tyvar for emit re-resolution */
    }
    return out;
}

/* unascribed-carrier-helper-read-collapses-element-tyvar: structurally match a
 * carrier helper's declared parameter type (carrying the helper's type-param
 * `R`) against the actual argument type, returning the subtype at `R`'s
 * position.  Mirror of emit_core.c:emit_pattern_extract_classvar for the elab
 * side. */
static bool tc_pattern_extract_var(const Type *pattern, const Type *concrete,
                                   const char *varname, Type *out) {
    if (!pattern || !concrete || !varname) return false;
    if (pattern->kind == TY_TYVAR && pattern->as.tyvar_.name &&
        strcmp(pattern->as.tyvar_.name, varname) == 0) {
        *out = *concrete;
        return true;
    }
    if (pattern->kind == TY_APP && concrete->kind == TY_APP) {
        if (tc_pattern_extract_var(pattern->as.app.fn, concrete->as.app.fn,
                                   varname, out))
            return true;
        if (tc_pattern_extract_var(pattern->as.app.arg, concrete->as.app.arg,
                                   varname, out))
            return true;
    }
    return false;
}

/* unascribed-carrier-helper-read-collapses-element-tyvar: true when `obj` is an
 * UNASCRIBED generic carrier-helper read used as a class-method receiver --
 * `(tag (vec-get v i))` -- whose recovered element type is still an abstract
 * tyvar.  Such a receiver arises inside a constrained generic instance body,
 * where the container's element is the constraint var; the helper's `:A` return
 * collapses to the int64 carrier (TY_INT) at elaboration, so without this it
 * would match a fixed `int` instance (silent mis-dispatch) or, with no `int`
 * instance, report TUR_E0020.  Treating it as an abstract-tyvar receiver routes
 * it to the carrier representative + dict tagging, and emit-side re-resolution
 * (emit_reresolve_disp_type) specializes the element call per ABI specialization
 * -- the same outcome the documented `(:: e A)` ascription idiom already gets. */
static bool obj_is_unascribed_carrier_elem(const Expr *obj) {
    while (obj && obj->kind == EX_ASCRIBE) obj = obj->as.ascribe_.inner;
    if (!obj || obj->kind != EX_CALL || !obj->as.call_.fn_binding ||
        !obj->as.call_.args)
        return false;
    const Type *ft = &obj->as.call_.fn_binding->type;
    if (ft->kind != TY_FN || !ft->as.fn.arg_full_types ||
        !ft->as.fn.result_full_type ||
        ft->as.fn.result_full_type->kind != TY_TYVAR ||
        !ft->as.fn.result_full_type->as.tyvar_.name)
        return false;
    const char *rname = ft->as.fn.result_full_type->as.tyvar_.name;
    uint32_t np = ft->as.fn.arity;
    for (uint32_t pi = 0; pi < np && pi < obj->as.call_.n_args; pi++) {
        const Type *pft = ft->as.fn.arg_full_types[pi];
        const Expr *ae = obj->as.call_.args[pi];
        if (!pft || !ae) continue;
        Type extracted;
        if (tc_pattern_extract_var(pft, &ae->type, rname, &extracted))
            return extracted.kind == TY_TYVAR;
    }
    return false;
}


/* erased-float-carrier: stamp a poly-wrap headed for typeclass-method param
 * `param` with the positions of its DECLARED `:fn` signature that are type
 * variables.  The instance body is compiled once for every `a`/`b`, so it
 * invokes such a param through the int64 carrier cast; the emitter routes a
 * float-class wrapper through a bits shim at exactly those positions (see
 * ensure_float_carrier_shim).  Concrete positions keep the native thunk ABI
 * the typed poly-to-fat shim and the F5 typed `:fn` cast already rely on. */
static void poly_wrap_stamp_carrier_erased(Expr *wrap, const Binding *param) {
    const Type *pt = param ? param->poly_type : NULL;
    if (pt && pt->kind == TY_FORALL) pt = pt->as.forall_.body;
    if (!pt || pt->kind != TY_FN) return;
    uint64_t mask = 0;
    for (uint8_t i = 0; i < pt->as.fn.arity; i++) {
        const Type *a = pt->as.fn.arg_full_types ? pt->as.fn.arg_full_types[i] : NULL;
        bool erased = a ? (a->kind == TY_TYVAR) : (pt->as.fn.arg_kinds[i] == TY_TYVAR);
        if (erased) mask |= ARG_IDX_BIT(i);
    }
    const Type *r = pt->as.fn.result_full_type;
    bool res_erased = r ? (r->kind == TY_TYVAR) : (pt->as.fn.result_kind == TY_TYVAR);
    wrap->as.poly_wrap_.carrier_erased_arg_mask = mask;
    wrap->as.poly_wrap_.carrier_erased_result = res_erased;
    if (r && r->kind == TY_APP) {
        const Type *h = r;
        while (h && h->kind == TY_APP) h = h->as.app.fn;
        wrap->as.poly_wrap_.carrier_erased_result_hkt = h && h->kind == TY_TYVAR;
    }
}

/* same-method-name-in-two-classes-dispatches-by-declaration-order: are these
 * the same typeclass?  Identity is the usual answer, but one `defclass` seen
 * through two import paths registers twice, so a name match counts too -- the
 * same equivalence the TY_TYVAR constraint check above already uses.  Without
 * the name arm, a doubly-registered class would report itself as ambiguous
 * with itself. */
static bool typeclass_same_class(const TypeClass *a, const TypeClass *b) {
    if (a == b) return true;
    if (!a || !b || !a->name || !b->name) return false;
    return a->name->len == b->name->len &&
           memcmp(a->name->name, b->name->name, a->name->len) == 0;
}

/* S9: does a class method's declared type mention a type variable anywhere --
 * the class variable itself, or one applied (`(Option a)`)?  Such a result is
 * a different type per instance, so no one signature can carry it. */
static bool saffron_type_mentions_tyvar(const Type *t) {
    if (!t) return false;
    switch (t->kind) {
        case TY_TYVAR: return true;
        case TY_APP:
            return saffron_type_mentions_tyvar(t->as.app.fn) ||
                   saffron_type_mentions_tyvar(t->as.app.arg);
        case TY_FN:
            if (saffron_type_mentions_tyvar(t->as.fn.result_full_type)) return true;
            if (t->as.fn.arg_full_types)
                for (uint32_t i = 0; i < t->as.fn.arity; i++)
                    if (saffron_type_mentions_tyvar(t->as.fn.arg_full_types[i])) return true;
            return false;
        default: return false;
    }
}

/* S9 (D8 Q1): is extra parameter `j` of a kind-* instance on the parametric
 * head `def` the class variable -- i.e. another value of the receiver's own
 * type?  Three spellings mean yes: a bare type variable, the head itself
 * (`y : (MutableMap K V)`), and the erased `int` an UNANNOTATED parameter
 * records in both the class and the impl (`Eq`'s `(eq? [x y])`). */
static bool saffron_extra_is_class_var(const TypeClass *tc, uint8_t slot,
                                       const FnDef *impl, uint32_t j,
                                       const AdtDef *def) {
    const Type *pt = &impl->param_types[j];
    if (pt->kind == TY_TYVAR) return true;
    const Type *h = pt;
    while (h && h->kind == TY_APP && h->as.app.fn) h = h->as.app.fn;
    if (h && h->kind == TY_ADT && h->as.adt_.def == def) return true;
    /* An `int` on both sides is how an unannotated parameter records (`Eq`'s
     * `(eq? [x y])`), so it is guessed to be the class variable -- but only
     * when the CLASS left it bare.  A spelled `n : int` is an int
     * (saffron-dyn-parametric-extra-read-as-class-var: `(nth-of [x n : int])`
     * cast its index to `(Vec any)` and panicked). */
    if (tc->methods[slot].param_explicit_type &&
        j < tc->methods[slot].n_params &&
        tc->methods[slot].param_explicit_type[j])
        return false;
    return pt->kind == TY_INT && tc->methods[slot].param_types &&
           j < tc->methods[slot].n_params &&
           tc->methods[slot].param_types[j].kind == TY_INT;
}

/* saffron-lang-plan S9 (D8 Q3) / saffron-dynamic-surface-pass M9: mint the
 * dynamic-dispatch WITNESS for (instance `wi`, method `slot`) -- an ordinary
 * defn, elaborated at global scope in the Saffron span:
 *
 *   (defn __dynwit_<Class>_<method>_<Recv> [__r : <RecvTy> __a1 __a2 ...] : any
 *     (.<method> __r __a1 __a2 ...))
 *
 * `__r` is the receiver at the type the registry row is keyed on (`recv_ty`,
 * the all-`any` instantiation of a parametric head, or the ground receiver of
 * a kind-* instance); every extra is `any` by the dialect default, so the D5
 * seam inserts the checked unbox to the impl's parameter type; the `: any`
 * return re-tags the result.  The emitter (emit_instance_dyn_table) writes a
 * two-line C shim that unboxes the receiver word and calls this.  `def` is the
 * parametric head's AdtDef (used for the applied-result continuation adaptor,
 * M1) or NULL for a ground receiver.  Memoised per (instance, slot). */
static void saffron_mint_dyn_witness(Elab *e, TypeClass *tc, uint8_t slot,
                                     TypeClassInstance *wi, AdtDef *def,
                                     Form *recv_ty, const char *recv_name,
                                     const char *method_name, size_t method_name_len,
                                     Span sp, Form *ret_ty) {
    if (!wi->dyn_witness) {
        wi->dyn_witness = (FnDef **)arena_alloc(
            e->arena, tc->n_methods * sizeof(FnDef *));
        memset(wi->dyn_witness, 0, tc->n_methods * sizeof(FnDef *));
    }
    if (wi->dyn_witness[slot]) return;
    /* `(Head any ... any)` -- the all-`any` instantiation, built fresh at every
     * use because a Form is not shared between two positions in one tree. */
    #define SAFFRON_ALL_ANY_APP(dst)                        \
        Form *dst;                                          \
        {   uint32_t __ntp = def->n_type_params;            \
            Form **__ti = (Form **)arena_alloc(             \
                e->arena, (1 + __ntp) * sizeof(Form *));    \
            __ti[0] = form_sym(e->arena, sp,                \
                symtab_intern(e->st, strslice(              \
                    def->name,                              \
                    (uint32_t)strlen(def->name))));         \
            for (uint32_t __k = 0; __k < __ntp; __k++)      \
                __ti[1 + __k] = form_sym(e->arena, sp,      \
                    symtab_intern(e->st,                    \
                        strslice("any", 3)));               \
            dst = form_list(e->arena, sp, __ti, 1 + __ntp); \
        }
        char wn[256];
        snprintf(wn, sizeof wn, "__dynwit_%s_%.*s_%s", tc->name->name,
                 (int)method_name_len, method_name, recv_name);
        const Symbol *wsym  = symtab_intern(e->st, strslice(wn, (uint32_t)strlen(wn)));
        const Symbol *anys  = symtab_intern(e->st, strslice("any", 3));
        const Symbol *defns = symtab_intern(e->st, strslice("defn", 4));
        char dm[160];
        snprintf(dm, sizeof dm, ".%.*s", (int)method_name_len, method_name);
        const Symbol *dots = symtab_intern(e->st, strslice(dm, (uint32_t)strlen(dm)));
        /* [__r : (Head any..) __a1 __a2 ...] -- class arity, not
         * this call's, so the witness matches the declaration. */
        uint32_t n_wextra = tc->methods[slot].n_params > 0
                              ? tc->methods[slot].n_params - 1 : 0;
        /* The reader spells `x : T` as `x` followed by an
         * F_TYPE_ANN wrapping T (and `: any` after the params
         * the same way); a bare `:` symbol is the legacy GADT
         * ctor spelling and elab_defn reads `any` after it as an
         * EXPRESSION ("unbound symbol 'any'"). */
        Form **pv = (Form **)arena_alloc(e->arena, (2 + n_wextra) * sizeof(Form *));
        const Symbol *rsym = symtab_intern(e->st, strslice("__r", 3));
        pv[0] = form_sym(e->arena, sp, rsym);
        pv[1] = form_type_ann(e->arena, sp, recv_ty);
        Form **cargs = (Form **)arena_alloc(e->arena, (2 + n_wextra) * sizeof(Form *));
        cargs[0] = form_sym(e->arena, sp, dots);
        cargs[1] = form_sym(e->arena, sp, rsym);
        /* An extra whose IMPL parameter is the erased fn carrier
         * (`ptr-void` -- how an inferred `(g v)` records `g`,
         * and what an explicit `: fn` lowers to) is passed as
         * `(cast __ak (fn [any] any))`, the one shape that
         * reaches the spec as a fat closure rather than as a
         * local the poly wrapper would try to call BY NAME from
         * file scope.  A Saffron lambda IS a `(fn [any] any)`,
         * so the checked cast admits exactly the closures the
         * spec can apply, and a closure of another arity panics
         * at the cast instead of being called wrongly.  The
         * impl never records the fn's arity, but the CLASS does
         * when it spells the parameter -- Foldable's `fn : (fn
         * [b a] b)` is binary -- so the cast takes that arity,
         * `(fn [any any] any)`.  A Saffron class never reaches
         * this arm with an arity-less `g : fn`: there the
         * parameter is an `any` called dynamically
         * (tc_fn_param_type).  Only a TYPED class's unannotated
         * parameter leaves the arity unknown, taken as unary. */
        FnDef *wimpl = wi->method_impls[slot];
        bool tc_hkt = false;
        if (tc->type_param_kinds)
            for (uint8_t ki = 0; ki < tc->n_type_params; ki++)
                if (tc->type_param_kinds[ki] != KIND_STAR) { tc_hkt = true; break; }
        const Symbol *casts = symtab_intern(e->st, strslice("cast", 4));
        const Symbol *fns   = symtab_intern(e->st, strslice("fn", 2));
        for (uint32_t k = 0; k < n_wextra; k++) {
            char an[24]; snprintf(an, sizeof an, "__a%u", k + 1);
            const Symbol *as = symtab_intern(e->st, strslice(an, (uint32_t)strlen(an)));
            pv[2 + k] = form_sym(e->arena, sp, as);
            bool erased_fn = wimpl && wimpl->binding &&
                wimpl->binding->type.kind == TY_FN &&
                (k + 1) < wimpl->binding->type.as.fn.arity &&
                wimpl->binding->type.as.fn.arg_kinds[k + 1] == TY_PTR_VOID;
            /* saffron-dynamic-surface-pass M1: a continuation
             * whose declared RESULT is the class variable
             * APPLIED -- Monad's `k : (fn [a] (m b))`, against
             * Functor's `g : (fn [a] b)` -- must not be cast to
             * `(fn [any] any)`.
             *
             * With that cast the method's result instantiation
             * never grounds (`(m b)` cannot unify with a bare
             * `any`), so `.bind` resolves to the ERASED CARRIER
             * `__inst_Monad_bind_Option(int64_t, tur_poly_fn_t)`
             * -- whose body calls the continuation through
             * `((int64_t (*)(void*, int64_t))k.fn)` while a
             * Saffron lambda returns a 16-byte `tur_tagged_t`,
             * dropping half the box on the return -- and the
             * `: any` pin then tags an unresolved result with a
             * bare TypeKind number, which is why `type-of` read
             * `unknown`.
             *
             * Pass a MARSHALLING adaptor instead, the same move
             * H7 makes at the argument seam:
             *
             *   (fn [__ka : any] : (Head any..)
             *     (cast (__ak __ka) (Head any..)))
             *
             * Now `k` really is `(fn [any] (Head any..))`, so
             * `b := any`, the instantiation is concrete, the
             * ABI scan mints the by-value spec, and the result
             * carries the right tag. */
            bool app_result_fn = false;
            if (erased_fn && slot < tc->n_methods &&
                tc->methods[slot].param_types) {
                const Type *mp = &tc->methods[slot].param_types[k + 1];
                if (mp->kind == TY_FN && mp->as.fn.result_full_type &&
                    mp->as.fn.result_full_type->kind == TY_APP)
                    app_result_fn = true;
            }
            if (app_result_fn && def) {
                SAFFRON_ALL_ANY_APP(ret_ty)
                SAFFRON_ALL_ANY_APP(cast_ty)
                char kn[24];
                snprintf(kn, sizeof kn, "__ka%u", k);
                const Symbol *ks = symtab_intern(
                    e->st, strslice(kn, (uint32_t)strlen(kn)));
                Form *inner[2] = { form_sym(e->arena, sp, as),
                                   form_sym(e->arena, sp, ks) };
                Form *cst2[3] = { form_sym(e->arena, sp, casts),
                                  form_list(e->arena, sp, inner, 2),
                                  cast_ty };
                Form *pv2[2] = { form_sym(e->arena, sp, ks),
                                 form_type_ann(e->arena, sp,
                                     form_sym(e->arena, sp, anys)) };
                Form *lam[4] = { form_sym(e->arena, sp, fns),
                                 form_vec(e->arena, sp, pv2, 2),
                                 form_type_ann(e->arena, sp, ret_ty),
                                 form_list(e->arena, sp, cst2, 3) };
                cargs[2 + k] = form_list(e->arena, sp, lam, 4);
            } else if (erased_fn) {
                uint32_t fn_arity = 1;
                if (slot < tc->n_methods && tc->methods[slot].param_types &&
                    (k + 1) < tc->methods[slot].n_params) {
                    const Type *mp = &tc->methods[slot].param_types[k + 1];
                    if (mp->kind == TY_FN && mp->as.fn.arity > 0)
                        fn_arity = mp->as.fn.arity;
                }
                Form **pany = (Form **)arena_alloc(e->arena, fn_arity * sizeof(Form *));
                for (uint32_t pa = 0; pa < fn_arity; pa++)
                    pany[pa] = form_sym(e->arena, sp, anys);
                Form *fnt[3] = { form_sym(e->arena, sp, fns),
                                 form_vec(e->arena, sp, pany, fn_arity),
                                 form_sym(e->arena, sp, anys) };
                Form *cst[3] = { form_sym(e->arena, sp, casts),
                                 form_sym(e->arena, sp, as),
                                 form_list(e->arena, sp, fnt, 3) };
                cargs[2 + k] = form_list(e->arena, sp, cst, 3);
            } else if (def && !tc_hkt && slot < tc->n_methods && wimpl &&
                       wimpl->param_types && (k + 1) < wimpl->n_params &&
                       saffron_extra_is_class_var(tc, slot, wimpl, k + 1, def)) {
                /* S9 (D8 Q1) -- M9 for a PARAMETRIC head (`Eq [Vec]`'s
                 * `(eq? [x y])`).  The class variable is `(Vec A)` here, and
                 * an unannotated parameter records as the erased `int` in both
                 * the class and the impl -- which is exactly the slot a typed
                 * `(.eq? v w)` hands a `(Vec any)` value to.  So cast the box
                 * to the all-`any` instantiation, as that typed call would
                 * spell it.  A genuinely int-typed extra on such a head is
                 * indistinguishable here; it panics at the cast rather than
                 * being misread. */
                SAFFRON_ALL_ANY_APP(cast_ty)
                Form *cst[3] = { form_sym(e->arena, sp, casts),
                                 form_sym(e->arena, sp, as), cast_ty };
                cargs[2 + k] = form_list(e->arena, sp, cst, 3);
            } else if (slot < tc->n_methods && tc->methods[slot].param_types) {
                /* Also a PARAMETRIC head's extra that is not the class
                 * variable (saffron-dyn-parametric-extra-read-as-class-var:
                 * `Nth [Vec]`'s spelled `n : int`).  saffron_extra_is_class_var
                 * has already claimed every tyvar-typed and head-typed impl
                 * parameter, so only a concrete one reaches here. */
                /* M9 (kind-* witness): the static `.m __r __a1` inside the
                 * witness resolves to ONE instance, whose impl declares the
                 * extra at a concrete type -- the class variable IS the
                 * receiver type here, `y : a` with `a := int` -- and the
                 * method-dispatch path does not run the D5 argument seam, so
                 * an `any` extra reached the impl unconverted (cc: incompatible
                 * type for argument 2).  Spell the checked cast in the witness
                 * itself: a class-variable extra casts to the receiver type, a
                 * primitive-typed extra to that primitive, anything else
                 * (another tyvar, a compound type) stays bare.  A mismatched
                 * box panics at the cast, as at every other seam. */
                /* The cast target is the IMPL's parameter type, which is
                 * concrete per instance (`y : float` in `Near [float]`, `y :
                 * Pt` in `Near [Pt]`).  The class declaration is no use here:
                 * an unannotated class parameter (`(near? [x y] : bool)`) is
                 * recorded as `int`, not as the class variable -- measured:
                 * every witness cast to the int tag, and the float and Pt
                 * instances' impls were handed an int64 word.  A class
                 * variable in the impl's own signature (a generic impl) still
                 * means the receiver type. */
                const Type *mp = &tc->methods[slot].param_types[k + 1];
                if (wimpl && wimpl->param_types && (k + 1) < wimpl->n_params)
                    mp = &wimpl->param_types[k + 1];
                const char *cast_name = NULL;
                /* A class-variable extra means the receiver type only for a
                 * kind-* head; on a parametric head the receiver's name is a
                 * constructor, not a type, and a tyvar extra there (Foldable's
                 * `init : b`) stays bare. */
                if (mp->kind == TY_TYVAR) cast_name = def ? NULL : recv_name;
                else switch (mp->kind) {
                    case TY_INT: case TY_FLOAT: case TY_BOOL: case TY_CSTR: case TY_SYM:
                    case TY_INT8: case TY_INT16: case TY_INT32: case TY_INT64:
                    case TY_UINT8: case TY_UINT16: case TY_UINT32: case TY_UINT64:
                    case TY_FLOAT32: case TY_FLOAT64:
                        cast_name = type_name(*mp); break;
                    case TY_ADT:
                        if (mp->as.adt_.def && mp->as.adt_.def->n_type_params == 0)
                            cast_name = mp->as.adt_.def->name;
                        break;
                    default: break;
                }
                if (cast_name) {
                    Form *cst[3] = { form_sym(e->arena, sp, casts),
                                     form_sym(e->arena, sp, as),
                                     form_sym(e->arena, sp, symtab_intern(e->st,
                                         strslice(cast_name, (uint32_t)strlen(cast_name)))) };
                    cargs[2 + k] = form_list(e->arena, sp, cst, 3);
                } else {
                    cargs[2 + k] = form_sym(e->arena, sp, as);
                }
            } else {
                cargs[2 + k] = form_sym(e->arena, sp, as);
            }
        }
        Form *params = form_vec(e->arena, sp, pv, 2 + n_wextra);
        Form *body   = form_list(e->arena, sp, cargs, 2 + n_wextra);
        /* The witness returns `any` -- or, for a one-parameter method with a
         * concrete declared result (S9's constrained parametric head), that
         * result, so its slot keeps the signature every other instance's
         * direct shim has. */
        Form *di[5] = { form_sym(e->arena, sp, defns), form_sym(e->arena, sp, wsym),
                        params,
                        form_type_ann(e->arena, sp,
                                      ret_ty ? ret_ty : form_sym(e->arena, sp, anys)),
                        body };
        Form *dform = form_list(e->arena, sp, di, 5);
        Scope *saved = e->scope;
        e->scope = &e->global;
        Expr *wdef = elab_defn(e, dform);
        e->scope = saved;
        if (wdef && wdef->kind == EX_FN_DEF && wdef->as.fn_def_.fn) {
            elab_register_file_def(e, wdef);
            wi->dyn_witness[slot] = wdef->as.fn_def_.fn;
        }
    #undef SAFFRON_ALL_ANY_APP
}

/* saffron-lang-plan S9 (D8 Q1): the dynamic dictionary for `A = any`.
 *
 * A constrained instance -- `(definstance Eq [Vec] [(Eq A)] ...)` -- discharges
 * its constraint at the element type, and the element type of every container
 * a dynamic dialect builds is `any` (S6).  With no `Eq [any]` instance that
 * discharge failed, and it failed three different ways depending on route:
 *
 *   - statically, the constrained instance was silently skipped as unsatisfied
 *     and the scalar leftovers were reported as an AMBIGUOUS dispatch
 *     (TUR-E0020, "receiver type is erased") on a receiver that is a perfectly
 *     concrete `(Vec any)`;
 *   - dynamically, compiled: the instance's spec at `A = any` re-resolved the
 *     element call to nothing and kept the elaborator's carrier representative
 *     -- `Kind [int]` -- so every element answered as an int.  A silent wrong
 *     answer;
 *   - dynamically, interpreted: no pin for `A`, so the same representative.
 *
 * All three want the same object, and it is the one the plan named: a
 * synthesised `C [any]` whose every method dispatches on the receiver box's
 * tag.  So mint it, as an ordinary definstance elaborated in the dynamic span
 * that needed it:
 *
 *   (definstance C [any]
 *     (m1 [__x __a1 ...] (.m1 __x __a1 ...))
 *     ...)
 *
 * Unannotated, so the dialect default makes every parameter `any` and
 * definstance takes the class's declared result (class variable := any).  The
 * body's `.m1` on an `any` receiver becomes EX_DYN_METHOD because the instance
 * is stamped `dyn_any_minted`, which the any-receiver arm treats as absent --
 * otherwise the body would resolve statically to itself.  Every consumer then
 * finds a real instance where it looks for one: the static constraint check,
 * emit_reresolve_method_call at the `A = any` spec, and the interpreter's
 * frame dictionaries.
 *
 * Kind-* single-parameter classes only: `any` is not a type constructor, so an
 * HKT class has no `[any]` head to mint.  A class that already has an `any`
 * instance (Hash, MapKey -- hand-written, and authoritative) is left alone. */
static void saffron_mint_dyn_any_instance(Elab *e, TypeClass *tc, Span sp) {
    if (!tc || !tc->name || tc->n_methods == 0 || tc->n_type_params != 1) return;
    if (tc->type_param_kinds && tc->type_param_kinds[0] != KIND_STAR) return;
    for (TypeClassInstance *i = e->typeclass_env.instances; i; i = i->next)
        if (typeclass_same_class(i->typeclass, tc) && i->n_type_args > 0 &&
            i->type_args[0].kind == TY_ANY)
            return;
    const Symbol *xs = symtab_intern(e->st, strslice("__x", 3));
    Form **items = (Form **)arena_alloc(e->arena,
                                        (3 + tc->n_methods) * sizeof(Form *));
    items[0] = form_sym(e->arena, sp,
                        symtab_intern(e->st, strslice("definstance", 11)));
    items[1] = form_sym(e->arena, sp, tc->name);
    Form *anyf = form_sym(e->arena, sp, symtab_intern(e->st, strslice("any", 3)));
    items[2] = form_vec(e->arena, sp, &anyf, 1);
    for (uint8_t mi = 0; mi < tc->n_methods; mi++) {
        const TypeClassMethod *m = &tc->methods[mi];
        uint32_t np = m->n_params > 0 ? m->n_params : 1;
        Form **pv = (Form **)arena_alloc(e->arena, np * sizeof(Form *));
        Form **cv = (Form **)arena_alloc(e->arena, (1 + np) * sizeof(Form *));
        char dm[160];
        snprintf(dm, sizeof dm, ".%s", m->name->name);
        cv[0] = form_sym(e->arena, sp,
                         symtab_intern(e->st, strslice(dm, (uint32_t)strlen(dm))));
        pv[0] = form_sym(e->arena, sp, xs);
        cv[1] = form_sym(e->arena, sp, xs);
        for (uint32_t k = 1; k < np; k++) {
            char an[24];
            snprintf(an, sizeof an, "__a%u", k);
            const Symbol *as = symtab_intern(e->st,
                                             strslice(an, (uint32_t)strlen(an)));
            pv[k] = form_sym(e->arena, sp, as);
            cv[1 + k] = form_sym(e->arena, sp, as);
        }
        Form *mbody = form_list(e->arena, sp, cv, 1 + np);
        /* A method with extras dispatches through a witness, which answers
         * `any`; narrow it back to the class's declared concrete result
         * (`eq?`'s bool) with the checked cast, since that is the signature
         * every caller of this instance was typed against. */
        if (np > 1 && m->return_type.kind != TY_TYVAR &&
            m->return_type.kind != TY_ANY && m->return_type.kind != TY_UNKNOWN &&
            m->return_type.kind != TY_NIL) {
            Form *rf = NULL;
            switch (m->return_type.kind) {
                case TY_INT: case TY_FLOAT: case TY_BOOL: case TY_CSTR: case TY_SYM: {
                    const char *pn = type_name(m->return_type);
                    rf = form_sym(e->arena, sp,
                                  symtab_intern(e->st, strslice(pn, (uint32_t)strlen(pn))));
                    break;
                }
                default:
                    rf = type_to_form(e, &m->return_type, sp);
                    break;
            }
            if (rf) {
                Form *cst[3] = { form_sym(e->arena, sp,
                                          symtab_intern(e->st, strslice("cast", 4))),
                                 mbody, rf };
                mbody = form_list(e->arena, sp, cst, 3);
            }
        }
        Form *mf[3] = { form_sym(e->arena, sp, m->name),
                        form_vec(e->arena, sp, pv, np),
                        mbody };
        items[3 + mi] = form_list(e->arena, sp, mf, 3);
    }
    Form *di = form_list(e->arena, sp, items, 3 + tc->n_methods);
    /* Nesting is expected and terminates: a minted body's `.m` reaches the
     * dispatch site below, which may mint ANOTHER class's dictionary, and the
     * instance is registered before its methods elaborate, so the existence
     * check above stops a class from being minted twice. */
    Scope *saved_scope = e->scope;
    Type *saved_expected = e->expected_type;
    bool saved_minting = e->minting_dyn_any;
    e->scope = &e->global;
    e->expected_type = NULL;
    e->minting_dyn_any = true;
    (void)elab_definstance(e, di);
    e->minting_dyn_any = saved_minting;
    e->scope = saved_scope;
    e->expected_type = saved_expected;
}

/* S9 (D8 Q1): mint the `[any]` dictionary for every class a constrained
 * instance of `tc` discharges -- the dictionaries its body will ask for when
 * it runs at the all-`any` instantiation. */
static void saffron_mint_constraint_any_instances(Elab *e, const TypeClassInstance *inst,
                                                  Span sp) {
    if (!inst || !inst->type_param_constraints) return;
    for (uint8_t ci = 0; ci < inst->n_type_param_constraints; ci++)
        saffron_mint_dyn_any_instance(e, inst->type_param_constraints[ci].typeclass, sp);
}

Expr *elab_method_call(Elab *e, const Form *call) {

    /* call is (.method obj arg1 arg2 ...)
     * call->as.list.items[0] is the symbol .method
     */
    if (call->as.list.len < 2) {
        diag_emit(DIAG_ERROR, call->span,
                  "method call requires (.method obj arg1 ...)");
        return NULL;
    }

    /* Clojure-style receiver-first sugar: `(. obj field args...)` where the
     * head is the bare `.` symbol (length 1) and the field name is a separate
     * symbol.  Desugar to `(.field obj args...)` so the joined-form paths below
     * (struct field access, function-typed field call-through, typeclass
     * dispatch) all apply uniformly.  Without this rewrite the head `.` yields
     * an empty method name and dispatch fails with "no typeclass method found
     * for ''" (docs/archive/history/dot-method-call-misroutes-to-typeclass.md). */
    if (call->as.list.items[0]->tag == F_SYM &&
        call->as.list.items[0]->as.sym->len == 1 &&
        call->as.list.items[0]->as.sym->name[0] == '.') {
        if (call->as.list.len < 3 || call->as.list.items[2]->tag != F_SYM) {
            diag_emit(DIAG_ERROR, call->span,
                      "(. obj field args...) requires a field-name symbol, "
                      "e.g. (. p age) or (. lens get s)");
            return NULL;
        }
        const Symbol *field_sym = call->as.list.items[2]->as.sym;
        char dotbuf[160];
        int dotlen = snprintf(dotbuf, sizeof(dotbuf), ".%.*s",
                              (int)field_sym->len, field_sym->name);
        if (dotlen <= 0 || (size_t)dotlen >= sizeof(dotbuf)) {
            diag_emit(DIAG_ERROR, call->span,
                      "(. obj field ...) field name too long");
            return NULL;
        }
        const Symbol *dot_sym =
            symtab_intern(e->st, strslice(dotbuf, (uint32_t)dotlen));
        /* New list: [.field, obj, args...] = [.field, items[1], items[3..]]. */
        uint32_t n_items = call->as.list.len - 1;
        Form **items = (Form **)arena_alloc(e->arena, n_items * sizeof(Form *));
        items[0] = form_sym(e->arena, call->as.list.items[2]->span, dot_sym);
        items[1] = call->as.list.items[1];
        for (uint32_t i = 3; i < call->as.list.len; i++)
            items[i - 1] = call->as.list.items[i];
        Form *dotcall = form_list(e->arena, call->span, items, n_items);
        return elab_method_call(e, dotcall);
    }

    /* Parse method name from the symbol (skip the leading '.') */
    Form *head = call->as.list.items[0];
    const Symbol *method_sym = head->as.sym;
    const char *method_name = method_sym->name + 1;  /* Skip '.' */
    uint32_t method_name_len = method_sym->len - 1;

    /* A bare `.` head (now handled by the receiver-first desugaring above)
     * leaves an empty method name; any other route here with an empty name is
     * a malformed call.  Reject it with a clear message rather than letting the
     * empty name reach the "no typeclass method found for ''" diagnostic. */
    if (method_name_len == 0) {
        diag_emit(DIAG_ERROR, call->span,
                  "method call requires a field/method name after '.', "
                  "e.g. (.field obj) or (. obj field)");
        return NULL;
    }

    /* Phase D1: Type witness @TypeName at call sites.
     * The reader converts @TypeName into (deref TypeName), so we detect
     * the pattern (deref sym) at items[1] where sym names a registered
     * typeclass instance type argument for this method.  When found the
     * witness pins the dispatch directly to that instance; the receiver
     * is items[2] and extra arguments start at items[3]. */
    if (call->as.list.len >= 3 &&
        call->as.list.items[1]->tag == F_LIST &&
        call->as.list.items[1]->as.list.len == 2 &&
        call->as.list.items[1]->as.list.items[0]->tag == F_SYM &&
        strcmp(call->as.list.items[1]->as.list.items[0]->as.sym->name, "deref") == 0 &&
        call->as.list.items[1]->as.list.items[1]->tag == F_SYM) {

        const Symbol *witness_sym = call->as.list.items[1]->as.list.items[1]->as.sym;
        const char   *witness_name = witness_sym->name;
        uint32_t      witness_len  = witness_sym->len;

        /* Walk all registered instances looking for one whose type arg symbol
         * (or primitive type name) matches the witness identifier. */
        TypeClassInstance *witness_inst   = NULL;
        FnDef             *witness_method_fn = NULL;
        bool               any_inst_for_method = false;
        /* typeclass-dispatch-on-any-receiver-emits-uncompilable-c: the type the
         * witness pins, kept so an `any` receiver can be unboxed to it below. */
        Type               witness_recv_type = type_simple(TY_UNKNOWN, CK_COPY);

        for (TypeClassInstance *inst = e->typeclass_env.instances;
             inst != NULL && !witness_inst; inst = inst->next) {
            for (uint8_t mi = 0; mi < inst->typeclass->n_methods; mi++) {
                const TypeClassMethod *m = &inst->typeclass->methods[mi];
                if (m->name->len != method_name_len ||
                    memcmp(m->name->name, method_name, method_name_len) != 0) continue;
                any_inst_for_method = true;
                /* Check if a type arg name matches the witness identifier. */
                bool name_match = false;
                uint8_t matched_ti = 0;
                for (uint8_t ti = 0; ti < inst->n_type_args && !name_match; ti++) {
                    if (inst->type_arg_syms && inst->type_arg_syms[ti] &&
                        inst->type_arg_syms[ti]->len == witness_len &&
                        memcmp(inst->type_arg_syms[ti]->name, witness_name, witness_len) == 0) {
                        name_match = true;
                    }
                    if (!name_match) {
                        /* Primitive type names that have no symbol (e.g. int, bool). */
                        const char *prim = NULL;
                        switch (inst->type_args[ti].kind) {
                            case TY_INT:   prim = "int";   break;
                            case TY_BOOL:  prim = "bool";  break;
                            case TY_CSTR:  prim = "cstr";  break;
                            case TY_NIL:   prim = "nil";   break;
                            case TY_FLOAT: prim = "float"; break;
                            default: break;
                        }
                        if (prim && strcmp(prim, witness_name) == 0) name_match = true;
                    }
                    if (name_match) matched_ti = ti;
                }
                if (name_match) {
                    witness_inst      = inst;
                    witness_method_fn = inst->method_impls[mi];
                    if (matched_ti < inst->n_type_args)
                        witness_recv_type = inst->type_args[matched_ti];
                }
                break; /* one method match per instance is enough */
            }
        }

        if (witness_inst) {
            /* Witness resolved: receiver is items[2], extra args are items[3..]. */
            Expr *obj_w = elab_form(e, call->as.list.items[2]);
            if (!obj_w) return NULL;

            /* typeclass-dispatch-on-any-receiver-emits-uncompilable-c: an `any`
             * receiver is a two-word `tur_tagged_t`, and the instance impl takes
             * the payload type.  Handing the box straight over emitted
             * "incompatible type for argument 1" from cc -- and this is the
             * route the ambiguity diagnostic RECOMMENDS for an erased receiver,
             * so following the compiler's own hint produced a build failure.
             *
             * The witness already names the instance, which is exactly what the
             * unbox needs, so pin and unbox together.  It is the CHECKED unbox
             * (the same node `(cast x T)` lowers to), so a witness that names
             * the wrong instance panics with `cast: any holds ...` rather than
             * reinterpreting the payload -- the witness pins which impl runs, it
             * does not get to assert what the box holds. */
            if (obj_w->type.kind == TY_ANY &&
                witness_recv_type.kind != TY_UNKNOWN &&
                witness_recv_type.kind != TY_ANY) {
                obj_w = elab_any_unbox_to(e, obj_w, witness_recv_type,
                                          call->as.list.items[2]->span);
                if (!obj_w) return NULL;
            }

            uint32_t n_args_w = call->as.list.len - 3;
            Expr **args_w = (Expr **)arena_alloc(e->arena, n_args_w * sizeof(Expr *));
            for (uint32_t i = 0; i < n_args_w; i++) {
                args_w[i] = elab_form(e, call->as.list.items[3 + i]);
                if (!args_w[i]) return NULL;
            }

            /* Determine result type from the method's binding.
             *
             * pinned-instance-dispatch-loses-an-opaque-return-type: this used
             * to rebuild the result from `result_kind` ALONE, which is lossless
             * only for a primitive -- `cstr` survives, but a `defopaque` (or any
             * nominal type) comes back as a bare TY_ADT with no def, and the
             * call site is then told it got `<adt>`, a type the user never
             * wrote.  The receiver-dispatch path carries the declaration
             * through, so `(enc @Cons xs)` demanded a hand-written
             * `(:: ... Buf)` that `(enc xs)` does not.  Prefer the declared
             * full type, substituting the instance's own type args for the
             * class's type parameters the way the method's elaboration does;
             * fall back to the kind when the result is still abstract after
             * that (a return-only-dispatch method whose result is the class
             * var takes the carrier, as before). */
            Type result_type_w;
            if (witness_method_fn->binding->type.kind == TY_FN) {
                result_type_w = type_from_kind(witness_method_fn->binding->type.as.fn.result_kind);
                const Type *rft_w = witness_method_fn->binding->type.as.fn.result_full_type;
                if (rft_w) {
                    Type rf_w = *rft_w;
                    if (witness_inst->typeclass && witness_inst->typeclass->type_params &&
                        witness_inst->type_args) {
                        rf_w = elab_subst_class_tyvars(
                            e->arena, rf_w,
                            witness_inst->typeclass->type_params,
                            witness_inst->typeclass->n_type_params,
                            witness_inst->type_args, witness_inst->n_type_args);
                    }
                    if (!tc_type_mentions_tyvar(&rf_w) && rf_w.kind != TY_UNKNOWN)
                        result_type_w = rf_w;
                }
            } else {
                result_type_w = witness_method_fn->body ? witness_method_fn->body->type : TYPE_INT;
                if (result_type_w.kind == TY_UNKNOWN || result_type_w.kind == TY_NIL)
                    result_type_w = TYPE_INT;
            }

            /* Build the EX_DICT node for the pinned instance. */
            Expr *dict_w = make_dict_expr(e, witness_inst, call->span);
            tur_mangle_ident(method_name, dict_w->as.dict_.method_name,
                             sizeof(dict_w->as.dict_.method_name));

            /* Build EX_CALL: args array is [obj_w, args_w...]. */
            Expr **call_args_w = (Expr **)arena_alloc(e->arena,
                                                       (n_args_w + 1) * sizeof(Expr *));
            call_args_w[0] = obj_w;
            for (uint32_t i = 0; i < n_args_w; i++) call_args_w[i + 1] = args_w[i];

            Expr *out_w = expr_new(e->arena, EX_CALL, result_type_w, call->span);
            /* Phase H §1: witness pins a specific instance statically, so emit
             * a direct call to the instance impl instead of dict-dispatching. */
            if (witness_method_fn && witness_method_fn->binding) {
                out_w->as.call_.fn_binding = witness_method_fn->binding;
                out_w->as.call_.fn_expr    = NULL;
            } else {
                out_w->as.call_.fn_binding = NULL;
                out_w->as.call_.fn_expr    = dict_w;
            }
            out_w->as.call_.args       = call_args_w;
            out_w->as.call_.n_args     = n_args_w + 1;
            out_w->as.call_.dict_arg   = dict_w;
            /* heap-struct-field-extraction-collapses-to-carrier: a `@TypeName`
             * witness call pins the instance but never recorded abi_bindings, so
             * emit_abi_register_call had nothing to specialize and the call fell
             * through to the int64-carrier base clone.  For a :heap container
             * (`(Cons int)`) that base clone derefs `(xs)->head` on the int64
             * carrier -- a hard C compile error -- and bakes the int element
             * instance for every A.  Mirror the receiver-dispatch path's M4c
             * Path A binding population (elab_method_call below, ~line 5627):
             * bind the class var to the witness receiver's concrete type and the
             * instance head's own tyvars (the constraint var `A`) to the
             * receiver's element types, so a per-instantiation by-value/heap spec
             * is minted and the inner `(enc (.head xs))` re-dispatches per
             * element.  HKT classes keep the uniform-carrier dispatch. */
            if (witness_inst && witness_inst->typeclass &&
                out_w->as.call_.fn_binding != NULL) {
                TypeClass *wtc = witness_inst->typeclass;
                bool w_is_hkt = false;
                if (wtc->type_param_kinds) {
                    for (uint8_t i = 0; i < wtc->n_type_params; i++)
                        if (wtc->type_param_kinds[i] != KIND_STAR) { w_is_hkt = true; break; }
                }
                bool recv_parametric =
                    obj_w->type.kind == TY_APP;
                if (!w_is_hkt && wtc->n_type_params == 1 && wtc->type_params[0] &&
                    recv_parametric) {
                    const Symbol *hb_names[ABI_TYPE_BINDINGS_MAX];
                    Type hb_types[ABI_TYPE_BINDINGS_MAX];
                    uint8_t hb_n = 0;
                    if (witness_inst->n_type_args >= 1)
                        m7_collect_tyvar_bindings(e, witness_inst->type_args[0],
                                                  obj_w->type, hb_names, hb_types,
                                                  &hb_n, ABI_TYPE_BINDINGS_MAX - 1);
                    uint8_t total = (uint8_t)(1 + hb_n);
                    AbiTypeBinding *bindings = (AbiTypeBinding *)arena_alloc(
                        e->arena, total * sizeof(AbiTypeBinding));
                    bindings[0].name = wtc->type_params[0]->name;
                    bindings[0].type = obj_w->type;
                    for (uint8_t k = 0; k < hb_n; k++) {
                        bindings[1 + k].name = hb_names[k]->name;
                        bindings[1 + k].type = hb_types[k];
                    }
                    out_w->as.call_.abi_bindings = bindings;
                    out_w->as.call_.n_abi_bindings = total;
                }
            }
            return out_w;

        } else if (any_inst_for_method) {
            /* Instances exist for this method but none match the witness type name.
             * The user clearly intended a witness (not a deref); emit a specific error. */
            diag_emit(DIAG_ERROR, call->as.list.items[1]->span,
                      "no instance of typeclass method '.%.*s' for type '%s' -- "
                      "check that (definstance ... [%s] ...) is in scope",
                      (int)method_name_len, method_name, witness_name, witness_name);
            return NULL;
        }
        /* No instances at all for this method: fall through to the normal path
         * so that (deref sym) is elaborated as the receiver and the existing
         * "no typeclass method found" error is emitted. */
    }

    /* Phase HKT H2: Elaborate the receiver object first so we can use its
     * type to select the correct typeclass instance (type-based dispatch).
     * This replaces the old "name-only / first-match" approach with a lookup
     * that distinguishes multiple instances of the same typeclass for
     * different types (e.g. MyShow[int] vs MyShow[bool]). */
    Expr *obj = elab_form(e, call->as.list.items[1]);
    if (!obj) return NULL;

    /* Existential-open witness dispatch.
     * When the receiver `v` was bound by `(open e [a v] ...)` over a
     * constraint-carrying existential, its static type is erased to the int64
     * carrier, so static instance search would either trivially pick the lone
     * instance (single in-scope) or fail as ambiguous (>=2 in scope).  Neither
     * consults the witness packed at the `pack` site.  Resolve instead through
     * the record's runtime witness vtable: locate the constraint class that
     * declares this method, find its witness slot, and build an
     * EX_EXISTS_DISPATCH node that the emitter lowers to an indirect call
     * through `record->witnesses[slot]`.  This is independent of how many
     * instances of the class are in scope. */
    if (obj->kind == EX_VAR && obj->as.var.binding
            && obj->as.var.binding->exists_open_type) {
        const Type *ex = obj->as.var.binding->exists_open_type;
        for (uint8_t ci = 0; ci < ex->as.forall_.n_constraints; ci++) {
            const TypeClass *tc = ex->as.forall_.constraint_classes[ci];
            if (!tc) continue;
            for (uint8_t mi = 0; mi < tc->n_methods; mi++) {
                if (tc->methods[mi].name->len != method_name_len ||
                    memcmp(tc->methods[mi].name->name, method_name,
                           method_name_len) != 0) {
                    continue;
                }
                /* Found the class+method.  Elaborate the receiver and any extra
                 * args (args[0] is the receiver `v`). */
                uint32_t n_args = call->as.list.len - 1;
                Expr **args = (Expr **)arena_alloc(e->arena,
                                                   n_args * sizeof(Expr *));
                args[0] = obj;
                bool args_ok = true;
                for (uint32_t i = 1; i < n_args; i++) {
                    args[i] = elab_form(e, call->as.list.items[1 + i]);
                    if (!args[i]) { args_ok = false; break; }
                }
                if (!args_ok) return NULL;

                Type result_type = tc->methods[mi].return_type;
                if (result_type.kind == TY_UNKNOWN ||
                    result_type.kind == TY_NIL ||
                    result_type.kind == TY_TYVAR) {
                    result_type = TYPE_INT;
                }

                Expr *out = expr_new(e->arena, EX_EXISTS_DISPATCH, result_type,
                                     call->span);
                out->as.exists_dispatch_.open_binding = obj->as.var.binding;
                out->as.exists_dispatch_.typeclass    = tc;
                out->as.exists_dispatch_.witness_idx  = ci;
                out->as.exists_dispatch_.method_idx   = mi;
                out->as.exists_dispatch_.args         = args;
                out->as.exists_dispatch_.n_args       = n_args;
                return out;
            }
        }
        /* No constraint class declares this method -- fall through to the
         * normal dispatch path (which will report no-method or an unrelated
         * struct-field access). */
    }

    /* Phase 12: EX_GET_FIELD — if the form is exactly (.field s) with no extra
     * args, try to resolve it as a struct field access first. */
    if (call->as.list.len == 2) {
        /* Unwrap borrow types */
        Type base = obj->type;
        const Type *field_owner_type = &obj->type;
        if (base.kind == TY_REF_IMMUT || base.kind == TY_REF_MUT) {
            base = type_from_kind(base.as.ref_borrow.target);
            field_owner_type = &base;
        }
        /* CONV-S1 (slice 2): auto-deref for an rc<ADT> receiver, so
         * `(.field rc-of-adt)` resolves through the record variant carried on
         * the rc type.  obj keeps its rc<ADT> type; codegen derefs the control
         * block's value pointer to reach the aggregate. */
        if (base.kind == TY_RC && base.as.rc.adt_def) {
            struct AdtDef *rc_adt = base.as.rc.adt_def;  /* read before union rewrite */
            base.kind = TY_ADT;
            base.as.adt_.def = rc_adt;
            field_owner_type = &base;
        }
        /* CONV-S0/S4: field access on a single-variant record ADT.  A
         * single-variant, non-GADT ADT whose sole constructor is record-style
         * is a product, so `(.field v)` / `(. v field)` reads the named field
         * directly -- the same surface a struct exposes (a struct *is* this
         * case).  Unwrap a parametric TY_APP head to its base ADT first. */
        {
            const Type *adt_base = field_owner_type;
            while (adt_base && adt_base->kind == TY_APP && adt_base->as.app.fn)
                adt_base = adt_base->as.app.fn;
            if (adt_base && adt_base->kind == TY_ADT && adt_base->as.adt_.def) {
                const AdtDef *adt = adt_base->as.adt_.def;
                /* CONV-S4N: the sole ctor of a single-variant record product,
                 * OR -- for a scrutinee a `match` arm has narrowed to one record
                 * variant -- the proven variant.  Reading `(.field s)` off a
                 * narrowed multi-variant scrutinee reads that variant's field out
                 * of the tagged union (the value's tag proves the variant), the
                 * same member path a match field-bind uses. */
                const CtorDef *ctor = NULL;
                /* A defdata that failed mid-elaboration (e.g. an unresolvable
                 * constructor field type) can leave n_ctors set with NULL
                 * ctors entries; guard so the error path degrades to "field
                 * not found" instead of a NULL-deref SIGSEGV. */
                if (adt_is_flat_product(adt) && adt->n_ctors == 1 &&
                    adt->ctors && adt->ctors[0] && adt->ctors[0]->is_record) {
                    ctor = adt->ctors[0];
                } else if (adt_is_narrowed_to_record_variant(*adt_base) &&
                           adt->ctors) {
                    ctor = adt->ctors[adt_base->as.adt_.narrowed_ctor_idx];
                }
                if (ctor) {
                    for (uint32_t i = 0; i < ctor->n_fields; i++) {
                        if (!ctor->fields[i].name) continue;
                        if (strcmp(ctor->fields[i].name, method_name) == 0) {
                            Type ftype;
                            if (ctor->fields[i].full_type) {
                                ftype = *ctor->fields[i].full_type;
                            } else if (ctor->fields[i].kind == TY_REF ||
                                       ctor->fields[i].kind == TY_LREF ||
                                       ctor->fields[i].kind == TY_RC ||
                                       ctor->fields[i].kind == TY_WEAK) {
                                /* linear-lref-struct-field: a pointer-kind field
                                 * (lref/ref/rc/weak) with no full_type still
                                 * carries its pointee in inner_kind.  Without it
                                 * `(.ptr b)` types as a bare lref<?> and `deref`
                                 * yields TY_UNKNOWN.  Mirror the struct path's
                                 * elab_struct_field_use_type, which threads the
                                 * inner kind onto the reconstructed type. */
                                ftype = type_from_kind(ctor->fields[i].kind);
                                if (ctor->fields[i].kind == TY_REF ||
                                    ctor->fields[i].kind == TY_LREF)
                                    ftype.as.ref.inner = ctor->fields[i].inner_kind;
                                else
                                    ftype.as.rc.inner = ctor->fields[i].inner_kind;
                            } else {
                                ftype = type_from_kind(ctor->fields[i].kind);
                            }
                            /* Parametric record ADT: substitute the receiver's
                             * concrete type args for the field's TY_TYVAR names so
                             * `(.val (Box 42))` reads as int, not the bare tyvar A
                             * (which fails overload resolution in untyped contexts
                             * like `(println (.val b))`).  Mirrors the match
                             * field-bind substitution (elab_structs.c) and the
                             * struct path's elab_struct_field_use_type. */
                            if (ctor->fields[i].full_type &&
                                    adt->n_type_params > 0 &&
                                    field_owner_type->kind == TY_APP) {
                                Type *type_args = (Type *)arena_alloc(e->arena,
                                    adt->n_type_params * sizeof(Type));
                                if (elab_adt_type_extract_args(field_owner_type,
                                                               adt, type_args))
                                    ftype = adt_field_instantiate_type(e, adt,
                                        ctor->fields[i].full_type, type_args);
                            }
                            /* Bare/unparameterized receiver (`r : Result`, a
                             * TY_ADT with no type-arg application) leaves a tyvar
                             * field type unbound; the value rides the int64
                             * carrier exactly as at default, so collapse the
                             * residual tyvar to the field's carrier kind (TY_INT).
                             * Without this `(.ok-val r)` types as a bare tyvar and
                             * fails overload resolution in an untyped context like
                             * `(println (.ok-val r))`.  Mirrors the struct path's
                             * elab_struct_field_use_type, which already collapses
                             * an unbound field tyvar.  Gated to the bare receiver:
                             * an APPLIED receiver `(Tuple2 A B)` inside a generic
                             * body keeps its field tyvar so the ABI spec can later
                             * resolve it to a concrete by-value aggregate (the
                             * accessor-unbox path). */
                            if (ftype.kind == TY_TYVAR &&
                                field_owner_type->kind != TY_APP)
                                ftype = type_from_kind(ctor->fields[i].kind);
                            Expr *out = expr_new(e->arena, EX_GET_FIELD, ftype,
                                                 call->span);
                            out->as.get_field_.struct_expr = obj;
                            out->as.get_field_.field_idx = i;
                            out->as.get_field_.adt_def = adt;
                            out->as.get_field_.adt_ctor = ctor;
                            /* effect-row-lost-through-a-constructor-argument:
                             * bind a control-bearing receiver out of the field
                             * read -- see elab_hoist_control_operands. */
                            return elab_hoist_control_operands(e, out);
                        }
                    }
                }
            }
        }
    }

    /* Phase 16 v2: capability field call — (.field-name cap arg1 arg2 ...)
     * When the receiver is a struct and the named field is :fn (TY_FN), and
     * there are arguments, emit an indirect function-pointer call through the
     * field. The call carries the EX_GET_FIELD as fn_expr for effect-row
     * propagation. Effect rows on the field are advisory in v1 (codegen erases
     * to a plain function pointer call). */
    if (call->as.list.len > 2) {
        Type base = obj->type;
        if (base.kind == TY_REF_IMMUT || base.kind == TY_REF_MUT) {
            base = type_from_kind(base.as.ref_borrow.target);
        }
        /* CONV-S1 (slice 6): the same capability-field call through an `fn` field
         * of a single-variant record ADT -- a record ADT *is* a product, so
         * `(.handler v arg ...)` calls the function stored in the named field,
         * exactly as a struct does (a `defstruct` with an `fn` field lowers to
         * this case).  Mirror the TY_STRUCT branch above, building the
         * EX_GET_FIELD with adt_def/adt_ctor instead of a StructDef. */
        {
            const Type *adt_base = &base;
            while (adt_base && adt_base->kind == TY_APP && adt_base->as.app.fn)
                adt_base = adt_base->as.app.fn;
            if (adt_base && adt_base->kind == TY_ADT && adt_base->as.adt_.def) {
                const AdtDef *adt = adt_base->as.adt_.def;
                if (adt_is_flat_product(adt) && adt->n_ctors == 1 &&
                    adt->ctors[0]->is_record) {
                    const CtorDef *ctor = adt->ctors[0];
                    for (uint32_t i = 0; i < ctor->n_fields; i++) {
                        if (!ctor->fields[i].name) continue;
                        if (strcmp(ctor->fields[i].name, method_name) != 0) continue;
                        if (ctor->fields[i].kind != TY_FN) break;
                        Type field_type = ctor->fields[i].full_type
                            ? *ctor->fields[i].full_type
                            : type_from_kind(ctor->fields[i].kind);
                        /* lowered-adt-ctor-skips-fn-field-type-param-inference:
                         * substitute the receiver's concrete type args into the
                         * fn-field's signature so `(.get l p)` on `l : (Lens Person
                         * cstr)` reads `get` as `(fn [Person] cstr)` -- otherwise
                         * the result stays the bare tyvar `A` and the call types as
                         * a tyvar (TUR-E0006 in an untyped context).  Mirrors the
                         * plain dot-read path's app-arg substitution above. */
                        if (ctor->fields[i].full_type && adt->n_type_params > 0 &&
                            base.kind == TY_APP) {
                            Type *type_args = (Type *)arena_alloc(
                                e->arena, adt->n_type_params * sizeof(Type));
                            if (elab_adt_type_extract_args(&base, adt, type_args))
                                field_type = adt_field_instantiate_type(
                                    e, adt, ctor->fields[i].full_type, type_args);
                        }
                        Expr *get_field = expr_new(e->arena, EX_GET_FIELD,
                                                   field_type, call->span);
                        get_field->as.get_field_.struct_expr = obj;
                        get_field->as.get_field_.field_idx = i;
                        get_field->as.get_field_.adt_def = adt;
                        get_field->as.get_field_.adt_ctor = ctor;

                        uint32_t n_args = call->as.list.len - 2;
                        Expr **args = (Expr **)arena_alloc(e->arena,
                                                           n_args * sizeof(Expr *));
                        for (uint32_t j = 0; j < n_args; j++) {
                            args[j] = elab_form(e, call->as.list.items[2 + j]);
                            if (!args[j]) return NULL;
                            /* A bare `fn` field has no signature, so the call
                             * uses the int64 register class; a float argument
                             * would reach the callee in the wrong register.
                             * Same rule as a `:fn` parameter's application. */
                            if (!ctor->fields[i].full_type &&
                                kind_is_float_class(args[j]->type.kind)) {
                                diag_emit(DIAG_ERROR, args[j]->span,
                                          "calling field '%s' with a floating-point "
                                          "argument is not supported: it is declared "
                                          "bare `fn` (no signature), and a call "
                                          "through it uses the int64 register class\n"
                                          "  = help: declare the field's function "
                                          "type, e.g. (fn [float] float)",
                                          method_name);
                                return NULL;
                            }
                        }
                        Type result_type = TYPE_INT;
                        if (field_type.kind == TY_FN) {
                            Type rt = field_type.as.fn.result_full_type
                                          ? *field_type.as.fn.result_full_type
                                          : type_from_kind(field_type.as.fn.result_kind);
                            if (rt.kind != TY_UNKNOWN && rt.kind != TY_NIL)
                                result_type = rt;
                        }
                        Expr *call_out = expr_new(e->arena, EX_CALL, result_type,
                                                  call->span);
                        call_out->as.call_.fn_binding = NULL;
                        call_out->as.call_.fn_expr = get_field;
                        call_out->as.call_.args = args;
                        call_out->as.call_.n_args = n_args;
                        /* struct-temporary-fn-field-box-leaks: an UNBOUND owning
                         * receiver -- `(.run (make-struct S f) x)`, a constructor
                         * or a call result -- owns the fn-field box the
                         * constructor made, and with no binding nothing released
                         * it (a `let`-bound one is freed at scope exit,
                         * local-struct-drop).  Bind it: `(let [t <recv>] (.run t
                         * x))` is the same program, and the `let` gives the
                         * release its owner. */
                        const Expr *ro = obj;
                        while (ro && ro->kind == EX_ASCRIBE) ro = ro->as.ascribe_.inner;
                        if (ro && (ro->kind == EX_CALL || ro->kind == EX_MAKE_STRUCT) &&
                            elab_type_owns_boxed_fnfield(obj->type)) {
                            LetBinding *lb = (LetBinding *)arena_alloc(
                                e->arena, sizeof(LetBinding));
                            Expr *rv = elab_bind_control_temp(e, obj, lb);
                            lb->binding->drops_fn_fields = true;
                            get_field->as.get_field_.struct_expr = rv;
                            Expr *let_expr = expr_new(e->arena, EX_LET,
                                                      call_out->type, call->span);
                            let_expr->as.let_.bindings = lb;
                            let_expr->as.let_.n = 1;
                            let_expr->as.let_.body = call_out;
                            return let_expr;
                        }
                        return call_out;
                    }
                }
            }
        }
    }

    /* structdef-retirement DS-D: the "search all registered struct defs for a
     * matching :fn field" fallback (for an untyped `(.method cap ...)` receiver)
     * is removed with the StructDef registry -- every former struct is a record
     * ADT and its capability fields dispatch through the ADT field paths above. */

    /* IT4: Typeclass intersection dispatch on union types.
     * When obj : (A | B), and every member type has an instance for .method,
     * generate a tag-dispatched EX_MATCH that calls the right instance per arm.
     * The synthetic scrutinee is `obj`; each arm unboxes the value and calls the
     * per-member method implementation directly (bypassing dictionary dispatch
     * to avoid nested tur_tagged_t complications). */
    if (obj->type.kind == TY_UNION) {
        uint8_t n_members = obj->type.as.union_.n_members;
        /* Elaborate the extra arguments once (they are shared across arms). */
        uint32_t n_extra = call->as.list.len - 2;
        Expr **extra_args = (Expr **)arena_alloc(e->arena, n_extra * sizeof(Expr *));
        for (uint32_t i = 0; i < n_extra; i++) {
            extra_args[i] = elab_form(e, call->as.list.items[2 + i]);
            if (!extra_args[i]) return NULL;
        }

        /* For each member, find a matching typeclass instance. */
        FnDef **member_methods = (FnDef **)arena_alloc(e->arena, n_members * sizeof(FnDef *));
        for (uint8_t um = 0; um < n_members; um++) {
            Type *mem_t = obj->type.as.union_.members[um];
            if (!mem_t) { member_methods[um] = NULL; continue; }
            FnDef *found = NULL;
            for (TypeClassInstance *inst = e->typeclass_env.instances;
                 inst != NULL && !found; inst = inst->next) {
                for (uint8_t mi = 0; mi < inst->typeclass->n_methods; mi++) {
                    const TypeClassMethod *meth = &inst->typeclass->methods[mi];
                    if (meth->name->len != method_name_len ||
                        memcmp(meth->name->name, method_name, method_name_len) != 0) continue;
                    if (inst->n_type_args > 0 &&
                        inst->type_args[0].kind != mem_t->kind) continue;
                    found = inst->method_impls[mi];
                    break;
                }
            }
            member_methods[um] = found;
            if (!found) {
                diag_emit(DIAG_ERROR, call->span,
                          "typeclass method '%.*s' not available for union member '%s'",
                          (int)method_name_len, method_name, type_name(*mem_t));
                return NULL;
            }
        }

        /* Determine result type from the first member's method. */
        Type result_type = TYPE_NIL;
        if (n_members > 0 && member_methods[0]) {
            FnDef *m0 = member_methods[0];
            if (m0->binding->type.kind == TY_FN)
                result_type = type_from_kind(m0->binding->type.as.fn.result_kind);
            else if (m0->body)
                result_type = m0->body->type;
        }
        if (result_type.kind == TY_UNKNOWN) result_type = TYPE_INT;

        /* Build a fresh binding name for the unboxed arm variable. */
        static uint32_t union_dispatch_ctr = 0;
        char arm_name_buf[32];
        snprintf(arm_name_buf, sizeof(arm_name_buf), "__udisp_%u", union_dispatch_ctr++);
        const Symbol *arm_sym = intern_cstr(e->st, arm_name_buf);

        /* Build the arms array. */
        MatchArm *arms = (MatchArm *)arena_alloc(e->arena, n_members * sizeof(MatchArm));
        for (uint8_t um = 0; um < n_members; um++) {
            Type *mem_t = obj->type.as.union_.members[um];
            FnDef *meth = member_methods[um];

            /* Pattern: type-narrowing, binds arm_sym to the unboxed value. */
            MatchArm *arm = &arms[um];
            memset(arm, 0, sizeof(*arm));
            arm->pattern.is_var = true;
            arm->pattern.var_sym = arm_sym;
            arm->pattern.union_member_idx = (int)um;
            arm->pattern.n_bindings = 1;
            arm->pattern.bindings = (Binding **)arena_alloc(e->arena, sizeof(Binding *));
            Binding *var_b = binding_new(e, arm_sym, *mem_t, false, false, call->span);
            arm->pattern.bindings[0] = var_b;
            arm->pattern.var_binding = var_b;
            arm->guard = NULL;

            /* Body: call meth with (var_b, extra_args...) */
            Expr *var_expr = expr_new(e->arena, EX_VAR, *mem_t, call->span);
            var_expr->as.var.binding = var_b;

            uint32_t total_args = 1 + n_extra;
            Expr **call_args = (Expr **)arena_alloc(e->arena, total_args * sizeof(Expr *));
            call_args[0] = var_expr;
            for (uint32_t ei = 0; ei < n_extra; ei++) call_args[1 + ei] = extra_args[ei];

            Expr *body_call = expr_new(e->arena, EX_CALL, result_type, call->span);
            body_call->as.call_.fn_binding = meth->binding;
            body_call->as.call_.fn_expr = NULL;
            body_call->as.call_.args = call_args;
            body_call->as.call_.n_args = total_args;
            arm->body = body_call;
        }

        Expr *out = expr_new(e->arena, EX_MATCH, result_type, call->span);
        out->as.match_.scrutinee = obj;
        out->as.match_.arms = arms;
        out->as.match_.n_arms = n_members;
        return out;
    }

    /* Phase HKT H2: Type-based instance lookup.
     * Build a TypeClassDispatchKey from the obj type, then use
     * typeclass_env_lookup_instance_by_key for a two-level search.
     * Fall back to name-only search if the type-based lookup yields nothing
     * (e.g. TY_UNKNOWN during forward-reference elaboration). */
    FnDef *best_method = NULL;
    /* Phase H §1: Track the selected instance so we can build an EX_DICT node. */
    TypeClassInstance *best_inst = NULL;
    /* Phase D0: count fallback candidates and track whether an exact match was found. */
    int fallback_count = 0;
    bool exact_match_found = false;
    /* stdlib-hkt-consolidation T1: when the receiver type is erased to int64_t,
     * every name-matching instance becomes a "fallback" and >1 of them is
     * reported as ambiguous (TUR_E0020). Adding stdlib HKT instances (e.g.
     * Functor/Monad [Option]) introduces a second fallback for programs that
     * define their own same-shaped instance, turning previously-unambiguous
     * dispatch into an error. To keep a locally-defined instance authoritative,
     * prefer the unique *user* (non-stdlib) fallback when the only other
     * candidates are stdlib-provided. This is purely additive: it only changes
     * cases that today emit TUR_E0020, so no currently-resolving dispatch is
     * affected. The key is the instance's *origin* file (not the call span,
     * which for macro-expanded `.bind`/`.fmap` points at stdlib/macros.tur). */
    FnDef *user_fallback_method = NULL;
    TypeClassInstance *user_fallback_inst = NULL;
    /* same-method-name-in-two-classes-dispatches-by-declaration-order: the
     * instance found (if any) whose class is a DIFFERENT non-stdlib class than
     * `best_inst`'s and which also matches this receiver.  Non-NULL means the
     * call is genuinely ambiguous and the search below stopped to say so
     * instead of silently keeping whichever registered last. */
    TypeClassInstance *ambig_inst = NULL;
    int user_fallback_count = 0;
    /* A head-matching instance the search dropped because its own constraint
     * does not hold for the receiver's element type -- `Kind [Vec]` requiring
     * `(Kind A)` against a `(Vec any)`.  When only scalar instances are left
     * over, the ambiguity diagnostic below would otherwise blame an "erased"
     * receiver that is in fact concrete; this names the real cause. */
    TypeClassInstance *unsat_inst = NULL;

    /* GHE (constrained-generic-instance-dispatch): when the receiver is a bare
     * type variable `K` (a constrained generic type parameter, e.g. the `x : K`
     * of `(defn f [^Hash K x :K] ...)`), the concrete instance is not known
     * until the function is monomorphized.  This compiler realizes constrained
     * generics via emit-time ABI specialization, so we must:
     *   (a) pick a *carrier-compatible* representative instance here -- the one
     *       whose type_args[0] is TY_INT, since the polymorphic base clone takes
     *       the int64_t carrier and a tyvar key bottoms out at the carrier.
     *       This makes the base clone valid C *and* correct for `int` keys.
     *   (b) tag the call (via dict_arg, built below from best_inst's typeclass)
     *       so emit_call_name can re-resolve to __inst_<Class>_<method>_<T> for
     *       each non-carrier ABI specialization (cstr/bool/float32/...).
     * Without this the old KIND_ARROW path spuriously matched the first instance
     * whose type_args[0] failed the (incomplete) primitive test -- typically
     * Hash[float32] -- baking a wrong, type-incompatible callee into the body. */
    /* M5 (docs/archive/history/m5-constrained-poly-wrong-instance-on-tyvar-receiver.md):
     * An EX_ASCRIBE-to-tyvar receiver (`(:: v A)`) elaborates to a
     * TY_STRUCT with NULL def -- the "abstract tyvar" representation
     * the elaborator uses when ascribing a concrete value to a class-
     * constraint tyvar.  Treat it the same as TY_TYVAR for typeclass
     * dispatch: pick the carrier-compatible (int) instance, then let
     * emit-side re-resolution (emit_core.c:emit_reresolve_method_call)
     * specialize to the concrete A per call site.  Without this branch
     * the dispatch fell through to the KIND_ARROW iteration below and
     * picked the first non-primitive instance from the env (typically
     * `Eq MutableMap`, alphabetically near the head), producing a
     * silent miscompile that SIGSEGV'd at runtime. */
    bool obj_is_abstract_tyvar =
        obj->type.kind == TY_TYVAR ||
        obj_is_unascribed_carrier_elem(obj);
    /* typeclass-method-resolution-ignores-the-class (symptom B): a method call
     * on a genuinely ABSTRACT type variable is only well formed when the
     * enclosing generic declares a constraint naming that method's class.
     *
     * The representative search just below deliberately binds such a receiver
     * to an arbitrary name-matching instance, because for a CONSTRAINED body
     * that is exactly right -- the representative keeps the polymorphic base
     * clone valid C, and monomorphization re-resolves the call per
     * instantiation.  For an UNCONSTRAINED body there is nothing to re-resolve
     * to, so the representative survived into the emitted C as a direct call to
     * some other type's impl: `tur check` exited 0 and `cc` then rejected
     * `__inst_Foo_foo_hyof_Bar(w)` for passing the int64 carrier where the
     * instance declared its own payload.  A check/build divergence is the worst
     * place to put this, since `check` is what editors and CI type-check with.
     *
     * Only a TY_TYVAR receiver is gated.  obj_is_unascribed_carrier_elem() is an
     * erased container element, not an abstract parameter -- the enclosing fn
     * need not (and usually does not) constrain it, so gating it would reject
     * working code.  A body with no constraint vector at all that is not inside
     * any defn (cur_fn_n_constraints == 0 with a NULL list) is left alone for
     * the same reason. */
    if (obj->type.kind == TY_TYVAR && method_name_len > 0 &&
        e->definstance_depth == 0) {
        TypeClass *owner = NULL;
        for (TypeClass *c = e->typeclass_env.typeclasses; c && !owner; c = c->next) {
            for (uint8_t mi = 0; mi < c->n_methods; mi++) {
                const Symbol *mn = c->methods[mi].name;
                if (mn && mn->len == method_name_len &&
                    memcmp(mn->name, method_name, method_name_len) == 0) {
                    owner = c;
                    break;
                }
            }
        }
        if (owner) {
            bool constrained = false;
            for (uint8_t ci = 0; ci < e->cur_fn_n_constraints && !constrained; ci++) {
                TypeConstraint *con = &e->cur_fn_constraints[ci];
                if (!con || !con->typeclass) continue;
                /* Match by class identity, or by name so a re-registered class
                 * (same defclass seen through two import paths) still counts --
                 * and, class-superclasses SC3 (Half A), through the constrained
                 * class's superclass closure: `[^Monoid A]` entails `Semigroup`,
                 * so `combine` is licensed.  typeclass_entails keeps the
                 * by-name match inside the closure walk. */
                if (typeclass_entails(&e->typeclass_env, con->typeclass, owner))
                    constrained = true;
            }
            if (!constrained) {
                /* Name the type VARIABLE (`W`), not its kind: type_name() on a
                 * TY_TYVAR prints the literal "tyvar", which tells the reader
                 * nothing about which parameter to constrain. */
                const char *tv = obj->type.as.tyvar_.name;
                if (!tv || !*tv) tv = "the receiver's type parameter";
                const char *fn = e->current_fn_name && e->current_fn_name->name
                                     ? e->current_fn_name->name : NULL;
                const char *cls = owner->name ? owner->name->name : "C";
                char whobuf[160];
                if (fn) snprintf(whobuf, sizeof(whobuf), "'%s'", fn);
                else    snprintf(whobuf, sizeof(whobuf), "%s", "this function");
                diag_emit_with_code(DIAG_ERROR, call->span,
                    TUR_E0015_TYPECLASS_CONSTRAINT_NOT_SATISFIED,
                    "'%.*s' is a method of typeclass '%s', but %s does not "
                    "constrain '%s' to it -- so there is no instance to dispatch "
                    "to. Add the constraint: (defn %s [%s] [(%s %s)] ...).",
                    (int)method_name_len, method_name, cls,
                    whobuf, tv,
                    fn ? fn : "f", tv, cls, tv);
                return NULL;
            }
        }
    }
    if (obj_is_abstract_tyvar) {
        TypeClassInstance *carrier_inst = NULL;
        FnDef *carrier_method = NULL;
        /* constrained-generic-instance-element-dispatch: when no `int` instance
         * exists, fall back to any *carrier-compatible scalar* instance as the
         * representative (e.g. `Enc [cstr]` for a json `Encode [Vec]` whose only
         * scalar instances are cstr/float).  Such a scalar rides the int64
         * carrier, so the polymorphic base clone stays valid C; emit-side
         * re-resolution (emit_reresolve_method_call) then specializes the inner
         * element call to the concrete A per ABI specialization.  Without this
         * representative the dispatch fell through to the generic search and a
         * tyvar receiver with >1 name-matching instance reported TUR_E0020. */
        TypeClassInstance *scalar_inst = NULL;
        FnDef *scalar_method = NULL;
        /* phantom-constrained-generic-base-body-picks-aggregate-instance: a
         * non-pointer `defopaque` newtype IS the int64 carrier at runtime, so it
         * is as valid a polymorphic-base representative as a bare `int` -- and
         * for some classes it is the ONLY carrier-shaped instance there is.
         * `JoinSemilattice` is the case in point: its scalar instances are
         * `Sum`/`Product`/`MinI`/`MaxI`, all `defopaque` over int, while its
         * other instances are by-value aggregates.  Without this tier both
         * searches below missed, the dispatch fell through to the generic
         * search, and that picked an AGGREGATE instance -- making the base clone
         * an aggregate-to-carrier reinterpret, i.e. a TUR-E0295 on a function
         * that is never called, naming a type the author never mentioned.
         * Adding an unrelated aggregate instance elsewhere in the program could
         * break a generic that compiled yesterday.
         *
         * The return-directed twin at the top of this file has had this tier
         * since nullary-class-method-unresolvable-over-newtype-tyvar; this is
         * the receiver-directed version, which never grew it.  Emit-side
         * re-resolution specializes the call per monomorphization, and Gap H
         * names same-carrier newtype specs apart with `__h<n>`, so the
         * representative is a placeholder rather than a baked answer. */
        TypeClassInstance *opaque_inst = NULL;
        FnDef *opaque_method = NULL;
        for (TypeClassInstance *inst = e->typeclass_env.instances;
             inst != NULL && !carrier_inst; inst = inst->next) {
            for (uint8_t i = 0; i < inst->typeclass->n_methods; i++) {
                const TypeClassMethod *method = &inst->typeclass->methods[i];
                if (method->name->len != method_name_len ||
                    memcmp(method->name->name, method_name, method_name_len) != 0) {
                    continue;
                }
                if (inst->n_type_args > 0 && inst->type_args[0].kind == TY_INT) {
                    carrier_inst = inst;
                    carrier_method = inst->method_impls[i];
                } else if (!scalar_inst && inst->n_type_args > 0) {
                    /* A scalar that fits the int64 carrier slot (pointer-or-word
                     * sized): cstr/bool/nil/sized-int.  Floats and aggregates do
                     * not ride the carrier and would make the base clone ill-typed,
                     * so they are not eligible representatives. */
                    TypeKind itk = inst->type_args[0].kind;
                    bool carrier_scalar =
                        (itk == TY_CSTR || itk == TY_BOOL || itk == TY_NIL ||
                         itk == TY_PTR_VOID || itk == TY_SYM ||
                         itk == TY_INT8 || itk == TY_INT16 || itk == TY_INT32 ||
                         itk == TY_INT64 ||
                         itk == TY_UINT8 || itk == TY_UINT16 || itk == TY_UINT32 ||
                         itk == TY_UINT64);
                    if (carrier_scalar) {
                        scalar_inst = inst;
                        scalar_method = inst->method_impls[i];
                    } else if (!opaque_inst && itk == TY_ADT) {
                        /* A `defopaque` newtype over a non-pointer base rides
                         * the int64 carrier exactly as `int` does. */
                        const AdtDef *ad = inst->type_args[0].as.adt_.def;
                        if (ad && ad->is_opaque && !ad->opaque_base_is_ptr) {
                            opaque_inst = inst;
                            opaque_method = inst->method_impls[i];
                        }
                    }
                }
                break; /* one method match per instance */
            }
        }
        if (!carrier_inst && scalar_inst) {
            carrier_inst = scalar_inst;
            carrier_method = scalar_method;
        }
        /* Lowest tier: a carrier-shaped opaque newtype, preferred over falling
         * through to a generic search that may land on an aggregate. */
        if (!carrier_inst && opaque_inst) {
            carrier_inst = opaque_inst;
            carrier_method = opaque_method;
        }
        if (carrier_inst) {
            best_method = carrier_method;
            best_inst = carrier_inst;
            exact_match_found = true;
            goto found_method;
        }
        /* No carrier-compatible instance for this class: fall through to the
         * generic search (keeps prior behavior for classes without a
         * carrier-compatible instance; such a constrained generic would still
         * need a fix). */
    }

    /* Determine the effective constructor kind from the obj type. */
    Kind obj_ck = KIND_STAR;
    {
        TypeKind tk = obj->type.kind;
        /* M5 fix: the sized numeric variants are primitives too; without
         * them, an obj of type :float32 / :int8 / etc. gets obj_ck=KIND_ARROW
         * and the wrong-direction iteration matches via the non-primitive
         * fallthrough branch instead of the kind-exact-match branch.  See
         * the symmetric inst_is_primitive fix below at line ~3675. */
        bool is_primitive = (tk == TY_INT  || tk == TY_BOOL  || tk == TY_CSTR ||
                             tk == TY_NIL  || tk == TY_FLOAT || tk == TY_PTR_VOID ||
                             tk == TY_SYM  || tk == TY_UNKNOWN ||
                             tk == TY_INT8 || tk == TY_INT16 || tk == TY_INT32 ||
                             tk == TY_INT64 ||
                             tk == TY_UINT8 || tk == TY_UINT16 || tk == TY_UINT32 ||
                             tk == TY_UINT64 ||
                             tk == TY_FLOAT32 || tk == TY_FLOAT64);
        obj_ck = is_primitive ? KIND_STAR : KIND_ARROW;
    }

    /* same-method-name-in-two-classes-dispatches-by-declaration-order: is this
     * method name declared by more than one non-stdlib class?
     *
     * The search below takes the FIRST instance whose name and receiver type
     * both match and stops (`goto found_method`), and the registry is a
     * singly-linked list whose head is the most recently registered entry --
     * so with two user classes declaring `encode`, `(encode 42)` silently
     * called whichever `definstance` came last, `tur check` exited 0 with no
     * output, and swapping two unrelated forms changed the answer.
     *
     * The check runs only when this cheap pre-scan (over the CLASS list, which
     * is short, not the instance list) says two classes are in play; otherwise
     * the search keeps its original early exit and costs nothing.  Only
     * non-stdlib classes count on both sides: a user `defn` deliberately
     * shadowing a stdlib class method is an existing, intentional pattern that
     * `from_stdlib` and TUR-W0039 exist to support, and it is defn-vs-method,
     * not the method-vs-method collision this is about. */
    bool ambig_watch = false;
    {
        const TypeClass *seen = NULL;
        for (TypeClass *c = e->typeclass_env.typeclasses; c && !ambig_watch;
             c = c->next) {
            if (c->from_stdlib) continue;
            for (uint8_t mi = 0; mi < c->n_methods; mi++) {
                const Symbol *mn = c->methods[mi].name;
                if (!mn || mn->len != method_name_len ||
                    memcmp(mn->name, method_name, method_name_len) != 0)
                    continue;
                if (!seen) seen = c;
                else if (!typeclass_same_class(seen, c)) ambig_watch = true;
                break;   /* one method-name match per class is enough */
            }
        }
    }

    /* Search instances — prefer the one whose type_args[0] matches obj's type. */
    for (TypeClassInstance *inst = e->typeclass_env.instances; inst != NULL; inst = inst->next) {
        for (uint8_t i = 0; i < inst->typeclass->n_methods; i++) {
            const TypeClassMethod *method = &inst->typeclass->methods[i];
            if (method->name->len != method_name_len ||
                memcmp(method->name->name, method_name, method_name_len) != 0) {
                continue;
            }
            /* Name matched.  Now check if this instance's first type_arg
             * matches the obj type.  For KIND_STAR we compare TypeKind
             * exactly; for KIND_ARROW we accept any non-primitive whose
             * struct constructor matches (when known via TY_APP). */
            if (inst->n_type_args > 0 && obj->type.kind != TY_UNKNOWN) {
                bool type_ok;
                if (obj_ck == KIND_STAR) {
                    type_ok = (inst->type_args[0].kind == obj->type.kind);
                } else {
                    /* KIND_ARROW: accept non-primitive instance type_args.
                     * F3-7 (cross-plan-followups): when the receiver is a
                     * TY_APP, walk to its head and use the struct identity
                     * to discriminate Eq[Vec] from Eq[Map] etc.  Without
                     * this, a TY_APP(Vec, int) receiver matches the first
                     * KIND_ARROW Eq instance in registration order
                     * (typically Eq[Set]) and we silently dispatch through
                     * the wrong vtable. */
                    TypeKind itk = inst->type_args[0].kind;
                    /* M5 fix (docs/archive/history/m5-constrained-poly-spec-wrong-
                     * dispatch-for-parametric-receiver.md): the sized numeric
                     * variants are primitives too -- without listing them, an
                     * `Eq float32` (or `Eq int32` / `uint8` / etc.) instance
                     * slips through `type_ok = !inst_is_primitive` for a
                     * parametric receiver like `(Vec A)`, becomes a false
                     * "good match", and overrides the correctly-rejected
                     * `Eq Vec` (rejected by its `(Eq A)` type-param constraint
                     * because A is still a TYVAR at this elab pass).  Result
                     * was a baked `__inst_Eq_eq_qu_float32(Vec__int, Vec__int)`
                     * call -- a hard cc error. */
                    bool inst_is_primitive =
                        (itk == TY_INT  || itk == TY_BOOL || itk == TY_CSTR ||
                         itk == TY_NIL  || itk == TY_FLOAT || itk == TY_PTR_VOID ||
                         itk == TY_SYM  ||
                         itk == TY_INT8 || itk == TY_INT16 || itk == TY_INT32 ||
                         itk == TY_INT64 ||
                         itk == TY_UINT8 || itk == TY_UINT16 || itk == TY_UINT32 ||
                         itk == TY_UINT64 ||
                         itk == TY_FLOAT32 || itk == TY_FLOAT64);
                    type_ok = !inst_is_primitive;
                    /* Bare type-variable instance argument (whole type_args[0] is
                     * a TY_TYVAR, not a partially-applied head like
                     * `Functor [(Result _ B)]` whose *argument* is erased): this
                     * arises when an instance is registered for a type name that
                     * did not resolve to a concrete nominal type -- e.g.
                     * `Eq [str]`, where `str` has no defstruct/defdata and stays
                     * an unbound tyvar.  Left as an exact match it becomes a
                     * catch-all wildcard: every non-primitive receiver "matches"
                     * it, so the first such instance in registration order shadows
                     * the receiver's own instance, producing a silent wrong-vtable
                     * dispatch that SIGSEGVs (docs/archive/
                     * eq-bound-misdispatch-extra-instance.md: `(.eq? (Inclusive 4)
                     * (Inclusive 4))` mis-dispatched to `Eq [str]`).  When the
                     * receiver is a concrete nominal type (an ADT/struct with a
                     * def, bare or applied), demote the bare-tyvar instance to a
                     * *fallback* -- the search then continues to the receiver's
                     * concrete instance and prefers it, while a lone bare-tyvar
                     * instance still wins when it is the only candidate.  This
                     * mirrors the KIND_STAR path, where a tyvar type_args[0] never
                     * equals a primitive receiver kind and is already a fallback. */
                    if (type_ok && itk == TY_TYVAR) {
                        const Type *oh = &obj->type;
                        while (oh && oh->kind == TY_APP) oh = oh->as.app.fn;
                        /* Structs lower to TY_ADT (from_struct_lowering), so a
                         * concrete nominal head is always a TY_ADT with a def. */
                        bool recv_concrete_nominal =
                            oh && oh->kind == TY_ADT && oh->as.adt_.def;
                        if (recv_concrete_nominal) type_ok = false;
                    }
                    /* An `any` head (`Hash [any]`, S9's minted `C [any]`) is
                     * the instance FOR an `any` receiver, not a wildcard over
                     * every non-primitive one: a concrete `(Vec any)` must
                     * reach `C [Vec]`, whose constraint the `[any]` instance
                     * then discharges.  Left as a match, the newest `[any]`
                     * instance shadowed every struct/ADT instance of the class
                     * (instances are prepended). */
                    if (type_ok && itk == TY_ANY && obj->type.kind != TY_ANY)
                        type_ok = false;
                    /* Arrow head (itk == TY_FN): an `Arrow [(->)]` instance
                     * matches only a function receiver, never a struct/vec.
                     * Conversely, a function receiver must not bind a non-arrow
                     * KIND_ARROW instance (e.g. an opaque container). */
                    if (type_ok && (itk == TY_FN || obj->type.kind == TY_FN)) {
                        type_ok = (itk == TY_FN && obj->type.kind == TY_FN);
                    }
                    /* ECS E2d-P6 (Issue 3): when BOTH the receiver and the
                     * instance head are fully-applied type constructors -- e.g. a
                     * receiver `(Dense Pos)` against instances `[(Dense Pos) Pos]`
                     * and `[(Sparse Vel) Vel]` -- the bare `!inst_is_primitive`
                     * test above accepts every non-primitive instance, so the
                     * first one in registration order silently wins regardless of
                     * the constructor (Dense vs Sparse) or argument (Pos vs Vel).
                     * Discriminate by (a) head-constructor identity and (b) the
                     * leftmost type argument when both sides are concrete, so each
                     * storage backend dispatches to its own instance.  This is the
                     * miscompile case CLAUDE.md flags: "works by luck because the
                     * register classes happen to match".
                     *
                     * Carrier/tyvar/erased instance arguments (a partially-applied
                     * head like `Functor [(Result _ B)]`, whose varying arm is
                     * erased to the int64 carrier) act as wildcards: comparing
                     * only concrete-vs-concrete keeps those instances matching. */
                    /* Plain (non-applied) aggregate receiver against a plain
                     * aggregate instance head: discriminate by type identity.
                     * Without this, every struct/ADT instance of a class
                     * (e.g. Render[Color4], Render[Color8], Render[Color24])
                     * looks like an equally-good non-primitive match, so the
                     * first one in registration order silently wins.  Since
                     * instances are prepended (typeclass.c:register_instance),
                     * "first in registration order" is the LAST-declared
                     * instance -- so a struct-argument typeclass call resolves
                     * every call site to the last-declared instance.  That is a
                     * miscompile (a hard cc type error when the struct layouts
                     * differ, a silent wrong-vtable dispatch when they happen to
                     * match -- the "works by luck because the register classes
                     * match" case CLAUDE.md flags).  Carrier/erased heads
                     * (NULL def) stay wildcards. */
                    if (type_ok && obj->type.kind == TY_ADT && itk == TY_ADT &&
                        obj->type.as.adt_.def && inst->type_args[0].as.adt_.def &&
                        obj->type.as.adt_.def != inst->type_args[0].as.adt_.def) {
                        type_ok = false;
                    }
                    if (type_ok && obj->type.kind == TY_APP &&
                        inst->type_args[0].kind == TY_APP) {
                        const Type *oh = &obj->type;
                        while (oh && oh->kind == TY_APP) oh = oh->as.app.fn;
                        const Type *ih = &inst->type_args[0];
                        while (ih && ih->kind == TY_APP) ih = ih->as.app.fn;
                        bool heads_differ = false;
                        if (oh && ih && oh->kind == TY_ADT && ih->kind == TY_ADT &&
                            oh->as.adt_.def && ih->as.adt_.def &&
                            oh->as.adt_.def != ih->as.adt_.def) {
                            heads_differ = true;
                        }
                        if (heads_differ) {
                            type_ok = false;
                        } else {
                            const Type *oa = obj->type.as.app.arg;
                            const Type *ia = inst->type_args[0].as.app.arg;
                            /* G10: discriminate on a concrete PRIMITIVE element
                             * too (cstr vs int), not only struct/ADT elements --
                             * so `Enc [(Option cstr)]` and `Enc [(Option int)]`
                             * are not conflated.  A tyvar element stays a
                             * wildcard. */
                            if (typeclass_type_arg_concrete(oa) &&
                                typeclass_type_arg_concrete(ia) &&
                                !type_eq(*oa, *ia)) {
                                type_ok = false;
                            }
                        }
                    }
                    /* CONV-S1: head-normalized nominal discrimination across the
                     * applied/bare and struct/ADT asymmetries the blocks above
                     * miss.  Once a parametric struct lowers, an ADT-app RECEIVER
                     * `(Option int)` (TY_APP, head TY_ADT) is matched against
                     * instances whose head is a *bare* ADT (`Eq [Option]`,
                     * type_arg TY_ADT) AND against still-struct heads (`:heap`
                     * `Eq [MutableMap]`, type_arg TY_STRUCT).  None of the
                     * struct/struct, app/app, or adt/adt blocks above fire on the
                     * cross-category (TY_ADT head vs TY_STRUCT head) or the
                     * applied-vs-bare pairing, so every non-primitive instance is
                     * an equally-good match and the first registered one wins
                     * (e.g. `(eq? (some 5) (some 5))` mis-dispatched to
                     * `Eq [MutableMap]`).  Walk both sides to their head; when BOTH
                     * heads are concrete nominal types (a struct/ADT with a def),
                     * require the same constructor.  A tyvar / fn / erased
                     * (NULL-def) head stays a wildcard. */
                    if (type_ok) {
                        const Type *oh = &obj->type;
                        while (oh && oh->kind == TY_APP) oh = oh->as.app.fn;
                        const Type *ih = &inst->type_args[0];
                        while (ih && ih->kind == TY_APP) ih = ih->as.app.fn;
                        bool o_adt = oh && oh->kind == TY_ADT && oh->as.adt_.def;
                        bool i_adt = ih && ih->kind == TY_ADT && ih->as.adt_.def;
                        if (o_adt && i_adt) {
                            bool same = oh->as.adt_.def == ih->as.adt_.def;
                            if (!same) type_ok = false;
                        }
                    }
                }
                if (!type_ok) {
                    /* Record as fallback but keep searching. */
                    fallback_count++;
                    if (!best_method) { best_method = inst->method_impls[i]; best_inst = inst; }
                    /* Track user (non-stdlib) fallbacks so an ambiguity between a
                     * local instance and a stdlib one resolves to the local one. */
                    {
                        const char *opath = diag_file_path(inst->origin_file_id);
                        /* stdlib files always live directly under a dir ending
                         * in "stdlib" (see resolve_stdlib_root in main.c), so the
                         * path contains "stdlib/<basename>". The root may be
                         * relative ("stdlib/option.tur") or absolute, so match
                         * the "stdlib/" component without requiring a leading
                         * slash. Classify purely by path: file_id 0 is the entry
                         * (user) file, NOT unknown -- its path is the user's
                         * program, so it must count as a user instance. */
                        bool is_stdlib = (opath && strstr(opath, "stdlib/") != NULL);
                        if (!is_stdlib) {
                            user_fallback_count++;
                            if (!user_fallback_method) {
                                user_fallback_method = inst->method_impls[i];
                                user_fallback_inst = inst;
                            }
                        }
                    }
                    continue;
                }
            }
            /* Phase PTC3/PTC4: Check type parameter constraints on this instance.
             * Extract TY_APP elem types in type_params order (innermost first)
             * to support multi-param types like Map[K V]. */
            if (inst->type_param_constraints && inst->n_type_param_constraints > 0) {
                Type obj_type = obj->type;
                const Type *elem_types = NULL;
                uint8_t n_elem = 0;
                Type elem_buf[8];
                if (obj->type.kind == TY_APP) {
                    Type raw[8];
                    uint8_t n_raw = 0;
                    for (const Type *tx = &obj->type;
                         tx && tx->kind == TY_APP && n_raw < 8;
                         tx = tx->as.app.fn) {
                        if (tx->as.app.arg) raw[n_raw++] = *tx->as.app.arg;
                    }
                    for (uint8_t ri = 0; ri < n_raw; ri++)
                        elem_buf[ri] = raw[n_raw - 1 - ri];
                    n_elem = n_raw;
                    if (n_elem > 0) elem_types = elem_buf;
                }
                bool cons_ok = typeclass_instance_constraints_satisfied(
                    inst, &obj_type, 1, elem_types, n_elem, &e->typeclass_env);
                /* D8 Q1: in a dynamic file an `any` element discharges the
                 * constraint through the minted `C [any]` dictionary; the
                 * first such call mints it.  A typed file keeps the refusal --
                 * it has no registry to dispatch through. */
                if (!cons_ok && lang_span_is_dynamic(call->span)) {
                    bool any_elem = false;
                    for (uint8_t ei = 0; ei < n_elem && !any_elem; ei++)
                        any_elem = elem_types[ei].kind == TY_ANY;
                    if (any_elem) {
                        saffron_mint_constraint_any_instances(e, inst, call->span);
                        cons_ok = typeclass_instance_constraints_satisfied(
                            inst, &obj_type, 1, elem_types, n_elem, &e->typeclass_env);
                    }
                }
                if (!cons_ok) {
                    if (!unsat_inst) unsat_inst = inst;
                    continue;
                }
            }
            /* Good match (or no type_args to check). */
            if (exact_match_found) {
                /* same-method-name-in-two-classes: a SECOND instance matches
                 * this receiver as well.  Another instance of the same class is
                 * not ambiguity (the first one registered still wins, exactly as
                 * before); a different user class is. */
                if (best_inst &&
                    !typeclass_same_class(best_inst->typeclass, inst->typeclass)) {
                    ambig_inst = inst;
                    goto found_method;
                }
                break;
            }
            best_method = inst->method_impls[i];
            best_inst = inst;
            exact_match_found = true;
            /* Original behaviour unless two user classes declare this name, in
             * which case keep scanning for the second match that makes the call
             * ambiguous. */
            if (!ambig_watch) goto found_method;
            break;
        }
    }
found_method:;

    /* same-method-name-in-two-classes-dispatches-by-declaration-order: report
     * the ambiguity rather than resolving it by registration order.  A call is
     * only genuinely ambiguous when BOTH classes have an instance that matches
     * this receiver, which is what `ambig_inst` records -- two classes merely
     * declaring the same method name is harmless and is not reported here. */
    if (ambig_inst && best_inst) {
        const char *c1 = (best_inst->typeclass && best_inst->typeclass->name)
                             ? best_inst->typeclass->name->name : "?";
        const char *c2 = (ambig_inst->typeclass && ambig_inst->typeclass->name)
                             ? ambig_inst->typeclass->name->name : "?";
        diag_emit_with_code(DIAG_ERROR, call->span, TUR_E0020_AMBIGUOUS_DISPATCH,
            "ambiguous method dispatch: '%.*s' is declared by typeclass '%s' and "
            "by typeclass '%s', and both have an instance for type '%s', so "
            "which one runs would depend on the order the two 'definstance' "
            "forms are declared in. Give each class's method a distinct name.",
            (int)method_name_len, method_name, c1, c2, type_name(obj->type));
        return NULL;
    }

    if (!best_method) {
        /* saffron-lang-plan S4/D4 (G11): a dynamic field read.
         *
         * `(.x p)` with `p : any` has exhausted both static routes -- there is
         * no record field to find on `any`, and no typeclass instance for it --
         * which in Turmeric is the end of the road.  In Saffron the receiver's
         * type is whatever arrived, so the field is looked up then, against the
         * value's own constructor.
         *
         * Placed here, after every static route has been tried, so a Saffron
         * program with a CONCRETE receiver still gets ordinary static field
         * access and ordinary method dispatch; only a genuinely dynamic
         * receiver defers. */
        if (obj && obj->type.kind == TY_ANY && lang_span_is_dynamic(call->span)) {
            Type any_t;
            memset(&any_t, 0, sizeof(any_t));
    any_t.copy_kind = CK_COPY;   /* CK_UNIQUE is 0: a zeroed type is unique-kinded (saffron-any-let-binding-is-unique) */
            any_t.kind = TY_ANY;
            Expr *df = expr_new(e->arena, EX_DYN_FIELD, any_t, call->span);
            df->as.dyn_field_.obj   = obj;
            df->as.dyn_field_.field =
                symtab_intern(e->st, strslice(method_name, method_name_len));
            return elab_hoist_control_operands(e, df);
        }
        /* typeclass-method-resolution-ignores-the-class (symptom A): before
         * claiming the method does not exist, ask the CLASS table.  The old
         * text ("no typeclass method found") denied the existence of a method
         * declared a few lines above, in the defclass.
         *
         * Since symptom A was fixed, a `definstance` declared BELOW the use
         * resolves (the driver defers and retries such a defn), so reaching
         * here with zero registered instances means the program declares none
         * at all -- not that one is merely out of order. */
        {
            TypeClass *owner = NULL;
            for (TypeClass *c = e->typeclass_env.typeclasses; c && !owner;
                 c = c->next) {
                for (uint8_t mi = 0; mi < c->n_methods; mi++) {
                    const Symbol *mn = c->methods[mi].name;
                    if (mn && mn->len == method_name_len &&
                        memcmp(mn->name, method_name, method_name_len) == 0) {
                        owner = c;
                        break;
                    }
                }
            }
            if (owner) {
                uint32_t n_inst = 0;
                for (TypeClassInstance *inst = e->typeclass_env.instances; inst;
                     inst = inst->next)
                    if (inst->typeclass == owner) n_inst++;
                const char *cls = owner->name ? owner->name->name : "?";
                if (n_inst == 0) {
                    /* No instance of this class exists in the program.
                     *
                     * This used to add "instances register in source order:
                     * place it ABOVE this use", because a `definstance` below
                     * the use genuinely did not resolve (symptom A).  That is
                     * fixed -- a defn that cannot resolve a class method is
                     * elaborated speculatively and retried after every form in
                     * its unit, so declaration order no longer matters -- and
                     * the advice would now send the reader to move a form that
                     * is already fine.  Reaching here means there is no
                     * instance ANYWHERE, which is a different problem.
                     *
                     * Inside an imported module "anywhere" means "anywhere
                     * yet": the importer's instances register after the import
                     * is elaborated.  The counter lets the module driver tell
                     * this failure apart and park the defn until one does
                     * (elab_noinst_retry, elab_module.c). */
                    e->noinst_failures++;
                    diag_emit_with_code(DIAG_ERROR, call->span,
                        TUR_E0015_TYPECLASS_CONSTRAINT_NOT_SATISFIED,
                        "'%.*s' is a method of typeclass '%s', but this program "
                        "declares no '%s' instance at all, so the call cannot be "
                        "resolved. Add a (definstance %s [...] ...) -- it may "
                        "appear anywhere in the file, above or below this use.",
                        (int)method_name_len, method_name, cls, cls, cls);
                } else {
                    /* Instances of this class DO exist and are in scope; none
                     * applies to this receiver -- e.g. a parametric instance
                     * whose own constraint is unsatisfied for the element type
                     * (`Measurable [Box]` requiring `(Measurable A)` against a
                     * `(Box bool)` with no `Measurable [bool]`).  Telling the
                     * author to move or declare an instance would be wrong, so
                     * report the receiver instead, matching the wording of the
                     * concrete-receiver arm further down. */
                    diag_emit_with_code(DIAG_ERROR, call->span,
                        TUR_E0015_TYPECLASS_CONSTRAINT_NOT_SATISFIED,
                        "no instance of typeclass '%s' applies to type '%s' "
                        "(method '.%.*s'). An instance is in scope but does not "
                        "match -- a parametric instance's own constraints must "
                        "also hold for the element type.",
                        cls, type_name(obj->type),
                        (int)method_name_len, method_name);
                }
                return NULL;
            }
        }
        /* No matching method found */
        diag_emit(DIAG_ERROR, call->span,
                  "no typeclass method found for '%.*s'",
                  method_name_len, method_name);
        return NULL;
    }

    /* ambiguous-dispatch-error-quality + method-dispatch-missing-instance:
     * when the receiver's static type is a genuinely DISTINCT concrete type and
     * NO instance matches it exactly, the true cause is "no instance of <Class>
     * for <that type>" -- regardless of how many carrier-compatible fallbacks
     * exist.  A positive whitelist (bool / cstr / float / sized ints / a TY_ADT
     * with a real def -- structs, opaque newtypes, user ADTs), NOT merely "not a
     * tyvar".  Two bugs collapse here:
     *   - fallback_count > 1: the misleading TUR-E0020 "ambiguous / Show[?] /
     *     annotate" wording (this report).
     *   - fallback_count == 1: a SILENT bind to the single carrier-compatible
     *     representative -- a wrong-instance dispatch that SIGSEGVs when the
     *     representative's layout differs (method-dispatch-missing-instance-
     *     falls-back-to-carrier-representative.md).
     * The bare int64 carrier (:int) is EXCLUDED (a `:int` receiver is
     * indistinguishable from an erased value -- errors/hkt-dispatch-ambiguous),
     * as are abstract tyvars / carrier-erased element reads (obj_is_abstract_-
     * tyvar) and null-def ADTs (abstract-tyvar stand-ins).  EXACT matches set
     * exact_match_found and never reach here, so the recursive self-type case
     * (a Debug[Tree] body calling .debug on a Tree subfield) still resolves to
     * self. */
    {
        TypeKind rrk = obj->type.kind;
        bool concrete_distinct_receiver = !obj_is_abstract_tyvar && (
            rrk == TY_BOOL   || rrk == TY_CSTR   || rrk == TY_FLOAT  ||
            rrk == TY_FLOAT32|| rrk == TY_NIL    || rrk == TY_SYM    ||
            rrk == TY_INT8   || rrk == TY_INT16  || rrk == TY_INT32  ||
            rrk == TY_UINT8  || rrk == TY_UINT16 || rrk == TY_UINT32 ||
            rrk == TY_UINT64 ||
            (rrk == TY_ADT && obj->type.as.adt_.def != NULL));
        if (!exact_match_found && fallback_count >= 1 &&
            concrete_distinct_receiver) {
            const char *cn = best_inst ? best_inst->typeclass->name->name : "?";
            /* Attribute a macro-emitted `.method` call (e.g. derive-show's
             * `(.show (.field __p))`) to the USER's derive call site rather than
             * the macro body in stdlib/macros.tur; a direct user call keeps its
             * own receiver span.  Scoped to THIS diagnostic. */
            Span err_span = call->as.list.items[1]->span;
            bool from_macro = e->macro_expand_depth > 0;
            if (from_macro) err_span = e->macro_call_site_span;
            diag_emit_with_code(DIAG_ERROR, err_span,
                                TUR_E0015_TYPECLASS_CONSTRAINT_NOT_SATISFIED,
                                "no instance of typeclass '%s' for type '%s' "
                                "(method '.%.*s'). Add (definstance %s [%s] ...) "
                                "or dispatch on a type that has one.",
                                cn, type_name(obj->type),
                                (int)method_name_len, method_name,
                                cn, type_name(obj->type));
            if (from_macro)
                diag_emit(DIAG_NOTE, call->as.list.items[1]->span,
                          "the '.%.*s' call is emitted by this macro expansion",
                          (int)method_name_len, method_name);
            return NULL;
        }
    }

    /* typeclass-dispatch-on-any-receiver-emits-uncompilable-c: an `any` receiver
     * never dispatches, however many instances matched.
     *
     * The ambiguity guard below counts CANDIDATES, so with exactly one instance
     * nothing fired and resolution took it -- without ever asking whether the
     * receiver was that type.  It is not: an `any` is a two-word `tur_tagged_t`
     * box, so the instance impl received the box where it declared the payload,
     * and cc rejected the call.  One candidate is not evidence that the
     * candidate is right.
     *
     * Checked before the count, so an `any` receiver gets this message rather
     * than the ambiguity one -- which named only `@TypeName` and left out the
     * two routes that have always worked. */
    /* typeclass-dispatch-on-any-receiver-emits-uncompilable-c: an `any` receiver
     * never dispatches to an instance declared for some other type.
     *
     * Nothing rejected it before.  An `any` carries no kind the matcher
     * recognises, so it took the KIND_ARROW arm, where `type_ok` is
     * "the instance head is not primitive" -- and every struct/ADT instance
     * satisfies that.  So the FIRST such instance was taken as an EXACT match
     * (fallback_count 0, so the ambiguity guard below never even looked), and
     * the instance impl was then called with a two-word `tur_tagged_t` where it
     * had declared the payload type.  cc rejected the call; with one instance
     * in scope there was no diagnostic at all beforehand.
     *
     * Keyed on the SELECTED instance rather than on match bookkeeping, so it
     * holds however dispatch got there -- and so a genuine `definstance C [any]`
     * still resolves, which is the one case where an `any` receiver is exactly
     * what the instance asked for. */
    if (obj && obj->type.kind == TY_ANY && best_inst &&
        (best_inst->dyn_any_minted ||
         !(best_inst->n_type_args > 0 &&
           best_inst->type_args[0].kind == TY_ANY))) {
        /* saffron-lang-plan S9 (D8 piece 4): in Saffron this is not the end of
         * the road -- it is the whole point.  The class and the method SLOT are
         * static (the name resolved), so only the instance is undecidable here,
         * and the box's tag decides it at runtime through the registry S9 piece
         * 3 publishes.
         *
         * The diagnostic below stays for Turmeric, where deferring a decision to
         * runtime would be the wrong default: there `narrow it first` really is
         * the answer.  So the two dialects differ in what they do with the same
         * resolution state, not in how they reach it. */
        if (lang_span_is_dynamic(call->span)) {
            TypeClass *tc = best_inst->typeclass;
            uint8_t slot = 0;
            bool found_slot = false;
            for (uint8_t i = 0; i < tc->n_methods; i++) {
                if (tc->methods[i].name->len == method_name_len &&
                    memcmp(tc->methods[i].name->name, method_name,
                           method_name_len) == 0) {
                    slot = i; found_slot = true; break;
                }
            }
            if (found_slot) {
                /* D8 Q1: the registry row this site dispatches through may be a
                 * CONSTRAINED instance -- `Eq [Vec]` -- whose spec runs at the
                 * all-`any` instantiation and discharges `(Eq A)` at `any`.
                 * Mint those dictionaries now, while the class is in hand. */
                for (TypeClassInstance *ci = e->typeclass_env.instances; ci; ci = ci->next)
                    if (typeclass_same_class(ci->typeclass, tc))
                        saffron_mint_constraint_any_instances(e, ci, call->span);
                uint32_t n_extra = call->as.list.len - 2;
                Expr **extra = n_extra
                    ? (Expr **)arena_alloc(e->arena, n_extra * sizeof(Expr *))
                    : NULL;
                for (uint32_t i = 0; i < n_extra; i++) {
                    extra[i] = elab_form(e, call->as.list.items[2 + i]);
                    if (!extra[i]) return NULL;
                    /* D8 Q3: every extra argument crosses the dispatch as a BOX
                     * -- the shim's signature is `(int64_t, tur_tagged_t, ...)`
                     * for every instance -- so a concrete one is widened here,
                     * exactly as a dyn call's arguments are. */
                    if (extra[i]->type.kind != TY_ANY && extra[i]->type.kind != TY_NEVER)
                        extra[i] = elab_coerce_to_any(e, extra[i]);
                }
                /* D8 Q3 -- a PARAMETRIC (HKT) receiver: key the registry on the
                 * head constructor, as directed.
                 *
                 * An HKT instance's receiver is the CONSTRUCTOR (`Option`) while
                 * a box's tag is an APPLIED type, so no ground row can serve it.
                 * The row that can is one per instantiation -- and in Saffron
                 * there is exactly one that matters: the all-`any` one, because
                 * the parametric-ctor widen builds every Saffron-side value at
                 * `(Option any)`.  A Turmeric-built `(Option float)` handed
                 * across still has no row and panics cleanly, by design: its
                 * elements are raw floats and a Saffron closure expects boxes.
                 *
                 * What the row calls is a WITNESS defn synthesised here, not a
                 * hand-rolled C shim:
                 *
                 *   (defn __dynwit_Functor_fmap_Option
                 *     [__r : (Option any) __a1] : any (.fmap __r __a1))
                 *
                 * elaborated at global scope in this Saffron span.  That single
                 * form buys everything the hard part needed: `.fmap` on a
                 * CONCRETE `(Option any)` resolves statically, so the ABI scan
                 * mints the by-value spec for that instantiation (the carrier
                 * base would read the 16-byte element as an int64); `__a1` is
                 * `any` by the dialect default, so the D5 seam inserts the
                 * checked unbox to `(fn [any] any)`; and the `: any` return
                 * re-tags the result with the id of `(Option any)`.  The emitter
                 * (emit_instance_dyn_table) then writes a two-line C shim that
                 * unboxes the receiver word and calls the witness.  Memoised per
                 * (instance, slot) so a program with many `.fmap` sites gets one
                 * witness per instance. */
                bool tc_is_hkt = false;
                if (tc->type_param_kinds)
                    for (uint8_t ki = 0; ki < tc->n_type_params; ki++)
                        if (tc->type_param_kinds[ki] != KIND_STAR) { tc_is_hkt = true; break; }
                if (tc_is_hkt) {
                    for (TypeClassInstance *wi = e->typeclass_env.instances; wi; wi = wi->next) {
                        if (wi->typeclass != tc || wi->n_type_args == 0) continue;
                        Type h = wi->type_args[0];
                        /* saffron-dynamic-surface-pass M2, second pass: a
                         * partially-applied head (`Functor [(Result _ B)]`)
                         * mints a witness too, at the all-`any` instantiation
                         * of its constructor -- but ONLY when the method body
                         * is by-value-expressible, the same gate the M2
                         * head-tyvar collection applies at dispatch.  The two
                         * gates are one condition seen from two sides: the
                         * collection grounds the witness's inner `.fmap` to a
                         * by-value spec exactly when this admits the witness,
                         * so no witness is ever minted whose dispatch would
                         * ride the erased carrier and tag its result with an
                         * unresolved id (the silent `false` from `is?` that
                         * sank the first attempt).  A carrier-bodied method on
                         * such a head gets no witness and its row's slot stays
                         * NULL -- a clean panic. */
                        bool w_partial = (h.kind == TY_APP);
                        while (h.kind == TY_APP && h.as.app.fn) h = *h.as.app.fn;
                        if (h.kind != TY_ADT || !h.as.adt_.def ||
                            h.as.adt_.def->n_type_params == 0) continue;
                        if (w_partial) {
                            FnDef *pimpl = (slot < wi->n_method_impls)
                                               ? wi->method_impls[slot] : NULL;
                            const TypeClassMethod *pcm = &tc->methods[slot];
                            bool p_applied = pcm->return_type.kind == TY_APP;
                            bool p_bare    = pcm->return_type.kind == TY_TYVAR;
                            bool p_body_ok = pimpl && pimpl->body &&
                                pimpl->body->kind != EX_INLINE_C &&
                                ((p_applied && m7_body_constructs_byvalue(pimpl->body)) ||
                                 (p_bare && m7_body_returns_byvalue_element(pimpl->body)));
                            if (!p_body_ok) continue;
                        }
                        AdtDef *def = h.as.adt_.def;
                        Span sp = call->span;
                        const Symbol *anys  = symtab_intern(e->st, strslice("any", 3));
                        const Symbol *heads = symtab_intern(e->st,
                            strslice(def->name, (uint32_t)strlen(def->name)));
                        /* (Head any ... any) -- the all-`any` instantiation. */
                        uint32_t ntp = def->n_type_params;
                        Form **ti = (Form **)arena_alloc(e->arena, (1 + ntp) * sizeof(Form *));
                        ti[0] = form_sym(e->arena, sp, heads);
                        for (uint32_t k = 0; k < ntp; k++) ti[1 + k] = form_sym(e->arena, sp, anys);
                        Form *recv_ty = form_list(e->arena, sp, ti, 1 + ntp);
                        saffron_mint_dyn_witness(e, tc, slot, wi, def, recv_ty, def->name,
                                                 method_name, method_name_len, sp, NULL);
                    }
                }
                /* saffron-dynamic-surface-pass M9: a kind-* method that takes
                 * MORE than the receiver (`near? [x : a y : a]`), or returns the
                 * class variable (`clone : a -> a`), used to get a NULL slot --
                 * "cannot be dispatched dynamically yet" -- because the v0 shim
                 * could unbox only the receiver word.  It is exactly the shape
                 * the HKT witness above already answers: a source-level defn
                 * whose extras are `any` (so the seam does the checked unbox to
                 * the impl's parameter type, per instance) and whose `: any`
                 * return re-tags the result.  So mint the same witness for
                 * every instance of the class whose receiver can be SPELLED as
                 * a type form -- a primitive or a non-parametric ADT; anything
                 * else keeps its NULL slot and the clean panic.  The node's
                 * result becomes `any`, as for an HKT class: a one-parameter
                 * concrete-result method keeps the direct shim and its typed
                 * result, so `.show` / `.hash` are untouched. */
                bool star_witness = false;
                if (!tc_is_hkt && slot < tc->n_methods) {
                    const TypeClassMethod *cm = &tc->methods[slot];
                    /* saffron-applied-class-var-result-takes-one-instances-type:
                     * a result that MENTIONS the class variable -- bare
                     * (`clone : a`) or applied (`wrap-self : (Option a)`) -- is
                     * a different type per instance, so no one slot signature
                     * carries it; each instance's witness returns `any` and
                     * tags its own instantiation.  Keyed on a bare tyvar alone,
                     * an applied one took the direct shim and the node took
                     * one instance's type for every instance. */
                    star_witness = cm->n_params > 1 ||
                                   saffron_type_mentions_tyvar(&cm->return_type);
                    /* ...but a RETURN-directed method (`(wrap-self [x] :
                     * (Option a))` -- `a` in the result and in no parameter,
                     * the unannotated `x` being `int`) is not selected by its
                     * receiver at all, and its instances' impls take that `int`:
                     * a witness keyed on the receiver's type cannot call them
                     * (cc: "incompatible type for argument 1"), and the direct
                     * shim it replaced mis-tagged every result.  Typed code
                     * refuses the static form of this call too, so say so
                     * here.  A bare `: a` result keeps its M9 path. */
                    if (cm->return_type.kind != TY_TYVAR &&
                        saffron_type_mentions_tyvar(&cm->return_type) &&
                        method_is_return_dispatch(tc, cm)) {
                        diag_emit_with_code(DIAG_ERROR, call->span,
                            TUR_E0020_AMBIGUOUS_DISPATCH,
                            "cannot dispatch '.%.*s' on an 'any' receiver: '%s' "
                            "names its class variable only in the result, so the "
                            "instance is chosen by the expected result type, not "
                            "by the receiver",
                            (int)method_name_len, method_name, tc->name->name);
                        diag_emit(DIAG_HELP, call->span,
                            "to dispatch on the receiver, spell it in the class: "
                            "`(%.*s [%s : %s] ...)`",
                            (int)method_name_len, method_name,
                            (cm->n_params > 0 && cm->param_names && cm->param_names[0])
                                ? cm->param_names[0]->name : "x",
                            tc->type_params[0] ? tc->type_params[0]->name : "a");
                        return NULL;
                    }
                }
                /* S9 (D8 Q1): a kind-* class's PARAMETRIC head (`Eq [Vec]`,
                 * `Show [Vec]`).  Its registry row is keyed on the all-`any`
                 * instantiation, `(Vec any)`, but a direct shim calls the
                 * CARRIER base impl, where a constrained body's element call is
                 * the elaborator's representative -- `Kind [int]` for every
                 * element, a silent wrong answer.  A witness at `(Vec any)` is
                 * a static call on that instantiation, so the ABI scan mints
                 * the by-value spec and the element call re-resolves to the
                 * `C [any]` dictionary.  With extras or a class-variable result
                 * it is the ordinary `any`-returning witness; for a one-
                 * parameter concrete-result method it returns the declared
                 * result, so the slot keeps every other instance's signature.
                 * An unconstrained parametric instance keeps its direct shim:
                 * its base impl never dispatches on the element. */
                if (!tc_is_hkt && slot < tc->n_methods) {
                    for (TypeClassInstance *wi = e->typeclass_env.instances; wi; wi = wi->next) {
                        if (wi->typeclass != tc || wi->n_type_args == 0) continue;
                        Type ht = wi->type_args[0];
                        if (ht.kind != TY_ADT || !ht.as.adt_.def ||
                            ht.as.adt_.def->n_type_params == 0)
                            continue;
                        Form *ret_form = NULL;
                        if (!star_witness) {
                            if (!wi->type_param_constraints ||
                                wi->n_type_param_constraints == 0)
                                continue;
                            const Type *crt = &tc->methods[slot].return_type;
                            FnDef *wimpl = (slot < wi->n_method_impls)
                                               ? wi->method_impls[slot] : NULL;
                            if (wimpl && wimpl->binding &&
                                wimpl->binding->type.kind == TY_FN &&
                                wimpl->binding->type.as.fn.result_full_type)
                                crt = wimpl->binding->type.as.fn.result_full_type;
                            const char *pn = NULL;
                            switch (crt->kind) {
                                case TY_INT: case TY_FLOAT: case TY_BOOL: case TY_CSTR:
                                case TY_SYM:
                                    pn = type_name(*crt); break;
                                default: break;
                            }
                            if (pn)
                                ret_form = form_sym(e->arena, call->span,
                                    symtab_intern(e->st, strslice(pn, (uint32_t)strlen(pn))));
                            else
                                ret_form = type_to_form(e, crt, call->span);
                            if (!ret_form) continue;
                        }
                        AdtDef *pdef = ht.as.adt_.def;
                        Span sp = call->span;
                        const Symbol *anys = symtab_intern(e->st, strslice("any", 3));
                        uint32_t ntp = pdef->n_type_params;
                        Form **ti = (Form **)arena_alloc(e->arena, (1 + ntp) * sizeof(Form *));
                        ti[0] = form_sym(e->arena, sp, symtab_intern(e->st,
                            strslice(pdef->name, (uint32_t)strlen(pdef->name))));
                        for (uint32_t k = 0; k < ntp; k++) ti[1 + k] = form_sym(e->arena, sp, anys);
                        Form *recv_ty = form_list(e->arena, sp, ti, 1 + ntp);
                        saffron_mint_dyn_witness(e, tc, slot, wi, pdef, recv_ty, pdef->name,
                                                 method_name, method_name_len, sp, ret_form);
                    }
                }
                if (star_witness) {
                    for (TypeClassInstance *wi = e->typeclass_env.instances; wi; wi = wi->next) {
                        if (wi->typeclass != tc || wi->n_type_args == 0) continue;
                        Type rt = wi->type_args[0];
                        const char *rn = NULL;
                        switch (rt.kind) {
                            case TY_INT: case TY_FLOAT: case TY_BOOL: case TY_CSTR:
                            case TY_SYM:
                            case TY_INT8: case TY_INT16: case TY_INT32: case TY_INT64:
                            case TY_UINT8: case TY_UINT16: case TY_UINT32: case TY_UINT64:
                            case TY_FLOAT32: case TY_FLOAT64:
                                rn = type_name(rt); break;
                            case TY_ADT:
                                if (rt.as.adt_.def && rt.as.adt_.def->n_type_params == 0 &&
                                    !rt.as.adt_.def->is_heap)
                                    rn = rt.as.adt_.def->name;
                                break;
                            default: break;
                        }
                        if (!rn) continue;
                        Span sp = call->span;
                        Form *recv_ty = form_sym(e->arena, sp,
                            symtab_intern(e->st, strslice(rn, (uint32_t)strlen(rn))));
                        saffron_mint_dyn_witness(e, tc, slot, wi, NULL, recv_ty, rn,
                                                 method_name, method_name_len, sp, NULL);
                    }
                }
                /* The result type comes from the METHOD's declaration, which is
                 * the same for every instance -- that is what makes one slot
                 * callable through one signature.  For an HKT class -- and for
                 * a kind-* method served by a witness (M9) -- the witness
                 * returns `any`, so the node is `any` regardless of the
                 * declaration (whose result mentions the class variable).
                 *
                 * S9: a class method must declare its result, so when that
                 * result is CONCRETE read it from the class -- not from the
                 * instance the static resolver happened to pick, which
                 * answered for every instance only because the instances
                 * agreed.  One that mentions the class variable at all is a
                 * star witness above, so the instance fallback below is for a
                 * declaration that elaborated to nothing usable. */
                Type result_type = TYPE_INT;
                if (tc_is_hkt || star_witness) result_type = type_from_kind(TY_ANY);
                bool class_result = false;
                if (!tc_is_hkt && !star_witness && slot < tc->n_methods) {
                    Type crt = tc->methods[slot].return_type;
                    if (crt.kind != TY_UNKNOWN && !saffron_type_mentions_tyvar(&crt)) {
                        result_type = crt;
                        class_result = true;
                    }
                }
                if (!tc_is_hkt && !star_witness && !class_result && best_method &&
                    best_method->binding && best_method->binding->type.kind == TY_FN) {
                    Type rt = best_method->binding->type.as.fn.result_full_type
                                  ? *best_method->binding->type.as.fn.result_full_type
                                  : type_from_kind(
                                        best_method->binding->type.as.fn.result_kind);
                    if (rt.kind != TY_UNKNOWN) result_type = rt;
                }
                Expr *dm = expr_new(e->arena, EX_DYN_METHOD, result_type, call->span);
                dm->as.dyn_method_.obj = obj;
                dm->as.dyn_method_.tc = tc;
                dm->as.dyn_method_.method_idx = slot;
                dm->as.dyn_method_.args = extra;
                dm->as.dyn_method_.n_args = n_extra;
                return elab_hoist_control_operands(e, dm);
            }
        }
        diag_emit_with_code(DIAG_ERROR, call->span,
                            TUR_E0020_AMBIGUOUS_DISPATCH,
                            "cannot dispatch '.%.*s' on an 'any' receiver: the "
                            "box holds one type at runtime, and which instance "
                            "to run is not decidable from it here",
                            (int)method_name_len, method_name);
        diag_emit(DIAG_HELP, call->span,
                  "narrow it first -- `(if (is? x T) (.%.*s x) ...)` -- or unbox "
                  "with `(cast x T)`, or pin the instance with a type witness: "
                  "`(.%.*s @T x)`",
                  (int)method_name_len, method_name,
                  (int)method_name_len, method_name);
        return NULL;
    }

    /* Phase D0: Ambiguous dispatch diagnostic.
     * If we reached here via the fallback path (no exact type match) and
     * more than one instance matched by name, emit TUR_E0020 so the user
     * gets a clear error instead of a silent wrong-instance selection. */
    if (!exact_match_found && fallback_count > 1) {
        /* stdlib-hkt-consolidation T1: if exactly one of the ambiguous
         * candidates is a user-defined (non-stdlib) instance, it shadows the
         * stdlib instance(s) and dispatch resolves to it instead of erroring. */
        if (user_fallback_count == 1 && user_fallback_method) {
            best_method = user_fallback_method;
            best_inst = user_fallback_inst;
            goto resolved_user_fallback;
        }
        /* rank2-class-float-float32-ambiguous: an ABSTRACT type-variable
         * receiver in a constrained generic is never ambiguous -- the instance
         * that runs comes from the dictionary (or per-spec re-resolution), and
         * best_inst is only the base clone's representative.  The carrier tier
         * above finds one when the class has an int-like instance; a class
         * whose instances are all floats or aggregates (`float` and
         * `float32`) fell through to here and was refused outright.  Keep the
         * first candidate the search recorded. */
        if (obj_is_abstract_tyvar && best_method && best_inst) {
            exact_match_found = true;
            goto resolved_user_fallback;
        }
        /* A RETURN-directed method reached through the dot form.
         *
         * `.m` means "dispatch on the first argument", which is the wrong
         * question for a method whose class variable appears only in the return
         * type: `pure`'s first argument is the payload, not the class type, so
         * the receiver is an erased int64 and every instance matches by name.
         * That is what made `for` unusable against the auto-loaded stdlib -- its
         * desugaring emits `.pure` inside a `fn` body, and with two Applicative
         * instances loaded (Schema and Option) every use failed with the
         * ambiguity below, from a call site inside the macro where no caller
         * could annotate.
         *
         * The expected type is available here even though the receiver is not
         * (`bind`'s signature pins the lambda's result), so ask the
         * return-directed dispatcher instead of guessing from the receiver.
         * Gated on an expected type being present so a genuinely
         * unresolvable case still gets the ambiguity error below rather than
         * "cannot infer type for return-directed method", which would be a
         * worse message for the same program.
         * See docs/archive/for-comprehension-pure-ambiguous-against-stdlib.md. */
        if (e->expected_type) {
            const Symbol *m_sym =
                symtab_intern(e->st, strslice(method_name, method_name_len));
            if (m_sym && elab_symbol_is_return_dispatch_method(e, m_sym)) {
                bool rt_handled = false;
                Expr *rt = elab_try_return_dispatch(e, call, m_sym, &rt_handled);
                if (rt) return rt;
            }
        }
        if (unsat_inst && obj && obj->type.kind == TY_APP) {
            const char *cls = unsat_inst->typeclass && unsat_inst->typeclass->name
                                  ? unsat_inst->typeclass->name->name : "?";
            diag_emit_with_code(DIAG_ERROR, call->span,
                TUR_E0015_TYPECLASS_CONSTRAINT_NOT_SATISFIED,
                "no instance of typeclass '%s' applies to type '%s' "
                "(method '.%.*s'). An instance is in scope but does not "
                "match -- a parametric instance's own constraints must "
                "also hold for the element type.",
                cls, type_name(obj->type), (int)method_name_len, method_name);
            return NULL;
        }
        /* Build a comma-separated list of matching instance names for the message,
         * and capture the typeclass name for the concrete-receiver diagnosis. */
        char inst_list[512];
        int pos = 0;
        int listed = 0;
        const char *class_name = NULL;
        for (TypeClassInstance *ci = e->typeclass_env.instances;
             ci != NULL && pos < (int)sizeof(inst_list) - 2; ci = ci->next) {
            for (uint8_t mi = 0; mi < ci->typeclass->n_methods; mi++) {
                const TypeClassMethod *cm = &ci->typeclass->methods[mi];
                if (cm->name->len != method_name_len ||
                    memcmp(cm->name->name, method_name, method_name_len) != 0) continue;
                if (!class_name) class_name = ci->typeclass->name->name;
                if (listed > 0 && pos < (int)sizeof(inst_list) - 3) {
                    inst_list[pos++] = ','; inst_list[pos++] = ' ';
                }
                int wrote = 0;
                if (ci->n_type_args > 0 && ci->type_arg_syms && ci->type_arg_syms[0]) {
                    wrote = snprintf(inst_list + pos, sizeof(inst_list) - (size_t)pos,
                                     "%s[%s]", ci->typeclass->name->name,
                                     ci->type_arg_syms[0]->name);
                } else {
                    wrote = snprintf(inst_list + pos, sizeof(inst_list) - (size_t)pos,
                                     "%s[?]", ci->typeclass->name->name);
                }
                if (wrote > 0) pos += wrote;
                listed++;
                break; /* one method match per instance is enough */
            }
        }
        inst_list[pos] = '\0';
        (void)class_name;
        diag_emit_with_code(DIAG_ERROR, call->span, TUR_E0020_AMBIGUOUS_DISPATCH,
                            "ambiguous method dispatch: '.%.*s' matches %d instances "
                            "(%s) -- receiver type is erased (int64_t). "
                            "Hint: annotate the receiver's type or use @TypeName syntax (see D1).",
                            (int)method_name_len, method_name, fallback_count, inst_list);
        return NULL;
    }
resolved_user_fallback:;

    /* obj was already elaborated above for dispatch; elaborate the remaining args. */
    uint32_t n_args = call->as.list.len - 2;
    Expr **args = (Expr **)arena_alloc(e->arena, n_args * sizeof(Expr *));
    for (uint32_t i = 0; i < n_args; i++) {
        args[i] = elab_form(e, call->as.list.items[2 + i]);
        if (!args[i]) return NULL;
    }

    /* Arrow-identity passthrough: capture the pre-shim argument types so a
     * method whose body returns one of its (fat) parameters verbatim -- e.g.
     * `(arr [f] f)` under `Arrow [(->)]` -- can recover the concrete arrow
     * signature (and thus the real arity) of the argument it passes through.
     * Later passes (poly-wrap, arrow_fat_shim) rewrite obj/args into opaque
     * fat boxes, erasing the TY_FN type, so snapshot it now. */
    Type obj_orig_type = obj->type;
    Type *args_orig_types = n_args
        ? (Type *)arena_alloc(e->arena, n_args * sizeof(Type)) : NULL;
    for (uint32_t i = 0; i < n_args; i++) args_orig_types[i] = args[i]->type;

    /* static-instance-spec-calls-any-lambda-as-concrete-result: a function
     * argument whose `any` slots disagree with the method's parameter AT THIS
     * CALL -- a Saffron `(fn [acc x] ...)`, all-`any`, handed to Foldable's
     * `f : (fn [b a] b)` with `b := float` from `init` -- is called by the
     * instance spec through the parameter's signature, so a `tur_tagged_t`
     * result was read out of `xmm0`.  Marshal it with the `any` bridge before
     * the poly-fn packing below sees it.  The tyvars are solved exactly as
     * the spec's own bindings are (receiver first, then the arguments in
     * order, first binding wins), so the adaptor and the spec agree. */
    if (best_inst && best_inst->typeclass && n_args > 0) {
        const TypeClassMethod *bcm = NULL;
        TypeClass *btc = best_inst->typeclass;
        for (uint8_t mi = 0; mi < btc->n_methods; mi++)
            if (btc->methods[mi].name &&
                strcmp(btc->methods[mi].name->name, method_name) == 0) {
                bcm = &btc->methods[mi];
                break;
            }
        bool any_fn_arg = false;
        for (uint32_t i = 0; i < n_args && bcm; i++)
            if (args[i]->type.kind == TY_FN && 1 + i < bcm->n_params &&
                bcm->param_types[1 + i].kind == TY_FN)
                any_fn_arg = true;
        if (bcm && any_fn_arg && bcm->n_params >= 1) {
            const Symbol *bn[16];
            Type bt[16];
            uint8_t nb = 0;
            m7_collect_tyvar_bindings(e, bcm->param_types[0], obj->type,
                                      bn, bt, &nb, 16);
            for (uint32_t i = 0; i < n_args && 1 + i < bcm->n_params; i++)
                m7_collect_tyvar_bindings(e, bcm->param_types[1 + i],
                                          args_orig_types[i], bn, bt, &nb, 16);
            AbiTypeBinding ab[16];
            for (uint8_t k = 0; k < nb; k++) {
                ab[k].name = bn[k]->name;
                ab[k].type = bt[k];
            }
            for (uint32_t i = 0; i < n_args && 1 + i < bcm->n_params; i++) {
                if (args[i]->type.kind != TY_FN ||
                    bcm->param_types[1 + i].kind != TY_FN) continue;
                /* The target is the signature the INSTANCE body calls the
                 * parameter through -- its binding's poly_type -- not the
                 * class's.  A Saffron instance body reads Foldable's `(fn [b
                 * a] b)` as `(fn [any any] b)`, and the adaptor must take
                 * what that body passes. */
                const Type *target = &bcm->param_types[1 + i];
                if (best_method && 1 + i < best_method->n_params &&
                    best_method->params[1 + i] &&
                    best_method->params[1 + i]->poly_type &&
                    best_method->params[1 + i]->poly_type->kind == TY_FN)
                    target = best_method->params[1 + i]->poly_type;
                /* A Saffron instance body ascribes a bare-tyvar argument of
                 * that call to `any` (D8 Q3), so read the target the same
                 * way. */
                Type saffron_view;
                if (best_method && 1 + i < best_method->n_params &&
                    best_method->params[1 + i] &&
                    lang_span_is_dynamic(best_method->params[1 + i]->span)) {
                    saffron_view = elab_saffron_call_view(e, target);
                    target = &saffron_view;
                }
                Expr *ad = elab_fn_any_bridge(e, args[i], target, ab, nb);
                if (ad) {
                    args[i] = ad;
                    args_orig_types[i] = ad->type;
                }
            }
        }
    }

    /* F3-5 (cross-plan-followups): per-call-site synthesis for the
     * recursive case of typed-collection `.eq?` dispatch.  When the
     * outer instance is a constrained typed-collection (e.g. Eq[Vec])
     * and the receiver's element type is itself a TY_APP (e.g.
     * Vec[Vec[int]]), bypass the constrained instance's hardcoded
     * `(fn [a b] (= a b))` body and synthesise a direct call to the
     * helper (vec-eq?) with an inline comparator lambda whose
     * params are ascribed to the element type.  The inner `.eq?`
     * re-enters this dispatcher at the next level, terminating at
     * primitives where F3-7 takes over. */
    if (best_inst &&
        best_inst->n_type_param_constraints > 0 &&
        method_name_len == 3 &&
        memcmp(method_name, "eq?", 3) == 0 &&
        n_args == 1) {
        Expr *synth = try_synth_recursive_eq(e, best_inst, obj, args[0], call->span);
        if (synth) return synth;
    }

    /* Phase HRT3/HRT4: For methods with rank-N (poly fn) parameters, wrap matching args
     * as EX_POLY_WRAP so they can be passed as tur_poly_fn_t.
     * params[0] is the receiver (obj), so method param i+1 matches arg i.
     * Phase HRT4: if the arg is already is_poly_fn, use pass-through (wrapper_binding=NULL). */
    bool has_poly_params = false;
    /* Check params[0] which corresponds to obj (the first/receiver argument). */
    if (best_method->n_params > 0 && best_method->params[0]->is_poly_fn) {
        has_poly_params = true;
        Binding *inner_b = poly_arg_fn_binding(obj);
        if (!inner_b) {
            /* Phase CCL (symmetric with the args path below): a fat closure --
             * a capturing or non-capturing lambda (boxed TY_FN) or a ptr<void>
             * closure handle -- is wrapped for tur_poly_fn_t packing rather than
             * rejected.  Reached when a typed-fn element param lands in params[0]
             * (Bifunctor `bimap [g h x]`, whose first param is a mapper fn, not
             * the HKT receiver). */
            if (obj->type.kind == TY_PTR_VOID ||
                (obj->type.kind == TY_FN && obj->type.as.fn.boxed)) {
                Expr *cwrap = expr_new(e->arena, EX_POLY_WRAP, TYPE_PTR_VOID, obj->span);
                cwrap->as.poly_wrap_.inner = obj;
                cwrap->as.poly_wrap_.wrapper_binding = NULL;
                cwrap->as.poly_wrap_.is_closure = true;
                poly_wrap_stamp_carrier_erased(cwrap, best_method->params[0]);
                obj = cwrap;
            } else {
                diag_emit(DIAG_ERROR, call->as.list.items[1]->span,
                          "rank-N typeclass method argument must be a named function");
                return NULL;
            }
        } else {
            Expr *wrap = expr_new(e->arena, EX_POLY_WRAP, TYPE_PTR_VOID, obj->span);
            wrap->as.poly_wrap_.inner = obj;
            poly_wrap_stamp_carrier_erased(wrap, best_method->params[0]);
            if (inner_b->is_poly_fn) {
                wrap->as.poly_wrap_.wrapper_binding = NULL; /* HRT4: pass-through */
            } else if (!inner_b->is_global) {
                /* Receiver-position twin of the local-binding pass-through in
                 * the args loop below (Bifunctor `bimap [g h x]` puts a mapper
                 * fn in params[0]): a local cannot be named from a file-scope
                 * wrapper, so pack the runtime value inline. */
                wrap->as.poly_wrap_.wrapper_binding = NULL;
                wrap->as.poly_wrap_.is_closure = true;
            } else {
                uint32_t inner_arity = (inner_b->type.kind == TY_FN)
                    ? (uint8_t)inner_b->type.as.fn.arity : 1;
                Binding *wrapper_b = make_poly_wrapper(e, inner_b, inner_arity, obj->span, false);
                if (!wrapper_b) return NULL;
                wrap->as.poly_wrap_.wrapper_binding = wrapper_b;
            }
            obj = wrap;
        }
    }
    for (uint32_t i = 0; i < n_args; i++) {
        uint8_t param_idx = 1 + (uint8_t)i;  /* params[0] is the receiver */
        if (param_idx < best_method->n_params && best_method->params[param_idx]->is_poly_fn) {
            has_poly_params = true;
            Binding *inner_b = poly_arg_fn_binding(args[i]);
            if (!inner_b) {
                /* Phase CCL: no named-function binding found.  If the argument
                 * is a fat closure (TY_PTR_VOID, or CRU B-1's boxed TY_FN
                 * closure value — capturing or non-capturing lambda), wrap it
                 * for tur_poly_fn_t packing in the emitter. */
                if (args[i]->type.kind == TY_PTR_VOID ||
                    (args[i]->type.kind == TY_FN && args[i]->type.as.fn.boxed)) {
                    Expr *orig2 = args[i];
                    Expr *cwrap = expr_new(e->arena, EX_POLY_WRAP, TYPE_PTR_VOID, orig2->span);
                    cwrap->as.poly_wrap_.inner = orig2;
                    cwrap->as.poly_wrap_.wrapper_binding = NULL;
                    cwrap->as.poly_wrap_.is_closure = true;
                    poly_wrap_stamp_carrier_erased(cwrap, best_method->params[param_idx]);
                    args[i] = cwrap;
                    continue;
                }
                diag_emit(DIAG_ERROR, call->as.list.items[2 + i]->span,
                          "rank-N typeclass method argument must be a named function or closure");
                return NULL;
            }
            Expr *orig = args[i];
            Expr *wrap = expr_new(e->arena, EX_POLY_WRAP, TYPE_PTR_VOID, orig->span);
            wrap->as.poly_wrap_.inner = orig;
            poly_wrap_stamp_carrier_erased(wrap, best_method->params[param_idx]);
            /* constrained-hkt-byvalue-carriers: when the receiver is the ABSTRACT
             * type constructor of a constrained poly fn, this method call lowers to
             * a dictionary-slot dispatch, whose method pointer returns the int64
             * carrier -- and the instance impl invokes this `:fn` argument through
             * `((int64_t (*)(void*, int64_t))k.fn)(...)`.  A continuation returning
             * a by-value aggregate (e.g. `(fn [v] (some (dbl v)))` at `(Option
             * int)`) therefore had a struct-returning thunk cast to an
             * int64-returning pointer: an x86-64 return-ABI mismatch (RAX:RDX vs
             * RAX) that handed the instance garbage, which the caller then
             * dereferenced -- the Gap 2 segfault.
             *
             * Ask for the carrier-spill shim so the thunk boxes its aggregate
             * return, matching the carrier ABI on both sides.  Only the abstract
             * receiver opts in: a CONCRETE receiver resolves to the instance's own
             * by-value entry point and must keep consuming the struct directly,
             * which is what the existing gate protects.  The shim is itself
             * defensive -- ensure_aggregate_spill_shim returns NULL unless the
             * result really is a by-value aggregate -- so this is a no-op for
             * carrier-returning continuations. */
            {
                /* The receiver is `(m int)` -- a TY_APP spine headed by the
                 * abstract constructor -- not a bare TY_TYVAR, so walk to the
                 * head before comparing against this body's constraint var. */
                Type rcv = obj->type;
                while (rcv.kind == TY_APP && rcv.as.app.fn) rcv = *rcv.as.app.fn;
                bool rcv_is_ambient_ctor =
                    rcv.kind == TY_TYVAR && rcv.as.tyvar_.name &&
                    e->cur_hkt_constraint_tyvar &&
                    strcmp(rcv.as.tyvar_.name, e->cur_hkt_constraint_tyvar) == 0;
                /* catch-error-ascribed-result-types-handler-by-value: an
                 * INLINE-C instance body is the same carrier consumer -- it
                 * cannot be re-specialised, and `MonadError [(Result _ B)]`'s
                 * `catch-error` returns `handler.fn(handler.env, ...)` as the
                 * int64 word -- so a continuation whose result an enclosing
                 * ascription grounded to a by-value aggregate must box it
                 * too.  Without this, the handler's shim returned the struct
                 * and the caller dereferenced its first word as a pointer. */
                bool impl_is_inline_c = best_method && best_method->body &&
                                        best_method->body->kind == EX_INLINE_C;
                if (obj_is_abstract_tyvar || rcv_is_ambient_ctor || impl_is_inline_c)
                    wrap->as.poly_wrap_.boxes_aggregate = true;
            }
            if (inner_b->is_poly_fn) {
                wrap->as.poly_wrap_.wrapper_binding = NULL; /* HRT4: pass-through */
            } else if (!inner_b->is_global) {
                /* CRU: a *capturing closure VALUE* bound to a local reaching a
                 * `:fn` typeclass-method param.  make_poly_wrapper would emit a
                 * file-scope wrapper statically referencing the local env var
                 * (out of scope at file scope -> uncompilable C).  Pack the
                 * runtime closure inline instead; the is_closure emit path reads
                 * the thunk from the box's slot 0 at runtime, so a capturing
                 * closure round-trips correctly.
                 *
                 * local-fn-value-into-rank2-slot-gets-a-by-name-wrapper: this
                 * used to admit only a binding WITH a closure_fn_binding, so a
                 * fn-typed PARAMETER (`f : (fn [any] any)` forwarded into
                 * `fmap`) and a let-bound NON-capturing lambda still took the
                 * by-name wrapper and died with `'f' undeclared`.  Every
                 * non-global binding has the same problem and the same answer:
                 * the value in the local already IS what the carrier reads (a
                 * fat box for a parameter or capturing closure; the emitter's
                 * bare-fnptr shim covers the unboxed let-bound lambda). */
                wrap->as.poly_wrap_.wrapper_binding = NULL;
                wrap->as.poly_wrap_.is_closure = true;
            } else {
                uint32_t inner_arity = (inner_b->type.kind == TY_FN)
                    ? (uint8_t)inner_b->type.as.fn.arity : 1;
                Binding *wrapper_b = make_poly_wrapper(e, inner_b, inner_arity, args[i]->span, false);
                if (!wrapper_b) return NULL;
                wrap->as.poly_wrap_.wrapper_binding = wrapper_b;
            }
            args[i] = wrap;
        }
    }

    /* Arrow head (and any fat-closure method parameter): auto-shim a bare
     * (non-capturing) function argument into a fat-closure box, mirroring the
     * `^fat` coercion the regular call path applies (elab_call.c).  A capturing
     * closure (boxed TY_FN) or an already-fat :ptr<void> handle passes through.
     * params[0] is the receiver (obj); params[i+1] matches args[i]. */
    if (best_method->n_params > 0 && best_method->params[0] &&
        best_method->params[0]->is_fat) {
        obj = arrow_fat_shim(e, obj);
    }
    for (uint32_t i = 0; i < n_args; i++) {
        uint8_t pidx = 1 + (uint8_t)i;
        if (pidx < best_method->n_params && best_method->params[pidx] &&
            best_method->params[pidx]->is_fat) {
            args[i] = arrow_fat_shim(e, args[i]);
        }
    }

    /* A <: any, as elab_call_fn's argument loop spells it: an argument whose
     * instance parameter is `any` crosses as a BOX.  The static dispatch path
     * never did this, so a lambda handed to a Saffron `g : fn` extra (an `any`
     * parameter -- tc_fn_param_type) reached the `tur_tagged_t` slot as a bare
     * function pointer, and cc refused it (saffron-dyn-witness-fn-arity-
     * defaults-unary, the direct-dispatch half). */
    for (uint32_t i = 0; i < n_args; i++) {
        uint8_t pidx = 1 + (uint8_t)i;
        if (pidx < best_method->n_params && best_method->params[pidx] &&
            best_method->params[pidx]->type.kind == TY_ANY && args[i] &&
            args[i]->type.kind != TY_ANY && args[i]->type.kind != TY_NEVER) {
            Expr *w = elab_coerce_to_any(e, args[i]);
            if (w) args[i] = w;
        }
    }

    /* Allocate arguments array with obj prepended */
    Expr **call_args = (Expr **)arena_alloc(e->arena, (n_args + 1) * sizeof(Expr *));
    call_args[0] = obj;
    for (uint32_t i = 0; i < n_args; i++) {
        call_args[i + 1] = args[i];
    }
    
    /* Create a call to the method function */
    /* The result type is the return type of the method.
     * For inline-C bodies the body type is TYPE_NIL, so prefer the
     * declared return type from the method's binding function type. */
    Type result_type;
    if (best_method->binding->type.kind == TY_FN) {
        /* Arrow head: when the method returns a *boxed* closure value (a
         * callable `(fn [x] ...)` recovered from an arrow instance body), use
         * its full signature so a caller applying the result -- `(h 3)` --
         * sees the real arity instead of an arity-0 shell.  Closure-returning
         * methods that carry the int64 fat handle (an unboxed fn-typed return
         * annotation) keep the carrier ABI via type_from_kind below. */
        result_type = method_callable_result_type(
            best_method->binding,
            type_from_kind(best_method->binding->type.as.fn.result_kind));
        /* ECS E2d-P6 (value-level projection): recover the precise by-value
         * struct/ADT return (with its def) from the impl binding's
         * result_full_type, so a struct result carries its def to the call site
         * -- `(.field (method ...))` resolves and a let-bound `p : Pos` lowers
         * to the by-value struct instead of the def-less carrier kind.  Gated to
         * the same non-parametric nominal types the impl side threads above;
         * boxed-fn results are already handled by method_callable_result_type. */
        {
            const Type *rft = best_method->binding->type.as.fn.result_full_type;
            if (rft &&
                rft->kind == TY_ADT && rft->as.adt_.def &&
                rft->as.adt_.def->n_type_params == 0) {
                result_type = *rft;
            }
            /* ECS E2d-P6 (parametric associated-type element): the impl's
             * result_full_type is the instance head's own tyvar (`A`, threaded
             * above for `(definstance StorageOps [(Dense A)] (type Elem = A))`).
             * Ground it through the receiver: unify the matched instance's head
             * (`(Dense A)`) against the call-site receiver (`(Dense Pos)`) to bind
             * `A -> Pos`, then substitute into `A`.  When that grounds to a
             * non-parametric struct/ADT, commit it (with def) so a struct element
             * carries its precise type to the call site -- `(.field (storage-get
             * ...))` resolves and a struct round-trips by value through the spec
             * minted below (the abi_bindings attachment grounds the same `A`). */
            else if (rft && rft->kind == TY_TYVAR && rft->as.tyvar_.name &&
                     best_inst && best_inst->n_type_args >= 1) {
                const Symbol *hb_names[8];
                Type hb_types[8];
                uint8_t hb_n = 0;
                m7_collect_tyvar_bindings(e, best_inst->type_args[0],
                                          obj_orig_type, hb_names, hb_types,
                                          &hb_n, 8);
                if (hb_n > 0) {
                    Type grounded = elab_subst_class_tyvars(
                        e->arena, *rft, hb_names, hb_n, hb_types, hb_n);
                    if (grounded.kind == TY_ADT && grounded.as.adt_.def &&
                        grounded.as.adt_.def->n_type_params == 0) {
                        result_type = grounded;
                    }
                }
            }
        }
        /* Arrow-identity passthrough: the method's declared return is the arrow
         * class variable (a boxed arity-0 TY_FN shell) and its body is a bare
         * reference to one of the method's parameters -- so the dispatched
         * result *is* exactly that argument (e.g. `(arr [f] f)` returns `f`).
         * The arity-0 shell is uncallable (`(a 5)` => "returns ?"), so recover
         * the argument's concrete arrow signature here, where the call-site arg
         * type is known.  Force the boxed (thunk) convention: a fat parameter is
         * returned as a fat box, so the caller must apply it through TUR_APPLY*
         * rather than a direct fn-pointer call. */
        if (result_type.kind == TY_FN && result_type.as.fn.arity == 0 &&
            best_method->body && best_method->body->kind == EX_VAR) {
            const Binding *vb = best_method->body->as.var.binding;
            int passthru_idx = -1;
            for (uint32_t pi = 0; pi < best_method->n_params; pi++) {
                if (best_method->params[pi] == vb) { passthru_idx = pi; break; }
            }
            if (passthru_idx >= 0) {
                Type at = (passthru_idx == 0)
                    ? obj_orig_type
                    : args_orig_types[passthru_idx - 1];
                if (at.kind == TY_FN) {
                    at.as.fn.boxed = true;
                    result_type = at;
                }
            }
        }
    } else {
        result_type = best_method->body->type;
        if (result_type.kind == TY_UNKNOWN || result_type.kind == TY_NIL) {
            result_type = TYPE_INT;
        }
    }

    /* SC7 (chainable HKT return): a class method may declare a concrete `:int`
     * return (so the dictionary slot/ABI stay int64), yet its instance body
     * yields a transparent int-newtype container -- e.g. a Functor `fmap` whose
     * body is `(make-struct Schema (schema/fmap ...))` typed `(Schema b)`.
     * Propagate that container type as the call's result so the next HKT
     * operator (`ap`/`alt-or`) can dispatch on its `(Schema _)` head.  The C
     * representation is unchanged (a transparent newtype is int64 everywhere),
     * and instances whose bodies are bare `:int` carriers are unaffected. */
    if (best_method->body &&
        type_is_transparent_int_newtype(best_method->body->type)) {
        result_type = best_method->body->type;
    }

    /* let-bound-class-method-result-in-constrained-generic-truncates: on an
     * abstract type-variable receiver (`x : A` inside `[^N A]`), best_method is
     * the carrier REPRESENTATIVE, not the instance that will run, so its result
     * type (`int` for `N [int]`) is not the call's.  When the CLASS declares
     * both the receiver and the result as its own variable -- every
     * `a -> ... -> a` method -- the call's type is the receiver's `A`, exactly
     * as the return-directed path keeps the abstract tyvar (see
     * return-dispatch-tyvar above).  Without this a `let` binding took the
     * representative's `int`: `(let [y (n x x)] y)` stored the float instance's
     * double into an int64_t, and `(n y x)` then dispatched on a concrete int.
     * The base clone is unchanged -- `A` lowers to the same int64 carrier. */
    if (obj->type.kind == TY_TYVAR && e->definstance_depth == 0 &&
        best_inst && best_inst->typeclass &&
        best_inst->typeclass->n_type_params >= 1 &&
        best_inst->typeclass->type_params[0]) {
        const TypeClass *rtc = best_inst->typeclass;
        const char *cv = rtc->type_params[0]->name;
        for (uint8_t rmi = 0; rmi < rtc->n_methods; rmi++) {
            const TypeClassMethod *cm = &rtc->methods[rmi];
            if (!cm->name || strlen(cm->name->name) != method_name_len ||
                strncmp(cm->name->name, method_name, method_name_len) != 0)
                continue;
            bool recv_is_cv = cm->n_params >= 1 && cm->param_types &&
                cm->param_types[0].kind == TY_TYVAR &&
                cm->param_types[0].as.tyvar_.name &&
                strcmp(cm->param_types[0].as.tyvar_.name, cv) == 0;
            bool res_is_cv = cm->return_type.kind == TY_TYVAR &&
                cm->return_type.as.tyvar_.name &&
                strcmp(cm->return_type.as.tyvar_.name, cv) == 0;
            if (recv_is_cv && res_is_cv) result_type = obj->type;
            /* associated-type-unusable-nullary-and-generic (half 2): a result
             * that is one of the class's ASSOCIATED types -- `(unwrap [x : a]
             * : Inner)` -- is the projection at the receiver, `(Inner A)`, not
             * the representative's binding: that is what a parameter declared
             * `(Inner A)` holds, and what each instantiation reduces. */
            else if (recv_is_cv && cm->return_type.kind == TY_TYVAR &&
                     cm->return_type.as.tyvar_.name &&
                     obj->type.as.tyvar_.name && rtc->n_assoc_types > 0) {
                for (uint8_t ak = 0; ak < rtc->n_assoc_types; ak++) {
                    const Symbol *an = rtc->assoc_type_names[ak];
                    if (!an || strcmp(an->name, cm->return_type.as.tyvar_.name) != 0)
                        continue;
                    Type proj;
                    if (elab_assoc_projection(e, an, &obj->type, 1, &proj))
                        result_type = proj;
                    break;
                }
            }
            /* class-var-applied-result-untyped-in-constrained-generic: the
             * same rule for a result that mentions the class variable INSIDE
             * an application -- `(co [x : a] : (Option a))`.  The
             * representative's result kind is TY_APP with no structure, the
             * def-less `(? ?)` that unifies with anything: `(unwrap-or (co x)
             * x)` was refused, `(match (co x) (Some q) q ...)` bound `q` as
             * int, and a `: cstr` return of `(co x)` was accepted.  Kind-*
             * classes only: an HKT class's application head is a constructor
             * variable, which the M7 path below grounds. */
            else if (recv_is_cv && cm->return_type.kind == TY_APP &&
                     !(rtc->type_param_kinds &&
                       rtc->type_param_kinds[0] != KIND_STAR)) {
                const Symbol *cvs[1] = { rtc->type_params[0] };
                Type rt = elab_subst_class_tyvars(e->arena, cm->return_type,
                                                  cvs, 1, &obj->type, 1);
                const Type *hd = &rt;
                while (hd->kind == TY_APP && hd->as.app.fn) hd = hd->as.app.fn;
                if (hd->kind == TY_ADT && hd->as.adt_.def) result_type = rt;
            }
            break;
        }
    }

    /* Phase H §1 (dict load): Build an EX_DICT node that carries both the
     * singleton identity AND the method field name.  When fn_expr is this
     * node, emit.c dispatches through the dictionary struct at the call site:
     *   dict_<Class>_<type>_singleton.<method>(args...)
     * instead of a direct call to the impl function.  Fall back to the direct
     * binding if, for some reason, no instance was resolved. */
    Expr *dict_expr = NULL;
    if (best_inst) {
        dict_expr = make_dict_expr(e, best_inst, call->span);
        /* Mangle the method name into the EX_DICT node (must match the dict
         * field + instance-function spelling produced by the shared mangler). */
        tur_mangle_ident(method_name, dict_expr->as.dict_.method_name,
                         sizeof(dict_expr->as.dict_.method_name));
    }

    /* M7 HKT layers 1+3 (flag-gated): an HKT-applied method result `(Option b)`
     * arrives here as an empty `type_from_kind(TY_APP)` => `(type-app ? ?)`,
     * because the method's result_kind is TY_APP but type_from_kind drops the
     * head/element.  Recover the real result: take the binding's
     * result_full_type rft (`(Option b)`, with `b` a named element tyvar after
     * the parse fix + layer-0 head substitution), unify the declared parameter
     * types against the actual argument types to bind `b` (and any other element
     * tyvars), and substitute into rft so it grounds to `(Option int)`. */
    /* M7 layer-4 prep: persist the element-tyvar bindings collected here so they
     * can be attached as the dispatch call's abi_bindings below (the emit-side
     * per-(f, A) by-value spec interning reads them). */
    const Symbol *m7_bind_names[16];
    Type m7_bind_types[16];
    uint8_t m7_nb = 0;
    /* M7 layer-4 guard (see m7_type_has_free_tyvar): stays false unless the
     * substituted result type fully grounds, so an `ap`-style call whose result
     * element cannot be recovered falls back to carrier dispatch. */
    bool m7_byvalue_grounded = false;
    /* M6 / gap G6(c) recursive combinator: true when the method result element
     * was bound SYMBOLICALLY to an ungrounded tyvar (the enclosing generic's own
     * type param, e.g. `b -> B` for `fmap` inside `re-cata [B]`).  The result
     * does NOT ground here (B is free), but attaching the symbolic binding lets
     * the emit side compose `b -> B -> bool` once the enclosing generic is
     * monomorphized.  Kept distinct from m7_byvalue_grounded so the by-value
     * RESULT TYPE is NOT committed at elab (only the bindings are attached). */
    bool m7_symbolic_elem = false;
    /* M7 layer-4: is the instance body by-value-expressible (pure-Turmeric)?
     * Computed up front because it gates BOTH the by-value result-type commit
     * below AND the abi_bindings attachment further down -- the two MUST agree.
     * Committing a by-value result type for a CARRIER-bodied method (so the
     * consumer reads by value) without also interning the by-value spec (so the
     * producer returns by value) is exactly the carrier-vs-by-value mismatch
     * that silently miscompiles a selection body to 0 (Alternative `<|>`). */
    /* The result shape selects the by-value body criterion, and is read from the
     * CLASS method's declared return type -- not the instance binding, whose
     * result_full_type is NULL for a bare-element body (extract's `(.value w)`)
     * and only reliably TY_APP for a constructing body (fmap).  Applied `(f b)`
     * results must CONSTRUCT in-body (fmap/bind/ap/alt); a bare-element result
     * (`a`) must merely READ a scalar out of the by-value receiver (Comonad
     * `extract`).  An unmigrated stdlib class declaring a `: int` carrier return
     * classifies as neither -> stays on the uniform carrier dispatch. */
    const TypeClassMethod *m7_cm = NULL;
    if (best_inst && best_inst->typeclass) {
        TypeClass *tc0 = best_inst->typeclass;
        for (uint8_t mi = 0; mi < tc0->n_methods; mi++)
            if (tc0->methods[mi].name &&
                strcmp(tc0->methods[mi].name->name, method_name) == 0) {
                m7_cm = &tc0->methods[mi];
                break;
            }
    }
    bool m7_result_is_applied   = m7_cm && m7_cm->return_type.kind == TY_APP;
    bool m7_result_is_bare_elem = m7_cm && m7_cm->return_type.kind == TY_TYVAR;
    /* erased-instance-body-tags-a-type-variable-widened-to-any: a concrete
     * `: any` result spells the same C for every element type, so nothing
     * else asks for a spec -- and the erased body then widens its element
     * with no type to tag it by.  Treat it as the bare-element shape: the
     * element bindings are what the widen needs. */
    bool m7_result_is_any = m7_cm && m7_cm->return_type.kind == TY_ANY;
    bool m7_body_byvalue_ok = best_method && best_method->body &&
        best_method->body->kind != EX_INLINE_C &&
        ((m7_result_is_applied &&
          m7_body_constructs_byvalue(best_method->body)) ||
         ((m7_result_is_bare_elem || m7_result_is_any) &&
          m7_body_returns_byvalue_element(best_method->body)));
    if (best_inst && best_inst->typeclass &&
        best_method->binding->type.kind == TY_FN && m7_cm &&
        (m7_result_is_applied || m7_result_is_bare_elem || m7_result_is_any)) {
        const TypeClassMethod *cm = m7_cm;
        {
            /* The declared element tyvars `(g a)` / `(fn [a] b)` live on the
             * CLASS method's param_types (parsed with the method-tyvar fix), not
             * on the instance binding (whose arg_full_types is NULL).  Substitute
             * into the binding's result_full_type when it carries the applied
             * result; for a bare-element result that field is NULL, so fall back
             * to the class method's declared return type (`a`). */
            const Type *rft = best_method->binding->type.as.fn.result_full_type;
            if (!rft) rft = &cm->return_type;
            if (cm->n_params >= 1)
                m7_collect_tyvar_bindings(e, cm->param_types[0], obj_orig_type,
                                          m7_bind_names, m7_bind_types, &m7_nb, 16);
            for (uint32_t i = 0; i < n_args; i++) {
                uint8_t pidx = 1 + (uint8_t)i;
                if (pidx < cm->n_params)
                    m7_collect_tyvar_bindings(e, cm->param_types[pidx],
                                              args_orig_types[i], m7_bind_names,
                                              m7_bind_types, &m7_nb, 16);
            }
            /* saffron-dynamic-surface-pass M2 (compiled half): also bind the
             * INSTANCE's own head type variables -- `B` in
             * `Functor [(Result _ B)]`, `E` in `Functor [(Either E)]` -- by
             * unifying the instance head against the receiver.  The class
             * method's param types are written in the CLASS variables (`f`,
             * `a`, `b`) and can never bind one of these, so the substituted
             * result kept it free, m7_byvalue_grounded stayed false, no
             * by-value spec was ever minted, and every Result `fmap` -- typed
             * or Saffron -- rode the erased carrier.  Measured on the Result
             * instance: `body_ok=1, nb=3, grounded=0` against Option's
             * `grounded=1`, and the free variable in the substituted result
             * was `B`.  The kind-* branch below already does exactly this for
             * its own head tyvars (ECS E2d-P6); this is its HKT twin.
             *
             * HOLE-AWARE, because T4 erased the hole.  A head `(Result _ B)`
             * is stored as `app(Result, B)` with `partial_hole_pos == 0`: the
             * fixed arm B is really the ctor's slot 1, the receiver's OUTERMOST
             * argument, and pairs with it under a direct unification.  A
             * leftmost partial application `(Either E)` (`partial_hole_pos`
             * 0xFF or 1) fixes the LEADING slot, which is the receiver's inner
             * application -- unified against the whole receiver, E would bind
             * to the wrong arm -- so the receiver's outer applications are
             * peeled down to the head's depth first.
             *
             * And the hole-at-0 shape is only slot-SAFE when the receiver is
             * HOMOGENEOUS.  T4's erasure also reversed the class method's
             * reading of `(f a)` -- `a` binds to the receiver's LAST argument,
             * the err arm, not the ok arm the hole stands for (measured on
             * `(Result int cstr)`: `a := cstr`) -- and `(f b)` reconstructs as
             * `(Result B b)`, arms swapped.  All of that is invisible when
             * every type argument is the same type, which is the Saffron
             * `(Result any any)` this exists for, and a silent miscompile when
             * they differ.  So a heterogeneous `(Result int cstr)` stays on
             * the carrier exactly as today; lifting that needs a
             * hole-preserving representation of the class-variable binding,
             * a wider change than this one. */
            /* Gated on m7_body_byvalue_ok, and that gate is load-bearing:
             * grounding a head tyvar only buys anything when the body can be
             * specialised by value.  For a carrier-bodied instance (Either's
             * `fmap` delegates to `either-map`, body_ok=0) it buys nothing and
             * costs a regression -- the now-grounded result reaches the
             * `byval_agg` "commit the precise type, stay on the carrier" arm
             * below, whose consumer-side bridge does not cover a let-init, and
             * `tur_adt_Either__int__cstr m = __ps_N` is an invalid
             * initializer.  Measured; the gate returns Either to its exact
             * prior behaviour. */
            /* partial-head-ap-calls-fat-closure-as-thin-pointer: STATIC
             * dispatch only, the same rule the hole-result refinement below
             * states.  Inside a constrained generic the receiver is `(F X)`
             * with `F` abstract and best_inst is only a REPRESENTATIVE; binding
             * its head `app(Either, E)` against `(F X)` pairs E with X -- in
             * `ap` that is the FUNCTION -- and the grounded result committed
             * `(Either int (fn int int))` to the generic's temp, an invalid
             * initializer.  Unreachable before, because a hole-at-0 `ap` body
             * did not type-check. */
            bool m7_head_bind_concrete = false;
            {
                const Type *hh = &obj_orig_type;
                while (hh && hh->kind == TY_APP) hh = hh->as.app.fn;
                m7_head_bind_concrete = hh && hh->kind == TY_ADT &&
                                        hh->as.adt_.def != NULL;
            }
            if (m7_body_byvalue_ok && m7_head_bind_concrete &&
                best_inst->n_type_args >= 1 &&
                best_inst->type_args[0].kind == TY_APP) {
                Type head = best_inst->type_args[0];
                Type recv = obj_orig_type;
                bool head_ok = true;
                if (best_inst->partial_hole_pos == 0) {
                    Type first;
                    bool have_first = false;
                    for (Type cur = recv;
                         cur.kind == TY_APP && cur.as.app.fn && cur.as.app.arg;
                         cur = *cur.as.app.fn) {
                        if (!have_first) { first = *cur.as.app.arg; have_first = true; }
                        else if (!type_eq(*cur.as.app.arg, first)) { head_ok = false; break; }
                    }
                } else {
                    uint32_t hd = 0, rd = 0;
                    for (Type t = head; t.kind == TY_APP && t.as.app.fn; t = *t.as.app.fn) hd++;
                    for (Type t = recv; t.kind == TY_APP && t.as.app.fn; t = *t.as.app.fn) rd++;
                    while (rd > hd && recv.kind == TY_APP && recv.as.app.fn) {
                        recv = *recv.as.app.fn;
                        rd--;
                    }
                }
                if (head_ok)
                    m7_collect_tyvar_bindings(e, head, recv, m7_bind_names,
                                              m7_bind_types, &m7_nb, 16);
            }
            /* M6 / G6(c): the method result `(f b)`'s element `b` is determined by
             * a CLOSURE argument's RESULT (the fmap/Monad shape: `g : (fn [a] b)`).
             * When that result is an ungrounded tyvar -- the enclosing generic's
             * own param `B` in `(defn re-cata [B] ... (fmap recv (fn [c] : B ...)))`
             * -- m7_collect SKIPPED it (its tyvar-actual skip guards the
             * Applicative `ap` shape, whose element comes from a wrapped VALUE arg,
             * not a closure result).  Bind it symbolically (`b -> B`) so emit can
             * compose it through the enclosing monomorphization.  Tightly gated:
             * only an APPLIED result whose element tyvar is the RESULT of a
             * fn-typed param (never `ap`, whose element sits in a value arg). */
            if (m7_result_is_applied && cm->return_type.as.app.arg &&
                cm->return_type.as.app.arg->kind == TY_TYVAR &&
                cm->return_type.as.app.arg->as.tyvar_.name) {
                const char *belem = cm->return_type.as.app.arg->as.tyvar_.name;
                bool present = false;
                for (uint8_t k = 0; k < m7_nb; k++)
                    if (m7_bind_names[k] &&
                        strcmp(m7_bind_names[k]->name, belem) == 0) { present = true; break; }
                if (!present) {
                    for (uint8_t p = 1; p < cm->n_params; p++) {
                        const Type *pt = &cm->param_types[p];
                        if (pt->kind != TY_FN) continue;
                        const Type *pr = pt->as.fn.result_full_type;
                        if (!pr || pr->kind != TY_TYVAR || !pr->as.tyvar_.name ||
                            strcmp(pr->as.tyvar_.name, belem) != 0) continue;
                        if ((uint32_t)(p - 1) >= n_args) break;
                        Type at = args_orig_types[p - 1];
                        if (at.kind != TY_FN) break;
                        Type ar = at.as.fn.result_full_type
                            ? *at.as.fn.result_full_type
                            : type_from_kind(at.as.fn.result_kind);
                        if (m7_nb < 16) {
                            m7_bind_names[m7_nb] = symtab_intern(
                                e->st, strslice(belem, (uint32_t)strlen(belem)));
                            m7_bind_types[m7_nb] = ar;
                            m7_nb++;
                            m7_symbolic_elem = true;
                        }
                        break;
                    }
                }
            }
            /* hkt-carrier-result-loses-payload-types: the result type of a
             * CARRIER-path dispatch on a partially-applied instance head.
             *
             * The two gates above leave two shapes on the erased carrier with
             * a def-less `(type-app ? ?)` result: a heterogeneous receiver
             * under a hole-at-0 head (`Functor [(Result _ B)]` on `(Result
             * int cstr)`), and a carrier-bodied instance (`Functor [(Either
             * E)]`, whose `fmap` delegates to `either-map`).  Both are right
             * to stay on the carrier -- neither can take the by-value route
             * -- but the CONSUMER then sees no payload types: the `match` on
             * the result binds every arm as the int carrier, and an `Err s`
             * of `cstr` printed as the string's ADDRESS.
             *
             * The result type is recoverable without touching `rft`, which is
             * where T4's hole erasure swaps the arms (`(f b)` reconstructs as
             * `(Result B b)`).  The class method's receiver IS `(f a)` and its
             * result IS `(f b)`, and the instance head fixes every slot of
             * `f` except the hole -- so `(f b)` is the RECEIVER with its hole
             * slot replaced by `b`, whatever the head's spelling.  `b` is read
             * from the function parameter that states it (below), with the
             * same hole rule when that statement is itself `(f b)`.  It is
             * derived for a STATIC dispatch only.  Committed below only as a
             * carrier-path refinement (m7_byvalue_grounded stays false), on
             * the same terms as the byval_agg arm: the consumer bridges the
             * carrier box to the aggregate, which the call hoist now does at
             * production (emit_expr.c) so every consumer position sees it. */
            Type m7_hole_result;
            bool m7_have_hole_result = false;
            /* STATIC dispatch only: the receiver's constructor head must be a
             * concrete ADT.  Inside a constrained generic (`x : (m int)` under
             * `^Monad m`) the resolved instance is a REPRESENTATIVE, and a type
             * minted from it (`(Result int int)`) would be committed on a call
             * whose real instance is decided per monomorph -- the dict clone
             * then materialised an aggregate its int64-carrier return could not
             * hand back (hkt-constrained-*, van-laarhoven-lens-wide-*). */
            bool m7_recv_head_concrete = false;
            {
                const Type *hh = &obj->type;
                while (hh && hh->kind == TY_APP) hh = hh->as.app.fn;
                m7_recv_head_concrete = hh && hh->kind == TY_ADT &&
                                        hh->as.adt_.def != NULL;
            }
            if (m7_result_is_applied && m7_recv_head_concrete &&
                !obj_is_abstract_tyvar && best_inst->n_type_args >= 1 &&
                best_inst->type_args[0].kind == TY_APP &&
                obj_orig_type.kind == TY_APP &&
                cm->n_params >= 1 && cm->param_types[0].kind == TY_APP &&
                cm->param_types[0].as.app.fn &&
                cm->param_types[0].as.app.fn->kind == TY_TYVAR &&
                cm->param_types[0].as.app.fn->as.tyvar_.name &&
                cm->return_type.as.app.fn &&
                cm->return_type.as.app.fn->kind == TY_TYVAR &&
                cm->return_type.as.app.fn->as.tyvar_.name &&
                strcmp(cm->return_type.as.app.fn->as.tyvar_.name,
                       cm->param_types[0].as.app.fn->as.tyvar_.name) == 0 &&
                cm->return_type.as.app.arg) {
                Type belem = *cm->return_type.as.app.arg;
                bool belem_ok = belem.kind != TY_TYVAR;
                const char *fvar = cm->param_types[0].as.app.fn->as.tyvar_.name;
                uint32_t recv_depth = 0;
                for (Type t = obj_orig_type; t.kind == TY_APP && t.as.app.fn;
                     t = *t.as.app.fn)
                    recv_depth++;
                uint32_t hole_layers =
                    (best_inst->partial_hole_pos == 0) ? recv_depth - 1 : 0;
                if (belem.kind == TY_TYVAR && belem.as.tyvar_.name) {
                    const char *bname = belem.as.tyvar_.name;
                    /* `b` is read from a FUNCTION parameter's declared result,
                     * the way the closure's own type states it -- a bare `b`
                     * (fmap) is the closure's result itself; an `(f b)` (bind's
                     * continuation) is the closure's result with the SAME hole
                     * slot read out of it, because the class-variable
                     * unification of `(f b)` against `(Result int cstr)` binds
                     * `b` to the OUTERMOST argument (T4's erasure again), which
                     * for a hole-at-0 head is the fixed arm, not the hole. */
                    for (uint8_t p = 1; p < cm->n_params && !belem_ok; p++) {
                        const Type *pt = &cm->param_types[p];
                        if (pt->kind != TY_FN || !pt->as.fn.result_full_type) continue;
                        const Type *pr = pt->as.fn.result_full_type;
                        if ((uint32_t)(p - 1) >= n_args) break;
                        Type at = args_orig_types[p - 1];
                        if (at.kind != TY_FN) continue;
                        Type ar = at.as.fn.result_full_type
                            ? *at.as.fn.result_full_type
                            : type_from_kind(at.as.fn.result_kind);
                        if (pr->kind == TY_TYVAR && pr->as.tyvar_.name &&
                            strcmp(pr->as.tyvar_.name, bname) == 0) {
                            if (ar.kind != TY_TYVAR && ar.kind != TY_UNKNOWN) {
                                belem = ar;
                                belem_ok = true;
                            }
                            break;
                        }
                        if (pr->kind == TY_APP && pr->as.app.fn &&
                            pr->as.app.fn->kind == TY_TYVAR &&
                            pr->as.app.fn->as.tyvar_.name && fvar &&
                            strcmp(pr->as.app.fn->as.tyvar_.name, fvar) == 0 &&
                            pr->as.app.arg && pr->as.app.arg->kind == TY_TYVAR &&
                            pr->as.app.arg->as.tyvar_.name &&
                            strcmp(pr->as.app.arg->as.tyvar_.name, bname) == 0) {
                            uint32_t ad = 0;
                            for (Type t = ar; t.kind == TY_APP && t.as.app.fn;
                                 t = *t.as.app.fn)
                                ad++;
                            if (ad == recv_depth) {
                                Type slot;
                                if (m7_app_slot_arg(ar, hole_layers, &slot) &&
                                    slot.kind != TY_TYVAR && slot.kind != TY_UNKNOWN) {
                                    belem = slot;
                                    belem_ok = true;
                                }
                            }
                            break;
                        }
                    }
                    /* partial-head-ap-calls-fat-closure-as-thin-pointer: `ap`
                     * states `b` nowhere in a function PARAMETER -- its function
                     * is the receiver's element, `ff : (f (fn [a] b))`.  Read
                     * `b` from the fn in the receiver's hole slot (the same slot
                     * the result's `b` replaces below), so `(ap ff fa)` on a
                     * `(Result (fn [int] int) int)` grounds to `(Result int
                     * int)` instead of the def-less `(type-app ? ?)`. */
                    if (!belem_ok && cm->param_types[0].as.app.arg &&
                        cm->param_types[0].as.app.arg->kind == TY_FN &&
                        cm->param_types[0].as.app.arg->as.fn.result_full_type) {
                        const Type *pr =
                            cm->param_types[0].as.app.arg->as.fn.result_full_type;
                        Type slot;
                        if (pr->kind == TY_TYVAR && pr->as.tyvar_.name &&
                            strcmp(pr->as.tyvar_.name, bname) == 0 &&
                            m7_app_slot_arg(obj_orig_type, hole_layers, &slot) &&
                            slot.kind == TY_FN) {
                            Type ar = slot.as.fn.result_full_type
                                ? *slot.as.fn.result_full_type
                                : type_from_kind(slot.as.fn.result_kind);
                            if (ar.kind != TY_TYVAR && ar.kind != TY_UNKNOWN) {
                                belem = ar;
                                belem_ok = true;
                            }
                        }
                    }
                    /* No function parameter states `b`: the collected binding
                     * is trustworthy only when `b` never sits under an `(f ..)`
                     * application in the class signature (where the erasure
                     * would have reversed it). */
                    if (!belem_ok &&
                        !m7_tyvar_under_app_of(&cm->return_type, fvar, bname) &&
                        !m7_params_mention_under_app_of(cm, fvar, bname)) {
                        for (uint8_t k = 0; k < m7_nb; k++)
                            if (m7_bind_names[k] &&
                                strcmp(m7_bind_names[k]->name, bname) == 0) {
                                belem = m7_bind_types[k];
                                belem_ok = true;
                                break;
                            }
                    }
                }
                /* Slot 0 is the INNERMOST application; a hole-at-0 head fixes
                 * the outer slots, any other partial head (`(Either E)`, hole
                 * pos 1 or 0xFF) leaves the OUTERMOST free -- hole_layers,
                 * computed above, for both the receiver and a `(f b)` result. */
                if (belem_ok && !m7_type_has_free_tyvar(belem) && recv_depth >= 1) {
                    m7_hole_result = m7_app_replace_slot(
                        e->arena, obj_orig_type, hole_layers, belem);
                    m7_have_hole_result = true;
                }
            }
            if (m7_nb > 0) {
                Type substituted = elab_subst_class_tyvars(
                    e->arena, *rft, m7_bind_names, m7_nb, m7_bind_types, m7_nb);
                Type ptr_family_result;
                bool ptr_family = m7_app_to_ptr_family(substituted, &ptr_family_result);
                /* hkt-inline-c-instance-body-loses-result-type: a :heap ADT is
                 * the third carrier-width result class, alongside the
                 * int-carrier newtypes below and the pointer-family handles
                 * above.  `(HBox int)` for a `:heap` parametric struct emits as
                 * `tur_adt_HBox__int *` -- a pointer, so the by-value
                 * representation IS the int64 carrier the method returns and
                 * committing the precise type needs no by-value spec.  Unlike
                 * the pointer-family case there is nothing to collapse: the
                 * applied type is already the right one. */
                bool heap_app = !ptr_family &&
                    (type_is_heap_adt(substituted) ||
                     type_is_heap_struct(substituted));
                /* hkt-inline-c-instance-body-loses-result-type: the last class,
                 * and the only one that is NOT carrier-width -- a by-value
                 * aggregate.  Committing it is safe because the CONSUMER bridges:
                 * emit_expr.c's fn_body_tail_byvalue_carrier_type reports the
                 * grounded aggregate for an inline-C-bodied dispatch call, so the
                 * existing carrier -> by-value deref runs at the binding
                 * (`Box__int m = *(Box__int *)(intptr_t)__ps_N`).  The argument
                 * was heap-spilled to reach the dict's int64 slot, so the carrier
                 * really is a pointer to the aggregate -- nothing new is
                 * generated, no wrapper and no second ABI.
                 *
                 * m7_byvalue_grounded still stays false: dispatch remains on the
                 * uniform carrier ABI and no by-value spec is minted.  That is the
                 * point -- an inline-C body cannot be re-specialized, so the
                 * PRODUCER keeps the carrier and only the consumer adapts.
                 *
                 * Recognized with type_app_is_concrete_adt, the same predicate
                 * the by-value HKT spec machinery uses for a monomorphizable
                 * parametric result.  It already excludes an opaque head (that
                 * is the int-carrier arm's job) and demands every type argument
                 * have a concrete layout, so a still-parametric result never
                 * reaches here. */
                bool byval_agg = !ptr_family && !heap_app &&
                    substituted.kind == TY_APP &&
                    type_app_is_concrete_adt(&substituted);
                /* Only commit the by-value result type (and, below, the by-value
                 * element bindings) when the result fully grounds.  A residual
                 * free element tyvar -- the `ap` fat-closure-carrier case --
                 * keeps the carrier result_type so dispatch falls back to the
                 * uniform carrier ABI instead of emitting a broken half-by-value
                 * spec with a dangling carrier-base dict reference. */
                if (!m7_type_has_free_tyvar(substituted)) {
                    if (ptr_family && result_type.kind == TY_APP) {
                        /* hkt-fmap-result-is-not-droppable: a pointer-family head
                         * (`(type-app rc<?> int)`) collapses to the concrete
                         * `rc<int>`, so `(rc/drop (fmap r f))` type-checks instead
                         * of failing on the def-less `(type-app ? ?)` shell.
                         *
                         * Checked BEFORE m7_body_byvalue_ok, not after: committing
                         * the TY_APP form for a pure-Turmeric body would leave the
                         * same un-droppable shape, and the by-value spec is not
                         * wanted here anyway -- an rc IS the carrier, so the
                         * uniform carrier dispatch is already the right ABI.
                         * m7_byvalue_grounded deliberately stays false, exactly as
                         * in the int-carrier-newtype arm below.
                         *
                         * Same `result_type.kind == TY_APP` guard as that arm: only
                         * REFINE the def-less carrier shell, never clobber an
                         * instance that overrode the class result with a concrete
                         * scalar (typeclass-instance-float-return). */
                        result_type = ptr_family_result;
                    } else if (m7_body_byvalue_ok) {
                        result_type = substituted;
                        m7_byvalue_grounded = true;
                    } else if (result_type.kind == TY_APP &&
                               (heap_app || byval_agg ||
                                m7_result_is_int_carrier(substituted))) {
                        /* method-result-functor-inference: the instance body
                         * delegates to a carrier helper (`(mk-box ...)`), so it
                         * is not by-value-constructible -- but the grounded
                         * result is an int64-carrier newtype (`(Box int)`),
                         * whose representation is ALREADY the carrier the method
                         * returns.  Commit the precise applied result type so a
                         * downstream `(un-box r)` matches `(Box A)`, WITHOUT
                         * setting m7_byvalue_grounded: dispatch stays on the
                         * uniform carrier ABI and no by-value spec is minted.
                         *
                         * Gate on `result_type.kind == TY_APP`: we only REFINE
                         * the def-less `(type-app ? ?)` carrier shell.  An
                         * instance that overrode the class's `(f b)` with a
                         * concrete scalar return (`fmap ... : float`) already
                         * has result_type == float here -- leave it alone so the
                         * divergent scalar is not clobbered by the recovered
                         * `(f int)` (typeclass-instance-float-return). */
                        result_type = substituted;
                    }
                }
            }
            /* hkt-carrier-result-loses-payload-types: the carrier-path
             * refinement for the partially-applied head (computed above).
             * Only when the by-value route did not take the call and the
             * result is still the def-less shell; the same three
             * representation classes the byval_agg arm admits, for the same
             * reason -- a :heap app and an int-carrier newtype ARE the
             * carrier, and a by-value aggregate is what the hoist bridges. */
            if (m7_have_hole_result && !m7_byvalue_grounded &&
                result_type.kind == TY_APP &&
                !m7_type_has_free_tyvar(m7_hole_result)) {
                Type hr = m7_hole_result;
                Type pf_dummy;
                bool hr_heap = type_is_heap_adt(hr) || type_is_heap_struct(hr);
                bool hr_byval = !hr_heap && hr.kind == TY_APP &&
                                type_app_is_concrete_adt(&hr);
                if (!m7_app_to_ptr_family(hr, &pf_dummy) &&
                    (hr_heap || hr_byval || m7_result_is_int_carrier(hr)))
                    result_type = hr;
            } else if (m7_have_hole_result && !m7_byvalue_grounded &&
                       result_type.kind == TY_APP &&
                       m7_type_has_free_tyvar(obj_orig_type)) {
                /* fmap-over-underdetermined-constructor-is-a-defless-shell: a
                 * receiver with an OPEN fixed slot -- `(Result int B)`, which is
                 * what `(ok 41)` and `(Ok 41)` are -- leaves the same open slot
                 * in `(f b)`, and `b` itself is ground (m7_have_hole_result is
                 * set only then), so every variable left in the result came
                 * from the receiver.  An application with a variable argument
                 * is the carrier, as the receiver was and as the method
                 * returns, so there is nothing to bridge: commit it, and a
                 * generic consumer (`ok-val [A B] [r : (Result A B)]`) unifies
                 * with it instead of meeting the def-less shell. */
                Type hr = m7_hole_result;
                Type pf_dummy;
                if (hr.kind == TY_APP && type_adt_app_def(&hr) &&
                    !type_is_heap_adt(hr) && !type_is_heap_struct(hr) &&
                    !m7_app_to_ptr_family(hr, &pf_dummy))
                    result_type = hr;
            }
        }
    }
    Expr *out = expr_new(e->arena, EX_CALL, result_type, call->span);
    /* Phase H §1: receiver type is statically known here (best_inst was resolved
     * by the type-based instance search above; ambiguous matches would have
     * errored with TUR_E0020 earlier).  Emit a direct call to the instance
     * impl, skipping the dictionary indirection.  Keep dict_arg as an
     * annotation for downstream passes that may still want to know which
     * instance was selected.  Fall back to dict dispatch only when there is
     * no resolved instance (defensive). */
    /* RT1: a statically-resolved method dispatch is a crossing into whatever
     * refinements the SELECTED INSTANCE's method parameters declare.  The
     * receiver sits at argument 0 of the impl and at index 1 of the source
     * form, so the parameter/argument slots line up exactly as for a named
     * call.
     *
     * A dispatch on an ABSTRACT receiver never resolved to an instance -- it
     * fell back to an arbitrary carrier-compatible one, which is not the
     * instance that will run.  Cross into the CLASS signature there instead:
     * it is the one demand every instance honours (see
     * rt_class_method_refine_binding).  Class parameter indices line up with
     * instance ones -- both count the receiver at 0 -- so the same arg_offset
     * of 1 applies.  Either way the method's own entry check still guards the
     * call; this layer reports, it never elides. */
    const Binding *rt_cs_callee = NULL;
    if (obj_is_abstract_tyvar && best_inst && best_inst->typeclass) {
        TypeClass *rt_tc = best_inst->typeclass;
        for (uint8_t rt_mi = 0; rt_mi < rt_tc->n_methods; rt_mi++) {
            const Symbol *mn = rt_tc->methods[rt_mi].name;
            if (mn && strlen(mn->name) == method_name_len &&
                strncmp(mn->name, method_name, method_name_len) == 0) {
                rt_cs_callee = rt_class_method_refine_binding(e, rt_tc, rt_mi);
                break;
            }
        }
    }
    bool rt_is_class_cs = (rt_cs_callee != NULL);
    if (!rt_cs_callee && best_method) rt_cs_callee = best_method->binding;
    if (rt_cs_callee) {
        (void)refine_note_call_site(e, rt_cs_callee, call, 1);
        /* Reading B + lint: a STATICALLY-resolved dispatch is obliged to
         * satisfy the instance it resolved to, which is the more precise
         * contract.  Carry the class's predicates alongside so a call the
         * class would reject can be linted (TUR-W0377) without changing
         * what it must prove.  Not for a dynamic crossing -- that one IS
         * the class signature, so there is nothing to compare against. */
        if (!rt_is_class_cs && best_inst && best_inst->typeclass) {
            TypeClass *rt_tc = best_inst->typeclass;
            for (uint8_t rt_mi = 0; rt_mi < rt_tc->n_methods; rt_mi++) {
                const Symbol *mn = rt_tc->methods[rt_mi].name;
                if (!mn || strlen(mn->name) != method_name_len ||
                    strncmp(mn->name, method_name, method_name_len) != 0)
                    continue;
                if (rt_tc->methods[rt_mi].param_refine_preds)
                    refine_note_call_site_class_preds(
                        e, rt_cs_callee, call,
                        rt_tc->methods[rt_mi].param_refine_preds,
                        rt_tc->methods[rt_mi].param_refine_vars,
                        rt_tc->methods[rt_mi].n_params);
                break;
            }
        }
    }

    if (best_method && best_method->binding) {
        out->as.call_.fn_binding = best_method->binding;
        out->as.call_.fn_expr    = NULL;
    } else if (dict_expr && !has_poly_params) {
        out->as.call_.fn_binding = NULL;
        out->as.call_.fn_expr    = dict_expr;
    } else {
        out->as.call_.fn_binding = best_method->binding;
        out->as.call_.fn_expr    = NULL;
    }
    out->as.call_.args    = call_args;
    out->as.call_.n_args  = n_args + 1;
    out->as.call_.dict_arg = dict_expr;  /* annotation for downstream passes */
    /* value-struct-payload-sum-monomorph-box-has-no-owner (dictionary sites):
     * the drop-after stamp elab_call.c puts on a fresh sum argument of a
     * non-retaining defn, for a CLASS-METHOD consumer.  This path never runs
     * elab_call's argument loop, so `(enc (some (make-struct Box ..)))` kept
     * its payload box for the process lifetime although the instance body
     * only reads `x` -- the one shape left after three rounds of that report.
     *
     * Same three facts as the defn stamp, asked of the instance that was
     * resolved here: a non-suspending consumer, a fresh producer in the slot,
     * and the consumer's inferred nonretain_sum_param_mask bit for it.
     * Param index i IS argument index i -- params[0] is the receiver and so is
     * call_args[0].  When the dispatch is STATIC (receiver head concrete) the
     * resolved binding is the method that runs and the stamp is final.  When
     * it is not -- inside a constrained generic body, where fn_binding is a
     * representative -- the argument is flagged TENTATIVELY
     * (sum_box_drop_after_dyn) and the consumer's emission re-resolves the
     * instance per monomorph before admitting the drop, exactly as the
     * freshness question is re-asked there. */
    {
        const Binding *cb = out->as.call_.fn_binding;
        if (cb && cb->type.kind == TY_FN &&
            effect_row_is_empty(cb->type.as.fn.effect_row)) {
            bool static_disp = call_dispatch_is_static(out);
            for (uint32_t ai = 0; ai < out->as.call_.n_args && ai < 32; ai++) {
                Expr *a = out->as.call_.args[ai];
                while (a && a->kind == EX_ASCRIBE) a = a->as.ascribe_.inner;
                if (!a || a->kind != EX_CALL) continue;
                bool fresh_arg = call_returns_fresh_sum_box(a) ||
                    (a->as.call_.dict_arg && !call_dispatch_is_static(a));
                if (!fresh_arg) continue;
                if (static_disp) {
                    if (cb->nonretain_sum_param_mask & (1u << ai))
                        a->sum_box_drop_after = true;
                } else {
                    a->sum_box_drop_after_dyn = true;
                }
            }
        }
    }
    /* M4c Path A step 1 (docs/archive/m4c-execution-plan.md): bind the
     * class variable to the CALL SITE'S receiver type so
     * emit_abi_register_call mints a per-instantiation spec.  HKT carve-out
     * stays — those keep the uniform-carrier dispatch per Plan M6/M7. */
    if (best_inst && best_inst->typeclass
        && out->as.call_.fn_binding != NULL) {
        TypeClass *tc = best_inst->typeclass;
        bool is_hkt = false;
        if (tc->type_param_kinds) {
            for (uint8_t i = 0; i < tc->n_type_params; i++) {
                if (tc->type_param_kinds[i] != KIND_STAR) { is_hkt = true; break; }
            }
        }
        if (!is_hkt && tc->n_type_params == 1 && tc->type_params[0]) {
            /* ECS E2d-P6 (parametric associated-type element): besides binding
             * the class var (`S -> (Dense Pos)`), also bind the matched
             * instance's OWN head tyvars by unifying its head (`(Dense A)`)
             * against the receiver (`(Dense Pos)`) -> `A -> Pos`.  The instance
             * method's param/return types are written in terms of `A` (the
             * `: Elem` associated member projects to it), so without these
             * bindings emit_abi_register_call sees nothing to substitute, mints
             * no by-value spec, and the struct element stays on the int64
             * carrier (`storage-get` returns int64_t, `storage-insert!` takes
             * int64_t v).  Grounding `A` lets the spec emit a by-value signature
             * and monomorphize the body's `dense-get`/`dense-set!` to the struct.
             * Inert for an int element (`A -> int` is the carrier identity) and
             * for a concrete instance head (no head tyvars to collect). */
            const Symbol *hb_names[ABI_TYPE_BINDINGS_MAX];
            Type hb_types[ABI_TYPE_BINDINGS_MAX];
            uint8_t hb_n = 0;
            if (best_inst->n_type_args >= 1)
                m7_collect_tyvar_bindings(e, best_inst->type_args[0],
                                          obj_orig_type, hb_names, hb_types,
                                          &hb_n, ABI_TYPE_BINDINGS_MAX - 1);
            /* fn-param-call-prototype-spelled-from-the-call-not-the-fn: bind
             * the METHOD's own type variables too -- `b` in
             * `(ap2 [x : a g : (fn [float float] b)] : b)` -- from the
             * arguments that fix them.  The spec a `: b` result mints knows
             * only its signature (`double`); without `b` in its bindings the
             * body kept spelling every `b` it held, the call through `g`
             * included, as the int64 carrier, and read a double result out of
             * `rax`.  Only a GROUND solution is recorded (a free tyvar actual
             * is skipped by the collector, and an argument still mentioning
             * one binds nothing), and a name the class var or the instance
             * head already binds keeps that binding. */
            const Symbol *mb_names[ABI_TYPE_BINDINGS_MAX];
            Type mb_types[ABI_TYPE_BINDINGS_MAX];
            uint8_t mb_n = 0;
            if (m7_cm) {
                const Symbol *raw_names[16];
                Type raw_types[16];
                uint8_t raw_n = 0;
                for (uint32_t i = 0; i < n_args; i++) {
                    uint8_t pidx = (uint8_t)(1 + i);
                    if (pidx >= m7_cm->n_params) break;
                    m7_collect_tyvar_bindings(e, m7_cm->param_types[pidx],
                                              args_orig_types[i], raw_names,
                                              raw_types, &raw_n, 16);
                }
                for (uint8_t k = 0; k < raw_n; k++) {
                    const char *nm = raw_names[k] ? raw_names[k]->name : NULL;
                    if (!nm || strcmp(nm, tc->type_params[0]->name) == 0) continue;
                    if (!elab_type_is_ground(&raw_types[k])) continue;
                    bool dup = false;
                    for (uint8_t h = 0; h < hb_n && !dup; h++)
                        if (strcmp(hb_names[h]->name, nm) == 0) dup = true;
                    if (dup) continue;
                    if ((uint8_t)(1 + hb_n + mb_n) >= ABI_TYPE_BINDINGS_MAX) break;
                    mb_names[mb_n] = raw_names[k];
                    mb_types[mb_n] = raw_types[k];
                    mb_n++;
                }
            }
            uint8_t total = (uint8_t)(1 + hb_n + mb_n);
            AbiTypeBinding *bindings = (AbiTypeBinding *)arena_alloc(
                e->arena, total * sizeof(AbiTypeBinding));
            bindings[0].name = tc->type_params[0]->name;
            bindings[0].type = obj_orig_type;
            for (uint8_t k = 0; k < hb_n; k++) {
                bindings[1 + k].name = hb_names[k]->name;
                bindings[1 + k].type = hb_types[k];
            }
            for (uint8_t k = 0; k < mb_n; k++) {
                bindings[1 + hb_n + k].name = mb_names[k]->name;
                bindings[1 + hb_n + k].type = mb_types[k];
            }
            out->as.call_.abi_bindings = bindings;
            out->as.call_.n_abi_bindings = total;
        }
        /* M7 layer-4 prep: for an HKT class, attach the class var
         * (`g -> Option`) plus the element tyvars collected by layers 1+3
         * (`a -> int`, `b -> int`) as the dispatch call's abi_bindings, so
         * emit_abi_register_call can mint a per-(f, A) by-value instance-method
         * spec instead of falling through to the carrier-double-boxing path. */
        /* M7 layer-4: only by-value-expressible (pure-Turmeric) instance bodies
         * can be monomorphized by value.  Carrier inline-C instance bodies (the
         * current stdlib HKT instances) must stay on the uniform-carrier
         * dispatch until Phase 4.2 rewrites them; attaching the element bindings
         * to them would mint a by-value spec whose signature contradicts the
         * carrier inline-C body.  Gate on the body kind (m7_body_byvalue_ok,
         * computed up front above). */
        if (is_hkt && m7_body_byvalue_ok &&
            (m7_byvalue_grounded || m7_symbolic_elem) &&
            tc->n_type_params >= 1 && tc->type_params[0]) {
            uint8_t total = (uint8_t)(1 + m7_nb + 4);  /* +4: struct-param grounding */
            if (total > ABI_TYPE_BINDINGS_MAX) total = ABI_TYPE_BINDINGS_MAX;
            AbiTypeBinding *bindings = (AbiTypeBinding *)arena_alloc(
                e->arena, total * sizeof(AbiTypeBinding));
            /* Bind the HKT class var to the CONSTRUCTOR HEAD of the receiver
             * (`Option`), not the full applied receiver (`(Option int)`), so an
             * applied occurrence in the body -- e.g. the monadic continuation's
             * result `(m b)` in `bind` -- resolves to `(Option b)` -> `(Option
             * int)` at emit time instead of the nonsensical `((Option int) int)`
             * (which type_c_name's to the int64 carrier).  For the fmap shape the
             * result `(g b)` is pre-resolved by the elaborator, so this only
             * matters for in-body applied occurrences like the bind tail call. */
            Type hkt_head = obj_orig_type;
            while (hkt_head.kind == TY_APP && hkt_head.as.app.fn)
                hkt_head = *hkt_head.as.app.fn;
            /* Prefer the class var's COLLECTED binding -- the receiver's
             * constructor head, recovered by m7_collect from whichever param
             * carries the class var (`(g a)` / `(p a b)`).  `obj` is the FIRST
             * arg, which is the HKT receiver only when the receiver is param 0
             * (fmap/bind/ap/extract).  For methods whose HKT receiver is NOT
             * first -- Bifunctor `bimap [g h x]`, Foldable `foldr [f z t]` --
             * obj is a function arg, so stripping its head gives the wrong type
             * (a `(fn ...)` instead of `Result`); the collected binding is right.
             * m7_collect already records the HEAD (it recurses into the TY_APP
             * fn position), so this needs no extra stripping. */
            for (uint8_t k = 0; k < m7_nb; k++) {
                if (m7_bind_names[k] &&
                    strcmp(m7_bind_names[k]->name,
                           tc->type_params[0]->name) == 0) {
                    hkt_head = m7_bind_types[k];
                    break;
                }
            }
            bindings[0].name = tc->type_params[0]->name;
            bindings[0].type = hkt_head;
            uint8_t bi = 1;
            for (uint8_t k = 0; k < m7_nb && bi < total; k++) {
                bindings[bi].name = m7_bind_names[k]->name;
                bindings[bi].type = m7_bind_types[k];
                bi++;
            }
            out->as.call_.abi_bindings = bindings;
            out->as.call_.n_abi_bindings = bi;
        }
    }
    /* arrow-instance-closure-erased-to-words: a method of a `(->)`-headed
     * instance whose class signature spells the arrows -- `(>>> [f : (a b c)
     * g : (a c d)] : (a b d))` -- called with concrete functions.  Bind the
     * method's element variables from the arguments (`b c d := float`) so the
     * emitter specializes the instance body, and the closure it returns, at
     * those types, and give the call its grounded result `(fn [b] d)`.
     * Without it the body ran once at erased words: the closure it built
     * called `f` and `g` through `int64_t (*)(void *, int64_t)` and the
     * caller read it back at `(fn [float] float)` -- three indirect calls
     * through the wrong function type, right only by register luck.  Only a
     * fully ground solution is attached; anything else keeps the erased path
     * (an untyped class method has no variables to bind, and is unchanged). */
    if (best_inst && best_inst->typeclass && best_inst->n_type_args >= 1 &&
        tc_is_arrow_head_marker(&best_inst->type_args[0]) && m7_cm &&
        out->as.call_.fn_binding != NULL &&
        best_inst->typeclass->n_type_params == 1 &&
        best_inst->typeclass->type_params[0]) {
        TypeClass *atc = best_inst->typeclass;
        const Symbol *an[16];
        Type at[16];
        uint8_t ann = 0;
        if (m7_cm->n_params >= 1)
            m7_collect_tyvar_bindings(e, m7_cm->param_types[0], obj_orig_type,
                                      an, at, &ann, 16);
        for (uint32_t i = 0; i < n_args; i++) {
            uint8_t pidx = (uint8_t)(1 + i);
            if (pidx >= m7_cm->n_params) break;
            m7_collect_tyvar_bindings(e, m7_cm->param_types[pidx],
                                      args_orig_types[i], an, at, &ann, 16);
        }
        bool ground = ann > 0 && ann < ABI_TYPE_BINDINGS_MAX;
        for (uint8_t k = 0; ground && k < ann; k++)
            if (!an[k] || !elab_type_is_ground(&at[k])) ground = false;
        Type res = TYPE_INT;
        if (ground) {
            const Symbol *sn[17];
            Type st[17];
            sn[0] = atc->type_params[0];
            st[0] = best_inst->type_args[0];
            for (uint8_t k = 0; k < ann; k++) { sn[1 + k] = an[k]; st[1 + k] = at[k]; }
            res = elab_subst_class_tyvars(e->arena, m7_cm->return_type, sn,
                                          (uint8_t)(1 + ann), st, (uint8_t)(1 + ann));
            ground = res.kind == TY_FN && res.as.fn.arity >= 1 &&
                     elab_type_is_ground(&res);
        }
        if (ground) {
            AbiTypeBinding *bindings = (AbiTypeBinding *)arena_alloc(
                e->arena, (size_t)(1 + ann) * sizeof(AbiTypeBinding));
            bindings[0].name = atc->type_params[0]->name;
            bindings[0].type = best_inst->type_args[0];
            for (uint8_t k = 0; k < ann; k++) {
                bindings[1 + k].name = an[k]->name;
                bindings[1 + k].type = at[k];
            }
            out->as.call_.abi_bindings = bindings;
            out->as.call_.n_abi_bindings = (uint8_t)(1 + ann);
            out->type = res;
        }
    }
    /* method-call-control-operand-evicted: a dict-dispatched method call is
     * an indirect callee to the CPS translation, which delegates it whole to
     * the direct emitter and so requires atomic operands -- `(.p (handle
     * ...))` evicted its function ("indirect call (non-atomic args)") and,
     * with it, every performer of the handled effect.  Bind a control-bearing
     * operand out first, as the constructor call already does. */
    return elab_hoist_control_operands(e, out);
}

/* ======================================================================== *
 * class-superclasses (docs/archive/typeclass-superclasses-plan.md): the
 * post-unit pass -- SC2 (resolve + validate the superclass graph) and SC4
 * (the instance obligation, plan Half B).
 *
 * Runs once every form in the unit is registered, for the same reason method
 * resolution already does (typeclass-guide: "instance order does not
 * matter"): a superclass may be declared below its subclass, and the
 * Semigroup [int] that discharges a Monoid [int] obligation may be declared
 * below that instance.  Half B is what makes Half A sound -- without it the
 * entailment licenses a `combine` call for which no instance need exist, and
 * the resolver's fallback in that case is the silent carrier-representative
 * bind the concrete-receiver arm of elab_method_call exists to refuse.
 * ======================================================================== */

static const char *sc_kind_name(const Kind *kinds, uint8_t i) {
    return kind_to_string(kinds ? kinds[i] : KIND_STAR);
}

/* "int cstr" / "Vec" -- the instance head as the author wrote it where the
 * symbol survived, else the structural type name. */
static void sc_type_args_str(const Type *args, const Symbol *const *syms,
                             uint8_t n, char *buf, size_t cap) {
    size_t used = 0;
    buf[0] = '\0';
    for (uint8_t i = 0; i < n && used + 2 < cap; i++) {
        const char *s = (syms && syms[i] && syms[i]->name) ? syms[i]->name
                                                            : type_name(args[i]);
        int w = snprintf(buf + used, cap - used, "%s%s", i ? " " : "", s);
        if (w < 0) break;
        used += (size_t)w;
        if (used >= cap) { used = cap - 1; break; }
    }
}

bool elab_typeclass_superclasses_finish(Elab *e) {
    TypeClassEnv *env = &e->typeclass_env;
    bool ok = true;

    /* SC2 (1): resolve every preamble element to its class and check the
     * shape fits: the superclass's parameter count is the number of variables
     * the element names, and each variable's kind is what that parameter
     * slot expects. */
    for (TypeClass *tc = env->typeclasses; tc; tc = tc->next) {
        if (tc->n_supers == 0) continue;
        typeclass_resolve_superclasses(env, tc);
        for (uint8_t i = 0; i < tc->n_supers; i++) {
            const Form *f = tc->super_forms[i];
            const char *sname = f->as.list.items[0]->as.sym->name;
            TypeClass *s = tc->supers ? tc->supers[i] : NULL;
            if (!s) {
                diag_emit_with_code(DIAG_ERROR, f->span,
                    TUR_E0391_CLASS_SUPERCLASS_UNRESOLVED,
                    "superclass '%s' of typeclass '%s' is not a defined typeclass",
                    sname, tc->name->name);
                ok = false;
                continue;
            }
            uint8_t n_args = tc->super_n_args[i];
            if (s->n_type_params != n_args) {
                diag_emit_with_code(DIAG_ERROR, f->span,
                    TUR_E0391_CLASS_SUPERCLASS_UNRESOLVED,
                    "superclass '%s' takes %u type parameter(s), but typeclass "
                    "'%s' applies it to %u",
                    sname, (unsigned)s->n_type_params, tc->name->name,
                    (unsigned)n_args);
                ok = false;
                continue;
            }
            for (uint8_t k = 0; k < n_args; k++) {
                uint8_t idx = tc->super_arg_idx[i * TUR_SUPER_MAX_ARGS + k];
                Kind want = s->type_param_kinds ? s->type_param_kinds[k] : KIND_STAR;
                Kind have = tc->type_param_kinds ? tc->type_param_kinds[idx] : KIND_STAR;
                if (want != have) {
                    diag_emit_with_code(DIAG_ERROR, f->as.list.items[1 + k]->span,
                        TUR_E0391_CLASS_SUPERCLASS_UNRESOLVED,
                        "superclass '%s' expects a parameter of kind '%s' here, "
                        "but '%s' of typeclass '%s' has kind '%s'",
                        sname, sc_kind_name(s->type_param_kinds, k),
                        tc->type_params[idx]->name, tc->name->name,
                        sc_kind_name(tc->type_param_kinds, idx));
                    ok = false;
                }
            }
        }
    }
    if (!ok) return false;

    /* SC2 (2): the graph must be a DAG.  Each cycle is reported once, from
     * the first class on it the registry walk reaches; the others on the same
     * cycle are remembered so they do not report it again. */
    {
        TypeClass *reported[64];
        uint32_t   n_reported = 0;
        for (TypeClass *tc = env->typeclasses; tc; tc = tc->next) {
            if (tc->n_supers == 0) continue;
            bool skip = false;
            for (uint32_t r = 0; r < n_reported && !skip; r++) skip = (reported[r] == tc);
            if (skip) continue;
            TypeClass *path[16];
            uint8_t n = typeclass_find_super_cycle(tc, path, 16);
            if (n == 0) continue;
            char buf[512];
            size_t used = 0;
            buf[0] = '\0';
            for (uint8_t k = 0; k < n && used + 1 < sizeof(buf); k++) {
                int w = snprintf(buf + used, sizeof(buf) - used, "%s%s",
                                 k ? " -> " : "", path[k]->name->name);
                if (w < 0) break;
                used += (size_t)w;
                if (used >= sizeof(buf)) break;
                if (n_reported < 64) reported[n_reported++] = path[k];
            }
            const Form *at = path[0]->decl_form ? path[0]->decl_form
                                                : path[0]->super_forms[0];
            diag_emit_with_code(DIAG_ERROR, at->span,
                TUR_E0392_CLASS_SUPERCLASS_CYCLE,
                "typeclass superclass cycle: %s -- the superclass graph must be "
                "acyclic", buf);
            ok = false;
        }
    }
    if (!ok) return false;

    /* SC4 (Half B): every instance of a class with a preamble needs, for each
     * DIRECT superclass, an instance applying to the same type arguments.
     * Transitivity follows by induction, because every instance in the
     * registry is checked and the superclass instance found here is itself
     * one of them.  The lookup is the ordinary instance-applies machinery --
     * parametric heads, holes, and the instance's own declared constraints
     * included -- not a second matcher; a type variable in the required
     * position is accepted tentatively there, which is exactly how
     * `Monoid [(Vec A)] [(Monoid A)]` discharges against a parametric
     * `Semigroup [(Vec A)]`. */
    for (TypeClassInstance *inst = env->instances; inst; inst = inst->next) {
        TypeClass *c = inst->typeclass;
        if (!c || c->n_supers == 0 || !c->supers || inst->super_obligations_ok)
            continue;
        bool inst_ok = true;
        for (uint8_t i = 0; i < c->n_supers; i++) {
            TypeClass *s = c->supers[i];
            if (!s) { inst_ok = false; continue; }
            uint8_t n_args = c->super_n_args[i];
            Type          args[TUR_SUPER_MAX_ARGS];
            const Symbol *arg_syms[TUR_SUPER_MAX_ARGS];
            bool mapped = true;
            for (uint8_t k = 0; k < n_args; k++) {
                uint8_t idx = c->super_arg_idx[i * TUR_SUPER_MAX_ARGS + k];
                if (idx >= inst->n_type_args || !inst->type_args) { mapped = false; break; }
                args[k]     = inst->type_args[idx];
                arg_syms[k] = inst->type_arg_syms ? inst->type_arg_syms[idx] : NULL;
            }
            if (!mapped) { inst_ok = false; continue; }
            if (typeclass_env_lookup_instance(env, s, args, n_args)) continue;
            char head[256], need[256];
            sc_type_args_str(inst->type_args, inst->type_arg_syms,
                             inst->n_type_args, head, sizeof(head));
            sc_type_args_str(args, arg_syms, n_args, need, sizeof(need));
            const Form *at = inst->decl_form ? inst->decl_form : c->decl_form;
            diag_emit_with_code(DIAG_ERROR, at ? at->span : c->super_forms[i]->span,
                TUR_E0393_CLASS_SUPERCLASS_INSTANCE_MISSING,
                "(definstance %s [%s]) requires a %s [%s] instance ('%s' is a "
                "superclass of '%s'); none is in scope. Add (definstance %s [%s] "
                "...) -- it may appear anywhere in the program.",
                c->name->name, head, s->name->name, need,
                s->name->name, c->name->name, s->name->name, need);
            inst_ok = false;
        }
        if (inst_ok) inst->super_obligations_ok = true;
        else         ok = false;
    }
    return ok;
}
