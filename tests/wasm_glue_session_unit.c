/* wasm_glue_session_unit.c -- the playground's eval session, driven natively
 * the way Try Turmeric drives it (playground-session-hygiene-plan).
 *
 * Each case is a sequence a user reaches from the page:
 *
 *   - PS2/PS3: hovering a name in the doc panel calls turi_doc_lookup.  It
 *     used to evaluate `(doc-lookup "name")` INTO the session; doc-lookup was
 *     never defined there, so the lookup failed, returned NULL for every stdlib
 *     name, and left the session unable to re-run the program in the editor
 *     ("'main' is already defined by an auto-loaded stdlib module").  A lookup
 *     now reads the generated table from C and must find stdlib docs, find
 *     builtin docs, miss cleanly, and leave Run working however many times it
 *     is called.
 *   - PS3: `(doc ...)` at the prompt, in each spelling, evaluates cleanly.
 *   - PS5: turi_wasm_rewind_to_prelude, which Run calls first, forgets what
 *     turns defined -- at runtime too -- and keeps the stdlib and its natives.
 *
 * Nothing in wasm_glue.c is wasm-specific, so linking libturi_wasm runs the
 * real entry points.  Runs from the source root, where stdlib/ lives (the
 * browser build embeds the same tree at /stdlib).
 */

#include <stdio.h>
#include <string.h>

#include "web/wasm_glue.h"
/* refine_stats(): the obligation counter the session-scaling case below reads.
 * src/compiler is on this target's include path. */
#include "refine_discharge.h"

static int passed = 0;
static int failed = 0;

#define CHECK(cond, msg) \
    do { \
        if (cond) { passed++; } \
        else { fprintf(stderr, "FAIL: %s (line %d)\n", (msg), __LINE__); failed++; } \
    } while (0)

/* Evaluate `src`; return 1 iff the result string contains `needle`. */
static int eval_contains(const char *src, const char *needle) {
    char *r = turi_wasm_eval(src);
    int hit = r && strstr(r, needle) != NULL;
    if (!hit) fprintf(stderr, "  eval %s\n    => %s\n", src, r ? r : "(null)");
    turi_wasm_free_string(r);
    return hit;
}

/* A Run of a program with a `main`, as runCode does it: the buffer, then
 * `(main)` as its own turn. */
static int run_main(const char *program, const char *expect) {
    return eval_contains(program, "#<fn main>") && eval_contains("(main)", expect);
}

int main(void) {
    CHECK(strcmp(turi_wasm_boot_timings(), "{}") == 0, "no boot phases before init");
    CHECK(turi_wasm_init() == 0, "turi_wasm_init succeeds");

    /* ---- WP5: the boot phase table the page reports ----------------------- */
    const char *bt = turi_wasm_boot_timings();
    CHECK(strncmp(bt, "{\"env_new\":", 11) == 0, "boot phases start with env_new");
    CHECK(strstr(bt, "\"preload_collections\":") != NULL, "boot phases carry the collection preload");
    CHECK(strstr(bt, "\"snapshot_prelude\":") != NULL && bt[strlen(bt) - 1] == '}',
          "boot phases end with the prelude snapshot, as a closed JSON object");

    /* ---- PS2/PS3: the doc panel ------------------------------------------ */
    CHECK(run_main("(defn main [] : int 1)", "1"), "first Run");

    const char *d = turi_doc_lookup("vec-map");
    CHECK(d && strncmp(d, "vec-map -- ", 11) == 0,
          "a stdlib name resolves to its generated docstring");
    CHECK(d && strstr(d, "Parameters:") != NULL,
          "the full docstring, not the one-line summary doc-names.json carries");
    d = turi_doc_lookup("tur/list");
    CHECK(d && strstr(d, "tur/list") != NULL, "a module name resolves too");
    d = turi_doc_lookup("let");
    CHECK(d && strstr(d, "(let ") != NULL, "a special form resolves from the builtin table");
    CHECK(turi_doc_lookup("no-such-name-anywhere") == NULL, "an unknown name misses cleanly");
    CHECK(turi_doc_lookup("a\"quote") == NULL, "a name that would break an eval string misses cleanly");

    for (int i = 0; i < 25; i++) (void)turi_doc_lookup("vec-map");
    CHECK(run_main("(defn main [] : int 2)", "2"),
          "Run after doc lookups re-runs the program (the reported failure)");
    CHECK(run_main("(defn main [] : int 3)", "3"), "and keeps working");

    /* ---- PS3: `(doc ...)` at the prompt ----------------------------------- */
    CHECK(eval_contains("(doc vec-map)", "nil"), "(doc name) evaluates");
    CHECK(eval_contains("(doc 'vec-map)", "nil"), "(doc 'name) evaluates");
    CHECK(eval_contains("(doc \"vec-map\")", "nil"), "(doc \"name\") evaluates");
    CHECK(eval_contains("(doc no-such-name-anywhere)", "nil"), "(doc unknown) evaluates");
    CHECK(run_main("(defn main [] : int 4)", "4"), "Run after (doc ...) still works");

    /* ---- PS5: rewinding to the prelude (what Run does first) --------------- */
    CHECK(eval_contains("(defn gone [] : int 11)", "#<fn gone>"), "define before the rewind");
    CHECK(eval_contains("(gone)", "11"), "and call it");
    CHECK(eval_contains("(defn map-count [m : int] : int 999)", "#<fn map-count>"),
          "reassign a prelude name");
    turi_wasm_rewind_to_prelude();
    CHECK(eval_contains("(gone)", "gone"), "a definition from before the rewind no longer resolves");
    CHECK(eval_contains("(map-count #map{:a 1 :b 2})", "2"),
          "a prelude name a turn reassigned has its prelude value back");
    CHECK(eval_contains("(when true 5)", "5"), "prelude macros survive");
    CHECK(eval_contains("(doc \"vec-map\")", "nil"), "native overrides survive");
    CHECK(run_main("(defn main [] : int 5)", "5"), "Run after a rewind");
    turi_wasm_rewind_to_prelude();
    CHECK(run_main("(defn main [] : int 6)", "6"), "and after a second one");

    /* ---- A turn pays for its own crossings, not for every earlier turn's ---
     *
     * refine_resolve_call_sites is deferred to the end of the UNIT so a call to
     * a later-defined function is checked like a call to an earlier-defined
     * one.  Under a session a turn is the unit -- but the crossing array is
     * session state and nothing clears it, so the pass used to re-resolve every
     * crossing the session had ever collected on every turn.  That is quadratic
     * work over a session, and because refine_collect_obligation does not
     * deduplicate, each re-resolution minted a FRESH undischarged obligation
     * that was then discharged again, backend calls and all.
     *
     * Counted rather than timed: obligations are deterministic, a clock is not.
     * g_stats is per-process here (only the compiler driver resets it), so the
     * delta over N turns that each add exactly one crossing is the measurement.
     * One per turn is the floor; pre-fix it was the running total, N*(N+1)/2 --
     * 1830 for the 60 turns below against a ceiling of 240.  The gap is wide
     * enough that this does not need a tight bound to be meaningful. */
    CHECK(eval_contains("(defn takes-pos [n : #refine{ x : int | (> x 0) }] : int n)",
                        "#<fn takes-pos>"),
          "a refined callee for the crossing below");
    {
        const unsigned before = refine_stats()->collected;
        char buf[256];
        const int turns = 60;
        int turns_ok = 1;
        for (int i = 0; i < turns; i++) {
            snprintf(buf, sizeof buf,
                     "(defn cross%d [x : int] : int (takes-pos (+ x %d)))", i, i + 1);
            /* A failed turn discards the session, and the replay that rebuilds
             * it would collect obligations of its own -- so a silent failure
             * here would look exactly like the bug. */
            turns_ok &= eval_contains(buf, "#<fn cross");
        }
        const unsigned added = refine_stats()->collected - before;
        CHECK(turns_ok, "every crossing turn succeeded");
        CHECK(added >= (unsigned)turns,
              "each turn's own crossing is still resolved (the pass is not skipped)");
        CHECK(added <= (unsigned)turns * 4,
              "a turn does not re-resolve the crossings of turns already finished");
        if (added > (unsigned)turns * 4)
            fprintf(stderr, "  %u obligations over %d turns (expected ~%d)\n",
                    added, turns, turns);
    }

    printf("wasm_glue_session_unit: %d passed, %d failed\n", passed, failed);
    return failed ? 1 : 0;
}
