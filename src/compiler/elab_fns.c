/* elab_fns.c -- function definition forms: defn, fn, extern-c, def. */
#include <stdarg.h>
#include "elab_internal.h"
#include "lang_dialects.h"   /* saffron-lang-plan S2: lang_span_is_dynamic */
#include "cps.h"          /* cps_expr_uses_control -- the control-cast hoist */
#include "refine_discharge.h"   /* RT3: decide a refinement obligation in place */
#include "refine_solver.h"      /* RT1: refine_model_search, for the W0377 witness */
#include "globals.h"            /* repr-trace: g_emit_abi_trace; G1: g_dump_write_frames */
#include "runtime/experiments.h" /* reflected-measures RF0: experiment_warn_if_used */

/* closure-drop-glue S1c: the closure-escape analysis (defined emit-side in
 * emit_core.c) is a pure walk of the shared Expr tree, reused here to infer
 * per-param non-retention when a defn is elaborated.  Forward-declared to avoid
 * pulling the whole emit-internal surface into the elaborator. */
bool closure_binding_escapes(const Expr *e, const Binding *b);
bool expr_subtree_has_inline_c(const Expr *e);
/* dynamic-returned-closure-env-is-never-freed: the `any` walk with the
 * self-application slots admitted, defined emit-side. */
bool any_box_binding_escapes_self_apply(const Expr *e, const Binding *b,
                                        uint32_t self_mask);
bool expr_is_fresh_any_closure(const Expr *x);
/* catch-box-reader-confinement-whitelist: the box-confinement walk, likewise
 * defined emit-side, reused here to infer per-param non-retention for
 * pointer-carrying scalars when a defn is elaborated. */
/* any-struct-box-leak-per-widen: the escape walk with the tail return excluded,
 * defined emit-side; reused here to check that a passthrough function does not
 * ALSO retain the argument it returns. */
bool catch_box_binding_escapes_except(const Expr *e, const Binding *b,
                                      const Expr *ignore);

/* RM1 (reclamation-plan): the freshness analysis, on the elaborated body.
 * True iff every VALUE PATH ends in a sum-constructor application or a call
 * to a binding already proven fresh.  Callees elaborate before callers within
 * a load order (stdlib first), so `some`/`ok`/`err`/`none` -- whose bodies are
 * bare ctor applications -- are stamped before the instance bodies that call
 * them; a self-recursive body reads its own flag before it is set and fails
 * conservatively.  Guarded match fall-through yields the zero-initialized
 * merge temp, i.e. the NULL carrier, which the null-guarded free ignores --
 * so guards need no special case.  Anything unrecognized is NOT fresh: the
 * polarity is that a wrong `false` leaks (status quo) and a wrong `true`
 * frees a live box, so every default answers false. */
/* The freshness walk.  `params`/`n_params` name the enclosing function's
 * parameters so a call THROUGH one of them (`(k v)`) can count as fresh
 * contingent on that parameter -- recorded in *need -- rather than as not
 * fresh.  NULL params is the unconditional question. */
static bool fresh_sum_walk(const Expr *e, Binding **params, uint32_t n_params,
                           uint32_t *need) {
    static int depth = 0;
    if (!e || depth > 64) return false;
    bool r = false;
    depth++;
    switch (e->kind) {
        case EX_ASCRIBE:
            r = fresh_sum_walk(e->as.ascribe_.inner, params, n_params, need);
            break;
        case EX_DO:
            r = e->as.do_.n > 0 &&
                fresh_sum_walk(e->as.do_.items[e->as.do_.n - 1], params, n_params, need);
            break;
        case EX_LET:
            r = fresh_sum_walk(e->as.let_.body, params, n_params, need);
            break;
        case EX_IF:
            r = e->as.if_.else_or_null &&
                fresh_sum_walk(e->as.if_.then_, params, n_params, need) &&
                fresh_sum_walk(e->as.if_.else_or_null, params, n_params, need);
            break;
        case EX_MATCH:
            if (e->as.match_.n_arms == 0) break;
            r = true;
            for (uint32_t i = 0; r && i < e->as.match_.n_arms; i++)
                r = fresh_sum_walk(e->as.match_.arms[i].body, params, n_params, need);
            break;
        /* residual-leaks (2026-09-02): a catch always mints its Result box --
         * tur_catch_unwind_box returns tur_box_ok / tur_box_err of a fresh
         * allocation on both paths, and an aggregate thunk's payload is a
         * fresh __tur_catchbox_* allocation whose pointer is the Ok word, the
         * same layout as a boxed value-struct arm.  So a defn whose body is a
         * catch (`(defn struct-ok [] : (Result Q int) (catch-unwind ...))`) is
         * a fresh producer, and its result passed to a non-retaining reader
         * gets the same drop a ctor's would. */
        case EX_CATCH_UNWIND:
        case EX_CATCH_PANIC_OF:
            r = true;
            break;
        case EX_CALL: {
            if (params && need) {
                const Expr *fe = e->as.call_.fn_expr;
                while (fe && fe->kind == EX_ASCRIBE) fe = fe->as.ascribe_.inner;
                for (uint32_t i = 0; i < n_params && i < 32; i++) {
                    const Binding *pb = params[i];
                    if (!pb) continue;
                    if (e->as.call_.fn_binding == pb ||
                        (fe && fe->kind == EX_VAR && fe->as.var.binding == pb)) {
                        *need |= (1u << i);
                        r = true;
                        break;
                    }
                }
                if (r) break;
            }
            r = call_returns_fresh_sum_box(e);
            /* generic-wrapper-tail-forwarding-a-return-dispatch-method: a
             * TYPECLASS-METHOD call is not statically dispatched here (the
             * instance is chosen per monomorph), so the walk above answers
             * "not fresh" and a wrapper whose tail forwards such a method
             * hands its caller an aggregate whose boxed payload nothing drops.
             *
             * Decide it by the same contract the inline-C rule below uses, and
             * for the same reason: a method DECLARED `: (Result a e)` /
             * `: (Option a)` is the MINTING shape -- an instance's only way to
             * produce one is tur_box_* / tur_some_ptr, which malloc -- while a
             * borrowed box wants a borrow-shaped `: a` signature, a bare tyvar
             * that never reaches here.  That is not a new assumption: the emit
             * side already marks such a call's carrier OWNED and frees it in
             * the readback bridge (emit_expr.c's emit_owned_carrier_mark), so
             * this only stops the payload arm from being the one piece of the
             * same box nobody owns. */
            if (!r && e->as.call_.dict_arg) {
                const Binding *fb = e->as.call_.fn_binding;
                const Type *decl = (fb && fb->type.kind == TY_FN)
                    ? fb->type.as.fn.result_full_type : NULL;
                Type rt = decl ? *decl : e->type;
                AdtDef *rd = NULL;
                Type ra[16];
                uint8_t rn = 0;
                if (type_extract_adt_app(&rt, &rd, ra, &rn) && rd && rd->name &&
                    (strcmp(rd->name, "Option") == 0 ||
                     strcmp(rd->name, "Result") == 0))
                    r = true;
            }
            break;
        }
        default:
            break;
    }
    depth--;
    return r;
}

bool elab_body_returns_fresh_sum_box(const Expr *e) {
    return fresh_sum_walk(e, NULL, 0, NULL);
}

void elab_stamp_sum_freshness(Binding *b, Binding **params, uint32_t n_params,
                              const Expr *body) {
    if (!b) return;
    uint32_t need = 0;
    bool fresh = fresh_sum_walk(body, params, n_params, &need);
    /* inline-c-results-guide, "Who owns the box": a body that is inline C
     * and whose DECLARED result is an Option/Result application hands its
     * caller a FRESH carrier box -- that is the contract the guide states
     * (`tur_some_ptr` / `tur_box_*` malloc it; a borrowed box wants a
     * borrow-shaped `: A` signature, which is a tyvar and stays out).  The
     * walk cannot see into C, so it answered "not fresh" and every such
     * producer was consumed by the readback bridge's cell-only free, leaving
     * a value-struct payload the body boxed as a pointer (`tur_box_ok((int64_t)
     * malloc'd_struct)`) unowned.  Fresh by declaration lets the pending
     * drain free the arm through the cell like any Turmeric producer's. */
    if (!fresh && b->body_is_inline_c && b->type.kind == TY_FN &&
        b->type.as.fn.result_full_type) {
        Type rt = *b->type.as.fn.result_full_type;
        AdtDef *rd = NULL;
        Type ra[16];
        uint8_t rn = 0;
        if (type_extract_adt_app(&rt, &rd, ra, &rn) && rd && rd->name &&
            (strcmp(rd->name, "Option") == 0 || strcmp(rd->name, "Result") == 0)) {
            fresh = true;
            need = 0;
        }
    }
    b->returns_fresh_sum_box = fresh && need == 0;
    b->fresh_sum_via_param_mask = fresh ? need : 0;
}

/* Peel the wraps elab puts around a function-valued argument on its way into
 * a fn-typed / ^fat / poly parameter slot. */
static const Expr *peel_fn_arg_wraps(const Expr *a) {
    while (a) {
        if (a->kind == EX_ASCRIBE) a = a->as.ascribe_.inner;
        else if (a->kind == EX_FN_TO_FAT) a = a->as.fn_to_fat_.inner;
        else if (a->kind == EX_POLY_TO_FAT) a = a->as.poly_to_fat_.inner;
        else if (a->kind == EX_POLY_WRAP) a = a->as.poly_wrap_.inner;
        else break;
    }
    return a;
}

static bool type_head_is_concrete(const Type *t) {
    const Type *h = t;
    while (h && h->kind == TY_APP) h = h->as.app.fn;
    if (!h) return false;
    switch (h->kind) {
        case TY_ADT: return h->as.adt_.def != NULL;
        case TY_STRUCT: return true;
        case TY_NIL: case TY_INT: case TY_BOOL: case TY_FLOAT: case TY_CSTR:
        case TY_INT64: case TY_UINT64: case TY_INT32: case TY_UINT32:
        case TY_INT16: case TY_UINT16: case TY_INT8: case TY_UINT8:
        case TY_FLOAT64: case TY_FLOAT32:
            return true;
        default: return false;
    }
}

bool call_dispatch_is_static(const Expr *call) {
    if (!call || call->kind != EX_CALL) return false;
    if (!call->as.call_.dict_arg) return true;
    /* A return-dispatched method (`pure`) keeps the abstract class variable
     * as the call's type; a concrete instance call's type is the instance
     * head applied (or a NULL-headed app, which decides nothing). */
    {
        const Type *h = &call->type;
        while (h && h->kind == TY_APP) h = h->as.app.fn;
        if (h && h->kind != TY_ADT && h->kind != TY_STRUCT &&
            !type_head_is_concrete(h))
            return false;
    }
    if (call->as.call_.n_args == 0 || !call->as.call_.args[0]) return false;
    const Expr *recv = call->as.call_.args[0];
    while (recv && recv->kind == EX_ASCRIBE) recv = recv->as.ascribe_.inner;
    return recv && type_head_is_concrete(&recv->type);
}

bool call_returns_fresh_sum_box_as(const Expr *call, const Binding *fb) {
    if (!call || call->kind != EX_CALL) return false;
    if (call->as.call_.ctor) return true;
    if (!fb) return false;
    if (fb->returns_fresh_sum_box) return true;
    uint32_t m = fb->fresh_sum_via_param_mask;
    if (!m) return false;
    for (uint32_t i = 0; i < 32; i++) {
        if (!(m & (1u << i))) continue;
        if (i >= call->as.call_.n_args) return false;
        const Expr *a = peel_fn_arg_wraps(call->as.call_.args[i]);
        if (!a) return false;
        if (a->kind == EX_CLOSURE) {
            const struct Closure *c = a->as.closure_.closure;
            bool cf = c && c->fn && elab_body_returns_fresh_sum_box(c->fn->body);
            if (!cf) return false;
        } else if (a->kind == EX_VAR) {
            /* A defn so flagged, or a let-bound closure (the hoisted-borrow
             * shape) whose anonymous fn binding is. */
            const Binding *ab = a->as.var.binding;
            bool vf = ab && (ab->returns_fresh_sum_box ||
                             (ab->closure_fn_binding &&
                              ab->closure_fn_binding->returns_fresh_sum_box) ||
                             (ab->hoist_closure_fn_binding &&
                              ab->hoist_closure_fn_binding->returns_fresh_sum_box));
            if (!vf) return false;
        } else {
            return false;
        }
    }
    return true;
}

bool call_returns_fresh_sum_box(const Expr *call) {
    if (!call || call->kind != EX_CALL) return false;
    /* phantom-parametric-heap-let-binding-repr-ice (second defect): a
     * transparent int newtype's constructor is an identity -- `(Holder 7)` IS
     * 7, no box is minted -- so it is not a fresh sum box, and the RM1 scope
     * drop freed the payload integer as a pointer (a segfault at scope end). */
    if (call->as.call_.ctor)
        return !type_is_transparent_int_newtype(call->type);
    if (!call_dispatch_is_static(call)) return false;
    return call_returns_fresh_sum_box_as(call, call->as.call_.fn_binding);
}


bool ptr_param_is_nonretaining(const Expr *body, const Binding *p,
                               bool result_cannot_carry);
bool sum_param_is_nonretaining(const Expr *body, const Binding *p,
                               bool result_cannot_carry);
bool localowned_param_is_nonretaining(const Expr *body, const Binding *p);
bool localowned_binding_is_confined(const Expr *body, const Binding *b,
                                    bool result_cannot_carry);

/* closure-capture-escapes-linearity: one enclosing linear/unique binding's
 * substructural state, recorded before a lambda body is elaborated so the body's
 * effect on it can be read off afterwards.  See the snapshot in elab_fn. */
typedef struct FnCaptureLinSnap {
    Binding *binding;
    bool     was_consumed;   /* is_linear_consumed before the body */
    bool     was_moved;      /* is_moved before the body */
} FnCaptureLinSnap;

/* closure-drop-glue S1c: a scalar Copy type kind -- safe to hold in / bare-free
 * around a closure env (no owning teardown, no aliasing of the env).  Excludes
 * rc/ref/weak (owning), structs/ADTs/tyvars (aggregate/opaque), and fn/cont. */
static bool fn_result_kind_is_scalar_copy(TypeKind k) {
    switch (k) {
        case TY_INT: case TY_BOOL: case TY_FLOAT: case TY_CSTR: case TY_NIL:
        case TY_PTR_VOID:
        case TY_INT8: case TY_INT16: case TY_INT32: case TY_INT64:
        case TY_UINT8: case TY_UINT16: case TY_UINT32: case TY_UINT64:
        case TY_FLOAT32: case TY_FLOAT64:
            return true;
        default:
            return false;
    }
}

/* ------------------------------------------------------------------------- *
 * RT1 (refinement-types-plan): obligation collection for a `defn`.
 *
 * Parameter refinements and `:pre` become HYPOTHESES; the return refinement
 * and `:post` become GOALS.  When a backend proves a goal, the runtime
 * contract check for it is not emitted at all -- that elision is the whole
 * point of the feature, and it is sound because the proof is over the same
 * body the check would have guarded.
 * ------------------------------------------------------------------------- */

/* Map a Turmeric type kind to the VC sort the solver reasons in.
 *
 * RM-B1: `:bool` denotes a PROPOSITION, not the integer 0/1.  Without this arm
 * a bool-returning function could not be used as a predicate atom at all --
 * `(alive? w x)` encoded Int-sorted and refine_vc_build dropped the whole
 * obligation as "does not denote a proposition". */
VCSort rt_sort_of_kind(TypeKind k) {
    switch (k) {
        case TY_FLOAT: case TY_FLOAT32: case TY_FLOAT64: return VS_REAL;
        case TY_BOOL: return VS_BOOL;
        default: return VS_INT;
    }
}

/* CT1: inject a contract entry check for each `{ v : T | pred }` parameter:
 *
 *     (tur-contract-check (let [v <param>] pred) "Contract violated")
 *
 * Shared by `defn` and `fn` so a lambda's contract parameters behave exactly
 * like a defn's rather than being silently decorative.  Returns the new body
 * (the original when nothing was injected).  Must run while the function's
 * inner scope is still current -- the predicate is elaborated in it. */
static bool rt_pred_observably_impure(const Expr *x);

/* C2 (#reads): does this predicate reference a `#reads`-annotated measure?
 * Defined after rt_resolve_fn; forward-declared here for rt_inject_param_checks. */
static bool rt_pred_reads_measure(Elab *e, const Form *f);

/* loop-invariants / C2: is every occurrence of `name` in `f` at a position
 * some callee promised not to write?  Defined with the other `li_*` walkers;
 * forward-declared here because li_register_site builds a loop's frozen set
 * with it, and registration is the only point where the loop's scope is live
 * to check an approved callee name against its global. */
static bool li_name_reads_only(Elab *e, const Form *f, const char *name,
                               uint32_t depth);

/* The typed half of the same question, over the ELABORATED loop: is every use
 * of the binding `b` in `x` a read?  Defined beside the `#reads` write walk it
 * reuses (rw_scan). */
static bool rw_binding_reads_only(const Expr *x, Binding *b);

/* CT1: a contract predicate whose EVALUATION does something observable.
 *
 * Checks are conditional on the build: `--no-contracts` strips them, a release
 * build drops them unless --keep-contracts, and static refinement discharge
 * elides the ones it can prove (unconditional since `refined` graduated in
 * v0.33.0).  A predicate with side effects therefore makes program
 * behaviour depend on whether its own contracts were compiled in --
 * `(>= (tick) 0)` advances a counter every time it is checked and not at all
 * when it is not.  That is a bug in the contract, not in any of those flags.
 *
 * Reported only on PROVEN impurity (`RT_P_IMPURE`), never on a predicate the
 * walk merely does not model, so a measure written with a `match` or a field
 * read is left alone.  That asymmetry is why the classifier is three-valued.
 *
 * Gate-independent on purpose: the predicate is equally wrong with the
 * refinement experiment off. */
static void rt_diag_impure_pred(Elab *e, const Expr *pred_e, Span span) {
    (void)e;
    if (!pred_e || !rt_pred_observably_impure(pred_e)) return;
    diag_emit_with_code(DIAG_ERROR, span, TUR_E0375_REFINE_EFFECTFUL,
        "contract predicate has side effects; predicates must be pure");
    diag_emit(DIAG_NOTE, span,
        "evaluating this predicate changes program state, so whether the "
        "check is compiled in becomes observable");
}

Expr *rt_inject_param_checks(Elab *e, Expr *body, Binding *check_fn,
                                    Binding **params, uint32_t n_params,
                                    const Form **ct_preds, const char **ct_vars,
                                    const uint32_t *ct_idx, uint32_t n_ct,
                                    Span span) {
    if (!check_fn || !body) return body;
    for (uint32_t ci = 0; ci < n_ct; ci++) {
        if (ct_preds[ci] == NULL) continue;
        uint32_t pi = ct_idx[ci];
        if (pi >= n_params || !params[pi]) continue;
        /* C2 (#reads): a param whose refinement is a `#reads`-annotated measure
         * is statically checked at its CROSSINGS, never at runtime.  Suppress the
         * entry-check injection here; the caller-side crossing obligation is the
         * enforcement point (proven -> elided; unknown -> reported, a warning or
         * under --strict-refine an error, as a proof-only obligation -- see
         * `reads_no_runtime` in refine_resolve_call_sites).  This is a choice of
         * enforcement point, not a purity verdict: the CT1 gate itself accepts a
         * `#reads` measure in every contract position (rt_pred_observably_impure),
         * and `:pre`, `:post`, a return refinement and `:invariant` keep their
         * runtime checks.  The grant's soundness is pinned by
         * tests/fixtures/errors/refine-stateful-*. */
        if (rt_pred_reads_measure(e, ct_preds[ci])) continue;
        const char *var_nm = ct_vars[ci];

        /* Bind the contract's own variable name as an alias for the parameter,
         * unless it already IS the parameter's name. */
        Binding *cv_b = NULL;
        if (var_nm) {
            StrSlice vnsl = strslice(var_nm, (uint32_t)strlen(var_nm));
            const Symbol *cv_sym = symtab_intern(e->st, vnsl);
            Binding *existing_cv = scope_lookup(e->scope, cv_sym);
            if (!existing_cv || existing_cv != params[pi]) {
                cv_b = binding_new(e, cv_sym, params[pi]->type, false, false, span);
                scope_add(e->scope, cv_b);
            }
        }
        Expr *pred_e = elab_form(e, (Form *)ct_preds[ci]);
        if (!pred_e) continue;
        rt_diag_impure_pred(e, pred_e, span);
        /* saffron-lang-plan D6: a refinement predicate over an `any` is a
         * dyn-op returning `any`, and `tur-contract-check` takes a `bool` --
         * so without this the emitted C was
         * `tur_contract_check(<tur_tagged_t>, ...)`, which does not compile.
         * D4's truthiness is the same answer `if` already uses for the same
         * question; the two now share one helper. */
        pred_e = elab_saffron_truthy(e, pred_e, span);

        Expr *check_expr = pred_e;
        if (cv_b) {
            Expr *param_var_e = expr_new(e->arena, EX_VAR, params[pi]->type, span);
            param_var_e->as.var.binding = params[pi];
            LetBinding *cv_lb = (LetBinding *)arena_alloc(e->arena, sizeof(LetBinding));
            cv_lb->binding = cv_b;
            cv_lb->init = param_var_e;
            Expr *let_cv = expr_new(e->arena, EX_LET, pred_e->type, span);
            let_cv->as.let_.bindings = cv_lb;
            let_cv->as.let_.n = 1;
            let_cv->as.let_.body = pred_e;
            check_expr = let_cv;
        }

        Expr **ck_args = (Expr **)arena_alloc(e->arena, 2 * sizeof(Expr *));
        ck_args[0] = check_expr;
        Expr *ck_msg = expr_new(e->arena, EX_CSTR_LIT, TYPE_CSTR, span);
        ck_msg->as.s.p = "Contract violated";
        ck_msg->as.s.len = 17;
        ck_args[1] = ck_msg;
        Expr *ck_call = expr_new(e->arena, EX_CALL, TYPE_NIL, span);
        ck_call->as.call_.fn_binding  = check_fn;
        ck_call->as.call_.args        = ck_args;
        ck_call->as.call_.n_args      = 2;
        ck_call->as.call_.fn_expr     = NULL;
        ck_call->as.call_.dict_arg    = NULL;
        ck_call->as.call_.is_poly_call = false;
        ck_call->as.call_.poly_arg_mask = 0;

        Expr **do2 = (Expr **)arena_alloc(e->arena, 2 * sizeof(Expr *));
        do2[0] = ck_call;
        do2[1] = body;
        Expr *new_b = expr_new(e->arena, EX_DO, body->type, span);
        new_b->as.do_.items = do2;
        new_b->as.do_.n = 2;
        body = new_b;
    }
    return body;
}

/* CT1: wrap `body` so its RESULT is checked against `pred`:
 *
 *   (let [<var> body] (tur-contract-check pred "<msg>") <var>)
 *
 * `var_name` is the name the predicate binds the result under -- the contract's
 * own variable for a `: #refine{ r : T | p }` return, or NULL for `:post`,
 * which uses `result`.  Returns `body` unchanged when there is nothing to do.
 *
 * Shared by `defn` and typeclass instance methods.  It lives in one place
 * because the sibling peel (rt_peel_contract) had to be reached independently
 * three times before it was centralised, and this is the same shape of trap:
 * a second hand-rolled copy is a second place for a refinement to be accepted
 * and silently not enforced. */
Expr *rt_wrap_return_check(Elab *e, Expr *body, Binding *check_fn,
                           const Form *pred, const char *var_name,
                           const char *fail_msg, Span span) {
    if (!check_fn || !body || !pred) return body;
    const Symbol *bind_sym = e->sym_result;
    if (var_name)
        bind_sym = symtab_intern(e->st, strslice(var_name, (uint32_t)strlen(var_name)));

    Binding *result_b = binding_new(e, bind_sym, body->type, false, false, span);
    scope_add(e->scope, result_b);
    Expr *pred_e = elab_form(e, (Form *)pred);
    if (!pred_e) return body;
    rt_diag_impure_pred(e, pred_e, span);
    pred_e = elab_saffron_truthy(e, pred_e, span);   /* D6, see rt_inject_param_checks */

    Expr **args = (Expr **)arena_alloc(e->arena, 2 * sizeof(Expr *));
    args[0] = pred_e;
    Expr *msg = expr_new(e->arena, EX_CSTR_LIT, TYPE_CSTR, span);
    msg->as.s.p   = fail_msg;
    msg->as.s.len = (uint32_t)strlen(fail_msg);
    args[1] = msg;
    Expr *check = expr_new(e->arena, EX_CALL, TYPE_NIL, span);
    check->as.call_.fn_binding   = check_fn;
    check->as.call_.args         = args;
    check->as.call_.n_args       = 2;
    check->as.call_.fn_expr      = NULL;
    check->as.call_.dict_arg     = NULL;
    check->as.call_.is_poly_call = false;
    check->as.call_.poly_arg_mask = 0;

    Expr *result_var = expr_new(e->arena, EX_VAR, body->type, span);
    result_var->as.var.binding = result_b;
    Expr **inner = (Expr **)arena_alloc(e->arena, 2 * sizeof(Expr *));
    inner[0] = check;
    inner[1] = result_var;
    Expr *inner_do = expr_new(e->arena, EX_DO, body->type, span);
    inner_do->as.do_.items = inner;
    inner_do->as.do_.n = 2;

    LetBinding *lb = (LetBinding *)arena_alloc(e->arena, sizeof(LetBinding));
    lb->binding = result_b;
    lb->init = body;
    Expr *let_e = expr_new(e->arena, EX_LET, body->type, span);
    let_e->as.let_.bindings = lb;
    let_e->as.let_.n = 1;
    let_e->as.let_.body = inner_do;
    return let_e;
}

/* An early `(return v)` leaves before the whole-body wrap above sees a value,
 * so `v` gets the same checks at the return itself (early-return-bypasses-
 * return-refinement).  The predicate is elaborated afresh per site, in a scope
 * of its own: the result binding must not leak into the code after the
 * `return`, where it would shadow a user name spelled the same. */
Expr *rt_check_returned_value(Elab *e, Expr *value, Span span) {
    const RetContract *rc = e ? e->ret_contract : NULL;
    if (!rc || !value) return value;
    Binding *check_fn = scope_lookup(&e->global, e->sym_tur_contract_check);
    if (!check_fn) return value;
    Scope s;
    scope_init(&s, e->scope);
    e->scope = &s;
    if (rc->post)
        value = rt_wrap_return_check(e, value, check_fn, rc->post, NULL,
                                     "Postcondition failed", span);
    if (rc->ret)
        value = rt_wrap_return_check(e, value, check_fn, rc->ret, rc->ret_var,
                                     "Return contract violated", span);
    if (rc->class_ret)
        value = rt_wrap_return_check(e, value, check_fn, rc->class_ret,
                                     rc->class_ret_var,
                                     "Class result contract violated", span);
    e->scope = s.parent;
    scope_free(&s);
    return value;
}

/* True when contract checks are being emitted for this build.  `--no-contracts`
 * strips them, and a release build drops them unless --keep-contracts. */
bool rt_contracts_emitted(void) {
#ifdef NDEBUG
    if (!g_keep_contracts_in_release) return false;
#endif
    return !g_no_contracts;
}

/* ------------------------------------------------------------------------- *
 * loop-invariants-plan, the elaboration side (LI0/LI1).
 *
 * A written `:invariant p` is a contract before it is anything else: it must
 * be a pure `bool` (the purity gate every contract predicate takes, TUR-E0375),
 * it is checked on entry and at the end of every iteration, and a proof only
 * ever REMOVES one of those checks.  The static half lives with the other
 * return-point machinery further down (li_analyze_loops).
 * ------------------------------------------------------------------------- */

Expr *elab_loop_invariant_pred(Elab *e, const Form *pred, Span span) {
    if (!pred) return NULL;
    Expr *pred_e = elab_form(e, (Form *)pred);
    if (!pred_e) return NULL;
    rt_diag_impure_pred(e, pred_e, span);
    if (rt_pred_observably_impure(pred_e)) return NULL;
    pred_e = elab_saffron_truthy(e, pred_e, span);   /* D6, as for :pre */
    if (!type_eq(pred_e->type, TYPE_BOOL)) {
        diag_emit(DIAG_ERROR, span,
                  "while: `:invariant` must be a bool predicate, got %s",
                  type_name(pred_e->type));
        return NULL;
    }
    return pred_e;
}

Expr *li_contract_check(Elab *e, Expr *pred_e, const char *msg, Span span) {
    if (!pred_e || !msg) return NULL;
    Binding *check_fn = scope_lookup(&e->global, e->sym_tur_contract_check);
    if (!check_fn) return NULL;
    Expr **args = (Expr **)arena_alloc(e->arena, 2 * sizeof(Expr *));
    args[0] = pred_e;
    Expr *m = expr_new(e->arena, EX_CSTR_LIT, TYPE_CSTR, span);
    m->as.s.p   = msg;
    m->as.s.len = (uint32_t)strlen(msg);
    args[1] = m;
    Expr *check = expr_new(e->arena, EX_CALL, TYPE_NIL, span);
    check->as.call_.fn_binding    = check_fn;
    check->as.call_.args          = args;
    check->as.call_.n_args        = 2;
    check->as.call_.fn_expr       = NULL;
    check->as.call_.dict_arg      = NULL;
    check->as.call_.is_poly_call  = false;
    check->as.call_.poly_arg_mask = 0;
    return check;
}

/* Free symbols of `f` a loop site cares about: every symbol, minus list heads
 * (a head is a function or special form, not a value the predicate reads). */
static void li_note_mutable_global(Elab *e, const Form *f, uint32_t depth,
                                   const char **out) {
    if (!f || *out || depth > 32) return;
    if (f->tag == F_SYM && f->as.sym) {
        Binding *b = scope_lookup(e->scope, f->as.sym);
        if (b && ((b->is_global && b->is_mut) || b->is_dynvar))
            *out = b->name ? b->name->name : f->as.sym->name;
        return;
    }
    if (f->tag != F_LIST && f->tag != F_VEC) return;
    for (uint32_t i = (f->tag == F_LIST ? 1 : 0); i < f->as.list.len; i++)
        li_note_mutable_global(e, f->as.list.items[i], depth + 1, out);
}

LoopInvSite *li_register_site(Elab *e, const Form *call, const Form *cond,
                              const Form *inv, uint32_t body_start, Span span,
                              const Expr *while_e) {
    if (!e || !call || !inv) return NULL;
    if (e->n_loop_inv_sites == e->cap_loop_inv_sites) {
        uint32_t ncap = e->cap_loop_inv_sites ? e->cap_loop_inv_sites * 2 : 8;
        LoopInvSite *nb = (LoopInvSite *)arena_alloc(e->arena,
                                                     ncap * sizeof(LoopInvSite));
        if (e->n_loop_inv_sites)
            memcpy(nb, e->loop_inv_sites, e->n_loop_inv_sites * sizeof(LoopInvSite));
        e->loop_inv_sites = nb;
        e->cap_loop_inv_sites = ncap;
    }
    LoopInvSite *site = &e->loop_inv_sites[e->n_loop_inv_sites++];
    memset(site, 0, sizeof(*site));
    site->while_form = call;
    site->cond       = cond;
    site->inv        = inv;
    site->body_start = body_start;
    site->span       = span;

    /* The sorts of every local in scope, innermost first.  Walks to the global
     * scope and no further: a global is not something the loop's facts may
     * mention (see the decline below). */
    uint32_t nl = 0;
    for (const Scope *sc = e->scope; sc && sc != &e->global; sc = sc->parent)
        nl += sc->n;
    if (nl) {
        site->local_names = (const char **)arena_alloc(e->arena, nl * sizeof(char *));
        site->local_sorts = (VCSort *)arena_alloc(e->arena, nl * sizeof(VCSort));
        for (const Scope *sc = e->scope; sc && sc != &e->global; sc = sc->parent) {
            for (uint32_t i = sc->n; i-- > 0; ) {
                const Binding *b = sc->bindings[i];
                if (!b || !b->name || b->is_global) continue;
                bool dup = false;
                for (uint32_t k = 0; k < site->n_locals && !dup; k++)
                    dup = strcmp(site->local_names[k], b->name->name) == 0;
                if (dup) continue;
                site->local_names[site->n_locals] = b->name->name;
                site->local_sorts[site->n_locals] = rt_sort_of_kind(b->type.kind);
                site->n_locals++;
            }
        }
    }

    /* A mutable global (or a dynamic var) is written by NAME, so a callee in the
     * body can change it with no `set!` for the body scan to see.  The
     * invariant or the condition reading one therefore has no sound
     * preservation argument here; decided now, while names still resolve. */
    const char *mg = NULL;
    li_note_mutable_global(e, inv, 0, &mg);
    if (!mg) li_note_mutable_global(e, cond, 0, &mg);
    if (mg) {
        char buf[160];
        snprintf(buf, sizeof(buf), "the loop reads the mutable global '%s', "
                 "which a call in the body could change", mg);
        site->decline = arena_strdup(e->arena, buf, strlen(buf));
    }

    /* C2 / #reads: the loop's FROZEN SET -- the shared borrows live here that
     * the loop's condition and body are promised not to write.  Decided here,
     * at registration, for two independent reasons: the borrow checker only
     * knows what is live at this point, and `li_name_reads_only`'s shadow
     * check needs the loop's scope to ask whether an approved callee name
     * still resolves to the binding its promise was read off.
     *
     * Two carve-outs on the candidates, both the crossing's
     * (refine_note_call_site) and for its reasons:
     *
     *   - a SHADOWED borrow is skipped.  The encoder matches a frozen name by
     *     spelling, so an inner binding of the same name would otherwise be
     *     read as frozen when it is not.
     *   - a MUTABLE GLOBAL is never frozen.  It is written by name rather than
     *     passed, so borrowing it does not stop a callee's `(set! *g* ...)`.
     *
     * And one the crossing does not make: only `BK_IMMUT`.  A `&mut` borrow IS
     * the write channel a frozen set exists to rule out.
     *
     * The invariant is not walked for write promises -- it is gated
     * non-observably-impure already, so evaluating it cannot write. */
    {
        uint32_t nb = 0;
        for (const Scope *sc = e->scope; sc; sc = sc->parent)
            for (const ScopeBorrow *bo = sc->borrows; bo; bo = bo->next)
                if (bo->kind == BK_IMMUT && bo->binding && bo->binding->name) nb++;
        if (nb) {
            const char **fz = (const char **)arena_alloc(e->arena,
                                                         nb * sizeof(char *));
            uint32_t k = 0;
            for (const Scope *sc = e->scope; sc; sc = sc->parent)
                for (const ScopeBorrow *bo = sc->borrows; bo; bo = bo->next) {
                    if (bo->kind != BK_IMMUT) continue;
                    Binding *b = bo->binding;
                    if (!b || !b->name) continue;
                    if (scope_lookup(e->scope, b->name) != b) continue;
                    if (b->is_global && b->is_mut) continue;
                    const char *nm = b->name->name;
                    if (!li_name_reads_only(e, cond, nm, 0)) continue;
                    bool ok = true;
                    for (uint32_t i = body_start; i < call->as.list.len && ok; i++)
                        ok = li_name_reads_only(e, call->as.list.items[i], nm, 0);
                    /* The Form walk cannot see what a call RETURNS, so it
                     * approved `(vec-push! (id v) 7)` -- `id` is pure, and a
                     * pure callee is safe in any position -- and proved a bound
                     * the push then broke, with the check elided.  The typed
                     * walk over the elaborated loop sees that `(id v)` is a
                     * non-scalar value flowing into a writer's slot.  Both must
                     * agree; the Form walk stays for the shadow check it makes
                     * by name. */
                    if (ok) ok = while_e && rw_binding_reads_only(while_e, b);
                    if (ok) fz[k++] = nm;
                }
            if (k) { site->frozen_names = fz; site->n_frozen = k; }
        }
    }
    return site;
}

/* ------------------------------------------------------------------------- *
 * RT4 purity -- a THREE-valued classification, because two questions are
 * asked of it and they want OPPOSITE conservatism.
 *
 * 1. CONGRUENCE: "may two occurrences of this call be modelled as the same
 *    value?"  Answering yes wrongly elides a real check -- `(- (tick) (tick))`
 *    with a counter-bumping `tick` encodes as `t - t`, the solver proves
 *    `t - t >= 0`, and a program that returns -1 ships unchecked.  This
 *    question needs PURE to mean proven-pure; everything else reads as impure.
 *
 * 2. DIAGNOSTICS: "is this predicate effectful, so that evaluating the check
 *    is itself part of the program's behaviour?"  Answering yes wrongly
 *    REJECTS WORKING CODE.  This question needs IMPURE to mean proven-impure;
 *    everything else must be left alone.
 *
 * These are not negations of each other, and collapsing them into one boolean
 * is exactly how a default-deny purity test turns into false errors on a
 * perfectly good measure whose body happens to use a form the walk has not
 * learned (a `match`, a field read).  The gap between them is RT_P_UNKNOWN,
 * and each caller reads it its own way.
 *
 * The DECLARED EFFECT ROW IS NOT EVIDENCE either way.  The effect system
 * tracks algebraic effects (`perform`/handlers); it infers nothing from
 * `set!`, from mutable globals, or from inline C -- effect_check.c says so
 * where it suppresses W0031 for `#fx{Unsafe}` bodies containing inline C.  A
 * function can declare `#fx{}`, carry a `static` counter in a C block, and
 * return a different value every call.
 * ------------------------------------------------------------------------- */

typedef enum RtPurity {
    RT_P_PURE = 0,   /* proven: computes a value, touches nothing        */
    RT_P_IMPURE,     /* proven: reaches a concrete effectful construct   */
    RT_P_UNKNOWN,    /* neither proven -- a form the walk does not model */
} RtPurity;

/* Combine sibling results.  IMPURE dominates (one proven effect is enough);
 * otherwise UNKNOWN dominates PURE. */
static inline RtPurity rt_p_join(RtPurity a, RtPurity b) {
    if (a == RT_P_IMPURE  || b == RT_P_IMPURE)  return RT_P_IMPURE;
    if (a == RT_P_UNKNOWN || b == RT_P_UNKNOWN) return RT_P_UNKNOWN;
    return RT_P_PURE;
}

#define RT_PURE_MAX_DEPTH 64
#define RT_PURE_MAX_NODES 20000

typedef struct RtPureCtx {
    Binding *stack[RT_PURE_MAX_DEPTH];
    uint32_t depth;
    /* Smallest stack index of an in-progress binding this subtree leaned on.
     * A result derived from an assumption about an OUTER frame is provisional
     * and must not be memoized -- that frame may still turn out impure. */
    uint32_t min_open;
    uint32_t budget;
    /* Set once the walk hit a binding with no body yet (a forward reference).
     * Every frame still open at that point depends on an answer that may
     * change later in the unit, so none of them may memoize -- sticky for
     * the rest of the walk (a frame opened afterwards loses a memo it could
     * have kept; that is a cost, never a wrong verdict). */
    bool     leaned_missing;
    /* CT1 gate only (rt_pred_observably_impure): a call the PREDICATE makes
     * directly to a `#reads` measure is UNKNOWN rather than walked.  Depth 0
     * only, so every callee's verdict -- and its memo -- is exactly what it
     * would be without the flag. */
    bool     reads_exempt;
} RtPureCtx;

static RtPurity rt_classify_expr(RtPureCtx *c, const Expr *x);

/* Builtins that compute a value from their arguments and touch nothing. */
static bool rt_builtin_shape_pure(BuiltinShape s) {
    switch (s) {
    case BS_BIN_INFIX:
    case BS_VARIADIC_FOLD:
    case BS_PREFIX_UNARY:
    case BS_AND_SC:
    case BS_OR_SC:
    case BS_DIV_CHECK:   /* may trap on /0, but a trap is not a state change */
        return true;
    default:
        return false;
    }
}

/* Builtins that definitely DO something: printing, writing memory, freeing,
 * loading libraries.  A shape in neither list is UNKNOWN, never impure. */
static bool rt_builtin_shape_impure(BuiltinShape s) {
    switch (s) {
    case BS_PRINTLN_INT:  case BS_PRINTLN_FLOAT:  case BS_PRINTLN_BOOL:
    case BS_PRINTLN_CSTR: case BS_PRINTLN_UINT:   case BS_PRINTLN_FLOAT32:
    case BS_PREFIX_UNARY_FREE:
    case BS_PTR_WRITE:    case BS_ARRAY_SET_UNCHECKED:
    case BS_RAW_MALLOC:   case BS_RAW_FREE:       case BS_RAW_REALLOC:
    case BS_RAW_MEMCPY:   case BS_RAW_MEMSET:
    case BS_DLOPEN:       case BS_DLSYM:          case BS_DLCLOSE:
        return true;
    default:
        return false;
    }
}

/* Does `b`'s DECLARED effect row rule out congruence?  Any non-empty row
 * does -- concrete (`#fx{Bt}`, `#fx{IO}`), a row variable (`#fx{e}`), or a
 * still-unresolved `#fx{...}` (rows resolve in the effect pass, after this
 * walk runs; an unresolved non-empty row is a declaration all the same). */
static bool rt_binding_row_vetoes_purity(const Binding *b) {
    return b->type.kind == TY_FN && b->type.as.fn.effect_row &&
           !effect_row_is_empty(b->type.as.fn.effect_row);
}

static RtPurity rt_classify_binding(RtPureCtx *c, Binding *b) {
    if (!b) return RT_P_UNKNOWN;
    if (b->refine_purity) return (RtPurity)(b->refine_purity - 1);
    for (uint32_t i = 0; i < c->depth; i++) {
        if (c->stack[i] == b) {
            /* Recursion: assume pure for now.  Impurity only ever enters
             * through a concrete leaf, so the greatest fixpoint is the right
             * one -- but record that we leaned on frame `i`. */
            if (i < c->min_open) c->min_open = i;
            return RT_P_PURE;
        }
    }
    FnDef *fd = b->source_fn_def;
    /* No body in hand: an extern, an inline-C-only defn whose FnDef never got
     * linked, or a forward reference not yet elaborated.  UNKNOWN -- not
     * congruent, and not diagnosable either -- and deliberately NOT memoized,
     * since the same name may be resolvable later in the unit. */
    if (!fd || !fd->body) {
        /* ... and neither is any verdict that LEANED on this answer: a caller
         * walked before its callee's body existed would otherwise memoize
         * UNKNOWN and never be asked again, which made every `^reflect`
         * function classified at its own definition (elab_reflect.c's eager
         * stamp) permanently impure if it called anything defined later.
         * The sticky flag keeps every open frame provisional, the way a
         * recursion edge keeps the frames above it provisional. */
        c->leaned_missing = true;
        return RT_P_UNKNOWN;
    }
    if (c->depth >= RT_PURE_MAX_DEPTH || c->budget == 0) return RT_P_UNKNOWN;

    uint32_t my_depth  = c->depth;
    uint32_t saved_min = c->min_open;
    c->stack[c->depth++] = b;
    c->min_open = UINT32_MAX;

    RtPurity r = rt_classify_expr(c, fd->body);

    /* A declared non-empty effect row rules PURE out.  It does not prove
     * IMPURE -- the row can name an effect the body never performs -- so it
     * only ever downgrades to UNKNOWN.  Applied HERE, at the binding, rather
     * than once at the top of the walk: it is memoized with the verdict (a
     * top-only veto was skipped on every memo hit after the first), and it
     * propagates -- a caller of a `#fx{Bt}` function is UNKNOWN too, which is
     * what makes the declared row worth more than the body walk alone. */
    if (r == RT_P_PURE && rt_binding_row_vetoes_purity(b)) r = RT_P_UNKNOWN;

    uint32_t used = c->min_open;
    c->depth--;
    if (used >= my_depth && !c->leaned_missing) b->refine_purity = (uint8_t)(r + 1);
    c->min_open = (used < saved_min) ? used : saved_min;
    return r;
}

static RtPurity rt_classify_expr(RtPureCtx *c, const Expr *x) {
    if (!x) return RT_P_PURE;
    if (c->budget == 0) return RT_P_UNKNOWN;
    c->budget--;

    switch (x->kind) {
    case EX_NIL_LIT: case EX_BOOL_LIT: case EX_INT_LIT:
    case EX_FLOAT_LIT: case EX_CSTR_LIT:
        return RT_P_PURE;

    case EX_VAR:
        /* Reading a mutable binding is not congruent -- two reads can differ --
         * but a read is not an EFFECT, so it is UNKNOWN, never impure. */
        if (!x->as.var.binding) return RT_P_UNKNOWN;
        return x->as.var.binding->is_mut ? RT_P_UNKNOWN : RT_P_PURE;

    /* Proven effects. */
    case EX_INLINE_C:
    case EX_SET: case EX_SET_DEREF:
    case EX_PERFORM:
        return RT_P_IMPURE;

    case EX_IF:
        return rt_p_join(rt_classify_expr(c, x->as.if_.cond),
                         rt_p_join(rt_classify_expr(c, x->as.if_.then_),
                                   rt_classify_expr(c, x->as.if_.else_or_null)));

    case EX_DO: {
        RtPurity r = RT_P_PURE;
        for (uint32_t i = 0; i < x->as.do_.n; i++)
            r = rt_p_join(r, rt_classify_expr(c, x->as.do_.items[i]));
        return r;
    }

    case EX_LET: {
        RtPurity r = rt_classify_expr(c, x->as.let_.body);
        for (uint32_t i = 0; i < x->as.let_.n; i++)
            r = rt_p_join(r, rt_classify_expr(c, x->as.let_.bindings[i].init));
        return r;
    }

    case EX_RETURN:
        return rt_classify_expr(c, x->as.return_.value);

    /* R4 groundwork (trusted-refinement-claims-plan): `::` and `(as T ...)`
     * are value computations -- an ascription is erased at codegen and a
     * numeric cast reads nothing but its operand -- so each is exactly as
     * pure as the expression under it.  Before this case they fell to the
     * UNKNOWN default, which made any measure that touches a defopaque
     * newtype via `::` unclassifiable and forced the RE0 pattern of
     * inline-C identity-cast unwrappers (themselves IMPURE by EX_INLINE_C).
     * Widening UNKNOWN -> operand's answer only ever removes diagnostics
     * and grows congruence, same as the EX_MATCH widening above. */
    case EX_ASCRIBE:
        return rt_classify_expr(c, x->as.ascribe_.inner);
    case EX_CAST:
        return rt_classify_expr(c, x->as.cast_.expr);
    case EX_REINTERPRET:   /* the compiler-built bitwise half of `::` */
        return rt_classify_expr(c, x->as.reinterpret_.expr);

    /* A `match` computes a value from its scrutinee and one arm; nothing about
     * dispatching on a constructor is observable.  Pattern BINDINGS are not
     * walked -- they are introduced by the pattern, not evaluated -- so an arm
     * that merely reads them stays pure.  Widening the classifier here grows
     * congruence (a measure written with a `match` is now usable as one) and
     * cannot grow TUR-E0375, since moving a form from UNKNOWN to PURE only
     * ever removes diagnostics. */
    case EX_MATCH: {
        RtPurity r = rt_classify_expr(c, x->as.match_.scrutinee);
        for (uint32_t i = 0; i < x->as.match_.n_arms; i++) {
            r = rt_p_join(r, rt_classify_expr(c, x->as.match_.arms[i].body));
            r = rt_p_join(r, rt_classify_expr(c, x->as.match_.arms[i].guard));
        }
        return r;
    }

    /* A field read is exactly as pure as its receiver.  There is no per-field
     * `mut` marker in the language, but there does not need to be one: `set!`
     * on `(.f s)` requires `s` to be bound `^mut`, so an immutable binding's
     * fields cannot be written THROUGH IT -- the same declaration-level
     * guarantee that already makes a non-mut variable read congruent.
     * Recursing on the receiver inherits that answer, and inherits IMPURE when
     * computing the receiver itself has an effect (`(.w (next-box))`).
     *
     * BEHIND A REFERENCE IT IS DECLINED.  The guarantee is about one binding,
     * not about the object: with `rc<T>` or a borrow, a caller can hold a
     * `^mut` handle to the very object the callee reads through a non-mut one,
     * and mutate it between two calls -- which is precisely the aliasing that
     * congruence assumes away.  A by-value receiver has no second handle to
     * mutate through (`:copy` copies, and a moved value leaves the caller
     * nothing), so the vector closes by construction.  Declining costs
     * precision on `rc` getters and nothing else.  Three congruence
     * miscompiles on this feature have come from assuming an aliasing
     * question away; this one is answered instead. */
    case EX_GET_FIELD: {
        const Expr *recv = x->as.get_field_.struct_expr;
        if (!recv) return RT_P_UNKNOWN;
        switch (recv->type.kind) {
            case TY_REF: case TY_RC: case TY_PTR_VOID:
            case TY_REF_IMMUT: case TY_REF_MUT:
                return RT_P_UNKNOWN;
            default: break;
        }
        return rt_classify_expr(c, recv);
    }

    case EX_BUILTIN: {
        if (!x->as.builtin.spec) return RT_P_UNKNOWN;
        BuiltinShape sh = x->as.builtin.spec->shape;
        RtPurity r = rt_builtin_shape_impure(sh) ? RT_P_IMPURE
                   : rt_builtin_shape_pure(sh)   ? RT_P_PURE
                                                 : RT_P_UNKNOWN;
        for (uint32_t i = 0; i < x->as.builtin.n; i++)
            r = rt_p_join(r, rt_classify_expr(c, x->as.builtin.args[i]));
        return r;
    }

    case EX_CALL: {
        /* An indirect call (`fn_expr`) or a rank-2 poly call can land
         * anywhere, so there is no callee to interrogate. */
        Binding *callee = x->as.call_.fn_binding;
        RtPurity r = (x->as.call_.fn_expr || x->as.call_.is_poly_call)
                   ? RT_P_UNKNOWN
                   : (c->reads_exempt && c->depth == 0 && callee &&
                      callee->reads_params_mask != 0)
                   ? RT_P_UNKNOWN
                   : rt_classify_binding(c, callee);
        for (uint32_t i = 0; i < x->as.call_.n_args; i++)
            r = rt_p_join(r, rt_classify_expr(c, x->as.call_.args[i]));
        return r;
    }

    default:
        /* A form the walk does not model -- match, field reads, closures,
         * while, STM, async, panic.  Neither proven pure nor proven
         * effectful, and that distinction is the whole point. */
        return RT_P_UNKNOWN;
    }
}

static RtPurity rt_classify_binding_top(Binding *b) {
    if (!b) return RT_P_UNKNOWN;
    if (b->refine_purity) return (RtPurity)(b->refine_purity - 1);
    /* The declared-row veto lives in rt_classify_binding itself, so it is
     * memoized and transitive; nothing to add at the top. */
    RtPureCtx c = { { 0 }, 0, UINT32_MAX, RT_PURE_MAX_NODES, false, false };
    return rt_classify_binding(&c, b);
}

/* True when calling `b` twice with equal arguments is guaranteed to produce
 * equal results with no observable side effect.  UNKNOWN reads as "no". */
bool rt_binding_is_pure(Binding *b) {
    return rt_classify_binding_top(b) == RT_P_PURE;
}

/* CT1/RT: does evaluating this elaborated predicate DO something observable?
 * Only a proven effect counts -- an unrecognised form answers false, so a
 * predicate the walk cannot model is never diagnosed and never blocks
 * elision on suspicion alone.
 *
 * C2 (#reads): a direct call to a `#reads` measure does not count.  CT1's
 * concern is a check whose evaluation changes state, so that compiling it in
 * or out is observable; a measure that only READS borrowed state is neutral
 * however often it runs.  Its body is still inline C (that is why it needs the
 * annotation), so the plain walk calls it IMPURE -- the annotation is the
 * trusted claim that it is not, the same trust a parameter refinement already
 * extends to it (TUR-W0383 reports the violations the compiler can see).  The
 * measure's ARGUMENTS are still walked, so `(vlen (next-vec!))` stays
 * E0375, and so does an unannotated wrapper around one: only the predicate's
 * own call sites are exempt.  One gate, so every contract position --
 * parameter, `:pre`, `:post`/return, `:invariant` -- agrees. */
static bool rt_pred_observably_impure(const Expr *x) {
    RtPureCtx c = { { 0 }, 0, UINT32_MAX, RT_PURE_MAX_NODES, false, true };
    return rt_classify_expr(&c, x) == RT_P_IMPURE;
}

/* mutable-globals-plan section 12.3, shipped as the warning tier (section
 * 13.1): does this elaborated body READ a mutable global?  Returns the first
 * such binding, or NULL.
 *
 * Positive evidence only.  The walk descends the kinds it models and simply
 * does not look inside the rest, so an inline-C body -- which is every
 * `#reads` measure that predates this -- yields no evidence and no warning.
 * Under-warning is the designed posture: this feeds a WARNING about a trusted
 * promise, never a proof, so a missed read costs one diagnostic while a false
 * positive would cost trust in the diagnostic.
 *
 * Direct reads only -- a global read inside a CALLEE is not followed.  The
 * transitive walk belongs to the gated refuse-the-override step if that ever
 * lands, where "could not see" must be distinguishable from "saw nothing"
 * (the same WG_UNKNOWN discipline the G1 write walk needed).
 *
 * A `(set! g ...)` TARGET deliberately does not count: writing a global from
 * a measure is a different defect, and this classifier is about what the
 * measure's ANSWER depends on.  The read half of `(set! g (+ g 1))` is still
 * seen -- it is an EX_VAR in the value expression. */
static const Binding *reads_scan_mut_global(const Expr *x, uint32_t *budget) {
    if (!x || *budget == 0) return NULL;
    (*budget)--;
    switch (x->kind) {
    case EX_VAR: {
        Binding *b = x->as.var.binding;
        return (b && b->is_mut && b->is_global) ? b : NULL;
    }
    case EX_LET: case EX_LETREC: {
        for (uint32_t i = 0; i < x->as.let_.n; i++) {
            const Binding *r =
                reads_scan_mut_global(x->as.let_.bindings[i].init, budget);
            if (r) return r;
        }
        return reads_scan_mut_global(x->as.let_.body, budget);
    }
    case EX_IF: {
        const Binding *r = reads_scan_mut_global(x->as.if_.cond, budget);
        if (!r) r = reads_scan_mut_global(x->as.if_.then_, budget);
        if (!r) r = reads_scan_mut_global(x->as.if_.else_or_null, budget);
        return r;
    }
    case EX_DO: {
        for (uint32_t i = 0; i < x->as.do_.n; i++) {
            const Binding *r = reads_scan_mut_global(x->as.do_.items[i], budget);
            if (r) return r;
        }
        return NULL;
    }
    case EX_WHILE: {
        const Binding *r = reads_scan_mut_global(x->as.while_.cond, budget);
        return r ? r : reads_scan_mut_global(x->as.while_.body, budget);
    }
    case EX_SET:
        return reads_scan_mut_global(x->as.set_.value, budget);
    case EX_MATCH: {
        const Binding *r = reads_scan_mut_global(x->as.match_.scrutinee, budget);
        for (uint32_t i = 0; !r && i < x->as.match_.n_arms; i++) {
            r = reads_scan_mut_global(x->as.match_.arms[i].guard, budget);
            if (!r) r = reads_scan_mut_global(x->as.match_.arms[i].body, budget);
        }
        return r;
    }
    case EX_GET_FIELD:
        return reads_scan_mut_global(x->as.get_field_.struct_expr, budget);
    case EX_BUILTIN: {
        for (uint32_t i = 0; i < x->as.builtin.n; i++) {
            const Binding *r = reads_scan_mut_global(x->as.builtin.args[i], budget);
            if (r) return r;
        }
        return NULL;
    }
    case EX_CALL: {
        const Binding *r = reads_scan_mut_global(x->as.call_.fn_expr, budget);
        for (uint32_t i = 0; !r && i < x->as.call_.n_args; i++)
            r = reads_scan_mut_global(x->as.call_.args[i], budget);
        return r;
    }
    case EX_ASCRIBE: return reads_scan_mut_global(x->as.ascribe_.inner, budget);
    case EX_CAST:    return reads_scan_mut_global(x->as.cast_.expr, budget);
    case EX_REINTERPRET:
        return reads_scan_mut_global(x->as.reinterpret_.expr, budget);
    case EX_RETURN:  return reads_scan_mut_global(x->as.return_.value, budget);
    /* R4 slice 1: `(unsafe ...)` desugars to a handle whose body is where
     * every raw-memory read lives (the gated builtins REQUIRE the block),
     * so not descending it would blind both evidence scans to exactly the
     * reads the trusted tier exists for.  Handler case bodies are read
     * positions too. */
    case EX_HANDLE: {
        const HandleExpr *h = x->as.handle_.handle;
        if (!h) return NULL;
        const Binding *r = reads_scan_mut_global(h->body, budget);
        for (uint8_t i = 0; !r && i < h->n_cases; i++)
            r = reads_scan_mut_global(h->cases[i].body, budget);
        return r;
    }
    default:
        return NULL;   /* unmodeled kind: no evidence, no warning */
    }
}

/* R4 slice 1 (trusted-refinement-claims-plan): chase a read's ROOT through
 * the value-shaping forms only -- VAR, `::`-ascription, cast, field hops,
 * and ptr-add/ptr-sub arithmetic (provenance follows the pointer operand).
 * Anything else (a CALL above all) returns NULL: no root, no evidence.
 * Interprocedural attribution belongs to the footprint walk proper, where
 * "could not see" must become a distinct verdict before it can back
 * anything stronger than silence.
 *
 * The chase also follows a raw LOAD (ptr-deref / array-get-unchecked)
 * through its pointer operand: a pointer loaded OUT of state reachable
 * from `s` points at state reachable from `s`, so a load through it still
 * roots at `s`.  This is what attributes the real ECS control-block shape
 * -- `gens = state[6]; gens[slot]` -- rather than only single-level
 * blocks.  Attribution is REACHABILITY-based on purpose: a block that
 * stores a pointer into state shared with some other root is the owning
 * module's aliasing discipline, the same documented trust boundary the
 * whole `frozen`+`#reads` tier already stands on (see
 * stateful-refinements-guide.md's trust-boundary section). */
static const Binding *reads_read_root(const Expr *x) {
    while (x) {
        switch (x->kind) {
        case EX_VAR:       return x->as.var.binding;
        case EX_ASCRIBE:   x = x->as.ascribe_.inner; break;
        case EX_CAST:      x = x->as.cast_.expr; break;
        case EX_REINTERPRET: x = x->as.reinterpret_.expr; break;
        case EX_GET_FIELD: x = x->as.get_field_.struct_expr; break;
        /* An `(unsafe ...)` marker handle's value IS its body's value (the
         * built-in Unsafe effect never performs, so the handler clause never
         * runs), and a `do`'s value is its last item -- both value-shaping,
         * so a load nested inside its own unsafe block still chases through.
         * A USER handle is not followed: its cases can replace the value. */
        case EX_HANDLE: {
            const HandleExpr *h = x->as.handle_.handle;
            if (!h || !h->is_unsafe_marker) return NULL;
            x = h->body;
            break;
        }
        case EX_DO:
            if (x->as.do_.n == 0) return NULL;
            x = x->as.do_.items[x->as.do_.n - 1];
            break;
        case EX_BUILTIN:
            if (x->as.builtin.spec &&
                (x->as.builtin.spec->shape == BS_PTR_ARITH ||
                 x->as.builtin.spec->shape == BS_PTR_DEREF ||
                 x->as.builtin.spec->shape == BS_ARRAY_GET_UNCHECKED) &&
                x->as.builtin.n >= 1) {
                x = x->as.builtin.args[0];
                break;
            }
            return NULL;
        default:           return NULL;
        }
    }
    return NULL;
}

/* Is this root a parameter of the frame's function that the mask OMITS?
 * Binding-pointer identity against the params array, so a let that shadows
 * a parameter's name can never mis-attribute. */
static const Binding *reads_root_unframed_param(const Binding *root,
                                                Binding **params,
                                                uint32_t n_params,
                                                uint64_t mask) {
    if (!root || !root->is_param) return NULL;
    for (uint32_t i = 0; i < n_params && i < 64; i++) {
        if (params[i] != root) continue;
        return (mask & (UINT64_C(1) << i)) ? NULL : root;
    }
    return NULL;
}

/* R4 slice 1: does this elaborated body DEMONSTRABLY read mutable state
 * rooted in a PARAMETER the `#reads` frame omits?  Same posture as
 * reads_scan_mut_global above: positive evidence only, direct reads only,
 * unmodeled kinds are not descended, and an inline-C body yields no
 * evidence.  The frame's contract is "the named parameters are the only
 * mutable state this answer depends on", and a mutable global is not the
 * only way to break it -- reading ANOTHER parameter's aliasable state does
 * too, and the per-parameter mask makes exactly that omission checkable.
 *
 * A "read of mutable state" here is one of:
 *   - a field read through a REFERENCE-typed receiver (the same decline
 *     list the purity walk uses at EX_GET_FIELD: a borrowed / rc / raw-ptr
 *     receiver is aliasable, so two calls the grant treats as one value
 *     can observe different fields), or
 *   - a raw-memory load (ptr-deref / array-get-unchecked).
 * By-value receivers stay silent on purpose: a copied aggregate cannot
 * change between two calls with equal arguments, which is the same
 * aliasing argument the purity walk's EX_GET_FIELD case documents. */
static const Binding *reads_scan_unframed_param(const Expr *x,
                                                Binding **params,
                                                uint32_t n_params,
                                                uint64_t mask,
                                                uint32_t *budget) {
#define RSUP(sub) reads_scan_unframed_param((sub), params, n_params, mask, budget)
    if (!x || *budget == 0) return NULL;
    (*budget)--;
    switch (x->kind) {
    case EX_GET_FIELD: {
        const Expr *recv = x->as.get_field_.struct_expr;
        if (recv) {
            switch (recv->type.kind) {
            case TY_REF: case TY_RC: case TY_PTR_VOID:
            case TY_REF_IMMUT: case TY_REF_MUT: {
                const Binding *r = reads_root_unframed_param(
                    reads_read_root(recv), params, n_params, mask);
                if (r) return r;
                break;
            }
            default: break;
            }
        }
        return RSUP(recv);
    }
    case EX_BUILTIN: {
        if (x->as.builtin.spec &&
            (x->as.builtin.spec->shape == BS_PTR_DEREF ||
             x->as.builtin.spec->shape == BS_ARRAY_GET_UNCHECKED) &&
            x->as.builtin.n >= 1) {
            const Binding *r = reads_root_unframed_param(
                reads_read_root(x->as.builtin.args[0]), params, n_params, mask);
            if (r) return r;
        }
        for (uint32_t i = 0; i < x->as.builtin.n; i++) {
            const Binding *r = RSUP(x->as.builtin.args[i]);
            if (r) return r;
        }
        return NULL;
    }
    case EX_LET: case EX_LETREC: {
        for (uint32_t i = 0; i < x->as.let_.n; i++) {
            const Binding *r = RSUP(x->as.let_.bindings[i].init);
            if (r) return r;
        }
        return RSUP(x->as.let_.body);
    }
    case EX_IF: {
        const Binding *r = RSUP(x->as.if_.cond);
        if (!r) r = RSUP(x->as.if_.then_);
        if (!r) r = RSUP(x->as.if_.else_or_null);
        return r;
    }
    case EX_DO: {
        for (uint32_t i = 0; i < x->as.do_.n; i++) {
            const Binding *r = RSUP(x->as.do_.items[i]);
            if (r) return r;
        }
        return NULL;
    }
    case EX_WHILE: {
        const Binding *r = RSUP(x->as.while_.cond);
        return r ? r : RSUP(x->as.while_.body);
    }
    case EX_SET:
        /* A write target is a different defect (same rule as the global
         * scan); the VALUE being written is still read. */
        return RSUP(x->as.set_.value);
    case EX_MATCH: {
        const Binding *r = RSUP(x->as.match_.scrutinee);
        for (uint32_t i = 0; !r && i < x->as.match_.n_arms; i++) {
            r = RSUP(x->as.match_.arms[i].guard);
            if (!r) r = RSUP(x->as.match_.arms[i].body);
        }
        return r;
    }
    case EX_CALL: {
        const Binding *r = RSUP(x->as.call_.fn_expr);
        for (uint32_t i = 0; !r && i < x->as.call_.n_args; i++)
            r = RSUP(x->as.call_.args[i]);
        return r;
    }
    case EX_ASCRIBE: return RSUP(x->as.ascribe_.inner);
    case EX_CAST:    return RSUP(x->as.cast_.expr);
    case EX_REINTERPRET: return RSUP(x->as.reinterpret_.expr);
    case EX_RETURN:  return RSUP(x->as.return_.value);
    case EX_HANDLE: {   /* see reads_scan_mut_global's EX_HANDLE rationale */
        const HandleExpr *h = x->as.handle_.handle;
        if (!h) return NULL;
        const Binding *r = RSUP(h->body);
        for (uint8_t i = 0; !r && i < h->n_cases; i++)
            r = RSUP(h->cases[i].body);
        return r;
    }
    default:
        return NULL;   /* unmodeled kind: no evidence, no warning */
    }
#undef RSUP
}

/* reads-frame-verification-ignores-a-callee-write-frame: does a `#reads`
 * body only READ its framed parameters?
 *
 * The two scans above ask what the body reads.  `#reads p` also promises the
 * body does not CHANGE p's state -- a measure that grows the vector it
 * measures is not a function of that vector -- and nothing asked.  That
 * mattered little while the only consumer of the promise was a call-site
 * crossing, whose callee entry check is never elided; the loop-invariant
 * frozen grant has no such backstop, so a false promise there elides a check
 * that would fail.
 *
 * Tri-state, like the WF2 / R4 walks:
 *   RW_CLEAN   -- every use of a framed parameter is a read this walk can see.
 *   RW_UNKNOWN -- some use is not: the parameter is aliased into a local,
 *                 returned, stored, captured, or handed to a callee slot with
 *                 no read-only promise.  Silent; withholds the loop grant.
 *   RW_WRITES  -- positive evidence: a field or deref store through it, a raw
 *                 memory write to it, or a callee slot whose `#writes` frame
 *                 names it (directly or through a `#reads` callee that itself
 *                 writes).  TUR-W0383, and every grant refused.
 *
 * A callee slot is read-only only when the callee is known PURE, or names that
 * slot in its own `#reads` frame and its own walk came back clean.  A declared
 * `#writes` frame that EXCLUDES the slot is not enough: it says the callee does
 * not write through the argument, not that it does not keep it.
 *
 * Inline C is the trust boundary, the same one the reads scans stop at: an
 * inline-C body uses its parameters in C text this walk cannot see, so it
 * answers CLEAN and the frame stays as trusted as it was.  What this closes is
 * the Turmeric body that says one thing and does another in plain sight.
 *
 * `allow` is "this expression's VALUE is consumed by a read-only use", so a
 * framed parameter reaching it is fine.  A scalar-typed value cannot alias
 * state, so it is always allowed; its sub-expressions are still walked. */
typedef enum { RW_CLEAN = 0, RW_UNKNOWN = 1, RW_WRITES = 2 } RwVerdict;

typedef struct {
    Binding  **params;
    uint32_t   n_params;
    uint64_t   mask;
    uint32_t   budget;
    uint64_t   writes;    /* framed params a RW_WRITES verdict reached */
} RwCtx;

static RwVerdict rw_worse(RwVerdict a, RwVerdict b) { return a > b ? a : b; }

static bool rw_scalar(const Expr *x) {
    switch (x->type.kind) {
    case TY_NIL: case TY_BOOL: case TY_INT: case TY_FLOAT: case TY_NEVER:
    case TY_INT8: case TY_INT16: case TY_INT32: case TY_INT64:
    case TY_UINT8: case TY_UINT16: case TY_UINT32: case TY_UINT64:
    case TY_FLOAT32: case TY_FLOAT64:
        return true;
    default:
        return false;
    }
}

/* The framed target index `root` is, or -1.  Binding identity, so a shadowing
 * local is never mistaken for the target. */
static int rw_framed(const RwCtx *c, const Binding *root) {
    if (!root) return -1;
    for (uint32_t i = 0; i < c->n_params && i < 64; i++)
        if (c->params[i] == root)
            return (c->mask & (UINT64_C(1) << i)) ? (int)i : -1;
    return -1;
}

/* What does `callee` do with the argument in slot j? */
static RwVerdict rw_slot(Binding *callee, uint32_t j) {
    if (!callee) return RW_UNKNOWN;
    if (j < WF_MAX_FRAME_PARAMS && callee->writes_declared &&
        (callee->writes_param_mask & ((uint32_t)1u << j)))
        return RW_WRITES;
    if (j < 64 && (callee->reads_params_mask & (UINT64_C(1) << j))) {
        if (callee->reads_write_mask & (UINT64_C(1) << j)) return RW_WRITES;
        if (callee->reads_frame_omits_state || callee->reads_write_unverified)
            return RW_UNKNOWN;
        return RW_CLEAN;
    }
    return rt_binding_is_pure(callee) ? RW_CLEAN : RW_UNKNOWN;
}

/* A store through `target`: RW_WRITES when it roots at a framed parameter. */
static RwVerdict rw_store(RwCtx *c, const Expr *target) {
    int k = rw_framed(c, reads_read_root(target));
    if (k < 0) return RW_CLEAN;
    c->writes |= UINT64_C(1) << k;
    return RW_WRITES;
}

static RwVerdict rw_scan(RwCtx *c, const Expr *x, bool allow) {
#define RWS(sub, a) rw_scan(c, (sub), (a))
    if (!x) return RW_CLEAN;
    if (c->budget == 0) return RW_UNKNOWN;
    c->budget--;
    if (rw_scalar(x)) allow = true;
    switch (x->kind) {
    case EX_NIL_LIT: case EX_BOOL_LIT: case EX_INT_LIT:
    case EX_FLOAT_LIT: case EX_CSTR_LIT:
        return RW_CLEAN;

    case EX_VAR:
        return (allow || rw_framed(c, x->as.var.binding) < 0) ? RW_CLEAN
                                                               : RW_UNKNOWN;

    case EX_INLINE_C:          /* the trust boundary; see above */
        return RW_CLEAN;

    case EX_ASCRIBE:     return RWS(x->as.ascribe_.inner, allow);
    case EX_CAST:        return RWS(x->as.cast_.expr, allow);
    case EX_REINTERPRET: return RWS(x->as.reinterpret_.expr, allow);
    case EX_GET_FIELD:   return RWS(x->as.get_field_.struct_expr, allow);

    case EX_BUILTIN: {
        if (!x->as.builtin.spec) return RW_UNKNOWN;
        BuiltinShape sh = x->as.builtin.spec->shape;
        RwVerdict v = RW_CLEAN;
        bool reads_args = rt_builtin_shape_pure(sh);
        switch (sh) {
        case BS_PRINTLN_INT:  case BS_PRINTLN_FLOAT: case BS_PRINTLN_BOOL:
        case BS_PRINTLN_CSTR: case BS_PRINTLN_UINT:  case BS_PRINTLN_FLOAT32:
        case BS_PTR_ARITH: case BS_UNSAFE_CAST: case BS_REINTERPRET:
        case BS_TRANSMUTE: case BS_RAW_MALLOC:
            reads_args = true;
            break;
        case BS_PREFIX_UNARY_FREE:
        case BS_PTR_WRITE: case BS_ARRAY_SET_UNCHECKED:
        case BS_RAW_MEMSET: case BS_RAW_FREE: case BS_RAW_REALLOC:
        case BS_RAW_MEMCPY:
            /* arg 0 is written; the rest are values that may be stored. */
            if (x->as.builtin.n >= 1) {
                v = rw_store(c, x->as.builtin.args[0]);
                v = rw_worse(v, RWS(x->as.builtin.args[0], true));
            }
            for (uint32_t i = 1; i < x->as.builtin.n; i++)
                v = rw_worse(v, RWS(x->as.builtin.args[i], false));
            return v;
        case BS_PTR_DEREF: case BS_ARRAY_GET_UNCHECKED:
            reads_args = true;
            break;
        default:
            break;
        }
        /* A read-only builtin whose result is not a scalar may hand back an
         * alias of its argument (pointer arithmetic, a cast), so its args are
         * only as allowed as its own value is. */
        bool a = reads_args && (allow || rw_scalar(x));
        for (uint32_t i = 0; i < x->as.builtin.n; i++)
            v = rw_worse(v, RWS(x->as.builtin.args[i], a));
        return v;
    }

    case EX_CALL: {
        RwVerdict v = RW_CLEAN;
        Binding *callee = (x->as.call_.fn_expr || x->as.call_.is_poly_call)
                              ? NULL : x->as.call_.fn_binding;
        if (x->as.call_.fn_expr) v = RWS(x->as.call_.fn_expr, false);
        for (uint32_t j = 0; j < x->as.call_.n_args; j++) {
            const Expr *a = x->as.call_.args[j];
            int k = rw_framed(c, reads_read_root(a));
            if (k < 0) { v = rw_worse(v, RWS(a, false)); continue; }
            RwVerdict sv = rw_slot(callee, j);
            if (sv == RW_WRITES) c->writes |= UINT64_C(1) << k;
            /* A read-only callee may still RETURN an alias of what it read,
             * which is only harmless where the call's own value is. */
            if (sv == RW_CLEAN && !allow) sv = RW_UNKNOWN;
            v = rw_worse(v, sv);
            v = rw_worse(v, RWS(a, true));
        }
        return v;
    }

    case EX_IF:
        return rw_worse(RWS(x->as.if_.cond, false),
                        rw_worse(RWS(x->as.if_.then_, allow),
                                 RWS(x->as.if_.else_or_null, allow)));
    case EX_DO: {
        /* A discarded value aliases nothing, so only the last item inherits. */
        RwVerdict v = RW_CLEAN;
        for (uint32_t i = 0; i < x->as.do_.n; i++)
            v = rw_worse(v, RWS(x->as.do_.items[i],
                                i + 1 < x->as.do_.n ? true : allow));
        return v;
    }
    case EX_LET: case EX_LETREC: {
        /* A binding's init is an alias the walk does not follow. */
        RwVerdict v = RWS(x->as.let_.body, allow);
        for (uint32_t i = 0; i < x->as.let_.n; i++)
            v = rw_worse(v, RWS(x->as.let_.bindings[i].init, false));
        return v;
    }
    case EX_WHILE:
        return rw_worse(RWS(x->as.while_.cond, false),
                        RWS(x->as.while_.body, true));
    case EX_SET: {
        RwVerdict v = RWS(x->as.set_.value, false);
        if (rw_framed(c, x->as.set_.target) >= 0) v = rw_worse(v, RW_UNKNOWN);
        return v;
    }
    case EX_SET_FIELD: {
        RwVerdict v = rw_store(c, x->as.set_field_.receiver);
        v = rw_worse(v, RWS(x->as.set_field_.receiver, true));
        return rw_worse(v, RWS(x->as.set_field_.value, false));
    }
    case EX_SET_DEREF: {
        RwVerdict v = rw_store(c, x->as.set_deref_.ref);
        v = rw_worse(v, RWS(x->as.set_deref_.ref, true));
        return rw_worse(v, RWS(x->as.set_deref_.value, false));
    }
    case EX_MATCH: {
        /* Pattern binders alias the scrutinee, so it is not a read-only use. */
        RwVerdict v = RWS(x->as.match_.scrutinee, false);
        for (uint32_t i = 0; i < x->as.match_.n_arms; i++) {
            v = rw_worse(v, RWS(x->as.match_.arms[i].guard, false));
            v = rw_worse(v, RWS(x->as.match_.arms[i].body, allow));
        }
        return v;
    }
    case EX_RETURN:
        return RWS(x->as.return_.value, false);
    case EX_HANDLE: {
        const HandleExpr *h = x->as.handle_.handle;
        if (!h) return RW_UNKNOWN;
        RwVerdict v = RWS(h->body, allow);
        for (uint8_t i = 0; i < h->n_cases; i++)
            v = rw_worse(v, RWS(h->cases[i].body, allow));
        return v;
    }
    case EX_RESUME: {
        const ResumeExpr *r = x->as.resume_.resume;
        if (!r) return RW_UNKNOWN;
        return rw_worse(RWS(r->k, false), RWS(r->value, false));
    }
    default:
        return RW_UNKNOWN;   /* unmodeled: could not see, never clean */
    }
#undef RWS
}

/* Run the walk over a `#reads` body.  The body's own value leaves the
 * function, so it is NOT a read-only use: a measure that returns its framed
 * parameter hands the caller a handle to write through. */
static RwVerdict reads_scan_frame_writes(const Expr *body, Binding **params,
                                         uint32_t n_params, uint64_t mask,
                                         uint64_t *writes_out) {
    RwCtx c = { params, n_params, mask, 4096, 0 };
    RwVerdict v = rw_scan(&c, body, false);
    if (writes_out) *writes_out = c.writes;
    return v;
}

/* A loop's frozen candidate: every use of `b` in the elaborated `while` is a
 * read.  The loop's own value is discarded, hence `allow`. */
static bool rw_binding_reads_only(const Expr *x, Binding *b) {
    Binding *t[1] = { b };
    RwCtx c = { t, 1, 1, 4096, 0 };
    return rw_scan(&c, x, true) == RW_CLEAN;
}


/* RT4: resolve a called function's return refinement for the encoder.  Owned
 * by the elaborator because it is the only side that can look a name up in the
 * global scope; refine_collect.c reaches it through a function pointer so it
 * stays free of scope/binding knowledge. */
bool rt_resolve_fn(void *ud, const char *name, RefineFnInfo *out) {
    Elab *e = (Elab *)ud;
    if (!e || !name) return false;
    /* No flag test here on purpose.  A refinement only reaches
     * `refine_return_pred` when something actually enforces it -- it was proved
     * statically, or the runtime check that guarantees it is being emitted --
     * and that decision is made where the callee is elaborated, which is the
     * only place that knows whether contracts survived for it.  Re-testing
     * `--no-contracts` here would also throw away INFERRED refinements, which
     * are proved facts and hold whether or not any check is emitted. */
    const Symbol *sym = symtab_intern(e->st, strslice(name, (uint32_t)strlen(name)));

    /* A DATA CONSTRUCTOR IS PURE BY CONSTRUCTION.  It stores its arguments and
     * runs no user code, so two applications to equal arguments hold equal
     * fields -- which is the only thing the VC ever asks, since a constructor
     * term is only ever reached through a selector.  (Object IDENTITY differs
     * between two applications, but nothing in the predicate language can
     * observe it.)
     *
     * Answered before the binding lookup because a constructor DOES have a
     * global binding and no `defn` body, so the default-deny body walk finds
     * no evidence and lands on UNKNOWN -- which congruence reads as impure.
     * That gave every occurrence its own symbol and made the constructor
     * axioms below inert: the `Box(p,3)` in the axiom and the `Box(p,3)` in
     * the goal were different terms. */
    CtorDef *cdef = elab_lookup_ctor(e, sym);
    if (cdef) {
        out->ret_pred    = NULL;
        out->ret_var     = NULL;
        out->param_names = NULL;
        out->n_params    = 0;
        out->pure        = true;
        /* A constructor yields an aggregate handle -- an opaque Int term, the
         * only thing the predicate language can say about it. */
        out->ret_sort    = VS_INT;
        out->is_ctor     = true;   /* reflected-measures: a ground argument head */
        /* RF4: tag and field names, so a reflected body's `match` can be
         * selected against a caller's tag fact and its binders tied to the
         * same `.field` selectors the arm hypotheses use (rt_prove_paths). */
        out->ctor_tag       = cdef->tag;
        out->ctor_is_record = cdef->is_record;
        out->ctor_n_fields  = cdef->n_fields;
        if (cdef->n_fields) {
            const char **fn = (const char **)arena_alloc(
                e->arena, cdef->n_fields * sizeof(char *));
            for (uint32_t i = 0; i < cdef->n_fields; i++)
                fn[i] = cdef->fields[i].name;
            out->ctor_field_names = fn;
        }
        return true;
    }

    Binding *b = scope_lookup(&e->global, sym);
    if (!b) {
        /* A TYPECLASS METHOD has no global binding under its bare name -- the
         * dispatch is resolved elsewhere -- so it used to fall through to the
         * abstract-measure rule below and pick up congruence for free.  That is
         * the same miscompile as the `tick` case, reached through a third door:
         * an instance method that counts up made `(- (tickm 1) (tickm 1))`
         * encode as `t - t`, the solver proved `t - t >= 0`, and the runtime
         * check that would have caught -1 was elided.
         *
         * A method is code that runs.  Report it as a known callee with NO
         * purity, so each occurrence gets its own symbol.  The class's result
         * refinement is deliberately NOT published here: which instance runs is
         * not known at the encoder, and the enforcement question is
         * per-instance. */
        /* Both dispatch spellings reach here: the bare `(m x)` as `m`, and the
         * dotted `(.m x)` as `.m`.  Look the dotted one up under its method
         * name -- checking only `sym` fixed the bare form and left the dotted
         * one still congruent, which the fuzzer caught two seeds later. */
        const Symbol *msym = sym;
        if (name[0] == '.' && name[1] != '\0')
            msym = symtab_intern(e->st, strslice(name + 1,
                                                 (uint32_t)strlen(name) - 1));
        const TypeClass *owner = NULL;
        const TypeClassMethod *m =
            typeclass_env_find_method(&e->typeclass_env, msym, &owner);
        if (m) {
            out->ret_pred    = NULL;
            out->ret_var     = NULL;
            out->param_names = NULL;
            out->n_params    = 0;
            out->pure        = false;
            /* RM-B1: the class signature's declared result type is the one
             * promise true of every instance, so its sort is the dispatch's. */
            out->ret_sort    = rt_sort_of_kind(m->return_type.kind);
            /* RT4: the CLASS's result refinement propagates, even though which
             * instance runs is unknown here -- because it is the one promise
             * true of EVERY instance.  Result variance is what buys that: an
             * instance either inherits the class predicate (and is checked
             * against it) or restates one, and then either its own is proved
             * to imply the class's or the class's is checked alongside it.
             *
             * Gated on the check surviving, matching `rt_ret_guaranteed` on the
             * `defn` path: `--no-contracts` removes the thing that enforces
             * this, so the fact must go with it. */
            if (m->return_refine_pred && rt_contracts_emitted()) {
                out->ret_pred = m->return_refine_pred;
                out->ret_var  = m->return_refine_var;
                /* Parameter names let a predicate that mentions a parameter be
                 * substituted with what the call site supplied.  The receiver
                 * is slot 0 of both the class signature and the dispatch, so
                 * the slots line up. */
                if (m->param_names && m->n_params) {
                    const char **pn = (const char **)arena_alloc(
                        e->arena, m->n_params * sizeof(char *));
                    for (uint8_t i = 0; i < m->n_params; i++)
                        pn[i] = m->param_names[i] ? m->param_names[i]->name : NULL;
                    out->param_names = pn;
                    out->n_params    = m->n_params;
                }
            }
            return true;
        }
        /* A RECORD FIELD SELECTOR `.f`.  Pure like the constructor it reads,
         * but its SORT is the field's.  Falling through to the abstract-measure
         * default declared every selector Int, so a `float` field's axiom
         * `(= (.a (FB 0.5 p)) 0.5)` -- or a written `(= (.a v) 0.5)` hypothesis
         * -- said an integer equals 0.5: a contradiction, which proved every
         * goal and elided its check.  `(let [x (FB 0.5 p)] 3)` "proved"
         * `(= r 7)` and returned 3.
         *
         * One sort per name, across every record that has such a field: the
         * field's own when they agree; Real when Int and Real fields share the
         * name (an integer is a Real, so this only forgoes integer reasoning);
         * and no answer when a Bool field shares it with a number -- the
         * encoder then refuses the mixed equality rather than tightening a
         * Bool into an Int. */
        if (name[0] == '.' && name[1] != '\0') {
            const char *fnm = name + 1;
            bool any = false, has_int = false, has_real = false, has_bool = false;
            for (uint32_t ai = 0; ai < e->n_adt_defs; ai++) {
                const AdtDef *adt = e->adt_defs[ai];
                if (!adt) continue;
                for (uint32_t ci = 0; ci < adt->n_ctors; ci++) {
                    const CtorDef *c = adt->ctors[ci];
                    if (!c || !c->name || !c->is_record) continue;
                    for (uint32_t fi = 0; fi < c->n_fields; fi++) {
                        if (!c->fields[fi].name || strcmp(c->fields[fi].name, fnm) != 0)
                            continue;
                        any = true;
                        VCSort s = rt_sort_of_kind(c->fields[fi].kind);
                        if (s == VS_REAL)      has_real = true;
                        else if (s == VS_BOOL) has_bool = true;
                        else                   has_int  = true;
                    }
                }
            }
            if (any && !(has_bool && (has_int || has_real))) {
                out->ret_pred    = NULL;
                out->ret_var     = NULL;
                out->param_names = NULL;
                out->n_params    = 0;
                out->pure        = true;
                out->ret_sort    = has_bool ? VS_BOOL : has_real ? VS_REAL : VS_INT;
                return true;
            }
        }
        /* Genuinely nothing: an abstract measure -- an uninterpreted
         * mathematical function, which the language defines as congruent. */
        return false;
    }

    out->ret_pred    = b->refine_return_pred;
    out->ret_var     = b->refine_return_var;
    out->param_names = b->refine_param_names;
    out->n_params    = b->n_refine_params;

    /* RM-B1: the sort the measure symbol is declared at.  `result_kind` is the
     * declared return type PEELED to its base, which is exactly what the VC
     * reasons over -- a `: #refine{ r : float | q }` return is a Real, and the
     * `q` is carried separately in ret_pred.  A non-fn binding (a `def` used as
     * a nullary measure) answers from its own type. */
    out->ret_sort = rt_sort_of_kind(b->type.kind == TY_FN ? b->type.as.fn.result_kind
                                                          : b->type.kind);

    /* C2 / #reads: publish the read-frame param so the encoder can grant
     * congruence when that argument is frozen at the call site.  R2: the
     * broken-promise evidence rides along so the encoder can refuse the grant
     * -- unconditional since checked-reads graduated (2026-08-20). */
    out->reads_params_mask      = b->reads_params_mask;
    out->reads_frame_omits_state = b->reads_frame_omits_state;
    out->reads_write_unverified  = b->reads_write_unverified;

    /* WF1/WF2 / #writes: publish the write frame and, crucially, whether it was
     * CHECKED.  A consumer that acts on the frame (WF3, WF4) must gate on
     * `writes_checked`; a trusted frame documents intent but has not been
     * verified against a body, so eliding on it would be trusting a promise. */
    out->writes_param_mask = b->writes_param_mask;
    out->writes_declared   = b->writes_declared;
    out->writes_checked    = b->writes_checked;

    /* PURITY, which decides whether two occurrences of this call may be
     * modelled as the same value.  See rt_binding_is_pure above: the declared
     * effect row is only a veto, the real evidence is a default-deny walk of
     * the callee's body. */
    out->pure = rt_binding_is_pure(b);

    /* reflected-measures RF3: a `^reflect` callee that PASSED the totality
     * gate (elab_reflect.c stamps 1; 2 is rejected, 0 not yet classified --
     * which the encoder treats as "no", so a verdict that arrives late costs
     * a missed unfolding, never a wrong one).  Re-tested against the gate so a
     * stale stamp can never publish without the experiment. */
    if (g_opt_reflected_measures && b->is_reflected && b->reflect_total == 1 &&
        b->reflect_body) {
        out->reflect_total       = true;
        out->reflect_body        = b->reflect_body;
        out->reflect_param_names = b->reflect_param_names;
        out->reflect_n_params    = b->reflect_n_params;
    }
    return true;
}

/* The resolver, for callers outside this file that build their own env. */
RefineFnResolver rt_refine_resolver(Elab *e) { (void)e; return rt_resolve_fn; }

#define RT_WF3_MAX_TARGETS 16   /* see the WF3 block below */
static bool rt_collect_set_targets(const Elab *e, const Form *f, uint32_t depth,
                                   const char **names, uint32_t *n);
static bool rt_form_mentions_name(const Elab *e, const Form *f,
                                  const char *name, uint32_t depth);
static bool rt_form_borrows_name(const Elab *e, const Form *f,
                                 const char *name, uint32_t depth);

/* A `^mut` parameter the body can rebind: assigned anywhere in it, borrowed
 * anywhere in it (the channel by which a callee writes a local), or any
 * `^mut` parameter at all when the body's assignments cannot be read.
 *
 * mut-param-refinement-trusted-after-set: its entry refinement (and a `:pre`
 * naming it) held ON ENTRY only.  Both used to enter this environment
 * unconditionally, so `(set! n -5) n` under `n : Nat` and a `Nat` return
 * "proved" the return obligation from the stale entry fact, elided the
 * return check, and returned -5. */
static bool rt_param_rebound(const Elab *e, const Binding *p, const Form *body,
                             const char *const *tg, uint32_t nt, bool tg_ok) {
    if (!p || !p->is_mut || !p->name) return false;
    if (!tg_ok) return true;
    for (uint32_t i = 0; i < nt; i++)
        if (strcmp(tg[i], p->name->name) == 0) return true;
    return body && rt_form_borrows_name(e, body, p->name->name, 0);
}

/* Build the hypothesis environment visible at a function's return point. */
static RefineEnv *rt_build_env(Elab *e, Binding **params, uint32_t n_params,
                               const Form **ct_param_preds,
                               const char **ct_param_varnames,
                               const uint32_t *ct_param_param_idx,
                               uint32_t n_ct_param_preds,
                               const Form *ct_pre_form,
                               const Form *whole_body, bool *dropped) {
    if (dropped) *dropped = false;
    RefineEnv *env = refine_env_new(e->arena);
    refine_env_set_resolver(env, rt_resolve_fn, e);
    for (uint32_t i = 0; i < n_params; i++) {
        if (!params[i] || !params[i]->name) continue;
        refine_env_declare(env, params[i]->name->name,
                           rt_sort_of_kind(params[i]->type.kind));
    }
    const char *tg[RT_WF3_MAX_TARGETS];
    uint32_t nt = 0;
    bool any_mut = false;
    for (uint32_t i = 0; i < n_params && !any_mut; i++)
        any_mut = params[i] && params[i]->is_mut;
    bool tg_ok = !any_mut || !whole_body ||
                 rt_collect_set_targets(e, whole_body, 0, tg, &nt);
    for (uint32_t i = 0; i < n_ct_param_preds; i++) {
        if (!ct_param_preds[i]) continue;
        uint32_t pi = ct_param_param_idx[i];
        if (pi >= n_params || !params[pi] || !params[pi]->name) continue;
        if (rt_param_rebound(e, params[pi], whole_body, tg, nt, tg_ok)) {
            if (dropped) *dropped = true;
            continue;
        }
        refine_env_push(env, ct_param_preds[i], ct_param_varnames[i],
                        params[pi]->name->name);
    }
    /* A `:pre` predicate mentions the parameters directly -- nothing to
     * rename, so it rides in with a NULL bound variable. */
    if (ct_pre_form) {
        bool stale = false;
        for (uint32_t i = 0; i < n_params && !stale; i++)
            stale = rt_param_rebound(e, params[i], whole_body, tg, nt, tg_ok) &&
                    rt_form_mentions_name(e, ct_pre_form, params[i]->name->name, 0);
        if (!stale) refine_env_push(env, ct_pre_form, NULL, NULL);
        else if (dropped) *dropped = true;
    }
    return env;
}

/* ------------------------------------------------------------------------- *
 * RT4: template-based predicate propagation.
 *
 * A deliberately small convenience, NOT the general refinement inference the
 * plan rules out.  We do not search for a predicate that satisfies a recursive
 * constraint system (that is the LiquidHaskell layer we deleted by making
 * refinements written rather than inferred); we try a fixed vocabulary of
 * shapes against the body and keep the first the solver can prove.  Wrong
 * guesses cost a discarded proof attempt, never a wrong answer.
 * ------------------------------------------------------------------------- */

#define RT4_RESULT_VAR "__rt_result"

/* Build the predicate Form `(<op> __rt_result 0)`. */
static const Form *rt4_template(Elab *e, const char *op, Span span, bool is_float) {
    Form **items = (Form **)arena_alloc(e->arena, 3 * sizeof(Form *));
    items[0] = form_sym(e->arena, span,
                        symtab_intern(e->st, strslice(op, (uint32_t)strlen(op))));
    items[1] = form_sym(e->arena, span,
                        symtab_intern(e->st, strslice(RT4_RESULT_VAR,
                                                      (uint32_t)strlen(RT4_RESULT_VAR))));
    items[2] = is_float ? form_float(e->arena, span, 0.0)
                        : form_int(e->arena, span, 0);
    return form_list(e->arena, span, items, 3);
}

/* Try each shape against `subject` under `env`; return the first one a backend
 * proves, or NULL.  Silent: these are probes, not obligations the user wrote,
 * so a failed one reports nothing and counts for nothing. */
static const Form *rt4_infer_return(Elab *e, const Form *subject, TypeKind ret_kind,
                                    RefineEnv *env, Span span) {
    if (!subject || !env) return NULL;
    bool is_float = (ret_kind == TY_FLOAT || ret_kind == TY_FLOAT32 ||
                     ret_kind == TY_FLOAT64);
    /* Ordered most-informative first, so `(> r 0)` wins over `(>= r 0)` when
     * both hold. */
    static const char *const SHAPES[] = { ">", "<", ">=", "<=", "not=" };
    for (size_t i = 0; i < sizeof(SHAPES) / sizeof(SHAPES[0]); i++) {
        const Form *tmpl = rt4_template(e, SHAPES[i], span, is_float);
        RefineObligation probe;
        memset(&probe, 0, sizeof(probe));
        probe.predicate   = tmpl;
        probe.var_name    = RT4_RESULT_VAR;
        probe.subject     = subject;
        probe.base_sort   = rt_sort_of_kind(ret_kind);
        probe.loc         = span;
        probe.env         = env;
        probe.what        = "inferred result refinement";
        probe.speculative = true;
        if (refine_discharge_one(&probe, e->arena)) return tmpl;
    }
    return NULL;
}

/* Collect one return-position obligation and decide it now.  Returns true when
 * a backend proved it, so the caller can skip injecting the runtime check. */
/* True when evaluating `pred` would do something observable -- it calls a
 * function that is not known pure.
 *
 * Eliding a check is only invisible when running the check was invisible.  A
 * predicate like `(>= (tick) 0)`, where `tick` bumps a counter, makes the
 * check itself part of the program's behaviour: with the check emitted the
 * counter advances, without it the counter does not, and the program prints
 * different numbers.  The fuzzer found exactly that as an OUTPUT DIVERGENCE
 * (not a soundness bug -- both runs were internally consistent, they just
 * disagreed).
 *
 * An impure predicate is a mistake in its own right -- `TUR-E0375` exists for
 * it and nothing emits it, so a contract whose evaluation has side effects is
 * accepted today and already behaves differently under `--no-contracts`. That
 * is a contract-layer issue and is reported separately. What this feature owes
 * is narrower and absolute: turning the gate on must not change behaviour, so
 * a check guarding an impure predicate is never elided. */
static bool rt_pred_is_impure(Elab *e, const Form *f) {
    if (!f) return false;
    if (f->tag != F_LIST || f->as.list.len == 0) return false;
    const Form *head = f->as.list.items[0];
    if (head->tag == F_SYM && head->as.sym) {
        RefineFnInfo info;
        memset(&info, 0, sizeof(info));
        /* A name that resolves to nothing is an abstract measure -- a
         * mathematical function, so nothing runs.  Builtins (`+`, `and`, ...)
         * land there too, which is correct: they are pure. */
        if (rt_resolve_fn(e, head->as.sym->name, &info) && !info.pure)
            return true;
    }
    for (uint32_t i = 0; i < f->as.list.len; i++)
        if (rt_pred_is_impure(e, f->as.list.items[i])) return true;
    return false;
}

/* C2 (#reads): does this predicate reference a `#reads`-annotated measure?
 *
 * Mirrors rt_pred_is_impure, but keys on the reads grant rather than impurity:
 * a measure declared `#reads w` resolves to a RefineFnInfo with a non-zero
 * reads_params_mask.  rt_inject_param_checks uses this to skip the (impossible)
 * runtime entry contract for such a param -- see the comment at its call site.
 *
 * The recursive walk matches a `#reads` measure anywhere in a compound
 * predicate (e.g. `(and (alive? w x) ...)`): the entry contract must be
 * suppressed as a whole, since the impure sub-term alone makes it unemittable. */
static bool rt_pred_reads_measure(Elab *e, const Form *f) {
    if (!f) return false;
    if (f->tag != F_LIST || f->as.list.len == 0) return false;
    const Form *head = f->as.list.items[0];
    if (head->tag == F_SYM && head->as.sym) {
        RefineFnInfo info;
        memset(&info, 0, sizeof(info));
        if (rt_resolve_fn(e, head->as.sym->name, &info) &&
            info.reads_params_mask != 0)
            return true;
    }
    for (uint32_t i = 0; i < f->as.list.len; i++)
        if (rt_pred_reads_measure(e, f->as.list.items[i])) return true;
    return false;
}

/* R2 (checked-reads, GRADUATED 2026-08-20): same walk, narrowed to a `#reads`
 * measure carrying broken-frame evidence (reads_frame_omits_state) -- i.e. one
 * whose congruence grant the encoder refuses.  Feeds only the
 * W0372 wording at the crossing, so the "guard it inside a `frozen` region"
 * advice is not given for a crossing where the region is present and the
 * FRAME is what failed. */
static bool rt_pred_reads_measure_refused(Elab *e, const Form *f) {
    if (!f) return false;
    if (f->tag != F_LIST || f->as.list.len == 0) return false;
    const Form *head = f->as.list.items[0];
    if (head->tag == F_SYM && head->as.sym) {
        RefineFnInfo info;
        memset(&info, 0, sizeof(info));
        if (rt_resolve_fn(e, head->as.sym->name, &info) &&
            info.reads_params_mask != 0 && info.reads_frame_omits_state)
            return true;
    }
    for (uint32_t i = 0; i < f->as.list.len; i++)
        if (rt_pred_reads_measure_refused(e, f->as.list.items[i])) return true;
    return false;
}

/* ------------------------------------------------------------------------- *
 * RT4 branching bodies: prove a return obligation PER PATH.
 *
 * The obligation is `pred[body/r]`.  When the body is a single expression the
 * encoder handles it directly, but `(if c t e)` is not an arithmetic term --
 * there is no value to substitute -- so the whole obligation answered unknown
 * and the check stayed.  That rules out most real function bodies.
 *
 * Splitting recovers them.  `(if c t e)` is discharged when BOTH
 *
 *     c  |- pred[t/r]        and        (not c) |- pred[e/r]
 *
 * hold, which is exactly the path-sensitive reading: on each path the value
 * returned is that branch's, and the condition that selected it is a fact.
 * `(let [x v] body)` adds `x = v` and recurses into the body, so a body that
 * names its result still gets checked.
 *
 * Every sub-obligation is discharged SILENTLY.  A failure here is not a
 * diagnostic -- it means this strategy did not work, and the caller falls back
 * to the ordinary whole-body obligation, which reports in the ordinary place
 * with the ordinary message.  So splitting can only ever prove MORE; it never
 * changes what an unproven function looks like.
 * ------------------------------------------------------------------------- */

#define RT_PATH_MAX_DEPTH 8

/* Discharge one path's obligation without reporting anything. */
static bool rt_prove_silent(Elab *e, const Form *pred, const char *var_name,
                            const Form *subject, TypeKind base_kind,
                            RefineEnv *env, Span loc) {
    if (!pred || !subject) return false;
    RefineObligation *ob = refine_collect_obligation(
        &e->refine_obs, pred, var_name, subject, rt_sort_of_kind(base_kind),
        type_name(type_simple(base_kind, CK_COPY)), loc, env, "path", NULL);
    if (!ob) return false;
    ob->speculative = true;
    ob->path_probe  = true;
    return refine_discharge_one(ob, e->arena);
}

/* True when `f` is a list whose head is the symbol `name`. */
static bool rt_head_is(const Form *f, const char *name) {
    if (!f || f->tag != F_LIST || f->as.list.len == 0) return false;
    const Form *h = f->as.list.items[0];
    if (h->tag != F_SYM || !h->as.sym) return false;
    return strcmp(h->as.sym->name, name) == 0;
}

static bool rt_prove_paths(Elab *e, const Form *pred, const char *var_name,
                           const Form *subject, TypeKind base_kind,
                           RefineEnv *env, Span loc, uint32_t depth);

/* True when `f` contains an assignment anywhere.  Used to decide whether a
 * statement can be stepped over: an assignment may stale a hypothesis already
 * in the environment, and every other form cannot.  Deliberately syntactic and
 * deliberately over-broad -- a name that merely LOOKS like an assignment costs
 * precision, and missing one costs soundness. */
/* 24, matching RT_CS_PATH_MAX_DEPTH: the scan is linear in nodes either way,
 * and a macro EXPANSION is legitimately deeper than the source that spells it
 * -- a 12 limit made rt_form_mentions_set return its conservative "too deep,
 * assume assignment" answer on a clean for-each expansion, spuriously
 * declining every crossing in the caller. */
#define RT_SET_SCAN_MAX_DEPTH 24

static const Form *rt_macro_expansion(const Elab *e, const Form *call);

static bool rt_form_mentions_set(const Elab *e, const Form *f, uint32_t depth) {
    if (!f) return false;
    if (depth >= RT_SET_SCAN_MAX_DEPTH) return true;   /* too deep to vouch for */
    if (f->tag == F_SYM && f->as.sym) {
        const char *n = f->as.sym->name;
        return strcmp(n, "set!") == 0 || strcmp(n, "swap!") == 0 ||
               strcmp(n, "reset!") == 0;
    }
    if (f->tag != F_LIST && f->tag != F_VEC) return false;
    /* A macro call scans as its EXPANSION: the source spelling shows no
     * `set!`, but the code that runs is the expansion, and an assignment
     * hidden in a template must decline exactly like a written one.  The hop
     * does NOT consume depth -- it is a lateral move to fresh nodes, not
     * structural nesting, and cannot cycle (an expansion's nodes are newly
     * constructed; only spliced ARGUMENTS are reused, and a call form cannot
     * be its own argument). */
    {
        const Form *mx = rt_macro_expansion(e, f);
        if (mx) return rt_form_mentions_set(e, mx, depth);
    }
    for (uint32_t i = 0; i < f->as.list.len; i++)
        if (rt_form_mentions_set(e, f->as.list.items[i], depth + 1)) return true;
    return false;
}

/* ------------------------------------------------------------------------- *
 * WF3: frame-aware hypothesis invalidation.
 *
 * `rt_form_mentions_set` above is the COARSEST correct rule -- one assignment
 * anywhere in a caller body drops every hypothesis for every crossing in it,
 * however unrelated.  That is the binding constraint on two shipped surfaces
 * (an accumulator `set!` in a `for-each-alive!` body, a counter `set!` in a
 * `while`), so the helpers below let a hypothesis survive an assignment that
 * provably cannot touch what it mentions.
 *
 * The slice is deliberately narrow (docs/archive/checked-write-frames-plan.md,
 * WF3).  A hypothesis survives only when EVERY assignment in the body targets a
 * PLAIN SYMBOL that the hypothesis does not mention and that the body never
 * borrows.  Anything else -- a place expression (`(set! (.n w) 9)`), an
 * assignment symbol used in a position this scan cannot attribute to a target,
 * a scan that runs out of depth or target slots -- keeps the whole-body
 * decline.  When in doubt the old rule is the fallback, because the failure
 * mode here is a stale hypothesis proving a fresh lie.
 *
 * Why the assignment's VALUE needs no separate check: a hypothesis is only
 * USABLE if its terms are congruent, and congruence is granted only to a pure
 * measure or to a `#reads` measure inside a region that freezes its argument
 * (`enc_reads_arg_frozen`).  A pure measure cannot be disturbed by any call, and
 * inside a frozen region a mutator of the frozen world is statically
 * unreachable (`TUR-E0200`).  So a call in the value position cannot stale a
 * hypothesis that was going to be believed; only rebinding a name it mentions
 * can, which is exactly what is tested here.
 *
 * Why a plain symbol target cannot alias: turmeric passes by value, so handing
 * a local to a callee -- even `^mut` -- cannot let the callee write the
 * caller's slot.  The one channel that could is an explicit borrow, so a body
 * that borrows an assigned name keeps the full decline.
 *
 * That by-value argument is about LOCALS, and it does not extend to a mutable
 * global -- a global is written by NAME rather than passed, so a callee can
 * write one this body never mentions.  This scan could not tell the two apart
 * even if it wanted to: it walks FORMS and compares symbol NAMES, and never
 * resolves a symbol to a Binding.
 *
 * It is sound anyway, for a reason worth writing down because it is not the
 * reason above.  A hypothesis is only USABLE if its terms are congruent, and
 * `rt_classify_expr`'s EX_VAR case answers UNKNOWN for a read of any `is_mut`
 * binding -- which is exactly the flag a `^mut` global carries.  So a
 * hypothesis that depends on a mutable global is never believed in the first
 * place, and there is nothing for a global write to stale.  Pinned by
 * errors/refine-impure-global-not-congruent.
 *
 * The consequence for anyone widening either side: if purity is ever widened
 * to admit a read of a global (say, "a global never written in this unit is
 * effectively constant"), THIS scan becomes the thing standing between a
 * callee's global write and a stale hypothesis -- and it cannot do that job as
 * written.  Widen the two together or not at all.  See
 * docs/archive/mutable-globals-plan.md §4.5.  RT_WF3_MAX_TARGETS (16) is
 * defined above rt_build_env, which needs it first. */

static bool rt_sym_is(const Form *f, const char *name);

/* Collect the names assigned anywhere in `f`.
 *
 * Returns false when the body contains an assignment this analysis cannot
 * vouch for; the caller must then keep the whole-body decline.  Mirrors
 * `rt_form_mentions_set`'s traversal exactly -- including the macro-expansion
 * hop -- so an assignment hidden in a template is attributed to its target
 * rather than merely detected. */
static bool rt_collect_set_targets(const Elab *e, const Form *f, uint32_t depth,
                                   const char **names, uint32_t *n) {
    if (!f) return true;
    if (depth >= RT_SET_SCAN_MAX_DEPTH) return false;  /* too deep to vouch for */
    if (f->tag == F_SYM && f->as.sym) {
        /* An assignment symbol reached anywhere other than the head of a form
         * this function recognized: it may be aliased, passed, or applied, and
         * the target is not readable from here. */
        const char *nm = f->as.sym->name;
        if (strcmp(nm, "set!") == 0 || strcmp(nm, "swap!") == 0 ||
            strcmp(nm, "reset!") == 0)
            return false;
        return true;
    }
    if (f->tag != F_LIST && f->tag != F_VEC) return true;
    {
        const Form *mx = rt_macro_expansion(e, f);
        if (mx) return rt_collect_set_targets(e, mx, depth, names, n);
    }
    if (f->as.list.len >= 2 &&
        (rt_sym_is(f->as.list.items[0], "set!") ||
         rt_sym_is(f->as.list.items[0], "swap!") ||
         rt_sym_is(f->as.list.items[0], "reset!"))) {
        const Form *target = f->as.list.items[1];
        if (!target || target->tag != F_SYM || !target->as.sym)
            return false;                    /* place expression: decline */
        const char *tn = target->as.sym->name;
        bool seen = false;
        for (uint32_t i = 0; i < *n; i++)
            if (strcmp(names[i], tn) == 0) { seen = true; break; }
        if (!seen) {
            if (*n >= RT_WF3_MAX_TARGETS) return false;   /* out of slots */
            names[(*n)++] = tn;
        }
        /* The head and the target are accounted for; the VALUE operands may
         * still hide further assignments. */
        for (uint32_t i = 2; i < f->as.list.len; i++)
            if (!rt_collect_set_targets(e, f->as.list.items[i], depth + 1,
                                        names, n))
                return false;
        return true;
    }
    for (uint32_t i = 0; i < f->as.list.len; i++)
        if (!rt_collect_set_targets(e, f->as.list.items[i], depth + 1, names, n))
            return false;
    return true;
}

/* True when `f` mentions the symbol `name` anywhere.  Used both to decide
 * whether an assignment kills a hypothesis and (via `rt_form_borrows_name`)
 * whether a name escapes by reference. */
static bool rt_form_mentions_name(const Elab *e, const Form *f,
                                  const char *name, uint32_t depth) {
    if (!f || !name) return false;
    if (depth >= RT_SET_SCAN_MAX_DEPTH) return true;   /* too deep to vouch for */
    if (f->tag == F_SYM)
        return rt_sym_is(f, name);
    if (f->tag != F_LIST && f->tag != F_VEC) return false;
    {
        const Form *mx = rt_macro_expansion(e, f);
        if (mx) return rt_form_mentions_name(e, mx, name, depth);
    }
    for (uint32_t i = 0; i < f->as.list.len; i++)
        if (rt_form_mentions_name(e, f->as.list.items[i], name, depth + 1))
            return true;
    return false;
}

/* True when `f` borrows `name` -- `(& name)`.  A borrowed local is the one
 * channel by which a callee could write a caller's slot, so an assigned name
 * that is also borrowed keeps the whole-body decline. */
static bool rt_form_borrows_name(const Elab *e, const Form *f,
                                 const char *name, uint32_t depth) {
    if (!f || !name) return false;
    if (depth >= RT_SET_SCAN_MAX_DEPTH) return true;   /* too deep to vouch for */
    if (f->tag != F_LIST && f->tag != F_VEC) return false;
    {
        const Form *mx = rt_macro_expansion(e, f);
        if (mx) return rt_form_borrows_name(e, mx, name, depth);
    }
    /* `&mut x` is the borrow a callee WRITES through (`(set! @r v)`), so it
     * is the one that matters most; it was missing here, which let a crossing
     * keep a guard on `x` across `(zap &mut x)`. */
    if (f->as.list.len >= 2 &&
        (rt_sym_is(f->as.list.items[0], "&") || rt_sym_is(f->as.list.items[0], "&mut")) &&
        rt_sym_is(f->as.list.items[1], name))
        return true;
    for (uint32_t i = 0; i < f->as.list.len; i++)
        if (rt_form_borrows_name(e, f->as.list.items[i], name, depth + 1))
            return true;
    return false;
}

/* ------------------------------------------------------------------------- *
 * WF2: the checked tier for `#writes` frames.
 *
 * A body with no inline C is walkable, so a declared frame can be VERIFIED
 * rather than believed.  That distinction is the whole point: `#reads` is step
 * 1 of the trajectory (trusted, sound because it changes nothing at runtime),
 * and every step past it -- WF3's callee-frame widening, WF4's entry-check
 * elision -- ELIDES or REORDERS real work on the strength of the claim.  A
 * promise cannot back that; a checked fact can.
 *
 * The walk classifies each function into exactly one of three outcomes:
 *
 *   - VERIFIED    -- every write channel was visible and landed inside the
 *                    frame.  `writes_checked = true`; consumers may act on it.
 *   - EXCEEDED    -- a write outside the frame was seen.  TUR-E0382, because a
 *                    declared frame the body exceeds is an error and not a
 *                    silent widening (the `#reads` rule).  `writes_checked`
 *                    stays false.
 *   - UNVERIFIED  -- something in the body could not be vouched for: inline C,
 *                    a callee that does not resolve, a resolvable callee with
 *                    no declared frame that receives a frame-relevant argument,
 *                    or the scan running out of depth.  NO diagnostic --
 *                    `writes_checked` simply stays false, so the declaration
 *                    still documents intent and nothing optimizes on it.
 *
 * Making the third case a downgrade rather than an error is what keeps
 * `#writes` adoptable: an error would mean no function could carry a frame
 * until its entire transitive callee set carried one too.  It is also the
 * honest answer -- "I could not check this" is not "you did something wrong".
 *
 * Three write channels are checked, matching the plan's WF2 list:
 *   1. a direct `set!`/`swap!`/`reset!` through a parameter;
 *   2. an argument passed to a `^mut` parameter of a callee;
 *   3. a callee's own declared `#writes` frame, mapped back through the
 *      argument list to this function's parameters.
 *
 * What is deliberately NOT a write: assigning a parameter's own bare symbol
 * (`(set! p 5)`).  Turmeric passes by value, so that rebinds this function's
 * local slot and the caller cannot observe it -- the same by-value argument
 * the WF3 slice above already rests on.  Keeping these two consistent matters:
 * if a bare-symbol assignment could escape, WF3's "a plain symbol target
 * cannot alias" rule would be unsound too.
 *
 * A PLACE expression rooted at a parameter (`(set! (.n w) 9)`) DOES count.
 * Note this is an OVER-approximation, not an equivalence, and an earlier
 * version of this comment overstated it as "does reach the caller's object":
 *
 *   - through an `rc<Struct>` or a `:heap` struct receiver it genuinely does
 *     reach the caller's object, which is the case the rule exists for;
 *   - through a plain by-value struct parameter it reaches a COPY, and the
 *     compiled backend emits a store the C compiler then deletes outright.
 *     Counting it is therefore pessimistic there -- it can force a frame wider
 *     than the caller could ever observe.
 *
 * Pessimism is the safe direction for a frame (it over-declares, never
 * under-declares), so the rule stands.  But the by-value case is a silent
 * no-op in the compiled backend and a write-through in the turi interpreter,
 * which is a live divergence -- see
 * docs/archive/struct-param-mutation-backend-divergence.md.  If that is
 * resolved by rejecting the no-op write, this branch narrows to the rc/heap
 * receivers and stops being an over-approximation at all. */
#define WF_SCAN_MAX_DEPTH 24   /* matches RT_SET_SCAN_MAX_DEPTH; same tradeoff */

typedef enum WfVerdict {
    WF_VERIFIED = 0,   /* every channel seen, all inside the frame */
    WF_UNVERIFIED,     /* something could not be vouched for; no diagnostic */
    WF_EXCEEDED,       /* a definite write outside the frame; TUR-E0382 */
} WfVerdict;

/* The 0-based parameter index `f` names, or -1 when `f` is not a bare symbol
 * naming one of `params`.  Compared by interned Symbol identity, not spelling,
 * so a local that shadows a parameter's name does not masquerade as it. */
static int wf_param_index(const Form *f, Binding *const *params, uint32_t n_params) {
    if (!f || f->tag != F_SYM || !f->as.sym) return -1;
    for (uint32_t i = 0; i < n_params; i++)
        if (params[i] && params[i]->name == f->as.sym) return (int)i;
    return -1;
}

/* The Form-era `wf_walk` / `wf_place_root_param` lived here.  They are gone:
 * the frame check now walks the elaborated Expr body (`wf_scan`, below), for
 * the reasons recorded in its header comment.  `wf_param_index` above stays --
 * the WF3 scanners still ask its question about a Form. */
static WfVerdict wf_worse(WfVerdict a, WfVerdict b) { return a > b ? a : b; }

/* G4b/§13.7: does this initializer form mention a `^thread-local` global?
 * Returns the offending symbol, or NULL.  Form-level and resolved through the
 * global scope, which is enough: a `^thread-local` is a global by definition,
 * so a reference to one is a bare symbol naming it.  A local that shadows the
 * name reads as a reference here -- pessimistic, and the safe direction. */
static const Symbol *tl_init_references_thread_local(Elab *e, const Form *f,
                                                     uint32_t depth) {
    if (!f || depth >= 24) return NULL;
    if (f->tag == F_SYM && f->as.sym) {
        Binding *b = scope_lookup(&e->global, f->as.sym);
        return (b && b->is_thread_local) ? f->as.sym : NULL;
    }
    if (f->tag != F_LIST && f->tag != F_VEC) return NULL;
    for (uint32_t i = 0; i < f->as.list.len; i++) {
        const Symbol *r = tl_init_references_thread_local(e, f->as.list.items[i],
                                                          depth + 1);
        if (r) return r;
    }
    return NULL;
}

/* G4a: may a value of this kind be loaded/stored by the atomics macro layer?
 *
 * EIGHT-BYTE KINDS ONLY, and the width is the whole point rather than a
 * detail.  The layer's host shim is `uint64_t`-typed (src/runtime/tur_atomics.c),
 * so the emitted access is an 8-byte one whatever the declaration says -- and a
 * narrower global really is declared narrower (`static bool g;` is one byte).
 * Admitting `:bool` produced an 8-byte `__atomic_store_n` into a 1-byte object:
 * a genuine overflow of the adjacent statics that GCC caught as
 * `-Wstringop-overflow` and that still printed the right answer, which is how
 * it would have shipped.
 *
 * A narrower kind is therefore rejected rather than silently widened.  Adding
 * `:bool` back means a 1-byte shim on the non-GNU branch, not a change here. */
bool type_is_atomic_scalar(TypeKind k) {
    switch (k) {
        case TY_INT: case TY_FLOAT: case TY_CSTR: case TY_PTR_VOID:
            return true;
        default:
            return false;
    }
}

/* ------------------------------------------------------------------------- *
 * G1: does a body write a MUTABLE GLOBAL?
 * docs/archive/mutable-globals-plan.md §4.1, §4.5.
 *
 * Orthogonal to the frame walk above, and deliberately so.  `#writes` speaks
 * about PARAMETERS; a global is written by name rather than passed, so it is
 * not a thing a frame can name or exclude.  Rather than teach wf_scan a second
 * vocabulary, this answers its own question over the same body and the verdict
 * is combined once, in wf_resolve_write_frames.
 *
 * Tri-state, because "I did not see a global write" and "I could not see" are
 * different answers and only the first may back a VERIFIED frame.
 *
 * What is NOT a channel here, and why -- both proven, not assumed:
 *
 *   - INLINE C.  A turmeric global emits as a C `static` whose name carries the
 *     binding id (`counter_1327`), and that suffix shifts with any unrelated
 *     edit earlier in the unit.  An inline-C body naming the source spelling
 *     (`counter = 99;`) is emitted verbatim and fails the C compile with
 *     "undeclared identifier" at the definition site.  So inline C cannot
 *     reach a turmeric global, and treating every inline-C callee as a
 *     possible global writer would poison most bodies for no reason.  (A
 *     sufficiently determined author could paste today's mangled spelling and
 *     have it work until the next edit; that is not a maintainable channel,
 *     and if they do it they get exactly the write they asked for.)
 *   - An EXTERN-C binding, for the same reason.
 *
 * What IS unvouchable, and answers WG_UNKNOWN:
 *
 *   - A call whose head does not resolve in the global scope: a local fn value,
 *     a typeclass method, or a builtin.  A builtin cannot write a user global
 *     (same argument as inline C), but a fn value CAN dispatch to a turmeric
 *     function that does, and this scan cannot tell the two apart from a bare
 *     symbol.  Conservative, per the file's standing rule that a frame
 *     over-declares rather than under-declares.
 *   - An indirect call, a place expression with an unreadable root, a scan that
 *     runs out of depth.
 *
 * Shadowing is resolved PESSIMISTICALLY: a `let` local spelled the same as a
 * global reads as a global write here, because the scan matches names rather
 * than resolved bindings.  That direction costs a verification, never a
 * missed one. */
typedef enum WgVerdict {
    WG_V_NO = 0,     /* no global write anywhere reachable */
    WG_V_UNKNOWN,    /* something could not be vouched for */
    WG_V_YES,        /* a definite global assignment */
} WgVerdict;

static WgVerdict wg_worse(WgVerdict a, WgVerdict b) { return a > b ? a : b; }

/* G2: WHICH globals a body writes, accumulated alongside the verdict.  G1 only
 * needed the yes/no; a frame that can NAME a global needs the set, so it can
 * ask whether every written global is covered.  `overflow` degrades coverage
 * to unanswerable rather than letting a truncated set read as "all covered". */
typedef struct WgSet {
    const Symbol *names[WF_MAX_FRAME_GLOBALS];
    uint32_t      n;
    bool          overflow;
} WgSet;

static void wg_set_add(WgSet *s, const Symbol *sym) {
    if (!s || !sym) return;
    for (uint32_t i = 0; i < s->n; i++) if (s->names[i] == sym) return;
    if (s->n >= WF_MAX_FRAME_GLOBALS) { s->overflow = true; return; }
    s->names[s->n++] = sym;
}

static void wg_set_union(WgSet *dst, const WgSet *src) {
    if (!dst || !src) return;
    if (src->overflow) dst->overflow = true;
    for (uint32_t i = 0; i < src->n; i++) wg_set_add(dst, src->names[i]);
}

static enum WritesGlobal wf_fn_writes_global(Elab *e, Binding *fn, WgSet *out);

/* True when `sym` names a mutable global -- i.e. a `set!` through it is
 * observable outside the assigning function.  A parameter of the enclosing
 * function shadows: turmeric passes by value, so `(set! p 5)` rebinds this
ature -- function's own slot, which is the same by-value argument wf_scan rests on. */
static bool wg_target_is_global(Elab *e, const Form *target,
                                Binding *const *params, uint32_t n_params) {
    if (!target || target->tag != F_SYM || !target->as.sym) return false;
    if (wf_param_index(target, params, n_params) >= 0) return false;
    Binding *b = scope_lookup(&e->global, target->as.sym);
    return b && b->is_global;
}

static WgVerdict wg_walk(Elab *e, const Form *f, Binding *const *params,
                         uint32_t n_params, uint32_t depth, WgSet *out) {
    if (!f) return WG_V_NO;
    if (depth >= WF_SCAN_MAX_DEPTH) return WG_V_UNKNOWN;
    if (f->tag == F_CBLOCK) return WG_V_NO;   /* cannot reach a global; see above */
    if (f->tag == F_SYM && f->as.sym) {
        const char *n = f->as.sym->name;
        if (strcmp(n, "set!") == 0 || strcmp(n, "swap!") == 0 ||
            strcmp(n, "reset!") == 0)
            return WG_V_UNKNOWN;              /* an assignment used as a value */
        return WG_V_NO;
    }
    if (f->tag != F_LIST && f->tag != F_VEC) return WG_V_NO;
    {
        const Form *mx = rt_macro_expansion(e, f);
        if (mx) return wg_walk(e, mx, params, n_params, depth, out);
    }
    if (f->as.list.len == 0) return WG_V_NO;

    WgVerdict v = WG_V_NO;

    /* A direct assignment. */
    if (f->as.list.len >= 2 &&
        (rt_sym_is(f->as.list.items[0], "set!") ||
         rt_sym_is(f->as.list.items[0], "swap!") ||
         rt_sym_is(f->as.list.items[0], "reset!"))) {
        const Form *target = f->as.list.items[1];
        if (target && target->tag == F_SYM) {
            if (wg_target_is_global(e, target, params, n_params)) {
                v = wg_worse(v, WG_V_YES);
                wg_set_add(out, target->as.sym);
            }
        } else {
            /* A place expression: `(set! (.n g) 9)` reaches whatever `g` names,
             * so it is a global write when its ROOT is a global. */
            bool opaque = false;
            const Form *root = f->as.list.items[1];
            uint32_t guard = 0;
            while (root && root->tag == F_LIST && root->as.list.len >= 2 &&
                   guard++ < WF_SCAN_MAX_DEPTH)
                root = root->as.list.items[root->as.list.len - 1];
            if (!root || root->tag != F_SYM) opaque = true;
            if (opaque) v = wg_worse(v, WG_V_UNKNOWN);
            else if (wg_target_is_global(e, root, params, n_params)) {
                v = wg_worse(v, WG_V_YES);
                wg_set_add(out, root->as.sym);
            }
        }
        for (uint32_t i = 2; i < f->as.list.len; i++)
            v = wg_worse(v, wg_walk(e, f->as.list.items[i], params, n_params,
                                    depth + 1, out));
        return v;
    }

    /* A call: consult the callee, whatever its arguments -- a call that
     * receives none of this function's parameters can still write a global,
     * which is exactly the gap that makes this a separate walk from wf_scan's
     * `passes_param` fast path. */
    if (f->tag == F_LIST && f->as.list.items[0]) {
        const Form *head_f = f->as.list.items[0];
        if (head_f->tag == F_SYM && head_f->as.sym) {
            if (!rt_sym_is(head_f, "set!") && !rt_sym_is(head_f, "swap!") &&
                !rt_sym_is(head_f, "reset!")) {
                if (wf_param_index(head_f, params, n_params) >= 0) {
                    /* Calling a function-typed PARAMETER: a higher-order call
                     * that dispatches to a body chosen by the caller, which
                     * this scan cannot name.  The common fn-value shape, and
                     * the one worth being conservative about. */
                    v = wg_worse(v, WG_V_UNKNOWN);
                } else {
                    Binding *callee = scope_lookup(&e->global, head_f->as.sym);
                    if (callee) {
                        WgSet cset = { { 0 }, 0, false };
                        enum WritesGlobal cw = wf_fn_writes_global(e, callee, &cset);
                        if (cw == WG_YES)          v = wg_worse(v, WG_V_YES);
                        else if (cw == WG_UNKNOWN) v = wg_worse(v, WG_V_UNKNOWN);
                        wg_set_union(out, &cset);
                    }
                    /* Otherwise: a special form (`if`, `let`, `do` -- syntax,
                     * whose operands the generic recursion below visits), a
                     * builtin (compiler-emitted C written against the runtime,
                     * which cannot name a user global for the same reason
                     * inline C cannot), or a field accessor.  None is a channel
                     * to a global, so none is a reason to give up.
                     *
                     * RESIDUAL GAP, stated rather than papered over: a `let`-
                     * bound closure invoked by its own name lands here too, and
                     * if it writes a global this walk misses it.  Answering
                     * UNKNOWN instead would be sound, but it would also fire on
                     * every `if` and every field read -- every special form
                     * shares this shape -- and downgrade essentially every
                     * frame, which trades a latent hole for a useless feature.
                     * Following indirect dispatch is G2's problem, where a fn
                     * value's own frame becomes a thing that can be asked
                     * about. */
                }
            }
        } else if (head_f->tag == F_LIST) {
            v = wg_worse(v, WG_V_UNKNOWN);    /* indirect call */
        }
    }

    for (uint32_t i = 0; i < f->as.list.len; i++)
        v = wg_worse(v, wg_walk(e, f->as.list.items[i], params, n_params,
                                depth + 1, out));
    return v;
}

/* Memoized per-function answer.  Recursion through a cycle answers WG_NO for
 * the back edge: a cycle contributes no global write of its own, and any real
 * write inside it is seen on the forward edge that reached it. */
static enum WritesGlobal wf_fn_writes_global(Elab *e, Binding *fn, WgSet *out) {
    if (!fn) return WG_UNKNOWN;
    if (fn->writes_global == WG_IN_PROGRESS) return WG_NO;
    if (fn->writes_global != WG_UNCOMPUTED) {
        /* G2: replay the memoized set for the caller unioning into `out`. */
        if (out) {
            for (uint32_t i = 0; i < fn->n_writes_globals_seen; i++)
                wg_set_add(out, fn->writes_globals_seen[i]);
            if (fn->writes_globals_overflow) out->overflow = true;
        }
        return fn->writes_global;
    }

    /* Find the body registered for this function.  A binding with no site is a
     * global `def`, an extern-c declaration, or a builtin -- none of which can
     * write a turmeric global by name. */
    const WriteFrameSite *site = NULL;
    for (uint32_t i = 0; i < e->n_wf_frame_sites; i++) {
        if (e->wf_frame_sites[i].fn == fn) { site = &e->wf_frame_sites[i]; break; }
    }
    if (!site || !site->defn_form) { fn->writes_global = WG_NO; return WG_NO; }

    fn->writes_global = WG_IN_PROGRESS;
    WgSet seen = { { 0 }, 0, false };
    WgVerdict v = WG_V_NO;
    const Form *d = site->defn_form;
    for (uint32_t bi = site->body_start; bi < d->as.list.len; bi++)
        v = wg_worse(v, wg_walk(e, d->as.list.items[bi], site->params,
                                site->n_params, 0, &seen));
    fn->writes_global = (v == WG_V_YES)     ? WG_YES
                      : (v == WG_V_UNKNOWN) ? WG_UNKNOWN
                                            : WG_NO;
    /* Memoize the set beside the verdict so a second caller replays it rather
     * than re-walking, and so the coverage check has it without a second pass. */
    fn->writes_globals_overflow = seen.overflow;
    fn->n_writes_globals_seen   = seen.n;
    if (seen.n > 0) {
        fn->writes_globals_seen = (const Symbol **)arena_alloc(
            e->arena, seen.n * sizeof(const Symbol *));
        for (uint32_t i = 0; i < seen.n; i++)
            fn->writes_globals_seen[i] = seen.names[i];
    }
    if (out) wg_set_union(out, &seen);
    return fn->writes_global;
}

/* The global half of a frame verdict, kept separate from wf_scan's parameter
 * half because they speak different vocabularies (mutable-globals-plan §4.2).
 *
 * G2: the frame can NAME globals, so the question is coverage, exactly as for
 * parameters:
 *   - every written global declared  -> VERIFIED (the frame holds)
 *   - a written global not declared  -> EXCEEDED, and `*uncovered` names it
 *   - cannot tell (UNKNOWN/overflow) -> UNVERIFIED
 *
 * Declared-but-never-written is deliberately fine: a frame is an UPPER bound on
 * what the body may write, the same reading `#writes [a]` already has for a
 * parameter the body happens not to touch. */
static WfVerdict wf_global_verdict(Elab *e, Binding *fn, const Symbol **uncovered) {
    if (uncovered) *uncovered = NULL;
    WgSet seen = { { 0 }, 0, false };
    enum WritesGlobal g = wf_fn_writes_global(e, fn, &seen);
    if (g == WG_NO)      return WF_VERIFIED;
    if (g == WG_UNKNOWN) return WF_UNVERIFIED;
    if (seen.overflow)   return WF_UNVERIFIED;   /* coverage unanswerable */
    for (uint32_t i = 0; i < seen.n; i++) {
        bool covered = false;
        for (uint32_t j = 0; j < fn->n_writes_globals_declared; j++) {
            if (fn->writes_globals_declared[j] == seen.names[i]) { covered = true; break; }
        }
        if (!covered) {
            if (uncovered) *uncovered = seen.names[i];
            return WF_EXCEEDED;
        }
    }
    return WF_VERIFIED;
}

/* =========================================================================
 * WF2, the checked half: verify a declared `#writes` frame against its body.
 *
 * This walks the ELABORATED `Expr` body, not the source `Form` tree, and that
 * is the whole point.  The frame question -- "can this form write a parameter
 * my frame does not name?" -- is semantic, and on a Form tree it is not
 * answerable: a head symbol is just a name, so the walk had to resolve every
 * head through the global scope and treat everything else as an opaque callee.
 * `(.n a)` (a field READ), `(if ...)`, `(do ...)` and `(+ p 1)` are none of
 * them global bindings, so all four declined, and a frame over any body doing
 * ordinary work was silently UNVERIFIED
 * (docs/reported/writes-frame-walk-treats-every-head-as-a-callee.md).
 *
 * On `Expr` each of those is a distinct node kind and the guessing stops.  The
 * decisive case is the dotted head: `(.f x)` is EX_GET_FIELD when `f` is a
 * field (a read, no write channel) and EX_CALL when it dispatches a method
 * (analyzed like any other call).  A Form-level walk cannot tell the two apart
 * without the receiver's type, which is exactly why admitting all dotted heads
 * there would have been unsound.
 *
 * The `#reads` side of this file already works this way -- see rf_scan -- and
 * this mirrors it deliberately, down to the budget and the fixed-point pass.
 *
 * Three write channels are checked, unchanged from the Form-era walk:
 *   1. a direct `set!` through a parameter place (EX_SET_FIELD / EX_SET_DEREF);
 *   2. an argument passed to a `^mut` parameter of a callee;
 *   3. a callee's own declared+CHECKED `#writes` frame, mapped back through the
 *      argument list to this function's parameters.
 *
 * A bare-symbol `(set! p v)` (EX_SET, whose target is a Binding) is NOT a
 * write: Turmeric passes by value, so it rebinds this function's own slot and
 * the caller cannot observe it.  WF3's "a plain symbol target cannot alias"
 * rule rests on the same argument, so the two must agree.
 * ========================================================================= */

/* Where does this expression's storage live?  Tri-state on purpose: "not one
 * of ours" and "cannot tell" are different answers, and collapsing them is how
 * a checker becomes either useless or unsound. */
typedef enum {
    WR_NOT_OURS = 0,   /* a literal, a local, a global: not in this frame's vocabulary */
    WR_PARAM,          /* definitely rooted at frame parameter *out_pi */
    WR_UNKNOWN,        /* could not tell -- may or may not alias a parameter */
} WfRootKind;

static WfRootKind wf_expr_root(const Expr *x, Binding *const *params,
                               uint32_t n_params, uint32_t depth, int *out_pi) {
    if (!x) return WR_UNKNOWN;
    if (depth >= WF_SCAN_MAX_DEPTH) return WR_UNKNOWN;
    switch (x->kind) {
    case EX_NIL_LIT: case EX_BOOL_LIT: case EX_INT_LIT:
    case EX_FLOAT_LIT: case EX_CSTR_LIT:
        return WR_NOT_OURS;              /* a value, not a place */
    case EX_VAR: {
        const Binding *b = x->as.var.binding;
        if (!b) return WR_UNKNOWN;
        if (b->is_param)
            for (uint32_t i = 0; i < n_params && i < 32; i++)
                if (params[i] == b) { *out_pi = (int)i; return WR_PARAM; }
        /* A local, a global, or an enclosing fn's parameter reached through a
         * closure: none of them is a slot in THIS frame. */
        return b->is_param ? WR_UNKNOWN : WR_NOT_OURS;
    }
    /* Value-shaping wrappers: the storage is the inner expression's. */
    case EX_ASCRIBE:     return wf_expr_root(x->as.ascribe_.inner, params, n_params, depth + 1, out_pi);
    case EX_CAST:        return wf_expr_root(x->as.cast_.expr, params, n_params, depth + 1, out_pi);
    case EX_REINTERPRET: return wf_expr_root(x->as.reinterpret_.expr, params, n_params, depth + 1, out_pi);
    /* An interior place: `(.f x)`'s storage is inside x's. */
    case EX_GET_FIELD:   return wf_expr_root(x->as.get_field_.struct_expr, params, n_params, depth + 1, out_pi);
    case EX_DO:
        if (x->as.do_.n == 0) return WR_UNKNOWN;
        return wf_expr_root(x->as.do_.items[x->as.do_.n - 1], params, n_params, depth + 1, out_pi);
    case EX_BUILTIN: {
        if (!x->as.builtin.spec) return WR_UNKNOWN;
        BuiltinShape sh = x->as.builtin.spec->shape;
        /* Address math and raw loads carry their pointer's provenance. */
        if ((sh == BS_PTR_ARITH || sh == BS_PTR_DEREF || sh == BS_ARRAY_GET_UNCHECKED)
            && x->as.builtin.n >= 1)
            return wf_expr_root(x->as.builtin.args[0], params, n_params, depth + 1, out_pi);
        /* A pure builtin computes a fresh value; it cannot alias its operands.
         * This is what makes `(+ p 1)` harmless, where the Form-era walk read
         * it as handing `p` onward. */
        if (rt_builtin_shape_pure(sh)) return WR_NOT_OURS;
        return WR_UNKNOWN;
    }
    default:
        /* A call result, a constructor, a match: it may be an interior pointer
         * derived from a parameter, and nothing here can rule that out. */
        return WR_UNKNOWN;
    }
}

/* Does this builtin write through one of its arguments?  Allowlist-shaped like
 * rf_builtin_reads_nothing: a shape in neither list is UNKNOWN, never assumed
 * harmless. */
static bool wf_builtin_writes_arg(BuiltinShape s) {
    switch (s) {
    case BS_PTR_WRITE: case BS_ARRAY_SET_UNCHECKED:
    case BS_RAW_MEMSET: case BS_RAW_MEMCPY:
    case BS_RAW_FREE:   case BS_RAW_REALLOC:
        return true;
    default:
        return false;
    }
}
static bool wf_builtin_writes_nothing(BuiltinShape s) {
    if (rt_builtin_shape_pure(s)) return true;
    switch (s) {
    /* Effects that are real but reach no argument's storage. */
    case BS_PRINTLN_INT:  case BS_PRINTLN_FLOAT: case BS_PRINTLN_BOOL:
    case BS_PRINTLN_CSTR: case BS_PRINTLN_UINT:  case BS_PRINTLN_FLOAT32:
    case BS_PTR_ARITH:    case BS_PTR_DEREF:     case BS_ARRAY_GET_UNCHECKED:
    case BS_RAW_MALLOC:   case BS_DLOPEN:        case BS_DLSYM:
    case BS_DLCLOSE:      case BS_UNSAFE_CAST:   case BS_REINTERPRET:
    case BS_TRANSMUTE:
        return true;
    default:
        return false;
    }
}

/* The verdict for writing through `x`, whose storage the caller has determined
 * is written. */
static WfVerdict wf_write_through(const Expr *x, Binding *const *params,
                                  uint32_t n_params, uint32_t mask,
                                  const Expr **witness) {
    int pi = -1;
    switch (wf_expr_root(x, params, n_params, 0, &pi)) {
    case WR_NOT_OURS: return WF_VERIFIED;
    case WR_UNKNOWN:  return WF_UNVERIFIED;
    case WR_PARAM:
        if (pi >= 0 && (mask & ((uint32_t)1u << pi))) return WF_VERIFIED;
        if (witness && !*witness) *witness = x;
        return WF_EXCEEDED;
    }
    return WF_UNVERIFIED;
}

static WfVerdict wf_scan(const Expr *x, Binding *const *params,
                         uint32_t n_params, uint32_t mask, uint32_t *budget,
                         const Expr **witness) {
#define WFS(sub) wf_scan((sub), params, n_params, mask, budget, witness)
    if (!x) return WF_VERIFIED;
    if (*budget == 0) return WF_UNVERIFIED;
    (*budget)--;
    switch (x->kind) {
    case EX_NIL_LIT: case EX_BOOL_LIT: case EX_INT_LIT:
    case EX_FLOAT_LIT: case EX_CSTR_LIT:
    case EX_VAR:
        return WF_VERIFIED;              /* a read is not a write */

    /* An inline-C body is opaque by construction.  This is the trust boundary
     * `#reads` already draws and the plan keeps: checked-when-checkable, never
     * checked-by-pretending. */
    case EX_INLINE_C:
        return WF_UNVERIFIED;

    /* --- channel 1: a direct assignment ------------------------------- */
    case EX_SET:
        /* Target is a BINDING, i.e. the bare-symbol form: a local rebind,
         * in-frame by the by-value argument above, whether or not it happens
         * to spell a parameter. */
        return WFS(x->as.set_.value);
    case EX_SET_FIELD:
        return wf_worse(wf_write_through(x->as.set_field_.receiver, params,
                                         n_params, mask, witness),
                        wf_worse(WFS(x->as.set_field_.receiver),
                                 WFS(x->as.set_field_.value)));
    case EX_SET_DEREF:
        return wf_worse(wf_write_through(x->as.set_deref_.ref, params,
                                         n_params, mask, witness),
                        wf_worse(WFS(x->as.set_deref_.ref),
                                 WFS(x->as.set_deref_.value)));

    case EX_GET_FIELD:
        /* A READ.  The whole reason this walk moved to the Expr tree. */
        return WFS(x->as.get_field_.struct_expr);

    case EX_BUILTIN: {
        if (!x->as.builtin.spec) return WF_UNVERIFIED;
        BuiltinShape sh = x->as.builtin.spec->shape;
        WfVerdict v = WF_VERIFIED;
        if (wf_builtin_writes_arg(sh)) {
            v = (x->as.builtin.n >= 1)
                    ? wf_write_through(x->as.builtin.args[0], params, n_params,
                                       mask, witness)
                    : WF_UNVERIFIED;
        } else if (!wf_builtin_writes_nothing(sh)) {
            v = WF_UNVERIFIED;           /* unclassified shape: cannot vouch */
        }
        for (uint32_t i = 0; i < x->as.builtin.n; i++)
            v = wf_worse(v, WFS(x->as.builtin.args[i]));
        return v;
    }

    /* --- channels 2 and 3: a call ------------------------------------- */
    case EX_CALL: {
        WfVerdict v = WF_VERIFIED;
        Binding *callee = x->as.call_.fn_binding;
        if (x->as.call_.fn_expr || x->as.call_.is_poly_call || !callee) {
            /* Dispatch this walk cannot name: assume it may write every slot
             * it is handed, so any argument that could be one of ours (or that
             * cannot be ruled out) leaves the body unvouchable. */
            for (uint32_t i = 0; i < x->as.call_.n_args; i++) {
                int pi = -1;
                WfRootKind k = wf_expr_root(x->as.call_.args[i], params,
                                            n_params, 0, &pi);
                if (k != WR_NOT_OURS) { v = WF_UNVERIFIED; break; }
            }
        } else {
            /* `source_fn_def` is the defn's own parameter list, which is where
             * the `^mut`/`^borrow` modes live -- the binding's TY_FN carries
             * only kinds. */
            const FnDef *fd = callee->source_fn_def;
            uint32_t cn = fd ? fd->n_params : 0;
            for (uint32_t i = 0; i < x->as.call_.n_args; i++) {
                int pi = -1;
                WfRootKind k = wf_expr_root(x->as.call_.args[i], params,
                                            n_params, 0, &pi);
                if (k == WR_NOT_OURS) continue;   /* cannot reach this frame */
                bool callee_writes_slot;
                if (callee->writes_declared) {
                    /* Channel 3: the callee said what it writes.  Only a
                     * CHECKED frame may narrow us -- a trusted one is a
                     * promise, and a chain of promises is what the checked
                     * tier exists to replace. */
                    if (!callee->writes_checked) { v = wf_worse(v, WF_UNVERIFIED); continue; }
                    callee_writes_slot =
                        i < WF_MAX_FRAME_PARAMS &&
                        (callee->writes_param_mask & ((uint32_t)1u << i)) != 0;
                } else if (fd && i < cn && fd->params[i]) {
                    /* Channel 2: no declared frame, so fall back to the
                     * parameter MODE, which is itself a declaration.
                     *
                     *   ^borrow -- safe, and not merely assumed safe: the
                     *     borrow checker already forbids writing through it.
                     *   ^mut    -- a DEFINITE write channel.
                     *   anything else -- UNVERIFIED.  A plain by-value slot
                     *     may still be an opaque handle the callee writes
                     *     through, which is what this scan cannot see. */
                    if (fd->params[i]->is_borrow) continue;
                    if (!fd->params[i]->is_mut) { v = wf_worse(v, WF_UNVERIFIED); continue; }
                    callee_writes_slot = true;
                } else {
                    v = wf_worse(v, WF_UNVERIFIED);
                    continue;
                }
                if (!callee_writes_slot) continue;
                if (k == WR_UNKNOWN) { v = wf_worse(v, WF_UNVERIFIED); continue; }
                if (pi >= 0 && !(mask & ((uint32_t)1u << pi))) {
                    if (witness && !*witness) *witness = x;
                    v = wf_worse(v, WF_EXCEEDED);
                }
            }
        }
        for (uint32_t i = 0; i < x->as.call_.n_args; i++)
            v = wf_worse(v, WFS(x->as.call_.args[i]));
        return v;
    }

    /* --- control flow: the writes are in the subexpressions ------------ */
    case EX_IF:
        return wf_worse(WFS(x->as.if_.cond),
                        wf_worse(WFS(x->as.if_.then_), WFS(x->as.if_.else_or_null)));
    case EX_DO: {
        WfVerdict v = WF_VERIFIED;
        for (uint32_t i = 0; i < x->as.do_.n; i++)
            v = wf_worse(v, WFS(x->as.do_.items[i]));
        return v;
    }
    case EX_LET: case EX_LETREC: {
        WfVerdict v = WFS(x->as.let_.body);
        for (uint32_t i = 0; i < x->as.let_.n; i++)
            v = wf_worse(v, WFS(x->as.let_.bindings[i].init));
        return v;
    }
    case EX_WHILE:
        return wf_worse(WFS(x->as.while_.cond), WFS(x->as.while_.body));
    case EX_MATCH: {
        WfVerdict v = WFS(x->as.match_.scrutinee);
        for (uint32_t i = 0; i < x->as.match_.n_arms; i++) {
            v = wf_worse(v, WFS(x->as.match_.arms[i].guard));
            v = wf_worse(v, WFS(x->as.match_.arms[i].body));
        }
        return v;
    }
    case EX_ASCRIBE:     return WFS(x->as.ascribe_.inner);
    case EX_CAST:        return WFS(x->as.cast_.expr);
    case EX_REINTERPRET: return WFS(x->as.reinterpret_.expr);
    case EX_RETURN:      return WFS(x->as.return_.value);
    case EX_DEFER:       return WFS(x->as.defer_.body);
    case EX_HANDLE: {
        const HandleExpr *h = x->as.handle_.handle;
        if (!h) return WF_UNVERIFIED;
        WfVerdict v = WFS(h->body);
        for (uint8_t i = 0; i < h->n_cases; i++)
            v = wf_worse(v, WFS(h->cases[i].body));
        return v;
    }
    case EX_RESUME: {
        const ResumeExpr *r = x->as.resume_.resume;
        if (!r) return WF_UNVERIFIED;
        return wf_worse(WFS(r->k), WFS(r->value));
    }
    default:
        return WF_UNVERIFIED;   /* unmodeled: "could not see", never clean */
    }
#undef WFS
}

/* The frame-only verdict for one site: the elaborated body, or UNVERIFIED when
 * there is none to read (an inline-C-only defn, an extern declaration). */
static WfVerdict wf_site_verdict(const WriteFrameSite *s, const Expr **witness) {
    const FnDef *fd = s->fn ? s->fn->source_fn_def : NULL;
    if (!fd || !fd->body) return WF_UNVERIFIED;
    uint32_t budget = 65536;   /* same bound rf_scan uses */
    return wf_scan(fd->body, s->params, s->n_params,
                   s->fn->writes_param_mask, &budget, witness);
}

void wf_note_frame_site(Elab *e, Binding *fn, Binding **params, uint32_t n_params,
                        const Form *defn_form, uint32_t body_start,
                        const Form *annot) {
    if (!e || !fn) return;
    if (e->n_wf_frame_sites == e->cap_wf_frame_sites) {
        uint32_t ncap = e->cap_wf_frame_sites ? e->cap_wf_frame_sites * 2 : 8;
        WriteFrameSite *nb = (WriteFrameSite *)arena_alloc(
            e->arena, ncap * sizeof(WriteFrameSite));
        if (e->wf_frame_sites)
            memcpy(nb, e->wf_frame_sites,
                   e->n_wf_frame_sites * sizeof(WriteFrameSite));
        e->wf_frame_sites     = nb;
        e->cap_wf_frame_sites = ncap;
    }
    WriteFrameSite *s = &e->wf_frame_sites[e->n_wf_frame_sites++];
    s->fn         = fn;
    s->params     = params;
    s->n_params   = n_params;
    s->defn_form  = defn_form;
    s->body_start = body_start;
    s->annot      = annot;
    s->from_stdlib = e->in_stdlib_load || g_turi_stdlib_preload;
}

void wf_note_image_cache_root(Elab *e, const Symbol *root, Span span) {
    if (!e || !root) return;
    if (e->n_image_cache_roots == e->cap_image_cache_roots) {
        uint32_t ncap = e->cap_image_cache_roots ? e->cap_image_cache_roots * 2 : 4;
        ImageCacheRoot *nb = (ImageCacheRoot *)arena_alloc(
            e->arena, ncap * sizeof(ImageCacheRoot));
        if (e->image_cache_roots)
            memcpy(nb, e->image_cache_roots,
                   e->n_image_cache_roots * sizeof(ImageCacheRoot));
        e->image_cache_roots     = nb;
        e->cap_image_cache_roots = ncap;
    }
    ImageCacheRoot *r = &e->image_cache_roots[e->n_image_cache_roots++];
    r->root = root;
    r->span = span;
}

/* AI3.1 (application-image-dumps-plan): TUR-W0706.  The image written by
 * with-image-cache-after-init is the CONTINUATION that runs `loop`; a
 * top-level global that `init` (or anything it calls) writes is not in it
 * unless a `defimage-global` declared it -- and init is exactly the code a
 * warm start skips, so the write is silently gone on the run that needed it.
 * A declared image global leaves a `<name>/image-deser` defn behind (that is
 * what the macro expands to), so registration is a global-scope lookup.
 * Reuses the G1 global-write walk, which is transitive over named callees
 * and answers UNKNOWN for indirect calls (no warning: nothing was seen). */
void wf_lint_image_globals(Elab *e) {
    if (!e || e->n_image_cache_roots == 0) return;
    for (uint32_t i = 0; i < e->n_image_cache_roots; i++) {
        const ImageCacheRoot *r = &e->image_cache_roots[i];
        Binding *fn = scope_lookup(&e->global, r->root);
        if (!fn) continue;
        WgSet seen = { { 0 }, 0, false };
        (void)wf_fn_writes_global(e, fn, &seen);
        for (uint32_t gi = 0; gi < seen.n; gi++) {
            const Symbol *g = seen.names[gi];
            char probe[512];
            snprintf(probe, sizeof probe, "%s/image-deser", g->name);
            const Symbol *ps = symtab_intern(e->st, strslice(probe, (uint32_t)strlen(probe)));
            if (scope_lookup(&e->global, ps)) continue;
            diag_emit_with_code(DIAG_WARNING, r->span,
                                TUR_W0706_IMAGE_GLOBAL_UNREGISTERED,
                                "`%s` writes the global `%s`, which is not an "
                                "image global: the image holds the captured "
                                "continuation, not the heap, and init is skipped "
                                "on a warm start, so the write is silently absent "
                                "after load -- declare it with (defimage-global %s "
                                ":T initial) and track it at the top of main, or "
                                "thread the value through the captured continuation",
                                r->root->name, g->name, g->name);
        }
    }
}

void wf_resolve_write_frames(Elab *e) {
    if (!e || e->n_wf_frame_sites == 0) return;
    /* Iterate to a fixed point: channel 3 consults a callee's `writes_checked`,
     * which a later pass may raise, so one linear sweep would under-verify a
     * caller purely because its callee had not been visited yet.  Verdicts only
     * ever move from unverified to verified, so this terminates; the bound is
     * belt-and-braces against a future non-monotone edit.  Definite violations
     * are reported once, on the LAST round, so a caller cannot be blamed twice
     * for the same write. */
    uint32_t rounds = e->n_wf_frame_sites + 1;
    for (uint32_t round = 0; round < rounds; round++) {
        bool progress = false;
        for (uint32_t i = 0; i < e->n_wf_frame_sites; i++) {
            WriteFrameSite *s = &e->wf_frame_sites[i];
            /* G1: every defn is registered so the global walk can reach any
             * callee's body, but only a DECLARED frame has anything to check. */
            if (!s->fn || !s->fn->writes_declared || s->fn->writes_checked) continue;
            const Expr *witness = NULL;
            WfVerdict v = wf_site_verdict(s, &witness);
            if (v == WF_VERIFIED)
                v = wf_worse(v, wf_global_verdict(e, s->fn, NULL));
            if (v == WF_VERIFIED) { s->fn->writes_checked = true; progress = true; }
        }
        if (!progress) break;
    }
    /* Final sweep: report the frames that are definitely exceeded. */
    for (uint32_t i = 0; i < e->n_wf_frame_sites; i++) {
        WriteFrameSite *s = &e->wf_frame_sites[i];
        if (!s->fn || !s->fn->writes_declared || s->fn->writes_checked) continue;
        const Expr *witness = NULL;
        WfVerdict v = wf_site_verdict(s, &witness);
        const Form *d = s->defn_form;
        /* G1 does NOT suppress an EXCEEDED report: writing a global is not an
         * excuse for also writing outside a declared frame. */
        const Symbol *uncovered = NULL;
        if (v != WF_EXCEEDED) {
            /* G2: an undeclared global write is the same kind of mistake as a
             * write outside the parameter frame, so it gets the same code --
             * but a message that names the global, because "widen the frame"
             * is not actionable if you cannot see what to widen it with. */
            if (wf_global_verdict(e, s->fn, &uncovered) != WF_EXCEEDED) continue;
        }
        Span at = witness ? witness->span
                          : (s->annot ? s->annot->span : d->span);
        if (uncovered) {
            diag_emit_with_code(DIAG_ERROR, at, TUR_E0382_WRITES_FRAME_EXCEEDED,
                                "'%s' writes the global '%s', which its `#writes` "
                                "frame does not name",
                                s->fn->name ? s->fn->name->name : "<fn>",
                                uncovered->name);
        } else {
            diag_emit_with_code(DIAG_ERROR, at, TUR_E0382_WRITES_FRAME_EXCEEDED,
                                "this writes outside the `#writes` frame declared by "
                                "'%s'",
                                s->fn->name ? s->fn->name->name : "<fn>");
        }
        if (s->annot)
            diag_emit(DIAG_NOTE, s->annot->span,
                      uncovered
                        ? "the declared frame is here; add the global to it, or "
                          "stop writing it"
                        : "the declared frame is here; widen it to what the body "
                          "writes, or narrow the body");
    }

    /* G1: --dump-write-frames.  One line per DECLARED frame, in source order.
     * `global=` is the answer that can downgrade VERIFIED to UNVERIFIED, kept
     * as its own column so a fixture can tell "the frame did not hold" from
     * "the frame held but the body writes a global". */
    if (g_dump_write_frames) {
        for (uint32_t i = 0; i < e->n_wf_frame_sites; i++) {
            WriteFrameSite *s = &e->wf_frame_sites[i];
            if (!s->fn || !s->fn->writes_declared) continue;
            /* The stdlib's own frames (the Vec mutators' `#writes [v]`) are
             * not what a dump of this program is asking about, and the
             * interpreter elaborates the stdlib more than once, which would
             * print them once per load. */
            if (s->from_stdlib) continue;
            /* Recompute the FRAME-ONLY verdict so the two questions stay
             * separable in the output: a fixture needs to tell "the frame did
             * not hold" from "the frame held but the body writes a global". */
            const Expr *w = NULL;
            WfVerdict fv = wf_site_verdict(s, &w);
            enum WritesGlobal g = wf_fn_writes_global(e, s->fn, NULL);
            printf("write-frame %s: %s mask=0x%x frame=%s global=%s",
                   s->fn->name ? s->fn->name->name : "<fn>",
                   s->fn->writes_checked ? "VERIFIED" : "UNVERIFIED",
                   s->fn->writes_param_mask,
                   fv == WF_VERIFIED ? "VERIFIED"
                 : fv == WF_EXCEEDED ? "EXCEEDED"
                                     : "UNVERIFIED",
                   g == WG_NO      ? "NO"
                 : g == WG_YES     ? "YES"
                 : g == WG_UNKNOWN ? "UNKNOWN"
                                   : "?");
            /* G2: the declared global frame, so a fixture can tell a covered
             * write from an unchecked one. */
            if (s->fn->n_writes_globals_declared > 0) {
                printf(" declared=[");
                for (uint32_t gi = 0; gi < s->fn->n_writes_globals_declared; gi++)
                    printf("%s%s", gi ? " " : "",
                           s->fn->writes_globals_declared[gi]->name);
                printf("]");
            }
            printf("\n");
        }
        fflush(stdout);
    }
}

/* ------------------------------------------------------------------------- *
 * R4 slice 2 (trusted-refinement-claims-plan): the read-frame VERIFICATION
 * walk -- the footprint walk proper, in its first honest form.
 *
 * Slice 1 above is the EVIDENCE half: it may only speak when it saw a read
 * the frame omits, and its silence means nothing.  This walk is the other
 * half: it decides whether silence was "saw the whole body and every read of
 * mutable state attributes to a frame-named parameter" (WF_VERIFIED, stamped
 * as Binding.reads_checked) or "could not see" (WF_UNVERIFIED, no stamp, no
 * diagnostic -- the frame simply stays at the trusted tier it is today).
 * WF_EXCEEDED can also fall out (the same finding slice 1 warns about,
 * possibly reached through a verified callee's frame); it is NOT re-reported
 * here -- TUR-W0383 and the checked-reads refusal own that -- it only blocks
 * the VERIFIED stamp.
 *
 * The discipline is the tri-state the plan required before silence could
 * back anything: every expression kind is either modeled (children walked,
 * read leaves attributed) or answers WF_UNVERIFIED.  Inline C, `perform`,
 * indirect and poly calls, and any unmodeled form are UNVERIFIED, never
 * quietly clean.  Reuses WfVerdict/wf_worse from the WF2 write walk: the
 * lattice is the same, only the vocabulary (reads, not writes) differs. */

/* Attribute a read's chased root.  VERIFIED = stable or framed state;
 * EXCEEDED = mutable state the frame omits (an unframed parameter, or a
 * mutable global); UNVERIFIED = a local or unknown root, whose provenance
 * this walk does not track. */
static WfVerdict rf_root_verdict(const Binding *root, Binding **params,
                                 uint32_t n_params, uint64_t mask) {
    if (!root) return WF_UNVERIFIED;
    if (root->is_param) {
        for (uint32_t i = 0; i < n_params && i < 64; i++) {
            if (params[i] != root) continue;
            return (mask & (UINT64_C(1) << i)) ? WF_VERIFIED : WF_EXCEEDED;
        }
        return WF_UNVERIFIED;   /* an enclosing fn's param, via a closure */
    }
    if (root->is_global)
        return root->is_mut ? WF_EXCEEDED : WF_VERIFIED;
    return WF_UNVERIFIED;
}

/* rf_root_verdict plus witness capture: the FIRST root that answers
 * EXCEEDED is recorded so the evidence sweep can name the omitted
 * parameter (or global) in TUR-W0383. */
static WfVerdict rf_attr(const Binding *root, Binding **params,
                         uint32_t n_params, uint64_t mask,
                         const Binding **witness) {
    WfVerdict v = rf_root_verdict(root, params, n_params, mask);
    if (v == WF_EXCEEDED && witness && !*witness) *witness = root;
    return v;
}

/* Builtins that read no program-visible mutable state: pure computations,
 * output/free/write-only memory ops, and value-reshaping casts.  Everything
 * else -- RAW_MEMCPY reads its source, the dl* family reaches outside the
 * program, and any shape this list has not vetted -- answers UNVERIFIED. */
static bool rf_builtin_reads_nothing(BuiltinShape s) {
    if (rt_builtin_shape_pure(s)) return true;
    switch (s) {
    case BS_PTR_ARITH:                                    /* address math */
    case BS_PRINTLN_INT:  case BS_PRINTLN_FLOAT: case BS_PRINTLN_BOOL:
    case BS_PRINTLN_CSTR: case BS_PRINTLN_UINT:  case BS_PRINTLN_FLOAT32:
    case BS_PREFIX_UNARY_FREE:
    case BS_PTR_WRITE:    case BS_ARRAY_SET_UNCHECKED:    /* write-only */
    case BS_RAW_MALLOC:   case BS_RAW_FREE:
    case BS_RAW_REALLOC:  case BS_RAW_MEMSET:
    case BS_UNSAFE_CAST:  case BS_REINTERPRET: case BS_TRANSMUTE:
        return true;
    default:
        return false;
    }
}

static WfVerdict rf_scan(const Expr *x, Binding **params, uint32_t n_params,
                         uint64_t mask, uint32_t *budget,
                         const Binding **witness) {
#define RFS(sub) rf_scan((sub), params, n_params, mask, budget, witness)
    if (!x) return WF_VERIFIED;
    if (*budget == 0) return WF_UNVERIFIED;
    (*budget)--;
    switch (x->kind) {
    case EX_NIL_LIT: case EX_BOOL_LIT: case EX_INT_LIT:
    case EX_FLOAT_LIT: case EX_CSTR_LIT:
        return WF_VERIFIED;

    case EX_VAR: {
        const Binding *b = x->as.var.binding;
        if (!b) return WF_UNVERIFIED;
        /* A mutable GLOBAL read is the slice-1 finding.  A mutable LOCAL is
         * per-call state -- fresh each invocation, so it cannot differ
         * between two calls with equal arguments -- and a by-value
         * parameter read is the stability the whole feature rests on. */
        if (b->is_mut && b->is_global) {
            if (witness && !*witness) *witness = b;
            return WF_EXCEEDED;
        }
        return WF_VERIFIED;
    }

    /* The unwalkables.  Each is the reason a frame stays TRUSTED today. */
    case EX_INLINE_C:
    case EX_PERFORM:
        return WF_UNVERIFIED;

    case EX_GET_FIELD: {
        const Expr *recv = x->as.get_field_.struct_expr;
        WfVerdict v = WF_VERIFIED;
        if (recv) {
            switch (recv->type.kind) {
            case TY_REF: case TY_RC: case TY_PTR_VOID:
            case TY_REF_IMMUT: case TY_REF_MUT:
                /* An aliasable receiver: the read is of mutable state and
                 * must attribute. */
                v = rf_attr(reads_read_root(recv), params, n_params,
                            mask, witness);
                break;
            default: break;   /* by-value receiver: as stable as its root */
            }
        }
        return wf_worse(v, RFS(recv));
    }

    case EX_BUILTIN: {
        if (!x->as.builtin.spec) return WF_UNVERIFIED;
        BuiltinShape sh = x->as.builtin.spec->shape;
        WfVerdict v;
        if (sh == BS_PTR_DEREF || sh == BS_ARRAY_GET_UNCHECKED) {
            /* A raw-memory load: attribute its pointer's root. */
            v = (x->as.builtin.n >= 1)
                    ? rf_attr(reads_read_root(x->as.builtin.args[0]),
                              params, n_params, mask, witness)
                    : WF_UNVERIFIED;
        } else {
            v = rf_builtin_reads_nothing(sh) ? WF_VERIFIED : WF_UNVERIFIED;
        }
        for (uint32_t i = 0; i < x->as.builtin.n; i++)
            v = wf_worse(v, RFS(x->as.builtin.args[i]));
        return v;
    }

    case EX_CALL: {
        WfVerdict v = WF_VERIFIED;
        if (x->as.call_.fn_expr || x->as.call_.is_poly_call) {
            v = WF_UNVERIFIED;    /* dispatch this walk cannot name */
        } else {
            Binding *callee = x->as.call_.fn_binding;
            if (callee && rt_binding_is_pure(callee)) {
                /* Pure: reads nothing mutable, whatever it is passed. */
            } else if (callee && callee->reads_params_mask != 0 &&
                       callee->reads_checked) {
                /* A VERIFIED callee frame is an upper bound on its mutable
                 * reads, so map each framed callee parameter to the root of
                 * the argument feeding it.  A framed slot fed by our framed
                 * parameter is covered; fed by an unframed one, the callee's
                 * verified read IS our omitted read (EXCEEDED); fed by
                 * anything untrackable -- including a partial application
                 * that leaves a framed slot unbound -- UNVERIFIED. */
                for (uint32_t j = 0; j < 64; j++) {
                    if (!(callee->reads_params_mask & (UINT64_C(1) << j)))
                        continue;
                    if (j >= x->as.call_.n_args) { v = wf_worse(v, WF_UNVERIFIED); break; }
                    v = wf_worse(v, rf_attr(
                            reads_read_root(x->as.call_.args[j]),
                            params, n_params, mask, witness));
                }
            } else {
                v = WF_UNVERIFIED;
            }
        }
        for (uint32_t i = 0; i < x->as.call_.n_args; i++)
            v = wf_worse(v, RFS(x->as.call_.args[i]));
        return v;
    }

    case EX_IF:
        return wf_worse(RFS(x->as.if_.cond),
                        wf_worse(RFS(x->as.if_.then_),
                                 RFS(x->as.if_.else_or_null)));
    case EX_DO: {
        WfVerdict v = WF_VERIFIED;
        for (uint32_t i = 0; i < x->as.do_.n; i++)
            v = wf_worse(v, RFS(x->as.do_.items[i]));
        return v;
    }
    case EX_LET: case EX_LETREC: {
        WfVerdict v = RFS(x->as.let_.body);
        for (uint32_t i = 0; i < x->as.let_.n; i++)
            v = wf_worse(v, RFS(x->as.let_.bindings[i].init));
        return v;
    }
    case EX_WHILE:
        return wf_worse(RFS(x->as.while_.cond), RFS(x->as.while_.body));
    case EX_SET:
        /* The write is not a read; the value being written is. */
        return RFS(x->as.set_.value);
    case EX_SET_DEREF:
        return wf_worse(RFS(x->as.set_deref_.ref), RFS(x->as.set_deref_.value));
    case EX_MATCH: {
        WfVerdict v = RFS(x->as.match_.scrutinee);
        for (uint32_t i = 0; i < x->as.match_.n_arms; i++) {
            v = wf_worse(v, RFS(x->as.match_.arms[i].guard));
            v = wf_worse(v, RFS(x->as.match_.arms[i].body));
        }
        return v;
    }
    case EX_ASCRIBE: return RFS(x->as.ascribe_.inner);
    case EX_CAST:    return RFS(x->as.cast_.expr);
    case EX_REINTERPRET: return RFS(x->as.reinterpret_.expr);
    case EX_RETURN:  return RFS(x->as.return_.value);
    case EX_HANDLE: {
        const HandleExpr *h = x->as.handle_.handle;
        if (!h) return WF_UNVERIFIED;
        WfVerdict v = RFS(h->body);
        for (uint8_t i = 0; i < h->n_cases; i++)
            v = wf_worse(v, RFS(h->cases[i].body));
        return v;
    }
    /* `(resume k v)` transfers control; the reads are in its operands.
     * Modeled because every `(unsafe ...)` desugar carries one as its
     * handler-case body -- without this the whole visible-measure shape
     * would be UNVERIFIED for a resume that reads nothing. */
    case EX_RESUME: {
        const ResumeExpr *r = x->as.resume_.resume;
        if (!r) return WF_UNVERIFIED;
        return wf_worse(RFS(r->k), RFS(r->value));
    }
    default:
        return WF_UNVERIFIED;   /* unmodeled: "could not see", never clean */
    }
#undef RFS
}

void rf_note_reads_site(Elab *e, Binding *fn, Binding **params,
                        uint32_t n_params, Span span, bool is_clone) {
    if (!e || !fn) return;
    if (e->n_rf_reads_sites == e->cap_rf_reads_sites) {
        uint32_t ncap = e->cap_rf_reads_sites ? e->cap_rf_reads_sites * 2 : 8;
        RfReadsSite *nb = (RfReadsSite *)arena_alloc(
            e->arena, ncap * sizeof(RfReadsSite));
        if (e->rf_reads_sites)
            memcpy(nb, e->rf_reads_sites,
                   e->n_rf_reads_sites * sizeof(RfReadsSite));
        e->rf_reads_sites     = nb;
        e->cap_rf_reads_sites = ncap;
    }
    RfReadsSite *s = &e->rf_reads_sites[e->n_rf_reads_sites++];
    s->fn       = fn;
    s->params   = params;
    s->n_params = n_params;
    s->span     = span;
    s->is_clone = is_clone;
}

void rf_resolve_read_frames(Elab *e) {
    if (!e || e->n_rf_reads_sites == 0) return;
    /* The pass is GATELESS (R4 slice 3): its EXCEEDED verdict is positive
     * evidence of a broken frame -- the call-shape finding the defn-site
     * scans structurally cannot see -- and evidence belongs to the same
     * gateless W0383 tier those scans feed.  Only the dump stays behind
     * its flag; the verified stamp costs nothing to compute and nothing
     * consumes it behaviorally yet.
     *
     * Fixed point, same shape and same rationale as wf_resolve_write_frames:
     * the call case consults a callee's reads_checked, which a later round
     * may raise.  Verdicts only move toward verified, so this terminates. */
    uint32_t rounds = e->n_rf_reads_sites + 1;
    for (uint32_t round = 0; round < rounds; round++) {
        bool progress = false;
        for (uint32_t i = 0; i < e->n_rf_reads_sites; i++) {
            RfReadsSite *s = &e->rf_reads_sites[i];
            if (!s->fn || s->fn->reads_checked) continue;
            FnDef *fd = s->fn->source_fn_def;
            if (!fd || !fd->body) continue;   /* inline-C-only: UNVERIFIED */
            uint32_t budget = 65536;
            WfVerdict v = rf_scan(fd->body, s->params, s->n_params,
                                  s->fn->reads_params_mask, &budget, NULL);
            if (v == WF_VERIFIED) { s->fn->reads_checked = true; progress = true; }
        }
        if (!progress) break;
    }
    /* R4 slice 3: the evidence sweep.  A frame the fixed point left
     * unverified whose recomputed verdict is EXCEEDED is a demonstrably
     * broken promise, reached through a verified callee's frame -- stamp it
     * exactly as the defn-site scans stamp their findings (every binding,
     * clones included, so the encoder's refusal sees it whichever one a
     * call resolves to), and warn once per declared frame.  A frame the
     * defn-site scans already stamped is skipped: one finding, one warning,
     * whichever tier saw it first. */
    for (uint32_t i = 0; i < e->n_rf_reads_sites; i++) {
        RfReadsSite *s = &e->rf_reads_sites[i];
        if (!s->fn || s->fn->reads_checked || s->fn->reads_frame_omits_state)
            continue;
        FnDef *fd = s->fn->source_fn_def;
        if (!fd || !fd->body) continue;
        uint32_t budget = 65536;
        const Binding *witness = NULL;
        if (rf_scan(fd->body, s->params, s->n_params,
                    s->fn->reads_params_mask, &budget, &witness) != WF_EXCEEDED)
            continue;
        s->fn->reads_frame_omits_state = true;
        if (!s->is_clone) {
            char frame_txt[256];
            size_t fo = 0;
            frame_txt[0] = '\0';
            for (uint32_t pi = 0; pi < s->n_params && pi < 64; pi++) {
                if (!(s->fn->reads_params_mask & (UINT64_C(1) << pi))) continue;
                const char *pn = (s->params[pi] && s->params[pi]->name)
                                     ? s->params[pi]->name->name : "?";
                int wrote = snprintf(frame_txt + fo, sizeof(frame_txt) - fo,
                                     "%s%s", fo ? " " : "", pn);
                if (wrote < 0 || (size_t)wrote >= sizeof(frame_txt) - fo) break;
                fo += (size_t)wrote;
            }
            diag_emit_with_code(DIAG_WARNING, s->span,
                                TUR_W0383_READS_FRAME_OMITS_MUTABLE,
                                "`#reads %s` omits mutable state the body reads: "
                                "a call in the body reaches state rooted in "
                                "'%s', which the frame does not name, through a "
                                "callee's own verified `#reads` frame; name it "
                                "in this frame too",
                                frame_txt[0] ? frame_txt : "?",
                                (witness && witness->name)
                                    ? witness->name->name : "?");
        }
    }
    /* --dump-read-frames: one line per DECLARED frame (clones are silent),
     * in registration order.  EXCEEDED is distinguished from UNVERIFIED so
     * a fixture can tell "broken" from "could not see". */
    if (g_dump_read_frames) {
        for (uint32_t i = 0; i < e->n_rf_reads_sites; i++) {
            RfReadsSite *s = &e->rf_reads_sites[i];
            if (!s->fn || s->is_clone) continue;
            const char *verdict = "UNVERIFIED";
            if (s->fn->reads_checked) {
                verdict = "VERIFIED";
            } else {
                FnDef *fd = s->fn->source_fn_def;
                if (fd && fd->body) {
                    uint32_t budget = 65536;
                    if (rf_scan(fd->body, s->params, s->n_params,
                                s->fn->reads_params_mask, &budget,
                                NULL) == WF_EXCEEDED)
                        verdict = "EXCEEDED";
                }
            }
            printf("read-frame %s: %s mask=0x%llx\n",
                   s->fn->name ? s->fn->name->name : "<fn>",
                   verdict,
                   (unsigned long long)s->fn->reads_params_mask);
        }
    }
}

/* ------------------------------------------------------------------------- *
 * WF3 widening: borrow-aware disjointness, backed by CHECKED callee frames.
 *
 * The landed WF3 slice declines a whole caller body the moment an assigned
 * name is also BORROWED, on the grounds that "a borrowed local is the one way a
 * callee could write this frame's slot".  That is true, and until WF2 it was
 * also the end of the conversation -- there was no way to ask what a callee
 * does, so assuming the worst was the only sound answer.
 *
 * With a checked frame there is now a question to ask instead of an assumption
 * to make.  A borrow is dangerous only if it REACHES a callee that writes
 * through it, so the decline can be lifted when every borrow of the assigned
 * name is provably write-free.  Two shapes are recognized, and everything else
 * keeps the old decline:
 *
 *   (f ... (& acc) ...)          -- passed straight into a call slot; safe iff
 *                                   that slot cannot be written.
 *   (let [b (& acc)] body)       -- `b` aliases the borrow, so every occurrence
 *                                   of `b` in `body` must itself sit in a slot
 *                                   that cannot be written.  Zero occurrences
 *                                   is the common case and trivially safe --
 *                                   that is the `frozen` region idiom, whose
 *                                   `(& w)` exists only to lock w down and is
 *                                   never passed anywhere.
 *
 * "Cannot be written" has exactly three sources, in descending strength:
 *   - a `^borrow` parameter, which the borrow checker already forbids writing
 *     through.  A fact, independent of any annotation.
 *   - a CHECKED `#writes` frame that excludes the slot.  A fact, because WF2
 *     walked the body.
 *   - nothing else.  An unresolvable callee, an unannotated non-borrow slot, or
 *     a DECLARED-but-unchecked frame all answer "assume it writes".  The last
 *     of those is the point of the tier split: acting on a trusted frame here
 *     would let a promise elide a check, which is the thing the checked tier
 *     exists to prevent.
 *
 * Unconditional since write-frames graduated (2026-08-20): it consumes WF2's
 * verdicts, and a body with no checked frame in reach simply answers "assume
 * it writes", which is what it answered before the checked tier existed. */

/* Can the callee named `head` write the argument it receives in `slot`
 * (0-based)?  Conservative: true unless something positively says otherwise. */
static bool wf_call_slot_writes(Elab *e, const Symbol *head, uint32_t slot) {
    Binding *callee = head ? scope_lookup(&e->global, head) : NULL;
    if (!callee) return true;              /* unresolvable: assume the worst */
    if (callee->writes_declared) {
        if (!callee->writes_checked) return true;   /* a promise, not a fact */
        return slot >= WF_MAX_FRAME_PARAMS ||
               (callee->writes_param_mask & ((uint32_t)1u << slot)) != 0;
    }
    const FnDef *fd = callee->source_fn_def;
    if (!fd || slot >= fd->n_params || !fd->params[slot]) return true;
    return !fd->params[slot]->is_borrow;
}

/* Every occurrence of `alias` in `f` sits in a call slot the callee cannot
 * write.  A bare occurrence anywhere else -- returned, stored, assigned, used
 * as a callee -- is not accounted for and answers false. */
static bool wf_alias_write_free(Elab *e, const Form *f, const char *alias,
                                uint32_t depth) {
    if (!f) return true;
    if (depth >= WF_SCAN_MAX_DEPTH) return false;   /* too deep to vouch for */
    if (f->tag == F_SYM) return !rt_sym_is(f, alias);
    if (f->tag != F_LIST && f->tag != F_VEC) return true;
    {
        const Form *mx = rt_macro_expansion(e, f);
        if (mx) return wf_alias_write_free(e, mx, alias, depth);
    }
    if (f->tag == F_LIST && f->as.list.len >= 1 && f->as.list.items[0] &&
        f->as.list.items[0]->tag == F_SYM && f->as.list.items[0]->as.sym) {
        const Symbol *head = f->as.list.items[0]->as.sym;
        if (rt_sym_is(f->as.list.items[0], alias))
            return false;                  /* the borrow itself is the callee */
        for (uint32_t i = 1; i < f->as.list.len; i++) {
            const Form *a = f->as.list.items[i];
            if (a && a->tag == F_SYM && rt_sym_is(a, alias)) {
                if (wf_call_slot_writes(e, head, i - 1)) return false;
                continue;                  /* this occurrence is accounted for */
            }
            if (!wf_alias_write_free(e, a, alias, depth + 1)) return false;
        }
        return true;
    }
    for (uint32_t i = 0; i < f->as.list.len; i++)
        if (!wf_alias_write_free(e, f->as.list.items[i], alias, depth + 1))
            return false;
    return true;
}

/* True when every borrow of `name` in `f` is provably write-free. */
static bool wf_borrow_write_free(Elab *e, const Form *f, const char *name,
                                 uint32_t depth) {
    if (!f) return true;
    if (depth >= WF_SCAN_MAX_DEPTH) return false;   /* too deep to vouch for */
    if (f->tag != F_LIST && f->tag != F_VEC) return true;
    {
        const Form *mx = rt_macro_expansion(e, f);
        if (mx) return wf_borrow_write_free(e, mx, name, depth);
    }
    /* `(let [b (& name)] body)`: b aliases the borrow for the whole body. */
    if (rt_head_is(f, "let") && f->as.list.len == 3) {
        const Form *binds = f->as.list.items[1];
        const Form *body  = f->as.list.items[2];
        if (binds && binds->tag == F_VEC && binds->as.list.len == 2) {
            const Form *x = binds->as.list.items[0];
            const Form *v = binds->as.list.items[1];
            if (v && v->tag == F_LIST && v->as.list.len >= 2 &&
                rt_sym_is(v->as.list.items[0], "&") &&
                rt_sym_is(v->as.list.items[1], name)) {
                if (!x || x->tag != F_SYM || !x->as.sym) return false;
                if (!wf_alias_write_free(e, body, x->as.sym->name, 0)) return false;
                /* The body may borrow `name` AGAIN, independently. */
                return wf_borrow_write_free(e, body, name, depth + 1);
            }
        }
    }
    /* A bare `(& name)`.  Checked BEFORE the call branch below, which would
     * otherwise walk this very form as a call to `&` and find nothing wrong
     * with it.  Reaching here means no enclosing form accounted for the
     * borrow, so there is no answer about where it goes. */
    if (f->tag == F_LIST && f->as.list.len >= 2 &&
        rt_sym_is(f->as.list.items[0], "&") &&
        rt_sym_is(f->as.list.items[1], name))
        return false;
    /* `&mut name` is a write channel by construction -- there is nothing to
     * vouch for, wherever it goes. */
    if (f->tag == F_LIST && f->as.list.len >= 2 &&
        rt_sym_is(f->as.list.items[0], "&mut") &&
        rt_sym_is(f->as.list.items[1], name))
        return false;
    /* `(g ... (& name) ...)`: handed straight to a call slot. */
    if (f->tag == F_LIST && f->as.list.len >= 1 && f->as.list.items[0] &&
        f->as.list.items[0]->tag == F_SYM && f->as.list.items[0]->as.sym) {
        const Symbol *head = f->as.list.items[0]->as.sym;
        for (uint32_t i = 1; i < f->as.list.len; i++) {
            const Form *a = f->as.list.items[i];
            if (a && a->tag == F_LIST && a->as.list.len >= 2 &&
                rt_sym_is(a->as.list.items[0], "&") &&
                rt_sym_is(a->as.list.items[1], name)) {
                if (wf_call_slot_writes(e, head, i - 1)) return false;
                continue;                  /* this borrow is accounted for */
            }
            if (!wf_borrow_write_free(e, a, name, depth + 1)) return false;
        }
        return true;
    }
    for (uint32_t i = 0; i < f->as.list.len; i++)
        if (!wf_borrow_write_free(e, f->as.list.items[i], name, depth + 1))
            return false;
    return true;
}

/* ------------------------------------------------------------------------- *
 * A datatype theory for match arms.
 *
 * The VC term language has no constructors and no selectors, and it is not
 * getting any: adding sorts and theory solvers for algebraic data would be a
 * far larger change than what a match arm actually needs.  What it needs is a
 * way to SAY "on this path the scrutinee is a Circle, and `r` is its radius",
 * and that is expressible today -- as ordinary equations over UNINTERPRETED
 * functions, which S1 already decides by congruence closure.
 *
 * So the theory is synthesized at the FORM level, before encoding:
 *
 *   (Circle r)  on scrutinee `s`   ==>   (= (#dt/tag s) 3)
 *                                        (= r (.radius s))
 *
 * `#dt/tag` is a total function from the datatype to its discriminant, which is
 * exactly what `CtorDef.tag` numbers.  A field selector is a total function too
 * -- undefined-in-principle off its own constructor, but an uninterpreted
 * symbol is free to take any value there, and this arm only ever asserts the
 * case where it is defined.  Both facts are true on the path being proved, so
 * asserting them is sound; the solver needs no new theory to use them, because
 * congruence over an uninterpreted symbol is the whole of what they require.
 *
 * THE SYNTHETIC NAME MUST NOT BE SPELLABLE.  `enc_measure` resolves a head name
 * to decide purity, and a name that resolves to a real PURE function would have
 * `(= (#dt/tag s) 3)` assert something about THAT function -- a hypothesis that
 * can be false, and a false hypothesis proves the goal and elides the check.
 * A leading `#` is reader dispatch, so no source symbol can begin with one;
 * the name therefore resolves to nothing, which the encoder reads as an
 * abstract measure -- uninterpreted and congruent, which is what it is.
 * ------------------------------------------------------------------------- */

#define RT_DT_TAG_FN "#dt/tag"

static Form *rt_form_call1(Elab *e, Span sp, const char *fn, const Form *arg) {
    Form **k = (Form **)arena_alloc(e->arena, 2 * sizeof(Form *));
    k[0] = form_sym(e->arena, sp,
                    symtab_intern(e->st, strslice(fn, (uint32_t)strlen(fn))));
    k[1] = (Form *)arg;
    return form_list(e->arena, sp, k, 2);
}

static Form *rt_form_eq(Elab *e, Span sp, const Form *a, const Form *b) {
    Form **k = (Form **)arena_alloc(e->arena, 3 * sizeof(Form *));
    k[0] = form_sym(e->arena, sp, symtab_intern(e->st, strslice("=", 1)));
    k[1] = (Form *)a;
    k[2] = (Form *)b;
    return form_list(e->arena, sp, k, 3);
}

/* True when `f` is exactly the symbol `name`. */
static bool rt_sym_is(const Form *f, const char *name) {
    return f && f->tag == F_SYM && f->as.sym &&
           strcmp(f->as.sym->name, name) == 0;
}

/* The constructor a pattern names, or NULL when the pattern is not a
 * constructor application (a literal, a wildcard, a bare binder). */
static CtorDef *rt_pat_ctor(Elab *e, const Form *pat) {
    if (!pat || pat->tag != F_LIST || pat->as.list.len == 0) return NULL;
    const Form *h = pat->as.list.items[0];
    if (h->tag != F_SYM || !h->as.sym) return NULL;
    return elab_lookup_ctor(e, h->as.sym);
}

/* Copy `f`, renaming every FREE occurrence of `from` to `to`.
 *
 * Path splitting asserts `x = v` in the environment's single flat namespace,
 * so a `let` that rebinds a name already in scope would assert
 * `x = <something mentioning x>` -- a contradiction, which proves the goal and
 * elides a check that was protecting something.  Renaming the binding is what
 * makes those bodies splittable instead of skipped.
 *
 * Returns NULL when the body REBINDS the same name again, which would need a
 * second rename to stay correct; declining is the safe answer and the caller
 * falls back to the whole-body obligation. */
static Form *rt_rename_free(Elab *e, const Form *f,
                            const Symbol *from, const Symbol *to) {
    if (!f) return NULL;
    if (f->tag == F_SYM)
        return (f->as.sym == from)
             ? form_sym(e->arena, f->span, to) : (Form *)f;
    if (f->tag != F_LIST && f->tag != F_VEC) return (Form *)f;

    /* A nested binder of the same name: decline rather than guess. */
    if (f->tag == F_LIST && f->as.list.len >= 2 &&
        f->as.list.items[0]->tag == F_SYM &&
        f->as.list.items[0]->as.sym &&
        strcmp(f->as.list.items[0]->as.sym->name, "let") == 0) {
        const Form *b = f->as.list.items[1];
        if (b && b->tag == F_VEC)
            for (uint32_t i = 0; i + 1 < b->as.list.len; i += 2)
                if (b->as.list.items[i]->tag == F_SYM &&
                    b->as.list.items[i]->as.sym == from)
                    return NULL;
    }

    uint32_t n = f->as.list.len;
    Form **kids = (Form **)arena_alloc(e->arena, (n ? n : 1) * sizeof(Form *));
    bool changed = false;
    for (uint32_t i = 0; i < n; i++) {
        kids[i] = rt_rename_free(e, f->as.list.items[i], from, to);
        if (!kids[i]) return NULL;
        if (kids[i] != f->as.list.items[i]) changed = true;
    }
    if (!changed) return (Form *)f;
    return f->tag == F_LIST ? form_list(e->arena, f->span, kids, n)
                            : form_vec(e->arena, f->span, kids, n);
}

/* Run `body` with `hyp` assumed.  The env's hypothesis list is a cons chain
 * and its name table only ever grows, so a scope is saved and restored by
 * rewinding both -- no copying. */
static bool rt_prove_under(Elab *e, const Form *pred, const char *var_name,
                           const Form *hyp, const Form *body,
                           TypeKind base_kind, RefineEnv *env, Span loc,
                           uint32_t depth) {
    RefineHyp *saved_head  = env->head;
    uint32_t   saved_names = env->n_names;
    if (hyp) refine_env_push(env, hyp, NULL, NULL);
    bool ok = rt_prove_paths(e, pred, var_name, body, base_kind, env, loc, depth + 1);
    env->head    = saved_head;
    env->n_names = saved_names;
    return ok;
}

static bool li_prove_paths_ext(Elab *e, const Form *pred, const char *var_name,
                               const Form *subject, TypeKind base_kind,
                               RefineEnv *env, Span loc, uint32_t depth,
                               bool *handled);

static bool rt_prove_paths(Elab *e, const Form *pred, const char *var_name,
                           const Form *subject, TypeKind base_kind,
                           RefineEnv *env, Span loc, uint32_t depth) {
    if (!subject || depth >= RT_PATH_MAX_DEPTH) return false;

    /* loop-invariants-plan LI3 (under the gate): multi-binding `let` and the
     * post-loop fact.  See li_prove_paths_ext. */
    {
        bool handled = false;
        bool ok = li_prove_paths_ext(e, pred, var_name, subject, base_kind, env,
                                     loc, depth, &handled);
        if (handled) return ok;
    }

    /* (if c t e) -- both arms, each under its own path condition.  A one-armed
     * `if` has no value on the false path and is left alone. */
    if (rt_head_is(subject, "if") && subject->as.list.len == 4) {
        const Form *c = subject->as.list.items[1];
        const Form *t = subject->as.list.items[2];
        const Form *f = subject->as.list.items[3];
        Form **notk = (Form **)arena_alloc(e->arena, 2 * sizeof(Form *));
        notk[0] = form_sym(e->arena, subject->span,
                           symtab_intern(e->st, strslice("not", 3)));
        notk[1] = (Form *)c;
        Form *nc = form_list(e->arena, subject->span, notk, 2);
        return rt_prove_under(e, pred, var_name, c,  t, base_kind, env, loc, depth) &&
               rt_prove_under(e, pred, var_name, nc, f, base_kind, env, loc, depth);
    }

    /* (let [x v] body) -- one binding, one body form.  `x = v` is the fact the
     * body's own obligation needs.  Wider shapes fall through rather than
     * guess which binding the result depends on. */
    if (rt_head_is(subject, "let") && subject->as.list.len == 3) {
        const Form *binds = subject->as.list.items[1];
        const Form *body  = subject->as.list.items[2];
        if (binds && binds->tag == F_VEC && binds->as.list.len == 2 &&
            binds->as.list.items[0]->tag == F_SYM) {
            const Form *x = binds->as.list.items[0];
            const Form *v = binds->as.list.items[1];
            /* SHADOWING IS A CONTRADICTION HERE.  The hypothesis is `x = v` in
             * the environment's single flat namespace, so rebinding a name
             * that is already in scope asserts `x = <something mentioning x>`.
             * `(let [x (- x 1)] x)` becomes `x = x - 1`, which is false for
             * every x -- and a false hypothesis proves the goal, so the check
             * gets elided and the function returns a value violating its own
             * refinement.  That is a miscompile, and this shape is ordinary
             * code, not a corner case.
             *
             * Declining to split is the fix: the whole-body obligation still
             * runs and still answers unknown, exactly as before path
             * splitting existed.  Alpha-renaming the binding would recover
             * these bodies and is the natural follow-up. */
            /* SHADOWING.  The hypothesis is `x = v` in one flat namespace,
             * so rebinding a name already in scope would assert
             * `x = <something mentioning x>` -- `(let [x (- x 1)] x)` becomes
             * `x = x - 1`, false for every x, and a false hypothesis proves
             * the goal.  That was a live miscompile.
             *
             * Alpha-rename instead of declining: the binding becomes a fresh
             * name, the body is rewritten to use it, and `v` keeps referring
             * to the OUTER x because only the body is renamed. */
            bool shadows = false;
            for (uint32_t _i = 0; _i < env->n_names; _i++)
                if (env->names[_i] &&
                    strcmp(env->names[_i], x->as.sym->name) == 0)
                    { shadows = true; break; }
            if (shadows) {
                char fresh[96];
                snprintf(fresh, sizeof(fresh), "%s~%u", x->as.sym->name, depth);
                const Symbol *fs = symtab_intern(
                    e->st, strslice(arena_strdup(e->arena, fresh, strlen(fresh)),
                                    (uint32_t)strlen(fresh)));
                Form *nx = form_sym(e->arena, x->span, fs);
                Form *nbody = rt_rename_free(e, body, x->as.sym, fs);
                if (!nbody) return false;
                x = nx; body = nbody;
            }
            refine_env_declare(env, x->as.sym->name, rt_sort_of_kind(base_kind));

            /* A BRANCHING VALUE splits too.  `(let [m (if c a b)] ...)` used to
             * assert `m = (if c a b)`, and an `if` is not a term the encoder
             * can build -- so the hypothesis was dropped and `m` went into the
             * body completely unconstrained.  `(let [m (if (<= a b) a b)] m)`
             * against `(<= r a)` was Unknown while the identical `if` written
             * directly as the body proved, which is a distinction no one
             * writing the code would expect to matter.
             *
             * Splitting the VALUE is the same rule as splitting the body, one
             * level up: on the true path `m = a`, on the false path `m = b`,
             * and the branch condition is a fact on each. */
            if (rt_head_is(v, "if") && v->as.list.len == 4 &&
                depth + 1 < RT_PATH_MAX_DEPTH) {
                const Form *c  = v->as.list.items[1];
                const Form *vt = v->as.list.items[2];
                const Form *vf = v->as.list.items[3];
                Form **notk = (Form **)arena_alloc(e->arena, 2 * sizeof(Form *));
                notk[0] = form_sym(e->arena, v->span,
                                   symtab_intern(e->st, strslice("not", 3)));
                notk[1] = (Form *)c;
                Form *nc = form_list(e->arena, v->span, notk, 2);

                RefineHyp *br_head  = env->head;
                uint32_t   br_names = env->n_names;
                refine_env_push(env, c, NULL, NULL);
                bool ok_t = rt_prove_under(e, pred, var_name,
                                           rt_form_eq(e, v->span, x, vt),
                                           body, base_kind, env, loc, depth + 1);
                env->head = br_head; env->n_names = br_names;
                if (!ok_t) return false;

                refine_env_push(env, nc, NULL, NULL);
                bool ok_f = rt_prove_under(e, pred, var_name,
                                           rt_form_eq(e, v->span, x, vf),
                                           body, base_kind, env, loc, depth + 1);
                env->head = br_head; env->n_names = br_names;
                return ok_f;
            }

            return rt_prove_under(e, pred, var_name,
                                  rt_form_eq(e, subject->span, x, v),
                                  body, base_kind, env, loc, depth);
        }
        return false;
    }

    /* (do s1 ... sn) -- the value is the LAST form; the rest are statements.
     *
     * Splitting on the last form is only sound while the statements cannot
     * invalidate the hypotheses already in the environment. Those hypotheses
     * are about parameters, so an assignment is exactly what would stale them:
     * `(do (set! x 0) (if (>= x 0) x 0))` must not be proved using the
     * parameter refinement that held BEFORE the assignment. A Form-level scan
     * for any `set!` in the statements is conservative -- it declines some
     * harmless writes to purely local state -- and cheap, which is the right
     * trade for a rule whose failure mode is a miscompile. */
    if (rt_head_is(subject, "do") && subject->as.list.len >= 2) {
        for (uint32_t i = 1; i + 1 < subject->as.list.len; i++)
            if (rt_form_mentions_set(e, subject->as.list.items[i], 0)) return false;
        return rt_prove_paths(e, pred, var_name,
                              subject->as.list.items[subject->as.list.len - 1],
                              base_kind, env, loc, depth + 1);
    }

    /* (match scrut pat1 [when g1] e1 pat2 [when g2] e2 ...) -- every arm, under
     * everything the arm's selection tells us.
     *
     * Arms are NOT fixed-width: `when` inserts a guard between a pattern and
     * its body, so the list is walked rather than strided in pairs.  Striding
     * misreads a guarded match wholesale -- `when` becomes a pattern and the
     * guard expression becomes a body -- which failed safe (the misread arm
     * never proved) but meant no guarded match was ever split.
     *
     * Four hypothesis sources, all true whenever the arm is the one that runs:
     *
     *   - the CONSTRUCTOR, as `(= (#dt/tag s) <tag>)`;
     *   - each record FIELD bound by the pattern, as `(= b (.field s))`,
     *     which is what lets a getter's postcondition mention the field it
     *     returns;
     *   - a LITERAL pattern, as `(= s <lit>)`;
     *   - the GUARD, verbatim -- a necessary condition for the arm to fire.
     *
     * The guard is only a NECESSARY condition, never sufficient: an arm also
     * requires every earlier arm to have failed, which is not asserted. That
     * direction is the safe one (fewer hypotheses only make a goal harder).
     *
     * Pattern binders that SHADOW a name in scope still decline the split. The
     * encoder has one flat namespace, so the arm's `r` would inherit an outer
     * `r`'s hypotheses -- the same class of bug as the shadowed `let`, and
     * unsound rather than imprecise.  Alpha-renaming would now be possible (a
     * binder finally HAS a fact to rename into), but the rename would have to
     * reach the synthesized selector facts as well, and declining costs only
     * precision. */
    if (rt_head_is(subject, "match") && subject->as.list.len >= 4) {
        const Form *scrut  = subject->as.list.items[1];
        const uint32_t len = subject->as.list.len;
        RefineHyp *saved_head  = env->head;
        uint32_t   saved_names = env->n_names;
        bool ok = true;
        uint32_t i = 2;
        while (ok && i < len) {
            const Form *pat = subject->as.list.items[i++];
            const Form *guard = NULL;
            if (i + 1 < len && rt_sym_is(subject->as.list.items[i], "when")) {
                guard = subject->as.list.items[i + 1];
                i += 2;
            }
            if (i >= len) { ok = false; break; }   /* malformed: no body */
            const Form *arm = subject->as.list.items[i++];

            /* PER-ARM SCOPE.  Each arm's facts must die with the arm: two arms
             * of the same match assert different tags for the same scrutinee,
             * so letting them accumulate would put `tag(s) = 0` and
             * `tag(s) = 1` in one environment -- a contradiction, which proves
             * every later arm's goal and elides its check.  The outer
             * save/restore below is not enough; this is the one that matters. */
            RefineHyp *arm_head  = env->head;
            uint32_t   arm_names = env->n_names;

            CtorDef *cd = rt_pat_ctor(e, pat);
            if (cd)
                refine_env_push(env,
                                rt_form_eq(e, pat->span,
                                           rt_form_call1(e, pat->span,
                                                         RT_DT_TAG_FN, scrut),
                                           form_int(e->arena, pat->span,
                                                    (int64_t)cd->tag)),
                                NULL, NULL);
            else if (pat && (pat->tag == F_INT || pat->tag == F_FLOAT))
                refine_env_push(env, rt_form_eq(e, pat->span, scrut, pat),
                                NULL, NULL);

            /* Binders: declare, decline on shadow, and tie each to its field. */
            if (pat && (pat->tag == F_LIST || pat->tag == F_VEC)) {
                for (uint32_t k = 1; ok && k < pat->as.list.len; k++) {
                    const Form *b = pat->as.list.items[k];
                    if (b->tag != F_SYM || !b->as.sym) continue;
                    for (uint32_t j = 0; j < env->n_names; j++)
                        if (env->names[j] &&
                            strcmp(env->names[j], b->as.sym->name) == 0)
                            { ok = false; break; }
                    if (!ok) break;
                    /* A binder's sort is its FIELD's, not the refinement's base
                     * type.  Declaring it at `base_kind` made every binder of
                     * a `: #refine{ r : bool | ... }` function a proposition,
                     * so its field equation `(= t (.tl xs))` was dropped as a
                     * Bool/Int mismatch and the arm knew nothing about `t` --
                     * which is what kept reflected-measures' non-ground
                     * unfolding (RF4) from firing in a bool-returning body. */
                    uint32_t fi = k - 1;
                    VCSort bsort = rt_sort_of_kind(base_kind);
                    if (cd && fi < cd->n_fields) bsort = rt_sort_of_kind(cd->fields[fi].kind);
                    refine_env_declare(env, b->as.sym->name, bsort);
                    /* Only a record constructor has a field NAME to select
                     * with; a positional variant has no accessor to speak of,
                     * so its binder stays unconstrained as before. */
                    if (cd && cd->is_record && fi < cd->n_fields &&
                        cd->fields[fi].name) {
                        char acc[128];
                        snprintf(acc, sizeof(acc), ".%s", cd->fields[fi].name);
                        refine_env_push(env,
                                        rt_form_eq(e, b->span, b,
                                                   rt_form_call1(e, b->span,
                                                                 acc, scrut)),
                                        NULL, NULL);
                    }
                }
            }

            if (ok && guard) refine_env_push(env, guard, NULL, NULL);
            if (ok) ok = rt_prove_paths(e, pred, var_name, arm, base_kind, env,
                                        loc, depth + 1);
            env->head    = arm_head;
            env->n_names = arm_names;
        }
        env->head    = saved_head;
        env->n_names = saved_names;
        return ok;
    }

    /* A leaf: an ordinary expression the encoder can substitute for `r`. */
    return rt_prove_silent(e, pred, var_name, subject, base_kind, env, loc);
}

/* ------------------------------------------------------------------------- *
 * Constructor axioms.
 *
 * The match-arm theory gives a selector its meaning going ONE way: a binder is
 * the field it destructures.  It says nothing about a value that was just
 * BUILT, so `(let [b (Box p 3)] (match b (Box w h) w))` against `(= r p)` had
 * the chain `b = Box(p,3)`, `w = .width(b)`, goal `w = p` -- and no rule
 * connecting `.width` to the `Box` that produced it.
 *
 * The missing fact is the defining equation of a record constructor:
 *
 *     (= (.width (Box p 3)) p)
 *
 * It is universally true -- not path-dependent, not an assumption -- so it can
 * be asserted once per constructor application found anywhere in the obligation
 * rather than per path.  Congruence closure then does the rest: `b` and
 * `Box(p,3)` are equal, so `.width(b)` and `.width(Box(p,3))` are the same
 * term, which is `p`.
 *
 * Only RECORD constructors take part.  A positional variant has no field name,
 * so there is no accessor to write the equation about.
 * ------------------------------------------------------------------------- */

#define RT_CTOR_AXIOM_MAX_DEPTH 8

static void rt_push_ctor_axioms(Elab *e, RefineEnv *env, const Form *f,
                                uint32_t depth) {
    if (!f || depth >= RT_CTOR_AXIOM_MAX_DEPTH) return;
    if (f->tag != F_LIST && f->tag != F_VEC) return;

    if (f->tag == F_LIST && f->as.list.len >= 1) {
        CtorDef *cd = rt_pat_ctor(e, f);
        /* Arity must match exactly: a partial application is a closure, not a
         * value with fields, and over-applying is not this form at all. */
        if (cd && cd->is_record && cd->n_fields == f->as.list.len - 1) {
            for (uint32_t i = 0; i < cd->n_fields; i++) {
                if (!cd->fields[i].name) continue;
                char acc[128];
                snprintf(acc, sizeof(acc), ".%s", cd->fields[i].name);
                refine_env_push(env,
                                rt_form_eq(e, f->span,
                                           rt_form_call1(e, f->span, acc, f),
                                           f->as.list.items[i + 1]),
                                NULL, NULL);
            }
        }
    }

    for (uint32_t i = 0; i < f->as.list.len; i++)
        rt_push_ctor_axioms(e, env, f->as.list.items[i], depth + 1);
}

static bool rt_return_obligation_proven(Elab *e, const Form *pred,
                                        const char *var_name,
                                        const Form *subject, TypeKind base_kind,
                                        RefineEnv *env, const char *what,
                                        const char *fn_name, Span loc) {
    if (!pred) return false;
    /* Never elide a check that is itself observable.  Reported as not-proven
     * rather than diagnosed: an impure predicate keeps the runtime check it
     * would have had anyway, so discharge declines rather than inventing an
     * error. */
    if (rt_pred_is_impure(e, pred)) return false;

    /* Constructor axioms hold on every path, so they go in once, ahead of the
     * split, and are rewound afterwards so nothing leaks into the next
     * function's environment. */
    RefineHyp *ax_head  = env->head;
    uint32_t   ax_names = env->n_names;
    rt_push_ctor_axioms(e, env, subject, 0);

    /* RT4: a branching body is proved per path when it can be.  Tried first
     * and silently; failing costs one extra pass and changes nothing the user
     * sees, because the whole-body obligation below still runs and still
     * reports.
     *
     * The probes are separate obligations discharged before this site's
     * obligation exists, so a cap that bites inside one is outside that
     * obligation's own snapshot window.  Bracket the split here -- this is the
     * only place where "on whose behalf" is well-defined -- and hand the delta
     * to the obligation as `caps_probe`.  Without it the per-compile summary
     * reports `** HIT` while the site's `caps_hit` reads empty, which is the
     * wrong direction to be wrong in for a field SX6's gate reads as
     * per-site evidence. */
    const RefineCapStats caps_before_probes = *refine_caps();
    RefineCapStats probe_caps;
    memset(&probe_caps, 0, sizeof(probe_caps));
    bool split_tried = false;
    if (subject && (rt_head_is(subject, "if") || rt_head_is(subject, "let") ||
                    rt_head_is(subject, "match") || rt_head_is(subject, "do"))) {
        split_tried = true;
        bool split_ok = rt_prove_paths(e, pred, var_name, subject, base_kind,
                                       env, loc, 0);
        refine_caps_delta(&probe_caps, refine_caps(), &caps_before_probes);
        if (split_ok) {
            refine_note_split_proven();
            /* Record the site as an obligation already decided, so the JSON
             * dump lists it.  It used to return before any record existed:
             * a return refinement proved per path appeared in the dump only
             * when it FAILED, so a consumer could not tell "proved" from "never
             * checked".  Pre-discharged, so refine_discharge_all skips it and
             * the stats refine_note_split_proven counted are not counted
             * twice. */
            RefineObligation *pob =
                refine_collect_obligation(&e->refine_obs, pred, var_name, subject,
                                          rt_sort_of_kind(base_kind),
                                          type_name(type_simple(base_kind, CK_COPY)),
                                          loc, env, what, fn_name);
            if (pob) {
                pob->discharged = true;
                pob->proven     = true;
                pob->decided_by = "RT4 (per-path split)";
                refine_caps_add_hits(&pob->caps_probe, &probe_caps);
            }
            env->head = ax_head; env->n_names = ax_names;
            return true;
        }
    }

    RefineObligation *ob =
        refine_collect_obligation(&e->refine_obs, pred, var_name, subject,
                                  rt_sort_of_kind(base_kind), type_name(type_simple(base_kind, CK_COPY)),
                                  loc, env, what, fn_name);
    if (ob && split_tried) refine_caps_add_hits(&ob->caps_probe, &probe_caps);
    bool proven = refine_discharge_one(ob, e->arena);
    env->head = ax_head; env->n_names = ax_names;
    return proven;
}

/* ------------------------------------------------------------------------- *
 * RT1 call-site crossings.
 *
 * Passing an argument where the parameter type is `#refine{ v : T | p }` is a
 * crossing INTO the refinement, so the caller owes a proof of `p[arg/v]`.  We
 * record the crossing during elaboration and resolve it afterwards; see the
 * comment on Elab.refine_call_sites for why the deferral is load-bearing.
 * ------------------------------------------------------------------------- */

/* Hash of the (callee, call_form) pointer pair. */
static uint32_t rt_cs_hash(const void *a, const void *b) {
    uintptr_t x = (uintptr_t)a * 0x9e3779b97f4a7c15ull;
    uintptr_t y = (uintptr_t)b * 0xc2b2ae3d27d4eb4full;
    uint32_t h = (uint32_t)((x ^ (y >> 17)) >> 13);
    return h ? h : 1;
}

/* Probe the dedup set; returns true when this pair was already recorded.
 * Inserts `slot` otherwise. */
static bool rt_cs_seen(Elab *e, const Binding *callee, const Form *cf, uint32_t slot) {
    if (e->refine_cs_htab_cap == 0 ||
        (e->n_refine_call_sites + 1) * 2 >= e->refine_cs_htab_cap) {
        uint32_t ncap = e->refine_cs_htab_cap ? e->refine_cs_htab_cap * 2 : 128;
        uint32_t *nt = (uint32_t *)arena_alloc(e->arena, ncap * sizeof(uint32_t));
        memset(nt, 0, ncap * sizeof(uint32_t));
        for (uint32_t i = 0; i < e->n_refine_call_sites; i++) {
            uint32_t h = rt_cs_hash(e->refine_call_sites[i].callee,
                                    e->refine_call_sites[i].call_form);
            uint32_t j = h & (ncap - 1);
            while (nt[j]) j = (j + 1) & (ncap - 1);
            nt[j] = i + 1;
        }
        e->refine_cs_htab = nt;
        e->refine_cs_htab_cap = ncap;
    }
    uint32_t h = rt_cs_hash(callee, cf);
    uint32_t j = h & (e->refine_cs_htab_cap - 1);
    while (e->refine_cs_htab[j]) {
        const RefineCallSite *cs = &e->refine_call_sites[e->refine_cs_htab[j] - 1];
        if (cs->callee == callee && cs->call_form == cf) return true;
        j = (j + 1) & (e->refine_cs_htab_cap - 1);
    }
    e->refine_cs_htab[j] = slot + 1;
    return false;
}

uint32_t refine_note_call_site(Elab *e, const Binding *callee,
                               const Form *call_form, uint32_t arg_offset) {
    if (!e || !callee || !call_form)
        return e ? e->n_refine_call_sites : 0;
    /* A form can be elaborated more than once (macro expansion, specialization
     * retries).  Deduplicate so one source call site yields one diagnostic. */
    if (rt_cs_seen(e, callee, call_form, e->n_refine_call_sites))
        return e->n_refine_call_sites;
    if (e->n_refine_call_sites == e->cap_refine_call_sites) {
        uint32_t ncap = e->cap_refine_call_sites ? e->cap_refine_call_sites * 2 : 16;
        RefineCallSite *nb = (RefineCallSite *)arena_alloc(e->arena,
                                                           ncap * sizeof(RefineCallSite));
        if (e->n_refine_call_sites)
            memcpy(nb, e->refine_call_sites,
                   e->n_refine_call_sites * sizeof(RefineCallSite));
        e->refine_call_sites = nb;
        e->cap_refine_call_sites = ncap;
    }
    /* Prefer the name at the call site over the binding's, which for a
     * typeclass method is the mangled instance symbol.  A leading '.' is the
     * dispatch marker, not part of the method name. */
    const char *display = callee->name ? callee->name->name : "?";
    if (call_form->tag == F_LIST && call_form->as.list.len > 0) {
        const Form *head = call_form->as.list.items[0];
        if (head->tag == F_SYM && head->as.sym && head->as.sym->name) {
            const char *hn = head->as.sym->name;
            display = (hn[0] == '.' && hn[1]) ? hn + 1 : hn;
        }
    }

    RefineCallSite *cs = &e->refine_call_sites[e->n_refine_call_sites++];
    cs->callee         = callee;
    cs->callee_display = display;
    cs->call_form   = call_form;
    cs->arg_offset  = arg_offset;
    cs->env         = NULL;   /* back-filled by refine_fill_call_site_env */
    cs->caller_body = NULL;
    cs->caller_name = NULL;
    cs->class_param_preds = NULL;
    cs->class_param_vars  = NULL;
    cs->n_class_params    = 0;
    /* C2 / #reads: snapshot the borrows LIVE at this crossing.  The borrow
     * checker's scope is authoritative about liveness right here (a `frozen`
     * region's `(& w)` borrow is active); recovering it syntactically later
     * would risk treating an already-ended borrow as live.  A binding borrowed
     * here cannot be `^unique ^mut`-mutated (UT2) or `set!` (borrow-checked),
     * so a `#reads`-it measure is a function of frozen state in this obligation.
     *
     * reads-grant-survives-callee-global-write: that argument is about LOCALS.
     * A mutable GLOBAL is written by NAME rather than passed, so a callee can
     * write one with no syntactic trace at the call site -- and the staleness
     * scan that drops guard hypotheses (rt_collect_set_targets, via
     * rt_push_cs_path_conds) walks only the CALLER body, so it never sees it.
     * Borrowing a global does not stop that: `(& *g*)` followed by a callee's
     * `(set! *g* ...)` compiles today.
     *
     * Publishing such a name here made the `#reads` grant believe a hypothesis
     * over mutable global state, which is exactly what rt_collect_set_targets'
     * own soundness note says never happens -- it leans on rt_classify_expr
     * answering UNKNOWN for a read of any `is_mut` binding (pinned by
     * errors/refine-impure-global-not-congruent).  That covers a global READ
     * inside a predicate; it does not cover a global passed as the ARGUMENT of
     * a `#reads` measure, which reaches congruence by a different door.
     * Withholding mutable globals from the frozen set restores the invariant
     * that note depends on. */
    cs->frozen_names = NULL;
    cs->n_frozen     = 0;
    {
        uint32_t nb = 0;
        for (const Scope *s = e->scope; s; s = s->parent)
            for (const ScopeBorrow *bo = s->borrows; bo; bo = bo->next)
                if (bo->binding && bo->binding->name) nb++;
        if (nb) {
            const char **fn = (const char **)arena_alloc(e->arena, nb * sizeof(char *));
            uint32_t k = 0;
            for (const Scope *s = e->scope; s; s = s->parent)
                for (const ScopeBorrow *bo = s->borrows; bo; bo = bo->next) {
                    Binding *b = bo->binding;
                    if (!b || !b->name) continue;
                    /* Only publish a frozen name that STILL resolves to the
                     * borrowed binding here.  If it has been SHADOWED by an
                     * inner binding of the same spelling, the encoder's
                     * name-based match would otherwise treat the (unfrozen)
                     * shadow as frozen -- an unsoundness the shadow+despawn
                     * fixture pins.  Comparing to scope_lookup's innermost
                     * result closes it. */
                    if (scope_lookup(e->scope, b->name) != b) continue;
                    /* A mutable global is never frozen -- see the note above. */
                    if (b->is_global && b->is_mut) continue;
                    fn[k++] = b->name->name;
                }
            if (k) { cs->frozen_names = fn; cs->n_frozen = k; }
        }
    }
    cs->loc         = call_form->span;
    return e->n_refine_call_sites;
}

void refine_note_macro_expansion(Elab *e, const Form *call,
                                 const Form *expansion) {
    if (!e || !call || !expansion) return;
    for (uint32_t i = 0; i < e->n_refine_mexps; i++) {
        if (e->refine_mexp_calls[i] == call) {
            e->refine_mexp_bodies[i] = expansion;   /* latest re-elaboration */
            return;
        }
    }
    if (e->n_refine_mexps == e->cap_refine_mexps) {
        uint32_t ncap = e->cap_refine_mexps ? e->cap_refine_mexps * 2 : 16;
        const Form **nc = (const Form **)arena_alloc(e->arena,
                                                     ncap * sizeof(Form *));
        const Form **nb = (const Form **)arena_alloc(e->arena,
                                                     ncap * sizeof(Form *));
        if (e->n_refine_mexps) {
            memcpy(nc, e->refine_mexp_calls, e->n_refine_mexps * sizeof(Form *));
            memcpy(nb, e->refine_mexp_bodies, e->n_refine_mexps * sizeof(Form *));
        }
        e->refine_mexp_calls  = nc;
        e->refine_mexp_bodies = nb;
        e->cap_refine_mexps   = ncap;
    }
    e->refine_mexp_calls[e->n_refine_mexps]  = call;
    e->refine_mexp_bodies[e->n_refine_mexps] = expansion;
    e->n_refine_mexps++;
}

/* The expansion recorded for a macro-call form, or NULL.  Linear scan: the
 * table only holds macro calls seen in refined units, and the path walks that
 * consult it are already bounded (RT_CS_PATH_MAX_DEPTH). */
static const Form *rt_macro_expansion(const Elab *e, const Form *call) {
    if (!e || !call || call->tag != F_LIST) return NULL;
    for (uint32_t i = 0; i < e->n_refine_mexps; i++)
        if (e->refine_mexp_calls[i] == call) return e->refine_mexp_bodies[i];
    return NULL;
}

/* Keyed on (callee, call_form) rather than on the index refine_note_call_site
 * returns: that function yields the CURRENT count when it deduplicates, which
 * names the last crossing recorded rather than the matching one.  Attaching the
 * class predicates to the wrong crossing would lint an unrelated call. */
void refine_note_call_site_class_preds(Elab *e, const Binding *callee,
                                       const Form *call_form,
                                       const Form **preds, const char **vars,
                                       uint32_t n_params) {
    if (!e || !callee || !call_form) return;
    for (uint32_t i = e->n_refine_call_sites; i-- > 0; ) {
        RefineCallSite *cs = &e->refine_call_sites[i];
        if (cs->callee != callee || cs->call_form != call_form) continue;
        cs->class_param_preds = preds;
        cs->class_param_vars  = vars;
        cs->n_class_params    = n_params;
        return;
    }
}

void refine_fill_call_site_env(Elab *e, uint32_t from, RefineEnv *env,
                               const char *caller, const Form *body) {
    if (!e) return;
    for (uint32_t i = from; i < e->n_refine_call_sites; i++) {
        if (e->refine_call_sites[i].env) continue;   /* an inner defn already owns it */
        e->refine_call_sites[i].env         = env;
        e->refine_call_sites[i].caller_name = caller;
        e->refine_call_sites[i].caller_body = body;
    }
}

/* ------------------------------------------------------------------------- *
 * Path conditions for call-site crossings.
 *
 * A crossing is collected during elaboration and discharged after the whole
 * unit -- which is what lets it see every callee's refinement, and is why the
 * deferral is load-bearing.  The cost was that a call inside a branch was
 * checked WITHOUT the condition that selected the branch, so the canonical
 * decreasing-argument recursion could not discharge its own recursive call:
 *
 *     (defn f [n : Nat] : Nat
 *       (if (= n 0) 0 (+ 1 (f (- n 1)))))     ; needs n - 1 >= 0
 *
 * `n >= 0` is in the environment and `n != 0` is not, so `n - 1 >= 0` was
 * Unknown and the crossing kept its check.
 *
 * The conditions are recovered SYNTACTICALLY rather than by threading a
 * condition stack through expression elaboration.  `call_form` is a pointer
 * into the caller's body, so walking the body down to that exact node collects
 * every branch that had to be taken to reach it -- no change to any elab_*
 * function, and the facts are the same ones `rt_prove_paths` already
 * synthesizes for return obligations.
 * ------------------------------------------------------------------------- */

#define RT_CS_PATH_MAX_DEPTH 24
/* RT_CS_PATH_MAX_HYPS lives in refine_solver.h beside the other capped
 * quantities, so the TUR_REFINE_STATS reporter can name the limit its peak is
 * a peak of. */

/* Crossing identity that survives macro expansion.
 *
 * Pointer equality is the fast path.  But a region written as a MACRO -- the
 * shipped `ecs/freeze` `frozen`, or any macro that wraps a body with `~@body` --
 * expands by quasiquote, which RECONSTRUCTS the body into fresh Form objects.
 * So the crossing that gets elaborated and registered as `cs->call_form` is a
 * different pointer than the crossing node reachable in `cs->caller_body` (the
 * source defn body), and a pointer-only match reports 0 occurrences -- the guard
 * on the path is then dropped and a perfectly good `(if (alive? w e) ...)` no
 * longer discharges its read (see docs/archive/history/frozen-macro-breaks-refinement-
 * guard-discharge.md).
 *
 * The copy preserves the source SPAN, so we fall back to matching a real
 * source location plus the head symbol and arity.  This only ever RESCUES the
 * "copied once" case: callers keep the `!= 1` ambiguity decline, so a span
 * shared by more than one node (a macro that DUPLICATES a crossing) still bails,
 * and a crossing genuinely absent from the body still matches nothing.  A span
 * match is therefore never less conservative than pointer identity where it
 * mattered -- it strictly recovers copies of the same source crossing, which
 * carry the same guards on the same path. */
static bool rt_form_ident(const Form *a, const Form *b) {
    if (a == b) return true;
    if (!a || !b || a->tag != b->tag) return false;
    if (a->tag != F_LIST && a->tag != F_VEC) return false;   /* only call forms */
    if (span_is_unknown(a->span) || span_is_unknown(b->span)) return false;
    if (a->span.file_id  != b->span.file_id ||
        a->span.off_start != b->span.off_start ||
        a->span.off_end   != b->span.off_end) return false;
    if (a->as.list.len != b->as.list.len || a->as.list.len == 0) return false;
    const Form *ha = a->as.list.items[0], *hb = b->as.list.items[0];
    if (!ha || !hb || ha->tag != F_SYM || hb->tag != F_SYM) return false;
    return ha->as.sym == hb->as.sym;                          /* interned */
}

/* Count occurrences of `target` (pointer- or source-identical), stopping at 2.
 *
 * A macro call counts as its recorded EXPANSION, not its raw arguments: the
 * expansion is the code that elaborated (so a template-GENERATED crossing is
 * only reachable there), and a `~body`-spliced argument appears in BOTH the
 * call form and the expansion -- walking both would double-count one crossing
 * and trip the `!= 1` ambiguity decline. */
static uint32_t rt_form_occurrences(const Elab *e, const Form *node,
                                    const Form *target, uint32_t depth) {
    if (!node || depth >= RT_CS_PATH_MAX_DEPTH) return 0;
    if (rt_form_ident(node, target)) return 1;
    if (node->tag != F_LIST && node->tag != F_VEC) return 0;
    {
        const Form *mx = rt_macro_expansion(e, node);
        if (mx) return rt_form_occurrences(e, mx, target, depth);  /* lateral hop */
    }
    uint32_t n = 0;
    for (uint32_t i = 0; i < node->as.list.len && n < 2; i++)
        n += rt_form_occurrences(e, node->as.list.items[i], target, depth + 1);
    return n;
}

/* Walk down to `target`, appending the condition of every `if` branch entered
 * on the way.  Returns true when the target is in this subtree.
 *
 * A target inside the CONDITION of an `if` gets no fact from that `if`: the
 * condition is evaluated before either branch is chosen, so neither it nor its
 * negation holds there. */
/* True when `name` is already declared in the crossing's environment -- i.e.
 * a binder of that name SHADOWS something the hypotheses talk about. */
static bool rt_env_has_name(RefineEnv *env, const Symbol *sym) {
    if (!env || !sym) return false;
    for (uint32_t j = 0; j < env->n_names; j++)
        if (env->names[j] && strcmp(env->names[j], sym->name) == 0) return true;
    return false;
}

/* Does `f` bind a name that `h` mentions?  Binding positions, conservatively:
 * the names of a `let`/`letrec`/`loop` binding vector; every symbol in any
 * other vector (`fn` parameters, destructuring -- and vector literals, which
 * only costs a fact); every symbol in a `match` pattern; and every symbol in a
 * `handle` clause head.  A false
 * "yes" drops a fact; a false "no" would let a fact about an OUTER binding be
 * read as one about the inner binding of the same name. */
static bool rt_syms_in_mentioned(const Elab *e, const Form *pat, const Form *h,
                                 uint32_t depth) {
    if (!pat || depth >= RT_CS_PATH_MAX_DEPTH) return false;
    if (pat->tag == F_SYM)
        return pat->as.sym && rt_form_mentions_name(e, h, pat->as.sym->name, 0);
    if (pat->tag != F_LIST && pat->tag != F_VEC) return false;
    for (uint32_t i = 0; i < pat->as.list.len; i++)
        if (rt_syms_in_mentioned(e, pat->as.list.items[i], h, depth + 1)) return true;
    return false;
}

static bool rt_rebinds_mentioned(const Elab *e, const Form *f, const Form *h,
                                 uint32_t depth);

/* A `let`-style binding vector: only the NAMES bind (an init is an ordinary
 * expression -- `[e __i]` binds `e`, not `__i`); the inits are walked like any
 * other code.  The shape elab_let accepts: `^ann`* name [type-ann] init. */
static bool rt_bvec_rebinds(const Elab *e, const Form *bv, const Form *h,
                            uint32_t depth) {
    uint32_t i = 0, len = bv->as.list.len;
    while (i < len) {
        const Form *cur = bv->as.list.items[i];
        while (cur->tag == F_SYM && cur->as.sym && cur->as.sym->name[0] == '^') {
            if (++i >= len) return false;
            cur = bv->as.list.items[i];
        }
        if (rt_syms_in_mentioned(e, cur, h, depth + 1)) return true;
        i++;
        if (i < len) {
            const Form *ann = bv->as.list.items[i];
            if (ann->tag == F_TYPE_ANN ||
                (ann->tag == F_KEYWORD && ann->as.sym &&
                 typekind_from_symbol(ann->as.sym->name) != TY_UNKNOWN))
                i++;
        }
        if (i >= len) return false;
        if (rt_rebinds_mentioned(e, bv->as.list.items[i++], h, depth + 1)) return true;
    }
    return false;
}

static bool rt_rebinds_mentioned(const Elab *e, const Form *f, const Form *h,
                                 uint32_t depth) {
    if (!f || !h || depth >= RT_CS_PATH_MAX_DEPTH) return false;
    if (f->tag != F_LIST && f->tag != F_VEC) return false;
    {
        const Form *mx = rt_macro_expansion(e, f);
        if (mx && rt_rebinds_mentioned(e, mx, h, depth)) return true;
    }
    if (f->tag == F_LIST && f->as.list.len >= 3 &&
        (rt_head_is(f, "let") || rt_head_is(f, "letrec") || rt_head_is(f, "loop"))) {
        uint32_t bi = 1;
        if (f->as.list.items[1]->tag == F_SYM && f->as.list.len >= 4) {
            /* A named `let`: the loop's name binds too. */
            if (rt_syms_in_mentioned(e, f->as.list.items[1], h, depth + 1)) return true;
            bi = 2;
        }
        if (f->as.list.items[bi]->tag == F_VEC) {
            if (rt_bvec_rebinds(e, f->as.list.items[bi], h, depth)) return true;
            for (uint32_t i = bi + 1; i < f->as.list.len; i++)
                if (rt_rebinds_mentioned(e, f->as.list.items[i], h, depth + 1)) return true;
            return false;
        }
    }
    if (f->tag == F_VEC)
        for (uint32_t i = 0; i < f->as.list.len; i++) {
            const Form *it = f->as.list.items[i];
            if (it->tag == F_SYM && it->as.sym &&
                rt_form_mentions_name(e, h, it->as.sym->name, 0))
                return true;
        }
    if (rt_head_is(f, "match")) {
        uint32_t i = 2;
        while (i < f->as.list.len) {
            if (rt_syms_in_mentioned(e, f->as.list.items[i++], h, depth + 1)) return true;
            if (i + 1 < f->as.list.len && rt_sym_is(f->as.list.items[i], "when")) i += 2;
            i++;   /* the arm body, walked below */
        }
    }
    if (rt_head_is(f, "handle") || rt_head_is(f, "handle-shallow"))
        for (uint32_t i = 2; i < f->as.list.len; i += 2)
            if (rt_syms_in_mentioned(e, f->as.list.items[i], h, depth + 1)) return true;
    for (uint32_t i = 0; i < f->as.list.len; i++)
        if (rt_rebinds_mentioned(e, f->as.list.items[i], h, depth + 1)) return true;
    return false;
}

/* The names a crossing's match-arm constructor facts must connect to (see the
 * `match` case of rt_collect_path_conds).  Seeded with every name the call
 * form mentions and grown bottom-up: a level whose facts mention one adds the
 * names its facts mention, so an outer arm that binds what an inner match
 * scrutinizes is reached too. */
#define RT_CS_REL_MAX 32
typedef struct RtRel {
    const char *names[RT_CS_REL_MAX];
    uint32_t    n;
} RtRel;

/* Variable names only: a list head is a function or constructor, and a
 * constructor name is in every pattern, so counting one would connect a
 * ground `(Cons 2 (Nil))` argument to every `(Cons h t)` arm. */
static void rt_rel_add_syms(Elab *e, RtRel *rel, const Form *f, uint32_t depth) {
    if (!f || depth >= RT_CS_PATH_MAX_DEPTH) return;
    if (f->tag == F_SYM && f->as.sym) {
        if (elab_lookup_ctor(e, f->as.sym)) return;
        for (uint32_t i = 0; i < rel->n; i++)
            if (strcmp(rel->names[i], f->as.sym->name) == 0) return;
        if (rel->n < RT_CS_REL_MAX) rel->names[rel->n++] = f->as.sym->name;
        return;
    }
    if (f->tag != F_LIST && f->tag != F_VEC) return;
    for (uint32_t i = (f->tag == F_LIST ? 1 : 0); i < f->as.list.len; i++)
        rt_rel_add_syms(e, rel, f->as.list.items[i], depth + 1);
}

static bool rt_rel_mentioned(const Elab *e, const RtRel *rel, const Form *f) {
    for (uint32_t i = 0; i < rel->n; i++)
        if (rt_form_mentions_name(e, f, rel->names[i], 0)) return true;
    return false;
}

/* Append path fact `h`, collected at a level whose descent toward the target
 * continues into `below`.  Dropped when `below` rebinds a name `h` mentions:
 * at the target that name is the INNER binding, and the encoder's one flat
 * namespace would read `h` as a fact about it.  `(let [x 1] (let [x -5] (f x)))`
 * put `x = -5` and `x = 1` side by side, a contradiction that "proved" every
 * crossing under --strict-refine.  Dropping a fact only ever weakens. */
static void rt_cs_push(const Elab *e, const Form **hyps, uint32_t *n,
                       const Form *h, const Form *below) {
    if (rt_rebinds_mentioned(e, below, h, 0)) return;
    if (*n >= RT_CS_PATH_MAX_HYPS) { refine_caps()->path_hyps_hits++; return; }
    hyps[(*n)++] = h;
}

/* `*shadowed` is set when a binder on the path rebinds a name the environment
 * already has hypotheses about.  The caller must then abandon the crossing
 * entirely, not merely drop a fact: the encoder has ONE FLAT NAMESPACE, so the
 * argument form's `x` is encoded as the same variable as the outer `x` and
 * silently inherits its hypotheses.  `(let [x (- x x)] (sdiv 10 x))` under
 * `x > 0` "proved" `x != 0` of a value that is zero.
 *
 * That predates path conditions -- the environment always carried the
 * parameter refinements -- and it was never a miscompile, because a crossing
 * is a diagnostic layer and never elides the callee's own entry check, which
 * still fires. What it was is a WRONG ANSWER: --strict-refine accepted a
 * program that panics. */
static bool rt_collect_path_conds(Elab *e, RefineEnv *env, const Form *node,
                                  const Form *target, const Form **hyps,
                                  uint32_t *n, bool *shadowed, RtRel *rel,
                                  uint32_t depth) {
    if (!node || depth >= RT_CS_PATH_MAX_DEPTH) return false;
    if (rt_form_ident(node, target)) return true;
    if (node->tag != F_LIST && node->tag != F_VEC) return false;

    /* A macro call walks as its recorded EXPANSION (mirrors
     * rt_form_occurrences): a guard or crossing the template GENERATED --
     * e.g. `(if (alive? ~w ~e) (get! ~w ~e) -1)` -- exists only there, and
     * an `if` in the expansion contributes its condition exactly like a
     * written one. */
    {
        const Form *mx = rt_macro_expansion(e, node);
        if (mx) return rt_collect_path_conds(e, env, mx, target, hyps, n,
                                             shadowed, rel, depth);   /* lateral hop */
    }

    if (rt_head_is(node, "if") && node->as.list.len == 4) {
        const Form *c = node->as.list.items[1];
        if (rt_collect_path_conds(e, env, c, target, hyps, n, shadowed, rel, depth + 1))
            return true;
        for (uint32_t br = 2; br <= 3; br++) {
            if (!rt_collect_path_conds(e, env, node->as.list.items[br], target,
                                       hyps, n, shadowed, rel, depth + 1))
                continue;
            if (br == 2) {
                rt_cs_push(e, hyps, n, c, node->as.list.items[br]);
            } else {
                Form **notk = (Form **)arena_alloc(e->arena, 2 * sizeof(Form *));
                notk[0] = form_sym(e->arena, node->span,
                                   symtab_intern(e->st, strslice("not", 3)));
                notk[1] = (Form *)c;
                rt_cs_push(e, hyps, n, form_list(e->arena, node->span, notk, 2),
                           node->as.list.items[br]);
            }
            return true;
        }
        return false;
    }

    /* (let [x v] body) -- `x = v` is a fact for a call in the BODY, and no
     * fact at all for a call inside `v` itself, which runs first.
     *
     * A binding that SHADOWS a name in scope contributes nothing rather than
     * declining the whole path: the equation would be `x = <something
     * mentioning x>` in one flat namespace -- false for every x, and a false
     * hypothesis proves the goal.  The return-obligation splitter alpha-renames
     * instead, which it can because it also rewrites the body; here there is no
     * body to rewrite, only a call form to leave alone, so dropping the one
     * fact is the honest answer.  Every other condition on the path stays. */
    if (rt_head_is(node, "let") && node->as.list.len == 3) {
        const Form *binds = node->as.list.items[1];
        const Form *body  = node->as.list.items[2];
        if (binds && binds->tag == F_VEC && binds->as.list.len == 2 &&
            binds->as.list.items[0]->tag == F_SYM &&
            binds->as.list.items[0]->as.sym) {
            const Form *x = binds->as.list.items[0];
            const Form *v = binds->as.list.items[1];
            if (rt_collect_path_conds(e, env, v, target, hyps, n, shadowed, rel, depth + 1))
                return true;
            if (!rt_collect_path_conds(e, env, body, target, hyps, n, shadowed, rel, depth + 1))
                return false;
            if (rt_env_has_name(env, x->as.sym)) { *shadowed = true; return true; }
            /* A bound FUNCTION is not an arithmetic fact, and asserting it
             * actively costs: the encoder abstracts the lambda to an
             * uninterpreted symbol, and `refine_model_search` declines any VC
             * containing one -- so a goal that was REFUTED with a
             * counterexample degrades to Unknown. `(let [f (fn ...)] (f 0))`
             * against `(> v 0)` stopped being a compile error the moment this
             * equation was added.
             *
             * The general shape of that hazard is worth remembering: adding a
             * hypothesis can never make a goal easier to PROVE incorrectly,
             * but it can make it harder to REFUTE. Hypotheses are not free. */
            if (!rt_head_is(v, "fn")) rt_cs_push(e, hyps, n, rt_form_eq(e, node->span, x, v), body);
            return true;
        }
        /* A wider binding list: descend without claiming any equation. */
    }

    /* (match scrut pat [when g] body ...) -- an arm contributes what selected
     * it: a guard, verbatim; a literal pattern's `(= scrut <lit>)`; and, with
     * `ctor_facts`, a constructor's tag `(= (#dt/tag scrut) k)` and one
     * `(= b (.field scrut))` per record binder -- the facts rt_prove_paths
     * gives a return obligation, spelled the same way so RF4 selects the same
     * arm from them (reflect-two-provable-facts-report-as-not-holding).
     *
     * A binder that shadows a name the environment talks about still abandons
     * the crossing (below); one rebound deeper on the path drops the facts that
     * mention it (rt_cs_push).  The constructor facts are opt-in because their
     * symbols are uninterpreted: `refine_model_search` declines a VC carrying
     * one, so a crossing that is REFUTED today, with a counterexample, would
     * degrade to unknown.  The caller asks for them only where the predicate
     * mentions a reflected measure, which cannot be decided without them. */
    if (rt_head_is(node, "match") && node->as.list.len >= 4) {
        const Form *scrut = node->as.list.items[1];
        const uint32_t len = node->as.list.len;
        if (rt_collect_path_conds(e, env, scrut, target, hyps, n, shadowed, rel, depth + 1))
            return true;
        uint32_t i = 2;
        while (i < len) {
            const Form *pat = node->as.list.items[i++];
            const Form *guard = NULL;
            if (i + 1 < len && rt_sym_is(node->as.list.items[i], "when")) {
                guard = node->as.list.items[i + 1];
                i += 2;
            }
            if (i >= len) return false;            /* malformed: no body */
            const Form *arm = node->as.list.items[i++];
            /* A call inside the guard runs before the arm is chosen. */
            if (guard &&
                rt_collect_path_conds(e, env, guard, target, hyps, n, shadowed, rel, depth + 1))
                return true;
            if (!rt_collect_path_conds(e, env, arm, target, hyps, n, shadowed, rel, depth + 1))
                continue;
            if (pat && (pat->tag == F_LIST || pat->tag == F_VEC))
                for (uint32_t k = 1; k < pat->as.list.len; k++) {
                    const Form *b = pat->as.list.items[k];
                    if (b->tag == F_SYM && rt_env_has_name(env, b->as.sym)) {
                        *shadowed = true;
                        return true;
                    }
                }
            if (guard) rt_cs_push(e, hyps, n, guard, arm);
            if (pat && (pat->tag == F_INT || pat->tag == F_FLOAT))
                rt_cs_push(e, hyps, n, rt_form_eq(e, pat->span, scrut, pat), arm);
            CtorDef *cd = rel ? rt_pat_ctor(e, pat) : NULL;
            /* Only when this arm connects to what the call is about: the
             * scrutinee or a binder is a relevant name.  A ground argument
             * connects to nothing, so its crossing keeps the model search that
             * refutes it with a counterexample. */
            bool connects = cd && (rt_rel_mentioned(e, rel, scrut) ||
                                   rt_rel_mentioned(e, rel, pat));
            if (connects) {
                rt_rel_add_syms(e, rel, scrut, 0);
                rt_rel_add_syms(e, rel, pat, 0);
                rt_cs_push(e, hyps, n,
                           rt_form_eq(e, pat->span,
                                      rt_form_call1(e, pat->span, RT_DT_TAG_FN, scrut),
                                      form_int(e->arena, pat->span, (int64_t)cd->tag)),
                           arm);
                /* Only a record constructor has a field name to select with,
                 * and only an Int-sorted field: the binder is not declared in
                 * this environment, and an undeclared name is read as Int. */
                for (uint32_t k = 1; cd->is_record && k < pat->as.list.len; k++) {
                    const Form *b = pat->as.list.items[k];
                    uint32_t fi = k - 1;
                    if (b->tag != F_SYM || !b->as.sym || fi >= cd->n_fields ||
                        !cd->fields[fi].name ||
                        rt_sort_of_kind(cd->fields[fi].kind) != VS_INT)
                        continue;
                    /* `(Pair _ _)` would equate both fields through one name. */
                    if (b->as.sym->name[0] == '_') continue;
                    bool dup = false;
                    for (uint32_t k2 = 1; k2 < pat->as.list.len && !dup; k2++)
                        dup = k2 != k && pat->as.list.items[k2]->tag == F_SYM &&
                              pat->as.list.items[k2]->as.sym == b->as.sym;
                    if (dup) continue;
                    char acc[128];
                    snprintf(acc, sizeof(acc), ".%s", cd->fields[fi].name);
                    rt_cs_push(e, hyps, n,
                               rt_form_eq(e, b->span, b,
                                          rt_form_call1(e, b->span, acc, scrut)),
                               arm);
                }
            }
            return true;
        }
        return false;
    }

    for (uint32_t i = 0; i < node->as.list.len; i++)
        if (rt_collect_path_conds(e, env, node->as.list.items[i], target, hyps,
                                  n, shadowed, rel, depth + 1))
            return true;
    return false;
}

/* The caller's WHOLE body, as one form the path walk can descend.
 *
 * `caller_body` used to be the defn's LAST body form -- correct for its other
 * job (the return obligation's subject) and wrong here.  A crossing in any
 * earlier form was then invisible to `rt_collect_path_conds`, which walks down
 * to `call_form` and gives up when it does not find it, so the call was
 * checked without the branch that guards it.  A zero-parameter caller showed
 * it plainly: `main`'s last form is the literal `0`, and the walk searched `0`
 * for the call.  See
 * docs/archive/history/refine-callsite-path-conds-lost-multi-form-body.md.
 *
 * A single-form body is passed through unwrapped, so the common case allocates
 * nothing and the resulting tree is byte-identical to before.  A multi-form
 * body is wrapped in a synthetic `(do ...)`: `do` is not one of the three
 * heads the walk treats specially, so it falls to the generic descent, which
 * is exactly the "a later body form is not guarded by an earlier one"
 * semantics wanted here.
 *
 * Widening also widens the two vetoes in rt_push_cs_path_conds, in the safe
 * direction both times: a `set!` anywhere in the body now declines every
 * crossing in it (it used to be checked only against the last form, so an
 * assignment in an earlier form could invalidate a condition unnoticed), and
 * a `call_form` node reachable from two body forms now counts as ambiguous
 * rather than unique. */
static const Form *rt_whole_body(Elab *e, const Form *call, uint32_t body_start) {
    if (!call || call->as.list.len <= body_start) return NULL;
    uint32_t nb = call->as.list.len - body_start;
    if (nb == 1) return call->as.list.items[body_start];
    Form **items = (Form **)arena_alloc(e->arena, (nb + 1) * sizeof(Form *));
    items[0] = form_sym(e->arena, call->span,
                        symtab_intern(e->st, strslice("do", 2)));
    for (uint32_t i = 0; i < nb; i++)
        items[i + 1] = call->as.list.items[body_start + i];
    return form_list(e->arena, call->span, items, nb + 1);
}

/* ========================================================================= *
 * loop-invariants-plan LI2/LI3: the static side of `:invariant`.
 *
 * A `(while c :invariant p body...)` owes the two Floyd-Hoare obligations,
 *
 *     1. initiation     facts-at-entry  |-  p
 *     2. preservation   p, c            |-  p'      (p' = p after the body)
 *
 * and, once both are proved, offers the third to whatever follows the loop:
 *
 *     3. use            p AND (not c)   holds after a normal exit.
 *
 * Checking, never inference: `p` is the user's.  Every decision below fails
 * toward KEEPING a runtime check -- a loop the analysis cannot model is
 * declined (TUR-W0372, both checks kept), never approximated.
 *
 * Three pieces, each small:
 *
 *   - the ENTRY WALK (li_walk) collects the facts that hold at a point of the
 *     function body -- branch conditions, `let` equations, proved loops'
 *     post-facts -- dropping any fact a preceding assignment could have
 *     staled.  It is position-aware where the crossing walk's WF3 filter is
 *     whole-body: an assignment AFTER the point does not stale a fact at it,
 *     but every iteration of an enclosing loop counts as before it.
 *   - the BODY COMPOSER (li_compose) turns a straight-line body into one
 *     sequential substitution, so p' is p with each assigned name replaced by
 *     the expression it holds after the body.  The substitution is applied to
 *     the FORM, not handed to the encoder as RefineSubst: the encoder leaves a
 *     substituted name FREE when its image does not encode, and a free `acc`
 *     in p' is the pre-body `acc` -- which would prove p' from p, the one
 *     answer preservation must never give by accident.
 *   - the REPORTER words the verdicts itself (quiet obligations), because the
 *     generic "refinement on X" headline cannot say which conjunct a body
 *     failed to preserve.
 * ========================================================================= */

#define LI_MAX_FACTS       64
#define LI_MAX_NAMES       32
#define LI_MAX_SUBST       32
#define LI_MAX_BINDS       32
#define LI_WALK_MAX_DEPTH  64
#define LI_TERM_MAX_NODES  400
#define LI_MAX_CONJ        8

/* One fact: `pred`, with `bv` (a refinement's bound variable) standing for the
 * in-scope name `subj` -- the RefineHyp shape, so a parameter refinement rides
 * in unchanged. */
typedef struct LiFact {
    const Form *pred;
    const char *bv;
    const char *subj;
} LiFact;

typedef struct LiFacts {
    LiFact   f[LI_MAX_FACTS];
    uint32_t n;
} LiFacts;

typedef struct LiWalk {
    Elab        *e;
    const Form  *target;
    LiFacts     *facts;
    const char **vol;       /* names some lambda assigns: never a fact */
    uint32_t     n_vol;
    bool         lossy;     /* a fact was dropped on the way down */
    bool         touched;   /* crossed or stepped over an annotated loop */
    /* Crossings only: the caller's env already holds hypotheses (parameter
     * refinements) this walk cannot drop, so a binder on the path that
     * rebinds one of its names makes the crossing unanswerable -- the flat
     * namespace would hand the inner name the outer one's facts.  Loop
     * obligations seed those hypotheses as facts instead, and a fact simply
     * dies with its name. */
    RefineEnv   *shadow_env;
    bool         shadowed;
} LiWalk;

/* `p` holds at the top of every iteration: initiation and preservation. */
static bool li_inductive(const LoopInvSite *s) {
    return s && s->analyzed && s->entry_proven && s->pres_proven;
}

/* ...and `p AND (not c)` holds after the loop, which also needs every exit to
 * be the condition going false. */
static bool li_proven(const LoopInvSite *s) {
    return li_inductive(s) && !s->early_return;
}

/* The latest site recorded for `f`, following a macro call to its expansion
 * (a `for-each`-style macro that expands to a `while`).  Latest first: a form
 * elaborated twice has one site per elaboration, and the enclosing defn is
 * analysing the most recent. */
static LoopInvSite *li_site_for(const Elab *e, const Form *f) {
    for (uint32_t hop = 0; f && hop < 8; hop++) {
        for (uint32_t i = e->n_loop_inv_sites; i-- > 0; )
            if (rt_form_ident(e->loop_inv_sites[i].while_form, f))
                return &e->loop_inv_sites[i];
        const Form *mx = rt_macro_expansion(e, f);
        if (!mx || mx == f) break;
        f = mx;
    }
    return NULL;
}

static Form *li_not(Elab *e, const Form *c) {
    return rt_form_call1(e, c ? c->span : SPAN_UNKNOWN, "not", c);
}

static bool li_name_in(const char *nm, const char *const *names, uint32_t n) {
    if (!nm) return false;
    for (uint32_t i = 0; i < n; i++)
        if (names[i] && strcmp(names[i], nm) == 0) return true;
    return false;
}

static void li_name_add(const char *nm, const char **names, uint32_t *n, uint32_t cap,
                        bool *overflow) {
    if (!nm || li_name_in(nm, names, *n)) return;
    if (*n >= cap) { if (overflow) *overflow = true; return; }
    names[(*n)++] = nm;
}

/* The encoder's own operators -- the heads that are NOT measures. */
static bool li_is_encoder_op(const char *h) {
    static const char *const OPS[] = {
        "+", "-", "*", "/", "mod", "<", "<=", ">", ">=", "=", "==", "not=",
        "!=", "<>", "and", "or", "not", "=>", "implies",
    };
    for (size_t i = 0; i < sizeof(OPS) / sizeof(OPS[0]); i++)
        if (strcmp(h, OPS[i]) == 0) return true;
    return false;
}

/* Does `f` apply a head the encoder would treat as an ABSTRACT measure -- a
 * name that resolves to no function?  Such a term is congruent by
 * construction, which is right for a mathematical function and wrong for a
 * field read (`.x`), a deref (`@`) or any other special form reading mutable
 * state: two occurrences either side of a call that wrote the field would be
 * equated.  The crossing walk tolerates that because a crossing never elides
 * the callee's check; an invariant proof elides a real check, so such a term
 * is kept out of every fact and declines an invariant that reads one. */
static const char *li_abstract_head(Elab *e, const Form *f, uint32_t depth) {
    if (!f || depth > 48) return NULL;
    if (f->tag != F_LIST && f->tag != F_VEC) return NULL;
    if (f->tag == F_LIST && f->as.list.len > 0) {
        const Form *h = f->as.list.items[0];
        if (h->tag == F_SYM && h->as.sym) {
            const char *hn = h->as.sym->name;
            if (!li_is_encoder_op(hn)) {
                RefineFnInfo info;
                memset(&info, 0, sizeof(info));
                if (!rt_resolve_fn(e, hn, &info)) return hn;
            }
        } else if (h->tag != F_SYM) {
            return "<computed head>";
        }
    }
    for (uint32_t i = (f->tag == F_LIST ? 1 : 0); i < f->as.list.len; i++) {
        const char *r = li_abstract_head(e, f->as.list.items[i], depth + 1);
        if (r) return r;
    }
    return NULL;
}

static bool li_fact_mentions(Elab *e, const LiFact *fa, const char *name) {
    if (fa->subj && strcmp(fa->subj, name) == 0) return true;
    if (fa->bv && strcmp(fa->bv, name) == 0) return false;
    return rt_form_mentions_name(e, fa->pred, name, 0);
}

static void li_drop(LiWalk *W, const char *const *names, uint32_t n) {
    if (!n) return;
    uint32_t w = 0;
    for (uint32_t i = 0; i < W->facts->n; i++) {
        bool stale = false;
        for (uint32_t j = 0; j < n && !stale; j++)
            stale = names[j] && li_fact_mentions(W->e, &W->facts->f[i], names[j]);
        if (stale) { W->lossy = true; continue; }
        W->facts->f[w++] = W->facts->f[i];
    }
    W->facts->n = w;
}

static void li_clear(LiWalk *W) {
    if (W->facts->n) W->lossy = true;
    W->facts->n = 0;
}

static void li_note_binder(LiWalk *W, const char *nm) {
    if (!W->shadow_env || !nm) return;
    for (uint32_t i = 0; i < W->shadow_env->n_names; i++)
        if (W->shadow_env->names[i] && strcmp(W->shadow_env->names[i], nm) == 0) {
            W->shadowed = true;
            return;
        }
}

/* Drop every fact mentioning a symbol that appears anywhere in `f` -- the
 * conservative answer to "this form may bind names I cannot read". */
static void li_drop_syms_in(LiWalk *W, const Form *f, uint32_t depth) {
    if (!f) return;
    if (depth > 48) { li_clear(W); if (W->shadow_env) W->shadowed = true; return; }
    if (f->tag == F_SYM && f->as.sym) {
        const char *nm = f->as.sym->name;
        li_drop(W, &nm, 1);
        li_note_binder(W, nm);
        return;
    }
    if (f->tag != F_LIST && f->tag != F_VEC) return;
    for (uint32_t i = 0; i < f->as.list.len; i++)
        li_drop_syms_in(W, f->as.list.items[i], depth + 1);
}

static void li_admit(LiWalk *W, const Form *pred, const char *bv, const char *subj) {
    if (!pred) return;
    if (rt_form_mentions_set(W->e, pred, 0)) return;
    if (li_abstract_head(W->e, pred, 0)) return;
    LiFact fa = { pred, bv, subj };
    for (uint32_t i = 0; i < W->n_vol; i++)
        if (li_fact_mentions(W->e, &fa, W->vol[i])) return;
    if (W->facts->n >= LI_MAX_FACTS) { W->lossy = true; return; }
    W->facts->f[W->facts->n++] = fa;
}

/* Names `f` borrows -- `(& x)` or `&mut x` -- the one channel by which a
 * callee writes a caller's local (turmeric passes by value).  False when a
 * borrow's operand is not a plain name. */
static bool li_collect_borrows(Elab *e, const Form *f, uint32_t depth,
                               const char **names, uint32_t *n) {
    if (!f) return true;
    if (depth >= RT_SET_SCAN_MAX_DEPTH) return false;
    if (f->tag != F_LIST && f->tag != F_VEC) return true;
    const Form *mx = rt_macro_expansion(e, f);
    if (mx) return li_collect_borrows(e, mx, depth, names, n);
    if (f->tag == F_LIST && f->as.list.len >= 2 &&
        (rt_sym_is(f->as.list.items[0], "&") || rt_sym_is(f->as.list.items[0], "&mut"))) {
        const Form *t = f->as.list.items[1];
        if (!t || t->tag != F_SYM || !t->as.sym) return false;
        bool of = false;
        li_name_add(t->as.sym->name, names, n, LI_MAX_NAMES, &of);
        if (of) return false;
    }
    for (uint32_t i = 0; i < f->as.list.len; i++)
        if (!li_collect_borrows(e, f->as.list.items[i], depth + 1, names, n))
            return false;
    return true;
}

/* Everything `f` could do to the facts when it runs in full: drop what its
 * assignments and borrows could stale, or everything when they cannot be
 * read. */
static void li_havoc(LiWalk *W, const Form *f) {
    if (!f) return;
    const char *tg[RT_WF3_MAX_TARGETS];
    uint32_t nt = 0;
    const char *bo[LI_MAX_NAMES];
    uint32_t nb = 0;
    if (!rt_collect_set_targets(W->e, f, 0, tg, &nt) ||
        !li_collect_borrows(W->e, f, 0, bo, &nb)) {
        li_clear(W);
        return;
    }
    li_drop(W, tg, nt);
    li_drop(W, bo, nb);
}

/* `f` ran to completion before the point being walked to.  A proved loop
 * contributes its post-fact `p AND (not c)` -- the havoc above already dropped
 * every fact about a name it assigns, which is what makes assuming `p` of the
 * CURRENT values sound. */
static void li_step_over(LiWalk *W, const Form *f) {
    if (!f) return;
    li_havoc(W, f);
    /* A plain `(set! x v)` statement: after it, `x = v` -- unless `v` reads
     * `x`, whose old value the havoc just forgot. */
    if (rt_head_is(f, "set!") && f->as.list.len == 3 &&
        f->as.list.items[1]->tag == F_SYM && f->as.list.items[1]->as.sym &&
        !rt_form_mentions_set(W->e, f->as.list.items[2], 0) &&
        !rt_form_mentions_name(W->e, f->as.list.items[2],
                               f->as.list.items[1]->as.sym->name, 0))
        li_admit(W, rt_form_eq(W->e, f->span, f->as.list.items[1],
                               f->as.list.items[2]), NULL, NULL);
    LoopInvSite *s = li_site_for(W->e, f);
    if (!s) return;
    W->touched = true;
    if (li_proven(s)) {
        li_admit(W, s->inv, NULL, NULL);
        if (!rt_form_mentions_set(W->e, s->cond, 0))
            li_admit(W, li_not(W->e, s->cond), NULL, NULL);
    }
}

typedef struct LiBind {
    const Form *name;   /* F_SYM, or a destructuring pattern */
    const Form *init;
} LiBind;

/* Read a `let` binding vector: `^ann`* name [type-ann] init, repeated -- the
 * shape elab_let accepts.  False on anything else. */
static bool li_parse_bvec(const Form *bv, LiBind *out, uint32_t cap, uint32_t *n) {
    *n = 0;
    if (!bv || bv->tag != F_VEC) return false;
    uint32_t i = 0, len = bv->as.list.len;
    while (i < len) {
        const Form *cur = bv->as.list.items[i];
        while (cur->tag == F_SYM && cur->as.sym && cur->as.sym->name[0] == '^') {
            if (++i >= len) return false;
            cur = bv->as.list.items[i];
        }
        if (cur->tag != F_SYM && cur->tag != F_VEC) return false;
        i++;
        if (i < len) {
            const Form *ann = bv->as.list.items[i];
            if (ann->tag == F_TYPE_ANN ||
                (ann->tag == F_KEYWORD && ann->as.sym &&
                 typekind_from_symbol(ann->as.sym->name) != TY_UNKNOWN))
                i++;
        }
        if (i >= len || *n >= cap) return false;
        out[*n].name = cur;
        out[*n].init = bv->as.list.items[i++];
        (*n)++;
    }
    return true;
}

/* A `let` binding reached on the way down: the name's old facts die (it may
 * shadow), and `name = init` is born -- unless `init` mentions the name
 * itself, which in one flat namespace is the contradiction `x = x - 1`. */
static void li_bind(LiWalk *W, const Form *name, const Form *init) {
    if (!name) return;
    if (name->tag != F_SYM || !name->as.sym) { li_drop_syms_in(W, name, 0); return; }
    const char *nm = name->as.sym->name;
    li_drop(W, &nm, 1);
    li_note_binder(W, nm);
    if (!init || rt_head_is(init, "fn") || rt_head_is(init, "lambda")) return;
    if (rt_form_mentions_name(W->e, init, nm, 0)) return;
    li_admit(W, rt_form_eq(W->e, name->span, name, init), NULL, NULL);
}

/* Heads that bind nothing: descending through one cannot bring a shadowing
 * name into scope.  Anything else that is not a resolvable function is a
 * special form whose binders this walk cannot read. */
static bool li_head_binder_free(Elab *e, const Form *h) {
    if (!h || h->tag != F_SYM || !h->as.sym) return true;   /* computed head: a call */
    const char *hn = h->as.sym->name;
    if (hn[0] == '.' || li_is_encoder_op(hn)) return true;
    static const char *const FREE[] = {
        "if", "do", "when", "unless", "cond", "case", "set!", "return", "?",
        "@", "&", "rem", "panic", "the", "tur-contract-check",
    };
    for (size_t i = 0; i < sizeof(FREE) / sizeof(FREE[0]); i++)
        if (strcmp(hn, FREE[i]) == 0) return true;
    if (scope_lookup(&e->global, h->as.sym)) return true;
    if (elab_lookup_ctor(e, h->as.sym)) return true;
    return false;
}

static uint32_t li_child_with(Elab *e, const Form *node, const Form *target) {
    for (uint32_t i = 0; i < node->as.list.len; i++)
        if (rt_form_occurrences(e, node->as.list.items[i], target, 0) > 0)
            return i;
    return UINT32_MAX;
}

static bool li_walk(LiWalk *W, const Form *node, uint32_t depth) {
    Elab *e = W->e;
    if (!node || depth >= LI_WALK_MAX_DEPTH) return false;
    if (rt_form_ident(node, W->target)) return true;
    if (node->tag != F_LIST && node->tag != F_VEC) return false;
    {
        const Form *mx = rt_macro_expansion(e, node);
        if (mx) return li_walk(W, mx, depth + 1);
    }
    uint32_t k = li_child_with(e, node, W->target);
    if (k == UINT32_MAX) return false;
    Form *const *it = node->as.list.items;
    uint32_t len = node->as.list.len;
    const char *h = (node->tag == F_LIST && len > 0 && it[0]->tag == F_SYM &&
                     it[0]->as.sym) ? it[0]->as.sym->name : NULL;

    if (h && strcmp(h, "if") == 0 && (len == 3 || len == 4)) {
        if (k == 1) return li_walk(W, it[1], depth + 1);
        li_havoc(W, it[1]);
        if (!rt_form_mentions_set(e, it[1], 0))
            li_admit(W, k == 2 ? it[1] : li_not(e, it[1]), NULL, NULL);
        return li_walk(W, it[k], depth + 1);
    }
    if (h && strcmp(h, "do") == 0) {
        for (uint32_t j = 1; j < k; j++) li_step_over(W, it[j]);
        return li_walk(W, it[k], depth + 1);
    }
    if (h && strcmp(h, "let") == 0 && len >= 2) {
        LiBind b[LI_MAX_BINDS];
        uint32_t nb = 0;
        if (!li_parse_bvec(it[1], b, LI_MAX_BINDS, &nb)) {
            li_havoc(W, it[1]);
            li_drop_syms_in(W, it[1], 0);
            if (k == 1) return li_walk(W, it[1], depth + 1);
        } else {
            for (uint32_t j = 0; j < nb; j++) {
                if (k == 1 && rt_form_occurrences(e, b[j].init, W->target, 0) > 0)
                    return li_walk(W, b[j].init, depth + 1);
                li_step_over(W, b[j].init);
                li_bind(W, b[j].name, b[j].init);
            }
            if (k == 1) return false;
        }
        for (uint32_t j = 2; j < k; j++) li_step_over(W, it[j]);
        return li_walk(W, it[k], depth + 1);
    }
    if (h && (strcmp(h, "fn") == 0 || strcmp(h, "lambda") == 0)) {
        /* A lambda body runs at some later point nothing here can place. */
        li_clear(W);
        for (uint32_t j = 1; j < k; j++)
            if (it[j]->tag == F_VEC) li_drop_syms_in(W, it[j], 0);
        return li_walk(W, it[k], depth + 1);
    }
    if (h && strcmp(h, "while") == 0 && len >= 2) {
        LoopInvSite *s = li_site_for(e, node);
        uint32_t bs = (len > 3 && it[2]->tag == F_KEYWORD &&
                       it[2]->as.sym == e->kw_invariant) ? 4 : 2;
        if (s) W->touched = true;
        /* Any number of iterations may already have run.  Inside the loop
         * only the inductive half is needed: how the loop exits is moot. */
        li_havoc(W, node);
        bool pv = li_inductive(s);
        if (pv) li_admit(W, s->inv, NULL, NULL);
        if (k < bs) return li_walk(W, it[k], depth + 1);
        if (!rt_form_mentions_set(e, it[1], 0)) li_admit(W, it[1], NULL, NULL);
        for (uint32_t j = bs; j < k; j++) li_step_over(W, it[j]);
        return li_walk(W, it[k], depth + 1);
    }
    if (h && strcmp(h, "match") == 0 && len >= 3) {
        if (k == 1) return li_walk(W, it[1], depth + 1);
        li_step_over(W, it[1]);
        uint32_t i = 2;
        while (i < len) {
            const Form *pat = it[i++];
            uint32_t gi = UINT32_MAX;
            if (i + 1 < len && rt_sym_is(it[i], "when")) { gi = i + 1; i += 2; }
            if (i >= len) return false;
            uint32_t bi = i++;
            if (k != gi && k != bi) {
                if (gi != UINT32_MAX) li_havoc(W, it[gi]);
                continue;
            }
            li_drop_syms_in(W, pat, 0);   /* pattern binders shadow */
            if (k == gi) return li_walk(W, it[gi], depth + 1);
            if (gi != UINT32_MAX) {
                li_havoc(W, it[gi]);
                if (!rt_form_mentions_set(e, it[gi], 0)) li_admit(W, it[gi], NULL, NULL);
            }
            if ((pat->tag == F_INT || pat->tag == F_FLOAT) &&
                !rt_form_mentions_set(e, it[1], 0))
                li_admit(W, rt_form_eq(e, pat->span, it[1], pat), NULL, NULL);
            return li_walk(W, it[bi], depth + 1);
        }
        return false;
    }

    /* A call, or a special form this walk does not model.  Argument order is
     * C's (unspecified), so every sibling may already have run; a special form
     * with binders it cannot read also forfeits every fact naming any symbol it
     * holds, since one of them may be a binder shadowing an outer name. */
    for (uint32_t j = (node->tag == F_LIST ? 1 : 0); j < len; j++)
        if (j != k) li_step_over(W, it[j]);
    if (node->tag == F_LIST && len > 0 && !li_head_binder_free(e, it[0]))
        for (uint32_t j = 1; j < len; j++)
            if (j != k) li_drop_syms_in(W, it[j], 0);
    return li_walk(W, it[k], depth + 1);
}

/* Does the code under this form's head run IN PLACE -- where it is written,
 * when control reaches it -- rather than at some later point: a lambda body,
 * an effect-handler clause (`(handle e (Tick [] k) (set! acc -50) ...)` runs
 * during whatever `perform`s Tick), a `defer`, a nested `defn`?  Allow-listed:
 * the structural forms, the encoder's operators, and calls to a resolvable
 * function (whose arguments are evaluated in place).  Anything else is a
 * special form this analysis cannot schedule, and an assignment inside one is
 * treated like an assignment inside a lambda. */
static bool li_head_runs_in_place(Elab *e, const Form *f) {
    const Form *h = f->as.list.items[0];
    if (!h || h->tag != F_SYM || !h->as.sym) return true;   /* computed head: a call */
    const char *hn = h->as.sym->name;
    if (strcmp(hn, "fn") == 0 || strcmp(hn, "lambda") == 0) return false;
    if (hn[0] == '.' || li_is_encoder_op(hn)) return true;
    static const char *const IN_PLACE[] = {
        "do", "let", "if", "when", "unless", "cond", "case", "match", "while",
        "set!", "swap!", "reset!", "return", "?", "@", "&", "&mut", "the",
        "cast", "as", "rem", "panic", "unsafe", "with-region", "bt-scope",
    };
    for (size_t i = 0; i < sizeof(IN_PLACE) / sizeof(IN_PLACE[0]); i++)
        if (strcmp(hn, IN_PLACE[i]) == 0) return true;
    if (elab_lookup_ctor(e, h->as.sym)) return true;
    Binding *b = scope_lookup(&e->global, h->as.sym);
    return b && b->type.kind == TY_FN;
}

/* An INERT shared borrow: `(& x)` bound by a `let` to a name that nothing in
 * the rest of that `let` ever mentions.  Such a borrow cannot be handed to
 * anything, which is the whole reason the borrow channel is a channel.
 *
 * It is the shape `(frozen v ...)` lowers to -- `(let [__f (& v)] ...)`, a
 * marker whose only job is to hold the borrow live so the borrow checker
 * forbids a conflicting `&mut`.  Recorded by FORM IDENTITY (the pointer), so
 * only the borrow actually bound this way is exempted; a second `(& x)`
 * elsewhere in the function is untouched.
 *
 * Why only this shape, when a shared borrow cannot be assigned through at all
 * ("cannot assign through immutable borrow; use `&mut T` for mutation")?
 * Because that is a rule about TURMERIC code, and it is not the only writer.
 * Measured: an inline-C callee handed `(& x)` writes straight through the
 * pointer, and `x` changes with nothing in the caller to see --
 *
 *     (defn poke [p : &int] : int ```c *(int64_t *)p = 99; return 0; ```)
 *     (let [^mut x 1] (poke (& x)) (println x))   ;; prints 99
 *
 * -- so narrowing the volatile set to `&mut` borrows outright, which is what
 * reads-measure-rejected-in-invariant-and-pre item 1 proposed, is unsound.  A
 * borrow nothing can name is the case where that exposure provably cannot
 * arise: there is no call to hand it to. */
static bool li_inert_borrows(Elab *e, const Form *f, uint32_t depth,
                             const Form **out, uint32_t *n) {
    if (!f) return true;
    if (depth >= RT_SET_SCAN_MAX_DEPTH) return false;
    if (f->tag != F_LIST && f->tag != F_VEC) return true;
    const Form *mx = rt_macro_expansion(e, f);
    if (mx) return li_inert_borrows(e, mx, depth, out, n);
    if (f->tag == F_LIST && f->as.list.len >= 2 &&
        (rt_head_is(f, "let") || rt_head_is(f, "let*")) &&
        f->as.list.items[1] && f->as.list.items[1]->tag == F_VEC) {
        const Form *bv = f->as.list.items[1];
        /* ONE binding, and nothing else in the vector.  Deliberately not a
         * general walk over binding specs: pairing a name with its init means
         * reimplementing `let`'s spec grammar (`^mut`, `name : T value`) in a
         * place where getting it wrong the other way -- calling a USED borrow
         * inert -- is unsound, not merely conservative.  A vector of exactly
         * `[sym (& sym)]`, or `[sym : T (& sym)]`, cannot be mispaired.
         *
         * This is the shape the idiom actually takes: a dedicated marker
         * `let`, as in refine-stateful-resizable-bounds.  A marker folded in
         * beside other bindings simply keeps today's conservative answer; to
         * widen this, reuse `let`'s own spec parser rather than guessing. */
        uint32_t ni = 0, vi = 1;
        if (bv->as.list.len == 4 && rt_sym_is(bv->as.list.items[1], ":")) vi = 3;
        else if (bv->as.list.len != 2) vi = 0;          /* no match */
        if (vi) {
            const Form *nm   = bv->as.list.items[ni];
            const Form *init = bv->as.list.items[vi];
            if (nm && nm->tag == F_SYM && nm->as.sym &&
                init && init->tag == F_LIST && init->as.list.len >= 2 &&
                rt_sym_is(init->as.list.items[0], "&")) {
                const Form *t = init->as.list.items[1];
                if (t && t->tag == F_SYM && t->as.sym) {
                    /* Named anywhere in the `let` body?  Then it can flow
                     * somewhere, and it stays a channel. */
                    bool used = false;
                    for (uint32_t k = 2; k < f->as.list.len && !used; k++)
                        used = rt_form_mentions_name(e, f->as.list.items[k],
                                                     nm->as.sym->name, 0);
                    if (!used) {
                        if (*n >= LI_MAX_NAMES) return false;
                        out[(*n)++] = init;
                    }
                }
            }
        }
    }
    for (uint32_t i = 0; i < f->as.list.len; i++)
        if (!li_inert_borrows(e, f->as.list.items[i], depth + 1, out, n))
            return false;
    return true;
}

static bool li_form_in_set(const Form *f, const Form **set, uint32_t n) {
    for (uint32_t i = 0; i < n; i++)
        if (set[i] == f) return true;
    return false;
}

/* The VOLATILE names of a body: those something can rebind with no `set!`
 * at the point it happens.  Two channels:
 *
 *   - a name some `fn`/`lambda` assigns.  A `^mut` a lambda captures lives in
 *     a shared cell (compiled-closure-copies-a-captured-mut), so calling that
 *     lambda -- from anywhere -- rebinds it.  Only a PLAIN symbol target
 *     counts: a place write inside a lambda changes a field, never a local.
 *   - a name borrowed ANYWHERE (`(& x)`, `&mut x`).  The borrow can be bound
 *     to an alias before a loop and handed to a writing callee inside it
 *     (`(zap rm)`), where no scan of the loop sees `x` at all.
 *
 * `inert` exempts the one borrow shape that provably cannot reach a callee
 * (li_inert_borrows).  Pass NULL / 0 to get the unnarrowed set.
 *
 * A volatile name is never a fact, and a loop whose invariant, condition or
 * assignments touch one is declined.  False on an assignment symbol this
 * cannot attribute. */
static bool li_lambda_targets(Elab *e, const Form *f, uint32_t depth, bool in_fn,
                              const char **names, uint32_t *n,
                              const Form **inert, uint32_t n_inert) {
    if (!f) return true;
    if (depth >= RT_SET_SCAN_MAX_DEPTH) return false;
    if (f->tag == F_SYM && f->as.sym) {
        const char *nm = f->as.sym->name;
        if (in_fn && (strcmp(nm, "set!") == 0 || strcmp(nm, "swap!") == 0 ||
                      strcmp(nm, "reset!") == 0))
            return false;
        return true;
    }
    if (f->tag != F_LIST && f->tag != F_VEC) return true;
    const Form *mx = rt_macro_expansion(e, f);
    if (mx) return li_lambda_targets(e, mx, depth, in_fn, names, n, inert, n_inert);
    /* `(handle e clauses...)`: `e` runs in place, the clauses do not. */
    if (!in_fn && f->tag == F_LIST && f->as.list.len >= 2 &&
        (rt_head_is(f, "handle") || rt_head_is(f, "handle-shallow"))) {
        if (!li_lambda_targets(e, f->as.list.items[1], depth + 1, false, names, n,
                               inert, n_inert))
            return false;
        for (uint32_t i = 2; i < f->as.list.len; i++)
            if (!li_lambda_targets(e, f->as.list.items[i], depth + 1, true, names, n,
                                   inert, n_inert))
                return false;
        return true;
    }
    if (f->tag == F_LIST && f->as.list.len > 0 && !li_head_runs_in_place(e, f))
        in_fn = true;
    if (f->tag == F_LIST && f->as.list.len >= 2 &&
        (rt_sym_is(f->as.list.items[0], "&") || rt_sym_is(f->as.list.items[0], "&mut"))) {
        const Form *t = f->as.list.items[1];
        /* A borrow nothing can name reaches no callee, so it stales nothing
         * (li_inert_borrows).  `&mut` is never exempt. */
        if (t && t->tag == F_SYM && t->as.sym &&
            !li_form_in_set(f, inert, n_inert)) {
            bool of = false;
            li_name_add(t->as.sym->name, names, n, LI_MAX_NAMES, &of);
            if (of) return false;
        }
    }
    if (in_fn && f->tag == F_LIST && f->as.list.len >= 2 &&
        (rt_sym_is(f->as.list.items[0], "set!") ||
         rt_sym_is(f->as.list.items[0], "swap!") ||
         rt_sym_is(f->as.list.items[0], "reset!"))) {
        const Form *t = f->as.list.items[1];
        if (t && t->tag == F_SYM && t->as.sym) {
            bool of = false;
            li_name_add(t->as.sym->name, names, n, LI_MAX_NAMES, &of);
            if (of) return false;
        }
        for (uint32_t i = 2; i < f->as.list.len; i++)
            if (!li_lambda_targets(e, f->as.list.items[i], depth + 1, in_fn, names, n,
                                   inert, n_inert))
                return false;
        return true;
    }
    for (uint32_t i = 0; i < f->as.list.len; i++)
        if (!li_lambda_targets(e, f->as.list.items[i], depth + 1, in_fn, names, n,
                               inert, n_inert))
            return false;
    return true;
}

/* A head that leaves the loop by a path other than the condition going false,
 * or re-enters it from a captured continuation.  `panic` is absent on
 * purpose: it diverges rather than exits.
 *
 * `return` is the one exit the analysis models: it leaves the whole function,
 * so the body composer can prune the paths through it (li_compose) and keep
 * initiation and preservation.  LI_EXIT_ANY finds every exit, LI_EXIT_OTHER
 * every exit but `return` (whose operands are still searched), and
 * LI_EXIT_RETURN only `return`. */
typedef enum { LI_EXIT_ANY, LI_EXIT_OTHER, LI_EXIT_RETURN } LiExitMode;

static const char *li_early_exit(Elab *e, const Form *f, uint32_t depth, LiExitMode mode) {
    if (!f || depth >= RT_SET_SCAN_MAX_DEPTH) return f ? "<too deep>" : NULL;
    if (f->tag != F_LIST && f->tag != F_VEC) return NULL;
    const Form *mx = rt_macro_expansion(e, f);
    if (mx) return li_early_exit(e, mx, depth, mode);
    if (f->tag == F_LIST && f->as.list.len > 0) {
        static const char *const EXITS[] = {
            "return", "?", "call/cc", "call/cc*", "escape", "shift", "shift0",
            "cloneable-shift", "serial-shift", "yield",
        };
        const Form *h = f->as.list.items[0];
        if (h->tag == F_SYM && h->as.sym)
            for (size_t i = 0; i < sizeof(EXITS) / sizeof(EXITS[0]); i++) {
                if (strcmp(h->as.sym->name, EXITS[i]) != 0) continue;
                bool is_return = i == 0;
                if (mode == LI_EXIT_ANY || (mode == LI_EXIT_OTHER) != is_return)
                    return EXITS[i];
            }
        /* A lambda's `return` leaves the lambda, not this loop. */
        if (rt_head_is(f, "fn") || rt_head_is(f, "lambda")) return NULL;
    }
    for (uint32_t i = 0; i < f->as.list.len; i++) {
        const char *r = li_early_exit(e, f->as.list.items[i], depth + 1, mode);
        if (r) return r;
    }
    return NULL;
}

static bool li_mentions_return(Elab *e, const Form *f) {
    return li_early_exit(e, f, 0, LI_EXIT_RETURN) != NULL;
}

/* ---- the body composer ------------------------------------------------- */

#define LI_MAX_PATHS  16
#define LI_MAX_PCOND  8

/* One path through the body: the substitution it composes to, and the branch
 * conditions (in PRE-body names -- each is substituted as it is met) that
 * select it.  A straight-line body is one path with no conditions. */
typedef struct LiPath {
    const char *name[LI_MAX_SUBST];
    const Form *img[LI_MAX_SUBST];
    uint32_t    n;
    const Form *cond[LI_MAX_PCOND];
    uint32_t    ncond;
} LiPath;

typedef struct LiPaths {
    LiPath  *p[LI_MAX_PATHS];
    uint32_t n;
} LiPaths;

typedef struct LiComp {
    Elab              *e;
    const LoopInvSite *site;
    const char        *inner[LI_MAX_NAMES];   /* names a body `let` binds */
    uint32_t           n_inner;
    const char        *why;                   /* decline reason, or NULL */
    char               whybuf[192];
    /* The image of a name a nested loop assigned: its value after the loop is
     * unknown, so a later READ of it declines (li_subst).  A sentinel rather
     * than a fresh symbol, because an unconstrained symbol would be given the
     * Int sort whatever the local's type. */
    const Form        *havoc;
} LiComp;

static const Form *li_decline(LiComp *C, const char *fmt, ...) TUR_PRINTF_FMT(2, 3);
static const Form *li_decline(LiComp *C, const char *fmt, ...) {
    if (!C->why) {
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(C->whybuf, sizeof(C->whybuf), fmt, ap);
        va_end(ap);
        C->why = C->whybuf;
    }
    return NULL;
}

static uint32_t li_tree_size(const Form *f, uint32_t cap) {
    if (!f) return 0;
    if (f->tag != F_LIST && f->tag != F_VEC) return 1;
    uint32_t n = 1;
    for (uint32_t i = 0; i < f->as.list.len && n <= cap; i++)
        n += li_tree_size(f->as.list.items[i], cap);
    return n;
}

static const Form *li_image(const LiPath *P, const char *nm) {
    for (uint32_t i = P->n; i-- > 0; )
        if (strcmp(P->name[i], nm) == 0) return P->img[i];
    return NULL;
}

/* `f` with every assigned name replaced by what it holds on path `P`.  A macro
 * call is substituted in its EXPANSION, which is the code that runs -- keeping
 * the call spelling would hand the encoder an unresolvable head it treats as a
 * congruent measure.  A binder inside `f` that would capture an image, or
 * rebind a substituted name, declines. */
static const Form *li_subst(LiComp *C, const LiPath *P, const Form *f, uint32_t depth) {
    if (!f) return NULL;
    if (C->why) return NULL;
    if (depth > 64) return li_decline(C, "an assigned expression is nested too deeply%s", "");
    if (f->tag == F_SYM && f->as.sym) {
        const Form *im = li_image(P, f->as.sym->name);
        if (im && im == C->havoc)
            return li_decline(C, "'%s' is assigned by a nested loop and read "
                                 "after it", f->as.sym->name);
        return im ? im : f;
    }
    if (f->tag != F_LIST && f->tag != F_VEC) return f;
    const Form *mx = rt_macro_expansion(C->e, f);
    if (mx) return li_subst(C, P, mx, depth + 1);
    if (f->tag == F_LIST && f->as.list.len > 0) {
        if (rt_head_is(f, "fn") || rt_head_is(f, "lambda") || rt_head_is(f, "match") ||
            rt_head_is(f, "handle") || rt_head_is(f, "handle-shallow"))
            return li_decline(C, "an assigned expression contains a binding form "
                                 "(`%s`)", f->as.list.items[0]->as.sym->name);
        if (rt_head_is(f, "let")) {
            LiBind b[LI_MAX_BINDS];
            uint32_t nb = 0;
            if (!li_parse_bvec(f->as.list.items[1], b, LI_MAX_BINDS, &nb))
                return li_decline(C, "an assigned expression contains a `let` "
                                     "this analysis cannot read%s", "");
            for (uint32_t j = 0; j < nb; j++) {
                if (b[j].name->tag != F_SYM)
                    return li_decline(C, "an assigned expression destructures%s", "");
                const char *bn = b[j].name->as.sym->name;
                bool clash = li_image(P, bn) != NULL;
                for (uint32_t i = 0; i < P->n && !clash; i++)
                    clash = rt_form_mentions_name(C->e, P->img[i], bn, 0);
                if (clash)
                    return li_decline(C, "an assigned expression's `let` rebinds "
                                         "'%s'", bn);
            }
        }
    }
    uint32_t n = f->as.list.len;
    Form **kids = (Form **)arena_alloc(C->e->arena, (n ? n : 1) * sizeof(Form *));
    bool changed = false;
    for (uint32_t i = 0; i < n; i++) {
        const Form *k = li_subst(C, P, f->as.list.items[i], depth + 1);
        if (!k) return NULL;
        kids[i] = (Form *)k;
        if (k != f->as.list.items[i]) changed = true;
    }
    if (!changed) return f;
    return f->tag == F_LIST ? form_list(C->e->arena, f->span, kids, n)
                            : form_vec(C->e->arena, f->span, kids, n);
}

static bool li_is_site_local(const LiComp *C, const char *nm) {
    return li_name_in(nm, C->site->local_names, C->site->n_locals);
}

static bool li_set_image(LiComp *C, LiPath *P, const char *nm, const Form *img) {
    if (li_tree_size(img, LI_TERM_MAX_NODES) > LI_TERM_MAX_NODES) {
        li_decline(C, "the body's assignments compose into too large a term "
                      "(at '%s')", nm);
        return false;
    }
    for (uint32_t i = 0; i < P->n; i++)
        if (strcmp(P->name[i], nm) == 0) { P->img[i] = img; return true; }
    if (P->n >= LI_MAX_SUBST) {
        li_decline(C, "the body assigns too many names%s", "");
        return false;
    }
    P->name[P->n] = nm;
    P->img[P->n]  = img;
    P->n++;
    return true;
}

static LiPath *li_path_clone(LiComp *C, const LiPath *P) {
    LiPath *q = (LiPath *)arena_alloc(C->e->arena, sizeof(LiPath));
    *q = *P;
    return q;
}

static bool li_path_assume(LiComp *C, LiPath *P, const Form *c) {
    if (P->ncond >= LI_MAX_PCOND) {
        li_decline(C, "the loop body branches too deeply%s", "");
        return false;
    }
    P->cond[P->ncond++] = c;
    return true;
}

/* Is `nm` bound by a `let` somewhere inside `f`?  Lambdas are not entered:
 * a name bound there cannot be assigned by the code around it. */
static bool li_binds_name(Elab *e, const Form *f, const char *nm, uint32_t depth) {
    if (!f || depth >= RT_SET_SCAN_MAX_DEPTH) return false;
    if (f->tag != F_LIST && f->tag != F_VEC) return false;
    const Form *mx = rt_macro_expansion(e, f);
    if (mx) return li_binds_name(e, mx, nm, depth);
    if (rt_head_is(f, "fn") || rt_head_is(f, "lambda")) return false;
    if (rt_head_is(f, "let") && f->as.list.len >= 2) {
        LiBind b[LI_MAX_BINDS];
        uint32_t nb = 0;
        if (li_parse_bvec(f->as.list.items[1], b, LI_MAX_BINDS, &nb))
            for (uint32_t j = 0; j < nb; j++)
                if (b[j].name->tag == F_SYM && b[j].name->as.sym &&
                    strcmp(b[j].name->as.sym->name, nm) == 0)
                    return true;
    }
    for (uint32_t i = 0; i < f->as.list.len; i++)
        if (li_binds_name(e, f->as.list.items[i], nm, depth + 1)) return true;
    return false;
}

/* A loop nested in the body.  It runs an unknown number of times, so nothing
 * it assigns has a known value after it -- but that only matters to code that
 * READS one of those names afterwards.  So rather than declining the outer
 * loop, every name it assigns is havocked on every path: a later statement or
 * the invariant reading one declines then, naming it (li_subst), while the
 * common shape -- an inner counter nobody reads again -- costs nothing.
 *
 * A name it assigns must be an outer local or one a body `let` bound (havocked;
 * when a `let` inside the nested loop shadows one of those, the havoc covers
 * the outer one too, which only loses facts), or bound by a `let` inside the
 * nested loop itself (out of scope after it: nothing to do).  Anything else is
 * declined, as an outer `set!` of it would be.  A nested loop that can
 * `return`, or that mutates a cell or writes through a place, is declined. */
static bool li_compose_nested_loop(LiComp *C, const Form *st, LiPaths *ps, bool ret) {
    Elab *e = C->e;
    if (ret) {
        li_decline(C, "a nested loop in the body can `return`%s", "");
        return false;
    }
    if (rt_form_mentions_name(e, st, "swap!", 0) || rt_form_mentions_name(e, st, "reset!", 0)) {
        li_decline(C, "a nested loop in the body mutates a cell%s", "");
        return false;
    }
    const char *tg[RT_WF3_MAX_TARGETS];
    uint32_t nt = 0;
    if (!rt_collect_set_targets(e, st, 0, tg, &nt)) {
        li_decline(C, "a nested loop in the body assigns in a way this analysis "
                      "cannot attribute%s", "");
        return false;
    }
    for (uint32_t k = 0; k < nt; k++) {
        bool outer = li_is_site_local(C, tg[k]) || li_name_in(tg[k], C->inner, C->n_inner);
        if (!outer) {
            if (li_binds_name(e, st, tg[k], 0)) continue;
            li_decline(C, "a nested loop assigns '%s', which is not a local variable", tg[k]);
            return false;
        }
        for (uint32_t i = 0; i < ps->n; i++)
            if (!li_set_image(C, ps->p[i], tg[k], C->havoc)) return false;
    }
    return true;
}

static bool li_compose(LiComp *C, const Form *st, LiPaths *ps, uint32_t depth) {
    Elab *e = C->e;
    if (!st || C->why) return !C->why;
    if (depth > 32) { li_decline(C, "the loop body is nested too deeply%s", ""); return false; }
    const Form *mx = rt_macro_expansion(e, st);
    if (mx) return li_compose(C, mx, ps, depth + 1);
    /* No assignment anywhere in it: whatever it does, it cannot rebind a
     * local (by-value; borrows and lambda cells are declined up front).  A
     * `return` in it must still be seen -- the paths through it end there. */
    bool ret = li_mentions_return(e, st);
    if (!ret && !rt_form_mentions_set(e, st, 0)) return true;
    if (st->tag != F_LIST || st->as.list.len == 0) {
        li_decline(C, "an assignment operator is used as a value in the loop body%s", "");
        return false;
    }
    if (rt_head_is(st, "return")) {
        for (uint32_t i = 1; i < st->as.list.len; i++)
            if (rt_form_mentions_set(e, st->as.list.items[i], 0) ||
                li_mentions_return(e, st->as.list.items[i])) {
                li_decline(C, "the value the loop body returns assigns%s", "");
                return false;
            }
        /* Every path that reaches here leaves the function: it never gets to
         * the end of the body, so it owes no re-establishment. */
        ps->n = 0;
        return true;
    }
    if (rt_head_is(st, "set!") && st->as.list.len == 3) {
        const Form *t   = st->as.list.items[1];
        const Form *rhs = st->as.list.items[2];
        if (!t || t->tag != F_SYM || !t->as.sym) {
            li_decline(C, "the loop body assigns through a place expression%s", "");
            return false;
        }
        const char *tn = t->as.sym->name;
        if (rt_form_mentions_set(e, rhs, 0)) {
            li_decline(C, "the value assigned to '%s' itself assigns", tn);
            return false;
        }
        if (li_mentions_return(e, rhs)) {
            li_decline(C, "the value assigned to '%s' can `return`", tn);
            return false;
        }
        if (!li_is_site_local(C, tn) && !li_name_in(tn, C->inner, C->n_inner)) {
            li_decline(C, "the loop assigns '%s', which is not a local variable", tn);
            return false;
        }
        for (uint32_t i = 0; i < ps->n; i++) {
            const Form *im = li_subst(C, ps->p[i], rhs, 0);
            if (!im || !li_set_image(C, ps->p[i], tn, im)) return false;
        }
        return true;
    }
    if (rt_head_is(st, "set!")) {
        li_decline(C, "the loop body has a `set!` of an unexpected shape%s", "");
        return false;
    }
    if (rt_head_is(st, "swap!") || rt_head_is(st, "reset!")) {
        li_decline(C, "the loop body mutates a cell with `%s`",
                   st->as.list.items[0]->as.sym->name);
        return false;
    }
    if (rt_head_is(st, "do")) {
        for (uint32_t i = 1; i < st->as.list.len; i++)
            if (!li_compose(C, st->as.list.items[i], ps, depth + 1)) return false;
        return true;
    }
    if (rt_head_is(st, "let") && st->as.list.len >= 2) {
        LiBind b[LI_MAX_BINDS];
        uint32_t nb = 0;
        if (!li_parse_bvec(st->as.list.items[1], b, LI_MAX_BINDS, &nb)) {
            li_decline(C, "the loop body has a `let` this analysis cannot read%s", "");
            return false;
        }
        for (uint32_t j = 0; j < nb; j++) {
            if (b[j].name->tag != F_SYM) {
                li_decline(C, "the loop body destructures in a `let`%s", "");
                return false;
            }
            const char *bn = b[j].name->as.sym->name;
            if (rt_form_mentions_set(e, b[j].init, 0)) {
                li_decline(C, "the initializer of '%s' assigns", bn);
                return false;
            }
            if (li_mentions_return(e, b[j].init)) {
                li_decline(C, "the initializer of '%s' can `return`", bn);
                return false;
            }
            if (li_is_site_local(C, bn) || li_name_in(bn, C->inner, C->n_inner)) {
                li_decline(C, "the loop body's `let` rebinds '%s'", bn);
                return false;
            }
            if (C->n_inner >= LI_MAX_NAMES) {
                li_decline(C, "the loop body binds too many names%s", "");
                return false;
            }
            C->inner[C->n_inner++] = bn;
            for (uint32_t i = 0; i < ps->n; i++) {
                const Form *im = li_subst(C, ps->p[i], b[j].init, 0);
                if (!im || !li_set_image(C, ps->p[i], bn, im)) return false;
            }
        }
        for (uint32_t i = 2; i < st->as.list.len; i++)
            if (!li_compose(C, st->as.list.items[i], ps, depth + 1)) return false;
        return true;
    }
    if (rt_head_is(st, "if") && (st->as.list.len == 3 || st->as.list.len == 4)) {
        /* Split every path in two.  Each side's condition is the branch test
         * AS IT READS AT THAT POINT, i.e. substituted -- so it is a fact about
         * the state at the top of the body, where the obligation's other
         * hypotheses live. */
        const Form *c2 = st->as.list.items[1];
        if (rt_form_mentions_set(e, c2, 0)) {
            li_decline(C, "a branch condition in the loop body assigns%s", "");
            return false;
        }
        if (li_mentions_return(e, c2)) {
            li_decline(C, "a branch condition in the loop body can `return`%s", "");
            return false;
        }
        if (ps->n * 2 > LI_MAX_PATHS) {
            li_decline(C, "the loop body has too many paths to check%s", "");
            return false;
        }
        LiPaths tp, ep;
        tp.n = ep.n = 0;
        for (uint32_t i = 0; i < ps->n; i++) {
            const Form *cs = li_subst(C, ps->p[i], c2, 0);
            if (!cs) return false;
            LiPath *t = li_path_clone(C, ps->p[i]);
            LiPath *f = li_path_clone(C, ps->p[i]);
            if (!li_path_assume(C, t, cs) || !li_path_assume(C, f, li_not(e, cs)))
                return false;
            tp.p[tp.n++] = t;
            ep.p[ep.n++] = f;
        }
        if (!li_compose(C, st->as.list.items[2], &tp, depth + 1)) return false;
        if (st->as.list.len == 4 &&
            !li_compose(C, st->as.list.items[3], &ep, depth + 1)) return false;
        if (tp.n + ep.n > LI_MAX_PATHS) {
            li_decline(C, "the loop body has too many paths to check%s", "");
            return false;
        }
        ps->n = 0;
        for (uint32_t i = 0; i < tp.n; i++) ps->p[ps->n++] = tp.p[i];
        for (uint32_t i = 0; i < ep.n; i++) ps->p[ps->n++] = ep.p[i];
        return true;
    }
    if (rt_head_is(st, "while"))
        return li_compose_nested_loop(C, st, ps, ret);
    const char *in = (st->as.list.items[0]->tag == F_SYM && st->as.list.items[0]->as.sym)
                   ? st->as.list.items[0]->as.sym->name : "a call";
    if (ret && !rt_form_mentions_set(e, st, 0))
        li_decline(C, "the loop can leave early through `return` in a position "
                      "this analysis does not model (inside `%s`)", in);
    else
        li_decline(C, "the loop body assigns in a position this analysis does not "
                      "model (inside `%s`)", in);
    return false;
}

/* ---- the analysis ------------------------------------------------------- */

/* Inputs the defn hands over -- rt_build_env's, so the parameter refinements
 * and `:pre` enter as ordinary facts and die like any other when the name
 * they describe is assigned. */
typedef struct LiFnCtx {
    Binding    **params;
    uint32_t     n_params;
    const Form **ct_param_preds;
    const char **ct_param_varnames;
    const uint32_t *ct_param_param_idx;
    uint32_t     n_ct_param_preds;
    const Form  *ct_pre_form;
    const Form  *body;
    const char  *fn_name;
    const char **vol;
    uint32_t     n_vol;
    bool         vol_unknown;
} LiFnCtx;

static void li_seed_facts(LiWalk *W, const LiFnCtx *F) {
    for (uint32_t i = 0; i < F->n_ct_param_preds; i++) {
        if (!F->ct_param_preds[i]) continue;
        uint32_t pi = F->ct_param_param_idx[i];
        if (pi >= F->n_params || !F->params[pi] || !F->params[pi]->name) continue;
        li_admit(W, F->ct_param_preds[i], F->ct_param_varnames[i],
                 F->params[pi]->name->name);
    }
    if (F->ct_pre_form) li_admit(W, F->ct_pre_form, NULL, NULL);
}

/* A fresh environment for one obligation: every in-scope name at its sort
 * (the loop's locals, innermost first, then the parameters), then `facts`. */
static RefineEnv *li_env(Elab *e, const LoopInvSite *s, const LiFnCtx *F,
                         const LiFacts *facts, const char *const *skip,
                         uint32_t n_skip) {
    RefineEnv *env = refine_env_new(e->arena);
    refine_env_set_resolver(env, rt_resolve_fn, e);
    for (uint32_t i = 0; i < s->n_locals; i++)
        refine_env_declare(env, s->local_names[i], s->local_sorts[i]);
    for (uint32_t i = 0; i < F->n_params; i++) {
        if (!F->params[i] || !F->params[i]->name) continue;
        if (li_name_in(F->params[i]->name->name, s->local_names, s->n_locals)) continue;
        refine_env_declare(env, F->params[i]->name->name,
                           rt_sort_of_kind(F->params[i]->type.kind));
    }
    for (uint32_t i = 0; facts && i < facts->n; i++) {
        bool stale = false;
        for (uint32_t j = 0; j < n_skip && !stale; j++)
            stale = li_fact_mentions(e, &facts->f[i], skip[j]);
        if (!stale)
            refine_env_push(env, facts->f[i].pred, facts->f[i].bv, facts->f[i].subj);
    }
    return env;
}

static VCSort li_local_sort(const LoopInvSite *s, const char *nm) {
    for (uint32_t i = 0; i < s->n_locals; i++)
        if (strcmp(s->local_names[i], nm) == 0) return s->local_sorts[i];
    return VS_INT;
}

/* Split a top-level `(and ...)` so a failure names the conjunct that broke. */
static uint32_t li_conjuncts(const Form *p, const Form **out, uint32_t cap) {
    if (rt_head_is(p, "and") && p->as.list.len >= 3 && p->as.list.len - 1 <= cap) {
        for (uint32_t i = 1; i < p->as.list.len; i++) out[i - 1] = p->as.list.items[i];
        return p->as.list.len - 1;
    }
    out[0] = p;
    return 1;
}

static void li_render(const Form *f, char *buf, size_t cap) {
    buf[0] = '\0';
    if (!f) return;
    Buf b; buf_init(&b);
    form_print(&b, f);
    size_t n = b.len < cap - 1 ? b.len : cap - 1;
    if (b.data) memcpy(buf, b.data, n);
    buf[n] = '\0';
    if (b.len > cap - 1 && cap > 4) memcpy(buf + cap - 4, "...", 4);
    buf_free(&b);
}

static void li_report_decline(Elab *e, const LoopInvSite *s, const char *fn,
                              const char *why) {
    refine_note_invariant_declined();
    diag_emit_with_code(g_strict_refine ? DIAG_ERROR : DIAG_WARNING,
                        s->inv ? s->inv->span : s->span, TUR_W0372_REFINE_UNKNOWN,
                        "the invariant of the while loop in '%s' is not analysed "
                        "statically: %s; both runtime checks kept",
                        fn ? fn : "?", why);
}

/* What a preservation failure was checked under: the invariant and the loop
 * condition at the top of the body, and -- for a branching body -- the branch
 * tests that pick the failing path. */
static void li_path_note(Elab *e, const LoopInvSite *s, Span loc, bool entry,
                         const LiPath *path) {
    (void)e;
    if (entry) return;
    char c[160];
    li_render(s->cond, c, sizeof(c));
    if (!path || path->ncond == 0) {
        diag_emit(DIAG_NOTE, loc, "checked assuming the invariant and the loop "
                  "condition %s hold at the top of the body", c);
        return;
    }
    char pc[320];
    size_t off = 0;
    pc[0] = '\0';
    for (uint32_t i = 0; i < path->ncond && off + 8 < sizeof(pc); i++) {
        char one[160];
        li_render(path->cond[i], one, sizeof(one));
        int k = snprintf(pc + off, sizeof(pc) - off, "%s%s", i ? " and " : "", one);
        if (k < 0) break;
        off += (size_t)k;
        if (off >= sizeof(pc)) { off = sizeof(pc) - 1; break; }
    }
    diag_emit(DIAG_NOTE, loc, "checked assuming the invariant and the loop "
              "condition %s hold at the top of the body, on the path where %s",
              c, pc);
}

/* Decide one obligation quietly and word its diagnostic.  `entry` selects the
 * initiation wording; `lossy` says the entry walk dropped a fact on the way,
 * so an OPEN counterexample may rest on something the analysis forgot rather
 * than on the program -- reported as undecided, not as refuted. */
static bool li_obligation(Elab *e, const LoopInvSite *s, const LiFnCtx *F,
                          RefineEnv *env, const Form *goal, const Form *conj,
                          bool split, bool entry, bool lossy,
                          const char *no_goal_why, const LiPath *path) {
    const char *fn = F->fn_name ? F->fn_name : "?";
    char what[192];
    snprintf(what, sizeof(what), "the invariant of the while loop in '%s' (%s)",
             fn, entry ? "entry" : "preservation");
    Span loc = conj ? conj->span : s->inv->span;
    char cj[160];
    li_render(conj, cj, sizeof(cj));

    RefineObligation *ob = NULL;
    const char *reason = NULL;
    if (goal) {
        ob = refine_collect_obligation(&e->refine_obs, goal, NULL, NULL, VS_BOOL,
                                       "bool", loc, env,
                                       arena_strdup(e->arena, what, strlen(what)), fn);
    } else {
        reason = no_goal_why ? no_goal_why
                             : "the invariant after the body is outside the supported fragment";
    }
    bool proven = false;
    if (ob) {
        ob->quiet = true;
        ob->runtime_guarded = false;   /* the user's own claim; see the plan */
        /* C2 / #reads: the names a live shared borrow pins AND the body is
         * promised not to write (li_frozen_for_site).  Borrow liveness alone
         * is NOT that promise -- see li_name_reads_only for the measurement
         * that settled it.  Without a set, every `(vlen v)` in the invariant
         * encodes as a fresh symbol, so a bound over a container is unknown
         * after the body even inside a region. */
        if (s->n_frozen)
            refine_obligation_set_frozen(ob, s->frozen_names, s->n_frozen);
        proven = refine_discharge_one(ob, e->arena);
        if (!proven && !ob->refuted)
            reason = ob->unknown_reason ? ob->unknown_reason : "the solver returned unknown";
    }
    refine_note_invariant(proven);
    if (proven) return true;

    bool refuted = ob && ob->refuted;
    bool closed  = refuted && ob->refuted_closed;
    /* When the facts that reach the loop refute the goal outright -- `(not q)`
     * provable from them -- every state they admit (and the real entry state
     * is one; the model says they are satisfiable) violates `q`.  That is as
     * definite as a closed goal, and is reported as one.  It also rescues a
     * lossy walk, whose OPEN counterexample might only exist because a fact
     * was forgotten. */
    if (refuted && entry && !closed) {
        RefineObligation probe;
        memset(&probe, 0, sizeof(probe));
        probe.predicate   = li_not(e, goal);
        probe.base_sort   = VS_BOOL;
        probe.loc         = loc;
        probe.env         = env;
        probe.what        = "loop invariant entry refutation";
        probe.speculative = true;
        probe.path_probe  = true;
        if (refine_discharge_one(&probe, e->arena)) closed = true;
    }
    if (refuted && (closed || !entry || !lossy)) {
        if (entry)
            diag_emit_with_code(DIAG_ERROR, loc, TUR_E0371_REFINE_NOT_PROVED,
                                "the invariant of the while loop in '%s' does not "
                                "hold on entry%s%s%s", fn,
                                split ? ": `" : "", split ? cj : "", split ? "`" : "");
        else if (split)
            diag_emit_with_code(DIAG_ERROR, loc, TUR_E0371_REFINE_NOT_PROVED,
                                "the body of the while loop in '%s' does not "
                                "preserve `%s`", fn, cj);
        else
            diag_emit_with_code(DIAG_ERROR, loc, TUR_E0371_REFINE_NOT_PROVED,
                                "the body of the while loop in '%s' does not "
                                "preserve its invariant", fn);
        refine_emit_obligation_notes(ob, e->arena, true, closed);
        li_path_note(e, s, loc, entry, path);
        return false;
    }
    diag_emit_with_code(g_strict_refine ? DIAG_ERROR : DIAG_WARNING, loc,
                        TUR_W0372_REFINE_UNKNOWN,
                        "the invariant of the while loop in '%s' could not be "
                        "proved %s%s%s%s (%s); runtime check kept", fn,
                        entry ? "on entry" : "to be preserved by the body",
                        split ? " for `" : "", split ? cj : "", split ? "`" : "",
                        refuted ? "the counterexample may rest on a fact the "
                                  "analysis could not carry to the loop"
                                : reason);
    if (ob && ob->vc) refine_emit_obligation_notes(ob, e->arena, refuted, false);
    li_path_note(e, s, loc, entry, path);
    return false;
}

/* Would eliding this loop's two checks be OBSERVABLE?
 *
 * Decided by the ONE contract-position gate (rt_pred_observably_impure), on
 * the ELABORATED predicate.  That gate's own comment states the design this
 * restores: "One gate, so every contract position -- parameter, `:pre`,
 * `:post`/return, `:invariant` -- agrees."  This site was the one left asking
 * the plain purity walk (rt_pred_is_impure), which calls a `#reads` measure
 * impure because its body is inline C -- so a PROVED `#reads` invariant still
 * kept both runtime checks, which is item 3 of
 * reads-measure-rejected-in-invariant-and-pre.
 *
 * The carve-out is the gate's, not a new one, and it is narrow in the way that
 * matters here: a DIRECT call to a `#reads` measure does not count, because
 * reading borrowed state is neutral however often it runs -- or never.  A
 * predicate that CHANGES state still counts, so `(>= (tick) 0)` (the
 * counter-bumping shape the fuzzer found as an output divergence, which is why
 * this veto exists at all) still keeps its checks, and so does an unannotated
 * wrapper around a measure, and so does `(vlen (next-vec!))`.
 *
 * Falls back to the Form walk when no elaborated predicate was kept -- that
 * happens only when contracts are not emitted, where there is no check to
 * elide in the first place. */
static bool li_elision_observable(Elab *e, const LoopInvSite *s) {
    if (s->pred_e) return rt_pred_observably_impure(s->pred_e);
    return rt_pred_is_impure(e, s->inv);
}

/* Is every occurrence of `name` in `f` one that a callee has PROMISED not to
 * write?
 *
 * This is the question a loop's frozen set actually turns on, and the reason a
 * live shared borrow does not answer it.  A crossing's frozen set means "a
 * shared borrow of x is live at this POINT", which makes two occurrences of a
 * measure inside one predicate congruent.  A loop needs x unchanged ACROSS THE
 * BODY, and a borrow does not give that: every stdlib container's mutator
 * takes the container BY VALUE -- `vec-push!` is `[v : (Vec A) val : A]`,
 * `#fx{}`, `#writes [v]` -- so there is no conflicting borrow for
 * TUR-E0200 to reject and the mutation is legal inside the region.  Publishing
 * the borrow-liveness set alone proved `(<= (vlen v) 3)` preserved by a body
 * calling `(vec-push! v 7)`; see item 2 of
 * docs/archive/reads-measure-rejected-in-invariant-and-pre.md.
 *
 * So the evidence has to be a WRITE promise, and there are exactly two.
 * `#reads p` is the trusted one: a measure declared that way claims it reads
 * p's state and nothing more.  A callee the walk knows is PURE is the proven
 * one -- `info.pure` means known-pure (that is the bar congruence itself is
 * held to), and something that writes nothing cannot write this.  A pure
 * callee is safe in EVERY position; `#reads` only in the positions its mask
 * names.
 *
 * An occurrence therefore counts only as a DIRECT argument -- a bare symbol --
 * at such a position.  Everything else withholds: a bare mention can be
 * captured, an argument to an unannotated impure callee can be written
 * through, and a term inside a constructed value escapes the loop entirely.
 *
 * False means "withhold the grant", which is always the safe answer, so every
 * shape this cannot model answers false -- an overflowing walk, a computed
 * head, a macro it cannot expand. */
static bool li_name_reads_only(Elab *e, const Form *f, const char *name,
                               uint32_t depth) {
    if (!f) return true;
    if (depth >= RT_SET_SCAN_MAX_DEPTH) return false;
    if (f->tag == F_SYM && f->as.sym)
        return strcmp(f->as.sym->name, name) != 0;
    if (f->tag != F_LIST && f->tag != F_VEC) return true;
    const Form *mx = rt_macro_expansion(e, f);
    if (mx) return li_name_reads_only(e, mx, name, depth + 1);
    if (f->tag == F_LIST && f->as.list.len > 0) {
        const Form *h = f->as.list.items[0];
        if (h && h->tag == F_SYM && h->as.sym) {
            if (strcmp(h->as.sym->name, name) == 0) return false;
            /* A head that resolves to nothing -- a special form (`set!`,
             * `let`, `do`), a builtin, an unannotated callee -- carries a zero
             * mask, so a bare `name` under it withholds.  That is the intended
             * reading for every one of them: `(set! v ...)` writes it, a `let`
             * rebinds it, and an unannotated callee may do either. */
            RefineFnInfo info;
            memset(&info, 0, sizeof(info));
            uint64_t mask = 0;
            bool pure = false;
            /* rt_resolve_fn answers from the GLOBAL scope only, so it will
             * happily hand back stdlib `vec-get`'s `#reads v` for a call that
             * actually goes to a LOCAL binding of that name.  Measured before
             * this check existed: a `(let [vec-get (fn ... (vec-push! vv 7))]
             * ...)` shadow was approved on the global's mask, preservation was
             * "proved", and the check that would have caught the violated
             * bound was elided -- the vector really did grow, to 5.
             *
             * So a promise counts only if the name still RESOLVES to the
             * binding it was read off.  This is the reason the whole filter
             * runs at REGISTRATION: that is the only point where the loop's
             * scope is live to ask. */
            if (scope_lookup(e->scope, h->as.sym) ==
                    scope_lookup(&e->global, h->as.sym) &&
                rt_resolve_fn(e, h->as.sym->name, &info)) {
                mask = info.reads_params_mask;
                pure = info.pure;
                /* A `#reads` promise the measure's own body was seen to break
                 * (W0383), or could not be shown to keep, backs nothing here:
                 * this grant elides a check, with no callee-side backstop.
                 * See reads_scan_frame_writes. */
                if (info.reads_frame_omits_state || info.reads_write_unverified)
                    mask = 0;
            }
            for (uint32_t i = 1; i < f->as.list.len; i++) {
                const Form *a = f->as.list.items[i];
                if (a && a->tag == F_SYM && a->as.sym &&
                    strcmp(a->as.sym->name, name) == 0) {
                    uint32_t p = i - 1;
                    if (!pure && (p >= 64 || !(mask & (UINT64_C(1) << p))))
                        return false;
                    continue;                      /* writes nothing, or reads */
                }
                if (!li_name_reads_only(e, a, name, depth + 1)) return false;
            }
            return true;
        }
    }
    for (uint32_t i = 0; i < f->as.list.len; i++)
        if (!li_name_reads_only(e, f->as.list.items[i], name, depth + 1))
            return false;
    return true;
}

static void li_analyze_one(Elab *e, LoopInvSite *s, const LiFnCtx *F) {
    const char *fn = F->fn_name ? F->fn_name : "?";
    const Form *wf = s->while_form;
    const char *why = s->decline;
    char whybuf[224];

    /* The loop as one form: condition and body, without the annotation. */
    uint32_t nbody = wf->as.list.len > s->body_start ? wf->as.list.len - s->body_start : 0;
    Form **li = (Form **)arena_alloc(e->arena, (nbody + 2) * sizeof(Form *));
    li[0] = form_sym(e->arena, wf->span, symtab_intern(e->st, strslice("do", 2)));
    li[1] = (Form *)s->cond;
    for (uint32_t i = 0; i < nbody; i++) li[2 + i] = wf->as.list.items[s->body_start + i];
    const Form *loop_do = form_list(e->arena, wf->span, li, nbody + 2);

    const char *A[RT_WF3_MAX_TARGETS];
    uint32_t nA = 0;
    if (!why) {
        const char *x = li_early_exit(e, loop_do, 0, LI_EXIT_OTHER);
        if (x) {
            snprintf(whybuf, sizeof(whybuf), "the loop can leave early through `%s`", x);
            why = whybuf;
        }
    }
    /* A `return` leaves the function, so initiation and preservation still
     * stand (li_compose prunes the paths through it); only the post-loop fact
     * is lost, since the condition may still hold where the body returned. */
    s->early_return = !why && li_mentions_return(e, loop_do);
    if (s->early_return && li_mentions_return(e, s->cond))
        why = "the loop condition can `return`";
    if (!why && rt_form_mentions_set(e, s->cond, 0))
        why = "the loop condition assigns";
    if (!why) {
        const char *h = li_abstract_head(e, s->inv, 0);
        if (!h) h = li_abstract_head(e, s->cond, 0);
        if (h) {
            snprintf(whybuf, sizeof(whybuf), "the loop reads `%s`, which the "
                     "analysis cannot treat as a pure function of the loop's "
                     "variables", h);
            why = whybuf;
        }
    }
    LiComp C;
    memset(&C, 0, sizeof(C));
    C.e = e;
    C.site = s;
    C.havoc = form_sym(e->arena, wf->span, symtab_intern(e->st, strslice("~nested-loop~", 13)));
    LiPaths ps;
    ps.n = 1;
    ps.p[0] = (LiPath *)arena_alloc(e->arena, sizeof(LiPath));
    memset(ps.p[0], 0, sizeof(LiPath));
    if (!why) {
        for (uint32_t i = 0; i < nbody && !C.why; i++)
            li_compose(&C, wf->as.list.items[s->body_start + i], &ps, 0);
        if (C.why) why = C.why;
    }
    if (!why && !rt_collect_set_targets(e, loop_do, 0, A, &nA))
        why = "the loop assigns in a way this analysis cannot attribute";
    /* The names the loop depends on: what p and c read, and what it assigns. */
    if (!why) {
        const char *bo[LI_MAX_NAMES];
        uint32_t nb = 0;
        if (!li_collect_borrows(e, loop_do, 0, bo, &nb))
            why = "the loop borrows something other than a plain local";
        for (uint32_t i = 0; i < nb && !why; i++)
            if (li_name_in(bo[i], A, nA) || rt_form_mentions_name(e, s->inv, bo[i], 0) ||
                rt_form_mentions_name(e, s->cond, bo[i], 0)) {
                snprintf(whybuf, sizeof(whybuf), "the loop borrows '%s', so a call "
                         "in it could write it", bo[i]);
                why = whybuf;
            }
    }
    if (!why && F->vol_unknown)
        why = "a lambda in the function assigns in a way this analysis cannot attribute, "
              "or the function borrows too many names";
    for (uint32_t i = 0; i < F->n_vol && !why; i++)
        if (li_name_in(F->vol[i], A, nA) || rt_form_mentions_name(e, s->inv, F->vol[i], 0) ||
            rt_form_mentions_name(e, s->cond, F->vol[i], 0)) {
            snprintf(whybuf, sizeof(whybuf), "'%s' is borrowed, or assigned "
                     "inside a lambda or handler clause, in '%s', so it can change "
                     "with no `set!` in the loop", F->vol[i], fn);
            why = whybuf;
        }
    if (!why && rt_form_occurrences(e, F->body, wf, 0) != 1)
        why = "the loop's position in the function is ambiguous (a macro repeats it)";

    LiFacts facts;
    facts.n = 0;
    LiWalk W;
    memset(&W, 0, sizeof(W));
    W.e = e; W.target = wf; W.facts = &facts; W.vol = F->vol; W.n_vol = F->n_vol;
    if (!why) {
        li_seed_facts(&W, F);
        W.lossy = false;   /* seeding is not a loss */
        if (!li_walk(&W, F->body, 0))
            why = "the loop could not be located in the function body";
    }
    if (why) {
        li_report_decline(e, s, fn, why);
        return;
    }

    s->assigned   = (const char **)arena_alloc(e->arena, (nA ? nA : 1) * sizeof(char *));
    memcpy(s->assigned, A, nA * sizeof(char *));
    s->n_assigned = nA;

    const Form *conj[LI_MAX_CONJ];
    uint32_t nc = li_conjuncts(s->inv, conj, LI_MAX_CONJ);
    bool split = nc > 1;

    /* 1. initiation */
    bool entry_ok = true;
    RefineEnv *env1 = li_env(e, s, F, &facts, NULL, 0);
    for (uint32_t i = 0; i < nc; i++)
        if (!li_obligation(e, s, F, env1, conj[i], split ? conj[i] : NULL, split,
                           true, W.lossy, NULL, NULL))
            entry_ok = false;

    /* 2. preservation: p and c at the top of the body, plus every entry fact
     * about a name the loop never assigns -- those hold on every iteration. */
    bool pres_ok = true;
    for (uint32_t i = 0; i < nc; i++) {
        /* One obligation per path through the body; the first path that
         * fails is the one reported, so a conjunct is named once. */
        for (uint32_t pi = 0; pi < ps.n; pi++) {
            const LiPath *P = ps.p[pi];
            RefineEnv *env2 = li_env(e, s, F, &facts, A, nA);
            refine_env_push(env2, s->inv, NULL, NULL);
            refine_env_push(env2, s->cond, NULL, NULL);
            for (uint32_t k = 0; k < P->ncond; k++)
                refine_env_push(env2, P->cond[k], NULL, NULL);
            C.why = NULL;
            const Form *g = li_subst(&C, P, conj[i], 0);
            if (!li_obligation(e, s, F, env2, g, split ? conj[i] : NULL, split,
                               false, false, C.why, ps.n > 1 ? P : NULL)) {
                pres_ok = false;
                break;
            }
        }
    }

    s->entry_proven = entry_ok;
    s->pres_proven  = pres_ok;

    /* Never elide a check that is itself observable.  The proof still stands
     * for what follows either way -- entry_proven / pres_proven are already
     * set above, so the post-loop fact does not depend on this. */
    if (li_elision_observable(e, s)) return;
    if (entry_ok && s->entry_check) *s->entry_check = e_nil(e, s->span);
    if (pres_ok && s->body_check)   *s->body_check  = e_nil(e, s->span);
}

/* The same loop elaborated again (a retry, a specialization): its verdict is
 * Form-level, so reuse an earlier site's -- and report it once.  False when no
 * earlier site holds a verdict for this loop. */
static bool li_reuse_prior(Elab *e, uint32_t i) {
    LoopInvSite *s = &e->loop_inv_sites[i];
    LoopInvSite *prior = NULL;
    for (uint32_t j = 0; j < i && !prior; j++)
        if (e->loop_inv_sites[j].analyzed &&
            e->loop_inv_sites[j].while_form == s->while_form)
            prior = &e->loop_inv_sites[j];
    if (!prior) return false;
    s->entry_proven = prior->entry_proven;
    s->pres_proven  = prior->pres_proven;
    s->early_return = prior->early_return;
    s->assigned     = prior->assigned;
    s->n_assigned   = prior->n_assigned;
    if (!li_elision_observable(e, s)) {
        if (s->entry_proven && s->entry_check) *s->entry_check = e_nil(e, s->span);
        if (s->pres_proven && s->body_check)   *s->body_check  = e_nil(e, s->span);
    }
    return true;
}

void li_analyze_loops(Elab *e, uint32_t from, Binding **params, uint32_t n_params,
                      const Form **ct_param_preds, const char **ct_param_varnames,
                      const uint32_t *ct_param_param_idx, uint32_t n_ct_param_preds,
                      const Form *ct_pre_form, const Form *body, const char *fn_name) {
    if (!e || from >= e->n_loop_inv_sites) return;
    LiFnCtx F;
    memset(&F, 0, sizeof(F));
    F.params = params; F.n_params = n_params;
    F.ct_param_preds = ct_param_preds; F.ct_param_varnames = ct_param_varnames;
    F.ct_param_param_idx = ct_param_param_idx; F.n_ct_param_preds = n_ct_param_preds;
    F.ct_pre_form = ct_pre_form; F.body = body; F.fn_name = fn_name;
    const char *vol[LI_MAX_NAMES];
    uint32_t nvol = 0;
    /* A borrow bound to a name nothing mentions cannot reach a callee, so it
     * is not a staling channel -- the `(frozen v ...)` marker.  An overflowing
     * or unwalkable scan yields an EMPTY exemption set, which is the
     * conservative direction (every borrow stays volatile). */
    const Form *inert[LI_MAX_NAMES];
    uint32_t n_inert = 0;
    if (!li_inert_borrows(e, body, 0, inert, &n_inert)) n_inert = 0;
    F.vol_unknown = !li_lambda_targets(e, body, 0, false, vol, &nvol,
                                       inert, n_inert);
    F.vol = vol; F.n_vol = nvol;
    for (uint32_t i = from; i < e->n_loop_inv_sites; i++) {
        LoopInvSite *s = &e->loop_inv_sites[i];
        if (s->analyzed) continue;
        s->analyzed = true;
        if (li_reuse_prior(e, i)) continue;
        if (!body) {
            li_report_decline(e, s, fn_name, "the enclosing function has no body");
            continue;
        }
        li_analyze_one(e, s, &F);
    }
}

/* A `definstance` method's loops, decided exactly as a `defn`'s are.  The
 * invariant is written in the instance's body, not inherited from the class,
 * so the class owning the signature changes nothing about what it can prove.
 * The method's parameter refinements -- its own, or the class's it inherits --
 * are the entry facts (indexed by parameter, NULL where there is none). */
void li_analyze_method_loops(Elab *e, uint32_t from, Binding **params,
                             uint32_t n_params, const Binding *mb,
                             const Form *impl_form, uint32_t body_start) {
    if (!e || from >= e->n_loop_inv_sites || !impl_form) return;
    const Form *nm = impl_form->as.list.len ? impl_form->as.list.items[0] : NULL;
    const char *fn = (nm && nm->tag == F_SYM && nm->as.sym) ? nm->as.sym->name : "?";
    uint32_t idx[MAX_FN_ARITY];
    uint32_t n_idx = 0;
    const Form **preds = NULL;
    const char **vars = NULL;
    if (mb && mb->refine_param_preds) {
        n_idx = mb->n_refine_params < MAX_FN_ARITY ? mb->n_refine_params : MAX_FN_ARITY;
        for (uint32_t i = 0; i < n_idx; i++) idx[i] = i;
        preds = mb->refine_param_preds;
        vars  = mb->refine_param_vars;
    }
    li_analyze_loops(e, from, params, n_params, preds, vars, idx, n_idx, NULL,
                     rt_whole_body(e, impl_form, body_start), fn);
}

/* Every annotated loop no definition analysed: one in a top-level `(def f (fn
 * ...))` lambda, or in any form elaborated outside a `defn` or `definstance`
 * method.  Elaboration registered it and gave it both runtime checks; nothing
 * proved or declined it, so without this sweep the annotation would cost those
 * checks and verify nothing, with no diagnostic at any strictness level.
 * Declined explicitly instead -- such a lambda has no definition to anchor its
 * parameter facts to and may be rebound -- so it is reported, and counted in the
 * stats line's `declined` column, like any other runtime-only invariant. */
void li_decline_unanalyzed(Elab *e) {
    if (!e) return;
    for (uint32_t i = 0; i < e->n_loop_inv_sites; i++) {
        LoopInvSite *s = &e->loop_inv_sites[i];
        if (s->analyzed) continue;
        s->analyzed = true;
        if (li_reuse_prior(e, i)) continue;
        refine_note_invariant_declined();
        diag_emit_with_code(g_strict_refine ? DIAG_ERROR : DIAG_WARNING,
                            s->inv ? s->inv->span : s->span, TUR_W0372_REFINE_UNKNOWN,
                            "the invariant of this while loop is not analysed "
                            "statically: it is not inside a `defn` or a "
                            "`definstance` method (a top-level lambda is not "
                            "analysed); both runtime checks kept");
    }
}

/* ---- LI3: the post-loop fact ------------------------------------------ */

static uint32_t li_fresh_ctr;

/* rt_prove_paths' extension under the gate, tried first.  Two shapes:
 *
 *   - a `let` with several bindings, or several body forms -- the shape
 *     every loop accumulator is declared in -- read as the nested
 *     single-binding lets it means (the binding list is sequential);
 *   - a `do` whose statements include a PROVED loop: havoc-and-assume.  Each
 *     name the loop assigns is renamed to a fresh `x~liN` in everything after
 *     the loop (and in the goal), so the hypotheses already in the
 *     environment keep describing the OLD value and constrain nothing
 *     downstream -- that is the havoc; then `p` and `(not c)` over the fresh
 *     names are assumed.  Without the rename the pre-loop `acc = 0` would sit
 *     beside the post-loop `acc`, and a stale fact would prove a fresh lie.
 *
 * `*handled` false means "not my shape": the ordinary cases run unchanged. */
static bool li_prove_paths_ext(Elab *e, const Form *pred, const char *var_name,
                               const Form *subject, TypeKind base_kind,
                               RefineEnv *env, Span loc, uint32_t depth,
                               bool *handled) {
    *handled = false;
    if (!subject) return false;

    if (rt_head_is(subject, "let") && subject->as.list.len >= 3) {
        const Form *bv = subject->as.list.items[1];
        if (bv && bv->tag == F_VEC && bv->as.list.len == 2 &&
            bv->as.list.items[0]->tag == F_SYM && subject->as.list.len == 3)
            return false;   /* the ordinary single-binding case */
        LiBind b[LI_MAX_BINDS];
        uint32_t nb = 0;
        if (!li_parse_bvec(bv, b, LI_MAX_BINDS, &nb) || nb == 0) return false;
        for (uint32_t j = 0; j < nb; j++)
            if (b[j].name->tag != F_SYM || !b[j].name->as.sym) return false;
        uint32_t len = subject->as.list.len;
        const Form *body;
        if (len == 3) {
            body = subject->as.list.items[2];
        } else {
            Form **it = (Form **)arena_alloc(e->arena, (len - 1) * sizeof(Form *));
            it[0] = form_sym(e->arena, subject->span,
                             symtab_intern(e->st, strslice("do", 2)));
            for (uint32_t i = 2; i < len; i++) it[i - 1] = subject->as.list.items[i];
            body = form_list(e->arena, subject->span, it, len - 1);
        }
        for (uint32_t j = nb; j-- > 0; ) {
            Form **pv = (Form **)arena_alloc(e->arena, 2 * sizeof(Form *));
            pv[0] = (Form *)b[j].name;
            pv[1] = (Form *)b[j].init;
            Form **lk = (Form **)arena_alloc(e->arena, 3 * sizeof(Form *));
            lk[0] = form_sym(e->arena, subject->span,
                             symtab_intern(e->st, strslice("let", 3)));
            lk[1] = form_vec(e->arena, bv->span, pv, 2);
            lk[2] = (Form *)body;
            body = form_list(e->arena, subject->span, lk, 3);
        }
        *handled = true;
        return rt_prove_paths(e, pred, var_name, body, base_kind, env, loc, depth);
    }

    if (rt_head_is(subject, "do") && subject->as.list.len >= 3) {
        uint32_t len = subject->as.list.len;
        uint32_t at = 0;
        LoopInvSite *ls = NULL;
        for (uint32_t i = 1; i + 1 < len && !ls; i++) {
            LoopInvSite *s = li_site_for(e, subject->as.list.items[i]);
            if (li_proven(s)) { ls = s; at = i; }
        }
        if (!ls) return false;
        *handled = true;
        for (uint32_t i = 1; i < at; i++)
            if (rt_form_mentions_set(e, subject->as.list.items[i], 0)) return false;
        const Form *rest;
        if (len - 1 - at == 1) {
            rest = subject->as.list.items[len - 1];
        } else {
            uint32_t nr = len - 1 - at;
            Form **it = (Form **)arena_alloc(e->arena, (nr + 1) * sizeof(Form *));
            it[0] = form_sym(e->arena, subject->span,
                             symtab_intern(e->st, strslice("do", 2)));
            for (uint32_t i = 0; i < nr; i++) it[i + 1] = subject->as.list.items[at + 1 + i];
            rest = form_list(e->arena, subject->span, it, nr + 1);
        }
        /* Havoc by renaming the PAST: every hypothesis already in the
         * environment is rewritten so an assigned `x` reads `x~liN` -- the
         * value before the loop, which nothing downstream mentions -- and the
         * loop's `p` and `(not c)` then describe the current `x`.  Renaming
         * the FUTURE instead (the statements after the loop) would give a
         * later loop a renamed copy of its own form, whose invariant and
         * assignments still speak of `x`: two proved loops in a row then
         * judged the final value by the first loop's post-fact. */
        RefineHyp *saved_head  = env->head;
        uint32_t   saved_names = env->n_names;
        const Symbol *xs[RT_WF3_MAX_TARGETS];
        const Symbol *fs[RT_WF3_MAX_TARGETS];
        uint32_t nx = 0;
        for (uint32_t i = 0; i < ls->n_assigned && nx < RT_WF3_MAX_TARGETS; i++) {
            const char *x = ls->assigned[i];
            char fresh[128];
            snprintf(fresh, sizeof(fresh), "%s~li%u", x, ++li_fresh_ctr);
            xs[nx] = symtab_intern(e->st, strslice(x, (uint32_t)strlen(x)));
            fs[nx] = symtab_intern(e->st,
                                   strslice(arena_strdup(e->arena, fresh, strlen(fresh)),
                                            (uint32_t)strlen(fresh)));
            refine_env_declare(env, fs[nx]->name, li_local_sort(ls, x));
            nx++;
        }
        /* Rebuild the chain in order; a hypothesis whose rename declines (a
         * nested binder of the same name) is dropped, which only weakens. */
        RefineHyp *nhead = NULL, **tail = &nhead;
        for (RefineHyp *h = saved_head; h; h = h->next) {
            const Form *pr = h->pred;
            const char *subj = h->subject_name;
            for (uint32_t j = 0; j < nx && pr; j++) {
                if (subj && strcmp(subj, xs[j]->name) == 0) subj = fs[j]->name;
                if (h->bound_var && strcmp(h->bound_var, xs[j]->name) == 0) continue;
                pr = rt_rename_free(e, pr, xs[j], fs[j]);
            }
            if (!pr) continue;
            RefineHyp *c = (RefineHyp *)arena_alloc(e->arena, sizeof(RefineHyp));
            c->pred = pr;
            c->bound_var = h->bound_var;
            c->subject_name = subj;
            c->next = NULL;
            *tail = c;
            tail = &c->next;
        }
        env->head = nhead;
        refine_env_push(env, ls->inv, NULL, NULL);
        refine_env_push(env, li_not(e, ls->cond), NULL, NULL);
        bool ok = rt_prove_paths(e, pred, var_name, rest, base_kind, env, loc, depth + 1);
        env->head = saved_head; env->n_names = saved_names;
        return ok;
    }
    return false;
}

/* LI3 for call-site crossings: when the path from the caller's body to the
 * call crosses or steps over an annotated loop, the position-aware walk
 * replaces the WF3 whole-body filter -- which would drop the loop's own
 * facts, since the loop assigns the very names they mention.  Inside the
 * body the crossing sees `p` (when proved) and `c`, minus whatever the
 * statements before it assigned; after the loop it sees `p AND (not c)`.
 * Returns false when the walk has nothing to add, leaving the crossing to the
 * ordinary path. */
static bool li_cs_path_facts(Elab *e, RefineCallSite *cs, bool *skip) {
    if (e->n_loop_inv_sites == 0) return false;
    if (!cs->env || !cs->caller_body || !cs->call_form) return false;
    if (rt_form_occurrences(e, cs->caller_body, cs->call_form, 0) != 1) return false;
    const char *vol[LI_MAX_NAMES];
    uint32_t nvol = 0;
    /* No exemption here on purpose: this is the CROSSING's path-condition
     * walk, which the loop change is not about.  Narrowing it would move
     * call-site behaviour in the same commit for no measured reason. */
    if (!li_lambda_targets(e, cs->caller_body, 0, false, vol, &nvol, NULL, 0))
        return false;
    LiFacts facts;
    facts.n = 0;
    LiWalk W;
    memset(&W, 0, sizeof(W));
    W.e = e; W.target = cs->call_form; W.facts = &facts;
    W.vol = vol; W.n_vol = nvol; W.shadow_env = cs->env;
    if (!li_walk(&W, cs->caller_body, 0) || !W.touched) return false;
    if (W.shadowed) { *skip = true; return true; }
    refine_cap_peak(&refine_caps()->path_hyps_peak, facts.n);
    for (uint32_t i = 0; i < facts.n; i++)
        refine_env_push(cs->env, facts.f[i].pred, facts.f[i].bv, facts.f[i].subj);
    return true;
}

/* Does predicate `f` call a `^reflect` measure the encoder can unfold? */
static bool rt_pred_mentions_reflected(Elab *e, const Form *f, uint32_t depth) {
    if (!f || depth >= RT_CS_PATH_MAX_DEPTH) return false;
    if (f->tag != F_LIST || f->as.list.len == 0) return false;
    const Form *h = f->as.list.items[0];
    if (h->tag == F_SYM && h->as.sym) {
        RefineFnInfo info;
        memset(&info, 0, sizeof(info));
        if (rt_resolve_fn(e, h->as.sym->name, &info) && info.reflect_body) return true;
    }
    for (uint32_t i = 0; i < f->as.list.len; i++)
        if (rt_pred_mentions_reflected(e, f->as.list.items[i], depth + 1)) return true;
    return false;
}

/* Push this crossing's path conditions onto `cs->env`, returning the saved
 * head so the caller can rewind.  Declines -- pushing nothing -- when the body
 * assigns anywhere, since a condition mentioning a reassigned name may no
 * longer hold at the call, and when `call_form` is reachable by more than one
 * route, which a macro that shares a node can produce and which would make
 * "the" path ambiguous. */
/* r7rs-conformance-program-emits-megabytes-of-c: what rt_push_cs_path_conds
 * learns from a caller's body alone -- the names it assigns (WF3), and whether
 * an assigned name is borrowed somewhere that may write it -- memoized per
 * body for one refine_resolve_call_sites pass.  Each crossing used to walk
 * its caller's whole body (macro expansions included) for this, so a caller
 * with N crossings cost N walks: `main` in a large `#lang r7rs` program holds
 * every top-level form, and this was about 40% of its `emit-c`.  Nothing it
 * reads changes during the pass (the bodies and the macro table are fixed by
 * then). */
typedef struct {
    const Form  *body;          /* NULL = empty slot */
    bool         decline;       /* the whole-body decline: return before any fact */
    uint32_t     nw;
    const char  *wtgt[RT_WF3_MAX_TARGETS];
} RtBodyWrites;
static struct { RtBodyWrites *v; uint32_t cap, n; } g_rt_body_writes;

static void rt_body_writes_reset(void) {
    free(g_rt_body_writes.v);
    g_rt_body_writes.v = NULL;
    g_rt_body_writes.cap = g_rt_body_writes.n = 0;
}

static const RtBodyWrites *rt_body_writes(Elab *e, const Form *body) {
    if (g_rt_body_writes.n * 2 + 2 > g_rt_body_writes.cap) {
        uint32_t ncap = g_rt_body_writes.cap ? g_rt_body_writes.cap * 2 : 64;
        RtBodyWrites *nv = (RtBodyWrites *)calloc(ncap, sizeof *nv);
        if (!nv) return NULL;
        for (uint32_t i = 0; i < g_rt_body_writes.cap; i++) {
            if (!g_rt_body_writes.v[i].body) continue;
            uint64_t x = (uint64_t)(uintptr_t)g_rt_body_writes.v[i].body;
            x ^= x >> 33; x *= 0xff51afd7ed558ccdULL; x ^= x >> 33;
            uint32_t h = (uint32_t)x & (ncap - 1);
            while (nv[h].body) h = (h + 1) & (ncap - 1);
            nv[h] = g_rt_body_writes.v[i];
        }
        free(g_rt_body_writes.v);
        g_rt_body_writes.v = nv;
        g_rt_body_writes.cap = ncap;
    }
    uint64_t x = (uint64_t)(uintptr_t)body;
    x ^= x >> 33; x *= 0xff51afd7ed558ccdULL; x ^= x >> 33;
    uint32_t mask = g_rt_body_writes.cap - 1;
    uint32_t h = (uint32_t)x & mask;
    while (g_rt_body_writes.v[h].body) {
        if (g_rt_body_writes.v[h].body == body) return &g_rt_body_writes.v[h];
        h = (h + 1) & mask;
    }
    RtBodyWrites *w = &g_rt_body_writes.v[h];
    w->body = body;
    g_rt_body_writes.n++;
    /* WF3: an assignment no longer declines the whole body outright.  Collect
     * what the body assigns, so each hypothesis can be tested against it; a
     * frame this analysis cannot vouch for (place-expression target, an
     * unattributable assignment symbol, overflow, too deep) declines. */
    w->nw = 0;
    if (!rt_collect_set_targets(e, body, 0, w->wtgt, &w->nw)) {
        w->decline = true;
        return w;
    }
    /* A borrowed local is the one way a callee could write this frame's slot,
     * so an assigned name that is also borrowed is not provably disjoint --
     * unless every borrow of it provably goes nowhere that writes, which is
     * the question WF2's checked frames made askable.  A body with no checked
     * frame in reach answers "no" and the guard stays as strict as it was. */
    for (uint32_t i = 0; i < w->nw; i++)
        if (rt_form_borrows_name(e, body, w->wtgt[i], 0) &&
            !wf_borrow_write_free(e, body, w->wtgt[i], 0)) {
            w->decline = true;
            return w;
        }
    w->decline = false;
    return w;
}

static RefineHyp *rt_push_cs_path_conds(Elab *e, RefineCallSite *cs,
                                        bool *skip) {
    RefineHyp *saved = cs->env ? cs->env->head : NULL;
    *skip = false;
    if (!cs->env || !cs->caller_body || !cs->call_form) return saved;
    if (li_cs_path_facts(e, cs, skip)) return saved;   /* loop-invariants-plan LI3 */

    /* What the body assigns, and the whole-body decline (rt_body_writes). */
    const RtBodyWrites *bw = rt_body_writes(e, cs->caller_body);
    if (!bw || bw->decline) return saved;
    const char *const *wtgt = bw->wtgt;
    uint32_t nw = bw->nw;

    if (rt_form_occurrences(e, cs->caller_body, cs->call_form, 0) != 1) return saved;

    /* Match-arm constructor facts only where a reflected measure needs them
     * (see the `match` case of rt_collect_path_conds for the cost). */
    bool ctor_facts = false;
    if (g_opt_reflected_measures && cs->callee && cs->callee->refine_param_preds)
        for (uint32_t i = 0; i < cs->callee->n_refine_params && !ctor_facts; i++)
            ctor_facts = rt_pred_mentions_reflected(e, cs->callee->refine_param_preds[i], 0);
    RtRel rel;
    rel.n = 0;
    if (ctor_facts)
        for (uint32_t i = cs->arg_offset; i < cs->call_form->as.list.len; i++)
            rt_rel_add_syms(e, &rel, cs->call_form->as.list.items[i], 0);

    const Form *hyps[RT_CS_PATH_MAX_HYPS];
    uint32_t n = 0;
    bool shadowed = false;
    bool reached = rt_collect_path_conds(e, cs->env, cs->caller_body,
                                         cs->call_form, hyps, &n, &shadowed,
                                         ctor_facts ? &rel : NULL, 0);
    /* Recorded on every crossing, not only the capped ones: a cap that never
     * fires still has to report how close it came.  Saturates at the limit --
     * see RefineCapStats.path_hyps_peak for why it cannot do better. */
    refine_cap_peak(&refine_caps()->path_hyps_peak, n);
    if (!reached) return saved;
    if (shadowed) { *skip = true; return saved; }
    for (uint32_t i = 0; i < n; i++) {
        bool stale = false;
        for (uint32_t j = 0; j < nw && !stale; j++)
            stale = rt_form_mentions_name(e, hyps[i], wtgt[j], 0);
        if (!stale) refine_env_push(cs->env, hyps[i], NULL, NULL);
    }
    return saved;
}

/* TUR-W0377: the resolved instance accepts this argument, but the CLASS
 * signature rejects it outright, so the call is relying on that instance
 * demanding less than its class.
 *
 * Only a DEFINITE violation warns.  `refine_model_search` returning a model
 * with no free variables means the goal folded to false on the values written
 * at the site; an OPEN model only says "not for every input", which for an
 * argument the instance genuinely accepts is not worth a diagnostic.  That
 * keeps the lint silent on `(.scale-by 3 n)` and loud on `(.scale-by 3 0)`.
 *
 * `speculative` keeps the obligation off the ordinary reporting path: its
 * failure is an interface-versus-implementation disagreement with its own
 * wording, not a TUR-E0371 about a value the program cannot supply. */
static void rt_lint_class_leniency(Elab *e, RefineCallSite *cs, uint32_t p,
                                   const Form *cls_pred, const Form *arg,
                                   TypeKind pk, const RefineSubst *subst,
                                   uint32_t n_subst) {
    if (!cls_pred || !cs) return;
    const Binding *callee = cs->callee;
    /* An instance that restated its class predicate has nothing to disagree
     * with -- the two are the same Form. */
    if (callee->refine_param_preds && p < callee->n_refine_params &&
        callee->refine_param_preds[p] == cls_pred) return;

    const char *cvar = (cs->class_param_vars && p < cs->n_class_params)
                     ? cs->class_param_vars[p] : NULL;
    RefineObligation *ob = refine_collect_obligation(
        &e->refine_obs, cls_pred, cvar, arg,
        rt_sort_of_kind(pk), type_name(type_simple(pk, CK_COPY)),
        cs->loc, cs->env, "class signature", cs->caller_name);
    if (!ob) return;
    refine_obligation_set_subst(ob, e->arena, subst, n_subst);
    ob->speculative = true;
    if (refine_discharge_one(ob, e->arena)) return;   /* class admits it too */
    if (!ob->vc) return;
    RefineModel *m = refine_model_search(ob->vc, e->arena);
    if (!m || m->n != 0) return;                      /* not a definite violation */

    const char *cname = cs->callee_display
                      ? cs->callee_display
                      : (callee->name ? callee->name->name : "?");
    diag_emit_with_code(DIAG_WARNING, cs->loc, TUR_W0377_REFINE_INSTANCE_LENIENCY,
                        "argument %u of '%s' is accepted by the resolved "
                        "instance but violates the class signature's refinement",
                        p + 1, cname);
    diag_emit(DIAG_NOTE, cs->loc,
              "the instance demands less than its class, which is legal; this "
              "call would fail if dispatch were dynamic or a stricter instance "
              "existed");
}

void refine_resolve_call_sites(Elab *e) {
    if (!e) return;
    rt_body_writes_reset();
    /* Start at THIS turn's crossings.  Zero for a compile and for a session's
     * first turn, so the whole-program path walks everything exactly as before.
     *
     * The deferral this pass implements is per-UNIT -- "by the end of the unit
     * every callee's predicates are stamped" -- and under an ElabSession a turn
     * IS the unit, so a crossing from an earlier turn was fully resolved at the
     * end of that turn and has nothing left to learn:
     *
     *   - Its callee was already bound when it was recorded (the crossing is
     *     only noted for an `fn_binding` of kind TY_FN), and pass 1 forward-
     *     declares everything the same unit defines, so nothing it depends on
     *     arrives later than the end of its own turn.  A turn that referenced
     *     an unbound name errored, and a failed turn discards the session.
     *   - A later turn REDEFINING the callee shadows it with a NEW Binding
     *     (elab_defn's prior-turn branch), so `cs->callee` still points at the
     *     definition this crossing actually crossed into.  Re-checking turn N's
     *     call against turn N+1's function would be a diagnostic about code
     *     that already ran, under a definition that did not exist when it did.
     *
     * Walking them again was not just wasted work: refine_collect_obligation
     * does not deduplicate, so every old crossing minted a FRESH undischarged
     * obligation each turn, which was then discharged again -- and the RT7 memo
     * reuses only the VERDICT, deliberately ("diagnostics still run below, so a
     * repeated unproven obligation reports at its own source location"), so the
     * report ran again too.  Which turns that reached depends on the crossing:
     * an E0371 fails its own turn and the session is discarded with it, but a
     * crossing with no runtime backstop (a `#reads` measure) or any session
     * under --strict-refine re-reported W0372 on every later turn.
     *
     * A 440-turn session spent 184 ms on one trivial turn that added nothing;
     * see docs/archive/refine-call-sites-re-resolved-every-session-turn.md. */
    for (uint32_t i = e->turn_start_n_refine_call_sites;
         i < e->n_refine_call_sites; i++) {
        RefineCallSite *cs = &e->refine_call_sites[i];
        const Binding *callee = cs->callee;
        /* A crossing is worth walking when EITHER side has a predicate.  An
         * instance that explicitly demands nothing publishes no arrays of its
         * own, and that is exactly the shape TUR-W0377 exists to lint. */
        bool has_inst_preds = callee->refine_param_preds &&
                              callee->n_refine_params > 0;
        bool has_class_preds = cs->class_param_preds && cs->n_class_params > 0;
        if (!has_inst_preds && !has_class_preds) continue;

        const Form *cf = cs->call_form;
        if (!cf || cf->tag != F_LIST) continue;
        uint32_t n_args = cf->as.list.len > cs->arg_offset
                        ? cf->as.list.len - cs->arg_offset : 0;

        /* The branches that had to be taken to reach this call are facts here.
         * Rewound below so one crossing's path never leaks into the next --
         * several crossings share one caller's env. */
        bool cs_skip = false;
        RefineHyp *cs_saved = rt_push_cs_path_conds(e, cs, &cs_skip);
        if (cs_skip) continue;   /* shadowed binder: no honest answer here */

        /* Every callee parameter name maps to the caller's argument in that
         * slot, so a predicate mentioning a SIBLING parameter is checked
         * against the real argument rather than a free variable. */
        RefineSubst subst[MAX_FN_ARITY];
        uint32_t n_subst = 0;
        for (uint32_t p = 0; has_inst_preds && p < callee->n_refine_params &&
                             p < n_args && n_subst < MAX_FN_ARITY; p++) {
            if (!callee->refine_param_names[p]) continue;
            subst[n_subst].name = callee->refine_param_names[p];
            subst[n_subst].form = cf->as.list.items[cs->arg_offset + p];
            n_subst++;
        }

        uint32_t n_slots = has_inst_preds ? callee->n_refine_params : 0;
        if (has_class_preds && cs->n_class_params > n_slots)
            n_slots = cs->n_class_params;
        for (uint32_t p = 0; p < n_slots && p < n_args; p++) {
            const Form *pred = (has_inst_preds && p < callee->n_refine_params)
                             ? callee->refine_param_preds[p] : NULL;
            const Form *cls_pred = (has_class_preds && p < cs->n_class_params)
                                 ? cs->class_param_preds[p] : NULL;
            if (!pred && !cls_pred) continue;
            const Form *arg = cf->as.list.items[cs->arg_offset + p];
            TypeKind pk = (callee->type.kind == TY_FN &&
                           callee->type.as.fn.arg_kinds &&
                           p < callee->type.as.fn.arity)
                        ? callee->type.as.fn.arg_kinds[p] : TY_INT;
            if (!pred) {
                /* The instance demands nothing here.  Nothing to prove -- but
                 * the class may still reject this argument. */
                rt_lint_class_leniency(e, cs, p, cls_pred, arg, pk, subst, n_subst);
                continue;
            }

            const char *cname = cs->callee_display ? cs->callee_display
                              : (callee->name ? callee->name->name : "?");
            char what[160];
            if (cs->caller_name)
                snprintf(what, sizeof(what), "argument %u of '%s' in '%s'",
                         p + 1, cname, cs->caller_name);
            else
                snprintf(what, sizeof(what), "argument %u of '%s'", p + 1, cname);
            const char *what_owned = arena_strdup(e->arena, what, strlen(what));

            RefineObligation *ob = refine_collect_obligation(
                &e->refine_obs, pred, callee->refine_param_vars[p], arg,
                rt_sort_of_kind(pk), type_name(type_simple(pk, CK_COPY)),
                cs->loc, cs->env, what_owned, cs->caller_name);
            if (!ob) continue;
            refine_obligation_set_subst(ob, e->arena, subst, n_subst);
            /* C2 / #reads: carry this crossing's frozen (borrowed) bindings so
             * the encoder can grant a `#reads w` measure congruence when w is
             * one of them. */
            refine_obligation_set_frozen(ob, cs->frozen_names, cs->n_frozen);
            /* The callee checks its own parameters on entry, so an argument
             * we cannot prove is the ordinary case, not news.  What still
             * errors is an argument that is DEFINITELY wrong -- a closed goal
             * that evaluates false -- which is `(safe-div 10 0)` becoming a
             * compile-time failure instead of a runtime panic.
             *
             * C2 / #reads exception: a `#reads`-measure predicate is impure, so
             * the callee's entry check is TUR-E0375-unemittable and is
             * suppressed (see rt_inject_param_checks) -- there is NO runtime
             * backstop for this crossing.  Mark it proof-only so an unproven
             * one is reported (a warning in non-strict, an error in strict)
             * rather than silently trusted, and so its W0372 does not claim a
             * runtime check was kept. */
            bool reads_crossing = rt_pred_reads_measure(e, pred);
            ob->runtime_guarded = !reads_crossing;
            ob->reads_no_runtime = reads_crossing;
            /* R2: only consulted for the W0372 wording; computed only when
             * the crossing is a #reads one at all. */
            ob->reads_grant_refused = reads_crossing &&
                                      rt_pred_reads_measure_refused(e, pred);
            bool inst_ok = refine_discharge_one(ob, e->arena);

            /* Reading B + lint: the obligation above is the resolved INSTANCE's,
             * which is the more precise of the two contracts and the one this
             * call actually has to satisfy.  But when the instance demands less
             * than its class and the argument is one the CLASS rejects
             * outright, the call is leaning on that instance's private
             * leniency -- which is not part of the interface, and is gone the
             * moment dispatch goes dynamic or a stricter instance is added.
             *
             * Only when the instance obligation itself was PROVED: an argument
             * that failed the instance check already has its own E0371 at this
             * span, and a second diagnostic about the class would just be
             * noise. */
            if (inst_ok)
                rt_lint_class_leniency(e, cs, p, cls_pred, arg, pk, subst, n_subst);
        }
        if (cs->env) cs->env->head = cs_saved;
    }
    rt_body_writes_reset();
}

/* Phase 2: defn — (defn name [param1 param2 ...] : return-type body...)
 * For now, we only support : int return type annotation. Param types are
 * inferred from usage. */
static bool fn_type_param_index(const Symbol **type_params, uint8_t n_type_params,
                                const Symbol *sym, uint8_t *out_idx) {
    if (!sym) return false;
    for (uint8_t i = 0; i < n_type_params; i++) {
        if (type_params[i] == sym) {
            if (out_idx) *out_idx = i;
            return true;
        }
    }
    return false;
}

static Type *fn_type_from_form_impl(Elab *e, const Form *form,
                                    const Symbol **type_params,
                                    Kind *type_param_kinds,
                                    uint8_t n_type_params);

/* Public entry for value-type annotations (defn/fn params + returns, defstruct
 * fields, let-bindings). A bare type-level row (`#row{...}`, kind [*]) is
 * rejected here: a value cannot have row type. A row nested inside a
 * constructor application -- `(Query #row{...})` -- is resolved by
 * type_expr_from_form and surfaces as the application's result kind `*`, so it
 * passes. Internal self-calls below go through this wrapper too, so a row is
 * rejected in every value-type sub-position (e.g. an arrow argument). The
 * innermost offender emits once and returns NULL, which propagates without a
 * second diagnostic. See docs/archive/history/row-type-in-value-position-loses-elements.md. */
Type *fn_type_from_form(Elab *e, const Form *form,
                        const Symbol **type_params,
                        Kind *type_param_kinds,
                        uint8_t n_type_params) {
    Type *t = fn_type_from_form_impl(e, form, type_params, type_param_kinds,
                                     n_type_params);
    if (t && t->hkt_kind == KIND_TYPEROW) {
        diag_emit_with_code(DIAG_ERROR, form ? form->span : (Span){0},
            TUR_E0012_KIND_MISMATCH,
            "kind mismatch (TUR-E0012): `#row{...}` is a type-level row "
            "(kind [*]); a value cannot have row type. A row may only appear "
            "as a type argument to a row-kinded ('^&') constructor parameter");
        return NULL;
    }
    return t;
}

static Type *fn_type_from_form_impl(Elab *e, const Form *form,
                                    const Symbol **type_params,
                                    Kind *type_param_kinds,
                                    uint8_t n_type_params) {
    if (!form) return NULL;
    if (form->tag == F_TYPE_ANN && form->as.list.len > 0) {
        return fn_type_from_form_impl(e, form->as.list.items[0],
                                      type_params, type_param_kinds, n_type_params);
    }
    if (form->tag == F_SYM || form->tag == F_KEYWORD) {
        const Symbol *sym = form->as.sym;
        uint8_t idx = 0;
        if (fn_type_param_index(type_params, n_type_params, sym, &idx)) {
            Type *t = (Type *)arena_alloc(e->arena, sizeof(Type));
            *t = type_tyvar_named(sym->name);
            t->hkt_kind = type_param_kinds ? type_param_kinds[idx] : KIND_STAR;
            return t;
        }
        /* CC4: a bare flavored continuation annotation -- :cont (cloneable),
         * :escape-cont (call/cc / escape), or :serial-cont.  type_expr_from_form
         * only knows the bare name "cont" (cloneable); the escape/serial flavors
         * must be preserved here so (k v) sugar dispatches to the right resume
         * runtime (tur_escape_resume for an undelimited call/cc landing). */
        {
            int cflav = cont_flavor_from_name(sym->name);
            if (cflav >= 0) {
                Type *t = (Type *)arena_alloc(e->arena, sizeof(Type));
                *t = type_cont_flavored(TY_INT, (ContFlavor)cflav);
                if (cont_name_is_multishot(sym->name)) t->copy_kind = CK_MULTISHOT;
                return t;
            }
        }
        /* rc-angle-bracket-annotation-becomes-tyvar: a typed reference-family
         * annotation reached through the spaced `: rc<T>` (F_TYPE_ANN) path or a
         * nested type position.  Resolve it here too, so it does not fall through
         * to type_expr_from_form and become a fresh tyvar named "rc<int>".  The
         * fused `:rc<T>` keyword path in the defn/fn ladders handles the keyword
         * form directly. */
        if (sym) {
            Type *rt = rc_family_type_from_keyword_name(e, sym->name, sym->len,
                                                        form->span, NULL,
                                                        type_params, type_param_kinds,
                                                        n_type_params);
            if (rt) return rt;
        }
        Type *t = type_expr_from_form(e, form, NULL, type_params, type_param_kinds, n_type_params);
        return t;
    }
    if (form->tag == F_LIST && form->as.list.len >= 1 &&
        form->as.list.items[0]->tag == F_SYM) {
        const Symbol *head = form->as.list.items[0]->as.sym;
        /* Variadic HKT rows (Layer 5): (row-concat/row-union/row-intersect ...)
         * are type-level row operations handled by type_expr_from_form, not
         * generic type applications. */
        if (head->name && (strcmp(head->name, "row-concat") == 0 ||
                           strcmp(head->name, "row-union") == 0 ||
                           strcmp(head->name, "row-intersect") == 0 ||
                           /* L6 follow-up D: row-canon, the permutation
                            * canonicaliser, is a row-level form like the
                            * other algebra ops -- route it through
                            * type_expr_from_form so signature annotations
                            * see (row-canon #row{...}) as a row, not as a
                            * generic type application. */
                           strcmp(head->name, "row-canon") == 0)) {
            return type_expr_from_form(e, form, NULL, type_params, type_param_kinds, n_type_params);
        }
        /* KB-008/KB-020/KB-018/KB-019: (-> T1 T2), (fn [...] :ret),
         * (lref T), (handler E V R), and the session/role type
         * constructors (Session, Send, Recv, Choose, Branch, Rec,
         * Timeout, Role) are all type-constructor forms with special
         * handling in type_expr_from_form, not generic type applications.
         * Route them through the same path so the spaced annotation form
         * (`x : (-> a b)`, `p : (lref int)`, `ch :(Session (Send ...))`)
         * resolves identically to the keyword form. */
        if (head == e->sym_forall || head == e->sym_exists ||
            head == e->sym_forall_u || head == e->sym_exists_u ||
            head == e->sym_arrow || head == e->sym_fn || head == e->sym_c_fn ||
            /* LS1: borrow *type* heads -- &T / &mut T (and lifetime-annotated
             * &'a T / &mut 'a T) are type-constructor forms, not generic
             * applications.  `&`-headed lists are also caught by has_amp below,
             * but &mut needs its own routing. */
            head == e->sym_ampersand || head == e->sym_borrow_mut ||
            head == e->sym_lref || head == e->sym_handler_type ||
            head == e->sym_session_type ||
            head == e->sym_session_Send || head == e->sym_session_Recv ||
            head == e->sym_session_Choose || head == e->sym_session_Branch ||
            head == e->sym_session_Rec || head == e->sym_session_Timeout ||
            head == e->sym_role_type || head == e->sym_project_type) {
            return type_expr_from_form(e, form, NULL, type_params, type_param_kinds, n_type_params);
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
        /* CC4 / value-typed cont: (cont R) / (escape-cont R) / (serial-cont R)
         * -- a flavored continuation whose result type is R. `cont` is not a
         * generic arrow-kind constructor, so handle the application here.
         *
         * slice 4: the two-arg form (cont BodyT ResetT) additionally pins the
         * resume-value type BodyT, so (k v) checks `v : BodyT`. The one-arg form
         * leaves BodyT unknown (unchecked), preserving existing signatures. */
        {
            int cflav = cont_flavor_from_name(head->name);
            bool cmulti = head->name && cont_name_is_multishot(head->name);
            if (cflav >= 0 && form->as.list.len == 2) {
                Type *ret = fn_type_from_form_impl(e, form->as.list.items[1],
                                              type_params, type_param_kinds, n_type_params);
                Type *t = (Type *)arena_alloc(e->arena, sizeof(Type));
                *t = type_cont_flavored(ret ? ret->kind : TY_INT, (ContFlavor)cflav);
                if (cmulti) t->copy_kind = CK_MULTISHOT;
                return t;
            }
            if (cflav >= 0 && form->as.list.len == 3) {
                Type *bt = fn_type_from_form_impl(e, form->as.list.items[1],
                                              type_params, type_param_kinds, n_type_params);
                Type *rt = fn_type_from_form_impl(e, form->as.list.items[2],
                                              type_params, type_param_kinds, n_type_params);
                Type *t = (Type *)arena_alloc(e->arena, sizeof(Type));
                *t = type_cont_arg_flavored(bt ? bt->kind : TY_UNKNOWN,
                                            rt ? rt->kind : TY_INT, (ContFlavor)cflav);
                if (cmulti) t->copy_kind = CK_MULTISHOT;
                return t;
            }
        }
        /* assoc-types-plan: type-level associated-type projection in annotation
         * position, e.g. `x : (Elem (Vec int))`.  fn_type_from_form builds type
         * applications itself (it never reaches type_expr_from_form's app path),
         * so resolve the projection here too.  Fires only when `head` is a
         * declared associated type member, leaving every ordinary application
         * unchanged. */
        if (form->as.list.len >= 2) {
            uint8_t assoc_idx;
            if (typeclass_env_find_assoc_type(&e->typeclass_env, head, &assoc_idx)) {
                /* assoc-types-2 (MP4): collect N projection arguments for a
                 * multi-parameter class; `(Elem (Vec int))` is the n == 1 path. */
                uint8_t n_args = (uint8_t)(form->as.list.len - 1);
                Type arg_buf[MAX_FN_ARITY];
                if (n_args > MAX_FN_ARITY) {
                    diag_emit(DIAG_ERROR, form->span,
                              "associated type '%s' projected over too many arguments",
                              head->name);
                    return NULL;
                }
                for (uint32_t i = 0; i < n_args; i++) {
                    Type *a = fn_type_from_form(e, form->as.list.items[i + 1],
                                                type_params, type_param_kinds, n_type_params);
                    if (!a) return NULL;
                    arg_buf[i] = *a;
                }
                const Type *bound = typeclass_env_resolve_assoc_type_n(
                    &e->typeclass_env, head, arg_buf, n_args);
                /* associated-type-unusable-nullary-and-generic (half 2): `(Inner
                 * A)` at a signature type variable stays unreduced; a call that
                 * fixes A reduces it (call_bind_assoc_projections). */
                Type proj;
                if (!bound && elab_assoc_projection(e, head, arg_buf, n_args, &proj)) {
                    Type *out = (Type *)arena_alloc(e->arena, sizeof(Type));
                    *out = proj;
                    return out;
                }
                if (!bound) {
                    diag_emit(DIAG_ERROR, form->span,
                              "no instance binding for associated type '%s' at this type",
                              head->name);
                    return NULL;
                }
                Type *out = (Type *)arena_alloc(e->arena, sizeof(Type));
                *out = *bound;
                return out;
            }
        }
        Type *cur = fn_type_from_form_impl(e, form->as.list.items[0],
                                      type_params, type_param_kinds, n_type_params);
        if (!cur) return NULL;
        /* Original constructor head -- carries the positional parameter kinds
         * used for row-kind validation below (cur becomes a TY_APP after the
         * first argument, losing the StructDef). */
        Type head_type = *cur;
        for (uint32_t i = 1; i < form->as.list.len; i++) {
            Form *arg_form = form->as.list.items[i];
            Type *arg = NULL;
            /* SZ8 non-GADT: accept Size GADT literals (Static N) / (Add s s) /
             * (Mul s s) as a type-app argument; lower to a TY_INT placeholder.
             * The real size info lives in the retained Form (param/return
             * annotation), where size_term_from_form recovers it for SZ8
             * cross-parameter unification.  Mirrors the same lifting added to
             * type_expr_from_form's type-app loop. */
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
                    arg = ph;
                }
            }
            if (!arg) {
                arg = fn_type_from_form_impl(e, arg_form,
                                          type_params, type_param_kinds, n_type_params);
            }
            if (!arg) return NULL;
            /* Same peel type_expr_from_form's app loop does -- this loop is the
             * one a `defn`/`fn` parameter annotation actually goes through, so
             * fixing only the other one leaves `(Box #refine{...})` broken in
             * the position people write it. */
            arg = rt_peel_type_arg_contract(arg, arg_form->span);
            if (!check_row_type_arg_kind(head_type, (uint8_t)(i - 1), *arg,
                                         form->as.list.items[i]->span))
                return NULL;
            Type *next = (Type *)arena_alloc(e->arena, sizeof(Type));
            *next = type_app(e->arena, *cur, *arg, form->span);
            cur = next;
        }
        return cur;
    }
    return type_expr_from_form(e, form, NULL, type_params, type_param_kinds, n_type_params);
}

/* Typed-variadic rest: resolve a `& rest :T` annotation form into a rest
 * element kind plus an optional full Type.  The full Type is non-NULL only for
 * user-defined int64_t-carried types (opaque / struct / ADT / type
 * application) so that call sites can compare type identity (e.g. distinguish
 * Route from Middleware); primitives keep rest_full_type == NULL and use the
 * fast TypeKind comparison.  Declared type parameters yield a polymorphic rest
 * (TY_TYVAR, accepts any arg).  An unknown type name is a hard error -- never
 * silently demoted to :int.  `ctx` is the form name for diagnostics
 * ("defn" / "fn"). */
static bool resolve_variadic_rest_type(Elab *e, const Form *type_p,
                                       const Symbol **fn_type_params,
                                       Kind *fn_type_param_kinds,
                                       uint8_t n_fn_type_params,
                                       const char *ctx,
                                       TypeKind *out_kind,
                                       Type **out_full) {
    *out_kind = TY_INT;
    *out_full = NULL;
    const Symbol *sym = type_p->as.sym;
    /* A declared type parameter (e.g. `& rest :A` in a `[A]` defn) is a
     * polymorphic rest that accepts any argument via the TY_TYVAR fast path. */
    uint8_t tpi = 0;
    if (fn_type_param_index(fn_type_params, n_fn_type_params, sym, &tpi)) {
        *out_kind = TY_TYVAR;
        /* Preserve the rest tyvar's *name* so the call site can substitute it
         * in the declared result type (e.g. recover A=int from `(list-of 7)`
         * into the `(List A)` return).  The call-site reader special-cases a
         * TY_TYVAR rest_full_type to keep the homogeneity acceptance path
         * (any single type) rather than treating it as a type-identity match. */
        Type *tv = (Type *)arena_alloc(e->arena, sizeof(Type));
        *tv = type_tyvar_named(sym ? sym->name : NULL);
        *out_full = tv;
        return true;
    }
    Type *rt = fn_type_from_form(e, type_p, fn_type_params,
                                 fn_type_param_kinds, n_fn_type_params);
    /* fn_type_from_form maps any unrecognised name to a named type variable.
     * Since we already handled declared type params above, a TY_TYVAR result
     * here means the name is undefined. */
    if (!rt || rt->kind == TY_TYVAR) {
        diag_emit(DIAG_ERROR, type_p->span,
                  "%s: unknown rest type '%s'", ctx, sym ? sym->name : "?");
        return false;
    }
    *out_kind = rt->kind;
    if (rt->kind == TY_STRUCT || rt->kind == TY_ADT || rt->kind == TY_APP) {
        *out_full = rt;
    }
    return true;
}

static bool fn_type_has_named_tyvar(const Type *t) {
    if (!t) return false;
    switch (t->kind) {
        case TY_TYVAR:
            /* An OPEN slot (fmap-over-underdetermined-constructor-is-a-
             * defless-shell) is no parameter to specialize over: a value of
             * `(Result float B)` is the carrier whatever B is. */
            return t->as.tyvar_.name != NULL && !t->as.tyvar_.open_slot;
        case TY_APP:
            return fn_type_has_named_tyvar(t->as.app.fn) ||
                   fn_type_has_named_tyvar(t->as.app.arg);
        case TY_FN:
            /* poly-hof-constrained-arg-baked-carrier: a function-typed parameter
             * whose signature carries a named tyvar (e.g. `f : (fn [A] int)` on
             * a HOF quantified over A) must stay on the regular generic TY_FN
             * path so it monomorphizes per instantiation -- demoting it to the
             * tur_poly_fn_t carrier bakes the int64-carrier ABI and miscompiles
             * a by-value struct argument.  Without this TY_FN recursion the
             * nested tyvar went undetected and `carrier_ok` wrongly fired. */
            if (fn_type_has_named_tyvar(t->as.fn.result_full_type)) return true;
            if (t->as.fn.arg_full_types) {
                for (uint32_t i = 0; i < t->as.fn.arity; i++) {
                    if (fn_type_has_named_tyvar(t->as.fn.arg_full_types[i])) return true;
                }
            }
            return false;
        case TY_UNION:
            for (uint8_t i = 0; i < t->as.union_.n_members; i++) {
                if (fn_type_has_named_tyvar(t->as.union_.members[i])) return true;
            }
            return false;
        case TY_INTERSECTION:
            for (uint8_t i = 0; i < t->as.intersection_.n_members; i++) {
                if (fn_type_has_named_tyvar(t->as.intersection_.members[i])) return true;
            }
            return false;
        default:
            return false;
    }
}

/* KB-025: append every named type variable appearing in `t` to the elaborator's
 * signature-tyvar set (deduped, capped).  Used so a GADT match arm can tell a
 * legitimately-polymorphic result (`a` is quantified by the fn) apart from a
 * skolem that escapes into a concrete return position. */
static void fn_collect_sig_tyvars(Elab *e, const Type *t) {
    if (!t) return;
    switch (t->kind) {
        case TY_TYVAR:
            if (t->as.tyvar_.name) {
                for (uint8_t i = 0; i < e->n_sig_tyvars; i++) {
                    if (e->sig_tyvars[i] &&
                        strcmp(e->sig_tyvars[i], t->as.tyvar_.name) == 0) {
                        /* van-laarhoven-lens-composition: a later occurrence with
                         * a constructor kind (e.g. `f` heading `(f int)`) upgrades
                         * a kind-* record so a nested lambda recovers `* -> *`. */
                        if (t->hkt_kind != KIND_STAR &&
                            e->sig_tyvar_kinds[i] == KIND_STAR)
                            e->sig_tyvar_kinds[i] = t->hkt_kind;
                        return;
                    }
                }
                if (e->n_sig_tyvars < 32) {
                    e->sig_tyvar_kinds[e->n_sig_tyvars] = t->hkt_kind;
                    e->sig_tyvars[e->n_sig_tyvars++] = t->as.tyvar_.name;
                }
            }
            return;
        case TY_APP:
            fn_collect_sig_tyvars(e, t->as.app.fn);
            fn_collect_sig_tyvars(e, t->as.app.arg);
            return;
        case TY_UNION:
            for (uint8_t i = 0; i < t->as.union_.n_members; i++)
                fn_collect_sig_tyvars(e, t->as.union_.members[i]);
            return;
        case TY_INTERSECTION:
            for (uint8_t i = 0; i < t->as.intersection_.n_members; i++)
                fn_collect_sig_tyvars(e, t->as.intersection_.members[i]);
            return;
        case TY_FN:
            if (t->as.fn.arg_full_types) {
                for (uint32_t i = 0; i < t->as.fn.arity; i++)
                    fn_collect_sig_tyvars(e, t->as.fn.arg_full_types[i]);
            }
            fn_collect_sig_tyvars(e, t->as.fn.result_full_type);
            return;
        default:
            return;
    }
}

/* KB-026: does `t` mention the named type variable `name` anywhere? */
static bool fn_type_mentions_named(const Type *t, const char *name) {
    if (!t || !name) return false;
    switch (t->kind) {
        case TY_TYVAR:
            return t->as.tyvar_.name && strcmp(t->as.tyvar_.name, name) == 0;
        case TY_APP:
            return fn_type_mentions_named(t->as.app.fn, name) ||
                   fn_type_mentions_named(t->as.app.arg, name);
        case TY_UNION:
            for (uint8_t i = 0; i < t->as.union_.n_members; i++)
                if (fn_type_mentions_named(t->as.union_.members[i], name)) return true;
            return false;
        case TY_INTERSECTION:
            for (uint8_t i = 0; i < t->as.intersection_.n_members; i++)
                if (fn_type_mentions_named(t->as.intersection_.members[i], name)) return true;
            return false;
        case TY_FN:
            if (t->as.fn.arg_full_types)
                for (uint32_t i = 0; i < t->as.fn.arity; i++)
                    if (fn_type_mentions_named(t->as.fn.arg_full_types[i], name)) return true;
            return fn_type_mentions_named(t->as.fn.result_full_type, name);
        default:
            return false;
    }
}

/* KB-026: is `name` the declared type parameter of some in-scope ADT or
 * struct?  Such a name (e.g. `a` from `(defgadt Witness [a] ...)`) is a genuine
 * type variable even when it occurs only once in a signature, because it gets
 * refined per match arm. */
static bool fn_name_is_adt_tyvar(const Elab *e, const char *name) {
    if (!name) return false;
    for (uint32_t i = 0; i < e->n_adt_defs; i++) {
        const AdtDef *d = e->adt_defs[i];
        for (uint8_t k = 0; k < d->n_type_params; k++)
            if (d->type_params[k] && strcmp(d->type_params[k], name) == 0) return true;
    }
    /* structdef-retirement DS-C: the parallel scan over the (always-empty)
     * struct_defs registry is dead -- every parametric type is an ADT now. */
    return false;
}

static bool form_mentions_type_param(const Form *form, const Symbol *sym) {
    if (!form || !sym) return false;
    switch (form->tag) {
        case F_SYM:
        case F_KEYWORD:
            return form->as.sym == sym;
        case F_CONTRACT_TYPE:
            /* items: [var, base-type, "|", predicate].  Only the BASE TYPE is
             * a type expression.  The bound variable and the predicate are
             * value-level: their free names are ordinary values, and treating
             * them as type-variable mentions makes the implicit-type-param
             * inference swallow the very parameters the predicate constrains
             * (`[v : int, n : int]` with a `(len v)` refinement lost both
             * parameters and then failed to parse their annotations). */
            return form->as.list.len > 1 &&
                   form_mentions_type_param(form->as.list.items[1], sym);
        case F_TYPE_ANN:
        case F_LIST:
        case F_VEC:
        case F_MAP:
            for (uint32_t i = 0; i < form->as.list.len; i++) {
                if (form_mentions_type_param(form->as.list.items[i], sym)) return true;
            }
            return false;
        default:
            return false;
    }
}

static const Form *fn_return_annotation_form(const Form *call, uint32_t after_params_idx) {
    if (!call || call->tag != F_LIST || call->as.list.len <= after_params_idx) return NULL;
    uint32_t idx = after_params_idx;
    if (idx < call->as.list.len && call->as.list.items[idx]->tag == F_MAP) idx++;
    if (idx < call->as.list.len) {
        Form *ret_f = call->as.list.items[idx];
        if (ret_f->tag == F_KEYWORD || ret_f->tag == F_TYPE_ANN) return ret_f;
    }
    return NULL;
}

/* Phase RT: does `t` (or any nested type) reference the named type variable?
 * Used to classify a `where (Class a)` constraint as argument-resolved (the
 * tyvar appears in some parameter type) vs return-resolved (it does not). */
static bool type_mentions_named_tyvar(const Type *t, const char *name) {
    if (!t || !name) return false;
    switch (t->kind) {
        case TY_TYVAR:
            return t->as.tyvar_.name && strcmp(t->as.tyvar_.name, name) == 0;
        case TY_APP:
            return type_mentions_named_tyvar(t->as.app.fn, name) ||
                   type_mentions_named_tyvar(t->as.app.arg, name);
        /* A constraint tyvar can reach a parameter ONLY through a
         * function-typed parameter -- `fold-map`'s shape, where the Monoid
         * variable B appears nowhere but inside `^fat f : (fn [A] B)`.  Before
         * this case such a tyvar was classified return-resolved and its
         * `cur_fn_constraint_param_mask` bit left clear, so the
         * return-directed representative search refused a carrier-compatible
         * opaque newtype and `(mempty)` reported `no instance 'Monoid tyvar'`.
         * The signature IS a parameter type, so walk it.  Monomorphization
         * still has something to split on: `(fn [A] Sum)` and `(fn [A]
         * Product)` are distinct Types, so the arg vector interns two specs
         * and Gap H's `__h<n>` discriminator separates the identically
         * rendered `int64_t` signatures -- the same soundness argument the
         * mask already rests on for a directly-mentioned tyvar.  See
         * docs/archive/nullary-class-method-unresolvable-over-newtype-tyvar.md
         * and docs/design/multiple-monoids-across-languages.md. */
        case TY_FN:
            if (t->as.fn.arg_full_types) {
                for (uint32_t i = 0; i < t->as.fn.arity; i++) {
                    if (type_mentions_named_tyvar(t->as.fn.arg_full_types[i], name))
                        return true;
                }
            }
            return type_mentions_named_tyvar(t->as.fn.result_full_type, name);
        default:
            return false;
    }
}

static uint8_t collect_implicit_fn_type_params(const Form *params_f, const Form *ret_f,
                                               const Symbol **out_params,
                                               Kind *out_kinds) {
    if (!params_f || params_f->tag != F_VEC) return 0;
    uint8_t n = 0;
    for (uint32_t i = 0; i < params_f->as.list.len && n < 8; i++) {
        Form *p = params_f->as.list.items[i];
        if (p->tag != F_SYM) break;
        bool mentioned = false;
        for (uint32_t j = i + 1; j < params_f->as.list.len; j++) {
            if (form_mentions_type_param(params_f->as.list.items[j], p->as.sym)) {
                mentioned = true;
                break;
            }
        }
        if (!mentioned && ret_f) mentioned = form_mentions_type_param(ret_f, p->as.sym);
        if (!mentioned) break;
        out_params[n] = p->as.sym;
        out_kinds[n] = KIND_STAR;
        n++;
    }
    return n;
}

/* bare-fat-param-non-int-result inference
 * (docs/archive/history/bare-fat-result-type-inference-plan.md, Phase A):
 * A bare `^fat g` parameter (TY_PTR_VOID + is_fat, no fn-type annotation) has
 * no recorded result type, so a direct call (g x) is elaborated with an
 * int64_t result (elab_call.c, the bare-^fat dispatch path).  When such a call
 * sits in a function's result position and the declared return type is a
 * float-register-class type, emitting `return <int64 expr>;` into a
 * double-returning C function would read the wrong register (rax vs xmm0).
 * (cstr/ptr returns share the integer register and round-trip, so only the
 * float-class case is wrong.)
 *
 * Codegen already lowers the bare-^fat dispatch using the call expression's
 * `e->type` (emit_expr.c, ~1799), so it is correct for any result type -- the
 * only thing wrong is that elaboration stamped the call TYPE_INT.  This pass
 * re-stamps the call's result type from the declared (return / binding) type
 * once it is known.  Soundness rests on the tail-only walk below: only result
 * positions are visited, so a non-tail bare-^fat call (e.g. an argument to
 * another call) is never retyped. */

/* Integer-register carriers (int/cstr/ptr/rc/...) round-trip through the
 * int64 slot; only xmm/by-value-distinct returns need retyping. */
bool kind_is_non_int_register_class(TypeKind k) {
    return k == TY_FLOAT /* || k == TY_FLOAT32 || ... (extend as carriers arise) */;
}

/* F5 (fn-first-class typed carrier): a `(fn [A...] : R)` parameter is routed
 * through the tur_poly_fn_t {env, fn} carrier only when every argument and the
 * result is a single-register scalar kind -- an integer/bool/float in a GP/xmm
 * register, or a pointer (cstr / ptr<T> / ptr<void>).  These are exactly the
 * kinds whose ABI is captured by their TypeKind, so the call site can cast
 * fn.fn to the concrete R(*)(void*, A...) and the make_poly_wrapper thunk can
 * carry each argument in its native kind.  A struct/ADT/union passed by value,
 * a nested function result, a type variable, etc. would lose information through
 * the kind-only carrier (e.g. type_from_kind drops a struct's def), so those
 * keep the nominal bare-function-pointer TY_FN representation. */
static bool fn_kind_is_carrier_scalar(TypeKind k) {
    switch (k) {
        case TY_NIL:  case TY_BOOL: case TY_INT:  case TY_FLOAT:
        case TY_CSTR: case TY_PTR_VOID:
        case TY_INT8: case TY_INT16: case TY_INT32: case TY_INT64:
        case TY_UINT8: case TY_UINT16: case TY_UINT32: case TY_UINT64:
        case TY_FLOAT32: case TY_FLOAT64:
            return true;
        default:
            return false;
    }
}

/* All arg kinds + the result kind of a concrete TY_FN are carrier scalars, and
 * the signature carries no substructural / fat / borrow argument annotations
 * (those discipline bits live only on the nominal TY_FN and are enforced at the
 * higher-order call site, e.g. the LT2 linear-function-type check -- the carrier
 * has no slot for them, so a function type that declares any must keep its
 * nominal representation). */
static bool fn_type_is_carrier_safe(const Type *ft) {
    if (!ft || ft->kind != TY_FN) return false;
    if (ft->as.fn.result_fat) return false;
    if (!fn_kind_is_carrier_scalar(ft->as.fn.result_kind)) return false;
    for (uint32_t i = 0; i < ft->as.fn.arity; i++) {
        if (!fn_kind_is_carrier_scalar(ft->as.fn.arg_kinds[i])) return false;
        if ((ft->as.fn.arg_flags[i] &
             (FA_LINEAR | FA_UNIQUE | FA_UNIQUE_MUT | FA_AFFINE |
              FA_RELEVANT | FA_BORROW | FA_FAT)) != 0)
            return false;
    }
    return true;
}

/* Increment 4 successor (repr-coverage-census): the fn-PARAM routing
 * decision, as one named routine.
 *
 * This is the answer the coverage census found missing: `repr_of(type, pos)`
 * has no carrier answer for a fn param, because the routing depends on the
 * BINDING's substructural flags as well as the annotation's shape -- exactly
 * the (Binding x Type) domain `repr_of_binding` was introduced for.  It lives
 * here rather than beside `repr_of` in types.c because its two gates
 * (`fn_type_has_named_tyvar`, `fn_type_is_carrier_safe`) are elaboration
 * predicates defined in this file; declaring it in types.h keeps the repr_*
 * family discoverable from one header.
 *
 * Returns the representation the parameter is routed onto:
 *   CARRIER_I64  the typed tur_poly_fn_t {env, fn} carrier
 *   FAT_HANDLE   an explicit `^fat` param
 *   THIN_FN      a nominal TY_FN -- a cfnptr, or a signature the carrier
 *                cannot round-trip (effect row, tyvar, variadic, wide arity,
 *                non-scalar), or a substructural binding with its own
 *                discipline
 *
 * Order matters and mirrors the routing it replaces: carrier wins, then
 * `^fat`, then everything nominal. */
static ReprForm repr_of_fn_param_impl(const Binding *b, const Type *ann);

ReprForm repr_of_fn_param(const Binding *b, const Type *ann) {
    ReprForm f = repr_of_fn_param_impl(b, ann);
    /* Counted by the coverage census like every other repr_* answer.  Without
     * this the fn axis would be consolidated but still invisible in the
     * matrix -- the census would keep reporting the hole it just closed,
     * which is how a metric quietly stops tracking the thing it names. */
    static int census = -1;
    if (census < 0) census = getenv("TUR_REPR_CENSUS") ? 1 : 0;
    if (census)
        fprintf(stderr, "repr-census fn-param %s\n", repr_form_name(f));
    return f;
}

static ReprForm repr_of_fn_param_impl(const Binding *b, const Type *ann) {
    if (!b || !ann || ann->kind != TY_FN) return REPR_THIN_FN;
    /* A substructural or ^fat binding keeps its nominal type -- those have
     * their own calling conventions and discipline checks. */
    bool plain = !b->is_fat && !b->is_borrow && !b->is_unique && !b->is_mut &&
                 !b->is_linear && !b->is_affine && !b->is_relevant;
    bool effectful = ann->as.fn.effect_row != NULL;
    bool carrier = plain && !effectful && !ann->as.fn.cfnptr &&
                   !ann->as.fn.is_variadic &&
                   ann->as.fn.arity <= (MAX_FN_ARITY - 1) &&
                   !fn_type_has_named_tyvar(ann) &&
                   fn_type_is_carrier_safe(ann);
    if (carrier) return REPR_CARRIER_I64;
    if (b->is_fat) return REPR_FAT_HANDLE;
    return REPR_THIN_FN;
}

/* Re-stamp bare-^fat int64 calls in `tail`'s result position(s) to `target`
 * (a concrete non-int register-class kind).  Returns true if any call was
 * retyped.  Tail-precise: only result positions are visited, so a non-tail
 * bare-^fat call is never touched. */
bool retype_bare_fat_tail_calls(Expr *tail, TypeKind target) {
    if (!tail) return false;
    switch (tail->kind) {
        case EX_DO: {
            if (tail->as.do_.n == 0) return false;
            Expr *last = tail->as.do_.items[tail->as.do_.n - 1];
            bool changed = retype_bare_fat_tail_calls(last, target);
            if (changed) tail->type = last->type;   /* keep wrapper consistent */
            return changed;
        }
        case EX_LET:
        case EX_LETREC: {
            bool changed = retype_bare_fat_tail_calls(tail->as.let_.body, target);
            if (changed) tail->type = tail->as.let_.body->type;
            return changed;
        }
        case EX_IF: {
            bool c1 = retype_bare_fat_tail_calls(tail->as.if_.then_, target);
            bool c2 = retype_bare_fat_tail_calls(tail->as.if_.else_or_null, target);
            /* The if's type follows its (now-consistent) then-branch. */
            if ((c1 || c2) && tail->as.if_.then_) tail->type = tail->as.if_.then_->type;
            return c1 || c2;
        }
        case EX_CALL: {
            Binding *b = tail->as.call_.fn_binding;
            if (b && b->is_fat && b->type.kind == TY_PTR_VOID &&
                tail->type.kind == TY_INT) {
                tail->type = type_from_kind(target);
                return true;
            }
            return false;
        }
        default:
            return false;
    }
}

/* bare-fat-sink-poly-box-slot0-int64-mismatch.md: recover the argument kinds
 * of a bare `^fat` param `g`'s tail-position invoke `(g x0 x1 ...)`, mirroring
 * the tail walk above.  Returns the arg count (>= 0) and fills `out`, or -1
 * when no tail-position call to `g` is found.  Used to synthesize `g`'s fn
 * signature on the enclosing function's type so a caller that boxes a
 * tur_poly_fn_t into this ^fat slot (EX_POLY_TO_FAT) can pick a slot-0 shim
 * whose ABI matches the typed-thunk cast the retyped invoke applies. */
static int bare_fat_tail_call_arg_kinds(Arena *arena, const Expr *tail,
                                        const Binding *g, TypeKind **out) {
    if (!tail) return -1;
    switch (tail->kind) {
        case EX_DO:
            return tail->as.do_.n
                ? bare_fat_tail_call_arg_kinds(arena, tail->as.do_.items[tail->as.do_.n - 1], g, out)
                : -1;
        case EX_LET:
        case EX_LETREC:
            return bare_fat_tail_call_arg_kinds(arena, tail->as.let_.body, g, out);
        case EX_IF: {
            int a = bare_fat_tail_call_arg_kinds(arena, tail->as.if_.then_, g, out);
            if (a >= 0) return a;
            return bare_fat_tail_call_arg_kinds(arena, tail->as.if_.else_or_null, g, out);
        }
        case EX_CALL:
            if (tail->as.call_.fn_binding == g) {
                /* Arena-allocate the arg-kind vector sized to the actual call --
                 * no fixed ceiling on the tail-called function's arity. */
                uint32_t n = tail->as.call_.n_args;
                TypeKind *buf = n ? (TypeKind *)arena_alloc(arena, n * sizeof(TypeKind)) : NULL;
                for (uint32_t i = 0; i < n; i++)
                    buf[i] = tail->as.call_.args[i]->type.kind;
                *out = buf;
                return (int)n;
            }
            return -1;
        default:
            return -1;
    }
}

/* TY4: reject a function whose result is a borrow of one of its own locals
 * (params or let-locals).  Such a borrow dangles once the frame is gone --
 * today it only surfaces as a C -Wdangling-pointer warning.  Walk the body's
 * result position through do/let/if tails; fn_local_depth is the depth of the
 * function's parameter scope, so any referent at that depth or deeper is a
 * function-local.  Emits TUR-E0105 and returns false on the first violation. */
static bool check_no_borrow_escape(const Expr *tail, uint32_t fn_local_depth,
                                   const Symbol *fn_name) {
    if (!tail) return true;
    switch (tail->kind) {
        case EX_DO:
            return tail->as.do_.n == 0 ? true
                : check_no_borrow_escape(tail->as.do_.items[tail->as.do_.n - 1],
                                         fn_local_depth, fn_name);
        case EX_LET:
        case EX_LETREC:
            return check_no_borrow_escape(tail->as.let_.body, fn_local_depth, fn_name);
        case EX_IF:
            return check_no_borrow_escape(tail->as.if_.then_, fn_local_depth, fn_name)
                && check_no_borrow_escape(tail->as.if_.else_or_null, fn_local_depth, fn_name);
        case EX_BORROW_IMMUT:
        case EX_BORROW_MUT: {
            const Binding *ref = borrow_referent_binding(tail);
            if (ref && !ref->is_global && ref->scope_depth >= fn_local_depth) {
                diag_emit_with_code(DIAG_ERROR, tail->span,
                    TUR_E0105_BORROW_ESCAPES_SCOPE,
                    "function '%s' returns a borrow of local `%s`, "
                    "which does not outlive the function",
                    fn_name ? fn_name->name : "?", ref->name->name);
                return false;
            }
            return true;
        }
        case EX_CALL: {
            /* LS4: inter-procedural borrow escape.  A call that returns a
             * lifetime-tied borrow (&'a T) yields a borrow of whatever the
             * tied argument borrows.  Follow the escape check into that
             * argument: if it ultimately borrows a caller-local, the returned
             * borrow dangles just as a direct (& local) would.  The callee's
             * result_borrow_arg names the parameter the return is tied to. */
            const Binding *cb = tail->as.call_.fn_binding;
            if (cb && cb->type.kind == TY_FN) {
                int8_t bi = cb->type.as.fn.result_borrow_arg;
                if (bi >= 0 && (uint32_t)bi < tail->as.call_.n_args
                        && tail->as.call_.args) {
                    return check_no_borrow_escape(tail->as.call_.args[bi],
                                                  fn_local_depth, fn_name);
                }
            }
            return true;
        }
        default:
            return true;
    }
}

/* captureless-algebra-arm-thin-through-carrier: classify a function's
 * tail/return leaves by closure representation.  Returns a bitmask: bit 0 set
 * iff some tail leaf is a *thin* bare fn pointer (an EX_VAR of non-boxed TY_FN
 * -- a captureless lifted lambda or a bare defn reference), bit 1 set iff some
 * tail leaf is a *fat* closure box (EX_CLOSURE / EX_FN_TO_FAT, or any boxed
 * TY_FN value).  A function returning the closure carrier whose leaves are
 * *mixed* (both bits set) returns a non-uniform value: the carrier-crossing
 * caller fat-dispatches every leaf, so a thin-pointer arm is read as a fat box
 * (slot-0 of a code address) and jumped into -> SIGSEGV.  Walks through
 * tail-position structure only (do/if/let/match/ascribe). */
/* fn-value-carrier-fat-seam-residuals (cell 1): the tail walkers classify
 * leaves by BINDING, so a let-ALIAS of a param loses its provenance -- e.g.
 * `(let [w v] w)` where `v` is a carrier (tur_poly_fn_t) fn param: `w` is a
 * local non-param binding of non-boxed TY_FN (or ptr<void>), so the stage-2
 * normalizer either thin-shims the by-value aggregate (invalid C) or skips
 * it entirely (the merge temp assign is invalid C instead).  The walkers
 * therefore carry a stack env of the let bindings they descend PAST, and a
 * leaf var resolves transitively through recorded inits (peeking through
 * ascriptions) back to the classifiable origin.  Only tail-position lets are
 * recorded -- exactly the ones whose aliases can BE a tail leaf. */
typedef struct FnTailAlias {
    const struct FnTailAlias *up;
    const Binding *binding;
    const Expr *init;
} FnTailAlias;

static const Expr *fn_tail_resolve_alias(const Expr *x, const FnTailAlias *env) {
    int guard = 32;  /* alias chains are tiny; guard against binding cycles */
    while (x && guard-- > 0) {
        while (x->kind == EX_ASCRIBE) x = x->as.ascribe_.inner;
        if (x->kind != EX_VAR || !x->as.var.binding) return x;
        const Binding *vb = x->as.var.binding;
        if (vb->is_param) return x;
        const Expr *init = NULL;
        for (const FnTailAlias *a = env; a; a = a->up)
            if (a->binding == vb) { init = a->init; break; }
        if (!init) return x;
        x = init;
    }
    return x;
}

/* Push one let form's bindings onto the alias env.  The storage array lives
 * in the caller's block scope; the recursive walk into the let body happens
 * inside that scope, so the nodes stay alive exactly as long as needed.
 * Lets wider than the fixed cap simply skip recording (the resolver then
 * conservatively leaves aliases unresolved -- current behavior). */
#define FN_TAIL_PUSH_LET_ALIASES(x, env, storage)                            \
    const FnTailAlias *env##_new = (env);                                    \
    do {                                                                     \
        uint32_t _n = (x)->as.let_.n;                                        \
        if (_n > 16) _n = 0;                                                 \
        for (uint32_t _i = 0; _i < _n; _i++) {                               \
            storage[_i].binding = (x)->as.let_.bindings[_i].binding;         \
            storage[_i].init = (x)->as.let_.bindings[_i].init;               \
            storage[_i].up = (_i == 0) ? (env) : &storage[_i - 1];           \
        }                                                                    \
        if (_n > 0) env##_new = &storage[_n - 1];                            \
    } while (0)

static unsigned fn_tail_fn_leaf_kinds(const Expr *x, const FnTailAlias *env) {
    if (!x) return 0;
    switch (x->kind) {
        case EX_DO:
            return x->as.do_.n > 0
                       ? fn_tail_fn_leaf_kinds(x->as.do_.items[x->as.do_.n - 1],
                                               env)
                       : 0;
        case EX_IF:
            return fn_tail_fn_leaf_kinds(x->as.if_.then_, env) |
                   (x->as.if_.else_or_null
                        ? fn_tail_fn_leaf_kinds(x->as.if_.else_or_null, env)
                        : 0);
        case EX_LET:
        case EX_LETREC: {
            FnTailAlias _st[16];
            FN_TAIL_PUSH_LET_ALIASES(x, env, _st);
            return fn_tail_fn_leaf_kinds(x->as.let_.body, env_new);
        }
        case EX_REINTERPRET:
            return fn_tail_fn_leaf_kinds(x->as.reinterpret_.expr, env);
        case EX_ASCRIBE:
            return fn_tail_fn_leaf_kinds(x->as.ascribe_.inner, env);
        case EX_MATCH: {
            unsigned acc = 0;
            for (uint32_t i = 0; i < x->as.match_.n_arms; i++)
                acc |= fn_tail_fn_leaf_kinds(x->as.match_.arms[i].body, env);
            return acc;
        }
        case EX_CLOSURE:
        case EX_FN_TO_FAT:
            return 2u;  /* fat box */
        case EX_VAR: {
            /* fn-value-fat-normalization: binding-aware classification.  A
             * carrier (is_poly_fn) fn param is a by-value tur_poly_fn_t --
             * neither thin nor fat (bit 4).  A ^fat param or a stage-1
             * NORMALIZED nominal param already holds a fat handle even though
             * its static type is a non-boxed TY_FN -- classifying those as
             * thin would double-box them.  A local ALIAS resolves to its
             * origin first (cell 1). */
            const Expr *r = fn_tail_resolve_alias(x, env);
            if (r != x && r->kind != EX_VAR)
                return fn_tail_fn_leaf_kinds(r, env);
            if (r->kind == EX_VAR && r->as.var.binding) {
                const Binding *vb = r->as.var.binding;
                if (vb->is_poly_fn && vb->is_param) return 4u;
                if (vb->is_fat ||
                    (vb->is_param && vb->type.kind == TY_FN &&
                     fn_param_type_is_fat_normalized(&vb->type)))
                    return 2u;
            }
            if (x->type.kind == TY_FN)
                return x->type.as.fn.boxed ? 2u : 1u;  /* boxed=fat, bare=thin */
            return 0;
        }
        default:
            if (x->type.kind == TY_FN && x->type.as.fn.boxed)
                return 2u;
            return 0;
    }
}

/* captureless-algebra-arm-thin-through-carrier: rewrite every *thin* bare-fn
 * tail leaf (an EX_VAR of non-boxed TY_FN) into a fat closure box via
 * EX_FN_TO_FAT, in place at *slot.  Applied only when the body is known to be
 * mixed (see fn_tail_fn_leaf_kinds), so the function's closure-carrier result
 * is uniformly fat across all return leaves and a fat-dispatch consumer reads a
 * valid { thunk, ... } layout on every arm.  Symmetric with the ^fat-result
 * auto-shim and the parametric-ADT-field shim in elab_call.c. */
static void elab_box_thin_fn_tail_leaves(Elab *e, Expr **slot,
                                         const FnTailAlias *env) {
    Expr *x = *slot;
    if (!x) return;
    switch (x->kind) {
        case EX_DO:
            if (x->as.do_.n > 0)
                elab_box_thin_fn_tail_leaves(e, &x->as.do_.items[x->as.do_.n - 1],
                                             env);
            return;
        case EX_IF:
            elab_box_thin_fn_tail_leaves(e, &x->as.if_.then_, env);
            if (x->as.if_.else_or_null)
                elab_box_thin_fn_tail_leaves(e, &x->as.if_.else_or_null, env);
            return;
        case EX_LET:
        case EX_LETREC: {
            FnTailAlias _st[16];
            FN_TAIL_PUSH_LET_ALIASES(x, env, _st);
            elab_box_thin_fn_tail_leaves(e, &x->as.let_.body, env_new);
            return;
        }
        case EX_ASCRIBE:
            elab_box_thin_fn_tail_leaves(e, &x->as.ascribe_.inner, env);
            return;
        case EX_MATCH:
            for (uint32_t i = 0; i < x->as.match_.n_arms; i++)
                elab_box_thin_fn_tail_leaves(e, &x->as.match_.arms[i].body, env);
            return;
        default:
            break;
    }
    if (x->kind == EX_VAR && x->type.kind == TY_FN && !x->type.as.fn.boxed &&
        x->type.as.fn.arity >= 1 && x->type.as.fn.arity <= 5) {
        /* stage-1 normalized params and ^fat params already hold fat handles
         * despite their non-boxed static type -- shimming them here would
         * double-box (mirrors the classifier's binding-aware cases).  A local
         * ALIAS resolves to its origin first (seam-residuals cell 1): an
         * alias of a fat/normalized/carrier value must not be thin-shimmed. */
        const Expr *r = fn_tail_resolve_alias(x, env);
        const Binding *rb = (r && r->kind == EX_VAR) ? r->as.var.binding : NULL;
        if (rb &&
            (rb->is_fat || (rb->is_poly_fn && rb->is_param) ||
             (rb->is_param &&
              fn_param_type_is_fat_normalized(&rb->type))))
            return;
        if (r && r != x && r->kind != EX_VAR &&
            (r->kind == EX_CLOSURE || r->kind == EX_FN_TO_FAT ||
             (r->type.kind == TY_FN && r->type.as.fn.boxed)))
            return;  /* alias of a fat producer -- already fat */
        Type bt = x->type;
        bt.as.fn.boxed = true;
        Expr *shim = expr_new(e->arena, EX_FN_TO_FAT, bt, x->span);
        shim->as.fn_to_fat_.inner = x;
        *slot = shim;
    }
}

/* fn-value-fat-normalization stage 2: normalize EVERY tail leaf of a defn
 * whose declared result is a concrete effect-free fn type
 * (fn_param_type_is_fat_normalized) onto the fat handle: a thin bare-fn leaf
 * is shimmed (EX_FN_TO_FAT), a carrier (tur_poly_fn_t) param leaf is boxed
 * (EX_POLY_TO_FAT -- previously `return (int64_t)(intptr_t)v;` on the
 * aggregate, a hard cc error), and already-fat leaves pass through.  The
 * caller then marks the declared result `boxed`, which steers the signature
 * and every consumer onto the existing fat-result plumbing. */
static void elab_normalize_fn_tail_leaves(Elab *e, Expr **slot,
                                          Type *sink_fn_type,
                                          const FnTailAlias *env) {
    Expr *x = *slot;
    if (!x) return;
    switch (x->kind) {
        case EX_DO:
            if (x->as.do_.n > 0)
                elab_normalize_fn_tail_leaves(e,
                    &x->as.do_.items[x->as.do_.n - 1], sink_fn_type, env);
            return;
        case EX_IF:
            elab_normalize_fn_tail_leaves(e, &x->as.if_.then_, sink_fn_type,
                                          env);
            if (x->as.if_.else_or_null)
                elab_normalize_fn_tail_leaves(e, &x->as.if_.else_or_null,
                                              sink_fn_type, env);
            return;
        case EX_LET:
        case EX_LETREC: {
            FnTailAlias _st[16];
            FN_TAIL_PUSH_LET_ALIASES(x, env, _st);
            elab_normalize_fn_tail_leaves(e, &x->as.let_.body, sink_fn_type,
                                          env_new);
            return;
        }
        case EX_MATCH:
            for (uint32_t i = 0; i < x->as.match_.n_arms; i++)
                elab_normalize_fn_tail_leaves(e, &x->as.match_.arms[i].body,
                                              sink_fn_type, env);
            return;
        default:
            break;
    }
    if (x->kind != EX_VAR || !x->as.var.binding) return;
    /* seam-residuals cell 1: resolve a local alias to its origin so the
     * classification below sees the value's REAL representation.  The
     * conversion (if any) is applied to the ORIGINAL leaf `x` -- its emitted
     * value carries the same representation as the origin (a let alias of a
     * tur_poly_fn_t param is a by-value tur_poly_fn_t copy). */
    const Expr *r = fn_tail_resolve_alias(x, env);
    Binding *vb = (r->kind == EX_VAR && r->as.var.binding)
                      ? r->as.var.binding
                      : x->as.var.binding;
    /* Increment 4 stage 3: the fn-value TAIL/JOIN shadow.  This walker is the
     * classification the fn-value axis turns on -- each tail leaf is sorted
     * into carrier / already-fat / thin-needing-a-shim, and the emitted
     * conversion follows.  Shadow it against `repr_of_binding`, the
     * binding-context decision function, so the two cannot drift.
     *
     * Only leaves whose BINDING is authoritative are shadowed: a param
     * carries its representation in its flags, but a let-bound alias carries
     * it in its INITIALISER (an alias of a closure is fat while its binding
     * type reads thin), which no binding-only signature can see.  Those are
     * left to the alias resolution below -- narrowing what the instrument
     * claims rather than emitting disagreements it cannot ground. */
    /* MIGRATED (increment 4 successor): for a PARAM leaf the decision function
     * IS the answer, so the hand-derived "already fat" test is deleted rather
     * than duplicated.  Its shadow ran silent on every evaluation before this
     * (8 corpus-wide), which is what licensed the swap; now the two cannot
     * disagree because there is only one of them, and the shadow is retired by
     * construction -- the same end state the container-element collapse
     * reached.
     *
     * Grounding guard: a NON-param binding keeps the old test.  Its
     * representation lives in its INITIALISER (an alias of a closure is fat
     * while its binding type reads thin), which no binding-only signature can
     * see, so there is nothing here for the decision function to own yet. */
    bool leaf_already_fat =
        vb->is_param ? repr_of_binding(vb, REPR_POS_RESULT) == REPR_FAT_HANDLE
                     : vb->is_fat;
    if (vb->is_poly_fn && vb->is_param) {
        Expr *conv = expr_new(e->arena, EX_POLY_TO_FAT, TYPE_PTR_VOID, x->span);
        conv->as.poly_to_fat_.inner = x;
        conv->as.poly_to_fat_.sink_fn_type =
            (sink_fn_type && sink_fn_type->kind == TY_FN) ? sink_fn_type : NULL;
        *slot = conv;
        return;
    }
    if (leaf_already_fat)
        return;  /* already fat */
    if (r != x && r->kind != EX_VAR &&
        (r->kind == EX_CLOSURE || r->kind == EX_FN_TO_FAT ||
         (r->type.kind == TY_FN && r->type.as.fn.boxed)))
        return;  /* alias of a fat producer -- already fat */
    if (x->type.kind == TY_FN && !x->type.as.fn.boxed &&
        x->type.as.fn.arity <= 5) {
        Type bt = x->type;
        bt.as.fn.boxed = true;
        Expr *shim = expr_new(e->arena, EX_FN_TO_FAT, bt, x->span);
        shim->as.fn_to_fat_.inner = x;
        *slot = shim;
    }
}

void elab_infer_nonretain_masks(Binding *b, Binding **params, uint32_t n_params,
                                Expr *body) {
    b->nonretain_param_mask = 0;
    /* An inline-C body can STORE a fn-param invisibly to the AST escape analysis
     * (a param is a C-visible formal, not an AST capture), so a body containing
     * any inline-C is never treated as non-retaining -- otherwise its stored
     * closure arg would be freed while the C-side copy is still live (UAF). */
    /* catch-box-reader-confinement-whitelist: the same inference, for the
     * pointer-carrying scalars (cstr / ptr<void>) that a caught-Result box
     * hands out.  Trusting a hardcoded print-family name list made the
     * confinement check a soundness-maintenance footgun AND needlessly leaked
     * for a user-defined logger that is every bit as safe; inferring it from
     * the body makes it a checked property.  The inline-C guard above is
     * load-bearing here too -- a C body can stash the pointer where no AST
     * walk can see it.
     *
     * The result gate mirrors catch_box_binding_reader_confined: the param may
     * only be treated as non-retained if the function's own result cannot carry
     * it back out. */
    b->nonretain_ptr_param_mask = 0;
    b->nonretain_sum_param_mask = 0;
    b->resalias_param_mask = 0;
    if (body && !expr_subtree_has_inline_c(body)) {
      /* residual-leaks (2026-09-02): a GREATEST fixed point for the fn-param
       * mask.  `list-eq?` hands `cmp-fn` to its own recursive call; with the
       * mask still zero during inference that self-call read as an escape and
       * the mask never set, so every comparator into a recursive stdlib walker
       * leaked its shim box.  Start with every fn-typed param assumed
       * non-retaining, clear the bits the walk proves escape, and repeat
       * until nothing changes: bits only ever clear, so this terminates, and
       * a bit that survives is one no use of the param can contradict --
       * including a pass-through into a slot that also survived. */
      /* cps-capturing-closure-env-leaks-through-dyn-call: an `any` parameter
       * joins the fn-param inference.  In a dynamic file a procedure argument
       * IS an `any`, and the body calls it through a dynamic call -- which the
       * walk now reads as an invocation, not an escape -- so `(defn apply-to
       * [g x] (g x))` does not retain `g`, and a capturing lambda handed to it
       * can be released by the caller the way a typed one handed to a
       * `(fn ...)`-typed parameter is.  The bit means the same thing for any
       * payload: the body does nothing with the value but invoke it, read its
       * tag, or pass it on to a slot that does the same. */
      for (uint32_t _pi = 0; _pi < n_params && _pi < 32; _pi++) {
          Binding *_pb = params[_pi];
          if (_pb && (_pb->is_fat || _pb->is_poly_fn || _pb->type.kind == TY_FN ||
                      _pb->type.kind == TY_ANY))
              b->nonretain_param_mask |= (1u << _pi);
      }
      /* byvalue-recursive-adt-boxes-are-never-freed (Residue 1): a by-value
       * RECURSIVE ADT parameter joins the inference, in the walk's alias-aware
       * strict mode, and only when the function's result is a non-pointer
       * scalar (an aggregate result could carry a sub-spine back out, and the
       * strict walk does not model which).  The bit means what it means for
       * the others -- "this body neither keeps a pointer this parameter
       * carries nor hands one back" -- and a caller passing a LOCAL to such a
       * parameter keeps ownership of the spine (binding_mark_lent) and frees
       * it at scope exit, where before the move handed it to a callee that
       * never discharged it.
       *
       * A GREATEST fixed point, like the fn-param mask: `llen` passes the
       * match binder `t` to its own recursive call, and with the bit clear
       * during its own walk that hand-off reads as an escape.  Start with
       * every eligible bit set on the binding the self-call reads, clear the
       * ones the walk contradicts, and repeat until stable -- bits only ever
       * clear, so this terminates.  The bits ride nonretain_ptr_param_mask
       * because that is the mask the walk's hand-off check consults, and a
       * TY_ADT parameter never enters the pointer-scalar rule below. */
      uint32_t _adt_bits = 0;
      {
          TypeKind _ark = (b->type.kind == TY_FN) ? b->type.as.fn.result_kind
                                                  : TY_UNKNOWN;
          bool _ares_scalar;
          switch (_ark) {
              case TY_NIL: case TY_BOOL: case TY_INT: case TY_FLOAT:
              case TY_INT8: case TY_INT16: case TY_INT32: case TY_INT64:
              case TY_UINT8: case TY_UINT16: case TY_UINT32: case TY_UINT64:
              case TY_FLOAT32: case TY_FLOAT64:
                  _ares_scalar = true; break;
              default:
                  _ares_scalar = false; break;
          }
          if (_ares_scalar)
              for (uint32_t _pi = 0; _pi < n_params && _pi < 32; _pi++) {
                  Binding *_pb = params[_pi];
                  if (_pb && !_pb->is_borrow &&
                      elab_byval_localowned_adt(_pb->type))
                      _adt_bits |= (1u << _pi);
              }
      }
      /* byvalue-recursive-shared-copies-leak: a `^borrow` recursive-ADT
       * parameter kept nowhere but the result (resalias_param_mask).  The same
       * strict walk with the result position CONFINED -- a bare `x` there is
       * the alias the caller will track -- and the same greatest fixed point,
       * so a walker handing its borrow to its own recursive call keeps the
       * bit. */
      uint32_t _ra_bits = 0;
      for (uint32_t _pi = 0; _pi < n_params && _pi < 32; _pi++) {
          Binding *_pb = params[_pi];
          if (_pb && _pb->is_borrow && elab_byval_localowned_adt(_pb->type))
              _ra_bits |= (1u << _pi);
      }
      uint32_t _prev_fn_mask, _prev_adt_bits, _prev_ra_bits;
      do {
        _prev_fn_mask = b->nonretain_param_mask;
        _prev_adt_bits = _adt_bits;
        _prev_ra_bits = _ra_bits;
        b->nonretain_ptr_param_mask = _adt_bits;
        b->nonretain_sum_param_mask = 0;
        b->resalias_param_mask = _ra_bits;
        for (uint32_t _pi = 0; _pi < n_params && _pi < 32; _pi++) {
            Binding *_pb = params[_pi];
            if (!_pb) continue;
            if ((_adt_bits & (1u << _pi)) &&
                !localowned_param_is_nonretaining(body, _pb)) {
                _adt_bits &= ~(1u << _pi);
                b->nonretain_ptr_param_mask &= ~(1u << _pi);
            }
            if ((_ra_bits & (1u << _pi)) &&
                !localowned_binding_is_confined(body, _pb, /*result_cannot_carry=*/true)) {
                _ra_bits &= ~(1u << _pi);
                b->resalias_param_mask &= ~(1u << _pi);
            }
            /* value-struct-payload-sum-monomorph-box-has-no-owner: a stdlib
             * Option/Result-typed parameter joins the inference.  Same result
             * gate as the pointer-scalar case (a non-pointer scalar result
             * cannot carry the param or its arm pointer back out), same
             * inline-C exclusion (enforced by the enclosing `if`). */
            {
                const AdtDef *_sd = NULL;
                if (_pb->type.kind == TY_APP) _sd = type_adt_app_def(&_pb->type);
                else if (_pb->type.kind == TY_ADT) _sd = _pb->type.as.adt_.def;
                bool _is_sum = _sd && _sd->name &&
                    (strcmp(_sd->name, "Option") == 0 || strcmp(_sd->name, "Result") == 0);
                /* A TYPE-VARIABLE parameter joins too (`w : a` in a constrained
                 * generic): the caller may hand it a fresh `(Option Box)`, and
                 * the stamp at that call site keys on the ARGUMENT being a
                 * fresh sum, so a bit here only ever matters for one.  The
                 * walk asks the same question of `w` it asks of a declared
                 * sum param -- every use confined, result unable to carry it. */
                if (_pb->type.kind == TY_TYVAR) _is_sum = true;
                if (_is_sum) {
                    TypeKind _srk = (b->type.kind == TY_FN) ? b->type.as.fn.result_kind
                                                            : TY_UNKNOWN;
                    /* Two result-kind facts feed the walk.  `_sres_refuse`:
                     * a result that can carry a pointer the walk does not
                     * model (borrow / raw pointer / any / fn / unknown) --
                     * refused outright, as before.  `_sres_scalar`: a
                     * non-pointer scalar result cannot carry the box or its
                     * arm pointer at all (a payload word or cstr field copied
                     * out does not point INTO the box's malloc), so the body's
                     * result position starts CONFINED and a bare `p` there is
                     * a read.  Everything else -- an aggregate such as the
                     * parameter's own `(Option S)` -- walks with the result
                     * position UNCONFINED: `(defn f [n : int v : (Option S)] :
                     * (Option S) (if (= n 0) v (f (- n 1) v)))` returns `v`
                     * as-is, arm box included, and the first version of this
                     * gate (a denylist that started every non-refused kind
                     * confined) stamped it non-retaining, so the caller freed
                     * the arm its own result still pointed at -- the type
                     * fuzzer printed the freed box's stale word.  `result-map`
                     * keeps its bit: its result is rebuilt from `ok-val` /
                     * `err-val` reads, which the walk admits unconfined. */
                    bool _sres_refuse;
                    switch (_srk) {
                        case TY_UNKNOWN: case TY_PTR_VOID: case TY_FN:
                        case TY_REF: case TY_RC: case TY_WEAK:
                        case TY_REF_IMMUT: case TY_REF_MUT:
                        case TY_ANY: case TY_NEVER:
                        case TY_CONT: case TY_CLONEABLE_CONT: case TY_EXCEPTION:
                            _sres_refuse = true; break;
                        default:
                            _sres_refuse = false; break;
                    }
                    bool _sres_scalar;
                    switch (_srk) {
                        case TY_NIL: case TY_BOOL: case TY_INT: case TY_FLOAT:
                        case TY_CSTR: case TY_SYM:
                        case TY_INT8: case TY_INT16: case TY_INT32: case TY_INT64:
                        case TY_UINT8: case TY_UINT16: case TY_UINT32: case TY_UINT64:
                        case TY_FLOAT32: case TY_FLOAT64:
                            _sres_scalar = true; break;
                        default:
                            _sres_scalar = false; break;
                    }
                    if (!_sres_refuse &&
                        sum_param_is_nonretaining(body, _pb, _sres_scalar))
                        b->nonretain_sum_param_mask |= (1u << _pi);
                }
            }
            bool _is_fnparam = _pb->is_fat || _pb->is_poly_fn ||
                               _pb->type.kind == TY_FN || _pb->type.kind == TY_ANY;
            if (_is_fnparam && closure_binding_escapes(body, _pb))
                b->nonretain_param_mask &= ~(1u << _pi);
            /* any-struct-box-leak-per-widen: an `any` parameter joins the same
             * inference, and means the same thing -- "this body does not retain
             * a pointer this parameter carries".  A tur_tagged_t whose payload
             * is a heap-boxed by-value struct carries exactly such a pointer, so
             * the caller may keep that payload in its own frame rather than
             * mallocing a box nothing frees.  Reusing this mask rather than
             * adding a parallel one keeps a single answer to a single question.
             *
             * union-tagged-union-c-emission: a TY_UNION parameter is the same
             * tur_tagged_t carrying the same kind of payload pointer, and the
             * widen that builds it heap-boxes a by-value member exactly as the
             * `any` widen does -- so it answers the same question and joins the
             * same mask.  It was left out when this rule was written, which is
             * why the union path still leaked one box per widen after the `any`
             * path stopped: not because a union box is harder to own, but
             * because the inference never ran for it. */
            bool _is_ptr_scalar = _pb->type.kind == TY_CSTR ||
                                  _pb->type.kind == TY_PTR_VOID ||
                                  _pb->type.kind == TY_ANY ||
                                  _pb->type.kind == TY_UNION;
            if (_is_ptr_scalar) {
                TypeKind _rk = (b->type.kind == TY_FN) ? b->type.as.fn.result_kind
                                                       : TY_UNKNOWN;
                bool _result_safe = false;
                switch (_rk) {
                    case TY_NIL: case TY_INT: case TY_BOOL: case TY_FLOAT:
                    case TY_INT64: case TY_UINT64: case TY_INT32: case TY_UINT32:
                    case TY_INT16: case TY_UINT16: case TY_INT8: case TY_UINT8:
                    case TY_FLOAT64: case TY_FLOAT32:
                        _result_safe = true; break;
                    default: break;
                }
                /* saffron-any-return-defeats-the-frame-box-rule: an `any`
                 * RESULT runs the walk too, unconfined.
                 *
                 * The whitelist above is a cheap conservative proxy for the
                 * real question -- "can the result carry a pointer into this
                 * parameter out?" -- and it answers no only for kinds that
                 * cannot carry a pointer at all.  `any` can, so it is excluded,
                 * and that is correct as far as it goes.  What it costs is
                 * everything: a Saffron function's unannotated return IS `any`
                 * (D3), so a rule written for the rare case stopped firing in
                 * the language where the widen is the common case, and every
                 * call widening a by-value payload malloc'd a box nothing
                 * freed.
                 *
                 * The walk itself answers the real question, and answers it
                 * better: run it with `result_cannot_carry = false` and a bare
                 * `p` in result position fails (EX_VAR checks `confined`), a
                 * general call taking `p` fails (its result may alias), while a
                 * body whose result is a FRESH box -- which is what every
                 * dynamic operator and field read produces -- passes.  So
                 * `(defn f [x] (+ x 1))` and `(defn get-x [p] (.x p))` qualify
                 * and `(defn dyn [x] x)` does not, which is exactly the
                 * distinction the result kind could not draw.
                 *
                 * Same posture as the rest of this family: only ever sets a
                 * bit, and the bit only ever moves an allocation into the
                 * caller's frame.  Every unmodelled form still falls through to
                 * the strict escape walk, whose default is "escapes". */
                bool _run_walk = _result_safe || _rk == TY_ANY;
                if (_run_walk && ptr_param_is_nonretaining(body, _pb, _result_safe))
                    b->nonretain_ptr_param_mask |= (1u << _pi);
            }
        }
      } while (b->nonretain_param_mask != _prev_fn_mask ||
               _adt_bits != _prev_adt_bits || _ra_bits != _prev_ra_bits);
    }
}

/* saffron-lang-plan S2/D3: the declared type of an UNANNOTATED positional
 * parameter.
 *
 * Turmeric defaults it to `int` -- a static language has to pick something, and
 * `int` is the carrier.  Saffron defaults it to `any`, which is the whole of
 * what "dynamically typed" means at the signature level: the call site stops
 * checking a concrete type, and the value arrives boxed with its own type
 * available for reflection.
 *
 * D3 is explicit that this is the SIGNATURE only.  Local inference still runs
 * inside the body, so `(let [x 7.1] (* x 2.0))` stays a double multiply in a
 * Saffron file; the ladder from there (un-boxing within a body, then
 * specialising a defn only ever called at one type) is future work this does
 * not foreclose.
 *
 * Deliberately NOT applied to three neighbouring defaults that also read
 * TY_INT:
 *   - a `& rest` parameter, in `defn` and `fn` alike.  That int is a cons-list
 *     HANDLE, not a value type, and the rest element type has its own
 *     declaration rules (AR6).
 *   - an `extern-c` parameter, which declares a C signature.  `any` there would
 *     describe an ABI that does not exist.
 *
 * `defeffect` DOES take this default, since 2026-09-14 -- it did not before,
 * and an unannotated effect parameter silently meant `int` in a file whose
 * whole premise is the other default (saffron-defeffect-params-default-to-int).
 * An effect parameter is an ordinary value slot, unlike the two exclusions
 * above, so there was never a reason for it to diverge.
 *
 * Keyed on the span's file, so a Saffron program that loads a Turmeric module
 * gets each file's own default -- see lang_span_is_dynamic.  Not static: also
 * read by defeffect elaboration in elab_effects.c. */
TypeKind saffron_default_param_kind(Span sp) {
    return lang_span_is_dynamic(sp) ? TY_ANY : TY_INT;
}


/* saffron-dynamic-surface-pass (low): `(defn mkv [] [1 2.5])` was read as the
 * generic form -- type params `[]`, params `[1 2.5]` -- and rejected with
 * "parameter must be a symbol or type annotation".  Two leading vectors are
 * the generic shape only when the SECOND could be a parameter list; a vector
 * carrying a literal in a top-level slot cannot be one, so it is the body and
 * the first vector is the (possibly empty) parameter list.  Only literals
 * decide: a symbol-only second vector (`[x y]`) is still read as params, as
 * before, since either reading is well-formed there. */
static bool form_vec_has_literal_item(const Form *v) {
    if (!v || v->tag != F_VEC) return false;
    for (uint32_t i = 0; i < v->as.list.len; i++) {
        switch (v->as.list.items[i]->tag) {
            case F_INT: case F_FLOAT: case F_STR: case F_BOOL:
                return true;
            default: break;
        }
    }
    return false;
}

Expr *elab_defn(Elab *e, const Form *call) {
    /* forward-call-to-aggregate-result-types-as-carrier: a module type
     * elaborated since the last defn may be what a pending forward decl's
     * return was waiting on; settle those before this body calls them. */
    if (e->fwd_pending_results) elab_fwd_refresh_pending(e);
    /* Phase R5: Check for #[no-unwind] attribute before name.
     * #[used]: retain with external C linkage (see Binding.retain_c_linkage).
     * Both are bare-symbol attributes and may appear in either order. */
    uint32_t name_idx = 1;  /* index of name in items (after 'defn') */
    bool no_unwind = false;
    bool retain_c_linkage = false;
    Form *name_f = call->as.list.items[name_idx];
    while (name_f->tag == F_SYM &&
           (name_f->as.sym == e->sym_no_unwind_attr ||
            name_f->as.sym == e->sym_used_attr)) {
        if (name_f->as.sym == e->sym_no_unwind_attr) no_unwind = true;
        else retain_c_linkage = true;
        name_idx++;
        name_f = call->as.list.items[name_idx];
    }

    /* Phase M6: Check for (export-as "c_name") attribute before name.
     * Syntax: (defn (export-as "c_name") fname [...] :ret body...)
     * The attribute is a list whose head is the symbol export-as. */
    const char *c_export_name = NULL;
    if (name_f->tag == F_LIST && name_f->as.list.len == 2 &&
        name_f->as.list.items[0]->tag == F_SYM &&
        name_f->as.list.items[0]->as.sym == e->sym_export_as_attr) {
        Form *cname_arg = name_f->as.list.items[1];
        if (cname_arg->tag != F_STR) {
            diag_emit(DIAG_ERROR, name_f->span,
                      "^:export-as argument must be a string literal: (export-as \"c_name\")");
            return NULL;
        }
        char *cname_buf = (char *)arena_alloc(e->arena, cname_arg->as.s.len + 1);
        memcpy(cname_buf, cname_arg->as.s.p, cname_arg->as.s.len);
        cname_buf[cname_arg->as.s.len] = '\0';
        c_export_name = cname_buf;
        name_idx++;
        name_f = call->as.list.items[name_idx];
    }

    /* F4 (cross-plan-followups): ^deprecated attribute before name.
     * Syntax: (defn ^deprecated "message" name [...] :ret body...)
     *         (defn ^deprecated name [...] :ret body...)            ; no message
     * Each use site of the defined binding emits a DIAG_WARNING with
     * the message (or a generic note if the message is omitted). */
    bool is_deprecated_attr = false;
    const char *deprecation_msg = NULL;
    /* reflected-measures RF0: `^reflect` before the name, composing with
     * `^deprecated` in either order.  Placement is on the defn, like
     * `#reads`/`#writes`: the totality obligation hangs on the FUNCTION, which
     * is where TUR-E0384 must land.  Without the gate the attribute is inert
     * and says so -- an annotation that quietly does nothing is how a reader
     * comes to believe a predicate is enforced when it is not. */
    bool        is_reflect_attr = false;
    const Form *reflect_annot   = NULL;
    /* effect-row-honesty W0: `^construct` (M2a) and `^byval` (M5) also sit
     * here.  They used to be spelled inside the effect row, ^construct /
     * ^byval, and plucked out before the row was built; an attribute is
     * not an effect, and the row is now only effects and row variables. */
    bool defn_has_construct_attr = false;
    bool defn_has_byval_attr = false;
    /* The pre-name attributes, in any order. */
    for (;;) {
        if (name_f->tag != F_SYM) break;
        const Symbol *as = name_f->as.sym;
        if (as == e->sym_caret_reflect && !is_reflect_attr) {
            is_reflect_attr = true;
            reflect_annot   = name_f;
        } else if (as == e->sym_caret_construct && !defn_has_construct_attr) {
            defn_has_construct_attr = true;
        } else if (as == e->sym_caret_byval && !defn_has_byval_attr) {
            defn_has_byval_attr = true;
        } else if (as == e->sym_caret_deprecated && !is_deprecated_attr) {
            is_deprecated_attr = true;
            name_idx++;
            if (name_idx >= call->as.list.len) {
                diag_emit(DIAG_ERROR, name_f->span,
                          "^deprecated must be followed by an optional message string "
                          "and the function name");
                return NULL;
            }
            Form *next = call->as.list.items[name_idx];
            if (next->tag == F_STR) {
                char *msg_buf = (char *)arena_alloc(e->arena, next->as.s.len + 1);
                memcpy(msg_buf, next->as.s.p, next->as.s.len);
                msg_buf[next->as.s.len] = '\0';
                deprecation_msg = msg_buf;
                name_idx++;
                if (name_idx >= call->as.list.len) {
                    diag_emit(DIAG_ERROR, next->span,
                              "^deprecated message must be followed by the function name");
                    return NULL;
                }
            }
            name_f = call->as.list.items[name_idx];
            continue;
        } else if (as->len > 1 && as->name[0] == '^' &&
                   as->name[1] >= 'a' && as->name[1] <= 'z') {
            /* An attribute this position does not take (or one given twice).
             * Without this, `(defn ^contruct some ...)` defined a function
             * named `^contruct` and the real name became its parameters. */
            diag_emit(DIAG_ERROR, name_f->span,
                      "defn: '%s' is not an attribute a defn takes before its name "
                      "(those are ^construct, ^byval, ^deprecated, ^reflect, each "
                      "at most once)", as->name);
            return NULL;
        } else {
            break;
        }
        name_idx++;
        if (name_idx >= call->as.list.len) {
            diag_emit(DIAG_ERROR, name_f->span,
                      "%s must be followed by the function name", as->name);
            return NULL;
        }
        name_f = call->as.list.items[name_idx];
    }
    if (is_reflect_attr && !g_opt_reflected_measures) {
        diag_emit(DIAG_WARNING, reflect_annot->span,
                  "^reflect is ignored: the measure stays opaque to the "
                  "refinement solver (enable with --enable=reflected-measures)");
        is_reflect_attr = false;
    }

    /* Minimum: (defn name []) or (defn #[no-unwind] name []) */
    if (name_idx + 2 >= call->as.list.len) {  /* need name, params, body */
        diag_emit(DIAG_ERROR, call->span,
                  "defn requires (defn name [params...] body...)");
        return NULL;
    }

    /* Parse name */
    if (name_f->tag != F_SYM) {
        diag_emit(DIAG_ERROR, name_f->span, "defn name must be a symbol");
        return NULL;
    }
    /* TUR-W0042 (docs/archive/history/defn-shadows-return-special-form.md): defining a
     * function whose name is a reserved special form (`return`, `match`, ...)
     * is accepted and bound, but every bare call site dispatches to the special
     * form instead.  Warn HERE, at the definition, rather than leaving the
     * author to decode a type error against the caller's argument.  Suppressed
     * during stdlib auto-load and for specialization clones (which re-elaborate
     * the same Form and would double-report). */
    if (!e->in_stdlib_load && !e->bare_fat_spec_active)
        tur_warn_if_shadows_special_form(name_f->as.sym, name_f->span, "defn");

    Binding *existing = scope_lookup(e->scope, name_f->as.sym);
    /* bare-fat-result-monomorphization: a specialized clone re-elaborates the
     * same `(defn ...)` Form under a mangled name, so the original (lazy)
     * binding is still in scope under the source name.  Do not treat it as a
     * redefinition / forward decl -- the clone gets a fresh binding below. */
    if (e->bare_fat_spec_active) existing = NULL;
    if (existing) {
        /* MF3: hard-error on collision with an auto-loaded stdlib name. The
         * elaborator otherwise treats stdlib bindings as forward declarations
         * (because they're TY_FN + is_global, same shape as a pass-1 user
         * forward-decl) and lets the user defn shadow them; the C compile
         * then fails with "conflicting types for 'ok'" or similar. Producing
         * a clear diagnostic here avoids the broken-C symptom.  Suppress
         * during stdlib auto-load itself so stdlib pass-1 forward decls can
         * be matched by their pass-2 real definitions. */
        if (existing->is_from_stdlib && !e->in_stdlib_load) {
            diag_emit(DIAG_ERROR, name_f->span,
                      "defn: '%s' is already defined by an auto-loaded stdlib "
                      "module; rename the local definition",
                      name_f->as.sym->name);
            return NULL;
        }
        /* Allow forward-declared bindings from pass 1 to be redefined */
        /* Forward declarations have TY_FN type (from pass 1) */
        if (existing->type.kind == TY_FN && existing->is_global) {
            /* This is a forward declaration - proceed with the real definition.
             *
             * duplicate-defn-in-one-file-reaches-the-c-compiler: unless another
             * defn form in this same file already claimed it.  Pass 1 declares
             * a name once, so a second `(defn f ...)` lands on the binding the
             * FIRST one filled in -- same shape, TY_FN + global -- and used to
             * be taken for a forward declaration too: `check` passed, turi ran
             * the later body, and cc met two C functions named `f`.  Mirrors
             * elab_def's rule: an earlier REPL/playground turn's defn is
             * replaced (PS4, elab_prior_turn_global); a duplicate within one
             * turn or one file is an error.  Scoped to one file on purpose --
             * a name from an import, an extern-c or a native stub has no claim
             * and keeps the old behaviour -- and to user code: a stdlib load
             * keeps its own rules (MF3 above). */
            const Span *claim = &existing->defn_claim;
            if (!e->in_stdlib_load && claim->line != 0 &&
                claim->file_id == call->span.file_id &&
                (claim->off_start != call->span.off_start ||
                 claim->off_end != call->span.off_end) &&
                !elab_prior_turn_global(e, existing)) {
                diag_emit(DIAG_ERROR, name_f->span,
                          "defn: '%s' is already defined",
                          name_f->as.sym->name);
                diag_emit(DIAG_NOTE, *claim, "'%s' was first defined here",
                          name_f->as.sym->name);
                return NULL;
            }
            existing->defn_claim = call->span;
        } else {
            diag_emit(DIAG_ERROR, name_f->span,
                      "defn: '%s' is already defined", name_f->as.sym->name);
            return NULL;
        }
    }

    const Symbol *fn_type_params[8];
    Kind fn_type_param_kinds[8];
    uint8_t n_fn_type_params = 0;
    uint8_t n_implicit_fn_type_params = 0;
    memset(fn_type_params, 0, sizeof(fn_type_params));
    for (uint8_t i = 0; i < 8; i++) fn_type_param_kinds[i] = KIND_STAR;

    uint32_t params_idx = name_idx + 1;
    /* Gap H item 1: collect class-constraint forms from an optional middle
     * vector when the user spells the function as the plan-canonical
     *   (defn name [TypeVars] [(Class1 V) (Class2 V) ...] [params] :ret body)
     * shape. Each constraint form is `(ClassName TypeVar1 TypeVar2 ...)`;
     * we stash them here and register them once the typeclass-resolution
     * machinery is reached below. */
    const Form *constraint_forms[8];
    uint8_t n_constraint_forms = 0;
    /* caret-constraint-vector-not-registered: `^Class binder` pairs collected
     * from the defn TYPE-PARAM vector (`(defn f [^Show a] [x : a] ...)`).
     * Previously `^Show` was silently minted as a KIND_ARROW type parameter
     * named after the class and NO TypeConstraint existed anywhere, so
     * call-site discharge and the interpreter's apply-time dict binding never
     * saw the obligation.  Materialized into constraint_list next to the
     * middle-vector constraints below. */
    TypeClass    *tpv_con_class[MAX_FN_CONSTRAINTS];
    const Symbol *tpv_con_binder[MAX_FN_CONSTRAINTS];
    uint8_t       n_tpv_cons = 0;
    if (call->as.list.len > name_idx + 2 &&
        call->as.list.items[name_idx + 1]->tag == F_VEC &&
        call->as.list.items[name_idx + 2]->tag == F_VEC &&
        !form_vec_has_literal_item(call->as.list.items[name_idx + 2])) {
        Form *type_params_f = call->as.list.items[name_idx + 1];
        /* Classes awaiting their binder: `^Show ^Eq a` constrains `a` twice. */
        TypeClass *tpv_pending[MAX_FN_CONSTRAINTS];
        uint8_t    n_tpv_pending = 0;
        for (uint32_t i = 0; i < type_params_f->as.list.len; i++) {
            Form *tp = type_params_f->as.list.items[i];
            if (tp->tag != F_SYM) {
                diag_emit(DIAG_ERROR, tp->span,
                          "defn: type parameter must be a symbol");
                return NULL;
            }
            const Symbol *psym = tp->as.sym;
            /* L6 follow-up B: `^&name` in the explicit defn type-param
             * vector marks a row-kinded ([*]) type variable, the defn analog
             * of defstruct/deftype/defgadt/defdata's `^&`. The `^&` is
             * stripped so parameter and return type annotations reference
             * the bare name, and the kind is stashed so call-site row-arg
             * validation (check_row_type_arg_kind) and uses in `#row{...}`
             * elements both see kind [*]. */
            if (psym->len > 2 && psym->name[0] == '^' && psym->name[1] == '&') {
                if (n_fn_type_params >= 8) {
                    diag_emit(DIAG_ERROR, tp->span,
                              "defn: too many type parameters (max 8)");
                    return NULL;
                }
                const Symbol *bare = symtab_intern(e->st,
                    strslice(psym->name + 2, psym->len - 2));
                fn_type_params[n_fn_type_params] = bare;
                fn_type_param_kinds[n_fn_type_params] = KIND_TYPEROW;
                n_fn_type_params++;
            } else if (psym->len > 1 && psym->name[0] == '^') {
                /* An UPPERCASE name after the caret that resolves to a defined
                 * typeclass is a constraint on the NEXT binder symbol -- the
                 * type-param-vector spelling of the params-vector `^Class`
                 * annotation (elab_defn's pending_constraints path below).  An
                 * unknown uppercase name keeps the legacy behavior (a
                 * higher-kinded type parameter) rather than erroring. */
                if (psym->name[1] >= 'A' && psym->name[1] <= 'Z') {
                    const Symbol *tc_sym = symtab_intern(e->st,
                        strslice(psym->name + 1, psym->len - 1));
                    TypeClass *tc = typeclass_env_lookup_typeclass(
                        &e->typeclass_env, tc_sym);
                    if (tc) {
                        if (n_tpv_pending >= MAX_FN_CONSTRAINTS) {
                            diag_emit(DIAG_ERROR, tp->span,
                                      "defn: too many class constraints (max %d)",
                                      MAX_FN_CONSTRAINTS);
                            return NULL;
                        }
                        tpv_pending[n_tpv_pending++] = tc;
                        continue;
                    }
                }
                /* MB2 (constrained-hkt-forall-mode-b-plan): a `^f` type parameter
                 * is higher-kinded (`* -> *`), the defn analog of the
                 * `(defclass Functor [^f] ...)` marker -- so the body may use it
                 * applied as `(f a)`.  Strip the caret; annotations reference the
                 * bare name.  (Higher arities via `^^f` are not needed yet.) */
                if (n_fn_type_params >= 8) {
                    diag_emit(DIAG_ERROR, tp->span,
                              "defn: too many type parameters (max 8)");
                    return NULL;
                }
                const Symbol *bare = symtab_intern(e->st,
                    strslice(psym->name + 1, psym->len - 1));
                fn_type_params[n_fn_type_params] = bare;
                fn_type_param_kinds[n_fn_type_params] = KIND_ARROW;
                n_fn_type_params++;
            } else {
                /* Bare symbol: a type parameter.  Deduplicate --
                 * `[^Hash K ^MapKey K V]` names K twice -- and derive the
                 * binder's kind from a pending class's own param kind, so an
                 * HKT class constraint yields an arrow-kinded binder. */
                bool already = false;
                for (uint8_t k = 0; k < n_fn_type_params; k++)
                    if (fn_type_params[k] == psym) { already = true; break; }
                if (!already) {
                    Kind bk = KIND_STAR;
                    for (uint8_t p = 0; p < n_tpv_pending && bk == KIND_STAR; p++) {
                        const TypeClass *ptc = tpv_pending[p];
                        if (!ptc->type_param_kinds) continue;
                        for (uint8_t k = 0; k < ptc->n_type_params; k++)
                            if (ptc->type_param_kinds[k] != KIND_STAR) {
                                bk = KIND_ARROW;
                                break;
                            }
                    }
                    if (n_fn_type_params >= 8) {
                        diag_emit(DIAG_ERROR, tp->span,
                                  "defn: too many type parameters (max 8)");
                        return NULL;
                    }
                    fn_type_params[n_fn_type_params] = psym;
                    fn_type_param_kinds[n_fn_type_params] = bk;
                    n_fn_type_params++;
                }
                for (uint8_t p = 0; p < n_tpv_pending; p++) {
                    if (n_tpv_cons >= MAX_FN_CONSTRAINTS) {
                        diag_emit(DIAG_ERROR, tp->span,
                                  "defn: too many class constraints (max %d)",
                                  MAX_FN_CONSTRAINTS);
                        return NULL;
                    }
                    tpv_con_class[n_tpv_cons]  = tpv_pending[p];
                    tpv_con_binder[n_tpv_cons] = psym;
                    n_tpv_cons++;
                }
                n_tpv_pending = 0;
            }
        }
        /* A dangling `^Class` with no following binder keeps the legacy
         * meaning: a higher-kinded type parameter named after the class. */
        for (uint8_t p = 0; p < n_tpv_pending; p++) {
            if (n_fn_type_params >= 8) {
                diag_emit(DIAG_ERROR, type_params_f->span,
                          "defn: too many type parameters (max 8)");
                return NULL;
            }
            fn_type_params[n_fn_type_params] = tpv_pending[p]->name;
            fn_type_param_kinds[n_fn_type_params] = KIND_ARROW;
            n_fn_type_params++;
        }
        params_idx = name_idx + 2;

        /* Gap H item 1: detect the three-vec form
         *   (defn name [TypeVars] [(Class V) ...] [params] :ret body)
         * by looking for a third F_VEC at name_idx + 3 whose payload looks
         * like a parameter list. If we see one, treat name_idx + 2 as the
         * constraint vector and bump params_idx to name_idx + 3.
         * Each constraint must be an F_LIST `(ClassName TyVar1 TyVar2 ...)`. */
        if (call->as.list.len > name_idx + 3 &&
            call->as.list.items[name_idx + 3]->tag == F_VEC) {
            Form *maybe_constraints = call->as.list.items[name_idx + 2];
            bool looks_like_constraints = (maybe_constraints->as.list.len > 0);
            for (uint32_t ci = 0; looks_like_constraints && ci < maybe_constraints->as.list.len; ci++) {
                Form *cf = maybe_constraints->as.list.items[ci];
                if (cf->tag != F_LIST || cf->as.list.len < 1 ||
                    cf->as.list.items[0]->tag != F_SYM) {
                    looks_like_constraints = false;
                }
            }
            if (looks_like_constraints) {
                if (maybe_constraints->as.list.len > 8) {
                    diag_emit(DIAG_ERROR, maybe_constraints->span,
                              "defn: too many class constraints (max 8)");
                    return NULL;
                }
                for (uint32_t ci = 0; ci < maybe_constraints->as.list.len; ci++) {
                    constraint_forms[n_constraint_forms++] =
                        maybe_constraints->as.list.items[ci];
                }
                params_idx = name_idx + 3;
            }
        }
    }

    /* Parse param vector */
    Form *params_f = call->as.list.items[params_idx];
    if (params_f->tag != F_VEC) {
        diag_emit(DIAG_ERROR, params_f->span,
                  "defn: parameter list must be a vector [name1 name2 ...]");
        return NULL;
    }

    /* LS1: open a lifetime context for this signature so borrow-type
     * annotations (&'a int, &mut 'a int) in the params and return type intern
     * their lifetimes into shared, stable per-function LifetimeIds.  Restored to
     * the saved value before the body is elaborated (a nested defn gets its own).
     */
    LifetimeContext sig_ltctx;
    lifetime_context_init(&sig_ltctx);
    LifetimeContext *saved_ltctx = e->cur_lifetime_ctx;
    e->cur_lifetime_ctx = &sig_ltctx;

    const Form *implicit_ret_f = NULL;
    if (n_fn_type_params == 0) {
        implicit_ret_f = fn_return_annotation_form(call, params_idx + 1);
        n_implicit_fn_type_params = collect_implicit_fn_type_params(params_f, implicit_ret_f,
                                                                    fn_type_params, fn_type_param_kinds);
        n_fn_type_params = n_implicit_fn_type_params;
    }

    /* Parse params - Phase 15 supports typeclass constraints.
     * Parameter type annotations accept both fused `[x :T]` and spaced
     * `[x : T]` forms.
     *
     * Syntax: [^Eq a x : a, y : a] means:
     *   - ^Eq is a constraint annotation (symbol starting with ^)
     *   - a is a type variable
     *   - x : a is parameter x with type annotation :a
     *   - y : a is parameter y with type annotation :a
     * 
     * We parse sequentially, collecting constraints and creating parameters.
     */
    Binding **params = NULL;
    uint32_t n_params = 0;
    /* Per-param scratch is sized to the parameter-vector length -- a safe upper
     * bound on the parameter count (each param spends >= 1 vector slot) -- and
     * arena-allocated rather than a fixed [MAX_FN_ARITY] stack array, so a defn
     * may declare any number of positional parameters (arbitrary-fn-arity: no
     * hard cap). */
    uint32_t pcap = params_f->as.list.len ? params_f->as.list.len : 1;
    TypeKind *param_kinds = (TypeKind *)arena_alloc(e->arena, pcap * sizeof(TypeKind));
    /* Phase HRT1: full type annotations for rank-2 poly params (NULL if not poly) */
    Type **param_poly_types = (Type **)arena_alloc(e->arena, pcap * sizeof(Type *));
    for (uint32_t _i = 0; _i < pcap; _i++) param_poly_types[_i] = NULL;
    /* typeclass-constrained-param-erases-adt-to-int64: the constraint binder
     * whose type the following BARE parameters take.  `[^Show a x]` reads as
     * `[^Show a x : a]` -- the binder was declared for those parameters, and
     * an untyped `x` defaulting to `int` silently bound the dispatch to the
     * int instance (or the single carrier-compatible instance) and erased a
     * by-value ADT argument to the int64 carrier at the call.  The run ends
     * at the first parameter that carries its own annotation. */
    const Symbol *constraint_binder_run = NULL;
    /* sized-types-cross-param-unification: retain each parameter's raw type
     * annotation Form so call-site unification can re-extract size-index
     * templates (e.g. `(SizedVec n)`).  NULL when a param has no list-form
     * annotation. */
    const Form **param_type_forms_buf = (const Form **)arena_alloc(e->arena, pcap * sizeof(const Form *));
    for (uint32_t _i = 0; _i < pcap; _i++) param_type_forms_buf[_i] = NULL;

    /* CT0: Contract type predicates from param annotations { v : T | p }.
     * Collected during param parsing; injected as pre-checks before the body. */
    const Form **ct_param_preds     = (const Form **)arena_alloc(e->arena, pcap * sizeof(const Form *));
    const char **ct_param_varnames  = (const char **)arena_alloc(e->arena, pcap * sizeof(const char *));
    uint32_t    *ct_param_param_idx = (uint32_t *)arena_alloc(e->arena, pcap * sizeof(uint32_t));
    uint32_t n_ct_param_preds = 0;
    for (uint32_t _ci = 0; _ci < pcap; _ci++) {
        ct_param_preds[_ci] = NULL;
        ct_param_varnames[_ci] = NULL;
        ct_param_param_idx[_ci] = 0;
    }

    /* Phase 15: Constraint parsing */
    /* Track pending constraints that apply to the next type variable */
    TypeClass *pending_constraints[8];  /* Max 8 constraints per function */
    uint8_t n_pending = 0;

    /* Map from type variable name to its index for constraint association */
    /* For v1, we use a simple approach: each constraint applies to the next type var */
    /* const Symbol *current_type_var = NULL; */  /* Deferred to v2 */

    /* Constraint set for this function - allocated on arena */
    TypeConstraint *constraint_list = NULL;
    uint8_t n_constraints = 0;

    /* Gap H item 1: register the (Class TyVar) forms collected from the
     * optional middle vector now that constraint_list is allocated. Each
     * form looks like `(HasPos W)`; we look up the class and stash a
     * TypeConstraint pointing at it. param_idx stays -1 until the param
     * loop binds W; type_arg stays TYPE_UNKNOWN. */
    if (n_constraint_forms > 0) {
        TypeConstraint *new_list = (TypeConstraint *)arena_alloc(e->arena,
            n_constraint_forms * sizeof(TypeConstraint));
        for (uint8_t ci = 0; ci < n_constraint_forms; ci++) {
            const Form *cf = constraint_forms[ci];
            const Symbol *class_sym = cf->as.list.items[0]->as.sym;
            TypeClass *tc = typeclass_env_lookup_typeclass(&e->typeclass_env, class_sym);
            if (!tc) {
                diag_emit(DIAG_ERROR, cf->span,
                          "defn: typeclass '%s' in constraint is not defined",
                          class_sym->name);
                return NULL;
            }
            const Symbol *tyvar_sym = NULL;
            if (cf->as.list.len >= 2 && cf->as.list.items[1]->tag == F_SYM) {
                tyvar_sym = cf->as.list.items[1]->as.sym;
            }
            new_list[ci].typeclass = tc;
            new_list[ci].type_arg = TYPE_UNKNOWN;
            new_list[ci].param_idx = -1;
            new_list[ci].tyvar = tyvar_sym;
            new_list[ci].return_resolved = false;
        }
        constraint_list = new_list;
        n_constraints = n_constraint_forms;
    }

    /* caret-constraint-vector-not-registered: materialize the `^Class binder`
     * pairs collected from the type-param vector, in the same layout as the
     * middle-vector constraints above -- type_arg resolves later from the
     * call site; `tyvar` carries the binder for constraint-driven dispatch
     * and the interpreter's apply-time dict binding. */
    if (n_tpv_cons > 0) {
        uint8_t new_count = (uint8_t)(n_constraints + n_tpv_cons);
        TypeConstraint *new_list = (TypeConstraint *)arena_alloc(e->arena,
            new_count * sizeof(TypeConstraint));
        if (constraint_list && n_constraints > 0)
            memcpy(new_list, constraint_list,
                   n_constraints * sizeof(TypeConstraint));
        for (uint8_t ci = 0; ci < n_tpv_cons; ci++) {
            new_list[n_constraints + ci].typeclass       = tpv_con_class[ci];
            new_list[n_constraints + ci].type_arg        = TYPE_UNKNOWN;
            new_list[n_constraints + ci].param_idx       = -1;
            new_list[n_constraints + ci].tyvar           = tpv_con_binder[ci];
            new_list[n_constraints + ci].return_resolved = false;
        }
        constraint_list = new_list;
        n_constraints = new_count;
    }

    /* Phase G3: Equality constraint env built from (: a T) param items */
    SkolemEnv param_constraint_env;
    param_constraint_env.n = 0;

    /* Phase HKT: kind-variable names collected from ^f annotations.
     * These are type-level variables with kind * -> *; no runtime param is
     * created.  They are pushed into a temporary scope so that return-type
     * annotations such as (Equal (f a) (f b)) can resolve (f a) as TY_APP. */
    const Symbol *kind_var_names[8];
    uint8_t n_kind_vars = 0;

    /* LT0: ^linear annotation applies to the next parameter */
    bool next_param_linear = false;
    /* UT0: ^unique annotation applies to the next parameter */
    bool next_param_unique = false;
    /* UT2: ^mut annotation applies to the next parameter */
    bool next_param_mut = false;
    /* ST0: ^affine / ^relevant annotations apply to the next parameter */
    bool next_param_affine    = false;
    bool next_param_relevant  = false;
    /* LB1: ^borrow annotation marks the next parameter as a non-consuming borrow */
    bool next_param_borrow    = false;
    /* A#1: ^fat annotation marks the next parameter as a fat-closure consumer */
    bool next_param_fat       = false;
    /* AR5: variadic rest parameter state */
    bool is_variadic = false;
    TypeKind rest_kind = TY_INT;  /* default rest element type */
    Type *rest_full_type = NULL;  /* typed-variadic: full Type for user-defined rest */
    /* closure-drop-glue (mw-compose-of): a `^borrow` immediately before `&`
     * marks the whole rest list as borrowed -- the callee reads/invokes each
     * element but retains none, so the caller may free a fresh uniquely-owned
     * closure passed as a rest arg at its own scope exit.  Recorded only under
     * the experiment; flag-off it stays false so the fn type is unchanged. */
    bool rest_borrow_flag = false;

    for (uint32_t i = 0; i < params_f->as.list.len; i++) {
        Form *p = params_f->as.list.items[i];

        /* AR5: & rest-name :type -- variadic rest parameter */
        if (p->tag == F_SYM && p->as.sym == e->sym_borrow) {
            if (is_variadic) {
                diag_emit(DIAG_ERROR, p->span, "defn: multiple '&' in parameter list");
                return NULL;
            }
            if (i + 1 >= params_f->as.list.len) {
                diag_emit(DIAG_ERROR, p->span,
                          "defn: '&' must be followed by a rest parameter name");
                return NULL;
            }
            Form *rest_p = params_f->as.list.items[i + 1];
            if (rest_p->tag != F_SYM) {
                diag_emit(DIAG_ERROR, rest_p->span,
                          "defn: rest parameter name must be a symbol");
                return NULL;
            }
            /* Parse optional type annotation: & rest :type */
            rest_kind = TY_INT;
            rest_full_type = NULL;
            if (i + 2 < params_f->as.list.len) {
                Form *type_p = params_f->as.list.items[i + 2];
                /* Accept fused `& rest :int` (F_KEYWORD) and spaced
                 * `& rest : int` (F_TYPE_ANN{inner: F_SYM/F_KEYWORD}). */
                Form *type_eff = type_p;
                if (type_p->tag == F_TYPE_ANN && type_p->as.list.len == 1 &&
                    (type_p->as.list.items[0]->tag == F_SYM ||
                     type_p->as.list.items[0]->tag == F_KEYWORD)) {
                    type_eff = type_p->as.list.items[0];
                }
                if (type_eff->tag == F_KEYWORD || type_eff->tag == F_SYM) {
                    if (!resolve_variadic_rest_type(e, type_eff,
                                                    fn_type_params, fn_type_param_kinds,
                                                    n_fn_type_params, "defn",
                                                    &rest_kind, &rest_full_type)) {
                        return NULL;
                    }
                    if (i + 3 < params_f->as.list.len) {
                        diag_emit(DIAG_ERROR, params_f->as.list.items[i + 3]->span,
                                  "defn: no parameters allowed after '& rest :type'");
                        return NULL;
                    }
                } else {
                    diag_emit(DIAG_ERROR, type_p->span,
                              "defn: '& rest' must be followed by a type annotation (e.g. :int)");
                    return NULL;
                }
            }
            is_variadic = true;
            /* closure-drop-glue (mw-compose-of): consume a pending `^borrow`
             * (`^borrow & rest ...`) as the rest-list borrow marker. */
            if (next_param_borrow) {
                rest_borrow_flag = true;
            }
            next_param_borrow = false;
            /* Add rest param as a regular int binding (cons-list pointer at runtime) */
            if (n_params == 0) {
                params = (Binding **)arena_alloc(e->arena, pcap * sizeof(Binding *));
            }
            /* saffron-dynamic-surface-pass (`& rest : any`): an `any` rest
             * element is a two-word tagged box and does not fit the int64
             * cons cell's head slot, so the rest list of a `: any` rest is
             * the stdlib `(Cons any)` monomorph -- the same cell a Saffron
             * `(list 1 "two" 7.1)` builds, whose head IS 16 bytes and whose
             * tail is the typed link (M7).  The parameter is typed as that
             * list, so the body walks it with `.head` / `.tail` / `tnil?`
             * and each element reads back with its own tag.  The call site
             * (elab_call.c) builds the chain with the `Cons` constructor
             * under an `any` ascription per element. */
            Type rest_bt = TYPE_INT;
            TypeKind rest_pk = TY_INT;
            if (rest_kind == TY_ANY) {
                Type *cons_t = elab_lookup_type_by_name(e, intern_cstr(e->st, "Cons"));
                if (cons_t && cons_t->kind == TY_ADT && cons_t->as.adt_.def &&
                    cons_t->as.adt_.def->n_type_params == 1) {
                    Type head = *cons_t;
                    head.hkt_kind = kind_for_arity(1);
                    Type any_t; memset(&any_t, 0, sizeof any_t);
                    any_t.kind = TY_ANY;
                    rest_bt = type_app(e->arena, head, any_t, rest_p->span);
                    rest_pk = TY_APP;
                }
            }
            param_kinds[n_params] = rest_pk;
            Binding *rest_b = binding_new(e, rest_p->as.sym, rest_bt, false, false, rest_p->span);
            rest_b->is_param = true;
            params[n_params++] = rest_b;
            break; /* & must be the last; done parsing params */
        }

        /* Phase G3: Handle equality constraint (: a T) in params.
         * Syntax: (: a int) means "type variable a equals int".
         * This is a type-level constraint; no runtime parameter is created.
         *
         * Two reader representations:
         *  Legacy:  F_LIST([sym(":"), sym("a"), sym("int")])  — 3-item list
         *  New:     F_LIST([F_TYPE_ANN(sym("a")), sym("int")])  — 2-item list
         *           (reader folds `: a` into a single F_TYPE_ANN node)
         */
        {
            Form *var_form = NULL;
            Form *type_form = NULL;
            if (p->tag == F_LIST && p->as.list.len == 3 &&
                p->as.list.items[0]->tag == F_SYM &&
                p->as.list.items[0]->as.sym == e->sym_colon) {
                var_form  = p->as.list.items[1];
                type_form = p->as.list.items[2];
            } else if (p->tag == F_LIST && p->as.list.len == 2 &&
                       p->as.list.items[0]->tag == F_TYPE_ANN &&
                       p->as.list.items[0]->as.list.len == 1) {
                var_form  = p->as.list.items[0]->as.list.items[0];
                type_form = p->as.list.items[1];
            } else if (p->tag == F_LIST && p->as.list.len == 2 &&
                       p->as.list.items[0]->tag == F_UNQUOTE &&
                       p->as.list.items[0]->as.list.len == 1 &&
                       p->as.list.items[0]->as.list.items[0]->tag == F_SYM) {
                /* Phase G3: (~ a T) equality constraint — reader turns (~ a T) into
                 * F_LIST[F_UNQUOTE(a), T] because ~ is the unquote reader macro */
                var_form  = p->as.list.items[0]->as.list.items[0]; /* symbol inside ~a */
                type_form = p->as.list.items[1];
            }
            if (var_form && type_form &&
                var_form->tag == F_SYM &&
                (type_form->tag == F_SYM || type_form->tag == F_KEYWORD)) {
                const char *tn = (type_form->tag == F_KEYWORD)
                    ? type_form->as.sym->name
                    : type_form->as.sym->name;
                TypeKind ck = typekind_from_symbol(tn);
                if (ck != TY_UNKNOWN && param_constraint_env.n < MAX_SKOLEM_BINDINGS) {
                    param_constraint_env.bindings[param_constraint_env.n].name =
                        var_form->as.sym->name;
                    param_constraint_env.bindings[param_constraint_env.n].kind = ck;
                    param_constraint_env.n++;
                }
                continue;
            }
        }

        /* LT0: ^linear annotation marks the next parameter as linear */
        if (p->tag == F_SYM && p->as.sym == e->sym_caret_linear) {
            next_param_linear = true;
            continue;
        }

        /* UT0: ^unique annotation marks the next parameter as unique */
        if (p->tag == F_SYM && p->as.sym == e->sym_caret_unique) {
            next_param_unique = true;
            continue;
        }

        /* UT2: ^mut annotation marks the next parameter as mutable (used with ^unique ^mut) */
        if (p->tag == F_SYM && p->as.sym == e->sym_caret_mut) {
            next_param_mut = true;
            continue;
        }

        /* ST0: ^affine annotation marks the next parameter as affine (no duplication) */
        if (p->tag == F_SYM && p->as.sym == e->sym_caret_affine) {
            next_param_affine = true;
            continue;
        }

        /* ST0: ^relevant annotation marks the next parameter as relevant (must be used) */
        if (p->tag == F_SYM && p->as.sym == e->sym_caret_relevant) {
            next_param_relevant = true;
            continue;
        }

        /* LB1: ^borrow annotation marks the next parameter as a non-consuming
         * borrow.  A linear/affine argument passed to this parameter is read
         * without discharging its single-consumption obligation (see the
         * call-site handling in elab_call.c).  Used by stdlib resource-handle
         * accessors (fs/tmpfile-path, mutex-lock, ...). */
        if (p->tag == F_SYM && p->as.sym == e->sym_caret_borrow) {
            next_param_borrow = true;
            continue;
        }

        /* A#1: ^fat annotation marks the next parameter as a fat-closure consumer.
         * A bare non-capturing fn passed to this parameter is auto-shimmed into a
         * fat closure at the call site (EX_FN_TO_FAT). */
        if (p->tag == F_SYM && p->as.sym == e->sym_caret_fat) {
            next_param_fat = true;
            continue;
        }

        /* MS2: ^multishot is not valid as a function parameter annotation */
        if (p->tag == F_SYM && p->as.sym == e->sym_caret_multishot) {
            diag_emit_with_code(DIAG_ERROR, p->span,
                TUR_E0501_MULTISHOT_ANN_OUTSIDE_HANDLER,
                "'^multishot' annotation is only valid on a handler continuation, "
                "not on a function parameter");
            return NULL;
        }

        /* Phase 15: Handle constraint annotations (^Eq, ^Show, etc.) */
        if (p->tag == F_SYM && p->as.sym->len > 0 && p->as.sym->name[0] == '^') {
            /* This is a constraint annotation like ^Eq */
            const Symbol *constraint_name = p->as.sym;
            /* Look up the typeclass (skip the ^ character) */
            const char *tc_name_str = constraint_name->name + 1;  /* Skip '^' */
            uint32_t tc_name_len = constraint_name->len - 1;

            /* Phase HKT H4: Kind variable annotation — type-level only, erased at runtime.
             * A kind variable like `^f` declares that `f` ranges over type constructors
             * of kind '* -> *'.  It creates no runtime parameter; it is used as a
             * kind annotation on the function and may be followed by a ^Typeclass f
             * constraint (which adds a regular typeclass constraint handled below). */
            if (tc_name_len > 0 && tc_name_str[0] >= 'a' && tc_name_str[0] <= 'z') {
                /* Phase HKT: Kind variable (e.g. ^f).  Record the bare name so
                 * the return-type parser can resolve (f a) as TY_APP.  No
                 * runtime parameter is created. */
                if (n_kind_vars < 8) {
                    kind_var_names[n_kind_vars++] = symtab_intern(e->st,
                        strslice(tc_name_str, tc_name_len));
                }
                continue;
            }

            /* Create symbol for typeclass name */
            char tmp_name[64];
            snprintf(tmp_name, sizeof(tmp_name), "%.*s", tc_name_len, tc_name_str);
            const Symbol *tc_sym = symtab_intern(e->st, strslice(tmp_name, tc_name_len));
            TypeClass *tc = typeclass_env_lookup_typeclass(&e->typeclass_env, tc_sym);
            if (!tc) {
                diag_emit(DIAG_ERROR, p->span,
                          "defn: typeclass '%.*s' in constraint is not defined",
                          tc_name_len, tc_name_str);
                return NULL;
            }
            if (n_pending < 8) {
                pending_constraints[n_pending++] = tc;
            } else {
                diag_emit(DIAG_ERROR, p->span,
                          "defn: too many constraints (max 8)");
                return NULL;
            }
            continue;
        }
        
        /* Phase 15: Handle type variable declarations */
        /* After constraints, the next symbol is a type variable name */
        if (n_pending > 0 && p->tag == F_SYM) {
            /* This is a type variable that the pending constraints apply to.
             *
             * dispatch-tyvar-name-independence: the binder name (`B` in
             * `^Backend B`) MUST be registered as a function type parameter.
             * Without it, a later parameter annotated with that name (`b : B`)
             * does not resolve against fn_type_params, so it falls through to
             * the KB-026 demotion pass below -- which rewrites the parameter's
             * type to an anonymous `<struct>` placeholder unless the name
             * happens to collide with some loaded ADT/struct's type-param name
             * (fn_name_is_adt_tyvar).  That accidental collision is exactly why
             * `^Backend B` resolved for B/A/.../K/V but not for T/X/Z or any
             * multi-letter binder: dispatch depended on the tyvar's *spelling*.
             * Registering the binder makes `b : B` a genuine named TY_TYVAR for
             * every spelling, so constraint-driven dispatch no longer hinges on
             * the name. */
            const Symbol *binder = p->as.sym;
            bool already_param = false;
            for (uint8_t k = 0; k < n_fn_type_params; k++) {
                if (fn_type_params[k] == binder) { already_param = true; break; }
            }
            if (!already_param && n_fn_type_params < 8) {
                fn_type_params[n_fn_type_params] = binder;
                fn_type_param_kinds[n_fn_type_params] = KIND_STAR;
                n_fn_type_params++;
            }

            /* Register all pending constraints for this type variable */
            /* Allocate space for new constraints */
            uint8_t new_count = n_constraints + n_pending;
            TypeConstraint *new_list = (TypeConstraint *)arena_alloc(e->arena,
                new_count * sizeof(TypeConstraint));
            if (constraint_list) {
                memcpy(new_list, constraint_list, n_constraints * sizeof(TypeConstraint));
            }
            constraint_list = new_list;

            for (uint8_t c = 0; c < n_pending; c++) {
                /* type_arg is resolved later from the call-site argument; the
                 * binder name is recorded so the constraint knows which type
                 * variable (and hence which parameter) it dispatches on. */
                constraint_list[n_constraints + c].typeclass = pending_constraints[c];
                constraint_list[n_constraints + c].type_arg = TYPE_UNKNOWN;
                constraint_list[n_constraints + c].param_idx = -1;
                constraint_list[n_constraints + c].tyvar = binder;
                constraint_list[n_constraints + c].return_resolved = false;
            }
            n_constraints = new_count;
            n_pending = 0;
            /* A `^f`-declared constructor variable (`^Functor f`) has kind
             * `* -> *` and is never a parameter's type; only a `*`-kinded
             * binder types the bare parameters that follow it. */
            constraint_binder_run = binder;
            for (uint8_t kvi = 0; kvi < n_kind_vars; kvi++) {
                if (kind_var_names[kvi] == binder) { constraint_binder_run = NULL; break; }
            }
            continue;
        }
        
        /* Phase HRT1: Handle complex type annotation (list form or F_TYPE_ANN) for previous param.
         * Syntax: [param-name (forall [a] (-> a a))] — list form follows a symbol.
         * Also: [param-name : (-> a b)] — F_TYPE_ANN wrapping any type form.
         * CT0: [param-name { v : T | pred }] — contract type annotation. */
        if (i < n_implicit_fn_type_params && p->tag == F_SYM &&
            fn_type_params[i] == p->as.sym) {
            continue;
        }

        if (p->tag == F_LIST || p->tag == F_VEC || p->tag == F_TYPE_ANN || p->tag == F_CONTRACT_TYPE) {
            if (n_params == 0) {
                diag_emit(DIAG_ERROR, p->span,
                          "defn: type annotation without preceding parameter");
                return NULL;
            }
            /* perform-does-not-typecheck-its-arguments: the author wrote a type
             * here, so this parameter's type is declared, not the carrier
             * default the binding was created with. */
            params[n_params - 1]->type_is_carrier_default = false;
            constraint_binder_run = NULL;
            /* An explicit annotation on a run-typed parameter replaces the
             * binder type it was given above, poly slot included. */
            param_poly_types[n_params - 1] = NULL;
            /* For F_TYPE_ANN, unwrap to the inner type form first */
            const Form *type_form = (p->tag == F_TYPE_ANN) ? p->as.list.items[0] : p;
            /* sized-types-cross-param-unification: record the raw type form so
             * call sites can re-extract the size-index template. */
            if (n_params > 0 && n_params <= MAX_FN_ARITY)
                param_type_forms_buf[n_params - 1] = type_form;
            /* SZ8 projection-size recovery: also stash the declared annotation
             * Form on the parameter binding itself, so a callee whose argument
             * is this parameter (or a field projection of it) can recover its
             * static size index at the call site. */
            params[n_params - 1]->decl_type_form = type_form;
            /* Phase CCL (fn-first-class-application): a spaced `: fn` annotation
             * (the bare `fn` keyword/symbol) is the first-class poly-closure
             * carrier -- mirror the fused `:fn` keyword handling above. */
            if ((type_form->tag == F_SYM || type_form->tag == F_KEYWORD) &&
                type_form->as.sym->len == 2 &&
                memcmp(type_form->as.sym->name, "fn", 2) == 0) {
                param_kinds[n_params - 1] = TY_PTR_VOID;
                params[n_params - 1]->type = TYPE_PTR_VOID;
                params[n_params - 1]->is_poly_fn = true;
                params[n_params - 1]->poly_type = NULL;
                continue;
            }
            /* Parse as a type expression — supports (forall [a] (-> a a)), (-> a b), etc. */
            Type *ann = fn_type_from_form(e, type_form,
                                          fn_type_params, fn_type_param_kinds, n_fn_type_params);
            if (!ann) return NULL;
            /* CT0: a contract-typed parameter takes its BASE type at the C
             * level, and its predicate becomes an entry check (CT1) plus --
             * under `refined` -- a hypothesis for the return obligation (RT1).
             *
             * Collecting from the RESOLVED TYPE rather than from the raw Form
             * is what makes all three spellings work uniformly: the bare
             * brace form `[x {v : int | p}]`, the current `[x : #refine{...}]`
             * (which the reader wraps in an F_TYPE_ANN -- the Form-shaped
             * check missed this one entirely, so such a parameter silently got
             * NO runtime check at all), and a NAMED refinement alias
             * `[x : Nat]` declared with `deftype` (stdlib/refine.tur), which
             * has no contract Form at the use site at all. */
            if (ann->kind == TY_CONTRACT && ann->as.contract_.base_type) {
                TypeKind base_kind = ann->as.contract_.base_type->kind;
                param_kinds[n_params - 1] = base_kind;
                params[n_params - 1]->type = *ann->as.contract_.base_type;
                if (ann->as.contract_.predicate && n_ct_param_preds < MAX_FN_ARITY) {
                    ct_param_preds[n_ct_param_preds]     = ann->as.contract_.predicate;
                    ct_param_varnames[n_ct_param_preds]  = ann->as.contract_.var_name;
                    ct_param_param_idx[n_ct_param_preds] = n_params - 1;
                    n_ct_param_preds++;
                }
                continue;
            }
            if (ann->kind == TY_FORALL) {
                /* Rank-2 polymorphic parameter: represented as tur_poly_fn_t at C level */
                param_kinds[n_params - 1] = TY_PTR_VOID;
                params[n_params - 1]->type = TYPE_PTR_VOID;
                params[n_params - 1]->is_poly_fn = true;
                params[n_params - 1]->poly_type = ann;
                param_poly_types[n_params - 1] = ann;
            } else if (ann->kind == TY_EXISTS) {
                /* F1-1: TY_EXISTS is a value type (produced by `pack`), not a
                 * rank-2 function.  Keep the full TY_EXISTS payload on the
                 * binding so `open` inside the body finds a valid
                 * `as.forall_.body`, and stash the full type in
                 * param_poly_types so call sites can subtype-check the
                 * argument against the declared existential.  Earlier code
                 * misrouted this into the rank-2 branch, which then
                 * rejected `(pack ...)` arguments with "rank-2 argument
                 * must be a named function". */
                param_kinds[n_params - 1] = TY_EXISTS;
                params[n_params - 1]->type = *ann;
                param_poly_types[n_params - 1] = ann;
            } else if (ann->kind == TY_FN) {
                Binding *pb = params[n_params - 1];
                /* F5 (fn-first-class-application, typed carrier): a plain
                 * (non-fat, non-substructural) `(fn [A...] : R)` parameter is the
                 * *typed* first-class `:fn` carrier -- the same tur_poly_fn_t
                 * {env, fn} representation as a bare `:fn`, but with a known
                 * concrete signature.  Routing it through the poly carrier rather
                 * than the bare-function-pointer regular path lets it uniformly
                 * hold a named fn, a lambda, OR a capturing closure (each boxed
                 * into {env, fn}), and round-trip float/cstr/ptr argument and
                 * result kinds via the concrete-signature cast at the call site
                 * (see elab_poly_call + emit_expr.c is_poly_call).  Fat (^fat)
                 * and substructural params keep their nominal TY_FN type -- they
                 * have their own calling conventions/discipline checks.
                 * See docs/archive/history/fn-first-class-float-carrier-gap.md. */
                bool plain = !pb->is_fat && !pb->is_borrow && !pb->is_unique &&
                             !pb->is_mut && !pb->is_linear && !pb->is_affine &&
                             !pb->is_relevant;
                /* A function type carrying an effect row (e.g. `(fn [:cstr]
                 * #{Write} :nil)`) must keep its nominal TY_FN type so the
                 * effect row propagates to callers when the param is invoked --
                 * the tur_poly_fn_t carrier has no effect-row slot.  Likewise a
                 * type variable in the signature (rank-2 / polymorphic element)
                 * needs the arg_full_types path.  Stay on the regular path for
                 * those; only fully-concrete, effect-free signatures become the
                 * typed carrier. */
                bool effectful = ann->as.fn.effect_row != NULL;
                /* typed-c-abi-function-pointers: a cfnptr is a *bare* C function
                 * pointer, not a tur_poly_fn_t {env, fn} carrier -- it must keep
                 * its nominal TY_FN (cfnptr) type so the param lowers to the
                 * concrete `R (*)(A...)` typedef and only captureless callbacks
                 * are admitted.  Never demote it onto the poly carrier. */
                /* Increment 4's successor -- the fn axis moves from a
                 * shadowed derivation to a consulted decision.  The census
                 * measured 2122 fn-param routings per corpus sweep made
                 * OUTSIDE the decision function; this is the site that makes
                 * them, and the gate set is now one named routine other
                 * sites can ask instead of re-deriving.  `plain` and
                 * `effectful` remain here because the routine reads them
                 * back off the binding and annotation. */
                bool carrier_ok =
                    repr_of_fn_param(pb, ann) == REPR_CARRIER_I64;
                /* fn-value-fat-normalization stage 2: a NESTED concrete
                 * effect-free fn RESULT inside a fn-typed param annotation is
                 * marked boxed, recursively -- stage-2 producers return fat
                 * handles for such types, so an annotation left thin would
                 * make `((f 1) 2)` thin-dispatch a fat handle (the
                 * curried-fn-typed-param SIGSEGV found by the stage-2 suite
                 * measurement). */
                {
                    Type *nr = ann->as.fn.result_full_type;
                    while (nr && nr->kind == TY_FN && !nr->as.fn.boxed &&
                           fn_result_type_is_fat_normalized(nr)) {
                        nr->as.fn.boxed = true;
                        nr = nr->as.fn.result_full_type;
                    }
                }
                /* repr-trace (representation-consolidation-meta-plan increment
                 * 0): with --emit-abi-trace, print the representation this
                 * fn-typed parameter was routed onto, and -- for the thin
                 * nominal TY_FN fallthrough -- which gate forced it there.
                 * This makes the per-boundary decision diffable: a
                 * consolidation increment can assert "only the intended
                 * boundaries moved" by diffing traces. */
                if (g_emit_abi_trace) {
                    const char *repr;
                    const char *why = "";
                    if (carrier_ok)                repr = "carrier";
                    else if (pb->is_fat)           repr = "fat";
                    else if (ann->as.fn.cfnptr)    repr = "cfnptr";
                    else {
                        repr = "thin-fn";
                        if (effectful)                              why = " effect-row";
                        else if (ann->as.fn.is_variadic)            why = " variadic";
                        else if (fn_type_has_named_tyvar(ann))      why = " tyvar-sig";
                        else if (!fn_type_is_carrier_safe(ann))     why = " non-scalar-sig";
                        else if (!plain)                            why = " substructural";
                        else                                        why = " arity";
                    }
                    fprintf(stderr, "repr-trace %u:%u fn-param %s %s%s\n",
                            p->span.line, p->span.col_start,
                            pb->name ? pb->name->name : "_", repr, why);
                }
                if (carrier_ok) {
                    param_kinds[n_params - 1] = TY_PTR_VOID;
                    pb->type = TYPE_PTR_VOID;
                    pb->is_poly_fn = true;
                    pb->poly_type = ann;        /* concrete signature (not a forall) */
                    param_poly_types[n_params - 1] = ann;
                    continue;
                }
                /* Plain function type annotation */
                param_kinds[n_params - 1] = TY_FN;
                params[n_params - 1]->type = *ann;
                /* LT2 / Defect-A: always store full TY_FN type in param_poly_types so
                 * it round-trips into arg_full_types for all callers -- including the
                 * early-update path (lines 1896-1904) that propagates arg_full_types to
                 * the forward-declaration binding before the body is elaborated.  Without
                 * this, a sibling defn that forward-references a ^fat fn-typed parameter
                 * sees arg_full_types=NULL on the forward binding and cannot subtype-check
                 * against the declared (fn [...] ret) signature.  The -Xlinear gate was
                 * a red herring: the full type is needed for all callers, not just linear
                 * ones.  Non-fat plain TY_FN parameters that go through the carrier_ok
                 * path above already set param_poly_types unconditionally. */
                param_poly_types[n_params - 1] = ann;
            } else {
                /* Other type annotation — use the kind */
                param_kinds[n_params - 1] = ann->kind;
                params[n_params - 1]->type = *ann;
                /* IT1: For union-typed parameters, store full type in param_poly_types
                 * so it propagates into arg_full_types for subtyping checks at call sites. */
                if (ann->kind == TY_UNION) {
                    param_poly_types[n_params - 1] = ann;
                }
                /* IT2: For intersection-typed parameters, same propagation. */
                if (ann->kind == TY_INTERSECTION) {
                    param_poly_types[n_params - 1] = ann;
                }
                /* PH1.1: For handler-typed parameters, store the full type so
                 * the declared handled-effect row + value/result kinds reach
                 * arg_full_types at the call site, enabling row-precise
                 * argument checking (PH1.2) and a precise mismatch diagnostic
                 * (PH1.3) instead of a kind-only handler<?, ?, ?> comparison. */
                if (ann->kind == TY_HANDLER) {
                    param_poly_types[n_params - 1] = ann;
                }
                /* GS2: preserve full applied struct parameter types so call
                 * sites can distinguish (Box int) from (Box float) instead of
                 * comparing only the TY_APP kind shell. */
                if (ann->kind == TY_APP) {
                    param_poly_types[n_params - 1] = ann;
                }
                if (fn_type_has_named_tyvar(ann)) {
                    param_poly_types[n_params - 1] = ann;
                }
            }
            /* LT3: Propagate linearity from the type annotation (e.g., [p : (lref int)]) */
            if (params[n_params - 1]->type.copy_kind == CK_LINEAR) {
                params[n_params - 1]->is_linear = true;
            }
            /* ST2: ref<T> params without an explicit discipline annotation are
             * inferred as SK_LINEAR. */
            if (!params[n_params - 1]->is_linear
                    && !params[n_params - 1]->is_affine
                    && !params[n_params - 1]->is_relevant
                    && params[n_params - 1]->type.kind == TY_REF) {
                params[n_params - 1]->is_linear = true;
                params[n_params - 1]->type.substruct = SK_LINEAR;
            }
            /* UT0: Propagate uniqueness from the type annotation */
            if (next_param_unique) {
                params[n_params - 1]->is_unique = true;
                params[n_params - 1]->type.copy_kind = CK_UNIQUE;
                next_param_unique = false;
            }
            /* UT2: Propagate mutability from the ^mut annotation */
            if (next_param_mut) {
                params[n_params - 1]->is_mut = true;
                next_param_mut = false;
            }
            continue;
        }

        /* Accept both fused :type (F_KEYWORD) and spaced `: type` (F_TYPE_ANN{inner: F_SYM}) */
        const Form *p_eff = (p->tag == F_TYPE_ANN) ? p->as.list.items[0] : p;
        if (p_eff->tag != F_SYM && p_eff->tag != F_KEYWORD) {
            diag_emit(DIAG_ERROR, p->span,
                      "defn: parameter must be a symbol or type annotation");
            /* params is arena-allocated, no need to free */
            return NULL;
        }

        /* Handle type annotations: if this is a keyword like :int, it's a type for the previous param */
        if (p->tag == F_KEYWORD || p->tag == F_TYPE_ANN) {
            /* This is a type annotation for the previous parameter */
            if (n_params == 0) {
                diag_emit(DIAG_ERROR, p->span,
                          "defn: type annotation without preceding parameter");
                return NULL;
            }
            /* perform-does-not-typecheck-its-arguments: the author wrote a type
             * here, so this parameter's type is declared, not the carrier
             * default the binding was created with. */
            params[n_params - 1]->type_is_carrier_default = false;
            constraint_binder_run = NULL;
            /* An explicit annotation on a run-typed parameter replaces the
             * binder type it was given above, poly slot included. */
            param_poly_types[n_params - 1] = NULL;
            /* Update the type of the last parameter */
            const Symbol *kw = p_eff->as.sym;
            uint8_t type_param_idx = 0;
            if (fn_type_param_index(fn_type_params, n_fn_type_params, kw, &type_param_idx)) {
                param_kinds[n_params - 1] = TY_TYVAR;
                params[n_params - 1]->type = type_tyvar_named(kw->name);
                params[n_params - 1]->type.hkt_kind = fn_type_param_kinds[type_param_idx];
                param_poly_types[n_params - 1] = (Type *)arena_alloc(e->arena, sizeof(Type));
                *param_poly_types[n_params - 1] = params[n_params - 1]->type;
                continue;
            }
            /* Phase CCL (fn-first-class-application): a bare `:fn` parameter is a
             * first-class poly-closure carrier (tur_poly_fn_t {env, fn}), the same
             * representation typeclass-method `:fn` params use.  Marking it
             * is_poly_fn (with a NULL poly_type to distinguish it from a rank-2
             * forall) makes it directly callable -- `(g x)` routes through
             * elab_poly_call -- and coercible into a `^fat` sink (EX_POLY_TO_FAT)
             * or constructed from a lambda/closure (EX_POLY_WRAP) at call sites. */
            if (kw->len == 2 && memcmp(kw->name, "fn", 2) == 0) {
                param_kinds[n_params - 1] = TY_PTR_VOID;
                params[n_params - 1]->type = TYPE_PTR_VOID;
                params[n_params - 1]->is_poly_fn = true;
                params[n_params - 1]->poly_type = NULL;
                continue;
            }
            /* CC4 (cps-transform-plan): a flavored continuation parameter --
             * :cont (cloneable), :escape-cont, or :serial-cont. The flavor
             * selects which runtime (k v) application sugar resumes against. */
            {
                int _cflav = cont_flavor_from_name(kw->name);
                if (_cflav >= 0) {
                    param_kinds[n_params - 1] = TY_CONT;
                    params[n_params - 1]->type =
                        type_cont_flavored(TY_INT, (ContFlavor)_cflav);
                    if (cont_name_is_multishot(kw->name))
                        params[n_params - 1]->type.copy_kind = CK_MULTISHOT;
                    continue;
                }
            }
            /* ptr-generic-parameterised-type: a typed `:ptr<T>` parameter. */
            {
                Type *pt = ptr_type_from_keyword_name(e, kw->name, kw->len,
                                                      p->span, NULL,
                                                      fn_type_params,
                                                      fn_type_param_kinds,
                                                      n_fn_type_params);
                if (pt) {
                    param_kinds[n_params - 1] = TY_PTR_VOID;
                    params[n_params - 1]->type = *pt;
                    continue;
                }
            }
            /* rc-angle-bracket-annotation-becomes-tyvar: a typed reference-family
             * `:rc<T>` / `:weak<T>` / `:ref<T>` / `:lref<T>` parameter.  Resolve
             * it to the real carrier instead of a fresh tyvar named "rc<int>".
             * :ref / :lref carry the same linear-by-default discipline the bare
             * keyword forms apply below. */
            {
                Type *rt = rc_family_type_from_keyword_name(e, kw->name, kw->len,
                                                            p->span, NULL,
                                                            fn_type_params,
                                                            fn_type_param_kinds,
                                                            n_fn_type_params);
                if (rt) {
                    param_kinds[n_params - 1] = rt->kind;
                    params[n_params - 1]->type = *rt;
                    if (rt->kind == TY_REF) {
                        if (!params[n_params - 1]->is_linear
                                && !params[n_params - 1]->is_affine
                                && !params[n_params - 1]->is_relevant) {
                            params[n_params - 1]->is_linear = true;
                            params[n_params - 1]->type.substruct = SK_LINEAR;
                        }
                    } else if (rt->kind == TY_LREF) {
                        params[n_params - 1]->is_linear = true;
                    }
                    continue;
                }
            }
            /* Phase N: use typekind_from_symbol to resolve all known type names
             * (including fixed-width numeric types) before falling through to the
             * type-variable path.  The fast-path checks below are kept for the
             * most common cases; everything else goes through typekind_from_symbol. */
            TypeKind _kw_kind = typekind_from_symbol(kw->name);
            if (_kw_kind != TY_UNKNOWN) {
                param_kinds[n_params - 1] = _kw_kind;
                params[n_params - 1]->type = type_from_kind(_kw_kind);
                params[n_params - 1]->type.copy_kind = typekind_default_copy_kind(_kw_kind);
                /* :ref and :lref params need substructural handling (see below) */
                if (_kw_kind == TY_REF) {
                    params[n_params - 1]->type = type_ref(TY_INT);
                    if (!params[n_params - 1]->is_linear
                            && !params[n_params - 1]->is_affine
                            && !params[n_params - 1]->is_relevant) {
                        params[n_params - 1]->is_linear = true;
                        params[n_params - 1]->type.substruct = SK_LINEAR;
                    }
                } else if (_kw_kind == TY_LREF) {
                    params[n_params - 1]->type = type_lref(TY_INT);
                    params[n_params - 1]->is_linear = true;
                }
            } else if (kw->len == 3 && memcmp(kw->name, "int", 3) == 0) {
                param_kinds[n_params - 1] = TY_INT;
                params[n_params - 1]->type = TYPE_INT;
            } else if (kw->len == 5 && memcmp(kw->name, "float", 5) == 0) {
                param_kinds[n_params - 1] = TY_FLOAT;
                params[n_params - 1]->type = TYPE_FLOAT;
            } else if (kw->len == 4 && memcmp(kw->name, "bool", 4) == 0) {
                param_kinds[n_params - 1] = TY_BOOL;
                params[n_params - 1]->type = TYPE_BOOL;
            } else if (kw->len == 4 && memcmp(kw->name, "cstr", 4) == 0) {
                param_kinds[n_params - 1] = TY_CSTR;
                params[n_params - 1]->type = TYPE_CSTR;
            } else if ((kw->len == 4 && memcmp(kw->name, "void", 4) == 0) || 
                       (kw->len == 3 && memcmp(kw->name, "nil", 3) == 0)) {
                param_kinds[n_params - 1] = TY_NIL;
                params[n_params - 1]->type = TYPE_NIL;
            } else if (kw->len == 3 && memcmp(kw->name, "ptr", 3) == 0) {
                param_kinds[n_params - 1] = TY_PTR_VOID;
                params[n_params - 1]->type = TYPE_PTR_VOID;
            } else if (kw->len == 9 && memcmp(kw->name, "ptr<void>", 9) == 0) {
                param_kinds[n_params - 1] = TY_PTR_VOID;
                params[n_params - 1]->type = TYPE_PTR_VOID;
            } else if (kw->len == 1 && memcmp(kw->name, "!", 1) == 0) {
                param_kinds[n_params - 1] = TY_NEVER;
                params[n_params - 1]->type = TYPE_NEVER;
            } else if (kw->len == 3 && memcmp(kw->name, "ref", 3) == 0) {
                /* Phase 5: :ref keyword — owning heap pointer (inner type unknown, use int) */
                param_kinds[n_params - 1] = TY_REF;
                params[n_params - 1]->type = type_ref(TY_INT);
                /* ST2: :ref params without an explicit discipline annotation are
                 * inferred as SK_LINEAR. */
                if (!params[n_params - 1]->is_linear
                        && !params[n_params - 1]->is_affine
                        && !params[n_params - 1]->is_relevant) {
                    params[n_params - 1]->is_linear = true;
                    params[n_params - 1]->type.substruct = SK_LINEAR;
                }
            } else if (kw->len == 4 && memcmp(kw->name, "lref", 4) == 0) {
                /* LT3: :lref keyword — linear owning pointer (inner type unknown, use int) */
                param_kinds[n_params - 1] = TY_LREF;
                params[n_params - 1]->type = type_lref(TY_INT);
                /* Mark as linear: lref<T> is always exactly-once */
                params[n_params - 1]->is_linear = true;
            } else if (kw->len == 3 && memcmp(kw->name, "set", 3) == 0) {
                /* Phase X3: set type annotation */
                Type set_type = { .kind = TY_SET, .copy_kind = CK_MOVE };
                param_kinds[n_params - 1] = TY_SET;
                params[n_params - 1]->type = set_type;
            } else {
                /* Phase G3: Try constraint env first (type variable resolution) */
                TypeKind ck = gadt_skolem_lookup(&param_constraint_env, kw->name);
                if (ck != TY_UNKNOWN) {
                    /* Resolved via equality constraint */
                    param_kinds[n_params - 1] = ck;
                    params[n_params - 1]->type = type_from_kind(ck);
                    params[n_params - 1]->type.copy_kind = typekind_default_copy_kind(ck);
                } else {
                    /* Phase TA1/TA2: check defalias table.  Copy the full
                     * target type so a composite alias keeps its payload
                     * (element types, struct def, fn signature). */
                    const Symbol *ksym = symtab_intern(e->st, strslice(kw->name, kw->len));
                    const Type *at = NULL;
                    for (uint32_t ai = 0; ai < e->n_type_aliases; ai++) {
                        if (e->type_alias_names[ai] == ksym) { at = e->type_alias_types[ai]; break; }
                    }
                    if (at) {
                        param_kinds[n_params - 1] = at->kind;
                        params[n_params - 1]->type = *at;
                    } else {
                    /* Try to look up as ADT name */
                    AdtDef *param_adt = NULL;
                    for (uint32_t ai = 0; ai < e->n_adt_defs; ai++) {
                        if (strcmp(e->adt_defs[ai]->name, kw->name) == 0) {
                            param_adt = e->adt_defs[ai];
                            break;
                        }
                    }
                    if (param_adt) {
                        param_kinds[n_params - 1] = TY_ADT;
                        params[n_params - 1]->type = type_adt(param_adt);
                    } else {
                        /* structdef-retirement DS-C: the Phase-D struct-name
                         * lookup here (scan struct_defs -> type_struct) is dead --
                         * struct_defs is always empty and a struct name resolves
                         * to the param_adt branch above.
                         * Phase HRT/G2: an unknown keyword is an implicit type
                         * variable.  A parameter annotation like :a where 'a' is
                         * not a known type or ADT is an implicit type variable.
                         * Mark the binding TY_TYVAR so that inside a GADT match
                         * arm, the per-arm skolem env can resolve it to a concrete
                         * type. */
                        param_kinds[n_params - 1] = TY_TYVAR;
                        params[n_params - 1]->type = type_tyvar_named(kw->name);
                        param_poly_types[n_params - 1] = (Type *)arena_alloc(e->arena, sizeof(Type));
                        *param_poly_types[n_params - 1] = params[n_params - 1]->type;
                    }
                    } /* end TA1 else */
                }
            }
            continue;
        }

        /* No hard parameter-count ceiling: per-param scratch is arena-sized to
         * the param vector and the fn type's arg arrays are out of line, so a
         * defn may take arbitrarily many positional parameters (the emitted C
         * function has no limit of its own).  Exceeding the historical 16 is a
         * lint nudge (arbitrary-fn-arity Phase 6), fired once on the 17th. */
        if (n_params == HIGH_ARITY_SOFT_LIMIT) {
            diag_emit_with_code(DIAG_WARNING, p->span, TUR_W0041_HIGH_ARITY,
                      "defn has more than %d positional parameters; prefer a defstruct "
                      "options value or a '& rest :type' variadic (see the Function "
                      "Arity Style Guide)", HIGH_ARITY_SOFT_LIMIT);
        }
        /* For phase 2, default to int -- or `any` in a Saffron file (S2/D3). */
        TypeKind dflt_k = saffron_default_param_kind(p->span);
        param_kinds[n_params] = dflt_k;
        Binding *b = binding_new(e, p->as.sym, type_from_kind(dflt_k),
                                 false, false, p->span);
        b->is_param = true;
        /* perform-does-not-typecheck-its-arguments: nothing has been declared
         * yet -- an annotation, if the author wrote one, arrives as the NEXT
         * form and clears this. */
        b->type_is_carrier_default = true;
        /* LT0: If the previous ^linear annotation applied to this parameter, mark it linear */
        if (next_param_linear) {
            b->is_linear = true;
            b->type.copy_kind = CK_LINEAR;
            next_param_linear = false;
        }
        /* UT0: If the previous ^unique annotation applied to this parameter, mark it unique */
        if (next_param_unique) {
            b->is_unique = true;
            b->type.copy_kind = CK_UNIQUE;
            next_param_unique = false;
        }
        /* UT2: If the previous ^mut annotation applied to this parameter, mark it mutable */
        if (next_param_mut) {
            b->is_mut = true;
            next_param_mut = false;
        }
        /* ST0: If the previous ^affine annotation applied to this parameter, mark it affine */
        if (next_param_affine) {
            b->is_affine = true;
            b->type.substruct = SK_AFFINE;
            next_param_affine = false;
        }
        /* ST0: If the previous ^relevant annotation applied to this parameter, mark it relevant */
        if (next_param_relevant) {
            b->is_relevant = true;
            b->type.substruct = SK_RELEVANT;
            next_param_relevant = false;
        }
        /* LB1: If the previous ^borrow annotation applied to this parameter, mark
         * it as a non-consuming borrow.  The parameter keeps its declared
         * (nominal) handle type; the borrow semantics are enforced at the call
         * site, where a linear/affine argument is read without being consumed. */
        if (next_param_borrow) {
            b->is_borrow = true;
            next_param_borrow = false;
        }
        /* A#1: If the previous ^fat annotation applied to this parameter, mark it
         * as a fat-closure consumer so call sites auto-shim bare fn arguments.
         * closure-representation-unification (Phase 0): a bare ^fat parameter
         * (no fn-type annotation) defaults to TY_INT, which is not directly
         * callable -- (g x) then errors "not a function".  A ^fat parameter
         * holds a fat-closure box, so default it to :ptr<void>, which routes
         * (g x) through the fat-dispatch path.  An explicit type annotation
         * (e.g. ^fat g :(fn [...] ...)) still overrides this default below. */
        if (next_param_fat) {
            b->is_fat = true;
            b->type = TYPE_PTR_VOID;
            param_kinds[n_params] = TY_PTR_VOID;
            next_param_fat = false;
            /* bare-fat-result-monomorphization: stamp the kind a direct call
             * `(g x)` yields.  The canonical body uses the int64 carrier (TY_INT,
             * the prior behavior); a specialized clone uses the incoming
             * closure's result kind so the call types correctly off the tail. */
            b->bare_fat_result_kind = e->bare_fat_spec_active
                                    ? e->bare_fat_spec_kind : TY_INT;
        }
        /* Bare parameter inside a constraint binder's run: it IS the binder's
         * type -- mirror the `: a` keyword branch exactly (a later explicit
         * annotation on this same parameter still overrides, as it does
         * there).  A `^fat` parameter keeps its fat-closure default. */
        if (constraint_binder_run && !b->is_fat) {
            uint8_t cb_idx = 0;
            if (fn_type_param_index(fn_type_params, n_fn_type_params,
                                    constraint_binder_run, &cb_idx)) {
                param_kinds[n_params] = TY_TYVAR;
                b->type = type_tyvar_named(constraint_binder_run->name);
                b->type.hkt_kind = fn_type_param_kinds[cb_idx];
                param_poly_types[n_params] = (Type *)arena_alloc(e->arena, sizeof(Type));
                *param_poly_types[n_params] = b->type;
            }
        }
        if (n_params == 0) {
            params = (Binding **)arena_alloc(e->arena, pcap * sizeof(Binding *));
        }
        params[n_params++] = b;
    }
    
    /* Phase 13: Lifetime annotations parsing deferred - restore original simple parsing */

    /* Parse return type annotation and body */
    /* body_start is the index of the first element after params (could be return type or body) */
    TypeKind return_kind = TY_NIL;
    /* nil-tail-not-checked-against-declared-return: `return_kind` starts at
     * TY_NIL, so "declared nil/void" and "not annotated" are the SAME value
     * here.  The nil-tail check below needs to tell them apart -- a `: void`
     * function must keep accepting a nil tail, an unannotated one infers its
     * return from the body -- so record whether a return type was written down.
     * Mirrors `return_annotated` in elab_fn. */
    bool return_annotated = false;
    AdtDef *return_adt_def = NULL; /* Phase G3: set when return type is an ADT name */
    /* structdef-retirement DS-C: return_struct_def (LT4) removed -- a struct
     * return name is a record ADT (return_adt_def); no StructDef is produced. */
    Type *return_session_type = NULL; /* SS3a/SS7: full session/role return type */
    Type *return_app_type = NULL; /* PTC4: full TY_APP return type for concrete type threading */
    Type *return_exists_type = NULL; /* F1-1: full TY_EXISTS/TY_FORALL return type so callers see the forall_ payload (without it elab_open SEGVs reading body) */
    Type *return_fn_type = NULL; /* Issue 1b: full TY_FN return type so callers see the complete function signature (arity, result_kind) rather than a zeroed TY_FN shell */
    Type *return_tyvar_type = NULL; /* GS4: full TY_TYVAR return type for call-site substitution */
    Type *return_borrow_type = NULL; /* LS2: full borrow return type (&'a T) so lifetime IDs survive */
    /* union-tagged-union-c-emission: full TY_UNION return type, so the
     * return-position widen below knows the MEMBER LIST -- `return_kind` alone
     * is a bare TY_UNION with no members to match the body's type against.
     * Same shape as the captures above, and the reason there was no widen at
     * all here: nothing had ever needed the union's payload in this scope. */
    Type *return_union_type = NULL;
    const Form *return_type_form_kept = NULL; /* SZ8 non-GADT: retain raw return-type Form for size-index inference at call sites */
    /* CT0/RT0: a contract RETURN type -- `: #refine{ r : T | p }`.  The
     * declared return kind is the BASE type T (mirroring how a contract
     * parameter annotation is handled); the predicate becomes a postcondition
     * on the result, checked at runtime and -- under the `refined` experiment
     * -- proved statically when a backend can, in which case no runtime check
     * is emitted at all.  Before this, a contract return type left
     * return_kind == TY_CONTRACT and every caller failed to type the call. */
    const Form *ct_ret_pred = NULL;
    const char *ct_ret_var  = NULL;
    /* RT4: a return refinement the solver PROVED about the body, when none was
     * declared.  Published on the binding so call sites can use it; never
     * turned into a runtime check. */
    const Form *rt_inferred_ret = NULL;
    const char *rt_inferred_var = NULL;
    /* True when a DECLARED return refinement is actually guaranteed of every
     * value this function returns -- either it was proved statically, or the
     * runtime check that enforces it is being emitted.  Neither holds when
     * contracts are stripped (`--no-contracts`, or a release build without
     * --keep-contracts), and assuming it then would let a call site prove a
     * goal from a fact nothing enforces. */
    bool rt_ret_guaranteed = false;
    uint32_t body_start = params_idx + 1;  /* params_idx = params vector */

    /* Phase 19: Parse optional effect-row annotation #{Read Write} or #{e} before return type.
     * Uppercase names are concrete effects; lowercase are row variables.
     * The row is stored as ERK_UNRESOLVED and resolved after PASS_EFFECT_LOWER. */
    EffectRow *declared_effect_row_defn = NULL;
    /* C2 / #reads: captured here, stamped onto the binding alongside the
     * refine_* metadata below.  1-based; 0 = no #reads annotation. */
    uint64_t reads_params_mask_defn = 0;
    bool     reads_declared_defn    = false;
    const Form *reads_annot_defn = NULL;    /* for the W0383 diagnostic's span */
    /* WF1 / #writes: the write frame, captured here and stamped alongside.
     * `declared` is separate from the mask because an empty mask is a real
     * frame ("writes nothing"), not the absence of one -- see expr.h. */
    uint32_t writes_mask_defn     = 0;
    bool     writes_declared_defn = false;
    /* G2: globals named in this defn's `#writes` frame. */
    const Symbol *writes_globals_defn[WF_MAX_FRAME_GLOBALS];
    uint32_t      n_writes_globals_defn = 0;
    const Form *writes_annot_defn = NULL;   /* for the WF2 diagnostic's span */
    if (call->as.list.len >= body_start + 1) {
        Form *maybe_row = call->as.list.items[body_start];
        if (maybe_row->tag == F_MAP) {
            warn_legacy_fx_row(maybe_row);
            uint8_t n_sym = (uint8_t)maybe_row->as.list.len;
            const Symbol **syms = (const Symbol **)arena_alloc(e->arena,
                                    (n_sym ? n_sym : 1) * sizeof(Symbol *));
            uint8_t n_valid = 0;
            for (uint32_t j = 0; j < maybe_row->as.list.len; j++) {
                Form *item = maybe_row->as.list.items[j];
                if (item->tag == F_SYM) {
                    /* `Construct` / `ByVal` are no longer plucked here: they
                     * are `^construct` / `^byval` before the name now, and
                     * left in the row they are TUR-E0026 like any other name
                     * no defeffect declares (with a hint saying where they
                     * went -- effect_check.c). */
                    syms[n_valid++] = item->as.sym;
                }
            }
            declared_effect_row_defn = effect_row_unresolved(e->arena, syms, n_valid,
                                                             maybe_row->span);
            body_start++;  /* skip past the effect row map */
        }
    }
    /* C2 / #reads <sym> and WF1 / #writes <sym>|[<sym>...]: the frame
     * annotations, at the same signature position as #fx{...} and (when it is
     * present) after it.  Each resolves its named symbols to parameter indices
     * and records them on the binding; the refinement encoder consults `#reads`
     * to grant congruence inside a frozen region, and WF2/WF3/WF4 consult
     * `#writes`.
     *
     * The loop accepts the two in EITHER order.  There is no reason to prefer
     * one spelling, and a fixed order would turn a reader's stylistic choice
     * into "unknown function or operator 'writes'" -- the annotation would fall
     * through to the body walk and elaborate as a call. */
    for (int annot_slot = 0; annot_slot < 2; annot_slot++) {
        if (call->as.list.len < body_start + 1) break;
        Form *maybe = call->as.list.items[body_start];
        if (maybe->tag != F_LIST) break;
        if (maybe->fx_prov == (uint8_t)PROV_READS) {
            /* multiple-reads-params: one frame per function, naming any number
             * of parameters.  The two annotation slots exist so `#reads` and
             * `#writes` may appear in either order, NOT so two `#reads` may --
             * before TUR-E0024 the second silently overwrote the first, which
             * handed the refinement solver a trusted claim the author never
             * wrote.  `#writes` rejects its own duplicate the same way. */
            if (reads_declared_defn) {
                diag_emit_with_code(DIAG_ERROR, maybe->span,
                                    TUR_E0024_READS_FRAME_INVALID,
                                    "duplicate `#reads` frame on this function; "
                                    "name every read parameter in one "
                                    "`#reads [...]`");
            }
            reads_declared_defn = true;
            reads_annot_defn    = maybe;
            if (maybe->as.list.len < 2) {
                /* `#reads []` / `#reads` with no names.  Unlike `#writes []`,
                 * which usefully asserts "writes nothing", an empty read frame
                 * says exactly what omitting the annotation says -- and giving
                 * one claim two spellings would give the encoder two ways to
                 * ask the same question. */
                diag_emit_with_code(DIAG_ERROR, maybe->span,
                                    TUR_E0024_READS_FRAME_INVALID,
                                    "empty `#reads` frame; omit the annotation "
                                    "to declare that a measure reads no "
                                    "parameter's mutable state");
            }
            for (uint32_t ai = 1; ai < maybe->as.list.len; ai++) {
                const Form *psym = maybe->as.list.items[ai];
                if (!psym || psym->tag != F_SYM || !psym->as.sym) {
                    diag_emit_with_code(DIAG_ERROR, maybe->span,
                                        TUR_E0024_READS_FRAME_INVALID,
                                        "#reads must name parameters: "
                                        "`#reads <param>` or `#reads [<param> ...]`");
                    continue;
                }
                uint32_t found = 0;
                for (uint32_t pi = 0; pi < n_params; pi++) {
                    if (params[pi] && params[pi]->name == psym->as.sym) {
                        found = pi + 1;
                        break;
                    }
                }
                if (!found) {
                    diag_emit_with_code(DIAG_ERROR, psym->span,
                                        TUR_E0024_READS_FRAME_INVALID,
                                        "#reads names '%s', which is not a "
                                        "parameter of this function",
                                        psym->as.sym->name);
                } else if (found - 1 >= 64) {
                    /* The mask is 64 bits wide.  Reaching this needs a 65+
                     * parameter function, which TUR-W0041 already flags. */
                    diag_emit_with_code(DIAG_ERROR, psym->span,
                                        TUR_E0024_READS_FRAME_INVALID,
                                        "#reads cannot name '%s': only the "
                                        "first 64 parameters can carry a read "
                                        "frame",
                                        psym->as.sym->name);
                } else if (reads_params_mask_defn & (UINT64_C(1) << (found - 1))) {
                    diag_emit_with_code(DIAG_ERROR, psym->span,
                                        TUR_E0024_READS_FRAME_INVALID,
                                        "#reads names '%s' twice",
                                        psym->as.sym->name);
                } else {
                    reads_params_mask_defn |= (UINT64_C(1) << (found - 1));
                }
            }
            body_start++;  /* skip past the #reads annotation */
        } else if (maybe->fx_prov == (uint8_t)PROV_WRITES) {
            if (writes_declared_defn) {
                diag_emit_with_code(DIAG_ERROR, maybe->span,
                                    TUR_E0381_WRITES_FRAME_INVALID,
                                    "duplicate `#writes` frame on this function; "
                                    "name every written parameter in one "
                                    "`#writes [...]`");
            }
            /* Declared even when a name inside fails to resolve: the intent to
             * carry a frame is unambiguous, and treating a typo'd frame as "no
             * frame" would silently downgrade the function to UNKNOWN-writes
             * after already reporting the typo -- two answers to one mistake. */
            writes_declared_defn = true;
            writes_annot_defn    = maybe;
            for (uint32_t ai = 1; ai < maybe->as.list.len; ai++) {
                const Form *psym = maybe->as.list.items[ai];
                if (!psym || psym->tag != F_SYM || !psym->as.sym) {
                    diag_emit_with_code(DIAG_ERROR, maybe->span,
                                        TUR_E0381_WRITES_FRAME_INVALID,
                                        "#writes must name parameters: "
                                        "`#writes <param>` or `#writes [<param> ...]`");
                    continue;
                }
                uint32_t found = 0;
                for (uint32_t pi = 0; pi < n_params; pi++) {
                    if (params[pi] && params[pi]->name == psym->as.sym) {
                        found = pi + 1;
                        break;
                    }
                }
                if (!found) {
                    /* G2 (mutable-globals-plan §4.2): a frame entry that is not
                     * a parameter may name a MUTABLE GLOBAL, so a body that
                     * maintains global state can carry a checked frame instead
                     * of being declined outright (G1).
                     *
                     * An IMMUTABLE global is rejected with its own reason
                     * rather than folded into "not a parameter": naming one in
                     * a write frame is a statement that cannot be true, and
                     * saying so beats a message about parameters. */
                    Binding *gb = scope_lookup(&e->global, psym->as.sym);
                    if (gb && gb->is_global && gb->is_mut) {
                        bool dup = false;
                        for (uint32_t gi = 0; gi < n_writes_globals_defn; gi++)
                            if (writes_globals_defn[gi] == psym->as.sym) { dup = true; break; }
                        if (!dup) {
                            if (n_writes_globals_defn < WF_MAX_FRAME_GLOBALS) {
                                writes_globals_defn[n_writes_globals_defn++] = psym->as.sym;
                            } else {
                                diag_emit_with_code(DIAG_ERROR, psym->span,
                                    TUR_E0381_WRITES_FRAME_INVALID,
                                    "#writes names '%s' -- a write frame covers at "
                                    "most %u globals",
                                    psym->as.sym->name,
                                    (unsigned)WF_MAX_FRAME_GLOBALS);
                            }
                        }
                    } else if (gb && gb->is_global && !gb->is_mut) {
                        diag_emit_with_code(DIAG_ERROR, psym->span,
                                            TUR_E0381_WRITES_FRAME_INVALID,
                                            "#writes names '%s', an immutable global -- "
                                            "nothing can write it; declare it `^mut` or "
                                            "drop it from the frame",
                                            psym->as.sym->name);
                    } else {
                        diag_emit_with_code(DIAG_ERROR, psym->span,
                                            TUR_E0381_WRITES_FRAME_INVALID,
                                            "#writes names '%s', which is not a parameter "
                                            "of this function",
                                            psym->as.sym->name);
                    }
                } else if (found > WF_MAX_FRAME_PARAMS) {
                    /* The mask is 32 bits wide.  Rejected rather than dropped:
                     * a silently ignored frame member is a frame the body can
                     * exceed without anyone hearing about it. */
                    diag_emit_with_code(DIAG_ERROR, psym->span,
                                        TUR_E0381_WRITES_FRAME_INVALID,
                                        "#writes names '%s', parameter %u -- a write "
                                        "frame covers at most the first %u parameters",
                                        psym->as.sym->name, found,
                                        (unsigned)WF_MAX_FRAME_PARAMS);
                } else {
                    writes_mask_defn |= (uint32_t)1u << (found - 1);
                }
            }
            body_start++;  /* skip past the #writes annotation */
        } else {
            break;         /* neither annotation: the body starts here */
        }
    }
    bool fn_declared_unsafe =
        effect_row_contains_symbol(declared_effect_row_defn, e->sym_effect_unsafe);

    /* Phase HKT: Create the inner scope early so that kind-variable bindings
     * (^f → TY_TYVAR/KIND_ARROW) are visible when the return-type annotation
     * is parsed below.  Regular parameter bindings are added to this same
     * scope after the annotation is parsed (see "Push params" below). */
    /* CF7.3: record the scope just before this function's inner scope is pushed,
     * so check_cloneable_capture can stop at the function boundary. */
    struct Scope *saved_fn_entry_outer_scope = e->fn_entry_outer_scope;
    e->fn_entry_outer_scope = e->scope;
    Scope inner;
    scope_init(&inner, e->scope);
    e->scope = &inner;
    /* TY4: depth of the function's parameter/local scope, for the
     * borrow-escape check (see check_no_borrow_escape, called after the body
     * is elaborated).  Bindings at this depth or deeper are function-locals. */
    uint32_t fn_local_depth = 0;
    for (const Scope *s = e->scope; s; s = s->parent) fn_local_depth++;
    for (uint8_t kvi = 0; kvi < n_kind_vars; kvi++) {
        Type kv_type = type_tyvar_named(kind_var_names[kvi]->name);
        kv_type.hkt_kind = KIND_ARROW;
        Binding *kvb = binding_new(e, kind_var_names[kvi], kv_type,
                                   false, true, call->span);
        scope_add(&inner, kvb);
    }

    /* A#1 (return position): an optional ^fat marker may precede the return
     * type, marking a fat-closure RESULT.  A non-capturing fn returned from
     * this function is auto-shimmed into a fat closure (EX_FN_TO_FAT) at the
     * tail, symmetric with the ^fat parameter marker.  Consume the marker here
     * and advance past it so the return-type parser sees the real type form. */
    bool result_fat = false;
    /* self-recursive-fn-returning-call-into-fat-sink: set when the RR1
     * early-forwarding block pre-marks the declared fn result `boxed` so
     * self-calls see the stage-2 truth; read by the stage-2 block so it
     * still runs the tail normalization. */
    bool premarked_self_fat_result = false;
    if (call->as.list.len >= (body_start + 1)) {
        Form *fat_f = call->as.list.items[body_start];
        if (fat_f->tag == F_SYM && fat_f->as.sym == e->sym_caret_fat) {
            result_fat = true;
            body_start++;
        }
    }

    /* Check for : return-type annotation.
     *
     * saffron-dynamic-surface-pass (low): a keyword that is the LAST item of
     * the form is the body, not an annotation -- `(defn k [] :kw)` returns
     * the Sym `:kw`.  A return annotation with nothing after it is always
     * "missing body", so reading the trailing keyword as the body can only
     * turn an error into the program that was written. */
    if (call->as.list.len >= (body_start + 1) &&
        !(call->as.list.len == body_start + 1 &&
          call->as.list.items[body_start]->tag == F_KEYWORD)) {
        /* nil-tail-not-checked-against-declared-return: an annotation is
         * consumed exactly when this block advances body_start, on every one of
         * its exits (three `goto done_return_annotation` paths, the keyword
         * ladder's fallthrough, and the F_TYPE_ANN branch).  Reading the cursor
         * once here beats setting a flag at five sites and missing one. */
        uint32_t ret_annot_bs_before = body_start;
        Form *ret_f = call->as.list.items[body_start];
        /* Spaced `: T` where T is a single symbol or keyword: treat as if
         * fused so the full F_KEYWORD lookup ladder (alias / ADT / struct /
         * sized prim / type-param / opaque-fallback) runs.  Compound
         * `: (-> a b)` etc. still falls through to the F_TYPE_ANN branch. */
        if (ret_f->tag == F_TYPE_ANN && ret_f->as.list.len == 1 &&
            (ret_f->as.list.items[0]->tag == F_KEYWORD ||
             ret_f->as.list.items[0]->tag == F_SYM)) {
            Form *inner = ret_f->as.list.items[0];
            ret_f = inner;
            if (ret_f->tag == F_SYM) {
                Form *kf = (Form *)arena_alloc(e->arena, sizeof(Form));
                *kf = *ret_f;
                kf->tag = F_KEYWORD;
                ret_f = kf;
            }
        }
        if (ret_f->tag == F_KEYWORD) {
            /* : int, : bool, etc. */
            const Symbol *kw = ret_f->as.sym;
            /* ptr-generic-parameterised-type: a typed `:ptr<T>` return type. */
            Type *ptr_ret = ptr_type_from_keyword_name(e, kw->name, kw->len,
                                                       ret_f->span, NULL,
                                                       fn_type_params,
                                                       fn_type_param_kinds,
                                                       n_fn_type_params);
            if (ptr_ret) {
                return_kind = TY_PTR_VOID;
                return_app_type = ptr_ret;  /* threads result_full_type for codegen */
                body_start++;
                goto done_return_annotation;
            }
            /* rc-angle-bracket-annotation-becomes-tyvar: a typed reference-family
             * `: rc<T>` / `: weak<T>` / `: ref<T>` / `: lref<T>` return type. */
            {
                Type *rc_ret = rc_family_type_from_keyword_name(e, kw->name, kw->len,
                                                                ret_f->span, NULL,
                                                                fn_type_params,
                                                                fn_type_param_kinds,
                                                                n_fn_type_params);
                if (rc_ret) {
                    return_kind = rc_ret->kind;
                    return_app_type = rc_ret;  /* threads result_full_type for codegen */
                    body_start++;
                    goto done_return_annotation;
                }
            }
            if (kw->len == 3 && memcmp(kw->name, "int", 3) == 0) {
                return_kind = TY_INT;
            } else if (kw->len == 5 && memcmp(kw->name, "float", 5) == 0) {
                return_kind = TY_FLOAT;
            } else if (kw->len == 4 && memcmp(kw->name, "bool", 4) == 0) {
                return_kind = TY_BOOL;
            } else if (kw->len == 4 && memcmp(kw->name, "void", 4) == 0) {
                return_kind = TY_NIL;
            } else if (kw->len == 4 && memcmp(kw->name, "cstr", 4) == 0) {
                return_kind = TY_CSTR;
            } else if (kw->len == 9 && memcmp(kw->name, "ptr<void>", 9) == 0) {
                return_kind = TY_PTR_VOID;
            } else if (kw->len == 3 && memcmp(kw->name, "nil", 3) == 0) {
                return_kind = TY_NIL;
            } else if (kw->len == 3 && memcmp(kw->name, "ptr", 3) == 0) {
                return_kind = TY_PTR_VOID;
            } else if (kw->len == 2 && memcmp(kw->name, "rc", 2) == 0) {
                return_kind = TY_RC;
            } else if (kw->len == 4 && memcmp(kw->name, "weak", 4) == 0) {
                return_kind = TY_WEAK;
            } else if (kw->len == 4 && memcmp(kw->name, "lref", 4) == 0) {
                /* LT3: :lref return type keyword — lref<T> (linear owning pointer) */
                return_kind = TY_LREF;
            } else if (kw->len == 1 && memcmp(kw->name, "!", 1) == 0) {
                return_kind = TY_NEVER;
            } else if (kw->len == 3 && memcmp(kw->name, "set", 3) == 0) {
                /* Phase X3: set return type */
                return_kind = TY_SET;
            } else {
                /* Phase N6: sized primitive types as return type keywords.
                 * elab_types.c handles these via typekind_from_symbol; mirror
                 * that lookup here so `:int32`, `:uint8`, etc. work in defn. */
                {
                    TypeKind sized_k = typekind_from_symbol(kw->name);
                    if (sized_k != TY_UNKNOWN && sized_k != TY_INT &&
                            sized_k != TY_FLOAT && sized_k != TY_BOOL &&
                            sized_k != TY_CSTR && sized_k != TY_NIL) {
                        return_kind = sized_k;
                        body_start++;
                        goto done_return_annotation;
                    }
                }
                /* Phase G3: Try constraint env (type variable resolution) */
                TypeKind ck = gadt_skolem_lookup(&param_constraint_env, kw->name);
                uint8_t type_param_idx = 0;
                if (fn_type_param_index(fn_type_params, n_fn_type_params, kw, &type_param_idx)) {
                    return_kind = TY_TYVAR;
                    return_tyvar_type = (Type *)arena_alloc(e->arena, sizeof(Type));
                    *return_tyvar_type = type_tyvar_named(kw->name);
                    return_tyvar_type->hkt_kind = fn_type_param_kinds[type_param_idx];
                } else if (ck != TY_UNKNOWN) {
                    return_kind = ck;
                } else {
                    /* Phase TA1/TA2: check defalias table.  A composite target
                     * has to thread its full type through the same capture
                     * variables the `: (type-expr)` path uses below, or the
                     * payload (struct def, fn signature, app args) is lost and
                     * call sites see a bare shell of the right kind. */
                    bool alias_found = false;
                    {
                        const Symbol *ksym = symtab_intern(e->st, strslice(kw->name, kw->len));
                        for (uint32_t ai = 0; ai < e->n_type_aliases; ai++) {
                            if (e->type_alias_names[ai] == ksym) {
                                Type *at = e->type_alias_types[ai];
                                return_kind = at->kind;
                                switch (at->kind) {
                                case TY_ADT:       return_adt_def       = at->as.adt_.def; break;
                                case TY_SESSION:
                                case TY_ROLE:      return_session_type  = at; break;
                                case TY_APP:       return_app_type      = at; break;
                                case TY_EXISTS:
                                case TY_FORALL:    return_exists_type   = at; break;
                                case TY_FN:        return_fn_type       = at; break;
                                case TY_TYVAR:     return_tyvar_type    = at; break;
                                case TY_REF_IMMUT:
                                case TY_REF_MUT:   return_borrow_type   = at; break;
                                default: break;
                                }
                                alias_found = true;
                                break;
                            }
                        }
                    }
                    if (!alias_found) {
                    /* Try to look up as ADT name */
                    for (uint32_t ai = 0; ai < e->n_adt_defs; ai++) {
                        if (strcmp(e->adt_defs[ai]->name, kw->name) == 0) {
                            return_adt_def = e->adt_defs[ai];
                            return_kind = TY_ADT;
                            break;
                        }
                    }
                    /* structdef-retirement DS-C: the LT4 struct-name lookup
                     * (scan struct_defs -> return_struct_def/TY_STRUCT) is dead --
                     * struct_defs is always empty; a struct return name resolves
                     * to return_adt_def above. */
                    if (!return_adt_def) {
                        /* GS4 compatibility: unknown return type keywords remain
                         * named type variables so :a and : a both work for generic
                         * binder forms. */
                        return_kind = TY_TYVAR;
                        return_tyvar_type = (Type *)arena_alloc(e->arena, sizeof(Type));
                        *return_tyvar_type = type_tyvar_named(kw->name);
                    }
                    } /* end !alias_found */
                }
            }
            body_start++;
            done_return_annotation:;
        } else if (ret_f->tag == F_TYPE_ANN) {
            /* Compound return type via `: type-expr` syntax: `: (-> a b)`, `: (vec int)`, etc. */
            if (ret_f->as.list.len > 0) {
                /* SZ8 non-GADT: retain the raw inner form so call sites can
                 * recover a phantom Size index (e.g. `(Dense (Static 2) A)`). */
                return_type_form_kept = ret_f->as.list.items[0];
                Type *ann = fn_type_from_form(e, ret_f->as.list.items[0],
                                              fn_type_params, fn_type_param_kinds, n_fn_type_params);
                /* CT0/RT0: peel a contract return type down to its base type.
                 * Everything downstream (return_kind, the ADT/session/forall
                 * capture below, codegen, call sites) then sees exactly the
                 * type it would have seen without the refinement, and the
                 * predicate rides along as a postcondition on the result.
                 * Before this, a `: #refine{...}` return left return_kind ==
                 * TY_CONTRACT and every call site failed to type the result. */
                if (ann && ann->kind == TY_CONTRACT) {
                    ct_ret_pred = ann->as.contract_.predicate;
                    ct_ret_var  = ann->as.contract_.var_name;
                    ann = ann->as.contract_.base_type;
                }
                if (ann) {
                    return_kind = ann->kind;
                    if (ann->kind == TY_TYVAR) {
                        return_tyvar_type = ann;
                    }
                    /* refined-adt-return-type-miscompiles: an ADT reached
                     * through this path -- the base of a peeled
                     * `#refine{ r : Lst | p }` -- keeps its def, exactly as
                     * the `: Lst` keyword path and the defalias path above
                     * do.  Without it return_kind said TY_ADT with no def, so
                     * the signature fell back to the int64_t carrier while the
                     * body returned the by-value aggregate, and call sites
                     * read the call as a constructor of the ADT. */
                    if (ann->kind == TY_ADT && ann->as.adt_.def) {
                        return_adt_def = ann->as.adt_.def;
                    }
                    /* SS3a: Capture full session return type so callers see the complete
                     * protocol type (e.g. Session[Rec[self, ...]]) rather than a bare
                     * TY_SESSION shell with a NULL protocol pointer. */
                    if (ann->kind == TY_SESSION || ann->kind == TY_ROLE) {
                        return_session_type = ann;
                    }
                    /* PTC4: capture full TY_APP return type so dispatch can extract elem types. */
                    if (ann->kind == TY_APP) {
                        return_app_type = ann;
                    }
                    /* F1-1: capture full TY_EXISTS / TY_FORALL return type so
                     * call sites can patch the resulting expression's type
                     * with the complete forall_ payload (var_names, var_kinds,
                     * body, constraints).  Without this, type_fn() leaves the
                     * union uninitialised and elab_open later dereferences a
                     * garbage `body` pointer. */
                    if (ann->kind == TY_EXISTS || ann->kind == TY_FORALL) {
                        return_exists_type = ann;
                    }
                    /* Issue 1b: capture full TY_FN return type so callers see
                     * the complete function signature (arity, result_kind). */
                    if (ann->kind == TY_FN) {
                        return_fn_type = ann;
                    }
                    /* LS2: capture full borrow return type (&'a T / &mut 'a T)
                     * so its lifetime IDs reach the lifetime pass via
                     * FnDef.return_type. */
                    if (ann->kind == TY_REF_IMMUT || ann->kind == TY_REF_MUT) {
                        return_borrow_type = ann;
                    }
                    /* union-tagged-union-c-emission: capture the member list so
                     * the return-position widen can tag the body's value. */
                    if (ann->kind == TY_UNION) {
                        return_union_type = ann;
                    }
                    /* F2-1: a `:linear` existential cannot escape past the
                     * scope that packs it -- the linear discipline relies on
                     * a single `open` in the same scope freeing the bare
                     * malloc'd record (no rc, no defer chain).  Returning
                     * one from a defn breaks that guarantee silently.
                     * Reject at the annotation parsing point so the
                     * diagnostic fires regardless of how the body returns
                     * the value (direct pack, let-tail, conditional). */
                    if (ann->kind == TY_EXISTS && ann->as.forall_.is_linear) {
                        diag_emit(DIAG_ERROR, ret_f->span,
                                  "defn: a :linear existential cannot escape "
                                  "its packing scope -- it must be opened in "
                                  "the same scope that packed it (no return, "
                                  "no struct/collection storage)");
                        e->scope = inner.parent;
                        scope_free(&inner);
                        return NULL;
                    }
                }
            }
            body_start++;
        }
        return_annotated = (body_start > ret_annot_bs_before);
    }

    /* Ergonomics: a misplaced effect annotation -- `: int #{Unsafe}` instead
     * of `#{Unsafe} : int` -- leaves the `#{...}` set as the first body form.
     * It would otherwise elaborate as an F_MAP and emit the misleading "map
     * literals are parsed but not yet supported" diagnostic, pointing the user
     * at data-literals rather than at the real cause (annotation ordering).
     * The leading effect-row parser above already consumed any correctly
     * placed `#{...}`, so an `#{...}` whose items are all bare symbols sitting
     * in body position is almost certainly a trailing effect annotation. Name
     * the real problem. */
    if (body_start < call->as.list.len) {
        Form *stray = call->as.list.items[body_start];
        if (stray->tag == F_MAP && stray->as.list.len >= 1) {
            bool all_syms = true;
            for (uint32_t j = 0; j < stray->as.list.len; j++) {
                if (stray->as.list.items[j]->tag != F_SYM) { all_syms = false; break; }
            }
            if (all_syms) {
                const Symbol *first = stray->as.list.items[0]->as.sym;
                diag_emit(DIAG_ERROR, stray->span,
                          "defn: effect annotation '#{%s%s}' must precede the "
                          "return type -- write `#{...} : <type>`, not "
                          "`: <type> #{...}`",
                          first->name,
                          stray->as.list.len > 1 ? " ..." : "");
                e->scope = inner.parent;
                scope_free(&inner);
                return NULL;
            }
        }
    }

    /* Phase RT: parse an optional `where` clause attaching typeclass
     * constraints to this defn:
     *
     *   (defn decode [raw :ptr<void>] : (Result a (Vec SchemaError))
     *     where (HasSchema a)
     *     ...)
     *
     * The `where` symbol is followed by one or more (Class tyvar) clauses.
     * Each clause becomes a TypeConstraint appended to constraint_list.  A
     * constraint whose tyvar appears only in the return type (no parameter
     * carries it) is flagged return_resolved so the call elaborator resolves
     * it from the expected result type (RT3).  Absent a `where` clause this
     * block is a no-op -- existing programs are unaffected. */
    if (body_start < call->as.list.len) {
        Form *maybe_where = call->as.list.items[body_start];
        if (maybe_where->tag == F_SYM &&
            maybe_where->as.sym->len == 5 &&
            memcmp(maybe_where->as.sym->name, "where", 5) == 0) {
            body_start++;  /* consume `where` */
            /* Parse one or more (Class tyvar) clauses. */
            while (body_start < call->as.list.len) {
                Form *clause = call->as.list.items[body_start];
                if (clause->tag != F_LIST || clause->as.list.len != 2 ||
                    clause->as.list.items[0]->tag != F_SYM ||
                    clause->as.list.items[1]->tag != F_SYM) {
                    break;  /* not a constraint clause -- start of body */
                }
                const Symbol *cls_sym = clause->as.list.items[0]->as.sym;
                const Symbol *tv_sym  = clause->as.list.items[1]->as.sym;
                TypeClass *tc =
                    typeclass_env_lookup_typeclass(&e->typeclass_env, cls_sym);
                if (!tc) {
                    diag_emit(DIAG_ERROR, clause->span,
                              "defn: typeclass '%s' in where clause is not defined",
                              cls_sym->name);
                    e->scope = inner.parent;
                    scope_free(&inner);
                    return NULL;
                }
                /* return_resolved when no parameter type mentions the tyvar. */
                bool in_param = false;
                for (uint32_t pi = 0; pi < n_params; pi++) {
                    if (type_mentions_named_tyvar(&params[pi]->type, tv_sym->name)) {
                        in_param = true;
                        break;
                    }
                }
                uint8_t new_count = n_constraints + 1;
                TypeConstraint *new_list = (TypeConstraint *)arena_alloc(
                    e->arena, new_count * sizeof(TypeConstraint));
                if (constraint_list && n_constraints > 0) {
                    memcpy(new_list, constraint_list,
                           n_constraints * sizeof(TypeConstraint));
                }
                constraint_list = new_list;
                constraint_list[n_constraints].typeclass       = tc;
                constraint_list[n_constraints].type_arg        = TYPE_UNKNOWN;
                constraint_list[n_constraints].param_idx       = -1;
                constraint_list[n_constraints].tyvar           = tv_sym;
                constraint_list[n_constraints].return_resolved = !in_param;
                n_constraints = new_count;
                body_start++;
            }
        }
    }

    /* CT0/CT1: Parse :pre and :post clauses between return annotation and body.
     * Scan items[body_start..] for F_KEYWORD(:pre) or F_KEYWORD(:post) pairs
     * and advance body_start past them. */
    const Form *ct_pre_form  = NULL;  /* :pre predicate form */
    const Form *ct_post_form = NULL;  /* :post predicate form */
    {
        while (body_start + 1 < call->as.list.len) {
            Form *maybe_kw = call->as.list.items[body_start];
            if (maybe_kw->tag == F_KEYWORD && maybe_kw->as.sym == e->kw_pre) {
                if (body_start + 1 >= call->as.list.len) break;
                ct_pre_form = call->as.list.items[body_start + 1];
                body_start += 2;
            } else if (maybe_kw->tag == F_KEYWORD && maybe_kw->as.sym == e->kw_post) {
                if (body_start + 1 >= call->as.list.len) break;
                ct_post_form = call->as.list.items[body_start + 1];
                body_start += 2;
            } else {
                break;
            }
        }
    }

    /* CT1: Param contract predicates will be injected as pre-checks below. */

    /* LS1: signature parsing is done; the body must not intern signature
     * lifetimes.  Restore the enclosing context (NULL at top level). */
    e->cur_lifetime_ctx = saved_ltctx;

    /* Elaborate body */
    if (call->as.list.len < body_start + 1) {
        diag_emit(DIAG_ERROR, call->span,
                  "defn: missing body");
        e->scope = inner.parent;
        scope_free(&inner);
        return NULL;
    }

    /* Phase HRT5: Early-update a forward-declared binding's arity and poly param
     * types before elaborating the body.  Without this, recursive calls inside
     * the body see the stale arity-1 / no-arg_full_types from pass-1, which
     * causes spurious arity-mismatch errors for functions with poly fn params. */
    if (existing && existing->type.kind == TY_FN && existing->is_global) {
        /* Grow the forward-declared type to the real arity.  Its pass-1
         * out-of-line arg arrays were sized to the pass-1 arity, so allocate
         * fresh arrays of n_params rather than write past them.  The FN_ARG_SET
         * flag writes below then land in the fresh arg_flags. */
        existing->type.as.fn.arity = n_params;
        existing->type.as.fn.arg_kinds = tur_fn_args_alloc(n_params);
        existing->type.as.fn.arg_flags = tur_fn_args_alloc(n_params);
        for (uint32_t _ei = 0; _ei < n_params; _ei++) {
            existing->type.as.fn.arg_kinds[_ei] = param_kinds[_ei];
        }
        /* AR6: propagate variadic flag early so call sites in the same body
         * see is_variadic=true even if body elaboration fails before b->type=fn_type. */
        existing->type.as.fn.is_variadic = is_variadic;
        existing->type.as.fn.rest_kind   = rest_kind;
        existing->type.as.fn.rest_full_type = rest_full_type;
        existing->type.as.fn.rest_borrow = rest_borrow_flag; /* closure-drop-glue (mw-compose-of) */
        bool _any_poly = false;
        for (uint32_t _ei = 0; _ei < n_params; _ei++) {
            if (param_poly_types[_ei]) { _any_poly = true; break; }
        }
        if (_any_poly) {
            Type **_aFT = (Type **)arena_alloc(e->arena, n_params * sizeof(Type *));
            for (uint32_t _ei = 0; _ei < n_params; _ei++) _aFT[_ei] = param_poly_types[_ei];
            existing->type.as.fn.arg_full_types = _aFT;
        }
        /* Propagate the `:fn` poly-closure marker early too, so a *recursive*
         * call inside the body boxes its function/lambda/`:fn` argument into the
         * tur_poly_fn_t carrier (and a pass-through `:fn` value is recognised as
         * already-a-carrier) -- mirroring the same arg_poly_fn assignment applied
         * to the final fn_type below.  Without this a self-call would fall back to
         * the generic int64/void* arg cast and miscompile (aggregate-as-integer). */
        for (uint32_t _ei = 0; _ei < n_params; _ei++) {
            FN_ARG_SET(existing->type.as.fn, _ei, FA_POLY_FN,
                params[_ei]->is_poly_fn &&
                (params[_ei]->poly_type == NULL ||
                 params[_ei]->poly_type->kind == TY_FN));
        }
        /* A#1 (recursion): propagate the ^fat param markers early too, so a
         * *recursive* call inside the body that passes a :ptr<void> fat box
         * into one of this function's own ^fat fn-typed parameters is accepted
         * (elab_call.c gates the :ptr<void> -> ^fat coercion on arg_fat[i]).
         * Without this the self-call sees the stale pass-1 arg_fat=false and
         * rejects the argument with a spurious "(fn []), got ptr<void>" mismatch.
         * Mirrors the final fn_type.arg_fat / result_fat assignment below. */
        for (uint32_t _ei = 0; _ei < n_params; _ei++) {
            FN_ARG_SET(existing->type.as.fn, _ei, FA_FAT, params[_ei]->is_fat);
        }
        existing->type.as.fn.result_fat = result_fat;
        /* LB1 (recursion): propagate the ^borrow param markers early too, so a
         * *recursive* call inside the body that passes one of this function's
         * own linear/affine parameters back into a ^borrow slot is recognised as
         * a non-consuming read and rolls back the consumption the var-use path
         * recorded (elab_call.c gates that rollback on arg_borrow[i]).  Without
         * this, the self-call sees the stale pass-1 arg_borrow=false, leaves the
         * argument marked consumed, and -- when the recursion sits in one arm of
         * a branch -- the merge wrongly reports TUR-E0104 "consumed in one
         * branch but not the other".  The remaining substructural/ownership
         * markers are propagated alongside it so recursive self-calls see the
         * complete discipline signature, mirroring the final fn_type assignment
         * below. */
        for (uint32_t _ei = 0; _ei < n_params; _ei++) {
            FN_ARG_SET(existing->type.as.fn, _ei, FA_BORROW,     params[_ei]->is_borrow);
            FN_ARG_SET(existing->type.as.fn, _ei, FA_LINEAR,     params[_ei]->is_linear);
            FN_ARG_SET(existing->type.as.fn, _ei, FA_AFFINE,     params[_ei]->is_affine);
            FN_ARG_SET(existing->type.as.fn, _ei, FA_RELEVANT,   params[_ei]->is_relevant);
            FN_ARG_SET(existing->type.as.fn, _ei, FA_UNIQUE,     params[_ei]->is_unique);
            FN_ARG_SET(existing->type.as.fn, _ei, FA_UNIQUE_MUT, params[_ei]->is_unique &&
                                                                 params[_ei]->is_mut);
        }
        /* RR1 (recursion): propagate the *result* shape early too.  Without
         * this, a recursive self-call inside the body reads the stale pass-1
         * forward-decl result (the int64 carrier `TY_INT`) and is typed `int`
         * instead of the declared return -- surfacing as a spurious branch
         * mismatch when the self-call sits in one arm of an `if` whose other
         * arm is the real return type (e.g. `then=Box else=int`).  Mirror the
         * result_kind / result_full_type assigned to the final fn_type below.
         *
         * Only the *annotated* return is forwarded here.  An inferred return
         * (return_kind still TY_NIL / TY_TYVAR, resolved from body->type after
         * elaboration) is the genuinely mutually-dependent case and is left to
         * the post-body fn_type construction. */
        /* saffron-lang-plan S4: forward an UNANNOTATED Saffron return as `any`.
         *
         * RR1 below leaves an inferred return to the post-body construction,
         * which is right for Turmeric: the self-call then reads the pass-1
         * forward-decl result, the int64 carrier `TY_INT`, and a static
         * function's inferred return really is usually an int.  In Saffron it
         * is usually not, and the mismatch surfaces one level up as a spurious
         * join failure -- `(if (p h) (Cons h (lfilter p t)) (lfilter p t))`
         * was rejected with "then=Lst else=int", the `int` being the self-call.
         *
         * `any` is the honest forward type for a function whose return is not
         * yet known, and being the top type it joins with whatever the body
         * turns out to produce, so the post-body construction still narrows it.
         *
         * This corrects the S2 note that returns needed no change: inference
         * does propagate for a non-recursive body, and a SELF-CALL is the case
         * it cannot cover, because the type is needed before the body is
         * analysed. */
        if (return_kind == TY_NIL && !return_annotated &&
            lang_span_is_dynamic(call->span)) {
            existing->type.as.fn.result_kind = TY_ANY;
        } else if (return_kind != TY_NIL && return_kind != TY_TYVAR) {
            existing->type.as.fn.result_kind = return_kind;
            Type *rft = NULL;
            if (return_adt_def) {
                rft = (Type *)arena_alloc(e->arena, sizeof(Type));
                *rft = type_adt(return_adt_def);
            }
            /* structdef-retirement DS-C: the return_struct_def branch (build a
             * TY_STRUCT full-type) is dead -- return_struct_def is always NULL. */
            if (return_session_type) rft = return_session_type;
            if (return_tyvar_type)   rft = return_tyvar_type;
            if (return_fn_type && !rft) rft = return_fn_type;
            if (return_app_type)     rft = return_app_type;
            if (return_exists_type)  rft = return_exists_type;
            if (return_borrow_type && !rft) rft = return_borrow_type;
            if (rft) existing->type.as.fn.result_full_type = rft;
            /* self-recursive-fn-returning-call-into-fat-sink: forward the
             * stage-2 fat-result marking early too, for the same reason this
             * whole block exists.  Stage 2 (below, post-body) marks a concrete
             * effect-free fn RESULT `boxed` so callers pass the fat handle
             * through -- but it runs AFTER the body is elaborated, so a
             * self-call inside the body consulted a still-unboxed result type,
             * took the bare-fn auto-shim branch at its ^fat sink, and
             * double-boxed the already-fat handle (slot 0 of the outer box is
             * a shim that then calls the inner box as code -> SIGSEGV).  A
             * one-line forwarder "fixed" it precisely because the forwarder's
             * COMPLETED binding carried the marking.  Every input to the
             * stage-2 decision except the body itself is the declared
             * signature, so decide it here and mark the shared result-type
             * pointer before the body sees it; the flag keeps the post-body
             * tail normalization running (its guard would otherwise read the
             * pre-marked bit as "already done"). */
            if (rft && rft->kind == TY_FN && !rft->as.fn.boxed &&
                !result_fat &&
                rft->as.fn.result_kind != TY_FN &&
                rft->as.fn.result_kind != TY_UNKNOWN &&
                fn_result_type_is_fat_normalized(rft) &&
                !(existing->name && existing->name->name &&
                  strncmp(existing->name->name, "__inst_", 7) == 0)) {
                rft->as.fn.boxed = true;
                premarked_self_fat_result = true;
            }
        }
    }

    /* Push params into the inner scope (created earlier for kind-var bindings). */
    for (uint32_t i = 0; i < n_params; i++) {
        scope_add(&inner, params[i]);
    }

    /* KB-026: gate GS4 implicit type variables.  A *bare* unknown lowercase
     * keyword used as a param or return type (e.g. `:a`) is treated as a named
     * type variable for generic binder forms -- but only when the name is
     * genuinely quantified by the signature: it is declared in an explicit
     * type-param list, is a kind variable, or relates two type positions
     * (appears in >=2 of {param types, return type}).  A name occurring in just
     * its own single annotation is a typo, and recovering it as a type variable
     * silently swallowed two diagnostics:
     *   - a bare `:a` return with no param mentioning `a` should be an
     *     "unsupported return type keyword" error; and
     *   - a lone `[n : nope]` should stay an unresolved type so the
     *     "parameter looks like it was followed by a type annotation" hint can
     *     fire on misuse.
     * This pass undoes those two over-eager recoveries. */
    {
        /* Demote lone bare-keyword params to an unresolved type. */
        for (uint32_t i = 0; i < n_params; i++) {
            if (params[i]->type.kind != TY_TYVAR ||
                !params[i]->type.as.tyvar_.name) continue;
            /* associated-type-unusable-nullary-and-generic (half 2): an
             * associated-type projection `(Inner A)` is determined by its
             * argument -- it is not a typo, however often it occurs. */
            if (params[i]->type.as.tyvar_.assoc_of) continue;
            const char *nm = params[i]->type.as.tyvar_.name;
            bool declared = false;
            for (uint8_t k = 0; k < n_fn_type_params; k++)
                if (fn_type_params[k] && strcmp(fn_type_params[k]->name, nm) == 0) {
                    declared = true; break;
                }
            for (uint8_t k = 0; !declared && k < n_kind_vars; k++)
                if (kind_var_names[k] && strcmp(kind_var_names[k]->name, nm) == 0) {
                    declared = true; break;
                }
            if (declared) continue;
            /* A name that is some ADT/struct's declared type parameter (e.g. `a`
             * from a defgadt) is a genuine type variable -- it is refined per
             * match arm -- even when it appears only once in this signature. */
            if (fn_name_is_adt_tyvar(e, nm)) continue;
            uint8_t occ = 0;
            for (uint32_t j = 0; j < n_params; j++) {
                const Type *jt = param_poly_types[j]
                    ? param_poly_types[j] : &params[j]->type;
                if (fn_type_mentions_named(jt, nm)) occ++;
            }
            if (fn_type_mentions_named(return_tyvar_type, nm) ||
                fn_type_mentions_named(return_app_type, nm) ||
                fn_type_mentions_named(return_fn_type, nm) ||
                fn_type_mentions_named(return_exists_type, nm)) occ++;
            if (occ >= 2) continue;  /* genuine: relates >=2 type positions */
            /* structdef-retirement slice 5 B2 (P1): a single-occurrence bare
             * type-param is an unresolved type variable, not a nominal struct.
             * Emit a named TY_TYVAR carrying the param name rather than the
             * legacy def-less TY_STRUCT placeholder (which codegen lowers to the
             * same int64 carrier).  See ty-struct-null-def-inventory.md P1. */
            Type unresolved = type_tyvar_named(nm);
            unresolved.copy_kind = CK_MOVE;
            unresolved.hkt_kind = KIND_STAR;
            params[i]->type = unresolved;
            param_kinds[i] = TY_TYVAR;
            param_poly_types[i] = NULL;
        }

        /* Reject a bare return type variable with no quantifying binder. */
        if (return_kind == TY_TYVAR && return_tyvar_type &&
            return_tyvar_type->as.tyvar_.name &&
            !return_tyvar_type->as.tyvar_.assoc_of) {
            const char *rn = return_tyvar_type->as.tyvar_.name;
            bool declared = false;
            for (uint8_t k = 0; k < n_fn_type_params; k++)
                if (fn_type_params[k] && strcmp(fn_type_params[k]->name, rn) == 0) {
                    declared = true; break;
                }
            for (uint8_t k = 0; !declared && k < n_kind_vars; k++)
                if (kind_var_names[k] && strcmp(kind_var_names[k]->name, rn) == 0) {
                    declared = true; break;
                }
            uint8_t occ = 1;  /* the return position itself */
            for (uint32_t j = 0; j < n_params; j++) {
                const Type *jt = param_poly_types[j]
                    ? param_poly_types[j] : &params[j]->type;
                if (fn_type_mentions_named(jt, rn)) occ++;
            }
            if (!declared && occ < 2) {
                diag_emit(DIAG_ERROR, name_f->span,
                          "unsupported return type keyword '%s': it is not a "
                          "built-in type and is not bound by any parameter; "
                          "declare it (e.g. `[%s]` type params) or annotate a "
                          "parameter with it to use it as a type variable",
                          rn, rn);
                e->scope = inner.parent;
                scope_free(&inner);
                return NULL;
            }
        }
    }

    /* KB-025: record this function's signature type variables (params + return)
     * so a GADT match arm can distinguish a quantified-`a` result from a skolem
     * that escapes.  Accumulated on top of any enclosing function's set. */
    uint8_t saved_n_sig_tyvars = e->n_sig_tyvars;
    bool saved_in_construct_template = e->in_construct_template;
    e->in_construct_template = defn_has_construct_attr;
    for (uint32_t i = 0; i < n_params; i++) {
        if (param_poly_types[i]) fn_collect_sig_tyvars(e, param_poly_types[i]);
        else                     fn_collect_sig_tyvars(e, &params[i]->type);
    }
    fn_collect_sig_tyvars(e, return_tyvar_type);
    fn_collect_sig_tyvars(e, return_app_type);
    fn_collect_sig_tyvars(e, return_fn_type);
    fn_collect_sig_tyvars(e, return_exists_type);

    Expr *body = e_nil(e, call->span);
    uint32_t n_body = call->as.list.len - body_start;

    /* bare-fat-result-monomorphization: a defn with a bare `^fat` param (no fn
     * annotation) may have a body that only typechecks once the closure's
     * result kind is known (e.g. a :float result consumed off the tail).  Try
     * the canonical (int-carrier) body under a diagnostic-capture frame; if it
     * does not typecheck, defer it -- emit no canonical FnDef and re-elaborate
     * per call site (elab_specialize_bare_fat).  Specialization is the only
     * sound way to type `(g x)` as a non-int register class off the tail. */
    bool has_bare_fat = false;
    for (uint32_t _i = 0; _i < n_params; _i++)
        if (params[_i]->is_fat && params[_i]->type.kind == TY_PTR_VOID) {
            has_bare_fat = true;
            break;
        }
    bool needs_lazy_probe = has_bare_fat && !e->bare_fat_spec_active
                            && !e->bare_fat_force_canonical;
    bool lazy_defer = false;
    uint32_t fsd_mark = e->n_file_scope_defs;
    if (needs_lazy_probe) diag_push_capture();

    e->fn_body_depth++;
    /* Phase R6: Track current function name for linting */
    e->current_fn_name = name_f->as.sym;
    if (fn_declared_unsafe) e->unsafe_depth++;
    /* van-laarhoven-lens-composition: expose this fn's single higher-kinded
     * constraint (`^Functor f`, tyvar `f : * -> *`) to its body so a nested call
     * to another constrained rank-2 fn at the same abstract functor can forward
     * this fn's dict.  Saved/restored around the body (paired with the
     * current_fn_name resets) so a nested defn's own constraint shadows it. */
    TypeClass  *saved_cur_hkt_class = e->cur_hkt_constraint_class;
    const char *saved_cur_hkt_tyvar = e->cur_hkt_constraint_tyvar;
    Binding    *saved_cur_hkt_dict  = e->cur_hkt_dict_binding;
    e->cur_hkt_constraint_class = NULL;
    e->cur_hkt_constraint_tyvar = NULL;
    e->cur_hkt_dict_binding     = NULL;
    /* typeclass-method-resolution-ignores-the-class: publish THIS fn's full
     * constraint vector for the duration of its body, on the same save/restore
     * discipline as the HKT ambient above.  The method-dispatch path reads it
     * to tell "this generic is entitled to call the method" from "this generic
     * just happens to have an instance in scope". */
    /* class-superclasses: a declared `^C W` also declares C's superclasses
     * at W, so dictionary passing and turi's frame dictionaries carry them
     * (see typeclass_constraints_with_supers).  Done here, once every
     * constraint spelling has been collected and before anything reads the
     * list; fd->constraints below stores the expanded list. */
    {
        uint8_t n_expanded = n_constraints;
        constraint_list = typeclass_constraints_with_supers(
            &e->typeclass_env, constraint_list, n_constraints, &n_expanded);
        n_constraints = n_expanded;
    }
    TypeConstraint *saved_cur_fn_constraints   = e->cur_fn_constraints;
    uint8_t         saved_cur_fn_n_constraints = e->cur_fn_n_constraints;
    uint32_t        saved_cur_fn_con_param_mask = e->cur_fn_constraint_param_mask;
    e->cur_fn_constraints   = constraint_list;
    e->cur_fn_n_constraints = n_constraints;
    /* See the field's comment in elab_internal.h: which constraints' tyvars
     * actually reach a parameter, and can therefore be split by
     * monomorphization. */
    {
        uint32_t mask = 0;
        for (uint8_t ci = 0; ci < n_constraints && ci < 32; ci++) {
            const TypeConstraint *con = &constraint_list[ci];
            if (!con->tyvar || !con->tyvar->name) continue;
            for (uint32_t pi = 0; pi < n_params; pi++) {
                if (params[pi] &&
                    type_mentions_named_tyvar(&params[pi]->type, con->tyvar->name)) {
                    mask |= (1u << ci);
                    break;
                }
            }
        }
        e->cur_fn_constraint_param_mask = mask;
    }
    /* constrained-hkt-pure-and-byvalue-carriers (gap 1): the ambient constraint
     * used to be recorded only for a SINGLE-constraint fn, so the moment a body
     * needed two classes on the same type constructor -- `[^Monad m ^Applicative
     * m ...]`, which is what any `bind`-then-`pure` combinator needs -- the
     * ambient went unset and return-directed dispatch had nothing to key on.
     * Accept N constraints as long as they all pin the SAME higher-kinded type
     * variable; the ambient class/dict stay the first one (the dict-clone path
     * below is still single-constraint by construction, see make_dict_clone),
     * but `cur_hkt_constraint_tyvar` now correctly reports "this body abstracts
     * over m". Constraints on DIFFERENT tyvars keep the old behaviour of
     * recording no ambient at all. */
    bool hkt_constraints_share_tyvar = (n_constraints >= 1 && constraint_list);
    for (uint8_t ci = 1; ci < n_constraints && hkt_constraints_share_tyvar; ci++) {
        if (!constraint_list[ci].typeclass || !constraint_list[ci].tyvar ||
            constraint_list[ci].tyvar != constraint_list[0].tyvar)
            hkt_constraints_share_tyvar = false;
    }
    if (hkt_constraints_share_tyvar &&
        constraint_list[0].typeclass && constraint_list[0].tyvar) {
        const Symbol *ctv = constraint_list[0].tyvar;
        for (uint8_t tpi = 0; tpi < n_fn_type_params; tpi++) {
            if (fn_type_params[tpi] == ctv &&
                fn_type_param_kinds[tpi] != KIND_STAR) {
                e->cur_hkt_constraint_class = constraint_list[0].typeclass;
                e->cur_hkt_constraint_tyvar = ctv->name;
                /* Synthetic ambient-dict binding for this fn's `Functor f` dict.
                 * Referencing it forwards the enclosing dict (Gap B2); an adapter
                 * lambda captures it through the ordinary free-var machinery. */
                const Symbol *adsym = symtab_intern(e->st,
                    strslice("__hkt_dict", 10));
                Binding *adb = binding_new(e, adsym, type_from_kind(TY_INT),
                                           false, false, call->span);
                adb->is_ambient_dict = true;
                for (TypeClassInstance *it = e->typeclass_env.instances; it;
                     it = it->next)
                    if (it->typeclass == e->cur_hkt_constraint_class) {
                        adb->ambient_repr = it; break;
                    }
                e->cur_hkt_dict_binding = adb;
                break;
            }
        }
    }
    /* generic-return-type-not-inferred-from-context: push the declared
     * return type onto the expected-type channel for the body's tail.
     * elab_call clears it before sub-arg elaboration, so this only binds
     * the body's outermost tail call -- exactly mirroring how (:: e T)
     * propagates to the call directly under the ascription.  We skip the
     * TY_TYVAR case because a bare type-variable return cannot bind a
     * callee's tyvar (no concrete witness to substitute). */
    /* RT1: everything the body records from here on is a call-site crossing
     * inside THIS function, so its hypotheses are this function's.  Marking the
     * range now and back-filling once the environment exists avoids keeping a
     * mutable "current env" that elab_defn's many error returns would have to
     * unwind correctly. */
    uint32_t rt_cs_start = e->n_refine_call_sites;
    /* loop-invariants-plan: same range bookkeeping for the `while` loops that
     * carry a written `:invariant` (li_analyze_loops, below). */
    uint32_t li_start = e->n_loop_inv_sites;

    Type *prev_body_expected = e->expected_type;
    Type *body_expected = NULL;
    /* structdef-retirement DS-C: the return_struct_def branch (a TY_STRUCT
     * expected body type) is dead -- return_struct_def is always NULL. */
    if (return_adt_def) {
        body_expected = (Type *)arena_alloc(e->arena, sizeof(Type));
        *body_expected = type_adt(return_adt_def);
    } else if (return_app_type) {
        body_expected = return_app_type;
    } else if (return_fn_type) {
        body_expected = return_fn_type;
    } else if (return_exists_type) {
        body_expected = return_exists_type;
    } else if (return_tyvar_type) {
        /* defopaque-struct-payload-fails-through-unsafe-helper: push a BARE
         * type-variable return as the body's expected type too.  It carries
         * no concrete witness here (so the ground-binding branch in elab_call
         * still declines), but a return-only-polymorphic tail call -- e.g.
         * `(__get b idx)` whose own result is a bare tyvar -- needs the
         * enclosing tyvar as a witness so elab can record the callee->caller
         * tyvar mapping.  emit then composes that mapping through the active
         * specialization to mint a per-instantiation clone instead of
         * silently lowering a by-value struct result to the int64 carrier. */
        body_expected = return_tyvar_type;
    } else if (return_kind == TY_ANY) {
        /* any-coercion-not-driven-by-expected-type: a declared `: any` return is
         * a widening TARGET for the body, and nothing was pushing it -- the
         * chain above covers ADT / app / fn / exists / tyvar returns and stops.
         *
         * The visible consequence was at an `if` join: with no expectation to
         * consult, `(defn f [b] : any (if b (Some 7.1) 42))` reported "if
         * branches have mismatched types" even though both arms would widen to
         * the very type the signature declares.  The join's own widening rule
         * only fired when a branch was ALREADY `any`, so there was nothing to
         * face. */
        body_expected = (Type *)arena_alloc(e->arena, sizeof(Type));
        memset(body_expected, 0, sizeof(Type));
        body_expected->kind = TY_ANY;
    }
    /* early-return-bypasses-return-refinement: a `return` in this body gets
     * the checks the whole-body wrap below gives the fall-through value. */
    const RetContract *saved_ret_contract = e->ret_contract;
    e->ret_contract = NULL;
    if ((ct_post_form || ct_ret_pred) && rt_contracts_emitted()) {
        RetContract *rc = (RetContract *)arena_alloc(e->arena, sizeof(RetContract));
        memset(rc, 0, sizeof(*rc));
        rc->post    = ct_post_form;
        rc->ret     = ct_ret_pred;
        rc->ret_var = ct_ret_var;
        e->ret_contract = rc;
    }
    if (body_expected) e->expected_type = body_expected;
    {
        /* Internal defines: splice (define name init) into nested let forms. */
        Form *spliced = splice_internal_defines(e,
                            &call->as.list.items[body_start], n_body, call->span);
        if (spliced) {
            body = elab_form(e, spliced);
            if (!body) {
                if (needs_lazy_probe) { lazy_defer = true; body = e_nil(e, call->span); }
                else {
                if (fn_declared_unsafe) e->unsafe_depth--;
                e->fn_body_depth--;
                e->n_sig_tyvars = saved_n_sig_tyvars;
                e->in_construct_template = saved_in_construct_template;
                e->expected_type = prev_body_expected;
                e->current_fn_name = NULL;
                e->cur_hkt_constraint_class = saved_cur_hkt_class;
                e->cur_hkt_constraint_tyvar = saved_cur_hkt_tyvar;
                e->cur_hkt_dict_binding = saved_cur_hkt_dict;
                e->cur_fn_constraints   = saved_cur_fn_constraints;
                e->cur_fn_n_constraints = saved_cur_fn_n_constraints;
                e->cur_fn_constraint_param_mask = saved_cur_fn_con_param_mask;
                e->fn_entry_outer_scope = saved_fn_entry_outer_scope;
                e->ret_contract = saved_ret_contract;
                e->scope = inner.parent;
                scope_free(&inner);
                return NULL;
                }
            }
        } else if (n_body == 1) {
            body = elab_form(e, call->as.list.items[body_start]);
            if (!body) {
                if (needs_lazy_probe) { lazy_defer = true; body = e_nil(e, call->span); }
                else {
                if (fn_declared_unsafe) e->unsafe_depth--;
                e->fn_body_depth--;
                e->n_sig_tyvars = saved_n_sig_tyvars;
                e->in_construct_template = saved_in_construct_template;
                e->expected_type = prev_body_expected;
                /* Phase R6: Reset current function name */
                e->current_fn_name = NULL;
                e->cur_hkt_constraint_class = saved_cur_hkt_class;
                e->cur_hkt_constraint_tyvar = saved_cur_hkt_tyvar;
                e->cur_hkt_dict_binding = saved_cur_hkt_dict;
                e->cur_fn_constraints   = saved_cur_fn_constraints;
                e->cur_fn_n_constraints = saved_cur_fn_n_constraints;
                e->cur_fn_constraint_param_mask = saved_cur_fn_con_param_mask;
                e->fn_entry_outer_scope = saved_fn_entry_outer_scope;
                e->ret_contract = saved_ret_contract;
                e->scope = inner.parent;
                scope_free(&inner);
                return NULL;
                }
            }
        } else {
            Expr **items = (Expr **)arena_alloc(e->arena, n_body * sizeof(Expr *));
            for (uint32_t i = 0; i < n_body; i++) {
                items[i] = elab_form(e, call->as.list.items[body_start + i]);
                if (!items[i]) {
                    if (needs_lazy_probe) { lazy_defer = true; break; }
                    if (fn_declared_unsafe) e->unsafe_depth--;
                    e->fn_body_depth--;
                    e->n_sig_tyvars = saved_n_sig_tyvars;
                    e->in_construct_template = saved_in_construct_template;
                    e->expected_type = prev_body_expected;
                    /* Phase R6: Reset current function name */
                    e->current_fn_name = NULL;
                    e->cur_hkt_constraint_class = saved_cur_hkt_class;
                    e->cur_hkt_constraint_tyvar = saved_cur_hkt_tyvar;
                    e->cur_hkt_dict_binding = saved_cur_hkt_dict;
                    e->cur_fn_constraints   = saved_cur_fn_constraints;
                    e->cur_fn_n_constraints = saved_cur_fn_n_constraints;
                    e->cur_fn_constraint_param_mask = saved_cur_fn_con_param_mask;
                    e->fn_entry_outer_scope = saved_fn_entry_outer_scope;
                    e->ret_contract = saved_ret_contract;
                    e->scope = inner.parent;
                    scope_free(&inner);
                    return NULL;
                }
            }
            if (lazy_defer) {
                body = e_nil(e, call->span);
            } else {
            /* Phase R6: Warn on discarded result values in function bodies */
            if (g_warn_unused_result) {
                for (uint32_t i = 0; i < n_body - 1; i++) {
                    if (items[i]->type.kind == TY_PTR_VOID) {
                        diag_emit(DIAG_WARNING, items[i]->span,
                                  "discarded result value of type ptr<void>; use ignore! to suppress this warning");
                    }
                }
            }
            body = expr_new(e->arena, EX_DO, items[n_body - 1]->type, call->span);
            body->as.do_.items = items;
            body->as.do_.n = n_body;
            }
        }
    }
    e->ret_contract = saved_ret_contract;
    /* nested-defn-accepted-outer-returns-zero: a body whose LAST form is a
     * definition leaves the function with no tail value, and codegen falls back
     * to `return 0;` -- it runs, exits 0, and returns the wrong answer with no
     * diagnostic anywhere.  In practice the cause is always a missing close
     * paren, which makes the following definitions parse as nested ones.
     *
     * A nested definition is NOT itself the problem: `defn` inside a function
     * body is a real feature (Phase B3 -- it lifts to file scope and is
     * callable by name, see tests/fixtures/nested-fn-basic), and the report
     * that found this proposed rejecting definitions in expression position,
     * which would have deleted that feature.  Only TAIL position is wrong, and
     * only when the function owes its caller a value. */
    if (body && return_kind != TY_NIL && return_kind != TY_NEVER) {
        const Expr *tail = body;
        if (tail->kind == EX_DO && tail->as.do_.n > 0)
            tail = tail->as.do_.items[tail->as.do_.n - 1];
        /* Kinds a definition collapses to.  defmacro/defclass/deftype reduce to
         * EX_NIL_LIT once registered and so are indistinguishable here from a
         * legitimate `nil` tail -- the source-form test below catches those. */
        bool tail_is_def = tail && (tail->kind == EX_FN_DEF   ||
                                    tail->kind == EX_DEF      ||
                                    tail->kind == EX_DEFDATA  ||
                                    tail->kind == EX_DEFECT   ||
                                    tail->kind == EX_EXTERN_C ||
                                    tail->kind == EX_TYPECLASS_DEF ||
                                    tail->kind == EX_INSTANCE_DEF);
        const char *tail_head = NULL;
        if (n_body > 0) {
            const Form *tf = call->as.list.items[body_start + n_body - 1];
            if (tf && tf->tag == F_LIST && tf->as.list.len > 0 &&
                tf->as.list.items[0]->tag == F_SYM) {
                /* EXACT names, never a "def" prefix test: an ordinary call to a
                 * user function whose name merely starts with "def" -- e.g.
                 * `(defined-later 5)` in tests/fixtures/refine-call-site -- is a
                 * perfectly good tail expression. */
                static const char *const def_heads[] = {
                    "def", "defn", "defmacro", "defmacro*", "defstruct",
                    "defopaque", "defdata", "defgadt", "defclass", "definstance",
                    "defkind", "defrec", "deftype", "defalias", "defdynamic",
                    "defeffect", "defprotocol", "extern-c",
                };
                const char *hn = tf->as.list.items[0]->as.sym->name;
                for (size_t k = 0; k < sizeof def_heads / sizeof def_heads[0]; k++) {
                    if (strcmp(hn, def_heads[k]) == 0) {
                        tail_is_def = true;
                        tail_head = hn;
                        break;
                    }
                }
            }
        }
        if (tail_is_def) {
            Span sp = tail ? tail->span : call->span;
            diag_emit_with_code(DIAG_ERROR, sp,
                                TUR_E0713_DEFINITION_IN_TAIL_POSITION,
                                "function '%s' ends its body with a definition%s%s%s, "
                                "so it has no value to return",
                                name_f->as.sym->name,
                                tail_head ? " (" : "", tail_head ? tail_head : "",
                                tail_head ? ")" : "");
            diag_emit(DIAG_NOTE, sp,
                      "check for a missing close paren on the enclosing "
                      "(defn %s ...) -- that is what makes the definitions "
                      "after it parse as nested ones.  A nested definition is "
                      "otherwise fine; it just cannot be the last form",
                      name_f->as.sym->name);
        }
    }
    e->expected_type = prev_body_expected;
    if (fn_declared_unsafe) e->unsafe_depth--;
    e->fn_body_depth--;
    e->n_sig_tyvars = saved_n_sig_tyvars;
    e->in_construct_template = saved_in_construct_template;
    /* Phase R6: Reset current function name */
    e->current_fn_name = NULL;
    e->cur_hkt_constraint_class = saved_cur_hkt_class;
    e->cur_hkt_constraint_tyvar = saved_cur_hkt_tyvar;
    e->cur_hkt_dict_binding = saved_cur_hkt_dict;
    e->cur_fn_constraints   = saved_cur_fn_constraints;
    e->cur_fn_n_constraints = saved_cur_fn_n_constraints;
    e->cur_fn_constraint_param_mask = saved_cur_fn_con_param_mask;
    e->fn_entry_outer_scope = saved_fn_entry_outer_scope;

    /* bare-fat-result-monomorphization: close the canonical-body capture frame.
     * Suppressed errors (cerr > 0) -- or any sub-form that bailed -- mean the
     * body is not int-typeable; defer it.  Roll back any file-scope defs the
     * failed attempt registered (e.g. a nested lambda) so they do not leak into
     * emission and then re-appear from the specialization. */
    if (needs_lazy_probe) {
        uint32_t cerr = diag_pop_capture();
        if (cerr > 0 || lazy_defer) {
            lazy_defer = true;
            body = e_nil(e, call->span);
            e->n_file_scope_defs = fsd_mark;
        } else {
            lazy_defer = false;
        }
    }

    /* saffron-lang-plan S5/D3: an UNANNOTATED Saffron return IS `any`, not
     * "whatever the body turned out to produce".
     *
     * S4 forwarded `any` for the recursive self-call only, and left the final
     * type to the inference below.  The interpreter did not care -- a TuriValue
     * carries its own tag whatever the static type said -- but the two answers
     * are a C type each, and they disagreed: `lmap`'s self-call was elaborated
     * against `any` while the function was finally typed `Lst`, so the emitted
     * self-call spoke a signature the definition did not have.
     *
     * Fixing it at the FORWARD decl instead would be the wrong half: inference
     * cannot see the recursive call's own result, so the two are only reliably
     * equal if the boundary is fixed at `any` -- which is what D3 says the
     * default type is.  Inference still runs everywhere inside the body; what
     * this pins is the SIGNATURE, the one place a caller has to agree.
     *
     * Placed before the widen below on purpose, so the body is boxed by the
     * existing return-position coercion rather than a second one written here.
     *
     * A DIVERGING body (`(defn fail [n] (panic "boom"))`, type `!`) gets the
     * same `any` signature.  It used to keep the inferred `!`, which emits as
     * `void` -- while every caller had already been elaborated against the
     * `any` default, so a caller returning the call's value (`(fn [] : any
     * (fail 5))`) was `return fail(...)` of a void: a cc error, found beside
     * saffron-catch-unwind-around-dyn-call-fn-crashes.  The body needs no box
     * (the widen below skips `!`); only the signature has to agree. */
    if (return_kind == TY_NIL && !return_annotated && body &&
        lang_span_is_dynamic(call->span)) {
        /* saffron-lang-plan open question 3: `main` is the ONE unannotated
         * Saffron function that does not default to `any`.
         *
         * `(defn main [] : int ... 0)` is awkward in a file with no annotations,
         * so a Saffron `main` should be writable without one -- but the default
         * makes it `: any`, and the emitted C then has `return TUR_TAG(...)` in
         * a function declared `int`: "incompatible types when returning type
         * 'tur_tagged_t' but 'int' was expected", a cc error with no Turmeric
         * diagnostic in front of it.  Every Saffron fixture wrote `: int` to
         * step around that, which is exactly the annotation the dialect exists
         * to remove.
         *
         * `:int` rather than the plan's `:nil`, because it keeps `main`'s
         * meaning identical in both dialects -- an explicit exit code still
         * works, and the process-exit convention does not fork per `#lang`.
         * A body that yields something else is then an ordinary return-type
         * error against `int`, pointing at the body rather than at cc.
         *
         * Only the ZERO-arity `main`; an `argc`/`argv`-shaped one keeps whatever
         * the general rule gives it. */
        if (name_f && name_f->tag == F_SYM &&
            strcmp(name_f->as.sym->name, "main") == 0 && n_params == 0) {
            return_kind = TY_INT;
        } else {
            return_kind = TY_ANY;
        }
    }

    /* TY2.2: return-position widening to `any`.  A function declared `: any`
     * whose body yields a narrower type must box the result, otherwise the
     * raw value leaks into a tur_tagged_t slot and breaks C codegen.  Mirror
     * the call-argument widening via the shared coercion helper.  Not an
     * inline-C body: its value type is fiat TY_NIL, trusted to match the
     * declared return (see the conflict check below), so an `: any` one
     * returns a tur_tagged_t itself; boxed as nil, it was pasted into an
     * expression and did not compile. */
    if (return_kind == TY_ANY && body && body->type.kind != TY_ANY &&
        body->type.kind != TY_NEVER && body->kind != EX_INLINE_C) {
        body = elab_coerce_to_any_return(e, body);
    }

    /* saffron-concrete-return-annotation-on-a-dynamic-body-emits-bad-c: the
     * INVERSE of the widen above, and it was missing.
     *
     * `(defn go [x] : int (+ x 41))` in a Saffron file has an `any` body -- `x`
     * is `any`, so `+` is the dynamic operator and yields a box -- against a
     * CONCRETE declared return.  Nothing narrowed it, and
     * `return_position_conflict` lets it through as a carrier bridge, so the
     * emitter wrote `return __tur_dyn_arith(...)` into an `int64_t` slot:
     * "incompatible types when returning type 'tur_tagged_t'".  No `call/cc`
     * needed -- this is the plain shape, and D2's promise that annotations stay
     * legal in Saffron did not hold at the return.
     *
     * The fix is the seam D5 already chose for ARGUMENTS, at the other end of
     * the same function: `elab_any_unbox_to`, the node `(cast x T)` lowers to,
     * so a box holding the wrong type panics with the ordinary `cast: any holds
     * ...` message instead of reinterpreting the payload word.  Erasing instead
     * -- returning the payload unchecked -- would turn a type error into a
     * memory-safety bug, which is the reasoning D5 recorded and this position
     * inherits unchanged.
     *
     * Only for an ANNOTATED, concrete return: an unannotated one is `any` in
     * Saffron and has nothing to check against, and a type variable has no
     * target.  Placed before the conflict check below so that check sees the
     * narrowed type and stays quiet. */
    if (body && return_annotated && body->type.kind == TY_ANY &&
        lang_span_is_dynamic(body->span) &&
        return_kind != TY_ANY && return_kind != TY_NIL &&
        return_kind != TY_UNION && return_kind != TY_NEVER &&
        return_kind != TY_TYVAR && return_kind != TY_UNKNOWN) {
        Type want = return_adt_def ? type_adt(return_adt_def)
                                   : type_from_kind(return_kind);
        /* cps-coloring-walk-has-no-arm-for-union-inject: if the body USES A
         * CONTROL OPERATOR, bind it first and narrow the variable, exactly as
         * the widen does at the other end.  The CPS IR delegates `EX_ANY_CAST`
         * rather than lowering it, so a cast wrapped around a `call/cc` is
         * `BODY-UNSUPPORTED` and the op reaches the direct emitter; a cast of a
         * LET VARIABLE is an ordinary delegatable node. */
        Expr *target = body;
        LetBinding *lb = NULL;
        if (cps_expr_uses_control(body)) {
            lb = (LetBinding *)arena_alloc(e->arena, sizeof(LetBinding));
            target = elab_bind_control_temp(e, body, &lb[0]);
        }
        Expr *unboxed = elab_any_unbox_to(e, target, want, body->span);
        if (unboxed) {
            if (lb) {
                Expr *let = expr_new(e->arena, EX_LET, unboxed->type, body->span);
                let->as.let_.bindings = lb;
                let->as.let_.n = 1;
                let->as.let_.body = unboxed;
                body = let;
            } else {
                body = unboxed;
            }
        }
    }

    /* union-tagged-union-c-emission: the same widening, one type up.  A function
     * declared `: (A | B)` whose body yields a bare member has to tag it; the
     * raw value in a `tur_tagged_t` return slot is "incompatible types when
     * returning", a hard cc error.  Only the ARGUMENT position ever did this
     * widen, which is why every other one was broken. */
    if (return_kind == TY_UNION && return_union_type && body &&
        body->type.kind != TY_UNION && body->type.kind != TY_NEVER) {
        body = elab_coerce_to_union(e, body, return_union_type);
    }

    /* byvalue-recursive-shared-copies-leak: the caller owns the result, so a
     * shared view in result position (a ^borrow, a global, a container
     * element) is cloned rather than handed out as a second owner. */
    body = elab_own_byval_copy(e, body, NULL);

    /* bare-fat-param-non-int-result: a bare `^fat g` call in result position
     * has no recorded result type (typed int64).  When the function declares a
     * non-int register-class return, infer the closure's result type from the
     * declared return and re-stamp the tail call(s) so codegen reads the right
     * register.  Tail-precise; sound under an honest signature (see
     * docs/archive/history/bare-fat-result-type-inference-plan.md). */
    if (kind_is_non_int_register_class(return_kind)) {
        if (retype_bare_fat_tail_calls(body, return_kind) &&
            body->type.kind == TY_INT) {
            body->type = type_from_kind(return_kind);
        }
    }

    /* declared-size-index-never-checked-against-value (A): a declared
     * return-type size index is recovered at every call site
     * (sz_recover_type_form's EX_CALL arm) and trusted there, so a wrong
     * annotation launders a mismatch past TUR-E0260.  Reconcile the
     * annotation with the body's tail expression when both indices fold to
     * constants; an open index on either side stays polymorphic. */
    if (return_type_form_kept && body && !lazy_defer) {
        const Expr *tail = body;
        while (tail && tail->kind == EX_DO && tail->as.do_.n > 0)
            tail = tail->as.do_.items[tail->as.do_.n - 1];
        int64_t claimed, actual;
        if (sz_claim_disagrees(e, return_type_form_kept, tail, &claimed, &actual)) {
            diag_emit_with_code(DIAG_ERROR, tail->span,
                TUR_E0260_SIZED_TYPE_MISMATCH,
                "sized type mismatch (TUR-E0260): '%s' declares return size "
                "%lld but its body has size %lld",
                name_f->as.sym->name, (long long)claimed, (long long)actual);
            e->scope = inner.parent;
            scope_free(&inner);
            return NULL;
        }
        /* gadt-length-index-not-enforced: a declared GADT index against the
         * tail's -- `(defn nil1 [] : (Vec (Succ Zero)) (VNil))` is false. */
        const Form *got = NULL;
        if (gadt_claim_disagrees(e, return_type_form_kept, tail, &got)) {
            Buf cb; buf_init(&cb); form_print(&cb, return_type_form_kept); buf_putc(&cb, '\0');
            Buf gb; buf_init(&gb); form_print(&gb, got); buf_putc(&gb, '\0');
            diag_emit_with_code(DIAG_ERROR, tail->span, TUR_E0001_TYPE_MISMATCH,
                "'%s' declares return type %s but its body has type %s",
                name_f->as.sym->name, cb.data, gb.data);
            buf_free(&cb); buf_free(&gb);
            e->scope = inner.parent;
            scope_free(&inner);
            return NULL;
        }
    }

    /* carrier-aware-return-unification Phase 1: reject a genuine return-position
     * conflict between the declared return and the elaborated body via the
     * shared `return_position_conflict` dispatcher (nominal -> register-class ->
     * pointer-scalar).  An ordinary `defn` is a COMMITTED position: the
     * register-class check runs symmetrically.  Skip the deferred (lazy) probe,
     * whose body is a nil placeholder, and inline-C bodies, whose value type is
     * fiat TY_NIL (trusted to match the declared return).  The int-literal ->
     * float coercion is widened in place first so it stays a coercion. */
    if (!lazy_defer && body && body->kind != EX_INLINE_C) {
        rc_widen_int_literal_to_float_return(return_kind, body);
        rc_narrow_float_literal_tail_to_declared_return(return_kind, body);
        /* carrier-aware-return-unification Phase 2: a defn is a genuinely
         * COMMITTED position only when it does not participate in the int64
         * carrier ABI -- i.e. it is monomorphic (no type params, implicit ones
         * included in n_fn_type_params) and not `#{Unsafe}`.  A generic or
         * `#{Unsafe}` defn is RET_CLASS_CARRIER_FN: it keeps the symmetric
         * register-class (float) check but tolerates the reverse pointer-scalar
         * carrier-handle bridge. */
        ReturnClass ret_cls = (n_fn_type_params == 0 && !fn_declared_unsafe)
                                  ? RET_CLASS_COMMITTED
                                  : RET_CLASS_CARRIER_FN;
        /* nil-tail-not-checked-against-declared-return: check a nil tail only
         * when the return was WRITTEN DOWN (return_kind starts TY_NIL, so
         * unannotated and `: void` are indistinguishable by kind) and the tail is
         * a nil LITERAL.  EX_NIL_LIT is what `defmacro` / `defclass` / `deftype`
         * collapse to once registered, and what a missing close paren leaves
         * behind -- the shapes that made this hole expensive.  A merely
         * nil-TYPED tail (a `println` call) is deliberately not checked; see the
         * predicate's comment for the measurement behind that line. */
        bool check_nil_body = return_annotated && body_tail_is_nil_literal(body);
        /* inferred-return-defaults-inconsistently: an UNANNOTATED return has
         * nothing to conflict with.
         *
         * `return_kind` starts TY_NIL and the inference that adopts the body's
         * type runs further down (the "Infer return type from body" block after
         * the scope pop), so this check used to compare the body against a
         * default the programmer never wrote.  Whether that mattered depended on
         * whether the body's type could ride the int64 carrier: an `int` or
         * `cstr` body bridged and nothing fired, while a `float` body hit the
         * register-class arm and reported `function 'pi' declares return type
         * 'nil'` for a defn that declares nothing.  Three unannotated returns,
         * three behaviours, and the failing one naming a declaration that does
         * not exist.
         *
         * The pair (unannotated, `: nil`/`: void`) is indistinguishable by KIND
         * -- which is what the check_nil_body comment above is about --
         * and `return_annotated` is exactly the bit that separates them.  The
         * `return_kind == TY_NIL` conjunct keeps this narrow: the annotation
         * paths that reach `done_return_annotation` by goto set a real kind
         * without setting `return_annotated`, and those must still be checked. */
        bool return_unannotated = (!return_annotated && return_kind == TY_NIL);
        ReturnConflict rc = return_unannotated
            ? RET_CONFLICT_NONE
            : return_position_conflict(return_adt_def, return_kind, body->type,
                                       ret_cls, check_nil_body);
        /* committed-applied-return-vs-scalar: the dispatcher compares the
         * declared return by kind, so an applied side never met a scalar one.
         * A monomorphic defn has no crossing to ground it. */
        if (rc == RET_CONFLICT_NONE && !return_unannotated &&
            ret_cls == RET_CLASS_COMMITTED && !return_adt_def &&
            return_type_applied_scalar_conflict(
                return_kind == TY_APP ? return_app_type : NULL,
                return_kind, body->type))
            rc = RET_CONFLICT_CARRIER_AGGREGATE;
        if (rc != RET_CONFLICT_NONE) {
            Buf wb; buf_init(&wb);
            if (!return_adt_def && return_kind == TY_APP && return_app_type)
                type_print(&wb, *return_app_type);
            buf_putc(&wb, '\0');
            const char *want = return_adt_def ? return_adt_def->name
                             : (wb.data && wb.data[0]) ? wb.data
                             : typekind_to_string(return_kind);
            Buf gb; buf_init(&gb);
            type_print(&gb, body->type);
            buf_putc(&gb, '\0');
            switch (rc) {
                case RET_CONFLICT_NOMINAL:
                    diag_emit_with_code(DIAG_ERROR, body->span,
                        TUR_E0001_TYPE_MISMATCH,
                        "function '%s' declares return type '%s' but its body "
                        "returns %s",
                        name_f->as.sym->name, want, gb.data);
                    break;
                case RET_CONFLICT_REGISTER_CLASS:
                    diag_emit_with_code(DIAG_ERROR, body->span,
                        TUR_E0707_RETURN_REGISTER_CLASS_MISMATCH,
                        "function '%s' declares return type '%s' but its body "
                        "returns %s -- a float and a non-float live in different "
                        "register classes (xmm vs general-purpose), so this is a "
                        "register-class miscompile, not a tolerable carrier bridge",
                        name_f->as.sym->name, want, gb.data);
                    break;
                case RET_CONFLICT_POINTER_SCALAR:
                    diag_emit_with_code(DIAG_ERROR, body->span,
                        TUR_E0708_RETURN_POINTER_SCALAR_MISMATCH,
                        "function '%s' declares return type 'cstr' but its body "
                        "returns %s -- a bare integer is never a valid string "
                        "pointer, so this is a type-erasure bug, not a tolerable "
                        "carrier bridge",
                        name_f->as.sym->name, gb.data);
                    break;
                case RET_CONFLICT_TYPE_REVERSE:
                    diag_emit_with_code(DIAG_ERROR, body->span,
                        TUR_E0709_RETURN_TYPE_MISMATCH,
                        "function '%s' declares return type '%s' but its body "
                        "returns %s -- a string pointer is never a valid integer, "
                        "and this committed monomorphic function has no carrier to "
                        "bridge it",
                        name_f->as.sym->name, want, gb.data);
                    break;
                case RET_CONFLICT_BOOL_INTEGER:
                    diag_emit_with_code(DIAG_ERROR, body->span,
                        TUR_E0709_RETURN_TYPE_MISMATCH,
                        "function '%s' declares return type '%s' but its body "
                        "returns %s -- bool and the integer family are distinct "
                        "types (boolean constants are true/false, not 0/1), and "
                        "this committed monomorphic function has no carrier to "
                        "bridge them",
                        name_f->as.sym->name, want, gb.data);
                    break;
                case RET_CONFLICT_CARRIER_AGGREGATE:
                    diag_emit_with_code(DIAG_ERROR, body->span,
                        TUR_E0709_RETURN_TYPE_MISMATCH,
                        "function '%s' declares return type '%s' but its body "
                        "returns %s -- an aggregate is a real C type (a struct, or "
                        "a typed pointer to one), not the int64 carrier, so there "
                        "is no representation these two share and nothing to "
                        "bridge them",
                        name_f->as.sym->name, want, gb.data);
                    break;
                case RET_CONFLICT_NIL_BODY:
                    /* The message names the two causes that actually produce
                     * this, because the bare type mismatch is not enough to find
                     * either: a void-returning call in tail position, and a
                     * missing close paren that swallowed the real tail.  The
                     * second is what made this hole expensive -- see TUR-E0713,
                     * the targeted diagnostic for the definition-in-tail case. */
                    diag_emit_with_code(DIAG_ERROR, body->span,
                        TUR_E0709_RETURN_TYPE_MISMATCH,
                        "function '%s' declares return type '%s' but its body "
                        "produces no value (nil) -- a tail that is a void call "
                        "(println, a set!, a while loop) or a definition returns "
                        "nothing, and a missing close paren can swallow the real "
                        "tail into the form above it. Declare ': void' if the "
                        "function is meant to return nothing",
                        name_f->as.sym->name, want);
                    break;
                case RET_CONFLICT_NONE: break;  /* unreachable */
            }
            buf_free(&gb);
            buf_free(&wb);
            e->scope = inner.parent;
            scope_free(&inner);
            return NULL;
        }
    }

    /* TY4: reject returning a borrow of a function-local (would dangle).  The
     * inner scope is still current here; binding depths were stamped at
     * creation, so the check only reads the elaborated body. */
    if (!check_no_borrow_escape(body, fn_local_depth, name_f->as.sym)) {
        e->scope = inner.parent;
        scope_free(&inner);
        return NULL;
    }

    /* CT1: Inject contract checks into body.
     * Determine whether to emit checks based on build mode. */
    {
        bool should_check = true;
#ifdef NDEBUG
        if (!g_keep_contracts_in_release) should_check = false;
#endif
        /* Phase C2: --no-contracts strips refinement-type ({ x : T | pred })
         * contract injection too, so the flag removes *all* contract checks. */
        if (g_no_contracts) should_check = false;

        /* RT1: under the `refined` experiment, build the hypothesis
         * environment (parameter refinements + `:pre`) once and use it twice --
         * to decide this function's return-position obligations, and (via the
         * back-fill below) as the hypotheses for every call-site crossing its
         * body produced.  A proved obligation gets no runtime check emitted; an
         * unproved one keeps exactly the check it would have had with contract
         * types alone.
         *
         * The parameter's own ENTRY check is still always emitted, even when
         * every visible call site is proved: eliding it would need whole-program
         * knowledge of the call graph (including exported and indirect callers),
         * and getting that wrong drops a check that was protecting something.
         * The call-site obligations here are a diagnostic layer on top -- they
         * turn `(safe-div 10 0)` into a compile error -- not a licence to
         * remove the callee's guard. */
        const Form *rt_subject = (call->as.list.len > body_start)
                               ? call->as.list.items[call->as.list.len - 1] : NULL;
        /* Path-condition recovery uses the WHOLE body (rt_whole_body below),
         * not just the return subject -- both sides of the 2026-07-26 merge
         * implemented that fix; main's helper form is kept. */
        bool rt_ret_proven  = false;
        bool rt_post_proven = false;
        bool rt_env_dropped = false;
        RefineEnv *rt_env = rt_build_env(e, params, n_params, ct_param_preds,
                                         ct_param_varnames, ct_param_param_idx,
                                         n_ct_param_preds, ct_pre_form,
                                         rt_whole_body(e, call, body_start),
                                         &rt_env_dropped);
        /* A rebound `^mut` parameter lost its entry facts, and the LAST body
         * form alone would then be judged with no knowledge of what the earlier
         * forms assigned -- a counterexample that is only an artifact of not
         * looking.  The whole body lets the `do` split see the assignment and
         * decline honestly (unknown) instead. */
        if (rt_env_dropped) rt_subject = rt_whole_body(e, call, body_start);
        /* Unconditional: even a function with no refinements of its own is
         * the named caller of the crossings its body produced, and its
         * parameters still need declared sorts in the environment. */
        refine_fill_call_site_env(e, rt_cs_start, rt_env,
                                  name_f->as.sym ? name_f->as.sym->name : NULL,
                                  rt_whole_body(e, call, body_start));
        const char *rt_fn = name_f->as.sym ? name_f->as.sym->name : "?";
        /* loop-invariants-plan: decide this body's loops first -- a proved
         * loop's `p AND (not c)` is what the return obligations below step
         * over it with. */
        li_analyze_loops(e, li_start, params, n_params, ct_param_preds,
                         ct_param_varnames, ct_param_param_idx, n_ct_param_preds,
                         ct_pre_form, rt_whole_body(e, call, body_start), rt_fn);
        /* early-return-bypasses-return-refinement: an early `return` is a
         * second exit.  Its value never reaches `rt_subject` (the LAST body
         * form), and the per-path split reads a `do`'s earlier forms as
         * statements, so a proof of the subject covers the fall-through path
         * only -- `(when (> n 5) (return 0)) n` "proved" `(>= r n)` and lost its
         * check.  Not attempted: the obligation is reported unknown and each
         * `return` carries the runtime check itself (rt_check_returned_value). */
        bool rt_early_ret = li_mentions_return(e, rt_whole_body(e, call, body_start));
        char rt_what[128];
        for (int ct_pass = 0; ct_pass < 2; ct_pass++) {
            const Form *pf = ct_pass == 0 ? ct_ret_pred : ct_post_form;
            if (!pf) continue;
            snprintf(rt_what, sizeof(rt_what), ct_pass == 0 ? "the return value of '%s'"
                                                            : "the postcondition of '%s'", rt_fn);
            bool pv = false;
            if (rt_early_ret)
                diag_emit_with_code(g_strict_refine ? DIAG_ERROR : DIAG_WARNING,
                                    call->span, TUR_W0372_REFINE_UNKNOWN,
                                    "refinement on %s could not be decided statically "
                                    "(the body can leave early through `return`); "
                                    "runtime check kept", rt_what);
            else
                pv = rt_return_obligation_proven(
                    e, pf, ct_pass == 0 ? ct_ret_var : "result", rt_subject,
                    return_kind, rt_env,
                    arena_strdup(e->arena, rt_what, strlen(rt_what)), rt_fn,
                    call->span);
            if (ct_pass == 0) {
                rt_ret_proven = pv;
                rt_ret_guaranteed = rt_ret_proven || should_check;
            } else {
                rt_post_proven = pv;
            }
        }
        /* RT4: with no DECLARED return refinement, try to infer one from
         * the parameter refinements and the body.  Scope is deliberately
         * narrow (the plan's): a single-expression body over a numeric
         * result, at most four refined parameters.  A branching body would
         * need a path-sensitive join at the merge point, which is deferred.
         * Nothing is emitted for an inferred refinement -- it is extra
         * knowledge published to call sites, not a new runtime check. */
        if (!ct_ret_pred && !rt_early_ret && n_body == 1 && n_ct_param_preds > 0 &&
            n_ct_param_preds <= 4 && rt_subject &&
            (return_kind == TY_INT || return_kind == TY_FLOAT ||
             return_kind == TY_FLOAT32 || return_kind == TY_FLOAT64)) {
            rt_inferred_ret = rt4_infer_return(e, rt_subject, return_kind,
                                               rt_env, call->span);
            if (rt_inferred_ret) rt_inferred_var = RT4_RESULT_VAR;
        }

        if (should_check && body) {
            /* Look up tur-contract-check binding */
            Binding *check_fn = scope_lookup(&e->global, e->sym_tur_contract_check);

            /* CT1: parameter contract predicates -- entry checks.  Shared
             * with `fn` so a lambda's contract parameters behave identically. */
            body = rt_inject_param_checks(e, body, check_fn, params, n_params,
                                          ct_param_preds, ct_param_varnames,
                                          ct_param_param_idx, n_ct_param_preds,
                                          call->span);

            /* CT1: :pre — prepend (tur-contract-check pre_pred "Precondition failed") */
            if (ct_pre_form && check_fn) {
                Expr *pred_e = elab_form(e, (Form *)ct_pre_form);
                rt_diag_impure_pred(e, pred_e, call->span);
                pred_e = elab_saffron_truthy(e, pred_e, call->span);   /* D6, see above */
                if (pred_e) {
                    /* Build call: (tur-contract-check pred "Precondition failed") */
                    Expr **check_args = (Expr **)arena_alloc(e->arena, 2 * sizeof(Expr *));
                    check_args[0] = pred_e;
                    /* String arg */
                    Expr *msg_e = expr_new(e->arena, EX_CSTR_LIT, TYPE_CSTR, call->span);
                    msg_e->as.s.p = "Precondition failed";
                    msg_e->as.s.len = 19;
                    check_args[1] = msg_e;
                    Expr *check_call = expr_new(e->arena, EX_CALL, TYPE_NIL, call->span);
                    check_call->as.call_.fn_binding = check_fn;
                    check_call->as.call_.args = check_args;
                    check_call->as.call_.n_args = 2;
                    check_call->as.call_.fn_expr = NULL;
                    check_call->as.call_.dict_arg = NULL;
                    check_call->as.call_.is_poly_call = false;
                    check_call->as.call_.poly_arg_mask = 0;
                    /* Prepend check to body as EX_DO */
                    Expr **do_items = (Expr **)arena_alloc(e->arena, 2 * sizeof(Expr *));
                    do_items[0] = check_call;
                    do_items[1] = body;
                    Expr *new_body = expr_new(e->arena, EX_DO, body->type, call->span);
                    new_body->as.do_.items = do_items;
                    new_body->as.do_.n = 2;
                    body = new_body;
                }
            }

            /* CT1: :post and a contract RETURN type — wrap body as:
             *   (let [<var> body] (tur-contract-check pred "...failed") <var>)
             *
             * `:post` binds the result as `result`; a `: #refine{ r : T | p }`
             * return type binds it as the contract's own variable (`r`).  RT3:
             * an obligation a backend proved emits NO check at all.
             *
             * Both can be present; the return contract wraps outermost. */
            for (int ct_pass = 0; ct_pass < 2 && check_fn; ct_pass++) {
                const Form *pred_form = ct_pass == 0 ? ct_post_form : ct_ret_pred;
                if (!pred_form) continue;
                if (ct_pass == 0 ? rt_post_proven : rt_ret_proven) continue;
                body = rt_wrap_return_check(
                    e, body, check_fn, pred_form,
                    ct_pass == 1 ? ct_ret_var : NULL,
                    ct_pass == 0 ? "Postcondition failed"
                                 : "Return contract violated",
                    call->span);
            }
        }
    }

    /* LT1: At function scope exit, verify all linear params were consumed.
     *
     * Exception: an inline-C body is opaque to the linearity checker -- the C
     * code is trusted to consume (free/close/hand off) the handle, and the
     * exactly-once obligation is enforced at the Turmeric call site instead (a
     * caller's linear binding is marked consumed when passed here). Without
     * this carve-out every resource-handle accessor written in inline-C
     * (file-close, promise-fulfill, ...) would spuriously report TUR-E0100 on
     * its own linear parameter. See linear-ffi fixture for the call-site model. */
    bool lt1_param_fail = false;
    bool body_is_inline_c = (body && body->kind == EX_INLINE_C);
    if (body && !body_is_inline_c) {
        for (uint32_t _li = 0; _li < n_params; _li++) {
            /* LB1: a ^borrow parameter is non-consuming -- the caller retains
             * ownership and the borrow auto-releases at scope exit -- so it
             * carries no consumption obligation. Without this exemption a
             * Turmeric-bodied accessor that merely forwards its borrowed handle
             * to another ^borrow parameter (e.g. a thin delegating wrapper)
             * would spuriously report TUR-E0100, even though inline-C-bodied
             * borrow accessors are already exempt via body_is_inline_c. */
            if (params[_li]->is_borrow) continue;
            /* UT0: a ^unique parameter is affine (at-most-once), not linear
             * (exactly-once) -- dropping it without consuming is legal.  A
             * ref<T> param annotated ^unique can otherwise also pick up the
             * implicit ST2 linear inference; the explicit uniqueness annotation
             * wins, so exempt it from the linear must-consume obligation. */
            if (params[_li]->is_unique) continue;
            if (params[_li]->is_linear && !params[_li]->is_linear_consumed && !params[_li]->is_moved) {
                /* SS0b: Session channels get a distinct error code.
                 * SS1: include the current protocol state in the message. */
                if (params[_li]->type.kind == TY_SESSION) {
                    Type *proto = params[_li]->type.as.session_.fst;
                    diag_emit_with_code(DIAG_ERROR, params[_li]->span,
                                        TUR_E0211_SESSION_DROPPED,
                                        "session channel '%s' dropped before protocol completion "
                                        "(at %s, expected Close)",
                                        params[_li]->name->name,
                                        proto ? type_name(*proto) : "?");
                } else {
                    diag_emit_with_code(DIAG_ERROR, params[_li]->span,
                                        TUR_E0100_LINEAR_DROPPED,
                                        "linear parameter '%s' dropped without being consumed",
                                        params[_li]->name->name);
                }
                lt1_param_fail = true;
            }
        }
    }

    /* ST1: At function scope exit, verify all relevant params were used at least once */
    bool st1_param_fail = false;
    if (body) {
        for (uint32_t _li = 0; _li < n_params; _li++) {
            if (params[_li]->is_relevant && params[_li]->usage_state == USAGE_UNUSED
                    && !params[_li]->is_moved) {
                diag_emit_with_code(DIAG_ERROR, params[_li]->span,
                                    TUR_E0151_RELEVANT_DROPPED,
                                    "relevant parameter '%s' dropped without being used",
                                    params[_li]->name->name);
                st1_param_fail = true;
            }
        }
    }

    /* Pop scope */
    e->scope = inner.parent;
    scope_free(&inner);
    if (lt1_param_fail || st1_param_fail) return NULL;

    /* word-result-fn-into-nil-slot: a WRITTEN `: nil` / `: void` means the
     * function returns nothing, whatever its last form computes -- the body
     * runs for effect.  Letting the body's value win typed
     * `(defn noop [x : int] : void (let [_ x] 0))` as `(fn [int] int)`, so a
     * function the programmer declared void was refused by a `(fn [...] nil)`
     * slot (or, before that slot was checked, called through the wrong C
     * type). */
    if (return_annotated && return_kind == TY_NIL && body &&
        body->type.kind != TY_NIL && body->type.kind != TY_NEVER &&
        body->kind != EX_INLINE_C) {
        Expr **items = (Expr **)arena_alloc(e->arena, 2 * sizeof(Expr *));
        items[0] = body;
        items[1] = e_nil(e, body->span);
        Expr *d = expr_new(e->arena, EX_DO, TYPE_NIL, body->span);
        d->as.do_.items = items;
        d->as.do_.n = 2;
        body = d;
    }

    /* Infer return type from body if not specified or polymorphic (TY_TYVAR).
     * For TY_TYVAR (named type variable like :a), use the body's concrete type
     * for codegen -- the polymorphic annotation is preserved in the declaration
     * but the C function signature uses the concrete type. */
    if ((return_kind == TY_NIL || return_kind == TY_TYVAR) && body->type.kind != TY_NIL
            && body->type.kind != TY_TYVAR) {
        return_kind = body->type.kind;
        /* fmap-over-underdetermined-constructor-is-a-defless-shell: an
         * unannotated defn whose body is an application -- `(Ok 1.5) :
         * (Result float B)`, or a concrete `(Result float int)` -- returns that
         * application.  Recording only the kind left callers the def-less
         * `(? ?)`, which no `(Result A B)` parameter accepts, and spelled the
         * C return as the carrier while a concrete body returned the by-value
         * aggregate (match-join-of-ctor-arm-and-byvalue-arm-reaches-cc).  The
         * lambda twin below has recorded a ground body this way since
         * closure-result-monomorphization.  Not when a signature tyvar is in
         * it: that is the generic case, which keeps its existing path.  An
         * open slot is not one (fn_type_has_named_tyvar). */
        if (body->type.kind == TY_APP && !return_app_type &&
            !fn_type_has_named_tyvar(&body->type)) {
            Type *rat = (Type *)arena_alloc(e->arena, sizeof(Type));
            *rat = body->type;
            return_app_type = rat;
        }
        /* SS7: propagate full TY_ROLE type from body so callers see the correct
         * current_step (the step after the body's last session operation). */
        if (body->type.kind == TY_ROLE && !return_session_type) {
            Type *rft = (Type *)arena_alloc(e->arena, sizeof(Type));
            *rft = body->type;
            return_session_type = rft;
        }
        /* Issue 1b: propagate full TY_FN type from body so callers see the
         * complete function signature (arity, result_kind) rather than a zeroed
         * TY_FN shell from type_from_kind(TY_FN). */
        if (body->type.kind == TY_FN && !return_fn_type) {
            Type *rft = (Type *)arena_alloc(e->arena, sizeof(Type));
            *rft = body->type;
            return_fn_type = rft;
        }
    }

    /* Create function type */
    TypeKind *arg_kinds = (TypeKind *)arena_alloc(e->arena, (n_params ? n_params : 1) * sizeof(TypeKind));
    for (uint32_t i = 0; i < n_params; i++) {
        arg_kinds[i] = param_kinds[i];
    }
    Type fn_type = type_fn(arg_kinds, n_params, return_kind);
    /* AR6: mark variadic functions in their type */
    fn_type.as.fn.is_variadic = is_variadic;
    fn_type.as.fn.rest_kind   = rest_kind;
    fn_type.as.fn.rest_full_type = rest_full_type;
    fn_type.as.fn.rest_borrow = rest_borrow_flag; /* closure-drop-glue (mw-compose-of) */

    /* Phase G3: attach full ADT return type if declared (for proper def propagation) */
    if (return_adt_def) {
        Type *rft = (Type *)arena_alloc(e->arena, sizeof(Type));
        *rft = type_adt(return_adt_def);
        fn_type.as.fn.result_full_type = rft;
    }
    /* structdef-retirement DS-C: the LT4 struct-return full-type attach is dead
     * -- return_struct_def is always NULL; a struct return is a record ADT
     * (handled by the return_adt_def branch above). */
    /* SS3a: attach full session return type if declared.
     * Without this, callers using type_from_kind(TY_SESSION) get a bare shell
     * with NULL protocol pointer, causing silent elaboration failures when the
     * returned channel is used in subsequent session operations. */
    if (return_session_type) {
        fn_type.as.fn.result_full_type = return_session_type;
    }
    if (return_tyvar_type) {
        fn_type.as.fn.result_full_type = return_tyvar_type;
    }
    /* Issue 1b: attach full TY_FN return type so callers see the complete
     * function signature (arity, result_kind) rather than a zeroed TY_FN
     * shell.  Only set if not already filled by a more specific path (e.g.
     * ADT, struct, session, TY_APP, TY_EXISTS). */
    if (return_fn_type && !fn_type.as.fn.result_full_type) {
        fn_type.as.fn.result_full_type = return_fn_type;
    }
    /* PTC4: attach full TY_APP return type so call sites can extract concrete elem types. */
    if (return_app_type) {
        fn_type.as.fn.result_full_type = return_app_type;
    }
    /* F1-1: attach full TY_EXISTS / TY_FORALL return type.  Mirrors the
     * ADT/struct/session/TY_APP paths above; consumed by elab_call.c to
     * patch the call expression's type. */
    if (return_exists_type) {
        fn_type.as.fn.result_full_type = return_exists_type;
    }
    /* LS2: attach the full borrow return type so a call site's result carries
     * the borrow target (and lifetime), letting @ deref recover the pointee
     * type instead of falling back to an unknown/void C type. */
    if (return_borrow_type && !fn_type.as.fn.result_full_type) {
        fn_type.as.fn.result_full_type = return_borrow_type;
    }
    /* LS4: precompute which parameter the borrow return is tied to so call
     * sites can check inter-procedural borrow escape.  A returned &'a T aliases
     * the argument bound to the param sharing lifetime 'a; an elided borrow
     * return follows the elision rules (the receiver-style first borrow param,
     * which also covers the single-borrow-param case). */
    if (return_borrow_type) {
        int8_t tied = -1;
        LifetimeId rlid = (return_borrow_type->n_lifetimes > 0)
                        ? return_borrow_type->lifetimes[0] : LIFETIME_NONE;
        if (rlid != LIFETIME_NONE) {
            for (uint32_t i = 0; i < n_params; i++) {
                if ((params[i]->type.kind == TY_REF_IMMUT
                     || params[i]->type.kind == TY_REF_MUT)
                        && params[i]->type.n_lifetimes > 0
                        && params[i]->type.lifetimes[0] == rlid) {
                    tied = (int8_t)i;
                    break;
                }
            }
        }
        if (tied < 0) {
            for (uint32_t i = 0; i < n_params; i++) {
                if (params[i]->type.kind == TY_REF_IMMUT
                        || params[i]->type.kind == TY_REF_MUT) {
                    tied = (int8_t)i;
                    break;
                }
            }
        }
        fn_type.as.fn.result_borrow_arg = tied;
    }

    /* Phase HRT1: attach full poly types for rank-2 params.
     *
     * Populate arg_full_types for ALL params, not just polymorphic ones, so
     * the saturated positional checker (elab_call.c) can enforce nominal
     * type identity for struct/opaque/ADT args. For a non-poly param we fall
     * back to the param binding's own full type. &params[i]->type is
     * arena-stable (each binding is arena-allocated), so the pointer outlives
     * the call. See docs/archive/history/positional-nominal-type-identity-fix-plan.md. */
    {
        Type **aFT = (Type **)arena_alloc(e->arena, n_params * sizeof(Type *));
        for (uint32_t i = 0; i < n_params; i++)
            aFT[i] = param_poly_types[i] ? param_poly_types[i] : &params[i]->type;
        /* bare-fat-sink-poly-box-slot0-int64-mismatch.md: a bare `^fat` param
         * carries no fn signature (its full type is TY_PTR_VOID), so a caller
         * boxing a tur_poly_fn_t into this slot could not see that the sink
         * invokes it through a non-int64 typed-thunk cast -- slot 0 kept the
         * int64 __tur_poly_to_fat1 shim while the (retyped) invoke cast to
         * e.g. double, a register-class mismatch masked only by xmm0 luck.
         * When the bare-fat result-type inference above retyped this param's
         * tail call to a non-int register class, synthesize `(fn [args] : R)`
         * here so the box site (elab_call.c, sink_fn_type) selects the typed
         * slot-0 shim that matches the invoke. */
        if (kind_is_non_int_register_class(return_kind)) {
            for (uint32_t i = 0; i < n_params; i++) {
                if (!(params[i]->is_fat && params[i]->type.kind == TY_PTR_VOID))
                    continue;
                TypeKind *akinds = NULL;
                int n = bare_fat_tail_call_arg_kinds(e->arena, body, params[i], &akinds);
                if (n < 0) continue;
                Type *ft = (Type *)arena_alloc(e->arena, sizeof(Type));
                *ft = type_fn(akinds, (uint32_t)n, return_kind);
                aFT[i] = ft;
            }
        }
        fn_type.as.fn.arg_full_types = aFT;
    }

    /* LT2: Store arg_linear flags from param bindings into fn_type */
    {
        bool any_linear = false;
        for (uint32_t i = 0; i < n_params; i++) {
            if (params[i]->is_linear) { any_linear = true; break; }
        }
        if (any_linear) {
            for (uint32_t i = 0; i < n_params; i++) {
                FN_ARG_SET(fn_type.as.fn, i, FA_LINEAR, params[i]->is_linear);
            }
        }
    }

    /* UT0: Store arg_unique flags from param bindings into fn_type */
    {
        bool any_unique = false;
        for (uint32_t i = 0; i < n_params; i++) {
            if (params[i]->is_unique) { any_unique = true; break; }
        }
        if (any_unique) {
            for (uint32_t i = 0; i < n_params; i++) {
                FN_ARG_SET(fn_type.as.fn, i, FA_UNIQUE, params[i]->is_unique);
            }
        }
    }
    /* UT2: Store arg_unique_mut flags (^unique ^mut) from param bindings into fn_type */
    {
        bool any_unique_mut = false;
        for (uint32_t i = 0; i < n_params; i++) {
            if (params[i]->is_unique && params[i]->is_mut) { any_unique_mut = true; break; }
        }
        if (any_unique_mut) {
            for (uint32_t i = 0; i < n_params; i++) {
                FN_ARG_SET(fn_type.as.fn, i, FA_UNIQUE_MUT, params[i]->is_unique && params[i]->is_mut);
            }
        }
    }
    /* ST0: Store arg_affine flags from param bindings into fn_type */
    {
        bool any_affine = false;
        for (uint32_t i = 0; i < n_params; i++) {
            if (params[i]->is_affine) { any_affine = true; break; }
        }
        if (any_affine) {
            for (uint32_t i = 0; i < n_params; i++) {
                FN_ARG_SET(fn_type.as.fn, i, FA_AFFINE, params[i]->is_affine);
            }
        }
    }
    /* ST0: Store arg_relevant flags from param bindings into fn_type */
    {
        bool any_relevant = false;
        for (uint32_t i = 0; i < n_params; i++) {
            if (params[i]->is_relevant) { any_relevant = true; break; }
        }
        if (any_relevant) {
            for (uint32_t i = 0; i < n_params; i++) {
                FN_ARG_SET(fn_type.as.fn, i, FA_RELEVANT, params[i]->is_relevant);
            }
        }
    }
    /* LB1: Store arg_borrow flags from param bindings into fn_type so call sites
     * can borrow (read without consuming) a linear/affine argument. */
    {
        bool any_borrow = false;
        for (uint32_t i = 0; i < n_params; i++) {
            if (params[i]->is_borrow) { any_borrow = true; break; }
        }
        if (any_borrow) {
            for (uint32_t i = 0; i < n_params; i++) {
                FN_ARG_SET(fn_type.as.fn, i, FA_BORROW, params[i]->is_borrow);
            }
        }
    }
    /* A#1: Store arg_fat flags from param bindings into fn_type so call sites
     * can auto-shim bare fn arguments into fat closures. */
    {
        bool any_fat = false;
        for (uint32_t i = 0; i < n_params; i++) {
            if (params[i]->is_fat) { any_fat = true; break; }
        }
        if (any_fat) {
            for (uint32_t i = 0; i < n_params; i++) {
                FN_ARG_SET(fn_type.as.fn, i, FA_FAT, params[i]->is_fat);
            }
        }
    }
    /* Phase CCL (fn-first-class-application): propagate the `:fn` poly-closure
     * marker into fn_type so call sites box function/lambda/closure arguments
     * into the tur_poly_fn_t carrier.  Both the bare `:fn` carrier (poly_type ==
     * NULL, int64-default signature) and the F5 typed carrier (poly_type is a
     * concrete TY_FN signature) take this path.  A rank-2 forall param (is_poly_fn
     * with a TY_FORALL poly_type) is handled by the arg_full_types path instead. */
    for (uint32_t i = 0; i < n_params; i++) {
        if (params[i]->is_poly_fn &&
            (params[i]->poly_type == NULL ||
             params[i]->poly_type->kind == TY_FN)) {
            FN_ARG_SET(fn_type.as.fn, i, FA_POLY_FN, true);
        }
    }
    /* A#1 (return position): propagate the ^fat result marker into fn_type so
     * the emitter shims a returned non-capturing fn into a fat closure. */
    fn_type.as.fn.result_fat = result_fat;

    /* sized-types-cross-param-unification: copy the per-parameter raw type
     * annotation Forms into an arena-allocated array on fn_type so call-site
     * elaboration can re-extract size-index templates without walking back
     * through FnDef.  NULL entries (params with no list-form annotation) are
     * fine -- the call-site walk just skips them. */
    if (n_params > 0) {
        const Form **ptf = (const Form **)arena_alloc(
            e->arena, n_params * sizeof(const Form *));
        bool any = false;
        for (uint32_t i = 0; i < n_params; i++) {
            ptf[i] = param_type_forms_buf[i];
            if (ptf[i]) any = true;
        }
        fn_type.as.fn.param_type_forms = any ? ptf : NULL;
    }
    /* SZ8 non-GADT: stash the retained return-type annotation form so call
     * sites can extract a phantom Size index from the declared return type
     * (e.g. `(Dense (Static 2) A)` -> SZT_CONST 2).  Independent of n_params
     * because nullary callers (zero-arg `mk-dense-2 [] : (Dense (Static 2) A)`)
     * are the canonical case for opaque-carrier size literals.  NULL when no
     * compound return annotation was captured -- the call site then falls
     * back to the existing CtorDef-based path for sized GADT constructors. */
    fn_type.as.fn.result_type_form = return_type_form_kept;

    /* Create/update binding for the function.
     * Reuse pass-1 forward bindings in place so subsequent lookups observe
     * updated arity/types from the real definition. */
    Binding *b = NULL;
    /* CC2: Only reuse an existing global binding as a forward
     * declaration/redefinition when it belongs to the *same* module being
     * defined into (or to no module yet -- e.g. a pre-module stdlib forward
     * decl).  In whole-program mode every module elaborates into the single
     * shared global scope, so a same-named *private* defn from a *different*
     * module would otherwise be reused here, collapsing two distinct functions
     * onto one mangled C symbol (and stamping it with whichever module came
     * last).  Creating a fresh binding instead gives each module's private its
     * own distinctly-mangled C name. */
    if (existing && existing->type.kind == TY_FN && existing->is_global &&
        (existing->defining_module_name == NULL ||
         existing->defining_module_name == e->current_module_name)) {
        b = existing;
        b->type = fn_type;
        b->span = name_f->span;
        /* Phase M7: Forward declarations from pass 1 don't know module context.
         * Override defining_module_name with the actual module the defn is in. */
        b->defining_module_name = e->current_module_name;
    } else {
        /* bare-fat-result-monomorphization: a clone takes a distinct mangled
         * name (e.g. run-with__bf_float) so it emits as its own C symbol. */
        const Symbol *bind_name = (e->bare_fat_spec_active && e->bare_fat_spec_name)
                                  ? e->bare_fat_spec_name : name_f->as.sym;
        b = binding_new(e, bind_name, fn_type, false, true, name_f->span);
        scope_add(&e->global, b);
    }
    /* bare-fat-result-monomorphization: hand the freshly-built clone binding
     * back to elab_specialize_bare_fat, which redirects the call site to it. */
    if (e->bare_fat_spec_active) e->bare_fat_spec_result = b;
    /* cps-backend-n6 cross-function resume: retain the source form of a fn with a
     * continuation parameter, so a named-fn resuming receiver used cross-function
     * (the reset is in a caller) can be re-elaborated into a
     * `multishot-effect-cont` specialization (elab_specialize_cont_receiver).
     * Cheap (one pointer) and scoped to cont-param fns; nothing else reads it. */
    if (!b->defn_form) {
        for (uint32_t _pi = 0; _pi < n_params; _pi++) {
            if (params[_pi] && params[_pi]->type.kind == TY_CONT) {
                b->defn_form = call;
                break;
            }
        }
    }
    /* RT1: publish the per-parameter refinement predicates on the binding so
     * call sites can check their arguments against them.  This is the same
     * Binding object pass 1 forward-declared, so a call elaborated BEFORE this
     * defn sees the predicates too -- the resolution pass runs after the whole
     * unit.  Parameter names ride along because a predicate may mention a
     * sibling parameter, which the call site replaces with that slot's
     * argument. */
    if (n_params > 0) {
        const Form **rp = (const Form **)arena_alloc(e->arena, n_params * sizeof(Form *));
        const char **rv = (const char **)arena_alloc(e->arena, n_params * sizeof(char *));
        const char **rn = (const char **)arena_alloc(e->arena, n_params * sizeof(char *));
        for (uint32_t _pi = 0; _pi < n_params; _pi++) {
            rp[_pi] = NULL;
            rv[_pi] = NULL;
            rn[_pi] = (params[_pi] && params[_pi]->name) ? params[_pi]->name->name : NULL;
        }
        for (uint32_t _ci = 0; _ci < n_ct_param_preds; _ci++) {
            uint32_t _pi = ct_param_param_idx[_ci];
            if (_pi >= n_params) continue;
            rp[_pi] = ct_param_preds[_ci];
            rv[_pi] = ct_param_varnames[_ci];
        }
        b->refine_param_preds = rp;
        b->refine_param_vars  = rv;
        b->refine_param_names = rn;
        b->n_refine_params    = n_params;
    }
    /* C2 / #reads: unconditional -- a `#reads` measure need not carry param
     * refinements, so this must not sit inside the block above. */
    b->reads_params_mask = reads_params_mask_defn;
    /* R4 slice 2/3: register the frame for the deferred verification pass.
     * Clones register too -- the pass stamps call-shape EXCEEDED evidence,
     * and the encoder's refusal must see it whichever binding a call
     * resolves to (the same rule as the defn-site stamp below) -- but they
     * are marked so the pass keeps one dump line and one warning per
     * declared frame. */
    if (reads_params_mask_defn != 0)
        rf_note_reads_site(e, b, params, n_params,
                           reads_annot_defn ? reads_annot_defn->span
                                            : name_f->span,
                           e->bare_fat_spec_active);
    /* mutable-globals-plan section 12.3, warning tier (section 13.1): a
     * `#reads` frame is TRUSTED and its one consumer GRANTS congruence, so a
     * frame that omits mutable state the body reads buys proofs it has not
     * earned (the elided caller-side crossing check --
     * tests/fixtures/refine-reads-frame-omits-global pins the cost).  Warn at
     * the definition: the frame is the broken promise, whether or not any
     * call site currently exercises the override.  Gateless -- it reports a
     * fact and changes nothing proved; escalating to refusing the override is
     * a later, gated step.  The bare_fat guard keeps a monomorphization clone
     * from repeating its original's warning. */
    if (reads_params_mask_defn != 0 && body) {
        uint32_t scan_budget = 4096;
        const Binding *gb = reads_scan_mut_global(body, &scan_budget);
        /* R4 slice 1: the same broken promise, reached through a PARAMETER
         * the frame omits rather than a global.  Scanned only when the
         * global scan found nothing so one defn reports one finding. */
        const Binding *pb_omitted = NULL;
        if (!(gb && gb->name)) {
            uint32_t scan_budget2 = 4096;
            pb_omitted = reads_scan_unframed_param(body, params, n_params,
                                                   reads_params_mask_defn,
                                                   &scan_budget2);
        }
        if ((gb && gb->name) || (pb_omitted && pb_omitted->name)) {
            /* R2 (trusted-refinement-claims-plan): the evidence is stamped on
             * EVERY elaboration of the defn -- clones included -- because the
             * encoder's refusal must see it whichever binding a call resolves
             * to.  Only the WARNING is deduped by the bare_fat guard below. */
            b->reads_frame_omits_state = true;
            if (!e->bare_fat_spec_active) {
                /* The frame may name several parameters, so render the whole
                 * list -- quoting just the first would misreport which claim
                 * is broken on a multi-param frame. */
                char frame_txt[256];
                size_t fo = 0;
                frame_txt[0] = '\0';
                for (uint32_t pi = 0; pi < n_params && pi < 64; pi++) {
                    if (!(reads_params_mask_defn & (UINT64_C(1) << pi))) continue;
                    const char *pn = (params[pi] && params[pi]->name)
                                         ? params[pi]->name->name : "?";
                    int wrote = snprintf(frame_txt + fo, sizeof(frame_txt) - fo,
                                         "%s%s", fo ? " " : "", pn);
                    if (wrote < 0 || (size_t)wrote >= sizeof(frame_txt) - fo) break;
                    fo += (size_t)wrote;
                }
                if (gb && gb->name) {
                    diag_emit_with_code(DIAG_WARNING,
                                        reads_annot_defn ? reads_annot_defn->span
                                                         : name_f->span,
                                        TUR_W0383_READS_FRAME_OMITS_MUTABLE,
                                        "`#reads %s` omits mutable state the body reads: "
                                        "the mutable global '%s' can change between two "
                                        "calls this frame lets the solver treat as one "
                                        "value; thread the state through a parameter the "
                                        "frame can name, or make the global immutable",
                                        frame_txt[0] ? frame_txt : "?",
                                        gb->name->name);
                } else {
                    diag_emit_with_code(DIAG_WARNING,
                                        reads_annot_defn ? reads_annot_defn->span
                                                         : name_f->span,
                                        TUR_W0383_READS_FRAME_OMITS_MUTABLE,
                                        "`#reads %s` omits mutable state the body reads: "
                                        "the body reads state reached through parameter "
                                        "'%s', which the frame does not name, and that "
                                        "state can change between two calls this frame "
                                        "lets the solver treat as one value; name it in "
                                        "the frame (`#reads [%s %s]`)",
                                        frame_txt[0] ? frame_txt : "?",
                                        pb_omitted->name->name,
                                        frame_txt[0] ? frame_txt : "?",
                                        pb_omitted->name->name);
                }
            }
        }
    }
    /* reads-frame-verification-ignores-a-callee-write-frame: `#reads p` also
     * promises the body does not change p.  Walked here, at the definition,
     * because the loop-invariant frozen grant that leans hardest on the
     * promise is decided while later defns are still being elaborated -- and
     * it only ever sees a measure whose mask is already stamped, i.e. one
     * that has already passed through here.  Stamped on clones too, for the
     * same whichever-binding-a-call-resolves-to reason as the evidence above. */
    b->reads_write_mask       = 0;
    b->reads_write_unverified = false;
    if (reads_params_mask_defn != 0 && body) {
        uint64_t wmask = 0;
        RwVerdict rv = reads_scan_frame_writes(body, params, n_params,
                                               reads_params_mask_defn, &wmask);
        b->reads_write_unverified = rv != RW_CLEAN;
        if (rv == RW_WRITES && wmask) {
            b->reads_write_mask        = wmask;
            b->reads_frame_omits_state = true;   /* refuses every grant */
            if (!e->bare_fat_spec_active) {
                const char *pn = "?";
                for (uint32_t pi = 0; pi < n_params && pi < 64; pi++)
                    if ((wmask & (UINT64_C(1) << pi)) && params[pi] && params[pi]->name) {
                        pn = params[pi]->name->name;
                        break;
                    }
                diag_emit_with_code(DIAG_WARNING,
                                    reads_annot_defn ? reads_annot_defn->span
                                                     : name_f->span,
                                    TUR_W0383_READS_FRAME_OMITS_MUTABLE,
                                    "`#reads %s` is broken: the body writes the state "
                                    "of '%s' (directly, or through a callee whose "
                                    "`#writes` frame names it), so two calls this "
                                    "frame lets the solver treat as one value can "
                                    "differ; drop `#reads %s`, or stop writing it",
                                    pn, pn, pn);
            }
        }
    }
    /* WF1 / #writes: same placement rationale as `#reads` -- a write frame is
     * independent of param refinements.  `writes_checked` starts false and is
     * raised by the DEFERRED WF2 pass (wf_resolve_write_frames), which is where
     * it has to happen: "every callee's own declared #writes frame stays inside
     * this frame" is a question about functions that may be defined later in
     * the file, and the answer must not depend on definition order. */
    b->writes_param_mask = writes_mask_defn;
    b->writes_declared   = writes_declared_defn;
    b->writes_checked    = false;
    /* G2: copy the declared global frame into the arena beside the mask. */
    b->n_writes_globals_declared = n_writes_globals_defn;
    if (n_writes_globals_defn > 0) {
        b->writes_globals_declared = (const Symbol **)arena_alloc(
            e->arena, n_writes_globals_defn * sizeof(const Symbol *));
        for (uint32_t gi = 0; gi < n_writes_globals_defn; gi++)
            b->writes_globals_declared[gi] = writes_globals_defn[gi];
    }
    /* G1: registered for EVERY defn, not only annotated ones.  The frame walk
     * still only ever checks a frame that was declared (both loops in
     * wf_resolve_write_frames filter on `writes_declared`), but the
     * global-write question has to be answerable for an ARBITRARY callee --
     * including one that carries no frame of its own -- and the body forms are
     * only reachable through this registry. */
    wf_note_frame_site(e, b, params, n_params, call, body_start,
                       writes_declared_defn ? writes_annot_defn : NULL);
    /* reflected-measures RF0: a `^reflect` defn joins the deferred totality
     * pass.  The verdict itself is stamped at the end of this function (the
     * eager attempt, once source_fn_def exists) and by rf_resolve_reflect_sites
     * after the unit. */
    if (is_reflect_attr) {
        experiment_warn_if_used("reflected-measures");
        b->is_reflected = true;
        rf_note_reflect_site(e, b, params, n_params, call, body_start, reflect_annot);
    }
    /* A declared return refinement wins -- but only when something actually
     * enforces it (see rt_ret_guaranteed).  An INFERRED one is always safe to
     * publish: RT4 only records what a backend proved. */
    if (ct_ret_pred && rt_ret_guaranteed) {
        b->refine_return_pred = ct_ret_pred;
        b->refine_return_var  = ct_ret_var;
    } else if (!ct_ret_pred && rt_inferred_ret) {
        b->refine_return_pred = rt_inferred_ret;
        b->refine_return_var  = rt_inferred_var;
    }

    /* Phase R5: Store #[no-unwind] attribute on the binding */
    b->no_unwind = no_unwind;
    /* #[used]: retain with external C linkage under separate compilation */
    b->retain_c_linkage = retain_c_linkage;
    /* closure-drop-glue S1c (non-retaining fn-param inference): a fn-typed / ^fat
     * parameter that the body only CALLS -- never lets escape as a value -- does
     * not retain a capturing-closure argument, so that argument's heap env may be
     * freed at the call scope's exit (like a ^borrow param).  Infer the mask now,
     * from the just-elaborated body; the conservative escape analysis only ever
     * clears the bit (a false "escapes" merely preserves the status-quo leak). */
    elab_infer_nonretain_masks(b, params, n_params, body);
    b->returns_closure_fn_binding = expr_closure_fn_binding(body);

    /* closure-drop-glue S1c (fresh-closure-returning fn): a fn whose body is a
     * bare capturing EX_CLOSURE constructs a FRESH, uniquely-owned heap env on
     * every call and returns it.  When such a call result is consumed by a
     * non-retaining fn-param, the caller can free that env at scope exit (the
     * make-scaler headline shape).  Restricted to scalar (Copy) captures and a
     * scalar closure result so a bare `free(env)` is fully safe: no owning
     * capture to double-free/leak, and the result cannot alias the env. */
    b->returns_fresh_closure = false;
    {
        const Expr *_fc = body;
        while (_fc && _fc->kind == EX_ASCRIBE) _fc = _fc->as.ascribe_.inner;
        /* closure-drop-glue (mw-compose-of): peel a trailing `(let [...] <closure>)`
         * wrapper so a factory like `(defn make-mw [tag] (let [_t tag] (fn [n] ...)))`
         * is still recognised as fresh-closure-returning.  Every call allocates a
         * fresh env (Turmeric lets are not memoised); the let-bound intermediates
         * are the factory's own scope and do not affect the returned env, whose
         * captures/result are still checked scalar-Copy below. */
        while (_fc && (_fc->kind == EX_LET || _fc->kind == EX_ASCRIBE))
            _fc = (_fc->kind == EX_ASCRIBE) ? _fc->as.ascribe_.inner
                                            : _fc->as.let_.body;
        if (_fc && _fc->kind == EX_CLOSURE && _fc->as.closure_.closure &&
            _fc->as.closure_.closure->n_captures > 0 &&
            _fc->as.closure_.closure->fn) {
            struct Closure *_c = _fc->as.closure_.closure;
            bool _ok = fn_result_kind_is_scalar_copy(_c->fn->return_type.kind);
            for (uint8_t _ci = 0; _ok && _ci < _c->n_captures; _ci++)
                if (!_c->captures[_ci] ||
                    !fn_result_kind_is_scalar_copy(_c->captures[_ci]->type.kind))
                    _ok = false;
            b->returns_fresh_closure = _ok;
        }
    }
    /* any-struct-box-leak-per-widen: the `any` twin of the above.  A function
     * whose body's tail is a WIDEN mints a fresh payload box on every call, so
     * the value it hands back is the caller's to drop.  A function that returns
     * an `any` it received (or read out of somewhere) does NOT qualify -- that
     * box is aliased, and dropping it would free memory the other holder still
     * uses.  Peeling let/ascribe mirrors the closure case: Turmeric lets are not
     * memoised, so the wrapper does not make the widen any less fresh. */
    b->returns_fresh_any = false;
    {
        const Expr *_fa = body;
        while (_fa && (_fa->kind == EX_LET || _fa->kind == EX_ASCRIBE))
            _fa = (_fa->kind == EX_ASCRIBE) ? _fa->as.ascribe_.inner
                                            : _fa->as.let_.body;
        if (_fa && _fa->kind == EX_UNION_INJECT && _fa->type.kind == TY_ANY)
            b->returns_fresh_any = true;
        /* dynamic-returned-closure-env-is-never-freed: which of those widen a
         * capturing closure built right here (expr_is_fresh_any_closure, which
         * also requires an env whose release frees nothing a call of the
         * closure could have handed back).  The env is then a fresh heap
         * allocation behind a drop-glue header, owned by nobody but the value
         * this function returns. */
        b->returns_fresh_any_closure = false;
        b->fresh_closure_self_apply_mask = 0;
        if (b->returns_fresh_any && expr_is_fresh_any_closure(_fa)) {
            b->returns_fresh_any_closure = true;
            const Expr *_cv = _fa->as.union_inject_.value;
            while (_cv && (_cv->kind == EX_ASCRIBE || _cv->kind == EX_FN_TO_FAT ||
                           _cv->kind == EX_POLY_TO_FAT || _cv->kind == EX_POLY_WRAP))
                _cv = _cv->kind == EX_ASCRIBE      ? _cv->as.ascribe_.inner
                    : _cv->kind == EX_FN_TO_FAT    ? _cv->as.fn_to_fat_.inner
                    : _cv->kind == EX_POLY_TO_FAT  ? _cv->as.poly_to_fat_.inner
                                                   : _cv->as.poly_wrap_.inner;
            const FnDef *_cfn = _cv->as.closure_.closure->fn;
            /* The self-application mask: which of the closure's own parameters
             * it only invokes, or hands back to itself in the same slot
             * (any_box_binding_escapes_self_apply).  params[0] is the env
             * parameter closure conversion prepends. */
            if (_cfn->params) {
                uint32_t _off = 0;
                const Binding *_p0 = _cfn->n_params ? _cfn->params[0] : NULL;
                if (_p0 && _p0->name && _p0->name->name &&
                    strncmp(_p0->name->name, "__env_p_", 8) == 0)
                    _off = 1;
                for (uint32_t _pj = 0; _pj + _off < _cfn->n_params && _pj < 32; _pj++) {
                    const Binding *_pp = _cfn->params[_pj + _off];
                    if (!_pp || _pp->type.kind != TY_ANY) continue;
                    if (!any_box_binding_escapes_self_apply(_cfn->body, _pp, 1u << _pj))
                        b->fresh_closure_self_apply_mask |= 1u << _pj;
                }
            }
        }
    }
    /* any-struct-box-leak-per-widen: the passthrough twin.  A body whose tail is
     * a bare parameter forwards that argument, so the caller's ownership of the
     * value survives the call -- `(reads (alias tmp))` may drop `tmp`'s box
     * after `reads`, exactly as `(reads tmp)` could.
     *
     * "Returns the parameter" is not enough on its own: a body that ALSO stores
     * it (`(do (set! g v) v)`) hands back a value the callee kept, and dropping
     * it would free memory the global still points at.  The tail use is what
     * makes it a return rather than an escape, so it is excluded from the walk
     * and everything else must come back clean. */
    b->returns_any_param_idx = -1;
    {
        const Expr *_pa = body;
        while (_pa && (_pa->kind == EX_LET || _pa->kind == EX_ASCRIBE))
            _pa = (_pa->kind == EX_ASCRIBE) ? _pa->as.ascribe_.inner
                                            : _pa->as.let_.body;
        if (_pa && _pa->kind == EX_VAR && _pa->as.var.binding &&
            _pa->type.kind == TY_ANY) {
            for (uint32_t _pi = 0; _pi < n_params && _pi < 32; _pi++) {
                if (params[_pi] != _pa->as.var.binding) continue;
                if (!expr_subtree_has_inline_c(body) &&
                    !catch_box_binding_escapes_except(body, params[_pi], _pa))
                    b->returns_any_param_idx = (int)_pi;
                break;
            }
        }
    }
    b->closure_return_dispatches = expr_closure_return_dispatches(body);
    b->closure_return_dispatches_untyped = expr_closure_return_dispatches_untyped(body);
    /* let-bound-sf-loses-outer-arg-type: record whether the return *value* is
     * itself a fat closure box (capturing lambda body) vs a thin fn pointer. */
    b->returns_boxed_closure = (body && body->type.kind == TY_FN &&
                                body->type.as.fn.boxed);
    /* boxed-fn-typed-closure-return: a plain defn whose declared return is a
     * non-boxed function type but whose body yields a *capturing* closure (a
     * fat box) must carry that return value as the void* fat-closure carrier,
     * not the function's result type.  type_c_name(TY_FN non-boxed) lowers to
     * the function's *result* type's C name ("double" for (fn [float] float)),
     * but the body actually returns the heap fat box (void*) -- a hard `cc`
     * error for float/cstr results and an int-only "works by luck" otherwise.
     * Marking the declared result type `boxed` steers the signature, forward
     * declaration, and consumer let-binding all onto the void* carrier in
     * lockstep (type_c_name(TY_FN boxed) == "void *").  Typeclass-method impls
     * (__inst_) keep their dict-driven carrier path (emit_inst_fn_return_carrier),
     * and ^fat returns are already shimmed into a fat box at the call site.
     *
     * Scope this narrowly to the exact mis-lowered shape: type_c_name(TY_FN)
     * only picks the *result* type's C name when the result kind is a concrete
     * leaf (not TY_FN/TY_UNKNOWN).  A curried/nested closure return
     * (result_kind == TY_FN or TY_UNKNOWN) is already carried as int64_t there,
     * so it is correct as-is and must not be re-spelled to void*. */
    if (b->returns_boxed_closure &&
        fn_type.as.fn.result_full_type &&
        fn_type.as.fn.result_full_type->kind == TY_FN &&
        !fn_type.as.fn.result_full_type->as.fn.boxed &&
        fn_type.as.fn.result_full_type->as.fn.result_kind != TY_FN &&
        fn_type.as.fn.result_full_type->as.fn.result_kind != TY_UNKNOWN &&
        !fn_type.as.fn.result_fat &&
        !(b->name && b->name->name &&
          strncmp(b->name->name, "__inst_", 7) == 0)) {
        fn_type.as.fn.result_full_type->as.fn.boxed = true;
    }
    /* Phase M6: Store ^:export-as C name on the binding */
    b->c_export_name = c_export_name;
    /* F4: Store ^deprecated attribute on the binding */
    b->is_deprecated = is_deprecated_attr;
    b->deprecation_message = deprecation_msg;
    /* M2a: propagate ^construct marker */
    b->is_construct_template = defn_has_construct_attr;
    /* M5 residual-straddle: propagate ^byval marker */
    b->prefer_byvalue_spec = defn_has_byval_attr;

    /* captureless-algebra-arm-thin-through-carrier: when a function returns the
     * closure carrier (declared result type is a function type) and its tail
     * leaves mix fat closure boxes (capturing arms) with thin bare fn pointers
     * (captureless arms), box the thin arms so every return leaf is uniformly
     * fat.  The carrier-crossing caller fat-dispatches the result, so a thin
     * arm left bare would be read as a fat box and jumped into -> SIGSEGV.
     * Gated on the mix: an all-thin (uniformly captureless) body stays thin and
     * is correctly thin-callable; only a fat/thin mix is non-uniform. */
    {
        bool result_is_fn =
            (return_fn_type != NULL) ||
            (fn_type.as.fn.result_full_type &&
             fn_type.as.fn.result_full_type->kind == TY_FN) ||
            (fn_type.as.fn.result_kind == TY_FN);
        if (result_is_fn && body) {
            unsigned leaves = fn_tail_fn_leaf_kinds(body, NULL);
            if ((leaves & 1u) && (leaves & 2u)) {
                elab_box_thin_fn_tail_leaves(e, &body, NULL);
            }
        }
    }

    /* fn-value-fat-normalization stage 2: a defn whose declared result is a
     * concrete effect-free fn type returns a fat handle, ALWAYS -- tail
     * leaves are normalized (thin shimmed, carrier param boxed via
     * poly-to-fat, fat passed through) and the result is marked boxed so
     * every consumer rides the existing boxed-result plumbing.  Same
     * narrowed claim as stage 1 (the shared predicate); the nested-result
     * (result_kind == TY_FN/TY_UNKNOWN) and ^fat-result carve-outs mirror
     * the returns_boxed_closure block above. */
    {
        Type *rft = fn_type.as.fn.result_full_type;
        /* `premarked_self_fat_result` means the RR1 recursion block already
         * set `boxed` on this same pointer (see there); the `!boxed` test
         * would read that as "already normalized" and skip the tail pass,
         * leaving thin leaves behind a boxed-claiming type -- the inverse
         * crash.  The other guards were vetted at pre-mark time. */
        if (body && rft && rft->kind == TY_FN &&
            (premarked_self_fat_result || !rft->as.fn.boxed) &&
            !fn_type.as.fn.result_fat &&
            rft->as.fn.result_kind != TY_FN &&
            rft->as.fn.result_kind != TY_UNKNOWN &&
            fn_result_type_is_fat_normalized(rft) &&
            !(b->name && b->name->name &&
              strncmp(b->name->name, "__inst_", 7) == 0)) {
            elab_normalize_fn_tail_leaves(e, &body, rft, NULL);
            rft->as.fn.boxed = true;
        }
    }

    /* Build FnDef */
    FnDef *fd = (FnDef *)arena_alloc(e->arena, sizeof(FnDef));
    memset(fd, 0, sizeof(FnDef));
    fd->binding = b;
    if (b) b->source_fn_def = fd;  /* MB1: Binding -> FnDef link */
    fd->params = params;
    fd->n_params = n_params;
    fd->body = body;
    /* reflected-measures RF0: the eager verdict, now that the purity walk can
     * see this body.  Only a TOTAL verdict is stamped here; rejections wait
     * for the deferred pass so they never depend on definition order. */
    if (b && b->is_reflected) rf_stamp_reflect_site_eager(e, b);
    fd->is_variadic = is_variadic;  /* AR5: propagate variadic flag */
    if (is_variadic) {
        extern bool g_has_variadics;
        g_has_variadics = true;  /* AR8: tell emit_module to include __tur_cons_of */
    }
    /* Mirror emit_fns.c:377's predicate on the binding so call sites can
     * detect an inline-C callee without walking back to the FnDef. */
    b->body_is_inline_c = (body && body->kind == EX_INLINE_C);
    elab_stamp_sum_freshness(b, params, n_params, body);
    fd->closure = NULL;
    fd->inferred_effect_row = NULL;  /* must be NULL; effect_check_pass reads this */
    /* Phase 19: Store declared effect row (ERK_UNRESOLVED until PASS_EFFECT_ROW_INFER). */
    if (declared_effect_row_defn) {
        b->type.as.fn.effect_row = declared_effect_row_defn;
    }
    /* Store param types for codegen.
     * For TY_FN params (annotated with :(fn [...] #{...} :type)), use the full
     * binding type which preserves the result kind and effect row.
     * For TY_STRUCT params, use the binding type which preserves the StructDef
     * pointer (needed so emit.c can emit the struct name in the C signature).
     * For all other kinds, fall back to type_from_kind which is sufficient. */
    fd->param_types = (Type *)arena_alloc(e->arena, n_params * sizeof(Type));
    for (uint32_t i = 0; i < n_params; i++) {
        if (param_kinds[i] == TY_FN && params[i]->type.kind == TY_FN) {
            fd->param_types[i] = params[i]->type;
        } else if (param_kinds[i] == TY_STRUCT && params[i]->type.kind == TY_STRUCT) {
            /* LT4: preserve StructDef so emit.c emits the struct name, not int64_t. */
            fd->param_types[i] = params[i]->type;
        } else if ((param_kinds[i] == TY_REF_IMMUT || param_kinds[i] == TY_REF_MUT)
                   && (params[i]->type.kind == TY_REF_IMMUT
                       || params[i]->type.kind == TY_REF_MUT)) {
            /* LS1: preserve borrow target + lifetime IDs so the lifetime pass and
             * borrow checker see the programmer's &'a T annotation, not a
             * lifetime-stripped type_from_kind() shell. */
            fd->param_types[i] = params[i]->type;
        } else if (param_kinds[i] == TY_PTR_VOID
                   && params[i]->type.kind == TY_PTR_VOID
                   && params[i]->type.as.ptr.inner) {
            /* ptr-generic-parameterised-type: preserve the pointee type so
             * emit.c lowers a `:ptr<T>` parameter to `T *`, not `void *`. */
            fd->param_types[i] = params[i]->type;
        } else if (param_kinds[i] == TY_INT && params[i]->type.kind == TY_ADT
                   && params[i]->type.as.adt_.def) {
            /* CONV-S1/B2: an untyped param (defaulted to TY_INT) that was used as
             * a match scrutinee had its binding refined to the inferred ADT during
             * body elaboration (elab_match).  The body runs before this realiser,
             * so preserve that ADT type instead of collapsing back to int64 -- the
             * parameter and the by-value match body then agree once the gate flips.
             * type_c_name(TY_ADT) is int64_t while the gate is off, so the emitted
             * signature is unchanged today. */
            fd->param_types[i] = params[i]->type;
        } else if (param_kinds[i] == TY_APP && params[i]->type.kind == TY_APP
                   && params[i]->type.as.app.fn) {
            /* multi-param-struct-annotation-degenerate-tyapp: preserve the
             * spined type application (`(Map K V)`, `(MutableMap K V)`) built
             * by fn_type_from_form via type_app -- its `app.fn` chain reaches
             * the head StructDef.  Without this the realiser rebuilt the param
             * as type_from_kind(TY_APP), a spineless shell whose `app.fn`/
             * `app.arg` are NULL, defeating every spine-walking predicate
             * (type_extract_struct_app / type_is_heap_struct / type_is_heap_vec).
             * Single-param `(Vec A)` already kept its spine through a different
             * realisation path; this brings multi-param apps in line. */
            fd->param_types[i] = params[i]->type;
        } else {
            fd->param_types[i] = type_from_kind(param_kinds[i]);
        }
    }
    /* LS1: carry the interned signature lifetimes into the FnDef so the always-on
     * lifetime pass (borrow_check.c) can solve over the programmer's lifetimes. */
    fd->lifetime_ctx = sig_ltctx;
    /* LS2: record the full declared return Type so borrow lifetimes survive.
     * For a borrow return we have the parsed Type (with lifetime IDs); otherwise
     * the bare kind is sufficient for the lifetime pass. */
    fd->return_type = return_borrow_type ? *return_borrow_type
                     : (return_app_type && return_app_type->kind == TY_APP
                        && return_app_type->as.app.fn)
                         ? *return_app_type
                         : type_from_kind(return_kind);
    /* Phase 15: Store collected constraints */
    fd->constraints.constraints = constraint_list;
    fd->constraints.n_constraints = n_constraints;
    fd->constraints.cap_constraints = n_constraints;
    /* class-defn-constraint-not-discharged-at-call-site: backlink the
     * constraint set onto the function's binding so a call site can re-discharge
     * each `^Class T` obligation at the concrete type its arguments pin.  Only a
     * constrained defn gets the link; NULL (the memset default) means "no
     * obligations to check". */
    if (fd->binding && n_constraints > 0) {
        fd->binding->fn_constraints = &fd->constraints;
    }

    /* AR9: variadic defn may not have an inline-C body (fixed C signatures only) */
    if (is_variadic && body && body->kind == EX_INLINE_C) {
        diag_emit(DIAG_ERROR, body->span,
                  "defn '%s': variadic body contains inline-C; "
                  "inline-C blocks need a fixed arity signature",
                  name_f->as.sym->name);
        return NULL;
    }

    /* Phase C: warn (or error under --Werror=inline-c-narrow-params) when a
     * narrow-width parameter reaches an inline-C body.  The C code sees the
     * parameter at its narrow C type (e.g. int16_t), not int64_t, which
     * surprises callers that wrote the body expecting the carrier width. */
    if (body && body->kind == EX_INLINE_C) {
        for (uint32_t _ci = 0; _ci < n_params; _ci++) {
            TypeKind k = param_kinds[_ci];
            bool is_narrow = (k == TY_INT8  || k == TY_INT16  || k == TY_INT32 ||
                              k == TY_UINT8 || k == TY_UINT16 || k == TY_UINT32 ||
                              k == TY_FLOAT32);
            if (!is_narrow) continue;
            DiagLevel sev = g_werror_inline_c_narrow_params ? DIAG_ERROR : DIAG_WARNING;
            diag_emit_with_code(sev, body->span, TUR_W0037_INLINE_C_NARROW_PARAM,
                "defn '%s': parameter '%s' has narrow type %s -- "
                "inline-C sees %s, not int64_t; add explicit casts if needed",
                name_f->as.sym->name,
                params[_ci]->name->name,
                type_name(type_from_kind(k)),
                type_c_name(type_from_kind(k)));
            if (g_werror_inline_c_narrow_params) return NULL;
        }
    }

    /* bare-fat-result-monomorphization: the canonical (int) body did not
     * typecheck -- emit no FnDef.  The binding keeps its honest TY_FN (declared
     * return + ^fat arg flags) so call sites resolve it, retains the Form for
     * per-call-site re-elaboration, and is swept after top-level elaboration so
     * a never-specialized one still surfaces its deferred diagnostic. */
    if (lazy_defer) {
        b->bare_fat_lazy = true;
        b->defn_form = call;
        elab_track_bare_fat_lazy(e, b);
        return e_nil(e, call->span);
    }

    Expr *out = expr_new(e->arena, EX_FN_DEF, fn_type, call->span);
    out->as.fn_def_.fn = fd;

    /* Nested defn: if not at file scope, register for file-scope emission
     * and return nil — the function is lifted to file scope, callable by
     * its name from this point on via the global binding. */
    if (e->scope != &e->global) {
        elab_register_file_def(e, out);
        return e_nil(e, call->span);
    }
    return out;
}

/* Phase 2: fn — (fn [param1 param2 ...] body...) — no capture for phase 2
 * Lifts to a static function. For now, we require a return type annotation.
 * Example: (fn [x y] :int (+ x y)) */
Expr *elab_fn(Elab *e, const Form *call) {
    /* H10 x call/cc: consumed here so only the immediate receiver sees it. */
    bool is_callcc_receiver = e->in_callcc_receiver;
    e->in_callcc_receiver = false;
    /* Minimum: (fn [params...] body...) */
    if (call->as.list.len < 3) {
        diag_emit(DIAG_ERROR, call->span,
                  "fn requires (fn [params...] body...)");
        return NULL;
    }

    /* CT0/CT1: contract-typed parameters of this lambda, collected during
     * parameter parsing and injected as entry checks once the body exists. */
    const Form *ct_param_preds[MAX_FN_ARITY];
    const char *ct_param_varnames[MAX_FN_ARITY];
    uint32_t    ct_param_param_idx[MAX_FN_ARITY];
    uint32_t    n_ct_param_preds = 0;

    /* Edge 1: snapshot and clear the active letrec self-exclude group at entry.
     * This `fn` IS the letrec init's top-level lambda iff elab_letrec set the
     * group right before calling us; a direct self/mutual call in our own body
     * must therefore be excluded from capture (handled by recursion machinery).
     * Clearing it immediately means any NESTED closure inside this body sees an
     * empty group and so captures the letrec-bound value through its env. */
    Binding **letrec_self_group   = e->letrec_self_group;
    uint32_t  n_letrec_self_group = e->letrec_self_group_n;
    e->letrec_self_group   = NULL;
    e->letrec_self_group_n = 0;

    const Symbol *fn_type_params[8];
    Kind fn_type_param_kinds[8];
    uint8_t n_fn_type_params = 0;
    uint8_t n_implicit_fn_type_params = 0;
    memset(fn_type_params, 0, sizeof(fn_type_params));
    for (uint8_t i = 0; i < 8; i++) fn_type_param_kinds[i] = KIND_STAR;

    uint32_t params_idx = 1;
    if (call->as.list.len > 3 &&
        call->as.list.items[1]->tag == F_VEC &&
        call->as.list.items[2]->tag == F_VEC &&
        !form_vec_has_literal_item(call->as.list.items[2])) {
        Form *type_params_f = call->as.list.items[1];
        if (type_params_f->as.list.len > 8) {
            diag_emit(DIAG_ERROR, type_params_f->span,
                      "fn: too many type parameters (max 8)");
            return NULL;
        }
        n_fn_type_params = (uint8_t)type_params_f->as.list.len;
        for (uint8_t i = 0; i < n_fn_type_params; i++) {
            Form *tp = type_params_f->as.list.items[i];
            if (tp->tag != F_SYM) {
                diag_emit(DIAG_ERROR, tp->span,
                          "fn: type parameter must be a symbol");
                return NULL;
            }
            const Symbol *psym = tp->as.sym;
            /* L6 follow-up B: `^&name` marks a row-kinded ([*]) type
             * variable on a `fn` form, mirroring the defn change. */
            if (psym->len > 2 && psym->name[0] == '^' && psym->name[1] == '&') {
                const Symbol *bare = symtab_intern(e->st,
                    strslice(psym->name + 2, psym->len - 2));
                fn_type_params[i] = bare;
                fn_type_param_kinds[i] = KIND_TYPEROW;
            } else if (psym->len > 1 && psym->name[0] == '^') {
                /* MB2 (constrained-hkt-forall-mode-b-plan): a `^f` type parameter
                 * is higher-kinded (`* -> *`), the defn analog of the
                 * `(defclass Functor [^f] ...)` marker -- so the body may use it
                 * applied as `(f a)`.  Strip the caret; annotations reference the
                 * bare name.  (Higher arities via `^^f` are not needed yet.) */
                const Symbol *bare = symtab_intern(e->st,
                    strslice(psym->name + 1, psym->len - 1));
                fn_type_params[i] = bare;
                fn_type_param_kinds[i] = KIND_ARROW;
            } else {
                fn_type_params[i] = psym;
                fn_type_param_kinds[i] = KIND_STAR;
            }
        }
        params_idx = 2;
    }

    /* Parse param vector */
    Form *params_f = call->as.list.items[params_idx];
    if (params_f->tag != F_VEC) {
        /* A symbol here with a vector right after it is not a malformed
         * parameter list -- it is a named lambda, the way Scheme, Racket, and
         * Common Lisp spell a self-recursive anonymous function.  Saying
         * "parameter list must be a vector" while a well-formed `[n : int]`
         * sits one token later sends the reader hunting for a bracket problem
         * that does not exist, and never mentions the two forms that DO give
         * recursion here.  Hitting this and concluding Turmeric has no
         * recursive lambdas is a reading the old message invited.
         *
         * Anything else in this slot really is a malformed parameter list, so
         * it keeps the generic message. */
        bool named_lambda = (params_f->tag == F_SYM)
            && (params_idx + 1 < call->as.list.len)
            && (call->as.list.items[params_idx + 1]->tag == F_VEC);
        if (named_lambda) {
            const Symbol *nm = params_f->as.sym;
            diag_emit(DIAG_ERROR, params_f->span,
                      "fn does not take a name; Turmeric has no named-lambda "
                      "form -- remove '%.*s'", (int)nm->len, nm->name);
            diag_emit(DIAG_HELP, params_f->span,
                      "for a self-recursive anonymous function, use letrec: "
                      "(letrec [%.*s (fn [...] ... (%.*s ...))] ...)",
                      (int)nm->len, nm->name, (int)nm->len, nm->name);
            diag_emit(DIAG_HELP, params_f->span,
                      "for a recursive loop, use a named let: "
                      "(let %.*s [n 5] ... (%.*s ...))",
                      (int)nm->len, nm->name, (int)nm->len, nm->name);
            return NULL;
        }
        diag_emit(DIAG_ERROR, params_f->span,
                  "fn: parameter list must be a vector [name1 name2 ...]");
        return NULL;
    }

    if (n_fn_type_params == 0) {
        const Form *implicit_ret_f = fn_return_annotation_form(call, params_idx + 1);
        n_implicit_fn_type_params = collect_implicit_fn_type_params(params_f, implicit_ret_f,
                                                                    fn_type_params, fn_type_param_kinds);
        n_fn_type_params = n_implicit_fn_type_params;
    }

    /* Parse params.  Per-param scratch is arena-sized to the param-vector length
     * (a safe upper bound), not a fixed [MAX_FN_ARITY] stack array, so a lambda
     * may take any number of positional parameters. */
    uint32_t pcap = params_f->as.list.len ? params_f->as.list.len : 1;
    Binding **params = NULL;
    uint32_t n_params = 0;
    TypeKind *param_kinds = (TypeKind *)arena_alloc(e->arena, pcap * sizeof(TypeKind));
    Type **param_full_types = (Type **)arena_alloc(e->arena, pcap * sizeof(Type *));
    for (uint32_t _i = 0; _i < pcap; _i++) param_full_types[_i] = NULL;
    /* AR5: variadic rest parameter state for fn */
    bool fn_is_variadic = false;
    TypeKind fn_rest_kind = TY_INT;
    Type *fn_rest_full_type = NULL;  /* typed-variadic: full Type for user-defined rest */
    /* LT0 (call-cc-completion CC4.4): ^linear marks the next lambda param linear
     * (exactly-once).  Consumption is tracked by the shared var-use path; the
     * LT1 drop check below mirrors elab_defn.  This lets (call/cc (fn [^linear k]
     * ...)) opt into one-shot-by-contract continuations (OQ3). */
    bool next_param_linear = false;
    /* LB1: ^borrow marks the next lambda param as a non-consuming borrow. */
    bool next_param_borrow = false;
    /* A#1 (bare-fat-lambda-param-plan): ^fat marks the next lambda param as a
     * fat-closure consumer, mirroring the defn-param path.  A bare ^fat binder
     * (no fn-type annotation) defaults to :ptr<void> + is_fat so (g x) inside
     * the body dispatches through the fat protocol instead of erroring "'g' is
     * not a function".  An explicit `:(fn [...] :T)` annotation overrides the
     * :ptr<void> default below. */
    bool next_param_fat = false;

    for (uint32_t i = 0; i < params_f->as.list.len; i++) {
        Form *p = params_f->as.list.items[i];
        if (i < n_implicit_fn_type_params && p->tag == F_SYM &&
            fn_type_params[i] == p->as.sym) {
            continue;
        }
        /* AR5: & rest-name :type -- variadic rest parameter for fn */
        if (p->tag == F_SYM && p->as.sym == e->sym_borrow) {
            if (fn_is_variadic) {
                diag_emit(DIAG_ERROR, p->span, "fn: multiple '&' in parameter list");
                return NULL;
            }
            if (i + 1 >= params_f->as.list.len) {
                diag_emit(DIAG_ERROR, p->span,
                          "fn: '&' must be followed by a rest parameter name");
                return NULL;
            }
            Form *rest_p = params_f->as.list.items[i + 1];
            if (rest_p->tag != F_SYM) {
                diag_emit(DIAG_ERROR, rest_p->span,
                          "fn: rest parameter name must be a symbol");
                return NULL;
            }
            fn_rest_kind = TY_INT;
            fn_rest_full_type = NULL;
            if (i + 2 < params_f->as.list.len) {
                Form *type_p = params_f->as.list.items[i + 2];
                /* Accept fused `& rest :int` and spaced `& rest : int`. */
                Form *type_eff = type_p;
                if (type_p->tag == F_TYPE_ANN && type_p->as.list.len == 1 &&
                    (type_p->as.list.items[0]->tag == F_SYM ||
                     type_p->as.list.items[0]->tag == F_KEYWORD)) {
                    type_eff = type_p->as.list.items[0];
                }
                if (type_eff->tag == F_KEYWORD || type_eff->tag == F_SYM) {
                    if (!resolve_variadic_rest_type(e, type_eff,
                                                    fn_type_params, fn_type_param_kinds,
                                                    n_fn_type_params, "fn",
                                                    &fn_rest_kind, &fn_rest_full_type)) {
                        return NULL;
                    }
                    if (i + 3 < params_f->as.list.len) {
                        diag_emit(DIAG_ERROR, params_f->as.list.items[i + 3]->span,
                                  "fn: no parameters allowed after '& rest :type'");
                        return NULL;
                    }
                } else {
                    diag_emit(DIAG_ERROR, type_p->span,
                              "fn: '& rest' must be followed by a type annotation (e.g. :int)");
                    return NULL;
                }
            }
            fn_is_variadic = true;
            if (n_params == 0) {
                params = (Binding **)arena_alloc(e->arena, pcap * sizeof(Binding *));
            }
            param_kinds[n_params] = TY_INT;
            Binding *rest_b = binding_new(e, rest_p->as.sym, TYPE_INT, false, false, rest_p->span);
            rest_b->is_param = true;
            params[n_params++] = rest_b;
            break;
        }
        if (p->tag == F_KEYWORD || p->tag == F_TYPE_ANN || p->tag == F_LIST ||
            p->tag == F_VEC || p->tag == F_CONTRACT_TYPE) {
            if (n_params == 0) {
                diag_emit(DIAG_ERROR, p->span,
                          "fn: type annotation without preceding parameter");
                return NULL;
            }
            /* perform-does-not-typecheck-its-arguments: the author wrote a type
             * here, so this parameter's type is declared, not the carrier
             * default the binding was created with. */
            params[n_params - 1]->type_is_carrier_default = false;
            const Form *type_form = (p->tag == F_TYPE_ANN) ? p->as.list.items[0] : p;
            Type *ann = fn_type_from_form(e, type_form,
                                          fn_type_params, fn_type_param_kinds, n_fn_type_params);
            if (!ann) return NULL;
            /* CT0: a contract-typed lambda parameter takes its BASE type, and
             * its predicate becomes an entry check -- exactly as for a `defn`.
             * Without the peel the parameter's type stayed TY_CONTRACT and
             * every call site failed with `expected { _ : ? | ... }, got int`,
             * so `#refine` on a lambda parameter did not compile at all. */
            if (ann->kind == TY_CONTRACT && ann->as.contract_.base_type) {
                if (ann->as.contract_.predicate && n_ct_param_preds < MAX_FN_ARITY) {
                    ct_param_preds[n_ct_param_preds]     = ann->as.contract_.predicate;
                    ct_param_varnames[n_ct_param_preds]  = ann->as.contract_.var_name;
                    ct_param_param_idx[n_ct_param_preds] = n_params - 1;
                    n_ct_param_preds++;
                }
                ann = ann->as.contract_.base_type;
            }
            param_kinds[n_params - 1] = ann->kind;
            params[n_params - 1]->type = *ann;
            /* Record the full type whenever the annotation carries information
             * its bare TypeKind cannot reconstruct.  TY_APP / TY_TYVAR / TY_FN
             * and any named-tyvar-bearing type obviously qualify, but so do
             * nominal TY_STRUCT / TY_ADT (and opaque newtypes, which lower to
             * TY_STRUCT): type_from_kind(TY_STRUCT) yields a def-less placeholder,
             * losing the nominal identity.  Without the full type on the
             * closure's arg_full_types, a generic callee binding its type
             * parameter through this closure's argument position (e.g. T in
             * `(fn [T] void)`) falls back to that def-less placeholder, so a
             * later bare `item : T` value parameter fails to unify against the
             * real opaque/struct type.  Recording it keeps the nominal identity
             * intact across the binding. */
            if (ann->kind == TY_APP || ann->kind == TY_TYVAR || ann->kind == TY_FN ||
                ann->kind == TY_STRUCT || ann->kind == TY_ADT ||
                fn_type_has_named_tyvar(ann)) {
                param_full_types[n_params - 1] = ann;
            }
            /* hkt-cata-function-arg (producer side): this lambda is being
             * elaborated as the value of an expected function type whose
             * argument at this position is ITSELF a function -- i.e. the lambda
             * is a function-typed carrier B = (fn [(fn ...) ...] ...), the value
             * an algebra arm folds to.  Such a carrier value is only ever
             * fat-dispatched (through the generic catamorphism / match-arm
             * carrier binding), and a capturing closure flows into this
             * function-typed slot, so the argument must cross the boundary as a
             * uniform fat box and be fat-dispatched here.  Mark the param
             * is_fat so (k ...) reads slot 0 instead of calling the fat box's
             * address as a thin function pointer -> SIGSEGV.  Symmetric with
             * the call-site arg_fat marking in elab_call.c.  Gated on the
             * expected arg being a (non-cfnptr) function type so an ordinary
             * fn-typed param of a non-carrier lambda is untouched. */
            if (ann->kind == TY_FN && !ann->as.fn.cfnptr &&
                e->expected_type && e->expected_type->kind == TY_FN &&
                n_params >= 1 && (n_params - 1) < e->expected_type->as.fn.arity &&
                e->expected_type->as.fn.arg_full_types) {
                Type *exp_arg =
                    e->expected_type->as.fn.arg_full_types[n_params - 1];
                if (exp_arg && exp_arg->kind == TY_FN && !exp_arg->as.fn.cfnptr) {
                    params[n_params - 1]->is_fat = true;
                }
            }
            continue;
        }
        /* LT0 (CC4.4): ^linear marks the next param linear (exactly-once). */
        if (p->tag == F_SYM && p->as.sym == e->sym_caret_linear) {
            next_param_linear = true;
            continue;
        }
        /* LB1: ^borrow marks the next lambda param as a non-consuming borrow. */
        if (p->tag == F_SYM && p->as.sym == e->sym_caret_borrow) {
            next_param_borrow = true;
            continue;
        }
        /* A#1 (bare-fat-lambda-param-plan): ^fat marks the next param as a
         * fat-closure consumer (see next_param_fat declaration above). */
        if (p->tag == F_SYM && p->as.sym == e->sym_caret_fat) {
            next_param_fat = true;
            continue;
        }
        if (p->tag != F_SYM) {
            diag_emit(DIAG_ERROR, p->span,
                      "fn: parameter name must be a symbol or type annotation");
            /* params is arena-allocated, no need to free */
            return NULL;
        }
        /* No hard parameter-count ceiling (see the defn path); arbitrary-fn-arity
         * Phase 6 lint nudge past the historical 16 cap. */
        if (n_params == HIGH_ARITY_SOFT_LIMIT) {
            diag_emit_with_code(DIAG_WARNING, p->span, TUR_W0041_HIGH_ARITY,
                      "fn has more than %d positional parameters; prefer a defstruct "
                      "options value or a '& rest :type' variadic (see the Function "
                      "Arity Style Guide)", HIGH_ARITY_SOFT_LIMIT);
        }
        /* Untyped fn params preserve the existing int default -- or take `any`
         * in a Saffron file, the same rule `defn` uses (S2/D3). */
        TypeKind fn_dflt_k = saffron_default_param_kind(p->span);
        param_kinds[n_params] = fn_dflt_k;
        Binding *b = binding_new(e, p->as.sym, type_from_kind(fn_dflt_k),
                                 false, false, p->span);
        b->is_param = true;
        /* perform-does-not-typecheck-its-arguments: see the defn path.  This is
         * the site that produces the carrier ints a naive perform-argument check
         * reports -- an un-annotated lambda param routinely carries a `cstr`. */
        b->type_is_carrier_default = true;
        /* Bidirectional inference (constrained-generic-as-value-bakes-
         * representative.md): when this lambda is elaborated against an expected
         * function type (pushed by the call site for a fn-typed parameter), type
         * an UN-annotated param from the expected arg type instead of defaulting
         * to the int carrier.  This lets `(fn [b] (count-it b))` passed where
         * `(fn [Box] int)` is expected see `b : Box`, so the inner direct
         * typeclass-method dispatch pins its real instance rather than baking the
         * carrier representative.  Gated to NON-primitive expected types
         * (struct/adt/app): for primitives the int carrier is already correct, so
         * leaving them untouched avoids churning existing codegen. */
        if (e->expected_type && e->expected_type->kind == TY_FN &&
            n_params < e->expected_type->as.fn.arity &&
            e->expected_type->as.fn.arg_full_types) {
            Type *exp_full = e->expected_type->as.fn.arg_full_types[n_params];
            if (exp_full && (exp_full->kind == TY_STRUCT ||
                             exp_full->kind == TY_ADT ||
                             exp_full->kind == TY_APP)) {
                param_kinds[n_params] = exp_full->kind;
                b->type = *exp_full;
                param_full_types[n_params] = exp_full;
                /* Determined from the expected fn type, so no longer a default.
                 * (This arm is gated to non-primitive expected types, which is
                 * exactly why a `cstr` param stays a carrier int.) */
                b->type_is_carrier_default = false;
            }
        }
        if (next_param_linear) {
            b->is_linear = true;
            next_param_linear = false;
        }
        /* LB1: a ^borrow lambda param reads its linear/affine argument without
         * consuming it (see call-site handling). */
        if (next_param_borrow) {
            b->is_borrow = true;
            next_param_borrow = false;
        }
        /* A#1 (bare-fat-lambda-param-plan): a bare ^fat lambda param holds a
         * fat-closure box.  Default it to :ptr<void> + is_fat so (g x) routes
         * through the fat-dispatch path; a later `:(fn [...] :T)` annotation
         * overrides the type (is_fat stays set, exactly as on defn params). */
        if (next_param_fat) {
            b->is_fat = true;
            b->type = TYPE_PTR_VOID;
            param_kinds[n_params] = TY_PTR_VOID;
            next_param_fat = false;
        }
        if (n_params == 0) {
            params = (Binding **)arena_alloc(e->arena, pcap * sizeof(Binding *));
        }
        params[n_params++] = b;
    }

    /* Parse return type annotation and body */
    TypeKind return_kind = TY_NIL;
    /* Whether the lambda DECLARED its return type.  `return_kind` starts at
     * TY_NIL, so an explicit `: nil` is indistinguishable from "unannotated"
     * without this -- and the inference below would then override the
     * declaration with the body's tail type.  That is how
     * `(fn [c : ptr<void>] : nil (bump _b))` came out of the emitter as
     * `static int64_t __fn_N(void *, void *)` while the typed-thunk ABI built
     * from the same declaration said `void (*)(void *, void *)` -- an indirect
     * call through mismatched function-pointer types, which
     * -fsanitize=function reports and CFI / CET-BTI / WASM call_indirect
     * reject outright.
     * See docs/reported/emitter-thunk-type-return-mismatch.md. */
    bool return_annotated = false;
    Type *return_full_type = NULL;
    Type *return_fn_type = NULL; /* Preserve full TY_FN returns for higher-order calls. */
    uint32_t body_start = params_idx + 1;

    /* Phase 19: Parse optional effect-row annotation #{Read Write} or #{e} before return type. */
    EffectRow *declared_effect_row_fn = NULL;
    if (call->as.list.len >= params_idx + 2) {
        Form *maybe_row = call->as.list.items[params_idx + 1];
        if (maybe_row->tag == F_MAP) {
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
            declared_effect_row_fn = effect_row_unresolved(e->arena, syms, n_valid,
                                                           maybe_row->span);
            body_start = params_idx + 2;
        }
    }
    bool fn_declared_unsafe =
        effect_row_contains_symbol(declared_effect_row_fn, e->sym_effect_unsafe);

    /* Check for : return-type annotation.  A trailing keyword is the body,
     * not an annotation -- see elab_defn (`(fn [] :kw)` was "fn: missing
     * body"). */
    if (call->as.list.len >= (body_start + 1) &&
        !(call->as.list.len == body_start + 1 &&
          call->as.list.items[body_start]->tag == F_KEYWORD)) {
        Form *ret_f = call->as.list.items[body_start];
        /* Spaced `: T` where T is a single symbol or keyword: treat as if
         * fused so the full F_KEYWORD lookup ladder (alias / ADT / struct /
         * sized prim / type-param / opaque-fallback) runs.  Compound
         * `: (-> a b)` etc. still falls through to the F_TYPE_ANN branch. */
        if (ret_f->tag == F_TYPE_ANN && ret_f->as.list.len == 1 &&
            (ret_f->as.list.items[0]->tag == F_KEYWORD ||
             ret_f->as.list.items[0]->tag == F_SYM)) {
            Form *inner = ret_f->as.list.items[0];
            ret_f = inner;
            if (ret_f->tag == F_SYM) {
                Form *kf = (Form *)arena_alloc(e->arena, sizeof(Form));
                *kf = *ret_f;
                kf->tag = F_KEYWORD;
                ret_f = kf;
            }
        }
        if (ret_f->tag == F_KEYWORD) {
            /* : int, : bool, etc. */
            const Symbol *kw = ret_f->as.sym;
            uint8_t type_param_idx = 0;
            Type *ptr_ret = ptr_type_from_keyword_name(e, kw->name, kw->len,
                                                       ret_f->span, NULL,
                                                       fn_type_params,
                                                       fn_type_param_kinds,
                                                       n_fn_type_params);
            /* rc-angle-bracket-annotation-becomes-tyvar: a typed reference-family
             * `: rc<T>` / `: weak<T>` / `: ref<T>` / `: lref<T>` return type. */
            Type *rc_ret = rc_family_type_from_keyword_name(e, kw->name, kw->len,
                                                            ret_f->span, NULL,
                                                            fn_type_params,
                                                            fn_type_param_kinds,
                                                            n_fn_type_params);
            if (ptr_ret) {
                return_kind = TY_PTR_VOID;
                return_full_type = ptr_ret;
            } else if (rc_ret) {
                return_kind = rc_ret->kind;
                return_full_type = rc_ret;
            } else if (fn_type_param_index(fn_type_params, n_fn_type_params, kw, &type_param_idx)) {
                return_kind = TY_TYVAR;
                return_full_type = (Type *)arena_alloc(e->arena, sizeof(Type));
                *return_full_type = type_tyvar_named(kw->name);
                return_full_type->hkt_kind = fn_type_param_kinds[type_param_idx];
            } else if (kw->len == 3 && memcmp(kw->name, "int", 3) == 0) {
                return_kind = TY_INT;
            } else if (kw->len == 5 && memcmp(kw->name, "float", 5) == 0) {
                return_kind = TY_FLOAT;
            } else if (kw->len == 4 && memcmp(kw->name, "bool", 4) == 0) {
                return_kind = TY_BOOL;
            } else if (kw->len == 4 && memcmp(kw->name, "void", 4) == 0) {
                return_kind = TY_NIL;
            } else if (kw->len == 4 && memcmp(kw->name, "cstr", 4) == 0) {
                return_kind = TY_CSTR;
            } else if (kw->len == 9 && memcmp(kw->name, "ptr<void>", 9) == 0) {
                return_kind = TY_PTR_VOID;
            } else if (kw->len == 3 && memcmp(kw->name, "nil", 3) == 0) {
                return_kind = TY_NIL;
            } else if (kw->len == 3 && memcmp(kw->name, "ptr", 3) == 0) {
                return_kind = TY_PTR_VOID;
            } else if (kw->len == 2 && memcmp(kw->name, "rc", 2) == 0) {
                return_kind = TY_RC;
            } else if (kw->len == 4 && memcmp(kw->name, "weak", 4) == 0) {
                return_kind = TY_WEAK;
            } else if (kw->len == 1 && memcmp(kw->name, "!", 1) == 0) {
                return_kind = TY_NEVER;
            } else {
                /* struct-return-through-closure-loses-type (CURRY-V0): a bare
                 * single-symbol return type that names a struct or ADT must
                 * resolve to that nominal type and carry it on the lambda's
                 * fn_type (result_full_type), exactly as elab_defn does.
                 * Without this, `(fn [..] : Person ..)` left the return type as
                 * a TY_TYVAR named "Person", so the lifted thunk returned the
                 * int64 carrier and the value's struct type was lost at the
                 * call site -- a following (.field ...) could not resolve. */
                bool resolved_nominal = false;
                /* Phase N6 mirror (see elab_defn): sized primitives and the
                 * other simple named kinds typekind_from_symbol knows
                 * (int32, uint8, ..., Sym, Syntax) as a fn LITERAL's return
                 * keyword.  Without this they fell through to the tyvar
                 * path and the lambda returned the int64 carrier -- e.g. a
                 * `(fn [i : int] : Syntax ...)` inside a defmacro* body
                 * typed its calls int and tripped spurious if-branch
                 * mismatches (then=Syntax else=int). */
                {
                    TypeKind sized_k = typekind_from_symbol(kw->name);
                    if (sized_k != TY_UNKNOWN && sized_k != TY_INT &&
                            sized_k != TY_FLOAT && sized_k != TY_BOOL &&
                            sized_k != TY_CSTR && sized_k != TY_NIL) {
                        return_kind      = sized_k;
                        resolved_nominal = true;
                    }
                }
                /* defalias table (mirror elab_defn's TA1/TA2 ladder) */
                if (!resolved_nominal) {
                    const Symbol *ksym = symtab_intern(e->st, strslice(kw->name, kw->len));
                    for (uint32_t ai = 0; ai < e->n_type_aliases; ai++) {
                        if (e->type_alias_names[ai] == ksym) {
                            return_kind      = e->type_alias_kinds[ai];
                            return_full_type = e->type_alias_types[ai];
                            resolved_nominal = true;
                            break;
                        }
                    }
                }
                /* ADT name */
                if (!resolved_nominal) {
                    for (uint32_t ai = 0; ai < e->n_adt_defs; ai++) {
                        if (strcmp(e->adt_defs[ai]->name, kw->name) == 0) {
                            return_kind = TY_ADT;
                            return_full_type = (Type *)arena_alloc(e->arena, sizeof(Type));
                            *return_full_type = type_adt(e->adt_defs[ai]);
                            resolved_nominal = true;
                            break;
                        }
                    }
                }
                /* structdef-retirement DS-D: the struct-name loop (scan
                 * e->struct_defs -> TY_STRUCT full-type) is dead -- the
                 * struct_defs registry is always empty; a struct return name
                 * resolves as a record ADT above or falls through to a tyvar. */
                if (!resolved_nominal) {
                    return_kind = TY_TYVAR;
                    return_full_type = (Type *)arena_alloc(e->arena, sizeof(Type));
                    *return_full_type = type_tyvar_named(kw->name);
                }
            }
            return_annotated = true;   /* an explicit `: T`, including `: nil` */
            body_start++;
        } else if (ret_f->tag == F_TYPE_ANN) {
            /* Compound return type via `: type-expr` syntax: `: (-> a b)`, `: (vec int)`, etc. */
            if (ret_f->as.list.len > 0) {
                Type *ann = fn_type_from_form(e, ret_f->as.list.items[0],
                                              fn_type_params, fn_type_param_kinds, n_fn_type_params);
                /* CT0: a contract return type on a lambda contributes its base
                 * type; the predicate is a `defn`-level construct (there is no
                 * postcondition injection point for an anonymous fn), so it is
                 * peeled and dropped rather than left to break the result kind. */
                if (ann && ann->kind == TY_CONTRACT && ann->as.contract_.base_type)
                    ann = ann->as.contract_.base_type;
                if (ann) {
                    return_kind = ann->kind;
                    if (ann->kind == TY_TYVAR || fn_type_has_named_tyvar(ann)) {
                        return_full_type = ann;
                    }
                    if (ann->kind == TY_FN) {
                        return_fn_type = ann;
                    }
                    /* fat-closure-dispatch-aggregate-return: a lambda whose
                     * declared return is an aggregate/applied type (TY_APP like
                     * (Pair A B), a concrete TY_STRUCT, or a TY_ADT) must carry
                     * the full type on its fn_type.  Otherwise emit lowers the
                     * lifted thunk's C return type to int64_t while the body
                     * returns the struct by value -- a hard C compile error. */
                    if (!return_full_type &&
                        (ann->kind == TY_APP || ann->kind == TY_ADT)) {
                        return_full_type = ann;
                    }
                }
            }
            return_annotated = true;   /* an explicit `: T`, including `: nil` */
            body_start++;
        }
    }

    /* Elaborate body */
    if (call->as.list.len < body_start + 1) {
        diag_emit(DIAG_ERROR, call->span,
                  "fn: missing body");
        /* params is arena-allocated, no need to free */
        return NULL;
    }

    /* Push a new scope for the function body with params bound */
    /* CF7.3: record the scope just before this lambda's inner scope is pushed. */
    struct Scope *saved_fn_entry_outer_scope = e->fn_entry_outer_scope;
    e->fn_entry_outer_scope = e->scope;
    Scope inner;
    scope_init(&inner, e->scope);
    e->scope = &inner;
    for (uint32_t i = 0; i < n_params; i++) {
        scope_add(&inner, params[i]);
    }

    /* KB-025: accumulate this closure's signature type variables on top of the
     * enclosing function's set (see elab_defn for the rationale). */
    uint8_t saved_n_sig_tyvars = e->n_sig_tyvars;
    for (uint32_t i = 0; i < n_params; i++) {
        fn_collect_sig_tyvars(e, &params[i]->type);
    }
    fn_collect_sig_tyvars(e, return_full_type);
    fn_collect_sig_tyvars(e, return_fn_type);

    Expr *body = e_nil(e, call->span);
    uint32_t n_body = call->as.list.len - body_start;

    /* closure-capture-escapes-linearity: snapshot the substructural state of
     * every enclosing linear/unique binding BEFORE the body runs.
     *
     * Elaborating the body marks an OUTER binding consumed at the point the
     * closure is built, not where it is called, so `(f) (f)` on a closure that
     * frees a captured `^linear` was two consumptions the checker never saw --
     * a real double-free.  Comparing this snapshot against the state after the
     * body identifies exactly which captures the body CONSUMES (as opposed to
     * merely reads), which is the distinction the fix turns on: a read-only
     * capture is safe at any arity and must stay accepted.
     *
     * Arena-allocated rather than malloc'd on purpose -- elab_fn has many early
     * returns between here and the use site below, and the compiler path is
     * leak-checked. */
    uint32_t n_lin_snap = 0;
    for (const Scope *cur = e->scope; cur; cur = cur->parent)
        for (uint32_t i = 0; i < cur->n; i++)
            if (cur->bindings[i]->is_linear || cur->bindings[i]->is_unique)
                n_lin_snap++;
    FnCaptureLinSnap *lin_snap =
        (FnCaptureLinSnap *)arena_alloc(e->arena,
                                        (n_lin_snap ? n_lin_snap : 1)
                                            * sizeof(FnCaptureLinSnap));
    {
        uint32_t k = 0;
        for (const Scope *cur = e->scope; cur; cur = cur->parent)
            for (uint32_t i = 0; i < cur->n; i++) {
                Binding *b = cur->bindings[i];
                if (!b->is_linear && !b->is_unique) continue;
                lin_snap[k].binding       = b;
                lin_snap[k].was_consumed  = b->is_linear_consumed;
                lin_snap[k].was_moved     = b->is_moved;
                k++;
            }
    }

    e->fn_body_depth++;
    /* A lambda's `return` leaves the lambda: the enclosing function's
     * result checks do not apply to it. */
    const RetContract *fn_saved_ret_contract = e->ret_contract;
    e->ret_contract = NULL;
    if (fn_declared_unsafe) e->unsafe_depth++;
    /* Propagate the lambda's declared return type onto the expected-type
     * channel during body elaboration.  Mirrors what elab_defn already does
     * for top-level defns, so bare parametric-ADT constructor calls
     * (`(PFail)` in `(fn [xs] : (PRes A) ...)`) infer their type parameter
     * from the declared return -- otherwise a lambda body's constructors
     * lack the enclosing expected type that elab_defn provides. */
    Type *saved_fn_body_expected = e->expected_type;
    if (return_full_type) {
        e->expected_type = return_full_type;
    } else if (return_fn_type) {
        e->expected_type = return_fn_type;
    }
    {
        /* Internal defines: splice (define name init) into nested let forms. */
        Form *spliced = splice_internal_defines(e,
                            &call->as.list.items[body_start], n_body, call->span);
        if (spliced) {
            body = elab_form(e, spliced);
            if (!body) {
                if (fn_declared_unsafe) e->unsafe_depth--;
                e->fn_body_depth--;
                e->ret_contract = fn_saved_ret_contract;
                e->n_sig_tyvars = saved_n_sig_tyvars;
                e->fn_entry_outer_scope = saved_fn_entry_outer_scope;
                e->scope = inner.parent;
                scope_free(&inner);
                return NULL;
            }
        } else if (n_body == 1) {
            body = elab_form(e, call->as.list.items[body_start]);
            if (!body) {
                if (fn_declared_unsafe) e->unsafe_depth--;
                e->fn_body_depth--;
                e->ret_contract = fn_saved_ret_contract;
                e->n_sig_tyvars = saved_n_sig_tyvars;
                e->fn_entry_outer_scope = saved_fn_entry_outer_scope;
                e->scope = inner.parent;
                scope_free(&inner);
                return NULL;
            }
        } else {
            Expr **items = (Expr **)arena_alloc(e->arena, n_body * sizeof(Expr *));
            for (uint32_t i = 0; i < n_body; i++) {
                items[i] = elab_form(e, call->as.list.items[body_start + i]);
                if (!items[i]) {
                    if (fn_declared_unsafe) e->unsafe_depth--;
                    e->fn_body_depth--;
                    e->ret_contract = fn_saved_ret_contract;
                    e->n_sig_tyvars = saved_n_sig_tyvars;
                    e->fn_entry_outer_scope = saved_fn_entry_outer_scope;
                    e->scope = inner.parent;
                    scope_free(&inner);
                    return NULL;
                }
            }
            body = expr_new(e->arena, EX_DO, items[n_body - 1]->type, call->span);
            body->as.do_.items = items;
            body->as.do_.n = n_body;
        }
    }
    e->expected_type = saved_fn_body_expected;
    if (fn_declared_unsafe) e->unsafe_depth--;
    e->fn_body_depth--;
    e->ret_contract = fn_saved_ret_contract;
    e->n_sig_tyvars = saved_n_sig_tyvars;
    e->fn_entry_outer_scope = saved_fn_entry_outer_scope;

    /* LT1 (call-cc-completion CC4.4): at lambda scope exit, verify every ^linear
     * param was consumed.  A ^linear continuation handed to (call/cc (fn [^linear
     * k] ...)) that is never invoked is a dropped linear value (TUR-E0100). */
    if (body) {
        for (uint32_t _li = 0; _li < n_params; _li++) {
            /* LB1: ^borrow params are non-consuming; they carry no consumption
             * obligation (see the defn-scope LT1 check above). */
            if (params[_li]->is_borrow) continue;
            if (params[_li]->is_linear && !params[_li]->is_linear_consumed
                    && !params[_li]->is_moved) {
                diag_emit_with_code(DIAG_ERROR, params[_li]->span,
                                    TUR_E0100_LINEAR_DROPPED,
                                    "linear parameter '%s' dropped without being consumed",
                                    params[_li]->name->name);
            }
        }
    }

    /* CT1: inject this lambda's parameter contract checks.  Done BEFORE capture
     * analysis so the final body is what gets scanned, and while the inner
     * scope is still current so the predicate can be elaborated in it. */
    if (n_ct_param_preds > 0 && body && rt_contracts_emitted()) {
        Binding *ct_check_fn = scope_lookup(&e->global, e->sym_tur_contract_check);
        body = rt_inject_param_checks(e, body, ct_check_fn, params, n_params,
                                      ct_param_preds, ct_param_varnames,
                                      ct_param_param_idx, n_ct_param_preds,
                                      call->span);
    }

    /* Phase 3: Capture analysis - collect free variables in the body */
    /* We need to do this before popping the scope */
    uint32_t n_captures = 0;
    Binding **captures = collect_free_vars(body, params, n_params,
                                           letrec_self_group, n_letrec_self_group,
                                           &n_captures);

    /* closure-capture-escapes-linearity: a closure that CONSUMES a captured
     * linear/unique value is itself linear/unique.
     *
     * Consuming the capture once per call means the closure carries exactly the
     * obligation the captured value had, so inheriting its copy_kind makes every
     * bad shape fall out of checks that already exist, with no new diagnostic:
     * calling it twice is TUR-E0101 use-after-consume, `(rc/of f)` is TUR-E0103,
     * and dropping it unused is TUR-E0100 (correct -- the captured resource would
     * never be released).  The unique case lands on TUR-E0201/E0202 the same way.
     *
     * CONSUMES, not merely captures.  A closure that only READS a captured linear
     * value is safe at any arity; a blanket capture-based rule (the shape
     * TUR-E0500 uses for ^multishot handlers, where N invocations are
     * definitional) would reject a large amount of working code -- 73 fixtures
     * carry captures, and the httpd middleware closures that dominate that list
     * are read-only. */
    bool capture_consumes_linear = false;
    bool capture_consumes_unique = false;
    for (uint32_t ci = 0; ci < n_captures && !(capture_consumes_linear
                                               && capture_consumes_unique); ci++) {
        Binding *cb = captures[ci];
        if (!cb->is_linear && !cb->is_unique) continue;
        for (uint32_t si = 0; si < n_lin_snap; si++) {
            if (lin_snap[si].binding != cb) continue;
            /* Transitioned during THIS body: the body is what consumed it. */
            if (cb->is_linear && cb->is_linear_consumed && !lin_snap[si].was_consumed)
                capture_consumes_linear = true;
            if (cb->is_unique && cb->is_moved && !lin_snap[si].was_moved)
                capture_consumes_unique = true;
            break;
        }
    }

    /* Pop scope */
    e->scope = inner.parent;
    scope_free(&inner);

    /* saffron-dynamic-surface-pass H10: the dialect's `any` default reached
     * `defn` (elab_defn pins the signature) but not `fn`, so a lambda whose
     * body has a concrete type -- `(fn [] 7.25)`, `(fn [x] "s")` -- was typed
     * `(fn [] float)` and the dynamic call site refused it: "cannot call this
     * function here -- it takes a different number of arguments".  A body
     * that is already an `any` (`(fn [x] (+ x 1))`) never showed it.
     *
     * Two gates.  An EXPECTED function type (this lambda is an argument to a
     * typed callee -- `vec-filter`'s predicate, a typed callback) already
     * decides the return, so the default yields to it exactly as the
     * parameter default above does -- which is also what keeps a side-effect
     * lambda passed straight to a typed `(fn [T] nil)` slot `nil`.
     *
     * A nil or diverging body with NO expected function type gets `any` too.
     * It used to stay `nil`, and a `void` callee is one the dynamic call site
     * refuses with the same "cannot call this function here": `(each xs (fn
     * [x] (set! acc (+ acc x))))` through an untyped `each` panicked, the
     * shape every Saffron accumulate-in-a-lambda loop takes (found fixing
     * compiled-closure-copies-a-captured-mut).  The body needs no box when it
     * diverges; a nil body is boxed as the nil tag by the widen below. */
    if (!return_annotated && return_kind == TY_NIL && body &&
        body->type.kind != TY_ANY && lang_span_is_dynamic(call->span) &&
        !is_callcc_receiver &&
        !(e->expected_type && e->expected_type->kind == TY_FN)) {
        return_kind = TY_ANY;
    }
    /* TY2.2 at the lambda: a `: any` return (declared, or pinned just above)
     * boxes a narrower body, as elab_defn does.  Without it an explicit
     * `(fn [] : any 7.25)` emitted `return 7.25` into a tur_tagged_t slot. */
    if (return_kind == TY_ANY && body && body->type.kind != TY_ANY &&
        body->type.kind != TY_NEVER) {
        body = elab_coerce_to_any_return(e, body);
    }
    /* byvalue-recursive-shared-copies-leak: as elab_defn -- the result is
     * owned, so a shared view there is cloned. */
    body = elab_own_byval_copy(e, body, NULL);

    /* Infer return type from body if not specified.
     *
     * Gated on `!return_annotated` so an explicit `: nil` is honoured: without
     * that, a lambda declared `: nil` whose body tails into a value-returning
     * call was silently retyped to that call's result, and the emitted function
     * disagreed with the typed-thunk pointer built from its declaration. */
    if (!return_annotated && return_kind == TY_NIL && body->type.kind != TY_NIL) {
        return_kind = body->type.kind;
        if (body->type.kind == TY_FN) {
            Type *rft = (Type *)arena_alloc(e->arena, sizeof(Type));
            *rft = body->type;
            return_fn_type = rft;
        } else if (!return_full_type && body->type.kind == TY_TYVAR &&
                   body->type.as.tyvar_.name) {
            /* generic-closure-capture-of-float-truncates: an unannotated lambda
             * whose body is a value of the enclosing defn's type parameter --
             * `(fn [] v)` with `v : A` -- records `A` exactly as an explicit
             * `: A` would.  Dropping it left a bare TY_TYVAR result kind with no
             * name, which nothing can instantiate: `((capture 7.25))` read the
             * result as the int carrier and printed 7, and the float spec's env
             * fill numerically converted 7.25 into the int64 slot. */
            Type *rft = (Type *)arena_alloc(e->arena, sizeof(Type));
            *rft = body->type;
            return_full_type = rft;
        } else if (!return_full_type &&
                   (body->type.kind == TY_APP || body->type.kind == TY_ADT) &&
                   !fn_type_has_named_tyvar(&body->type)) {
            /* closure-result-monomorphization Phase 2 grounding: an unannotated
             * lambda whose body is a carrier-ABI aggregate (e.g. a monadic
             * continuation `(fn [x] (some (+ x 1)))` returning `(Option int)`)
             * previously recorded only result_kind=TY_APP and dropped the full
             * type, leaving the fn type's result as `(type-app ? ?)`.  That
             * erasure is exactly why `m7_collect_tyvar_bindings` could not ground
             * `b` in `bind`/`ap`'s `(m b)` result (the continuation's concrete
             * `(Option int)` was unrecoverable), forcing the carrier fallback.
             * Preserve the body's full aggregate type so the element grounds.
             *
             * Gate on a FULLY-GROUND body: a residual free named tyvar (e.g. the
             * fixed arm `B` of a partial ok-biased `(Result int B)` instance)
             * would make the lifted wrapper `emit_abi_fn_is_generic_unsafe` and
             * get skipped as dead generic code while its address is still taken
             * (an undeclared `__poly_N`).  Only concrete aggregates are safe to
             * record here. */
            Type *rft = (Type *)arena_alloc(e->arena, sizeof(Type));
            *rft = body->type;
            return_full_type = rft;
        }
    }

    /* bare-fat-param-non-int-result (Phase A): same retype as elab_defn, run on
     * each lambda's own body so a `fn` whose declared result is non-int-class
     * and whose tail is a bare-^fat call gets the correct call result type.
     * Per-lambda scope -> no cross-lambda leakage. */
    if (kind_is_non_int_register_class(return_kind)) {
        if (retype_bare_fat_tail_calls(body, return_kind) &&
            body->type.kind == TY_INT) {
            body->type = type_from_kind(return_kind);
        }
    }

    /* Create function type */
    TypeKind *arg_kinds = (TypeKind *)arena_alloc(e->arena, (n_params ? n_params : 1) * sizeof(TypeKind));
    for (uint32_t i = 0; i < n_params; i++) {
        arg_kinds[i] = param_kinds[i];
    }
    Type fn_type = type_fn(arg_kinds, n_params, return_kind);
    /* AR6: mark variadic functions in their type */
    fn_type.as.fn.is_variadic = fn_is_variadic;
    fn_type.as.fn.rest_kind   = fn_rest_kind;
    fn_type.as.fn.rest_full_type = fn_rest_full_type;
    {
        bool any_full = false;
        for (uint32_t i = 0; i < n_params; i++) {
            if (param_full_types[i]) { any_full = true; break; }
        }
        if (any_full) {
            Type **aFT = (Type **)arena_alloc(e->arena, n_params * sizeof(Type *));
            for (uint32_t i = 0; i < n_params; i++) aFT[i] = param_full_types[i];
            fn_type.as.fn.arg_full_types = aFT;
        }
    }
    /* bare-fat-sink-poly-box-slot0-int64-mismatch.md: same bare-^fat signature
     * synthesis as elab_defn, for a lambda sink.  A retyped bare `^fat` param
     * gets a `(fn [args] : R)` recorded on arg_full_types so a caller boxing a
     * tur_poly_fn_t selects the typed slot-0 shim. */
    if (kind_is_non_int_register_class(return_kind)) {
        for (uint32_t i = 0; i < n_params; i++) {
            if (!(params[i]->is_fat && params[i]->type.kind == TY_PTR_VOID))
                continue;
            TypeKind *akinds = NULL;
            int n = bare_fat_tail_call_arg_kinds(e->arena, body, params[i], &akinds);
            if (n < 0) continue;
            if (!fn_type.as.fn.arg_full_types) {
                Type **aFT = (Type **)arena_alloc(e->arena, n_params * sizeof(Type *));
                for (uint32_t k = 0; k < n_params; k++) aFT[k] = NULL;
                fn_type.as.fn.arg_full_types = aFT;
            }
            Type *ft = (Type *)arena_alloc(e->arena, sizeof(Type));
            *ft = type_fn(akinds, (uint8_t)n, return_kind);
            fn_type.as.fn.arg_full_types[i] = ft;
        }
    }
    if (return_full_type) {
        fn_type.as.fn.result_full_type = return_full_type;
    }
    if (return_fn_type) {
        fn_type.as.fn.result_full_type = return_fn_type;
    }
    /* A#1 (bare-fat-lambda-param-plan): propagate ^fat param flags into the
     * lambda's fn_type so call sites auto-shim bare fn arguments into fat
     * closures, mirroring the defn path. */
    {
        bool any_fat = false;
        for (uint32_t i = 0; i < n_params; i++) {
            if (params[i]->is_fat) { any_fat = true; break; }
        }
        if (any_fat) {
            for (uint32_t i = 0; i < n_params; i++) {
                FN_ARG_SET(fn_type.as.fn, i, FA_FAT, params[i]->is_fat);
            }
        }
    }
    /* Phase CCL (fn-first-class-application): propagate `:fn` poly-closure
     * markers into the lambda's fn_type, mirroring the defn path (bare `:fn`
     * carrier with poly_type == NULL, or the F5 typed carrier with a concrete
     * TY_FN poly_type). */
    for (uint32_t i = 0; i < n_params; i++) {
        if (params[i]->is_poly_fn &&
            (params[i]->poly_type == NULL ||
             params[i]->poly_type->kind == TY_FN)) {
            FN_ARG_SET(fn_type.as.fn, i, FA_POLY_FN, true);
        }
    }
    /* LB1: propagate ^borrow param flags into the lambda's fn_type. */
    {
        bool any_borrow = false;
        for (uint32_t i = 0; i < n_params; i++) {
            if (params[i]->is_borrow) { any_borrow = true; break; }
        }
        if (any_borrow) {
            for (uint32_t i = 0; i < n_params; i++) {
                FN_ARG_SET(fn_type.as.fn, i, FA_BORROW, params[i]->is_borrow);
            }
        }
    }

    /* closure-capture-escapes-linearity: stamp the inherited obligation onto the
     * closure's own type.  A `let` binding whose initializer type carries
     * CK_LINEAR is already marked linear (elab_forms.c), so this is all it takes
     * for the existing use-after-consume / rc-wrap / dropped checks to start
     * seeing the closure.  Linear wins over unique when a body consumes both --
     * it is the stricter obligation. */
    if (capture_consumes_linear)      fn_type.copy_kind = CK_LINEAR;
    else if (capture_consumes_unique) fn_type.copy_kind = CK_UNIQUE;

    /* Check if we're at top level */
    bool at_top_level = (e->scope == &e->global);

    /* For anonymous fn, we lift it to a static function with a generated name.
     * We use the arena to allocate a unique name. */
    char fn_name_buf[32];
    snprintf(fn_name_buf, sizeof(fn_name_buf), "__fn_%u", elab_fresh_id(e));
    const Symbol *fn_name_sym = symtab_intern(e->st, 
        strslice(fn_name_buf, (uint32_t)strlen(fn_name_buf)));
    
    Binding *b = binding_new(e, fn_name_sym, fn_type, false, true, call->span);
    scope_add(&e->global, b);
    /* pr-386 regression fix: mark this as a lifted-lambda helper so the
     * let-bound source_binding alias rule (elab_forms.c) does not chain to it.
     * A captureless closure-returning lambda must be called through the
     * closure-dispatch protocol on the let binding, not by a direct call to
     * __fn_N (whose C signature returns the int64 carrier, not a fn pointer). */
    b->is_lifted_lambda = true;
    /* `__fn_%u` above is a name the elaborator made up; nothing that lists
     * symbols for a human should offer it. */
    b->is_synthesized = true;
    b->synth_kind = SYNTH_LAMBDA;
    /* ...and no diagnostic should print it: say where the `fn` is instead.
     * The span already points at the literal; the enclosing defn is what
     * lets a reader find it without counting lambdas. */
    if (e->current_fn_name && e->current_fn_name->name) {
        char lbl[160];
        int n = snprintf(lbl, sizeof(lbl), "anonymous function in '%s'",
                         e->current_fn_name->name);
        if (n > 0 && (size_t)n < sizeof(lbl))
            b->diag_label = arena_strdup(e->arena, lbl, (size_t)n);
    }
    if (!b->diag_label) b->diag_label = "anonymous function";
    b->returns_closure_fn_binding = expr_closure_fn_binding(body);

    /* RT1: publish this lambda's contract parameters on its lifted thunk
     * binding.  A `let` bound to the lambda copies them across (elab_forms.c),
     * which is what lets `(let [f (fn [x : Pos] ...)] (f 0))` be checked at the
     * call the way a call to a named function is. */
    if (n_ct_param_preds > 0 && n_params > 0) {
        const Form **rp = (const Form **)arena_alloc(e->arena, n_params * sizeof(Form *));
        const char **rv = (const char **)arena_alloc(e->arena, n_params * sizeof(char *));
        const char **rn = (const char **)arena_alloc(e->arena, n_params * sizeof(char *));
        for (uint32_t _pi = 0; _pi < n_params; _pi++) {
            rp[_pi] = NULL;
            rv[_pi] = NULL;
            rn[_pi] = (params[_pi] && params[_pi]->name) ? params[_pi]->name->name : NULL;
        }
        for (uint32_t _ci = 0; _ci < n_ct_param_preds; _ci++) {
            uint32_t _pi = ct_param_param_idx[_ci];
            if (_pi >= n_params) continue;
            rp[_pi] = ct_param_preds[_ci];
            rv[_pi] = ct_param_varnames[_ci];
        }
        b->refine_param_preds = rp;
        b->refine_param_vars  = rv;
        b->refine_param_names = rn;
        b->n_refine_params    = n_params;
    }
    b->closure_return_dispatches = expr_closure_return_dispatches(body);
    b->closure_return_dispatches_untyped = expr_closure_return_dispatches_untyped(body);
    /* lambda-thin-fn-result-read-as-fat: fn-value-fat-normalization stage 2
     * for a LAMBDA whose result is a concrete effect-free fn type, under the
     * guards the defn path uses.  Stage 2 marks such a result `boxed` inside
     * every fn-typed parameter annotation, so a consumer of
     * `(f : (fn [] (fn [int] int)))` -- and of a generic `(fn [] A)` at a
     * function -- reads `(f)` as a fat handle.  A defn producer returns one;
     * a captureless lambda returned its bare code pointer, and `((f) 41)`
     * dereferenced that pointer as a closure box: SIGSEGV.  Normalizing the
     * tail leaves and marking the result makes the lambda say, and be, what
     * its consumers read. */
    {
        Type *rft = fn_type.as.fn.result_full_type;
        if (body && rft && rft->kind == TY_FN && !rft->as.fn.boxed &&
            !fn_type.as.fn.result_fat &&
            rft->as.fn.result_kind != TY_FN &&
            rft->as.fn.result_kind != TY_UNKNOWN &&
            fn_result_type_is_fat_normalized(rft)) {
            elab_normalize_fn_tail_leaves(e, &body, rft, NULL);
            rft->as.fn.boxed = true;
        }
    }
    /* let-bound-sf-loses-outer-arg-type: see the defn path -- record whether the
     * lambda's return *value* is a fat closure box vs a thin fn pointer. */
    b->returns_boxed_closure = (body && body->type.kind == TY_FN &&
                                body->type.as.fn.boxed) ||
                               (fn_type.as.fn.result_full_type &&
                                fn_type.as.fn.result_full_type->kind == TY_FN &&
                                fn_type.as.fn.result_full_type->as.fn.boxed);
    /* fn-typed-tyvar-drops-a-capturing-closure: the defn path's
     * boxed-fn-typed-closure-return marking, mirrored.  A lambda declared
     * `: (fn [int] int)` whose body yields a CAPTURING closure returns a fat
     * box, but its declared result type stayed thin -- so when that lambda
     * is handed to `(defn app [A] [f : (fn [int] A) ...] : A (f x))`, `A`
     * was instantiated to the THIN fn type, the consumer bound the result as
     * a bare code pointer and called it thin, and the first call through it
     * faulted (SIGBUS/SIGSEGV).  Marking the declared result `boxed` here,
     * under the same guards the defn path uses, makes the type say what the
     * value is: `A := (fn [int] int){boxed}`, the consumer binds the int64
     * carrier and dispatches through the fat thunk protocol -- exactly what
     * the concretely-typed twin `app2` already did.  A captureless body is
     * untouched (thin value, thin type, consistent as before). */
    if (b->returns_boxed_closure &&
        fn_type.as.fn.result_full_type &&
        fn_type.as.fn.result_full_type->kind == TY_FN &&
        !fn_type.as.fn.result_full_type->as.fn.boxed &&
        fn_type.as.fn.result_full_type->as.fn.result_kind != TY_FN &&
        fn_type.as.fn.result_full_type->as.fn.result_kind != TY_UNKNOWN &&
        !fn_type.as.fn.result_fat) {
        fn_type.as.fn.result_full_type->as.fn.boxed = true;
    }

    /* Build FnDef */
    FnDef *fd = (FnDef *)arena_alloc(e->arena, sizeof(FnDef));
    memset(fd, 0, sizeof(FnDef));
    fd->binding = b;
    if (b) b->source_fn_def = fd;  /* MB1: Binding -> FnDef link */
    fd->params = params;
    fd->n_params = n_params;
    fd->body = body;
    fd->is_variadic = fn_is_variadic;  /* AR5: propagate variadic flag */
    if (fn_is_variadic) {
        extern bool g_has_variadics;
        g_has_variadics = true;  /* AR8: tell emit_module to include __tur_cons_of */
    }
    /* Mirror emit_fns.c:377's predicate so call sites referencing this
     * anonymous fn binding can see the same flag a defn binding would. */
    b->body_is_inline_c = (body && body->kind == EX_INLINE_C);
    b->returns_fresh_sum_box = elab_body_returns_fresh_sum_box(body);
    fd->closure = NULL;
    fd->inferred_effect_row = NULL;  /* must be NULL; effect_check_pass reads this */
    /* Phase 19: Store declared effect row (ERK_UNRESOLVED until PASS_EFFECT_ROW_INFER). */
    if (declared_effect_row_fn) {
        b->type.as.fn.effect_row = declared_effect_row_fn;
    }
    /* Store param types for codegen */
    fd->param_types = (Type *)arena_alloc(e->arena, n_params * sizeof(Type));
    for (uint32_t i = 0; i < n_params; i++) {
        fd->param_types[i] = params[i]->type;
    }
    /* Phase 15: Initialize constraints */
    constraint_set_init(&fd->constraints);
    /* LS2: lambdas carry no surface borrow-return lifetimes; record the bare
     * return kind and an empty lifetime context so the lifetime pass reads a
     * defined (not garbage) return Type. */
    lifetime_context_init(&fd->lifetime_ctx);
    fd->return_type = type_from_kind(return_kind);

    /* Create the FN_DEF expression that will be emitted at file scope */
    Expr *fn_def_expr = expr_new(e->arena, EX_FN_DEF, fn_type, call->span);
    fn_def_expr->as.fn_def_.fn = fd;

    if (n_captures == 0) {
        /* Register FN_DEF for file-scope emission regardless of scope level.
         * Without this, an anonymous fn used as an argument at top level would
         * return EX_VAR("__fn_N") but never emit the EX_FN_DEF that creates
         * the runtime closure. */
        elab_register_file_def(e, fn_def_expr);
        /* Return VAR reference to the function */
        Expr *var_expr = expr_new(e->arena, EX_VAR, fn_type, call->span);
        var_expr->as.var.binding = b;
        free(captures);
        return var_expr;
    } else {
        /* Phase 3: Closure with captures */
        /* Generate env struct name */
        char env_name_buf[32];
        snprintf(env_name_buf, sizeof(env_name_buf), "__env_%u", elab_fresh_id(e));
        const Symbol *env_name_sym = symtab_intern(e->st,
            strslice(env_name_buf, (uint32_t)strlen(env_name_buf)));
        
        /* Modify the FnDef to include env parameter as first parameter.  No hard
         * ceiling: the env-prepended arg-kind scratch below is arena-sized to
         * new_n_params (arbitrary-fn-arity). */
        uint32_t new_n_params = n_params + 1;
        
        /* Create new params array with env as first parameter */
        Binding **new_params = (Binding **)arena_alloc(e->arena, new_n_params * sizeof(Binding *));
        Type *new_param_types = (Type *)arena_alloc(e->arena, new_n_params * sizeof(Type));
        
        /* First param is env (void*) */
        char env_param_name[32];
        snprintf(env_param_name, sizeof(env_param_name), "__env_p_%u", elab_fresh_id(e));
        const Symbol *env_param_sym = symtab_intern(e->st,
            strslice(env_param_name, (uint32_t)strlen(env_param_name)));
        Binding *env_param_binding = binding_new(e, env_param_sym, TYPE_PTR_VOID, false, false, call->span);
        new_params[0] = env_param_binding;
        new_param_types[0] = TYPE_PTR_VOID;
        
        /* Copy existing params */
        for (uint32_t i = 0; i < n_params; i++) {
            new_params[i + 1] = params[i];
            new_param_types[i + 1] = params[i]->type;
        }
        
        /* Update FnDef with new params */
        fd->params = new_params;
        fd->n_params = new_n_params;
        fd->param_types = new_param_types;
        
        /* Update function type to include env parameter (arena-sized scratch,
         * no fixed cap). */
        TypeKind *new_arg_kinds = (TypeKind *)arena_alloc(e->arena, new_n_params * sizeof(TypeKind));
        new_arg_kinds[0] = TY_PTR_VOID;  /* env parameter */
        for (uint32_t i = 0; i < n_params; i++) {
            new_arg_kinds[i + 1] = param_kinds[i];
        }
        Type new_fn_type = type_fn(new_arg_kinds, new_n_params, return_kind);
        if (b->type.as.fn.arg_full_types) {
            Type **shifted = (Type **)arena_alloc(e->arena, new_n_params * sizeof(Type *));
            shifted[0] = NULL;
            for (uint32_t i = 0; i < n_params; i++) shifted[i + 1] = b->type.as.fn.arg_full_types[i];
            new_fn_type.as.fn.arg_full_types = shifted;
        }
        if (b->type.as.fn.result_full_type) {
            new_fn_type.as.fn.result_full_type = b->type.as.fn.result_full_type;
        }
        if (return_fn_type) {
            new_fn_type.as.fn.result_full_type = return_fn_type;
        }
        /* two-level-sf-closure / vec-get-typed-fat-closure-readback: shift the
         * ^fat param markers (and the ^fat result marker) into the env-prepended
         * thunk type.  type_fn() zero-inits arg_fat, so without this the thunk
         * type a *capturing* closure dispatches through loses its ^fat flags --
         * and a call site that passes a captureless lambda into one of the
         * closure's ^fat fn params reads arg_fat[env+i]=false, skips the
         * EX_FN_TO_FAT shim, and feeds a bare fn pointer to a fat-dispatch
         * consumer (slot-0 read of code bytes -> SEGV).  arg_full_types above is
         * shifted for exactly the same reason; arg_fat must travel with it. */
        for (uint32_t i = 0; i < n_params; i++) {
            FN_ARG_SET(new_fn_type.as.fn, i + 1, FA_FAT, FN_ARG_FLAG(b->type.as.fn, i, FA_FAT));
        }
        new_fn_type.as.fn.result_fat = b->type.as.fn.result_fat;
        /* closure-capture-escapes-linearity: carry the inherited obligation onto
         * the env-prepended thunk type.  type_fn() zero-inits copy_kind, and this
         * is the CAPTURING path -- the only one that can have inherited one -- so
         * without this the mark set above is dropped before any binding sees it. */
        new_fn_type.copy_kind = b->type.copy_kind;
        b->type = new_fn_type;
        fd->binding->type = new_fn_type;
        fn_def_expr->type = new_fn_type;
        
        /* Register the modified FN_DEF for file-scope emission */
        if (!at_top_level) {
            elab_register_file_def(e, fn_def_expr);
        }
        
        /* Create Closure struct */
        struct Closure *closure = (struct Closure *)arena_alloc(e->arena, sizeof(struct Closure));
        closure->fn = fd;
        closure->fat_captures_borrowed = false;   /* arena mem is not zeroed */
        /* Copy captures into arena memory so it shares the closure's lifetime. */
        Binding **arena_captures = (Binding **)arena_alloc(e->arena, n_captures * sizeof(Binding *));
        memcpy(arena_captures, captures, n_captures * sizeof(Binding *));
        closure->captures = arena_captures;
        closure->n_captures = n_captures;
        closure->env_name = env_name_sym;
        closure->is_shift_receiver = false;   /* arena mem is not zeroed */
        closure->is_effect_payload = false;
        closure->capture_drop_insts = NULL;   /* Model R #1b (arena is non-zeroing) */
        closure->capture_clone_insts = NULL;

        /* closure-drop-glue (Model R): make each OWNING capture participate in the
         * env's lifecycle so it is released when the env dies instead of leaking.
         * Gated on the experiment; the base language is unchanged.  Two kinds:
         *
         *  (a) type-honesty -- an OWNED `^fat` closure handle: MOVE it into the env
         *      (mark the source consumed), so the env is its sole owner and a second
         *      capture is a compile-time use-after-move, not a double-free.  The
         *      is_fat drop-glue walk releases it via TUR_CLOSURE_DROP.
         *
         *  (b) Drop typeclass -- a capture whose type implements Drop (e.g. an owned
         *      refcounted String): record its Drop instance (and Clone, if any).  A
         *      Drop+Clone type is RETAINed at capture / released at env death
         *      (refcount balances, aliasing-safe -- like the rc walk); a Drop-only
         *      (move-only) type is consumed at capture and released once.
         *
         * The letrec self-capture (env storing its own pointer) is not a transfer
         * and is skipped. */
        {
            const Symbol *drop_name = intern_cstr(e->st, "Drop");
            TypeClass *drop_tc = typeclass_env_lookup_typeclass(&e->typeclass_env, drop_name);
            struct TypeClassInstance **drops = NULL;
            for (uint32_t ci = 0; ci < n_captures; ci++) {
                Binding *cap = arena_captures[ci];
                if (!cap || cap->is_global) continue;
                bool self_cap = cap->closure_fn_binding &&
                                cap->closure_fn_binding == fd->binding;
                if (self_cap) continue;
                /* (a) ^fat owned closure handle -> move (is_fat walk releases it). */
                if (cap->is_fat) {
                    (void)binding_mark_moved(cap, call->span);
                    continue;
                }
                /* rc/weak/ref are already released by the type-kind drop-glue arms;
                 * do not also route them through Drop (would double-release). */
                if (cap->type.kind == TY_RC || cap->type.kind == TY_WEAK ||
                    cap->type.kind == TY_REF || cap->type.kind == TY_LREF)
                    continue;
                /* (b) Drop-implementing capture (e.g. an owned String).  MOVE it:
                 * an opaque Drop type has NO scope-exit auto-drop (release is
                 * manual), so retaining a second owner would leak the source (which
                 * nothing releases).  Moving makes the closure the SOLE owner -- the
                 * drop-glue releases it exactly once via the Drop instance, and a
                 * second capture is a compile-time use-after-move. */
                struct TypeClassInstance *di = drop_tc
                    ? typeclass_env_lookup_instance(&e->typeclass_env, drop_tc, &cap->type, 1)
                    : NULL;
                if (!di) continue;
                if (!drops) {
                    drops = (struct TypeClassInstance **)arena_alloc(
                        e->arena, n_captures * sizeof(struct TypeClassInstance *));
                    for (uint32_t k = 0; k < n_captures; k++) drops[k] = NULL;
                }
                drops[ci] = di;
                (void)binding_mark_moved(cap, call->span);
            }
            closure->capture_drop_insts  = drops;
            closure->capture_clone_insts = NULL;   /* unused: Drop captures MOVE */
        }

        /* Store closure reference in FnDef for codegen */
        fd->closure = closure;
        
        /* Create EX_CLOSURE expression */
        /* CRU Phase 3 / Option B (B-1): a capturing closure is a first-class
         * closure *value* -- a fat box { thunk, env... } -- typed as a boxed
         * TY_FN carrying the lambda's user-facing signature (param_kinds /
         * return_kind, i.e. WITHOUT the prepended env param).  A boxed TY_FN
         * shares TY_PTR_VOID's C carrier (a void* holding the box) and is
         * type_eq-interchangeable with it, so it flows through every legacy
         * :ptr<void> closure sink unchanged; the only new behavior is that a
         * direct call on a value statically typed boxed TY_FN dispatches
         * through the fat protocol for all arities (emit_expr.c).  See
         * docs/archive/history/closure-first-class-type-plan.md. */
        TypeKind *clo_arg_kinds = (TypeKind *)arena_alloc(e->arena, (n_params ? n_params : 1) * sizeof(TypeKind));
        for (uint32_t i = 0; i < n_params; i++) clo_arg_kinds[i] = param_kinds[i];
        Type clo_ty = type_fn(clo_arg_kinds, n_params, return_kind);
        clo_ty.as.fn.boxed = true;
        /* r7rs-lang-plan R6: the closure VALUE's type carries the rest marker
         * too.  Without it a capturing variadic closure was typed as a fixed
         * arity: a static call passed the surplus unpacked, its `any` box id
         * was the fixed signature's (so a dynamic call could not pack for
         * it), and `(apply f xs)` on a Scheme closure over its environment --
         * a continuation wrapper, a parameter object -- read a bare word as
         * its rest chain. */
        clo_ty.as.fn.is_variadic    = fn_is_variadic;
        clo_ty.as.fn.rest_kind      = fn_rest_kind;
        clo_ty.as.fn.rest_full_type = fn_rest_full_type;
        /* curried-fn-typed-param: preserve the closure's full result type so a
         * closure that *returns a function* keeps the inner (fn ...) type on
         * its first-class value.  Without this, a let-bound closure value
         * applied as ((f 1) 2) recovers a bare result kind for (f 1) and the
         * chained application is rejected as not callable. */
        if (return_full_type) {
            clo_ty.as.fn.result_full_type = return_full_type;
        }
        if (return_fn_type) {
            clo_ty.as.fn.result_full_type = return_fn_type;
        }
        /* closure-capture-escapes-linearity: the closure VALUE is what a `let`
         * binds and what every downstream substructural check inspects, so the
         * inherited obligation has to reach this type -- the thunk types above
         * are internal.  This is the last of the three fn types elab_fn builds
         * on the capturing path; all three are freshly type_fn()'d, so the mark
         * has to be restated at each. */
        if (capture_consumes_linear)      clo_ty.copy_kind = CK_LINEAR;
        else if (capture_consumes_unique) clo_ty.copy_kind = CK_UNIQUE;
        Expr *closure_expr = expr_new(e->arena, EX_CLOSURE, clo_ty, call->span);
        closure_expr->as.closure_.closure = closure;

        free(captures);
        return closure_expr;
    }
}

/* jit-ffi F4 follow-on: validate a record named in an extern-c slot as a
 * by-value C aggregate.  Same admission rule as call-ptr's signature vector
 * (elab_unsafe.c call_ptr_aggregate_type): only a single-constructor,
 * non-:heap, non-parametric record has the C struct layout the declaration
 * claims.  Returns false with a diagnostic emitted. */
static bool extern_c_aggregate_ok(const Type *t, Span span) {
    const AdtDef *def = t->as.adt_.def;
    if (def && def->n_ctors == 1 && def->ctors[0]->is_record &&
        !def->is_heap && def->n_type_params == 0 &&
        def->ctors[0]->n_fields > 0 && adt_is_byvalue_product(def))
        return true;
    diag_emit(DIAG_ERROR, span,
              "extern-c: '%s' cannot cross the C boundary by value -- only "
              "a single-constructor, non-:heap, non-parametric record with "
              "at least one field has a by-value C struct layout",
              (def && def->name) ? def->name : "this type");
    return false;
}

/* Phase 2: extern-c — (extern-c name [param1 param2 ...] : return-type)
 * Declares an external C function. For phase 2, we don't support capture.
 * Example: (extern-c printf [^cstr fmt] : int)
 * The ^ prefix on a param indicates it's a C type annotation (not yet implemented).
 * For now, all params are treated as int64_t or pointers.
 * 
 * Supported annotations:
 *   ^cstr - const char* (string)
 *   ^ptr  - void* (pointer)
 */
Expr *elab_extern_c(Elab *e, const Form *call) {
    /* Minimum: (extern-c name [params...] : ret-type) */
    if (call->as.list.len < 4) {
        diag_emit(DIAG_ERROR, call->span,
                  "extern-c requires (extern-c name [params...] : ret-type)");
        return NULL;
    }

    /* Parse name */
    Form *name_f = call->as.list.items[1];
    if (name_f->tag != F_SYM) {
        diag_emit(DIAG_ERROR, name_f->span, "extern-c name must be a symbol");
        return NULL;
    }
    if (scope_lookup(e->scope, name_f->as.sym)) {
        diag_emit(DIAG_ERROR, name_f->span,
                  "extern-c: '%s' is already defined", name_f->as.sym->name);
        return NULL;
    }

    /* Parse param vector */
    Form *params_f = call->as.list.items[2];
    if (params_f->tag != F_VEC) {
        diag_emit(DIAG_ERROR, params_f->span,
                  "extern-c: parameter list must be a vector [name1 name2 ...]");
        return NULL;
    }

    /* Parse params - support type annotations: [name :type ...]
     * e.g. (extern-c getenv [key :cstr] :cstr) */
    /* Per-param scratch arena-sized to the param-vector length (a safe upper
     * bound), not a fixed [MAX_FN_ARITY] stack array -- an extern-c shim may
     * mirror a wide C signature with any number of parameters. */
    uint32_t pcap = params_f->as.list.len ? params_f->as.list.len : 1;
    Binding **params = NULL;
    uint32_t n_params = 0;
    TypeKind *param_kinds = (TypeKind *)arena_alloc(e->arena, pcap * sizeof(TypeKind));
    /* jit-ffi F4 follow-on: a `[v : SomeRecord]` annotation names a by-value
     * aggregate, whose AdtDef must survive into ec->param_types for the
     * prototype emitter and the interpreter's thunk marshaller.  Slots stay
     * TY_UNKNOWN here and are back-filled from param_kinds after the loop,
     * so only a validated record annotation carries a full type. */
    Type *param_full = (Type *)arena_alloc(e->arena, pcap * sizeof(Type));
    for (uint32_t pi = 0; pi < pcap; pi++)
        param_full[pi] = type_from_kind(TY_UNKNOWN);
    /* A#1: ^fat marks the next extern-c parameter as a fat-closure consumer. */
    bool next_param_fat = false;

    for (uint32_t i = 0; i < params_f->as.list.len; i++) {
        Form *p = params_f->as.list.items[i];
        /* A#1: ^fat annotation applies to the next parameter.  Intercept before
         * the generic ^ctype handling below (which would misparse "fat"). */
        if (p->tag == F_SYM && p->as.sym == e->sym_caret_fat) {
            next_param_fat = true;
            continue;
        }
        /* Handle type annotation keyword or F_TYPE_ANN after the previous param */
        if (p->tag == F_KEYWORD || p->tag == F_TYPE_ANN) {
            if (n_params == 0) {
                diag_emit(DIAG_ERROR, p->span,
                          "extern-c: type annotation without preceding parameter");
                return NULL;
            }
            TypeKind pk;
            if (p->tag == F_TYPE_ANN) {
                Type *ann = (p->as.list.len > 0)
                    ? type_expr_from_form(e, p->as.list.items[0], NULL, NULL, NULL, 0)
                    : NULL;
                if (!ann) return NULL;
                pk = ann->kind;
                /* jit-ffi F4 follow-on: a record annotation is a by-value
                 * aggregate parameter -- keep the full type (the AdtDef is
                 * what the prototype emitter and the interpreter's layout
                 * engine read).  Anything else keeps the kind-only path. */
                if (pk == TY_ADT) {
                    if (!extern_c_aggregate_ok(ann, p->span)) return NULL;
                    param_full[n_params - 1] = *ann;
                }
            } else {
                const Symbol *kw = p->as.sym;
                pk = typekind_from_symbol(kw->name);
                if (pk == TY_UNKNOWN) {
                    /* Legacy fallback names */
                    if (kw->len == 3 && memcmp(kw->name, "ptr", 3) == 0) pk = TY_PTR_VOID;
                    else if (kw->len == 4 && memcmp(kw->name, "void", 4) == 0) pk = TY_NIL;
                    else if (kw->len == 9 && memcmp(kw->name, "ptr<void>", 9) == 0) pk = TY_PTR_VOID;
                    else {
                        diag_emit(DIAG_ERROR, p->span,
                                  "extern-c: unsupported parameter type :%s", kw->name);
                        return NULL;
                    }
                }
            }
            param_kinds[n_params - 1] = pk;
            params[n_params - 1]->type = (param_full[n_params - 1].kind == TY_ADT)
                                             ? param_full[n_params - 1]
                                             : type_from_kind(pk);
            continue;
        }
        if (p->tag != F_SYM) {
            diag_emit(DIAG_ERROR, p->span,
                      "extern-c: parameter must be a symbol");
            return NULL;
        }
        /* No hard parameter-count ceiling: an extern-c shim may mirror a wide
         * C signature with arbitrarily many parameters (arbitrary-fn-arity). */

        /* Handle ^type prefix: ^int, ^ptr<void>, etc.
         * When a symbol starts with '^', the rest is a C type annotation and the
         * NEXT symbol in the vector is the parameter name. */
        if (p->as.sym->len > 1 && p->as.sym->name[0] == '^') {
            const char *tname = p->as.sym->name + 1;
            size_t tlen = p->as.sym->len - 1;
            TypeKind ck;
            if (tlen == 3 && memcmp(tname, "int", 3) == 0) ck = TY_INT;
            else if (tlen == 4 && memcmp(tname, "bool", 4) == 0) ck = TY_BOOL;
            else if (tlen == 4 && memcmp(tname, "cstr", 4) == 0) ck = TY_CSTR;
            else if (tlen == 3 && memcmp(tname, "ptr", 3) == 0) ck = TY_PTR_VOID;
            else if (tlen == 9 && memcmp(tname, "ptr<void>", 9) == 0) ck = TY_PTR_VOID;
            else if (tlen == 4 && memcmp(tname, "void", 4) == 0) ck = TY_NIL;
            else ck = TY_INT; /* unknown ^type: default to int */
            /* Peek at next element to get the param name */
            i++;
            if (i >= params_f->as.list.len) {
                /* ^type at end with no following name — create anonymous param */
                if (n_params == 0) {
                    params = (Binding **)arena_alloc(e->arena, pcap * sizeof(Binding *));
                }
                param_kinds[n_params] = ck;
                /* Use the ^type symbol itself as a placeholder name */
                Binding *b = binding_new(e, p->as.sym, type_from_kind(ck), false, false, p->span);
                b->is_param = true;
                params[n_params++] = b;
                break;
            }
            Form *name_f = params_f->as.list.items[i];
            if (name_f->tag != F_SYM) {
                diag_emit(DIAG_ERROR, name_f->span,
                          "extern-c: expected parameter name after ^type");
                return NULL;
            }
            if (n_params == 0) {
                params = (Binding **)arena_alloc(e->arena, pcap * sizeof(Binding *));
            }
            param_kinds[n_params] = ck;
            Binding *b = binding_new(e, name_f->as.sym, type_from_kind(ck), false, false, name_f->span);
            b->is_param = true;
            b->is_fat = next_param_fat; next_param_fat = false;
            params[n_params++] = b;
            continue;
        }

        param_kinds[n_params] = TY_INT;
        Binding *b = binding_new(e, p->as.sym, TYPE_INT, false, false, p->span);
        b->is_param = true;
        b->is_fat = next_param_fat; next_param_fat = false;
        if (n_params == 0) {
            params = (Binding **)arena_alloc(e->arena, pcap * sizeof(Binding *));
        }
        params[n_params++] = b;
    }

    /* Parse return type annotation; optionally skip #{Effect...} advisory row */
    uint32_t ret_idx = 3;
    if (ret_idx < call->as.list.len && call->as.list.items[ret_idx]->tag == F_MAP) {
        /* #{...} effect-row annotation: skip silently (advisory in v1) */
        warn_legacy_fx_row(call->as.list.items[ret_idx]);
        ret_idx++;
    }
    if (ret_idx >= call->as.list.len) {
        diag_emit(DIAG_ERROR, call->span,
                  "extern-c requires (extern-c name [params...] : ret-type)");
        return NULL;
    }
    Form *ret_f = call->as.list.items[ret_idx];
    if (ret_f->tag != F_KEYWORD && ret_f->tag != F_TYPE_ANN) {
        diag_emit(DIAG_ERROR, ret_f->span,
                  "extern-c: return type must be a keyword (:int, :bool, :void, :cstr, :ptr)");
        return NULL;
    }

    TypeKind return_kind;
    Type return_full = type_from_kind(TY_UNKNOWN);
    if (ret_f->tag == F_TYPE_ANN) {
        Type *ann = (ret_f->as.list.len > 0)
            ? type_expr_from_form(e, ret_f->as.list.items[0], NULL, NULL, NULL, 0)
            : NULL;
        if (!ann) return NULL;
        return_kind = ann->kind;
        /* jit-ffi F4 follow-on: an aggregate return keeps its full type, so
         * call sites type the result as the record (result_full_type below)
         * and the interpreter can rebuild it from the returned bytes. */
        if (return_kind == TY_ADT) {
            if (!extern_c_aggregate_ok(ann, ret_f->span)) return NULL;
            return_full = *ann;
        }
    } else {
        return_kind = typekind_from_symbol(ret_f->as.sym->name);
        if (return_kind == TY_UNKNOWN) {
            /* Legacy fallback names */
            const Symbol *kw = ret_f->as.sym;
            if (kw->len == 4 && memcmp(kw->name, "void", 4) == 0) return_kind = TY_NIL;
            else if (kw->len == 3 && memcmp(kw->name, "ptr", 3) == 0) return_kind = TY_PTR_VOID;
            else if (kw->len == 9 && memcmp(kw->name, "ptr<void>", 9) == 0) return_kind = TY_PTR_VOID;
            else {
                diag_emit(DIAG_ERROR, ret_f->span,
                          "extern-c: unsupported return type :%s", kw->name);
                return NULL;
            }
        }
    }

    /* Create function type */
    Type fn_type = type_fn(param_kinds, n_params, return_kind);
    /* A#1: propagate ^fat parameter flags into the fn type for call-site shimming. */
    for (uint32_t i = 0; i < n_params; i++) {
        if (params[i]->is_fat) FN_ARG_SET(fn_type.as.fn, i, FA_FAT, true);
    }
    /* jit-ffi F4 follow-on: an aggregate return needs the full record type on
     * the fn type -- elab_call types the call result from result_full_type,
     * which is what makes `(.field (my-extern ...))` resolve. */
    if (return_full.kind == TY_ADT) {
        Type *rft = (Type *)arena_alloc(e->arena, sizeof(Type));
        *rft = return_full;
        fn_type.as.fn.result_full_type = rft;
    }

    /* Create a binding for the extern-c function so it can be looked up and called */
    Binding *b = binding_new(e, name_f->as.sym, fn_type, false, true, call->span);
    b->is_extern_c = true;  /* ER6: mark as extern-c for effect inference */
    /* The extern-c C name is produced by the LEGACY fold (raw_name_for_binding /
     * elab_mangle_binding_name special-case is_extern_c), matching the prototype
     * emitted via mangle_field_name. Not c_export_name and never the injective
     * scheme: extern-c names are real C symbols (`tur_hamt_new`, `tvar/new` ->
     * `tvar_new`) that must agree across prototype, call sites, and inline-C. */
    scope_add(&e->global, b);

    /* CT4: Parse optional :pre and :post clauses after the return type annotation.
     * Syntax: (extern-c name [params...] :ret-type :pre pred :post pred) */
    const Form *ec_pre_form  = NULL;
    const Form *ec_post_form = NULL;
    for (uint32_t ci = ret_idx + 1; ci < call->as.list.len; ci++) {
        const Form *maybe_kw = call->as.list.items[ci];
        if (maybe_kw->tag == F_KEYWORD && maybe_kw->as.sym == e->kw_pre) {
            if (ci + 1 < call->as.list.len) {
                ec_pre_form = call->as.list.items[++ci];
            }
        } else if (maybe_kw->tag == F_KEYWORD && maybe_kw->as.sym == e->kw_post) {
            if (ci + 1 < call->as.list.len) {
                ec_post_form = call->as.list.items[++ci];
            }
        }
    }

    /* Create ExternC declaration */
    ExternC *ec = (ExternC *)arena_alloc(e->arena, sizeof(ExternC));
    ec->c_name = name_f->as.sym;
    ec->binding = b;
    ec->return_type = (return_full.kind == TY_ADT)
                          ? return_full
                          : type_from_kind(return_kind);
    ec->param_types = (Type *)arena_alloc(e->arena, n_params * sizeof(Type));
    for (uint32_t i = 0; i < n_params; i++) {
        /* jit-ffi F4 follow-on: a validated record annotation carries its
         * full type (AdtDef and all); every other slot stays kind-only. */
        ec->param_types[i] = (param_full[i].kind == TY_ADT)
                                 ? param_full[i]
                                 : type_from_kind(param_kinds[i]);
    }
    ec->n_params = n_params;
    ec->is_variadic = false;
    /* CT4: store pre/post predicates for contract check emission */
    ec->pre_cond  = ec_pre_form;
    ec->post_cond = ec_post_form;

    Expr *out = expr_new(e->arena, EX_EXTERN_C, fn_type, call->span);
    out->as.extern_c_.ext = ec;

    /* params was allocated with arena_alloc, so no need to free */
    return out;
}

/* True when `def_form` sits in STATEMENT position at file scope: it is the
 * top-level form itself, or an item of a top-level `do` chain.
 *
 * `e->scope == &e->global` cannot answer this on its own -- it is equally true
 * of a `def` buried in a top-level expression, e.g. `(if c (def x 1) (def y 2))`
 * or `(when c (def x 1))`.  Those elaborate as GLOBALS and pass the type
 * checker, but codegen emits them as locals of the enclosing statement, so any
 * later reference dies in the emitted C with `'x_1326' undeclared`.  Nothing
 * caught that: the "nothing to scope over" diagnostic below fired for these
 * only when the synthesized-main fold happened to relocate the statement into a
 * function body, which in turn happened only when the file declared no `main`
 * of its own.  Adding `(defn main [] : int 0)` to the same file made the error
 * disappear and the miscompile reappear.
 *
 * `do` is threaded through because it is a statement sequence, not an
 * expression context, and the diagnostic below advertises it as a valid body.
 *
 * When e->toplevel_stmt is NULL we are not in file-scope elaboration at all
 * (an imported module, a REPL form, an interpreter session); return true so the
 * scope test alone decides, exactly as before.
 * See docs/archive/turi-toplevel-expr-subforms-elaborate-in-global-scope.md. */
static bool def_form_reachable_as_stmt(const Elab *e, const Form *from,
                                       const Form *target, int depth) {
    if (!from || depth > 32) return false;
    if (from == target) return true;
    if (from->tag != F_LIST || from->as.list.len == 0) return false;
    const Form *head = from->as.list.items[0];
    if (!head || head->tag != F_SYM || head->as.sym != e->sym_do) return false;
    for (uint32_t i = 1; i < from->as.list.len; i++)
        if (def_form_reachable_as_stmt(e, from->as.list.items[i], target,
                                       depth + 1))
            return true;
    return false;
}

static bool def_form_is_statement_position(const Elab *e, const Form *def_form) {
    if (!e->toplevel_stmt) return true;
    return def_form_reachable_as_stmt(e, e->toplevel_stmt, def_form, 0);
}

Expr *elab_def(Elab *e, const Form *call) {
    /* def/define consolidation D1: `define` is a spelling of `def`, so every
     * diagnostic below quotes the head symbol the user actually wrote rather
     * than a fixed "def". */
    const char *kw = "def";
    if (call->as.list.len >= 1 && call->as.list.items[0]->tag == F_SYM)
        kw = call->as.list.items[0]->as.sym->name;

    /* Syntax: (def [^mut|^persistent|^deprecated ["msg"]]* name [: type] init)
     *
     * D4/§3.5(b): annotations are consumed in any order (matching `let`), and
     * every annotation this form does NOT support is rejected by name with a
     * reason.  Silently dropping one is the outcome D4 exists to prevent. */
    uint32_t name_idx = 1;
    bool is_persistent = false;
    bool is_mut = false;
    bool is_atomic = false;
    bool is_thread_local = false;
    bool is_deferred_init = false;
    bool is_deprecated_attr = false;
    const char *deprecation_msg = NULL;

    while (name_idx < call->as.list.len &&
           call->as.list.items[name_idx]->tag == F_SYM) {
        Form *cur = call->as.list.items[name_idx];
        const Symbol *s = cur->as.sym;

        if (s == e->sym_caret_persistent) { is_persistent = true; name_idx++; continue; }

        /* toplevel-def-initializers-run-before-toplevel-expressions:
         * `^deferred-init` -- the initializer gives the binding its type
         * here but runs where `(__tur-deferred-init__ name)` stands, which
         * the Scheme lowering puts in the program body at the define's
         * source position.  Internal to that lowering. */
        if (s == e->sym_caret_deferred_init) { is_deferred_init = true; name_idx++; continue; }

        /* G4b: `^thread-local` -- each thread gets its own copy, initialized
         * on first access by running the initializer on that thread. */
        if (s == e->sym_caret_thread_local) {
            is_thread_local = true; name_idx++; continue;
        }

        /* G4a: `^atomic` -- reads and writes of this global are sequentially
         * consistent.  Scalars only; the type check happens below, once the
         * initializer has been elaborated and the type is known. */
        if (s == e->sym_caret_atomic) {
            is_atomic = true; name_idx++; continue;
        }

        /* D4: ^mut on a global is meaningful -- static storage that `set!` may
         * write.  Without it, `(set! g v)` told the user to "use ^mut at the
         * binding site" and the binding site rejected ^mut: a dead end. */
        if (s == e->sym_caret_mut) { is_mut = true; name_idx++; continue; }

        /* F4: ^deprecated ["message"] */
        if (s == e->sym_caret_deprecated) {
            is_deprecated_attr = true;
            name_idx++;
            if (name_idx < call->as.list.len &&
                call->as.list.items[name_idx]->tag == F_STR) {
                Form *msg_f = call->as.list.items[name_idx];
                char *msg_buf = (char *)arena_alloc(e->arena, msg_f->as.s.len + 1);
                memcpy(msg_buf, msg_f->as.s.p, msg_f->as.s.len);
                msg_buf[msg_f->as.s.len] = '\0';
                deprecation_msg = msg_buf;
                name_idx++;
            }
            continue;
        }

        /* D4: the substructural annotations are rejected, each for its own
         * reason.  They are not "unsupported yet" -- they are unenforceable on
         * a global, and a half-working check is worse than none.
         *
         * ^linear / ^relevant are verified at SCOPE EXIT (elab_let's ST1 pass,
         * elab_forms.c) -- "was this consumed / used by the time its scope
         * ended".  The global scope has no exit, so there is no point at which
         * the obligation could be discharged or reported.
         *
         * ^affine is checked per use site (elab_toplevel.c), which WOULD fire
         * on a global -- but on elaboration order across the whole program, not
         * on anything the program does.  Two functions naming the global would
         * be rejected even if only one is ever called.  That is the
         * silently-half-working outcome, so it is refused outright.
         *
         * ^unique asserts no aliasing; a global is a name every function in the
         * program can reach, so uniqueness is not a property it can have. */
        if (s == e->sym_caret_linear || s == e->sym_caret_relevant) {
            diag_emit(DIAG_ERROR, cur->span,
                      "%s: '%s' cannot be enforced on a top-level binding -- the "
                      "obligation is checked when the binding's scope ends, and a "
                      "global's scope never ends. Bind it in a body (`let`, or a "
                      "`%s` inside a `defn`/`fn`/`do`) where the check has a "
                      "scope exit to run at",
                      kw, s->name, kw);
            return NULL;
        }
        if (s == e->sym_caret_affine) {
            diag_emit(DIAG_ERROR, cur->span,
                      "%s: '%s' cannot be enforced on a top-level binding -- "
                      "\"used at most once\" would count elaboration sites across "
                      "the whole program, not uses at run time, so two functions "
                      "naming it would be rejected even if only one ever runs. "
                      "Bind it in a body (`let`, or a `%s` inside a "
                      "`defn`/`fn`/`do`) instead",
                      kw, s->name, kw);
            return NULL;
        }
        if (s == e->sym_caret_unique) {
            diag_emit(DIAG_ERROR, cur->span,
                      "%s: '%s' has no meaning on a top-level binding -- a global "
                      "is a name every function in the program can reach, so it "
                      "cannot be unaliased. Bind it in a body (`let`, or a `%s` "
                      "inside a `defn`/`fn`/`do`), or pass it as a `^unique` "
                      "parameter",
                      kw, s->name, kw);
            return NULL;
        }

        /* Any other caret-led symbol in annotation position is a typo or an
         * annotation that belongs to some other form; say so rather than
         * falling through to the generic arity diagnostic, which never
         * mentions the annotation at all. */
        if (s->len > 1 && s->name[0] == '^') {
            diag_emit(DIAG_ERROR, cur->span,
                      "%s: unknown annotation '%s'; %s accepts ^mut, ^persistent, "
                      "and ^deprecated \"msg\"",
                      kw, s->name, kw);
            return NULL;
        }

        break;  /* not an annotation -- this is the binding name */
    }

    Form *init_f = NULL;
    Form *type_f = NULL;

    if (name_idx + 3 == call->as.list.len) {
        Form *maybe_ann = call->as.list.items[name_idx + 1];
        if (maybe_ann->tag == F_KEYWORD || maybe_ann->tag == F_TYPE_ANN) {
            type_f = maybe_ann;
            init_f = call->as.list.items[name_idx + 2];
        }
    }

    if (!init_f) {
        if (name_idx + 2 == call->as.list.len) {
            init_f = call->as.list.items[name_idx + 1];
        } else {
            diag_emit(DIAG_ERROR, call->span,
                      "%s takes (%s [^mut] [^persistent] [^deprecated \"msg\"] "
                      "name [: type] init)", kw, kw);
            return NULL;
        }
    }

    Form *name_f = call->as.list.items[name_idx];
    if (name_f->tag != F_SYM) {
        diag_emit(DIAG_ERROR, name_f->span, "%s name must be a symbol", kw);
        return NULL;
    }
    /* D1/§3.3: a body window was already rewritten to a `let` by
     * splice_internal_defines, so reaching here in non-global scope means an
     * expression position -- an `if` branch, a call argument, a `cond` test --
     * where the binding would have nothing to scope over. */
    if (e->scope != &e->global) {
        diag_emit(DIAG_ERROR, call->span,
                  "`%s` here has nothing to scope over: a `%s` in an expression "
                  "position binds a name no later form can see. Put it at the top "
                  "level, or in a body (`do`, `fn`, `let`, `when`, `while`), or "
                  "use `let` if you meant a binding local to this expression",
                  kw, kw);
        return NULL;
    }
    /* Same defect, different advice: at file scope the user DID put it at the
     * top level, just inside an expression there.  Telling them to "put it at
     * the top level" would be nonsense, so name the enclosing expression as the
     * problem and point at the `do` form that actually works. */
    if (!def_form_is_statement_position(e, call)) {
        diag_emit(DIAG_ERROR, call->span,
                  "`%s` is inside a top-level expression, which cannot bind a "
                  "global: it elaborates as one but is emitted as a local, so a "
                  "later reference fails to compile. Lift it out to its own "
                  "top-level form, or wrap the statements in `(do ...)` -- a "
                  "top-level `do` is a statement sequence and a `%s` inside one "
                  "does bind a global",
                  kw, kw);
        return NULL;
    }
    /* r7rs-procedure-body-forward-reference: the binding Pass 1 declared
     * for this def, if any (elab_pre_declare_any_mut_def); the bodies above
     * already refer to it, so it is filled in below rather than shadowed. */
    Binding *fwd = NULL;
    {
        Binding *prior = scope_lookup(e->scope, name_f->as.sym);
        if (prior && prior->is_forward_def && prior->name == name_f->as.sym) { fwd = prior; prior = NULL; }
        /* PS4: an earlier REPL/playground turn's global is replaced -- the new
         * binding below shadows it -- so re-running a program that defines
         * `x` does not fail on the `x` the last run left.  A duplicate within
         * one turn (or one file) is still an error. */
        if (prior && !elab_prior_turn_global(e, prior)) {
            diag_emit(DIAG_ERROR, name_f->span,
                      "%s: '%s' is already defined", kw, name_f->as.sym->name);
            return NULL;
        }
    }

    Type *declared_type = NULL;
    if (type_f) {
        declared_type = type_expr_from_form(e, type_f, NULL, NULL, NULL, 0);
        if (!declared_type) return NULL;
    }

    Type *saved_expected = e->expected_type;
    if (declared_type) e->expected_type = declared_type;
    Expr *init = elab_form(e, init_f);
    e->expected_type = saved_expected;
    if (!init) return NULL;

    if (declared_type) {
        /* any-coercion-not-driven-by-expected-type: a `: any` declaration is a
         * widening request, and this form hand-builds its EX_ASCRIBE rather than
         * going through elab_ascribe -- so it missed the coercion elab_ascribe
         * does, whose own comment says why relabelling alone is not enough:
         * "It must heap-box the value (EX_UNION_INJECT) ... NOT merely relabel
         * its static type."
         *
         * `(def g : any 42)` therefore declared a `tur_tagged_t` slot and
         * assigned a bare `long int` to it -- "incompatible types when
         * assigning", a cc error against generated code rather than a
         * diagnostic.  Widen FIRST, so the size-matched EX_REINTERPRET below
         * cannot fire on the `any` pair either. */
        if (declared_type->kind == TY_ANY && init->type.kind != TY_ANY &&
            init->type.kind != TY_NEVER) {
            init = elab_coerce_to_any(e, init);
            if (!init) return NULL;
        }

        /* Align with elab_ascribe */
        if (declared_type->kind == TY_APP && init->kind == EX_CALL &&
            init->as.call_.ctor &&
            (init->type.kind == TY_ADT || init->type.kind == TY_APP)) {
            AdtDef *asc_def = type_adt_app_def(declared_type);
            AdtDef *inner_def = (init->type.kind == TY_ADT)
                ? init->type.as.adt_.def : type_adt_app_def(&init->type);
            if (asc_def && asc_def == inner_def)
                init->type = *declared_type;
        }

        TypeKind src_kind = init->type.kind;
        TypeKind dst_kind = declared_type->kind;
        if (src_kind != dst_kind) {
            int src_size = type_size_bytes(src_kind);
            int dst_size = type_size_bytes(dst_kind);
            if (src_size > 0 && dst_size > 0 && src_size == dst_size) {
                Expr *r = expr_new(e->arena, EX_REINTERPRET, *declared_type, call->span);
                r->as.reinterpret_.expr = init;
                r->as.reinterpret_.source_kind = src_kind;
                r->as.reinterpret_.target_kind = dst_kind;
                init = r;
            }
        }

        Expr *asc = expr_new(e->arena, EX_ASCRIBE, *declared_type, call->span);
        asc->as.ascribe_.inner = init;

        if (declared_type->kind == TY_FN &&
            (src_kind == TY_INT || src_kind == TY_PTR_VOID)) {
            asc->type.as.fn.boxed = true;
        }
        init = asc;
    }

    /* Phase 5: ref<T> is scope-local only — disallow at top-level def */
    if (init->type.kind == TY_REF) {
        diag_emit(DIAG_ERROR, call->span,
                  "%s: ref<T> values must be scope-local; use let instead of %s for '%s'",
                  kw, kw, name_f->as.sym->name);
        return NULL;
    }

    /* G4b: `^thread-local` and `^atomic` are mutually exclusive.  A per-thread
     * copy is unshared by construction, so making its accesses atomic buys
     * nothing and would suggest a synchronisation that is not happening. */
    if (is_thread_local && is_atomic) {
        diag_emit(DIAG_ERROR, call->span,
                  "%s: '^thread-local' and '^atomic' do not combine -- a "
                  "per-thread copy is unshared, so its accesses need no "
                  "synchronisation",
                  kw);
        return NULL;
    }
    /* G4b/§13.7: a `^thread-local` initializer may not reference another
     * `^thread-local`.  Per-thread initialization order is source order, and
     * allowing cross-references would make that order observable and therefore
     * load-bearing, for no demonstrated need.  Loosenable later; rejecting is
     * the direction that stays correct if the order ever changes. */
    if (is_thread_local && init_f) {
        const Symbol *bad = tl_init_references_thread_local(e, init_f, 0);
        if (bad) {
            diag_emit(DIAG_ERROR, init_f->span,
                      "%s: a '^thread-local' initializer may not reference "
                      "another '^thread-local' ('%s') -- per-thread "
                      "initialization order would become observable; read it "
                      "inside a function instead",
                      kw, bad->name);
            return NULL;
        }
    }

    /* G4a: `^atomic` requires an explicit `^mut` (decided 2026-08-05).  An
     * atomic global nothing may write is just a global, and making `^atomic`
     * confer write permission would make it the only annotation in the language
     * that does so as a side effect. */
    if (is_atomic && !is_mut) {
        diag_emit(DIAG_ERROR, call->span,
                  "%s: '^atomic' does not by itself allow writes -- write "
                  "`(%s ^atomic ^mut %s ...)`; without `^mut` an atomic global "
                  "is just a global",
                  kw, kw, name_f->as.sym->name);
        return NULL;
    }
    /* G4a: scalars only.  A wider value cannot be loaded or stored in one
     * machine operation, and pretending otherwise would ship exactly the
     * looks-atomic-and-is-not outcome this plan keeps refusing. */
    if (is_atomic && !type_is_atomic_scalar(init->type.kind)) {
        diag_emit(DIAG_ERROR, call->span,
                  "%s: '^atomic' needs an 8-byte scalar (int, float, cstr, or "
                  "ptr); '%s' is %s -- for a bool use an int flag, and for a "
                  "wider value use a lock (stdlib/mutex.tur)",
                  kw, name_f->as.sym->name, type_name(init->type));
        return NULL;
    }

    /* fn-cell-set-with-capturing-closure-segfaults: a `^mut` fn global is a
     * fat cell, exactly as a `^mut` fn `let` is (elab_forms.c) and for the
     * same reason -- a later `set!` may store a capturing closure. */
    if (is_mut && init->type.kind == TY_FN && !init->type.as.fn.boxed) {
        Expr *fat = elab_fn_value_to_fat(e, init);
        if (fat != init) init = fat;
    }

    Binding *b = binding_new(e, name_f->as.sym, init->type,
                             /*is_mut=*/is_mut, /*is_global=*/true, name_f->span);
    b->is_persistent    = is_persistent;
    b->is_atomic        = is_atomic;
    b->is_thread_local  = is_thread_local;
    /* F4: ^deprecated on def */
    b->is_deprecated = is_deprecated_attr;
    b->deprecation_message = deprecation_msg;
    /* async-await-payload-is-int64-only: same provenance note as a `let`. */
    {
        const Expr *ai = init;
        while (ai && ai->kind == EX_ASCRIBE) ai = ai->as.ascribe_.inner;
        if (ai && ai->kind == EX_ASYNC) b->async_payload = &ai->as.async_.payload;
    }
    if (fwd) {
        /* The pre-declared binding becomes this one, in place: every
         * reference elaborated above holds its pointer. */
        *fwd = *b;
        fwd->is_forward_def = false;
        b = fwd;
    } else {
        scope_add(&e->global, b);
    }

    Expr *out = expr_new(e->arena, EX_DEF, TYPE_NIL, call->span);
    out->as.def_.binding = b;
    if (is_deferred_init) {
        /* The def declares; the body's marker initializes.  Both back ends
         * treat an init-less EX_DEF as a declaration (the interpreter binds
         * nil, the emitter declares the static), so nothing runs here. */
        b->deferred_init = init;
        out->as.def_.init = NULL;
    } else {
        out->as.def_.init = init;
    }
    return out;
}
