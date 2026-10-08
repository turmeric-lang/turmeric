#include "types.h"
#include "typeclass.h"  /* Phase 15 */
#include "kind_check.h"  /* Phase HKT-P1: for kind_of_type_app */
#include "forms.h"      /* Phase HKT-P1: for Span */
#include "effect.h"     /* FH4.1: EffectRow name-set helpers for TY_HANDLER */
#include "expr.h"     /* increment 4 stage 3: Binding, for repr_of_binding */
#include "globals.h"  /* increment 4 stage 3: g_emit_abi_trace (container-elem shadow) */
#include "experiments.h" /* SR3 slice B: option-niche lifecycle warning */
#include "mangle.h"
char *mangle_adt_name(const char *name);  /* emit_core.c; the one ADT/ctor spelling */
/* c-keyword guard: keep append_c_ident_mangled in lockstep with mangle_field_name */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <assert.h>   /* structdef-retirement slice 5 B4: def-less TY_STRUCT guard */

/* Global type arena backing fn types' out-of-line per-arg arrays (arg_kinds /
 * arg_flags).  Process-lifetime and never freed: the arrays must outlive every
 * by-value Type copy that shares them, and a compiler process builds a bounded
 * set of fn types.  Reachable via this file-scope static, so LeakSanitizer does
 * not report it.  Single-threaded: tur elaborates/emits on one thread per
 * invocation (the suite parallelises across processes, not threads). */
static Arena g_type_arena;
static bool  g_type_arena_ready = false;

uint8_t *tur_fn_args_alloc(uint32_t n) {
    if (n == 0) return NULL;
    if (!g_type_arena_ready) { arena_init(&g_type_arena, 0); g_type_arena_ready = true; }
    /* arena_alloc zero-fills is not guaranteed; zero explicitly so arg_flags
     * defaults to all-clear and arg_kinds has no indeterminate slots. */
    uint8_t *p = (uint8_t *)arena_alloc(&g_type_arena, (size_t)n);
    memset(p, 0, (size_t)n);
    return p;
}

/* constrained-hkt-abstract-var-requires-last-param-free: the same
 * process-lifetime arena, exposed for building a hole-headed partial
 * application where no caller-supplied arena is in scope (call-site
 * unification).  Same lifetime rationale as tur_fn_args_alloc: the Type is
 * copied by value into a binding table that outlives any per-call scratch. */
Arena *tur_type_arena(void) {
    if (!g_type_arena_ready) { arena_init(&g_type_arena, 0); g_type_arena_ready = true; }
    return &g_type_arena;
}

/* CONV-S1 seam 4 (keystone): named-layout helpers defined in emit_core.c.
 * Forward-declared here (types.c does not include emit_internal.h) so the
 * single-variant record monomorph emit routes its field stores through the same
 * member-path the typedef / field-read / match sites use. */
char *mangle_field_name(const char *name);
char *mangle_adt_name(const char *name);
/* duplicate-ctor-names-collide-in-emitted-c: the ADT-qualified base token
 * of a constructor's emitted C FUNCTION symbol.  Defined in emit_core.c;
 * forward-declared here (types.c does not include emit_internal.h) so the
 * monomorph ctor emit spells the symbol exactly as the base-ctor emit and
 * every call site do. */
char *mangle_ctor_symbol(const struct AdtDef *adt, const char *ctor_name);
char *adt_field_member_path(const struct AdtDef *def, const struct CtorDef *ctor,
                            uint32_t fi);
/* region-lock-hardening: the store-side region note for a ctor field write,
 * defined in emit_core.c and declared in emit_internal.h (not included here);
 * the monomorph ctor's boxed branch uses it exactly as the base ctor does. */
void emit_region_note_lvalue(struct Buf *body, int indent, const char *ctype, const char *lv);

/* ptr-generic-parameterised-type: intern compound type-name strings (e.g.
 * "double *", "ptr<float>") so type_c_name/type_name can return a stable,
 * reachable pointer.  Without this, building names with tur_strdup on every
 * call both leaks (LeakSanitizer) and breaks the "returns a long-lived string"
 * contract callers rely on.  Append-only + dedup keeps the table bounded. */
static char    **g_interned_type_names = NULL;
static uint32_t   g_n_interned_type_names = 0;
static uint32_t   g_cap_interned_type_names = 0;

static const char *intern_type_name(const char *s) {
    for (uint32_t i = 0; i < g_n_interned_type_names; i++) {
        if (strcmp(g_interned_type_names[i], s) == 0)
            return g_interned_type_names[i];
    }
    if (g_n_interned_type_names >= g_cap_interned_type_names) {
        uint32_t nc = g_cap_interned_type_names ? g_cap_interned_type_names * 2 : 16;
        char **ni = (char **)realloc(g_interned_type_names, nc * sizeof(char *));
        if (!ni) { fprintf(stderr, "tur: oom\n"); abort(); }
        g_interned_type_names = ni;
        g_cap_interned_type_names = nc;
    }
    char *d = tur_strdup(s);
    if (!d) { fprintf(stderr, "tur: oom\n"); abort(); }
    g_interned_type_names[g_n_interned_type_names++] = d;
    return d;
}

/* Phase 13: Helper to compare lifetimes */
static bool lifetimes_eq(LifetimeId a_lifetimes[], uint8_t a_n,
                        LifetimeId b_lifetimes[], uint8_t b_n) {
    if (a_n != b_n) return false;
    for (uint8_t i = 0; i < a_n; i++) {
        if (a_lifetimes[i] != b_lifetimes[i]) {
            return false;
        }
    }
    return true;
}

/* session-type-eq-ignores-the-protocol: equirecursive equality for session
 * protocols.
 *
 * Protocols are equirecursive: `session_protocol_of` unfolds a `Rec` binder at
 * every use site (elab_sessions.c), so a DECLARED parameter type can hold the
 * folded `Rec[X, P]` while the argument holds its one-step unfolding
 * `P[X := Rec[X, P]]`. Those are the same protocol, so a purely structural
 * comparison would reject working programs -- the failure mode to avoid here
 * is a false NEGATIVE, which breaks compiling code, not a false positive.
 *
 * Termination is not the difficulty it looks like. A back-reference is a
 * SENTINEL (`TY_SESSION_REC` with `fst == NULL`) carrying its binder's interned
 * label, and a binder's body holds that sentinel rather than a pointer back to
 * itself, so the type graph is a finite DAG. What the assumption set below
 * buys is convergence when one side is folded and the other is not: the pair
 * of nodes under comparison recurs, and assuming it equal on the second
 * encounter is the standard coinductive rule.
 *
 * Everything unrecognised is treated as EQUAL. This routine only ever needs to
 * be strict enough to separate genuinely different protocols; being permissive
 * on an inference hole (`TY_UNKNOWN`), an unresolved tyvar, or a pathologically
 * deep type that overflows the fixed bounds keeps it from rejecting a program
 * the old always-equal behaviour accepted. */

#define SESS_EQ_MAX 32

typedef struct { const char *label; const Type *binder; } SessBind;

typedef struct {
    SessBind    env_a[SESS_EQ_MAX];
    SessBind    env_b[SESS_EQ_MAX];
    int         n_env_a, n_env_b;
    const Type *assume_a[SESS_EQ_MAX];
    const Type *assume_b[SESS_EQ_MAX];
    int         n_assume;
} SessEqCtx;

static bool sess_is_rec_binder(const Type *t) {
    return t && t->kind == TY_SESSION_REC && t->as.session_.fst != NULL;
}

static bool sess_is_backref(const Type *t) {
    return t && t->kind == TY_SESSION_REC && t->as.session_.fst == NULL;
}

static const Type *sess_lookup(const SessBind *env, int n, const char *label) {
    for (int i = n - 1; i >= 0; i--)
        if (env[i].label == label) return env[i].binder;
    return NULL;
}

static bool sess_assumed(const SessEqCtx *ctx, const Type *a, const Type *b) {
    for (int i = 0; i < ctx->n_assume; i++)
        if (ctx->assume_a[i] == a && ctx->assume_b[i] == b) return true;
    return false;
}

static int sess_proto_eq(const Type *a, const Type *b, SessEqCtx *ctx) {
    if (a == b) return 1;
    if (!a || !b) return 1;          /* a missing protocol tells us nothing */

    /* Resolve a back-reference to the binder it names, then re-compare. */
    if (sess_is_backref(a)) {
        const Type *bind = sess_lookup(ctx->env_a, ctx->n_env_a, a->as.session_.label);
        if (!bind) return 1;         /* unbound -- cannot judge, stay permissive */
        if (sess_assumed(ctx, bind, b)) return 1;
        return sess_proto_eq(bind, b, ctx);
    }
    if (sess_is_backref(b)) {
        const Type *bind = sess_lookup(ctx->env_b, ctx->n_env_b, b->as.session_.label);
        if (!bind) return 1;
        if (sess_assumed(ctx, a, bind)) return 1;
        return sess_proto_eq(a, bind, ctx);
    }

    /* A `Rec` binder on either side is compared against the other side's
     * unfolding, which is what makes folded and unfolded spellings agree. */
    bool rec_a = sess_is_rec_binder(a), rec_b = sess_is_rec_binder(b);
    if (rec_a || rec_b) {
        if (sess_assumed(ctx, a, b)) return 1;
        if (ctx->n_assume >= SESS_EQ_MAX) return 1;   /* too deep -- permissive */
        int save_a = ctx->n_env_a, save_b = ctx->n_env_b, save_as = ctx->n_assume;
        ctx->assume_a[ctx->n_assume] = a;
        ctx->assume_b[ctx->n_assume] = b;
        ctx->n_assume++;
        if (rec_a) {
            if (ctx->n_env_a >= SESS_EQ_MAX) return 1;
            ctx->env_a[ctx->n_env_a].label  = a->as.session_.label;
            ctx->env_a[ctx->n_env_a].binder = a;
            ctx->n_env_a++;
        }
        if (rec_b) {
            if (ctx->n_env_b >= SESS_EQ_MAX) return 1;
            ctx->env_b[ctx->n_env_b].label  = b->as.session_.label;
            ctx->env_b[ctx->n_env_b].binder = b;
            ctx->n_env_b++;
        }
        const Type *ua = rec_a ? a->as.session_.fst : a;
        const Type *ub = rec_b ? b->as.session_.fst : b;
        int r = sess_proto_eq(ua, ub, ctx);
        ctx->n_env_a = save_a; ctx->n_env_b = save_b; ctx->n_assume = save_as;
        return r;
    }

    if (a->kind != b->kind) return 0;

    switch (a->kind) {
        case TY_CLOSE:
            return 1;
        case TY_SEND:
        case TY_RECV:
            /* fst is the MESSAGE type (an ordinary type), snd the continuation. */
            if (a->as.session_.fst && b->as.session_.fst &&
                !type_eq(*a->as.session_.fst, *b->as.session_.fst))
                return 0;
            return sess_proto_eq(a->as.session_.snd, b->as.session_.snd, ctx);
        case TY_CHOOSE:
        case TY_BRANCH:
        case TY_TIMEOUT:
            /* Both slots are protocols (the two branches / the two outcomes). */
            if (!sess_proto_eq(a->as.session_.fst, b->as.session_.fst, ctx)) return 0;
            return sess_proto_eq(a->as.session_.snd, b->as.session_.snd, ctx);
        default:
            /* TY_UNKNOWN, a tyvar standing in for a protocol, anything the
             * session elaborator has not ground yet: not our business. */
            return 1;
    }
}

int type_eq(Type a, Type b) {
    /* CRU Phase 3 / Option B (B-1): a boxed TY_FN (a first-class closure
     * value) and TY_PTR_VOID share the same C carrier -- a void* holding the
     * { thunk, env... } box.  Treat them as interchangeable so closures flow
     * through legacy :ptr<void> sinks (params, returns, lets, struct fields)
     * with no explicit coercion node and no codegen change.  This preserves
     * the pre-B-1 world, where capturing closures *were* TY_PTR_VOID, so any
     * site that accepted TY_PTR_VOID accepted closures. */
    {
        bool a_box = (a.kind == TY_FN && a.as.fn.boxed);
        bool b_box = (b.kind == TY_FN && b.as.fn.boxed);
        if ((a_box && b.kind == TY_PTR_VOID) || (b_box && a.kind == TY_PTR_VOID))
            return 1;
    }
    if (a.kind != b.kind) return 0;
    /* Phase 13: Check lifetime annotations - only if either has lifetimes */
    if (a.n_lifetimes > 0 || b.n_lifetimes > 0) {
        if (!lifetimes_eq(a.lifetimes, a.n_lifetimes, b.lifetimes, b.n_lifetimes)) {
            return 0;
        }
    }
    if (a.kind == TY_FN) {
        /* typed-c-abi-function-pointers: a cfnptr is a bare C-ABI function
         * pointer.  It unifies with another cfnptr, or with a *captureless*
         * (non-boxed) bare fn of the same signature -- because a captureless
         * fn already IS a bare code pointer at the C ABI -- but never with a
         * *capturing* (boxed) closure value.  This is what keeps a fat closure
         * from silently flowing into a raw C callback sink. */
        if (a.as.fn.cfnptr || b.as.fn.cfnptr) {
            if ((a.as.fn.cfnptr && b.as.fn.boxed) ||
                (b.as.fn.cfnptr && a.as.fn.boxed))
                return 0;
        }
        if (a.as.fn.arity != b.as.fn.arity) return 0;
        for (uint32_t i = 0; i < a.as.fn.arity; i++) {
            if (a.as.fn.arg_kinds[i] != b.as.fn.arg_kinds[i])
                return 0;
        }
        return a.as.fn.result_kind == b.as.fn.result_kind;
    }
    if (a.kind == TY_REF) {
        return a.as.ref.inner == b.as.ref.inner;
    }
    /* ptr-generic-parameterised-type: typed ptr<T>.  Two typed pointers are
     * equal iff their pointee types are equal.  The untyped ptr<void> (inner
     * NULL) stays interoperable with any pointer for back-compat -- the whole
     * runtime threads raw handles through ptr<void> sinks. */
    if (a.kind == TY_PTR_VOID) {
        if (!a.as.ptr.inner || !b.as.ptr.inner) return 1;
        return type_eq(*a.as.ptr.inner, *b.as.ptr.inner);
    }
    /* Phase 9: rc<T> and weak<T> */
    if (a.kind == TY_RC || a.kind == TY_WEAK) {
        /* stdlib-weak-ref-audit WR1: an rc/weak inner is a bare TypeKind, not a
         * full Type, so `rc<A>` over a type parameter lowers to
         * `type_rc(TY_TYVAR)` with the variable's NAME already erased.  Two
         * distinct tyvars (`rc<A>` vs `rc<B>`) were therefore already equal
         * here; what was not, and should be, is a tyvar inner against a
         * concrete one.  Without it every polymorphic rc/weak function returned
         * an un-instantiable `rc<tyvar>`:
         *
         *   (defn rc/downgrade [A] [^borrow r : rc<A>] : weak<A> (weak r))
         *   (set! (.parent child) (rc/downgrade parent))
         *   ; error: value type weak<tyvar> does not match field type weak<<adt>>
         *
         * so a generic wrapper over `weak` could not install the back-edge that
         * weak<T> exists for, and callers had to use the raw intrinsic instead.
         * Since the name is erased there is no binding to check against; the
         * tyvar is an unrefined pointee on an identical carrier
         * (RcControlBlock *), so it unifies with any concrete inner.  Two
         * CONCRETE inners are still compared strictly. */
        if (a.as.rc.inner == TY_TYVAR || b.as.rc.inner == TY_TYVAR) return 1;
        return a.as.rc.inner == b.as.rc.inner;
    }
    /* Phase 12: Borrow types */
    if (a.kind == TY_REF_IMMUT || a.kind == TY_REF_MUT) {
        if (a.as.ref_borrow.target != b.as.ref_borrow.target) return 0;
        /* borrowed-aggregate-key-skips-the-key-check: two borrows of the same
         * aggregate KIND are the same type only when their targets are --
         * `&Pt` is not `&String`.  A side that never recorded its target
         * (an imported signature, an owning-ref reborrow) compares by kind,
         * as every borrow did before target_full existed. */
        if (a.as.ref_borrow.target_full && b.as.ref_borrow.target_full)
            return type_eq(*a.as.ref_borrow.target_full, *b.as.ref_borrow.target_full);
        return 1;
    }
    /* Phase 15: Typeclass types */
    if (a.kind == TY_TYPECLASS) {
        return a.as.typeclass.typeclass == b.as.typeclass.typeclass;
    }
    if (a.kind == TY_TYPECLASS_INST) {
        return a.as.typeclass_inst.instance == b.as.typeclass_inst.instance;
    }
    /* Phase 17: Exception types */
    if (a.kind == TY_EXCEPTION) {
        return a.as.exn.payload_type == b.as.exn.payload_type;
    }
    /* Phase 18: Continuation types */
    if (a.kind == TY_CONT) {
        return a.as.cont.returns == b.as.cont.returns;
    }
    if (a.kind == TY_CLONEABLE_CONT) {
        return a.as.cont.returns == b.as.cont.returns;
    }
    /* ET3/FH4.1: Handler types -- equal when they handle the same effect *set*
     * (compared by name, order-insensitive) and agree on value/result kinds. */
    if (a.kind == TY_HANDLER) {
        bool rows_eq;
        if (a.as.handler_.handled_row || b.as.handler_.handled_row) {
            rows_eq = effect_row_name_set_eq(a.as.handler_.handled_row,
                                             b.as.handler_.handled_row);
        } else {
            rows_eq = (a.as.handler_.effect_name == b.as.handler_.effect_name);
        }
        /* TY_UNKNOWN value/result kinds act as wildcards: a composed handler's
         * value summary is unconstrained (FH4.1), so it matches any concrete
         * kind on the other side. */
        bool vk_ok = a.as.handler_.value_kind == b.as.handler_.value_kind
                  || a.as.handler_.value_kind == TY_UNKNOWN
                  || b.as.handler_.value_kind == TY_UNKNOWN;
        bool rk_ok = a.as.handler_.result_kind == b.as.handler_.result_kind
                  || a.as.handler_.result_kind == TY_UNKNOWN
                  || b.as.handler_.result_kind == TY_UNKNOWN;
        return rows_eq && vk_ok && rk_ok;
    }
    /* Named type variables -- compare by name pointer (interned strings, so
     * pointer equality is name equality).  Two unnamed tyvars are still equal
     * (the historical default); but two distinctly-named tyvars are NOT.  This
     * is load-bearing for Direction A of
     * docs/archive/history/open-binder-skolems-not-distinguishable.md, where each
     * `open` mints a fresh skolem-named tyvar and the call-side unifier must
     * reject mismatches across nested opens. */
    if (a.kind == TY_TYVAR) {
        if (!a.as.tyvar_.name || !b.as.tyvar_.name) return 1;
        return a.as.tyvar_.name == b.as.tyvar_.name
            || (strcmp(a.as.tyvar_.name, b.as.tyvar_.name) == 0);
    }
    /* Phase G0: ADT types - identity by AdtDef pointer */
    if (a.kind == TY_ADT) {
        return a.as.adt_.def == b.as.adt_.def;
    }
    /* Phase HKT-P1: Type application - compare fn and arg */
    if (a.kind == TY_APP) {
        if (!a.as.app.fn || !b.as.app.fn) return a.as.app.fn == b.as.app.fn;
        if (!a.as.app.arg || !b.as.app.arg) return a.as.app.arg == b.as.app.arg;
        /* constrained-hkt-abstract-var-requires-last-param-free: `(Result _ B)`
         * and the ordinary application `(Result B)` share fn and arg but denote
         * different constructors, so the hole slot is part of identity. */
        if (a.as.app.hole_pos_p1 != b.as.app.hole_pos_p1) return false;
        return type_eq(*a.as.app.fn, *b.as.app.fn) && type_eq(*a.as.app.arg, *b.as.app.arg);
    }
    /* Phase HKT-P2: Recursive types - identity by name pointer (interned) */
    if (a.kind == TY_REC) {
        return a.as.rec.name == b.as.rec.name;
    }
    /* CT0 / concrete-codegen-layout-kind-enumerations-drift Finding 2: two
     * contract types are the same type only when they refine the same BASE
     * type.  Without this they fell through to the `return 1` below and EVERY
     * pair of contracts compared equal -- while `type_c_name` delegates to the
     * base, so `{ x : int | .. }` and `{ y : float | .. }` had different C
     * layouts.  `type_register_adt_app` keys its registry on `type_eq`, so
     * `(Box { x : int | .. })` and `(Box { y : float | .. })` shared one
     * registry entry and one `tur_adt_Box__contract` typedef: the surviving
     * definition stored `int64_t` while the float arm's match read it back with
     * a `(double)` conversion.  Predicates are deliberately NOT compared: they
     * are checked at run time and never C-visible, so `{ x : int | (> x 0) }`
     * and `{ x : int | (< x 0) }` stay interchangeable exactly as before. */
    if (a.kind == TY_CONTRACT) {
        if (!a.as.contract_.base_type || !b.as.contract_.base_type)
            return a.as.contract_.base_type == b.as.contract_.base_type;
        return type_eq(*a.as.contract_.base_type, *b.as.contract_.base_type);
    }
    /* Phase HRT0/EX1b: Quantified types — structural equality on n_vars, body, and
     * constraint set.  Bound variable names are not significant (alpha-renaming);
     * constraints are compared by (typeclass identity, bound-var index) tuple. */
    if (a.kind == TY_FORALL || a.kind == TY_EXISTS) {
        if (a.as.forall_.n_vars != b.as.forall_.n_vars) return 0;
        if (a.as.forall_.n_constraints != b.as.forall_.n_constraints) return 0;
        for (uint8_t i = 0; i < a.as.forall_.n_constraints; i++) {
            TypeClass *ca = a.as.forall_.constraint_classes
                                ? a.as.forall_.constraint_classes[i] : NULL;
            TypeClass *cb = b.as.forall_.constraint_classes
                                ? b.as.forall_.constraint_classes[i] : NULL;
            if (ca != cb) return 0;
            uint8_t ia = a.as.forall_.constraint_var_idx
                                ? a.as.forall_.constraint_var_idx[i] : 0;
            uint8_t ib = b.as.forall_.constraint_var_idx
                                ? b.as.forall_.constraint_var_idx[i] : 0;
            if (ia != ib) return 0;
        }
        if (!a.as.forall_.body || !b.as.forall_.body)
            return a.as.forall_.body == b.as.forall_.body;
        return type_eq(*a.as.forall_.body, *b.as.forall_.body);
    }
    /* IT0: Union types — structural equality: same n_members, each member equal */
    if (a.kind == TY_UNION) {
        if (a.as.union_.n_members != b.as.union_.n_members) return 0;
        for (uint8_t i = 0; i < a.as.union_.n_members; i++) {
            if (!a.as.union_.members[i] || !b.as.union_.members[i]) {
                if (a.as.union_.members[i] != b.as.union_.members[i]) return 0;
                continue;
            }
            if (!type_eq(*a.as.union_.members[i], *b.as.union_.members[i])) return 0;
        }
        return 1;
    }
    /* IT2: Intersection types — structural equality: same n_members, each member equal */
    if (a.kind == TY_INTERSECTION) {
        if (a.as.intersection_.n_members != b.as.intersection_.n_members) return 0;
        for (uint8_t i = 0; i < a.as.intersection_.n_members; i++) {
            if (!a.as.intersection_.members[i] || !b.as.intersection_.members[i]) {
                if (a.as.intersection_.members[i] != b.as.intersection_.members[i]) return 0;
                continue;
            }
            if (!type_eq(*a.as.intersection_.members[i], *b.as.intersection_.members[i])) return 0;
        }
        return 1;
    }
    /* Variadic HKT rows: order-SENSITIVE structural equality. Two rows are
     * equal iff they have the same length and equal element types at each
     * position. (Permutation-insensitive equality is type_typerow_eq_perm.)
     * Without this explicit case a row would fall through to `return 1` and
     * any two rows would compare equal -- a silent miscompile. */
    if (a.kind == TY_TYPEROW) {
        if (a.as.typerow_.n_elements != b.as.typerow_.n_elements) return 0;
        /* P0 typed-field rows: if either side has field names, both must,
         * and names must agree positionally. A bare row and a typed-field
         * row are distinct types even if the element types coincide. */
        const char **an = a.as.typerow_.field_names;
        const char **bn = b.as.typerow_.field_names;
        if ((an == NULL) != (bn == NULL)) return 0;
        for (uint8_t i = 0; i < a.as.typerow_.n_elements; i++) {
            if (!a.as.typerow_.elements[i] || !b.as.typerow_.elements[i]) {
                if (a.as.typerow_.elements[i] != b.as.typerow_.elements[i]) return 0;
                continue;
            }
            if (!type_eq(*a.as.typerow_.elements[i], *b.as.typerow_.elements[i])) return 0;
            if (an && bn) {
                if (!an[i] || !bn[i]) {
                    if (an[i] != bn[i]) return 0;
                } else if (strcmp(an[i], bn[i]) != 0) {
                    return 0;
                }
            }
        }
        return 1;
    }
    /* session-type-eq-ignores-the-protocol: a session endpoint carries its
     * protocol in `session_.fst`, and without this case two Session types fell
     * through to the `return 1` below -- so any Session[P] compared equal to
     * any Session[Q] and a protocol mismatch across a call boundary was
     * accepted silently. Same defect the TY_TYPEROW case above exists to
     * prevent.
     *
     * The three internal pair kinds (TY_SESSION_PAIR, TY_SESSION_RECV_PAIR,
     * TY_SESSION_OFFER) deliberately stay permissive: they are transient
     * elaborator products that are always destructured on the spot, never a
     * type a user writes on a parameter, so they are not part of the boundary
     * this fixes. */
    if (a.kind == TY_SESSION) {
        SessEqCtx ctx = {0};
        return sess_proto_eq(a.as.session_.fst, b.as.session_.fst, &ctx);
    }
    if (a.kind == TY_SEND   || a.kind == TY_RECV   || a.kind == TY_CLOSE ||
        a.kind == TY_CHOOSE || a.kind == TY_BRANCH ||
        a.kind == TY_SESSION_REC || a.kind == TY_TIMEOUT) {
        SessEqCtx ctx = {0};
        return sess_proto_eq(&a, &b, &ctx);
    }
    /* Multi-party endpoints have the same hole as binary sessions did: without
     * this case, `(Role Ping A)` and `(Role Ping B)` -- or two roles of two
     * different protocols -- compared equal, so handing a function the wrong
     * participant's endpoint was accepted silently.
     *
     * Compares the protocol and the role name, and deliberately NOT
     * `current_step`: a role endpoint partway through its interaction tree is
     * still the same role of the same protocol, and comparing positions would
     * reject a recursive call the way it advances its own step. Both names are
     * interned, so a pointer compare is the fast path and strcmp is only the
     * fallback for a non-interned spelling. */
    if (a.kind == TY_ROLE) {
        const char *ar = a.as.role_.role_name, *br = b.as.role_.role_name;
        if (ar != br && (!ar || !br || strcmp(ar, br) != 0)) return 0;
        const Type *ag = a.as.role_.global_type, *bg = b.as.role_.global_type;
        if (ag && bg && ag != bg &&
            ag->kind == TY_GLOBAL && bg->kind == TY_GLOBAL) {
            const char *an2 = ag->as.global_.name, *bn2 = bg->as.global_.name;
            if (an2 != bn2 && (!an2 || !bn2 || strcmp(an2, bn2) != 0)) return 0;
        }
        return 1;
    }
    return 1;
}

/* LT2: Check whether function type `actual' is compatible with function type
 * `expected' with respect to arg_linear constraints.  Returns 1 if compatible,
 * 0 if there is a linearity mismatch.
 *
 * Linearity is invariant for function types:
 *   - if expected.arg_linear[i] is true, actual.arg_linear[i] must also be true
 *     (you cannot pass a non-consuming function where a consuming one is required)
 *   - if expected.arg_linear[i] is false, actual.arg_linear[i] must also be false
 *     (you cannot pass a consuming function where a non-consuming one is required,
 *     because the caller would not know to treat the argument as consumed)
 *
 * Called from elab_call_fn when -Xlinear is enabled and higher-order functions
 * are passed as arguments. */
int fn_type_subtype(Type actual, Type expected) {
    if (actual.kind != TY_FN || expected.kind != TY_FN) return 1;
    if (actual.as.fn.arity != expected.as.fn.arity) return 1; /* arity mismatch caught elsewhere */
    for (uint32_t i = 0; i < actual.as.fn.arity; i++) {
        if (FN_ARG_FLAG(actual.as.fn, i, FA_LINEAR) != FN_ARG_FLAG(expected.as.fn, i, FA_LINEAR)) return 0;
    }
    return 1;
}

/* stdlib-int-stand-in-audit S1: STRUCTURAL compatibility of two function types
 * -- arity, argument kinds, result kind.
 *
 * The comment above says "arity mismatch caught elsewhere". It is not: a
 * parameter declared `(fn [int] int)` accepted `(fn [cstr] cstr)` with exit 0,
 * because the call-argument check compares TY_FN to TY_FN by KIND and the only
 * refinement was the linearity pass above. That is the real mechanism behind
 * S1's "38 callback parameters whose signature is entirely unchecked" -- and it
 * means spelling the handler type out, which the report proposed as the fix,
 * documents intent without enforcing anything.
 *
 * Deliberately permissive where the checker cannot know:
 *   - either side a tyvar / unknown / any slot: accept (generic code passes
 *     functions whose shape is only fixed per monomorphization);
 *   - expected side lacking recorded full types: accept (nothing to compare).
 * Only a concrete-vs-concrete disagreement is rejected. */
static int fn_slot_same_carrier(TypeKind a, TypeKind b) {
    /* Compare CARRIER CLASS, not exact kind. Under the hybrid ABI an ADT, a
     * type-application, a struct handle, an opaque and a ptr<void> all ride the
     * same int64 word, so `(fn [(Option String)] bool)` genuinely does satisfy a
     * slot recorded as `(fn [int] bool)` -- rejecting that broke
     * niche-elem-comparator-conventions and backtrack-mbind-captureless-lambda,
     * both of which are correct programs.
     *
     * What survives is what the hybrid ABI actually distinguishes: ARITY, and a
     * float (a `double` register) against a word. That is the tour-tourist class
     * of mistake -- `(fn [req : int] : int)` where `(fn [ctx : Ctx] ...)` was
     * meant is an arity/shape error far more often than a same-arity word swap. */
    if (a == b) return 1;
    return (a != TY_FLOAT) && (b != TY_FLOAT);
}

static int fn_slot_is_wildcard(const Type *full, TypeKind lowered) {
    /* A slot the checker must not judge. The FULL type is authoritative: a
     * generic `(fn [a] b)` lowers its tyvar slots to the int64 CARRIER kind, so
     * arg_kinds alone reads as a concrete `int` and would be compared against a
     * typed sink's `float`. That is the erased-fn-param-to-typed-fat-sink shape,
     * which is supported -- the poly-to-fat box's slot-0 shim bridges the bits. */
    TypeKind k = full ? full->kind : lowered;
    return k == TY_TYVAR || k == TY_UNKNOWN || k == TY_ANY ||
           k == TY_FORALL || k == TY_EXISTS;
}

int fn_type_structurally_compatible(Type actual, Type expected) {
    if (actual.kind != TY_FN || expected.kind != TY_FN) return 1;
    /* Arity first: a nullary fn has no arg_kinds array at all, so testing that
     * array before the arity would let `(fn [] int)` satisfy `(fn [int] int)`. */
    if (actual.as.fn.arity != expected.as.fn.arity) return 0;
    /* A nullary fn has no arg_kinds array either, and that is not "nothing
     * to compare": its result still is.  Returning early here let
     * `(fn [] float)` satisfy `(fn [] int)` -- the result check below never
     * ran for any nullary function. */
    if (actual.as.fn.arity > 0 &&
        (!actual.as.fn.arg_kinds || !expected.as.fn.arg_kinds)) return 1;
    for (uint32_t i = 0; i < actual.as.fn.arity; i++) {
        const Type *af = actual.as.fn.arg_full_types
            ? actual.as.fn.arg_full_types[i] : NULL;
        const Type *ef = expected.as.fn.arg_full_types
            ? expected.as.fn.arg_full_types[i] : NULL;
        TypeKind ak = (TypeKind)actual.as.fn.arg_kinds[i];
        TypeKind ek = (TypeKind)expected.as.fn.arg_kinds[i];
        if (fn_slot_is_wildcard(af, ak) || fn_slot_is_wildcard(ef, ek)) continue;
        if (!fn_slot_same_carrier(ak, ek)) return 0;
    }
    {
        const Type *af = actual.as.fn.result_full_type;
        const Type *ef = expected.as.fn.result_full_type;
        TypeKind ak = actual.as.fn.result_kind;
        TypeKind ek = expected.as.fn.result_kind;
        if (!fn_slot_is_wildcard(af, ak) && !fn_slot_is_wildcard(ef, ek) &&
            !fn_slot_same_carrier(ak, ek))
            return 0;
        /* word-result-fn-into-nil-slot: a `nil` result is `void` in C, which
         * is neither carrier, so a word-returning function in a `(fn [...] nil)`
         * slot is called through the wrong function type (a trap under
         * -fsanitize=function and WASM's call_indirect).  The reverse -- a
         * nil-returning function where a word is expected -- is bridged by the
         * nil_result_word shim, so only this direction is refused.  A written
         * `: nil` / `: void` makes a defn void (elab_defn), so a function
         * DECLARED void is never caught by this. */
        if (!fn_slot_is_wildcard(af, ak) && !fn_slot_is_wildcard(ef, ek) &&
            ek == TY_NIL && ak != TY_NIL && ak != TY_NEVER)
            return 0;
    }
    return 1;
}

/* Helper to create a Type from TypeKind.
 * Zero-initialises the entire struct so that compound type fields (union_,
 * intersection_, etc.) are safe to pass to type_name() even for kinds that
 * store no additional data. */
static Type type_from_kind(TypeKind k) {
    Type t;
    memset(&t, 0, sizeof(t));
    t.kind = k;
    t.copy_kind = typekind_default_copy_kind(k);
    t.hkt_kind = KIND_STAR;  /* Phase HKT-P6: all types are kind * in v1 */
    return t;
}

/* Phase 13: Helper to format lifetime annotations */
static void lifetime_format(Buf *b, LifetimeId id) {
    /* Format lifetime as 'a, 'b, 'c, etc. based on ID */
    buf_putc(b, '\'');
    buf_putc(b, 'a' + (id - 1));  /* ID 1 -> 'a, ID 2 -> 'b, etc. */
}

/* Phase 13: Helper to format lifetimes for a type */
static void lifetimes_format(Buf *b, Type t) {
    if (t.n_lifetimes == 0) return;
    buf_putc(b, '<');
    for (uint8_t i = 0; i < t.n_lifetimes; i++) {
        if (i > 0) buf_putc(b, ',');
        lifetime_format(b, t.lifetimes[i]);
    }
    buf_putc(b, '>');
}

static void type_name_buf(Buf *b, Type t);

/* TS4P1: Registered polymorphic ADT application (monomorphised instance).
 * (The former struct-app registry was removed in structdef-retirement DS-D.) */
typedef struct RegisteredAdtApp {
    Type         type;     /* the TY_APP type (cloned via clone_struct_app_type) */
    char        *name;     /* mangled C typedef name, e.g. "tur_adt_Maybe__float" */
    bool         emitted;
    bool         emitting;
    /* self-typed-heap-parametric-field-unsupported: set while this entry's
     * ctor signatures are being recorded.  Recording c-names every field, and
     * a `:heap` self-typed field (`(next (Node A))` in `Node`) c-names to the
     * very app being registered -- so without a guard the lookup re-enters
     * record_adt_app_ctor_sigs on the entry it is in the middle of filling
     * and the stack goes (ASan DEADLYSIGNAL on `tur check`). */
    bool         recording;
} RegisteredAdtApp;

static RegisteredAdtApp *g_adt_apps = NULL;
static uint32_t g_n_adt_apps = 0;
static uint32_t g_cap_adt_apps = 0;

/* Phase E: Registry for typed function-pointer typedefs used as struct field types.
 * Each entry tracks one concrete fn type and its generated typedef name. */
typedef struct RegisteredFnPtrTypedef {
    char    *typedef_name;            /* e.g. "tur_fnptr_int64_t_int32_t_t" */
    TypeKind result_kind;
    uint8_t  arity;
    TypeKind arg_kinds[MAX_FN_ARITY];
    /* c-fn-ptr-element-and-size-precision-gap fix: per-arg full Type
     * (interned in the type arena, lives for the whole compilation) so a
     * `ptr<T>` parameter on a (c-fn ...) lowers to `T *` instead of the
     * lossy `void *` that type_from_kind would give us.  NULL entries
     * (typical for non-cfnptr fn-pointer typedefs and for primitives)
     * fall back to type_from_kind(arg_kinds[i]). */
    Type    *arg_full_types[MAX_FN_ARITY];
    /* c-fn-ptr-element-and-size-precision-gap fix: per-cfnptr full result Type
     * (NULL otherwise), so a size_t / element-typed / const-qualified return
     * lowers precisely instead of via type_from_kind(result_kind). */
    Type    *result_full_type;
    bool     emitted;
} RegisteredFnPtrTypedef;

static RegisteredFnPtrTypedef *g_fn_ptr_typedefs = NULL;
static uint32_t g_n_fn_ptr_typedefs = 0;
static uint32_t g_cap_fn_ptr_typedefs = 0;

/* TS4P1: Extract an ADT-headed TY_APP chain into (AdtDef*, args[], n_args). */
bool type_extract_adt_app(const Type *t, AdtDef **out_def,
                          Type *out_args, uint8_t *out_n) {
    if (!t) return false;
    Type raw[16];
    uint8_t n_raw = 0;
    const Type *cur = t;
    while (cur && cur->kind == TY_APP && n_raw < 16) {
        if (!cur->as.app.arg) return false;
        raw[n_raw++] = *cur->as.app.arg;
        cur = cur->as.app.fn;
    }
    if (!cur || cur->kind != TY_ADT || !cur->as.adt_.def) return false;
    AdtDef *def = cur->as.adt_.def;
    if (def->n_type_params == 0 || n_raw != def->n_type_params) return false;
    if (out_def) *out_def = def;
    if (out_n)   *out_n = n_raw;
    if (out_args) {
        for (uint8_t i = 0; i < n_raw; i++) out_args[i] = raw[n_raw - 1 - i];
    }
    return true;
}

/* ============================================================================
 * Increment 4 stage 1 (repr-decision-function-plan): the SIMPLE-KIND
 * REPRESENTATION TABLE -- the single place a payload-free TypeKind's three
 * representation answers live:
 *
 *     X(kind,  C type name,  mangle token,  has-concrete-codegen-layout)
 *
 * The three per-kind switches (`type_c_name`, `append_type_mangle`,
 * `type_has_concrete_codegen_layout`) each expand these rows with their own
 * projection, so adding a simple kind is ONE row giving all three answers at
 * once -- the drift that produced `map-show-keyword-key-raw-int` (TY_SYM
 * present in type_c_name but absent from the layout switch, silently losing
 * `(Vec Sym)`'s by-value monomorph to the int64 carrier) and both findings of
 * `concrete-codegen-layout-kind-enumerations-drift` is structurally closed
 * for this class of kind.  Kinds whose answer depends on a PAYLOAD (inner
 * types, defs, arities: TY_PTR_VOID, TY_FN, TY_ADT, TY_APP, the ref family,
 * ...) keep hand-written arms in each switch; -Wswitch still forces every
 * enum member to appear in every switch either as a row here or as an arm
 * there.  tests/check-typekind-mangle-exhaustive.sh reads this table plus
 * the residual arms and re-checks exhaustiveness, no-default, and mangle
 * injectivity on every run.
 *
 * Row notes:
 * - TY_SYM is a pointer-sized scalar (`const struct __tur_sym *`) -- as
 *   concrete a codegen layout as cstr (map-show-keyword-key-raw-int).
 * - TY_UNKNOWN / TY_TYVAR deliberately SHARE the mangle token "struct" (the
 *   def-less placeholder convention; see the guard's BARE_OK list) while
 *   keeping distinct C names.
 * - TY_ANY / TY_UNION c-name to the two-word `tur_tagged_t` but are NOT
 *   concrete-layout (a 16-byte by-value field is an ABI change, not a table
 *   edit); TY_UNION mangles its members, so only TY_ANY is a row here.
 * - The session-protocol descriptors and other compile-time-only kinds
 *   c-name to comment-void placeholders; none is ever a runtime value.
 * ==========================================================================*/
#define TY_SIMPLE_REPR_ROWS(X) \
    X(TY_NIL,               "void",                     "nil",               true)  \
    X(TY_BOOL,              "bool",                     "bool",              true)  \
    X(TY_INT,               "int64_t",                  "int",               true)  \
    X(TY_FLOAT,             "double",                   "float",             true)  \
    X(TY_INT8,              "int8_t",                   "int8",              true)  \
    X(TY_INT16,             "int16_t",                  "int16",             true)  \
    X(TY_INT32,             "int32_t",                  "int32",             true)  \
    X(TY_INT64,             "int64_t",                  "int64",             true)  \
    X(TY_UINT8,             "uint8_t",                  "uint8",             true)  \
    X(TY_UINT16,            "uint16_t",                 "uint16",            true)  \
    X(TY_UINT32,            "uint32_t",                 "uint32",            true)  \
    X(TY_UINT64,            "uint64_t",                 "uint64",            true)  \
    X(TY_FLOAT32,           "float",                    "float32",           true)  \
    X(TY_FLOAT64,           "double",                   "float64",           true)  \
    X(TY_CSTR,              "const char *",             "cstr",              true)  \
    X(TY_SYM,               "const struct __tur_sym *", "sym",               true)  \
    X(TY_SET,               "tur_set_t *",              "set",               true)  \
    X(TY_ROLE,              "void *",                   "role",              true)  \
    X(TY_SESSION,           "void *",                   "session",           true)  \
    X(TY_GENERATOR,         "void *",                   "generator",         true)  \
    X(TY_NEVER,             "void",                     "never",             false) \
    X(TY_ANY,               "tur_tagged_t",             "any",               false) \
    X(TY_STRUCT,            "int64_t",                  "structdef",         false) \
    X(TY_UNKNOWN,           "/*unknown*/ void",         "struct",            false) \
    X(TY_TYVAR,             "int64_t",                  "struct",            false) \
    X(TY_DYNVAR,            "/*dynvar*/ void",          "dynvar",            false) \
    X(TY_GLOBAL,            "/*global-protocol*/ void", "global",            false) \
    X(TY_SEND,              "/*session-protocol*/ void", "send",             false) \
    X(TY_RECV,              "/*session-protocol*/ void", "recv",             false) \
    X(TY_CLOSE,             "/*session-protocol*/ void", "close",            false) \
    X(TY_CHOOSE,            "/*session-protocol*/ void", "choose",           false) \
    X(TY_BRANCH,            "/*session-protocol*/ void", "branch",           false) \
    X(TY_SESSION_REC,       "/*session-protocol*/ void", "session_rec",      false) \
    X(TY_TIMEOUT,           "/*session-protocol*/ void", "timeout",          false) \
    X(TY_SESSION_PAIR,      "void *",                   "session_pair",      false) \
    X(TY_SESSION_RECV_PAIR, "void *",                   "session_recv_pair", false) \
    X(TY_SESSION_OFFER,     "int64_t",                  "session_offer",     false) \
    X(TY_SYNTAX,            "/*syntax*/ void",          "syntax",            false)

bool type_has_concrete_codegen_layout(const Type *t) {
    if (!t) return false;
    switch (t->kind) {
#define X(k, cn, mg, lay) case k: return lay;
        TY_SIMPLE_REPR_ROWS(X)
#undef X
        case TY_PTR_VOID:
        case TY_REF:
        case TY_LREF:
        case TY_RC:
        case TY_WEAK:
        case TY_REF_IMMUT:
        case TY_REF_MUT:
        case TY_ADT:
        case TY_FN:
        case TY_EXCEPTION:
        case TY_CONT:
        case TY_CLONEABLE_CONT:
        case TY_HANDLER:
            return true;
        /* concrete-codegen-layout-kind-enumerations-drift Finding 2 proposed
         * delegating this to the base type, since type_c_name does.  Done: a
         * contract IS its base type at the representation level, and saying so
         * here keeps this switch agreeing with type_c_name instead of quietly
         * dropping a refined value onto the int64 carrier.
         *
         * The blocker was that admitting contracts split one carrier into two
         * by-value monomorphs that no crossing reconciled --
         *
         *   (defn mk [] : (Box #refine{ v : int | (> v 0) }) (MkBox 5))
         *
         * failed with `cc`: "incompatible types when returning
         * 'tur_adt_Box__int' but 'tur_adt_Box__contract_int' was expected",
         * because the ctor call is typed from its argument, `(Box int)`, and
         * nothing peeled the declared `(Box contract)` to it.  That peel now
         * exists (rt_peel_type_arg_contract, called from both type-application
         * loops), so a contract can no longer reach a type-argument slot and
         * the two names can no longer diverge.  Verified: the program above
         * compiles and prints 5.
         *
         * The collision fix never depended on this either way: the per-base
         * FIELD width comes from type_c_name, and distinct monomorph NAMES come
         * from the type_eq / append_type_mangle arms. */
        case TY_CONTRACT:
            return t->as.contract_.base_type
                 ? type_has_concrete_codegen_layout(t->as.contract_.base_type)
                 : false;
        case TY_APP:
            /* structdef-retirement DS-D: a struct-headed TY_APP can never form
             * (no Type has kind TY_STRUCT), so a parametric-struct monomorph has
             * no concrete by-value layout here.  Concrete parametric ADTs are
             * recognised separately by type_app_is_concrete_adt. */
            return false;

        /* ---- Deliberately NOT concrete.  ------------------------------------
         * There is no `default` arm, for the reason append_type_mangle has
         * none: a kind missing from this switch is not diagnosed, it silently
         * loses the by-value monomorph and falls back to the int64 carrier --
         * the map-show-keyword-key-raw-int bug, where an absent TY_SYM printed
         * a raw carrier integer with no diagnostic anywhere.  Listing every
         * kind makes -Wall's -Wswitch flag the next one at the point it is
         * added.  Simple payload-free kinds live in TY_SIMPLE_REPR_ROWS
         * (expanded above); each remaining rejection below says why.
         *
         * TY_ANY / TY_UNION rationale (Finding 2, the `tur_tagged_t` pair):
         * these DO have a real C type, and it is a TWO-WORD struct -- every
         * accepted member is one word.  Admitting them is a by-value-ABI
         * change (a 16-byte field in a monomorph, its own copy/drop
         * crossings), not a table edit, and there is no way to build one
         * today to test it: `(Vec any)` type-checks but `vec-of`'s
         * type-witness binding trips TUR-E0201 inside stdlib/vec.tur.
         * TY_ANY's row rejects; TY_UNION rejects below. */

        /* type_c_name gives both of these the plain int64_t carrier, so a
         * by-value monomorph over one would have exactly the carrier's layout
         * and buy nothing but an extra C name.  TY_INTERSECTION is documented
         * as an IT2 placeholder awaiting real codegen; revisit both if either
         * ever grows a representation of its own. */
        case TY_REC:
        case TY_INTERSECTION:
            return false;
        /* Two-word tagged union (see the TY_ANY note above). */
        case TY_UNION:
            return false;
        /* Compile-time-only kinds: type_c_name gives each a comment-void or
         * erased placeholder, so none is ever the type of a runtime value.
         * (Rows are eliminated before codegen; quantified types are erased at
         * instantiation.) */
        case TY_TYPECLASS:
        case TY_TYPECLASS_INST:
        case TY_FORALL:
        case TY_EXISTS:
        case TY_TYPEROW:
            return false;
    }
    return false;
}

/* True when `t` is a parametric ADT (`defdata` sum) application all of whose
 * type arguments have a concrete codegen layout -- e.g. `(ReF bool)`,
 * `(Cons int)`.  Such a type is monomorphized to a per-element C struct
 * (`tur_adt_ReF__bool`) whose field WIDTHS follow the element, distinct from the
 * int64-carrier `tur_adt_ReF`.  The struct-app branch of
 * type_has_concrete_codegen_layout deliberately rejects ADT apps (its callers
 * then call type_extract_struct_app), so this is a separate predicate used by
 * the M7 by-value HKT machinery to recognize a parametric-SUM result/element. */
/* Is one type ARGUMENT of an ADT application concrete enough to monomorphise?
 *
 * type_has_concrete_codegen_layout answers `true` for every TY_ADT, which is
 * right when the question is "does this value have a C representation" but
 * wrong here: an UNAPPLIED parametric ADT names a type CONSTRUCTOR, not a type.
 * Admitting it mangles `(Opt (Opt int))` -- whose inner argument was dropped
 * somewhere upstream, leaving `(Opt Opt)` -- to the suffix `__Opt`, so a ctor
 * call site names `ctor_Some__Opt` (an int64 carrier ctor) while the value in
 * hand is `tur_adt_Opt__Opt__int`.  Declining lets the caller fall through to
 * the spec-grounded suffix, which knows the real family.
 *
 * Shared by type_app_is_concrete_adt and adt_app_is_byvalue_product, whose
 * argument loops were already required to stay in lockstep -- see the comment
 * in the former.  A single helper is how they stay that way. */
static bool adt_app_type_arg_is_concrete(const Type *a) {
    if (!a) return false;
    if (a->kind == TY_ADT && a->as.adt_.def && a->as.adt_.def->n_type_params > 0)
        return false;
    if (type_has_concrete_codegen_layout(a) || adt_app_is_byvalue_product(*a))
        return true;
    /* A nested app that NAMES a real monomorph, whatever its REPRESENTATION.
     *
     * The two tests above ask "does this argument have a C layout" and "is it a
     * by-value monomorph".  A multi-variant parametric sum monomorph answers no
     * to both on the default path -- it rides the carrier -- yet
     * `tur_adt_Opt2__int` is a registered typedef all the same, and the OUTER
     * app is perfectly nameable over it.  Answering no here made
     * `(Vec (Opt2 int))` not-a-concrete-app, so the let binder fell to the int64
     * carrier while repr_of said typed heap pointer, and the repr-shadow
     * aborted the compile:
     *
     *   ICE: want=heap-ptr got=carrier-i64 ... type=(type-app Vec (type-app Opt2 int))
     *
     * A Vec of a parametric PRODUCT monomorph never hit it, because a product
     * IS by-value and passed the second test.  See
     * docs/archive/vec-of-parametric-sum-monomorph-ice.md. */
    /* vec-of-any-repr-decision-ice: `any` is a concrete type ARGUMENT.
     *
     * It c-names to `tur_tagged_t`, so `(Vec any)` is as nameable a monomorph
     * as `(Vec int)` -- but the layout table rejects TY_ANY (a 16-byte
     * by-value FIELD is an ABI change, which is a different question), so it
     * failed both tests above and `(Vec any)` was not-a-concrete-app.  The let
     * binder then fell to the int64 carrier while repr_of said typed heap
     * pointer, and the repr-shadow aborted the compile -- the identical shape,
     * and the identical ICE text, as the `(Vec (Opt2 int))` row above.
     *
     * Admitting it HERE rather than in the layout table is what keeps the
     * scope right: this predicate decides whether an application NAMES a
     * monomorph, which is the question that was answered wrongly.  Whether an
     * `any` may be a by-value monomorph FIELD is untouched -- the element goes
     * through the boxed-aggregate path in repr_of instead, exactly as a
     * by-value ADT element already does. */
    if (a->kind == TY_ANY) return true;
    return a->kind == TY_APP && type_app_is_concrete_adt(a);
}

bool type_app_is_concrete_adt(const Type *t) {
    if (!t || t->kind != TY_APP) return false;
    AdtDef *def = NULL;
    Type args[16];
    uint8_t n_args = 0;
    if (!type_extract_adt_app(t, &def, args, &n_args) || !def) return false;
    /* structdef-retirement slice 5: an opaque newtype application `(Name X)` is an
     * int64 carrier with its type args erased -- never a monomorphizable concrete
     * ADT -- so it must not be registered/monomorphized. */
    if (def->is_opaque) return false;
    for (uint32_t i = 0; i < n_args; i++) {
        /* A nested by-value-product element is accepted for ANY outer, not just
         * a :heap one.  This must stay in lockstep with the identical loop in
         * adt_app_is_byvalue_product: that predicate decides the monomorph's
         * REPRESENTATION and this one decides whether a ctor CALL SITE names
         * the monomorph.  Widening one alone makes `some__spec__..._Vec__int`
         * return the by-value aggregate while its body still calls the
         * carrier `ctor_Option` -- eight fixtures fail to compile with
         * "incompatible types when returning type 'int64_t'". */
        if (!adt_app_type_arg_is_concrete(&args[i])) return false;
    }
    return true;
}

/* The AdtDef at the head of an ADT application (`(ReF bool)` -> ReF's def), or
 * NULL when `t` is not a parametric ADT application.  Lets emit recover the
 * constructed family without the static type_extract_adt_app helper. */
AdtDef *type_adt_app_def(const Type *t) {
    if (!t || t->kind != TY_APP) return NULL;
    AdtDef *def = NULL;
    Type args[16];
    uint8_t n_args = 0;
    if (!type_extract_adt_app(t, &def, args, &n_args)) return NULL;
    return def;
}

/* fmap-over-underdetermined-constructor-is-a-defless-shell: does `t` mention a
 * parameter a constructor application left open (tyvar_.open_slot)? */
bool type_has_open_slot(const Type *t) {
    if (!t) return false;
    switch (t->kind) {
        case TY_TYVAR: return t->as.tyvar_.open_slot;
        case TY_APP:
            return type_has_open_slot(t->as.app.fn) ||
                   type_has_open_slot(t->as.app.arg);
        default: return false;
    }
}

/* SC7 (carrier-duality): a "transparent int newtype" is a parametric struct
 * with a single field declared as a plain int64-width scalar (`:int`) -- e.g.
 * `(defstruct Schema [A] (raw :int))`.  Because the lone field is a concrete
 * `:int`, the type parameter is necessarily phantom (it appears in no field),
 * and the struct value carries exactly one int64.  Such a wrapper would
 * otherwise have *two* coexisting C representations -- a by-value `Schema__int`
 * aggregate and the int64 carrier ABI every parametric struct rides -- which
 * codegen mixes at different sites and breaks HKT chaining (.fmap (.fmap s g) h).
 *
 * We collapse the duality by making the wrapper a transparent newtype over
 * int64: `make-struct` and field access are identities and the C type is
 * int64_t everywhere.  The gate is deliberately narrow -- a single concrete
 * `:int` field excludes value-carrying parametric structs like
 * `(defstruct Box [A] (x A))` (whose field is the type parameter), so no
 * existing by-value parametric struct changes representation. */
/* SC7 helper: a lowered record-ADT field is a CONCRETE `:int` (not a `:a` tyvar
 * field whose carrier kind merely happens to be int64).  A parametric ADT's
 * tyvar field (`(defdata Box [a] (Box a))`) reports kind == TY_INT (the int64
 * carrier representation) but carries a TY_TYVAR full_type; such a field is a
 * genuine by-value element slot, NOT a transparent int payload, so it must be
 * rejected -- otherwise `(Box float)` would be mistaken for an int newtype and
 * its aggregate value silently collapsed to int64. */
static bool ctor_field_is_concrete_int(const CtorField *f) {
    if (!f || f->kind != TY_INT) return false;
    return f->full_type == NULL || f->full_type->kind == TY_INT;
}

/* SC7 helper: the transparent-int-newtype shape is the lowered-`defstruct`
 * RECORD form -- a single named `:int` field accessed via `.field`.  A
 * positional defdata constructor (`(defdata Fix [^f] (Roll :int))`) is a
 * genuine ADT whose values flow through `(Roll x)` / `match`, NOT field access,
 * so it must keep its tagged/flat-product representation even though it is
 * structurally a parametric single-int-field single-variant ADT.  Gating on the
 * record style (named field) is exactly what separates the lowered defstruct
 * from a hand-written positional ADT. */
static bool adt_ctor_is_transparent_int_record(const CtorDef *c) {
    return c && c->is_record && c->n_fields == 1 &&
           c->fields[0].name != NULL &&
           ctor_field_is_concrete_int(&c->fields[0]);
}

bool type_is_transparent_int_newtype(Type t) {
    if (t.kind == TY_ADT) {
        /* CONV-S1 seam 4: under the defstruct->defadt lowering the same phantom
         * int-newtype (`(defstruct Schema [A] (raw :int))`) is a single-variant
         * record ADT, not a TY_STRUCT.  Recognize that shape so the SC7
         * chainable-HKT-return propagation (and every other transparency check)
         * keeps treating it as the int64 carrier it still is at runtime. */
        AdtDef *adef = t.as.adt_.def;
        if (!adef || adef->n_type_params == 0 || adef->n_ctors != 1) return false;
        if (adef->is_heap) return false;   /* see the TY_APP arm */
        return adt_ctor_is_transparent_int_record(adef->ctors[0]);
    } else if (t.kind == TY_APP) {
        /* structdef-retirement DS-D: no struct-headed app forms, so an applied
         * transparent int newtype is always the ADT-headed shape. */
        Type args[16];
        uint8_t n_args = 0;
        AdtDef *adef = NULL;
        if (!type_extract_adt_app(&t, &adef, args, &n_args) || !adef)
            return false;
        if (adef->n_type_params == 0 || adef->n_ctors != 1) return false;
        /* phantom-parametric-heap-let-binding-repr-ice: `:heap` asks for
         * REFERENCE semantics -- a node mutated through one handle is seen
         * through every other -- and an int64 identity cannot give it that.
         * repr_of already ranks :heap first (heap-ptr in every position), so
         * collapsing `(defstruct Holder :heap [V] [payload : int])` to int64
         * here made every site this predicate steers disagree with it: the
         * binding ICE'd, and a `set!` through a generic callee would have
         * written a copy.  A :heap record stays a heap record. */
        if (adef->is_heap) return false;
        return adt_ctor_is_transparent_int_record(adef->ctors[0]);
    }
    return false;
}

/* fn-value-fat-normalization stage 1: true when a DECLARED nominal fn-typed
 * parameter type is normalized onto the fat {thunk, env} protocol -- every
 * value flowing into such a parameter is a fat handle (bare fns shimmed via
 * EX_FN_TO_FAT at the call site) and every invoke of it dispatches through
 * slot 0.  This is THE shared decision: the elaborator's call-site shim
 * (elab_call.c) and the emitter's invoke dispatch (emit_expr.c ER2) both
 * consult it, so the two sides cannot disagree.  Deliberately excluded:
 *   - cfnptr    -- a raw C function pointer must stay thin (extern-c ABI);
 *   - variadic  -- no shim family;
 *   - arity > TUR_FAT_SHIM_MAX_ARITY -- outside the __tur_fatshim family (mirrors the ^fat
 *                  auto-shim bound; such params keep today's thin protocol).
 * Carrier-eligible params never reach this predicate: elab_fns retypes them
 * to TY_PTR_VOID (is_poly_fn) before any caller asks.  ^fat params are fat
 * by their own flag; this predicate makes the NOMINAL remainder match them.
 *
 * Stage-1 measurement narrowed the claim (the CPS-graduation rule: when the
 * flip disagrees with the estimate, narrow the new path's claim rather than
 * patch the misses).  Increment 2 (2026-08-01) RESTORED the tyvar half of
 * that narrowing by doing the carrier-side work it was waiting on: a
 * tyvar-sig param's arguments arrive through the generic/carrier machinery,
 * so the two paths that feed such a slot now shim there as well --
 * `elab_poly_call` (a call THROUGH a rank-2/forall param, `l.fn(l.env, a)`)
 * and the make-struct fn-field store (which must not re-box an already
 * normalized param).  Named tyvars are therefore no longer excluded here.
 *
 * The LAST exclusion -- effect rows -- was lifted 2026-08-16 by the CPS
 * increment it was waiting on.  The load-bearing thin dependence was the E2a
 * direct-entry-keyed CPS twin registry; the via_registry call sites now
 * dispatch fat (slot 0 = a registered capturing-lambda entry with an
 * env-taking __cps twin, slot 1 = a fatshim's stashed bare-fn entry), the
 * effect_check walkers peel the EX_FN_TO_FAT shim, and threadable capturing
 * lambdas are CPS-admitted with the env unpacked exactly like the direct
 * thunk.  Effect-annotated fn params are therefore normalized like every
 * other nominal fn param.  Pinned by
 * tests/fixtures/effect-capturing-closure-thin-param/.
 *
 * Note the asymmetry with fn_result_type_is_fat_normalized below: a tyvar-sig
 * fn type is normalized in PARAM position but not as a declared RESULT.  The
 * stage-2 tail normalizer would otherwise double-box against the
 * hrt-curried-result poly-call protocol, which boxes returned closures
 * itself.  The two sides may differ safely because the call-site shim
 * normalizes whatever it is handed. */
static bool fn_sig_type_has_tyvar(const Type *t) {
    if (!t) return false;
    if (t->kind == TY_TYVAR) return true;
    if (t->kind == TY_APP)
        return fn_sig_type_has_tyvar(t->as.app.fn) ||
               fn_sig_type_has_tyvar(t->as.app.arg);
    if (t->kind == TY_FN) {
        if (fn_sig_type_has_tyvar(t->as.fn.result_full_type)) return true;
        if (t->as.fn.arg_full_types)
            for (uint32_t i = 0; i < t->as.fn.arity; i++)
                if (fn_sig_type_has_tyvar(t->as.fn.arg_full_types[i]))
                    return true;
    }
    return false;
}

static bool fn_type_sig_has_named_tyvar_(const Type *t) {
    if (t->as.fn.result_kind == TY_TYVAR) return true;
    if (fn_sig_type_has_tyvar(t->as.fn.result_full_type)) return true;
    for (uint32_t i = 0; i < t->as.fn.arity; i++) {
        if (t->as.fn.arg_kinds[i] == TY_TYVAR) return true;
        if (t->as.fn.arg_full_types &&
            fn_sig_type_has_tyvar(t->as.fn.arg_full_types[i]))
            return true;
    }
    return false;
}

bool fn_param_type_is_fat_normalized(const Type *t) {
    if (!(t && t->kind == TY_FN && !t->as.fn.cfnptr &&
          !t->as.fn.is_variadic && t->as.fn.arity <= TUR_FAT_SHIM_MAX_ARITY))
        return false;
    return true;
}

/* The RESULT-position twin: a defn whose declared result is a fn type returns
 * a fat handle (stage-2 tail normalization), and a nested fn result inside a
 * param annotation is marked boxed to match.  Narrower than the param rule --
 * see the asymmetry note above. */
bool fn_result_type_is_fat_normalized(const Type *t) {
    if (!fn_param_type_is_fat_normalized(t)) return false;
    return !fn_type_sig_has_named_tyvar_(t);
}

/* Append `name` folded to a valid C-identifier component: any non-[A-Za-z0-9_]
 * byte becomes '_'.  Mirrors mangle_field_name (emit_core.c) and
 * adt_byval_c_name so a monomorph name built here agrees, byte for byte, with
 * the base typedef the emitter produces.  A struct/ADT name may carry sigils
 * (e.g. `Lens'`); folding it here keeps the apostrophe from leaking into an
 * emitted C identifier (`tur_adt_Lens'__...` is not valid C). */
static void append_c_ident_mangled(Buf *b, const char *name) {
    /* c-keyword-function-names-not-mangled: keep this byte-for-byte in lockstep
     * with mangle_adt_name in emit_core.c, which spells the same names at the
     * declaration sites. A keyword-named ADT is not itself a C collision here
     * (every use is prefixed, `tur_adt_enum`), but if the two manglers disagree
     * the typedef and its use sites name different types.
     * separator-fold-collides-emitted-c-names: the injective scheme (mangle.h),
     * so the `_` / `__` joiners around this name are structural only -- a
     * user ADT `Foo__int` no longer spells the `(Foo int)` monomorph's name. */
    if (!name) return;
    size_t len = strlen(name);
    if (tur_name_is_c_keyword(name, len)) buf_puts(b, TUR_NAME_GUARD_PREFIX);
    char *tmp = (char *)malloc(tur_mangle_bound(len) + 1);
    if (!tmp) { fprintf(stderr, "tur: oom\n"); abort(); }
    size_t k = 0;
    tur_mangle_append(tmp, &k, name, len);
    tmp[k] = '\0';
    buf_puts(b, tmp);
    free(tmp);
}

/* Mangle a bare TypeKind by routing it through append_type_mangle, so a
 * payload kind and a top-level kind always spell the same token. */
static void append_type_mangle(Buf *b, Type t);
static void append_kind_mangle(Buf *b, TypeKind k) {
    append_type_mangle(b, type_from_kind(k));
}

static void append_u32(Buf *b, uint32_t n) {
    char tmp[12];
    snprintf(tmp, sizeof(tmp), "%u", (unsigned)n);
    buf_puts(b, tmp);
}

static void append_type_mangle(Buf *b, Type t) {
    switch (t.kind) {
        /* Simple payload-free kinds: token comes from the shared repr table
         * (TY_SIMPLE_REPR_ROWS), the single place their three representation
         * answers live.  TY_UNKNOWN / TY_TYVAR share the "struct" token by
         * design: a monomorph over either placeholder is the int64 carrier
         * instance, and routing both to one token keeps the def-emitter and
         * the call site agreed regardless of which placeholder representation
         * the elaborator left behind (the producer inconsistency that
         * surfaced the baseline-ctor-option-struct-mangling bug). */
#define X(k, cn, mg, lay) case k: buf_puts(b, mg); break;
        TY_SIMPLE_REPR_ROWS(X)
#undef X
        /* The pointer/reference family: type_eq discriminates these by their
         * INNER type, so the mangling has to as well -- a flat "ref" token made
         * `(Box (ref int))` and `(Box (ref float))` one C name. */
        case TY_PTR_VOID:
            buf_puts(b, "ptr_void");
            if (t.as.ptr.inner) { buf_putc(b, '_'); append_type_mangle(b, *t.as.ptr.inner); }
            break;
        case TY_REF:
            buf_puts(b, "ref"); buf_putc(b, '_');
            append_kind_mangle(b, t.as.ref.inner);
            break;
        case TY_LREF:
            buf_puts(b, "lref"); buf_putc(b, '_');
            append_kind_mangle(b, t.as.ref.inner);
            break;
        case TY_RC:
            buf_puts(b, "rc"); buf_putc(b, '_');
            append_kind_mangle(b, t.as.rc.inner);
            break;
        case TY_WEAK:
            buf_puts(b, "weak"); buf_putc(b, '_');
            append_kind_mangle(b, t.as.rc.inner);
            break;
        case TY_REF_IMMUT:
            buf_puts(b, "ref_immut"); buf_putc(b, '_');
            append_kind_mangle(b, t.as.ref_borrow.target);
            break;
        case TY_REF_MUT:
            buf_puts(b, "ref_mut"); buf_putc(b, '_');
            append_kind_mangle(b, t.as.ref_borrow.target);
            break;
        case TY_ADT:
            append_c_ident_mangled(b, t.as.adt_.def && t.as.adt_.def->name
                                          ? t.as.adt_.def->name : "adt");
            break;
        case TY_APP: {
            /* structdef-retirement DS-D: no struct-headed app forms, so a mangled
             * application is always ADT-headed (e.g. (Maybe float)). */
            Type args[16];
            AdtDef *adef = NULL;
            uint8_t an_args = 0;
            if (type_extract_adt_app(&t, &adef, args, &an_args) && adef) {
                append_c_ident_mangled(b, adef->name);
                for (uint32_t i = 0; i < an_args; i++) {
                    buf_puts(b, "__");
                    append_type_mangle(b, args[i]);
                }
            } else {
                buf_puts(b, "app");
            }
            break;
        }
        /* docs/archive/history/concrete-codegen-layout-kind-enumerations-drift.md:
         * this switch used to end in `default: "opaque"`, which did not drop an
         * unlisted kind but MERGED it -- every kind without a case mangled to
         * the single token `opaque`, so two distinct instantiations claimed one
         * C name.  `type_register_adt_app` keys its registry on `type_eq`, so
         * both got registry entries, both emitted a typedef and a constructor
         * under that one name, and the `#ifndef TUR_TY_...` / `#ifndef TUR_FN_...`
         * guards silently dropped the second of each -- leaving the second type
         * using the first's layout.  Reached by ordinary code:
         * `(Box (fn [int] float))` alongside `(Box (fn [int] int))` compiled and
         * passed `(int64_t)`-cast closure handles into a surviving `double`
         * constructor, exact only below 2^53.
         *
         * There is no `default` arm now, and there must not be one again: `-Wall`
         * (`-Wswitch`) then makes a newly-added TypeKind a build failure here
         * rather than a silent name merge.  `type_c_name` is exhaustive for the
         * same reason.  Every kind below needs a token distinct from every other
         * kind's; a kind with no runtime representation still needs one, because
         * "cannot appear here" is not enforced and a merge is unrecoverable. */
        /* type_eq compares a TY_FN by arity, argument kinds and result kind --
         * mangle exactly that triple.  A bare "fn" token merged every function
         * type, which is the collision the report demonstrates: two `Box`
         * instantiations over `(fn [int] float)` and `(fn [int] int)`.
         *
         * macos-int-conversion-carrier-pointer-straddles: that triple is not the
         * WHOLE of type_eq.  It also refuses to equate a `cfnptr` with a `boxed`
         * fn (types.c:118-122) -- a fat closure must never flow into a raw C
         * callback sink -- so `(Option (c-fn [int] int))` and an
         * `(Option (fn [int] int))` holding a capturing closure are DISTINCT
         * types that both mangled to `fn1_int__int`.  Two registry entries, one
         * C name: the second `#ifndef TUR_TY_` / `TUR_FN_` block was
         * preprocessed away and the second type silently adopted the first's
         * ctor slot (`void *` vs `tur_fnptr_int64_t_int64_t_t`), with no
         * diagnostic from any compiler.  Benign only because every TY_FN
         * representation is a same-bits 8-byte word today; a variant that is not
         * would be a silent miscompile.  Give cfnptr its own token.
         *
         * ONLY cfnptr, deliberately.  `boxed` is not mangled: type_eq equates a
         * boxed fn with a bare one of the same signature (and, at its top, with
         * TY_PTR_VOID), so those share ONE registry entry -- mangling `boxed`
         * would only make which name that entry gets depend on registration
         * order, without splitting anything.  cfnptr-vs-boxed is the sole
         * type_eq-distinct pair, and this is exactly enough to separate it. */
        case TY_FN:
            buf_puts(b, "fn");
            if (t.as.fn.cfnptr) buf_putc(b, 'c');
            append_u32(b, t.as.fn.arity);
            for (uint32_t i = 0; i < t.as.fn.arity; i++) {
                buf_putc(b, '_');
                append_kind_mangle(b, t.as.fn.arg_kinds[i]);
            }
            buf_puts(b, "__");
            append_kind_mangle(b, t.as.fn.result_kind);
            break;
        /* type_eq treats every TY_SET as equal (no payload comparison), so one
         * token is injective here. */
        case TY_CONT:
            buf_puts(b, "cont"); buf_putc(b, '_');
            append_kind_mangle(b, t.as.cont.returns);
            break;
        case TY_CLONEABLE_CONT:
            buf_puts(b, "cont_cloneable"); buf_putc(b, '_');
            append_kind_mangle(b, t.as.cont.returns);
            break;
        case TY_EXCEPTION:
            buf_puts(b, "exception"); buf_putc(b, '_');
            append_kind_mangle(b, t.as.exn.payload_type);
            break;
        case TY_HANDLER:
            /* type_eq also consults handled_row, which has no short spelling
             * here; value/result kinds are the part that changes the C-visible
             * shape.  Two handlers differing ONLY in handled_row still merge --
             * narrower than before, and recorded rather than silent. */
            buf_puts(b, "handler"); buf_putc(b, '_');
            append_kind_mangle(b, t.as.handler_.value_kind);
            buf_putc(b, '_');
            append_kind_mangle(b, t.as.handler_.result_kind);
            break;
        /* Finding 2 of the same report: injectivity is required against
         * `type_eq`, and `type_register_adt_app` mints a monomorph name for ANY
         * type argument that is not a bare tyvar -- the concrete-layout list
         * does NOT gate it.  So a kind whose `type_eq` discriminates on a
         * payload needs that payload in its token even when
         * type_has_concrete_codegen_layout rejects it: `(Box (| int float))`
         * and `(Box (| int cstr))` are two registry entries that both emitted
         * `tur_adt_Box__union`, and the `#ifndef` guard dropped the second.
         * The kinds below whose `type_eq` compares nothing (it falls through to
         * `return 1` -- any two are the same type) keep a bare token, which is
         * injective by that definition. */
        /* type_eq compares union/intersection members structurally. */
        case TY_UNION:
            buf_puts(b, "union");
            append_u32(b, t.as.union_.n_members);
            for (uint8_t i = 0; i < t.as.union_.n_members; i++) {
                buf_putc(b, '_');
                if (t.as.union_.members && t.as.union_.members[i])
                    append_type_mangle(b, *t.as.union_.members[i]);
                else
                    buf_puts(b, "none");
            }
            break;
        case TY_INTERSECTION:
            buf_puts(b, "intersection");
            append_u32(b, t.as.intersection_.n_members);
            for (uint8_t i = 0; i < t.as.intersection_.n_members; i++) {
                buf_putc(b, '_');
                if (t.as.intersection_.members && t.as.intersection_.members[i])
                    append_type_mangle(b, *t.as.intersection_.members[i]);
                else
                    buf_puts(b, "none");
            }
            break;
        /* type_eq compares TY_REC by its interned binder name. */
        case TY_REC:
            buf_puts(b, "rec");
            if (t.as.rec.name) { buf_putc(b, '_'); append_c_ident_mangled(b, t.as.rec.name); }
            break;
        /* type_eq compares a contract by its BASE type (see the TY_CONTRACT arm
         * there), which is also what type_c_name lowers it to -- so the base is
         * exactly what distinguishes two contract monomorphs.  Predicates are
         * not compared and so are not mangled. */
        case TY_CONTRACT:
            buf_puts(b, "contract");
            if (t.as.contract_.base_type) {
                buf_putc(b, '_');
                append_type_mangle(b, *t.as.contract_.base_type);
            }
            break;
        /* type_eq compares forall/exists on n_vars, the constraint set and the
         * body (bound-variable names are not significant). */
        case TY_FORALL:
        case TY_EXISTS:
            buf_puts(b, t.kind == TY_FORALL ? "forall" : "exists");
            append_u32(b, t.as.forall_.n_vars);
            buf_putc(b, '_');
            append_u32(b, t.as.forall_.n_constraints);
            for (uint8_t i = 0; i < t.as.forall_.n_constraints; i++) {
                buf_putc(b, '_');
                TypeClass *c = t.as.forall_.constraint_classes
                                   ? t.as.forall_.constraint_classes[i] : NULL;
                append_c_ident_mangled(b, c && c->name ? c->name->name : "class");
                buf_putc(b, '_');
                append_u32(b, t.as.forall_.constraint_var_idx
                                  ? t.as.forall_.constraint_var_idx[i] : 0);
            }
            buf_puts(b, "__");
            if (t.as.forall_.body) append_type_mangle(b, *t.as.forall_.body);
            else                   buf_puts(b, "none");
            break;
        /* type_eq compares TY_TYPECLASS by the TypeClass pointer; the interned
         * class name stands in for it here (two distinct classes cannot share
         * a name in one program). */
        case TY_TYPECLASS:
            buf_puts(b, "typeclass_");
            append_c_ident_mangled(b, t.as.typeclass.typeclass &&
                                      t.as.typeclass.typeclass->name
                                          ? t.as.typeclass.typeclass->name->name
                                          : "class");
            break;
        /* type_eq compares TY_TYPECLASS_INST by the instance pointer; the class
         * name plus the instance's type arguments is what distinguishes two
         * instances of a class. */
        case TY_TYPECLASS_INST: {
            const TypeClassInstance *inst = t.as.typeclass_inst.instance;
            buf_puts(b, "typeclass_inst_");
            append_c_ident_mangled(b, inst && inst->typeclass && inst->typeclass->name
                                          ? inst->typeclass->name->name : "class");
            for (uint8_t i = 0; inst && i < inst->n_type_args; i++) {
                buf_putc(b, '_');
                append_type_mangle(b, inst->type_args[i]);
            }
            break;
        }
        /* type_eq compares a row order-sensitively on its elements, and on the
         * field names when the row is a typed-field row. */
        case TY_TYPEROW:
            buf_puts(b, "typerow");
            append_u32(b, t.as.typerow_.n_elements);
            for (uint8_t i = 0; i < t.as.typerow_.n_elements; i++) {
                buf_putc(b, '_');
                if (t.as.typerow_.field_names && t.as.typerow_.field_names[i]) {
                    append_c_ident_mangled(b, t.as.typerow_.field_names[i]);
                    buf_putc(b, '_');
                }
                if (t.as.typerow_.elements && t.as.typerow_.elements[i])
                    append_type_mangle(b, *t.as.typerow_.elements[i]);
                else
                    buf_puts(b, "none");
            }
            break;
    }
}

Type clone_struct_app_type(Type t) {
    if (t.kind != TY_APP) return t;
    Type out = t;
    /* A TY_APP can carry a null fn/arg -- a partially-resolved application,
     * e.g. a parametric instance head whose argument never got bound.  The
     * paired free_struct_app_type below has always null-checked both, so this
     * state is expected; cloning dereferenced them unguarded and segfaulted in
     * memmove instead of letting the caller report a real diagnostic.
     * See docs/archive/clone-struct-app-type-segv-on-null-arg.md. */
    out.as.app.fn = NULL;
    out.as.app.arg = NULL;
    if (t.as.app.fn) {
        out.as.app.fn = (Type *)malloc(sizeof(Type));
        if (!out.as.app.fn) { fprintf(stderr, "tur: oom\n"); abort(); }
        *out.as.app.fn = clone_struct_app_type(*t.as.app.fn);
    }
    if (t.as.app.arg) {
        out.as.app.arg = (Type *)malloc(sizeof(Type));
        if (!out.as.app.arg) { fprintf(stderr, "tur: oom\n"); abort(); }
        *out.as.app.arg = clone_struct_app_type(*t.as.app.arg);
    }
    return out;
}

void free_struct_app_type(Type t) {
    if (t.kind != TY_APP) return;
    if (t.as.app.fn) {
        free_struct_app_type(*t.as.app.fn);
        free(t.as.app.fn);
    }
    if (t.as.app.arg) {
        free_struct_app_type(*t.as.app.arg);
        free(t.as.app.arg);
    }
}

/* Propagate a parametric opaque's substructural discipline into a TY_APP node.
 * A `(defopaque Name [T] :int :linear)` applied as `(Name X)` should yield a
 * TY_APP whose copy_kind / substruct reflect the head's :linear (or :affine)
 * qualifier; otherwise the linear-discipline checker silently treats the
 * applied value as a plain copyable handle.  See
 * docs/archive/history/parametric-linear-opaque-not-enforced.md.
 * Walks the `fn` spine down to its head; if the head is a :linear / :affine
 * opaque/struct, lifts the discipline onto the TY_APP node in place. */
void propagate_app_discipline(Type *app, const Type *fn) {
    const Type *head = fn;
    while (head && head->kind == TY_APP) head = head->as.app.fn;
    if (head && head->kind == TY_ADT && head->as.adt_.def) {
        /* structdef-retirement slice 5: a parametric opaque is now a TY_ADT head,
         * so `(Name X)` for a `:linear`/`:affine` opaque lifts the discipline onto
         * the TY_APP node exactly as the struct path above did. */
        const AdtDef *hd = head->as.adt_.def;
        if (hd->is_linear) {
            app->copy_kind = CK_LINEAR;
        } else if (hd->is_affine) {
            app->copy_kind = CK_UNIQUE;
            app->substruct = SK_AFFINE;
        }
    }
}

/* Phase E: Returns true if a TypeKind maps to a concrete C scalar type. */
static bool fn_kind_is_primitive(TypeKind k) {
    switch (k) {
        case TY_NIL: case TY_BOOL: case TY_INT: case TY_FLOAT:
        case TY_CSTR: case TY_PTR_VOID:
        case TY_INT8: case TY_INT16: case TY_INT32: case TY_INT64:
        case TY_UINT8: case TY_UINT16: case TY_UINT32: case TY_UINT64:
        case TY_FLOAT32: case TY_FLOAT64:
            return true;
        default:
            return false;
    }
}

/* Phase E: Returns true when all arg and result kinds of a TY_FN type are
 * concrete primitives (no TY_TYVAR). */
static bool fn_type_is_concrete_for_ptr(const Type *t) {
    if (!t || t->kind != TY_FN) return false;
    if (!fn_kind_is_primitive(t->as.fn.result_kind)) return false;
    for (uint32_t i = 0; i < t->as.fn.arity; i++) {
        if (!fn_kind_is_primitive(t->as.fn.arg_kinds[i])) return false;
    }
    return true;
}

/* Phase E: Build a stable typedef name for a concrete fn type, e.g.
 * "tur_fnptr_int64_t_int32_t_t" for (fn [:int32] :int).
 *
 * c-fn-ptr-element-and-size-precision-gap fix: when the fn_type is a cfnptr
 * and an arg has a non-NULL arg_full_types[i], mangle in its precise C name
 * (e.g. `unsigned char *` -> `unsigned_char___` rather than the lossy
 * `void *` -> `void___`).  This is what lets two cfnptrs with different
 * ptr<T> element types get distinct typedefs. */
static char *fn_ptr_typedef_name(const Type *fn_type) {
    Buf name; buf_init(&name);
    buf_puts(&name, "tur_fnptr_");
    /* c-fn-ptr-element-and-size-precision-gap fix: mangle the precise result
     * type when a cfnptr preserved one (size_t / element-ptr / const-ptr), so
     * two cfnptrs that share a carrier result kind but differ on the precise
     * return type get distinct typedefs. */
    const char *ret_c = (fn_type->as.fn.cfnptr && fn_type->as.fn.result_full_type)
        ? type_c_name(*fn_type->as.fn.result_full_type)
        : type_c_name(type_from_kind(fn_type->as.fn.result_kind));
    for (const char *p = ret_c; *p; p++)
        buf_putc(&name, isalnum((unsigned char)*p) ? *p : '_');
    bool use_full = fn_type->as.fn.cfnptr && fn_type->as.fn.arg_full_types != NULL;
    for (uint32_t i = 0; i < fn_type->as.fn.arity; i++) {
        buf_putc(&name, '_');
        const char *arg_c;
        if (use_full && fn_type->as.fn.arg_full_types[i]) {
            arg_c = type_c_name(*fn_type->as.fn.arg_full_types[i]);
        } else {
            arg_c = type_c_name(type_from_kind(fn_type->as.fn.arg_kinds[i]));
        }
        for (const char *p = arg_c; *p; p++)
            buf_putc(&name, isalnum((unsigned char)*p) ? *p : '_');
    }
    buf_puts(&name, "_t");
    buf_putc(&name, '\0');
    char *result = tur_strdup(name.data);
    buf_free(&name);
    return result;
}

/* Phase E: Register a concrete fn type and return its typedef name.
 * Returns NULL when the fn type is not fully concrete. */
const char *register_fn_ptr_typedef(const Type *fn_type) {
    if (!fn_type_is_concrete_for_ptr(fn_type)) return NULL;

    char *name = fn_ptr_typedef_name(fn_type);

    for (uint32_t i = 0; i < g_n_fn_ptr_typedefs; i++) {
        if (strcmp(g_fn_ptr_typedefs[i].typedef_name, name) == 0) {
            free(name);
            return g_fn_ptr_typedefs[i].typedef_name;
        }
    }

    if (g_n_fn_ptr_typedefs >= g_cap_fn_ptr_typedefs) {
        uint32_t new_cap = g_cap_fn_ptr_typedefs ? g_cap_fn_ptr_typedefs * 2 : 8;
        RegisteredFnPtrTypedef *arr = (RegisteredFnPtrTypedef *)realloc(
            g_fn_ptr_typedefs, new_cap * sizeof(RegisteredFnPtrTypedef));
        if (!arr) { fprintf(stderr, "tur: oom\n"); abort(); }
        g_fn_ptr_typedefs = arr;
        g_cap_fn_ptr_typedefs = new_cap;
    }
    RegisteredFnPtrTypedef *entry = &g_fn_ptr_typedefs[g_n_fn_ptr_typedefs++];
    entry->typedef_name = name;
    entry->emitted = false;
    entry->result_kind = fn_type->as.fn.result_kind;
    entry->arity = fn_type->as.fn.arity;
    for (uint32_t i = 0; i < fn_type->as.fn.arity; i++)
        entry->arg_kinds[i] = fn_type->as.fn.arg_kinds[i];
    /* c-fn-ptr-element-and-size-precision-gap fix: snapshot per-arg full
     * types for cfnptrs so the later emit pass can use precise element-typed
     * pointer names.  The Type pointers live in the elaboration arena, which
     * outlives codegen; storing the pointer is safe. */
    for (uint8_t i = 0; i < MAX_FN_ARITY; i++) entry->arg_full_types[i] = NULL;
    if (fn_type->as.fn.cfnptr && fn_type->as.fn.arg_full_types) {
        for (uint32_t i = 0; i < fn_type->as.fn.arity; i++)
            entry->arg_full_types[i] = fn_type->as.fn.arg_full_types[i];
    }
    /* c-fn-ptr-element-and-size-precision-gap fix: snapshot the precise result
     * type (size_t / element-ptr / const-ptr) for cfnptrs, NULL otherwise. */
    entry->result_full_type = (fn_type->as.fn.cfnptr)
        ? fn_type->as.fn.result_full_type : NULL;

    return entry->typedef_name;
}

/* TS4P1: Helpers for ADT-app monomorphisation. */

static bool adt_type_param_index(const AdtDef *def, const char *name, uint8_t *out_idx) {
    if (!def || !name) return false;
    for (uint8_t i = 0; i < def->n_type_params; i++) {
        if (def->type_params[i] && strcmp(def->type_params[i], name) == 0) {
            if (out_idx) *out_idx = i;
            return true;
        }
    }
    return false;
}

Type substitute_adt_app_type(const Type *t, const AdtDef *def, const Type *args) {
    if (!t) return type_from_kind(TY_UNKNOWN);
    switch (t->kind) {
        case TY_TYVAR: {
            uint8_t idx = 0;
            if (t->as.tyvar_.name &&
                adt_type_param_index(def, t->as.tyvar_.name, &idx)) {
                return args[idx];
            }
            return *t;
        }
        case TY_APP: {
            Type fn  = substitute_adt_app_type(t->as.app.fn,  def, args);
            Type arg = substitute_adt_app_type(t->as.app.arg, def, args);
            Type out = {0};
            out.kind = TY_APP;
            out.copy_kind = CK_COPY;
            out.hkt_kind = KIND_STAR;
            propagate_app_discipline(&out, &fn);
            out.as.app.fn  = (Type *)malloc(sizeof(Type));
            out.as.app.arg = (Type *)malloc(sizeof(Type));
            if (!out.as.app.fn || !out.as.app.arg) { fprintf(stderr, "tur: oom\n"); abort(); }
            *out.as.app.fn  = fn;
            *out.as.app.arg = arg;
            return out;
        }
        default:
            return *t;
    }
}

/* Owned analogue of substitute_adt_app_type: the substituted tyvar arg is
 * DEEP-CLONED (clone_struct_app_type) so the returned tree aliases nothing in
 * the caller's `args[]` spine and can be released with free_struct_app_type.
 * Mirrors substitute_struct_app_type's ownership discipline.  Use this (not the
 * aliasing variant above) at any call site that frees the result -- the plain
 * substitute_adt_app_type returns `args[idx]` by reference for a bare tyvar,
 * and a free() over that (or over a spine whose leaves alias it) is a bad-free
 * of arena-backed nodes. */
Type substitute_adt_app_type_owned(const Type *t, const AdtDef *def,
                                   const Type *args) {
    if (!t) return type_from_kind(TY_UNKNOWN);
    switch (t->kind) {
        case TY_TYVAR: {
            uint8_t idx = 0;
            if (t->as.tyvar_.name &&
                adt_type_param_index(def, t->as.tyvar_.name, &idx)) {
                return clone_struct_app_type(args[idx]);
            }
            return *t;
        }
        case TY_APP: {
            Type fn  = substitute_adt_app_type_owned(t->as.app.fn,  def, args);
            Type arg = substitute_adt_app_type_owned(t->as.app.arg, def, args);
            Type out = {0};
            out.kind = TY_APP;
            out.copy_kind = CK_COPY;
            out.hkt_kind = KIND_STAR;
            propagate_app_discipline(&out, &fn);
            out.as.app.fn  = (Type *)malloc(sizeof(Type));
            out.as.app.arg = (Type *)malloc(sizeof(Type));
            if (!out.as.app.fn || !out.as.app.arg) { fprintf(stderr, "tur: oom\n"); abort(); }
            *out.as.app.fn  = fn;
            *out.as.app.arg = arg;
            return out;
        }
        default:
            return *t;
    }
}

/* Return the C type string for a CtorField with concrete type args substituted. */
/* CONV-S1 seam 4 / structdef-retirement slice 1: true when a `Result`/`Option`
 * monomorph field whose resolved type is `resolved` is stored as a heap pointer
 * `T *` (box-as-pointer) rather than inline / int64-boxed.  Applies to a
 * non-parametric value-STRUCT field (the original carrier-box layout match) and,
 * since the defstruct-as-defadt lowering, to a non-parametric by-value record-ADT
 * field (a lowered defstruct -- `User` -> `tur_adt_User`).  Centralised so the
 * typedef, ctor param/store, and field-read sites agree -- and so it takes
 * precedence over the B4 wide-by-value-element int64 box (`type_is_wide_byval_adt`)
 * for these fields, which would otherwise pick a conflicting int64 slot for a wide
 * (>8-byte) record-ADT payload like `User`. */
/* SR3 slice A (null-None).  Keyed by name like adt_field_is_ros_pointer_box
 * above, with the shape pinned down (2 ctors, nullary tag 0 named None) so a
 * user ADT that happens to be called Option but is shaped differently never
 * takes the niche.  A same-shaped user Option does -- and gets the same
 * semantics, since the readers treat NULL as tag 0 uniformly. */
bool adt_ctor_is_null_none(const AdtDef *def, const CtorDef *ctor) {
    return def && ctor && def->name && ctor->name &&
           strcmp(def->name, "Option") == 0 && def->n_ctors == 2 &&
           ctor->tag == 0 && ctor->n_fields == 0 &&
           strcmp(ctor->name, "None") == 0;
}

/* SR3 slice B (the Option niche -- default since 2026-09-03, TUR_OPTION_NICHE=0
 * restores the tagged form; docs/archive/sr3-option-niche-plan.md):
 * represents an `(Option P)` monomorph whose payload is a NON-NULLABLE pointer
 * as the bare payload pointer -- 16 bytes down to 8, with `(none)` as the null
 * pointer.  Slice A already made the CARRIER None the null pointer, so the two
 * representations already agree about None; what this adds is Some carried AS
 * its payload, with no tag word anywhere.
 *
 * A real experiment rather than an env seam (the CLAUDE.md rule for an
 * in-flight feature): it carries a hand-maintained soundness allowlist, so a
 * user who turns it on should get the TUR-W0060 lifecycle warning and a plan to
 * read.  `experiment_warn_if_used` is once-per-compile guarded, so calling it
 * from this hot predicate costs one index lookup after the first. */
/* RM3 regions (docs/archive/regions-plan.md): declared lifetimes over the
 * arena, for values whose owner is a SCOPE rather than another value -- the
 * per-node spine box of a persistent recursive structure, which RM1 cannot
 * reach and RM2 cannot own.
 *
 * GRADUATED 2026-09-05 (RM3 R5): on by default.  It was a real experiment
 * rather than an env seam while in flight, per the CLAUDE.md rule -- it is
 * user-visible surface and the reclamation phase most able to produce a silent
 * wrong answer -- and R5 held it at prototype on one blocker, the boundary
 * form.  With `with-region` landed, the static walk widened across four pinned
 * batches, and the three R4 residues closed, the default flipped.  `TUR_REGIONS=0`
 * is the bisection hatch (read once in main.c): it restores plain malloc at
 * the four routed ctor sites, no bracket around bt-scope / with-region, and no
 * atexit shutdown -- byte-for-byte the pre-graduation default build.
 * tests/run-regions-seam.sh keeps THAT path green (inverted at graduation, per
 * the flags guide), so the hatch cannot rot the way an unused flag does. */
bool regions_enabled(void) {
    return g_opt_regions;
}

/* RM3 R4: the free-side twin of the ctor routing above.
 *
 * From R2 onward a `:heap` ADT node allocated inside a region is ARENA memory,
 * while the same emitted drop glue still ends in `free(ptr)`.  That is not a
 * leak if it goes wrong -- it is an allocator mismatch, and glibc aborts.  So
 * every node free path spells this instead of `free`, and it resolves to plain
 * `free` in a default build, exactly as `region_alloc_fn` does on the other
 * side.  Keep the two in step: a new node allocation site needs a matching
 * free site, and vice versa. */
const char *region_free_fn(void) {
    return regions_enabled() ? "tur_region_free" : "free";
}

static bool sr3_option_niche(void) {
    /* The niche is layered ON TOP of SR2: narrowing `(Option P)` to its payload
     * pointer only means anything if that Option is a by-value parametric sum in
     * the first place.  With SR2 off, the same type rides the int64 carrier as a
     * pointer to a tagged box -- and a niche value is then byte-identical to a
     * carrier box, which is precisely the confusion the payload-must-be-a-pointer
     * test above exists to prevent, arriving through the other door.
     *
     * So the niche follows its substrate down.  Without this,
     * TUR_SR2_APP_SUM_BYVALUE=0 is not the one-variable switch a bisection hatch
     * has to be: it ABORTED the compiler on four fixtures and silently
     * wrong-answered httpd-req-string-opt, all of which recover when the niche
     * comes off too, and none of which points at the niche from the switch the
     * user actually flipped.  Measured and filed 2026-09-04 in
     * docs/reported/sr2-carrier-seam-rotted.md; tests/run-sr2-seam.sh pins it.
     *
     * This is a HATCH interaction, not a default one: both globals are true in
     * every shipping build, so the added test is dead weight on the default path
     * and changes no emitted byte there. */
    if (!g_sr2_app_sum_byvalue) return false;
    /* Graduated 2026-09-03: default-on, no lifecycle warning.  The global is
     * only ever cleared by TUR_OPTION_NICHE=0 (main.c), the bisection hatch. */
    return g_opt_option_niche;
}

/* opaque-pointer-c-spelling (GRADUATED 2026-08-28, docs/archive/opaque-pointer-
 * c-spelling-gate-results.md): an opaque newtype declared over a POINTER
 * (`(defopaque String :ptr<void>)`) c-names as `void *`, not as the `int64_t`
 * carrier word.
 *
 * The motivation is not size -- both are 8 bytes -- it is DISTINGUISHABILITY.
 * The emitter asks "is this the int64 carrier?" by string-comparing the C name
 * (94 `strcmp(cname, "int64_t")` sites), so an opaque pointer handle used to
 * alias every other carrier value: a tagged ADT box, a closure handle, a boxed
 * Option.  A pointer spelling is what lets those sites tell a handle apart from
 * a box -- and it is the precondition SR3 slice B needs for `(Option String)`
 * niche filling, which is the whole of that phase's census.
 *
 * The declared base type reaches here through `AdtDef.opaque_base_is_ptr`; it
 * was parsed for POSITION only and discarded before this landed, which is why
 * `:ptr<void>` and `:int` were indistinguishable downstream. */
bool adt_opaque_c_names_as_pointer(const AdtDef *def) {
    return def && def->is_opaque && def->opaque_base_is_ptr;
}

/* The soundness condition for the niche is "P's valid values EXCLUDE 0",
 * because that is the bit pattern None claims.  Nothing in the type system
 * PROVES non-nullness, so eligibility rests on two claims of different
 * strengths -- and the polarity matters throughout: an unrecognised payload
 * merely misses the optimisation, where a denylist that missed an entry would
 * make `(some x)` and `(none)` the same value.
 *
 *   1. DECLARED (opaque newtypes): `(defopaque String :ptr<void> :non-null)`.
 *      The type's author claims it at the definition site, next to the
 *      constructors the claim is about.  The compiler cannot verify it --
 *      inline-C or a coercing `::` can still smuggle a 0 -- so the niche
 *      `Some` ctor enforces it at construction (emit_registered_adt_app_rec):
 *      a null payload aborts with a message naming the type, instead of
 *      silently reading back as `(none)`.
 *   2. LISTED (compiler-lowered `:heap` collections): Vec / Map / Set /
 *      MutableMap, whose constructors the compiler itself emits as
 *      unconditional mallocs; an empty collection is a valid non-null header.
 *      These stay a name list because they are defstruct-lowered heap ADTs,
 *      not opaques -- there is no author-facing attribute slot for them yet --
 *      and because the compiler emitting the ctor is a stronger warrant than
 *      any annotation.
 *
 * `:heap`-ness itself is NOT the condition, which is the finding that shrank
 * this phase.  `Cons` is a `:heap` record ADT whose EMPTY LIST is the null
 * handle ("At runtime, nil is 0" -- stdlib/list.tur, and `(defn tnil [] : int
 * 0)`), so `(Option (Cons T))` would read `(some (tnil))` as `(none)`.  A
 * `:ptr<T>`, a `cstr`, or a bare closure handle is never eligible: null is a
 * value each of them can legitimately hold, and none of them has an author who
 * can claim otherwise. */
static bool sr3_payload_is_nonnull_pointer(Type p) {
    const AdtDef *pdef = NULL;
    if (p.kind == TY_ADT && p.as.adt_.def) pdef = p.as.adt_.def;
    else {
        AdtDef *d = NULL;
        Type pargs[16];
        uint8_t pn = 0;
        Type pt = p;
        if (type_extract_adt_app(&pt, &d, pargs, &pn)) pdef = d;
    }
    if (!pdef || !pdef->name) return false;
    bool allowed = false;
    if (pdef->is_opaque) {
        /* Claim 1: the declaration.  opaque_base_is_ptr is implied (the
         * attribute is rejected on any other base) but asked anyway so a stale
         * def can never widen eligibility. */
        allowed = pdef->opaque_non_null && pdef->opaque_base_is_ptr;
    } else {
        static const char *const HEAP_ALLOW[] = {
            "Vec", "Map", "Set", "MutableMap", NULL
        };
        for (uint32_t i = 0; HEAP_ALLOW[i] && !allowed; i++)
            if (strcmp(pdef->name, HEAP_ALLOW[i]) == 0) allowed = true;
    }
    if (!allowed) return false;
    /* And the payload's C spelling must be a real POINTER, not the int64
     * carrier word.  This is about telling representations apart rather than
     * about null: an `int64_t`-spelled niche `(Option P)` would be
     * byte-identical to the CARRIER form of the same type, which is a pointer
     * to a tagged box, and every `strcmp(cname, "int64_t")` test in the
     * emitter (there are dozens) would read a niche value as a box and
     * dereference the payload's first word as a tag.  This was the condition
     * that ruled `String` out before `defopaque` over a pointer got its
     * pointer C spelling; it is now a tautology for the opaque arm above (a
     * `:non-null` opaque spells `void *`) and load-bearing only as a backstop. */
    const char *pc = type_c_name(p);
    return pc && strchr(pc, '*') != NULL;
}

/* True when `t` is an `(Option P)` monomorph eligible for the SR3 slice B
 * niche.  Shape-pinned exactly as adt_ctor_is_null_none is (a user ADT that
 * happens to be called Option but is shaped differently never takes it), plus
 * the payload allowlist above. */
bool adt_app_is_niche_option(Type t) {
    if (!sr3_option_niche()) return false;
    AdtDef *def = NULL;
    Type args[16];
    uint8_t n_args = 0;
    if (!type_extract_adt_app(&t, &def, args, &n_args) || !def) return false;
    if (n_args != 1 || def->n_ctors != 2 || !def->ctors) return false;
    if (!def->ctors[0] || !def->ctors[1]) return false;
    if (!adt_ctor_is_null_none(def, def->ctors[0])) return false;
    if (def->ctors[1]->n_fields != 1 || !def->ctors[1]->name ||
        strcmp(def->ctors[1]->name, "Some") != 0) return false;
    if (!adt_app_type_arg_is_concrete(&args[0])) return false;
    return sr3_payload_is_nonnull_pointer(args[0]);
}

bool adt_field_is_ros_pointer_box(const AdtDef *owner, const Type *resolved) {
    if (!owner || !owner->name || !resolved) return false;
    if (strcmp(owner->name, "Result") != 0 && strcmp(owner->name, "Option") != 0)
        return false;
    if (resolved->kind == TY_ADT && resolved->as.adt_.def &&
        !resolved->as.adt_.def->is_heap &&
        resolved->as.adt_.def->n_type_params == 0 &&
        adt_is_byvalue_product(resolved->as.adt_.def))
        return true;
    return false;
}

static const char *adt_field_c_type(const AdtDef *owner, const CtorField *field,
                                     const Type *args) {
    if (field->full_type && owner && args) {
        /* nested-carrier-match: use the OWNED substitution (deep-cloned spine)
         * and release it before returning -- type_c_name interns the app name, so
         * the returned string outlives the freed Type.  The plain aliasing variant
         * leaked the malloc'd TY_APP spine here for every concrete app field. */
        Type resolved = substitute_adt_app_type_owned(field->full_type, owner, args);
        /* CONV-S1 seam 4 (graduation): a lowered carrier-helper-backed parametric
         * stdlib type (`Result`/`Option`) whose monomorph field resolves to a
         * non-parametric value-struct stores it as a HEAP POINTER `T *`, exactly
         * as the struct path does (struct_field_c_type, "Direction (1)").  Two
         * wins: (1) the monomorph + its ctor reference `T` only by pointer, so a
         * guarded forward `typedef struct T T;` (emitted by the dependency
         * pre-pass) suffices and the embedding monomorph no longer needs `T`'s
         * full typedef flushed ahead of it (the struct-with-Option-field typedef
         * ordering blocker); (2) the 8-byte slot matches the carrier-box layout
         * the prelude helpers (`tur_box_ok`/`tur_box_some`) produce.  Inert at
         * default: there `Result`/`Option` are structs, so this ADT path is never
         * reached for them.  The ctor heap-boxes the by-value param into the slot
         * (see the byval ctor branch's struct-pointer box). */
        if (adt_field_is_ros_pointer_box(owner, &resolved)) {
            /* INTERNED, so the returned pointer is stable for the whole
             * compilation -- the same contract type_c_name already honours, and
             * the reason the distinction between the two was invisible at the
             * call site.
             *
             * This used to be one function-scoped `static char ptrbuf[128]`.
             * Callers collect several of these before printing any of them --
             * the monomorph ctor emitter fills `val_ctype[]` for every field and
             * only then writes the parameter list -- so a single buffer handed
             * every field the LAST field's spelling.  That emitted
             * `ctor_Result__Rational__ArithError(bool, tur_adt_ArithError *,
             * tur_adt_ArithError *)`, silently mistyping ok_val as the error
             * arm: well-formed C with the wrong type in it, no crash, no ASan
             * report, no diagnostic.  A 16-slot rotating pool fixed the two-field
             * case in 2026-08-26; interning removes the bound entirely.
             *
             * Latent until two fields of one constructor could both take this
             * path: it needs a Result/Option monomorph whose OK and ERR arms are
             * both non-parametric by-value ADTs, which is what a by-value sum
             * makes ordinary (`(Result Rational ArithError)`).  See
             * docs/archive/c-name-accessors-share-static-buffers.md. */
            Buf pb; buf_init(&pb);
            buf_printf(&pb, "%s *", type_c_name(resolved));
            buf_putc(&pb, '\0');
            const char *boxed = intern_type_name(pb.data);
            buf_free(&pb);
            free_struct_app_type(resolved);
            return boxed;
        }
        const char *nm = type_c_name(resolved);
        free_struct_app_type(resolved);
        /* result-nil-ok-payload-emits-void-field: `nil` c-names to `void`, and
         * `void` is not something a union member, a ctor parameter, or a match
         * binder can be.  A `(Result nil E)` / `(Option nil)` monomorph
         * therefore emitted `struct { void _0; } Ok;`,
         * `ctor_Result_Ok__nil__int(void _0)`, and
         * `void _un_N = (void)__scrut->as.Ok._0;` -- three cc errors naming
         * only generated identifiers, for a signature (`: (Result nil int)`)
         * the type checker accepts without comment.
         *
         * `nil` is the right type for "it worked and carries nothing", which
         * is the return shape of every setter, connector, and binder in a
         * C-wrapping spice, so this has to WORK rather than be diagnosed away.
         * The slot becomes the int64 the erased twin already reads there: a
         * zero-information payload needs no storage, but it does need a
         * well-formed member, and every other arm of the union is int64-shaped
         * anyway.  Nothing ever reads the value -- `nil` has no values to read
         * -- so the eight bytes are dead by construction, like the other dead
         * union arms the by-value ctor prologue already leaves indeterminate.
         *
         * Same remedy as the sub-word widening just below, and for the same
         * reason: the monomorph's member and the generic layout every erased
         * reader goes through have to agree on the slot. */
        if (nm && strcmp(nm, "void") == 0) return "int64_t";
        /* SR2b layout contract: a MULTI-VARIANT parametric monomorph's union
         * payload lives at offset 8 in a 16-byte tagged aggregate -- that is
         * what the generic base layout (`tur_adt_Option`'s int64 slot), the
         * preamble helpers (tur_opt_value & co.), and every erased reader
         * assume.  A sub-word scalar member (`bool _0;`) gives the union
         * 1-byte alignment: the payload lands at offset 4 in an 8-byte
         * aggregate, and a generic-layout read of offset 8 returns garbage --
         * `(unwrap-or (some true) false)` was false whenever the producer was
         * the `Option__bool` monomorph ctor and the consumer an erased spec
         * (type-fuzz BUG_wrong_output-000010).  Widen sub-word INTEGER
         * scalars to the int64 slot; the store zero/sign-extends and the
         * typed binder reads cast back, so both the monomorph-typed and the
         * generic-layout views agree.  Single-variant records keep exact
         * member types (they have no generic-union twin), and float/double
         * keep theirs (an implicit float->int64 store would VALUE-convert;
         * double is already 8-aligned). */
        /* erased-generic-field-read-overruns-subword-monomorph-box: the
         * single-variant record DOES have a generic twin after all.  The base
         * typedef of a parametric record (`tur_adt_Identity { int64_t
         * wrapped; }`) is what every erased generic body reads through --
         * `(defn run-id [A] [i : (Identity A)] : A (.wrapped i))` -- and a
         * `(Identity bool)` monomorph boxed at an aggregate spill with a 1-byte
         * `bool wrapped;` is then read 8 bytes wide (ASan heap-buffer-overflow;
         * the right answer only by little-endian luck).  So the widening applies
         * to a record too, but ONLY for a field whose DECLARED type is a type
         * parameter: that is the field the twin spells as int64_t.  A record
         * field declared concretely (`:bool`) is `bool` in both layouts and
         * keeps its width. */
        bool declared_tyvar = field->full_type &&
                              field->full_type->kind == TY_TYVAR;
        if (owner && (owner->n_ctors > 1 || declared_tyvar) && nm &&
            (strcmp(nm, "bool") == 0 ||
             strcmp(nm, "int8_t") == 0 || strcmp(nm, "uint8_t") == 0 ||
             strcmp(nm, "int16_t") == 0 || strcmp(nm, "uint16_t") == 0 ||
             strcmp(nm, "int32_t") == 0 || strcmp(nm, "uint32_t") == 0))
            return "int64_t";
        return nm;
    }
    switch (field->kind) {
        case TY_INT:      return "int64_t";
        case TY_BOOL:     return "bool";
        case TY_FLOAT:    return "double";
        case TY_CSTR:     return "const char *";
        /* saffron-lang-plan S5: an `:any` field is a TWO-word box, and the
         * `default: int64_t` below is not a narrower spelling of that -- it is
         * the tag thrown away.  A `(Cons [hd : any tl : any])` laid out as two
         * int64 slots stores the payload word and loses which type it was, so
         * `type-of` on a field read answers whatever the carrier happens to
         * collide with.  The interpreter never had this problem: a TuriValue
         * carries its own tag wherever it is stored.
         *
         * Only reachable for a field whose declared kind is TY_ANY and whose
         * `full_type` is NULL; a field carrying a full type already resolves to
         * `tur_tagged_t` through type_c_name above. */
        case TY_ANY:      return "tur_tagged_t";
        case TY_PTR_VOID: return "void *";
        case TY_RC:
        case TY_WEAK:     return "RcControlBlock *";
        case TY_REF:
        case TY_LREF:     return "void *";
        case TY_INT8:     return "int8_t";
        case TY_INT16:    return "int16_t";
        case TY_INT32:    return "int32_t";
        case TY_INT64:    return "int64_t";
        case TY_UINT8:    return "uint8_t";
        case TY_UINT16:   return "uint16_t";
        case TY_UINT32:   return "uint32_t";
        case TY_UINT64:   return "uint64_t";
        case TY_FLOAT32:  return "float";
        case TY_FLOAT64:  return "double";
        default:          return "int64_t";
    }
}

/* Resolve an ADT ctor field's declared type against a concrete ADT-app receiver
 * type (substitutes the app's type args into the field's declared type).  See
 * types.h.  TY_UNKNOWN when `recv` is not a resolvable ADT app. */
Type adt_field_type_for_app(const Type *recv, const CtorField *field) {
    Type unknown = type_simple(TY_UNKNOWN, CK_COPY);
    if (!recv || !field || !field->full_type) return unknown;
    AdtDef *def = NULL;
    Type args[16];
    uint8_t n_args = 0;
    if (!type_extract_adt_app(recv, &def, args, &n_args) || !def) return unknown;
    return substitute_adt_app_type(field->full_type, def, args);
}

/* Append the mangled type-arg suffix for an ADT app (e.g. "__float" for (Maybe float)). */
static void append_adt_app_type_suffix(Buf *b, const AdtDef *def,
                                        const Type *args, uint8_t n_args) {
    (void)def;
    for (uint32_t i = 0; i < n_args; i++) {
        buf_puts(b, "__");
        append_type_mangle(b, args[i]);
    }
}

/* TS4P1: Register a concrete ADT-app type and return its C typedef name
 * (e.g. "tur_adt_Maybe__float"), or NULL if the type is not a concrete ADT app. */
/* S1 (jit-engine-plan): record every `ctor_X__T` monomorph's return C type in
 * the signature side table, at REGISTRATION time.  The two non-parametric ctor
 * emission sites in emit_module.c already record, but the monomorph clones are
 * rendered by emit_registered_adt_app_rec at final program assembly -- AFTER
 * every function body -- so recording there comes too late for any call-site
 * lookup and left 2,289 ctor calls corpus-wide on `__auto_type` (findings 16).
 * Registration happens the moment a body first names the type, which is
 * always at-or-before the first ctor call.  Recording is idempotent
 * (re-recording strdups the same string), so running on every registration
 * call -- including ones that find an existing entry -- is safe, and it makes
 * the record independent of WHICH caller registered first.
 *
 * The ret-type strings mirror emit_registered_adt_app_rec's `ctor_ret`
 * exactly: `<inst> *` for :heap, the aggregate for a by-value product, else
 * the int64 carrier.  Keeping the two in lockstep is the same seam discipline
 * as CONV-S1's member-path routing. */
/* Declared in emit_internal.h, which this file does not include (same
 * convention as emit_cps_ir.c's forward decls); adt_app_is_byvalue_product is
 * already public in types.h. */
void emit_sig_record_ret_ctype(const char *cname, uint32_t n_params,
                               const char *ctype);
void emit_sig_record_param_ctype(const char *cname, uint32_t idx,
                                 uint32_t n_params, const char *ctype);
static void record_adt_app_ctor_sigs(AdtDef *def, Type *args, uint8_t n_args,
                                     Type t) {
    Buf name; buf_init(&name);
    buf_puts(&name, "tur_adt_");
    append_c_ident_mangled(&name, def->name);
    append_adt_app_type_suffix(&name, def, args, n_args);
    buf_putc(&name, '\0');
    Buf suffix; buf_init(&suffix);
    append_adt_app_type_suffix(&suffix, def, args, n_args);
    buf_putc(&suffix, '\0');
    char ret_heap[512];
    snprintf(ret_heap, sizeof ret_heap, "%s *", name.data);
    bool app_heap  = def->is_heap;
    bool app_byval = adt_app_is_byvalue_product(t);
    /* SR3 slice B: a niche ctor returns the PAYLOAD's C type -- this table is
     * what the call-site temp is declared from (`emit_sig_lookup_ret_ctype`),
     * so recording the tagged monomorph name here spells a struct that has no
     * definition.  Must stay in lockstep with emit_registered_adt_app_rec's
     * `ctor_ret`, exactly as this function's header comment says. */
    bool app_niche = adt_app_is_niche_option(t);
    const char *ctor_ret = app_niche ? type_c_name(t)
                         : app_heap ? ret_heap
                         : app_byval ? name.data : "int64_t";
    for (uint32_t ci = 0; ci < def->n_ctors; ci++) {
        CtorDef *ctor = def->ctors[ci];
        /* duplicate-ctor-names-collide-in-emitted-c: this table is keyed by the
         * emitted FUNCTION symbol, so it has to spell it the same way
         * emit_registered_adt_app_rec does -- through the shared builder, which
         * qualifies the constructor with its owning ADT.  A key that disagrees
         * silently misses, and the call site then falls back to re-deriving a
         * ctype the prototype does not carry. */
        char *csym = mangle_ctor_symbol(def, ctor->name);
        char ctor_sym[512];
        int off = snprintf(ctor_sym, sizeof ctor_sym, "ctor_%s", csym);
        free(csym);
        if (off < 0) off = 0;
        if (off > (int)sizeof ctor_sym - 1) off = (int)sizeof ctor_sym - 1;
        ctor_sym[off] = '\0';
        if (off + suffix.len < sizeof ctor_sym)
            memcpy(ctor_sym + off, suffix.data, suffix.len);   /* includes NUL */
        emit_sig_record_ret_ctype(ctor_sym, ctor->n_fields, ctor_ret);
        /* macos-int-conversion-carrier-pointer-straddles (case A): also
         * record each ctor PARAM's C type, mirroring `val_ctype[fi]` in
         * emit_registered_adt_app_rec below.  The call site cannot re-derive
         * it: type_c_name(TY_FN) branches on the `boxed` flag, which carries
         * no type identity, so two monomorphs of the same fn-typed field
         * genuinely differ (`ctor_Option__fn1_int__int(bool, void *)` vs
         * `ctor_Option__fn1_float__float(bool, int64_t)`) while any
         * type-based re-derivation collapses them.  Recording the string the
         * prototype actually carries is the same "stop re-deriving" discipline
         * the return-type recording above already follows. */
        for (uint32_t fi = 0; fi < ctor->n_fields; fi++)
            emit_sig_record_param_ctype(ctor_sym, fi, ctor->n_fields,
                                        adt_field_c_type(def, &ctor->fields[fi],
                                                         args));
    }
    buf_free(&suffix);
    buf_free(&name);
}

const char *type_register_adt_app(Type t) {
    AdtDef *def = NULL;
    Type args[16];
    uint8_t n_args = 0;
    if (!type_extract_adt_app(&t, &def, args, &n_args) || !def) return NULL;
    if (def->is_gadt) return NULL;
    /* Require all type args to have concrete codegen layout. */
    for (uint32_t i = 0; i < n_args; i++) {
        if (args[i].kind == TY_TYVAR || args[i].kind == TY_UNKNOWN) return NULL;
    }
    /* Look for an existing registration. */
    for (uint32_t i = 0; i < g_n_adt_apps; i++) {
        if (type_eq(g_adt_apps[i].type, t)) {
            /* macos-int-conversion-carrier-pointer-straddles (case A):
             * record off the CANONICAL entry's type, never the incoming one.
             * type_eq (and append_type_mangle) ignore TY_FN's `boxed`/`cfnptr`
             * flags, so an equal-but-differently-boxed `t` lands on this entry
             * -- and recording from `t` would file a ctor signature the
             * emission, which renders g_adt_apps[i].type, never produces
             * (`int64_t` recorded against a `void *` slot).  The call-site
             * bridge trusts these strings, so a stale one is worse than none. */
            if (g_adt_apps[i].recording) return g_adt_apps[i].name;
            Type ct = g_adt_apps[i].type;
            AdtDef *cdef = NULL; Type cargs[16]; uint8_t cn_args = 0;
            if (type_extract_adt_app(&ct, &cdef, cargs, &cn_args) && cdef) {
                g_adt_apps[i].recording = true;
                record_adt_app_ctor_sigs(cdef, cargs, cn_args, ct);
                g_adt_apps[i].recording = false;
            }
            return g_adt_apps[i].name;
        }
    }
    /* Grow the registry if needed. */
    if (g_n_adt_apps >= g_cap_adt_apps) {
        uint32_t new_cap = g_cap_adt_apps ? g_cap_adt_apps * 2 : 8;
        RegisteredAdtApp *new_items = (RegisteredAdtApp *)realloc(
            g_adt_apps, new_cap * sizeof(RegisteredAdtApp));
        if (!new_items) { fprintf(stderr, "tur: oom\n"); abort(); }
        g_adt_apps = new_items;
        g_cap_adt_apps = new_cap;
    }
    /* Build the C typedef name: tur_adt_<Name>__<arg1>__... */
    Buf name; buf_init(&name);
    buf_puts(&name, "tur_adt_");
    append_c_ident_mangled(&name, def->name);
    append_adt_app_type_suffix(&name, def, args, n_args);
    buf_putc(&name, '\0');
    g_adt_apps[g_n_adt_apps].type     = clone_struct_app_type(t);
    g_adt_apps[g_n_adt_apps].name     = tur_strdup(name.data);
    g_adt_apps[g_n_adt_apps].emitted  = false;
    g_adt_apps[g_n_adt_apps].emitting = false;
    g_adt_apps[g_n_adt_apps].recording = false;
    buf_free(&name);
    if (!g_adt_apps[g_n_adt_apps].name) { fprintf(stderr, "tur: oom\n"); abort(); }
    /* Register FIRST, record the ctor signatures SECOND: recording c-names
     * every field, and a `:heap` self-typed field resolves to this very app.
     * With the entry already in the table (and flagged `recording`) that
     * re-entrant lookup returns the name instead of recursing.  The registry
     * may be realloc'd by a nested registration of some OTHER app the field
     * types name, so re-index through `idx` rather than a held pointer. */
    uint32_t idx = g_n_adt_apps++;
    g_adt_apps[idx].recording = true;
    {
        Type ct = g_adt_apps[idx].type;
        AdtDef *cdef = NULL; Type cargs[16]; uint8_t cn_args = 0;
        if (type_extract_adt_app(&ct, &cdef, cargs, &cn_args) && cdef)
            record_adt_app_ctor_sigs(cdef, cargs, cn_args, ct);
    }
    g_adt_apps[idx].recording = false;
    return g_adt_apps[idx].name;
}

/* TS4P2: Return a heap-allocated string with the C-name suffix for a concrete
 * ADT-app type (e.g. "__float" for (Maybe float)), or NULL if not applicable.
 * Caller must free() the returned string. */
char *type_adt_app_ctor_suffix(Type t) {
    AdtDef *def = NULL;
    Type args[16];
    uint8_t n_args = 0;
    if (!type_extract_adt_app(&t, &def, args, &n_args) || !def) return NULL;
    if (def->is_gadt) return NULL;
    for (uint32_t i = 0; i < n_args; i++) {
        if (args[i].kind == TY_TYVAR || args[i].kind == TY_UNKNOWN) return NULL;
    }
    /* SR2b: choosing a suffix is a PROMISE that `ctor_<Name><suffix>` exists,
     * and only a REGISTERED app emits its monomorph ctors.  Every caller used
     * to reach here for types that were registered anyway (by being named in a
     * signature); a ctor call deep in a spec body -- `(ok (some v))` grounding
     * to `(Result (Option int) cstr)` -- can be the type's ONLY appearance,
     * and skipping registration left the call referencing an undefined
     * `ctor_Ok__Option__int__cstr` at link time.  Registration is idempotent
     * and an unused registration costs one guarded typedef. */
    (void)type_register_adt_app(t);
    Buf b; buf_init(&b);
    append_adt_app_type_suffix(&b, def, args, n_args);
    buf_putc(&b, '\0');
    char *result = tur_strdup(b.data);
    buf_free(&b);
    return result;
}


/* TS4P1: Emit the typedef and per-constructor functions for one registered ADT app. */
static const char *heap_ptr_c_name(const char *base);  /* defined below */

static void emit_registered_adt_app_rec(Buf *out, uint32_t idx) {
    if (idx >= g_n_adt_apps) return;
    if (g_adt_apps[idx].emitted || g_adt_apps[idx].emitting) return;
    g_adt_apps[idx].emitting = true;

    AdtDef *def = NULL;
    Type args[16];
    uint8_t n_args = 0;
    if (!type_extract_adt_app(&g_adt_apps[idx].type, &def, args, &n_args) || !def) {
        g_adt_apps[idx].emitting = false;
        return;
    }

    const char *adt_inst_name = g_adt_apps[idx].name;

    /* Dependency pre-pass (typedef ordering): a ctor field whose resolved type
     * is itself a parametric monomorph -- e.g. `Cons__Option__int` whose head
     * field is the by-value `Option__int` -- references that nested typedef by
     * value INSIDE this aggregate, so the dependency's typedef MUST precede
     * ours.  The struct-app emitter already does this (emit_registered_struct_
     * app_rec); mirror it here, recursing into both the ADT-app and struct-app
     * registries.  The `emitting` guard above breaks the cycle for a recursive
     * field (a `Cons` tail referencing the same `Cons` monomorph). */
    for (uint32_t ci = 0; ci < def->n_ctors; ci++) {
        CtorDef *ctor = def->ctors[ci];
        for (uint32_t fi = 0; fi < ctor->n_fields; fi++) {
            const CtorField *fld = &ctor->fields[fi];
            if (!fld->full_type) continue;
            Type resolved = substitute_adt_app_type_owned(fld->full_type, def, args);
            if (resolved.kind == TY_APP) {
                /* self-typed-heap-parametric-field-unsupported: a dependency
                 * this record holds by POINTER -- a `:heap` monomorph, whose C
                 * spelling is `tur_adt_Node__int *` -- needs only a forward
                 * `typedef struct X X;`, never its full typedef ahead of ours.
                 * Recursing into it instead breaks a cycle at the wrong point:
                 * for `(defstruct Node :heap [A] (val A) (next (Option (Node
                 * A))))`, `Option__Node__int` is emitted first, its pre-pass
                 * recursed into `Node__int`, and Node's typedef then embedded
                 * the still-incomplete Option BY VALUE (`unknown type name`).
                 * With the forward decl, Option completes first and Node
                 * (which really does need Option whole) follows in the outer
                 * loop.  The same forward decl is what a direct self-reference
                 * (`(next (Node A))`) needs: the field names the typedef the
                 * body is in the middle of introducing. */
                AdtDef *rdef = NULL; Type rargs[16]; uint8_t rn = 0;
                Type rt = resolved;
                bool ptr_dep = type_extract_adt_app(&rt, &rdef, rargs, &rn) &&
                               rdef && rdef->is_heap &&
                               adt_app_is_byvalue_product(resolved);
                for (uint32_t di = 0; di < g_n_adt_apps; di++) {
                    if (type_eq(g_adt_apps[di].type, resolved)) {
                        if (ptr_dep) {
                            const char *dn = g_adt_apps[di].name;
                            buf_printf(out, "#if !defined(TUR_FWD_%s) && "
                                            "!defined(TUR_TY_%s)\n", dn, dn);
                            buf_printf(out, "#define TUR_FWD_%s\n", dn);
                            buf_printf(out, "typedef struct %s %s;\n", dn, dn);
                            buf_printf(out, "#endif\n");
                        } else {
                            emit_registered_adt_app_rec(out, di);
                        }
                        break;
                    }
                }
            }
            /* structdef-retirement slice 1: same forward decl for a non-parametric
             * by-value record-ADT field stored as `tur_adt_T *` (the Result/Option
             * box-as-pointer above).  The user ADT is not in either app registry
             * (non-parametric), so the recursion cannot reach it; a guarded forward
             * `typedef struct tur_adt_T tur_adt_T;` is all a pointer slot needs. */
            if (resolved.kind == TY_ADT && resolved.as.adt_.def &&
                resolved.as.adt_.def->name &&
                !resolved.as.adt_.def->is_heap &&
                /* structdef-retirement slice 5: an opaque newtype ADT has no
                 * `tur_adt_<Name>` typedef (it is the int64 carrier and is never
                 * box-as-pointer'd -- adt_field_is_ros_pointer_box excludes it),
                 * so a forward `typedef struct tur_adt_<Name> ...;` would dangle. */
                !resolved.as.adt_.def->is_opaque &&
                resolved.as.adt_.def->n_type_params == 0) {
                char *un = mangle_adt_name(resolved.as.adt_.def->name);
                /* Also guard on TUR_TD_<Name>, the FULL layout's macro: that
                 * block emits `typedef struct tur_adt_X { ... } tur_adt_X;`
                 * for the same name, so two different guards protected one
                 * typedef and each let the other through.  Re-typedefing a
                 * name is a C11 feature, so cc accepted it with
                 * -Wtypedef-redefinition -- noise in EMITTED code, and a hard
                 * error for anyone compiling the output as C99.  Surfaced by a
                 * `:heap` parametric struct with a by-value struct field
                 * (`crdt/ormap`'s `ctx : DotContext`), which reaches both
                 * emitters for the field type. */
                buf_printf(out, "#if !defined(TUR_FWD_tur_adt_%s) && "
                                "!defined(TUR_TD_tur_adt_%s)\n", un, un);
                buf_printf(out, "#define TUR_FWD_tur_adt_%s\n", un);
                buf_printf(out, "typedef struct tur_adt_%s tur_adt_%s;\n", un, un);
                buf_printf(out, "#endif\n");
                free(un);
            }
            free_struct_app_type(resolved);
        }
    }

    /* Emit the typedef for this monomorphised ADT instance.
     *
     * result-typedef-duplicated-across-modules: same guard story as the
     * struct-app emitter above -- when two module .h files each emit this
     * typedef for the same instantiation, a third TU including both fails
     * with `redefinition` (each anonymous-struct typedef makes a fresh tag).
     * Per-instantiation include guard keyed on the mangled name. */
    bool flat = adt_is_flat_product(def);
    /* SR3 slice B: a niche-filled `(Option P)` has no struct at all -- its C
     * type IS P's, so there is no typedef to emit.  Emitting one anyway would
     * define a `tur_adt_Option__String` nothing references, and `type_c_name`
     * (which answers P's name for this type) would never spell it. */
    bool app_niche = adt_app_is_niche_option(g_adt_apps[idx].type);
    if (!app_niche) {
    buf_printf(out, "#ifndef TUR_TY_%s\n", adt_inst_name);
    buf_printf(out, "#define TUR_TY_%s\n", adt_inst_name);
    /* self-typed-heap-parametric-field-unsupported: the typedef NAME is
     * introduced exactly once, by a guarded forward declaration, and the
     * body below defines only the struct TAG.  The dependency pre-pass above
     * emits the same guarded forward decl for a pointer-held `:heap`
     * monomorph -- including a self-typed one's own name, right before its
     * body -- and `typedef struct X {...} X;` after `typedef struct X X;`
     * is a typedef REDEFINITION: silent under gcc, but clang -std=c99 (the
     * flags `tur build` passes) warns `redefinition of typedef 'X' is a C11
     * feature`, which turned every self-typed monomorph's program into a
     * warning on macOS (tur_offtree_load). */
    buf_printf(out, "#ifndef TUR_FWD_%s\n", adt_inst_name);
    buf_printf(out, "#define TUR_FWD_%s\n", adt_inst_name);
    buf_printf(out, "typedef struct %s %s;\n", adt_inst_name, adt_inst_name);
    buf_printf(out, "#endif\n");
    /* CONV-S1 seam 4 (keystone): a single-variant record monomorph carries the
     * record's real field names (`{ T data; ... }`) -- the parametric analogue
     * of the non-parametric named layout -- so inline-C that reads it by field
     * name (`v->len`, `t.e1`) compiles.  Byte-identical to the positional
     * `union { struct { T _0; } <Ctor>; } as;` form; every access site routes
     * through adt_field_member_path, which keys off this same predicate. */
    bool named = adt_uses_named_layout(def);
    if (named) {
        CtorDef *ctor = def->ctors[0];
        buf_printf(out, "struct %s {\n", adt_inst_name);
        for (uint32_t fi = 0; fi < ctor->n_fields; fi++) {
            const CtorField *fld = &ctor->fields[fi];
            Type fres = (fld->full_type && def)
                ? substitute_adt_app_type_owned(fld->full_type, def, args)
                : type_simple(TY_UNKNOWN, CK_COPY);
            const char *ctype = (type_is_wide_byval_adt(fres) &&
                                 !adt_field_is_ros_pointer_box(def, &fres))
                ? "int64_t"
                : adt_field_c_type(def, fld, args);
            free_struct_app_type(fres);
            char *fname = mangle_field_name(fld->name);
            buf_printf(out, "    %s %s;\n", ctype, fname);
            /* erased-generic-field-read-overruns-subword-monomorph-box, the
             * float32 residue: a type-parameter field instantiated at float32
             * stays a 4-byte `float` (widening it to the int64 slot would
             * VALUE-convert on every implicit store), so pad it to the word.
             * The erased twin reads this slot as int64 and recovers the float
             * with a 4-byte memcpy from the slot's FIRST bytes -- byte position,
             * not value -- which is exactly where the float sits; the pad only
             * keeps that 8-byte read inside the aggregate on any endianness. */
            if (fld->full_type && fld->full_type->kind == TY_TYVAR &&
                ctype && strcmp(ctype, "float") == 0)
                buf_printf(out, "    int32_t __pad_%s;\n", fname);
            free(fname);
        }
        buf_printf(out, "};\n");
        buf_printf(out, "#endif\n\n");
    } else {
    buf_printf(out, "struct %s {\n", adt_inst_name);
    if (!flat) buf_printf(out, "    int tag;\n");
    buf_printf(out, "    union {\n");
    /* subword-payload-box-read-at-wrong-offset: the float32 word pad below
     * fixes the payload's SIZE but not its OFFSET.  After `int tag` a union
     * whose widest member is 4-aligned starts at offset 4, while the carrier
     * monomorph (`int64_t _0`) starts at 8 -- so a carrier read of a boxed
     * `(Option float32)` (a dict slot's word, an open-typed result boxed at
     * production) landed on the pad and answered 0 for 3.25.  An int64 union
     * member aligns the union like the carrier's. */
    {
        bool has_f32_pad = false;
        for (uint32_t ci = 0; ci < def->n_ctors && !has_f32_pad; ci++) {
            CtorDef *ctor = def->ctors[ci];
            for (uint32_t fi = 0; fi < ctor->n_fields && !has_f32_pad; fi++) {
                const CtorField *fld = &ctor->fields[fi];
                if (!fld->full_type || fld->full_type->kind != TY_TYVAR) continue;
                Type fres = def ? substitute_adt_app_type_owned(fld->full_type, def, args)
                                : type_simple(TY_UNKNOWN, CK_COPY);
                const char *ctype = (type_is_wide_byval_adt(fres) &&
                                     !adt_field_is_ros_pointer_box(def, &fres))
                    ? "int64_t"
                    : adt_field_c_type(def, fld, args);
                free_struct_app_type(fres);
                if (ctype && strcmp(ctype, "float") == 0) has_f32_pad = true;
            }
        }
        if (has_f32_pad && !flat) buf_printf(out, "        int64_t __tur_align;\n");
    }
    for (uint32_t ci = 0; ci < def->n_ctors; ci++) {
        CtorDef *ctor = def->ctors[ci];
        /* adt-ctor-underscore-mangles-twice: the member name must be the SAME
         * spelling every access site uses (`adt_field_member_path`, the ctor
         * body, the match arm), which is the injective mangler's -- a literal
         * '_' in the ctor name is "_un" there. This used to be a local
         * "non-alnum -> '_'" fold that kept '_' raw, so `Wrap_q` was declared
         * as `Wrap_q` and addressed as `Wrap_unq`: uncompilable C. */
        char *mctor = mangle_adt_name(ctor->name);
        buf_printf(out, "        struct {");
        for (uint32_t fi = 0; fi < ctor->n_fields; fi++) {
            /* B4 (slice 2): a wide (>8 byte) by-value ADT element is stored as an
             * int64 heap-box pointer, not inline -- so the monomorph layout
             * agrees with the generic int64 carrier the fmap spec reads, and the
             * value crosses the fat-closure boundary via the existing box/deref
             * bridge.  A <= 8 byte by-value ADT (slice 1) still stores inline. */
            const CtorField *fld = &ctor->fields[fi];
            Type fres = (fld->full_type && def)
                ? substitute_adt_app_type_owned(fld->full_type, def, args)
                : type_simple(TY_UNKNOWN, CK_COPY);
            const char *ctype = (type_is_wide_byval_adt(fres) &&
                                 !adt_field_is_ros_pointer_box(def, &fres))
                ? "int64_t"
                : adt_field_c_type(def, fld, args);
            free_struct_app_type(fres);
            buf_printf(out, " %s _%u;", ctype, fi);
            /* float32 residue: same word pad as the named layout above. */
            if (fld->full_type && fld->full_type->kind == TY_TYVAR &&
                ctype && strcmp(ctype, "float") == 0)
                buf_printf(out, " int32_t __pad_%u;", fi);
        }
        buf_printf(out, " } %s;\n", mctor);
        free(mctor);
    }
    buf_printf(out, "    } as;\n");
    buf_printf(out, "};\n");
    buf_printf(out, "#endif\n\n");
    }
    }   /* !app_niche */

    /* Build the type-arg suffix (e.g. "__float"). */
    Buf suffix; buf_init(&suffix);
    append_adt_app_type_suffix(&suffix, def, args, n_args);
    buf_putc(&suffix, '\0');

    /* Parametric-by-value (gated): a concrete by-value flat-product monomorph
     * returns/takes its aggregate by value, exactly like the non-parametric
     * CONV-S1 ctor -- no heap box, no tag.  Hard-off until the crossings wire. */
    bool app_byval = adt_app_is_byvalue_product(g_adt_apps[idx].type);

    /* Emit per-constructor functions for this monomorphised ADT instance.
     *
     * Same cross-emission dedup story as the typedef guard above: the typedef is
     * keyed on TUR_TY_<name>, but the ctor functions were emitted UNGUARDED, so
     * a monomorph reached by two emit paths (e.g. the whole-program pass and the
     * early-file mirror, common once a lowered parametric struct's monomorph --
     * `Option__opaque`, `Vec__opaque` -- is requested from both) produced a
     * `redefinition of ctor_<name>` at cc.  Guard the ctor block on TUR_FN_<name>
     * so the first definition wins, exactly as the typedef does. */
    buf_printf(out, "#ifndef TUR_FN_%s\n", adt_inst_name);
    buf_printf(out, "#define TUR_FN_%s\n", adt_inst_name);
    for (uint32_t ci = 0; ci < def->n_ctors; ci++) {
        CtorDef *ctor = def->ctors[ci];
        /* adt-ctor-underscore-mangles-twice: the union member spelling, shared
         * with the typedef above and every access site (see the note there). */
        char *mctor = mangle_adt_name(ctor->name);
        /* duplicate-ctor-names-collide-in-emitted-c: two different names, and
         * they were the same variable before.  `csym` is the FUNCTION symbol and
         * carries the owning ADT (`ctor_<Adt>_<Ctor><suffix>`); `mctor` stays the
         * bare union MEMBER name (`__r.as.<Ctor>`), which is already scoped by
         * this monomorph's own struct and must NOT be qualified.  Using the
         * shared builder for the symbol also puts this site on the same C-keyword
         * guard as the call sites -- the hand-rolled mangle above has none, so a
         * constructor named after a C keyword used to spell its definition and
         * its calls differently. */
        char *csym = mangle_ctor_symbol(def, ctor->name);

        /* B4 (slice 2): a wide by-value ADT element is passed to the ctor by
         * VALUE (the aggregate) but STORED as an int64 heap box -- so the ctor
         * param type stays the aggregate while the field storage is int64. */
        bool *wide_box = ctor->n_fields
            ? (bool *)calloc(ctor->n_fields, sizeof(bool)) : NULL;
        const char **val_ctype = ctor->n_fields
            ? (const char **)calloc(ctor->n_fields, sizeof(char *)) : NULL;
        for (uint32_t fi = 0; fi < ctor->n_fields; fi++) {
            const CtorField *fld = &ctor->fields[fi];
            Type fres = (fld->full_type && def)
                ? substitute_adt_app_type_owned(fld->full_type, def, args)
                : type_simple(TY_UNKNOWN, CK_COPY);
            val_ctype[fi] = adt_field_c_type(def, fld, args);
            wide_box[fi] = type_is_wide_byval_adt(fres) &&
                           !adt_field_is_ros_pointer_box(def, &fres);
            free_struct_app_type(fres);
        }

        /* CONV-S1 seam 3: a :heap monomorph ctor mallocs the by-value header and
         * returns a TYPED pointer (`tur_adt_Vec__int *`), the ADT analogue of a
         * :heap struct ctor.  Fields are stored INLINE by value in the header
         * (no int64 carrier box) -- the pointer indirection is the whole point. */
        bool app_heap = def->is_heap;
        /* SR3 slice B: a niche ctor is the identity on the payload.  `Some`
         * returns its argument unchanged and `None` returns that pointer type's
         * null -- no struct, no tag, no zeroing prologue.  The C name of the
         * whole type IS the payload's (type_c_name), so `ctor_ret` and the
         * `Some` param spell the same thing. */
        const char *niche_ctype = NULL;
        if (app_niche) niche_ctype = type_c_name(g_adt_apps[idx].type);
        const char *ctor_ret = app_niche ? niche_ctype
                             : app_heap ? heap_ptr_c_name(adt_inst_name)
                             : app_byval ? adt_inst_name : "int64_t";
        if (app_niche) {
            buf_printf(out, "static %s ctor_%s%s(", ctor_ret, csym, suffix.data);
            if (ctor->n_fields == 1)
                buf_printf(out, "%s _0", niche_ctype);
            else
                buf_puts(out, "void");   /* prototyped, not `()` */
            buf_printf(out, ") {\n");
            if (ctor->n_fields == 1) {
                /* option-niche: the eligibility claim ("this payload's valid
                 * values exclude 0") is DECLARED, not proven -- inline-C or a
                 * coercing `::` can still hand `some` a null.  Under the niche
                 * that null IS `(none)`, so an unchecked identity ctor turns
                 * the violation into a silent wrong answer downstream.  Check
                 * it here, at the one point every niche Some passes through,
                 * and die loudly naming the type.  Cost: one compare on a path
                 * that exists to remove a malloc. */
                buf_printf(out,
                    "    if (!_0) { fprintf(stderr, \"tur: (some x) with a "
                    "NULL payload for niche-represented %s -- the payload "
                    "type's :non-null declaration was violated (a null here "
                    "would silently read back as (none))\\n\"); abort(); }\n",
                    adt_inst_name);
                buf_printf(out, "    return _0;\n");
            } else {
                buf_printf(out, "    return (%s)0;\n", niche_ctype);
            }
            buf_printf(out, "}\n\n");
            free(wide_box); free(val_ctype); free(mctor); free(csym);
            continue;
        }
        buf_printf(out, "static %s ctor_%s%s(", ctor_ret, csym, suffix.data);
        for (uint32_t fi = 0; fi < ctor->n_fields; fi++) {
            if (fi > 0) buf_puts(out, ", ");
            buf_printf(out, "%s _%u", val_ctype[fi], fi);
        }
        if (ctor->n_fields == 0) buf_puts(out, "void");   /* prototyped, not `()` */
        buf_printf(out, ") {\n");
        /* CONV-S1 seam 4 (keystone): route every field store through
         * adt_field_member_path so a named-layout monomorph writes `__r->len`
         * and a positional one writes `__r->as.<Ctor>._N`, in lockstep with the
         * typedef + field-read sites. */
        if (app_heap) {
            /* RM3 R2 (docs/archive/regions-plan.md): the spine node, at the
             * MONOMORPH ctor.  Its sibling is the base ctor in emit_module.c
             * (`emit_adt_typedef_and_ctors`) -- the two mirror each other and a
             * change to one belongs in both.  That is not a general caution: it
             * is what happened here.  The first R2 pass changed only the base
             * emitter, and `ctor_Cons_Cons__int` -- the ctor the leak sweep
             * actually blames -- kept its plain malloc, which the reclamation
             * plan's own standing habit predicts ("when a representation change
             * lands in a ctor or temp emitter, grep for the other emitter
             * before calling it done").
             *
             * Gated, so a default build emits the identical malloc. */
            buf_printf(out, "    %s *__r = (%s *)%s(sizeof(%s));\n",
                       adt_inst_name, adt_inst_name,
                       regions_enabled() ? "tur_region_alloc_or_malloc" : "malloc",
                       adt_inst_name);
            if (!flat) buf_printf(out, "    __r->tag = %u;\n", ctor->tag);
            for (uint32_t fi = 0; fi < ctor->n_fields; fi++) {
                char *mp = adt_field_member_path(def, ctor, fi);
                buf_printf(out, "    __r->%s = _%u;\n", mp, fi);
                free(mp);
            }
            buf_printf(out, "    return __r;\n");
        } else if (app_byval) {
            buf_printf(out, "    %s __r;\n", adt_inst_name);
            /* SR2: a by-value SUM monomorph stores its tag and makes the dead
             * bytes deterministic at the least possible cost -- the pad after
             * the tag and the union tail beyond this variant.  Mirrors
             * emit_byval_ctor_prologue (emit_module.c, SR4-perf); a flat
             * product has no tag and skips all three lines. */
            if (!flat) {
                buf_printf(out, "    __r.tag = %u;\n", ctor->tag);
                buf_printf(out,
                    "    memset((char *)&__r + sizeof(__r.tag), 0, "
                    "offsetof(%s, as) - sizeof(__r.tag));\n",
                    adt_inst_name);
                buf_printf(out,
                    "    memset((char *)&__r.as + sizeof(__r.as.%s), 0, "
                    "sizeof(__r.as) - sizeof(__r.as.%s));\n",
                    mctor, mctor);
            }
            for (uint32_t fi = 0; fi < ctor->n_fields; fi++) {
                char *mp = adt_field_member_path(def, ctor, fi);
                /* CONV-S1 seam 4 (B4 wide element in a by-value product): the
                 * typedef stores a >8-byte by-value ADT field as the int64 box
                 * pointer (line ~1459, type_is_wide_byval_adt), so the by-value
                 * ctor must heap-box it too -- not assign the aggregate param into
                 * the int64 slot.  Matches the carrier-ctor branch below and the
                 * accessor's wide-element deref (emit_expr.c). */
                if (wide_box[fi]) {
                    buf_printf(out,
                        "    { %s *__b = (%s *)malloc(sizeof(%s)); *__b = _%u;"
                        " __r.%s = (int64_t)(intptr_t)__b; }\n",
                        val_ctype[fi], val_ctype[fi], val_ctype[fi], fi, mp);
                } else {
                    buf_printf(out, "    __r.%s = _%u;\n", mp, fi);
                }
                free(mp);
            }
            buf_printf(out, "    return __r;\n");
        } else if (adt_ctor_is_null_none(def, ctor)) {
            /* SR3 slice A: the carrier None IS the null pointer.  Every reader
             * already accepts it (tur_is_some(0) is false; a carrier match
             * reads a NULL scrutinee as tag 0), so the box whose only content
             * was `tag = 0` is pure allocation. */
            buf_printf(out, "    return 0;\n");
        } else {
            buf_printf(out, "    %s *__r = (%s *)malloc(sizeof(%s));\n",
                       adt_inst_name, adt_inst_name, adt_inst_name);
            if (!flat) buf_printf(out, "    __r->tag = %u;\n", ctor->tag);
            for (uint32_t fi = 0; fi < ctor->n_fields; fi++) {
                char *mp = adt_field_member_path(def, ctor, fi);
                if (wide_box[fi]) {
                    /* heap-box the wide by-value element; the box is owned by this
                     * carrier node and lives as long as it does. */
                    buf_printf(out,
                        "    { %s *__b = (%s *)malloc(sizeof(%s)); *__b = _%u;"
                        " __r->%s = (int64_t)(intptr_t)__b; }\n",
                        val_ctype[fi], val_ctype[fi], val_ctype[fi], fi, mp);
                    /* region-lock-hardening: the boxed aggregate's words. */
                    buf_printf(out,
                        "    TUR_REGION_NOTE_WORDS((void *)(intptr_t)__r->%s, sizeof(%s));\n",
                        mp, val_ctype[fi]);
                } else {
                    buf_printf(out, "    __r->%s = _%u;\n", mp, fi);
                    /* region-lock-hardening: a malloc'd carrier box holding a
                     * region node is an escape once the box outlives the
                     * bracket; note the field at construction (the twin of the
                     * base-ctor note in emit_module.c). */
                    {
                        Buf lv; buf_init(&lv);
                        buf_printf(&lv, "__r->%s", mp);
                        buf_putc(&lv, '\0');
                        emit_region_note_lvalue(out, 4, val_ctype[fi], lv.data);
                        buf_free(&lv);
                    }
                }
                free(mp);
            }
            buf_printf(out, "    return (int64_t)(intptr_t)__r;\n");
        }
        buf_printf(out, "}\n\n");
        free(wide_box);
        free(val_ctype);
        free(mctor);
        free(csym);
    }
    buf_printf(out, "#endif\n");

    buf_free(&suffix);
    g_adt_apps[idx].emitted  = true;
    g_adt_apps[idx].emitting = false;
}

/* any-fn-tag-does-not-discriminate-signatures: the one renderer for a function
 * type's identity spelling, "(fn [int cstr] : bool)" / "(c-fn [...] : ...)".
 *
 * It was the body of `type_name`'s TY_FN case, lifted out because a second
 * caller now needs the SAME string: `emit_any_type_id` interns a fn payload's
 * `any` box id by this key, and the interpreter has to reproduce it from a
 * TuriClosure's FnDef to answer `is?` / `cast` the way the compiled path does.
 * Two hand-written renderers would drift, and drift here is a silent
 * compiled/interpreted disagreement -- exactly the defect being fixed -- so
 * both sides go through this.
 *
 * The result is interned (process-lifetime, deduped), so the caller neither
 * owns nor frees it, and two calls with the same shape return the same
 * pointer. */
const char *tur_fn_type_key(const uint8_t *arg_kinds, uint32_t arity,
                            TypeKind result_kind, bool cfnptr, bool is_variadic) {
    Buf tmp;
    buf_init(&tmp);
    buf_puts(&tmp, cfnptr ? "(c-fn [" : "(fn [");
    for (uint32_t i = 0; i < arity; i++) {
        if (i > 0) buf_puts(&tmp, " ");
        /* r7rs-lang-plan R6: a variadic's rest slot is spelled `& T`, so
         * `(fn [& any] : any)` and `(fn [any] : any)` get different ids -- the
         * dynamic call reads the id to decide whether to pack surplus
         * arguments into the rest chain (emit_dyn_call / __tur_dyn_call_var),
         * and the two must never be confused: one takes a tagged word where
         * the other takes a chain pointer. */
        if (is_variadic && i + 1 == arity) buf_puts(&tmp, "& ");
        buf_puts(&tmp, type_name(type_from_kind(
                           arg_kinds ? (TypeKind)arg_kinds[i] : TY_UNKNOWN)));
    }
    buf_puts(&tmp, "] : ");
    type_name_buf(&tmp, type_from_kind(result_kind));
    buf_puts(&tmp, ")");
    buf_putc(&tmp, '\0');
    const char *r = intern_type_name(tmp.data);
    buf_free(&tmp);
    return r;
}

/* OWNERSHIP CONTRACT (PH2): type_name has *mixed* ownership -- it returns a
 * static string literal for atomic/primitive kinds but a freshly tur_strdup-ed
 * heap string for composite kinds (TY_FN, TY_HANDLER, TY_UNION, TY_REF,
 * applied structs/ADTs, ...). Callers cannot free the result without crashing
 * on the static cases, so every composite name returned here leaks.
 *
 * For diagnostics that may name a composite type, prefer type_print(Buf*, Type)
 * (a thin public wrapper over type_name_buf): build the name into a Buf you own,
 * pass buf.data to the formatter, and buf_free() it afterward. The handler/
 * intersection/linear arg-mismatch paths in elab_call.c use this pattern and
 * are LeakSanitizer-clean. type_name is retained for the many borrow-only
 * primitive sites; a full strategy-(a) conversion to uniform ownership is a
 * tracked, optional follow-up. */
const char *type_name(Type t) {
    switch (t.kind) {
        /* TY_STRUCT is a retired Type.kind (no Type ever carries it); the
         * enumerator survives only as a reflection/mangle tag.  Unreachable
         * here -- route it to the same placeholder as TY_UNKNOWN. */
        case TY_STRUCT:
        case TY_UNKNOWN: return "?";
        case TY_NIL:     return "nil";
        case TY_BOOL:    return "bool";
        case TY_INT:     return "int";
        case TY_FLOAT:   return "float";
        case TY_INT8:    return "int8";
        case TY_INT16:   return "int16";
        case TY_INT32:   return "int32";
        case TY_INT64:   return "int64";
        case TY_UINT8:   return "uint8";
        case TY_UINT16:  return "uint16";
        case TY_UINT32:  return "uint32";
        case TY_UINT64:  return "uint64";
        case TY_FLOAT32: return "float32";
        case TY_FLOAT64: return "float64";
        case TY_CSTR:    return "cstr";
        case TY_PTR_VOID:
            /* ptr-generic-parameterised-type: render typed pointers as ptr<T>. */
            if (t.as.ptr.inner) {
                const char *inner_n = type_name(*t.as.ptr.inner);
                Buf tmp; buf_init(&tmp);
                buf_puts(&tmp, "ptr<");
                buf_puts(&tmp, inner_n);
                buf_putc(&tmp, '>');
                buf_putc(&tmp, '\0');
                const char *r = intern_type_name(tmp.data);
                buf_free(&tmp);
                return r;
            }
            return "ptr<void>";
        case TY_NEVER:   return "!";
        case TY_TYVAR:   return "tyvar";
        /* IT4: Top type */
        case TY_ANY:     return "any";
        case TY_FN:
            return tur_fn_type_key(t.as.fn.arg_kinds, t.as.fn.arity,
                                   t.as.fn.result_kind, t.as.fn.cfnptr,
                                   t.as.fn.is_variadic);
        case TY_REF: {
            /* Build "ref<T>" name */
            Buf tmp;
            buf_init(&tmp);
            buf_puts(&tmp, "ref<");
            buf_puts(&tmp, type_name(type_from_kind(t.as.ref.inner)));
            buf_puts(&tmp, ">");
            buf_putc(&tmp, '\0');
            const char *r = intern_type_name(tmp.data);
            buf_free(&tmp);
            return r;
        }
        /* LT3: lref<T> — linear ref */
        case TY_LREF: {
            Buf tmp;
            buf_init(&tmp);
            buf_puts(&tmp, "lref<");
            buf_puts(&tmp, type_name(type_from_kind(t.as.ref.inner)));
            buf_puts(&tmp, ">");
            buf_putc(&tmp, '\0');
            const char *r = intern_type_name(tmp.data);
            buf_free(&tmp);
            return r;
        }
        /* Phase 9: rc<T> and weak<T> */
        case TY_RC: {
            /* Build "rc<T>" name */
            Buf tmp;
            buf_init(&tmp);
            buf_puts(&tmp, "rc<");
            buf_puts(&tmp, type_name(type_from_kind(t.as.rc.inner)));
            buf_puts(&tmp, ">");
            buf_putc(&tmp, '\0');
            const char *r = intern_type_name(tmp.data);
            buf_free(&tmp);
            return r;
        }
        case TY_WEAK: {
            /* Build "weak<T>" name */
            Buf tmp;
            buf_init(&tmp);
            buf_puts(&tmp, "weak<");
            buf_puts(&tmp, type_name(type_from_kind(t.as.rc.inner)));
            buf_puts(&tmp, ">");
            buf_putc(&tmp, '\0');
            const char *r = intern_type_name(tmp.data);
            buf_free(&tmp);
            return r;
        }
        /* Phase 12: Borrow types */
        case TY_REF_IMMUT: {
            /* Build "&T" name */
            Buf tmp;
            buf_init(&tmp);
            buf_puts(&tmp, "&");
            buf_puts(&tmp, type_name(t.as.ref_borrow.target_full
                                         ? *t.as.ref_borrow.target_full
                                         : type_from_kind(t.as.ref_borrow.target)));
            buf_putc(&tmp, '\0');
            const char *r = intern_type_name(tmp.data);
            buf_free(&tmp);
            return r;
        }
        case TY_REF_MUT: {
            /* Build "&mut T" name */
            Buf tmp;
            buf_init(&tmp);
            buf_puts(&tmp, "&mut ");
            buf_puts(&tmp, type_name(t.as.ref_borrow.target_full
                                         ? *t.as.ref_borrow.target_full
                                         : type_from_kind(t.as.ref_borrow.target)));
            buf_putc(&tmp, '\0');
            const char *r = intern_type_name(tmp.data);
            buf_free(&tmp);
            return r;
        }
        /* Phase 15: Typeclass types */
        case TY_TYPECLASS:
            return t.as.typeclass.typeclass ? t.as.typeclass.typeclass->name->name : "<typeclass>";
        case TY_TYPECLASS_INST:
            return t.as.typeclass_inst.instance && t.as.typeclass_inst.instance->typeclass ?
                   t.as.typeclass_inst.instance->typeclass->name->name : "<typeclass-inst>";
        /* Phase 17: Exception types */
        case TY_EXCEPTION: {
            /* Build "exception<T>" name */
            Buf tmp;
            buf_init(&tmp);
            buf_puts(&tmp, "exception<");
            buf_puts(&tmp, type_name(type_from_kind(t.as.exn.payload_type)));
            buf_puts(&tmp, ">");
            buf_putc(&tmp, '\0');
            const char *r = intern_type_name(tmp.data);
            buf_free(&tmp);
            return r;
        }
        /* Phase 18: Continuation types */
        case TY_CONT: {
            /* Build "cont<T>" name */
            Buf tmp;
            buf_init(&tmp);
            buf_puts(&tmp, "cont<");
            buf_puts(&tmp, type_name(type_from_kind(t.as.cont.returns)));
            buf_puts(&tmp, ">");
            buf_putc(&tmp, '\0');
            const char *r = intern_type_name(tmp.data);
            buf_free(&tmp);
            return r;
        }
        case TY_CLONEABLE_CONT: {
            /* Build "cloneable_cont<T>" name */
            Buf tmp;
            buf_init(&tmp);
            buf_puts(&tmp, "cloneable_cont<");
            buf_puts(&tmp, type_name(type_from_kind(t.as.cont.returns)));
            buf_puts(&tmp, ">");
            buf_putc(&tmp, '\0');
            const char *r = intern_type_name(tmp.data);
            buf_free(&tmp);
            return r;
        }
        /* Phase G0: ADT types */
        case TY_ADT:
            return t.as.adt_.def ? t.as.adt_.def->name : "<adt>";
        /* Phase HKT-P1: Type application */
        case TY_APP: {
            /* The same `(F a b)` spelling type_name_buf prints. */
            Buf tmp;
            buf_init(&tmp);
            type_name_buf(&tmp, t);
            buf_putc(&tmp, '\0');
            const char *r = intern_type_name(tmp.data);
            buf_free(&tmp);
            return r;
        }
        /* Phase HKT-P2: Recursive types */
        case TY_REC:
            return t.as.rec.name ? t.as.rec.name : "<rec>";
        /* Phase X3: Set literal */
        case TY_SET:
            return "set";
        /* SYM0: interned runtime symbol */
        case TY_SYM:
            return "Sym";
        /* Phase HRT0: Quantified types.
         * Phase EX1b: print constraint vector when present. */
        case TY_FORALL:
        case TY_EXISTS: {
            Buf tmp;
            buf_init(&tmp);
            buf_puts(&tmp, t.kind == TY_FORALL ? "(forall [" : "(exists [");
            for (uint8_t i = 0; i < t.as.forall_.n_vars; i++) {
                if (i > 0) buf_putc(&tmp, ' ');
                buf_puts(&tmp, t.as.forall_.var_names && t.as.forall_.var_names[i]
                                ? t.as.forall_.var_names[i] : "?");
            }
            buf_puts(&tmp, "] ");
            if (t.as.forall_.n_constraints > 0
                    && t.as.forall_.constraint_classes
                    && t.as.forall_.constraint_var_idx) {
                buf_putc(&tmp, '[');
                for (uint8_t i = 0; i < t.as.forall_.n_constraints; i++) {
                    if (i > 0) buf_putc(&tmp, ' ');
                    buf_putc(&tmp, '(');
                    TypeClass *tc = t.as.forall_.constraint_classes[i];
                    buf_puts(&tmp, (tc && tc->name && tc->name->name) ? tc->name->name : "?");
                    buf_putc(&tmp, ' ');
                    uint8_t vi = t.as.forall_.constraint_var_idx[i];
                    if (vi < t.as.forall_.n_vars
                            && t.as.forall_.var_names
                            && t.as.forall_.var_names[vi]) {
                        buf_puts(&tmp, t.as.forall_.var_names[vi]);
                    } else {
                        buf_putc(&tmp, '?');
                    }
                    buf_putc(&tmp, ')');
                }
                buf_puts(&tmp, "] ");
            }
            if (t.as.forall_.body) {
                buf_puts(&tmp, type_name(*t.as.forall_.body));
            } else {
                buf_puts(&tmp, "?");
            }
            buf_putc(&tmp, ')');
            buf_putc(&tmp, '\0');
            const char *r = intern_type_name(tmp.data);
            buf_free(&tmp);
            return r;
        }
        /* IT0: Union types — "(T1 | T2 | ...)" */
        case TY_UNION: {
            Buf tmp;
            buf_init(&tmp);
            buf_putc(&tmp, '(');
            for (uint8_t i = 0; i < t.as.union_.n_members; i++) {
                if (i > 0) buf_puts(&tmp, " | ");
                if (t.as.union_.members && t.as.union_.members[i]) {
                    buf_puts(&tmp, type_name(*t.as.union_.members[i]));
                } else {
                    buf_putc(&tmp, '?');
                }
            }
            buf_putc(&tmp, ')');
            buf_putc(&tmp, '\0');
            const char *r = intern_type_name(tmp.data);
            buf_free(&tmp);
            return r;
        }
        /* IT2: Intersection types — "(T1 & T2 & ...)" */
        case TY_INTERSECTION: {
            Buf tmp;
            buf_init(&tmp);
            buf_putc(&tmp, '(');
            for (uint8_t i = 0; i < t.as.intersection_.n_members; i++) {
                if (i > 0) buf_puts(&tmp, " & ");
                if (t.as.intersection_.members && t.as.intersection_.members[i]) {
                    buf_puts(&tmp, type_name(*t.as.intersection_.members[i]));
                } else {
                    buf_putc(&tmp, '?');
                }
            }
            buf_putc(&tmp, ')');
            buf_putc(&tmp, '\0');
            const char *r = intern_type_name(tmp.data);
            buf_free(&tmp);
            return r;
        }
        /* Stage 1 (macro-system-direction-plan): compile-time syntax object. */
        case TY_SYNTAX:
            return "Syntax";
        /* Variadic HKT rows: short name "#row{T1 T2 ...}". */
        case TY_TYPEROW: {
            Buf tmp;
            buf_init(&tmp);
            buf_puts(&tmp, "#row{");
            const char **names = t.as.typerow_.field_names;
            for (uint8_t i = 0; i < t.as.typerow_.n_elements; i++) {
                if (i > 0) buf_putc(&tmp, ' ');
                if (names && names[i]) {
                    buf_puts(&tmp, names[i]);
                    buf_puts(&tmp, " : ");
                }
                if (t.as.typerow_.elements && t.as.typerow_.elements[i]) {
                    buf_puts(&tmp, type_name(*t.as.typerow_.elements[i]));
                } else {
                    buf_putc(&tmp, '?');
                }
            }
            buf_putc(&tmp, '}');
            buf_putc(&tmp, '\0');
            const char *r = intern_type_name(tmp.data);
            buf_free(&tmp);
            return r;
        }
        /* ET3/FH4.1: Handler type — "handler<EffectSet, ValueType, ResultType>".
         * EffectSet is the handled row ("A | B" for multi-effect); falls back to
         * the single effect_name for legacy types. */
        case TY_HANDLER: {
            Buf tmp;
            buf_init(&tmp);
            buf_puts(&tmp, "handler<");
            if (t.as.handler_.handled_row)
                effect_row_format_names(&tmp, t.as.handler_.handled_row);
            else
                buf_puts(&tmp, t.as.handler_.effect_name ? t.as.handler_.effect_name : "?");
            buf_puts(&tmp, ", ");
            buf_puts(&tmp, type_name(type_from_kind(t.as.handler_.value_kind)));
            buf_puts(&tmp, ", ");
            buf_puts(&tmp, type_name(type_from_kind(t.as.handler_.result_kind)));
            buf_putc(&tmp, '>');
            buf_putc(&tmp, '\0');
            const char *r = intern_type_name(tmp.data);
            buf_free(&tmp);
            return r;
        }
        /* CT0: Contract type — "{ x : T | p }" */
        case TY_CONTRACT: {
            Buf tmp;
            buf_init(&tmp);
            buf_puts(&tmp, "{ ");
            buf_puts(&tmp, t.as.contract_.var_name ? t.as.contract_.var_name : "_");
            buf_puts(&tmp, " : ");
            buf_puts(&tmp, t.as.contract_.base_type
                          ? type_name(*t.as.contract_.base_type) : "?");
            buf_puts(&tmp, " | ... }");
            buf_putc(&tmp, '\0');
            const char *r = intern_type_name(tmp.data);
            buf_free(&tmp);
            return r;
        }
        /* SS0a: Session protocol type names */
        case TY_SESSION: {
            Buf tmp;
            buf_init(&tmp);
            buf_puts(&tmp, "Session[");
            buf_puts(&tmp, t.as.session_.fst ? type_name(*t.as.session_.fst) : "?");
            buf_puts(&tmp, "]");
            buf_putc(&tmp, '\0');
            const char *r = intern_type_name(tmp.data);
            buf_free(&tmp);
            return r;
        }
        case TY_SEND: {
            Buf tmp;
            buf_init(&tmp);
            buf_puts(&tmp, "Send[");
            buf_puts(&tmp, t.as.session_.fst ? type_name(*t.as.session_.fst) : "?");
            buf_puts(&tmp, ", ");
            buf_puts(&tmp, t.as.session_.snd ? type_name(*t.as.session_.snd) : "?");
            buf_puts(&tmp, "]");
            buf_putc(&tmp, '\0');
            const char *r = intern_type_name(tmp.data);
            buf_free(&tmp);
            return r;
        }
        case TY_RECV: {
            Buf tmp;
            buf_init(&tmp);
            buf_puts(&tmp, "Recv[");
            buf_puts(&tmp, t.as.session_.fst ? type_name(*t.as.session_.fst) : "?");
            buf_puts(&tmp, ", ");
            buf_puts(&tmp, t.as.session_.snd ? type_name(*t.as.session_.snd) : "?");
            buf_puts(&tmp, "]");
            buf_putc(&tmp, '\0');
            const char *r = intern_type_name(tmp.data);
            buf_free(&tmp);
            return r;
        }
        case TY_CLOSE:
            return "Close";
        case TY_CHOOSE: {
            Buf tmp;
            buf_init(&tmp);
            buf_puts(&tmp, "Choose[");
            buf_puts(&tmp, t.as.session_.fst ? type_name(*t.as.session_.fst) : "?");
            buf_puts(&tmp, ", ");
            buf_puts(&tmp, t.as.session_.snd ? type_name(*t.as.session_.snd) : "?");
            buf_puts(&tmp, "]");
            buf_putc(&tmp, '\0');
            const char *r = intern_type_name(tmp.data);
            buf_free(&tmp);
            return r;
        }
        case TY_BRANCH: {
            Buf tmp;
            buf_init(&tmp);
            buf_puts(&tmp, "Branch[");
            buf_puts(&tmp, t.as.session_.fst ? type_name(*t.as.session_.fst) : "?");
            buf_puts(&tmp, ", ");
            buf_puts(&tmp, t.as.session_.snd ? type_name(*t.as.session_.snd) : "?");
            buf_puts(&tmp, "]");
            buf_putc(&tmp, '\0');
            const char *r = intern_type_name(tmp.data);
            buf_free(&tmp);
            return r;
        }
        case TY_SESSION_REC: {
            Buf tmp;
            buf_init(&tmp);
            buf_puts(&tmp, "Rec[");
            buf_puts(&tmp, t.as.session_.label ? t.as.session_.label : "?");
            buf_puts(&tmp, ", ");
            buf_puts(&tmp, t.as.session_.fst ? type_name(*t.as.session_.fst) : "?");
            buf_puts(&tmp, "]");
            buf_putc(&tmp, '\0');
            const char *r = intern_type_name(tmp.data);
            buf_free(&tmp);
            return r;
        }
        case TY_TIMEOUT: {
            Buf tmp;
            buf_init(&tmp);
            buf_puts(&tmp, "Timeout[");
            buf_puts(&tmp, t.as.session_.fst ? type_name(*t.as.session_.fst) : "?");
            buf_puts(&tmp, ", ");
            buf_puts(&tmp, t.as.session_.snd ? type_name(*t.as.session_.snd) : "?");
            buf_puts(&tmp, "]");
            buf_putc(&tmp, '\0');
            const char *r = intern_type_name(tmp.data);
            buf_free(&tmp);
            return r;
        }
        case TY_SESSION_PAIR: {
            Buf tmp;
            buf_init(&tmp);
            buf_puts(&tmp, "SessionPair[");
            buf_puts(&tmp, t.as.session_.fst ? type_name(*t.as.session_.fst) : "?");
            buf_puts(&tmp, ", ");
            buf_puts(&tmp, t.as.session_.snd ? type_name(*t.as.session_.snd) : "?");
            buf_puts(&tmp, "]");
            buf_putc(&tmp, '\0');
            const char *r = intern_type_name(tmp.data);
            buf_free(&tmp);
            return r;
        }
        case TY_SESSION_RECV_PAIR: {
            Buf tmp;
            buf_init(&tmp);
            buf_puts(&tmp, "RecvPair[");
            buf_puts(&tmp, t.as.session_.fst ? type_name(*t.as.session_.fst) : "?");
            buf_puts(&tmp, ", ");
            buf_puts(&tmp, t.as.session_.snd ? type_name(*t.as.session_.snd) : "?");
            buf_puts(&tmp, "]");
            buf_putc(&tmp, '\0');
            const char *r = intern_type_name(tmp.data);
            buf_free(&tmp);
            return r;
        }
        case TY_SESSION_OFFER: {
            Buf tmp;
            buf_init(&tmp);
            buf_puts(&tmp, "Offer[");
            buf_puts(&tmp, t.as.session_.fst ? type_name(*t.as.session_.fst) : "?");
            buf_puts(&tmp, ", ");
            buf_puts(&tmp, t.as.session_.snd ? type_name(*t.as.session_.snd) : "?");
            buf_puts(&tmp, "]");
            buf_putc(&tmp, '\0');
            const char *r = intern_type_name(tmp.data);
            buf_free(&tmp);
            return r;
        }
        /* SS5: Global protocol types */
        case TY_GLOBAL:
            return t.as.global_.name ? t.as.global_.name : "<global>";
        case TY_ROLE: {
            Buf tmp;
            buf_init(&tmp);
            buf_puts(&tmp, "Role[");
            buf_puts(&tmp, t.as.role_.global_type
                          ? (t.as.role_.global_type->as.global_.name
                             ? t.as.role_.global_type->as.global_.name : "?")
                          : "?");
            buf_puts(&tmp, ", ");
            buf_puts(&tmp, t.as.role_.role_name ? t.as.role_.role_name : "?");
            buf_puts(&tmp, "]");
            buf_putc(&tmp, '\0');
            const char *r = intern_type_name(tmp.data);
            buf_free(&tmp);
            return r;
        }
        /* DV0: Dynamic var type */
        case TY_DYNVAR: {
            Buf tmp;
            buf_init(&tmp);
            buf_puts(&tmp, "dynvar<");
            buf_puts(&tmp, t.as.dynvar_.value_type ? type_name(*t.as.dynvar_.value_type) : "?");
            buf_puts(&tmp, ">");
            buf_putc(&tmp, '\0');
            const char *r = intern_type_name(tmp.data);
            buf_free(&tmp);
            return r;
        }
        /* GF1: Generator type */
        case TY_GENERATOR:
            return "generator";
    }
    return "?";
}

static void type_name_buf(Buf *b, Type t) {
    switch (t.kind) {
        /* TY_STRUCT is a retired Type.kind: unreachable, route to the
         * TY_UNKNOWN placeholder. */
        case TY_STRUCT:
        case TY_UNKNOWN: buf_puts(b, "?"); break;
        case TY_NIL:     buf_puts(b, "nil"); break;
        case TY_BOOL:    buf_puts(b, "bool"); break;
        case TY_INT:     buf_puts(b, "int"); break;
        case TY_FLOAT:   buf_puts(b, "float"); break;
        case TY_INT8:    buf_puts(b, "int8"); break;
        case TY_INT16:   buf_puts(b, "int16"); break;
        case TY_INT32:   buf_puts(b, "int32"); break;
        case TY_INT64:   buf_puts(b, "int64"); break;
        case TY_UINT8:   buf_puts(b, "uint8"); break;
        case TY_UINT16:  buf_puts(b, "uint16"); break;
        case TY_UINT32:  buf_puts(b, "uint32"); break;
        case TY_UINT64:  buf_puts(b, "uint64"); break;
        case TY_FLOAT32: buf_puts(b, "float32"); break;
        case TY_FLOAT64: buf_puts(b, "float64"); break;
        case TY_CSTR:    buf_puts(b, "cstr"); break;
        case TY_PTR_VOID:
            if (t.as.ptr.inner) {
                buf_puts(b, "ptr<");
                type_name_buf(b, *t.as.ptr.inner);
                buf_putc(b, '>');
            } else {
                buf_puts(b, "ptr<void>");
            }
            break;
        case TY_NEVER:   buf_puts(b, "!"); break;
        case TY_TYVAR:
            /* A named variable prints as its name -- `A`, the spelling the
             * signature wrote -- so `(Chan A)` reads as source, not as
             * `(Chan tyvar 'A')` (parametric-stdlib-diagnostics-print-tyvar-
             * internals).  The name alone still tells cross-skolem mismatches
             * apart (Direction A of
             * docs/archive/history/open-binder-skolems-not-distinguishable.md):
             * `n` vs `__open_skolem_3_0`.  An anonymous one keeps `tyvar`. */
            if (t.as.tyvar_.name) {
                buf_puts(b, t.as.tyvar_.name);
            } else {
                buf_puts(b, "tyvar");
            }
            break;
        /* IT4: Top type */
        case TY_ANY:     buf_puts(b, "any"); break;
        case TY_FN: {
            /* Print each parameter from its FULL type when one is recorded,
             * falling back to the bare kind.  Rendering the kind alone collapses
             * every composite to its constructor and a type variable to nothing,
             * which is how `option-eq?` came to reject a lambda with
             * "expected (fn [int int] : bool), got (fn [int int] : bool)" -- the
             * two types differ, and the message showed neither difference.  A
             * diagnostic a reader cannot act on is worse than none. */
            buf_puts(b, t.as.fn.cfnptr ? "(c-fn [" : "(fn [");
            for (uint32_t i = 0; i < t.as.fn.arity; i++) {
                if (i > 0) buf_puts(b, " ");
                if (t.as.fn.arg_full_types && t.as.fn.arg_full_types[i])
                    type_name_buf(b, *t.as.fn.arg_full_types[i]);
                else
                    type_name_buf(b, type_from_kind(t.as.fn.arg_kinds[i]));
            }
            buf_puts(b, "] : ");
            if (t.as.fn.result_full_type)
                type_name_buf(b, *t.as.fn.result_full_type);
            else
                type_name_buf(b, type_from_kind(t.as.fn.result_kind));
            buf_puts(b, ")");
            break;
        }
        case TY_REF: {
            buf_puts(b, "ref<");
            type_name_buf(b, type_from_kind(t.as.ref.inner));
            buf_puts(b, ">");
            break;
        }
        /* LT3: lref<T> — linear ref */
        case TY_LREF: {
            buf_puts(b, "lref<");
            type_name_buf(b, type_from_kind(t.as.ref.inner));
            buf_puts(b, ">");
            break;
        }
        /* Phase 9: rc<T> and weak<T> */
        case TY_RC: {
            buf_puts(b, "rc<");
            type_name_buf(b, type_from_kind(t.as.rc.inner));
            buf_puts(b, ">");
            break;
        }
        case TY_WEAK: {
            buf_puts(b, "weak<");
            type_name_buf(b, type_from_kind(t.as.rc.inner));
            buf_puts(b, ">");
            break;
        }
        /* Phase 12: Borrow types */
        case TY_REF_IMMUT: {
            buf_puts(b, "&");
            type_name_buf(b, t.as.ref_borrow.target_full
                                 ? *t.as.ref_borrow.target_full
                                 : type_from_kind(t.as.ref_borrow.target));
            /* Phase 13: Add lifetime annotation if present */
            if (t.n_lifetimes > 0) {
                buf_putc(b, ' ');
                lifetimes_format(b, t);
            }
            break;
        }
        case TY_REF_MUT: {
            buf_puts(b, "&mut ");
            type_name_buf(b, t.as.ref_borrow.target_full
                                 ? *t.as.ref_borrow.target_full
                                 : type_from_kind(t.as.ref_borrow.target));
            /* Phase 13: Add lifetime annotation if present */
            if (t.n_lifetimes > 0) {
                buf_putc(b, ' ');
                lifetimes_format(b, t);
            }
            break;
        }
        /* Phase 15: Typeclass types */
        case TY_TYPECLASS: {
            if (t.as.typeclass.typeclass) {
                buf_puts(b, t.as.typeclass.typeclass->name->name);
            } else {
                buf_puts(b, "<typeclass>");
            }
            break;
        }
        case TY_TYPECLASS_INST: {
            if (t.as.typeclass_inst.instance && t.as.typeclass_inst.instance->typeclass) {
                buf_puts(b, t.as.typeclass_inst.instance->typeclass->name->name);
                buf_puts(b, "<");
                for (uint8_t i = 0; i < t.as.typeclass_inst.instance->n_type_args; i++) {
                    if (i > 0) buf_puts(b, ", ");
                    type_name_buf(b, t.as.typeclass_inst.instance->type_args[i]);
                }
                buf_puts(b, ">");
            } else {
                buf_puts(b, "<typeclass-inst>");
            }
            break;
        }
        /* Phase 17: Exception types */
        case TY_EXCEPTION: {
            buf_puts(b, "exception<");
            type_name_buf(b, type_from_kind(t.as.exn.payload_type));
            buf_puts(b, ">");
            break;
        }
        /* Phase 18: Continuation types */
        case TY_CONT: {
            buf_puts(b, "cont<");
            type_name_buf(b, type_from_kind(t.as.cont.returns));
            buf_puts(b, ">");
            break;
        }
        case TY_CLONEABLE_CONT: {
            buf_puts(b, "cloneable_cont<");
            type_name_buf(b, type_from_kind(t.as.cont.returns));
            buf_puts(b, ">");
            break;
        }
        /* Phase G0: ADT types */
        case TY_ADT: {
            if (t.as.adt_.def) {
                buf_puts(b, t.as.adt_.def->name);
            } else {
                buf_puts(b, "<adt>");
            }
            break;
        }
        /* Phase HKT-P1: Type application */
        case TY_APP: {
            /* constrained-hkt-abstract-var-requires-last-param-free: print a
             * hole-headed partial application in its source spelling --
             * `(Result _ cstr)` -- rather than as a bare application, so a
             * diagnostic naming one is recognisable. */
            if (t.as.app.hole_pos_p1 != 0) {
                buf_putc(b, '(');
                if (t.as.app.fn) type_name_buf(b, *t.as.app.fn); else buf_puts(b, "?");
                uint8_t hp = (uint8_t)(t.as.app.hole_pos_p1 - 1);
                buf_puts(b, hp == 0 ? " _ " : " ");
                if (t.as.app.arg) type_name_buf(b, *t.as.app.arg); else buf_puts(b, "?");
                if (hp != 0) buf_puts(b, " _");
                buf_putc(b, ')');
                break;
            }
            /* parametric-stdlib-diagnostics-print-tyvar-internals: the
             * source spelling `(Chan A)`, `(Map K V)` -- not `(type-app Chan
             * tyvar 'A')`.  A curried application is flattened along its
             * spine, so `((Map K) V)` prints as the `(Map K V)` it was
             * written as; a hole-headed partial application keeps the arm
             * above. */
            {
                const Type *spine[16];
                uint32_t n = 0;
                const Type *head = &t;
                while (head && head->kind == TY_APP && head->as.app.hole_pos_p1 == 0 &&
                       n < 16) {
                    spine[n++] = head->as.app.arg;
                    head = head->as.app.fn;
                }
                buf_putc(b, '(');
                if (head) type_name_buf(b, *head); else buf_puts(b, "?");
                for (uint32_t k = n; k > 0; k--) {
                    buf_putc(b, ' ');
                    if (spine[k - 1]) type_name_buf(b, *spine[k - 1]); else buf_puts(b, "?");
                }
                buf_putc(b, ')');
            }
            break;
        }
        /* Phase HKT-P2: Recursive types */
        case TY_REC: {
            buf_puts(b, t.as.rec.name ? t.as.rec.name : "<rec>");
            break;
        }
        /* Phase X3: Set literal */
        case TY_SET: {
            buf_puts(b, "set");
            break;
        }
        /* SYM0: interned runtime symbol -- prints as :Sym */
        case TY_SYM: {
            buf_puts(b, ":Sym");
            break;
        }
        /* Phase HRT0: Quantified types — always print quantifiers explicitly.
         * Phase EX1b: print optional constraint vector. */
        case TY_FORALL:
        case TY_EXISTS: {
            buf_puts(b, t.kind == TY_FORALL ? "(forall [" : "(exists [");
            for (uint8_t i = 0; i < t.as.forall_.n_vars; i++) {
                if (i > 0) buf_putc(b, ' ');
                buf_puts(b, t.as.forall_.var_names && t.as.forall_.var_names[i]
                             ? t.as.forall_.var_names[i] : "?");
            }
            buf_puts(b, "] ");
            if (t.as.forall_.n_constraints > 0
                    && t.as.forall_.constraint_classes
                    && t.as.forall_.constraint_var_idx) {
                buf_putc(b, '[');
                for (uint8_t i = 0; i < t.as.forall_.n_constraints; i++) {
                    if (i > 0) buf_putc(b, ' ');
                    buf_putc(b, '(');
                    TypeClass *tc = t.as.forall_.constraint_classes[i];
                    buf_puts(b, (tc && tc->name && tc->name->name) ? tc->name->name : "?");
                    buf_putc(b, ' ');
                    uint8_t vi = t.as.forall_.constraint_var_idx[i];
                    if (vi < t.as.forall_.n_vars
                            && t.as.forall_.var_names
                            && t.as.forall_.var_names[vi]) {
                        buf_puts(b, t.as.forall_.var_names[vi]);
                    } else {
                        buf_putc(b, '?');
                    }
                    buf_putc(b, ')');
                }
                buf_puts(b, "] ");
            }
            if (t.as.forall_.body) type_name_buf(b, *t.as.forall_.body);
            else buf_puts(b, "?");
            buf_putc(b, ')');
            break;
        }
        /* IT0: Union types — "(T1 | T2 | ...)" */
        case TY_UNION: {
            buf_putc(b, '(');
            for (uint8_t i = 0; i < t.as.union_.n_members; i++) {
                if (i > 0) buf_puts(b, " | ");
                if (t.as.union_.members && t.as.union_.members[i]) {
                    type_name_buf(b, *t.as.union_.members[i]);
                } else {
                    buf_putc(b, '?');
                }
            }
            buf_putc(b, ')');
            break;
        }
        /* IT2: Intersection types — "(T1 & T2 & ...)" */
        case TY_INTERSECTION: {
            buf_putc(b, '(');
            for (uint8_t i = 0; i < t.as.intersection_.n_members; i++) {
                if (i > 0) buf_puts(b, " & ");
                if (t.as.intersection_.members && t.as.intersection_.members[i]) {
                    type_name_buf(b, *t.as.intersection_.members[i]);
                } else {
                    buf_putc(b, '?');
                }
            }
            buf_putc(b, ')');
            break;
        }
        /* Stage 1 (macro-system-direction-plan): compile-time syntax object. */
        case TY_SYNTAX:
            buf_puts(b, "Syntax");
            break;
        /* Variadic HKT rows: print as the surface form `#row{T1 T2 ...}`. */
        case TY_TYPEROW: {
            buf_puts(b, "#row{");
            const char **names = t.as.typerow_.field_names;
            for (uint8_t i = 0; i < t.as.typerow_.n_elements; i++) {
                if (i > 0) buf_putc(b, ' ');
                if (names && names[i]) {
                    buf_puts(b, names[i]);
                    buf_puts(b, " : ");
                }
                if (t.as.typerow_.elements && t.as.typerow_.elements[i]) {
                    type_name_buf(b, *t.as.typerow_.elements[i]);
                } else {
                    buf_putc(b, '?');
                }
            }
            buf_putc(b, '}');
            break;
        }
        /* ET3: Handler type */
        case TY_HANDLER: {
            buf_puts(b, "handler<");
            /* PH2.2: prefer the full handled-effect *row* (e.g. "Ask | Tell")
             * when present, matching type_name; fall back to the single
             * effect_name only when no row is attached. */
            if (t.as.handler_.handled_row)
                effect_row_format_names(b, t.as.handler_.handled_row);
            else
                buf_puts(b, t.as.handler_.effect_name ? t.as.handler_.effect_name : "?");
            buf_puts(b, ", ");
            type_name_buf(b, type_from_kind(t.as.handler_.value_kind));
            buf_puts(b, ", ");
            type_name_buf(b, type_from_kind(t.as.handler_.result_kind));
            buf_putc(b, '>');
            break;
        }
        /* CT0: Contract type */
        case TY_CONTRACT: {
            buf_puts(b, "{ ");
            buf_puts(b, t.as.contract_.var_name ? t.as.contract_.var_name : "_");
            buf_puts(b, " : ");
            if (t.as.contract_.base_type) {
                type_name_buf(b, *t.as.contract_.base_type);
            } else {
                buf_putc(b, '?');
            }
            buf_puts(b, " | ... }");
            break;
        }
        /* SS0a: Session protocol types */
        case TY_SESSION:
            buf_puts(b, "Session[");
            if (t.as.session_.fst) type_name_buf(b, *t.as.session_.fst);
            else buf_putc(b, '?');
            buf_putc(b, ']');
            break;
        case TY_SEND:
            buf_puts(b, "Send[");
            if (t.as.session_.fst) type_name_buf(b, *t.as.session_.fst);
            else buf_putc(b, '?');
            buf_puts(b, ", ");
            if (t.as.session_.snd) type_name_buf(b, *t.as.session_.snd);
            else buf_putc(b, '?');
            buf_putc(b, ']');
            break;
        case TY_RECV:
            buf_puts(b, "Recv[");
            if (t.as.session_.fst) type_name_buf(b, *t.as.session_.fst);
            else buf_putc(b, '?');
            buf_puts(b, ", ");
            if (t.as.session_.snd) type_name_buf(b, *t.as.session_.snd);
            else buf_putc(b, '?');
            buf_putc(b, ']');
            break;
        case TY_CLOSE:
            buf_puts(b, "Close");
            break;
        case TY_CHOOSE:
            buf_puts(b, "Choose[");
            if (t.as.session_.fst) type_name_buf(b, *t.as.session_.fst);
            else buf_putc(b, '?');
            buf_puts(b, ", ");
            if (t.as.session_.snd) type_name_buf(b, *t.as.session_.snd);
            else buf_putc(b, '?');
            buf_putc(b, ']');
            break;
        case TY_BRANCH:
            buf_puts(b, "Branch[");
            if (t.as.session_.fst) type_name_buf(b, *t.as.session_.fst);
            else buf_putc(b, '?');
            buf_puts(b, ", ");
            if (t.as.session_.snd) type_name_buf(b, *t.as.session_.snd);
            else buf_putc(b, '?');
            buf_putc(b, ']');
            break;
        case TY_SESSION_REC:
            buf_puts(b, "Rec[");
            buf_puts(b, t.as.session_.label ? t.as.session_.label : "?");
            buf_puts(b, ", ");
            if (t.as.session_.fst) type_name_buf(b, *t.as.session_.fst);
            else buf_putc(b, '?');
            buf_putc(b, ']');
            break;
        case TY_TIMEOUT:
            buf_puts(b, "Timeout[");
            if (t.as.session_.fst) type_name_buf(b, *t.as.session_.fst);
            else buf_putc(b, '?');
            buf_puts(b, ", ");
            if (t.as.session_.snd) type_name_buf(b, *t.as.session_.snd);
            else buf_putc(b, '?');
            buf_putc(b, ']');
            break;
        case TY_SESSION_PAIR:
            buf_puts(b, "SessionPair[");
            if (t.as.session_.fst) type_name_buf(b, *t.as.session_.fst);
            else buf_putc(b, '?');
            buf_puts(b, ", ");
            if (t.as.session_.snd) type_name_buf(b, *t.as.session_.snd);
            else buf_putc(b, '?');
            buf_putc(b, ']');
            break;
        case TY_SESSION_RECV_PAIR:
            buf_puts(b, "RecvPair[");
            if (t.as.session_.fst) type_name_buf(b, *t.as.session_.fst);
            else buf_putc(b, '?');
            buf_puts(b, ", ");
            if (t.as.session_.snd) type_name_buf(b, *t.as.session_.snd);
            else buf_putc(b, '?');
            buf_putc(b, ']');
            break;
        case TY_SESSION_OFFER:
            buf_puts(b, "Offer[");
            if (t.as.session_.fst) type_name_buf(b, *t.as.session_.fst);
            else buf_putc(b, '?');
            buf_puts(b, ", ");
            if (t.as.session_.snd) type_name_buf(b, *t.as.session_.snd);
            else buf_putc(b, '?');
            buf_putc(b, ']');
            break;
        /* SS5: Global protocol types */
        case TY_GLOBAL:
            buf_puts(b, t.as.global_.name ? t.as.global_.name : "<global>");
            break;
        case TY_ROLE:
            buf_puts(b, "Role[");
            buf_puts(b, t.as.role_.global_type
                        ? (t.as.role_.global_type->as.global_.name
                           ? t.as.role_.global_type->as.global_.name : "?")
                        : "?");
            buf_puts(b, ", ");
            buf_puts(b, t.as.role_.role_name ? t.as.role_.role_name : "?");
            buf_putc(b, ']');
            break;
        /* DV0: Dynamic var type */
        case TY_DYNVAR:
            buf_puts(b, "dynvar<");
            if (t.as.dynvar_.value_type) type_name_buf(b, *t.as.dynvar_.value_type);
            else buf_putc(b, '?');
            buf_putc(b, '>');
            break;
        /* GF1: Generator type */
        case TY_GENERATOR:
            buf_puts(b, "generator<");
            buf_puts(b, type_name(type_from_kind(t.as.generator_.element_kind)));
            buf_putc(b, '>');
            break;
    }
}

/* end-to-end-monomorphization: intern "<base> *" for a :heap-tagged type whose
 * monomorphic ABI is a typed pointer to its heap-allocated header. `base` is the
 * by-value struct C name (e.g. "Vec__int"); the result is "Vec__int *". */
static const char *heap_ptr_c_name(const char *base) {
    Buf b; buf_init(&b);
    buf_puts(&b, base);
    buf_puts(&b, " *");
    buf_putc(&b, '\0');
    const char *r = intern_type_name(b.data);
    buf_free(&b);
    return r;
}

/* Increment 4 stage 3 (lens family): the typed heap-pointer C name for a
 * :heap record ADT, built from its def -- `tur_adt_<Name> *`.  Exists
 * because type_c_name's TY_ADT arm gates the heap-pointer spelling on
 * adt_is_byvalue_product, which a :heap record with heap-struct FIELDS
 * fails -- so type_c_name says int64_t while the ctor emitter returns the
 * typed pointer (the Line-family drift).  A chokepoint that wants the
 * pointer spelling for such a def asks here. */
const char *adt_heap_ptr_c_name(const AdtDef *def) {
    return heap_ptr_c_name(adt_byval_c_name(def));
}

/* CONV-S1: the stable C typedef name (`tur_adt_<mangled>`) for the BY-VALUE
 * representation of a non-parametric flat-product ADT.  Mirrors the name the
 * emitters build for the base typedef (emit_module.c:emit_adt_typedef_and_ctors)
 * -- `tur_adt_` + mangle_adt_name(def->name) -- so signatures, constructors,
 * `match`, and field-access all agree on one spelling.  The mangler (replace any
 * non `[A-Za-z0-9_]` byte with `_`) is replicated inline here so the type layer
 * does not have to reach up into the emit layer; the result is interned (same
 * stable, leak-checked storage heap_ptr_c_name uses) so type_c_name can return a
 * stable `const char *`.  Reached for every leaf product now that
 * adt_is_byvalue_product() is LIVE (CONV-S1/B3). */
/* CONV-S1/B3: gate the by-value ADT representation.  A single-variant, non-GADT,
 * NON-parametric flat product flows by value -- BUT only when it is a "leaf":
 * every field a scalar (int/bool/float/cstr/ptr/...).  A field that is itself an
 * aggregate (ADT / type-application / struct / type variable) is stored as an
 * int64 carrier in the tagged-union typedef -- its storage `kind` is collapsed to
 * TY_INT, so the real type lives in `full_type` (e.g. `(ExprF Expr)` reads as
 * kind TY_INT with a TY_APP full_type).  Such an aggregate child would need the
 * byval<->carrier field bridge on both sides; the recursive HKT carriers
 * (Expr = (Roll (ExprF Expr)), Re = (Roll (ReF Re))) are exactly that non-leaf
 * case and stay on the carrier path until B4.  Deliberately conservative -- a
 * precise transitive recursion check can widen this to non-recursive
 * aggregate-bearing products later, once every crossing is wired. */
/* CONV-S1 (slice 4): a depth-guarded by-value-product check, used both as the
 * public entry point and recursively by adt_field_is_inline_byval_d to decide
 * whether a nested aggregate field is itself a by-value product (and so can be
 * stored INLINE rather than via the int64 carrier).  A by-value product cannot
 * contain itself by value (that is infinite size and ill-formed), so any cycle
 * is rejected upstream; the depth cap is a belt-and-braces loop guard, not a
 * semantic limit. */
static bool adt_is_byvalue_product_d(const AdtDef *def, int depth);

/* CONV-S1 (slice 4): true when a ctor field is itself a by-value aggregate that
 * should be stored INLINE (by value) in the owning by-value product, exactly as
 * `defstruct` inlines a nested struct field -- instead of boxing it behind the
 * int64 carrier.  An inline candidate is a non-heap, non-opaque, drop-glue-free
 * struct, or a by-value ADT product with no drop glue.  Restricting to
 * drop-glue-free inners keeps the outer product trivially copyable, so the
 * outer needs no recursive drop glue. */
static bool adt_field_is_inline_byval_d(const CtorField *f, int depth) {
    if (depth <= 0 || !f || !f->full_type) return false;
    const Type *ft = f->full_type;
    if (ft->kind == TY_ADT) {
        const AdtDef *ad = ft->as.adt_.def;
        /* seam 3: a :heap ADT is a typed POINTER, not an inline aggregate -- store
         * it as the pointer carrier, exactly as a :heap struct field is excluded. */
        return ad && !ad->is_heap && !ad->needs_drop_glue &&
               adt_is_byvalue_product_d(ad, depth - 1);
    }
    if (ft->kind == TY_APP) {
        /* structdef-retirement slice 1: an APPLIED/parametric by-value monomorph
         * field (`(Option cstr)` -> `tur_adt_Option__cstr`, `(Box int)`, `(Pair2
         * cstr int)`) is a by-value aggregate stored INLINE exactly as a nested
         * struct/ADT field is.  A :heap monomorph is a typed pointer (excluded,
         * like the TY_ADT/TY_STRUCT :heap cases); a drop-glue-bearing base def is
         * excluded to keep the owning product trivially copyable. */
        const AdtDef *ad = type_adt_app_def(ft);
        return ad && !ad->is_heap && !ad->needs_drop_glue &&
               adt_app_is_byvalue_product(*ft);
    }
    return false;
}

/* SR1: does the ADT reference graph rooted at `from` reach `target`?
 *
 * A by-value product embeds its by-value ADT fields INLINE, so a CYCLE in that
 * graph has no finite C layout.  Self-recursion (`(Node :Tree :Tree)`) is the
 * one-step case and was the only one the gate excluded; the mutual case is the
 * same defect one hop further out -- `(defdata Expr (Expr :ExprNode))` against
 * `(defdata ExprNode (Lit :int) (Add :Expr :Expr))`, where admitting `ExprNode`
 * makes each type embed the other.
 *
 * That shape is worse than an unorderable typedef, which is how it surfaced.
 * `adt_is_byvalue_product_d` walks the same graph under a depth budget, so on a
 * cycle it answers the SAME question differently at different depths -- and the
 * typedef emitter (depth 16 from the owner) and a field-store site (depth 16
 * from the field) enter at different points.  Two emission sites disagreeing
 * about one type's layout is a silent miscompile, not a build error.
 *
 * Conservative by construction: it follows every non-`:heap` ADT-typed field,
 * whether or not that field would actually be inlined, and treats "not known
 * yet" (a NULL ctor mid-definition, an unresolved def, an exhausted budget) as
 * reaching.  Declining leaves the type exactly where it is with SR1 off -- on
 * the int64 carrier -- so a false positive costs representation, never
 * correctness.  A `:heap` field is a typed POINTER, which breaks the cycle the
 * same way the carrier does. */
static bool adt_graph_reaches(const AdtDef *from, const AdtDef *target,
                              const AdtDef **seen, uint32_t *n_seen,
                              uint32_t cap) {
    if (!from || !target) return true;
    for (uint32_t i = 0; i < *n_seen; i++)
        if (seen[i] == from) return false;      /* already fully explored */
    if (*n_seen >= cap) return true;            /* out of budget */
    seen[(*n_seen)++] = from;
    if (!from->ctors) return true;
    for (uint32_t ci = 0; ci < from->n_ctors; ci++) {
        const CtorDef *c = from->ctors[ci];
        if (!c) return true;                    /* ctor array still filling */
        for (uint32_t fi = 0; fi < c->n_fields; fi++) {
            const Type *ft = c->fields[fi].full_type;
            if (!ft || ft->kind != TY_ADT) continue;
            const AdtDef *fd = ft->as.adt_.def;
            if (!fd) return true;
            if (fd == target) return true;
            /* A field naming the target by NAME but not yet resolved to its
             * AdtDef is the same forward reference wearing a different hat. */
            if (fd->name && target->name &&
                strcmp(fd->name, target->name) == 0) return true;
            if (fd->is_heap) continue;
            if (adt_graph_reaches(fd, target, seen, n_seen, cap)) return true;
        }
    }
    return false;
}

/* SR1 prototype gate (docs/archive/sum-representation-plan.md).  True when
 * `def` is a MULTI-VARIANT sum that could flow by value as a tag+union
 * aggregate: non-parametric, non-GADT, non-heap, and not recursive.
 *
 * The two recursion exclusions below are NOT the same test and are not
 * redundant, which is worth stating because an earlier revision of this comment
 * conflated them:
 *
 *   - `is_self_recursive` is the SR1/SR4 PHASE boundary.  Such a type lowers by
 *     value perfectly well -- its recursive field is a one-word carrier, so the
 *     layout is finite, and the gate proved a self-recursive `Tree` compiles and
 *     runs.  (The plan's original claim that `(TPair :Term :Term)` "has no
 *     finite inline size" is wrong for Turmeric.)  It is excluded because of
 *     library source, not layout: see the comment at the test itself.
 *
 *   - `adt_graph_reaches` is a SOUNDNESS gate over the INLINE-by-value field
 *     graph, where a cycle genuinely has no finite layout.  It covers the
 *     mutual case, which the phase boundary does not.
 *
 * Note what this does NOT touch: `adt_is_flat_product` still reports false for
 * these, so the tagged-union typedef, the tag store in each ctor and the tag
 * test in `match` all stay exactly as they are.  Only the ABI moves.  Today
 * those two questions -- "has no tag" and "flows by value" -- are conflated,
 * because adt_is_byvalue_product_d requires adt_is_flat_product; separating
 * them is the whole of SR1. */
static bool adt_sr1_sum_candidate(const AdtDef *def) {
    if (!g_sr1_sum_byvalue) return false;
    if (!def || def->is_gadt || def->is_heap) return false;
    if (def->n_ctors < 2 || def->n_type_params != 0) return false;
    if (!def->ctors) return false;
    /* SR4 measurement seam (2026-08-27): TUR_SR4_RECURSIVE_BYVALUE=1 admitted
     * recursive sums to the by-value path (the default since RM4, below).  The suite is GREEN with it on --
     * every codegen blocker is fixed (the fat-dispatch ABI disagreement,
     * resolved by unifying every fat boundary on the b4box convention via
     * thunk_param_slot_c_name; see the archived
     * fat-dispatch-wide-byvalue-aggregate-argument report) -- so this line is
     * now a PERFORMANCE decision, not a correctness one, and it was measured
     * before being made (re-measured after the SR4-perf ctor-prologue fix,
     * which halved the original ~1.4x regression -- the whole-union `{0}`
     * zero-init was half the cost; see emit_byval_ctor_prologue):
     *
     *   logic.tur bind+walk, 400k passes:  carrier 0.40s / by-value 0.45s
     *                                      peak RSS 116 MB -> 51 MB
     *   re.tur compile+match, 5k passes:   carrier 14 ms / by-value 15 ms
     *
     * By value halves the mallocs (the payload no longer boxes; the spine box
     * per node remains) but each walk step still deref-COPIES a 24-48 byte
     * aggregate out of the spine box where the carrier copied one word --
     * ~1.13x slower on logic, ~1.07x on regex, for ~2.2x less memory.  The
     * residue is spread across many small copies (an arg spill here, a
     * return-slot copy there; each hand-measured at ~2%), so there is no
     * second big lever short of pointer-binding match fields.  The SR plan
     * says to justify this phase against the post-reclamation baseline; the
     * trade is now close enough that a memory-constrained workload could
     * reasonably take it, but the default stays carrier until one asks.  is_self_recursive is the
     * boundary; adt_graph_reaches below is the separate SOUNDNESS gate over
     * inline-field cycles and is never bypassed. */
    /* RM4 (reclamation-plan.md, 2026-09-02): the default FLIPPED to by value.
     * Re-measured on the same two workloads after RM0 established that no
     * arena is coming to make the carrier's mallocs cheap (RM2/RM3 have no
     * constituency): logic bind+walk 400k passes carrier 0.49s / by-value
     * 0.51s (~1.03x) at 370 MB -> 202 MB peak RSS; re compile+match 5k passes
     * 68-90 ms / 65-71 ms (no slower) at 41 MB -> 33 MB.  The time side of the
     * trade shrank to noise while the memory side held, and the premise for
     * waiting is gone.  TUR_SR4_RECURSIVE_CARRIER=1 restores the carrier for
     * A/B measurement; tests/run-sr4-seam.sh keeps THAT path from rotting now. */
    if (def->is_self_recursive && getenv("TUR_SR4_RECURSIVE_CARRIER"))
        return false;
    for (uint32_t ci = 0; ci < def->n_ctors; ci++) {
        const CtorDef *c = def->ctors[ci];
        /* The predicate is reached DURING elaboration of the very def it is
         * asked about -- a self-recursive `(Node :Tree :Tree)` queries `Tree`
         * while `Tree`'s ctor array is still being filled, so an entry can be
         * NULL.  Decline rather than guess: a def whose variants are not all
         * known yet cannot be shown non-recursive, and admitting it by default
         * is how a recursive sum would slip onto the by-value path. */
        if (!c) return false;
        for (uint32_t i = 0; i < c->n_fields; i++) {
            const CtorField *f = &c->fields[i];
            if (f->full_type && f->full_type->kind == TY_APP) return false;
        }
    }
    /* Direct AND mutual recursion, in one check: a sum that can reach itself
     * through ADT-typed fields has no finite by-value layout. */
    const AdtDef *seen[64];
    uint32_t n_seen = 0;
    if (adt_graph_reaches(def, def, seen, &n_seen, 64)) return false;
    return true;
}

static bool adt_is_byvalue_product_d(const AdtDef *def, int depth) {
    const bool sr1_sum = adt_sr1_sum_candidate(def);
    if (!sr1_sum && (!adt_is_flat_product(def) || def->n_type_params != 0))
        return false;
    /* Every variant's fields must be by-value-able, not just variant 0's --
     * a union is only as flat as its widest arm. */
    for (uint32_t ci = 0; ci < (sr1_sum ? def->n_ctors : 1u); ci++) {
    const CtorDef *c = def->ctors[ci];
    for (uint32_t i = 0; i < c->n_fields; i++) {
        const CtorField *f = &c->fields[i];
        /* slice 4: a by-value aggregate field is inlined -- admit it. */
        if (adt_field_is_inline_byval_d(f, depth)) continue;
        /* B4 (graduated 2026-06-25): a recursive carrier field -- an (F Self)
         * type-application that stays on the int64 carrier -- does not disqualify
         * the owning product from flowing by value.  A single such field yields
         * an 8-byte wrapper around the carrier int64 (Re/Expr) that crosses the
         * fat closure by reinterpret; a wider product carrying an (F Self) field
         * plus extra scalars flows by value too, its by-value-ADT element fields
         * riding a heap box in the parametric carrier monomorph
         * (type_is_wide_byval_adt). */
        if (f->full_type && f->full_type->kind == TY_APP) continue;
        if (f->full_type) {
            TypeKind fk = f->full_type->kind;
            if (fk == TY_ADT || fk == TY_APP || fk == TY_STRUCT ||
                fk == TY_TYVAR || fk == TY_FORALL || fk == TY_EXISTS)
                return false;
        }
        TypeKind k = f->kind;
        if (k == TY_ADT || k == TY_APP || k == TY_STRUCT || k == TY_TYVAR)
            return false;
    }
    }
    return true;
}

bool adt_is_byvalue_product(const AdtDef *def) {
    return adt_is_byvalue_product_d(def, 16);
}

/* B4 (byvalue-recursive-carrier): true when `def` is a single-variant,
 * single-field recursive carrier wrapper -- its sole field is an (F Self)
 * type-application that stays on the int64 carrier (e.g. Re = (Roll (ReF Re)),
 * Expr = (Roll (ExprF Expr))).  Such a value is an 8-byte wrapper whose by-value
 * representation IS its carrier int64, so it crosses the fat-closure boundary by
 * reinterpreting the carrier (no heap box, no deref).  (B4 graduated 2026-06-25;
 * unconditional.) */
bool adt_is_byval_recursive_carrier_wrapper(const AdtDef *def) {
    if (!def || !adt_is_flat_product(def) || def->n_type_params != 0) return false;
    const CtorDef *c = def->ctors[0];
    if (c->n_fields != 1) return false;
    const CtorField *f = &c->fields[0];
    return f->full_type && f->full_type->kind == TY_APP;
}

/* True when `arg_def` and `functor_def` form a FIXPOINT PAIR: `arg_def` is a
 * non-parametric ADT one of whose fields is a type-application headed by
 * `functor_def`, so `Expr`/`ExprF` and `Re`/`ReF` qualify.  Such a pair's C
 * typedefs are mutually recursive, and only the carrier breaks the cycle -- the
 * functor monomorph `tur_adt_ExprF__Expr` is emitted before `tur_adt_Expr`
 * exists, so laying its fields out inline is an incomplete type, not merely a
 * representation choice.
 *
 * Deliberately broader than adt_is_byval_recursive_carrier_wrapper, which is
 * about a DIFFERENT property (an 8-byte wrapper whose by-value form IS its
 * carrier int64) and whose sole-field requirement its own callers rely on. */
bool adt_is_fixpoint_partner_of(const AdtDef *arg_def, const AdtDef *functor_def) {
    if (!arg_def || !functor_def || arg_def->n_type_params != 0) return false;
    if (!arg_def->ctors) return false;
    for (uint32_t ci = 0; ci < arg_def->n_ctors; ci++) {
        const CtorDef *c = arg_def->ctors[ci];
        if (!c) continue;
        for (uint32_t i = 0; i < c->n_fields; i++) {
            const CtorField *f = &c->fields[i];
            if (!f->full_type || f->full_type->kind != TY_APP) continue;
            Type ft = *f->full_type;
            if (type_adt_app_def(&ft) == functor_def) return true;
        }
    }
    return false;
}

/* CONV-S1 (slice 4): public predicate -- is this field stored inline by value?
 * (See adt_field_is_inline_byval_d.)  Codegen sites that emit the field type,
 * construct the product, read a field, or bind a match pattern consult this to
 * choose the inline-aggregate path over the int64-carrier path. */
bool adt_field_is_inline_byval(const CtorField *f) {
    return adt_field_is_inline_byval_d(f, 16);
}

/* CONV-S1 (slice 4): the inline C type name for a by-value-aggregate field
 * (e.g. "tur_adt_Pt" for a nested by-value ADT, or a struct's C name).  Only
 * valid when adt_field_is_inline_byval(f). */
const char *adt_field_inline_c_name(const CtorField *f) {
    return type_c_name(*f->full_type);
}

/* CONV-S1 (slice 3): a by-value ADT product is laid out like a struct, so it
 * adopts the same size-gated calling convention -- a product whose fields sum to
 * more than 16 bytes is passed as `const tur_adt_<Name> *` (pass-by-pointer)
 * rather than by value, exactly as `defstruct` does (StructDef.pass_by_ptr,
 * elab_structs.c).  Only by-value products qualify (carrier ADTs already flow as
 * an int64 heap pointer).  The field kinds are the scalar/pointer set
 * adt_is_byvalue_product admits -- aggregates are rejected -- so the byte sizes
 * are computed locally here (types.c does not pull in elab's type_size_bytes). */
static size_t adt_field_size_bytes(TypeKind k) {
    switch (k) {
        case TY_BOOL:  case TY_INT8:  case TY_UINT8:                 return 1;
        case TY_INT16: case TY_UINT16:                              return 2;
        case TY_INT32: case TY_UINT32: case TY_FLOAT32:             return 4;
        case TY_NIL:                                                return 0;
        default:                                                    return 8;
    }
}

/* slice 4: size of a single ctor field, recursing into an inline by-value
 * aggregate (its fields contribute their own bytes rather than a single 8-byte
 * carrier slot), so the >16-byte pass-by-pointer threshold lines up with the
 * nested-struct layout. */
static size_t adt_byval_value_size_bytes_d(const AdtDef *def, int depth);

static size_t adt_ctor_field_size_bytes(const CtorField *f, int depth) {
    if (depth > 0 && adt_field_is_inline_byval_d(f, depth) && f->full_type) {
        if (f->full_type->kind == TY_ADT && f->full_type->as.adt_.def) {
            /* SR1: an inline field can be a SUM now, so its size is the shared
             * whole-value computation (tag + widest variant), not a walk of
             * ctors[0].  Before SR1 only a flat product could be inlined, so
             * the two were the same loop. */
            return adt_byval_value_size_bytes_d(f->full_type->as.adt_.def,
                                                depth - 1);
        }
    }
    return adt_field_size_bytes(f->kind);
}

bool adt_byval_pass_by_ptr(const AdtDef *def) {
    /* seam 3: a :heap ADT already lowers to a typed pointer `tur_adt_<Name> *`,
     * so it is passed by value (the pointer itself), never pass-by-pointer-
     * wrapped -- wrapping would make the param a double pointer and address-of
     * the call site (`bsum(&p)` where `p` is already a pointer). */
    if (def && def->is_heap) return false;
    if (!adt_is_byvalue_product(def)) return false;
    return adt_byval_value_size_bytes(def) > 16;
}

/* B4: the by-value (aggregate) byte size of a by-value ADT product -- the sum of
 * its sole ctor's field sizes.  Used to decide whether a by-value-ADT element
 * stored in a parametric carrier monomorph fits the int64 carrier slot directly
 * (<= 8 bytes, reinterpret; slice 1) or must ride a heap box (> 8 bytes; slice
 * 2).  Returns 0 for a non-by-value product. */
static size_t adt_byval_value_size_bytes_d(const AdtDef *def, int depth) {
    if (!adt_is_byvalue_product_d(def, depth > 0 ? depth : 1)) return 0;
    /* SR1: a multi-variant sum's by-value size is the TAG WORD plus its WIDEST
     * variant -- `struct { int tag; union { ... } as; }` -- not a walk of
     * ctors[0].  The ctors[0]-only accounting (a flat-product assumption from
     * before sums could be by value) sized `Subst` by its narrow SNil variant
     * as 8 bytes, so type_is_wide_byval_adt called a ~40-byte value
     * carrier-slot-safe: the closure b4box preamble never fired and the value
     * was truncated to whatever the int64 slot held.  It also made
     * adt_byval_pass_by_ptr's answer depend on DECLARATION ORDER.
     *
     * The 8 for the tag models `int tag;` padded to the union's 8-byte
     * alignment -- exact for the int64-class fields these layouts hold, and
     * safely conservative for the >8 / >16 threshold questions this number
     * feeds.  Single-variant products keep the old sum-of-fields answer,
     * byte for byte. */
    const bool tagged = def->n_ctors > 1;
    size_t widest = 0;
    for (uint32_t ci = 0; ci < (tagged ? def->n_ctors : 1u); ci++) {
        const CtorDef *c = def->ctors[ci];
        if (!c) continue;
        size_t total = 0;
        for (uint32_t i = 0; i < c->n_fields; i++)
            total += adt_ctor_field_size_bytes(&c->fields[i],
                                               depth > 0 ? depth : 1);
        if (total > widest) widest = total;
    }
    return (tagged ? 8u : 0u) + widest;
}

size_t adt_byval_value_size_bytes(const AdtDef *def) {
    return adt_byval_value_size_bytes_d(def, 16);
}

/* B4 (byvalue-recursive-carrier, slice 2): true when `t` resolves to a WIDE
 * (> 8 byte) by-value ADT -- one that cannot be carried in the int64 carrier
 * slot directly.  Such a value, when it is an element field of a parametric
 * carrier monomorph (e.g. the (ExprF Expr) element of a wide Expr), must be
 * stored BOXED (an int64 heap pointer) so the monomorph layout agrees with the
 * generic int64 carrier the fmap spec reads, and so it crosses the fat-closure
 * boundary by box+deref (the existing B3 bridge) rather than inline.  This is a
 * pure size predicate; its call sites are guarded (monomorph boxing fires only
 * for parametric-carrier elements; the field-read raw-carrier path only for an
 * erased int64 binding) so an ordinary wide by-value ADT match keeps its
 * existing B3 treatment. */
bool type_is_wide_byval_adt(Type t) {
    if (t.kind == TY_ADT && t.as.adt_.def) {
        /* van-laarhoven-lens-composition (Gap B2): a `:heap` struct/ADT is a typed
         * POINTER (rides the int64 carrier as an 8-byte pointer), never a wide
         * by-value aggregate -- so it must not take the box+deref closure-param
         * bridge, or a struct-typed lens-adapter param (`p : Point`, Point being
         * `:heap`) is wrongly dereferenced through a bogus box pointer -> SIGSEGV.
         * Mirrors the `def->is_heap` guard already in adt_app_byval_pass_by_ptr. */
        if (t.as.adt_.def->is_heap) return false;
        return adt_byval_value_size_bytes(t.as.adt_.def) > 8;
    }
    return false;
}

/* The b4box CLOSURE-SLOT width question, deliberately NOT folded into
 * type_is_wide_byval_adt.
 *
 * A wide by-value aggregate crossing a closure/thunk parameter slot rides the
 * int64 box-pointer convention: the typed-thunk typedef spells the slot
 * `int64_t`, the dispatch site boxes, the thunk body derefs at entry.  A
 * concrete parametric monomorph -- `(ExprF (fn [int] int))`, 24 bytes and
 * pass-by-pointer at the callee -- belongs to that convention exactly as its
 * non-parametric sibling does; spelled BY VALUE in the fat-dispatch cast, the
 * call hands an aggregate to a `const T *` slot and the program SEGVs.  Same
 * shape as the archived fat-dispatch-wide-byvalue-aggregate-argument, one
 * representation over.
 *
 * The separation is load-bearing, not tidiness.  type_is_wide_byval_adt also
 * drives ADT FIELD layout, where a parametric monomorph must stay INLINE:
 * `(Cons (Option int))` lays its `head` out as a `tur_adt_Option__int`, and
 * answering "wide" there retypes the field to the int64 carrier while every
 * reader still binds the aggregate ("invalid initializer" -- six fixtures on
 * the DEFAULT path).  Two questions, two predicates; only the closure-slot
 * lockstep set (thunk_param_slot_c_name, the typed fatshims, the dispatch-site
 * boxing, the thunk-body deref) consults this one. */
bool type_is_b4box_closure_slot(Type t) {
    if (type_is_wide_byval_adt(t)) return true;
    if (t.kind == TY_APP) return adt_app_byval_value_size_bytes(t) > 8;
    return false;
}

/* fnsan-ptr-void-fn-slot-word: the closure-slot WORD question.  An untyped
 * `ptr<void>` and a function value are both pointer-sized handles a program
 * can erase into one another -- `(:: f :ptr<void>)`, a `(Vec ptr<void>)` of
 * closures, a `: ptr<void>` result re-ascribed as `(fn [ptr<void>] ...)` --
 * so the slot-0 signature a closure is DEFINED at and the one a re-ascribing
 * caller CASTS to must spell the two parameters alike, or the call is an
 * indirect call through the wrong function type (a -fsanitize=function trap,
 * a call_indirect trap on WASM).  A function-typed parameter is already the
 * int64 word in every definition (ER4), and every erased consumer calls slot
 * 0 at words, so the shared spelling is the word: `int64_t`.  Parameters
 * only -- a fat function result and a `ptr<void>` result are both `void *`
 * already.  A typed `ptr<T>`, `ptr<const-void>` and a cfnptr keep their real
 * C types: they are FFI spellings, not erasure. */
bool type_is_word_closure_slot(Type t) {
    if (t.kind == TY_PTR_VOID) return !t.as.ptr.inner && !t.as.ptr.is_const;
    if (t.kind == TY_FN) return !t.as.fn.cfnptr;
    return false;
}

ContainerElemForm container_elem_form(Type elem) {
    /* CE1: one chokepoint, defined AS repr_of's container answer so the two
     * cannot drift (the retired-shadow argument below applies here too).  A
     * niche option resolves to REPR_HEAP_PTR in repr_of's SR3 arm, which is
     * what makes it CE_WORD; a by-value aggregate is REPR_BOXED_AGG. */
    return repr_of(&elem, REPR_POS_CONTAINER_ELEM) == REPR_BOXED_AGG ? CE_BOX
                                                                     : CE_WORD;
}

/* Increment 3 (representation-consolidation meta-plan): the CONTAINER-ELEMENT
 * boxing predicate.  True when `t` is a non-heap by-value ADT product of ANY
 * width -- the class of element that must be heap-boxed when stored into a
 * heap container (Vec slot, HAMT value) and deref-unboxed on read-back.
 *
 * This deliberately drops the > 8-byte width fork of type_is_wide_byval_adt
 * for container slots: a NARROW (<= 8 byte) by-value struct has no other
 * working slot representation -- the plain concrete->carrier bridge spills it
 * to a stack local whose address dangles once the frame returns, and the read
 * side had no un-spill at all (vec-byvalue-struct-element-invalid-c).  Width
 * still matters in positions with a paired inline layout (monomorph fields,
 * B4 closure params); those keep type_is_wide_byval_adt.  Every container
 * BOXING site, its ownership probe (tur-wide-byval? / tur-vec-elem-wide?
 * folds), and the read-back recovery must consult THIS predicate so the four
 * decisions cannot drift. */
bool type_is_boxed_container_elem(Type t) {
    /* Increment 4, THE COLLAPSE (2026-08-16): this predicate is now
     * `repr_of`'s answer rather than a second derivation of it.  That is what
     * increment 4 was for -- "collapse the per-site representation choices
     * into the single repr-of(type, position) routine".
     *
     * The shadow that measured this position reported one disagreement class:
     * a concrete by-value APP element (`(Vec (Option int))`), where repr_of
     * said BOXED and this predicate said not-boxed.  The diagnosis at the
     * time was "two mechanisms deciding one thing and AGREEING" -- the push
     * side really does malloc the monomorph either way -- and that was true
     * of the BOXING half and false of the OWNERSHIP half.  Measured: the
     * `tur-vec-elem-wide?` / `tur-wide-byval?` folds consult this predicate,
     * so they answered 0 for app elements and `vec-free` never released the
     * boxes the push side had allocated.  `(Vec (Option int))` leaked one
     * element box per push (32 bytes / 2 allocations under LeakSanitizer for
     * a two-push probe, while the sibling `(Vec Sm)` in the same program
     * freed both of its).  Two mechanisms that agree on one half of a
     * decision and disagree on the other is exactly the defect shape this
     * campaign exists to close. */
    bool boxed = repr_of(&t, REPR_POS_CONTAINER_ELEM) == REPR_BOXED_AGG;
    /* The shadow that lived here is retired BY CONSTRUCTION: with the
     * predicate defined as repr_of's answer, want and got are the same
     * expression and a disagreement is unrepresentable.  That is the
     * intended end state for a consolidated position -- a chokepoint that
     * cannot drift needs no instrument watching it drift -- and it is why
     * `run-repr-trace.sh` now asserts SILENCE on the shape that used to be
     * the pinned TY_APP row.  The other six positions keep their shadows
     * because their sites still derive their own answers.
     *
     * Kept deliberately: the >0-size and non-heap conditions now live in
     * repr_of (heap types return HEAP_PTR before the by-value arm; a
     * transparent int newtype returns SCALAR_BITS), so this function has no
     * conditions of its own left to drift.
     */
    return boxed;
}

/* Parametric-by-value: app-aware sibling of adt_byval_pass_by_ptr.  A concrete
 * flat-product ADT-app (e.g. `(Pair2 int float)`) is laid out like its
 * monomorph aggregate, so it adopts the same >16-byte pass-by-pointer
 * convention.  Each field's byte size is taken from its monomorphised kind
 * (substituting the app args for the base def's type params); aggregate fields
 * default to the 8-byte carrier slot size, monotone for the threshold. */
/* The in-memory size of a by-value parametric monomorph: the tag word (when the
 * def is a sum) plus its WIDEST substituted variant -- exactly what
 * adt_byval_value_size_bytes_d computes on the non-parametric side.  Returns 0
 * when `t` is not a by-value monomorph at all, so every caller can treat 0 as
 * "not applicable" rather than "zero-sized".
 *
 * Every width question about such a value routes here: pass-by-pointer (> 16,
 * the SysV threshold) and the b4box closure-slot convention (> 8) are two
 * thresholds on ONE size, and reading `ctors[0]` alone made the answer depend on
 * declaration order. */
size_t adt_app_byval_value_size_bytes(Type t) {
    if (!adt_app_is_byvalue_product(t)) return 0;
    /* SR3 slice B: a niche-filled Option IS one pointer -- 8 bytes, whatever
     * the tagged layout its ctors describe would have measured.  Answering 16
     * here would make it "wide" for the b4box closure-slot convention and get
     * it heap-boxed as a carrier element, which is the allocation the niche
     * exists to remove. */
    if (adt_app_is_niche_option(t)) return 8;
    AdtDef *def = NULL;
    Type args[16];
    uint8_t n_args = 0;
    if (!type_extract_adt_app(&t, &def, args, &n_args) || !def) return 0;
    /* seam 3: a :heap parametric ADT monomorph is a typed pointer, not an
     * aggregate -- it has no by-value width. */
    if (def->is_heap) return 0;
    const bool tagged = def->n_ctors > 1;
    size_t widest = 0;
    for (uint32_t ci = 0; ci < (tagged ? def->n_ctors : 1u); ci++) {
        const CtorDef *c = def->ctors[ci];
        if (!c) continue;
        size_t total = 0;
        for (uint32_t i = 0; i < c->n_fields; i++) {
            TypeKind k = c->fields[i].kind;
            if (c->fields[i].full_type) {
                Type rf = substitute_adt_app_type_owned(c->fields[i].full_type, def, args);
                k = rf.kind;
                free_struct_app_type(rf);
            }
            total += adt_field_size_bytes(k);
        }
        if (total > widest) widest = total;
    }
    return (tagged ? 8u : 0u) + widest;
}

bool adt_app_byval_pass_by_ptr(Type t) {
    return adt_app_byval_value_size_bytes(t) > 16;
}

/* Parametric-by-value: true when `t` is a by-value monomorph -- either a
 * non-parametric flat-product TY_ADT (adt_is_byvalue_product) or a concrete
 * parametric flat-product TY_APP (adt_app_is_byvalue_product).  Codegen sites
 * that choose the by-value (aggregate, non-carrier) representation for a
 * scrutinee or value consult this single app-aware predicate. */
bool type_is_byvalue_adt_product(Type t) {
    if (t.kind == TY_ADT) return adt_is_byvalue_product(t.as.adt_.def);
    if (t.kind == TY_APP) return adt_app_is_byvalue_product(t);
    return false;
}

/* Parametric-by-value monomorphisation (heavy prerequisite for CONV-S1
 * graduation; see docs/archive/parametric-adt-byvalue-plan.md).
 *
 * A concrete monomorphisation of a single-variant, non-GADT, parametric flat
 * product -- e.g. `(Pair2 int float)` from `(defdata Pair2 [A B] (Pair2 [a:A
 * b:B]))` -- has a fully concrete layout (`tur_adt_Pair2__int__float`) and is
 * representationally a by-value aggregate, exactly like a monomorphised struct.
 * Today it still flows through the int64 heap-pointer carrier (the ctor mallocs
 * and returns int64, the param is int64).  Flipping it to by-value is the
 * parametric analog of CONV-S1/B1-B4 and the foundation the `:heap` typed-pointer
 * ADT ABI builds on.
 *
 * LIVE (P2-P4).  The plumbing keyed on this predicate (type_c_name TY_APP,
 * type_uses_carrier_abi, the monomorphised ctor emitter) flows a concrete
 * flat-product ADT-app by value, and both crossings the gate-on smoke test
 * enumerated are wired: Crossing A (match / field-access on an ADT-app
 * receiver binds the aggregate and reads with `.`, with an app-aware
 * pass-by-pointer size gate -- adt_app_byval_pass_by_ptr) and Crossing B (a
 * by-value ADT-app value boxes into / unboxes out of a carrier ctor field
 * slot via emit_type_is_byvalue_adt).  The M7 by-value-HKT carriers
 * (`ReF`/`ExprF`) carry a residual tyvar field and are excluded by the
 * predicate, so this never touches that machinery (B4 remains separate).
 *
 * The `g_adt_app_byvalue` bool this paragraph used to introduce is GONE
 * (2026-09-04).  It was `static const bool ... = true`, written nowhere and read
 * once as `if (!g_adt_app_byvalue) return false;` -- a gate frozen open when P2-P4
 * went live, which the compiler had been folding away ever since.  The prose is
 * kept because it still describes the predicate below; only the dead bit is
 * removed. */

/* SR2a (docs/archive/sum-representation-plan.md SR2): a MULTI-VARIANT
 * parametric sum monomorph (`(Opt2 int)`, and above all `(Option int)` /
 * `(Result int cstr)`) flows by value -- the app-side sibling of SR1's
 * adt_sr1_sum_candidate, and the prerequisite SR2b's stdlib conversion is
 * built on: on the carrier, the conversion takes the two most-used types in
 * the language to heap-boxed-and-leaked, the exact regression the plan's
 * section 5 warns about (SR1 as shipped covers non-parametric sums only).
 * Same exclusions as the SR1 gate: non-GADT, non-heap, and not self-recursive
 * (`(Cons2 a (Cons2 a))` stays on the carrier -- SR4's population, measured
 * and declined there).
 *
 * GRADUATED 2026-08-27 out of `--enable=parametric-sum-byvalue`: the row's
 * soak existed so the representation would not be fixed against SR2b, and
 * SR2b has landed (in-tree and across the spices).  `TUR_SR2_APP_SUM_BYVALUE=0`
 * is the bisection escape hatch, read once in main.c the way SR1's is. */
static bool sr2_app_sum_byvalue(void) {
    return g_sr2_app_sum_byvalue;
}

/* self-typed-heap-field-overflows-the-compiler: the defs whose field walk is
 * in progress.  A field naming an application of the SAME def -- `(next (Node
 * A))` in `(defstruct Node :heap [A] (val A) (next (Node A)))` -- re-entered
 * this predicate on `(Node int)` from inside `(Node int)`, with no base case:
 * `is_self_recursive` is not set for that shape, so the SR2b exclusion below
 * never saw it and `tur check` overflowed its stack.  A def already on this
 * stack answers false (a self-referential application cannot be inlined as a
 * flat by-value product), which is also what the recursive-field marking
 * decides for the non-parametric case. */
static bool adt_app_is_byvalue_product_inner(Type t, AdtDef *def, Type *args,
                                             uint8_t n_args);
#define ADT_APP_BYVAL_DEPTH 32
static const AdtDef *g_adt_app_byval_stack[ADT_APP_BYVAL_DEPTH];
static uint32_t g_adt_app_byval_n = 0;

bool adt_app_is_byvalue_product(Type t) {
    AdtDef *def = NULL;
    Type args[16];
    uint8_t n_args = 0;
    if (!type_extract_adt_app(&t, &def, args, &n_args) || !def) return false;
    for (uint32_t si = 0; si < g_adt_app_byval_n; si++)
        if (g_adt_app_byval_stack[si] == def) return false;
    if (g_adt_app_byval_n >= ADT_APP_BYVAL_DEPTH) return false;
    g_adt_app_byval_stack[g_adt_app_byval_n++] = def;
    bool r = adt_app_is_byvalue_product_inner(t, def, args, n_args);
    g_adt_app_byval_n--;
    return r;
}

static bool adt_app_is_byvalue_product_inner(Type t, AdtDef *def, Type *args,
                                             uint8_t n_args) {
    (void)t;
    bool app_sum = sr2_app_sum_byvalue() && def->n_ctors > 1 &&
                   !def->is_gadt && !def->is_heap &&
                   !def->is_self_recursive && def->ctors != NULL;
    /* SR2 exclusion: an app over a recursive-carrier wrapper -- `(ExprF Expr)`,
     * `(ReF Re)`, the fixpoint functors -- stays on B4's machinery.  Its element
     * rides the int64 carrier by DESIGN (the wrapper is the carrier), and
     * admitting the functor here spells `tur_adt_Expr` fields inline into a
     * typedef that PRECEDES the wrapper's own ("parameter has incomplete type"),
     * colliding with everything B4 already does for these shapes.  This is the
     * same boundary the g_adt_app_byvalue comment above draws for the
     * flat-product path.
     *
     * The question is fixpoint PARTNERSHIP, not B4's narrower
     * adt_is_byval_recursive_carrier_wrapper: that predicate means "an 8-byte
     * wrapper whose by-value representation IS its carrier int64", so it
     * requires a SOLE field and its caller in emit_expr.c reinterprets the
     * carrier on that promise.  `Expr = (Roll :int (ExprF Expr))` carries a tag
     * alongside the carrier field, so it is not that -- yet `(ExprF Expr)` is
     * still the typedef-ordering cycle this exclusion exists to avoid.  Ask the
     * broader question here and leave B4's promise alone. */
    for (uint32_t i = 0; app_sum && i < n_args; i++)
        if (args[i].kind == TY_ADT &&
            adt_is_fixpoint_partner_of(args[i].as.adt_.def, def))
            app_sum = false;
    if (!adt_is_flat_product(def) && !app_sum) return false; /* single-variant, non-GADT */
    if (def->n_type_params == 0) return false;          /* non-parametric is CONV-S1's path */
    /* A nested by-value-product element (`(Cons (Option int))`'s `(Option int)`)
     * is accepted ONLY for a :heap outer.  A :heap cell stores its element inline
     * and its field read needs the ADT monomorph layout; a non-heap nested
     * aggregate already round-trips via the struct-app monomorph path, so leaving
     * it untouched avoids perturbing the constrained-instance-body specs. */
    for (uint32_t i = 0; i < n_args; i++)
        if (!adt_app_type_arg_is_concrete(&args[i])) return false;
    /* Every monomorphised field must resolve to a by-value-able concrete type --
     * no residual tyvar / HKT / non-concrete application (those are M7's job).
     * SR2: for a sum, EVERY variant's fields -- a union is only as flat as its
     * widest arm, exactly as adt_is_byvalue_product_d checks on the
     * non-parametric side. */
    for (uint32_t ci = 0; ci < (app_sum ? def->n_ctors : 1u); ci++) {
    const CtorDef *c = def->ctors[ci];
    if (!c) return false;                    /* ctor array still filling */
    for (uint32_t i = 0; i < c->n_fields; i++) {
        if (!c->fields[i].full_type) continue;          /* scalar storage -- by value */
        Type rf = substitute_adt_app_type_owned(c->fields[i].full_type, def, args);
        TypeKind k = rf.kind;
        bool bad = (k == TY_TYVAR || k == TY_FORALL || k == TY_EXISTS) ||
                   (k == TY_APP && !type_has_concrete_codegen_layout(&rf) &&
                    !adt_app_is_byvalue_product(rf));
        /* SR2b: a field that NAMES a concrete monomorph is storable whatever
         * its representation -- `(Cons (Option int))`'s `head` holds a
         * carrier-riding sum monomorph as the int64 word its own type_c_name
         * spells, exactly as a scalar field holds its word.  This is the field
         * loop's copy of the arg loop's adt_app_type_arg_is_concrete widening
         * (the two must move together; see type_app_is_concrete_adt).  Vec
         * never needed it because its element parameter is PHANTOM -- no
         * field mentions it -- which is why `(Vec (Option int))` worked while
         * `(Cons (Option int))` fell to the carrier and the let binder ICEd
         * against repr_of's heap-ptr answer.  Excluded for a self-recursive
         * def, where accepting the self-field would inline the typedef into
         * itself. */
        /* self-typed-heap-parametric-field-unsupported: a `:heap` def's
         * self-reference -- `(next (Node A))` on a `:heap` Node -- is a
         * pointer-sized word in the record (the monomorph IS a typed pointer
         * to its heap header), so it inlines no typedef into itself and the
         * self-recursive exclusion does not apply.  Without this the app fell
         * to the int64 carrier here while repr_of said typed pointer, and the
         * repr shadow refused the binding.  A non-heap self-recursive def keeps
         * the exclusion: there the field really would be the aggregate. */
        if (bad && k == TY_APP && (!def->is_self_recursive || def->is_heap) &&
            type_app_is_concrete_adt(&rf))
            bad = false;
        free_struct_app_type(rf);
        if (bad) return false;
    }
    }
    return true;
}

const char *adt_byval_c_name(const AdtDef *def) {
    Buf b; buf_init(&b);
    buf_puts(&b, "tur_adt_");
    /* Shared with type_register_adt_app above and mangle_field_name in
     * emit_core.c -- all three must spell a given ADT identically or the
     * typedef and its use sites name different types. */
    append_c_ident_mangled(&b, def->name);
    buf_putc(&b, '\0');
    const char *r = intern_type_name(b.data);
    buf_free(&b);
    return r;
}

const char *type_c_name(Type t) {
    /* c-fn-ptr-element-and-size-precision-gap fix: a precise FFI spelling on an
     * integer carrier overrides the by-TypeKind name.  Only set on full Types
     * preserved for cfnptr signatures (CNUM_DEFAULT everywhere else), so this
     * never perturbs ordinary carrier lowering. */
    if (t.c_num_spelling == CNUM_SIZE_T)    return "size_t";
    if (t.c_num_spelling == CNUM_PTRDIFF_T) return "ptrdiff_t";
    switch (t.kind) {
        /* Simple payload-free kinds: the C name comes from the shared repr
         * table (TY_SIMPLE_REPR_ROWS), the single place their three
         * representation answers live. */
#define X(k, cn, mg, lay) case k: return cn;
        TY_SIMPLE_REPR_ROWS(X)
#undef X
        case TY_PTR_VOID:
            /* ptr-generic-parameterised-type: a typed ptr<T> lowers to `T *`.
             * The legacy untyped ptr<void> (inner == NULL) stays `void *`. */
            if (t.as.ptr.inner) {
                const char *inner_c = type_c_name(*t.as.ptr.inner);
                Buf b; buf_init(&b);
                /* c-fn-ptr-element-and-size-precision-gap fix: `ptr<const-T>`
                 * lowers to `const T *` so a typed c-fn slot matches a C
                 * callback's const-qualified pointer parameter. */
                if (t.as.ptr.is_const) buf_puts(&b, "const ");
                buf_puts(&b, inner_c);
                buf_puts(&b, " *");
                buf_putc(&b, '\0');
                const char *r = intern_type_name(b.data);
                buf_free(&b);
                return r;
            }
            /* ptr<const-void> -- the untyped-pointer spelling with a const
             * qualifier (e.g. a `const void *userData` C-callback slot). */
            if (t.as.ptr.is_const) return "const void *";
            return "void *";
        case TY_FN: {
            /* typed-c-abi-function-pointers: a cfnptr lowers to the concrete
             * bare `R (*)(A...)` typedef -- it is a raw C function pointer with
             * no implicit environment, so it never uses the int64_t/void*
             * carrier.  Falls back to void* only when the signature is not
             * fully concrete (no real C function-pointer type can be formed). */
            if (t.as.fn.cfnptr) {
                const char *td = register_fn_ptr_typedef(&t);
                return td ? td : "void *";
            }
            /* CRU B-1: a boxed TY_FN (first-class closure value) is carried as
             * a void* holding the { thunk, env... } box -- identical to the
             * TY_PTR_VOID carrier, so closures and legacy :ptr<void> sinks
             * share one C declaration. */
            if (t.as.fn.boxed) return "void *";
            /* Closure-returning-instance-method codegen: a curried closure
             * return -- a function whose result is itself a function (e.g.
             * (fn [:int] (fn [:int] :int))) -- is a single fat-closure handle
             * at runtime, carried as int64_t.  Recursing into the result kind
             * here would unwrap to a zeroed TY_FN shell (result kind
             * TY_UNKNOWN) and emit an unknown-void carrier, silently dropping
             * the handle.  A bare TY_UNKNOWN result reaches this path the same way.
             * Carry both as the int64_t handle.  Non-curried bare function
             * references keep returning their result type's C name below. */
            if (t.as.fn.result_kind == TY_FN || t.as.fn.result_kind == TY_UNKNOWN)
                return "int64_t";
            /* EXPERIMENT: a non-boxed, non-cfnptr fn is a closure handle. */
            return "int64_t";
        }
        case TY_REF: {
            /* ref<T> lowers to a pointer to T in C */
            /* For now, we use void* for all refs (simple approach) */
            /* TODO: could use T* directly but void* is simpler for v1 */
            return "void *";
        }
        /* LT3: lref<T> lowers identically to ref<T> in C */
        case TY_LREF:
            return "void *";
        /* Phase 9: rc<T> and weak<T> both lower to RcControlBlock* in C */
        case TY_RC:
        case TY_WEAK:
            return "RcControlBlock *";
        /* Phase 12: Borrow types lower to pointers in C */
        case TY_REF_IMMUT: {
            /* &T lowers to const T* in C (immutable borrow) */
            /* For now, use void* since we don't have full type info */
            return "const void *";
        }
        case TY_REF_MUT: {
            /* &mut T lowers to T* in C (mutable borrow) */
            /* For now, use void* since we don't have full type info */
            return "void *";
        }
        /* Phase 15: Typeclass types don't have a C representation - they're compile-time only */
        case TY_TYPECLASS:
        case TY_TYPECLASS_INST:
            return "void";  /* Typeclasses don't exist at runtime */
        /* Phase 17: Exceptions removed. TY_EXCEPTION is orphaned; lower to void* */
        case TY_EXCEPTION:
            return "void *";
        /* Phase 18 / CC4 (cps-transform-plan): a continuation lowers to an
         * int64_t handle. (k v) application sugar resumes it via the cloneable
         * continuation runtime; the legacy fiber path refers to tur_cont*
         * through its own typedefs, not via this carrier name. */
        case TY_CONT:
            return "int64_t";
        case TY_CLONEABLE_CONT:
            return "tur_cloneable_cont *";
        /* Phase G0: ADT types are passed as int64_t (opaque heap pointer).
         * CONV-S1: a non-parametric flat-product ADT flows by value -- its C type
         * is the flat `tur_adt_<Name>` aggregate, not the int64 carrier.  Gated
         * by adt_is_byvalue_product (LIVE for leaf products as of B3). */
        case TY_ADT:
            /* SC7: a lowered transparent int-newtype ADT is just its int64
             * payload (mirrors the TY_STRUCT / TY_APP arms above). */
            if (type_is_transparent_int_newtype(t)) return "int64_t";
            /* opaque-pointer-c-spelling: an opaque newtype declared over a
             * pointer spells `void *`.  Asked before the by-value arm (which an
             * opaque never reaches -- adt_is_byvalue_product is false for
             * is_opaque) purely so the two opaque answers sit together. */
            if (adt_opaque_c_names_as_pointer(t.as.adt_.def)) return "void *";
            if (adt_is_byvalue_product(t.as.adt_.def)) {
                /* CONV-S1 seam 3: a :heap record ADT lowers to a typed pointer to
                 * its by-value header (the ADT analogue of a :heap struct's
                 * `Name *`). */
                if (t.as.adt_.def->is_heap)
                    return heap_ptr_c_name(adt_byval_c_name(t.as.adt_.def));
                return adt_byval_c_name(t.as.adt_.def);
            }
            return "int64_t";
        /* Phase HKT-P1: Type application — generic struct values with
         * field-level type variables lower to the same concrete C struct as
         * their head constructor; other applications stay opaque int64_t. */
        case TY_APP: {
            /* SC7: a transparent int newtype is just its int64 payload. */
            if (type_is_transparent_int_newtype(t)) return "int64_t";
            /* opaque-pointer-c-spelling: a PARAMETRIC opaque over a
             * pointer -- `(defopaque Goal [A] :ptr<void>)`, `(defopaque Parser
             * [A] :ptr<void>)`, `(defopaque SChan [P] :ptr<void>)` -- erases its
             * type arguments to the same handle, so `(Goal int)` must spell
             * exactly what bare `Goal` does or the two disagree at every
             * instantiation boundary. */
            {
                AdtDef *odef = type_adt_app_def(&t);
                if (adt_opaque_c_names_as_pointer(odef)) return "void *";
            }
            /* Parametric-by-value (gated): a concrete flat-product ADT-app flows
             * as its by-value monomorph aggregate (`tur_adt_Pair2__int__float`),
             * not the int64 carrier.  Hard-off until the crossings are wired. */
            /* SR3 slice B: a niche-filled `(Option P)` IS its payload -- the
             * C spelling is P's, there is no `tur_adt_Option__P` struct, and
             * `(none)` is that pointer type's null.  Asked BEFORE the by-value
             * arm below, which would otherwise mint the tagged monomorph name. */
            if (adt_app_is_niche_option(t)) {
                AdtDef *ndef = NULL;
                Type nargs[16];
                uint8_t nn = 0;
                Type nt = t;
                if (type_extract_adt_app(&nt, &ndef, nargs, &nn) && nn == 1)
                    return type_c_name(nargs[0]);
            }
            if (adt_app_is_byvalue_product(t)) {
                const char *nm = type_register_adt_app(t);
                if (nm) {
                    /* CONV-S1 seam 3: a :heap parametric record ADT monomorph
                     * (`(Vec int)`) lowers to a typed pointer `tur_adt_Vec__int *`
                     * to its heap header -- the ADT analogue of `Vec__int *`. */
                    AdtDef *adef = type_adt_app_def(&t);
                    if (adef && adef->is_heap) return heap_ptr_c_name(nm);
                    return nm;
                }
            }
            /* structdef-retirement DS-D: the former struct-app monomorph naming
             * branch (register_struct_app, gated on type_has_concrete_codegen_
             * layout of a struct-headed app) is dead -- no struct-headed app
             * forms.  Concrete parametric ADTs are named above via
             * type_register_adt_app; everything else is the int64 carrier. */
            return "int64_t";
        }
        /* Phase HKT-P2: Recursive types — opaque int64_t handle in v1 */
        case TY_REC:
            return "int64_t";
        /* Phase HRT0: Quantified types — no runtime representation in HRT0 */
        case TY_FORALL:
        case TY_EXISTS:
            return "void *";
        /* IT4: Union types — tagged union struct {int64_t tag; int64_t val} */
        case TY_UNION:
            return "tur_tagged_t";
        /* IT2: Intersection types — opaque int64_t placeholder (full codegen in IT4) */
        case TY_INTERSECTION:
            return "int64_t";
        /* ET3/FH1: Handler value — pointer to an effect-keyed dispatch table.
         * (Was the type-only tur_handler_t struct before first-class handlers;
         * handler values are now created/passed/applied as tur_handler_table_t*.) */
        case TY_HANDLER:
            return "tur_handler_table_t *";
        /* CT0: Contract type — same C representation as its base type */
        case TY_CONTRACT:
            return t.as.contract_.base_type
                   ? type_c_name(*t.as.contract_.base_type)
                   : "int64_t";
        /* SS0a/SS1: Session channel endpoints lower to void* in C until SS2
         * defines the TurChannel struct. Protocol descriptor types are erased
         * and never appear as C values. */
        /* Variadic HKT rows: compile-time only -- a row should be eliminated
         * before codegen (it only ever appears as a type argument). If one
         * reaches here it has no runtime representation; erase to void with a
         * marker, mirroring TY_GLOBAL / TY_DYNVAR. */
        case TY_TYPEROW:
            return "/*type-row*/ void";
    }
    return "void";
}

/* DV0: Dynamic var type constructor (-Xdynamic-vars). */
Type type_dynvar(Arena *a, Type value_type) {
    Type t;
    memset(&t, 0, sizeof(t));
    t.kind = TY_DYNVAR;
    t.copy_kind = CK_COPY;
    t.hkt_kind = KIND_STAR;
    t.as.dynvar_.value_type = (Type *)arena_alloc(a, sizeof(Type));
    *t.as.dynvar_.value_type = value_type;
    return t;
}

/* Phase HKT-P1: Type application constructor.
 * Creates a TY_APP type node with the given fn and arg types.
 * The result kind is computed using kind_of_type_app. */
Type type_app(Arena *a, Type fn, Type arg, Span span) {
    Type t;
    memset(&t, 0, sizeof(t));
    t.kind = TY_APP;
    t.copy_kind = CK_COPY;  /* TY_APP is an opaque handle, copy by value */
    t.n_lifetimes = 0;
    t.typeclass_instances = NULL;
    t.n_typeclass_instances = 0;

    propagate_app_discipline(&t, &fn);

    /* Allocate memory for fn and arg on the arena */
    t.as.app.fn = (Type *)arena_alloc(a, sizeof(Type));
    memcpy(t.as.app.fn, &fn, sizeof(Type));
    t.as.app.arg = (Type *)arena_alloc(a, sizeof(Type));
    memcpy(t.as.app.arg, &arg, sizeof(Type));

    /* Compute the result kind */
    t.hkt_kind = kind_of_type_app(fn, arg, span);

    return t;
}

/* constrained-hkt-abstract-var-requires-last-param-free: hole-headed partial
 * application.  Same shape as type_app, plus the free-slot index; the kind is
 * forced to `* -> *` because exactly one slot remains to be filled. */
Type type_app_hole(Arena *a, Type fn, Type arg, uint8_t hole_pos, Span span) {
    Type t = type_app(a, fn, arg, span);
    t.as.app.hole_pos_p1 = (uint8_t)(hole_pos + 1);
    t.hkt_kind = KIND_ARROW;
    return t;
}

/* Saturate a (possibly hole-headed) partial application with `elem`.  For a
 * hole at index h the result places `elem` at slot h and the already-fixed
 * argument in the remaining slot, so `(Result _ cstr)` + `int` yields
 * `(Result int cstr)` rather than the curried `(Result cstr int)`. */
Type type_app_fill_hole(Arena *a, Type head, Type elem, Span span) {
    if (!type_app_has_hole(&head)) return type_app(a, head, elem, span);
    uint8_t h = type_app_hole_pos(&head);
    Type ctor  = head.as.app.fn ? *head.as.app.fn : head;
    Type fixed = head.as.app.arg ? *head.as.app.arg : elem;
    /* Binary constructors only: the hole is slot 0 or slot 1, and the single
     * fixed argument takes the other.  (A wider wildcard head is rejected at
     * the instance-head parser, so no such Type is constructible today.) */
    Type first  = (h == 0) ? elem  : fixed;
    Type second = (h == 0) ? fixed : elem;
    Type inner = type_app(a, ctor, first, span);
    return type_app(a, inner, second, span);
}

/* Phase HKT-P2: One-step unrolling of a TY_REC type.
 * Returns the body of the recursive type (the type with the binder variable in scope).
 * In v1, simply returns the body pointer without substitution.
 * Returns NULL if t is not TY_REC or body is not yet evaluated. */
Type *type_rec_unfold(Type *t) {
    if (!t || t->kind != TY_REC) return NULL;
    return t->as.rec.body;
}

void type_print(Buf *b, Type t) {
    type_name_buf(b, t);
}

/* Phase HKT H0: Kind utilities */

bool kind_eq(Kind a, Kind b) {
    return a == b;
}

/* Phase HKT-P2: Check if a type body is guarded-recursive.
 * A recursive reference to `rec_name` is guarded if it appears under at least
 * one type constructor (TY_APP or TY_STRUCT). This prevents infinite types
 * like `(defrec X [] X)` while allowing `(defrec Fix [f] (Fix (f Fix)))`.
 *
 * `t`: the type body to check
 * `rec_name`: the name of the recursive type binder
 * `depth`: current nesting depth of type constructors (guard count)
 * Returns true if all occurrences of rec_name are guarded (depth >= 1),
 * or if there are no occurrences of rec_name at all.
 */
static bool type_is_guarded_recursive_helper(const Type *t, const char *rec_name, int depth) {
    if (!t) return true;

    switch (t->kind) {
        case TY_REC: {
            /* A nested TY_REC with a different name is fine.
             * Same name at depth 0 means unguarded recursion -> reject. */
            if (t->as.rec.name && strcmp(t->as.rec.name, rec_name) == 0) {
                return depth > 0;  /* Guarded iff under a type constructor */
            }
            /* Different name or no name - recurse into body */
            return type_is_guarded_recursive_helper(t->as.rec.body, rec_name, depth);
        }

        case TY_APP: {
            /* TY_APP is a type constructor - it guards recursion */
            int new_depth = depth + 1;
            /* Check both the function and argument types */
            if (!type_is_guarded_recursive_helper(t->as.app.fn, rec_name, new_depth))
                return false;
            if (!type_is_guarded_recursive_helper(t->as.app.arg, rec_name, new_depth))
                return false;
            return true;
        }

        case TY_STRUCT: {
            /* TY_STRUCT is a type constructor - it guards recursion */
            /* In v1, StructDef stores TypeKind, not full Type structs.
             * This is a limitation - we can only check TY_APP and TY_REC properly.
             * For v1, we assume struct fields are fine and just return true.
             * The recursion checking only works for TY_APP and TY_REC. */
            (void)depth;  /* Unused in this branch */
            return true;
        }

        case TY_FN: {
            /* Function types: in v1, TY_FN stores TypeKind arrays, not full Type structs */
            /* So we can't recursively check - this is a v1 limitation */
            (void)depth;  /* Unused in this branch */
            return true;
        }

        case TY_REF:
        case TY_LREF:
        case TY_RC:
        case TY_WEAK:
        case TY_REF_IMMUT:
        case TY_REF_MUT: {
            /* In v1, these store TypeKind not Type, so we can't recurse.
             * Just return true and assume no unguarded recursion. */
            (void)depth;  /* Unused in this branch */
            return true;
        }

        case TY_EXCEPTION:
        case TY_CONT:
        case TY_CLONEABLE_CONT: {
            /* In v1, they store TypeKind, not Type */
            (void)depth;  /* Unused in this branch */
            return true;
        }

        /* Phase HRT0: Quantified types guard recursion (the forall/exists is itself a constructor) */
        case TY_FORALL:
        case TY_EXISTS: {
            int new_depth = depth + 1;
            return type_is_guarded_recursive_helper(t->as.forall_.body, rec_name, new_depth);
        }

        /* Leaf types - no recursion possible */
        case TY_UNKNOWN:
        case TY_NIL:
        case TY_BOOL:
        case TY_INT:
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
        case TY_CSTR:
        case TY_PTR_VOID:
        case TY_TYPECLASS:
        case TY_TYPECLASS_INST:
        case TY_NEVER:
        case TY_SET:
        /* SYM0: symbols are opaque pointers — guard recursion like other leaves */
        case TY_SYM:
        /* Phase G0: ADT types guard recursion like structs */
        case TY_ADT:
        /* Phase G2: unresolved type variable — treated as opaque/guarded */
        case TY_TYVAR:
            return true;
        /* IT0: Union types — guard recursion like other type constructors */
        case TY_UNION: {
            int new_depth = depth + 1;
            for (uint8_t i = 0; i < t->as.union_.n_members; i++) {
                if (!type_is_guarded_recursive_helper(t->as.union_.members[i], rec_name, new_depth))
                    return false;
            }
            return true;
        }
        /* IT2: Intersection types — guard recursion like union types */
        case TY_INTERSECTION: {
            int new_depth = depth + 1;
            for (uint8_t i = 0; i < t->as.intersection_.n_members; i++) {
                if (!type_is_guarded_recursive_helper(t->as.intersection_.members[i], rec_name, new_depth))
                    return false;
            }
            return true;
        }
        /* IT4: any — top type; always safe (no recursive members) */
        case TY_ANY:
            return true;
        /* ET3: Handler type — leaf type; no recursive members */
        case TY_HANDLER:
            return true;
        /* CT0: Contract type — guarded by the base type constructor */
        case TY_CONTRACT:
            return type_is_guarded_recursive_helper(t->as.contract_.base_type, rec_name, depth + 1);
        /* SS0a: Session protocol types — guarded by their constructors.
         * TY_SESSION wraps the protocol (one constructor deep = depth+1 counts).
         * TY_CLOSE is a leaf; others have child protocol types. */
        case TY_CLOSE:
            return true;
        case TY_SESSION:
            return type_is_guarded_recursive_helper(t->as.session_.fst, rec_name, depth + 1);
        case TY_SEND:
        case TY_RECV:
        case TY_CHOOSE:
        case TY_BRANCH:
            return type_is_guarded_recursive_helper(t->as.session_.fst, rec_name, depth + 1)
                && (!t->as.session_.snd
                    || type_is_guarded_recursive_helper(t->as.session_.snd, rec_name, depth + 1));
        case TY_SESSION_REC:
            return type_is_guarded_recursive_helper(t->as.session_.fst, rec_name, depth + 1);
        case TY_TIMEOUT:
            return type_is_guarded_recursive_helper(t->as.session_.fst, rec_name, depth + 1)
                && (!t->as.session_.snd
                    || type_is_guarded_recursive_helper(t->as.session_.snd, rec_name, depth + 1));
        case TY_SESSION_PAIR:
        case TY_SESSION_RECV_PAIR:
        case TY_SESSION_OFFER:
            return type_is_guarded_recursive_helper(t->as.session_.fst, rec_name, depth + 1)
                && (!t->as.session_.snd
                    || type_is_guarded_recursive_helper(t->as.session_.snd, rec_name, depth + 1));
        /* SS5: Global protocol types -- never recursive in the TY_REC sense */
        case TY_GLOBAL:
        case TY_ROLE:
            return true;
        /* DV0: Dynamic var — leaf; no recursive members */
        case TY_DYNVAR:
            return true;
        /* GF1: Generator -- heap pointer, leaf; no recursive members */
        case TY_GENERATOR:
            return true;
        /* Stage 1: syntax objects are leaves; no recursive members. */
        case TY_SYNTAX:
            return true;
        /* Variadic HKT rows -- the row is a constructor over its elements;
         * guard recursion like a union and recurse into each element. */
        case TY_TYPEROW: {
            int new_depth = depth + 1;
            for (uint8_t i = 0; i < t->as.typerow_.n_elements; i++) {
                if (!type_is_guarded_recursive_helper(t->as.typerow_.elements[i], rec_name, new_depth))
                    return false;
            }
            return true;
        }
    }

    return true;  /* Unknown type kind - assume safe */
}

bool type_is_guarded_recursive(Type t, const char *rec_name) {
    /* Start with depth 0 - we need at least one type constructor
     * (TY_APP or TY_STRUCT) before we hit the recursive reference */
    return type_is_guarded_recursive_helper(&t, rec_name, 0);
}

const char *kind_to_string(Kind k) {
    if (k == KIND_ROW) return "Row";
    if (k == KIND_TYPEROW) return "[*]";   /* kind-level List Type */
    /* Zero-alloc table for arities 0..15 (covers all practical usage). */
    static const char * const tbl[16] = {
        "*",
        "* -> *",
        "* -> * -> *",
        "* -> * -> * -> *",
        "* -> * -> * -> * -> *",
        "* -> * -> * -> * -> * -> *",
        "* -> * -> * -> * -> * -> * -> *",
        "* -> * -> * -> * -> * -> * -> * -> *",
        "* -> * -> * -> * -> * -> * -> * -> * -> *",
        "* -> * -> * -> * -> * -> * -> * -> * -> * -> *",
        "* -> * -> * -> * -> * -> * -> * -> * -> * -> * -> *",
        "* -> * -> * -> * -> * -> * -> * -> * -> * -> * -> * -> *",
        "* -> * -> * -> * -> * -> * -> * -> * -> * -> * -> * -> * -> *",
        "* -> * -> * -> * -> * -> * -> * -> * -> * -> * -> * -> * -> * -> *",
        "* -> * -> * -> * -> * -> * -> * -> * -> * -> * -> * -> * -> * -> * -> *",
        "* -> * -> * -> * -> * -> * -> * -> * -> * -> * -> * -> * -> * -> * -> * -> *",
    };
    if (k < 16) return tbl[k];
    /* Diagnostic fallback for very high arities (not reentrant; only for errors). */
    static char fallback[64];
    snprintf(fallback, sizeof(fallback), "* (arity %u)", (unsigned)k);
    return fallback;
}

Kind kind_parse(const char *s) {
    if (!s) return KIND_STAR;
    /* Kind-level List Type: "[*]" (round-trips kind_to_string(KIND_TYPEROW)). */
    if (s[0] == '[' && s[1] == '*' && s[2] == ']' && s[3] == '\0') return KIND_TYPEROW;
    /* Count " -> *" suffixes after the leading "*". An arity-N constructor
     * has N occurrences of " -> *". */
    if (s[0] != '*') return KIND_STAR;
    uint32_t arrows = 0;
    const char *p = s + 1;
    while (p[0] == ' ' && p[1] == '-' && p[2] == '>' && p[3] == ' ' && p[4] == '*') {
        arrows++;
        p += 5;
    }
    if (*p != '\0') return KIND_STAR;
    return kind_for_arity(arrows);
}

Kind kind_for_arity(uint32_t n) {
    return (Kind)n;
}

Kind kind_apply_one(Kind k) {
    /* STAR/TYPEROW/ROW are not arrow kinds -- applying an argument to them is
     * ill-kinded; return them unchanged so the caller can validate/diagnose. */
    if (k == KIND_STAR || k == KIND_TYPEROW || k == KIND_ROW) return k;
    return (Kind)(k - 1);
}

/* ===== Phase SZ6/SZ7: type-level size index terms ===== */

static SizeTerm *size_term_alloc(Arena *a, SizeTermKind kind) {
    SizeTerm *t = (SizeTerm *)arena_alloc(a, sizeof(SizeTerm));
    t->kind  = kind;
    t->konst = 0;
    t->var   = NULL;
    t->lhs   = NULL;
    t->rhs   = NULL;
    return t;
}

SizeTerm *size_term_from_form(Arena *a, const Form *f,
                              bool (*is_size_var)(const char *, void *),
                              void *ctx) {
    if (!f) return NULL;
    /* A bare integer literal stands for that constant size (handy for nested
     * (Static <int>) and for direct numeric indices). */
    if (f->tag == F_INT) {
        SizeTerm *t = size_term_alloc(a, SZT_CONST);
        t->konst = f->as.i;
        return t;
    }
    if (f->tag == F_SYM) {
        const char *nm = f->as.sym->name;
        if (is_size_var && !is_size_var(nm, ctx)) return NULL;
        SizeTerm *t = size_term_alloc(a, SZT_VAR);
        t->var = nm;
        return t;
    }
    if (f->tag == F_LIST && f->as.list.len >= 1) {
        const Form *hd = f->as.list.items[0];
        if (hd->tag != F_SYM) return NULL;
        const char *op = hd->as.sym->name;
        /* Accept both the type-level GADT constructor spelling (Static/Add/Mul)
         * and the value-level stdlib spelling (size-static/size-add/size-mul),
         * so the same parser serves SZ6 (type indices) and SZ7 (value-level
         * size-assert-eq! arguments). */
        bool is_static = (strcmp(op, "Static") == 0 || strcmp(op, "size-static") == 0);
        bool is_add    = (strcmp(op, "Add")    == 0 || strcmp(op, "size-add")    == 0);
        bool is_mul    = (strcmp(op, "Mul")    == 0 || strcmp(op, "size-mul")    == 0);
        if (is_static && f->as.list.len == 2) {
            const Form *arg = f->as.list.items[1];
            if (arg->tag != F_INT) return NULL;
            SizeTerm *t = size_term_alloc(a, SZT_CONST);
            t->konst = arg->as.i;
            return t;
        }
        if ((is_add || is_mul) && f->as.list.len == 3) {
            SizeTerm *l = size_term_from_form(a, f->as.list.items[1], is_size_var, ctx);
            SizeTerm *r = size_term_from_form(a, f->as.list.items[2], is_size_var, ctx);
            if (!l || !r) return NULL;
            SizeTerm *t = size_term_alloc(a, is_add ? SZT_ADD : SZT_MUL);
            t->lhs = l;
            t->rhs = r;
            return t;
        }
        return NULL;
    }
    return NULL;
}

SizeTerm *size_term_subst(Arena *a, const SizeTerm *t,
                          const char *var, const SizeTerm *replacement) {
    if (!t) return NULL;
    switch (t->kind) {
        case SZT_CONST: {
            SizeTerm *c = size_term_alloc(a, SZT_CONST);
            c->konst = t->konst;
            return c;
        }
        case SZT_VAR:
            if (var && t->var && strcmp(t->var, var) == 0)
                return size_term_subst(a, replacement, NULL, NULL);
            else {
                SizeTerm *v = size_term_alloc(a, SZT_VAR);
                v->var = t->var;
                return v;
            }
        case SZT_ADD:
        case SZT_MUL: {
            SizeTerm *n = size_term_alloc(a, t->kind);
            n->lhs = size_term_subst(a, t->lhs, var, replacement);
            n->rhs = size_term_subst(a, t->rhs, var, replacement);
            return n;
        }
    }
    return NULL;
}

bool size_term_eval(const SizeTerm *t, int64_t *out) {
    if (!t) return false;
    switch (t->kind) {
        case SZT_CONST:
            if (out) *out = t->konst;
            return true;
        case SZT_VAR:
            return false;
        case SZT_ADD: {
            int64_t l, r;
            if (!size_term_eval(t->lhs, &l) || !size_term_eval(t->rhs, &r)) return false;
            if (out) *out = l + r;
            return true;
        }
        case SZT_MUL: {
            int64_t l, r;
            if (!size_term_eval(t->lhs, &l) || !size_term_eval(t->rhs, &r)) return false;
            if (out) *out = l * r;
            return true;
        }
    }
    return false;
}

/* Syntactic equality after light normalisation (used as a fallback when a term
 * is not closed).  Handles commutativity of Add/Mul and identity elements. */
static bool size_term_syntactic_eq(const SizeTerm *a, const SizeTerm *b) {
    if (a == b) return true;
    if (!a || !b) return false;
    if (a->kind != b->kind) return false;
    switch (a->kind) {
        case SZT_CONST: return a->konst == b->konst;
        case SZT_VAR:   return a->var && b->var && strcmp(a->var, b->var) == 0;
        case SZT_ADD:
        case SZT_MUL:
            /* commutative: (op x y) == (op y x) */
            return (size_term_syntactic_eq(a->lhs, b->lhs) &&
                    size_term_syntactic_eq(a->rhs, b->rhs)) ||
                   (size_term_syntactic_eq(a->lhs, b->rhs) &&
                    size_term_syntactic_eq(a->rhs, b->lhs));
    }
    return false;
}

bool size_term_equal(const SizeTerm *a, const SizeTerm *b) {
    if (!a || !b) return false;
    int64_t va, vb;
    bool ca = size_term_eval(a, &va);
    bool cb = size_term_eval(b, &vb);
    if (ca && cb) return va == vb;        /* both closed: decide by value */
    if (ca != cb) return false;           /* one closed, one open: not provably equal */
    return size_term_syntactic_eq(a, b);  /* both open: syntactic fallback */
}

static int size_term_to_string_rec(const SizeTerm *t, char *buf, size_t cap, int pos) {
    if (!t || pos < 0 || (size_t)pos >= cap) return pos;
    switch (t->kind) {
        case SZT_CONST:
            pos += snprintf(buf + pos, cap - (size_t)pos, "%lld", (long long)t->konst);
            return pos;
        case SZT_VAR:
            pos += snprintf(buf + pos, cap - (size_t)pos, "%s", t->var ? t->var : "?");
            return pos;
        case SZT_ADD:
        case SZT_MUL:
            pos += snprintf(buf + pos, cap - (size_t)pos, "(%s ",
                            t->kind == SZT_ADD ? "+" : "*");
            pos = size_term_to_string_rec(t->lhs, buf, cap, pos);
            if (pos < (int)cap) pos += snprintf(buf + pos, cap - (size_t)pos, " ");
            pos = size_term_to_string_rec(t->rhs, buf, cap, pos);
            if (pos < (int)cap) pos += snprintf(buf + pos, cap - (size_t)pos, ")");
            return pos;
    }
    return pos;
}

const char *size_term_to_string(const SizeTerm *t, char *buf, size_t cap) {
    if (!buf || cap == 0) return "";
    buf[0] = '\0';
    size_term_to_string_rec(t, buf, cap, 0);
    return buf;
}

const char *typekind_to_string(TypeKind k) {
    switch (k) {
        case TY_UNKNOWN:   return "unknown";
        case TY_NIL:      return "nil";
        case TY_BOOL:     return "bool";
        case TY_INT:      return "int";
        case TY_FLOAT:    return "float";
        case TY_INT8:     return "int8";
        case TY_INT16:    return "int16";
        case TY_INT32:    return "int32";
        case TY_INT64:    return "int64";
        case TY_UINT8:    return "uint8";
        case TY_UINT16:   return "uint16";
        case TY_UINT32:   return "uint32";
        case TY_UINT64:   return "uint64";
        case TY_FLOAT32:  return "float32";
        case TY_FLOAT64:  return "float64";
        case TY_CSTR:     return "cstr";
        case TY_PTR_VOID: return "ptr-void";
        case TY_FN:       return "fn";
        case TY_REF:      return "ref";
        case TY_RC:       return "rc";
        case TY_WEAK:     return "weak";
        case TY_REF_IMMUT: return "&immut";
        case TY_REF_MUT:  return "&mut";
        case TY_TYPECLASS: return "typeclass";
        case TY_TYPECLASS_INST: return "typeclass-inst";
        case TY_EXCEPTION: return "exception";
        case TY_CONT:     return "cont";
        case TY_CLONEABLE_CONT: return "cloneable_cont";
        case TY_STRUCT:   return "struct";
        case TY_ADT:      return "adt";
        case TY_NEVER:    return "!";
        case TY_SET:      return "set";
        case TY_SYM:      return "Sym";
        /* Phase HRT0 */
        case TY_FORALL:   return "forall";
        case TY_EXISTS:   return "exists";
        /* Phase G2 */
        case TY_TYVAR:    return "tyvar";
        /* IT0: Union types */
        case TY_UNION:        return "union";
        /* IT2: Intersection types */
        case TY_INTERSECTION: return "intersection";
        /* IT4: Top type */
        case TY_ANY:          return "any";
        /* ET3: Handler type */
        case TY_HANDLER:      return "handler";
        /* CT0: Contract type */
        case TY_CONTRACT:     return "contract";
        /* Phase HKT-P1/P2 */
        case TY_APP:          return "app";
        case TY_REC:          return "rec";
        /* Phase LT3 */
        case TY_LREF:         return "lref";
        /* SS0a: Session protocol types */
        case TY_SESSION:      return "Session";
        case TY_SEND:         return "Send";
        case TY_RECV:         return "Recv";
        case TY_CLOSE:        return "Close";
        case TY_CHOOSE:       return "Choose";
        case TY_BRANCH:       return "Branch";
        case TY_SESSION_REC:       return "Rec";
        case TY_TIMEOUT:           return "Timeout";
        case TY_SESSION_PAIR:      return "SessionPair";
        case TY_SESSION_RECV_PAIR: return "RecvPair";
        case TY_SESSION_OFFER:     return "Offer";
        /* SS5: Global protocol types */
        case TY_GLOBAL:            return "Global";
        case TY_ROLE:              return "Role";
        default:          return "<?>";
    }
}

/* TS4P1: Reset the ADT-app registry (called before each compilation unit). */
void type_codegen_reset_adt_apps(void) {
    for (uint32_t i = 0; i < g_n_adt_apps; i++) {
        free_struct_app_type(g_adt_apps[i].type);
        free(g_adt_apps[i].name);
    }
    free(g_adt_apps);
    g_adt_apps     = NULL;
    g_n_adt_apps   = 0;
    g_cap_adt_apps = 0;
}

/* TS4P1: Emit all registered ADT-app typedefs and constructor functions. */
void type_codegen_emit_adt_apps(Buf *out) {
    for (uint32_t i = 0; i < g_n_adt_apps; i++) {
        emit_registered_adt_app_rec(out, i);
    }
}

/* Phase E: Reset the fn-ptr typedef registry (called before each compilation unit). */
void type_codegen_reset_fn_ptr_typedefs(void) {
    for (uint32_t i = 0; i < g_n_fn_ptr_typedefs; i++)
        free(g_fn_ptr_typedefs[i].typedef_name);
    free(g_fn_ptr_typedefs);
    g_fn_ptr_typedefs     = NULL;
    g_n_fn_ptr_typedefs   = 0;
    g_cap_fn_ptr_typedefs = 0;
}

/* Phase E: Emit all registered fn-ptr typedefs.
 * Must be called before any struct typedef that uses them as field types.
 *
 * c-fn-ptr-element-and-size-precision-gap fix: when an entry carries a
 * non-NULL arg_full_types[j] (populated for cfnptrs in
 * register_fn_ptr_typedef), emit its precise C type rather than the
 * lossy carrier from type_from_kind(arg_kinds[j]).  Lets a (c-fn
 * [ptr<u8>] void) lower to `void (*)(uint8_t *)` instead of
 * `void (*)(void *)`. */
/* c-fn-ptr-element-and-size-precision-gap fix: true if a Type lowers to a C
 * name declared in <stddef.h> (size_t / ptrdiff_t) -- directly or through a
 * pointee. */
static bool type_needs_stddef(const Type *t) {
    if (!t) return false;
    if (t->c_num_spelling == CNUM_SIZE_T || t->c_num_spelling == CNUM_PTRDIFF_T)
        return true;
    if (t->kind == TY_PTR_VOID && t->as.ptr.inner)
        return type_needs_stddef(t->as.ptr.inner);
    return false;
}

void type_codegen_emit_fn_ptr_typedefs(Buf *out) {
    /* Emit <stddef.h> once if any not-yet-emitted typedef references size_t /
     * ptrdiff_t.  These spellings only appear with the new :usize / :isize
     * cfnptr surface, so this never fires for pre-existing fixtures -- no
     * snapshot churn -- and it keeps the size types self-contained instead of
     * adding the header to the shared preamble (which would churn every
     * snapshot).  A mid-file include is legal at file scope and idempotent. */
    for (uint32_t i = 0; i < g_n_fn_ptr_typedefs; i++) {
        RegisteredFnPtrTypedef *entry = &g_fn_ptr_typedefs[i];
        if (entry->emitted) continue;
        bool needs = type_needs_stddef(entry->result_full_type);
        for (uint8_t j = 0; !needs && j < entry->arity; j++)
            needs = type_needs_stddef(entry->arg_full_types[j]);
        if (needs) { buf_puts(out, "#include <stddef.h>\n"); break; }
    }
    for (uint32_t i = 0; i < g_n_fn_ptr_typedefs; i++) {
        RegisteredFnPtrTypedef *entry = &g_fn_ptr_typedefs[i];
        if (entry->emitted) continue;
        entry->emitted = true;
        const char *ret_c = entry->result_full_type
            ? type_c_name(*entry->result_full_type)
            : type_c_name(type_from_kind(entry->result_kind));
        buf_printf(out, "typedef %s (*%s)(", ret_c, entry->typedef_name);
        if (entry->arity == 0) {
            buf_puts(out, "void");
        } else {
            for (uint8_t j = 0; j < entry->arity; j++) {
                if (j > 0) buf_puts(out, ", ");
                const char *arg_c;
                if (entry->arg_full_types[j]) {
                    arg_c = type_c_name(*entry->arg_full_types[j]);
                } else {
                    arg_c = type_c_name(type_from_kind(entry->arg_kinds[j]));
                }
                buf_puts(out, arg_c);
            }
        }
        buf_puts(out, ");\n");
    }
}

/* Phase HRT0: Compute the rank of a type.
 * Rank 0 = monotype (no quantifiers).
 * Rank 1 = forall/exists at the outermost level.
 * Rank N = forall/exists nested N levels deep (each level adds 1).
 * Note: full rank computation requires Type* args in TY_FN (deferred to HRT1).
 */
int type_rank(const Type *t) {
    if (!t) return 0;
    switch (t->kind) {
        case TY_FORALL:
        case TY_EXISTS: {
            int body_rank = type_rank(t->as.forall_.body);
            return body_rank + 1;
        }
        default:
            return 0;
    }
}

/* IT0: Union type constructor.
 * Builds a TY_UNION type from an array of member types.
 * Nested TY_UNION members are flattened: (A | (B | C)) -> (A | B | C).
 * IT4: If any member is TY_ANY, the union simplifies to any.
 * The members array and its contents are allocated on the given arena. */
Type type_union_build(Arena *a, Type **members, uint8_t n_members) {
    /* IT4: If any member is TY_ANY, the whole union is any */
    for (uint8_t i = 0; i < n_members; i++) {
        if (members[i] && members[i]->kind == TY_ANY) {
            Type t;
            memset(&t, 0, sizeof(t));
            t.kind = TY_ANY;
            t.copy_kind = CK_COPY;
            t.hkt_kind = KIND_STAR;
            return t;
        }
    }

    /* First, compute flattened count */
    uint8_t flat_count = 0;
    for (uint8_t i = 0; i < n_members; i++) {
        if (members[i] && members[i]->kind == TY_UNION) {
            flat_count += members[i]->as.union_.n_members;
        } else {
            flat_count++;
        }
    }

    /* Allocate flattened array */
    Type **flat = (Type **)arena_alloc(a, flat_count * sizeof(Type *));
    uint8_t fi = 0;
    for (uint8_t i = 0; i < n_members; i++) {
        if (members[i] && members[i]->kind == TY_UNION) {
            /* Flatten nested union */
            for (uint8_t j = 0; j < members[i]->as.union_.n_members; j++) {
                flat[fi++] = members[i]->as.union_.members[j];
            }
        } else {
            flat[fi++] = members[i];
        }
    }

    Type t;
    memset(&t, 0, sizeof(t));
    t.kind = TY_UNION;
    t.copy_kind = CK_MOVE;
    t.hkt_kind = KIND_STAR;
    t.as.union_.members = flat;
    t.as.union_.n_members = flat_count;
    return t;
}

/* IT2: Intersection type constructor.
 * Builds a TY_INTERSECTION type from an array of member types.
 * Nested TY_INTERSECTION members are flattened: (A & (B & C)) -> (A & B & C).
 * The members array and its contents are allocated on the given arena. */
Type type_intersection_build(Arena *a, Type **members, uint8_t n_members) {
    /* First, compute flattened count */
    uint8_t flat_count = 0;
    for (uint8_t i = 0; i < n_members; i++) {
        if (members[i] && members[i]->kind == TY_INTERSECTION) {
            flat_count += members[i]->as.intersection_.n_members;
        } else {
            flat_count++;
        }
    }

    /* Allocate flattened array */
    Type **flat = (Type **)arena_alloc(a, flat_count * sizeof(Type *));
    uint8_t fi = 0;
    for (uint8_t i = 0; i < n_members; i++) {
        if (members[i] && members[i]->kind == TY_INTERSECTION) {
            /* Flatten nested intersection */
            for (uint8_t j = 0; j < members[i]->as.intersection_.n_members; j++) {
                flat[fi++] = members[i]->as.intersection_.members[j];
            }
        } else {
            flat[fi++] = members[i];
        }
    }

    Type t;
    memset(&t, 0, sizeof(t));
    t.kind = TY_INTERSECTION;
    t.copy_kind = CK_MOVE;
    t.hkt_kind = KIND_STAR;
    t.as.intersection_.members = flat;
    t.as.intersection_.n_members = flat_count;
    return t;
}

/* Variadic HKT rows (Layer 2): row-of-types constructor.
 * Unlike unions/intersections, a row is an ordered list: element order is
 * significant, duplicates are preserved, and nested rows are NOT flattened. */
Type type_typerow(Arena *a, Type **elements, uint8_t n_elements) {
    Type **copy = NULL;
    if (n_elements > 0) {
        copy = (Type **)arena_alloc(a, n_elements * sizeof(Type *));
        for (uint8_t i = 0; i < n_elements; i++) copy[i] = elements[i];
    }
    Type t;
    memset(&t, 0, sizeof(t));
    t.kind = TY_TYPEROW;
    t.copy_kind = CK_COPY;          /* compile-time only -- copy/move is moot */
    t.hkt_kind = KIND_TYPEROW;
    t.as.typerow_.elements = copy;
    t.as.typerow_.n_elements = n_elements;
    t.as.typerow_.field_names = NULL;
    return t;
}

/* P0 typed-field row constructor. `field_names[i]` is the field name for
 * `elements[i]`; pass NULL to fall back to the bare-positional form. */
Type type_typerow_named(Arena *a, Type **elements, const char **field_names,
                       uint8_t n_elements) {
    Type t = type_typerow(a, elements, n_elements);
    if (field_names && n_elements > 0) {
        const char **ncopy = (const char **)arena_alloc(
            a, n_elements * sizeof(const char *));
        for (uint8_t i = 0; i < n_elements; i++) ncopy[i] = field_names[i];
        t.as.typerow_.field_names = ncopy;
    }
    return t;
}

/* Variadic HKT rows: order-insensitive (multiset) row equality.
 * Greedy bipartite match: for each element of `a`, consume the first
 * not-yet-consumed type_eq-equal element of `b`. Because type_eq is an
 * equivalence relation, greedy matching is complete here -- if every `a`
 * element finds a fresh partner, the multisets are equal. */
bool type_typerow_eq_perm(Type a, Type b) {
    if (a.kind != TY_TYPEROW || b.kind != TY_TYPEROW) return false;
    uint8_t n = a.as.typerow_.n_elements;
    if (n != b.as.typerow_.n_elements) return false;
    if (n == 0) return true;
    /* P0: rows with vs without field names are distinct; rows with names
     * must match each (name, type) pair in some permutation. */
    const char **an = a.as.typerow_.field_names;
    const char **bn = b.as.typerow_.field_names;
    if ((an == NULL) != (bn == NULL)) return false;
    /* Track which b-elements have already been matched. n <= 255 (uint8_t). */
    bool used[256];
    for (uint8_t j = 0; j < n; j++) used[j] = false;
    for (uint8_t i = 0; i < n; i++) {
        Type *ai = a.as.typerow_.elements[i];
        bool matched = false;
        for (uint8_t j = 0; j < n; j++) {
            if (used[j]) continue;
            Type *bj = b.as.typerow_.elements[j];
            bool eq = (!ai || !bj) ? (ai == bj) : type_eq(*ai, *bj);
            if (!eq) continue;
            if (an && bn) {
                const char *na = an[i], *nb = bn[j];
                bool name_eq = (!na || !nb) ? (na == nb) : strcmp(na, nb) == 0;
                if (!name_eq) continue;
            }
            used[j] = true; matched = true; break;
        }
        if (!matched) return false;
    }
    return true;
}

/* Variadic HKT rows (Layer 5): row algebra.
 *
 * These are pure compile-time operations on TY_TYPEROW values -- the
 * primitives the ECS query / relational layers build on (membership for
 * `with`/`without` filters, union/concat for query joins, intersection for
 * "entities matching both"). A non-row argument yields the empty row /
 * `false`, so callers can pass through unchecked. */

/* row-ops-drop-field-names: label plumbing for the row algebra.
 *
 * A labeled row (`#row{id : int}`) carries a parallel `field_names` array. Every
 * operation below has to forward it, or the result silently degrades to a
 * POSITIONAL row -- and a positional row compares equal to any other positional
 * row with the same element types, so `#row{id : int}` and `#row{name : int}`
 * start unifying the moment either passes through the algebra.
 *
 * An EMPTY row is label-NEUTRAL, not "bare". `(row-union R #row{})` is the
 * identity and must stay legal whether or not R is labeled, so emptiness never
 * counts as a bare operand for the all-or-nothing mixing rule. */
bool type_typerow_is_labeled(Type r) {
    return r.kind == TY_TYPEROW && r.as.typerow_.n_elements > 0 &&
           r.as.typerow_.field_names != NULL;
}

/* Field name of slot `i`, or NULL for a positional row. */
static const char *row_name_at(Type r, uint32_t i) {
    if (r.kind != TY_TYPEROW || !r.as.typerow_.field_names) return NULL;
    return r.as.typerow_.field_names[i];
}

static bool row_name_eq(const char *x, const char *y) {
    return (!x || !y) ? (x == y) : strcmp(x, y) == 0;
}

const char *type_typerow_dup_field_name(Type r) {
    if (!type_typerow_is_labeled(r)) return NULL;
    uint32_t n = r.as.typerow_.n_elements;
    for (uint32_t i = 0; i < n; i++) {
        const char *ni = r.as.typerow_.field_names[i];
        if (!ni) continue;
        for (uint32_t j = 0; j < i; j++)
            if (row_name_eq(r.as.typerow_.field_names[j], ni)) return ni;
    }
    return NULL;
}

/* True if `row` contains an element type_eq to `elem`. */
bool type_typerow_contains(Type row, Type elem) {
    if (row.kind != TY_TYPEROW) return false;
    for (uint8_t i = 0; i < row.as.typerow_.n_elements; i++) {
        Type *e = row.as.typerow_.elements[i];
        if (e && type_eq(*e, elem)) return true;
    }
    return false;
}

/* Concatenate two rows: x's elements then y's, order-preserving, duplicates
 * kept (`++` / list semantics). The combined length is clamped to 255. */
Type type_typerow_concat(Arena *a, Type x, Type y) {
    uint32_t nx = (x.kind == TY_TYPEROW) ? x.as.typerow_.n_elements : 0;
    uint32_t ny = (y.kind == TY_TYPEROW) ? y.as.typerow_.n_elements : 0;
    uint32_t n = nx + ny;
    if (n > 255) n = 255;
    Type **elems = (n > 0) ? (Type **)arena_alloc(a, n * sizeof(Type *)) : NULL;
    /* Labels ride along with their slots. The caller rejects a mixed
     * labeled/bare pair, so "either side is labeled" means "both are" (modulo
     * an empty operand, which contributes no slots). */
    bool labeled = type_typerow_is_labeled(x) || type_typerow_is_labeled(y);
    const char **names =
        (labeled && n > 0) ? (const char **)arena_alloc(a, n * sizeof(const char *)) : NULL;
    uint32_t k = 0;
    for (uint32_t i = 0; i < nx && k < n; i++) {
        if (names) names[k] = row_name_at(x, i);
        elems[k++] = x.as.typerow_.elements[i];
    }
    for (uint32_t i = 0; i < ny && k < n; i++) {
        if (names) names[k] = row_name_at(y, i);
        elems[k++] = y.as.typerow_.elements[i];
    }
    return type_typerow_named(a, elems, names, (uint8_t)k);
}

/* Append `el` to `acc` (an array of n Type*) iff no existing entry is type_eq
 * to it. Returns the new count. Used by union/intersect for dedup. */
static uint32_t row_push_unique(Type **acc, const char **acc_names, uint32_t n,
                                Type *el, const char *name) {
    if (!el) return n;
    for (uint32_t i = 0; i < n; i++) {
        if (!acc[i] || !type_eq(*acc[i], *el)) continue;
        /* Labeled rows dedup on the (name, type) PAIR: two slots are the same
         * slot only if they agree on both. Same name with a different type is
         * therefore kept, and the caller's duplicate-name scan turns it into
         * TUR-E0291 rather than silently dropping one of the two. */
        if (acc_names && !row_name_eq(acc_names[i], name)) continue;
        return n;
    }
    acc[n] = el;
    if (acc_names) acc_names[n] = name;
    return n + 1;
}

/* Set-union of two rows: x's elements (deduplicated) in order, then y's
 * elements not already present (by type_eq). Order-preserving, no duplicates
 * -- the join of two component rows. */
Type type_typerow_union(Arena *a, Type x, Type y) {
    uint32_t nx = (x.kind == TY_TYPEROW) ? x.as.typerow_.n_elements : 0;
    uint32_t ny = (y.kind == TY_TYPEROW) ? y.as.typerow_.n_elements : 0;
    uint32_t cap = nx + ny;
    Type **elems = (cap > 0) ? (Type **)arena_alloc(a, cap * sizeof(Type *)) : NULL;
    bool labeled = type_typerow_is_labeled(x) || type_typerow_is_labeled(y);
    const char **names =
        (labeled && cap > 0) ? (const char **)arena_alloc(a, cap * sizeof(const char *)) : NULL;
    uint32_t n = 0;
    for (uint32_t i = 0; i < nx; i++)
        n = row_push_unique(elems, names, n, x.as.typerow_.elements[i], row_name_at(x, i));
    for (uint32_t i = 0; i < ny; i++)
        n = row_push_unique(elems, names, n, y.as.typerow_.elements[i], row_name_at(y, i));
    if (n > 255) n = 255;
    return type_typerow_named(a, elems, names, (uint8_t)n);
}

/* L6 follow-up D: canonical (sorted) copy of a row, the opt-in surface for
 * permutation-aware row equality. Two rows that differ only by element order
 * canonicalise to identical TY_TYPEROW values, so ordinary order-sensitive
 * type_eq returns true. Sort key is type_name (compile-time only -- rows
 * erase at codegen, so the cost is paid once during elaboration).
 *
 * The L5 helper type_typerow_eq_perm remains the direct way to ask
 * "are these rows equal up to permutation?" without rebuilding either side;
 * row-canon is the opt-in users reach for at the type-annotation boundary
 * when they want the equality to flow through ordinary type_eq (signature
 * unification, function-argument compatibility, etc.). Both routes are
 * order-insensitive and consistent with each other. */
Type type_typerow_canonical(Arena *a, Type x) {
    if (x.kind != TY_TYPEROW) return type_typerow(a, NULL, 0);
    uint32_t n = x.as.typerow_.n_elements;
    if (n == 0) return type_typerow(a, NULL, 0);
    bool labeled = type_typerow_is_labeled(x);
    Type **elems = (Type **)arena_alloc(a, n * sizeof(Type *));
    const char **names =
        labeled ? (const char **)arena_alloc(a, n * sizeof(const char *)) : NULL;
    for (uint32_t i = 0; i < n; i++) {
        elems[i] = x.as.typerow_.elements[i];
        if (names) names[i] = row_name_at(x, i);
    }
    /* Insertion sort -- stable, fine for the row sizes in practice (<= 255,
     * almost always single digits). Labels are permuted alongside their slots.
     *
     * The sort key is (field_name, type_name) for a labeled row, type_name
     * alone for a positional one. Field name has to lead: labeled rows are
     * equal up to permutation of (name, type) pairs, so `#row{a : int  b : int}`
     * and `#row{b : int  a : int}` must canonicalise identically -- and on
     * type_name alone they compare equal at every slot, leaving the stable sort
     * to preserve two *different* input orders. Field names are unique within a
     * row (TUR-E0291), so the key is a total order. */
    for (uint32_t i = 1; i < n; i++) {
        Type *cur = elems[i];
        const char *cur_field = names ? names[i] : NULL;
        const char *cur_name = cur ? type_name(*cur) : "";
        uint32_t j = i;
        while (j > 0) {
            Type *prev = elems[j - 1];
            const char *prev_field = names ? names[j - 1] : NULL;
            const char *prev_name = prev ? type_name(*prev) : "";
            int c = 0;
            if (names)
                c = strcmp(prev_field ? prev_field : "", cur_field ? cur_field : "");
            if (c == 0) c = strcmp(prev_name, cur_name);
            if (c <= 0) break;
            elems[j] = prev;
            if (names) names[j] = prev_field;
            j--;
        }
        elems[j] = cur;
        if (names) names[j] = cur_field;
    }
    return type_typerow_named(a, elems, names, (uint8_t)n);
}

/* Intersection: x's elements that also appear in y (by type_eq), in x's order,
 * deduplicated -- the components common to both queries. */
Type type_typerow_intersect(Arena *a, Type x, Type y) {
    if (x.kind != TY_TYPEROW || y.kind != TY_TYPEROW) {
        return type_typerow(a, NULL, 0);
    }
    uint32_t nx = x.as.typerow_.n_elements;
    Type **elems = (nx > 0) ? (Type **)arena_alloc(a, nx * sizeof(Type *)) : NULL;
    /* An intersection can only narrow x, so the result is labeled exactly when
     * x is; the surviving slots keep x's names. */
    bool labeled = type_typerow_is_labeled(x);
    const char **names =
        (labeled && nx > 0) ? (const char **)arena_alloc(a, nx * sizeof(const char *)) : NULL;
    uint32_t n = 0;
    for (uint32_t i = 0; i < nx; i++) {
        Type *e = x.as.typerow_.elements[i];
        if (!e) continue;
        const char *xn = row_name_at(x, i);
        /* Labeled rows match on the (name, type) PAIR, the same rule that
         * governs how they unify everywhere else -- so `#row{id : int}` and
         * `#row{name : int}` intersect to the empty row, not to `int`. */
        bool present = false;
        for (uint32_t j = 0; j < y.as.typerow_.n_elements && !present; j++) {
            Type *yj = y.as.typerow_.elements[j];
            if (!yj || !type_eq(*yj, *e)) continue;
            if (labeled && !row_name_eq(row_name_at(y, j), xn)) continue;
            present = true;
        }
        if (present) n = row_push_unique(elems, names, n, e, xn);
    }
    return type_typerow_named(a, elems, names, (uint8_t)n);
}

TypeKind typekind_from_name(const char *name) {
    if (!name) return TY_UNKNOWN;
    if (strcmp(name, "unknown") == 0) return TY_UNKNOWN;
    if (strcmp(name, "nil") == 0) return TY_NIL;
    if (strcmp(name, "bool") == 0) return TY_BOOL;
    if (strcmp(name, "int") == 0) return TY_INT;
    if (strcmp(name, "int64") == 0) return TY_INT;      /* alias */
    if (strcmp(name, "float") == 0) return TY_FLOAT;
    if (strcmp(name, "float64") == 0) return TY_FLOAT;  /* alias */
    if (strcmp(name, "int8") == 0) return TY_INT8;
    if (strcmp(name, "int16") == 0) return TY_INT16;
    if (strcmp(name, "int32") == 0) return TY_INT32;
    if (strcmp(name, "uint8") == 0) return TY_UINT8;
    if (strcmp(name, "uint16") == 0) return TY_UINT16;
    if (strcmp(name, "uint32") == 0) return TY_UINT32;
    if (strcmp(name, "uint64") == 0) return TY_UINT64;
    if (strcmp(name, "float32") == 0) return TY_FLOAT32;
    if (strcmp(name, "cstr") == 0) return TY_CSTR;
    if (strcmp(name, "ptr-void") == 0 || strcmp(name, "ptr<void>") == 0) return TY_PTR_VOID;
    if (strcmp(name, "fn") == 0) return TY_FN;
    if (strcmp(name, "ref") == 0) return TY_REF;
    if (strcmp(name, "lref") == 0) return TY_LREF;
    if (strcmp(name, "rc") == 0) return TY_RC;
    if (strcmp(name, "weak") == 0) return TY_WEAK;
    if (strcmp(name, "&immut") == 0 || strcmp(name, "&") == 0) return TY_REF_IMMUT;
    if (strcmp(name, "&mut") == 0) return TY_REF_MUT;
    if (strcmp(name, "typeclass") == 0) return TY_TYPECLASS;
    if (strcmp(name, "typeclass-inst") == 0) return TY_TYPECLASS_INST;
    if (strcmp(name, "exception") == 0) return TY_EXCEPTION;
    if (strcmp(name, "cont") == 0) return TY_CONT;
    if (strcmp(name, "struct") == 0) return TY_STRUCT;
    if (strcmp(name, "!") == 0 || strcmp(name, "never") == 0) return TY_NEVER;
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
    if (strcmp(name, "set") == 0) return TY_SET;
    if (strcmp(name, "handler") == 0) return TY_HANDLER;
    /* SS0a: Session protocol types */
    if (strcmp(name, "Session") == 0) return TY_SESSION;
    if (strcmp(name, "Send") == 0)    return TY_SEND;
    if (strcmp(name, "Recv") == 0)    return TY_RECV;
    if (strcmp(name, "Close") == 0)   return TY_CLOSE;
    if (strcmp(name, "Choose") == 0)  return TY_CHOOSE;
    if (strcmp(name, "Branch") == 0)  return TY_BRANCH;
    if (strcmp(name, "Rec") == 0)     return TY_SESSION_REC;
    return TY_UNKNOWN;
}

/* ET3-D: type_is_subtype -- check if sub is a subtype of super_.
 * Returns true if sub is assignable where super_ is expected.
 * Rules:
 *   - TY_ANY as super_ accepts any subtype (top type).
 *   - TY_NEVER as sub is a subtype of everything (bottom type).
 *   - TY_CONTRACT as sub is a subtype of its base type (predicate already checked).
 *   - TY_HANDLER: covariant in result, contravariant in value (simplified: equality for now).
 *   - Otherwise: use type_eq.
 */
bool type_is_subtype(Type sub, Type super_) {
    if (super_.kind == TY_ANY) return true;
    if (sub.kind == TY_NEVER) return true;
    /* CT0: Contract type is a subtype of its base type */
    if (sub.kind == TY_CONTRACT && sub.as.contract_.base_type) {
        if (type_is_subtype(*sub.as.contract_.base_type, super_)) return true;
    }
    if (sub.kind == super_.kind) {
        if (sub.kind == TY_HANDLER) {
            /* FH4.1: same handled effect *set* (by name) + matching kinds.
             * (Variance is left as equality for v1, per the plan.) */
            bool rows_eq;
            if (sub.as.handler_.handled_row || super_.as.handler_.handled_row)
                rows_eq = effect_row_name_set_eq(sub.as.handler_.handled_row,
                                                 super_.as.handler_.handled_row);
            else
                rows_eq = (sub.as.handler_.effect_name == super_.as.handler_.effect_name);
            bool vk_ok = sub.as.handler_.value_kind == super_.as.handler_.value_kind
                      || sub.as.handler_.value_kind == TY_UNKNOWN
                      || super_.as.handler_.value_kind == TY_UNKNOWN;
            bool rk_ok = sub.as.handler_.result_kind == super_.as.handler_.result_kind
                      || sub.as.handler_.result_kind == TY_UNKNOWN
                      || super_.as.handler_.result_kind == TY_UNKNOWN;
            return rows_eq && vk_ok && rk_ok;
        }
        return type_eq(sub, super_);
    }
    return false;
}

/* Phase D: returns true when t is a struct type whose estimated sizeof exceeds 16
 * bytes, meaning it should be passed as const T* rather than by value. */
bool type_struct_pass_by_ptr(Type t) {
    /* SC7: a transparent int newtype is its bare int64 payload at the C level
     * (type_c_name -> "int64_t"), so it is always passed by value, never
     * pass-by-pointer-wrapped.  Catch it before the struct/ADT aggregate arms
     * below, which would otherwise treat the lowered single-int-field record
     * ADT as an aggregate and take its address at the call site. */
    if (type_is_transparent_int_newtype(t)) return false;
    /* end-to-end-monomorphization: a :heap type already lowers to a typed
     * pointer `T__A *`, so it is passed by value (the pointer itself), never
     * pass-by-pointer-wrapped -- wrapping would make the param a double
     * pointer `T__A **` and every field access read the wrong memory. */
    if (type_is_heap_struct(t)) return false;
    switch (t.kind) {
        case TY_APP: {
            /* Parametric-by-value: a large concrete flat-product ADT-app passes
             * as `const tur_adt_<Name>__A *`, mirroring the struct convention. */
            if (adt_app_is_byvalue_product(t))
                return adt_app_byval_pass_by_ptr(t);
            /* structdef-retirement DS-D: the former struct-app registry lookup is
             * dead -- no struct-headed app has a concrete by-value layout. */
            return false;
        }
        case TY_ADT:
            /* CONV-S1 (slice 3): a large by-value ADT product passes as
             * `const tur_adt_<Name> *`, mirroring the struct convention. */
            return t.as.adt_.def && adt_byval_pass_by_ptr(t.as.adt_.def);
        default:
            return false;
    }
}

/* end-to-end-monomorphization: true when t is a (possibly applied) struct type
 * whose StructDef carries the :heap attribute -- i.e. its monomorphic ABI is a
 * typed pointer `T__A *` to a heap-allocated header (the parametric-type ABI
 * matrix's typed-pointer class). */
bool type_is_heap_struct(Type t) {
    /* structdef-retirement DS-D: no struct-headed app forms, so a `:heap` struct
     * application no longer exists -- a lowered `:heap` defstruct is a `:heap`
     * record ADT, recognised by type_is_heap_adt.  Retained (called at many live
     * sites) but now always false. */
    (void)t;
    return false;
}

/* seam 3: the ADT analogue of type_is_heap_struct -- true when t is a (possibly
 * applied) ADT whose AdtDef carries :heap, i.e. its monomorphic ABI is a typed
 * pointer `tur_adt_<Name>__<args> *` to a heap-allocated header.  A lowered
 * `:heap` defstruct flows through this path. */
bool type_is_heap_adt(Type t) {
    if (t.kind == TY_ADT)
        return t.as.adt_.def && t.as.adt_.def->is_heap;
    if (t.kind == TY_APP) {
        AdtDef *def = NULL;
        Type args[16]; uint8_t n_args = 0;
        if (type_extract_adt_app(&t, &def, args, &n_args))
            return def && def->is_heap;
    }
    return false;
}

/* end-to-end-monomorphization: the by-value struct C name for a (heap or not)
 * struct/struct-app -- the same name type_c_name returns for a non-heap struct
 * (`Vec__int`), WITHOUT the trailing " *" that the heap pointer lowering adds.
 * make-struct uses this to build the underlying header literal before boxing it
 * onto the heap. Returns "int64_t" (via the generic fallback) when t has no
 * concrete layout. */
const char *type_struct_value_c_name(Type t) {
    /* structdef-retirement DS-D: the struct-app monomorph name branch is dead
     * (no struct-headed app has a concrete by-value layout); the generic
     * type_c_name handles every remaining case (ADT monomorphs included). */
    return type_c_name(t);
}

/* ============================================================================
 * Increment 4 stage 2 (repr-decision-function-plan): repr_of -- the intended
 * representation protocol per (type, position).  See types.h for the contract:
 * in stage 2 this is consulted only by shadow checks under --emit-abi-trace;
 * a disagreement with a site's actual decision is a logged finding, never a
 * behavior change.  The rules below are the consolidated protocol increments
 * 1-3 established:
 *
 *   - scalars are their bits in every position;
 *   - heap structs/ADTs/containers are heap pointers everywhere (the carrier
 *     round trip is lossless);
 *   - non-heap by-value products are real aggregates at param/result/binding/
 *     field positions, and HEAP-BOXED (any width) in container element slots
 *     and generic carrier sinks (increment 3);
 *   - fn values: cfnptr is a bare C pointer; effect-row'd, tyvar-signature,
 *     variadic, or arity>5 signatures keep the thin convention (the narrowed
 *     stage-1 claim); every other fn value is a fat handle (stages 1-2);
 *   - tyvars and compile-time-only kinds are the erased int64 carrier.
 * ==========================================================================*/
/* True when an app spine mentions an unresolved or deliberately-erased
 * argument (a tyvar, an unknown, or an existential package type) -- the
 * cases whose container spelling stays the erased int64 carrier. */
static bool repr_app_mentions_erased_arg(const Type *t) {
    if (!t) return false;
    if (t->kind == TY_TYVAR || t->kind == TY_UNKNOWN || t->kind == TY_EXISTS)
        return true;
    if (t->kind == TY_APP)
        return repr_app_mentions_erased_arg(t->as.app.fn) ||
               repr_app_mentions_erased_arg(t->as.app.arg);
    return false;
}

const char *repr_form_name(ReprForm f) {
    switch (f) {
        case REPR_SCALAR_BITS: return "scalar-bits";
        case REPR_HEAP_PTR:    return "heap-ptr";
        case REPR_BYVAL_AGG:   return "byval-agg";
        case REPR_BOXED_AGG:   return "boxed-agg";
        case REPR_CARRIER_I64: return "carrier-i64";
        case REPR_FAT_HANDLE:  return "fat-handle";
        case REPR_THIN_FN:     return "thin-fn";
    }
    return "?";
}

const char *repr_position_name(ReprPosition pos) {
    switch (pos) {
        case REPR_POS_PARAM:          return "param";
        case REPR_POS_RESULT:         return "result";
        case REPR_POS_LET_BIND:       return "let-bind";
        case REPR_POS_CONTAINER_ELEM: return "container-elem";
        case REPR_POS_STRUCT_FIELD:   return "struct-field";
        case REPR_POS_CARRIER_SINK:   return "carrier-sink";
    }
    return "?";
}

/* Increment 5 precondition: the coverage census.  Increment 5 is CONDITIONAL
 * -- "if the decision function shows a form with no remaining (type,
 * position) pairs" -- and nothing had ever produced that matrix.  Under
 * TUR_REPR_CENSUS=1 every answer prints one line, so a corpus sweep
 * aggregates into a position x form table.  An empty cell is a retirement
 * CANDIDATE, never a decision: the meta-plan's most repeated stall verdict is
 * "load-bearing, not redundant", so a candidate still owes a
 * redundancy-falsification probe before any code moves. */
static ReprForm repr_of_impl(const Type *t, ReprPosition pos);

ReprForm repr_of(const Type *t, ReprPosition pos) {
    ReprForm f = repr_of_impl(t, pos);
    static int census = -1;
    if (census < 0) census = getenv("TUR_REPR_CENSUS") ? 1 : 0;
    if (census)
        fprintf(stderr, "repr-census %s %s\n", repr_position_name(pos),
                repr_form_name(f));
    return f;
}

static ReprForm repr_of_impl(const Type *t, ReprPosition pos) {
    if (!t) return REPR_CARRIER_I64;

    /* Contracts share their base type's representation (type_c_name rule). */
    if (t->kind == TY_CONTRACT)
        return t->as.contract_.base_type
                   ? repr_of(t->as.contract_.base_type, pos)
                   : REPR_CARRIER_I64;

    /* fn values (the increment-1 protocol). */
    if (t->kind == TY_FN) {
        if (t->as.fn.cfnptr) return REPR_THIN_FN;
        if (t->as.fn.boxed) return REPR_FAT_HANDLE;
        if (pos == REPR_POS_CARRIER_SINK) return REPR_CARRIER_I64;
        /* Param and result positions ask different questions -- a tyvar-sig fn
         * type is fat in a param slot and thin as a declared result. */
        if (pos == REPR_POS_PARAM)
            return fn_param_type_is_fat_normalized(t) ? REPR_FAT_HANDLE
                                                      : REPR_THIN_FN;
        return fn_result_type_is_fat_normalized(t) ? REPR_FAT_HANDLE
                                                   : REPR_THIN_FN;
    }

    /* Erased/unresolved: the int64 carrier in every position. */
    if (t->kind == TY_TYVAR || t->kind == TY_UNKNOWN || t->kind == TY_FORALL)
        return REPR_CARRIER_I64;

    /* An existential package is a heap-boxed (value, dict) pair -- a heap
     * pointer wherever it travels (first sweep: 52 sites, all pointer-
     * declared; the initial carrier spelling here was the spec hole). */
    if (t->kind == TY_EXISTS)
        return REPR_HEAP_PTR;

    /* SS1/SS2 internal session pairs (make-session's [Session, Session]
     * and recv's [T, Session]) are the same shape: a heap-boxed pair whose
     * only C spelling is its `void *` pointer, so the pointer IS the value
     * wherever it travels.  Their TY_SIMPLE_REPR_ROWS layout column stays
     * false on purpose (they must never form a by-value monomorph or a
     * type argument), which is why the generic scalar fallthrough below
     * cannot see them -- without this arm the merge-temp shadow reports
     * the site's correct `void *` decl as a carrier/heap-ptr seam
     * (stdlib/session.tur, recv inside a control-form tail).  The offer
     * result (TY_SESSION_OFFER, int64_t-spelled) rides the carrier via
     * the fallthrough, which already agrees with its declarations. */
    if (t->kind == TY_SESSION_PAIR || t->kind == TY_SESSION_RECV_PAIR)
        return REPR_HEAP_PTR;

    /* `any` / union values are the two-word tur_tagged_t -- a real by-value
     * aggregate at direct positions; the erased carrier at generic sinks and
     * container slots (the layout switch deliberately rejects them from
     * by-value monomorph fields -- see the TY_ANY note there). */
    if (t->kind == TY_ANY || t->kind == TY_UNION) {
        /* vec-of-any-repr-decision-ice: a CONTAINER ELEMENT is boxed, not
         * erased.  A container slot is one machine word and a tur_tagged_t is
         * two, so erasing an `any` element to the carrier keeps the payload
         * and drops the tag -- `type-of` on a element read back then answers
         * whatever the carrier collides with.  The by-value-aggregate arm
         * below has answered REPR_BOXED_AGG at this position for exactly this
         * reason since increment 4; an `any` is the same problem wearing a
         * different kind, so it gets the same answer and the same machinery
         * (type_is_boxed_container_elem IS this call, so the store, the read
         * and the element-free all follow from here).
         *
         * A carrier SINK and a STRUCT FIELD keep the carrier: the sink is
         * generic-erased by construction, and a field is where a by-value
         * monomorph would need the 16-byte slot the layout table declines. */
        if (pos == REPR_POS_CONTAINER_ELEM) return REPR_BOXED_AGG;
        if (pos == REPR_POS_CARRIER_SINK || pos == REPR_POS_STRUCT_FIELD)
            return REPR_CARRIER_I64;
        return REPR_BYVAL_AGG;
    }

    /* Heap-represented nominal/parametric types: the pointer IS the value.
     * A heap app with UNRESOLVED (tyvar) or EXISTENTIAL arguments -- `(Vec
     * A)` inside a generic body, `(Vec (exists ...))` whose element is a
     * deliberately-erased package (the vec-get-existential-element design)
     * -- is the SAME pointer spelled as the erased carrier (int64_t); report
     * the erased spelling so the shadow log measures real seams, not the
     * lossless pointer/carrier round trip (first sweep: 431 of 521 lines
     * were this spelling distinction; the exists-element rows surfaced in
     * the third). */
    if (type_is_heap_struct(*t) || type_is_heap_adt(*t)) {
        /* Increment 4 stage 3 (2026-08-16): the erased spelling is a
         * DECLARATION fact, not a value fact, so it is scoped to the
         * positions that declare.  `(Vec A)` inside a generic body is
         * DECLARED `int64_t` -- that is what the let-bind chokepoint
         * migrated around -- but the value is a pointer in both spellings,
         * and a CROSSING of it is a pure reinterpret.
         *
         * This position-scoping was earned three times over: the same
         * pointer/carrier spelling identity accounted for 431 of the first
         * sweep's 521 let-bind lines, all 84 of the first adt-field sweep,
         * and all 65 of the first arg-bridge sweep.  Three positions
         * rediscovering one calibration is the decision function's job to
         * absorb, not each site's to re-exclude. */
        if (t->kind == TY_APP && repr_app_mentions_erased_arg(t) &&
            (pos == REPR_POS_LET_BIND || pos == REPR_POS_RESULT))
            return REPR_CARRIER_I64;
        /* Same erased-declaration fact, reached by the other spelling: a BARE
         * parametric base (`ORMap` standing for `(ORMap V)` inside a generic
         * body, its args dropped rather than set to tyvars) is the erased
         * container too.  The TY_APP test above cannot see it -- there are no
         * args left to find a tyvar in.
         *
         * emit_repr_concrete_heap_ptr_c_name has always excluded BOTH
         * spellings (it tests `n_type_params > 0` right beside its
         * mentions-tyvar tests, with a comment saying why), so the two sides
         * disagreed here and the shadow ICE'd with
         * `want=heap-ptr got=carrier-i64` on a bare parametric :heap struct.
         * That is what a `(defstruct ORMap :heap [V] ...)` merge temp hit. */
        if (t->kind == TY_ADT && t->as.adt_.def &&
            t->as.adt_.def->n_type_params > 0 &&
            (pos == REPR_POS_LET_BIND || pos == REPR_POS_RESULT))
            return REPR_CARRIER_I64;
        return REPR_HEAP_PTR;
    }

    /* A transparent int newtype (a PARAMETRIC single-ctor record whose one
     * field is a concrete int -- `(defstruct Schema [A] (raw :int))`) is its
     * int64 payload in every position: the SC7 rule type_c_name applies, and
     * the reason `(:: (ArrShadow 7) (ArrShadow int))` emits no ctor call at
     * all.  Fourth-sweep finding: the byval-agg prediction for these was a
     * spec hole, not a site seam. */
    if (type_is_transparent_int_newtype(*t))
        return REPR_SCALAR_BITS;

    /* opaque-pointer-c-spelling: an opaque newtype over a pointer is a
     * LEAF pointer -- the same form `cstr` / `ptr<T>` take, not a heap handle:
     * `void *` is the type's own spelling, so the bits ARE the value and
     * repr_form_from_cty recovers SCALAR_BITS from the declaration.  Predicting
     * the carrier here instead is what ICEs every `Arc` / `Mutex` / `String`
     * let-binding the moment the spelling changes. */
    {
        const AdtDef *odef = t->kind == TY_ADT ? t->as.adt_.def
                           : t->kind == TY_APP ? type_adt_app_def(t)
                                               : NULL;
        if (adt_opaque_c_names_as_pointer(odef)) return REPR_SCALAR_BITS;
    }

    /* SR3 slice B: a niche-filled `(Option P)` IS P's pointer in every
     * position -- there is no aggregate, so the by-value-product arm below
     * would predict a form the type has no spelling for.  The eligibility rule
     * requires P to c-name as a real pointer, which is what makes HEAP_PTR the
     * right answer rather than the carrier word. */
    if (t->kind == TY_APP && adt_app_is_niche_option(*t))
        return REPR_HEAP_PTR;

    /* Non-heap by-value products (nominal ADT or concrete parametric app):
     * real aggregates in direct positions; boxed in container slots and
     * generic sinks (increment 3, width-independent). */
    bool byval_product =
        (t->kind == TY_ADT && t->as.adt_.def &&
         adt_is_byvalue_product(t->as.adt_.def)) ||
        (t->kind == TY_APP && adt_app_is_byvalue_product(*t));
    if (byval_product) {
        if (pos == REPR_POS_CONTAINER_ELEM || pos == REPR_POS_CARRIER_SINK)
            return REPR_BOXED_AGG;
        /* Increment 4 stage 3 (adt-field shadow, 2026-08-15): a field slot
         * boxes a by-value product that OWNS drop glue, exactly as a
         * container slot does.  The reason is the owner's copyability, not
         * the element protocol -- inlining a sub-aggregate that owns an
         * rc/ref would force recursive drop glue onto the outer product, so
         * `adt_field_is_inline_byval` restricts inlining to drop-glue-free
         * inners and the owning ones ride the carrier with `drop_inner_def`
         * driving their release.  The shadow found this as a spec hole: it
         * was the only field shape whose slot form disagreed, and stripping
         * this arm makes it fire again (sabotage-verified). */
        const AdtDef *bp_def = t->kind == TY_ADT ? t->as.adt_.def
                                                 : type_adt_app_def(t);
        if (pos == REPR_POS_STRUCT_FIELD && bp_def && bp_def->needs_drop_glue)
            return REPR_BOXED_AGG;
        return REPR_BYVAL_AGG;
    }

    /* Non-product ADTs / non-concrete apps ride the carrier. */
    if (t->kind == TY_ADT || t->kind == TY_APP)
        return REPR_CARRIER_I64;

    /* Everything else with a concrete layout is a one-word scalar (int,
     * float, bool, cstr, sym, ptr leaves, refs, handles...); the rest is the
     * erased carrier (compile-time-only kinds, unions, placeholders). */
    if (type_has_concrete_codegen_layout(t)) return REPR_SCALAR_BITS;
    return REPR_CARRIER_I64;
}

bool repr_shadow_active(void) {
#ifndef NDEBUG
    return true;              /* enforcement mode: evaluate even without the flag */
#else
    return g_emit_abi_trace;  /* Release: measurement only, and only on request */
#endif
}

void repr_shadow_disagree(const char *site, bool known, const char *line) {
    /* Measurement mode collects the whole list; it never aborts, because the
     * point of a sweep is to see all of it at once. */
    if (g_emit_abi_trace) {
        fputs(line, stderr);
        return;
    }
    if (known) return;        /* pinned work-list row -- never a build failure */
#ifndef NDEBUG
    if (getenv("TUR_REPR_NO_SHADOW_ICE")) {
        fprintf(stderr, "tur: warning: representation shadow disagreement at "
                        "%s; downgraded by TUR_REPR_NO_SHADOW_ICE\n  %s",
                site, line);
        return;
    }
    fprintf(stderr,
            "tur: internal error (ICE): a representation decision disagrees "
            "with repr_of at %s.\n  %s"
            "Two sites now decide this value's representation differently -- "
            "the defect family\n"
            "docs/archive/repr-decision-function-plan.md exists to close.  "
            "Re-run with\n"
            "--emit-abi-trace to see every disagreement, or set "
            "TUR_REPR_NO_SHADOW_ICE=1 to\ndowngrade this to a warning while "
            "fixing.\n",
            site, line);
    abort();
#else
    (void)site;
#endif
}

ReprForm repr_of_binding(const struct Binding *b, ReprPosition pos) {
    if (!b) return REPR_CARRIER_I64;
    /* The two decisions elaboration records on the binding and the Type does
     * not carry.  Order matters: a rank-2 poly fn PARAM is the by-value
     * tur_poly_fn_t carrier even though its type is spelled ptr<void>, and
     * `^fat` is an explicit request for the fat handle. */
    if (b->is_poly_fn && b->is_param) return REPR_CARRIER_I64;
    if (b->is_fat) return REPR_FAT_HANDLE;
    /* A parameter's representation is fixed where it was DECLARED, not where
     * it is later used -- a fn param normalized to fat at the signature stays
     * fat when it appears in a tail, so asking the use position would give
     * the wrong answer (param and result positions deliberately disagree; see
     * fn_param_type_is_fat_normalized vs fn_result_type_is_fat_normalized). */
    if (b->is_param) return repr_of(&b->type, REPR_POS_PARAM);
    return repr_of(&b->type, pos);
}
