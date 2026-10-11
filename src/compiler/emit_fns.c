/* emit_fns.c -- function-definition C emission (emit_fn_def). */
#include "emit_internal.h"

/* The inline-C function names of the program being emitted -- written by
 * emit_program as one trailing `tur:inline-c-fns: a b c` comment for the
 * emitted-C checks.
 * One line per program instead of one per function: the stdlib alone puts
 * ~190 inline-C definitions in every program. */
static Buf g_inline_c_fns;
static bool g_inline_c_fns_init;
void emit_note_inline_c_fn(const char *cname) {
    if (!cname || !*cname) return;
    if (!g_inline_c_fns_init) { buf_init(&g_inline_c_fns); g_inline_c_fns_init = true; }
    buf_putc(&g_inline_c_fns, ' ');
    buf_puts(&g_inline_c_fns, cname);
}
void emit_write_inline_c_fns(Buf *out) {
    if (g_inline_c_fns_init && g_inline_c_fns.len > 0) {
        buf_puts(out, "/* tur:inline-c-fns:");
        buf_write(out, g_inline_c_fns.data, g_inline_c_fns.len);
        buf_puts(out, " */\n");
    }
    if (g_inline_c_fns_init) { buf_free(&g_inline_c_fns); g_inline_c_fns_init = false; }
}
#include "emit_cps_ir.h"  /* cps-ir-to-c-backend: colored-fn CPS lowering */
#include "globals.h"   /* g_cps_path */

/* carrier-return-concrete-tail: an `int64_t` (carrier) return fed a value whose
 * emitted C is concrete -- a by-value aggregate or a float kind.  The mixed
 * clone the ABI scan mints for a sole-instance method called from a carrier
 * base (`__inst_Rc_rm_W__spec__int64_t_tur_adt_W(tur_adt_W x) { return x; }`)
 * returned the struct into the int64 (invalid C), and a `double` there is a
 * VALUE conversion.  Box the aggregate / put the float in as its bits, the
 * carrier convention.  With `out` NULL, only answers whether it applies. */
/* Does the function being emitted return the int64 word in its EMITTED C
 * signature?  The declared kind can say `int` while an earlier rule retyped
 * the C return to the body's aggregate (`opt-carrier : int` returning an
 * `(Option int)`); the signature table records what was actually printed. */
static bool fn_emitted_ret_is_word(EmitCtx *ctx, const FnDef *fd, bool use_abi_spec) {
    char *cn = (use_abi_spec && ctx->current_abi_specialization->clone_name)
        ? strdup(ctx->current_abi_specialization->clone_name)
        : (fd && fd->binding ? raw_name_for_binding(fd->binding) : NULL);
    const char *rc = cn ? emit_sig_lookup_ret_ctype(cn) : NULL;
    bool w = rc && strcmp(rc, "int64_t") == 0;
    free(cn);
    return w;
}

static bool carrier_return_concrete_stmt(EmitCtx *ctx, const Expr *tail_e,
                                         const char *ret_val, Buf *out) {
    const char *c = NULL;
    if (emit_str_is_bare_ident(ret_val)) c = emit_localvar_lookup_ctype(ret_val);
    if (!c) {
        Type rt = emit_resolve_type(ctx, tail_e->type);
        if (rt.kind == TY_FLOAT || rt.kind == TY_FLOAT64) c = "double";
        else if (rt.kind == TY_FLOAT32) c = "float";
        else if ((rt.kind == TY_ADT || rt.kind == TY_APP) &&
                 !type_is_heap_adt(rt) && !type_is_heap_struct(rt))
            c = emit_type_c_name(ctx, rt);
    }
    if (!c) return false;
    bool flt = strcmp(c, "double") == 0 || strcmp(c, "float") == 0;
    bool agg = strncmp(c, "tur_adt_", 8) == 0 && !strchr(c, '*');
    if (!flt && !agg) return false;
    if (!out) return true;
    if (flt) {
        Type ft = emit_type_from_kind(strcmp(c, "float") == 0 ? TY_FLOAT32 : TY_FLOAT);
        char *w = emit_word_slot_bits(&ft, ret_val);
        buf_printf(out, "return %s;\n", w);
        free(w);
    } else {
        buf_printf(out,
            "{ %s *__tur_ret_p = (%s *)malloc(sizeof(%s)); "
            "*__tur_ret_p = %s; "
            "TUR_REGION_NOTE_WORDS(__tur_ret_p, sizeof *__tur_ret_p); "
            "return (int64_t)(intptr_t)__tur_ret_p; }\n",
            c, c, c, ret_val);
    }
    return true;
}

/* Direction (1) of polymorphic-ok-in-typeclass-instance-method-...md, as ONE
 * answer: does this non-spec instance-method impl return the int64 carrier
 * because its declared result rides the carrier ABI?  The impl signature
 * decides it here, and the dict slot / by-value wrapper in emit_stmt.c must
 * spell the same type -- saffron-applied-class-var-result-takes-one-instances-
 * type made a ground `(Vec float)` result reach them, and they had re-derived
 * the question as type_c_name, which said `tur_adt_Vec__float *`. */
bool emit_inst_result_rides_carrier(EmitCtx *ctx, const FnDef *fd, const Type *rft) {
    return rft && fd && fd->binding && fd->binding->name && fd->binding->name->name &&
           strncmp(fd->binding->name->name, "__inst_", 7) == 0 &&
           type_uses_carrier_abi(emit_resolve_type(ctx, *rft));
}

/* Append a pass-by-ptr binding to ctx->pbp_param_ptrs, growing the
 * (realloc-backed) array on demand -- no fixed arity ceiling.  Not only
 * params any more: a match binder that BORROWS a boxed recursive field
 * (emit_expr.c, the B3 branch) registers here too, so every use site that
 * already knows how to read a `const T *` parameter reads it the same way. */
void emit_pbp_push(EmitCtx *ctx, Binding *b) {
    if (ctx->n_pbp_params >= ctx->cap_pbp_params) {
        ctx->cap_pbp_params = ctx->cap_pbp_params ? ctx->cap_pbp_params * 2 : 16;
        ctx->pbp_param_ptrs = (Binding **)realloc(ctx->pbp_param_ptrs,
            ctx->cap_pbp_params * sizeof(Binding *));
    }
    ctx->pbp_param_ptrs[ctx->n_pbp_params++] = b;
}

/* ============================================================================
 * CF1: Self-tail-call optimization (self-TCO).
 *
 * A self-recursive call in tail position is lowered to a backedge: the argument
 * values are evaluated into temporaries, the C parameter variables are
 * reassigned, and control jumps to a `__tur_tailcall:` label at the top of the
 * function body -- turning `(let loop [...] ... (loop ...))` and equivalent
 * self-recursive `defn`s into an iterative loop that does not grow the C stack.
 *
 * Scope (1.0): self-tail-calls only, reached through `if`, `do`, and `let`/
 * `letrec` (which is where the named-let idiom and `cond`/`when` -- macro-
 * expanded to `if` -- put them).  Mutual/general tail calls and tail calls
 * inside `match` arms remain ordinary recursive calls; they are correct, just
 * not stack-optimized.  See docs/guides/generators-guide.md and
 * docs/control-flow-completeness-plan.md (Phase CF1).
 * ========================================================================== */

/* The declared C type of parameter `i`, matching the function signature's
 * default (non-pbp, non-poly) path.
 *
 * M4 follow-up (docs/archive/history/tco-in-abi-specs-for-stdlib-iteration.md):
 * when emitting an ABI spec body, the spec's per-instantiation arg type
 * is the authoritative C-level shape — the bare fn binding's full type is
 * the generic (pre-substitution) form which lowers to int64_t for TY_APP.
 * Consult `current_abi_specialization->arg_types[i]` first when set; this
 * lets `tco_params_simple` accept by-value-struct params (e.g.
 * `Vec__int`) that the generic form would reject as carrier-ABI. */
static Type tco_param_type(EmitCtx *ctx, const Expr *fn_e, FnDef *fd, uint8_t i) {
    /* A lifted CLOSURE thunk carries the env pointer as params[0], so its
     * fd->params / fd->param_types are one longer than the SOURCE fn type
     * (`fn_e->type.as.fn.arg_full_types`, which has no env).  Indexing the
     * source type here would read the wrong param -- take the FnDef's own
     * types, which are parallel to fd->params.  (The pbp scan above
     * side-steps the same skew by skipping closures entirely.) */
    if (fd->closure) return fd->param_types[i];
    if (ctx && ctx->current_abi_specialization
        && i < ctx->current_abi_specialization->n_args) {
        return ctx->current_abi_specialization->arg_types[i];
    }
    if (fn_e->type.kind == TY_FN && fn_e->type.as.fn.arg_full_types &&
        fn_e->type.as.fn.arg_full_types[i])
        return *fn_e->type.as.fn.arg_full_types[i];
    return fd->param_types[i];
}

/* Index of the first SOURCE parameter in fd->params: 1 for a lifted closure
 * thunk (params[0] is the env pointer), 0 otherwise.  A self-tail backedge
 * reassigns only the source params -- the env is loop-invariant, because the
 * emitter routes a closure's own recursive self-call through the env pointer
 * the thunk was called with (see the S5 self-call arm in emit_expr.c's
 * closure-call emission), never a freshly built box. */
static uint32_t tco_env_offset(const FnDef *fd) {
    return (fd->closure && fd->n_params > 0) ? 1u : 0u;
}

/* A function is TCO-eligible only if every parameter is a plain scalar we can
 * reassign with `p = tmp;`.  Pass-by-pointer structs, poly-fn, fn-typed, and
 * carrier-ABI parameters are excluded so the backedge temporary's C type is
 * unambiguous.
 *
 * M4 follow-up: a param that resolves to a concrete by-value struct (post-
 * Path A spec substitution: e.g. `Vec__int` from `(Vec int)` at a spec
 * call site) is OK to reassign — `Vec__int new = ...; x = new;` is a valid
 * C struct copy.  The carrier-ABI rejection now checks the EMIT C name
 * rather than the type-kind: only reject when the emitted name is
 * "int64_t" (the carrier fallback), which indicates the type would need
 * a bridge to/from the carrier on reassignment. */
static bool tco_params_simple(EmitCtx *ctx, const Expr *fn_e, FnDef *fd) {
    if (fd->is_variadic) return false;
    /* A lifted closure thunk IS eligible: its env param is skipped (it is
     * loop-invariant across a self-call, see tco_env_offset) and the remaining
     * params are checked exactly as a plain defn's are.  This is what makes the
     * CAPTURING named-let -- `(let go [...] ... (go ...))` inside a fn whose
     * params the loop reads -- iterative instead of self-recursive; without it
     * every such loop is one engine switch away from a stack overflow (it
     * survives the cc path only because gcc -O2 turns the emitted self-call
     * into a sibling call; MIR performs no such optimization).  See
     * docs/archive/history/named-let-self-tail-not-tco.md. */
    for (uint32_t i = tco_env_offset(fd); i < fd->n_params; i++) {
        if (fd->params[i]->is_poly_fn) return false;
        Type pty = tco_param_type(ctx, fn_e, fd, i);
        if (pty.kind == TY_FN) return false;
        Type rpty = emit_resolve_type(ctx, pty);
        if (type_struct_pass_by_ptr(rpty)) return false;
        if (type_uses_carrier_abi(rpty)) {
            /* M4 follow-up: a TY_APP/TY_STRUCT-with-tparams that emits as
             * a concrete struct name is by-value at this call site (Path A
             * spec) — TCO is safe.  Only reject when the C emit collapses
             * to the int64 carrier. */
            const char *c = emit_type_c_name(ctx, rpty);
            if (!c || strcmp(c, "int64_t") == 0) return false;
        }
    }
    return true;
}

/* True if `call` is a direct, arity-matching self-call of the function being
 * emitted (whose C name is `fn_cname`).  Identity is by resolved C name, not
 * Binding pointer: named-let desugars `(let loop ...)` to a `letrec` whose loop
 * binding (the call's target) is a *different* Binding object than the
 * anonymous `fn`'s own FnDef binding, yet both mangle to the same C function
 * name (e.g. __fn_691). */
static bool tco_is_self_call(FnDef *fd, const char *fn_cname, const Expr *call) {
    if (!call || call->kind != EX_CALL) return false;
    if (!call->as.call_.fn_binding) return false;     /* must be a direct call */
    if (call->as.call_.fn_expr) return false;          /* indirect field call */
    /* M4 follow-up (docs/archive/history/tco-in-abi-specs-for-stdlib-iteration.md):
     * Path A's elab resolves typeclass dispatch directly to the instance
     * method's binding (`fn_binding = method->binding; fn_expr = NULL`).
     * `dict_arg` is still set as an annotation, but the actual call IS a
     * direct call — the fn_binding identity check below already handles
     * recursive Path A specs correctly.  The original `dict_arg` reject
     * existed for pre-Path-A indirect dispatch through the dict slot
     * cast, which `fn_expr != NULL` already filters out. */
    if (call->as.call_.is_poly_call) return false;     /* rank-2 poly call */
    if (call->as.call_.n_args != fd->n_params - tco_env_offset(fd)) return false;
    /* A lifted closure thunk is never NAMED by the call: the call's target is
     * the enclosing letrec binding (`go`), whose closure_fn_binding points at
     * the thunk being emitted.  That is the same identity test the closure-call
     * emitter uses to decide it is looking at the closure's own recursive
     * self-call (and so to thread the current env rather than an outer box), so
     * matching it here keeps the backedge and the ordinary call in agreement. */
    if (tco_env_offset(fd) > 0)
        return call->as.call_.fn_binding->closure_fn_binding == fd->binding;
    if (call->as.call_.fn_binding == fd->binding) return true;  /* fast path */
    char *cn = raw_name_for_binding(call->as.call_.fn_binding);
    bool same = fn_cname && cn && strcmp(cn, fn_cname) == 0;
    free(cn);
    return same;
}

/* A let/letrec is tail-transparent for TCO unless one of its bindings is a
 * shape emit_tail's inline `let` arm cannot serve.  Returns the reason (one of
 * the TC_LET_* strings below, phrased for the `^tailcall` verifier) or NULL.
 *
 *   - fn-typed (incl. letrec global fns) and poly-fn bindings: their
 *     declarations are the fn-pointer / fat-carrier / tur_poly_fn_t arms of the
 *     shared declaration ladder, and a letrec of them needs the knot.
 *   - a binding emit_let_value would RELEASE at scope exit (a recursive spine,
 *     a `^mut` cell, a caught Result box, a fresh sum box, a struct's fn-field
 *     handles), unless every use of it is a plain scalar read.  The inline arm
 *     has no trailing code, so the release fires at the backedge or `return`
 *     instead (let_binding_push_scope_frees) -- after the arguments or the
 *     value are in temps, but before a next iteration that could still hold
 *     the binding had it been passed along.  tco_drop_use_ok is the same
 *     conservative test T4 applies to drop glue.
 *
 * tail-grammar-skips-and-or-and-carrier-lets: a carrier-ABI binding (a `Vec`,
 * a `Cons` list, a parametric heap ADT) used to take the whole `let` off the
 * tail path.  The bail was load-bearing for two reasons, both gone: the
 * inline arm repeated only part of emit_let_value's declaration ladder (it
 * shares emit_let_binding_decl now), and it had no channel for the scope-exit
 * releases (it pushes them onto the `any` drop channel now). */
#define TC_LET_FN    "a binding of the enclosing `let` is `fn`-typed or " \
                     "poly-fn, which takes the whole `let` off the tail path"
#define TC_LET_DROP  "a binding of the enclosing `let` is released when the " \
                     "`let` ends (a recursive spine, a `^mut` cell or a fresh " \
                     "box) and is used as more than a plain number, so its " \
                     "release cannot move ahead of the call"
static bool tco_drop_use_ok(const Expr *e, const Binding *b);
/* Set by tco_let_refusal around a binding whose only release is the
 * value-struct payload box (let_binding_scope_free_is_vsp_only). */
static bool g_tco_copying_match_ok = false;
static bool tco_plain_scalar(TypeKind k);
static bool tco_builtin_short_circuit(const Expr *e);

/* Does every value that LEAVES `e` through its tail structure -- a call's
 * arguments (a backedge carries them into the next iteration), or the value
 * itself when it is returned -- have a plain scalar type?  Then nothing that
 * leaves can alias a local released on the way out, and the release may fire
 * at the exit instead of after the body: everything else the body does with
 * the local happens before the exit, exactly as it does before emit_let_value's
 * trailing release, whose own predicates already vouch for those uses.
 *
 * Walks the same structure tco_mark does; a form it does not know is a leaf,
 * judged by its type.  An explicit `return`/`throw` anywhere in the body, or a
 * dynamic call, answers no -- the conservative tco_drop_use_ok decides then. */
static bool tco_tail_values_scalar(const Expr *e) {
    if (!e) return true;
    switch (e->kind) {
        case EX_IF:
            return tco_tail_values_scalar(e->as.if_.then_) &&
                   tco_tail_values_scalar(e->as.if_.else_or_null);
        case EX_DO:
            return e->as.do_.n == 0 ||
                   tco_tail_values_scalar(e->as.do_.items[e->as.do_.n - 1]);
        case EX_LET:
        case EX_LETREC:
            return tco_tail_values_scalar(e->as.let_.body);
        case EX_MATCH:
            for (uint32_t i = 0; i < e->as.match_.n_arms; i++)
                if (!tco_tail_values_scalar(e->as.match_.arms[i].body))
                    return false;
            return true;
        case EX_BUILTIN:
            if (tco_builtin_short_circuit(e))
                return tco_tail_values_scalar(
                    e->as.builtin.args[e->as.builtin.n - 1]);
            return tco_plain_scalar(e->type.kind);
        case EX_CALL:
            if (e->as.call_.fn_expr) return false;
            for (uint32_t i = 0; i < e->as.call_.n_args; i++) {
                const Expr *a = e->as.call_.args[i];
                if (!a || !tco_plain_scalar(a->type.kind)) return false;
            }
            return e->type.kind == TY_NIL || e->type.kind == TY_NEVER ||
                   tco_plain_scalar(e->type.kind);
        case EX_DYN_CALL:
            return false;
        default:
            return e->type.kind == TY_NIL || e->type.kind == TY_NEVER ||
                   tco_plain_scalar(e->type.kind);
    }
}

static const char *tco_let_refusal(EmitCtx *ctx, const Expr *e) {
    for (uint32_t i = 0; i < e->as.let_.n; i++) {
        const Binding *b = e->as.let_.bindings[i].binding;
        if (!b) return TC_LET_FN;
        if (b->type.kind == TY_FN || b->is_poly_fn) return TC_LET_FN;
    }
    int values_scalar = -1;   /* computed on first need */
    for (uint32_t i = 0; i < e->as.let_.n; i++) {
        if (!let_binding_may_need_scope_free(ctx, e, i)) continue;
        if (values_scalar < 0)
            values_scalar = !expr_contains_return_or_throw(e->as.let_.body) &&
                            tco_tail_values_scalar(e->as.let_.body);
        if (values_scalar) continue;
        const Binding *b = e->as.let_.bindings[i].binding;
        g_tco_copying_match_ok = let_binding_scope_free_is_vsp_only(ctx, e, i);
        bool ok = tco_drop_use_ok(e->as.let_.body, b);
        for (uint32_t j = 0; ok && j < e->as.let_.n; j++)
            if (j != i && !tco_drop_use_ok(e->as.let_.bindings[j].init, b))
                ok = false;
        g_tco_copying_match_ok = false;
        if (!ok) return TC_LET_DROP;
    }
    return NULL;
}

static bool tco_let_simple(EmitCtx *ctx, const Expr *e) {
    return tco_let_refusal(ctx, e) == NULL;
}

/* proper-tail-calls T2 (T-D2): can this NON-self call in tail position be
 * emitted as a genuine C tail `return f(args);` with no panic check?
 *
 * The bar here is deliberately low, because the decision is layered: this only
 * has to rule out shapes whose emission is not a plain call expression at all.
 * Everything that depends on how the call actually emits -- an argument leaving
 * an owned box pending, a region bracket, a carrier bridge on the way out -- is
 * re-asked at emission time, where the answer is knowable, and a refusal there
 * falls back to the ordinary hoist-and-check spelling.  So a false positive
 * here costs an unused mark, never wrong output.
 *
 * INDIRECT calls are excluded: a call through a `fn` value is T-D6's trampoline
 * problem, and its emission goes through the fat-closure protocol rather than a
 * named callee, so it is not a plain `f(args)` to hand back.
 *
 * T2's first cut ALSO required the callee's C return type to equal the enclosing
 * function's, and refused the body outright otherwise.  That was not a statement
 * about tail position -- it was a workaround for
 * docs/reported/emit-tail-return-path-lacks-carrier-bridges.md, which is fixed:
 * both return paths share `emit_fn_return_spelling` now, so a marked call whose
 * return does need a bridge is simply emitted with its hoist and its check, the
 * way it always was.  The type question still gets asked, but where it belongs
 * -- at the call's own emission in emit_value, as the condition for dropping the
 * check rather than as a veto on the whole body. */
static bool tc_nonself_tail_eligible(EmitCtx *ctx, const Expr *e) {
    if (e->type.kind == TY_NIL || e->type.kind == TY_NEVER) return false;
    if (e->type.kind == TY_FN) return false;   /* fat-return shim, not a call */
    if (e->as.call_.fn_expr || e->as.call_.is_poly_call) return false;
    if (!e->as.call_.fn_binding) return false;
    if (!ctx->current_fn_ret_ctype) return false;
    /* The callee's C return type must be exactly the enclosing function's, so
     * that `return f(args);` needs no cast and no carrier bridge between the
     * call and the return.
     *
     * Ask `emit_call_name` -- the emitter's own answer to "what will this call
     * be spelled as" -- rather than the binding's raw name: a monomorphized
     * callee is emitted as `some__spec__tur_adt_Option__int_int64_t`, whose
     * return is the by-value aggregate, while the generic `some` it came from
     * returns the int64 carrier.  Comparing against the generic said "match" and
     * routed the body into a return path that then had to spill the aggregate it
     * had been promised would not appear.
     *
     * A name that is not a plain C identifier is a dispatch EXPRESSION (a cast
     * function-pointer head through a dict slot or a captured witness), which
     * has no forward declaration to read and is refused by the same test. */
    return true;
}

/* proper-tail-calls T3 (T-D4): is this `match` one whose arms emit_tail can put
 * in tail position?
 *
 * `tco_mark` and `emit_tail` have to agree here exactly -- a marked self call
 * whose arm never reaches emit_tail would leave `__tur_tailcall:` emitted and
 * unused, which is a C compiler warning -- so both ask this one question.
 *
 * A nil-typed match has no value to return, and a SESSION-OFFER scrutinee emits
 * through its own arm shape (the tag recv, `emit_expr.c`), which is not one of
 * the five sites carrying the tail hook.  Everything else -- ADT constructor
 * patterns, `any` type-narrowing, literal patterns, guards, wildcards -- is
 * served. */
static bool tco_match_tail_ok(const Expr *e) {
    if (e->type.kind == TY_NIL || e->type.kind == TY_NEVER) return false;
    if (e->as.match_.n_arms == 0) return false;
    if (!e->as.match_.scrutinee) return false;
    if (e->as.match_.scrutinee->type.kind == TY_SESSION_OFFER) return false;
    return true;
}

/* ============================================================================
 * proper-tail-calls T5 (docs/archive/proper-tail-calls-plan.md, T-D5):
 * mutual tail-call groups -- the registry tco_mark consults.
 *
 * A strongly-connected component of the TAIL-call graph (edges: a direct call
 * in tail position under tco_mark's grammar) with two or more members is fused
 * into one C function, `__tcg_group_N(int __tcg_st, <every member's params>)`,
 * that dispatches on `__tcg_st` in a `switch` at the top of a loop.  Each
 * member's body is emitted as one `case`, with its parameters declared as
 * block locals read from the member's slots; a tail call to any member of the
 * group -- itself included -- evaluates its arguments, writes the target's
 * slots, sets `__tcg_st` and jumps back to the top.  Each member keeps its own
 * C function as a thin wrapper, `return __tcg_group_N(<i>, ...);`, for every
 * call from outside the group and every non-tail call from inside it.
 *
 * This is the generalization of the self-call backedge the plan asks for, not
 * a second mechanism: it is a `goto` (so it holds at -O0 and needs nothing
 * from the C compiler), and the same argument-temps / frame-fire / reassign
 * order the backedge uses.  Section 2.4 of the plan is why it matters: at -O2
 * clang inlines a SMALL cycle into a loop, which evaporates the moment the
 * bodies grow; at -O0 nothing did.
 * ========================================================================== */
#define TCG_MAXMEM    8     /* members per fused group (risk TR4) */
#define TCG_MAXSLOTS  16    /* total parameters across the group */

typedef struct TcgGroup {
    const Expr *mem_e[TCG_MAXMEM];
    FnDef      *mem[TCG_MAXMEM];
    int         n_mem;
    int         last_mem;               /* the member emitted LAST, in item order */
    uint32_t    slot_base[TCG_MAXMEM];  /* first slot of each member */
    char       *slot_ctype[TCG_MAXSLOTS];
    uint32_t    n_slots;
    char       *ret_ctype;
    char        name[40];
    bool        declared;               /* prototype written */
    bool        defined;                /* fused body written */
} TcgGroup;

typedef struct TcgCur {
    const TcgGroup *g;
    int             idx;                /* the member being emitted */
} TcgCur;

static TcgGroup g_tcg_groups[64];
static int      g_n_tcg_groups;
static int      g_tcg_ctr;

void tcg_reset_group_registry(void) {
    for (int i = 0; i < g_n_tcg_groups; i++) {
        for (uint32_t k = 0; k < g_tcg_groups[i].n_slots; k++)
            free(g_tcg_groups[i].slot_ctype[k]);
        free(g_tcg_groups[i].ret_ctype);
    }
    g_n_tcg_groups = 0;
    g_tcg_ctr = 0;
}

/* The member of the current group a call targets, or -1.  Arity must match
 * exactly: a jump writes every one of the target's slots. */
static int tcg_call_target(const EmitCtx *ctx, const Expr *call) {
    if (!ctx->tcg_cur || !call || call->kind != EX_CALL) return -1;
    if (call->as.call_.fn_expr || call->as.call_.is_poly_call) return -1;
    const Binding *fb = call->as.call_.fn_binding;
    if (!fb || !fb->source_fn_def) return -1;
    const TcgGroup *g = ctx->tcg_cur->g;
    for (int i = 0; i < g->n_mem; i++)
        if (g->mem[i] == fb->source_fn_def &&
            call->as.call_.n_args == g->mem[i]->n_params)
            return i;
    return -1;
}

/* ============================================================================
 * proper-tail-calls T4 (docs/archive/proper-tail-calls-plan.md, T-D3):
 * hoisting an owned local's drop glue ahead of a tail call.
 *
 * A `let` binding an owned value -- a `ref<T>`, an `rc<T>`, a move-only Drop
 * opaque, or a by-value ADT with owning fields -- gets its scope-exit drop as a
 * synthesized `defer` appended to the `let` body's `do`.  That defer used to
 * take the whole block out of tail position, because "a defer runs after the
 * call" is true of a defer the author wrote.  For drop glue it is only true
 * when the value is still LIVE at the call: when nothing the next activation
 * can reach refers to it, running the drop before the backedge is
 * indistinguishable from running it after the recursive call returns.
 *
 * So a block qualifies when:
 *
 *   - every defer in it is drop glue (`is_drop_glue`), and they are all
 *     trailing -- the elaborator appends them after the block's value;
 *   - no item returns or throws (those fire the frame chain on their own
 *     schedule, which this path does not re-derive); and
 *   - every dropped binding is DEAD at every exit of the tail: each of its
 *     occurrences, in the block and in the enclosing `let`'s initializers, is a
 *     read that copies a plain scalar out (`@b`, `(.count o)`) or a store
 *     through it (`(set-deref! b v)`).  A bare occurrence -- an argument, a
 *     closure capture, a borrow, anything the walker does not recognise -- may
 *     let the value outlive the drop, and refuses.
 *
 * The lowering keeps the frame rather than deleting it.  The plan's first
 * reading was to drop the `tur_frame_push_defer` / `tur_frame_fire_lifo` pair
 * outright, but the frame is what fires the drop on the PANIC path: a panic in
 * one of the backedge's argument expressions returns through
 * `emit_panic_signal_return`, which fires `ctx->frame_var`'s chain.  Without the
 * frame that exit leaks the local under `catch-unwind`.  What changes is when
 * the frame fires on the normal path: at every exit of the tail -- after a
 * backedge's argument temps, before a `return` -- rather than once after a
 * call that is no longer a tail call.  The frame is block-scoped, so a
 * backedge leaves it and the next iteration re-declares it: constant stack.
 * ========================================================================== */

/* Forward decl: the capture analysis the elaborator uses for closures and defer
 * thunks.  Complete over every ExprKind, which is what makes it the right
 * fallback for the node kinds tco_drop_use_ok does not enumerate. */
Binding **collect_free_vars(const Expr *e, Binding **params, uint8_t n_params,
                            Binding **self_exclude, uint32_t n_self_exclude,
                            uint32_t *n_out);

/* A value a drop cannot invalidate once it has been copied out: a machine
 * number or a bool.  Pointer-shaped results (a `cstr`, a nested owner, an
 * aggregate) may alias storage the drop glue frees, so they do not qualify. */
static bool tco_plain_scalar(TypeKind k) {
    switch (k) {
        case TY_BOOL: case TY_INT: case TY_FLOAT:
        case TY_INT8: case TY_INT16: case TY_INT32: case TY_INT64:
        case TY_UINT8: case TY_UINT16: case TY_UINT32: case TY_UINT64:
        case TY_FLOAT32: case TY_FLOAT64:
            return true;
        default:
            return false;
    }
}

static bool tco_mentions(const Expr *e, const Binding *b) {
    if (!e) return false;
    uint32_t n = 0;
    Binding **fv = collect_free_vars(e, NULL, 0, NULL, 0, &n);
    bool hit = false;
    for (uint32_t i = 0; i < n && !hit; i++) hit = (fv[i] == b);
    free(fv);
    return hit;
}

static bool tco_is_var(const Expr *e, const Binding *b) {
    while (e && e->kind == EX_ASCRIBE) e = e->as.ascribe_.inner;
    return e && e->kind == EX_VAR && e->as.var.binding == b;
}

/* Is every occurrence of `b` in `e` a use that cannot outlive a drop of `b`?
 * Conservative by construction: a node kind not enumerated here is answered by
 * the complete free-variable walk, so it refuses whenever `b` appears in it at
 * all. */
static bool tco_drop_use_ok(const Expr *e, const Binding *b) {
    if (!e) return true;
    switch (e->kind) {
        case EX_VAR:
            return e->as.var.binding != b;
        case EX_DEREF:
            if (tco_is_var(e->as.deref_.expr, b))
                return tco_plain_scalar(e->type.kind);
            return tco_drop_use_ok(e->as.deref_.expr, b);
        case EX_GET_FIELD:
            if (tco_is_var(e->as.get_field_.struct_expr, b))
                return tco_plain_scalar(e->type.kind);
            return tco_drop_use_ok(e->as.get_field_.struct_expr, b);
        case EX_SET_DEREF:
            if (!tco_is_var(e->as.set_deref_.ref, b) &&
                !tco_drop_use_ok(e->as.set_deref_.ref, b))
                return false;
            return tco_drop_use_ok(e->as.set_deref_.value, b);
        case EX_REINTERPRET:
            return tco_drop_use_ok(e->as.reinterpret_.expr, b);
        case EX_ASCRIBE:
            return tco_drop_use_ok(e->as.ascribe_.inner, b);
        case EX_IF:
            return tco_drop_use_ok(e->as.if_.cond, b) &&
                   tco_drop_use_ok(e->as.if_.then_, b) &&
                   tco_drop_use_ok(e->as.if_.else_or_null, b);
        case EX_DO:
            for (uint32_t i = 0; i < e->as.do_.n; i++)
                if (!tco_drop_use_ok(e->as.do_.items[i], b)) return false;
            return true;
        case EX_LET:
        case EX_LETREC:
            for (uint32_t i = 0; i < e->as.let_.n; i++)
                if (!tco_drop_use_ok(e->as.let_.bindings[i].init, b)) return false;
            return tco_drop_use_ok(e->as.let_.body, b);
        case EX_CALL:
            if (e->as.call_.fn_expr && tco_mentions(e->as.call_.fn_expr, b))
                return false;
            for (uint32_t i = 0; i < e->as.call_.n_args; i++)
                if (!tco_drop_use_ok(e->as.call_.args[i], b)) return false;
            return true;
        case EX_BUILTIN:
            for (uint32_t i = 0; i < e->as.builtin.n; i++)
                if (!tco_drop_use_ok(e->as.builtin.args[i], b)) return false;
            return true;
        case EX_MATCH:
            /* sum-payload-box-leaks-at-match: for a binding whose only release
             * is its value-struct payload box, a match that copies out of it
             * is a read -- the binders are deref-copies, gone from the box
             * before the release fires at the exit. */
            if (!(g_tco_copying_match_ok && match_copies_out_of_b(e, b)) &&
                !tco_drop_use_ok(e->as.match_.scrutinee, b))
                return false;
            for (uint32_t i = 0; i < e->as.match_.n_arms; i++)
                if (!tco_drop_use_ok(e->as.match_.arms[i].guard, b) ||
                    !tco_drop_use_ok(e->as.match_.arms[i].body, b))
                    return false;
            return true;
        default:
            return !tco_mentions(e, b);
    }
}

/* The index of a `do` block's value item when its defers are exclusively
 * TRAILING DROP GLUE -- the shape the elaborator builds for an owned local --
 * or -1 when they are not (a written `defer`, a defer before the value, or no
 * defer at all). */
static int tco_drop_glue_tail_idx(const Expr *e) {
    int last = -1;
    for (int i = (int)e->as.do_.n - 1; i >= 0; i--)
        if (e->as.do_.items[i]->kind != EX_DEFER) { last = i; break; }
    if (last < 0 || (uint32_t)last + 1 == e->as.do_.n) return -1;
    for (int i = 0; i < last; i++)
        if (e->as.do_.items[i]->kind == EX_DEFER) return -1;
    for (uint32_t i = (uint32_t)last + 1; i < e->as.do_.n; i++)
        if (!e->as.do_.items[i]->as.defer_.is_drop_glue) return -1;
    return last;
}

#define TC_DEFER_USER "a `defer` written in this block runs AFTER the call, " \
                      "innermost first -- running it before a backedge would " \
                      "reorder what it does, so nothing in the block is in " \
                      "tail position"
#define TC_DROP_LIVE  "an owned local of the enclosing `let` (a `ref<T>`, " \
                      "`rc<T>` or other value with drop glue) is still live " \
                      "at the call -- it is passed, captured, borrowed or " \
                      "read as more than a plain number -- so its drop cannot " \
                      "move ahead of the call"
#define TC_DROP_EXIT  "this block's drop glue shares it with a `return` or " \
                      "`throw`, which the tail path does not re-derive"

/* Why a `do` block (the body of `let_e` when non-NULL) cannot carry a tail
 * position through its drop glue, or NULL when it can.  Only called for a block
 * that HAS defers. */
static const char *tco_drop_glue_refusal(const Expr *e, const Expr *let_e) {
    int last = tco_drop_glue_tail_idx(e);
    if (last < 0) return TC_DEFER_USER;
    for (int i = 0; i <= last; i++)
        if (expr_contains_return_or_throw(e->as.do_.items[i])) return TC_DROP_EXIT;
    /* The elaborator only ever builds this shape as a `let` body; without the
     * `let`, the initializers that may alias the local are out of sight. */
    if (!let_e) return TC_DROP_LIVE;
    for (uint32_t d = (uint32_t)last + 1; d < e->as.do_.n; d++) {
        const Expr *dx = e->as.do_.items[d];
        for (uint8_t c = 0; c < dx->as.defer_.n_captures; c++) {
            const Binding *b = dx->as.defer_.captures[c];
            for (int i = 0; i <= last; i++)
                if (!tco_drop_use_ok(e->as.do_.items[i], b)) return TC_DROP_LIVE;
            for (uint32_t i = 0; i < let_e->as.let_.n; i++)
                if (!tco_drop_use_ok(let_e->as.let_.bindings[i].init, b))
                    return TC_DROP_LIVE;
        }
    }
    return NULL;
}

/* Mark tail-position calls, recursing through if/do/let/letrec/match.  This mirrors
 * emit_tail's structural recursion exactly so that "marked >= 1" predicts
 * whether emit_tail will emit a backedge (and thus whether the
 * `__tur_tailcall:` label is used).
 *
 * Returns the number of SELF calls marked -- the backedge count, which is what
 * the label is gated on.  `*n_ok` receives the number of non-self tail calls
 * marked for T2's checkless `return f(args);` (proper-tail-calls T-D2).  The two
 * are kept apart because a backedge reassigns the C parameters and so needs
 * `tco_params_simple`, while a plain tail call does not, and because only a
 * backedge makes the label live.
 *
 * There is no third count.  T2's first cut also tracked REFUSED leaves, so that
 * a body could be kept out of emit_tail entirely when any one of its tail leaves
 * could not be served -- emit_tail's return path was a thinner one than
 * emit_fn_def's, and a refused leaf landing there emitted an unbridged return.
 * Both paths now share `emit_fn_return_spelling`, so a refused leaf is simply
 * emitted the way it always was and the all-or-nothing rule is gone. */
static int tco_mark(EmitCtx *ctx, FnDef *fd, const char *fn_cname, Expr *e,
                    int *n_ok);

/* tail-grammar-skips-and-or-and-carrier-lets: `(and a ... z)` / `(or a ...
 * z)`.  Every operand but the last is a test (a false / true one answers the
 * whole form without evaluating the rest), and the LAST is the form's value,
 * so it is in the enclosing tail position.  tco_mark, the `^tailcall`
 * verifier and emit_tail all ask this one question (plan risk TR1). */
static bool tco_builtin_short_circuit(const Expr *e) {
    return e && e->kind == EX_BUILTIN && e->as.builtin.spec &&
           (e->as.builtin.spec->shape == BS_AND_SC ||
            e->as.builtin.spec->shape == BS_OR_SC) &&
           e->as.builtin.n >= 2;
}

/* The `do` arm of tco_mark, split out because a block of trailing drop glue
 * needs to see the `let` it is the body of (`let_e`, NULL otherwise) -- the
 * initializers are where the dropped local may have been aliased. */
static int tco_mark_do(EmitCtx *ctx, FnDef *fd, const char *fn_cname, Expr *e,
                       const Expr *let_e, int *n_ok) {
    if (e->as.do_.n == 0) return 0;
    bool has_defer = false;
    for (uint32_t i = 0; i < e->as.do_.n; i++)
        if (e->as.do_.items[i]->kind == EX_DEFER) { has_defer = true; break; }
    if (!has_defer)
        return tco_mark(ctx, fd, fn_cname, e->as.do_.items[e->as.do_.n - 1],
                        n_ok);
    /* proper-tail-calls T4 (T-D3): drop glue on a dead local no longer takes
     * the block out of the tail grammar.  Only a BACKEDGE is taken through it:
     * a non-self call under an open frame is not a C tail call (the frame fires
     * after it), so those are left unmarked and the block's leaves return
     * normally.  The block qualifies only when it carries at least one
     * backedge, which keeps every body without one on the path it always took. */
    e->as.do_.tail_drop_hoist = false;
    if (tco_drop_glue_refusal(e, let_e)) return 0;
    int last = tco_drop_glue_tail_idx(e);
    int n = tco_mark(ctx, fd, fn_cname, e->as.do_.items[last], NULL);
    if (n > 0) e->as.do_.tail_drop_hoist = true;
    return n;
}

static int tco_mark(EmitCtx *ctx, FnDef *fd, const char *fn_cname, Expr *e,
                    int *n_ok) {
    if (!e) return 0;
    switch (e->kind) {
        case EX_CALL: {
            e->as.call_.tail_group_idx = 0;
            if (tco_is_self_call(fd, fn_cname, e)) {
                e->as.call_.is_tail_self_call = true;
                return 1;
            }
            /* T5: a tail call to another member of this function's mutual
             * tail-call group is a jump, and counts as a backedge. */
            int gi = tcg_call_target(ctx, e);
            if (gi >= 0) {
                e->as.call_.tail_group_idx = (uint8_t)(gi + 1);
                return 1;
            }
            if (tc_nonself_tail_eligible(ctx, e)) {
                e->as.call_.is_tail_call = true;
                if (n_ok) (*n_ok)++;
            }
            return 0;
        }
        case EX_IF: {
            /* void-self-tail-call-not-lowered: in a `: nil` function the
             * missing arm of a one-armed `if` is the empty return, so the
             * `then` arm is a tail position. */
            if (!e->as.if_.else_or_null)
                return ctx->tail_void ? tco_mark(ctx, fd, fn_cname, e->as.if_.then_, n_ok)
                                      : 0;
            int n = tco_mark(ctx, fd, fn_cname, e->as.if_.then_, n_ok);
            n += tco_mark(ctx, fd, fn_cname, e->as.if_.else_or_null, n_ok);
            return n;
        }
        case EX_DO:
            return tco_mark_do(ctx, fd, fn_cname, e, NULL, n_ok);
        case EX_LET:
        case EX_LETREC:
            if (!tco_let_simple(ctx, e)) return 0;
            if (e->as.let_.body && e->as.let_.body->kind == EX_DO)
                return tco_mark_do(ctx, fd, fn_cname, e->as.let_.body, e, n_ok);
            return tco_mark(ctx, fd, fn_cname, e->as.let_.body, n_ok);
        case EX_MATCH: {
            /* proper-tail-calls T3 (T-D4): every ARM of a `match` is in the
             * enclosing tail position -- the scrutinee is evaluated first and
             * nothing runs after the arm that answers it.  This is the shape a
             * Turmeric programmer reaches for first when looping over an ADT,
             * and until now it silently got ordinary recursion (2.3).
             *
             * A guarded arm is still tail: a failing guard falls through to the
             * next arm's test, and the guard itself is evaluated before the
             * body, not after it. */
            if (!tco_match_tail_ok(e)) return 0;
            int n = 0;
            for (uint32_t i = 0; i < e->as.match_.n_arms; i++)
                n += tco_mark(ctx, fd, fn_cname, e->as.match_.arms[i].body,
                              n_ok);
            return n;
        }
        case EX_DYN_CALL:
            /* proper-tail-calls T6 (T-D6): a dynamic call has no callee to
             * branch to, but in tail position it bounces to a trampoline --
             * which emit_tail must see to spell. */
            ctx->tail_dyn_seen = true;
            return 0;
        case EX_BUILTIN:
            if (tco_builtin_short_circuit(e))
                return tco_mark(ctx, fd, fn_cname,
                                e->as.builtin.args[e->as.builtin.n - 1], n_ok);
            return 0;
        default:
            /* Not a call at all: a literal, a variable.  Neither served nor
             * refused -- it is simply not a tail CALL. */
            return 0;
    }
}


/* ============================================================================
 * proper-tail-calls T1 (docs/archive/proper-tail-calls-plan.md, T-D1):
 * verifying the `^tailcall` annotation.
 *
 * `^tailcall` is a request to be TOLD, not a request to be optimized: the
 * author asserts that a call must become a real tail call, and a call that does
 * not is TUR-E0716 naming the reason.  Without it, whether a call was lowered
 * to a backedge is invisible in the source and the failure mode is a stack
 * overflow at an unpredictable depth, in production.
 *
 * The check lives HERE, beside tco_mark / emit_tail, rather than in
 * elaboration, because the tail grammar is theirs: `tco_params_simple`,
 * `tco_let_simple` and `tco_is_self_call` ARE the decision, and a second copy
 * of that grammar in the elaborator would be one more pair that can drift
 * (risk TR1).  The walker below mirrors tco_mark's tail spine exactly, and
 * additionally descends into the non-tail sub-expressions people write calls
 * in, so it can say WHICH rule refused the call rather than only that one did.
 * ========================================================================== */

/* Why a position is not a tail position.  NULL means "this IS a tail position
 * under tco_mark's grammar"; a non-NULL string is the reason, phrased to
 * complete the sentence "not in tail position: ...". */
#define TC_ARG        "the call sits in an argument position, and arguments are " \
                      "evaluated before the call they belong to"
#define TC_IF_COND    "the condition of an `if` is evaluated before either branch"
#define TC_IF_ONE_ARM "a one-armed `if` (no else branch) is not part of the tail " \
                      "grammar -- give it an else branch"
#define TC_NOT_LAST   "only the LAST form of a `do` block is in tail position"
#define TC_LET_INIT   "a `let` initializer is evaluated before the body"
#define TC_MATCH_SCR  "the scrutinee of a `match` is evaluated before any arm"
#define TC_SC_TEST    "only the LAST operand of `and` / `or` is in tail position; " \
                      "the others are tests evaluated before it"
/* T3 landed, so a `match` ARM is in the tail grammar.  What is left to refuse
 * is the match whose SHAPE emit_tail cannot serve -- see tco_match_tail_ok. */
#define TC_MATCH_ARM  "the enclosing `match` is not one the tail path can " \
                      "serve: it yields no value, has no arms, or offers over " \
                      "a session channel (whose arms are emitted by the " \
                      "session-offer path)"
#define TC_ASCRIBE    "an ascription sits between the call and the return, and " \
                      "is not part of the tail grammar"
#define TC_RETURN     "an explicit `return` form is not part of the tail grammar " \
                      "-- make the call the block's value instead"
#define TC_WHILE      "a `while` body is not in tail position; its value is " \
                      "discarded and the loop continues"
#define TC_GUARD      "a `when`-guard is evaluated before the arm it guards"
#define TC_CPS_BODY   "the enclosing function is lowered through the CPS " \
                      "backend -- it is effect-colored, or reaches a " \
                      "delimited-control construct -- and that backend emits " \
                      "every call as an ordinary call followed by a " \
                      "continuation invocation"


/* The shape of the call itself, when the call sits in a genuine tail position
 * but still did not become a backedge.  Returns NULL when the call shape is
 * fine and the refusal belongs to the enclosing function (`fn_block`). */
static const char *tc_call_shape_reason(FnDef *fd, const char *fn_cname,
                                        const Expr *call) {
    if (call->as.call_.fn_expr || call->as.call_.is_poly_call)
        return "the callee is not statically known -- this is an indirect call "
               "through a function value (a `fn`-typed parameter, a struct "
               "field, or a rank-2 polymorphic parameter), and typed Turmeric "
               "does not trampoline those (T-D6 of "
               "docs/archive/proper-tail-calls-plan.md)";
    if (!call->as.call_.fn_binding)
        return "this is not a direct call to a named function";
    if (!tco_is_self_call(fd, fn_cname, call)) {
        /* Same function, wrong arity is a different complaint from "a different
         * function" -- and much more likely to be a typo. */
        bool same_fn = (call->as.call_.fn_binding == fd->binding);
        if (!same_fn && fn_cname) {
            char *cn = raw_name_for_binding(call->as.call_.fn_binding);
            same_fn = (cn && strcmp(cn, fn_cname) == 0);
            free(cn);
        }
        if (same_fn && call->as.call_.n_args != fd->n_params - tco_env_offset(fd))
            return "the call is under- or over-saturated -- a backedge has to "
                   "reassign every parameter, so the arity must match exactly";
        return "the callee is a DIFFERENT function that does not share a "
               "mutual tail-call group with this one.  A group forms when "
               "the functions call each other in a cycle of TAIL calls and "
               "each is a plain top-level function -- no closure, dictionary, "
               "inline-C, effectful or `catch-unwind` body, no variadic or "
               "`fn`-typed parameter -- with the same C return type, at most "
               "8 members and 16 parameters in all (T5 of "
               "docs/archive/proper-tail-calls-plan.md)";
    }
    return NULL;
}

static void tc_report(FnDef *fd, const char *fn_cname, Expr *call,
                      const char *why, const char *fn_block) {
    /* A function can be emitted more than once (header + implementation, or
     * several ABI specializations); report each annotated call once. */
    if (call->as.call_.tailcall_diagnosed) return;
    call->as.call_.tailcall_diagnosed = true;

    const char *reason = why;
    if (!reason) reason = tc_call_shape_reason(fd, fn_cname, call);
    if (!reason) reason = fn_block;
    if (!reason) {
        /* tco_mark's grammar and this walker disagreed.  That is exactly the
         * TR1 drift the annotation exists to catch, so say so rather than
         * inventing a reason -- it is a compiler bug, not the author's. */
        reason = "the emitter's tail grammar refused it for a reason this check "
                 "could not name -- please report this (tco_mark / emit_tail "
                 "drift)";
    }
    diag_emit_with_code(DIAG_ERROR, call->span, TUR_E0716_TAILCALL_NOT_TAIL,
                        "`^tailcall` call is not in tail position: %s", reason);
}

static void tc_check(EmitCtx *ctx, FnDef *fd, const char *fn_cname, Expr *e,
                     const char *why, const char *fn_block);

/* The `do` arm of tc_check, mirroring tco_mark_do: only the value item is in
 * tail position, and a block with defers passes that position on only when its
 * defers are drop glue on dead locals (T4); otherwise the value item inherits
 * the block's own refusal. */
static void tc_check_do(EmitCtx *ctx, FnDef *fd, const char *fn_cname, Expr *e,
                        const Expr *let_e, const char *why,
                        const char *fn_block) {
    bool has_defer = false;
    for (uint32_t i = 0; i < e->as.do_.n; i++)
        if (e->as.do_.items[i]->kind == EX_DEFER) { has_defer = true; break; }
    int last = (int)e->as.do_.n - 1;
    const char *last_why = why;
    if (has_defer) {
        int li = tco_drop_glue_tail_idx(e);
        if (li >= 0) last = li;
        else
            for (int i = (int)e->as.do_.n - 1; i >= 0; i--)
                if (e->as.do_.items[i]->kind != EX_DEFER) { last = i; break; }
        if (!why) last_why = tco_drop_glue_refusal(e, let_e);
    }
    for (uint32_t i = 0; i < e->as.do_.n; i++) {
        const char *sub = ((int)i == last) ? last_why : (why ? why : TC_NOT_LAST);
        tc_check(ctx, fd, fn_cname, e->as.do_.items[i], sub, fn_block);
    }
}

/* Walk `e`, reporting every `^tailcall`-annotated call that did not become a
 * backedge.  `why` is NULL exactly while the position is a tail position. */
static void tc_check(EmitCtx *ctx, FnDef *fd, const char *fn_cname, Expr *e,
                     const char *why, const char *fn_block) {
    if (!e) return;
    switch (e->kind) {
        case EX_CALL:
            if (e->as.call_.wants_tailcall && !e->as.call_.is_tail_self_call &&
                !e->as.call_.tail_group_idx)
                tc_report(fd, fn_cname, e, why, fn_block);
            for (uint32_t i = 0; i < e->as.call_.n_args; i++)
                tc_check(ctx, fd, fn_cname, e->as.call_.args[i], TC_ARG, fn_block);
            tc_check(ctx, fd, fn_cname, e->as.call_.fn_expr, TC_ARG, fn_block);
            return;
        case EX_IF:
            tc_check(ctx, fd, fn_cname, e->as.if_.cond, TC_IF_COND, fn_block);
            if (e->as.if_.else_or_null) {
                tc_check(ctx, fd, fn_cname, e->as.if_.then_, why, fn_block);
                tc_check(ctx, fd, fn_cname, e->as.if_.else_or_null, why, fn_block);
            } else {
                tc_check(ctx, fd, fn_cname, e->as.if_.then_,
                         why ? why : (ctx->tail_void ? NULL : TC_IF_ONE_ARM),
                         fn_block);
            }
            return;
        case EX_DO:
            tc_check_do(ctx, fd, fn_cname, e, NULL, why, fn_block);
            return;
        case EX_LET:
        case EX_LETREC: {
            for (uint32_t i = 0; i < e->as.let_.n; i++)
                tc_check(ctx, fd, fn_cname, e->as.let_.bindings[i].init,
                         TC_LET_INIT, fn_block);
            const char *lr = tco_let_refusal(ctx, e);
            const char *sub = !lr ? why : (why ? why : lr);
            if (e->as.let_.body && e->as.let_.body->kind == EX_DO)
                tc_check_do(ctx, fd, fn_cname, e->as.let_.body, e, sub, fn_block);
            else
                tc_check(ctx, fd, fn_cname, e->as.let_.body, sub, fn_block);
            return;
        }
        case EX_MATCH:
            tc_check(ctx, fd, fn_cname, e->as.match_.scrutinee, TC_MATCH_SCR,
                     fn_block);
            /* T3 (T-D4): an arm body IS in tail position now, so it inherits
             * the enclosing reason (`why`) rather than being refused outright --
             * exactly as an `if` branch does.  Only a match whose shape the tail
             * path cannot serve still refuses on its own account. */
            const char *arm_why = why ? why
                                      : (tco_match_tail_ok(e) ? NULL
                                                              : TC_MATCH_ARM);
            for (uint32_t i = 0; i < e->as.match_.n_arms; i++) {
                tc_check(ctx, fd, fn_cname, e->as.match_.arms[i].guard,
                         TC_GUARD, fn_block);
                tc_check(ctx, fd, fn_cname, e->as.match_.arms[i].body,
                         arm_why, fn_block);
            }
            return;
        case EX_ASCRIBE:
            tc_check(ctx, fd, fn_cname, e->as.ascribe_.inner,
                     why ? why : TC_ASCRIBE, fn_block);
            return;
        case EX_RETURN:
            tc_check(ctx, fd, fn_cname, e->as.return_.value,
                     why ? why : TC_RETURN, fn_block);
            return;
        case EX_WHILE:
            tc_check(ctx, fd, fn_cname, e->as.while_.cond, TC_WHILE, fn_block);
            tc_check(ctx, fd, fn_cname, e->as.while_.body, TC_WHILE, fn_block);
            return;
        case EX_BUILTIN: {
            /* Every arithmetic and comparison operator is a builtin, so this is
             * the node a call-used-as-a-value most often sits under:
             * `(+ 1 (f x))` is the canonical "not a tail call".  The LAST
             * operand of `and` / `or` is the exception: it is the form's
             * value (tco_builtin_short_circuit). */
            bool sc = tco_builtin_short_circuit(e);
            for (uint32_t i = 0; i < e->as.builtin.n; i++) {
                bool last = sc && i + 1 == e->as.builtin.n;
                tc_check(ctx, fd, fn_cname, e->as.builtin.args[i],
                         last ? why : (sc ? TC_SC_TEST : TC_ARG), fn_block);
            }
            return;
        }
        case EX_DYN_OP:
            for (uint32_t i = 0; i < e->as.dyn_op_.n_args; i++)
                tc_check(ctx, fd, fn_cname, e->as.dyn_op_.args[i], TC_ARG, fn_block);
            return;
        case EX_DYN_CALL:
            /* T6 (T-D6): a dynamic call in tail position bounces or drives, so
             * the only refusals are positional -- plus a function-level one
             * that keeps its body off the tail path.  A CPS-lowered body is not
             * such a refusal here: the CPS emitter spells its tail dynamic
             * calls itself. */
            if (e->as.dyn_call_.wants_tailcall &&
                !e->as.dyn_call_.tailcall_diagnosed) {
                const char *reason = why ? why
                    : (fn_block && strcmp(fn_block, TC_CPS_BODY) != 0 ? fn_block : NULL);
                if (reason) {
                    e->as.dyn_call_.tailcall_diagnosed = true;
                    diag_emit_with_code(DIAG_ERROR, e->span,
                                        TUR_E0716_TAILCALL_NOT_TAIL,
                                        "`^tailcall` call is not in tail position: %s",
                                        reason);
                }
            }
            tc_check(ctx, fd, fn_cname, e->as.dyn_call_.fn, TC_ARG, fn_block);
            for (uint32_t i = 0; i < e->as.dyn_call_.n_args; i++)
                tc_check(ctx, fd, fn_cname, e->as.dyn_call_.args[i], TC_ARG, fn_block);
            return;
        default:
            /* Other node kinds carry sub-expressions this walker does not
             * enumerate (there are 120 ExprKinds).  A `^tailcall` buried in one
             * of those goes unchecked rather than mis-reported; every shape a
             * loop is actually written in is covered above. */
            return;
    }
}

/* Called from emit_fn_def once tco_mark has run (or been skipped).  `fn_block`
 * is the enclosing function's own reason for carrying no backedge at all, or
 * NULL when the function itself was eligible. */
static void tc_verify_fn(EmitCtx *ctx, FnDef *fd, const char *fn_cname,
                         const char *fn_block) {
    tc_check(ctx, fd, fn_cname, fd->body, NULL, fn_block);
}

/* Forward decl: the shared return-spelling decision (defined beside
 * emit_fn_def, where its helpers are all in scope). */
static void emit_fn_return_spelling(EmitCtx *ctx, Buf *out, const Expr *fn_e,
                                    FnDef *fd, const Expr *tail_e,
                                    char *ret_val, TypeKind result_kind,
                                    bool is_main);

/* Forward decl: tail-position emitter (mutually recursive). */
static void emit_tail(EmitCtx *ctx, Buf *body, const Expr *fn_e, FnDef *fd,
                      const Expr *e, TypeKind result_kind, bool is_main);

/* proper-tail-calls T4 (T-D3): fire every defer frame this function has open,
 * innermost first, before leaving it through a backedge or a `return` on the
 * tail path.  Only the drop-glue blocks emit_tail_drop_hoist_do opens can be
 * on the chain here -- emit_tail is entered with no frame open, and every
 * other frame is closed by the emitter that opened it -- so this is exactly the
 * scope-exit `tur_frame_fire_lifo` those blocks would have run after the call.
 * The same inlined walk emit_panic_signal_return uses for the panic exit. */
static void emit_tail_fire_frames(EmitCtx *ctx, Buf *body) {
    for (const char *f = ctx->frame_var; f; f = emit_frame_parent(f)) {
        indent_buf(body, ctx->indent);
        buf_printf(body, "tur_frame_fire_lifo(&%s);\n", f);
    }
}

/* void-self-tail-call-not-lowered: leave a `: nil` function from its tail
 * path.  Everything a backedge releases on the way round is released here on
 * the way out -- the open drop-glue frames and the enclosing scopes' `any`
 * locals, which emit_tail's inline `let` arm deferred to the exit. */
static void emit_tail_void_return(EmitCtx *ctx, Buf *body) {
    emit_tail_fire_frames(ctx, body);
    emit_any_scope_drops(ctx, body);
    indent_buf(body, ctx->indent);
    buf_puts(body, "return;\n");
}

/* Emit a self-tail-call as a backedge: evaluate all args into temporaries
 * first (so argument expressions still see the *old* parameter values, which
 * matters for reorderings/swaps), reassign the C parameter variables, then
 * `goto __tur_tailcall;`. */
static void emit_tail_backedge(EmitCtx *ctx, Buf *body, const Expr *fn_e,
                               FnDef *fd, const Expr *call) {
    uint32_t n = fd->n_params;
    /* Closure thunk: params[0] is the env, which the self-call reuses unchanged
     * -- reassign only params[env..n) from the call's args[0..). */
    uint32_t off = tco_env_offset(fd);
    char **tmps = n ? (char **)calloc(n, sizeof(char *)) : NULL;
    for (uint8_t i = (uint8_t)off; i < n; i++) {
        char *av = emit_value(ctx, body, call->as.call_.args[i - off]);
        char *t = fresh_tmp(ctx);
        Type pty = tco_param_type(ctx, fn_e, fd, i);
        const char *pcty = emit_type_c_name(ctx, pty);
        /* gcc14-int-conversion (carrier-representation-tracking, reverse straddle
         * at the tail-backedge): the param slot is the int64 carrier but the arg
         * value is a bare temp recorded as a concrete pointer (a heap-producer
         * call result, e.g. `int64_t __t160 = __ps_159;` where __ps_159 is a
         * `tur_adt_Cons__int *`).  Reinterpret the pointer to the int64 carrier --
         * value-preserving, and only fires for a genuine recorded pointer temp
         * into an int64 param slot. */
        bool av_is_bare = av && (av[0] == '_' || isalpha((unsigned char)av[0]));
        if (av_is_bare)
            for (const char *p = av + 1; *p && av_is_bare; p++)
                if (!(*p == '_' || isalnum((unsigned char)*p))) av_is_bare = false;
        if (pcty && strcmp(pcty, "int64_t") == 0 && av_is_bare) {
            const char *lvty = emit_localvar_lookup_ctype(av);
            size_t lL = lvty ? strlen(lvty) : 0;
            if (lvty && lL >= 1 && lvty[lL - 1] == '*' && strcmp(lvty, "void *") != 0) {
                Buf b; buf_init(&b);
                buf_printf(&b, "(int64_t)(intptr_t)(%s)", av);
                buf_putc(&b, '\0');
                free(av);
                av = strdup(b.data);
                buf_free(&b);
            }
        }
        indent_buf(body, ctx->indent);
        buf_printf(body, "%s %s = %s;\n", pcty, t, av);
        tmps[i] = t;
        free(av);
    }
    /* T4: the arguments are all read, so an owned local's drop glue runs now --
     * tco_mark admitted the block only because none of them keeps it alive. */
    emit_tail_fire_frames(ctx, body);
    for (uint8_t i = (uint8_t)off; i < n; i++) {
        char *pn = raw_name_for_binding(fd->params[i]);
        indent_buf(body, ctx->indent);
        buf_printf(body, "%s = %s;\n", pn, tmps[i]);
        free(pn);
        free(tmps[i]);
    }
    free(tmps);
    /* any-struct-box-leak-per-widen: the back-edge re-enters the function, so
     * every trailing free between here and the loop head is skipped.  Drop the
     * enclosing scopes' `any` locals now -- after the argument temporaries
     * above, which is where a use of one would have been read. */
    emit_any_scope_drops(ctx, body);
    indent_buf(body, ctx->indent);
    buf_puts(body, "goto __tur_tailcall;\n");
}

/* proper-tail-calls T5 (T-D5): a tail call to member `t` of the current
 * mutual tail-call group -- the function itself included -- inside the group's
 * fused function.  The same order as emit_tail_backedge: every argument into a
 * temp first (they read the CURRENT member's locals, which the slots do not
 * alias, but a self jump's arguments may read the very parameters being
 * replaced), then the frame chain and the `any` drops, then the target's slots,
 * then the jump. */
static void emit_tail_group_jump(EmitCtx *ctx, Buf *body, const Expr *call,
                                 int t) {
    const TcgGroup *g = ctx->tcg_cur->g;
    uint32_t base = g->slot_base[t];
    uint32_t n = g->mem[t]->n_params;
    char **tmps = n ? (char **)calloc(n, sizeof(char *)) : NULL;
    for (uint32_t k = 0; k < n; k++) {
        char *av = emit_value(ctx, body, call->as.call_.args[k]);
        tmps[k] = fresh_tmp(ctx);
        indent_buf(body, ctx->indent);
        buf_printf(body, "%s %s = %s;\n", g->slot_ctype[base + k], tmps[k], av);
        free(av);
    }
    emit_tail_fire_frames(ctx, body);
    emit_any_scope_drops(ctx, body);
    for (uint32_t k = 0; k < n; k++) {
        indent_buf(body, ctx->indent);
        buf_printf(body, "__tcg_s%u = %s;\n", (unsigned)(base + k), tmps[k]);
        free(tmps[k]);
    }
    free(tmps);
    indent_buf(body, ctx->indent);
    buf_printf(body, "__tcg_st = %d;\n", t);
    indent_buf(body, ctx->indent);
    buf_puts(body, "goto __tcg_top;\n");
}

/* A#1 (return position): emit a tail/return value, wrapping it in EX_FN_TO_FAT
 * when the enclosing function carries the ^fat result marker and the value is a
 * bare non-capturing fn (TY_FN).  This makes the returned value a heap fat
 * closure { thunk, env } so a fat-call consumer reads a valid layout instead of
 * a bare function pointer -- symmetric with the ^fat parameter auto-shim.  A
 * capturing closure (already TY_PTR_VOID) or any non-fn value passes through. */
static char *emit_fat_return_value(EmitCtx *ctx, Buf *body, const Expr *fn_e,
                                   const Expr *e) {
    if (fn_e && fn_e->type.kind == TY_FN && fn_e->type.as.fn.result_fat &&
        e->type.kind == TY_FN) {
        Expr shim = {0};
        shim.kind = EX_FN_TO_FAT;
        shim.type = e->type;
        shim.span = e->span;
        shim.as.fn_to_fat_.inner = (Expr *)e;
        return emit_value(ctx, body, &shim);
    }
    return emit_value(ctx, body, e);
}

/* CONV-S1 seam 4: the single question that decides an inline-C function's C
 * return type -- does it return its declared aggregate BY VALUE, or through the
 * int64 carrier?  Under the defstruct-as-defadt lowering a by-value
 * record/product result (`Pos` -> `tur_adt_Pos`) is a concrete aggregate, so an
 * inline-C body returns it by value (`Pos r; return r;`) and the C signature
 * must say so, matching the dict slot.  A parametric ADT-app with no single C
 * layout, a :heap ADT (a pointer), and a carrier-ABI ADT (a multi-variant sum
 * returned as the int64 handle) all stay on the carrier.
 *
 * Factored out of two hand-duplicated copies (the definition signature here and
 * the forward-decl mirror in emit_module.c) so they cannot drift, and so the
 * CONSUMER side can ask the same question instead of guessing from the type's
 * shape -- see fn_body_tail_byvalue_carrier_type in emit_expr.c.  Declared in
 * emit_internal.h. */
bool inline_c_returns_byvalue_adt(EmitCtx *ctx, bool body_is_inline_c,
                                  const Type *rft) {
    if (!body_is_inline_c || !rft) return false;
    Type rft_r = emit_resolve_type(ctx, *rft);
    return (rft_r.kind == TY_ADT || rft_r.kind == TY_APP) &&
           !type_uses_carrier_abi(rft_r) &&
           !type_is_heap_adt(rft_r) &&
           type_has_concrete_codegen_layout(&rft_r);
}

/* M5 straddle (root cause C of m5-suite-residual-6-failures): true when every
 * tail leaf of `e` is a call that emits an int64 carrier value -- a
 * ^construct helper (some/ok/err/none) or a typeclass-method impl
 * (__inst_*), both of which return the carrier handle regardless of their
 * declared (Option A)/(Result A B) type.  Used to decide whether a by-value
 * carrier-aggregate return needs a carrier->concrete deref bridge.  Shared
 * with emit_module.c's forward-decl mirror via emit_internal.h. */
bool fn_body_tail_is_carrier_producer(const Expr *e) {
    if (!e) return false;
    switch (e->kind) {
        case EX_ASCRIBE:
            return fn_body_tail_is_carrier_producer(e->as.ascribe_.inner);
        case EX_DO:
            return e->as.do_.n > 0 &&
                   fn_body_tail_is_carrier_producer(e->as.do_.items[e->as.do_.n - 1]);
        case EX_IF:
            return e->as.if_.else_or_null &&
                   fn_body_tail_is_carrier_producer(e->as.if_.then_) &&
                   fn_body_tail_is_carrier_producer(e->as.if_.else_or_null);
        case EX_LET:
        case EX_LETREC:
            return fn_body_tail_is_carrier_producer(e->as.let_.body);
        case EX_CALL: {
            const Binding *b = e->as.call_.fn_binding;
            if (!b) return false;
            if (b->is_construct_template) return true;
            if (b->name && b->name->name &&
                strncmp(b->name->name, "__inst_", 7) == 0) {
                /* consolidation increment 2 (method-result bridging): the
                 * "__inst_ returns the carrier regardless of its declared
                 * type" premise is stale for a PURE-TURMERIC instance method
                 * whose declared result is a concrete by-value product -- the
                 * M7 by-value path emits it returning the aggregate
                 * (`tur_adt_FzW __inst_FzT_thru_FzW(tur_adt_FzW)`), and
                 * classifying it as a carrier producer made the spec-call arg
                 * path deref the aggregate as if it were a pointer
                 * (class-method-result-into-generic-invalid-c; the let-bind
                 * workaround dodged this classifier, which is why it worked).
                 * Inline-C instance bodies still lower to the int64 carrier
                 * and multi-variant/parametric/tyvar results stay on the
                 * carrier -- those keep the producer classification. */
                if (!b->body_is_inline_c && b->type.kind == TY_FN &&
                    b->type.as.fn.result_full_type) {
                    Type rr = *b->type.as.fn.result_full_type;
                    if ((rr.kind == TY_ADT || rr.kind == TY_APP) &&
                        !type_uses_carrier_abi(rr) &&
                        !type_is_heap_adt(rr) && !type_is_heap_struct(rr) &&
                        type_has_concrete_codegen_layout(&rr))
                        return false;
                }
                return true;
            }
            /* result-bridge-tail-call-from-pure-tur-to-inline-c: an inline-C
             * body whose declared return type uses the carrier ABI is lowered
             * with an int64_t C return type, so a tail call to it yields the
             * carrier handle.  A pure-Turmeric wrapper around such a helper
             * needs the same carrier->by-value bridge that the ^construct
             * and __inst_ producers above already get.
             * See docs/archive/history/tail-call-inline-c-carrier-bridge.md. */
            if (b->body_is_inline_c && b->type.kind == TY_FN &&
                b->type.as.fn.result_full_type &&
                type_uses_carrier_abi(*b->type.as.fn.result_full_type))
                return true;
            /* CONV-S1 seam 4 (return-bridge): under the defstruct-as-defadt
             * lowering a parametric ADT app (`(Result cstr cstr)`) is a BY-VALUE
             * type, so `type_uses_carrier_abi` reports false -- yet an inline-C
             * body is still EMITTED with an int64 carrier return (emit_fns.c's
             * signature path lowers an inline-C TY_APP result to int64_t, since it
             * is neither a typed pointer nor a TY_STRUCT).  A pure-Turmeric wrapper
             * whose own return is the by-value aggregate (`capture` -> the int64
             * `captures-get`) therefore still needs the carrier->by-value bridge.
             * Recognise a non-heap TY_APP inline-C result as a carrier producer. */
            if (b->body_is_inline_c && b->type.kind == TY_FN &&
                b->type.as.fn.result_full_type &&
                b->type.as.fn.result_full_type->kind == TY_APP &&
                !type_is_heap_struct(*b->type.as.fn.result_full_type) &&
                !type_is_heap_adt(*b->type.as.fn.result_full_type))
                return true;
            return false;
        }
        default:
            return false;
    }
}

/* CONV-S1 seam 4 (return-bridge): true when the body tail is a direct call to an
 * INLINE-C function whose by-value ADT-app result is nonetheless EMITTED as the
 * int64 carrier.  Under the defstruct-as-defadt lowering a parametric ADT app
 * (`(Result cstr cstr)`) is a by-value type, so `type_uses_carrier_abi` reports
 * false -- but emit_fns.c still lowers an inline-C body with a TY_APP result to an
 * int64_t C return, so a pure-Turmeric wrapper whose own return is the by-value
 * aggregate returns that int64 into the aggregate slot.  This narrowly widens the
 * M5 straddle gate (whose `type_uses_carrier_abi(body type)` test misses the
 * lowered case) to exactly that shape -- NOT to an if/construct merge tail (which
 * the (a) fix already lowers by value, so unboxing it would be wrong).  At default
 * (no lowering) the inline-C result is the carrier rep and the M5 gate already
 * handles it, so this stays inert. */
static bool fn_body_tail_returns_carrier_value(EmitCtx *ctx, const Expr *e) {
    (void)ctx;
    if (!e) return false;
    switch (e->kind) {
        case EX_ASCRIBE:
            return fn_body_tail_returns_carrier_value(ctx, e->as.ascribe_.inner);
        /* NOTE: do NOT descend into EX_DO/EX_LET/EX_LETREC here.  Under the
         * defstruct-as-defadt lowering those control forms now bridge their own
         * tail value into a by-value merge temp (emit_expr.c's
         * bridge_control_value_to_byvalue_temp), so the merge result is already
         * the by-value aggregate and `return <temp>` needs no further unbox.
         * Descending would re-fire the M5 carrier->concrete deref on a value that
         * is no longer the carrier (the assignment-straddle double-bridge). */
        case EX_CALL: {
            const Binding *b = e->as.call_.fn_binding;
            if (!b || b->type.kind != TY_FN || !b->type.as.fn.result_full_type)
                return false;
            Type rft = *b->type.as.fn.result_full_type;
            return b->body_is_inline_c && rft.kind == TY_APP
                && !type_uses_carrier_abi(rft)
                && !type_is_heap_struct(rft) && !type_is_heap_adt(rft);
        }
        default:
            return false;
    }
}

/* Descend a function body to its linear tail expression, following let/do
 * bodies and ascriptions.  Stops at a branching form (if/match) or a leaf. */
static const Expr *fn_body_linear_tail(const Expr *e) {
    while (e) {
        if (e->kind == EX_ASCRIBE) { e = e->as.ascribe_.inner; continue; }
        if (e->kind == EX_LET || e->kind == EX_LETREC) { e = e->as.let_.body; continue; }
        if (e->kind == EX_DO && e->as.do_.n > 0) {
            e = e->as.do_.items[e->as.do_.n - 1]; continue;
        }
        break;
    }
    return e;
}

/* Find the initializer of binding `b` in `e`'s enclosing let chain, or NULL. */
static const Expr *fn_body_binding_init(const Expr *e, const Binding *b) {
    while (e) {
        if (e->kind == EX_ASCRIBE) { e = e->as.ascribe_.inner; continue; }
        if (e->kind == EX_LET || e->kind == EX_LETREC) {
            for (uint32_t i = 0; i < e->as.let_.n; i++)
                if (e->as.let_.bindings[i].binding == b)
                    return e->as.let_.bindings[i].init;
            e = e->as.let_.body; continue;
        }
        if (e->kind == EX_DO && e->as.do_.n > 0) {
            e = e->as.do_.items[e->as.do_.n - 1]; continue;
        }
        break;
    }
    return NULL;
}

/* True when the linear tail of `e` is a `catch-unwind` / `catch-panic-of` heap
 * box -- directly, through a let-bound variable whose initializer is one, or
 * (catch-unwind-return-bridge-residuals Part C) through an `if`/`match` all of
 * whose arms are themselves catch-box tails.  Every such tail hands back the
 * int64 carrier box, so a by-value Result/Option return needs the same
 * carrier->concrete bridge whether or not a branch sits between the tail and
 * the return.  The `if`/`match` recursion requires EVERY arm to be a catch box
 * (a mixed branch, where one arm is an ordinary constructor, is left to the
 * M4c/M5 by-value-producer paths). */
static bool expr_tail_is_catch_box(const Expr *fnbody, const Expr *e) {
    const Expr *tail = fn_body_linear_tail(e);
    if (!tail) return false;
    if (tail->kind == EX_CATCH_UNWIND || tail->kind == EX_CATCH_PANIC_OF)
        return true;
    if (tail->kind == EX_VAR && tail->as.var.binding) {
        const Expr *init = fn_body_binding_init(fnbody, tail->as.var.binding);
        return init && (init->kind == EX_CATCH_UNWIND ||
                        init->kind == EX_CATCH_PANIC_OF);
    }
    if (tail->kind == EX_IF && tail->as.if_.else_or_null)
        return expr_tail_is_catch_box(fnbody, tail->as.if_.then_) &&
               expr_tail_is_catch_box(fnbody, tail->as.if_.else_or_null);
    if (tail->kind == EX_MATCH && tail->as.match_.n_arms > 0) {
        for (uint32_t i = 0; i < tail->as.match_.n_arms; i++)
            if (!expr_tail_is_catch_box(fnbody, tail->as.match_.arms[i].body))
                return false;
        return true;
    }
    return false;
}

/* catch-unwind-return-bridge-residuals (Part B): true when EVERY box the
 * catch-box tail of `e` can produce is provably sole-owned, so the source box
 * can be freed after the return bridge copies its fields out -- with no double
 * free and no dangling alias.
 *   - a DIRECT catch-unwind / catch-panic-of tail is a fresh anonymous box that
 *     nothing else can reference -> always sole-owned;
 *   - a let-bound VAR tail is sole-owned iff the box escapes nowhere but this
 *     return (the escape walk ignoring the tail use finds no other reference);
 *   - an if/match tail is sole-owned iff every arm is.
 * Conservative: a shape it cannot prove returns false, preserving the leak. */
static bool catch_box_tail_sole_owned(const Expr *fnbody, const Expr *e) {
    const Expr *tail = fn_body_linear_tail(e);
    if (!tail) return false;
    if (tail->kind == EX_CATCH_UNWIND || tail->kind == EX_CATCH_PANIC_OF)
        return true;
    if (tail->kind == EX_VAR && tail->as.var.binding) {
        const Expr *init = fn_body_binding_init(fnbody, tail->as.var.binding);
        if (!init || (init->kind != EX_CATCH_UNWIND &&
                      init->kind != EX_CATCH_PANIC_OF))
            return false;
        return !catch_box_binding_escapes_except(fnbody, tail->as.var.binding,
                                                 tail);
    }
    if (tail->kind == EX_IF && tail->as.if_.else_or_null)
        return catch_box_tail_sole_owned(fnbody, tail->as.if_.then_) &&
               catch_box_tail_sole_owned(fnbody, tail->as.if_.else_or_null);
    if (tail->kind == EX_MATCH && tail->as.match_.n_arms > 0) {
        for (uint32_t i = 0; i < tail->as.match_.n_arms; i++)
            if (!catch_box_tail_sole_owned(fnbody, tail->as.match_.arms[i].body))
                return false;
        return true;
    }
    return false;
}

/* catch-unwind-byvalue-result-return-mismatch: true when the function returns a
 * by-value Result/Option aggregate (`ret_ctype` is that struct) but its tail
 * value is a `catch-unwind` / `catch-panic-of` heap box -- returned directly,
 * through a let-bound variable, or through an `if`/`match` whose arms are all
 * catch boxes (Part C).  `tur_catch_unwind_box` ALWAYS yields the int64 heap
 * box (never a by-value struct), and the declared by-value return type is a
 * distinct monomorph, so `return <box>` is an int64->struct cc error without a
 * carrier->concrete bridge.  Gating on the catch-box tail structurally --
 * rather than on emit_type_c_name, which reports "int64_t" for a by-value
 * ^construct tail (`ok`/`err`/`some`/`none`) too -- is what keeps this from
 * mis-firing on the ordinary Result constructors the M4c/M5 branches own. */
static bool fn_return_needs_carrier_result_bridge(EmitCtx *ctx, const FnDef *fd,
                                                  const Expr *fn_e,
                                                  const char *ret_ctype,
                                                  bool ret_is_int64_carrier,
                                                  const char *ret_val) {
    if (ret_is_int64_carrier || !ret_ctype || !fd || !fd->body) return false;
    if (fd->body->type.kind == TY_NEVER) return false;
    if (strchr(ret_ctype, '*') || strcmp(ret_ctype, "int64_t") == 0 ||
        strcmp(ret_ctype, "void") == 0)
        return false;
    if (expr_tail_is_catch_box(fd->body, fd->body)) return true;
    /* generic-vec-read-wrapper-spec-returns-carrier-word: a generic wrapper
     * whose tail is a RAW container read --
     *
     *   (defn get-it [A] [v : (Vec A) i : int] : A (vec-get v i))
     *
     * -- specialized to a CE_BOX element.  `vec-get` hands back the slot word,
     * which for a boxed by-value element is the element's heap-box pointer, so
     * the spec's tail is an `int64_t` where its declared result is the element
     * aggregate: `incompatible types when returning type 'int64_t'`, a hard cc
     * error for a shape `tur check` accepts.  A direct
     * `(:: (vec-get v 0) (Option Pt))` at a concrete site gets the box readback
     * from the ascription bridge; the spec's return position had no
     * counterpart.  Route it through the same bridge, which emits the same
     * NULL-guarded deref the concrete site gets.
     *
     * A CE_WORD element (a niche option, the default since 2026-09-03) is
     * excluded and must be: there the slot word IS the value and the readback
     * would unbox a box that was never there -- the same distinction the
     * carrier->concrete bridge's own niche row draws, asked here about the
     * DECLARED result rather than about a marked temp. */
    if (emit_call_is_raw_slot_read(fd->body)) {
        /* The DECLARED result, never the body's type: `vec-get`'s result is
         * already lowered to the int64 carrier, so `fd->body->type` reads back
         * as plain `int` and would answer this question about the wrong type. */
        if (!fn_e || fn_e->type.kind != TY_FN ||
            !fn_e->type.as.fn.result_full_type)
            return false;
        Type rt = emit_resolve_type(ctx, *fn_e->type.as.fn.result_full_type);
        return repr_of(&rt, REPR_POS_CONTAINER_ELEM) == REPR_BOXED_AGG;
    }
    /* generic-wrapper-tail-forwarding-a-return-dispatch-method: the tail is a
     * call whose emitted temp is the int64 carrier -- a typeclass method whose
     * DECLARED result is the class tyvar, so every instance returns the carrier
     * (`__inst_DecodeJson_decode_hyjson_int` -> int64_t) -- while this spec's
     * declared result is the by-value aggregate:
     *
     *   (defn decode [A] [(DecodeJson A)] [doc : int val : int] : (Result A cstr)
     *     (decode-json doc val))
     *
     * Exactly the raw-slot-read case above one seam over, and the same fix: a
     * concrete `(:: (decode-json d v) (Result int cstr))` gets the box readback
     * from the ascription bridge, and the spec's RETURN position had no
     * counterpart, so it emitted `return <int64_t>;` from a struct-returning
     * function -- a hard cc error for a shape `tur check` accepts.
     *
     * The recorded-ctype test is the same one the pointer-return straddle below
     * uses.  Reaching here with an int64 temp and an aggregate return type is
     * always the miscompile: `return <int64_t>` into a by-value struct has no
     * valid reading, so this can only turn a guaranteed cc failure into the
     * readback the value actually needs.  The bridge's own ownership mark
     * decides whether the box is freed after the copy, so a borrowed carrier
     * (a container's element) is read without being released. */
    if (ret_val && emit_str_is_bare_ident(ret_val)) {
        const char *rec = emit_localvar_lookup_ctype(ret_val);
        if (rec && strcmp(rec, "int64_t") == 0) {
            if (!fn_e || fn_e->type.kind != TY_FN ||
                !fn_e->type.as.fn.result_full_type)
                return false;
            Type rt = emit_resolve_type(ctx, *fn_e->type.as.fn.result_full_type);
            return repr_of(&rt, REPR_POS_RESULT) == REPR_BYVAL_AGG;
        }
    }
    return false;
}

/* macos-int-conversion-carrier-pointer-straddles (case B): true when the
 * function's tail expression is EMITTED as the int64 carrier, decided from the
 * typed AST rather than by sniffing the emitted C text.
 *
 * The `(int64_t)`-prefix / localvar-table sniff the return bridge below uses
 * cannot see three shapes that reach it, all of which straddle a pointer
 * return type:
 *
 *   static void * thru_hyfat(int64_t v)          { return v; }
 *   static void * __fn_1374(void *p)             { return __env->c; }
 *   static tur_adt_Cons__int * mk_hylist(void)   { return cons(...); }
 *
 * A parameter, a closure-env field read and a builtin call are all absent from
 * the localvar table, and registering them there is unsafe (that table is keyed
 * by bare C name and reset per PROGRAM, so one function's `v` would define the
 * type every other function's `v` resolves to).  Ask the AST instead:
 *
 *  - EX_VAR: a fn-typed binding is carried as the int64 fn-ABI carrier both as
 *    a parameter (the TY_FN param branch below) and as a closure-env field
 *    (emit_expr.c pins TY_FN captures to int64_t).  Two spellings opt out and
 *    keep a concrete C type -- a rank-2 poly param (`tur_poly_fn_t`) and a
 *    cfnptr (a typed C function pointer) -- so both are excluded.
 *  - EX_BUILTIN `cons`: the preamble cons-cell helper returns the int64
 *    carrier.  Keyed on `c_op` the way elab_call.c:3351 and emit_core.c:3516
 *    already key their carrier-ABI special cases for it, NOT on
 *    `result_type.kind == TY_INT` generally -- a builtin's declared result kind
 *    does not pin its C helper's return type (`tur_cloneable_cont_clone`
 *    declares TY_INT and returns `tur_cloneable_cont *`), and a rule that
 *    claimed "carrier" there would cast a pointer to a pointer through
 *    intptr_t.
 *
 * Deliberately narrow: it must never claim "carrier" for a value that is
 * already the pointer, or the bridge would paper over a genuinely mis-selected
 * monomorph (the failure mode `data-literal-nested` documents). */
static bool fn_tail_emits_int64_carrier(const FnDef *fd, const Expr *body) {
    if (!fd || !body) return false;
    if (body->kind == EX_VAR) {
        Binding *b = body->as.var.binding;
        if (!b || b->is_poly_fn) return false;
        if (b->type.kind != TY_FN || b->type.as.fn.cfnptr) return false;
        for (uint32_t i = 0; i < fd->n_params; i++)
            if (fd->params[i] == b)
                return fd->param_types[i].kind == TY_FN &&
                       !fd->param_types[i].as.fn.cfnptr;
        if (fd->closure && !b->is_global)
            for (uint8_t i = 0; i < fd->closure->n_captures; i++)
                if (fd->closure->captures[i] == b) return true;
        return false;
    }
    if (body->kind == EX_BUILTIN) {
        const BuiltinSpec *spec = body->as.builtin.spec;
        return spec && spec->shape == BS_FUNC_CALL && spec->c_op &&
               strcmp(spec->c_op, "cons") == 0;
    }
    return false;
}

/* proper-tail-calls T3 (T-D4): what emit_tail hands the match emitter so an arm
 * body can be emitted back through emit_tail.  The four values are constant for
 * one function body; they ride here rather than in EmitCtx so that a nested
 * `match` cannot pick up an outer one's. */
typedef struct {
    const Expr *fn_e;
    FnDef      *fd;
    TypeKind    result_kind;
    bool        is_main;
} MatchTailEnv;

static void emit_tail_match_arm(EmitCtx *ctx, Buf *body, const Expr *arm_body,
                                void *env) {
    const MatchTailEnv *mt = (const MatchTailEnv *)env;
    emit_tail(ctx, body, mt->fn_e, mt->fd, arm_body, mt->result_kind,
              mt->is_main);
}

/* proper-tail-calls T4 (T-D3): a `do` block whose only defers are trailing
 * drop glue on locals dead at every exit (tco_mark set `tail_drop_hoist`).
 * Opened exactly as emit_do_value opens one -- frame, the statements before
 * the value, the defer thunks -- and then the value is emitted in TAIL
 * position with the frame still open.  Every exit below fires it
 * (emit_tail_fire_frames): a backedge after its argument temps, a `return`
 * after its value is in a temp.  A panic exit fires it through
 * emit_panic_signal_return, which is why the frame stays. */
static void emit_tail_drop_hoist_do(EmitCtx *ctx, Buf *body, const Expr *fn_e,
                                    FnDef *fd, const Expr *e,
                                    TypeKind result_kind, bool is_main) {
    int last = tco_drop_glue_tail_idx(e);
    const char *saved_frame = ctx->frame_var;
    char *frame_var = fresh_frame(ctx);
    ctx->frame_var = frame_var;
    emit_frame_note_parent(frame_var, saved_frame);
    indent_buf(body, ctx->indent);
    buf_printf(body, "tur_frame %s;\n", frame_var);
    indent_buf(body, ctx->indent);
    if (saved_frame)
        buf_printf(body, "tur_frame_init(&%s, &%s);\n", frame_var, saved_frame);
    else
        buf_printf(body, "tur_frame_init(&%s, NULL);\n", frame_var);
    for (int i = 0; i < last; i++)
        emit_stmt(ctx, body, e->as.do_.items[i]);
    for (uint32_t i = (uint32_t)last + 1; i < e->as.do_.n; i++)
        emit_frame_push_defer(ctx, body, frame_var, e->as.do_.items[i]);
    emit_tail(ctx, body, fn_e, fd, e->as.do_.items[last], result_kind, is_main);
    ctx->frame_var = saved_frame;
    free(frame_var);
}

/* proper-tail-calls T2b (T-D2): may this checkless tail call `return v;` also
 * carry `musttail`?  The attribute is a hard contract -- the C compiler must
 * make the call a tail call or reject the program -- so everything it
 * requires is checked here, against text and records, not inferred:
 *
 *   - `v` is exactly a direct call `name(args)`, optionally in parentheses,
 *     with nothing around it (a cast or a wrapper is not a call), and both
 *     functions take at least one parameter (a zero-parameter function is
 *     declared `f()`, which C99 does not count as a prototype);
 *   - caller and callee have IDENTICAL recorded C signatures -- same arity,
 *     same spelling of every parameter and of the return type, as the
 *     forward-declaration pass wrote them.  T2 already proved the return
 *     types match; musttail needs the parameters too;
 *   - no parameter is a `const T *` pass-by-pointer aggregate, which is a
 *     pointer into some caller's frame by construction;
 *   - no address is taken ANYWHERE in the body emitted so far, nor in the
 *     call.  This is the one condition an optimizer checks for itself and
 *     musttail skips: a tail call reuses the caller's frame, so an argument
 *     pointing into it -- a stack fat box for a non-retaining sink, a frame
 *     `any` box, a defer frame, a pass-by-pointer temp -- would dangle.  The
 *     emitter builds all of those with a `&`, so refusing any `&` (other than
 *     `&&`) is sound, if blunt.
 *
 * Refusing costs nothing: the call is still T2's `return f(args);`. */
/* musttail-indirect-aggregate-arg-dangles-on-aarch64: may a parameter of
 * this C type ride a `musttail` call?  Yes for a pointer, a scalar, and the
 * 16-byte `tur_tagged_t`; no for any other by-value aggregate.  AAPCS64
 * passes an aggregate over 16 bytes INDIRECTLY -- the caller makes a copy
 * in its own frame and passes its address -- and a musttail call's frame is
 * gone by the time the callee reads that copy.  SysV x86-64 passes the same
 * aggregate in the outgoing argument area, which musttail reuses safely, so
 * only arm64 saw it: `__dynwit_Comb_comb_Two` forwarding its 32-byte
 * `tur_adt_Two__any` printed nothing and panicked on macOS
 * (saffron-class-fn-extra, #1007).  A by-value ADT's size is not visible in
 * its spelling, so every aggregate other than `tur_tagged_t` is refused;
 * the call is still an ordinary `return f(args);`. */
static bool musttail_param_in_registers(const char *c) {
    if (!c || !*c) return false;
    if (strchr(c, '*')) return true;
    static const char *const ok[] = {
        "tur_tagged_t", "int64_t", "int32_t", "int16_t", "int8_t",
        "uint64_t", "uint32_t", "uint16_t", "uint8_t", "double", "float",
        "bool", "_Bool", "char", "int", "unsigned", "long", "size_t",
        "intptr_t", "uintptr_t", NULL
    };
    for (int i = 0; ok[i]; i++)
        if (strcmp(c, ok[i]) == 0) return true;
    return false;
}

static bool tail_call_musttail_ok(EmitCtx *ctx, const Buf *body, const char *v) {
    if (!ctx->mt_fn_cname || !v || body != ctx->mt_body_buf) return false;
    const char *q = v;
    size_t open = 0;
    while (*q == '(') { q++; open++; }
    size_t idlen = 0;
    while (q[idlen] && (isalnum((unsigned char)q[idlen]) || q[idlen] == '_')) idlen++;
    if (idlen == 0 || idlen >= 256 || q[idlen] != '(') return false;
    char callee[256];
    memcpy(callee, q, idlen);
    callee[idlen] = '\0';
    /* The argument list must close, and only the wrapping parens follow it. */
    int depth = 0;
    const char *p = q + idlen;
    for (; *p; p++) {
        if (*p == '(') depth++;
        else if (*p == ')' && --depth == 0) { p++; break; }
    }
    if (depth != 0) return false;
    for (size_t i = 0; i < open; i++, p++) if (*p != ')') return false;
    if (*p != '\0') return false;

    /* A zero-parameter function is declared `f()` -- in C99 an UNPROTOTYPED
     * declaration, not `f(void)` -- and clang refuses musttail on a call
     * whose caller or callee has no prototype. */
    int n = emit_sig_lookup_n_params(ctx->mt_fn_cname);
    if (n <= 0 || n != emit_sig_lookup_n_params(callee)) return false;
    const char *rc = emit_sig_lookup_ret_ctype(ctx->mt_fn_cname);
    const char *rg = emit_sig_lookup_ret_ctype(callee);
    if (!rc || !rg || strcmp(rc, rg) != 0) return false;
    for (int i = 0; i < n; i++) {
        const char *a = emit_sig_lookup_param_ctype(ctx->mt_fn_cname, (uint32_t)i);
        const char *b = emit_sig_lookup_param_ctype(callee, (uint32_t)i);
        if (!a || !b || strcmp(a, b) != 0) return false;
        if (strncmp(a, "const ", 6) == 0) return false;
        if (!musttail_param_in_registers(a)) return false;
    }
    const char *segs[2] = { body->data + ctx->mt_body_start, v };
    size_t lens[2] = { body->len - ctx->mt_body_start, strlen(v) };
    if (ctx->mt_body_start > body->len) return false;
    for (int k = 0; k < 2; k++)
        for (size_t i = 0; i < lens[k]; i++) {
            if (segs[k][i] != '&') continue;
            if (i + 1 < lens[k] && segs[k][i + 1] == '&') { i++; continue; }
            return false;
        }
    return true;
}

/* proper-tail-calls T6: a bouncer's static tail call to another bouncer
 * (tb_static_tail_callee) hands its arming on.  When this activation was
 * entered from a trampoline driver, arm the callee just before the call: it
 * consumes the arming on entry, and a bounce it hands back is this
 * function's own value, which goes straight to the same driver.  `v` is the
 * call expression; its arguments are already evaluated temps and plain reads,
 * except that a second call to the callee inside them would consume the
 * arming first -- refused.  The callee must be emitted with the entry check
 * (fn_may_bounce), and return the driver's `tur_tagged_t`. */
static void emit_tail_hand_on_arming(EmitCtx *ctx, Buf *body, const Expr *e,
                                     const char *v) {
    if (!ctx->tb_guard || !v || !ctx->current_fn_ret_ctype ||
        strcmp(ctx->current_fn_ret_ctype, "tur_tagged_t") != 0)
        return;
    const FnDef *cd = tb_static_tail_callee(e);
    if (!cd || !fn_may_bounce(cd)) return;
    const char *q = v;
    while (*q == '(') q++;
    size_t idlen = 0;
    while (q[idlen] && (isalnum((unsigned char)q[idlen]) || q[idlen] == '_')) idlen++;
    if (idlen == 0 || idlen >= 256 || q[idlen] != '(') return;
    char callee[256];
    memcpy(callee, q, idlen);
    callee[idlen] = '\0';
    const char *rg = emit_sig_lookup_ret_ctype(callee);
    if (!rg || strcmp(rg, "tur_tagged_t") != 0) return;
    for (const char *p = strstr(q + idlen, callee); p; p = strstr(p + 1, callee)) {
        char before = p[-1];
        char after = p[idlen];
        if (!(isalnum((unsigned char)before) || before == '_') &&
            !(isalnum((unsigned char)after) || after == '_'))
            return;
    }
    indent_buf(body, ctx->indent);
    buf_printf(body, "if (%s) tur_tb_armed_for = (void *)%s;\n", ctx->tb_guard, callee);
}

/* Emit `e` in tail position: every path ends in `return <v>;` or a backedge
 * `goto __tur_tailcall;`.  Only invoked for functions tco_mark flagged. */
static void emit_tail(EmitCtx *ctx, Buf *body, const Expr *fn_e, FnDef *fd,
                      const Expr *e, TypeKind result_kind, bool is_main) {
    switch (e->kind) {
        case EX_BUILTIN:
            /* tail-grammar-skips-and-or-and-carrier-lets: `(and a ... z)` is
             * `if (!a) return false; ... <tail z>`, and `(or a ... z)` is
             * `if (a) return true; ... <tail z>` -- the lowering the
             * equivalent `if` already gets.  The early answer goes back
             * through this same function as a literal, so it takes the
             * ordinary return path (frames, drops, the return spelling). */
            if (tco_builtin_short_circuit(e)) {
                bool is_or = e->as.builtin.spec->shape == BS_OR_SC;
                uint32_t n = e->as.builtin.n;
                /* Read only for the duration of the emit_tail calls below. */
                Expr lit_e;
                memset(&lit_e, 0, sizeof(lit_e));
                lit_e.kind = EX_BOOL_LIT;
                lit_e.type = TYPE_BOOL;
                lit_e.span = e->span;
                lit_e.as.b = is_or;
                const Expr *lit = &lit_e;
                for (uint32_t i = 0; i + 1 < n; i++) {
                    char *cond = emit_value(ctx, body, e->as.builtin.args[i]);
                    indent_buf(body, ctx->indent);
                    buf_printf(body, "if (%s(%s)) {\n", is_or ? "" : "!", cond);
                    free(cond);
                    ctx->indent += 4;
                    emit_tail(ctx, body, fn_e, fd, lit, result_kind, is_main);
                    ctx->indent -= 4;
                    indent_buf(body, ctx->indent);
                    buf_puts(body, "}\n");
                }
                emit_tail(ctx, body, fn_e, fd, e->as.builtin.args[n - 1],
                          result_kind, is_main);
                return;
            }
            break;
        case EX_CALL:
            /* T5: inside a fused group every backedge -- to itself or to a
             * sibling -- is a jump through the group's dispatch, since each
             * member's parameters are block locals of one C function. */
            if (ctx->tcg_cur && e->as.call_.is_tail_self_call) {
                emit_tail_group_jump(ctx, body, e, ctx->tcg_cur->idx);
                return;
            }
            if (ctx->tcg_cur && e->as.call_.tail_group_idx) {
                emit_tail_group_jump(ctx, body, e, e->as.call_.tail_group_idx - 1);
                return;
            }
            if (e->as.call_.is_tail_self_call) {
                emit_tail_backedge(ctx, body, fn_e, fd, e);
                return;
            }
            break;  /* non-self call -> default return path */
        case EX_IF:
            if (e->as.if_.else_or_null) {
                char *cond = emit_value(ctx, body, e->as.if_.cond);
                indent_buf(body, ctx->indent);
                buf_printf(body, "if (%s) {\n", cond);
                free(cond);
                ctx->indent += 4;
                emit_tail(ctx, body, fn_e, fd, e->as.if_.then_, result_kind, is_main);
                ctx->indent -= 4;
                indent_buf(body, ctx->indent);
                buf_puts(body, "} else {\n");
                ctx->indent += 4;
                emit_tail(ctx, body, fn_e, fd, e->as.if_.else_or_null, result_kind, is_main);
                ctx->indent -= 4;
                indent_buf(body, ctx->indent);
                buf_puts(body, "}\n");
                return;
            }
            if (ctx->tail_void) {
                /* void-self-tail-call-not-lowered: the one-armed `if` of a
                 * `: nil` body -- the absent arm is the empty return. */
                char *cond = emit_value(ctx, body, e->as.if_.cond);
                indent_buf(body, ctx->indent);
                buf_printf(body, "if (%s) {\n", cond);
                free(cond);
                ctx->indent += 4;
                emit_tail(ctx, body, fn_e, fd, e->as.if_.then_, result_kind, is_main);
                ctx->indent -= 4;
                indent_buf(body, ctx->indent);
                buf_puts(body, "}\n");
                emit_tail_void_return(ctx, body);
                return;
            }
            break;
        case EX_DO: {
            bool has_defer = false;
            for (uint32_t i = 0; i < e->as.do_.n; i++)
                if (e->as.do_.items[i]->kind == EX_DEFER) { has_defer = true; break; }
            if (!has_defer && e->as.do_.n > 0) {
                for (uint32_t i = 0; i + 1 < e->as.do_.n; i++)
                    emit_stmt(ctx, body, e->as.do_.items[i]);
                emit_tail(ctx, body, fn_e, fd, e->as.do_.items[e->as.do_.n - 1],
                          result_kind, is_main);
                return;
            }
            if (has_defer && e->as.do_.tail_drop_hoist) {
                emit_tail_drop_hoist_do(ctx, body, fn_e, fd, e, result_kind,
                                        is_main);
                return;
            }
            break;
        }
        case EX_LET:
        case EX_LETREC:
            if (tco_let_simple(ctx, e)) {
                indent_buf(body, ctx->indent);
                buf_puts(body, "{\n");
                ctx->indent += 4;
                /* This arm emits a tail-position `let` INLINE rather than
                 * through emit_let_value, because nothing may follow the body:
                 * every path through it ends in a `return` or a backedge.  The
                 * declarations are emit_let_value's own
                 * (emit_let_binding_decl) -- this arm used to repeat a subset
                 * of that ladder, one straddle at a time
                 * (tail-recursive-let-drops-carrier-bridge,
                 * let-bound-erasing-ascription-int-to-pointer, gcc14-int-
                 * conversion), which is why a carrier-ABI binding was kept off
                 * the tail path altogether
                 * (tail-grammar-skips-and-or-and-carrier-lets).
                 *
                 * The releases emit_let_value runs after the body ride the
                 * `any` scope-drop channel instead, which the backedge and
                 * every `return` fire -- any-struct-box-leak-per-widen started
                 * it for `any` locals, and let_binding_push_scope_frees adds the
                 * rest.  No trailing drop is emitted: anything after the body
                 * would be dead code. */
                uint32_t any_mark = ctx->n_any_scope_drops;
                for (uint32_t i = 0; i < e->as.let_.n; i++) {
                    const Binding *b = e->as.let_.bindings[i].binding;
                    char *bn = name_for_binding(ctx, b);
                    char *iv = emit_value(ctx, body, e->as.let_.bindings[i].init);
                    LetBindDecl d;
                    iv = emit_let_binding_decl(ctx, body, e, i, bn, iv, &d);
                    indent_buf(body, ctx->indent);
                    buf_printf(body, "(void)%s;\n", bn);
                    free(bn);
                    free(iv);
                    /* union-tagged-union-c-emission 1b: the channel carries the
                     * drop STATEMENT, so a union local's plain free reaches the
                     * early exits this arm is entirely made of. */
                    if (let_binding_any_freeable(ctx, e, i)) {
                        char *nm = let_binding_widen_drop_stmt(ctx, e, i);
                        any_scope_drops_push(ctx, nm);
                        free(nm);
                    }
                    if (let_binding_may_need_scope_free(ctx, e, i))
                        let_binding_push_scope_frees(ctx, e, i, &d);
                }
                emit_tail(ctx, body, fn_e, fd, e->as.let_.body, result_kind, is_main);
                any_scope_drops_pop(ctx, any_mark);
                ctx->indent -= 4;
                indent_buf(body, ctx->indent);
                buf_puts(body, "}\n");
                return;
            }
            break;
        case EX_MATCH: {
            /* proper-tail-calls T3 (T-D4): emit the match through its ordinary
             * emitter, with `match_tail` set so each ARM routes back into
             * emit_tail and ends in a `return` or a backedge.
             *
             * Deliberately NOT a second copy of the match emitter: `match` has
             * five arm shapes (ADT constructors, `any` narrowing, literals,
             * session offers, and the guarded variants), and a parallel
             * implementation of the pattern tests is exactly the drift T-D4
             * warns about.  The hook changes one statement per shape and leaves
             * every pattern test, binder and guard where it is.
             *
             * The result temp and the `goto <end>` after each arm stay too.
             * They become unread and unreachable rather than absent, which
             * costs nothing and buys two things: no unused-label or
             * unused-variable warning, and the emitter's existing
             * no-arm-matched fall-through -- the temp's zero init -- is still
             * what this function returns if the match is not exhaustive. */
            if (!tco_match_tail_ok(e)) break;
            MatchTailEnv env = { fn_e, fd, result_kind, is_main };
            MatchTailCtx mt = { e, emit_tail_match_arm, &env };
            const MatchTailCtx *save = ctx->match_tail;
            ctx->match_tail = &mt;
            char *v = emit_value(ctx, body, e);
            ctx->match_tail = save;
            emit_tail_fire_frames(ctx, body);
            indent_buf(body, ctx->indent);
            if (is_main && result_kind == TY_INT)
                buf_printf(body, "return (int)%s;\n", v);
            else
                buf_printf(body, "return %s;\n", v);
            free(v);
            return;
        }
        default:
            break;
    }

    /* void-self-tail-call-not-lowered: a leaf of a `: nil` body is a
     * statement followed by the empty return -- there is no value to spell,
     * and a non-self call is not made a C tail call (`want_tail_call` below
     * refuses a void return anyway). */
    if (ctx->tail_void) {
        emit_stmt(ctx, body, e);
        emit_tail_void_return(ctx, body);
        return;
    }

    /* proper-tail-calls T2 (T-D2): a non-self call in tail position becomes a
     * genuine C tail `return f(args);` -- no `__ps_N` temp, and no
     * `if (tur_panicking) return ...;` after it.  The check is redundant here:
     * a tail call does nothing with the value, so a panic beneath it is caught
     * by the nearest enclosing NON-tail frame, and there always is one.  (It is
     * also only ever observable inside a `catch-unwind` at all -- with no
     * handler installed `tur_panic` prints and aborts.)
     *
     * Requested only when nothing on the way out would put work after the call.
     * Two layers answer that, and they divide along a useful line:
     *
     *   - HERE, the properties of the ENCLOSING function that make its return a
     *     transformation rather than a forward -- `main`'s cast, a `void` return
     *     (emitted as a statement plus a bare `return`), a `box_aggregate_result`
     *     or dict-clone wrapper (both of which spill or bit-reinterpret on the
     *     way out), the by-value-carrier spill, the MB2 tyvar cast, an open `any`
     *     scope-drop list whose frees follow the value, and
     *     `panic_signal_is_break` (inside the stackless trampoline the signal
     *     must reach the driver's unwind loop as a `break`; dropping it would
     *     abandon the live continuation chain).
     *   - IN emit_value, the one property of the CALL: its callee's C return type
     *     must be exactly this function's, which is what proves the shared return
     *     ladder will forward the value rather than bridge it.  That is a genuine
     *     tail-position condition, not conservatism -- a return that has to spill
     *     or cast is work after the call.
     *
     * emit_value may also refuse because an argument left an owned box pending or
     * a region bracket is open; `tail_call_no_hoist_taken` reports any refusal
     * back and the ordinary hoisted spelling is what lands. */
    bool want_tail_call =
        e->kind == EX_CALL && e->as.call_.is_tail_call && !is_main &&
        !ctx->frame_var &&
        !ctx->panic_signal_is_break && ctx->n_any_scope_drops == 0 &&
        e->type.kind != TY_NIL && e->type.kind != TY_NEVER &&
        ctx->current_fn_ret_ctype &&
        strcmp(ctx->current_fn_ret_ctype, "void") != 0 &&
        !fd->box_aggregate_result && fd->n_dict_clone == 0 &&
        !fn_body_tail_emits_byvalue_carrier_abi(ctx, e) &&
        !(strchr(ctx->current_fn_ret_ctype, '*') != NULL &&
          emit_tail_call_returns_tyvar_carrier(ctx, e));
    const Expr *nh_save = ctx->tail_call_no_hoist;
    bool taken_save = ctx->tail_call_no_hoist_taken;
    if (want_tail_call) {
        ctx->tail_call_no_hoist = e;
        ctx->tail_call_no_hoist_taken = false;
    }

    /* T4: with a drop-glue frame open, the value must be in a temp BEFORE the
     * frame fires -- `v` may be an expression that reads the very local being
     * dropped (`*b`).  Routing it through a one-item `do` gets exactly the temp
     * and carrier bridges emit_do_value gives a block's value on the ordinary
     * path, which is what that path handed the return ladder here before.  A
     * diverging tail (a `panic`) is left alone: it fires the chain itself. */
    /* dynamic-returned-closure-env-is-never-freed: the same holds for the
     * `any` locals of the enclosing tail-position lets (emit_tail's inline
     * `let` arm pushes them and emits no trailing drop, leaving it to "the
     * return").  A backedge and a void return fire them; this value return
     * did not, so every `any` a tail `let` owned leaked on the way out.  The
     * value goes into a temp first for the same reason as a frame's: `v` may
     * read the local being dropped. */
    bool any_drops_open = ctx->n_any_scope_drops > 0;
    Expr *one[1] = { (Expr *)e };
    Expr wrap = {0};
    bool wrap_value = (ctx->frame_var || any_drops_open) && e->type.kind != TY_NEVER;
    if (wrap_value) {
        wrap.kind = EX_DO;
        wrap.type = e->type;
        wrap.span = e->span;
        wrap.as.do_.items = one;
        wrap.as.do_.n = 1;
    }
    const Expr *ve = wrap_value ? &wrap : e;

    /* proper-tail-calls T6 (T-D6): a dynamic call as the function's whole
     * answer.  It bounces when this activation was entered from a trampoline
     * driver (`ctx->tb_guard`), and otherwise DRIVES -- makes the call in a
     * loop that takes the callee's own bounces -- so a chain of dynamic tail
     * calls runs in one frame either way.  Bouncing needs nothing to run after
     * the return: no open defer frame (its drops would precede the call) and
     * no `any` scope drops (the same, for the frees). */
    if (e->kind == EX_DYN_CALL && !is_main && ctx->current_fn_ret_ctype &&
        strcmp(ctx->current_fn_ret_ctype, "tur_tagged_t") == 0) {
        bool can_bounce = ctx->tb_guard && !ctx->frame_var &&
                          ctx->n_any_scope_drops == 0;
        ctx->dyn_tail_mode = can_bounce ? DYN_TAIL_DIRECT : DYN_TAIL_DRIVE;
        ctx->dyn_tail_guard = can_bounce ? ctx->tb_guard : NULL;
    }

    /* Default: emit as a value and return it. */
    char *v = emit_fat_return_value(ctx, body, fn_e, ve);
    ctx->dyn_tail_mode = DYN_TAIL_NONE;   /* never outlives the one call */
    ctx->dyn_tail_guard = NULL;
    if (wrap_value) {
        emit_tail_fire_frames(ctx, body);
        if (any_drops_open) emit_any_scope_drops(ctx, body);
    }
    bool emitted_tail_call = want_tail_call && ctx->tail_call_no_hoist_taken;
    ctx->tail_call_no_hoist = nh_save;
    ctx->tail_call_no_hoist_taken = taken_save;
    if (emitted_tail_call) {
        /* `v` is the call expression itself.  Nothing between it and the
         * return, which is the whole point.  T2b: where the C compiler can
         * guarantee the tail call, ask it to (`TUR_MUSTTAIL`). */
        bool mt = tail_call_musttail_ok(ctx, body, v);
        if (mt) {
            ensure_musttail_macro(ctx);
            emit_musttail_self_pin(body, ctx->indent, ctx->mt_fn_cname);
        }
        emit_tail_hand_on_arming(ctx, body, e, v);
        indent_buf(body, ctx->indent);
        buf_printf(body, "%sreturn %s;\n", mt ? "TUR_MUSTTAIL " : "", v);
        free(v);
        return;
    }
    if (e->type.kind == TY_NIL) {
        /* A nil-typed tail in a value-returning function: there is no value to
         * bridge, so the zero of the result kind goes back directly.  (The
         * shared ladder's predicates all key on the tail's type, which here
         * says `nothing`.) */
        free(v);
        indent_buf(body, ctx->indent);
        if (is_main && result_kind == TY_INT)
            buf_printf(body, "return (int)%s;\n",
                       result_kind == TY_BOOL ? "false" : "0");
        else
            buf_printf(body, "return %s;\n",
                       result_kind == TY_BOOL ? "false" : "0");
        return;
    }
    /* emit-tail-return-path-lacks-carrier-bridges: ONE decision, shared with
     * emit_fn_def.  This arm used to carry a hand-maintained subset of that
     * ladder -- the by-value carrier spill, the pass-by-ptr deref and the MB2
     * tyvar cast, three of sixteen arms -- so a TCO-routed body whose result
     * wanted any of the other thirteen was emitted unbridged.  `e`, not
     * `fd->body`, is the tail: in this path the value being returned is the
     * branch/arm actually in tail position, which is the more precise reading
     * of every result-shape question the ladder asks. */
    emit_fn_return_spelling(ctx, body, fn_e, fd, ve, v, result_kind, is_main);
}

/* Scalar kinds the trampoline can round-trip through an int64 `saved[]` slot:
 * machine int, bool, non-owning pointer-repr values (cstr, raw ptr) -- via an
 * intptr_t cast -- and float/float32 -- via BIT reinterpretation.  Carrier /
 * opaque / aggregate types are excluded on purpose (ownership / RC concerns). */
static bool sc_scalar_kind(TypeKind k) {
    return k == TY_INT || k == TY_BOOL || k == TY_CSTR || k == TY_PTR_VOID ||
           k == TY_FLOAT || k == TY_FLOAT32;
}

/* Build "the int64 bits of VAL" for a scalar of kind K (caller frees). */
static char *sc_save_expr(TypeKind k, const char *val) {
    const char *fn = (k == TY_FLOAT) ? "tur_sc_bits_f64"
                   : (k == TY_FLOAT32) ? "tur_sc_bits_f32" : NULL;
    size_t n = strlen(val) + 64;
    char *s = (char *)malloc(n);
    if (fn) snprintf(s, n, "%s(%s)", fn, val);
    else    snprintf(s, n, "(int64_t)(intptr_t)(%s)", val);
    return s;
}

/* Build "the value of C type CTYPE recovered from the int64 SAVED" (caller frees). */
static char *sc_restore_expr(const char *ctype, TypeKind k, const char *saved) {
    size_t n = strlen(saved) + (ctype ? strlen(ctype) : 0) + 64;
    char *s = (char *)malloc(n);
    if (k == TY_FLOAT)        snprintf(s, n, "tur_sc_f64_from_bits(%s)", saved);
    else if (k == TY_FLOAT32) snprintf(s, n, "tur_sc_f32_from_bits(%s)", saved);
    else                      snprintf(s, n, "(%s)(intptr_t)(%s)", ctype, saved);
    return s;
}

/* G6 (compiled-catch-unwind-general-lowering): decide whether a value of type T
 * can ride the trampoline's int64 `saved[]` slot, and with which (kind, ctype)
 * the sc_save/restore casts should treat it.  Scalars round-trip directly;
 * anything whose C representation is a plain `int64_t` -- carrier handles,
 * `defopaque` newtypes over an int/handle, `:fn` function pointers, result/
 * option boxes -- rides the same slot through an intptr cast (kind forced to
 * TY_INT so the float bit-path is not taken).  This matches native, which keeps
 * such a param live across the recursive call by value with no retain/drop; the
 * node relocates it to the heap, not into a different ownership regime.  A
 * genuine by-value aggregate (a C struct / by-ptr ADT param) is NOT int64 and
 * still bails to the fallback path. */
static bool gs_slot_type(EmitCtx *ctx, Type t, TypeKind *out_kind, const char **out_ctype) {
    const char *ct = emit_type_c_name(ctx, t);
    if (sc_scalar_kind(t.kind)) { *out_kind = t.kind; *out_ctype = ct; return true; }
    if (ct && strcmp(ct, "int64_t") == 0) { *out_kind = TY_INT; *out_ctype = ct; return true; }
    return false;
}

/* Classify a PARAMETER type for the trampoline.  Beyond gs_slot_type (scalar /
 * int64 carrier), a by-value AGGREGATE -- a C struct passed by value, whose
 * ctype is a bare struct name (no `*`, not int64/scalar) -- is accepted and
 * flagged *out_aggr; it is relocated across a descend by heap-boxing (see
 * gs_save/gs_restore).  A by-const-pointer aggregate (a large by-value product
 * passed as `const <struct> *`, e.g. a `(Result int int)` param) is ALSO
 * accepted and additionally flagged *out_ref: its pointee -- which lives in the
 * caller's collapsed C frame -- is materialized on the heap across a descend and
 * re-homed into a stable function-scope buffer on resume.  For both aggregate
 * classes *out_ctype names the (pointee) struct so sizeof works.  Returns false
 * for anything not rideable. */
static bool gs_param_class(EmitCtx *ctx, Type t, TypeKind *out_kind,
                           const char **out_ctype, bool *out_aggr, bool *out_ref) {
    *out_ref = false;
    if (gs_slot_type(ctx, t, out_kind, out_ctype)) { *out_aggr = false; return true; }
    const char *ct = emit_type_c_name(ctx, t);
    /* type_c_name yields the bare struct name for BOTH a small by-value product
     * (passed by value) and a large one (passed as `const <struct> *`); the ABI
     * choice is type_struct_pass_by_ptr, not a `*` in the name.  Use the Type to
     * tell them apart rather than string-matching the ctype. */
    if (ct && ct[0] && !strchr(ct, '*')) {   /* a by-value struct name */
        *out_kind = TY_INT; *out_ctype = ct; *out_aggr = true;
        *out_ref = type_struct_pass_by_ptr(emit_resolve_type(ctx, t));
        return true;
    }
    return false;
}

/* ============================================================================
 * G3 -- general stackless catch-unwind segment splitter
 * ----------------------------------------------------------------------------
 * The scaffold (above) matches one fixed grammar.  This splitter lowers an
 * ARBITRARY catch-crossing self-recursive body into heap-continuation segments
 * driven by a `for(;;)` / `switch(__pc)` trampoline, so multiple catch sites,
 * non-tail self-recursion (`(+ 1 (f ...))`), a catch whose thunk actually
 * panics, and nested if/let/do all run with a flat C stack.
 *
 * Model (mirrors docs/artifacts/d3-stackless-catch-unwind.c):
 *   - Every function-scope scalar local (params + hoisted let-vars + one result
 *     temp per suspension) rides a `tur_cont` node's saved[] across a descend.
 *   - A "suspension" is a self-call or a catch-unwind: its value is produced on
 *     a later driver iteration.  A suspension nested inside a pure expression is
 *     hoisted into a temp, and the enclosing expression is re-emitted in the
 *     resume segment with the suspension replaced by that temp (an emit_value
 *     "hole", ctx->sub_holes).
 *   - `__k` is always the continuation the current segment's value returns to.
 *     Descending pushes a child node whose `next` is `__k`; a RETURN sets
 *     `__pc = __k->tag` so the driver re-enters the matching resume segment.
 *   - A pending panic unwinds by popping boundary-less nodes until a catch node
 *     (or DONE -> uncaught abort), exactly as the D1a signal contract wants.
 * ==========================================================================*/

/* G4 (cross-function / mutual recursion): the active "trampolined group" -- the
 * set of functions whose calls to one another descend through one shared driver
 * instead of C-calling.  A singleton group is the G3 single-function case.  Set
 * before the structural predicates / collection / emission run, cleared after;
 * the predicates treat a call to ANY member as a suspension. */
#define GS_MAXMEM 8
static FnDef *g_gs_mem[GS_MAXMEM];
static int    g_gs_nmem;
/* BR3b: the EmitCtx active while the trampoline gate/emission predicates run.
 * Set alongside g_gs_nmem at each entry point.  The gate helpers gs_value_ok /
 * gs_stmt_ok take only (Expr, FnDef), so the by-ref/reader classification --
 * which needs gs_param_class(ctx, ...) -- reads the context from here. */
static EmitCtx *g_gs_ctx;

/* If e is a direct full-arity call to a group member, its index; else -1. */
static int gs_call_member_idx(const Expr *e) {
    if (!e || e->kind != EX_CALL || e->as.call_.fn_expr) return -1;
    for (int i = 0; i < g_gs_nmem; i++)
        if (g_gs_mem[i] && e->as.call_.fn_binding == g_gs_mem[i]->binding &&
            e->as.call_.n_args == g_gs_mem[i]->n_params) return i;
    return -1;
}

/* A call whose target is trampolined (self- or cross-recursion): descends
 * through the driver.  fd is retained for call-site compatibility only. */
static bool gs_is_self_call(const Expr *e, FnDef *fd) {
    (void)fd;
    return gs_call_member_idx(e) >= 0;
}

/* The 0-arg thunk FnDef of a catch-unwind, or NULL.  A capture-free thunk is
 * fat/poly-wrapped (EX_POLY_WRAP / EX_FN_TO_FAT) or cast-erased before it
 * reaches the catch site; peel those to the underlying EX_FN / EX_CLOSURE.
 * Re-emitting the inner body in the enclosing context resolves any captures to
 * their param C names (captures share the outer binding). */
static FnDef *gs_thunk_fn(const Expr *cu) {
    const Expr *th = cu->as.catch_unwind_.thunk;
    for (int guard = 0; th && guard < 8; guard++) {
        switch (th->kind) {
            case EX_FN:      return th->as.fn_.fn;
            case EX_CLOSURE: return th->as.closure_.closure ? th->as.closure_.closure->fn : NULL;
            case EX_VAR: {
                /* A capture-free thunk is lambda-lifted to a top-level defn and
                 * referenced by name; recover its FnDef from the binding. */
                Binding *b = th->as.var.binding;
                return (b && b->source_fn_def) ? b->source_fn_def : NULL;
            }
            case EX_POLY_WRAP: th = th->as.poly_wrap_.inner; break;
            case EX_FN_TO_FAT: th = th->as.fn_to_fat_.inner; break;
            case EX_CAST:      th = th->as.cast_.expr; break;
            case EX_REINTERPRET:   th = th->as.reinterpret_.expr; break;
            case EX_ASCRIBE:   th = th->as.ascribe_.inner; break;
            default: return NULL;
        }
    }
    return NULL;
}

/* Does e contain a suspension anywhere (ignoring hole state)?  Used for
 * eligibility and var collection, before any hole is registered. */
static bool gs_suspends(const Expr *e, FnDef *fd) {
    if (!e) return false;
    if (gs_is_self_call(e, fd)) return true;
    if (e->kind == EX_CATCH_UNWIND) return true;
    switch (e->kind) {
        case EX_IF:
            return gs_suspends(e->as.if_.cond, fd) ||
                   gs_suspends(e->as.if_.then_, fd) ||
                   gs_suspends(e->as.if_.else_or_null, fd);
        case EX_LET: {
            for (uint32_t i = 0; i < e->as.let_.n; i++)
                if (gs_suspends(e->as.let_.bindings[i].init, fd)) return true;
            return gs_suspends(e->as.let_.body, fd);
        }
        case EX_DO:
            for (uint32_t i = 0; i < e->as.do_.n; i++)
                if (gs_suspends(e->as.do_.items[i], fd)) return true;
            return false;
        case EX_BUILTIN:
            for (uint32_t i = 0; i < e->as.builtin.n; i++)
                if (gs_suspends(e->as.builtin.args[i], fd)) return true;
            return false;
        case EX_CALL:
            for (uint32_t i = 0; i < e->as.call_.n_args; i++)
                if (gs_suspends(e->as.call_.args[i], fd)) return true;
            return false;
        case EX_CAST:    return gs_suspends(e->as.cast_.expr, fd);
        case EX_REINTERPRET: return gs_suspends(e->as.reinterpret_.expr, fd);
        case EX_ASCRIBE: return gs_suspends(e->as.ascribe_.inner, fd);
        case EX_PANIC_PAYLOAD_TYPE:  return gs_suspends(e->as.panic_payload_type_.payload, fd);
        case EX_PANIC_PAYLOAD_VALUE: return gs_suspends(e->as.panic_payload_value_.payload, fd);
        /* BR3a: a match/field-read/deref is a pure read admitted in value
         * position (gs_value_ok) only when it does not suspend -- so this walk
         * must descend into their sub-expressions, or a suspension buried in a
         * match arm would be missed and the whole form wrongly admitted. */
        case EX_MATCH: {
            if (gs_suspends(e->as.match_.scrutinee, fd)) return true;
            for (uint32_t i = 0; i < e->as.match_.n_arms; i++) {
                if (gs_suspends(e->as.match_.arms[i].body, fd)) return true;
                if (gs_suspends(e->as.match_.arms[i].guard, fd)) return true;
            }
            return false;
        }
        case EX_GET_FIELD: return gs_suspends(e->as.get_field_.struct_expr, fd);
        case EX_DEREF:     return gs_suspends(e->as.deref_.expr, fd);
        default:         return false;
    }
}

static bool gs_stmt_ok(const Expr *e, FnDef *fd);
static bool gs_has_panic(const Expr *e);
static bool gs_is_pure_accessor(const Expr *e);

/* ===== BR3b: pass a by-ref aggregate param to a pure const-by-ref reader =====
 *
 * BR3a admits reading a by-ref aggregate param IN PLACE (match/.field/@).  BR3b
 * admits the one remaining read-only use: handing the param to another function
 * that takes it `const`-by-ref and only reads it.  Two facts make this sound:
 *   - the callee receives a `const <struct> *` into the trampoline's stable
 *     `<cname>__agg` buffer, which outlives the call (the borrow does not
 *     escape as long as the callee only reads it -- gs_reader_use_ok proves so);
 *   - the call is FALLIBLE (the reader can panic), unlike BR3a's total reads, so
 *     emission pairs it with an `if (tur_panicking) break;` (see cps_emit_br3b).
 */

/* BR3c: bound on the transitive reader-proof recursion (depth of the chain of
 * pure readers a borrow may thread through).  `stack` holds the FnDefs on the
 * current chain so a (mutually) recursive callee is detected and rejected
 * conservatively rather than looping. */
#define GS_READER_MAX_DEPTH 8
static bool gs_callee_reads_arg_only(const Expr *e, uint32_t argidx,
                                     FnDef **stack, int depth);

/* True iff every occurrence of binding `p` in `e` is a READ: p appears only as
 * the receiver of a `.field`, a `match` scrutinee, a `@`-deref, the arg of a
 * pure accessor, or (BR3c) the arg of a further callee that likewise only reads
 * that borrow (proven transitively by gs_callee_reads_arg_only).  A bare use of
 * p anywhere else -- returned, borrowed (`(& p)`), stored, handed to a callee
 * that escapes/mutates it, or reached through an unrecognized form -- rejects.
 * `stack`/`depth` carry the transitive-proof chain (see GS_READER_MAX_DEPTH). */
static bool gs_reader_use_ok(const Expr *e, Binding *p,
                             FnDef **stack, int depth) {
    if (!e) return true;
    switch (e->kind) {
        case EX_INT_LIT: case EX_BOOL_LIT: case EX_FLOAT_LIT:
        case EX_CSTR_LIT: case EX_NIL_LIT:
            return true;
        case EX_VAR:
            /* A bare p not consumed by a read context below -> escape. */
            return e->as.var.binding != p;
        case EX_GET_FIELD: {
            const Expr *s = e->as.get_field_.struct_expr;
            if (s && s->kind == EX_VAR && s->as.var.binding == p) return true;
            return gs_reader_use_ok(s, p, stack, depth);
        }
        case EX_DEREF: {
            const Expr *s = e->as.deref_.expr;
            if (s && s->kind == EX_VAR && s->as.var.binding == p) return true;
            return gs_reader_use_ok(s, p, stack, depth);
        }
        case EX_MATCH: {
            const Expr *s = e->as.match_.scrutinee;
            if (!(s && s->kind == EX_VAR && s->as.var.binding == p) &&
                !gs_reader_use_ok(s, p, stack, depth)) return false;
            for (uint32_t i = 0; i < e->as.match_.n_arms; i++) {
                if (!gs_reader_use_ok(e->as.match_.arms[i].body, p, stack, depth)) return false;
                if (!gs_reader_use_ok(e->as.match_.arms[i].guard, p, stack, depth)) return false;
            }
            return true;
        }
        case EX_IF:
            return gs_reader_use_ok(e->as.if_.cond, p, stack, depth) &&
                   gs_reader_use_ok(e->as.if_.then_, p, stack, depth) &&
                   gs_reader_use_ok(e->as.if_.else_or_null, p, stack, depth);
        case EX_LET:
            for (uint32_t i = 0; i < e->as.let_.n; i++)
                if (!gs_reader_use_ok(e->as.let_.bindings[i].init, p, stack, depth)) return false;
            return gs_reader_use_ok(e->as.let_.body, p, stack, depth);
        case EX_DO:
            for (uint32_t i = 0; i < e->as.do_.n; i++)
                if (!gs_reader_use_ok(e->as.do_.items[i], p, stack, depth)) return false;
            return true;
        case EX_BUILTIN:
            for (uint32_t i = 0; i < e->as.builtin.n; i++)
                if (!gs_reader_use_ok(e->as.builtin.args[i], p, stack, depth)) return false;
            return true;
        case EX_CALL: {
            if (e->as.call_.fn_expr) return false;   /* indirect call: could stash p */
            bool acc = gs_is_pure_accessor(e);
            for (uint32_t i = 0; i < e->as.call_.n_args; i++) {
                const Expr *a = e->as.call_.args[i];
                if (a && a->kind == EX_VAR && a->as.var.binding == p) {
                    /* p handed to a call.  A pure accessor is a proven read.
                     * Otherwise BR3c: prove the callee treats its matching param
                     * as a read-only borrow too, transitively. */
                    if (acc) continue;
                    if (!gs_callee_reads_arg_only(e, i, stack, depth)) return false;
                } else if (!gs_reader_use_ok(a, p, stack, depth)) {
                    return false;
                }
            }
            return true;
        }
        case EX_CAST:    return gs_reader_use_ok(e->as.cast_.expr, p, stack, depth);
        case EX_ASCRIBE: return gs_reader_use_ok(e->as.ascribe_.inner, p, stack, depth);
        /* A reader may `panic` -- that fallibility is exactly what the BR3b
         * post-call break check handles; only the payload could carry p out. */
        case EX_PANIC:      return gs_reader_use_ok(e->as.panic_.payload, p, stack, depth);
        case EX_PANIC_WITH: return gs_reader_use_ok(e->as.panic_with_.payload, p, stack, depth);
        default:
            /* Unrecognized form: cannot prove p is used only as a read. */
            return false;
    }
}

/* BR3c: `e` is a non-accessor direct call, and the borrow being tracked is
 * handed to it at argument `argidx`.  Prove the callee only READS that borrow.
 * Resolve `e` to a top-level defn; then either
 *   - the callee receives that arg by-VALUE (an independent aggregate copy is
 *     made at the boundary, so the trampoline buffer cannot escape through it --
 *     accept with no recursion), or
 *   - the callee receives it by-const-REF (the same pointer): recurse with
 *     gs_reader_use_ok on the callee body to prove that callee is itself a pure
 *     reader of its param.
 * Structural guards mirror gs_is_br3b_reader_call (resolvable top-level defn,
 * fixed arity, no inline-C -- which could stash the raw pointer).  A cycle
 * (mutual/self recursion) or a depth blowout is rejected conservatively rather
 * than assuming read-only.  Reads g_gs_ctx for the classification context. */
static bool gs_callee_reads_arg_only(const Expr *e, uint32_t argidx,
                                     FnDef **stack, int depth) {
    if (!g_gs_ctx || !e || e->kind != EX_CALL || e->as.call_.fn_expr) return false;
    Binding *fb = e->as.call_.fn_binding;
    FnDef *g = (fb && fb->source_fn_def) ? fb->source_fn_def : NULL;
    if (!g || !g->body || !g->param_types) return false;
    if (g->is_variadic || g->n_params != e->as.call_.n_args) return false;
    if (g->body->kind == EX_INLINE_C) return false;         /* raw pointer access */
    if (argidx >= g->n_params) return false;
    TypeKind k; const char *ct; bool aggr, ref;
    if (!gs_param_class(g_gs_ctx, g->param_types[argidx], &k, &ct, &aggr, &ref))
        return false;
    if (!ref)
        /* by-value: an aggregate is copied at the call boundary (a scalar param
         * would not type-check against the borrow); the copy cannot alias the
         * trampoline buffer, so no transitive proof is needed. */
        return aggr;
    /* by-const-ref: the callee borrows the same pointer -- recurse, guarding
     * against a cycle (co-recursion) and runaway depth. */
    if (depth >= GS_READER_MAX_DEPTH) return false;
    for (int i = 0; i < depth; i++) if (stack[i] == g) return false;
    FnDef *nstack[GS_READER_MAX_DEPTH];
    for (int i = 0; i < depth; i++) nstack[i] = stack[i];
    nstack[depth] = g;
    return gs_reader_use_ok(g->body, g->params[argidx], nstack, depth + 1);
}

/* True iff `e` is a call that hands fd's by-const-ptr aggregate param to a
 * non-member pure const-by-ref reader: the callee resolves to a top-level defn
 * whose matching param is by-const-ptr, whose return is a scalar slot (never the
 * aggregate borrow itself), and whose body only READS that param with no
 * inline-C.  Such a call is a READ of the borrow; it rides the single-function
 * trampoline behind a tur_panicking check.  Reads g_gs_ctx for the context. */
static bool gs_is_br3b_reader_call(const Expr *e, FnDef *fd) {
    if (!g_gs_ctx || !fd) return false;
    if (!e || e->kind != EX_CALL || e->as.call_.fn_expr) return false;
    if (gs_is_self_call(e, fd)) return false;      /* members descend, not BR3b */
    if (gs_is_pure_accessor(e)) return false;      /* accessors already admitted */
    Binding *fb = e->as.call_.fn_binding;
    FnDef *g = (fb && fb->source_fn_def) ? fb->source_fn_def : NULL;
    if (!g || !g->body || !g->param_types) return false;
    if (g->is_variadic || g->n_params != e->as.call_.n_args) return false;
    if (g->body->kind == EX_INLINE_C) return false;         /* raw pointer access */
    /* scalar / int64-slot return: never the aggregate borrow the reader was lent */
    TypeKind rk; const char *rct;
    if (!gs_slot_type(g_gs_ctx, g->return_type, &rk, &rct)) return false;
    bool passes_byref = false;
    for (uint32_t i = 0; i < e->as.call_.n_args; i++) {
        const Expr *a = e->as.call_.args[i];
        bool is_fd_byref = false;
        if (a && a->kind == EX_VAR && a->as.var.binding) {
            for (uint32_t pp = 0; pp < fd->n_params; pp++) {
                if (fd->params[pp] != a->as.var.binding) continue;
                TypeKind k; const char *ct; bool aggr, ref;
                if (gs_param_class(g_gs_ctx, fd->param_types[pp], &k, &ct, &aggr, &ref) && ref)
                    is_fd_byref = true;
            }
        }
        if (is_fd_byref) {
            /* the callee must RECEIVE it by const-ptr, and only read it */
            TypeKind k; const char *ct; bool aggr, ref;
            if (!gs_param_class(g_gs_ctx, g->param_types[i], &k, &ct, &aggr, &ref) || !ref)
                return false;
            /* Seed the transitive-proof chain (BR3c) with the reader g itself so
             * a callee that loops back to g is caught as a cycle. */
            FnDef *rstack[GS_READER_MAX_DEPTH] = { g };
            if (!gs_reader_use_ok(g->body, g->params[i], rstack, 1)) return false;
            passes_byref = true;
        } else {
            /* other args: plain non-suspending values (a suspension there would
             * need the descend machinery, out of scope for a BR3b hoist) */
            if (gs_suspends(a, fd)) return false;
        }
    }
    return passes_byref;
}

/* Does `e` contain a BR3b reader call anywhere?  Used to keep a BR3b call out of
 * a value-position conditional (an if-branch / match-arm), where emission would
 * hoist it UNCONDITIONALLY -- wrong, and possibly a spurious panic.  Such an `e`
 * bails to native; BR3b calls are admitted only in unconditional value contexts
 * (directly, or under a builtin / call-arg / cast), or in a statement-position
 * branch that cps_emit splits structurally down to an unconditional leaf. */
static bool gs_has_br3b(const Expr *e, FnDef *fd) {
    if (!e) return false;
    if (gs_is_br3b_reader_call(e, fd)) return true;
    switch (e->kind) {
        case EX_IF:
            return gs_has_br3b(e->as.if_.cond, fd) ||
                   gs_has_br3b(e->as.if_.then_, fd) ||
                   gs_has_br3b(e->as.if_.else_or_null, fd);
        case EX_LET:
            for (uint32_t i = 0; i < e->as.let_.n; i++)
                if (gs_has_br3b(e->as.let_.bindings[i].init, fd)) return true;
            return gs_has_br3b(e->as.let_.body, fd);
        case EX_DO:
            for (uint32_t i = 0; i < e->as.do_.n; i++)
                if (gs_has_br3b(e->as.do_.items[i], fd)) return true;
            return false;
        case EX_BUILTIN:
            for (uint32_t i = 0; i < e->as.builtin.n; i++)
                if (gs_has_br3b(e->as.builtin.args[i], fd)) return true;
            return false;
        case EX_CALL:
            for (uint32_t i = 0; i < e->as.call_.n_args; i++)
                if (gs_has_br3b(e->as.call_.args[i], fd)) return true;
            return false;
        case EX_MATCH: {
            if (gs_has_br3b(e->as.match_.scrutinee, fd)) return true;
            for (uint32_t i = 0; i < e->as.match_.n_arms; i++) {
                if (gs_has_br3b(e->as.match_.arms[i].body, fd)) return true;
                if (gs_has_br3b(e->as.match_.arms[i].guard, fd)) return true;
            }
            return false;
        }
        case EX_GET_FIELD: return gs_has_br3b(e->as.get_field_.struct_expr, fd);
        case EX_DEREF:     return gs_has_br3b(e->as.deref_.expr, fd);
        case EX_CAST:      return gs_has_br3b(e->as.cast_.expr, fd);
        case EX_REINTERPRET:   return gs_has_br3b(e->as.reinterpret_.expr, fd);
        case EX_ASCRIBE:   return gs_has_br3b(e->as.ascribe_.inner, fd);
        default:           return false;
    }
}

/* e occupies a VALUE position: suspensions here are handled only by hole
 * hoisting (builtins / casts / self-call args / the suspension itself), never
 * pulled out of a nested if/let/do -- so a suspending if/let/do is illegal here
 * (it must sit in statement/tail position, checked by gs_stmt_ok). */
static bool gs_value_ok(const Expr *e, FnDef *fd) {
    if (!e) return true;
    /* BR3b: a reader call that shares a value expression with a suspension
     * (a self-call / catch-unwind) would be REORDERED after the suspension's
     * descend -- cps_emit_value_susp hoists the leftmost suspension first and
     * re-emits the rest (including the reader) in the resume segment.  A pure
     * accessor reorders harmlessly, but a reader can panic, so the reorder is
     * observable.  Keep them apart: such an `e` bails to native.  (The common
     * "read after a descend" -- `(+ n (describe acc))` sitting past a separate
     * catch-unwind do-item -- has no suspension in this expression, so it is
     * unaffected.) */
    if (gs_has_br3b(e, fd) && gs_suspends(e, fd)) return false;
    switch (e->kind) {
        case EX_INT_LIT: case EX_BOOL_LIT: case EX_FLOAT_LIT:
        case EX_CSTR_LIT: case EX_NIL_LIT: case EX_VAR:
            return true;
        case EX_CAST:       return gs_value_ok(e->as.cast_.expr, fd);
        case EX_REINTERPRET:    return gs_value_ok(e->as.reinterpret_.expr, fd);
        case EX_ASCRIBE:    return gs_value_ok(e->as.ascribe_.inner, fd);
        case EX_PANIC_PAYLOAD_TYPE:  return gs_value_ok(e->as.panic_payload_type_.payload, fd);
        case EX_PANIC_PAYLOAD_VALUE: return gs_value_ok(e->as.panic_payload_value_.payload, fd);
        case EX_PANIC:      return gs_value_ok(e->as.panic_.payload, fd);
        case EX_PANIC_WITH: return gs_value_ok(e->as.panic_with_.payload, fd);
        case EX_BUILTIN:
            for (uint32_t i = 0; i < e->as.builtin.n; i++)
                if (!gs_value_ok(e->as.builtin.args[i], fd)) return false;
            return true;
        case EX_IF: case EX_LET: case EX_DO:
            /* pure nesting only in value position; a BR3b call in a branch would
             * be hoisted unconditionally (see gs_has_br3b), so keep it out here
             * (statement-position if/let/do is split structurally instead). */
            return !gs_suspends(e, fd) && !gs_has_br3b(e, fd);
        /* BR3a (catch-unwind-byref-aggregate-br3-plan): widen the read gate past
         * the pure-accessor whitelist.  A `match`/destructure, a `.field` read on
         * a by-value product, and a `@`-deref are all identity-agnostic READS --
         * exactly the read-only uses of a by-const-ptr aggregate param that the
         * borrow-becomes-owned-copy contract permits.  Admit them in value
         * position when they are fully pure: no suspension (which only a
         * structural split, not the emit_value fast path, could handle) and no
         * inline `panic` (which emit_value would emit as a bare `return`,
         * escaping the driver -- gs_value_ok has no split for these forms, unlike
         * the if/let/do arms above).  emit_value then emits the whole read
         * atomically.  The unsound uses (address observation via `(& p)`, a
         * borrow escape, mutation, or a raw-pointer inline-C body) all reach
         * gs_value_ok's `default: return false` and bail the function to native.
         *
         * SINGLE-FUNCTION ONLY (g_gs_nmem == 1).  The shared cross-function group
         * driver carries a by-ref aggregate param through the int64 `__a` shim,
         * and its pbp-deref of these reads is only wired for the accessor path a
         * group already admitted -- a `match`/`.field` on a group member's by-ref
         * param mis-lowers (the pointee slot is read as a by-value struct).  The
         * plan scopes BR3 to the single-function trampoline; widening the group
         * path is a separate follow-up (aggregate-followups plan).  In a group
         * context these forms fall through to `default: return false`, so the
         * function bails to native exactly as it did before BR3a. */
        case EX_MATCH: case EX_GET_FIELD: case EX_DEREF:
            return g_gs_nmem == 1 && !gs_suspends(e, fd) && !gs_has_panic(e) &&
                   !gs_has_br3b(e, fd);
        case EX_CATCH_UNWIND: {
            FnDef *tf = gs_thunk_fn(e);
            if (!tf || !tf->body) return false;
            return gs_stmt_ok(tf->body, fd);
        }
        case EX_CALL: {
            if (gs_is_self_call(e, fd)) {
                for (uint32_t i = 0; i < e->as.call_.n_args; i++)
                    if (!gs_value_ok(e->as.call_.args[i], fd)) return false;
                return true;
            }
            if (!e->as.call_.fn_expr) {
                Binding *fb = e->as.call_.fn_binding;
                const char *nm = (fb && fb->name) ? fb->name->name : NULL;
                static const char *const pure[] = {
                    "ok?", "err?", "ok-val", "err-val", "some?", "none?", NULL };
                for (int i = 0; nm && pure[i]; i++)
                    if (strcmp(nm, pure[i]) == 0) {
                        for (uint32_t j = 0; j < e->as.call_.n_args; j++)
                            if (!gs_value_ok(e->as.call_.args[j], fd)) return false;
                        return true;
                    }
            }
            /* BR3b (catch-unwind-byref-aggregate-br3b-plan): a call that hands
             * fd's by-const-ptr aggregate param to a pure const-by-ref reader is
             * a READ of the borrow -- admit it (single-function only, like BR3a).
             * Unlike BR3a's total reads it is FALLIBLE, so emission hoists it and
             * emits `if (tur_panicking) break;` (cps_emit_br3b); admitting it here
             * only declares eligibility.  The args are checked by
             * gs_is_br3b_reader_call (the by-ref arg goes to a const-by-ref reader
             * param; other args do not suspend). */
            if (g_gs_nmem == 1 && gs_is_br3b_reader_call(e, fd)) return true;
            return false;   /* any other call could panic outside a boundary */
        }
        default:
            return false;
    }
}

/* e occupies STATEMENT/TAIL position: if/let/do are split structurally so a
 * suspension may live in a branch, a binding init, or the tail. */
static bool gs_stmt_ok(const Expr *e, FnDef *fd) {
    if (!e) return true;
    switch (e->kind) {
        case EX_IF:
            return gs_value_ok(e->as.if_.cond, fd) &&
                   gs_stmt_ok(e->as.if_.then_, fd) &&
                   gs_stmt_ok(e->as.if_.else_or_null, fd);
        case EX_LET: {
            for (uint32_t i = 0; i < e->as.let_.n; i++)
                if (!gs_value_ok(e->as.let_.bindings[i].init, fd)) return false;
            return gs_stmt_ok(e->as.let_.body, fd);
        }
        case EX_DO:
            for (uint32_t i = 0; i < e->as.do_.n; i++)
                if (!gs_stmt_ok(e->as.do_.items[i], fd)) return false;
            return true;
        default: return gs_value_ok(e, fd);
    }
}

/* Does e contain a `panic` / `panic-with` that emit_value would emit INLINE?
 * A panic is a control escape: routed through emit_value it emits a bare
 * `return` that leaves the driver's C frame directly, abandoning any live
 * boundary/continuation nodes (so an active catch would never see it).  When
 * true, cps_emit must split e structurally down to the panic instead of taking
 * the emit_value fast path, so the panic reaches the EX_PANIC arm (which emits
 * `break` -> the driver's unwind loop pops to the nearest boundary).  Does not
 * descend into a catch-unwind thunk -- that panic belongs to the thunk's own
 * segment (and a catch makes e suspend, so the fast path is off anyway). */
static bool gs_has_panic(const Expr *e) {
    if (!e) return false;
    if (e->kind == EX_PANIC || e->kind == EX_PANIC_WITH) return true;
    switch (e->kind) {
        case EX_IF:
            return gs_has_panic(e->as.if_.cond) || gs_has_panic(e->as.if_.then_) ||
                   gs_has_panic(e->as.if_.else_or_null);
        case EX_LET: {
            for (uint32_t i = 0; i < e->as.let_.n; i++)
                if (gs_has_panic(e->as.let_.bindings[i].init)) return true;
            return gs_has_panic(e->as.let_.body);
        }
        case EX_DO:
            for (uint32_t i = 0; i < e->as.do_.n; i++)
                if (gs_has_panic(e->as.do_.items[i])) return true;
            return false;
        case EX_BUILTIN:
            for (uint32_t i = 0; i < e->as.builtin.n; i++)
                if (gs_has_panic(e->as.builtin.args[i])) return true;
            return false;
        case EX_CALL:
            for (uint32_t i = 0; i < e->as.call_.n_args; i++)
                if (gs_has_panic(e->as.call_.args[i])) return true;
            return false;
        case EX_CAST:                return gs_has_panic(e->as.cast_.expr);
        case EX_REINTERPRET:             return gs_has_panic(e->as.reinterpret_.expr);
        case EX_ASCRIBE:             return gs_has_panic(e->as.ascribe_.inner);
        case EX_PANIC_PAYLOAD_TYPE:  return gs_has_panic(e->as.panic_payload_type_.payload);
        case EX_PANIC_PAYLOAD_VALUE: return gs_has_panic(e->as.panic_payload_value_.payload);
        /* BR3a: match/field-read/deref are admitted in value position by
         * gs_value_ok only when panic-free; this walk must see into their
         * sub-expressions for that check to be sound (a `panic` in a match arm
         * emitted via the emit_value fast path would escape the driver). */
        case EX_MATCH: {
            if (gs_has_panic(e->as.match_.scrutinee)) return true;
            for (uint32_t i = 0; i < e->as.match_.n_arms; i++) {
                if (gs_has_panic(e->as.match_.arms[i].body)) return true;
                if (gs_has_panic(e->as.match_.arms[i].guard)) return true;
            }
            return false;
        }
        case EX_GET_FIELD: return gs_has_panic(e->as.get_field_.struct_expr);
        case EX_DEREF:     return gs_has_panic(e->as.deref_.expr);
        default: return false;
    }
}

/* The leftmost `panic` / `panic-with` node in e (eval order), or NULL.  Used to
 * route a panic buried in a non-suspending value expression through the driver.
 * Since a panic diverges, any pure pre-panic sub-eval is dead and skipped. */
static const Expr *gs_leftmost_panic(const Expr *e) {
    if (!e) return NULL;
    if (e->kind == EX_PANIC || e->kind == EX_PANIC_WITH) return e;
    switch (e->kind) {
        case EX_IF: {
            const Expr *r = gs_leftmost_panic(e->as.if_.cond); if (r) return r;
            r = gs_leftmost_panic(e->as.if_.then_); if (r) return r;
            return gs_leftmost_panic(e->as.if_.else_or_null);
        }
        case EX_LET: {
            for (uint32_t i = 0; i < e->as.let_.n; i++) {
                const Expr *r = gs_leftmost_panic(e->as.let_.bindings[i].init); if (r) return r;
            }
            return gs_leftmost_panic(e->as.let_.body);
        }
        case EX_DO:
            for (uint32_t i = 0; i < e->as.do_.n; i++) {
                const Expr *r = gs_leftmost_panic(e->as.do_.items[i]); if (r) return r;
            }
            return NULL;
        case EX_BUILTIN:
            for (uint32_t i = 0; i < e->as.builtin.n; i++) {
                const Expr *r = gs_leftmost_panic(e->as.builtin.args[i]); if (r) return r;
            }
            return NULL;
        case EX_CALL:
            for (uint32_t i = 0; i < e->as.call_.n_args; i++) {
                const Expr *r = gs_leftmost_panic(e->as.call_.args[i]); if (r) return r;
            }
            return NULL;
        case EX_CAST:                return gs_leftmost_panic(e->as.cast_.expr);
        case EX_REINTERPRET:             return gs_leftmost_panic(e->as.reinterpret_.expr);
        case EX_ASCRIBE:             return gs_leftmost_panic(e->as.ascribe_.inner);
        case EX_PANIC_PAYLOAD_TYPE:  return gs_leftmost_panic(e->as.panic_payload_type_.payload);
        case EX_PANIC_PAYLOAD_VALUE: return gs_leftmost_panic(e->as.panic_payload_value_.payload);
        default: return NULL;
    }
}

static bool gs_has_catch(const Expr *e, FnDef *fd) {
    if (!e) return false;
    if (e->kind == EX_CATCH_UNWIND) return true;
    switch (e->kind) {
        case EX_IF:
            return gs_has_catch(e->as.if_.cond, fd) ||
                   gs_has_catch(e->as.if_.then_, fd) ||
                   gs_has_catch(e->as.if_.else_or_null, fd);
        case EX_LET: {
            for (uint32_t i = 0; i < e->as.let_.n; i++)
                if (gs_has_catch(e->as.let_.bindings[i].init, fd)) return true;
            return gs_has_catch(e->as.let_.body, fd);
        }
        case EX_DO:
            for (uint32_t i = 0; i < e->as.do_.n; i++)
                if (gs_has_catch(e->as.do_.items[i], fd)) return true;
            return false;
        case EX_BUILTIN:
            for (uint32_t i = 0; i < e->as.builtin.n; i++)
                if (gs_has_catch(e->as.builtin.args[i], fd)) return true;
            return false;
        case EX_CALL:
            for (uint32_t i = 0; i < e->as.call_.n_args; i++)
                if (gs_has_catch(e->as.call_.args[i], fd)) return true;
            return false;
        case EX_CAST:    return gs_has_catch(e->as.cast_.expr, fd);
        case EX_REINTERPRET: return gs_has_catch(e->as.reinterpret_.expr, fd);
        case EX_ASCRIBE: return gs_has_catch(e->as.ascribe_.inner, fd);
        case EX_PANIC_PAYLOAD_TYPE:  return gs_has_catch(e->as.panic_payload_type_.payload, fd);
        case EX_PANIC_PAYLOAD_VALUE: return gs_has_catch(e->as.panic_payload_value_.payload, fd);
        default:         return false;
    }
}

#define GS_MAXSEG 256
enum { GSK_RETURN = 0, GSK_ASSIGN = 1, GSK_SEQ = 2 };

/* is_aggr: a by-value aggregate (C struct) param that does not fit the int64
 * slot.  It is relocated across a descend by heap-boxing (malloc + memcpy) with
 * the slot holding the box pointer -- ctype names the struct so sizeof works.
 *
 * is_ref: an is_aggr param that is passed by CONST POINTER (`const <ctype> *`,
 * a large-enough by-value product -- type_struct_pass_by_ptr).  cname is the
 * pointer, not the struct; its pointee lives in the CALLER's C frame, which the
 * trampoline collapses.  So a descend materializes the pointee on the heap
 * (memcpy FROM the pointer, not from its address) and a resume copies it into a
 * stable function-scope buffer `<cname>__agg`, re-pointing cname at it.  The
 * borrow becomes an owned copy for the trampolined region -- sound only for a
 * read-only, identity-agnostic const borrow (the pure-accessor gate enforces
 * this). */
typedef struct { char *cname; const char *ctype; TypeKind kind; bool is_aggr; bool is_ref; } GsVar;

typedef struct GsSink {
    int kind;
    TypeKind ret_kind;                                   /* GSK_RETURN */
    const char *ret_ctype;                               /* GSK_RETURN (aggregate: struct name for sizeof) */
    bool ret_aggr;                                       /* GSK_RETURN: value is a by-value aggregate, box it into __v */
    const char *dest;                                    /* GSK_ASSIGN */
    const LetBinding *binds; uint32_t bi, bn; const Expr *body;  /* GSK_ASSIGN */
    const Expr **items; uint32_t si, sn;                 /* GSK_SEQ */
    const struct GsSink *cont;                           /* GSK_ASSIGN / GSK_SEQ */
    /* catch-unwind-panic-payload-leaks (Leak 3): C-name of a caught Result box
     * to tur_result_box_free just before this sink consumes its value.  Set on
     * the terminal sink of a let-bound catch-box continuation that is
     * straight-line and non-escaping (gs_catch_descend), so the box (and its
     * owned payload/message) is reclaimed once the reads that consume it have
     * completed -- the stackless counterpart to the native let_binding_box_freeable
     * scope-exit free.  The delivered value is proven not to alias the box (the
     * escape check greenlights only scalar-returning accessor reads), so freeing
     * before the delivery is safe. */
    const char *free_box;                                /* deferred box free */
} GsSink;

typedef struct {
    EmitCtx *ctx; FnDef *fd; TypeKind ret_kind; const char *ret_ctype;
    GsVar vars[TUR_SC_MAXN]; int n_vars;
    const Expr *susp_expr[TUR_SC_MAXN]; int susp_idx[TUR_SC_MAXN]; int n_susp;
    Buf *bufs[GS_MAXSEG]; int tags[GS_MAXSEG]; int n_segs;
    int next_tag;
    int base_ind, cur_ind;
    bool overflow;
    /* G4: group members sharing this driver.  For a singleton (G3) n_mem == 1
     * and mem[0] == fd, mem_entry_tag[0] == 1, mem_pbase[0] == 0 -- identical to
     * the single-function lowering.  A call to member m descends to its ENTRY
     * (mem_entry_tag[m]) after setting m's params (vars[mem_pbase[m]..]); the
     * resume restores the returned value as m's own return type. */
    int n_mem;
    FnDef *mem[GS_MAXMEM];
    int mem_entry_tag[GS_MAXMEM];
    int mem_pbase[GS_MAXMEM];
    TypeKind mem_ret[GS_MAXMEM];
    const char *mem_retctype[GS_MAXMEM];
    /* AR (catch-unwind-aggregate-returns): member m returns a by-value aggregate
     * (a C struct returned by value, too wide for the int64 __v register).  Its
     * return value is heap-boxed into __v on produce and unboxed on consume. */
    bool mem_ret_aggr[GS_MAXMEM];
    int n_param_vars;   /* count of leading vars[] that are member params */
} GsCtx;

static int gs_add_var_a(GsCtx *gs, char *cname, const char *ctype, TypeKind k,
                        bool is_aggr, bool is_ref) {
    if (gs->n_vars >= TUR_SC_MAXN) { gs->overflow = true; free(cname); return -1; }
    gs->vars[gs->n_vars].cname = cname;
    gs->vars[gs->n_vars].ctype = ctype;
    gs->vars[gs->n_vars].kind = k;
    gs->vars[gs->n_vars].is_aggr = is_aggr;
    gs->vars[gs->n_vars].is_ref = is_ref;
    return gs->n_vars++;
}
static int gs_add_var(GsCtx *gs, char *cname, const char *ctype, TypeKind k) {
    return gs_add_var_a(gs, cname, ctype, k, false, false);
}

static Buf *gs_add_seg(GsCtx *gs, int tag) {
    if (gs->n_segs >= GS_MAXSEG) { gs->overflow = true; return NULL; }
    Buf *b = (Buf *)malloc(sizeof(Buf));
    buf_init(b);
    gs->bufs[gs->n_segs] = b;
    gs->tags[gs->n_segs] = tag;
    gs->n_segs++;
    return b;
}

static int gs_temp_index(GsCtx *gs, const Expr *S) {
    for (int i = 0; i < gs->n_susp; i++)
        if (gs->susp_expr[i] == S) return gs->susp_idx[i];
    return -1;
}

static bool gs_is_hole(EmitCtx *ctx, const Expr *e) {
    for (uint8_t i = 0; i < ctx->n_sub_holes; i++)
        if (ctx->sub_holes[i] == e) return true;
    return false;
}

/* Collect the function-scope var table: params (added by the caller), then
 * every suspending let's bindings, then one result temp per suspension. */
static void gs_collect(GsCtx *gs, const Expr *e) {
    if (!e || gs->overflow) return;
    if (e->kind == EX_LET && gs_suspends(e, gs->fd)) {
        for (uint32_t i = 0; i < e->as.let_.n; i++) {
            const LetBinding *lb = &e->as.let_.bindings[i];
            const char *ct = emit_type_c_name(gs->ctx, lb->init->type);
            TypeKind lk = lb->init->type.kind;
            /* A caught result / option (and other carrier values) is an int64
             * handle in C even when its TypeKind is a Result/Option/App: save it
             * through the int64 slot.  Genuinely non-scalar, non-carrier locals
             * (by-value aggregates) still bail to the fallback path. */
            if (!sc_scalar_kind(lk)) {
                if (ct && strcmp(ct, "int64_t") == 0) { lk = TY_INT; }
                else { gs->overflow = true; return; }
            }
            gs_add_var(gs, atom_var(gs->ctx, lb->binding), ct, lk);
        }
    }
    int cm = gs_call_member_idx(e);
    if (cm >= 0) {
        /* Result temp typed by the CALLEE member's return type.  AR: if that
         * return is a by-value aggregate, the temp is a struct held by value and
         * is relocated across any further descend by heap-boxing (is_aggr). */
        char nm[32]; snprintf(nm, sizeof nm, "__t%d", gs->ctx->tmp_n++);
        int vi = gs_add_var_a(gs, tur_strdup(nm), gs->mem_retctype[cm], gs->mem_ret[cm],
                              gs->mem_ret_aggr[cm], false);
        if (vi >= 0 && gs->n_susp < TUR_SC_MAXN) {
            gs->susp_expr[gs->n_susp] = e; gs->susp_idx[gs->n_susp] = vi; gs->n_susp++;
        }
        for (uint32_t i = 0; i < e->as.call_.n_args; i++)
            gs_collect(gs, e->as.call_.args[i]);
        return;
    }
    if (e->kind == EX_CATCH_UNWIND) {
        /* AR: the thunk's value is packed into __v as raw int64 bits and boxed
         * with tur_box_ok(__v) at the catch resume.  An aggregate thunk return
         * (a by-value struct) does not fit that path -- keep it out of scope
         * (AR3) by bailing to native rather than emitting an ill-typed cast. */
        FnDef *tfc = gs_thunk_fn(e);
        if (tfc) {
            TypeKind tk_; const char *tct_; bool taggr_; bool tref_;
            if (gs_param_class(gs->ctx, tfc->return_type, &tk_, &tct_, &taggr_, &tref_) && taggr_) {
                gs->overflow = true; return;
            }
        }
        char nm[32]; snprintf(nm, sizeof nm, "__t%d", gs->ctx->tmp_n++);
        int vi = gs_add_var(gs, tur_strdup(nm), "int64_t", TY_INT);
        if (vi >= 0 && gs->n_susp < TUR_SC_MAXN) {
            gs->susp_expr[gs->n_susp] = e; gs->susp_idx[gs->n_susp] = vi; gs->n_susp++;
        }
        FnDef *tf = gs_thunk_fn(e);
        if (tf && tf->body) gs_collect(gs, tf->body);
        return;
    }
    switch (e->kind) {
        case EX_IF: gs_collect(gs, e->as.if_.cond); gs_collect(gs, e->as.if_.then_);
                    gs_collect(gs, e->as.if_.else_or_null); break;
        case EX_LET:
            for (uint32_t i = 0; i < e->as.let_.n; i++) { gs_collect(gs, e->as.let_.bindings[i].init); }
            gs_collect(gs, e->as.let_.body); break;
        case EX_DO:
            for (uint32_t i = 0; i < e->as.do_.n; i++) { gs_collect(gs, e->as.do_.items[i]); }
            break;
        case EX_BUILTIN:
            for (uint32_t i = 0; i < e->as.builtin.n; i++) { gs_collect(gs, e->as.builtin.args[i]); }
            break;
        case EX_CALL:
            for (uint32_t i = 0; i < e->as.call_.n_args; i++) { gs_collect(gs, e->as.call_.args[i]); }
            break;
        case EX_CAST:       gs_collect(gs, e->as.cast_.expr); break;
        case EX_REINTERPRET:    gs_collect(gs, e->as.reinterpret_.expr); break;
        case EX_ASCRIBE:    gs_collect(gs, e->as.ascribe_.inner); break;
        case EX_PANIC_PAYLOAD_TYPE:  gs_collect(gs, e->as.panic_payload_type_.payload); break;
        case EX_PANIC_PAYLOAD_VALUE: gs_collect(gs, e->as.panic_payload_value_.payload); break;
        case EX_PANIC:      gs_collect(gs, e->as.panic_.payload); break;
        case EX_PANIC_WITH: gs_collect(gs, e->as.panic_with_.payload); break;
        default: break;
    }
}

/* Hole-aware "does e still contain a live suspension?" (a registered hole is a
 * resolved value, no longer a suspension). */
static bool gs_suspends_live(GsCtx *gs, const Expr *e) {
    if (!e || gs_is_hole(gs->ctx, e)) return false;
    if (gs_is_self_call(e, gs->fd)) return true;
    if (e->kind == EX_CATCH_UNWIND) return true;
    switch (e->kind) {
        case EX_IF:
            return gs_suspends_live(gs, e->as.if_.cond) ||
                   gs_suspends_live(gs, e->as.if_.then_) ||
                   gs_suspends_live(gs, e->as.if_.else_or_null);
        case EX_LET: {
            for (uint32_t i = 0; i < e->as.let_.n; i++)
                if (gs_suspends_live(gs, e->as.let_.bindings[i].init)) return true;
            return gs_suspends_live(gs, e->as.let_.body);
        }
        case EX_DO:
            for (uint32_t i = 0; i < e->as.do_.n; i++)
                if (gs_suspends_live(gs, e->as.do_.items[i])) return true;
            return false;
        case EX_BUILTIN:
            for (uint32_t i = 0; i < e->as.builtin.n; i++)
                if (gs_suspends_live(gs, e->as.builtin.args[i])) return true;
            return false;
        case EX_CALL:
            for (uint32_t i = 0; i < e->as.call_.n_args; i++)
                if (gs_suspends_live(gs, e->as.call_.args[i])) return true;
            return false;
        case EX_CAST:    return gs_suspends_live(gs, e->as.cast_.expr);
        case EX_REINTERPRET: return gs_suspends_live(gs, e->as.reinterpret_.expr);
        case EX_ASCRIBE: return gs_suspends_live(gs, e->as.ascribe_.inner);
        case EX_PANIC_PAYLOAD_TYPE:  return gs_suspends_live(gs, e->as.panic_payload_type_.payload);
        case EX_PANIC_PAYLOAD_VALUE: return gs_suspends_live(gs, e->as.panic_payload_value_.payload);
        /* BR3a: mirror gs_suspends' descent into the pure-read forms so an
         * emission-time hole check never treats a suspension buried in a match
         * arm as absent (which would emit the enclosed self-call natively). */
        case EX_MATCH: {
            if (gs_suspends_live(gs, e->as.match_.scrutinee)) return true;
            for (uint32_t i = 0; i < e->as.match_.n_arms; i++) {
                if (gs_suspends_live(gs, e->as.match_.arms[i].body)) return true;
                if (gs_suspends_live(gs, e->as.match_.arms[i].guard)) return true;
            }
            return false;
        }
        case EX_GET_FIELD: return gs_suspends_live(gs, e->as.get_field_.struct_expr);
        case EX_DEREF:     return gs_suspends_live(gs, e->as.deref_.expr);
        default:         return false;
    }
}

/* Leftmost live suspension in eval order (only into cond of an if, never into
 * a branch/body/thunk -- those are handled structurally / as their own seg). */
static const Expr *gs_leftmost(GsCtx *gs, const Expr *e) {
    if (!e || gs_is_hole(gs->ctx, e)) return NULL;
    if (gs_is_self_call(e, gs->fd)) {
        for (uint32_t i = 0; i < e->as.call_.n_args; i++) {
            const Expr *r = gs_leftmost(gs, e->as.call_.args[i]);
            if (r) return r;
        }
        return e;
    }
    if (e->kind == EX_CATCH_UNWIND) return e;
    switch (e->kind) {
        case EX_BUILTIN:
            for (uint32_t i = 0; i < e->as.builtin.n; i++) {
                const Expr *r = gs_leftmost(gs, e->as.builtin.args[i]);
                if (r) return r;
            }
            return NULL;
        case EX_CALL:
            for (uint32_t i = 0; i < e->as.call_.n_args; i++) {
                const Expr *r = gs_leftmost(gs, e->as.call_.args[i]);
                if (r) return r;
            }
            return NULL;
        case EX_CAST:    return gs_leftmost(gs, e->as.cast_.expr);
        case EX_REINTERPRET: return gs_leftmost(gs, e->as.reinterpret_.expr);
        case EX_ASCRIBE: return gs_leftmost(gs, e->as.ascribe_.inner);
        case EX_PANIC_PAYLOAD_TYPE:  return gs_leftmost(gs, e->as.panic_payload_type_.payload);
        case EX_PANIC_PAYLOAD_VALUE: return gs_leftmost(gs, e->as.panic_payload_value_.payload);
        case EX_IF:      return gs_leftmost(gs, e->as.if_.cond);
        default:         return NULL;
    }
}

static void gs_save(GsCtx *gs, Buf *b, const char *node) {
    /* Record which saved[] slots hold a malloc'd aggregate box so a panic that
     * pops this node without running its resume can free them (Part B). */
    uint32_t amask = 0;
    for (int i = 0; i < gs->n_vars; i++) if (gs->vars[i].is_aggr) amask |= ((uint32_t)1 << i);
    indent_buf(b, gs->cur_ind);
    buf_printf(b, "%s->aggr_mask = 0x%xu;\n", node, amask);
    for (int i = 0; i < gs->n_vars; i++) {
        if (gs->vars[i].is_aggr) {
            /* Heap-box a copy of the aggregate; the slot holds the box pointer.
             * Byte-copy matches native (a param kept live by value across the
             * recursive call, no retain/drop).  A by-ref param (is_ref) is
             * ALREADY a pointer to the pointee, so memcpy reads through it
             * (`memcpy(box, cname, ...)`); a by-value param needs its address
             * taken (`memcpy(box, &cname, ...)`). */
            indent_buf(b, gs->cur_ind);
            buf_printf(b, "{ void *__ab = malloc(sizeof(%s)); memcpy(__ab, %s%s, sizeof(%s)); %s->saved[%d] = (int64_t)(intptr_t)__ab; }\n",
                       gs->vars[i].ctype, gs->vars[i].is_ref ? "" : "&",
                       gs->vars[i].cname, gs->vars[i].ctype, node, i);
            continue;
        }
        char *sv = sc_save_expr(gs->vars[i].kind, gs->vars[i].cname);
        indent_buf(b, gs->cur_ind);
        buf_printf(b, "%s->saved[%d] = %s;\n", node, i, sv);
        free(sv);
    }
}
static void gs_restore(GsCtx *gs, Buf *b) {
    for (int i = 0; i < gs->n_vars; i++) {
        if (gs->vars[i].is_aggr) {
            if (gs->vars[i].is_ref) {
                /* Re-home the boxed pointee into this var's stable function-scope
                 * buffer and re-point cname at it, then free the box.  cname is a
                 * `const <ctype> *`; the buffer lives as long as the trampoline,
                 * so the resumed segment reads a valid pointee. */
                indent_buf(b, gs->cur_ind);
                buf_printf(b, "{ void *__ab = (void*)(intptr_t)__k->saved[%d]; memcpy(&%s__agg, __ab, sizeof(%s)); free(__ab); %s = &%s__agg; }\n",
                           i, gs->vars[i].cname, gs->vars[i].ctype,
                           gs->vars[i].cname, gs->vars[i].cname);
                continue;
            }
            indent_buf(b, gs->cur_ind);
            buf_printf(b, "{ void *__ab = (void*)(intptr_t)__k->saved[%d]; memcpy(&%s, __ab, sizeof(%s)); free(__ab); }\n",
                       i, gs->vars[i].cname, gs->vars[i].ctype);
            continue;
        }
        char slot[48]; snprintf(slot, sizeof slot, "__k->saved[%d]", i);
        char *rs = sc_restore_expr(gs->vars[i].ctype, gs->vars[i].kind, slot);
        indent_buf(b, gs->cur_ind);
        buf_printf(b, "%s = %s;\n", gs->vars[i].cname, rs);
        free(rs);
    }
}

static void cps_emit(GsCtx *gs, Buf *b, const Expr *e, const GsSink *sink);
static void cps_emit_let(GsCtx *gs, Buf *b, const LetBinding *binds, uint32_t i,
                         uint32_t n, const Expr *body, const GsSink *sink);
static void cps_emit_do(GsCtx *gs, Buf *b, const Expr **items, uint32_t i,
                        uint32_t n, const GsSink *sink);

/* Route a produced value `val` to its sink (may continue emitting a let/do
 * tail into the same segment, or end the segment with a RETURN + break). */
static void gs_deliver(GsCtx *gs, Buf *b, const char *val, const GsSink *sink) {
    /* catch-unwind-panic-payload-leaks (Leak 3): reclaim a let-bound caught
     * Result box now that its consuming reads have run and `val` (proven not to
     * alias the box) is ready to be delivered.  Emitted before the sink's
     * terminal write/break so it is not stranded as dead code after it. */
    if (sink->free_box) {
        indent_buf(b, gs->cur_ind);
        buf_printf(b, "tur_result_box_free((int64_t)(intptr_t)%s);\n", sink->free_box);
    }
    switch (sink->kind) {
        case GSK_RETURN: {
            if (sink->ret_aggr) {
                /* AR: heap-box the returned aggregate; __v carries the box
                 * pointer (like an aggregate param, but for the return value).
                 * The matching consumer (self-call resume / DONE) unboxes and
                 * frees.  A temp holds `val` so its address is taken safely. */
                indent_buf(b, gs->cur_ind);
                buf_printf(b, "{ %s __rv = (%s); void *__rb = malloc(sizeof(%s)); "
                              "memcpy(__rb, &__rv, sizeof(%s)); __v = (int64_t)(intptr_t)__rb; }\n",
                           sink->ret_ctype, val, sink->ret_ctype, sink->ret_ctype);
                indent_buf(b, gs->cur_ind);
                buf_puts(b, "__pc = __k->tag; break;\n");
                return;
            }
            char *sv = sc_save_expr(sink->ret_kind, val);
            indent_buf(b, gs->cur_ind);
            buf_printf(b, "__v = %s; __pc = __k->tag; break;\n", sv);
            free(sv);
            return;
        }
        case GSK_ASSIGN:
            indent_buf(b, gs->cur_ind);
            buf_printf(b, "%s = %s;\n", sink->dest, val);
            cps_emit_let(gs, b, sink->binds, sink->bi, sink->bn, sink->body, sink->cont);
            return;
        case GSK_SEQ:
            indent_buf(b, gs->cur_ind);
            buf_printf(b, "(void)(%s);\n", val);
            cps_emit_do(gs, b, sink->items, sink->si, sink->sn, sink->cont);
            return;
    }
}

/* Descend into a self-call S.  temp_vi < 0: resume delivers S's value to `sink`
 * (S is the whole expr).  temp_vi >= 0: resume stores S's value into that temp
 * and re-emits `enc` (with S held as a hole) to `sink`. */
static void gs_self_descend(GsCtx *gs, Buf *b, const Expr *S, const GsSink *sink,
                            int temp_vi, const Expr *enc) {
    int m = gs_call_member_idx(S);           /* callee group member */
    int pbase = gs->mem_pbase[m];
    int entry = gs->mem_entry_tag[m];
    int id = gs->ctx->tmp_n++;
    int rtag = gs->next_tag++;
    Buf *rb = gs_add_seg(gs, rtag);
    if (!rb) return;
    uint32_t na = S->as.call_.n_args;
    char node[32]; snprintf(node, sizeof node, "__n%d", id);

    gs->ctx->indent = gs->cur_ind;
    indent_buf(b, gs->cur_ind);
    buf_printf(b, "tur_cont *%s = (tur_cont*)malloc(sizeof(tur_cont));\n", node);
    indent_buf(b, gs->cur_ind);
    buf_printf(b, "%s->tag = %d; %s->boundary = 0; %s->next = __k;\n", node, rtag, node, node);
    gs_save(gs, b, node);
    char **tmps = (char **)malloc(sizeof(char *) * (na ? na : 1));
    for (uint32_t i = 0; i < na; i++) {
        GsVar *pv = &gs->vars[pbase + (int)i];
        gs->ctx->indent = gs->cur_ind;
        char *av = emit_value(gs->ctx, b, S->as.call_.args[i]);
        char nm[48]; snprintf(nm, sizeof nm, "__ra%d_%u", id, i);
        indent_buf(b, gs->cur_ind);
        /* S1 (jit-engine-plan section 4): name the temp's type instead of
         * __auto_type (GNU-only; c2mir cannot parse it).  The type is not
         * guessed: gs_param_class gates entry to this whole lowering and never
         * succeeds without a ctype, and the temp's only consumer is the
         * assignment into that same param below, so declaring it as the param's
         * type moves the identical conversion one line earlier.  An is_ref
         * param's arg is a borrow pointer, so the temp is `const <ctype> *`. */
        if (pv->is_ref)
            buf_printf(b, "const %s *%s = (%s);\n", pv->ctype, nm, av);
        else
            buf_printf(b, "%s %s = (%s);\n", pv->ctype, nm, av);
        free(av); tmps[i] = tur_strdup(nm);
    }
    for (uint32_t i = 0; i < na; i++) {
        GsVar *pv = &gs->vars[pbase + (int)i];
        indent_buf(b, gs->cur_ind);
        if (pv->is_ref) {
            /* The callee param is a `const <ctype> *` borrow; the arg (`tmps[i]`)
             * is itself such a pointer whose pointee lives in this segment's
             * scope, which the descend collapses.  Materialize the pointee into
             * the param's stable function-scope buffer and point the param var at
             * it, so the child entry reads a live value (memmove tolerates the
             * arg == this same param aliasing case). */
            buf_printf(b, "memmove(&%s__agg, %s, sizeof(%s)); %s = &%s__agg;\n",
                       pv->cname, tmps[i], pv->ctype, pv->cname, pv->cname);
        } else {
            buf_printf(b, "%s = %s;\n", pv->cname, tmps[i]);
        }
        free(tmps[i]);
    }
    free(tmps);
    indent_buf(b, gs->cur_ind);
    buf_printf(b, "__k = %s; __pc = %d; break;\n", node, entry);

    int saved_ind = gs->cur_ind;
    gs->cur_ind = gs->base_ind + 3;
    gs_restore(gs, rb);
    char *rv;
    if (gs->mem_ret_aggr[m]) {
        /* AR: the callee boxed its aggregate return into __v; unbox it into a
         * fresh by-value local and free the box.  rv is then that local. */
        int uid = gs->ctx->tmp_n++;
        char urnm[32]; snprintf(urnm, sizeof urnm, "__ur%d", uid);
        indent_buf(rb, gs->cur_ind);
        buf_printf(rb, "%s %s; { void *__rb = (void*)(intptr_t)__v; "
                       "memcpy(&%s, __rb, sizeof(%s)); free(__rb); }\n",
                   gs->mem_retctype[m], urnm, urnm, gs->mem_retctype[m]);
        rv = tur_strdup(urnm);
    } else {
        rv = sc_restore_expr(gs->mem_retctype[m], gs->mem_ret[m], "__v");
    }
    if (temp_vi >= 0) {
        indent_buf(rb, gs->cur_ind);
        buf_printf(rb, "%s = %s;\n", gs->vars[temp_vi].cname, rv);
    }
    indent_buf(rb, gs->cur_ind);
    buf_puts(rb, "{ tur_cont *__pk = __k->next; free(__k); __k = __pk; }\n");
    if (temp_vi >= 0) {
        gs->ctx->sub_holes[gs->ctx->n_sub_holes] = S;
        gs->ctx->sub_names[gs->ctx->n_sub_holes] = gs->vars[temp_vi].cname;
        gs->ctx->n_sub_holes++;
        cps_emit(gs, rb, enc, sink);
        gs->ctx->n_sub_holes--;
    } else {
        gs_deliver(gs, rb, rv, sink);
    }
    free(rv);
    gs->cur_ind = saved_ind;
}

/* catch-unwind-return-bridge-residuals (Part C): the by-value aggregate return
 * type behind a GSK_RETURN `ret_aggr` sink, or TY_UNKNOWN when it cannot be
 * recovered.  A caught box delivered to such a sink is the int64 carrier, not
 * the aggregate the sink heap-boxes, so it must be bridged carrier->concrete
 * first (via this type).  The sink's ret_ctype string aliases the member's
 * mem_retctype entry, so match on it to recover the member's declared return
 * Type. */
static Type gs_sink_return_aggr_type(GsCtx *gs, const GsSink *sink) {
    Type unknown = type_simple(TY_UNKNOWN, CK_COPY);
    if (!sink || sink->kind != GSK_RETURN || !sink->ret_aggr) return unknown;
    for (int m = 0; m < gs->n_mem; m++) {
        if (gs->mem_retctype[m] == sink->ret_ctype ||
            (gs->mem_retctype[m] && sink->ret_ctype &&
             strcmp(gs->mem_retctype[m], sink->ret_ctype) == 0)) {
            if (gs->mem[m])
                return emit_resolve_type(gs->ctx, gs->mem[m]->return_type);
        }
    }
    return unknown;
}

/* Descend into a catch-unwind S: push a boundary, run the thunk body as its own
 * segment, resume by popping the boundary and building the ok/err result box. */
static void gs_catch_descend(GsCtx *gs, Buf *b, const Expr *S, const GsSink *sink,
                             int temp_vi, const Expr *enc) {
    int id = gs->ctx->tmp_n++;
    FnDef *tf = gs_thunk_fn(S);
    /* The thunk value is packed into __v as raw int64 bits and boxed with
     * tur_box_ok(__v).  A scalar thunk return uses its own kind (float via bit
     * reinterpret); a non-scalar (int64-carrier) thunk return uses TY_INT (an
     * intptr cast) -- NOT the enclosing member's ret kind, which may now be an
     * aggregate (AR) and would mis-pack the carrier.  Aggregate thunk returns are
     * already gated out in gs_collect. */
    TypeKind tk = (tf && tf->body && sc_scalar_kind(tf->body->type.kind))
                  ? tf->body->type.kind : TY_INT;
    int thunktag = gs->next_tag++;
    int rtag = gs->next_tag++;
    Buf *tb = gs_add_seg(gs, thunktag);
    Buf *rb = gs_add_seg(gs, rtag);
    if (!tb || !rb) return;
    char node[32]; snprintf(node, sizeof node, "__n%d", id);

    gs->ctx->indent = gs->cur_ind;
    indent_buf(b, gs->cur_ind);
    buf_printf(b, "tur_handler_node *__b%d = (tur_handler_node*)malloc(sizeof(tur_handler_node));\n", id);
    indent_buf(b, gs->cur_ind);
    buf_printf(b, "__b%d->parent = tur_handler_chain; tur_handler_chain = __b%d;\n", id, id);
    indent_buf(b, gs->cur_ind);
    buf_printf(b, "tur_cont *%s = (tur_cont*)malloc(sizeof(tur_cont));\n", node);
    indent_buf(b, gs->cur_ind);
    buf_printf(b, "%s->tag = %d; %s->boundary = __b%d; %s->next = __k;\n", node, rtag, node, id, node);
    gs_save(gs, b, node);
    indent_buf(b, gs->cur_ind);
    buf_printf(b, "__k = %s; __pc = %d; break;\n", node, thunktag);

    int saved_ind = gs->cur_ind;
    /* thunk body segment: its value returns to the catch node (this __k). */
    gs->cur_ind = gs->base_ind + 3;
    GsSink tsink; memset(&tsink, 0, sizeof tsink);
    tsink.kind = GSK_RETURN; tsink.ret_kind = tk;
    tsink.ret_ctype = "int64_t"; tsink.ret_aggr = false;
    cps_emit(gs, tb, tf ? tf->body : NULL, &tsink);

    /* resume segment: pop boundary, consume any caught panic, build the box. */
    gs->cur_ind = gs->base_ind + 3;
    gs_restore(gs, rb);
    indent_buf(rb, gs->cur_ind);
    buf_puts(rb, "tur_handler_chain = __k->boundary->parent; free(__k->boundary);\n");
    indent_buf(rb, gs->cur_ind);
    buf_printf(rb, "int64_t __box%d;\n", id);
    /* Consume a caught panic exactly as native tur_catch_unwind_box does: box
     * the payload pointer into the err result (do NOT free it -- err-val hands
     * it back), so the err branch and err-val/ok-val extraction match native.
     * The result box lives on, matching native's documented box leak. */
    indent_buf(rb, gs->cur_ind);
    buf_printf(rb, "if (tur_panicking) { tur_panicking = 0; tur_panic_in_progress = 0; "
                   "tur_panic_payload *__pp%d = global_panic_payload; global_panic_payload = 0; "
                   "__box%d = tur_box_err((int64_t)(intptr_t)__pp%d); }\n", id, id, id);
    indent_buf(rb, gs->cur_ind);
    buf_printf(rb, "else { __box%d = tur_box_ok(__v); }\n", id);
    indent_buf(rb, gs->cur_ind);
    buf_puts(rb, "{ tur_cont *__pk = __k->next; free(__k); __k = __pk; }\n");
    char boxnm[32]; snprintf(boxnm, sizeof boxnm, "__box%d", id);
    if (temp_vi >= 0) {
        indent_buf(rb, gs->cur_ind);
        buf_printf(rb, "%s = %s;\n", gs->vars[temp_vi].cname, boxnm);
        gs->ctx->sub_holes[gs->ctx->n_sub_holes] = S;
        gs->ctx->sub_names[gs->ctx->n_sub_holes] = gs->vars[temp_vi].cname;
        gs->ctx->n_sub_holes++;
        cps_emit(gs, rb, enc, sink);
        gs->ctx->n_sub_holes--;
    } else {
        if (sink->kind == GSK_SEQ) {
            /* Discarded catch (statement position): the box is (void)-cast and
             * provably does not escape, so free it here -- the stackless
             * counterpart to the native statement-position free in emit_stmt.c
             * (catch-unwind-result-box-leak). Reuse tur_result_box_free so the
             * err-branch payload-vs-value distinction stays in one place. */
            indent_buf(rb, gs->cur_ind);
            buf_printf(rb, "tur_result_box_free((int64_t)(intptr_t)%s);\n", boxnm);
        }
        /* catch-unwind-return-bridge-residuals (Part C): a caught box delivered
         * as a function's by-value (Result/Option ...) return is the int64
         * carrier, but the ret_aggr sink heap-boxes the value as if it were the
         * aggregate -- `Result__int__int __rv = (__box)` is an invalid int64->
         * struct initializer.  Bridge the carrier box to the concrete aggregate
         * first (the same canonical field-by-field readback the native return
         * bridge uses), so the sink boxes a real aggregate value. */
        Type ret_aggr_ty = gs_sink_return_aggr_type(gs, sink);
        if (ret_aggr_ty.kind != TY_UNKNOWN) {
            gs->ctx->indent = gs->cur_ind;
            char *bridged = emit_carrier_bridge(gs->ctx, rb, strdup(boxnm),
                                                CK_CARRIER, CK_CONCRETE,
                                                ret_aggr_ty);
            /* Part B (stackless mirror): the resume-built box is fresh and
             * delivered straight to the return, so it is sole-owned.  Materialize
             * the aggregate into a temp, free the now-dead box struct (never the
             * payload -- the returned err aggregate may alias it), then deliver
             * the aggregate the sink heap-boxes. */
            const char *agg_cty = emit_type_c_name(gs->ctx, ret_aggr_ty);
            char aggnm[40]; snprintf(aggnm, sizeof aggnm, "__caggr%d", id);
            indent_buf(rb, gs->cur_ind);
            buf_printf(rb, "%s %s = %s;\n", agg_cty, aggnm, bridged);
            indent_buf(rb, gs->cur_ind);
            /* catch-unwind-returned-err-box-payload-leak: full free reclaims the
             * 32 B payload when the err arm is a scalar (never aliases the
             * payload); shallow otherwise. */
            buf_printf(rb, "%s((int64_t)(intptr_t)%s);\n",
                       result_err_arm_is_freeable_scalar(&ret_aggr_ty)
                           ? "tur_result_box_free"
                           : "tur_result_box_free_shallow",
                       boxnm);
            gs_deliver(gs, rb, aggnm, sink);
            free(bridged);
        } else {
            /* catch-unwind-panic-payload-leaks (Leak 3): a let-bound caught box
             * (`(let [r (catch-unwind ...)] BODY)`) whose BODY is straight-line
             * (no further suspension / catch / panic) and reads `r` only through
             * non-escaping scalar accessors is reclaimable at scope exit, just
             * like the native let_binding_box_freeable path.  Arm the terminal
             * sink of BODY to free the box once its consuming reads have run (the
             * delivered value is proven not to alias the box), so the loop leak
             * (~4.8 MB over 200k iters) is gone.  A body that suspends would save
             * `r` across a descend, so the straight-line gate is required for the
             * one-shot free to be sound. */
            GsSink cont_copy, assign_copy;
            const GsSink *dsink = sink;
            if (sink->kind == GSK_ASSIGN && sink->bi >= 1 &&
                sink->bi == sink->bn && sink->cont && sink->body) {
                const Binding *boxbind = sink->binds[sink->bi - 1].binding;
                /* residual-leaks (2026-09-02): the straight-line gate is no
                 * longer needed when the box has a machine VARIABLE to be
                 * freed through.  The gate existed because the free named
                 * `__box<id>`, a local of THIS resume segment, which a later
                 * segment (after a self-call descend in BODY) cannot see; but
                 * gs_save / gs_restore carry every machine var across a
                 * descend, so naming `sink->dest` -- the let-bound var the box
                 * is assigned to -- is valid in whichever segment BODY's
                 * terminal delivery lands in, and that delivery runs exactly
                 * once per activation (branches are exclusive).  A nested
                 * catch and a panic in BODY stay excluded: a panic unwinds
                 * past the delivery (a leak, not a UAF), a nested catch's own
                 * resume must not be confused with this one.  `(let [r
                 * (catch-unwind ...)] (if (err? r) (+ 1 (h (- n 1))) 999))` --
                 * the stackless panic stress fixtures, 53 B x 200k iterations
                 * -- is the shape this admits. */
                bool through_var = sink->dest != NULL;
                if (boxbind &&
                    (through_var || !gs_suspends_live(gs, sink->body)) &&
                    !gs_has_catch(sink->body, gs->fd) &&
                    !gs_has_panic(sink->body) &&
                    !catch_box_binding_escapes(sink->body, boxbind)) {
                    cont_copy = *sink->cont;
                    cont_copy.free_box = through_var ? sink->dest : boxnm;
                    assign_copy = *sink;
                    assign_copy.cont = &cont_copy;
                    dsink = &assign_copy;
                }
            }
            gs_deliver(gs, rb, boxnm, dsink);
        }
    }
    gs->cur_ind = saved_ind;
}

/* A value-position expression that contains a live suspension. */
/* BR3b: does `e` contain an admitted reader call not already held as a hole? */
static bool gs_has_br3b_live(GsCtx *gs, const Expr *e) {
    if (!e || gs_is_hole(gs->ctx, e)) return false;
    if (gs_is_br3b_reader_call(e, gs->fd)) return true;
    switch (e->kind) {
        case EX_IF:
            return gs_has_br3b_live(gs, e->as.if_.cond) ||
                   gs_has_br3b_live(gs, e->as.if_.then_) ||
                   gs_has_br3b_live(gs, e->as.if_.else_or_null);
        case EX_LET:
            for (uint32_t i = 0; i < e->as.let_.n; i++)
                if (gs_has_br3b_live(gs, e->as.let_.bindings[i].init)) return true;
            return gs_has_br3b_live(gs, e->as.let_.body);
        case EX_DO:
            for (uint32_t i = 0; i < e->as.do_.n; i++)
                if (gs_has_br3b_live(gs, e->as.do_.items[i])) return true;
            return false;
        case EX_BUILTIN:
            for (uint32_t i = 0; i < e->as.builtin.n; i++)
                if (gs_has_br3b_live(gs, e->as.builtin.args[i])) return true;
            return false;
        case EX_CALL:
            for (uint32_t i = 0; i < e->as.call_.n_args; i++)
                if (gs_has_br3b_live(gs, e->as.call_.args[i])) return true;
            return false;
        case EX_CAST:    return gs_has_br3b_live(gs, e->as.cast_.expr);
        case EX_REINTERPRET: return gs_has_br3b_live(gs, e->as.reinterpret_.expr);
        case EX_ASCRIBE: return gs_has_br3b_live(gs, e->as.ascribe_.inner);
        default:         return false;
    }
}

/* BR3b: pre-emit every UNCONDITIONALLY-evaluated reader call in `e` (eval order,
 * innermost first) into a segment-local temp, each followed by `if
 * (tur_panicking) break;` so a reader panic routes to the driver's unwind loop
 * before its garbage result is used -- the segment analogue of native's
 * post-call `if (tur_panicking) return;`.  Each call is then registered as an
 * emit_value hole so the residual expression reads the temp.  Only walks
 * unconditional contexts (builtin/call args, cast); gs_value_ok kept BR3b calls
 * out of value-position branches, so there are none to (wrongly) hoist here. */
static void gs_preemit_br3b(GsCtx *gs, Buf *b, const Expr *e) {
    if (!e || gs_is_hole(gs->ctx, e)) return;
    switch (e->kind) {
        case EX_BUILTIN:
            for (uint32_t i = 0; i < e->as.builtin.n; i++)
                gs_preemit_br3b(gs, b, e->as.builtin.args[i]);
            return;
        case EX_CAST:    gs_preemit_br3b(gs, b, e->as.cast_.expr); return;
        case EX_REINTERPRET: gs_preemit_br3b(gs, b, e->as.reinterpret_.expr); return;
        case EX_ASCRIBE: gs_preemit_br3b(gs, b, e->as.ascribe_.inner); return;
        case EX_CALL: {
            for (uint32_t i = 0; i < e->as.call_.n_args; i++)
                gs_preemit_br3b(gs, b, e->as.call_.args[i]);
            if (!gs_is_br3b_reader_call(e, gs->fd)) return;
            if (gs->ctx->n_sub_holes >= 16) { gs->overflow = true; return; }
            /* emit_value A-normalizes the call into an `__ps_N` temp; with the
             * break flag set it follows it with `if (tur_panicking) break;`
             * (routing a reader panic to the driver unwind, not a driver-escaping
             * return).  The returned temp name becomes this call's hole. */
            gs->ctx->indent = gs->cur_ind;
            bool saved = gs->ctx->panic_signal_is_break;
            gs->ctx->panic_signal_is_break = true;
            char *cv = emit_value(gs->ctx, b, e);
            gs->ctx->panic_signal_is_break = saved;
            gs->ctx->sub_holes[gs->ctx->n_sub_holes] = e;
            gs->ctx->sub_names[gs->ctx->n_sub_holes] = cv;   /* owned; freed by cps_emit_br3b */
            gs->ctx->n_sub_holes++;
            return;
        }
        default: return;
    }
}

/* BR3b: emit a value expression that contains a reader call but no suspension --
 * hoist the reader calls (with panic checks) then emit the residual via the fast
 * path, the calls standing in as their temps. */
static void cps_emit_br3b(GsCtx *gs, Buf *b, const Expr *e, const GsSink *sink) {
    uint8_t base = gs->ctx->n_sub_holes;
    gs_preemit_br3b(gs, b, e);
    gs->ctx->indent = gs->cur_ind;
    char *v = emit_value(gs->ctx, b, e);
    gs_deliver(gs, b, v, sink); free(v);
    for (uint8_t i = base; i < gs->ctx->n_sub_holes; i++)
        free((void *)gs->ctx->sub_names[i]);
    gs->ctx->n_sub_holes = base;
}

static void cps_emit_value_susp(GsCtx *gs, Buf *b, const Expr *e, const GsSink *sink) {
    const Expr *S = gs_leftmost(gs, e);
    if (!S) {
        /* No suspension.  A panic buried in a value position (e.g. an operand of
         * a pure builtin) must still route through the driver, not emit_value's
         * bare return -- emit the panic; it diverges, so the rest is dead. */
        const Expr *P = gs_leftmost_panic(e);
        if (P) { cps_emit(gs, b, P, sink); return; }
        /* BR3b: a fallible reader call (no suspension) is hoisted with a panic
         * check before the residual expression is emitted. */
        if (gs_has_br3b_live(gs, e)) { cps_emit_br3b(gs, b, e, sink); return; }
        gs->ctx->indent = gs->cur_ind;
        char *v = emit_value(gs->ctx, b, e);
        gs_deliver(gs, b, v, sink); free(v); return;
    }
    int scall = gs_call_member_idx(S);
    bool is_self = scall >= 0;
    if (S == e) {
        if (is_self && sink->kind == GSK_RETURN) {
            /* tail call (self or cross-member): reassign the callee's params and
             * loop to its ENTRY, no node.  Well-typed tail position guarantees
             * the callee's return type matches this member's, so __v flows on. */
            int pbase = gs->mem_pbase[scall];
            int entry = gs->mem_entry_tag[scall];
            int id = gs->ctx->tmp_n++;
            uint32_t na = S->as.call_.n_args;
            char **tmps = (char **)malloc(sizeof(char *) * (na ? na : 1));
            for (uint32_t i = 0; i < na; i++) {
                GsVar *pv = &gs->vars[pbase + (int)i];
                gs->ctx->indent = gs->cur_ind;
                char *av = emit_value(gs->ctx, b, S->as.call_.args[i]);
                char nm[48]; snprintf(nm, sizeof nm, "__ra%d_%u", id, i);
                indent_buf(b, gs->cur_ind);
                /* S1: same named-type reasoning as gs_self_descend above. */
                if (pv->is_ref)
                    buf_printf(b, "const %s *%s = (%s);\n", pv->ctype, nm, av);
                else
                    buf_printf(b, "%s %s = (%s);\n", pv->ctype, nm, av);
                free(av); tmps[i] = tur_strdup(nm);
            }
            for (uint32_t i = 0; i < na; i++) {
                GsVar *pv = &gs->vars[pbase + (int)i];
                indent_buf(b, gs->cur_ind);
                if (pv->is_ref) {
                    /* A by-const-ptr aggregate param: the arg pointer's pointee
                     * lives in this segment's scope, gone once we loop to ENTRY.
                     * Re-home it into the param's stable buffer (memmove tolerates
                     * the arg == this same param aliasing case). */
                    buf_printf(b, "memmove(&%s__agg, %s, sizeof(%s)); %s = &%s__agg;\n",
                               pv->cname, tmps[i], pv->ctype, pv->cname, pv->cname);
                } else {
                    buf_printf(b, "%s = %s;\n", pv->cname, tmps[i]);
                }
                free(tmps[i]);
            }
            free(tmps);
            indent_buf(b, gs->cur_ind);
            buf_printf(b, "__pc = %d; break;\n", entry);
            return;
        }
        if (is_self) gs_self_descend(gs, b, S, sink, -1, NULL);
        else         gs_catch_descend(gs, b, S, sink, -1, NULL);
        return;
    }
    int vi = gs_temp_index(gs, S);
    if (vi < 0) { gs->overflow = true; return; }
    if (is_self) gs_self_descend(gs, b, S, sink, vi, e);
    else         gs_catch_descend(gs, b, S, sink, vi, e);
}

static void cps_emit_let(GsCtx *gs, Buf *b, const LetBinding *binds, uint32_t i,
                         uint32_t n, const Expr *body, const GsSink *sink) {
    if (i >= n) { cps_emit(gs, b, body, sink); return; }
    char *dn = atom_var(gs->ctx, binds[i].binding);
    GsSink as; memset(&as, 0, sizeof as);
    as.kind = GSK_ASSIGN; as.dest = dn;
    as.binds = binds; as.bi = i + 1; as.bn = n; as.body = body; as.cont = sink;
    cps_emit(gs, b, binds[i].init, &as);
    free(dn);
}

static void cps_emit_do(GsCtx *gs, Buf *b, const Expr **items, uint32_t i,
                        uint32_t n, const GsSink *sink) {
    if (n == 0) { gs_deliver(gs, b, "INT64_C(0)", sink); return; }
    if (i >= n - 1) { cps_emit(gs, b, items[i], sink); return; }
    GsSink ss; memset(&ss, 0, sizeof ss);
    ss.kind = GSK_SEQ; ss.items = items; ss.si = i + 1; ss.sn = n; ss.cont = sink;
    cps_emit(gs, b, items[i], &ss);
}

static void cps_emit(GsCtx *gs, Buf *b, const Expr *e, const GsSink *sink) {
    if (gs->overflow) return;
    if (!e) { gs_deliver(gs, b, "INT64_C(0)", sink); return; }
    /* Panic must NOT flow through emit_value (which would inject a bare
     * `return` and escape the driver): set the signal and let the loop-top
     * unwind pop to the nearest boundary. */
    if (e->kind == EX_PANIC) {
        const Expr *pl = e->as.panic_.payload;
        gs->ctx->indent = gs->cur_ind;
        char *m = emit_value(gs->ctx, b, pl);
        indent_buf(b, gs->cur_ind);
        emit_panic_call(b, e->span, pl->type.kind == TY_CSTR ? m : "\"(non-string panic)\"");
        free(m);
        indent_buf(b, gs->cur_ind); buf_puts(b, "break;\n");
        return;
    }
    if (e->kind == EX_PANIC_WITH) {
        const Expr *pl = e->as.panic_with_.payload;
        gs->ctx->indent = gs->cur_ind;
        char *pv = emit_value(gs->ctx, b, pl);
        indent_buf(b, gs->cur_ind);
        buf_printf(b, "tur_panic_with(%d, (void*)%s, __FILE__, __LINE__);\n",
                   (int)pl->type.kind, pv);
        free(pv);
        indent_buf(b, gs->cur_ind); buf_puts(b, "break;\n");
        return;
    }
    if (!gs_suspends_live(gs, e) && !gs_has_panic(e)) {
        /* BR3b: a fallible reader call must be hoisted with a tur_panicking
         * check -- it cannot ride the atomic emit_value fast path (which has no
         * place for a statement-level check).  Route it through the value-susp
         * path, which pre-emits the calls then emits the residual. */
        if (gs_has_br3b_live(gs, e)) { cps_emit_value_susp(gs, b, e, sink); return; }
        gs->ctx->indent = gs->cur_ind;
        char *v = emit_value(gs->ctx, b, e);
        gs_deliver(gs, b, v, sink); free(v); return;
    }
    switch (e->kind) {
        case EX_IF: {
            if (gs_suspends_live(gs, e->as.if_.cond)) {
                cps_emit_value_susp(gs, b, e, sink); return;
            }
            gs->ctx->indent = gs->cur_ind;
            char *cv = emit_value(gs->ctx, b, e->as.if_.cond);
            indent_buf(b, gs->cur_ind);
            buf_printf(b, "if (%s) {\n", cv); free(cv);
            int si = gs->cur_ind;
            gs->cur_ind = si + 1;
            cps_emit(gs, b, e->as.if_.then_, sink);
            gs->cur_ind = si; indent_buf(b, gs->cur_ind); buf_puts(b, "} else {\n");
            gs->cur_ind = si + 1;
            cps_emit(gs, b, e->as.if_.else_or_null, sink);
            gs->cur_ind = si; indent_buf(b, gs->cur_ind); buf_puts(b, "}\n");
            return;
        }
        case EX_LET:
            cps_emit_let(gs, b, e->as.let_.bindings, 0, e->as.let_.n, e->as.let_.body, sink);
            return;
        case EX_DO:
            cps_emit_do(gs, b, (const Expr **)e->as.do_.items, 0, e->as.do_.n, sink);
            return;
        default:
            cps_emit_value_susp(gs, b, e, sink);
            return;
    }
}

/* Basic (member-set-independent) constraints a trampolined function must meet. */
static bool gs_basic_ok(EmitCtx *ctx, FnDef *fd) {
    if (!fd || fd->n_params < 1 || fd->n_params > TUR_SC_MAXP) return false;
    if (fd->n_dict_clone > 0 || fd->n_dict_env > 0) return false;
    if (!fd->param_types || !fd->body) return false;
    /* Must be the canonical top-level defn of its binding, and not a lifted
     * closure / catch-unwind thunk (fd->closure set; its synthetic env param
     * would otherwise look scalar) -- those are never trampoline members. */
    if (!fd->binding || fd->binding->source_fn_def != fd) return false;
    if (fd->closure) return false;
    if (fd->binding && fd->binding->name && fd->binding->name->name &&
        strcmp(fd->binding->name->name, "main") == 0) return false;
    /* G6/AR: the return type must be an int64-slot type (scalar / int64 carrier)
     * OR a by-value aggregate (AR -- catch-unwind-aggregate-returns: heap-boxed
     * into __v on produce, unboxed on consume).  Params may likewise be a
     * by-value aggregate (heap-boxed across a descend). */
    TypeKind k; const char *ct; bool aggr; bool ref;
    if (!gs_param_class(ctx, fd->return_type, &k, &ct, &aggr, &ref)) return false;
    for (uint32_t i = 0; i < fd->n_params; i++)
        if (!gs_param_class(ctx, fd->param_types[i], &k, &ct, &aggr, &ref)) return false;
    return true;
}

static bool stackless_general_eligible(EmitCtx *ctx, const Expr *e, FnDef *fd,
                                       TypeKind result_kind) {
    if (ctx->current_abi_specialization) return false;
    if (!gs_basic_ok(ctx, fd)) return false;
    (void)result_kind;
    if (!ctx->current_fn_ret_ctype) return false;
    /* Single-function (G3) view: only self-calls are trampolined. */
    g_gs_mem[0] = fd; g_gs_nmem = 1; g_gs_ctx = ctx;
    const Expr *b = fd->body;
    bool ok = gs_has_catch(b, fd) && gs_stmt_ok(b, fd);
    g_gs_nmem = 0;
    (void)e;
    return ok;
}

/* Populate the per-member var table (all members' params, recording pbase),
 * collect hoisted let-vars + suspension temps, and emit each member's ENTRY
 * segment.  Assumes gs->mem[..] / mem_ret / mem_retctype / mem_entry_tag are
 * set and g_gs_mem mirrors them.  Returns false on overflow. */
static bool gs_build(GsCtx *gs) {
    EmitCtx *ctx = gs->ctx;
    for (int m = 0; m < gs->n_mem; m++) {
        FnDef *f = gs->mem[m];
        gs->mem_pbase[m] = gs->n_vars;
        for (uint32_t i = 0; i < f->n_params; i++) {
            /* G6: an int64-carrier / opaque param rides the slot with kind
             * TY_INT (intptr path); a by-value aggregate rides via heap-boxing. */
            TypeKind k; const char *ct; bool aggr; bool ref;
            if (!gs_param_class(ctx, f->param_types[i], &k, &ct, &aggr, &ref)) { gs->overflow = true; return false; }
            gs_add_var_a(gs, atom_var(ctx, f->params[i]), ct, k, aggr, ref);
        }
    }
    gs->n_param_vars = gs->n_vars;
    for (int m = 0; m < gs->n_mem && !gs->overflow; m++)
        gs_collect(gs, gs->mem[m]->body);
    if (gs->overflow) return false;

    for (int m = 0; m < gs->n_mem; m++) {
        Buf *entry = gs_add_seg(gs, gs->mem_entry_tag[m]);
        if (!entry) return false;
        gs->cur_ind = gs->base_ind + 3;
        gs->ret_kind = gs->mem_ret[m]; gs->ret_ctype = gs->mem_retctype[m];
        GsSink rs; memset(&rs, 0, sizeof rs);
        rs.kind = GSK_RETURN; rs.ret_kind = gs->mem_ret[m];
        rs.ret_ctype = gs->mem_retctype[m]; rs.ret_aggr = gs->mem_ret_aggr[m];
        cps_emit(gs, entry, gs->mem[m]->body, &rs);
        if (gs->overflow) return false;
    }
    return true;
}

/* Emit the driver's `for(;;)` loop + `switch(__pc)` (the DONE case + one case
 * per built segment) into `out` at indent `bi`.  The caller emits the __k /
 * __pc / __v prologue.  `done_typed` chooses whether the DONE case returns the
 * value cast to the (single) member's C return type (inline single-function
 * body) or raw int64 bits (group driver; the shim casts). */
static void gs_emit_driver(GsCtx *gs, Buf *out, int bi, bool done_typed) {
    indent_buf(out, bi); buf_puts(out, "for (;;) {\n");
    indent_buf(out, bi + 1); buf_puts(out, "if (tur_panicking) {\n");
    /* Part B: a self-call resume node (boundary == 0) is popped here without
     * running its resume, so free any aggregate-param boxes it owns (aggr_mask)
     * before the node itself -- otherwise they leak on the panic path. */
    indent_buf(out, bi + 2); buf_puts(out, "while (__k->boundary == 0 && __k->tag != 0) { for (int __i = 0; __i < TUR_SC_MAXN; __i++) if (__k->aggr_mask & ((uint32_t)1 << __i)) free((void*)(intptr_t)__k->saved[__i]); tur_cont *__pk = __k->next; free(__k); __k = __pk; }\n");
    /* G7: the panic escaped every boundary this trampoline owns (all popped in
     * their resume segments, so tur_handler_chain now reflects the OUTER chain
     * at entry).  Mirror native tur_panic's precedence: an outer handler ->
     * propagate the signal by returning (the native caller's tur_panicking check
     * or the outer catch consumes it); else a fiber panic jmpbuf -> longjmp into
     * it; else it is genuinely uncaught -> abort. */
    {
        /* AR: an aggregate return propagates a zero-initialized struct on the
         * escaping-panic path (the value is ignored by the caller but must
         * type-check as the C return type). */
        char *rz;
        if (done_typed && gs->mem_ret_aggr[0]) {
            /* S1/findings 16.4: mem_retctype can be a pointer spelling, and a
             * pointer zero is scalar; emit_c_zero_of picks the legal form. */
            rz = emit_c_zero_of(gs->mem_retctype[0]);
        } else if (done_typed) {
            rz = sc_restore_expr(gs->mem_retctype[0], gs->mem_ret[0], "INT64_C(0)");
        } else {
            rz = tur_strdup("INT64_C(0)");
        }
        indent_buf(out, bi + 2); buf_puts(out, "if (__k->tag == 0) {\n");
        indent_buf(out, bi + 3); buf_puts(out, "free(__k);\n");
        indent_buf(out, bi + 3); buf_printf(out, "if (tur_handler_chain) return %s;\n", rz);
        indent_buf(out, bi + 3); buf_puts(out, "if (tur_current_fiber && tur_current_fiber->panic_jmpbuf_valid) TUR_LONGJMP(tur_current_fiber->panic_jmpbuf);\n");
        indent_buf(out, bi + 3); buf_puts(out, "fprintf(stderr, \"panic (uncaught, stackless)\\n\"); fflush(NULL); abort();\n");
        indent_buf(out, bi + 2); buf_puts(out, "}\n");
        free(rz);
    }
    indent_buf(out, bi + 2); buf_puts(out, "__pc = __k->tag;\n");
    indent_buf(out, bi + 1); buf_puts(out, "}\n");
    indent_buf(out, bi + 1); buf_puts(out, "switch (__pc) {\n");
    if (done_typed && gs->mem_ret_aggr[0]) {
        /* AR: DONE unboxes the aggregate return from __v, frees the box, then
         * returns the by-value struct. */
        indent_buf(out, bi + 2);
        buf_printf(out, "case 0: { void *__rb = (void*)(intptr_t)__v; %s __rr; "
                        "memcpy(&__rr, __rb, sizeof(%s)); free(__rb); free(__k); return __rr; }\n",
                   gs->mem_retctype[0], gs->mem_retctype[0]);
    } else if (done_typed) {
        char *rv = sc_restore_expr(gs->mem_retctype[0], gs->mem_ret[0], "__v");
        indent_buf(out, bi + 2); buf_printf(out, "case 0: { free(__k); return %s; }\n", rv); free(rv);
    } else {
        indent_buf(out, bi + 2); buf_puts(out, "case 0: { free(__k); return __v; }\n");
    }
    for (int s = 0; s < gs->n_segs; s++) {
        indent_buf(out, bi + 2); buf_printf(out, "case %d: {\n", gs->tags[s]);
        buf_write(out, gs->bufs[s]->data, gs->bufs[s]->len);
        indent_buf(out, bi + 2); buf_puts(out, "}\n");
    }
    indent_buf(out, bi + 1); buf_puts(out, "}\n");
    indent_buf(out, bi); buf_puts(out, "}\n");
}

static void gs_free(GsCtx *gs) {
    for (int i = 0; i < gs->n_vars; i++) free(gs->vars[i].cname);
    for (int s = 0; s < gs->n_segs; s++) { buf_free(gs->bufs[s]); free(gs->bufs[s]); }
}

/* Emit the general single-function trampoline as the function body (no shim).
 * Returns false (leaving `file` untouched) if a hard limit is hit. */
static bool emit_stackless_general_body(EmitCtx *ctx, Buf *file, FnDef *fd,
                                        TypeKind result_kind) {
    GsCtx gs; memset(&gs, 0, sizeof gs);
    gs.ctx = ctx; gs.fd = fd; gs.base_ind = ctx->indent; gs.next_tag = 2;
    gs.n_mem = 1; gs.mem[0] = fd; gs.mem_entry_tag[0] = 1;
    gs.mem_ret[0] = result_kind; gs.mem_retctype[0] = ctx->current_fn_ret_ctype;
    {   /* AR: is this function's return a by-value aggregate? */
        TypeKind rk; const char *rct; bool raggr; bool rref;
        gs.mem_ret_aggr[0] = gs_param_class(ctx, fd->return_type, &rk, &rct, &raggr, &rref) && raggr;
    }
    uint8_t saved_holes = ctx->n_sub_holes;
    ctx->n_sub_holes = 0;
    g_gs_mem[0] = fd; g_gs_nmem = 1; g_gs_ctx = ctx;

    bool ok = gs_build(&gs);
    if (ok) {
        int bi = gs.base_ind;
        /* A by-const-ptr aggregate param needs a stable function-scope buffer to
         * re-home its heap-boxed pointee into on resume (the param pointer itself
         * points into the caller's collapsed frame; see gs_restore / the descend
         * arg-pass in gs_self_descend). */
        for (int i = 0; i < gs.n_param_vars; i++) {
            if (!gs.vars[i].is_ref) continue;
            indent_buf(file, bi);
            buf_printf(file, "%s %s__agg;\n", gs.vars[i].ctype, gs.vars[i].cname);
        }
        for (int i = gs.n_param_vars; i < gs.n_vars; i++) {
            indent_buf(file, bi);
            /* AR: an aggregate-return suspension temp is a by-value struct; a
             * scalar `= 0` init is ill-typed, so zero-init with a compound. */
            if (gs.vars[i].is_aggr)
                buf_printf(file, "%s %s = {0};\n", gs.vars[i].ctype, gs.vars[i].cname);
            else
                buf_printf(file, "%s %s = 0;\n", gs.vars[i].ctype, gs.vars[i].cname);
        }
        indent_buf(file, bi); buf_puts(file, "tur_cont *__k = (tur_cont*)malloc(sizeof(tur_cont));\n");
        indent_buf(file, bi); buf_puts(file, "__k->tag = 0; __k->boundary = 0; __k->next = 0; __k->aggr_mask = 0;\n");
        indent_buf(file, bi); buf_puts(file, "int __pc = 1;\n");
        indent_buf(file, bi); buf_puts(file, "int64_t __v = 0;\n");
        gs_emit_driver(&gs, file, bi, /*done_typed=*/true);
        ctx->indent = bi;
    }

    gs_free(&gs);
    g_gs_nmem = 0;
    ctx->n_sub_holes = saved_holes;
    return ok;
}

/* ===== G4: cross-function / mutual recursion (shared group driver) ===== */

static bool gs_is_pure_accessor(const Expr *e) {
    if (!e || e->kind != EX_CALL || e->as.call_.fn_expr) return false;
    Binding *fb = e->as.call_.fn_binding;
    const char *nm = (fb && fb->name) ? fb->name->name : NULL;
    static const char *const pure[] = { "ok?", "err?", "ok-val", "err-val", "some?", "none?", NULL };
    for (int i = 0; nm && pure[i]; i++) if (strcmp(nm, pure[i]) == 0) return true;
    return false;
}

/* Collect the distinct trampoline-candidate callees (direct calls resolvable to
 * a top-level FnDef, excluding the pure accessors) reachable in e, descending
 * into catch-unwind thunk bodies. */
static void gs_callees(const Expr *e, FnDef **out, int *n, int cap) {
    if (!e || *n >= cap) return;
    if (e->kind == EX_CALL && !e->as.call_.fn_expr && !gs_is_pure_accessor(e)) {
        Binding *fb = e->as.call_.fn_binding;
        FnDef *g = (fb && fb->source_fn_def) ? fb->source_fn_def : NULL;
        if (g) {
            bool seen = false;
            for (int i = 0; i < *n; i++) if (out[i] == g) seen = true;
            if (!seen && *n < cap) out[(*n)++] = g;
        }
        for (uint32_t i = 0; i < e->as.call_.n_args; i++) gs_callees(e->as.call_.args[i], out, n, cap);
        return;
    }
    if (e->kind == EX_CATCH_UNWIND) {
        FnDef *tf = gs_thunk_fn(e);
        if (tf && tf->body) gs_callees(tf->body, out, n, cap);
        return;
    }
    switch (e->kind) {
        case EX_IF: gs_callees(e->as.if_.cond, out, n, cap); gs_callees(e->as.if_.then_, out, n, cap);
                    gs_callees(e->as.if_.else_or_null, out, n, cap); break;
        case EX_LET:
            for (uint32_t i = 0; i < e->as.let_.n; i++) { gs_callees(e->as.let_.bindings[i].init, out, n, cap); }
            gs_callees(e->as.let_.body, out, n, cap); break;
        case EX_DO:
            for (uint32_t i = 0; i < e->as.do_.n; i++) { gs_callees(e->as.do_.items[i], out, n, cap); }
            break;
        case EX_BUILTIN:
            for (uint32_t i = 0; i < e->as.builtin.n; i++) { gs_callees(e->as.builtin.args[i], out, n, cap); }
            break;
        case EX_CALL:
            for (uint32_t i = 0; i < e->as.call_.n_args; i++) { gs_callees(e->as.call_.args[i], out, n, cap); }
            break;
        case EX_CAST:       gs_callees(e->as.cast_.expr, out, n, cap); break;
        case EX_REINTERPRET:    gs_callees(e->as.reinterpret_.expr, out, n, cap); break;
        case EX_ASCRIBE:    gs_callees(e->as.ascribe_.inner, out, n, cap); break;
        case EX_PANIC_PAYLOAD_TYPE:  gs_callees(e->as.panic_payload_type_.payload, out, n, cap); break;
        case EX_PANIC_PAYLOAD_VALUE: gs_callees(e->as.panic_payload_value_.payload, out, n, cap); break;
        case EX_PANIC:      gs_callees(e->as.panic_.payload, out, n, cap); break;
        case EX_PANIC_WITH: gs_callees(e->as.panic_with_.payload, out, n, cap); break;
        default: break;
    }
}

/* Compute the trampolined group containing fd: the greatest set of basic-eligible
 * functions, reachable from fd via calls, such that every member's user-calls
 * target only members (or pure accessors) and the group as a whole reaches a
 * catch-unwind.  Returns the member count (fd first) or 0 if fd is not a valid
 * group member.  Leaves g_gs_* cleared. */
static int gs_find_group(EmitCtx *ctx, FnDef *fd, FnDef **group) {
    g_gs_ctx = ctx;
    FnDef *cand[GS_MAXMEM]; int nc = 0;
    cand[nc++] = fd;
    for (int i = 0; i < nc; i++) {
        FnDef *cs[64]; int ncs = 0;
        gs_callees(cand[i]->body, cs, &ncs, 64);
        for (int j = 0; j < ncs; j++) {
            if (!gs_basic_ok(ctx, cs[j])) continue;
            bool seen = false;
            for (int k = 0; k < nc; k++) if (cand[k] == cs[j]) seen = true;
            if (!seen) { if (nc >= GS_MAXMEM) return 0; cand[nc++] = cs[j]; }
        }
    }
    /* Refine to a fixpoint: drop any member whose body is not structurally
     * handled under the current member set (e.g. it calls a non-member). */
    bool changed = true;
    while (changed) {
        changed = false;
        g_gs_nmem = nc;
        for (int i = 0; i < nc; i++) g_gs_mem[i] = cand[i];
        for (int i = 0; i < nc; i++) {
            if (!gs_stmt_ok(cand[i]->body, cand[i])) {
                for (int k = i; k < nc - 1; k++) cand[k] = cand[k + 1];
                nc--; i--; changed = true;
            }
        }
        g_gs_nmem = 0;
    }
    bool has_fd = false;
    for (int i = 0; i < nc; i++) if (cand[i] == fd) has_fd = true;
    if (!has_fd || nc < 1) return 0;
    g_gs_nmem = nc;
    for (int i = 0; i < nc; i++) g_gs_mem[i] = cand[i];
    bool has_catch = false;
    for (int i = 0; i < nc; i++) if (gs_has_catch(cand[i]->body, cand[i])) has_catch = true;
    g_gs_nmem = 0;
    if (!has_catch) return 0;
    for (int i = 0; i < nc; i++) group[i] = cand[i];
    return nc;
}

/* Registry of emitted group drivers (reset per module in emit_module). */
typedef struct {
    FnDef *mem[GS_MAXMEM]; int n_mem;
    int entry[GS_MAXMEM];
    TypeKind ret[GS_MAXMEM];
    const char *retctype[GS_MAXMEM];
    char name[40];
    uint32_t max_arity;
} GsGroup;
static GsGroup g_cu_groups[64];
static int g_n_cu_groups;
static int g_cu_driver_ctr;

void gs_reset_group_registry(void) { g_n_cu_groups = 0; g_cu_driver_ctr = 0; }

/* Emit the shared driver function for a built group into pending_handler_fns
 * (so it lands at file scope before any member shim). */
static void gs_emit_group_driver(EmitCtx *ctx, Buf *file, GsCtx *gs,
                                 const char *name, uint32_t maxar) {
    Buf *out = ctx->pending_handler_fns ? ctx->pending_handler_fns : file;
    buf_printf(out, "static int64_t %s(int __pc", name);
    for (uint32_t a = 0; a < maxar; a++) buf_printf(out, ", int64_t __a%d", a);
    buf_puts(out, ") {\n");
    int bi = 1;
    for (int i = 0; i < gs->n_vars; i++) {
        indent_buf(out, bi);
        if (gs->vars[i].is_ref) {
            /* by-const-ptr aggregate: a pointer local plus a stable pointee
             * buffer (the seed / descend / resume re-home the boxed pointee into
             * the buffer and point the local at it -- mirrors the single-function
             * path's function-scope `<cname>__agg`).  Point it at a zeroed buffer
             * from the start, not null: gs_save boxes EVERY member's aggregate
             * param each descend (reading THROUGH this pointer), so a not-yet-
             * seeded member's param must be a valid (if unused) pointee, never a
             * null deref. */
            buf_printf(out, "%s %s__agg; memset(&%s__agg, 0, sizeof(%s)); const %s *%s = &%s__agg;\n",
                       gs->vars[i].ctype, gs->vars[i].cname, gs->vars[i].cname, gs->vars[i].ctype,
                       gs->vars[i].ctype, gs->vars[i].cname, gs->vars[i].cname);
        } else if (gs->vars[i].is_aggr) {
            /* by-value aggregate: a zeroed struct local, seeded from its transfer
             * box on entry and saved/restored by the heap-box path across
             * descends.  Zero-init so a save of a not-yet-seeded member's param
             * boxes defined bytes. */
            buf_printf(out, "%s %s; memset(&%s, 0, sizeof(%s));\n",
                       gs->vars[i].ctype, gs->vars[i].cname, gs->vars[i].cname, gs->vars[i].ctype);
        } else {
            buf_printf(out, "%s %s = 0;\n", gs->vars[i].ctype, gs->vars[i].cname);
        }
    }
    /* Seed the entering member's params from the __a bit-slots.  An aggregate
     * param arrives as a heap box pointer (the shim malloc'd + memcpy'd it); copy
     * it into the by-value local and free the transfer box. */
    indent_buf(out, bi); buf_puts(out, "switch (__pc) {\n");
    for (int m = 0; m < gs->n_mem; m++) {
        indent_buf(out, bi + 1);
        buf_printf(out, "case %d: {\n", gs->mem_entry_tag[m]);
        for (uint32_t p = 0; p < gs->mem[m]->n_params; p++) {
            int vi = gs->mem_pbase[m] + p;
            char slot[16]; snprintf(slot, sizeof slot, "__a%u", (unsigned)p);
            indent_buf(out, bi + 2);
            if (gs->vars[vi].is_ref) {
                buf_printf(out, "{ void *__ab = (void*)(intptr_t)%s; memcpy(&%s__agg, __ab, sizeof(%s)); free(__ab); %s = &%s__agg; }\n",
                           slot, gs->vars[vi].cname, gs->vars[vi].ctype, gs->vars[vi].cname, gs->vars[vi].cname);
            } else if (gs->vars[vi].is_aggr) {
                buf_printf(out, "{ void *__ab = (void*)(intptr_t)%s; memcpy(&%s, __ab, sizeof(%s)); free(__ab); }\n",
                           slot, gs->vars[vi].cname, gs->vars[vi].ctype);
            } else {
                char *rs = sc_restore_expr(gs->vars[vi].ctype, gs->vars[vi].kind, slot);
                buf_printf(out, "%s = %s;\n", gs->vars[vi].cname, rs); free(rs);
            }
        }
        indent_buf(out, bi + 2); buf_puts(out, "break; }\n");
    }
    indent_buf(out, bi); buf_puts(out, "}\n");
    indent_buf(out, bi); buf_puts(out, "tur_cont *__k = (tur_cont*)malloc(sizeof(tur_cont));\n");
    indent_buf(out, bi); buf_puts(out, "__k->tag = 0; __k->boundary = 0; __k->next = 0; __k->aggr_mask = 0;\n");
    indent_buf(out, bi); buf_puts(out, "int64_t __v = 0;\n");
    gs_emit_driver(gs, out, bi, /*done_typed=*/false);
    buf_puts(out, "}\n");
}

/* Emit fd as a member of a cross-function trampolined group: ensure the group's
 * shared driver exists, then emit fd's body as a shim into that driver.  Returns
 * false (nothing emitted) on overflow. */
static bool emit_group_member(EmitCtx *ctx, Buf *file, FnDef *fd, TypeKind result_kind,
                              FnDef **group, int ng) {
    /* Part A: an aggregate param now rides the shared driver too -- it travels
     * by pointer across the shim/seed boundary (a heap box the driver takes
     * ownership of), then behaves like the single-function aggregate param
     * inside the driver.  A member that is otherwise un-rideable still bails. */
    for (int m = 0; m < ng; m++)
        for (uint32_t i = 0; i < group[m]->n_params; i++) {
            TypeKind k; const char *ct; bool aggr; bool ref;
            if (!gs_param_class(ctx, group[m]->param_types[i], &k, &ct, &aggr, &ref))
                return false;
        }
    GsGroup *g = NULL;
    for (int i = 0; i < g_n_cu_groups && !g; i++)
        for (int j = 0; j < g_cu_groups[i].n_mem; j++)
            if (g_cu_groups[i].mem[j] == fd) { g = &g_cu_groups[i]; break; }

    if (!g) {
        if (g_n_cu_groups >= 64) return false;
        /* The shared driver is at file scope, inside no closure/defer/handle/gen
         * body: clear the capture context so member params resolve to plain
         * driver locals, not env-slot accesses (captures share the outer binding,
         * so a catch thunk that captures a param re-emits it as that local). */
        struct Closure *sv_closure = ctx->closure;
        const char *sv_envname = ctx->env_var_name;
        Binding **sv_fnparams = ctx->fn_params; uint8_t sv_nfnparams = ctx->n_fn_params;
        Binding **sv_defer = ctx->defer_captures; uint8_t sv_ndefer = ctx->n_defer_captures;
        Binding **sv_handle = ctx->handle_captures; uint32_t sv_nhandle = ctx->n_handle_captures;
        const char *sv_genvar = ctx->gen_var_name;
        ctx->closure = NULL; ctx->env_var_name = NULL;
        ctx->fn_params = NULL; ctx->n_fn_params = 0;
        ctx->defer_captures = NULL; ctx->n_defer_captures = 0;
        ctx->handle_captures = NULL; ctx->n_handle_captures = 0;
        ctx->gen_var_name = NULL;
        GsCtx gs; memset(&gs, 0, sizeof gs);
        gs.ctx = ctx; gs.fd = group[0]; gs.base_ind = 1;
        gs.n_mem = ng;
        uint32_t maxar = 0;
        for (int m = 0; m < ng; m++) {
            gs.mem[m] = group[m];
            gs.mem_entry_tag[m] = 1 + m;
            gs.mem_ret[m] = group[m]->return_type.kind;
            gs.mem_retctype[m] = emit_type_c_name(ctx, group[m]->return_type);
            /* AR: is THIS member's return a by-value aggregate?  The
             * single-function path (emit_stackless_general_body) has always set
             * this for its one member; the group path left the whole array at
             * its memset-0, so every member's suspension temp was declared
             * `T __t = 0` and saved with `(int64_t)(intptr_t)(__t)` -- an
             * aggregate cast as a scalar.  Nothing in the group population
             * returned a by-value aggregate until SR2a went default and
             * `(Result int int)` became one
             * (stackless-catch-unwind-byref-aggregate-reader-transitive-escape-bail,
             * whose `leak` returns its Result param). */
            {
                TypeKind mrk; const char *mrct; bool mraggr; bool mrref;
                gs.mem_ret_aggr[m] =
                    gs_param_class(ctx, group[m]->return_type,
                                   &mrk, &mrct, &mraggr, &mrref) && mraggr;
            }
            if (group[m]->n_params > maxar) maxar = group[m]->n_params;
        }
        gs.next_tag = ng + 1;
        uint8_t sh = ctx->n_sub_holes; ctx->n_sub_holes = 0;
        /* Register EVERY member's by-const-ptr params as pbp so an accessor /
         * field access on any member's aggregate param (not just the triggering
         * member's) reads the pointer directly instead of materializing it into a
         * struct temp (which would assign a `const T *` into a `T` -- a cc error).
         * The single-member prologue in emit_fn_def only records the triggering
         * member's params. */
        uint32_t sv_npbp = ctx->n_pbp_params; ctx->n_pbp_params = 0;
        for (int m = 0; m < ng; m++)
            for (uint32_t i = 0; i < group[m]->n_params; i++) {
                TypeKind k; const char *ct; bool aggr; bool ref;
                if (gs_param_class(ctx, group[m]->param_types[i], &k, &ct, &aggr, &ref) && ref)
                    emit_pbp_push(ctx, group[m]->params[i]);
            }
        g_gs_nmem = ng; g_gs_ctx = ctx; for (int m = 0; m < ng; m++) g_gs_mem[m] = group[m];
        bool ok = gs_build(&gs);
        if (!ok) { gs_free(&gs); ctx->n_sub_holes = sh; ctx->n_pbp_params = sv_npbp; g_gs_nmem = 0; return false; }
        char name[40]; snprintf(name, sizeof name, "__cu_group_%d", g_cu_driver_ctr++);
        gs_emit_group_driver(ctx, file, &gs, name, maxar);
        ctx->n_pbp_params = sv_npbp;
        g = &g_cu_groups[g_n_cu_groups++];
        g->n_mem = ng; g->max_arity = maxar;
        snprintf(g->name, sizeof g->name, "%s", name);
        for (int m = 0; m < ng; m++) {
            g->mem[m] = group[m]; g->entry[m] = gs.mem_entry_tag[m];
            g->ret[m] = gs.mem_ret[m]; g->retctype[m] = gs.mem_retctype[m];
        }
        gs_free(&gs); ctx->n_sub_holes = sh; g_gs_nmem = 0;
        ctx->closure = sv_closure; ctx->env_var_name = sv_envname;
        ctx->fn_params = sv_fnparams; ctx->n_fn_params = sv_nfnparams;
        ctx->defer_captures = sv_defer; ctx->n_defer_captures = sv_ndefer;
        ctx->handle_captures = sv_handle; ctx->n_handle_captures = sv_nhandle;
        ctx->gen_var_name = sv_genvar;
    }

    int mi = -1;
    for (int j = 0; j < g->n_mem; j++) if (g->mem[j] == fd) mi = j;
    if (mi < 0) return false;

    /* Shim: __cu_group_N(entry, <this fn's args as bit-slots>, 0-fill).  An
     * aggregate arg travels by pointer: box a heap copy here whose ownership the
     * driver's seed takes (it copies into the by-value param local and frees the
     * box).  A by-const-ptr aggregate (is_ref) arg is already a pointer, so read
     * through it; a by-value aggregate needs its address taken. */
    Buf call; buf_init(&call);
    buf_printf(&call, "%s(%d", g->name, g->entry[mi]);
    for (uint32_t p = 0; p < fd->n_params; p++) {
        char *cn = atom_var(ctx, fd->params[p]);
        TypeKind k; const char *ct; bool aggr; bool ref;
        gs_param_class(ctx, fd->param_types[p], &k, &ct, &aggr, &ref);
        if (aggr) {
            char bn[32]; snprintf(bn, sizeof bn, "__ab%u", (unsigned)p);
            indent_buf(file, ctx->indent);
            buf_printf(file, "void *%s = malloc(sizeof(%s)); memcpy(%s, %s%s, sizeof(%s));\n",
                       bn, ct, bn, ref ? "" : "&", cn, ct);
            buf_printf(&call, ", (int64_t)(intptr_t)%s", bn);
        } else {
            char *sv = sc_save_expr(fd->param_types[p].kind, cn);
            buf_printf(&call, ", %s", sv);
            free(sv);
        }
        free(cn);
    }
    for (uint32_t p = fd->n_params; p < g->max_arity; p++) buf_puts(&call, ", 0");
    buf_putc(&call, ')');
    buf_putc(&call, '\0');
    char *rv = sc_restore_expr(ctx->current_fn_ret_ctype, result_kind, call.data);
    indent_buf(file, ctx->indent);
    buf_printf(file, "return %s;\n", rv);
    free(rv); buf_free(&call);
    return true;
}

/* ------------ Phase 2: function emission ------------ */

/* The float kind a tail value actually has in the emitted C, or TY_UNKNOWN when
 * it is not a float.  Usually that is its elaborated type.  The exception is a
 * dictionary-dispatched class-method call inside a spec: elaboration typed it
 * against the class's representative (first-declared) instance, and the
 * emitter re-targets it to the instance this spec selects -- so `(n0 x x)`
 * elaborated `: int` is emitted as `__inst_N0_n0_float(x, x)` into a `double`
 * temp.  That re-resolved instance's DECLARED result is then the answer, read
 * the same way emit_reresolved_returns_byvalue_aggregate reads it.
 * (constrained-generic-float-result-into-generic-value-converts) */
static TypeKind emit_tail_float_kind(EmitCtx *ctx, const Expr *tail_e) {
    TypeKind k = emit_resolve_type(ctx, tail_e->type).kind;
    if (k == TY_FLOAT || k == TY_FLOAT32 || k == TY_FLOAT64) return k;
    /* No call_dispatch_is_static gate: the call emitter does not apply one
     * either (emit_reresolve_method_call gates itself), and a NESTED receiver
     * -- `(n0 (n0 x x) x)` -- elaborates to a concrete `int`, so the outer
     * call reads as static while the emitter still re-targets it. */
    if (tail_e->kind != EX_CALL || !tail_e->as.call_.dict_arg)
        return TY_UNKNOWN;
    FnDef *rfd = emit_reresolve_method_fndef(ctx, tail_e);
    if (!rfd || !rfd->binding || rfd->binding->type.kind != TY_FN)
        return TY_UNKNOWN;
    k = rfd->binding->type.as.fn.result_kind;
    return (k == TY_FLOAT || k == TY_FLOAT32 || k == TY_FLOAT64) ? k : TY_UNKNOWN;
}

/* proper-tail-calls / emit-tail-return-path-lacks-carrier-bridges: the ONE
 * decision for "how does a body's value reach the `return`".
 *
 * This ladder used to live inline in emit_fn_def, with emit_tail carrying a
 * hand-maintained subset of it -- three of its sixteen arms.  Which copy a body
 * got was decided by whether its tail spine happened to hold a self tail call,
 * a question with nothing to do with its result shape, so a body that was BOTH
 * TCO-routed and carrier-returning got the thin copy and miscompiled.  Nothing
 * reported that: the divergence was only reachable once proper-tail-calls T2
 * widened the routing, and it then failed 24 fixtures with hard `cc` errors.
 * Filed as docs/reported/emit-tail-return-path-lacks-carrier-bridges.md, and
 * this function is the fix -- the same treatment `inline_c_returns_byvalue_adt`
 * got, for the same stated reason: so the two copies cannot drift apart again.
 *
 * `tail_e` is the expression whose value is being returned, which is what every
 * result-shape predicate below asks about.  For emit_fn_def that is the whole
 * body; for emit_tail it is the branch, arm or `do`-tail actually in tail
 * position, which is the more precise reading of the same question.
 *
 * Takes ownership of `ret_val` and frees it. */
static void emit_fn_return_spelling(EmitCtx *ctx, Buf *out, const Expr *fn_e,
                                    FnDef *fd, const Expr *tail_e,
                                    char *ret_val, TypeKind result_kind,
                                    bool is_main) {
    /* Derived rather than passed: emit_fn_def's own `use_abi_spec` is exactly
     * this, and a second caller computing it by hand is a way for the two to
     * disagree about which spec is active. */
    const bool use_abi_spec = ctx->current_abi_specialization &&
        ctx->current_abi_specialization->fn == fd;
    /* RSP1: returning a pass-by-ptr struct *parameter* directly.  The param
     * is held in C as `const T *` but the function returns the struct by
     * value, so `return x;` returns a pointer where a `T` is expected.
     * Dereference to copy it out.  A bare struct param is a concrete sized
     * value, never an Option/Result carrier aggregate, so it never collides
     * with the carrier-spill return paths below. */
    if (tail_e) {
        const Expr *re = tail_e;
        while (re && re->kind == EX_ASCRIBE) re = re->as.ascribe_.inner;
        if (re && re->kind == EX_VAR && re->as.var.binding &&
            !(is_main && result_kind == TY_INT)) {
            for (uint32_t _i = 0; _i < ctx->n_pbp_params; _i++) {
                if (ctx->pbp_param_ptrs[_i] == re->as.var.binding) {
                    char *deref = (char *)malloc(strlen(ret_val) + 4);
                    sprintf(deref, "*(%s)", ret_val);
                    free(ret_val);
                    ret_val = deref;
                    break;
                }
            }
        }
    }
    indent_buf(out, ctx->indent);
    /* closure-carrier-return-and-arg-int-pointer-warnings: determine the
     * function's actual C return type so a fn-typed body returned through
     * the int64_t/void* closure carrier gets the matching bridge cast.
     * This mirrors the signature-emission branches in emit_fn_def; the body
     * is guaranteed non-inline-c here (inline-c is handled before either
     * caller reaches this), so the inline-c-only sub-branches there do not
     * apply. */
    const char *ret_ctype = NULL;
    bool inst_method_carrier_spill = false;
    if (fn_e->type.kind == TY_FN && !is_main) {
        /* structdef-retirement DS-C: emit_carrier_return_override is dead
         * (method body never TY_STRUCT); its `carrier_override.kind ==
         * TY_STRUCT` branch below is removed. */
        if (fd->box_aggregate_result) {
            /* WF1/WF2 (van-laarhoven-wide-functor-carrier-plan): the closure
             * returns its wide `(f A)` aggregate boxed into the int64 carrier;
             * the explicit heap-spill return below performs the box. */
            ret_ctype = "int64_t";
        } else if (fd->n_dict_clone > 0) {
            /* MB2.5: keep the return ret_ctype in lockstep with the signature
             * override above -- a dict-clone wrapper returns the int64 carrier. */
            ret_ctype = "int64_t";
        } else if (use_abi_spec) {
            Type rt = ctx->current_abi_specialization->result_type;
            bool is_inst = fd->binding && fd->binding->name &&
                fd->binding->name->name &&
                strncmp(fd->binding->name->name, "__inst_", 7) == 0;
            /* M4c Path A result-side: non-HKT instance method specs
             * (typeclass_inst set) skip the carrier-int64 override.
             * The signature emit at L412 + the forward decl at
             * emit_module.c:1853 use the same gate; keep them in sync. */
            Type rt_resolved = emit_resolve_type(ctx, rt);
            /* M7 layer-4: a per-(f, A) by-value HKT instance-method spec
             * returns the resolved struct (`Option__int`) BY VALUE, not the
             * int64 carrier -- this is the whole point of the spec.  Detect
             * it by the concrete by-value TY_APP result (same guards as
             * construct_recovered_byvalue) and skip the carrier spill
             * below. */
            if (is_inst &&
                rt_resolved.kind == TY_APP &&
                !type_is_heap_struct(rt_resolved) &&
                type_has_concrete_codegen_layout(&rt_resolved)) {
                ret_ctype = emit_type_c_name(ctx, rt);
            } else if (is_inst && type_uses_carrier_abi(rt_resolved)
                && ctx->current_abi_specialization->typeclass_inst == NULL) {
                ret_ctype = "int64_t";
                inst_method_carrier_spill = true;
            } else {
                ret_ctype = emit_type_c_name(ctx, rt);
            }
        } else if (fn_e->type.as.fn.result_full_type &&
                   fd->binding && fd->binding->name && fd->binding->name->name &&
                   strncmp(fd->binding->name->name, "__inst_", 7) == 0 &&
                   type_uses_carrier_abi(emit_resolve_type(ctx,
                       *fn_e->type.as.fn.result_full_type))) {
            ret_ctype = "int64_t";
            inst_method_carrier_spill = true;
        } else if (fn_e->type.as.fn.result_full_type &&
                   emit_inst_fn_return_carrier(fd,
                       fn_e->type.as.fn.result_full_type)) {
            ret_ctype = emit_inst_fn_return_carrier(fd,
                fn_e->type.as.fn.result_full_type);
        } else if (fn_e->type.as.fn.result_full_type) {
            Type rft = *fn_e->type.as.fn.result_full_type;
            const char *fn_ret_td = fn_e->type.as.fn.result_fat
                ? NULL : emit_fn_return_typedef(fd, &rft);
            ret_ctype = fn_ret_td ? fn_ret_td : emit_type_c_name(ctx, rft);
        } else if (fd->binding && fd->binding->name && fd->binding->name->name &&
                   strncmp(fd->binding->name->name, "__inst_", 7) == 0 &&
                   tail_e && (tail_e->type.kind == TY_APP ||
                                tail_e->type.kind == TY_STRUCT ||
                                type_uses_carrier_abi(tail_e->type))) {
            /* Direction (1): non-spec instance method, result_full_type
             * absent.  Spill only when body codegen is by-value struct
             * (matching the signature fallback above).  repro 2: a by-value
             * monomorph body (carrier-ABI false) is included -- the dict
             * slot is int64 regardless, so it must spill too. */
            const char *_body_c2 = emit_type_c_name(ctx, tail_e->type);
            if (_body_c2 && strcmp(_body_c2, "int64_t") != 0) {
                ret_ctype = "int64_t";
                inst_method_carrier_spill = true;
            } else {
                ret_ctype = emit_type_c_name(ctx,
                    emit_type_from_kind(fn_e->type.as.fn.result_kind));
            }
        } else {
            ret_ctype = emit_type_c_name(ctx,
                emit_type_from_kind(fn_e->type.as.fn.result_kind));
        }
    }
    bool ret_is_int64_carrier = ret_ctype &&
        strcmp(ret_ctype, "int64_t") == 0;
    /* What the hoist temp HOLDS settles an int64 return before any of the
     * tail predicates below, which read the elaborated EXPRESSION: a temp
     * recorded `int64_t` is already the word, whatever its type says.  A mixed
     * spec (`__inst_R_rm_W__spec__int64_t_tur_adt_W`) and a word-returning
     * dict slot both hand one back under a concrete-looking type (`W`), and
     * every box path below then assigned the int64 into a struct. */
    const char *ret_val_ct = (ret_val && emit_str_is_bare_ident(ret_val))
        ? emit_localvar_lookup_ctype(ret_val) : NULL;
    /* Special case: if this is main and it returns int64_t, cast to int */
    if (is_main && result_kind == TY_INT) {
        buf_printf(out, "return (int)%s;\n", ret_val);
    } else if (ret_is_int64_carrier && ret_val_ct &&
               strcmp(ret_val_ct, "int64_t") == 0) {
        buf_printf(out, "return %s;\n", ret_val);
    } else if (fd->box_aggregate_result) {
        /* WF1/WF2/WF3 (van-laarhoven-wide-functor-carrier-plan): a functor-
         * wrapping closure `g` for a wide-functor lens must return the int64
         * carrier (the generic dict-clone fat-dispatches it int64-in/int64-out
         * and the lens caller unboxes at the poly-carrier boundary).  Its body
         * tail comes in two shapes:
         *   - a by-value aggregate (`mk_id__spec(...)` -> `tur_adt_Identity__int`)
         *     in the concrete / ABI-specialized emit -- heap-box it (the
         *     inverse of the caller's unbox, mirroring emit_agg_box); or
         *   - the int64 carrier already (`mk_hyid(...)`) in the GENERIC base
         *     emit (the functor is abstract there) -- return it directly, or
         *     malloc-boxing a carrier word would double-box. */
        if (tail_e && fn_body_tail_emits_byvalue_carrier_abi(ctx, tail_e)) {
            Type src = fn_body_tail_byvalue_carrier_type(ctx, tail_e);
            if (src.kind == TY_UNKNOWN)
                src = fn_e->type.as.fn.result_full_type
                    ? emit_resolve_type(ctx, *fn_e->type.as.fn.result_full_type)
                    : emit_resolve_type(ctx, tail_e->type);
            const char *scty = emit_type_c_name(ctx, src);
            buf_printf(out,
                "{ %s *__tur_ret_p = (%s *)malloc(sizeof(%s)); "
                "*__tur_ret_p = %s; "
                "return (int64_t)(intptr_t)__tur_ret_p; }\n",
                scty, scty, scty, ret_val);
        } else {
            buf_printf(out, "return (int64_t)(intptr_t)%s;\n", ret_val);
        }
    } else if (fd->n_dict_clone > 0) {
        /* MB2.5 (constrained-hkt-forall-mode-b-plan): a dict-clone wrapper's
         * body is a single dispatch through the carrier dict, which already
         * yields the int64 carrier (emit_call_name + the M7-spec suppression
         * keep it carrier even for by-value aggregate functors).  Return that
         * carrier directly -- NONE of the by-value/aggregate spill heuristics
         * below apply, and firing one would malloc-box a value that is already
         * the carrier (double-box).  The `(int64_t)(intptr_t)` cast is a no-op
         * on an int64 and harmless if the body tail is a bare pointer.
         *
         * forall-dict-float-result-truncated: it is NOT a no-op on a double.
         * A method declared `: float` dispatches to a `double`-returning slot,
         * and `(int64_t)(intptr_t)` on a double is a C numeric conversion --
         * 2.5 arrived as 2, 7.1 as 7, with no warning and no diagnostic.
         * Reinterpret the BITS instead, which is what every other float/carrier
         * crossing does (see tur_sc_bits_f64 in the preamble, whose own comment
         * says an intptr_t cast would truncate).  The consumer side unpacks
         * symmetrically in emit_expr.c's poly-call result handling -- the two
         * must stay in lockstep, since a one-sided change turns a truncation
         * into garbage. */
        TypeKind clone_rk = tail_e
            ? emit_resolve_type(ctx, tail_e->type).kind : TY_UNKNOWN;
        /* dict-slot-classvar-scalar-result: a dispatch through a slot whose
         * class result is the class variable now returns the WORD (the slot
         * wrapper boxed / bit-packed it), into a temp recorded `int64_t` --
         * even where the elaborator typed the call with a sole instance's
         * concrete result (`W`, `double`).  That value is already the carrier;
         * boxing or bit-packing it again would double-convert. */
        if (ret_val && emit_str_is_bare_ident(ret_val)) {
            const char *rvc = emit_localvar_lookup_ctype(ret_val);
            if (rvc && strcmp(rvc, "int64_t") == 0) {
                buf_printf(out, "return %s;\n", ret_val);
                goto clone_ret_done;
            }
        }
        /* hkt-dict-clone-tail-generic-call: the body is NOT always a single
         * dispatch.  A constrained generic that calls ANOTHER constrained
         * generic -- `(defn add-two [^Monad M] ... (add-one (add-one m)))` --
         * has a tail that resolves to the callee's by-value spec
         * (`add_one__spec__tur_adt_Option__int...`), a by-value aggregate,
         * and `(int64_t)(intptr_t)` on a struct is a hard cc error ("aggregate
         * value used where an integer was expected").  Heap-box it into the
         * carrier, exactly as the box_aggregate_result branch above does; a
         * tail that is already the carrier falls through to the bare bridge.
         * The box outlives any region bracket around the call, so its words
         * are noted like every other malloc'd carrier box (emit_agg_box). */
        if (tail_e && fn_body_tail_emits_byvalue_carrier_abi(ctx, tail_e)) {
            Type src = fn_body_tail_byvalue_carrier_type(ctx, tail_e);
            if (src.kind == TY_UNKNOWN) src = emit_resolve_type(ctx, tail_e->type);
            const char *scty = emit_type_c_name(ctx, src);
            if (scty && strcmp(scty, "int64_t") != 0 && !strchr(scty, '*')) {
                buf_printf(out,
                    "{ %s *__tur_ret_p = (%s *)malloc(sizeof(%s)); "
                    "*__tur_ret_p = %s; "
                    "TUR_REGION_NOTE_WORDS(__tur_ret_p, sizeof *__tur_ret_p); "
                    "return (int64_t)(intptr_t)__tur_ret_p; }\n",
                    scty, scty, scty, ret_val);
                goto clone_ret_done;
            }
        }
        if (clone_rk == TY_FLOAT || clone_rk == TY_FLOAT64)
            buf_printf(out, "return tur_sc_bits_f64(%s);\n", ret_val);
        else if (clone_rk == TY_FLOAT32)
            buf_printf(out, "return tur_sc_bits_f32(%s);\n", ret_val);
        else
            buf_printf(out, "return (int64_t)(intptr_t)%s;\n", ret_val);
    clone_ret_done: ;
    } else if (inst_method_carrier_spill) {
        /* Direction (1): instance method whose declared result is a
         * parameterized struct (e.g. (Result T E)) returns the carrier
         * int64 handle.  Heap-spill the by-value struct and cast its
         * pointer as int64_t so the dispatch dict's uniform
         * `int64_t (*)(...)` signature is honored. */
        Type rt;
        if (use_abi_spec) {
            rt = ctx->current_abi_specialization->result_type;
        } else if (fn_e->type.as.fn.result_full_type) {
            rt = emit_resolve_type(ctx, *fn_e->type.as.fn.result_full_type);
        } else {
            rt = tail_e->type;
        }
        const char *struct_cty = emit_type_c_name(ctx, rt);
        /* M7 (flag-gated): in a GENERIC carrier base instance method whose
         * declared result is a parameterized struct with a free element
         * tyvar -- e.g. `Decode`'s `(Result a cstr)` -- `rt` resolves to the
         * int64 carrier (no instance specialization active here).  But the
         * concrete instance body recovers its construct BY VALUE
         * (`(ok (make-struct Point ...))` -> `ok__spec` returning
         * `Result__Point__cstr`), so `ret_val` is a by-value aggregate.
         * Spilling it as `sizeof(int64_t)` + `*(int64_t*)p = <struct>` is a
         * hard cc error (aggregate into integer).  When the body's own type
         * is a concrete non-carrier aggregate, spill THAT type so the malloc
         * size and the cast match the actual value.
         * See docs/archive/history/m7-hkt-bimap-twoparam-struct-tyvar-leak.md
         * / the instance-method-return-carrier-bridge fixture. */
        if (struct_cty &&
            strcmp(struct_cty, "int64_t") == 0 && tail_e) {
            const char *body_cty = emit_type_c_name(ctx, tail_e->type);
            if (body_cty && strcmp(body_cty, "int64_t") != 0)
                struct_cty = body_cty;
        }
        /* hkt-rc-construct-body-boxes-handle: the spill exists to pass a
         * BY-VALUE aggregate through the dict's uniform `int64_t` slot.  A
         * value that is already carrier-width needs no box, and boxing one
         * anyway is a silent miscompile -- the consumer reads the
         * pointer-to-value as the value.
         *
         * Two shapes qualify.  The int64 carrier itself was already handled
         * (see below).  A POINTER is the other: an instance method whose
         * result is a pointer-family handle -- `Functor [rc]`'s `(f b)`
         * grounding to `rc<int>`, i.e. `RcControlBlock *` -- returns a
         * pointer that fits the carrier exactly.  Boxing it emitted
         * `RcControlBlock **__tur_ret_p = malloc(...)` and the dispatch
         * consumer then cast that cell straight to `RcControlBlock *`, so
         * `rc/strong-count` read a malloc header as a refcount and the value
         * was never reachable (measured: garbage count, fold 0, 752768 bytes
         * leaked over 5000 iterations).
         *
         * The next branch down already treats a TY_RC/TY_WEAK/TY_REF/TY_LREF
         * body returned through the int64 carrier exactly this way -- a bare
         * `(int64_t)(intptr_t)` bridge -- so this only stops the spill from
         * intercepting a case that was already handled correctly downstream. */
        bool spill_ty_is_ptr = struct_cty && strchr(struct_cty, '*') != NULL;
        if (struct_cty &&
            (strcmp(struct_cty, "int64_t") == 0 || spill_ty_is_ptr)) {
            /* M7: the body already produced the carrier int64 handle --
             * e.g. a partial-application `(Result _ E)` instance whose
             * pure-Turmeric body lowered to the carrier `ok`/`err` (the
             * spill type stays int64 because the result element doesn't
             * ground by value).  There is nothing to box: malloc'ing another
             * int64 and storing the handle into it DOUBLE-boxes (the
             * by-value consumer then reads a pointer-to-handle as the struct
             * -> garbage, the `(Result _ E)` fmr returning 0 for 42).
             * Return the carrier value directly. */
            buf_printf(out, "return (int64_t)(intptr_t)%s;\n", ret_val);
        } else {
            buf_printf(out,
                "{ %s *__tur_ret_p = (%s *)malloc(sizeof(%s)); "
                "*__tur_ret_p = %s; "
                "return (int64_t)(intptr_t)__tur_ret_p; }\n",
                struct_cty, struct_cty, struct_cty, ret_val);
        }
    } else if (((emit_resolve_type(ctx, tail_e->type).kind == TY_FN ||
                 emit_resolve_type(ctx, tail_e->type).kind == TY_PTR_VOID ||
                 emit_resolve_type(ctx, tail_e->type).kind == TY_CSTR ||
                 emit_resolve_type(ctx, tail_e->type).kind == TY_RC ||
                 emit_resolve_type(ctx, tail_e->type).kind == TY_WEAK ||
                 emit_resolve_type(ctx, tail_e->type).kind == TY_REF ||
                 emit_resolve_type(ctx, tail_e->type).kind == TY_LREF ||
                 emit_resolve_type(ctx, tail_e->type).kind == TY_FORALL ||
                 emit_resolve_type(ctx, tail_e->type).kind == TY_EXISTS ||
                 (emit_resolve_type(ctx, tail_e->type).kind == TY_STRUCT &&
                  strchr(type_c_name(emit_resolve_type(ctx, tail_e->type)), '*') != NULL))) &&
               (result_kind == TY_INT || ret_is_int64_carrier)) {
        /* A function-typed, pointer-typed, or cstr body returned through the int64_t
         * carrier: a bare non-capturing fn reference is a `void *(...)`
         * function pointer, and a fat box is declared `void *`, but the C
         * return type is the int64_t carrier.  Without the (int64_t)(intptr_t)
         * bridge, clang trips -Wint-conversion on the implicit pointer-to-int
         * conversion.  (A void* carrier return needs no cast: the body value
         * is already a pointer.) */
        buf_printf(out, "return (int64_t)(intptr_t)%s;\n", ret_val);
    } else if (ret_is_int64_carrier && fd && tail_e &&
        fn_e->type.kind == TY_FN &&
        (fn_e->type.as.fn.result_kind == TY_FLOAT ||
         fn_e->type.as.fn.result_kind == TY_FLOAT64 ||
         fn_e->type.as.fn.result_kind == TY_FLOAT32) &&
        (tail_e->type.kind == TY_FLOAT ||
         tail_e->type.kind == TY_FLOAT64 ||
         tail_e->type.kind == TY_FLOAT32)) {
        /* method-result-float-spec-return-value-converts: this clone's C
         * return is the int64 carrier, its DECLARED result is a float, and
         * its body tail is a float.  A plain `return ret_val;` is then C's
         * implicit floating-to-integer CONVERSION (7.1 -> 7) while every
         * consumer of a float-declared method result reinterprets the
         * carrier's BITS back -- so the value is destroyed before it
         * leaves the callee (`(l1g2 (l1p1 7.1))` printed 3.45846e-323).
         *
         * The key is the DECLARED result kind (`fn_e->type.as.fn.result_kind`
         * -- the instance-resolved signature), and that key is what pairs
         * producer with consumer: the consumer bit-reinterprets exactly
         * when the call's resolved result type is a float, i.e. the same
         * declared type read from the other end.  A method DECLARED `: int`
         * whose instance body happens to produce a float (BoxMap's boxmap,
         * pinned by poly-to-fat-float-roundtrip expecting `7`) keeps the
         * value conversion, because for it the conversion IS the declared
         * semantics.  The first attempt at this fix keyed on the BODY type
         * alone and broke exactly those fixtures -- see the report's
         * "reverted" record for the measurement.
         *
         * Routed through the carrier chokepoint, whose inline-scalar arm
         * emits the same union bit-cast the consumer side uses. */
        char *bridged = emit_carrier_bridge(ctx, out, strdup(ret_val),
                                            CK_CONCRETE, CK_CARRIER,
                                            tail_e->type);
        indent_buf(out, ctx->indent);
        buf_printf(out, "return %s;\n", bridged);
        free(bridged);
    } else if (use_abi_spec
               && ctx->current_abi_specialization->typeclass_inst != NULL
               && ret_ctype && strcmp(ret_ctype, "int64_t") != 0
               && tail_e->type.kind != TY_NEVER
               && type_uses_carrier_abi(emit_resolve_type(ctx, tail_e->type))
               && strcmp(emit_type_c_name(ctx,
                            emit_resolve_type(ctx, tail_e->type)),
                         "int64_t") == 0
               && !fn_body_tail_emits_byvalue_carrier_abi(ctx, tail_e)) {
        /* M4c Path A result-side: the spec's declared return is a
         * concrete by-value struct (e.g. `Result__int__cstr`), but the
         * body's last expression elaborates to a carrier-ABI value
         * (`int64_t` at C level) — e.g. `(ok v)` where ok still emits
         * its bare carrier symbol.  Route through emit_carrier_bridge so
         * the canonical-carrier field-by-field unbox kicks in for
         * Result/Option sinks with sub-word payloads
         * (decode-bool-carrier-instance-ascription); the legacy
         * `*(T *)(intptr_t)x` deref stays for other parametric sinks.
         *
         * instance-method-return-carrier-bridge: also skip the deref when
         * the body tail already emits the struct by value (a post-M2
         * ^construct spec like `(ok (make-struct ...))` lowers to its
         * by-value `*__spec__*` clone). */
        Type sink_rt = ctx->current_abi_specialization->result_type;
        char *bridged = emit_carrier_bridge(ctx, out, strdup(ret_val),
                                            CK_CARRIER, CK_CONCRETE, sink_rt);
        indent_buf(out, ctx->indent);
        buf_printf(out, "return %s;\n", bridged);
        free(bridged);
    } else if (!ret_is_int64_carrier && ret_ctype
               && tail_e->type.kind != TY_NEVER
               && (type_uses_carrier_abi(emit_resolve_type(ctx, tail_e->type))
                   || fn_body_tail_returns_carrier_value(ctx, tail_e))
               && fn_body_tail_is_carrier_producer(tail_e)
               && !fn_body_tail_emits_byvalue_carrier_abi(ctx, tail_e)) {
        /* M5 straddle (root cause C): an ordinary function or lifted lambda
         * whose declared return is a by-value carrier aggregate
         * (Option__int / Result__int__int) but whose tail value comes from
         * a carrier-int64 producer (some/ok/err/none or an __inst_ method).
         * Same carrier->concrete unbox as the M4c spec branch above. */
        Type sink_rt = (fn_e->type.kind == TY_FN && fn_e->type.as.fn.result_full_type)
            ? emit_resolve_type(ctx, *fn_e->type.as.fn.result_full_type)
            : emit_resolve_type(ctx, tail_e->type);
        char *bridged = emit_carrier_bridge(ctx, out, strdup(ret_val),
                                            CK_CARRIER, CK_CONCRETE, sink_rt);
        indent_buf(out, ctx->indent);
        buf_printf(out, "return %s;\n", bridged);
        free(bridged);
    } else if (ret_is_int64_carrier && tail_e
               && tail_e->type.kind != TY_NEVER
               && fn_body_tail_emits_byvalue_carrier_abi(ctx, tail_e)
               && fn_body_tail_byvalue_carrier_type(ctx, tail_e).kind != TY_UNKNOWN) {
        /* Phase 5 carrier-bridge deletion (concrete->carrier return): the C
         * return is the uniform int64 carrier (a lifted lambda thunk in a
         * poly_fn slot, or a generic carrier base) but the body tail now
         * produces a by-value Option/Result struct (a monomorphized
         * ^construct spec like `some__spec`).  Heap-spill the struct and
         * return its pointer as int64 -- the SAME malloc spill the
         * inst_method_carrier_spill path uses, so the carrier consumer
         * (which derefs a {is_some,value}/{is_ok,...} layout) reads it
         * correctly.  A stack spill (emit_carrier_bridge concrete->carrier)
         * would return a dangling address, so it is NOT used here.  The
         * concrete type comes from the matched spec, since the construct's
         * own fn_e->type is collapsed to the int64 carrier. */
        Type src = fn_body_tail_byvalue_carrier_type(ctx, tail_e);
        if (type_is_heap_struct(src) || type_is_heap_adt(src)) {
            /* constrained-defn-monomorphize: a `:heap` struct tail (e.g.
             * `(Cons A)` built by `tcons-of`) is ALREADY a pointer (`Cons__A *`)
             * that fits the int64 carrier directly.  Malloc-boxing it
             * double-boxes (the carrier consumer then reads a pointer-to-pointer
             * as the cell -> a length-1 / garbage list), the heap-struct mirror
             * of the M7 double-box guard above.  Cast the pointer to the carrier
             * instead of spilling.  seam 3: a lowered `:heap` ADT tail
             * (`type_is_heap_adt`) is the same typed-pointer shape. */
            buf_printf(out, "return (int64_t)(intptr_t)%s;\n", ret_val);
        } else {
            const char *cty = emit_type_c_name(ctx, src);
            buf_printf(out,
                "{ %s *__tur_ret_p = (%s *)malloc(sizeof(%s)); "
                "*__tur_ret_p = %s; "
                "return (int64_t)(intptr_t)__tur_ret_p; }\n",
                cty, cty, cty, ret_val);
        }
    } else if (ret_is_int64_carrier && tail_e &&
               tail_e->type.kind != TY_NEVER &&
               type_is_heap_adt(emit_resolve_type(ctx, tail_e->type))) {
        /* seam 3: the function returns the int64 carrier (an abstract
         * parametric base like `tcons : (Cons A)`), but the body tail is a
         * lowered `:heap` ADT value -- a typed pointer (`ctor_Cons(..)` ->
         * `tur_adt_Cons *`) whose bit pattern IS the carrier.  Reinterpret-cast
         * it, never return it uncast (-Wint-conversion) and never malloc-box
         * (double-box).  The non-parametric mirror is the `type_is_heap_adt`
         * arm in `type_uses_carrier_abi` / the heap-struct return cast above. */
        buf_printf(out, "return (int64_t)(intptr_t)%s;\n", ret_val);
    } else if (ret_is_int64_carrier && tail_e &&
               tail_e->type.kind != TY_NEVER &&
               fn_e->type.kind == TY_FN &&
               (fn_e->type.as.fn.result_kind == TY_TYVAR ||
                (fn_e->type.as.fn.result_full_type &&
                 fn_e->type.as.fn.result_full_type->kind == TY_TYVAR)) &&
               emit_tail_float_kind(ctx, tail_e) != TY_UNKNOWN) {
        /* nested-construct-byvalue (Gap #4, float element): a generic
         * accessor (`ok-val`) whose DECLARED result is a bare tyvar (`: A`)
         * collapses to the int64 carrier return, but inside a `(Result float
         * cstr)` spec its body tail is a concrete `double` field read.  A plain
         * `return <double>;` through an `int64_t` result NUMERICALLY converts
         * (3.25 -> 3), and the caller's union reinterpret then reads garbage.
         * Bit-reinterpret the float into the int64 carrier so the carried
         * value round-trips (the cstr/pointer element is already bit-
         * preserving through the implicit pointer->int64 return cast).  Gated
         * on a TYVAR-declared result so a genuine `: float` function carried
         * through the int64 poly-fn slot (poly-to-fat-float-*) -- which uses a
         * NUMERIC convention -- is untouched.
         *
         * constrained-generic-float-result-into-generic-value-converts: the
         * tail may also be a class-method call that elaborated against the
         * class's representative (first-declared, e.g. `int`) instance and
         * that this spec re-targets to its float instance.  Its hoist temp is
         * then a `double`, so the bridge source is the float kind
         * emit_tail_float_kind recovered, not the elaborated int.  Such a
         * CONSTRAINED generic's `result_kind` is already lowered to the int
         * carrier, so the tyvar-declared test also reads `result_full_type`,
         * which still spells the `A`. */
        Type body_rt = emit_type_from_kind(emit_tail_float_kind(ctx, tail_e));
        char *bridged = emit_carrier_bridge(ctx, out, strdup(ret_val),
                                            CK_CONCRETE, CK_CARRIER, body_rt);
        indent_buf(out, ctx->indent);
        buf_printf(out, "return %s;\n", bridged);
        free(bridged);
    } else if (!ret_is_int64_carrier && ret_ctype && ret_val &&
               tail_e && tail_e->type.kind != TY_NEVER &&
               (strcmp(ret_ctype, "double") == 0 ||
                strcmp(ret_ctype, "float") == 0) &&
               (emit_tail_call_returns_tyvar_carrier(ctx, tail_e) ||
                (emit_str_is_bare_ident(ret_val) &&
                 emit_localvar_lookup_ctype(ret_val) &&
                 strcmp(emit_localvar_lookup_ctype(ret_val), "int64_t") == 0))) {
        /* generic-tail-call-carrier-into-float-return: a spec whose C return
         * is a concrete `double`/`float` (`first-of [A] ... : A` at A=float)
         * but whose tail value is the int64 CARRIER -- a generic callee
         * returning a bare tyvar (`(vec-get v 0)`), hoisted into an int64
         * temp.  `return __ps;` is then C's implicit integer->float VALUE
         * conversion: 7.1's bit pattern printed as 4.61968e+18.  Keyed on the
         * two C types, not on the tail's shape: an int64 word handed to a
         * float return is a carrier, because a conversion the program ASKED
         * for is spelled TUR_AS and never reaches here as a bare temp. */
        Type sink_rt = emit_type_from_kind(strcmp(ret_ctype, "float") == 0
                                               ? TY_FLOAT32 : TY_FLOAT);
        char *bridged = emit_carrier_bridge(ctx, out, strdup(ret_val),
                                            CK_CARRIER, CK_CONCRETE, sink_rt);
        indent_buf(out, ctx->indent);
        buf_printf(out, "return %s;\n", bridged);
        free(bridged);
    } else if (!ret_is_int64_carrier &&
               ctx->current_fn_ret_ctype &&
               strchr(ctx->current_fn_ret_ctype, '*') != NULL &&
               emit_tail_call_returns_tyvar_carrier(ctx, tail_e)) {
        /* MB2 (constrained-hkt-forall-mode-b-plan): the function's C return
         * type is a concrete pointer (a :heap ADT like `Point *`), but the
         * body tail is a call to a generic fn whose declared result is a bare
         * type variable (`run-id [A] ... : A`), lowered to the int64 carrier.
         * The elaborator kept the call's full composite type (a reinterpret
         * cannot carry a composite) so no wrap was inserted, leaving `ret_val`
         * as the int64 carrier where a pointer is expected -- a
         * -Wint-conversion.  Bridge through intptr_t.  Inert when the call
         * resolves to a concrete by-value spec (the wide-monomorphization path
         * calls `run_id__spec__...Point`, already a pointer). */
        buf_printf(out, "return (%s)(intptr_t)%s;\n",
                   ctx->current_fn_ret_ctype, ret_val);
    } else if (fn_return_needs_carrier_result_bridge(
                   ctx, fd, fn_e, ret_ctype, ret_is_int64_carrier, ret_val)) {
        /* catch-unwind-byvalue-result-return-mismatch: the body value is the
         * int64 carrier (a heap Result box, e.g. a let-bound catch-unwind
         * result returned directly) but the declared return is the by-value
         * Result/Option struct.  Bridge carrier->concrete using the DECLARED
         * return type (whose emit_type_c_name is the struct), so the canonical
         * box readback reconstructs the aggregate field-by-field. */
        Type sink_rt = (fn_e->type.kind == TY_FN && fn_e->type.as.fn.result_full_type)
            ? emit_resolve_type(ctx, *fn_e->type.as.fn.result_full_type)
            : emit_resolve_type(ctx, tail_e->type);
        if (catch_box_tail_sole_owned(tail_e, tail_e)) {
            /* catch-unwind-return-bridge-residuals (Part B): the caught box
             * is sole-owned, so free it after the aggregate is materialized.
             * Capture the carrier in a stable temp (so it is not re-emitted),
             * read the box fields into a return temp, free the now-dead box
             * struct, then return -- freeing the struct only (never the
             * payload, which the returned err aggregate may alias). */
            char *box_tmp = fresh_tmp(ctx);
            indent_buf(out, ctx->indent);
            buf_printf(out, "int64_t %s = (int64_t)(intptr_t)(%s);\n",
                       box_tmp, ret_val);
            char *bridged = emit_carrier_bridge(ctx, out, strdup(box_tmp),
                                                CK_CARRIER, CK_CONCRETE,
                                                sink_rt);
            const char *agg_cty = emit_type_c_name(ctx, sink_rt);
            char *ret_tmp = fresh_tmp(ctx);
            indent_buf(out, ctx->indent);
            buf_printf(out, "%s %s = %s;\n", agg_cty, ret_tmp, bridged);
            indent_buf(out, ctx->indent);
            /* catch-unwind-returned-err-box-payload-leak: a scalar err arm
             * cannot alias the box's panic payload, so the full free
             * reclaims the 32 B payload too; else stay shallow (a
             * pointer/cstr/aggregate err field aliases the payload the
             * returned aggregate now owns). */
            buf_printf(out, "%s(%s);\n",
                       result_err_arm_is_freeable_scalar(&sink_rt)
                           ? "tur_result_box_free"
                           : "tur_result_box_free_shallow",
                       box_tmp);
            indent_buf(out, ctx->indent);
            buf_printf(out, "return %s;\n", ret_tmp);
            free(bridged); free(box_tmp); free(ret_tmp);
        } else {
            char *bridged = emit_carrier_bridge(ctx, out, strdup(ret_val),
                                                CK_CARRIER, CK_CONCRETE,
                                                sink_rt);
            indent_buf(out, ctx->indent);
            buf_printf(out, "return %s;\n", bridged);
            free(bridged);
        }
    } else if (ret_ctype && !is_main && ret_val &&
               ret_ctype[strlen(ret_ctype) - 1] == '*' &&
               ((strcmp(ret_ctype, "void *") != 0 &&
                 (strncmp(ret_val, "(int64_t)", 9) == 0 ||
                  (emit_str_is_bare_ident(ret_val) &&
                   emit_localvar_lookup_ctype(ret_val) &&
                   strcmp(emit_localvar_lookup_ctype(ret_val), "int64_t") == 0))) ||
                fn_tail_emits_int64_carrier(fd, tail_e))) {
        /* gcc14-int-conversion (carrier-representation-tracking): a spec
         * clone whose C return type is a concrete pointer (e.g. an element
         * accessor `err-val [A B] : B` monomorphized to `const char *` /
         * `tur_adt_Vec__X *`) but whose body VALUE is the int64 carrier --
         * either an explicit `(int64_t)..` field read, or a bare call temp
         * RECORDED as int64 (`return __ps_224;` where run_id__spec returns
         * int64 but the fn returns `tur_adt_Point *`).  `return <int64>` into
         * a pointer return type is `pointer from integer` -- a hard error
         * under GCC >= 14.  Reinterpret to the return type (value-preserving).
         *
         * macos-int-conversion-carrier-pointer-straddles (case B): the
         * `void *` exclusion above is conservatism from fe6f47b60, not
         * semantics -- `return <int64>` into `void *` is the same
         * -Wint-conversion error and the same intptr_t round-trip fixes it.
         * It only stays because the string sniff cannot tell a carrier from
         * a genuine pointer; the AST predicate can, so a tail it recognizes
         * bridges for any pointer return type, `void *` included. */
        buf_printf(out, "return (%s)(intptr_t)%s;\n", ret_ctype, ret_val);
    } else if (ret_is_int64_carrier && ret_val &&
               emit_str_is_bare_ident(ret_val) &&
               emit_localvar_lookup_ctype(ret_val) &&
               emit_localvar_lookup_ctype(ret_val)[
                   strlen(emit_localvar_lookup_ctype(ret_val)) - 1] == '*') {
        /* clang int-conversion (reverse straddle): the function returns the
         * int64 carrier but the body value is a bare temp whose real emitted C
         * type is a pointer -- a `void *`-returning `extern-c ... :ptr` ascribed
         * to an opaque carrier (e.g. `(:: (tur_string_from_cstr s) String)`
         * whose __auto_type panic temp is `void *`).  `return <void*>;` from an
         * `int64_t` function is `pointer to integer conversion` -- a hard error
         * under clang's default `-Wint-conversion` (and GCC >= 14 -Werror).
         * Bridge through intptr_t (value-preserving; a no-op for a genuine
         * int64 temp, which this branch never sees -- the recorded type is a
         * pointer). */
        buf_printf(out, "return (int64_t)(intptr_t)%s;\n", ret_val);
    } else if (!is_main && ret_ctype && strcmp(ret_ctype, "void") == 0) {
        /* C11 6.8.6.4p1: a `return` WITH an expression is a constraint
         * violation in a function returning void -- even when the
         * expression is itself void-typed.  A `!`-returning defn whose tail
         * is a call to another `!`-returning defn lands exactly there:
         *   (defn outer [] : ! (inner))  ->  static void outer() { return inner(); }
         * clang accepts it as an extension, so the cc path never
         * complained, but c2mir rejects it and the program silently loses
         * the JIT (panic-trace was the fixture that surfaced this).  Emit
         * the tail as a statement and return separately; the cast keeps
         * -Wunused-value quiet for a non-call tail and is valid on a
         * void-typed one. */
        buf_printf(out, "(void)(%s);\n", ret_val);
        indent_buf(out, ctx->indent);
        buf_puts(out, "return;\n");
    } else if (ret_is_int64_carrier && tail_e && ret_val &&
               /* the function as actually EMITTED returns the word -- a
                * declared `: int` an earlier rule retyped to the body's
                * `(Option int)` returns that aggregate, not the carrier */
               fn_emitted_ret_is_word(ctx, fd, use_abi_spec) &&
               carrier_return_concrete_stmt(ctx, tail_e, ret_val, NULL)) {
        Buf rs; buf_init(&rs);
        carrier_return_concrete_stmt(ctx, tail_e, ret_val, &rs);
        buf_putc(&rs, '\0');
        buf_puts(out, rs.data);
        buf_free(&rs);
    } else {
        buf_printf(out, "return %s;\n", ret_val);
    }
    free(ret_val);
}

/* ============================================================================
 * proper-tail-calls T5 (T-D5): forming a mutual tail-call group, and emitting
 * its fused function.  See the registry above tco_mark for the shape.
 * ========================================================================== */

static const Expr *tcg_expr_of(const EmitCtx *ctx, const FnDef *fd) {
    for (uint32_t i = 0; i < ctx->n_tcg_fn_exprs; i++)
        if (ctx->tcg_fn_exprs[i]->as.fn_def_.fn == fd) return ctx->tcg_fn_exprs[i];
    return NULL;
}

static int tcg_item_pos(const EmitCtx *ctx, const Expr *e) {
    for (uint32_t i = 0; i < ctx->n_tcg_fn_exprs; i++)
        if (ctx->tcg_fn_exprs[i] == e) return (int)i;
    return -1;
}

/* The C return type emit_fn_def will give `e`, for the plain functions a group
 * admits -- the tail of its return-type ladder, whose earlier arms (dict
 * clones, boxed aggregates, instance methods, ABI specs) tcg_member_ok has
 * already excluded.  Cross-checked against the forward declaration's recorded
 * return type where there is one, so a disagreement refuses rather than emits
 * a mismatched fused function.  NULL when the function cannot be a member. */
static char *tcg_ret_ctype(EmitCtx *ctx, const Expr *e, FnDef *fd) {
    if (e->type.kind != TY_FN || e->type.as.fn.result_fat) return NULL;
    TypeKind rk = e->type.as.fn.result_kind;
    if (rk == TY_NIL || rk == TY_NEVER) return NULL;
    const char *ct;
    if (e->type.as.fn.result_full_type) {
        const Type *rft = e->type.as.fn.result_full_type;
        if (emit_inst_fn_return_carrier(fd, rft)) return NULL;
        const char *td = emit_fn_return_typedef(fd, rft);
        ct = td ? td : emit_type_c_name(ctx, *rft);
    } else {
        ct = emit_type_c_name(ctx, emit_type_from_kind(rk));
    }
    if (!ct || !*ct || strcmp(ct, "void") == 0) return NULL;
    char *cn = raw_name_for_binding(fd->binding);
    const char *sig = cn ? emit_sig_lookup_ret_ctype(cn) : NULL;
    free(cn);
    if (sig && strcmp(sig, ct) != 0) return NULL;
    return strdup(ct);
}

/* Can `e` be a member of a fused group at all?  A plain top-level function
 * whose body emit_fn_def would send down the ordinary tail path, and whose
 * every parameter is a plain C value a block local can hold: the signature
 * emitter spells it `<ctype> <name>` and marks nothing on its binding.  Returns
 * the C return type (malloc'd), or NULL. */
static char *tcg_member_ok(EmitCtx *ctx, const Expr *e) {
    FnDef *fd = e->as.fn_def_.fn;
    if (!fd || fd->skip_emission || !fd->binding || !fd->binding->name) return NULL;
    if (fd->closure || fd->is_variadic) return NULL;
    if (fd->n_dict_clone || fd->n_dict_env || fd->box_aggregate_result) return NULL;
    const char *nm = fd->binding->name->name;
    if (!nm || strcmp(nm, "main") == 0 || strncmp(nm, "__inst_", 7) == 0)
        return NULL;
    if (!fd->body || fd->body->kind == EX_INLINE_C) return NULL;
    if (expr_tail_diverges(fd->body)) return NULL;
    /* G3/G4's flat-stack catch-unwind lowerings claim these bodies. */
    if (gs_has_catch(fd->body, fd)) return NULL;
    /* A body the CPS backend emits is not a C tail position; its mutual tail
     * calls are fused there instead (emit_cps_ir.c, CPS mutual tail-call
     * groups).
     *
     * cps-self-tail-call-relies-on-sibling-call: a COLORED function the CPS
     * backend declined is emitted right here, as ordinary direct C -- the
     * fiber path a colored body used to run on is gone -- so its tail calls
     * are C tail positions exactly like an uncolored function's, and the self
     * backedge (tco_mark) already takes them.  Refusing it here left a
     * colored pair such as r7rs's `(define (ping f n) (if (= n 0) 'done
     * (begin (f n) (pong f (- n 1)))))` making a C call per step, which
     * overflowed below -O2. */
    if (ctx->program_root &&
        emit_cps_ir_emits_binding(ctx->program_root, fd->binding))
        return NULL;
    if (!tco_params_simple(ctx, e, fd)) return NULL;
    for (uint32_t i = 0; i < fd->n_params; i++) {
        const Binding *pb = fd->params[i];
        if (!pb || pb->is_poly_fn || pb->is_fat || pb->arrives_as_carrier_box)
            return NULL;
        Type pty = tco_param_type(ctx, e, fd, (uint8_t)i);
        if (type_struct_pass_by_ptr(pty)) return NULL;
        const char *pc = emit_type_c_name(ctx, pty);
        if (!pc) return NULL;
        /* A carrier-ABI param that spells as a concrete struct is by-value
         * here, and one that collapses to the int64 carrier was already
         * refused by tco_params_simple -- the same bar self-TCO uses. */
        if (strcmp(pc, "int64_t") == 0) {
            const char *btc = emit_type_c_name(ctx, emit_resolve_type(ctx, pb->type));
            if (!btc || strcmp(btc, "int64_t") != 0) return NULL;
        }
    }
    return tcg_ret_ctype(ctx, e, fd);
}

/* The direct callees `e` reaches in TAIL position, walking exactly the spine
 * tco_mark walks -- so an edge here is a call tco_mark will mark. */
static void tcg_tail_callees(EmitCtx *ctx, const Expr *e, const Expr *let_e,
                             FnDef **out, int *n, int cap) {
    if (!e || *n >= cap) return;
    switch (e->kind) {
        case EX_CALL: {
            if (e->as.call_.fn_expr || e->as.call_.is_poly_call) return;
            const Binding *fb = e->as.call_.fn_binding;
            FnDef *g = fb ? fb->source_fn_def : NULL;
            if (!g || e->as.call_.n_args != g->n_params) return;
            for (int i = 0; i < *n; i++) if (out[i] == g) return;
            out[(*n)++] = g;
            return;
        }
        case EX_IF:
            if (!e->as.if_.else_or_null) return;
            tcg_tail_callees(ctx, e->as.if_.then_, NULL, out, n, cap);
            tcg_tail_callees(ctx, e->as.if_.else_or_null, NULL, out, n, cap);
            return;
        case EX_DO: {
            if (e->as.do_.n == 0) return;
            bool has_defer = false;
            for (uint32_t i = 0; i < e->as.do_.n; i++)
                if (e->as.do_.items[i]->kind == EX_DEFER) { has_defer = true; break; }
            if (!has_defer) {
                tcg_tail_callees(ctx, e->as.do_.items[e->as.do_.n - 1], NULL,
                                 out, n, cap);
                return;
            }
            if (tco_drop_glue_refusal(e, let_e)) return;
            tcg_tail_callees(ctx, e->as.do_.items[tco_drop_glue_tail_idx(e)],
                             NULL, out, n, cap);
            return;
        }
        case EX_LET:
        case EX_LETREC:
            if (!tco_let_simple(ctx, e)) return;
            tcg_tail_callees(ctx, e->as.let_.body,
                             (e->as.let_.body && e->as.let_.body->kind == EX_DO)
                                 ? e : NULL,
                             out, n, cap);
            return;
        case EX_MATCH:
            if (!tco_match_tail_ok(e)) return;
            for (uint32_t i = 0; i < e->as.match_.n_arms; i++)
                tcg_tail_callees(ctx, e->as.match_.arms[i].body, NULL, out, n, cap);
            return;
        default:
            return;
    }
}

#define TCG_EXPLORE 32

/* The group `e` belongs to, forming it on first sight.  NULL when `e` is not
 * in a strongly-connected component of two or more eligible members.  The
 * component is a property of the graph, not of which member asked first, so
 * every member finds the same group; members are kept in item order, which is
 * also the order their wrappers are emitted in. */
static TcgGroup *tcg_group_of(EmitCtx *ctx, const Expr *e, int *idx) {
    FnDef *fd = e->as.fn_def_.fn;
    for (int i = 0; i < g_n_tcg_groups; i++)
        for (int j = 0; j < g_tcg_groups[i].n_mem; j++)
            if (g_tcg_groups[i].mem[j] == fd) { *idx = j; return &g_tcg_groups[i]; }
    if (g_n_tcg_groups >= 64) return NULL;
    if (tcg_expr_of(ctx, fd) != e) return NULL;
    char *rc = tcg_member_ok(ctx, e);
    if (!rc) return NULL;

    const Expr *node[TCG_EXPLORE];
    bool adj[TCG_EXPLORE][TCG_EXPLORE];
    memset(adj, 0, sizeof adj);
    int nn = 0;
    node[nn++] = e;
    for (int i = 0; i < nn; i++) {
        FnDef *cs[TCG_EXPLORE];
        int nc = 0;
        tcg_tail_callees(ctx, node[i]->as.fn_def_.fn->body, NULL, cs, &nc,
                         TCG_EXPLORE);
        for (int c = 0; c < nc; c++) {
            const Expr *ce = tcg_expr_of(ctx, cs[c]);
            if (!ce || ce == node[i]) continue;
            int j = -1;
            for (int k = 0; k < nn; k++) if (node[k] == ce) j = k;
            if (j < 0) {
                bool grouped = false;
                for (int gi = 0; gi < g_n_tcg_groups && !grouped; gi++)
                    for (int gm = 0; gm < g_tcg_groups[gi].n_mem; gm++)
                        if (g_tcg_groups[gi].mem[gm] == cs[c]) grouped = true;
                if (grouped) continue;
                char *crc = tcg_member_ok(ctx, ce);
                bool same = crc && strcmp(crc, rc) == 0;
                free(crc);
                if (!same) continue;
                if (nn >= TCG_EXPLORE) { free(rc); return NULL; }
                j = nn;
                node[nn++] = ce;
            }
            adj[i][j] = true;
        }
    }
    /* Every node was reached FROM `e`; the component is the ones that also
     * reach back TO it. */
    bool back[TCG_EXPLORE] = { false };
    back[0] = true;
    for (bool changed = true; changed; ) {
        changed = false;
        for (int i = 0; i < nn; i++) {
            if (back[i]) continue;
            for (int j = 0; j < nn; j++)
                if (adj[i][j] && back[j]) { back[i] = true; changed = true; break; }
        }
    }
    int pos[TCG_EXPLORE], nm = 0;
    const Expr *mem[TCG_EXPLORE];
    for (int i = 0; i < nn; i++) if (back[i]) mem[nm++] = node[i];
    if (nm < 2 || nm > TCG_MAXMEM) { free(rc); return NULL; }
    for (int i = 0; i < nm; i++) pos[i] = tcg_item_pos(ctx, mem[i]);
    /* item order: a stable insertion sort on the item position */
    for (int i = 1; i < nm; i++)
        for (int j = i; j > 0 && pos[j] < pos[j - 1]; j--) {
            int tp = pos[j]; pos[j] = pos[j - 1]; pos[j - 1] = tp;
            const Expr *te = mem[j]; mem[j] = mem[j - 1]; mem[j - 1] = te;
        }
    uint32_t n_slots = 0;
    for (int i = 0; i < nm; i++) n_slots += mem[i]->as.fn_def_.fn->n_params;
    if (n_slots > TCG_MAXSLOTS) { free(rc); return NULL; }

    TcgGroup *g = &g_tcg_groups[g_n_tcg_groups++];
    memset(g, 0, sizeof *g);
    g->n_mem = nm;
    g->last_mem = nm - 1;
    g->ret_ctype = rc;
    snprintf(g->name, sizeof g->name, "__tcg_group_%d", g_tcg_ctr++);
    for (int i = 0; i < nm; i++) {
        g->mem_e[i] = mem[i];
        g->mem[i] = mem[i]->as.fn_def_.fn;
        g->slot_base[i] = g->n_slots;
        for (uint32_t k = 0; k < g->mem[i]->n_params; k++)
            g->slot_ctype[g->n_slots++] = strdup(emit_type_c_name(ctx,
                tco_param_type(ctx, mem[i], g->mem[i], (uint8_t)k)));
        if (g->mem[i] == fd) *idx = i;
    }
    return g;
}

static void tcg_emit_signature(Buf *out, const TcgGroup *g, bool named) {
    buf_printf(out, "static %s %s(int%s", g->ret_ctype, g->name,
               named ? " __tcg_st" : "");
    for (uint32_t k = 0; k < g->n_slots; k++) {
        if (named) buf_printf(out, ", %s __tcg_s%u", g->slot_ctype[k], (unsigned)k);
        else       buf_printf(out, ", %s", g->slot_ctype[k]);
    }
    buf_puts(out, ")");
}

/* The fused function.  Written at the LAST member's position in the item
 * stream, so every global any member's body names has already been declared
 * (each member is only ever emitted after the globals it reads); the earlier
 * members' wrappers reach it through the prototype written at the first. */
static void tcg_emit_def(EmitCtx *ctx, Buf *file, TcgGroup *g) {
    Buf F; buf_init(&F);
    Buf *saved_file = ctx->file;
    int saved_indent = ctx->indent;
    const char *saved_ret = ctx->current_fn_ret_ctype;
    Binding **saved_params = ctx->fn_params;
    uint32_t saved_n_params = ctx->n_fn_params;
    const Binding **saved_ic_locals = ctx->inline_c_raw_locals;
    uint32_t saved_n_ic_locals = ctx->n_inline_c_raw_locals;
    uint32_t saved_n_pbp = ctx->n_pbp_params;
    bool saved_no_unwind = ctx->no_unwind;
    struct Closure *saved_closure = ctx->closure;
    const char *saved_env = ctx->env_var_name;
    const char *saved_frame = ctx->frame_var;
    const TcgCur *saved_cur = ctx->tcg_cur;
    const char *saved_tb = ctx->tb_guard;
    const char *saved_mt = ctx->mt_fn_cname;

    ctx->file = &F;
    ctx->closure = NULL;
    ctx->env_var_name = NULL;
    ctx->frame_var = NULL;
    ctx->n_pbp_params = 0;

    tcg_emit_signature(&F, g, true);
    buf_puts(&F, " {\n");
    buf_puts(&F, "    __tcg_top:;\n");
    buf_puts(&F, "    switch (__tcg_st) {\n");
    for (int m = 0; m < g->n_mem; m++) {
        const Expr *e = g->mem_e[m];
        FnDef *fd = g->mem[m];
        buf_printf(&F, "    case %d: {\n", m);
        for (uint32_t k = 0; k < fd->n_params; k++) {
            char *pn = raw_name_for_binding(fd->params[k]);
            buf_printf(&F, "        %s %s = __tcg_s%u;\n", g->slot_ctype[g->slot_base[m] + k],
                       pn, (unsigned)(g->slot_base[m] + k));
            buf_printf(&F, "        (void)%s;\n", pn);
            free(pn);
        }
        TcgCur cur = { g, m };
        ctx->tcg_cur = &cur;
        ctx->tb_guard = NULL;   /* T6: a fused body has no `__tb_may` */
        ctx->mt_fn_cname = NULL; /* T2b: nor a signature of its own on record */
        ctx->current_fn_ret_ctype = g->ret_ctype;
        ctx->fn_params = fd->params;
        ctx->n_fn_params = fd->n_params;
        ctx->no_unwind = fd->binding->no_unwind;
        ctx->inline_c_raw_locals = NULL;
        ctx->n_inline_c_raw_locals = 0;
        emit_inline_c_raw_locals_collect(fd->body, fd->params, fd->n_params,
                                         &ctx->inline_c_raw_locals,
                                         &ctx->n_inline_c_raw_locals);
        rc_elision_analyze_fn(fd->body);
        ctx->indent = 8;
        emit_line_reset(ctx);
        emit_line_directive(ctx, &F, e->span);
        char *cn = raw_name_for_binding(fd->binding);
        int n_ok = 0;
        tco_mark(ctx, fd, cn, fd->body, &n_ok);
        free(cn);
        emit_tail(ctx, &F, e, fd, fd->body, e->type.as.fn.result_kind, false);
        free((void *)ctx->inline_c_raw_locals);
        buf_puts(&F, "    }\n");
    }
    char *z = emit_c_zero_of(g->ret_ctype);
    buf_printf(&F, "    default: break;\n    }\n    return %s;\n}\n\n", z ? z : "0");
    free(z);

    ctx->file = saved_file;
    ctx->indent = saved_indent;
    ctx->current_fn_ret_ctype = saved_ret;
    ctx->fn_params = saved_params;
    ctx->n_fn_params = saved_n_params;
    ctx->inline_c_raw_locals = saved_ic_locals;
    ctx->n_inline_c_raw_locals = saved_n_ic_locals;
    ctx->n_pbp_params = saved_n_pbp;
    ctx->no_unwind = saved_no_unwind;
    ctx->closure = saved_closure;
    ctx->env_var_name = saved_env;
    ctx->frame_var = saved_frame;
    ctx->tcg_cur = saved_cur;
    ctx->tb_guard = saved_tb;
    ctx->mt_fn_cname = saved_mt;

    /* Anything the bodies lifted to file scope (handler functions) has to
     * precede the fused function that references it. */
    if (ctx->pending_handler_fns && ctx->pending_handler_fns->len > 0) {
        buf_write(file, ctx->pending_handler_fns->data, ctx->pending_handler_fns->len);
        buf_free(ctx->pending_handler_fns);
        buf_init(ctx->pending_handler_fns);
    }
    buf_write(file, F.data, F.len);
    buf_free(&F);
}

void emit_fn_def(EmitCtx *ctx, Buf *file, const Expr *e) {
    FnDef *fd = e->as.fn_def_.fn;
    /* forall-dict-pass-nested-lambda-dispatch-plan (Phase 2): a mapper's dead
     * poly-wrapper, orphaned when the mapper became a dict-capturing closure. */
    if (fd && fd->skip_emission) return;
    /* cps-ir-to-c-backend-plan (C1): a colored function whose CTerm lies in the
     * emittable subset is lowered through the DK-threading CPS backend (always
     * on).  Anything outside the subset returns false and keeps its direct-style
     * emission below. */
    if (emit_cps_ir_try_fn(ctx, file, e)) {
        /* proper-tail-calls T1 (T-D1): this body never reaches tco_mark, so a
         * `^tailcall` in it would otherwise go silently unchecked.  The CPS
         * backend emits every call as an ordinary C call, a panic check, and a
         * continuation invocation -- Appendix A.6 of the plan measures exactly
         * this -- so nothing in a CPS-lowered body is in tail position. */
        if (fd) tc_verify_fn(ctx, fd, NULL, TC_CPS_BODY);
        return;
    }
    /* proper-tail-calls T5 (T-D5): a member of a mutual tail-call group is
     * emitted as a wrapper around the group's fused function.  The first
     * member to arrive writes the fused function's prototype; the last (in
     * item order) writes its definition.  `tcg_cur` stays set for the whole
     * emission so tco_mark marks the group's tail calls and the `^tailcall`
     * verifier accepts them. */
    const TcgCur *saved_tcg_cur = ctx->tcg_cur;
    TcgCur tcg_cur_v = { NULL, -1 };
    const TcgGroup *tcg = NULL;
    if (fd && !ctx->tcg_cur && !ctx->current_abi_specialization &&
        !ctx->fn_name_override && ctx->n_tcg_fn_exprs) {
        int ti = -1;
        TcgGroup *g = tcg_group_of(ctx, e, &ti);
        if (g && ti >= 0) {
            if (!g->declared) {
                tcg_emit_signature(file, g, false);
                buf_puts(file, ";\n");
                g->declared = true;
            }
            if (!g->defined && g->mem_e[g->last_mem] == e) {
                tcg_emit_def(ctx, file, g);
                g->defined = true;
            }
            tcg = g;
            tcg_cur_v.g = g;
            tcg_cur_v.idx = ti;
            ctx->tcg_cur = &tcg_cur_v;
        }
    }
    /* MB1 (constrained-hkt-forall-mode-b-plan): while this dict-clone's body is
     * emitted, route its class-method calls on the constrained var through the
     * dict param (emit_call_name).  emit_fn_def is single-exit (no early
     * returns) and never recurses into another emit_fn_def, so set here and
     * clear at the end. */
    const char   *saved_dd_cname = ctx->dict_dispatch_param_cname;
    TypeClass    *saved_dd_class = ctx->dict_dispatch_class;
    uint8_t       saved_dd_n = ctx->dict_dispatch_n;
    TypeClass    *saved_dd_classes[MAX_FN_CONSTRAINTS];
    const char   *saved_dd_cnames[MAX_FN_CONSTRAINTS];
    memcpy(saved_dd_classes, ctx->dict_dispatch_classes, sizeof saved_dd_classes);
    memcpy(saved_dd_cnames, ctx->dict_dispatch_param_cnames, sizeof saved_dd_cnames);
    struct Binding *saved_dd_params[MAX_FN_CONSTRAINTS];
    memcpy(saved_dd_params, ctx->dict_dispatch_params, sizeof saved_dd_params);
    char         *dd_cnames_owned[MAX_FN_CONSTRAINTS] = {0};
    if (fd->n_dict_clone > 0) {
        /* forall-dict-pass-multi-constraint-hkt-plan (Task 1.4): install the full
         * (class, dict-param) vector so each class-method call dispatches through
         * the dict for its own class.  The scalar pair mirrors slot 0 for the
         * ambient-dict lowering paths that predate the vector. */
        ctx->dict_dispatch_n = fd->n_dict_clone;
        for (uint8_t k = 0; k < fd->n_dict_clone; k++) {
            dd_cnames_owned[k] = raw_name_for_binding(fd->dict_clone_params[k]);
            ctx->dict_dispatch_param_cnames[k] = dd_cnames_owned[k];
            ctx->dict_dispatch_params[k] = fd->dict_clone_params[k];
            ctx->dict_dispatch_classes[k] = fd->dict_clone_classes[k];
        }
        ctx->dict_dispatch_param_cname = dd_cnames_owned[0];
        ctx->dict_dispatch_class = fd->dict_clone_classes[0];
    }
    /* forall-dict-pass-nested-lambda-dispatch-plan (Phase 3): while emitting a
     * dict-capturing mapper closure, install its (class, captured-dict-binding)
     * so a class-method call on the constraint var dispatches through the
     * env-loaded dict.  Saved/restored like the dict_dispatch pair above. */
    uint8_t    saved_de_n = ctx->cur_dict_env_n;
    TypeClass *saved_de_classes[MAX_FN_CONSTRAINTS];
    Binding   *saved_de_bindings[MAX_FN_CONSTRAINTS];
    memcpy(saved_de_classes, ctx->cur_dict_env_classes, sizeof saved_de_classes);
    memcpy(saved_de_bindings, ctx->cur_dict_env_bindings, sizeof saved_de_bindings);
    ctx->cur_dict_env_n = fd->n_dict_env;
    for (uint8_t k = 0; k < fd->n_dict_env; k++) {
        ctx->cur_dict_env_classes[k] = fd->dict_env_classes[k];
        ctx->cur_dict_env_bindings[k] = fd->dict_env_bindings[k];
    }
    /* SYM5: detect the opt-in str->sym definition (from sym-dynamic.tur).  Its
     * presence is what links the runtime intern table, so it gates the
     * static-record seeding constructor emitted by sym_codegen_emit. */
    if (fd->binding && fd->binding->name && fd->binding->name->name &&
        strcmp(fd->binding->name->name, "str->sym") == 0) {
        sym_codegen_note_intern_used();
    }
    bool use_abi_spec = ctx->current_abi_specialization &&
        ctx->current_abi_specialization->fn == fd;

    const char *current_fn_ret_ctype_eff = NULL;
    bool is_main_check = fd->binding && fd->binding->name && fd->binding->name->name &&
        strcmp(fd->binding->name->name, "main") == 0;
    if (e->type.kind == TY_FN && !is_main_check) {
        /* structdef-retirement DS-C: emit_carrier_return_override is dead (a
         * method body is never TY_STRUCT), so the `carrier_override.kind ==
         * TY_STRUCT` branch that used to sit here is removed. */
        if (fd->box_aggregate_result) {
            /* WF1/WF2 (van-laarhoven-wide-functor-carrier-plan): this functor-
             * wrapping closure `g` returns its wide `(f A)` aggregate boxed into
             * the int64 carrier (the box the lens's poly-carrier boundary
             * unboxes), so the generic dict-clone's int64-returning fat-dispatch
             * of `g` reads a valid carrier word. */
            current_fn_ret_ctype_eff = "int64_t";
        } else if (fd->n_dict_clone > 0) {
            /* MB2.5 (constrained-hkt-forall-mode-b-plan): a dict-clone wrapper
             * dispatches through the carrier dict and lives behind the poly
             * carrier, so its result is ALWAYS the int64 carrier -- even when the
             * class-var-applied result `(f a)` resolves to a by-value aggregate
             * (`Maybe int`).  The aggregate box/unbox happens at the caller
             * (poly-carrier) boundary.  Carrier-compatible functors (Box) already
             * land on int64 via result_kind, so this is inert for them and only
             * corrects the by-value aggregate case. */
            current_fn_ret_ctype_eff = "int64_t";
        } else if (use_abi_spec) {
            Type rt = ctx->current_abi_specialization->result_type;
            bool is_inst = fd->binding && fd->binding->name &&
                fd->binding->name->name &&
                strncmp(fd->binding->name->name, "__inst_", 7) == 0;
            Type rt_resolved = emit_resolve_type(ctx, rt);
            if (is_inst &&
                rt_resolved.kind == TY_APP &&
                !type_is_heap_struct(rt_resolved) &&
                type_has_concrete_codegen_layout(&rt_resolved)) {
                current_fn_ret_ctype_eff = emit_type_c_name(ctx, rt);
            } else if (is_inst && type_uses_carrier_abi(rt_resolved)
                && ctx->current_abi_specialization->typeclass_inst == NULL) {
                current_fn_ret_ctype_eff = "int64_t";
            } else {
                current_fn_ret_ctype_eff = emit_type_c_name(ctx, rt);
            }
        } else if (e->type.as.fn.result_full_type &&
                   fd->binding && fd->binding->name && fd->binding->name->name &&
                   strncmp(fd->binding->name->name, "__inst_", 7) == 0 &&
                   type_uses_carrier_abi(emit_resolve_type(ctx,
                       *e->type.as.fn.result_full_type))) {
            current_fn_ret_ctype_eff = "int64_t";
        } else if (e->type.as.fn.result_full_type &&
                   emit_inst_fn_return_carrier(fd,
                       e->type.as.fn.result_full_type)) {
            current_fn_ret_ctype_eff = emit_inst_fn_return_carrier(fd,
                e->type.as.fn.result_full_type);
        } else if (e->type.as.fn.result_full_type) {
            Type rft = *e->type.as.fn.result_full_type;
            const char *fn_ret_td = e->type.as.fn.result_fat
                ? NULL : emit_fn_return_typedef(fd, &rft);
            current_fn_ret_ctype_eff = fn_ret_td ? fn_ret_td : emit_type_c_name(ctx, rft);
        } else if (fd->binding && fd->binding->name && fd->binding->name->name &&
                   strncmp(fd->binding->name->name, "__inst_", 7) == 0 &&
                   fd->body && (fd->body->type.kind == TY_APP ||
                                fd->body->type.kind == TY_STRUCT ||
                                type_uses_carrier_abi(fd->body->type))) {
            /* repro 2 (see the signature site): a by-value aggregate body
             * with no result_full_type is spilled to the carrier at the tail. */
            const char *_body_c2 = emit_type_c_name(ctx, fd->body->type);
            if (_body_c2 && strcmp(_body_c2, "int64_t") != 0) {
                current_fn_ret_ctype_eff = "int64_t";
            } else {
                current_fn_ret_ctype_eff = emit_type_c_name(ctx,
                    emit_type_from_kind(e->type.as.fn.result_kind));
            }
        } else {
            current_fn_ret_ctype_eff = emit_type_c_name(ctx,
                emit_type_from_kind(e->type.as.fn.result_kind));
        }
    }
    const char *saved_current_fn_ret_ctype = ctx->current_fn_ret_ctype;
    ctx->current_fn_ret_ctype = current_fn_ret_ctype_eff;
    const struct FnDef *saved_own_cur_fn = ctx->own_cur_fn;
    ctx->own_cur_fn = fd;

    /* Use raw name (without ID suffix) for function name */
    const char *fn_name = ctx->fn_name_override
        ? ctx->fn_name_override
        : raw_name_for_binding(fd->binding);

    /* Phase 19: Drain any pending effect handler functions accumulated while
     * elaborating the PREVIOUS top-level expression.  They must appear before
     * this function definition so they are at file scope. */
    if (ctx->pending_handler_fns && ctx->pending_handler_fns->len > 0) {
        buf_write(file, ctx->pending_handler_fns->data, ctx->pending_handler_fns->len);
        buf_free(ctx->pending_handler_fns);
        buf_init(ctx->pending_handler_fns);
    }


    /* Phase 19 (per-fiber): Route THIS function's body through a temp buffer so
     * that any handler functions accumulated during body emission are flushed to
     * file-scope BEFORE this function's definition. Without this, a handle
     * expression inside a non-final function generates a forward reference: the
     * function body is written to `file` first, then the handler appears after
     * the next function drains the pending buffer.
     *
     * Steps:
     *   1. Redirect ctx->file to a temp buf
     *   2. Emit the function definition into the temp buf
     *   3. Drain any newly-added handlers to the real file (file scope, before def)
     *   4. Append the temp buf to the real file
     */
    Buf fn_tmp; buf_init(&fn_tmp);
    Buf *real_file = file;
    ctx->file = &fn_tmp;
    file = &fn_tmp;

    /* poly-closure-result-specialization: an inner-closure-body spec emits its
     * env struct + cast under a suffixed env name so the float layout does not
     * collide with the base int64-carrier struct.  NULL/identical for ordinary
     * specs and non-spec closures. */
    const Symbol *env_name_eff = fd->closure ? fd->closure->env_name : NULL;
    if (use_abi_spec && ctx->current_abi_specialization->env_name_override)
        env_name_eff = ctx->current_abi_specialization->env_name_override;

    /* Phase 3: Emit env struct for closure thunks */
    if (fd->closure) {
        const Symbol *env_name = env_name_eff;
        /* Check if we've already emitted this env struct */
        bool already_emitted = false;
        if (ctx->env_struct_names) {
            for (uint32_t i = 0; i < ctx->n_env_struct_names; i++) {
                if (ctx->env_struct_names[i] == env_name) {
                    already_emitted = true;
                    break;
                }
            }
        }
        if (!already_emitted) {
            Type thunk_result = (e->type.kind == TY_FN && e->type.as.fn.result_full_type)
                ? *e->type.as.fn.result_full_type
                : emit_type_from_kind(e->type.kind == TY_FN ? e->type.as.fn.result_kind : TY_NIL);
            /* poly-closure-result-specialization: resolve the inner result tyvar
             * to its concrete (float) type so the typed thunk slot is xmm0-correct. */
            thunk_result = emit_resolve_type(ctx, thunk_result);
            Type *thunk_params = fd->n_params > 1 ? &fd->param_types[1] : NULL;
            if (thunk_params && use_abi_spec) {
                uint32_t tp_n = (uint32_t)(fd->n_params - 1);
                Type *resolved_thunk_params = (Type *)arena_alloc(ctx->type_arena, (tp_n ? tp_n : 1) * sizeof(Type));
                for (uint32_t _t = 0; _t < tp_n; _t++)
                    resolved_thunk_params[_t] = emit_resolve_type(ctx, fd->param_types[_t + 1]);
                thunk_params = resolved_thunk_params;
            }
            uint32_t thunk_arity = fd->n_params > 0 ? (uint32_t)(fd->n_params - 1) : 0;
            char *thunk_typedef = ensure_typed_thunk_typedef(ctx, file, thunk_result, thunk_params, thunk_arity);
            emit_env_struct_register(ctx, env_name, thunk_typedef);


            /* Phase HKT §5: fat closure layout — __fn (int64_t) first, then
             * captures.  The fat pointer doubles as the env ptr for the thunk:
             * thunk receives fat_ptr as self, accesses captures via ->field
             * (which are at the correct offsets after __fn). */
            if (thunk_typedef) {
                buf_printf(file, "struct %s { %s __fn; ", env_name->name, thunk_typedef);
            } else {
                buf_printf(file, "struct %s { int64_t __fn; ", env_name->name);
            }
            Buf capty; buf_init(&capty);
            for (uint8_t i = 0; i < fd->closure->n_captures; i++) {
                Binding *captured = fd->closure->captures[i];
                if (i > 0) buf_putc(&capty, '\n');
                /* CY4: rank-2 captures must live as tur_poly_fn_t. */
                /* A captured fn value (fat closure or bare fn ptr) is carried as
                 * the int64_t fn-ABI carrier -- the same C type fn-typed
                 * parameters use.  type_c_name(TY_FN) returns the *result*
                 * type's C name (e.g. "double" for a :float-returning closure),
                 * which would alias the closure pointer through a floating-point
                 * field and reinterpret it via a double on read -- a latent
                 * miscompile that only survives because valid pointers fit
                 * exactly in a double's 53-bit integer range. */
                const char *cap_ctype = captured->is_poly_fn
                                          ? "tur_poly_fn_t"
                                          : (captured->type.kind == TY_FN
                                               ? "int64_t"
                                               : emit_type_c_name(ctx, captured->type));
                char *field = raw_name_for_binding(captured);
                buf_printf(file, "%s %s; ", cap_ctype, field);
                buf_puts(&capty, cap_ctype);
                free(field);
            }
            buf_puts(file, "};\n");
            buf_putc(&capty, '\0');
            emit_env_struct_set_cap_ctypes(ctx, env_name, capty.data ? capty.data : "");
            buf_free(&capty);
            free(thunk_typedef);

            /* closure-drop-glue (Model R): emit the env's drop-glue beside its
             * struct def (this pre-pass is the authoritative early emission site;
             * the EX_CLOSURE path shares the env_struct_names guard, so the glue is
             * emitted exactly once).  The glue RELEASES each refcounted owning
             * capture (rc strong decrement, balancing the env-fill's retain) and
             * frees the base allocation (the header word precedes the env pointer).
             *
             * The rc walk is sound regardless of aliasing -- rc counting is the
             * sharing-safe owning mechanism.  Owning captures that are NOT
             * refcounted -- a raw nested-closure handle (`!is_poly_fn` TY_FN), a
             * `ref` -- are NOT walked here: recursing one blind is finding #1's
             * double-free (a capture another owner also drops), which needs
             * move/uniqueness analysis.  The httpd `_n`/string case additionally
             * needs type-honest owned captures.  Both are the next slice. */
            {
                buf_printf(file, "static void drop_glue_%s(void *__p) {\n", env_name->name);
                buf_printf(file, "    struct %s *__e = (struct %s *)__p; (void)__e;\n",
                           env_name->name, env_name->name);
                for (int32_t i = (int32_t)fd->closure->n_captures - 1; i >= 0; i--) {
                    Binding *cap = fd->closure->captures[i];
                    if (cap && cap->is_global) continue;
                    if (cap && cap->type.kind == TY_RC) {
                        char *cf = raw_name_for_binding(cap);
                        buf_printf(file,
                            "    if (__e->%s) { rc_strong_decrement(__e->%s); rc_free_queue_drain(); }\n",
                            cf, cf);
                        free(cf);
                    } else if (cap && cap->is_fat && !fd->closure->fat_captures_borrowed &&
                               !(cap->closure_fn_binding &&
                                 fd->binding &&
                                 cap->closure_fn_binding == fd->binding)) {
                        /* Type-honesty (a): a `^fat` capture carries is_fat even
                         * though its env field is the erased int64 carrier, so it
                         * is provably an OWNED fat closure handle -- release it via
                         * TUR_CLOSURE_DROP (uniform across representations).  Sound
                         * because the capture is MOVED into this env (elab marks the
                         * source consumed, so no other owner drops it -- an aliasing
                         * second capture is a use-after-consume error, not a
                         * double-free).  The letrec self-capture (the env storing
                         * its OWN pointer) is excluded -- dropping it would recurse
                         * into itself. */
                        char *cf = raw_name_for_binding(cap);
                        buf_printf(file, "    TUR_CLOSURE_DROP(__e->%s);\n", cf);
                        free(cf);
                    } else if (fd->closure->capture_drop_insts &&
                               fd->closure->capture_drop_insts[i] &&
                               fd->closure->capture_drop_insts[i]->n_method_impls > 0 &&
                               fd->closure->capture_drop_insts[i]->method_impls[0] &&
                               fd->closure->capture_drop_insts[i]->method_impls[0]->binding) {
                        /* Model R #1b: a capture whose type implements Drop -- release
                         * it through the resolved Drop instance's `drop` method.  A
                         * Drop+Clone (refcounted) capture was retained at env-fill, so
                         * this decrement balances; a move-only Drop capture was
                         * consumed at capture, so this is its sole release. */
                        char *cf = raw_name_for_binding(cap);
                        char *dm = raw_name_for_binding(
                            fd->closure->capture_drop_insts[i]->method_impls[0]->binding);
                        buf_printf(file, "    %s(__e->%s);\n", dm, cf);
                        free(dm); free(cf);
                    }
                }
                buf_puts(file, "    free((void *)((char *)__p - sizeof(void *)));\n}\n");
            }
        }
    }

    /* Emit function signature */
    /* Special case: C's main() must return int, not int64_t.
     * fn_name is already the mangled name; main is never module-prefixed. */
    bool is_main = (strcmp(fn_name, "main") == 0);
    /* Phase M3: In separate compilation mode, exported functions get extern
     * linkage (no static) so they can be called from other compilation units.
     * J3: ABI-specialization clone bodies emitted with external_linkage also
     * drop 'static' so they are visible to borrower TUs at link time. */
    /* spice-defn-return-result-kind-mismatch: stdlib defns preloaded into
     * every project-mode TU stay `static` -- otherwise every TU emits an
     * external copy of e.g. `ok-val` and the linker rejects the duplicates. */
    /* #[used]: a defn reachable only via its mangled C symbol (cross-module
     * inline-C bridge or by-address C-ABI callback) must keep external linkage
     * so the raw `extern <mangled>` reference in another TU resolves. */
    /* definstance-not-dispatchable-across-modules: an instance METHOD is never
     * `is_exported` -- a `definstance` has no export list and the module's
     * `(export ...)` names defns, not methods -- so it was emitted `static` and
     * left out of the module header.  Dispatching on that instance from another
     * module then elaborated clean (the mangled name resolves) and died in cc
     * with `call to undeclared function '__inst_<Class>_<method>_<Type>'`.
     *
     * A user module's instance is defined in exactly one TU, so external
     * linkage introduces no duplicate symbol.  Stdlib instances are excluded by
     * the `is_from_stdlib` test that already guards this branch: those ARE
     * preloaded into every project-mode TU and must stay static. */
    bool user_inst_method = emit_inst_method_wants_external(fd);
    /* r7rs-programs-compile-slowly: the library unit of a split build gives
     * every stdlib defn external linkage for the client unit to call (its ABI
     * clones, under fn_name_override, stay file-local like any other). */
    bool needs_static = !is_main &&
        !ctx->fn_name_override_external &&
        !(ctx->separate_compilation
          && (fd->binding->is_exported || fd->binding->retain_c_linkage
              || user_inst_method)
          && !fd->binding->is_from_stdlib) &&
        !(emit_split_lib_owns(fd->binding) && !ctx->fn_name_override);
    /* A HAND-WRITTEN body is recorded for the emitted-C checks
     * (tests/check-emitted-float-conversions.py): they police what the
     * EMITTER decides, and a conversion an inline-C author wrote on purpose
     * -- `return (double)n;` in int->float -- is reviewed as code, not
     * inferred.  emit_program writes the list as one trailing comment. */
    if (fd->body && fd->body->kind == EX_INLINE_C)
        emit_note_inline_c_fn(fn_name);
    if (needs_static) {
        buf_printf(file, "static ");
    }
    /* Return type from fn_type */
    if (e->type.kind == TY_FN) {
        TypeKind result = e->type.as.fn.result_kind;
        /* structdef-retirement DS-C: the RT/SC5 carrier-return-override bridge
         * (emit_carrier_return_override) is dead -- a method body is never
         * TY_STRUCT -- so its assignment and the `carrier_override.kind ==
         * TY_STRUCT` branch below are removed. */
        if (is_main) {
            buf_puts(file, "int");  /* C main must always return int */
        } else if (fd->box_aggregate_result) {
            /* WF1/WF2 (van-laarhoven-wide-functor-carrier-plan): a functor-
             * wrapping closure `g` boxes its wide `(f A)` aggregate into the int64
             * carrier (keep in lockstep with current_fn_ret_ctype + the body-side
             * return-boxing below). */
            buf_puts(file, "int64_t");
        } else if (fd->n_dict_clone > 0) {
            /* MB2.5 (constrained-hkt-forall-mode-b-plan): a dict-clone wrapper
             * returns the int64 carrier (see the body-side + current_fn_ret_ctype
             * overrides and the forward decl in emit_module.c -- keep all four in
             * lockstep).  A by-value aggregate functor's `(f a)` result otherwise
             * resolves the header to the aggregate C type while the body returns
             * the carrier, a return-type cc error. */
            buf_puts(file, "int64_t");
        } else if (use_abi_spec) {
            /* Direction (1) of the open report: a typeclass instance
             * method (__inst_*) whose return type uses the carrier ABI
             * (parameterized struct like (Result A B)) emits int64_t
             * return -- the dispatch dict expects a uniform
             * `int64_t (*)(int64_t, ...)` shape, and the body's
             * by-value Result__T__B struct gets heap-spilled to a
             * pointer-as-int64 at the body emit.
             *
             * M4c Path A result-side: per-instantiation specs on non-HKT
             * classes (spec->typeclass_inst set) bypass the dispatch dict
             * — Path A's call site invokes the spec directly with no
             * carrier indirection.  Skip the int64_t override for those so
             * the spec's signature matches its return value and the
             * caller's bridge becomes unnecessary.  HKT-class specs keep
             * the carrier override per the M6/M7 carve-out. */
            Type rt = ctx->current_abi_specialization->result_type;
            bool is_instance_method =
                fd->binding && fd->binding->name && fd->binding->name->name &&
                strncmp(fd->binding->name->name, "__inst_", 7) == 0;
            Type rt_resolved = emit_resolve_type(ctx, rt);
            /* M7 layer-4: a per-(f, A) by-value HKT instance-method spec
             * returns the resolved struct by value (keep in sync with the
             * body-side return-type branch and the forward decl in
             * emit_module.c). */
            if (is_instance_method &&
                rt_resolved.kind == TY_APP &&
                !type_is_heap_struct(rt_resolved) &&
                type_has_concrete_codegen_layout(&rt_resolved)) {
                buf_puts(file, emit_type_c_name(ctx, rt));
            } else if (is_instance_method &&
                type_uses_carrier_abi(rt_resolved)
                && ctx->current_abi_specialization->typeclass_inst == NULL) {
                buf_puts(file, "int64_t");
            } else {
                buf_puts(file, emit_type_c_name(ctx, rt));
            }
        } else if (!is_main &&
                   emit_inst_result_rides_carrier(ctx, fd, e->type.as.fn.result_full_type)) {
            /* Direction (1) of polymorphic-ok-in-typeclass-instance-method-...md:
             * non-spec instance method whose declared return is a carrier-ABI
             * parameterized struct (e.g. (Result T E)).  The dispatch dict's
             * uniform `int64_t (*)(...)` signature requires an int64 return; the
             * body's by-value struct gets heap-spilled at the return-emit below. */
            buf_puts(file, "int64_t");
        } else if (!is_main && e->type.as.fn.result_full_type &&
                   emit_inst_fn_return_carrier(fd, e->type.as.fn.result_full_type)) {
            /* instance-method-closure-return: a typeclass-method impl whose
             * declared return is a function value uses the thin fn-ptr typedef
             * (non-capturing) or the int64_t closure carrier (fat box) -- never
             * the function's result type.  Must agree with the dict field type
             * in emit_stmt.c and the forward decl in emit_module.c. */
            buf_puts(file, emit_inst_fn_return_carrier(fd, e->type.as.fn.result_full_type));
        } else if (e->type.as.fn.result_full_type) {
            bool body_is_inline_c = (fd->body && fd->body->kind == EX_INLINE_C);
            Type rft = *e->type.as.fn.result_full_type;
            /* fn-typed-return: a declared function-value return lowers to its
             * matching fn-ptr typedef (not the function's result type).  Skipped
             * for ^fat returns, which are wrapped into a void* heap fat box. */
            const char *fn_ret_td = e->type.as.fn.result_fat
                ? NULL : emit_fn_return_typedef(fd, &rft);
            /* ptr-generic-parameterised-type: a typed ptr<T> return lowers to
             * `T *` even from an inline-C body, so the C body's `return ptr;`
             * type-checks against the declared pointer type with no cast. */
            bool typed_ptr = (rft.kind == TY_PTR_VOID && rft.as.ptr.inner);
            /* inline-c-struct-return: a by-value struct return (e.g. the linear
             * FileHandle from io's file-open) lowers to the struct's C name even
             * from an inline-C body, so `return fh;` agrees with the declared
             * type.  Carrier-ABI structs are handled by carrier_override above. */
            bool typed_struct = (rft.kind == TY_STRUCT);
            /* typed-c-abi-function-pointers: a cfnptr return lowers to its
             * concrete `R (*)(A...)` typedef even from an inline-C body, so
             * the header prototype (which uses type_c_name) and the .c
             * definition agree -- and so an inline-C body returning the bare
             * function pointer type-checks against the declared return.  */
            bool typed_cfnptr = (rft.kind == TY_FN && rft.as.fn.cfnptr);
            /* end-to-end-monomorphization (bucket A): a `:heap` result on an
             * inline-C producer (e.g. `vec-new : (Vec A)`) lowers to the typed
             * pointer (`Vec__int *`) in an active ABI spec, so the spec binds a
             * typed-pointer local at the call site (no `(Vec__int *)(intptr_t)h`
             * cast at downstream typed consumers).  The carrier base (no active
             * spec) keeps int64.  The body returns via `__TUR_RET__`, which
             * matches: int64_t for the base, the typed pointer for the spec. */
            bool typed_heap_spec = body_is_inline_c && use_abi_spec &&
                                   type_is_heap_struct(rft);
            /* CONV-S1 seam 4 (inline-C instance-method signature): under the
             * defstruct-as-defadt lowering a by-value record/product result
             * (e.g. `Pos` -> `tur_adt_Pos`) is a concrete aggregate, not the
             * int64 carrier -- an inline-C body returns it by value (`Pos r;
             * return r;`), so the C signature must be the aggregate, exactly as
             * the dict slot (emit_stmt.c, via type_c_name(result_full_type))
             * already is.  Mirror `typed_struct` for the ADT representation: a
             * non-:heap ADT/ADT-app whose ABI is by-value (carrier-ABI false)
             * with a concrete codegen layout.  Carrier-ABI ADTs (multi-variant
             * sums returned as the int64 handle) are excluded and stay int64. */
            Type rft_r = emit_resolve_type(ctx, rft);
            bool typed_byval_adt =
                inline_c_returns_byvalue_adt(ctx, body_is_inline_c, &rft);
            /* inline-c-rc-return-misses-carrier-bridge: an owning return
             * (rc/weak/ref/lref) lowers to `RcControlBlock *` even from an
             * inline-C body, for the same reason typed_ptr does -- it IS a
             * pointer, and the carrier holds exactly its bits.  Without this the
             * base came out `int64_t` while every specialized consumer took
             * `RcControlBlock *`, so an ordinary
             *
             *     (chain-cons item (chain-nil))
             *
             * emitted a -Wint-conversion warning in the user's build with
             * nothing in their source to fix.  `return 0;` in the body stays
             * valid (a null pointer constant), which is the shape that matters:
             * a null rc is the one thing an inline-C body is reached for here.  */
            bool typed_rc = body_is_inline_c &&
                (rft.kind == TY_RC || rft.kind == TY_WEAK ||
                 rft.kind == TY_REF || rft.kind == TY_LREF);
            if (fn_ret_td && !body_is_inline_c) {
                buf_puts(file, fn_ret_td);
            } else if (!body_is_inline_c || typed_ptr || typed_struct ||
                       typed_cfnptr || typed_heap_spec || typed_byval_adt ||
                       typed_rc) {
                buf_puts(file, emit_type_c_name(ctx, typed_byval_adt ? rft_r : rft));
            } else {
                buf_puts(file, "int64_t");
                /* The body is C text, and its `__TUR_RET__` must name the type
                 * this signature returns: the carrier, not the typed pointer
                 * the return-type computation above chose for a `:heap` result
                 * (a non-generic `(Map int any)` inline-C function used to
                 * return `(tur_adt_Map__int__any *)` from an int64_t function). */
                if (body_is_inline_c) ctx->current_fn_ret_ctype = "int64_t";
            }
        } else {
            /* SF-application carrier bridge (struct/app closure return):
             * a lifted closure body whose declared return is a typed application
             * (e.g. (Pair float float)) often has result_full_type=NULL on the
             * outer TY_FN type, so the bare result_kind (TY_APP / TY_STRUCT)
             * here would lower to the int64_t carrier and mismatch the body's
             * actual struct return.  When the body's type has the concrete
             * codegen layout (Pair__float__float), prefer it so the C return
             * type matches the value the body produces. */
            const char *_body_c = (fd->body && (fd->body->type.kind == TY_APP
                                                || fd->body->type.kind == TY_STRUCT))
                ? emit_type_c_name(ctx, fd->body->type) : NULL;
            /* Direction (1) of polymorphic-ok-in-typeclass-instance-method-...md:
             * a non-spec instance method whose body codegen produces a
             * by-value carrier-ABI struct (e.g. Result__User__cstr) must
             * return int64_t to honor the dispatch dict's uniform pointer
             * signature; the body return is heap-spilled below.  If the
             * body codegen is already int64_t (TY_APP that lowered to a
             * carrier handle), the existing int64_t path is correct. */
            /* return-dispatched-sum-mint-in-constrained-instance-miscompiles
             * (repro 2): NOT conditioned on type_uses_carrier_abi(body type).
             * A base clone whose declared result is the bare class variable
             * has no result_full_type, and its body constructs the
             * representative monomorph BY VALUE (`(Option int)`, carrier-ABI
             * false), so the conjunct made the header say `tur_adt_Option__int`
             * while the dict slot (emit_stmt.c: result_kind -> int64) and the
             * body's tail (ret_ctype fallback -> int64, heap-spilled) both say
             * the carrier.  Any by-value aggregate body here is spilled at the
             * tail, so the header is int64 whenever the body is one. */
            bool inst_method_struct_body =
                fd->binding && fd->binding->name && fd->binding->name->name &&
                strncmp(fd->binding->name->name, "__inst_", 7) == 0 &&
                _body_c && strcmp(_body_c, "int64_t") != 0;
            /* M5 straddle (root cause C): a lifted lambda with no
             * result_full_type whose body's tail value comes from a
             * carrier-int64 producer (some/ok/err/none or an __inst_ method)
             * emits the int64 carrier -- and it is dispatched through the
             * int64-in/int64-out fat/poly thunk, so its signature must be
             * int64 too, NOT the by-value `_body_c` aggregate.  Without this
             * the signature (Option__int) and the body-return block (which
             * lowers the bare result_kind to int64) disagree -> cc error.  A
             * genuine by-value aggregate body (e.g. (Pair float float) via
             * make-struct) is not a carrier producer and keeps `_body_c`. */
            bool body_is_carrier_producer =
                _body_c && strcmp(_body_c, "int64_t") != 0 &&
                type_uses_carrier_abi(emit_resolve_type(ctx, fd->body->type)) &&
                fn_body_tail_is_carrier_producer(fd->body);
            /* opaque-pointer-c-spelling: a pointer opaque body does NOT override
             * a declared carrier result -- see the helper's comment.  `(defn
             * two-sum [] : int ...)` over a `(Parser int)` body emitted `static
             * void * two_hysum()` whose every `return` was `(int64_t)...`. */
            bool body_is_opaque_ptr =
                emit_fn_body_is_opaque_ptr_over_carrier_result(fd, result);
            if (inst_method_struct_body || body_is_carrier_producer ||
                body_is_opaque_ptr) {
                buf_puts(file, "int64_t");
            } else if (_body_c && strcmp(_body_c, "int64_t") != 0) {
                buf_puts(file, _body_c);
            } else {
                buf_puts(file, emit_type_c_name(ctx, emit_type_from_kind(result)));
            }
        }
    } else {
        buf_puts(file, "void");
    }
    buf_printf(file, " %s(", fn_name);

    /* CLI-ARGS: a user-defined zero-arg `main` must still take (argc, argv)
     * at the C level so g_tur_args can be populated from the process argv.
     * Without this, `*args*` is always empty in user-main programs. */
    bool emit_main_argv = (is_main && fd->n_params == 0);
    if (emit_main_argv) {
        buf_puts(file, "int argc, char **argv");
    }

    /* Emit parameters - use raw names (without ID suffix) */
    bool body_is_inline_c = (fd->body && fd->body->kind == EX_INLINE_C);
    /* Prereq 6: when a polymorphic stdlib constructor's inline-C body
     * (which assumes int64-castable `x`) is monomorphized for a value-
     * struct A, the source `(int64_t)(intptr_t)x` cast on the struct
     * rvalue is invalid C. The boxing-needed shape is three existing
     * predicates: body_is_inline_c + return type uses carrier ABI + the
     * spec arg type is a non-parametric value-struct. When all three
     * hold for param i, we rename its C identifier to `__tur_inbox_<orig>`
     * here and emit a heap-spill stanza after the param list closer so
     * `<orig>` shadows the parameter as a `T *` heap pointer -- the
     * inline-C body's `(int64_t)(intptr_t)<orig>` cast then operates on
     * a pointer and produces the int64 carrier the call site expects.
     * No source-level annotation: the recognizer is purely structural.
     * See docs/archive/history/polymorphic-ok-fails-for-value-struct-payload.md. */
    uint32_t nbp = fd->n_params ? fd->n_params : 1;
    bool *needs_box_spill = (bool *)arena_alloc(ctx->type_arena, nbp * sizeof(bool));
    for (uint32_t i = 0; i < nbp; i++) needs_box_spill[i] = false;
    /* The box-spill recognizer keyed on a value-struct parameter (a Type of
     * kind TY_STRUCT).  No Type ever carries that kind now -- value records
     * lower to TY_ADT/TY_APP -- so the population never fired and has been
     * removed.  `needs_box_spill` therefore stays all-false; the rename +
     * heap-spill stanza below remain as an inert safety net for future
     * inline-C `^construct` bodies that cast a pointer-carried param. */
    /* B4 (byvalue-recursive-carrier, slice 2): the inverse of box-spill.  A
     * CLOSURE thunk whose parameter is a WIDE (>8 byte) by-value ADT receives it
     * across the fat-closure boundary as an int64 heap-box POINTER (the value
     * cannot ride the uniform int64 carrier slot directly).  Emit that param as
     * int64_t under a `__tur_b4box_<orig>` alias and deref+copy it back into the
     * by-value aggregate `<orig>` at body entry.  The box is owned by the
     * enclosing carrier node (allocated in its monomorph ctor), so the thunk
     * only borrows it -- no free here.  Restricted to closures: a top-level fn
     * taking a wide by-value ADT uses the ordinary pass-by-ptr ABI, not the fat
     * carrier. */
    bool *needs_box_load = (bool *)arena_alloc(ctx->type_arena, nbp * sizeof(bool));
    for (uint32_t i = 0; i < nbp; i++) needs_box_load[i] = false;
    /* Blocker 2c: a closure stored into a typed fn-field is invoked through that
     * field's by-value typed thunk, so its wide by-value ADT params cross by
     * value -- suppress B4 b4box boxing (it would disagree with the typed thunk
     * + call site and corrupt the arg). */
    /* SR-fat-abi: the byval_fn_field_closure suppression is retired -- the
     * typed-thunk typedef now spells a wide by-value param slot as int64_t
     * (see thunk_param_slot_c_name), so a fn-field closure b4boxes exactly
     * like every other closure and the field dispatch passes the box. */
    if (fd->closure) {
        for (uint32_t i = 0; i < fd->n_params; i++) {
            if (fd->params[i]->is_poly_fn ||
                fd->param_types[i].kind == TY_FN) continue;
            Type pty = use_abi_spec
                ? ctx->current_abi_specialization->arg_types[i]
                : ((e->type.as.fn.arg_full_types && e->type.as.fn.arg_full_types[i])
                       ? *e->type.as.fn.arg_full_types[i] : fd->param_types[i]);
            if (type_is_b4box_closure_slot(emit_resolve_type(ctx, pty)))
                needs_box_load[i] = true;
        }
    }
    for (uint32_t i = 0; i < fd->n_params; i++) {
        if (i > 0) buf_puts(file, ", ");
        /* B4 slice 2: wide by-value ADT closure param arrives as int64 box ptr. */
        if (needs_box_load[i]) {
            const char *pn = raw_name_for_binding(fd->params[i]);
            buf_printf(file, "int64_t __tur_b4box_%s", pn);
            free((void*)pn);
            continue;
        }
        /* typed-comparator-over-hamt-box: a niche-typed parameter whose caller
         * hands over a carrier box arrives as the carrier and is unboxed at
         * body entry (below), exactly like the B4 load above. */
        if (fd->params[i]->arrives_as_carrier_box) {
            const char *pn = raw_name_for_binding(fd->params[i]);
            buf_printf(file, "int64_t __tur_nbox_%s", pn);
            free((void*)pn);
            continue;
        }
        /* Phase HRT1: poly fn params use tur_poly_fn_t in signature */
        if (fd->params[i]->is_poly_fn) {
            buf_puts(file, "tur_poly_fn_t");
        } else if (fd->param_types[i].kind == TY_FN
                   && fd->param_types[i].as.fn.cfnptr) {
            /* typed-c-abi-function-pointers: a cfnptr parameter is a bare C
             * function pointer `R (*)(A...)`, declared via the concrete
             * fn-ptr typedef so the (possibly inline-C) body can invoke it
             * directly and external C libraries see the exact ABI shape.
             * Falls back to int64_t only when the signature is non-concrete. */
            const char *td = register_fn_ptr_typedef(&fd->param_types[i]);
            buf_puts(file, td ? td : "int64_t");
        } else if (fd->param_types[i].kind == TY_FN) {
            /* ER4: function-typed parameters are passed as int64_t (opaque
             * function pointer) in Turmeric's calling convention. */
            buf_puts(file, "int64_t");
        } else if (fd->params[i]->is_fat && body_is_inline_c) {
            /* fat-param-emitted-as-void-ptr-warns-in-inline-c.md: a ^fat param
             * is a fat-closure *carrier* handle.  In an inline-C body, emit it as
             * int64_t -- the same carrier ABI every other Turmeric value uses --
             * so the hand-written C can treat it as a handle without a
             * void*->int64_t -Wint-conversion warning.  The C ABI is identical
             * (8-byte register arg); the call site coerces with (int64_t)(intptr_t).
             * Ordinary (compiler-generated) bodies keep the void* fat carrier so
             * the fat-call dispatch and typeclass-dictionary slot types are
             * unchanged. */
            buf_puts(file, "int64_t");
        } else if (use_abi_spec) {
            const char *pc = emit_type_c_name(ctx, ctx->current_abi_specialization->arg_types[i]);
            buf_puts(file, pc);
            /* KB-021: a concrete carrier-ABI spec param is passed by value.
             * CONV-S1: a by-value parametric record-ADT monomorph param
             * (`tur_adt_Option__int`) is ALSO passed by value but reports
             * NON-carrier from type_uses_carrier_abi (it is genuinely by-value,
             * P2-P4), so it was not flagged -- leaving the ACB carrier->concrete
             * bridge to wrongly deref it (`(*(tur_adt_Option__int *)(intptr_t)
             * container)`) inside an HKT instance body whose param's generic elab
             * type `(Option A)` still reads as the carrier.  Flag a concrete ADT-app
             * spec param too so it is recognised as already by-value. */
            Type resolved_spec_arg =
                emit_resolve_type(ctx, ctx->current_abi_specialization->arg_types[i]);
            if (fd->params[i] && strcmp(pc, "int64_t") != 0 &&
                (type_uses_carrier_abi(resolved_spec_arg) ||
                 type_app_is_concrete_adt(&resolved_spec_arg)))
                fd->params[i]->emit_byvalue_carrier_abi = true;
        } else {
            /* Phase D: large structs use const T* ABI unless in inline-C or closure.
             * bare-fat-sink-poly-box-slot0-int64-mismatch.md: a ^fat param is a
             * fat-closure *carrier* handle, never a by-value fn; its arg_full_types
             * slot may now hold a synthesized (fn ...) signature purely so a caller
             * boxing a tur_poly_fn_t can pick the right slot-0 shim.  Emit the
             * carrier type from param_types here so that signature does not leak
             * into the param's own C type (type_c_name(fn) -> the result type). */
            Type param_ty = (!fd->params[i]->is_fat &&
                             e->type.as.fn.arg_full_types && e->type.as.fn.arg_full_types[i])
                ? *e->type.as.fn.arg_full_types[i]
                : fd->param_types[i];
            if (!fd->closure && !body_is_inline_c && type_struct_pass_by_ptr(param_ty)) {
                buf_printf(file, "const %s *", emit_type_c_name(ctx, param_ty));
            } else {
                const char *pc = emit_type_c_name(ctx, param_ty);
                buf_puts(file, pc);
                /* KB-021: a by-value concrete carrier-ABI parameter must be
                 * bridged to the carrier when used at a dispatch callsite. */
                if (fd->params[i] &&
                    type_uses_carrier_abi(emit_resolve_type(ctx, param_ty)) &&
                    strcmp(pc, "int64_t") != 0)
                    fd->params[i]->emit_byvalue_carrier_abi = true;
                /* CONV-S1 seam 4 (carrier-held by-value-ADT receiver): the param
                 * signature is the int64 carrier (param_ty is the dispatch class
                 * var `a`), but the binding's elaborated type is a by-value
                 * aggregate ADT (`(Option cstr)` under lowering).  Flag it so a
                 * field read off this receiver derefs the carrier pointer to the
                 * concrete monomorph instead of reading the by-value aggregate. */
                if (fd->params[i] && strcmp(pc, "int64_t") == 0) {
                    Type bt = emit_resolve_type(ctx, fd->params[i]->type);
                    const char *btc = emit_type_c_name(ctx, bt);
                    if (btc && strcmp(btc, "int64_t") != 0 &&
                        (bt.kind == TY_ADT || bt.kind == TY_APP) &&
                        !type_is_heap_struct(bt) && !type_is_heap_adt(bt) &&
                        !type_uses_carrier_abi(bt))
                        fd->params[i]->emit_carrier_holds_byval = true;
                    /* MB2 (constrained-hkt-forall-mode-b-plan): a dict-clone's
                     * poly-carrier param is the int64 carrier, but a concrete
                     * :heap-ADT / pointer binding type (`Point`) emits its env
                     * capture field as `tur_adt_Point *`.  Flag the param so the
                     * closure-capture site bridges int64->pointer (field reads
                     * already bridge via the heap-ADT-recv path). */
                    /* hkt-foldable-rc-param: the dict-clone gate used to be the
                     * only way in.  The flag's meaning -- "carrier signature slot,
                     * pointer-shaped binding type" -- holds for any such param, and
                     * an HKT instance method over a pointer-family builtin is now
                     * one: its receiver's elaborated type is `rc<a>`
                     * (`RcControlBlock *`) while its dict-ABI slot stays int64_t.
                     * Every consumer of the flag reacts by adding a value-preserving
                     * int64->pointer bridge, which is correct wherever the flag's
                     * meaning holds, so widening it can only remove straddles. */
                    if (btc && strcmp(btc, "int64_t") != 0 &&
                        strchr(btc, '*') != NULL)
                        fd->params[i]->emit_carrier_holds_ptr = true;
                }
            }
        }
        const char *pn = raw_name_for_binding(fd->params[i]);
        if (needs_box_spill[i]) {
            /* Prereq 6: rename the parameter so the inline-C body's
             * `<orig>` identifier can be redeclared as a heap pointer
             * inside the function. */
            buf_printf(file, " __tur_inbox_%s", pn);
        } else {
            buf_printf(file, " %s", pn);
        }
        free((void*)pn);
    }
    /* `(void)`, not `()`: see the forward declaration in emit_module.c. */
    if (fd->n_params == 0 && !emit_main_argv) buf_puts(file, "void");
    buf_puts(file, ") {\n");

    /* proper-tail-calls T6 (T-D6): a bouncer -- a function whose tail reaches a
     * dynamic call -- learns on entry whether the call that entered it came
     * from a trampoline driver that will take a bounce, and clears the arming
     * so nothing it calls in turn inherits it.  The comparison is against its
     * own address: the driver arms exactly the function it is about to enter. */
    const char *saved_tb_guard = ctx->tb_guard;
    ctx->tb_guard = NULL;
    /* T2b: this body's name and where it starts, for tail_call_musttail_ok. */
    const char *saved_mt_name = ctx->mt_fn_cname;
    const Buf *saved_mt_buf = ctx->mt_body_buf;
    size_t saved_mt_start = ctx->mt_body_start;
    ctx->mt_fn_cname = is_main ? NULL : fn_name;
    ctx->mt_body_buf = file;
    ctx->mt_body_start = file->len;
    if (!is_main && !body_is_inline_c && fn_may_bounce(fd)) {
        ensure_saffron_dyn_runtime(ctx);
        indent_buf(file, ctx->indent + 4);
        buf_printf(file, "bool __tb_may = (tur_tb_armed_for == (void *)%s); "
                         "if (__tb_may) tur_tb_armed_for = NULL; (void)__tb_may;\n",
                   fn_name);
        ctx->tb_guard = "__tb_may";
        if (fd->closure) tb_register_thunk(ctx, fn_name);
    }

    /* Debugger Phase 4 (--debug): anchor the function body to the defn's source
     * line so a gdb/lldb frame for this function resolves to its `.tur` site.
     * Reset the dedup tracker first: this body is a fresh statement stream
     * (possibly into a temp buffer), so the first statement must re-anchor. */
    emit_line_reset(ctx);
    emit_line_directive(ctx, file, e->span);

    /* B4 (byvalue-recursive-carrier, slice 2): materialize each wide by-value
     * ADT closure param from its int64 heap box at entry -- deref + copy into
     * the by-value aggregate `<orig>` the body expects.  Borrow only: the box is
     * owned by the carrier node that produced it, so no free here. */
    for (uint32_t i = 0; i < fd->n_params; i++) {
        if (!needs_box_load[i]) continue;
        Type pty = use_abi_spec
            ? ctx->current_abi_specialization->arg_types[i]
            : ((e->type.as.fn.arg_full_types && e->type.as.fn.arg_full_types[i])
                   ? *e->type.as.fn.arg_full_types[i] : fd->param_types[i]);
        const char *ctype = emit_type_c_name(ctx, pty);
        const char *pn = raw_name_for_binding(fd->params[i]);
        indent_buf(file, ctx->indent + 4);
        buf_printf(file, "%s %s = *(%s *)(intptr_t)__tur_b4box_%s;\n",
                   ctype, pn, ctype, pn);
        free((void*)pn);
    }

    /* typed-comparator-over-hamt-box: materialize each niche-typed parameter
     * whose caller hands over a carrier box.  The CHECKED read, for the same
     * reason the carrier->niche bridge uses it: a box can hold Some(NULL), and
     * the unchecked value would hand the niche a 0 payload -- silently turning
     * a value `some?` calls true into `(none)`.  Borrow only, like the B4 load
     * above: the box belongs to the container that stored it. */
    for (uint32_t i = 0; i < fd->n_params; i++) {
        if (!fd->params[i]->arrives_as_carrier_box) continue;
        Type pty = use_abi_spec
            ? ctx->current_abi_specialization->arg_types[i]
            : ((e->type.as.fn.arg_full_types && e->type.as.fn.arg_full_types[i])
                   ? *e->type.as.fn.arg_full_types[i] : fd->param_types[i]);
        const char *ctype = emit_type_c_name(ctx, emit_resolve_type(ctx, pty));
        const char *pn = raw_name_for_binding(fd->params[i]);
        indent_buf(file, ctx->indent + 4);
        buf_printf(file,
                   "%s %s = (__tur_nbox_%s ? (%s)(intptr_t)"
                   "tur_opt_value_checked(__tur_nbox_%s) : (%s)0);\n",
                   ctype, pn, pn, ctype, pn, ctype);
        free((void*)pn);
    }

    /* M2b RETIRED M2a inference (2026-06-13).
     *
     * Background: M2a (generalized from Prereq 6) used shape inference to
     * synthesize a direct by-value constructor body for `^construct`-
     * annotated polymorphic constructors with value-struct payloads.  It
     * reconstructed the discriminator-tag + payload-field assignment by
     * matching the param's C type against StructDef type-params, then
     * emitted `__tur_p6_r.is_ok = true; memset(...); __tur_p6_r.ok_val =
     * payload;` style code.
     *
     * Why retired: M2b shipped `(make-struct StructName :field val ...)`
     * and `(default-of T)` body forms in elab_structs.c / elab_types.c.
     * stdlib's ok/err/some/none/pair/tcons-of now have explicit make-struct
     * bodies; the normal EX_MAKE_STRUCT emit at emit_expr.c:3700-3731
     * handles the field-by-field construction including heap-spill for
     * value-struct payload fields via struct_field_c_type's pointer
     * lowering.  Empirically, disabling the inference branch produced
     * cleaner C (designated initializer instead of memset-then-assign)
     * with zero fixture-snapshot diffs across the suite (1564/86 baseline)
     * because no fixture's snapshot exercised the inference path -- the
     * stdlib migration to make-struct made it dead code.
     *
     * The variable name `prereq6_synthesized_body` stays for the symmetric
     * m2b_carrier_synth control flow below; this flag remains false
     * because the normal body-emit path handles the by-value case now. */
    bool prereq6_synthesized_body = false;

    /* M2b (end-to-end-monomorphization-plan, Plan M2): synthesize a CARRIER
     * body for any `^construct` polymorphic constructor whose user body is
     * a `(make-struct …)` expression and which is being emitted in the
     * generic carrier-emit path (no `current_abi_specialization`).  Without
     * this, the make-struct body lowers to `(int64_t){.field = …}` — invalid
     * C (designated initializer on a non-aggregate) — because the result
     * type's tyvars never get bound.
     *
     * The synthesized body replays the legacy tur_box_ok / tur_box_err / tur_box_some /
     * TUR_NONE pattern: identify (a) the bool discriminator field's literal
     * value in the make-struct, and (b) the payload field whose value is a
     * direct parameter reference; then emit
     *     return tur_<helper>((int64_t)(intptr_t)<payload>);
     * The helper is chosen from the StructDef name + discriminator value.
     * Currently supports Result (ok / err) and Option (some / none).
     *
     * Out of scope: structs without a bool discriminator (Pair, Cons) — they
     * have no carrier helper and never produce carrier values, so the
     * generic body for them is unreachable from carrier call sites.
     *
     * See docs/archive/history/m2b-stdlib-migration-blocked-on-carrier-fallback.md
     * for the rationale; this implements that report's option (a). */
    bool m2b_carrier_synth = false;
    /* Fire in TWO contexts:
     *   (a) generic carrier-only emit (no spec at all) — every ^construct
     *       polymorphic defn must always emit a callable fallback symbol;
     *   (b) a spec whose declared result type lowers to the int64 carrier
     *       (typeclass-method dispatch context: the method's signature is
     *       int64-in/int64-out so the dict slot is uniformly typed). */
    /* structdef-retirement DS-C: the M2b make-struct-return carrier synth
     * was keyed on the StructDef* in the make-struct node, which is now
     * always NULL (structs lower to ADTs).  The branch never fired, so
     * m2b_carrier_synth stays false and the normal body-emit path handles
     * every ^construct constructor. */

    {
        /* M2b retirement of the M2a inference path: the shape-inference
         * code that used to live here (Result/Option/discriminator field
         * detection + tyvar-position matching + memset-spill emit) is gone.
         * ok/err/some/none now have explicit (make-struct ...) bodies in
         * stdlib; the normal EX_MAKE_STRUCT emit handles their construction
         * including value-struct payload heap-spill via struct_field_c_type.
         * Empirically the inference branch was dead code -- disabling it
         * produced zero fixture-snapshot diffs and ran the suite clean
         * (1564/86 baseline).  See the M2b design doc and audit Section 10
         * for the migration history.
         *
         * The heap-spill stanza below stays: it sets up the `__tur_inbox_X`
         * pointer for the few inline-C `^construct` bodies that may still
         * cast their params via `(int64_t)(intptr_t)x`.  Stdlib has none,
         * but the safety net is cheap and future user code can rely on it. */
        {
            for (uint32_t i = 0; i < fd->n_params; i++) {
                if (!needs_box_spill[i]) continue;
                Type pty = ctx->current_abi_specialization->arg_types[i];
                const char *ctype = emit_type_c_name(ctx, pty);
                const char *pn = raw_name_for_binding(fd->params[i]);
                indent_buf(file, ctx->indent + 4);
                buf_printf(file,
                           "%s *%s = (%s *)malloc(sizeof(%s)); *%s = __tur_inbox_%s;\n",
                           ctype, pn, ctype, ctype, pn, pn);
                free((void*)pn);
            }
        }
    }

    /* WIN1: binary stdout/stderr, before any user code runs.  This is the
     * user-defined-main path; the synthesized-main paths in emit_module.c and
     * the CPS D2b wrapper in emit_cps_ir.c call the same helper. */
    if (is_main) {
        /* S1b (jit-engine-plan): explicit static initialization, ahead of
         * everything else in main -- that is where the `constructor`
         * attributes it replaces used to run.  Idempotent, so the constructor
         * wrapper emitted alongside the definition is harmless here. */
        emit_main_deep_stack_prologue(file);
        ctx->indent += 4;
        indent_buf(file, ctx->indent);
        buf_puts(file, "__tur_static_init();\n");
        ctx->indent -= 4;
        emit_win_binary_stdio_prologue(file);
    }

    /* CLI-ARGS: build *args* cons list from argv[1..argc-1] before user code runs.
     * Mirrors the synthesized-main path in emit_module.c so user-main and
     * synthesized-main programs see *args* identically. */
    if (emit_main_argv) {
        ctx->indent += 4;
        indent_buf(file, ctx->indent);
        buf_puts(file, "/* *args*: build cons list from argv[1..argc-1]; *argv0* is argv[0] */\n");
        indent_buf(file, ctx->indent);
        buf_puts(file, "if (argc > 0 && argv[0]) g_tur_argv0 = argv[0];\n");
        indent_buf(file, ctx->indent);
        buf_puts(file, "g_tur_args = 0;\n");
        indent_buf(file, ctx->indent);
        buf_puts(file, "for (int _ai = argc - 1; _ai >= 1; _ai--) {\n");
        indent_buf(file, ctx->indent);
        buf_puts(file, "    typedef struct { int64_t value; int64_t next; } __tur_args_cell;\n");
        indent_buf(file, ctx->indent);
        buf_puts(file, "    __tur_args_cell *_c = (__tur_args_cell *)malloc(sizeof(__tur_args_cell));\n");
        indent_buf(file, ctx->indent);
        buf_puts(file, "    _c->value = (int64_t)(intptr_t)argv[_ai];\n");
        indent_buf(file, ctx->indent);
        buf_puts(file, "    _c->next = g_tur_args;\n");
        indent_buf(file, ctx->indent);
        buf_puts(file, "    g_tur_args = (int64_t)(intptr_t)_c;\n");
        indent_buf(file, ctx->indent);
        buf_puts(file, "}\n");
        ctx->indent -= 4;
    }

    /* Phase 9 follow-up: Run last-use elision analysis on the function body
     * before emitting.  This marks eligible EX_RC_CLONE/EX_RC_DROP nodes so
     * the emitter can skip redundant rc_strong_increment/decrement pairs. */
    rc_elision_analyze_fn(fd->body);

    /* Emit function body */
    ctx->indent += 4;

    /* Set up function parameter context so that parameter references
     * in the body use raw names (without ID suffix) */
    Binding **saved_params = ctx->fn_params;
    uint32_t saved_n_params = ctx->n_fn_params;
    ctx->fn_params = fd->params;
    ctx->n_fn_params = fd->n_params;

    /* inline-c-locals-invisible-to-inline-c-blocks: same idea one level down --
     * the locals this body's inline-C blocks name get their raw spelling too.
     * Collected per body; empty (and therefore free) for every function whose
     * inline C does not name a local, which is nearly all of them. */
    const Binding **saved_ic_locals = ctx->inline_c_raw_locals;
    uint32_t saved_n_ic_locals = ctx->n_inline_c_raw_locals;
    emit_inline_c_raw_locals_collect(fd->body, fd->params, fd->n_params,
                                     &ctx->inline_c_raw_locals,
                                     &ctx->n_inline_c_raw_locals);

    /* Phase D: record which params are pbp for field-access and call-site handling. */
    uint32_t saved_n_pbp = ctx->n_pbp_params;
    ctx->n_pbp_params = 0;
    /* constrained-generic-monomorph-passbyptr-arg: an ABI-spec clone spells
     * its parameters from the spec's argument types, BY VALUE (the
     * `use_abi_spec` arm of the signature loop), so none of them is a `const
     * T *` however wide -- registering one sent every use through the pointer
     * path: `*(y)` on a `tur_adt_Reg y`. */
    bool spec_params_by_value = ctx->current_abi_specialization &&
                                ctx->current_abi_specialization->fn == fd;
    if (!fd->closure && !body_is_inline_c && !spec_params_by_value) {
        for (uint32_t _pi = 0; _pi < fd->n_params; _pi++) {
            Type pty = (e->type.as.fn.arg_full_types && e->type.as.fn.arg_full_types[_pi])
                ? *e->type.as.fn.arg_full_types[_pi] : fd->param_types[_pi];
            if (type_struct_pass_by_ptr(pty))
                emit_pbp_push(ctx, fd->params[_pi]);
        }
    }

    /* Phase R5: Set no_unwind context from the function's binding attribute */
    bool saved_no_unwind = ctx->no_unwind;
    ctx->no_unwind = fd->binding->no_unwind;

    /* Phase 3: Set up closure context if this is a closure thunk */
    struct Closure *saved_closure = ctx->closure;
    const char *saved_env_var_name = ctx->env_var_name;
    if (fd->closure) {
        ctx->closure = fd->closure;
        /* First parameter is the env */
        if (fd->n_params > 0) {
            Binding *env_param = fd->params[0];
            /* Emit cast of env parameter to env struct type */
            indent_buf(file, ctx->indent);
            char *env_param_name = raw_name_for_binding(env_param);
            /* Create a local variable name for the casted env */
            char env_var_name_buf[80];
            snprintf(env_var_name_buf, sizeof(env_var_name_buf), "__env_%s", env_name_eff->name);
            buf_printf(file, "struct %s *%s = (struct %s *)%s;\n",
                       env_name_eff->name,
                       env_var_name_buf,
                       env_name_eff->name,
                       env_param_name);
            free(env_param_name);
            /* Store the env variable name for use in name_for_binding */
            ctx->env_var_name = strdup(env_var_name_buf);
        }
    }

    /* Get the return type */
    TypeKind result_kind = use_abi_spec
        ? ctx->current_abi_specialization->result_type.kind
        : (e->type.kind == TY_FN ? e->type.as.fn.result_kind : TY_NIL);

    /* Phase 3/4: If the body diverges on every path (return/panic), emit it as
     * statements only -- a trailing `return` would be dead code.  A body that
     * may still fall through with a value (e.g. an early `return` in only one
     * `if` branch) must go through the emit_value path below so the
     * fall-through value is returned. */
    bool body_diverges = expr_tail_diverges(fd->body);

    /* CF1: detect self-tail-calls so a value-returning, simple-parameter
     * function can be emitted as an iterative loop instead of self-recursion.
     * Excludes main (different signature handling).
     *
     * M4 follow-up (docs/archive/history/tco-in-abi-specs-for-stdlib-iteration.md):
     * lifted the `!use_abi_spec` restriction.  TCO inside an ABI spec is
     * safe: the `__tur_tailcall:` label and the param-reassign loop are
     * pure C-level constructs, and `tco_params_simple` reads the spec's
     * resolved param types (post-substitution) so the backedge declarations
     * match the spec's signature.  Carrier-ABI params still reject in
     * `tco_params_simple` so a backedge's reassignment never crosses an
     * int64→struct boundary unintentionally. */
    /* The tail SPINE is walkable under all the same function-level conditions;
     * what the spine then contains decides how the body is emitted.  Split out
     * of the old single `tco_eligible` expression so the two kinds of tail call
     * can be counted separately (proper-tail-calls T2 / T-D2). */
    /* void-self-tail-call-not-lowered: a `: nil` function walks the same
     * spine, as long as it really is emitted `void` -- its leaves then end in
     * a bare `return;` (emit_tail_void_return).  Only a genuine backedge
     * routes it through emit_tail: a void return cannot be a C tail call, so a
     * spine of non-self calls alone gains nothing there. */
    const char *fn_ret_c = ctx->current_fn_ret_ctype;
    bool fn_ret_c_void = !fn_ret_c || !*fn_ret_c || strcmp(fn_ret_c, "void") == 0;
    bool tail_void = result_kind == TY_NIL && !is_main && fn_ret_c_void;
    bool tco_spine_ok = !body_diverges && fd->body->kind != EX_INLINE_C &&
        !(result_kind == TY_NIL && !tail_void) && !is_main &&
        tco_params_simple(ctx, e, fd);
    int n_tco_self = 0, n_tco_tail = 0;
    ctx->tail_dyn_seen = false;
    bool saved_tail_void = ctx->tail_void;
    ctx->tail_void = tail_void;
    if (tco_spine_ok)
        n_tco_self = tco_mark(ctx, fd, fn_name, fd->body, &n_tco_tail);
    /* T2: a body whose tail spine holds ONLY non-self tail calls still goes
     * through emit_tail -- that is what puts the `return` in each branch, and so
     * what makes the call a C tail call at all.  The `__tur_tailcall:` label is
     * gated on a genuine backedge, because an emitted-and-unused label is a C
     * compiler warning.
     *
     * No all-or-nothing rule: T2's first cut kept a body out of emit_tail unless
     * EVERY non-self tail leaf could be served, because a refused leaf landing in
     * emit_tail's thinner return path emitted an unbridged return.  Both paths
     * share emit_fn_return_spelling now, so a refused leaf is emitted exactly as
     * it always was. */
    bool tco_eligible = tco_spine_ok &&
        (tail_void ? n_tco_self > 0
                   : (n_tco_self > 0 || n_tco_tail > 0 || ctx->tail_dyn_seen));
    bool tco_wants_label = n_tco_self > 0;

    /* proper-tail-calls T1 (T-D1): now that tco_mark has had its say, work out
     * what to tell the author about a `^tailcall` that did NOT become a
     * backedge.  The conditions above are in && order, so re-test them
     * individually to name the one that refused -- a function-level refusal is
     * reported only when the call's own shape was fine.  The reporting itself
     * waits until `did_general` is known, below. */
    const char *tc_fn_block = NULL;
    if (is_main)
        tc_fn_block = "`main` is emitted with a different signature and carries "
                      "no backedge";
    else if (fd->is_variadic)
        tc_fn_block = "the enclosing function is variadic, and a variadic body "
                      "is never TCO-eligible";
    else if (fd->body->kind == EX_INLINE_C)
        tc_fn_block = "the enclosing function has an inline-C body, which the "
                      "emitter passes through verbatim";
    else if (result_kind == TY_NIL && !tail_void)
        tc_fn_block = "the enclosing function returns nothing but is not "
                      "emitted as a C `void` function, so its body is emitted "
                      "as statements rather than through the tail path";
    else if (body_diverges)
        tc_fn_block = "every path of the enclosing function diverges (`return` "
                      "or `panic`), so no tail path is emitted";
    else if (!tco_params_simple(ctx, e, fd))
        tc_fn_block = "a parameter of the enclosing function cannot be "
                      "reassigned by a backedge (pass-by-pointer struct, "
                      "`fn`-typed, poly-fn, or carrier-ABI)";

    bool did_general = false;
    if (!prereq6_synthesized_body && !m2b_carrier_synth &&
        stackless_general_eligible(ctx, e, fd, result_kind)) {
        /* G3 (compiled-catch-unwind-general-lowering): general segment splitter
         * -- arbitrary catch-crossing bodies (multiple catch sites, non-tail
         * self-recursion, nested if/let/do, a panicking thunk) as a flat-stack
         * trampoline.  Returns false (falls through to normal emission) only if
         * a hard limit is hit. */
        did_general = emit_stackless_general_body(ctx, file, fd, result_kind);
    }
    /* G4 (cross-function / mutual recursion): a function that cross-calls other
     * trampolined members lowers as a shim into a shared group driver. */
    if (!did_general && !prereq6_synthesized_body && !m2b_carrier_synth &&
        !ctx->current_abi_specialization &&
        gs_basic_ok(ctx, fd) && sc_scalar_kind(result_kind) && ctx->current_fn_ret_ctype) {
        FnDef *group[GS_MAXMEM];
        int ng = gs_find_group(ctx, fd, group);
        if (ng >= 2) did_general = emit_group_member(ctx, file, fd, result_kind, group, ng);
    }
    /* proper-tail-calls T1 (T-D1): report here, where `did_general` is known.
     * G3's segment splitter and G4's group driver are both FLAT-STACK
     * lowerings: they take over the whole body and run it without growing the
     * C stack, which is exactly what `^tailcall` asks for, so a body they
     * claimed is not an error even when tco_mark refused it.  Reporting before
     * this point would have made those a false positive. */
    if (!did_general) tc_verify_fn(ctx, fd, fn_name, tc_fn_block);

    if (did_general) {
        /* whole body emitted by the general splitter / group shim */
    } else if (tcg) {
        /* T5: the body lives in the group's fused function; this function is
         * the entry for every call that is not a tail call from inside it. */
        indent_buf(file, ctx->indent);
        buf_printf(file, "return %s(%d", tcg->name, tcg_cur_v.idx);
        uint32_t base = tcg->slot_base[tcg_cur_v.idx];
        for (uint32_t k = 0; k < tcg->n_slots; k++) {
            if (k >= base && k < base + fd->n_params) {
                char *pn = raw_name_for_binding(fd->params[k - base]);
                buf_printf(file, ", %s", pn);
                free(pn);
            } else {
                char *z = emit_c_zero_of(tcg->slot_ctype[k]);
                buf_printf(file, ", %s", z ? z : "0");
                free(z);
            }
        }
        buf_puts(file, ");\n");
    } else if (prereq6_synthesized_body) {
        /* Prereq 6: the function body was already emitted as a synthesized
         * wrapper above (heap-spill + carrier helper call + cast back to
         * the by-value struct). Skip the normal body-emit paths; the
         * function's closing brace at the end of emit_fn_def still runs. */
    } else if (m2b_carrier_synth) {
        /* M2b carrier-emit synth (above) already wrote a `return tur_*(...)`
         * line; skip the make-struct body so it doesn't double-emit. */
    } else if (body_diverges) {
        /* Body diverges on every path - emit as statements only, then a
         * trailing `return` that is dead code by construction.
         *
         * `panic` is NOT `noreturn` on the compiled path: it sets
         * tur_panicking and returns, and the per-call-site
         * `if (tur_panicking) return ...;` is what unwinds.  So a C compiler
         * reading a value-returning function whose body ends in a panic sees
         * control reach the closing brace:
         *
         *   warning: non-void function does not return a value in all control
         *   paths [-Wreturn-type]
         *
         * That is not just noise.  One such function in stdlib
         * (`schema-decode-abort`) put a clang warning on stderr of every
         * program that loaded the module, which is how it broke
         * tests/run-offtree-load.sh: that harness captures 2>&1 and compares
         * against expected stdout, so the program printed the right answer
         * and the assertion failed anyway -- on macOS only, because gcc and
         * clang word and trigger this warning differently.
         *
         * Fixing it in the stdlib source does not work: a value written after
         * the panic is unreachable, and elaboration elides it, so the body
         * still diverges and the emitted C is unchanged.  It has to be here.
         *
         * The default follows the same scalar/aggregate split as every other
         * synthesized zero in the emitter -- `((T)0)` for a scalar, `(T){0}`
         * for a struct/ADT typedef.  Getting this wrong is a build failure,
         * not a warning: `return 0;` from a by-value-aggregate function does
         * not compile (caught by catch-unwind-aggregate-thunk). */
        emit_stmt(ctx, file, fd->body);
        /* Only when the EMITTED C type is a known non-void.  `result_kind`
         * cannot decide this on its own: a `: !` (never) function is
         * TY_NEVER, not TY_NIL, and emits as `void` -- so keying off the kind
         * put `return 0;` in a void function, which clang rejects outright
         * ("void function 'inner' should not return a value") while gcc only
         * warns.  panic-trace is that fixture, and it broke macOS CI while
         * passing on Linux.
         *
         * The bias when the C type is unknown is to emit NOTHING: a missing
         * return is a -Wreturn-type warning, a wrong one is a build failure,
         * and this branch exists to remove a warning in the first place. */
        const char *rc = ctx->current_fn_ret_ctype;
        bool rc_is_void = (!rc || !*rc || strcmp(rc, "void") == 0);
        if (is_main && result_kind == TY_INT) {
            indent_buf(file, ctx->indent);
            buf_puts(file, "return (int)0;\n");
        } else if (!rc_is_void && result_kind != TY_NIL) {
            indent_buf(file, ctx->indent);
            if (emit_c_type_is_scalar(rc)) {
                buf_printf(file, "return ((%s)0);\n", rc);
            } else {
                buf_printf(file, "return (%s){0};\n", rc);
            }
        }
    } else if (fd->body->kind == EX_INLINE_C) {
        /* region-lock-hardening follow-up: an inline-C body is OPAQUE to the
         * emitter, so a caller's node handed to one may be retained past the
         * call (written into a malloc'd cell, a global, a foreign structure)
         * with no store the compiler can hook.  That was a silent wrong
         * answer on the default build: a `Link` built inside `with-region`,
         * passed to a hand-written `cell-set!`, read back after the pop --
         * the bracket rewound and the reader printed garbage, where
         * TUR_REGIONS=0 printed the value
         * (docs/archive/region-escape-through-unhooked-stores.md).
         *
         * The callee is the one place each parameter is a plain C identifier,
         * so the note goes here rather than at every call site: one site per
         * function, no argument re-emission, and it covers stdlib and user
         * inline-C by the same rule.  Only a parameter whose TYPE can reach a
         * region node is noted -- a scalar cannot be one, and an erased node
         * (`(:: (Link ..) :int)`) was already noted at its ascription -- so
         * the common `(defn f [x : int] ...)` inline-C body is untouched and
         * a bracket that hands no node to inline-C keeps its rewind.
         *
         * Conservative by construction: the note only ever BLOCKS a rewind,
         * so a body that does not in fact retain its argument costs a saving
         * on that generation, never correctness. */
        for (uint32_t i = 0; i < fd->n_params; i++) {
            if (!fd->params[i]) continue;
            if (fd->params[i]->is_poly_fn) continue;
            /* The BINDING's type, not fd->param_types[i]: the latter carries
             * a def-less TY_ADT for a `:heap` parameter (its c-name falls back
             * to the int64 carrier), so every node param read as "erased" and
             * the note never fired -- the repro still printed garbage with the
             * loop in place. */
            Type pt = emit_resolve_type(ctx, fd->params[i]->type);
            /* The filter is "this word can BE region memory", not the result
             * lock's "cannot prove it reaches nothing" -- see
             * emit_region_word_can_be_node.  The difference is most of the
             * stdlib: a `ptr<void>`, a `cstr`, a bare tyvar and a `(Map K V)`
             * handle all fail the result lock and none can ever be a node, so
             * noting them would put a runtime call atop every hot accessor
             * and flag nothing. */
            if (!emit_region_word_can_be_node(ctx, &pt)) continue;
            char *pn = raw_name_for_binding(fd->params[i]);
            if (pn) {
                emit_region_note_lvalue(file, ctx->indent,
                                        emit_type_c_name(ctx, pt), pn);
                free(pn);
            }
        }
        /* panic-location-names-the-runtime-not-the-call-site: a body that
         * calls `tur_panic(` panics through tur_panic_sited, which names the
         * site its direct caller set around the call (the general call arm
         * in emit_expr.c, the cps->direct arms), so `(vec-get v 9)` names
         * the program's line.  Nothing happens at entry; a body entered with
         * no site set (through a function value) names the runtime's own
         * line, as before. */
        bool sited = fn_def_panics_in_inline_c(fd);
        if (sited) buf_puts(file, "#define tur_panic(m) tur_panic_sited(m)\n");
        /* Inline C body - emit as-is (it contains its own return statements) */
        emit_stmt(ctx, file, fd->body);
        if (sited) buf_puts(file, "#undef tur_panic\n");
    } else if (result_kind == TY_NIL && !is_main && !tco_eligible) {
        /* void function - emit body as statements */
        emit_stmt(ctx, file, fd->body);
    } else if (tco_eligible) {
        /* CF1: self-tail-call loop. Label the top of the body; emit_tail turns
         * each self-tail-call into a parameter-reassignment + goto backedge.
         * T2 (T-D2): with no self call in the spine there is no backedge and so
         * no label -- emit_tail is entered purely to put the `return` in each
         * branch, which is what makes a non-self tail call a C tail call. */
        if (tco_wants_label) {
            indent_buf(file, ctx->indent);
            buf_puts(file, "__tur_tailcall:;\n");
        }
        emit_tail(ctx, file, e, fd, fd->body, result_kind, is_main);
    } else if (fd->body->type.kind == TY_NIL) {
        /* Body is nil-typed (e.g. a bare void call like (show 99)) but the
         * function expects a return value -- typically (defn main [] :int
         * (some-void-call)).  Emit the body as a STATEMENT so its side effects
         * (and the call itself) actually execute; emit_value would only return
         * the call string for a caller to consume, and the discard below would
         * drop the call.  Then return the default for result_kind. */
        emit_stmt(ctx, file, fd->body);
        indent_buf(file, ctx->indent);
        const char *dflt = (result_kind == TY_BOOL) ? "false" : "0";
        if (is_main && result_kind == TY_INT) {
            buf_printf(file, "return (int)%s;\n", dflt);
        } else {
            buf_printf(file, "return %s;\n", dflt);
        }
    } else {
        /* Function with return value.  The spelling -- carrier bridges, heap
         * spills, straddle casts -- is emit_fn_return_spelling's decision, the
         * same one emit_tail's tail path takes. */
        emit_fn_return_spelling(ctx, file, e, fd, fd->body,
                                emit_fat_return_value(ctx, file, e, fd->body),
                                result_kind, is_main);
    }

    /* Restore previous context */
    free((void *)ctx->inline_c_raw_locals);
    ctx->inline_c_raw_locals   = saved_ic_locals;
    ctx->n_inline_c_raw_locals = saved_n_ic_locals;
    ctx->fn_params = saved_params;
    ctx->n_fn_params = saved_n_params;
    ctx->no_unwind = saved_no_unwind;  /* Phase R5 */
    ctx->n_pbp_params = saved_n_pbp;   /* Phase D */
    ctx->closure = saved_closure;
    free((void*)ctx->env_var_name);
    ctx->env_var_name = saved_env_var_name;
    ctx->current_fn_ret_ctype = saved_current_fn_ret_ctype;
    ctx->own_cur_fn = saved_own_cur_fn;
    ctx->tail_void = saved_tail_void;

    ctx->indent -= 4;
    buf_printf(file, "}\n\n");
    if (!ctx->fn_name_override) free((void*)fn_name);

    /* Phase 19 (per-fiber): flush handlers accumulated during this body, then
     * commit the temp function buffer to the real file. */
    if (ctx->pending_handler_fns && ctx->pending_handler_fns->len > 0) {
        buf_write(real_file, ctx->pending_handler_fns->data, ctx->pending_handler_fns->len);
        buf_free(ctx->pending_handler_fns);
        buf_init(ctx->pending_handler_fns);
    }

    buf_write(real_file, fn_tmp.data, fn_tmp.len);
    buf_free(&fn_tmp);
    ctx->file = real_file;
    ctx->tcg_cur = saved_tcg_cur;
    ctx->tb_guard = saved_tb_guard;
    ctx->mt_fn_cname = saved_mt_name;
    ctx->mt_body_buf = saved_mt_buf;
    ctx->mt_body_start = saved_mt_start;

    /* CPS3: emit __cps wrapper for colored non-closure functions when --cps-path */
    if (fd->is_cps && g_cps_path && !is_main && !fd->closure) {
        char *wrap_name = raw_name_for_binding(fd->binding);
        buf_printf(real_file, "static void %s__cps(tur_cps_cont_t *__k", wrap_name);
        for (uint32_t i = 0; i < fd->n_params; i++) {
            buf_puts(real_file, ", ");
            if (fd->params[i]->is_poly_fn) {
                buf_puts(real_file, "tur_poly_fn_t");
            } else if (fd->param_types[i].kind == TY_FN) {
                buf_puts(real_file, "int64_t");
            } else if (fd->params[i]->is_fat && body_is_inline_c) {
                /* fat-param-emitted-as-void-ptr-warns-in-inline-c.md: ^fat
                 * carrier handle -> int64_t (matches the direct signature).
                 * Inline-C bodies are never CPS, so this stays void* in
                 * practice; gated for parity with the direct signature. */
                buf_puts(real_file, "int64_t");
            } else {
                Type param_ty = (!fd->params[i]->is_fat &&
                                 e->type.as.fn.arg_full_types && e->type.as.fn.arg_full_types[i])
                    ? *e->type.as.fn.arg_full_types[i]
                    : fd->param_types[i];
                buf_puts(real_file, emit_type_c_name(ctx, param_ty));
            }
            const char *pn = raw_name_for_binding(fd->params[i]);
            buf_printf(real_file, " %s", pn);
            free((void*)pn);
        }
        buf_puts(real_file, ") {\n");
        /* Call the direct function and apply the continuation with the result.
         * nil-returning functions lower to `void` in C, so pass 0 to the
         * continuation instead of casting a void expression. */
        if (result_kind == TY_NIL) {
            buf_printf(real_file, "    %s(", wrap_name);
            for (uint32_t i = 0; i < fd->n_params; i++) {
                if (i > 0) buf_puts(real_file, ", ");
                const char *pn = raw_name_for_binding(fd->params[i]);
                buf_puts(real_file, pn);
                free((void*)pn);
            }
            buf_puts(real_file, ");\n");
            buf_puts(real_file, "    tur_cps_apply(__k, 0);\n");
        } else {
            buf_printf(real_file, "    int64_t __result = (int64_t)%s(", wrap_name);
            for (uint32_t i = 0; i < fd->n_params; i++) {
                if (i > 0) buf_puts(real_file, ", ");
                const char *pn = raw_name_for_binding(fd->params[i]);
                buf_puts(real_file, pn);
                free((void*)pn);
            }
            buf_puts(real_file, ");\n");
            buf_puts(real_file, "    tur_cps_apply(__k, __result);\n");
        }
        buf_puts(real_file, "}\n\n");
        free(wrap_name);
    }
    /* MB1: restore dict-dispatch mode. */
    ctx->dict_dispatch_param_cname = saved_dd_cname;
    ctx->dict_dispatch_class = saved_dd_class;
    ctx->dict_dispatch_n = saved_dd_n;
    memcpy(ctx->dict_dispatch_classes, saved_dd_classes, sizeof saved_dd_classes);
    memcpy(ctx->dict_dispatch_param_cnames, saved_dd_cnames, sizeof saved_dd_cnames);
    memcpy(ctx->dict_dispatch_params, saved_dd_params, sizeof saved_dd_params);
    for (uint8_t k = 0; k < MAX_FN_CONSTRAINTS; k++) free(dd_cnames_owned[k]);
    ctx->cur_dict_env_n = saved_de_n;
    memcpy(ctx->cur_dict_env_classes, saved_de_classes, sizeof saved_de_classes);
    memcpy(ctx->cur_dict_env_bindings, saved_de_bindings, sizeof saved_de_bindings);
}
