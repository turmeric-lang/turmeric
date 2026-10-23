/* CPS5 (cps-transform-plan): unit tests for the multi-prompt delimited-control
 * machine (src/runtime/cps_prompt.c).
 *
 * Programs are written as CPS continuation chains (head = innermost). Each test
 * checks a delimited-control law on the substrate:
 *   - CPS5.1: reset/shift capture + compose; shift vs shift0 re-install.
 *   - CPS5.2: a captured sub-continuation is multi-shot (invoke N times).
 *   - CPS5.3: an implicit root prompt lets an undelimited capture reach entry.
 *
 * Prints PASS/FAIL; exits nonzero on any failure.
 */

#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>

#include "cps_prompt.h"

#define TAG 1

static int g_pass = 0, g_fail = 0;
static void check(int cond, const char *name, const char *msg) {
    if (cond) { g_pass++; printf("PASS %s\n", name); }
    else      { g_fail++; printf("FAIL %s -- %s\n", name, msg); }
}

/* ---- frames ---- */
static intptr_t add1(intptr_t env, intptr_t v) { (void)env; return v + 1; }
static intptr_t mul2(intptr_t env, intptr_t v) { (void)env; return v * 2; }

/* ---- E3a: owning-env clone/drop hooks (cps-backend-owning-env-teardown) ----
 * Model an `rc` handle as a heap cell with a strong count: env_clone increfs
 * and returns the SAME handle (rc is shared); env_drop decrefs and frees at 0.
 * Because the cell is heap-allocated and freed exactly at strong==0, the ASan/
 * LSan build enforces the accounting for free: a leaked +1 shows as a leak, a
 * double-free / use-after-free as an ASan abort. The globals let the test also
 * assert the clone/drop call counts and that the cell is freed exactly once. */
typedef struct { int strong; intptr_t payload; } RcCell;
static int g_rc_clone, g_rc_drop, g_rc_freed;
static void rc_reset(void) { g_rc_clone = g_rc_drop = g_rc_freed = 0; }
static RcCell *rc_new(intptr_t payload) {
    RcCell *c = (RcCell *)calloc(1, sizeof(RcCell));
    c->strong = 1;                 /* base populate = +1 */
    c->payload = payload;
    return c;
}
static intptr_t rc_clone(intptr_t env) {
    RcCell *c = (RcCell *)(intptr_t)env;
    g_rc_clone++; c->strong++;
    return env;                     /* rc: shared handle, incref, same pointer */
}
static void rc_drop(intptr_t env) {
    RcCell *c = (RcCell *)(intptr_t)env;
    g_rc_drop++;
    if (--c->strong == 0) { g_rc_freed++; free(c); }
}
/* An owning frame: read the (still-alive) payload and add it to the value. Every
 * copy that runs this must see strong>0 -- a premature drop would be a UAF. */
static intptr_t rc_add_payload(intptr_t env, intptr_t v) {
    RcCell *c = (RcCell *)(intptr_t)env;
    return v + c->payload;
}

/* ---- shift bodies ---- */

/* reset { 1 + shift(k => 2 + k(k(3))) }  -- k = (1 + []).  k(3)=4, k(4)=5 => 7 */
static intptr_t body_2_plus_kk3(intptr_t env, DK *sub) {
    (void)env;
    intptr_t a = dk_invoke(sub, 3);     /* 4 */
    intptr_t b = dk_invoke(sub, a);     /* 5 */
    return 2 + b;                       /* 7 */
}

/* reset { shift(k => k(10) + k(20)) * 2 }  -- multi-shot: k=( []*2 ) => 20+40=60 */
static intptr_t body_k10_plus_k20(intptr_t env, DK *sub) {
    (void)env;
    return dk_invoke(sub, 10) + dk_invoke(sub, 20); /* 20 + 40 = 60 */
}

/* probe whether the captured sub-cont carries a re-installed prompt */
static bool g_sub_has_prompt;
static intptr_t body_probe(intptr_t env, DK *sub) {
    (void)env;
    g_sub_has_prompt = dk_has_prompt(sub);
    return 0;
}

/* root capture: shift to ROOT with no enclosing reset; k = (1 + []) up to entry.
 * k(100) = 101; + 1000 = 1101. */
static intptr_t body_root(intptr_t env, DK *sub) {
    (void)env;
    return dk_invoke(sub, 100) + 1000;
}

/* ---- F3: async / await on heap continuations (await-as-shift) ---- */

#define ASYNC_TAG 7
#define SUSPEND ((intptr_t)-999)   /* sentinel the async driver reads as "parked" */

/* A mock reactor: the single continuation an `await` parked, to be resumed when
 * the awaited future completes. */
static DK *g_reactor_pending;

/* `await` lowered as a shift against the async prompt.  It receives the captured
 * sub-continuation (the rest of the async body up to the async prompt), hands a
 * RETAINED copy to the reactor (dk_run_impl frees the original `sub` once this
 * body returns, so the copy is what survives the suspend), and returns SUSPEND
 * -- which unwinds out of dk_run to the scheduler instead of running the
 * continuation inline.  This is the swapcontext-free analogue of the fiber
 * runtime's "register on_complete + yield". */
static intptr_t await_body(intptr_t env, DK *subk) {
    (void)env;
    g_reactor_pending = dk_copy(subk);
    return SUSPEND;
}

/* ---- F3.2: deferred park/resume via an on_complete seam + outer-future
 * chaining (mirrors the emitted __tur_await_body / __tur_async_resume /
 * tur_async_fiber).  A mock future carries a status/value plus an on_complete
 * callback; a park record carries the retained continuation copy plus the OUTER
 * future the async boundary handed out.  Two sequential pending awaits exercise
 * the re-park branch: the first resume immediately re-parks on the second future
 * and threads the outer future onto the new park; only the second fulfill drives
 * the body to `x + y` and fulfills the outer.  The compiled path can only reach a
 * SINGLE-await function today (the CPS backend admits one control op per body),
 * so this probe is the guard for the re-park chaining until two-await functions
 * become CPS-admissible. */
typedef struct MockFuture MockFuture;
typedef struct { DK *subk; MockFuture *outer; } AsyncPark;
struct MockFuture {
    int done;
    intptr_t value;
    void (*on_complete)(MockFuture *, intptr_t);
    void *oc_env;
};
static int        g_async_susp;   /* mirrors tur_async_suspended */
static AsyncPark *g_async_park;   /* mirrors tur_async_pending_park */
static MockFuture *g_fut_a, *g_fut_b;
static void async_resume(MockFuture *inner, intptr_t value);

static void mock_fulfill(MockFuture *f, intptr_t v) {
    if (!f || f->done) return;
    f->done = 1;
    f->value = v;
    if (f->on_complete) f->on_complete(f, v);
}

/* await lowered as a shift: resolved -> resume inline; pending -> park a copy on
 * on_complete and suspend (set the flag). */
static intptr_t await_park_body(intptr_t env, DK *subk) {
    MockFuture *f = (MockFuture *)(intptr_t)env;
    if (f->done) return dk_invoke(subk, f->value);
    AsyncPark *rec = (AsyncPark *)calloc(1, sizeof(AsyncPark));
    rec->subk = dk_copy(subk);
    rec->outer = NULL;
    f->on_complete = async_resume;
    f->oc_env = rec;
    g_async_susp = 1;
    g_async_park = rec;
    return 0;
}

/* second-await continuation: add the first awaited value x (frame env) to the
 * second awaited value y (delivered), completing the body. */
static intptr_t async_add(intptr_t env, intptr_t v) { return env + v; }

/* first-await continuation: bind x, then await the SECOND future (b). */
static intptr_t async_after_a(intptr_t env, intptr_t x) {
    (void)env;
    /* build the second-await chain, run it, then free the original nodes
     * (dk_run copies the captured range; the park retains its own copy). */
    DK *c = dk_shift(ASYNC_TAG, await_park_body, (intptr_t)g_fut_b,
              dk_frame(async_add, x,
                dk_prompt(ASYNC_TAG, dk_done())));
    intptr_t r = dk_run(c, 0);
    dk_free(c);
    return r;
}

/* on_complete resumer: replay the parked continuation; if it re-parks, thread
 * the outer future through; otherwise fulfill the outer with the result. */
static void async_resume(MockFuture *inner, intptr_t value) {
    AsyncPark *rec = (AsyncPark *)inner->oc_env;
    g_async_susp = 0;
    g_async_park = NULL;
    intptr_t r = dk_invoke(rec->subk, value);
    if (g_async_susp && g_async_park) g_async_park->outer = rec->outer;
    else mock_fulfill(rec->outer, r);
    g_async_susp = 0;
    g_async_park = NULL;
    dk_free(rec->subk);
    free(rec);
}

/* the async boundary (mirrors tur_async_fiber): run the body; park -> leave the
 * outer pending and thread it onto the park; else fulfill inline. */
static MockFuture *async_run(DK *body) {
    MockFuture *outer = (MockFuture *)calloc(1, sizeof(MockFuture));
    g_async_susp = 0;
    g_async_park = NULL;
    intptr_t result = dk_run(body, 0);
    if (g_async_susp && g_async_park) g_async_park->outer = outer;
    else mock_fulfill(outer, result);
    g_async_susp = 0;
    g_async_park = NULL;
    return outer;
}

/* ---- effect handler cases ---- */

/* Structural probe: does the captured sub-continuation carry a re-installed
 * handler marker for TAG?  Deep -> yes; shallow -> no. */
static bool g_case_sub_has_handler;
static intptr_t case_probe(intptr_t env, intptr_t arg, DK *subk) {
    (void)env; (void)arg;
    g_case_sub_has_handler = dk_has_handler(subk, TAG);
    return dk_invoke(subk, 1);       /* resume with 1 (rest of body up to handle) */
}

/* F2.1 outer-propagation: an OUTER deep handler for TAG that resumes the
 * captured continuation with (arg + 100). */
static intptr_t case_outer_add100(intptr_t env, intptr_t arg, DK *subk) {
    (void)env;
    return dk_invoke(subk, arg + 100);
}

/* F2.1 outer-propagation: the INNER shallow handler for TAG.  It checks that the
 * captured sub now carries an enclosing handler marker (the copied outer
 * handler), then emulates the resumed body re-performing TAG: dk_perform against
 * subk must find the OUTER handler rather than aborting.  Before F2.1 the shallow
 * tail was a bare dk_done(), so this dk_perform would abort as "unhandled". */
static bool g_inner_sub_has_handler;
static intptr_t case_inner_reperform(intptr_t env, intptr_t arg, DK *subk) {
    (void)env; (void)arg;
    g_inner_sub_has_handler = dk_has_handler(subk, TAG);
    return dk_perform(TAG, "Test", 5, subk);   /* re-perform; caught by outer -> 5+100=105 */
}

int main(void) {
    /* ---- CPS5.1: reset/shift capture + compose ---- */
    {
        DK *chain = dk_shift(TAG, body_2_plus_kk3, 0,
                       dk_frame(add1, 0,
                         dk_prompt(TAG, dk_done())));
        intptr_t r = dk_run(chain, 0);
        check(r == 7, "cps5-shift-compose", "reset/shift(k(k(3))) != 7");
        dk_free(chain);
    }

    /* ---- CPS5.2: multi-shot sub-continuation ---- */
    {
        DK *chain = dk_shift(TAG, body_k10_plus_k20, 0,
                       dk_frame(mul2, 0,
                         dk_prompt(TAG, dk_done())));
        intptr_t r = dk_run(chain, 0);
        check(r == 60, "cps5-multishot", "k(10)+k(20) over []*2 != 60");
        dk_free(chain);
    }

    /* ---- CPS5.1: shift RE-INSTALLS the prompt on the captured sub ---- */
    {
        g_sub_has_prompt = false;
        DK *chain = dk_shift(TAG, body_probe, 0,
                       dk_frame(add1, 0, dk_prompt(TAG, dk_done())));
        dk_run(chain, 0);
        check(g_sub_has_prompt, "cps5-shift-reinstall",
              "shift did not re-install the prompt on the sub-continuation");
        dk_free(chain);
    }

    /* ---- CPS5.1: shift0 does NOT re-install the prompt ---- */
    {
        g_sub_has_prompt = true;
        DK *chain = dk_shift0(TAG, body_probe, 0,
                        dk_frame(add1, 0, dk_prompt(TAG, dk_done())));
        dk_run(chain, 0);
        check(!g_sub_has_prompt, "cps5-shift0-no-reinstall",
              "shift0 wrongly re-installed the prompt");
        dk_free(chain);
    }

    /* ---- CPS5.3: implicit root prompt (undelimited capture to entry) ---- */
    {
        /* no explicit reset; shift targets ROOT */
        DK *chain = dk_shift(DK_ROOT_TAG, body_root, 0,
                       dk_frame(add1, 0, dk_done()));
        intptr_t r = dk_run_root(chain, 0);
        check(r == 1101, "cps5-root-prompt",
              "undelimited capture to the implicit root prompt != 1101");
        dk_free(chain);
    }

    /* ---- F2: DEEP handler re-installs on resume (perform side) ----
     * A perform against a deep handler captures the sub-continuation up to the
     * handler and re-installs the handler on it, so the case sees a re-installed
     * marker (dk_has_handler == true) and a subsequent same-tag perform in the
     * resumed computation would be re-handled.  The delivered value: the case
     * resumes with 1 -> add1(1) = 2 -> handler-case returns 2 -> mul2(2) = 4. */
    {
        g_case_sub_has_handler = false;
        DK *chain = dk_frame(add1, 0,
                       dk_handler(TAG, case_probe, 0,
                         dk_frame(mul2, 0, dk_done())));
        intptr_t r = dk_perform(TAG, "Test", 0, chain);
        check(g_case_sub_has_handler && r == 4, "cps-effect-deep-reinstall",
              "deep handler did not re-install on the captured sub-continuation");
        dk_free(chain);
    }

    /* ---- F2: SHALLOW handler does NOT re-install on resume (perform side) ----
     * The effect-side analogue of shift0: the captured sub-continuation carries
     * no re-installed handler marker (dk_has_handler == false), so a resume runs
     * the rest of the computation with this handler already removed.  The
     * delivered value is identical to the deep case (4) -- only the reinstall bit
     * differs, mirroring the shift vs shift0 structural probes above. */
    {
        g_case_sub_has_handler = true;
        DK *chain = dk_frame(add1, 0,
                       dk_handler_shallow(TAG, case_probe, 0,
                         dk_frame(mul2, 0, dk_done())));
        intptr_t r = dk_perform(TAG, "Test", 0, chain);
        check(!g_case_sub_has_handler && r == 4, "cps-effect-shallow-no-reinstall",
              "shallow handler wrongly re-installed on the captured sub-continuation");
        dk_free(chain);
    }

    /* ---- F2.1: SHALLOW handler resume re-performs, caught by an ENCLOSING
     * deep handler (shallow outer-propagation) ----
     * Chain at the perform point: add1 frame, then the inner SHALLOW handler for
     * TAG, then the outer DEEP handler for TAG, then done.  The first perform
     * hits the inner shallow handler; because it is shallow the handler removes
     * itself, but F2.1 copies the enclosing (outer) handler marker onto the
     * captured sub.  The inner case then re-performs TAG, which -- with F2.1 --
     * reaches the outer deep handler (before F2.1 it would abort as unhandled).
     * The outer resumes with 5+100=105 -> add1(105)=106. */
    {
        g_inner_sub_has_handler = false;
        DK *chain = dk_frame(add1, 0,
                       dk_handler_shallow(TAG, case_inner_reperform, 0,
                         dk_handler(TAG, case_outer_add100, 0,
                           dk_done())));
        intptr_t r = dk_perform(TAG, "Test", 0, chain);
        check(g_inner_sub_has_handler && r == 106,
              "cps-effect-shallow-outer-propagation",
              "shallow resume's re-perform did not reach the enclosing handler");
        dk_free(chain);
    }

    /* ---- F3: await-as-shift suspend/resume on heap continuations ----
     * The async body `1 + await(fut)` runs under an implicit async prompt
     * installed at the body's entry (its outer continuation IS the scheduler --
     * modeled here as the prompt's `next` being DONE).  `await` is a shift
     * against that prompt: it captures the rest of the body (`1 + []`) as a heap
     * continuation, parks a retained copy in the reactor, and SUSPENDS (dk_run
     * returns SUSPEND to the scheduler -- no C stack is retained across the
     * await).  When the awaited future completes with 41, the reactor resumes the
     * parked continuation with dk_invoke, replaying `1 + 41 = 42`. */
    {
        g_reactor_pending = NULL;
        DK *chain = dk_shift(ASYNC_TAG, await_body, 0,
                       dk_frame(add1, 0,
                         dk_prompt(ASYNC_TAG, dk_done())));
        intptr_t suspended = dk_run(chain, 0);
        /* the async computation parked: control returned to the scheduler and a
         * continuation is waiting in the reactor. */
        int parked = (suspended == SUSPEND && g_reactor_pending != NULL);
        /* the awaited future completes with 41; the reactor resumes the kont. */
        intptr_t result = parked ? dk_invoke(g_reactor_pending, 41) : -1;
        check(parked && result == 42, "cps-async-await-shift-suspend-resume",
              "await-as-shift did not park then resume to 1 + 41 = 42");
        dk_free(g_reactor_pending);
        dk_free(chain);
    }

    /* ---- F3.2: two sequential pending awaits -- park, re-park, then complete.
     * body = `(await a) + (await b)`.  Await a parks (a pending); the outer future
     * stays pending.  Fulfilling a resumes into await b, which RE-PARKS (b
     * pending) with the outer future threaded through.  Fulfilling b runs
     * `10 + 32 = 42` and fulfills the outer.  Guards the __tur_async_resume
     * re-park branch. */
    {
        MockFuture fa = {0}, fb = {0};
        g_fut_a = &fa; g_fut_b = &fb;
        DK *body = dk_shift(ASYNC_TAG, await_park_body, (intptr_t)g_fut_a,
                     dk_frame(async_after_a, 0,
                       dk_prompt(ASYNC_TAG, dk_done())));
        MockFuture *outer = async_run(body);
        int parked_a = (!outer->done && fa.on_complete != NULL);
        mock_fulfill(&fa, 10);                 /* resume -> re-parks on b */
        int reparked_b = (!outer->done && fb.on_complete != NULL);
        mock_fulfill(&fb, 32);                 /* resume -> 10 + 32 = 42 */
        check(parked_a && reparked_b && outer->done && outer->value == 42,
              "cps-async-await-repark-chain",
              "two-await re-park did not thread the outer future to 10 + 32 = 42");
        free(outer);
        dk_free(body);
    }

    /* ---- E3a: owning-env drop fires once on a straight-line run (no copy) ----
     * A single owning frame, no shift/capture: dk_run transforms the value and
     * dk_free(chain) drops the base env exactly once.  env_clone must NOT fire
     * (nothing copies the chain); env_drop fires once, freeing the rc cell. */
    {
        rc_reset();
        RcCell *cell = rc_new(5);
        DK *chain = dk_frame_owning(rc_add_payload, (intptr_t)cell,
                       rc_clone, rc_drop, dk_done());
        intptr_t r = dk_run(chain, 100);          /* 100 + 5 */
        dk_free(chain);                            /* drop base -> free */
        check(r == 105 && g_rc_clone == 0 && g_rc_drop == 1 && g_rc_freed == 1,
              "cps-e3a-owning-straightline-drop-once",
              "owning env not dropped exactly once on a no-copy run");
    }

    /* ---- E3a: owning-env clone/drop across a MULTI-SHOT resume (net zero) ----
     * reset { shift(k => k(10) + k(20)) } with an owning rc frame `[] + 5` in the
     * captured continuation.  k is resumed twice; each resume dk_copy's the sub,
     * so the owning frame is cloned per copy (incref) and dropped per free
     * (decref).  Value: (10+5) + (20+5) = 40.  Accounting: base populate +1;
     * capture copy +1; each of the 2 resumes +1 clone / -1 drop; sub free -1;
     * base free -1.  net zero -> the cell is freed exactly once (LSan enforces no
     * leak; ASan enforces no double-free / UAF from the payload reads). */
    {
        rc_reset();
        RcCell *cell = rc_new(5);
        DK *chain = dk_shift(TAG, body_k10_plus_k20, 0,
                       dk_frame_owning(rc_add_payload, (intptr_t)cell,
                         rc_clone, rc_drop,
                         dk_prompt(TAG, dk_done())));
        intptr_t r = dk_run(chain, 0);
        dk_free(chain);                            /* fire the base drop */
        check(r == 40 && g_rc_freed == 1 && g_rc_drop == g_rc_clone + 1
                  && g_rc_clone >= 1,
              "cps-e3a-owning-multishot-net-zero",
              "owning env not correctly cloned/dropped across a multi-shot resume");
    }

    printf("cps_prompt summary: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
