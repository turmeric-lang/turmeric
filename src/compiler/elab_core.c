/* elab_core.c -- Elab state, scope, free-var analysis, binding/move/linear-state helpers. */
#include "elab_internal.h"
#include "lang_dialects.h"   /* saffron-dynamic-surface-pass H6: lang_span_is_dynamic */
#include "mangle.h"
#include "cps.h"          /* cps_visit_children -- collect_free_vars' fallback */
#include <string.h>  /* memset for elab_init_state */

/* ---- shared file-scope state (declared in elab_internal.h) ---- */
bool elab_in_atomically = false;
bool elab_in_stm = false;

/* ---- file-local helper forward declarations ---- */
static bool type_is_concrete_for_disjoint(Type *t);

/* Helper to create a Type from TypeKind. */
Type type_from_kind(TypeKind k) {
    Type t;
    memset(&t, 0, sizeof(t));
    t.kind = k;
    t.copy_kind = typekind_default_copy_kind(k);
    t.hkt_kind = KIND_STAR;  /* Phase HKT-P6: all types are kind * in v1 */
    return t;
}

/* Helper to convert a type name string to TypeKind (Phase 19). */
TypeKind typekind_from_symbol(const char *name) {
    if (strcmp(name, "int") == 0) return TY_INT;
    if (strcmp(name, "int64") == 0) return TY_INT;       /* alias */
    if (strcmp(name, "bool") == 0) return TY_BOOL;
    if (strcmp(name, "float") == 0) return TY_FLOAT;
    if (strcmp(name, "float64") == 0) return TY_FLOAT;   /* alias */
    if (strcmp(name, "cstr") == 0) return TY_CSTR;
    if (strcmp(name, "nil") == 0) return TY_NIL;
    if (strcmp(name, "ptr-void") == 0 || strcmp(name, "ptr<void>") == 0) return TY_PTR_VOID;
    /* Bare `ptr` -- same as ptr<void>.  Surfaces via spaced annotation `: ptr`
     * (F_TYPE_ANN inner is the bare symbol `ptr`); fused `:ptr` is handled
     * directly by the F_KEYWORD ladder in elab_fns.c. */
    if (strcmp(name, "ptr") == 0) return TY_PTR_VOID;
    if (strcmp(name, "ref") == 0) return TY_REF;
    if (strcmp(name, "lref") == 0) return TY_LREF;
    if (strcmp(name, "rc") == 0) return TY_RC;
    if (strcmp(name, "weak") == 0) return TY_WEAK;
    if (strcmp(name, "exception") == 0) return TY_EXCEPTION;
    if (strcmp(name, "cont") == 0) return TY_CONT;
    /* Phase N: fixed-width numeric types */
    if (strcmp(name, "int8")   == 0) return TY_INT8;
    if (strcmp(name, "int16")  == 0) return TY_INT16;
    if (strcmp(name, "int32")  == 0) return TY_INT32;
    if (strcmp(name, "int64")  == 0) return TY_INT64;
    if (strcmp(name, "uint8")  == 0) return TY_UINT8;
    if (strcmp(name, "uint16") == 0) return TY_UINT16;
    if (strcmp(name, "uint32") == 0) return TY_UINT32;
    if (strcmp(name, "uint64") == 0) return TY_UINT64;
    if (strcmp(name, "float32") == 0) return TY_FLOAT32;
    if (strcmp(name, "float64") == 0) return TY_FLOAT64;
    /* c-fn-ptr-element-and-size-precision-gap fix: pointer-width integer
     * carriers for C FFI.  `usize`/`size` are size_t-spelled (carrier uint64),
     * `isize`/`ssize` are ptrdiff_t-spelled (carrier int64).  The size_t /
     * ptrdiff_t spelling is attached as a CNumSpelling at the Type-construction
     * site (see typekind_size_spelling); here we only resolve the carrier kind. */
    if (strcmp(name, "usize") == 0) return TY_UINT64;
    if (strcmp(name, "size")  == 0) return TY_UINT64;
    if (strcmp(name, "isize") == 0) return TY_INT64;
    if (strcmp(name, "ssize") == 0) return TY_INT64;
    /* Short-form sized aliases */
    if (strcmp(name, "i8")  == 0) return TY_INT8;
    if (strcmp(name, "i16") == 0) return TY_INT16;
    if (strcmp(name, "i32") == 0) return TY_INT32;
    if (strcmp(name, "i64") == 0) return TY_INT64;
    if (strcmp(name, "u8")  == 0) return TY_UINT8;
    if (strcmp(name, "u16") == 0) return TY_UINT16;
    if (strcmp(name, "u32") == 0) return TY_UINT32;
    if (strcmp(name, "u64") == 0) return TY_UINT64;
    if (strcmp(name, "f32") == 0) return TY_FLOAT32;
    if (strcmp(name, "f64") == 0) return TY_FLOAT64;
    /* IT4: Top type — available with -Xunion-types or -Xintersection-types */
    if (strcmp(name, "any") == 0) return TY_ANY;
    /* SYM0: interned runtime symbol type (-Xsymbols) */
    if (strcmp(name, "Sym") == 0) return TY_SYM;
    /* Stage 1 (macro-system-direction-plan): compile-time syntax object.
     * Interpreter/macro-time only; a compiled runtime value never has it. */
    if (strcmp(name, "Syntax") == 0) return TY_SYNTAX;
    return TY_UNKNOWN;
}

/* Forward-declaration param scan for the defmodule / top-level pre-pass.
 *
 * Mirrors the real param parse closely enough that a sibling forward-reference
 * (a defn whose body calls another defn declared later in the same module)
 * sees the right arity and the right scalar argument kinds:
 *
 *  - `^`-prefixed markers (^fat, ^mut, ^borrow, ^linear, ...) annotate the
 *    *next* parameter and are NOT slots -- counting them over-states the arity,
 *    which made a saturated call look under-saturated and synthesised a bogus
 *    extra-arg partial-application wrapper that failed to C-compile
 *    (docs/archive/history/pap-defmodule-fat-fn-too-many-args.md);
 *  - a fused `:T` (F_KEYWORD) or spaced `: T` (F_TYPE_ANN) annotates the most
 *    recently opened slot;
 *  - scalar primitive annotations (float/bool/cstr/sized numerics/...) are
 *    recorded so a saturated forward-ref call type-checks against the real
 *    kind instead of the TY_INT placeholder; compound/unknown types (fn,
 *    structs, ptr, type applications) stay TY_INT and are resolved later by
 *    the HRT5 early-update once the callee's body is elaborated.
 *
 * Fills arg_kinds[0..arity) (capacity MAX_FN_ARITY) and returns the arity. */
/* r7rs-lang-plan R7: does this parameter vector declare a rest parameter
 * `& name [: T]` whose element type a bare name settles -- `any` or a scalar?
 * Then *rest_kind is that kind and the forward decl can carry the variadic
 * shape; a compound rest type (a struct, a type application) returns false
 * and the pre-pass keeps its old treatment, since resolving it needs the
 * elaborator. */
bool fwd_decl_scan_variadic(const Form *params_f, TypeKind *rest_kind) {
    if (!params_f || params_f->tag != F_VEC) return false;
    uint32_t n = params_f->as.list.len;
    for (uint32_t i = 0; i < n; i++) {
        const Form *p = params_f->as.list.items[i];
        if (p->tag != F_SYM || strcmp(p->as.sym->name, "&") != 0) continue;
        if (i + 1 >= n || params_f->as.list.items[i + 1]->tag != F_SYM) return false;
        TypeKind k = lang_span_is_dynamic(params_f->span) ? TY_ANY : TY_INT;
        if (i + 2 < n) {
            const Form *t = params_f->as.list.items[i + 2];
            if (t->tag == F_TYPE_ANN && t->as.list.len >= 1) t = t->as.list.items[0];
            if (t->tag == F_SYM || t->tag == F_KEYWORD) {
                k = typekind_from_symbol(t->as.sym->name);
                if (k == TY_UNKNOWN) return false;
                if (!(k == TY_ANY || typekind_is_numeric(k) || k == TY_BOOL || k == TY_CSTR))
                    return false;
            } else {
                return false;
            }
        }
        *rest_kind = k;
        return true;
    }
    return false;
}

/* r7rs-lang-plan R7: give a forward-declared fn type the rest shape its
 * definition will have -- the variadic flag, the rest element kind, and for an
 * `any` rest the `(Cons any)` chain as the rest slot's type (elab_fns.c builds
 * the same one), so a call elaborated before the definition packs its surplus
 * arguments and passes the chain exactly as a later call would. */
void fwd_decl_apply_variadic(Elab *e, Arena *arena, Type *fn_type, const Form *params_f) {
    TypeKind rk;
    if (!fn_type || fn_type->kind != TY_FN || !fwd_decl_scan_variadic(params_f, &rk)) return;
    fn_type->as.fn.is_variadic = true;
    fn_type->as.fn.rest_kind = rk;
    uint32_t n = fn_type->as.fn.arity;
    if (n == 0 || rk != TY_ANY) {
        if (n > 0 && fn_type->as.fn.arg_kinds) fn_type->as.fn.arg_kinds[n - 1] = (uint8_t)TY_INT;
        return;
    }
    Type *cons_t = elab_lookup_type_by_name(e, intern_cstr(e->st, "Cons"));
    if (!(cons_t && cons_t->kind == TY_ADT && cons_t->as.adt_.def &&
          cons_t->as.adt_.def->n_type_params == 1)) return;
    Type head = *cons_t;
    head.hkt_kind = kind_for_arity(1);
    Type any_t; memset(&any_t, 0, sizeof any_t);
    any_t.kind = TY_ANY;
    Type *rt = (Type *)arena_alloc(arena, sizeof(Type));
    *rt = type_app(arena, head, any_t, params_f->span);
    if (fn_type->as.fn.arg_kinds) fn_type->as.fn.arg_kinds[n - 1] = (uint8_t)TY_APP;
    if (!fn_type->as.fn.arg_full_types) {
        fn_type->as.fn.arg_full_types = (Type **)arena_alloc(arena, n * sizeof(Type *));
        for (uint32_t i = 0; i < n; i++) fn_type->as.fn.arg_full_types[i] = NULL;
    }
    fn_type->as.fn.arg_full_types[n - 1] = rt;
}

uint32_t fwd_decl_scan_params(Arena *arena, const Form *params_f, TypeKind **out_arg_kinds) {
    *out_arg_kinds = NULL;
    uint32_t arity = 0;
    if (!params_f || params_f->tag != F_VEC) return 0;
    /* R7: a simple rest parameter's `&` is not a slot (the elaborated fn type
     * counts the rest parameter itself as the last slot). */
    TypeKind fwd_rest_kind;
    bool skip_amp = fwd_decl_scan_variadic(params_f, &fwd_rest_kind);
    /* Scratch is sized to the parameter-vector length -- a safe upper bound on
     * the parameter count -- so the forward-declared arity is not capped
     * (arbitrary-fn-arity: no MAX_FN_ARITY ceiling). */
    uint32_t cap = params_f->as.list.len ? params_f->as.list.len : 1;
    TypeKind *arg_kinds = (TypeKind *)arena_alloc(arena, cap * sizeof(TypeKind));
    /* saffron-dynamic-surface-pass H6: in a Saffron file an UNANNOTATED
     * parameter is `any` (D3), and this forward decl is what a caller
     * elaborated earlier than the callee checks against -- with the `int`
     * placeholder, `(later 7.25 "s")` above `(defn later [a b] ...)` was
     * "expected int, got float", and mutual recursion emitted a call whose C
     * signature the definition did not have.  A slot that an annotation
     * follows is handled below exactly as before: the annotation either
     * commits a scalar kind or restores the compound placeholder. */
    bool saffron = lang_span_is_dynamic(params_f->span);
    for (uint32_t pi = 0; pi < params_f->as.list.len; pi++) {
        const Form *p = params_f->as.list.items[pi];
        /* `^`-prefixed substructural / fat / mut markers annotate the next
         * param; they are not slots. */
        if (p->tag == F_SYM && p->as.sym->name && p->as.sym->name[0] == '^')
            continue;
        if (skip_amp && p->tag == F_SYM && strcmp(p->as.sym->name, "&") == 0)
            continue;
        if (p->tag == F_KEYWORD || p->tag == F_TYPE_ANN) {
            /* Type annotation for the most recent slot. */
            if (arity == 0) continue;
            const Form *t = p;
            if (p->tag == F_TYPE_ANN && p->as.list.len >= 1)
                t = p->as.list.items[0];
            if (t->tag == F_SYM || t->tag == F_KEYWORD) {
                TypeKind k = typekind_from_symbol(t->as.sym->name);
                /* Only commit kinds a bare type NAME determines completely;
                 * leave compound/unknown as the TY_INT placeholder (matches
                 * prior pre-pass behavior).
                 *
                 * any-coercion-not-driven-by-expected-type: `any` belongs on
                 * that list.  It is spelled as a bare symbol and is fully
                 * determined by it, exactly like bool/cstr/nil -- it is only
                 * "compound" in its representation, which this pre-pass does not
                 * care about.  Leaving it off meant a letrec (and therefore a
                 * NAMED LET, which desugars to one) forward-declared an `: any`
                 * parameter as `int`, so the recursive self-call reported
                 * "function 'loop' arg 2: expected int, got any" -- the annotation
                 * silently not taking effect.  `cstr` and `float` accumulators
                 * worked, which is what localised it here. */
                if (k != TY_UNKNOWN && k != TY_INT &&
                    (typekind_is_numeric(k) || k == TY_BOOL || k == TY_CSTR ||
                     k == TY_NIL || k == TY_PTR_VOID || k == TY_SYM ||
                     k == TY_ANY)) {
                    arg_kinds[arity - 1] = k;
                } else {
                    arg_kinds[arity - 1] = TY_INT;   /* compound: the placeholder */
                }
            } else {
                arg_kinds[arity - 1] = TY_INT;       /* compound: the placeholder */
            }
            continue;
        }
        /* Otherwise this form opens a new parameter slot. */
        arg_kinds[arity] = saffron ? TY_ANY : TY_INT;
        arity++;
    }
    *out_arg_kinds = arg_kinds;
    return arity;
}

/* Phase N: returns true if kind is any numeric type */
bool typekind_is_numeric(TypeKind k) {
    switch (k) {
        case TY_INT: case TY_FLOAT:
        case TY_INT8: case TY_INT16: case TY_INT32: case TY_INT64:
        case TY_UINT8: case TY_UINT16: case TY_UINT32: case TY_UINT64:
        case TY_FLOAT32: case TY_FLOAT64:
            return true;
        default:
            return false;
    }
}

/* Helper to get the compile-time size of a type in bytes (0 = unknown). */
int type_size_bytes(TypeKind kind) {
    switch (kind) {
        case TY_BOOL:     return 1;   /* bool → 1 byte in C */
        case TY_INT:      return 8;   /* int64_t → 8 bytes */
        case TY_FLOAT:    return 8;   /* double → 8 bytes */
        case TY_CSTR:     return 8;   /* const char* → pointer size */
        case TY_PTR_VOID: return 8;   /* void* → pointer size */
        case TY_NIL:      return 0;   /* unit / void — no size */
        case TY_REF:      return 8;   /* heap pointer */
        case TY_RC:       return 8;   /* rc pointer */
        case TY_WEAK:     return 8;   /* weak pointer */
        /* Phase N: fixed-width numeric types */
        case TY_INT8:   return 1;
        case TY_INT16:  return 2;
        case TY_INT32:  return 4;
        case TY_INT64:  return 8;
        case TY_UINT8:  return 1;
        case TY_UINT16: return 2;
        case TY_UINT32: return 4;
        case TY_UINT64: return 8;
        case TY_FLOAT32: return 4;
        case TY_FLOAT64: return 8;
        case TY_SYM:      return 8;   /* SYM0: pointer into static .rodata */
        default:          return 0;   /* unknown / composite */
    }
}

/* IT3: Return true if t is a concrete (closed) type that can participate in
 * provable-disjointness checks.  TY_STRUCT/TY_ADT with a NULL def are type
 * variables and are therefore open -- not concrete. */
static bool type_is_concrete_for_disjoint(Type *t) {
    if (!t) return false;
    switch (t->kind) {
        case TY_INT: case TY_FLOAT: case TY_BOOL: case TY_CSTR:
        case TY_NIL: case TY_PTR_VOID:
        case TY_INT8: case TY_INT16: case TY_INT32: case TY_INT64:
        case TY_UINT8: case TY_UINT16: case TY_UINT32: case TY_UINT64:
        case TY_FLOAT32: case TY_FLOAT64:
            return true;
        case TY_ADT:
            return t->as.adt_.def != NULL;
        default:
            return false;
    }
}

/* IT3: Return true if TypeKind k (for a primitive) is a concrete base kind.
 * Used from the intersection-member-mismatch check where we only have a kind. */
bool typekind_is_concrete_for_disjoint(TypeKind k) {
    switch (k) {
        case TY_INT: case TY_FLOAT: case TY_BOOL: case TY_CSTR:
        case TY_NIL: case TY_PTR_VOID:
        case TY_INT8: case TY_INT16: case TY_INT32: case TY_INT64:
        case TY_UINT8: case TY_UINT16: case TY_UINT32: case TY_UINT64:
        case TY_FLOAT32: case TY_FLOAT64:
        /* For ADT we cannot check concreteness without the full Type* here;
         * the caller should use type_is_concrete_for_disjoint() instead when possible. */
        case TY_ADT:
            return true;
        default:
            return false;
    }
}

/* IT3: Check whether two concrete types are provably disjoint.
 * Returns true if it is impossible for a single value to satisfy both types.
 * Only catches the obvious cases (distinct primitives, distinct named structs,
 * distinct named ADTs). Returns false for any pair involving a typeclass, forall,
 * type variable, or other open type. */
bool types_provably_disjoint(Type *a, Type *b) {
    if (!a || !b) return false;
    /* A type is never disjoint from itself */
    if (type_eq(*a, *b)) return false;
    /* Only check pairs where both sides are fully concrete */
    if (!type_is_concrete_for_disjoint(a) ||
        !type_is_concrete_for_disjoint(b)) return false;
    TypeKind ka = a->kind, kb = b->kind;
    /* Two different primitive kinds are disjoint */
    if (ka != kb) return true;
    /* Same kind -- check identity for nominal types */
    if (ka == TY_ADT) {
        AdtDef *da = a->as.adt_.def;
        AdtDef *db = b->as.adt_.def;
        if (da && db && da->name && db->name && da->name != db->name) return true;
    }
    return false;
}

void scope_init(Scope *s, Scope *parent) {
    s->parent = parent;
    s->bindings = NULL;
    s->n = 0;
    s->cap = 0;
    s->borrows = NULL;
    s->idx = NULL;
    s->idx_cap = 0;
    s->idx_n = 0;
    s->prev = NULL;
}

void scope_free(Scope *s) {
    free(s->bindings);
    /* Free borrow list */
    ScopeBorrow *b = s->borrows;
    while (b) {
        ScopeBorrow *next = b->next;
        free(b);
        b = next;
    }
    s->bindings = NULL;
    s->n = s->cap = 0;
    s->borrows = NULL;
    free(s->idx);
    s->idx = NULL;
    s->idx_cap = s->idx_n = 0;
    free(s->prev);
    s->prev = NULL;
}

/* Phase 12: Check if a binding has an active borrow that conflicts with the requested kind */
bool scope_borrow_conflicts(const Scope *s, Binding *binding, BorrowKind kind) {
    for (const Scope *cur = s; cur; cur = cur->parent) {
        for (ScopeBorrow *b = cur->borrows; b; b = b->next) {
            if (b->binding == binding) {
                /* Same binding is borrowed - check for conflict */
                if (kind == BK_MUT) {
                    /* &mut T cannot coexist with any other borrow of T */
                    return true;
                }
                if (b->kind == BK_MUT) {
                    /* Existing &mut T conflicts with new &T */
                    return true;
                }
                /* Both are &T - allowed (multiple immutable borrows) */
            }
        }
    }
    return false;
}

/* Phase 12: Add a borrow to the current scope */
bool scope_add_borrow(Scope *s, Binding *binding, BorrowKind kind, Span span) {
    if (scope_borrow_conflicts(s, binding, kind)) {
        /* Conflict - emit error */
        if (kind == BK_MUT) {
            diag_emit(DIAG_ERROR, span,
                      "cannot borrow `%s` as mutable while it is already borrowed",
                      binding->name->name);
        } else {
            diag_emit(DIAG_ERROR, span,
                      "cannot borrow `%s` as immutable while it is mutably borrowed",
                      binding->name->name);
        }
        return false;
    }
    
    /* Add to this scope's borrow list */
    ScopeBorrow *b = (ScopeBorrow *)malloc(sizeof(ScopeBorrow));
    if (!b) { fprintf(stderr, "tur: oom\n"); abort(); }
    b->binding = binding;
    b->kind = kind;
    b->next = s->borrows;
    s->borrows = b;
    return true;
}

/* r7rs-conformance-program-emits-megabytes-of-c: scope_lookup scanned every
 * binding of every scope it passed, newest first -- and the global scope holds
 * every stdlib and program definition, so each lookup of a global name (or of
 * a special form, which misses) cost thousands of compares: 40% of `emit-c` on
 * a large `#lang r7rs` program once the passes after it were fixed.  A scope
 * past SCOPE_INDEX_MIN bindings keeps a hash from name to its NEWEST binding,
 * which is the one the newest-first scan returns.  Names are interned, so the
 * key is the Symbol pointer. */
#define SCOPE_INDEX_MIN 32

/* Mixed at a fixed 64-bit width: a `uintptr_t` is 32 bits on wasm32, where
 * `>> 33` is an over-wide shift (UB) -- the Try Turmeric worker hung in
 * turi_wasm_init on it.  On LP64 this is bit-identical (see promo_hash). */
static uint32_t scope_idx_hash(const Symbol *name) {
    uint64_t x = (uint64_t)(uintptr_t)name;
    x ^= x >> 33; x *= 0xff51afd7ed558ccdULL; x ^= x >> 33;
    return (uint32_t)x;
}

/* Point `name`'s slot at binding `i` (newer than any it held). */
static void scope_idx_put(Scope *s, uint32_t i) {
    const Symbol *name = s->bindings[i]->name;
    uint32_t m = s->idx_cap - 1, h = scope_idx_hash(name) & m;
    while (s->idx[h]) {
        if (s->bindings[s->idx[h] - 1]->name == name) {
            s->prev[i] = s->idx[h];
            s->idx[h] = i + 1;
            return;
        }
        h = (h + 1) & m;
    }
    s->prev[i] = 0;
    s->idx[h] = i + 1;
    s->idx_n++;
}

static void scope_idx_rebuild(Scope *s, uint32_t cap) {
    uint32_t *ni = (uint32_t *)calloc(cap, sizeof *ni);
    if (!ni) { fprintf(stderr, "tur: oom\n"); abort(); }
    free(s->idx);
    s->idx = ni;
    s->idx_cap = cap;
    uint32_t *np = (uint32_t *)realloc(s->prev, (s->cap ? s->cap : 1) * sizeof *np);
    if (!np) { fprintf(stderr, "tur: oom\n"); abort(); }
    s->prev = np;
    s->idx_n = 0;
    for (uint32_t i = 0; i < s->n; i++) scope_idx_put(s, i);   /* oldest first */
}

void scope_add(Scope *s, Binding *b) {
    if (s->n == s->cap) {
        s->cap = s->cap ? s->cap * 2 : 4;
        s->bindings = (Binding **)realloc(s->bindings, s->cap * sizeof(Binding *));
        if (!s->bindings) { fprintf(stderr, "tur: oom\n"); abort(); }
        if (s->prev) {
            s->prev = (uint32_t *)realloc(s->prev, s->cap * sizeof *s->prev);
            if (!s->prev) { fprintf(stderr, "tur: oom\n"); abort(); }
        }
    }
    s->bindings[s->n++] = b;
    if (s->idx) {
        if ((s->idx_n + 1) * 2 > s->idx_cap) scope_idx_rebuild(s, s->idx_cap * 2);
        else scope_idx_put(s, s->n - 1);
    } else if (s->n >= SCOPE_INDEX_MIN) {
        scope_idx_rebuild(s, 128);
    }
}

/* `index + 1` of the newest binding of `name` in indexed scope `s`, 0 if it
 * has none.  Older ones follow s->prev. */
uint32_t scope_idx_newest(const Scope *s, const Symbol *name) {
    uint32_t m = s->idx_cap - 1, h = scope_idx_hash(name) & m;
    while (s->idx[h]) {
        if (s->bindings[s->idx[h] - 1]->name == name) return s->idx[h];
        h = (h + 1) & m;
    }
    return 0;
}

Binding *scope_lookup(Scope *s, const Symbol *name) {
    for (Scope *cur = s; cur; cur = cur->parent) {
        if (cur->idx) {
            uint32_t m = cur->idx_cap - 1, h = scope_idx_hash(name) & m;
            while (cur->idx[h]) {
                Binding *b = cur->bindings[cur->idx[h] - 1];
                if (b->name == name) return b;
                h = (h + 1) & m;
            }
            continue;   /* no binding of that name in this scope */
        }
        for (uint32_t i = cur->n; i > 0; i--) {
            Binding *b = cur->bindings[i - 1];
            if (b->name == name) return b;
        }
    }
    return NULL;
}

/* Edge 1 + letrec-sibling-closure-not-captured + letrec-mutual-recursion-
 * between-capturing-closures: is a call from a letrec init's own top-level
 * body to group member `b` recursion (left to the recursion machinery) rather
 * than a capture?  Only for the member being elaborated itself (the S5 env-ptr
 * self-call) or a member that will be captureless (lifted to a global).  A
 * sibling already elaborated to a closure, or one the pre-scan predicts will
 * be one, must be carried in the env. */
static bool letrec_member_is_recursion(const Binding *b) {
    if (b->closure_fn_binding) return false;
    if (b->letrec_predicted_closure && !b->letrec_elaborating) return false;
    return true;
}

/* collect_free_vars' two work stacks.  They were fixed at 256 entries with no
 * bound check, so a lambda whose body was a `do` of 300 forms overflowed the
 * heap inside the compiler. */
typedef struct FvStack { const Expr ***st; int *sp; int *cap; } FvStack;

static void fv_push(const Expr ***st, int *sp, int *cap, const Expr *x) {
    if (*sp == *cap) {
        *cap *= 2;
        const Expr **p = (const Expr **)realloc((void *)*st, (size_t)*cap * sizeof(const Expr *));
        if (!p) { fprintf(stderr, "tur: oom\n"); abort(); }
        *st = p;
    }
    (*st)[(*sp)++] = x;
}

static bool fv_push_visit(const Expr *c, void *ud) {
    FvStack *s = (FvStack *)ud;
    fv_push(s->st, s->sp, s->cap, c);
    return false;
}

static void fv_add_local(Binding ***defs, uint32_t *n, uint32_t *cap, Binding *b) {
    if (*n >= *cap) {
        *cap = *cap ? *cap * 2 : 8;
        Binding **p = (Binding **)realloc(*defs, *cap * sizeof(Binding *));
        if (!p) { fprintf(stderr, "tur: oom\n"); abort(); }
        *defs = p;
    }
    (*defs)[(*n)++] = b;
}

/* Phase 3: Collect free variables in an expression that are not in the given
 * param bindings. Returns a malloc'd list of captured Binding pointers. */
Binding **collect_free_vars(const Expr *e, Binding **params, uint8_t n_params,
                                  Binding **self_exclude, uint32_t n_self_exclude,
                                  uint32_t *n_out) {
    /* Pre-pass: collect all bindings introduced by `let` forms anywhere within
     * this expression.  These are locally defined — they must never be treated
     * as free variables even when referenced inside a nested `let` body.
     * Binding comparison is by pointer identity, so two `let` forms that bind
     * the same name but produce distinct Binding* objects don't interfere. */
    Binding **local_defs = NULL;
    uint32_t  n_local = 0, cap_local = 0;
    {
        int ls_cap = 256;
        const Expr **ls = (const Expr **)malloc((size_t)ls_cap * sizeof(const Expr *));
        if (!ls) { fprintf(stderr, "tur: oom\n"); abort(); }
        int lsp = 0;
        FvStack ls_ctx = { &ls, &lsp, &ls_cap };
        fv_push(&ls, &lsp, &ls_cap, e);
        while (lsp > 0) {
            const Expr *cur = ls[--lsp];
            if (!cur) continue;
            switch (cur->kind) {
                case EX_LET:
                case EX_LETREC:
                    for (uint32_t i = 0; i < cur->as.let_.n; i++) {
                        Binding *b = cur->as.let_.bindings[i].binding;
                        if (b) {
                            if (n_local >= cap_local) {
                                cap_local = cap_local ? cap_local * 2 : 8;
                                local_defs = (Binding **)realloc(local_defs,
                                    cap_local * sizeof(Binding *));
                            }
                            local_defs[n_local++] = b;
                        }
                        fv_push(&ls, &lsp, &ls_cap, cur->as.let_.bindings[i].init);
                    }
                    fv_push(&ls, &lsp, &ls_cap, cur->as.let_.body);
                    break;
                case EX_IF:
                    if (cur->as.if_.else_or_null) fv_push(&ls, &lsp, &ls_cap, cur->as.if_.else_or_null);
                    fv_push(&ls, &lsp, &ls_cap, cur->as.if_.then_);
                    fv_push(&ls, &lsp, &ls_cap, cur->as.if_.cond);
                    break;
                case EX_DO:
                    for (uint32_t i = cur->as.do_.n; i > 0; i--)
                        fv_push(&ls, &lsp, &ls_cap, cur->as.do_.items[i-1]);
                    break;
                case EX_WHILE:
                    fv_push(&ls, &lsp, &ls_cap, cur->as.while_.body);
                    fv_push(&ls, &lsp, &ls_cap, cur->as.while_.cond);
                    break;
                case EX_SET:   fv_push(&ls, &lsp, &ls_cap, cur->as.set_.value);      break;
                case EX_DEF:   fv_push(&ls, &lsp, &ls_cap, cur->as.def_.init);       break;
                case EX_BUILTIN:
                    for (uint32_t i = cur->as.builtin.n; i > 0; i--)
                        fv_push(&ls, &lsp, &ls_cap, cur->as.builtin.args[i-1]);
                    break;
                case EX_CALL:
                    for (uint32_t i = cur->as.call_.n_args; i > 0; i--)
                        fv_push(&ls, &lsp, &ls_cap, cur->as.call_.args[i-1]);
                    break;
                case EX_FN_DEF:  fv_push(&ls, &lsp, &ls_cap, cur->as.fn_def_.fn->body); break;
                case EX_RETURN:
                    if (cur->as.return_.value) fv_push(&ls, &lsp, &ls_cap, cur->as.return_.value);
                    break;
                case EX_PANIC:   fv_push(&ls, &lsp, &ls_cap, cur->as.panic_.payload);   break;
                case EX_RC_DROP: fv_push(&ls, &lsp, &ls_cap, cur->as.rc_drop_.expr);    break;
                case EX_DEFER:   fv_push(&ls, &lsp, &ls_cap, cur->as.defer_.body);      break;
                case EX_RC_OF:   fv_push(&ls, &lsp, &ls_cap, cur->as.rc_of_.expr);      break;
                case EX_GET_FIELD: fv_push(&ls, &lsp, &ls_cap, cur->as.get_field_.struct_expr); break;
                case EX_SET_FIELD:
                    fv_push(&ls, &lsp, &ls_cap, cur->as.set_field_.value);
                    fv_push(&ls, &lsp, &ls_cap, cur->as.set_field_.receiver);
                    break;
                /* Mirror the main traversal's `any` widen / reader arms, so a
                 * local bound inside one of their operands is registered as
                 * locally defined rather than reported free. */
                case EX_UNION_INJECT: fv_push(&ls, &lsp, &ls_cap, cur->as.union_inject_.value);  break;
                case EX_ANY_TYPE_OF:  fv_push(&ls, &lsp, &ls_cap, cur->as.any_type_of_.value);   break;
                case EX_ANY_IS:       fv_push(&ls, &lsp, &ls_cap, cur->as.any_is_.value);        break;
                case EX_ANY_CAST:     fv_push(&ls, &lsp, &ls_cap, cur->as.any_cast_.value);      break;
                /* saffron-dynamic-surface-pass H1: the Saffron operator layer's
                 * own nodes, mirrored in the main traversal below. */
                case EX_DYN_OP:
                    for (uint32_t i = cur->as.dyn_op_.n_args; i > 0; i--)
                        fv_push(&ls, &lsp, &ls_cap, cur->as.dyn_op_.args[i-1]);
                    break;
                case EX_DYN_CALL:
                    for (uint32_t i = cur->as.dyn_call_.n_args; i > 0; i--)
                        fv_push(&ls, &lsp, &ls_cap, cur->as.dyn_call_.args[i-1]);
                    fv_push(&ls, &lsp, &ls_cap, cur->as.dyn_call_.fn);
                    break;
                case EX_DYN_FIELD:  fv_push(&ls, &lsp, &ls_cap, cur->as.dyn_field_.obj);  break;
                case EX_DYN_METHOD:
                    for (uint32_t i = cur->as.dyn_method_.n_args; i > 0; i--)
                        fv_push(&ls, &lsp, &ls_cap, cur->as.dyn_method_.args[i-1]);
                    fv_push(&ls, &lsp, &ls_cap, cur->as.dyn_method_.obj);
                    break;
                case EX_MAKE_STRUCT:
                    for (uint32_t i = cur->as.make_struct_.n_fields; i > 0; i--)
                        fv_push(&ls, &lsp, &ls_cap, cur->as.make_struct_.field_values[i-1]);
                    break;
                case EX_SET_LIT:
                    for (uint32_t i = cur->as.set_lit_.n; i > 0; i--)
                        fv_push(&ls, &lsp, &ls_cap, cur->as.set_lit_.items[i-1]);
                    break;
                case EX_CONS_LIST:
                    for (uint32_t i = cur->as.cons_list_.n; i > 0; i--)
                        fv_push(&ls, &lsp, &ls_cap, cur->as.cons_list_.items[i-1]);
                    break;
                /* (:: expr T) is type-erased; descend into the inner expr so any
                 * `let` bindings under an ascription are still collected. */
                case EX_ASCRIBE:
                    if (cur->as.ascribe_.inner) fv_push(&ls, &lsp, &ls_cap, cur->as.ascribe_.inner);
                    break;
                /* Conversion shims: descend so a `let` nested under one is still
                 * registered as locally defined (mirrors the main traversal). */
                case EX_REINTERPRET:
                    if (cur->as.reinterpret_.expr) fv_push(&ls, &lsp, &ls_cap, cur->as.reinterpret_.expr);
                    break;
                case EX_CAST:
                    if (cur->as.cast_.expr) fv_push(&ls, &lsp, &ls_cap, cur->as.cast_.expr);
                    break;
                case EX_FN_TO_FAT:
                    if (cur->as.fn_to_fat_.inner) fv_push(&ls, &lsp, &ls_cap, cur->as.fn_to_fat_.inner);
                    break;
                case EX_POLY_TO_FAT:
                    if (cur->as.poly_to_fat_.inner) fv_push(&ls, &lsp, &ls_cap, cur->as.poly_to_fat_.inner);
                    break;
                case EX_POLY_WRAP:
                    if (cur->as.poly_wrap_.inner) fv_push(&ls, &lsp, &ls_cap, cur->as.poly_wrap_.inner);
                    break;
                /* GF1: Generator body -- traverse to find local defs */
                case EX_GEN:
                    if (cur->as.gen_.def && cur->as.gen_.def->body)
                        fv_push(&ls, &lsp, &ls_cap, cur->as.gen_.def->body);
                    break;
                case EX_YIELD:
                    if (cur->as.yield_.value) fv_push(&ls, &lsp, &ls_cap, cur->as.yield_.value);
                    break;
                case EX_GEN_NEXT:
                    if (cur->as.gen_next_.gen_expr) fv_push(&ls, &lsp, &ls_cap, cur->as.gen_next_.gen_expr);
                    break;
                case EX_GEN_UNWRAP:
                    if (cur->as.gen_unwrap_.ptr_expr) fv_push(&ls, &lsp, &ls_cap, cur->as.gen_unwrap_.ptr_expr);
                    break;
                case EX_GEN_DONE:
                    if (cur->as.gen_done_.gen_expr) fv_push(&ls, &lsp, &ls_cap, cur->as.gen_done_.gen_expr);
                    break;
                case EX_HANDLE: {
                    /* Handle case params (effect args + k) are locally defined.
                     * Register them so collect_free_vars doesn't treat them as
                     * free variables when the handler body is inside an outer
                     * closure (e.g. an async block). */
                    HandleExpr *handle = cur->as.handle_.handle;
                    fv_push(&ls, &lsp, &ls_cap, handle->body);
                    for (uint8_t ci = 0; ci < handle->n_cases; ci++) {
                        HandleCase *hc = &handle->cases[ci];
                        /* Register k binding as local */
                        if (hc->k_binding) {
                            if (n_local >= cap_local) {
                                cap_local = cap_local ? cap_local * 2 : 8;
                                local_defs = (Binding **)realloc(local_defs,
                                    cap_local * sizeof(Binding *));
                            }
                            local_defs[n_local++] = hc->k_binding;
                        }
                        /* Register effect param bindings as local */
                        for (uint32_t pi = 0; pi < hc->n_params; pi++) {
                            if (hc->param_bindings && hc->param_bindings[pi]) {
                                if (n_local >= cap_local) {
                                    cap_local = cap_local ? cap_local * 2 : 8;
                                    local_defs = (Binding **)realloc(local_defs,
                                        cap_local * sizeof(Binding *));
                                }
                                local_defs[n_local++] = hc->param_bindings[pi];
                            }
                        }
                        fv_push(&ls, &lsp, &ls_cap, hc->body);
                    }
                    break;
                }
                case EX_MATCH: {
                    fv_push(&ls, &lsp, &ls_cap, cur->as.match_.scrutinee);
                    for (uint32_t ai = 0; ai < cur->as.match_.n_arms; ai++) {
                        MatchArm *arm = &cur->as.match_.arms[ai];
                        for (uint32_t bi = 0; bi < arm->pattern.n_bindings; bi++) {
                            if (arm->pattern.bindings && arm->pattern.bindings[bi]) {
                                if (n_local >= cap_local) {
                                    cap_local = cap_local ? cap_local * 2 : 8;
                                    local_defs = (Binding **)realloc(local_defs,
                                        cap_local * sizeof(Binding *));
                                }
                                local_defs[n_local++] = arm->pattern.bindings[bi];
                            }
                        }
                        if (arm->guard) fv_push(&ls, &lsp, &ls_cap, arm->guard);
                        if (arm->body)  fv_push(&ls, &lsp, &ls_cap, arm->body);
                    }
                    break;
                }
                /* Existential elimination: `(open packed [a v] body)` binds the
                 * unboxed value `v` locally, so register var_binding as a local
                 * def (it must never be treated as a free var) and descend into
                 * both the scrutinee and body so any `let` nested there is
                 * collected -- mirrors EX_MATCH's arm-binding handling. */
                case EX_EXISTS_OPEN:
                    if (cur->as.exists_open_.var_binding) {
                        if (n_local >= cap_local) {
                            cap_local = cap_local ? cap_local * 2 : 8;
                            local_defs = (Binding **)realloc(local_defs,
                                cap_local * sizeof(Binding *));
                        }
                        local_defs[n_local++] = cur->as.exists_open_.var_binding;
                    }
                    if (cur->as.exists_open_.packed) fv_push(&ls, &lsp, &ls_cap, cur->as.exists_open_.packed);
                    if (cur->as.exists_open_.body)   fv_push(&ls, &lsp, &ls_cap, cur->as.exists_open_.body);
                    break;
                case EX_EXISTS_PACK:
                    if (cur->as.exists_pack_.value) fv_push(&ls, &lsp, &ls_cap, cur->as.exists_pack_.value);
                    break;
                case EX_EXISTS_DISPATCH:
                    for (uint32_t i = cur->as.exists_dispatch_.n_args; i > 0; i--)
                        fv_push(&ls, &lsp, &ls_cap, cur->as.exists_dispatch_.args[i-1]);
                    break;
                /* Delimited control: descend so a `let` nested under a
                 * shift/reset body is registered as locally defined (matches the
                 * main traversal below). */
                case EX_RESET:
                    if (cur->as.reset_.body) fv_push(&ls, &lsp, &ls_cap, cur->as.reset_.body);
                    break;
                case EX_SHIFT:
                    if (cur->as.shift_.k_fn) fv_push(&ls, &lsp, &ls_cap, cur->as.shift_.k_fn);
                    if (cur->as.shift_.body) fv_push(&ls, &lsp, &ls_cap, cur->as.shift_.body);
                    break;
                case EX_SHIFT0:
                    if (cur->as.shift0_.k_fn) fv_push(&ls, &lsp, &ls_cap, cur->as.shift0_.k_fn);
                    if (cur->as.shift0_.body) fv_push(&ls, &lsp, &ls_cap, cur->as.shift0_.body);
                    break;
                case EX_CLONEABLE_RESET:
                    if (cur->as.cloneable_reset_.body) fv_push(&ls, &lsp, &ls_cap, cur->as.cloneable_reset_.body);
                    break;
                case EX_CLONEABLE_SHIFT:
                    if (cur->as.cloneable_shift_.k_fn) fv_push(&ls, &lsp, &ls_cap, cur->as.cloneable_shift_.k_fn);
                    if (cur->as.cloneable_shift_.body) fv_push(&ls, &lsp, &ls_cap, cur->as.cloneable_shift_.body);
                    break;
                case EX_SERIAL_RESET:
                    if (cur->as.serial_reset_.body) fv_push(&ls, &lsp, &ls_cap, cur->as.serial_reset_.body);
                    break;
                case EX_SERIAL_SHIFT:
                    if (cur->as.serial_shift_.k_fn) fv_push(&ls, &lsp, &ls_cap, cur->as.serial_shift_.k_fn);
                    if (cur->as.serial_shift_.body) fv_push(&ls, &lsp, &ls_cap, cur->as.serial_shift_.body);
                    break;
                /* The two forms below the explicit arms do not cover that bind
                 * a name of their own: a handler value's case params and k, and
                 * a select clause's received value.  Register them, then let
                 * the generic enumeration push the operands. */
                case EX_HANDLER_LIT: {
                    const HandleExpr *h = cur->as.handler_lit_.handle;
                    for (uint8_t ci = 0; h && ci < h->n_cases; ci++) {
                        const HandleCase *hc = &h->cases[ci];
                        if (hc->k_binding)
                            fv_add_local(&local_defs, &n_local, &cap_local, hc->k_binding);
                        for (uint32_t pi = 0; hc->param_bindings && pi < hc->n_params; pi++)
                            if (hc->param_bindings[pi])
                                fv_add_local(&local_defs, &n_local, &cap_local, hc->param_bindings[pi]);
                    }
                    cps_visit_children(cur, fv_push_visit, &ls_ctx);
                    break;
                }
                case EX_SELECT:
                    for (uint32_t ci = 0; ci < cur->as.select_.n_clauses; ci++)
                        if (cur->as.select_.clauses[ci].recv_binding)
                            fv_add_local(&local_defs, &n_local, &cap_local,
                                         cur->as.select_.clauses[ci].recv_binding);
                    cps_visit_children(cur, fv_push_visit, &ls_ctx);
                    break;
                /* Every other kind with an evaluated operand: the arms above
                 * only exist where a kind needs more than its operands
                 * visited.  This used to be `break`, so a `let` under any
                 * unlisted form (an `stm` body, a `select` clause) was never
                 * registered. */
                default:
                    cps_visit_children(cur, fv_push_visit, &ls_ctx);
                    break;
            }
        }
        free(ls);
    }

    Binding **result = NULL;
    uint32_t cap = 0;
    uint32_t n = 0;

    /* Simple recursive traversal using a stack */
    int stack_cap = 256;
    const Expr **stack = (const Expr **)malloc((size_t)stack_cap * sizeof(const Expr *));
    if (!stack) { fprintf(stderr, "tur: oom\n"); abort(); }
    int sp = 0;
    FvStack stack_ctx = { &stack, &sp, &stack_cap };
    fv_push(&stack, &sp, &stack_cap, e);

    while (sp > 0) {
        const Expr *cur = stack[--sp];

        if (cur->kind == EX_VAR) {
            /* Check if this is a param or global */
            bool is_param = false;
            for (uint32_t i = 0; i < n_params; i++) {
                if (params[i] == cur->as.var.binding) {
                    is_param = true;
                    break;
                }
            }
            if (!is_param && !cur->as.var.binding->is_global) {
                /* Exclude bindings introduced by `let` within this expression —
                 * they are locally defined, not free variables. */
                bool is_local = false;
                for (uint32_t i = 0; i < n_local; i++) {
                    if (local_defs[i] == cur->as.var.binding) {
                        is_local = true;
                        break;
                    }
                }
                if (!is_local) {
                    /* This is a free variable - check if it's already in result */
                    bool found = false;
                    for (uint32_t i = 0; i < n; i++) {
                        if (result[i] == cur->as.var.binding) {
                            found = true;
                            break;
                        }
                    }
                    if (!found) {
                        if (n >= cap) {
                            cap = cap ? cap * 2 : 8;
                            result = (Binding **)realloc(result, cap * sizeof(Binding *));
                        }
                        result[n++] = cur->as.var.binding;
                    }
                }
            }
            continue;
        }

        /* Traverse children (in reverse order for depth-first) */
        switch (cur->kind) {
            case EX_LET:
            case EX_LETREC:
                for (uint32_t i = cur->as.let_.n; i > 0; i--) {
                    fv_push(&stack, &sp, &stack_cap, cur->as.let_.bindings[i-1].init);
                }
                fv_push(&stack, &sp, &stack_cap, cur->as.let_.body);
                break;
            case EX_IF:
                if (cur->as.if_.else_or_null) fv_push(&stack, &sp, &stack_cap, cur->as.if_.else_or_null);
                fv_push(&stack, &sp, &stack_cap, cur->as.if_.then_);
                fv_push(&stack, &sp, &stack_cap, cur->as.if_.cond);
                break;
            case EX_DO:
                for (uint32_t i = cur->as.do_.n; i > 0; i--) {
                    fv_push(&stack, &sp, &stack_cap, cur->as.do_.items[i-1]);
                }
                break;
            case EX_WHILE:
                fv_push(&stack, &sp, &stack_cap, cur->as.while_.body);
                fv_push(&stack, &sp, &stack_cap, cur->as.while_.cond);
                break;
            case EX_SET:
                fv_push(&stack, &sp, &stack_cap, cur->as.set_.value);
                break;
            case EX_DEF:
                fv_push(&stack, &sp, &stack_cap, cur->as.def_.init);
                break;
            case EX_BUILTIN:
                for (uint32_t i = cur->as.builtin.n; i > 0; i--) {
                    fv_push(&stack, &sp, &stack_cap, cur->as.builtin.args[i-1]);
                }
                break;
            case EX_CALL:
                /* CC3-EV (curried-call-cast-rough-edges-plan): the callee in
                 * an EX_CALL is stored as fn_binding (a Binding *) rather
                 * than an EX_VAR child, so a call to a let-bound closure
                 * value from inside an inner closure body would not be seen
                 * here as a free variable -- the inner closure would compile
                 * without capturing it, and the generated C would reference
                 * an undeclared local.  Treat fn_binding as if it were an
                 * EX_VAR for free-var purposes: if it's not a param, not a
                 * global, and not a let-local, count it as captured. */
                if (cur->as.call_.fn_binding &&
                    (cur->as.call_.fn_binding->closure_fn_binding ||
                     cur->as.call_.fn_binding->type.kind == TY_PTR_VOID ||
                     (cur->as.call_.fn_binding->type.kind == TY_FN &&
                      cur->as.call_.fn_binding->is_param) ||
                     (cur->as.call_.fn_binding->type.kind == TY_FN &&
                      cur->as.call_.fn_binding->is_match_binding) ||
                     (cur->as.call_.fn_binding->type.kind == TY_FN &&
                      cur->as.call_.fn_binding->is_letrec_binding) ||
                     (cur->as.call_.fn_binding->type.kind == TY_FN &&
                      !cur->as.call_.fn_binding->is_global) ||
                     cur->as.call_.fn_binding->is_fat ||
                     cur->as.call_.is_poly_call)) {
                    /* TY_FN local value: a function-typed binding invoked as the
                     * callee `(g x)` inside an inner closure must be captured by
                     * env; otherwise the lifted closure references the local `g`
                     * at file scope ('g' undeclared).  Two forms qualify:
                     *
                     *  - a typed fn PARAMETER (`g : (fn [a] b)`), and
                     *  - a `match`-arm payload binding destructured over a
                     *    function-typed carrier (`(AddF f g)` where the ADT's
                     *    element type is `(fn ...)`).
                     *
                     * hkt-cata-function-typed-carrier-not-threaded: the original
                     * `is_param`-only gate dropped the match-arm case, so a fold
                     * whose carrier is `(fn ...)` (a CPS matcher / environment-
                     * passing interpreter algebra) lifted its `(fn [env] (+ (f
                     * env) (g env)))` lambda THIN -- f/g were never added to the
                     * env, and the generated C referenced undeclared locals.
                     *
                     * Both forms are gated by `is_param` / `is_match_binding`
                     * precisely so a letrec/named-let self-recursive TY_FN binding
                     * (which is neither, and is handled by the recursion
                     * machinery) is NOT wrongly captured. */
                    /* Restrict to bindings that hold a closure VALUE.  Four
                     * forms qualify: a let-bound closure (closure_fn_binding),
                     * a :ptr<void> binding being invoked as a fat closure, a
                     * ^fat binding, and a `:fn` poly carrier applied directly
                     * (is_poly_call -- the dispatch path emits `f.fn(f.env,...)`,
                     * so the carrier value must be reached through the env).
                     * All are callable values that the inner closure must
                     * capture by env, not bare function references.
                     *
                     * So is any other function-typed LOCAL.  A let-bound lambda
                     * that captures nothing has no closure_fn_binding: its
                     * lifted body is a global, but the binding itself is a
                     * local holding the C function pointer
                     * (`int64_t (*g)() = __fn_N`), and `(fn [] (g))` must carry
                     * that pointer in its env or name an undeclared `g`
                     * (r7rs-srfi-plan S3; tests/fixtures/closure-calls-let-
                     * bound-lambda).  A binding that really is global -- a
                     * defn, or a captureless letrec member lifted as one -- is
                     * excluded by the is_global check below, which also keeps
                     * letrec/named-let self-recursion out. */
                    Binding *fb = cur->as.call_.fn_binding;
                    bool fb_is_param = false;
                    for (uint32_t i = 0; i < n_params; i++) {
                        if (params[i] == fb) { fb_is_param = true; break; }
                    }
                    /* Edge 1: a letrec/named-let group member called *directly*
                     * from the init's own top-level body is recursion, not a
                     * capture -- the recursion machinery (captureless global
                     * lifting, or the S5 env-ptr self-call) handles it.  Skip
                     * any fn_binding that is in the active self-exclude group.
                     * A reference from a NESTED closure passes an empty group
                     * (cleared at elab_fn entry), so it is captured normally. */
                    /* letrec-sibling-closure-not-captured: except a member
                     * already elaborated to a CLOSURE (closure_fn_binding is
                     * set once its init is done, so never on the init being
                     * elaborated): the recursion machinery covers only the
                     * init's own self-call and captureless (global) members,
                     * so a call to an earlier sibling that captures -- `mp`
                     * calling `fq` over `m` -- must carry it in the env, or the
                     * lifted body names an undeclared local. */
                    bool fb_is_self_excluded = false;
                    for (uint32_t i = 0; i < n_self_exclude; i++) {
                        if (self_exclude[i] == fb && letrec_member_is_recursion(fb)) { fb_is_self_excluded = true; break; }
                    }
                    if (!fb_is_param && !fb->is_global && !fb_is_self_excluded) {
                        bool fb_is_local = false;
                        for (uint32_t i = 0; i < n_local; i++) {
                            if (local_defs[i] == fb) { fb_is_local = true; break; }
                        }
                        if (!fb_is_local) {
                            bool found = false;
                            for (uint32_t i = 0; i < n; i++) {
                                if (result[i] == fb) { found = true; break; }
                            }
                            if (!found) {
                                if (n >= cap) {
                                    cap = cap ? cap * 2 : 8;
                                    result = (Binding **)realloc(result, cap * sizeof(Binding *));
                                }
                                result[n++] = fb;
                            }
                        }
                    }
                }
                if (cur->as.call_.fn_expr) {
                    fv_push(&stack, &sp, &stack_cap, cur->as.call_.fn_expr);
                }
                for (uint32_t i = cur->as.call_.n_args; i > 0; i--) {
                    fv_push(&stack, &sp, &stack_cap, cur->as.call_.args[i-1]);
                }
                break;
            case EX_FN_DEF:
                fv_push(&stack, &sp, &stack_cap, cur->as.fn_def_.fn->body);
                break;
            /* Transitive capture: a nested closure that has already been
             * elaborated is an EX_CLOSURE node whose body no longer exposes
             * its free variables as bare EX_VARs (they are accessed through
             * the inner env at emit time).  Descending into fn->body here
             * would also wrongly collect the inner closure's own params.
             * Instead, fold in the inner closure's precomputed capture set:
             * every variable the inner closure needs that is bound *outside*
             * this enclosing scope must also be captured by this scope so the
             * frame can forward it inward.  The inner capture set already
             * excludes the inner's own params/locals, so the param/global/
             * local filtering below is all that's required. */
            case EX_CLOSURE:
                if (cur->as.closure_.closure) {
                    struct Closure *inner = cur->as.closure_.closure;
                    for (uint32_t ci = 0; ci < inner->n_captures; ci++) {
                        Binding *icap = inner->captures[ci];
                        if (!icap || icap->is_global) continue;
                        bool is_param = false;
                        for (uint32_t i = 0; i < n_params; i++) {
                            if (params[i] == icap) { is_param = true; break; }
                        }
                        if (is_param) continue;
                        /* Edge 1: a nested closure that captured a letrec self
                         * binding must NOT forward that capture up into the
                         * very closure the letrec binds it to (this scope IS
                         * that closure -- it provides the value via its own env
                         * pointer at emit time, not via a capture slot, so a
                         * self-referential env is avoided). */
                        bool is_self_excluded = false;
                        for (uint32_t i = 0; i < n_self_exclude; i++) {
                            /* An earlier sibling closure is forwarded, as above. */
                            if (self_exclude[i] == icap && letrec_member_is_recursion(icap)) { is_self_excluded = true; break; }
                        }
                        if (is_self_excluded) continue;
                        bool is_local = false;
                        for (uint32_t i = 0; i < n_local; i++) {
                            if (local_defs[i] == icap) { is_local = true; break; }
                        }
                        if (is_local) continue;
                        bool found = false;
                        for (uint32_t i = 0; i < n; i++) {
                            if (result[i] == icap) { found = true; break; }
                        }
                        if (!found) {
                            if (n >= cap) {
                                cap = cap ? cap * 2 : 8;
                                result = (Binding **)realloc(result, cap * sizeof(Binding *));
                            }
                            result[n++] = icap;
                        }
                    }
                }
                break;
            case EX_RC_DROP:
                fv_push(&stack, &sp, &stack_cap, cur->as.rc_drop_.expr);
                break;
            case EX_DEFER:
                fv_push(&stack, &sp, &stack_cap, cur->as.defer_.body);
                break;
            case EX_RC_OF:
                fv_push(&stack, &sp, &stack_cap, cur->as.rc_of_.expr);
                break;
            case EX_RC_FROM_REF:
                fv_push(&stack, &sp, &stack_cap, cur->as.rc_from_ref_.expr);
                break;
            case EX_REF_FROM_RC:
                fv_push(&stack, &sp, &stack_cap, cur->as.ref_from_rc_.expr);
                break;
            case EX_WEAK:
                fv_push(&stack, &sp, &stack_cap, cur->as.weak_.expr);
                break;
            case EX_WEAK_UPGRADE:
                fv_push(&stack, &sp, &stack_cap, cur->as.weak_upgrade_.expr);
                break;
            case EX_RC_CLONE:
                fv_push(&stack, &sp, &stack_cap, cur->as.rc_clone_.expr);
                break;
            case EX_RC_PTR:
                fv_push(&stack, &sp, &stack_cap, cur->as.rc_ptr_.expr);
                break;
            case EX_RC_COUNT:
                fv_push(&stack, &sp, &stack_cap, cur->as.rc_count_.expr);
                break;
            case EX_WEAK_PRED:
                fv_push(&stack, &sp, &stack_cap, cur->as.weak_pred_.expr);
                break;
            case EX_REF_PRED:
                fv_push(&stack, &sp, &stack_cap, cur->as.ref_pred_.expr);
                break;
            /* Phase 5: ref/deref */
            case EX_REF:
                fv_push(&stack, &sp, &stack_cap, cur->as.ref_.expr);
                break;
            case EX_DEREF:
                fv_push(&stack, &sp, &stack_cap, cur->as.deref_.expr);
                break;
            /* Phase 12: Borrow traits */
            case EX_BORROW_IMMUT:
                fv_push(&stack, &sp, &stack_cap, cur->as.borrow_immut_.expr);
                break;
            case EX_BORROW_MUT:
                fv_push(&stack, &sp, &stack_cap, cur->as.borrow_mut_.expr);
                break;
            case EX_SET_DEREF:
                fv_push(&stack, &sp, &stack_cap, cur->as.set_deref_.ref);
                fv_push(&stack, &sp, &stack_cap, cur->as.set_deref_.value);
                break;
            case EX_MAKE_STRUCT:
                for (uint32_t i = cur->as.make_struct_.n_fields; i > 0; i--) {
                    fv_push(&stack, &sp, &stack_cap, cur->as.make_struct_.field_values[i-1]);
                }
                break;
            case EX_SET_LIT:
                for (uint32_t i = cur->as.set_lit_.n; i > 0; i--) {
                    fv_push(&stack, &sp, &stack_cap, cur->as.set_lit_.items[i-1]);
                }
                break;
            case EX_CONS_LIST:
                /* A `& rest` variadic cons-list build -- its items are ordinary
                 * expressions that may reference enclosing locals; descend so a
                 * captured item is surfaced (a delegated cons-list riding a lifted
                 * CPS continuation env would otherwise miss the capture). */
                for (uint32_t i = cur->as.cons_list_.n; i > 0; i--) {
                    fv_push(&stack, &sp, &stack_cap, cur->as.cons_list_.items[i-1]);
                }
                break;
            case EX_GET_FIELD:
                fv_push(&stack, &sp, &stack_cap, cur->as.get_field_.struct_expr);
                break;
            case EX_SET_FIELD:
                fv_push(&stack, &sp, &stack_cap, cur->as.set_field_.receiver);
                fv_push(&stack, &sp, &stack_cap, cur->as.set_field_.value);
                break;
            /* perform-in-fn-with-any-param-has-no-cps-lowering: the `any` widen
             * and its three readers, for the same reason the catch forms below
             * were added -- an unwalked node kind falls to `default:` and its
             * operand's free variables are silently lost.  Here that surfaced as
             * `'v' undeclared` in a lifted CPS continuation helper: `(is? v Pt)`
             * after a `perform` is delegated as a CT_LETRAW, whose capture set is
             * exactly this walk's answer, so a missed `v` never rode the env. */
            case EX_UNION_INJECT:
                fv_push(&stack, &sp, &stack_cap, cur->as.union_inject_.value);
                break;
            /* saffron-dynamic-surface-pass H1: a captured variable referenced
             * only inside a dynamic operator, call, field read or method
             * dispatch was never collected -- the walker had no arm for the
             * Saffron nodes -- so `(defn adder [k] (fn [x] (+ x k)))` was
             * emitted as a plain function with `k` free (cc: undeclared) and
             * the interpreter, which builds the closure env from this same
             * list, reported `unbound variable: k`.  Every capturing lambda
             * in a Saffron file was affected; a non-capturing one, which is
             * all the fixtures had, was not. */
            case EX_DYN_OP:
                for (uint32_t i = cur->as.dyn_op_.n_args; i > 0; i--)
                    fv_push(&stack, &sp, &stack_cap, cur->as.dyn_op_.args[i-1]);
                break;
            case EX_DYN_CALL:
                for (uint32_t i = cur->as.dyn_call_.n_args; i > 0; i--)
                    fv_push(&stack, &sp, &stack_cap, cur->as.dyn_call_.args[i-1]);
                fv_push(&stack, &sp, &stack_cap, cur->as.dyn_call_.fn);
                break;
            case EX_DYN_FIELD:
                fv_push(&stack, &sp, &stack_cap, cur->as.dyn_field_.obj);
                break;
            case EX_DYN_METHOD:
                for (uint32_t i = cur->as.dyn_method_.n_args; i > 0; i--)
                    fv_push(&stack, &sp, &stack_cap, cur->as.dyn_method_.args[i-1]);
                fv_push(&stack, &sp, &stack_cap, cur->as.dyn_method_.obj);
                break;
            case EX_ANY_TYPE_OF:
                fv_push(&stack, &sp, &stack_cap, cur->as.any_type_of_.value);
                break;
            case EX_ANY_IS:
                fv_push(&stack, &sp, &stack_cap, cur->as.any_is_.value);
                break;
            case EX_ANY_CAST:
                fv_push(&stack, &sp, &stack_cap, cur->as.any_cast_.value);
                break;
            /* Phase 19: Algebraic effects */
            case EX_PERFORM:
                for (uint32_t i = cur->as.perform_.perform->n_args; i > 0; i--) {
                    fv_push(&stack, &sp, &stack_cap, cur->as.perform_.perform->args[i-1]);
                }
                break;
            case EX_HANDLE:
                fv_push(&stack, &sp, &stack_cap, cur->as.handle_.handle->body);
                for (uint8_t i = cur->as.handle_.handle->n_cases; i > 0; i--) {
                    fv_push(&stack, &sp, &stack_cap, cur->as.handle_.handle->cases[i-1].body);
                }
                break;
            case EX_RESUME:
                fv_push(&stack, &sp, &stack_cap, cur->as.resume_.resume->k);
                fv_push(&stack, &sp, &stack_cap, cur->as.resume_.resume->value);
                break;
            case EX_DISCONTINUE:
                fv_push(&stack, &sp, &stack_cap, cur->as.discontinue_.discontinue->k);
                fv_push(&stack, &sp, &stack_cap, cur->as.discontinue_.discontinue->exception);
                break;
            /* (call/cc f) / (escape f): the receiver `f` references enclosing
             * locals, which must be surfaced as free vars of the enclosing scope
             * -- otherwise a capturing escape inside a lifted helper (a handler
             * case, a shift body, an async block) would reference an enclosing
             * local that was never threaded into the helper's env ('<name>'
             * undeclared).  Two receiver shapes occur depending on elaboration
             * order: an already-closure-converted EX_CLOSURE (its captures are
             * folded by the EX_CLOSURE case), or a still-raw EX_FN/EX_FN_DEF
             * (e.g. a handler case whose captures are computed before the escape
             * lambda is closure-converted).  For the raw form, recursively
             * collect the receiver body's free vars excluding its own params and
             * merge them. */
            /* httpd-mw-recover-unblocked-but-unwritten (C)/(D): a catch
             * boundary's THUNK is a lambda like call/cc's receiver, and its
             * free variables are free in the enclosing scope too.  Neither
             * catch form was walked here at all (they fell through to
             * `default:`), so a closure whose only use of an enclosing name
             * was inside `(catch-unwind (fn [] ... name ...))` never captured
             * it -- and the lifted thunk's env-fill then referenced a name
             * that is not in scope there: `'next_1337' undeclared`.
             * Same two receiver shapes as call/cc, so the same handling. */
            case EX_CATCH_UNWIND:
            case EX_CATCH_PANIC_OF: {
                const Expr *rf = (cur->kind == EX_CATCH_UNWIND)
                                     ? cur->as.catch_unwind_.thunk
                                     : cur->as.catch_panic_of_.thunk;
                while (rf && rf->kind == EX_ASCRIBE) rf = rf->as.ascribe_.inner;
                FnDef *rfn = NULL;
                if (rf && rf->kind == EX_FN)          rfn = rf->as.fn_.fn;
                else if (rf && rf->kind == EX_FN_DEF) rfn = rf->as.fn_def_.fn;
                if (rfn && rfn->body) {
                    uint32_t sub_n = 0;
                    Binding **sub = collect_free_vars(rfn->body, rfn->params,
                                                      rfn->n_params,
                                                      self_exclude, n_self_exclude,
                                                      &sub_n);
                    for (uint32_t si = 0; si < sub_n; si++) {
                        Binding *sb = sub[si];
                        if (!sb || sb->is_global) continue;
                        bool skip = false;
                        for (uint32_t i = 0; i < n_params; i++) if (params[i] == sb) { skip = true; break; }
                        for (uint32_t i = 0; !skip && i < n_local; i++) if (local_defs[i] == sb) skip = true;
                        for (uint32_t i = 0; !skip && i < n; i++) if (result[i] == sb) skip = true;
                        if (skip) continue;
                        if (n >= cap) {
                            cap = cap ? cap * 2 : 8;
                            result = (Binding **)realloc(result, cap * sizeof(Binding *));
                        }
                        result[n++] = sb;
                    }
                    free(sub);
                } else if (rf) {
                    fv_push(&stack, &sp, &stack_cap, (Expr *)rf);   /* EX_CLOSURE folds captures */
                }
                break;
            }
            case EX_CALLCC: {
                const Expr *rf = cur->as.callcc_.fn;
                while (rf && rf->kind == EX_ASCRIBE) rf = rf->as.ascribe_.inner;
                FnDef *rfn = NULL;
                if (rf && rf->kind == EX_FN)     rfn = rf->as.fn_.fn;
                else if (rf && rf->kind == EX_FN_DEF) rfn = rf->as.fn_def_.fn;
                if (rfn && rfn->body) {
                    uint32_t sub_n = 0;
                    Binding **sub = collect_free_vars(rfn->body, rfn->params, rfn->n_params,
                                                      self_exclude, n_self_exclude, &sub_n);
                    for (uint32_t si = 0; si < sub_n; si++) {
                        Binding *sb = sub[si];
                        if (!sb || sb->is_global) continue;
                        bool skip = false;
                        for (uint32_t i = 0; i < n_params; i++) if (params[i] == sb) { skip = true; break; }
                        for (uint32_t i = 0; !skip && i < n_local; i++) if (local_defs[i] == sb) skip = true;
                        for (uint32_t i = 0; !skip && i < n; i++) if (result[i] == sb) skip = true;
                        if (skip) continue;
                        if (n >= cap) {
                            cap = cap ? cap * 2 : 8;
                            result = (Binding **)realloc(result, cap * sizeof(Binding *));
                        }
                        result[n++] = sb;
                    }
                    free(sub);
                } else {
                    fv_push(&stack, &sp, &stack_cap, cur->as.callcc_.fn);   /* EX_CLOSURE folds captures */
                }
                break;
            }
            /* Phase T21: Async/await */
            case EX_ASYNC:
                fv_push(&stack, &sp, &stack_cap, cur->as.async_.fn_expr);
                break;
            case EX_AWAIT:
                fv_push(&stack, &sp, &stack_cap, cur->as.await_.fut_expr);
                break;
            /* Phase 3/4: Return */
            case EX_RETURN:
                if (cur->as.return_.value) fv_push(&stack, &sp, &stack_cap, cur->as.return_.value);
                break;
            /* Phase R2: Panic */
            case EX_PANIC:
                fv_push(&stack, &sp, &stack_cap, cur->as.panic_.payload);
                break;
            case EX_MATCH:
                fv_push(&stack, &sp, &stack_cap, cur->as.match_.scrutinee);
                for (uint32_t ai = cur->as.match_.n_arms; ai > 0; ai--) {
                    MatchArm *arm = &cur->as.match_.arms[ai-1];
                    if (arm->guard) fv_push(&stack, &sp, &stack_cap, arm->guard);
                    if (arm->body)  fv_push(&stack, &sp, &stack_cap, arm->body);
                }
                break;
            /* SS2: Walk val_exprs so channel EX_VAR nodes are found as free vars */
            case EX_INLINE_C: {
                InlineC *ic = cur->as.inline_c_.inline_c;
                if (ic) {
                    for (uint8_t vi = 0; vi < ic->n_val_exprs; vi++) {
                        if (ic->val_exprs[vi]) fv_push(&stack, &sp, &stack_cap, ic->val_exprs[vi]);
                    }
                }
                break;
            }
            /* GF1: Generator forms */
            case EX_GEN:
                if (cur->as.gen_.def && cur->as.gen_.def->body)
                    fv_push(&stack, &sp, &stack_cap, cur->as.gen_.def->body);
                break;
            case EX_YIELD:
                if (cur->as.yield_.value) fv_push(&stack, &sp, &stack_cap, cur->as.yield_.value);
                break;
            case EX_GEN_NEXT:
                if (cur->as.gen_next_.gen_expr) fv_push(&stack, &sp, &stack_cap, cur->as.gen_next_.gen_expr);
                break;
            case EX_GEN_UNWRAP:
                if (cur->as.gen_unwrap_.ptr_expr) fv_push(&stack, &sp, &stack_cap, cur->as.gen_unwrap_.ptr_expr);
                break;
            case EX_GEN_DONE:
                if (cur->as.gen_done_.gen_expr) fv_push(&stack, &sp, &stack_cap, cur->as.gen_done_.gen_expr);
                break;
            /* (:: expr T) is type-erased at codegen; descend into the inner
             * expr so a variable that only appears under an ascription is still
             * seen as a free variable and captured by the enclosing closure.
             * Without this, `(use-raw (:: ch :ptr<void>))` inside a `(fn ...)`
             * misses `ch` and emits the bare local instead of the env access. */
            case EX_ASCRIBE:
                if (cur->as.ascribe_.inner) fv_push(&stack, &sp, &stack_cap, cur->as.ascribe_.inner);
                break;
            /* Compiler-only conversion shims wrap an inner expr that may be the
             * sole reference to a free variable -- e.g. a `:fn` value passed
             * through to another `:fn` param is wrapped in EX_POLY_WRAP, and a
             * captured `:fn` boxed into a ^fat sink is wrapped in EX_POLY_TO_FAT.
             * Descend so that variable is still captured. */
            case EX_REINTERPRET:
                if (cur->as.reinterpret_.expr) fv_push(&stack, &sp, &stack_cap, cur->as.reinterpret_.expr);
                break;
            case EX_CAST:
                if (cur->as.cast_.expr) fv_push(&stack, &sp, &stack_cap, cur->as.cast_.expr);
                break;
            case EX_FN_TO_FAT:
                if (cur->as.fn_to_fat_.inner) fv_push(&stack, &sp, &stack_cap, cur->as.fn_to_fat_.inner);
                break;
            case EX_POLY_TO_FAT:
                if (cur->as.poly_to_fat_.inner) fv_push(&stack, &sp, &stack_cap, cur->as.poly_to_fat_.inner);
                break;
            case EX_POLY_WRAP:
                if (cur->as.poly_wrap_.inner) fv_push(&stack, &sp, &stack_cap, cur->as.poly_wrap_.inner);
                break;
            /* Existential forms.  Without descending here, a variable referenced
             * only inside an `open` scrutinee (`(open (vec-get rs i) [a v] ...)`),
             * a `pack` value, or a witness-dispatch arg is never seen as a free
             * variable -- so the enclosing closure fails to capture it and the
             * lifted C body references the bare outer local ('rs_NNNN'
             * undeclared).  The open's `v` binding is registered as a local def
             * in the pre-pass, so its body references to `v` are filtered out. */
            case EX_EXISTS_OPEN:
                if (cur->as.exists_open_.packed) fv_push(&stack, &sp, &stack_cap, cur->as.exists_open_.packed);
                if (cur->as.exists_open_.body)   fv_push(&stack, &sp, &stack_cap, cur->as.exists_open_.body);
                break;
            case EX_EXISTS_PACK:
                if (cur->as.exists_pack_.value) fv_push(&stack, &sp, &stack_cap, cur->as.exists_pack_.value);
                break;
            case EX_EXISTS_DISPATCH:
                for (uint32_t i = cur->as.exists_dispatch_.n_args; i > 0; i--)
                    fv_push(&stack, &sp, &stack_cap, cur->as.exists_dispatch_.args[i-1]);
                break;
            /* Delimited control (reset/shift/shift0, cloneable variants): descend
             * into the delimited body and receiver so a local referenced only
             * inside a shift/reset body surfaces as a free variable of the
             * enclosing scope.  Without this the constructs hit `default` and
             * their subtrees were invisible -- e.g. a value captured as a
             * cloneable-shift body was never seen by the E4 Clone-capture check. */
            case EX_RESET:
                if (cur->as.reset_.body) fv_push(&stack, &sp, &stack_cap, cur->as.reset_.body);
                break;
            case EX_SHIFT:
                if (cur->as.shift_.k_fn) fv_push(&stack, &sp, &stack_cap, cur->as.shift_.k_fn);
                if (cur->as.shift_.body) fv_push(&stack, &sp, &stack_cap, cur->as.shift_.body);
                break;
            case EX_SHIFT0:
                if (cur->as.shift0_.k_fn) fv_push(&stack, &sp, &stack_cap, cur->as.shift0_.k_fn);
                if (cur->as.shift0_.body) fv_push(&stack, &sp, &stack_cap, cur->as.shift0_.body);
                break;
            case EX_CLONEABLE_RESET:
                if (cur->as.cloneable_reset_.body) fv_push(&stack, &sp, &stack_cap, cur->as.cloneable_reset_.body);
                break;
            case EX_CLONEABLE_SHIFT:
                if (cur->as.cloneable_shift_.k_fn) fv_push(&stack, &sp, &stack_cap, cur->as.cloneable_shift_.k_fn);
                if (cur->as.cloneable_shift_.body) fv_push(&stack, &sp, &stack_cap, cur->as.cloneable_shift_.body);
                break;
            case EX_SERIAL_RESET:
                if (cur->as.serial_reset_.body) fv_push(&stack, &sp, &stack_cap, cur->as.serial_reset_.body);
                break;
            case EX_SERIAL_SHIFT:
                if (cur->as.serial_shift_.k_fn) fv_push(&stack, &sp, &stack_cap, cur->as.serial_shift_.k_fn);
                if (cur->as.serial_shift_.body) fv_push(&stack, &sp, &stack_cap, cur->as.serial_shift_.body);
                break;
            /* Every kind without an arm above: visit its evaluated operands
             * through the one enumeration that covers them all.  This used to
             * be `break`, which silently dropped every capture under an
             * unlisted form -- `(fn [] (atomically (stm (tvar/write tv 5))))`
             * lifted as captureless and its body named an undeclared `tv`
             * (docs/archive/stm-inside-closure-captured-tvar-undeclared.md);
             * `select`, `with-handler` and dynamic `binding` had the same
             * hole.  Nested functions are not enumerated there either, and
             * EX_FN_DEF / EX_CLOSURE keep their own arms above. */
            default:
                cps_visit_children(cur, fv_push_visit, &stack_ctx);
                break;
        }
    }
    free(stack);
    free(local_defs);
    *n_out = n;
    return result;
}

/* Phase 3: Register a file-scope definition to be emitted later */
void elab_register_file_def(Elab *e, Expr *def_expr) {
    if (!def_expr) return;
    /* load-not-expanded-in-imported-or-project-modules: under separate
     * compilation, definitions elaborated while loading an imported module
     * belong to that module's own translation unit -- it is compiled
     * independently and the importer #includes its header.  Registering them
     * here would re-emit them in the importer's TU (e.g. a typeclass instance's
     * method body referencing the owner module's internal ADT, whose typedef is
     * absent in this TU).  Skip; the self-registering forms (defclass /
     * definstance / method defs) route through this function too, so this one
     * gate covers them all. */
    if (e->separate_compilation && e->in_imported_module) return;
    if (e->n_file_scope_defs >= e->cap_file_scope_defs) {
        e->cap_file_scope_defs = e->cap_file_scope_defs ? e->cap_file_scope_defs * 2 : 16;
        e->file_scope_defs = (Expr **)realloc(e->file_scope_defs, 
            e->cap_file_scope_defs * sizeof(Expr *));
    }
    e->file_scope_defs[e->n_file_scope_defs++] = def_expr;
}

const Symbol *intern_cstr(SymbolTable *st, const char *s) {
    return symtab_intern(st, strslice(s, (uint32_t)strlen(s)));
}

/* Phase 11: Copy/Move trait tracking */

/* Mark a binding as moved (poisoned). Returns true if successfully marked,
 * false if it was already moved (use-after-move). */
bool binding_mark_moved(Binding *b, Span use_span) {
    b->moved_owning = true;   /* sticky; see Binding.moved_owning */
    if (b->is_moved) {
        return false; /* Already moved - use-after-move */
    }
    b->is_moved = true;
    b->moved_at = use_span;
    return true;
}

/* byvalue-recursive-adt-boxes-are-never-freed (Residue 1): the value is
 * passed to a callee proven not to retain it.  Same static effect as a move
 * (the binding is poisoned for later uses, so the type checker's answer does
 * not change), but ownership stays with this scope, which the scope-exit
 * spine drop reads through `lent_to_nonretaining`. */
bool binding_mark_lent(Binding *b, Span use_span) {
    b->lent_to_nonretaining = true;
    if (b->is_moved) return false;
    b->is_moved = true;
    b->moved_at = use_span;
    return true;
}

static bool binding_has_suspicious_param_annotation(const Binding *b) {
    if (!b || !b->is_param) return false;
    if (span_is_unknown(b->span)) return false;
    /* structdef-retirement slice 5 B2 (P1): the demoted single-occurrence
     * unresolved param is now a named TY_TYVAR.  This code is only
     * reached for a *moved* (CK_MOVE) binding, and a genuine declared tyvar
     * param is CK_COPY (never moved), so the source-text `:` check below
     * remains the precise discriminator -- it does not false-fire on real
     * polymorphic params. */
    if (b->type.kind != TY_TYVAR) return false;
    const SourceFile *file = diag_source_file(b->span.file_id);
    if (!file || !file->src || b->span.off_end > file->len) return false;
    const char *p = file->src + b->span.off_end;
    const char *end = file->src + file->len;
    while (p < end && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')) p++;
    return p < end && *p == ':';
}

/* Check if a binding has been moved. Emits use-after-move diagnostic if so. */
bool binding_check_not_moved(Binding *b, Span use_span, const char *use_desc) {
    if (b->is_moved) {
        /* Emit use-after-move error */
        diag_emit_with_code(DIAG_ERROR, use_span, TUR_E0005_USE_AFTER_MOVE,
                            "use-after-move: %s '%s' was moved and cannot be used again",
                            use_desc, b->name->name);
        if (!span_is_unknown(b->moved_at)) {
            diag_emit(DIAG_NOTE, b->moved_at, "moved here");
        }
        if (binding_has_suspicious_param_annotation(b)) {
            diag_emit(DIAG_NOTE, b->span,
                      "parameter '%s' looks like it was followed by a type annotation; "
                      "use `[%s :Type]` or `[%s : Type]`",
                      b->name->name, b->name->name, b->name->name);
        }
        return false;
    }
    return true;
}

/* Snapshot/restore move-state for all currently visible bindings.
 * Used to make branch elaboration path-sensitive for move tracking. */
uint32_t move_state_snapshot_bindings(const Scope *scope,
                                             Binding ***out_bindings,
                                             bool **out_states) {
    uint32_t n = 0;
    uint32_t cap = 16;
    Binding **bindings = (Binding **)malloc(cap * sizeof(Binding *));
    bool *states = (bool *)malloc(cap * sizeof(bool));
    if (!bindings || !states) {
        fprintf(stderr, "tur: oom\n");
        abort();
    }

    for (const Scope *cur = scope; cur; cur = cur->parent) {
        for (uint32_t i = 0; i < cur->n; i++) {
            if (n == cap) {
                cap *= 2;
                bindings = (Binding **)realloc(bindings, cap * sizeof(Binding *));
                states = (bool *)realloc(states, cap * sizeof(bool));
                if (!bindings || !states) {
                    fprintf(stderr, "tur: oom\n");
                    abort();
                }
            }
            bindings[n] = cur->bindings[i];
            states[n] = cur->bindings[i]->is_moved;
            n++;
        }
    }

    *out_bindings = bindings;
    *out_states = states;
    return n;
}

bool *move_state_capture_current(Binding **bindings, uint32_t n) {
    bool *states = (bool *)malloc((n == 0 ? 1 : n) * sizeof(bool));
    if (!states) {
        fprintf(stderr, "tur: oom\n");
        abort();
    }
    for (uint32_t i = 0; i < n; i++) {
        states[i] = bindings[i]->is_moved;
    }
    return states;
}

void move_state_restore(Binding **bindings, const bool *states, uint32_t n) {
    for (uint32_t i = 0; i < n; i++) {
        bindings[i]->is_moved = states[i];
    }
}

/* LT1: Parallel helpers for is_linear_consumed state across branches.
 * Only linear bindings (is_linear == true) are included in the snapshot. */

uint32_t linear_state_snapshot_bindings(const Scope *scope,
                                                Binding ***out_bindings,
                                                bool **out_states) {
    uint32_t n = 0;
    uint32_t cap = 16;
    Binding **bindings = (Binding **)malloc(cap * sizeof(Binding *));
    bool *states = (bool *)malloc(cap * sizeof(bool));
    if (!bindings || !states) {
        fprintf(stderr, "tur: oom\n");
        abort();
    }

    for (const Scope *cur = scope; cur; cur = cur->parent) {
        for (uint32_t i = 0; i < cur->n; i++) {
            Binding *b = cur->bindings[i];
            if (!b->is_linear) continue;
            if (n == cap) {
                cap *= 2;
                bindings = (Binding **)realloc(bindings, cap * sizeof(Binding *));
                states = (bool *)realloc(states, cap * sizeof(bool));
                if (!bindings || !states) {
                    fprintf(stderr, "tur: oom\n");
                    abort();
                }
            }
            bindings[n] = b;
            states[n] = b->is_linear_consumed;
            n++;
        }
    }

    *out_bindings = bindings;
    *out_states = states;
    return n;
}

bool *linear_state_capture_current(Binding **bindings, uint32_t n) {
    bool *states = (bool *)malloc((n == 0 ? 1 : n) * sizeof(bool));
    if (!states) {
        fprintf(stderr, "tur: oom\n");
        abort();
    }
    for (uint32_t i = 0; i < n; i++) {
        states[i] = bindings[i]->is_linear_consumed;
    }
    return states;
}

void linear_state_restore(Binding **bindings, const bool *states, uint32_t n) {
    for (uint32_t i = 0; i < n; i++) {
        bindings[i]->is_linear_consumed = states[i];
    }
}

/* Check if a binding is explicitly disposed in the body: an RC binding consumed
 * by ref/from-rc or dropped via rc/drop, or a ref<T> binding dropped via drop!.
 * Returns true if the binding is disposed (so the auto-drop must be skipped to
 * avoid a double-free). */
bool is_binding_consumed(const Expr *body, Binding *binding) {
    if (!body) return false;
    
    const Expr **stack = (const Expr **)malloc(256 * sizeof(const Expr *));
    if (!stack) {
        fprintf(stderr, "tur: oom\n");
        abort();
    }
    int sp = 0;
    
    /* Start by pushing body onto stack */
    stack[sp++] = body;
    
    while (sp > 0) {
        const Expr *cur = stack[--sp];
        if (!cur) continue;
        
        /* Check if this expression consumes the binding via ref/from-rc */
        if (cur->kind == EX_REF_FROM_RC &&
            cur->as.ref_from_rc_.expr &&
            cur->as.ref_from_rc_.expr->kind == EX_VAR &&
            cur->as.ref_from_rc_.expr->as.var.binding == binding) {
            free(stack);
            return true;
        }
        
        /* Check if this expression consumes the binding via rc/drop */
        if (cur->kind == EX_RC_DROP &&
            cur->as.rc_drop_.expr &&
            cur->as.rc_drop_.expr->kind == EX_VAR &&
            cur->as.rc_drop_.expr->as.var.binding == binding) {
            free(stack);
            return true;
        }

        /* Check if this expression disposes the binding via explicit (drop! r) */
        if (cur->kind == EX_BUILTIN &&
            cur->as.builtin.spec &&
            cur->as.builtin.spec->name &&
            strcmp(cur->as.builtin.spec->name, "drop!") == 0 &&
            cur->as.builtin.n == 1 &&
            cur->as.builtin.args[0]->kind == EX_VAR &&
            cur->as.builtin.args[0]->as.var.binding == binding) {
            free(stack);
            return true;
        }

        /* Recursively traverse sub-expressions */
        switch (cur->kind) {
            case EX_LET:
            case EX_LETREC:
                for (uint32_t i = cur->as.let_.n; i > 0; i--) {
                    stack[sp++] = cur->as.let_.bindings[i-1].init;
                }
                stack[sp++] = cur->as.let_.body;
                break;
            case EX_IF:
                if (cur->as.if_.else_or_null) stack[sp++] = cur->as.if_.else_or_null;
                stack[sp++] = cur->as.if_.then_;
                stack[sp++] = cur->as.if_.cond;
                break;
            case EX_DO:
                for (uint32_t i = cur->as.do_.n; i > 0; i--) {
                    stack[sp++] = cur->as.do_.items[i-1];
                }
                break;
            case EX_CALL:
                for (uint32_t i = cur->as.call_.n_args; i > 0; i--) {
                    stack[sp++] = cur->as.call_.args[i-1];
                }
                break;
            case EX_BUILTIN:
                for (uint32_t i = cur->as.builtin.n; i > 0; i--) {
                    stack[sp++] = cur->as.builtin.args[i-1];
                }
                break;
            case EX_CLOSURE:
                if (cur->as.closure_.closure && cur->as.closure_.closure->fn) {
                    stack[sp++] = cur->as.closure_.closure->fn->body;
                }
                break;
            case EX_DEFER:
                stack[sp++] = cur->as.defer_.body;
                break;
            case EX_WHILE:
                stack[sp++] = cur->as.while_.body;
                stack[sp++] = cur->as.while_.cond;
                break;
            case EX_PANIC:
                stack[sp++] = cur->as.panic_.payload;
                break;
            case EX_SET:
                stack[sp++] = cur->as.set_.value;
                break;
            case EX_SET_DEREF:
                stack[sp++] = cur->as.set_deref_.ref;
                stack[sp++] = cur->as.set_deref_.value;
                break;
            case EX_MAKE_STRUCT:
                for (uint32_t i = cur->as.make_struct_.n_fields; i > 0; i--) {
                    stack[sp++] = cur->as.make_struct_.field_values[i-1];
                }
                break;
            case EX_SET_LIT:
                for (uint32_t i = cur->as.set_lit_.n; i > 0; i--) {
                    stack[sp++] = cur->as.set_lit_.items[i-1];
                }
                break;
            case EX_GET_FIELD:
                stack[sp++] = cur->as.get_field_.struct_expr;
                break;
            case EX_SET_FIELD:
                stack[sp++] = cur->as.set_field_.receiver;
                stack[sp++] = cur->as.set_field_.value;
                break;
            default:
                break;
        }
    }

    free(stack);
    return false;
}

/* set-bang-rc-release (docs/archive/history/set-bang-does-not-release-old-rc-value.md).
 *
 * An rc-managed `^mut` binding owns exactly ONE strong reference for its whole
 * lifetime: its init takes the +1 and the scope-exit auto-drop releases it.
 * `(set! b v)` overwrote the binding without releasing what was there, so every
 * assignment past the first leaked a block -- and the auto-drop only ever saw
 * the FINAL value (its defer snapshots the variable where it is pushed, at the
 * end of the do-block). Measured: 4000 rounds x 4 assignments leaked exactly
 * 16000 blocks, none of them reclaimable by the cycle collector either, since
 * they are acyclic with a positive strong count.
 *
 * Two things have to happen for the release to be sound, and this pass does the
 * first while emit_set_stmt does the second:
 *
 *  1. `v` must genuinely carry a +1, because the release drops the binding's.
 *     A fresh `(rc/of ...)` does, an explicit `(rc/clone x)` does, and a bare
 *     `(set! b other)` does because the elaborator treats it as a MOVE (the
 *     source binding's own auto-drop is suppressed).  A bare rc FIELD read does
 *     NOT: `(set! h (.next h))` copies the word with no increment, so the
 *     binding would release a reference it never acquired.  Those are wrapped
 *     in EX_RC_CLONE here -- exactly the clone-on-read normalization the
 *     let-binding init path already performs for the same borrow shape.
 *
 *  2. `v` must be fully evaluated BEFORE the old value is released, since it may
 *     read the very binding being overwritten (`(set! h (.next h))` lowers to an
 *     inline `h->value->next`, not a temp).  emit_set_stmt spills to a temp.
 *
 * Only ever called for a binding that qualifies for the scope-exit auto-drop.
 * A binding whose ownership is hand-managed -- moved, or explicitly dropped via
 * `(rc/drop b)` / `(drop! b)` / `ref/from-rc` -- gets no auto-drop, and must get
 * no release here either, or the explicit disposal and this one double-free.
 * Self-assignment `(set! b b)` marks the binding moved and so is excluded by
 * that same gate, which matters: it lowers to `b = b` with no auto-drop at all,
 * and a release would leave the binding dangling. */
void elab_set_rc_release(Arena *arena, Expr *body, Binding *binding) {
    if (!body || !binding) return;

    const Expr **stack = (const Expr **)malloc(256 * sizeof(const Expr *));
    if (!stack) { fprintf(stderr, "tur: oom\n"); abort(); }
    uint32_t cap = 256;
    int sp = 0;
    stack[sp++] = body;

    while (sp > 0) {
        Expr *cur = (Expr *)stack[--sp];
        if (!cur) continue;

        /* `release_old` doubles as the already-processed marker, so a second
         * pass over the same node (a re-elaborated body) cannot wrap the value
         * in a SECOND EX_RC_CLONE -- which would take an increment nothing
         * balances and turn this fix into a leak of its own. */
        if (cur->kind == EX_SET && cur->as.set_.target == binding &&
            !cur->as.set_.release_old) {
            /* (1) normalize a borrow-shaped value to a real +1. */
            Expr *v = cur->as.set_.value;
            Expr *inner = v;
            while (inner && inner->kind == EX_ASCRIBE)
                inner = inner->as.ascribe_.inner;
            if (inner && inner->kind == EX_GET_FIELD && inner->type.kind == TY_RC) {
                Expr *clone = expr_new(arena, EX_RC_CLONE, v->type, v->span);
                clone->as.rc_clone_.expr = v;
                clone->as.rc_clone_.elide = false;
                cur->as.set_.value = clone;
            }
            /* (2) tell codegen to release what is being overwritten. */
            cur->as.set_.release_old = true;
        }

        /* Reserve for the widest child count this node can push -- a `do` block,
         * call, builtin, struct literal or binding group is n-ary, so a fixed
         * headroom would overflow the stack on a long body. */
        uint32_t need = 4;
        switch (cur->kind) {
            case EX_DO:          need = cur->as.do_.n + 1u;             break;
            case EX_CALL:        need = cur->as.call_.n_args + 1u;      break;
            case EX_BUILTIN:     need = cur->as.builtin.n + 1u;         break;
            case EX_MAKE_STRUCT: need = cur->as.make_struct_.n_fields + 1u; break;
            case EX_LET:
            case EX_LETREC:      need = cur->as.let_.n + 2u;            break;
            default:                                                     break;
        }
        if ((uint32_t)sp + need > cap) {
            while (cap < (uint32_t)sp + need) cap *= 2u;
            const Expr **grown =
                (const Expr **)realloc(stack, (size_t)cap * sizeof(const Expr *));
            if (!grown) { fprintf(stderr, "tur: oom\n"); abort(); }
            stack = grown;
        }

        switch (cur->kind) {
            case EX_LET:
            case EX_LETREC:
                for (uint32_t i = cur->as.let_.n; i > 0; i--)
                    stack[sp++] = cur->as.let_.bindings[i-1].init;
                stack[sp++] = cur->as.let_.body;
                break;
            case EX_IF:
                if (cur->as.if_.else_or_null) stack[sp++] = cur->as.if_.else_or_null;
                stack[sp++] = cur->as.if_.then_;
                stack[sp++] = cur->as.if_.cond;
                break;
            case EX_DO:
                for (uint32_t i = cur->as.do_.n; i > 0; i--)
                    stack[sp++] = cur->as.do_.items[i-1];
                break;
            case EX_WHILE:
                stack[sp++] = cur->as.while_.body;
                stack[sp++] = cur->as.while_.cond;
                break;
            case EX_SET:       stack[sp++] = cur->as.set_.value;   break;
            case EX_SET_DEREF:
                stack[sp++] = cur->as.set_deref_.ref;
                stack[sp++] = cur->as.set_deref_.value;
                break;
            case EX_SET_FIELD:
                stack[sp++] = cur->as.set_field_.receiver;
                stack[sp++] = cur->as.set_field_.value;
                break;
            case EX_GET_FIELD: stack[sp++] = cur->as.get_field_.struct_expr; break;
            case EX_CALL:
                for (uint32_t i = cur->as.call_.n_args; i > 0; i--)
                    stack[sp++] = cur->as.call_.args[i-1];
                break;
            case EX_BUILTIN:
                for (uint32_t i = cur->as.builtin.n; i > 0; i--)
                    stack[sp++] = cur->as.builtin.args[i-1];
                break;
            case EX_MAKE_STRUCT:
                for (uint32_t i = cur->as.make_struct_.n_fields; i > 0; i--)
                    stack[sp++] = cur->as.make_struct_.field_values[i-1];
                break;
            case EX_DEFER:     stack[sp++] = cur->as.defer_.body;    break;
            case EX_PANIC:     stack[sp++] = cur->as.panic_.payload; break;
            case EX_RC_OF:     stack[sp++] = cur->as.rc_of_.expr;    break;
            case EX_RC_DROP:   stack[sp++] = cur->as.rc_drop_.expr;  break;
            case EX_RC_CLONE:  stack[sp++] = cur->as.rc_clone_.expr; break;
            case EX_REINTERPRET:   stack[sp++] = cur->as.reinterpret_.expr; break;
            case EX_ASCRIBE:   stack[sp++] = cur->as.ascribe_.inner; break;
            case EX_RETURN:
                if (cur->as.return_.value) stack[sp++] = cur->as.return_.value;
                break;
            /* Deliberately NOT descending into EX_CLOSURE / EX_FN_DEF: a nested
             * function body has its own frame and its own binding lifetimes, and
             * a captured rc is reached through the closure env rather than this
             * binding's slot. */
            default:
                break;
        }
    }

    free(stack);
}

/* Field-level analog of is_binding_consumed: returns true when the body
 * explicitly drops a SPECIFIC owning field of a by-value struct/record local --
 * `(rc/drop (.f o))` for an rc field, `(drop! (.f o))` for a ref field, matched
 * by (binding, field_idx).  A by-value aggregate gets one scope-exit auto-drop
 * per owning field; without this per-field check an explicit field drop would be
 * followed by the auto-drop of the same field -> double-free.  Mirrors
 * is_binding_consumed's conservative convention: a drop found ANYWHERE (incl.
 * one branch of an if) suppresses the auto-drop -- a leak on the untaken path is
 * memory-safe, a double-free is not. */
bool is_field_consumed(const Expr *body, Binding *binding, uint32_t field_idx) {
    if (!body) return false;

    const Expr **stack = (const Expr **)malloc(256 * sizeof(const Expr *));
    if (!stack) {
        fprintf(stderr, "tur: oom\n");
        abort();
    }
    int sp = 0;
    stack[sp++] = body;

    /* True when `fe` is `(.field_idx binding)` -- a field read of the tracked
     * owning field off the by-value local. */
#define TUR_IS_TRACKED_FIELD(fe)                                                \
    ((fe) && (fe)->kind == EX_GET_FIELD &&                                      \
     (fe)->as.get_field_.field_idx == field_idx &&                             \
     (fe)->as.get_field_.struct_expr &&                                        \
     (fe)->as.get_field_.struct_expr->kind == EX_VAR &&                        \
     (fe)->as.get_field_.struct_expr->as.var.binding == binding)

    while (sp > 0) {
        const Expr *cur = stack[--sp];
        if (!cur) continue;

        /* (rc/drop (.f o)) -- explicit drop of the rc field. */
        if (cur->kind == EX_RC_DROP && TUR_IS_TRACKED_FIELD(cur->as.rc_drop_.expr)) {
            free(stack);
            return true;
        }

        /* (drop! (.f o)) -- explicit drop of the ref field. */
        if (cur->kind == EX_BUILTIN &&
            cur->as.builtin.spec &&
            cur->as.builtin.spec->name &&
            strcmp(cur->as.builtin.spec->name, "drop!") == 0 &&
            cur->as.builtin.n == 1 &&
            TUR_IS_TRACKED_FIELD(cur->as.builtin.args[0])) {
            free(stack);
            return true;
        }

        switch (cur->kind) {
            case EX_LET:
            case EX_LETREC:
                for (uint32_t i = cur->as.let_.n; i > 0; i--)
                    stack[sp++] = cur->as.let_.bindings[i-1].init;
                stack[sp++] = cur->as.let_.body;
                break;
            case EX_IF:
                if (cur->as.if_.else_or_null) stack[sp++] = cur->as.if_.else_or_null;
                stack[sp++] = cur->as.if_.then_;
                stack[sp++] = cur->as.if_.cond;
                break;
            case EX_DO:
                for (uint32_t i = cur->as.do_.n; i > 0; i--)
                    stack[sp++] = cur->as.do_.items[i-1];
                break;
            case EX_CALL:
                for (uint32_t i = cur->as.call_.n_args; i > 0; i--)
                    stack[sp++] = cur->as.call_.args[i-1];
                break;
            case EX_BUILTIN:
                for (uint32_t i = cur->as.builtin.n; i > 0; i--)
                    stack[sp++] = cur->as.builtin.args[i-1];
                break;
            case EX_CLOSURE:
                if (cur->as.closure_.closure && cur->as.closure_.closure->fn)
                    stack[sp++] = cur->as.closure_.closure->fn->body;
                break;
            case EX_DEFER:
                stack[sp++] = cur->as.defer_.body;
                break;
            case EX_WHILE:
                stack[sp++] = cur->as.while_.body;
                stack[sp++] = cur->as.while_.cond;
                break;
            case EX_PANIC:
                stack[sp++] = cur->as.panic_.payload;
                break;
            case EX_SET:
                stack[sp++] = cur->as.set_.value;
                break;
            case EX_SET_DEREF:
                stack[sp++] = cur->as.set_deref_.ref;
                stack[sp++] = cur->as.set_deref_.value;
                break;
            case EX_MAKE_STRUCT:
                for (uint32_t i = cur->as.make_struct_.n_fields; i > 0; i--)
                    stack[sp++] = cur->as.make_struct_.field_values[i-1];
                break;
            case EX_SET_LIT:
                for (uint32_t i = cur->as.set_lit_.n; i > 0; i--)
                    stack[sp++] = cur->as.set_lit_.items[i-1];
                break;
            case EX_RC_DROP:
                /* Descend so a nested drop (not of the tracked field) still
                 * walks its operand. */
                stack[sp++] = cur->as.rc_drop_.expr;
                break;
            case EX_GET_FIELD:
                stack[sp++] = cur->as.get_field_.struct_expr;
                break;
            case EX_SET_FIELD:
                stack[sp++] = cur->as.set_field_.receiver;
                stack[sp++] = cur->as.set_field_.value;
                break;
            default:
                break;
        }
    }

#undef TUR_IS_TRACKED_FIELD
    free(stack);
    return false;
}

/* Returns true when a specific owning field of a by-value local is dropped
 * inside a HANDLER CASE body -- `(rc/drop (.f o))` / `(drop! (.f o))` reached
 * through a `handle`.  Unlike is_field_consumed (which deliberately does not
 * descend into handler cases), this walker finds the enclosing `handle` and
 * checks each case body.  It exists ONLY to REJECT that shape, not to suppress
 * an auto-drop: a handler case runs 0..N times, so it can neither balance the
 * enclosing scope's single auto-drop of the same field (double-free when the
 * case runs, leak when it does not) nor be made safe by local drop-suppression.
 * The captured local is owned by the enclosing scope; a case consuming its
 * owning field is unsound regardless of shot count. */
bool is_field_consumed_in_handler(const Expr *body, Binding *binding,
                                  uint32_t field_idx) {
    if (!body) return false;

    const Expr **stack = (const Expr **)malloc(256 * sizeof(const Expr *));
    if (!stack) {
        fprintf(stderr, "tur: oom\n");
        abort();
    }
    int sp = 0;
    stack[sp++] = body;

    while (sp > 0) {
        const Expr *cur = stack[--sp];
        if (!cur) continue;

        if (cur->kind == EX_HANDLE || cur->kind == EX_HANDLER_LIT) {
            HandleExpr *h = cur->as.handle_.handle;
            if (h) {
                for (uint8_t ci = 0; ci < h->n_cases; ci++) {
                    /* A case that drops the tracked field -> reject. Reuse the
                     * (non-descending) field-consume check on the case body. */
                    if (is_field_consumed(h->cases[ci].body, binding, field_idx)) {
                        free(stack);
                        return true;
                    }
                }
                /* Keep walking into the handled body -- a nested handle there
                 * could also consume the field. */
                if (sp < 250) stack[sp++] = h->body;
            }
            continue;
        }

        switch (cur->kind) {
            case EX_LET:
            case EX_LETREC:
                for (uint32_t i = cur->as.let_.n; i > 0; i--)
                    stack[sp++] = cur->as.let_.bindings[i-1].init;
                stack[sp++] = cur->as.let_.body;
                break;
            case EX_IF:
                if (cur->as.if_.else_or_null) stack[sp++] = cur->as.if_.else_or_null;
                stack[sp++] = cur->as.if_.then_;
                stack[sp++] = cur->as.if_.cond;
                break;
            case EX_DO:
                for (uint32_t i = cur->as.do_.n; i > 0; i--)
                    stack[sp++] = cur->as.do_.items[i-1];
                break;
            case EX_CALL:
                for (uint32_t i = cur->as.call_.n_args; i > 0; i--)
                    stack[sp++] = cur->as.call_.args[i-1];
                break;
            case EX_BUILTIN:
                for (uint32_t i = cur->as.builtin.n; i > 0; i--)
                    stack[sp++] = cur->as.builtin.args[i-1];
                break;
            case EX_DEFER:
                stack[sp++] = cur->as.defer_.body;
                break;
            case EX_WHILE:
                stack[sp++] = cur->as.while_.body;
                stack[sp++] = cur->as.while_.cond;
                break;
            case EX_SET:
                stack[sp++] = cur->as.set_.value;
                break;
            default:
                break;
        }
    }

    free(stack);
    return false;
}

void elab_init_state(Elab *e, Arena *arena, SymbolTable *st) {
    /* Zero everything first so any bool/pointer field not explicitly
     * initialised below (e.g. `in_stdlib_load`, set later by
     * elaborate_program once stdlib_prefix is known) doesn't read
     * uninitialised stack memory. This used to "work" by luck because
     * the struct layout left those bytes zero on common stacks; the
     * Transitive-RM `user_macros` field shifted the layout and exposed
     * the latent UB. */
    memset(e, 0, sizeof(*e));
    e->arena = arena;
    e->st = st;
    scope_init(&e->global, NULL);
    e->scope = &e->global;
    e->next_id = 0;
    e->next_stdlib_id = ELAB_STDLIB_ID_BASE;
    e->ids_stdlib = false;
    e->next_gensym_id = 0;  /* Phase 6 */
    /* RT1: refinement obligation vector (empty unless `refined` is on). */
    refine_obligations_init(&e->refine_obs, arena);
    /* Transitive-RM: driver overrides this via elaborate_program's
     * user_macros param when there's a shared registry to thread into
     * module loaders. */
    e->user_macros = NULL;
    /* Phase 3: file-scope defs collection */
    e->file_scope_defs = NULL;
    e->n_file_scope_defs = 0;
    e->cap_file_scope_defs = 0;
    /* Phase 15: Typeclass environment */
    typeclass_env_init(&e->typeclass_env, arena);
    /* Phase 19: Effect environment */
    e->effect_env = effect_env_new(arena);

    e->sym_def       = intern_cstr(st, "def");
    e->sym_define    = intern_cstr(st, "define");
    e->sym_let       = intern_cstr(st, "let");
    e->sym_letstar   = intern_cstr(st, "let*");
    e->sym_letrec    = intern_cstr(st, "letrec");
    e->sym_if        = intern_cstr(st, "if");
    e->sym_do        = intern_cstr(st, "do");
    e->sym_unsafe    = intern_cstr(st, "unsafe");
    e->sym_when      = intern_cstr(st, "when");
    e->sym_unless    = intern_cstr(st, "unless");
    e->sym_case      = intern_cstr(st, "case");
    e->sym_set       = intern_cstr(st, "set!");
    e->sym_while     = intern_cstr(st, "while");
    e->sym_defn      = intern_cstr(st, "defn");
    e->sym_fn        = intern_cstr(st, "fn");
    e->sym_c_fn      = intern_cstr(st, "c-fn");
    e->sym_lambda    = intern_cstr(st, "\xce\xbb"); /* λ */
    e->sym_extern_c  = intern_cstr(st, "extern-c");
    e->sym_caret_mut     = intern_cstr(st, "^mut");
    e->sym_caret_private = intern_cstr(st, "^private");
    /* Phase P3: HAMT lowering */
    e->sym_caret_persistent = intern_cstr(st, "^persistent");
    /* LT0: Linear types */
    e->sym_caret_linear = intern_cstr(st, "^linear");
    /* UT0: Uniqueness types */
    e->sym_caret_unique = intern_cstr(st, "^unique");
    /* ST0: Substructural types */
    e->sym_caret_affine    = intern_cstr(st, "^affine");
    e->sym_caret_relevant  = intern_cstr(st, "^relevant");
    e->sym_caret_atomic   = intern_cstr(st, "^atomic");   /* G4a */
    e->sym_caret_thread_local = intern_cstr(st, "^thread-local"); /* G4b */
    e->sym_caret_deferred_init = intern_cstr(st, "^deferred-init");
    e->sym_deferred_init       = intern_cstr(st, "__tur-deferred-init__");
    /* LB1: ^borrow -- non-consuming linear/affine handle accessor parameter */
    e->sym_caret_borrow    = intern_cstr(st, "^borrow");
    e->sym_caret_fat       = intern_cstr(st, "^fat");  /* A#1: fat-closure param */
    e->sym_caret_extends   = intern_cstr(st, "^extends");  /* ET4: effect hierarchy */
    e->sym_caret_capability = intern_cstr(st, "^capability");  /* stdlib-effect-rows */
    e->sym_caret_multishot        = intern_cstr(st, "^multishot");        /* MS1 */
    e->sym_caret_deprecated       = intern_cstr(st, "^deprecated");       /* F4 */
    e->sym_caret_reflect          = intern_cstr(st, "^reflect");          /* reflected-measures RF0 */
    e->sym_map_new = intern_cstr(st, "map-new");
    e->sym_assoc = intern_cstr(st, "assoc");
    e->sym_dissoc = intern_cstr(st, "dissoc");
    e->sym_map_get = intern_cstr(st, "get");
    e->sym_map_has = intern_cstr(st, "has?");
    e->sym_map_count = intern_cstr(st, "count");
    e->sym_map_merge = intern_cstr(st, "merge");
    /* HAMT function symbols for lowering */
    e->sym_hamt_new = intern_cstr(st, "hamt/new");
    e->sym_hamt_set = intern_cstr(st, "hamt/set");
    e->sym_hamt_del = intern_cstr(st, "hamt/del");
    e->sym_hamt_get = intern_cstr(st, "hamt/get");
    e->sym_hamt_has = intern_cstr(st, "hamt/has?");
    e->sym_hamt_count = intern_cstr(st, "hamt/count");
    e->sym_hamt_merge = intern_cstr(st, "hamt/merge");
    e->sym_hamt_hash_ptr = intern_cstr(st, "hamt/hash-ptr");
    e->sym_hamt_set_cstr = intern_cstr(st, "hamt/set-cstr");
    e->sym_hamt_del_cstr = intern_cstr(st, "hamt/del-cstr");
    e->sym_hamt_get_cstr = intern_cstr(st, "hamt/get-cstr");
    e->sym_hamt_has_cstr = intern_cstr(st, "hamt/has-cstr?");
    e->sym_defer     = intern_cstr(st, "defer");
    e->sym_return    = intern_cstr(st, "return");
    e->sym_tailcall  = intern_cstr(st, "^tailcall");
    /* Phase 5 */
    e->sym_ref       = intern_cstr(st, "ref");
    e->sym_deref     = intern_cstr(st, "deref");
    e->sym_drop      = intern_cstr(st, "drop!");
    e->kw_else       = intern_cstr(st, "else");
    e->kw_derive     = intern_cstr(st, "as");
    /* LT3: lref<T> — linear owning pointer */
    e->sym_lref     = intern_cstr(st, "lref");
    e->sym_lref_new = intern_cstr(st, "lref/new");
    /* Phase 9: rc<T> + weak<T> */
    e->sym_rc_of = intern_cstr(st, "rc/of");
    e->sym_rc_clone = intern_cstr(st, "rc/clone");
    e->sym_rc_drop = intern_cstr(st, "rc/drop");
    e->sym_rc_ptr = intern_cstr(st, "rc->ptr");
    e->sym_rc_strong_count = intern_cstr(st, "rc/strong-count");
    e->sym_rc_from_ref = intern_cstr(st, "rc/from-ref");
    e->sym_ref_from_rc = intern_cstr(st, "ref/from-rc");
    e->sym_weak = intern_cstr(st, "weak");
    e->sym_upgrade = intern_cstr(st, "upgrade");
    e->sym_weak_pred = intern_cstr(st, "weak?");
    e->sym_ref_pred  = intern_cstr(st, "ref?");
    /* Phase 18: Delimited continuations */
    e->sym_reset = intern_cstr(st, "reset");
    e->sym_shift = intern_cstr(st, "shift");
    e->sym_shift0 = intern_cstr(st, "shift0");
    e->sym_call_cc = intern_cstr(st, "call/cc");
    e->sym_escape = intern_cstr(st, "escape");
    /* Phase B2: Cloneable continuations */
    e->sym_cloneable_reset = intern_cstr(st, "cloneable-reset");
    e->sym_cloneable_shift = intern_cstr(st, "cloneable-shift");
    e->sym_call_cc_star = intern_cstr(st, "call/cc*");
    /* Phase 21: Serializable continuations */
    e->sym_serial_reset = intern_cstr(st, "serial-reset");
    e->sym_serial_shift = intern_cstr(st, "serial-shift");
    /* Phase 19: Algebraic effects */
    e->sym_defeffect = intern_cstr(st, "defeffect");
    e->sym_perform = intern_cstr(st, "perform");
    e->sym_handle = intern_cstr(st, "handle");
    e->sym_handle_shallow = intern_cstr(st, "handle-shallow");
    e->sym_try_with = intern_cstr(st, "try-with");
    e->sym_with_handler = intern_cstr(st, "with-handler");
    e->sym_with = intern_cstr(st, "with"); /* WITH-V0 */
    e->sym_resume = intern_cstr(st, "resume");
    e->sym_discontinue = intern_cstr(st, "discontinue");
    e->sym_k = intern_cstr(st, "k");
    e->sym_cont_pred = intern_cstr(st, "cont?");
    e->sym_effect_unsafe = intern_cstr(st, EFFECT_NAME_UNSAFE);
    effect_env_register_builtin_unsafe(e->effect_env, e->arena, e->sym_effect_unsafe);
    effect_env_register_builtin_capabilities(e->effect_env, e->arena, st);
    /* ET3: handler type expression and compose-handlers */
    e->sym_handler_type     = intern_cstr(st, "handler");
    e->sym_compose_handlers = intern_cstr(st, "compose-handlers");
    /* Phase U3: Unsafe primitives - pointer operations */
    e->sym_ptr_deref = intern_cstr(st, "ptr-deref");
    e->sym_ptr_write = intern_cstr(st, "ptr-write");
    e->sym_ptr_add = intern_cstr(st, "ptr-add");
    e->sym_ptr_sub = intern_cstr(st, "ptr-sub");
    e->sym_ptr_nullq = intern_cstr(st, "ptr-null?");
    e->sym_ptr_of = intern_cstr(st, "ptr-of");
    /* Phase U3: Unsafe primitives - type casting */
    e->sym_unsafe_cast = intern_cstr(st, "unsafe-cast");
    e->sym_reinterpret = intern_cstr(st, "reinterpret");
    e->sym_transmute = intern_cstr(st, "transmute");
    /* Phase U3: Unsafe primitives - unchecked array ops */
    e->sym_array_get_unchecked = intern_cstr(st, "array-get-unchecked");
    e->sym_array_set_unchecked = intern_cstr(st, "array-set-unchecked");
    /* Phase U3: Unsafe primitives - raw memory */
    e->sym_raw_malloc = intern_cstr(st, "raw-malloc");
    e->sym_raw_free = intern_cstr(st, "raw-free");
    e->sym_raw_realloc = intern_cstr(st, "raw-realloc");
    e->sym_raw_memcpy = intern_cstr(st, "raw-memcpy");
    e->sym_raw_memset = intern_cstr(st, "raw-memset");
    /* Phase U3: Unsafe primitives - FFI */
    e->sym_c_call = intern_cstr(st, "c-call");
    e->sym_call_ptr = intern_cstr(st, "call-ptr");
    e->sym_callback_ptr = intern_cstr(st, "callback-ptr");
    e->sym_dlopen = intern_cstr(st, "dlopen");
    e->sym_dlsym = intern_cstr(st, "dlsym");
    e->sym_dlclose = intern_cstr(st, "dlclose");
    /* Phase 19 TUR-E0008: Effect scope tracking */
    e->handled_effect_names = NULL;
    e->n_handled_effects = 0;
    e->cap_handled_effects = 0;
    e->fn_body_depth = 0;
    e->unsafe_depth = 0;
    e->macro_expand_depth = 0;
    /* Phase U5: Unsafe linting configuration */
    e->unsafe_max_lines = 20;      /* default threshold */
    e->unsafe_warn_nested = false;  /* disabled by default */
    e->unsafe_require_safety = false;/* disabled by default */
    e->unsafe_stats_enabled = false; /* disabled by default */
    e->unsafe_block_count = 0;
    e->unsafe_total_lines = 0;
    /* Phase 10: GC */
    e->sym_gc_force = intern_cstr(st, "gc!");
    e->sym_gc_enable = intern_cstr(st, "gc-enable!");
    e->sym_gc_disable = intern_cstr(st, "gc-disable!");
    e->sym_gc_auto = intern_cstr(st, "gc-auto!");   /* CG5 */
    /* CG6 */
    e->sym_gc_collections   = intern_cstr(st, "gc-collections");
    e->sym_gc_objects_freed = intern_cstr(st, "gc-objects-freed");
    e->sym_gc_live_blocks   = intern_cstr(st, "gc-live-blocks");
    e->sym_gc_cand_hw       = intern_cstr(st, "gc-candidate-high-water");
    /* Phase 6 */
    e->sym_defmacro = intern_cstr(st, "defmacro");
    e->sym_defmacro_star = intern_cstr(st, "defmacro*");
    e->sym_quote = intern_cstr(st, "quote");
    e->sym_quasiquote = intern_cstr(st, "quasiquote");
    e->sym_unquote = intern_cstr(st, "unquote");
    e->sym_unquote_splicing = intern_cstr(st, "unquote-splicing");
    e->sym_gensym = intern_cstr(st, "gensym");
    e->sym_thread = intern_cstr(st, "->");
    e->sym_thread_last = intern_cstr(st, "->>");
    e->next_gensym_id = 0;  /* Phase 6 */
    /* Phase 11: defstruct */
    e->sym_defstruct = intern_cstr(st, "defstruct");
    e->sym_make_struct = intern_cstr(st, "make-struct");
    /* M2b: (default-of T) — zero-valued T literal */
    e->sym_default_of = intern_cstr(st, "default-of");
    /* SI4-C: defopaque */
    e->sym_defopaque = intern_cstr(st, "defopaque");
    e->kw_copy = intern_cstr(st, "copy");
    e->kw_move = intern_cstr(st, "move");
    e->kw_linear = intern_cstr(st, "linear"); /* LT4 */
    e->kw_affine = intern_cstr(st, "affine");
    e->kw_sealed = intern_cstr(st, "sealed");   /* sealed-opaque experiment */
    e->kw_non_null = intern_cstr(st, "non-null"); /* option-niche eligibility */
    e->kw_heap = intern_cstr(st, "heap");
    e->kw_no_auto_ctor = intern_cstr(st, "no-auto-ctor"); /* CTOR-V0 opt-out */
    /* Phase G0: ADT registry */
    e->adt_defs = NULL;
    e->n_adt_defs = 0;
    e->cap_adt_defs = 0;
    e->sym_defdata = intern_cstr(st, "defdata");
    e->sym_match = intern_cstr(st, "match");
    /* Phase G1: GADT */
    e->sym_defgadt = intern_cstr(st, "defgadt");
    e->sym_colon = intern_cstr(st, ":");
    /* IT0: Union type pipe separator */
    e->sym_pipe = intern_cstr(st, "|");
    /* IT2: Intersection type ampersand separator */
    e->sym_ampersand = intern_cstr(st, "&");
    /* LS1: no signature lifetime context active until a defn opens one */
    e->cur_lifetime_ctx = NULL;
    /* Phase G2: per-arm skolem environment (NULL until inside a GADT match arm) */
    e->g2_skolem_env = NULL;
    /* KB-025: no enclosing fn signature type variables at top level */
    e->n_sig_tyvars = 0;
    /* Phase G3: coerce */
    e->sym_coerce = intern_cstr(st, "coerce");
    /* Phase G3: (~ a b) equality constraint */
    e->sym_tilde = intern_cstr(st, "~");
    /* Phase 12: Borrow traits */
    e->sym_borrow = intern_cstr(st, "&");
    e->sym_borrow_mut = intern_cstr(st, "&mut");
    /* Phase 15: Typeclasses */
    e->sym_defclass = intern_cstr(st, "defclass");
    e->sym_definstance = intern_cstr(st, "definstance");
    /* Phase HKT H5: defkind */
    e->sym_defkind = intern_cstr(st, "defkind");
    /* Phase HKT-P2: defrec */
    e->sym_defrec = intern_cstr(st, "defrec");
    e->sym_deftype = intern_cstr(st, "deftype");
    /* Phase TA1: defalias */
    e->sym_defalias      = intern_cstr(st, "defalias");
    e->type_alias_names  = NULL;
    e->type_alias_kinds  = NULL;
    e->type_alias_types  = NULL;
    e->n_type_aliases    = 0;
    e->cap_type_aliases  = 0;
    /* Phase HKT-P1: type-app */
    e->sym_type_app = intern_cstr(st, "type-app");
    /* Phase HRT0: forall/exists */
    e->sym_forall = intern_cstr(st, "forall");
    e->sym_exists = intern_cstr(st, "exists");
    e->sym_forall_u = intern_cstr(st, "\xe2\x88\x80"); /* ∀ */
    e->sym_exists_u = intern_cstr(st, "\xe2\x88\x83"); /* ∃ */
    /* Phase HRT1: -> and :: */
    e->sym_arrow   = intern_cstr(st, "->");
    e->sym_ascribe = intern_cstr(st, "::");
    /* Phase HRT2: pack and open */
    e->sym_pack = intern_cstr(st, "pack");
    e->sym_open = intern_cstr(st, "open");
    /* Phase HKT (v2): reserved typeclass names */
    e->sym_hkt_Functor      = intern_cstr(st, "Functor");
    e->sym_hkt_Applicative  = intern_cstr(st, "Applicative");
    e->sym_hkt_Monad        = intern_cstr(st, "Monad");
    e->sym_hkt_Traversable  = intern_cstr(st, "Traversable");
    e->sym_hkt_Foldable     = intern_cstr(st, "Foldable");
    /* Phase R2: Panic */
    e->sym_panic = intern_cstr(st, "panic");
    e->sym_panic_with = intern_cstr(st, "panic-with");
    e->sym_catch_unwind = intern_cstr(st, "catch-unwind");
    e->sym_catch_panic_of = intern_cstr(st, "catch-panic-of");
    e->sym_panic_payload_type = intern_cstr(st, "panic-payload-type");
    /* Phase R5: no-unwind attribute */
    e->sym_no_unwind_attr = intern_cstr(st, "#no-unwind");
    /* #[used] attribute: retain a defn with external C linkage */
    e->sym_used_attr = intern_cstr(st, "#used");
    /* Phase M6: (export-as "c_name") attribute head symbol */
    e->sym_export_as_attr = intern_cstr(st, "export-as");
    /* M2a: `^construct` polymorphic-constructor synthesis marker, and M5
     * residual-straddle: `^byval` prefer-byvalue-spec marker.  Both used to
     * be spelled inside the effect row (^construct / ^byval);
     * effect-row-honesty-plan W0 moved them onto the defn's attributes. */
    e->sym_caret_construct = intern_cstr(st, "^construct");
    e->sym_caret_byval     = intern_cstr(st, "^byval");
    /* W0: `(match ^non-exhaustive x ...)`, and the deprecated spelling it
     * replaces, `(match #fx{NonExhaustive} x ...)` (TUR-D0004). */
    e->sym_caret_non_exhaustive = intern_cstr(st, "^non-exhaustive");
    e->sym_non_exhaustive_legacy = intern_cstr(st, "NonExhaustive");
    e->sym_panic_payload_value = intern_cstr(st, "panic-payload-value");
    e->sym_panic_payload_file = intern_cstr(st, "panic-payload-file");
    e->sym_panic_payload_line = intern_cstr(st, "panic-payload-line");
    e->sym_panic_payload_downcast = intern_cstr(st, "panic-payload-downcast");
    /* Phase R1: ? operator */
    e->sym_question = intern_cstr(st, "?");
    /* Phase T19-B: thread-spawn */
    e->sym_thread_spawn = intern_cstr(st, "thread-spawn");
    /* Phase T21-F: async/await */
    e->sym_async = intern_cstr(st, "async");
    e->sym_await = intern_cstr(st, "await");
    /* Phase SEL1: fair multi-channel select */
    e->sym_select = intern_cstr(st, "select");
    e->sym_recv   = intern_cstr(st, "recv");
    e->sym_send   = intern_cstr(st, "send");
    /* Phase 20: Software Transactional Memory */
    e->sym_stm = intern_cstr(st, "stm");
    e->sym_atomically = intern_cstr(st, "atomically");
    e->sym_retry = intern_cstr(st, "retry");
    e->sym_check = intern_cstr(st, "check");
    e->sym_or_else = intern_cstr(st, "or-else");
    e->sym_tvar = intern_cstr(st, "tvar");
    e->sym_new = intern_cstr(st, "new");
    e->sym_read = intern_cstr(st, "read");
    e->sym_write = intern_cstr(st, "write");
    e->sym_modify = intern_cstr(st, "modify");
    e->sym_swap = intern_cstr(st, "swap");
    e->sym_cas = intern_cstr(st, "cas");
    e->sym_tmvar = intern_cstr(st, "tmvar");
    e->sym_tchan = intern_cstr(st, "tchan");
    e->sym_tsem = intern_cstr(st, "tsem");
    e->sym_dosync = intern_cstr(st, "dosync");
    e->sym_with_tvar = intern_cstr(st, "with-tvar");
    /* Legacy symbols for tvar/ syntax */
    e->sym_tvar_new = intern_cstr(st, "tvar/new");
    e->sym_tvar_read = intern_cstr(st, "tvar/read");
    e->sym_tvar_write = intern_cstr(st, "tvar/write");
    e->sym_tvar_modify = intern_cstr(st, "tvar/modify");
    e->sym_tvar_swap = intern_cstr(st, "tvar/swap");
    e->sym_tvar_cas = intern_cstr(st, "tvar/cas");
    /* Macro storage */
    e->macros = NULL;
    e->n_macros = 0;
    e->cap_macros = 0;
    /* Phase M0: Module system */
    e->sym_defmodule = intern_cstr(st, "defmodule");
    e->sym_export = intern_cstr(st, "export");
    e->sym_export_from = intern_cstr(st, "export-from");
    e->sym_effect = intern_cstr(st, "effect");
    e->sym_export_mut = intern_cstr(st, "mut");   /* G3: (export (mut g)) */
    e->sym_import = intern_cstr(st, "import");
    e->sym_load = intern_cstr(st, "load");
    e->sym_as = intern_cstr(st, "as");   /* Phase N: (as Type expr) cast */
    e->sym_type_of = intern_cstr(st, "type-of");  /* IT4: (type-of x) */
    e->sym_cast    = intern_cstr(st, "cast");      /* IT4: (cast x T) */
    e->sym_is_q    = intern_cstr(st, "is?");       /* TY3: (is? x T) type test */
    e->kw_as = intern_cstr(st, "as");   /* same interned symbol, used as :as keyword */
    e->kw_refer = intern_cstr(st, "refer");
    e->kw_for_macros = intern_cstr(st, "for-macros"); /* Stage 3 macro-time imports */
    e->has_defmodule = false;
    e->current_module_name = NULL;
    e->current_module = NULL;
    /* Phase M2: Module registry */
    e->module_base_dir = ".";
    e->loaded_modules = NULL;
    e->n_loaded_modules = 0;
    e->cap_loaded_modules = 0;
    /* Import/load file ids start past every file already registered.  The
     * old fixed 10 assumed "0-9 reserved for main + stdlib files", but the
     * compiled driver numbers its ~40 auto-loaded stdlib files from 1, so a
     * `(load ...)` or module import re-registered id 10 over an auto-loaded
     * file's SourceFile -- diagnostics in that stdlib file then named the
     * wrong file, and when the loaded file was `#lang r7rs` the stdlib
     * file's forms were lowered as Scheme (found by r7rs-lang-plan R7).  The
     * driver records where its band ends; every Elab starts there (so the
     * compile-time macro evaluator's sessions reuse one range, as they did
     * at 10). */
    e->next_import_file_id = 10;
    if (diag_autoload_file_ids_end() > e->next_import_file_id)
        e->next_import_file_id = diag_autoload_file_ids_end();
    e->separate_compilation = false;
    e->in_imported_module = false;
    e->macro_expansion_module = NULL;
    e->cloneable_reset_depth = 0;
    e->serial_reset_depth = 0;
    /* Phase EX1d: existential `open` skolem tracking. */
    e->open_skolem_depth = 0;
    e->open_skolem_next  = 0;
    /* Phase P3: HAMT lowering */
    e->needs_hamt = false;
    /* PR5-3-D: referred effects */
    e->referred_effects     = NULL;
    e->n_referred_effects   = 0;
    e->cap_referred_effects = 0;
    /* Phase PKG-1: extra module search dirs (-I) */
    e->module_include_dirs = NULL;
    e->n_module_include_dirs = 0;
    /* LS2: workspace-sibling provenance + warning dedup */
    e->module_include_workspace_producer = NULL;
    e->module_include_warned             = NULL;
    e->module_consumer_declared_spices   = NULL;
    e->n_module_consumer_declared_spices = 0;
    /* Phase RF0: forward type declaration tracking */
    e->forward_type_syms = NULL;
    e->n_forward_type_syms = 0;
    e->cap_forward_type_syms = 0;
    /* CT0: Contract keyword symbols */
    e->kw_pre  = intern_cstr(st, "pre");
    e->kw_post = intern_cstr(st, "post");
    e->kw_invariant = intern_cstr(st, "invariant");
    e->sym_result             = intern_cstr(st, "result");
    e->sym_tur_contract_check = intern_cstr(st, "tur-contract-check");
    e->sym_tur_contract_check_at = intern_cstr(st, "tur-contract-check-at");
    /* DV0: Dynamic vars */
    e->sym_defdynamic    = intern_cstr(st, "defdynamic");
    e->sym_binding       = intern_cstr(st, "binding");
    e->dynvar_entries    = NULL;
    e->n_dynvars         = 0;
    e->cap_dynvars       = 0;
    e->active_dynvar_bindings     = NULL;
    e->n_active_dynvar_bindings   = 0;
    e->cap_active_dynvar_bindings = 0;
    /* SS0b: Session type constructor symbols (capitalized, appear in type positions) */
    e->sym_session_type  = intern_cstr(st, "Session");
    e->sym_session_Send  = intern_cstr(st, "Send");
    e->sym_session_Recv  = intern_cstr(st, "Recv");
    e->sym_session_Close = intern_cstr(st, "Close");
    e->sym_session_Choose = intern_cstr(st, "Choose");
    e->sym_session_Branch = intern_cstr(st, "Branch");
    e->sym_session_Rec    = intern_cstr(st, "Rec");
    e->sym_session_Timeout = intern_cstr(st, "Timeout");
    /* SS0b: Session channel operation symbols (lowercase, appear in expression position) */
    e->sym_close        = intern_cstr(st, "close");
    e->sym_offer        = intern_cstr(st, "offer");
    e->sym_choose_left  = intern_cstr(st, "choose-left");
    e->sym_choose_right = intern_cstr(st, "choose-right");
    e->sym_make_session = intern_cstr(st, "make-session");
    e->sym_recv_timeout = intern_cstr(st, "recv-timeout");
    /* SS3a: Initialize session Rec label stack */
    e->rec_depth = 0;
    /* SS5: Global protocol symbols */
    e->sym_defprotocol  = intern_cstr(st, "defprotocol");
    e->sym_make_protocol = intern_cstr(st, "make-protocol");
    e->sym_send_to       = intern_cstr(st, "send-to");
    e->sym_recv_from     = intern_cstr(st, "recv-from");
    e->sym_recv_timeout_from = intern_cstr(st, "recv-timeout-from");
    e->sym_global_type   = intern_cstr(st, "Global");
    e->sym_role_type     = intern_cstr(st, "Role");
    e->sym_project_type  = intern_cstr(st, "project");
    e->global_protocols  = NULL;
    e->n_global_protocols = 0;
    e->cap_global_protocols = 0;
    /* GF1: Generator forms */
    e->sym_gen       = intern_cstr(st, "gen");
    e->sym_yield     = intern_cstr(st, "yield");
    e->sym_gen_next  = intern_cstr(st, "gen-next");
    e->sym_gen_done  = intern_cstr(st, "gen-done?");
    e->sym_gen_unwrap = intern_cstr(st, "gen-unwrap");
    e->gen_ctx       = NULL;
    e->gen_counter   = 0;
    /* CLI-ARGS: Pre-declare *args* as a global :int binding backed by g_tur_args.
     * Scripts access CLI arguments (after --) via *args* as a list of :cstr values
     * represented as int64_t pointers (same layout as stdlib/list.tur cons cells). */
    {
        const Symbol *sym_args = intern_cstr(st, "*args*");
        Binding *b_args = binding_new(e, sym_args, TYPE_INT, false, true, SPAN_UNKNOWN);
        b_args->c_export_name = "g_tur_args";
        scope_add(&e->global, b_args);
        /* *argv0*: the program's own name (argv[0] of a compiled binary, the
         * script path under the interpreter), a :cstr global backed by
         * g_tur_argv0.  *args* keeps its meaning -- the arguments only --
         * so this is a second value, not a change to the list
         * (r7rs-command-line-first-element-is-tur). */
        const Symbol *sym_argv0 = intern_cstr(st, "*argv0*");
        Binding *b_argv0 = binding_new(e, sym_argv0, TYPE_CSTR, false, true, SPAN_UNKNOWN);
        b_argv0->c_export_name = "g_tur_argv0";
        scope_add(&e->global, b_argv0);
    }
}

/* Phase 13: Lifetime annotation helpers (deferred - infrastructure in place) */
/* Phase 15: Typeclass cached symbols */

/* Phase 6: Macro lookup */
/* PS4: earlier-turn tests.  Each compares against the watermark taken when
 * the call began (elaborate_program_session), so they are all false for a
 * compile and for a session's first turn. */
bool elab_prior_turn_global(const Elab *e, const Binding *b) {
    if (!e->turn_continues_session || !b) return false;
    for (uint32_t i = 0; i < e->turn_start_n_globals && i < e->global.n; i++)
        if (e->global.bindings[i] == b) return true;
    return false;
}

bool elab_prior_turn_macro(const Elab *e, const MacroDef *m) {
    if (!e->turn_continues_session || !m) return false;
    for (uint32_t i = 0; i < e->turn_start_n_macros && i < e->n_macros; i++)
        if (e->macros[i] == m) return true;
    return false;
}

bool elab_prior_turn_adt(const Elab *e, const AdtDef *ad) {
    if (!e->turn_continues_session || !ad) return false;
    for (uint32_t i = 0; i < e->turn_start_n_adt_defs && i < e->n_adt_defs; i++)
        if (e->adt_defs[i] == ad) return true;
    return false;
}

bool elab_prior_turn_effect(const Elab *e, const Symbol *name) {
    if (!e->turn_continues_session || !e->effect_env) return false;
    for (uint32_t i = 0; i < e->turn_start_n_effects && i < e->effect_env->n_effects; i++)
        if (e->effect_env->effects[i]->name == name) return true;
    return false;
}

bool elab_prior_turn_instance(const Elab *e, const TypeClassInstance *inst) {
    if (!e->turn_continues_session || !inst) return false;
    /* The list is prepended to, so everything reachable from the head the
     * turn started with is older than the turn. */
    for (const TypeClassInstance *p = e->turn_start_instances; p; p = p->next)
        if (p == inst) return true;
    return false;
}

bool elab_file_is_stdlib(uint16_t file_id) {
    const char *path = diag_file_path(file_id);
    return path && strstr(path, "stdlib/") != NULL;
}

void elab_remove_macro(Elab *e, MacroDef *m) {
    for (uint32_t i = 0; i < e->n_macros; i++) {
        if (e->macros[i] != m) continue;
        memmove(&e->macros[i], &e->macros[i + 1],
                (e->n_macros - i - 1) * sizeof(MacroDef *));
        e->n_macros--;
        if (i < e->turn_start_n_macros) e->turn_start_n_macros--;
        return;
    }
}

MacroDef *elab_lookup_macro(Elab *e, const Symbol *name) {
    for (uint32_t i = 0; i < e->n_macros; i++) {
        MacroDef *m = e->macros[i];
        if (m->name != name) continue;
        /* Phase M4: visibility rules.
         * - is_referred: injected via :refer — always visible.
         * - defining_module_name == NULL: stdlib/pre-module — always visible.
         * - defining_module_name == current module: visible within the defining module.
         * - defining_module_name appears anywhere on the macro-expansion stack:
         *   private helper called transitively from an exported macro of that
         *   module. Cross-module wrapper-macro bug fix: previously only the
         *   innermost expansion module was checked, so an outer macro M in
         *   module A whose expansion went through a stdlib wrapper W lost
         *   visibility of A's private helpers (e.g. recursive `fold-len`
         *   companion) during W's body re-elaboration -- the helper fell back
         *   to being elaborated as a regular function call, and any vec
         *   argument hit the data-literals lowering with unbound symbols.
         *   See docs/archive/history/cross-module-wrapper-macro-vec-arg-elaborated-as-expression.md. */
        if (m->is_referred) return m;
        if (m->defining_module_name == NULL) return m;
        if (m->defining_module_name == e->current_module_name) return m;
        /* The `tur/` namespace is implicitly imported everywhere (see the
         * stdlib/macros.tur header: "Macros here are globally visible without
         * an explicit import because the tur/ namespace is implicitly
         * imported").  The end-of-stdlib M7 promotion sweep (elab_toplevel.c)
         * eventually rewrites every `tur/`-module macro's defining_module_name
         * to NULL, but that sweep fires only once, at the stdlib/user boundary.
         * On the interpreter's incremental re-elaboration a stdlib file that
         * uses a tur/macros macro (e.g. typeclass-show.tur's `when`) can sit
         * *inside* the accumulated stdlib prefix while a later turn's user form
         * is the new tail -- so the boundary, and thus the promotion, lands
         * after the using file, and the macro is invisible during its
         * elaboration (it then falls back to a runtime-dispatch call, spamming
         * TUR-W0040).  Honour the implicit tur/ import directly at lookup time
         * so visibility no longer depends on the sweep having already run.
         * See docs/archive/tur-macros-invisible-across-stdlib-reelab.md. */
        if (m->defining_module_name->len >= 4 &&
            memcmp(m->defining_module_name->name, "tur/", 4) == 0)
            return m;
        for (uint32_t k = 0; k < e->n_macro_expansion_stack; k++) {
            if (m->defining_module_name == e->macro_expansion_stack[k]) return m;
        }
    }
    return NULL;
}

Binding *binding_new(Elab *e, const Symbol *name, Type type,
                            bool is_mut, bool is_global, Span span) {
    Binding *b = (Binding *)arena_alloc(e->arena, sizeof(Binding));
    memset(b, 0, sizeof(Binding));
    b->name = name;
    b->type = type;
    b->is_mut = is_mut;
    b->is_global = is_global;
    b->id = elab_fresh_id(e);
    b->span = span;
    /* TY4: stamp the lexical scope depth at which this binding is introduced,
     * walked from the live scope chain.  The borrow-escape check compares a
     * borrow referent's depth against where the borrow lands: a borrow may not
     * outlive its referent (flow into a shallower/longer-lived binding or out
     * of the function frame). */
    {
        uint32_t depth = 0;
        for (const Scope *s = e->scope; s; s = s->parent) depth++;
        b->scope_depth = depth;
    }
    b->closure_fn_binding = NULL;
    b->returns_closure_fn_binding = NULL;
    b->widen_fn_alias = NULL;
    b->is_moved = false;  /* Phase 5: move semantics */
    b->moved_at = SPAN_UNKNOWN;
    b->no_unwind = false;  /* Phase R5: #[no-unwind] attribute */
    b->retain_c_linkage = false;  /* #[used] attribute */
    b->is_exported = false;
    b->defining_module_name = e->current_module_name;
    b->c_export_name = NULL;  /* Phase M6: ^:export-as C name */
    /* MF3: mark global bindings created during stdlib auto-load so user
     * code that later shadows them gets a hard diagnostic. */
    b->is_from_stdlib = is_global && e->in_stdlib_load;
    return b;
}

/* Gap 1 (instance-method-return-not-unified): see elab_internal.h. */
bool return_type_nominal_conflict(const AdtDef *ret_adt, Type body) {
    if (!ret_adt) return false;
    const AdtDef *ba = (body.kind == TY_ADT) ? body.as.adt_.def : NULL;
    /* A non-nominal body (primitive, opaque-int carrier, tyvar, applied type,
     * unknown/inline-C) is tolerated: those are exactly the int64 carrier /
     * by-value bridges the ABI relies on, not a soundly-rejectable mismatch.
     * (structdef-retirement DS-D: the former StructDef ret_struct arm is gone --
     * every former struct is a record ADT.) */
    if (!ba) return false;
    return ba != ret_adt;   /* different ADT, or a non-ADT nominal body */
}

/* float-register-class-returns: see elab_internal.h. */
static bool rc_is_float_kind(TypeKind k) {
    return k == TY_FLOAT || k == TY_FLOAT32 || k == TY_FLOAT64;
}
/* A kind whose register class is not yet pinned to a concrete machine class:
 * a type variable (could resolve to anything), an inferred/unknown placeholder,
 * the bottom type `!`, or the boxed `any` carrier.  Tolerated on either side --
 * none is a concrete cross-register-class result. */
static bool rc_is_unpinned_kind(TypeKind k) {
    return k == TY_TYVAR || k == TY_UNKNOWN || k == TY_NEVER || k == TY_ANY;
}
bool return_type_register_class_conflict(TypeKind declared, Type body) {
    bool decl_float = rc_is_float_kind(declared);
    bool body_float = rc_is_float_kind(body.kind);
    /* Same register class (both float or both non-float): no cross-class clash.
     * Same-GP-register non-float mismatches (cstr-where-int, bare-int-where-
     * handle) are the deliberate int64 carrier bridges -- left to a future
     * carrier-aware return unification, not this register-class check. */
    if (decl_float == body_float) return false;
    /* Exactly one side is float (always concrete); the other must be a concrete,
     * register-pinned non-float to be a true cross-class result. */
    if (rc_is_unpinned_kind(declared) || rc_is_unpinned_kind(body.kind))
        return false;
    return true;
}
bool rc_widen_int_literal_to_float_return(TypeKind declared, Expr *body) {
    if (!body || !rc_is_float_kind(declared) || body->kind != EX_INT_LIT)
        return false;
    body->as.f = (double)body->as.i;
    body->kind = EX_FLOAT_LIT;
    body->type = type_from_kind(declared);
    return true;
}

/* float32-block-temp-widens-to-double: see elab_internal.h. */
static bool rc_narrow_block_tail_float_literal(TypeKind declared, Expr *x, int depth) {
    if (!x || depth > 64) return false;
    switch (x->kind) {
        case EX_FLOAT_LIT:
            if (x->type.kind != TY_FLOAT) return false;   /* already at a width */
            x->type = type_from_kind(declared);
            return true;
        case EX_DO: {
            if (x->as.do_.n == 0) return false;
            Expr *last = x->as.do_.items[x->as.do_.n - 1];
            if (!rc_narrow_block_tail_float_literal(declared, last, depth + 1))
                return false;
            if (x->type.kind == TY_FLOAT) x->type = type_from_kind(declared);
            return true;
        }
        case EX_LET:
        case EX_LETREC:
            if (!rc_narrow_block_tail_float_literal(declared, x->as.let_.body, depth + 1))
                return false;
            if (x->type.kind == TY_FLOAT) x->type = type_from_kind(declared);
            return true;
        case EX_IF: {
            if (!x->as.if_.else_or_null) return false;
            bool t = rc_narrow_block_tail_float_literal(declared, x->as.if_.then_, depth + 1);
            bool el = rc_narrow_block_tail_float_literal(declared, x->as.if_.else_or_null, depth + 1);
            if (!t && !el) return false;
            if (x->type.kind == TY_FLOAT) x->type = type_from_kind(declared);
            return true;
        }
        default:
            return false;
    }
}
bool rc_narrow_float_literal_tail_to_declared_return(TypeKind declared, Expr *body) {
    if (!body || (declared != TY_FLOAT32 && declared != TY_FLOAT64)) return false;
    if (body->kind == EX_FLOAT_LIT) return false;   /* the bare literal is fine as is */
    return rc_narrow_block_tail_float_literal(declared, body, 0);
}

/* pointer-vs-scalar-returns: see elab_internal.h. */
static bool ps_is_integer_scalar_kind(TypeKind k) {
    switch (k) {
        case TY_BOOL:
        case TY_INT:
        case TY_INT8:  case TY_INT16:  case TY_INT32:  case TY_INT64:
        case TY_UINT8: case TY_UINT16: case TY_UINT32: case TY_UINT64:
            return true;
        default:
            return false;
    }
}
bool return_type_pointer_scalar_conflict(TypeKind declared, Type body) {
    /* Only the commit direction (declared cstr, integer body) is sound: the
     * reverse is the int64 carrier-handle bridge, left to a carrier-aware
     * unification. */
    if (declared != TY_CSTR) return false;
    return ps_is_integer_scalar_kind(body.kind);
}

/* carrier-aware-return-unification Phase 2: the REVERSE pointer-scalar
 * direction -- a concrete integer-family declared return with a concrete `cstr`
 * body.  This is the carrier-handle bridge that the commit-direction helper
 * above deliberately tolerates, so it is sound ONLY for a genuinely committed
 * position (a monomorphic non-`#{Unsafe}` defn that does not participate in the
 * carrier).  The dispatcher gates it on RET_CLASS_COMMITTED; the helper itself
 * just recognises the shape. */
bool return_type_pointer_scalar_reverse_conflict(TypeKind declared, Type body) {
    if (!ps_is_integer_scalar_kind(declared)) return false;
    return body.kind == TY_CSTR;
}

/* nil-tail-not-checked-against-declared-return: a `nil`-typed body under a
 * declared non-nil return.  The dispatcher's other predicates are each targeted
 * at one confusable PAIR of types, and TY_NIL had no predicate at all -- so
 * `(defn f [] : int nil)` was accepted and returned 0, while every other wrong
 * tail (`"hello"`, `1.5`, `true`) was already rejected.
 *
 * Worth more than its repro's arity suggests: `nil` is what a lot of things
 * collapse to.  `defmacro` / `defclass` / `deftype` all elaborate to
 * EX_NIL_LIT once registered, and a void-returning call (`println`) is nil-typed
 * too, so this hole is what made OTHER mistakes silent -- a missing close paren
 * that swallowed the real tail among them.
 *
 * The caller decides `checkable`, and it carries TWO facts this predicate
 * cannot see:
 *
 *  - **The return was written down.** `return_kind` is initialized to TY_NIL, so
 *    an unannotated function and an explicit `: void` are the same TypeKind
 *    here.  An unannotated function infers its return from its body and must not
 *    be held to a return it never declared.
 *  - **The tail is a nil LITERAL** (EX_NIL_LIT), not merely nil-TYPED.  This is
 *    the deliberate scope line.  Widening to any nil-typed tail also rejects
 *    `(defn main [] : int (println ...))` -- a void call in tail position -- which
 *    is the idiomatic entry point here and 25 fixtures in this corpus.  Whether
 *    that shape should be an error is a real question, but a much larger one
 *    than this check, and the reported defect does not need it: the shapes that
 *    made this hole expensive (`defmacro` / `defclass` / `deftype` collapsing to
 *    EX_NIL_LIT once registered, a missing close paren swallowing the real tail)
 *    all land on the literal.  See the report's "What narrows it" section.
 *
 * `: void` / `: nil` keeps accepting a nil tail either way -- that is this
 * predicate's own job, below. */
bool return_type_nil_body_conflict(TypeKind declared, Type body,
                                   bool checkable) {
    if (!checkable) return false;
    if (body.kind != TY_NIL) return false;
    if (declared == TY_NIL) return false;   /* `: void` / `: nil` -- correct */
    /* A tyvar return is inferred or substituted later; TY_ANY absorbs anything;
     * TY_NEVER is unreachable-typed.  None of the three is a commitment this
     * check can hold the body to. */
    if (declared == TY_TYVAR || declared == TY_ANY || declared == TY_NEVER)
        return false;
    return true;
}

/* carrier-aware-return-unification Phase 2b: a `bool`-vs-non-bool-integer return
 * mismatch.  `bool` and the integer family share the int64 0/1 representation,
 * so the carrier ABI cannot see the swap -- but the language already treats them
 * as distinct everywhere else (a `(let [b : bool 1] ...)` binding is rejected;
 * boolean constants are `true`/`false`, not `0`/`1`).  In a genuinely committed
 * position there is no carrier to bridge, so a `bool` declared with a non-bool
 * integer body (or the reverse) is a real type mismatch.  Fires iff EXACTLY ONE
 * side is `bool` and the other is a concrete non-bool integer-family scalar; the
 * dispatcher gates it on RET_CLASS_COMMITTED. */
static bool bi_is_nonbool_integer_kind(TypeKind k) {
    return k != TY_BOOL && ps_is_integer_scalar_kind(k);
}
bool return_type_bool_integer_conflict(TypeKind declared, Type body) {
    bool decl_bool = (declared == TY_BOOL);
    bool body_bool = (body.kind == TY_BOOL);
    if (decl_bool == body_bool) return false;  /* both bool / neither: not this */
    return decl_bool ? bi_is_nonbool_integer_kind(body.kind)
                     : bi_is_nonbool_integer_kind(declared);
}

/* carrier-aware-return-unification Phase 2c: an aggregate that does NOT ride the
 * int64 carrier on one side and a carrier scalar on the other.
 *
 * The tolerances above are all calibrated to the int64 carrier: a bare integer
 * where a handle is declared is a real bridge, because both are `int64_t` in the
 * emitted C.  A by-value record ADT is not -- it lowers to a real `tur_adt_S`
 * aggregate (or, when :heap, a typed pointer to one), so there is nothing to
 * bridge and the mismatch reaches `cc` as "incompatible types when returning".
 *
 * Which ADTs those are is not restated here: the question is put to
 * `type_c_name`, the same function codegen uses to spell the C type, so this
 * check cannot drift from what the emitter actually emits.  A transparent int
 * newtype (`defopaque H :int`) and anything else the carrier swallows answer
 * "int64_t" and stay tolerated.
 *
 * TY_APP is deliberately NOT included even though a by-value monomorph
 * (`(Pair int float)`) has a real C name too: a parametric return position has
 * a carrier crossing that grounds it, and `(defn f [x : (Option int)] : int x)`
 * compiles and runs today.  Only the bare, unparameterised record ADT reaches
 * `cc` with "incompatible types when returning". Widening this to TY_APP
 * rejects four working shapes, so it needs the crossing story sorted out
 * first, not a one-line change here. */
static bool rv_is_noncarrier_aggregate(Type t) {
    /* By-value aggregates are a property of the COMPILED path.  The tree-walking
     * interpreter boxes every value as a handle, so there an ADT under an
     * integer return is a genuine bridge -- and programs write exactly that
     * deliberately, via the `:turi` arm of a `#?(:tur ... :turi ...)` reader
     * conditional whose `:tur` arm is inline-C returning the boxed pointer
     * (tests/fixtures/map-multiword-struct-key). */
    if (g_interpret_mode) return false;
    /* A :heap ADT-app (`(Vec int)`, `(Cons int)`) lowers to a typed pointer
     * `tur_adt_Vec__int *`, which is no more the int64 carrier than a by-value
     * aggregate is.  It compiles today only because `-Wint-conversion` is a
     * warning rather than an error -- under `-Werror` it is the same hard
     * failure -- so it belongs here.  A NON-heap by-value ADT-app
     * (`(Option int)`) is left alone: its return crossing genuinely grounds it
     * and it emits clean. */
    if (t.kind == TY_APP) return type_is_heap_adt(t);
    if (t.kind != TY_ADT) return false;
    const char *cn = type_c_name(t);
    return cn && strcmp(cn, "int64_t") != 0;
}
/* The other side must be a CONCRETE, register-pinned scalar for the clash to be
 * real; a tyvar / unknown / never / any could still resolve to the aggregate. */
static bool rv_is_pinned_scalar(Type t) {
    if (t.kind == TY_ADT || t.kind == TY_APP) return false;
    if (rc_is_unpinned_kind(t.kind)) return false;
    return ps_is_integer_scalar_kind(t.kind) || t.kind == TY_CSTR ||
           rc_is_float_kind(t.kind);
}
bool return_type_carrier_aggregate_conflict(Type declared, Type body) {
    if (rv_is_noncarrier_aggregate(declared)) return rv_is_pinned_scalar(body);
    if (rv_is_noncarrier_aggregate(body))     return rv_is_pinned_scalar(declared);
    return false;
}

/* committed-applied-return-vs-scalar: a by-value APPLIED type against a
 * concrete scalar in a committed return position.
 *
 * return_type_carrier_aggregate_conflict leaves TY_APP out on purpose -- "a
 * parametric return position has a carrier crossing that grounds it" -- and
 * that is true where there IS a crossing: a generic, an `#{Unsafe}` defn, an
 * instance method.  A monomorphic defn has none, and the check compares the
 * declared return by KIND only, so `(defn h [x : float] : cstr (some x))`,
 * `: int (pair x 1)` and `: (Option int) 7` all type-checked, and the program
 * then reached `cc` with two C types for one value -- or, where `cc` lets a
 * pointer pass for an integer, handed back an address as the answer.
 *
 * Fires when one side is a GROUND applied type whose C spelling is a real
 * aggregate (not the transparent `int64_t` of an opaque-over-:int app, which
 * stays a bridge, as the bare-ADT check above tolerates it) and the other is a
 * pinned scalar.  Floats are already the register-class check's.  The caller
 * gates it on the committed class. */
static bool ac_type_is_ground(const Type *t, int depth) {
    if (!t || depth > 32) return false;
    switch (t->kind) {
        case TY_TYVAR: case TY_UNKNOWN: case TY_ANY: case TY_NEVER:
            return false;
        case TY_APP:
            return ac_type_is_ground(t->as.app.fn, depth + 1) &&
                   ac_type_is_ground(t->as.app.arg, depth + 1);
        default:
            return true;
    }
}
/* The application's head ADT, when it is a by-value sum or record with real
 * constructors: every instantiation of it, ground or not, is a C aggregate.
 * An opaque newtype head (n_ctors == 0) is the int64 carrier and a :heap head
 * is a typed pointer (the aggregate check's), so neither qualifies. */
static const AdtDef *ac_byvalue_head(Type t) {
    if (t.kind != TY_APP) return NULL;
    const Type *h = &t;
    while (h->kind == TY_APP && h->as.app.fn) h = h->as.app.fn;
    if (h->kind != TY_ADT || !h->as.adt_.def) return NULL;
    const AdtDef *d = h->as.adt_.def;
    if (d->is_opaque || d->is_heap || d->n_ctors == 0) return NULL;
    return d;
}
static bool ac_is_byvalue_app(Type t) {
    /* Not exempt under the interpreter, unlike rv_is_noncarrier_aggregate:
     * this is a TYPE mismatch the program states, and turi printed the same
     * box address the compiled program did. */
    if (!ac_byvalue_head(t)) return false;
    if (type_is_heap_adt(t) || type_is_transparent_int_newtype(t)) return false;
    if (!ac_type_is_ground(&t, 0)) return true;   /* `(Result float E)` */
    const char *cn = type_c_name(t);
    return cn && strncmp(cn, "tur_adt_", 8) == 0;
}
static bool ac_is_pinned_word_scalar(Type t) {
    return ps_is_integer_scalar_kind(t.kind) || t.kind == TY_CSTR;
}
/* Any application whose head is a real (non-opaque) ADT, :heap included. */
static const AdtDef *ac_real_head(Type t) {
    if (t.kind != TY_APP) return NULL;
    const Type *h = &t;
    while (h->kind == TY_APP && h->as.app.fn) h = h->as.app.fn;
    if (h->kind != TY_ADT || !h->as.adt_.def) return NULL;
    const AdtDef *d = h->as.adt_.def;
    return (d->is_opaque || d->n_ctors == 0) ? NULL : d;
}
bool applied_type_conflict(Type want, Type got) {
    /* `(Vec int)` claimed, `(Option float)` given: different types whatever
     * either one's representation. */
    const AdtDef *wh = ac_real_head(want), *gh = ac_real_head(got);
    if (wh && gh && wh != gh) return true;
    /* One head, both ground, different arguments: `(Vec int)` claimed,
     * `(Vec float)` given. */
    if (wh && gh && ac_type_is_ground(&want, 0) && ac_type_is_ground(&got, 0) &&
        !type_eq(want, got))
        return true;
    /* A :heap app is a typed pointer: never a bool, string or float.  (An
     * integer stays accepted -- `0` is the nil of the cons-list family.) */
    if (wh && wh->is_heap &&
        (got.kind == TY_BOOL || got.kind == TY_CSTR || rc_is_float_kind(got.kind)))
        return true;
    if (ac_is_byvalue_app(want)) {
        if (ac_is_pinned_word_scalar(got) || rc_is_float_kind(got.kind))
            return true;
        if (!ac_is_byvalue_app(got)) return false;
        /* Different heads are different types, ground or not; two ground
         * apps of one head are two C aggregates -- `(Option int)` and
         * `(Option float)` share no layout. */
        if (ac_byvalue_head(want) != ac_byvalue_head(got)) return true;
        return ac_type_is_ground(&want, 0) && ac_type_is_ground(&got, 0) &&
               !type_eq(want, got);
    }
    return ac_is_pinned_word_scalar(want) && ac_is_byvalue_app(got);
}
bool return_type_applied_scalar_conflict(const Type *declared_app,
                                         TypeKind ret_kind, Type body) {
    if (declared_app) return applied_type_conflict(*declared_app, body);
    Type d; memset(&d, 0, sizeof d); d.kind = ret_kind;
    return applied_type_conflict(d, body);
}

/* carrier-aware-return-unification: single dispatcher over the return-position
 * predicates -- see elab_internal.h.  Runs them in the established order
 * (nominal -> register-class -> pointer-scalar commit -> pointer-scalar reverse)
 * and returns the first conflict.  `cls` calibrates two axes against the carrier
 * ABI:
 *   - Register-class (float-vs-non-float): symmetric for EVERY class as of
 *     2026-08-16 (a float never rides the int64 carrier, so a
 *     float-vs-concrete-non-float result is always an xmm-vs-GP miscompile).
 *     The former RET_CLASS_CARRIER_METHOD tolerance claimed the per-instance
 *     emit path resolves such a method to its real register class; measured
 *     false (the emitted C value-converts), and the engines diverged on it --
 *     see docs/archive/int-declared-method-float-body-engine-divergence.md.
 *   - Pointer-scalar REVERSE (integer-declared, cstr body): only RET_CLASS_
 *     COMMITTED rejects it (Phase 2).  For a generic / `#{Unsafe}` defn
 *     (RET_CLASS_CARRIER_FN) or an instance method (RET_CLASS_CARRIER_METHOD)
 *     this is the deliberate carrier-handle bridge and stays accepted.
 * The commit-direction pointer-scalar (cstr-declared, integer body, TUR-E0708)
 * and nominal-identity clash (TUR-E0001) are unconditional.  The int-literal ->
 * float widening is the caller's pre-step. */
ReturnConflict return_position_conflict(const AdtDef *ret_adt,
                                        TypeKind ret_kind, Type body,
                                        ReturnClass cls, bool check_nil_body) {
    if (ret_adt && return_type_nominal_conflict(ret_adt, body))
        return RET_CONFLICT_NOMINAL;

    /* nil-tail-not-checked-against-declared-return: checked BEFORE the
     * representation-pair predicates below, and NOT gated on the return class.
     * Every tolerance those classes buy is a bridge between two things that are
     * both int64_t in the emitted C and both carry a value; a nil body carries
     * no value to bridge, so a generic or `#{Unsafe}` function is as wrong here
     * as a committed one.  An ADT-declared return reaches this too: `ret_adt`
     * non-NULL with a nil body is the same defect. */
    if (return_type_nil_body_conflict(ret_adt ? TY_ADT : ret_kind, body,
                                      check_nil_body))
        return RET_CONFLICT_NIL_BODY;

    /* Register-class: commit-direction-only for a typeclass instance method;
     * symmetric otherwise.  The predicate already tolerates a same-register-class
     * pair, so the gate only narrows the instance-method case. */
    /* int-declared-method-float-body-engine-divergence: SYMMETRIC for every
     * return class, instance methods included.  The old commit-direction gate
     * tolerated a float body under a non-float instance slot on the claim
     * that "the per-instance emit path resolves [it] to its real register
     * class" -- measured false: the emitted C was `static int64_t ... {
     * return 7.5; }`, a destructive value conversion, while turi kept the
     * float, so the two engines disagreed on every program that used the
     * shape.  Unlike a cstr body under `: int` (pointer bits ride the
     * carrier losslessly and come back -- still tolerated below), a float
     * under an int slot cannot bridge: its bits and its value part ways,
     * and the shape cannot say which the author meant (stdlib Clone wanted
     * bits; the poly-to-fat fixtures wanted the value).  The stdlib half
     * was resolved by giving Clone its honest `[x : a] : a` signature;
     * this closes the user-facing half by making the shape an error, the
     * same TUR-E0707 the equivalent defn has always been. */
    if (return_type_register_class_conflict(ret_kind, body))
        return RET_CONFLICT_REGISTER_CLASS;
    (void)cls;

    if (return_type_pointer_scalar_conflict(ret_kind, body))
        return RET_CONFLICT_POINTER_SCALAR;

    /* Reverse pointer-scalar: only a genuinely committed position (a monomorphic
     * non-#{Unsafe} defn) has no carrier to bridge, so only there is a cstr body
     * under an integer return a real error (TUR-E0709). */
    if (cls == RET_CLASS_COMMITTED &&
        return_type_pointer_scalar_reverse_conflict(ret_kind, body))
        return RET_CONFLICT_TYPE_REVERSE;

    /* bool-vs-integer: likewise committed-only.  `bool` and the integer family
     * share the int64 0/1 carrier, but the language treats them as distinct
     * (binding position already rejects the swap), so a committed defn with no
     * carrier to bridge gets a real mismatch (TUR-E0709). */
    if (cls == RET_CLASS_COMMITTED &&
        return_type_bool_integer_conflict(ret_kind, body))
        return RET_CONFLICT_BOOL_INTEGER;

    /* Non-carrier aggregate vs carrier scalar.  Unlike the two checks above this
     * is NOT gated on the return class: every tolerance the carrier classes buy
     * is a tolerance between two things that are both `int64_t` in the emitted
     * C, and a by-value aggregate is not one of them.  A generic defn, an
     * `#{Unsafe}` one, and a typeclass instance method all reach `cc` with
     * "incompatible types when returning" on this shape -- an instance method
     * declared `: int` whose body returns a record ADT is exactly what
     * tests/fixtures/typeclass/parametric-clone-list was sitting on -- so
     * gating it would leave the reported case unfixed.  The declared side is
     * reconstructed from (ret_adt, ret_kind) since the caller carries those
     * rather than a whole Type. */
    {
        Type declared;
        memset(&declared, 0, sizeof declared);
        if (ret_adt) {
            declared.kind = TY_ADT;
            declared.as.adt_.def = (AdtDef *)ret_adt;
        } else {
            declared.kind = ret_kind;
        }
        if (return_type_carrier_aggregate_conflict(declared, body))
            return RET_CONFLICT_CARRIER_AGGREGATE;
    }

    return RET_CONFLICT_NONE;
}

/* nil-tail-not-checked-against-declared-return: is this body's TAIL a nil
 * literal?  A multi-form body is wrapped (EX_DO), and a body that opens a scope
 * is wrapped again (EX_LET / EX_LETREC), so the literal the check cares about is
 * not the body Expr itself -- testing `body->kind == EX_NIL_LIT` directly let
 * `(defn f [] : int (println "x") nil)` through while rejecting the single-form
 * `(defn f [] : int nil)`, which is exactly the inconsistency the report says
 * does not exist ("multi-form bodies behave identically").
 *
 * EX_IF is deliberately NOT peeled.  Its branch types are already unified and
 * checked against each other, `!`-typed (panic) arms included, and a nil branch
 * under a non-nil peer is that check's business, not this one's. */
bool body_tail_is_nil_literal(const Expr *e) {
    if (!e) return false;
    switch (e->kind) {
        case EX_NIL_LIT:
            return true;
        case EX_DO:
            return e->as.do_.n
                 ? body_tail_is_nil_literal(e->as.do_.items[e->as.do_.n - 1])
                 : false;
        case EX_LET:
        case EX_LETREC:
            return body_tail_is_nil_literal(e->as.let_.body);
        default:
            return false;
    }
}

/* TY4: borrow referent extraction -- see elab_internal.h.
 *
 * Looks through result-position wrappers (do/let/letrec bodies and both `if`
 * branches) so a borrow produced inside a nested block is still attributed to
 * its referent.  For an `if`, the *shorter-lived* (deeper) of the two branch
 * referents is returned, since that is the one that would dangle first. */
const Binding *borrow_referent_binding(const Expr *e) {
    if (!e) return NULL;
    switch (e->kind) {
        case EX_BORROW_IMMUT: {
            const Expr *inner = e->as.borrow_immut_.expr;
            return (inner && inner->kind == EX_VAR) ? inner->as.var.binding : NULL;
        }
        case EX_BORROW_MUT: {
            const Expr *inner = e->as.borrow_mut_.expr;
            return (inner && inner->kind == EX_VAR) ? inner->as.var.binding : NULL;
        }
        case EX_DO:
            return e->as.do_.n ? borrow_referent_binding(e->as.do_.items[e->as.do_.n - 1])
                               : NULL;
        case EX_LET:
        case EX_LETREC:
            return borrow_referent_binding(e->as.let_.body);
        case EX_IF: {
            const Binding *t = borrow_referent_binding(e->as.if_.then_);
            const Binding *f = borrow_referent_binding(e->as.if_.else_or_null);
            if (!t) return f;
            if (!f) return t;
            return (f->scope_depth > t->scope_depth) ? f : t;
        }
        default:
            return NULL;
    }
}

Binding *expr_closure_fn_binding(const Expr *expr) {
    if (!expr) return NULL;

    switch (expr->kind) {
        case EX_REINTERPRET:
            return expr_closure_fn_binding(expr->as.reinterpret_.expr);
        case EX_ASCRIBE:
            return expr_closure_fn_binding(expr->as.ascribe_.inner);
        case EX_CLOSURE:
            if (expr->as.closure_.closure && expr->as.closure_.closure->fn) {
                return expr->as.closure_.closure->fn->binding;
            }
            return NULL;
        case EX_VAR:
            if (!expr->as.var.binding) return NULL;
            if (expr->as.var.binding->closure_fn_binding) {
                return expr->as.var.binding->closure_fn_binding;
            }
            return expr->as.var.binding->returns_closure_fn_binding;
        case EX_CALL:
            if (!expr->as.call_.fn_binding) return NULL;
            return expr->as.call_.fn_binding->returns_closure_fn_binding;
        case EX_LET:
        case EX_LETREC:
            return expr_closure_fn_binding(expr->as.let_.body);
        case EX_DO:
            for (int i = (int)expr->as.do_.n - 1; i >= 0; i--) {
                const Expr *item = expr->as.do_.items[i];
                if (item->kind == EX_DEFER) continue;
                return expr_closure_fn_binding(item);
            }
            return NULL;
        case EX_IF: {
            Binding *then_binding = expr_closure_fn_binding(expr->as.if_.then_);
            Binding *else_binding = expr->as.if_.else_or_null
                ? expr_closure_fn_binding(expr->as.if_.else_or_null)
                : NULL;
            return (then_binding && then_binding == else_binding) ? then_binding : NULL;
        }
        default:
            return NULL;
    }
}

/* poly-closure-result-specialization: does `e` (or a subexpression) fat-dispatch
 * a captured closure -- i.e. CALL a value whose binding is a ptr<void>/fn-typed
 * carrier (a captured closure box), as opposed to a directly-named global fn?
 * Such a call's result type is erased to the int64 carrier in the elaborated
 * body, so an inner-closure clone cannot recover its float register class. */
static bool expr_fat_dispatches_closure(const Expr *e) {
    if (!e) return false;
    switch (e->kind) {
        case EX_CALL: {
            Binding *fb = e->as.call_.fn_binding;
            if (fb && (fb->type.kind == TY_PTR_VOID || fb->type.kind == TY_FN) &&
                !fb->is_global)
                return true;
            if (e->as.call_.fn_expr && expr_fat_dispatches_closure(e->as.call_.fn_expr))
                return true;
            for (uint32_t i = 0; i < e->as.call_.n_args; i++)
                if (expr_fat_dispatches_closure(e->as.call_.args[i])) return true;
            return false;
        }
        case EX_LET:
        case EX_LETREC:
            for (uint32_t i = 0; i < e->as.let_.n; i++)
                if (expr_fat_dispatches_closure(e->as.let_.bindings[i].init)) return true;
            return expr_fat_dispatches_closure(e->as.let_.body);
        case EX_DO:
            for (uint32_t i = 0; i < e->as.do_.n; i++)
                if (expr_fat_dispatches_closure(e->as.do_.items[i])) return true;
            return false;
        case EX_IF:
            return expr_fat_dispatches_closure(e->as.if_.cond) ||
                   expr_fat_dispatches_closure(e->as.if_.then_) ||
                   expr_fat_dispatches_closure(e->as.if_.else_or_null);
        case EX_REINTERPRET:
            return expr_fat_dispatches_closure(e->as.reinterpret_.expr);
        case EX_ASCRIBE:
            return expr_fat_dispatches_closure(e->as.ascribe_.inner);
        default:
            return false;
    }
}

/* poly-closure-inner-dispatch-result-erased (Direction 3): return true when a
 * fat-dispatch in `e` goes through a binding that Direction 3 cannot handle --
 * i.e. a TY_PTR_VOID bare-fat capture, or a TY_FN without a named-tyvar
 * result_full_type.  A TY_FN with result_full_type = TY_TYVAR(name) IS handled
 * by Direction 3 (emit derives the real C type from the binding's resolved type)
 * and should NOT trigger the guard or block the inner-spec clone. */
/* A result kind the fat-dispatch call's own type spells exactly: a scalar is
 * never erased to the carrier, so the call site's C return type is it. */
static bool dispatch_result_kind_is_scalar(TypeKind k) {
    switch (k) {
        case TY_NIL: case TY_BOOL: case TY_INT: case TY_FLOAT: case TY_CSTR:
        case TY_INT8: case TY_INT16: case TY_INT32: case TY_INT64:
        case TY_UINT8: case TY_UINT16: case TY_UINT32: case TY_UINT64:
        case TY_FLOAT32: case TY_FLOAT64:
            return true;
        default:
            return false;
    }
}

static bool binding_dispatch_is_untyped(const Binding *fb) {
    if (!fb || fb->is_global) return false;
    if (fb->type.kind == TY_PTR_VOID) return true;
    if (fb->type.kind != TY_FN) return false;
    const Type *rfull = fb->type.as.fn.result_full_type;
    if (rfull && rfull->kind == TY_TYVAR && rfull->as.tyvar_.name) return false;
    /* generic-closure-float-passed-to-fn-typed-callback: a CONCRETE scalar
     * result is not erased either -- the call's own type is that scalar, so
     * Direction 3 has nothing to recover on the result side, and it already
     * resolves the declared ARG types through the spec.  `(k v)` with
     * `k : (fn [A] bool)` is therefore typed; only its A needed the spec. */
    TypeKind rk = rfull ? rfull->kind : fb->type.as.fn.result_kind;
    if (dispatch_result_kind_is_scalar(rk)) return false;
    return true;
}

static bool expr_fat_dispatches_untyped(const Expr *e) {
    if (!e) return false;
    switch (e->kind) {
        case EX_CALL: {
            Binding *fb = e->as.call_.fn_binding;
            if (binding_dispatch_is_untyped(fb)) return true;
            if (e->as.call_.fn_expr && expr_fat_dispatches_untyped(e->as.call_.fn_expr))
                return true;
            for (uint32_t i = 0; i < e->as.call_.n_args; i++)
                if (expr_fat_dispatches_untyped(e->as.call_.args[i])) return true;
            return false;
        }
        case EX_LET:
        case EX_LETREC:
            for (uint32_t i = 0; i < e->as.let_.n; i++)
                if (expr_fat_dispatches_untyped(e->as.let_.bindings[i].init)) return true;
            return expr_fat_dispatches_untyped(e->as.let_.body);
        case EX_DO:
            for (uint32_t i = 0; i < e->as.do_.n; i++)
                if (expr_fat_dispatches_untyped(e->as.do_.items[i])) return true;
            return false;
        case EX_IF:
            return expr_fat_dispatches_untyped(e->as.if_.cond) ||
                   expr_fat_dispatches_untyped(e->as.if_.then_) ||
                   expr_fat_dispatches_untyped(e->as.if_.else_or_null);
        case EX_REINTERPRET:
            return expr_fat_dispatches_untyped(e->as.reinterpret_.expr);
        case EX_ASCRIBE:
            return expr_fat_dispatches_untyped(e->as.ascribe_.inner);
        default:
            return false;
    }
}

/* Return true when the closure that `expr` evaluates to has a body that
 * fat-dispatches only through untyped bindings (Direction 3 cannot recover
 * the result type). */
bool expr_closure_return_dispatches_untyped(const Expr *expr) {
    if (!expr) return false;
    switch (expr->kind) {
        case EX_REINTERPRET:
            return expr_closure_return_dispatches_untyped(expr->as.reinterpret_.expr);
        case EX_ASCRIBE:
            return expr_closure_return_dispatches_untyped(expr->as.ascribe_.inner);
        case EX_CLOSURE:
            if (expr->as.closure_.closure && expr->as.closure_.closure->fn)
                return expr_fat_dispatches_untyped(expr->as.closure_.closure->fn->body);
            return false;
        case EX_LET:
        case EX_LETREC:
            return expr_closure_return_dispatches_untyped(expr->as.let_.body);
        case EX_DO:
            for (int i = (int)expr->as.do_.n - 1; i >= 0; i--) {
                const Expr *item = expr->as.do_.items[i];
                if (item->kind == EX_DEFER) continue;
                return expr_closure_return_dispatches_untyped(item);
            }
            return false;
        case EX_IF:
            return expr_closure_return_dispatches_untyped(expr->as.if_.then_) ||
                   (expr->as.if_.else_or_null &&
                    expr_closure_return_dispatches_untyped(expr->as.if_.else_or_null));
        default:
            return false;
    }
}

/* Return true when the closure that `expr` evaluates to has a body that
 * fat-dispatches a captured closure (see expr_fat_dispatches_closure). Mirrors
 * the structural walk of expr_closure_fn_binding to locate the inner closure. */
bool expr_closure_return_dispatches(const Expr *expr) {
    if (!expr) return false;
    switch (expr->kind) {
        case EX_REINTERPRET:
            return expr_closure_return_dispatches(expr->as.reinterpret_.expr);
        case EX_ASCRIBE:
            return expr_closure_return_dispatches(expr->as.ascribe_.inner);
        case EX_CLOSURE:
            if (expr->as.closure_.closure && expr->as.closure_.closure->fn)
                return expr_fat_dispatches_closure(expr->as.closure_.closure->fn->body);
            return false;
        case EX_LET:
        case EX_LETREC:
            return expr_closure_return_dispatches(expr->as.let_.body);
        case EX_DO:
            for (int i = (int)expr->as.do_.n - 1; i >= 0; i--) {
                const Expr *item = expr->as.do_.items[i];
                if (item->kind == EX_DEFER) continue;
                return expr_closure_return_dispatches(item);
            }
            return false;
        case EX_IF:
            return expr_closure_return_dispatches(expr->as.if_.then_) ||
                   (expr->as.if_.else_or_null &&
                    expr_closure_return_dispatches(expr->as.if_.else_or_null));
        default:
            return false;
    }
}

/* Phase M2: File reading helper — returns 0 on success, -1 on failure. */
int elab_read_file(const char *path, char **out, size_t *out_len) {
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return -1; }
    long size = ftell(f);
    if (size < 0) { fclose(f); return -1; }
    if (fseek(f, 0, SEEK_SET) != 0) { fclose(f); return -1; }
    char *buf = (char *)malloc((size_t)size + 1);
    if (!buf) { fclose(f); return -1; }
    size_t n = fread(buf, 1, (size_t)size, f);
    fclose(f);
    if (n != (size_t)size) { free(buf); return -1; }
    buf[size] = '\0';
    *out = buf;
    *out_len = (size_t)size;
    return 0;
}

/* Phase M2: Find a loaded module in the registry by interned name. */
ElabModule *elab_find_loaded_module(Elab *e, const Symbol *name) {
    for (uint32_t i = 0; i < e->n_loaded_modules; i++) {
        if (e->loaded_modules[i].name == name)
            return &e->loaded_modules[i];
    }
    return NULL;
}

/* ---- helpers ---- */

bool effect_row_contains_symbol(const EffectRow *row, const Symbol *name) {
    if (!row || !name) return false;
    switch (row->kind) {
    case ERK_EMPTY:
        return false;
    case ERK_CONCRETE:
        for (uint8_t i = 0; i < row->as.concrete.n_effects; i++) {
            Effect *eff = row->as.concrete.effects[i];
            if (eff && eff->name == name) return true;
        }
        return false;
    case ERK_VAR:
        return false;
    case ERK_UNION:
        return effect_row_contains_symbol(row->as.union_.left, name) ||
               effect_row_contains_symbol(row->as.union_.right, name);
    case ERK_UNRESOLVED:
        for (uint8_t i = 0; i < row->as.unresolved.n_sym_names; i++) {
            if (row->as.unresolved.sym_names[i] == name) return true;
        }
        return false;
    }
    return false;
}

Expr *e_nil(Elab *e, Span span) {
    return expr_new(e->arena, EX_NIL_LIT, TYPE_NIL, span);
}

/* Phase M6: Compute the mangled C name for an exported binding.
 * Mirrors the logic in emit.c:raw_name_for_binding.
 * Returns a malloc'd string that the caller must free. */
char *elab_mangle_binding_name(const Binding *b) {
    if (b->c_export_name) return strdup(b->c_export_name);

    /* extern-c: legacy fold (alnum/'_' passthrough, everything else -> '_'),
     * matching mangle_field_name in emit_module.c. Real C symbols, not mangled. */
    if (b->is_extern_c) {
        char *p = (char *)malloc(b->name->len + 1);
        if (!p) { fprintf(stderr, "tur: oom\n"); abort(); }
        for (uint32_t i = 0; i < b->name->len; i++) {
            char c = b->name->name[i];
            p[i] = ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                    (c >= '0' && c <= '9') || c == '_') ? c : '_';
        }
        p[b->name->len] = '\0';
        return p;
    }

    char mod_prefix[512];
    size_t mod_prefix_len = 0;
    bool is_main_binding = (b->name->len == 4 &&
                             memcmp(b->name->name, "main", 4) == 0);
    /* Phase M7: mirror emit.c — only globals get module-prefixed C names. */
    if (b->defining_module_name != NULL && !is_main_binding && b->is_global) {
        const char *mn = b->defining_module_name->name;
        size_t mn_len  = b->defining_module_name->len;
        size_t j = 0;
        /* Mirror raw_name_for_binding in emit_core.c: each '/'-separated
         * component is mangled through the shared injective scheme, with '/'
         * becoming the "__" structural separator. Keep these two in lockstep. */
        for (size_t i = 0; i < mn_len && j + 6 < sizeof(mod_prefix); i++) {
            char c = mn[i];
            if (c == '/') { mod_prefix[j++] = '_'; mod_prefix[j++] = '_'; }
            else { tur_mangle_append(mod_prefix, &j, &mn[i], 1); }
        }
        mod_prefix[j++] = '_';
        mod_prefix[j++] = '_';
        mod_prefix[j]   = '\0';
        mod_prefix_len  = j;
    }

    /* +8 slack mirrors raw_name_for_binding: room for the `tur_u_` libc-
     * collision guard prefix (6 bytes) plus the NUL. */
    size_t total = mod_prefix_len + tur_mangle_bound(b->name->len) + 8;
    char *p = (char *)malloc(total);
    if (!p) { fprintf(stderr, "tur: oom\n"); abort(); }
    size_t k = 0;
    if (mod_prefix_len > 0) { memcpy(p, mod_prefix, mod_prefix_len); k = mod_prefix_len; }
    /* Mirror raw_name_for_binding: "__"-prefixed pure-C-id synthesized names
     * verbatim (module prefix still applied); injective for other globals;
     * legacy fold for function-locals referenced by inline-C. */
    if (tur_name_is_c_identifier(b->name->name, b->name->len) &&
        b->name->len >= 2 && b->name->name[0] == '_' && b->name->name[1] == '_') {
        memcpy(p + k, b->name->name, b->name->len);
        k += b->name->len;
    } else if (b->is_global) {
        /* codegen-user-defn-collides-with-libc-pipe2: mirror raw_name_for_binding
         * -- a bare (non-module) global whose spelling is a libc symbol gets the
         * `tur_u_` guard prefix so def, use, and inline-C `__TUR_CNAME_` all
         * resolve to the same collision-free C name. */
        if (mod_prefix_len == 0 && !is_main_binding &&
            tur_name_collides_libc(b->name->name, b->name->len)) {
            memcpy(p + k, TUR_NAME_GUARD_PREFIX, TUR_NAME_GUARD_PREFIX_LEN);
            k += TUR_NAME_GUARD_PREFIX_LEN;
        }
        /* c-keyword-function-names-not-mangled: mirror raw_name_for_binding --
         * a bare global whose spelling is a C reserved word gets the same guard
         * prefix so def, use, and inline-C `__TUR_CNAME_` all agree. */
        else if (mod_prefix_len == 0 &&
                 tur_name_is_c_keyword(b->name->name, b->name->len)) {
            memcpy(p + k, TUR_NAME_GUARD_PREFIX, TUR_NAME_GUARD_PREFIX_LEN);
            k += TUR_NAME_GUARD_PREFIX_LEN;
        }
        tur_mangle_append(p, &k, b->name->name, b->name->len);
    } else {
        /* Mirror raw_name_for_binding's local/parameter branch. */
        if (tur_name_is_c_keyword(b->name->name, b->name->len)) {
            memcpy(p + k, TUR_NAME_GUARD_PREFIX, TUR_NAME_GUARD_PREFIX_LEN);
            k += TUR_NAME_GUARD_PREFIX_LEN;
        }
        tur_mangle_legacy_append(p, &k, b->name->name, b->name->len);
    }
    p[k] = '\0';
    return p;
}
