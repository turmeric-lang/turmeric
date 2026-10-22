/* elab_unsafe.c -- unsafe pointer ops, casts, raw memory, and C FFI primitives. */
#include "elab_internal.h"

#include "globals.h"            /* g_needs_dlfcn */
#include "turi/jit_ffi.h"       /* signature vocabulary (class_for_kind) */

/* Phase U3: Unsafe primitives implementations */

Expr *elab_ptr_deref(Elab *e, const Form *call) {
    /* (ptr-deref ptr) - dereference a pointer, return the value at that address */
    if (call->as.list.len != 2) {
        diag_emit(DIAG_ERROR, call->span,
                  "ptr-deref requires exactly 1 argument: (ptr-deref ptr)");
        return NULL;
    }
    if (e->unsafe_depth == 0) {
        diag_emit(DIAG_ERROR, call->span,
                  "ptr-deref requires an enclosing (unsafe ...) block");
        return NULL;
    }
    Expr *ptr = elab_form(e, call->as.list.items[1]);
    if (!ptr) return NULL;
    if (ptr->type.kind != TY_PTR_VOID) {
        diag_emit(DIAG_ERROR, call->span,
                  "ptr-deref requires ptr<void>, got %s", type_name(ptr->type));
        return NULL;
    }
    const BuiltinSpec *spec = builtin_lookup(e->sym_ptr_deref, ptr->type, 1);
    if (!spec) {
        diag_emit(DIAG_ERROR, call->span, "ptr-deref: builtin lookup failed");
        return NULL;
    }
    Expr *out = expr_new(e->arena, EX_BUILTIN, spec->result_type, call->span);
    out->as.builtin.spec = spec;
    Expr **args = arena_alloc(e->arena, sizeof(Expr *));
    args[0] = ptr;
    out->as.builtin.args = args;
    out->as.builtin.n = 1;
    return out;
}

Expr *elab_ptr_write(Elab *e, const Form *call) {
    /* (ptr-write ptr value) - write a value to a pointer address */
    if (call->as.list.len != 3) {
        diag_emit(DIAG_ERROR, call->span,
                  "ptr-write requires exactly 2 arguments: (ptr-write ptr value)");
        return NULL;
    }
    if (e->unsafe_depth == 0) {
        diag_emit(DIAG_ERROR, call->span,
                  "ptr-write requires an enclosing (unsafe ...) block");
        return NULL;
    }
    Expr *ptr = elab_form(e, call->as.list.items[1]);
    if (!ptr) return NULL;
    Expr *value = elab_form(e, call->as.list.items[2]);
    if (!value) return NULL;
    if (ptr->type.kind != TY_PTR_VOID) {
        diag_emit(DIAG_ERROR, call->span,
                  "ptr-write requires ptr<void> as first argument, got %s", type_name(ptr->type));
        return NULL;
    }
    const BuiltinSpec *spec = builtin_lookup(e->sym_ptr_write, ptr->type, 2);
    if (!spec) {
        diag_emit(DIAG_ERROR, call->span, "ptr-write: builtin lookup failed");
        return NULL;
    }
    Expr *out = expr_new(e->arena, EX_BUILTIN, spec->result_type, call->span);
    out->as.builtin.spec = spec;
    Expr **args = arena_alloc(e->arena, 2 * sizeof(Expr *));
    args[0] = ptr;
    args[1] = value;
    out->as.builtin.args = args;
    out->as.builtin.n = 2;
    return out;
}

Expr *elab_ptr_add(Elab *e, const Form *call) {
    /* (ptr-add ptr offset) - pointer arithmetic: add offset to pointer */
    if (call->as.list.len != 3) {
        diag_emit(DIAG_ERROR, call->span,
                  "ptr-add requires exactly 2 arguments: (ptr-add ptr offset)");
        return NULL;
    }
    if (e->unsafe_depth == 0) {
        diag_emit(DIAG_ERROR, call->span,
                  "ptr-add requires an enclosing (unsafe ...) block");
        return NULL;
    }
    Expr *ptr = elab_form(e, call->as.list.items[1]);
    if (!ptr) return NULL;
    Expr *offset = elab_form(e, call->as.list.items[2]);
    if (!offset) return NULL;
    if (ptr->type.kind != TY_PTR_VOID) {
        diag_emit(DIAG_ERROR, call->span,
                  "ptr-add requires ptr<void> as first argument, got %s", type_name(ptr->type));
        return NULL;
    }
    if (offset->type.kind != TY_INT) {
        diag_emit(DIAG_ERROR, call->span,
                  "ptr-add requires int as second argument, got %s", type_name(offset->type));
        return NULL;
    }
    const BuiltinSpec *spec = builtin_lookup(e->sym_ptr_add, ptr->type, 2);
    if (!spec) {
        diag_emit(DIAG_ERROR, call->span, "ptr-add: builtin lookup failed");
        return NULL;
    }
    Expr *out = expr_new(e->arena, EX_BUILTIN, spec->result_type, call->span);
    out->as.builtin.spec = spec;
    Expr **args = arena_alloc(e->arena, 2 * sizeof(Expr *));
    args[0] = ptr;
    args[1] = offset;
    out->as.builtin.args = args;
    out->as.builtin.n = 2;
    return out;
}

Expr *elab_ptr_sub(Elab *e, const Form *call) {
    /* (ptr-sub ptr offset) - pointer arithmetic: subtract offset from pointer */
    if (call->as.list.len != 3) {
        diag_emit(DIAG_ERROR, call->span,
                  "ptr-sub requires exactly 2 arguments: (ptr-sub ptr offset)");
        return NULL;
    }
    if (e->unsafe_depth == 0) {
        diag_emit(DIAG_ERROR, call->span,
                  "ptr-sub requires an enclosing (unsafe ...) block");
        return NULL;
    }
    Expr *ptr = elab_form(e, call->as.list.items[1]);
    if (!ptr) return NULL;
    Expr *offset = elab_form(e, call->as.list.items[2]);
    if (!offset) return NULL;
    if (ptr->type.kind != TY_PTR_VOID) {
        diag_emit(DIAG_ERROR, call->span,
                  "ptr-sub requires ptr<void> as first argument, got %s", type_name(ptr->type));
        return NULL;
    }
    if (offset->type.kind != TY_INT) {
        diag_emit(DIAG_ERROR, call->span,
                  "ptr-sub requires int as second argument, got %s", type_name(offset->type));
        return NULL;
    }
    const BuiltinSpec *spec = builtin_lookup(e->sym_ptr_sub, ptr->type, 2);
    if (!spec) {
        diag_emit(DIAG_ERROR, call->span, "ptr-sub: builtin lookup failed");
        return NULL;
    }
    Expr *out = expr_new(e->arena, EX_BUILTIN, spec->result_type, call->span);
    out->as.builtin.spec = spec;
    Expr **args = arena_alloc(e->arena, 2 * sizeof(Expr *));
    args[0] = ptr;
    args[1] = offset;
    out->as.builtin.args = args;
    out->as.builtin.n = 2;
    return out;
}

Expr *elab_ptr_nullq(Elab *e, const Form *call) {
    /* (ptr-null? ptr) - check if pointer is null */
    if (call->as.list.len != 2) {
        diag_emit(DIAG_ERROR, call->span,
                  "ptr-null? requires exactly 1 argument: (ptr-null? ptr)");
        return NULL;
    }
    if (e->unsafe_depth == 0) {
        diag_emit(DIAG_ERROR, call->span,
                  "ptr-null? requires an enclosing (unsafe ...) block");
        return NULL;
    }
    Expr *ptr = elab_form(e, call->as.list.items[1]);
    if (!ptr) return NULL;
    if (ptr->type.kind != TY_PTR_VOID) {
        diag_emit(DIAG_ERROR, call->span,
                  "ptr-null? requires ptr<void>, got %s", type_name(ptr->type));
        return NULL;
    }
    const BuiltinSpec *spec = builtin_lookup(e->sym_ptr_nullq, ptr->type, 1);
    if (!spec) {
        diag_emit(DIAG_ERROR, call->span, "ptr-null?: builtin lookup failed");
        return NULL;
    }
    Expr *out = expr_new(e->arena, EX_BUILTIN, spec->result_type, call->span);
    out->as.builtin.spec = spec;
    Expr **args = arena_alloc(e->arena, sizeof(Expr *));
    args[0] = ptr;
    out->as.builtin.args = args;
    out->as.builtin.n = 1;
    return out;
}

Expr *elab_ptr_of(Elab *e, const Form *call) {
    /* (ptr-of value) - get pointer to a value (address-of) */
    if (call->as.list.len != 2) {
        diag_emit(DIAG_ERROR, call->span,
                  "ptr-of requires exactly 1 argument: (ptr-of value)");
        return NULL;
    }
    if (e->unsafe_depth == 0) {
        diag_emit(DIAG_ERROR, call->span,
                  "ptr-of requires an enclosing (unsafe ...) block");
        return NULL;
    }
    Expr *value = elab_form(e, call->as.list.items[1]);
    if (!value) return NULL;
    const BuiltinSpec *spec = builtin_lookup(e->sym_ptr_of, value->type, 1);
    if (!spec) {
        diag_emit(DIAG_ERROR, call->span, "ptr-of: builtin lookup failed");
        return NULL;
    }
    Expr *out = expr_new(e->arena, EX_BUILTIN, spec->result_type, call->span);
    out->as.builtin.spec = spec;
    Expr **args = arena_alloc(e->arena, sizeof(Expr *));
    args[0] = value;
    out->as.builtin.args = args;
    out->as.builtin.n = 1;
    return out;
}

Expr *elab_unsafe_cast(Elab *e, const Form *call) {
    /* (unsafe-cast value to-type) - C-style cast
     * For v1, we just check we're in an unsafe block and emit a cast to int64_t
     * This is a simplified implementation; full type checking comes later */
    if (call->as.list.len != 3) {
        diag_emit(DIAG_ERROR, call->span,
                  "unsafe-cast requires exactly 2 arguments: (unsafe-cast value to-type)");
        return NULL;
    }
    if (e->unsafe_depth == 0) {
        diag_emit(DIAG_ERROR, call->span,
                  "unsafe-cast requires an enclosing (unsafe ...) block");
        return NULL;
    }
    Expr *value = elab_form(e, call->as.list.items[1]);
    if (!value) return NULL;
    /* For v1, we ignore the target type and just cast to int64_t */
    Form *to_type_form = call->as.list.items[2];
    /* We don't validate the target type for v1; just require it exists */
    if (!to_type_form) {
        diag_emit(DIAG_ERROR, call->span, "unsafe-cast requires a target type");
        return NULL;
    }
    const BuiltinSpec *spec = builtin_lookup(e->sym_unsafe_cast, value->type, 2);
    if (!spec) {
        diag_emit(DIAG_ERROR, call->span, "unsafe-cast: builtin lookup failed");
        return NULL;
    }
    /* Result type is int for v1 */
    Type result_type = TYPE_INT;
    Expr *out = expr_new(e->arena, EX_BUILTIN, result_type, call->span);
    out->as.builtin.spec = spec;
    Expr **args = arena_alloc(e->arena, 1 * sizeof(Expr *));
    args[0] = value;
    out->as.builtin.args = args;
    out->as.builtin.n = 1;
    return out;
}

Expr *elab_reinterpret(Elab *e, const Form *call) {
    /* (reinterpret value as-type) - bitwise reinterpretation, with a
     * compile-time size check.  Same lowering as transmute. */
    if (call->as.list.len != 3) {
        diag_emit(DIAG_ERROR, call->span,
                  "reinterpret requires exactly 2 arguments: (reinterpret value as-type)");
        return NULL;
    }
    if (e->unsafe_depth == 0) {
        diag_emit(DIAG_ERROR, call->span,
                  "reinterpret requires an enclosing (unsafe ...) block");
        return NULL;
    }
    Expr *value = elab_form(e, call->as.list.items[1]);
    if (!value) return NULL;
    /* Compile-time size check: source and target types must have the same size */
    Form *to_type_form = call->as.list.items[2];
    TypeKind target_kind = TY_UNKNOWN;
    if (to_type_form && to_type_form->tag == F_KEYWORD) {
        target_kind = typekind_from_symbol(to_type_form->as.sym->name);
    } else if (to_type_form && to_type_form->tag == F_SYM) {
        target_kind = typekind_from_symbol(to_type_form->as.sym->name);
    }
    int src_size = type_size_bytes(value->type.kind);
    int dst_size = (target_kind != TY_UNKNOWN) ? type_size_bytes(target_kind) : 0;
    if (src_size > 0 && dst_size > 0 && src_size != dst_size) {
        diag_emit(DIAG_ERROR, call->span,
                  "reinterpret: size mismatch — source type '%s' (%d bytes) vs target type '%s' (%d bytes)",
                  type_name(value->type), src_size,
                  to_type_form->as.sym->name, dst_size);
        return NULL;
    }
    const BuiltinSpec *spec = builtin_lookup(e->sym_reinterpret, value->type, 2);
    if (!spec) {
        diag_emit(DIAG_ERROR, call->span, "reinterpret: builtin lookup failed");
        return NULL;
    }
    /* The result has the target type, as transmute's does: a bit copy read
     * back as an `int` would print a float's bits as a number. */
    Type result_type = (target_kind != TY_UNKNOWN) ? (Type){ .kind = target_kind,
        .copy_kind = CK_COPY, .n_lifetimes = 0 } : TYPE_INT;
    Expr *out = expr_new(e->arena, EX_BUILTIN, result_type, call->span);
    out->as.builtin.spec = spec;
    /* One argument: a placeholder second slot evaluated `value` twice, on
     * both engines. */
    Expr **args = arena_alloc(e->arena, 1 * sizeof(Expr *));
    args[0] = value;
    out->as.builtin.args = args;
    out->as.builtin.n = 1;
    return out;
}

Expr *elab_transmute(Elab *e, const Form *call) {
    /* (transmute value to-type) - type-punning with compile-time size check */
    if (call->as.list.len != 3) {
        diag_emit(DIAG_ERROR, call->span,
                  "transmute requires exactly 2 arguments: (transmute value to-type)");
        return NULL;
    }
    if (e->unsafe_depth == 0) {
        diag_emit(DIAG_ERROR, call->span,
                  "transmute requires an enclosing (unsafe ...) block");
        return NULL;
    }
    Expr *value = elab_form(e, call->as.list.items[1]);
    if (!value) return NULL;

    /* Resolve the target type from the keyword or symbol form. */
    Form *to_type_form = call->as.list.items[2];
    TypeKind target_kind = TY_UNKNOWN;
    if (to_type_form->tag == F_KEYWORD || to_type_form->tag == F_SYM) {
        target_kind = typekind_from_symbol(to_type_form->as.sym->name);
    }

    /* Compile-time size check: source and target types must have the same size. */
    int src_size = type_size_bytes(value->type.kind);
    int dst_size = (target_kind != TY_UNKNOWN) ? type_size_bytes(target_kind) : 0;
    if (src_size > 0 && dst_size > 0 && src_size != dst_size) {
        diag_emit(DIAG_ERROR, call->span,
                  "transmute: size mismatch -- source type '%s' (%d bytes) vs "
                  "target type '%s' (%d bytes)",
                  type_name(value->type), src_size,
                  to_type_form->as.sym->name, dst_size);
        return NULL;
    }

    const BuiltinSpec *spec = builtin_lookup(e->sym_transmute, value->type, 2);
    if (!spec) {
        diag_emit(DIAG_ERROR, call->span, "transmute: builtin lookup failed");
        return NULL;
    }
    Type result_type = (target_kind != TY_UNKNOWN) ? (Type){ .kind = target_kind,
        .copy_kind = CK_COPY, .n_lifetimes = 0 } : TYPE_INT;
    Expr *out = expr_new(e->arena, EX_BUILTIN, result_type, call->span);
    out->as.builtin.spec = spec;
    Expr **args = arena_alloc(e->arena, 1 * sizeof(Expr *));
    args[0] = value;
    out->as.builtin.args = args;
    out->as.builtin.n = 1;
    return out;
}

Expr *elab_array_get_unchecked(Elab *e, const Form *call) {
    /* (array-get-unchecked ptr index) - get element at index without bounds check */
    if (call->as.list.len != 3) {
        diag_emit(DIAG_ERROR, call->span,
                  "array-get-unchecked requires exactly 2 arguments: (array-get-unchecked ptr index)");
        return NULL;
    }
    if (e->unsafe_depth == 0) {
        diag_emit(DIAG_ERROR, call->span,
                  "array-get-unchecked requires an enclosing (unsafe ...) block");
        return NULL;
    }
    Expr *ptr = elab_form(e, call->as.list.items[1]);
    if (!ptr) return NULL;
    Expr *index = elab_form(e, call->as.list.items[2]);
    if (!index) return NULL;
    if (ptr->type.kind != TY_PTR_VOID) {
        diag_emit(DIAG_ERROR, call->span,
                  "array-get-unchecked requires ptr<void> as first argument, got %s",
                  type_name(ptr->type));
        return NULL;
    }
    if (index->type.kind != TY_INT) {
        diag_emit(DIAG_ERROR, call->span,
                  "array-get-unchecked requires int as second argument, got %s",
                  type_name(index->type));
        return NULL;
    }
    const BuiltinSpec *spec = builtin_lookup(e->sym_array_get_unchecked, ptr->type, 2);
    if (!spec) {
        diag_emit(DIAG_ERROR, call->span, "array-get-unchecked: builtin lookup failed");
        return NULL;
    }
    Expr *out = expr_new(e->arena, EX_BUILTIN, spec->result_type, call->span);
    out->as.builtin.spec = spec;
    Expr **args = arena_alloc(e->arena, 2 * sizeof(Expr *));
    args[0] = ptr;
    args[1] = index;
    out->as.builtin.args = args;
    out->as.builtin.n = 2;
    return out;
}

Expr *elab_array_set_unchecked(Elab *e, const Form *call) {
    /* (array-set-unchecked ptr index value) - set element at index without bounds check */
    if (call->as.list.len != 4) {
        diag_emit(DIAG_ERROR, call->span,
                  "array-set-unchecked requires exactly 3 arguments: (array-set-unchecked ptr index value)");
        return NULL;
    }
    if (e->unsafe_depth == 0) {
        diag_emit(DIAG_ERROR, call->span,
                  "array-set-unchecked requires an enclosing (unsafe ...) block");
        return NULL;
    }
    Expr *ptr = elab_form(e, call->as.list.items[1]);
    if (!ptr) return NULL;
    Expr *index = elab_form(e, call->as.list.items[2]);
    if (!index) return NULL;
    Expr *value = elab_form(e, call->as.list.items[3]);
    if (!value) return NULL;
    if (ptr->type.kind != TY_PTR_VOID) {
        diag_emit(DIAG_ERROR, call->span,
                  "array-set-unchecked requires ptr<void> as first argument, got %s",
                  type_name(ptr->type));
        return NULL;
    }
    if (index->type.kind != TY_INT) {
        diag_emit(DIAG_ERROR, call->span,
                  "array-set-unchecked requires int as second argument, got %s",
                  type_name(index->type));
        return NULL;
    }
    const BuiltinSpec *spec = builtin_lookup(e->sym_array_set_unchecked, ptr->type, 3);
    if (!spec) {
        diag_emit(DIAG_ERROR, call->span, "array-set-unchecked: builtin lookup failed");
        return NULL;
    }
    Expr *out = expr_new(e->arena, EX_BUILTIN, spec->result_type, call->span);
    out->as.builtin.spec = spec;
    Expr **args = arena_alloc(e->arena, 3 * sizeof(Expr *));
    args[0] = ptr;
    args[1] = index;
    args[2] = value;
    out->as.builtin.args = args;
    out->as.builtin.n = 3;
    return out;
}

Expr *elab_raw_malloc(Elab *e, const Form *call) {
    /* (raw-malloc size) - allocate raw memory */
    if (call->as.list.len != 2) {
        diag_emit(DIAG_ERROR, call->span,
                  "raw-malloc requires exactly 1 argument: (raw-malloc size)");
        return NULL;
    }
    if (e->unsafe_depth == 0) {
        diag_emit(DIAG_ERROR, call->span,
                  "raw-malloc requires an enclosing (unsafe ...) block");
        return NULL;
    }
    Expr *size = elab_form(e, call->as.list.items[1]);
    if (!size) return NULL;
    if (size->type.kind != TY_INT) {
        diag_emit(DIAG_ERROR, call->span,
                  "raw-malloc requires int as argument, got %s", type_name(size->type));
        return NULL;
    }
    const BuiltinSpec *spec = builtin_lookup(e->sym_raw_malloc, size->type, 1);
    if (!spec) {
        diag_emit(DIAG_ERROR, call->span, "raw-malloc: builtin lookup failed");
        return NULL;
    }
    Expr *out = expr_new(e->arena, EX_BUILTIN, spec->result_type, call->span);
    out->as.builtin.spec = spec;
    Expr **args = arena_alloc(e->arena, sizeof(Expr *));
    args[0] = size;
    out->as.builtin.args = args;
    out->as.builtin.n = 1;
    return out;
}

Expr *elab_raw_free(Elab *e, const Form *call) {
    /* (raw-free ptr) - free raw memory */
    if (call->as.list.len != 2) {
        diag_emit(DIAG_ERROR, call->span,
                  "raw-free requires exactly 1 argument: (raw-free ptr)");
        return NULL;
    }
    if (e->unsafe_depth == 0) {
        diag_emit(DIAG_ERROR, call->span,
                  "raw-free requires an enclosing (unsafe ...) block");
        return NULL;
    }
    Expr *ptr = elab_form(e, call->as.list.items[1]);
    if (!ptr) return NULL;
    if (ptr->type.kind != TY_PTR_VOID) {
        diag_emit(DIAG_ERROR, call->span,
                  "raw-free requires ptr<void> as argument, got %s", type_name(ptr->type));
        return NULL;
    }
    const BuiltinSpec *spec = builtin_lookup(e->sym_raw_free, ptr->type, 1);
    if (!spec) {
        diag_emit(DIAG_ERROR, call->span, "raw-free: builtin lookup failed");
        return NULL;
    }
    Expr *out = expr_new(e->arena, EX_BUILTIN, spec->result_type, call->span);
    out->as.builtin.spec = spec;
    Expr **args = arena_alloc(e->arena, sizeof(Expr *));
    args[0] = ptr;
    out->as.builtin.args = args;
    out->as.builtin.n = 1;
    return out;
}

Expr *elab_raw_realloc(Elab *e, const Form *call) {
    /* (raw-realloc ptr new-size) - reallocate raw memory */
    if (call->as.list.len != 3) {
        diag_emit(DIAG_ERROR, call->span,
                  "raw-realloc requires exactly 2 arguments: (raw-realloc ptr new-size)");
        return NULL;
    }
    if (e->unsafe_depth == 0) {
        diag_emit(DIAG_ERROR, call->span,
                  "raw-realloc requires an enclosing (unsafe ...) block");
        return NULL;
    }
    Expr *ptr = elab_form(e, call->as.list.items[1]);
    if (!ptr) return NULL;
    Expr *new_size = elab_form(e, call->as.list.items[2]);
    if (!new_size) return NULL;
    if (ptr->type.kind != TY_PTR_VOID) {
        diag_emit(DIAG_ERROR, call->span,
                  "raw-realloc requires ptr<void> as first argument, got %s",
                  type_name(ptr->type));
        return NULL;
    }
    if (new_size->type.kind != TY_INT) {
        diag_emit(DIAG_ERROR, call->span,
                  "raw-realloc requires int as second argument, got %s",
                  type_name(new_size->type));
        return NULL;
    }
    const BuiltinSpec *spec = builtin_lookup(e->sym_raw_realloc, ptr->type, 2);
    if (!spec) {
        diag_emit(DIAG_ERROR, call->span, "raw-realloc: builtin lookup failed");
        return NULL;
    }
    Expr *out = expr_new(e->arena, EX_BUILTIN, spec->result_type, call->span);
    out->as.builtin.spec = spec;
    Expr **args = arena_alloc(e->arena, 2 * sizeof(Expr *));
    args[0] = ptr;
    args[1] = new_size;
    out->as.builtin.args = args;
    out->as.builtin.n = 2;
    return out;
}

Expr *elab_raw_memcpy(Elab *e, const Form *call) {
    /* (raw-memcpy dest src n) - copy memory */
    if (call->as.list.len != 4) {
        diag_emit(DIAG_ERROR, call->span,
                  "raw-memcpy requires exactly 3 arguments: (raw-memcpy dest src n)");
        return NULL;
    }
    if (e->unsafe_depth == 0) {
        diag_emit(DIAG_ERROR, call->span,
                  "raw-memcpy requires an enclosing (unsafe ...) block");
        return NULL;
    }
    Expr *dest = elab_form(e, call->as.list.items[1]);
    if (!dest) return NULL;
    Expr *src = elab_form(e, call->as.list.items[2]);
    if (!src) return NULL;
    Expr *n = elab_form(e, call->as.list.items[3]);
    if (!n) return NULL;
    if (dest->type.kind != TY_PTR_VOID) {
        diag_emit(DIAG_ERROR, call->span,
                  "raw-memcpy requires ptr<void> as first argument, got %s",
                  type_name(dest->type));
        return NULL;
    }
    if (src->type.kind != TY_PTR_VOID) {
        diag_emit(DIAG_ERROR, call->span,
                  "raw-memcpy requires ptr<void> as second argument, got %s",
                  type_name(src->type));
        return NULL;
    }
    if (n->type.kind != TY_INT) {
        diag_emit(DIAG_ERROR, call->span,
                  "raw-memcpy requires int as third argument, got %s",
                  type_name(n->type));
        return NULL;
    }
    const BuiltinSpec *spec = builtin_lookup(e->sym_raw_memcpy, dest->type, 3);
    if (!spec) {
        diag_emit(DIAG_ERROR, call->span, "raw-memcpy: builtin lookup failed");
        return NULL;
    }
    Expr *out = expr_new(e->arena, EX_BUILTIN, spec->result_type, call->span);
    out->as.builtin.spec = spec;
    Expr **args = arena_alloc(e->arena, 3 * sizeof(Expr *));
    args[0] = dest;
    args[1] = src;
    args[2] = n;
    out->as.builtin.args = args;
    out->as.builtin.n = 3;
    return out;
}

Expr *elab_raw_memset(Elab *e, const Form *call) {
    /* (raw-memset dest byte n) - set memory to byte value */
    if (call->as.list.len != 4) {
        diag_emit(DIAG_ERROR, call->span,
                  "raw-memset requires exactly 3 arguments: (raw-memset dest byte n)");
        return NULL;
    }
    if (e->unsafe_depth == 0) {
        diag_emit(DIAG_ERROR, call->span,
                  "raw-memset requires an enclosing (unsafe ...) block");
        return NULL;
    }
    Expr *dest = elab_form(e, call->as.list.items[1]);
    if (!dest) return NULL;
    Expr *byte_val = elab_form(e, call->as.list.items[2]);
    if (!byte_val) return NULL;
    Expr *n = elab_form(e, call->as.list.items[3]);
    if (!n) return NULL;
    if (dest->type.kind != TY_PTR_VOID) {
        diag_emit(DIAG_ERROR, call->span,
                  "raw-memset requires ptr<void> as first argument, got %s",
                  type_name(dest->type));
        return NULL;
    }
    if (byte_val->type.kind != TY_INT) {
        diag_emit(DIAG_ERROR, call->span,
                  "raw-memset requires int as second argument, got %s",
                  type_name(byte_val->type));
        return NULL;
    }
    if (n->type.kind != TY_INT) {
        diag_emit(DIAG_ERROR, call->span,
                  "raw-memset requires int as third argument, got %s",
                  type_name(n->type));
        return NULL;
    }
    const BuiltinSpec *spec = builtin_lookup(e->sym_raw_memset, dest->type, 3);
    if (!spec) {
        diag_emit(DIAG_ERROR, call->span, "raw-memset: builtin lookup failed");
        return NULL;
    }
    Expr *out = expr_new(e->arena, EX_BUILTIN, spec->result_type, call->span);
    out->as.builtin.spec = spec;
    Expr **args = arena_alloc(e->arena, 3 * sizeof(Expr *));
    args[0] = dest;
    args[1] = byte_val;
    args[2] = n;
    out->as.builtin.args = args;
    out->as.builtin.n = 3;
    return out;
}

Expr *elab_c_call(Elab *e, const Form *call) {
    /* (c-call fn-name arg1 arg2 ...) - call a C function by name
     * For v1, we emit a direct C call. The function must be declared via extern-c.
     * We look up the function name in the scope and emit a call to it.
     * This is a simplified implementation for v1. */
    if (call->as.list.len < 2) {
        diag_emit(DIAG_ERROR, call->span,
                  "c-call requires at least a function name: (c-call fn-name arg1 ...)");
        return NULL;
    }
    if (e->unsafe_depth == 0) {
        diag_emit(DIAG_ERROR, call->span,
                  "c-call requires an enclosing (unsafe ...) block");
        return NULL;
    }
    Form *name_f = call->as.list.items[1];
    if (name_f->tag != F_SYM) {
        diag_emit(DIAG_ERROR, name_f->span, "c-call: function name must be a symbol");
        return NULL;
    }
    /* Look up the function in the scope - it should be an extern-c declaration */
    Binding *b = scope_lookup(e->scope, name_f->as.sym);
    if (!b) {
        diag_emit(DIAG_ERROR, name_f->span,
                  "c-call: function '%s' is not declared (use extern-c to declare it)",
                  name_f->as.sym->name);
        return NULL;
    }
    if (b->type.kind != TY_FN) {
        diag_emit(DIAG_ERROR, name_f->span,
                  "c-call: '%s' is not a function", name_f->as.sym->name);
        return NULL;
    }
    /* Elaborate arguments */
    uint32_t n_args = call->as.list.len - 2;
    Expr **args = (Expr **)arena_alloc(e->arena, n_args * sizeof(Expr *));
    for (uint32_t i = 0; i < n_args; i++) {
        args[i] = elab_form(e, call->as.list.items[i + 2]);
        if (!args[i]) return NULL;
    }
    /* Create a call expression */
    Expr *fn_expr = expr_new(e->arena, EX_VAR, b->type, name_f->span);
    fn_expr->as.var.binding = b;
    
    /* For v1, we use the simple call_ structure which stores a binding */
    Expr *out = expr_new(e->arena, EX_CALL, type_from_kind(TY_INT), call->span);
    out->as.call_.fn_binding = b;
    out->as.call_.args = args;
    out->as.call_.n_args = n_args;
    out->as.call_.fn_expr = NULL;
    return out;
}

/* jit-ffi-c2mir-plan F4: a `call-ptr` signature slot may name a by-value
 * aggregate.  A `defstruct` lowers to a single-variant record ADT whose C
 * emission is already an exact by-value struct with the declared field types
 * (`struct tur_adt_Vec2 { double x; double y; }`), so the compiled path
 * passes one by naming the type -- there is no marshalling step and no
 * layout guesswork.  The interpreter builds the bytes itself; see
 * eval_call_ptr.
 *
 * Returns the resolved TY_ADT type, or a TY_UNKNOWN type when `name` does
 * not name a struct at all (caller falls through to its own diagnostic).
 * `*fatal` is set when the name IS a struct but cannot cross the boundary,
 * in which case a diagnostic has already been emitted. */
static Type call_ptr_aggregate_type(Elab *e, const Symbol *name, Span span,
                                    bool *fatal) {
    Type unknown = type_from_kind(TY_UNKNOWN);
    *fatal = false;
    Type *t = elab_lookup_type_by_name(e, name);
    if (!t || t->kind != TY_ADT || !t->as.adt_.def) return unknown;

    const AdtDef *def = t->as.adt_.def;
    /* Only a product type has a by-value C layout.  A multi-constructor sum
     * carries a tag and (in the general case) a union; that is a real ABI
     * shape but not one any C API declares, so it is out of scope rather
     * than mis-described. */
    if (def->n_ctors != 1 || !def->ctors[0]->is_record) {
        diag_emit(DIAG_ERROR, span,
                  "call-ptr: '%s' is not a by-value aggregate -- only a "
                  "single-constructor record (a defstruct, or a defdata with "
                  "one record variant) has a C struct layout",
                  def->name ? def->name : name->name);
        *fatal = true;
        return unknown;
    }
    /* A `:heap` record's natural ABI is a pointer to a header, not the
     * aggregate itself; passing it by value would pass the header. */
    if (def->is_heap) {
        diag_emit(DIAG_ERROR, span,
                  "call-ptr: '%s' is a :heap record -- its ABI is a pointer, "
                  "not a by-value aggregate; declare the slot :ptr instead",
                  def->name ? def->name : name->name);
        *fatal = true;
        return unknown;
    }
    if (def->n_type_params != 0) {
        diag_emit(DIAG_ERROR, span,
                  "call-ptr: '%s' is parametric -- a by-value aggregate slot "
                  "needs a monomorphic layout",
                  def->name ? def->name : name->name);
        *fatal = true;
        return unknown;
    }

    const CtorDef *ct = def->ctors[0];
    if (ct->n_fields == 0) {
        diag_emit(DIAG_ERROR, span,
                  "call-ptr: '%s' has no fields -- an empty aggregate has no "
                  "portable C layout", def->name ? def->name : name->name);
        *fatal = true;
        return unknown;
    }
    for (uint32_t i = 0; i < ct->n_fields; i++) {
        if (tur_jit_ffi_member_code_for_kind(ct->fields[i].kind) == 0) {
            diag_emit(DIAG_ERROR, span,
                      "call-ptr: field '%s' of '%s' is %s, which has no "
                      "by-value C member type -- aggregate fields must be "
                      "scalars (:int, :float, :float32, :bool, :cstr, :ptr, "
                      "sized ints)",
                      ct->fields[i].name ? ct->fields[i].name : "(positional)",
                      def->name ? def->name : name->name,
                      typekind_to_string(ct->fields[i].kind));
            *fatal = true;
            return unknown;
        }
    }
    return *t;
}

/* Parse a `[T1 T2 -> R]` C-signature vector, shared by `call-ptr` (F3/F4)
 * and `callback-ptr` (F5) -- the two directions describe the same boundary,
 * so they read the same notation.  The vocabulary is the FFI thunk's
 * (turi/jit_ffi.h): int-class scalars, floats, cstr, ptr, the name of a
 * by-value record, and (return position only) :void.  Zero-parameter form is
 * `[-> R]`.  `what` names the form in diagnostics.  Returns false with a
 * diagnostic already emitted. */
static bool elab_c_sig_vector(Elab *e, const Form *sig_f, const char *what,
                              Type *out_ret, Type **out_params,
                              uint32_t *out_n) {
    if (sig_f->tag != F_VEC) {
        diag_emit(DIAG_ERROR, sig_f->span,
                  "%s: signature must be a vector [T1 T2 -> R]", what);
        return false;
    }
    uint32_t sig_len = sig_f->as.list.len;
    uint32_t arrow_at = sig_len;
    for (uint32_t i = 0; i < sig_len; i++) {
        const Form *t = sig_f->as.list.items[i];
        if (t->tag == F_SYM && t->as.sym->len == 2 &&
            memcmp(t->as.sym->name, "->", 2) == 0) {
            arrow_at = i;
            break;
        }
    }
    if (arrow_at == sig_len || arrow_at + 2 != sig_len) {
        diag_emit(DIAG_ERROR, sig_f->span,
                  "%s: signature needs `->` followed by exactly one "
                  "return type: [T1 T2 -> R]", what);
        return false;
    }

    uint32_t n_params = arrow_at;
    Type *param_types =
        (Type *)arena_alloc(e->arena, (n_params ? n_params : 1) * sizeof(Type));
    Type ret_type = type_from_kind(TY_NIL);
    for (uint32_t i = 0; i <= n_params; i++) {
        bool is_return = (i == n_params);
        const Form *t = sig_f->as.list.items[is_return ? n_params + 1 : i];
        TypeKind k = TY_UNKNOWN;
        if (t->tag == F_KEYWORD || t->tag == F_SYM) {
            const char *nm = t->as.sym->name;
            k = typekind_from_symbol(nm);
            if (k == TY_UNKNOWN) {
                if (strcmp(nm, "void") == 0)      k = TY_NIL;
                else if (strcmp(nm, "ptr") == 0)  k = TY_PTR_VOID;
            }
        }
        /* F4: a bare name that is not a scalar keyword may be a by-value
         * aggregate.  Tried before the '?' diagnostic so a struct name gets
         * the specific reason it was rejected, not the scalar-list message. */
        if (k == TY_UNKNOWN && t->tag == F_SYM) {
            bool fatal = false;
            Type agg = call_ptr_aggregate_type(e, t->as.sym, t->span, &fatal);
            if (fatal) return false;
            if (agg.kind == TY_ADT) {
                if (is_return) ret_type = agg;
                else           param_types[i] = agg;
                continue;
            }
        }
        char cls = (k == TY_UNKNOWN)
                       ? '?'
                       : tur_jit_ffi_class_for_kind(k, is_return);
        if (cls == '?') {
            diag_emit(DIAG_ERROR, t->span,
                      "%s: unsupported %s type in signature -- expected "
                      "a scalar (:int, :float, :float32, :bool, :cstr, :ptr, "
                      "sized ints%s) or the name of a by-value record",
                      what, is_return ? "return" : "parameter",
                      is_return ? ", :void" : "");
            return false;
        }
        if (is_return) ret_type = type_from_kind(k);
        else           param_types[i] = type_from_kind(k);
    }
    *out_ret    = ret_type;
    *out_params = param_types;
    *out_n      = n_params;
    return true;
}

/* jit-ffi-c2mir-plan F3: `(call-ptr p [T1 T2 -> R] args...)` -- call an
 * arbitrary function pointer with a signature stated at the site.  The
 * pointer typically comes from dlsym, which until this form existed had no
 * way to be invoked at all.  AOT codegen is a pure cast-and-call; turi
 * routes through the JIT FFI thunk provider.  Requires an `unsafe` block
 * (exactly like c-call); the `jit-ffi` experiment graduated 2026-08-21, so
 * there is no longer an enable gate.  The turi path still needs a
 * `-DTUR_JIT=ON` build and says so cleanly when it does not have one. */
Expr *elab_call_ptr(Elab *e, const Form *call) {
    g_needs_dlfcn = true;   /* emitted C needs <dlfcn.h> + -ldl */
    if (e->unsafe_depth == 0) {
        diag_emit(DIAG_ERROR, call->span,
                  "call-ptr requires an enclosing (unsafe ...) block");
        return NULL;
    }
    if (call->as.list.len < 3) {
        diag_emit(DIAG_ERROR, call->span,
                  "call-ptr requires a pointer and a signature: "
                  "(call-ptr p [T1 T2 -> R] args...)");
        return NULL;
    }

    uint32_t n_params = 0;
    Type *param_types = NULL;
    Type  ret_type    = type_from_kind(TY_NIL);
    if (!elab_c_sig_vector(e, call->as.list.items[2], "call-ptr",
                           &ret_type, &param_types, &n_params))
        return NULL;

    /* The pointer expression: a dlsym result (:ptr<void>) or a raw :int
     * address. */
    Expr *pexpr = elab_form(e, call->as.list.items[1]);
    if (!pexpr) return NULL;
    if (pexpr->type.kind != TY_PTR_VOID && pexpr->type.kind != TY_INT) {
        diag_emit(DIAG_ERROR, call->as.list.items[1]->span,
                  "call-ptr: pointer must be ptr<void> (dlsym result) or "
                  ":int, got %s", type_name(pexpr->type));
        return NULL;
    }

    /* Arguments, checked count- and class-wise against the signature; each
     * is cast to its stated C parameter type by codegen. */
    uint32_t n_args = call->as.list.len - 3;
    if (n_args != n_params) {
        diag_emit(DIAG_ERROR, call->span,
                  "call-ptr: signature declares %u parameter%s, got %u "
                  "argument%s", (unsigned)n_params, n_params == 1 ? "" : "s",
                  (unsigned)n_args, n_args == 1 ? "" : "s");
        return NULL;
    }
    Expr **args =
        (Expr **)arena_alloc(e->arena, (n_args ? n_args : 1) * sizeof(Expr *));
    for (uint32_t a = 0; a < n_args; a++) {
        args[a] = elab_form(e, call->as.list.items[3 + a]);
        if (!args[a]) return NULL;
        /* F4: an aggregate slot is matched by IDENTITY, not by class -- two
         * records with the same field kinds still describe different C
         * types, and silently accepting one for the other is exactly the
         * mis-call this form exists to make impossible. */
        if (param_types[a].kind == TY_ADT) {
            if (args[a]->type.kind != TY_ADT ||
                args[a]->type.as.adt_.def != param_types[a].as.adt_.def) {
                diag_emit(DIAG_ERROR, call->as.list.items[3 + a]->span,
                          "call-ptr: argument %u is %s, but the signature "
                          "declares the by-value record %s",
                          (unsigned)a, type_name(args[a]->type),
                          param_types[a].as.adt_.def->name);
                return NULL;
            }
            continue;
        }
        char want = tur_jit_ffi_class_for_kind(param_types[a].kind, 0);
        char got  = tur_jit_ffi_class_for_kind(args[a]->type.kind, 0);
        /* int-class accepts any int-class value -- the exact-width codes
         * (scalar-width-fidelity) distinguish the C declaration the thunk
         * emits, not what an argument may be passed as; the width cast
         * happens at the boundary.  Float-class also accepts ints
         * (widened, matching the interpreter's marshaller). */
        bool okc = (tur_jit_ffi_class_is_int(want) &&
                    tur_jit_ffi_class_is_int(got)) ||
                   (want == got) ||
                   ((want == 'f' || want == 'F') &&
                    tur_jit_ffi_class_is_int(got));
        if (!okc) {
            diag_emit(DIAG_ERROR, call->as.list.items[3 + a]->span,
                      "call-ptr: argument %u is %s, but the signature "
                      "declares %s", (unsigned)a, type_name(args[a]->type),
                      type_name(param_types[a]));
            return NULL;
        }
    }

    CallPtrSig *ps = (CallPtrSig *)arena_alloc(e->arena, sizeof(CallPtrSig));
    ps->return_type = ret_type;
    ps->param_types = param_types;
    ps->n_params    = n_params;
    ps->is_callback = false;

    Expr *out = expr_new(e->arena, EX_CALL, ret_type, call->span);
    out->as.call_.fn_binding = NULL;
    out->as.call_.fn_expr    = pexpr;
    out->as.call_.args       = args;
    out->as.call_.n_args     = n_args;
    out->as.call_.ptr_sig    = ps;
    return out;
}

/* jit-ffi-c2mir-plan F5: `(callback-ptr f [T1 T2 -> R])` -- the reverse of
 * call-ptr.  Produces a raw C function pointer with the stated signature
 * that, when a C library calls it, runs the Turmeric closure `f`.  This is
 * what lets a C API that takes a callback (qsort's comparator, an event
 * loop's handler) be driven from Turmeric at all.
 *
 * Compiled: a per-site static trampoline plus a static slot holding the
 * closure -- see emit_expr.c.  Interpreted: the provider synthesizes one
 * with c2mir, with the context address baked in as a literal.
 *
 * Lifetime is the PROCESS, deliberately, matching turi's existing closure
 * policy: a C library that holds a callback pointer has no way to tell us it
 * is done with it, so there is no safe moment to reclaim one.  The plan's
 * optional `callback-free!` is not implemented -- it would be a no-op on the
 * compiled path (where the trampoline is a static function, not an
 * allocation), and a use-after-free primitive on the other. */
Expr *elab_callback_ptr(Elab *e, const Form *call) {
    if (e->unsafe_depth == 0) {
        diag_emit(DIAG_ERROR, call->span,
                  "callback-ptr requires an enclosing (unsafe ...) block");
        return NULL;
    }
    if (call->as.list.len != 3) {
        diag_emit(DIAG_ERROR, call->span,
                  "callback-ptr requires a function and a signature: "
                  "(callback-ptr f [T1 T2 -> R])");
        return NULL;
    }

    uint32_t n_params = 0;
    Type *param_types = NULL;
    Type  ret_type    = type_from_kind(TY_NIL);
    if (!elab_c_sig_vector(e, call->as.list.items[2], "callback-ptr",
                           &ret_type, &param_types, &n_params))
        return NULL;

    Expr *fexpr = elab_form(e, call->as.list.items[1]);
    if (!fexpr) return NULL;
    if (fexpr->type.kind != TY_FN) {
        diag_emit(DIAG_ERROR, call->as.list.items[1]->span,
                  "callback-ptr: first argument must be a function, got %s",
                  type_name(fexpr->type));
        return NULL;
    }
    /* Deliberately narrow: the function must lower to a PLAIN C FUNCTION.
     * That admits a top-level defn and an inline non-capturing lambda (which
     * is lifted to exactly such a function) -- both land as a global binding
     * -- and rejects anything carrying an environment.
     *
     * A C callback slot is a bare function pointer with no room for an
     * environment, so a capturing closure would need the trampoline to
     * recover the env from the closure's runtime representation.  That
     * representation is not currently uniform -- a `:fn` value is variously a
     * bare function pointer, a boxed closure whose slot 0 is the code
     * pointer, or a `__poly_` adapter, and one of those combinations already
     * SIGSEGVs on the ordinary argument path
     * (docs/archive/let-bound-noncapturing-lambda-segfaults-as-fn-arg.md).
     * Building an FFI trampoline on top of that would turn a representation
     * mismatch into a wild jump from inside a C library.
     *
     * Enforced HERE rather than in codegen so both paths agree: the
     * interpreter could accept any closure, but a form that means different
     * things under `tur run` and `tur --interpret` is worse than one that is
     * uniformly narrow.  Note this rejects a LET-BOUND non-capturing lambda
     * even though it could in principle work -- the binding is local, and
     * that is the very shape the SIGSEGV above lives in. */
    if (fexpr->kind != EX_VAR || !fexpr->as.var.binding ||
        !fexpr->as.var.binding->is_global) {
        diag_emit(DIAG_ERROR, call->as.list.items[1]->span,
                  "callback-ptr: the callback must be a top-level function, "
                  "named directly or written inline with no captures -- a C "
                  "callback slot is a bare function pointer with no room for "
                  "a captured environment");
        return NULL;
    }
    if (fexpr->type.as.fn.arity != n_params) {
        diag_emit(DIAG_ERROR, call->as.list.items[1]->span,
                  "callback-ptr: the signature declares %u parameter%s but the "
                  "function takes %u", (unsigned)n_params,
                  n_params == 1 ? "" : "s",
                  (unsigned)fexpr->type.as.fn.arity);
        return NULL;
    }
    /* Class-check each slot against the closure's own type.  A mismatch here
     * is a register-class error at the C boundary -- the callee would read an
     * xmm register for a value the caller left in a general-purpose one --
     * so it is worth catching at the site rather than at the crash. */
    for (uint32_t i = 0; i < n_params; i++) {
        TypeKind fk = (TypeKind)fexpr->type.as.fn.arg_kinds[i];
        /* F4 follow-on: a record slot requires the function to take a record
         * there too.  The fn type carries per-arg KINDS only, so identity
         * (same record, not just "a record") is enforced where the type
         * survives: the interpreter's dispatch builds the SIG's record and
         * the compiled adapter passes the aggregate through uncast, so a
         * wrong record is a C type error in the generated adapter. */
        if (param_types[i].kind == TY_ADT) {
            if (fk != TY_ADT) {
                diag_emit(DIAG_ERROR, call->as.list.items[1]->span,
                          "callback-ptr: parameter %u is the by-value record "
                          "%s in the signature but %s in the function",
                          (unsigned)i, type_name(param_types[i]),
                          typekind_to_string(fk));
                return NULL;
            }
            continue;
        }
        char want = tur_jit_ffi_class_for_kind(param_types[i].kind, 0);
        char got  = tur_jit_ffi_class_for_kind(fk, 0);
        bool okc = (tur_jit_ffi_class_is_int(want) &&
                    tur_jit_ffi_class_is_int(got)) ||
                   (want == got) ||
                   ((want == 'f' || want == 'F') && (got == 'f' || got == 'F'));
        if (!okc) {
            diag_emit(DIAG_ERROR, call->as.list.items[1]->span,
                      "callback-ptr: parameter %u is %s in the signature but "
                      "%s in the function", (unsigned)i,
                      type_name(param_types[i]),
                      typekind_to_string(fk));
            return NULL;
        }
    }
    {
        TypeKind fr = fexpr->type.as.fn.result_kind;
        bool okc;
        if (ret_type.kind == TY_ADT) {
            /* F4 follow-on: an aggregate return requires an aggregate-
             * returning function, and identity when the fn type carries it. */
            const Type *rft = fexpr->type.as.fn.result_full_type;
            okc = (fr == TY_ADT) &&
                  (!rft || rft->kind != TY_ADT ||
                   rft->as.adt_.def == ret_type.as.adt_.def);
        } else {
            char want = tur_jit_ffi_class_for_kind(ret_type.kind, 1);
            char got  = tur_jit_ffi_class_for_kind(fr, 1);
            okc = (tur_jit_ffi_class_is_int(want) &&
                   tur_jit_ffi_class_is_int(got)) ||
                  (want == got) ||
                  ((want == 'f' || want == 'F') && (got == 'f' || got == 'F'));
            /* A :void C return discards whatever the closure produced, which
             * is the ordinary way to adapt a value-returning function to a
             * void callback slot -- allowed rather than an error. */
            if (want == 'v') okc = true;
        }
        if (!okc) {
            diag_emit(DIAG_ERROR, call->as.list.items[1]->span,
                      "callback-ptr: the signature returns %s but the function "
                      "returns %s", type_name(ret_type),
                      typekind_to_string(fr));
            return NULL;
        }
    }

    CallPtrSig *ps = (CallPtrSig *)arena_alloc(e->arena, sizeof(CallPtrSig));
    ps->return_type = ret_type;
    ps->param_types = param_types;
    ps->n_params    = n_params;
    ps->is_callback = true;

    /* The node's VALUE is the function pointer, so its type is ptr<void> --
     * not the signature's return type, which describes the callback's own
     * calls.  fn_expr carries the closure; there are no arguments. */
    Expr *out = expr_new(e->arena, EX_CALL, type_from_kind(TY_PTR_VOID),
                         call->span);
    out->as.call_.fn_binding = NULL;
    out->as.call_.fn_expr    = fexpr;
    out->as.call_.args       = NULL;
    out->as.call_.n_args     = 0;
    out->as.call_.ptr_sig    = ps;
    return out;
}

Expr *elab_dlopen(Elab *e, const Form *call) {
    g_needs_dlfcn = true;   /* emitted C needs <dlfcn.h> + -ldl */
    /* (dlopen path) - dynamic library open
     * For v1, we emit a call to dlopen with RTLD_LAZY flag
     * Returns ptr<void> (the library handle) */
    if (call->as.list.len != 2) {
        diag_emit(DIAG_ERROR, call->span,
                  "dlopen requires exactly 1 argument: (dlopen path)");
        return NULL;
    }
    if (e->unsafe_depth == 0) {
        diag_emit(DIAG_ERROR, call->span,
                  "dlopen requires an enclosing (unsafe ...) block");
        return NULL;
    }
    Expr *path = elab_form(e, call->as.list.items[1]);
    if (!path) return NULL;
    if (path->type.kind != TY_CSTR) {
        diag_emit(DIAG_ERROR, call->span,
                  "dlopen requires cstr as argument, got %s", type_name(path->type));
        return NULL;
    }
    /* Emit: dlopen(path, RTLD_LAZY) */
    const BuiltinSpec *spec = builtin_lookup(e->sym_dlopen, path->type, 1);
    if (!spec) {
        diag_emit(DIAG_ERROR, call->span, "dlopen: builtin lookup failed");
        return NULL;
    }
    Expr *out = expr_new(e->arena, EX_BUILTIN, spec->result_type, call->span);
    out->as.builtin.spec = spec;
    Expr **args = arena_alloc(e->arena, sizeof(Expr *));
    args[0] = path;
    out->as.builtin.args = args;
    out->as.builtin.n = 1;
    return out;
}

Expr *elab_dlsym(Elab *e, const Form *call) {
    g_needs_dlfcn = true;   /* emitted C needs <dlfcn.h> + -ldl */
    /* (dlsym handle symbol) - get symbol address from dynamic library
     * For v1, we emit a call to dlsym
     * Returns ptr<void> (the symbol address) */
    if (call->as.list.len != 3) {
        diag_emit(DIAG_ERROR, call->span,
                  "dlsym requires exactly 2 arguments: (dlsym handle symbol)");
        return NULL;
    }
    if (e->unsafe_depth == 0) {
        diag_emit(DIAG_ERROR, call->span,
                  "dlsym requires an enclosing (unsafe ...) block");
        return NULL;
    }
    Expr *handle = elab_form(e, call->as.list.items[1]);
    if (!handle) return NULL;
    Expr *symbol = elab_form(e, call->as.list.items[2]);
    if (!symbol) return NULL;
    if (handle->type.kind != TY_PTR_VOID) {
        diag_emit(DIAG_ERROR, call->span,
                  "dlsym requires ptr<void> as first argument, got %s",
                  type_name(handle->type));
        return NULL;
    }
    if (symbol->type.kind != TY_CSTR) {
        diag_emit(DIAG_ERROR, call->span,
                  "dlsym requires cstr as second argument, got %s",
                  type_name(symbol->type));
        return NULL;
    }
    const BuiltinSpec *spec = builtin_lookup(e->sym_dlsym, handle->type, 2);
    if (!spec) {
        diag_emit(DIAG_ERROR, call->span, "dlsym: builtin lookup failed");
        return NULL;
    }
    Expr *out = expr_new(e->arena, EX_BUILTIN, spec->result_type, call->span);
    out->as.builtin.spec = spec;
    Expr **args = arena_alloc(e->arena, 2 * sizeof(Expr *));
    args[0] = handle;
    args[1] = symbol;
    out->as.builtin.args = args;
    out->as.builtin.n = 2;
    return out;
}

Expr *elab_dlclose(Elab *e, const Form *call) {
    g_needs_dlfcn = true;   /* emitted C needs <dlfcn.h> + -ldl */
    /* (dlclose handle) - close dynamic library
     * For v1, we emit a call to dlclose
     * Returns int (0 on success, non-zero on error) */
    if (call->as.list.len != 2) {
        diag_emit(DIAG_ERROR, call->span,
                  "dlclose requires exactly 1 argument: (dlclose handle)");
        return NULL;
    }
    if (e->unsafe_depth == 0) {
        diag_emit(DIAG_ERROR, call->span,
                  "dlclose requires an enclosing (unsafe ...) block");
        return NULL;
    }
    Expr *handle = elab_form(e, call->as.list.items[1]);
    if (!handle) return NULL;
    if (handle->type.kind != TY_PTR_VOID) {
        diag_emit(DIAG_ERROR, call->span,
                  "dlclose requires ptr<void> as argument, got %s",
                  type_name(handle->type));
        return NULL;
    }
    const BuiltinSpec *spec = builtin_lookup(e->sym_dlclose, handle->type, 1);
    if (!spec) {
        diag_emit(DIAG_ERROR, call->span, "dlclose: builtin lookup failed");
        return NULL;
    }
    Expr *out = expr_new(e->arena, EX_BUILTIN, spec->result_type, call->span);
    out->as.builtin.spec = spec;
    Expr **args = arena_alloc(e->arena, sizeof(Expr *));
    args[0] = handle;
    out->as.builtin.args = args;
    out->as.builtin.n = 1;
    return out;
}

Expr *elab_unsafe(Elab *e, const Form *call) {
    /* (unsafe body...) — desugars to (handle (do body...) (Unsafe [] k) (resume k nil))
     * This wraps the body in a handler that discharges the Unsafe effect,
     * ensuring it cannot leak to the caller. */
    uint32_t n = call->as.list.len - 1;
    
    /* Phase U5: Linting for unsafe blocks */
    
    /* Check for nested unsafe blocks */
    if (g_unsafe_warn_nested && e->unsafe_depth > 0) {
        diag_emit(DIAG_WARNING, call->span,
                  "nested unsafe block: unsafe blocks should not be nested");
    }
    
    /* Check for ;;; SAFETY: comment */
    if (g_unsafe_require_safety) {
        /* For v1, we don't have access to the raw source to check for ;;; SAFETY: comments
         * So we just emit a warning that SAFETY comments are required */
        diag_emit(DIAG_WARNING, call->span,
                  "unsafe block requires a ;;; SAFETY: comment documenting why unsafe code is needed");
    }
    
    /* Track statistics */
    if (g_unsafe_stats_enabled) {
        g_unsafe_block_count++;
        g_unsafe_total_lines += n;
    }
    
    /* Check size threshold */
    if (g_unsafe_max_lines > 0 && n > g_unsafe_max_lines) {
        diag_emit(DIAG_WARNING, call->span,
                  "unsafe block has %u expressions (threshold: %u); consider breaking into smaller blocks",
                  n, g_unsafe_max_lines);
    }
    
    if (n == 0) {
        /* Empty unsafe block - warning */
        diag_emit(DIAG_WARNING, call->span,
                  "empty unsafe block has no effect");
        return e_nil(e, call->span);
    }

    /* Elaborate body expressions with increased unsafe_depth
     * (this allows unsafe-only operations inside the block) */
    Expr **items = (Expr **)arena_alloc(e->arena, n * sizeof(Expr *));
    e->unsafe_depth++;
    for (uint32_t i = 0; i < n; i++) {
        items[i] = elab_form(e, call->as.list.items[1 + i]);
        if (!items[i]) {
            e->unsafe_depth--;
            return NULL;
        }
    }
    e->unsafe_depth--;

    /* Wrap multiple expressions in a do block */
    Expr *body;
    if (n == 1) {
        body = items[0];
    } else {
        body = expr_new(e->arena, EX_DO, items[n - 1]->type, call->span);
        body->as.do_.items = items;
        body->as.do_.n = n;
    }

    /* Create handle expression with Unsafe handler
     * The handler just resumes with nil (Unsafe effect carries no value) */
    HandleCase *unsafe_case = arena_alloc(e->arena, sizeof(HandleCase));
    /* Arena memory is not zeroed; every field this site does not set
     * (resumable_payload, cont_kind, ...) must still hold a valid value for
     * any reader -- same rule as the memset'd HandleExpr below. */
    memset(unsafe_case, 0, sizeof(HandleCase));
    unsafe_case->effect_name = e->sym_effect_unsafe;
    unsafe_case->n_params = 0;
    unsafe_case->param_names = NULL;
    unsafe_case->param_bindings = NULL;
    
    /* Create k binding (dummy continuation for Unsafe which returns nil) */
    Binding *k_binding = binding_new(e, e->sym_k, TYPE_NIL, false, false, call->span);
    k_binding->type.copy_kind = CK_MOVE;
    unsafe_case->k_name = e->sym_k;
    unsafe_case->k_binding = k_binding;
    
    /* Create handler body: (resume k nil) */
    /* First create nil literal */
    Expr *nil_lit = expr_new(e->arena, EX_NIL_LIT, TYPE_NIL, call->span);
    
    /* Create k variable reference */
    Expr *k_var = expr_new(e->arena, EX_VAR, k_binding->type, call->span);
    k_var->as.var.binding = k_binding;
    
    /* Create resume expression directly (resume k nil) */
    ResumeExpr *resume = arena_alloc(e->arena, sizeof(ResumeExpr));
    resume->k = k_var;
    resume->value = nil_lit;
    
    Expr *resume_expr = expr_new(e->arena, EX_RESUME, TYPE_NIL, call->span);
    resume_expr->as.resume_.resume = resume;
    
    unsafe_case->body = resume_expr;

    /* Create handle expression */
    HandleExpr *handle = arena_alloc(e->arena, sizeof(HandleExpr));
    memset(handle, 0, sizeof(HandleExpr));   /* arena memory is not zeroed */
    handle->body = body;
    handle->cases = unsafe_case;
    handle->n_cases = 1;
    handle->shallow = false;   /* arena mem is not zeroed; Unsafe is a deep marker */
    /* Mark this as the pure-marker Unsafe discharge so emit can run the body
     * in place rather than through the int64-truncating fiber result slot. */
    handle->is_unsafe_marker = true;

    Expr *out = expr_new(e->arena, EX_HANDLE, body->type, call->span);
    out->as.handle_.handle = handle;
    return out;
}
