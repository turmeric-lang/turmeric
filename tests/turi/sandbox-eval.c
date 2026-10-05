/* Sandbox eval test harness (SB0; security-audit-plan WP3, S-5).
 *
 * Four parts (the fourth, host exit, is further down):
 *
 *   1. Fixtures.  Each file in tests/fixtures/sandbox/ is evaluated in a fresh
 *      turi_env_new_sandboxed() env and must fail FOR ITS STATED REASON: the
 *      expected substring has to appear in the error value or in a diagnostic
 *      the env emitted.  (Before WP3 this only checked for TURI_ERROR, and
 *      sb-println passed on "unknown function 'println-int'" and sb-import on
 *      "import is only allowed inside defmodule" -- neither of which was the
 *      sandbox refusing anything.)
 *
 *   2. The classification table (src/turi/native_caps.c), generated rather
 *      than listed, so a new native cannot be added unclassified:
 *        - the table is strictly sorted (it is binary-searched);
 *        - every native a fresh turi_env_new() holds has a row;
 *        - every row with a requirement is REFUSED in a sandboxed env, by the
 *          capability check, before the native runs.  A row whose native is
 *          only registered conditionally (reload, break, ...) is registered
 *          here under its name with a trap body, which also exercises the
 *          name-to-requirement stamping in turi_env_register_native.
 *
 *   3. Capability API behaviour: granting a bit admits exactly the natives
 *      that need it; an explicit TURI_CAP_NONE registration overrides the
 *      table; pure natives keep working under CAP_NONE.
 *
 * Compile (via CMake):
 *   cmake --build build --target tur_eval_sandbox
 *   ./build/tur_eval_sandbox
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "turi/eval.h"

static int failures = 0;
static int passes   = 0;

static void pass(const char *what, const char *detail) {
    printf("PASS [%s]%s%s\n", what, detail ? " => " : "", detail ? detail : "");
    passes++;
}

static void fail(const char *what, const char *detail) {
    fprintf(stderr, "FAIL [%s]: %s\n", what, detail);
    failures++;
}

/* ---- diagnostic capture ------------------------------------------------ */

typedef struct { char text[4096]; size_t len; } DiagLog;

static void capture_diag(TuriEnv *env, int level, const char *code,
                         const char *file, uint32_t line, uint32_t cs,
                         uint32_t ce, const char *message, void *ud) {
    (void)env; (void)level; (void)code; (void)file; (void)line; (void)cs; (void)ce;
    DiagLog *log = (DiagLog *)ud;
    if (!message) return;
    int w = snprintf(log->text + log->len, sizeof log->text - log->len,
                     "%s\n", message);
    if (w > 0) {
        log->len += (size_t)w;
        if (log->len >= sizeof log->text) log->len = sizeof log->text - 1;
    }
}

/* ---- part 1: fixtures -------------------------------------------------- */

static void run_fixture(const char *dir, const char *file, const char *expect) {
    char path[512];
    snprintf(path, sizeof path, "%s/%s", dir, file);
    TuriEnv *env = turi_env_new_sandboxed();
    if (!env) { fail(file, "turi_env_new_sandboxed returned NULL"); return; }
    DiagLog log = {{0}, 0};
    turi_env_set_diag_sink(env, capture_diag, &log);
    TuriValue got = turi_eval_file(env, path);
    char msg[512];
    if (got.tag != TURI_ERROR) {
        char repr[128];
        turi_value_repr(repr, sizeof repr, got);
        snprintf(msg, sizeof msg, "expected TURI_ERROR, got tag %d (%s)", got.tag, repr);
        fail(file, msg);
    } else {
        const char *err = got.as_error ? got.as_error : "";
        if (strstr(err, expect) || strstr(log.text, expect)) {
            pass(file, strstr(err, expect) ? err : expect);
        } else {
            snprintf(msg, sizeof msg,
                     "failed for the wrong reason: wanted \"%s\", got \"%s\" / diags \"%.200s\"",
                     expect, err, log.text);
            fail(file, msg);
        }
    }
    turi_env_free(env);
}

/* ---- part 2: the classification table ---------------------------------- */

static TuriValue trap_native(TuriEnv *env, TuriValue *args, uint32_t n, void *ud) {
    (void)env; (void)args; (void)n;
    fprintf(stderr, "FAIL [table]: native '%s' RAN in a sandboxed env\n",
            (const char *)ud);
    failures++;
    return turi_nil();
}

static void check_table_sorted(void) {
    size_t n = 0;
    const TuriNativeCapRow *t = turi_native_cap_table(&n);
    for (size_t i = 1; i < n; i++) {
        if (strcmp(t[i - 1].name, t[i].name) >= 0) {
            char msg[256];
            snprintf(msg, sizeof msg, "rows out of order or duplicated: '%s' then '%s'",
                     t[i - 1].name, t[i].name);
            fail("table/sorted", msg);
            return;
        }
    }
    char detail[64];
    snprintf(detail, sizeof detail, "%zu rows", n);
    pass("table/sorted", detail);
}

static void check_every_native_classified(void) {
    TuriEnv *env = turi_env_new();
    size_t seen = 0, missing = 0;
    for (EnvBinding *b = env->globals; b; b = b->next) {
        if (!turi_value_is_native(b->value)) continue;
        seen++;
        if (!turi_native_cap_find(b->name)) {
            fprintf(stderr, "FAIL [table/classified]: native '%s' has no row in "
                            "src/turi/native_caps.c -- classify it\n", b->name);
            missing++;
        }
    }
    turi_env_free(env);
    if (missing) {
        failures++;
    } else {
        char detail[64];
        snprintf(detail, sizeof detail, "%zu natives, all classified", seen);
        pass("table/classified", detail);
    }
}

static void check_every_requirement_refused(void) {
    size_t n = 0, checked = 0, stamped = 0;
    const TuriNativeCapRow *t = turi_native_cap_table(&n);
    for (size_t i = 0; i < n; i++) {
        if (t[i].caps == 0) continue;
        TuriEnv *env = turi_env_new_sandboxed();
        TuriValue fn = turi_env_get(env, t[i].name);
        if (!turi_value_is_native(fn)) {
            /* Conditionally registered in real use; register a trap under the
             * same name so the table's stamping is what is under test. */
            turi_env_register_native(env, t[i].name, trap_native, (void *)t[i].name);
            fn = turi_env_get(env, t[i].name);
            stamped++;
        }
        TuriValue r = turi_call(env, fn, NULL, 0);
        if (r.tag != TURI_ERROR || !r.as_error ||
            !strstr(r.as_error, "requires capability")) {
            char msg[256];
            snprintf(msg, sizeof msg, "'%s' was not refused by the capability check (tag %d: %s)",
                     t[i].name, r.tag, r.tag == TURI_ERROR && r.as_error ? r.as_error : "-");
            fail("table/refused", msg);
        }
        checked++;
        turi_env_free(env);
    }
    char detail[96];
    snprintf(detail, sizeof detail, "%zu classified natives refused (%zu via a stamped trap)",
             checked, stamped);
    pass("table/refused", detail);
}

/* ---- part 3: capability API behaviour ---------------------------------- */

static void check_grant_admits(void) {
    TuriEnv *env = turi_env_new_sandboxed();
    TuriValue fn = turi_env_get(env, "r7rs-getenv__");
    TuriValue arg = turi_cstr("PATH");
    TuriValue denied = turi_call(env, fn, &arg, 1);
    turi_env_allow(env, TURI_CAP_ENV);
    TuriValue allowed = turi_call(env, fn, &arg, 1);
    turi_env_allow(env, TURI_CAP_IO | TURI_CAP_FS);   /* not enough for proc */
    TuriValue spawn = turi_call(env, turi_env_get(env, "process/spawn-raw"), NULL, 0);
    if (denied.tag == TURI_ERROR && allowed.tag == TURI_CSTR &&
        spawn.tag == TURI_ERROR && spawn.as_error &&
        strstr(spawn.as_error, "requires capability proc"))
        pass("caps/grant-admits-exactly", "env admits getenv; io+fs still refuse spawn");
    else
        fail("caps/grant-admits-exactly", "granting TURI_CAP_ENV did not admit exactly getenv");
    turi_env_free(env);
}

static TuriValue answer_native(TuriEnv *env, TuriValue *args, uint32_t n, void *ud) {
    (void)env; (void)args; (void)n; (void)ud;
    return turi_int(42);
}

static void check_explicit_override(void) {
    TuriEnv *env = turi_env_new_sandboxed();
    /* An embedder that deliberately exposes a safe native under a classified
     * builtin name states so with TURI_CAP_NONE. */
    turi_env_register_native_caps(env, "r7rs-getenv__", answer_native, NULL, TURI_CAP_NONE);
    /* And one that wants its own native gated asks for it. */
    turi_env_register_native_caps(env, "host/secret", answer_native, NULL, TURI_CAP_ENV);
    TuriValue a = turi_call(env, turi_env_get(env, "r7rs-getenv__"), NULL, 0);
    TuriValue b = turi_call(env, turi_env_get(env, "host/secret"), NULL, 0);
    if (a.tag == TURI_INT && a.as_int == 42 && b.tag == TURI_ERROR &&
        b.as_error && strstr(b.as_error, "'host/secret' requires capability env"))
        pass("caps/explicit-registration", "CAP_NONE overrides the table; explicit caps gate");
    else
        fail("caps/explicit-registration", "turi_env_register_native_caps not honoured");
    turi_env_free(env);
}

static void check_pure_still_works(void) {
    TuriEnv *env = turi_env_new_sandboxed();
    TuriValue r1 = turi_eval(env, "(str-concat \"a\" \"b\")");
    TuriValue r2 = turi_eval(env, "(vec-len (vec-new))");
    if (r1.tag == TURI_CSTR && r1.as_cstr && strcmp(r1.as_cstr, "ab") == 0 &&
        r2.tag == TURI_INT && r2.as_int == 0)
        pass("caps/pure-natives-run", "str-concat, vec-new, vec-len");
    else
        fail("caps/pure-natives-run", "a pure native was refused under CAP_NONE");
    turi_env_free(env);
}

/* ---- part 4: host exit (S-5's second half) ------------------------------
 * A restricted env may not end the host process.  Every panic path that
 * would exit() or abort() -- an uncaught panic, a native's own
 * out-of-bounds exit, a failed contract -- comes back as TURI_ERROR
 * "panic: <msg>", the env stays usable, and catch-unwind still catches. */

static void expect_panic_error(TuriEnv *env, const char *what, const char *src,
                               const char *want) {
    TuriValue r = turi_eval(env, src);
    if (r.tag == TURI_ERROR && r.as_error && strstr(r.as_error, want))
        pass(what, r.as_error);
    else {
        char msg[256];
        snprintf(msg, sizeof msg, "wanted TURI_ERROR containing \"%s\", got tag %d (%s)",
                 want, r.tag, r.tag == TURI_ERROR && r.as_error ? r.as_error : "-");
        fail(what, msg);
    }
}

static void check_host_exit_is_an_error(void) {
    TuriEnv *env = turi_env_new_sandboxed();
    expect_panic_error(env, "host-exit/panic", "(panic \"x\")", "panic: x");
    expect_panic_error(env, "host-exit/vec-oob",
                       "(let [v (vec-new)] (vec-get v 5))",
                       "panic: vec index out of bounds");
    expect_panic_error(env, "host-exit/contract",
                       "(tur-contract-check false \"contract broke\")",
                       "panic: contract broke");

    /* The env is still usable after each of those. */
    TuriValue after = turi_eval(env, "(+ 1 2)");
    if (after.tag == TURI_INT && after.as_int == 3)
        pass("host-exit/env-survives", "(+ 1 2) => 3 after three panics");
    else
        fail("host-exit/env-survives", "env unusable after a caught host exit");

    /* User catch-unwind is untouched: it catches before the pad is reached. */
    TuriValue caught = turi_eval(env,
        "(err? (catch-unwind (fn [] : int (panic \"inner\"))))");
    if (caught.tag == TURI_BOOL && caught.as_bool)
        pass("host-exit/catch-unwind-still-catches", NULL);
    else
        fail("host-exit/catch-unwind-still-catches", "catch-unwind no longer caught the panic");

    /* The turi_call entry point (how the macro env calls a defmacro*) too. */
    TuriValue fn = turi_eval(env, "(fn [] : int (panic \"via call\"))");
    TuriValue r = turi_call(env, fn, NULL, 0);
    if (r.tag == TURI_ERROR && r.as_error && strstr(r.as_error, "panic: via call"))
        pass("host-exit/turi-call", r.as_error);
    else
        fail("host-exit/turi-call", "a panic through turi_call did not come back as an error");
    turi_env_free(env);
}

/* ---- part 5: handle forgery (S-5) ---------------------------------------
 * A restricted env carries every collection / string / iterator / symbol /
 * cons handle as a bare int64 a native casts back to a pointer.  The
 * provenance registry refuses a handle argument that was not minted by a
 * constructor of the matching kind, so a raw integer cannot be forged into a
 * wild read/write -- while a genuinely-minted handle still round-trips. */

static void expect_forgery_refused(TuriEnv *env, const char *what, const char *src) {
    TuriValue r = turi_eval(env, src);
    if (r.tag == TURI_ERROR && r.as_error && strstr(r.as_error, "not a live handle"))
        pass(what, NULL);
    else {
        char msg[256];
        snprintf(msg, sizeof msg, "forged handle was NOT refused (tag %d: %s)",
                 r.tag, r.tag == TURI_ERROR && r.as_error ? r.as_error : "-");
        fail(what, msg);
    }
}

static void check_handle_forgery_refused(void) {
    TuriEnv *env = turi_env_new_sandboxed();
    if (!env) { fail("forgery", "env alloc"); return; }
    /* The report's headline repro and its siblings: a raw integer as a handle. */
    expect_forgery_refused(env, "forgery/vec-get",     "(vec-get 4096 0)");
    expect_forgery_refused(env, "forgery/vec-len",     "(vec-len 4096)");
    expect_forgery_refused(env, "forgery/vec-set",     "(vec-set! 4096 0 9)");
    expect_forgery_refused(env, "forgery/hamt-count",  "(tur_hamt_count 4096)");
    expect_forgery_refused(env, "forgery/hamt-get",    "(tur_hamt_get 4096 1 1)");
    expect_forgery_refused(env, "forgery/string-len",  "(tur_string_len 4096)");
    expect_forgery_refused(env, "forgery/sym",         "(sym->str 4096)");
    expect_forgery_refused(env, "forgery/list-head",   "(head 4096)");
    expect_forgery_refused(env, "forgery/list-tail",   "(tail 4096)");
    /* Kind confusion: a real Vec handle replayed where a HAMT is expected. */
    expect_forgery_refused(env, "forgery/kind-confusion",
                           "(let [v (vec-new)] (vec-push! v 5) (tur_hamt_count v))");
    /* Use-after-free: the freed vec's pointer is forgotten. */
    expect_forgery_refused(env, "forgery/use-after-free",
                           "(let [v (vec-new)] (vec-free v) (vec-len v))");
    /* The continuation channel: resume / clone / serialize are folded by the
     * driver or dispatched as builtins, not through the native dispatch the
     * guard above sits on, so they carry a check of their own (TURI_HK_CONT). */
    expect_forgery_refused(env, "forgery/resume-cont",  "(resume-cont! 4096 0)");
    expect_forgery_refused(env, "forgery/save-cont",    "(save-cont! 4096)");
    expect_forgery_refused(env, "forgery/cloneable-cont-resume",
                           "(tur_cloneable_cont_resume 4096 0)");
    expect_forgery_refused(env, "forgery/serial-cont-resume",
                           "(tur_serial_cont_resume 4096 0)");
    expect_forgery_refused(env, "forgery/cloneable-cont-clone",
                           "(tur_cloneable_cont_clone 4096)");
    /* The value-model channel: an erasing ascription on a type variable
     * re-types a caller integer as a function, and the call through it used
     * to jump to 4096.  No native is involved, so the retag carries its own
     * check (TURI_HK_CLOSURE, minted where a closure loses its tag). */
    turi_eval(env, "(defn mk-forged [A] [x : int] : A (:: x A))");
    expect_forgery_refused(env, "forgery/closure-retag",
                           "(let [f : (fn [int] int) (mk-forged 4096)] (f 1))");
    turi_env_free(env);
}

static void check_handles_still_round_trip(void) {
    TuriEnv *env = turi_env_new_sandboxed();
    if (!env) { fail("handles-ok", "env alloc"); return; }
    struct { const char *what; const char *src; int64_t want; } cases[] = {
        { "handles-ok/vec",    "(let [v (vec-new)] (vec-push! v 7) (vec-get v 0))", 7 },
        { "handles-ok/vec-len","(vec-len (vec-new))",                                0 },
        { "handles-ok/hamt",   "(tur_hamt_count (tur_hamt_set (tur_hamt_new) 42 100 200))", 1 },
        { "handles-ok/hamt-get","(tur_hamt_get (tur_hamt_set (tur_hamt_new) 42 100 200) 42 100)", 200 },
        { "handles-ok/string", "(tur_string_len (tur_string_from_cstr \"hello\"))",  5 },
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        TuriValue r = turi_eval(env, cases[i].src);
        if (r.tag == TURI_INT && r.as_int == cases[i].want) pass(cases[i].what, NULL);
        else {
            char msg[256];
            snprintf(msg, sizeof msg, "a live handle was wrongly refused (tag %d: %s)",
                     r.tag, r.tag == TURI_ERROR && r.as_error ? r.as_error : "-");
            fail(cases[i].what, msg);
        }
    }
    /* A continuation a capture handed out is minted, so it still resumes. */
    turi_eval(env, "(defn k-resume [k] : int (tur_cloneable_cont_resume k 10))");
    TuriValue kr = turi_eval(env, "(cloneable-reset (+ 1 (cloneable-shift k-resume 0)))");
    if (kr.tag == TURI_INT && kr.as_int == 11) pass("handles-ok/continuation", NULL);
    else {
        char msg[256];
        snprintf(msg, sizeof msg, "a live continuation was wrongly refused (tag %d: %s)",
                 kr.tag, kr.tag == TURI_ERROR && kr.as_error ? kr.as_error : "-");
        fail("handles-ok/continuation", msg);
    }
    /* A closure that really lost its tag -- stored in a Vec, read back as a
     * bare word -- was minted when it entered vec-push!, so the re-tag at the
     * fn-typed call head still accepts it (the closure-retag guard's control). */
    turi_eval(env, "(defn call1-sbx [^fat f : (fn [int] int) x : int] : int (f x))");
    TuriValue cr = turi_eval(env,
        "(let [v (vec-new) n 1]"
        "  (vec-push! v (:: (fn [x : int] : int (+ x n)) int))"
        "  (call1-sbx (:: (vec-get v 0) ptr<void>) 41))");
    if (cr.tag == TURI_INT && cr.as_int == 42) pass("handles-ok/closure-carrier", NULL);
    else {
        char msg[256];
        snprintf(msg, sizeof msg, "a stored closure was wrongly refused (tag %d: %s)",
                 cr.tag, cr.tag == TURI_ERROR && cr.as_error ? cr.as_error : "-");
        fail("handles-ok/closure-carrier", msg);
    }
    /* A cstr literal is a trusted reader pointer, not a forgeable integer. */
    TuriValue s = turi_eval(env, "(str-concat \"a\" \"b\")");
    if (s.tag == TURI_CSTR && s.as_cstr && strcmp(s.as_cstr, "ab") == 0)
        pass("handles-ok/cstr-literal-trusted", NULL);
    else
        fail("handles-ok/cstr-literal-trusted", "a cstr literal was refused");
    turi_env_free(env);
}

/* The handle-signature table must stay sorted (it is binary-searched), and
 * every handle-taking native must still carry a cap row -- so a native cannot
 * lose its classification and slip a handle argument past the guard. */
static void check_handle_table(void) {
    size_t n = 0;
    const TuriNativeHandleRow *t = turi_native_handle_table(&n);
    for (size_t i = 1; i < n; i++) {
        if (strcmp(t[i - 1].name, t[i].name) >= 0) {
            char msg[256];
            snprintf(msg, sizeof msg, "handle rows out of order: '%s' then '%s'",
                     t[i - 1].name, t[i].name);
            fail("handle-table/sorted", msg);
            return;
        }
    }
    for (size_t i = 0; i < n; i++) {
        if (!turi_native_cap_find(t[i].name)) {
            char msg[256];
            snprintf(msg, sizeof msg, "handle native '%s' has no cap row", t[i].name);
            fail("handle-table/cap-row", msg);
            return;
        }
    }
    char detail[64];
    snprintf(detail, sizeof detail, "%zu handle rows", n);
    pass("handle-table", detail);
}

/* Re-enable async; I/O must still be denied. */
static void run_mixed_caps_test(void) {
    TuriEnv *env = turi_env_new_sandboxed();
    if (!env) { fail("mixed-caps", "env alloc"); return; }
    turi_env_allow(env, TURI_CAP_ASYNC);

    TuriValue r1 = turi_eval(env, "(async (fn [] :int 42))");
    if (r1.tag == TURI_ERROR)
        fail("mixed-caps/async-allowed", r1.as_error ? r1.as_error : "(null)");
    else
        pass("mixed-caps/async-allowed", NULL);

    TuriValue r2 = turi_eval(env, "(println 1)");
    if (r2.tag == TURI_ERROR && r2.as_error && strstr(r2.as_error, "not allowed"))
        pass("mixed-caps/io-denied", r2.as_error);
    else
        fail("mixed-caps/io-denied", "println was not refused for want of TURI_CAP_IO");

    turi_env_free(env);
}

/* Verify turi_env_set_fuel and turi_env_set_max_depth are callable. */
static void run_api_smoke_tests(void) {
    TuriEnv *env = turi_env_new_sandboxed();
    if (!env) { fail("api-smoke", "env alloc"); return; }

    turi_env_set_fuel(env, 1000000u);
    turi_env_allow(env, TURI_CAP_IO);
    TuriValue r = turi_eval(env, "(+ 1 2)");
    if (r.tag == TURI_INT && r.as_int == 3) pass("api-smoke/set-fuel", NULL);
    else fail("api-smoke/set-fuel", "expected 3");

    turi_env_set_max_depth(env, 512);
    TuriValue r2 = turi_eval(env, "(+ 1 1)");
    if (r2.tag == TURI_INT && r2.as_int == 2) pass("api-smoke/set-depth", NULL);
    else fail("api-smoke/set-depth", "expected 2");

    turi_env_free(env);
}

int main(void) {
    turi_init(false);

    /* Locate fixture directory relative to working directory (project root). */
    const char *fixture_dir = "tests/fixtures/sandbox";

    struct { const char *file; const char *expect; } fixtures[] = {
        { "sb-inline-c.tur",       "inline-C not allowed"     },
        { "sb-println.tur",        "builtin not allowed"      },
        { "sb-dlopen.tur",         "builtin not allowed"      },
        { "sb-async.tur",          "async not allowed"        },
        { "sb-raw-malloc.tur",     "builtin not allowed"      },
        { "sb-raw-free.tur",       "builtin not allowed"      },
        { "sb-ptr-deref.tur",      "builtin not allowed"      },
        { "sb-ptr-write.tur",      "builtin not allowed"      },
        { "sb-ptr-arith.tur",      "builtin not allowed"      },
        { "sb-raw-memset.tur",     "builtin not allowed"      },
        { "sb-raw-memcpy.tur",     "builtin not allowed"      },
        { "sb-unsafe-cast.tur",    "builtin not allowed"      },
        { "sb-reinterpret.tur",    "builtin not allowed"      },
        { "sb-transmute.tur",      "builtin not allowed"      },
        { "sb-import.tur",         "import not allowed"       },
        { "sb-step-limit.tur",     "step fuel exhausted"      },
        { "sb-depth-limit.tur",    "step fuel exhausted"      },
        /* security-audit-plan WP3 */
        { "sb-load.tur",           "load not allowed"         },
        { "sb-process-spawn.tur",  "requires capability proc" },
        { "sb-read-async.tur",     "requires capability io"   },
        { "sb-fopen-write.tur",    "requires capability fs"   },
        { "sb-getenv.tur",         "requires capability env"  },
        { "sb-r7rs-eval.tur",      "requires capability"      },
        { "sb-extern-printf.tur",  "ffi"                      },
        { "sb-extern-getenv.tur",  "ffi"                      },
    };

    size_t n = sizeof fixtures / sizeof fixtures[0];
    for (size_t i = 0; i < n; i++)
        run_fixture(fixture_dir, fixtures[i].file, fixtures[i].expect);

    check_table_sorted();
    check_every_native_classified();
    check_every_requirement_refused();
    check_grant_admits();
    check_explicit_override();
    check_pure_still_works();
    check_host_exit_is_an_error();

    check_handle_forgery_refused();
    check_handles_still_round_trip();
    check_handle_table();

    run_mixed_caps_test();
    run_api_smoke_tests();

    printf("\nsummary: %d passed, %d failed\n", passes, failures);
    return failures == 0 ? 0 : 1;
}
