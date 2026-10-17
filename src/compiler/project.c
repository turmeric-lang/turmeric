/* project.c -- SS6: Projection algorithm for global session types.
 *
 * Implements session_project(e, G, R, span) -- compute the binary local
 * session protocol Type* for role R from global interaction tree G.
 *
 * Projection rules (Honda/Yoshida/Carbone):
 *   GI_MSG(from=R, to, T, rest) -> Send[T, project(rest, R)]
 *   GI_MSG(from, to=R, T, rest) -> Recv[T, project(rest, R)]
 *   GI_MSG(from, to, T, rest)   -> project(rest, R)  (R uninvolved)
 *   GI_CHOICE(decider=R, ...)   -> Choose(B0, B1)
 *   GI_CHOICE(decider!=R, ...)  -> Branch(B0, B1) if branches differ,
 *                                   B0 if branches are equal (merge)
 *   GI_LOOP(label, body, rest)  -> Rec label (project(body, R)) if R involved
 *                                   project(rest, R) if R uninvolved
 *   GI_CONTINUE(label)          -> back-reference sentinel for label
 *   GI_END                      -> Close (or tail into end_cont)
 *   GI_TIMEOUT(from, to=R, T)   -> Recv[T, Timeout[ok, expired]]
 *   GI_TIMEOUT(from=R, to, T)   -> Send[T, Timeout[ok, expired]], ok == expired
 *   GI_TIMEOUT(from, to, T)     -> ok, when ok == expired  (R uninvolved)
 *     Only the receiver observes whether the deadline passed, so every other
 *     role -- the sender included -- must continue the same way in both
 *     outcomes (TUR-E0220 otherwise).  The sender keeps the Timeout wrapper
 *     so that, for two roles, its projection is the binary dual of the
 *     receiver's: dual(Recv[T, Timeout[Q, P]]) = Send[T, Timeout[dQ, dP]].
 */
#include "elab_internal.h"
#include <string.h>

/* ---- loop stack entry ---- */

#define PROJ_MAX_LOOP_DEPTH 16

typedef struct LoopEntry {
    const char *label;    /* interned loop label */
    Type       *sentinel; /* TY_SESSION_REC with fst=NULL -- back-reference for GI_CONTINUE */
} LoopEntry;

/* ---- arena helper ---- */

/* arena_type -- allocate and copy a Type value into the arena. */
static Type *arena_type(Elab *e, Type t) {
    Type *p = (Type *)arena_alloc(e->arena, sizeof(Type));
    *p = t;
    return p;
}

/* ---- structural equality for merge check ---- */

/* proto_equal -- check structural equality of two local session protocol types.
 * Used to decide whether an uninvolved role can merge two branch projections. */
static bool proto_equal(Type *a, Type *b) {
    if (!a && !b) return true;
    if (!a || !b) return false;
    if (a->kind != b->kind) return false;
    switch (a->kind) {
        case TY_CLOSE:
            return true;
        case TY_SEND:
        case TY_RECV:
        case TY_CHOOSE:
        case TY_BRANCH:
        case TY_TIMEOUT:
            return proto_equal(a->as.session_.fst, b->as.session_.fst)
                && proto_equal(a->as.session_.snd, b->as.session_.snd);
        case TY_SESSION_REC:
            if (a->as.session_.label != b->as.session_.label) {
                /* Labels are interned so pointer equality suffices; fall back to strcmp */
                if (!a->as.session_.label || !b->as.session_.label) return false;
                if (strcmp(a->as.session_.label, b->as.session_.label) != 0) return false;
            }
            /* Both sentinels (back-references) */
            if (!a->as.session_.fst && !b->as.session_.fst) return true;
            return proto_equal(a->as.session_.fst, b->as.session_.fst);
        default:
            /* For non-session types (message types), use kind equality */
            return a->kind == b->kind;
    }
}

/* ---- role name matching ---- */

/* role_eq -- compare two role name strings using both pointer and strcmp. */
static bool role_eq(const char *a, const char *b) {
    if (a == b) return true;
    if (!a || !b) return false;
    return strcmp(a, b) == 0;
}

/* ---- recursive projection helper ---- */

/* The whole protocol being projected (session_project), for the choice rule
 * below: whether a role other than two given ones takes part anywhere. */
static const GlobalInteraction *s_proj_root;

static bool gi_mentions_other_role(const GlobalInteraction *g, const char *a,
                                   const char *b, int depth) {
    if (!g || depth > 4096) return false;
    #define OTHER(r) ((r) && !role_eq((r), a) && !role_eq((r), b))
    switch (g->kind) {
    case GI_MSG:
        if (OTHER(g->msg.from) || OTHER(g->msg.to)) return true;
        return gi_mentions_other_role(g->msg.rest, a, b, depth + 1);
    case GI_CHOICE:
        if (OTHER(g->choice.decider)) return true;
        for (int i = 0; i < g->choice.n_branches; i++)
            if (gi_mentions_other_role(g->choice.branches[i].body, a, b, depth + 1))
                return true;
        return gi_mentions_other_role(g->choice.rest, a, b, depth + 1);
    case GI_LOOP:
        return gi_mentions_other_role(g->loop.body, a, b, depth + 1) ||
               gi_mentions_other_role(g->loop.rest, a, b, depth + 1);
    case GI_TIMEOUT:
        if (OTHER(g->timed.from) || OTHER(g->timed.to)) return true;
        return gi_mentions_other_role(g->timed.ok, a, b, depth + 1) ||
               gi_mentions_other_role(g->timed.expired, a, b, depth + 1);
    default:
        return false;
    }
    #undef OTHER
}

static Type *project_inner(Elab *e, GlobalInteraction *step, const char *role,
                            LoopEntry *loops, int n_loops,
                            GlobalInteraction *end_cont, Span span);

static Type *project_inner(Elab *e, GlobalInteraction *step, const char *role,
                            LoopEntry *loops, int n_loops,
                            GlobalInteraction *end_cont, Span span) {
    if (!step) {
        /* NULL step treated as GI_END */
        if (end_cont) {
            return project_inner(e, end_cont, role, loops, n_loops, NULL, span);
        }
        return arena_type(e, type_close());
    }

    switch (step->kind) {

    /* ---- GI_END ---- */
    case GI_END:
        if (end_cont) {
            return project_inner(e, end_cont, role, loops, n_loops, NULL, span);
        }
        return arena_type(e, type_close());

    /* ---- GI_MSG ---- */
    case GI_MSG: {
        const char *from = step->msg.from;
        const char *to   = step->msg.to;
        Type       *msg  = step->msg.msg;

        if (role_eq(from, role)) {
            /* Role is the sender */
            Type *cont = project_inner(e, step->msg.rest, role,
                                       loops, n_loops, end_cont, span);
            if (!cont) return NULL;
            return arena_type(e, type_send(msg, cont));
        } else if (role_eq(to, role)) {
            /* Role is the receiver */
            Type *cont = project_inner(e, step->msg.rest, role,
                                       loops, n_loops, end_cont, span);
            if (!cont) return NULL;
            return arena_type(e, type_recv(msg, cont));
        } else {
            /* Role is uninvolved -- skip this message */
            return project_inner(e, step->msg.rest, role,
                                 loops, n_loops, end_cont, span);
        }
    }

    /* ---- GI_CHOICE ---- */
    case GI_CHOICE: {
        int n_branches = step->choice.n_branches;
        if (n_branches != 2) {
            diag_emit_with_code(DIAG_ERROR, span, TUR_E0220_GLOBAL_NOT_PROJECTABLE,
                                "project: choice with %d branches is not yet supported "
                                "(only 2-branch choice is supported)",
                                n_branches);
            return NULL;
        }

        /* Determine the effective continuation after all branches */
        GlobalInteraction *branch_cont =
            (step->choice.rest && step->choice.rest->kind != GI_END)
            ? step->choice.rest
            : end_cont;

        /* Project each branch body */
        Type *b0 = project_inner(e, step->choice.branches[0].body, role,
                                 loops, n_loops, branch_cont, span);
        if (!b0) return NULL;

        Type *b1 = project_inner(e, step->choice.branches[1].body, role,
                                 loops, n_loops, branch_cont, span);
        if (!b1) return NULL;

        if (role_eq(step->choice.decider, role)) {
            /* Role is the deciding role -- produces Choose */
            return arena_type(e, type_choose(b0, b1));
        } else {
            /* Role is not the deciding role */
            if (proto_equal(b0, b1)) {
                /* Branches are equal -- role is uninvolved in the choice */
                return b0;
            } else if (!gi_mentions_other_role(s_proj_root, step->choice.decider,
                                               role, 0)) {
                /* Branches differ -- role is notified (Branch).  Only the
                 * decider's peer can be: the choice travels on their channel,
                 * which is the whole protocol when it has these two roles. */
                return arena_type(e, type_branch(b0, b1));
            } else {
                /* session-project-nonprojectable (found when needle files
                 * started being read): a third role that behaves differently
                 * in the branches has no way to learn which one was taken --
                 * nothing carries the choice to it -- so it is not
                 * projectable, as the session-types guide specifies.  It used
                 * to project to a Branch it could never be offered. */
                diag_emit_with_code(DIAG_ERROR, span, TUR_E0220_GLOBAL_NOT_PROJECTABLE,
                                    "project: role '%s' behaves differently in the "
                                    "branches of the choice made by '%s', but cannot "
                                    "learn which branch was taken; make its protocol "
                                    "the same in every branch",
                                    role, step->choice.decider);
                return NULL;
            }
        }
    }

    /* ---- GI_LOOP ---- */
    case GI_LOOP: {
        const char *label = step->loop.label;

        if (n_loops >= PROJ_MAX_LOOP_DEPTH) {
            diag_emit_with_code(DIAG_ERROR, span, TUR_E0220_GLOBAL_NOT_PROJECTABLE,
                                "project: loop nesting depth exceeds maximum (%d)",
                                PROJ_MAX_LOOP_DEPTH);
            return NULL;
        }

        /* Allocate a back-reference sentinel: TY_SESSION_REC with fst=NULL */
        Type *sentinel = arena_type(e, type_session_rec(label, NULL));

        /* Push new loop entry onto a local copy of the stack */
        LoopEntry new_loops[PROJ_MAX_LOOP_DEPTH];
        for (int i = 0; i < n_loops; i++) new_loops[i] = loops[i];
        new_loops[n_loops].label    = label;
        new_loops[n_loops].sentinel = sentinel;

        /* Determine what follows the loop */
        GlobalInteraction *effective_rest =
            (step->loop.rest && step->loop.rest->kind != GI_END)
            ? step->loop.rest
            : end_cont;

        /* Project the loop body */
        Type *body_proj = project_inner(e, step->loop.body, role,
                                        new_loops, n_loops + 1,
                                        effective_rest, span);
        if (!body_proj) return NULL;

        /* If the projected body is Close, role is uninvolved in the loop --
         * skip the loop entirely and project the rest. */
        if (body_proj->kind == TY_CLOSE) {
            return project_inner(e, step->loop.rest, role,
                                 loops, n_loops, end_cont, span);
        }

        /* Build the Rec node wrapping the projected body */
        return arena_type(e, type_session_rec(label, body_proj));
    }

    /* ---- GI_TIMEOUT ---- */
    case GI_TIMEOUT: {
        /* Both continuations already end in whatever follows the timeout form
         * (the parser appended it), so end_cont passes through unchanged. */
        Type *b_ok = project_inner(e, step->timed.ok, role,
                                   loops, n_loops, end_cont, span);
        if (!b_ok) return NULL;
        Type *b_exp = project_inner(e, step->timed.expired, role,
                                    loops, n_loops, end_cont, span);
        if (!b_exp) return NULL;
        Type *msg = step->timed.msg;

        if (role_eq(step->timed.to, role)) {
            Type *tmo = arena_type(e, type_timeout(b_ok, b_exp));
            return arena_type(e, type_recv(msg, tmo));
        }
        if (!proto_equal(b_ok, b_exp)) {
            if (role_eq(step->timed.from, role)) {
                diag_emit_with_code(DIAG_ERROR, span, TUR_E0220_GLOBAL_NOT_PROJECTABLE,
                                    "project: role '%s' sends into a timed receive but cannot "
                                    "observe whether '%s' timed out -- its protocol after the "
                                    "message must be the same in the ok and expired branches",
                                    role, step->timed.to);
            } else {
                diag_emit_with_code(DIAG_ERROR, span, TUR_E0220_GLOBAL_NOT_PROJECTABLE,
                                    "project: role '%s' is not party to the timed receive "
                                    "(-> %s %s ...) and cannot observe its outcome -- its protocol "
                                    "must be the same in the ok and expired branches (have '%s' "
                                    "tell it with a message in each branch instead)",
                                    role, step->timed.from, step->timed.to, step->timed.to);
            }
            return NULL;
        }
        if (role_eq(step->timed.from, role)) {
            Type *tmo = arena_type(e, type_timeout(b_ok, b_exp));
            return arena_type(e, type_send(msg, tmo));
        }
        return b_ok;
    }

    /* ---- GI_CONTINUE ---- */
    case GI_CONTINUE: {
        const char *label = step->cont.label;
        /* Search the loop stack from the top */
        for (int i = n_loops - 1; i >= 0; i--) {
            if (role_eq(loops[i].label, label)) {
                return loops[i].sentinel;
            }
        }
        diag_emit_with_code(DIAG_ERROR, span, TUR_E0220_GLOBAL_NOT_PROJECTABLE,
                            "project: (continue %s) has no enclosing loop with that label",
                            label ? label : "?");
        return NULL;
    }

    default:
        diag_emit_with_code(DIAG_ERROR, span, TUR_E0220_GLOBAL_NOT_PROJECTABLE,
                            "project: unknown GlobalInteraction kind %d", (int)step->kind);
        return NULL;
    }
}

/* ---- public API ---- */

/* session_project -- compute the binary local session type for role R from
 * global protocol step G.
 *
 * Parameters:
 *   e    -- elaborator state (arena, symbol table)
 *   step -- root of the GlobalInteraction tree
 *   role -- interned role name string
 *   span -- source span for diagnostics
 *
 * Returns:
 *   A heap-allocated (arena) Session protocol Type* on success.
 *   Returns NULL and emits TUR_E0220 on failure (non-projectable).
 *
 * Since: Phase SS6
 */
Type *session_project(Elab *e, GlobalInteraction *step, const char *role, Span span) {
    LoopEntry empty_loops[PROJ_MAX_LOOP_DEPTH];
    (void)empty_loops; /* quiet unused-variable warning when used as pointer */
    const GlobalInteraction *saved_root = s_proj_root;
    s_proj_root = step;
    Type *t = project_inner(e, step, role, empty_loops, 0, NULL, span);
    s_proj_root = saved_root;
    return t;
}
