/* elab_structs.c -- struct/ADT/GADT definitions, pattern matching, and borrow traits. */
#include "elab_internal.h"
#include "lang_dialects.h"  /* saffron-lang-plan S5: lang_span_is_dynamic */
#include <assert.h>   /* structdef-retirement slice 5 DS-B: zero-producer guard */

/* ---- file-local helper forward declarations ---- */
static void parse_struct_field_type(const char *tname, uint32_t tlen,
    TypeKind *out_kind, TypeKind *out_inner);
static bool typekind_is_copy_for_struct(TypeKind k);
/* Exposed (declared in elab_internal.h) for CTOR-V0 struct-name call routing. */
static bool elab_is_forward_type(Elab *e, const Symbol *sym);
static Type gadt_resolve_type_from_form(Elab *e, const AdtDef *gadt, const Form *f,
    const SkolemEnv *senv);
static void gadt_build_skolem_env(Elab *e, SkolemEnv *out, const AdtDef *def,
    const CtorDef *ctor);
static TypeKind gadt_field_typekind_from_form(const Form *f);
static Type *adt_field_type_from_form(Arena *arena, const Form *ft_form,
    const char **type_params, uint8_t n_type_params);
static void infer_type_param_kinds(AdtDef *def);

/* CONV-S6 (diagnostic wording pass): product-shaped construction diagnostics
 * (missing / duplicate / unknown field, mixed positional+keyword args, and the
 * `with` copy/field errors) all fire against a single-variant record ADT.  That
 * ADT may have been written as a `defdata`/`defgadt` variant OR lowered from a
 * `defstruct`.  The user should see the surface they actually wrote, so these
 * helpers classify the surface and format the shared noun phrase:
 *
 *   from a `defstruct`      -> "struct 'Person'"
 *   any other record variant -> "variant 'Circle' of type 'Shape'"
 *
 * A single-variant from-struct lowering keeps its name in both slots
 * (def->name == ctor->name), so the "variant X of type X" form would be
 * redundant -- hence the dedicated "struct" wording. */
bool conv_surface_is_struct(const AdtDef *def) {
    return def && def->from_struct_lowering && def->n_ctors == 1;
}

const char *conv_surface_phrase(const AdtDef *def, const CtorDef *ctor,
                                char *buf, size_t buflen) {
    if (conv_surface_is_struct(def)) {
        snprintf(buf, buflen, "struct '%s'", def->name);
    } else {
        snprintf(buf, buflen, "variant '%s' of type '%s'",
                 ctor ? ctor->name : (def ? def->name : "?"),
                 def ? def->name : "?");
    }
    return buf;
}

/* Phase 11: defstruct - define a struct type
 * Syntax: (defstruct Name [:copy] [field1 :type1 field2 :type2 ...])
 * 
 * The :copy annotation indicates the struct is bitwise-copyable (all fields must be Copy).
 * Without :copy, the struct is move-only (default).
 * 
 * Returns an EX_DEF expression that defines the struct type at file scope.
 */

/* Helper: parse a field type keyword like "int", "rc<int>", "ref<bool>", "ptr<void>" */
static void parse_struct_field_type(const char *tname, uint32_t tlen,
                                     TypeKind *out_kind, TypeKind *out_inner) {
    *out_kind = TY_UNKNOWN;
    *out_inner = TY_UNKNOWN;

    if (tlen == 3  && memcmp(tname, "int",   3) == 0) { *out_kind = TY_INT;      return; }
    if (tlen == 5  && memcmp(tname, "int64", 5) == 0) { *out_kind = TY_INT;      return; }
    if (tlen == 4  && memcmp(tname, "bool",  4) == 0) { *out_kind = TY_BOOL;     return; }
    if (tlen == 5  && memcmp(tname, "float", 5) == 0) { *out_kind = TY_FLOAT;    return; }
    if (tlen == 7  && memcmp(tname, "float64", 7) == 0) { *out_kind = TY_FLOAT;  return; }
    if (tlen == 4  && memcmp(tname, "int8",  4) == 0) { *out_kind = TY_INT8;     return; }
    if (tlen == 5  && memcmp(tname, "int16", 5) == 0) { *out_kind = TY_INT16;    return; }
    if (tlen == 5  && memcmp(tname, "int32", 5) == 0) { *out_kind = TY_INT32;    return; }
    if (tlen == 5  && memcmp(tname, "uint8", 5) == 0) { *out_kind = TY_UINT8;    return; }
    if (tlen == 6  && memcmp(tname, "uint16", 6) == 0) { *out_kind = TY_UINT16;  return; }
    if (tlen == 6  && memcmp(tname, "uint32", 6) == 0) { *out_kind = TY_UINT32;  return; }
    if (tlen == 6  && memcmp(tname, "uint64", 6) == 0) { *out_kind = TY_UINT64;  return; }
    if (tlen == 7  && memcmp(tname, "float32", 7) == 0) { *out_kind = TY_FLOAT32; return; }
    if (tlen == 4  && memcmp(tname, "cstr",  4) == 0) { *out_kind = TY_CSTR;     return; }
    if (tlen == 3  && memcmp(tname, "nil",   3) == 0) { *out_kind = TY_NIL;      return; }
    if (tlen == 4  && memcmp(tname, "void",  4) == 0) { *out_kind = TY_NIL;      return; }
    if (tlen == 9  && memcmp(tname, "ptr<void>", 9) == 0) { *out_kind = TY_PTR_VOID; return; }
    /* Phase 16 v2: :fn field type — function pointer (may carry #{...} effect-row annotation) */
    if (tlen == 2  && memcmp(tname, "fn",    2) == 0) { *out_kind = TY_FN;       return; }
    /* saffron-lang-plan S4 (PROBE): `:any` field, so a container can hold
     * values of different types. */
    if (tlen == 3  && memcmp(tname, "any",   3) == 0) { *out_kind = TY_ANY;      return; }
    /* saffron-dynamic-surface-pass (low): `Sym` had no row here, so
     * `(defstruct S [k : Sym])` -- and the `(defdata D (D Sym))` it lowers to
     * -- was "defdata: field has unrecognized type :Sym", in typed Turmeric
     * too.  A Sym is an INTERNED pointer, so it is a pointer-sized scalar
     * carrier like the rows above it, not a new representation. */
    if (tlen == 3  && memcmp(tname, "Sym",   3) == 0) { *out_kind = TY_SYM;      return; }

    /* Compound types: rc<T>, ref<T>, lref<T>, weak<T> */
    /* Parse the prefix and inner type */
    const char *prefix_rc   = "rc<";
    const char *prefix_ref  = "ref<";
    const char *prefix_lref = "lref<";
    const char *prefix_weak = "weak<";

    TypeKind prefix_kind = TY_UNKNOWN;
    uint32_t prefix_len = 0;
    if (tlen > 3 && memcmp(tname, prefix_rc, 3) == 0)   { prefix_kind = TY_RC;   prefix_len = 3; }
    if (tlen > 4 && memcmp(tname, prefix_ref, 4) == 0)  { prefix_kind = TY_REF;  prefix_len = 4; }
    if (tlen > 5 && memcmp(tname, prefix_lref, 5) == 0) { prefix_kind = TY_LREF; prefix_len = 5; }
    if (tlen > 5 && memcmp(tname, prefix_weak, 5) == 0) { prefix_kind = TY_WEAK; prefix_len = 5; }

    if (prefix_kind != TY_UNKNOWN && prefix_len > 0 && tname[tlen - 1] == '>') {
        const char *inner_name = tname + prefix_len;
        uint32_t inner_len = tlen - prefix_len - 1; /* strip trailing '>' */
        TypeKind inner_kind = TY_UNKNOWN, dummy = TY_UNKNOWN;
        parse_struct_field_type(inner_name, inner_len, &inner_kind, &dummy);
        *out_kind = prefix_kind;
        *out_inner = inner_kind;
        return;
    }

    /* Unknown type - leave as TY_UNKNOWN */
}

/* Helper: check if a TypeKind is considered copy */
static bool typekind_is_copy_for_struct(TypeKind k) {
    switch (k) {
        case TY_INT: case TY_BOOL: case TY_FLOAT: case TY_CSTR:
        case TY_PTR_VOID: case TY_NIL:
        case TY_INT8: case TY_INT16: case TY_INT32:
        case TY_UINT8: case TY_UINT16: case TY_UINT32: case TY_UINT64:
        case TY_FLOAT32:
        /* Phase 16 v2: :fn fields are stored as int64_t at the C level, so they are
         * trivially copyable (function pointer stored as an integer value). */
        case TY_FN:
            return true;
        default:
            return false;
    }
}

/* Phase RF0: Look up a binding for a user-defined struct or ADT type by name.
 * Unlike scope_lookup (which returns the most recent binding), this searches all
 * bindings for one with kind TY_STRUCT or TY_ADT.  Needed because a constructor
 * with the same name as its type (e.g. (defdata Expr (Expr :ExprNode))) shadows
 * the type binding with a TY_FN constructor binding. */
Binding *scope_lookup_type_def(Scope *s, const Symbol *name) {
    for (Scope *cur = s; cur; cur = cur->parent) {
        if (cur->idx) {
            /* The global scope holds thousands of bindings and every call
             * head is looked up here: walk only this name's bindings,
             * newest first, as the scan below would meet them. */
            for (uint32_t k = scope_idx_newest(cur, name); k; k = cur->prev[k - 1]) {
                Binding *b = cur->bindings[k - 1];
                if (b->type.kind == TY_STRUCT || b->type.kind == TY_ADT) return b;
            }
            continue;
        }
        for (uint32_t i = cur->n; i > 0; i--) {
            Binding *b = cur->bindings[i - 1];
            if (b->name == name &&
                (b->type.kind == TY_STRUCT || b->type.kind == TY_ADT)) {
                return b;
            }
        }
    }
    return NULL;
}

/* CONV-S1 (slice 5): the rc<Name> / weak<Name> inner-def resolver for a
 * record-variant field.  When a record-variant field is annotated `rc<Name>` and
 * `Name` resolves to an in-scope struct or single-variant record ADT, build the
 * inner-carrying rc Type (`type_rc_struct` / `type_rc_adt`) so field access
 * through the rc receiver can auto-deref to the named field -- exactly the
 * surface a `defstruct` rc<Struct> field already exposes (DS3 / slice 2), now
 * reached by the lowered struct path.  Returns NULL (the field stays a bare
 * carrier) when `tname` is not an `rc<...>` / `weak<...>` over a known
 * aggregate, so a scalar inner (`rc<int>`) or an unknown name is unaffected.
 *
 * stdlib-weak-ref-audit WR1: `weak<Name>` resolves the same way.  It used not
 * to, and the omission bit exactly the shape weak<T> exists for -- the
 * back-edge of a parent/child graph.  `(weak parent)` over an `rc<Node>` yields
 * `weak<ADT>`, while an unresolved `[parent : weak<Node>]` field stayed
 * `weak<?>`, so `(set! (.parent child) (weak parent))` failed to type-check with
 * "value type weak<<adt>> does not match field type weak<?>" and the canonical
 * cycle break was not expressible at all. */
static Type *adt_rc_inner_full_type(Elab *e, const char *tname, uint32_t tlen) {
    TypeKind family;
    uint32_t prefix_len;
    if (tlen > 4 && memcmp(tname, "rc<", 3) == 0)          { family = TY_RC;   prefix_len = 3; }
    else if (tlen > 6 && memcmp(tname, "weak<", 5) == 0)   { family = TY_WEAK; prefix_len = 5; }
    else return NULL;
    if (tname[tlen - 1] != '>') return NULL;
    const char *inner_name = tname + prefix_len;
    uint32_t inner_len = tlen - prefix_len - 1;  /* strip the prefix and '>' */
    const Symbol *sym = symtab_intern(e->st, strslice(inner_name, inner_len));
    Binding *tb = scope_lookup_type_def(e->scope, sym);
    if (!tb) return NULL;
    Type *t = (Type *)arena_alloc(e->arena, sizeof(Type));
    if (tb->type.kind == TY_ADT) {
        *t = (family == TY_WEAK) ? type_weak_adt(tb->type.as.adt_.def)
                                 : type_rc_adt(tb->type.as.adt_.def);
        return t;
    }
    return NULL;
}



static void struct_field_storage_from_type(const Type *t, TypeKind *out_kind, TypeKind *out_inner) {
    *out_kind = TY_UNKNOWN;
    *out_inner = TY_UNKNOWN;
    if (!t) return;
    switch (t->kind) {
        case TY_APP:
            /* structdef-retirement DS-D: a parametric struct application no longer
             * has a concrete by-value struct layout (a parametric aggregate is a
             * record ADT); every TY_APP field rides the int64 carrier here. */
            *out_kind = TY_INT;
            return;
        case TY_TYVAR:
        case TY_EXISTS:
        case TY_FORALL:
        case TY_STRUCT:
        case TY_ADT:
            *out_kind = TY_INT;
            return;
        case TY_HANDLER:
            /* EF-2: a `(handler E V R)` field is a runtime handler object.  Keep
             * TY_HANDLER as the storage kind (do not remap to TY_INT): the record
             * ADT's C emitter maps it to the int64 handler-pointer carrier via
             * adt_field_scalar_c_type's default, exactly as a `fn` field's TY_FN
             * kind does, while the constructor's argument type-check reads this
             * kind and so accepts a handler value (remapping to TY_INT would make
             * the ctor demand an `int` and reject the handler).  Made explicit
             * rather than relying on `default:` so the intent is documented. */
            *out_kind = TY_HANDLER;
            return;
        case TY_SESSION:
        case TY_ROLE:
            /* EF-4: a `(Session P)` / `(project G R)` / `(Role G R)` field is a
             * runtime channel/role endpoint (a `TurChannel *` / role pointer at
             * the C level).  Keep the TY_SESSION / TY_ROLE kind (do not remap to
             * TY_INT), exactly as the TY_HANDLER case above: the record ADT's C
             * emitter maps both to the int64 pointer carrier via
             * adt_field_scalar_c_type's default, while the constructor's argument
             * type-check reads this kind and so accepts a session/role value
             * (remapping to TY_INT would make the ctor demand an `int` and reject
             * the endpoint).  Both are CK_LINEAR; the exactly-once discipline
             * rides the field type's copy_kind, like the borrow family. */
            *out_kind = t->kind;
            return;
        case TY_REF:
        case TY_LREF:
            *out_kind = t->kind;
            *out_inner = t->as.ref.inner;
            return;
        case TY_RC:
        case TY_WEAK:
            *out_kind = t->kind;
            *out_inner = t->as.rc.inner;
            return;
        default:
            *out_kind = t->kind;
            return;
    }
}

static Type *struct_field_type_from_form(Elab *e, const Form *form,
                                         const Symbol **type_params,
                                         Kind *type_param_kinds,
                                         uint8_t n_type_params) {
    if (!form) return NULL;
    if (form->tag == F_TYPE_ANN && form->as.list.len > 0) {
        return struct_field_type_from_form(e, form->as.list.items[0],
                                           type_params, type_param_kinds, n_type_params);
    }
    if (form->tag == F_SYM) {
        for (uint8_t i = 0; i < n_type_params; i++) {
            if (type_params[i] == form->as.sym) {
                Type *t = (Type *)arena_alloc(e->arena, sizeof(Type));
                *t = type_tyvar_named(form->as.sym->name);
                t->hkt_kind = type_param_kinds ? type_param_kinds[i] : KIND_STAR;
                return t;
            }
        }
        return type_expr_from_form(e, form, NULL, type_params, type_param_kinds, n_type_params);
    }
    if (form->tag == F_LIST && form->as.list.len >= 1 &&
        form->as.list.items[0]->tag == F_SYM) {
        const Symbol *head = form->as.list.items[0]->as.sym;
        /* structdef-retirement slice 5 DS-A4 / EF-4: reject the built-in compound
         * field forms that have NO usable field representation.  The session
         * PROTOCOL DESCRIPTORS (`Send`/`Recv`/`Choose`/`Branch`/`Rec`/`Timeout`)
         * are type-level constructs -- a value is always a `Session[P]`, never a
         * bare `(Send ...)`; a descriptor is only meaningful NESTED inside
         * `(Session ...)`, where type_expr_from_form parses it recursively.
         * `Global` (`(Global Name)`) is a compile-time-only choreography type
         * (types.h: "no runtime representation").  `forall` (EF-3) is SHELVED
         * (2026-07-02): its storage is free -- it rides the exists int64 carrier
         * and struct_field_storage_from_type maps TY_FORALL -> TY_INT -- but a
         * poly value read from a field has no consumption path under erasure-based
         * HRT (a rank-N argument must be a named function, not a field-read
         * expression), so a lowered forall field would be write-only; kept
         * rejected until HRT can instantiate a poly value from a field.  A
         * defstruct with such a field lowers to a record ADT like every other
         * and errors CLEANLY here.  `exists`, `fn`/`c-fn`,
         * `arrow` (`->`), `handler`, the borrow family, and the value-carrying
         * session heads `Session`/`project`/`Role` (EF-4) DO lower and are handled
         * below.  Tracked in
         * docs/archive/history/structdef-exotic-field-forms-plan.md. */
        if (head == e->sym_forall || head == e->sym_forall_u ||
            head == e->sym_session_Send ||
            head == e->sym_session_Recv || head == e->sym_session_Choose ||
            head == e->sym_session_Branch || head == e->sym_session_Rec ||
            head == e->sym_session_Timeout ||
            head == e->sym_global_type) {
            diag_emit(DIAG_ERROR, form->span,
                      "type form '(%.*s ...)' is not supported as a struct/ADT "
                      "field; this built-in compound type form has no runtime "
                      "value to store in a field (a session protocol descriptor "
                      "is only meaningful nested inside `(Session ...)`; `Global` "
                      "is a compile-time-only choreography type; `forall` is the "
                      "EF-3 gate).  See "
                      "docs/archive/history/structdef-exotic-field-forms-plan.md",
                      (int)head->len, head->name);
            return NULL;
        }
        if (head == e->sym_exists || head == e->sym_exists_u) {
            return type_expr_from_form(e, form, NULL, type_params, type_param_kinds, n_type_params);
        }
        /* parametric-defstruct-fn-field-gaps (Gap 1): a fn-typed field
         * `(fn [A...] R)` / `(c-fn [A...] R)` is a function type, not a
         * type-application.  Without this dispatch the generic type-app loop
         * below recurses into the `[A...]` param vector and mis-parses it as a
         * TupleN literal (a 1-arg fn hits the "tuple type must have 2 to 8
         * element types" error).  type_expr_from_form has the real fn-type
         * parser; route there directly.  `arrow`/`->` (`(-> A B)`) is an
         * alternate spelling of the fn type and shares that parser (it lowers
         * to TY_FN), so it routes through the same dispatch (EF-1). */
        if (head == e->sym_fn || head == e->sym_c_fn || head == e->sym_arrow) {
            return type_expr_from_form(e, form, NULL, type_params, type_param_kinds, n_type_params);
        }
        /* EF-2: a `(handler E V R)` field is a runtime handler object -- its own
         * TypeKind (TY_HANDLER), not a type application.  type_expr_from_form has
         * the handler-type parser (ET3-A); route there so the field resolves to
         * TY_HANDLER, which struct_field_storage_from_type maps to the int64
         * carrier slot.  A handler is a CK_COPY value, so no linear/affine or
         * `:copy` diagnostic applies (unlike the borrow family). */
        if (head == e->sym_handler_type) {
            return type_expr_from_form(e, form, NULL, type_params, type_param_kinds, n_type_params);
        }
        /* EF-4: the value-carrying session heads -- `(Session P)` (a linear
         * channel endpoint), `(project G R)` (which resolves to `Session[P]` for
         * the projected role), and `(Role G R)` (a linear endpoint of a global
         * protocol).  Each is its own TypeKind (TY_SESSION / TY_ROLE), not a type
         * application; type_expr_from_form has the session-type parser (SS0b/SS5/
         * SS6).  Route there so the field resolves to TY_SESSION / TY_ROLE, which
         * struct_field_storage_from_type keeps as the storage kind (the C emitter
         * maps both to the int64 pointer carrier, like a handler field).  Both are
         * CK_LINEAR, so the ADT `:copy` check reproduces TUR-E0102 for such a
         * field in a `:copy` struct -- exactly as the borrow family (lref) does.
         * The protocol DESCRIPTORS (Send/Recv/...) and `Global` are rejected above
         * (no runtime value). */
        if (head == e->sym_session_type || head == e->sym_project_type ||
            head == e->sym_role_type) {
            return type_expr_from_form(e, form, NULL, type_params, type_param_kinds, n_type_params);
        }
        /* structdef-retirement slice 5 DS-A3: a list-form built-in compound type
         * -- `(lref T)`, `(borrow-mut T)` -- is its own TypeKind (TY_LREF /
         * TY_REF_MUT), not a type application.  Route it to the real type
         * elaborator so a lowered `defstruct` field resolves to the correct kind
         * instead of the generic type-app loop below mis-parsing `(lref int)` as
         * apply(lref, int) (TUR-E0012).  `(& T)` immutable borrow already routes
         * via the has_amp path below.  This lets the borrow-family field forms
         * lower to the record-ADT path, where the ADT's own :copy/linear check
         * reproduces the struct-path diagnostic. */
        if (head == e->sym_lref || head == e->sym_borrow_mut) {
            return type_expr_from_form(e, form, NULL, type_params, type_param_kinds, n_type_params);
        }
        /* assoc-types: `(Storage Pos)` where the head is an ASSOCIATED TYPE
         * member of some registered typeclass is a type-level projection, not
         * a type application.  type_expr_from_form has the resolver (it
         * matches the instance and substitutes the bound type); the generic
         * app loop below would instead apply the kind-* head and emit a
         * spurious TUR-E0012 -- which is exactly what broke `defworld`'s
         * `(Storage ~Comp)` fields when defstruct lowered onto this path.
         * Guarded on the head actually being a declared associated type, so
         * an ordinary constructor application is never intercepted. */
        {
            uint8_t assoc_idx;
            if (typeclass_env_find_assoc_type(&e->typeclass_env, head, &assoc_idx))
                return type_expr_from_form(e, form, NULL, type_params,
                                           type_param_kinds, n_type_params);
        }
        bool has_pipe = false, has_amp = false;
        for (uint32_t i = 0; i < form->as.list.len; i++) {
            Form *item = form->as.list.items[i];
            if (item->tag != F_SYM) continue;
            if (item->as.sym == e->sym_pipe) has_pipe = true;
            if (item->as.sym == e->sym_ampersand) has_amp = true;
        }
        if (has_pipe || has_amp) {
            return type_expr_from_form(e, form, NULL, type_params, type_param_kinds, n_type_params);
        }
        Type *cur = struct_field_type_from_form(e, form->as.list.items[0],
                                                type_params, type_param_kinds, n_type_params);
        if (!cur) return NULL;
        for (uint32_t i = 1; i < form->as.list.len; i++) {
            Form *arg_form = form->as.list.items[i];
            Type *arg = NULL;
            /* SZ8 non-GADT: a Size GADT literal (Static N)/(Add s s)/(Mul s s)
             * in a type-app argument slot lowers to a placeholder TY_INT --
             * the size information is recovered from the retained field Form
             * by size_term_from_form.  Mirrors type_expr_from_form's app loop;
             * without it a sized phantom index like `(SizedDense (Static 8)
             * Pos)` in a lowered defstruct/defdata field recursed into
             * `(Static 8)` as a type application and died on the integer
             * literal ("unsupported type expression form"). */
            if (arg_form->tag == F_LIST && arg_form->as.list.len >= 1 &&
                    arg_form->as.list.items[0]->tag == F_SYM) {
                const char *op = arg_form->as.list.items[0]->as.sym->name;
                bool is_size_op = (strcmp(op, "Static") == 0 ||
                                   strcmp(op, "Add")    == 0 ||
                                   strcmp(op, "Mul")    == 0);
                if (is_size_op &&
                        size_term_from_form(e->arena, arg_form, NULL, NULL) != NULL) {
                    Type *ph = (Type *)arena_alloc(e->arena, sizeof(Type));
                    *ph = type_from_kind(TY_INT);
                    arg = ph;
                }
            }
            if (!arg)
                arg = struct_field_type_from_form(e, arg_form,
                                                  type_params, type_param_kinds, n_type_params);
            if (!arg) return NULL;
            Type *next = (Type *)arena_alloc(e->arena, sizeof(Type));
            *next = type_app(e->arena, *cur, *arg, form->span);
            cur = next;
        }
        return cur;
    }
    return type_expr_from_form(e, form, NULL, type_params, type_param_kinds, n_type_params);
}

/* TP6: Unpack a TY_APP chain on an ADT type to recover concrete type arguments.
 * Analogous to elab_struct_type_extract_args but for AdtDef instead of StructDef. */
bool elab_adt_type_extract_args(const Type *t, const AdtDef *def, Type *out_args) {
    if (!t || !def || def->n_type_params == 0 || !out_args) return false;
    const Type *cur = t;
    uint8_t n_raw = 0;
    Type *raw = (Type *)malloc(def->n_type_params * sizeof(Type));
    if (!raw) return false;
    while (cur && cur->kind == TY_APP && n_raw < def->n_type_params) {
        if (!cur->as.app.arg) break;
        raw[n_raw++] = *cur->as.app.arg;
        cur = cur->as.app.fn;
    }
    bool ok = (cur && cur->kind == TY_ADT && cur->as.adt_.def == def &&
               n_raw == def->n_type_params);
    if (ok) {
        for (uint8_t i = 0; i < n_raw; i++) out_args[i] = raw[n_raw - 1 - i];
    }
    free(raw);
    return ok;
}

/* TP6: Instantiate a field type by substituting TY_TYVAR names with concrete type args.
 * Analogous to struct_field_instantiate_type but uses AdtDef.type_params for name lookup. */
Type adt_field_instantiate_type(Elab *e, const AdtDef *def, const Type *t,
                                const Type *type_args) {
    if (!t) return TYPE_UNKNOWN;
    switch (t->kind) {
        case TY_TYVAR: {
            if (!t->as.tyvar_.name) return *t;
            for (uint8_t i = 0; i < def->n_type_params; i++) {
                if (def->type_params[i] &&
                    strcmp(def->type_params[i], t->as.tyvar_.name) == 0) {
                    return type_args[i];
                }
            }
            return *t;
        }
        case TY_APP: {
            Type fn  = adt_field_instantiate_type(e, def, t->as.app.fn,  type_args);
            Type arg = adt_field_instantiate_type(e, def, t->as.app.arg, type_args);
            return type_app(e->arena, fn, arg, (Span){0});
        }
        case TY_FN: {
            /* lowered-adt-ctor-skips-fn-field-type-param-inference: a record-ADT
             * type parameter may appear only inside a fn-typed field
             * (`(get (fn [S] A))`).  Substitute it through the fn's arg/result
             * slots so `(.get l p)` over `l : (Lens Person cstr)` reads the field
             * as `(fn [Person] cstr)` instead of leaving `S`/`A` as bare tyvars
             * (which lower to the int64 carrier and fail untyped overload
             * resolution).  Mirrors struct_field_instantiate_type's TY_FN arm. */
            Type out = *t;
            uint32_t arity = t->as.fn.arity;
            /* out shares t's out-of-line arg arrays; give it a private arg_kinds
             * (and a copied arg_flags) before overwriting per-arg kinds below. */
            if (arity) {
                uint8_t *ok = tur_fn_args_alloc(arity), *of = tur_fn_args_alloc(arity);
                for (uint32_t i = 0; i < arity; i++) {
                    ok[i] = t->as.fn.arg_kinds[i];
                    of[i] = t->as.fn.arg_flags[i];
                }
                out.as.fn.arg_kinds = ok;
                out.as.fn.arg_flags = of;
            }
            struct Type **new_args = arity
                ? (struct Type **)arena_alloc(e->arena, arity * sizeof(struct Type *))
                : NULL;
            for (uint32_t i = 0; i < arity; i++) {
                Type slot = (t->as.fn.arg_full_types && t->as.fn.arg_full_types[i])
                    ? *t->as.fn.arg_full_types[i]
                    : type_from_kind(t->as.fn.arg_kinds[i]);
                Type inst = adt_field_instantiate_type(e, def, &slot, type_args);
                out.as.fn.arg_kinds[i] = inst.kind;
                struct Type *boxed = (struct Type *)arena_alloc(e->arena, sizeof(Type));
                *boxed = inst;
                new_args[i] = boxed;
            }
            out.as.fn.arg_full_types = new_args;
            {
                Type rslot = t->as.fn.result_full_type
                    ? *t->as.fn.result_full_type
                    : type_from_kind(t->as.fn.result_kind);
                Type rinst = adt_field_instantiate_type(e, def, &rslot, type_args);
                out.as.fn.result_kind = rinst.kind;
                struct Type *rboxed = (struct Type *)arena_alloc(e->arena, sizeof(Type));
                *rboxed = rinst;
                out.as.fn.result_full_type = rboxed;
            }
            return out;
        }
        default:
            return *t;
    }
}

/* lowered-adt-ctor-skips-fn-field-type-param-inference: the record-ADT analogue
 * of struct_field_collect_type_args.  Grounds a record-ADT ctor's type parameters
 * (named in `tps`) by unifying each declared field full_type against the supplied
 * value's actual type, descending into a FN-typed field so a parameter that
 * appears only inside a fn field (`(get (fn [S] A))`) still infers.  Inference
 * only: a concrete (non-tyvar, non-app, non-fn) field never fails here -- the
 * ctor call's own type check reports a genuine mismatch.  Returns false only on a
 * structural shape disagreement that should abort inference for the field. */
bool adt_field_collect_type_args(const char **tps, uint8_t n_tps,
                                 const Type *expected, Type actual,
                                 Type *type_args, bool *have_type_args) {
    if (!expected || !tps) return true;
    switch (expected->kind) {
        case TY_TYVAR: {
            if (!expected->as.tyvar_.name) return true;
            uint8_t idx = 0; bool found = false;
            for (uint8_t i = 0; i < n_tps; i++)
                if (tps[i] && strcmp(tps[i], expected->as.tyvar_.name) == 0) {
                    idx = i; found = true; break;
                }
            if (!found) return true;
            if (!have_type_args[idx]) {
                type_args[idx] = actual;
                have_type_args[idx] = true;
                return true;
            }
            return type_eq(type_args[idx], actual);
        }
        case TY_APP:
            if (actual.kind != TY_APP || !expected->as.app.fn ||
                !expected->as.app.arg || !actual.as.app.fn || !actual.as.app.arg)
                return false;
            return adt_field_collect_type_args(tps, n_tps, expected->as.app.fn,
                                               *actual.as.app.fn, type_args,
                                               have_type_args) &&
                   adt_field_collect_type_args(tps, n_tps, expected->as.app.arg,
                                               *actual.as.app.arg, type_args,
                                               have_type_args);
        case TY_FN: {
            if (actual.kind != TY_FN) return false;
            if (expected->as.fn.arity != actual.as.fn.arity) return false;
            for (uint32_t i = 0; i < expected->as.fn.arity; i++) {
                Type exp_arg = (expected->as.fn.arg_full_types &&
                                expected->as.fn.arg_full_types[i])
                    ? *expected->as.fn.arg_full_types[i]
                    : type_from_kind(expected->as.fn.arg_kinds[i]);
                Type act_arg = (actual.as.fn.arg_full_types &&
                                actual.as.fn.arg_full_types[i])
                    ? *actual.as.fn.arg_full_types[i]
                    : type_from_kind(actual.as.fn.arg_kinds[i]);
                if (!adt_field_collect_type_args(tps, n_tps, &exp_arg, act_arg,
                                                 type_args, have_type_args))
                    return false;
            }
            Type exp_res = expected->as.fn.result_full_type
                ? *expected->as.fn.result_full_type
                : type_from_kind(expected->as.fn.result_kind);
            Type act_res = actual.as.fn.result_full_type
                ? *actual.as.fn.result_full_type
                : type_from_kind(actual.as.fn.result_kind);
            return adt_field_collect_type_args(tps, n_tps, &exp_res, act_res,
                                               type_args, have_type_args);
        }
        default:
            /* Concrete field (int/cstr/...): nothing to bind, never fail. */
            return true;
    }
}

/* Phase RF0: Check if a symbol was registered as a forward-declared type stub */
static bool elab_is_forward_type(Elab *e, const Symbol *sym) {
    for (uint32_t i = 0; i < e->n_forward_type_syms; i++) {
        if (e->forward_type_syms[i] == sym) return true;
    }
    return false;
}

/* Phase RF0: Add a symbol to the forward-declared types list */
void elab_add_forward_type(Elab *e, const Symbol *sym) {
    if (e->n_forward_type_syms >= e->cap_forward_type_syms) {
        e->cap_forward_type_syms = e->cap_forward_type_syms ? e->cap_forward_type_syms * 2 : 8;
        e->forward_type_syms = (const Symbol **)realloc(e->forward_type_syms,
            e->cap_forward_type_syms * sizeof(Symbol *));
    }
    e->forward_type_syms[e->n_forward_type_syms++] = sym;
}

/* structdef-retirement DS-D: elab_register_struct_def and the e->struct_defs[]
 * registry it wrote are deleted.  Every defstruct lowers to a record ADT, so no
 * StructDef is ever produced and the registry stayed empty (proven by DS-B's
 * live assert across a green suite). */

/* defstruct-grouped-field-spec-vectors: flatten an old-syntax field vector so a
 * grouped `[name : type]` sub-vector splices into the surrounding token stream
 * (the shape a `~@(map (fn [c] `[~c : T]) comps)` macro splice produces).
 * Returns the input vec unchanged when there is no grouping. */
static Form *defstruct_flatten_grouped_field_vec(Elab *e, const Form *fields_vec) {
    if (!fields_vec || fields_vec->tag != F_VEC) return (Form *)fields_vec;
    bool has_grouped = false;
    for (uint32_t i = 0; i < fields_vec->as.list.len; i++)
        if (fields_vec->as.list.items[i]->tag == F_VEC) { has_grouped = true; break; }
    if (!has_grouped) return (Form *)fields_vec;
    uint32_t flat_n = 0;
    for (uint32_t i = 0; i < fields_vec->as.list.len; i++) {
        const Form *it = fields_vec->as.list.items[i];
        flat_n += (it->tag == F_VEC) ? it->as.list.len : 1;
    }
    Form **flat = (Form **)arena_alloc(e->arena, (flat_n ? flat_n : 1) * sizeof(Form *));
    uint32_t k = 0;
    for (uint32_t i = 0; i < fields_vec->as.list.len; i++) {
        Form *it = fields_vec->as.list.items[i];
        if (it->tag == F_VEC)
            for (uint32_t j = 0; j < it->as.list.len; j++) flat[k++] = it->as.list.items[j];
        else
            flat[k++] = it;
    }
    return form_vec(e->arena, fields_vec->span, flat, flat_n);
}

Expr *elab_defstruct(Elab *e, const Form *call) {
    if (call->as.list.len < 2) {
        diag_emit(DIAG_ERROR, call->span,
                  "defstruct requires a name and field list: (defstruct Name [:copy] [f1 : T1 ...])");
        return NULL;
    }
    
    Form *name_form = call->as.list.items[1];
    if (name_form->tag != F_SYM) {
        diag_emit(DIAG_ERROR, name_form->span,
                  "defstruct name must be a symbol");
        return NULL;
    }
    const Symbol *name = name_form->as.sym;
    
    /* Check for optional leading :copy / :move / :linear / :heap annotations.
     * A loop (rather than a single check at index 2) lets :heap combine with the
     * substructural keyword in any order, while remaining backward-compatible
     * with the single-keyword form (the `else break` preserves the old
     * "unrecognised keyword -> fall through to fields/type-params" behaviour). */
    bool is_copy = false;
    bool is_linear = false;
    bool is_heap = false;
    bool no_auto_ctor = false; /* CTOR-V0 */
    uint32_t fields_start_idx = 2;

    while (fields_start_idx < call->as.list.len) {
        Form *kw_form = call->as.list.items[fields_start_idx];
        if (kw_form->tag != F_KEYWORD) break;
        if (kw_form->as.sym == e->kw_copy) {
            is_copy = true;
        } else if (kw_form->as.sym == e->kw_move) {
            is_copy = false;
        } else if (kw_form->as.sym == e->kw_linear) {
            /* LT4: :linear structs are exactly-once (CK_LINEAR). */
            is_linear = true;
        } else if (kw_form->as.sym == e->kw_heap) {
            /* end-to-end-monomorphization: typed-pointer ABI (Vec/Map/Set/...). */
            is_heap = true;
        } else if (kw_form->as.sym == e->kw_no_auto_ctor) {
            /* CTOR-V0: opt out of the auto-bound value-namespace constructor. */
            no_auto_ctor = true;
        } else {
            break;
        }
        fields_start_idx++;
    }
    
    /* Phase TM0: optional type-parameter vector [K V ...] before field definitions.
     * If the next form is a vector containing only symbols (no keyword annotations),
     * treat it as a type-param list; field defs then come as separate list forms.
     * Old syntax [field :type ...] is unchanged (vector contains keywords). */
    const char **type_params_arr = NULL;
    uint8_t n_type_params_v = 0;
    bool new_field_syntax = false;
    const Symbol **field_type_params = NULL;
    Kind *field_type_param_kinds = NULL;
    Form *type_param_vec_form = NULL;  /* original [A B ...] form, for lowering */

    if (fields_start_idx < call->as.list.len &&
        call->as.list.items[fields_start_idx]->tag == F_VEC) {
        Form *maybe_tp = call->as.list.items[fields_start_idx];
        bool all_syms = true;
        for (uint32_t pi = 0; pi < maybe_tp->as.list.len; pi++) {
            if (maybe_tp->as.list.items[pi]->tag != F_SYM) {
                all_syms = false;
                break;
            }
        }
        if (all_syms && maybe_tp->as.list.len > 0) {
            type_param_vec_form = maybe_tp;
            /* This is a type-params list; remaining forms are (field :type) lists */
            n_type_params_v = (uint8_t)maybe_tp->as.list.len;
            type_params_arr = (const char **)arena_alloc(e->arena,
                n_type_params_v * sizeof(char *));
            field_type_params = (const Symbol **)arena_alloc(e->arena,
                n_type_params_v * sizeof(Symbol *));
            field_type_param_kinds = (Kind *)arena_alloc(e->arena,
                n_type_params_v * sizeof(Kind));
            for (uint8_t pi = 0; pi < n_type_params_v; pi++) {
                const Symbol *psym = maybe_tp->as.list.items[pi]->as.sym;
                /* Variadic HKT rows: a `^&name` prefix marks a row-kinded type
                 * parameter (kind [*], KIND_TYPEROW) -- it ranges over a row of
                 * types (`#row{...}`), e.g. an ECS Query's component row. The
                 * `^&` is stripped so field types and call-site ascriptions
                 * reference the bare name. Other kinds are still inferred from
                 * field usage by infer_struct_type_param_kinds below. */
                if (psym->len > 2 && psym->name[0] == '^' && psym->name[1] == '&') {
                    const Symbol *bare = symtab_intern(e->st,
                        strslice(psym->name + 2, psym->len - 2));
                    type_params_arr[pi]      = bare->name;
                    field_type_params[pi]    = bare;
                    field_type_param_kinds[pi] = KIND_TYPEROW;
                } else {
                    type_params_arr[pi]      = psym->name;
                    field_type_params[pi]    = psym;
                    field_type_param_kinds[pi] = KIND_STAR;
                }
            }
            fields_start_idx++;
            new_field_syntax = true;
        }
    }

    /* defstruct-bracket-fields-with-type-params: after a leading type-param
     * vector [S A ...] is consumed, a following bracket field vector
     * `[a : T b : U ...]` is old-style fields WITH type params -- NOT the
     * new-syntax `(name type)` list form.  The new-syntax collector below reads
     * only items[0]/items[1] of the single field vector, silently dropping every
     * field after the first (with any type-param count, 1 or more).  Route a
     * bracket field vec back through the old-syntax field-vec path, which flattens
     * and keeps every field. */
    if (new_field_syntax && fields_start_idx < call->as.list.len &&
        call->as.list.items[fields_start_idx]->tag == F_VEC) {
        new_field_syntax = false;
    }

    if (call->as.list.len < fields_start_idx + 1) {
        diag_emit(DIAG_ERROR, call->span,
                  "defstruct requires a field list");
        return NULL;
    }

    /* For new-style syntax, fields are F_LIST forms; for old-style, one F_VEC. */
    Form *fields_form = NULL;
    if (!new_field_syntax) {
        fields_form = call->as.list.items[fields_start_idx];
        if (fields_form->tag != F_VEC) {
            diag_emit(DIAG_ERROR, fields_form->span,
                      "defstruct field list must be a vector [f1 : T1 f2 : T2 ...]");
            return NULL;
        }
        /* defstruct-grouped-field-spec-vectors: flatten grouped `[name : type]`
         * sub-vectors into the surrounding `name`, `: type` token stream (the
         * shape a `~@(map (fn [c] `[~c : (T ~c)]) comps)` splice produces). */
        fields_form = defstruct_flatten_grouped_field_vec(e, fields_form);
    }

    /* CONV-S1 (defstruct-as-defadt, graduated): lower the `defstruct` to a
     * single-variant record `defadt`, so it flows through the by-value ADT path
     * -- scalar/pointer/fn/aggregate/`any` fields, old OR new field syntax,
     * `:copy`/`:linear`/`:heap`/`:no-auto-ctor`, and PARAMETRIC structs (a
     * parametric record defadt; the dot-accessor and by-value codegen
     * substitute the app's type args).  Rewrite, preserving the keywords and the
     * type-param vec:
     *     (defstruct P [a : int b : int])      -> (defdata P (P [a : int b : int]))
     *     (defstruct Box [A] (val A))           -> (defdata Box [A] (Box [val : A]))
     * and dispatch to elab_defdata, reusing all the AdtDef machinery.
     *
     * UNCONDITIONAL.  This used to sit behind `defstruct_lowers_to_adt`, a
     * per-field-shape gate that was widened slice by slice until every
     * well-formed shape lowered; what it still rejected was only MALFORMED
     * field lists (`[]`, `[a : int b]`, `[1 : int]`, `[v : 7]`), for which it
     * produced a single vague "unsupported field form" where the record-ADT
     * parser below reports the precise defect at the offending token.  The
     * gate was also the one place a field shape could be accepted by the
     * parser yet refused here -- `[v : any]` was, until its TY_ANY arm was
     * added -- so keeping a second classifier bought nothing and cost a
     * disagreement.  The pre-pass (elab_toplevel.c) registers an ADT stub for
     * every defstruct on the same understanding.  See
     * docs/archive/defstruct-struct-path-is-dead.md and
     * docs/archive/defstruct-as-defadt-plan.md. */
    {
        /* Redefinition guard -- must run BEFORE dispatching into elab_defdata.
         * A `defstruct` that redefines a fully-defined name (commonly an
         * auto-loaded stdlib type such as `Cons`/`Pair`) must produce the
         * defstruct-specific "already defined" diagnostic, not crash inside
         * elab_defdata's forward-stub-reuse path.  A same-name GADT may
         * legitimately coexist with a struct (MF4), so don't block that. */
        {
            /* Scan the def registries directly rather than via scope_lookup:
             * the top-level pre-pass registers a forward stub for THIS
             * redefinition (n_ctors == 0), which masks the already-FILLED-IN
             * stdlib/earlier definition in the scope.  Mirror the struct-path
             * DS4-2 check (n_fields/n_ctors > 0 == filled in == redefinition; an
             * empty forward stub is still re-elaborable).  A same-name GADT may
             * coexist with a struct (MF4), so it is not "fully defined" here. */
            AdtDef *prior = NULL;
            for (uint32_t ai = 0; ai < e->n_adt_defs && !prior; ai++) {
                AdtDef *ad = e->adt_defs[ai];
                if (ad && ad->name && strcmp(ad->name, name->name) == 0 &&
                    ad->n_ctors > 0 && !ad->is_gadt)
                    prior = ad;
            }
            /* structdef-retirement DS-C: the parallel scan over the (always-empty)
             * struct_defs registry is dead -- a prior definition of this name is
             * an AdtDef (every lowered defstruct/defdata) checked above.
             *
             * PS4: a type an earlier REPL/playground turn defined is not a clash
             * -- defdata re-elaborates over it, and defstruct lowers onto that
             * path -- unless the stdlib defined it: rewriting the stdlib's own
             * type in place breaks the stdlib code built on it (redefine Pair,
             * and `(.fst (pair 1 2))` stops resolving). */
            bool from_stdlib = prior && elab_file_is_stdlib(prior->origin_file_id);
            if (prior && (from_stdlib || !elab_prior_turn_adt(e, prior))) {
                if (from_stdlib)
                    diag_emit(DIAG_ERROR, name_form->span,
                              "defstruct: '%s' is already defined by an auto-loaded "
                              "stdlib module, and redefining it would change it "
                              "for the stdlib code built on it; pick a distinct name",
                              name->name);
                else
                    diag_emit(DIAG_ERROR, name_form->span,
                              "defstruct: '%s' is already defined by an earlier "
                              "form; pick a distinct name", name->name);
                return NULL;
            }
        }
        /* Build the record-variant field vector [name : type ...].  Old syntax
         * already has fields_form in that shape; new syntax has separate
         * (name type) forms (call->items[fields_start_idx ..]) that we flatten to
         * [name type ...] -- the record-variant parser unwraps an F_TYPE_ANN or
         * accepts a bare type form in the odd slots either way. */
        Form *field_vec;
        if (!new_field_syntax) {
            field_vec = fields_form;
        } else {
            uint32_t nf = call->as.list.len - fields_start_idx;
            Form **fv = (Form **)arena_alloc(e->arena,
                            (nf > 0 ? nf * 2 : 1) * sizeof(Form *));
            uint32_t k = 0;
            for (uint32_t ci = fields_start_idx; ci < call->as.list.len; ci++) {
                Form *ff = call->as.list.items[ci];
                fv[k++] = ff->as.list.items[0];   /* field name */
                fv[k++] = ff->as.list.items[1];   /* field type (bare or F_TYPE_ANN) */
            }
            field_vec = form_vec(e->arena, call->span, fv, k);
        }
        /* Constructor variant form: (Name <field-vec>) */
        Form *ctor_items[2] = { name_form, field_vec };
        Form *ctor_form = form_list(e->arena, call->span, ctor_items, 2);
        /* (defdata Name [:copy] [:heap] [type-params] (Name <field-vec>)) --
         * defdata's argument order is name, optional :copy/:move/:heap keywords,
         * optional type-param vec, then the constructor(s).  Seam 3: a :heap
         * struct lowers to a :heap record defadt (typed-pointer ABI). */
        Form *dd_items[8];
        uint32_t ddn = 0;
        dd_items[ddn++] = form_sym(e->arena, name_form->span, e->sym_defdata);
        dd_items[ddn++] = name_form;
        if (is_copy)
            dd_items[ddn++] = form_keyword(e->arena, name_form->span, e->kw_copy);
        if (is_heap)
            dd_items[ddn++] = form_keyword(e->arena, name_form->span, e->kw_heap);
        /* structdef-retirement slice 4: forward `:linear` so the lowered ADT type
         * carries CK_LINEAR and the exactly-once enforcement propagates. */
        if (is_linear)
            dd_items[ddn++] = form_keyword(e->arena, name_form->span, e->kw_linear);
        /* structdef-retirement slice 2: forward `:no-auto-ctor` so elab_defdata
         * suppresses the value-namespace constructor (the `(Name ...)` call form
         * stays rejected; construction is via make-struct). */
        if (no_auto_ctor)
            dd_items[ddn++] = form_keyword(e->arena, name_form->span,
                                           e->kw_no_auto_ctor);
        if (type_param_vec_form)
            dd_items[ddn++] = type_param_vec_form;  /* original [A B ...] vec, kinds intact */
        dd_items[ddn++] = ctor_form;
        Form *dd_form = form_list(e->arena, call->span, dd_items, ddn);
        Expr *dd_out = elab_defdata(e, dd_form);
        /* Mark the synthesized AdtDef as struct-origin so consumers that must
         * keep the lowering invisible (runtime type-of/cast/is?) can treat it
         * as the struct it came from. */
        if (dd_out && dd_out->kind == EX_DEFDATA && dd_out->as.defdata_.def)
            dd_out->as.defdata_.def->from_struct_lowering = true;
        return dd_out;
    }

}

/* SI4-C: defopaque -- named opaque int64_t newtype for REPL type tags.
 * Syntax: (defopaque Name :int)
 *         (defopaque Name [A ...] :int)         ;; phantom type parameters
 *         (defopaque Name :ptr<void> :linear)   ;; exactly-once resource handle
 *         (defopaque Name :ptr<void> :affine)   ;; at-most-once resource handle
 * structdef-retirement slice 5: creates an opaque AdtDef (n_ctors == 0) with
 * is_opaque=true (migrated off StructDef); type_c_name → "int64_t" everywhere.
 * The optional trailing :linear / :affine keyword promotes the newtype to a
 * substructural resource handle (enforced only under -Xlinear / -Xsubstructural;
 * the C ABI is unaffected -- the handle still lowers to int64_t).
 *
 * An optional type-parameter vector [A ...] between the name and the base type
 * declares phantom type parameters: the carrier stays the declared base type
 * (always int64_t at the C level), but the newtype becomes a type constructor
 * of kind '* -> *' (etc.) so it can be spelled `(Name A)` in annotations and
 * track an element/index type at the type level. */
Expr *elab_defopaque(Elab *e, const Form *call) {
    if (call->as.list.len < 3) {
        diag_emit(DIAG_ERROR, call->span,
                  "defopaque requires a name and base type: (defopaque Name :int)");
        return NULL;
    }
    Form *name_form = call->as.list.items[1];
    if (name_form->tag != F_SYM) {
        diag_emit(DIAG_ERROR, name_form->span, "defopaque name must be a symbol");
        return NULL;
    }
    const Symbol *name = name_form->as.sym;
    Binding *existing_b = scope_lookup(e->scope, name);
    if (existing_b && !elab_is_forward_type(e, name)) {
        diag_emit(DIAG_ERROR, name_form->span,
                  "defopaque: '%s' is already defined", name->name);
        return NULL;
    }

    /* Optional phantom type-parameter vector [A ...] immediately after the name.
     * Stored on the StructDef as phantoms; the carrier stays int64_t. */
    const char **type_params_arr = NULL;
    uint8_t n_type_params_v = 0;
    uint32_t base_idx = 2;
    if (call->as.list.items[2]->tag == F_VEC) {
        Form *tp_form = call->as.list.items[2];
        n_type_params_v = (uint8_t)tp_form->as.list.len;
        if (n_type_params_v == 0) {
            diag_emit(DIAG_ERROR, tp_form->span,
                      "defopaque: type-parameter vector cannot be empty");
            return NULL;
        }
        type_params_arr = (const char **)arena_alloc(e->arena,
            n_type_params_v * sizeof(char *));
        for (uint8_t pi = 0; pi < n_type_params_v; pi++) {
            Form *pf = tp_form->as.list.items[pi];
            if (pf->tag != F_SYM) {
                diag_emit(DIAG_ERROR, pf->span,
                          "defopaque: type parameter must be a symbol, e.g. A");
                return NULL;
            }
            type_params_arr[pi] = pf->as.sym->name;
        }
        base_idx = 3;
        /* With a type-param vector consumed, the base type is mandatory. */
        if (call->as.list.len < base_idx + 1) {
            diag_emit(DIAG_ERROR, call->span,
                      "defopaque requires a base type after the type-parameter "
                      "vector: (defopaque Name [A] :int)");
            return NULL;
        }
    }

    /* Optional attributes after the base type.  These used to be a single
     * substructural keyword; `:sealed` is orthogonal to :linear/:affine (it
     * governs `::` visibility, not how many times the value may be used), so
     * the slot now takes a SET.  :linear and :affine remain mutually exclusive
     * -- "exactly once" and "at most once" are contradictory claims. */
    bool opaque_linear = false;
    bool opaque_affine = false;
    bool opaque_sealed = false;
    bool opaque_non_null = false;
    for (uint32_t ai = base_idx + 1; ai < call->as.list.len; ai++) {
        Form *attr = call->as.list.items[ai];
        if (attr->tag == F_KEYWORD && attr->as.sym == e->kw_linear) {
            opaque_linear = true;
        } else if (attr->tag == F_KEYWORD && attr->as.sym == e->kw_affine) {
            opaque_affine = true;
        } else if (attr->tag == F_KEYWORD && attr->as.sym == e->kw_sealed) {
            opaque_sealed = true;
        } else if (attr->tag == F_KEYWORD && attr->as.sym == e->kw_non_null) {
            opaque_non_null = true;
        } else {
            diag_emit(DIAG_ERROR, attr->span,
                      "defopaque: unexpected attribute -- expected :linear, "
                      ":affine, :sealed or :non-null");
            return NULL;
        }
    }
    if (opaque_linear && opaque_affine) {
        diag_emit(DIAG_ERROR, call->span,
                  "defopaque: :linear and :affine are mutually exclusive "
                  "(exactly-once vs at-most-once)");
        return NULL;
    }
    AdtDef *def;
    Binding *b;
    /* structdef-retirement slice 5: an opaque newtype is an opaque AdtDef
     * (n_ctors == 0), migrated off StructDef so StructDef can be retired.
     * Phase RF0: the top-level pre-pass forward-registers every defopaque as a
     * stub def (is_copy=true) and binds the type name to type_adt(stub). If we
     * allocated a fresh def here, the type binding -- and every `: Name`
     * annotation resolved through it -- would keep pointing at the stub, so the
     * :linear / :affine discipline would silently never apply. Reuse the stub in
     * place (mirroring elab_defstruct) and refresh the binding's cached type so
     * copy_kind / substruct reflect the declared discipline. */
    if (existing_b && elab_is_forward_type(e, name) &&
            existing_b->type.kind == TY_ADT && existing_b->type.as.adt_.def) {
        b = existing_b;
        def = b->type.as.adt_.def;
    } else {
        def = (AdtDef *)arena_alloc(e->arena, sizeof(AdtDef));
        memset(def, 0, sizeof(*def));  /* DS5: zero all bool / scalar fields by default */
        b = NULL;
    }
    def->name = name->name;
    /* A linear/affine handle is not freely copyable; only a plain opaque is. */
    def->is_copy = !(opaque_linear || opaque_affine);
    def->is_linear = opaque_linear;
    def->is_affine = opaque_affine;
    def->is_opaque = true;
    /* opaque-pointer-c-spelling gate: record whether the DECLARED base type is
     * a pointer.  Until now `base_idx` located the base form only so the
     * attribute scan could start after it -- the form itself was never read, so
     * `(defopaque String :ptr<void>)` and `(defopaque UserId :int)` were
     * indistinguishable downstream.  The base is a type keyword (`:ptr`,
     * `:ptr<void>`, `:ptr<T>`, `:int`, ...), so the test is a prefix match on
     * the keyword's own name. */
    bool base_is_ptr_keyword = false;
    {
        const Form *base_form = call->as.list.items[base_idx];
        if (base_form->tag == F_KEYWORD && base_form->as.sym) {
            const char *bn = base_form->as.sym->name;
            base_is_ptr_keyword =
                (strcmp(bn, "ptr") == 0 || strcmp(bn, "ptr-void") == 0 ||
                 strncmp(bn, "ptr<", 4) == 0);
            /* defopaque-over-sym-skips-the-ptr-bridge: `cstr` and `Sym` are
             * pointer-sized carriers that are not SPELLED as pointers, so they
             * took the int64 path -- and every store of one into the newtype
             * emitted `int64_t r = (const struct __tur_sym *)...` /
             * `int64_t n = "alice"`, a -Wint-conversion that clang >= 21
             * rejects outright, with the read-back seam (`puts(n_1607)`)
             * failing the other way.  A one-field `defstruct` over the same
             * `Sym` had been fixed a release earlier (`4ff9ad724`); this is
             * the opaque twin.  The judgement is "is this carrier a
             * pointer", not "is it spelled :ptr", so both take the pointer
             * spelling (`void *`), and the ascription seams that already
             * bridge an opaque pointer handle cover them. */
            def->opaque_base_is_ptr =
                base_is_ptr_keyword ||
                strcmp(bn, "cstr") == 0 || strcmp(bn, "Sym") == 0;
        }
    }
    /* option-niche: `:non-null` declares "this handle's valid values exclude
     * the null pointer", which only means anything over a pointer base -- on an
     * int newtype 0 is just a number, and accepting the attribute there would
     * let a later base-type edit silently keep a stale claim.  Kept to the
     * `:ptr` spellings: a `cstr` can legitimately be null and no author claim
     * is taken for it. */
    if (opaque_non_null && !base_is_ptr_keyword) {
        diag_emit(DIAG_ERROR, call->span,
                  "defopaque: :non-null requires a pointer base type "
                  "(:ptr or :ptr<...>) -- it declares that the handle's valid "
                  "values exclude the null pointer");
        return NULL;
    }
    def->opaque_non_null = opaque_non_null;
    /* sealed-opaque: assigned UNCONDITIONALLY, like the flags above -- the
     * forward-declared-stub path reuses an existing def rather than the
     * freshly memset one, so a conditional assignment would silently leave a
     * re-elaborated def unsealed. */
    def->sealed = opaque_sealed;
    def->sealed_module = e->current_module_name;   /* NULL at a moduleless top level */
    def->n_ctors = 0;        /* an opaque newtype has no constructors */
    def->ctors = NULL;
    def->origin_file_id = call->span.file_id;
    /* Phantom type parameters: the carrier is still int64_t, but the newtype is
     * a type constructor (kind '* -> *' etc.) so `(Name A)` annotations parse
     * and the element/index type is tracked at the type level. */
    def->type_params = type_params_arr;
    def->n_type_params = n_type_params_v;
    if (n_type_params_v > 0) {
        Kind *kinds = (Kind *)arena_alloc(e->arena, n_type_params_v * sizeof(Kind));
        for (uint8_t pi = 0; pi < n_type_params_v; pi++) kinds[pi] = KIND_STAR;
        def->type_param_kinds = kinds;
    }
    Type opaque_type = type_adt(def);
    opaque_type.hkt_kind = kind_for_arity(n_type_params_v);
    if (b) {
        b->type = opaque_type;  /* refresh cached copy_kind / substruct + hkt_kind */
    } else {
        b = binding_new(e, name, opaque_type, false, true, name_form->span);
        scope_add(&e->global, b);
        elab_register_adt_def(e, def);
    }
    Expr *out = expr_new(e->arena, EX_DEF, TYPE_NIL, call->span);
    out->as.def_.binding = b;
    out->as.def_.init = NULL;
    return out;
}

/* Phase G0: Helper - register AdtDef in elab registry */
void elab_register_adt_def(Elab *e, AdtDef *def) {
    if (e->n_adt_defs >= e->cap_adt_defs) {
        e->cap_adt_defs = e->cap_adt_defs ? e->cap_adt_defs * 2 : 8;
        e->adt_defs = (AdtDef **)realloc(e->adt_defs,
            e->cap_adt_defs * sizeof(AdtDef *));
    }
    e->adt_defs[e->n_adt_defs++] = def;
}

/* CONV-S0 (struct/ADT convergence): resolve a single constructor field-type
 * form into ctor->fields[fi] (kind/inner_kind/full_type) and ctor->field_forms[fi].
 * Shared by positional-style variants (`(Just :int)`) and record-style variants
 * (`(Circle [radius : float])`); the record path also sets ctor->fields[fi].name.
 * Returns false (diag already emitted) on an unresolvable field type. */
/* SR1: does this field-type form name `def` itself?  See AdtDef.is_self_recursive
 * -- a recursive field's resolved full_type is deliberately NULL, so the only
 * place the fact is observable is here, against the form the user wrote.  Walks
 * a compound form (`(Vec Term)`, `(Pair Term int)`) so a self-reference nested
 * inside a type application counts too. */
static bool ctor_field_form_names_adt(const Form *ft_form, const char *adt_name) {
    if (!ft_form || !adt_name) return false;
    if (ft_form->tag == F_SYM || ft_form->tag == F_KEYWORD) {
        const Symbol *s = ft_form->as.sym;
        return s && s->name && strlen(adt_name) == s->len &&
               memcmp(s->name, adt_name, s->len) == 0;
    }
    if (ft_form->tag == F_LIST) {
        for (uint32_t i = 0; i < ft_form->as.list.len; i++)
            if (ctor_field_form_names_adt(ft_form->as.list.items[i], adt_name))
                return true;
    }
    return false;
}

static bool resolve_ctor_field(Elab *e, AdtDef *def, CtorDef *ctor, uint32_t fi,
                               Form *ft_form, const Symbol **tp_syms,
                               uint32_t n_type_params, bool record_style) {
    ctor->fields[fi].full_type = NULL;
    if (ctor->field_forms) ctor->field_forms[fi] = NULL;
    if (ctor_field_form_names_adt(ft_form, def->name))
        def->is_self_recursive = true;

    /* byvalue-recursive-adt-boxes-are-never-freed: a DIRECT self-reference
     * (`tl : Lst`, not `(Vec Lst)`) is an owning field, and until now nothing
     * said so.  The slot holds a pointer to a heap copy -- a by-value product
     * cannot ride the int64 carrier and cannot contain itself inline -- so a
     * list of N links is N allocations and no frees.  Measured in plain
     * Turmeric with no `any` anywhere: 3 cells / 3 allocations, 5 / 5.
     *
     * `drop_inner_def = def` is the same channel a nested owning aggregate
     * already uses; pointing it at the ADT itself makes the emitted glue
     * recursive, which is exactly the walk a spine needs.
     *
     * `:copy` IS THE SOUNDNESS LINE, and it is measured rather than assumed.
     * `needs_drop_glue` makes a type move-only, and the move discipline is what
     * guarantees the single owner this free depends on -- `(let [a (Cons 1 t)
     * b (Cons 2 t)] ...)` is already TUR-E0201 "cannot copy unique value 't'".
     * Under `:copy` that same program compiles, and the emitted C shows both
     * boxes carrying the SAME tail pointer, so freeing each chain would free it
     * twice.  So a `:copy` recursive ADT keeps the leak; `with-region` reclaims
     * it (verified: zero leaks inside a bracket) and is the answer there.
     *
     * A `:heap` ADT is excluded for the reason the sibling rules exclude it:
     * its node is a typed pointer with its own teardown story. */
    if (!def->is_copy && !def->is_heap &&
        (ft_form->tag == F_SYM || ft_form->tag == F_KEYWORD) &&
        ft_form->as.sym && ft_form->as.sym->name && def->name &&
        strlen(def->name) == ft_form->as.sym->len &&
        memcmp(ft_form->as.sym->name, def->name, ft_form->as.sym->len) == 0) {
        ctor->fields[fi].drop_inner_def = def;
        def->needs_drop_glue = true;
    }

    /* TP1: a bare symbol (non-keyword) may be a declared type parameter.
     * E.g. `a` in `(defdata Opt2 [a] (Yep a))`. */
    {
        Type *tv = adt_field_type_from_form(e->arena, ft_form,
                                            def->type_params,
                                            def->n_type_params);
        if (tv) {
            ctor->fields[fi].kind = TY_INT;
            ctor->fields[fi].inner_kind = TY_UNKNOWN;
            ctor->fields[fi].full_type = tv;
            return true;
        }
    }

    /* Applied type constructor or other compound type form in field position. */
    if (ft_form->tag == F_LIST) {
        Type *t = struct_field_type_from_form(e, ft_form, tp_syms,
                                              def->type_param_kinds,
                                              n_type_params);
        if (!t) {
            diag_emit(DIAG_ERROR, ft_form->span,
                      "defdata: could not resolve constructor field type");
            return false;
        }
        /* capturing-closure-in-struct-field-segv: a concrete `(fn ...)` field
         * uses the FAT closure representation (a `{thunk, env}` handle in the
         * int64 slot), so a CAPTURING closure stored in it dispatches correctly
         * -- not the thin fn-pointer path, which called a fat env block as code
         * (SIGSEGV).  Marking the field type `boxed` steers every `(.f v)` read to
         * the fat dispatch (TUR_APPLY*); the make-struct store shims a bare/thin
         * fn into a fat handle (elab_call.c constructor arg loop).  Storage stays
         * the int64 carrier -- only the dispatch/representation changes. */
        /* Bound to arity 0..4: the field-call fat dispatch (emit_expr.c,
         * TUR_APPLY<N>_T) covers N<=4; the store shim (elab_call.c) covers the
         * same range.  Arity 0 was originally excluded alongside >4, which
         * left a THUNK field -- the lazy-stream shape, and the one the
         * closure-in-defdata-field report was filed about -- on the thin path,
         * where a capturing closure stored into it segfaulted at force time
         * (TUR_APPLY0_T existed all along; the exclusion was stale).  A >4-arg
         * fn field stays thin, and a capturing store into one is rejected at
         * elaboration rather than left to crash. */
        if (t && t->kind == TY_FN && !t->as.fn.boxed &&
            t->as.fn.arity <= 4) {
            Type *bt = (Type *)arena_alloc(e->arena, sizeof(Type));
            *bt = *t;
            bt->as.fn.boxed = true;
            t = bt;
            /* closure-drop-glue S2 (Model U): the boxed fn-field owns a heap fat
             * handle (a shim box for a bare fn, or a capturing env), so the struct
             * needs drop glue to free it -- which also makes the struct move-only,
             * the precondition that keeps a single owner and avoids a copy
             * double-freeing the shared handle. */
            def->needs_drop_glue = true;
        }
        TypeKind fkind = TY_UNKNOWN, finner = TY_UNKNOWN;
        struct_field_storage_from_type(t, &fkind, &finner);
        if (fkind == TY_UNKNOWN) { fkind = TY_INT; finner = TY_UNKNOWN; }
        ctor->fields[fi].kind = fkind;
        ctor->fields[fi].inner_kind = finner;
        ctor->fields[fi].full_type = t;
        if (ctor->field_forms) ctor->field_forms[fi] = ft_form;
        if (fkind == TY_RC || fkind == TY_REF || fkind == TY_WEAK) {
            def->needs_drop_glue = true;
        }
        /* drop-glue-shallow-nested-owning-aggregate: a nominal by-value
         * aggregate field whose inner def itself `needs_drop_glue` is stored
         * behind the int64 carrier (adt_field_is_inline_byval excludes a
         * drop-glue inner).  Flag the owner and remember the inner def so the
         * by-value drop/walk glue releases the boxed sub-aggregate.  Restricted
         * to a non-parametric nominal ADT: its drop glue is the plain
         * `drop_glue_tur_adt_<name>`, whereas a parametric applied monomorph
         * (`(Pair rc<int> int)`) uses a mangled monomorph name -- threading that
         * through is separate work.  A :heap inner is a typed pointer whose
         * teardown is separate deferred work. */
        if (t->kind == TY_ADT) {
            const AdtDef *iad = t->as.adt_.def;
            if (iad && iad->needs_drop_glue && !iad->is_heap &&
                iad->n_type_params == 0 && adt_is_byvalue_product(iad) &&
                (fkind == TY_INT || fkind == TY_ADT)) {
                ctor->fields[fi].drop_inner_def = iad;
                def->needs_drop_glue = true;
            }
        }
        return true;
    }

    /* Both positional and record-style variants accept keyword (`:int`) or
     * bare symbol (`int`) type names, mirroring defgadt / defstruct syntax. */
    bool ok_tag = (ft_form->tag == F_KEYWORD) || (ft_form->tag == F_SYM);
    if (!ok_tag) {
        diag_emit(DIAG_ERROR, ft_form->span,
                  "defdata: constructor field type must be a keyword like :int, :bool, :cstr");
        return false;
    }
    const char *tname = ft_form->as.sym->name;
    uint32_t tlen = ft_form->as.sym->len;
    TypeKind fkind, finner;
    parse_struct_field_type(tname, tlen, &fkind, &finner);
    if ((fkind == TY_RC || fkind == TY_WEAK) && finner == TY_UNKNOWN) {
        /* CONV-S1 (slice 5): rc<Name> over a user struct / record ADT -- carry
         * the inner def on the field's full_type so receivers of the rc field
         * auto-deref through it (mirrors DS3's lookup_rc_inner_struct_def on the
         * struct path).  The field still stores as the TY_RC carrier (the
         * inline-byval gate rejects a TY_RC full_type), so layout is unchanged;
         * only field-access resolution gains the inner layout.
         *
         * stdlib-weak-ref-audit WR1: weak<Name> takes the same path, so a
         * `[parent : weak<Node>]` back-edge agrees with the `weak<ADT>` that
         * `(weak r)` produces.  TY_WEAK is the same carrier as TY_RC
         * (RcControlBlock *), so this is likewise layout-neutral. */
        Type *rc_full = adt_rc_inner_full_type(e, tname, tlen);
        if (rc_full) {
            ctor->fields[fi].full_type = rc_full;
            finner = (rc_full->kind == TY_RC || rc_full->kind == TY_WEAK)
                         ? rc_full->as.rc.inner : finner;
        }
    }
    if (fkind == TY_UNKNOWN) {
        /* CONV-S1 seam 4: a KEYWORD field type (`:A`) naming a declared type
         * parameter.  The defstruct-as-defadt lowering carries defstruct's `:A`
         * field-type syntax verbatim into the record variant, but the bare-symbol
         * TP1 check above (adt_field_type_from_form, F_SYM only) misses the
         * keyword form, so a parametric `(defstruct Box [A] (val :A) ...)` lowered
         * to `(defdata Box [A] (Box [val : A] ...))` errored "unrecognized type
         * :A".  Resolve a keyword that matches a declared type param to a tyvar
         * field, exactly as the bare `A` symbol form is. */
        for (uint8_t pi = 0; pi < def->n_type_params; pi++) {
            if (def->type_params[pi] &&
                strlen(def->type_params[pi]) == tlen &&
                memcmp(def->type_params[pi], tname, tlen) == 0) {
                Type *tv = (Type *)arena_alloc(e->arena, sizeof(Type));
                *tv = type_tyvar_named(def->type_params[pi]);
                ctor->fields[fi].kind = TY_INT;
                ctor->fields[fi].inner_kind = TY_UNKNOWN;
                ctor->fields[fi].full_type = tv;
                if (ctor->field_forms) ctor->field_forms[fi] = ft_form;
                return true;
            }
        }
        /* Phase RF0: fall back to user-defined type lookup. */
        const Symbol *type_sym = symtab_intern(e->st, strslice(tname, tlen));
        Binding *tb = scope_lookup_type_def(e->scope, type_sym);
        if (tb && (tb->type.kind == TY_STRUCT || tb->type.kind == TY_ADT)) {
            fkind = TY_INT;
            finner = TY_UNKNOWN;
            /* CONV-S1 (slice 4 + slice 8): record the bare field's nominal
             * full_type so a field read resolves to the declared user type.  We
             * record it only for inner types whose by-value-vs-carrier
             * representation the record-ADT codegen handles end-to-end:
             *   - slice 4: a by-value aggregate inner (a non-heap/non-opaque/
             *     drop-glue-free struct, or a by-value ADT product) is stored
             *     INLINE by value in the owning by-value product, the way a struct
             *     inlines a nested struct field; `adt_field_is_inline_byval` reads
             *     full_type to spell the inline aggregate C type.
             *   - slice 8: a :heap struct inner is an int64 typed-pointer carrier
             *     -- `type_is_heap_struct` guards every byval<->carrier bridge, so
             *     the field carries its struct type for read-back (matching the
             *     struct path's struct_field_user_type_storage) without tripping a
             *     spurious concrete->carrier spill.
             * A carrier ADT inner (multi-variant / parametric / drop-glue) is
             * deliberately left full_type NULL: it stays an opaque int64 carrier,
             * exactly as the *struct* path erases an ADT field's type (a defstruct
             * with an ADT field cannot read it back typed either, so this is
             * parity).  Recording a carrier-ADT full_type would misclassify the
             * field read as a concrete by-value aggregate and emit a spurious
             * concrete->carrier address-of bridge (a silent miscompile). */
            bool record_full = false;
            /* structdef-retirement DS-D: no Type is ever TY_STRUCT, so the
             * former struct branch (reading the removed `.as.struct_` member) is
             * dead -- the guard above only admits TY_ADT here. */
            AdtDef *ad = tb->type.as.adt_.def;
            {
                /* by-value ADT product (inlined, slice 4), a :heap record ADT
                 * (typed-pointer carrier, seam 3 -- the ADT analogue of a :heap
                 * struct field), or a forward-declared stub (n_ctors == 0) whose
                 * fill-in is a product/heap -- record optimistically, matching the
                 * struct path which records a forward struct stub via
                 * !needs_drop_glue.  The def pointer is stable across the fill, so
                 * the recorded full_type sees the final layout at codegen. */
                record_full = ad && !ad->needs_drop_glue &&
                              (ad->is_heap || ad->n_ctors == 0 ||
                               adt_is_byvalue_product(ad));
            }
            if (record_full) {
                Type *ft = (Type *)arena_alloc(e->arena, sizeof(Type));
                *ft = tb->type;
                ctor->fields[fi].full_type = ft;
            }
            /* drop-glue-shallow-nested-owning-aggregate: a nested by-value
             * aggregate field whose inner def itself `needs_drop_glue` is stored
             * behind the int64 carrier (record_full is false above, so full_type
             * stays NULL and the read path is unchanged).  Flag the owner as
             * needing drop glue and stash the inner def so the by-value drop/walk
             * glue tears the boxed sub-aggregate down (a `drop_glue_<Inner>` /
             * `walk_glue_<Inner>` call) instead of leaking it.  A :heap inner is
             * excluded -- its typed-pointer teardown is separate deferred work. */
            if (ad && ad->needs_drop_glue && !ad->is_heap &&
                adt_is_byvalue_product(ad)) {
                ctor->fields[fi].drop_inner_def = ad;
                def->needs_drop_glue = true;
            }
        } else {
            diag_emit(DIAG_ERROR, ft_form->span,
                      "defdata: field has unrecognized type :%s", tname);
            return false;
        }
    }
    ctor->fields[fi].kind = fkind;
    ctor->fields[fi].inner_kind = finner;
    if (ctor->field_forms) ctor->field_forms[fi] = ft_form;
    if (fkind == TY_RC || fkind == TY_REF || fkind == TY_WEAK) {
        def->needs_drop_glue = true;
    }
    /* any-widen-stored-in-an-adt-field-has-no-owner: an `:any` field is OWNING.
     *
     * `any-struct-box-leak-per-widen` gave every `any` payload box an owner
     * across five passes, and every one of those owners is a SCOPE -- an
     * argument, a local, a temporary, a narrowed binding.  A box stored into a
     * FIELD outlives every scope they can attach a drop to, correctly so, and
     * so it had none: one leaked malloc per value widened into the field, which
     * a container with `any` elements pays once per element.
     *
     * The drop is `__tur_any_drop`, not a statically-named glue: the payload's
     * type is whatever the tag says at run time, and the registry row already
     * carries the `boxed` flag that says whether it was heap-boxed at all.  So
     * an `any` field holding an int frees nothing and one holding a widened
     * aggregate frees the box, decided by the same row the widen wrote.
     *
     * `:copy` and `:heap` are excluded for the reasons the recursive-field rule
     * beside this one records: drop glue makes a type move-only, and that move
     * discipline is the single-owner guarantee the free depends on.  Under
     * `:copy` two values can share the box and a per-owner free would free it
     * twice. */
    if (fkind == TY_ANY && !def->is_copy && !def->is_heap) {
        def->needs_drop_glue = true;
    }
    return true;
}

/* CONV-S0: a constructor form is record-style when its sole payload is a
 * vector, e.g. `(Circle [radius : float])`.  The vector holds `name : type`
 * pairs exactly like the old-style defstruct field list. */
static bool ctor_form_is_record(const Form *ctor_form) {
    return ctor_form->as.list.len == 2 &&
           ctor_form->as.list.items[1]->tag == F_VEC;
}

/* Phase G0: defdata — define a sum type (ADT)
 * Syntax: (defdata Name [:copy]
 *           (Ctor1)
 *           (Ctor2 :T1 :T2)               ; positional-style variant
 *           (Ctor3 [a : int b : cstr])    ; CONV-S0 record-style variant
 *           ...)
 */
Expr *elab_defdata(Elab *e, const Form *call) {
    if (call->as.list.len < 3) {
        diag_emit(DIAG_ERROR, call->span,
                  "defdata requires a name and at least one constructor: "
                  "(defdata Name (Ctor1) (Ctor2 :T1) ...)");
        return NULL;
    }

    Form *name_form = call->as.list.items[1];
    if (name_form->tag != F_SYM) {
        diag_emit(DIAG_ERROR, name_form->span,
                  "defdata name must be a symbol");
        return NULL;
    }
    const Symbol *name = name_form->as.sym;

    /* Check for optional :copy / :move / :heap annotations (any order).
     * CONV-S1 seam 3: :heap marks a typed-pointer record ADT (the analogue of a
     * :heap struct -- Vec/Map/Set), set by a lowered `:heap` defstruct. */
    bool is_copy = false;
    bool is_heap = false;
    bool is_linear = false;     /* structdef-retirement slice 4 (LT4) */
    bool no_auto_ctor = false;  /* structdef-retirement slice 2 (CTOR-V0) */
    uint32_t ctors_start_idx = 2;
    while (ctors_start_idx < call->as.list.len) {
        Form *kw_form = call->as.list.items[ctors_start_idx];
        if (kw_form->tag != F_KEYWORD) break;
        if (kw_form->as.sym == e->kw_copy) { is_copy = true; ctors_start_idx++; continue; }
        if (kw_form->as.sym == e->kw_move) { is_copy = false; ctors_start_idx++; continue; }
        if (kw_form->as.sym == e->kw_heap) { is_heap = true; ctors_start_idx++; continue; }
        /* structdef-retirement slice 4: a lowered `:linear` defstruct (exactly-once)
         * carries CK_LINEAR on its ADT type; the value/binding-level linearity
         * enforcement then propagates from the type's copy_kind. */
        if (kw_form->as.sym == e->kw_linear) {
            is_linear = true; ctors_start_idx++; continue;
        }
        /* structdef-retirement slice 2: a lowered `:no-auto-ctor` defstruct (and,
         * by extension, a `defdata` that opts in) suppresses the auto-bound
         * value-namespace constructor so the `(Name ...)` call form stays rejected
         * ("not a function"); construction goes through `make-struct`. */
        if (kw_form->as.sym == e->kw_no_auto_ctor) {
            no_auto_ctor = true; ctors_start_idx++; continue;
        }
        break;
    }

    /* Phase RF1: Check for an optional type-parameter vector [^f a b ...] between
     * the name (or :copy annotation) and the first constructor.  Type parameters
     * are stored on the AdtDef and used for documentation / future type-checking;
     * they do not affect C codegen (all values are int64_t pointers). */
    const char **type_params = NULL;
    uint8_t n_type_params = 0;
    /* L6 follow-up C: parallel kind array so `^&name` in a defdata type-param
     * list (e.g. (defdata Frame [^&cols] (Frame :int))) carries KIND_TYPEROW. */
    Kind *parsed_type_param_kinds = NULL;
    if (ctors_start_idx < call->as.list.len &&
        call->as.list.items[ctors_start_idx]->tag == F_VEC) {
        Form *tp_form = call->as.list.items[ctors_start_idx];
        n_type_params = (uint8_t)tp_form->as.list.len;
        if (n_type_params > 0) {
            type_params = (const char **)arena_alloc(e->arena,
                              n_type_params * sizeof(char *));
            parsed_type_param_kinds = (Kind *)arena_alloc(e->arena,
                              n_type_params * sizeof(Kind));
            for (uint8_t pi = 0; pi < n_type_params; pi++) {
                Form *pf = tp_form->as.list.items[pi];
                if (pf->tag != F_SYM) {
                    diag_emit(DIAG_ERROR, pf->span,
                              "defdata: type parameter must be a symbol, e.g. a, ^f");
                    return NULL;
                }
                const Symbol *psym = pf->as.sym;
                /* L6 follow-up C: '^&name' marks a row-kinded ([*]) parameter. */
                if (psym->len > 2 && psym->name[0] == '^' && psym->name[1] == '&') {
                    const Symbol *bare = symtab_intern(e->st,
                        strslice(psym->name + 2, psym->len - 2));
                    type_params[pi] = bare->name;
                    parsed_type_param_kinds[pi] = KIND_TYPEROW;
                /* '^^name' marks a kind '* -> * -> *' (binary type constructor)
                 * parameter; '^name' marks a kind '* -> *' parameter.  Mirrors
                 * the defclass handling so a higher-kinded defdata param like
                 * `^f` in (defdata Fix [^f] (Roll (f (Fix f)))) stores the bare
                 * name `f` (so the body's `f` references resolve) and carries the
                 * arrow kind (so applying `f` is well-kinded). */
                } else if (psym->len > 2 && psym->name[0] == '^' && psym->name[1] == '^') {
                    const Symbol *bare = symtab_intern(e->st,
                        strslice(psym->name + 2, psym->len - 2));
                    type_params[pi] = bare->name;
                    parsed_type_param_kinds[pi] = KIND_ARROW2;
                } else if (psym->len > 1 && psym->name[0] == '^') {
                    const Symbol *bare = symtab_intern(e->st,
                        strslice(psym->name + 1, psym->len - 1));
                    type_params[pi] = bare->name;
                    parsed_type_param_kinds[pi] = KIND_ARROW;
                } else {
                    type_params[pi] = psym->name;
                    parsed_type_param_kinds[pi] = KIND_STAR;
                }
            }
        }
        ctors_start_idx++;
    }

    /* Phase RF0: allow re-elaboration of forward-declared stub types */
    bool is_forward_stub_adt = false;
    Binding *existing_adt_b = scope_lookup(e->scope, name);
    /* PS4: a record type from an earlier REPL/playground turn -- a lowered
     * defstruct -- is shadowed in scope by its own same-named constructor
     * function, so the lookup above lands on the function.  Re-elaborate over
     * the type binding underneath it, exactly as a defdata redefinition
     * re-elaborates over its (unshadowed) type binding. */
    if (existing_adt_b && existing_adt_b->type.kind != TY_ADT &&
        elab_prior_turn_global(e, existing_adt_b)) {
        for (uint32_t bi = e->global.n; bi-- > 0; ) {
            Binding *tb = e->global.bindings[bi];
            if (tb->name == name && tb->type.kind == TY_ADT &&
                elab_prior_turn_global(e, tb) &&
                elab_prior_turn_adt(e, tb->type.as.adt_.def)) {
                existing_adt_b = tb;
                break;
            }
        }
    }
    if (existing_adt_b) {
        /* Reuse the pre-registered stub ONLY when it is genuinely an ADT stub.
         * A forward-typed binding whose payload is not a TY_ADT (e.g. a stdlib
         * type of the same name shadowed by a forward entry) would otherwise be
         * dereferenced as `as.adt_.def` below -- a wild pointer / UBSan
         * misaligned-access crash.  Treat that as a redefinition. */
        const AdtDef *prior_def = existing_adt_b->type.kind == TY_ADT
                                ? existing_adt_b->type.as.adt_.def : NULL;
        /* A FILLED def (n_ctors > 0) is not a forward stub, whatever the
         * forward-type list says: the stdlib's own type pre-pass registered
         * `Option` / `Result` / `Cons` as forward types, so a user `defdata`
         * reusing one of those names passed elab_is_forward_type after the
         * stdlib had already filled the stub -- and re-elaborated the
         * stdlib's type IN PLACE, turning `(Option a)` into a kind-`*` type
         * and blaming stdlib/option.tur's Functor instance for it
         * (compiled-defdata-over-stdlib-type-rewrites-the-stdlib-type).
         * Mirror elab_defstruct's guard: a filled stdlib type is a
         * redefinition in a compile AND in a session (PS4); a filled type from
         * an earlier session turn is the one shape that legitimately
         * re-elaborates over its binding (PS4's reuse path), and a filled
         * type from THIS compile is "already defined". */
        if (prior_def && prior_def->n_ctors > 0) {
            bool from_stdlib = elab_file_is_stdlib(prior_def->origin_file_id);
            if (from_stdlib) {
                diag_emit(DIAG_ERROR, name_form->span,
                          "defdata: '%s' is already defined by an auto-loaded stdlib "
                          "module, and redefining it would change it for the stdlib "
                          "code built on it; pick a distinct name", name->name);
                return NULL;
            }
            if (!elab_prior_turn_adt(e, prior_def)) {
                diag_emit(DIAG_ERROR, name_form->span,
                          "defdata: '%s' is already defined by an earlier form; "
                          "pick a distinct name", name->name);
                return NULL;
            }
        }
        if (elab_is_forward_type(e, name) && existing_adt_b->type.kind == TY_ADT) {
            is_forward_stub_adt = true;
        } else {
            diag_emit(DIAG_ERROR, name_form->span,
                      "defdata: '%s' is already defined", name->name);
            return NULL;
        }
    }

    uint32_t n_ctors = call->as.list.len - ctors_start_idx;
    if (n_ctors == 0) {
        diag_emit(DIAG_ERROR, call->span,
                  "defdata: '%s' must have at least one constructor", name->name);
        return NULL;
    }

    /* Phase RF0: Allocate (or reuse forward stub) AdtDef and register BEFORE
     * parsing constructors so that self-referential and mutually-recursive
     * constructor field types resolve correctly. */
    AdtDef *def;
    Binding *adt_binding;
    Type adt_type;
    if (is_forward_stub_adt) {
        /* Reuse the pre-registered stub and fill it in */
        adt_binding = existing_adt_b;
        def = adt_binding->type.as.adt_.def;
        def->n_ctors = n_ctors;
        def->ctors = (CtorDef **)arena_alloc(e->arena, n_ctors * sizeof(CtorDef *));
        /* Zero the array: predicates reached MID-DEFINITION (a recursive field
         * probing adt_is_byvalue_product while this def's ctors are still being
         * filled) guard on `ctors[ci] == NULL`, and arena memory is not zeroed
         * -- the guard only ever worked on lucky fresh pages.  turi's
         * longer-lived arena handed back dirty memory and the guard read a
         * garbage CtorDef (SEGV in adt_sr1_sum_candidate). */
        memset(def->ctors, 0, n_ctors * sizeof(CtorDef *));
        def->is_copy = is_copy;
        def->is_heap = is_heap;
        def->is_linear = is_linear; /* LT4 (structdef-retirement slice 4) */
        def->no_auto_ctor = no_auto_ctor;
        def->needs_drop_glue = false;
        def->is_gadt = false;
        def->type_params = type_params;
        def->n_type_params = n_type_params;
        /* TP1: allocate Kind array initialised to KIND_STAR (TP4 refines).
         * L6 follow-up C: seed from parsed_type_param_kinds so `^&` is preserved. */
        if (n_type_params > 0) {
            Kind *tpk = (Kind *)arena_alloc(e->arena, n_type_params * sizeof(Kind));
            for (uint8_t pi = 0; pi < n_type_params; pi++) {
                tpk[pi] = parsed_type_param_kinds ? parsed_type_param_kinds[pi]
                                                  : KIND_STAR;
            }
            def->type_param_kinds = tpk;
        } else {
            def->type_param_kinds = NULL;
        }
        /* Refresh adt_type from the def so copy_kind reflects is_copy correctly.
         * The pre-pass stub was created with is_copy=false; now that we know the
         * real is_copy flag, regenerate the type and update the binding. */
        adt_type = type_adt(def);
        /* Phase G1/HKT: Apply KIND_ARROW fix so that the kind check can detect
         * when a parameterized defdata type is used in a kind-* slot. */
        adt_type.hkt_kind = kind_for_arity(n_type_params);
        adt_binding->type = adt_type;
        /* Already in global scope and elab registry from the pre-pass */
    } else {
        def = (AdtDef *)arena_alloc(e->arena, sizeof(AdtDef));
        memset(def, 0, sizeof(*def));  /* DS5: zero is_gadt and any future bool fields */
        def->name = name->name;
        def->n_ctors = n_ctors;
        def->ctors = (CtorDef **)arena_alloc(e->arena, n_ctors * sizeof(CtorDef *));
        /* Zero the array: predicates reached MID-DEFINITION (a recursive field
         * probing adt_is_byvalue_product while this def's ctors are still being
         * filled) guard on `ctors[ci] == NULL`, and arena memory is not zeroed
         * -- the guard only ever worked on lucky fresh pages.  turi's
         * longer-lived arena handed back dirty memory and the guard read a
         * garbage CtorDef (SEGV in adt_sr1_sum_candidate). */
        memset(def->ctors, 0, n_ctors * sizeof(CtorDef *));
        def->is_copy = is_copy;
        def->is_heap = is_heap;
        def->is_linear = is_linear; /* LT4 (structdef-retirement slice 4) */
        def->no_auto_ctor = no_auto_ctor;
        /* Phase RF1: store type parameters */
        def->type_params = type_params;
        def->n_type_params = n_type_params;
        /* TP1: allocate Kind array initialised to KIND_STAR (TP4 refines).
         * L6 follow-up C: seed from parsed_type_param_kinds so `^&` is preserved. */
        if (n_type_params > 0) {
            Kind *tpk = (Kind *)arena_alloc(e->arena, n_type_params * sizeof(Kind));
            for (uint8_t pi = 0; pi < n_type_params; pi++) {
                tpk[pi] = parsed_type_param_kinds ? parsed_type_param_kinds[pi]
                                                  : KIND_STAR;
            }
            def->type_param_kinds = tpk;
        } else {
            def->type_param_kinds = NULL;
        }

        /* Pre-register ADT type so constructors can reference it.
         * Phase G1/HKT: Set hkt_kind based on type-parameter count so that
         * elab_defgadt's belt-and-suspenders kind check can detect when a
         * parameterized type constructor is used in a kind-* argument slot. */
        adt_type = type_adt(def);
        adt_type.hkt_kind = kind_for_arity(n_type_params);
        adt_binding = binding_new(e, name, adt_type, false, true, name_form->span);
        scope_add(&e->global, adt_binding);
    }

    /* Record the owning compilation unit so the orphan-instance check can
     * credit instances over this ADT to its defining module (mirrors
     * StructDef.origin_file_id).  Set on both the fresh and reused-stub
     * paths to the real definition's file. */
    def->origin_file_id = name_form->span.file_id;

    /* Build a Symbol** view of the (bare) type-param names so that applied-type
     * constructor fields can be lowered through struct_field_type_from_form,
     * which matches type parameters by interned Symbol identity.  The names in
     * def->type_params are already interned, so re-interning yields the same
     * Symbol* the field forms carry. */
    const Symbol **tp_syms = NULL;
    if (n_type_params > 0) {
        tp_syms = (const Symbol **)arena_alloc(e->arena,
                                               n_type_params * sizeof(Symbol *));
        for (uint8_t pi = 0; pi < n_type_params; pi++) {
            tp_syms[pi] = symtab_intern(e->st,
                strslice(type_params[pi], (uint32_t)strlen(type_params[pi])));
        }
    }

    /* Parse each constructor.
     * `ci` is declared outside the loop so the `data_ctor_parse_error`
     * bail-out below can truncate `def->n_ctors` to the number of slots we
     * actually filled (every error path bails before `def->ctors[ci]` is
     * assigned, so `ci` is exactly the populated-slot count) -- the same
     * guard defgadt's ctor loop carries.  Without it, the pre-registered
     * binding advertises n_ctors slots whose pointers were never written
     * (arena memory is NOT zeroed), and later field-access elaboration in
     * the same failing compile dereferences the junk. */
    uint32_t ci = 0;
    for (; ci < n_ctors; ci++) {
        Form *ctor_form = call->as.list.items[ctors_start_idx + ci];
        if (ctor_form->tag != F_LIST) {
            diag_emit(DIAG_ERROR, ctor_form->span,
                      "defdata: constructor must be a list form (Ctor :T1 :T2 ...)");
            goto data_ctor_parse_error;
        }
        if (ctor_form->as.list.len < 1) {
            diag_emit(DIAG_ERROR, ctor_form->span,
                      "defdata: constructor form cannot be empty");
            goto data_ctor_parse_error;
        }
        Form *ctor_name_form = ctor_form->as.list.items[0];
        if (ctor_name_form->tag != F_SYM) {
            diag_emit(DIAG_ERROR, ctor_name_form->span,
                      "defdata: constructor name must be a symbol");
            goto data_ctor_parse_error;
        }
        const Symbol *ctor_name = ctor_name_form->as.sym;

        /* CONV-S0: detect record-style variant `(Ctor [a : T b : U ...])`. */
        bool is_record = ctor_form_is_record(ctor_form);
        Form *rec_vec = is_record ? ctor_form->as.list.items[1] : NULL;

        CtorDef *ctor = (CtorDef *)arena_alloc(e->arena, sizeof(CtorDef));
        ctor->name = ctor_name->name;
        ctor->adt = def;
        ctor->tag = ci;
        ctor->result_type_form = NULL; /* Phase G1: NULL for defdata */
        ctor->is_record = is_record;

        uint32_t n_fields;
        /* Names parallel to fields[], populated for record-style variants. */
        const Symbol **rec_field_names = NULL;
        /* Type forms (one per field) to resolve, gathered uniformly for both
         * styles so the field-resolution loop below is shared. */
        Form **field_type_forms = NULL;
        /* structdef-retirement slice 5 A1: optional `#fx{...}` effect-row form
         * per field (record style only), parallel to fields[]; NULL when the
         * field has no effect annotation. */
        Form **field_effect_forms = NULL;

        if (is_record) {
            /* Vector holds `name : type` pairs.  The reader collapses `: T`
             * into an F_TYPE_ANN node, so items alternate name, type, name, ...
             * Count name/type pairs first. */
            uint32_t n_items = rec_vec->as.list.len;
            if (n_items == 0) {
                diag_emit(DIAG_ERROR, rec_vec->span,
                          "defdata: record-style variant '%s' field list cannot be empty",
                          ctor_name->name);
                goto data_ctor_parse_error;
            }
            /* Walk name/type pairs with a cursor rather than fixed `fi*2`
             * indexing: structdef-retirement slice 5 A1 -- a `fn`-typed field may
             * be followed by a trailing `#fx{...}` effect-row F_MAP (the shape a
             * lowered `defstruct` capability field `[run : fn #fx{Write}]`
             * produces), which is NOT a name/type pair.  Attach it to that field
             * and skip it, so the record parser accepts the effect annotation the
             * struct path already understood. n_items is an upper bound on
             * n_fields. */
            rec_field_names = (const Symbol **)arena_alloc(e->arena,
                                  n_items * sizeof(Symbol *));
            field_type_forms = (Form **)arena_alloc(e->arena,
                                  n_items * sizeof(Form *));
            field_effect_forms = (Form **)arena_alloc(e->arena,
                                  n_items * sizeof(Form *));
            uint32_t nf = 0, ci = 0;
            while (ci < n_items) {
                Form *name_f = rec_vec->as.list.items[ci++];
                if (name_f->tag != F_SYM) {
                    diag_emit(DIAG_ERROR, name_f->span,
                              "defdata: record-style variant '%s' expected a field "
                              "name symbol", ctor_name->name);
                    goto data_ctor_parse_error;
                }
                if (ci >= n_items) {
                    diag_emit(DIAG_ERROR, rec_vec->span,
                              "defdata: record-style variant '%s' field list must be "
                              "[name : type ...] pairs", ctor_name->name);
                    goto data_ctor_parse_error;
                }
                Form *type_f = rec_vec->as.list.items[ci++];
                /* Unwrap `: T` (F_TYPE_ANN) to the bare type form. */
                if (type_f->tag == F_TYPE_ANN)
                    type_f = type_f->as.list.items[0];
                rec_field_names[nf] = name_f->as.sym;
                field_type_forms[nf] = type_f;
                field_effect_forms[nf] = NULL;
                /* A1: a trailing F_MAP after a `fn`/`c-fn` field type is that
                 * field's effect row (mirrors the struct path, elab_structs.c
                 * ~1582). Only consume it for a fn field so a stray F_MAP after a
                 * non-fn field still surfaces as the missing-field-name error. */
                bool type_is_fn =
                    (type_f->tag == F_SYM &&
                     (type_f->as.sym == e->sym_fn || type_f->as.sym == e->sym_c_fn)) ||
                    (type_f->tag == F_LIST && type_f->as.list.len >= 1 &&
                     type_f->as.list.items[0]->tag == F_SYM &&
                     (type_f->as.list.items[0]->as.sym == e->sym_fn ||
                      type_f->as.list.items[0]->as.sym == e->sym_c_fn));
                if (type_is_fn && ci < n_items &&
                    rec_vec->as.list.items[ci]->tag == F_MAP) {
                    field_effect_forms[nf] = rec_vec->as.list.items[ci++];
                }
                nf++;
            }
            n_fields = nf;
        } else {
            n_fields = ctor_form->as.list.len - 1;
            if (n_fields > 0) {
                field_type_forms = (Form **)arena_alloc(e->arena,
                                       n_fields * sizeof(Form *));
                for (uint32_t fi = 0; fi < n_fields; fi++)
                    field_type_forms[fi] = ctor_form->as.list.items[1 + fi];
            }
        }

        ctor->n_fields = n_fields;
        ctor->fields = n_fields > 0
            ? (CtorField *)arena_alloc(e->arena, n_fields * sizeof(CtorField))
            : NULL;
        /* arena_alloc does not zero, and resolve_ctor_field writes
         * `drop_inner_def` only on the nested-owning-aggregate branch -- so
         * every other field kept the arena's 0xbe poison and the drop/walk glue
         * emitter dereferenced it.  Latent for as long as the emitter only
         * walked ctor 0; it surfaced when the sum-type glue fix widened that
         * walk to every ctor.  Zero the whole array once rather than chase
         * per-field defaults. */
        if (ctor->fields) memset(ctor->fields, 0, n_fields * sizeof(CtorField));
        /* F6-1 (cross-plan-followups): stash the raw field-type forms so pattern
         * extraction at match time can recover the declared ADT/struct type. */
        ctor->field_forms = n_fields > 0
            ? (const struct Form **)arena_alloc(e->arena, n_fields * sizeof(const Form *))
            : NULL;

        /* Parse field types (shared between positional and record styles). */
        for (uint32_t fi = 0; fi < n_fields; fi++) {
            if (!resolve_ctor_field(e, def, ctor, fi, field_type_forms[fi],
                                    tp_syms, n_type_params, is_record)) {
                goto data_ctor_parse_error;
            }
            ctor->fields[fi].name = rec_field_names ? rec_field_names[fi]->name : NULL;

            /* structdef-retirement slice 5 A1: parse the optional `#fx{...}`
             * effect-row annotation collected for a `fn`-typed field, mirroring
             * the struct path (elab_structs.c ~1596).  This is what keeps
             * capability-field effect tracking sound after a `defstruct` with a
             * `[run : fn #fx{Eff}]` field lowers to this record ADT: effect_check
             * reads the row off the CtorField when a `(.run v)` call fires. */
            ctor->fields[fi].effect_row = NULL;
            if (field_effect_forms && field_effect_forms[fi]) {
                Form *row_form = field_effect_forms[fi];
                warn_legacy_fx_row(row_form);
                uint8_t n_sym = (uint8_t)row_form->as.list.len;
                const Symbol **syms = (const Symbol **)arena_alloc(e->arena,
                                        (n_sym ? n_sym : 1) * sizeof(Symbol *));
                uint8_t n_valid = 0;
                for (uint32_t j = 0; j < row_form->as.list.len; j++) {
                    Form *item = row_form->as.list.items[j];
                    if (item->tag == F_SYM) syms[n_valid++] = item->as.sym;
                }
                ctor->fields[fi].effect_row =
                    effect_row_unresolved(e->arena, syms, n_valid, row_form->span);
            }

            /* CONV-S5: a :copy ADT requires every variant's payload to be
             * copy-compatible.  A non-copy field (rc/ref/weak/lref ownership)
             * makes the value move-only, which contradicts :copy.  Reject it
             * here, pinpointing the offending field and variant. */
            if (def->is_copy &&
                !typekind_is_copy_for_struct(ctor->fields[fi].kind)) {
                /* structdef-retirement slice 5 DS-A3: a lowered `defstruct` with a
                 * linear (`lref`) field reaches here now that borrow-family field
                 * forms lower.  Reproduce the struct path's precise TUR-E0102
                 * "cannot copy linear field" diagnostic (elab_structs.c ~1583)
                 * for a linear field rather than the generic non-copy message,
                 * so the surface diagnostic is unchanged by the lowering. */
                if (typekind_default_copy_kind(ctor->fields[fi].kind) == CK_LINEAR &&
                    ctor->fields[fi].name) {
                    diag_emit_with_code(DIAG_ERROR, field_type_forms[fi]->span,
                                        TUR_E0102_LINEAR_COPY,
                                        "cannot copy linear field '%s' -- "
                                        "linear values cannot appear in :copy structs",
                                        ctor->fields[fi].name);
                    goto data_ctor_parse_error;
                }
                const char *fdesc = ctor->fields[fi].name;
                if (fdesc) {
                    diag_emit(DIAG_ERROR, field_type_forms[fi]->span,
                              "defdata: :copy type '%s' cannot contain non-copy "
                              "field '%s' of variant '%s'",
                              def->name, fdesc, ctor->name);
                } else {
                    diag_emit(DIAG_ERROR, field_type_forms[fi]->span,
                              "defdata: :copy type '%s' cannot contain non-copy "
                              "field %u of variant '%s'",
                              def->name, fi, ctor->name);
                }
                goto data_ctor_parse_error;
            }
        }

        def->ctors[ci] = ctor;

        /* Register constructor as a global binding.
         * 0-arg constructor: TY_ADT binding (it IS a value).
         * N-arg constructor: TY_FN binding (call it like a function).
         *
         * structdef-retirement slice 2 (CTOR-V0): a `:no-auto-ctor` def still binds
         * the value-namespace constructor (make-struct rewrites `(make-struct Name
         * ...)` to the ctor call `(Name ...)` and relies on it), but a DIRECT
         * `(Name ...)` call is rejected in elab_call -- see the no_auto_ctor guard
         * there, which fires unless the call came from the make-struct rewrite. */
        if (n_fields == 0) {
            /* 0-arg: register as TY_ADT so calls to (Red) work */
            Binding *cb = binding_new(e, ctor_name, adt_type, false, true,
                                      ctor_name_form->span);
            scope_add(&e->global, cb);
        } else {
            /* N-arg: register as TY_FN */
            TypeKind arg_kinds[MAX_FN_ARITY];
            uint8_t arity = (uint8_t)(n_fields > MAX_FN_ARITY ? MAX_FN_ARITY : n_fields);
            bool any_tyvar = false;
            for (uint8_t fi = 0; fi < arity; fi++) {
                arg_kinds[fi] = ctor->fields[fi].kind;
                if (ctor->fields[fi].full_type && ctor->fields[fi].full_type->kind == TY_TYVAR)
                    any_tyvar = true;
            }
            Type fn_type = type_fn(arg_kinds, arity, TY_ADT);
            /* TS4P1: If any field carries a type-variable full_type, attach the
             * arg_full_types array so elab_call_fn can detect the polymorphic
             * constructor and accept concrete types where TY_INT is expected. */
            if (any_tyvar) {
                Type **aft = (Type **)arena_alloc(e->arena, arity * sizeof(Type *));
                for (uint8_t fi = 0; fi < arity; fi++) {
                    aft[fi] = ctor->fields[fi].full_type; /* may be NULL for non-tyvar fields */
                }
                fn_type.as.fn.arg_full_types = aft;
            }
            Binding *cb = binding_new(e, ctor_name, fn_type, false, true,
                                      ctor_name_form->span);
            scope_add(&e->global, cb);
        }
    }

    /* Register ADT in elab registry (skip if it was a forward stub -- already registered) */
    if (!is_forward_stub_adt) {
        elab_register_adt_def(e, def);
    }

    /* TP4: refine type_param_kinds based on CtorField.full_type usage */
    infer_type_param_kinds(def);

    /* Return EX_DEFDATA node */
    Expr *out = expr_new(e->arena, EX_DEFDATA, TYPE_NIL, call->span);
    out->as.defdata_.def = def;
    out->as.defdata_.binding = adt_binding;
    def->from_stdlib = e->in_stdlib_load;   /* r7rs-programs-compile-slowly */
    return out;

data_ctor_parse_error:
    /* A constructor failed to parse after the AdtDef was pre-registered in
     * scope.  The slots `[ci, n_ctors)` were never written -- and arena
     * memory is NOT zeroed, so they hold junk, not NULL -- while `def->
     * n_ctors` advertises the full declared count.  Later field-access /
     * match elaboration in the same (already failing) compile would then
     * dereference the junk (the ecs sized-* suite crash).  Truncate to the
     * slots actually filled, mirroring defgadt's ctor_parse_error. */
    def->n_ctors = ci;
    return NULL;
}

/* Phase G2: Look up a type parameter name in the current skolem environment.
 * Returns TY_UNKNOWN if not found. */
TypeKind gadt_skolem_lookup(const SkolemEnv *env, const char *name) {
    if (!env) return TY_UNKNOWN;
    for (uint8_t i = 0; i < env->n; i++) {
        if (strcmp(env->bindings[i].name, name) == 0)
            return env->bindings[i].kind;
    }
    return TY_UNKNOWN;
}

/* TP3: Return the full Type* stored in the skolem binding for name, or NULL.
 * Use when the concrete type is an ADT/struct and the flat TypeKind is insufficient. */
static const Type *gadt_skolem_lookup_type(const SkolemEnv *env, const char *name) {
    if (!env) return NULL;
    for (uint8_t i = 0; i < env->n; i++) {
        if (strcmp(env->bindings[i].name, name) == 0)
            return env->bindings[i].full_type;
    }
    return NULL;
}

/* Phase G2: Resolve a type form to a full Type using the current skolem env.
 * For primitive symbols → concrete Type.
 * For type variable names → look up in senv; TY_TYVAR if unresolved.
 * For ADT reference forms `(AdtName ...)` → look up ADT in global scope.
 * Falls back to TY_INT (opaque int64_t) for unknown forms.
 */
static Type gadt_resolve_type_from_form(Elab *e, const AdtDef *gadt, const Form *f,
                                         const SkolemEnv *senv) {
    if (!f) return type_from_kind(TY_INT);

    if (f->tag == F_SYM) {
        const char *n = f->as.sym->name;
        /* Primitive types */
        if (strcmp(n, "int")    == 0) return type_from_kind(TY_INT);
        if (strcmp(n, "bool")   == 0) return type_from_kind(TY_BOOL);
        if (strcmp(n, "float")  == 0) return type_from_kind(TY_FLOAT);
        if (strcmp(n, "cstr")   == 0) return type_from_kind(TY_CSTR);
        if (strcmp(n, "ptr")    == 0) return type_from_kind(TY_PTR_VOID);
        if (strcmp(n, "int8")   == 0) return type_from_kind(TY_INT8);
        if (strcmp(n, "int16")  == 0) return type_from_kind(TY_INT16);
        if (strcmp(n, "int32")  == 0) return type_from_kind(TY_INT32);
        if (strcmp(n, "int64")  == 0) return type_from_kind(TY_INT64);
        if (strcmp(n, "uint8")  == 0) return type_from_kind(TY_UINT8);
        if (strcmp(n, "uint16") == 0) return type_from_kind(TY_UINT16);
        if (strcmp(n, "uint32") == 0) return type_from_kind(TY_UINT32);
        if (strcmp(n, "uint64") == 0) return type_from_kind(TY_UINT64);
        if (strcmp(n, "float32")== 0) return type_from_kind(TY_FLOAT32);
        if (strcmp(n, "float64")== 0) return type_from_kind(TY_FLOAT64);
        /* Type variable: look up in skolem env — prefer full Type if available (TP3) */
        const Type *full_resolved = gadt_skolem_lookup_type(senv, n);
        if (full_resolved) return *full_resolved;
        TypeKind resolved = gadt_skolem_lookup(senv, n);
        if (resolved != TY_UNKNOWN) return type_from_kind(resolved);
        /* Unresolved type variable → anonymous TY_TYVAR (name=NULL signals skolem escape) */
        (void)gadt; /* suppress unused warning */
        return type_tyvar_named(NULL);
    }

    if (f->tag == F_LIST && f->as.list.len >= 1) {
        /* Possibly an ADT reference: (AdtName type-args...) */
        Form *head = f->as.list.items[0];
        if (head->tag == F_SYM) {
            Binding *b = scope_lookup(e->scope, head->as.sym);
            if (!b) b = scope_lookup(&e->global, head->as.sym);
            if (b && b->type.kind == TY_ADT && b->type.as.adt_.def) {
                return b->type; /* TY_ADT with the def pointer */
            }
            /* Phase HKT: kind-variable application (f a) where f : * -> *.
             * Return an anonymous TY_TYVAR so the arm body is accepted as
             * int64_t-sized (same runtime repr as TY_ADT/TY_APP). */
            if (b && b->type.kind == TY_TYVAR && b->type.hkt_kind == KIND_ARROW) {
                return type_tyvar_named(NULL);
            }
        }
    }

    return type_from_kind(TY_INT); /* fallback: opaque int64_t */
}

/* TP4: Infer Kind for each type parameter of an ADT/GADT from the declared
 * CtorField.full_type nodes populated by TP1/TP2.
 *
 * Rules (applied across all constructors):
 *   - If a param appears as a direct TY_TYVAR (e.g. field `a`) → KIND_STAR.
 *   - If a param appears as the fn-side of a TY_APP node           → KIND_ARROW.
 *   - Default (param never mentioned in full_type)                 → KIND_STAR.
 *
 * The inferred kinds are written into def->type_param_kinds (allocated by TP1/TP2).
 */
static void infer_type_param_kinds(AdtDef *def) {
    if (!def->type_param_kinds || def->n_type_params == 0) return;

    for (uint32_t ci = 0; ci < def->n_ctors; ci++) {
        const CtorDef *ctor = def->ctors[ci];
        if (!ctor) continue;
        for (uint32_t fi = 0; fi < ctor->n_fields; fi++) {
            const Type *ft = ctor->fields[fi].full_type;
            if (!ft) continue;
            if (ft->kind == TY_TYVAR && ft->as.tyvar_.name) {
                /* Direct param use → KIND_STAR */
                for (uint8_t pi = 0; pi < def->n_type_params; pi++) {
                    if (strcmp(def->type_params[pi], ft->as.tyvar_.name) == 0) {
                        /* Only upgrade if not already KIND_ARROW */
                        if (def->type_param_kinds[pi] == KIND_STAR)
                            def->type_param_kinds[pi] = KIND_STAR; /* no-op; keep default */
                        break;
                    }
                }
            } else if (ft->kind == TY_APP) {
                /* Function-side of TY_APP — param used as a type constructor */
                const Type *fn_side = ft->as.app.fn;
                if (fn_side && fn_side->kind == TY_TYVAR && fn_side->as.tyvar_.name) {
                    for (uint8_t pi = 0; pi < def->n_type_params; pi++) {
                        if (strcmp(def->type_params[pi], fn_side->as.tyvar_.name) == 0) {
                            def->type_param_kinds[pi] = KIND_ARROW;
                            break;
                        }
                    }
                }
            }
        }
    }
}



/* gadt-length-index-not-enforced: can a GADT constructor's result-type index
 * form `f` (as written in `(VCons int (Vec n) : (Vec (Succ n)))`) match the
 * scrutinee's index type `t`?  A type parameter of `def` is flexible; a
 * primitive name matches its kind; a type name (`Zero`, a defopaque / defdata
 * / defgadt in scope) matches that def; an application `(Succ n)` matches an
 * application of the same head whose arguments match position by position.
 *
 * Every shape this cannot read answers "maybe" (true): a size term
 * (`(Static n)`, `(Add a b)` -- SZ6's own machinery), an unknown symbol, a
 * scrutinee index that is itself a tyvar.  So the only constructor the
 * exhaustiveness check drops on this answer is one whose index PROVABLY
 * differs from the scrutinee's -- `VNil : (Vec Zero)` against a scrutinee
 * typed `(Vec (Succ n))`.  That is what lets a `head` over a non-empty vector
 * omit the `VNil` arm. */
/* Primitive index classes for the "provably differs" test.  Integer widths are
 * one class (an `int` index and a TY_INT64 scrutinee are not a contradiction
 * the checker would stand behind), floats another, bool and cstr their own.
 * 0 = not a primitive. */
static int gadt_index_prim_class(TypeKind k) {
    switch (k) {
        case TY_INT: case TY_INT8: case TY_INT16: case TY_INT32: case TY_INT64:
        case TY_UINT8: case TY_UINT16: case TY_UINT32: case TY_UINT64:
            return 1;
        case TY_FLOAT: case TY_FLOAT32: case TY_FLOAT64:
            return 2;
        case TY_BOOL: return 3;
        case TY_CSTR: return 4;
        default: return 0;
    }
}

static bool gadt_index_form_may_match(Elab *e, const AdtDef *def,
                                      const Form *f, const Type *t) {
    if (!f || !t) return true;
    if (t->kind == TY_TYVAR || t->kind == TY_UNKNOWN || t->kind == TY_ANY)
        return true;
    if (f->tag == F_SYM) {
        const char *n = f->as.sym->name;
        for (uint8_t pi = 0; pi < def->n_type_params; pi++)
            if (def->type_params[pi] && strcmp(def->type_params[pi], n) == 0)
                return true;
        static const struct { const char *name; TypeKind kind; } prims[] = {
            { "int", TY_INT }, { "bool", TY_BOOL }, { "float", TY_FLOAT },
            { "cstr", TY_CSTR }, { "int8", TY_INT8 }, { "int16", TY_INT16 },
            { "int32", TY_INT32 }, { "int64", TY_INT64 }, { "uint8", TY_UINT8 },
            { "uint16", TY_UINT16 }, { "uint32", TY_UINT32 },
            { "uint64", TY_UINT64 }, { "float32", TY_FLOAT32 },
            { "float64", TY_FLOAT64 },
        };
        for (size_t i = 0; i < sizeof prims / sizeof prims[0]; i++) {
            if (strcmp(prims[i].name, n) != 0) continue;
            /* Two primitives of different classes differ; a primitive
             * against a named type differs; anything else is "maybe". */
            int fc = gadt_index_prim_class(prims[i].kind);
            int tc = gadt_index_prim_class(t->kind);
            if (tc) return fc == tc;
            return !(t->kind == TY_ADT || t->kind == TY_APP);
        }
        Binding *tb = scope_lookup(e->scope, f->as.sym);
        if (!tb) tb = scope_lookup(&e->global, f->as.sym);
        if (tb && tb->type.kind == TY_ADT && tb->type.as.adt_.def) {
            if (t->kind == TY_ADT) return t->as.adt_.def == tb->type.as.adt_.def;
            /* A nullary name against an application of another head is a
             * different index.  Against a primitive it is left "maybe": a
             * defopaque's carrier IS a primitive, and a resolution path that
             * collapsed the name to its base must not drop an arm. */
            if (t->kind == TY_APP) return type_adt_app_def(t) == tb->type.as.adt_.def;
            return true;
        }
        return true;
    }
    if (f->tag == F_LIST && f->as.list.len >= 1 &&
            f->as.list.items[0]->tag == F_SYM) {
        const Form *hd = f->as.list.items[0];
        const char *op = hd->as.sym->name;
        if (strcmp(op, "Static") == 0 || strcmp(op, "Add") == 0 ||
                strcmp(op, "Mul") == 0)
            return true;                       /* SZ6 size index: not ours */
        Binding *tb = scope_lookup(e->scope, hd->as.sym);
        if (!tb) tb = scope_lookup(&e->global, hd->as.sym);
        if (!tb || tb->type.kind != TY_ADT || !tb->type.as.adt_.def) return true;
        const AdtDef *hdef = tb->type.as.adt_.def;
        if (t->kind == TY_ADT) return t->as.adt_.def == hdef;
        if (t->kind != TY_APP) return true;     /* not classified: "maybe" */
        if (type_adt_app_def(t) != hdef) return false;
        uint32_t n_args = f->as.list.len - 1;
        if (hdef->n_type_params == 0 || n_args != hdef->n_type_params) return true;
        Type *targs = (Type *)malloc(hdef->n_type_params * sizeof(Type));
        if (!targs) return true;
        bool ok = true;
        if (elab_adt_type_extract_args(t, hdef, targs)) {
            for (uint32_t i = 0; i < n_args && ok; i++)
                ok = gadt_index_form_may_match(e, def, f->as.list.items[1 + i],
                                               &targs[i]);
        }
        free(targs);
        return ok;
    }
    return true;
}

/* Is GADT constructor `c` reachable from a scrutinee of type `scrut`?  False
 * only when the scrutinee is an application of the GADT with every index
 * position readable and some position provably mismatching the constructor's
 * declared result index.  An unannotated / bare-`Vec` scrutinee carries no
 * instantiation, so every constructor stays reachable (today's behaviour). */
static bool gadt_ctor_reachable_from_scrutinee(Elab *e, const AdtDef *adt,
                                               const CtorDef *c, const Type *scrut) {
    if (!adt->is_gadt || adt->n_type_params == 0 || !c || !c->result_type_form)
        return true;
    if (!scrut || scrut->kind != TY_APP) return true;
    const Form *rt = c->result_type_form;
    if (rt->tag != F_LIST || rt->as.list.len < 2) return true;
    Type *sargs = (Type *)malloc(adt->n_type_params * sizeof(Type));
    if (!sargs) return true;
    bool reachable = true;
    if (elab_adt_type_extract_args(scrut, adt, sargs)) {
        uint32_t n_args = rt->as.list.len - 1;
        for (uint32_t i = 0; i < n_args && i < adt->n_type_params && reachable; i++)
            reachable = gadt_index_form_may_match(e, adt, rt->as.list.items[1 + i],
                                                  &sargs[i]);
    }
    free(sargs);
    return reachable;
}

/* Phase G2: Build a SkolemEnv for a GADT constructor arm.
 * Parses the constructor's result_type_form (e.g. "(Expr int)") against
 * the ADT's type_params (e.g. ["a"]) to produce concrete bindings
 * such as {a → TY_INT}.  Primitive-type args are mapped to their TypeKind;
 * TP3: ADT/struct-type args are now also resolved and stored with their full Type. */
static void gadt_build_skolem_env(Elab *e, SkolemEnv *out, const AdtDef *def,
                                   const CtorDef *ctor) {
    out->n = 0;
    if (!ctor->result_type_form || def->n_type_params == 0) return;

    const Form *rt = ctor->result_type_form;
    /* rt should be (AdtName arg0 arg1 ...) */
    if (rt->tag != F_LIST || rt->as.list.len < 2) return;

    /* arg i is at items[1+i] */
    uint32_t n_args = rt->as.list.len - 1;
    uint32_t n_bind = (n_args < def->n_type_params) ? n_args : def->n_type_params;

    for (uint32_t i = 0; i < n_bind && out->n < MAX_SKOLEM_BINDINGS; i++) {
        Form *arg = rt->as.list.items[1 + i];
        const char *param_name = def->type_params[i];
        TypeKind k = TY_UNKNOWN;
        Type *full_type = NULL;
        /* SZ6: capture a type-level size index when this argument is a Size
         * expression `(Static n)`, `(Add a b)`, or `(Mul a b)`.  Detected
         * structurally by the head symbol, so ordinary ADT applications like
         * `(Foo int)` are never mistaken for size terms. */
        SizeTerm *size_index = NULL;
        if (arg->tag == F_LIST && arg->as.list.len >= 1 &&
                arg->as.list.items[0]->tag == F_SYM) {
            const char *op = arg->as.list.items[0]->as.sym->name;
            if (strcmp(op, "Static") == 0 || strcmp(op, "Add") == 0 ||
                    strcmp(op, "Mul") == 0) {
                size_index = size_term_from_form(e->arena, arg, NULL, NULL);
            }
        }

        if (arg->tag == F_SYM) {
            const char *an = arg->as.sym->name;
            if (strcmp(an, "int")    == 0) k = TY_INT;
            else if (strcmp(an, "bool")   == 0) k = TY_BOOL;
            else if (strcmp(an, "float")  == 0) k = TY_FLOAT;
            else if (strcmp(an, "cstr")   == 0) k = TY_CSTR;
            else if (strcmp(an, "int8")   == 0) k = TY_INT8;
            else if (strcmp(an, "int16")  == 0) k = TY_INT16;
            else if (strcmp(an, "int32")  == 0) k = TY_INT32;
            else if (strcmp(an, "int64")  == 0) k = TY_INT64;
            else if (strcmp(an, "uint8")  == 0) k = TY_UINT8;
            else if (strcmp(an, "uint16") == 0) k = TY_UINT16;
            else if (strcmp(an, "uint32") == 0) k = TY_UINT32;
            else if (strcmp(an, "uint64") == 0) k = TY_UINT64;
            else if (strcmp(an, "float32")== 0) k = TY_FLOAT32;
            else if (strcmp(an, "float64")== 0) k = TY_FLOAT64;
            else {
                /* TP3: may be a named ADT/struct type — resolve via scope */
                Binding *tb = scope_lookup(e->scope, arg->as.sym);
                if (!tb) tb = scope_lookup(&e->global, arg->as.sym);
                if (tb && (tb->type.kind == TY_ADT || tb->type.kind == TY_STRUCT)) {
                    k = tb->type.kind;
                    full_type = (Type *)arena_alloc(e->arena, sizeof(Type));
                    *full_type = tb->type;
                }
                /* else: type variable in return pos — leave unresolved */
            }
        }
        /* TP3: List form like (Foo int) — ADT/struct application; resolve head */
        else if (arg->tag == F_LIST && arg->as.list.len >= 1) {
            Form *hd = arg->as.list.items[0];
            if (hd->tag == F_SYM) {
                Binding *tb = scope_lookup(e->scope, hd->as.sym);
                if (!tb) tb = scope_lookup(&e->global, hd->as.sym);
                if (tb && (tb->type.kind == TY_ADT || tb->type.kind == TY_STRUCT)) {
                    k = tb->type.kind;
                    full_type = (Type *)arena_alloc(e->arena, sizeof(Type));
                    *full_type = tb->type;
                } else {
                    k = TY_INT; /* opaque ADT reference fallback */
                }
            } else {
                k = TY_INT;
            }
        }

        if (k != TY_UNKNOWN || size_index) {
            out->bindings[out->n].name       = param_name;
            out->bindings[out->n].kind       = k;
            out->bindings[out->n].full_type  = full_type;
            out->bindings[out->n].size_index = size_index;
            out->n++;
        }
    }
}

/* Phase G1: Map a simple type form to a TypeKind for codegen.
 * Handles primitive names (int, bool, cstr, etc.) and falls back to
 * TY_INT (opaque int64_t) for ADT references and type variables. */
static TypeKind gadt_field_typekind_from_form(const Form *f) {
    if (!f) return TY_INT;
    if (f->tag == F_SYM) {
        const char *n = f->as.sym->name;
        if (strcmp(n, "int")   == 0) return TY_INT;
        if (strcmp(n, "bool")  == 0) return TY_BOOL;
        if (strcmp(n, "float") == 0) return TY_FLOAT;
        if (strcmp(n, "cstr")  == 0) return TY_CSTR;
        if (strcmp(n, "ptr")   == 0) return TY_PTR_VOID;
        if (strcmp(n, "int8")  == 0) return TY_INT8;
        if (strcmp(n, "int16") == 0) return TY_INT16;
        if (strcmp(n, "int32") == 0) return TY_INT32;
        if (strcmp(n, "int64") == 0) return TY_INT64;
        if (strcmp(n, "uint8") == 0) return TY_UINT8;
        if (strcmp(n, "uint16")== 0) return TY_UINT16;
        if (strcmp(n, "uint32")== 0) return TY_UINT32;
        if (strcmp(n, "uint64")== 0) return TY_UINT64;
        if (strcmp(n, "float32")== 0) return TY_FLOAT32;
        if (strcmp(n, "float64")== 0) return TY_FLOAT64;
        /* Type variable or unknown type — opaque int64_t */
        return TY_INT;
    }
    /* List form like (Expr int) — ADT reference, opaque int64_t */
    return TY_INT;
}

/* TP1/TP2: If ft_form is a bare symbol matching a declared type parameter,
 * return an arena-allocated TY_TYVAR Type* carrying the parameter name.
 * Returns NULL if the form is not a type-param reference (caller falls back
 * to its normal field-type resolution path).
 * The C-level storage kind remains TY_INT; full_type is elaboration-only. */
static Type *adt_field_type_from_form(Arena *arena, const Form *ft_form,
                                       const char **type_params,
                                       uint8_t n_type_params) {
    if (!ft_form || ft_form->tag != F_SYM) return NULL;
    const char *pname = ft_form->as.sym->name;
    for (uint8_t pi = 0; pi < n_type_params; pi++) {
        if (strcmp(type_params[pi], pname) == 0) {
            Type *tv = (Type *)arena_alloc(arena, sizeof(Type));
            *tv = type_tyvar_named(pname);
            return tv;
        }
    }
    return NULL;
}

/* Phase G1: defgadt — define a GADT (Generalized Algebraic Data Type).
 * Syntax: (defgadt Name [type-params...]
 *           (Ctor1 : return-type)
 *           (Ctor2 FieldType1 FieldType2 : return-type)
 *           ...)
 *
 * The ':' separator is a bare F_SYM(":") token.
 * Field types are forms appearing between the constructor name and ':'.
 * The return-type annotation is stored on the CtorDef for future G2 use.
 * Codegen is identical to defdata (tagged union).
 */
Expr *elab_defgadt(Elab *e, const Form *call) {
    /* range-gadt-typeclass-migration-plan A1: GADTs are enabled by default,
     * so the former -Xgadt gate is gone. */
    if (call->as.list.len < 3) {
        diag_emit(DIAG_ERROR, call->span,
                  "defgadt requires a name, type-params, and constructors: "
                  "(defgadt Name [params] (Ctor : return-type) ...)");
        return NULL;
    }

    /* Parse name */
    Form *name_form = call->as.list.items[1];
    if (name_form->tag != F_SYM) {
        diag_emit(DIAG_ERROR, name_form->span, "defgadt name must be a symbol");
        return NULL;
    }
    const Symbol *name = name_form->as.sym;

    /* Parse type parameters — must be a vector [a b c] */
    uint32_t ctors_start_idx = 2;
    uint8_t n_type_params = 0;
    const char **type_params = NULL;
    /* L6 follow-up C: parallel kind array, so a `^&name` row-kinded parameter
     * carries KIND_TYPEROW through to def->type_param_kinds. Without it, the
     * caller-side kind check at (MyGADT #row{...}) sees KIND_STAR and rejects
     * the row as a non-row argument. */
    Kind *parsed_type_param_kinds = NULL;
    if (call->as.list.len >= 3 && call->as.list.items[2]->tag == F_VEC) {
        Form *params_form = call->as.list.items[2];
        n_type_params = (uint8_t)params_form->as.list.len;
        if (n_type_params > 0) {
            type_params = (const char **)arena_alloc(e->arena,
                                                      n_type_params * sizeof(const char *));
            parsed_type_param_kinds = (Kind *)arena_alloc(e->arena,
                                                      n_type_params * sizeof(Kind));
            for (uint8_t i = 0; i < n_type_params; i++) {
                Form *pf = params_form->as.list.items[i];
                if (pf->tag != F_SYM) {
                    diag_emit(DIAG_ERROR, pf->span,
                              "defgadt: type parameter must be a symbol");
                    return NULL;
                }
                const Symbol *psym = pf->as.sym;
                /* L6 follow-up C: '^&name' marks a row-kinded ([*]) parameter.
                 * Strip the `^&` so body and call-site usages reference the
                 * bare name; stash the row kind in parsed_type_param_kinds. */
                if (psym->len > 2 && psym->name[0] == '^' && psym->name[1] == '&') {
                    const Symbol *bare = symtab_intern(e->st,
                        strslice(psym->name + 2, psym->len - 2));
                    type_params[i] = bare->name;
                    parsed_type_param_kinds[i] = KIND_TYPEROW;
                } else {
                    type_params[i] = psym->name;
                    parsed_type_param_kinds[i] = KIND_STAR;
                }
            }
        }
        ctors_start_idx = 3;
    }

    /* Check for optional :copy / :move annotation after the type-param vector.
     * Mirrors defdata: a GADT marked :copy opts out of affine move tracking so
     * its values can be read repeatedly (e.g. shared range bounds). */
    bool is_copy = false;
    if (ctors_start_idx < call->as.list.len) {
        Form *kw_form = call->as.list.items[ctors_start_idx];
        if (kw_form->tag == F_KEYWORD && kw_form->as.sym == e->kw_copy) {
            is_copy = true;
            ctors_start_idx++;
        } else if (kw_form->tag == F_KEYWORD && kw_form->as.sym == e->kw_move) {
            is_copy = false;
            ctors_start_idx++;
        }
    }

    if (call->as.list.len <= ctors_start_idx) {
        diag_emit(DIAG_ERROR, call->span,
                  "defgadt: '%s' must have at least one constructor", name->name);
        return NULL;
    }

    /* Phase RF0: allow re-elaboration of forward-declared stub types.
     * MF4: only reuse the existing binding as a stub when its kind matches
     * the kind we're elaborating (TY_ADT here).  A same-name struct
     * binding (TY_STRUCT) does not block the GADT -- per the MF4 design,
     * GADTs and structs occupy separate namespaces and type-annotation
     * lookups prefer the GADT.  Emit a one-shot warning at the defgadt
     * span so the shadowing is visible. */
    bool is_forward_stub_gadt = false;
    Binding *existing_gadt_b = scope_lookup(e->scope, name);
    if (existing_gadt_b) {
        bool same_kind_forward =
            (existing_gadt_b->type.kind == TY_ADT) && elab_is_forward_type(e, name);
        /* The value-preferring scope_lookup above returns the lowered struct's
         * auto-bound CONSTRUCTOR (TY_FN), which shadows the struct-origin record
         * ADT type binding; consult the type namespace to recognise it. */
        Binding *existing_type_b = scope_lookup_type_def(e->scope, name);
        AdtDef *lowered_struct_def = NULL;
        if (existing_gadt_b->type.kind == TY_ADT &&
            existing_gadt_b->type.as.adt_.def &&
            existing_gadt_b->type.as.adt_.def->from_struct_lowering)
            lowered_struct_def = existing_gadt_b->type.as.adt_.def;
        else if (existing_type_b && existing_type_b->type.kind == TY_ADT &&
                 existing_type_b->type.as.adt_.def &&
                 existing_type_b->type.as.adt_.def->from_struct_lowering)
            lowered_struct_def = existing_type_b->type.as.adt_.def;
        if (same_kind_forward) {
            is_forward_stub_gadt = true;
        } else if (existing_gadt_b->type.kind == TY_STRUCT) {
            /* MF4: GADT shadows an existing struct of the same name.
             * Allow coexistence; warn so the shadowing is visible. */
            diag_emit(DIAG_WARNING, name_form->span,
                      "GADT '%s' shadows existing struct '%s'; uses of "
                      "':%s' in type annotations resolve to the GADT",
                      name->name, name->name, name->name);
            /* fall through: is_forward_stub_gadt stays false, the else
             * branch below registers a fresh GADT binding and AdtDef. */
        } else if (lowered_struct_def) {
            /* CONV-S1 (defstruct-as-defadt): under lowering the struct IS an
             * ADT, so structs and ADTs share one namespace -- the GADT wins and
             * SUPERSEDES the struct-origin ADT.  Mark the loser superseded so it
             * is skipped at C emission (no `tur_adt_<Name>` collision), then fall
             * through to register the GADT's own binding/AdtDef.  `:Name`
             * annotations resolve to the GADT via elab_lookup_type_by_name's
             * prefer-non-struct-origin rule. */
            lowered_struct_def->superseded = true;
            diag_emit(DIAG_WARNING, name_form->span,
                      "GADT '%s' supersedes the same-named struct '%s'; uses of "
                      "':%s' resolve to the GADT",
                      name->name, name->name, name->name);
            /* fall through: register a fresh GADT binding/AdtDef. */
        } else {
            diag_emit(DIAG_ERROR, name_form->span,
                      "defgadt: '%s' is already defined", name->name);
            return NULL;
        }
    }

    uint32_t n_ctors = call->as.list.len - ctors_start_idx;

    /* Phase RF0: Allocate (or reuse forward stub) AdtDef and register BEFORE
     * parsing constructors so that self-referential and mutually-recursive
     * constructor field types resolve correctly. */
    AdtDef *def;
    Binding *adt_binding;
    Type adt_type;
    if (is_forward_stub_gadt) {
        /* Reuse the pre-registered stub and fill it in */
        adt_binding = existing_gadt_b;
        def = adt_binding->type.as.adt_.def;
        def->n_ctors = n_ctors;
        def->ctors = (CtorDef **)arena_alloc(e->arena, n_ctors * sizeof(CtorDef *));
        /* Zero the array: predicates reached MID-DEFINITION (a recursive field
         * probing adt_is_byvalue_product while this def's ctors are still being
         * filled) guard on `ctors[ci] == NULL`, and arena memory is not zeroed
         * -- the guard only ever worked on lucky fresh pages.  turi's
         * longer-lived arena handed back dirty memory and the guard read a
         * garbage CtorDef (SEGV in adt_sr1_sum_candidate). */
        memset(def->ctors, 0, n_ctors * sizeof(CtorDef *));
        def->is_copy = is_copy;
        def->needs_drop_glue = false;
        def->is_gadt = true;
        def->type_params = type_params;
        def->n_type_params = n_type_params;
        /* TP2: allocate Kind array initialised to KIND_STAR (TP4 refines).
         * L6 follow-up C: seed from parsed_type_param_kinds so a `^&name`
         * row-kinded parameter is preserved as KIND_TYPEROW. */
        if (n_type_params > 0) {
            Kind *tpk = (Kind *)arena_alloc(e->arena, n_type_params * sizeof(Kind));
            for (uint8_t pi = 0; pi < n_type_params; pi++) {
                tpk[pi] = parsed_type_param_kinds ? parsed_type_param_kinds[pi]
                                                  : KIND_STAR;
            }
            def->type_param_kinds = tpk;
        } else {
            def->type_param_kinds = NULL;
        }
        adt_type = type_adt(def);
        /* Phase G1/HKT: Apply the same KIND_ARROW fix as the non-stub branch so
         * that kind checks see the correct kind for parameterized GADTs. */
        adt_type.hkt_kind = kind_for_arity(n_type_params);
        adt_binding->type = adt_type;
        /* Already in global scope and elab registry from the pre-pass */
    } else {
        def = (AdtDef *)arena_alloc(e->arena, sizeof(AdtDef));
        memset(def, 0, sizeof(*def));  /* DS5: zero all bool / scalar fields by default */
        def->name = name->name;
        def->n_ctors = n_ctors;
        def->ctors = (CtorDef **)arena_alloc(e->arena, n_ctors * sizeof(CtorDef *));
        /* Zero the array: predicates reached MID-DEFINITION (a recursive field
         * probing adt_is_byvalue_product while this def's ctors are still being
         * filled) guard on `ctors[ci] == NULL`, and arena memory is not zeroed
         * -- the guard only ever worked on lucky fresh pages.  turi's
         * longer-lived arena handed back dirty memory and the guard read a
         * garbage CtorDef (SEGV in adt_sr1_sum_candidate). */
        memset(def->ctors, 0, n_ctors * sizeof(CtorDef *));
        def->is_copy = is_copy;
        def->is_gadt = true;
        def->type_params = type_params;
        def->n_type_params = n_type_params;
        /* TP2: allocate Kind array initialised to KIND_STAR (TP4 refines).
         * L6 follow-up C: seed from parsed_type_param_kinds so a `^&name`
         * row-kinded parameter is preserved as KIND_TYPEROW. */
        if (n_type_params > 0) {
            Kind *tpk = (Kind *)arena_alloc(e->arena, n_type_params * sizeof(Kind));
            for (uint8_t pi = 0; pi < n_type_params; pi++) {
                tpk[pi] = parsed_type_param_kinds ? parsed_type_param_kinds[pi]
                                                  : KIND_STAR;
            }
            def->type_param_kinds = tpk;
        } else {
            def->type_param_kinds = NULL;
        }

        /* Pre-register ADT type so constructors can reference it.
         * Phase G1/HKT: Set hkt_kind based on type-parameter count so that
         * the belt-and-suspenders kind check can detect when a parameterized
         * GADT is used in a kind-* argument slot of another GADT. */
        adt_type = type_adt(def);
        adt_type.hkt_kind = kind_for_arity(n_type_params);
        adt_binding = binding_new(e, name, adt_type, false, true, name_form->span);
        scope_add(&e->global, adt_binding);
    }

    /* Record the owning compilation unit for the orphan-instance check
     * (mirrors StructDef.origin_file_id). */
    def->origin_file_id = name_form->span.file_id;

    /* Parse each constructor.
     * `ci` is declared outside the loop so the `ctor_parse_error` bail-out
     * path below can truncate `def->n_ctors` to the number of slots we
     * actually filled (every error path bails before `def->ctors[ci]` is
     * assigned, so `ci` is exactly the populated-slot count). */
    uint32_t ci = 0;
    for (; ci < n_ctors; ci++) {
        Form *ctor_form = call->as.list.items[ctors_start_idx + ci];
        if (ctor_form->tag != F_LIST || ctor_form->as.list.len < 1) {
            diag_emit(DIAG_ERROR, ctor_form->span,
                      "defgadt: constructor must be a list form");
            goto ctor_parse_error;
        }
        Form *ctor_name_form = ctor_form->as.list.items[0];
        if (ctor_name_form->tag != F_SYM) {
            diag_emit(DIAG_ERROR, ctor_name_form->span,
                      "defgadt: constructor name must be a symbol");
            goto ctor_parse_error;
        }
        const Symbol *ctor_name = ctor_name_form->as.sym;

        /* Find the ':' separator in the constructor form.
         * Format: (CtorName FieldType1 ... FieldTypeN : return-type-form)
         * The ':' may be a bare F_SYM(":") token (legacy) or an F_TYPE_ANN node
         * produced by the new `: type-expr` reader (Phase G3 compat). */
        int colon_idx = -1;
        bool type_ann_colon = false; /* true when the ':' was absorbed into F_TYPE_ANN */
        for (uint32_t fi = 1; fi < ctor_form->as.list.len; fi++) {
            Form *item = ctor_form->as.list.items[fi];
            if (item->tag == F_SYM && item->as.sym == e->sym_colon) {
                colon_idx = (int)fi;
                break;
            }
            if (item->tag == F_TYPE_ANN) {
                /* `: return-type` was folded into a single F_TYPE_ANN node.
                 * Treat this index as the separator; the return type is inside. */
                colon_idx = (int)fi;
                type_ann_colon = true;
                break;
            }
        }
        if (colon_idx < 0) {
            diag_emit(DIAG_ERROR, ctor_form->span,
                      "defgadt: constructor '%s' requires an explicit return-type annotation\n"
                      "  hint: add ': (return-type)' after the constructor name",
                      ctor_name->name);
            goto ctor_parse_error;
        }
        /* For the F_TYPE_ANN case the return-type form is the inner form;
         * for the bare-':' case it is the item immediately after. */
        Form *return_type_form;
        if (type_ann_colon) {
            Form *ann = ctor_form->as.list.items[colon_idx];
            if (ann->as.list.len < 1) {
                diag_emit(DIAG_ERROR, ann->span,
                          "defgadt: constructor '%s': missing return type after ':'",
                          ctor_name->name);
                goto ctor_parse_error;
            }
            return_type_form = ann->as.list.items[0];
        } else {
            if ((uint32_t)colon_idx + 1 >= ctor_form->as.list.len) {
                diag_emit(DIAG_ERROR, ctor_form->span,
                          "defgadt: constructor '%s': missing return type after ':'",
                          ctor_name->name);
                goto ctor_parse_error;
            }
            return_type_form = ctor_form->as.list.items[colon_idx + 1];
        }
        /* Validate: return type must mention the GADT name */
        bool mentions_gadt = false;
        if (return_type_form->tag == F_LIST && return_type_form->as.list.len >= 1) {
            Form *head = return_type_form->as.list.items[0];
            if (head->tag == F_SYM && strcmp(head->as.sym->name, name->name) == 0) {
                mentions_gadt = true;
            }
        } else if (return_type_form->tag == F_SYM &&
                   strcmp(return_type_form->as.sym->name, name->name) == 0) {
            mentions_gadt = true;
        }
        if (!mentions_gadt) {
            diag_emit(DIAG_ERROR, return_type_form->span,
                      "defgadt: constructor '%s' return type must be an application of '%s'",
                      ctor_name->name, name->name);
            goto ctor_parse_error;
        }

        /* Change 3: Validate that the number of type args in the return type
         * matches the GADT's declared type-parameter count. */
        if (return_type_form->tag == F_LIST) {
            uint32_t n_rt_args = return_type_form->as.list.len - 1; /* subtract head */
            if (n_type_params > 0 && n_rt_args != n_type_params) {
                diag_emit(DIAG_ERROR, return_type_form->span,
                          "defgadt: constructor '%s' return type has %u type argument(s) "
                          "but '%s' has %u type parameter(s)",
                          ctor_name->name, n_rt_args, name->name, n_type_params);
                goto ctor_parse_error;
            }
        }

        /* Change 4: Validate each type arg in the return type is a known type. */
        if (return_type_form->tag == F_LIST) {
            for (uint32_t ai = 1; ai < return_type_form->as.list.len; ai++) {
                Form *arg = return_type_form->as.list.items[ai];
                if (arg->tag == F_LIST) continue; /* type application -- ok */
                if (arg->tag != F_SYM) continue;  /* other forms -- ok */
                const char *an = arg->as.sym->name;
                /* Check if it's a bound type param */
                bool is_param = false;
                for (uint8_t pi = 0; pi < n_type_params; pi++) {
                    if (strcmp(type_params[pi], an) == 0) { is_param = true; break; }
                }
                if (is_param) continue;
                /* Check if it's a concrete primitive */
                static const char *primitives[] = {
                    "int", "bool", "float", "cstr", "nil", "void", "ptr",
                    "int8", "int16", "int32", "int64",
                    "uint8", "uint16", "uint32", "uint64",
                    "float32", "float64", NULL
                };
                bool is_prim = false;
                for (int pi = 0; primitives[pi]; pi++) {
                    if (strcmp(primitives[pi], an) == 0) { is_prim = true; break; }
                }
                if (is_prim) continue;
                /* Check if it's a known type in scope */
                const Symbol *type_sym = symtab_intern(e->st,
                    strslice(an, (uint32_t)strlen(an)));
                Binding *tb = scope_lookup_type_def(e->scope, type_sym);
                if (tb && (tb->type.kind == TY_STRUCT || tb->type.kind == TY_ADT)) {
                    /* Phase G1/HKT: Belt-and-suspenders kind check.
                     * All GADT type parameters have kind * in Phase G1.  A
                     * type constructor of kind * -> * (hkt_kind == KIND_ARROW)
                     * in a plain type-argument slot is a kind mismatch. */
                    if (tb->type.hkt_kind == KIND_ARROW ||
                            tb->type.hkt_kind == KIND_ARROW2) {
                        diag_emit_with_code(DIAG_ERROR, arg->span,
                            TUR_E0012_KIND_MISMATCH,
                            "kind mismatch (TUR-E0012): type argument '%s' in constructor "
                            "'%s' return type has kind '* -> *' but kind '*' is expected",
                            an, ctor_name->name);
                        goto ctor_parse_error;
                    }
                    continue;
                }
                /* Unknown -- error */
                diag_emit(DIAG_ERROR, arg->span,
                          "defgadt: unknown type argument '%s' in return type of constructor '%s' "
                          "(must be a type parameter, primitive type, or defined type)",
                          an, ctor_name->name);
                goto ctor_parse_error;
            }
        }

        /* Field types: items[1 .. colon_idx-1] */
        uint32_t n_fields = (uint32_t)(colon_idx - 1);
        CtorDef *ctor = (CtorDef *)arena_alloc(e->arena, sizeof(CtorDef));
        ctor->name = ctor_name->name;
        ctor->n_fields = n_fields;
        ctor->fields = n_fields > 0
            ? (CtorField *)arena_alloc(e->arena, n_fields * sizeof(CtorField))
            : NULL;
        /* arena_alloc does not zero, and resolve_ctor_field writes
         * `drop_inner_def` only on the nested-owning-aggregate branch -- so
         * every other field kept the arena's 0xbe poison and the drop/walk glue
         * emitter dereferenced it.  Latent for as long as the emitter only
         * walked ctor 0; it surfaced when the sum-type glue fix widened that
         * walk to every ctor.  Zero the whole array once rather than chase
         * per-field defaults. */
        if (ctor->fields) memset(ctor->fields, 0, n_fields * sizeof(CtorField));
        ctor->adt = def;
        ctor->tag = ci;
        ctor->result_type_form = return_type_form;
        ctor->is_record = false; /* CONV-S0: GADT variants are positional-only */
        /* Phase G2: store raw field-type annotation forms for per-arm resolution */
        ctor->field_forms = n_fields > 0
            ? (const struct Form **)arena_alloc(e->arena, n_fields * sizeof(const Form *))
            : NULL;

        for (uint32_t fi = 0; fi < n_fields; fi++) {
            Form *ft_form = ctor_form->as.list.items[1 + fi];
            TypeKind fkind = gadt_field_typekind_from_form(ft_form);
            ctor->fields[fi].kind = fkind;
            ctor->fields[fi].inner_kind = TY_UNKNOWN;
            ctor->fields[fi].name = NULL; /* CONV-S0: positional */
            /* A positional/GADT ctor field carries no `#fx{...}` capability row;
             * initialize the pointer explicitly (arena_alloc does not zero) so
             * readers -- effect_check's CtorField-row resolve, cps_ir's
             * expr_fn_effect_row -- never dereference uninitialized garbage. */
            ctor->fields[fi].effect_row = NULL;
            /* TP2: populate full_type when the field references a declared type
             * parameter (e.g. `a` in `(MkBox a : (Box a))`).  The C-level kind
             * stays TY_INT; full_type is elaboration-only. */
            ctor->fields[fi].full_type =
                adt_field_type_from_form(e->arena, ft_form,
                                         def->type_params, def->n_type_params);
            if (fkind == TY_RC || fkind == TY_REF || fkind == TY_WEAK) {
                def->needs_drop_glue = true;
            }
            /* Phase G2: also stash the raw form for per-arm type resolution */
            if (ctor->field_forms) ctor->field_forms[fi] = ft_form;
        }
        def->ctors[ci] = ctor;

        /* Register constructor binding */
        if (n_fields == 0) {
            Binding *cb = binding_new(e, ctor_name, adt_type, false, true,
                                      ctor_name_form->span);
            scope_add(&e->global, cb);
        } else {
            TypeKind arg_kinds[MAX_FN_ARITY];
            uint8_t arity = (uint8_t)(n_fields > MAX_FN_ARITY ? MAX_FN_ARITY : n_fields);
            for (uint8_t fi = 0; fi < arity; fi++) {
                arg_kinds[fi] = ctor->fields[fi].kind;
            }
            Type fn_type = type_fn(arg_kinds, arity, TY_ADT);
            Binding *cb = binding_new(e, ctor_name, fn_type, false, true,
                                      ctor_name_form->span);
            scope_add(&e->global, cb);
        }
    }

    /* Register ADT in elab registry (skip if it was a forward stub -- already registered) */
    if (!is_forward_stub_gadt) {
        elab_register_adt_def(e, def);
    }

    /* TP4: refine type_param_kinds based on CtorField.full_type usage */
    infer_type_param_kinds(def);

    Expr *out = expr_new(e->arena, EX_DEFGADT, TYPE_NIL, call->span);
    out->as.defgadt_.def = def;
    out->as.defgadt_.binding = adt_binding;
    def->from_stdlib = e->in_stdlib_load;   /* r7rs-programs-compile-slowly */
    return out;

ctor_parse_error:
    /* A constructor failed to parse after the AdtDef was pre-registered in
     * scope. The slots `[ci, n_ctors)` are still NULL, but `def->n_ctors`
     * advertises the full declared count, so a later `(match ...)` on this
     * type would dereference a NULL CtorDef and crash (see
     * docs/archive/history/defgadt-malformed-pattern-segfault.md). Truncate the
     * count to the slots we actually filled. Compilation has already failed
     * (a diagnostic was emitted), so this only prevents the crash. */
    def->n_ctors = ci;
    return NULL;
}

/* Phase G3: coerce — (coerce eq x) where eq : (Equal a b), x : a → x : b
 * Zero-cost cast: the runtime representation of a and b are identical (both int64_t).
 * The equality proof eq is evaluated for side-effects but its value is discarded.
 * Error if eq is not a value of the built-in Equal GADT. */
Expr *elab_coerce(Elab *e, const Form *call) {
    if (call->as.list.len != 3) {
        diag_emit(DIAG_ERROR, call->span,
                  "coerce requires exactly 2 arguments: (coerce eq x)");
        return NULL;
    }
    Expr *eq = elab_form(e, call->as.list.items[1]);
    if (!eq) return NULL;
    /* Verify eq has type Equal */
    if (eq->type.kind != TY_ADT ||
        !eq->type.as.adt_.def ||
        strcmp(eq->type.as.adt_.def->name, "Equal") != 0) {
        diag_emit(DIAG_ERROR, call->as.list.items[1]->span,
                  "coerce requires an (Equal a b) proof as first argument");
        return NULL;
    }
    Expr *x = elab_form(e, call->as.list.items[2]);
    if (!x) return NULL;
    /* SS1: Reject coerce whose source is a session channel.
     * Reinterpreting a Session endpoint at a different protocol step would
     * strand the linear resource; the Equal witness is insufficient to make
     * this safe without full protocol equality checking (SS3+). */
    if (x->type.kind == TY_SESSION) {
        diag_emit(DIAG_ERROR, call->span,
                  "coerce cannot reinterpret a session channel endpoint; "
                  "use session operations (send/recv/close/...) to advance the protocol");
        return NULL;
    }
    /* Zero-cost cast: return x unchanged (same runtime representation) */
    return x;
}

/* Phase G0: Helper - look up CtorDef by name across all known ADTs */
CtorDef *elab_lookup_ctor(Elab *e, const Symbol *name) {
    /* Scan most-recently-registered first so a later definition shadows an
     * earlier one of the same constructor name -- lexical-shadowing semantics,
     * and the same answer `scope_lookup` gives for the auto-bound ctor fn.  This
     * matters once stdlib `defstruct`s lower to record ADTs: the stdlib `Cons`
     * (autoloaded first) would otherwise shadow a user `(defdata List (Cons ...))`
     * registered later, mis-resolving the user's `(Cons ...)` to the stdlib cell.
     * At default (stdlib Cons is a struct, absent from adt_defs) only the user's
     * ctor is present, so the scan direction is immaterial there. */
    for (uint32_t ai = e->n_adt_defs; ai-- > 0; ) {
        AdtDef *adt = e->adt_defs[ai];
        for (uint32_t ci = 0; ci < adt->n_ctors; ci++) {
            /* A constructor slot can be NULL (with a NULL ->name) when its
             * defdata bailed early on a malformed field type, leaving the
             * AdtDef registered with an unfilled slot. Skip such partial
             * slots so lookups recover cleanly instead of dereferencing
             * NULL. */
            CtorDef *ctor = adt->ctors[ci];
            if (!ctor || !ctor->name) continue;
            if (strcmp(ctor->name, name->name) == 0) {
                return ctor;
            }
        }
    }
    return NULL;
}

/* Phase G0: match expression
 * Syntax: (match scrutinee
 *   (Ctor1 x y) body1
 *   (Ctor2 z)   body2
 *   _           default-body)
 * Arms are interleaved: pattern body pattern body ...
 */
/* poly-defn-inner-lambda-codegen: two match arms over the same parametric ADT
 * may disagree only in that one produced the bare TY_ADT carrier (a constructor
 * applied to a polymorphic tyvar argument whose element could not be pinned to a
 * ground type -- e.g. `(POK v rest)` with `v : A`) while a peer arm produced the
 * full TY_APP (`(PRes A)`, from a forward-referenced sibling call or the
 * enclosing lambda's declared return).  They denote the same type; treat them as
 * compatible and promote the unified result to the more specific TY_APP so the
 * match (and the enclosing defn/lambda's declared return) keeps the element
 * type.  On success *out receives the promoted (more specific) type. */
/* class-method-level-hkt-tyvar-grounding: deep structural join for the
 * ARGUMENTS of a shared TY_APP spine.  Two arms of a `match` may denote the
 * same applied type while differing only in that one side still carries an
 * ungrounded type variable (a method-level HKT tyvar like the `b` in a
 * Traversable-style `trav : (t a) -> (fn [a] (Opt b)) -> (Opt (t b))`) where
 * the peer arm produced a concrete type.  Within a matching constructor spine
 * we treat an ungrounded TY_TYVAR / TY_UNKNOWN as a unification variable: it
 * grounds to whatever the peer has in that position, and the join promotes the
 * more specific type.  This is scoped to descent BELOW a shared TY_APP head --
 * the top-level entry point keeps its strict same-head guard, so a bare
 * top-level tyvar arm (the distinct pure/empty inference gap) is unaffected. */
static bool arm_arg_join(Elab *e, Type a, Type b, Type *out) {
    if (type_eq(a, b)) { *out = a; return true; }
    /* An ungrounded tyvar/unknown on either side grounds to the peer. */
    if (a.kind == TY_TYVAR || a.kind == TY_UNKNOWN) { *out = b; return true; }
    if (b.kind == TY_TYVAR || b.kind == TY_UNKNOWN) { *out = a; return true; }
    /* Two applications: join head and argument, rebuild the promoted spine. */
    if (a.kind == TY_APP && b.kind == TY_APP &&
        a.as.app.fn && b.as.app.fn && a.as.app.arg && b.as.app.arg) {
        Type jfn, jarg;
        if (arm_arg_join(e, *a.as.app.fn, *b.as.app.fn, &jfn) &&
            arm_arg_join(e, *a.as.app.arg, *b.as.app.arg, &jarg)) {
            Type *pfn  = (Type *)arena_alloc(e->arena, sizeof(Type));
            Type *parg = (Type *)arena_alloc(e->arena, sizeof(Type));
            *pfn = jfn; *parg = jarg;
            Type app = a; /* inherit copy_kind / substruct flags */
            app.kind = TY_APP;
            app.as.app.fn = pfn;
            app.as.app.arg = parg;
            *out = app;
            return true;
        }
        return false;
    }
    /* One bare TY_ADT, one TY_APP over the same def: keep the TY_APP. */
    AdtDef *ad = (a.kind == TY_ADT) ? a.as.adt_.def
               : (a.kind == TY_APP) ? type_adt_app_def(&a) : NULL;
    AdtDef *bd = (b.kind == TY_ADT) ? b.as.adt_.def
               : (b.kind == TY_APP) ? type_adt_app_def(&b) : NULL;
    if (ad && ad == bd && a.kind != b.kind) {
        *out = (a.kind == TY_APP) ? a : b;
        return true;
    }
    return false;
}

static bool match_arm_type_compatible(Elab *e, Type a, Type b, Type *out) {
    if (type_eq(a, b)) {
        /* match-result-temp-typed-from-first-arm: two arms of one fn type, a
         * capture-free lambda (a thin code pointer) and a capturing one (a fat
         * box), join to the FAT representation -- every arm is boxed to it
         * on the way into the result.  Keeping the first arm's made the result
         * temporary `int64_t` beside the arms' `void *` boxes: invalid C under
         * clang when the capture-free arm came first. */
        *out = (a.kind == TY_FN && b.kind == TY_FN && !a.as.fn.boxed && b.as.fn.boxed) ? b : a;
        return true;
    }
    /* `!` (TY_NEVER) is bottom: an arm that diverges -- `(panic ...)`, a
     * `return`, a call to a `!`-returning function -- produces no value, so it
     * is compatible with any peer arm and contributes nothing to the result
     * type.  Without this a `(match x (A) 1 (B) (panic "no"))` is rejected
     * with "arm types are incompatible -- expected int, got !". */
    if (a.kind == TY_NEVER) { *out = b; return true; }
    if (b.kind == TY_NEVER) { *out = a; return true; }
    /* saffron-lang-plan S4: `any` is the TOP type, so an arm producing one
     * joins with an arm producing anything else -- the result is `any`.
     *
     * The `if` join already widens this way (any-coercion-not-driven-by-
     * expected-type); `match` had its own unifier and did not, so a Saffron
     * fold over a heterogeneous list was rejected with "expected int (from
     * earlier arm), got any" -- the earlier arm being the recursive call, whose
     * return type is still being inferred, and the later one the `any`
     * accumulator.
     *
     * Not gated on the dialect: `any` is Turmeric's top type too, and an arm
     * of type `any` beside an arm of type `int` has exactly one sound join in
     * either language.  Widening it is what the type already means. */
    if (a.kind == TY_ANY || b.kind == TY_ANY) {
        Type any_t;
        memset(&any_t, 0, sizeof(any_t));
    any_t.copy_kind = CK_COPY;   /* CK_UNIQUE is 0: a zeroed type is unique-kinded (saffron-any-let-binding-is-unique) */
        any_t.kind = TY_ANY;
        *out = any_t;
        return true;
    }
    AdtDef *ad = (a.kind == TY_ADT) ? a.as.adt_.def
               : (a.kind == TY_APP) ? type_adt_app_def(&a) : NULL;
    AdtDef *bd = (b.kind == TY_ADT) ? b.as.adt_.def
               : (b.kind == TY_APP) ? type_adt_app_def(&b) : NULL;
    if (ad && ad == bd) {
        if (a.kind != b.kind) {
            /* One bare TY_ADT, one TY_APP over the same def: keep the TY_APP. */
            *out = (a.kind == TY_APP) ? a : b;
            return true;
        }
        if (a.kind == TY_APP && b.kind == TY_APP &&
            (type_has_open_slot(&a) || type_has_open_slot(&b))) {
            /* fmap-over-underdetermined-constructor-is-a-defless-shell: an
             * arm whose constructor left a parameter OPEN is the carrier.
             * Grounding each side's open slot against the other -- `(Left l)`
             * is `(Either int R)`, `(Right (f r))` is `(Either L int)` -- made
             * `(Either int int)`, a by-value monomorph that neither arm's
             * carrier value is.  A concrete peer still wins, as it did over
             * the bare ADT these arms used to be; two open arms join to that
             * bare ADT, the carrier they both are. */
            if (!type_has_open_slot(&a) && type_app_is_concrete_adt(&a)) { *out = a; return true; }
            if (!type_has_open_slot(&b) && type_app_is_concrete_adt(&b)) { *out = b; return true; }
            *out = type_adt(ad);
            return true;
        }
        if (a.kind == TY_APP && b.kind == TY_APP) {
            /* Both applied over the same head: structurally join the argument
             * spines, grounding any method-level tyvar against its concrete
             * peer.  See arm_arg_join above. */
            return arm_arg_join(e, a, b, out);
        }
    }
    /* SR2b: one arm's type is an application headed by an UNGROUNDED tyvar --
     * `(m b)` from `(k v)` in a Monad bind's Some arm -- and the peer is a
     * concrete ADT application (`(Option A)` from the None arm's `(none)`).
     * The class variable `m` is grounded by the INSTANCE, not by this join,
     * so the abstract head can never win here: take the concrete side.  This
     * is the class-method-hkt-tyvar-grounding rule one level up -- that fix
     * grounds a method tyvar BELOW a shared concrete head (arm_arg_join);
     * this grounds the head itself.  Reachable once Option/Result are sums,
     * because a sum's empty arm is a bare constructor call rather than an
     * accessor whose type the record head already fixed. */
    if (a.kind == TY_APP && b.kind == TY_APP && (ad != NULL) != (bd != NULL)) {
        const Type *abs = ad ? &b : &a;
        const Type *spine = abs;
        while (spine->kind == TY_APP && spine->as.app.fn) spine = spine->as.app.fn;
        if (spine->kind == TY_TYVAR) {
            *out = ad ? a : b;
            return true;
        }
    }
    return false;
}

/* ---------------------------------------------------------------------------
 * match-nested-constructor-patterns: nested constructor patterns in match arms
 *
 * `(match e (Add (Lit 0) r) ...)` used to be rejected with "match: field
 * binding must be a symbol": an arm could bind exactly one constructor level,
 * so every nested test had to be hand-flattened into an inner `match`.
 *
 * The flattening is mechanical, so the lowering below does it -- at the FORM
 * level, before anything is elaborated.  All the arms for one constructor fold
 * into a single arm binding fresh field names, whose body is a chain of tests
 * over the nested sub-patterns, each falling through to the next candidate arm
 * on failure:
 *
 *   (match e                        (let [__ms0 e]
 *     (Lit v)         v      =>       (match __ms0
 *     (Add (Lit 0) r) r                 (Lit v) v
 *     (Add l r)       (f l r))          (Add __mp1 __mp2)
 *                                         (match __mp1
 *                                           (Lit __mp3)
 *                                             (if (= __mp3 0)
 *                                               (let [r __mp2] r)
 *                                               (let [l __mp1 r __mp2] (f l r)))
 *                                           _ (let [l __mp1 r __mp2] (f l r)))))
 *
 * Depth needs no special handling: the inner `match` forms emitted here go
 * back through elab_match and are lowered again if they nest further.  The
 * fallback is duplicated per failing test -- that costs code size, never
 * execution: exactly one copy runs.
 *
 * The scrutinee is let-bound to a temp only when a fallback needs the whole
 * value (a variable catch-all arm); otherwise the original scrutinee form is
 * passed through untouched.
 * ------------------------------------------------------------------------- */

/* A sub-pattern that is a TEST rather than a plain binder. */
static bool match_subpat_is_test(const Form *q) {
    return q->tag == F_LIST || q->tag == F_INT || q->tag == F_BOOL ||
           q->tag == F_FLOAT;
}

/* Does this arm pattern carry a nested sub-pattern?  Gated on the head
 * resolving to a real constructor, so union / session-offer patterns (whose
 * shapes also start with a symbol) are never touched. */
static bool match_pattern_has_nested(Elab *e, const Form *pat) {
    if (pat->tag != F_LIST || pat->as.list.len < 2) return false;
    if (pat->as.list.items[0]->tag != F_SYM) return false;
    if (!elab_lookup_ctor(e, pat->as.list.items[0]->as.sym)) return false;
    if (pat->as.list.items[1]->tag == F_KEYWORD) return false;  /* by-name form */
    for (uint32_t i = 1; i < pat->as.list.len; i++)
        if (pat->as.list.items[i]->tag != F_SYM) return true;
    return false;
}

static Form *mlist(Elab *e, Span sp, Form **items, uint32_t n) {
    Form **it = (Form **)arena_alloc(e->arena, n * sizeof(Form *));
    for (uint32_t i = 0; i < n; i++) it[i] = items[i];
    return form_list(e->arena, sp, it, n);
}

static Form *mk_fresh_sym(Elab *e, Span sp, const char *prefix) {
    char buf[48];
    snprintf(buf, sizeof(buf), "%s%u", prefix, e->next_gensym_id++);
    const Symbol *s = symtab_intern(e->st, strslice(buf, (uint32_t)strlen(buf)));
    return form_sym(e->arena, sp, s);
}

/* (let [n0 v0 n1 v1 ...] body) */
static Form *mk_let(Elab *e, Span sp, Form **pairs, uint32_t n_pairs, Form *body) {
    if (n_pairs == 0) return body;
    Form **bv = (Form **)arena_alloc(e->arena, n_pairs * 2 * sizeof(Form *));
    for (uint32_t i = 0; i < n_pairs * 2; i++) bv[i] = pairs[i];
    Form *vec = form_vec(e->arena, sp, bv, n_pairs * 2);
    Form *items[3];
    items[0] = form_sym(e->arena, sp, e->sym_let);
    items[1] = vec;
    items[2] = body;
    return mlist(e, sp, items, 3);
}

/* Do these constructor patterns, all applying to one value position, cover
 * every constructor of `adt`?  A plain binder covers everything; a constructor
 * pattern covers its own constructor when its own nested positions are covered
 * in turn, which is the recursion.  A literal sub-pattern covers only itself,
 * so a position holding only literals is never provably exhaustive.
 *
 * Conservative by construction: anything this cannot prove reports "not
 * covered", which costs the user an explicit catch-all arm and never
 * mis-accepts a genuinely partial match. */
static bool match_pats_cover_adt(Elab *e, const Form **pats, uint32_t n,
                                 AdtDef *adt, int depth);

/* One constructor's group of arm patterns: are the values of that constructor
 * fully covered?  `pats` are whole patterns `(C f1 ... fm)`. */
static bool match_ctor_group_covers(Elab *e, const Form **pats, uint32_t n,
                                    int depth) {
    if (depth > 16 || n == 0) return false;
    int field = -1;
    for (uint32_t i = 0; i < n; i++) {
        const Form *p = pats[i];
        if (p->tag != F_LIST) return false;
        int this_field = -1;
        bool two = false;
        for (uint32_t f = 1; f < p->as.list.len; f++) {
            if (p->as.list.items[f]->tag == F_SYM) continue;
            if (this_field >= 0) { two = true; break; }
            this_field = (int)f;
        }
        if (two) return false;                 /* two nested fields: not analysed */
        if (this_field < 0) return true;       /* all binders: matches every value */
        if (field < 0) field = this_field;
        else if (field != this_field) return false;
    }
    /* Every pattern nests at the same single position: recurse there. */
    const Form *subs[256];
    uint32_t n_subs = 0;
    AdtDef *sub_adt = NULL;
    for (uint32_t i = 0; i < n && n_subs < 256; i++) {
        const Form *q = pats[i]->as.list.items[field];
        subs[n_subs++] = q;
        if (q->tag == F_LIST && q->as.list.len >= 1 &&
            q->as.list.items[0]->tag == F_SYM) {
            CtorDef *qc = elab_lookup_ctor(e, q->as.list.items[0]->as.sym);
            if (qc && qc->adt) {
                if (sub_adt && sub_adt != qc->adt) return false;
                sub_adt = qc->adt;
            }
        }
    }
    return match_pats_cover_adt(e, subs, n_subs, sub_adt, depth + 1);
}

static bool match_pats_cover_adt(Elab *e, const Form **pats, uint32_t n,
                                 AdtDef *adt, int depth) {
    if (depth > 16) return false;
    for (uint32_t i = 0; i < n; i++)
        if (pats[i]->tag == F_SYM) return true;   /* a binder covers everything */
    if (!adt || adt->n_ctors == 0) return false;  /* literals only, or unknown */
    for (uint32_t t = 0; t < adt->n_ctors; t++) {
        const Form *group[256];
        uint32_t n_group = 0;
        for (uint32_t i = 0; i < n && n_group < 256; i++) {
            const Form *q = pats[i];
            if (q->tag != F_LIST || q->as.list.len < 1 ||
                q->as.list.items[0]->tag != F_SYM) continue;
            CtorDef *qc = elab_lookup_ctor(e, q->as.list.items[0]->as.sym);
            if (qc && qc->tag == t && qc->adt == adt) group[n_group++] = q;
        }
        if (n_group == 0) return false;
        if (!match_ctor_group_covers(e, group, n_group, depth)) return false;
    }
    return true;
}

/* Can the arms of one constructor group fail to match a value of that
 * constructor?  A trailing plain, unguarded arm always matches; so does a
 * group whose nested sub-patterns provably cover the sub-ADT. */
static bool match_group_falls_through(Elab *e, const Form *call,
                                      const uint32_t *arm_pat,
                                      Form *const *arm_guard,
                                      const uint32_t *group, uint32_t n_group) {
    const Form *last = call->as.list.items[arm_pat[group[n_group - 1]]];
    if (!arm_guard[group[n_group - 1]] && !match_pattern_has_nested(e, last))
        return false;
    const Form *pats[256];
    uint32_t n = 0;
    for (uint32_t gi = 0; gi < n_group && n < 256; gi++) {
        if (arm_guard[group[gi]]) return true;   /* a guard can always fail */
        pats[n++] = call->as.list.items[arm_pat[group[gi]]];
    }
    return !match_ctor_group_covers(e, pats, n, 0);
}

/* Build the test chain for ONE arm of a group: bind the arm's symbol
 * sub-patterns, run its guard, and thread `next` through every failure edge. */
static Form *match_build_arm_test(Elab *e, const Form *pat, Form *guard,
                                  Form *body, Form **field_syms, Form *next,
                                  bool *err) {
    Span sp = pat->span;
    Form *inner = body;
    if (guard) {
        Form *items[4];
        items[0] = form_sym(e->arena, sp, e->sym_if);
        items[1] = guard;
        items[2] = inner;
        items[3] = next;
        inner = mlist(e, sp, items, 4);
    }
    /* Alias the plain-binder positions to the user's names. */
    {
        uint32_t n_pairs = 0;
        Form *pairs[2 * 64];
        const Symbol *sym_wc = intern_cstr(e->st, "_");
        for (uint32_t f = 1; f < pat->as.list.len && n_pairs < 64; f++) {
            const Form *q = pat->as.list.items[f];
            if (q->tag != F_SYM || q->as.sym == sym_wc) continue;
            pairs[n_pairs * 2]     = (Form *)q;
            pairs[n_pairs * 2 + 1] = field_syms[f - 1];
            n_pairs++;
        }
        inner = mk_let(e, sp, pairs, n_pairs, inner);
    }
    /* Wrap the tests outermost-first (field order). */
    for (uint32_t f = pat->as.list.len; f-- > 1; ) {
        const Form *q = pat->as.list.items[f];
        if (!match_subpat_is_test(q)) continue;
        Form *fv = field_syms[f - 1];
        if (q->tag == F_LIST) {
            if (q->as.list.len < 1 || q->as.list.items[0]->tag != F_SYM ||
                !elab_lookup_ctor(e, q->as.list.items[0]->as.sym)) {
                diag_emit(DIAG_ERROR, q->span,
                          "match: nested pattern must be a constructor "
                          "application or a scalar literal");
                *err = true;
                return NULL;
            }
            /* (match fv <subpat> inner _ next) */
            Form *items[6];
            items[0] = form_sym(e->arena, sp, e->sym_match);
            items[1] = fv;
            items[2] = (Form *)q;
            items[3] = inner;
            items[4] = form_sym(e->arena, sp, intern_cstr(e->st, "_"));
            items[5] = next;
            inner = mlist(e, sp, items, 6);
        } else {
            /* (if (= fv <lit>) inner next) */
            Form *cmp[3];
            cmp[0] = form_sym(e->arena, sp, intern_cstr(e->st, "="));
            cmp[1] = fv;
            cmp[2] = (Form *)q;
            Form *test = mlist(e, sp, cmp, 3);
            Form *items[4];
            items[0] = form_sym(e->arena, sp, e->sym_if);
            items[1] = test;
            items[2] = inner;
            items[3] = next;
            inner = mlist(e, sp, items, 4);
        }
    }
    return inner;
}

/* Lower every nesting constructor group in `call`.  Returns the rewritten
 * form, NULL when there is nothing to lower (the caller proceeds with the
 * original), and NULL with *err set when a diagnostic was emitted.
 * `arms_base` is the index of the first arm (2, or 3 behind a marker). */
static Form *match_lower_nested_patterns(Elab *e, const Form *call,
                                         uint32_t arms_base, bool *err) {
    *err = false;
    uint32_t len = call->as.list.len;
    uint32_t arm_pat[256], arm_body[256];
    Form    *arm_guard[256];
    uint32_t n_arms = 0;
    {
        uint32_t idx = arms_base;
        while (idx < len) {
            if (n_arms >= 256) return NULL;   /* the caller diagnoses the cap */
            arm_pat[n_arms]   = idx++;
            arm_guard[n_arms] = NULL;
            if (idx + 1 < len && call->as.list.items[idx]->tag == F_SYM &&
                call->as.list.items[idx]->as.sym == e->sym_when) {
                arm_guard[n_arms] = call->as.list.items[idx + 1];
                idx += 2;
            }
            if (idx >= len) return NULL;      /* malformed; the caller diagnoses */
            arm_body[n_arms] = idx++;
            n_arms++;
        }
    }
    bool any_nested = false;
    for (uint32_t i = 0; i < n_arms && !any_nested; i++)
        any_nested = match_pattern_has_nested(e, call->as.list.items[arm_pat[i]]);
    if (!any_nested) return NULL;

    CtorDef *arm_ctor[256];
    bool     arm_generic[256];
    for (uint32_t i = 0; i < n_arms; i++) {
        const Form *p = call->as.list.items[arm_pat[i]];
        arm_ctor[i]    = NULL;
        arm_generic[i] = (p->tag == F_SYM);
        if (p->tag == F_LIST && p->as.list.len >= 1 &&
            p->as.list.items[0]->tag == F_SYM)
            arm_ctor[i] = elab_lookup_ctor(e, p->as.list.items[0]->as.sym);
    }

    Form *scrut_sym = mk_fresh_sym(e, call->span, "__ms_");
    bool  used_scrut_sym = false;

    /* Output arms: pattern/body pairs (guards are folded into the chain). */
    Form *out_pat[256], *out_body[256];
    Form *out_guard[256];
    uint32_t n_out = 0;
    bool consumed[256];
    for (uint32_t i = 0; i < n_arms; i++) consumed[i] = false;

    for (uint32_t i = 0; i < n_arms; i++) {
        if (consumed[i]) continue;
        CtorDef *c = arm_ctor[i];
        bool group_nests = false;
        uint32_t group[256], n_group = 0;
        if (c) {
            for (uint32_t j = i; j < n_arms; j++) {
                if (arm_ctor[j] != c) continue;
                group[n_group++] = j;
                if (match_pattern_has_nested(e, call->as.list.items[arm_pat[j]]))
                    group_nests = true;
            }
        }
        if (!c || !group_nests) {
            out_pat[n_out]   = call->as.list.items[arm_pat[i]];
            out_guard[n_out] = arm_guard[i];
            out_body[n_out]  = call->as.list.items[arm_body[i]];
            n_out++;
            continue;
        }
        for (uint32_t gi = 0; gi < n_group; gi++) consumed[group[gi]] = true;

        Span sp = call->as.list.items[arm_pat[i]]->span;
        /* A by-name (`:field var`) arm inside a nesting group is not lowered. */
        for (uint32_t gi = 0; gi < n_group; gi++) {
            const Form *p = call->as.list.items[arm_pat[group[gi]]];
            if (p->as.list.len >= 2 && p->as.list.items[1]->tag == F_KEYWORD) {
                diag_emit(DIAG_ERROR, p->span,
                          "match: by-name (`:field var`) and nested patterns "
                          "cannot be mixed in the arms for constructor '%s'",
                          c->name);
                *err = true;
                return NULL;
            }
            if (p->as.list.len - 1 != c->n_fields) {
                diag_emit(DIAG_ERROR, p->span,
                          "match: constructor '%s' expects %u fields, got %u",
                          c->name, c->n_fields, p->as.list.len - 1);
                *err = true;
                return NULL;
            }
        }

        /* Fresh binders for the constructor's fields. */
        Form *field_syms[64];
        if (c->n_fields > 64) {
            diag_emit(DIAG_ERROR, sp,
                      "match: nested patterns support at most 64 fields");
            *err = true;
            return NULL;
        }
        for (uint32_t f = 0; f < c->n_fields; f++)
            field_syms[f] = mk_fresh_sym(e, sp, "__mp_");

        /* Fallback: the first generic arm after the group's last arm. */
        Form *fallback = NULL;
        uint32_t last = group[n_group - 1];
        for (uint32_t j = last + 1; j < n_arms; j++) {
            if (!arm_generic[j]) continue;
            if (arm_guard[j]) break;   /* a guarded catch-all can fail too */
            const Form *gp = call->as.list.items[arm_pat[j]];
            Form *gb = call->as.list.items[arm_body[j]];
            if (gp->as.sym == intern_cstr(e->st, "_")) {
                fallback = gb;
            } else {
                Form *pairs[2];
                pairs[0] = (Form *)gp;
                pairs[1] = scrut_sym;
                used_scrut_sym = true;
                fallback = mk_let(e, gp->span, pairs, 1, gb);
            }
            break;
        }
        bool falls = match_group_falls_through(e, call, arm_pat, arm_guard,
                                               group, n_group);
        if (!fallback) {
            if (falls) {
                diag_emit(DIAG_ERROR, sp,
                          "match: the nested patterns for constructor '%s' are "
                          "not exhaustive -- add a `(%s ...)` arm binding plain "
                          "names, or a `_` arm after it",
                          c->name, c->name);
                *err = true;
                return NULL;
            }
            /* Unreachable: the group provably covers every value of '%s'. */
            Form *items[2];
            items[0] = form_sym(e->arena, sp, intern_cstr(e->st, "panic"));
            items[1] = form_str(e->arena, sp,
                                "match: nested pattern fallthrough (unreachable)",
                                (uint32_t)strlen(
                                "match: nested pattern fallthrough (unreachable)"));
            fallback = mlist(e, sp, items, 2);
        }

        /* Chain the group's arms, last first. */
        Form *chain = fallback;
        for (uint32_t gi = n_group; gi-- > 0; ) {
            uint32_t ai = group[gi];
            chain = match_build_arm_test(e, call->as.list.items[arm_pat[ai]],
                                         arm_guard[ai],
                                         call->as.list.items[arm_body[ai]],
                                         field_syms, chain, err);
            if (!chain) return NULL;
        }

        /* (C __mp_0 ... __mp_{n-1}) */
        Form *pitems[65];
        pitems[0] = call->as.list.items[arm_pat[i]]->as.list.items[0];
        for (uint32_t f = 0; f < c->n_fields; f++) pitems[f + 1] = field_syms[f];
        out_pat[n_out]   = mlist(e, sp, pitems, c->n_fields + 1);
        out_guard[n_out] = NULL;
        out_body[n_out]  = chain;
        n_out++;
    }

    /* Rebuild the match form. */
    uint32_t cap = arms_base + n_out * 4;
    Form **items = (Form **)arena_alloc(e->arena, cap * sizeof(Form *));
    uint32_t k = 0;
    for (uint32_t b = 0; b < arms_base; b++) items[k++] = call->as.list.items[b];
    if (used_scrut_sym) items[arms_base - 1] = scrut_sym;
    for (uint32_t a = 0; a < n_out; a++) {
        items[k++] = out_pat[a];
        if (out_guard[a]) {
            items[k++] = form_sym(e->arena, out_pat[a]->span, e->sym_when);
            items[k++] = out_guard[a];
        }
        items[k++] = out_body[a];
    }
    Form *lowered = form_list(e->arena, call->span, items, k);
    if (!used_scrut_sym) return lowered;

    Form *pairs[2];
    pairs[0] = scrut_sym;
    pairs[1] = call->as.list.items[arms_base - 1];
    return mk_let(e, call->span, pairs, 1, lowered);
}

Expr *elab_match(Elab *e, const Form *call) {
    /* call->as.list.items[0] = "match"
     * call->as.list.items[1] = scrutinee
     * call->as.list.items[2..] = pattern body pattern body ... */
    if (call->as.list.len < 2) {
        diag_emit(DIAG_ERROR, call->span,
                  "match requires a scrutinee: (match scrutinee pattern body ...)");
        return NULL;
    }
    /* Sum-types plan T6: optional `^non-exhaustive` opt-out marker.
     * Placed immediately after `match`, before the scrutinee:
     *   (match ^non-exhaustive scrutinee (Left l) ... )
     * When present, the non-exhaustiveness diagnostic on a known sum/ADT
     * scrutinee is suppressed -- the programmer asserts they have proven
     * exhaustiveness (or coverage of overlap) by other means.  The marker
     * is spliced out so the rest of the function sees an ordinary match.
     *
     * It used to be spelled `#fx{NonExhaustive}` -- an attribute borrowing
     * the effect-row brackets (effect-row-honesty-plan W0).  That spelling is
     * still accepted, with TUR-D0004 pointing at the new one. */
    bool nonexhaustive_optout = false;
    const Form *m1 = call->as.list.items[1];
    bool marker_sym = (m1->tag == F_SYM && m1->as.sym == e->sym_caret_non_exhaustive);
    if (marker_sym || m1->tag == F_MAP) {
        if (m1->tag == F_MAP) {
            warn_legacy_fx_row(call->as.list.items[1]);
            const Form *marker = m1;
            for (uint32_t mi = 0; mi < marker->as.list.len; mi++) {
                if (marker->as.list.items[mi]->tag == F_SYM &&
                    marker->as.list.items[mi]->as.sym == e->sym_non_exhaustive_legacy) {
                    nonexhaustive_optout = true;
                } else {
                    diag_emit(DIAG_ERROR, marker->span,
                              "match: unknown marker in '#fx{...}'; the only "
                              "match marker is ^non-exhaustive: "
                              "(match ^non-exhaustive scrutinee pattern body ...)");
                    return NULL;
                }
            }
            if (nonexhaustive_optout) {
                diag_emit_with_code(g_werror_deprecated ? DIAG_ERROR : DIAG_WARNING,
                    marker->span, TUR_D0004_NONEXHAUSTIVE_FX_MARKER,
                    "'#fx{NonExhaustive}' is deprecated; write "
                    "(match ^non-exhaustive scrutinee ...) -- it is a match "
                    "attribute, not an effect");
                if (g_werror_deprecated) return NULL;
            }
        } else {
            nonexhaustive_optout = true;
        }
        if (call->as.list.len < 3) {
            diag_emit(DIAG_ERROR, call->span,
                      "match requires a scrutinee after the '^non-exhaustive' "
                      "marker: (match ^non-exhaustive scrutinee pattern body ...)");
            return NULL;
        }
        /* Splice the marker out: rebuild the call form without items[1]. */
        uint32_t new_len = call->as.list.len - 1;
        Form **new_items = (Form **)arena_alloc(e->arena, new_len * sizeof(Form *));
        new_items[0] = call->as.list.items[0];               /* 'match' */
        for (uint32_t k = 2; k < call->as.list.len; k++)
            new_items[k - 1] = call->as.list.items[k];
        call = form_list(e->arena, call->span, new_items, new_len);
    }
    /* match-nested-constructor-patterns: fold nested constructor / literal
     * sub-patterns into the flat one-level-per-arm shape the rest of this
     * function understands.  A no-op (and a single pattern scan) for every
     * match that does not nest. */
    {
        bool lower_err = false;
        Form *lowered = match_lower_nested_patterns(e, call, 2, &lower_err);
        if (lower_err) return NULL;
        if (lowered) {
            if (nonexhaustive_optout) {
                /* Re-attach the marker the splice above removed, in the
                 * current spelling (the legacy one was already reported). */
                Form *marker = form_sym(e->arena, call->span,
                                        e->sym_caret_non_exhaustive);
                const Form *inner = lowered;
                bool wrapped = (inner->as.list.items[0]->tag == F_SYM &&
                                inner->as.list.items[0]->as.sym == e->sym_let);
                const Form *m = wrapped ? inner->as.list.items[2] : inner;
                uint32_t n = m->as.list.len + 1;
                Form **mi = (Form **)arena_alloc(e->arena, n * sizeof(Form *));
                mi[0] = m->as.list.items[0];
                mi[1] = marker;
                for (uint32_t q = 1; q < m->as.list.len; q++) mi[q + 1] = m->as.list.items[q];
                Form *m2 = form_list(e->arena, m->span, mi, n);
                if (wrapped) {
                    Form *li[3];
                    li[0] = inner->as.list.items[0];
                    li[1] = inner->as.list.items[1];
                    li[2] = m2;
                    lowered = form_list(e->arena, inner->span, li, 3);
                } else {
                    lowered = m2;
                }
            }
            if (getenv("TUR_DUMP_MATCH_LOWER")) {
                Buf db; buf_init(&db);
                form_print(&db, lowered);
                buf_putc(&db, '\0');
                fprintf(stderr, "match lowered:\n%s\n", db.data);
                buf_free(&db);
            }
            return elab_form(e, lowered);
        }
    }

    /* Phase G4: Pre-scan arms to count and find per-arm start indices.
     * Arms can be (pat body) or (pat when guard body). */
    uint32_t arm_start[256];    /* start index of each arm's pattern */
    bool arm_has_guard[256];    /* whether the arm has a when-guard */
    uint32_t n_arms = 0;
    {
        uint32_t idx = 2; /* skip 'match' and scrutinee */
        while (idx < call->as.list.len) {
            if (n_arms >= 256) {
                diag_emit(DIAG_ERROR, call->span, "match: too many arms (max 256)");
                return NULL;
            }
            arm_start[n_arms] = idx;
            arm_has_guard[n_arms] = false;
            idx++; /* skip pattern */
            /* Check for optional 'when guard' */
            if (idx + 1 < call->as.list.len &&
                call->as.list.items[idx]->tag == F_SYM &&
                call->as.list.items[idx]->as.sym == e->sym_when) {
                arm_has_guard[n_arms] = true;
                idx += 2; /* skip 'when' and guard expr */
            }
            idx++; /* skip body */
            n_arms++;
        }
        /* Verify last arm ends exactly at list end */
        if (idx != call->as.list.len) {
            diag_emit(DIAG_ERROR, call->span,
                      "match: malformed arm list (missing body for last arm?)");
            return NULL;
        }
    }
    if (n_arms == 0) {
        diag_emit(DIAG_ERROR, call->span,
                  "match requires at least one arm: (match scrutinee pattern body)");
        return NULL;
    }

    /* CONV-S3 (struct/ADT convergence): the old `match` on a *struct* value
     * dispatch is dead post structdef-retirement (DS-D) -- a lowered struct is
     * a single-variant record ADT, so it flows through the normal ADT match
     * path below.  No Type is ever TY_STRUCT, so there is nothing to detect. */

    /* Elaborate scrutinee */
    Expr *scrutinee = elab_form(e, call->as.list.items[1]);
    if (!scrutinee) return NULL;

    /* saffron-lang-plan S5/D4 (row 7): `match` on an `any` scrutinee.
     *
     * In Saffron an unannotated parameter is `any`, so `(match xs (Cons h t)
     * ... )` reaches here with no ADT to match against.  The interpreter needed
     * nothing for this -- a TuriValue carries its own constructor, so matching
     * works whatever the static type claimed -- but a compiled `match` reads a
     * tag out of a C aggregate, and `tur_adt_Lst __scrut_v = xs;` where `xs` is
     * a two-word box is "invalid initializer".
     *
     * The narrow is D5's seam, applied to the scrutinee position: the arms name
     * an ADT, so unbox to it and let the ordinary match machinery run.  A value
     * of the wrong type panics with the standard cast message rather than
     * reinterpreting the payload -- the same trade D5 already made for
     * arguments, and the reason `cast` was built checked.
     *
     * ONE ADT only.  Arms drawn from two different ADTs would need a box-id
     * switch over unrelated layouts, which is a bigger thing than a narrow and
     * has no arm-unifier story yet; leaving it to the existing diagnostic is
     * better than narrowing to whichever ADT happened to be named first. */
    if (scrutinee->type.kind == TY_ANY && lang_span_is_dynamic(call->span)) {
        AdtDef *only = NULL;
        bool mixed = false;
        for (uint32_t ai = 0; ai < n_arms && !mixed; ai++) {
            Form *pf = call->as.list.items[2 + ai * 2];
            if (!pf || pf->tag != F_LIST || pf->as.list.len == 0) continue;
            Form *hd = pf->as.list.items[0];
            if (!hd || hd->tag != F_SYM) continue;
            CtorDef *cd = elab_lookup_ctor(e, hd->as.sym);
            if (!cd || !cd->adt) continue;
            if (!only) only = cd->adt;
            else if (only != cd->adt) mixed = true;
        }
        if (only && !mixed) {
            Type target = type_adt(only);
            bool do_narrow = true;
            /* saffron-match-any-scrutinee-on-parametric-adt-emits-bad-c: a
             * PARAMETRIC ADT narrows to itself applied to `any`, once per type
             * parameter.  This used to be refused outright (`n_type_params ==
             * 0`), which left the emitter casting the 16-byte `tur_tagged_t`
             * straight to a pointer -- uncompilable C on the one shape the
             * dialect exists to make writable.
             *
             * Sound only because the ctor call now BUILDS at `any` in a Saffron
             * file (elab_call.c), so the box really does hold a `(Shape any)`.
             * Relaxing this guard on its own was tried and reverted: the narrow
             * fired and then panicked `cast: any holds a different
             * instantiation of Shape`, because `(Sq 6)` had built a
             * `(Shape int)`. The two halves only work together.
             *
             * `type_adt` hardcodes KIND_STAR, so the arrow kind has to be
             * restored before applying, or the application is a TUR-E0012 kind
             * mismatch -- the same restore the ctor result path in elab_call.c
             * does. */
            /* A GADT is deliberately excluded, and it must be excluded LOUDLY.
             *
             * `(defgadt Shape [a] (Sq int : (Shape int)))` pins its own result
             * INDEX, so its box really does hold a `(Shape int)` -- the ctor
             * widen in elab_call.c does not touch it, and cannot: the indices
             * are the whole point of a GADT, and erasing them to `any` would
             * discard the type-level information the author wrote down.
             * Narrowing to `(Shape any)` anyway compiles and then panics
             * `cast: any holds a different instantiation`, which is a WORSE
             * failure than the uncompilable C this replaced -- the report says
             * so explicitly, and the first attempt at this fix died there.
             *
             * So decline, and say why. Before this the user got a C compiler
             * error about aggregates, naming nothing they wrote. */
            /* "Indexed" means a result-type ARGUMENT that is not simply the
             * ADT's own type parameter.  A `defgadt` whose ctors all return
             * `(Box a)` writes result types but pins nothing -- it indexes
             * exactly as a `defdata` does -- and narrowing it to `(Box any)` is
             * both sound and necessary: `errors/saffron-gadt-skolem-escape`
             * asserts that such a match still reaches the skolem-escape check,
             * which it cannot do if the narrow declines first.  Testing merely
             * for `result_type_form` conflated the two and swallowed that
             * fixture's diagnostic with this one. */
            bool gadt_indexed = false;
            for (uint32_t ci = 0; ci < only->n_ctors && !gadt_indexed; ci++) {
                const CtorDef *cd = only->ctors[ci];
                if (!cd || !cd->result_type_form) continue;
                const Form *rt = cd->result_type_form;
                if (rt->tag != F_LIST) continue;
                for (uint32_t ai2 = 1; ai2 < rt->as.list.len; ai2++) {
                    const Form *arg = rt->as.list.items[ai2];
                    bool is_own_param = false;
                    if (arg && arg->tag == F_SYM) {
                        for (uint8_t pi = 0; pi < only->n_type_params; pi++)
                            if (only->type_params[pi] && arg->as.sym->name &&
                                strcmp(only->type_params[pi], arg->as.sym->name) == 0) {
                                is_own_param = true; break;
                            }
                    }
                    if (!is_own_param) { gadt_indexed = true; break; }
                }
            }
            if (only->is_gadt && only->n_type_params > 0) {
                /* Every parametric GADT declines the narrow, which is what it
                 * did before this change -- an UNINDEXED one
                 * (`errors/saffron-gadt-skolem-escape`) must keep reaching the
                 * skolem-escape check that fixture pins, and narrowing it to
                 * `(Box any)` instead types the arm binder `tur_tagged_t` over
                 * a carrier field (`invalid initializer`). Declining is the old
                 * path and the right one.
                 *
                 * An INDEXED one additionally gets a diagnostic. Its old
                 * behaviour was a C compiler error about aggregates, naming
                 * nothing the user wrote; this is report direction 3 for the
                 * one shape direction 1 cannot serve. */
                if (gadt_indexed) {
                    diag_emit(DIAG_ERROR, scrutinee->span,
                              "cannot match an 'any' scrutinee against '%s': its "
                              "constructors pin their own result type, so the "
                              "box's instantiation is not decidable here",
                              only->name);
                    diag_emit(DIAG_HELP, scrutinee->span,
                              "annotate the scrutinee with the instantiation you "
                              "expect -- `[s : (%s int)]` -- or narrow with "
                              "`cast` before matching",
                              only->name);
                    return NULL;
                }
                do_narrow = false;   /* unindexed: decline, as before */
            } else if (only->n_type_params > 0) {
                /* `type_from_kind`, NOT a memset-zeroed Type: `CK_UNIQUE` is 0
                 * and `CK_MOVE` is an alias for it, so a zeroed `any` is
                 * MOVE-typed.  The arm binder inherits the type argument, so
                 * `(match s (Sq w) (* w w))` then failed TUR-E0005
                 * use-after-move on its second `w` -- while the same `any` as a
                 * PARAMETER was fine, because parameters are built through this
                 * helper.  Building it the same way is what makes an arm binder
                 * and a parameter agree, which is the property the dialect
                 * needs. */
                Type any_t = type_from_kind(TY_ANY);
                target.hkt_kind = kind_for_arity(only->n_type_params);
                for (uint8_t pi = 0; pi < only->n_type_params; pi++)
                    target = type_app(e->arena, target, any_t, scrutinee->span);
            }
            if (do_narrow) {
                Expr *nar = elab_any_unbox_to(e, scrutinee, target, scrutinee->span);
                if (nar) scrutinee = nar;
            }
        }
    }

    /* IT1: Union type match — when scrutinee is TY_UNION, handle type-narrowing patterns.
     * Pattern syntax: (varname : TypeName) or bare _ / variable for wildcard.
     * Returns early via the union match path. */
    if (scrutinee->type.kind == TY_UNION) {
        Type *union_t = &scrutinee->type;
        uint8_t n_members = union_t->as.union_.n_members;

        /* Track which union members are covered */
        bool *member_covered = (bool *)calloc(n_members, sizeof(bool));
        bool has_wildcard = false;
        Type result_type = TYPE_UNKNOWN;

        MatchArm *arms = (MatchArm *)arena_alloc(e->arena, n_arms * sizeof(MatchArm));

        for (uint32_t ai = 0; ai < n_arms; ai++) {
            Form *pat_form = call->as.list.items[2 + ai * 2];
            Form *body_form = call->as.list.items[3 + ai * 2];
            MatchPattern *pat = &arms[ai].pattern;
            memset(pat, 0, sizeof(MatchPattern));
            /* Union match arms carry no guard; initialise the field so readers
             * (e.g. scan_adt_apps_in_expr) do not dereference arena garbage.
             * The arm array is arena-allocated and never zeroed. */
            arms[ai].guard = NULL;

            /* Wildcard: bare _ or bare variable symbol */
            if (pat_form->tag == F_SYM) {
                const Symbol *sym_wildcard = intern_cstr(e->st, "_");
                if (pat_form->as.sym == sym_wildcard) {
                    pat->is_wildcard = true;
                } else {
                    pat->is_var = true;
                    pat->var_sym = pat_form->as.sym;
                }
                pat->union_member_idx = -1; /* IT4: wildcard arm — no specific member */
                has_wildcard = true;
                { bool _s = e->in_match_arm; e->in_match_arm = true;
                Expr *body = elab_form(e, body_form);
                e->in_match_arm = _s;
                if (!body) { free(member_covered); return NULL; }
                arms[ai].body = body;
                if (result_type.kind == TY_UNKNOWN) result_type = body->type; }
                continue;
            }

            /* Type-narrowing pattern: (varname : TypeName)
             * The reader collapses ': TypeName' into a single F_TYPE_ANN node, so the
             * list has 2 items: [F_SYM varname, F_TYPE_ANN{inner}].
             * The 3-item form [F_SYM varname, F_SYM ":", F_SYM TypeName] is also
             * supported for defgadt-style bare colon separators. */
            bool is_type_narrowing_2 = (pat_form->tag == F_LIST &&
                pat_form->as.list.len == 2 &&
                pat_form->as.list.items[0]->tag == F_SYM &&
                pat_form->as.list.items[1]->tag == F_TYPE_ANN);
            bool is_type_narrowing_3 = (pat_form->tag == F_LIST &&
                pat_form->as.list.len == 3 &&
                pat_form->as.list.items[0]->tag == F_SYM &&
                pat_form->as.list.items[1]->tag == F_SYM &&
                pat_form->as.list.items[1]->as.sym == e->sym_colon);
            if (is_type_narrowing_2 || is_type_narrowing_3) {
                Form *var_form  = pat_form->as.list.items[0];
                Form *type_form = is_type_narrowing_2
                    ? pat_form->as.list.items[1]->as.list.items[0]  /* inner of F_TYPE_ANN */
                    : pat_form->as.list.items[2];
                /* Parse narrowed type */
                Type *narrowed = type_expr_from_form(e, type_form, NULL, NULL, NULL, 0);
                if (!narrowed) { free(member_covered); return NULL; }

                /* Find which union member this pattern covers */
                int covered_member = -1;
                for (uint8_t um = 0; um < n_members; um++) {
                    if (union_t->as.union_.members[um] &&
                        type_eq(*narrowed, *union_t->as.union_.members[um])) {
                        covered_member = (int)um;
                        break;
                    }
                }
                if (covered_member < 0) {
                    diag_emit_with_code(DIAG_ERROR, pat_form->span,
                                        TUR_E0300_UNION_TYPE_MISMATCH,
                                        "match: type '%s' is not a member of union '%s'",
                                        type_name(*narrowed), type_name(*union_t));
                    free(member_covered);
                    return NULL;
                }
                member_covered[covered_member] = true;
                pat->union_member_idx = covered_member; /* IT4: record for tag-dispatch in emit.c */

                /* Introduce arm scope with the narrowed binding */
                Scope arm_scope;
                scope_init(&arm_scope, e->scope);
                Scope *saved_scope = e->scope;
                e->scope = &arm_scope;

                Binding *var_b = binding_new(e, var_form->as.sym, *narrowed,
                                             false, false, var_form->span);
                scope_add(&arm_scope, var_b);

                bool _s187 = e->in_match_arm; e->in_match_arm = true;
                Expr *body = elab_form(e, body_form);
                e->in_match_arm = _s187;
                e->scope = saved_scope;
                scope_free(&arm_scope);
                if (!body) { free(member_covered); return NULL; }

                /* Record the binding in the pattern */
                pat->is_var = true;
                pat->var_sym = var_form->as.sym;
                pat->n_bindings = 1;
                pat->bindings = (Binding **)arena_alloc(e->arena, sizeof(Binding *));
                pat->bindings[0] = var_b;

                arms[ai].body = body;
                if (result_type.kind == TY_UNKNOWN) result_type = body->type;
                continue;
            }

            /* Unrecognized pattern for union match */
            diag_emit(DIAG_ERROR, pat_form->span,
                      "match on union type: expected '(varname : Type)' or wildcard '_', got unexpected pattern");
            free(member_covered);
            return NULL;
        }

        /* Exhaustiveness check: every union member must be covered */
        if (!has_wildcard) {
            for (uint8_t um = 0; um < n_members; um++) {
                if (!member_covered[um] && union_t->as.union_.members[um]) {
                    diag_emit_with_code(DIAG_ERROR, call->span,
                                        TUR_E0301_NON_EXHAUSTIVE_UNION_MATCH,
                                        "match on union type '%s': missing arm for member '%s'",
                                        type_name(*union_t),
                                        type_name(*union_t->as.union_.members[um]));
                    free(member_covered);
                    return NULL;
                }
            }
        }
        free(member_covered);

        if (result_type.kind == TY_UNKNOWN) result_type = TYPE_NIL;
        Expr *out = expr_new(e->arena, EX_MATCH, result_type, call->span);
        out->as.match_.scrutinee = scrutinee;
        out->as.match_.arms = arms;
        out->as.match_.n_arms = n_arms;
        return out;
    }

    /* SS2: Session offer match — when scrutinee is TY_SESSION_OFFER, handle
     * Left/Right branch patterns.  The scrutinee carries the tag as int64_t
     * and keeps the channel pointer in val_exprs[0] for arm binding. */
    if (scrutinee->type.kind == TY_SESSION_OFFER) {
        MatchArm *arms = (MatchArm *)arena_alloc(e->arena, n_arms * sizeof(MatchArm));
        Type result_type = TYPE_UNKNOWN;
        const Symbol *sym_left  = intern_cstr(e->st, "Left");
        const Symbol *sym_right = intern_cstr(e->st, "Right");

        for (uint32_t ai = 0; ai < n_arms; ai++) {
            uint32_t base = arm_start[ai];
            Form *pat_form  = call->as.list.items[base];
            Form *body_form = call->as.list.items[arm_has_guard[ai] ? base + 3 : base + 1];
            MatchPattern *pat = &arms[ai].pattern;
            memset(pat, 0, sizeof(MatchPattern));
            arms[ai].guard = NULL;
            pat->union_member_idx = -1;

            if (pat_form->tag == F_SYM) {
                /* Wildcard */
                pat->is_wildcard = true;
                { bool _s = e->in_match_arm; e->in_match_arm = true;
                Expr *body = elab_form(e, body_form);
                e->in_match_arm = _s;
                if (!body) return NULL;
                arms[ai].body = body;
                if (result_type.kind == TY_UNKNOWN) result_type = body->type; }
                continue;
            }
            if (pat_form->tag == F_LIST && pat_form->as.list.len == 2 &&
                pat_form->as.list.items[0]->tag == F_SYM &&
                pat_form->as.list.items[1]->tag == F_SYM) {
                const Symbol *ctor_sym = pat_form->as.list.items[0]->as.sym;
                Form *var_form = pat_form->as.list.items[1];
                int arm_tag;
                Type arm_type;
                if (ctor_sym == sym_left) {
                    arm_tag = 0;
                    arm_type = scrutinee->type.as.session_.fst
                        ? *scrutinee->type.as.session_.fst : type_from_kind(TY_PTR_VOID);
                } else if (ctor_sym == sym_right) {
                    arm_tag = 1;
                    arm_type = scrutinee->type.as.session_.snd
                        ? *scrutinee->type.as.session_.snd : type_from_kind(TY_PTR_VOID);
                } else {
                    diag_emit(DIAG_ERROR, pat_form->span,
                              "session offer match: expected Left or Right pattern, got '%s'",
                              ctor_sym->name);
                    return NULL;
                }
                pat->union_member_idx = arm_tag;
                pat->n_bindings = 1;
                pat->bindings = (Binding **)arena_alloc(e->arena, sizeof(Binding *));
                Binding *fb = binding_new(e, var_form->as.sym, arm_type,
                                          false, false, var_form->span);
                fb->is_match_binding = true;
                if (arm_type.copy_kind == CK_LINEAR) fb->is_linear = true;
                pat->bindings[0] = fb;
                Scope arm_scope;
                scope_init(&arm_scope, e->scope);
                Scope *saved_scope = e->scope;
                e->scope = &arm_scope;
                scope_add(&arm_scope, fb);
                bool _s295 = e->in_match_arm; e->in_match_arm = true;
                Expr *body = elab_form(e, body_form);
                e->in_match_arm = _s295;
                e->scope = saved_scope;
                scope_free(&arm_scope);
                if (!body) return NULL;
                arms[ai].body = body;
                if (result_type.kind == TY_UNKNOWN) result_type = body->type;
                continue;
            }
            diag_emit(DIAG_ERROR, pat_form->span,
                      "session offer match: expected (Left var) or (Right var) pattern");
            return NULL;
        }
        if (result_type.kind == TY_UNKNOWN) result_type = TYPE_NIL;
        Expr *out = expr_new(e->arena, EX_MATCH, result_type, call->span);
        out->as.match_.scrutinee = scrutinee;
        out->as.match_.arms = arms;
        out->as.match_.n_arms = n_arms;
        return out;
    }

    /* Phase S4-lit: Literal match — scrutinee is a primitive (non-ADT) type.
     * Patterns are literals (F_INT/F_BOOL/F_FLOAT/F_STR), _ wildcards, or
     * bare variable captures.  Emits EX_MATCH with is_literal arms. */
    {
        TypeKind _sk = scrutinee->type.kind;
        bool _is_prim = (_sk == TY_INT    || _sk == TY_BOOL   || _sk == TY_FLOAT  ||
                         _sk == TY_CSTR   || _sk == TY_INT8   || _sk == TY_INT16  ||
                         _sk == TY_INT32  || _sk == TY_INT64  || _sk == TY_UINT8  ||
                         _sk == TY_UINT16 || _sk == TY_UINT32 || _sk == TY_UINT64 ||
                         _sk == TY_FLOAT32 || _sk == TY_FLOAT64);
        /* Also trigger when first non-wildcard arm is a literal pattern */
        if (!_is_prim && (_sk == TY_UNKNOWN || _sk == TY_NIL)) {
            for (uint32_t _ai = 0; _ai < n_arms; _ai++) {
                FormTag _ft = call->as.list.items[arm_start[_ai]]->tag;
                if (_ft == F_INT || _ft == F_BOOL || _ft == F_FLOAT ||
                    _ft == F_STR || _ft == F_NIL) {
                    _is_prim = true;
                    break;
                }
                if (_ft != F_SYM) break; /* constructor found — not literal match */
            }
        }
        /* If any arm is an ADT constructor call (F_LIST), defer to the ADT path
         * even when the scrutinee type looks primitive (e.g. unannotated param
         * that defaulted to TY_INT — the ADT path infers the type from patterns). */
        if (_is_prim) {
            for (uint32_t _ai = 0; _ai < n_arms; _ai++) {
                if (call->as.list.items[arm_start[_ai]]->tag == F_LIST) {
                    _is_prim = false;
                    break;
                }
            }
        }
        if (_is_prim) {
            MatchArm *lit_arms = (MatchArm *)arena_alloc(e->arena, n_arms * sizeof(MatchArm));
            Type lit_result = TYPE_UNKNOWN;
            for (uint32_t ai = 0; ai < n_arms; ai++) {
                uint32_t base     = arm_start[ai];
                Form *pat_form    = call->as.list.items[base];
                Form *guard_raw   = arm_has_guard[ai] ? call->as.list.items[base + 2] : NULL;
                Form *body_form   = call->as.list.items[arm_has_guard[ai] ? base + 3 : base + 1];
                MatchPattern *pat = &lit_arms[ai].pattern;
                memset(pat, 0, sizeof(MatchPattern));
                pat->union_member_idx = -1;
                lit_arms[ai].guard = NULL;

                if (pat_form->tag == F_SYM) {
                    const Symbol *sym_wc = intern_cstr(e->st, "_");
                    if (pat_form->as.sym == sym_wc) {
                        pat->is_wildcard = true;
                    } else {
                        pat->is_var     = true;
                        pat->var_sym    = pat_form->as.sym;
                    }
                } else if (pat_form->tag == F_INT  || pat_form->tag == F_BOOL ||
                           pat_form->tag == F_FLOAT || pat_form->tag == F_STR  ||
                           pat_form->tag == F_NIL) {
                    pat->is_literal  = true;
                    pat->lit_kind    = (int8_t)pat_form->tag;
                    switch (pat_form->tag) {
                    case F_INT:   pat->lit_int   = pat_form->as.i; break;
                    case F_BOOL:  pat->lit_bool  = pat_form->as.b; break;
                    case F_FLOAT: pat->lit_float = pat_form->as.f; break;
                    case F_STR:   pat->lit_cstr  = pat_form->as.s.p; break;
                    default: break;
                    }
                } else {
                    diag_emit(DIAG_ERROR, pat_form->span,
                              "match: pattern must be a literal value, _ wildcard, "
                              "or variable capture when matching a primitive type");
                    return NULL;
                }

                /* Elaborate the guard and the body in the arm's scope: a var
                 * pattern's binder must be visible to its own when-guard, not
                 * just to the body.  The ADT path already does this (it keeps
                 * the arm scope live across both); the scalar path used to
                 * elaborate the guard before the binding existed, so
                 * `(match p x when (> x 2) x _ 0)` failed with "unbound symbol
                 * 'x'".  Wildcard/literal patterns bind nothing, so for them
                 * the two orders are equivalent. */
                Expr *body;
                Expr *guard = NULL;
                bool arm_ok = true;
                bool _s_lit = e->in_match_arm; e->in_match_arm = true;
                if (pat->is_var && pat->var_sym) {
                    Binding *vb = binding_new(e, pat->var_sym, scrutinee->type,
                                              false, false, pat_form->span);
                    pat->var_binding = vb;
                    Scope arm_sc;
                    scope_init(&arm_sc, e->scope);
                    Scope *saved_sc = e->scope;
                    e->scope = &arm_sc;
                    scope_add(&arm_sc, vb);
                    if (guard_raw) {
                        guard = elab_form(e, guard_raw);
                        if (!guard) arm_ok = false;
                    }
                    body = arm_ok ? elab_form(e, body_form) : NULL;
                    e->scope = saved_sc;
                    scope_free(&arm_sc);
                } else {
                    if (guard_raw) {
                        guard = elab_form(e, guard_raw);
                        if (!guard) arm_ok = false;
                    }
                    body = arm_ok ? elab_form(e, body_form) : NULL;
                }
                e->in_match_arm = _s_lit;
                if (!arm_ok || !body) return NULL;
                /* Parity with the ADT path: a when-guard must be a bool. */
                if (guard && guard->type.kind != TY_BOOL) {
                    diag_emit(DIAG_ERROR, guard_raw->span,
                              "match: when-guard must have type bool, got %s",
                              typekind_to_string(guard->type.kind));
                    return NULL;
                }
                lit_arms[ai].guard = guard;
                lit_arms[ai].body = body;
                if (lit_result.kind == TY_UNKNOWN) lit_result = body->type;
                /* match-join-of-ctor-arm-and-byvalue-arm-reaches-cc: the
                 * result is the first arm's type, so `(match c true (Ok 1.25)
                 * false r)` was the carrier `(Result float B)` over a by-value
                 * `r`.  A later arm that is the concrete application of the
                 * same ADT wins, as it does in the ADT path's join
                 * (match_arm_type_compatible). */
                else if (type_has_open_slot(&lit_result) &&
                         body->type.kind == TY_APP &&
                         !type_has_open_slot(&body->type) &&
                         type_app_is_concrete_adt(&body->type) &&
                         type_adt_app_def(&body->type) == type_adt_app_def(&lit_result))
                    lit_result = body->type;
            }
            if (lit_result.kind == TY_UNKNOWN) lit_result = TYPE_NIL;
            Expr *out = expr_new(e->arena, EX_MATCH, lit_result, call->span);
            out->as.match_.scrutinee = scrutinee;
            out->as.match_.arms      = lit_arms;
            out->as.match_.n_arms    = n_arms;
            return out;
        }
    }

    /* If the scrutinee type is not already TY_ADT (e.g. an untyped param
     * that defaulted to TY_INT), or is TY_ADT with no def (e.g. return of
     * ADT-returning fn without result_full_type), infer the ADT from the
     * first constructor pattern in the arm list.
     * TP6: Also accept TY_APP chains whose base is a TY_ADT — these carry
     * concrete type arguments from an explicit type annotation. */
    {
        /* Unwrap TY_APP chain to find base type */
        const Type *base = &scrutinee->type;
        while (base && base->kind == TY_APP && base->as.app.fn) {
            base = base->as.app.fn;
        }
        if (base && base->kind == TY_ADT && base->as.adt_.def) {
            /* Scrutinee is a TY_APP(... TY_ADT) chain — valid, nothing to patch */
        } else if (scrutinee->type.kind != TY_ADT || !scrutinee->type.as.adt_.def) {
            AdtDef *inferred_adt = NULL;
            for (uint32_t ai = 0; ai < n_arms && !inferred_adt; ai++) {
                Form *pat_f = call->as.list.items[arm_start[ai]];
                if (pat_f->tag == F_LIST && pat_f->as.list.len >= 1 &&
                    pat_f->as.list.items[0]->tag == F_SYM) {
                    CtorDef *cd = elab_lookup_ctor(e, pat_f->as.list.items[0]->as.sym);
                    if (cd) inferred_adt = cd->adt;
                }
            }
            if (inferred_adt) {
                /* Patch the scrutinee type to the inferred ADT */
                scrutinee->type = type_adt(inferred_adt);
                /* CONV-S1/B2: when the scrutinee is a bare variable whose binding
                 * still carries the int64 default (an untyped param defaulted to
                 * TY_INT), propagate the inferred ADT back onto the binding.  The
                 * signature realiser (elab_fns.c) runs AFTER the body, so it picks
                 * this up and gives the param its ADT type -- making the parameter
                 * and the match body agree on the representation once the by-value
                 * gate flips.  Matching a value with constructor patterns proves it
                 * IS that ADT, so this never mis-types a genuine int.  No-op for
                 * emitted C while the gate is off (type_c_name(TY_ADT) is int64_t).
                 * Only refine a still-default TY_INT binding -- never an explicit
                 * annotation, which already carries its own type kind. */
                if (scrutinee->kind == EX_VAR && scrutinee->as.var.binding &&
                    scrutinee->as.var.binding->type.kind == TY_INT) {
                    scrutinee->as.var.binding->type = type_adt(inferred_adt);
                }
            } else {
                diag_emit(DIAG_ERROR, call->as.list.items[1]->span,
                          "match: scrutinee must be an ADT type, got %s",
                          typekind_to_string(scrutinee->type.kind));
                return NULL;
            }
        }
    }

    /* Extract the ADT def, looking through TY_APP chains */
    AdtDef *adt;
    {
        const Type *base = &scrutinee->type;
        while (base && base->kind == TY_APP && base->as.app.fn) {
            base = base->as.app.fn;
        }
        adt = (base && base->kind == TY_ADT) ? base->as.adt_.def
                                              : scrutinee->type.as.adt_.def;
    }

    /* CONV-S4N: per-arm variant narrowing target.  When the scrutinee is a
     * bare symbol bound to a variable, we narrow that binding's type to the
     * matched variant for the duration of each constructor arm (restoring it
     * after).  Reusing the binding -- rather than shadowing it with a fresh one
     * -- keeps the arm body pointing at the same C variable, so codegen is
     * unchanged; only the arm-local *type* changes so `with`/field-access can
     * see the proven variant. */
    Binding *scrut_narrow_binding =
        (call->as.list.items[1]->tag == F_SYM && scrutinee->kind == EX_VAR)
            ? scrutinee->as.var.binding
            : NULL;

    /* Allocate arms array */
    MatchArm *arms = (MatchArm *)arena_alloc(e->arena, n_arms * sizeof(MatchArm));

    /* Track covered constructors for exhaustiveness */
    bool *covered = (bool *)calloc(adt->n_ctors, sizeof(bool));
    bool has_wildcard = false;

    Type result_type = TYPE_UNKNOWN;

    /* LT1: Snapshot outer-scope linear consumed state before match arms.
     * We restore before each arm's body and verify consistency across arms at the end. */
    Binding **match_lin_bindings = NULL;
    bool *match_lin_before = NULL;
    uint32_t n_match_lin = 0;
    bool **arm_lin_states = NULL;
    bool *arm_diverges = NULL;
    n_match_lin = linear_state_snapshot_bindings(e->scope, &match_lin_bindings,
                                                 &match_lin_before);
    if (n_match_lin > 0) {
        arm_lin_states = (bool **)calloc(n_arms, sizeof(bool *));
    }
    arm_diverges = (bool *)calloc(n_arms, sizeof(bool));

    /* match-arm move state: arms are ALTERNATIVES, exactly like the two
     * branches of an `if`, so a value consumed in one arm must not read as
     * moved in the next.  `if` has rewound and merged this since forever
     * (elab_forms.c: before || (then && else)); `match` rewound only the
     * LINEAR state, so `(match t (TA) (f x) (TB) (f x))` was a spurious
     * TUR-E0005 while the identical if/else was accepted.  Snapshot here,
     * rewind before each arm, and merge with the same rule below -- a
     * diverging arm cannot reach the merge, so it does not vote. */
    Binding **match_move_bindings = NULL;
    bool     *match_move_before   = NULL;
    bool    **arm_move_states     = NULL;
    uint32_t  n_match_move = move_state_snapshot_bindings(e->scope,
                                 &match_move_bindings, &match_move_before);
    if (n_match_move > 0)
        arm_move_states = (bool **)calloc(n_arms, sizeof(bool *));

    for (uint32_t ai = 0; ai < n_arms; ai++) {
        uint32_t base = arm_start[ai];
        Form *pat_form = call->as.list.items[base];
        Form *guard_form_raw = arm_has_guard[ai]
            ? call->as.list.items[base + 2]
            : NULL;
        Form *body_form = call->as.list.items[arm_has_guard[ai] ? base + 3 : base + 1];

        MatchPattern *pat = &arms[ai].pattern;
        memset(pat, 0, sizeof(MatchPattern));
        arms[ai].guard = NULL;

        if (pat_form->tag == F_SYM) {
            /* Bare symbol: either _ wildcard or variable capture */
            const Symbol *sym = pat_form->as.sym;
            /* intern "_" */
            const Symbol *sym_wildcard = intern_cstr(e->st, "_");
            if (sym == sym_wildcard) {
                pat->is_wildcard = true;
                has_wildcard = true;
            } else {
                /* Variable binding — captures entire scrutinee.
                 *
                 * match-adt-var-arm-does-not-bind: this used to record
                 * `is_var`/`var_sym` and then elaborate the body with NO
                 * binding and no scope, so `(match o (OA n) n whole (g whole))`
                 * was "unbound symbol 'whole'" -- the ADT path's only usable
                 * catch-all was `_`, while the literal path 500 lines above
                 * bound its var arm properly.  Bind it to the scrutinee's type,
                 * exactly as that path does. */
                pat->is_var = true;
                pat->var_sym = sym;
                has_wildcard = true; /* covers all remaining */
                pat->var_binding = binding_new(e, sym, scrutinee->type,
                                               false, false, pat_form->span);
            }
            /* LT1: Restore outer linear state before this arm's body. */
            if (n_match_lin > 0) {
                linear_state_restore(match_lin_bindings, match_lin_before, n_match_lin);
            }
            if (n_match_move > 0) {
                move_state_restore(match_move_bindings, match_move_before, n_match_move);
            }
            bool _s_wc = e->in_match_arm; e->in_match_arm = true;
            Expr *body = NULL;
            if (pat->is_var && pat->var_binding) {
                /* The binder must be visible to the arm body (and, once guards
                 * reach this path, to its own when-guard -- the same order the
                 * literal path was corrected to). */
                Scope var_scope;
                scope_init(&var_scope, e->scope);
                Scope *saved_var_scope = e->scope;
                e->scope = &var_scope;
                scope_add(&var_scope, pat->var_binding);
                body = elab_form(e, body_form);
                e->scope = saved_var_scope;
                scope_free(&var_scope);
            } else {
                body = elab_form(e, body_form);
            }
            e->in_match_arm = _s_wc;
            if (!body) { goto match_fail; }
            /* LT1: Capture outer linear state after this arm's body. */
            if (n_match_lin > 0) {
                arm_lin_states[ai] = linear_state_capture_current(match_lin_bindings, n_match_lin);
                linear_state_restore(match_lin_bindings, match_lin_before, n_match_lin);
            }
            arm_diverges[ai] = (body->type.kind == TY_NEVER) ||
                               (body->kind == EX_RETURN) ||
                               (body->kind == EX_PANIC)  ||
                               (body->kind == EX_PANIC_WITH);
            if (n_match_move > 0) {
                arm_move_states[ai] = move_state_capture_current(match_move_bindings,
                                                                 n_match_move);
                move_state_restore(match_move_bindings, match_move_before, n_match_move);
            }
            /* Phase G0: Arm body type consistency check for wildcard/variable arms. */
            Type _wc_unified;
            if (result_type.kind != TY_UNKNOWN
                    && body->type.kind != TY_UNKNOWN
                    && !match_arm_type_compatible(e, result_type, body->type, &_wc_unified)) {
                diag_emit_with_code(DIAG_ERROR, body_form->span,
                                    TUR_E0001_TYPE_MISMATCH,
                                    "match: arm types are incompatible -- "
                                    "expected %s (from earlier arm), got %s",
                                    typekind_to_string(result_type.kind),
                                    typekind_to_string(body->type.kind));
                goto match_fail;
            }
            arms[ai].body = body;
            if (result_type.kind == TY_UNKNOWN) result_type = body->type;
            else if (body->type.kind != TY_UNKNOWN) result_type = _wc_unified;
        } else if (pat_form->tag == F_LIST) {
            /* Constructor pattern: (CtorName var1 var2 ...) */
            if (pat_form->as.list.len < 1) {
                diag_emit(DIAG_ERROR, pat_form->span,
                          "match: empty constructor pattern");
                goto match_fail;
            }
            Form *ctor_name_form = pat_form->as.list.items[0];
            if (ctor_name_form->tag != F_SYM) {
                diag_emit(DIAG_ERROR, ctor_name_form->span,
                          "match: constructor pattern must start with a constructor name");
                goto match_fail;
            }
            const Symbol *ctor_sym = ctor_name_form->as.sym;
            /* Look up constructor in this ADT */
            CtorDef *ctor = NULL;
            uint32_t ctor_idx = 0;   /* CONV-S4N: index into adt->ctors[] */
            for (uint32_t ci = 0; ci < adt->n_ctors; ci++) {
                if (strcmp(adt->ctors[ci]->name, ctor_sym->name) == 0) {
                    ctor = adt->ctors[ci];
                    ctor_idx = ci;
                    break;
                }
            }
            if (!ctor) {
                diag_emit(DIAG_ERROR, ctor_name_form->span,
                          "match: '%s' is not a constructor of '%s'",
                          ctor_sym->name, adt->name);
                goto match_fail;
            }

            /* CONV-S0: by-name binding for record-style variants.
             * `(Circle :radius r)` binds field `radius` to `r`.  We rewrite the
             * pattern into the equivalent positional form (reordered to field
             * order) so the positional binding loop below is unchanged.  All
             * fields must be listed exactly once; order is free. */
            if (pat_form->as.list.len >= 2 &&
                pat_form->as.list.items[1]->tag == F_KEYWORD) {
                if (!ctor->is_record) {
                    diag_emit(DIAG_ERROR, pat_form->span,
                              "match: constructor '%s' is positional; by-name "
                              "binding (`:field var`) requires a record-style variant",
                              ctor->name);
                    goto match_fail;
                }
                uint32_t n_pairs = (pat_form->as.list.len - 1);
                if (n_pairs % 2 != 0) {
                    diag_emit(DIAG_ERROR, pat_form->span,
                              "match: by-name pattern for '%s' must be "
                              "`:field var` pairs", ctor->name);
                    goto match_fail;
                }
                n_pairs /= 2;
                if (n_pairs != ctor->n_fields) {
                    diag_emit(DIAG_ERROR, pat_form->span,
                              "match: by-name pattern for '%s' must bind all %u "
                              "fields, got %u", ctor->name, ctor->n_fields, n_pairs);
                    goto match_fail;
                }
                Form **pos_items = (Form **)arena_alloc(e->arena,
                                       (ctor->n_fields + 1) * sizeof(Form *));
                pos_items[0] = ctor_name_form;
                for (uint32_t fi = 0; fi < ctor->n_fields; fi++) pos_items[fi + 1] = NULL;
                for (uint32_t pi = 0; pi < n_pairs; pi++) {
                    Form *kw = pat_form->as.list.items[1 + pi * 2];
                    Form *var = pat_form->as.list.items[2 + pi * 2];
                    if (kw->tag != F_KEYWORD || var->tag != F_SYM) {
                        diag_emit(DIAG_ERROR, kw->span,
                                  "match: by-name pattern for '%s' must be "
                                  "`:field var` pairs", ctor->name);
                        goto match_fail;
                    }
                    int fidx = -1;
                    for (uint32_t fi = 0; fi < ctor->n_fields; fi++) {
                        if (ctor->fields[fi].name &&
                            strcmp(ctor->fields[fi].name, kw->as.sym->name) == 0) {
                            fidx = (int)fi; break;
                        }
                    }
                    if (fidx < 0) {
                        diag_emit(DIAG_ERROR, kw->span,
                                  "match: '%s' is not a field of variant '%s'",
                                  kw->as.sym->name, ctor->name);
                        goto match_fail;
                    }
                    if (pos_items[fidx + 1]) {
                        diag_emit(DIAG_ERROR, kw->span,
                                  "match: field '%s' bound more than once in "
                                  "pattern for '%s'", kw->as.sym->name, ctor->name);
                        goto match_fail;
                    }
                    pos_items[fidx + 1] = var;
                }
                pat_form = form_list(e->arena, pat_form->span, pos_items,
                                     ctor->n_fields + 1);
            }

            uint32_t n_bindings = pat_form->as.list.len - 1;
            if (n_bindings != ctor->n_fields) {
                diag_emit(DIAG_ERROR, pat_form->span,
                          "match: constructor '%s' expects %u fields, got %u",
                          ctor->name, ctor->n_fields, n_bindings);
                goto match_fail;
            }

            pat->ctor = ctor;
            pat->n_bindings = n_bindings;
            pat->bindings = n_bindings > 0
                ? (Binding **)arena_alloc(e->arena, n_bindings * sizeof(Binding *))
                : NULL;

            /* Phase G0: Redundant arm warning -- emit a warning if this constructor
             * was already covered by an earlier non-guarded arm.  We still
             * elaborate the body so any errors inside it are reported. */
            if (covered[ctor->tag]) {
                diag_emit(DIAG_WARNING, pat_form->span,
                          "match: arm for constructor '%s' is unreachable -- "
                          "already covered by an earlier arm",
                          ctor->name);
            }

            /* Phase G4: guarded arm doesn't guarantee coverage */
            covered[ctor->tag] = !arm_has_guard[ai];

            /* Phase G2: For GADT arms, build a per-arm SkolemEnv to resolve
             * type-variable field types to concrete kinds. */
            SkolemEnv arm_senv;
            arm_senv.n = 0;
            if (adt->is_gadt && ctor->result_type_form) {
                gadt_build_skolem_env(e, &arm_senv, adt, ctor);
            }
            SkolemEnv *saved_senv = e->g2_skolem_env;
            const CtorDef *saved_ctor = e->g2_current_ctor;
            if (adt->is_gadt) {
                e->g2_skolem_env = &arm_senv;
                e->g2_current_ctor = ctor;
            }

            /* Introduce a new scope with the field bindings */
            Scope arm_scope;
            scope_init(&arm_scope, e->scope);
            Scope *saved_scope = e->scope;
            e->scope = &arm_scope;

            for (uint32_t bi = 0; bi < n_bindings; bi++) {
                Form *var_form = pat_form->as.list.items[1 + bi];
                if (var_form->tag != F_SYM) {
                    diag_emit(DIAG_ERROR, var_form->span,
                              "match: field binding must be a symbol");
                    e->scope = saved_scope;
                    scope_free(&arm_scope);
                    e->g2_skolem_env = saved_senv;
                    e->g2_current_ctor = saved_ctor;
                    goto match_fail;
                }
                Type ftype;
                if (adt->is_gadt && ctor->field_forms && ctor->field_forms[bi]) {
                    /* Phase G2: resolve field type using the skolem env.
                     * TP2: if full_type is a named TY_TYVAR, do the skolem
                     * lookup directly — avoids re-parsing and is authoritative. */
                    if (ctor->fields[bi].full_type &&
                            ctor->fields[bi].full_type->kind == TY_TYVAR &&
                            ctor->fields[bi].full_type->as.tyvar_.name) {
                        const char *tvname = ctor->fields[bi].full_type->as.tyvar_.name;
                        TypeKind resolved = gadt_skolem_lookup(&arm_senv, tvname);
                        if (resolved != TY_UNKNOWN) {
                            ftype = type_from_kind(resolved);
                        } else {
                            /* KB-025: the constructor's return-type annotation did
                             * not pin this parameter to a concrete kind.  Refine it
                             * via the scrutinee's instantiation -- a scrutinee of
                             * type (Box t) binds the `a` field to `t`, so a properly
                             * polymorphic arm yields the function's own signature
                             * variable rather than the GADT's internal parameter
                             * name.  When the scrutinee carries no instantiation
                             * (e.g. an unannotated parameter), the field keeps its
                             * unresolved tyvar so the skolem-escape check can fire. */
                            ftype = *ctor->fields[bi].full_type;
                            if (adt->n_type_params > 0) {
                                Type *type_args = (Type *)arena_alloc(e->arena,
                                    adt->n_type_params * sizeof(Type));
                                if (elab_adt_type_extract_args(&scrutinee->type,
                                                               adt, type_args)) {
                                    ftype = adt_field_instantiate_type(e, adt,
                                        ctor->fields[bi].full_type, type_args);
                                }
                            }
                        }
                    } else {
                        ftype = gadt_resolve_type_from_form(e, adt,
                                                            ctor->field_forms[bi], &arm_senv);
                    }
                } else if (ctor->fields[bi].full_type &&
                               (ctor->fields[bi].full_type->kind == TY_TYVAR ||
                                ctor->fields[bi].full_type->kind == TY_APP ||
                                ctor->fields[bi].full_type->kind == TY_FN) &&
                               adt->n_type_params > 0) {
                    /* TS4P1/TP6 / nested-carrier-match: a defdata constructor
                     * field that mentions the ADT's type params -- a bare
                     * type-variable (`a` in `(defdata Maybe [a] (Just a))`), or a
                     * nested TY_APP / TY_FN carrying them.  When the scrutinee has
                     * a TY_APP chain (monomorphised instance), substitute the
                     * concrete type args through the whole field type.  This also
                     * covers the case where ctor->field_forms[bi] is NULL because
                     * the field was a bare type-variable symbol (the type-variable
                     * branch in defdata parsing used `continue`, skipping
                     * field_forms). */
                    Type *type_args = (Type *)arena_alloc(e->arena,
                        adt->n_type_params * sizeof(Type));
                    if (elab_adt_type_extract_args(&scrutinee->type, adt, type_args)) {
                        ftype = adt_field_instantiate_type(e, adt,
                                    ctor->fields[bi].full_type, type_args);
                        /* fmap-over-underdetermined-constructor-is-a-defless-shell:
                         * a scrutinee with an OPEN parameter -- `(Result float
                         * B)`, what `(Ok 7.1)` and `(ok 7.1)` are -- leaves the
                         * field that parameter types as a variable nothing in
                         * this function quantifies, and `(println e)` in the
                         * `(Err e)` arm then had no overload.  Such a binder is
                         * the carrier word, as the whole field was when the
                         * scrutinee was the bare ADT.  Only a slot a constructor
                         * left open: a variable a signature or an instance head
                         * quantifies stays abstract. */
                        if (ftype.kind == TY_TYVAR && ftype.as.tyvar_.open_slot)
                            ftype = type_from_kind(ctor->fields[bi].kind);
                    } else {
                        ftype = type_from_kind(ctor->fields[bi].kind);
                    }
                } else if (ctor->field_forms && ctor->field_forms[bi]) {
                    /* F6-1 (cross-plan-followups): defdata ctor field with
                     * a stashed type form -- re-parse the type so the binding
                     * carries the declared ADT/struct, not just the C-level
                     * `int` collapsed by parse_struct_field_type for ADT-typed
                     * fields.  Falls back to type_from_kind below if the
                     * re-parse fails (e.g. unknown type).
                     * TP6 / nested-carrier-match: If the field type mentions any
                     * of the ADT's type params -- a bare TY_TYVAR, or a nested
                     * TY_APP / TY_FN that carries them (e.g. N's `(Pair2 a a)` in
                     * `(defdata Nest [a] (N (Pair2 a a)))`) -- and the scrutinee
                     * carries a concrete TY_APP chain, substitute the concrete
                     * args through the whole field type so the inner bindings
                     * thread the element type instead of leaving bare tyvars. */
                    bool has_param =
                        ctor->fields[bi].full_type &&
                        (ctor->fields[bi].full_type->kind == TY_TYVAR ||
                         ctor->fields[bi].full_type->kind == TY_APP ||
                         ctor->fields[bi].full_type->kind == TY_FN) &&
                        adt->n_type_params > 0;
                    Type *type_args = has_param
                        ? (Type *)arena_alloc(e->arena,
                                              adt->n_type_params * sizeof(Type))
                        : NULL;
                    if (has_param &&
                        elab_adt_type_extract_args(&scrutinee->type, adt,
                                                   type_args)) {
                        ftype = adt_field_instantiate_type(e, adt,
                                    ctor->fields[bi].full_type, type_args);
                    } else {
                        Type *resolved = type_expr_from_form(e,
                            (Form *)ctor->field_forms[bi],
                            NULL, NULL, NULL, 0);
                        if (resolved) {
                            ftype = *resolved;
                        } else {
                            ftype = type_from_kind(ctor->fields[bi].kind);
                        }
                    }
                } else {
                    ftype = type_from_kind(ctor->fields[bi].kind);
                }
                Binding *fb = binding_new(e, var_form->as.sym, ftype, false, false,
                                          var_form->span);
                fb->is_match_binding = true;
                /* byvalue-recursive-shared-copies-leak: a binder of a shared
                 * view (a ^borrow, a global, an element) shares its owning
                 * fields with the scrutinee's owner. */
                fb->shared_view = elab_expr_is_shared_view(scrutinee);
                /* hkt-cata-function-carrier: a TY_FN value stored in a
                 * PARAMETRIC ADT field (one declared as a bare type variable,
                 * e.g. `a` in `(defdata ExprF [a] (AddF a a))`) is uniformly a
                 * fat box -- a thin fn passed into a tyvar field is boxed via
                 * EX_FN_TO_FAT at construction (elab_call.c ~3310), and a
                 * closure value is already a fat box.  Mark the extracted match
                 * binding `boxed` so an application `(f env)` fat-dispatches
                 * through slot 0 (ER2, emit_expr.c) instead of jumping into the
                 * box as thin code -> SIGSEGV.
                 *
                 * A concrete BOXED `(fn ...)` field is the same case: since
                 * resolve_ctor_field started boxing concrete arity<=4 fn
                 * fields, construction stores a fat handle there too -- but
                 * this site still said "a concrete TY_FN field is NOT
                 * auto-boxed" and marked only tyvar fields, so a match-arm
                 * extraction called the fat box THIN and jumped into the env
                 * block as code.  That store/read disagreement is why the
                 * defdata half of closure-in-defdata-field crashed at every
                 * arity while the defstruct field-read half worked at 1..4:
                 * the two force paths consulted different facts. */
                if (fb->type.kind == TY_FN &&
                    ctor->fields[bi].full_type &&
                    (ctor->fields[bi].full_type->kind == TY_TYVAR ||
                     (ctor->fields[bi].full_type->kind == TY_FN &&
                      ctor->fields[bi].full_type->as.fn.boxed))) {
                    fb->is_fat = true;
                }
                /* LT1: Propagate linearity from the field's type to its binding */
                if (ftype.copy_kind == CK_LINEAR) {
                    fb->is_linear = true;
                }
                scope_add(&arm_scope, fb);
                pat->bindings[bi] = fb;
            }

            /* CONV-S4N: per-arm variant narrowing.  When the scrutinee is a
             * bare symbol bound to a variable and this arm has destructured a
             * *multi-variant*, *:copy* ADT to a specific variant, narrow that
             * binding's type to the matched ctor for the arm body (and guard).
             * That lets `(with s [...])` and `(.field s)` inside the arm see the
             * proven variant (single-variant record ADTs already work via the
             * n_ctors==1 gate).  Gated to :copy so we do not disturb the
             * move/linear tracking of a move-only scrutinee.  We narrow to
             * positional variants too (not just record ones) so `elab_with` can
             * tell "narrowed to a fieldless variant" apart from "no narrowing
             * context at all" and diagnose each precisely.  The original type is
             * restored at arm exit, so narrowing never leaks past the arm. */
            Type scrut_narrow_saved = {0};
            bool scrut_narrowed = false;
            if (scrut_narrow_binding && adt->n_ctors > 1 && adt->is_copy) {
                scrut_narrow_saved = scrut_narrow_binding->type;
                Type narrowed = scrut_narrow_binding->type;
                /* Copy the TY_APP spine so setting the narrowing marker on the
                 * head TY_ADT does not mutate any shared Type node. */
                Type *cur = &narrowed;
                while (cur->kind == TY_APP && cur->as.app.fn) {
                    Type *fn_copy = (Type *)arena_alloc(e->arena, sizeof(Type));
                    *fn_copy = *cur->as.app.fn;
                    cur->as.app.fn = fn_copy;
                    cur = fn_copy;
                }
                if (cur->kind == TY_ADT) {
                    cur->as.adt_.is_narrowed = true;
                    cur->as.adt_.narrowed_ctor_idx = ctor_idx;
                }
                scrut_narrow_binding->type = narrowed;
                scrut_narrowed = true;
            }

            /* LT1: Restore outer linear state before this arm's body. */
            if (n_match_lin > 0) {
                linear_state_restore(match_lin_bindings, match_lin_before, n_match_lin);
            }
            if (n_match_move > 0) {
                move_state_restore(match_move_bindings, match_move_before, n_match_move);
            }
            bool _s_ctor = e->in_match_arm; e->in_match_arm = true;
            /* SR2b: sibling-arm bidirectional inference, the match twin of the
             * if-form's have_sibling_ty push (elab_forms.c).  Once an earlier
             * arm established a concrete parametric-app result -- `(k v)` in a
             * Monad bind grounding `(Option b)` -- push it as the expected type
             * so a later arm that is a bare parametric constructor (`(none)`,
             * `(err e)`) selects its monomorph from the join instead of
             * failing it with an ungrounded fresh tyvar.  Narrowed to a TY_APP
             * join so every non-app match elaborates exactly as before, and
             * restored immediately. */
            Type *_arm_saved_expected = e->expected_type;
            bool  _arm_pushed = false;
            if (result_type.kind == TY_APP && !e->expected_type) {
                e->expected_type = &result_type;
                _arm_pushed = true;
            }
            Expr *body = elab_form(e, body_form);
            if (_arm_pushed) e->expected_type = _arm_saved_expected;
            e->in_match_arm = _s_ctor;

            /* Phase G4: Elaborate optional when-guard while arm scope is still live */
            Expr *guard_expr = NULL;
            if (arm_has_guard[ai] && guard_form_raw) {
                guard_expr = elab_form(e, guard_form_raw);
                if (!guard_expr) {
                    e->scope = saved_scope;
                    scope_free(&arm_scope);
                    e->g2_skolem_env = saved_senv;
                    e->g2_current_ctor = saved_ctor;
                    goto match_fail;
                }
                if (guard_expr->type.kind != TY_BOOL) {
                    diag_emit(DIAG_ERROR, guard_form_raw->span,
                              "match: when-guard must have type bool, got %s",
                              typekind_to_string(guard_expr->type.kind));
                    e->scope = saved_scope;
                    scope_free(&arm_scope);
                    e->g2_skolem_env = saved_senv;
                    e->g2_current_ctor = saved_ctor;
                    goto match_fail;
                }
            }

            /* CONV-S4N: restore the scrutinee binding's full type -- narrowing
             * is arm-local and must not leak to later arms or past the match. */
            if (scrut_narrowed) scrut_narrow_binding->type = scrut_narrow_saved;

            e->scope = saved_scope;
            scope_free(&arm_scope);
            e->g2_skolem_env = saved_senv;
            e->g2_current_ctor = saved_ctor;
            /* LT1: Verify all linear field bindings in this arm were consumed */
            bool lt1_arm_fail = false;
            if (body) {
                for (uint32_t bi2 = 0; bi2 < n_bindings; bi2++) {
                    Binding *fb2 = pat->bindings[bi2];
                    if (fb2->is_linear && !fb2->is_linear_consumed && !fb2->is_moved) {
                        diag_emit_with_code(DIAG_ERROR, fb2->span,
                                            TUR_E0100_LINEAR_DROPPED,
                                            "linear field '%s' dropped without being consumed in match arm",
                                            fb2->name->name);
                        lt1_arm_fail = true;
                    }
                }
            }
            /* ST1: Verify all relevant field bindings in this arm were used */
            bool st1_arm_fail = false;
            if (body) {
                for (uint32_t bi2 = 0; bi2 < n_bindings; bi2++) {
                    Binding *fb2 = pat->bindings[bi2];
                    if (fb2->is_relevant && fb2->usage_state == USAGE_UNUSED && !fb2->is_moved) {
                        diag_emit_with_code(DIAG_ERROR, fb2->span,
                                            TUR_E0151_RELEVANT_DROPPED,
                                            "relevant field '%s' dropped without being used in match arm",
                                            fb2->name->name);
                        st1_arm_fail = true;
                    }
                }
            }
            /* Phase G2: GADT constructor context -- when body elaboration fails
             * inside a GADT arm, emit a note naming the constructor whose
             * return-type annotation caused the type refinement and listing the
             * active skolem equalities (e.g. "in this arm: a ~ int").
             * This surfaces the "why" behind type errors that occur because of
             * GADT-induced substitutions. */
            if (!body && adt->is_gadt && arm_senv.n > 0) {
                char skolem_note[256];
                int pos = 0;
                for (uint8_t si = 0; si < arm_senv.n && pos < 230; si++) {
                    if (si > 0) pos += snprintf(skolem_note + pos,
                                                sizeof(skolem_note) - pos, ", ");
                    pos += snprintf(skolem_note + pos, sizeof(skolem_note) - pos,
                                   "%s ~ %s",
                                   arm_senv.bindings[si].name,
                                   typekind_to_string(arm_senv.bindings[si].kind));
                }
                skolem_note[pos] = '\0';
                diag_emit(DIAG_NOTE, body_form->span,
                          "inside arm for constructor '%s' of '%s'; "
                          "active refinements: %s",
                          ctor->name, adt->name, skolem_note);
            }
            if (!body || lt1_arm_fail || st1_arm_fail) { goto match_fail; }

            /* Phase G2/HRT: detect skolem escape -- the arm body result is a GADT
             * type variable that the enclosing function does not quantify.
             *
             * An anonymous TY_TYVAR (name == NULL) is always a skolem escape: it
             * is an unresolved GADT field type with no name.
             *
             * KB-025: a *named* TY_TYVAR is only legitimately polymorphic when the
             * surrounding function's signature actually binds it (e.g. `[b :
             * (Box a)] : a`).  When the name is absent from the signature-tyvar
             * set -- as in `[b] : int`, where matching a `(Box a)` binds `x : a`
             * and the arm yields `x` through a concrete `:int` return -- the
             * skolem `a` escapes the match arm. */
            if (adt->is_gadt && body->type.kind == TY_TYVAR) {
                const char *tvname = body->type.as.tyvar_.name;
                bool quantified = false;
                if (tvname) {
                    for (uint8_t si = 0; si < e->n_sig_tyvars; si++) {
                        if (e->sig_tyvars[si] &&
                            strcmp(e->sig_tyvars[si], tvname) == 0) {
                            quantified = true;
                            break;
                        }
                    }
                }
                if (!quantified) {
                    diag_emit(DIAG_ERROR, body_form->span,
                              "match: skolem type variable escapes match arm "
                              "(constructor '%s' of '%s'): the arm body has an "
                              "unresolved GADT type variable as its result type",
                              ctor->name, adt->name);
                    goto match_fail;
                }
            }

            /* Phase G0/G2: Arm body type consistency check.
             * All arms of a match expression must return the same type.
             * If this arm's body type differs from the type established by the
             * first arm, emit TUR_E0001_TYPE_MISMATCH.  For GADT arms, also
             * emit a note listing the active skolem equalities so the user can
             * see which type refinement is in effect. */
            Type _arm_unified;
            if (result_type.kind != TY_UNKNOWN
                    && body->type.kind != TY_UNKNOWN
                    && !match_arm_type_compatible(e, result_type, body->type, &_arm_unified)) {
                if (adt->is_gadt && arm_senv.n > 0) {
                    char skolem_note[256];
                    int pos = 0;
                    for (uint8_t si = 0; si < arm_senv.n && pos < 230; si++) {
                        if (si > 0) pos += snprintf(skolem_note + pos,
                                                    sizeof(skolem_note) - pos, ", ");
                        pos += snprintf(skolem_note + pos, sizeof(skolem_note) - pos,
                                       "%s ~ %s",
                                       arm_senv.bindings[si].name,
                                       typekind_to_string(arm_senv.bindings[si].kind));
                    }
                    skolem_note[pos] = '\0';
                    diag_emit(DIAG_NOTE, body_form->span,
                              "inside arm for constructor '%s' of '%s'; "
                              "active refinements: %s",
                              ctor->name, adt->name, skolem_note);
                }
                diag_emit_with_code(DIAG_ERROR, body_form->span,
                                    TUR_E0001_TYPE_MISMATCH,
                                    "match: arm types are incompatible -- "
                                    "expected %s (from earlier arm), got %s",
                                    typekind_to_string(result_type.kind),
                                    typekind_to_string(body->type.kind));
                goto match_fail;
            }

            arms[ai].body = body;
            arms[ai].guard = guard_expr;
            if (result_type.kind == TY_UNKNOWN) result_type = body->type;
            else if (body->type.kind != TY_UNKNOWN) result_type = _arm_unified;
            /* LT1: Capture outer linear state after this arm's body. */
            if (n_match_lin > 0) {
                arm_lin_states[ai] = linear_state_capture_current(match_lin_bindings, n_match_lin);
                linear_state_restore(match_lin_bindings, match_lin_before, n_match_lin);
            }
            arm_diverges[ai] = (body->type.kind == TY_NEVER) ||
                               (body->kind == EX_RETURN) ||
                               (body->kind == EX_PANIC)  ||
                               (body->kind == EX_PANIC_WITH);
            if (n_match_move > 0) {
                arm_move_states[ai] = move_state_capture_current(match_move_bindings,
                                                                 n_match_move);
                move_state_restore(match_move_bindings, match_move_before, n_match_move);
            }
        } else {
            diag_emit(DIAG_ERROR, pat_form->span,
                      "match: pattern must be a constructor list or _ wildcard");
            goto match_fail;
        }
    }

    /* Merge the per-arm move state, with `if`'s rule: a binding is moved after
     * the match only if it was moved before it, or every arm that can reach the
     * merge point moved it.  Arms are alternatives, so consuming a value in two
     * different arms is one move at run time, not two. */
    if (n_match_move > 0 && arm_move_states) {
        for (uint32_t mi = 0; mi < n_match_move; mi++) {
            if (match_move_before[mi]) {
                match_move_bindings[mi]->is_moved = true;
                continue;
            }
            bool all = true, any_voter = false;
            for (uint32_t ai = 0; ai < n_arms; ai++) {
                if (!arm_move_states[ai] || arm_diverges[ai]) continue;
                any_voter = true;
                if (!arm_move_states[ai][mi]) { all = false; break; }
            }
            match_move_bindings[mi]->is_moved = any_voter && all;
        }
        for (uint32_t ai = 0; ai < n_arms; ai++) free(arm_move_states[ai]);
        free(arm_move_states); arm_move_states = NULL;
    }
    free(match_move_before);   match_move_before   = NULL;
    free(match_move_bindings); match_move_bindings = NULL;

    /* LT1: Verify consistent outer-scope linear consumption across match arms */
    if (n_match_lin > 0 && arm_lin_states) {
        /* Find the first non-diverging arm as the reference. */
        int ref_ai = -1;
        for (uint32_t ai = 0; ai < n_arms; ai++) {
            if (arm_lin_states[ai] && !arm_diverges[ai]) {
                ref_ai = (int)ai; break;
            }
        }
        bool lin_ok = true;
        if (ref_ai >= 0) {
            for (uint32_t ai = (uint32_t)(ref_ai + 1); ai < n_arms; ai++) {
                if (!arm_lin_states[ai] || arm_diverges[ai]) continue;
                for (uint32_t li = 0; li < n_match_lin; li++) {
                    if (!match_lin_before[li] &&
                        arm_lin_states[ai][li] != arm_lin_states[ref_ai][li]) {
                        diag_emit_with_code(DIAG_ERROR, call->span,
                                            TUR_E0104_LINEAR_BRANCH_MISMATCH,
                                            "linear value '%s' consumed in some match arms but "
                                            "not others -- consume it in all arms or none",
                                            match_lin_bindings[li]->name->name);
                        lin_ok = false;
                    }
                }
            }
        }
        /* Merge: consumed after match only if all non-diverging arms consumed it. */
        for (uint32_t li = 0; li < n_match_lin; li++) {
            match_lin_bindings[li]->is_linear_consumed =
                (ref_ai >= 0) ? arm_lin_states[ref_ai][li] : false;
        }
        for (uint32_t ai = 0; ai < n_arms; ai++) free(arm_lin_states[ai]);
        free(arm_lin_states);   arm_lin_states   = NULL;
        free(match_lin_before); match_lin_before = NULL;
        free(match_lin_bindings); match_lin_bindings = NULL;
        if (!lin_ok) { goto match_fail; }
    } else {
        /* linear_state_snapshot_bindings always allocates its buffers (cap=16),
         * even when the outer scope holds no linear bindings (n_match_lin == 0).
         * The merge block above only frees them when n_match_lin > 0, so free
         * the snapshot here to avoid a LeakSanitizer-visible leak on every match
         * with no outer linear bindings. */
        free(match_lin_before); match_lin_before = NULL;
        free(match_lin_bindings); match_lin_bindings = NULL;
    }

    /* Exhaustiveness check */
    if (!has_wildcard) {
        /* Phase G2: For GADT ADTs, warn about unreachable constructors rather
         * than erroring — a constructor whose concrete type-argument instantiation
         * differs from every covered arm's instantiation is unreachable.
         * An uncovered constructor that IS reachable (or whose reachability is
         * unknown) is still an error. */
        if (adt->is_gadt) {
            /* Collect the first type-arg string from each covered arm's result type. */
            const char *covered_arg0[64]; /* max constructors we check */
            uint32_t n_covered = 0;
            for (uint32_t ci = 0; ci < adt->n_ctors && n_covered < 64; ci++) {
                if (!covered[ci]) continue;
                CtorDef *c = adt->ctors[ci];
                const char *a0 = NULL;
                if (c->result_type_form && c->result_type_form->tag == F_LIST
                    && c->result_type_form->as.list.len >= 2) {
                    Form *arg = c->result_type_form->as.list.items[1];
                    if (arg->tag == F_SYM) a0 = arg->as.sym->name;
                }
                covered_arg0[n_covered++] = a0;
            }
            for (uint32_t ci = 0; ci < adt->n_ctors; ci++) {
                if (covered[ci]) continue;
                CtorDef *c = adt->ctors[ci];
                /* gadt-length-index-not-enforced: a constructor whose declared
                 * result index provably differs from the SCRUTINEE's index --
                 * `VNil : (Vec Zero)` when `v : (Vec (Succ n))` -- cannot be
                 * the scrutinee's value, so it needs no arm.  Silently: that
                 * omission is the whole point of the annotation. */
                if (!gadt_ctor_reachable_from_scrutinee(e, adt, c, &scrutinee->type))
                    continue;
                /* Extract first type arg from this constructor's return type */
                const char *my_a0 = NULL;
                if (c->result_type_form && c->result_type_form->tag == F_LIST
                    && c->result_type_form->as.list.len >= 2) {
                    Form *arg = c->result_type_form->as.list.items[1];
                    if (arg->tag == F_SYM) my_a0 = arg->as.sym->name;
                }
                /* Check if my_a0 conflicts with all covered arms */
                bool all_covered_differ = (n_covered > 0) && (my_a0 != NULL);
                for (uint32_t k = 0; k < n_covered && all_covered_differ; k++) {
                    if (covered_arg0[k] == NULL ||
                        strcmp(covered_arg0[k], my_a0) == 0) {
                        all_covered_differ = false;
                    }
                }
                if (all_covered_differ) {
                    diag_emit(DIAG_WARNING, call->span,
                              "match: constructor '%s' of '%s' is unreachable for "
                              "this GADT instantiation",
                              c->name, adt->name);
                } else if (nonexhaustive_optout) {
                    /* T6: programmer opted out with ^non-exhaustive. */
                } else {
                    diag_emit(DIAG_ERROR, call->span,
                              "match: non-exhaustive patterns — constructor '%s' of '%s' not covered",
                              c->name, adt->name);
                    goto match_fail;
                }
            }
        } else {
            for (uint32_t ci = 0; ci < adt->n_ctors; ci++) {
                if (!covered[ci] && !nonexhaustive_optout) {
                    diag_emit(DIAG_ERROR, call->span,
                              "match: non-exhaustive patterns — constructor '%s' of '%s' not covered",
                              adt->ctors[ci]->name, adt->name);
                    goto match_fail;
                }
            }
        }
    }
    free(covered);
    free(arm_diverges);

    if (result_type.kind == TY_UNKNOWN) result_type = TYPE_NIL;

    /* saffron-lang-plan S5: when the arms UNIFIED to `any`, box the ones that
     * are not.
     *
     * S4 taught match_arm_type_compatible that `any` is the top type, so an
     * `any` arm beside a `Lst` arm joins to `any` instead of being rejected.
     * That settled the TYPE and left the VALUES alone, which the interpreter
     * did not notice -- every TuriValue is one word wide there -- and the
     * compiled path cannot survive: the result temp is a `tur_tagged_t` and the
     * `Lst` arm assigns a raw aggregate into it ("incompatible types when
     * assigning").  Widening a value to the type its own arm already claims is
     * the same coercion the return and argument positions make; this is the
     * third position that needed it.
     *
     * A `!`-typed arm (a `panic`) is skipped: it produces no value to box. */
    if (result_type.kind == TY_ANY) {
        for (uint32_t ai = 0; ai < n_arms; ai++) {
            Expr *b = arms[ai].body;
            if (!b || b->type.kind == TY_ANY || b->type.kind == TY_NEVER) continue;
            Expr *w = elab_coerce_to_any(e, b);
            if (w) arms[ai].body = w;
        }
    }

    Expr *out = expr_new(e->arena, EX_MATCH, result_type, call->span);
    out->as.match_.scrutinee = scrutinee;
    out->as.match_.arms = arms;
    out->as.match_.n_arms = n_arms;
    return out;

    /* Shared error epilogue: every failing exit inside the arm loop and the
     * post-loop checks jumps here so the linear-state snapshot buffers
     * (`match_lin_bindings` / `match_lin_before`), the per-arm state arrays
     * (`arm_lin_states` rows + array, `arm_diverges`), and `covered` are freed
     * on the error path -- previously only `covered` was freed, leaking the
     * snapshot buffers on any match that failed to elaborate.  The merge/else
     * blocks above NULL out the linear buffers after freeing them, so the
     * NULL-safe frees here never double-free. */
match_fail:
    if (arm_lin_states) {
        for (uint32_t ai = 0; ai < n_arms; ai++) free(arm_lin_states[ai]);
        free(arm_lin_states);
    }
    /* The match-arm move-state snapshot (taken above, released on the normal
     * path after the merge) was not released here, so every match that failed
     * to elaborate -- an unknown function in an arm body, a bad pattern --
     * leaked it, and a Debug `tur check` on such a program ended in a
     * LeakSanitizer report on top of the diagnostic.  All three are NULLed
     * after their normal free, so this is safe on either path. */
    if (arm_move_states) {
        for (uint32_t ai = 0; ai < n_arms; ai++) free(arm_move_states[ai]);
        free(arm_move_states);
    }
    free(match_move_before);
    free(match_move_bindings);
    free(arm_diverges);
    free(match_lin_before);
    free(match_lin_bindings);
    free(covered);
    return NULL;
}

/* Phase 11: make-struct - construct a struct value
 * Syntax: (make-struct StructName val1 val2 ...)
 * Returns a struct value (TY_STRUCT) with fields filled in positional order.
 */
Expr *elab_make_struct(Elab *e, const Form *call) {
    if (call->as.list.len < 2) {
        diag_emit(DIAG_ERROR, call->span,
                  "make-struct requires a struct name: (make-struct StructName val1 ...)");
        return NULL;
    }

    Form *name_form = call->as.list.items[1];
    if (name_form->tag != F_SYM) {
        diag_emit(DIAG_ERROR, name_form->span,
                  "make-struct: first argument must be a struct name");
        return NULL;
    }

    /* Look up the struct binding.  CTOR-V0: use scope_lookup_type_def (which
     * filters for TY_STRUCT/TY_ADT, newest-first) rather than a bare
     * scope_lookup, so a same-named auto-bound constructor (TY_FN, in the value
     * namespace) does not shadow the struct type binding we need here. */
    Binding *struct_binding = scope_lookup_type_def(e->scope, name_form->as.sym);
    /* CONV-S1 (defstruct-as-defadt): when a `defstruct` has lowered to a
     * single-variant record `defadt`, its name resolves to a TY_ADT, not a
     * TY_STRUCT.  An existing `(make-struct Name args...)` callsite must keep
     * working: it is exactly the auto-bound constructor call `(Name args...)`
     * (positional or keyword), which the record-ADT path already elaborates
     * (CONV-S0 positional, CONV-S4 keyword).  Rewrite to that constructor form
     * and elaborate it, so the lowering is transparent to make-struct. */
    if (struct_binding && struct_binding->type.kind == TY_ADT) {
        AdtDef *ad = struct_binding->type.as.adt_.def;
        if (ad && ad->n_ctors == 1 && ad->ctors[0] && ad->ctors[0]->is_record) {
            uint32_t nitems = call->as.list.len - 1;  /* drop the `make-struct` head */
            Form **items = (Form **)arena_alloc(e->arena, nitems * sizeof(Form *));
            items[0] = name_form;                     /* constructor name */
            for (uint32_t i = 2; i < call->as.list.len; i++)
                items[i - 1] = call->as.list.items[i];
            Form *ctor_call = form_list(e->arena, call->span, items, nitems);
            /* make-struct of a NON-parametric record ADT did no field typecheck
             * at default (it accepted `0`/NULL ptr<void> for an rc<T> or ptr<T>
             * field, etc.).  The ctor-call rewrite would now subject those args
             * to elab_call's strict positional check.  Set the leniency flag so
             * the ctor call's OWN args relax to that parity; elab_call_fn reads-
             * and-clears it so nested arg-elaboration calls do not inherit it.
             * Only for the non-parametric case -- parametric ctors still
             * infer/check. */
            bool saved_ms_lenient = e->make_struct_lenient_args;
            e->make_struct_lenient_args = (ad->n_type_params == 0);
            /* slice 2: allow the no_auto_ctor ctor call through for this rewrite. */
            bool saved_ms_rewrite = e->make_struct_ctor_rewrite;
            e->make_struct_ctor_rewrite = true;
            Expr *ce = elab_call(e, ctor_call);
            e->make_struct_ctor_rewrite = saved_ms_rewrite;
            e->make_struct_lenient_args = saved_ms_lenient;
            /* structdef-retirement slice 3 (DS1 parity for exists fields): the
             * lenient ADT-ctor rewrite relaxes scalar/pointer arg checks to match
             * default make-struct, but an EXISTENTIAL field MUST still be checked:
             * an exists lowers to the int64 carrier, so a raw `42` (TY_INT) passed
             * where `(exists [a] [(C a)] a)` is expected slips through the kind
             * check and the generated C then reads the int as a `tur_existential_t
             * *` and SEGVs on the next `open`/dispatch.  Re-impose the struct-path
             * per-field full_type check for TY_EXISTS fields (positional form). */
            if (ce && ce->kind == EX_CALL) {
                const CtorDef *rc = ad->ctors[0];
                uint32_t nv = ce->as.call_.n_args;
                bool positional = (nv == 0 ||
                                   call->as.list.items[2]->tag != F_KEYWORD);
                if (positional && nv == rc->n_fields) {
                    for (uint32_t fi = 0; fi < rc->n_fields; fi++) {
                        const Type *ft = rc->fields[fi].full_type;
                        if (ft && ft->kind == TY_EXISTS &&
                            ce->as.call_.args[fi] &&
                            !type_eq(ce->as.call_.args[fi]->type, *ft)) {
                            diag_emit(DIAG_ERROR, call->as.list.items[fi + 2]->span,
                                      "make-struct '%s': field '%s' expects %s, got %s",
                                      ad->name, rc->fields[fi].name,
                                      type_name(*ft),
                                      type_name(ce->as.call_.args[fi]->type));
                            return NULL;
                        }
                    }
                }
            }
            /* lowered-adt-ctor-skips-fn-field-type-param-inference: the struct
             * make-struct path runs struct_field_collect_type_args to ground the
             * struct's type params from the supplied field values -- crucially
             * descending into a FN-typed field so a param that appears only inside
             * a fn field (Lens's `(get (fn [S] A))`) still infers.  The ADT ctor
             * short-circuit above delegates to elab_call, whose
             * call_collect_type_bindings bails on a fat-boxed/ptr<void> fn arg, so
             * `S`/`A` stay unbound and a later `(.get l p)` types as a bare tyvar.
             * Port the inference: re-elaborate the positional value forms, unify
             * each ctor field's declared full_type against the value's actual type
             * (descending into fn fields), and stamp the grounded app result type
             * onto the ctor call so the binding's type carries `(Lens Person cstr)`.
             * Gated to a parametric record ctor with at least one fn-typed field,
             * positional form -- inert for every other make-struct. */
            if (ce && ad->n_type_params > 0 &&
                ad->ctors[0]->n_fields == (nitems - 1) &&
                nitems >= 2 && items[1] && items[1]->tag != F_KEYWORD) {
                bool any_fn = false;
                for (uint32_t i = 0; i < ad->ctors[0]->n_fields; i++) {
                    const Type *ft = ad->ctors[0]->fields[i].full_type;
                    if (ft && ft->kind == TY_FN) { any_fn = true; break; }
                }
                if (any_fn) {
                    Type *targs = (Type *)arena_alloc(
                        e->arena, ad->n_type_params * sizeof(Type));
                    bool *have = (bool *)arena_alloc(
                        e->arena, ad->n_type_params * sizeof(bool));
                    memset(targs, 0, ad->n_type_params * sizeof(Type));
                    memset(have, 0, ad->n_type_params * sizeof(bool));
                    bool ok = true;
                    for (uint32_t i = 0; i < ad->ctors[0]->n_fields && ok; i++) {
                        const Type *ft = ad->ctors[0]->fields[i].full_type;
                        if (!ft) continue;
                        Expr *av = elab_form(e, items[i + 1]);
                        if (!av) { ok = false; break; }
                        adt_field_collect_type_args(ad->type_params,
                                                    ad->n_type_params, ft,
                                                    av->type, targs, have);
                    }
                    bool all = ok;
                    for (uint8_t i = 0; i < ad->n_type_params; i++)
                        if (!have[i]) all = false;
                    if (all) {
                        Type rt = struct_binding->type;  /* TY_ADT base */
                        rt.hkt_kind = kind_for_arity(ad->n_type_params);
                        for (uint8_t i = 0; i < ad->n_type_params; i++)
                            rt = type_app(e->arena, rt, targs[i], call->span);
                        ce->type = rt;
                    }
                }
            }
            return ce;
        }
    }
    /* structdef-retirement DS-D: a lowered struct resolves to a single-variant
     * record ADT and is fully handled by the constructor rewrite above.  The
     * former StructDef make-struct path (field reorder / per-field typecheck /
     * EX_MAKE_STRUCT emission) is dead -- no name resolves to a TY_STRUCT.
     * Reaching here means the name is not a make-struct-able record type. */
    diag_emit(DIAG_ERROR, name_form->span,
              "make-struct: '%s' is not a defined struct type",
              name_form->as.sym->name);
    return NULL;
}

/* CONV-S4: functional update for a single-variant record ADT.  Mirrors the
 * struct lowering in elab_with, but constructs through the auto-bound variant
 * constructor `Ctor`:
 *
 *   (let [G src] (Ctor <f0> <f1> ...))   ; <fi> = override value or (.field G)
 *
 * The ctor is positional in declared field order, so each unchanged field is
 * filled by `(.field G)` (which now resolves on a single-variant record ADT,
 * see the dot-accessor handler in elab_typeclasses.c). */
static Expr *elab_with_record_adt(Elab *e, const Form *call,
                                  const Form *src_form, const Form *ovr_form,
                                  const CtorDef *ctor) {
    Span sp = call->span;
    if (!ctor->adt->is_copy) {
        bool is_struct = conv_surface_is_struct(ctor->adt);
        diag_emit(DIAG_ERROR, call->span,
                  "TUR-E0296: with requires a :copy %s -- '%s' is move-only, "
                  "so copying its unchanged fields out of the source would "
                  "consume it. Declare it `(%s %s :copy ...)` to use with.",
                  is_struct ? "struct" : "variant", ctor->adt->name,
                  is_struct ? "defstruct" : "defdata", ctor->adt->name);
        return NULL;
    }

    /* Map each override field name to its declared field index. */
    Form **ovr_val = (Form **)arena_alloc(e->arena,
        (ctor->n_fields ? ctor->n_fields : 1) * sizeof(Form *));
    for (uint32_t i = 0; i < ctor->n_fields; i++) ovr_val[i] = NULL;
    uint32_t n_pairs = ovr_form->as.list.len / 2u;
    for (uint32_t p = 0; p < n_pairs; p++) {
        Form *fname = ovr_form->as.list.items[p * 2u];
        Form *fval  = ovr_form->as.list.items[p * 2u + 1u];
        if (fname->tag != F_SYM) {
            diag_emit(DIAG_ERROR, fname->span,
                      "with: override field must be a bare field-name symbol");
            return NULL;
        }
        const char *kn = fname->as.sym->name;
        uint32_t klen = fname->as.sym->len;
        uint32_t fi = UINT32_MAX;
        for (uint32_t i = 0; i < ctor->n_fields; i++) {
            const char *dn = ctor->fields[i].name;
            if (dn && strlen(dn) == klen && memcmp(dn, kn, klen) == 0) { fi = i; break; }
        }
        if (fi == UINT32_MAX) {
            char surf[160];
            conv_surface_phrase(ctor->adt, ctor, surf, sizeof(surf));
            diag_emit(DIAG_ERROR, fname->span,
                      "TUR-E0297: with: unknown field '%s' on %s", kn, surf);
            return NULL;
        }
        if (ovr_val[fi]) {
            diag_emit(DIAG_ERROR, fname->span,
                      "TUR-E0298: with duplicate override field '%s'", kn);
            return NULL;
        }
        ovr_val[fi] = fval;
    }

    /* Build (let [G src] (Ctor <f0> ...)). */
    char g_name[32];
    snprintf(g_name, sizeof(g_name), "__with_%u", elab_fresh_id(e));
    const Symbol *g_sym = symtab_intern(e->st, strslice(g_name, (uint32_t)strlen(g_name)));
    Form *g_form = form_sym(e->arena, sp, g_sym);

    const Symbol *ctor_sym = symtab_intern(e->st,
        strslice(ctor->name, (uint32_t)strlen(ctor->name)));

    uint32_t cc_n = 1u + ctor->n_fields;
    Form **cc = (Form **)arena_alloc(e->arena, (cc_n ? cc_n : 1) * sizeof(Form *));
    cc[0] = form_sym(e->arena, sp, ctor_sym);
    for (uint32_t i = 0; i < ctor->n_fields; i++) {
        if (ovr_val[i]) {
            cc[1u + i] = ovr_val[i];
        } else {
            char acc[160];
            int al = snprintf(acc, sizeof(acc), ".%s", ctor->fields[i].name);
            const Symbol *acc_sym = symtab_intern(e->st,
                strslice(acc, (al > 0 ? (uint32_t)al : 0)));
            Form *ai[2];
            ai[0] = form_sym(e->arena, sp, acc_sym);
            ai[1] = g_form;
            cc[1u + i] = form_list(e->arena, sp, ai, 2);
        }
    }
    Form *ctor_call = form_list(e->arena, sp, cc, cc_n);

    Form *bind_items[2];
    bind_items[0] = g_form;
    bind_items[1] = (Form *)src_form;
    Form *bind_vec = form_vec(e->arena, sp, bind_items, 2);

    Form *let_items[3];
    let_items[0] = form_sym(e->arena, sp, e->sym_let);
    let_items[1] = bind_vec;
    let_items[2] = ctor_call;
    Form *let_form = form_list(e->arena, sp, let_items, 3);
    return elab_form(e, let_form);
}

/* WITH-V0: functional struct update.
 *
 * Syntax: (with src [field0 val0 field1 val1 ...])
 *
 * Returns a new struct of the same type as `src` with the listed fields
 * overridden and every other field copied from `src`.  Only valid on :copy
 * structs (copying the unchanged fields out of a move-only source would
 * consume it).  Lowers to:
 *
 *   (let [G src]
 *     (make-struct Name <f0> <f1> ...))   ; <fi> = override value or (.fi G)
 *
 * Reusing make-struct gives field-order independence, per-field type checking
 * (the same diagnostic the positional constructor produces), and parametric
 * type inference for free.
 */
Expr *elab_with(Elab *e, const Form *call) {
    if (call->as.list.len != 3) {
        diag_emit(DIAG_ERROR, call->span,
                  "with requires a source value and an override vector: "
                  "(with src [field value ...])");
        return NULL;
    }
    Form *src_form = call->as.list.items[1];
    Form *ovr_form = call->as.list.items[2];
    if (ovr_form->tag != F_VEC) {
        diag_emit(DIAG_ERROR, ovr_form->span,
                  "with: override list must be a vector [field value ...]");
        return NULL;
    }
    if ((ovr_form->as.list.len % 2u) != 0u) {
        diag_emit(DIAG_ERROR, ovr_form->span,
                  "with: override list must have an even number of forms "
                  "([field value ...] pairs)");
        return NULL;
    }

    /* Elaborate the source once to learn its struct type.  This Expr is used
     * only for type detection and is discarded; the let binding below
     * re-elaborates src_form so it is evaluated exactly once at runtime. */
    Expr *src = elab_form(e, src_form);
    if (!src) return NULL;
    Type st = src->type;
    while (st.kind == TY_APP && st.as.app.fn) st = *st.as.app.fn; /* parametric head */
    /* CONV-S4 (struct/ADT convergence): a single-variant record ADT is a
     * product, so `with` updates it exactly like a struct.  Lower to
     * `(let [G src] (Ctor <f0> <f1> ...))` where each `<fi>` is the override
     * value or `(.field G)` for unchanged fields -- the same shape the struct
     * path below uses, but constructing through the auto-bound variant
     * constructor instead of make-struct.  Field-order independence,
     * per-field type checking, and inference all come from reusing the ctor. */
    if (st.kind == TY_ADT && st.as.adt_.def &&
        ((adt_is_flat_product(st.as.adt_.def) &&
          st.as.adt_.def->n_ctors == 1 && st.as.adt_.def->ctors[0]->is_record) ||
         adt_is_narrowed_to_record_variant(st))) {
        /* CONV-S4N: for a narrowed multi-variant ADT the proven variant is the
         * recorded ctor; for the single-variant product it is the sole ctor.
         * `elab_with_record_adt` reconstructs through that ctor -- the lowered
         * `(Ctor <f0> ...)` call produces a tagged value of the full ADT type,
         * exactly as an arm body `(Circle r)` does today. */
        const CtorDef *ctor = adt_is_narrowed_to_record_variant(st)
            ? st.as.adt_.def->ctors[st.as.adt_.narrowed_ctor_idx]
            : st.as.adt_.def->ctors[0];
        return elab_with_record_adt(e, call, src_form, ovr_form, ctor);
    }
    /* CONV-S4N: the arm narrowed the scrutinee, but to a POSITIONAL variant --
     * it has no field names for `with` to reference.  Point the user at the
     * fact that only record-style variants (named fields) are updatable. */
    if (st.kind == TY_ADT && st.as.adt_.def && st.as.adt_.is_narrowed &&
        st.as.adt_.narrowed_ctor_idx < st.as.adt_.def->n_ctors) {
        const CtorDef *ctor = st.as.adt_.def->ctors[st.as.adt_.narrowed_ctor_idx];
        diag_emit(DIAG_ERROR, src_form->span,
                  "TUR-E0302: with cannot update variant '%s' of '%s' -- it is "
                  "positional (no field names). 'with' can only reconstruct a "
                  "record-style variant whose fields are named; rewrite '%s' as "
                  "(%s [f0 : T0 ...]) to use named-field updates.",
                  ctor->name, st.as.adt_.def->name, ctor->name, ctor->name);
        return NULL;
    }
    /* CONV-S4N: `with` on a multi-variant ADT outside a narrowing context.
     * The value could be any variant, so a single reconstructing ctor call is
     * ambiguous.  Point the user at the fix -- wrap the `with` in a `match` arm
     * that proves which variant is held. */
    if (st.kind == TY_ADT && st.as.adt_.def && st.as.adt_.def->n_ctors > 1) {
        const AdtDef *def = st.as.adt_.def;
        char variants[256];
        int vp = 0;
        for (uint32_t ci = 0; ci < def->n_ctors && vp < 230; ci++) {
            if (ci > 0) vp += snprintf(variants + vp, sizeof(variants) - vp, ", ");
            vp += snprintf(variants + vp, sizeof(variants) - vp, "%s",
                           def->ctors[ci]->name ? def->ctors[ci]->name : "?");
        }
        variants[vp] = '\0';
        const char *first_rec = NULL;
        for (uint32_t ci = 0; ci < def->n_ctors; ci++) {
            if (def->ctors[ci]->is_record) { first_rec = def->ctors[ci]->name; break; }
        }
        diag_emit(DIAG_ERROR, src_form->span,
                  "TUR-E0302: with on a multi-variant ADT requires a narrowing "
                  "context. '%s' has %u variants (%s); 'with' can only "
                  "reconstruct one. Wrap the call in a 'match' arm: "
                  "(match s (%s r) (with s [field ...]))",
                  def->name, def->n_ctors, variants,
                  first_rec ? first_rec : def->ctors[0]->name);
        return NULL;
    }
    /* structdef-retirement DS-D: a lowered struct is a single-variant record
     * ADT handled by elab_with_record_adt above; the old StructDef `with`
     * lowering is dead (no source resolves to a TY_STRUCT). */
    diag_emit(DIAG_ERROR, src_form->span,
              "with: source must be a struct value, got %s",
              type_name(src->type));
    return NULL;
}

/* Elaborate (& expr) - create an immutable borrow
 * 
 * Syntax: (& expr)
 * Returns: &T where T is the type of expr
 * The borrow is valid for the duration of the enclosing scope.
 */
Expr *elab_borrow_immut(Elab *e, const Form *call) {
    if (call->as.list.len != 2) {
        diag_emit(DIAG_ERROR, call->span,
                  "& takes exactly one argument: (& expr)");
        return NULL;
    }
    
    /* Elaborate the expression being borrowed */
    Expr *inner = elab_form(e, call->as.list.items[1]);
    if (!inner) return NULL;
    
    /* Phase 12: Borrow tracking - check if inner is a binding and track the borrow */
    if (inner->kind == EX_VAR) {
        Binding *target = inner->as.var.binding;
        /* Check for use-after-move */
        if (target->is_moved) {
            diag_emit_with_code(DIAG_ERROR, call->span, TUR_E0005_USE_AFTER_MOVE,
                                "cannot borrow `%s` because it was moved",
                                target->name->name);
            return NULL;
        }
        /* Check for borrow conflicts and add to active borrows */
        if (!scope_add_borrow(e->scope, target, BK_IMMUT, call->span)) {
            return NULL; /* Error already emitted */
        }
        /* LT1: a borrow is a non-consuming read.  Elaborating the inner
         * EX_VAR above marked the linear binding consumed (the generic
         * EX_VAR path consumes on use); undo it so a later move/drop/consume
         * of the owner is still valid.  With substructural/linear checking
         * now always-on, omitting this turns `(& r)` ... `(drop! r)` into a
         * spurious use-after-consume (TUR-E0101). */
        if (target->is_linear)
            target->is_linear_consumed = false;
    }

    /* Phase U: Borrowing from ptr<void> requires an unsafe context. */
    if (inner->type.kind == TY_PTR_VOID && e->unsafe_depth == 0) {
        diag_emit(DIAG_ERROR, call->span,
                  "cannot borrow from ptr<void> outside (unsafe ...)");
        return NULL;
    }

    /* Create the borrow type: &T where T is the referenced value's type.
     * Special cases:
     *   (& r) where r: ref<T>     → &T (borrow from owning ref)
     *   (& r) where r: &T or &mut T → &T (reborrow — same target type, not &&T)
     *   (& x) where x: T           → &T (plain borrow)
     */
    Type borrow_type;
    if (inner->type.kind == TY_REF) {
        borrow_type = type_ref_immut(inner->type.as.ref.inner);
    } else if (inner->type.kind == TY_REF_IMMUT || inner->type.kind == TY_REF_MUT) {
        borrow_type = type_ref_immut(inner->type.as.ref_borrow.target);
        borrow_type.as.ref_borrow.target_full = inner->type.as.ref_borrow.target_full;
    } else {
        borrow_type = type_ref_immut(inner->type.kind);
        /* borrowed-aggregate-key-skips-the-key-check: keep an aggregate
         * target's full type, which its kind does not name. */
        if (borrow_target_needs_full(inner->type.kind)) {
            Type *tf = (Type *)arena_alloc(e->arena, sizeof(Type));
            *tf = inner->type;
            borrow_type.as.ref_borrow.target_full = tf;
        }
    }
    
    /* Create the borrow expression */
    Expr *out = expr_new(e->arena, EX_BORROW_IMMUT, borrow_type, call->span);
    out->as.borrow_immut_.expr = inner;
    return out;
}

/* Elaborate (&mut expr) - create a mutable borrow
 * 
 * Syntax: (&mut expr)
 * Returns: &mut T where T is the type of expr
 * The borrow is valid for the duration of the enclosing scope.
 * Only one &mut T can exist for a given T at a time.
 */
Expr *elab_borrow_mut(Elab *e, const Form *call) {
    if (call->as.list.len != 2) {
        diag_emit(DIAG_ERROR, call->span,
                  "&mut takes exactly one argument: (&mut expr)");
        return NULL;
    }
    
    /* Elaborate the expression being borrowed */
    Expr *inner = elab_form(e, call->as.list.items[1]);
    if (!inner) return NULL;
    
    /* Phase 12: Borrow tracking - check if inner is a binding and track the borrow */
    if (inner->kind == EX_VAR) {
        Binding *target = inner->as.var.binding;
        /* Check for use-after-move */
        if (target->is_moved) {
            diag_emit_with_code(DIAG_ERROR, call->span, TUR_E0005_USE_AFTER_MOVE,
                                "cannot mutably borrow `%s` because it was moved",
                                target->name->name);
            return NULL;
        }
        /* Check for borrow conflicts and add to active borrows */
        if (!scope_add_borrow(e->scope, target, BK_MUT, call->span)) {
            return NULL; /* Error already emitted */
        }
        /* LT1: a mutable borrow is also a non-consuming read -- undo the
         * linear-consume the inner EX_VAR applied (see elab_borrow_immut). */
        if (target->is_linear)
            target->is_linear_consumed = false;
    }
    
    /* Phase U: Borrowing from ptr<void> requires an unsafe context. */
    if (inner->type.kind == TY_PTR_VOID && e->unsafe_depth == 0) {
        diag_emit(DIAG_ERROR, call->span,
                  "cannot borrow from ptr<void> outside (unsafe ...)");
        return NULL;
    }

    /* Create the borrow type: &mut T where T is the referenced value's type.
     * Special cases:
     *   (&mut r) where r: ref<T>  → &mut T (mutable borrow from owning ref)
     *   (&mut r) where r: &mut T  → &mut T (mutable reborrow)
     *   (&mut r) where r: &T      → error (cannot take mutable borrow of immutable borrow)
     *   (&mut x) where x: T       → &mut T (plain mutable borrow)
     */
    Type borrow_type;
    if (inner->type.kind == TY_REF) {
        borrow_type = type_ref_mut(inner->type.as.ref.inner);
    } else if (inner->type.kind == TY_REF_IMMUT) {
        diag_emit(DIAG_ERROR, call->span,
                  "cannot borrow as mutable: source is already an immutable borrow `&T`");
        return NULL;
    } else if (inner->type.kind == TY_REF_MUT) {
        borrow_type = type_ref_mut(inner->type.as.ref_borrow.target);
        borrow_type.as.ref_borrow.target_full = inner->type.as.ref_borrow.target_full;
    } else {
        borrow_type = type_ref_mut(inner->type.kind);
        /* borrowed-aggregate-key-skips-the-key-check: as `&` does. */
        if (borrow_target_needs_full(inner->type.kind)) {
            Type *tf = (Type *)arena_alloc(e->arena, sizeof(Type));
            *tf = inner->type;
            borrow_type.as.ref_borrow.target_full = tf;
        }
    }
    
    /* Create the borrow expression */
    Expr *out = expr_new(e->arena, EX_BORROW_MUT, borrow_type, call->span);
    out->as.borrow_mut_.expr = inner;
    return out;
}
