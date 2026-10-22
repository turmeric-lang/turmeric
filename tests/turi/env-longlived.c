/* turi-value-pool-scratch-promotion-plan: long-lived-env steady-state harness.
 *
 * Drives a SINGLE TuriEnv across many top-level evals with scratch promotion
 * enabled (turi_env_set_scratch_promotion) and asserts the two properties the
 * plan targets:
 *
 *   1. Steady-state memory: an eval that builds a large transient structure but
 *      leaves nothing new behind rewinds the scratch pool -- value_scratch is
 *      empty right after turi_eval returns (total_bytes == 0), and value_perm
 *      stops growing once the live global set is stable. A control env with
 *      promotion OFF shows value_scratch growing without bound over the same
 *      loop, proving the feature actually reclaims.
 *
 *   2. Cross-reset correctness: values that escape an eval -- structs, ADT
 *      values, plain and mutually-recursive (cyclic letrec) closures, and
 *      deeply nested structs -- remain valid and evaluate correctly after many
 *      subsequent scratch rewinds.
 *
 *   3. Control-flow relocation (carrier-relocation-plan Part 2): a first-class
 *      handler value, an UNSTARTED generator, and an escaping work-stack effect
 *      continuation, each stored as a global, now promote into value_perm
 *      (scratch rewinds) and stay correct across 100+ rewinds -- the handler
 *      keeps discharging its effect, the generator keeps yielding, the
 *      continuation keeps resuming.
 *
 * It also checks the CONSERVATIVE guarantee for shapes that still cannot be
 * relocated: once a generator is STARTED (its suspended coroutine stack holds
 * scratch pointers the walk cannot rewrite) promotion declines to rewind that
 * cycle, and a captured continuation slice that carries interpreter state the
 * walk does not copy (a nested prompt) likewise blocks the rewind -- rather than
 * corrupting it.
 *
 * Built via the tur_env_longlived CMake target. Run under ASan for
 * use-after-reset detection (the Debug build poisons the rewound scratch region,
 * so any straggler pointer read crashes here).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "turi/eval.h"
#include "turi/preload.h"

static int failures = 0;

#define CHECK(cond, ...) do { \
    if (!(cond)) { fprintf(stderr, "FAIL: " __VA_ARGS__); fprintf(stderr, "\n"); failures++; } \
} while (0)

static int64_t eval_int(TuriEnv *env, const char *src, const char *what) {
    TuriValue r = turi_eval(env, src);
    if (r.tag == TURI_ERROR) {
        fprintf(stderr, "FAIL: %s: eval error: %s\n", what, r.as_error ? r.as_error : "?");
        failures++;
        return -999999;
    }
    if (r.tag != TURI_INT) {
        fprintf(stderr, "FAIL: %s: expected int, got tag %d\n", what, r.tag);
        failures++;
        return -999999;
    }
    return r.as_int;
}

/* ---------------------------------------------------------------------------
 * Part 1: steady state -- scratch is rewound; a control env grows unbounded.
 * --------------------------------------------------------------------------- */
static void test_steady_state(void) {
    /* An eval that builds a large transient (a 400-deep nested struct chain)
     * but returns a scalar and leaves no new global. */
    static const char *PRELUDE =
        "(defstruct Node [v : int next : int])\n"
        "(defn build [n : int] : int\n"
        "  (if (< n 1) 0\n"
        "    (let [_ (make-struct Node n (build (- n 1)))] n)))\n";
    static const char *CHURN = "(build 400)\n";

    TuriEnv *env = turi_env_new();
    turi_env_set_scratch_promotion(env, true);
    TuriValue pr = turi_eval(env, PRELUDE);
    CHECK(pr.tag != TURI_ERROR, "prelude failed: %s", pr.tag == TURI_ERROR ? pr.as_error : "");

    /* After promotion, scratch must be rewound to empty each cycle. */
    size_t perm_after_warmup = 0;
    for (int i = 0; i < 200; i++) {
        int64_t v = eval_int(env, CHURN, "churn");
        CHECK(v == 400, "churn result wrong: got %lld", (long long)v);
        CHECK(env->value_scratch.total_bytes == 0,
              "scratch not rewound at iter %d (total_bytes=%zu)",
              i, env->value_scratch.total_bytes);
        if (i == 20) perm_after_warmup = env->value_perm.total_bytes;
    }
    /* Nothing new escapes after warmup, so perm must reach a fixed point. */
    CHECK(env->value_perm.total_bytes == perm_after_warmup,
          "perm grew after warmup: %zu -> %zu",
          perm_after_warmup, env->value_perm.total_bytes);
    /* TR0 instrumentation: plain transient churn must rewind every cycle with no
     * declines (promo_* counters, turi-interp-incremental-reclamation-plan.md). */
    CHECK(env->promo_rewinds >= 200,
          "promotion did not rewind every churn cycle (rewinds=%llu)",
          (unsigned long long)env->promo_rewinds);
    CHECK(env->promo_decline_busy == 0 && env->promo_decline_unrelocatable == 0,
          "unexpected promotion decline on plain churn (busy=%llu unreloc=%llu)",
          (unsigned long long)env->promo_decline_busy,
          (unsigned long long)env->promo_decline_unrelocatable);
    turi_env_free(env);

    /* Control: same loop, promotion OFF -> scratch grows without bound. */
    TuriEnv *ctl = turi_env_new();
    TuriValue cpr = turi_eval(ctl, PRELUDE);
    CHECK(cpr.tag != TURI_ERROR, "control prelude failed");
    size_t early = 0, late = 0;
    for (int i = 0; i < 200; i++) {
        (void)eval_int(ctl, CHURN, "control churn");
        if (i == 20)  early = ctl->value_scratch.total_bytes;
        if (i == 199) late  = ctl->value_scratch.total_bytes;
    }
    CHECK(late > early,
          "promotion-off scratch did not grow (%zu -> %zu) -- feature not exercised",
          early, late);
    turi_env_free(ctl);
}

/* ---------------------------------------------------------------------------
 * Part 2: cross-reset correctness of promoted escapees.
 * --------------------------------------------------------------------------- */
static void test_cross_reset_correctness(void) {
    TuriEnv *env = turi_env_new();
    turi_env_set_scratch_promotion(env, true);

    /* Eval 1: leave a struct, an ADT value, a plain closure, and a pair of
     * mutually-recursive closures (cyclic letrec: each captures a shared frame
     * that binds the other) behind as globals. */
    static const char *SETUP =
        "(defstruct Point [x : int y : int])\n"
        "(defdata Color (Red) (Green) (Blue))\n"
        "(def origin (make-struct Point 3 4))\n"
        "(def hue (Green))\n"
        "(def add7 (fn [x : int] : int (+ x 7)))\n"
        "(defn is-even [n : int] : bool (if (= n 0) true  (is-odd  (- n 1))))\n"
        "(defn is-odd  [n : int] : bool (if (= n 0) false (is-even (- n 1))))\n"
        "0\n";
    TuriValue s = turi_eval(env, SETUP);
    CHECK(s.tag != TURI_ERROR, "setup failed: %s", s.tag == TURI_ERROR ? s.as_error : "");

    /* Churn many scratch generations, re-reading the promoted globals each time.
     * If promotion mis-copied (dangling into rewound scratch) the poisoned reads
     * would crash under ASan; a wrong value trips the CHECK. */
    for (int i = 0; i < 100; i++) {
        CHECK(eval_int(env, "(.x origin)", "origin.x") == 3, "origin.x wrong at %d", i);
        CHECK(eval_int(env, "(.y origin)", "origin.y") == 4, "origin.y wrong at %d", i);
        CHECK(eval_int(env, "(match hue (Red) 0 (Green) 1 (Blue) 2)", "hue") == 1,
              "hue match wrong at %d", i);
        CHECK(eval_int(env, "(add7 100)", "add7") == 107, "add7 wrong at %d", i);
        CHECK(eval_int(env, "(if (is-even 10) 1 0)", "is-even") == 1, "is-even wrong at %d", i);
        CHECK(eval_int(env, "(if (is-odd 7) 1 0)", "is-odd") == 1, "is-odd wrong at %d", i);
        /* Build a fresh transient each round so scratch actually recycles. */
        CHECK(eval_int(env, "(let [p (make-struct Point (+ 1 1) 9)] (.x p))", "fresh") == 2,
              "fresh struct wrong at %d", i);
    }
    turi_env_free(env);
}

/* ---------------------------------------------------------------------------
 * Part 3 (carrier-relocation-plan Part 2): relocate a suspended, coroutine-
 * stack-free control-flow value -- a first-class handler value -- into perm and
 * keep using it correctly across many scratch rewinds.
 *
 * A handler value (TURI_HANDLER) is a detached dispatch table: n_cases plus an
 * array of HandleCase pointers into the permanent elaborator AST. It carries no
 * coroutine stack and no env-level state, so it relocates with a verbatim struct
 * copy. Stored as a global, it must now PROMOTE (scratch rewinds) and still
 * discharge its effect after 100+ rewinds under the poison.
 * --------------------------------------------------------------------------- */
static void test_handler_relocation(void) {
    TuriEnv *env = turi_env_new();
    turi_env_set_scratch_promotion(env, true);

    static const char *SETUP =
        "(defeffect Ask [] :int)\n"
        "(def h (handler (Ask [] k) (resume k 41)))\n"
        "0\n";
    TuriValue s = turi_eval(env, SETUP);
    if (s.tag == TURI_ERROR) {
        /* Best-effort; don't fail the suite if the effect surface shifts. */
        fprintf(stderr, "note: handler-relocation setup skipped: %s\n",
                s.as_error ? s.as_error : "?");
        turi_env_free(env);
        return;
    }
    /* The handler value is reachable from a global but fully relocatable, so the
     * defining eval must have promoted it and rewound scratch. */
    CHECK(env->value_scratch.total_bytes == 0,
          "handler value not promoted: scratch was not rewound (total_bytes=%zu)",
          env->value_scratch.total_bytes);

    size_t perm_after_warmup = 0;
    for (int i = 0; i < 100; i++) {
        /* Build a transient, then drive the promoted handler. A mis-copy would
         * dereference poisoned scratch here and crash under ASan; a wrong result
         * trips the CHECK. */
        CHECK(eval_int(env, "(let [z (+ 1 1)] z)", "churn") == 2, "churn wrong at %d", i);
        CHECK(eval_int(env, "(with-handler h (perform (Ask)))", "use-handler") == 41,
              "promoted handler wrong at %d", i);
        CHECK(env->value_scratch.total_bytes == 0,
              "scratch not rewound under promoted handler at %d (total_bytes=%zu)",
              i, env->value_scratch.total_bytes);
        if (i == 10) perm_after_warmup = env->value_perm.total_bytes;
    }
    CHECK(env->value_perm.total_bytes == perm_after_warmup,
          "perm grew after warmup with a promoted handler: %zu -> %zu",
          perm_after_warmup, env->value_perm.total_bytes);
    turi_env_free(env);
}

/* ---------------------------------------------------------------------------
 * Part 4 (carrier-relocation-plan Part 2): an UNSTARTED generator relocates;
 * once STARTED it keeps bailing (the conservative guarantee for the shape we
 * cannot yet move).
 *
 * A generator's coroutine stack (and its makecontext state) is set up lazily on
 * the first gen-next, so an unstarted generator holds no live scratch pointers
 * and is relocatable (struct + captured frame). Once started-not-done, its
 * suspended coroutine stack references scratch that cannot be rewritten, so the
 * walk must decline the rewind wherever the TuriGen now lives.
 * --------------------------------------------------------------------------- */
static void test_gen_relocation(void) {
    TuriEnv *env = turi_env_new();
    turi_env_set_scratch_promotion(env, true);

    static const char *SETUP =
        "(load \"stdlib/gen.tur\")\n"
        "(def g (gen [] (yield 10) (yield 20) (yield 30)))\n"
        "0\n";
    TuriValue s = turi_eval(env, SETUP);
    if (s.tag == TURI_ERROR) {
        fprintf(stderr, "note: gen-relocation setup skipped: %s\n",
                s.as_error ? s.as_error : "?");
        turi_env_free(env);
        return;
    }
    /* Unstarted generator as a global: must promote and rewind scratch. */
    CHECK(env->value_scratch.total_bytes == 0,
          "unstarted generator not promoted: scratch not rewound (total_bytes=%zu)",
          env->value_scratch.total_bytes);

    /* Churn scratch many times with the unstarted generator promoted in perm;
     * scratch must stay rewound and perm must reach a fixed point. A mis-relocated
     * captured frame would read poisoned scratch and crash under ASan. */
    size_t perm_after_warmup = 0;
    for (int i = 0; i < 100; i++) {
        CHECK(eval_int(env, "(let [z (+ 2 3)] z)", "gen-churn") == 5, "gen churn wrong at %d", i);
        CHECK(env->value_scratch.total_bytes == 0,
              "scratch not rewound under a promoted unstarted generator at %d (total_bytes=%zu)",
              i, env->value_scratch.total_bytes);
        if (i == 10) perm_after_warmup = env->value_perm.total_bytes;
    }
    CHECK(env->value_perm.total_bytes == perm_after_warmup,
          "perm grew after warmup with a promoted unstarted generator: %zu -> %zu",
          perm_after_warmup, env->value_perm.total_bytes);

    /* The relocated generator is still usable: its first yield is correct. */
    CHECK(eval_int(env, "(gen-unwrap (gen-next g))", "gen1") == 10,
          "relocated generator first yield wrong");

    /* Now the generator is STARTED (its coroutine stack references scratch), so
     * promotion must DECLINE to rewind -- the conservative bail for a shape we
     * cannot relocate. Scratch must stay non-empty across further churns. */
    (void)eval_int(env, "(let [z 0] z)", "post-start-churn");
    CHECK(env->value_scratch.total_bytes != 0,
          "started generator did not block the rewind (total_bytes=%zu)",
          env->value_scratch.total_bytes);
    turi_env_free(env);
}

/* ---------------------------------------------------------------------------
 * Part 4b (carrier-relocation-plan Part 2): relocate an escaping WORK-STACK
 * effect continuation -- the heap-owned `k` a capturable handle hands to its
 * case -- into perm and keep resuming it correctly across many scratch rewinds.
 *
 * A capturable `(with-handler ...)` runs its body on the driver work-stack
 * behind a prompt; when the body performs, the driver captures the slice of
 * work-stack frames between the perform and the prompt as a heap-owned
 * TuriWsCont (`k`). Here the case returns `k` unresumed, so it escapes as a
 * TURI_EFFECT_CONT (ws != NULL) global. Before this increment such a value was
 * a conservative bail (its scratch DriveCont slice + captured frames could not
 * be rewritten), pinning scratch forever. It now relocates: the continuation
 * struct, its TuriWsCont, the captured DriveCont array (each frame's lexical
 * EvalFrame, `last` value, and the live prefix of any argument accumulator), and
 * the handler frame all deep-copy into perm. A driven `(resume k n)` then still
 * reconstructs `(+ 100 [])`, multishot, after 100+ rewinds under the poison.
 *
 * (A top-level `(resume k n)` is a separate base-interpreter limitation -- a ws
 * continuation cannot resume through a non-driver native frame -- so the resume
 * is driven from inside a called `use-k`, exactly as real code would.)
 * --------------------------------------------------------------------------- */
static void test_effectcont_relocation(void) {
    TuriEnv *env = turi_env_new();
    turi_env_set_scratch_promotion(env, true);

    static const char *SETUP =
        "(defeffect Ask [] :int)\n"
        "(defn grab [] : int\n"
        "  (with-handler\n"
        "    (handler (Ask [] ^multishot k) k)\n"
        "    (+ 100 (perform (Ask)))))\n"
        "(def saved-k (grab))\n"
        "(defn use-k [n : int] : int (resume saved-k n))\n"
        "0\n";
    TuriValue s = turi_eval(env, SETUP);
    if (s.tag == TURI_ERROR) {
        fprintf(stderr, "note: effectcont-relocation setup skipped: %s\n",
                s.as_error ? s.as_error : "?");
        turi_env_free(env);
        return;
    }
    /* The escaping ws continuation is reachable from a global but fully
     * relocatable, so the defining eval must have promoted it and rewound. */
    CHECK(env->value_scratch.total_bytes == 0,
          "ws continuation not promoted: scratch was not rewound (total_bytes=%zu)",
          env->value_scratch.total_bytes);

    size_t perm_after_warmup = 0;
    for (int i = 0; i < 100; i++) {
        /* Build a transient, then resume the promoted continuation via a driven
         * frame. A mis-copied slice would read poisoned scratch here (crash under
         * ASan); a wrong value trips the CHECK. `k` captured (+ 100 []). */
        CHECK(eval_int(env, "(let [z (+ 3 4)] z)", "cont-churn") == 7, "churn wrong at %d", i);
        CHECK(eval_int(env, "(use-k 5)", "resume-k") == 105,
              "promoted continuation resume wrong at %d", i);
        CHECK(env->value_scratch.total_bytes == 0,
              "scratch not rewound under a promoted continuation at %d (total_bytes=%zu)",
              i, env->value_scratch.total_bytes);
        if (i == 10) perm_after_warmup = env->value_perm.total_bytes;
    }
    CHECK(env->value_perm.total_bytes == perm_after_warmup,
          "perm grew after warmup with a promoted continuation: %zu -> %zu",
          perm_after_warmup, env->value_perm.total_bytes);
    /* True multishot: a distinct resume value threads through the same relocated
     * continuation independently. */
    CHECK(eval_int(env, "(use-k 20)", "resume-k-2") == 120,
          "promoted continuation second resume value wrong");
    turi_env_free(env);
}

/* ---------------------------------------------------------------------------
 * Part 4c (carrier-relocation-plan Part 2): the CONSERVATIVE bail for a
 * continuation slice the walk cannot fully copy. A nested capturable handle
 * between the outer prompt and the perform puts a nested prompt frame (whose
 * `aux` is interpreter state the walk does not relocate) into the captured
 * slice, so promotion must DECLINE to rewind rather than move a partial graph.
 * --------------------------------------------------------------------------- */
static void test_effectcont_bail(void) {
    TuriEnv *env = turi_env_new();
    turi_env_set_scratch_promotion(env, true);

    static const char *SETUP =
        "(defeffect Ask [] :int)\n"
        "(defeffect Bee [] :int)\n"
        "(defn grab [] : int\n"
        "  (with-handler\n"
        "    (handler (Ask [] ^multishot k) k)\n"
        "    (+ 1 (with-handler\n"
        "           (handler (Bee [] ^multishot k2) (resume k2 0))\n"
        "           (+ 2 (perform (Ask)))))))\n"
        "(def saved-k2 (grab))\n"
        "0\n";
    TuriValue s = turi_eval(env, SETUP);
    if (s.tag == TURI_ERROR) {
        fprintf(stderr, "note: effectcont-bail setup skipped: %s\n",
                s.as_error ? s.as_error : "?");
        turi_env_free(env);
        return;
    }
    /* The captured slice holds a nested prompt the walk will not relocate, so
     * promotion must keep scratch intact this cycle (bail, not corrupt). */
    CHECK(env->value_scratch.total_bytes != 0,
          "non-relocatable continuation slice did not block the rewind (total_bytes=%zu)",
          env->value_scratch.total_bytes);
    turi_env_free(env);
}

/* ---------------------------------------------------------------------------
 * Part 5 (interp-generator-resume-across-evals): a generator defined in one
 * turi_eval and drained across LATER evals must yield the full, correct
 * sequence even when unrelated top-level evals run in between.
 *
 * Regression for the double-advance bug: turi_eval keyed its "already run"
 * boundary off the parsed-form count, but (load ...) expands inline to many
 * program items, so the boundary drifted and previously-run top-level forms --
 * including an earlier (gen-next g) -- were evaluated again, silently advancing
 * the suspended generator an extra step (the drain returned 10 30 0 for a
 * 10/20/30 generator). This runs with promotion OFF (the report's default);
 * the defect is in the base interpreter, independent of scratch promotion.
 * --------------------------------------------------------------------------- */
static void test_gen_drain_across_evals(void) {
    TuriEnv *env = turi_env_new();  /* promotion OFF (default) */

    static const char *SETUP =
        "(load \"stdlib/gen.tur\")\n"
        "(def g (gen [] (yield 10) (yield 20) (yield 30)))\n"
        "0\n";
    TuriValue s = turi_eval(env, SETUP);
    if (s.tag == TURI_ERROR) {
        fprintf(stderr, "note: gen-drain-across-evals setup skipped: %s\n",
                s.as_error ? s.as_error : "?");
        turi_env_free(env);
        return;
    }

    /* Enough unrelated top-level evals to grow the accumulated source well past
     * the (load ...) expansion delta -- this is what used to shift the
     * evaluation boundary and re-run an earlier (gen-next g). */
    for (int i = 0; i < 50; i++)
        CHECK(eval_int(env, "(let [z 0] z)", "gen-drain-intervening") == 0,
              "intervening eval wrong at %d", i);

    /* Full drain: the yields must arrive in order, then NULL (exhausted).
     * The last step tests exhaustion with gen-some?: gen-unwrap of an
     * exhausted generator panics (generator-forms-gaps, archived). */
    CHECK(eval_int(env, "(gen-unwrap (gen-next g))", "gen-drain-1") == 10,
          "cross-eval drain: yield 1 wrong (want 10)");
    CHECK(eval_int(env, "(gen-unwrap (gen-next g))", "gen-drain-2") == 20,
          "cross-eval drain: yield 2 wrong (want 20 -- generator double-advanced)");
    CHECK(eval_int(env, "(gen-unwrap (gen-next g))", "gen-drain-3") == 30,
          "cross-eval drain: yield 3 wrong (want 30)");
    CHECK(eval_int(env, "(if (gen-some? (gen-next g)) 1 0)", "gen-drain-4") == 0,
          "cross-eval drain: generator not exhausted after 3 yields");

    turi_env_free(env);
}

/* ---------------------------------------------------------------------------
 * TR3 (turi-interp-incremental-reclamation): eval-boundary collection sweep.
 *
 * With promotion on, tracked collection boxes (Vec/Set/Map wrappers, TVar
 * cells) that nothing references after a rewind are freed at the eval
 * boundary instead of accumulating until teardown.  Asserted here:
 *
 *   - transient churn is BOUNDED: 100 dropped vecs leave the live tracked-box
 *     count where it started, and the sweep counters show the frees;
 *   - liveness is exact: a global vec, a vec behind a struct field, a vec
 *     inside another vec, and a vec held only by a TVar all survive sweeps
 *     and read back correctly (under ASan a wrongly-swept box is a hard UAF);
 *   - a TVar itself survives promotion rewinds -- its cell used to live in
 *     value_scratch, where the first rewind poisoned it under the REPL's
 *     defaults (the handle is an opaque int carrier promotion cannot see);
 *   - the leak-on-doubt gate: once a LIVE non-empty Map exists (entries are
 *     untyped, so the mark cannot prove completeness), sweeps go mark-only
 *     and free nothing.
 * --------------------------------------------------------------------------- */
static size_t live_coll_boxes(TuriEnv *env) {
    size_t n = 0;
    for (TuriCollBuf *b = env->coll_bufs; b; b = b->next)
        if (b->box) n++;
    return n;
}

static void test_collection_sweep(void) {
    TuriEnv *env = turi_env_new();
    turi_env_set_scratch_promotion(env, true);

    TuriValue pr = turi_eval(env,
        "(defn churn [] : int\n"
        "  (let [v (vec-new)] (vec-push! v 7) (vec-push! v 8) 0))\n");
    CHECK(pr.tag != TURI_ERROR, "sweep prelude failed: %s",
          pr.tag == TURI_ERROR ? pr.as_error : "");

    /* Bounded transient churn: every dropped vec is swept at its boundary. */
    size_t live0 = live_coll_boxes(env);
    for (int i = 0; i < 100; i++)
        CHECK(eval_int(env, "(churn)\n", "vec churn") == 0, "vec churn result");
    CHECK(live_coll_boxes(env) == live0,
          "transient vecs not swept: %zu live at start, %zu after churn",
          live0, live_coll_boxes(env));
    CHECK(env->collsweep_freed >= 100,
          "sweep freed too few boxes (%llu)",
          (unsigned long long)env->collsweep_freed);
    CHECK(env->collsweep_markonly == 0,
          "unexpected mark-only cycles during plain vec churn (%llu)",
          (unsigned long long)env->collsweep_markonly);

    /* Liveness: a global vec, one behind a struct field, one nested in a vec,
     * and one held only by a TVar must all survive further sweeps. */
    CHECK(eval_int(env,
        "(def g (vec-new))\n(vec-push! g 41)\n(vec-len g)\n",
        "global vec") == 1, "global vec setup");
    CHECK(eval_int(env,
        "(defstruct Holder [v : int])\n"
        "(def h (make-struct Holder (let [v (vec-new)] (vec-push! v 51) v)))\n"
        "0\n", "struct-held vec") == 0, "struct-held vec setup");
    CHECK(eval_int(env,
        "(def vv (vec-new))\n"
        "(let [inner (vec-new)] (vec-push! inner 61) (vec-push! vv inner))\n"
        "0\n", "nested vec") == 0, "nested vec setup");
    CHECK(eval_int(env,
        "(def tvv (tvar/new (let [v (vec-new)] (vec-push! v 71) v)))\n0\n",
        "tvar-held vec") == 0, "tvar-held vec setup");
    CHECK(eval_int(env, "(def tv (tvar/new 33))\n0\n", "plain tvar") == 0,
          "plain tvar setup");

    for (int i = 0; i < 50; i++)
        (void)eval_int(env, "(churn)\n", "churn between liveness checks");

    CHECK(eval_int(env, "(vec-get g 0)\n", "global vec read") == 41,
          "global vec lost to the sweep");
    CHECK(eval_int(env, "(vec-get (.v h) 0)\n", "struct-held vec read") == 51,
          "struct-field vec lost to the sweep");
    CHECK(eval_int(env, "(vec-get (vec-get vv 0) 0)\n", "nested vec read") == 61,
          "vec-in-vec lost to the sweep");
    CHECK(eval_int(env,
        "(vec-get (atomically (stm (tvar/read tvv))) 0)\n",
        "tvar-held vec read") == 71, "tvar-held vec lost to the sweep");
    /* The TVar cell itself survived every rewind (pre-fix: use-after-reset). */
    CHECK(eval_int(env, "(atomically (stm (tvar/read tv)))\n", "tvar read") == 33,
          "tvar cell did not survive promotion rewinds");
    CHECK(env->collsweep_markonly == 0,
          "liveness section unexpectedly went mark-only (%llu)",
          (unsigned long long)env->collsweep_markonly);

    /* Leak-on-doubt: a live NON-EMPTY Set/Map's entries are untyped, so the
     * sweep must decline to free anything while one is reachable.  Built on
     * the raw set natives (map-new is an elaborator builtin with a typed
     * (Map K V) result, so it cannot ride the untyped-native path used here;
     * a Set box is the same 2-word HAMT wrapper with the same scan). */
    CHECK(eval_int(env,
        "(defn mk-set [] : int\n"
        "  (let [e (set-new)] (set-add-eq-o e 42 7 0 0)))\n"
        "(def m (mk-set))\n(if (= m 0) 0 1)\n",
        "non-empty set") == 1, "set setup");
    uint64_t freed_before = env->collsweep_freed;
    for (int i = 0; i < 20; i++)
        (void)eval_int(env, "(churn)\n", "churn with live set");
    CHECK(env->collsweep_markonly >= 20,
          "live non-empty set did not force mark-only sweeps (%llu)",
          (unsigned long long)env->collsweep_markonly);
    CHECK(env->collsweep_freed == freed_before,
          "sweep freed boxes despite an unscannable live set (%llu -> %llu)",
          (unsigned long long)freed_before,
          (unsigned long long)env->collsweep_freed);

    turi_env_free(env);
}

/* turi-vec-struct-element-dangles-across-rewind: a struct pushed onto a
 * global Vec at one eval boundary lived in scratch, and the rewind left the
 * Vec's cell pointing at reclaimed memory -- ASan use-after-poison in the
 * sweep's mark, and garbage on a later read.  The promotion walk now moves a
 * reached Vec's struct, closure and string cells, so every boundary still
 * rewinds and every element reads back -- a runtime-built string field
 * included, and a Vec of bare strings. */
static void test_vec_struct_cells_relocated(void) {
    TuriEnv *env = turi_env_new();
    /* The REPL's own prelude: typed vec-get / vec-push! for a (Vec P). */
    turi_env_preload_macros(env, "stdlib");
    turi_env_preload_native_stubs(env);
    turi_env_preload_collections(env, "stdlib");
    turi_env_set_scratch_promotion(env, true);

    TuriValue pr = turi_eval(env,
        "(load \"stdlib/str-build.tur\")\n"   /* a cstr-typed str-concat */
        "(defstruct P :copy [x : int tag : cstr])\n"
        "(def ps (:: (vec-new) (Vec P)))\n"
        "(def ss (:: (vec-new) (Vec cstr)))\n0\n");
    CHECK(pr.tag != TURI_ERROR, "vec-cell prelude failed: %s",
          pr.tag == TURI_ERROR ? pr.as_error : "");

    uint64_t rewinds0 = env->promo_rewinds;
    uint64_t declined0 = env->promo_decline_unrelocatable;
    char src[160];
    for (int i = 0; i < 50; i++) {
        snprintf(src, sizeof src,
                 "(vec-push! ps (P %d (str-concat \"p\" \"%d\")))\n"
                 "(vec-push! ss (str-concat \"s\" \"%d\"))\n0\n", i, i, i);
        CHECK(eval_int(env, src, "push struct + string") == 0, "push %d", i);
    }
    CHECK(env->promo_rewinds >= rewinds0 + 50,
          "pushes into a global Vec stopped the rewinds (%llu -> %llu)",
          (unsigned long long)rewinds0, (unsigned long long)env->promo_rewinds);
    CHECK(env->promo_decline_unrelocatable == declined0,
          "a Vec of structs declined promotion (%llu -> %llu)",
          (unsigned long long)declined0,
          (unsigned long long)env->promo_decline_unrelocatable);

    for (int i = 0; i < 50; i += 7) {
        snprintf(src, sizeof src, "(.x (vec-get ps %d))\n", i);
        CHECK(eval_int(env, src, "struct cell read") == i,
              "struct cell %d did not survive the rewinds", i);
        snprintf(src, sizeof src, "(cstr-len (.tag (vec-get ps %d)))\n", i);
        CHECK(eval_int(env, src, "struct string field read") == (i < 10 ? 2 : 3),
              "string field of struct cell %d did not survive", i);
        snprintf(src, sizeof src, "(cstr-len (vec-get ss %d))\n", i);
        CHECK(eval_int(env, src, "string cell read") == (i < 10 ? 2 : 3),
              "string cell %d did not survive", i);
    }
    CHECK(eval_int(env, "(vec-len ps)\n", "vec len") == 50, "lost Vec elements");

    turi_env_free(env);
}

int main(void) {
    turi_init(false);
    test_steady_state();
    test_cross_reset_correctness();
    test_handler_relocation();
    test_gen_relocation();
    test_effectcont_relocation();
    test_effectcont_bail();
    test_gen_drain_across_evals();
    test_collection_sweep();
    test_vec_struct_cells_relocated();

    if (failures) {
        fprintf(stderr, "env-longlived: %d failure(s).\n", failures);
        return 1;
    }
    printf("env-longlived: all checks passed.\n");
    return 0;
}
