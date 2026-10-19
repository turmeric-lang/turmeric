/* elab_dynvars.c -- elaboration for Clojure-style dynamic vars (-Xdynamic-vars).
 * Phase DV0: parse (defdynamic ...) and register DynVarEntry metadata.
 * Phase DV1: elab_binding() for (binding [...] body); dynvar-read for *name*;
 *            dynvar-set extension to set! (in elab_forms.c). */
#include "elab_internal.h"

/* ---- helpers ---- */

/* Return true if `name` matches the *earmuffs* convention: starts and ends
 * with '*'.  Used to emit TUR-W0600 when the convention is absent. */
static bool has_earmuffs(const char *name) {
    size_t len = strlen(name);
    return len >= 3 && name[0] == '*' && name[len - 1] == '*';
}

/* ---- public API ---- */

/* DV0: Look up a registered dynamic var by name.
 * Returns NULL if no matching entry exists. */
DynVarEntry *dynvar_lookup(const Elab *e, const Symbol *name) {
    for (uint32_t i = 0; i < e->n_dynvars; i++) {
        if (e->dynvar_entries[i]->name == name) return e->dynvar_entries[i];
    }
    return NULL;
}

/* DV0: Elaborate (defdynamic [^private] *name* :type root-expr).
 *
 * Syntax:
 *   (defdynamic *name* :type root-expr)
 *   (defdynamic ^private *name* :type root-expr)
 *
 * Checks performed:
 *   - Feature flag must be active              (hard error, no code)
 *   - Must be at module top-level              (TUR-E0604)
 *   - Value type must not be substructural     (TUR-E0603)
 *   - Name missing *earmuffs* convention       (TUR-W0600, non-fatal)
 *   - Root-expr type must match declared type  (TUR-E0602)
 *
 * On success registers a DynVarEntry and returns EX_DEFDYNAMIC (nil at runtime).
 */
Expr *elab_defdynamic(Elab *e, const Form *call) {
    /* Minimum shape: (defdynamic *name* :type root-expr) — 4 items */
    if (call->as.list.len < 4) {
        diag_emit(DIAG_ERROR, call->span,
                  "defdynamic requires (defdynamic *name* :type root-expr)");
        return NULL;
    }

    /* Parse optional ^private visibility annotation */
    bool is_private = false;
    uint32_t name_idx = 1;
    if (call->as.list.len >= 5) {
        Form *maybe_priv = call->as.list.items[1];
        if (maybe_priv->tag == F_SYM
            && maybe_priv->as.sym == e->sym_caret_private) {
            is_private = true;
            name_idx = 2;
        }
    }

    /* Parse var name */
    Form *name_f = call->as.list.items[name_idx];
    if (name_f->tag != F_SYM) {
        diag_emit(DIAG_ERROR, name_f->span,
                  "defdynamic: var name must be a symbol");
        return NULL;
    }
    const Symbol *name = name_f->as.sym;

    /* TUR-E0604: must be at module top-level */
    if (e->fn_body_depth > 0) {
        diag_emit_with_code(DIAG_ERROR, name_f->span, TUR_E0604_DYNVAR_NOT_TOPLEVEL,
                            "defdynamic: '%s' must be declared at module top-level, "
                            "not inside a function", name->name);
        return NULL;
    }

    /* Parse the type annotation (keyword or type-ann form) */
    Form *type_f = call->as.list.items[name_idx + 1];
    Type *vtype = NULL;
    if (type_f->tag == F_KEYWORD || type_f->tag == F_SYM) {
        /* :int  or  int  — simple keyword / symbol type */
        vtype = type_expr_from_form(e, type_f, NULL, NULL, NULL, 0);
    } else if (type_f->tag == F_TYPE_ANN && type_f->as.list.len > 0) {
        vtype = type_expr_from_form(e, type_f->as.list.items[0], NULL, NULL, NULL, 0);
    } else {
        diag_emit(DIAG_ERROR, type_f->span,
                  "defdynamic: expected type annotation, e.g. :int");
        return NULL;
    }
    if (!vtype) {
        diag_emit(DIAG_ERROR, type_f->span,
                  "defdynamic: unknown or invalid type");
        return NULL;
    }

    /* TUR-E0603: reject substructural (linear / move-only) value types */
    if (vtype->copy_kind == CK_LINEAR || vtype->copy_kind == CK_UNIQUE) {
        diag_emit_with_code(DIAG_ERROR, type_f->span, TUR_E0603_DYNVAR_SUBSTRUCTURAL_TYPE,
                            "defdynamic: '%s' -- dynamic vars may not hold substructural "
                            "(linear or move-only) types", name->name);
        return NULL;
    }

    /* TUR-W0600: *earmuffs* convention warning */
    if (!has_earmuffs(name->name)) {
        diag_emit_with_code(DIAG_WARNING, name_f->span, TUR_W0600_DYNVAR_NO_EARMUFFS,
                            "defdynamic: '%s' -- dynamic var names should use the "
                            "*earmuffs* convention (e.g. *%s*)", name->name, name->name);
    }

    /* Elaborate the root (initial) expression.  The declared type rides the
     * expected-type channel, as a let annotation's does, so a generic call
     * whose type variable lives only on its result -- `(None)`, `(none)` --
     * takes the dynvar's type instead of the bare `Option`, which TUR-E0602
     * below refused (defdynamic-generic-adt-root-emitted-before-its-type). */
    Form *root_f = call->as.list.items[name_idx + 2];
    Type *prev_expected = e->expected_type;
    e->expected_type = vtype;
    Expr *root_expr = elab_form(e, root_f);
    e->expected_type = prev_expected;
    if (!root_expr) return NULL;

    /* TUR-E0602: root-expr type must match declared value type */
    if (root_expr->type.kind != vtype->kind) {
        diag_emit_with_code(DIAG_ERROR, root_f->span, TUR_E0602_DYNVAR_TYPE_MISMATCH,
                            "defdynamic: '%s' declared as '%s' but root expression has type '%s'",
                            name->name, type_name(*vtype), type_name(root_expr->type));
        return NULL;
    }

    /* Allocate and register a DynVarEntry */
    DynVarEntry *entry = (DynVarEntry *)arena_alloc(e->arena, sizeof(DynVarEntry));
    entry->name       = name;
    entry->value_type = *vtype;
    entry->index      = (int)e->n_dynvars;
    entry->is_private = is_private;

    if (e->n_dynvars >= e->cap_dynvars) {
        e->cap_dynvars = e->cap_dynvars ? e->cap_dynvars * 2 : 8;
        e->dynvar_entries = (DynVarEntry **)realloc(
            e->dynvar_entries, e->cap_dynvars * sizeof(DynVarEntry *));
    }
    e->dynvar_entries[e->n_dynvars++] = entry;

    /* Add a global binding for the var so later elaboration can resolve it */
    Type binding_type = type_dynvar(e->arena, *vtype);
    Binding *b = binding_new(e, name, binding_type, /*is_mut=*/false,
                             /*is_global=*/true, name_f->span);
    b->is_dynvar    = true;
    b->dynvar_entry = entry;
    scope_add(&e->global, b);

    /* Build and return the compile-time-only AST node */
    Expr *out = expr_new(e->arena, EX_DEFDYNAMIC, TYPE_NIL, call->span);
    out->as.defdynamic_.entry     = entry;
    out->as.defdynamic_.root_expr = root_expr;
    return out;
}

/* ---- DV1 active-frame tracking helpers ---- */

/* Push a dynvar name onto the compile-time active-binding set. */
static void dynvar_push_active(Elab *e, const Symbol *name) {
    if (e->n_active_dynvar_bindings >= e->cap_active_dynvar_bindings) {
        e->cap_active_dynvar_bindings =
            e->cap_active_dynvar_bindings ? e->cap_active_dynvar_bindings * 2 : 8;
        e->active_dynvar_bindings = (const Symbol **)realloc(
            e->active_dynvar_bindings,
            e->cap_active_dynvar_bindings * sizeof(const Symbol *));
    }
    e->active_dynvar_bindings[e->n_active_dynvar_bindings++] = name;
}

/* Pop the most recent active-binding entry for `name` (linear stack discipline). */
static void dynvar_pop_active(Elab *e, const Symbol *name) {
    /* Scan from the top; pop the last occurrence. */
    for (int32_t i = (int32_t)e->n_active_dynvar_bindings - 1; i >= 0; i--) {
        if (e->active_dynvar_bindings[i] == name) {
            /* Shift everything above it down by one */
            for (uint32_t j = (uint32_t)i; j + 1 < e->n_active_dynvar_bindings; j++) {
                e->active_dynvar_bindings[j] = e->active_dynvar_bindings[j + 1];
            }
            e->n_active_dynvar_bindings--;
            return;
        }
    }
    /* Should never happen if push/pop are balanced. */
}

/* DV1: Elaborate (binding [*v1* expr1 *v2* expr2 ...] body).
 *
 * The binding vector must have an even number of elements.  Each `*vi*`
 * must resolve to a registered DynVarEntry (TUR-E0600 otherwise).  Each
 * `expri` must have a type matching the var's declared type (TUR-E0602).
 *
 * The body is elaborated with all named vars pushed onto the compile-time
 * active-binding set so that inner (set! *vi* ...) forms can verify a
 * frame is present (TUR-E0601).
 *
 * Result type is the result type of body.
 */
Expr *elab_binding(Elab *e, const Form *call) {
    /* Minimum: (binding [pairs...] body) -- 3 items */
    if (call->as.list.len < 3) {
        diag_emit(DIAG_ERROR, call->span,
                  "binding requires (binding [*var* expr ...] body)");
        return NULL;
    }

    Form *vec_f = call->as.list.items[1];
    if (vec_f->tag != F_VEC) {
        diag_emit(DIAG_ERROR, vec_f->span,
                  "binding: first argument must be a vector [*var* expr ...]");
        return NULL;
    }

    uint32_t vec_len = vec_f->as.list.len;
    if (vec_len % 2 != 0) {
        diag_emit(DIAG_ERROR, vec_f->span,
                  "binding: vector must have an even number of elements "
                  "(got %u)", vec_len);
        return NULL;
    }

    uint32_t n_pairs = vec_len / 2;
    DynBinding *pairs = (DynBinding *)arena_alloc(e->arena, n_pairs * sizeof(DynBinding));

    /* Elaborate each (var, override-expr) pair */
    for (uint32_t pi = 0; pi < n_pairs; pi++) {
        Form *var_f  = vec_f->as.list.items[pi * 2];
        Form *expr_f = vec_f->as.list.items[pi * 2 + 1];

        /* The var must be a symbol that resolves to a DynVarEntry */
        if (var_f->tag != F_SYM) {
            diag_emit(DIAG_ERROR, var_f->span,
                      "binding: var names must be symbols, got non-symbol at position %u",
                      pi * 2);
            return NULL;
        }
        Binding *b = scope_lookup(e->scope, var_f->as.sym);
        if (!b) b = scope_lookup(&e->global, var_f->as.sym);
        if (!b || !b->is_dynvar || !b->dynvar_entry) {
            diag_emit_with_code(DIAG_ERROR, var_f->span, TUR_E0600_DYNVAR_SET_NOT_DYNAMIC,
                                "binding: '%s' is not a dynamic var",
                                var_f->as.sym->name);
            return NULL;
        }
        DynVarEntry *entry = b->dynvar_entry;

        /* The var's declared type is the override's expected type, as for
         * the root above: `(binding [*n* (none)] ...)` on an `(Option int)`
         * var types `none` at `(Option int)`. */
        Type *prev_expected = e->expected_type;
        e->expected_type = &entry->value_type;
        Expr *override_expr = elab_form(e, expr_f);
        e->expected_type = prev_expected;
        if (!override_expr) return NULL;

        /* TUR-E0602: override type must match declared value type */
        if (override_expr->type.kind != entry->value_type.kind) {
            diag_emit_with_code(DIAG_ERROR, expr_f->span, TUR_E0602_DYNVAR_TYPE_MISMATCH,
                                "binding: '%s' declared as '%s' but override has type '%s'",
                                entry->name->name, type_name(entry->value_type),
                                type_name(override_expr->type));
            return NULL;
        }

        pairs[pi].entry         = entry;
        pairs[pi].override_expr = override_expr;
    }

    /* Push all vars onto the compile-time active-binding set before elaborating body */
    for (uint32_t pi = 0; pi < n_pairs; pi++) {
        dynvar_push_active(e, pairs[pi].entry->name);
    }

    /* Elaborate the body: (binding [...] body) -- the body is the third item,
     * but if there are additional trailing forms we wrap them in an implicit do. */
    uint32_t n_body_forms = call->as.list.len - 2;
    Expr *body = NULL;
    if (n_body_forms == 1) {
        body = elab_form(e, call->as.list.items[2]);
    } else {
        /* Multiple body forms -- treat as implicit do */
        Expr **body_exprs = (Expr **)arena_alloc(e->arena, n_body_forms * sizeof(Expr *));
        for (uint32_t bi = 0; bi < n_body_forms; bi++) {
            body_exprs[bi] = elab_form(e, call->as.list.items[2 + bi]);
            if (!body_exprs[bi]) {
                /* Pop active bindings before returning */
                for (uint32_t pi = 0; pi < n_pairs; pi++) {
                    dynvar_pop_active(e, pairs[pi].entry->name);
                }
                return NULL;
            }
        }
        body = expr_new(e->arena, EX_DO, body_exprs[n_body_forms - 1]->type, call->span);
        body->as.do_.items = body_exprs;
        body->as.do_.n     = n_body_forms;
    }

    /* Pop active bindings */
    for (uint32_t pi = 0; pi < n_pairs; pi++) {
        dynvar_pop_active(e, pairs[pi].entry->name);
    }

    if (!body) return NULL;

    Expr *out = expr_new(e->arena, EX_DYNVAR_BINDING, body->type, call->span);
    out->as.dynvar_binding_.pairs   = pairs;
    out->as.dynvar_binding_.n_pairs = n_pairs;
    out->as.dynvar_binding_.body    = body;
    return out;
}
