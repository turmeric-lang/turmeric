/* emit_effects.c -- expression-position emission for algebraic effects and
 * CPS (shift/reset) constructs, on the DIRECT emitter.  What is here:
 *
 *   - the effect arms: EX_DEFECT, EX_HANDLE (a non-effect `handle` shape still
 *     has a direct path), the handler literal and compose-handlers, EX_RESUME
 *     and EX_DISCONTINUE;
 *   - the reset/shift family: EX_RESET / EX_CLONEABLE_RESET / EX_SHIFT /
 *     EX_SHIFT0 / EX_CLONEABLE_SHIFT / EX_SERIAL_RESET / EX_SERIAL_SHIFT;
 *   - the continuation predicate, EX_CONT_PRED.
 *
 * EX_PERFORM is not here: the CPS/DK backend (emit_cps_ir.c) is the only
 * effect lowering, and emit_value_dispatch's EX_PERFORM arm emits nothing.
 */
#include "emit_internal.h"
#include "globals.h"
#include "effect.h"   /* serial receiver escaping-effect note */

/* =========================================================================
 * Region C -- algebraic effects
 * ========================================================================= */

/* Tier C (P2): a by-value aggregate effect value cannot ride the int64 fiber-
 * effect ABI by a cast (a struct is not convertible to int64).  It crosses
 * boxed -- heap-copy + pointer -- exactly as it crosses the DK slot on the CPS
 * backend, and is unboxed on the consuming side.  Restricted to owning-free
 * flat products (needs_drop_glue == false), so the box is a pure value copy;
 * owning-field aggregates stay unsupported (as on the CPS path). */
static bool eff_type_boxed(Type t) {
    if (!type_is_byvalue_adt_product(t)) return false;
    const AdtDef *d = (t.kind == TY_ADT) ? t.as.adt_.def
                    : (t.kind == TY_APP) ? type_adt_app_def(&t) : NULL;
    return d && !d->needs_drop_glue;
}

/* Box `expr` (of aggregate C type `cty`) into an int64 heap pointer. Malloc'd. */
static char *eff_box_expr(const char *cty, const char *expr) {
    Buf b; buf_init(&b);
    buf_printf(&b, "(int64_t)(intptr_t)({ %s *__eb = (%s *)malloc(sizeof(%s)); *__eb = (%s); __eb; })",
               cty, cty, cty, expr);
    buf_putc(&b, '\0');
    char *s = strdup(b.data); buf_free(&b); return s;
}

/* Store a value of Turmeric type `t` into the int64 fiber-effect ABI slot
 * (an effect arg, a resume value, an effect/handle result).  Mirrors
 * emit_cps_ir.c's slot_store so the direct/fiber path agrees bit-for-bit with
 * the CPS backend:
 *   - a FLOAT is bit-REINTERPRETED into the int64 slot -- a plain `(int64_t)d`
 *     cast truncates the fraction (7.1 -> 7); the union pun preserves the bits.
 *   - a by-value aggregate is boxed (heap-copy + pointer).
 *   - everything else rides the slot by a plain integer cast.
 * The bit-exact inverse of eff_slot_load. */
static char *eff_slot_store(Type t, const char *e) {
    if (eff_type_boxed(t))
        return eff_box_expr(type_c_name(t), e);
    Buf b; buf_init(&b);
    if (t.kind == TY_FLOAT || t.kind == TY_FLOAT64)
        buf_printf(&b, "(int64_t)((union { double d; int64_t i; }){ .d = (%s) }).i", e);
    else if (t.kind == TY_FLOAT32)
        buf_printf(&b, "(int64_t)(uint32_t)((union { float f; uint32_t u; }){ .f = (%s) }).u", e);
    else
        buf_printf(&b, "(int64_t)%s", e);
    buf_putc(&b, '\0');
    char *s = strdup(b.data); buf_free(&b); return s;
}

/* Load a value of Turmeric type `t` out of the int64 fiber-effect ABI slot.
 * The bit-exact inverse of eff_slot_store: a FLOAT is bit-reinterpreted (not
 * numeric-converted), a by-value aggregate is unboxed, everything else is a
 * plain cast back to its C type. `s` is always a call/subscript (binds tighter
 * than the cast), so no wrapping parens are needed -- keeping the non-float
 * output byte-identical to the pre-fix emission. */
static char *eff_slot_load(Type t, const char *s) {
    Buf b; buf_init(&b);
    if (eff_type_boxed(t))
        buf_printf(&b, "*(%s *)(intptr_t)%s", type_c_name(t), s);
    else if (t.kind == TY_FLOAT || t.kind == TY_FLOAT64)
        buf_printf(&b, "((union { double d; int64_t i; }){ .i = (int64_t)(%s) }).d", s);
    else if (t.kind == TY_FLOAT32)
        buf_printf(&b, "((union { float f; uint32_t u; }){ .u = (uint32_t)(%s) }).f", s);
    else
        buf_printf(&b, "(%s)%s", type_c_name(t), s);
    buf_putc(&b, '\0');
    char *r = strdup(b.data); buf_free(&b); return r;
}

char *emit_effects_defect(EmitCtx *ctx, Buf *body, const Expr *e) {
    (void)ctx; (void)body; (void)e;
    /* Effect definitions are compile-time only - no runtime code */
    return atom_nil();
}


bool emit_handle_is_pure_unsafe(const HandleExpr *h) {
    return h && h->is_unsafe_marker;
}

bool emit_handle_is_inert(const HandleExpr *h) {
    return h && (h->is_unsafe_marker || h->no_clause_reachable);
}

void emit_handle_no_lowering(const Expr *e) {
    diag_emit(DIAG_ERROR, e->span,
              "this `handle` has no lowering here: the enclosing function left "
              "the CPS backend's supported subset, and the direct emitter "
              "cannot install a handler. Run with TUR_TRACE_EVICT=1 to see "
              "which form evicted the function: SIG-REJECT is a parameter or "
              "result type the backend cannot thread (such as `(Vec int)`), "
              "SIG-TAINT a call to a function rejected for one. Moving the "
              "`handle` into a function whose signature the backend accepts "
              "usually avoids it. This is a compiler limitation, not a mistake "
              "in this expression.");
}

/* emit_effects_handle's walk: report every effect operation under a handle
 * the direct emitter cannot lower through emit_value's own located error,
 * which is all emit_value does for one (nothing is emitted). */
typedef struct { EmitCtx *ctx; int n; } HandlePerformWalk;

static bool handle_report_performs(const Expr *c, void *ud) {
    HandlePerformWalk *w = (HandlePerformWalk *)ud;
    if (c->kind == EX_PERFORM || c->kind == EX_WITH_HANDLER || c->kind == EX_DISCONTINUE) {
        Buf scratch; buf_init(&scratch);
        free(emit_value(w->ctx, &scratch, c));
        buf_free(&scratch);
        w->n++;
        return false;
    }
    return cps_visit_children(c, handle_report_performs, ud);
}

char *emit_effects_handle(EmitCtx *ctx, Buf *body, const Expr *e) {
    /* (handle body (E [params] k) handler ...) on the DIRECT emitter.
     *
     * A handle lowers on the DK backend (emit_cps_ir); one that reaches here
     * belongs to a function the backend evicted.  Two shapes are still sound,
     * because nothing under them suspends, so no continuation is captured and
     * the body can run in place:
     *
     *  - the `(unsafe ...)` desugar (defopaque-struct-payload-fails-through-
     *    unsafe-helper): the built-in Unsafe effect is a compile-time marker,
     *    never performed.  Emitting the body directly also keeps its real C
     *    type -- a by-value struct return used to be truncated through the
     *    fiber's int64 result slot;
     *  - a handle none of whose clauses the body can reach (TUR-W0033 for
     *    every clause; effect_check records it).
     *
     * Statement position is emit_stmt's EX_HANDLE case, which emit_stmt's the
     * body so its side effects still run.
     *
     * Anything else would need a handler installed in direct code.  That was
     * the fiber effect runtime, deleted 2026-07-19; this function kept its
     * emitter, and an inert-looking `handle` over a `(Vec int)`-taking
     * function got C naming TurEffectCaptureCtx and FiberBlock::eff_ctx
     * (evicted-handle-reaches-deleted-fiber-emitter).  Report it at the
     * handle, as `perform` reports its own case. */
    HandleExpr *h = e->as.handle_.handle;
    if (emit_handle_is_inert(h))
        return emit_value(ctx, body, h->body);
    /* A `perform` under the handle names the shape that evicted the function
     * at its own site (the known-shapes list is there), as when the fiber
     * emitter walked into it; report the handle itself only when there is
     * none -- the SIG-REJECT case. */
    HandlePerformWalk w = { ctx, 0 };
    cps_visit_children(e, handle_report_performs, &w);
    if (w.n == 0)
        emit_handle_no_lowering(e);
    return atom_nil();
}

/* B3 part 2: emit a DK-ABI case fn for a first-class handler literal case, so a
 * dynamic `(with-handler <value> body)` can install it on the DK
 * (dk_hgroup_from_table).  Restricted to the shape the 3 dynamic-handler target
 * fixtures use: a case body that is `[non-control stmt]* then a TAIL single-shot
 * (resume k V)`, with at most one effect parameter.  Returns true (and writes the
 * fn to `hbuf`) when emittable; false leaves dk_fn=0 so the dynamic install
 * evicts gracefully.  The non-tail prefix is emitted with the normal direct
 * emitter; only the tail resume is hand-lowered to dk_tail_resume (the fiber
 * emitter would emit tur_effect_cont_resume). */
static bool emit_dk_handler_case_fn(EmitCtx *ctx, Buf *hbuf, const char *dkfn_name,
                                    HandleCase *c, const char *env_type,
                                    const char *env_var, bool has_caps,
                                    Binding **caps, uint32_t n_caps) {
    if (!c || c->n_params > 1) return false;
    const Expr *cbody = c->body;
    const Expr *tail = cbody;
    const Expr *const *prefix = NULL;
    uint32_t n_prefix = 0;
    if (cbody && cbody->kind == EX_DO && cbody->as.do_.n >= 1) {
        prefix = (const Expr *const *)cbody->as.do_.items;
        n_prefix = cbody->as.do_.n - 1;
        tail = cbody->as.do_.items[cbody->as.do_.n - 1];
    }
    if (!tail || tail->kind != EX_RESUME || !tail->as.resume_.resume) return false;
    ResumeExpr *r = tail->as.resume_.resume;
    if (!r->k || r->k->type.copy_kind == CK_MULTISHOT) return false;  /* single-shot only */

    Buf fn; buf_init(&fn);
    Buf pend; buf_init(&pend);
    buf_puts(&fn, "    (void)__dkenv; (void)__dkarg; (void)__dksubk;\n");
    if (has_caps)
        buf_printf(&fn, "    %s *%s = (%s *)(void *)(intptr_t)__dkenv;\n",
                   env_type, env_var, env_type);
    /* the single effect param (if any) arrives in __dkarg */
    if (c->n_params == 1 && c->param_bindings && c->param_bindings[0]) {
        const char *ct = type_c_name(c->param_bindings[0]->type);
        char *raw = raw_name_for_binding(c->param_bindings[0]);
        char *ld = eff_slot_load(c->param_bindings[0]->type, "((int64_t)__dkarg)");
        bool fnval = c->param_bindings[0]->type.kind == TY_FN
                     && !c->param_bindings[0]->type.as.fn.boxed;
        if (fnval) buf_printf(&fn, "    %s %s = %s;\n", ct, raw, ld);
        else       buf_printf(&fn, "    %s %s_%u = %s;\n", ct, raw,
                              (unsigned)c->param_bindings[0]->id, ld);
        free(ld); free(raw);
    }
    /* the continuation binding holds the DK subk (as an int64, like the fiber k) */
    if (c->k_binding) {
        char *raw = raw_name_for_binding(c->k_binding);
        buf_printf(&fn, "    int64_t %s_%u = (int64_t)(intptr_t)__dksubk;\n",
                   raw, (unsigned)c->k_binding->id);
        free(raw);
    }
    EmitCtx hc = *ctx;
    hc.file = &fn; hc.pending_handler_fns = &pend; hc.indent = 4;
    hc.fn_params = NULL; hc.n_fn_params = 0; hc.closure = NULL;
    hc.env_var_name = has_caps ? env_var : NULL;
    hc.defer_captures = NULL; hc.n_defer_captures = 0; hc.frame_var = NULL;
    hc.return_emitted = false;
    hc.handle_captures = has_caps ? caps : NULL;
    hc.n_handle_captures = has_caps ? n_caps : 0;
    hc.handle_env_name = has_caps ? env_var : NULL;
    /* byval-spine-drop-past-early-exit: a copied context inherits the OUTER
     * function's signature verdict; this C function has its own. */
    hc.current_fn_spine_drop_safe = false;
    /* non-tail prefix statements (e.g. (println s)) -- ordinary direct emit */
    for (uint32_t i = 0; i < n_prefix; i++) emit_stmt(&hc, &fn, prefix[i]);
    /* tail (resume k V): consume the continuation and tail-resume it on the DK */
    char *kv = emit_value(&hc, &fn, r->k);
    Type vtype = r->value ? r->value->type : (Type){0};
    bool v_is_nil = (vtype.kind == TY_NIL || vtype.kind == TY_NEVER);
    char *v_arg;
    if (v_is_nil) {
        v_arg = strdup("(intptr_t)0");
    } else {
        char *vv = emit_value(&hc, &fn, r->value);
        char *sv = eff_slot_store(vtype, vv);
        Buf vb; buf_init(&vb);
        buf_printf(&vb, "(intptr_t)(%s)", sv);
        buf_putc(&vb, '\0');
        v_arg = strdup(vb.data);
        buf_free(&vb); free(sv); free(vv);
    }
    buf_printf(&fn, "    ((struct DK *)(intptr_t)%s)->consumed = 1;\n", kv);
    buf_printf(&fn, "    return dk_tail_resume((struct DK *)(intptr_t)%s, %s);\n", kv, v_arg);
    free(kv); free(v_arg);
    ctx->tmp_n = hc.tmp_n;
    if (pend.len > 0) buf_write(hbuf, pend.data, pend.len);
    buf_free(&pend);
    buf_printf(hbuf, "static intptr_t %s(intptr_t __dkenv, intptr_t __dkarg, struct DK *__dksubk);\n", dkfn_name);
    buf_printf(hbuf, "static intptr_t %s(intptr_t __dkenv, intptr_t __dkarg, struct DK *__dksubk) {\n", dkfn_name);
    buf_write(hbuf, fn.data, fn.len);
    buf_puts(hbuf, "}\n\n");
    buf_free(&fn);
    return true;
}

/* FH2: emit a handler literal as a runtime tur_handler_table_t* value.
 * Emits the single case as a static __effect_handler_<id> function (same ABI
 * and capture-via-__env scheme as emit_effects_handle), heap-allocates the
 * capture env at the creation site, and builds a one-entry owning table. */
char *emit_effects_handler_lit(EmitCtx *ctx, Buf *body, const Expr *e) {
    HandleExpr *h = e->as.handler_lit_.handle;
    HandleCase *c = &h->cases[0];
    int id = ctx->tmp_n++;
    Buf *hbuf = ctx->pending_handler_fns;

    /* Collect captures from the case body, excluding k + effect params. */
    uint32_t n_caps = 0;
    Binding **caps = collect_handle_captures(c->body, &n_caps);
    uint32_t filtered = 0;
    for (uint32_t j = 0; j < n_caps; j++) {
        Binding *b = caps[j];
        bool is_local = (c->k_binding && b == c->k_binding);
        for (uint32_t p = 0; p < c->n_params && !is_local; p++)
            if (c->param_bindings && c->param_bindings[p] == b) is_local = true;
        if (!is_local) caps[filtered++] = b;
    }
    n_caps = filtered;
    bool has_caps = (n_caps > 0);

    char env_type[64], hfn_name[64], env_var[64];
    snprintf(env_type, sizeof(env_type), "__HLEnv_%d", id);
    snprintf(hfn_name, sizeof(hfn_name), "__effect_handler_%d", id);
    snprintf(env_var, sizeof(env_var), "__hlenv_%d", id);

    if (has_caps) {
        buf_printf(hbuf, "typedef struct %s %s;\n", env_type, env_type);
        buf_printf(hbuf, "struct %s {\n", env_type);
        for (uint32_t j = 0; j < n_caps; j++) {
            Binding *b = caps[j];
            char *raw = raw_name_for_binding(b);
            const char *ft = b->is_poly_fn ? "tur_poly_fn_t" : type_c_name(b->type);
            buf_printf(hbuf, "    %s %s;\n", ft, raw);
            free(raw);
        }
        buf_printf(hbuf, "};\n\n");
    }

    /* Emit the case function (mirrors emit_effects_handle Step 4).  Buffer it into
     * `fiberfn` rather than hbuf directly: the fiber `__effect_handler_N` (whose
     * body direct-emits the case's `resume` as `tur_effect_cont_resume`) is DEAD
     * when a DK case fn is also emitted (dk_hgroup_from_table installs via `dk_fn`
     * and never reads `.fn`; with the flag always-on every perform DK-lowers).  It
     * is appended to hbuf only when the DK case is NOT emittable, so a DK-lowered
     * handler value no longer drags the fiber `tur_effect_cont_resume` runtime into
     * the emitted C.  Buffering (not reordering) keeps tmp_n allocation identical. */
    Buf fiberfn; buf_init(&fiberfn);
    {
        Buf fn; buf_init(&fn);
        Buf pend; buf_init(&pend);
        if (has_caps)
            buf_printf(&fn, "    %s *%s = (%s *)__env;\n", env_type, env_var, env_type);
        for (uint32_t j = 0; j < c->n_params; j++) {
            if (c->param_bindings && c->param_bindings[j]) {
                const char *ct = type_c_name(c->param_bindings[j]->type);
                char *raw = raw_name_for_binding(c->param_bindings[j]);
                /* Unpack from the int64 slot with the perform-side inverse
                 * (float bit-reinterpret / aggregate unbox / plain cast). */
                char src[48];
                snprintf(src, sizeof(src), "__effect_args[%d]", j);
                char *ld = eff_slot_load(c->param_bindings[j]->type, src);
                /* Fn-value payload: declare with the raw id-less name to match the
                 * TY_FN-non-boxed reference path (see the multi-case site above). */
                bool fnval = c->param_bindings[j]->type.kind == TY_FN
                             && !c->param_bindings[j]->type.as.fn.boxed;
                if (fnval)
                    buf_printf(&fn, "    %s %s = %s;\n", ct, raw, ld);
                else
                    buf_printf(&fn, "    %s %s_%u = %s;\n",
                               ct, raw, (unsigned)c->param_bindings[j]->id, ld);
                free(ld);
                free(raw);
            }
        }
        if (c->k_binding) {
            char *raw = raw_name_for_binding(c->k_binding);
            buf_printf(&fn, "    int64_t %s_%u = __k;\n", raw, (unsigned)c->k_binding->id);
            free(raw);
        }
        EmitCtx hc = *ctx;
        hc.file = &fn; hc.pending_handler_fns = &pend; hc.indent = 4;
        hc.fn_params = NULL; hc.n_fn_params = 0; hc.closure = NULL;
        hc.env_var_name = has_caps ? env_var : NULL;
        hc.defer_captures = NULL; hc.n_defer_captures = 0; hc.frame_var = NULL;
        hc.return_emitted = false;
        hc.handle_captures = has_caps ? caps : NULL;
        hc.n_handle_captures = has_caps ? n_caps : 0;
        hc.handle_env_name = has_caps ? env_var : NULL;
        /* byval-spine-drop-past-early-exit: same reset as above. */
        hc.current_fn_spine_drop_safe = false;
        if (!c->body) {
            buf_puts(&fn, "    return 0;\n");
        } else if (c->body->type.kind == TY_NEVER) {
            emit_stmt(&hc, &fn, c->body);
            buf_puts(&fn, "    return 0; /* unreachable */\n");
        } else {
            char *hret = emit_value(&hc, &fn, c->body);
            if (strcmp(hret, "((void)0)") == 0) {
                free(hret);
                buf_puts(&fn, "    return 0;\n");
            } else {
                /* Pack the handler result into the int64 slot (float
                 * bit-reinterpret / aggregate box / plain cast). */
                char *sv = eff_slot_store(c->body->type, hret);
                buf_printf(&fn, "    return %s;\n", sv);
                free(sv);
                free(hret);
            }
        }
        ctx->tmp_n = hc.tmp_n;
        if (pend.len > 0) buf_write(&fiberfn, pend.data, pend.len);
        buf_free(&pend);
        buf_printf(&fiberfn, "static int64_t %s(int64_t *__effect_args, int __n_effect_args,"
                         " int64_t __k, void *__env);\n", hfn_name);
        buf_printf(&fiberfn, "static int64_t %s(int64_t *__effect_args, int __n_effect_args,"
                         " int64_t __k, void *__env) {\n", hfn_name);
        buf_write(&fiberfn, fn.data, fn.len);
        buf_puts(&fiberfn, "}\n\n");
        buf_free(&fn);
    }

    /* B3 part 2: also emit a DK-ABI case fn (when the case shape allows) so a
     * dynamic with-handler over this value can install it on the DK. */
    char dkfn_name[64];
    snprintf(dkfn_name, sizeof(dkfn_name), "__dk_hcase_%d", id);
    bool dk_emitted = emit_dk_handler_case_fn(ctx, hbuf, dkfn_name, c, env_type,
                                              env_var, has_caps, caps, n_caps);
    /* Fiber case fn is DEAD once the DK case is emitted -- append it only as the
     * non-DK fallback, so `tur_effect_cont_resume` leaves the DK-lowered output. */
    if (!dk_emitted && fiberfn.len > 0) buf_write(hbuf, fiberfn.data, fiberfn.len);
    buf_free(&fiberfn);

    /* Inline: heap-alloc the capture env, build a one-entry owning table. */
    char env_inl[64];
    snprintf(env_inl, sizeof(env_inl), "__hlenv_inl_%d", id);
    if (has_caps) {
        indent_buf(body, ctx->indent);
        buf_printf(body, "%s *%s = (%s *)malloc(sizeof(%s));\n",
                   env_type, env_inl, env_type, env_type);
        for (uint32_t j = 0; j < n_caps; j++) {
            Binding *b = caps[j];
            char *raw = raw_name_for_binding(b);
            char *cur = name_for_binding(ctx, b);
            indent_buf(body, ctx->indent);
            buf_printf(body, "%s->%s = %s;\n", env_inl, raw, cur);
            free(raw); free(cur);
        }
    }
    char *tbl = fresh_tmp(ctx);
    indent_buf(body, ctx->indent);
    buf_printf(body, "tur_handler_table_t *%s = tur_handler_table_new(1);\n", tbl);
    indent_buf(body, ctx->indent);
    buf_printf(body, "%s->entries[0].eff_name = \"%s\";\n", tbl, c->effect_name->name);
    indent_buf(body, ctx->indent);
    /* fn is the fiber case entry; when the DK case is present it is dead (and no
     * longer emitted), so store NULL -- the DK install path reads only dk_fn. */
    buf_printf(body, "%s->entries[0].fn = %s;\n", tbl, dk_emitted ? "NULL" : hfn_name);
    indent_buf(body, ctx->indent);
    buf_printf(body, "%s->entries[0].env = %s;\n", tbl, has_caps ? env_inl : "NULL");
    indent_buf(body, ctx->indent);
    buf_printf(body, "%s->entries[0].cont_kind = %d;\n", tbl, (int)c->cont_kind);
    if (dk_emitted) {
        indent_buf(body, ctx->indent);
        buf_printf(body, "%s->entries[0].dk_tag = %d;\n", tbl, effect_tag(c->effect_name));
        indent_buf(body, ctx->indent);
        buf_printf(body, "%s->entries[0].dk_fn = %s;\n", tbl, dkfn_name);
    }
    free(caps);
    return tbl;
}

/* FH5: emit (compose-handlers h1 h2) as table concatenation (h1 outer). */
char *emit_effects_compose_handlers(EmitCtx *ctx, Buf *body, const Expr *e) {
    char *a = emit_value(ctx, body, e->as.compose_handlers_.h1);
    char *b = emit_value(ctx, body, e->as.compose_handlers_.h2);
    char *t = fresh_tmp(ctx);
    indent_buf(body, ctx->indent);
    buf_printf(body, "tur_handler_table_t *%s = tur_handler_table_concat(%s, %s);\n", t, a, b);
    free(a); free(b);
    return t;
}

/* FH3: emit (with-handler hv body) -- run body in a fiber that dispatches
 * performed effects against hv's runtime table via the generic
 * tur_handler_dispatch.  Mirrors emit_effects_handle Steps 5-7 with the table
 * supplied at runtime instead of compile-time inline cases. */

char *emit_effects_resume(EmitCtx *ctx, Buf *body, const Expr *e) {
    /* (resume k value) - resume continuation with value.
     *
     * MS1: CK_MULTISHOT k is a tur_cloneable_cont* (int64_t).
     * tur_cloneable_cont_resume(tur_continuation_snapshot(k), v) clones before each resume.
     *
     * Any other k is a DK chain word, resumed with dk_invoke (below).
     */
    if (e->as.resume_.resume) {
        char *k_var = emit_value(ctx, body, e->as.resume_.resume->k);
        char *v_var = emit_value(ctx, body, e->as.resume_.resume->value);
        Type k_type = e->as.resume_.resume->k->type;
        if (k_type.copy_kind == CK_MULTISHOT) {
            /* MS1: ^multishot k -- snapshot before each resume so k stays usable. */
            char *tmp = fresh_tmp(ctx);
            char *snap = fresh_tmp(ctx);
            Type vtype = e->as.resume_.resume->value->type;
            bool v_is_nil = (vtype.kind == TY_NIL || vtype.kind == TY_NEVER);
            char *v_arg = v_is_nil ? strdup("(int64_t)0")
                                   : eff_slot_store(vtype, v_var);
            /* Snapshot -> resume -> drop.  tur_continuation_snapshot clones the
             * cont (a fresh struct + a deep clone of its env); the resume runs
             * cont_fn on that clone but never reclaims it (cont_fn = __dk_cont_fn
             * = dk_invoke, which dk_copy_range's the chain and frees only its own
             * internal copy -- the snapshot's env is left caller-owned).  So the
             * per-resume snapshot (struct + env) leaked one clone per `(k v)`.
             * Drop it right after the resume returns: tur_cloneable_cont_drop
             * fires drop_env (dk_free of the chain clone) then frees the struct --
             * safe because the resume's dk_invoke never took ownership of the env,
             * and the returned result is a plain value, not the snapshot.  The
             * original k is untouched (this frees the clone, not k), so a
             * subsequent resume of k still works (multi-shot).
             * (docs/archive/cps-resume-frame-node-leak.md, snapshot sibling.) */
            indent_buf(body, ctx->indent);
            buf_printf(body, "int64_t %s = tur_continuation_snapshot(%s);\n", snap, k_var);
            /* The resume returns what the handle delivers in the int64 slot;
             * load it as the resume's real type (a float by its bits). */
            char *raw = fresh_tmp(ctx);
            indent_buf(body, ctx->indent);
            buf_printf(body, "int64_t %s = tur_cloneable_cont_resume(%s, %s);\n", raw, snap, v_arg);
            indent_buf(body, ctx->indent);
            buf_printf(body, "tur_cloneable_cont_drop(%s);\n", snap);
            indent_buf(body, ctx->indent);
            if (e->type.kind == TY_NIL || e->type.kind == TY_NEVER) {
                buf_printf(body, "int64_t %s = %s;\n", tmp, raw);
            } else {
                char *ld = eff_slot_load(e->type, raw);
                buf_printf(body, "%s %s = %s;\n", type_c_name(e->type), tmp, ld);
                free(ld);
            }
            free(raw);
            free(snap);
            free(v_arg);
            free(k_var);
            free(v_var);
            return tmp;
        }
        /* Every other k is a handler continuation word: the DK chain
         * dk_perform handed the case (`k = (int64_t)subk`), resumed here
         * outside the case that bound it -- a helper the case passed it to
         * (`(defn helper [k v] (resume k v))`), a cell it was stored in, or an
         * `effect-cont` receiver.  Resume it as the CPS backend's emit_resume
         * does any k that is not the case's own: dk_invoke runs the captured
         * chain to the handle and returns what the handle delivers.
         *
         * This arm used to be two wrong ones (resume-of-cont-typed-param-is-a-
         * no-op): a TY_INT k called tur_effect_cont_resume, deleted with the
         * fiber effect runtime (2026-07-19), so it failed to link; any other
         * k -- a `cont<int>` annotation is a type variable -- returned the
         * resume VALUE and never ran the continuation, a silent wrong answer.
         * A cloneable / escape / serial `cont` never gets here: elab_resume
         * refuses `resume` on one (it is resumed by calling it, `(k v)`). */
        Type vtype = e->as.resume_.resume->value->type;
        bool v_is_nil = (vtype.kind == TY_NIL || vtype.kind == TY_NEVER);
        char *v_arg;
        if (v_is_nil) {
            v_arg = strdup("(int64_t)0");
        } else {
            char *st = eff_slot_store(vtype, v_var);
            /* A boxed aggregate is read back without freeing (the resumed
             * chain may be re-run), so the per-run reap list owns the box --
             * as slot_store_reap does on the CPS backend. */
            if (eff_type_boxed(vtype)) {
                Buf rb; buf_init(&rb);
                buf_printf(&rb, "(int64_t)__dk_reap_ptr((intptr_t)%s)", st);
                buf_putc(&rb, '\0');
                v_arg = strdup(rb.data);
                buf_free(&rb);
                free(st);
            } else {
                v_arg = st;
            }
        }
        /* __dk_resume_word (emit_dk_runtime.c) also takes the word a
         * `^multishot` / fn-payload case wraps its chain in. */
        Buf rcall; buf_init(&rcall);
        buf_printf(&rcall, "__dk_resume_word((intptr_t)(%s), (intptr_t)(%s))", k_var, v_arg);
        buf_putc(&rcall, '\0');
        char *tmp = fresh_tmp(ctx);
        indent_buf(body, ctx->indent);
        if (e->type.kind == TY_NIL || e->type.kind == TY_NEVER) {
            buf_printf(body, "int64_t %s = (int64_t)%s;\n", tmp, rcall.data);
        } else {
            char *ld = eff_slot_load(e->type, rcall.data);
            buf_printf(body, "%s %s = %s;\n", type_c_name(e->type), tmp, ld);
            free(ld);
        }
        buf_free(&rcall);
        /* The resumed chain may have panicked under a handler and signalled
         * back through dk_invoke (cps-body-panic-not-propagated). */
        emit_panic_signal_return(ctx, body);
        free(v_arg);
        free(k_var);
        free(v_var);
        return tmp;
    }
    return atom_nil();
}

char *emit_effects_discontinue(EmitCtx *ctx, Buf *body, const Expr *e) {
    /* (discontinue k exception): the fiber lowering that marked the fiber done
     * and abort()ed is gone, and the CPS backend has no rule for it
     * (`BODY-UNSUPPORTED EX_DISCONTINUE`), so every function holding one is
     * evicted and the build used to fail with a message about the `perform`.
     * Name the form that is the cause. */
    (void)ctx; (void)body;
    /* discontinue-plan: with the experiment on, the CPS backend lowers it
     * (a raising CT_RESUME), so reaching the direct emitter means the
     * enclosing function was evicted -- most often for a catch-unwind inside
     * a handler clause, which a case body cannot hold yet. */
    if (g_opt_discontinue) {
        diag_emit(DIAG_ERROR, e->span,
                  "this `discontinue` has no lowering here: the enclosing function "
                  "left the CPS backend's supported subset (a `catch-unwind` inside "
                  "a handler clause is the usual cause; it cannot be lowered yet). "
                  "Catch around the `handle` instead, or run with TUR_TRACE_EVICT=1 "
                  "to see which form evicted the function.");
        return atom_nil();
    }
    diag_emit(DIAG_ERROR, e->span,
              "`discontinue` has no lowering in the compiled backend yet: the "
              "CPS backend cannot raise at the `perform`. To end the handled "
              "body early, leave the clause without resuming `k`; for a failure "
              "the caller sees, return a `Result` from the clause. "
              "(The interpreter does not run `discontinue` either.) "
              "--enable=discontinue lowers it (prototype).");
    return atom_nil();
}

/* =========================================================================
 * Region A -- delimited / cloneable / serial continuations
 * ========================================================================= */

/* evicted-reset-shift-degrades-to-body-eval: does `e` hold a base shift or
 * shift0 the direct emitter would lower in place -- lexically, outside a
 * nested function body?  (A shift under a nested reset counts too: that
 * reset installs its own landing, and an extra one here costs one setjmp.) */
static bool reset_body_has_base_shift(const Expr *e) {
    if (!e) return false;
    switch (e->kind) {
        case EX_SHIFT: case EX_SHIFT0:
            return true;
        case EX_RESET:
            return reset_body_has_base_shift(e->as.reset_.body);
        case EX_LET: case EX_LETREC:
            for (uint32_t i = 0; i < e->as.let_.n; i++)
                if (reset_body_has_base_shift(e->as.let_.bindings[i].init)) return true;
            return reset_body_has_base_shift(e->as.let_.body);
        case EX_IF:
            return reset_body_has_base_shift(e->as.if_.cond) ||
                   reset_body_has_base_shift(e->as.if_.then_) ||
                   reset_body_has_base_shift(e->as.if_.else_or_null);
        case EX_DO:
            for (uint32_t i = 0; i < e->as.do_.n; i++)
                if (reset_body_has_base_shift(e->as.do_.items[i])) return true;
            return false;
        case EX_WHILE:
            return reset_body_has_base_shift(e->as.while_.cond) ||
                   reset_body_has_base_shift(e->as.while_.body);
        case EX_SET:
            return reset_body_has_base_shift(e->as.set_.value);
        case EX_BUILTIN:
            for (uint32_t i = 0; i < e->as.builtin.n; i++)
                if (reset_body_has_base_shift(e->as.builtin.args[i])) return true;
            return false;
        case EX_CALL:
            for (uint32_t i = 0; i < e->as.call_.n_args; i++)
                if (reset_body_has_base_shift(e->as.call_.args[i])) return true;
            return false;
        case EX_ASCRIBE:
            return reset_body_has_base_shift(e->as.ascribe_.inner);
        case EX_CAST:
            return reset_body_has_base_shift(e->as.cast_.expr);
        default:
            /* A form this does not walk: the CPS translation's own scan
             * (cps_expr_contains_shift) is the conservative answer. */
            return e->kind != EX_FN && e->kind != EX_CLOSURE &&
                   e->kind != EX_FN_DEF && cps_expr_contains_shift(e);
    }
}

char *emit_effects_reset(EmitCtx *ctx, Buf *body, const Expr *e) {
    /* (reset body) - establish a continuation boundary and run body.
     *
     * cps-backend-direct-lowering-removal D3 deleted the direct emitter's
     * reset lowering (emit_cps.c) on the reading that every reset whose body
     * can reach a shift is colored and emitted by the CT-IR backend.  It is,
     * until its function is EVICTED (a parameter `fn_sig_ok` refuses, an
     * export, an inline-C body), and then it lands here.  Evaluating the body
     * made the shift's receiver an ordinary call whose value flowed back into
     * the context: `(reset (+ 1 (shift twice x)))` was 21, not 20
     * (evicted-reset-shift-degrades-to-body-eval).
     *
     * So a reset whose body holds a base shift gets the escape landing back.
     * A base shift is abortive: it computes the receiver's value, stores it on
     * the tur_cur_shift_reset stack and longjmps here, discarding the context
     * from wherever it is, a branch included.  The thread-local stack makes
     * the innermost reset the target; `prev` restores the enclosing one.
     *
     * The value rides the context's int64 slot, so the result must be a
     * scalar of at most a word.  A reset of any other type is refused with a
     * located error rather than computed wrongly.  A reset with no shift in
     * reach just evaluates its body, as before. */
    const Expr *rb = e->as.reset_.body;
    if (!reset_body_has_base_shift(rb)) return emit_value(ctx, body, rb);
    const char *ct = emit_type_c_name(ctx, emit_resolve_type(ctx, e->type));
    if (!type_kind_is_order_spillable(e->type.kind) || !ct) {
        diag_emit(DIAG_ERROR, e->span,
                  "this `reset` has no lowering here: its function left the "
                  "CPS backend's supported subset, and the direct emitter "
                  "carries an abortive `shift`'s value back to its `reset` "
                  "only for a scalar result (int, float, bool, cstr, a "
                  "pointer). Run with TUR_TRACE_EVICT=1 to see which form "
                  "evicted the function; a signature the backend accepts, or a "
                  "scalar result, avoids it. This is a compiler limitation, "
                  "not a mistake in this expression.");
        char *tmp = fresh_tmp(ctx);
        indent_buf(body, ctx->indent);
        buf_printf(body, "int64_t %s = 0; /* reset: no lowering (error above) */\n", tmp);
        return tmp;
    }
    char *rs  = fresh_tmp(ctx);
    char *res = fresh_tmp(ctx);
    indent_buf(body, ctx->indent);
    buf_printf(body, "tur_shift_reset_ctx %s;\n", rs);
    indent_buf(body, ctx->indent);
    buf_printf(body, "%s %s;\n", ct, res);
    indent_buf(body, ctx->indent);
    buf_printf(body, "%s.prev = tur_cur_shift_reset;\n", rs);
    indent_buf(body, ctx->indent);
    buf_printf(body, "tur_cur_shift_reset = &%s;\n", rs);
    indent_buf(body, ctx->indent);
    buf_printf(body, "if (TUR_SETJMP(%s.buf) == 0) {\n", rs);
    ctx->indent += 4;
    bool saved_escape = ctx->in_shift_escape;
    const char *saved_ct = ctx->shift_escape_ctype;
    ctx->in_shift_escape = true;
    ctx->shift_escape_ctype = ct;
    char *v = emit_value(ctx, body, rb);
    ctx->in_shift_escape = saved_escape;
    ctx->shift_escape_ctype = saved_ct;
    indent_buf(body, ctx->indent);
    buf_printf(body, "%s = %s;\n", res, v);
    free(v);
    ctx->indent -= 4;
    indent_buf(body, ctx->indent);
    buf_puts(body, "} else {\n");
    /* Read through the stack's head: it is memory, unlike a local the
     * longjmp could have left stale in a register. */
    indent_buf(body, ctx->indent + 4);
    buf_printf(body, "memcpy(&%s, &tur_cur_shift_reset->result, sizeof %s);\n", res, res);
    indent_buf(body, ctx->indent);
    buf_puts(body, "}\n");
    indent_buf(body, ctx->indent);
    buf_printf(body, "tur_cur_shift_reset = %s.prev;\n", rs);
    emit_localvar_record_ctype(res, ct);
    free(rs);
    return res;
}

char *emit_effects_cloneable_reset(EmitCtx *ctx, Buf *body, const Expr *e) {
    /* (cloneable-reset body) - evaluate body within a cloneable reset boundary.
     *
     * CPS-CL3 + CPS-CL5: When the body IS a cloneable-shift (Case 1), use the
     * full CPS path: emit a trivial __cont_fn, alloc a tur_cloneable_cont, and
     * call k_fn with the continuation.  The reset's value = k_fn's return value.
     *
     * Fallback: body contains no shift -- just evaluate and return body value.
     */
    const Expr *rb = e->as.cloneable_reset_.body;

    /* cps-backend-direct-lowering-removal D3: the CPS lowering of a
     * cloneable-reset (emit_cps_cloneable_reset) is deleted. A cloneable-reset
     * wrapping a cloneable-shift in a supported delimited context is now colored
     * and emitted natively by the CT-IR backend, so it never reaches this
     * wrapper. What remains here is the legacy Case-1 lowering (shift is the
     * entire reset body) plus the no-shift fallback. */
    if (rb->kind == EX_CLONEABLE_SHIFT) {
        /* Full CPS: shift is the entire reset body (Case 1 -- trivial continuation). */
        const Expr *shift = rb;
        int cont_id = ctx->tmp_n++;

        /* CPS-CL3: Emit trivial continuation function to pending_handler_fns.
         * Appears at file scope before the enclosing function definition. */
        if (ctx->pending_handler_fns) {
            buf_printf(ctx->pending_handler_fns,
                "static int64_t __cont_fn_%d(void *__env, int64_t __value) {"
                " (void)__env; return __value; }\n\n", cont_id);
        }

        /* CPS-CL2: Emit env struct + alloc if there are live captures.
         * (For the trivial continuation the env is unused; emit placeholder.) */
        char *env_val_str  = NULL;   /* NULL -> "NULL" in format */
        char *clone_fn_str = NULL;
        char *drop_fn_str  = NULL;
        char *env_var = NULL;
        /* CPS-CL2 / D6b: the Case-1 continuation is the trivial identity
         * (`return __value`, ignoring its env), so it never carries a captured
         * env -- allocate the cont with a NULL env below.  (The always-dead
         * has_env env-struct emission -- has_env was hardcoded false, since the
         * live_captures list may name the not-yet-declared shift-result binding
         * and the trivial cont ignores it anyway -- was removed here.) */

        /* CPS-CL5: Alloc the continuation */
        char *cont_var = fresh_tmp(ctx);
        indent_buf(body, ctx->indent);
        buf_printf(body,
            "tur_cloneable_cont *%s = tur_cloneable_cont_alloc("
            "__cont_fn_%d, %s, %s, %s);\n",
            cont_var, cont_id,
            env_val_str  ? env_val_str  : "NULL",
            clone_fn_str ? clone_fn_str : "NULL",
            drop_fn_str  ? drop_fn_str  : "NULL");

        /* CPS-CL5: Evaluate k_fn, then call k_fn(cont_as_int64) */
        char *k_fn_val = emit_value(ctx, body, shift->as.cloneable_shift_.k_fn);
        char *result   = fresh_tmp(ctx);
        indent_buf(body, ctx->indent);
        if (shift->as.cloneable_shift_.k_fn->kind == EX_CLOSURE) {
            struct Closure *closure =
                shift->as.cloneable_shift_.k_fn->as.closure_.closure;
            char *thunk_name;
            if (closure->fn->binding) {
                thunk_name = raw_name_for_binding(closure->fn->binding);
            } else {
                thunk_name = malloc(64);
                snprintf(thunk_name, 64, "__fn_anon_%d",
                         closure->fn->n_params > 0
                             ? closure->fn->params[0]->id : 0);
            }
            buf_printf(body, "%s %s = %s(%s, (int64_t)(intptr_t)%s);\n",
                       type_c_name(e->type), result,
                       thunk_name, k_fn_val, cont_var);
            free(thunk_name);
        } else {
            buf_printf(body, "%s %s = %s((int64_t)(intptr_t)%s);\n",
                       type_c_name(e->type), result, k_fn_val, cont_var);
        }
        free(cont_var);
        free(k_fn_val);
        if (env_var)      free(env_var);
        if (clone_fn_str) free(clone_fn_str);
        if (drop_fn_str)  free(drop_fn_str);
        return result;
    }
    /* CPS-CL4 Case 2: body contains cloneable-shift somewhere inside
     * (not as the direct body).
     *
     * cloneable-shift-unsupported-context-miscompile (D6a): reaching this point
     * means the cloneable-reset could not lower its context-bearing shift onto
     * the native DK machine (build_cloneable rejected the shape) and evicted to
     * the direct emitter.  The historical legacy lowering here was a setjmp
     * boundary whose captured continuation was the IDENTITY -- it silently
     * DROPPED the delimited context, so `(+ (compute) (cloneable-shift ...))`
     * returned the wrong value with no error (see
     * docs/archive/history/cloneable-shift-nonatomic-operand-context-dropped.md).  The
     * common case (a non-atomic pure arithmetic operand) now lowers natively;
     * any residual shape is rejected here with a real diagnostic instead of the
     * silent miscompile, mirroring the serial TUR-E0706 path. */
    if (cps_expr_contains_cloneable_shift(rb)) {
        diag_emit_with_code(DIAG_ERROR, e->span,
                            TUR_E0710_CLONEABLE_CONTEXT_NOT_CAPTURABLE,
                            "cloneable-shift context is not capturable\n"
                            "  = note: the delimited context falls outside the "
                            "native build_cloneable grammar, so the continuation "
                            "cannot be reified into a multi-shot cloneable cont\n"
                            "  = help: restructure into a supported shape (scalar "
                            "let prelude / arithmetic with an atomic or pure "
                            "operand / 1- or 2-arg call / if), or move the "
                            "non-capturable work outside the cloneable-reset; run "
                            "`tur explain TUR-E0710` for details");
        /* Typed placeholder so downstream codegen does not crash; the C output is
         * discarded because diag_had_error() now fails the build. */
        char *tmp = fresh_tmp(ctx);
        indent_buf(body, ctx->indent);
        buf_printf(body, "%s %s = 0; /* cloneable-shift: rejected (TUR-E0710) */\n",
                   type_c_name(e->type), tmp);
        return tmp;
    }

    /* Fallback: no shift in body -- just evaluate and return */
    return emit_value(ctx, body, rb);
}

/* direct-reset-shift-degrades fix: when a base shift/shift0 is emitted inside a
 * setjmp-lowered reset (ctx->in_shift_escape), it is abortive -- deliver the
 * computed abort value `result` (= f(operand)) to the innermost reset landing
 * and longjmp there, discarding the delimited continuation from wherever the
 * shift sits (including inside an `if` branch).  Outside escape mode this is a
 * no-op and the shift's value flows on as before (the DK abort-value model, or
 * the legacy degrade). */
static void emit_shift_escape_abort(EmitCtx *ctx, Buf *body, const char *result) {
    if (!ctx->in_shift_escape || !ctx->shift_escape_ctype) return;
    /* The value is copied at the reset's own C type, bit for bit: a cast
     * through int64_t would convert a double's value.  Guarded on the stack
     * head, so a shift reached with no landing live keeps its old value. */
    indent_buf(body, ctx->indent);
    buf_printf(body, "if (tur_cur_shift_reset) {\n");
    indent_buf(body, ctx->indent + 4);
    buf_printf(body, "%s __sv = (%s)(%s);\n",
               ctx->shift_escape_ctype, ctx->shift_escape_ctype, result);
    indent_buf(body, ctx->indent + 4);
    buf_puts(body, "memcpy(&tur_cur_shift_reset->result, &__sv, sizeof __sv);\n");
    indent_buf(body, ctx->indent + 4);
    buf_puts(body, "TUR_LONGJMP(tur_cur_shift_reset->buf);\n");
    indent_buf(body, ctx->indent);
    buf_puts(body, "}\n");
}

char *emit_effects_shift(EmitCtx *ctx, Buf *body, const Expr *e) {
    /* (shift k body) - shift with continuation handler k and value body
     *
     * Semantics: evaluate body to get value v, call k(v), return result.
     * Note: Without CPS, we can't capture the continuation, so this is a
     * simplified version that just calls k with body's value.
     * Full implementation requires CPS transformation.
     */
    char *body_val = emit_value(ctx, body, e->as.shift_.body);
    char *result = fresh_tmp(ctx);

    /* Emit the call: k_fn(body_val) */
    char *k_fn = emit_value(ctx, body, e->as.shift_.k_fn);

    /* Check if this is a closure call by looking at the expression */
    if (e->as.shift_.k_fn->kind == EX_CLOSURE) {
        /* For closures, k_fn is the env pointer, need to call thunk */
        struct Closure *closure = e->as.shift_.k_fn->as.closure_.closure;
        /* The thunk function name is based on the closure's function binding */
        char *thunk_name = (char *)malloc(64);
        if (closure->fn->binding) {
            snprintf(thunk_name, 64, "%s", closure->fn->binding->name->name);
        } else {
            /* Anonymous function - this shouldn't happen for closures */
            snprintf(thunk_name, 64, "__fn_anon_%d", closure->fn->params[0]->id);
        }
        indent_buf(body, ctx->indent);
        buf_printf(body, "%s %s = %s(%s, %s);\n",
                  type_c_name(e->type), result, thunk_name, k_fn, body_val);
        free(thunk_name);
    } else {
        /* Regular function call */
        indent_buf(body, ctx->indent);
        buf_printf(body, "%s %s = %s(%s);\n",
                  type_c_name(e->type), result, k_fn, body_val);
    }
    free(k_fn);
    free(body_val);
    emit_shift_escape_abort(ctx, body, result);
    return result;
}

char *emit_effects_shift0(EmitCtx *ctx, Buf *body, const Expr *e) {
    /* (shift0 k body) - one-shot shift
     * Similar to shift but k cannot resume the continuation.
     * For now, same implementation as shift.
     */
    char *body_val = emit_value(ctx, body, e->as.shift0_.body);
    char *result = fresh_tmp(ctx);

    char *k_fn = emit_value(ctx, body, e->as.shift0_.k_fn);

    /* Check if this is a closure call by looking at the expression */
    if (e->as.shift0_.k_fn->kind == EX_CLOSURE) {
        /* For closures, k_fn is the env pointer, need to call thunk */
        struct Closure *closure = e->as.shift0_.k_fn->as.closure_.closure;
        char *thunk_name = (char *)malloc(64);
        if (closure->fn->binding) {
            snprintf(thunk_name, 64, "%s", closure->fn->binding->name->name);
        } else {
            snprintf(thunk_name, 64, "__fn_anon_%d", closure->fn->params[0]->id);
        }
        indent_buf(body, ctx->indent);
        buf_printf(body, "%s %s = %s(%s, %s);\n",
                  type_c_name(e->type), result, thunk_name, k_fn, body_val);
        free(thunk_name);
    } else {
        /* Regular function call */
        indent_buf(body, ctx->indent);
        buf_printf(body, "%s %s = %s(%s);\n",
                  type_c_name(e->type), result, k_fn, body_val);
    }
    free(k_fn);
    free(body_val);
    emit_shift_escape_abort(ctx, body, result);
    return result;
}

char *emit_effects_cloneable_shift(EmitCtx *ctx, Buf *body, const Expr *e) {
    /* cps-backend-direct-lowering-removal D6b: the legacy standalone cloneable-
     * shift emission -- a setjmp/longjmp cooperation with the old
     * emit_effects_cloneable_reset Case-2 -- is deleted.  D6a replaced Case-2
     * with a TUR-E0710 diagnostic (a context-bearing cloneable shift outside the
     * native build_cloneable subset is rejected, not silently miscompiled), so
     * this emitter's longjmp landing (tur_current_reset_ctx) no longer exists and
     * the function is unreachable in the corpus: a cloneable-shift is always
     * either the whole reset body (Case-1, native-first) or inside a context
     * Case-2 already rejected.  Reaching here means a bare cloneable-shift escaped
     * its reset -- reject it cleanly rather than longjmp to a landing that is no
     * longer established. */
    diag_emit_with_code(DIAG_ERROR, e->span,
                        TUR_E0710_CLONEABLE_CONTEXT_NOT_CAPTURABLE,
                        "cloneable-shift is not in a capturable reset context\n"
                        "  = note: a cloneable-shift must sit inside a "
                        "cloneable-reset whose delimited context the native "
                        "backend can reify; run `tur explain TUR-E0710` for details");
    char *tmp = fresh_tmp(ctx);
    indent_buf(body, ctx->indent);
    buf_printf(body, "%s %s = 0; /* cloneable-shift: rejected (TUR-E0710) */\n",
               type_c_name(e->type), tmp);
    return tmp;
}

char *emit_effects_serial_reset(EmitCtx *ctx, Buf *body, const Expr *e) {
    /* cps-backend-direct-lowering-removal D3: the CPS lowering of a serial-reset
     * (emit_cps_serial_reset) is deleted. A serial-reset wrapping a serial-shift
     * in a supported delimited context is now colored and emitted natively by
     * the CT-IR backend, so it never reaches this wrapper. What remains is the
     * sole surviving behavior: a serial-reset with no reachable shift just
     * evaluates its body. */
    return emit_value(ctx, body, e->as.serial_reset_.body);
}

/* serial-shift-colored-receiver-rejected: the one receiver the native backend
 * still refuses is a function an effect ESCAPES -- a non-empty declared or
 * inferred row.  It is called from the shift body through its direct entry,
 * whose fresh DK root cannot see the handler enclosing the reset.  Returns
 * that function's FnDef so the diagnostic can say so instead of blaming the
 * context shape; NULL for any other rejection. */
static const FnDef *serial_receiver_with_escaping_effect(EmitCtx *ctx, const Expr *e) {
    const Expr *kf = e->as.serial_shift_.k_fn;
    while (kf && kf->kind == EX_ASCRIBE) kf = kf->as.ascribe_.inner;
    const Binding *rb = NULL;
    if (kf && kf->kind == EX_VAR) rb = kf->as.var.binding;
    else if (kf && kf->kind == EX_CLOSURE && kf->as.closure_.closure
             && kf->as.closure_.closure->fn)
        rb = kf->as.closure_.closure->fn->binding;
    const Expr *prog = ctx->program_root;
    if (!rb || !prog || prog->kind != EX_PROGRAM) return NULL;
    for (uint32_t i = 0; i < prog->as.program.n; i++) {
        const Expr *it = prog->as.program.items[i];
        if (!it || it->kind != EX_FN_DEF || !it->as.fn_def_.fn) continue;
        const FnDef *fd = it->as.fn_def_.fn;
        if (fd->binding != rb) continue;
        /* Runtime-pure rows (empty, or only capability tags such as IO
         * from a `println`) cannot escape: nothing in them is performed. */
        bool declared = fd->binding->type.kind == TY_FN
            && !effect_row_is_runtime_pure(fd->binding->type.as.fn.effect_row);
        bool inferred = fd->inferred_effect_row
            && !effect_row_is_runtime_pure(fd->inferred_effect_row);
        return (declared || inferred) ? fd : NULL;
    }
    return NULL;
}

char *emit_effects_serial_shift(EmitCtx *ctx, Buf *body, const Expr *e) {
    const FnDef *eff_recv = serial_receiver_with_escaping_effect(ctx, e);
    if (eff_recv) {
        Buf row; buf_init(&row);
        effect_row_print(&row, eff_recv->inferred_effect_row
                                   && !effect_row_is_empty(eff_recv->inferred_effect_row)
                               ? eff_recv->inferred_effect_row
                               : eff_recv->binding->type.as.fn.effect_row);
        buf_putc(&row, '\0');
        const char *nm = eff_recv->binding->name ? eff_recv->binding->name->name : "<lambda>";
        diag_emit_with_code(DIAG_ERROR, e->span,
                            TUR_E0706_SERIAL_CONTEXT_NOT_CAPTURABLE,
                            "serial-shift receiver '%s' performs an effect that escapes it (%s)\n"
                            "  = note: the receiver runs once, when the continuation is captured, "
                            "outside the handlers that enclose the serial-reset -- an effect it "
                            "does not handle itself has nowhere to go\n"
                            "  = help: handle the effect inside the receiver (or in a function it "
                            "calls), or perform it outside the serial-reset; run `tur explain "
                            "TUR-E0706` for details",
                            nm, row.data ? row.data : "?");
        buf_free(&row);
        char *tmp = fresh_tmp(ctx);
        indent_buf(body, ctx->indent);
        buf_printf(body, "int64_t %s = 0; /* serial-shift: rejected (TUR-E0706) */\n", tmp);
        return tmp;
    }
    /* serial-shift-unsupported-context-miscompile: reaching this fallback means
     * the enclosing serial-reset could not lower its delimited context onto the
     * DK machine (sk_can_lower / collect_ctx rejected the shape), so the shift
     * is being emitted as a bare value.  There is no legacy lowering for serial
     * continuations, so the historical behaviour here was to emit `0` -- a
     * silent miscompile (the receiver k was never invoked, the shift evaluated
     * to 0).  Reject it instead with a real diagnostic. */
    diag_emit_with_code(DIAG_ERROR, e->span,
                        TUR_E0706_SERIAL_CONTEXT_NOT_CAPTURABLE,
                        "serial-shift context is not capturable\n"
                        "  = note: the delimited context falls outside the DK "
                        "lowering grammar, so the continuation cannot be reified\n"
                        "  = help: restructure into a supported shape (scalar let "
                        "prelude / arithmetic / 2-arg call / if / do-tail), e.g. "
                        "pack loop state into one Serializable struct passed as a "
                        "tail call's argument; run `tur explain TUR-E0706` for details");
    /* Emit a typed placeholder so downstream codegen does not crash; the C
     * output is discarded because diag_had_error() now fails the build. */
    char *tmp = fresh_tmp(ctx);
    indent_buf(body, ctx->indent);
    buf_printf(body, "int64_t %s = 0; /* serial-shift: rejected (TUR-E0706) */\n", tmp);
    return tmp;
}

/* =========================================================================
 * Region B -- continuation predicate
 * ========================================================================= */

char *emit_effects_cont_pred(EmitCtx *ctx, Buf *body, const Expr *e) {
    /* (cont? k) - check if a continuation is still valid (not done).
     * A handler continuation -- an `effect-cont` or a plain `int` k, the
     * shapes a direct-emitted `resume` takes -- is a DK chain or the cloneable
     * wrapper around one, and __dk_cont_live_word tells them apart as
     * __dk_resume_word does (cont-query-on-wrapped-handler-k-reads-past-
     * wrapper).  Both used to call fiber-runtime helpers deleted 2026-07-19,
     * so either failed to link. */
    char *inner = emit_value(ctx, body, e->as.cont_pred_.expr);
    char *tmp = fresh_tmp(ctx);
    indent_buf(body, ctx->indent);
    const Type *kt = &e->as.cont_pred_.expr->type;
    int flavor = kt->kind == TY_CONT ? kt->as.cont.flavor : CONT_EFFECT;
    if (flavor == CONT_ESCAPE) {
        /* cont-pred-on-escape-cont-fails-to-link: a call/cc / escape k is the
         * `tur_escape_cont` landing pad, invocable while its prompt is on the
         * stack -- the test tur_escape_resume makes before it longjmps.  The
         * other non-effect flavors went to the deleted fiber runtime's
         * tur_cont_consumed, so `(cont? k)` failed to link. */
        buf_printf(body, "bool %s = tur_escape_is_live((int64_t)(intptr_t)(%s));\n",
                   tmp, inner);
    } else if (flavor == CONT_CLONEABLE || flavor == CONT_SERIAL) {
        /* A cloneable or serial k is multi-shot: it never becomes consumed. */
        buf_printf(body, "bool %s = (intptr_t)(%s) != 0;\n", tmp, inner);
    } else {
        buf_printf(body, "bool %s = __dk_cont_live_word((intptr_t)(%s));\n", tmp, inner);
    }
    free(inner);
    return tmp;
}
