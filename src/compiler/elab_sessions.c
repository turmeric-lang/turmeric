/* elab_sessions.c -- session type channel operations (-Xsessions). */
#include "elab_internal.h"

/* SS3a: Substitute all TY_SESSION_REC sentinel nodes (fst==NULL) whose label
 * matches `label` with `replacement`.  Used for one-step equirecursive unrolling.
 * Returns a new arena-allocated Type* with the substitution applied. */
static Type *session_proto_subst(Elab *e, Type *proto, const char *label, Type *replacement) {
    if (!proto) return NULL;
    /* Sentinel: TY_SESSION_REC with fst==NULL and matching label -- replace it. */
    if (proto->kind == TY_SESSION_REC && proto->as.session_.fst == NULL
            && proto->as.session_.label == label) {
        return replacement;
    }
    switch (proto->kind) {
        case TY_CLOSE:
        case TY_UNKNOWN:
            return proto;  /* leaf -- no substitution needed */
        case TY_SEND:
        case TY_RECV: {
            Type *new_fst = session_proto_subst(e, proto->as.session_.fst, label, replacement);
            Type *new_snd = session_proto_subst(e, proto->as.session_.snd, label, replacement);
            if (new_fst == proto->as.session_.fst && new_snd == proto->as.session_.snd)
                return proto;
            Type *r = (Type *)arena_alloc(e->arena, sizeof(Type));
            *r = *proto;
            r->as.session_.fst = new_fst;
            r->as.session_.snd = new_snd;
            return r;
        }
        case TY_CHOOSE:
        case TY_BRANCH: {
            Type *new_fst = session_proto_subst(e, proto->as.session_.fst, label, replacement);
            Type *new_snd = session_proto_subst(e, proto->as.session_.snd, label, replacement);
            if (new_fst == proto->as.session_.fst && new_snd == proto->as.session_.snd)
                return proto;
            Type *r = (Type *)arena_alloc(e->arena, sizeof(Type));
            *r = *proto;
            r->as.session_.fst = new_fst;
            r->as.session_.snd = new_snd;
            return r;
        }
        case TY_TIMEOUT: {
            Type *new_fst = session_proto_subst(e, proto->as.session_.fst, label, replacement);
            Type *new_snd = session_proto_subst(e, proto->as.session_.snd, label, replacement);
            if (new_fst == proto->as.session_.fst && new_snd == proto->as.session_.snd)
                return proto;
            Type *r = (Type *)arena_alloc(e->arena, sizeof(Type));
            *r = *proto;
            r->as.session_.fst = new_fst;
            r->as.session_.snd = new_snd;
            return r;
        }
        case TY_SESSION_REC: {
            /* A different Rec binder -- substitute inside its body but don't
             * shadow the outer label (the inner label is different). */
            if (proto->as.session_.fst == NULL) return proto; /* different sentinel */
            Type *new_body = session_proto_subst(e, proto->as.session_.fst, label, replacement);
            if (new_body == proto->as.session_.fst) return proto;
            Type *r = (Type *)arena_alloc(e->arena, sizeof(Type));
            *r = *proto;
            r->as.session_.fst = new_body;
            return r;
        }
        default:
            return proto;
    }
}

/* SS3a: One-step equirecursive unrolling of a TY_SESSION_REC type.
 * Rec[label, body] -> body[label := Rec[label, body]]
 * Returns the unrolled protocol (arena-allocated). */
static Type *session_rec_unfold(Elab *e, Type *rec_proto) {
    if (!rec_proto || rec_proto->kind != TY_SESSION_REC) return rec_proto;
    if (!rec_proto->as.session_.fst) return rec_proto; /* sentinel -- can't unfold */
    return session_proto_subst(e, rec_proto->as.session_.fst,
                               rec_proto->as.session_.label, rec_proto);
}

/* SS1: Compute the dual of a session protocol type.
 * dual(Send[T,Q])    = Recv[T, dual(Q)]
 * dual(Recv[T,Q])    = Send[T, dual(Q)]
 * dual(Close)        = Close
 * dual(Choose[P,Q])  = Branch[dual(P), dual(Q)]
 * dual(Branch[P,Q])  = Choose[dual(P), dual(Q)]
 * dual(Rec[X,P])     = Rec[X, dual(P)]   (SS3a)
 * dual(Timeout[Q,P]) = Timeout[dual(Q), dual(P)]  (SS3c; self-dual structure)
 * Returns NULL and emits TUR_E0210 if the protocol kind is not dualizable. */
static Type *session_dual(Elab *e, Type *proto, Span span) {
    if (!proto) return NULL;
    switch (proto->kind) {
        case TY_CLOSE: {
            Type *d = (Type *)arena_alloc(e->arena, sizeof(Type));
            *d = type_close();
            return d;
        }
        case TY_SEND: {
            /* dual(Send[T,Q]) = Recv[T, dual(Q)] */
            Type *dual_cont = session_dual(e, proto->as.session_.snd, span);
            if (!dual_cont) return NULL;
            Type *d = (Type *)arena_alloc(e->arena, sizeof(Type));
            *d = type_recv(proto->as.session_.fst, dual_cont);
            return d;
        }
        case TY_RECV: {
            /* dual(Recv[T,Q]) = Send[T, dual(Q)] */
            Type *dual_cont = session_dual(e, proto->as.session_.snd, span);
            if (!dual_cont) return NULL;
            Type *d = (Type *)arena_alloc(e->arena, sizeof(Type));
            *d = type_send(proto->as.session_.fst, dual_cont);
            return d;
        }
        case TY_CHOOSE: {
            /* dual(Choose[P,Q]) = Branch[dual(P), dual(Q)] */
            Type *dp = session_dual(e, proto->as.session_.fst, span);
            if (!dp) return NULL;
            Type *dq = session_dual(e, proto->as.session_.snd, span);
            if (!dq) return NULL;
            Type *d = (Type *)arena_alloc(e->arena, sizeof(Type));
            *d = type_branch(dp, dq);
            return d;
        }
        case TY_BRANCH: {
            /* dual(Branch[P,Q]) = Choose[dual(P), dual(Q)] */
            Type *dp = session_dual(e, proto->as.session_.fst, span);
            if (!dp) return NULL;
            Type *dq = session_dual(e, proto->as.session_.snd, span);
            if (!dq) return NULL;
            Type *d = (Type *)arena_alloc(e->arena, sizeof(Type));
            *d = type_choose(dp, dq);
            return d;
        }
        case TY_SESSION_REC: {
            /* SS3a: dual(Rec[X, P]) = Rec[X, dual(P)].
             * A sentinel (fst==NULL) means a back-reference -- dual of it is the
             * same sentinel (will be resolved by the outer Rec dual). */
            if (!proto->as.session_.fst) {
                /* sentinel back-reference: return as-is (label preserved) */
                Type *d = (Type *)arena_alloc(e->arena, sizeof(Type));
                *d = *proto;
                return d;
            }
            Type *dual_body = session_dual(e, proto->as.session_.fst, span);
            if (!dual_body) return NULL;
            Type *d = (Type *)arena_alloc(e->arena, sizeof(Type));
            *d = type_session_rec(proto->as.session_.label, dual_body);
            return d;
        }
        case TY_TIMEOUT: {
            /* SS3c: Timeout is self-dual in structure.
             * dual(Timeout[Q, P]) = Timeout[dual(Q), dual(P)] */
            Type *dq = session_dual(e, proto->as.session_.fst, span);
            if (!dq) return NULL;
            Type *dp = session_dual(e, proto->as.session_.snd, span);
            if (!dp) return NULL;
            Type *d = (Type *)arena_alloc(e->arena, sizeof(Type));
            *d = type_timeout(dq, dp);
            return d;
        }
        default:
            diag_emit_with_code(DIAG_ERROR, span, TUR_E0210_SESSION_NOT_DUAL,
                                "cannot compute dual of protocol '%s'",
                                type_name(*proto));
            return NULL;
    }
}

/* SS2: Build an EX_INLINE_C node with substitution placeholders.
 * code_str/code_len: the template (may contain __TUR_VAL_N__)
 * ret_type: the Turmeric type of the expression
 * chan_binding: if non-NULL, stored as EX_VAR in val_exprs[0] for __TUR_VAL_0__
 * chan_fallback: EF-4 -- when chan_binding is NULL but the channel operand is a
 *   non-variable expression (e.g. a Session/Role struct-field read `(.s box)`),
 *   the elaborated operand expr is stored in val_exprs[0] directly.  Pass NULL
 *   for a template that carries no __TUR_VAL_0__ (e.g. make-session). */
static Expr *session_inline_c(Elab *e, const char *code_str, uint32_t code_len,
                               Type ret_type, Binding *chan_binding,
                               Expr *chan_fallback, Span span) {
    Expr *out = expr_new(e->arena, EX_INLINE_C, ret_type, span);
    InlineC *ic = (InlineC *)arena_alloc(e->arena, sizeof(InlineC));
    memset(ic, 0, sizeof(InlineC));
    ic->code = strslice(code_str, code_len);
    ic->return_type = ret_type;
    ic->captures = NULL;
    ic->n_captures = 0;
    if (chan_binding) {
        ic->val_exprs = (Expr **)arena_alloc(e->arena, sizeof(Expr *));
        Expr *chan_var = expr_new(e->arena, EX_VAR, chan_binding->type, span);
        chan_var->as.var.binding = chan_binding;
        ic->val_exprs[0] = chan_var;
        ic->n_val_exprs = 1;
    } else if (chan_fallback) {
        /* EF-4: no plain-variable binding, but a real channel operand expr (a
         * field read).  Substitute it for __TUR_VAL_0__ so a Session/Role field
         * endpoint can be closed/received directly, instead of leaving val_exprs
         * empty (which left __TUR_VAL_0__ unsubstituted / crashed emit). */
        ic->val_exprs = (Expr **)arena_alloc(e->arena, sizeof(Expr *));
        ic->val_exprs[0] = chan_fallback;
        ic->n_val_exprs = 1;
    } else {
        ic->val_exprs = NULL;
        ic->n_val_exprs = 0;
    }
    out->as.inline_c_.inline_c = ic;
    return out;
}

/* (make-session P) — create a dual pair [Session[P], Session[Dual[P]]].
 * Parses the protocol type P, computes dual(P), and returns a
 * TY_SESSION_PAIR expression so that vector destructuring can assign
 * the correct Session types to each binding. The actual emission
 * (tur_session_new() for fst, __TUR_CAP_0__ for snd) happens when
 * elab_forms.c splits the pair during destructuring. */
Expr *elab_session_make(Elab *e, const Form *call) {
    if (call->as.list.len != 2) {
        diag_emit(DIAG_ERROR, call->span,
                  "make-session requires a protocol: (make-session Protocol)");
        return NULL;
    }

    /* Parse the protocol type expression (e.g. (Send int Close), Close, ...) */
    Type *proto = type_expr_from_form(e, call->as.list.items[1],
                                      NULL, NULL, NULL, 0);
    if (!proto) return NULL;

    /* Compute dual(P) — emits TUR_E0210 on failure */
    Type *dual_proto = session_dual(e, proto, call->span);
    if (!dual_proto) return NULL;

    /* Build Session[P] and Session[dual(P)] */
    Type *sess_p = (Type *)arena_alloc(e->arena, sizeof(Type));
    *sess_p = type_session(proto);
    Type *sess_dp = (Type *)arena_alloc(e->arena, sizeof(Type));
    *sess_dp = type_session(dual_proto);

    /* Return a TY_SESSION_PAIR node. SS2: elab_forms.c splits this into two
     * separate EX_INLINE_C init expressions: tur_session_new() for fst and
     * __TUR_CAP_0__ (referencing fst's binding) for snd. */
    Type pair_type = type_session_pair(sess_p, sess_dp);
    return session_inline_c(e, "/*make-session*/", 16, pair_type, NULL, NULL, call->span);
}

/* Helper: validate that an expression is a Session[...] channel and extract
 * its inner protocol type.  On failure, emits a diagnostic and returns NULL.
 * SS3a: If the protocol is TY_SESSION_REC, unroll it one step (equirecursive). */
static Type *session_protocol_of(Elab *e, Expr *chan, const char *op, Span span) {
    if (!chan) return NULL;
    if (chan->type.kind != TY_SESSION) {
        diag_emit_with_code(DIAG_ERROR, span, TUR_E0212_SESSION_PROTO_MISMATCH,
                            "%s requires a Session[...] channel, got %s",
                            op, type_name(chan->type));
        return NULL;
    }
    Type *proto = chan->type.as.session_.fst;
    /* SS3a: Equirecursive unrolling -- transparently unfold Rec at use sites. */
    while (proto && proto->kind == TY_SESSION_REC && proto->as.session_.fst != NULL) {
        proto = session_rec_unfold(e, proto);
    }
    return proto;
}

/* --- Payload lowering onto the int64 rendezvous word ------------------------
 * (session-payloads-are-int64-only).  Classified from the payload's C
 * spelling rather than a TypeKind table so every pointer-lowered kind
 * (cstr, ptr<T>, Session, Role, rc, opaque-over-pointer, :heap records, ...)
 * lands in the same arm without enumerating them here. */
SessPayloadClass session_payload_class(Type t) {
    const char *cn = type_c_name(t);
    if (!cn || !*cn) return SESS_PAY_INT;
    if (strcmp(cn, "double") == 0) return SESS_PAY_F64;
    if (strcmp(cn, "float")  == 0) return SESS_PAY_F32;
    size_t n = strlen(cn);
    if (cn[n - 1] == '*') return SESS_PAY_PTR;
    /* A user aggregate that lowers to a by-value C struct (`tur_adt_Pt`,
     * `Name`) has no word-sized carrier.  Everything else that reaches here
     * (int64_t, bool, sized ints, int-carried handles, void) is a word. */
    if ((t.kind == TY_STRUCT || t.kind == TY_ADT || t.kind == TY_APP) &&
        strcmp(cn, "int64_t") != 0)
        return SESS_PAY_UNSUPPORTED;
    return SESS_PAY_INT;
}

bool session_payload_supported(Elab *e, Type t, Span span, const char *op) {
    (void)e;
    if (session_payload_class(t) != SESS_PAY_UNSUPPORTED) return true;
    diag_emit_with_code(DIAG_ERROR, span, TUR_E0212_SESSION_PROTO_MISMATCH,
                        "%s: session payload type %s is a by-value aggregate; "
                        "the session runtime carries one machine word per "
                        "message -- send it behind a pointer (rc<%s>, ref<%s>, "
                        "or a :heap record) instead",
                        op, type_name(t), type_name(t), type_name(t));
    return false;
}

static const char *session_payload_wrap(Elab *e, const char *pre, const char *inner,
                                        size_t inner_len, const char *post) {
    size_t need = strlen(pre) + inner_len + strlen(post) + 1;
    char *s = (char *)arena_alloc(e->arena, need);
    memcpy(s, pre, strlen(pre));
    memcpy(s + strlen(pre), inner, inner_len);
    memcpy(s + strlen(pre) + inner_len, post, strlen(post) + 1);
    return s;
}

const char *session_payload_to_word(Elab *e, Type t, const char *inner, size_t inner_len) {
    switch (session_payload_class(t)) {
        case SESS_PAY_F64: return session_payload_wrap(e, "tur_session_f64_bits(", inner, inner_len, ")");
        case SESS_PAY_F32: return session_payload_wrap(e, "tur_session_f32_bits(", inner, inner_len, ")");
        case SESS_PAY_PTR: return session_payload_wrap(e, "(int64_t)(intptr_t)(", inner, inner_len, ")");
        default:           return session_payload_wrap(e, "(int64_t)(", inner, inner_len, ")");
    }
}

/* The interpreter (src/turi/eval.c, eval_session_intercept) peels exactly
 * these three wrapper spellings off the recv templates before matching them
 * by prefix; keep the two in sync. */
const char *session_payload_from_word(Elab *e, Type t, const char *inner, size_t inner_len) {
    switch (session_payload_class(t)) {
        case SESS_PAY_F64: return session_payload_wrap(e, "tur_session_bits_f64(", inner, inner_len, ")");
        case SESS_PAY_F32: return session_payload_wrap(e, "tur_session_bits_f32(", inner, inner_len, ")");
        case SESS_PAY_PTR: return session_payload_wrap(e, "(void *)(intptr_t)", inner, inner_len, "");
        default:           return session_payload_wrap(e, "", inner, inner_len, "");
    }
}

/* (send chan val) — consume chan : Session[Send[T, Q]]; return chan' : Session[Q].
 * Emits TUR-E0212 if chan's protocol is not Send[...]. */
Expr *elab_session_send(Elab *e, const Form *call) {
    if (call->as.list.len != 3) {
        diag_emit(DIAG_ERROR, call->span,
                  "send requires two arguments: (send chan val)");
        return NULL;
    }
    Expr *chan = elab_form(e, call->as.list.items[1]);
    if (!chan) return NULL;
    Expr *val  = elab_form(e, call->as.list.items[2]);
    if (!val) return NULL;

    Type *proto = session_protocol_of(e, chan, "send", call->span);
    if (!proto) return NULL;

    if (proto->kind != TY_SEND) {
        diag_emit_with_code(DIAG_ERROR, call->span, TUR_E0212_SESSION_PROTO_MISMATCH,
                            "send is not valid on Session[%s] -- "
                            "this endpoint's next action is not a send",
                            type_name(*proto));
        return NULL;
    }

    /* The payload must fit the runtime's one-word slot (see
     * session_payload_class); reject a by-value aggregate here rather than
     * from cc. */
    Type *payload = proto->as.session_.fst;
    if (payload && !session_payload_supported(e, *payload, call->span, "send"))
        return NULL;

    /* Mark chan as consumed (it's linear) */
    Binding *chan_binding = (chan->kind == EX_VAR) ? chan->as.var.binding : NULL;
    if (chan_binding) binding_mark_moved(chan_binding, call->span);

    /* Build Session[Q] result type */
    Type *cont_proto = proto->as.session_.snd;
    Type *cont_proto_alloc = (Type *)arena_alloc(e->arena, sizeof(Type));
    *cont_proto_alloc = *cont_proto;
    Type sess_q = type_session(cont_proto_alloc);
    Expr *sess_q_alloc = expr_new(e->arena, EX_INLINE_C, sess_q, call->span);

    /* SS2: emit a GNU statement-expression that sends and returns the channel.
     * val_exprs[0] = EX_VAR(chan_binding), val_exprs[1] = val
     *
     * Bare `({ ... })`, NOT `__extension__ ({ ... })`: the prefix only silences
     * -pedantic, which the generated-C compile (-O2 -std=c99 -Wall) never sets,
     * and c2mir has no __extension__ keyword -- so it cost the JIT every
     * session fixture on any libc whose headers do not #define it away.  See
     * docs/archive/history/jit-macos-full-corpus-extension-and-atexit.md.  The
     * interpreter matches this text by prefix in src/turi/eval.c; keep the two
     * in sync.
     *
     * The payload operand is lowered onto the int64 slot by
     * session_payload_to_word: `(int64_t)(x)` for ints, a bit-reinterpret for
     * floats (a value cast truncated 7.25 to 7), `(int64_t)(intptr_t)(x)` for
     * pointers (session-payloads-are-int64-only). */
    const char *word = session_payload_to_word(
        e, payload ? *payload : val->type, "__TUR_VAL_1__", strlen("__TUR_VAL_1__"));
    static const char send_pre[]  = "({ tur_session_send(__TUR_VAL_0__, ";
    static const char send_post[] = "); (void *)__TUR_VAL_0__; })";
    size_t send_len = sizeof(send_pre) - 1 + strlen(word) + sizeof(send_post) - 1;
    char *send_code = (char *)arena_alloc(e->arena, send_len + 1);
    snprintf(send_code, send_len + 1, "%s%s%s", send_pre, word, send_post);
    InlineC *ic = (InlineC *)arena_alloc(e->arena, sizeof(InlineC));
    memset(ic, 0, sizeof(InlineC));
    ic->code = strslice(send_code, (uint32_t)send_len);
    ic->return_type = sess_q;
    ic->captures = NULL; ic->n_captures = 0;
    ic->val_exprs = (Expr **)arena_alloc(e->arena, 2 * sizeof(Expr *));
    ic->n_val_exprs = 2;
    if (chan_binding) {
        Expr *chan_var = expr_new(e->arena, EX_VAR, chan_binding->type, call->span);
        chan_var->as.var.binding = chan_binding;
        ic->val_exprs[0] = chan_var;
    } else {
        /* EF-4: the channel operand is not a plain variable (e.g. a Session
         * struct-field read `(.s box)`).  Use the elaborated operand expr
         * directly so __TUR_VAL_0__ substitutes to the field access -- a NULL
         * here crashed emit_value.  A Session-field read emits `(void *)(box).s`,
         * which flows into tur_session_send's TurChannel* param unchanged. */
        ic->val_exprs[0] = chan;
    }
    ic->val_exprs[1] = val;
    sess_q_alloc->as.inline_c_.inline_c = ic;
    return sess_q_alloc;
}

/* (recv chan) — consume chan : Session[Recv[T, Q]]; return placeholder for [T, Session[Q]].
 * Emits TUR-E0212 if chan's protocol is not Recv[...]. */
Expr *elab_session_recv(Elab *e, const Form *call) {
    if (call->as.list.len != 2) {
        diag_emit(DIAG_ERROR, call->span,
                  "recv requires one argument: (recv chan)");
        return NULL;
    }
    Expr *chan = elab_form(e, call->as.list.items[1]);
    if (!chan) return NULL;

    Type *proto = session_protocol_of(e, chan, "recv", call->span);
    if (!proto) return NULL;

    if (proto->kind != TY_RECV) {
        diag_emit_with_code(DIAG_ERROR, call->span, TUR_E0212_SESSION_PROTO_MISMATCH,
                            "recv is not valid on Session[%s] -- "
                            "this endpoint's next action is not a recv",
                            type_name(*proto));
        return NULL;
    }

    /* Mark chan as consumed */
    Binding *chan_binding = (chan->kind == EX_VAR) ? chan->as.var.binding : NULL;
    if (chan_binding) binding_mark_moved(chan_binding, call->span);

    /* SS2: return TY_SESSION_RECV_PAIR so vector destructuring can split it.
     * fst = value type T; snd = Session[Q] (continuation).
     * captures[0] = chan_binding — used by elab_forms.c to create separate inits. */
    Type *val_tp = (Type *)arena_alloc(e->arena, sizeof(Type));
    *val_tp = *proto->as.session_.fst;  /* T */
    Type *cont_proto = proto->as.session_.snd;
    Type *cont_proto_alloc = (Type *)arena_alloc(e->arena, sizeof(Type));
    *cont_proto_alloc = *cont_proto;
    Type *sess_q = (Type *)arena_alloc(e->arena, sizeof(Type));
    *sess_q = type_session(cont_proto_alloc);  /* Session[Q] */
    Type pair_type = type_session_recv_pair(val_tp, sess_q);
    return session_inline_c(e, "/*recv-pair*/", 13, pair_type, chan_binding, NULL, call->span);
}

/* (close chan) — consume chan : Session[Close]; return nil.
 * Emits TUR-E0212 if chan's protocol is not Close.
 * SS5: also handles TY_ROLE endpoints (dispatches to elab_role_close). */
Expr *elab_session_close(Elab *e, const Form *call) {
    if (call->as.list.len != 2) {
        diag_emit(DIAG_ERROR, call->span,
                  "close requires one argument: (close chan)");
        return NULL;
    }
    /* SS5: Dispatch TY_ROLE close to elab_role_close (handled in elab_global.c).
     * Note: elab_role_close will re-elaborate the argument, so we pass the call form. */
    Expr *chan = elab_form(e, call->as.list.items[1]);
    if (!chan) return NULL;
    if (chan->type.kind == TY_ROLE) {
        /* The chan is already elaborated; inline the role-close logic here. */
        const char *role_name = chan->type.as.role_.role_name;
        /* SS8: skip bystander steps before checking for protocol end */
        GlobalInteraction *step =
            role_skip_bystander_steps(chan->type.as.role_.current_step, role_name);
        if (!step || step->kind != GI_END) {
            const char *step_name = "unknown";
            if (step) {
                if (step->kind == GI_MSG)      step_name = "->";
                else if (step->kind == GI_CHOICE) step_name = "choice";
                else if (step->kind == GI_LOOP)   step_name = "loop";
                else if (step->kind == GI_CONTINUE) step_name = "continue";
                else if (step->kind == GI_TIMEOUT)  step_name = "timeout";
            }
            diag_emit_with_code(DIAG_ERROR, call->span, TUR_E0212_SESSION_PROTO_MISMATCH,
                                "close: role '%s' cannot close the protocol here -- "
                                "current step is %s, expected end of protocol",
                                chan->type.as.role_.role_name, step_name);
            return NULL;
        }
        Binding *chan_binding = (chan->kind == EX_VAR) ? chan->as.var.binding : NULL;
        if (chan_binding) binding_mark_moved(chan_binding, call->span);
        /* SS7: emit tur_role_close(chan) */
        static const char role_close_code[] = "tur_role_close((void *)__TUR_VAL_0__)";
        Type nil_t = TYPE_NIL;
        return session_inline_c(e, role_close_code, sizeof(role_close_code) - 1,
                                nil_t, chan_binding, chan, call->span);
    }

    Type *proto = session_protocol_of(e, chan, "close", call->span);
    if (!proto) return NULL;

    /* SS3c: Also allow close on Session[Timeout[Close, Close]].
     * When the sender has sent and both timeout branches are Close, the sender
     * can transparently close regardless of which branch the receiver took. */
    bool is_trivial_timeout = (proto->kind == TY_TIMEOUT &&
        proto->as.session_.fst && proto->as.session_.fst->kind == TY_CLOSE &&
        proto->as.session_.snd && proto->as.session_.snd->kind == TY_CLOSE);

    if (proto->kind != TY_CLOSE && !is_trivial_timeout) {
        diag_emit_with_code(DIAG_ERROR, call->span, TUR_E0212_SESSION_PROTO_MISMATCH,
                            "close is not valid on Session[%s] -- "
                            "protocol is not yet complete",
                            type_name(*proto));
        return NULL;
    }

    /* Mark chan as consumed */
    Binding *chan_binding = (chan->kind == EX_VAR) ? chan->as.var.binding : NULL;
    if (chan_binding) binding_mark_moved(chan_binding, call->span);

    /* SS2: call tur_session_close(__TUR_VAL_0__); return nil. */
    static const char close_code[] = "tur_session_close(__TUR_VAL_0__)";
    return session_inline_c(e, close_code, sizeof(close_code) - 1,
                             TYPE_NIL, chan_binding, chan, call->span);
}

/* (offer chan) — consume chan : Session[Branch[P, Q]]; return Either(Session[P], Session[Q]).
 * Emits TUR-E0212 if chan's protocol is not Branch[...]. */
Expr *elab_session_offer(Elab *e, const Form *call) {
    if (call->as.list.len != 2) {
        diag_emit(DIAG_ERROR, call->span,
                  "offer requires one argument: (offer chan)");
        return NULL;
    }
    Expr *chan = elab_form(e, call->as.list.items[1]);
    if (!chan) return NULL;

    Type *proto = session_protocol_of(e, chan, "offer", call->span);
    if (!proto) return NULL;

    if (proto->kind != TY_BRANCH) {
        diag_emit_with_code(DIAG_ERROR, call->span, TUR_E0212_SESSION_PROTO_MISMATCH,
                            "offer is not valid on Session[%s] -- "
                            "only Branch sessions support offer",
                            type_name(*proto));
        return NULL;
    }

    /* Mark chan as consumed */
    Binding *chan_binding = (chan->kind == EX_VAR) ? chan->as.var.binding : NULL;
    if (chan_binding) binding_mark_moved(chan_binding, call->span);

    /* SS2: receive a branch tag from the peer's choose-left/choose-right.
     * Result type TY_SESSION_OFFER: fst=Session[P] (Left), snd=Session[Q] (Right).
     * The tag value (0 or 1) is used by match emit to select the branch arm.
     * The channel is still live as captures[0] for arm binding. */
    Type *left_proto = proto->as.session_.fst;   /* P in Branch[P,Q] */
    Type *right_proto = proto->as.session_.snd;  /* Q in Branch[P,Q] */
    Type *left_proto_alloc  = (Type *)arena_alloc(e->arena, sizeof(Type));
    *left_proto_alloc = *left_proto;
    Type *right_proto_alloc = (Type *)arena_alloc(e->arena, sizeof(Type));
    *right_proto_alloc = *right_proto;
    Type *sess_p = (Type *)arena_alloc(e->arena, sizeof(Type));
    *sess_p = type_session(left_proto_alloc);   /* Session[P] */
    Type *sess_q = (Type *)arena_alloc(e->arena, sizeof(Type));
    *sess_q = type_session(right_proto_alloc);  /* Session[Q] */
    Type offer_type = type_session_offer(sess_p, sess_q);
    static const char offer_code[] = "tur_session_recv_tag(__TUR_VAL_0__)";
    return session_inline_c(e, offer_code, sizeof(offer_code) - 1,
                             offer_type, chan_binding, NULL, call->span);
}

/* (choose-left chan) — consume chan : Session[Choose[P, Q]]; return Session[P].
 * Emits TUR-E0212 if chan's protocol is not Choose[...]. */
Expr *elab_session_choose_left(Elab *e, const Form *call) {
    if (call->as.list.len != 2) {
        diag_emit(DIAG_ERROR, call->span,
                  "choose-left requires one argument: (choose-left chan)");
        return NULL;
    }
    Expr *chan = elab_form(e, call->as.list.items[1]);
    if (!chan) return NULL;

    Type *proto = session_protocol_of(e, chan, "choose-left", call->span);
    if (!proto) return NULL;

    if (proto->kind != TY_CHOOSE) {
        diag_emit_with_code(DIAG_ERROR, call->span, TUR_E0212_SESSION_PROTO_MISMATCH,
                            "choose-left is not valid on Session[%s] -- "
                            "only Choose sessions support choose-left",
                            type_name(*proto));
        return NULL;
    }

    /* Mark chan as consumed */
    Binding *chan_binding = (chan->kind == EX_VAR) ? chan->as.var.binding : NULL;
    if (chan_binding) binding_mark_moved(chan_binding, call->span);

    /* SS2: send tag=0 to peer and return channel as Session[P]. */
    Type *left_proto = proto->as.session_.fst;
    Type *left_proto_alloc = (Type *)arena_alloc(e->arena, sizeof(Type));
    *left_proto_alloc = *left_proto;
    Type sess_p = type_session(left_proto_alloc);
    Expr *out = expr_new(e->arena, EX_INLINE_C, sess_p, call->span);
    static const char choose_left_code[] =
        "({ tur_session_send_tag(__TUR_VAL_0__, (int64_t)0); (void *)__TUR_VAL_0__; })";
    InlineC *ic = (InlineC *)arena_alloc(e->arena, sizeof(InlineC));
    memset(ic, 0, sizeof(InlineC));
    ic->code = strslice(choose_left_code, sizeof(choose_left_code) - 1);
    ic->return_type = sess_p;
    ic->captures = NULL; ic->n_captures = 0;
    if (chan_binding) {
        ic->val_exprs = (Expr **)arena_alloc(e->arena, sizeof(Expr *));
        Expr *chan_var = expr_new(e->arena, EX_VAR, chan_binding->type, call->span);
        chan_var->as.var.binding = chan_binding;
        ic->val_exprs[0] = chan_var;
        ic->n_val_exprs = 1;
    } else {
        /* EF-4: field-read (non-variable) channel operand -- substitute the
         * elaborated expr for __TUR_VAL_0__ rather than leaving it unbound. */
        ic->val_exprs = (Expr **)arena_alloc(e->arena, sizeof(Expr *));
        ic->val_exprs[0] = chan;
        ic->n_val_exprs = 1;
    }
    out->as.inline_c_.inline_c = ic;
    return out;
}

/* (choose-right chan) — consume chan : Session[Choose[P, Q]]; return Session[Q].
 * Emits TUR-E0212 if chan's protocol is not Choose[...]. */
Expr *elab_session_choose_right(Elab *e, const Form *call) {
    if (call->as.list.len != 2) {
        diag_emit(DIAG_ERROR, call->span,
                  "choose-right requires one argument: (choose-right chan)");
        return NULL;
    }
    Expr *chan = elab_form(e, call->as.list.items[1]);
    if (!chan) return NULL;

    Type *proto = session_protocol_of(e, chan, "choose-right", call->span);
    if (!proto) return NULL;

    if (proto->kind != TY_CHOOSE) {
        diag_emit_with_code(DIAG_ERROR, call->span, TUR_E0212_SESSION_PROTO_MISMATCH,
                            "choose-right is not valid on Session[%s] -- "
                            "only Choose sessions support choose-right",
                            type_name(*proto));
        return NULL;
    }

    /* Mark chan as consumed */
    Binding *chan_binding = (chan->kind == EX_VAR) ? chan->as.var.binding : NULL;
    if (chan_binding) binding_mark_moved(chan_binding, call->span);

    /* SS2: send tag=1 to peer and return channel as Session[Q]. */
    Type *right_proto = proto->as.session_.snd;
    Type *right_proto_alloc = (Type *)arena_alloc(e->arena, sizeof(Type));
    *right_proto_alloc = *right_proto;
    Type sess_q = type_session(right_proto_alloc);
    Expr *out = expr_new(e->arena, EX_INLINE_C, sess_q, call->span);
    static const char choose_right_code[] =
        "({ tur_session_send_tag(__TUR_VAL_0__, (int64_t)1); (void *)__TUR_VAL_0__; })";
    InlineC *ic = (InlineC *)arena_alloc(e->arena, sizeof(InlineC));
    memset(ic, 0, sizeof(InlineC));
    ic->code = strslice(choose_right_code, sizeof(choose_right_code) - 1);
    ic->return_type = sess_q;
    ic->captures = NULL; ic->n_captures = 0;
    if (chan_binding) {
        ic->val_exprs = (Expr **)arena_alloc(e->arena, sizeof(Expr *));
        Expr *chan_var = expr_new(e->arena, EX_VAR, chan_binding->type, call->span);
        chan_var->as.var.binding = chan_binding;
        ic->val_exprs[0] = chan_var;
        ic->n_val_exprs = 1;
    } else {
        /* EF-4: field-read (non-variable) channel operand -- substitute the
         * elaborated expr for __TUR_VAL_0__ rather than leaving it unbound. */
        ic->val_exprs = (Expr **)arena_alloc(e->arena, sizeof(Expr *));
        ic->val_exprs[0] = chan;
        ic->n_val_exprs = 1;
    }
    out->as.inline_c_.inline_c = ic;
    return out;
}

/* SS3c: (recv-timeout chan duration) -- timed receive on a session channel.
 * chan     : Session[Recv[T, Timeout[Q, P]]]
 * duration : int (milliseconds)
 * Returns  : TY_SESSION_OFFER where fst = TY_SESSION_RECV_PAIR[T, Session[Q]]
 *                                   snd = Session[P]
 *
 * Emits tur_session_recv_timeout(chan, dur_ms) which returns
 * 0 (success, value stashed in tur__rtv_ thread-local) or 1 (timeout).
 *
 * The match Left arm gets a TY_SESSION_RECV_PAIR which the user can
 * vector-destructure to [value, chan2].  The match Right arm gets Session[P]. */
Expr *elab_session_recv_timeout(Elab *e, const Form *call) {
    if (call->as.list.len != 3) {
        diag_emit(DIAG_ERROR, call->span,
                  "recv-timeout requires two arguments: (recv-timeout chan duration)");
        return NULL;
    }
    Expr *chan = elab_form(e, call->as.list.items[1]);
    if (!chan) return NULL;
    Expr *dur  = elab_form(e, call->as.list.items[2]);
    if (!dur) return NULL;

    Type *proto = session_protocol_of(e, chan, "recv-timeout", call->span);
    if (!proto) return NULL;

    /* Expect Recv[T, Timeout[Q, P]] */
    if (proto->kind != TY_RECV) {
        diag_emit_with_code(DIAG_ERROR, call->span, TUR_E0212_SESSION_PROTO_MISMATCH,
                            "recv-timeout is not valid on Session[%s] -- "
                            "expected Recv[T, Timeout[Q, P]]",
                            type_name(*proto));
        return NULL;
    }
    Type *cont = proto->as.session_.snd;
    if (!cont || cont->kind != TY_TIMEOUT) {
        diag_emit_with_code(DIAG_ERROR, call->span, TUR_E0212_SESSION_PROTO_MISMATCH,
                            "recv-timeout: continuation must be Timeout[Q, P], got %s",
                            cont ? type_name(*cont) : "?");
        return NULL;
    }

    /* Mark chan as consumed */
    Binding *chan_binding = (chan->kind == EX_VAR) ? chan->as.var.binding : NULL;
    if (chan_binding) binding_mark_moved(chan_binding, call->span);

    /* Build types:
     *   T   = proto->fst
     *   Q   = cont->fst  (success continuation protocol)
     *   P   = cont->snd  (timeout continuation protocol)
     *   Left  arm type = RecvPair[T, Session[Q]]
     *   Right arm type = Session[P] */
    Type *val_tp = (Type *)arena_alloc(e->arena, sizeof(Type));
    *val_tp = *proto->as.session_.fst;  /* T */

    Type *q_proto = (Type *)arena_alloc(e->arena, sizeof(Type));
    *q_proto = *cont->as.session_.fst;  /* Q */
    Type *sess_q = (Type *)arena_alloc(e->arena, sizeof(Type));
    *sess_q = type_session(q_proto);    /* Session[Q] */

    Type *p_proto = (Type *)arena_alloc(e->arena, sizeof(Type));
    *p_proto = *cont->as.session_.snd;  /* P */
    Type *sess_p = (Type *)arena_alloc(e->arena, sizeof(Type));
    *sess_p = type_session(p_proto);    /* Session[P] */

    /* Left arm type = RecvPair[T, Session[Q]] -- vector-destructured by user */
    Type *recv_pair = (Type *)arena_alloc(e->arena, sizeof(Type));
    *recv_pair = type_session_recv_pair(val_tp, sess_q);

    /* Build TY_SESSION_OFFER: fst = RecvPair[T, Session[Q]]; snd = Session[P] */
    Type offer_type = type_session_offer(recv_pair, sess_p);

    /* Emit: tur_session_recv_timeout(chan, dur_ms) returns 0/1 tag.
     * On success the received value is in tur__rtv_ (thread-local in generated C).
     * val_exprs[0] = chan, val_exprs[1] = dur */
    static const char rto_code[] =
        "tur_session_recv_timeout(__TUR_VAL_0__, __TUR_VAL_1__)";
    Expr *rto_out = expr_new(e->arena, EX_INLINE_C, offer_type, call->span);
    InlineC *rto_ic = (InlineC *)arena_alloc(e->arena, sizeof(InlineC));
    memset(rto_ic, 0, sizeof(InlineC));
    rto_ic->code = strslice(rto_code, sizeof(rto_code) - 1);
    rto_ic->return_type = offer_type;
    rto_ic->captures = NULL; rto_ic->n_captures = 0;
    rto_ic->val_exprs = (Expr **)arena_alloc(e->arena, 2 * sizeof(Expr *));
    rto_ic->n_val_exprs = 2;
    if (chan_binding) {
        Expr *cv = expr_new(e->arena, EX_VAR, chan_binding->type, call->span);
        cv->as.var.binding = chan_binding;
        rto_ic->val_exprs[0] = cv;
    } else {
        /* EF-4: field-read (non-variable) channel operand -- use the elaborated
         * expr directly rather than a NULL val-expr (which crashed emit). */
        rto_ic->val_exprs[0] = chan;
    }
    rto_ic->val_exprs[1] = dur;
    rto_out->as.inline_c_.inline_c = rto_ic;
    return rto_out;
}
