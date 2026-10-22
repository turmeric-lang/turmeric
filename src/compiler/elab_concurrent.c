/* elab_concurrent.c -- threads, async/await, select, panics, and software transactional memory. */
#include "elab_internal.h"

/* Phase T19-B: (thread-spawn closure)
 *
 * Performs a Send-safety check on all variables captured by the closure.
 * Types that are not Send (ref<T>, rc<T>, weak<T>, cont<T>, &T, &mut T)
 * are rejected with TUR-E0010.
 *
 * This form is the elaboration-level gate for cross-thread closure passing.
 * Actual OS-thread spawning is provided by stdlib/thread.tur (T19-C); this
 * form only validates send-safety and returns nil as a compile-time assertion.
 *
 * Usage: (thread-spawn (fn [...] body))
 */
Expr *elab_thread_spawn(Elab *e, const Form *call) {
    if (call->as.list.len != 2) {
        diag_emit(DIAG_ERROR, call->span,
                  "thread-spawn requires exactly one argument: "
                  "(thread-spawn closure)");
        return NULL;
    }

    Expr *closure_expr = elab_form(e, call->as.list.items[1]);
    if (!closure_expr) return NULL;

    /* Send-check: walk the capture set of any closure argument. */
    if (closure_expr->kind == EX_CLOSURE) {
        struct Closure *cl = closure_expr->as.closure_.closure;
        bool had_error = false;
        for (uint8_t i = 0; i < cl->n_captures; i++) {
            Binding *cap = cl->captures[i];
            if (!type_is_send(cap->type)) {
                diag_emit_with_code(DIAG_ERROR, call->span,
                    TUR_E0010_NOT_SEND,
                    "type `%s` cannot be sent across thread boundaries "
                    "(not `Send`); captured variable `%s`",
                    type_name(cap->type), cap->name->name);
                had_error = true;
            }
        }
        if (had_error) return NULL;
    }

    /* Return the closure expression (type flows through; T19-C provides the
     * actual runtime thread-spawn wrapper in stdlib/thread.tur).            */
    return closure_expr;
}

/* compiled-async-fiber-deadlocks-on-a-session-op (fix direction 3): true if
 * the (unelaborated) form spells a session op anywhere inside it.  Macros
 * and callee bodies are opaque to this walk; the captured-endpoint check in
 * elab_async covers the callee case. */
static bool form_mentions_session_op(const Elab *e, const Form *f) {
    if (!f) return false;
    switch (f->tag) {
        case F_LIST: case F_VEC: case F_MAP: case F_SET: {
            if (f->tag == F_LIST && f->as.list.len > 0 && f->as.list.items[0]
                    && f->as.list.items[0]->tag == F_SYM) {
                const Symbol *h = f->as.list.items[0]->as.sym;
                if (h == e->sym_send || h == e->sym_recv || h == e->sym_offer
                        || h == e->sym_choose_left || h == e->sym_choose_right
                        || h == e->sym_recv_timeout || h == e->sym_send_to
                        || h == e->sym_recv_from || h == e->sym_recv_timeout_from)
                    return true;
            }
            for (uint32_t i = 0; i < f->as.list.len; i++)
                if (form_mentions_session_op(e, f->as.list.items[i])) return true;
            return false;
        }
        default:
            return false;
    }
}

/* Phase T21-F: (async fn-expr) — launch fn-expr (no-arg function) in a new
 * thread; return a TurAsyncTask* as ptr<void> (the future handle).          */
Expr *elab_async(Elab *e, const Form *call) {
    if (call->as.list.len != 2) {
        diag_emit(DIAG_ERROR, call->span,
                  "(async fn-expr) requires exactly one argument");
        return NULL;
    }
    /* CF6's in_async_body flag lived here: it told elab_await to run the
     * Send-across-await check.  That check is unconditional now (see
     * elab_await), so the flag had no remaining reader and is gone rather than
     * left set-but-never-read. */
    Expr *fn_expr = elab_form(e, call->as.list.items[1]);
    if (!fn_expr) return NULL;

    /* Normalize `(async EXPR)` where EXPR is a BARE VALUE expression (not a
     * fn-value) -- e.g. `(async (with-handler ...))` -- into `(async (fn []
     * EXPR))`.  Semantically `(async EXPR)` already means "run EXPR in a fiber",
     * i.e. a thunk; making that thunk an explicit lambda routes the body through
     * the normal lifted-fn pipeline (a colored fn whose interior effect handle
     * CPS-lowers) instead of the inline fiber thunk the direct emitter's path (b)
     * synthesizes -- so `async-with-handler` DK-lowers exactly like the explicit
     * `(async (fn [] (handle ...)))` in `effects-async`.  A fn-VALUED arg (an
     * explicit lambda or a named fn passed by value) is already the thunk and is
     * left untouched -- wrapping it would spawn a fn that RETURNS the fn instead
     * of running it.  The bare-expr thunk path already forbids capturing outer
     * locals (emit_expr.c path (b) "must not capture" limitation), so the
     * synthesized lambda is capture-free and introduces no new Send obligation. */
    if (fn_expr->type.kind != TY_FN) {
        Span sp = call->as.list.items[1]->span;
        Form **fnitems = (Form **)arena_alloc(e->arena, 3 * sizeof(Form *));
        fnitems[0] = form_sym(e->arena, sp, e->sym_fn);
        fnitems[1] = form_vec(e->arena, sp, NULL, 0);      /* [] -- no params */
        fnitems[2] = (Form *)call->as.list.items[1];        /* the original body */
        Form *thunk = form_list(e->arena, sp, fnitems, 3);
        Expr *wrapped = elab_form(e, thunk);
        if (!wrapped) return NULL;
        fn_expr = wrapped;
    }
    
    /* AW-012 / AW-011B-1: Send-check for async closures.
     * Values captured in async blocks must be Send (can be moved to fiber context).
     * T25: Also check that no effect-handler continuation (k) escapes into an async
     * block, which would cause a resume-on-wrong-fiber runtime error. */
    const struct Closure *cl = NULL;
    if (fn_expr->kind == EX_CLOSURE) {
        cl = fn_expr->as.closure_.closure;
    } else if (fn_expr->kind == EX_FN) {
        cl = fn_expr->as.fn_.fn->closure;
    }
    if (cl) {
        bool had_error = false;
        for (uint8_t i = 0; i < cl->n_captures; i++) {
            Binding *cap = cl->captures[i];
            if (!type_is_send(cap->type)) {
                diag_emit_with_code(DIAG_ERROR, call->span,
                    TUR_E0010_NOT_SEND,
                    "type `%s` cannot be sent across thread boundaries "
                    "(not `Send`); captured variable `%s` in async block",
                    type_name(cap->type), cap->name->name);
                had_error = true;
            }
            /* T25: Prevent continuation escape into async scope.
             * A handler continuation k cannot be captured by an async block because
             * the continuation is bound to the fiber where perform was called; resuming
             * it from a different fiber produces a runtime error. Catching this at
             * compile time is safer. */
            if (cap->is_continuation) {
                diag_emit_with_code(DIAG_ERROR, call->span,
                    TUR_E0017_CONT_ESCAPE_ASYNC,
                    "effect handler continuation `%s` cannot be captured by an async block; "
                    "resuming it from a different fiber would cause a runtime error",
                    cap->name->name);
                had_error = true;
            }
        }
        if (had_error) return NULL;
    }

    /* compiled-async-fiber-deadlocks-on-a-session-op: compiled `async` runs
     * its body inline on the spawning thread, so a session op in it blocked
     * that thread on the session runtime's condvar -- and nothing on that
     * thread could then run the peer.  The program hung, with no diagnostic.
     *
     * A body that CAPTURES a session / role endpoint (the op may be inside a
     * callee, `(async (fn [] (server-loop r)))`) is now spawned on its own OS
     * thread (`on_thread`, emit_expr.c / tur_async_thread_via), exactly the
     * shape session-spawn gives a peer, so the peer's ops on this thread and
     * the body's on that one meet in the channel.  Under --interpret the
     * rendezvous is cooperative already.
     *
     * TUR-W0043 stays for the one shape a thread does not help: a body that
     * spells session ops on endpoints it makes itself.  Both ends are then in
     * the same straight-line body, on whichever thread it runs. */
    bool async_on_thread = false;
    if (!g_interpret_mode) {
        const char *cap_name = NULL;
        if (cl) {
            for (uint8_t i = 0; i < cl->n_captures; i++) {
                Binding *cap = cl->captures[i];
                if (cap->type.kind == TY_SESSION || cap->type.kind == TY_ROLE) {
                    cap_name = cap->name->name;
                    break;
                }
            }
        }
        async_on_thread = cap_name != NULL;
        bool lexical = !cap_name && form_mentions_session_op(e, call->as.list.items[1]);
        if (lexical)
            diag_emit_with_code(DIAG_WARNING, call->span,
                TUR_W0043_SESSION_OP_IN_ASYNC,
                "async body performs a session op: compiled `async` runs on "
                "the spawning thread and a session op blocks that thread until "
                "the peer arrives -- unless the peer runs on another OS thread "
                "this deadlocks with no further diagnostic (it runs under "
                "--interpret); run the peer with session-spawn from "
                "stdlib/session.tur instead");
    }

    Expr *out = expr_new(e->arena, EX_ASYNC, TYPE_PTR_VOID, call->span);
    out->as.async_.fn_expr = fn_expr;
    out->as.async_.on_thread = async_on_thread && fn_expr->type.kind == TY_FN;
    /* async-await-payload-is-int64-only: the thunk's declared result is the
     * payload the future carries.  A fn value states it in its type; an
     * expression body (the with-handler thunk shape) IS the value. */
    if (fn_expr->type.kind == TY_FN) {
        out->as.async_.payload = fn_expr->type.as.fn.result_full_type
            ? *fn_expr->type.as.fn.result_full_type
            : type_from_kind(fn_expr->type.as.fn.result_kind);
    } else {
        out->as.async_.payload = fn_expr->type;
    }
    return out;
}

/* async-await-payload-is-int64-only: can this payload type be read back out
 * of the future's int64 slot at its own type?  A word-shaped scalar, a cstr
 * and a raw pointer can (float by bit-reinterpret, the rest by cast).  A
 * by-value aggregate cannot ride the slot at all -- the thunk's C return would
 * not fit an int64 -- and keeps the status-quo `int` read, which cc rejects
 * loudly when it is used as the aggregate (the report's "only honest row"). */
static bool elab_async_payload_rides_slot(const Type *t) {
    if (!t) return false;
    switch (t->kind) {
        case TY_BOOL: case TY_FLOAT: case TY_FLOAT64: case TY_FLOAT32:
        case TY_CSTR: case TY_PTR_VOID:
        case TY_INT: case TY_INT64: case TY_UINT64: case TY_INT32:
        case TY_UINT32: case TY_INT16: case TY_UINT16: case TY_INT8:
        case TY_UINT8: case TY_SYM:
            return true;
        default:
            return false;
    }
}

/* The async payload behind a future expression, when its provenance is
 * visible: the `(async ..)` itself, or a variable a `let`/`def` bound to one
 * (Binding.async_payload), through ascriptions.  NULL when unknown -- a
 * future that arrived through a parameter or a container is an opaque
 * `ptr<void>` and its await keeps the int64 read. */
static const Type *async_payload_of(const Expr *fut) {
    while (fut && fut->kind == EX_ASCRIBE) fut = fut->as.ascribe_.inner;
    if (!fut) return NULL;
    if (fut->kind == EX_ASYNC) return &fut->as.async_.payload;
    if (fut->kind == EX_VAR && fut->as.var.binding)
        return fut->as.var.binding->async_payload;
    return NULL;
}

/* Phase T21-F: (await fut) — block on TurAsyncTask* future; return int64_t. */
Expr *elab_await(Elab *e, const Form *call) {
    if (call->as.list.len != 2) {
        diag_emit(DIAG_ERROR, call->span,
                  "(await fut) requires exactly one argument");
        return NULL;
    }
    /* CF6 (TUR-E0022): conservative Send-across-await check.
     * Every binding currently in scope is treated as live across this await.
     * Any non-Send binding is a soundness hole: the fiber may resume on a
     * different OS thread, racing with e.g. the rc<T> refcount.
     *
     * This used to run only inside an inline `(async (fn [] ...))` closure,
     * gated on an in_async_body flag, because a pre-defined function passed to
     * `(async f)` is elaborated at its own definition site and never
     * re-elaborated here.  That made the check trivially evadable -- hoisting
     * the same body into a named defn and writing `(async f)` silenced it --
     * so a non-Send value could sit live across an await with no diagnostic at
     * all.  cps-async lowering is unconditional now (fixture async-await-cps),
     * so a body containing an await is fiber-resumable regardless of how it
     * reaches `async`, and the check belongs at every await point rather than
     * at the ones the elaborator happens to be walking inside an async form.
     * See docs/archive/async-send-check-skips-predefined-fns.md. */
    {
        bool had_error = false;
        for (Scope *sc = e->scope; sc != NULL; sc = sc->parent) {
            for (uint32_t i = 0; i < sc->n; i++) {
                Binding *b = sc->bindings[i];
                if (!type_is_send(b->type)) {
                    diag_emit_with_code(DIAG_ERROR, call->span,
                        TUR_E0022_AWAIT_LIVE_NOT_SEND,
                        "binding `%s` of type `%s` is not Send and may be live "
                        "across this await point; all bindings in scope at an "
                        "await must be Send (1.0 conservative check)",
                        b->name->name, type_name(b->type));
                    had_error = true;
                }
            }
        }
        if (had_error) return NULL;
    }
    Expr *fut_expr = elab_form(e, call->as.list.items[1]);
    if (!fut_expr) return NULL;
    /* async-await-payload-is-int64-only: read the slot back at the thunk's
     * declared type when the future's provenance says what it is.  The slot
     * itself is still one int64 word: the emitter stores a float's bits and
     * reinterprets them here, the way the fiber effect path already does. */
    Type payload = TYPE_INT;
    const Type *pt = async_payload_of(fut_expr);
    if (pt && elab_async_payload_rides_slot(pt)) payload = *pt;
    Expr *out = expr_new(e->arena, EX_AWAIT, payload, call->span);
    out->as.await_.fut_expr = fut_expr;
    out->as.await_.payload  = payload;
    return out;
}

/* Phase SEL1: (select ((ch :recv v) body) ... (:default body))
 * Waiter-based fair multi-channel select. Returns the value of the selected clause body.
 * All channel expressions must be ptr<void>. Clause bodies must be type-compatible. */
Expr *elab_select(Elab *e, const Form *call) {
    /* call->as.list.items[0] = "select"
     * call->as.list.items[1..n] = clauses
     * Each clause is a 2-element list:
     *   ((chan :recv v) body)   -- recv clause
     *   ((chan :send val) body) -- send clause
     *   (:default body)        -- default arm (only keyword at head, rest is body)
     */
    if (call->as.list.len < 2) {
        diag_emit(DIAG_ERROR, call->span,
                  "(select ...) requires at least one clause");
        return NULL;
    }
    uint32_t n_clauses_raw = call->as.list.len - 1;
    SelectClauseEntry *clauses = (SelectClauseEntry *)arena_alloc(
        e->arena, n_clauses_raw * sizeof(SelectClauseEntry));
    uint32_t n_clauses = 0;
    int has_default = 0;
    Expr *default_body = NULL;
    const Symbol *sym_default = intern_cstr(e->st, "default");

    for (uint32_t ci = 0; ci < n_clauses_raw; ci++) {
        Form *clause_form = call->as.list.items[1 + ci];
        /* Must be a list with exactly 2 items: descriptor and body */
        if (clause_form->tag != F_LIST || clause_form->as.list.len != 2) {
            diag_emit(DIAG_ERROR, clause_form->span,
                      "select clause must be ((chan :recv v) body) or ((chan :send val) body) or (:default body)");
            return NULL;
        }
        Form *desc      = clause_form->as.list.items[0];
        Form *body_form = clause_form->as.list.items[1];

        /* Check for (:default body) */
        if (desc->tag == F_KEYWORD && desc->as.sym == sym_default) {
            if (has_default) {
                diag_emit(DIAG_ERROR, clause_form->span,
                          "select: at most one :default arm is allowed");
                return NULL;
            }
            has_default = 1;
            default_body = elab_form(e, body_form);
            if (!default_body) return NULL;
            continue;
        }

        /* Channel clause: desc must be (chan :recv v) or (chan :send val) */
        if (desc->tag != F_LIST || desc->as.list.len < 2) {
            diag_emit(DIAG_ERROR, desc->span,
                      "select channel descriptor must be (chan :recv binding) or (chan :send val)");
            return NULL;
        }
        Form *chan_form = desc->as.list.items[0];
        Form *op_form   = desc->as.list.items[1];

        if (op_form->tag != F_KEYWORD) {
            diag_emit(DIAG_ERROR, op_form->span,
                      "select clause operation must be :recv or :send");
            return NULL;
        }
        int op;
        if (op_form->as.sym == e->sym_recv) {
            op = 0;
        } else if (op_form->as.sym == e->sym_send) {
            op = 1;
        } else {
            diag_emit(DIAG_ERROR, op_form->span,
                      "select clause operation must be :recv or :send");
            return NULL;
        }

        /* Elaborate channel expression */
        Expr *chan_expr = elab_form(e, chan_form);
        if (!chan_expr) return NULL;

        Expr *send_val = NULL;
        Binding *recv_bind = NULL;

        if (op == 0) {
            /* recv: (chan :recv binding) -- 3 elements */
            if (desc->as.list.len != 3) {
                diag_emit(DIAG_ERROR, desc->span,
                          "select :recv clause descriptor: (chan :recv binding)");
                return NULL;
            }
            Form *bind_form = desc->as.list.items[2];
            if (bind_form->tag != F_SYM) {
                diag_emit(DIAG_ERROR, bind_form->span,
                          "select :recv binding must be a symbol");
                return NULL;
            }
            recv_bind = binding_new(e, bind_form->as.sym, TYPE_INT, false, false, body_form->span);
        } else {
            /* send: (chan :send val) -- 3 elements */
            if (desc->as.list.len != 3) {
                diag_emit(DIAG_ERROR, desc->span,
                          "select :send clause descriptor: (chan :send val)");
                return NULL;
            }
            Form *val_form = desc->as.list.items[2];
            send_val = elab_form(e, val_form);
            if (!send_val) return NULL;
        }

        /* Elaborate body with recv binding in scope (if recv clause) */
        Expr *body_expr;
        if (op == 0 && recv_bind) {
            /* Introduce a scope with the received value binding */
            Scope recv_scope;
            scope_init(&recv_scope, e->scope);
            e->scope = &recv_scope;
            scope_add(&recv_scope, recv_bind);
            body_expr = elab_form(e, body_form);
            e->scope = recv_scope.parent;
            scope_free(&recv_scope);
        } else {
            body_expr = elab_form(e, body_form);
        }
        if (!body_expr) return NULL;

        SelectClauseEntry *sc = &clauses[n_clauses++];
        sc->chan          = chan_expr;
        sc->op            = op;
        sc->send_val      = send_val;
        sc->recv_binding  = recv_bind;
        sc->body          = body_expr;
    }

    if (n_clauses == 0 && !has_default) {
        diag_emit(DIAG_ERROR, call->span,
                  "select: requires at least one channel clause");
        return NULL;
    }

    /* Determine return type from first clause body (or default body) */
    Type result_type = TYPE_INT;
    if (n_clauses > 0) result_type = clauses[0].body->type;
    else if (default_body) result_type = default_body->type;

    Expr *out = expr_new(e->arena, EX_SELECT, result_type, call->span);
    out->as.select_.clauses      = clauses;
    out->as.select_.n_clauses    = n_clauses;
    out->as.select_.has_default  = has_default;
    out->as.select_.default_body = default_body;
    return out;
}

/* Phase R2: (panic msg) — print msg to stderr, then abort.
 * msg must be of type :cstr (or a string literal).
 * Return type is TYPE_NEVER (diverging; caller never observes a value). */
Expr *elab_panic(Elab *e, const Form *call) {
    /* Phase R6b: panic-site linting is handled centrally in elab_call
     * (TUR-W0038, allow-list aware); no per-form lint here. */
    if (call->as.list.len != 2) {
        diag_emit(DIAG_ERROR, call->span,
                  "(panic msg) requires exactly one argument");
        return NULL;
    }
    Expr *payload = elab_form(e, call->as.list.items[1]);
    if (!payload) return NULL;
    Expr *out = expr_new(e->arena, EX_PANIC, TYPE_NEVER, call->span);
    out->as.panic_.payload = payload;
    return out;
}

/* Phase R2: (panic-with payload) — panic with typed payload.
 * payload is any value; its TypeKind is stored for catch-panic-of filtering.
 * Return type is TYPE_NEVER (diverging). */
Expr *elab_panic_with(Elab *e, const Form *call) {
    if (call->as.list.len != 2) {
        diag_emit(DIAG_ERROR, call->span,
                  "(panic-with payload) requires exactly one argument");
        return NULL;
    }
    Expr *payload = elab_form(e, call->as.list.items[1]);
    if (!payload) return NULL;
    Expr *out = expr_new(e->arena, EX_PANIC_WITH, TYPE_NEVER, call->span);
    out->as.panic_with_.payload = payload;
    return out;
}

/* Phase R2/G5: catch-unwind/catch-panic-of yield a result box carried as the
 * int64 carrier stdlib's ok/err return.  The box is { is_ok, ok_val, err_val };
 * the err slot holds the opaque Panic payload pointer.
 *
 * We give the value the *surface* type (Result ThunkRet Panic) rather than the
 * bare :int carrier.  A (Result A B) handle IS that carrier (identical layout,
 * and both are represented as an int64 handle), so the value still coerces to
 * :int for the stdlib ok?/err? predicates (declared [r :int]) and the inline-C
 * result helpers -- exactly as a stdlib `(ok x)` value does.  Surfacing the
 * Result type additionally lets the ok-val/err-val accessors (declared over
 * (Result A B)) extract the payload, which the bare :int carrier blocked (see
 * docs/archive/history/ok-val-on-catch-unwind-result-fails-infer.md).
 *
 * A = the thunk's return type (the ok payload); B = the opaque Panic handle
 * (:ptr<void>).  Falls back to the bare :int carrier when the Result head is
 * not in scope (e.g. stdlib/result.tur not imported), preserving the old
 * behaviour for those programs. */
static Type catch_unwind_result_type(Elab *e, Expr *thunk, Span span) {
    Type *head = elab_lookup_type_by_name(e, intern_cstr(e->st, "Result"));
    if (!head) return TYPE_INT;
    Type ok_ty = TYPE_INT;
    if (thunk && thunk->type.kind == TY_FN) {
        if (thunk->type.as.fn.result_full_type)
            ok_ty = *thunk->type.as.fn.result_full_type;
        else
            ok_ty = type_simple(thunk->type.as.fn.result_kind, CK_COPY);
    }
    /* Keep the err arm a free type variable rather than grounding it to the
     * concrete Panic handle (:ptr<void>).  A fully-concrete (Result A B) is
     * monomorphised into a by-value record struct, but the runtime hands back
     * the int64 carrier box -- so a concrete err arm would mismatch the ABI at
     * the `let` binding.  Leaving B open keeps the value in the carrier
     * representation (identical to how stdlib `(ok x)` flows with a free err
     * arm), while the grounded A still gives ok-val a useful payload type. */
    Type err_ty = type_tyvar_named("__catch_err");
    Type applied_a = type_app(e->arena, *head, ok_ty, span);
    return type_app(e->arena, applied_a, err_ty, span);
}

/* Phase R2: ensure the catch-unwind thunk is a fat closure so the runtime
 * helper can invoke it through the standard closure protocol (TUR_APPLY0).
 * A bare non-capturing (fn [] ...) has type TY_FN with as.fn.boxed=false (a
 * raw C function pointer); auto-shim it into a { fatshim0, orig } box via
 * EX_FN_TO_FAT.  A capturing closure has TY_FN with as.fn.boxed=true (a fat
 * handle whose layout already places the thunk at slot 0); pass it through
 * unchanged so TUR_APPLY0 dispatches to the lifted thunk with the env box
 * as its first argument.  Double-boxing a capturing closure through
 * EX_FN_TO_FAT here previously dropped the env and segfaulted -- see
 * docs/archive/history/catch-unwind-drops-captures-segv.md. */
static Expr *catch_thunk_to_fat(Elab *e, Expr *thunk) {
    if (thunk && thunk->type.kind == TY_FN && !thunk->type.as.fn.boxed) {
        Expr *shim = expr_new(e->arena, EX_FN_TO_FAT, TYPE_PTR_VOID, thunk->span);
        shim->as.fn_to_fat_.inner = thunk;
        return shim;
    }
    return thunk;
}

/* httpd-mw-recover-unblocked-but-unwritten (B): mark a catch boundary's thunk
 * so its env drop-glue does not release captured `^fat` handles -- see the
 * `fat_captures_borrowed` comment on struct Closure. */
static void catch_thunk_mark_borrowed(Expr *thunk) {
    Expr *t = thunk;
    while (t && (t->kind == EX_ASCRIBE || t->kind == EX_FN_TO_FAT)) {
        t = (t->kind == EX_ASCRIBE) ? t->as.ascribe_.inner
                                    : t->as.fn_to_fat_.inner;
    }
    if (t && t->kind == EX_CLOSURE && t->as.closure_.closure)
        t->as.closure_.closure->fat_captures_borrowed = true;
}

/* panic-forms-gaps 4: the thunk of a catch boundary is called with no
 * arguments, and `(catch-unwind 5)` compiled and segfaulted at run time where
 * every other call form rejects a non-callable.  A literal, a scalar other than
 * the int carrier (which is how a closure travels through an unannotated
 * parameter, so `int` stays accepted -- as `resume` accepts it), a by-value
 * struct, or a function that takes arguments is not a thunk. */
static bool catch_thunk_check(const Expr *thunk, const char *who) {
    if (!thunk) return false;
    bool bad = false;
    switch (thunk->kind) {
        case EX_INT_LIT: case EX_FLOAT_LIT: case EX_BOOL_LIT: case EX_CSTR_LIT:
            bad = true; break;
        default: break;
    }
    switch (thunk->type.kind) {
        case TY_FLOAT: case TY_FLOAT32: case TY_FLOAT64: case TY_BOOL:
        case TY_CSTR: case TY_NIL: case TY_STRUCT:
            bad = true; break;
        case TY_FN:
            if (thunk->type.as.fn.arity != 0) bad = true;
            break;
        default: break;
    }
    if (!bad) return true;
    Buf tb; buf_init(&tb); type_print(&tb, thunk->type); buf_putc(&tb, '\0');
    diag_emit_with_code(DIAG_ERROR, thunk->span, TUR_E0001_TYPE_MISMATCH,
                        "%s: the thunk must be a function of no arguments, got %s",
                        who, tb.data);
    buf_free(&tb);
    return false;
}

/* Phase R2: (catch-unwind thunk) — catch any panic at a boundary.
 * thunk is a nullary function; returns result<T, panic-payload>.
 * In v1, result is (Result :int :ptr<void>); lowering uses tur_catch_unwind_box. */
Expr *elab_catch_unwind(Elab *e, const Form *call) {
    /* Phase R6b: catch-unwind is not a panic site under --lint-panic (it is
     * the recovery boundary); no lint here. */
    if (call->as.list.len != 2) {
        diag_emit(DIAG_ERROR, call->span,
                  "(catch-unwind thunk) requires exactly one argument");
        return NULL;
    }
    Expr *thunk = elab_form(e, call->as.list.items[1]);
    if (!thunk) return NULL;
    if (!catch_thunk_check(thunk, "catch-unwind")) return NULL;
    Type result_ty = catch_unwind_result_type(e, thunk, call->span);
    thunk = catch_thunk_to_fat(e, thunk);
    catch_thunk_mark_borrowed(thunk);
    Expr *out = expr_new(e->arena, EX_CATCH_UNWIND, result_ty, call->span);
    out->as.catch_unwind_.thunk = thunk;
    return out;
}

/* Phase R2: (catch-panic-of Type thunk) — catch panics of a specific type.
 * Type is a type identifier (symbol); thunk is a nullary function.
 * Returns result<T, panic-payload> if type matches, otherwise re-panics.
 * In v1, result is ptr<void> and lowering uses tur_catch_panic_of from runtime. */
Expr *elab_catch_panic_of(Elab *e, const Form *call) {
    if (call->as.list.len != 3) {
        diag_emit(DIAG_ERROR, call->span,
                  "(catch-panic-of Type thunk) requires exactly two arguments");
        return NULL;
    }
    /* First arg: type identifier -- accept both a bare symbol (cstr) and a
     * keyword (:cstr); both spell a primitive TypeKind for payload filtering. */
    Form *type_form = call->as.list.items[1];
    TypeKind type_kind = TY_UNKNOWN;
    if (type_form->tag == F_SYM) {
        type_kind = typekind_from_name(type_form->as.sym->name);
    } else if (type_form->tag == F_KEYWORD) {
        type_kind = typekind_from_symbol(type_form->as.sym->name);
    }
    /* Second arg: thunk */
    Expr *thunk = elab_form(e, call->as.list.items[2]);
    if (!thunk) return NULL;
    if (!catch_thunk_check(thunk, "catch-panic-of")) return NULL;
    Type result_ty = catch_unwind_result_type(e, thunk, call->span);
    thunk = catch_thunk_to_fat(e, thunk);
    catch_thunk_mark_borrowed(thunk);
    Expr *out = expr_new(e->arena, EX_CATCH_PANIC_OF, result_ty, call->span);
    out->as.catch_panic_of_.type_kind = type_kind;
    out->as.catch_panic_of_.thunk = thunk;
    return out;
}

/* Phase R2: (panic-payload-type p) — get the TypeKind tag from a panic payload. */
Expr *elab_panic_payload_type(Elab *e, const Form *call) {
    if (call->as.list.len != 2) {
        diag_emit(DIAG_ERROR, call->span,
                  "(panic-payload-type payload) requires exactly one argument");
        return NULL;
    }
    Expr *payload = elab_form(e, call->as.list.items[1]);
    if (!payload) return NULL;
    Expr *out = expr_new(e->arena, EX_PANIC_PAYLOAD_TYPE, TYPE_INT, call->span);
    out->as.panic_payload_type_.payload = payload;
    return out;
}

/* Phase R2: (panic-payload-value p) — get the boxed value from a panic payload. */
Expr *elab_panic_payload_value(Elab *e, const Form *call) {
    if (call->as.list.len != 2) {
        diag_emit(DIAG_ERROR, call->span,
                  "(panic-payload-value payload) requires exactly one argument");
        return NULL;
    }
    Expr *payload = elab_form(e, call->as.list.items[1]);
    if (!payload) return NULL;
    Expr *out = expr_new(e->arena, EX_PANIC_PAYLOAD_VALUE, TYPE_PTR_VOID, call->span);
    out->as.panic_payload_value_.payload = payload;
    return out;
}

/* Phase R2: (panic-payload-file p) — get the source file from a panic payload. */
Expr *elab_panic_payload_file(Elab *e, const Form *call) {
    if (call->as.list.len != 2) {
        diag_emit(DIAG_ERROR, call->span,
                  "(panic-payload-file payload) requires exactly one argument");
        return NULL;
    }
    Expr *payload = elab_form(e, call->as.list.items[1]);
    if (!payload) return NULL;
    Expr *out = expr_new(e->arena, EX_PANIC_PAYLOAD_FILE, TYPE_CSTR, call->span);
    out->as.panic_payload_file_.payload = payload;
    return out;
}

/* Phase R2: (panic-payload-line p) — get the source line from a panic payload. */
Expr *elab_panic_payload_line(Elab *e, const Form *call) {
    if (call->as.list.len != 2) {
        diag_emit(DIAG_ERROR, call->span,
                  "(panic-payload-line payload) requires exactly one argument");
        return NULL;
    }
    Expr *payload = elab_form(e, call->as.list.items[1]);
    if (!payload) return NULL;
    Expr *out = expr_new(e->arena, EX_PANIC_PAYLOAD_LINE, TYPE_INT, call->span);
    out->as.panic_payload_line_.payload = payload;
    return out;
}

/* Phase R2: (panic-payload-downcast p Type) — cast panic payload to a specific type.
 * Returns the boxed value if type matches, NULL otherwise. */
Expr *elab_panic_payload_downcast(Elab *e, const Form *call) {
    if (call->as.list.len != 3) {
        diag_emit(DIAG_ERROR, call->span,
                  "(panic-payload-downcast payload Type) requires exactly two arguments");
        return NULL;
    }
    Expr *payload = elab_form(e, call->as.list.items[1]);
    if (!payload) return NULL;
    /* Second arg: type identifier */
    Form *type_form = call->as.list.items[2];
    TypeKind target_type = TY_UNKNOWN;
    if (type_form->tag == F_SYM) {
        const char *type_name = type_form->as.sym->name;
        target_type = typekind_from_name(type_name);
    } else if (type_form->tag == F_KEYWORD) {
        /* `:cstr`, the spelling catch-panic-of and the guide use.  Only the
         * bare symbol was read, so a keyword left the target TY_UNKNOWN and
         * the downcast could never match. */
        target_type = typekind_from_symbol(type_form->as.sym->name);
    }
    if (target_type == TY_UNKNOWN) {
        diag_emit(DIAG_ERROR, type_form->span,
                  "panic-payload-downcast: unknown payload type -- expected a "
                  "type name such as :cstr or :int");
        return NULL;
    }
    Expr *out = expr_new(e->arena, EX_PANIC_PAYLOAD_DOWNS, TYPE_PTR_VOID, call->span);
    out->as.panic_payload_downs_.payload = payload;
    out->as.panic_payload_downs_.target_type = target_type;
    return out;
}

Expr *elab_stm(Elab *e, const Form *call) {
    if (call->as.list.len < 1) {
        diag_emit(DIAG_ERROR, call->span, "stm requires at least one body expression");
        return NULL;
    }

    /* Save previous stm state */
    bool prev_in_stm = elab_in_stm;
    elab_in_stm = true;

    /* Elaborate body expressions */
    Expr **body = arena_alloc(e->arena, (call->as.list.len - 1) * sizeof(Expr *));
    uint32_t n_body = 0;
    for (uint32_t i = 1; i < call->as.list.len; i++) {
        Expr *expr = elab_form(e, call->as.list.items[i]);
        if (!expr) {
            elab_in_stm = prev_in_stm;
            return NULL;
        }
        body[n_body++] = expr;
    }

    /* Create STM block expression */
    /* The type is the type of the last expression, or NIL if empty */
    Type result_type = TYPE_NIL;
    if (n_body > 0) {
        result_type = body[n_body - 1]->type;
    }
    Expr *result = expr_new(e->arena, EX_STM, result_type, call->span);
    result->as.stm_.body = body;
    result->as.stm_.n_body = n_body;

    /* Restore stm state */
    elab_in_stm = prev_in_stm;

    return result;
}

Expr *elab_atomically(Elab *e, const Form *call) {
    /* The body is VARIADIC.  `(atomically (stm ...))` is still the explicit
     * spelling; anything else is wrapped in an IMPLICIT stm block, so
     * `(atomically (tvar/write tv 7) (tvar/cas tv 7 8))` means what it reads
     * as.  The dispatch row in elab_call.c used to gate on `len == 2`, which
     * dropped a 3-element call out of the special-form table entirely and
     * reported `unknown function or operator 'atomically'` -- a wrong
     * diagnostic for an arity mistake. */
    if (call->as.list.len < 2) {
        diag_emit(DIAG_ERROR, call->span,
                  "atomically requires at least one body expression");
        return NULL;
    }

    uint32_t n = call->as.list.len - 1;

    /* stm-forms-gaps item 3: a nested `atomically` was accepted and ran as a
     * SEPARATE transaction -- the inner one did not see the outer's uncommitted
     * writes, and an inner write committed first and was then overwritten by
     * the outer's commit.  GHC rejects it (the type system keeps STM out of IO),
     * and joining the outer transaction would change what the inner block means,
     * so say so.  Lexical only: a function called from inside an atomically that
     * runs its own is not seen here. */
    if (elab_in_atomically) {
        diag_emit(DIAG_ERROR, call->span,
                  "atomically cannot be nested: the inner block would be a separate "
                  "transaction that does not see the outer one's writes. Put both "
                  "bodies in one `(atomically ...)`, or use the `*-stm` helper "
                  "(TUR-E0009)");
        return NULL;
    }

    /* MS2: Set atomically flag so elab_resume can detect TUR-E0502.
     * elab_in_stm is set too: the body forms are inside the transaction
     * whether or not the user wrote the `(stm ...)` by hand, so `retry`,
     * `check` and `or-else` are in scope for an implicit block. */
    bool prev_in_atomically = elab_in_atomically;
    bool prev_in_stm        = elab_in_stm;
    elab_in_atomically = true;
    elab_in_stm        = true;

    Expr **items = (Expr **)arena_alloc(e->arena, n * sizeof(Expr *));
    for (uint32_t i = 0; i < n; i++) {
        items[i] = elab_form(e, call->as.list.items[1 + i]);
        if (!items[i]) {
            elab_in_atomically = prev_in_atomically;
            elab_in_stm        = prev_in_stm;
            return NULL;
        }
    }

    elab_in_atomically = prev_in_atomically;
    elab_in_stm        = prev_in_stm;

    Expr *stm_expr;
    if (n == 1 && items[0]->kind == EX_STM) {
        /* Explicit `(atomically (stm ...))` -- unchanged. */
        stm_expr = items[0];
    } else {
        /* Implicit transaction block over the body forms. */
        stm_expr = expr_new(e->arena, EX_STM, items[n - 1]->type, call->span);
        stm_expr->as.stm_.body   = items;
        stm_expr->as.stm_.n_body = n;
    }

    Expr *result = expr_new(e->arena, EX_ATOMICALLY, stm_expr->type, call->span);
    result->as.atomically_.stm_expr = stm_expr;
    return result;
}

Expr *elab_retry(Elab *e, const Form *call) {
    if (!elab_in_stm) {
        diag_emit(DIAG_ERROR, call->span, "retry can only be used inside an stm block (TUR-E0009)");
        return NULL;
    }

    Expr *result = expr_new(e->arena, EX_RETRY, TYPE_NIL, call->span);
    return result;
}

Expr *elab_check(Elab *e, const Form *call) {
    if (call->as.list.len != 2) {
        diag_emit(DIAG_ERROR, call->span, "check requires exactly one condition argument");
        return NULL;
    }

    if (!elab_in_stm) {
        diag_emit(DIAG_ERROR, call->span, "check can only be used inside an stm block (TUR-E0009)");
        return NULL;
    }

    Expr *cond = elab_form(e, call->as.list.items[1]);
    if (!cond) return NULL;

    /* Check should return bool */
    if (cond->type.kind != TY_BOOL) {
        diag_emit(DIAG_ERROR, call->span, "check condition must be a bool");
        return NULL;
    }

    Expr *result = expr_new(e->arena, EX_CHECK, TYPE_NIL, call->span);
    result->as.check_.cond = cond;
    return result;
}

Expr *elab_or_else(Elab *e, const Form *call) {
    if (call->as.list.len != 3) {
        diag_emit(DIAG_ERROR, call->span, "or-else requires exactly two stm block arguments");
        return NULL;
    }

    if (!elab_in_stm) {
        diag_emit(DIAG_ERROR, call->span, "or-else can only be used inside an stm block (TUR-E0009)");
        return NULL;
    }

    Expr *stm1 = elab_form(e, call->as.list.items[1]);
    Expr *stm2 = elab_form(e, call->as.list.items[2]);
    if (!stm1 || !stm2) return NULL;

    if (stm1->kind != EX_STM) {
        diag_emit(DIAG_ERROR, call->as.list.items[1]->span, "or-else first argument must be an stm block");
        return NULL;
    }
    if (stm2->kind != EX_STM) {
        diag_emit(DIAG_ERROR, call->as.list.items[2]->span, "or-else second argument must be an stm block");
        return NULL;
    }

    /* or-else result type is the common type of both branches */
    /* For simplicity, use stm2's type (the fallback branch) */
    Type result_type = stm2->type;
    /* If stm2 returns nil, try stm1's type */
    if (result_type.kind == TY_NIL) {
        result_type = stm1->type;
    }

    Expr *result = expr_new(e->arena, EX_OR_ELSE, result_type, call->span);
    result->as.or_else_.stm1 = stm1;
    result->as.or_else_.stm2 = stm2;
    return result;
}

Expr *elab_tvar_new(Elab *e, const Form *call) {
    if (call->as.list.len != 2) {
        diag_emit(DIAG_ERROR, call->span, "TVar/new requires exactly one initial value argument");
        return NULL;
    }

    Expr *init = elab_form(e, call->as.list.items[1]);
    if (!init) return NULL;

    /* TVar::new returns a TVar pointer (ptr), not the type of the initial value */
    Expr *result = expr_new(e->arena, EX_TVAR_NEW, TYPE_PTR_VOID, call->span);
    result->as.tvar_new_.init = init;
    return result;
}

Expr *elab_tvar_read(Elab *e, const Form *call) {
    if (call->as.list.len != 2) {
        diag_emit(DIAG_ERROR, call->span, "TVar/read requires exactly one TVar argument");
        return NULL;
    }

    if (!elab_in_stm) {
        diag_emit(DIAG_ERROR, call->span, "TVar/read can only be used inside an stm block (TUR-E0009)");
        return NULL;
    }

    Expr *tvar = elab_form(e, call->as.list.items[1]);
    if (!tvar) return NULL;

    /* TVar/read returns the value stored in the TVar, which is a ptr<void> */
    Expr *result = expr_new(e->arena, EX_TVAR_READ, TYPE_PTR_VOID, call->span);
    result->as.tvar_read_.tvar = tvar;
    return result;
}

Expr *elab_tvar_write(Elab *e, const Form *call) {
    if (call->as.list.len != 3) {
        diag_emit(DIAG_ERROR, call->span, "TVar/write requires exactly two arguments: tvar and value");
        return NULL;
    }

    if (!elab_in_stm) {
        diag_emit(DIAG_ERROR, call->span, "TVar/write can only be used inside an stm block (TUR-E0009)");
        return NULL;
    }

    Expr *tvar = elab_form(e, call->as.list.items[1]);
    Expr *value = elab_form(e, call->as.list.items[2]);
    if (!tvar || !value) return NULL;

    Expr *result = expr_new(e->arena, EX_TVAR_WRITE, TYPE_NIL, call->span);
    result->as.tvar_write_.tvar = tvar;
    result->as.tvar_write_.value = value;
    return result;
}

Expr *elab_tvar_modify(Elab *e, const Form *call) {
    if (call->as.list.len != 3) {
        diag_emit(DIAG_ERROR, call->span, "TVar/modify requires exactly two arguments: tvar and function");
        return NULL;
    }

    if (!elab_in_stm) {
        diag_emit(DIAG_ERROR, call->span, "TVar/modify can only be used inside an stm block (TUR-E0009)");
        return NULL;
    }

    /* Lower (tvar/modify tv f) to
     *   (let [g tv] (tvar/swap g (f (tvar/read g))))
     * so the function application reuses the ordinary call-dispatch path
     * (fat/thin closures, fn-to-fat shims, typed thunks) instead of a bespoke
     * indirect call.  This makes modify work identically on the compiled and
     * interpreted backends -- a previous direct EX_TVAR_MODIFY emission was a
     * no-op stub on the compiled path (docs/reported/
     * stm-tvar-cas-swap-modify-compiled-path-broken.md).  Binding g evaluates
     * the TVar expression exactly once.  swap(g, f(read(g))) writes f(old) and
     * returns the old value, matching modify's semantics. */
    Span sp = call->span;
    Form *tvar_form = call->as.list.items[1];
    Form *fn_form   = call->as.list.items[2];

    static unsigned modify_gensym_ctr = 0;
    char gbuf[32];
    snprintf(gbuf, sizeof(gbuf), "__tur_modtv_%u", modify_gensym_ctr++);
    const Symbol *g = intern_cstr(e->st, gbuf);
    Form *g_ref = form_sym(e->arena, sp, g);

    /* (tvar/read g) */
    Form *read_items[2] = { form_sym(e->arena, sp, e->sym_tvar_read), g_ref };
    Form *read_form = form_list(e->arena, sp, read_items, 2);

    /* (f (tvar/read g)) */
    Form *call_items[2] = { fn_form, read_form };
    Form *fn_call_form = form_list(e->arena, sp, call_items, 2);

    /* (tvar/swap g (f (tvar/read g))) */
    Form *swap_items[3] = { form_sym(e->arena, sp, e->sym_tvar_swap), g_ref, fn_call_form };
    Form *swap_form = form_list(e->arena, sp, swap_items, 3);

    /* [g tv] */
    Form *bind_items[2] = { g_ref, tvar_form };
    Form *bind_vec = form_vec(e->arena, sp, bind_items, 2);

    /* (let [g tv] (tvar/swap ...)) */
    Form *let_items[3] = { form_sym(e->arena, sp, e->sym_let), bind_vec, swap_form };
    Form *let_form = form_list(e->arena, sp, let_items, 3);

    return elab_form(e, let_form);
}

Expr *elab_tvar_swap(Elab *e, const Form *call) {
    if (call->as.list.len != 3) {
        diag_emit(DIAG_ERROR, call->span, "TVar/swap requires exactly two arguments: tvar and new value");
        return NULL;
    }

    if (!elab_in_stm) {
        diag_emit(DIAG_ERROR, call->span, "TVar/swap can only be used inside an stm block (TUR-E0009)");
        return NULL;
    }

    Expr *tvar = elab_form(e, call->as.list.items[1]);
    Expr *new_val = elab_form(e, call->as.list.items[2]);
    if (!tvar || !new_val) return NULL;

    /* TVar/swap returns the old value, which is a ptr<void> */
    Expr *result = expr_new(e->arena, EX_TVAR_SWAP, TYPE_PTR_VOID, call->span);
    result->as.tvar_swap_.tvar = tvar;
    result->as.tvar_swap_.new_val = new_val;
    return result;
}

Expr *elab_tvar_cas(Elab *e, const Form *call) {
    if (call->as.list.len != 4) {
        diag_emit(DIAG_ERROR, call->span, "TVar/cas requires exactly three arguments: tvar, old value, new value");
        return NULL;
    }

    if (!elab_in_stm) {
        diag_emit(DIAG_ERROR, call->span, "TVar/cas can only be used inside an stm block (TUR-E0009)");
        return NULL;
    }

    Expr *tvar = elab_form(e, call->as.list.items[1]);
    Expr *old_val = elab_form(e, call->as.list.items[2]);
    Expr *new_val = elab_form(e, call->as.list.items[3]);
    if (!tvar || !old_val || !new_val) return NULL;

    Expr *result = expr_new(e->arena, EX_TVAR_CAS, TYPE_BOOL, call->span);
    result->as.tvar_cas_.tvar = tvar;
    result->as.tvar_cas_.old_val = old_val;
    result->as.tvar_cas_.new_val = new_val;
    return result;
}
