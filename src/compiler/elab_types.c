/* elab_types.c -- type-expression forms: defkind/defrec/deftype/type-app, ascribe/pack/open. */
#include <stdint.h>
#include "elab_internal.h"
#include "experiments.h"  /* Slice 1 (constrained-hkt-forall): forall-kinds gate */

/* --- Slice 1 (constrained-hkt-forall-plan): kind-annotation parsing ---------
 *
 * A `forall`/`exists` bound variable may carry an explicit kind annotation
 * spelled `(name :: <kind>)`, where <kind> is the arrow-kind grammar built
 * from `*` and `->`:
 *
 *   *                 -> KIND_STAR      (arity 0)
 *   * -> *            -> KIND_ARROW     (arity 1)
 *   * -> * -> *       -> KIND_ARROW2    (arity 2)
 *   (* -> *) -> *     -> KIND_ARROW     (arity 1; the argument is itself
 *                                        higher-kinded, but a constructor's
 *                                        *arity* only counts its arguments)
 *
 * The `Kind` encoding (types.h) is arity-only: it records how many type
 * arguments a constructor takes, not the kinds of those arguments.  So a
 * parenthesized higher-order argument is validated for well-formedness but
 * contributes exactly one to the outer arity, same as a bare `*`.
 *
 * Diagnostics: TUR-E0303 (unknown/malformed kind form), TUR-E0304 (arity
 * exceeds KIND_ARROW5 -- raise the ceiling first).  Both are emitted at the
 * offending span; the helpers return false without touching *out on failure. */

static bool parse_kind_seq(Elab *e, Form **items, uint32_t n, Span span,
                           Kind *out);

/* Parse a single kind atom: `*` or a parenthesized kind sub-expression. */
static bool parse_kind_atom(Elab *e, Form *f, Kind *out) {
    if (f->tag == F_SYM) {
        if (strcmp(f->as.sym->name, "*") == 0) { *out = KIND_STAR; return true; }
        diag_emit(DIAG_ERROR, f->span,
                  "malformed kind form: expected '*' or a parenthesized kind, "
                  "got '%s' (TUR-E0303)", f->as.sym->name);
        return false;
    }
    if (f->tag == F_LIST) {
        return parse_kind_seq(e, f->as.list.items, f->as.list.len, f->span, out);
    }
    diag_emit(DIAG_ERROR, f->span,
              "malformed kind form: expected '*' or a parenthesized kind "
              "(TUR-E0303)");
    return false;
}

/* Parse a top-level arrow-kind sequence `atom (-> atom)*`.  The overall arity
 * is the number of arrows (== #atoms - 1); each atom is validated but its own
 * internal arity does not fold into the outer count. */
static bool parse_kind_seq(Elab *e, Form **items, uint32_t n, Span span,
                           Kind *out) {
    if (n == 0) {
        diag_emit(DIAG_ERROR, span,
                  "malformed kind form: empty kind (TUR-E0303)");
        return false;
    }
    /* Even length means a dangling '->' with no trailing atom (or, for n==2,
     * an atom followed by '->').  A well-formed chain always has odd length:
     * atom (-> atom)* == 1 + 2k tokens. */
    if ((n & 1u) == 0) {
        diag_emit(DIAG_ERROR, span,
                  "malformed kind form: kind must be `*` or `k -> k -> ... -> *`, "
                  "not ending in '->' (TUR-E0303)");
        return false;
    }
    uint32_t n_atoms = 0;
    for (uint32_t i = 0; i < n; i++) {
        if ((i & 1u) == 0) {
            Kind sub;
            if (!parse_kind_atom(e, items[i], &sub)) return false;
            n_atoms++;
        } else if (items[i]->tag != F_SYM || items[i]->as.sym != e->sym_arrow) {
            diag_emit(DIAG_ERROR, items[i]->span,
                      "malformed kind form: expected '->' between kind atoms "
                      "(TUR-E0303)");
            return false;
        }
    }
    uint32_t arity = n_atoms - 1;
    if (arity > 5) {
        diag_emit(DIAG_ERROR, span,
                  "kind arity %u exceeds KIND_ARROW5 -- raise the ceiling first "
                  "(TUR-E0304)", (unsigned)arity);
        return false;
    }
    *out = kind_for_arity(arity);
    return true;
}

/* Parse a kind-annotated forall/exists binder `(name :: <kind>)`.  On success
 * writes the bound name symbol to *out_name and its kind to *out_kind; returns
 * false (after emitting a diagnostic) on any malformed shape. */
static bool parse_kind_binder(Elab *e, Form *vf, const Symbol **out_name,
                              Kind *out_kind) {
    /* Shape: (name :: <kind-tokens...>) -- at least name, '::', one atom. */
    if (vf->as.list.len < 3) {
        diag_emit(DIAG_ERROR, vf->span,
                  "kind-annotated bound variable must be `(name :: <kind>)`, "
                  "e.g. (f :: * -> *) (TUR-E0303)");
        return false;
    }
    Form *name_f = vf->as.list.items[0];
    Form *dc_f   = vf->as.list.items[1];
    if (name_f->tag != F_SYM) {
        diag_emit(DIAG_ERROR, name_f->span,
                  "kind-annotated bound variable name must be a symbol "
                  "(TUR-E0303)");
        return false;
    }
    if (dc_f->tag != F_SYM || dc_f->as.sym != e->sym_ascribe) {
        diag_emit(DIAG_ERROR, dc_f->span,
                  "kind-annotated bound variable must use `::` between the name "
                  "and its kind, e.g. (f :: * -> *) (TUR-E0303)");
        return false;
    }
    Kind k;
    if (!parse_kind_seq(e, vf->as.list.items + 2, vf->as.list.len - 2,
                        vf->span, &k))
        return false;
    *out_name = name_f->as.sym;
    *out_kind = k;
    return true;
}

/* Variadic HKT rows: validate one type-application argument's kind against the
 * constructor's positional parameter kind, for the row concern only. Returns
 * false (and emits TUR-E0012) when a row-of-types argument (kind [*]) is given
 * to a non-row-kinded parameter, or a row-kinded ('^&') parameter is given a
 * non-row argument. Scoped to KIND_TYPEROW mismatches (xor) so existing,
 * non-row applications are never perturbed. arg_index is 0-based.
 * Shared by type_expr_from_form and fn_type_from_form's application paths. */
bool check_row_type_arg_kind(Type ctor_type, uint8_t arg_index, Type arg_type,
                             Span arg_span) {
    /* Pull the parameter kinds out of whichever def shape ctor_type carries.
     * L6 follow-up C extends this from TY_STRUCT to TY_ADT so a defgadt or
     * defdata with a `^&name` row-kinded parameter gets the same xor-scoped
     * row/non-row kind check at its application sites. TY_REC (deftype) does
     * not yet carry a per-param kind array; its apply-site validation falls
     * back to the arrow-arity check in kind_apply_one (a follow-up that
     * needs a kinds array on TY_REC, not in this layer). */
    const Kind *param_kinds = NULL;
    uint8_t     n_params    = 0;
    const char *ctor_name   = NULL;
    if (ctor_type.kind == TY_ADT && ctor_type.as.adt_.def) {
        const AdtDef *adef = ctor_type.as.adt_.def;
        param_kinds = adef->type_param_kinds;
        n_params    = adef->n_type_params;
        ctor_name   = adef->name;
    } else {
        return true;
    }
    if (!param_kinds || arg_index >= n_params) return true;
    Kind param_kind = param_kinds[arg_index];
    bool param_row = (param_kind == KIND_TYPEROW);
    bool arg_row   = (arg_type.hkt_kind == KIND_TYPEROW);
    if (param_row == arg_row) return true;
    diag_emit_with_code(DIAG_ERROR, arg_span, TUR_E0012_KIND_MISMATCH,
        "kind mismatch (TUR-E0012): type constructor '%s' parameter %u expects "
        "kind '%s', but the argument has kind '%s'",
        ctor_name, (unsigned)(arg_index + 1),
        kind_to_string(param_kind), kind_to_string(arg_type.hkt_kind));
    return false;
}

/* MF4: separate struct / GADT namespaces. Resolve a type name by walking
 * the ADT registry first, then the struct registry. When the same name is
 * registered as both a defgadt and a defstruct, the GADT wins -- a name
 * appearing in a type annotation almost always means the GADT (the
 * struct's phantom-type projection is rare; a qualified `Struct/Vec` form
 * can be added later if needed). Returns an arena-allocated Type* (TY_ADT
 * or TY_STRUCT), or NULL when neither registry has the name. */
Type *elab_lookup_type_by_name(Elab *e, const Symbol *name) {
    if (!name) return NULL;
    /* CONV-S1 (defstruct-as-defadt): a `defstruct` and a `defgadt`/`defdata`
     * may share a name -- the struct lowers to a struct-origin record ADT and
     * the hand-written ADT/GADT supersedes it (the GADT-wins rule).  Both can
     * live in e->adt_defs, so prefer a NON-superseded, non-struct-origin match
     * and only fall back to a struct-origin def when no winner of that name
     * exists.  A superseded def is never returned (its winner shadows it). */
    AdtDef *struct_origin_fallback = NULL;
    for (uint32_t i = 0; i < e->n_adt_defs; i++) {
        AdtDef *d = e->adt_defs[i];
        if (d && d->name && strcmp(d->name, name->name) == 0) {
            if (d->superseded) continue;
            if (d->from_struct_lowering) {
                if (!struct_origin_fallback) struct_origin_fallback = d;
                continue;
            }
            Type *t = (Type *)arena_alloc(e->arena, sizeof(Type));
            *t = type_adt(d);
            /* Phase G1/HKT: parameterized GADTs need a non-* kind so kind
             * checks against type-constructor arg slots stay consistent
             * with how defgadt itself stamps the binding. */
            t->hkt_kind = kind_for_arity(d->n_type_params);
            return t;
        }
    }
    if (struct_origin_fallback) {
        Type *t = (Type *)arena_alloc(e->arena, sizeof(Type));
        *t = type_adt(struct_origin_fallback);
        t->hkt_kind = kind_for_arity(struct_origin_fallback->n_type_params);
        return t;
    }
    return NULL;
}

/* Phase HKT H5: (defkind Name kind-expr)
 *
 * Registers a kind alias.  The kind-expr is currently stored as a string
 * for diagnostic purposes but not enforced at use sites (future phases).
 * Syntax: (defkind F1 (* -> *))
 *         (defkind F2 (* -> * -> *))
 */
Expr *elab_defkind(Elab *e, const Form *call) {
    /* Minimum: (defkind Name kind-expr) */
    if (call->as.list.len < 3) {
        diag_emit(DIAG_ERROR, call->span,
                  "defkind requires (defkind Name kind-expr)");
        return NULL;
    }

    /* Validate name */
    Form *name_form = call->as.list.items[1];
    if (name_form->tag != F_SYM) {
        diag_emit(DIAG_ERROR, name_form->span,
                  "defkind: name must be a symbol");
        return NULL;
    }

    /* kind-expr is accepted in any form (list, symbol, etc.) — parsed for
     * documentation only.  Currently treated as a no-op. */
    (void)call->as.list.items[2];

    /* Return nil — defkind has no runtime effect. */
    return e_nil(e, call->span);
}

/* Phase TA1/TA2: (defalias Name <type-expr>)
 *
 * Declares a *transparent* type alias.  The alias is stored in
 * e->type_alias_names / e->type_alias_kinds / e->type_alias_types and consulted
 * during type-annotation resolution in type_expr_from_form and the elab_fns
 * parameter/return ladders; every use site sees the target type itself, so the
 * alias never participates in unification as a nominal type of its own.
 *
 * TA1 accepted primitive keywords only:
 *
 *   (defalias Sample :int)
 *   (defalias Timestamp :int64)
 *
 * TA2 (composite-type-alias-gap) accepts any type expression the elaborator can
 * resolve -- type applications, struct/ADT names, function types, refinements:
 *
 *   (defalias IntList (Cons int))
 *   (defalias Point P)                    ; P declared by defstruct
 *   (defalias Backtrack (fn [int] int))
 *   (defalias NonZero #refine{ x : int | (not= x 0) })
 *
 * `deftype` stays what it always was -- the recursive type binder (TY_REC).
 * The two forms are disjoint: `defalias` is transparent, `deftype` is
 * recursive-nominal.  Alias *parameters* -- `(defalias Name [a] ...)` -- are
 * not supported; a parameterised alias would need type-level substitution at
 * every use site, which nothing in the resolver does today.
 */
Expr *elab_defalias(Elab *e, const Form *call) {
    /* Minimum: (defalias Name <type-expr>) — 3 elements */
    if (call->as.list.len < 3) {
        diag_emit(DIAG_ERROR, call->span,
                  "defalias requires (defalias Name <type-expr>)");
        return NULL;
    }

    /* Parse alias name */
    Form *name_form = call->as.list.items[1];
    if (name_form->tag != F_SYM) {
        diag_emit(DIAG_ERROR, name_form->span,
                  "defalias: name must be a symbol");
        return NULL;
    }
    const Symbol *alias_name = name_form->as.sym;

    /* A `deftype`-shaped parameter vector -- (defalias Name [a] body) -- is not
     * a defalias.  Name it rather than letting the vector fall through to the
     * type-expression parser and produce a confusing downstream error. */
    Form *type_form = call->as.list.items[2];
    if (type_form->tag == F_VEC && call->as.list.len > 3) {
        diag_emit(DIAG_ERROR, type_form->span,
                  "defalias '%s': type parameters are not supported -- a "
                  "defalias target must be a fully applied type "
                  "(e.g. (defalias IntList (Cons int)))",
                  alias_name->name);
        return NULL;
    }

    /* Resolve the target.  Primitive keywords keep the TA1 fast path so the
     * exact TypeKind (and nothing else) lands in the table; everything else
     * goes through the full type-expression resolver. */
    Type *target = NULL;
    if (type_form->tag == F_KEYWORD) {
        TypeKind prim = typekind_from_symbol(type_form->as.sym->name);
        if (prim != TY_UNKNOWN) {
            target = (Type *)arena_alloc(e->arena, sizeof(Type));
            *target = type_from_kind(prim);
        }
    }
    if (!target) {
        target = type_expr_from_form(e, type_form, NULL, NULL, NULL, 0);
        if (!target) {
            return NULL;
        }
        /* An alias to itself never terminates at a use site.  Checked before
         * the tyvar-echo test below so the recursive case gets the diagnostic
         * that names the right remedy rather than a generic "unknown type". */
        if ((type_form->tag == F_SYM || type_form->tag == F_KEYWORD) &&
            type_form->as.sym == alias_name) {
            diag_emit(DIAG_ERROR, type_form->span,
                      "defalias '%s': an alias cannot refer to itself -- use "
                      "deftype for a recursive type",
                      alias_name->name);
            return NULL;
        }
        /* type_expr_from_form's last resort for an unknown name is a TY_TYVAR
         * carrying that very name.  Aliasing to it would silently accept a
         * typo -- `(defalias Bad :not-a-type)` must stay an error -- so reject
         * a tyvar that is just the target spelling echoed back.  (A *nested*
         * unknown name, `(defalias Foo (Cons Bar))`, still lands on the same
         * unresolved-tyvar fallback every other type position uses.) */
        if (target->kind == TY_TYVAR &&
            (type_form->tag == F_SYM || type_form->tag == F_KEYWORD) &&
            target->as.tyvar_.name &&
            strcmp(target->as.tyvar_.name, type_form->as.sym->name) == 0) {
            diag_emit(DIAG_ERROR, type_form->span,
                      "defalias: '%s' is not a recognised type",
                      type_form->as.sym->name);
            return NULL;
        }
        /* Same fallback, one level in: `(defalias L (list int))` applies a name
         * that is not a type constructor, so the head of the TY_APP chain is an
         * unresolved tyvar.  The direct annotation `[xs : (list int)]` reports
         * this as a kind mismatch; without this the alias would swallow the
         * declaration-site error and only surface a confusing unification
         * failure at the first use.  No type parameters are in scope here, so a
         * tyvar head can only ever mean an unknown name. */
        {
            const Type *head = target;
            while (head && head->kind == TY_APP) head = head->as.app.fn;
            if (head && head->kind == TY_TYVAR && head->as.tyvar_.name &&
                target->kind == TY_APP) {
                diag_emit(DIAG_ERROR, type_form->span,
                          "defalias '%s': '%s' is not a type constructor -- "
                          "it cannot be applied to type arguments",
                          alias_name->name, head->as.tyvar_.name);
                return NULL;
            }
        }
    }

    /* Grow the alias table if needed */
    if (e->n_type_aliases >= e->cap_type_aliases) {
        uint32_t new_cap = e->cap_type_aliases ? e->cap_type_aliases * 2 : 8;
        const Symbol **new_names = (const Symbol **)arena_alloc(e->arena,
            new_cap * sizeof(const Symbol *));
        TypeKind *new_kinds = (TypeKind *)arena_alloc(e->arena,
            new_cap * sizeof(TypeKind));
        Type **new_types = (Type **)arena_alloc(e->arena,
            new_cap * sizeof(Type *));
        if (e->n_type_aliases > 0) {
            memcpy(new_names, e->type_alias_names,
                   e->n_type_aliases * sizeof(const Symbol *));
            memcpy(new_kinds, e->type_alias_kinds,
                   e->n_type_aliases * sizeof(TypeKind));
            memcpy(new_types, e->type_alias_types,
                   e->n_type_aliases * sizeof(Type *));
        }
        e->type_alias_names = new_names;
        e->type_alias_kinds = new_kinds;
        e->type_alias_types = new_types;
        e->cap_type_aliases = new_cap;
    }
    e->type_alias_names[e->n_type_aliases] = alias_name;
    e->type_alias_kinds[e->n_type_aliases] = target->kind;
    e->type_alias_types[e->n_type_aliases] = target;
    e->n_type_aliases++;

    /* defalias has no runtime effect — return nil */
    return e_nil(e, call->span);
}

/* Elaborate (defrec Name [params])
 *
 * Defines a recursive type constructor Name and registers it in global scope
 * as a TY_REC binding with kind KIND_ARROW (i.e., kind * -> *).
 * In v1 the body expression is accepted but not evaluated; TY_REC.body = NULL.
 *
 * Syntax: (defrec Fix [^f])
 *         (defrec Name [params] body-ignored-in-v1)
 */
Expr *elab_defrec(Elab *e, const Form *call) {
    /* Minimum: (defrec Name) */
    if (call->as.list.len < 2) {
        diag_emit(DIAG_ERROR, call->span,
                  "defrec requires (defrec Name [params])");
        return NULL;
    }

    /* Validate name */
    Form *name_form = call->as.list.items[1];
    if (name_form->tag != F_SYM) {
        diag_emit(DIAG_ERROR, name_form->span,
                  "defrec: name must be a symbol");
        return NULL;
    }
    const Symbol *name = name_form->as.sym;

    /* Accept params if present (arg 2) — validate is a list/vector */
    uint32_t body_idx = 2;
    if (call->as.list.len >= 3) {
        Form *params_f = call->as.list.items[2];
        if (params_f->tag != F_LIST && params_f->tag != F_VEC) {
            diag_emit(DIAG_ERROR, params_f->span,
                      "defrec: expected parameter list");
            return NULL;
        }
        body_idx = 3;
    }

    /* Parse body form (arg body_idx) — optional in v1 */
    /* The body can reference the recursive type itself, creating a circular dependency.
     * In v1, we just store the form without elaborating it. */
    Form *body_form = (call->as.list.len >= body_idx + 1) ? call->as.list.items[body_idx] : NULL;

    /* Create TY_REC type: kind * -> * (recursive type constructor) */
    Type rec_type;
    memset(&rec_type, 0, sizeof(rec_type));
    rec_type.kind      = TY_REC;
    rec_type.copy_kind = CK_MOVE;
    rec_type.hkt_kind  = KIND_ARROW;    /* defrec types are kind * -> * */
    rec_type.as.rec.name = name->name;
    rec_type.as.rec.body = NULL;        /* v1: body not yet evaluated, but we store the form for future use */
    
    /* If a body was provided, store it for later elaboration */
    if (body_form) {
        /* For now, we just note that a body exists but don't elaborate it */
        /* This is a placeholder for when full recursive type support is added */
    }

    /* Register in global scope */
    Binding *b = binding_new(e, name, rec_type, false, true, name_form->span);
    scope_add(&e->global, b);

    /* defrec has no runtime effect */
    return e_nil(e, call->span);
}

/* c-fn-ptr-element-and-size-precision-gap fix: map a pointer-width integer type
 * name to its precise FFI C spelling (size_t / ptrdiff_t).  Returns
 * CNUM_DEFAULT for every other name.  Kept in sync with the usize/isize
 * carrier entries in typekind_from_symbol -- the carrier (TY_UINT64 / TY_INT64)
 * comes from there; this only adds the precise spelling hint. */
static uint8_t c_num_spelling_for_name(const char *name) {
    if (strcmp(name, "usize") == 0 || strcmp(name, "size")  == 0) return CNUM_SIZE_T;
    if (strcmp(name, "isize") == 0 || strcmp(name, "ssize") == 0) return CNUM_PTRDIFF_T;
    return CNUM_DEFAULT;
}

/* ptr-generic-parameterised-type: parse a "ptr<T>" type-name string into a
 * typed raw pointer Type (kind TY_PTR_VOID with a non-NULL pointee).  The
 * pointee spelling carries no leading colon -- e.g. "ptr<float>",
 * "ptr<Pair>", "ptr<ptr<float>>".  The inner type is resolved recursively
 * through type_expr_from_form so primitives, structs, ADTs, type variables,
 * and nested pointers all work.
 *
 * Returns:
 *   - a non-NULL Type* for a well-formed typed pointer,
 *   - NULL when `name` is not a "ptr<...>" form, OR is the legacy "ptr<void>"
 *     spelling (left to the existing TY_PTR_VOID code paths).
 * Emits a diagnostic and returns NULL on a malformed/empty pointee. */
Type *ptr_type_from_keyword_name(Elab *e, const char *name, uint32_t len,
                                 Span span, const Symbol *rec_name,
                                 const Symbol **type_params,
                                 Kind *type_param_kinds, uint8_t n_type_params) {
    /* shortest typed form is "ptr<x>" == 6 chars */
    if (len < 6) return NULL;
    if (memcmp(name, "ptr<", 4) != 0) return NULL;
    if (name[len - 1] != '>') return NULL;
    const char *inner = name + 4;
    uint32_t inner_len = len - 5;            /* between "ptr<" and the final '>' */
    /* c-fn-ptr-element-and-size-precision-gap fix: a `const-` prefix on the
     * pointee marks a const-qualified pointer (`ptr<const-u8>` -> `const u8 *`).
     * The space-free `const-` spelling is used because `<` / `>` are
     * symbol-continuation chars but ':' and ' ' are not, so the whole
     * `ptr<...>` must tokenize as one keyword/symbol. */
    bool is_const = false;
    if (inner_len > 6 && memcmp(inner, "const-", 6) == 0) {
        is_const = true;
        inner += 6;
        inner_len -= 6;
    }
    /* Legacy untyped pointer: defer to the existing TY_PTR_VOID handling --
     * except `ptr<const-void>`, which needs a const-qualified void* and so is
     * built here (the bare ptr<void> path cannot carry the const flag). */
    if (inner_len == 4 && memcmp(inner, "void", 4) == 0) {
        if (!is_const) return NULL;
        Type *t = (Type *)arena_alloc(e->arena, sizeof(Type));
        *t = type_simple(TY_PTR_VOID, CK_COPY);
        t->as.ptr.inner = NULL;
        t->as.ptr.is_const = true;
        return t;
    }
    if (inner_len == 0) {
        diag_emit(DIAG_ERROR, span, "empty pointee type in 'ptr<...>'");
        return NULL;
    }
    /* Resolve the inner type by recursing on a synthetic symbol form whose
     * name is the pointee spelling.  An F_SYM routes through the same
     * primitive / type-param / struct / ADT / nested-ptr resolution. */
    const Symbol *inner_sym = symtab_intern(e->st, strslice(inner, inner_len));
    Form inner_form;
    memset(&inner_form, 0, sizeof(inner_form));
    inner_form.tag = F_SYM;
    inner_form.span = span;
    inner_form.as.sym = inner_sym;
    Type *pointee = type_expr_from_form(e, &inner_form, rec_name,
                                        type_params, type_param_kinds, n_type_params);
    if (!pointee) return NULL;
    Type *t = (Type *)arena_alloc(e->arena, sizeof(Type));
    *t = type_ptr(pointee);
    t->as.ptr.is_const = is_const;
    return t;
}

/* rc-angle-bracket-annotation-becomes-tyvar: resolve a typed reference-family
 * keyword annotation -- `rc<T>`, `weak<T>`, `ref<T>`, `lref<T>` -- to its real
 * TY_RC/TY_WEAK/TY_REF/TY_LREF Type carrying the inner pointee kind.  This
 * mirrors `ptr<T>` (ptr_type_from_keyword_name) and the struct-field parser
 * (parse_struct_field_type), both of which already accept the angle-bracket
 * spelling.  The defn/fn param and return ladders were the one place that did
 * NOT: a `rc<int>` annotation there fell through to a fresh type variable named
 * "rc<int>".  Under expected-type pressure that tyvar unifies with a real rc
 * value (so `(sink r)` / a struct field of type rc "work"), but any post-hoc
 * `rc/...` builtin sees the bare tyvar and errors with a confusing "got tyvar"
 * -- a silent footgun.  Returns NULL when `name` is not one of these
 * angle-bracket forms, so the caller falls through to its existing resolution
 * (bare `rc`, tyvar, alias, ...). */
Type *rc_family_type_from_keyword_name(Elab *e, const char *name, uint32_t len,
                                       Span span, const Symbol *rec_name,
                                       const Symbol **type_params,
                                       Kind *type_param_kinds, uint8_t n_type_params) {
    /* shortest typed form is "rc<x>" == 5 chars; must end in '>' */
    if (len < 5 || name[len - 1] != '>') return NULL;
    TypeKind family = TY_UNKNOWN;
    uint32_t prefix_len = 0;
    if (len > 3 && memcmp(name, "rc<", 3) == 0)        { family = TY_RC;   prefix_len = 3; }
    else if (len > 4 && memcmp(name, "ref<", 4) == 0)  { family = TY_REF;  prefix_len = 4; }
    else if (len > 5 && memcmp(name, "lref<", 5) == 0) { family = TY_LREF; prefix_len = 5; }
    else if (len > 5 && memcmp(name, "weak<", 5) == 0) { family = TY_WEAK; prefix_len = 5; }
    if (family == TY_UNKNOWN) return NULL;

    const char *inner = name + prefix_len;
    uint32_t inner_len = len - prefix_len - 1;   /* between "rc<" and the final '>' */
    if (inner_len == 0) {
        diag_emit(DIAG_ERROR, span, "empty inner type in '%.*s<...>'",
                  (int)(prefix_len - 1), name);
        return NULL;
    }
    /* Resolve the inner spelling through the same synthetic-F_SYM path as
     * ptr<T>: primitive / type-param / struct / ADT / nested resolution. */
    const Symbol *inner_sym = symtab_intern(e->st, strslice(inner, inner_len));
    Form inner_form;
    memset(&inner_form, 0, sizeof(inner_form));
    inner_form.tag = F_SYM;
    inner_form.span = span;
    inner_form.as.sym = inner_sym;
    Type *pointee = type_expr_from_form(e, &inner_form, rec_name,
                                        type_params, type_param_kinds, n_type_params);
    if (!pointee) return NULL;

    Type *t = (Type *)arena_alloc(e->arena, sizeof(Type));
    /* rc<ADT>/rc<Struct-lowered-to-ADT> carries the def so field access through
     * the rc receiver auto-derefs -- parity with a defstruct rc<Struct> field
     * (elab_structs.c adt_rc_inner_full_type). */
    if (family == TY_RC && pointee->kind == TY_ADT && pointee->as.adt_.def) {
        *t = type_rc_adt(pointee->as.adt_.def);
        return t;
    }
    switch (family) {
        case TY_RC:   *t = type_rc(pointee->kind);   break;
        case TY_WEAK: *t = type_weak(pointee->kind); break;
        case TY_REF:  *t = type_ref(pointee->kind);  break;
        case TY_LREF: *t = type_lref(pointee->kind); break;
        default: return NULL;
    }
    return t;
}

/* fn-type-bare-identifier-plan Phase 4: a type slot inside a (fn ...) type
 * expression that carries a leading colon -- the fused `:int` keyword form or
 * the spaced-but-still-redundant `: int` F_TYPE_ANN wrapper -- is rejected.
 * Position alone marks the slot as a type, so the colon is noise.  The
 * preceding minor releases (v0.18.x / v0.19.x) carried this as a TUR-D0001
 * deprecation warning; Phase 4 promotes it to a hard error.  Code stays as
 * TUR_D0001 so `tur explain TUR-D0001` and any tooling pinned on the code
 * keep working across the warning-to-error transition. */
/* A `(fn ...)` type cannot carry refinements.  Rejecting is deliberate: the
 * alternative is to PEEL and discard the predicate, which would accept the
 * syntax and silently drop the guarantee -- the worst of the three options,
 * because the annotation then reads like something that is being checked.
 *
 * Left unhandled entirely, the contract type survives into the signature and
 * every use of the value fails with `expected { _ : ? | ... }, got int`, naming
 * a type the programmer never wrote.  That is the fourth site to hit this;
 * CT0's note on `rt_peel_contract` lists the first three.  This one is not a
 * missing peel -- there is nowhere for the predicate to ride -- so it gets a
 * diagnostic that says what is actually unsupported. */
/* Peel a contract written in TYPE-ARGUMENT position -- `(Box #refine{...})`.
 *
 * This is the fifth annotation site to need a peel (the comment above
 * reject_fn_type_contract counts the first four).  Storing the TY_CONTRACT node
 * whole is what the old behavior did, and nothing between here and codegen
 * peeled it, so the payload a `match` arm binds stayed contract-typed and every
 * ordinary use of it failed: operator lookup (TUR-E0006), overload resolution,
 * and the register-class check (TUR-E0707) all compare kinds without peeling,
 * so a float-based contract even read as "non-float".  Peeling makes the
 * annotation inert instead of actively breaking the program.
 *
 * It WARNS rather than dropping the predicate quietly: peeling here gives up on
 * ever checking the payload, and an annotation that silently does nothing is
 * how a reader ends up believing a container's contents are checked.  Enforcing
 * it needs the refinement to survive as a type argument down to the unpacking
 * binder, plus a checked crossing where a ctor call's result type is matched
 * against a declared one -- a real feature, not a patch.
 *
 * Shared because there are TWO type-application loops: type_expr_from_form's
 * and fn_type_from_form_impl's (the latter never reaches the former's app
 * path).  Fixing only one leaves the parameter-annotation position broken,
 * which is the position that actually gets written.
 * See docs/archive/contract-type-arg-not-peeled-to-base.md. */
Type *rt_peel_type_arg_contract(Type *arg_type, Span at) {
    if (!arg_type || arg_type->kind != TY_CONTRACT) return arg_type;
    Type *peeled = rt_peel_contract(arg_type, NULL, NULL);
    if (peeled == arg_type) return arg_type;   /* malformed contract: leave it */
    diag_emit_with_code(DIAG_WARNING, at,
        TUR_W0380_REFINE_TYPE_ARG_UNENFORCED,
        "refinement in type-argument position is not enforced; the payload is "
        "treated as '%s'", type_name(*peeled));
    diag_emit(DIAG_NOTE, at,
        "refine a parameter, a return type, or a `let` binding instead -- "
        "those positions emit a check");
    return peeled;
}

static void reject_fn_type_contract(Elab *e, Type *t, const Form *slot,
                                    const char *what) {
    (void)e;
    if (!t || t->kind != TY_CONTRACT || !slot) return;
    diag_emit_with_code(DIAG_ERROR, slot->span, TUR_E0378_REFINE_IN_FN_TYPE,
                        "a refinement cannot be written on the %s of a "
                        "(fn ...) type; function types do not carry "
                        "refinements", what);
    diag_emit(DIAG_NOTE, slot->span,
              "a function value with refined parameters can still be passed "
              "and called -- its own entry checks run -- but the refinement "
              "is not visible through the function type, so the call is "
              "checked at run time rather than statically");
    /* Recover to the BASE type.  The compile already fails on the error above,
     * so this cannot make a bad program compile; what it prevents is the
     * cascade -- a surviving TY_CONTRACT makes every use of the value report
     * `expected { _ : ? | ... }, got int`, which is the confusing message this
     * diagnostic exists to replace, not to accompany. */
    if (t->as.contract_.base_type) *t = *t->as.contract_.base_type;
}

static void reject_fn_type_colon(Elab *e, const Form *slot) {
    (void)e;
    if (!slot) return;
    if (slot->tag != F_KEYWORD && slot->tag != F_TYPE_ANN) return;
    diag_emit_with_code(DIAG_ERROR, slot->span, TUR_D0001_FN_TYPE_COLON,
                        "leading colon on a type inside a (fn ...) type is "
                        "no longer accepted; drop it -- position already "
                        "marks this as a type (e.g. (fn [int] int), not "
                        "(fn [:int] :int))");
}

/* Helper: parse a type expression form into a Type for deftype body.
 * Supports:
 *   - Symbols: primitive types (int, bool, etc.) or type bindings
 *   - Type parameters: ^f, ^^f (stripped to f and looked up in type_params)
 *   - Type applications: (ctor arg) -> TY_APP
 *   - Recursive references: the type name being defined
 * Returns NULL on error. */

/* Does `t` mention a named type variable (so a projection over it cannot be
 * answered yet)? */
static bool assoc_arg_is_open(const Type *t) {
    if (!t) return false;
    switch (t->kind) {
        case TY_TYVAR: return t->as.tyvar_.name != NULL;
        case TY_APP:   return assoc_arg_is_open(t->as.app.fn) ||
                              assoc_arg_is_open(t->as.app.arg);
        default:       return false;
    }
}

bool elab_assoc_projection(Elab *e, const Symbol *assoc, const Type *args,
                           uint8_t n_args, Type *out) {
    if (!e || !assoc || !args || n_args != 1 || !out) return false;
    if (!assoc_arg_is_open(&args[0])) return false;
    Buf nb; buf_init(&nb);
    buf_printf(&nb, "(%s ", assoc->name);
    type_print(&nb, args[0]);
    buf_putc(&nb, ')');
    const Symbol *nm = symtab_intern(e->st, strslice(nb.data, (uint32_t)nb.len));
    buf_free(&nb);
    Type *ap = (Type *)arena_alloc(e->arena, sizeof(Type));
    *ap = args[0];
    Type t = type_tyvar_named(nm->name);
    t.as.tyvar_.assoc_of = assoc;
    t.as.tyvar_.assoc_arg = ap;
    *out = t;
    return true;
}

Type *type_expr_from_form(Elab *e, const Form *form, const Symbol *rec_name,
                                  const Symbol **type_params, Kind *type_param_kinds,
                                  uint8_t n_type_params) {
    /* ET3: bare `nil` literal (F_NIL) in a type expression means the nil/unit type */
    if (form->tag == F_NIL) {
        Type *t = (Type *)arena_alloc(e->arena, sizeof(Type));
        *t = TYPE_NIL;
        return t;
    }
    if (form->tag == F_SYM) {
        const Symbol *sym = form->as.sym;
        
        /* Check if it's the recursive type name */
        if (sym == rec_name) {
            /* Return a TY_REC type referencing itself (will be resolved later) */
            Type *t = (Type *)arena_alloc(e->arena, sizeof(Type));
            memset(t, 0, sizeof(Type));
            t->kind = TY_REC;
            t->copy_kind = CK_MOVE;
            t->hkt_kind = KIND_STAR; /* Will be updated based on context */
            t->as.rec.name = rec_name->name;
            t->as.rec.body = NULL; /* Circular - will be filled in later */
            return t;
        }
        
        /* Check if it's a type parameter (^f or ^^f) */
        if (sym->len > 1 && sym->name[0] == '^') {
            /* Strip the ^ prefix(es) and look up in type_params */
            const char *bare = NULL;
            uint32_t bare_len = 0;
            uint8_t param_idx = 0;
            bool found = false;
            
            if (sym->len > 2 && sym->name[0] == '^' && sym->name[1] == '^') {
                bare = sym->name + 2;
                bare_len = sym->len - 2;
                /* Look for the bare name in type_params (which stores the stripped name) */
                for (param_idx = 0; param_idx < n_type_params; param_idx++) {
                    if (type_params[param_idx] && 
                        type_params[param_idx]->len == bare_len &&
                        memcmp(type_params[param_idx]->name, bare, bare_len) == 0) {
                        found = true;
                        break;
                    }
                }
            } else if (sym->len > 1) {
                bare = sym->name + 1;
                bare_len = sym->len - 1;
                /* Look for the bare name in type_params */
                for (param_idx = 0; param_idx < n_type_params; param_idx++) {
                    if (type_params[param_idx] && 
                        type_params[param_idx]->len == bare_len &&
                        memcmp(type_params[param_idx]->name, bare, bare_len) == 0) {
                        found = true;
                        break;
                    }
                }
            }
            
            if (found) {
                /* Return a type variable.  structdef-retirement slice 5 B2:
                 * a `^f`/`^^f` higher-kinded type-param reference is a genuine
                 * named type variable, so emit a named TY_TYVAR carrying the
                 * param name (its constructor kind lives in hkt_kind) rather
                 * than a def-less TY_STRUCT placeholder. */
                Type *t = (Type *)arena_alloc(e->arena, sizeof(Type));
                *t = type_tyvar_named(type_params[param_idx]->name);
                t->copy_kind = CK_MOVE;
                t->hkt_kind = type_param_kinds[param_idx];
                return t;
            }
            /* If not found, fall through to look up as a type binding */
        }
        
        /* Check if it's a bare type parameter name (without ^ prefix) */
        if (n_type_params > 0) {
            for (uint8_t i = 0; i < n_type_params; i++) {
                if (type_params[i] && type_params[i] == sym) {
                    /* Direction A step 2a of
                     * docs/archive/history/open-binder-skolems-not-distinguishable.md:
                     * return a NAMED TY_TYVAR so binder identity survives down
                     * to the call-site unifier.  Previously this returned an
                     * anonymous TY_STRUCT{def=NULL}, which made every type-param
                     * reference indistinguishable -- nested `open` skolems
                     * collapsed under `type_eq`.  Prereq shims at sites 2/3/7
                     * /8/9/10/11 accept both shapes during the transition. */
                    Type *t = (Type *)arena_alloc(e->arena, sizeof(Type));
                    *t = type_tyvar_named(sym->name);
                    /* Prereq 5: tolerate NULL kinds. parse_typeclass_method
                     * passes class type params for return-type resolution
                     * but doesn't have the kinds array assembled yet (those
                     * are computed in elab_defclass). Default to KIND_STAR
                     * for type-param refs whose kind isn't supplied; the
                     * kind only matters for HKT-checking, which all uses
                     * of class type params route through KIND_STAR anyway. */
                    t->hkt_kind = type_param_kinds ? type_param_kinds[i] : KIND_STAR;
                    return t;
                }
            }
        }
        
        /* MF4: prefer the typed-registry lookup so struct / GADT namespaces
         * stay separate. When both a defstruct and a defgadt share a name,
         * the GADT wins in type-annotation context. Falls back to
         * scope_lookup so user-defined type aliases and forward
         * declarations continue to resolve as today. */
        {
            Type *resolved = elab_lookup_type_by_name(e, sym);
            if (resolved) return resolved;
        }
        Binding *b = scope_lookup(e->scope, sym);
        if (!b) {
            b = scope_lookup(&e->global, sym);
        }
        if (b) {
            /* Return a copy of the binding's type */
            Type *t = (Type *)arena_alloc(e->arena, sizeof(Type));
            *t = b->type;
            return t;
        }
        
        /* Primitive type names and defalias names.
         * Spaced annotations like [x : float] arrive here as bare symbols,
         * so keep this path in sync with the keyword-based resolver below. */
        {
            TypeKind k = typekind_from_symbol(sym->name);
            if (k == TY_UNKNOWN && sym->len == 4 && memcmp(sym->name, "void", 4) == 0) {
                k = TY_NIL;
            } else if (k == TY_UNKNOWN && sym->len == 9 && memcmp(sym->name, "ptr<void>", 9) == 0) {
                k = TY_PTR_VOID;
            }
            if (k != TY_UNKNOWN) {
                Type *t = (Type *)arena_alloc(e->arena, sizeof(Type));
                *t = type_from_kind(k);
                t->c_num_spelling = c_num_spelling_for_name(sym->name);
                return t;
            }
        }
        {
            const Symbol *alias_sym = symtab_intern(e->st, strslice(sym->name, sym->len));
            for (uint32_t ai = 0; ai < e->n_type_aliases; ai++) {
                if (e->type_alias_names[ai] == alias_sym) {
                    /* TA2: hand back the full target type, not just its kind --
                     * a composite alias ((list int), a struct, (fn [int] int))
                     * carries payload a bare TypeKind would drop. */
                    Type *t = (Type *)arena_alloc(e->arena, sizeof(Type));
                    *t = *e->type_alias_types[ai];
                    return t;
                }
            }
        }

        /* IT4: any — top type. */
        if (sym->len == 3 && memcmp(sym->name, "any", 3) == 0) {
            Type *t = (Type *)arena_alloc(e->arena, sizeof(Type));
            memset(t, 0, sizeof(Type));
            t->kind     = TY_ANY;
            t->copy_kind = CK_COPY;
            t->hkt_kind  = KIND_STAR;
            return t;
        }

        /* SS0b: Close — terminal session protocol (bare symbol, no arguments) */
        if (sym == e->sym_session_Close) {
            Type *t = (Type *)arena_alloc(e->arena, sizeof(Type));
            *t = type_close();
            return t;
        }

        /* SS3a: bare Rec label reference -- resolve to back-reference sentinel.
         * When we encounter a bare symbol that matches an active Rec binder label,
         * return a TY_SESSION_REC node with fst=NULL (sentinel for recursive call). */
        if (e->rec_depth > 0) {
            for (uint8_t ri = 0; ri < e->rec_depth; ri++) {
                if (e->rec_labels[ri] == sym) {
                    /* Return a sentinel TY_SESSION_REC with fst=NULL to indicate
                     * a recursive reference that will be unrolled at use time. */
                    Type *t = (Type *)arena_alloc(e->arena, sizeof(Type));
                    *t = type_session_rec(sym->name, NULL);
                    return t;
                }
            }
        }

        /* ptr-generic-parameterised-type: spaced/bare `ptr<T>` annotation. */
        {
            Type *pt = ptr_type_from_keyword_name(e, sym->name, sym->len, form->span,
                                                  rec_name, type_params,
                                                  type_param_kinds, n_type_params);
            if (pt) return pt;
        }

        /* rc<T>/weak<T>/ref<T>/lref<T> in ANY type position, not just the defn
         * param/return ladders that call the resolver directly (elab_fns.c).
         * Without this the reference-family spelling reached the tyvar fallback
         * below and became a type variable literally *named* "rc<S>" -- the
         * very footgun rc_family_type_from_keyword_name was written to close,
         * still open everywhere that routes through here: type-application
         * arguments, `::` ascriptions, aliases.
         *
         * The symptom was a real program silently mistyped rather than
         * rejected.  `(:: (vec-new) (Vec rc<S>))` bound the element tyvar A to
         * the *name* "rc<S>", so a later `(vec-push! v a)` with an honest rc<S>
         * value failed unification against its own annotation -- "expected
         * tyvar, got rc<S>" -- while `(Vec S)` and `(Vec int)` were fine.  The
         * error pointed at the argument when the annotation was at fault.
         * Placed beside the ptr<T> hook, which has the same shape and was
         * already wired in here. */
        {
            Type *rt = rc_family_type_from_keyword_name(e, sym->name, sym->len,
                                                        form->span, rec_name,
                                                        type_params,
                                                        type_param_kinds,
                                                        n_type_params);
            if (rt) return rt;
        }

        /* Unknown -- return an unresolved named type variable.
         * structdef-retirement slice 5 (P7): an unknown bare type name is an
         * unresolved type variable, not a nominal struct.  Emit a named
         * TY_TYVAR (codegen lowers it to the same int64 carrier) rather than a
         * def-less TY_STRUCT placeholder.  The knock-on: a container element
         * that lands here (`(Option Unknown)`) now stays polymorphic
         * (uniform-carrier `ctor_Option`) instead of minting a placeholder
         * `Option__struct` monomorph -- the more honest lowering. */
        if (e->strict_unknown_types) {
            diag_emit(DIAG_ERROR, form->span,
                      "unknown type name '%.*s' in #row{...} element position",
                      (int)sym->len, sym->name);
            return NULL;
        }
        Type *t = (Type *)arena_alloc(e->arena, sizeof(Type));
        *t = type_tyvar_named(sym->name);
        t->copy_kind = CK_MOVE;
        /* van-laarhoven-lens-composition: this bare name is not a LOCAL type
         * param, but it may be a higher-kinded type variable quantified by an
         * ENCLOSING fn signature -- e.g. a composed lens body's inner lambda
         * `(fn [p : Point] : (f Point) ...)` where `f : * -> *` is bound by the
         * outer rank-2 fn.  The enclosing signature already recorded `f`'s
         * constructor kind in `sig_tyvar_kinds` (before the body was elaborated),
         * so recover it here; otherwise `(f Point)` would apply a kind-* head and
         * trip TUR-E0012.  A genuinely free name keeps the kind-* default. */
        for (uint8_t si = 0; si < e->n_sig_tyvars; si++) {
            if (e->sig_tyvars[si] &&
                strcmp(e->sig_tyvars[si], sym->name) == 0) {
                if (e->sig_tyvar_kinds[si] != KIND_STAR)
                    t->hkt_kind = e->sig_tyvar_kinds[si];
                /* van-laarhoven-lens-composition: a name quantified by an
                 * ENCLOSING fn signature is an unconstrained type variable, so
                 * it must follow the same copy discipline as a LOCAL declared
                 * type param -- CK_COPY (see type_tyvar_named / the n_type_params
                 * branch above), NOT the CK_MOVE forced on genuinely-free unknown
                 * names.  Without this, an inner closure param typed by an outer
                 * tyvar (e.g. `(fn [s : S] ...)` in a lens `put`) is wrongly
                 * treated as affine and a legitimate double-use trips TUR-E0005. */
                t->copy_kind = CK_COPY;
                break;
            }
        }
        return t;
    } else if (form->tag == F_LIST) {
        /* Variadic HKT rows (Layer 5): type-level row algebra.
         *   (row-concat A B ...)    -- A ++ B ++ ...  (order-preserving, dup-keeping)
         *   (row-union A B ...)     -- set-union (deduplicated query join)
         *   (row-intersect A B ...) -- components common to all operands
         * Each operand must resolve to a #row{...} (kind [*]); the result is a
         * folded TY_TYPEROW usable wherever a row is (e.g. a row-kinded type
         * argument). Zero operands yield the empty row; one operand passes
         * through. */
        if (form->as.list.len >= 1 && form->as.list.items[0]->tag == F_SYM) {
            const char *h = form->as.list.items[0]->as.sym->name;
            /* L6 follow-up D: (row-canon R) elaborates R, requires it to be a
             * row, and returns a sorted-by-type_name copy. Two callers writing
             * (row-canon #row{a b}) and (row-canon #row{b a}) get identical
             * TY_TYPEROW values, so ordinary order-sensitive type_eq returns
             * true -- the opt-in surface for permutation equivalence at type
             * boundaries (signature unification, parameter passing).
             * Unary, so it's handled separately from the binary fold below. */
            if (strcmp(h, "row-canon") == 0) {
                if (form->as.list.len != 2) {
                    diag_emit(DIAG_ERROR, form->span,
                        "'row-canon' takes exactly one row argument: "
                        "(row-canon #row{...})");
                    return NULL;
                }
                Type *operand = type_expr_from_form(e, form->as.list.items[1],
                    rec_name, type_params, type_param_kinds, n_type_params);
                if (!operand) return NULL;
                if (operand->kind != TY_TYPEROW) {
                    diag_emit(DIAG_ERROR, form->as.list.items[1]->span,
                        "'row-canon' operand must be a #row{...} (kind [*]), "
                        "got a non-row type");
                    return NULL;
                }
                Type *out = (Type *)arena_alloc(e->arena, sizeof(Type));
                *out = type_typerow_canonical(e->arena, *operand);
                return out;
            }
            int rop = 0; /* 1=concat, 2=union, 3=intersect */
            if      (strcmp(h, "row-concat")    == 0) rop = 1;
            else if (strcmp(h, "row-union")     == 0) rop = 2;
            else if (strcmp(h, "row-intersect") == 0) rop = 3;
            if (rop != 0) {
                uint32_t nargs = form->as.list.len - 1;
                Type acc; /* accumulator row */
                memset(&acc, 0, sizeof(acc));
                bool have_acc = false;
                for (uint32_t ai = 1; ai < form->as.list.len; ai++) {
                    Type *operand = type_expr_from_form(e, form->as.list.items[ai],
                        rec_name, type_params, type_param_kinds, n_type_params);
                    if (!operand) return NULL;
                    if (operand->kind != TY_TYPEROW) {
                        diag_emit(DIAG_ERROR, form->as.list.items[ai]->span,
                            "'%s' operand %u must be a #row{...} (kind [*]), "
                            "got a non-row type", h, (unsigned)ai);
                        return NULL;
                    }
                    if (!have_acc) { acc = *operand; have_acc = true; continue; }
                    /* row-ops-drop-field-names, all-or-nothing labels: the
                     * literal level already rejects a `#row{...}` that mixes
                     * bare types with `name : T` slots (TUR-E0290); the algebra
                     * has to hold the same line, or the fold would have to
                     * invent a name for the bare side. An EMPTY operand is
                     * label-neutral, so `(row-union R #row{})` stays legal --
                     * type_typerow_is_labeled is false for a zero-length row on
                     * both sides of this check. */
                    if (operand->as.typerow_.n_elements > 0 &&
                        acc.as.typerow_.n_elements > 0 &&
                        type_typerow_is_labeled(acc) !=
                            type_typerow_is_labeled(*operand)) {
                        diag_emit(DIAG_ERROR, form->as.list.items[ai]->span,
                            "'%s' operand %u mixes a labeled row with a bare one; "
                            "labels are all-or-nothing across the whole operation "
                            "(TUR-E0290)", h, (unsigned)ai);
                        return NULL;
                    }
                    switch (rop) {
                        case 1: acc = type_typerow_concat(e->arena, acc, *operand); break;
                        case 2: acc = type_typerow_union(e->arena, acc, *operand); break;
                        default: acc = type_typerow_intersect(e->arena, acc, *operand); break;
                    }
                    /* concat keeps duplicates outright, and union keeps two
                     * slots that share a name but disagree on type -- both can
                     * produce a labeled row that no literal could have spelled.
                     * Report it against the operand that introduced it. */
                    const char *dup = type_typerow_dup_field_name(acc);
                    if (dup) {
                        diag_emit(DIAG_ERROR, form->as.list.items[ai]->span,
                            "'%s' result has duplicate field name `%s` "
                            "(TUR-E0291)", h, dup);
                        return NULL;
                    }
                }
                if (nargs == 0) acc = type_typerow(e->arena, NULL, 0);
                Type *out = (Type *)arena_alloc(e->arena, sizeof(Type));
                *out = acc;
                return out;
            }
        }
        /* LS1: borrow *type* annotation -- &T / &mut T, optionally carrying a
         * Rust-style lifetime: &'a T / &mut 'a T.  The reader produces these as
         * a list whose head is the borrow operator (& or &mut):
         *   (& T)         (&mut T)         -- no lifetime
         *   (& 'a T)      (&mut 'a T)      -- lifetime in the middle slot
         * (An intersection type `A & B` instead carries `&` in a *middle* slot,
         * so keying on items[0] keeps the two unambiguous.) */
        if (form->as.list.len >= 2 && form->as.list.items[0]->tag == F_SYM
                && (form->as.list.items[0]->as.sym == e->sym_ampersand
                    || form->as.list.items[0]->as.sym == e->sym_borrow_mut)) {
            bool is_mut = (form->as.list.items[0]->as.sym == e->sym_borrow_mut);

            /* An optional lifetime sits between the operator and the target. */
            Form *lt_f = NULL;
            Form *target_f = NULL;
            if (form->as.list.len == 2) {
                target_f = form->as.list.items[1];
            } else if (form->as.list.len == 3
                       && form->as.list.items[1]->tag == F_SYM
                       && form->as.list.items[1]->as.sym->len >= 1
                       && form->as.list.items[1]->as.sym->name[0] == '\'') {
                lt_f = form->as.list.items[1];
                target_f = form->as.list.items[2];
            } else {
                diag_emit(DIAG_ERROR, form->span,
                          "malformed borrow type: expected (& T), (&mut T), or a "
                          "lifetime-annotated &'a T / &mut 'a T");
                return NULL;
            }

            Type *target = type_expr_from_form(e, target_f, rec_name,
                                               type_params, type_param_kinds, n_type_params);
            if (!target) return NULL;

            /* Resolve the optional lifetime to a stable per-function LifetimeId.
             * Implicit quantification: each distinct 'a in the signature is a
             * fresh lifetime; a repeated 'a resolves to the same ID. */
            LifetimeId lid = LIFETIME_NONE;
            if (lt_f != NULL && e->cur_lifetime_ctx != NULL) {
                lid = lifetime_context_intern(e->cur_lifetime_ctx, lt_f->as.sym->name);
                if (lid == LIFETIME_NONE) {
                    diag_emit(DIAG_ERROR, lt_f->span,
                              "too many distinct lifetimes in this signature "
                              "(maximum %d)", MAX_LIFETIMES);
                    return NULL;
                }
            }

            Type *t = (Type *)arena_alloc(e->arena, sizeof(Type));
            *t = is_mut ? type_ref_mut_lifetime(target->kind, lid)
                        : type_ref_immut_lifetime(target->kind, lid);
            /* saffron-dynamic-surface-pass H9: keep the tyvar's NAME on the
             * borrow so a call can bind K from a `(& K)` argument. */
            t->as.ref_borrow.target_tyvar =
                (target->kind == TY_TYVAR) ? target->as.tyvar_.name : NULL;
            /* borrowed-aggregate-key-skips-the-key-check: a `(& Pt)` /
             * `(& (Vec A))` parameter keeps the target its kind cannot name,
             * so a call compares a borrowed argument against it (and binds
             * the tyvars inside it). */
            if (borrow_target_needs_full(target->kind))
                t->as.ref_borrow.target_full = target;

            /* LS3: well-formedness of a nested borrow &'a &'b T.  The outer
             * reference (lifetime 'a) points at the inner reference (lifetime
             * 'b); for the outer to be valid for 'a, the inner must live at
             * least as long, so 'b must outlive 'a.  Record that outlives edge
             * ('b : 'a) on the signature's lifetime context.  Two opposing
             * nested borrows in one signature (&'a &'b ... and &'b &'a ...) then
             * form a constraint cycle the solver rejects as TUR-E0106.
             * (Type.lifetimes only carries the head's own lifetime, so the inner
             * lifetime is also surfaced on the outer Type's lifetimes[1] for
             * collectors that walk a single Type.) */
            if (lid != LIFETIME_NONE && e->cur_lifetime_ctx != NULL
                    && (target->kind == TY_REF_IMMUT || target->kind == TY_REF_MUT)
                    && target->n_lifetimes > 0
                    && target->lifetimes[0] != LIFETIME_NONE) {
                LifetimeId inner = target->lifetimes[0];
                lifetime_context_add_constraint(e->cur_lifetime_ctx, inner, lid);
                if (t->n_lifetimes < MAX_TYPE_LIFETIMES) {
                    t->lifetimes[t->n_lifetimes++] = inner;
                }
            }
            return t;
        }

        /* IT0: (A | B | C) — union type expression.
         * Detect by scanning for any element that is the "|" pipe symbol.
         * Only active when -Xunion-types is enabled. */
        {
            bool has_pipe = false;
            for (uint32_t uk = 0; uk < form->as.list.len; uk++) {
                Form *uit = form->as.list.items[uk];
                if (uit->tag == F_SYM && uit->as.sym == e->sym_pipe) {
                    has_pipe = true;
                    break;
                }
            }
            if (has_pipe) {
                /* Count non-pipe member forms */
                uint8_t n_members = 0;
                for (uint32_t uk = 0; uk < form->as.list.len; uk++) {
                    Form *uit = form->as.list.items[uk];
                    if (uit->tag == F_SYM && uit->as.sym == e->sym_pipe) continue;
                    n_members++;
                }
                if (n_members < 2) {
                    diag_emit(DIAG_ERROR, form->span,
                              "union type requires at least two member types separated by '|'");
                    return NULL;
                }
                /* Collect member types */
                Type **members = (Type **)arena_alloc(e->arena, n_members * sizeof(Type *));
                uint8_t mi = 0;
                for (uint32_t uk = 0; uk < form->as.list.len; uk++) {
                    Form *uit = form->as.list.items[uk];
                    if (uit->tag == F_SYM && uit->as.sym == e->sym_pipe) continue;
                    Type *mt = type_expr_from_form(e, uit, rec_name,
                                                   type_params, type_param_kinds, n_type_params);
                    if (!mt) return NULL;
                    members[mi++] = mt;
                }
                /* Build and return union type (type_union_build flattens nested unions) */
                Type built = type_union_build(e->arena, members, n_members);
                Type *t = (Type *)arena_alloc(e->arena, sizeof(Type));
                *t = built;
                return t;
            }
        }

        /* IT2: (A & B & C) — intersection type expression.
         * Detect by scanning for any element that is the bare "&" ampersand symbol.
         * Only active when -Xintersection-types is enabled. */
        {
            bool has_amp = false;
            for (uint32_t uk = 0; uk < form->as.list.len; uk++) {
                Form *uit = form->as.list.items[uk];
                if (uit->tag == F_SYM && uit->as.sym == e->sym_ampersand) {
                    has_amp = true;
                    break;
                }
            }
            if (has_amp) {
                /* Count non-ampersand member forms */
                uint8_t n_members = 0;
                for (uint32_t uk = 0; uk < form->as.list.len; uk++) {
                    Form *uit = form->as.list.items[uk];
                    if (uit->tag == F_SYM && uit->as.sym == e->sym_ampersand) continue;
                    n_members++;
                }
                if (n_members < 2) {
                    diag_emit(DIAG_ERROR, form->span,
                              "intersection type requires at least two member types separated by '&'");
                    return NULL;
                }
                /* Collect member types */
                Type **members = (Type **)arena_alloc(e->arena, n_members * sizeof(Type *));
                uint8_t mi = 0;
                for (uint32_t uk = 0; uk < form->as.list.len; uk++) {
                    Form *uit = form->as.list.items[uk];
                    if (uit->tag == F_SYM && uit->as.sym == e->sym_ampersand) continue;
                    Type *mt = type_expr_from_form(e, uit, rec_name,
                                                   type_params, type_param_kinds, n_type_params);
                    if (!mt) return NULL;
                    members[mi++] = mt;
                }
                /* IT3: TUR_E0350 -- reject provably-unsatisfiable intersections.
                 * For each pair of members, check if they are known-disjoint concrete
                 * types. Only emit one error (the first pair found). */
                for (uint8_t ia = 0; ia < n_members; ia++) {
                    for (uint8_t ib = ia + 1; ib < n_members; ib++) {
                        if (types_provably_disjoint(members[ia], members[ib])) {
                            diag_emit_with_code(DIAG_ERROR, form->span,
                                TUR_E0350_INTERSECTION_UNSATISFIABLE,
                                "intersection type is unsatisfiable: no value can be both %s and %s",
                                type_name(*members[ia]), type_name(*members[ib]));
                            return NULL;
                        }
                    }
                }
                /* Build and return intersection type (type_intersection_build flattens nested) */
                Type built = type_intersection_build(e->arena, members, n_members);
                Type *t = (Type *)arena_alloc(e->arena, sizeof(Type));
                *t = built;
                return t;
            }
        }

        /* ET3-A: (handler EffectName ValueType ResultType) type expression */
        if (form->as.list.len == 4 && form->as.list.items[0]->tag == F_SYM
                && form->as.list.items[0]->as.sym == e->sym_handler_type) {
            Form *eff_f = form->as.list.items[1];
            Form *val_f = form->as.list.items[2];
            Form *res_f = form->as.list.items[3];
            /* FH4.1: the effect position is either a single effect symbol
             * (single-effect handler, source-compatible) or a #{E1 E2 ...}
             * effect-row map (multi-effect / composed handler).  Either way the
             * handled set is stored as an ERK_UNRESOLVED name-set row. */
            const char *single_name = NULL;
            EffectRow *handled_row = NULL;
            if (eff_f->tag == F_SYM) {
                single_name = eff_f->as.sym->name;
                const Symbol *one[1] = { eff_f->as.sym };
                handled_row = effect_row_unresolved(e->arena, one, 1, eff_f->span);
            } else if (eff_f->tag == F_MAP) {
                warn_legacy_fx_row(eff_f);
                uint8_t n_sym = (uint8_t)eff_f->as.list.len;
                const Symbol **syms = (const Symbol **)arena_alloc(e->arena,
                                        (n_sym ? n_sym : 1) * sizeof(Symbol *));
                uint8_t n_valid = 0;
                for (uint32_t j = 0; j < eff_f->as.list.len; j++) {
                    Form *item = eff_f->as.list.items[j];
                    if (item->tag == F_SYM) syms[n_valid++] = item->as.sym;
                }
                handled_row = effect_row_unresolved(e->arena, syms, n_valid,
                                                    eff_f->span);
                if (n_valid == 1) single_name = syms[0]->name;  /* still single */
            } else {
                diag_emit(DIAG_ERROR, eff_f->span,
                          "(handler E V R): effect position must be an effect name "
                          "or a #{E1 E2 ...} effect set");
                return NULL;
            }
            Type *val_t = type_expr_from_form(e, val_f, rec_name, type_params, type_param_kinds, n_type_params);
            Type *res_t = type_expr_from_form(e, res_f, rec_name, type_params, type_param_kinds, n_type_params);
            if (!val_t || !res_t) return NULL;
            Type *t = (Type *)arena_alloc(e->arena, sizeof(Type));
            memset(t, 0, sizeof(Type));
            t->kind = TY_HANDLER;
            t->copy_kind = CK_COPY;
            t->hkt_kind = KIND_STAR;
            t->as.handler_.effect_name = single_name;  /* NULL for multi-effect */
            t->as.handler_.handled_row = handled_row;
            t->as.handler_.value_kind = val_t->kind;
            t->as.handler_.result_kind = res_t->kind;
            return t;
        }

        /* Phase HRT0: Handle (forall [vars...] body) and (exists [vars...] body) */
        if (form->as.list.len >= 1 && form->as.list.items[0]->tag == F_SYM) {
            const Symbol *head = form->as.list.items[0]->as.sym;
            bool is_forall_form = (head == e->sym_forall || head == e->sym_forall_u);
            bool is_exists_form = (head == e->sym_exists || head == e->sym_exists_u);

            if (is_forall_form || is_exists_form) {
                /* Syntax:
                 *   (forall [a b ...] body-type)
                 *   (exists [a b ...] body-type)
                 *   (exists [a b ...] [(C a) ...] body-type)   -- EX1b
                 *   (exists :linear [a b ...] [(C a) ...] body-type)   -- EXG6
                 *
                 * The optional constraint vector (EX1b) is only accepted on
                 * `exists`; constraints on `forall` are tracked separately.
                 * EXG6: an optional `:linear` keyword may appear immediately
                 * after `exists` to opt the type into use-exactly-once. */
                uint32_t cursor = 1;
                bool is_linear_attr = false;
                if (is_exists_form && cursor < form->as.list.len) {
                    Form *maybe_attr = form->as.list.items[cursor];
                    if (maybe_attr->tag == F_KEYWORD && maybe_attr->as.sym == e->kw_linear) {
                        is_linear_attr = true;
                        cursor++;
                    }
                }
                uint32_t remaining = form->as.list.len - cursor;
                if (remaining != 2 && remaining != 3) {
                    diag_emit(DIAG_ERROR, form->span,
                              "'%s' requires a variable list, an optional constraint vector, "
                              "and a body type",
                              is_forall_form ? "forall" : "exists");
                    return NULL;
                }
                Form *vars_form = form->as.list.items[cursor];
                Form *constraint_form = NULL;
                Form *body_form = NULL;
                if (remaining == 2) {
                    body_form = form->as.list.items[cursor + 1];
                } else {
                    /* Middle slot is the constraint vector. */
                    constraint_form = form->as.list.items[cursor + 1];
                    body_form = form->as.list.items[cursor + 2];
                    /* forall-constraints GRADUATED 2026-07-06: a constraint
                     * vector on `forall` is always parsed here and always
                     * enforced at each rank-2 instantiation site
                     * (elab_poly_call). */
                    if (constraint_form->tag != F_VEC) {
                        diag_emit(DIAG_ERROR, constraint_form->span,
                                  "'%s' constraint vector must be a vector, "
                                  "e.g. [(Show a)]",
                                  is_forall_form ? "forall" : "exists");
                        return NULL;
                    }
                }

                if (vars_form->tag != F_VEC) {
                    diag_emit(DIAG_ERROR, vars_form->span,
                              "'%s' variable list must be a vector, e.g. [a b]",
                              is_forall_form ? "forall" : "exists");
                    return NULL;
                }

                uint8_t n_bound = (uint8_t)vars_form->as.list.len;
                if (n_bound == 0) {
                    diag_emit(DIAG_ERROR, vars_form->span,
                              "'%s' requires at least one bound variable",
                              is_forall_form ? "forall" : "exists");
                    return NULL;
                }

                /* Collect bound variable names and kinds */
                const char **var_names = (const char **)arena_alloc(
                    e->arena, n_bound * sizeof(const char *));
                Kind *var_kinds = (Kind *)arena_alloc(
                    e->arena, n_bound * sizeof(Kind));

                /* Build extended type_params including the new bound variables */
                uint8_t n_extended = n_type_params + n_bound;
                const Symbol **ext_params = (const Symbol **)arena_alloc(
                    e->arena, n_extended * sizeof(const Symbol *));
                Kind *ext_kinds = (Kind *)arena_alloc(
                    e->arena, n_extended * sizeof(Kind));

                /* Copy existing type params.  type_param_kinds follows the
                 * codebase-wide "NULL means all-KIND_STAR" convention (see the
                 * guarded reads in emit_module.c / elab_typeclasses.c); without
                 * this guard a rank-N HKT signature with a NULL kinds array
                 * NULL-derefs here. */
                for (uint8_t i = 0; i < n_type_params; i++) {
                    ext_params[i] = type_params[i];
                    ext_kinds[i] = type_param_kinds ? type_param_kinds[i] : KIND_STAR;
                }

                /* Parse each bound variable */
                for (uint8_t i = 0; i < n_bound; i++) {
                    Form *vf = vars_form->as.list.items[i];
                    const Symbol *var_sym = NULL;
                    Kind          var_kind;
                    if (vf->tag == F_SYM) {
                        var_sym = vf->as.sym;
                        /* ET2-A: lowercase binders are row variables (effect
                         * rows); uppercase binders remain type variables
                         * (KIND_STAR). */
                        bool is_row_binder = (var_sym->name[0] >= 'a'
                                              && var_sym->name[0] <= 'z');
                        var_kind = is_row_binder ? KIND_ROW : KIND_STAR;
                    } else if (vf->tag == F_LIST) {
                        /* forall-kinds GRADUATED 2026-07-06: an explicit kind
                         * annotation `(name :: <kind>)` on a bound variable is
                         * always accepted. */
                        if (!parse_kind_binder(e, vf, &var_sym, &var_kind))
                            return NULL;
                    } else {
                        diag_emit(DIAG_ERROR, vf->span,
                                  "'%s' bound variable must be a symbol or a "
                                  "kind-annotated '(name :: <kind>)' form",
                                  is_forall_form ? "forall" : "exists");
                        return NULL;
                    }
                    /* Reject shadowing of outer type params */
                    for (uint8_t j = 0; j < n_type_params; j++) {
                        if (type_params[j] == var_sym) {
                            diag_emit(DIAG_WARNING, vf->span,
                                      "'%s' bound variable '%s' shadows an outer type variable",
                                      is_forall_form ? "forall" : "exists",
                                      var_sym->name);
                        }
                    }
                    var_names[i] = var_sym->name;
                    var_kinds[i] = var_kind;
                    ext_params[n_type_params + i] = var_sym;
                    ext_kinds[n_type_params + i] = var_kind;
                }

                /* Parse the body type with bound vars in scope */
                Type *body_type = type_expr_from_form(e, body_form, rec_name,
                    ext_params, ext_kinds, n_extended);
                if (!body_type) return NULL;

                /* EX1b: Parse the optional constraint vector.  Each entry is a
                 * 2-form list `(C v)` where C is a typeclass name in scope and
                 * v is one of the bound variables. */
                TypeClass **cclasses = NULL;
                uint8_t    *cvar_idx = NULL;
                uint8_t     n_constraints = 0;
                if (constraint_form != NULL) {
                    uint32_t nc = constraint_form->as.list.len;
                    if (nc > 0) {
                        cclasses = (TypeClass **)arena_alloc(
                            e->arena, nc * sizeof(TypeClass *));
                        cvar_idx = (uint8_t *)arena_alloc(
                            e->arena, nc * sizeof(uint8_t));
                        for (uint32_t i = 0; i < nc; i++) {
                            const char *qname = is_forall_form ? "forall"
                                                                : "exists";
                            Form *cf = constraint_form->as.list.items[i];
                            if (cf->tag != F_LIST || cf->as.list.len != 2) {
                                diag_emit(DIAG_ERROR, cf->span,
                                          "%s: constraint must be a 2-element "
                                          "list (TypeclassName var), e.g. (Show a)",
                                          qname);
                                return NULL;
                            }
                            Form *tc_form = cf->as.list.items[0];
                            Form *var_ref = cf->as.list.items[1];
                            if (tc_form->tag != F_SYM) {
                                diag_emit(DIAG_ERROR, tc_form->span,
                                          "%s: constraint head must be a typeclass name",
                                          qname);
                                return NULL;
                            }
                            if (var_ref->tag != F_SYM) {
                                diag_emit(DIAG_ERROR, var_ref->span,
                                          "%s: constraint argument must be one of "
                                          "the bound variable names", qname);
                                return NULL;
                            }
                            TypeClass *tc = typeclass_env_lookup_typeclass(
                                &e->typeclass_env, tc_form->as.sym);
                            if (!tc) {
                                diag_emit(DIAG_ERROR, tc_form->span,
                                          "%s: unknown typeclass '%s'",
                                          qname, tc_form->as.sym->name);
                                return NULL;
                            }
                            /* Find the var index.  Match against the resolved
                             * bound-var symbols (ext_params) so kind-annotated
                             * `(name :: <kind>)` binders are found too. */
                            uint8_t vi = (uint8_t)0xFF;
                            for (uint8_t j = 0; j < n_bound; j++) {
                                if (ext_params[n_type_params + j]
                                        == var_ref->as.sym) {
                                    vi = j;
                                    break;
                                }
                            }
                            if (vi == (uint8_t)0xFF) {
                                diag_emit(DIAG_ERROR, var_ref->span,
                                          "%s: constraint variable '%s' is not "
                                          "one of the bound variables",
                                          qname, var_ref->as.sym->name);
                                return NULL;
                            }
                            cclasses[i] = tc;
                            cvar_idx[i] = vi;
                        }
                        n_constraints = (uint8_t)nc;
                    }
                }

                /* class-superclasses: a forall's constraints imply their
                 * superclasses exactly as a defn's do (elab_defn applies the
                 * same typeclass_constraints_with_supers).  A constrained
                 * rank-2 forall carries one dictionary per constraint and
                 * forall-dict-pass aligns those slots positionally with the
                 * inner function's constraint list, so the two lists must be
                 * expanded the same way or `[^Applicative m]` (three after
                 * expansion, with Functor) would no longer line up with
                 * `[(Applicative m)]`.  Existentials keep their declared list:
                 * their pack layout is a separate ABI. */
                if (is_forall_form && n_constraints > 0) {
                    TypeConstraint *tmp = (TypeConstraint *)arena_alloc(
                        e->arena, n_constraints * sizeof(TypeConstraint));
                    for (uint8_t i = 0; i < n_constraints; i++) {
                        memset(&tmp[i], 0, sizeof(tmp[i]));
                        tmp[i].typeclass = cclasses[i];
                        tmp[i].type_arg  = TYPE_UNKNOWN;
                        tmp[i].param_idx = -1;
                        tmp[i].tyvar     = ext_params[n_type_params + cvar_idx[i]];
                    }
                    uint8_t n_exp = n_constraints;
                    TypeConstraint *exp = typeclass_constraints_with_supers(
                        &e->typeclass_env, tmp, n_constraints, &n_exp);
                    if (n_exp > n_constraints) {
                        TypeClass **ncls = (TypeClass **)arena_alloc(
                            e->arena, n_exp * sizeof(TypeClass *));
                        uint8_t *nidx = (uint8_t *)arena_alloc(e->arena, n_exp);
                        for (uint8_t i = 0; i < n_exp; i++) {
                            ncls[i] = exp[i].typeclass;
                            nidx[i] = 0;
                            for (uint8_t j = 0; j < n_bound; j++)
                                if (ext_params[n_type_params + j] == exp[i].tyvar) {
                                    nidx[i] = j;
                                    break;
                                }
                        }
                        cclasses = ncls;
                        cvar_idx = nidx;
                        n_constraints = n_exp;
                    }
                }

                /* Build the TY_FORALL or TY_EXISTS node */
                Type *t = (Type *)arena_alloc(e->arena, sizeof(Type));
                memset(t, 0, sizeof(Type));
                t->kind = is_forall_form ? TY_FORALL : TY_EXISTS;
                /* EXG6: linear existentials use CK_LINEAR so the type-system
                 * machinery already routes them through LT1's exactly-once
                 * tracking when bound by a let. */
                t->copy_kind = is_linear_attr ? CK_LINEAR : CK_MOVE;
                t->hkt_kind = KIND_STAR;
                t->as.forall_.var_names = var_names;
                t->as.forall_.var_kinds = var_kinds;
                t->as.forall_.n_vars = n_bound;
                t->as.forall_.body = body_type;
                t->as.forall_.constraint_classes = cclasses;
                t->as.forall_.constraint_var_idx = cvar_idx;
                t->as.forall_.n_constraints = n_constraints;
                t->as.forall_.is_linear = is_linear_attr;
                return t;
            }
        }

        /* ER2: (fn [param-types...] #{effect-row} :return-type) — function type
         * annotation form used in parameter position.
         * Syntax: (fn [] :int)            — zero-arg fn returning int, no effects
         *         (fn [] #{e} :int)       — zero-arg fn with effect-row variable e
         *         (fn [:int] #{Write} :nil) — one-arg fn with concrete effect row
         *
         * The param list is a vector of type annotations (or empty).
         * The optional #{...} map is an effect-row annotation.
         * The last element is the return type. */
        if (form->as.list.len >= 2 && form->as.list.items[0]->tag == F_SYM
                && (form->as.list.items[0]->as.sym == e->sym_fn
                    || form->as.list.items[0]->as.sym == e->sym_c_fn)) {
            /* typed-c-abi-function-pointers: `(c-fn [A...] R)` shares the
             * `(fn [A...] R)` parsing path but stamps the result type as a
             * bare C-ABI function pointer (cfnptr) rather than a closure. */
            bool is_cfnptr = (form->as.list.items[0]->as.sym == e->sym_c_fn);
            uint32_t idx = 1; /* skip 'fn'/'c-fn' head */
            /* Expect a vector of param type annotations */
            if (idx >= form->as.list.len || form->as.list.items[idx]->tag != F_VEC) {
                diag_emit(DIAG_ERROR, form->span,
                          "'fn' type expression requires a parameter vector: (fn [params...] :return)");
                return NULL;
            }
            Form *params_vec = form->as.list.items[idx++];
            /* Parse param types from the vector */
            uint8_t n_fn_args = (uint8_t)params_vec->as.list.len;
            TypeKind fn_arg_kinds[MAX_FN_ARITY];
            Type *fn_arg_full[MAX_FN_ARITY];
            bool any_compound_arg = false;
            memset(fn_arg_kinds, 0, sizeof(fn_arg_kinds));
            memset(fn_arg_full, 0, sizeof(fn_arg_full));
            for (uint32_t pi2 = 0; pi2 < n_fn_args && pi2 < MAX_FN_ARITY; pi2++) {
                reject_fn_type_colon(e, params_vec->as.list.items[pi2]);
                Type *at = type_expr_from_form(e, params_vec->as.list.items[pi2],
                                               rec_name, type_params, type_param_kinds, n_type_params);
                reject_fn_type_contract(e, at, params_vec->as.list.items[pi2],
                                        "parameter");
                fn_arg_full[pi2] = at;
                /* Type variables map to the int64 carrier kind; use TY_INT so
                 * the fn's result_kind/arg_kinds stay consistent with the ->
                 * form's handling of type params. */
                fn_arg_kinds[pi2] = (at && at->kind == TY_TYVAR) ? TY_INT
                                  : (at ? at->kind : TY_INT);
                /* curried-fn-typed-param: a compound arg (notably a nested
                 * (fn ...) type) or a named tyvar cannot be reconstructed from
                 * its bare TypeKind; preserve the full type. */
                if (at && (at->kind == TY_TYVAR ||
                           at->kind == TY_FN || at->kind == TY_APP ||
                           at->kind == TY_ADT)) {
                    any_compound_arg = true;
                }
                /* c-fn-ptr-element-and-size-precision-gap fix: when this is a
                 * c-fn (bare C-ABI function pointer) and the arg is `ptr<T>`
                 * with a known element type, preserve the full type so the
                 * typedef lowering can emit `T *` instead of `void *`.  The
                 * non-cfnptr (closure) path keeps its lossy TypeKind lowering
                 * to avoid snapshot churn on unrelated typedefs. */
                if (is_cfnptr && at && at->kind == TY_PTR_VOID &&
                    (at->as.ptr.inner || at->as.ptr.is_const)) {
                    any_compound_arg = true;
                }
                /* c-fn-ptr-element-and-size-precision-gap fix (residual): a
                 * size_t / ptrdiff_t-spelled integer carrier (`:usize` /
                 * `:isize`) must keep its full type so the typedef lowering
                 * emits `size_t` / `ptrdiff_t` instead of the lossy int64
                 * carrier.  Only matters for cfnptrs (FFI signatures). */
                if (is_cfnptr && at && at->c_num_spelling != CNUM_DEFAULT) {
                    any_compound_arg = true;
                }
            }
            /* Optional #{...} effect-row annotation */
            EffectRow *fn_effect_row = NULL;
            if (idx < form->as.list.len && form->as.list.items[idx]->tag == F_MAP) {
                Form *row_form = form->as.list.items[idx++];
                warn_legacy_fx_row(row_form);
                uint8_t n_row_sym = (uint8_t)row_form->as.list.len;
                const Symbol **row_syms = (const Symbol **)arena_alloc(e->arena,
                    (n_row_sym ? n_row_sym : 1) * sizeof(Symbol *));
                uint8_t n_row_valid = 0;
                for (uint32_t rj = 0; rj < row_form->as.list.len; rj++) {
                    Form *item = row_form->as.list.items[rj];
                    if (item->tag == F_SYM) row_syms[n_row_valid++] = item->as.sym;
                }
                fn_effect_row = effect_row_unresolved(e->arena, row_syms, n_row_valid,
                                                      row_form->span);
            }
            /* Return type — must be the last element */
            if (idx >= form->as.list.len) {
                diag_emit(DIAG_ERROR, form->span,
                          "'fn' type expression requires a return type: (fn [params...] :return)");
                return NULL;
            }
            reject_fn_type_colon(e, form->as.list.items[idx]);
            Type *ret_t = type_expr_from_form(e, form->as.list.items[idx],
                                               rec_name, type_params, type_param_kinds, n_type_params);
            reject_fn_type_contract(e, ret_t, form->as.list.items[idx], "result");
            /* The return type is the LAST element.  Anything after it used to be
             * dropped without a word -- notably an effect row written after the
             * result, `(fn [B] B #fx{E})`, which then typed the parameter as
             * effect-free (the cps-evicts-handle-in-operand-positions repro
             * carried one, and its "the row makes no difference" was this). */
            if (idx + 1 < form->as.list.len) {
                Form *extra = form->as.list.items[idx + 1];
                if (extra->tag == F_MAP && !fn_effect_row)
                    diag_emit(DIAG_ERROR, extra->span,
                              "'fn' type: the effect row goes before the return type: "
                              "(fn [params...] #fx{...} :return)");
                else
                    diag_emit(DIAG_ERROR, extra->span,
                              "'fn' type: unexpected form after the return type: "
                              "(fn [params...] :return)");
            }
            /* Type variables lower to the int64 carrier for the kind slot. */
            TypeKind ret_kind = ret_t ? (ret_t->kind == TY_TYVAR ? TY_INT : ret_t->kind)
                                      : TY_INT;

            Type *fn_t = (Type *)arena_alloc(e->arena, sizeof(Type));
            *fn_t = type_fn(fn_arg_kinds, n_fn_args, ret_kind);
            if (fn_effect_row) fn_t->as.fn.effect_row = fn_effect_row;
            /* fat-closure-dispatch-aggregate-return: an aggregate/applied result
             * type (a TY_APP like (Pair A B), a concrete TY_STRUCT, or a TY_ADT)
             * cannot be reconstructed from its bare TypeKind -- type_from_kind
             * drops the def/args and lowers it to the int64_t carrier.  Preserve
             * the full type so a call through this fat-closure param (and its
             * typed-thunk dispatch) emit the real C return type, not int64_t. */
            /* curried-fn-typed-param: a result that is itself a (fn ...) type
             * must keep its full Type so a chained application -- ((f 1) 2)
             * through a parameter typed (fn [int] (fn [int] int)) -- can
             * recover the inner function type at the call site instead of
             * erasing it to a bare result kind. */
            /* Part 1: a named TY_TYVAR result must be preserved so Direction 3
             * in emit can resolve the real dispatch type per monomorphization. */
            if (ret_t &&
                (ret_t->kind == TY_TYVAR ||
                 ret_t->kind == TY_FN || ret_t->kind == TY_APP ||
                 ret_t->kind == TY_ADT)) {
                fn_t->as.fn.result_full_type = ret_t;
            }
            /* c-fn-ptr-element-and-size-precision-gap fix: a cfnptr result that
             * is size_t/ptrdiff_t-spelled, an element-typed pointer, or a
             * const-qualified pointer must keep its full type so the typedef
             * return slot lowers precisely (e.g. `size_t (*)(...)` or
             * `const unsigned char *(*)(...)`) instead of the lossy carrier. */
            if (is_cfnptr && ret_t &&
                (ret_t->c_num_spelling != CNUM_DEFAULT ||
                 (ret_t->kind == TY_PTR_VOID &&
                  (ret_t->as.ptr.inner || ret_t->as.ptr.is_const)))) {
                fn_t->as.fn.result_full_type = ret_t;
            }
            if (any_compound_arg) {
                Type **aFT = (Type **)arena_alloc(e->arena, n_fn_args * sizeof(Type *));
                for (uint32_t i = 0; i < n_fn_args; i++) aFT[i] = fn_arg_full[i];
                fn_t->as.fn.arg_full_types = aFT;
            }
            /* typed-c-abi-function-pointers: a (c-fn ...) is a bare C function
             * pointer, never a closure box. */
            fn_t->as.fn.cfnptr = is_cfnptr;
            return fn_t;
        }

        /* Phase HRT1/LT2: (-> T1 T2 ... Tn) — function type expression.
         * All but the last element are argument types; the last is the result type.
         * LT2: ^linear may prefix any arg type: (-> ^linear T1 T2 R) marks T1 as linear.
         * Requires at least (-> result) i.e. 2 elements. */
        if (form->as.list.len >= 1 && form->as.list.items[0]->tag == F_SYM
                && form->as.list.items[0]->as.sym == e->sym_arrow) {
            uint32_t len = form->as.list.len;
            if (len < 2) {
                diag_emit(DIAG_ERROR, form->span,
                          "'->': function type requires at least a result type: (-> result) or (-> arg result)");
                return NULL;
            }
            /* LT2: Two-pass parse to handle optional ^linear annotations.
             * items[1..len-2] may contain ^linear prefixes before each arg type.
             * Pass 1: count actual type items (skip ^linear / ^unique / ^mut symbols) */
            uint8_t n_args = 0;
            for (uint32_t _pi = 1; _pi < len - 1; _pi++) {
                Form *_it = form->as.list.items[_pi];
                if (_it->tag == F_SYM && _it->as.sym == e->sym_caret_linear)   continue;
                if (_it->tag == F_SYM && _it->as.sym == e->sym_caret_unique)   continue;
                if (_it->tag == F_SYM && _it->as.sym == e->sym_caret_mut)      continue;
                if (_it->tag == F_SYM && _it->as.sym == e->sym_caret_affine)   continue;
                if (_it->tag == F_SYM && _it->as.sym == e->sym_caret_relevant) continue;
                n_args++;
            }
            if (n_args > MAX_FN_ARITY) {
                diag_emit(DIAG_ERROR, form->span,
                          "'->': too many function arguments (max %d)", MAX_FN_ARITY);
                return NULL;
            }

            TypeKind arg_kinds[MAX_FN_ARITY];
            Type *arg_full_types_local[MAX_FN_ARITY];
            bool arg_linear_local[MAX_FN_ARITY];
            bool arg_unique_local[MAX_FN_ARITY];
            bool arg_unique_mut_local[MAX_FN_ARITY];
            bool arg_affine_local[MAX_FN_ARITY];    /* ST0 */
            bool arg_relevant_local[MAX_FN_ARITY];  /* ST0 */
            bool any_poly_arg = false;
            bool any_app_arg = false;  /* Slice 3: a (F A) arg needs its full type kept */
            for (uint8_t _ai = 0; _ai < MAX_FN_ARITY; _ai++) arg_linear_local[_ai] = false;
            for (uint8_t _ai = 0; _ai < MAX_FN_ARITY; _ai++) arg_unique_local[_ai] = false;
            for (uint8_t _ai = 0; _ai < MAX_FN_ARITY; _ai++) arg_unique_mut_local[_ai] = false;
            for (uint8_t _ai = 0; _ai < MAX_FN_ARITY; _ai++) arg_affine_local[_ai] = false;
            for (uint8_t _ai = 0; _ai < MAX_FN_ARITY; _ai++) arg_relevant_local[_ai] = false;

            /* Pass 2: parse types, tracking ^linear / ^unique / ^mut / ^affine / ^relevant prefix per arg */
            uint8_t arg_idx = 0;
            bool next_linear   = false;
            bool next_unique   = false;
            bool next_mut      = false;
            bool next_affine   = false;  /* ST0 */
            bool next_relevant = false;  /* ST0 */
            for (uint32_t _pi2 = 1; _pi2 < len - 1; _pi2++) {
                Form *_it2 = form->as.list.items[_pi2];
                if (_it2->tag == F_SYM && _it2->as.sym == e->sym_caret_linear) {
                    next_linear = true;
                    continue;
                }
                if (_it2->tag == F_SYM && _it2->as.sym == e->sym_caret_unique) {
                    next_unique = true;
                    continue;
                }
                if (_it2->tag == F_SYM && _it2->as.sym == e->sym_caret_mut) {
                    next_mut = true;
                    continue;
                }
                /* ST0: ^affine and ^relevant prefix annotations */
                if (_it2->tag == F_SYM && _it2->as.sym == e->sym_caret_affine) {
                    next_affine = true;
                    continue;
                }
                if (_it2->tag == F_SYM && _it2->as.sym == e->sym_caret_relevant) {
                    next_relevant = true;
                    continue;
                }
                Type *at = type_expr_from_form(e, _it2,
                                               rec_name, type_params, type_param_kinds, n_type_params);
                if (!at) return NULL;
                arg_full_types_local[arg_idx] = at;
                arg_linear_local[arg_idx] = next_linear;
                arg_unique_local[arg_idx] = next_unique;
                /* UT2: ^unique ^mut combination — track as unique_mut */
                arg_unique_mut_local[arg_idx] = next_unique && next_mut;
                arg_affine_local[arg_idx]   = next_affine;    /* ST0 */
                arg_relevant_local[arg_idx] = next_relevant;  /* ST0 */
                next_linear   = false;
                next_unique   = false;
                next_mut      = false;
                next_affine   = false;   /* ST0 */
                next_relevant = false;   /* ST0 */
                /* Type variables (named TY_TYVAR) and quantified types are
                 * represented as TY_INT (int64_t) at the C level. */
                if (at->kind == TY_TYVAR) {
                    arg_kinds[arg_idx] = TY_INT; /* type variable -> universal int64_t */
                    any_poly_arg = true;
                } else if (at->kind == TY_FORALL || at->kind == TY_EXISTS) {
                    arg_kinds[arg_idx] = TY_PTR_VOID; /* rank-2 fn -> tur_poly_fn_t */
                    any_poly_arg = true;
                } else {
                    arg_kinds[arg_idx] = at->kind;
                    /* Slice 3 (constrained-hkt-forall): a type-application arg
                     * such as `(f a)` (higher-kinded instantiation) or a
                     * concrete `(Box int)` must keep its full type so the
                     * rank-2 validation and instantiation can see the container
                     * shape -- otherwise arg_full_types is dropped and the
                     * `(f a)` body param becomes invisible downstream. */
                    if (at->kind == TY_APP) any_app_arg = true;
                }
                arg_idx++;
            }

            /* Result type — always items[len-1] */
            Type *result_type = type_expr_from_form(e, form->as.list.items[len - 1],
                                                     rec_name, type_params, type_param_kinds, n_type_params);
            if (!result_type) return NULL;

            TypeKind result_kind;
            bool poly_result = false;
            /* fat-closure-dispatch-aggregate-return: an aggregate/applied result
             * type (a TY_APP like (Pair A B), a concrete TY_STRUCT, or a TY_ADT)
             * cannot be reconstructed from its bare TypeKind -- type_from_kind
             * drops the def/args and lowers it to the int64_t carrier.  Preserve
             * the full type so the call site (and the typed-thunk dispatch) emit
             * the real C return type instead of int64_t. */
            bool aggregate_result = false;
            if (result_type->kind == TY_TYVAR) {
                result_kind = TY_INT;
                poly_result = true;
            } else {
                result_kind = result_type->kind;
                /* curried-fn-typed-param: a (fn ...) result must keep its full
                 * Type so a chained application ((f 1) 2) recovers the inner
                 * function type rather than erasing it to a bare result kind. */
                aggregate_result =
                    result_type->kind == TY_FN ||
                    result_type->kind == TY_APP ||
                    result_type->kind == TY_ADT;
            }

            Type *fn_t = (Type *)arena_alloc(e->arena, sizeof(Type));
            *fn_t = type_fn(arg_kinds, n_args, result_kind);

            /* Store full type info if any polymorphic args or an aggregate/poly result */
            if (any_poly_arg || any_app_arg || poly_result || aggregate_result) {
                Type **aFT = (Type **)arena_alloc(e->arena, n_args * sizeof(Type *));
                for (uint32_t i = 0; i < n_args; i++) aFT[i] = arg_full_types_local[i];
                fn_t->as.fn.arg_full_types = aFT;
                if (poly_result || aggregate_result) fn_t->as.fn.result_full_type = result_type;
            }

            /* LT2: Store arg_linear flags if any were annotated */
            {
                bool any_lin = false;
                for (uint32_t i = 0; i < n_args; i++) {
                    if (arg_linear_local[i]) { any_lin = true; break; }
                }
                if (any_lin) {
                    for (uint32_t i = 0; i < n_args; i++) {
                        FN_ARG_SET(fn_t->as.fn, i, FA_LINEAR, arg_linear_local[i]);
                    }
                }
            }

            /* UT0: Store arg_unique flags if any were annotated */
            {
                bool any_uniq = false;
                for (uint32_t i = 0; i < n_args; i++) {
                    if (arg_unique_local[i]) { any_uniq = true; break; }
                }
                if (any_uniq) {
                    for (uint32_t i = 0; i < n_args; i++) {
                        FN_ARG_SET(fn_t->as.fn, i, FA_UNIQUE, arg_unique_local[i]);
                    }
                }
            }

            /* UT2: Store arg_unique_mut flags if any were annotated */
            {
                bool any_uniq_mut = false;
                for (uint32_t i = 0; i < n_args; i++) {
                    if (arg_unique_mut_local[i]) { any_uniq_mut = true; break; }
                }
                if (any_uniq_mut) {
                    for (uint32_t i = 0; i < n_args; i++) {
                        FN_ARG_SET(fn_t->as.fn, i, FA_UNIQUE_MUT, arg_unique_mut_local[i]);
                    }
                }
            }

            /* ST0: Store arg_affine flags if any were annotated */
            {
                bool any_aff = false;
                for (uint32_t i = 0; i < n_args; i++) {
                    if (arg_affine_local[i]) { any_aff = true; break; }
                }
                if (any_aff) {
                    for (uint32_t i = 0; i < n_args; i++) {
                        FN_ARG_SET(fn_t->as.fn, i, FA_AFFINE, arg_affine_local[i]);
                    }
                }
            }

            /* ST0: Store arg_relevant flags if any were annotated */
            {
                bool any_rel = false;
                for (uint32_t i = 0; i < n_args; i++) {
                    if (arg_relevant_local[i]) { any_rel = true; break; }
                }
                if (any_rel) {
                    for (uint32_t i = 0; i < n_args; i++) {
                        FN_ARG_SET(fn_t->as.fn, i, FA_RELEVANT, arg_relevant_local[i]);
                    }
                }
            }

            return fn_t;
        }

        /* SS0b: Session type constructors */
        if (form->as.list.len >= 2
                && form->as.list.items[0]->tag == F_SYM) {
            const Symbol *head_sym = form->as.list.items[0]->as.sym;

            /* (Session P) — linear channel endpoint carrying protocol P */
            if (head_sym == e->sym_session_type && form->as.list.len == 2) {
                Type *proto = type_expr_from_form(e, form->as.list.items[1],
                                                  rec_name, type_params, type_param_kinds, n_type_params);
                if (!proto) return NULL;
                Type *t = (Type *)arena_alloc(e->arena, sizeof(Type));
                *t = type_session(proto);
                return t;
            }
            /* (Send T Q) — send T then continue as Q */
            if (head_sym == e->sym_session_Send && form->as.list.len == 3) {
                Type *msg = type_expr_from_form(e, form->as.list.items[1],
                                                rec_name, type_params, type_param_kinds, n_type_params);
                if (!msg) return NULL;
                Type *cont = type_expr_from_form(e, form->as.list.items[2],
                                                 rec_name, type_params, type_param_kinds, n_type_params);
                if (!cont) return NULL;
                Type *t = (Type *)arena_alloc(e->arena, sizeof(Type));
                *t = type_send(msg, cont);
                return t;
            }
            /* (Recv T Q) — receive T then continue as Q */
            if (head_sym == e->sym_session_Recv && form->as.list.len == 3) {
                Type *msg = type_expr_from_form(e, form->as.list.items[1],
                                                rec_name, type_params, type_param_kinds, n_type_params);
                if (!msg) return NULL;
                Type *cont = type_expr_from_form(e, form->as.list.items[2],
                                                 rec_name, type_params, type_param_kinds, n_type_params);
                if (!cont) return NULL;
                Type *t = (Type *)arena_alloc(e->arena, sizeof(Type));
                *t = type_recv(msg, cont);
                return t;
            }
            /* (Choose P Q) — internal choice: this endpoint picks the branch */
            if (head_sym == e->sym_session_Choose && form->as.list.len == 3) {
                Type *left = type_expr_from_form(e, form->as.list.items[1],
                                                 rec_name, type_params, type_param_kinds, n_type_params);
                if (!left) return NULL;
                Type *right = type_expr_from_form(e, form->as.list.items[2],
                                                  rec_name, type_params, type_param_kinds, n_type_params);
                if (!right) return NULL;
                Type *t = (Type *)arena_alloc(e->arena, sizeof(Type));
                *t = type_choose(left, right);
                return t;
            }
            /* (Branch P Q) — external choice: peer picks; this endpoint waits */
            if (head_sym == e->sym_session_Branch && form->as.list.len == 3) {
                Type *left = type_expr_from_form(e, form->as.list.items[1],
                                                 rec_name, type_params, type_param_kinds, n_type_params);
                if (!left) return NULL;
                Type *right = type_expr_from_form(e, form->as.list.items[2],
                                                  rec_name, type_params, type_param_kinds, n_type_params);
                if (!right) return NULL;
                Type *t = (Type *)arena_alloc(e->arena, sizeof(Type));
                *t = type_branch(left, right);
                return t;
            }
            /* SS3a: (Rec label body) -- recursive protocol mu-type.
             * label is a bare symbol (the recursion variable).
             * Inside body, label may appear as a bare symbol to back-reference this Rec. */
            if (head_sym == e->sym_session_Rec && form->as.list.len == 3) {
                Form *label_form = form->as.list.items[1];
                if (label_form->tag != F_SYM) {
                    diag_emit(DIAG_ERROR, label_form->span,
                              "Rec: label must be a bare symbol, e.g. (Rec self Body)");
                    return NULL;
                }
                const Symbol *label_sym = label_form->as.sym;
                /* Allocate the Rec node first (body filled below). */
                Type *rec_t = (Type *)arena_alloc(e->arena, sizeof(Type));
                *rec_t = type_session_rec(label_sym->name, NULL);
                /* Push label onto rec stack so bare references inside body resolve. */
                if (e->rec_depth >= ELAB_MAX_REC_DEPTH) {
                    diag_emit(DIAG_ERROR, form->span,
                              "Rec: maximum recursive protocol depth exceeded");
                    return NULL;
                }
                e->rec_labels[e->rec_depth] = label_sym;
                e->rec_types[e->rec_depth]  = rec_t;
                e->rec_depth++;
                /* Parse body with the label in scope. */
                Type *body = type_expr_from_form(e, form->as.list.items[2],
                                                 rec_name, type_params, type_param_kinds, n_type_params);
                e->rec_depth--;
                if (!body) return NULL;
                rec_t->as.session_.fst = body;
                return rec_t;
            }
            /* SS3c: (Timeout Q P) -- success continuation Q; timeout continuation P. */
            if (head_sym == e->sym_session_Timeout && form->as.list.len == 3) {
                Type *success = type_expr_from_form(e, form->as.list.items[1],
                                                    rec_name, type_params, type_param_kinds, n_type_params);
                if (!success) return NULL;
                Type *timeout = type_expr_from_form(e, form->as.list.items[2],
                                                    rec_name, type_params, type_param_kinds, n_type_params);
                if (!timeout) return NULL;
                Type *t = (Type *)arena_alloc(e->arena, sizeof(Type));
                *t = type_timeout(success, timeout);
                return t;
            }
            /* SS6: (project G R) -- compute Session[P] where P = project(G, R).
             * G is the protocol name; R is the role name. */
            if (head_sym == e->sym_project_type && form->as.list.len == 3
                    && form->as.list.items[1]->tag == F_SYM
                    && form->as.list.items[2]->tag == F_SYM) {
                const char *proto_name = form->as.list.items[1]->as.sym->name;
                const char *role_name  = form->as.list.items[2]->as.sym->name;

                /* Look up protocol */
                Type *global_t = NULL;
                for (uint32_t gi = 0; gi < e->n_global_protocols; gi++) {
                    if (strcmp(e->global_protocols[gi].name, proto_name) == 0) {
                        global_t = e->global_protocols[gi].type;
                        break;
                    }
                }
                if (!global_t) {
                    diag_emit(DIAG_ERROR, form->span,
                              "project: unknown protocol '%s'", proto_name);
                    return NULL;
                }

                /* Validate role is declared in this protocol */
                bool role_found = false;
                for (int ri = 0; ri < global_t->as.global_.n_roles; ri++) {
                    if (strcmp(global_t->as.global_.roles[ri], role_name) == 0) {
                        role_found = true;
                        break;
                    }
                }
                if (!role_found) {
                    diag_emit_with_code(DIAG_ERROR, form->span, TUR_E0221_ROLE_NOT_DECLARED,
                                        "project: role '%s' is not declared in protocol '%s'",
                                        role_name, proto_name);
                    return NULL;
                }

                /* Intern the role name so pointer comparisons work in project_inner */
                const char *role_interned = intern_cstr(e->st, role_name)->name;

                /* Compute projection */
                Type *proj = session_project(e, global_t->as.global_.body,
                                             role_interned, form->span);
                if (!proj) return NULL;

                /* Wrap in Session[P] */
                Type *t = (Type *)arena_alloc(e->arena, sizeof(Type));
                *t = type_session(proj);
                return t;
            }
            /* SS5: (Global Name) -- reference to a named global protocol type.
             * Looks up the protocol by name and returns a TY_GLOBAL type. */
            if (head_sym == e->sym_global_type && form->as.list.len == 2
                    && form->as.list.items[1]->tag == F_SYM) {
                const char *proto_name = form->as.list.items[1]->as.sym->name;
                for (uint32_t gi = 0; gi < e->n_global_protocols; gi++) {
                    if (e->global_protocols[gi].name == proto_name
                            || strcmp(e->global_protocols[gi].name, proto_name) == 0) {
                        return e->global_protocols[gi].type;
                    }
                }
                diag_emit(DIAG_ERROR, form->span,
                          "Global: unknown protocol '%s'", proto_name);
                return NULL;
            }
            /* SS5: (Role G R) -- role endpoint type for protocol G, role R.
             * G is looked up in the global protocol registry; R is a role name. */
            if (head_sym == e->sym_role_type && form->as.list.len == 3
                    && form->as.list.items[1]->tag == F_SYM
                    && form->as.list.items[2]->tag == F_SYM) {
                const char *proto_name = form->as.list.items[1]->as.sym->name;
                const char *role_name  = form->as.list.items[2]->as.sym->name;
                for (uint32_t gi = 0; gi < e->n_global_protocols; gi++) {
                    if (e->global_protocols[gi].name == proto_name
                            || strcmp(e->global_protocols[gi].name, proto_name) == 0) {
                        Type *global_t = e->global_protocols[gi].type;
                        /* Validate role is declared in this protocol */
                        bool role_found = false;
                        for (int ri = 0; ri < global_t->as.global_.n_roles; ri++) {
                            if (strcmp(global_t->as.global_.roles[ri], role_name) == 0) {
                                role_found = true;
                                break;
                            }
                        }
                        if (!role_found) {
                            diag_emit_with_code(DIAG_ERROR, form->span,
                                                TUR_E0221_ROLE_NOT_DECLARED,
                                                "Role: '%s' is not a declared role in protocol '%s'",
                                                role_name, proto_name);
                            return NULL;
                        }
                        Type *t = (Type *)arena_alloc(e->arena, sizeof(Type));
                        *t = type_role(global_t, role_name, global_t->as.global_.body);
                        return t;
                    }
                }
                diag_emit(DIAG_ERROR, form->span,
                          "Role: unknown protocol '%s'", proto_name);
                return NULL;
            }
        }

        /* LT3: (lref T) — linear owning pointer type expression.
         * Syntax: (lref inner-type)  e.g. (lref int), (lref MyStruct)
         * Produces a TY_LREF type with CK_LINEAR copy kind. */
        if (form->as.list.len == 2
                && form->as.list.items[0]->tag == F_SYM
                && form->as.list.items[0]->as.sym == e->sym_lref) {
            Type *inner_t = type_expr_from_form(e, form->as.list.items[1],
                                                rec_name, type_params, type_param_kinds, n_type_params);
            if (!inner_t) return NULL;
            Type *t = (Type *)arena_alloc(e->arena, sizeof(Type));
            *t = type_lref(inner_t->kind);
            return t;
        }

        /* Empty list () = unit / nil type */
        if (form->as.list.len == 0) {
            Type *t = (Type *)arena_alloc(e->arena, sizeof(Type));
            *t = TYPE_NIL;
            return t;
        }

        /* assoc-types-plan: type-level associated-type projection.
         * `(AssocName T)` where AssocName is an associated type member of some
         * registered typeclass resolves to the concrete type the matching
         * instance binds for that member (e.g. `(Elem (Vec int))` -> int).
         * Resolution matches the instance by precise structural equality on T,
         * so distinct instances of the same constructor (Vec int vs Vec cstr)
         * project to distinct types.  This fires only when AssocName was
         * actually declared as an associated type, so it never intercepts an
         * ordinary type application. */
        if (form->as.list.len >= 2 && form->as.list.items[0]->tag == F_SYM) {
            const Symbol *head_sym = form->as.list.items[0]->as.sym;
            uint8_t assoc_idx;
            if (typeclass_env_find_assoc_type(&e->typeclass_env, head_sym, &assoc_idx)) {
                /* assoc-types-2 (MP4): collect N projection arguments so a
                 * multi-parameter class projects via an exact N-ary match.
                 * `(Elem (Vec int))` stays the n == 1 fast path. */
                uint8_t n_args = (uint8_t)(form->as.list.len - 1);
                Type arg_buf[MAX_FN_ARITY];
                if (n_args > MAX_FN_ARITY) {
                    diag_emit(DIAG_ERROR, form->span,
                              "associated type '%s' projected over too many arguments",
                              head_sym->name);
                    return NULL;
                }
                for (uint32_t i = 0; i < n_args; i++) {
                    Type *a = type_expr_from_form(e, form->as.list.items[i + 1], rec_name,
                                                  type_params, type_param_kinds, n_type_params);
                    if (!a) return NULL;
                    arg_buf[i] = *a;
                }
                const Type *bound = typeclass_env_resolve_assoc_type_n(
                    &e->typeclass_env, head_sym, arg_buf, n_args);
                /* associated-type-unusable-nullary-and-generic (half 2): at a
                 * type variable the projection stays unreduced until a call
                 * fixes the variable. */
                Type proj;
                if (!bound && elab_assoc_projection(e, head_sym, arg_buf, n_args, &proj)) {
                    Type *out = (Type *)arena_alloc(e->arena, sizeof(Type));
                    *out = proj;
                    return out;
                }
                if (!bound) {
                    diag_emit(DIAG_ERROR, form->span,
                              "no instance binding for associated type '%s' at this type",
                              head_sym->name);
                    return NULL;
                }
                Type *out = (Type *)arena_alloc(e->arena, sizeof(Type));
                *out = *bound;
                return out;
            }
        }

        /* Type application: (ctor arg1 arg2 ...) — left-associative currying.
         * (ctor a b) == ((ctor a) b), (ctor a b c) == (((ctor a) b) c), etc.
         * Requires at least 2 elements (ctor + 1 arg). */
        if (form->as.list.len < 2) {
            diag_emit(DIAG_ERROR, form->span,
                      "type expression must be a symbol or (ctor arg ...) application");
            return NULL;
        }

        /* Parse constructor */
        Type *ctor_type = type_expr_from_form(e, form->as.list.items[0], rec_name,
                                              type_params, type_param_kinds, n_type_params);
        if (!ctor_type) return NULL;

        /* Left-associatively apply each argument: (ctor a b c) → (((ctor a) b) c) */
        Type *t = ctor_type;
        for (uint32_t ai = 1; ai < form->as.list.len; ai++) {
            Form *arg_form = form->as.list.items[ai];
            Type *arg_type = NULL;
            /* SZ8 non-GADT: a Size GADT literal (Static N), (Add s s), or
             * (Mul s s) appearing in any type-app argument slot lowers to a
             * placeholder TY_INT type.  The actual size information lives in
             * the retained Form (param_type_forms / ctor result_type_form),
             * where size_term_from_form recovers it for SZ8 unification.
             * Mirrors how GADT constructor return-type signatures (e.g.
             * `(SizedVec (Static 0))`) are stored as raw Forms and consulted
             * separately -- this extends the same treatment to defopaque /
             * defstruct phantom indices and to function return-type slots. */
            if (arg_form->tag == F_LIST &&
                    arg_form->as.list.len >= 1 &&
                    arg_form->as.list.items[0]->tag == F_SYM) {
                const char *op = arg_form->as.list.items[0]->as.sym->name;
                bool is_size_op = (strcmp(op, "Static") == 0 ||
                                   strcmp(op, "Add")    == 0 ||
                                   strcmp(op, "Mul")    == 0);
                if (is_size_op &&
                        size_term_from_form(e->arena, arg_form, NULL, NULL) != NULL) {
                    Type *ph = (Type *)arena_alloc(e->arena, sizeof(Type));
                    *ph = type_from_kind(TY_INT);
                    arg_type = ph;
                }
            }
            if (!arg_type) {
                arg_type = type_expr_from_form(e, arg_form, rec_name,
                                                  type_params, type_param_kinds, n_type_params);
            }
            if (!arg_type) return NULL;
            arg_type = rt_peel_type_arg_contract(arg_type, arg_form->span);

            /* Variadic HKT rows: enforce that a row-of-types argument lands in
             * a row-kinded parameter slot (and vice versa). ctor_type is the
             * original head (carries the StructDef's positional param kinds). */
            if (!check_row_type_arg_kind(*ctor_type, (uint8_t)(ai - 1), *arg_type,
                                         form->as.list.items[ai]->span))
                return NULL;

            /* Create TY_APP node */
            Type *app = (Type *)arena_alloc(e->arena, sizeof(Type));
            memset(app, 0, sizeof(Type));
            app->kind = TY_APP;
            app->copy_kind = CK_COPY;
            propagate_app_discipline(app, t);
            /* Derive kind: step one rung down the arrow ladder. */
            app->hkt_kind = kind_apply_one(t->hkt_kind);
            app->as.app.fn  = t;
            app->as.app.arg = arg_type;
            t = app;
        }
        return t;
    } else if (form->tag == F_VEC) {
        /* TupleN surface sugar: a bracketed type list `[T1 T2 ... Tn]` in
         * type position lowers to the stdlib `TupleN` struct application
         * `(TupleN T1 T2 ... Tn)`.  This mirrors the value-level tuple
         * destructure surface (`(let [[a b] e] ...)`) so a tuple *type*
         * reads like a tuple *pattern*.  Supported arities are 2..8
         * (Tuple2..Tuple8 in stdlib/tuple.tur); for other counts the user
         * should write `(TupleN ...)` or a named struct directly.
         *
         * Closes the KB-029 "residual parser-surface gap": type position
         * previously rejected every `[...]` form outright. */
        uint32_t n = form->as.list.len;
        if (n < 2 || n > 8) {
            diag_emit(DIAG_ERROR, form->span,
                      "tuple type `[...]` must have 2 to 8 element types "
                      "(got %u); use (TupleN ...) or a named struct instead",
                      n);
            return NULL;
        }
        char tuple_name[16];
        int tuple_name_len = snprintf(tuple_name, sizeof(tuple_name),
                                      "Tuple%u", n);
        const Symbol *tuple_sym =
            symtab_intern(e->st, strslice(tuple_name, (uint32_t)tuple_name_len));
        /* Synthesize `(TupleN T1 ... Tn)` and reuse the type-application
         * path (which resolves the struct def and derives the HKT kind). */
        Form **items =
            (Form **)arena_alloc(e->arena, sizeof(Form *) * (n + 1));
        items[0] = form_sym(e->arena, form->span, tuple_sym);
        for (uint32_t i = 0; i < n; i++) {
            items[i + 1] = form->as.list.items[i];
        }
        Form *app = form_list(e->arena, form->span, items, n + 1);
        return type_expr_from_form(e, app, rec_name, type_params,
                                   type_param_kinds, n_type_params);
    } else if (form->tag == F_ROW_LITERAL) {
        /* Variadic HKT rows: `#row{T1 T2 ...}` in type position lowers to a
         * TY_TYPEROW whose elements are the (positional) element type forms.
         * Distinct from the F_VEC tuple sugar above: a row has kind [*]
         * (KIND_TYPEROW), is compile-time only, and is order-significant.
         * An empty `#row{}` is the unit row.
         *
         * P0 typed-field rows: items of the shape `name : T` (a bare F_SYM
         * immediately followed by an F_TYPE_ANN wrapper) lower to a typed-
         * field row whose TY_TYPEROW carries a parallel field_names array.
         * The shape must be homogeneous -- mixing bare types with typed
         * fields in a single literal is rejected. */
        uint32_t n_items = form->as.list.len;
        Form **items_in = form->as.list.items;
        bool any_ann = false;
        for (uint32_t i = 0; i < n_items; i++) {
            if (items_in[i] && items_in[i]->tag == F_TYPE_ANN) { any_ann = true; break; }
        }
        Type **elems = NULL;
        const char **names = NULL;
        uint32_t n = 0;
        if (any_ann) {
            /* Typed-field row: require strict alternation
             * (F_SYM, F_TYPE_ANN, F_SYM, F_TYPE_ANN, ...) with even count. */
            if ((n_items & 1u) != 0) {
                diag_emit(DIAG_ERROR, form->span,
                          "#row{...} typed-field literal needs `name : type` slots; "
                          "got odd number of forms (TUR-E0290)");
                return NULL;
            }
            n = n_items / 2;
            if (n > 0) {
                elems = (Type **)arena_alloc(e->arena, sizeof(Type *) * n);
                names = (const char **)arena_alloc(
                    e->arena, sizeof(const char *) * n);
            }
            e->strict_unknown_types++;
            for (uint32_t i = 0; i < n; i++) {
                Form *kf = items_in[2 * i];
                Form *tf = items_in[2 * i + 1];
                if (!kf || kf->tag != F_SYM || !tf || tf->tag != F_TYPE_ANN) {
                    e->strict_unknown_types--;
                    diag_emit(DIAG_ERROR,
                              (kf ? kf->span : form->span),
                              "#row{...} slot %u: expected `name : type` "
                              "(do not mix bare types with typed fields) "
                              "(TUR-E0290)", i);
                    return NULL;
                }
                /* P0: duplicate field-name check */
                const char *kname = kf->as.sym->name;
                for (uint32_t j = 0; j < i; j++) {
                    if (names[j] && strcmp(names[j], kname) == 0) {
                        e->strict_unknown_types--;
                        diag_emit(DIAG_ERROR, kf->span,
                                  "#row{...} duplicate field name `%s` "
                                  "(TUR-E0291)", kname);
                        return NULL;
                    }
                }
                Type *et = type_expr_from_form(e, tf->as.list.items[0], rec_name,
                                               type_params, type_param_kinds,
                                               n_type_params);
                if (!et) {
                    e->strict_unknown_types--;
                    return NULL;
                }
                elems[i] = et;
                names[i] = kname;
            }
            e->strict_unknown_types--;
        } else {
            n = n_items;
            if (n > 0) {
                elems = (Type **)arena_alloc(e->arena, sizeof(Type *) * n);
                /* L6 follow-up A (strict-row-elements): switch the elaborator
                 * into strict-unknown-name mode while resolving row elements,
                 * so a typo'd component name fails to compile instead of
                 * silently elaborating to a NULL-def opaque placeholder.
                 * Counter-based so a nested #row{...} inside an element type
                 * (e.g. #row{(Query #row{Pos Vel})}) restores correctly. */
                e->strict_unknown_types++;
                for (uint32_t i = 0; i < n; i++) {
                    Type *et = type_expr_from_form(e, items_in[i], rec_name,
                                                   type_params, type_param_kinds,
                                                   n_type_params);
                    if (!et) {
                        e->strict_unknown_types--;
                        return NULL;
                    }
                    elems[i] = et;
                }
                e->strict_unknown_types--;
            }
        }
        if (n > 255) {
            diag_emit(DIAG_ERROR, form->span,
                      "#row{...} type-row has too many elements (%u); max 255", n);
            return NULL;
        }
        Type *row = (Type *)arena_alloc(e->arena, sizeof(Type));
        *row = type_typerow_named(e->arena, elems, names, (uint8_t)n);
        return row;
    } else if (form->tag == F_KEYWORD) {
        /* `:int`, `:bool`, etc. — treat the keyword name as a primitive type lookup */
        const Symbol *sym = form->as.sym;
        TypeKind k = typekind_from_symbol(sym->name);
        if (k != TY_UNKNOWN) {
            Type *t = (Type *)arena_alloc(e->arena, sizeof(Type));
            *t = type_from_kind(k);
            t->c_num_spelling = c_num_spelling_for_name(sym->name);
            return t;
        }
        /* Phase TA1/TA2: check defalias table before struct/ADT fallback */
        {
            const Symbol *alias_sym = symtab_intern(e->st, strslice(sym->name, sym->len));
            for (uint32_t ai = 0; ai < e->n_type_aliases; ai++) {
                if (e->type_alias_names[ai] == alias_sym) {
                    Type *t = (Type *)arena_alloc(e->arena, sizeof(Type));
                    *t = *e->type_alias_types[ai];
                    return t;
                }
            }
        }
        /* F6-1 (cross-plan-followups): look up unknown keyword as a
         * user-defined ADT or struct name so callers that round-trip a
         * declared `:MyADT` field type get a real TY_ADT (with def) /
         * TY_STRUCT (with def) back, not a NULL-def placeholder.  This
         * fixes the `match inner ...` nested-ADT path: previously the
         * extracted binding had TY_STRUCT/def=NULL ("got struct").
         *
         * Walk the scope manually (we don't have access to the
         * scope_lookup_type_def helper that's static in elab_structs.c). */
        const Symbol *type_sym = symtab_intern(e->st, strslice(sym->name, sym->len));
        for (Scope *cur = e->scope; cur; cur = cur->parent) {
            for (uint32_t i = 0; i < cur->n; i++) {
                Binding *cand = cur->bindings[i];
                if (cand->name != type_sym) continue;
                if (cand->type.kind == TY_ADT && cand->type.as.adt_.def) {
                    Type *t = (Type *)arena_alloc(e->arena, sizeof(Type));
                    *t = type_adt(cand->type.as.adt_.def);
                    return t;
                }
            }
        }
        /* ptr-generic-parameterised-type: `:ptr<T>` keyword annotation. */
        {
            Type *pt = ptr_type_from_keyword_name(e, sym->name, sym->len, form->span,
                                                  rec_name, type_params,
                                                  type_param_kinds, n_type_params);
            if (pt) return pt;
        }

        /* rc<T>/weak<T>/ref<T>/lref<T> in ANY type position, not just the defn
         * param/return ladders that call the resolver directly (elab_fns.c).
         * Without this the reference-family spelling reached the tyvar fallback
         * below and became a type variable literally *named* "rc<S>" -- the
         * very footgun rc_family_type_from_keyword_name was written to close,
         * still open everywhere that routes through here: type-application
         * arguments, `::` ascriptions, aliases.
         *
         * The symptom was a real program silently mistyped rather than
         * rejected.  `(:: (vec-new) (Vec rc<S>))` bound the element tyvar A to
         * the *name* "rc<S>", so a later `(vec-push! v a)` with an honest rc<S>
         * value failed unification against its own annotation -- "expected
         * tyvar, got rc<S>" -- while `(Vec S)` and `(Vec int)` were fine.  The
         * error pointed at the argument when the annotation was at fault.
         * Placed beside the ptr<T> hook, which has the same shape and was
         * already wired in here. */
        {
            Type *rt = rc_family_type_from_keyword_name(e, sym->name, sym->len,
                                                        form->span, rec_name,
                                                        type_params,
                                                        type_param_kinds,
                                                        n_type_params);
            if (rt) return rt;
        }

        /* Unknown keyword type -- return an unresolved named type variable.
         * structdef-retirement slice 5 (P8): keyword analog of the bare-symbol
         * fallback above -- emit a named TY_TYVAR rather than a def-less
         * TY_STRUCT. */
        if (e->strict_unknown_types) {
            diag_emit(DIAG_ERROR, form->span,
                      "unknown type name ':%.*s' in #row{...} element position",
                      (int)sym->len, sym->name);
            return NULL;
        }
        Type *t = (Type *)arena_alloc(e->arena, sizeof(Type));
        *t = type_tyvar_named(sym->name);
        t->copy_kind = CK_MOVE;
        return t;
    } else if (form->tag == F_TYPE_ANN) {
        /* Unwrap the `: type-expr` annotation wrapper and process the inner form */
        if (form->as.list.len == 0) {
            diag_emit(DIAG_ERROR, form->span, "empty type annotation");
            return NULL;
        }
        return type_expr_from_form(e, form->as.list.items[0], rec_name,
                                   type_params, type_param_kinds, n_type_params);
    } else if (form->tag == F_CONTRACT_TYPE) {
        /* CT0: { var : T | pred } contract type.
         * items layout: [F_SYM(var), F_TYPE_ANN(T) or F_KEYWORD(:T), F_SYM("|"), pred...]
         * We need at least 4 items: var, type-ann, "|", predicate. */
        if (form->as.list.len < 4) {
            diag_emit(DIAG_ERROR, form->span,
                      "contract type requires { var : T | predicate }");
            return NULL;
        }
        /* Extract var name */
        Form *var_form = form->as.list.items[0];
        const char *var_name = NULL;
        if (var_form->tag == F_SYM) {
            var_name = var_form->as.sym->name;
        }
        /* Extract base type from item[1] */
        Form *type_form = form->as.list.items[1];
        Type *base_type = type_expr_from_form(e, type_form, rec_name,
                                              type_params, type_param_kinds, n_type_params);
        if (!base_type) return NULL;
        /* item[2] should be "|" (sym) — we skip it */
        /* item[3..] is the predicate — use item[3] as the predicate form */
        const Form *pred_form = form->as.list.items[3];

        Type *base_copy = (Type *)arena_alloc(e->arena, sizeof(Type));
        *base_copy = *base_type;

        Type *ct = (Type *)arena_alloc(e->arena, sizeof(Type));
        *ct = type_contract(e->arena, base_copy, var_name, pred_form);
        return ct;
    } else {
        diag_emit(DIAG_ERROR, form->span,
                  "unsupported type expression form (expected symbol, keyword, or list)");
        return NULL;
    }
}

/* Elaborate (deftype Name [type-params...] body)
 *
 * Defines a recursive type alias with kind-polymorphic parameters.
 * Similar to defrec but accepts kind annotations on parameters and a body.
 * In v1, the body is stored but not fully validated.
 *
 * Syntax: (deftype Fix [^f] (Fix (f (Fix f))))
 *         (deftype Name [^f ^a] body)
 *
 * The kind of the deftype is determined by the kind parameters:
 *   []        -> KIND_STAR
 *   [^f]      -> KIND_ARROW
 *   [^f ^a]   -> KIND_ARROW (if ^f is *->* and ^a is *)
 *   [^^f]     -> KIND_ARROW2
 *
 * Phase HKT-P2: This is the primary mechanism for defining Fix and Free.
 */
Expr *elab_deftype(Elab *e, const Form *call) {
    /* Minimum: (deftype Name [params] body) or (deftype Name body) */
    if (call->as.list.len < 2) {
        diag_emit(DIAG_ERROR, call->span,
                  "deftype requires a name: (deftype Name [params] body)");
        return NULL;
    }

    /* Parse name */
    Form *name_form = call->as.list.items[1];
    if (name_form->tag != F_SYM) {
        diag_emit(DIAG_ERROR, name_form->span,
                  "deftype name must be a symbol");
        return NULL;
    }
    const Symbol *name = name_form->as.sym;

    /* Parse type parameters (optional) - arg 2 */
    const Symbol **type_params = NULL;
    Kind         *type_param_kinds = NULL;
    uint8_t n_type_params = 0;
    uint32_t body_index = 2;

    if (call->as.list.len >= 3) {
        Form *params_form = call->as.list.items[2];
        if (params_form->tag == F_VEC) {
            n_type_params = params_form->as.list.len;
            if (n_type_params > 0) {
                type_params = (const Symbol **)arena_alloc(e->arena,
                    n_type_params * sizeof(const Symbol *));
                type_param_kinds = (Kind *)arena_alloc(e->arena,
                    n_type_params * sizeof(Kind));
                for (uint8_t i = 0; i < n_type_params; i++) {
                    type_param_kinds[i] = KIND_STAR;  /* Default kind */
                }

                for (uint8_t i = 0; i < n_type_params; i++) {
                    Form *p = params_form->as.list.items[i];
                    if (p->tag != F_SYM) {
                        diag_emit(DIAG_ERROR, p->span,
                                  "deftype type parameter must be a symbol");
                        return NULL;
                    }
                    /* Phase HKT H1: '^name' prefix marks a kind * -> * (type constructor) */
                    /* Phase HKT H1: '^^name' prefix marks a kind * -> * -> * (binary type constructor) */
                    /* L6 follow-up C: '^&name' prefix marks a row-kinded type parameter
                     * (kind [*], KIND_TYPEROW), the deftype analog of defstruct's `^&`.
                     * The `^&` is stripped so the body references the bare name. */
                    if (p->as.sym->len > 2 && p->as.sym->name[0] == '^' && p->as.sym->name[1] == '&') {
                        const char  *bare     = p->as.sym->name + 2;
                        uint32_t     bare_len = p->as.sym->len  - 2;
                        if (bare_len > 0 && bare[0] >= 'a' && bare[0] <= 'z') {
                            type_params[i]      = symtab_intern(e->st, strslice(bare, bare_len));
                            type_param_kinds[i] = KIND_TYPEROW;
                        } else {
                            diag_emit(DIAG_ERROR, p->span,
                                      "'%s' is not a valid type parameter; "
                                      "use lowercase '^&name' for a row-kinded ('[*]') parameter",
                                      p->as.sym->name);
                            return NULL;
                        }
                    } else if (p->as.sym->len > 2 && p->as.sym->name[0] == '^' && p->as.sym->name[1] == '^') {
                        const char  *bare     = p->as.sym->name + 2;
                        uint32_t     bare_len = p->as.sym->len  - 2;
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
                        if (bare_len > 0 && bare[0] >= 'a' && bare[0] <= 'z') {
                            type_params[i]      = symtab_intern(e->st, strslice(bare, bare_len));
                            type_param_kinds[i] = KIND_ARROW;
                        } else {
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
            body_index = 3;
        }
    }

    /* Parse body - required */
    if (call->as.list.len <= body_index) {
        diag_emit(DIAG_ERROR, call->span,
                  "deftype requires a body type expression");
        return NULL;
    }
    Form *body_form = call->as.list.items[body_index];

    /* Parse the body as a type expression */
    Type *body_type = type_expr_from_form(e, body_form, name,
                                          type_params, type_param_kinds, n_type_params);
    if (!body_type) {
        return NULL;
    }

    /* CT0/RT5b: `(deftype Nat #refine{ x : int | (>= x 0) })` is a REFINEMENT
     * ALIAS, not a recursive type.  Bind the name straight to the contract
     * type so `[n : Nat]` resolves to `{ x : int | (>= x 0) }` -- the
     * parameter takes the base type, the predicate becomes its contract check,
     * and (under `refined`) its hypothesis.  Wrapping it in a TY_REC the way an
     * ordinary deftype body is wrapped would make every use site fail to type
     * (`expected <rec>, got int`), which is what stdlib/refine.tur needs. */
    if (body_type->kind == TY_CONTRACT) {
        if (n_type_params > 0) {
            diag_emit(DIAG_ERROR, call->span,
                      "deftype '%s': a refinement alias takes no type parameters "
                      "in this prototype (the predicate cannot mention them)",
                      name->name);
            return NULL;
        }
        Binding *cb = binding_new(e, name, *body_type, false, true, name_form->span);
        scope_add(&e->global, cb);
        return e_nil(e, call->span);
    }

    /* Phase HKT-P2: Validate guarded recursion
     * The recursive type name must only appear under type constructors
     * (i.e., as an argument to a type constructor, not at the top level) */
    if (!type_is_guarded_recursive(*body_type, name->name)) {
        diag_emit(DIAG_ERROR, body_form->span,
                  "recursive type '%s' must have guarded recursion: "
                  "'%s' appears at top level of its own body; "
                  "wrap it in a type constructor (e.g., (Fix (f (Fix f))) not (Fix Fix))",
                  name->name, name->name);
        return NULL;
    }

    /* Determine the kind of this deftype: prefer kind_for_arity(n_type_params)
     * so a deftype with N params has kind '* -> ... -> *' (arity = N),
     * matching defgadt/defdata. The legacy "pick the highest param kind"
     * heuristic remains as a floor in case a higher-kinded parameter is
     * present, but for the common case (and for the L6 follow-up C `^&`
     * row-kinded case) arity is the right answer. KIND_TYPEROW
     * (0xFFFE, an unrelated sentinel) is skipped in the floor scan so it
     * cannot dominate. */
    Kind result_kind = kind_for_arity(n_type_params);
    for (uint8_t i = 0; i < n_type_params; i++) {
        Kind k = type_param_kinds[i];
        if (k == KIND_TYPEROW) continue;
        if (k > result_kind) {
            result_kind = k;
        }
    }

    /* Create TY_REC type */
    Type rec_type;
    memset(&rec_type, 0, sizeof(rec_type));
    rec_type.kind      = TY_REC;
    rec_type.copy_kind = CK_MOVE;
    rec_type.hkt_kind  = result_kind;
    rec_type.as.rec.name = name->name;
    rec_type.as.rec.body = body_type;

    /* Register in global scope */
    Binding *b = binding_new(e, name, rec_type, false, true, name_form->span);
    scope_add(&e->global, b);

    /* deftype has no runtime effect */
    return e_nil(e, call->span);
}

/* Elaborate (type-app F A)
 *
 * Type-level application: applies type constructor F to type argument A.
 * Creates a TY_APP type and validates kind constraints.
 *
 * Syntax: (type-app result int)
 *         (type-app Fix (result int))
 *
 * The result kind is derived from F's kind:
 *   - If F has kind KIND_ARROW (* -> *), result is KIND_STAR
 *   - If F has kind KIND_ARROW2 (* -> * -> *), result is KIND_ARROW
 *
 * Phase HKT-P1.
 */
Expr *elab_type_app(Elab *e, const Form *call) {
    /* Minimum: (type-app F A) */
    if (call->as.list.len < 3) {
        diag_emit(DIAG_ERROR, call->span,
                  "type-app requires (type-app F A)");
        return NULL;
    }

    /* Parse type constructor F (arg 1) - must be a symbol */
    Form *fn_form = call->as.list.items[1];
    if (fn_form->tag != F_SYM) {
        diag_emit(DIAG_ERROR, fn_form->span,
                  "type-app: type constructor must be a symbol");
        return NULL;
    }

    /* Look up F in the type environment */
    Binding *fn_binding = scope_lookup(e->scope, fn_form->as.sym);
    if (!fn_binding) {
        fn_binding = scope_lookup(&e->global, fn_form->as.sym);
    }
    Type fn_type;
    if (!fn_binding) {
        /* Fall back to typeclass environment: (type-app Functor option) */
        TypeClass *tc = typeclass_env_lookup_typeclass(&e->typeclass_env, fn_form->as.sym);
        if (!tc) {
            diag_emit(DIAG_ERROR, fn_form->span,
                      "type-app: unknown type constructor '%s'", fn_form->as.sym->name);
            return NULL;
        }
        fn_type = type_typeclass(tc);
        /* Set hkt_kind from number of type parameters so kind_of_type_app succeeds */
        fn_type.hkt_kind = kind_for_arity(tc->n_type_params);
    } else {
        fn_type = fn_binding->type;
    }

    /* Parse type argument A (arg 2) - can be a symbol, keyword, or nested type-app */
    Form *arg_form = call->as.list.items[2];
    Type arg_type;
    
    if (arg_form->tag == F_SYM || arg_form->tag == F_KEYWORD || arg_form->tag == F_TYPE_ANN) {
        /* Unwrap F_TYPE_ANN if present */
        if (arg_form->tag == F_TYPE_ANN) {
            arg_form = (arg_form->as.list.len > 0) ? arg_form->as.list.items[0] : arg_form;
        }
        const Symbol *arg_sym = arg_form->as.sym;
        /* Try primitive types first */
        if (arg_sym->len == 3 && memcmp(arg_sym->name, "int", 3) == 0) {
            arg_type = TYPE_INT;
        } else if (arg_sym->len == 4 && memcmp(arg_sym->name, "bool", 4) == 0) {
            arg_type = TYPE_BOOL;
        } else if (arg_sym->len == 4 && memcmp(arg_sym->name, "cstr", 4) == 0) {
            arg_type = TYPE_CSTR;
        } else if ((arg_sym->len == 4 && memcmp(arg_sym->name, "void", 4) == 0) ||
                   (arg_sym->len == 3 && memcmp(arg_sym->name, "nil", 3) == 0)) {
            arg_type = TYPE_NIL;
        } else if (arg_sym->len == 9 && memcmp(arg_sym->name, "ptr<void>", 9) == 0) {
            arg_type = TYPE_PTR_VOID;
        } else {
            /* Look up as a type binding */
            Binding *arg_binding = scope_lookup(e->scope, arg_sym);
            if (!arg_binding) {
                arg_binding = scope_lookup(&e->global, arg_sym);
            }
            if (!arg_binding) {
                /* Unknown name — treat as an unresolved type variable (like
                 * definstance does).  structdef-retirement slice 5 B2 (P2):
                 * emit a named TY_TYVAR carrying the arg name rather than the
                 * legacy def-less TY_STRUCT placeholder.  An unresolved tyvar
                 * already lowers to the int64 carrier in codegen (types.c
                 * type_c_name), matching the previous "container values are
                 * int64-sized opaque handles" intent.  See
                 * ty-struct-null-def-inventory.md P2. */
                arg_type = type_tyvar_named(arg_sym->name);
                arg_type.copy_kind = CK_MOVE;
            } else {
                arg_type = arg_binding->type;
            }
        }
    } else if (arg_form->tag == F_LIST) {
        /* Nested type application: (type-app result (type-app option int))
         * For now, recursively elaborate as type-app */
        Expr *arg_expr = elab_type_app(e, arg_form);
        if (!arg_expr) {
            return NULL;
        }
        diag_emit(DIAG_ERROR, arg_form->span,
                  "type-app: nested type applications not yet supported");
        return NULL;
    } else {
        diag_emit(DIAG_ERROR, arg_form->span,
                  "type-app: type argument must be a symbol, keyword, or type application");
        return NULL;
    }

    /* Create TY_APP type using the type_app helper */
    /* The type is computed but not stored - in v1, type-app is a compile-time-only
     * construct that validates the kind application. Future work will register
     * the result type for use in type annotations. */
    Type app_type = type_app(e->arena, fn_type, arg_type, call->span);
    (void)app_type;  /* Suppress unused variable warning in v1 */

    /* type-app has no runtime effect; return nil */
    return e_nil(e, call->span);
}

/* Phase HRT1: (:: expr type) — type ascription.
 * The ascribed type is checked for well-formedness but erased at codegen.
 * Syntax: (:: expr type-form)
 *
 * TS3.3: when the source and target types are distinct same-size scalar
 * kinds (e.g. `:float` → `:int`, `:i32` → `:f32`), emit an EX_REINTERPRET
 * node so the bit pattern survives the carrier crossing intact.  Without
 * this, downstream codegen would emit an implicit C value cast that
 * truncates floats and rounds integers. */
/* An opaque newtype handle -- `(defopaque Name [T...] :base)` -- is carried at
 * runtime as the int64/pointer carrier slot (a `TY_ADT`/`TY_APP` with
 * is_opaque set).  When such a handle actually holds a closure box, ascribing
 * it to/from a `(fn ...)` type is the fat-handle bridge (slot-0 dispatch), the
 * same way :int / :ptr<void> carriers are. */
static bool ascribe_type_is_opaque_handle(const Type *t) {
    if (!t) return false;
    if (t->kind == TY_ADT)
        return t->as.adt_.def && t->as.adt_.def->is_opaque;
    if (t->kind == TY_APP) {
        AdtDef *def = type_adt_app_def(t);
        return def && def->is_opaque;
    }
    return false;
}

/* A non-parametric, non-recursive by-value ADT/struct product
 * (`(defdata P (P :int :int))`, `defstruct P [x y]`) is a C aggregate with no
 * int64 handle -- it does NOT ride the carrier ABI (see type_uses_carrier_abi /
 * adt_is_byvalue_product).  Opaque newtypes, :heap ADTs, and recursive ADTs are
 * all int64-carried and are excluded: those cast to/from :int soundly.
 *
 * Scoped to the non-parametric TY_ADT case (the reported GAP 3 shape).  A
 * *parametric* by-value monomorph like `(Option int)` is deliberately NOT caught
 * here -- HKT / typeclass code legitimately erases such containers to the int64
 * carrier, and `adt_is_byvalue_product` already requires n_type_params == 0, so
 * a non-parametric product is always TY_ADT, never TY_APP. */
static bool ascribe_type_is_byvalue_aggregate(const Type *t) {
    if (!t || t->kind != TY_ADT) return false;
    const AdtDef *def = t->as.adt_.def;
    return def && !def->is_opaque && !def->is_heap &&
           adt_is_byvalue_product(def);
}

/* A one-word erased carrier that a handle rides -- :int or :ptr<void>.  These
 * are the reinterpret targets a by-value aggregate has no sound bridge to. */
static bool ascribe_type_is_word_carrier(const Type *t) {
    return t && (t->kind == TY_INT || t->kind == TY_PTR_VOID);
}

/* sealed-opaque (GRADUATED 2026-08-17): the AdtDef behind a `:sealed`
 * defopaque, or NULL.  Descends a
 * parameterised head so a phantom-parameterised sealed opaque `(H A)` is caught
 * as well as a bare `H`. */
static const AdtDef *ascribe_sealed_opaque_def(const Type *t) {
    if (!t) return NULL;
    /* A TY_APP spine is left-nested -- `(H A B)` is app(app(H, A), B) -- so walk
     * down to the head rather than unwrapping one level. */
    while (t->kind == TY_APP && t->as.app.fn) t = t->as.app.fn;
    if (t->kind != TY_ADT) return NULL;
    const AdtDef *def = t->as.adt_.def;
    return (def && def->is_opaque && def->sealed) ? def : NULL;
}

/* Reject a `::` that crosses a sealed opaque's representation boundary from
 * outside the module that declared it.  Returns true if a diagnostic was
 * emitted.  Unconditional since the sealed-opaque graduation (2026-08-17); see
 * docs/archive/sealed-opaque-plan.md.
 *
 * The rule is "exactly one side is this sealed type": an identity relabel
 * (`(:: w H)` where w is already an H) crosses nothing and stays legal, while
 * both the unwrap (`(:: w :int)`) and the fabricate (`(:: n H)`) directions are
 * refused.  Sealing BOTH directions is what makes the representation private
 * rather than merely hard to rebuild -- see the plan's "why both directions". */
/* option-niche static :non-null enforcement: the AdtDef behind a `:non-null`
 * defopaque, or NULL.  Descends a parameterised head, mirroring
 * ascribe_sealed_opaque_def above. */
static const AdtDef *ascribe_non_null_opaque_def(const Type *t) {
    if (!t) return NULL;
    while (t->kind == TY_APP && t->as.app.fn) t = t->as.app.fn;
    if (t->kind != TY_ADT) return NULL;
    const AdtDef *def = t->as.adt_.def;
    return (def && def->is_opaque && def->opaque_non_null) ? def : NULL;
}

/* Reject `(:: 0 T)` where T is a `:non-null` opaque -- a statically PROVABLE
 * violation of the declaration.  The literal is peeled through nested
 * ascriptions/reinterprets so the two spellings of the same forge --
 * `(:: 0 :String)` and `(:: (:: 0 :ptr<void>) String)` -- are both caught.
 * Deliberately literal-0-only: a computed zero is not provable here and stays
 * the runtime Some-ctor check's job (as does inline-C, which no static check
 * can reach).  Returns true if a diagnostic was emitted. */
static bool ascribe_check_non_null_zero(const Form *call,
                                        const Expr *inner, const Type *to) {
    const AdtDef *nn = ascribe_non_null_opaque_def(to);
    if (!nn) return false;
    const Expr *v = inner;
    while (v && (v->kind == EX_ASCRIBE || v->kind == EX_REINTERPRET))
        v = v->kind == EX_ASCRIBE ? v->as.ascribe_.inner
                                  : v->as.reinterpret_.expr;
    if (!v || v->kind != EX_INT_LIT || v->as.i != 0) return false;
    diag_emit_with_code(
        DIAG_ERROR, call->span, TUR_E0303_NON_NULL_OPAQUE_ZERO,
        "cannot ascribe the literal 0 into '%s' -- its :non-null declaration "
        "says the handle's valid values exclude the null pointer. Express "
        "absence as (none) / option<%s>, or build the value through '%s's "
        "real constructors",
        nn->name ? nn->name : "<opaque>",
        nn->name ? nn->name : "T",
        nn->name ? nn->name : "<opaque>");
    return true;
}

static bool ascribe_check_sealed(Elab *e, const Form *call,
                                 const Type *from, const Type *to) {
    const AdtDef *sf = ascribe_sealed_opaque_def(from);
    const AdtDef *st = ascribe_sealed_opaque_def(to);
    if (sf == st) return false;               /* both sides same sealed def, or neither */

    const AdtDef *sealed_def = sf ? sf : st;
    /* A moduleless top level has no module identity to compare, so nothing is
     * "outside" it -- single-file programs are exactly where sealing has the
     * least to offer, and erroring there would be noise.  Documented as a known
     * limitation in the plan. */
    if (!sealed_def->sealed_module) return false;
    if (e->current_module_name == sealed_def->sealed_module) return false;

    diag_emit_with_code(
        DIAG_ERROR, call->span, TUR_E0302_SEALED_OPAQUE_CAST,
        "cannot %s sealed opaque '%s' %s its representation -- '%s' is sealed to "
        "module '%s', so `::` may not cross that boundary here",
        sf ? "unwrap" : "fabricate",
        sealed_def->name ? sealed_def->name : "<opaque>",
        sf ? "to" : "from",
        sealed_def->name ? sealed_def->name : "<opaque>",
        sealed_def->sealed_module->name);
    return true;
}

Expr *elab_ascribe(Elab *e, const Form *call) {
    if (call->as.list.len != 3) {
        diag_emit(DIAG_ERROR, call->span,
                  "'::' requires exactly two arguments: (:: expr type)");
        return NULL;
    }
    Form *expr_form = call->as.list.items[1];
    Form *type_form = call->as.list.items[2];

    /* Parse the ascribed type.
     *
     * Thread the enclosing definition's in-scope type variables
     * (`e->sig_tyvars`, populated while elaborating a defn/instance body) into
     * the resolver as `type_params`.  type_expr_from_form checks a bare name
     * against `type_params` BEFORE the global type table, so an instance tyvar
     * like `A` shadows a same-named global type.  Without this, a user program
     * that defines a top-level type named `A` (a common tyvar name) captures
     * the `A` in stdlib ascriptions such as `(:: t1 (Cons A))` in the `Eq Cons`
     * instance body -- the `(Eq A)` constraint stops behaving as a dictionary
     * tyvar and `.eq?` dispatch goes ambiguous.  See
     * docs/archive/history/m5-suite-residual-6-failures-2026-06-14.md (root cause A). */
    const Symbol **scope_tyvars = NULL;
    uint8_t n_scope_tyvars = e->n_sig_tyvars;
    if (n_scope_tyvars > 0) {
        scope_tyvars = (const Symbol **)arena_alloc(
            e->arena, sizeof(const Symbol *) * n_scope_tyvars);
        for (uint8_t i = 0; i < n_scope_tyvars; i++) {
            scope_tyvars[i] = e->sig_tyvars[i]
                ? intern_cstr(e->st, e->sig_tyvars[i]) : NULL;
        }
    }
    Type *ascribed = type_expr_from_form(e, type_form, NULL,
                                         scope_tyvars, NULL, n_scope_tyvars);
    if (!ascribed) return NULL;

    /* Phase RT: push the ascribed type onto the expected-type channel so that
     * an enclosed call with a return-resolved typeclass constraint can resolve
     * its instance from T.  Save/restore for proper nesting. */
    Type *saved_expected = e->expected_type;
    e->expected_type = ascribed;
    /* Elaborate the expression */
    Expr *inner = elab_form(e, expr_form);
    e->expected_type = saved_expected;
    if (!inner) return NULL;

    /* any-narrowing-ascription-does-not-compile: `::` cannot narrow OUT of an
     * `any`, and every spelling of it was broken in a different way.
     *
     * `::` is a REPRESENTATION ASSERTION -- on the compiled path the carrier
     * word is reinterpreted bit-for-bit -- and that is coherent exactly while
     * the operand is a one-word carrier.  An `any` is not: it is the two-word
     * `tur_tagged_t`, so there is no single word to reinterpret from.  Measured,
     * with `a` an `any` holding 7.1:
     *
     *   (:: a int)    cc: aggregate value used where an integer was expected
     *   (:: a float)  cc: aggregate value used where a floating-point was ...
     *   (:: a cstr)   cc: incompatible type for argument 1 of 'puts'
     *   (:: a bool)   cc: used struct type value where scalar is required
     *   (:: a Pt)     cc: 'tur_tagged_t' has no member named 'x'
     *
     * -- four different cc errors with no Turmeric diagnostic in front of any
     * of them.  And under --interpret, four WRONG ANSWERS instead: `int` prints
     * 4619679907765970534 (the IEEE-754 bits), while `cstr`, `bool` and `float`
     * all print 7.1, because the interpreter's EX_ASCRIBE arm coerces only on a
     * tag mismatch and is otherwise transparent.  A union target is a third
     * behaviour again: it type-checks and silently does not narrow at all (the
     * result stays `any`).
     *
     * Both back ends are doing what `::` MEANS.  The defect is that `::`
     * accepts an `any` operand at all, so it is refused here -- the same shape
     * as the owning-value rule and the int/float ambiguity refusal further
     * down: reject the spelling that cannot work and name the one that does.
     *
     * Placed ABOVE the union-target branch, not with the scalar-reinterpret
     * rules beside those two.  That branch calls `elab_coerce_to_union`, which
     * accepts an `any` operand that is not a member of the union and leaves the
     * result typed `any` -- the silent no-op above.  Below it, the union row
     * stays broken; above it, all six targets get this one message.
     *
     * `cast` is that one, and it is correct for EVERY target measured above --
     * scalars, structs and unions -- on both back ends, panicking with
     * "cast: any holds float, not int" on a tag mismatch.  It is also what D5's
     * Saffron -> Turmeric boundary already inserts, so pointing at it keeps the
     * hand-written spelling and the compiler-inserted one the same operation.
     *
     * Ascribing an `any` TO `any` stays legal and is load-bearing: it is
     * identity, and it is what the Saffron data-literal widen emits for an
     * element that is already `any` (`dl_saffron_widen_elem`). */
    if (inner->type.kind == TY_ANY && ascribed->kind != TY_ANY) {
        diag_emit(DIAG_ERROR, call->span,
                  "`::` cannot narrow an `any` to %s: `::` asserts a "
                  "REPRESENTATION, and an `any` is a two-word tagged value with "
                  "no single word to reinterpret from",
                  type_name(*ascribed));
        diag_emit(DIAG_NOTE, call->span,
                  "use (cast x %s), which reads the tag the `any` is carrying "
                  "and panics if it is not %s",
                  type_name(*ascribed), type_name(*ascribed));
        return NULL;
    }
    /* GAP 3 (byvalue-adt-int-cast-plan Part B): `(:: v :any)` is the explicit
     * coercion of a value into the `any` erased carrier.  It must heap-box the
     * value (EX_UNION_INJECT), exactly as passing it to an `any`-typed parameter
     * does -- NOT merely relabel its static type, which miscompiles a by-value
     * aggregate (a cc "aggregate used where integer expected" error compiled;
     * diverging under --interpret).  This is the explicit "box" spelling the
     * TUR-E0295 diagnostic points at; `(cast h T)` reads it back by value. */
    if (ascribed->kind == TY_ANY && inner->type.kind != TY_ANY) {
        return elab_coerce_to_any(e, inner);
    }

    /* union-tagged-union-c-emission: `(:: v (A | B))` is the same coercion one
     * type up.  Ascribing a MEMBER value to the union must tag it, exactly as
     * passing it to a union-typed parameter does; relabelling the static type
     * alone hands a raw aggregate to a `tur_tagged_t` slot, which is a hard cc
     * error ("invalid initializer" / "incompatible types when returning"), not a
     * miscompile that runs.  This is the ascription spelling, and it is what a
     * caller reaches for to put a member in a local or return one. */
    if (ascribed->kind == TY_UNION && inner->type.kind != TY_UNION) {
        return elab_coerce_to_union(e, inner, ascribed);
    }

    /* sealed-opaque: refuse to cross a sealed opaque's representation boundary
     * from outside its declaring module.  Placed before the remaining coercion
     * rules because it is a visibility question, not a representation one --
     * whether the cast would LOWER soundly is beside the point if the caller is
     * not allowed to express it. */
    if (ascribe_check_sealed(e, call, &inner->type, ascribed)) return NULL;

    /* option-niche: a literal 0 into a :non-null opaque is a provable
     * violation of the declaration -- error here instead of aborting at the
     * niche Some ctor at runtime.  Placed with the sealed check because it is
     * the same kind of question (is this coercion allowed to be EXPRESSED),
     * not a representation-lowering one. */
    if (ascribe_check_non_null_zero(call, inner, ascribed)) return NULL;

    /* fn-value-carrier-fat-seam-residuals (ascribed-alias variant): ascribing
     * a CARRIER (tur_poly_fn_t) fn param to a fn type -- `(:: v (fn [] int))`
     * where `v : (fn [] int)` is is_poly_fn -- is a pure type assertion, not a
     * representation change.  Wrapping it in an EX_ASCRIBE node hands the let-
     * binding path a TY_FN-typed init it bridges with the generic
     * `(int64_t)(intptr_t)` pointer cast, which is invalid C on the by-value
     * aggregate.  Return the var itself: an alias binding then copies the
     * tur_poly_fn_t by value (the shape that works), and the tail machinery's
     * alias resolution classifies it by its binding as usual. */
    if (ascribed->kind == TY_FN && inner->kind == EX_VAR &&
        inner->as.var.binding && inner->as.var.binding->is_poly_fn &&
        inner->as.var.binding->is_param)
        return inner;

    /* CONV-S1 (defstruct-as-defadt, seam 4): ascribing a concrete ADT app onto
     * an ADT constructor call whose own type is left bare/under-applied.  A
     * parametric struct whose type parameter is not pinned by its field types
     * (e.g. `(defstruct ArrW [a] (raw :int))` -- `a` never appears in a field)
     * gives `(make-struct ArrW 0)` the bare TY_ADT result type; only the
     * ascription `(:: ... (ArrW int))` knows the monomorph.  Without refining
     * the ctor call's type, codegen emits the int64-carrier base `ctor_ArrW`
     * but the ascribed binding is declared `tur_adt_ArrW__int`, so the
     * initializer is a hard cc type error.  When the ascription names a concrete
     * app of the SAME ADT the ctor builds, push that app type onto the ctor call
     * so the per-monomorph ctor (`ctor_ArrW__int`) is selected. */
    if (ascribed->kind == TY_APP && inner->kind == EX_CALL &&
        inner->as.call_.ctor &&
        (inner->type.kind == TY_ADT || inner->type.kind == TY_APP)) {
        AdtDef *asc_def = type_adt_app_def(ascribed);
        AdtDef *inner_def = (inner->type.kind == TY_ADT)
            ? inner->type.as.adt_.def : type_adt_app_def(&inner->type);
        if (asc_def && asc_def == inner_def)
            inner->type = *ascribed;
    }

    /* Producer-side fat-boxing: a bare (captureless) closure ascribed to a
     * pointer-carrier handle -- an opaque newtype whose runtime carrier is the
     * int64/ptr fat-box slot, or :ptr<void> itself -- must cross as a uniform
     * { thunk, env } fat box, exactly like a closure escaping into a TY_TYVAR
     * parameter (see elab_call.c "captureless-closure-lost-through-untyped-vec").
     * A capturing closure's value is already TY_PTR_VOID (a fat box) and is left
     * alone; only a bare, unboxed TY_FN is shimmed via EX_FN_TO_FAT.  Without
     * this, `(:: (fn [s] ...) :Goal)` on a captureless lambda stores a raw code
     * address that a later slot-0 fat-dispatch reads as a thunk -> SIGSEGV
     * (docs/archive/history/logic-port-language-gaps.md GAP 1). */
    if ((ascribed->kind == TY_PTR_VOID || ascribe_type_is_opaque_handle(ascribed)) &&
        inner->type.kind == TY_FN && !inner->type.as.fn.boxed) {
        uint32_t box_ar = inner->type.as.fn.arity;
        if (box_ar >= 1 && box_ar <= 5) {
            Type *bt = (Type *)arena_alloc(e->arena, sizeof(Type));
            *bt = inner->type;
            bt->as.fn.boxed = true;
            Expr *shim = expr_new(e->arena, EX_FN_TO_FAT, *bt, inner->span);
            shim->as.fn_to_fat_.inner = inner;
            inner = shim;
        }
    }

    /* GAP 3 (byvalue-adt-int-cast-plan): reject `::` between a by-value
     * aggregate and a one-word carrier (:int / :ptr<void>) in either direction.
     * A by-value ADT/struct product is a C aggregate with no int64 handle, so
     * the reinterpret has no sound lowering: emit would assign an aggregate into
     * an int slot (a raw cc error) or read an int as the struct (a segfault),
     * and the two harnesses diverge (the interpreter yields a garbage handle /
     * throws).  Diagnosing it here -- in the shared elaborator -- fixes both
     * paths at once.  A recursive ADT rides the carrier and is NOT caught (it
     * casts soundly); opaque/:heap handles are excluded by the predicate. */
    if ((ascribe_type_is_byvalue_aggregate(&inner->type) &&
         ascribe_type_is_word_carrier(ascribed)) ||
        (ascribe_type_is_word_carrier(&inner->type) &&
         ascribe_type_is_byvalue_aggregate(ascribed))) {
        const Type *agg = ascribe_type_is_byvalue_aggregate(&inner->type)
            ? &inner->type : ascribed;
        const char *agg_name =
            (agg->kind == TY_ADT && agg->as.adt_.def) ? agg->as.adt_.def->name
            : (agg->kind == TY_APP && type_adt_app_def(agg))
                ? type_adt_app_def(agg)->name
                : "aggregate";
        diag_emit_with_code(DIAG_ERROR, call->span,
            TUR_E0295_BYVALUE_CARRIER_CAST,
            "cannot reinterpret by-value aggregate '%s' as a one-word carrier "
            "(:int / :ptr<void>); it is a C struct with no int64 handle. To carry "
            "it through an erased handle, box it into `any`: `(:: v :any)` (or "
            "just pass it where an `any` is expected) heap-boxes it, and "
            "`(cast h %s)` reads it back by value.",
            agg_name, agg_name);
        return NULL;
    }

    /* TS3.3: if both source and target are scalar same-size kinds and they
     * differ, insert an EX_REINTERPRET. */
    TypeKind src_kind = inner->type.kind;
    TypeKind dst_kind = ascribed->kind;
    /* collections-cannot-hold-rc-values item 2, ascription side.  The carrier
     * rules added there run in elab_call.c, so they cover CALL arguments and
     * results only -- `::` builds its EX_REINTERPRET here and walked straight
     * past them.  That left the check trivially defeatable:
     *
     *     (vec-push! v (:: a :int))     ;; a : rc<S>
     *
     * type-checks, stores the bare control-block pointer in a slot nobody owns,
     * and the program's refcounts are nonsense from that point on -- measured:
     * `(rc/strong-count a)` printing garbage after the vec is freed.
     *
     * Two asymmetric rules, because the two directions are not equally bad:
     *
     *   OWNING -> anything is always rejected.  Erasing a counted handle to a
     *   machine integer is exactly the `:int` type-erasure the codebase rules
     *   out, and there is no shape where it is what the author meant.
     *
     *   anything -> OWNING is rejected EXCEPT for the literal 0.  That one form
     *   is a null handle -- the only way to write an empty rc without inline-C
     *   (see docs/archive/inline-c-rc-return-misses-carrier-bridge.md), and
     *   what stdlib/rcchain.tur's `rcchain-nil` uses.  Any other integer
     *   fabricates a control-block pointer out of arithmetic. */
    {
        bool src_owning = src_kind == TY_RC || src_kind == TY_WEAK ||
                          src_kind == TY_REF || src_kind == TY_LREF;
        bool dst_owning = dst_kind == TY_RC || dst_kind == TY_WEAK ||
                          dst_kind == TY_REF || dst_kind == TY_LREF;
        if (src_owning && src_kind != dst_kind) {
            diag_emit(DIAG_ERROR, call->span,
                      "cannot reinterpret an owning value (%s) as %s: the "
                      "handle is reference-counted, and erasing it to an "
                      "unmanaged type leaves a pointer nobody owns. Pass the "
                      "%s itself, or use rc/ptr for a borrowed raw pointer",
                      typekind_to_string(src_kind),
                      typekind_to_string(dst_kind),
                      typekind_to_string(src_kind));
            return NULL;
        }
        if (dst_owning && !src_owning &&
            !(inner->kind == EX_INT_LIT && inner->as.i == 0)) {
            diag_emit(DIAG_ERROR, call->span,
                      "cannot reinterpret %s as an owning value (%s): only the "
                      "literal 0 (a null handle) may be ascribed to a "
                      "reference-counted type",
                      typekind_to_string(src_kind),
                      typekind_to_string(dst_kind));
            return NULL;
        }
    }
    /* float32-ascribed-literal-compares-as-double: a float LITERAL ascribed
     * to a float kind is retyped in place, exactly as if it had been written
     * with the width suffix (`(:: 7.1 float32)` == `7.1f32`).  Without this
     * the sizes differ (8 vs 4), no reinterpret is built, and the EX_ASCRIBE
     * wrapper is erased at codegen -- so the literal renders as the bare
     * DOUBLE literal, and any mixed C expression promotes the float32
     * operand up to it: `(= (mono (:: 7.1 float32)) (:: 7.1 float32))` was
     * `false` compiled and `true` under turi.  A literal is the one shape
     * where the narrowing is unambiguous -- the author wrote a constant AT
     * that width; there is no runtime value to preserve the wider bits of.
     * (A non-literal float64 expression ascribed to float32 keeps today's
     * erased-ascription behavior; narrowing IT is a value conversion with a
     * representation question this arm deliberately does not answer.) */
    if (inner->kind == EX_FLOAT_LIT &&
        (src_kind == TY_FLOAT || src_kind == TY_FLOAT32 ||
         src_kind == TY_FLOAT64) &&
        (dst_kind == TY_FLOAT || dst_kind == TY_FLOAT32 ||
         dst_kind == TY_FLOAT64)) {
        inner->type = *ascribed;
        return inner;
    }
    /* ascribe-int-to-float-reinterprets: an INTEGER literal ascribed to a
     * float kind is the number, not its bit pattern.  `(:: 3 :float)` printed
     * 1.4822e-323 -- the double whose bits are 0x...03 -- because :int and
     * :float are both 8 bytes and the same-size rule below turned the
     * ascription into an EX_REINTERPRET.  The bug was width-dependent in a way
     * that made it obviously unintended: `(:: 3 :float32)` printed 3, since 8
     * != 4 misses that rule, so one operator disagreed with itself across
     * float widths.
     *
     * Scoped to literals on purpose.  `::` from an int-typed EXPRESSION to a
     * float could equally mean a bit-reinterpret -- that is the carrier
     * round-trip typed slots, variadic rest collection, and the cons/HAMT
     * carriers depend on, where a value's static type is :int while it holds
     * float bits.  Nothing in the static types separates that from a genuine
     * int being converted, so no reading can be picked for it; that case is
     * refused by the arm below, and the two meanings are spelled
     * `int->float` (stdlib/math.tur) and `bits->float` (stdlib/bits.tur).
     * A literal is the one shape with no such ambiguity -- the author wrote a
     * constant, and there is no carried value whose bits could be meant.  This
     * mirrors the float-literal arm directly above, which retypes for the same
     * reason.
     *
     * The residual expression-level ambiguity is resolved by the arm directly
     * below, which refuses the ambiguous spelling outright rather than picking
     * a reading; see
     * docs/archive/ascribe-int-to-float-expression-ambiguity.md. */
    if ((dst_kind == TY_FLOAT || dst_kind == TY_FLOAT32 ||
         dst_kind == TY_FLOAT64) &&
        (src_kind == TY_INT || src_kind == TY_INT64 ||
         src_kind == TY_INT32 || src_kind == TY_INT16 || src_kind == TY_INT8 ||
         src_kind == TY_UINT32 || src_kind == TY_UINT16 || src_kind == TY_UINT8)) {
        /* Peel integer-typed ascription wrappers to reach the literal, so a
         * stepped `(:: (:: 3 i32) f32)` reads the same as a direct one.
         *
         * Each hop must hold the value EXACTLY.  Today that guard never fires:
         * an integer-to-integer `::` is erased at codegen and does not narrow
         * (`(:: 300 :int8)` prints 300, not 44), so peeling could not observe
         * a truncated value even if it ignored the question.  It is here so
         * that this arm does not silently become wrong the day narrowing is
         * implemented -- at which point `(:: (:: 300 :int8) :float)` means
         * 44.0, and folding it to 300.0 would trade one wrong answer for
         * another.  Declining falls through to the existing path rather than
         * inventing a third behavior. */
        const Expr *lit_src = inner;
        bool exact = true;
        while (lit_src && lit_src->kind == EX_ASCRIBE) {
            TypeKind k = lit_src->type.kind;
            if (k != TY_INT && k != TY_INT64 && k != TY_INT32 &&
                k != TY_INT16 && k != TY_INT8 && k != TY_UINT32 &&
                k != TY_UINT16 && k != TY_UINT8) { exact = false; break; }
            lit_src = lit_src->as.ascribe_.inner;
        }
        if (exact && lit_src && lit_src->kind == EX_INT_LIT) {
            int64_t v = lit_src->as.i;
            /* Re-walk the chain outward-in, checking the value survives each
             * declared width unchanged. */
            for (const Expr *w = inner; exact && w && w->kind == EX_ASCRIBE;
                 w = w->as.ascribe_.inner) {
                switch (w->type.kind) {
                    case TY_INT8:   exact = (v >= INT8_MIN  && v <= INT8_MAX);  break;
                    case TY_INT16:  exact = (v >= INT16_MIN && v <= INT16_MAX); break;
                    case TY_INT32:  exact = (v >= INT32_MIN && v <= INT32_MAX); break;
                    case TY_UINT8:  exact = (v >= 0 && v <= UINT8_MAX);  break;
                    case TY_UINT16: exact = (v >= 0 && v <= UINT16_MAX); break;
                    case TY_UINT32: exact = (v >= 0 && v <= UINT32_MAX); break;
                    default: break;   /* int / int64 -- no narrowing */
                }
            }
            if (exact) {
                Expr *lit = expr_new(e->arena, EX_FLOAT_LIT, *ascribed, call->span);
                lit->as.f = (double)v;
                return lit;
            }
        }
    }
    /* ascribe-int-to-float-expression-ambiguity: `::` between an int kind and a
     * float kind means two different things, and nothing in the static types
     * separates them.  `(:: (.n m) :float)` on a genuine integer wants the
     * NUMBER; `(:: (list-head c) :float)` on a value that reached an :int slot
     * through the carrier wants the BITS.  Both operands are statically :int.
     *
     * The same-size rule below used to answer "bits", silently, in both cases.
     * That reading is right for the carrier round-trip that typed slots,
     * variadic rest collection and the cons/HAMT carriers depend on -- and
     * catastrophically wrong for the other, where the int becomes a denormal
     * that contributes nothing to a sum.  A dropped term, not a garbage one,
     * which is what made it easy to miss.
     *
     * Neither reading gets to win by default: whichever were chosen, the other
     * one's existing code would keep compiling and start returning silently
     * wrong answers.  So the ambiguous spelling is refused and the author says
     * which they meant.  Both spellings already exist and neither is new
     * syntax -- `int->float` / `float->int` in stdlib/math.tur convert,
     * `bits->float` / `float->bits` in stdlib/bits.tur reinterpret.
     *
     * LITERALS are exempt, and keep converting: the author wrote a constant, so
     * there is no carried value whose bits could be meant.  That half was fixed
     * separately (the arm above, and ascribe-int-to-float-reinterprets). */
    {
        bool src_int = (src_kind == TY_INT || src_kind == TY_INT64 ||
                        src_kind == TY_INT32 || src_kind == TY_INT16 ||
                        src_kind == TY_INT8 || src_kind == TY_UINT64 ||
                        src_kind == TY_UINT32 || src_kind == TY_UINT16 ||
                        src_kind == TY_UINT8);
        bool dst_int = (dst_kind == TY_INT || dst_kind == TY_INT64 ||
                        dst_kind == TY_INT32 || dst_kind == TY_INT16 ||
                        dst_kind == TY_INT8 || dst_kind == TY_UINT64 ||
                        dst_kind == TY_UINT32 || dst_kind == TY_UINT16 ||
                        dst_kind == TY_UINT8);
        bool src_flt = (src_kind == TY_FLOAT || src_kind == TY_FLOAT32 ||
                        src_kind == TY_FLOAT64);
        bool dst_flt = (dst_kind == TY_FLOAT || dst_kind == TY_FLOAT32 ||
                        dst_kind == TY_FLOAT64);
        if ((src_int && dst_flt) || (src_flt && dst_int)) {
            const char *conv = (src_int && dst_flt) ? "int->float" : "float->int";
            const char *bits = (src_int && dst_flt) ? "bits->float" : "float->bits";
            diag_emit(DIAG_ERROR, call->span,
                      "`::` between an integer and a float kind is ambiguous: it "
                      "could convert the NUMBER or reinterpret the BIT PATTERN, "
                      "and the static types do not say which");
            diag_emit(DIAG_NOTE, call->span,
                      "to convert, use (%s x) -- (load \"stdlib/math.tur\")", conv);
            diag_emit(DIAG_NOTE, call->span,
                      "to reinterpret the bits, as when reading a float back out "
                      "of an :int carrier slot (a cons cell, a variadic rest "
                      "list, a HAMT value), use (%s x) -- (load "
                      "\"stdlib/bits.tur\")", bits);
            return NULL;
        }
    }
    if (src_kind != dst_kind) {
        int src_size = type_size_bytes(src_kind);
        int dst_size = type_size_bytes(dst_kind);
        if (src_size > 0 && dst_size > 0 && src_size == dst_size) {
            Expr *r = expr_new(e->arena, EX_REINTERPRET, *ascribed, call->span);
            r->as.reinterpret_.expr = inner;
            r->as.reinterpret_.source_kind = src_kind;
            r->as.reinterpret_.target_kind = dst_kind;
            return r;
        }
    }

    /* Build ascription node — the expression's type is overridden by the ascribed type
     * for downstream type-checking purposes. Type is erased at codegen. */
    Expr *out = expr_new(e->arena, EX_ASCRIBE, *ascribed, call->span);
    out->as.ascribe_.inner = inner;
    out->as.ascribe_.type_form = type_form;

    /* declared-size-index-never-checked-against-value (B): an ascription that
     * pins a size index is a claim about `inner`; when the inner's own index
     * is statically known and disagrees, the claim is false and is rejected
     * here rather than laundered into every downstream call.  An inner whose
     * index is open (`sized-buf-new`'s polymorphic `n`) is skipped -- that is
     * the legitimate pin-by-ascription case. */
    {
        int64_t claimed, actual;
        if (sz_claim_disagrees(e, type_form, inner, &claimed, &actual)) {
            diag_emit_with_code(DIAG_ERROR, call->span,
                TUR_E0260_SIZED_TYPE_MISMATCH,
                "sized type mismatch (TUR-E0260): ascription declares size "
                "%lld but the expression has size %lld",
                (long long)claimed, (long long)actual);
            return NULL;
        }
        /* gadt-length-index-not-enforced: the same claim about a GADT's
         * phantom index -- `(:: (VNil) (Vec (Succ Zero)))` is false. */
        const Form *got = NULL;
        if (gadt_claim_disagrees(e, type_form, inner, &got)) {
            Buf cb; buf_init(&cb); form_print(&cb, type_form); buf_putc(&cb, '\0');
            Buf gb; buf_init(&gb); form_print(&gb, got); buf_putc(&gb, '\0');
            diag_emit_with_code(DIAG_ERROR, call->span, TUR_E0001_TYPE_MISMATCH,
                "ascription declares %s but the expression has type %s",
                cb.data, gb.data);
            buf_free(&cb); buf_free(&gb);
            return NULL;
        }
    }

    /* Fat-handle ascription: reinterpreting a one-word carrier (:int or
     * :ptr<void>) as a function type means "this carrier already holds a
     * { thunk, env } fat-closure handle" -- e.g. a closure pulled out of a
     * cons cell with list-head, or a handler threaded around as :int.  Mark
     * the resulting TY_FN as *boxed* so a ^fat parameter passes it through and
     * fat-dispatches it (slot 0), instead of auto-shimming it as a bare,
     * non-capturing function pointer.  The shim path double-boxes an already-
     * fat record (slot 1 becomes the record pointer, then dispatch calls it as
     * code) and BUS/SEGV-faults.  type_eq ignores the boxed bit for two TY_FN
     * of the same signature, and boxed-TY_FN is interchangeable with
     * TY_PTR_VOID, so this is invisible to ordinary type-checking; it only
     * flips the ^fat arg classifier from shim to pass-through.  See
     * docs/archive/ascribing-fat-closure-value-to-fn-type-double-shims.md. */
    if (ascribed->kind == TY_FN &&
        (src_kind == TY_INT || src_kind == TY_PTR_VOID ||
         ascribe_type_is_opaque_handle(&inner->type) ||
         /* fn-value-fat-normalization stage 2: the inner value is ALREADY a
          * fat handle (a boxed TY_FN -- e.g. a let whose init was a
          * closure-returning call).  Retyping it to the bare non-boxed fn
          * type would send the invoke down the thin path and jump into the
          * box (the ascribe-around-let SIGSEGV in
          * fn-typed-value-return-ascribe-miscompiles).  Fat identity is
          * preserved through `::`. */
         (inner->type.kind == TY_FN && inner->type.as.fn.boxed))) {
        out->type.as.fn.boxed = true;
    }
    return out;
}

/* M2b: (default-of T) — yields a zero-valued T.  Used to fill payload slots
 * the constructor doesn't have a meaningful value for (e.g. `err-val` in
 * `(ok x)` once stdlib migrates to `make-struct` bodies).
 *
 * Surface:  (default-of T)
 *
 * Lowers to an EX_DEFAULT_OF expression carrying T as its result type.  The
 * emit layer turns this into `(T){0}` — a C99 compound literal that zero-
 * inits any scalar, pointer, or aggregate.
 *
 * T may reference type parameters in scope (resolved via the binding scope
 * the same way ascription resolves them). */
Expr *elab_default_of(Elab *e, const Form *call) {
    if (call->as.list.len != 2) {
        diag_emit(DIAG_ERROR, call->span,
                  "default-of: takes exactly one type argument: (default-of T)");
        return NULL;
    }
    Form *type_form = call->as.list.items[1];
    /* SR2b: an enclosing signature's type PARAMETER shadows a same-named
     * registered type.  `(default-of A)` inside `unwrap [A] [...] :A` means
     * the method tyvar, and resolving it through type_expr_from_form with no
     * type-param scope let a program's own `(defopaque A :int)` capture the
     * name -- the match arm then typed as that concrete ADT and failed the
     * join against the `Some` arm's genuine tyvar (positional-opaque-ok).
     * The signature's tyvars are already collected in e->sig_tyvars for
     * exactly this body-elaboration window; consult them first, the same
     * shadowing rule a declared type param has everywhere else. */
    if (type_form->tag == F_SYM) {
        const Symbol *ts = type_form->as.sym;
        for (uint8_t si = 0; si < e->n_sig_tyvars; si++) {
            if (e->sig_tyvars[si] &&
                strcmp(e->sig_tyvars[si], ts->name) == 0) {
                Type tv = type_tyvar_named(ts->name);
                tv.copy_kind = CK_COPY;
                if (e->sig_tyvar_kinds[si] != KIND_STAR)
                    tv.hkt_kind = e->sig_tyvar_kinds[si];
                Expr *tout = expr_new(e->arena, EX_DEFAULT_OF, tv, call->span);
                return tout;
            }
        }
    }
    Type *t = type_expr_from_form(e, type_form, NULL, NULL, NULL, 0);
    if (!t) return NULL;
    Expr *out = expr_new(e->arena, EX_DEFAULT_OF, *t, call->span);
    return out;
}

/* Phase HRT2/EX1c: (pack value (exists [a] T)) -- existential introduction.
 * Boxes the value as an opaque existential; result type is the TY_EXISTS type node.
 * Syntax:
 *   (pack expr (exists [a] T))
 *   (pack expr (exists [a] [(C a) ...] T))   -- EX1b/EX1c with constraints
 *
 * When the target existential carries constraints, this verifies that the
 * concrete type of `expr` has an instance for each constraint and records the
 * resolved witnesses on the EX_EXISTS_PACK node for later runtime bundling. */
Expr *elab_pack(Elab *e, const Form *call) {
    if (call->as.list.len != 3) {
        diag_emit(DIAG_ERROR, call->span,
                  "'pack' requires exactly 2 arguments: (pack value (exists [a] T))");
        return NULL;
    }
    Form *val_form  = call->as.list.items[1];
    Form *type_form = call->as.list.items[2];

    /* Parse the existential type annotation */
    Type *ex_type = type_expr_from_form(e, type_form, NULL, NULL, NULL, 0);
    if (!ex_type) return NULL;
    if (ex_type->kind != TY_EXISTS) {
        diag_emit(DIAG_ERROR, type_form->span,
                  "pack: second argument must be an existential type (exists [a] T), got %s",
                  typekind_to_string(ex_type->kind));
        return NULL;
    }

    /* Elaborate the value to pack */
    Expr *val = elab_form(e, val_form);
    if (!val) return NULL;

    /* EX1c-1: For each constraint (C v) on the existential, verify that the
     * concrete type of `val` has an instance for C.  Multi-var existentials
     * with mixed bound vars are out of scope for EX1; require all constraints
     * to reference var_idx == 0 (the first/only bound var representing the
     * packed value).  Store the resolved witnesses on the node. */
    TypeClassInstance **witnesses = NULL;
    uint8_t n_witnesses = 0;
    if (ex_type->as.forall_.n_constraints > 0) {
        uint8_t nc = ex_type->as.forall_.n_constraints;
        witnesses = (TypeClassInstance **)arena_alloc(
            e->arena, nc * sizeof(TypeClassInstance *));
        Type concrete = val->type;
        for (uint8_t i = 0; i < nc; i++) {
            TypeClass *tc = ex_type->as.forall_.constraint_classes[i];
            uint8_t    vi = ex_type->as.forall_.constraint_var_idx[i];
            if (vi != 0) {
                /* EX1: only constraints over the first bound variable are
                 * resolvable here; other vars require EX2 phantom-variable
                 * support. */
                diag_emit(DIAG_ERROR, type_form->span,
                          "pack: constraints over bound variables other than the "
                          "first are not supported in EX1");
                return NULL;
            }
            TypeClassInstance *inst = typeclass_env_lookup_instance(
                &e->typeclass_env, tc, &concrete, 1);
            if (!inst) {
                diag_emit(DIAG_ERROR, val_form->span,
                          "pack: no instance '%s' for type '%s' "
                          "(required by existential constraint)",
                          (tc && tc->name && tc->name->name) ? tc->name->name : "?",
                          type_name(concrete));
                return NULL;
            }
            witnesses[i] = inst;
        }
        n_witnesses = nc;

        /* A by-value `defstruct` payload in a constraint-carrying existential
         * is supported by heap-boxing the aggregate at the pack site and
         * reading it back through the pointer on open (EX_EXISTS_PACK /
         * EX_EXISTS_OPEN in emit_expr.c).  Witness-dispatch on a by-value-struct
         * receiver is the remaining gap -- the dispatch site assumes the int64
         * carrier ABI; see
         * docs/archive/history/constrained-exists-open-dispatch-byval-struct-receiver.md. */
    }

    /* F1-2-3: move-at-pack for RC-payload existentials.  If the packed
     * value is a bare EX_VAR over an rc-managed binding (TY_RC, TY_WEAK,
     * or a constrained existential), ownership of that rc reference now
     * lives inside the new existential record.  The smart drop hook in
     * rc_cb_free (F1-2-4) will decrement that inner reference when the
     * outer existential is freed.  Mark the source binding as moved so
     * the surrounding scope's auto-drop pass does not also decrement it
     * (which would double-free or, in zombie-with-weaks scenarios, race
     * the GC walker).
     *
     * Users who want to keep the source binding alive should clone
     * explicitly:  (pack (rc/clone src) (exists ...)). */
    if (val->kind == EX_VAR && val->as.var.binding) {
        TypeKind vk = val->type.kind;
        if (vk == TY_RC || vk == TY_WEAK || vk == TY_EXISTS) {
            (void)binding_mark_moved(val->as.var.binding, val_form->span);
        }
    }

    /* Result type is the full TY_EXISTS type node (void* at codegen) */
    Expr *out = expr_new(e->arena, EX_EXISTS_PACK, *ex_type, call->span);
    out->as.exists_pack_.value = val;
    out->as.exists_pack_.witnesses = witnesses;
    out->as.exists_pack_.n_witnesses = n_witnesses;
    return out;
}

/* Phase EX1d: scope-escape check helpers.
 *
 * After elaborating an `open` body, we verify that the existentially-bound
 * value `v` (or anything that aliases it through `let`) does not flow out of
 * the body's tail position.  This catches the canonical leak patterns:
 *
 *   (open e [a v] v)                             ; direct leak
 *   (open e [a v] (do step v))                   ; tail of `do`
 *   (open e [a v] (let [w v] w))                 ; aliased through let
 *   (open e [a v] (if cond v 0))                 ; one branch of `if`
 *
 * Indirect leaks via opaque function-return paths (e.g. `(identity v)`) are
 * deliberately not flagged here -- the EX1 spec leaves general inference of
 * skolem-tainted return types to a future pass.  When v is leaked through
 * such a path the runtime payload is still a plain int64_t, so soundness
 * relies only on the type-level check at the open boundary.
 *
 * The tainted set starts as { v } and grows as we walk through `let`s
 * whose initializer is itself tainted. */

#define EX1D_MAX_TAINTED 32

typedef struct ExistsEscapeCtx {
    Binding *tainted[EX1D_MAX_TAINTED];
    uint8_t  n_tainted;
    bool     overflowed;     /* set if we hit MAX_TAINTED; check becomes conservative */
} ExistsEscapeCtx;

static bool ex1d_is_tainted(const ExistsEscapeCtx *ctx, const Binding *b) {
    if (!b) return false;
    for (uint8_t i = 0; i < ctx->n_tainted; i++) {
        if (ctx->tainted[i] == b) return true;
    }
    return false;
}

static void ex1d_add_tainted(ExistsEscapeCtx *ctx, Binding *b) {
    if (!b || ex1d_is_tainted(ctx, b)) return;
    if (ctx->n_tainted >= EX1D_MAX_TAINTED) { ctx->overflowed = true; return; }
    ctx->tainted[ctx->n_tainted++] = b;
}

/* Returns true if the expression's tail value can be the existentially-bound
 * binding (or any binding aliased to it through `let`). */
static bool ex1d_tail_leaks(ExistsEscapeCtx *ctx, const Expr *e) {
    if (!e) return false;
    switch (e->kind) {
        case EX_VAR:
            return ex1d_is_tainted(ctx, e->as.var.binding);
        case EX_DO:
            if (e->as.do_.n == 0) return false;
            return ex1d_tail_leaks(ctx, e->as.do_.items[e->as.do_.n - 1]);
        case EX_LET: {
            /* First, propagate taint through let-binding initializers so
             * later references see the alias. */
            for (uint32_t i = 0; i < e->as.let_.n; i++) {
                LetBinding *lb = &e->as.let_.bindings[i];
                if (lb->init && ex1d_tail_leaks(ctx, lb->init)) {
                    ex1d_add_tainted(ctx, lb->binding);
                }
            }
            return ex1d_tail_leaks(ctx, e->as.let_.body);
        }
        case EX_IF:
            return ex1d_tail_leaks(ctx, e->as.if_.then_)
                || ex1d_tail_leaks(ctx, e->as.if_.else_or_null);
        case EX_EXISTS_OPEN:
            /* The value flowing out of a nested `open` is the tail of its
             * body.  Recurse so that a leak of an outer binding through an
             * inner open is still caught by the outer's check. */
            return ex1d_tail_leaks(ctx, e->as.exists_open_.body);
        default:
            return false;
    }
}

/* Direction A step 2b of
 * docs/archive/history/open-binder-skolems-not-distinguishable.md: walk a Type and
 * rename every TY_TYVAR whose name matches `from` (interned-pointer compare,
 * with strcmp fallback) to `to`.  Allocates new Type nodes for the renamed
 * branches; leaves the rest of the tree intact.  Used by elab_open to mint
 * per-open skolem identities so two nested opens produce distinguishable
 * binders and a cross-skolem call (e.g. `(sized-buf-copy! a b)` where a/b
 * came from different opens) rejects at the call-side unifier. */
static Type subst_tyvar_name(Elab *e, const Type *t,
                             const char *from, const char *to) {
    if (!t) return TYPE_UNKNOWN;
    if (t->kind == TY_TYVAR && t->as.tyvar_.name &&
        (t->as.tyvar_.name == from || strcmp(t->as.tyvar_.name, from) == 0)) {
        Type out = *t;
        out.as.tyvar_.name = to;
        return out;
    }
    if (t->kind == TY_APP) {
        Type out = *t;
        Type *new_fn  = (Type *)arena_alloc(e->arena, sizeof(Type));
        Type *new_arg = (Type *)arena_alloc(e->arena, sizeof(Type));
        *new_fn  = subst_tyvar_name(e, t->as.app.fn,  from, to);
        *new_arg = subst_tyvar_name(e, t->as.app.arg, from, to);
        out.as.app.fn  = new_fn;
        out.as.app.arg = new_arg;
        return out;
    }
    if (t->kind == TY_UNION) {
        Type out = *t;
        uint8_t n = t->as.union_.n_members;
        Type **members = (Type **)arena_alloc(e->arena, (n ? n : 1) * sizeof(Type *));
        for (uint8_t i = 0; i < n; i++) {
            members[i] = (Type *)arena_alloc(e->arena, sizeof(Type));
            *members[i] = subst_tyvar_name(e, t->as.union_.members[i], from, to);
        }
        out.as.union_.members = members;
        return out;
    }
    if (t->kind == TY_INTERSECTION) {
        Type out = *t;
        uint8_t n = t->as.intersection_.n_members;
        Type **members = (Type **)arena_alloc(e->arena, (n ? n : 1) * sizeof(Type *));
        for (uint8_t i = 0; i < n; i++) {
            members[i] = (Type *)arena_alloc(e->arena, sizeof(Type));
            *members[i] = subst_tyvar_name(e, t->as.intersection_.members[i], from, to);
        }
        out.as.intersection_.members = members;
        return out;
    }
    return *t;
}

/* EX2-2: Peel a TY_APP chain to expose the underlying concrete carrier.
 * When the existential body has the shape (Ctor i1 i2 ... in), the indices
 * i1..in are phantom at runtime; the value carries only the Ctor itself.
 * Used by elab_open to derive a usable v_type that callers expecting the
 * bare Ctor (e.g. :SizedVec) will accept. */
static Type ex2_peel_phantom_app(const Type *t) {
    while (t && t->kind == TY_APP && t->as.app.fn) {
        t = t->as.app.fn;
    }
    return t ? *t : TYPE_INT;
}

/* EX2-3: Type-level escape check for phantom skolems.
 *
 * Companion to ex1d_tail_leaks, which catches the case where the body's
 * tail value is the existentially-bound binding.  This helper catches
 * the type-level analog: when the body's inferred tail type carries a
 * partially-applied constructor whose argument is a raw, unresolved
 * type variable (TY_STRUCT with def==NULL).  In the EX2 phantom-binder
 * setting that residue could only come from the open's bound type
 * variable, since no other tyvar is in lexical scope inside the body --
 * letting it flow out of the open would be exactly the array-length
 * escape the plan warns about.
 *
 * The check only runs when the existential body itself is a TY_APP
 * (i.e. a phantom-indexed shape); plain (exists [a] a) opens are
 * unaffected.  Depth-limited to avoid pathological recursion. */
static bool ex2_3_type_has_phantom_residue(const Type *t, int depth) {
    if (!t || depth > 16) return false;
    switch (t->kind) {
        case TY_APP:
            /* A TY_APP whose argument is a raw tyvar means an index slot
             * survived elaboration -- treat as phantom-skolem escape.
             * Accepts the anonymous TY_STRUCT{def=NULL} shape and the
             * named TY_TYVAR shape introduced by Direction A step 2a. */
            if (t->as.app.arg && t->as.app.arg->kind == TY_TYVAR) {
                return true;
            }
            if (ex2_3_type_has_phantom_residue(t->as.app.fn,  depth + 1)) return true;
            if (ex2_3_type_has_phantom_residue(t->as.app.arg, depth + 1)) return true;
            return false;
        case TY_FORALL:
        case TY_EXISTS:
            return ex2_3_type_has_phantom_residue(t->as.forall_.body, depth + 1);
        default:
            return false;
    }
}

/* Phase HRT2: (open packed [a v] body) — existential elimination.
 * Unboxes the existential value, binding v to the inner value.
 * Syntax: (open packed-expr [abstract-type-var value-name] body...) */
Expr *elab_open(Elab *e, const Form *call) {
    if (call->as.list.len < 4) {
        diag_emit(DIAG_ERROR, call->span,
                  "'open' requires at least 3 arguments: (open packed [a v] body)");
        return NULL;
    }
    Form *packed_form  = call->as.list.items[1];
    Form *binders_form = call->as.list.items[2];

    /* Elaborate the packed expression */
    Expr *packed = elab_form(e, packed_form);
    if (!packed) return NULL;

    /* Accept TY_EXISTS (full info) or TY_PTR_VOID (opaque) */
    if (packed->type.kind != TY_EXISTS && packed->type.kind != TY_PTR_VOID) {
        diag_emit(DIAG_ERROR, packed_form->span,
                  "open: expected existential value (exists [a] T) or ptr<void>, got %s",
                  typekind_to_string(packed->type.kind));
        return NULL;
    }

    /* Parse [a v] binders */
    if ((binders_form->tag != F_VEC && binders_form->tag != F_LIST) ||
        binders_form->as.list.len != 2) {
        diag_emit(DIAG_ERROR, binders_form->span,
                  "open: binders must be a 2-element vector [abstract-type-var value-name]");
        return NULL;
    }
    Form *abstract_form = binders_form->as.list.items[0];
    Form *val_name_form = binders_form->as.list.items[1];
    if (abstract_form->tag != F_SYM || val_name_form->tag != F_SYM) {
        diag_emit(DIAG_ERROR, binders_form->span,
                  "open: binders must be symbols");
        return NULL;
    }
    const Symbol *abs_sym = abstract_form->as.sym;
    const Symbol *val_sym = val_name_form->as.sym;

    /* Determine type of v from the existential body type.
     * Type variables (TY_STRUCT with def=NULL) resolve to TY_INT (int64_t at runtime).
     * EX2-2: Bodies of the form (Ctor i1 .. in) -- where i1..in are phantom
     * type indices bound by the existential -- peel through the TY_APP chain
     * so that v gets the underlying carrier (e.g. the ADT for SizedVec).  The
     * indices have no runtime representation, so this exposes the type a
     * caller expecting `:SizedVec` would accept while still keeping the
     * existential variable abstract at the type level. */
    Type v_type = TYPE_INT;  /* default: int64_t */
    if (packed->type.kind == TY_EXISTS) {
        const Type *T = packed->type.as.forall_.body;
        if (T) {
            if (T->kind == TY_TYVAR) {
                v_type = TYPE_INT;  /* type variable → int64_t */
            } else if (T->kind == TY_APP) {
                /* Peel iff the head is an ADT (SizedVec / defgadt path): call
                 * sites accept bare nominal there, and 14+ stdlib entry
                 * points are declared with bare param types.  For a
                 * defopaque head (TY_STRUCT with a real def), keep the
                 * applied form so signatures declared as `(Op n)` can
                 * unify their `n` against the open's bound binder -- this
                 * is the SizedBuf path and the eventual sized-world `(World n)`
                 * path.  See docs/archive/history/pack-open-phantom-opaque-body-type-collapses.md. */
                Type head = ex2_peel_phantom_app(T);
                /* structdef-retirement slice 5: a `defopaque` head is now an
                 * opaque TY_ADT (it was a real-def TY_STRUCT before).  Keep the
                 * applied form `(Op n)` for it -- exactly as for the struct case --
                 * so the bound index `n` survives into v_type, gets a fresh
                 * per-open skolem name below, and a cross-skolem `(SizedBuf na)`
                 * vs `(SizedBuf nb)` mismatch is rejected at the call site.  A
                 * NON-opaque ADT head (SizedVec / defgadt) still peels to bare
                 * nominal, matching the 14+ stdlib bare-param entry points. */
                bool head_is_real_struct =
                    (head.kind == TY_ADT && head.as.adt_.def != NULL &&
                     head.as.adt_.def->is_opaque);
                if (head_is_real_struct) {
                    v_type = *T;
                } else {
                    v_type = head;
                    /* If the peeled head is itself a free type variable, fall
                     * back to int64_t (consistent with the bare-tyvar case).
                     * Accepts anonymous TY_STRUCT{def=NULL} and named TY_TYVAR. */
                    if (v_type.kind == TY_TYVAR) {
                        v_type = TYPE_INT;
                    }
                }
            } else {
                v_type = *T;  /* use body type directly (by value) */
            }
        }
    }

    /* EX1d: mint a fresh skolem id and push the depth counter so nested
     * `open` forms cannot confuse each other's escape checks. */
    uint32_t my_skolem_id = e->open_skolem_next++;
    e->open_skolem_depth++;

    /* Direction A step 2b: substitute each existential binder name in v_type
     * with a fresh skolem name unique to this open.  Without this, two nested
     * opens of the same `(exists [n] (Op n))` would both project bodies
     * carrying TY_TYVAR(name="n"), and the call-side unifier would treat
     * them as identical (TUR-E0260 cross-skolem mismatches would silently
     * type-check). */
    if (packed->type.kind == TY_EXISTS
            && packed->type.as.forall_.var_names
            && packed->type.as.forall_.n_vars > 0) {
        char namebuf[40];
        for (uint8_t bi = 0; bi < packed->type.as.forall_.n_vars; bi++) {
            const char *src = packed->type.as.forall_.var_names[bi];
            if (!src) continue;
            int n_chars = snprintf(namebuf, sizeof(namebuf),
                                   "__open_skolem_%u_%u", my_skolem_id, bi);
            if (n_chars < 0 || (size_t)n_chars >= sizeof(namebuf)) continue;
            size_t len = (size_t)n_chars;
            char *fresh = (char *)arena_alloc(e->arena, len + 1);
            memcpy(fresh, namebuf, len + 1);
            v_type = subst_tyvar_name(e, &v_type, src, fresh);
        }
    }

    /* Create binding for v in the open body */
    Binding *v_binding = binding_new(e, val_sym, v_type, false, false, val_name_form->span);

    /* Existential-open witness dispatch: when the scrutinee is a
     * constraint-carrying existential, record its TY_EXISTS type on the `v`
     * binding.  A `(.m v ...)` method call in the body whose method belongs to
     * one of the packed constraint classes then resolves through the record's
     * runtime witness vtable (elab_method_call) instead of failing as
     * ambiguous over the erased int64 carrier. */
    if (packed->type.kind == TY_EXISTS
            && packed->type.as.forall_.n_constraints > 0) {
        Type *ex_copy = (Type *)arena_alloc(e->arena, sizeof(Type));
        *ex_copy = packed->type;
        v_binding->exists_open_type = ex_copy;
    }

    /* Push a scope with v */
    Scope inner;
    scope_init(&inner, e->scope);
    scope_add(&inner, v_binding);
    e->scope = &inner;

    /* Elaborate body (one or more forms) */
    uint32_t n_body = call->as.list.len - 3;
    Expr *body = NULL;
    bool body_ok = true;
    if (n_body == 1) {
        body = elab_form(e, call->as.list.items[3]);
        if (!body) body_ok = false;
    } else {
        Expr **items = (Expr **)arena_alloc(e->arena, n_body * sizeof(Expr *));
        for (uint32_t i = 0; i < n_body && body_ok; i++) {
            items[i] = elab_form(e, call->as.list.items[3 + i]);
            if (!items[i]) { body_ok = false; break; }
        }
        if (body_ok) {
            body = expr_new(e->arena, EX_DO, items[n_body - 1]->type, call->span);
            body->as.do_.items = items;
            body->as.do_.n = n_body;
        }
    }

    /* Restore scope and depth */
    e->scope = inner.parent;
    scope_free(&inner);
    e->open_skolem_depth--;

    if (!body_ok || !body) return NULL;

    /* EX1d: scope-escape check.  Verify that the body's tail position does
     * not leak the existentially-bound value out of the open scope. */
    ExistsEscapeCtx ctx = (ExistsEscapeCtx){0};
    ex1d_add_tainted(&ctx, v_binding);
    if (ex1d_tail_leaks(&ctx, body)) {
        diag_emit(DIAG_ERROR, call->span,
                  "open: existential type variable '%s' escapes its scope "
                  "(the body returns the bound value '%s' or an alias)",
                  abs_sym && abs_sym->name ? abs_sym->name : "a",
                  val_sym && val_sym->name ? val_sym->name : "v");
        return NULL;
    }

    /* EX2-3: type-level escape check for phantom skolems.  Only meaningful
     * when the existential body is itself a TY_APP (phantom-indexed
     * shape); other shapes can't leak a phantom binder by construction. */
    if (packed->type.kind == TY_EXISTS
            && packed->type.as.forall_.body
            && packed->type.as.forall_.body->kind == TY_APP
            && ex2_3_type_has_phantom_residue(&body->type, 0)) {
        diag_emit(DIAG_ERROR, call->span,
                  "open: existential phantom type variable '%s' escapes "
                  "through the tail expression's type",
                  abs_sym && abs_sym->name ? abs_sym->name : "a");
        return NULL;
    }
    (void)my_skolem_id; /* reserved for EX1e runtime dispatch */

    Expr *out = expr_new(e->arena, EX_EXISTS_OPEN, body->type, call->span);
    out->as.exists_open_.packed      = packed;
    out->as.exists_open_.var_binding = v_binding;
    out->as.exists_open_.body        = body;
    return out;
}
