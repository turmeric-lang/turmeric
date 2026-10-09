#include "cps_ir.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cps.h"
#include "effect.h"   /* effect_row_is_empty */
#include "builtins.h"
#include "globals.h"

/* The elaborator's free-variable walker (elab_internal.h): a direct-dispatch
 * match's guard reads (build_direct_match_term). */
Binding **collect_free_vars(const Expr *e, Binding **params, uint8_t n_params,
                            Binding **self_exclude, uint32_t n_self_exclude,
                            uint32_t *n_out);

/* =========================================================================
 * CPS2 (cps-transform-plan): ANF/CPS translation for colored functions.
 *
 * The translation is the textbook two-function scheme:
 *   - cps_tail(e, k)      : emit a term that delivers e's value to continuation k.
 *   - cps_bind(e, x, rest): emit a term that binds e's value to x, then runs rest.
 * Non-trivial arguments are first "atomized" (named by a fresh binder), which is
 * what establishes the ANF invariant. Colored callees are tail-called with the
 * continuation threaded through (CT_TAILCALL); in non-tail position a join
 * continuation (CT_LETCONT) names the call's result. See cps_ir.h.
 * ========================================================================= */

/* Partial-application inlining (currying-effect-partial keystone): a let-bound
 * closure `f = (partial-app TARGET captured...)` whose ONLY use is a saturated
 * direct call `(f rest...)` is rewritten to the underlying saturated call
 * `(TARGET captured... rest...)` -- no fat closure, no env drop-glue, and (when
 * TARGET is colored) a native DK tailcall that threads `k`.  The capture bindings
 * stay in scope (emitted from the closure's prelude let), so the rewrite only
 * substitutes the callee + prepends the capture var-refs. */
typedef struct PapInline {
    const Binding *var;      /* the let-bound closure var (e.g. add10) */
    const Binding *target;   /* the underlying fn the pap wraps (e.g. log-add) */
    Binding **caps;          /* captured arg bindings, in leftmost-first arg order */
    uint32_t  n_caps;
    uint32_t  rem_arity;     /* args a saturated call to `var` supplies (= target arity - n_caps) */
    bool      is_alias;      /* `var` only renames the fn PARAM `target` (no caps) */
    /* `var` holds a fresh capturing closure of the lifted lambda `target`
     * (cps_ir_let_local_closure): a call through it is `(target var rest...)`,
     * the env box first.  The binding is KEPT -- the box is still built. */
    bool      is_env_call;
} PapInline;

typedef struct CpsB {
    Arena *a;
    Expr  *program;     /* for colored-callee lookup */
    uint32_t counter;   /* fresh-id source */
    CKont  retk;        /* the function's return continuation (KK_RET) */
    const Binding *cur_fn;   /* binding of the fn being translated (self-call detection) */
    const FnDef   *cur_fd;   /* ...and its FnDef (a lifted lambda's env parameter) */
    /* The pending expression fold_pending is binding right now, when its
     * consuming slot does not retain it (PendItem.reap_ok); NULL otherwise.
     * The EX_CLOSURE arm reaps a capturing closure's env only for this one. */
    const Expr *reap_closure_expr;
    bool cur_fn_leaf_fiber;  /* the fn's body indirect-calls a fn-VALUE -> permanently fiber */
    PapInline pap[32];       /* active pap-inline registrations (scoped per-let) */
    uint32_t  n_pap;
    /* cps-while-native: loop-lowering state, active only while lowering a CT_LOOP
     * body.  loop_vars[i] is a loop-carried ^mut binding; loop_nexts[i] is the
     * pre-created `$next` CVar a `set!` of that var binds and CT_CONTINUE reads
     * (stable identity -> build-order-independent).  n_loop == 0 when not in a loop. */
    const Binding *loop_vars[16];
    CVar           loop_nexts[16];
    uint32_t       n_loop;
    /* cps-while-native (cell-carried vars): >0 while lowering a loop body.
     * Nested loops are detected here rather than by n_loop, which is 0 for a
     * loop whose every carried var is a cell. */
    uint32_t       loop_depth;
    /* cps-while-native read-after-set: a forward pre-pass over the straight-line
     * loop body records, by Expr-node identity, each carried-var READ that occurs
     * AFTER that var's `set!` this iteration -- such a read must resolve to the
     * var's `$next` CVar, not the entry param.  Consulted in atomize().  Computed
     * BEFORE lowering (which is backward), so ordering is not an issue. */
    const Expr    *rs_nodes[64];
    CVar           rs_cvars[64];
    uint32_t       rs_n;
    /* cps-async: handler-case-body nesting depth.  Bumped around building a
     * `(handle ...)`/`(with-handler ...)` case body (build_handle_core), so the
     * per-node lowering of an `await` nested in a handler case delegates to the
     * fiber path (build_letraw -> CT_LETRAW, which handle_case_ok admits) rather
     * than heap-lowering to a CT_AWAIT the handler-case admission cannot host.
     * An `await` in a plain async body (depth 0) still heap-lowers via build_await. */
    uint32_t       in_handler_case;
} CpsB;

/* cps-while-native: index of `bd` among the active loop-carried vars, or -1. */
static int loop_var_index(CpsB *b, const Binding *bd) {
    if (!bd) return -1;
    for (uint32_t i = 0; i < b->n_loop; i++)
        if (b->loop_vars[i] == bd) return (int)i;
    return -1;
}

static bool cps_self_env_call(const CpsB *b, const Binding *callee);   /* fwd */

static const PapInline *pap_lookup(CpsB *b, const Binding *v) {
    if (!v) return NULL;
    for (int i = (int)b->n_pap - 1; i >= 0; i--)
        if (b->pap[i].var == v) return &b->pap[i];
    return NULL;
}

static const Expr *ascribe_peel(const Expr *e);   /* fwd */

/* E2a thread-param set (populated by emit_cps_ir.c before each fn is translated). */
static const Binding *g_thread_params[256];
static int            g_thread_params_n;
void cps_ir_thread_param_reset(void) { g_thread_params_n = 0; }
void cps_ir_thread_param_add(const Binding *p) {
    if (!p) return;
    for (int i = 0; i < g_thread_params_n; i++) if (g_thread_params[i] == p) return;
    if (g_thread_params_n < 256) g_thread_params[g_thread_params_n++] = p;
}
bool cps_ir_thread_param_has(const Binding *p) {
    if (!p) return false;
    for (int i = 0; i < g_thread_params_n; i++) if (g_thread_params[i] == p) return true;
    return false;
}

/* Does `e` (recursively) contain a call THROUGH AN EFFECTFUL FN-VALUE -- an
 * indirect call whose callee binding is a fn-typed NON-global (a param or local
 * closure) with a NON-EMPTY effect row (concrete effects, or a row variable)?
 * Such a call cannot thread the DK (the callee is opaque) AND performs an effect,
 * so a function whose body contains one is PERMANENTLY fiber: its effect is
 * reached through the indirect call and can never be DK-emitted.  Used to gate
 * the self-recursive-call whole-body delegation to genuinely leaf-fiber HOFs
 * (map-list `f : #fx{e}`, apply-logged), so that neither a normal recursive
 * function NOR a PURE higher-order function that CPS-emits natively via a
 * heap-join (P6: `__cons-fmap` indirect-calls a PURE fmap `f` and recurses --
 * empty effect row, so NOT matched) is wrongly routed onto the direct path.
 * A bare fn-value with no effect annotation (empty row) is NOT leaf-fiber.
 * Bounded depth guard against pathological nesting. */
static bool fn_binding_effectful(const Binding *fb) {
    if (!fb || fb->is_global || fb->type.kind != TY_FN) return false;
    struct EffectRow *r = fb->type.as.fn.effect_row;
    return r && r->kind != ERK_EMPTY;
}
/* E2/taint-completeness (cps-tramp-resume): is this call THROUGH A FN-VALUE that
 * performs an effect (a fn-typed param/local, or an fn_expr callee, with a
 * non-empty effect row)?  Such a call cannot yet thread the DK (E2 needs the
 * fat-closure __fn_cps slot), so delegating it to fiber inside a CPS body would
 * ESCAPE the effect (the enclosing handle is DK).  Under the flag we EVICT the
 * enclosing function so it stays fiber (correct), until real E2 threading lands. */
static bool call_is_effectful_fnvalue(const Expr *e) {
    const Binding *fb = e->as.call_.fn_binding;
    if (fb) return fn_binding_effectful(fb);
    const Expr *fe = e->as.call_.fn_expr;
    /* E2c: a struct-field fn-value call `(.f obj)` -- the effect row of an
     * effect-annotated capability field lives on the record CtorField, not the
     * field-access node's TY_FN type (which reads empty).  Read it off the ctor. */
    if (fe && fe->kind == EX_GET_FIELD) {
        const CtorDef *c = fe->as.get_field_.adt_ctor;
        uint32_t fi = fe->as.get_field_.field_idx;
        if (c && fi < c->n_fields) {
            struct EffectRow *r = c->fields[fi].effect_row;
            if (r && r->kind != ERK_EMPTY) return true;
        }
    }
    if (fe && fe->type.kind == TY_FN) {
        struct EffectRow *r = fe->type.as.fn.effect_row;
        return r && r->kind != ERK_EMPTY;
    }
    return false;
}
/* cps-evicts-handle-in-operand-positions (the un-annotated generic fn param):
 * is this call's callee a FAT fn-value PARAMETER -- a `^fat` param with a fn
 * signature, or a fn-typed param the elaborator fat-normalizes -- so that its
 * EMPTY-row call can thread through the registry when the param is a thread
 * param?  The parameter's row says nothing about the value passed, which may
 * perform; an unregistered (pure) value is called directly by the dispatch's
 * fallback (emit_e2a_fat_dispatch), which needs the signature to spell. */
static bool call_through_fat_param(const Expr *e) {
    const Binding *fb = e->as.call_.fn_binding;
    if (!fb || fb->is_global || fb->is_poly_fn || !fb->is_param || fb->type.kind != TY_FN)
        return false;
    return fb->is_fat || fn_param_type_is_fat_normalized(&fb->type);
}

static bool expr_has_indirect_fnvalue_call(const Expr *e, int depth) {
    e = ascribe_peel(e);
    if (!e || depth > 64) return false;
    #define IFC(x) expr_has_indirect_fnvalue_call((x), depth + 1)
    switch (e->kind) {
        case EX_CALL: {
            if (fn_binding_effectful(e->as.call_.fn_binding)) return true;
            if (e->as.call_.fn_expr && IFC(e->as.call_.fn_expr)) return true;
            for (uint32_t i = 0; i < e->as.call_.n_args; i++)
                if (IFC(e->as.call_.args[i])) return true;
            return false;
        }
        case EX_BUILTIN:
            for (uint32_t i = 0; i < e->as.builtin.n; i++)
                if (IFC(e->as.builtin.args[i])) return true;
            return false;
        case EX_DO:
            for (uint32_t i = 0; i < e->as.do_.n; i++)
                if (IFC(e->as.do_.items[i])) return true;
            return false;
        case EX_LET: case EX_LETREC:
            for (uint32_t i = 0; i < e->as.let_.n; i++)
                if (IFC(e->as.let_.bindings[i].init)) return true;
            return IFC(e->as.let_.body);
        case EX_IF:
            return IFC(e->as.if_.cond) || IFC(e->as.if_.then_)
                || (e->as.if_.else_or_null && IFC(e->as.if_.else_or_null));
        case EX_HANDLE:
            if (e->as.handle_.handle) {
                HandleExpr *h = e->as.handle_.handle;
                if (IFC(h->body)) return true;
                for (uint8_t i = 0; i < h->n_cases; i++)
                    if (IFC(h->cases[i].body)) return true;
            }
            return false;
        case EX_WHILE:
            return IFC(e->as.while_.cond) || IFC(e->as.while_.body);
        case EX_SET:      return IFC(e->as.set_.value);
        case EX_CAST:     return IFC(e->as.cast_.expr);
        case EX_RETURN:   return IFC(e->as.return_.value);
        default:          return false;
    }
    #undef IFC
}

/* E2 (fat-closure fn-value threading): each argument of a fat-closure fn-value
 * call crosses the `tur_poly_fn_t.fn_cps` ABI as one `int64_t` word.  The
 * `__poly_N__cps` twin the poly-wrap emits (emit_module.c) converts each word
 * back to the wrapped fn's own C parameter type (cps_ir_fncps_arg_ctype) for
 * its direct call, so the kinds admitted are the ones whose C spelling is known
 * from the kind alone: every scalar.  A float crosses as its bits
 * (cps_ir_fncps_word_of), the convention the DK slot itself uses, and travels
 * to the registered `__cps` entry at its own type, as the E2a registry's call
 * sites pass it (e2a_cast).  Until 2026-10-09 a narrow integer, a `bool` or a
 * float stayed on the delegated direct path, so an effectful callback of one
 * was refused. */
static bool fncps_arg_kind_ok(TypeKind k) {
    switch (k) {
        case TY_INT: case TY_INT64: case TY_CSTR: case TY_PTR_VOID:
        case TY_BOOL:
        case TY_INT8: case TY_INT16: case TY_INT32:
        case TY_UINT8: case TY_UINT16: case TY_UINT32: case TY_UINT64:
        case TY_FLOAT: case TY_FLOAT64: case TY_FLOAT32:
            return true;
        default:
            return false;
    }
}
const char *cps_ir_fncps_arg_ctype(TypeKind k) {
    switch (k) {
        case TY_CSTR:     return "const char *";
        case TY_PTR_VOID: return "void *";
        case TY_BOOL:     return "bool";
        case TY_INT8:     return "int8_t";
        case TY_INT16:    return "int16_t";
        case TY_INT32:    return "int32_t";
        case TY_UINT8:    return "uint8_t";
        case TY_UINT16:   return "uint16_t";
        case TY_UINT32:   return "uint32_t";
        case TY_UINT64:   return "uint64_t";
        case TY_FLOAT: case TY_FLOAT64: return "double";
        case TY_FLOAT32:  return "float";
        default:          return "int64_t";
    }
}
static char *fncps_wrap(const char *pre, const char *x, const char *post) {
    size_t a = strlen(pre), b = strlen(x), c = strlen(post);
    char *s = (char *)malloc(a + b + c + 1);
    if (!s) { fprintf(stderr, "tur: oom\n"); abort(); }
    memcpy(s, pre, a); memcpy(s + a, x, b); memcpy(s + a + b, post, c);
    s[a + b + c] = '\0';
    return s;
}
char *cps_ir_fncps_word_of(TypeKind k, const char *value) {
    switch (k) {
        case TY_FLOAT: case TY_FLOAT64:
            return fncps_wrap("((union { double d; int64_t i; }){ .d = (", value, ") }).i");
        case TY_FLOAT32:
            return fncps_wrap("(int64_t)((union { float f; uint32_t u; }){ .f = (", value, ") }).u");
        case TY_CSTR: case TY_PTR_VOID:
            return fncps_wrap("(int64_t)(intptr_t)(", value, ")");
        default:
            return fncps_wrap("(int64_t)(", value, ")");
    }
}
char *cps_ir_fncps_value_of(TypeKind k, const char *word) {
    switch (k) {
        case TY_FLOAT: case TY_FLOAT64:
            return fncps_wrap("((union { double d; int64_t i; }){ .i = (int64_t)(", word, ") }).d");
        case TY_FLOAT32:
            return fncps_wrap("((union { float f; uint32_t u; }){ .u = (uint32_t)(", word, ") }).f");
        case TY_CSTR:
            return fncps_wrap("(const char *)(intptr_t)(", word, ")");
        case TY_PTR_VOID:
            return fncps_wrap("(void *)(intptr_t)(", word, ")");
        default: {
            char pre[32];
            snprintf(pre, sizeof pre, "(%s)(", cps_ir_fncps_arg_ctype(k));
            return fncps_wrap(pre, word, ")");
        }
    }
}
/* ...and a RESULT the direct fallback can call `f.fn` for with its real C type
 * (emit_cps_ir.c, fncps_direct_call): the closure's wrapper returns exactly the
 * param's declared result, so calling it through an `int64_t` function type is a
 * mismatched call -- a -fsanitize=function trap -- and a float result would be
 * read from the wrong register.  A pointer result crosses as its word, through
 * intptr_t, both ways. */
static bool fncps_result_kind_ok(TypeKind k) {
    if (k == TY_NIL) return true;
    return fncps_arg_kind_ok(k);
}
bool cps_ir_fncps_sig_ok(const Type *fn_ty) {
    if (!fn_ty || fn_ty->kind != TY_FN) return false;
    uint32_t n = fn_ty->as.fn.arity;
    if (n > CPS_FNCPS_MAX_ARGS || (n && !fn_ty->as.fn.arg_kinds)) return false;
    for (uint32_t i = 0; i < n; i++)
        if (!fncps_arg_kind_ok((TypeKind)fn_ty->as.fn.arg_kinds[i])) return false;
    return fncps_result_kind_ok(fn_ty->as.fn.result_kind);
}
bool cps_ir_fncps_closure_sig_ok(const Type *lifted_ty) {
    if (!lifted_ty || lifted_ty->kind != TY_FN) return false;
    uint32_t n = lifted_ty->as.fn.arity;
    if (n < 1 || n - 1 > CPS_FNCPS_MAX_ARGS || !lifted_ty->as.fn.arg_kinds) return false;
    /* The dispatcher (ensure_fncps_env_dispatch) keys the registry on the env
     * box's slot 0 and, on a miss, calls slot 0 at its own type -- so slot 0
     * must BE the lifted entry the registry knows.  It is for every scalar
     * but two: an untyped `ptr<void>` parameter is the word in the slot and
     * `void *` in the definition, and a narrow-integer result comes back
     * widened (thunk_param_slot_c_name / thunk_result_slot_c_name); either
     * puts a widening wrapper in slot 0 (ensure_closure_slot0_widen). */
    for (uint32_t i = 1; i < n; i++) {
        TypeKind ak = (TypeKind)lifted_ty->as.fn.arg_kinds[i];
        /* ...and the registry hit calls the twin with every non-float kind as
         * the word, which a `uint64_t` parameter's twin does not declare (its
         * E2a registration has no word adapter for it). */
        if (!fncps_arg_kind_ok(ak) || ak == TY_PTR_VOID || ak == TY_UINT64) return false;
    }
    switch (lifted_ty->as.fn.result_kind) {
        case TY_NIL: case TY_INT: case TY_INT64: case TY_UINT64:
        case TY_FLOAT: case TY_FLOAT64: case TY_FLOAT32:
        case TY_CSTR: case TY_PTR_VOID:
            return true;
        default:
            return false;
    }
}
/* E2 (fat-closure fn-value threading): is a call `(fn args...)` through the
 * poly-fn PARAM `fn` a candidate for `fn_cps` DK-threading?  It must be a
 * CONCRETE fat closure -- a bare `:fn` carrier (poly_type NULL) or a typed `:fn`
 * signature (poly_type TY_FN) -- with up to CPS_FNCPS_MAX_ARGS int-register-class
 * NON-poly arguments (the `tur_poly_fn_t.fn_cps` ABI is `(void*, int64_t...,
 * DK*)`).  A rank-2/3 forall poly param (poly_type TY_FORALL) or a poly-wrapped
 * arg (poly_arg_mask) crosses a different, wider ABI and is excluded (it stays
 * on the delegated direct path). */
static bool fncps_param_call_ok(const Binding *fn, const Expr *e) {
    if (!fn || !fn->is_poly_fn) return false;
    if (fn->poly_type && fn->poly_type->kind == TY_FORALL) return false;
    if (e->as.call_.n_args > CPS_FNCPS_MAX_ARGS) return false;
    if (e->as.call_.poly_arg_mask) return false;
    if (!fncps_result_kind_ok(e->type.kind)) return false;
    for (uint32_t i = 0; i < e->as.call_.n_args; i++)
        if (!fncps_arg_kind_ok(e->as.call_.args[i]->type.kind)) return false;
    return true;
}

/* ---- small allocation helpers ----------------------------------------- */

static CTerm *new_term(CpsB *b, CTermKind k) {
    CTerm *t = arena_alloc(b->a, sizeof(CTerm));
    memset(t, 0, sizeof(CTerm));
    t->kind = k;
    return t;
}

/* Name the source form that fell outside the CPS2 subset, for a diagnostic /
 * measurement (N6.5 seed: a residual CT_UNSUPPORTED should say WHICH form it
 * was, so the coverage gate names the shape rather than reporting a generic
 * miss).  Covers the kinds that can reach the CPS-translation default arm and
 * fail safe_to_delegate; anything else prints its numeric kind. */
static const char *cps_form_name(const Expr *e) {
    if (!e) return "null";
    switch (e->kind) {
        case EX_FN:            return "EX_FN (bare lambda)";
        case EX_CLOSURE:       return "EX_CLOSURE (capturing closure)";
        case EX_FN_TO_FAT:     return "EX_FN_TO_FAT";
        case EX_POLY_WRAP:     return "EX_POLY_WRAP";
        case EX_POLY_TO_FAT:   return "EX_POLY_TO_FAT";
        case EX_WHILE:         return "EX_WHILE";
        case EX_PANIC:         return "EX_PANIC";
        case EX_CONS_LIST:     return "EX_CONS_LIST";
        case EX_BORROW_IMMUT:  return "EX_BORROW_IMMUT";
        case EX_INLINE_C:      return "EX_INLINE_C";
        case EX_SET:           return "EX_SET (mutation)";
        case EX_SET_DEREF:     return "EX_SET_DEREF";
        case EX_SET_FIELD:     return "EX_SET_FIELD";
        case EX_SET_LIT:       return "EX_SET_LIT";
        case EX_MATCH:         return "EX_MATCH";
        case EX_LETREC:        return "EX_LETREC";
        case EX_DEFER:         return "EX_DEFER";
        case EX_GEN:           return "EX_GEN";
        case EX_YIELD:         return "EX_YIELD";
        case EX_GEN_NEXT:      return "EX_GEN_NEXT";
        case EX_GEN_UNWRAP:    return "EX_GEN_UNWRAP";
        case EX_SELECT:        return "EX_SELECT";
        case EX_STM:           return "EX_STM";
        case EX_ATOMICALLY:    return "EX_ATOMICALLY";
        case EX_RETRY:         return "EX_RETRY";
        case EX_CHECK:         return "EX_CHECK";
        case EX_OR_ELSE:       return "EX_OR_ELSE";
        case EX_TVAR_NEW:      return "EX_TVAR_NEW";
        case EX_TVAR_READ:     return "EX_TVAR_READ";
        case EX_TVAR_WRITE:    return "EX_TVAR_WRITE";
        case EX_TVAR_MODIFY:   return "EX_TVAR_MODIFY";
        case EX_TVAR_SWAP:     return "EX_TVAR_SWAP";
        case EX_TVAR_CAS:      return "EX_TVAR_CAS";
        case EX_WITH_HANDLER:  return "EX_WITH_HANDLER";
        case EX_COMPOSE_HANDLERS: return "EX_COMPOSE_HANDLERS";
        case EX_HANDLER_LIT:   return "EX_HANDLER_LIT";
        case EX_DISCONTINUE:   return "EX_DISCONTINUE";
        case EX_CATCH_UNWIND:  return "EX_CATCH_UNWIND";
        case EX_CATCH_PANIC_OF:return "EX_CATCH_PANIC_OF";
        case EX_CLONEABLE_SHIFT: return "EX_CLONEABLE_SHIFT";
        case EX_SERIAL_SHIFT:  return "EX_SERIAL_SHIFT";
        case EX_REINTERPRET:   return "EX_REINTERPRET";
        case EX_CALL:          return "EX_CALL";
        case EX_GET_FIELD:     return "EX_GET_FIELD";
        case EX_DICT:          return "EX_DICT";
        default:               return NULL;   /* caller prints numeric kind */
    }
}

/* Build a CT_UNSUPPORTED whose `why` names the offending source form. */
static CTerm *unsupported_form(CpsB *b, const Expr *e) {
    CTerm *t = new_term(b, CT_UNSUPPORTED);
    const char *nm = cps_form_name(e);
    if (nm && e->kind == EX_REINTERPRET) {
        /* cps-evicts-handle-in-operand-positions: name what the reinterpret
         * converts and what it wraps -- the wrapped form is the one that made
         * it non-delegatable. */
        const Expr *in = e->as.reinterpret_.expr;
        const char *inm = in ? cps_form_name(in) : NULL;
        char ib[24];
        if (!inm) { snprintf(ib, sizeof ib, "EX_#%d", in ? (int)in->kind : -1); inm = ib; }
        char buf[160];
        snprintf(buf, sizeof(buf), "unsupported form: EX_REINTERPRET %s -> %s of %s",
                 typekind_to_string(e->as.reinterpret_.source_kind),
                 typekind_to_string(e->as.reinterpret_.target_kind), inm);
        t->as.unsupported.why = arena_strdup(b->a, buf, strlen(buf));
    } else if (nm) {
        char buf[64];
        snprintf(buf, sizeof(buf), "unsupported form: %s", nm);
        t->as.unsupported.why = arena_strdup(b->a, buf, strlen(buf));
    } else if (e) {
        char buf[48];
        snprintf(buf, sizeof(buf), "unsupported form: EX_#%d", (int)e->kind);
        t->as.unsupported.why = arena_strdup(b->a, buf, strlen(buf));
    } else {
        t->as.unsupported.why = "unsupported form: null";
    }
    return t;
}

/* A fresh binder's id is drawn from its own range, above every Binding id: the
 * two meet in the capture analysis's bound sets (emit_cps_ir.c keys both on
 * `id`), and a source binder whose id happened to equal a fresh one's was
 * taken as bound where it was free -- a handler's `k_7` missing from the env
 * of a reopen frame whose slot was `__t7`.  That only stayed hidden while a
 * program's binding ids all sat above the stdlib's thousands, which
 * r7rs-programs-compile-slowly's separate stdlib id range undid.  The name
 * keeps the small per-function number. */
#define CPS_FRESH_ID_BASE 0x80000000u

/* How many fresh binders the last cps_ir_translate_fn minted (see
 * cps_ir_last_fresh_count). */
static uint32_t g_cps_last_fresh;

static CVar fresh_cvar(CpsB *b, const Type *ty) {
    CVar v;
    uint32_t n = b->counter++;
    v.id = CPS_FRESH_ID_BASE + n;
    char buf[24];
    /* `__`-reserved so a synthesized result temporary can never collide with a
     * user identifier (globals are not name-guarded like params are -- a `t<N>`
     * form shadowed a user fn/global `t0` referenced from a colored context and
     * segfaulted).  The reader/param guard treat `__`-prefixed names as
     * off-limits for user code. */
    snprintf(buf, sizeof(buf), "__t%u", n);
    v.name = arena_strdup(b->a, buf, strlen(buf));
    v.ty = ty ? ty->kind : TY_UNKNOWN;
    v.type = ty;
    v.bind = NULL;
    return v;
}

static CVar cvar_of_binding(const Binding *bd) {
    CVar v;
    v.id = bd->id;
    v.name = (bd->name ? bd->name->name : "_");
    v.ty = bd->type.kind;
    v.type = &bd->type;
    v.bind = bd;
    return v;
}

static CKont kont_var(CVar j) {
    CKont k; k.kind = KK_VAR; k.id = j.id; k.ty = j.ty; return k;
}

static CKont kont_prompt(TypeKind ty) {
    CKont k; k.kind = KK_PROMPT; k.id = 0; k.ty = ty; return k;
}

/* cps-while-native: the synthesized loop helper's own return continuation. */
static CKont kont_ret(TypeKind ty) {
    CKont k; k.kind = KK_RET; k.id = 0; k.ty = ty; return k;
}

/* ---- atoms ------------------------------------------------------------ */

/* Type ascription `(:: e T)` is erased at codegen (its value is the inner
 * expression's value), so peel it everywhere the translator inspects a form. */
static const Expr *ascribe_peel(const Expr *e) {
    /* generic-call-result-in-generic-collapses-to-int: a generic call whose
     * result is the enclosing signature's own tyvar is wrapped in a reinterpret
     * typed `A` (elab_call.c).  The direct emitter lowers it per clone; to this
     * pass it is the call it wraps, exactly what it saw before the wrap. */
    while (e && (e->kind == EX_ASCRIBE ||
                 (e->kind == EX_REINTERPRET &&
                  e->as.reinterpret_.target_kind == TY_TYVAR)))
        e = e->kind == EX_ASCRIBE ? e->as.ascribe_.inner
                                  : e->as.reinterpret_.expr;
    return e;
}

/* A Tier A scalar kind: an integer/bool/pointer that occupies its C width with a
 * direct bit pattern.  A same-size reinterpret between two such kinds (e.g. the
 * `int -> uint64` the frontend emits for `(:: 10 :uint64)`, since both are 64
 * bits) is bit-identical, so it is a value-preserving retype the CPS translator
 * can see through.  Floats are excluded -- an int<->double reinterpret changes
 * bit meaning and is a Tier B concern, left to the fallback. */
static bool tierA_scalar_kind(TypeKind k) {
    switch (k) {
        case TY_INT: case TY_INT8: case TY_INT16: case TY_INT32: case TY_INT64:
        case TY_UINT8: case TY_UINT16: case TY_UINT32: case TY_UINT64:
        case TY_BOOL: case TY_CSTR: case TY_PTR_VOID:
            return true;
        default:
            return false;
    }
}

/* True if `e` is a same-size Tier A reinterpret we can peel like an ascription. */
static bool is_tierA_reinterp(const Expr *e) {
    return e && e->kind == EX_REINTERPRET
        && tierA_scalar_kind(e->as.reinterpret_.source_kind)
        && tierA_scalar_kind(e->as.reinterpret_.target_kind);
}

static bool is_atomic(const Expr *e) {
    e = ascribe_peel(e);
    if (!e) return false;
    if (is_tierA_reinterp(e)) return is_atomic(e->as.reinterpret_.expr);
    switch (e->kind) {
        case EX_INT_LIT:
        case EX_BOOL_LIT:
        case EX_NIL_LIT:
        case EX_FLOAT_LIT:
        case EX_CSTR_LIT:
        case EX_VAR:
            return true;
        default:
            return false;
    }
}

static CAtom atom_of(const Expr *e) {
    e = ascribe_peel(e);
    CAtom a; memset(&a, 0, sizeof(a));
    a.ty = e ? e->type.kind : TY_UNKNOWN;
    a.type = e ? &e->type : NULL;
    if (!e) { a.kind = CA_UNIT; return a; }
    /* A Tier A reinterpret is a bit-identical retype: take the inner atom's
     * value but keep the reinterpret's (retyped) result type. */
    if (is_tierA_reinterp(e)) {
        TypeKind tgt = e->as.reinterpret_.target_kind;
        CAtom inner = atom_of(e->as.reinterpret_.expr);
        inner.ty = tgt;
        return inner;
    }
    switch (e->kind) {
        case EX_INT_LIT:   a.kind = CA_INT;  a.i = e->as.i; break;
        case EX_BOOL_LIT:  a.kind = CA_BOOL; a.b = e->as.b; break;
        case EX_NIL_LIT:   a.kind = CA_UNIT; break;
        case EX_FLOAT_LIT: a.kind = CA_FLOAT; a.f = e->as.f; break;
        case EX_CSTR_LIT:  a.kind = CA_STR;  a.str = e->as.s; break;
        case EX_VAR:       a.kind = CA_VAR;  a.var = e->as.var.binding; break;
        default:           a.kind = CA_OTHER; break;
    }
    return a;
}

static CAtom atom_cvar(CVar v) {
    CAtom a; memset(&a, 0, sizeof(a));
    a.kind = CA_CVAR; a.ty = v.ty; a.type = v.type; a.cvar_id = v.id; a.cvar_name = v.name;
    return a;
}

/* ---- colored-callee lookup -------------------------------------------- */

/* True if `fn` resolves to a colored top-level function -- including a function
 * that is a MEMBER of a top-level `(defmodule ...)` (its FnDef lives in the
 * module body, never as a top-level program item).  Without the module descent
 * a colored module member reads as uncolored, so a caller in the same module
 * emits a DIRECT (unthreaded) call instead of the DK-threaded `__cps` call, and
 * an effect performed in the callee escapes the caller's handler. */
/* r7rs-conformance-program-emits-megabytes-of-c: asked for every call the
 * translation meets (callee_colored, safe_to_delegate), each answer a scan of
 * the whole program.  A binding -> FnDef table, rebuilt when the program or
 * its item count changes -- the same policy as emit_cps_ir.c's
 * fd_for_binding -- holding the FIRST match the scan below finds. */
static const Expr     *cfd_prog;
static uint32_t        cfd_np, cfd_cap;
static const Binding **cfd_keys;
static const FnDef   **cfd_vals;

static uint32_t cfd_slot(const Binding *k) {
    uint64_t x = (uint64_t)(uintptr_t)k;
    x ^= x >> 33; x *= 0xff51afd7ed558ccdULL; x ^= x >> 33;
    return (uint32_t)x & (cfd_cap - 1);
}
static void cfd_put(const Binding *k, const FnDef *fd) {
    uint32_t h = cfd_slot(k);
    while (cfd_keys[h] && cfd_keys[h] != k) h = (h + 1) & (cfd_cap - 1);
    if (!cfd_keys[h]) { cfd_keys[h] = k; cfd_vals[h] = fd; }
}
static void cfd_build(const Expr *program) {
    uint32_t np = program->as.program.n, n = 0;
    for (uint32_t i = 0; i < np; i++) {
        Expr *it = program->as.program.items[i];
        if (!it) continue;
        if (it->kind == EX_FN_DEF) n++;
        else if (it->kind == EX_DEFMODULE && it->as.defmodule_.mod) n += it->as.defmodule_.mod->n_body;
    }
    uint32_t cap = 16;
    while (cap < 2 * n + 2) cap <<= 1;
    free((void *)cfd_keys); free((void *)cfd_vals);
    cfd_keys = (const Binding **)calloc(cap, sizeof *cfd_keys);
    cfd_vals = (const FnDef **)calloc(cap, sizeof *cfd_vals);
    cfd_prog = NULL;
    if (!cfd_keys || !cfd_vals) {
        free((void *)cfd_keys); free((void *)cfd_vals);
        cfd_keys = NULL; cfd_vals = NULL; cfd_cap = 0;
        return;   /* OOM: callee_fndef scans */
    }
    cfd_cap = cap;
    for (uint32_t i = 0; i < np; i++) {
        Expr *it = program->as.program.items[i];
        if (!it) continue;
        if (it->kind == EX_FN_DEF && it->as.fn_def_.fn && it->as.fn_def_.fn->binding)
            cfd_put(it->as.fn_def_.fn->binding, it->as.fn_def_.fn);
        if (it->kind == EX_DEFMODULE && it->as.defmodule_.mod) {
            DefModule *m = it->as.defmodule_.mod;
            for (uint32_t j = 0; j < m->n_body; j++) {
                Expr *mb = m->body[j];
                if (mb && mb->kind == EX_FN_DEF && mb->as.fn_def_.fn && mb->as.fn_def_.fn->binding)
                    cfd_put(mb->as.fn_def_.fn->binding, mb->as.fn_def_.fn);
            }
        }
    }
    cfd_prog = program;
    cfd_np = np;
}

void cps_ir_callee_cache_reset(void) { cfd_prog = NULL; }

static const FnDef *callee_fndef(CpsB *b, const Binding *fn) {
    if (!fn || !b->program || b->program->kind != EX_PROGRAM) return NULL;
    if (b->program != cfd_prog || b->program->as.program.n != cfd_np || !cfd_keys)
        cfd_build(b->program);
    if (cfd_keys) {
        uint32_t h = cfd_slot(fn);
        while (cfd_keys[h]) {
            if (cfd_keys[h] == fn) return cfd_vals[h];
            h = (h + 1) & (cfd_cap - 1);
        }
        return NULL;
    }
    for (uint32_t i = 0; i < b->program->as.program.n; i++) {
        Expr *it = b->program->as.program.items[i];
        if (!it) continue;
        if (it->kind == EX_FN_DEF && it->as.fn_def_.fn &&
            it->as.fn_def_.fn->binding == fn)
            return it->as.fn_def_.fn;
        if (it->kind == EX_DEFMODULE && it->as.defmodule_.mod) {
            DefModule *m = it->as.defmodule_.mod;
            for (uint32_t j = 0; j < m->n_body; j++) {
                Expr *mb = m->body[j];
                if (mb && mb->kind == EX_FN_DEF && mb->as.fn_def_.fn &&
                    mb->as.fn_def_.fn->binding == fn)
                    return mb->as.fn_def_.fn;
            }
        }
    }
    return NULL;
}

static bool callee_colored(CpsB *b, const Binding *fn) {
    const FnDef *fd = callee_fndef(b, fn);
    return fd && fd->cps_colored;
}

/* serial-shift-colored-receiver-rejected: may a perform inside this function
 * reach a handler OUTSIDE it?  True when its declared or inferred effect row is
 * non-empty (or unknown -- no FnDef, or a row effect_check never inferred).
 *
 * A marshal-reset receiver is called from the per-site shift-body helper as a
 * plain C function, i.e. through a colored function's DIRECT-ENTRY wrapper,
 * which runs its __cps body under a fresh DK root.  That is exactly how every
 * other effect-free colored function is called (a `helper` calling `apply1`
 * through a fn value colors both, and the call is still direct), and it is
 * sound for the same reason: a function with an empty row performs nothing
 * that escapes it, so the fresh root is never asked to handle anything.  Only
 * an effect that ESCAPES the receiver would need the handler enclosing the
 * reset -- that is the case the restriction exists for, and it stays refused. */
static bool fn_effect_may_escape(CpsB *b, const Binding *fn) {
    const FnDef *fd = callee_fndef(b, fn);
    if (!fd) return true;
    /* Runtime-pure, not merely empty: a capability tag (IO from a `println`)
     * is never performed, so it cannot escape (effect-row-honesty W4). */
    if (fd->binding && fd->binding->type.kind == TY_FN
        && !effect_row_is_runtime_pure(fd->binding->type.as.fn.effect_row))
        return true;
    if (!fd->inferred_effect_row) return true;   /* never inferred: be safe */
    return !effect_row_is_runtime_pure(fd->inferred_effect_row);
}

/* ---- pending bindings (drives atomization order) ---------------------- */

/* reap_ok: the item is a call argument whose callee slot does not retain it
 * (call_slot_nonretaining), so a capturing closure bound here is dead once the
 * call returns and its env may be reaped at the entry boundary. */
typedef struct { Expr *expr; CVar x; bool reap_ok; } PendItem;
typedef struct { PendItem items[32]; uint32_t n; } Pending;

/* forward decls */
static CTerm *cps_tail(CpsB *b, Expr *e, CKont kont);
static CTerm *cps_bind(CpsB *b, Expr *e, CVar x, CTerm *rest);
static CTerm *cps_bind_reinterp(CpsB *b, Expr *e, CVar x, CTerm *rest);
static CTerm *cps_bind_cast(CpsB *b, Expr *e, CVar x, CTerm *rest);
static CTerm *build_letraw(CpsB *b, Expr *e, CVar x, CTerm *rest);

/* An owning-value operation on a local rc handle (`rc/of`, `rc/clone`, `rc/drop`,
 * `rc/strong-count`, `rc->ptr`).  These do not cross a DK slot -- the rc stays a
 * local -- so the CPS backend delegates their emission to the direct emitter
 * (`emit_value`), which already carries the correct control-block / refcount /
 * drop-glue discipline.  A form whose operand is atomic (a var or literal) is
 * always delegated; O1-a widens this to a non-atomic operand that is provably
 * free of control operators (`operand_uses_control` below), so no control
 * operator (`perform` / `shift`) can hide in an operand and get emitted in
 * direct style inside a CPS function. */
static const Expr *owning_operand(const Expr *e) {
    if (!e) return NULL;
    switch (e->kind) {
        case EX_RC_OF:    return e->as.rc_of_.expr;
        case EX_RC_CLONE: return e->as.rc_clone_.expr;
        case EX_RC_DROP:  return e->as.rc_drop_.expr;
        case EX_RC_COUNT: return e->as.rc_count_.expr;
        case EX_RC_PTR:   return e->as.rc_ptr_.expr;
        default:          return NULL;
    }
}

/* O1-a: does `e` (an owning-op operand delegated whole to the direct emitter)
 * MIGHT contain a control operator?  A delegated operand is emitted in direct
 * style inside a CPS function, so any control operator lexically inside it would
 * be direct-emitted where the DK-threaded discipline expects a CPS lowering --
 * unsound.  This mirrors the control-op SEED set and structural recursion of
 * `cps_directly_uses_control` (src/passes/cps.c) but INVERTS the default: a node
 * kind this scan does not positively recognize as control-free is treated as if
 * it might hide a control op (return true -> not delegatable), so a missed
 * control op falls back rather than delegating.  Nested fn / closure bodies are
 * call-graph boundaries -- a control op inside one is colored on its own merits
 * and never emitted at this position -- so they contribute no control op here,
 * exactly as the coloring scan treats them. */
static bool operand_uses_control(const Expr *e) {
    e = ascribe_peel(e);
    if (!e) return false;
    if (is_atomic(e)) return false;
    switch (e->kind) {
        /* Control-op seeds (mirror cps_directly_uses_control). */
        case EX_RESET:
        case EX_SHIFT:
        case EX_SHIFT0:
        case EX_CLONEABLE_RESET:
        case EX_CLONEABLE_SHIFT:
        case EX_SERIAL_RESET:
        case EX_SERIAL_SHIFT:
        case EX_PERFORM:
        case EX_HANDLE:
        case EX_RESUME:
        case EX_DISCONTINUE:
        case EX_CALLCC:
            return true;
        /* Structural recursion into value-position children. */
        case EX_LET:
            for (uint32_t i = 0; i < e->as.let_.n; i++)
                if (operand_uses_control(e->as.let_.bindings[i].init)) return true;
            return operand_uses_control(e->as.let_.body);
        case EX_IF:
            return operand_uses_control(e->as.if_.cond)
                || operand_uses_control(e->as.if_.then_)
                || operand_uses_control(e->as.if_.else_or_null);
        case EX_DO:
            for (uint32_t i = 0; i < e->as.do_.n; i++)
                if (operand_uses_control(e->as.do_.items[i])) return true;
            return false;
        case EX_BUILTIN:
            for (uint32_t i = 0; i < e->as.builtin.n; i++)
                if (operand_uses_control(e->as.builtin.args[i])) return true;
            return false;
        case EX_CALL:
            if (operand_uses_control(e->as.call_.fn_expr)) return true;
            for (uint32_t i = 0; i < e->as.call_.n_args; i++)
                if (operand_uses_control(e->as.call_.args[i])) return true;
            return false;
        case EX_GET_FIELD:
            return operand_uses_control(e->as.get_field_.struct_expr);
        case EX_MAKE_STRUCT:
            for (uint32_t i = 0; i < e->as.make_struct_.n_fields; i++)
                if (operand_uses_control(e->as.make_struct_.field_values[i])) return true;
            return false;
        case EX_DEFAULT_OF:
            return false;
        case EX_RC_OF:    return operand_uses_control(e->as.rc_of_.expr);
        case EX_RC_CLONE: return operand_uses_control(e->as.rc_clone_.expr);
        case EX_RC_DROP:  return operand_uses_control(e->as.rc_drop_.expr);
        case EX_RC_COUNT: return operand_uses_control(e->as.rc_count_.expr);
        case EX_RC_PTR:   return operand_uses_control(e->as.rc_ptr_.expr);
        case EX_REINTERPRET:
            return operand_uses_control(e->as.reinterpret_.expr);
        /* The `any` widen / readers carry no control op of their own; only their
         * operand can hide one. */
        case EX_UNION_INJECT: return operand_uses_control(e->as.union_inject_.value);
        /* saffron: the dynamic nodes carry no control op of their own either --
         * only their operands can.  Without these arms they took the
         * conservative `default: return true`, so a control-FREE `(println x)`
         * read as control-bearing and evicted every function it appeared in --
         * and a Saffron program is mostly these nodes. */
        case EX_DYN_OP:
            for (uint32_t i = 0; i < e->as.dyn_op_.n_args; i++)
                if (operand_uses_control(e->as.dyn_op_.args[i])) return true;
            return false;
        case EX_DYN_CALL:
            if (operand_uses_control(e->as.dyn_call_.fn)) return true;
            for (uint32_t i = 0; i < e->as.dyn_call_.n_args; i++)
                if (operand_uses_control(e->as.dyn_call_.args[i])) return true;
            return false;
        case EX_DYN_FIELD:    return operand_uses_control(e->as.dyn_field_.obj);
        case EX_DYN_METHOD:
            if (operand_uses_control(e->as.dyn_method_.obj)) return true;
            for (uint32_t i = 0; i < e->as.dyn_method_.n_args; i++)
                if (operand_uses_control(e->as.dyn_method_.args[i])) return true;
            return false;
        case EX_ANY_TYPE_OF:  return operand_uses_control(e->as.any_type_of_.value);
        case EX_ANY_IS:       return operand_uses_control(e->as.any_is_.value);
        case EX_ANY_CAST:     return operand_uses_control(e->as.any_cast_.value);
        /* mutual-tail-call-through-guard-grows-the-stack: a quoted symbol is the
         * address of a static record -- a literal, with nothing under it. */
        case EX_SYM_LIT:      return false;
        /* Nested fn boundaries: a control op inside is emitted elsewhere. */
        case EX_FN_DEF:
        case EX_FN:
        case EX_CLOSURE:
            return false;
        /* Conservative default: an un-modeled node kind might hide a control op
         * -- do not delegate. */
        default:
            return true;
    }
}

static bool is_delegatable_owning(const Expr *e) {
    const Expr *arg = owning_operand(e);
    if (!arg) return false;
    if (is_atomic(arg)) return true;
    /* O1-a: a non-atomic operand delegates only when provably control-free.  The
     * straight-line-drop-before-control invariant on the resulting node is still
     * enforced by owning_dropped_before_control (letraw_ok, emit_cps_ir.c). */
    return !operand_uses_control(arg);
}

/* A struct/ADT value op that stays a local -- construct (make-struct), read a
 * field (.field), or a default value.  Like the owning ops, these do not need
 * CPS lowering: the aggregate is a local (never crosses the DK slot -- a crossing
 * use is rejected by atom_ok / fn_sig_ok), so emission is delegated to the direct
 * emitter.  Delegated only when every operand is atomic, so no control operator
 * hides in an operand. */
static bool is_delegatable_struct(const Expr *e) {
    if (!e) return false;
    switch (e->kind) {
        case EX_GET_FIELD:
            return is_atomic(e->as.get_field_.struct_expr);
        case EX_MAKE_STRUCT:
            for (uint32_t i = 0; i < e->as.make_struct_.n_fields; i++)
                if (!is_atomic(e->as.make_struct_.field_values[i])) return false;
            return true;
        case EX_DEFAULT_OF:
            return true;
        /* perform-in-fn-with-any-param-has-no-cps-lowering: the `any` widen and
         * the three `any` readers are value ops of exactly this shape -- one
         * operand, no control operator, a result the direct emitter already
         * knows how to spell (`TUR_TAG(...)`, a tag compare, a deref).  Left
         * out they translated to CT_UNSUPPORTED, which evicted the enclosing
         * function; a `handle` in it then had no CPS lowering, and its
         * `perform` reached the direct emitter, which has none either.  That is
         * why `(with-any 3)` in main -- a widen, nothing more -- took the whole
         * program off the DK backend.
         *
         * These take the WIDER operand rule is_delegatable_owning uses -- atomic
         * OR provably control-free -- rather than the atomic-only rule the
         * struct ops above kept.  Atomic-only is not enough for the shape that
         * motivates this: `(f (make-struct Pt 1 2))` widens a CONSTRUCTOR CALL
         * to `any`, so the operand is never atomic and the caller evicted even
         * after the widen itself was admitted.  Control-freedom is the property
         * that actually matters for handing the whole subtree to the direct
         * emitter, and it is the same soundness posture the owning ops rely on.
         */
        case EX_UNION_INJECT:
            return is_atomic(e->as.union_inject_.value)
                || !operand_uses_control(e->as.union_inject_.value);
        /* saffron: same admission rule as the widen above, for the same reason.
         * A dynamic op is an ordinary direct-emitted call once nothing under it
         * threads the DK, and the elaborator hoists a control-bearing operand
         * into a let (elab_coerce_to_any / elab_dyn_hoist_control) so the
         * operand it sees here is a variable. */
        case EX_DYN_OP:
        case EX_DYN_CALL:
        case EX_DYN_FIELD:
        case EX_DYN_METHOD:
            return !operand_uses_control(e);
        case EX_ANY_TYPE_OF:
            return is_atomic(e->as.any_type_of_.value)
                || !operand_uses_control(e->as.any_type_of_.value);
        case EX_ANY_IS:
            return is_atomic(e->as.any_is_.value)
                || !operand_uses_control(e->as.any_is_.value);
        case EX_ANY_CAST:
            /* A checked downcast can panic, which is a plain direct-path abort,
             * not a control operator the DK machine has to thread. */
            return is_atomic(e->as.any_cast_.value)
                || !operand_uses_control(e->as.any_cast_.value);
        default:
            return false;
    }
}

/* WHOLE-BODY delegation mode: while checking whether an ENTIRE colour-only
 * function body can be handed to the direct emitter as one CT_LETRAW (see
 * whole_body_delegatable / cps_ir_translate_fn), a CAPTURING closure IS a
 * delegatable value -- the whole `let` is emitted by emit_value(EX_LET), which
 * heap-allocates the closure env AND frees it at scope exit via
 * let_binding_env_freeable (the direct emitter's scoped-env free), so no leaf
 * CT_LETRAW leaks.  This flag NEVER leaks into the per-node cps_tail/cps_bind
 * path (where admitting a capturing closure as a lone leaf WOULD leak); it is set
 * only around the whole-body probe. */
static bool g_whole_body_delegate;

/* P5's handled-effect stack (g_wbd_handled / WBD_MAX_HANDLED / the
 * wbd_effect_handled + row_*_all_wbd_handled predicates) lived here.
 *
 * It let a `handle` be whole-body-delegated to the direct/fiber emitter, and a
 * `perform` or `resume` ride along when the enclosing delegated handle
 * discharged the effect.  The EX_HANDLE case below was the only thing that ever
 * pushed onto the stack, and it has returned false unconditionally since
 * cps-tramp-resume graduated (2026-07-19) -- a deep handle must DK-lower, which
 * is what dissolves the handler-installer's base taint.  With no writer the
 * stack was permanently empty, so every predicate reading it answered "no" and
 * every path guarded by `g_wbd_n_handled > 0` was unreachable.
 *
 * Verified before removal, two ways: by induction on the assignments (the only
 * ones were `= 0` and a restore of a value read from the variable), and
 * empirically -- a probe on every read site, compiled across all 2119 fixtures,
 * never fired.
 *
 * `g_whole_body_delegate` itself is NOT dead and stays: the closure-only
 * whole-body delegation it guards (a function "colored" only because it builds
 * a capturing closure, delegated so the direct emitter's scoped-env free
 * applies) never needed the effect stack.
 *
 * The helpers that existed only to serve the stack -- fndef_of_binding and
 * expr_fn_effect_row -- went with it; their last live callers were on these
 * paths. */

/* Is a call to colored GLOBAL callee `fn` (the EX_CALL `e`) safe to delegate to
 * the direct emitter?
 *
 * Only one shape is left, and it has nothing to do with handles: a SELF-recursive
 * call in a LEAF-FIBER function (one whose body indirect-calls a fn-value, so it
 * is permanently fiber and evicts anyway) has the same classification as the
 * function itself -- if the rest of the body whole-body-delegates, the self-call
 * is fiber-to-fiber.  Admitting it lets a leaf-fiber HOF
 * (`map-list = (+ (f n) (map-list (- n 1) f))`) whole-body-delegate its recursion
 * instead of evicting; it was already on the direct path, so no codegen moves.
 * The leaf-fiber gate is essential: without it a normal recursive function, which
 * should CPS-emit natively, would be wrongly routed onto the direct path.
 *
 * The two shapes that used to live here -- a CONCRETE-row callee whose every
 * effect was in g_wbd_handled, and a ROW-VARIABLE callee whose fn-value arguments
 * were all handled -- both required an enclosing DELEGATED handle.  No handle has
 * been delegated since cps-tramp-resume graduated, so both were unreachable and
 * went with the effect stack (see its note above), taking fndef_of_binding and
 * expr_fn_effect_row with them. */
static bool colored_call_wbd_delegatable(CpsB *b, const Expr *e) {
    const Binding *fn = e->as.call_.fn_binding;
    return fn && b->cur_fn && fn == b->cur_fn && b->cur_fn_leaf_fiber;
}

/* A capture-free fn-value wrapper -- a bare fn coerced to a fat/poly callable, or
 * a closure literal with no captured free variables.  Such a value is not a call
 * and not a control op, and (being capture-free) references no enclosing local,
 * so it is safe to delegate to the direct emitter via CT_LETRAW anywhere,
 * including inside a lifted zero-capture body.  A CAPTURING closure is NOT
 * delegated here: its free vars would need the has_capture cut to see them, so it
 * falls back conservatively.  The wrapped fn body is a call-graph boundary
 * (colored on its own merits), never executed at this position. (N6.1) */
static bool is_delegatable_value(const Expr *e) {
    if (!e) return false;
    switch (e->kind) {
        case EX_FN_TO_FAT:
        case EX_POLY_WRAP:
        case EX_FN:
            return true;
        /* mutual-tail-call-through-guard-grows-the-stack: a quoted symbol --
         * `'done`, a `#lang r7rs` base case -- is a literal the direct emitter
         * spells as its record's address.  Unlisted, it translated to
         * CT_UNSUPPORTED and evicted the whole function: `(define (g1 f n) (if
         * (= n 0) 'g-done (g2 f (- n 1))))` stayed direct, so its tail call into
         * a CPS partner could never join a CPS tail-call group. */
        case EX_SYM_LIT:
            return true;
        case EX_CLOSURE:
            /* A capture-free closure is always delegatable.  A CAPTURING closure
             * is delegatable ONLY when it is a cross-function `shift` receiver
             * (is_shift_receiver, set at the __Shift desugar): it is invoked
             * exclusively as `(recv k)` in the bridge-wrapping __Shift handler
             * case, never as a general indirect callee, so the has_capture cut /
             * indirect-callee hazard does not apply.  collect_caps walks its
             * scalar (Copy) captures into the lifted env; a non-Copy capture bails
             * to fallback.  See docs/archive/cps-native-handle-in-reset-plan.md.
             *
             * Admitting a GENERAL capturing closure as a plain value (the Phase-1
             * keystone) is functionally correct here but leaks its fat-closure env
             * on the CPS delegation path (the direct emitter's scoped-env free is
             * not applied); it must land WITH the Phase-3 escaping-fat-closure-env
             * free, not before.  See the cps-runtime-finish-plan.md Progress log. */
            return e->as.closure_.closure
                && (e->as.closure_.closure->n_captures == 0
                    || e->as.closure_.closure->is_shift_receiver
                    || e->as.closure_.closure->is_effect_payload
                    || g_whole_body_delegate);
        default:
            return false;
    }
}

/* A CAPTURING closure is NOT a bare "direct entry point" value.  The direct
 * emitter's indirect-call path (emit_expr.c, the `fn_expr` block ~2950) casts
 * the callee VALUE to a bare function pointer and calls it -- but a fat
 * closure's value is its ENV pointer, so the cast jumps into the heap-allocated
 * env struct as if it were code (segfault).  Only a bare fn / 0-capture closure
 * / fn-pointer value is a valid indirect callee there.  A `shift`/`shift0` whose
 * receiver is a capturing closure lowers via a synthesized `(recv val)` that
 * hits exactly this path (cps_shift_body_kf), so such a shift must EVICT to the
 * direct shift lowering (emit_cps.c), which handles a capturing receiver
 * correctly (verified: continuation-substrate t-deep).  Returns true for a
 * callee the indirect delegation can safely emit. */
static bool indirect_callee_ok(const Expr *fe) {
    return !(fe && fe->kind == EX_CLOSURE && fe->as.closure_.closure
             && fe->as.closure_.closure->n_captures > 0);
}

/* Declared in emit_internal.h; forward-declared here to avoid pulling the whole
 * emitter header into the CPS pass.  Conservative: over-reports escapes (EX_PERFORM
 * and every unmodeled control form default to "escapes"), so a `false` result
 * PROVES the binding does not escape `e`. */
bool closure_binding_escapes(const Expr *e, const Binding *b);
/* An inline-C block in the closure's body names one of its captures (an
 * lvalue into the env box); defined emit-side (emit_core.c). */
bool closure_body_inline_c_touches_env(const struct Closure *c);

/* Is let-binding `idx` of `let` a capturing closure whose heap fat-env may be
 * freed when the closure dies?  Mirrors the direct emitter's
 * let_binding_env_freeable (emit_expr.c), but for a closure that the CPS backend
 * leaf-admits (delegates via CT_LETRAW) in an effect-bearing body -- where the
 * direct emitter's let-scope free does NOT run, so the env would otherwise leak.
 * Sound iff:
 *   - the initializer is a CAPTURING EX_CLOSURE (a capture-free one is already a
 *     delegatable value; a __Shift receiver is reaped by the handler-case path,
 *     P3.d -- exclude it so its env is never reaped twice),
 *   - the closure returns a SCALAR (its result can never alias the env), and
 *   - the bound name does not escape the let body or any sibling initializer
 *     (closure_binding_escapes is conservative, so a false negative merely keeps
 *     the status-quo leak; it never frees a still-live env). */
static bool cps_closure_env_freeable(const Expr *let, uint32_t idx) {
    const Expr *init = ascribe_peel(let->as.let_.bindings[idx].init);
    const Binding *b = let->as.let_.bindings[idx].binding;
    if (!init || !b) return false;
    if (init->kind != EX_CLOSURE || !init->as.closure_.closure) return false;
    if (init->as.closure_.closure->n_captures == 0) return false;   /* capture-free: already delegatable */
    if (init->as.closure_.closure->is_shift_receiver) return false; /* reaped by P3.d */
    if (init->as.closure_.closure->is_effect_payload) return false; /* reaped at handler case */
    if (b->type.kind != TY_FN) return false;
    switch (b->type.as.fn.result_kind) {
        case TY_INT: case TY_FLOAT: case TY_FLOAT32: case TY_FLOAT64:
        case TY_BOOL: case TY_NIL: break;
        /* As the direct emitter's let_binding_env_freeable (RM1): a by-value
         * aggregate is copied out, a carrier box is a malloc of its own, and a
         * cstr points at characters -- none points into the env unless the
         * body took its address, which only an inline C block naming a
         * capture can do (closure_body_inline_c_touches_env; the conservative
         * expr_subtree_has_inline_c reads a `perform` as possible inline C). */
        case TY_APP: case TY_ADT: case TY_CSTR:
            if (!init->as.closure_.closure->fn
                || closure_body_inline_c_touches_env(init->as.closure_.closure))
                return false;
            break;
        default: return false;   /* non-scalar result may alias the env */
    }
    if (closure_binding_escapes(let->as.let_.body, b)) return false;
    for (uint32_t j = 0; j < let->as.let_.n; j++) {
        if (j == idx) continue;
        if (closure_binding_escapes(let->as.let_.bindings[j].init, b)) return false;
    }
    return true;
}

/* Bind let-binding `idx`'s init to `bx`, then run `rest`.  A freeable capturing
 * closure (cps_closure_env_freeable) is leaf-admitted via CT_LETRAW with
 * reap_env set -- so the direct emitter builds its heap fat-env and emit_letraw
 * registers that env for a single-node free at the DK entry boundary, closing
 * the leak that made a general leaf-admitted closure unsound on the CPS path.
 * Everything else goes through cps_bind unchanged. */
/* dynamic-returned-closure-env-is-never-freed: the `any` escape walks, defined
 * emit-side (emit_core.c). */
bool any_box_binding_escapes(const Expr *e, const Binding *b);
bool any_box_binding_escapes_self_apply(const Expr *e, const Binding *b,
                                        uint32_t self_mask);
bool expr_is_fresh_any_closure(const Expr *x);

/* dynamic-returned-closure-env-is-never-freed (the CPS path): is let-binding
 * `idx` an `any` holding a fresh capturing closure (expr_is_fresh_any_closure:
 * a widened lambda -- the __borrowc hoist of cps-capturing-closure-env-leaks-
 * through-dyn-call -- or a call whose callee returns one), and does the name
 * stay inside the let?  The direct emitter drops such a binding at the let's
 * scope exit; a colored function's let is flattened into CPS binders and has
 * no scope exit to put a drop at, so the env is reaped at the outermost DK
 * entry boundary instead, exactly as reap_env does for a typed capturing
 * closure.  Same escape facts as the direct path (let_binding_any_freeable),
 * self application included.
 *
 * The one hazard the boundary adds is a proper-tail-calls T6 BOUNCE: a
 * dynamic tail call made where `__kont` is the trampoline's root hands its
 * callee back to the driver, which invokes it after the entry -- and its reap
 * -- are over.  The emitter guards the registration on exactly that
 * condition (emit_letraw, reap_any_env). */
static bool cps_any_closure_env_freeable(const Expr *let, uint32_t idx) {
    const Expr *init = ascribe_peel(let->as.let_.bindings[idx].init);
    const Binding *b = let->as.let_.bindings[idx].binding;
    if (!init || !b || b->type.kind != TY_ANY) return false;
    if (!expr_is_fresh_any_closure(init)) return false;
    uint32_t self_mask = init->kind == EX_CALL
        ? init->as.call_.fn_binding->fresh_closure_self_apply_mask : 0;
    if (any_box_binding_escapes_self_apply(let->as.let_.body, b, self_mask))
        return false;
    for (uint32_t j = 0; j < let->as.let_.n; j++) {
        if (j == idx) continue;
        if (any_box_binding_escapes(let->as.let_.bindings[j].init, b)) return false;
    }
    return true;
}

/* closure-let-in-self-tail-loop-leaks: the other shape the direct emitter
 * drops at a let's scope exit (let_binding_env_freeable) -- a call to a
 * `returns_fresh_closure` function, the make-scaler `(let [f (mk i)] ...)`.
 * Every such call mallocs a fresh, uniquely owned env with scalar captures and
 * a scalar result (the inference checked both), so the only question is the
 * same escape walk.  On the CPS path the let has no scope exit, so the call
 * was bound and the env never freed at all: 24 B a turn of a loop.  Only a
 * call the direct emitter may take whole qualifies -- a colored callee keeps
 * its continuation threading. */
static bool safe_to_delegate(CpsB *b, const Expr *e);   /* fwd (defined below) */
static bool cps_fresh_call_env_freeable(CpsB *b, const Expr *let, uint32_t idx) {
    const Expr *init = ascribe_peel(let->as.let_.bindings[idx].init);
    const Binding *bd = let->as.let_.bindings[idx].binding;
    if (!init || !bd || init->kind != EX_CALL) return false;
    if (!init->as.call_.fn_binding || !init->as.call_.fn_binding->returns_fresh_closure)
        return false;
    if (!safe_to_delegate(b, let->as.let_.bindings[idx].init)) return false;
    if (closure_binding_escapes(let->as.let_.body, bd)) return false;
    for (uint32_t j = 0; j < let->as.let_.n; j++) {
        if (j == idx) continue;
        if (closure_binding_escapes(let->as.let_.bindings[j].init, bd)) return false;
    }
    return true;
}

bool closure_binding_only_invoked(const Expr *e, const Binding *b);

/* closure-let-in-self-tail-loop-leaks: may a backedge free this binder (see
 * letraw.reap_at_backedge)?  Every use in the let -- body and sibling inits --
 * must be an invocation. */
static bool cps_closure_only_invoked(const Expr *let, uint32_t idx) {
    const Binding *bd = let->as.let_.bindings[idx].binding;
    if (!bd || !closure_binding_only_invoked(let->as.let_.body, bd)) return false;
    for (uint32_t j = 0; j < let->as.let_.n; j++) {
        if (j == idx) continue;
        if (!closure_binding_only_invoked(let->as.let_.bindings[j].init, bd)) return false;
    }
    return true;
}

/* A local closure only ever called in place (cps_ir_let_local_closure) is
 * reaped at the DK entry boundary like a freeable one: every use is a
 * saturated call, so the box never leaves the let (a sibling or body `perform`,
 * which closure_binding_escapes must read as an escape, cannot hand it
 * anywhere).  The predicate itself checks that the reap frees nothing the
 * result can point into. */
static const Binding *local_closure_shape(const Expr *let, uint32_t i);   /* fwd */
static bool cps_local_closure_env_freeable(const Expr *let, uint32_t idx) {
    return local_closure_shape(let, idx) != NULL;
}

static CTerm *cps_bind_let_init(CpsB *b, const Expr *let, uint32_t idx, CVar bx, CTerm *rest) {
    Expr *init = (Expr *)let->as.let_.bindings[idx].init;
    if (cps_closure_env_freeable(let, idx) || cps_fresh_call_env_freeable(b, let, idx)
        || cps_local_closure_env_freeable(let, idx)) {
        CTerm *t = build_letraw(b, init, bx, rest);
        t->as.letraw.reap_env = true;
        t->as.letraw.reap_at_backedge = cps_closure_only_invoked(let, idx);
        return t;
    }
    CTerm *t = cps_bind(b, init, bx, rest);
    /* Only when the call was delegated as a raw let: that is the one lowering
     * whose binder holds the `any` the call returned, in this function. */
    if (t && t->kind == CT_LETRAW && t->as.letraw.e == init &&
        t->as.letraw.x.id == bx.id && cps_any_closure_env_freeable(let, idx))
        t->as.letraw.reap_any_env = true;
    return t;
}

/* True if every argument of an EX_CALL is atomic (so the whole call can be
 * delegated to the direct emitter without control ops hiding in an arg). */
/* handle-over-effectful-fn-field-in-arg-let-evicted: the registry-threaded
 * effectful fn-value calls (E2a / E2c) ATOMIZE their arguments -- a non-atomic
 * one becomes a pending CPS binding folded around the call -- so they never
 * needed atomic arguments, only room in the Pending array (32, and atomize
 * silently drops past it; one slot is the E2c field-load callee).  Requiring
 * call_args_atomic evicted `(.run s (let [v 1.25] v))` -- and with it every
 * handler and performer of the effect. */
static bool call_args_pendable(const Expr *e) {
    uint32_t non_atomic = 0;
    for (uint32_t i = 0; i < e->as.call_.n_args; i++)
        if (!is_atomic(e->as.call_.args[i])) non_atomic++;
    return non_atomic + 1 < 32;
}

/* mutual-tail-call-through-guard-grows-the-stack: an argument the direct
 * emitter spells with no call in it -- an atom, a quoted symbol, or either one
 * widened to `any`.  `#lang r7rs`'s `(list 1 2)` is an indirect call whose
 * arguments are `(:: 1 any)` and `(:: 2 any)`; refusing them as non-atomic
 * evicted a base case like `(if (= n 0) (list 1 2) (g2 ...))` and with it the
 * whole function, so its tail call into a CPS partner stayed a C call.  A
 * CONSTRUCTOR over such arguments is one too -- a variadic callee's rest list
 * is a `Cons` chain -- since a constructor stores its arguments and invokes
 * nothing (the coloring pass reasons the same way, cps_collect_calls).
 * Deliberately not "control-free": a call to a colored function is
 * control-free to that scan and must not be delegated. */
static bool is_widened_literal(const Expr *a) {
    a = ascribe_peel(a);
    while (a && a->kind == EX_UNION_INJECT) a = ascribe_peel(a->as.union_inject_.value);
    if (!a) return false;
    if (is_atomic(a) || a->kind == EX_SYM_LIT) return true;
    if (a->kind == EX_CALL && a->as.call_.ctor && !a->as.call_.fn_expr) {
        for (uint32_t i = 0; i < a->as.call_.n_args; i++)
            if (!is_widened_literal(a->as.call_.args[i])) return false;
        return true;
    }
    return false;
}

static bool call_args_literal(const Expr *e) {
    for (uint32_t i = 0; i < e->as.call_.n_args; i++)
        if (!is_widened_literal(e->as.call_.args[i])) return false;
    return true;
}

static CTerm *build_letraw(CpsB *b, Expr *e, CVar x, CTerm *rest) {
    CTerm *t = new_term(b, CT_LETRAW);
    t->as.letraw.x = x;
    t->as.letraw.e = e;
    t->as.letraw.body = rest;
    return t;
}

/* U7 callcc: a (call/cc f)/(escape f) receiver `f` the native escape landing
 * (emit_callcc) can emit as a value -- a named top-level fn, a fat/poly/fn value,
 * or a closure literal (capturing OR not).  A CAPTURING closure's free vars are
 * surfaced by collect_free_vars (through the EX_CALLCC receiver) and ride the
 * lifted continuation's env exactly like the CT_LETRAW path: in a lifted body a
 * non-scalar capture bails to fallback (the enclosing continuation's collect_caps
 * sets cs.ok=false and evicts), and in the main body emit_value captures the
 * visible local directly.  A non-fn-value receiver returns NULL so the caller
 * keeps the CT_LETRAW delegation. */
static bool callcc_native_recv(const Expr *f) {
    f = ascribe_peel(f);
    if (!f) return false;
    if (is_delegatable_value(f)) return true;          /* fat/poly/fn/zero-cap closure */
    if (f->kind == EX_CLOSURE) return true;            /* a closure literal (capturing too) */
    if (f->kind == EX_VAR && f->as.var.binding
        && f->as.var.binding->is_global
        && f->as.var.binding->type.kind == TY_FN)
        return true;                                    /* a named top-level fn */
    return false;
}

/* Build a native CT_CALLCC (binds x = the call/cc value, continues body) for a
 * receiver emit_callcc can emit; NULL otherwise so the caller falls back to the
 * CT_LETRAW delegation. */
static CTerm *build_callcc(CpsB *b, Expr *e, CVar x, CTerm *body) {
    if (!callcc_native_recv(e->as.callcc_.fn)) return NULL;
    CTerm *t = new_term(b, CT_CALLCC);
    t->as.callcc.x = x;
    t->as.callcc.e = e;
    t->as.callcc.body = body;
    return t;
}

/* A named top-level fn receiver of a cloneable/serial-shift; NULL otherwise.
 * The two families gate the receiver slightly differently:
 *   - cloneable: a top-level (is_global) fn.
 *   - serial: any TY_FN value that is not a fat closure; the colored cut only
 *     applies to a global receiver (a local fn value is admitted).
 * Either way a COLORED receiver is admitted only when no effect can escape it
 * (fn_effect_may_escape): it is called through its direct-entry wrapper, whose
 * fresh DK root cannot see the handler enclosing the reset.
 * A capturing-closure receiver returns NULL here; the caller then picks it up
 * via the shift's EX_CLOSURE k_fn (receiver_expr path). */
static const Binding *marshal_named_receiver(CpsB *b, const Expr *shift,
                                             bool serial, bool *outward) {
    if (outward) *outward = false;
    const Expr *kf = ascribe_peel(serial ? shift->as.serial_shift_.k_fn
                                         : shift->as.cloneable_shift_.k_fn);
    if (!kf || kf->kind != EX_VAR || !kf->as.var.binding) return NULL;
    const Binding *recv = kf->as.var.binding;
    if (serial) {
        if (recv->type.kind != TY_FN) return NULL;   /* a function value */
        if (recv->closure_fn_binding || recv->hoist_closure_fn_binding) return NULL;   /* not a fat closure */
        /* serial-receiver-effect-cannot-reach-enclosing-handler: an effect that
         * escapes a named global receiver is reachable from the reset's own
         * continuation when the receiver is CALLED from it as a colored callee
         * (recv_outward) rather than from the shift body's fresh root. */
        if (recv->is_global && callee_colored(b, recv)
            && fn_effect_may_escape(b, recv)) {
            if (!outward) return NULL;
            *outward = true;
        }
    } else {
        if (!recv->is_global) return NULL;           /* a top-level fn */
        if (callee_colored(b, recv)
            && fn_effect_may_escape(b, recv)) return NULL;
    }
    return recv;
}

static bool cloneable_op_supported(const char *op) {
    return op && (strcmp(op, "+") == 0 || strcmp(op, "-") == 0 ||
                  strcmp(op, "*") == 0 || strcmp(op, "/") == 0);
}

static bool safe_to_delegate(CpsB *b, const Expr *e);   /* fwd (defined below) */

/* True if `e` contains a shift of kind `shift_kind` (EX_CLONEABLE_SHIFT or
 * EX_SERIAL_SHIFT) reachable through the supported context spine -- arithmetic
 * binops, calls, pure lets, an if branch point, and a do-sequence -- mirroring
 * the direct emitter's reaches_shift_kind.  Any nested control form (a further
 * reset/shift, an fn/closure body) self-delimits and stops the descent, so a
 * nested reset is not descended.  A shift of the *other* family is opaque here
 * (each family only reaches its own shift kind). */
static bool ctx_reaches_shift(const Expr *e, ExprKind shift_kind) {
    e = ascribe_peel(e);
    if (!e) return false;
    switch (e->kind) {
        case EX_CLONEABLE_SHIFT:
        case EX_SERIAL_SHIFT:
            return e->kind == shift_kind;
        case EX_BUILTIN:
            for (uint32_t i = 0; i < e->as.builtin.n; i++)
                if (ctx_reaches_shift(e->as.builtin.args[i], shift_kind)) return true;
            return false;
        case EX_CALL:
            for (uint32_t i = 0; i < e->as.call_.n_args; i++)
                if (ctx_reaches_shift(e->as.call_.args[i], shift_kind)) return true;
            return false;
        case EX_LET:
            for (uint32_t i = 0; i < e->as.let_.n; i++)
                if (ctx_reaches_shift(e->as.let_.bindings[i].init, shift_kind)) return true;
            return ctx_reaches_shift(e->as.let_.body, shift_kind);
        case EX_IF:
            return ctx_reaches_shift(e->as.if_.cond, shift_kind)
                || ctx_reaches_shift(e->as.if_.then_, shift_kind)
                || ctx_reaches_shift(e->as.if_.else_or_null, shift_kind);
        case EX_DO:
            for (uint32_t i = 0; i < e->as.do_.n; i++)
                if (ctx_reaches_shift(e->as.do_.items[i], shift_kind)) return true;
            return false;
        default:
            return false;
    }
}

#define CL_IR_MAX_FRAMES 8
#define CL_IR_MAX_LETS   8

/* A pure `let` binding is admissible as cloneable-context prelude iff its type is
 * a simple scalar local (int/bool/nil/cstr/float) -- matching the direct
 * emitter's ty_simple_local gate. */
static bool clone_let_ty_ok(TypeKind k) {
    return k == TY_INT || k == TY_BOOL || k == TY_NIL ||
           k == TY_CSTR || k == TY_FLOAT;
}

/* A scalar kind a delimited context frame's hole / env / result can carry: int
 * or cstr.  Both fit the intptr_t carrier the DK machine threads, so a value-
 * typed cont<cstr> (CC4) rides the same machinery as cont<int>, with the frame
 * wrapper casting to the real kind at the call boundary. */
static bool cps_scalar_kind_ok(TypeKind k) { return k == TY_INT || k == TY_CSTR; }

/* E3a (owning-cloneable-capture): may an OWNING value ride a cloneable 2-arg
 * call frame's captured-env slot?  Off-gate, no -- the env must be a Copy scalar
 * (cps_scalar_kind_ok), so an owning env evicts to TUR-E0710.  On-gate, a
 * ONE-WORD owning HANDLE is admitted WHEN the callee takes it ^borrow (a
 * type-system guarantee the callee never drops it): an `rc<T>` or a `:heap`
 * ADT / struct carrier handle.  Both ride the frame env by a bare pointer copy;
 * ^borrow means the frame never drops it, so the shallow-shared env is
 * read-only-correct across resumes and the owner drops it once (borrow teardown,
 * never a double-drop).  A multi-word owning aggregate does not fit the one-word
 * env and is excluded here.  See
 * docs/archive/history/cps-backend-owning-env-teardown-e3-plan.md. */
/* An owning by-value AGGREGATE (a one-ctor by-value ADT product with an
 * rc/ref field -- needs_drop_glue).  Multi-word, so it cannot ride the one-word
 * frame env by value; the cloneable emit carries a POINTER to the owner's
 * by-value local instead (the reset runs synchronously in the owner's frame, so
 * the address outlives the resumes, and a ^borrow frame never drops it). */
static bool cloneable_owning_agg(const Type *t) {
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
static bool cloneable_owning_env_ok(const Type *t) {
    if (!t) return false;
    return t->kind == TY_RC
        || type_is_heap_adt(*(Type *)t)
        || type_is_heap_struct(*(Type *)t)
        || cloneable_owning_agg(t);
}

/* An owning kind whose CONSUMING capture (a non-^borrow frame that drops it once
 * per call) the cloneable emit can give per-copy clone glue for:
 *   - `rc<T>`: env_clone = rc_strong_increment (incref -- cheap shared clone);
 *   - a FLAT `:heap` ADT / struct carrier (no owning fields, `!needs_drop_glue`):
 *     env_clone = malloc + shallow header copy (a genuine deep copy, so each
 *     resume frees its OWN allocation -- a shared handle would double-free).
 * A heap handle WITH owning fields needs a recursive deep clone (a later slice);
 * excluded here so a shallow copy never shares an owning field. */
/* Is a field kind a plain (non-owning, shallow-copyable) scalar? */
static bool clone_field_is_scalar(TypeKind k) {
    return !(k == TY_RC || k == TY_WEAK || k == TY_REF || k == TY_LREF
             || k == TY_ADT || k == TY_STRUCT || k == TY_APP);
}
/* A one-ctor ADT whose deep clone we can synthesize as a shallow copy + a
 * per-owning-field INCREF: every field is a plain value or an increfable owning
 * handle (rc / weak).  Used for a by-value AGGREGATE (its ref field would need a
 * boxed per-copy clone the by-value pass cannot provide) -- rc/weak only. */
static bool adt_fields_incref_cloneable(const AdtDef *d) {
    if (!d || d->n_ctors == 0) return false;
    const CtorDef *c = d->ctors[0];
    for (uint32_t i = 0; i < c->n_fields; i++)
        if (!(c->fields[i].kind == TY_RC || c->fields[i].kind == TY_WEAK
              || clone_field_is_scalar(c->fields[i].kind)))
            return false;
    return true;
}
/* A one-ctor HEAP ADT whose deep clone we can synthesize: rc/weak fields (incref)
 * PLUS `ref<scalar>` / `lref<scalar>` fields, which the env_clone deep-copies as a
 * fresh box (malloc + scalar copy) -- exactly mirroring drop_glue's `free` of the
 * box (a heap header is malloc'd, so its owning fields' boxes are separately
 * malloc'd and freed).  A ref to an OWNING inner, or a nested aggregate / heap
 * field, is rejected: the base drop_glue is shallow there (frees the box / does
 * not recurse), so a matching clone would only reproduce that base leak, not a
 * clean deep copy. */
static bool adt_fields_heap_cloneable(const AdtDef *d) {
    if (!d || d->n_ctors == 0) return false;
    const CtorDef *c = d->ctors[0];
    for (uint32_t i = 0; i < c->n_fields; i++) {
        TypeKind k = c->fields[i].kind;
        if (k == TY_RC || k == TY_WEAK || clone_field_is_scalar(k)) continue;
        if ((k == TY_REF || k == TY_LREF)
            && clone_field_is_scalar(c->fields[i].inner_kind)) continue;
        return false;
    }
    return true;
}
static const AdtDef *adt_def_of(const Type *t) {
    return (t->kind == TY_ADT) ? t->as.adt_.def
         : (t->kind == TY_APP) ? type_adt_app_def((Type *)t) : NULL;
}
/* A one-word `:heap` carrier handle consume-cloneable by malloc + shallow copy +
 * per-field incref. */
static bool heap_consume_cloneable(const Type *t) {
    if (!(type_is_heap_adt(*(Type *)t) || type_is_heap_struct(*(Type *)t)))
        return false;
    return adt_fields_heap_cloneable(adt_def_of(t));
}
/* A multi-word owning by-value AGGREGATE consume-cloneable by increfing its
 * owning fields (the frame passes it BY VALUE -> the callee drops its own copy,
 * balanced by the incref; no fresh allocation -- the env stays `&o`). */
static bool agg_consume_cloneable(const Type *t) {
    return cloneable_owning_agg(t) && adt_fields_incref_cloneable(adt_def_of(t));
}
static bool cloneable_consume_env_ok(const Type *t) {
    if (!t) return false;
    if (t->kind == TY_RC) return true;
    return heap_consume_cloneable(t) || agg_consume_cloneable(t);
}

/* Pure existence check: does `program` declare a Serializable instance for the
 * nominal (TY_ADT) type `t`?  Lets build_serial admit a SER-env frame; the
 * emitter (emit_cps_ir.c) resolves the instance's serialize/deserialize method
 * C names.  Kept in the pass (no emit helpers), mirroring the existence path of
 * emit_cps.c's sk_find_serializable. */
static bool cps_serializable_exists(const Expr *program, Type t) {
    if (!program || program->kind != EX_PROGRAM) return false;
    if (t.kind != TY_ADT || !t.as.adt_.def || !t.as.adt_.def->name) return false;
    const char *tname = t.as.adt_.def->name;
    for (uint32_t i = 0; i < program->as.program.n; i++) {
        const Expr *it = program->as.program.items[i];
        if (!it || it->kind != EX_INSTANCE_DEF) continue;
        const TypeClassInstance *inst = it->as.instance_def_.instance;
        if (!inst || !inst->typeclass || !inst->typeclass->name) continue;
        if (strcmp(inst->typeclass->name->name, "Serializable") != 0) continue;
        if (inst->n_type_args < 1) continue;
        Type at = inst->type_args[0];
        if (at.kind == TY_ADT && at.as.adt_.def && at.as.adt_.def->name &&
            strcmp(at.as.adt_.def->name, tname) == 0) return true;
    }
    return false;
}

/* U3/U4 native marshal-reset spine walk (unified cloneable + serial).
 *
 * Lowers  (<family>-reset <ctx> (<family>-shift receiver v))  where <ctx> is a
 * context spine bottoming out in a shift with a named uncolored receiver (or a
 * capturing closure receiver).  The spine may contain, in any nesting:
 *   - arithmetic frames `(<op> <operand> ... [])` reified as DK frames
 *     (outermost-first); n_frames == 0 is Shape 1 (identity continuation), >= 1
 *     is Shape 2 (dk_copy_range);
 *   - 1-arg / 2-arg call frames `(f [])` / `(f other [])`;
 *   - a do-sequence with a statement-position shift (side-effect prelude +
 *     ignore-value tail call frames);
 *   - pure `let` bindings (shift-free scalar inits, direct-emitted at the reset
 *     site as C locals so captured frame operands referencing them resolve);
 *   - one `if` branch point (pure condition; the shift-bearing arm rides the
 *     frame chain, the other arm is direct-emitted on the opposite branch).
 *
 * The `serial` parameter selects the family: it swaps the reset-body accessor,
 * the shift kind threaded into the reach test / post-loop, the receiver gate,
 * and drops the cloneable-only `n_live_captures` Shape-2 gate.  A handful of
 * frame branches differ per family (the serial 2-arg call frame admits a
 * Serializable ADT env and gates only the hole param; the serial do-tail admits
 * a 1-arg captured-config frame); those carve out on `serial` inline.  The node
 * is CT_CLONEABLE with `serial` set, so the emit path handles both families off
 * the shared fields.  Returns NULL for any richer shape -- the caller then
 * either evicts (cps_tail) or falls back to the CT_LETRAW delegation (cps_bind). */
/* TUR_TRACE_CORE=1: name the collector line that rejected a serial / cloneable
 * context, so an eviction traced as BODY-UNSUPPORTED "?" says which rule of the
 * DK lowering grammar the shape fell out of (guestbook-example-has-no-import-graph). */
#define SK_REJECT() do { \
        if (getenv("TUR_TRACE_CORE")) \
            fprintf(stderr, "[CTX-REJECT] cps_ir.c:%d\n", (int)__LINE__); \
        return NULL; \
    } while (0)

static CTerm *build_marshal_reset(CpsB *b, Expr *e, CVar x, CTerm *rest,
                                  bool serial) {
    const ExprKind shift_kind = serial ? EX_SERIAL_SHIFT : EX_CLONEABLE_SHIFT;
    const Expr *cur = ascribe_peel(serial ? e->as.serial_reset_.body
                                          : e->as.cloneable_reset_.body);
    CloneFrame frames[CL_IR_MAX_FRAMES];
    CloneLet   lets[CL_IR_MAX_LETS];
    uint32_t nf = 0, nl = 0;
    uint32_t n_outer = 0;   /* frames collected before the `if` (outer context) */
    const Expr *if_cond = NULL, *if_pure = NULL;
    bool if_when = true, saw_if = false;

    for (;;) {
        cur = ascribe_peel(cur);
        if (!cur) SK_REJECT();

        /* Arithmetic frame: single-hole int binop.  Cloneable reifies it as a
         * per-site DK frame; serial as a shared tagged-marshaler frame -- the
         * build shape is identical, the divergence is entirely in emit. */
        if (cur->kind == EX_BUILTIN && cur->as.builtin.n == 2
            && cur->as.builtin.spec && cur->type.kind == TY_INT
            && cloneable_op_supported(cur->as.builtin.spec->c_op)) {
            const Expr *a0 = ascribe_peel(cur->as.builtin.args[0]);
            const Expr *a1 = ascribe_peel(cur->as.builtin.args[1]);
            bool h0 = ctx_reaches_shift(a0, shift_kind);
            bool h1 = ctx_reaches_shift(a1, shift_kind);
            if (h0 == h1) SK_REJECT();               /* need exactly one hole side */
            const Expr *other = h0 ? a1 : a0;
            if (!other || other->type.kind != TY_INT) SK_REJECT();
            /* D6a: the non-hole operand may be non-atomic (e.g. a call `(id 3)`)
             * as long as it is shift-free and pure/emit_value-able.  It is
             * emit_value'd once at the reset site and its value rides the frame's
             * env (deep-cloned with the chain for cloneable, serialized inline for
             * serial), so `(+ (id 3) [])` reifies its context correctly.  (An
             * atomic operand rides `operand` directly; env_expr stays NULL.) */
            bool arith_atom = is_atomic(other);
            if (!arith_atom && (ctx_reaches_shift(other, shift_kind)
                          || !safe_to_delegate(b, other))) SK_REJECT();
            if (nf >= CL_IR_MAX_FRAMES) SK_REJECT();
            memset(&frames[nf], 0, sizeof(CloneFrame));
            frames[nf].op        = cur->as.builtin.spec->c_op;   /* stable string */
            frames[nf].operand   = atom_of(other);
            frames[nf].env_expr  = arith_atom ? NULL : other;
            frames[nf].hole_left = h0;
            nf++;
            cur = h0 ? a0 : a1;                       /* descend the hole side */
            continue;
        }

        /* Call frame (1-arg): a call `(f [])` to a top-level scalar->scalar fn
         * that no effect escapes (uncolored, or colored with an empty row --
         * the frame wrapper calls it through its direct entry, exactly as the
         * receiver is called; see fn_effect_may_escape); the hole is the sole
         * argument, so there is no captured env. */
        if (cur->kind == EX_CALL && cur->as.call_.n_args == 1
            && cur->as.call_.fn_binding && !cur->as.call_.fn_expr) {
            const Binding *fb = cur->as.call_.fn_binding;
            if (fb->type.kind != TY_FN || fb->type.as.fn.arity != 1) SK_REJECT();
            if (fb->closure_fn_binding || fb->hoist_closure_fn_binding) SK_REJECT();         /* not a fat closure */
            if (callee_colored(b, fb) && fn_effect_may_escape(b, fb))
                SK_REJECT();                                 /* nothing escapes it */
            if (!cps_scalar_kind_ok(cur->type.kind)) SK_REJECT();         /* result */
            if (!cps_scalar_kind_ok(fb->type.as.fn.arg_kinds[0])) SK_REJECT();  /* arg */
            const Expr *a0 = ascribe_peel(cur->as.call_.args[0]);
            if (!ctx_reaches_shift(a0, shift_kind)) SK_REJECT();  /* sole arg is hole */
            if (nf >= CL_IR_MAX_FRAMES) SK_REJECT();
            memset(&frames[nf], 0, sizeof(CloneFrame));
            frames[nf].op        = NULL;
            frames[nf].call_fn   = fb;
            frames[nf].operand.kind = CA_INT;   /* unused placeholder (env passed as 0) */
            frames[nf].operand.ty   = TY_INT;
            frames[nf].hole_left = true;         /* the hole is the sole arg */
            nf++;
            cur = a0;                            /* descend into the hole arg */
            continue;
        }

        /* Call frame (2-arg): `(f other [])` / `(f [] other)` to a top-level
         * uncolored scalar fn; the hole is one arg, the other is the captured env.
         * Cloneable requires both params + the env to be scalars and deep-clones
         * the env with the chain; serial gates only the hole param and admits an
         * int/cstr env inline OR a nominal env with a Serializable instance (the
         * marshaler serializes it via the instance codec). */
        if (cur->kind == EX_CALL && cur->as.call_.n_args == 2
            && cur->as.call_.fn_binding && !cur->as.call_.fn_expr) {
            const Binding *fb = cur->as.call_.fn_binding;
            if (fb->type.kind != TY_FN || fb->type.as.fn.arity != 2) SK_REJECT();
            if (fb->closure_fn_binding || fb->hoist_closure_fn_binding) SK_REJECT();         /* not a fat closure */
            if (callee_colored(b, fb) && fn_effect_may_escape(b, fb))
                SK_REJECT();                                 /* nothing escapes it */
            if (!cps_scalar_kind_ok(cur->type.kind)) SK_REJECT();   /* result: scalar */
            const Expr *a0 = ascribe_peel(cur->as.call_.args[0]);
            const Expr *a1 = ascribe_peel(cur->as.call_.args[1]);
            bool h0 = ctx_reaches_shift(a0, shift_kind);
            bool h1 = ctx_reaches_shift(a1, shift_kind);
            if (h0 == h1) SK_REJECT();               /* exactly one hole side */
            if (serial) {
                /* the hole slot's param is the resume value -- a scalar (int/cstr) */
                if (!cps_scalar_kind_ok(fb->type.as.fn.arg_kinds[h0 ? 0 : 1]))
                    SK_REJECT();
            } else {
                /* The hole param takes the resumed value -- always a scalar.  The
                 * non-hole (env) param may be a scalar, OR -- under the E3a gate --
                 * an owning `rc` the callee takes ^borrow (checked on the operand
                 * below; ^borrow guarantees the frame never drops it). */
                if (!cps_scalar_kind_ok(fb->type.as.fn.arg_kinds[h0 ? 0 : 1]))
                    SK_REJECT();
                TypeKind envk = fb->type.as.fn.arg_kinds[h0 ? 1 : 0];
                const Expr *env_op = ascribe_peel(h0 ? a1 : a0);
                bool env_borrow = FN_ARG_FLAG(fb->type.as.fn, (h0 ? 1 : 0), FA_BORROW);
                /* Borrow: any admissible owning kind + ^borrow.  Consume: rc only,
                 * non-^borrow (the frame drops it; env_clone glue balances each
                 * resume copy). */
                bool owning_borrow_env =
                    env_op && cloneable_owning_env_ok(&env_op->type) && env_borrow;
                bool owning_consume_env =
                    env_op && !env_borrow && cloneable_consume_env_ok(&env_op->type);
                if (!cps_scalar_kind_ok(envk)
                    && !owning_borrow_env && !owning_consume_env)
                    SK_REJECT();
            }
            const Expr *other = h0 ? a1 : a0;        /* the captured env operand */
            if (!other) SK_REJECT();
            bool env_atom = is_atomic(other);
            if (serial) {
                if (ctx_reaches_shift(other, shift_kind)) SK_REJECT();
                TypeKind ek = other->type.kind;
                if (!cps_scalar_kind_ok(ek)
                    && !cps_serializable_exists(b->program, other->type))
                    SK_REJECT();
                if (!env_atom && !safe_to_delegate(b, other)) SK_REJECT();
            } else {
                /* E3a: an owning env frame is admitted in two modes:
                 *  - BORROW (any admissible owning kind + ^borrow callee param):
                 *    the frame never drops it, so the shallow-shared / by-address
                 *    env is read-only-correct across resumes and the owner drops it
                 *    once (no per-frame glue).
                 *  - CONSUME (rc only, non-^borrow callee): the frame DROPS its rc
                 *    argument once per call.  A multi-shot resume would over-drop a
                 *    shared handle, so the frame gets dk_frame_owning env_clone glue
                 *    (rc incref per dk_copy_node copy) -- each resume increfs its own
                 *    +1 that its drop balances; the owner's base +1 is released once
                 *    by its (P5b-threaded) scope-exit drop.  rc only: it is the sole
                 *    owning kind with scalar clone glue (rc_strong_increment). */
                bool env_borrow = FN_ARG_FLAG(fb->type.as.fn, (h0 ? 1 : 0), FA_BORROW);
                bool owning_borrow_env =
                    cloneable_owning_env_ok(&other->type) && env_borrow;
                bool owning_consume_env =
                    !env_borrow && cloneable_consume_env_ok(&other->type);
                if (!cps_scalar_kind_ok(other->type.kind)
                    && !owning_borrow_env && !owning_consume_env)
                    SK_REJECT();
                /* A multi-word owning AGGREGATE env is captured by ADDRESS (`&o`),
                 * so its operand must be an atomic lvalue (a bare local) -- a
                 * computed/non-atomic aggregate has no stable address to take. */
                if (cloneable_owning_agg(&other->type) && !env_atom) SK_REJECT();
                if (!env_atom && (ctx_reaches_shift(other, shift_kind)
                              || !safe_to_delegate(b, other))) SK_REJECT();
            }
            if (nf >= CL_IR_MAX_FRAMES) SK_REJECT();
            memset(&frames[nf], 0, sizeof(CloneFrame));
            frames[nf].op        = NULL;
            frames[nf].call_fn   = fb;
            frames[nf].operand   = atom_of(other);   /* captured scalar env */
            frames[nf].env_expr  = env_atom ? NULL : other;
            frames[nf].hole_left = h0;                /* hole is the left arg? */
            nf++;
            cur = h0 ? a0 : a1;                        /* descend into the hole side */
            continue;
        }

        /* do-sequence with a statement-position shift:
         *   (do PRELUDE... (<family>-shift receiver v) TAIL...)
         * The prelude items run once at capture time (side-effect-only); the shift
         * must be the do-item itself; each tail item is a top-level uncolored
         * int-returning call reified as an ignore-value frame (runs on resume
         * regardless of the resumed value).  Cloneable admits only 0-arg tails;
         * serial also admits a 1-arg captured-config tail `(f cap)` (cap serialized
         * inline / via a Serializable instance).  Whole reset body, no outer frames,
         * no `if`. */
        if (cur->kind == EX_DO && nf == 0 && !saw_if) {
            uint32_t N = cur->as.do_.n;
            int32_t m = -1;
            for (uint32_t i = 0; i < N; i++) {
                if (ctx_reaches_shift(cur->as.do_.items[i], shift_kind)) {
                    if (m >= 0) SK_REJECT();             /* at most one hole */
                    m = (int32_t)i;
                }
            }
            if (m < 0) SK_REJECT();
            const Expr *shift_item = ascribe_peel(cur->as.do_.items[m]);
            if (!shift_item || shift_item->kind != shift_kind) SK_REJECT();
            /* Prelude items [0, m): direct-emitted for side effect at the reset site. */
            for (int32_t i = 0; i < m; i++) {
                const Expr *pre = cur->as.do_.items[i];
                if (ctx_reaches_shift(pre, shift_kind)) SK_REJECT();
                if (!safe_to_delegate(b, pre)) SK_REJECT();
                if (nl >= CL_IR_MAX_LETS) SK_REJECT();
                lets[nl].binding = NULL;                 /* side-effect prelude */
                lets[nl].init    = pre;
                nl++;
            }
            /* Tail items (m, N): ignore-value frames.  Record in reverse so the
             * first tail item is innermost (runs first on resume) and the last is
             * outermost (its value is the reset's value). */
            for (int32_t i = (int32_t)N - 1; i > m; i--) {
                const Expr *tail = ascribe_peel(cur->as.do_.items[i]);
                if (!tail || tail->kind != EX_CALL ||
                    !tail->as.call_.fn_binding || tail->as.call_.fn_expr) SK_REJECT();
                const Binding *fb = tail->as.call_.fn_binding;
                if (fb->type.kind != TY_FN) SK_REJECT();
                if (fb->closure_fn_binding || fb->hoist_closure_fn_binding) SK_REJECT();    /* not a fat closure */
                if (callee_colored(b, fb) && fn_effect_may_escape(b, fb))
                    SK_REJECT();                            /* nothing escapes it */
                if (tail->type.kind != TY_INT) SK_REJECT(); /* result: int */
                if (nf >= CL_IR_MAX_FRAMES) SK_REJECT();
                if (fb->type.as.fn.arity == 0 && tail->as.call_.n_args == 0) {
                    /* 0-arg ignore-value tail `(f)`: run f() on resume, no env. */
                    memset(&frames[nf], 0, sizeof(CloneFrame));
                    frames[nf].op           = NULL;
                    frames[nf].call_fn      = fb;
                    frames[nf].ignore_value = true;
                    frames[nf].operand.kind = CA_INT;   /* unused (env passed as 0) */
                    frames[nf].operand.ty   = TY_INT;
                    frames[nf].hole_left    = true;
                } else if (serial && fb->type.as.fn.arity == 1
                           && tail->as.call_.n_args == 1) {
                    /* 1-arg captured-config tail `(f cap)`: cap is a pure atomic
                     * value captured at the reset site (int inline / cstr length-
                     * prefixed / Serializable SER codec) and applied to f on resume,
                     * ignoring the resumed value -- what lets a real loop `(loop cfg)`
                     * receive its config through the saved continuation. */
                    const Expr *cap = ascribe_peel(tail->as.call_.args[0]);
                    if (!cap || ctx_reaches_shift(cap, shift_kind)) SK_REJECT();
                    if (!is_atomic(cap)) SK_REJECT();
                    TypeKind ek = cap->type.kind;
                    bool inline_ok = (ek == TY_INT || ek == TY_CSTR);  /* inline codec */
                    bool ser_ok = !inline_ok
                        && cps_serializable_exists(b->program, cap->type);  /* SER codec */
                    if (!inline_ok && !ser_ok) SK_REJECT();
                    if (fb->type.as.fn.arg_kinds[0] != ek) SK_REJECT();
                    memset(&frames[nf], 0, sizeof(CloneFrame));
                    frames[nf].op           = NULL;
                    frames[nf].call_fn      = fb;
                    frames[nf].ignore_value = true;
                    frames[nf].operand      = atom_of(cap);   /* real captured env */
                    frames[nf].hole_left    = true;
                } else {
                    SK_REJECT();
                }
                nf++;
            }
            cur = shift_item;                       /* the shift; loop exits below */
            continue;
        }

        /* Pure `let` prelude: inits shift-free + scalar, body carries the hole.
         * emit_cloneable lays every prelude local down at the reset site, ahead of
         * the `if` branch and the frame-operand pushes, so a `let` either ABOVE the
         * `if` (referenced by outer frames + the pure arm) or nested in the shift-
         * bearing arm (referenced by inner frames) is in scope where it is used.
         * Hoisting a shift-arm let out of its branch is sound because its init is
         * pure and scalar; a binding the shift BODY itself needs would be a live
         * capture, which the shift admission rejects. */
        if (cur->kind == EX_LET) {
            const Expr *lbody = cur->as.let_.body;
            if (!ctx_reaches_shift(lbody, shift_kind)) SK_REJECT();
            for (uint32_t i = 0; i < cur->as.let_.n; i++) {
                const Expr    *init = cur->as.let_.bindings[i].init;
                const Binding *bd   = cur->as.let_.bindings[i].binding;
                if (ctx_reaches_shift(init, shift_kind)) SK_REJECT();
                if (!bd || !clone_let_ty_ok(bd->type.kind)) SK_REJECT();
                if (!safe_to_delegate(b, init)) SK_REJECT();   /* pure, emit_value-able */
                if (nl >= CL_IR_MAX_LETS) SK_REJECT();
                lets[nl].binding = bd;
                lets[nl].init    = init;
                nl++;
            }
            cur = lbody;
            continue;
        }

        /* One `if` branch point: pure condition, exactly one shift-bearing arm;
         * the pure arm is direct-emitted on the opposite branch. */
        if (cur->kind == EX_IF) {
            if (saw_if) SK_REJECT();                  /* only one branch point */
            const Expr *cond = cur->as.if_.cond;
            const Expr *thn  = cur->as.if_.then_;
            const Expr *els  = cur->as.if_.else_or_null;
            if (!cond || !thn || !els) SK_REJECT();   /* need both arms */
            if (ctx_reaches_shift(cond, shift_kind)) SK_REJECT();
            if (!safe_to_delegate(b, cond)) SK_REJECT();
            bool ht = ctx_reaches_shift(thn, shift_kind);
            bool he = ctx_reaches_shift(els, shift_kind);
            if (ht == he) SK_REJECT();                /* exactly one shift arm */
            const Expr *shift_arm = ht ? thn : els;
            const Expr *pure_arm  = ht ? els : thn;
            if (ctx_reaches_shift(pure_arm, shift_kind)) SK_REJECT();   /* defensive */
            if (!safe_to_delegate(b, pure_arm)) SK_REJECT();
            if_cond = cond; if_pure = pure_arm; if_when = ht; saw_if = true;
            n_outer = nf;   /* frames so far are OUTSIDE the if; later frames are inner */
            cur = shift_arm;
            continue;
        }

        break;
    }

    if (!cur || cur->kind != shift_kind) SK_REJECT();
    /* Shape 2 (nf >= 1) reifies the delimited context as a DK frame chain, so a
     * captured continuation genuinely re-runs the context and references the
     * captured locals -- native Shape 2 has no env-carrying prelude, so a live-
     * capture cloneable Shape 2 still delegates.  Shape 1 (nf == 0) captures the
     * IDENTITY continuation (the shift sits at the reset tail), which never reads
     * a captured local, so a conservatively-marked live capture on a Shape 1 shift
     * is emit-time dead and safe to admit.  The gate is cloneable-only: serial's
     * prelude is presence-gated so it needs no such check. */
    /* Live-capture Shape-2: there used to be a blunt count gate here rejecting a
     * Shape 2 with any live capture, on the grounds that native Shape 2 carries
     * captured locals only via frame operands, so a live capture that is NOT a
     * frame operand would be lost.  Reaching here means the frame loop parsed
     * the ENTIRE continuation into validated frames, so every local the
     * continuation references IS a carried frame operand (a scalar, or a
     * ^borrow rc whose shallow-shared env is read-only-correct across resumes);
     * the remaining counted locals are receiver-body captures (single-shot: the
     * receiver runs once) or bound after the reset (dead at the shift).  The
     * frame validation is what is trusted instead, unconditionally since
     * owning-cloneable-capture graduated (2026-07-20). */
    bool recv_outward = false;
    const Binding *recv = marshal_named_receiver(b, cur, serial, &recv_outward);
    const Expr *recv_expr = NULL;
    /* The outward lowering runs the receiver on the reset's own continuation.
     * An `if` branch point needs that continuation on both arms: the emitter
     * lifts the rest once and delivers the pure arm into it
     * (serial-receiver-effect-under-if-closure-or-leaf, shape 1). */
    if (!recv) {
        /* U7: a CLOSURE receiver (capturing or not).  Shape 1 calls it directly at
         * the reset site; Shape 2 threads it through the dk_shift body env -- the
         * emitter bakes the closure's thunk into the per-site body fn.  The receiver
         * runs once at capture; only the continuation is cloned/marshaled. */
        const Expr *kf = ascribe_peel(serial ? cur->as.serial_shift_.k_fn
                                             : cur->as.cloneable_shift_.k_fn);
        if (kf && kf->kind == EX_CLOSURE) recv_expr = kf;
        else SK_REJECT();
        /* serial-receiver-effect-cannot-reach-enclosing-handler: a closure
         * receiver is called from the shift body's fresh root, so one an effect
         * escapes is refused here as a named one used to be -- the fallback's
         * TUR-E0706 names it.  (It was masked while a `k` parameter kept the
         * lambda off the CPS path; admitting `k` made it compile and abort with
         * "unhandled effect" at run time.)  The outward lowering calls a NAMED
         * receiver's `__cps` twin; a capturing lambda's env-taking twin is not
         * threaded through here. */
        const struct Closure *rcl = kf->as.closure_.closure;
        const Binding *rfb = (rcl && rcl->fn) ? rcl->fn->binding : NULL;
        if (!rfb) SK_REJECT();
        if (callee_colored(b, rfb) && fn_effect_may_escape(b, rfb)) {
            /* serial-receiver-effect-under-if-closure-or-leaf (shape 2): a
             * serial closure receiver is called outward too -- through its
             * env-taking `__cps` twin, which the emitter registers because
             * this use makes the lambda threadable.  Classification evicts the
             * function if the twin was not emitted after all (Rule D,
             * outward_receivers_in_s), so the fallback's TUR-E0706 still
             * names it then. */
            if (!serial) SK_REJECT();
            recv_outward = true;
        }
    }

    CTerm *t = new_term(b, CT_CLONEABLE);
    t->as.cloneable.serial = serial;
    t->as.cloneable.recv_outward = recv_outward;
    t->as.cloneable.x = x;
    t->as.cloneable.receiver = recv;
    t->as.cloneable.receiver_expr = recv_expr;
    t->as.cloneable.n_frames = nf;
    if (nf) {
        CloneFrame *fa = arena_alloc(b->a, nf * sizeof(CloneFrame));
        memcpy(fa, frames, nf * sizeof(CloneFrame));
        t->as.cloneable.frames = fa;
    } else {
        t->as.cloneable.frames = NULL;
    }
    t->as.cloneable.n_lets = nl;
    if (nl) {
        CloneLet *la = arena_alloc(b->a, nl * sizeof(CloneLet));
        memcpy(la, lets, nl * sizeof(CloneLet));
        t->as.cloneable.lets = la;
    } else {
        t->as.cloneable.lets = NULL;
    }
    t->as.cloneable.n_outer_frames = n_outer;
    t->as.cloneable.if_cond = if_cond;
    t->as.cloneable.if_pure = if_pure;
    t->as.cloneable.if_when = if_when;
    t->as.cloneable.body = rest;
    return t;
}

static CTerm *build_cloneable(CpsB *b, Expr *e, CVar x, CTerm *rest) {
    return build_marshal_reset(b, e, x, rest, /*serial=*/false);
}

static CTerm *build_serial(CpsB *b, Expr *e, CVar x, CTerm *rest) {
    return build_marshal_reset(b, e, x, rest, /*serial=*/true);
}

/* N6.1: a subexpression that can be emitted wholesale by the direct emitter
 * (via CT_LETRAW) because it neither threads a continuation nor could reach one:
 * it contains no syntactic control op AND no call to a colored (may-capture)
 * function AND no indirect call (unknown coloring).  Nested fn/closure bodies are
 * call-graph boundaries -- not descended (a capture-free fn value is itself a
 * delegatable leaf; a capturing one is rejected so the has_capture cut is not
 * bypassed).  The scan is SOUND by construction: it returns true only for a
 * closed set of forms whose children it fully recurses; any unrecognized form
 * (or a control op / colored / indirect call) yields false and falls back.
 *
 * Delegating such a form in a lifted (zero-capture) body would still need the
 * has_capture cut to see its free vars; has_capture conservatively rejects an
 * un-scanned CT_LETRAW operand, so a delegated composite lands only where
 * has_capture is not the gate (the main function body) -- exactly the safe set. */
static bool safe_to_delegate(CpsB *b, const Expr *e) {
    e = ascribe_peel(e);
    if (!e) return false;
    if (is_atomic(e)) return true;
    if (is_delegatable_value(e)) return true;   /* capture-free fn value */
    switch (e->kind) {
        /* An `(unsafe ...)` block desugars to a handle on the built-in Unsafe
         * effect, but Unsafe is a pure compile-time MARKER that is never performed
         * (is_unsafe_marker) -- the handle's fiber-lift never suspends and the
         * direct emitter emits the body directly in place.  So an unsafe-marker
         * handle whose body is itself delegatable is delegatable: the whole region
         * (unsafe scope + body, including a fat-closure arg the direct emitter owns)
         * emits through the direct emitter, which also frees any scoped closure env.
         * This lets a function colored ONLY by an `unsafe` block (e.g. `free-lift-bind`
         * / `unsafe-closure-capture`: a capturing/fat closure passed to `free-run`
         * inside `unsafe`) whole-body-delegate instead of evicting on the closure. */
        case EX_HANDLE: {
            HandleExpr *h = e->as.handle_.handle;
            if (!h) return false;
            /* An `(unsafe ...)` marker handle: Unsafe is never performed, so the
             * body emits directly in place regardless of control shape. */
            if (h->is_unsafe_marker)
                return safe_to_delegate(b, h->body);
            /* A REAL handle is never whole-body-delegated.  P5 used to delegate a
             * self-contained one -- the direct emitter emits the whole handle as
             * one region, and its handler chain falls back to
             * global_effect_handler_chain -- pushing its case effects so an
             * interior `perform` of them was admitted too.  E7/E1: a deep handle
             * must DK-lower (build_handle -> CT_HANDLE), which is what dissolves
             * the handler-installer's base taint (ensure_S:2814) and lets its
             * effects leave the fiber runtime.  Unconditional since
             * cps-tramp-resume graduated (2026-07-19), so the delegating branch
             * never ran again -- and being the only writer of the handled-effect
             * stack, it took the rest of P5 with it (see the note above). */
            return false;
        }
        /* Control operators: never delegatable (they thread a continuation).
         *
         * EX_PERFORM and EX_RESUME joined this group when P5's handled-effect
         * stack was removed (see its note above).  A `perform` used to be
         * delegatable when an enclosing DELEGATED handle discharged its effect,
         * and a `resume` when it sat inside one; since no handle has been
         * delegated after cps-tramp-resume graduated, neither condition could
         * hold.  EX_HANDLE stays its own case above -- an `(unsafe ...)` marker
         * handle IS still delegatable, and that branch is live. */
        case EX_PERFORM:
        case EX_RESUME:
        case EX_DISCONTINUE:
        case EX_RESET: case EX_SHIFT: case EX_SHIFT0:
        case EX_CLONEABLE_SHIFT:
        case EX_SERIAL_SHIFT:
            return false;
        /* U5 (cps-backend-unification): (async f) and (await fut) are self-
         * contained runtime calls (tur_async_fiber / tur_await_future) -- they do
         * NOT thread the caller's DK continuation (the future/result is an ordinary
         * value, and the async thunk's own effects are handled in its fiber scope).
         * Delegating them via CT_LETRAW reuses the proven fiber runtime and lets a
         * colored function that ALSO awaits stay CPS-emitted instead of wholly
         * evicting.  The delegated region is emitted by the direct emitter
         * (emit_value -> EX_ASYNC/EX_AWAIT), the bound future/result becomes a local,
         * and the CPS continuation runs after. */
        case EX_ASYNC:
            return true;
        case EX_AWAIT:
            /* cps-async (graduated 2026-07-19): a shifted await threads a
             * continuation, so in the whole-body probe it is always a control op
             * (not wholly delegatable).  A handler-case await still delegates, but
             * per-node (build_letraw guarded by b->in_handler_case), not here. */
            return false;
        /* D4 (cps-backend-direct-lowering-removal): the cloneable-reset (U3) and
         * serial-reset (U4) delimited-control carve-out is deleted.  These regions
         * are now lowered natively by the CT-IR backend (build_cloneable /
         * build_serial, plus the no-shift-reset and Shape-1 live-capture paths), so
         * they are no longer delegated to the direct emitter via CT_LETRAW.  They
         * fall to the `default: return false` below -- non-delegatable, forcing the
         * enclosing form to decompose so the reset lands at a bind position where
         * cps_tail emits it natively (or evicts the whole function for a shape
         * outside the native subset, rather than routing a sub-region out). */
        /* nested fn defs: call-graph boundaries, delegatable as values. */
        case EX_FN_DEF: case EX_FN: case EX_CLOSURE:
            return is_delegatable_value(e);
        /* U2 (cps-backend-unification): (call/cc f) / (escape f) is an
         * UNDELIMITED escape whose continuation is captured at a *local* setjmp
         * landing that emit_cps_callcc establishes inline -- it does NOT thread
         * the DK continuation the way shift/perform do.  So delegating it via
         * CT_LETRAW is sound: the escape's setjmp/longjmp landing sits before the
         * bound result, and "the rest of the computation" from the call/cc site
         * is exactly the CPS continuation that runs after the binding.  This lets
         * a colored function that ALSO contains a call/cc/escape stay on the
         * CT-IR path instead of wholly evicting to the direct emitter.
         *
         * The receiver `f` is emitted by emit_cps_callcc regardless.  A capture-
         * free receiver (a plain fn, a fat-boxed fn, or a zero-capture closure)
         * delegates via the normal is-delegatable-value check.  A CAPTURING
         * closure also delegates: collect_caps (CT_LETRAW) walks the receiver's
         * free vars into the lifted continuation's env, and cap_add admits a
         * scalar (Copy) capture while a non-Copy capture bails to fallback. */
        case EX_CALLCC: {
            const Expr *f = ascribe_peel(e->as.callcc_.fn);
            /* A call/cc / escape whose receiver the native escape landing can emit
             * (build_callcc, via callcc_native_recv) is NOT delegated: reporting it
             * non-delegatable forces the enclosing form to decompose, so the callcc
             * lands at a bind position and lowers to a native CT_CALLCC instead of
             * riding a CT_LETRAW delegation (which would drag emit_cps.c's escape
             * lowering into an otherwise-native function -- e.g. a capturing escape
             * nested in a shift body's abort value).  A receiver build_callcc cannot
             * emit still delegates, keeping the colored function CPS-emitted. */
            if (callcc_native_recv(e->as.callcc_.fn)) return false;
            return safe_to_delegate(b, e->as.callcc_.fn)
                || (f && f->kind == EX_CLOSURE);
        }
        case EX_CALL: {
            const Binding *fn = e->as.call_.fn_binding;
            /* E2a: a thread-param call takes the per-node path (cps_tail) so it
             * threads the DK via the registry -- never whole-body-delegate to fiber. */
            if (fn && cps_ir_thread_param_has(fn))
                return false;
            /* Nor a call through a fat poly-fn PARAM that the `fn_cps` dispatch
             * covers: it picks the callee's DK-threading entry when the value
             * has one and the direct entry otherwise, so it is right for every
             * value the param can hold.  Delegated, the call is always the
             * direct `f.fn` -- and the param is a thread param only when every
             * value passed to it is registered, so ONE pure fn-value passed in
             * another call sent an effectful lambda's `perform` off the
             * trampoline: "unhandled effect" from a program that compiled. */
            if (fn && fncps_param_call_ok(fn, e))
                return false;
            /* Nor a call through an inlined alias of a fn param: its `let` is
             * dropped, so the direct emitter would name an undeclared local.
             * Refused here, it reaches cps_tail / cps_bind, which rewrite it to
             * the call through the param. */
            {
                const PapInline *pe = b ? pap_lookup(b, fn) : NULL;
                if (pe && pe->is_alias) return false;
                /* ...nor through a local closure of a colored lambda: delegated,
                 * the call runs the lambda's direct entry from a fresh root. */
                if (pe && pe->is_env_call) return false;
                if (cps_self_env_call(b, fn)) return false;
            }
            /* An indirect callee with no binding -- e.g. a capability CALL
             * `(.print-line cap "..")` whose callee is a `.field` access yielding
             * an effect-annotated fn.  This was delegatable inside a DELEGATED
             * handle that discharged the fn-value's whole effect row; no handle is
             * delegated any more (see P5's note above), so it never is. */
            if (!fn) return false;
            /* cps-local-fn-alias-or-lambda-called-in-place-refused: a call
             * through an immutable local holding a colored global -- a lifted
             * lambda, which has no source_binding -- performs what the global
             * performs.  Delegated, it would run the global's direct entry from
             * a fresh root; refused here, cps_tail / cps_bind rewrite it to the
             * direct call (pap_register_let) or lower it as a fn-value call. */
            if (!fn->is_global && fn->widen_fn_alias
                && callee_colored(b, fn->widen_fn_alias))
                return false;
            {
                const Binding *lt = cps_ir_letrec_member_target(fn);
                if (lt && callee_colored(b, lt)) return false;
            }
            if (callee_colored(b, fn)) {
                /* A colored GLOBAL callee whose effects are ALL discharged by the
                 * enclosing delegated handle runs entirely under the local fiber
                 * handler -- its performs land on the global chain the handle
                 * installs, never escaping to the enclosing DK -- so the call is
                 * safe to delegate to the direct emitter (which calls the callee's
                 * direct entry).  This lets a handler-installer like `run`
                 * (`(handle (apply-cb ...) (Ask ..))`, apply-cb : #fx{Ask})
                 * whole-body-delegate instead of evicting.  Any other colored
                 * callee (an unhandled or DK-threaded effect, an fn-value callee)
                 * stays native. */
                if (!colored_call_wbd_delegatable(b, e))
                    return false;
            }
            /* P5: inside a delegated handle, a call through a fn-VALUE (a non-global
             * fn-typed binding -- a param or a local closure -- or any indirect
             * fn_expr callee) can reach a COLORED fn-value that performs via the DK.
             * The delegated handle installs a FIBER handler frame, which does not
             * catch a DK-threaded perform (fiber<->DK non-interop), so such a call
             * would leave the effect unhandled.  callee_colored only knows global
             * FnDefs, so it misses fn-value callees -- reject them here so the
             * handle stays native (its perform lowers to CT_PERFORM on the DK).
             *
             * The PN exception that used to sit here -- admitting a fn-value whose
             * entire concrete effect row was discharged by the enclosing delegated
             * handle -- only ever applied on the P5 handle-delegation path, which
             * no longer exists (see the note above).  The closure-only whole-body
             * delegation this case still serves never had an enclosing handle, so
             * nothing it admits changes. */
            if (e->as.call_.fn_expr && !safe_to_delegate(b, e->as.call_.fn_expr))
                return false;
            for (uint32_t i = 0; i < e->as.call_.n_args; i++)
                if (!safe_to_delegate(b, e->as.call_.args[i])) return false;
            return true;
        }
        case EX_BUILTIN:
            for (uint32_t i = 0; i < e->as.builtin.n; i++)
                if (!safe_to_delegate(b, e->as.builtin.args[i])) return false;
            return true;
        case EX_IF:
            return safe_to_delegate(b, e->as.if_.cond)
                && safe_to_delegate(b, e->as.if_.then_)
                && (!e->as.if_.else_or_null || safe_to_delegate(b, e->as.if_.else_or_null));
        case EX_DO:
            for (uint32_t i = 0; i < e->as.do_.n; i++)
                if (!safe_to_delegate(b, e->as.do_.items[i])) return false;
            return true;
        case EX_LET:
            for (uint32_t i = 0; i < e->as.let_.n; i++) {
                if (!safe_to_delegate(b, e->as.let_.bindings[i].init)) return false;
                /* A local closure of a colored lambda: its calls become the
                 * lambda's CPS calls (pap_register_let), which delegating the
                 * let whole would turn back into fresh-root direct calls. */
                const Binding *lc = cps_ir_let_local_closure(e, i);
                if (lc && callee_colored(b, lc)) return false;
            }
            return safe_to_delegate(b, e->as.let_.body);
        case EX_WHILE:
            return safe_to_delegate(b, e->as.while_.cond)
                && safe_to_delegate(b, e->as.while_.body);
        case EX_MATCH:
            if (!safe_to_delegate(b, e->as.match_.scrutinee)) return false;
            for (uint32_t i = 0; i < e->as.match_.n_arms; i++) {
                if (e->as.match_.arms[i].guard
                    && !safe_to_delegate(b, e->as.match_.arms[i].guard)) return false;
                if (!safe_to_delegate(b, e->as.match_.arms[i].body)) return false;
            }
            return true;
        case EX_SET:
            return safe_to_delegate(b, e->as.set_.value);
        case EX_CAST:
            return safe_to_delegate(b, e->as.cast_.expr);
        case EX_DEREF:
            return safe_to_delegate(b, e->as.deref_.expr);
        case EX_GET_FIELD:
            return safe_to_delegate(b, e->as.get_field_.struct_expr);
        case EX_MAKE_STRUCT:
            for (uint32_t i = 0; i < e->as.make_struct_.n_fields; i++)
                if (!safe_to_delegate(b, e->as.make_struct_.field_values[i])) return false;
            return true;
        /* B3 part 2: a first-class handler VALUE construction (a `(handler ...)`
         * literal or `(compose-handlers ...)`) is a pure, control-op-free value
         * the direct emitter builds as a tur_handler_table_t* -- and, under the
         * flag, also emits DK-ABI case fns into the table (emit_effects_handler_lit)
         * so a downstream dynamic with-handler can install it on the DK.  Delegate
         * the whole construction (e.g. `(HRow (handler ...))`) so the enclosing
         * colored fn is not evicted by an "unsupported EX_HANDLER_LIT" form. */
        case EX_HANDLER_LIT:
            return true;
        case EX_COMPOSE_HANDLERS:
            return safe_to_delegate(b, e->as.compose_handlers_.h1)
                && safe_to_delegate(b, e->as.compose_handlers_.h2);
        case EX_DEFAULT_OF:
            return true;
        /* Owning-value ops (rc/of, rc/clone, rc/drop, rc/strong-count, rc->ptr)
         * are control-op-free leaf ops the direct emitter already emits with the
         * correct control-block / refcount / drop-glue discipline; delegating a
         * form that contains one (e.g. `(make-struct S :field (rc/of x))`, a
         * constructor call whose argument allocates) keeps the whole thing on the
         * direct emitter -- which also resolves the monomorphized ctor name. */
        case EX_RC_OF:    return safe_to_delegate(b, e->as.rc_of_.expr);
        case EX_RC_CLONE: return safe_to_delegate(b, e->as.rc_clone_.expr);
        case EX_RC_DROP:  return safe_to_delegate(b, e->as.rc_drop_.expr);
        case EX_RC_COUNT: return safe_to_delegate(b, e->as.rc_count_.expr);
        case EX_RC_PTR:   return safe_to_delegate(b, e->as.rc_ptr_.expr);
        /* P1 (cps-runtime-finish, Phase 1): control-op-free leaf forms the direct
         * emitter emits wholesale as pure expressions/terminators.  Each is
         * delegatable exactly when its operand(s) are -- no control op and no
         * colored/indirect call can hide inside.
         *   - EX_PANIC: a no-successor terminator (emit_value emits the abort +
         *     panic-signal return, then yields a nil placeholder; the CPS
         *     continuation after it is dead but well-formed C).
         *   - EX_CONS_LIST: a pure right-folded `__tur_cons_of(...)` heap build
         *     (the `& rest` variadic argument constructor).
         *   - EX_BORROW_IMMUT: `(& x)` -- an address-of / pointer expression over
         *     an in-scope local; no control op, no allocation. */
        case EX_PANIC:    return safe_to_delegate(b, e->as.panic_.payload);
        case EX_CONS_LIST:
            for (uint32_t i = 0; i < e->as.cons_list_.n; i++)
                if (!safe_to_delegate(b, e->as.cons_list_.items[i])) return false;
            return true;
        case EX_BORROW_IMMUT:
            return safe_to_delegate(b, e->as.borrow_immut_.expr);
        /* O1-b: an owning `ref<T>` constructor is a control-op-free leaf alloc the
         * direct emitter emits with the right malloc/drop glue.  Delegating it is
         * gated in letraw_ok by ref_dropped_before_control: the ref's hoisted
         * `drop!` must precede every control op (the non-crossing P1 slice), so a
         * crossing ref -- whose auto-drop defer is not hoisted and stays an
         * unlowered EX_DEFER, or a moved ref with no in-scope drop -- falls back. */
        case EX_REF:      return safe_to_delegate(b, e->as.ref_.expr);
        /* P4 (cps-runtime-finish): a `defer` -- explicit `(defer ...)` or the
         * auto-inserted RC-drop / owning-value cleanup -- runs its body at the
         * enclosing block's scope exit via the direct emitter's `tur_frame`
         * discipline (init / push_defer / fire_lifo).  A `__cps` function
         * establishes NO defer frame of its own, so a defer is delegatable ONLY
         * inside a WHOLE-BODY delegation (g_whole_body_delegate): there the direct
         * emitter emits the entire body region -- frame setup, the defer push, the
         * guarded code, and the scope-exit fire -- as one CT_LETRAW, then the DK
         * continuation runs, so the defer fires exactly at its lexical scope.
         * Outside a whole-body probe (per-node decompose), delegating a lone defer
         * would register it into a sub-region frame that pops early -- or push it
         * where no frame is emitted -- so it stays non-delegatable and the function
         * evicts honestly on EX_DEFER (e.g. `effect-defer`: a defer sharing a `do`
         * with a `perform` genuinely needs native defer-frame lowering).  This
         * admits the control-op-free colored bodies -- `unsafe`-marked mains
         * carrying an rc/of auto-drop (`unsafe-basic`/`-nested`/`-defer`) -- whose
         * whole body the direct emitter owns, without the reshuffle-into-
         * STRUCT-OR-TAINT that a gateless widening would cause. */
        case EX_DEFER:    return g_whole_body_delegate
                              && safe_to_delegate(b, e->as.defer_.body);
        /* B8: a control-op-free inline-C EXPRESSION (a session channel op --
         * make-session/send/recv/close -- lowers to one, a synchronous
         * value-returning `tur_session_*` call) is delegatable exactly like a
         * direct-emitted value op (EX_RC_OF, EX_MAKE_STRUCT): it runs to
         * completion and never threads a DK continuation, so a body that
         * interleaves `perform` with inline-C session ops CPS-lowers with the
         * inline-C as a CT_LETRAW.  Unconditional since cps-tramp-resume
         * graduated (2026-07-19). */
        case EX_INLINE_C:  return true;
        /* cps-evicts-handle-in-operand-positions: a reinterpret is a pure value
         * conversion the direct emitter lowers in place, so it is exactly as
         * delegatable as the value it converts.  A generic call whose result
         * the elaborator re-typed to its instantiation -- `(g2 "s")` as the
         * argument of an effectful fn-field call under a `handle` -- is
         * wrapped in one, and evicted the whole function. */
        case EX_REINTERPRET:
            return safe_to_delegate(b, e->as.reinterpret_.expr);
        default:
            return false;   /* conservative: unrecognized form -> not delegatable */
    }
}

/* Every argument of a call is control-op-free and colored-call-free, so the
 * WHOLE call can be handed to the direct emitter (which emits the args inline and
 * resolves the monomorphized callee name -- constructors, specialized fns).  This
 * is a superset of call_args_atomic: it also delegates a call whose argument is a
 * non-atomic-but-delegatable form (e.g. `(rc/of x)`, a nested make-struct), which
 * would otherwise be atomized into a CT_LETCALL that mis-names the callee. */
static bool call_args_delegatable(CpsB *b, const Expr *e) {
    for (uint32_t i = 0; i < e->as.call_.n_args; i++)
        if (!safe_to_delegate(b, e->as.call_.args[i])) return false;
    return true;
}

static CAtom atomize(CpsB *b, Expr *e, Pending *p) {
    if (is_atomic(e)) {
        /* cps-while-native read-after-set: a straight-line read of a carried var
         * after its set! resolves to the var's `$next` CVar (recorded by
         * loop_rs_scan, keyed by node identity). */
        const Expr *pe = ascribe_peel(e);
        for (uint32_t i = 0; i < b->rs_n; i++)
            if (b->rs_nodes[i] == pe) return atom_cvar(b->rs_cvars[i]);
        return atom_of(e);
    }
    CVar x = fresh_cvar(b, e ? &e->type : NULL);
    if (p->n < 32) { p->items[p->n].expr = e; p->items[p->n].x = x; p->n++; }
    return atom_cvar(x);
}

/* Atomize every argument of a `fn_cps` call, left to right. */
static CAtom *fncps_atomize_args(CpsB *b, const Expr *e, Pending *pp) {
    uint32_t n = e->as.call_.n_args;
    CAtom *args = arena_alloc(b->a, (n ? n : 1) * sizeof(CAtom));
    for (uint32_t i = 0; i < n; i++)
        args[i] = atomize(b, e->as.call_.args[i], pp);
    return args;
}

/* Wrap `core` with the pending bindings, leftmost outermost. */
static CTerm *fold_pending(CpsB *b, Pending *p, CTerm *core) {
    for (int i = (int)p->n - 1; i >= 0; i--) {
        b->reap_closure_expr = p->items[i].reap_ok ? p->items[i].expr : NULL;
        core = cps_bind(b, p->items[i].expr, p->items[i].x, core);
        b->reap_closure_expr = NULL;
    }
    return core;
}

/* Does argument slot `i` of `call` keep nothing of what it is handed?  The
 * callee is statically known and its parameter is `^borrow`, or the
 * elaborator inferred it non-retaining (nonretain_param_mask: the body only
 * calls it, or passes it to a slot that does the same) -- exactly the slots
 * the direct emitter's escape walk (binding_escapes_impl) admits. */
static bool call_slot_nonretaining(const Expr *call, uint32_t i) {
    if (!call || call->kind != EX_CALL || call->as.call_.fn_expr) return false;
    if (!call_dispatch_is_static(call)) return false;
    const Binding *fb = call->as.call_.fn_binding;
    if (!fb || fb->type.kind != TY_FN) return false;
    if (i < fb->type.as.fn.arity && fb->type.as.fn.arg_flags &&
        FN_ARG_FLAG(fb->type.as.fn, i, FA_BORROW))
        return true;
    return i < 32 && (fb->nonretain_param_mask & (1u << i));
}

/* atomize() for argument `i` of `call`, recording whether its slot retains
 * it (see PendItem.reap_ok). */
static CAtom atomize_call_arg(CpsB *b, Expr *call, uint32_t i, Pending *p) {
    uint32_t before = p->n;
    CAtom a = atomize(b, call->as.call_.args[i], p);
    if (p->n == before + 1)
        p->items[before].reap_ok = call_slot_nonretaining(call, i);
    return a;
}

static const char *builtin_name(const Expr *e) {
    if (e->kind == EX_BUILTIN && e->as.builtin.spec && e->as.builtin.spec->name)
        return e->as.builtin.spec->name;
    return "?";
}

/* The value a `(shift k_fn body)` delivers to its prompt is `k_fn(body)` (the
 * abortive-shift semantics: the receiver is applied to the body value, and the
 * result becomes the enclosing reset's value; see eval_abortive_shift).  Model
 * that by synthesizing the application `(k_fn body)` and CPS-translating it to
 * the prompt continuation.  A receiver that is not a directly-callable binding
 * leaves fn_binding NULL, so cps_tail yields CT_UNSUPPORTED and the backend
 * falls back cleanly. */
/* A shift / shift0 body: apply the receiver `recv` (the (fn [k] ...) that gets
 * the captured continuation) to the delimited body expression `arg`, delivered
 * to the prompt.  Shared by both the plain and shift0 lowerings (which differ
 * only in the emitted dk_shift vs dk_shift0). */
static CTerm *cps_shift_body_kf(CpsB *b, Expr *recv, Expr *arg, Type *ty) {
    /* A CLOSURE receiver `(fn [v] body)` -- the abortive-shift value is
     * `receiver(arg)` = `body[v := arg]`.  A capturing closure's value is its env
     * pointer, so a synthesized `(closure arg)` is an indirect call the direct
     * emitter cannot lower (indirect_callee_ok rejects it) and the whole function
     * evicts.  Beta-reduce instead: bind the user param to arg and deliver the
     * closure body to the prompt directly.  The body still references its captured
     * source bindings (closure conversion prepends the env param but leaves the
     * body's capture references as the original bindings, resolved to env fields
     * only when the thunk is emitted -- so inlining resolves them to the visible
     * reset-context locals, which the lifted shift body captures via collect_caps).
     * A non-capturing lambda is already lifted to a named global (an EX_VAR here),
     * so this fires only for the capturing case that otherwise evicts. */
    {
        const Expr *rp = recv ? ascribe_peel(recv) : NULL;
        if (rp && rp->kind == EX_CLOSURE && rp->as.closure_.closure
            && rp->as.closure_.closure->fn && rp->as.closure_.closure->fn->body
            && rp->as.closure_.closure->fn->n_params >= 1) {
            FnDef *cfn = rp->as.closure_.closure->fn;
            /* The user param is the last one (the env, if any, is prepended). */
            Expr *let = arena_alloc(b->a, sizeof(Expr));
            memset(let, 0, sizeof(Expr));
            let->kind = EX_LET;
            let->type = *ty;
            LetBinding *lb = arena_alloc(b->a, sizeof(LetBinding));
            memset(lb, 0, sizeof(LetBinding));
            lb->binding = cfn->params[cfn->n_params - 1];
            lb->init = arg;
            let->as.let_.bindings = lb;
            let->as.let_.n = 1;
            let->as.let_.body = cfn->body;
            return cps_tail(b, let, kont_prompt(ty->kind));
        }
    }
    Expr *call = arena_alloc(b->a, sizeof(Expr));
    memset(call, 0, sizeof(Expr));
    call->kind = EX_CALL;
    call->type = *ty;
    call->as.call_.fn_binding =
        (recv && recv->kind == EX_VAR) ? recv->as.var.binding : NULL;
    call->as.call_.fn_expr = recv;
    call->as.call_.args = arena_alloc(b->a, sizeof(Expr *));
    call->as.call_.args[0] = arg;
    call->as.call_.n_args = 1;
    return cps_tail(b, call, kont_prompt(ty->kind));
}

static CTerm *cps_shift_body(CpsB *b, Expr *e) {
    return cps_shift_body_kf(b, e->as.shift_.k_fn, e->as.shift_.body, &e->type);
}

/* ---- algebraic effects: handle / perform / resume --------------------- *
 * Lowered to CT_HANDLE / CT_PERFORM / CT_RESUME.  `cont` is the continuation
 * term (an APPCONT to the enclosing kont in tail position, or the `rest` term in
 * bind position).  perform/resume atomize their sub-expressions into `p`. */

/* Build a CT_HANDLE from a HandleExpr `h` (its CASES) run over `body_expr` (the
 * delimited region), delivering the result to `cont` bound at `x`.  `result_ty` is
 * the delim body's value type.  Shared by EX_HANDLE (body == h->body) and
 * EX_WITH_HANDLER with a handler-literal (body == the with-handler body, cases
 * from the literal). */
static CTerm *build_handle_core(CpsB *b, HandleExpr *h, Expr *body_expr,
                                TypeKind result_ty, CVar x, CTerm *cont) {
    if (!h || h->n_cases < 1) {
        CTerm *u = new_term(b, CT_UNSUPPORTED);
        u->as.unsupported.why = "handle: no cases";
        return u;
    }
    /* An `(unsafe ...)` block desugars to a handle on the built-in Unsafe effect,
     * but Unsafe is a pure compile-time MARKER that is never performed at runtime
     * (is_unsafe_marker): the handler never fires.  So the handle is semantically
     * transparent -- lower it to its body directly rather than a CT_HANDLE.  This
     * keeps the body's operations (e.g. delegated owning inline-C allocs in the
     * sized-bitvec/matrix mains) in the plain function body, where term_core_ok
     * admits a CT_LETRAW -- instead of inside a handle delim, where handle_delim_ok
     * (correctly) rejects a fiber-runtime CT_LETRAW.  A REAL effect performed
     * inside the body still threads the enclosing DK continuation natively. */
    if (h->is_unsafe_marker)
        return cps_bind(b, body_expr, x, cont);
    CTerm *t = new_term(b, CT_HANDLE);
    t->as.handle.x = x;
    t->as.handle.delim = cps_tail(b, body_expr, kont_prompt(result_ty));
    t->as.handle.n_cases = h->n_cases;
    t->as.handle.shallow = h->shallow;   /* F2: handle-shallow -> dk_handler_shallow */
    CHandleCase *cs = arena_alloc(b->a, h->n_cases * sizeof(CHandleCase));
    for (uint32_t ci = 0; ci < h->n_cases; ci++) {
        HandleCase *c = &h->cases[ci];
        cs[ci].effect = c->effect_name;
        cs[ci].n_params = c->n_params;
        cs[ci].params = NULL;
        if (c->n_params) {
            const Binding **ps = arena_alloc(b->a, c->n_params * sizeof(const Binding *));
            for (uint32_t i = 0; i < c->n_params; i++) ps[i] = c->param_bindings[i];
            cs[ci].params = ps;
        }
        cs[ci].k = c->k_binding;
        /* cps-dk-multishot-user-effects (Phase A): a resumable-payload handler
         * needs the DK-backed cloneable-cont wrap + boxed-payload reap at emit. */
        cs[ci].resumable_payload = c->resumable_payload;
        /* The case clause delivers its value by return (KK_PROMPT), which
         * dk_perform routes to the handler's outer continuation.  cps-async: mark
         * the case-body descent so a nested `await` delegates to the fiber path
         * (CT_LETRAW) instead of a CT_AWAIT the handler-case admission cannot host. */
        b->in_handler_case++;
        cs[ci].case_body = cps_tail(b, c->body, kont_prompt(c->body ? c->body->type.kind : TY_INT));
        b->in_handler_case--;
    }
    t->as.handle.cases = cs;
    t->as.handle.body = cont;
    return t;
}

static CTerm *build_handle(CpsB *b, Expr *e, CVar x, CTerm *cont) {
    HandleExpr *h = e->as.handle_.handle;
    return build_handle_core(b, h, h ? (Expr *)h->body : NULL, e->type.kind, x, cont);
}

/* B3: is `hv` a compose-tree whose every leaf is a `(handler ...)` LITERAL?
 * Such a tree is statically knowable -- its cases merge into one multi-effect
 * DK handler group.  A dynamic leaf (a variable / field read) is not. */
static bool compose_all_literal(const Expr *hv) {
    hv = ascribe_peel(hv);
    if (!hv) return false;
    if (hv->kind == EX_HANDLER_LIT) return true;
    if (hv->kind == EX_COMPOSE_HANDLERS)
        return compose_all_literal(hv->as.compose_handlers_.h1)
            && compose_all_literal(hv->as.compose_handlers_.h2);
    return false;
}

/* Count the total handler cases across a compose-tree of literals. */
static uint32_t compose_case_count(const Expr *hv) {
    hv = ascribe_peel(hv);
    if (!hv) return 0;
    if (hv->kind == EX_HANDLER_LIT)
        return hv->as.handler_lit_.handle ? hv->as.handler_lit_.handle->n_cases : 0;
    if (hv->kind == EX_COMPOSE_HANDLERS)
        return compose_case_count(hv->as.compose_handlers_.h1)
             + compose_case_count(hv->as.compose_handlers_.h2);
    return 0;
}

/* Copy every case of a compose-tree of literals into `out` (in h1-outer order,
 * matching the fiber-path table concatenation in emit_effects_compose_handlers). */
static void compose_fill_cases(const Expr *hv, HandleCase *out, uint32_t *k) {
    hv = ascribe_peel(hv);
    if (!hv) return;
    if (hv->kind == EX_HANDLER_LIT) {
        HandleExpr *h = hv->as.handler_lit_.handle;
        if (h) for (uint32_t i = 0; i < h->n_cases; i++) out[(*k)++] = h->cases[i];
    } else if (hv->kind == EX_COMPOSE_HANDLERS) {
        compose_fill_cases(hv->as.compose_handlers_.h1, out, k);
        compose_fill_cases(hv->as.compose_handlers_.h2, out, k);
    }
}

/* E2 (cps-tramp-resume): `(with-handler hv body)` where hv is a `(handler ...)`
 * LITERAL is exactly `(handle body <hv-cases>)` -- reuse build_handle_core with the
 * literal's cases and the with-handler body, so a fn that discharges one effect via
 * with-handler and leaves a leftover (fh-discharge-row's do-work) DK-lowers, its
 * leftover threading to the enclosing handler.
 *
 * B3: a `(compose-handlers ...)` whose leaves are all literals merges its cases
 * into a single multi-effect DK handler group over the body (a multi-case handle
 * dispatches per effect tag, exactly what compose installs).  A DYNAMIC handler
 * value (a variable, a `(.field obj)` read, a compose with a dynamic leaf) is not
 * statically known here -> evict (unchanged). */
static CTerm *build_with_handler(CpsB *b, Expr *e, CVar x, CTerm *cont) {
    const Expr *hv = ascribe_peel(e->as.with_handler_.handler);
    Expr *body = e->as.with_handler_.body;
    if (hv && hv->kind == EX_HANDLER_LIT)
        return build_handle_core(b, hv->as.handler_lit_.handle, body,
                                 e->type.kind, x, cont);
    if (hv && hv->kind == EX_COMPOSE_HANDLERS && compose_all_literal(hv)) {
        uint32_t nc = compose_case_count(hv);
        if (nc >= 1 && nc <= 255) {
            HandleCase *cases = arena_alloc(b->a, nc * sizeof(HandleCase));
            uint32_t k = 0;
            compose_fill_cases(hv, cases, &k);
            HandleExpr *merged = arena_alloc(b->a, sizeof(HandleExpr));
            memset(merged, 0, sizeof(HandleExpr));
            merged->cases = cases;
            merged->n_cases = (uint8_t)nc;
            merged->shallow = false;   /* compose installs deep handlers */
            return build_handle_core(b, merged, body, e->type.kind, x, cont);
        }
    }
    /* B3 part 2: DYNAMIC handler value (a variable, a `(.field obj)` read, a
     * compose with a dynamic leaf) -- the cases are not statically known.  Bind
     * the handler value to a temp, then install a DK handler group from its
     * runtime table (dk_hgroup_from_table) over the with-handler body.  The DK
     * case fns were emitted at the handler literal's creation site (step 2).  A
     * value whose literals were NOT DK-emittable carries dk_fn=0 entries; the
     * runtime install skips those, so such an effect surfaces as unhandled rather
     * than a crash (a value all of whose cases DK-lower installs fully). */
    CVar tvar = fresh_cvar(b, hv ? &hv->type : NULL);
    CTerm *h = new_term(b, CT_HANDLE);
    h->as.handle.x = x;
    h->as.handle.delim = cps_tail(b, body, kont_prompt(e->type.kind));
    h->as.handle.n_cases = 0;
    h->as.handle.cases = NULL;
    h->as.handle.shallow = false;
    h->as.handle.dyn = true;
    h->as.handle.dyn_table = atom_cvar(tvar);
    h->as.handle.body = cont;
    return cps_bind(b, (Expr *)hv, tvar, h);
}

static CTerm *build_perform(CpsB *b, Expr *e, CVar x, CTerm *cont, Pending *p) {
    PerformExpr *pf = e->as.perform_.perform;
    CTerm *t = new_term(b, CT_PERFORM);
    t->as.perform.effect = pf->effect_name;
    t->as.perform.resumable_payload = pf->resumable_payload;
    t->as.perform.n = pf->n_args;
    CAtom *args = arena_alloc(b->a, (pf->n_args ? pf->n_args : 1) * sizeof(CAtom));
    for (uint32_t i = 0; i < pf->n_args; i++) args[i] = atomize(b, pf->args[i], p);
    t->as.perform.args = args;
    t->as.perform.x = x;
    t->as.perform.body = cont;
    return t;
}

/* F3 (cps-async): (await fut) as a heap-continuation shift.  Mirrors
 * build_perform: atomize the future, bind the awaited value to x, carry the
 * continuation in body.  emit_await lowers it to a dk_shift against the entry
 * prompt whose body is the __tur_await_body runtime helper. */
static CTerm *build_await(CpsB *b, Expr *e, CVar x, CTerm *cont, Pending *p) {
    CTerm *t = new_term(b, CT_AWAIT);
    t->as.await.fut = atomize(b, e->as.await_.fut_expr, p);
    t->as.await.x = x;
    t->as.await.body = cont;
    return t;
}

static CTerm *build_resume(CpsB *b, Expr *e, CVar x, CTerm *cont, Pending *p) {
    ResumeExpr *r = e->as.resume_.resume;
    CTerm *t = new_term(b, CT_RESUME);
    t->as.resume.k = atomize(b, r->k, p);
    t->as.resume.v = atomize(b, r->value, p);
    t->as.resume.x = x;
    t->as.resume.body = cont;
    return t;
}

/* ======================================================================= *
 * O1-b: ref<T> scope-exit auto-drop hoisting on the CPS path
 * (cps-backend-ref-scope-exit-drop-plan.md, P1).
 *
 * The elaborator injects `(defer (drop! r))` at let-scope exit for an owning
 * `ref<T>` local (elab_forms.c) -- the single ownership discharge.  EX_DEFER has
 * no CT-IR lowering, so a colored function still carrying such an auto-drop falls
 * back wholesale.  P1 converts the tractable slice: a ref whose live range does
 * NOT cross a control op.  There the scope-exit drop is a drop-at-last-use
 * hoisted to the wrong place -- we recognise the auto-drop shape, verify
 * non-crossing, and emit the drop straight-line BEFORE the first control op (or
 * at scope exit when the ref's do has none), delegated to the direct emitter via
 * CT_LETRAW exactly like an explicit rc/drop (O1-a).  Because the drop precedes
 * every control op, `owning_dropped_before_control` is satisfied and the drop is
 * never captured into a reified continuation.  A ref that crosses a control op,
 * or a non-auto-drop user `defer`, keeps falling back -- P2/P3 own those.
 * ======================================================================= */

static bool autodrop_owning_kind(TypeKind k) {
    return k == TY_RC || k == TY_REF || k == TY_WEAK || k == TY_LREF;
}

/* The ROOT LOCAL of an auto-drop's operand: a bare owning var, or the by-value
 * struct/record local under a field read `(.f o)` (peel EX_GET_FIELD).  This is
 * the binding whose live range the crossing check keys on -- a field-drop of `o`
 * counts `o` as the crossed value.  Returns NULL if the operand is not rooted at
 * a bare local. */
static const Binding *autodrop_root_local(const Expr *arg) {
    arg = ascribe_peel(arg);
    while (arg && arg->kind == EX_GET_FIELD)
        arg = ascribe_peel(arg->as.get_field_.struct_expr);
    if (arg && arg->kind == EX_VAR && arg->as.var.binding)
        return arg->as.var.binding;
    return NULL;
}

/* If `e` is an elaborator-injected owning scope-exit auto-drop, return the
 * discharged value's ROOT LOCAL binding; else NULL.  Generalizes the O1-b `ref`
 * recognizer to every owning auto-drop shape the elaborator injects (bare rc /
 * ref / weak / lref, and a by-value struct's owning field), and NOTHING else so
 * a general user `defer` of arbitrary effects is never hoisted:
 *   (defer (rc/drop X))   -- X a bare rc var or a field read (.f o)
 *   (defer (drop! X))     -- X a bare ref/weak/lref var or a field read (.f o)
 * The body must be a pure drop (`EX_RC_DROP`, or the `drop!` free-shape builtin)
 * whose operand is an owning value (checked), rooted at a bare local. */
static const Binding *autodrop_defer_owning(const Expr *e) {
    if (!e || e->kind != EX_DEFER) return NULL;
    const Expr *body = ascribe_peel(e->as.defer_.body);
    if (!body) return NULL;
    const Expr *arg;
    if (body->kind == EX_RC_DROP) {
        arg = body->as.rc_drop_.expr;
    } else if (body->kind == EX_BUILTIN) {
        const BuiltinSpec *sp = body->as.builtin.spec;
        if (!sp || sp->shape != BS_PREFIX_UNARY_FREE || body->as.builtin.n != 1)
            return NULL;
        arg = body->as.builtin.args[0];
    } else {
        return NULL;
    }
    /* the drop operand (a bare owning var or a `(.f o)` field read) must be
     * owning-typed -- guards against a non-drop free-shape builtin. */
    const Expr *op = ascribe_peel(arg);
    TypeKind opk = TY_UNKNOWN;
    if (op && op->kind == EX_GET_FIELD) opk = op->type.kind;
    else if (op && op->kind == EX_VAR && op->as.var.binding)
        opk = op->as.var.binding->type.kind;
    if (!autodrop_owning_kind(opk)) return NULL;
    return autodrop_root_local(arg);
}

/* The `(drop! r)` builtin inside an auto-drop defer, delegated whole to the
 * direct emitter (its operand is the atomic ref var). */
static Expr *autodrop_defer_body(const Expr *e) {
    Expr *body = (Expr *)ascribe_peel(e->as.defer_.body);
    return body;
}

/* Does `e` contain a control operator (the coloring seed set)?  Mirrors
 * cps_directly_uses_control (src/passes/cps.c): the named control-op nodes are
 * seeds, structural children recurse, nested fn bodies are call-graph boundaries,
 * and an un-modelled node is NOT a control op (default false).  Used to locate
 * the first control barrier a hoisted drop must precede.  Over-approximating the
 * barrier set (treating reset/handle/perform/resume as barriers too) is sound --
 * it only forces more fallback; the genuine abortive/multi-shot seeds (shift and
 * friends, resume, discontinue) are all covered. */
static bool item_has_control(const Expr *e) {
    e = ascribe_peel(e);
    if (!e) return false;
    switch (e->kind) {
        case EX_RESET:
        case EX_SHIFT:
        case EX_SHIFT0:
        case EX_CLONEABLE_RESET:
        case EX_CLONEABLE_SHIFT:
        case EX_SERIAL_RESET:
        case EX_SERIAL_SHIFT:
        case EX_PERFORM:
        case EX_HANDLE:
        case EX_RESUME:
        case EX_DISCONTINUE:
        case EX_CALLCC:
            return true;
        case EX_LET:
            for (uint32_t i = 0; i < e->as.let_.n; i++)
                if (item_has_control(e->as.let_.bindings[i].init)) return true;
            return item_has_control(e->as.let_.body);
        case EX_IF:
            return item_has_control(e->as.if_.cond)
                || item_has_control(e->as.if_.then_)
                || item_has_control(e->as.if_.else_or_null);
        case EX_DO:
            for (uint32_t i = 0; i < e->as.do_.n; i++)
                if (item_has_control(e->as.do_.items[i])) return true;
            return false;
        case EX_BUILTIN:
            for (uint32_t i = 0; i < e->as.builtin.n; i++)
                if (item_has_control(e->as.builtin.args[i])) return true;
            return false;
        case EX_CALL:
            if (item_has_control(e->as.call_.fn_expr)) return true;
            for (uint32_t i = 0; i < e->as.call_.n_args; i++)
                if (item_has_control(e->as.call_.args[i])) return true;
            return false;
        case EX_DEFER:  return item_has_control(e->as.defer_.body);
        case EX_RETURN: return item_has_control(e->as.return_.value);
        case EX_SET:    return item_has_control(e->as.set_.value);
        case EX_DEREF:  return item_has_control(e->as.deref_.expr);
        case EX_GET_FIELD:
            return item_has_control(e->as.get_field_.struct_expr);
        case EX_MAKE_STRUCT:
            for (uint32_t i = 0; i < e->as.make_struct_.n_fields; i++)
                if (item_has_control(e->as.make_struct_.field_values[i])) return true;
            return false;
        case EX_RC_OF:    return item_has_control(e->as.rc_of_.expr);
        case EX_RC_CLONE: return item_has_control(e->as.rc_clone_.expr);
        case EX_RC_DROP:  return item_has_control(e->as.rc_drop_.expr);
        case EX_REINTERPRET: return item_has_control(e->as.reinterpret_.expr);
        /* Nested fn definitions are call-graph boundaries. */
        case EX_FN_DEF: case EX_FN: case EX_CLOSURE:
            return false;
        default:
            return false;
    }
}

/* P2 gate: does `e` contain a control op whose continuation is NOT single-shot --
 * i.e. one whose post-op continuation may be DISCARDED (abortive) or run more
 * than once (delimited-multishot / escape)?  A scope-exit owning drop that
 * crosses such an op is unsound (the drop is lost or duplicated), so P2 falls
 * back on it (-> P3 / E3).  Single-shot effect ops (handle / perform / resume:
 * their post-op continuation runs exactly once in the admitted subset) are NOT
 * unsafe in themselves, but a nested unsafe op inside their bodies is -- so
 * recurse into them.  CONSERVATIVE: an un-modelled node returns true (assume
 * unsafe), so P2 only engages on shapes it can prove single-shot. */
static bool expr_has_unsafe_control(const Expr *e) {
    e = ascribe_peel(e);
    if (!e) return false;
    if (is_atomic(e)) return false;
    switch (e->kind) {
        /* abortive / delimited-multishot / escape: NOT single-shot. */
        case EX_SHIFT: case EX_SHIFT0: case EX_RESET:
        case EX_CLONEABLE_SHIFT: case EX_CLONEABLE_RESET:
        case EX_SERIAL_SHIFT: case EX_SERIAL_RESET:
        case EX_CALLCC: case EX_DISCONTINUE:
        case EX_ASYNC: case EX_AWAIT:
            return true;
        /* single-shot effect ops: recurse into bodies (a nested shift is unsafe). */
        case EX_HANDLE: {
            const HandleExpr *h = e->as.handle_.handle;
            if (!h) return true;
            if (expr_has_unsafe_control(h->body)) return true;
            for (uint32_t i = 0; i < h->n_cases; i++)
                if (expr_has_unsafe_control(h->cases[i].body)) return true;
            return false;
        }
        case EX_PERFORM: {
            const PerformExpr *p = e->as.perform_.perform;
            if (!p) return true;
            for (uint32_t i = 0; i < p->n_args; i++)
                if (expr_has_unsafe_control(p->args[i])) return true;
            return false;
        }
        case EX_RESUME: {
            const ResumeExpr *r = e->as.resume_.resume;
            if (!r) return true;
            return expr_has_unsafe_control(r->k) || expr_has_unsafe_control(r->value);
        }
        /* nested fn defs / closures: call-graph boundaries (not run here). */
        case EX_FN: case EX_FN_DEF: case EX_CLOSURE:
            return false;
        /* structural containers: recurse. */
        case EX_LET: case EX_LETREC:
            for (uint32_t i = 0; i < e->as.let_.n; i++)
                if (expr_has_unsafe_control(e->as.let_.bindings[i].init)) return true;
            return expr_has_unsafe_control(e->as.let_.body);
        case EX_IF:
            return expr_has_unsafe_control(e->as.if_.cond)
                || expr_has_unsafe_control(e->as.if_.then_)
                || expr_has_unsafe_control(e->as.if_.else_or_null);
        case EX_DO:
            for (uint32_t i = 0; i < e->as.do_.n; i++)
                if (expr_has_unsafe_control(e->as.do_.items[i])) return true;
            return false;
        case EX_CALL:
            if (e->as.call_.fn_expr && expr_has_unsafe_control(e->as.call_.fn_expr)) return true;
            for (uint32_t i = 0; i < e->as.call_.n_args; i++)
                if (expr_has_unsafe_control(e->as.call_.args[i])) return true;
            return false;
        case EX_BUILTIN:
            for (uint32_t i = 0; i < e->as.builtin.n; i++)
                if (expr_has_unsafe_control(e->as.builtin.args[i])) return true;
            return false;
        case EX_WHILE:
            return expr_has_unsafe_control(e->as.while_.cond)
                || expr_has_unsafe_control(e->as.while_.body);
        case EX_MATCH:
            if (expr_has_unsafe_control(e->as.match_.scrutinee)) return true;
            for (uint32_t i = 0; i < e->as.match_.n_arms; i++) {
                if (e->as.match_.arms[i].guard
                    && expr_has_unsafe_control(e->as.match_.arms[i].guard)) return true;
                if (expr_has_unsafe_control(e->as.match_.arms[i].body)) return true;
            }
            return false;
        case EX_DEFER:     return expr_has_unsafe_control(e->as.defer_.body);
        case EX_RETURN:    return expr_has_unsafe_control(e->as.return_.value);
        case EX_SET:       return expr_has_unsafe_control(e->as.set_.value);
        case EX_DEREF:     return expr_has_unsafe_control(e->as.deref_.expr);
        case EX_CAST:      return expr_has_unsafe_control(e->as.cast_.expr);
        case EX_GET_FIELD: return expr_has_unsafe_control(e->as.get_field_.struct_expr);
        case EX_MAKE_STRUCT:
            for (uint32_t i = 0; i < e->as.make_struct_.n_fields; i++)
                if (expr_has_unsafe_control(e->as.make_struct_.field_values[i])) return true;
            return false;
        case EX_RC_OF:    return expr_has_unsafe_control(e->as.rc_of_.expr);
        case EX_RC_CLONE: return expr_has_unsafe_control(e->as.rc_clone_.expr);
        case EX_RC_DROP:  return expr_has_unsafe_control(e->as.rc_drop_.expr);
        case EX_RC_COUNT: return expr_has_unsafe_control(e->as.rc_count_.expr);
        case EX_RC_PTR:   return expr_has_unsafe_control(e->as.rc_ptr_.expr);
        case EX_REF:      return expr_has_unsafe_control(e->as.ref_.expr);
        default:
            return true;   /* conservative: un-modelled -> assume not single-shot */
    }
}

/* Conservative "does `e` reference binding `bid`?"  Used to verify a ref's live
 * range does not cross a control op before hoisting its auto-drop.  Over-detection
 * (an un-modelled node -> assume it might reference the ref) only widens the live
 * range and forces fallback, so the DEFAULT is `true`; under-detection would drop
 * a still-live ref and is never allowed.  The straight-line + shift shapes P1
 * actually hoists are walked precisely; anything else (perform/handle/resume/...)
 * is treated as a possible use and bails to fallback. */
static bool expr_refs_binding(const Expr *e, uint32_t bid) {
    e = ascribe_peel(e);
    if (!e) return false;
    switch (e->kind) {
        case EX_VAR:
            return e->as.var.binding && e->as.var.binding->id == bid;
        case EX_INT_LIT: case EX_BOOL_LIT: case EX_NIL_LIT:
        case EX_FLOAT_LIT: case EX_CSTR_LIT:
            return false;
        case EX_LET:
            for (uint32_t i = 0; i < e->as.let_.n; i++)
                if (expr_refs_binding(e->as.let_.bindings[i].init, bid)) return true;
            return expr_refs_binding(e->as.let_.body, bid);
        case EX_IF:
            return expr_refs_binding(e->as.if_.cond, bid)
                || expr_refs_binding(e->as.if_.then_, bid)
                || expr_refs_binding(e->as.if_.else_or_null, bid);
        case EX_DO:
            for (uint32_t i = 0; i < e->as.do_.n; i++)
                if (expr_refs_binding(e->as.do_.items[i], bid)) return true;
            return false;
        case EX_BUILTIN:
            for (uint32_t i = 0; i < e->as.builtin.n; i++)
                if (expr_refs_binding(e->as.builtin.args[i], bid)) return true;
            return false;
        case EX_CALL:
            if (expr_refs_binding(e->as.call_.fn_expr, bid)) return true;
            for (uint32_t i = 0; i < e->as.call_.n_args; i++)
                if (expr_refs_binding(e->as.call_.args[i], bid)) return true;
            return false;
        case EX_RETURN: return expr_refs_binding(e->as.return_.value, bid);
        case EX_DEFER:  return expr_refs_binding(e->as.defer_.body, bid);
        case EX_SET:
            return (e->as.set_.target && e->as.set_.target->id == bid)
                || expr_refs_binding(e->as.set_.value, bid);
        case EX_DEREF:  return expr_refs_binding(e->as.deref_.expr, bid);
        case EX_GET_FIELD:
            return expr_refs_binding(e->as.get_field_.struct_expr, bid);
        case EX_MAKE_STRUCT:
            for (uint32_t i = 0; i < e->as.make_struct_.n_fields; i++)
                if (expr_refs_binding(e->as.make_struct_.field_values[i], bid)) return true;
            return false;
        case EX_RC_OF:    return expr_refs_binding(e->as.rc_of_.expr, bid);
        case EX_RC_CLONE: return expr_refs_binding(e->as.rc_clone_.expr, bid);
        case EX_RC_DROP:  return expr_refs_binding(e->as.rc_drop_.expr, bid);
        case EX_REINTERPRET: return expr_refs_binding(e->as.reinterpret_.expr, bid);
        case EX_SHIFT:
            return expr_refs_binding(e->as.shift_.k_fn, bid)
                || expr_refs_binding(e->as.shift_.body, bid);
        case EX_SHIFT0:
            return expr_refs_binding(e->as.shift0_.k_fn, bid)
                || expr_refs_binding(e->as.shift0_.body, bid);
        case EX_RESET:  return expr_refs_binding(e->as.reset_.body, bid);
        /* A bare lambda / nested fn def captures no enclosing local; a closure
         * reaches `r` only if `r` is in its explicit capture list. */
        case EX_FN: case EX_FN_DEF:
            return false;
        case EX_CLOSURE: {
            const struct Closure *cl = e->as.closure_.closure;
            if (!cl) return true;
            for (uint8_t i = 0; i < cl->n_captures; i++)
                if (cl->captures[i] && cl->captures[i]->id == bid) return true;
            return false;
        }
        default:
            return true;   /* un-modelled: assume a possible use -> fall back */
    }
}

/* Plan for hoisting the trailing auto-drop defers of a `do` block. */
typedef struct {
    bool             engage;      /* hoist applies (all trailing autodrop, non-crossing) */
    uint32_t         n_real;      /* count of leading non-defer items to lower normally */
    uint32_t         first_ctrl;  /* index of first control-barrier real item, else n_real */
    Expr            *drops[8];    /* the (drop! r) builtins to emit, in source order */
    const Binding   *refs[8];     /* parallel ref bindings */
    uint32_t         n_drops;
} AutodropPlan;

/* Recognise a `do` block ending in owning-ref auto-drop defers and, when the
 * refs' live ranges do not cross a control op, produce a hoist plan.  Returns
 * false (no engage) for: a `do` with no trailing auto-drop defer; a trailing
 * defer that is not the auto-drop shape (user defer -> keep falling back); more
 * refs than the fixed plan holds; or any ref used at/after the first control
 * barrier (a crossing ref -> P2/P3, keep falling back). */
static bool plan_autodrop(const Expr *e, AutodropPlan *pl) {
    memset(pl, 0, sizeof(*pl));
    if (!e || e->kind != EX_DO || e->as.do_.n == 0) return false;
    uint32_t n = e->as.do_.n;
    Expr **items = e->as.do_.items;

    /* Count the contiguous trailing auto-drop defers; bail on a trailing
     * non-auto-drop defer (a general user `defer`). */
    uint32_t n_defer = 0;
    while (n_defer < n) {
        Expr *it = items[n - 1 - n_defer];
        if (it && it->kind == EX_DEFER) {
            if (!autodrop_defer_owning(it)) return false;  /* user defer -> fall back */
            n_defer++;
        } else {
            break;
        }
    }
    if (n_defer == 0) return false;             /* nothing to hoist */
    if (n_defer > 8) return false;              /* more than the plan holds */
    uint32_t n_real = n - n_defer;
    if (n_real == 0) return false;              /* a value item is required */

    /* Collect the drops in source order (items[n_real .. n)). */
    for (uint32_t i = 0; i < n_defer; i++) {
        Expr *d = items[n_real + i];
        pl->drops[i] = autodrop_defer_body(d);
        pl->refs[i]  = autodrop_defer_owning(d);
    }
    pl->n_drops = n_defer;
    pl->n_real  = n_real;

    /* First control barrier among the real items (n_real if none). */
    uint32_t fc = n_real;
    for (uint32_t i = 0; i < n_real; i++) {
        if (item_has_control(items[i])) { fc = i; break; }
    }
    pl->first_ctrl = fc;

    /* Non-crossing: no owning value may be referenced at/after the first barrier.
     * When there is no barrier (fc == n_real) the range is empty, so the drop
     * lands at scope exit after the (possibly owning-value-using) value item. */
    bool crossing = false;
    for (uint32_t i = fc; i < n_real && !crossing; i++)
        for (uint32_t k = 0; k < pl->n_drops; k++)
            if (expr_refs_binding(items[i], pl->refs[k]->id)) { crossing = true; break; }

    if (crossing) {
        /* P2: a CROSSING owning value (used at/after the barrier -- captured into
         * the control op) whose crossed control op(s) are all SINGLE-SHOT (their
         * post-op continuation runs exactly once) -- lower the drop at scope exit,
         * in that single-shot continuation, instead of hoisting (which would drop
         * a still-live value before its use).  This is how E1's explicit
         * `(rc/drop r)` after a `handle` already works; it unblocks an owning
         * value borrowed across a handle (Track B E2).  An abortive / delimited /
         * multishot crossing keeps falling back (P3 / E3): its post-op
         * continuation may discard or re-run the drop. */
        for (uint32_t i = 0; i < n_real; i++)
            if (expr_has_unsafe_control(items[i]))
                return false;                   /* not provably single-shot -> fall back */
        pl->first_ctrl = n_real;                /* route to the drops-at-exit branch */
    }

    pl->engage = true;
    return true;
}

/* Emit the hoisted drops (each delegated via CT_LETRAW) in front of `rest`,
 * innermost-last so they fire in reverse (LIFO) source order like the direct
 * defer stack.  Order is immaterial for correctness (distinct refs). */
static CTerm *cps_emit_hoisted_drops(CpsB *b, const AutodropPlan *pl, CTerm *rest) {
    for (int k = (int)pl->n_drops - 1; k >= 0; k--) {
        CVar dx = fresh_cvar(b, &pl->drops[k]->type);   /* nil-typed drop binder */
        rest = build_letraw(b, pl->drops[k], dx, rest);
    }
    return rest;
}

/* Extract the inlinable partial application from a let-binding init.  The
 * elaborator lowers `(TARGET c0 c1 ...)` (an under-saturated call) to
 * `(let [__papc0 c0 ...] CLOSURE)` -- a capture-binding prelude wrapping an
 * EX_CLOSURE over a synthesized `__pap` thunk whose body is the saturated
 * `(TARGET __papc0 ... rem...)`.  Recognize that shape (and a bare CLOSURE):
 * on success, `*target` = the underlying fn, `*caps`/`*n_caps` = the captured
 * arg bindings (leftmost-first), `*rem_arity` = the args a saturated call to the
 * closure supplies, and `*prelude` = the wrapping EX_LET (whose capture bindings
 * are emitted, whose CLOSURE body is dropped) or NULL for a bare closure. */
static bool pap_extract(const Expr *init, const Binding **target,
                        Binding ***caps, uint32_t *n_caps, uint32_t *rem_arity,
                        const Expr **prelude) {
    init = ascribe_peel(init);
    if (!init) return false;
    const Expr *pl = NULL;
    const Expr *clo = init;
    if (init->kind == EX_LET) { pl = init; clo = ascribe_peel(init->as.let_.body); }
    if (!clo || clo->kind != EX_CLOSURE || !clo->as.closure_.closure) return false;
    struct Closure *c = clo->as.closure_.closure;
    if (c->n_captures == 0) return false;                  /* capture-free: already a value */
    if (c->is_shift_receiver || c->is_effect_payload) return false;  /* reaped elsewhere */
    if (!c->fn || !c->fn->body) return false;
    const Expr *cbody = ascribe_peel(c->fn->body);
    if (!cbody || cbody->kind != EX_CALL) return false;    /* wrapper must be a single call */
    const Binding *tgt = cbody->as.call_.fn_binding;
    if (!tgt || tgt->type.kind != TY_FN) return false;     /* direct call to a named fn */
    uint32_t tarity = tgt->type.as.fn.arity;
    /* The wrapper body must be a FULLY saturated call to TARGET (n_caps captured +
     * remaining), so `(var rest)` reconstructs the complete arg list. */
    if (cbody->as.call_.n_args != tarity || tarity < c->n_captures) return false;
    /* NOTE for anyone tempted to tighten this with the closure's own arity:
     * `c->fn->n_params` is the LIFTED lambda's parameter count, which includes
     * the env pointer, not the source-level arity.  A `(fn [] ...)` thunk
     * measures n_params=1 here, so `n_params == tarity - n_captures` is
     * satisfied by a thunk that takes no arguments at all -- it does not
     * distinguish a partial application from a thunk, and imposing it would
     * reject genuine paps (whose lifted count is rem+1).  Tried while fixing
     * cps-direct-bt-scope-closure-temp-undeclared; it changed nothing there and
     * would have quietly disabled the optimization.  The real guard is in
     * `pap_calls_saturated` below. */
    /* The arity match says nothing about WHICH values the body passes: any
     * closure whose body is one saturated call has it.  The rewrite replaces
     * the body's arguments with the captures followed by the call site's
     * arguments, so it is only the same program when the body passes exactly
     * those, in that order -- capture i as argument i, then the thunk's own
     * parameters (its params[0] is the env).  An elaborator `__pap` has that
     * shape by construction; a written lambda may not.  `(fn [] (car (list
     * p 1)))` over one capture `p` matched the arity test (car takes one) and
     * was rewritten to `(car p)`; `(fn [x] (g x p))` to `(g p x)`
     * (cps-pap-inline-ignores-wrapper-arguments). */
    uint32_t rem = tarity - c->n_captures;
    if (c->fn->n_params != rem + 1) return false;
    for (uint32_t i = 0; i < tarity; i++) {
        const Expr *a = cbody->as.call_.args[i];
        if (a && a->kind == EX_POLY_WRAP) a = a->as.poly_wrap_.inner;
        const Binding *want = i < c->n_captures ? c->captures[i]
                                                : c->fn->params[1 + (i - c->n_captures)];
        if (!a || a->kind != EX_VAR || a->as.var.binding != want) return false;
    }
    *target = tgt; *caps = c->captures; *n_caps = c->n_captures;
    *rem_arity = rem; *prelude = pl;
    return true;
}

/* Conservative, complete check: is EVERY use of `var` in `e` a saturated direct
 * call `(var <rem_arity args>)`?  This makes the partial-application inlining
 * sound on its own: any unmodeled form returns false (bail, do not inline), any
 * wrong-arity call to `var` returns false, and any use of `var` that is not a
 * call CALLEE reaches the EX_VAR leaf below and returns false.
 *
 * That last clause used to be delegated -- the comment here said the property
 * was established by `closure_binding_escapes(e,var) == false`, "which proves
 * `var` appears ONLY as a call callee".  It does not, and never did:
 * `closure_binding_escapes` answers whether an env may be freed at scope exit,
 * and it deliberately reports NO escape for a value passed to a `^borrow` or
 * inferred-non-retaining fn param.  So `(bt-scope __borrowc)` -- a genuine
 * VALUE use of the closure -- cleared the check, the closure was dropped as
 * pap-inlined, and the emitted C named a temp nothing declared
 * (cps-direct-bt-scope-closure-temp-undeclared).  The check is local now; the
 * escape call remains at the registration site as a second, independent
 * condition rather than as this one's proof.
 *
 * This leaf is the WHOLE fix -- verified by disabling it and keeping everything
 * else, which reproduces the failure exactly. */
typedef struct { const Binding *var; uint32_t rem_arity; bool ok; } PapSatUd;
static bool pap_sat_visit(const Expr *c, void *ud);
static bool pap_calls_saturated(const Expr *e, const Binding *var, uint32_t rem_arity) {
    e = ascribe_peel(e);
    if (!e) return true;
    switch (e->kind) {
        case EX_NIL_LIT: case EX_BOOL_LIT: case EX_INT_LIT:
        case EX_FLOAT_LIT: case EX_CSTR_LIT:
            return true;
        /* A bare reference REACHED here is by construction a non-callee use: the
         * EX_CALL arm never recurses into a callee that is `var` (neither the
         * resolved `fn_binding` nor an `fn_expr` peeling to it), so anything
         * arriving at this leaf is an argument, an init, a branch value -- a use
         * of the closure as a VALUE, which inlining it away would break. */
        case EX_VAR:
            return e->as.var.binding != var;
        case EX_CALL: {
            const Expr *fe = e->as.call_.fn_expr;
            bool is_var_callee =
                (e->as.call_.fn_binding == var)
                || (fe && ascribe_peel(fe)->kind == EX_VAR
                    && ascribe_peel(fe)->as.var.binding == var);
            if (is_var_callee && e->as.call_.n_args != rem_arity) return false;
            /* fn_expr (when not the var callee) and every arg must also be clean. */
            if (fe && !(ascribe_peel(fe)->kind == EX_VAR
                        && ascribe_peel(fe)->as.var.binding == var)
                && !pap_calls_saturated(fe, var, rem_arity)) return false;
            for (uint32_t i = 0; i < e->as.call_.n_args; i++)
                if (!pap_calls_saturated(e->as.call_.args[i], var, rem_arity)) return false;
            return true;
        }
        case EX_LET: case EX_LETREC:
            for (uint32_t i = 0; i < e->as.let_.n; i++)
                if (!pap_calls_saturated(e->as.let_.bindings[i].init, var, rem_arity)) return false;
            return pap_calls_saturated(e->as.let_.body, var, rem_arity);
        case EX_IF:
            return pap_calls_saturated(e->as.if_.cond, var, rem_arity)
                && pap_calls_saturated(e->as.if_.then_, var, rem_arity)
                && pap_calls_saturated(e->as.if_.else_or_null, var, rem_arity);
        case EX_DO:
            for (uint32_t i = 0; i < e->as.do_.n; i++)
                if (!pap_calls_saturated(e->as.do_.items[i], var, rem_arity)) return false;
            return true;
        case EX_BUILTIN:
            for (uint32_t i = 0; i < e->as.builtin.n; i++)
                if (!pap_calls_saturated(e->as.builtin.args[i], var, rem_arity)) return false;
            return true;
        case EX_HANDLE:
            if (!pap_calls_saturated(e->as.handle_.handle->body, var, rem_arity)) return false;
            for (uint8_t i = 0; i < e->as.handle_.handle->n_cases; i++)
                if (!pap_calls_saturated(e->as.handle_.handle->cases[i].body, var, rem_arity)) return false;
            return true;
        case EX_PERFORM:
            for (uint8_t i = 0; i < e->as.perform_.perform->n_args; i++)
                if (!pap_calls_saturated(e->as.perform_.perform->args[i], var, rem_arity)) return false;
            return true;
        case EX_REINTERPRET:
            return pap_calls_saturated(e->as.reinterpret_.expr, var, rem_arity);
        case EX_ASCRIBE:
            return pap_calls_saturated(e->as.ascribe_.inner, var, rem_arity);
        case EX_MATCH:
            if (!pap_calls_saturated(e->as.match_.scrutinee, var, rem_arity)) return false;
            for (uint32_t i = 0; i < e->as.match_.n_arms; i++)
                if (!pap_calls_saturated(e->as.match_.arms[i].guard, var, rem_arity)
                    || !pap_calls_saturated(e->as.match_.arms[i].body, var, rem_arity))
                    return false;
            return true;
        /* A nested fn definition may capture `var` -- a use as a value the
         * enumeration below would not see (it does not enter fn bodies). */
        case EX_FN: case EX_CLOSURE: case EX_FN_DEF:
            return false;
        default: {
            /* Every other node: its evaluated operands, through the shared
             * enumeration -- a bare `var` among them is the EX_VAR leaf above,
             * a value use. */
            PapSatUd u = { var, rem_arity, true };
            cps_visit_children(e, pap_sat_visit, &u);
            return u.ok;
        }
    }
}
static bool pap_sat_visit(const Expr *c, void *ud) {
    PapSatUd *u = (PapSatUd *)ud;
    if (!pap_calls_saturated(c, u->var, u->rem_arity)) { u->ok = false; return true; }
    return false;
}

/* Build the saturated call that a pap-var call inlines to: `(TARGET cap0 ...
 * capN rest...)`.  The capture bindings stay in scope (emitted from the pap
 * prelude), so we reference them by fresh EX_VAR nodes; the remaining args come
 * straight from the original `(var rest...)` call. */
static Expr *pap_build_saturated_call(CpsB *b, const PapInline *pe, const Expr *call) {
    if (pe->is_env_call) {
        /* `(var rest...)` -> `(target var rest...)`: the lifted lambda's first
         * parameter is its env, and the closure's value IS the env box -- the
         * direct emitter spells the same call `__fn_N((void *)var, rest...)`. */
        uint32_t n = call->as.call_.n_args + 1;
        Expr **args = arena_alloc(b->a, n * sizeof(Expr *));
        Expr *v = expr_new(b->a, EX_VAR, pe->var->type, call->span);
        v->as.var.binding = (Binding *)pe->var;
        args[0] = v;
        for (uint32_t i = 0; i < call->as.call_.n_args; i++)
            args[i + 1] = call->as.call_.args[i];
        Expr *nc = expr_new(b->a, EX_CALL, call->type, call->span);
        nc->as.call_.fn_binding = (Binding *)pe->target;
        nc->as.call_.fn_expr = NULL;
        nc->as.call_.args = args;
        nc->as.call_.n_args = n;
        nc->as.call_.dict_arg = NULL;
        return nc;
    }
    if (pe->is_alias) {
        /* A renamed fn param: the same call, made through the param.  Copy the
         * node whole -- the alias shares the param's representation (is_poly_fn,
         * fat), so the poly-call flags the elaborator set on it still hold --
         * and name the param the way a direct `(g x)` does, by fn_binding. */
        Expr *nc = expr_new(b->a, EX_CALL, call->type, call->span);
        nc->as.call_ = call->as.call_;
        nc->as.call_.fn_binding = (Binding *)pe->target;
        nc->as.call_.fn_expr = NULL;
        return nc;
    }
    uint32_t n = pe->n_caps + call->as.call_.n_args;
    Expr **args = arena_alloc(b->a, (n ? n : 1) * sizeof(Expr *));
    for (uint32_t i = 0; i < pe->n_caps; i++) {
        Expr *v = expr_new(b->a, EX_VAR, pe->caps[i]->type, call->span);
        v->as.var.binding = pe->caps[i];
        args[i] = v;
    }
    for (uint32_t i = 0; i < call->as.call_.n_args; i++)
        args[pe->n_caps + i] = call->as.call_.args[i];
    Expr *nc = expr_new(b->a, EX_CALL, call->type, call->span);
    nc->as.call_.fn_binding = (Binding *)pe->target;
    nc->as.call_.fn_expr = NULL;
    nc->as.call_.args = args;
    nc->as.call_.n_args = n;
    nc->as.call_.dict_arg = NULL;
    return nc;
}

/* If `e` is a saturated call to a pap-registered var, rewrite it to the
 * underlying saturated call; otherwise return `e` unchanged. */
/* A recursive call of the lambda being translated, made through the `letrec`
 * member that holds it: the direct emitter threads the CURRENT env into it
 * (emit_fns.c, tco_is_self_call), so it is a call of the lambda itself with
 * its own env parameter -- which, translated, threads the continuation.  Left
 * a call through the member, it would be delegated to the lambda's direct
 * entry, a fresh root. */
static bool cps_self_env_call(const CpsB *b, const Binding *callee) {
    return b && callee && b->cur_fn && b->cur_fd && b->cur_fd->closure
        && b->cur_fd->n_params >= 1 && !callee->is_global
        && callee->closure_fn_binding == b->cur_fn;
}

/* The `letrec` cps_tail / cps_bind lower like a `let` of a local closure. */
static bool letrec_local_closure(CpsB *b, const Expr *e) {
    if (!e || e->kind != EX_LETREC) return false;
    const Binding *lc = cps_ir_let_local_closure(e, 0);
    return lc && callee_colored(b, lc);
}

static Expr *pap_maybe_rewrite(CpsB *b, Expr *e) {
    if (!e || e->kind != EX_CALL) return e;
    const Binding *callee = e->as.call_.fn_binding;
    if (!callee && e->as.call_.fn_expr) {
        const Expr *fe = ascribe_peel(e->as.call_.fn_expr);
        if (fe && fe->kind == EX_VAR) callee = fe->as.var.binding;
    }
    if (cps_self_env_call(b, callee) && e->as.call_.n_args + 1 == b->cur_fd->n_params) {
        uint32_t n = e->as.call_.n_args + 1;
        Expr **args = arena_alloc(b->a, n * sizeof(Expr *));
        const Binding *envp = b->cur_fd->params[0];
        Expr *v = expr_new(b->a, EX_VAR, envp->type, e->span);
        v->as.var.binding = (Binding *)envp;
        args[0] = v;
        for (uint32_t i = 0; i < e->as.call_.n_args; i++)
            args[i + 1] = e->as.call_.args[i];
        Expr *nc = expr_new(b->a, EX_CALL, e->type, e->span);
        nc->as.call_.fn_binding = (Binding *)b->cur_fn;
        nc->as.call_.fn_expr = NULL;
        nc->as.call_.args = args;
        nc->as.call_.n_args = n;
        nc->as.call_.dict_arg = NULL;
        return nc;
    }
    const Binding *lt = cps_ir_letrec_member_target(callee);
    if (lt && e->as.call_.n_args == lt->type.as.fn.arity) {
        Expr *nc = expr_new(b->a, EX_CALL, e->type, e->span);
        nc->as.call_ = e->as.call_;
        nc->as.call_.fn_binding = (Binding *)lt;
        nc->as.call_.fn_expr = NULL;
        return nc;
    }
    const PapInline *pe = pap_lookup(b, callee);
    if (!pe || e->as.call_.n_args != pe->rem_arity) return e;
    return pap_build_saturated_call(b, pe, e);
}

/* Register any pap-inlinable let bindings of `let` (a closure whose sole use in
 * the body is a saturated call).  Returns the count pushed (pop with the saved
 * b->n_pap after the let is fully translated). */
static const Type *fn_alias_sig(const Binding *b);   /* fwd */

/* Returns false when a binding whose calls MUST be rewritten (an alias of a
 * global, a local closure) found the table full: the caller evicts rather
 * than delegate a call the classifier counted as rewritten. */
static bool pap_register_let(CpsB *b, const Expr *let) {
    for (uint32_t i = 0; i < let->as.let_.n; i++) {
        const Binding *vb = let->as.let_.bindings[i].binding;
        const Binding *tgt; Binding **caps; uint32_t nc, rem; const Expr *pl;
        if (b->n_pap >= 32) {
            if (cps_ir_let_global_fn_alias(let, i) || cps_ir_let_local_closure(let, i))
                return false;
            continue;
        }
        if (vb
            && pap_extract(let->as.let_.bindings[i].init, &tgt, &caps, &nc, &rem, &pl)
            && !closure_binding_escapes(let->as.let_.body, vb)
            && pap_calls_saturated(let->as.let_.body, vb, rem)) {
            b->pap[b->n_pap].var = vb;      b->pap[b->n_pap].target = tgt;
            b->pap[b->n_pap].caps = caps;   b->pap[b->n_pap].n_caps = nc;
            b->pap[b->n_pap].rem_arity = rem;
            b->pap[b->n_pap].is_alias = false;
            b->pap[b->n_pap].is_env_call = false;
            b->n_pap++;
            continue;
        }
        /* An alias of a fn param is the zero-capture case: a call through it IS
         * a call through the param.  Without this the binding crossed a slot as
         * a fat value, and the classifier counted it as the param escaping, so
         * an effectful callback called through the alias had no lowering
         * (cps-let-alias-of-effectful-fn-param-refused). */
        const Binding *src = cps_ir_let_fnparam_alias(let, i);
        if (!src) src = cps_ir_let_global_fn_alias(let, i);
        if (src) {
            b->pap[b->n_pap].var = vb;      b->pap[b->n_pap].target = src;
            b->pap[b->n_pap].caps = NULL;   b->pap[b->n_pap].n_caps = 0;
            b->pap[b->n_pap].rem_arity = fn_alias_sig(src)->as.fn.arity;
            b->pap[b->n_pap].is_alias = true;
            b->pap[b->n_pap].is_env_call = false;
            b->n_pap++;
            continue;
        }
        /* A local capturing closure of a colored lambda: each call becomes the
         * lambda's own CPS call, threading the continuation.  An uncolored one
         * keeps the closure protocol (the direct emitter's call). */
        const Binding *lc = cps_ir_let_local_closure(let, i);
        if (lc && callee_colored(b, lc)) {
            b->pap[b->n_pap].var = vb;      b->pap[b->n_pap].target = lc;
            b->pap[b->n_pap].caps = NULL;   b->pap[b->n_pap].n_caps = 0;
            b->pap[b->n_pap].rem_arity = vb->type.as.fn.arity;
            b->pap[b->n_pap].is_alias = false;
            b->pap[b->n_pap].is_env_call = true;
            b->n_pap++;
        }
    }
    return true;
}

/* The call signature of a fn-valued binding: its own TY_FN type, or -- for a fat
 * (is_poly_fn) param, whose binding type is the `tur_poly_fn_t` carrier -- the
 * TY_FN recorded in poly_type.  NULL for a bare `:fn` carrier or a rank-2 forall,
 * whose calls cross a different ABI than a plain call through it. */
static const Type *fn_alias_sig(const Binding *b) {
    if (b->is_poly_fn)
        return b->poly_type && b->poly_type->kind == TY_FN ? b->poly_type : NULL;
    return b->type.kind == TY_FN ? &b->type : NULL;
}

static const Binding *fnparam_alias_src(const Binding *vb, const Expr *init) {
    if (!vb || vb->is_mut) return NULL;
    while (init && init->kind == EX_ASCRIBE) init = init->as.ascribe_.inner;
    if (!init || init->kind != EX_VAR) return NULL;
    const Binding *src = init->as.var.binding;
    if (!src || !src->is_param || src->is_global || src->is_mut) return NULL;
    /* Same representation: the elaborator copies is_poly_fn (and the carrier
     * type) onto the alias. */
    if (vb->is_poly_fn != src->is_poly_fn || vb->type.kind != src->type.kind)
        return NULL;
    const Type *ss = fn_alias_sig(src), *vs = fn_alias_sig(vb);
    if (!ss || !vs || ss->as.fn.arity != vs->as.fn.arity) return NULL;
    return src;
}

/* cps-local-fn-alias-or-lambda-called-in-place-refused: a GLOBAL function held
 * by an immutable local -- a named fn (`(let [g via-adt] ...)`) or the lifted
 * `__fn_N` of a captureless lambda (`(let [g (fn [n] ...)] ...)`) -- whose
 * every use is a saturated call.  A call through it IS a direct call to the
 * global.  A generic target is left out (a call through the local carries no
 * instantiation), and so is one whose result is a function: a lifted lambda
 * that returns a closure is callable only through the closure protocol on the
 * local (pr-386, Binding.is_lifted_lambda). */
static bool fn_alias_kind_concrete(TypeKind k) {
    return k != TY_TYVAR && k != TY_APP && k != TY_FORALL && k != TY_UNKNOWN;
}
static bool global_fn_target_ok(const Binding *src) {
    if (!src || !src->is_global || src->is_mut || src->is_poly_fn || src->is_fat
        || src->closure_fn_binding || src->type.kind != TY_FN)
        return false;
    if (src->type.as.fn.result_kind == TY_FN) return false;
    const FnDef *fd = src->source_fn_def;
    if (!fd || fd->closure || fd->constraints.n_constraints > 0) return false;
    for (uint32_t k = 0; k < fd->n_params; k++) {
        const Binding *pk = fd->params ? fd->params[k] : NULL;
        if (!pk || !fn_alias_kind_concrete(pk->type.kind)) return false;
    }
    return fn_alias_kind_concrete(src->type.as.fn.result_kind);
}
static const Binding *global_fn_alias_src(const Binding *vb, const Expr *init) {
    if (!vb || vb->is_mut || vb->is_poly_fn || vb->is_fat || vb->type.kind != TY_FN)
        return NULL;
    while (init && init->kind == EX_ASCRIBE) init = init->as.ascribe_.inner;
    if (!init || init->kind != EX_VAR) return NULL;
    const Binding *src = init->as.var.binding;
    if (!global_fn_target_ok(src)) return NULL;
    if (src->type.as.fn.arity != vb->type.as.fn.arity) return NULL;
    return src;
}

/* A captureless `letrec` member: elab_letrec lifts its lambda to a global
 * `__fn_N`, marks the member itself global (its calls are direct C calls) and
 * names the lambda in source_binding.  A call through the member -- from the
 * `letrec` body or the lambda's own self-call -- is a direct call to the
 * lambda.  NULL for anything else. */
const Binding *cps_ir_letrec_member_target(const Binding *f) {
    if (!f || !f->is_letrec_binding || !f->is_global || f->is_mut || f->is_poly_fn)
        return NULL;
    const Binding *src = f->source_binding;
    if (!global_fn_target_ok(src) || !src->is_lifted_lambda) return NULL;
    if (f->type.kind != TY_FN || f->type.as.fn.arity != src->type.as.fn.arity) return NULL;
    return src;
}

/* Is every member of this `letrec` such a global (so the group binds names,
 * not values, and its body is all there is to translate)? */
static bool letrec_all_global_members(const Expr *e) {
    if (!e || e->kind != EX_LETREC || e->as.let_.n == 0) return false;
    for (uint32_t i = 0; i < e->as.let_.n; i++) {
        const Binding *f = e->as.let_.bindings[i].binding;
        const Expr *init = ascribe_peel(e->as.let_.bindings[i].init);
        const Binding *src = cps_ir_letrec_member_target(f);
        if (!src || !init || init->kind != EX_VAR || init->as.var.binding != src)
            return false;
    }
    return true;
}

const Binding *cps_ir_let_global_fn_alias(const Expr *let, uint32_t i) {
    if (!let || (let->kind != EX_LET) || i >= let->as.let_.n) return NULL;
    const Binding *vb = let->as.let_.bindings[i].binding;
    const Binding *src = global_fn_alias_src(vb, let->as.let_.bindings[i].init);
    if (!src) return NULL;
    uint32_t ar = src->type.as.fn.arity;
    for (uint32_t j = i + 1; j < let->as.let_.n; j++)
        if (!pap_calls_saturated(let->as.let_.bindings[j].init, vb, ar)) return NULL;
    if (closure_binding_escapes(let->as.let_.body, vb)) return NULL;
    if (!pap_calls_saturated(let->as.let_.body, vb, ar)) return NULL;
    return src;
}

/* cps-local-fn-alias-or-lambda-called-in-place-refused, the capturing shape:
 * an immutable local bound to a fresh CAPTURING lambda, whose every use is a
 * saturated call.  The closure's value is its env box, and the lifted lambda
 * takes that box as its first parameter, so a call through the local is a
 * call to the lambda -- the direct emitter spells it `__fn_N((void *)g, x)`.
 * For a colored lambda the translator makes it the lambda's CPS call
 * (pap_register_let, is_env_call), threading the continuation; the binding
 * stays, since the box is still built.  The lambda is then no fn value: no
 * fresh root ever calls it, which is what lets the classifier admit it.  A
 * generic lambda and one whose result is a function are left out, as for
 * the captureless alias above. */
/* A lambda's parameter or result type, free of type variables: a type
 * application only with a concrete head and arguments (`(Option int)`), since
 * a call through the local carries no instantiation. */
static bool local_closure_type_ok(const Type *t) {
    if (!t) return false;
    if (t->kind == TY_APP)
        return local_closure_type_ok(t->as.app.fn) && local_closure_type_ok(t->as.app.arg);
    return fn_alias_kind_concrete(t->kind);
}

static const Binding *local_closure_shape(const Expr *let, uint32_t i) {
    if (!let || i >= let->as.let_.n) return NULL;
    /* A `letrec` of one capturing member is the same shape: its lambda reaches
     * itself by a self-call, which threads its own env (cps_self_env_call),
     * never by a capture of the member. */
    bool rec = let->kind == EX_LETREC;
    if (!(let->kind == EX_LET || (rec && let->as.let_.n == 1))) return NULL;
    const Binding *vb = let->as.let_.bindings[i].binding;
    if (!vb || vb->is_global || vb->is_param || vb->is_mut || vb->is_poly_fn
        || vb->type.kind != TY_FN)
        return NULL;
    const Expr *init = ascribe_peel(let->as.let_.bindings[i].init);
    if (!init || init->kind != EX_CLOSURE || !init->as.closure_.closure) return NULL;
    const struct Closure *c = init->as.closure_.closure;
    if (c->n_captures == 0 || c->is_shift_receiver || c->is_effect_payload) return NULL;
    for (uint32_t k = 0; k < c->n_captures; k++)
        if (c->captures[k] == vb) return NULL;
    const FnDef *fd = c->fn;
    const Binding *lam = fd ? fd->binding : NULL;
    if (!lam || lam != vb->closure_fn_binding || !lam->is_lifted_lambda
        || lam->type.kind != TY_FN)
        return NULL;
    uint32_t ar = vb->type.as.fn.arity;
    if (fd->n_params != ar + 1 || lam->type.as.fn.arity != ar + 1) return NULL;
    if (fd->constraints.n_constraints > 0) return NULL;
    for (uint32_t k = 1; k < fd->n_params; k++)
        if (!fd->params[k] || fd->params[k]->is_poly_fn
            || !local_closure_type_ok(&fd->params[k]->type))
            return NULL;
    TypeKind rk = lam->type.as.fn.result_kind;
    if (rk == TY_FN) return NULL;
    /* The binding's full result type: the FnDef's return_type of a lambda
     * can still read a body-typed `(Option ?)` where the annotation says
     * `(Option int)`. */
    const Type *rft = lam->type.as.fn.result_full_type;
    if (rk == TY_APP ? !rft || !local_closure_type_ok(rft) : !fn_alias_kind_concrete(rk))
        return NULL;
    /* Every use a saturated call: pap_calls_saturated reads any other use --
     * an argument, a capture by a nested fn, a stored value -- as a refusal,
     * so the box never leaves the let. */
    for (uint32_t j = i + 1; j < let->as.let_.n; j++)
        if (!pap_calls_saturated(let->as.let_.bindings[j].init, vb, ar)) return NULL;
    if (!pap_calls_saturated(let->as.let_.body, vb, ar)) return NULL;
    if (rec && !pap_calls_saturated(fd->body, vb, ar)) return NULL;
    /* The box is reaped at the DK entry boundary (cps_bind_let_init), so its
     * release must free nothing the lambda's result can still point into: a
     * scalar result points into nothing, and otherwise the env's drop glue
     * (emit_expr.c) must free the box alone -- no rc, owned fat closure or
     * Drop-instance capture -- and no inline C block may name a capture (an
     * lvalue into the box). */
    switch (rk) {
        case TY_INT: case TY_FLOAT: case TY_FLOAT32: case TY_FLOAT64:
        case TY_BOOL: case TY_NIL:
            break;
        default:
            for (uint32_t k = 0; k < c->n_captures; k++) {
                const Binding *cap = c->captures[k];
                if (!cap) return NULL;
                if (cap->is_global) continue;
                if (cap->type.kind == TY_RC || cap->is_fat) return NULL;
                if (c->capture_drop_insts && c->capture_drop_insts[k]) return NULL;
            }
            if (closure_body_inline_c_touches_env(c)) return NULL;
    }
    return lam;
}

/* The rewrite is for a lambda whose effects may leave it: a pure one gains
 * nothing from a CPS entry, and it keeps the direct path it always had (its
 * calls stay closure calls; a `letrec` of it keeps the direct lowering). */
static bool local_lambda_effectful(const FnDef *fd) {
    if (fd->binding && fd->binding->type.kind == TY_FN
        && !effect_row_is_runtime_pure(fd->binding->type.as.fn.effect_row))
        return true;
    return fd->inferred_effect_row
        && !effect_row_is_runtime_pure(fd->inferred_effect_row);
}

const Binding *cps_ir_let_local_closure(const Expr *let, uint32_t i) {
    const Binding *lam = local_closure_shape(let, i);
    if (!lam) return NULL;
    const Expr *init = ascribe_peel(let->as.let_.bindings[i].init);
    return local_lambda_effectful(init->as.closure_.closure->fn) ? lam : NULL;
}

const Binding *cps_ir_let_fnparam_alias(const Expr *let, uint32_t i) {
    if (!let || (let->kind != EX_LET) || i >= let->as.let_.n) return NULL;
    const Binding *vb = let->as.let_.bindings[i].binding;
    const Binding *src = fnparam_alias_src(vb, let->as.let_.bindings[i].init);
    if (!src) return NULL;
    /* Every use -- in the body, or in a later binding's init, which is
     * translated while the registration is live -- a saturated call through it:
     * the alias is never a value, so dropping its binding loses nothing. */
    uint32_t ar = fn_alias_sig(src)->as.fn.arity;
    for (uint32_t j = i + 1; j < let->as.let_.n; j++)
        if (!pap_calls_saturated(let->as.let_.bindings[j].init, vb, ar)) return NULL;
    if (closure_binding_escapes(let->as.let_.body, vb)) return NULL;
    if (!pap_calls_saturated(let->as.let_.body, vb, ar)) return NULL;
    return src;
}

/* Was let-binding `idx` of `let` pap-inlined (registered in [saved, n_pap))?  If
 * so, its closure is dropped; only its prelude capture bindings need emitting. */
static bool pap_binding_inlined(CpsB *b, const Expr *let, uint32_t idx, uint32_t saved) {
    const Binding *vb = let->as.let_.bindings[idx].binding;
    for (uint32_t j = saved; j < b->n_pap; j++)
        if (b->pap[j].var == vb) return !b->pap[j].is_env_call;
    return false;
}

/* ==== cps-while-native: EX_WHILE -> synthesized recursive __cps loop ==== *
 *
 * A `while` whose body contains an interior control op (handle/perform/resume)
 * cannot delegate to the direct emitter under the flag (that keeps a non-DK
 * effect lowering) and has no same-function join lowering (emit_handle lifts the
 * handle continuation into a SEPARATE C function, so the loop back-edge -- which
 * depends on the handle result -- lives in that continuation, out of reach of a
 * `goto`).  So the loop is lowered to a synthesized tail-recursive colored `__cps`
 * helper: the ^mut loop-carried vars become the helper params, `set!` writes a
 * pre-created `$next` CVar, and the back-edge (CT_CONTINUE) re-enters the helper.
 *
 * Soundness rests on a conservative guard (loop_body_ok): every in-body read of a
 * loop-carried var must resolve to the loop-ENTRY version (no read observes a
 * `set!` done earlier in the same iteration).  Then reads resolve to the params by
 * naming (name_for_binding) with no rebinding map, and the right-to-left build
 * order is irrelevant because the `$next` CVars are pre-created (stable identity),
 * not read from a mutable table.  Anything outside the guard EVICTS (returns NULL
 * -> unsupported_form), preserving today's correct fiber fallback. */

/* Collect every EX_LET binder Binding appearing anywhere in `e` (so a set! target
 * that is a loop-body-local ^mut is excluded from the loop-carried set). */
static void loop_collect_let_binders(const Expr *e, const Binding **out,
                                     uint32_t *n, uint32_t cap) {
    e = ascribe_peel(e);
    if (!e) return;
    switch (e->kind) {
        case EX_LET: case EX_LETREC:
            for (uint32_t i = 0; i < e->as.let_.n; i++) {
                const Binding *bd = e->as.let_.bindings[i].binding;
                if (bd && *n < cap) out[(*n)++] = bd;
                loop_collect_let_binders(e->as.let_.bindings[i].init, out, n, cap);
            }
            loop_collect_let_binders(e->as.let_.body, out, n, cap);
            break;
        case EX_DO:
            for (uint32_t i = 0; i < e->as.do_.n; i++)
                loop_collect_let_binders(e->as.do_.items[i], out, n, cap);
            break;
        case EX_IF:
            loop_collect_let_binders(e->as.if_.cond, out, n, cap);
            loop_collect_let_binders(e->as.if_.then_, out, n, cap);
            loop_collect_let_binders(e->as.if_.else_or_null, out, n, cap);
            break;
        case EX_SET:  loop_collect_let_binders(e->as.set_.value, out, n, cap); break;
        case EX_HANDLE: {
            HandleExpr *h = e->as.handle_.handle;
            if (h) {
                loop_collect_let_binders(h->body, out, n, cap);
                for (uint8_t i = 0; i < h->n_cases; i++)
                    loop_collect_let_binders(h->cases[i].body, out, n, cap);
            }
            break;
        }
        case EX_PERFORM: {
            PerformExpr *pf = e->as.perform_.perform;
            for (uint8_t i = 0; pf && i < pf->n_args; i++)
                loop_collect_let_binders(pf->args[i], out, n, cap);
            break;
        }
        case EX_BUILTIN:
            for (uint32_t i = 0; i < e->as.builtin.n; i++)
                loop_collect_let_binders(e->as.builtin.args[i], out, n, cap);
            break;
        case EX_CALL:
            for (uint32_t i = 0; i < e->as.call_.n_args; i++)
                loop_collect_let_binders(e->as.call_.args[i], out, n, cap);
            break;
        default: break;
    }
}

/* Collect the ^mut set! targets in `e` that are NOT loop-body-locals -> the
 * loop-carried variable set.  Returns false if a target is not ^mut, a target is
 * a loop-body-local (a per-iteration ^mut we must not thread), or capacity is
 * exceeded (all conservative EVICT conditions). */
static bool loop_collect_carried(const Expr *e, const Binding **locals, uint32_t n_locals,
                                  const Binding **out, uint32_t *n, uint32_t cap) {
    e = ascribe_peel(e);
    if (!e) return true;
    switch (e->kind) {
        case EX_SET: {
            const Binding *tg = e->as.set_.target;
            if (!tg || !tg->is_mut) {
                if (getenv("TUR_TRACE_CORE"))
                    fprintf(stderr, "[LOOP-CARRIED] set! target %s is not ^mut (is_mut=%d)\n",
                            tg && tg->name ? tg->name->name : "?", tg ? (int)tg->is_mut : -1);
                return false;
            }
            for (uint32_t i = 0; i < n_locals; i++)
                if (locals[i] == tg) {
                    if (getenv("TUR_TRACE_CORE"))
                        fprintf(stderr, "[LOOP-CARRIED] set! target %s is a loop-body local\n",
                                tg->name ? tg->name->name : "?");
                    return false;   /* loop-body-local ^mut */
                }
            bool seen = false;
            for (uint32_t i = 0; i < *n; i++) if (out[i] == tg) seen = true;
            if (!seen) { if (*n >= cap) return false; out[(*n)++] = tg; }
            return loop_collect_carried(e->as.set_.value, locals, n_locals, out, n, cap);
        }
        case EX_LET: case EX_LETREC:
            for (uint32_t i = 0; i < e->as.let_.n; i++)
                if (!loop_collect_carried(e->as.let_.bindings[i].init, locals, n_locals, out, n, cap))
                    return false;
            return loop_collect_carried(e->as.let_.body, locals, n_locals, out, n, cap);
        case EX_DO:
            for (uint32_t i = 0; i < e->as.do_.n; i++)
                if (!loop_collect_carried(e->as.do_.items[i], locals, n_locals, out, n, cap))
                    return false;
            return true;
        case EX_IF:
            return loop_collect_carried(e->as.if_.cond, locals, n_locals, out, n, cap)
                && loop_collect_carried(e->as.if_.then_, locals, n_locals, out, n, cap)
                && (!e->as.if_.else_or_null
                    || loop_collect_carried(e->as.if_.else_or_null, locals, n_locals, out, n, cap));
        case EX_HANDLE: {
            HandleExpr *h = e->as.handle_.handle;
            if (!h) return true;
            if (!loop_collect_carried(h->body, locals, n_locals, out, n, cap)) return false;
            for (uint8_t i = 0; i < h->n_cases; i++)
                if (!loop_collect_carried(h->cases[i].body, locals, n_locals, out, n, cap))
                    return false;
            return true;
        }
        case EX_PERFORM: {
            PerformExpr *pf = e->as.perform_.perform;
            for (uint8_t i = 0; pf && i < pf->n_args; i++)
                if (!loop_collect_carried(pf->args[i], locals, n_locals, out, n, cap)) return false;
            return true;
        }
        case EX_RESUME: {
            ResumeExpr *rs = e->as.resume_.resume;
            return !rs
                || (loop_collect_carried(rs->k, locals, n_locals, out, n, cap)
                    && loop_collect_carried(rs->value, locals, n_locals, out, n, cap));
        }
        case EX_BUILTIN:
            for (uint32_t i = 0; i < e->as.builtin.n; i++)
                if (!loop_collect_carried(e->as.builtin.args[i], locals, n_locals, out, n, cap))
                    return false;
            return true;
        case EX_CALL:
            for (uint32_t i = 0; i < e->as.call_.n_args; i++)
                if (!loop_collect_carried(e->as.call_.args[i], locals, n_locals, out, n, cap))
                    return false;
            return true;
        default:
            /* A form we don't structurally sequence must contain no set! of a
             * carried var to be sound -- if it does, we'd miss it.  Conservatively
             * accept only when it references no set!-shaped subform: use the
             * has-control/refs walkers are overkill; simplest safe rule is that an
             * unrecognized form with any nested EX_SET is caught here by returning
             * true only if it has none. */
            return true;
    }
}

/* Execution-order read-after-set guard.  `mask` tracks which loop-carried vars
 * (by index in vars[]) have been set so far this iteration.  Returns false if any
 * expression READS a loop-carried var that was already set (its source semantics
 * would observe the updated value, but the lowering resolves reads to the entry
 * param -- a miscompile).  Also fails on a set! inside an EX_IF/EX_MATCH arm
 * (conditional set -> the single-back-edge $next binder would be unbound on the
 * other path) and on any control/looping form we do not model. */
static int loop_idx(const Binding **vars, uint32_t n, const Binding *bd) {
    for (uint32_t i = 0; i < n; i++) if (vars[i] == bd) return (int)i;
    return -1;
}
static bool loop_guard(const Expr *e, const Binding **vars, uint32_t nv,
                       uint32_t *mask, bool in_branch) {
    e = ascribe_peel(e);
    if (!e) return true;
    switch (e->kind) {
        case EX_VAR: {
            int idx = loop_idx(vars, nv, e->as.var.binding);
            /* A read of an already-set carried var on the STRAIGHT-LINE path is now
             * admitted -- loop_rs_scan records it and atomize resolves it to the
             * var's `$next` CVar.  A read inside a BRANCH / handle case (in_branch)
             * still evicts: those bodies are lifted into separate frames where the
             * loop helper's `$next` local is out of scope. */
            if (idx >= 0 && (*mask & (1u << idx)) && in_branch) return false;
            return true;
        }
        case EX_INT_LIT: case EX_BOOL_LIT: case EX_NIL_LIT:
        case EX_FLOAT_LIT: case EX_CSTR_LIT:
            return true;
        case EX_SET: {
            if (!loop_guard(e->as.set_.value, vars, nv, mask, in_branch)) return false;
            int idx = loop_idx(vars, nv, e->as.set_.target);
            if (idx >= 0) {
                if (in_branch) return false;         /* conditional set */
                if (*mask & (1u << idx)) return false; /* set twice */
                *mask |= (1u << idx);
            }
            return true;
        }
        case EX_LET: case EX_LETREC:
            for (uint32_t i = 0; i < e->as.let_.n; i++)
                if (!loop_guard(e->as.let_.bindings[i].init, vars, nv, mask, in_branch))
                    return false;
            return loop_guard(e->as.let_.body, vars, nv, mask, in_branch);
        case EX_DO:
            for (uint32_t i = 0; i < e->as.do_.n; i++)
                if (!loop_guard(e->as.do_.items[i], vars, nv, mask, in_branch)) return false;
            return true;
        case EX_BUILTIN:
            for (uint32_t i = 0; i < e->as.builtin.n; i++)
                if (!loop_guard(e->as.builtin.args[i], vars, nv, mask, in_branch)) return false;
            return true;
        case EX_CALL:
            if (e->as.call_.fn_expr
                && !loop_guard(e->as.call_.fn_expr, vars, nv, mask, in_branch)) return false;
            for (uint32_t i = 0; i < e->as.call_.n_args; i++)
                if (!loop_guard(e->as.call_.args[i], vars, nv, mask, in_branch)) return false;
            return true;
        case EX_HANDLE: {
            HandleExpr *h = e->as.handle_.handle;
            if (!h) return false;
            /* The delimited body executes, then a case may run to produce the value.
             * Neither may set! a carried var (a set! reached through a resume is not
             * on the straight-line single-back-edge path). */
            uint32_t save = *mask;
            if (!loop_guard(h->body, vars, nv, mask, in_branch)) return false;
            if (*mask != save) return false;   /* body set! a carried var: unsupported */
            for (uint8_t i = 0; i < h->n_cases; i++) {
                uint32_t cm = *mask;
                if (!loop_guard(h->cases[i].body, vars, nv, &cm, true)) return false;
                if (cm != *mask) return false; /* case set! a carried var */
            }
            return true;
        }
        case EX_PERFORM: {
            PerformExpr *pf = e->as.perform_.perform;
            for (uint8_t i = 0; pf && i < pf->n_args; i++)
                if (!loop_guard(pf->args[i], vars, nv, mask, in_branch)) return false;
            return true;
        }
        case EX_RESUME: {
            ResumeExpr *rs = e->as.resume_.resume;
            return rs
                && loop_guard(rs->k, vars, nv, mask, in_branch)
                && loop_guard(rs->value, vars, nv, mask, in_branch);
        }
        case EX_IF: {
            if (!loop_guard(e->as.if_.cond, vars, nv, mask, in_branch)) return false;
            uint32_t tm = *mask, em = *mask;
            if (!loop_guard(e->as.if_.then_, vars, nv, &tm, true)) return false;
            if (e->as.if_.else_or_null
                && !loop_guard(e->as.if_.else_or_null, vars, nv, &em, true)) return false;
            if (tm != *mask || em != *mask) return false;  /* set! inside a branch */
            return true;
        }
        default:
            /* An unmodeled form: safe only if it neither reads a set var nor sets a
             * carried var.  Reject if it references any carried var at all (we can't
             * order its reads) once any set has happened; else accept. */
            if (*mask) {
                for (uint32_t i = 0; i < nv; i++)
                    if ((*mask & (1u << i)) && expr_refs_binding(e, vars[i]->id)) return false;
            }
            return true;
    }
}

/* cps-while-native read-after-set: a forward pre-pass mirroring loop_guard's walk.
 * Records, by Expr-node identity, each STRAIGHT-LINE carried-var READ that occurs
 * after that var's `set!` this iteration; atomize() resolves such a read to the
 * var's `$next` CVar instead of the entry param.  `mask` is the set-so-far bits;
 * branch/handle-case bodies scan with a mask copy and in_branch=true (their reads
 * are not recorded -- loop_guard evicts a read-after-set there). */
static void loop_rs_scan(CpsB *b, const Expr *e, uint32_t *mask, bool in_branch) {
    e = ascribe_peel(e);
    if (!e) return;
    switch (e->kind) {
        case EX_VAR: {
            int idx = loop_var_index(b, e->as.var.binding);
            if (idx >= 0 && (*mask & (1u << idx)) && !in_branch && b->rs_n < 64) {
                b->rs_nodes[b->rs_n] = e;
                b->rs_cvars[b->rs_n] = b->loop_nexts[idx];
                b->rs_n++;
            }
            return;
        }
        case EX_INT_LIT: case EX_BOOL_LIT: case EX_NIL_LIT:
        case EX_FLOAT_LIT: case EX_CSTR_LIT:
            return;
        case EX_SET: {
            loop_rs_scan(b, e->as.set_.value, mask, in_branch);
            int idx = loop_var_index(b, e->as.set_.target);
            if (idx >= 0) *mask |= (1u << idx);
            return;
        }
        case EX_LET: case EX_LETREC:
            for (uint32_t i = 0; i < e->as.let_.n; i++)
                loop_rs_scan(b, e->as.let_.bindings[i].init, mask, in_branch);
            loop_rs_scan(b, e->as.let_.body, mask, in_branch);
            return;
        case EX_DO:
            for (uint32_t i = 0; i < e->as.do_.n; i++)
                loop_rs_scan(b, e->as.do_.items[i], mask, in_branch);
            return;
        case EX_BUILTIN:
            for (uint32_t i = 0; i < e->as.builtin.n; i++)
                loop_rs_scan(b, e->as.builtin.args[i], mask, in_branch);
            return;
        case EX_CALL:
            if (e->as.call_.fn_expr) loop_rs_scan(b, e->as.call_.fn_expr, mask, in_branch);
            for (uint32_t i = 0; i < e->as.call_.n_args; i++)
                loop_rs_scan(b, e->as.call_.args[i], mask, in_branch);
            return;
        case EX_HANDLE: {
            HandleExpr *h = e->as.handle_.handle;
            if (!h) return;
            loop_rs_scan(b, h->body, mask, in_branch);
            for (uint8_t i = 0; i < h->n_cases; i++) {
                uint32_t cm = *mask;
                loop_rs_scan(b, h->cases[i].body, &cm, true);
            }
            return;
        }
        case EX_PERFORM: {
            PerformExpr *pf = e->as.perform_.perform;
            for (uint8_t i = 0; pf && i < pf->n_args; i++)
                loop_rs_scan(b, pf->args[i], mask, in_branch);
            return;
        }
        case EX_RESUME: {
            ResumeExpr *rs = e->as.resume_.resume;
            if (rs) { loop_rs_scan(b, rs->k, mask, in_branch);
                      loop_rs_scan(b, rs->value, mask, in_branch); }
            return;
        }
        case EX_IF: {
            loop_rs_scan(b, e->as.if_.cond, mask, in_branch);
            uint32_t tm = *mask, em = *mask;
            loop_rs_scan(b, e->as.if_.then_, &tm, true);
            if (e->as.if_.else_or_null) loop_rs_scan(b, e->as.if_.else_or_null, &em, true);
            return;
        }
        default: return;   /* unmodeled forms: loop_guard rejects any that ref a set var */
    }
}

/* Build the CT_LOOP for an EX_WHILE in bind position (its unit result discarded
 * into `x`, the continuation `rest`).  Returns NULL to EVICT. */
/* TUR_TRACE_CORE=1: name the build_loop rule that refused to lower a `while`
 * natively (the eviction then reads BODY-UNSUPPORTED "EX_WHILE"). */
#define LOOP_REJECT() do { \
        if (getenv("TUR_TRACE_CORE")) \
            fprintf(stderr, "[LOOP-REJECT] cps_ir.c:%d\n", (int)__LINE__); \
        return NULL; \
    } while (0)

static CTerm *build_loop(CpsB *b, Expr *e, CVar x, CTerm *rest) {
    (void)x;   /* the while's unit result is subsumed by the loop's exit delivery */
    Expr *cond = e->as.while_.cond;
    Expr *body = e->as.while_.body;
    if (!cond || !body) LOOP_REJECT();

    /* The continuation is a trivial delivery of a single loop-carried var to an
     * enclosing continuation (KK_RET / KK_PROMPT) -- the loop's live-after
     * value -- or, perform-inside-loop-has-no-lowering, a loop with NOTHING
     * live after it (the while is the function's or the prompt's last form, so
     * the continuation delivers the while's own unit result: its CVar `x`, or
     * a unit literal): a unit exit.  Anything richer -- the loop is followed by
     * more statements -- is reified as a JOIN whose body is that continuation
     * (`join_mode` below): the helper delivers unit to the join, which the
     * emitter lifts as an escaping-join resume-frame, and every carried var
     * becomes a shared cell so the code after the loop reads its final value. */
    const Binding *result_bd = NULL;
    bool unit_exit = false;
    bool join_mode = false;
    CKont rk;
    if (rest->kind == CT_APPCONT &&
        (rest->as.appcont.kont.kind == KK_RET || rest->as.appcont.kont.kind == KK_PROMPT) &&
        ((rest->as.appcont.v.kind == CA_VAR && rest->as.appcont.v.var) ||
         rest->as.appcont.v.kind == CA_UNIT ||
         (rest->as.appcont.v.kind == CA_CVAR && rest->as.appcont.v.cvar_id == x.id))) {
        rk = rest->as.appcont.kont;
        if (rest->as.appcont.v.kind == CA_VAR) result_bd = rest->as.appcont.v.var;
        else unit_exit = true;
    } else {
        /* A control-free loop followed by statements keeps its whole-form
         * delegation; the join reification is for loops that perform. */
        if (safe_to_delegate(b, e)) LOOP_REJECT();
        join_mode = true;
        unit_exit = true;
        CVar j = fresh_cvar(b, x.type);
        j.name = arena_strdup(b->a, "j", 1);
        rk = kont_var(j);
    }

    /* Loop-carried vars = the ^mut set! targets (excluding loop-body locals). */
    const Binding *locals[64]; uint32_t n_locals = 0;
    loop_collect_let_binders(body, locals, &n_locals, 64);
    const Binding *vars[16]; uint32_t nv = 0;
    if (!loop_collect_carried(body, locals, n_locals, vars, &nv, 16)) LOOP_REJECT();
    if (nv > 16) LOOP_REJECT();
    if (b->loop_depth) LOOP_REJECT();   /* nested loops: out of subset */

    /* Soundness guard: reads resolve to loop-entry versions; unconditional single
     * set per carried var. */
    uint32_t mask = 0;
    bool strict = loop_guard(body, vars, nv, &mask, false);
    for (uint32_t i = 0; strict && i < nv; i++)
        if (!(mask & (1u << i))) strict = false;   /* every carried var set once */

    /* perform-inside-loop-has-no-lowering (the snake residue): a var the strict
     * guard rejects -- assigned inside an `if`/`match` arm, more than once per
     * iteration, or read in a branch after its set -- has no sound home in the
     * helper's parameters, whose reads resolve to the loop-entry version.  Give
     * it the B7 by-reference CELL instead of evicting the loop: its set!s lower
     * as ordinary delegated `(set! m v)` (a write through the shared cell) and
     * its reads deref the cell, so every frame -- the helper, a lifted perform
     * continuation, an escaping join -- sees one location.  The emitter promotes
     * exactly these (a letraw set! in a loop body whose target is not a loop
     * param: byref_scan's CT_LOOP case).  Vars the guard accepts stay params, so
     * the strict shape's codegen is unchanged. */
    const Binding *cells[16]; uint32_t ncell = 0;
    if (join_mode) {
        /* The code after the loop reads the carried vars by name; a param
         * would leave the outer local at its entry value.  Cells, all of them. */
        for (uint32_t i = 0; i < nv; i++) cells[ncell++] = vars[i];
        nv = 0;
        strict = true;
    }
    if (!strict) {
        const Binding *keep[16]; uint32_t nk = 0;
        for (uint32_t i = 0; i < nv; i++) {
            uint32_t m = 0;
            const Binding *one = vars[i];
            if (loop_guard(body, &one, 1, &m, false) && (m & 1u)) keep[nk++] = vars[i];
            else cells[ncell++] = vars[i];
        }
        for (uint32_t i = 0; i < nk; i++) vars[i] = keep[i];
        nv = nk;
        /* The kept set must still pass jointly (a kept var read in a branch
         * after another kept var's set, ...). */
        mask = 0;
        if (!loop_guard(body, vars, nv, &mask, false)) LOOP_REJECT();
        for (uint32_t i = 0; i < nv; i++)
            if (!(mask & (1u << i))) LOOP_REJECT();
    }

    /* The delivered result var must itself be loop-carried (a param, or a cell
     * whose value the exit reads through the cell); a non-carried result is not
     * this shape. */
    if (!unit_exit && loop_idx(vars, nv, result_bd) < 0 &&
        loop_idx(cells, ncell, result_bd) < 0) LOOP_REJECT();

    /* Params carry their source Binding so in-body reads name the param via
     * name_for_binding; inits are the entry values (the vars as currently bound). */
    CVar *params = arena_alloc(b->a, (nv ? nv : 1) * sizeof(CVar));
    CAtom *inits = arena_alloc(b->a, (nv ? nv : 1) * sizeof(CAtom));
    for (uint32_t i = 0; i < nv; i++) {
        params[i] = cvar_of_binding(vars[i]);
        CAtom a; memset(&a, 0, sizeof a);
        a.kind = CA_VAR; a.ty = vars[i]->type.kind; a.type = &vars[i]->type; a.var = vars[i];
        inits[i] = a;
    }

    /* Pre-create the $next CVars (stable identity -> build-order independent). */
    uint32_t saved_n_loop = b->n_loop;
    for (uint32_t i = 0; i < nv; i++) {
        b->loop_vars[i]  = vars[i];
        b->loop_nexts[i] = fresh_cvar(b, &vars[i]->type);
    }
    b->n_loop = nv;
    b->loop_depth++;

    /* Read-after-set pre-pass: record straight-line carried-var reads that follow
     * that var's set! so atomize resolves them to `$next` (see loop_rs_scan). */
    uint32_t saved_rs_n = b->rs_n;
    { uint32_t rsmask = 0; loop_rs_scan(b, body, &rsmask, false); }

    /* Lower the body with the loop-continue kont; its tail becomes CT_CONTINUE. */
    CKont lk; lk.kind = KK_LOOP; lk.id = 0; lk.ty = TY_NIL;
    CTerm *iter = cps_tail(b, body, lk);

    b->rs_n = saved_rs_n;       /* drop this loop's read-set entries */
    b->n_loop = saved_n_loop;   /* restore (no nesting) */
    b->loop_depth--;

    /* Exit arm: deliver the live-after var (as its param) -- or unit, when
     * nothing is live after the loop -- to the helper's KK_RET. */
    CKont hret = kont_ret(unit_exit ? TY_NIL : result_bd->type.kind);
    CTerm *exit = new_term(b, CT_APPCONT);
    exit->as.appcont.kont = hret;
    CAtom rv; memset(&rv, 0, sizeof rv);
    if (unit_exit) {
        rv.kind = CA_UNIT; rv.ty = TY_NIL;
    } else {
        rv.kind = CA_VAR; rv.ty = result_bd->type.kind; rv.type = &result_bd->type; rv.var = result_bd;
    }
    exit->as.appcont.v = rv;

    /* Body = fold(cond pending) around CT_IF(cond, iter, exit), re-evaluated each
     * iteration inside the helper. */
    Pending cp = {0};
    CAtom ca = atomize(b, cond, &cp);
    CTerm *iff = new_term(b, CT_IF);
    iff->as.if_.cond = ca;
    iff->as.if_.then_ = iter;
    iff->as.if_.else_ = exit;
    CTerm *loopbody = fold_pending(b, &cp, iff);

    CTerm *t = new_term(b, CT_LOOP);
    t->as.loop.params = params;
    t->as.loop.n_params = nv;
    t->as.loop.inits = inits;
    t->as.loop.body = loopbody;
    t->as.loop.result_kont = rk;
    if (join_mode) {
        /* letcont j(x) = rest in LOOP -- the loop's exit delivers unit to j. */
        CTerm *lc = new_term(b, CT_LETCONT);
        CVar jv; memset(&jv, 0, sizeof jv);
        jv.id = rk.id; jv.name = "j"; jv.ty = rk.ty; jv.type = x.type;
        lc->as.letcont.j = jv; lc->as.letcont.param = x;
        lc->as.letcont.jbody = rest; lc->as.letcont.body = t;
        return lc;
    }
    return t;
}

/* cps-while-native: the loop back-edge -- re-enter with the current $next CVars. */
static CTerm *make_continue(CpsB *b) {
    CTerm *t = new_term(b, CT_CONTINUE);
    CAtom *args = arena_alloc(b->a, (b->n_loop ? b->n_loop : 1) * sizeof(CAtom));
    for (uint32_t i = 0; i < b->n_loop; i++) args[i] = atom_cvar(b->loop_nexts[i]);
    t->as.cont_.args = args;
    t->as.cont_.n = b->n_loop;
    return t;
}

/* (cont? k): lower to a LETPRIM whose sentinel spec (c_op "TUR_DK_CONT_PRED")
 * the CT emitter special-cases to `!((DK *)k)->consumed`.  The DK `consumed`
 * flag is set at the user resume site (emit_resume), so this matches the fiber
 * `tur_effect_cont_valid` semantics (true while unconsumed, false after resume).
 * Shared by the tail and bind translations; intercepted before their switches so
 * flag-off (no `consumed` field) keeps the historical direct-emit/unsupported
 * behaviour byte-identical. */
static const BuiltinSpec s_cont_pred_spec = {
    "cont?", NULL, 1, 1, {0}, {0}, BS_PREFIX_UNARY, "TUR_DK_CONT_PRED", NULL
};
static CTerm *build_cont_pred(CpsB *b, Expr *e, CVar x, CTerm *body) {
    Pending p = {0};
    CAtom *args = arena_alloc(b->a, sizeof(CAtom));
    args[0] = atomize(b, e->as.cont_pred_.expr, &p);
    CTerm *t = new_term(b, CT_LETPRIM);
    t->as.letprim.x = x; t->as.letprim.op = "cont?";
    t->as.letprim.spec = &s_cont_pred_spec;
    t->as.letprim.args = args; t->as.letprim.n = 1;
    t->as.letprim.body = body;
    return fold_pending(b, &p, t);
}

/* ---- cps_tail: deliver e's value to `kont` ---------------------------- */

/* B4: is a `match` DK-lowerable to CT_MATCH?  Restricted to the tractable shape:
 * a HEAP-ADT (int64-carrier, tagged) scrutinee, arms that are constructor
 * patterns (an optional trailing wildcard catch-all), no guards, no literal /
 * union / by-value-product arms.  Anything else evicts (CT_UNSUPPORTED).
 * Unconditional since cps-tramp-resume graduated (2026-07-19). */
static bool match_dk_ok(const Expr *e) {
    const Expr *scrut = e->as.match_.scrutinee;
    if (!scrut) return false;
    Type st = scrut->type;
    if (st.kind != TY_ADT || !st.as.adt_.def) return false;
    const AdtDef *def = st.as.adt_.def;
    /* Target a BOXED TAGGED SUM ADT: >=2 constructors (so it carries a `tag`
     * word) AND at least one field-bearing ctor (so a value is a pointer to a
     * `tur_adt_<Name>` struct read via `->tag` / `->Ctor.fieldN`, the int64
     * carrier the emitter casts).  This excludes bare-int enums (all-nullary --
     * the value IS the tag, no struct).
     *
     * colored-call-inside-match-evicts-the-cps-backend: a BY-VALUE ADT
     * scrutinee (a `defdata` record or flat sum -- `adt_is_byvalue_product`,
     * not `:heap`) is admitted too.  emit_match already binds a by-value
     * scrutinee to a local copy and reads through its address (the SR1 path),
     * so the only thing keeping `(match (MkWrap 1) (MkWrap n) (+ n (g)))` out
     * of the CPS backend was this gate -- and with `(g)` colored, the fallback
     * (safe_to_delegate) cannot take it either, so the function evicted with
     * "no lowering here".  A single-constructor record has NO tag word (its
     * one arm is unconditional at emit); a by-value sum has the same `tag` /
     * `as.<Ctor>` layout the boxed one reads.  All-nullary enums stay out on
     * both paths (the value is the bare tag).  Whether the by-value atom may
     * cross a DK slot at all is slot_box_ty's question (owning-free products
     * only), asked by term_core_ok on the scrutinee atom exactly as before. */
    bool byval = adt_is_byvalue_product(def) && !def->is_heap;
    if (def->n_ctors < 2 && !byval) return false;
    if (def->n_ctors < 1) return false;
    bool has_field_ctor = false;
    for (uint32_t c = 0; c < def->n_ctors; c++)
        if (def->ctors[c] && def->ctors[c]->n_fields > 0) { has_field_ctor = true; break; }
    if (!has_field_ctor) return false;
    uint32_t n = e->as.match_.n_arms;
    if (n == 0) return false;
    for (uint32_t i = 0; i < n; i++) {
        const MatchArm *arm = &e->as.match_.arms[i];
        if (arm->guard) return false;
        const MatchPattern *pat = &arm->pattern;
        /* The scrutinee is TY_ADT (unions are excluded above), so union_member_idx
         * here is the ctor index, not a union tag -- do not reject on it. */
        if (pat->is_literal) return false;
        if (pat->ctor) continue;                        /* ctor arm: ok */
        if (pat->is_wildcard && i == n - 1) continue;   /* trailing catch-all: ok */
        return false;                                    /* var catch-all / mid wildcard: evict */
    }
    return true;
}

/* Build a CT_MATCH whose arms each deliver to `kont` (tail).  Each ctor arm's
 * field bindings are recorded for the emitter to extract from the scrutinee. */
static CTerm *build_match_term(CpsB *b, Expr *e, CAtom scrut, CKont kont) {
    CTerm *t = new_term(b, CT_MATCH);
    t->as.match.scrut = scrut;
    t->as.match.adt = e->as.match_.scrutinee->type.as.adt_.def;
    uint32_t n = e->as.match_.n_arms;
    t->as.match.n_arms = n;
    CMatchArm *arms = arena_alloc(b->a, (n ? n : 1) * sizeof(CMatchArm));
    for (uint32_t i = 0; i < n; i++) {
        const MatchArm *arm = &e->as.match_.arms[i];
        const MatchPattern *pat = &arm->pattern;
        arms[i].ctor = pat->ctor;
        arms[i].fields = (const struct Binding **)pat->bindings;
        arms[i].n_fields = pat->ctor ? pat->n_bindings : 0;
        arms[i].body = cps_tail(b, arm->body, kont);
    }
    t->as.match.arms = arms;
    return t;
}

/* cps-match-on-builtin-sum-evicts: may the DIRECT emitter dispatch this match
 * while its arm bodies stay CPS terms?  match_dk_ok's shape (a tagged ADT the
 * CPS emitter reads itself) is the only one CT_MATCH could take, so a colored
 * arm under a match on an `(Option A)`, a `(Result T E)`, an int or string
 * literal, or an `any` narrowing evicted the whole function -- the most common
 * way to look at a value an effectful function was handed.  The direct match
 * emitter already routes every arm body through one hook (EmitCtx.match_tail,
 * proper-tail-calls T3) on all its arm shapes but a session offer; the CPS
 * emitter takes that hook here.  What the direct emitter still evaluates must
 * be delegatable: the scrutinee is bound to an atom first (any control in it
 * is translated), and a guard must carry none.  A `!`-typed match has no
 * result temp to declare. */
static Expr *cvar_expr(CpsB *b, CVar *y, const Type *ty, Span sp);
static bool expr_writes(const Expr *e);
static bool match_direct_dispatch_ok(CpsB *b, const Expr *e) {
    const Expr *scrut = e->as.match_.scrutinee;
    if (!scrut || e->as.match_.n_arms == 0) return false;
    if (scrut->type.kind == TY_SESSION_OFFER) return false;
    if (e->type.kind == TY_NEVER) return false;
    for (uint32_t i = 0; i < e->as.match_.n_arms; i++) {
        const MatchArm *arm = &e->as.match_.arms[i];
        if (!arm->body) return false;
        /* A guard is direct-emitted inside the dispatch, so it must be
         * delegatable, and it must not write: the capture and mutation walks
         * see its reads (match.guard_vars), not a store. */
        if (arm->guard && (!safe_to_delegate(b, arm->guard) || expr_writes(arm->guard)))
            return false;
    }
    return true;
}

/* Does `e` store anywhere (a `set!` of any shape, a `def`)? */
static bool expr_writes_visit(const Expr *c, void *ud) { (void)ud; return expr_writes(c); }
static bool expr_writes(const Expr *e) {
    e = ascribe_peel(e);
    if (!e) return false;
    switch (e->kind) {
        case EX_SET: case EX_SET_DEREF: case EX_SET_FIELD: case EX_DEF:
            return true;
        case EX_FN: case EX_CLOSURE: case EX_FN_DEF:
            return false;     /* a definition runs nothing here */
        default:
            return cps_visit_children(e, expr_writes_visit, NULL);
    }
}

/* Build the direct-dispatch CT_MATCH (see match_direct_dispatch_ok), each arm
 * delivering to `kont`.  The scrutinee is bound to a fresh binder the copied
 * match names, unless it is already a variable. */
static CTerm *build_direct_match_term(CpsB *b, Expr *e, CKont kont) {
    Expr *scrut = e->as.match_.scrutinee;
    const Expr *ps = ascribe_peel(scrut);
    Expr *m = arena_alloc(b->a, sizeof(Expr));
    *m = *e;
    CVar y = {0};
    bool bind_scrut = !(ps && ps->kind == EX_VAR && ps->as.var.binding);
    if (bind_scrut) {
        y = fresh_cvar(b, &scrut->type);
        m->as.match_.scrutinee = cvar_expr(b, &y, &scrut->type, scrut->span);
    }
    Pending p = {0};
    CAtom sa = atomize(b, m->as.match_.scrutinee, &p);
    CTerm *t = new_term(b, CT_MATCH);
    t->as.match.scrut = sa;
    t->as.match.adt = NULL;
    t->as.match.direct = m;
    uint32_t n = e->as.match_.n_arms;
    t->as.match.n_arms = n;
    CMatchArm *arms = arena_alloc(b->a, n * sizeof(CMatchArm));
    for (uint32_t i = 0; i < n; i++) {
        const MatchArm *arm = &e->as.match_.arms[i];
        const MatchPattern *pat = &arm->pattern;
        arms[i].ctor = pat->ctor;
        if (pat->ctor) {
            arms[i].fields = (const struct Binding **)pat->bindings;
            arms[i].n_fields = pat->n_bindings;
        } else if (pat->var_binding) {
            const struct Binding **one = arena_alloc(b->a, sizeof *one);
            one[0] = pat->var_binding;
            arms[i].fields = one;
            arms[i].n_fields = 1;
        } else {
            arms[i].fields = NULL;
            arms[i].n_fields = 0;
        }
        arms[i].body = cps_tail(b, arm->body, kont);
        /* The guard's enclosing reads (its arm's binders are its own). */
        if (arm->guard) {
            uint32_t nfv = 0;
            Binding **fv = collect_free_vars(arm->guard, (Binding **)arms[i].fields,
                                             (uint8_t)(arms[i].n_fields > 255 ? 255 : arms[i].n_fields),
                                             NULL, 0, &nfv);
            for (uint32_t k = 0; k < nfv; k++) {
                const Binding *gb = fv[k];
                if (!gb || gb->is_global) continue;
                bool dup = false;
                for (uint32_t j = 0; j < t->as.match.n_guard_vars; j++)
                    if (t->as.match.guard_vars[j].var == gb) dup = true;
                if (dup) continue;
                if (t->as.match.n_guard_vars % 8 == 0) {
                    CAtom *ng = arena_alloc(b->a, (t->as.match.n_guard_vars + 8) * sizeof(CAtom));
                    if (t->as.match.n_guard_vars)
                        memcpy(ng, t->as.match.guard_vars, t->as.match.n_guard_vars * sizeof(CAtom));
                    t->as.match.guard_vars = ng;
                }
                CAtom *ga = &t->as.match.guard_vars[t->as.match.n_guard_vars++];
                memset(ga, 0, sizeof *ga);
                ga->kind = CA_VAR; ga->var = gb;
                ga->ty = gb->type.kind; ga->type = &gb->type;
            }
            free(fv);
        }
    }
    t->as.match.arms = arms;
    CTerm *r = fold_pending(b, &p, t);
    return bind_scrut ? cps_bind(b, scrut, y, r) : r;
}

/* The implicit `else` of a one-armed `if` -- what `(when c body)` desugars to.
 * Such an `if` is nil-typed (the type checker rejects it otherwise: a present
 * else must match the then branch, and a nil then branch is what makes the
 * one-armed form well-typed), so the missing arm delivers unit to the same
 * continuation the taken arm does.
 *
 * Before this existed both sites below called `cps_tail(b, NULL, kont)`, which
 * lands in the null guard and yields CT_UNSUPPORTED -- so any `when` in a
 * position that has to stay on the CPS path evicted the whole function.  In a
 * handler clause that eviction had no recovery: the clause's `perform` reached
 * the direct emitter, which aborts.  See
 * docs/archive/handler-clause-statement-if-ices-emitter.md. */
/* An EX_VAR naming fresh binder y (its Binding synthesized and attached, so
 * the delegated Expr and the binder's declaration spell the same C name). */
static Expr *cvar_expr(CpsB *b, CVar *y, const Type *ty, Span sp) {
    Binding *yb = arena_alloc(b->a, sizeof(Binding));
    memset(yb, 0, sizeof(Binding));
    Symbol *ys = arena_alloc(b->a, sizeof(Symbol));
    memset(ys, 0, sizeof(Symbol));
    ys->name = y->name;
    ys->len = (uint32_t)strlen(y->name);
    yb->name = ys;
    yb->type = *ty;
    yb->id = y->id;
    y->bind = yb;
    Expr *yv = arena_alloc(b->a, sizeof(Expr));
    memset(yv, 0, sizeof(Expr));
    yv->kind = EX_VAR;
    yv->type = *ty;
    yv->span = sp;
    yv->as.var.binding = yb;
    return yv;
}
/* `x := reinterpret(e')` where e' carries control: bind e' to a fresh binder y
 * of e''s own type (any translation the operand needs), then x from a
 * synthesized reinterpret of y -- atomic, so delegated to the direct emitter,
 * which spells the retype (pointer through intptr_t, a narrowing cast). */
/* The same for a numeric conversion `(as T e')` (EX_CAST) whose operand
 * carries control -- `(as float (+ x (perform (Ask))))`, which evicted the
 * whole function (`unsupported form: EX_CAST`): bind e' to a binder of its own
 * type, then x from a delegated cast of that binder. */
static CTerm *cps_bind_cast(CpsB *b, Expr *e, CVar x, CTerm *rest) {
    Expr *inner = e->as.cast_.expr;
    CVar y = fresh_cvar(b, &inner->type);
    Expr *yv = cvar_expr(b, &y, &inner->type, e->span);
    Expr *ce = arena_alloc(b->a, sizeof(Expr));
    *ce = *e;
    ce->as.cast_.expr = yv;
    return cps_bind(b, inner, y, build_letraw(b, ce, x, rest));
}
static CTerm *cps_bind_reinterp(CpsB *b, Expr *e, CVar x, CTerm *rest) {
    Expr *inner = e->as.reinterpret_.expr;
    CVar y = fresh_cvar(b, &inner->type);
    Expr *yv = cvar_expr(b, &y, &inner->type, e->span);
    Expr *re = arena_alloc(b->a, sizeof(Expr));
    *re = *e;
    re->as.reinterpret_.expr = yv;
    return cps_bind(b, inner, y, build_letraw(b, re, x, rest));
}

/* cps-evicts-handle-in-operand-positions: an INDIRECT call (a fn value, a
 * rank-2 poly param) whose arguments are not all literals -- `(l (handle ...))`
 * -- was refused outright.  The call itself never joins the caller's
 * delimited-control chain (see the arms that use this), so it is delegatable
 * once its arguments are: bind each non-atomic argument first, left to right
 * (any translation its control needs), and delegate the call over binders of
 * those values.  NULL when an argument cannot be bound that way. */
static CTerm *delegate_call_atomized(CpsB *b, Expr *e, CVar x, CTerm *rest,
                                     bool tail) {
    uint32_t n = e->as.call_.n_args;
    if (n == 0 || n > 32) return NULL;
    /* Only for an argument that CARRIES CONTROL (the `handle`): a call whose
     * arguments are merely non-literal keeps the function's eviction, which
     * also keeps a TAIL indirect call a tail call -- delegated, it becomes a
     * bind and a return, and the r7rs programs that recurse through closures
     * ran out of stack.  And only scalar / pointer binders: a by-value
     * aggregate argument comes back from a carrier-returning call as a word,
     * and the binder would need the direct emitter's unbox. */
    bool any_control = false, all_scalar = true;
    for (uint32_t i = 0; i < n; i++) {
        Expr *a = e->as.call_.args[i];
        const Expr *pa = ascribe_peel(a);
        if (is_atomic(a) || is_widened_literal(a) || (pa && pa->kind == EX_DICT))
            continue;
        if (!safe_to_delegate(b, a)) any_control = true;
        TypeKind k = a->type.kind;
        if (!(tierA_scalar_kind(k) || k == TY_FLOAT || k == TY_FLOAT64 ||
              k == TY_FLOAT32))
            all_scalar = false;
    }
    /* In BIND position nothing is lost by delegating the whole call when its
     * arguments are themselves delegatable (`(.app s (g "x"))` ahead of a
     * `handle` in the same body); the tail rule above is about tail calls. */
    if (!any_control) return tail ? NULL : build_letraw(b, e, x, rest);
    if (!all_scalar) return NULL;
    Pending p = {0};
    Expr **nargs = arena_alloc(b->a, n * sizeof(Expr *));
    for (uint32_t i = 0; i < n; i++) {
        Expr *a = e->as.call_.args[i];
        const Expr *pa = ascribe_peel(a);
        if (is_atomic(a) || is_widened_literal(a) || (pa && pa->kind == EX_DICT)) {
            nargs[i] = a;
            continue;
        }
        if (p.n >= 32) return NULL;
        CVar y = fresh_cvar(b, &a->type);
        nargs[i] = cvar_expr(b, &y, &a->type, a->span);
        p.items[p.n].expr = a; p.items[p.n].x = y; p.n++;
    }
    Expr *call = arena_alloc(b->a, sizeof(Expr));
    *call = *e;
    call->as.call_.args = nargs;
    return fold_pending(b, &p, build_letraw(b, call, x, rest));
}

static CTerm *cps_tail_unit(CpsB *b, CKont kont) {
    if (kont.kind == KK_LOOP) return make_continue(b);
    CTerm *t = new_term(b, CT_APPCONT);
    CAtom u; memset(&u, 0, sizeof u); u.kind = CA_UNIT; u.ty = TY_NIL;
    t->as.appcont.kont = kont;
    t->as.appcont.v = u;
    return t;
}

/* generic-call-result-in-generic-collapses-to-int: the outermost reinterpret
 * typed as the enclosing generic's own tyvar in `e`'s ascription chain, or NULL.
 * Its VALUE is the clone's concrete `A` (the direct emitter's tyvar arm), while
 * the call it wraps is typed with the carrier `int`; a delegated binding of the
 * wrapped call must therefore delegate the WRAPPER and take ITS type, or the
 * result variable is declared `int64_t` and a spec returning `double` lands in
 * it by value conversion. */
static const Expr *tyvar_reinterp_wrapper(const Expr *e) {
    while (e && e->kind == EX_ASCRIBE) e = e->as.ascribe_.inner;
    return (e && e->kind == EX_REINTERPRET &&
            e->as.reinterpret_.target_kind == TY_TYVAR &&
            e->as.reinterpret_.expr) ? e : NULL;
}

static CTerm *cps_tail(CpsB *b, Expr *e, CKont kont) {
    {
        const Expr *w = tyvar_reinterp_wrapper(e);
        const Expr *in = w ? ascribe_peel(w) : NULL;
        if (w && in && kont.kind != KK_LOOP && !is_atomic(in) &&
            safe_to_delegate(b, in)) {
            CVar x = fresh_cvar(b, &w->type);
            CTerm *ac = new_term(b, CT_APPCONT);
            ac->as.appcont.kont = kont; ac->as.appcont.v = atom_cvar(x);
            return build_letraw(b, (Expr *)w, x, ac);
        }
    }
    e = (Expr *)ascribe_peel(e);
    e = pap_maybe_rewrite(b, e);
    /* A `letrec` of captureless members binds global names, not values. */
    if (e && e->kind == EX_LETREC && letrec_all_global_members(e))
        return cps_tail(b, e->as.let_.body, kont);
    /* A `letrec` of one capturing member, only ever called: lowered like a
     * `let` of a local closure (cps_ir_let_local_closure) -- its calls, and
     * its lambda's own self-calls (cps_self_env_call), are the lambda's CPS
     * calls. */
    if (e && letrec_local_closure(b, e)) {
        uint32_t saved_pap = b->n_pap;
        if (!pap_register_let(b, e)) {
            b->n_pap = saved_pap;
            return unsupported_form(b, e);
        }
        CTerm *rest = cps_tail(b, e->as.let_.body, kont);
        rest = cps_bind_let_init(b, e, 0, cvar_of_binding(e->as.let_.bindings[0].binding), rest);
        b->n_pap = saved_pap;
        return rest;
    }
    if (!e) {
        /* cps-while-native: a null (unit) loop-body tail is the back-edge. */
        if (kont.kind == KK_LOOP) return make_continue(b);
        CTerm *t = new_term(b, CT_UNSUPPORTED);
        t->as.unsupported.why = "null";
        return t;
    }
    if (is_atomic(e)) {
        /* cps-while-native: the loop-body tail value (unit) is discarded -- the
         * carried state rides the $next CVars -- so a tail in KK_LOOP position is
         * the back-edge. */
        if (kont.kind == KK_LOOP) return make_continue(b);
        CTerm *t = new_term(b, CT_APPCONT);
        t->as.appcont.kont = kont;
        t->as.appcont.v = atom_of(e);
        return t;
    }
    /* perform-inside-loop-has-no-lowering: a NON-structural form in loop-tail
     * position (a call, a set! of a cell-carried var, a handle, a match, ...).
     * The arms below that deliver `(kont, x)` have no KK_LOOP spelling -- an
     * APPCONT to the loop kont reached the emitter as a join jump -- so bind the
     * form's value and take the back-edge instead.  The structural forms
     * (do/let/if/return) thread the loop kont into their own tails. */
    if (kont.kind == KK_LOOP) {
        switch (e->kind) {
            case EX_DO: case EX_LET: case EX_LETREC: case EX_IF: case EX_RETURN:
                break;
            default: {
                CVar x = fresh_cvar(b, &e->type);
                return cps_bind(b, e, x, make_continue(b));
            }
        }
    }
    if (is_delegatable_owning(e) || is_delegatable_struct(e) || is_delegatable_value(e)) {
        /* bind the delegated op's result, then deliver it to the continuation. */
        CVar x = fresh_cvar(b, &e->type);
        CTerm *ac = new_term(b, CT_APPCONT);
        ac->as.appcont.kont = kont; ac->as.appcont.v = atom_cvar(x);
        return build_letraw(b, e, x, ac);
    }
    /* (cont? k) in tail position: deliver the unconsumed-check to kont. */
    if (e->kind == EX_CONT_PRED) {
        CVar x = fresh_cvar(b, &e->type);
        CTerm *ac = new_term(b, CT_APPCONT);
        ac->as.appcont.kont = kont; ac->as.appcont.v = atom_cvar(x);
        return build_cont_pred(b, e, x, ac);
    }
    /* (with-handler <literal> body) in tail position: a handle over body. */
    if (e->kind == EX_WITH_HANDLER) {
        CVar x = fresh_cvar(b, &e->type);
        CTerm *ac = new_term(b, CT_APPCONT);
        ac->as.appcont.kont = kont; ac->as.appcont.v = atom_cvar(x);
        return build_with_handler(b, e, x, ac);
    }
    switch (e->kind) {
        case EX_BUILTIN: {
            Pending p = {0};
            CAtom *args = arena_alloc(b->a, (e->as.builtin.n ? e->as.builtin.n : 1) * sizeof(CAtom));
            for (uint32_t i = 0; i < e->as.builtin.n; i++)
                args[i] = atomize(b, e->as.builtin.args[i], &p);
            CVar x = fresh_cvar(b, &e->type);
            CTerm *ac = new_term(b, CT_APPCONT);
            ac->as.appcont.kont = kont; ac->as.appcont.v = atom_cvar(x);
            CTerm *t = new_term(b, CT_LETPRIM);
            t->as.letprim.x = x; t->as.letprim.op = builtin_name(e);
            t->as.letprim.spec = e->as.builtin.spec;
            t->as.letprim.args = args; t->as.letprim.n = e->as.builtin.n;
            t->as.letprim.body = ac;
            return fold_pending(b, &p, t);
        }
        case EX_CALL: {
            /* An EMPTY-row call through a FAT thread param threads the caller's
             * continuation the way an effectful-row one does
             * (call_through_fat_param); the dispatch calls an unregistered --
             * pure -- value directly. */
            if (!call_is_effectful_fnvalue(e) && call_through_fat_param(e)
                && cps_ir_thread_param_has(e->as.call_.fn_binding) && call_args_pendable(e)) {
                Pending pp = {0};
                const Binding *pf = e->as.call_.fn_binding;
                uint32_t n = e->as.call_.n_args;
                CAtom *args = arena_alloc(b->a, (n ? n : 1) * sizeof(CAtom));
                for (uint32_t i = 0; i < n; i++)
                    args[i] = atomize_call_arg(b, e, i, &pp);
                CTerm *t = new_term(b, CT_TAILCALL);
                t->as.tailcall.fn = pf; t->as.tailcall.args = args;
                t->as.tailcall.n = n; t->as.tailcall.kont = kont;
                t->as.tailcall.via_registry = true;
                t->as.tailcall.call_expr = e;
                return fold_pending(b, &pp, t);
            }
            /* E2/taint-completeness: an effectful fn-value call cannot thread the
             * DK yet; delegating it to fiber under a DK handle escapes the effect.
             * Evict so the whole fn stays fiber (its effect taints -> the DK
             * handler-installer co-classifies to fiber). */
            if (call_is_effectful_fnvalue(e)) {
                /* E2a: a tier-`now` thread-param call THREADS the DK via the registry. */
                const Binding *pf = e->as.call_.fn_binding;
                if (pf && cps_ir_thread_param_has(pf) && call_args_pendable(e)) {
                    Pending pp = {0};
                    uint32_t n = e->as.call_.n_args;
                    CAtom *args = arena_alloc(b->a, (n ? n : 1) * sizeof(CAtom));
                    for (uint32_t i = 0; i < n; i++)
                        args[i] = atomize_call_arg(b, e, i, &pp);
                    CTerm *t = new_term(b, CT_TAILCALL);
                    t->as.tailcall.fn = pf; t->as.tailcall.args = args;
                    t->as.tailcall.n = n; t->as.tailcall.kont = kont;
                    t->as.tailcall.via_registry = true;
                    return fold_pending(b, &pp, t);
                }
                /* E2c: an effectful fn-value stored in a STRUCT FIELD, called via
                 * `(.f obj args)` -- the callee is a field load (fn_expr =
                 * EX_GET_FIELD), not a param.  Thread it via the registry keyed on
                 * the field-load atom, delivering __kont so the fn-value's perform
                 * reaches the caller's handler.  The field-stored fn-value is
                 * force-registered (emit_cps_ir.c registration loop). */
                if (!pf && e->as.call_.fn_expr
                    && e->as.call_.fn_expr->kind == EX_GET_FIELD
                    && call_args_pendable(e)) {
                    Pending pp = {0};
                    CAtom fnatom = atomize(b, e->as.call_.fn_expr, &pp);
                    uint32_t n = e->as.call_.n_args;
                    CAtom *args = arena_alloc(b->a, (n ? n : 1) * sizeof(CAtom));
                    for (uint32_t i = 0; i < n; i++)
                        args[i] = atomize_call_arg(b, e, i, &pp);
                    CTerm *t = new_term(b, CT_TAILCALL);
                    t->as.tailcall.fn = NULL; t->as.tailcall.fn_atom = fnatom;
                    t->as.tailcall.args = args; t->as.tailcall.n = n;
                    t->as.tailcall.kont = kont; t->as.tailcall.via_registry = true;
                    return fold_pending(b, &pp, t);
                }
                CTerm *t = new_term(b, CT_UNSUPPORTED);
                t->as.unsupported.why = "effectful fn-value call (E2 pending)";
                return t;
            }
            const Binding *fn = e->as.call_.fn_binding;
            if (!fn) {
                /* Indirect call: the fn VALUE is the callee's direct entry point
                 * (a colored fn's value points at its direct wrapper, which
                 * installs its own root prompt), so an indirect call never
                 * participates in the caller's delimited-control chain -- its
                 * behaviour is identical whether the surrounding code is CPS- or
                 * direct-emitted.  Delegate it to the direct emitter with atomic
                 * args, exactly as an uncolored direct call is delegated. */
                if (!indirect_callee_ok(e->as.call_.fn_expr)) {
                    CTerm *t = new_term(b, CT_UNSUPPORTED);
                    t->as.unsupported.why = "indirect call through capturing closure";
                    return t;
                }
                if (!call_args_literal(e)) {
                    CVar x = fresh_cvar(b, &e->type);
                    CTerm *ac = new_term(b, CT_APPCONT);
                    ac->as.appcont.kont = kont; ac->as.appcont.v = atom_cvar(x);
                    CTerm *d = delegate_call_atomized(b, e, x, ac, true);
                    if (d) return d;
                    CTerm *t = new_term(b, CT_UNSUPPORTED);
                    t->as.unsupported.why = "indirect call (non-atomic args)";
                    return t;
                }
                CVar x = fresh_cvar(b, &e->type);
                CTerm *ac = new_term(b, CT_APPCONT);
                ac->as.appcont.kont = kont; ac->as.appcont.v = atom_cvar(x);
                return build_letraw(b, e, x, ac);
            }
            /* E2 (fat-closure fn-value threading): a TAIL call THROUGH a
             * fat-closure poly-fn PARAM (`f : (-> int int)`, is_poly_fn) is
             * dispatched through the closure's `fn_cps` DK-threading slot instead
             * of being delegated to the direct emitter (which would call `f.fn`
             * off the trampoline, escaping an effectful callback's perform).  The
             * emitter picks `fn_cps` when populated (an effectful fn-value) and
             * the direct `f.fn` path otherwise, so a pure fn-value is unchanged.
             * Restricted to the slot's word ABI (fncps_param_call_ok). */
            if (fncps_param_call_ok(fn, e)) {
                Pending pp = {0};
                CAtom *args = fncps_atomize_args(b, e, &pp);
                CTerm *t = new_term(b, CT_TAILCALL);
                t->as.tailcall.fn = fn; t->as.tailcall.args = args;
                t->as.tailcall.n = e->as.call_.n_args; t->as.tailcall.kont = kont;
                t->as.tailcall.via_fncps = true;
                t->as.tailcall.call_expr = e;
                return fold_pending(b, &pp, t);
            }
            /* A cps->direct call to an uncolored callee with atomic args is
             * delegated to the direct emitter, which resolves the monomorphized
             * callee name (constructors, specialized fns) the CPS backend's own
             * naming does not.  Colored callees still thread the continuation. */
            if (!callee_colored(b, fn) && call_args_delegatable(b, e)) {
                CVar x = fresh_cvar(b, &e->type);
                CTerm *ac = new_term(b, CT_APPCONT);
                ac->as.appcont.kont = kont; ac->as.appcont.v = atom_cvar(x);
                return build_letraw(b, e, x, ac);
            }
            /* all-any-fn-param-is-unusable: a callee BINDING with no FnDef
             * behind it is a fn VALUE (a fn-typed param or local), not a name
             * the CT_LETCALL / CT_TAILCALL arms can spell as `f(args)` -- that
             * emitted `__t1 = f(__t0)` against an `int64_t f` for
             * `(defn tany [f : (fn [any] any)] : any (f (:: 4.5 any)))`, a hard
             * cc error.  Route it exactly like the binding-less indirect call
             * above: delegate with atomic args, otherwise evict. */
            if (!fn->source_fn_def && !callee_colored(b, fn)) {
                if (!call_args_literal(e)) {
                    CVar x = fresh_cvar(b, &e->type);
                    CTerm *ac = new_term(b, CT_APPCONT);
                    ac->as.appcont.kont = kont; ac->as.appcont.v = atom_cvar(x);
                    CTerm *d = delegate_call_atomized(b, e, x, ac, true);
                    if (d) return d;
                    CTerm *t = new_term(b, CT_UNSUPPORTED);
                    t->as.unsupported.why = "indirect call (non-atomic args)";
                    return t;
                }
                CVar x = fresh_cvar(b, &e->type);
                CTerm *ac = new_term(b, CT_APPCONT);
                ac->as.appcont.kont = kont; ac->as.appcont.v = atom_cvar(x);
                return build_letraw(b, e, x, ac);
            }
            Pending p = {0};
            uint32_t n = e->as.call_.n_args;
            CAtom *args = arena_alloc(b->a, (n ? n : 1) * sizeof(CAtom));
            for (uint32_t i = 0; i < n; i++)
                args[i] = atomize_call_arg(b, e, i, &p);
            if (callee_colored(b, fn)) {
                CTerm *t = new_term(b, CT_TAILCALL);
                t->as.tailcall.fn = fn; t->as.tailcall.args = args;
                t->as.tailcall.n = n; t->as.tailcall.kont = kont;
                t->as.tailcall.call_expr = e;   /* RC2: retain for method re-resolution */
                return fold_pending(b, &p, t);
            } else {
                CVar x = fresh_cvar(b, &e->type);
                CTerm *ac = new_term(b, CT_APPCONT);
                ac->as.appcont.kont = kont; ac->as.appcont.v = atom_cvar(x);
                CTerm *t = new_term(b, CT_LETCALL);
                t->as.letcall.x = x; t->as.letcall.fn = fn;
                t->as.letcall.args = args; t->as.letcall.n = n; t->as.letcall.body = ac;
                t->as.letcall.call_expr = e;   /* RC2: retain for method re-resolution */
                return fold_pending(b, &p, t);
            }
        }
        case EX_IF: {
            Pending p = {0};
            CAtom c = atomize(b, e->as.if_.cond, &p);
            CTerm *t = new_term(b, CT_IF);
            t->as.if_.cond = c;
            t->as.if_.then_ = cps_tail(b, e->as.if_.then_, kont);
            t->as.if_.else_ = e->as.if_.else_or_null
                ? cps_tail(b, e->as.if_.else_or_null, kont)
                : cps_tail_unit(b, kont);
            return fold_pending(b, &p, t);
        }
        case EX_MATCH: {
            /* B4: DK-lower a restricted heap-ADT ctor match to CT_MATCH.  A match
             * outside that shape (an int/enum match, a by-value product, a match
             * with guards) is NOT intercepted -- it falls through to the wholesale
             * direct-emitter delegation (safe_to_delegate -> CT_LETRAW) the default
             * case owns, exactly as before this case existed. */
            if (match_dk_ok(e)) {
                Pending p = {0};
                CAtom scrut = atomize(b, e->as.match_.scrutinee, &p);
                CTerm *t = build_match_term(b, e, scrut, kont);
                return fold_pending(b, &p, t);
            }
            if (safe_to_delegate(b, e)) {
                CVar x = fresh_cvar(b, &e->type);
                CTerm *ac = new_term(b, CT_APPCONT);
                ac->as.appcont.kont = kont; ac->as.appcont.v = atom_cvar(x);
                return build_letraw(b, e, x, ac);
            }
            if (match_direct_dispatch_ok(b, e))
                return build_direct_match_term(b, e, kont);
            return unsupported_form(b, e);
        }
        case EX_LET: {
            uint32_t saved_pap = b->n_pap;
            if (!pap_register_let(b, e)) {
                b->n_pap = saved_pap;
                return unsupported_form(b, e);
            }
            CTerm *rest = cps_tail(b, e->as.let_.body, kont);
            for (int i = (int)e->as.let_.n - 1; i >= 0; i--) {
                if (pap_binding_inlined(b, e, (uint32_t)i, saved_pap)) {
                    /* Closure dropped; emit only its prelude capture bindings so
                     * the inlined saturated call's capture refs stay in scope. */
                    const Expr *init = ascribe_peel(e->as.let_.bindings[i].init);
                    if (init && init->kind == EX_LET)
                        for (int j = (int)init->as.let_.n - 1; j >= 0; j--)
                            rest = cps_bind_let_init(b, init, (uint32_t)j,
                                        cvar_of_binding(init->as.let_.bindings[j].binding), rest);
                    continue;
                }
                rest = cps_bind_let_init(b, e, (uint32_t)i,
                                cvar_of_binding(e->as.let_.bindings[i].binding), rest);
            }
            b->n_pap = saved_pap;
            return rest;
        }
        case EX_DO: {
            uint32_t n = e->as.do_.n;
            if (n == 0) return cps_tail(b, NULL, kont);
            /* O1-b: a `do` ending in owning-ref auto-drop defers whose refs do
             * not cross a control op -- hoist each drop to before the first
             * barrier (or to scope exit) instead of falling back. */
            AutodropPlan pl;
            if (plan_autodrop(e, &pl)) {
                Expr **items = e->as.do_.items;
                uint32_t nr = pl.n_real, fc = pl.first_ctrl;
                CTerm *rest;
                if (fc == nr) {
                    /* No barrier in the ref's scope: compute the value, drop the
                     * refs at scope exit, then deliver. */
                    CVar x = fresh_cvar(b, &items[nr - 1]->type);
                    CTerm *ac = new_term(b, CT_APPCONT);
                    ac->as.appcont.kont = kont; ac->as.appcont.v = atom_cvar(x);
                    rest = cps_emit_hoisted_drops(b, &pl, ac);
                    rest = cps_bind(b, items[nr - 1], x, rest);
                    for (int i = (int)nr - 2; i >= 0; i--) {
                        CVar discard = fresh_cvar(b, &items[i]->type);
                        rest = cps_bind(b, items[i], discard, rest);
                    }
                } else {
                    /* Deliver the tail item, then splice the drops in just before
                     * the first control barrier (after every use of the refs). */
                    rest = cps_tail(b, items[nr - 1], kont);
                    for (int i = (int)nr - 2; i >= (int)fc; i--) {
                        CVar discard = fresh_cvar(b, &items[i]->type);
                        rest = cps_bind(b, items[i], discard, rest);
                    }
                    rest = cps_emit_hoisted_drops(b, &pl, rest);
                    for (int i = (int)fc - 1; i >= 0; i--) {
                        CVar discard = fresh_cvar(b, &items[i]->type);
                        rest = cps_bind(b, items[i], discard, rest);
                    }
                }
                return rest;
            }
            /* P5b (cps-runtime-finish): a `do` carrying explicit `(defer D)`
             * items alongside a control op (perform / colored call) the
             * whole-body path does not own.  A `__cps` function establishes no
             * defer frame, so a lone defer normally evicts (see safe_to_delegate
             * EX_DEFER).  But an explicit defer's effect is exactly "run D at this
             * block's scope exit, LIFO" -- which the continuation models directly:
             * thread each defer body into the block's continuation so it fires
             * after the tail value is produced (through any perform) and before
             * the value is delivered, in reverse-declaration order.  Scoped to a
             * value-producing tail (not itself a defer) whose every defer body is
             * control-free (safe_to_delegate); anything else falls through and
             * evicts as before.  Runs only on the per-node path (g_whole_body_-
             * delegate already delegated the whole do), so it can only turn an
             * eviction into a native emit -- never alter a working delegated
             * defer.  Matches the direct emitter's tur_frame fire-LIFO order
             * (e.g. `effect-defer`: `(do (defer (println "cleanup")) (perform
             * (Ask)))` prints cleanup then returns the resumed value). */
            {
                Expr **items = e->as.do_.items;
                bool has_defer = false, defer_ok = true;
                int tail_idx = -1;   /* last NON-defer item -- the do's value */
                for (uint32_t i = 0; i < n; i++) {
                    const Expr *it = ascribe_peel(items[i]);
                    if (it && it->kind == EX_DEFER) {
                        has_defer = true;
                        if (!safe_to_delegate(b, it->as.defer_.body)) defer_ok = false;
                    } else {
                        tail_idx = (int)i;
                    }
                }
                /* E3b (owning-cloneable-capture): the elaborator APPENDS an
                 * auto-inserted scope-exit drop `(defer (rc/drop r))` AFTER the
                 * real body, so the value item is not the last item -- trailing
                 * defer(s) follow it.  The original P5b threading bailed on a
                 * defer tail (`tail_idx != n-1`); it is allowed now, using the
                 * last non-defer item as the value and threading the
                 * trailing defer into the continuation (fires after the value is
                 * produced -- through the reset -- before delivery), exactly the
                 * straight-line drop the explicit-drop channel already emits.  This
                 * is what lets an owning `rc` the CPS-colored fn OWNS ride a
                 * cloneable capture without a hand-written drop.  Unconditional
                 * since owning-cloneable-capture graduated (2026-07-20); a defer
                 * tail no longer bails. */
                if (has_defer && defer_ok && tail_idx >= 0) {
                    CVar x = fresh_cvar(b, &items[tail_idx]->type);
                    CTerm *deliver = new_term(b, CT_APPCONT);
                    deliver->as.appcont.kont = kont;
                    deliver->as.appcont.v = atom_cvar(x);
                    /* Thread defer bodies in declaration order so they EXECUTE in
                     * reverse-declaration (LIFO) order after x is bound. */
                    CTerm *chain = deliver;
                    for (uint32_t i = 0; i < n; i++) {
                        const Expr *it = ascribe_peel(items[i]);
                        if (!it || it->kind != EX_DEFER) continue;
                        CVar d = fresh_cvar(b, &it->as.defer_.body->type);
                        chain = cps_bind(b, (Expr *)it->as.defer_.body, d, chain);
                    }
                    /* Bind the value item (threading any perform continuation). */
                    chain = cps_bind(b, items[tail_idx], x, chain);
                    /* Prepend the non-defer statements before the value, in order. */
                    for (int i = tail_idx - 1; i >= 0; i--) {
                        const Expr *it = ascribe_peel(items[i]);
                        if (it && it->kind == EX_DEFER) continue;
                        CVar discard = fresh_cvar(b, &items[i]->type);
                        chain = cps_bind(b, items[i], discard, chain);
                    }
                    return chain;
                }
            }
            CTerm *rest = cps_tail(b, e->as.do_.items[n - 1], kont);
            for (int i = (int)n - 2; i >= 0; i--) {
                CVar discard = fresh_cvar(b, &e->as.do_.items[i]->type);
                rest = cps_bind(b, e->as.do_.items[i], discard, rest);
            }
            return rest;
        }
        case EX_RETURN:
            return cps_tail(b, e->as.return_.value, b->retk);
        case EX_SET: {
            /* cps-while-native: a loop-carried set! in tail position -- bind its
             * $next CVar, then take the loop back-edge (KK_LOOP) or deliver unit. */
            int idx = loop_var_index(b, e->as.set_.target);
            if (b->n_loop && idx >= 0) {
                CTerm *cont;
                if (kont.kind == KK_LOOP) {
                    cont = make_continue(b);
                } else {
                    cont = new_term(b, CT_APPCONT);
                    CAtom u; memset(&u, 0, sizeof u); u.kind = CA_UNIT; u.ty = TY_NIL;
                    cont->as.appcont.kont = kont; cont->as.appcont.v = u;
                }
                return cps_bind(b, e->as.set_.value, b->loop_nexts[idx], cont);
            }
            if (safe_to_delegate(b, e)) {
                CVar x = fresh_cvar(b, &e->type);
                CTerm *ac = new_term(b, CT_APPCONT);
                ac->as.appcont.kont = kont; ac->as.appcont.v = atom_cvar(x);
                return build_letraw(b, e, x, ac);
            }
            return unsupported_form(b, e);
        }
        case EX_RESET: {
            CVar x = fresh_cvar(b, &e->type);
            CTerm *t = new_term(b, CT_RESET);
            t->as.reset.x = x;
            t->as.reset.delim = cps_tail(b, e->as.reset_.body, kont_prompt(e->type.kind));
            CTerm *ac = new_term(b, CT_APPCONT);
            ac->as.appcont.kont = kont; ac->as.appcont.v = atom_cvar(x);
            t->as.reset.body = ac;
            return t;
        }
        case EX_SHIFT: {
            CVar k = fresh_cvar(b, &e->type);
            k.name = "k'";
            CTerm *t = new_term(b, CT_SHIFT);
            t->as.shift.k = k;
            t->as.shift.body = cps_shift_body(b, e);
            return t;
        }
        case EX_SHIFT0: {
            CVar k = fresh_cvar(b, &e->type);
            k.name = "k'";
            CTerm *t = new_term(b, CT_SHIFT);
            t->as.shift.k = k;
            t->as.shift.shift0 = true;
            t->as.shift.body = cps_shift_body_kf(b, e->as.shift0_.k_fn,
                                                 e->as.shift0_.body, &e->type);
            return t;
        }
        case EX_CLONEABLE_RESET: {
            /* U3 native cloneable context (Shape 1/2, incl. no-shift and Shape-1
             * live-capture), else (D4) evict rather than delegate. */
            CVar x = fresh_cvar(b, &e->type);
            CTerm *ac = new_term(b, CT_APPCONT);
            ac->as.appcont.kont = kont; ac->as.appcont.v = atom_cvar(x);
            CTerm *nat = build_cloneable(b, e, x, ac);
            if (nat) return nat;
            /* No cloneable-shift is bound to this reset -> its value is just the
             * body (nothing to delimit).  Translate the body directly. */
            if (!ctx_reaches_shift(e->as.cloneable_reset_.body, EX_CLONEABLE_SHIFT))
                return cps_tail(b, e->as.cloneable_reset_.body, kont);
            /* D4: the delimited-control carve-out (delegate the reset to the direct
             * emitter via CT_LETRAW) is deleted.  A shift-bearing reset outside the
             * native build_cloneable subset now evicts the whole function to the
             * direct emitter instead of routing a sub-region to it -- so no colored
             * function delegates a delimited region.  (No corpus fixture reaches
             * here; the native subset covers every cloneable reset in the suite.) */
            return new_term(b, CT_UNSUPPORTED);
        }
        case EX_SERIAL_RESET: {
            /* U4: native arithmetic serial context, else (D4) handle a no-shift
             * reset as its plain body, else evict rather than delegate. */
            CVar x = fresh_cvar(b, &e->type);
            CTerm *ac = new_term(b, CT_APPCONT);
            ac->as.appcont.kont = kont; ac->as.appcont.v = atom_cvar(x);
            CTerm *nat = build_serial(b, e, x, ac);
            if (nat) return nat;
            if (!ctx_reaches_shift(e->as.serial_reset_.body, EX_SERIAL_SHIFT))
                return cps_tail(b, e->as.serial_reset_.body, kont);
            /* D4: carve-out deleted -- a shift-bearing serial reset outside the
             * native build_serial subset evicts the whole function to the direct
             * emitter rather than delegating the region.  (No corpus fixture
             * reaches here.) */
            return new_term(b, CT_UNSUPPORTED);
        }
        case EX_ASYNC: case EX_AWAIT: {
            /* U5: delegate the self-contained async region (async spawn / await)
             * so a colored function containing it stays CPS-emitted rather than
             * wholly evicting.  F3 (cps-async): a tail `await` lowers instead to a
             * dk_shift (build_await) capturing the continuation as a heap kont --
             * EXCEPT inside a handler case body, where it delegates to the fiber
             * path (CT_LETRAW) that handle_case_ok admits (b->in_handler_case). */
            CVar x = fresh_cvar(b, &e->type);
            CTerm *ac = new_term(b, CT_APPCONT);
            ac->as.appcont.kont = kont; ac->as.appcont.v = atom_cvar(x);
            if (e->kind == EX_AWAIT && !b->in_handler_case) {
                Pending p = {0};
                CTerm *core = build_await(b, e, x, ac, &p);
                return fold_pending(b, &p, core);
            }
            return build_letraw(b, e, x, ac);
        }
        case EX_HANDLE: {
            CVar x = fresh_cvar(b, &e->type);
            CTerm *ac = new_term(b, CT_APPCONT);
            ac->as.appcont.kont = kont; ac->as.appcont.v = atom_cvar(x);
            return build_handle(b, e, x, ac);
        }
        case EX_PERFORM: {
            Pending p = {0};
            CVar x = fresh_cvar(b, &e->type);
            CTerm *ac = new_term(b, CT_APPCONT);
            ac->as.appcont.kont = kont; ac->as.appcont.v = atom_cvar(x);
            CTerm *core = build_perform(b, e, x, ac, &p);
            return fold_pending(b, &p, core);
        }
        case EX_RESUME: {
            Pending p = {0};
            CVar x = fresh_cvar(b, &e->type);
            CTerm *ac = new_term(b, CT_APPCONT);
            ac->as.appcont.kont = kont; ac->as.appcont.v = atom_cvar(x);
            CTerm *core = build_resume(b, e, x, ac, &p);
            return fold_pending(b, &p, core);
        }
        case EX_CALLCC: {
            /* U7: native local-escape landing for a capture-free receiver, else
             * fall back to the CT_LETRAW delegation (emit_cps_callcc). */
            CVar x = fresh_cvar(b, &e->type);
            CTerm *ac = new_term(b, CT_APPCONT);
            ac->as.appcont.kont = kont; ac->as.appcont.v = atom_cvar(x);
            CTerm *nat = build_callcc(b, e, x, ac);
            return nat ? nat : build_letraw(b, e, x, ac);
        }
        case EX_WHILE: {
            /* perform-inside-loop-has-no-lowering: a `while` in TAIL position
             * (the last form of a nil-returning function, or of a handle body)
             * used to fall to the default below, which cannot delegate a loop
             * that performs and so evicted the whole function -- while the
             * same loop followed by a live value went through cps_bind and
             * build_loop.  Route the tail loop through build_loop too; its
             * continuation delivers the loop's own unit result, which
             * build_loop admits as a unit exit. */
            CVar x = fresh_cvar(b, &e->type);
            CTerm *ac = new_term(b, CT_APPCONT);
            ac->as.appcont.kont = kont; ac->as.appcont.v = atom_cvar(x);
            /* A control-free loop keeps its whole-form delegation (as the
             * default arm gave it); only a loop that performs needs the helper. */
            if (safe_to_delegate(b, e)) return build_letraw(b, e, x, ac);
            CTerm *loop = build_loop(b, e, x, ac);
            if (loop) return loop;
            return unsupported_form(b, e);
        }
        default: {
            /* N6.1: a control-op-free, colored-call-free form -- emit it wholesale
             * via the direct emitter, then deliver its result to the continuation. */
            if (safe_to_delegate(b, e)) {
                CVar x = fresh_cvar(b, &e->type);
                CTerm *ac = new_term(b, CT_APPCONT);
                ac->as.appcont.kont = kont; ac->as.appcont.v = atom_cvar(x);
                return build_letraw(b, e, x, ac);
            }
            /* As cps_bind's default arm: a Tier A reinterpret over a control-
             * bearing operand. */
            if (e->kind == EX_REINTERPRET && e->as.reinterpret_.expr) {
                CVar x = fresh_cvar(b, &e->type);
                CTerm *ac = new_term(b, CT_APPCONT);
                ac->as.appcont.kont = kont; ac->as.appcont.v = atom_cvar(x);
                return cps_bind_reinterp(b, e, x, ac);
            }
            if (e->kind == EX_CAST && e->as.cast_.expr) {
                CVar x = fresh_cvar(b, &e->type);
                CTerm *ac = new_term(b, CT_APPCONT);
                ac->as.appcont.kont = kont; ac->as.appcont.v = atom_cvar(x);
                return cps_bind_cast(b, e, x, ac);
            }
            return unsupported_form(b, e);
        }
    }
}

/* ---- cps_bind: bind e's value to x, then run rest --------------------- */

static CTerm *cps_bind(CpsB *b, Expr *e, CVar x, CTerm *rest) {
    {
        const Expr *w = tyvar_reinterp_wrapper(e);
        const Expr *in = w ? ascribe_peel(w) : NULL;
        if (w && in && !is_atomic(in) && safe_to_delegate(b, in))
            return build_letraw(b, (Expr *)w, x, rest);
    }
    e = (Expr *)ascribe_peel(e);
    if (!e) return rest;
    e = pap_maybe_rewrite(b, e);
    if (e->kind == EX_LETREC && letrec_all_global_members(e))
        return cps_bind(b, e->as.let_.body, x, rest);
    if (letrec_local_closure(b, e)) {
        uint32_t saved_pap = b->n_pap;
        if (!pap_register_let(b, e)) {
            b->n_pap = saved_pap;
            return unsupported_form(b, e);
        }
        CTerm *r = cps_bind(b, e->as.let_.body, x, rest);
        r = cps_bind_let_init(b, e, 0, cvar_of_binding(e->as.let_.bindings[0].binding), r);
        b->n_pap = saved_pap;
        return r;
    }
    if (is_atomic(e)) {
        CTerm *t = new_term(b, CT_LETVAL);
        t->as.letval.x = x; t->as.letval.v = atom_of(e); t->as.letval.body = rest;
        return t;
    }
    if (is_delegatable_owning(e) || is_delegatable_struct(e) || is_delegatable_value(e))
        return build_letraw(b, e, x, rest);
    /* (cont? k) in bind position: bind the unconsumed-check to x. */
    if (e->kind == EX_CONT_PRED)
        return build_cont_pred(b, e, x, rest);
    /* (with-handler <literal> body) in bind position. */
    if (e->kind == EX_WITH_HANDLER)
        return build_with_handler(b, e, x, rest);
    switch (e->kind) {
        case EX_BUILTIN: {
            Pending p = {0};
            CAtom *args = arena_alloc(b->a, (e->as.builtin.n ? e->as.builtin.n : 1) * sizeof(CAtom));
            for (uint32_t i = 0; i < e->as.builtin.n; i++)
                args[i] = atomize(b, e->as.builtin.args[i], &p);
            CTerm *t = new_term(b, CT_LETPRIM);
            t->as.letprim.x = x; t->as.letprim.op = builtin_name(e);
            t->as.letprim.spec = e->as.builtin.spec;
            t->as.letprim.args = args; t->as.letprim.n = e->as.builtin.n;
            t->as.letprim.body = rest;
            return fold_pending(b, &p, t);
        }
        case EX_CALL: {
            /* An empty-row call through a fat thread param, in BIND position:
             * the continuation `rest` reified as a heap join, threaded to the
             * value's entry via the registry (see cps_tail). */
            if (!call_is_effectful_fnvalue(e) && call_through_fat_param(e)
                && cps_ir_thread_param_has(e->as.call_.fn_binding) && call_args_pendable(e)) {
                Pending pp = {0};
                const Binding *pf = e->as.call_.fn_binding;
                uint32_t n = e->as.call_.n_args;
                CAtom *args = arena_alloc(b->a, (n ? n : 1) * sizeof(CAtom));
                for (uint32_t i = 0; i < n; i++)
                    args[i] = atomize_call_arg(b, e, i, &pp);
                CVar j = fresh_cvar(b, x.type);
                j.name = arena_strdup(b->a, "j", 1);
                CTerm *call = new_term(b, CT_TAILCALL);
                call->as.tailcall.fn = pf; call->as.tailcall.args = args;
                call->as.tailcall.n = n; call->as.tailcall.kont = kont_var(j);
                call->as.tailcall.via_registry = true;
                call->as.tailcall.call_expr = e;
                CTerm *t = new_term(b, CT_LETCONT);
                t->as.letcont.j = j; t->as.letcont.param = x;
                t->as.letcont.jbody = rest; t->as.letcont.body = call;
                return fold_pending(b, &pp, t);
            }
            /* E2/taint-completeness: evict an effectful fn-value call (see cps_tail). */
            if (call_is_effectful_fnvalue(e)) {
                /* E2a tier-`nontail`: a thread-param call in BIND position threads the
                 * DK by reifying the continuation `rest` as a heap join `j(x)` and
                 * threading it to the fn-value's __cps (via the registry).  Same shape
                 * as a colored-callee non-tail call (below), but via_registry. */
                const Binding *pf = e->as.call_.fn_binding;
                if (pf && cps_ir_thread_param_has(pf) && call_args_pendable(e)) {
                    Pending pp = {0};
                    uint32_t n = e->as.call_.n_args;
                    CAtom *args = arena_alloc(b->a, (n ? n : 1) * sizeof(CAtom));
                    for (uint32_t i = 0; i < n; i++)
                        args[i] = atomize_call_arg(b, e, i, &pp);
                    CVar j = fresh_cvar(b, x.type);
                    j.name = arena_strdup(b->a, "j", 1);
                    CTerm *call = new_term(b, CT_TAILCALL);
                    call->as.tailcall.fn = pf; call->as.tailcall.args = args;
                    call->as.tailcall.n = n; call->as.tailcall.kont = kont_var(j);
                    call->as.tailcall.via_registry = true;
                    CTerm *t = new_term(b, CT_LETCONT);
                    t->as.letcont.j = j; t->as.letcont.param = x;
                    t->as.letcont.jbody = rest; t->as.letcont.body = call;
                    return fold_pending(b, &pp, t);
                }
                /* E2c (bind position): an effectful struct-field fn-value call
                 * `(.f obj args)` -- reify `rest` as a heap join and thread it to
                 * the field-load callee via the registry (fn_atom key). */
                if (!pf && e->as.call_.fn_expr
                    && e->as.call_.fn_expr->kind == EX_GET_FIELD
                    && call_args_pendable(e)) {
                    Pending pp = {0};
                    CAtom fnatom = atomize(b, e->as.call_.fn_expr, &pp);
                    uint32_t n = e->as.call_.n_args;
                    CAtom *args = arena_alloc(b->a, (n ? n : 1) * sizeof(CAtom));
                    for (uint32_t i = 0; i < n; i++)
                        args[i] = atomize_call_arg(b, e, i, &pp);
                    CVar j = fresh_cvar(b, x.type);
                    j.name = arena_strdup(b->a, "j", 1);
                    CTerm *call = new_term(b, CT_TAILCALL);
                    call->as.tailcall.fn = NULL; call->as.tailcall.fn_atom = fnatom;
                    call->as.tailcall.args = args; call->as.tailcall.n = n;
                    call->as.tailcall.kont = kont_var(j);
                    call->as.tailcall.via_registry = true;
                    CTerm *t = new_term(b, CT_LETCONT);
                    t->as.letcont.j = j; t->as.letcont.param = x;
                    t->as.letcont.jbody = rest; t->as.letcont.body = call;
                    return fold_pending(b, &pp, t);
                }
                CTerm *t = new_term(b, CT_UNSUPPORTED);
                t->as.unsupported.why = "effectful fn-value call (E2 pending)";
                return t;
            }
            const Binding *fn = e->as.call_.fn_binding;
            if (!fn) {
                /* Indirect call: the fn value is the callee's direct entry, which
                 * never joins the caller's delimited-control chain -- delegate to
                 * the direct emitter with atomic args (see cps_tail). */
                if (!indirect_callee_ok(e->as.call_.fn_expr)) {
                    CTerm *t = new_term(b, CT_UNSUPPORTED);
                    t->as.unsupported.why = "indirect call through capturing closure";
                    return t;
                }
                if (!call_args_literal(e)) {
                    CTerm *d = delegate_call_atomized(b, e, x, rest, false);
                    if (d) return d;
                    CTerm *t = new_term(b, CT_UNSUPPORTED);
                    t->as.unsupported.why = "indirect call (non-atomic args)";
                    return t;
                }
                return build_letraw(b, e, x, rest);
            }
            /* E2 (fat-closure fn-value threading), BIND position: a NON-tail call
             * through a fat-closure poly-fn param reifies the continuation `rest`
             * as a heap join `j(x)` and threads it to the closure's `fn_cps` slot
             * (or the direct `f.fn` path when NULL).  Mirrors the E2a via_registry
             * non-tail shape (int-register-class args). */
            if (fncps_param_call_ok(fn, e)) {
                Pending pp = {0};
                CAtom *fargs = fncps_atomize_args(b, e, &pp);
                CVar j = fresh_cvar(b, x.type);
                j.name = arena_strdup(b->a, "j", 1);
                CTerm *call = new_term(b, CT_TAILCALL);
                call->as.tailcall.fn = fn; call->as.tailcall.args = fargs;
                call->as.tailcall.n = e->as.call_.n_args; call->as.tailcall.kont = kont_var(j);
                call->as.tailcall.via_fncps = true;
                call->as.tailcall.call_expr = e;
                CTerm *t = new_term(b, CT_LETCONT);
                t->as.letcont.j = j; t->as.letcont.param = x;
                t->as.letcont.jbody = rest; t->as.letcont.body = call;
                return fold_pending(b, &pp, t);
            }
            /* cps->direct call to an uncolored callee with atomic args: delegate
             * to the direct emitter (monomorphized callee names). */
            if (!callee_colored(b, fn) && call_args_delegatable(b, e))
                return build_letraw(b, e, x, rest);
            /* all-any-fn-param-is-unusable: see the tail-position twin -- a fn
             * VALUE callee never takes the named CT_LETCALL arm. */
            if (!fn->source_fn_def && !callee_colored(b, fn)) {
                if (!call_args_literal(e)) {
                    CTerm *d = delegate_call_atomized(b, e, x, rest, false);
                    if (d) return d;
                    CTerm *t = new_term(b, CT_UNSUPPORTED);
                    t->as.unsupported.why = "indirect call (non-atomic args)";
                    return t;
                }
                return build_letraw(b, e, x, rest);
            }
            Pending p = {0};
            uint32_t n = e->as.call_.n_args;
            CAtom *args = arena_alloc(b->a, (n ? n : 1) * sizeof(CAtom));
            for (uint32_t i = 0; i < n; i++)
                args[i] = atomize_call_arg(b, e, i, &p);
            if (callee_colored(b, fn)) {
                CVar j = fresh_cvar(b, x.type);
                j.name = arena_strdup(b->a, "j", 1);
                CTerm *call = new_term(b, CT_TAILCALL);
                call->as.tailcall.fn = fn; call->as.tailcall.args = args;
                call->as.tailcall.n = n; call->as.tailcall.kont = kont_var(j);
                call->as.tailcall.call_expr = e;   /* RC2: retain for method re-resolution */
                CTerm *t = new_term(b, CT_LETCONT);
                t->as.letcont.j = j; t->as.letcont.param = x;
                t->as.letcont.jbody = rest; t->as.letcont.body = call;
                return fold_pending(b, &p, t);
            } else {
                CTerm *t = new_term(b, CT_LETCALL);
                t->as.letcall.x = x; t->as.letcall.fn = fn;
                t->as.letcall.args = args; t->as.letcall.n = n; t->as.letcall.body = rest;
                t->as.letcall.call_expr = e;   /* RC2: retain for method re-resolution */
                return fold_pending(b, &p, t);
            }
        }
        case EX_IF: {
            Pending p = {0};
            CAtom c = atomize(b, e->as.if_.cond, &p);
            CVar j = fresh_cvar(b, x.type);
            j.name = arena_strdup(b->a, "j", 1);
            CTerm *body = new_term(b, CT_IF);
            body->as.if_.cond = c;
            body->as.if_.then_ = cps_tail(b, e->as.if_.then_, kont_var(j));
            body->as.if_.else_ = e->as.if_.else_or_null
                ? cps_tail(b, e->as.if_.else_or_null, kont_var(j))
                : cps_tail_unit(b, kont_var(j));
            CTerm *t = new_term(b, CT_LETCONT);
            t->as.letcont.j = j; t->as.letcont.param = x;
            t->as.letcont.jbody = rest; t->as.letcont.body = body;
            return fold_pending(b, &p, t);
        }
        case EX_MATCH: {
            /* B4: bind-position DK-lowering -- a restricted heap-ADT ctor match
             * whose result flows into `rest` becomes a CT_MATCH wrapped in a join
             * point (CT_LETCONT), mirroring the EX_IF bind case.  A match outside
             * the shape falls through to the wholesale direct delegation. */
            if (match_dk_ok(e)) {
                Pending p = {0};
                CAtom scrut = atomize(b, e->as.match_.scrutinee, &p);
                CVar j = fresh_cvar(b, x.type);
                j.name = arena_strdup(b->a, "j", 1);
                CTerm *body = build_match_term(b, e, scrut, kont_var(j));
                CTerm *t = new_term(b, CT_LETCONT);
                t->as.letcont.j = j; t->as.letcont.param = x;
                t->as.letcont.jbody = rest; t->as.letcont.body = body;
                return fold_pending(b, &p, t);
            }
            if (safe_to_delegate(b, e)) return build_letraw(b, e, x, rest);
            if (match_direct_dispatch_ok(b, e)) {
                CVar j = fresh_cvar(b, x.type);
                j.name = arena_strdup(b->a, "j", 1);
                CTerm *body = build_direct_match_term(b, e, kont_var(j));
                CTerm *t = new_term(b, CT_LETCONT);
                t->as.letcont.j = j; t->as.letcont.param = x;
                t->as.letcont.jbody = rest; t->as.letcont.body = body;
                return t;
            }
            return unsupported_form(b, e);
        }
        case EX_LET: {
            uint32_t saved_pap = b->n_pap;
            if (!pap_register_let(b, e)) {
                b->n_pap = saved_pap;
                return unsupported_form(b, e);
            }
            CTerm *r = cps_bind(b, e->as.let_.body, x, rest);
            for (int i = (int)e->as.let_.n - 1; i >= 0; i--) {
                if (pap_binding_inlined(b, e, (uint32_t)i, saved_pap)) {
                    const Expr *init = ascribe_peel(e->as.let_.bindings[i].init);
                    if (init && init->kind == EX_LET)
                        for (int j = (int)init->as.let_.n - 1; j >= 0; j--)
                            r = cps_bind_let_init(b, init, (uint32_t)j,
                                        cvar_of_binding(init->as.let_.bindings[j].binding), r);
                    continue;
                }
                r = cps_bind_let_init(b, e, (uint32_t)i,
                             cvar_of_binding(e->as.let_.bindings[i].binding), r);
            }
            b->n_pap = saved_pap;
            return r;
        }
        case EX_DO: {
            uint32_t n = e->as.do_.n;
            if (n == 0) { return cps_bind(b, NULL, x, rest); }
            /* O1-b: hoist trailing owning-ref auto-drop defers (see cps_tail). */
            AutodropPlan pl;
            if (plan_autodrop(e, &pl)) {
                Expr **items = e->as.do_.items;
                uint32_t nr = pl.n_real, fc = pl.first_ctrl;
                CTerm *r;
                if (fc == nr) {
                    /* No barrier: value -> x, drop refs at scope exit, then rest. */
                    r = cps_emit_hoisted_drops(b, &pl, rest);
                    r = cps_bind(b, items[nr - 1], x, r);
                    for (int i = (int)nr - 2; i >= 0; i--) {
                        CVar discard = fresh_cvar(b, &items[i]->type);
                        r = cps_bind(b, items[i], discard, r);
                    }
                } else {
                    r = cps_bind(b, items[nr - 1], x, rest);
                    for (int i = (int)nr - 2; i >= (int)fc; i--) {
                        CVar discard = fresh_cvar(b, &items[i]->type);
                        r = cps_bind(b, items[i], discard, r);
                    }
                    r = cps_emit_hoisted_drops(b, &pl, r);
                    for (int i = (int)fc - 1; i >= 0; i--) {
                        CVar discard = fresh_cvar(b, &items[i]->type);
                        r = cps_bind(b, items[i], discard, r);
                    }
                }
                return r;
            }
            CTerm *r = cps_bind(b, e->as.do_.items[n - 1], x, rest);
            for (int i = (int)n - 2; i >= 0; i--) {
                CVar discard = fresh_cvar(b, &e->as.do_.items[i]->type);
                r = cps_bind(b, e->as.do_.items[i], discard, r);
            }
            return r;
        }
        case EX_RETURN:
            return cps_tail(b, e->as.return_.value, b->retk);
        case EX_RESET: {
            CTerm *t = new_term(b, CT_RESET);
            t->as.reset.x = x;
            t->as.reset.delim = cps_tail(b, e->as.reset_.body, kont_prompt(e->type.kind));
            t->as.reset.body = rest;
            return t;
        }
        case EX_SHIFT: {
            CVar k = fresh_cvar(b, &e->type);
            k.name = "k'";
            CTerm *t = new_term(b, CT_SHIFT);
            t->as.shift.k = k;
            t->as.shift.body = cps_shift_body(b, e);
            return t;
        }
        case EX_SHIFT0: {
            CVar k = fresh_cvar(b, &e->type);
            k.name = "k'";
            CTerm *t = new_term(b, CT_SHIFT);
            t->as.shift.k = k;
            t->as.shift.shift0 = true;
            t->as.shift.body = cps_shift_body_kf(b, e->as.shift0_.k_fn,
                                                 e->as.shift0_.body, &e->type);
            return t;
        }
        case EX_CLONEABLE_RESET: {
            /* U3 Shape 1 native, else fall back to the CT_LETRAW delegation. */
            CTerm *nat = build_cloneable(b, e, x, rest);
            return nat ? nat : build_letraw(b, e, x, rest);
        }
        case EX_SERIAL_RESET: {
            /* U4: native arithmetic serial context, else delegate (see cps_tail). */
            CTerm *nat = build_serial(b, e, x, rest);
            return nat ? nat : build_letraw(b, e, x, rest);
        }
        case EX_ASYNC: case EX_AWAIT:
            /* U5: delegate the self-contained async region (see cps_tail).  F3
             * (cps-async): a tail `await` lowers to a dk_shift (build_await),
             * except inside a handler case body -- there it delegates to the fiber
             * path (CT_LETRAW) that handle_case_ok admits (b->in_handler_case). */
            if (e->kind == EX_AWAIT && !b->in_handler_case) {
                Pending p = {0};
                CTerm *core = build_await(b, e, x, rest, &p);
                return fold_pending(b, &p, core);
            }
            return build_letraw(b, e, x, rest);
        case EX_HANDLE:
            return build_handle(b, e, x, rest);
        case EX_PERFORM: {
            Pending p = {0};
            CTerm *core = build_perform(b, e, x, rest, &p);
            return fold_pending(b, &p, core);
        }
        case EX_RESUME: {
            Pending p = {0};
            CTerm *core = build_resume(b, e, x, rest, &p);
            return fold_pending(b, &p, core);
        }
        case EX_CALLCC: {
            /* U7: native local-escape landing for a capture-free receiver, else
             * fall back to the CT_LETRAW delegation (see cps_tail). */
            CTerm *nat = build_callcc(b, e, x, rest);
            return nat ? nat : build_letraw(b, e, x, rest);
        }
        case EX_WHILE: {
            /* cps-while-native: a control-op-bearing while (a control-free while is
             * already delegated by safe_to_delegate) lowers to a synthesized
             * recursive __cps loop.  Unconditional since cps-tramp-resume
             * graduated (2026-07-19); NULL -> evict as before. */
            {
                CTerm *loop = build_loop(b, e, x, rest);
                if (loop) return loop;
            }
            /* Not lowered natively (outside the guarded subset): preserve the
             * prior default -- delegate a control-free while to the direct
             * emitter, else evict. */
            if (safe_to_delegate(b, e)) return build_letraw(b, e, x, rest);
            return unsupported_form(b, e);
        }
        case EX_SET: {
            /* cps-while-native: a set! of a loop-carried var binds its pre-created
             * $next CVar; the unit result binds x and continues.  Any other set!
             * is unsupported (evicts). */
            int idx = loop_var_index(b, e->as.set_.target);
            if (b->n_loop && idx >= 0) {
                CTerm *lv = new_term(b, CT_LETVAL);
                CAtom u; memset(&u, 0, sizeof u); u.kind = CA_UNIT; u.ty = TY_NIL;
                lv->as.letval.x = x; lv->as.letval.v = u; lv->as.letval.body = rest;
                return cps_bind(b, e->as.set_.value, b->loop_nexts[idx], lv);
            }
            /* Not a loop-carried set!: preserve the prior default (delegate a
             * control-free set! to the direct emitter, else evict). */
            if (safe_to_delegate(b, e)) return build_letraw(b, e, x, rest);
            return unsupported_form(b, e);
        }
        case EX_CLOSURE: {
            /* B8 slice-3: delegate a capturing closure, reaping its env at the
             * entry boundary only when the slot consuming it keeps nothing of
             * it (fold_pending names that expression in reap_closure_expr).
             * Reaping unconditionally freed the env of a lambda a callee had
             * stored -- `(keep1 (fn [x] (+ x n)) 3)` with keep1 doing
             * `(set! saved f)` -- so a later `(saved 10)` jumped through freed
             * memory.  Anywhere else the env is left to leak, which is safe. */
            const struct Closure *cl = e->as.closure_.closure;
            if (cl && cl->n_captures > 0
                && !cl->is_shift_receiver && !cl->is_effect_payload) {
                bool reap = b->reap_closure_expr &&
                            ascribe_peel(b->reap_closure_expr) == e;
                CTerm *t = build_letraw(b, e, x, rest);
                t->as.letraw.reap_env = reap;
                return t;
            }
            if (safe_to_delegate(b, e)) return build_letraw(b, e, x, rest);
            return unsupported_form(b, e);
        }
        default: {
            /* N6.1: delegate a control-op-free, colored-call-free form to the
             * direct emitter (binds x, continues rest). */
            if (safe_to_delegate(b, e))
                return build_letraw(b, e, x, rest);
            /* cps-evicts-handle-in-operand-positions: a reinterpret whose
             * OPERAND carries a control op (a generic call with a `handle` in
             * its arguments, or a colored generic's erased result).  Translate
             * the operand into a binder of its own type, then retype it into x
             * through a delegated reinterpret of that binder -- the direct
             * emitter spells the retype, including an int64 carrier read back
             * as the double it holds (the float instantiation; until
             * 2026-10-09 only a same-size Tier A retype took this path). */
            if (e->kind == EX_REINTERPRET && e->as.reinterpret_.expr)
                return cps_bind_reinterp(b, e, x, rest);
            if (e->kind == EX_CAST && e->as.cast_.expr)
                return cps_bind_cast(b, e, x, rest);
            return unsupported_form(b, e);
        }
    }
}

/* Whole-body delegation probe: is the ENTIRE function body a control-op-free,
 * colored-call-free composite that -- with capturing closures admitted as
 * delegatable values -- the direct emitter can emit wholesale?  Such a function
 * is "colored" only because it constructs a capturing closure (a may-capture, not
 * an effectful, function): it threads no continuation and calls nothing that
 * does, so handing its whole body to the direct emitter is sound AND leak-clean
 * (emit_value(EX_LET) frees the closure env via let_binding_env_freeable).  This
 * is the Phase-1 closure KEYSTONE for the control-free colour-only shape: instead
 * of leaf-admitting the closure (which leaks the env on the CPS path), the whole
 * body is one CT_LETRAW so the direct emitter's scoped-env free applies.  A
 * function that performs / handles / shifts, or calls a colored callee, is NOT
 * whole-body delegatable (it genuinely needs the DK machine) and stays on the
 * per-node path. */
static bool whole_body_delegatable(CpsB *b, const Expr *body) {
    bool saved = g_whole_body_delegate;
    g_whole_body_delegate = true;
    bool ok = safe_to_delegate(b, body);
    g_whole_body_delegate = saved;
    return ok;
}

/* ---- public: translate a function ------------------------------------ */

CTerm *cps_ir_translate_fn(Arena *a, Expr *program, FnDef *fd) {
    if (!fd || !fd->body) return NULL;
    /* Zero the whole builder first.  `loop_depth` (cps-while-native nesting)
     * was added without an initializer here, so whether a `while` lowered
     * natively depended on stack garbage: the same fixture passed under
     * `tur check` and was evicted ("unsupported form: EX_WHILE") under
     * `tur build`, and any unrelated stdlib addition flipped it. */
    CpsB b;
    memset(&b, 0, sizeof b);
    b.a = a; b.program = program; b.counter = 0;
    b.retk.kind = KK_RET; b.retk.id = 0; b.retk.ty = fd->return_type.kind;
    b.cur_fn = fd->binding;
    b.cur_fd = fd;
    b.cur_fn_leaf_fiber = expr_has_indirect_fnvalue_call(fd->body, 0);
    b.n_pap = 0;
    b.n_loop = 0;   /* cps-while-native: not inside a loop body */
    b.rs_n = 0;     /* cps-while-native: read-after-set version map empty */
    b.in_handler_case = 0;  /* cps-async: not inside a handler-case body */
    /* Phase-1 keystone (control-free colour-only shape): a colored function whose
     * whole body is delegatable (no control op, no colored call -- colored only by
     * a capturing closure it builds) emits as a single CT_LETRAW, so the direct
     * emitter lowers the closure with its scoped-env free instead of the CPS path
     * leaf-admitting (and leaking) the closure.  The general per-node path handles
     * every function with a real control op / colored call. */
    CTerm *t;
    if (whole_body_delegatable(&b, fd->body)) {
        Expr *body = (Expr *)ascribe_peel(fd->body);
        if (is_atomic(body)) {
            t = cps_tail(&b, body, b.retk);
        } else {
            CVar x = fresh_cvar(&b, &body->type);
            CTerm *ac = new_term(&b, CT_APPCONT);
            ac->as.appcont.kont = b.retk; ac->as.appcont.v = atom_cvar(x);
            t = build_letraw(&b, body, x, ac);
        }
    } else {
        t = cps_tail(&b, fd->body, b.retk);
    }
    g_cps_last_fresh = b.counter;
    return t;
}

uint32_t cps_ir_last_fresh_count(void) { return g_cps_last_fresh; }

/* ---- printing --------------------------------------------------------- */

static void print_indent(FILE *out, int n) { for (int i = 0; i < n; i++) fputs("  ", out); }

static const char *kind_name(TypeKind k) {
    switch (k) {
        case TY_INT:   return "int";
        case TY_BOOL:  return "bool";
        case TY_FLOAT: return "float";
        case TY_NIL:   return "nil";
        case TY_CSTR:  return "cstr";
        default:       return "_";
    }
}

static void print_atom(const CAtom *a, FILE *out) {
    switch (a->kind) {
        case CA_VAR:  fprintf(out, "%s", a->var && a->var->name ? a->var->name->name : "_"); break;
        case CA_CVAR: fprintf(out, "%s", a->cvar_name ? a->cvar_name : "_"); break;
        case CA_INT:  fprintf(out, "%lld", (long long)a->i); break;
        case CA_BOOL: fprintf(out, "%s", a->b ? "true" : "false"); break;
        case CA_UNIT: fprintf(out, "()"); break;
        case CA_STR:  fprintf(out, "\"%.*s\"", (int)a->str.len, a->str.p ? a->str.p : ""); break;
        case CA_FLOAT: fprintf(out, "%g", a->f); break;
        default:      fprintf(out, "<val>"); break;
    }
}

static void print_kont(const CKont *k, FILE *out) {
    switch (k->kind) {
        case KK_RET:    fprintf(out, "k"); break;
        case KK_VAR:    fprintf(out, "j%u", k->id); break;
        case KK_PROMPT: fprintf(out, "<prompt>"); break;
        case KK_LOOP:   fprintf(out, "<loop>"); break;
    }
}

static void print_atoms(const CAtom *args, uint32_t n, FILE *out) {
    for (uint32_t i = 0; i < n; i++) { if (i) fputc(' ', out); print_atom(&args[i], out); }
}

void cps_ir_print(const CTerm *t, FILE *out, int indent) {
    if (!t) { print_indent(out, indent); fputs("<null>\n", out); return; }
    print_indent(out, indent);
    switch (t->kind) {
        case CT_APPCONT:
            fputc('(', out); print_kont(&t->as.appcont.kont, out); fputc(' ', out);
            print_atom(&t->as.appcont.v, out); fputs(")\n", out);
            break;
        case CT_LETVAL:
            fprintf(out, "let %s = ", t->as.letval.x.name);
            print_atom(&t->as.letval.v, out); fputs("\n", out);
            cps_ir_print(t->as.letval.body, out, indent);
            break;
        case CT_LETPRIM:
            fprintf(out, "let %s = (%s ", t->as.letprim.x.name, t->as.letprim.op);
            print_atoms(t->as.letprim.args, t->as.letprim.n, out); fputs(")\n", out);
            cps_ir_print(t->as.letprim.body, out, indent);
            break;
        case CT_LETCALL:
            fprintf(out, "let %s = call %s(", t->as.letcall.x.name,
                    t->as.letcall.fn && t->as.letcall.fn->name ? t->as.letcall.fn->name->name : "?");
            print_atoms(t->as.letcall.args, t->as.letcall.n, out);
            fputs(")  ; cps->direct\n", out);
            cps_ir_print(t->as.letcall.body, out, indent);
            break;
        case CT_TAILCALL:
            fprintf(out, "tailcall %s(",
                    t->as.tailcall.fn && t->as.tailcall.fn->name ? t->as.tailcall.fn->name->name : "?");
            print_atoms(t->as.tailcall.args, t->as.tailcall.n, out);
            if (t->as.tailcall.n) fputc(' ', out);
            print_kont(&t->as.tailcall.kont, out); fputs(")  ; cps->cps\n", out);
            break;
        case CT_LETCONT:
            fprintf(out, "letcont j%u(%s) =\n", t->as.letcont.j.id, t->as.letcont.param.name);
            cps_ir_print(t->as.letcont.jbody, out, indent + 1);
            print_indent(out, indent); fputs("in\n", out);
            cps_ir_print(t->as.letcont.body, out, indent);
            break;
        case CT_IF:
            fputs("if ", out); print_atom(&t->as.if_.cond, out); fputs("\n", out);
            print_indent(out, indent); fputs("then\n", out);
            cps_ir_print(t->as.if_.then_, out, indent + 1);
            print_indent(out, indent); fputs("else\n", out);
            cps_ir_print(t->as.if_.else_, out, indent + 1);
            break;
        case CT_MATCH:
            fputs("match ", out); print_atom(&t->as.match.scrut, out); fputs("\n", out);
            for (uint32_t ai = 0; ai < t->as.match.n_arms; ai++) {
                print_indent(out, indent);
                fprintf(out, "case %s ->\n",
                        t->as.match.arms[ai].ctor && t->as.match.arms[ai].ctor->name
                            ? t->as.match.arms[ai].ctor->name : "_");
                cps_ir_print(t->as.match.arms[ai].body, out, indent + 1);
            }
            break;
        case CT_RESET:
            fprintf(out, "reset %s {\n", t->as.reset.x.name);
            cps_ir_print(t->as.reset.delim, out, indent + 1);
            print_indent(out, indent); fputs("}\n", out);
            cps_ir_print(t->as.reset.body, out, indent);
            break;
        case CT_SHIFT:
            fprintf(out, "%s %s.\n", t->as.shift.shift0 ? "shift0" : "shift",
                    t->as.shift.k.name);
            cps_ir_print(t->as.shift.body, out, indent + 1);
            break;
        case CT_HANDLE:
            fprintf(out, "handle %s = {\n", t->as.handle.x.name);
            cps_ir_print(t->as.handle.delim, out, indent + 1);
            print_indent(out, indent); fputs("}\n", out);
            for (uint32_t ci = 0; ci < t->as.handle.n_cases; ci++) {
                const CHandleCase *c = &t->as.handle.cases[ci];
                print_indent(out, indent);
                fprintf(out, "with %s(", c->effect ? c->effect->name : "?");
                for (uint32_t i = 0; i < c->n_params; i++) {
                    if (i) fputc(' ', out);
                    fputs(c->params[i] && c->params[i]->name ? c->params[i]->name->name : "_", out);
                }
                fprintf(out, ") %s ->\n", c->k && c->k->name ? c->k->name->name : "k");
                cps_ir_print(c->case_body, out, indent + 1);
            }
            print_indent(out, indent); fputs("in\n", out);
            cps_ir_print(t->as.handle.body, out, indent);
            break;
        case CT_PERFORM:
            fprintf(out, "let %s = perform %s(", t->as.perform.x.name,
                    t->as.perform.effect ? t->as.perform.effect->name : "?");
            print_atoms(t->as.perform.args, t->as.perform.n, out); fputs(")\n", out);
            cps_ir_print(t->as.perform.body, out, indent);
            break;
        case CT_AWAIT:
            fprintf(out, "let %s = await(", t->as.await.x.name);
            print_atoms(&t->as.await.fut, 1, out); fputs(")\n", out);
            cps_ir_print(t->as.await.body, out, indent);
            break;
        case CT_RESUME:
            fprintf(out, "let %s = resume ", t->as.resume.x.name);
            print_atom(&t->as.resume.k, out); fputc(' ', out);
            print_atom(&t->as.resume.v, out); fputs("\n", out);
            cps_ir_print(t->as.resume.body, out, indent);
            break;
        case CT_LETRAW:
            fprintf(out, "let %s = <owning-op %s>  ; direct-emitted\n",
                    t->as.letraw.x.name,
                    t->as.letraw.e && owning_operand(t->as.letraw.e) ? "rc" : "?");
            cps_ir_print(t->as.letraw.body, out, indent);
            break;
        case CT_CLONEABLE:
            fprintf(out, "let %s = %s-cont -> %s(<cont>)  ; %s %s"
                         "[%u frame(s), %u let(s)%s]\n",
                    t->as.cloneable.x.name,
                    t->as.cloneable.serial ? "serial" : "cloneable",
                    t->as.cloneable.receiver_expr ? "<closure>"
                        : (t->as.cloneable.receiver && t->as.cloneable.receiver->name
                           ? t->as.cloneable.receiver->name->name : "?"),
                    t->as.cloneable.serial ? "U4" : "U3",
                    t->as.cloneable.n_frames == 0 ? "Shape 1 " : "Shape 2 ",
                    t->as.cloneable.n_frames, t->as.cloneable.n_lets,
                    t->as.cloneable.if_cond ? ", if" : "");
            cps_ir_print(t->as.cloneable.body, out, indent);
            break;
        case CT_CALLCC:
            fprintf(out, "let %s = call/cc(<recv>)  ; U7 native escape landing\n",
                    t->as.callcc.x.name);
            cps_ir_print(t->as.callcc.body, out, indent);
            break;
        case CT_LOOP:
            fputs("loop (", out);
            for (uint32_t i = 0; i < t->as.loop.n_params; i++) {
                if (i) fputc(' ', out);
                fprintf(out, "%s=", t->as.loop.params[i].name);
                print_atom(&t->as.loop.inits[i], out);
            }
            fputs(") ->\n", out);
            cps_ir_print(t->as.loop.body, out, indent + 1);
            break;
        case CT_CONTINUE:
            fputs("continue(", out);
            print_atoms(t->as.cont_.args, t->as.cont_.n, out);
            fputs(")\n", out);
            break;
        case CT_UNSUPPORTED:
            fprintf(out, "<unsupported: %s>\n", t->as.unsupported.why ? t->as.unsupported.why : "?");
            break;
    }
}

/* ---- CPS3: direct<->CPS boundary classification ----------------------- */

/* Does e's subtree resolved-call `target`? (no descent into nested fns). */
static bool body_calls_binding(const Expr *e, const Binding *target) {
    if (!e) return false;
    switch (e->kind) {
        case EX_CALL:
            if (e->as.call_.fn_binding == target) return true;
            if (body_calls_binding(e->as.call_.fn_expr, target)) return true;
            for (uint32_t i = 0; i < e->as.call_.n_args; i++)
                if (body_calls_binding(e->as.call_.args[i], target)) return true;
            return false;
        case EX_LET:
            for (uint32_t i = 0; i < e->as.let_.n; i++)
                if (body_calls_binding(e->as.let_.bindings[i].init, target)) return true;
            return body_calls_binding(e->as.let_.body, target);
        case EX_IF:
            return body_calls_binding(e->as.if_.cond, target)
                || body_calls_binding(e->as.if_.then_, target)
                || body_calls_binding(e->as.if_.else_or_null, target);
        case EX_DO:
            for (uint32_t i = 0; i < e->as.do_.n; i++)
                if (body_calls_binding(e->as.do_.items[i], target)) return true;
            return false;
        case EX_WHILE:
            return body_calls_binding(e->as.while_.cond, target)
                || body_calls_binding(e->as.while_.body, target);
        case EX_SET:    return body_calls_binding(e->as.set_.value, target);
        case EX_DEF:    return body_calls_binding(e->as.def_.init, target);
        case EX_RETURN: return body_calls_binding(e->as.return_.value, target);
        case EX_DEFER:  return body_calls_binding(e->as.defer_.body, target);
        case EX_BUILTIN:
            for (uint32_t i = 0; i < e->as.builtin.n; i++)
                if (body_calls_binding(e->as.builtin.args[i], target)) return true;
            return false;
        case EX_RESET:  return body_calls_binding(e->as.reset_.body, target);
        case EX_SHIFT:
            return body_calls_binding(e->as.shift_.k_fn, target)
                || body_calls_binding(e->as.shift_.body, target);
        case EX_SHIFT0:
            return body_calls_binding(e->as.shift0_.k_fn, target)
                || body_calls_binding(e->as.shift0_.body, target);
        /* nested fn definitions are boundaries */
        case EX_FN_DEF: case EX_FN: case EX_CLOSURE:
            return false;
        default:
            return false;
    }
}

/* A colored function is a direct->CPS *entry* (needs a driver to supply the
 * initial continuation) if no colored function resolved-calls it. */
static bool is_cps_entry(Expr *program, FnDef *fd) {
    for (uint32_t i = 0; i < program->as.program.n; i++) {
        Expr *it = program->as.program.items[i];
        if (!it || it->kind != EX_FN_DEF || !it->as.fn_def_.fn) continue;
        FnDef *g = it->as.fn_def_.fn;
        if (g == fd || !g->cps_colored) continue;
        if (body_calls_binding(g->body, fd->binding)) return false;
    }
    return true;
}

/* ---- public: dump all colored functions ------------------------------ */

void cps_ir_dump_program(Arena *a, Expr *program, FILE *out) {
    if (!program || program->kind != EX_PROGRAM || !out) return;
    cps_color_program(a, program);
    for (uint32_t i = 0; i < program->as.program.n; i++) {
        Expr *it = program->as.program.items[i];
        if (!it || it->kind != EX_FN_DEF || !it->as.fn_def_.fn) continue;
        FnDef *fd = it->as.fn_def_.fn;
        if (!fd->cps_colored) continue;
        if (!fd->binding || !fd->binding->name) continue;
        if (fd->binding->defining_module_name) continue;
        fprintf(out, "cps-fn %s [", fd->binding->name->name);
        for (uint32_t pi = 0; pi < fd->n_params; pi++) {
            if (pi) fputc(' ', out);
            fprintf(out, "%s", fd->params[pi]->name ? fd->params[pi]->name->name : "_");
        }
        fprintf(out, "] k:cont<%s> %s\n", kind_name(fd->return_type.kind),
                is_cps_entry(program, fd) ? "entry" : "internal");
        CTerm *t = cps_ir_translate_fn(a, program, fd);
        cps_ir_print(t, out, 1);
        fputs("cps-end\n", out);
    }
}

/* Public: does this call through the fn-value PARAM `p` thread the caller's DK
 * continuation into the callee, so an effectful value's `perform` reaches the
 * caller's handler?  Mirrors the EX_CALL lowering above: an effectful-row call
 * threads via the registry when `p` is a thread param (otherwise it is
 * unsupported, and the E2 taint routes it); an empty-row call threads only
 * through the fat value's `fn_cps` slot.  Any other call through `p` is a plain
 * direct call, which an effectful value escapes from. */
bool cps_ir_param_call_threads(const Binding *p, const Expr *call) {
    if (!p || !call || call->kind != EX_CALL || call->as.call_.fn_binding != p)
        return false;
    if (call_is_effectful_fnvalue(call))
        return cps_ir_thread_param_has(p) && call_args_pendable(call);
    /* An empty-row call through a fat thread param threads too (the dispatch
     * calls an unregistered value directly). */
    if (call_through_fat_param(call) && cps_ir_thread_param_has(p) && call_args_pendable(call))
        return true;
    return fncps_param_call_ok(p, call);
}
