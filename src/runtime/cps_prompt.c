#include "cps_prompt.h"

#include <stdio.h>
#include <stdlib.h>

/* =========================================================================
 * CPS5 (cps-transform-plan): multi-prompt delimited-control machine.
 * See cps_prompt.h for the model.
 * ========================================================================= */

typedef enum {
    DKK_DONE,
    DKK_FRAME,
    DKK_PROMPT,
    DKK_SHIFT,
    DKK_SHIFT0,
    DKK_HANDLER,
    DKK_RESUME_FRAME,
} DKKind;

struct DK {
    DKKind    kind;
    DKFrame   fn;        /* DKK_FRAME */
    intptr_t  env;       /* DKK_FRAME */
    int       tag;       /* DKK_PROMPT / DKK_SHIFT* / DKK_HANDLER */
    DKBody    body;      /* DKK_SHIFT* */
    intptr_t  body_env;  /* DKK_SHIFT* */
    DKHandler handler;   /* DKK_HANDLER */
    intptr_t  handler_env; /* DKK_HANDLER */
    bool      shallow;   /* DKK_HANDLER: true = do NOT reinstall on resume (shift0-like) */
    DKResumeFrame rfn;   /* DKK_RESUME_FRAME */
    DKEnvClone env_clone; /* E3a: owning-env clone glue (NULL = shallow env copy) */
    DKEnvDrop  env_drop;  /* E3a: owning-env drop glue  (NULL = no env teardown) */
    DK       *next;
};

static DK *dk_new(DKKind kind, DK *next) {
    DK *k = calloc(1, sizeof(DK));
    k->kind = kind;
    k->next = next;
    return k;
}

DK *dk_done(void) { return dk_new(DKK_DONE, NULL); }

DK *dk_frame(DKFrame fn, intptr_t env, DK *next) {
    DK *k = dk_new(DKK_FRAME, next);
    k->fn = fn; k->env = env;
    return k;
}

DK *dk_frame_owning(DKFrame fn, intptr_t env,
                    DKEnvClone env_clone, DKEnvDrop env_drop, DK *next) {
    DK *k = dk_new(DKK_FRAME, next);
    k->fn = fn; k->env = env;
    k->env_clone = env_clone; k->env_drop = env_drop;
    return k;
}

DK *dk_frame_resume(DKResumeFrame fn, intptr_t env, DK *next) {
    DK *k = dk_new(DKK_RESUME_FRAME, next);
    k->rfn = fn; k->env = env;
    return k;
}

DK *dk_prompt(int tag, DK *next) {
    DK *k = dk_new(DKK_PROMPT, next);
    k->tag = tag;
    return k;
}

static DK *dk_shift_impl(DKKind kind, int tag, DKBody body, intptr_t env, DK *next) {
    DK *k = dk_new(kind, next);
    k->tag = tag; k->body = body; k->body_env = env;
    return k;
}

DK *dk_shift(int tag, DKBody body, intptr_t env, DK *next) {
    return dk_shift_impl(DKK_SHIFT, tag, body, env, next);
}
DK *dk_shift0(int tag, DKBody body, intptr_t env, DK *next) {
    return dk_shift_impl(DKK_SHIFT0, tag, body, env, next);
}

static DK *dk_handler_impl(int tag, DKHandler fn, intptr_t env, bool shallow, DK *next) {
    DK *k = dk_new(DKK_HANDLER, next);
    k->tag = tag; k->handler = fn; k->handler_env = env; k->shallow = shallow;
    return k;
}

DK *dk_handler(int tag, DKHandler fn, intptr_t env, DK *next) {
    return dk_handler_impl(tag, fn, env, false, next);
}
DK *dk_handler_shallow(int tag, DKHandler fn, intptr_t env, DK *next) {
    return dk_handler_impl(tag, fn, env, true, next);
}

/* Copy one node (next set to NULL).  E3a: a frame carrying env_clone gets an
 * OWNED copy of its env (rc incref / aggregate deep-copy) instead of a shared
 * shallow pointer alias, so each multi-shot resume owns its own +1; a NULL
 * env_clone keeps the shallow copy, byte-identical to the pre-E3a path. The
 * clone/drop glue pointers ride along so the copy frees its env symmetrically. */
static DK *dk_copy_node(const DK *n) {
    DK *c = dk_new(n->kind, NULL);
    c->fn = n->fn; c->tag = n->tag;
    c->env = n->env_clone ? n->env_clone(n->env) : n->env;
    c->body = n->body; c->body_env = n->body_env;
    c->handler = n->handler; c->handler_env = n->handler_env;
    c->shallow = n->shallow;
    c->rfn = n->rfn;
    c->env_clone = n->env_clone; c->env_drop = n->env_drop;
    return c;
}

DK *dk_copy(const DK *k) {
    if (!k) return NULL;
    DK *head = NULL, *tail = NULL;
    for (const DK *p = k; p; p = p->next) {
        DK *c = dk_copy_node(p);
        if (!head) head = tail = c;
        else { tail->next = c; tail = c; }
    }
    return head;
}

/* Copy frames in [from, stop) (stop exclusive; stop==NULL copies to the end).
 * Returns a fresh chain whose tail->next is NULL, or NULL if the range is
 * empty. */
static DK *dk_copy_range(const DK *from, const DK *stop) {
    DK *head = NULL, *tail = NULL;
    for (const DK *p = from; p && p != stop; p = p->next) {
        DK *c = dk_copy_node(p);
        if (!head) head = tail = c;
        else { tail->next = c; tail = c; }
    }
    return head;
}

/* Copy just the enclosing handler markers reachable from `from` (skipping frames
 * and prompts), chained and terminated by a fresh dk_done().  Used to build a
 * SHALLOW handler's captured sub-continuation (F2.1): the handler markers are
 * transparent to a returning value (dk_run_impl's DKK_HANDLER case), so a normal
 * resume still surfaces the body-rest value into the case, but a re-perform
 * during that resume can find an enclosing handler for its tag -- shallow
 * outer-propagation.  Frames are deliberately NOT copied: copying them would
 * transform the returning value instead of surfacing it.  Prompts are skipped
 * too, matching dk_perform's own search, which walks through prompts to the next
 * handler or the root.  Stops at DKK_DONE / end of chain. */
static DK *dk_copy_enclosing_handlers(const DK *from) {
    DK *head = NULL, *tail = NULL;
    for (const DK *p = from; p && p->kind != DKK_DONE; p = p->next) {
        if (p->kind != DKK_HANDLER) continue;
        DK *c = dk_copy_node(p);
        if (!head) head = tail = c;
        else { tail->next = c; tail = c; }
    }
    DK *done = dk_done();
    if (!head) return done;
    tail->next = done;
    return head;
}

/* Append b to the end of a; returns the head. */
static DK *dk_append(DK *a, DK *b) {
    if (!a) return b;
    DK *p = a;
    while (p->next) p = p->next;
    p->next = b;
    return a;
}

void dk_free(DK *k) {
    while (k) {
        DK *n = k->next;
        if (k->env_drop) k->env_drop(k->env);   /* E3a: drop the owning env first */
        free(k);
        k = n;
    }
}

void dk_free_node(DK *k) {
    if (k && k->env_drop) k->env_drop(k->env);   /* E3a: symmetric single-node drop */
    free(k);
}

bool dk_has_prompt(const DK *k) {
    for (const DK *p = k; p; p = p->next)
        if (p->kind == DKK_PROMPT) return true;
    return false;
}

bool dk_has_handler(const DK *k, int tag) {
    for (const DK *p = k; p; p = p->next)
        if (p->kind == DKK_HANDLER && p->tag == tag) return true;
    return false;
}

/* Core machine. `root` enables the implicit root prompt: a shift whose target
 * prompt is not found is delimited at program end (DONE). */
static intptr_t dk_run_impl(DK *k, intptr_t v, bool root) {
    while (k) {
        switch (k->kind) {
            case DKK_DONE:
                return v;
            case DKK_PROMPT:
            case DKK_HANDLER:
                k = k->next;            /* transparent to a returning value */
                break;
            case DKK_FRAME:
                v = k->fn(k->env, v);
                k = k->next;
                break;
            case DKK_RESUME_FRAME:
                /* A suspending continuation frame: hand it the run-time
                 * downstream chain (k->next) and let it own delivery. */
                return k->rfn(k->env, v, k->next);
            case DKK_SHIFT:
            case DKK_SHIFT0: {
                /* find the nearest enclosing prompt with this tag */
                DK *P = k->next;
                while (P && !(P->kind == DKK_PROMPT && P->tag == k->tag)
                         && P->kind != DKK_DONE)
                    P = P->next;
                bool to_root = (!P || P->kind == DKK_DONE);
                bool reinstall = (k->kind == DKK_SHIFT);
                /* reify sub-continuation: frames between the shift and the
                 * prompt (P is the matching prompt, or the trailing DONE when
                 * capturing to root), then (for shift) a re-installed prompt,
                 * then DONE. */
                DK *sub = dk_copy_range(k->next, P);
                DK *tail = reinstall
                    ? dk_prompt(to_root ? DK_ROOT_TAG : k->tag, dk_done())
                    : dk_done();
                sub = dk_append(sub, tail);
                intptr_t bodyval = k->body(k->body_env, sub);
                dk_free(sub);
                /* deliver bodyval to the prompt's outer continuation */
                if (to_root) return bodyval;       /* root: value escapes program */
                k = P->next;
                v = bodyval;
                break;
            }
        }
    }
    return v;
}

intptr_t dk_run(DK *k, intptr_t v)      { return dk_run_impl(k, v, false); }
intptr_t dk_run_root(DK *k, intptr_t v) { return dk_run_impl(k, v, true); }

intptr_t dk_invoke(DK *sub, intptr_t w) {
    DK *c = dk_copy(sub);          /* fresh copy: multi-shot */
    intptr_t r = dk_run_impl(c, w, false);
    dk_free(c);
    return r;
}

intptr_t dk_perform(int tag, const char *name, intptr_t arg, DK *k) {
    /* Find the nearest enclosing handler for this effect tag. */
    DK *H = k;
    while (H && !(H->kind == DKK_HANDLER && H->tag == tag) && H->kind != DKK_DONE)
        H = H->next;
    if (!H || H->kind == DKK_DONE) {
        fflush(stdout);
        fprintf(stderr, "tur: unhandled effect: %s (tag %d)\n", name ? name : "?", tag);
        abort();
    }
    /* Reify the sub-continuation from the perform point up to the handler.  For a
     * DEEP handler (H->shallow == false), re-install the handler on the captured
     * copy so a resume re-delimits under it (each subsequent perform of the same
     * effect in the resumed computation is handled again) -- the effect-side
     * analogue of `shift`.  For a SHALLOW handler (H->shallow == true), do NOT
     * re-install: a resume runs the rest of the computation with this handler
     * already removed -- the analogue of `shift0`.  Both share the same
     * reify/copy machinery; only the reinstall tail differs.
     *
     * Shallow outer-propagation (F2.1): the shallow tail is not a bare dk_done()
     * but a copy of the ENCLOSING handler markers (dk_copy_enclosing_handlers,
     * skipping frames/prompts).  Those markers are transparent to a returning
     * value, so a normal resume still surfaces the body-rest value into the case,
     * while a subsequent same-effect perform inside the resume finds the copied
     * enclosing handler for its tag (or aborts as unhandled if none carries it --
     * correct).  The deep tail is unchanged: it re-installs this handler. */
    DK *sub = dk_copy_range(k, H);
    DK *tail = H->shallow
        ? dk_copy_enclosing_handlers(H->next)
        : dk_handler(tag, H->handler, H->handler_env, dk_done());
    sub = dk_append(sub, tail);
    intptr_t r = H->handler(H->handler_env, arg, sub);
    dk_free(sub);
    /* Deliver the handler-case result to the handler's outer continuation. */
    return dk_run_impl(H->next, r, false);
}
