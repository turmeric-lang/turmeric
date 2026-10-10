/* emit_effects.c -- expression-position emission for algebraic effects and
 * CPS (shift/reset) constructs.  Covers the three regions of emit_value that
 * were extracted from emit_expr.c (EE1--EE3):
 *
 *   Region C  EX_DEFECT / EX_PERFORM / EX_HANDLE / EX_RESUME / EX_DISCONTINUE
 *   Region A  EX_RESET / EX_CLONEABLE_RESET / EX_SHIFT / EX_SHIFT0 /
 *             EX_CLONEABLE_SHIFT / EX_SERIAL_RESET / EX_SERIAL_SHIFT
 *   Region B  EX_CONT_PRED
 *
 * The emit_program runtime prelude fragments (fiber struct fields,
 * global_effect_handler_chain, tur_effect_perform definition) remain in
 * emit_module.c -- see emit-effects-extraction-plan.md §EE4 for rationale.
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

char *emit_effects_handle(EmitCtx *ctx, Buf *body, const Expr *e) {
    /* (handle body (E [params] k) handler ...)
     *
     * Phase 19D: Deep-handler continuation capture semantics.
     * The body runs in a new fiber. When an effect is performed,
     * the fiber yields to the parent dispatch loop, which calls
     * the matching handler case function with k = (int64_t)fiber.
     * (resume k v) resumes the fiber via tur_effect_cont_resume.
     * Handler case functions receive outer captures via __env.
     */
    HandleExpr *h = e->as.handle_.handle;

    /* defopaque-struct-payload-fails-through-unsafe-helper: the built-in
     * `Unsafe` effect is a pure compile-time marker -- it is never actually
     * `perform`ed, so this handle's fiber-lift never suspends; it exists only
     * to discharge the Unsafe row from the body's effect type.  Routing the
     * body result through the fiber's `int64_t` result slot truncates a
     * by-value struct return (e.g. `(unsafe (dense-get d i))` whose element
     * type is a struct -- "aggregate value used where an integer was
     * expected").  Emit the body directly in the current context instead:
     * semantically identical (nothing suspends, so no continuation is ever
     * captured) and it preserves the body's real C type.  Any genuine effect
     * the body performs is handled by an enclosing handle exactly as it would
     * have been after the Unsafe dispatch forwarded it to the parent. */
    if (emit_handle_is_pure_unsafe(h)) {
        /* Value position: emit the body and return its value directly.  Its C
         * type is whatever the body produces (e.g. a per-instantiation struct
         * return `Pos`), so no carrier truncation occurs.  Statement position
         * (where the result is discarded) is handled in emit_stmt's EX_HANDLE
         * case, which emit_stmt's the body so its side effects still run. */
        return emit_value(ctx, body, h->body);
    }

    /* F2.4 tail: shallow effect handlers (`handle-shallow`) on the fiber fallback.
     * The intercept EffectHandlerFrame stays installed for the fiber's whole
     * lifetime, so deep re-catches every perform.  For a shallow handler we make
     * the per-handle dispatch DECLINE its own effect once its case has run (a
     * `shallow_consumed` flag on the capture ctx): a subsequent same-effect
     * perform then falls through to the existing bubble-up path, which forwards it
     * to the enclosing handler and re-dispatches the inner body with that
     * handler's resume value -- exactly the interpreter's shallow semantics.  See
     * the dispatch emission below (Step 6), all gated on `h->shallow`. */

    bool returns_value = (e->type.kind != TY_NIL);

    /* Allocate unique IDs for this handle expression */
    int handle_id = ctx->tmp_n++;

    /* ----------------------------------------------------------------
     * Step 1: Collect outer variables captured by handler case bodies.
     * ---------------------------------------------------------------- */
    /* Collect captures for each case body plus the handle body */
    Binding **all_caps = NULL;
    uint32_t n_all_caps = 0, c_all_caps = 0;

    /* Collect captures from handle body (for body fiber function) */
    uint32_t n_body_caps = 0;
    Binding **body_caps = collect_handle_captures(h->body, &n_body_caps);
    for (uint32_t ci = 0; ci < n_body_caps; ci++) {
        bool already = false;
        for (uint32_t ai = 0; ai < n_all_caps; ai++)
            if (all_caps[ai] == body_caps[ci]) { already = true; break; }
        if (!already) {
            if (n_all_caps >= c_all_caps) {
                c_all_caps = (c_all_caps == 0) ? 8 : c_all_caps * 2;
                all_caps = (Binding **)realloc(all_caps, c_all_caps * sizeof(Binding *));
            }
            all_caps[n_all_caps++] = body_caps[ci];
        }
    }
    free(body_caps);

    /* Collect captures from handler case bodies (they use outer vars + k + effect params) */
    /* Handler case bodies are emitted as separate static fns; outer vars are captured via __env */
    Binding ***case_caps = (Binding ***)malloc(h->n_cases * sizeof(Binding **));
    uint32_t *case_ncaps = (uint32_t *)malloc(h->n_cases * sizeof(uint32_t));
    for (uint8_t ci = 0; ci < h->n_cases; ci++) {
        HandleCase *c = &h->cases[ci];
        case_caps[ci] = collect_handle_captures(c->body, &case_ncaps[ci]);
        /* Filter out k_binding and param_bindings (those are local to the handler fn) */
        uint32_t filtered = 0;
        for (uint32_t j = 0; j < case_ncaps[ci]; j++) {
            Binding *b = case_caps[ci][j];
            bool is_local = false;
            if (c->k_binding && b == c->k_binding) is_local = true;
            for (uint32_t p = 0; p < c->n_params && !is_local; p++)
                if (c->param_bindings && c->param_bindings[p] == b) is_local = true;
            if (!is_local) case_caps[ci][filtered++] = b;
        }
        case_ncaps[ci] = filtered;
        /* Add to all_caps */
        for (uint32_t j = 0; j < case_ncaps[ci]; j++) {
            bool already = false;
            for (uint32_t ai = 0; ai < n_all_caps; ai++)
                if (all_caps[ai] == case_caps[ci][j]) { already = true; break; }
            if (!already) {
                if (n_all_caps >= c_all_caps) {
                    c_all_caps = (c_all_caps == 0) ? 8 : c_all_caps * 2;
                    all_caps = (Binding **)realloc(all_caps, c_all_caps * sizeof(Binding *));
                }
                all_caps[n_all_caps++] = case_caps[ci][j];
            }
        }
    }

    bool has_captures = (n_all_caps > 0);

    /* ----------------------------------------------------------------
     * Step 2: Emit env struct type (if captures exist).
     * ---------------------------------------------------------------- */
    char env_type_name[64];
    snprintf(env_type_name, sizeof(env_type_name), "__HEnv_%d", handle_id);
    char env_var_name[64];
    snprintf(env_var_name, sizeof(env_var_name), "__henv_%d", handle_id);

    Buf *hbuf = ctx->pending_handler_fns;

    if (has_captures) {
        buf_printf(hbuf, "typedef struct %s %s;\n", env_type_name, env_type_name);
        buf_printf(hbuf, "struct %s {\n", env_type_name);
        for (uint32_t ci = 0; ci < n_all_caps; ci++) {
            Binding *b = all_caps[ci];
            char *raw = raw_name_for_binding(b);
            /* ET2: poly fn params (TY_FORALL) must be stored as tur_poly_fn_t */
            if (b->is_poly_fn) {
                buf_printf(hbuf, "    tur_poly_fn_t %s;\n", raw);
            } else {
                /* When the captured binding is a function parameter whose
                 * type is a pass-by-pointer struct (Phase D: size > 16
                 * bytes), the param arrives at the function as `const T *`;
                 * the env field must match that shape so the fill
                 * `env->b = b;` and writeback `b = env->b;` compile, and so
                 * that captured-binding field accesses (which emit `->`
                 * for pass-by-ptr struct receivers) remain valid through
                 * the env. Let-bound captures still store the struct value
                 * by value -- the local was declared by value, not as a
                 * pointer. */
                bool is_param = false;
                if (ctx->fn_params) {
                    for (uint32_t pi = 0; pi < ctx->n_fn_params; pi++) {
                        if (ctx->fn_params[pi] == b) { is_param = true; break; }
                    }
                }
                /* Resolve through the current ABI specialization so a
                 * captured `:A` binding inside a per-instantiation clone
                 * picks up the concrete C type (e.g. struct Pos), not
                 * the int64_t carrier. Without this, the env-fill
                 * `__henv->v = v;` and writeback `v = __henv->v;` would
                 * be int64_t-vs-struct mismatches at C compile time --
                 * the closure-env counterpart of the per-instantiation
                 * monomorphization fix at emit_module.c (see archived
                 * generic-inline-c-struct-arg-monomorphises-to-int64). */
                Type resolved_ty = emit_resolve_type(ctx, b->type);
                if (is_param && type_struct_pass_by_ptr(resolved_ty)) {
                    buf_printf(hbuf, "    const %s *%s;\n",
                               type_c_name(resolved_ty), raw);
                } else {
                    buf_printf(hbuf, "    %s %s;\n",
                               type_c_name(resolved_ty), raw);
                }
            }
            free(raw);
        }
        buf_printf(hbuf, "};\n\n");
    }

    /* ----------------------------------------------------------------
     * Step 3: Allocate names for handler functions, body fn, dispatch fn.
     * ---------------------------------------------------------------- */
    char **hfn_names = (char **)malloc(h->n_cases * sizeof(char *));
    for (uint8_t i = 0; i < h->n_cases; i++) {
        hfn_names[i] = (char *)malloc(48);
        snprintf(hfn_names[i], 48, "__effect_handler_%d", ctx->tmp_n++);
    }
    char body_fn_name[64];
    snprintf(body_fn_name, sizeof(body_fn_name), "__handle_body_%d", handle_id);
    char dispatch_fn_name[64];
    snprintf(dispatch_fn_name, sizeof(dispatch_fn_name), "__dispatch_%d", handle_id);

    /* ----------------------------------------------------------------
     * Step 4: Emit handler case functions (static, file-scope).
     * Handler receives outer captures via __env (cast to HEnv*).
     * ---------------------------------------------------------------- */
    for (uint8_t i = 0; i < h->n_cases; i++) {
        HandleCase *c = &h->cases[i];

        /* Emit handler body with handle_captures context for outer var access.
         * Use temp buffers so nested handles emit their static fns at file scope
         * (before this function), not inside it. Pattern mirrors emit_fn_def. */
        {
            Buf hfn_buf; buf_init(&hfn_buf);
            Buf hfn_pending; buf_init(&hfn_pending);

            /* Cast __env to typed env pointer if we have captures */
            if (has_captures) {
                buf_printf(&hfn_buf,
                    "    %s *%s = (%s *)__env;\n",
                    env_type_name, env_var_name, env_type_name);
            }

            /* Unpack effect arguments into named locals */
            for (uint32_t j = 0; j < c->n_params; j++) {
                if (c->param_bindings && c->param_bindings[j]) {
                    const char *ctype = type_c_name(c->param_bindings[j]->type);
                    char *raw = raw_name_for_binding(c->param_bindings[j]);
                    /* Unpack the effect arg from the int64 slot: a by-value
                     * aggregate arrived boxed (deref), a float was
                     * bit-reinterpreted by the perform side (reinterpret back --
                     * a plain cast would numerically convert the bits),
                     * everything else is a plain cast. */
                    char src[48];
                    snprintf(src, sizeof(src), "__effect_args[%d]", j);
                    char *ld = eff_slot_load(c->param_bindings[j]->type, src);
                    /* A fn-value payload is referenced (indirect call, emit_value)
                     * by its RAW id-less name -- name_for_binding's TY_FN-non-boxed
                     * path -- so declare it the same way; otherwise the declaration
                     * (`f_<id>`) and the use (`f`) diverge and `f` is undeclared. */
                    bool fnval = c->param_bindings[j]->type.kind == TY_FN
                                 && !c->param_bindings[j]->type.as.fn.boxed;
                    if (fnval)
                        buf_printf(&hfn_buf, "    %s %s = %s;\n", ctype, raw, ld);
                    else
                        buf_printf(&hfn_buf, "    %s %s_%u = %s;\n",
                                   ctype, raw, (unsigned)c->param_bindings[j]->id, ld);
                    free(ld);
                    free(raw);
                }
            }

            /* Bind k - now it's a fiber pointer cast to int64_t */
            if (c->k_binding) {
                char *raw = raw_name_for_binding(c->k_binding);
                buf_printf(&hfn_buf,
                    "    int64_t %s_%u = __k;\n",
                    raw, (unsigned)c->k_binding->id);
                free(raw);
            }

            EmitCtx hctx = *ctx;
            hctx.file = &hfn_buf;
            hctx.pending_handler_fns = &hfn_pending;
            hctx.indent = 4;
            hctx.fn_params = NULL;
            hctx.n_fn_params = 0;
            hctx.closure = NULL;
            hctx.env_var_name = has_captures ? env_var_name : NULL;
            hctx.defer_captures = NULL;
            hctx.n_defer_captures = 0;
            hctx.frame_var = NULL;
            hctx.return_emitted = false;
            hctx.handle_captures = has_captures ? all_caps : NULL;
            hctx.n_handle_captures = has_captures ? n_all_caps : 0;
            hctx.handle_env_name = has_captures ? env_var_name : NULL;
            /* Tier C (P2): this fiber handler fn returns the int64 effect ABI, not
             * the outer function's C return type -- so a pending-panic propagation
             * return is `(int64_t){0}`, and an aggregate result is boxed below. */
            hctx.current_fn_ret_ctype = "int64_t";

            /* Emit handler case body.
             * Phase 19D: for non-never, non-nil bodies, or nil bodies ending in
             * fiber-resume, use emit_value so the fiber result is returned.
             * For never-typed or nil bodies that don't resume, use emit_stmt + return 0. */
            if (!c->body) {
                buf_puts(&hfn_buf, "    return 0;\n");
            } else if (c->body->type.kind == TY_NEVER) {
                /* Never-returning body: emit as stmt (panic, discontinue, etc.) */
                emit_stmt(&hctx, &hfn_buf, c->body);
                buf_puts(&hfn_buf, "    return 0; /* unreachable */\n");
            } else {
                char *hret = emit_value(&hctx, &hfn_buf, c->body);
                /* Check if the value is nil placeholder (atom_nil = ((void)0)) */
                bool is_nil_placeholder = (strcmp(hret, "((void)0)") == 0);
                if (is_nil_placeholder) {
                    /* Body is nil-typed and doesn't produce a fiber result */
                    free(hret);
                    buf_puts(&hfn_buf, "    return 0;\n");
                } else {
                    /* Store the handle result into the int64 ABI: a by-value
                     * aggregate boxed, a float bit-reinterpreted (the
                     * resume/perform/handle side unpacks with the inverse). */
                    char *sv = eff_slot_store(c->body->type, hret);
                    buf_printf(&hfn_buf, "    return %s;\n", sv);
                    free(sv); free(hret);
                }
            }

            ctx->tmp_n = hctx.tmp_n;

            /* Flush inner pending fns BEFORE this handler function */
            if (hfn_pending.len > 0) {
                buf_write(hbuf, hfn_pending.data, hfn_pending.len);
            }
            buf_free(&hfn_pending);

            /* Now emit forward decl + function definition */
            buf_printf(hbuf,
                "static int64_t %s(int64_t *__effect_args, int __n_effect_args,"
                " int64_t __k, void *__env);\n", hfn_names[i]);
            buf_printf(hbuf,
                "static int64_t %s(int64_t *__effect_args, int __n_effect_args,"
                " int64_t __k, void *__env) {\n", hfn_names[i]);
            buf_write(hbuf, hfn_buf.data, hfn_buf.len);
            buf_puts(hbuf, "}\n\n");
            buf_free(&hfn_buf);
        }
    }

    /* ----------------------------------------------------------------
     * Step 5: Emit handle body fiber function.
     * This is called as the fiber's entry_fn. It accesses the env
     * via tur_current_fiber->eff_ctx->body_env.
     *
     * Use a temporary buffer + nested pending_hfns so that inner
     * handles write their static functions BEFORE this function
     * (not inside it). Pattern mirrors emit_fn_def.
     * ---------------------------------------------------------------- */
    {
        Buf fn_buf; buf_init(&fn_buf);
        Buf inner_pending; buf_init(&inner_pending);

        if (has_captures) {
            buf_printf(&fn_buf,
                "    TurEffectCaptureCtx *__cap = (TurEffectCaptureCtx *)tur_current_fiber->eff_ctx;\n");
            buf_printf(&fn_buf,
                "    %s *%s = (%s *)__cap->body_env;\n",
                env_type_name, env_var_name, env_type_name);
        }

        EmitCtx bctx = *ctx;
        bctx.file = &fn_buf;
        bctx.pending_handler_fns = &inner_pending;
        bctx.indent = 4;
        bctx.fn_params = NULL;
        bctx.n_fn_params = 0;
        bctx.closure = NULL;
        bctx.env_var_name = has_captures ? env_var_name : NULL;
        bctx.defer_captures = NULL;
        bctx.n_defer_captures = 0;
        bctx.frame_var = NULL;
        bctx.return_emitted = false;
        bctx.handle_captures = has_captures ? all_caps : NULL;
        bctx.n_handle_captures = has_captures ? n_all_caps : 0;
        bctx.handle_env_name = has_captures ? env_var_name : NULL;
        /* Tier C (P2): the body fiber fn is void (it stores into
         * tur_current_fiber->result), so a pending-panic propagation is a bare
         * `return;`, not a typed zero of the outer function's return type. */
        bctx.current_fn_ret_ctype = "void";

        if (h->body->type.kind == TY_NIL || h->body->type.kind == TY_NEVER) {
            emit_stmt(&bctx, &fn_buf, h->body);
            buf_puts(&fn_buf, "    tur_current_fiber->result = 0;\n");
        } else {
            char *bret = emit_value(&bctx, &fn_buf, h->body);
            /* Store the body result into the int64 fiber result slot: a by-value
             * aggregate boxed, a float bit-reinterpreted (a plain cast would
             * truncate the fraction); the handle-site / resume unpacks it. */
            char *sv = eff_slot_store(h->body->type, bret);
            buf_printf(&fn_buf, "    tur_current_fiber->result = %s;\n", sv);
            free(sv);
            free(bret);
        }

        ctx->tmp_n = bctx.tmp_n;

        /* Emit defer thunks registered during handle body emission.
         * They were queued on bctx but not yet flushed. Emit them to
         * inner_pending so they appear before the body function in hbuf. */
        if (bctx.pending_defer_thunks != ctx->pending_defer_thunks) {
            /* Extract only the NEW thunks (those prepended during body emit) */
            DeferThunk *new_thunks = bctx.pending_defer_thunks;
            if (new_thunks) {
                DeferThunk *p = new_thunks;
                while (p->next != ctx->pending_defer_thunks) p = p->next;
                p->next = NULL;  /* terminate new-thunks list */
                /* Use bctx so name_for_binding resolves handle captures correctly */
                EmitCtx thunk_ctx = bctx;
                thunk_ctx.pending_defer_thunks = new_thunks;
                emit_pending_defer_thunks(&thunk_ctx, &inner_pending, &inner_pending);
                /* emit_pending_defer_thunks freed the new thunks; ctx unchanged */
            }
        }

        /* Flush nested pending handler fns before the body function */
        if (inner_pending.len > 0) {
            buf_write(hbuf, inner_pending.data, inner_pending.len);
        }
        buf_free(&inner_pending);

        /* Forward declaration */
        buf_printf(hbuf, "static void %s(void);\n", body_fn_name);
        /* Function definition */
        buf_printf(hbuf, "static void %s(void) {\n", body_fn_name);
        /* Emit body content */
        buf_write(hbuf, fn_buf.data, fn_buf.len);
        buf_puts(hbuf, "}\n\n");
        buf_free(&fn_buf);
    }

    /* ----------------------------------------------------------------
     * Step 6: Emit dispatch function.
     * ---------------------------------------------------------------- */
    buf_printf(hbuf, "static int64_t %s(void *__ctx_void, int64_t __k_int, int64_t __resume_val);\n",
               dispatch_fn_name);

    /* MS1: Emit multishot wrapper helpers (env struct + cont_fn + clone_fn).
     * These are emitted after the dispatch fn forward declaration so they can
     * call dispatch_fn_name, and before the dispatch fn body that calls them. */
    for (uint8_t i = 0; i < h->n_cases; i++) {
        HandleCase *ms_c = &h->cases[i];
        if (ms_c->cont_kind != CK_MULTISHOT) continue;
        /* Env struct: captures the dispatch context + fiber, plus a snapshot of
         * the fiber's suspended-at-perform stack image so each resume re-runs an
         * INDEPENDENT copy of the continuation (true multishot).  See
         * docs/archive/history/turi-effect-multishot-degenerate-resume.md. */
        buf_printf(hbuf, "struct __ms_env_%d_%d { void *__ctx; int64_t __k_int; "
                         "FiberBlock *__fiber; char *__stack_image; size_t __image_size; "
                         "ucontext_t __saved_ctx; };\n",
                   handle_id, (int)i);
        /* Continuation function: restore the fiber to its captured perform point,
         * then dispatch_fn(ctx, k_int, v) resumes that fresh copy. */
        buf_printf(hbuf,
            "static int64_t __ms_cont_fn_%d_%d(void *__env, int64_t __v) {\n",
            handle_id, (int)i);
        buf_printf(hbuf,
            "    struct __ms_env_%d_%d *__e = (struct __ms_env_%d_%d *)__env;\n",
            handle_id, (int)i, handle_id, (int)i);
        buf_puts(hbuf,
            "    if (__e->__fiber && __e->__stack_image) {\n"
            "        memcpy(__e->__fiber->stack, __e->__stack_image, __e->__image_size);\n"
            "        __e->__fiber->ctx = __e->__saved_ctx;\n"
            "        __e->__fiber->done = 0;\n"
            "    }\n");
        buf_printf(hbuf,
            "    return %s(__e->__ctx, __e->__k_int, __v);\n", dispatch_fn_name);
        buf_puts(hbuf, "}\n");
        /* Clone function: shallow copy.  The stack image is written once at
         * capture and only read on restore, so clones safely share it. */
        buf_printf(hbuf,
            "static void *__ms_env_clone_%d_%d(const void *__env) {\n",
            handle_id, (int)i);
        buf_printf(hbuf,
            "    struct __ms_env_%d_%d *__orig = (struct __ms_env_%d_%d *)__env;\n",
            handle_id, (int)i, handle_id, (int)i);
        buf_printf(hbuf,
            "    struct __ms_env_%d_%d *__copy = (struct __ms_env_%d_%d *)malloc(sizeof(struct __ms_env_%d_%d));\n",
            handle_id, (int)i, handle_id, (int)i, handle_id, (int)i);
        buf_puts(hbuf, "    if (__copy) *__copy = *__orig;\n");
        buf_puts(hbuf, "    return __copy;\n");
        buf_puts(hbuf, "}\n");
    }

    buf_printf(hbuf, "static int64_t %s(void *__ctx_void, int64_t __k_int, int64_t __resume_val) {\n",
               dispatch_fn_name);
    buf_puts(hbuf, "    TurEffectCaptureCtx *__dcap = (TurEffectCaptureCtx *)__ctx_void;\n");
    buf_puts(hbuf, "    FiberBlock *__fiber = (FiberBlock *)(intptr_t)__k_int;\n");
    buf_puts(hbuf, "    int64_t __r = tur_fiber_block_resume(__fiber, __resume_val);\n");
    buf_puts(hbuf, "    if (__fiber->done) { return __fiber->result; }\n");
    buf_puts(hbuf, "    if (!__dcap->has_pending_effect) return __r;\n");
    /* Dispatch to matching handler case.  F2: a shallow handler guards every case
     * on `!shallow_consumed` and sets the flag when it matches, so once its case
     * has run, a later same-effect perform matches no case and falls through to
     * the bubble-up else branch (forward to the enclosing handler). */
    const char *sh_guard = h->shallow ? "!__dcap->shallow_consumed && " : "";
    for (uint8_t i = 0; i < h->n_cases; i++) {
        HandleCase *c = &h->cases[i];
        if (i == 0)
            buf_printf(hbuf, "    if (%sstrcmp(__dcap->eff_name, \"%s\") == 0) {\n",
                       sh_guard, c->effect_name->name);
        else
            buf_printf(hbuf, "    } else if (%sstrcmp(__dcap->eff_name, \"%s\") == 0) {\n",
                       sh_guard, c->effect_name->name);
        if (h->shallow)
            buf_puts(hbuf, "        __dcap->shallow_consumed = true;\n");
        if (c->cont_kind == CK_MULTISHOT) {
            /* MS1: wrap fiber k in a tur_cloneable_cont for snapshot-safe multi-shot. */
            buf_printf(hbuf,
                "        struct __ms_env_%d_%d *__ms_e = (struct __ms_env_%d_%d *)malloc(sizeof(struct __ms_env_%d_%d));\n",
                handle_id, (int)i, handle_id, (int)i, handle_id, (int)i);
            buf_puts(hbuf, "        if (!__ms_e) abort();\n");
            buf_printf(hbuf, "        __ms_e->__ctx = __ctx_void; __ms_e->__k_int = __k_int;\n");
            /* Snapshot the fiber's stack at the perform point so each resume can
             * restore and re-run an independent copy (true multishot). */
            buf_puts(hbuf,
                "        __ms_e->__fiber = __fiber;\n"
                "        __ms_e->__image_size = __fiber->stack_size;\n"
                "        __ms_e->__stack_image = (char *)malloc(__fiber->stack_size);\n"
                "        if (!__ms_e->__stack_image) abort();\n"
                "        memcpy(__ms_e->__stack_image, __fiber->stack, __fiber->stack_size);\n"
                "        __ms_e->__saved_ctx = __fiber->ctx;\n");
            buf_printf(hbuf,
                "        int64_t __k_ms = (int64_t)(intptr_t)tur_cloneable_cont_alloc(__ms_cont_fn_%d_%d, __ms_e, __ms_env_clone_%d_%d, free);\n",
                handle_id, (int)i, handle_id, (int)i);
            if (has_captures)
                buf_printf(hbuf,
                    "        return %s(__dcap->eff_args, __dcap->eff_n_args, __k_ms, __dcap->body_env);\n",
                    hfn_names[i]);
            else
                buf_printf(hbuf,
                    "        return %s(__dcap->eff_args, __dcap->eff_n_args, __k_ms, NULL);\n",
                    hfn_names[i]);
        } else if (has_captures)
            buf_printf(hbuf,
                "        return %s(__dcap->eff_args, __dcap->eff_n_args, __k_int, __dcap->body_env);\n",
                hfn_names[i]);
        else
            buf_printf(hbuf,
                "        return %s(__dcap->eff_args, __dcap->eff_n_args, __k_int, NULL);\n",
                hfn_names[i]);
    }
    if (h->n_cases > 0) {
        buf_puts(hbuf, "    } else {\n");
        /* Phase 19D: bubble unhandled effect to outer fiber if possible */
        buf_puts(hbuf, "        /* Phase 19D: bubble up unhandled effect to outer fiber */\n");
        buf_puts(hbuf, "        FiberBlock *__outer_f = tur_current_fiber;\n");
        buf_puts(hbuf, "        if (__outer_f && __outer_f->eff_ctx) {\n");
        buf_puts(hbuf, "            TurEffectCaptureCtx *__outer_cap = (TurEffectCaptureCtx *)__outer_f->eff_ctx;\n");
        buf_puts(hbuf, "            __outer_cap->eff_name = __dcap->eff_name;\n");
        buf_puts(hbuf, "            int __bun = __dcap->eff_n_args < 8 ? __dcap->eff_n_args : 8;\n");
        buf_puts(hbuf, "            for (int __bi = 0; __bi < __bun; __bi++) __outer_cap->eff_args[__bi] = __dcap->eff_args[__bi];\n");
        buf_puts(hbuf, "            __outer_cap->eff_n_args = __dcap->eff_n_args;\n");
        buf_puts(hbuf, "            __outer_cap->has_pending_effect = true;\n");
        buf_puts(hbuf, "            tur_fiber_block_yield(0);\n");
        buf_puts(hbuf, "            __outer_cap->has_pending_effect = false;\n");
        /* Recursively dispatch inner fiber with the value the outer handler provided */
        buf_printf(hbuf, "            return %s(__ctx_void, __k_int, __outer_f->arg);\n",
                   dispatch_fn_name);
        buf_puts(hbuf, "        }\n");
        buf_puts(hbuf, "        fprintf(stderr, \"dispatch: unhandled effect: %s\\n\", __dcap->eff_name);\n");
        buf_puts(hbuf, "        abort();\n");
        buf_puts(hbuf, "    }\n");
    }
    buf_puts(hbuf, "    return 0;\n");
    buf_puts(hbuf, "}\n\n");

    /* ----------------------------------------------------------------
     * Step 7: Inline code -- set up fiber and run dispatch.
     * ---------------------------------------------------------------- */
    char *result = fresh_tmp(ctx);

    /* Alloc env struct if needed */
    if (has_captures) {
        indent_buf(body, ctx->indent);
        buf_printf(body, "%s *%s = (%s *)malloc(sizeof(%s));\n",
                   env_type_name, env_var_name, env_type_name, env_type_name);
        /* Populate env fields */
        for (uint32_t ci = 0; ci < n_all_caps; ci++) {
            Binding *b = all_caps[ci];
            char *raw = raw_name_for_binding(b);
            char *cur_name = name_for_binding(ctx, b);
            indent_buf(body, ctx->indent);
            buf_printf(body, "%s->%s = %s;\n", env_var_name, raw, cur_name);
            free(raw);
            free(cur_name);
        }
    }

    /* Alloc TurEffectCaptureCtx */
    char cap_var[64];
    snprintf(cap_var, sizeof(cap_var), "__cap_%d", handle_id);
    indent_buf(body, ctx->indent);
    buf_printf(body, "TurEffectCaptureCtx %s;\n", cap_var);
    indent_buf(body, ctx->indent);
    buf_printf(body, "%s.has_pending_effect = false;\n", cap_var);
    indent_buf(body, ctx->indent);
    buf_printf(body, "%s.eff_name = NULL;\n", cap_var);
    indent_buf(body, ctx->indent);
    buf_printf(body, "%s.eff_n_args = 0;\n", cap_var);
    indent_buf(body, ctx->indent);
    buf_printf(body, "%s.dispatch = %s;\n", cap_var, dispatch_fn_name);
    indent_buf(body, ctx->indent);
    buf_printf(body, "%s.shallow_consumed = false;\n", cap_var);  /* F2 */
    indent_buf(body, ctx->indent);
    buf_printf(body, "%s.body_env = %s;\n", cap_var,
               has_captures ? env_var_name : "NULL");

    /* Create the body fiber */
    char fiber_var[64];
    snprintf(fiber_var, sizeof(fiber_var), "__fiber_%d", handle_id);
    indent_buf(body, ctx->indent);
    buf_printf(body, "FiberBlock *%s = tur_fiber_block_new(%s, 0);\n",
               fiber_var, body_fn_name);
    indent_buf(body, ctx->indent);
    buf_printf(body, "%s->eff_ctx = &%s;\n", fiber_var, cap_var);

    /* Set up intercept frame (handler_fn == NULL means fiber-intercept) */
    char frame_var[64];
    snprintf(frame_var, sizeof(frame_var), "__eff_frame_%d", handle_id);
    char chain_var[64];
    snprintf(chain_var, sizeof(chain_var), "__eff_chain_%d", handle_id);

    /* The intercept frame must be installed in the FIBER's effect handler chain,
     * not the current chain.  We install it before starting the fiber by setting
     * fiber->effect_handler_chain directly.
     * parent is set to the current chain so effects not handled here can bubble up. */
    char parent_chain_var[64];
    snprintf(parent_chain_var, sizeof(parent_chain_var), "__parent_chain_%d", handle_id);
    indent_buf(body, ctx->indent);
    buf_printf(body,
        "EffectHandlerFrame *%s = (tur_current_fiber"
        " ? (EffectHandlerFrame *)tur_current_fiber->effect_handler_chain"
        " : global_effect_handler_chain);\n", parent_chain_var);
    indent_buf(body, ctx->indent);
    buf_printf(body, "EffectHandlerFrame %s;\n", frame_var);
    indent_buf(body, ctx->indent);
    buf_printf(body, "%s.parent = %s;\n", frame_var, parent_chain_var);
    indent_buf(body, ctx->indent);
    buf_printf(body, "%s.n_cases = %d;\n", frame_var, h->n_cases);
    for (uint8_t i = 0; i < h->n_cases; i++) {
        HandleCase *c = &h->cases[i];
        indent_buf(body, ctx->indent);
        buf_printf(body, "%s.cases[%d].effect_name = \"%s\";\n",
                   frame_var, i, c->effect_name->name);
        indent_buf(body, ctx->indent);
        /* NULL handler_fn = intercept case (fiber yields to dispatch loop) */
        buf_printf(body, "%s.cases[%d].handler_fn = NULL;\n", frame_var, i);
        indent_buf(body, ctx->indent);
        buf_printf(body, "%s.cases[%d].env = NULL;\n", frame_var, i);
    }
    indent_buf(body, ctx->indent);
    buf_printf(body, "%s->effect_handler_chain = &%s;\n", fiber_var, frame_var);

    /* Start the dispatch loop -- first call resumes with 0 to start body */
    indent_buf(body, ctx->indent);
    if (returns_value) {
        /* Load the handle result out of the int64 ABI returned by dispatch: a
         * boxed aggregate deref'd, a float bit-reinterpreted (a plain cast would
         * numerically convert the double's bits), everything else a plain cast. */
        Buf dcall; buf_init(&dcall);
        buf_printf(&dcall, "%s(&%s, (int64_t)(intptr_t)%s, 0)",
                   dispatch_fn_name, cap_var, fiber_var);
        buf_putc(&dcall, '\0');
        char *ld = eff_slot_load(e->type, dcall.data);
        buf_printf(body, "%s %s = %s;\n", type_c_name(e->type), result, ld);
        free(ld);
        buf_free(&dcall);
    } else {
        buf_printf(body, "%s(&%s, (int64_t)(intptr_t)%s, 0);\n",
                   dispatch_fn_name, cap_var, fiber_var);
    }

    /* Phase 19D: Write env fields back to outer locals (so mutations in handler
     * are visible in the outer scope after dispatch). Must be done BEFORE freeing
     * the env, so do this unconditionally while env is still valid. */
    if (has_captures) {
        indent_buf(body, ctx->indent);
        buf_puts(body, "/* Phase 19D: write-back env captures to outer locals */\n");
        for (uint32_t ci = 0; ci < n_all_caps; ci++) {
            Binding *b = all_caps[ci];
            char *raw = raw_name_for_binding(b);
            char *cur_name = name_for_binding(ctx, b);
            indent_buf(body, ctx->indent);
            buf_printf(body, "%s = %s->%s;\n", cur_name, env_var_name, raw);
            free(raw);
            free(cur_name);
        }
    }

    /* If fiber is done, free it.  If not (k was stored), it lives on.
     * Cache `done` into a local FIRST: freeing the fiber and then reading
     * `fiber->done` again to decide whether to free the env is a
     * use-after-free (it only "worked" because the freed slot was not yet
     * reused). Reading the flag once up front, and freeing the env before
     * the fiber, keeps the teardown sound. */
    char done_var[80];
    snprintf(done_var, sizeof(done_var), "__fiber_done_%d", handle_id);
    indent_buf(body, ctx->indent);
    buf_printf(body, "bool %s = %s->done;\n", done_var, fiber_var);

    /* Free env if we allocated it and fiber is done (for immediate-resume case).
     * If fiber is NOT done (k stored), leak env for v1 -- it must stay alive
     * as long as the fiber might be resumed. Done before the fiber free so the
     * cached flag (not the freed fiber) gates it. */
    if (has_captures) {
        indent_buf(body, ctx->indent);
        buf_printf(body, "if (%s) { free(%s); }\n", done_var, env_var_name);
    }

    indent_buf(body, ctx->indent);
    buf_printf(body, "if (%s) { free(%s->stack); free(%s); }\n",
               done_var, fiber_var, fiber_var);

    /* Cleanup */
    for (uint8_t i = 0; i < h->n_cases; i++) free(hfn_names[i]);
    free(hfn_names);
    for (uint8_t i = 0; i < h->n_cases; i++) free(case_caps[i]);
    free(case_caps);
    free(case_ncaps);
    free(all_caps);

    (void)chain_var;  /* named but unused; kept for symmetry with frame_var */

    if (returns_value) return result;
    free(result);
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
     * Phase 19D: k is a FiberBlock* cast to int64_t.
     * tur_effect_cont_resume(k, v) resumes the fiber via the dispatch fn.
     *
     * MS1: CK_MULTISHOT k is a tur_cloneable_cont* (int64_t).
     * tur_cloneable_cont_resume(tur_continuation_snapshot(k), v) clones before each resume.
     *
     * For Phase 18 tur_cont* continuations (TY_CONT), use the old path.
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
            indent_buf(body, ctx->indent);
            buf_printf(body, "int64_t %s = tur_cloneable_cont_resume(%s, %s);\n", tmp, snap, v_arg);
            indent_buf(body, ctx->indent);
            buf_printf(body, "tur_cloneable_cont_drop(%s);\n", snap);
            free(snap);
            free(v_arg);
            free(k_var);
            free(v_var);
            return tmp;
        }
        if (k_type.kind == TY_INT) {
            /* Phase 19D: fiber-based k.
             * tur_effect_cont_resume always returns int64_t (the fiber's final result),
             * regardless of the effect's return type. Always capture the return value
             * so the handler can return it as the handle expression's result. */
            char *tmp = fresh_tmp(ctx);
            Type vtype = e->as.resume_.resume->value->type;
            bool v_is_nil = (vtype.kind == TY_NIL || vtype.kind == TY_NEVER);
            /* Store the resume value into the int64 ABI: a by-value aggregate
             * boxed (the perform result unboxes it), a float bit-reinterpreted
             * (a plain cast would truncate the fraction), else a plain cast. */
            char *v_arg = v_is_nil ? strdup("(int64_t)0")
                                   : eff_slot_store(vtype, v_var);
            indent_buf(body, ctx->indent);
            Buf rcall; buf_init(&rcall);
            buf_printf(&rcall, "tur_effect_cont_resume((int64_t)(intptr_t)%s, %s)", k_var, v_arg);
            buf_putc(&rcall, '\0');
            /* tur_effect_cont_resume returns the fiber's final result in the
             * int64 ABI.  Load it back to the resume expr's real type: a boxed
             * aggregate deref'd, a float bit-reinterpreted into a genuine double
             * local (so a downstream store re-packs it correctly rather than
             * re-reinterpreting an int as a double).  Plain scalars stay in an
             * int64_t local, matching the pre-fix ABI the callers expect. */
            bool res_is_float = (e->type.kind == TY_FLOAT || e->type.kind == TY_FLOAT64
                                 || e->type.kind == TY_FLOAT32);
            if (eff_type_boxed(e->type) || res_is_float) {
                char *ld = eff_slot_load(e->type, rcall.data);
                buf_printf(body, "%s %s = %s;\n", type_c_name(e->type), tmp, ld);
                free(ld);
            } else {
                buf_printf(body, "int64_t %s = %s;\n", tmp, rcall.data);
            }
            buf_free(&rcall);
            free(v_arg);
            free(k_var);
            free(v_var);
            return tmp;
        }
        free(k_var);
        /* Phase 18 path: return value directly */
        return v_var;
    }
    return atom_nil();
}

char *emit_effects_discontinue(EmitCtx *ctx, Buf *body, const Expr *e) {
    /* (discontinue k exception) - discontinue with exception.
     * Phase 19D: mark fiber as done and throw the exception.
     */
    if (e->as.discontinue_.discontinue) {
        char *k_var = emit_value(ctx, body, e->as.discontinue_.discontinue->k);
        if (e->as.discontinue_.discontinue->k->type.kind == TY_INT) {
            indent_buf(body, ctx->indent);
            buf_printf(body, "((FiberBlock*)(intptr_t)%s)->done = 1;\n", k_var);
        }
        free(k_var);
        char *e_var = emit_value(ctx, body, e->as.discontinue_.discontinue->exception);
        (void)e_var;  /* exception payload evaluated for side effects; discontinue aborts */
        free(e_var);
        indent_buf(body, ctx->indent);
        buf_puts(body, "abort();\n");
    }
    return atom_nil();
}

/* =========================================================================
 * Region A -- delimited / cloneable / serial continuations
 * ========================================================================= */

char *emit_effects_reset(EmitCtx *ctx, Buf *body, const Expr *e) {
    /* (reset body) - establish a continuation boundary and run body.
     *
     * cps-backend-direct-lowering-removal D3: the CPS lowering of a base reset
     * (emit_cps_reset) is deleted. Every reset whose body can dynamically reach
     * a base shift bound to it is now colored and emitted natively by the CT-IR
     * backend (emit_cps_ir.c), so it never reaches this wrapper. What remains
     * here is the sole surviving behavior: a reset with no reachable shift (or
     * one self-delimited by an inner reset/operator) just evaluates its body. */
    return emit_value(ctx, body, e->as.reset_.body);
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
    if (!ctx->in_shift_escape) return;
    indent_buf(body, ctx->indent);
    buf_printf(body, "tur_cur_shift_reset->result = (int64_t)(%s);\n", result);
    indent_buf(body, ctx->indent);
    buf_puts(body, "TUR_LONGJMP(tur_cur_shift_reset->buf);\n");
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
    /* Use emit_call_helper to handle both functions and closures */
    /* For now, just emit as a regular call */
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
     * Phase 19D: for Phase-19 TY_INT k values (FiberBlock*), check !fiber->done.
     * For Phase 18 tur_cont* continuations, call tur_cont_consumed(). */
    char *inner = emit_value(ctx, body, e->as.cont_pred_.expr);
    char *tmp = fresh_tmp(ctx);
    indent_buf(body, ctx->indent);
    if (e->as.cont_pred_.expr->type.kind == TY_CONT) {
        buf_printf(body, "bool %s = !tur_cont_consumed((tur_cont *)(intptr_t)%s);\n", tmp, inner);
    } else {
        /* Phase 19D: k is a FiberBlock* cast to int64_t */
        buf_printf(body, "bool %s = tur_effect_cont_valid(%s);\n", tmp, inner);
    }
    free(inner);
    return tmp;
}
