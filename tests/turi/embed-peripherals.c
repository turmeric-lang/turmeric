/* libturi-per-embed-env-and-peripherals Gaps 1-8: C embedding test harness.
 *
 * Exercises the per-embed-env peripheral API added for multi-script embedders:
 *   Gap 1 -- turi_register_default_native / turi_env_new_with_natives
 *   Gap 2 -- turi_env_reset
 *   Gap 3 -- turi_env_set_diag_sink
 *   Gap 4 -- turi_env_set_module_base_dir
 *   Gap 5 -- turi_env_register_native_ex (ud finalizer fired at env free)
 *   Gap 6 -- closure origin-env tag (debug-only; exercised, not assert-tripped)
 *   Gap 7 -- turi_env_set_interpret_mode (per-env mode snapshot)
 *   Gap 8 -- turi_env_set_shared_spice_image (borrowed image not double-freed)
 *   Gap 9 -- turi_env_set_search_path_for (imports resolve as `tur run` would)
 *            and turi_env_set_toplevel_imports (a turn may import)
 *
 * Compile with:
 *   cmake --build build --target tur_embed_peripherals
 *   ./build/tur_embed_peripherals
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "turi/eval.h"

static int failures = 0;

#define CHECK(cond, msg) do {                                   \
    if (cond) { printf("PASS [%s]\n", msg); }                   \
    else { fprintf(stderr, "FAIL [%s]\n", msg); failures++; }   \
} while (0)

/* --- Gap 1: a native that returns a fixed sentinel ---------------------- */
static TuriValue native_seven(TuriEnv *env, TuriValue *args, uint32_t n, void *ud) {
    (void)env; (void)args; (void)n;
    return turi_int((int64_t)(intptr_t)ud);
}

static void test_default_natives(void) {
    turi_register_default_native("embed-magic", native_seven, (void *)(intptr_t)7);

    /* Every env built after registration sees the native, with no per-env
     * re-registration. */
    TuriEnv *a = turi_env_new();
    TuriEnv *b = turi_env_new();
    TuriValue va = turi_eval(a, "(embed-magic)");
    TuriValue vb = turi_eval(b, "(embed-magic)");
    CHECK(va.tag == TURI_INT && va.as_int == 7, "default native visible on env A");
    CHECK(vb.tag == TURI_INT && vb.as_int == 7, "default native visible on env B");
    turi_env_free(a);
    turi_env_free(b);

    /* Explicit table via turi_env_new_with_natives. */
    TuriNativeSpec specs[] = {
        { "embed-explicit", native_seven, (void *)(intptr_t)42, NULL },
    };
    TuriEnv *c = turi_env_new_with_natives(specs, 1);
    TuriValue vc = turi_eval(c, "(embed-explicit)");
    CHECK(vc.tag == TURI_INT && vc.as_int == 42, "explicit native installed");
    /* Default natives also present on the with-natives env. */
    TuriValue vc2 = turi_eval(c, "(embed-magic)");
    CHECK(vc2.tag == TURI_INT && vc2.as_int == 7, "default native present on with-natives env");
    turi_env_free(c);

    turi_clear_default_natives();
    TuriEnv *d = turi_env_new();
    TuriValue vd = turi_eval(d, "(embed-magic)");
    CHECK(vd.tag == TURI_ERROR, "default native gone after clear");
    turi_env_free(d);
}

/* --- Gap 2: reset drops user defns but keeps registered natives --------- */
static void test_reset(void) {
    TuriEnv *env = turi_env_new_with_natives(
        (TuriNativeSpec[]){ { "embed-magic", native_seven, (void *)(intptr_t)7, NULL } }, 1);

    /* Install a user defn; both it and the native resolve. */
    turi_eval(env, "(defn user-fn [x :int] :int (* x x))");
    TuriValue r1 = turi_eval(env, "(user-fn 5)");
    CHECK(r1.tag == TURI_INT && r1.as_int == 25, "user defn works before reset");

    turi_env_reset(env);

    /* After reset the native survives... */
    TuriValue r2 = turi_eval(env, "(embed-magic)");
    CHECK(r2.tag == TURI_INT && r2.as_int == 7, "native survives reset");
    /* ...but the user defn is gone. */
    TuriValue r3 = turi_eval(env, "(user-fn 5)");
    CHECK(r3.tag == TURI_ERROR, "user defn dropped by reset");

    /* The same name can now be redefined cleanly (no stale shadow). */
    turi_eval(env, "(defn user-fn [x :int] :int (+ x 1))");
    TuriValue r4 = turi_eval(env, "(user-fn 5)");
    CHECK(r4.tag == TURI_INT && r4.as_int == 6, "name redefinable after reset");

    turi_env_free(env);
}

/* --- Gap 3: diagnostics route to the env's sink, not stderr ------------- */
static int   sink_hits = 0;
static int   sink_last_level = -1;
static char  sink_last_msg[256];

static void my_sink(TuriEnv *env, int level, const char *code, const char *file,
                    uint32_t line, uint32_t col_start, uint32_t col_end,
                    const char *message, void *ud) {
    (void)env; (void)code; (void)file; (void)line; (void)col_start; (void)col_end;
    sink_hits++;
    sink_last_level = level;
    snprintf(sink_last_msg, sizeof(sink_last_msg), "%s", message ? message : "");
    int *counter = (int *)ud;
    if (counter) (*counter)++;
}

static void test_diag_sink(void) {
    int ud_counter = 0;
    TuriEnv *env = turi_env_new();
    turi_env_set_diag_sink(env, my_sink, &ud_counter);

    sink_hits = 0;
    /* A let binding with no initializer is an elaboration diagnostic (a real
     * compile error, unlike an unbound call which interpret-mode defers to a
     * runtime error with no diagnostic). */
    TuriValue r = turi_eval(env, "(let [x] x)");
    CHECK(r.tag == TURI_ERROR, "bad eval returns error");
    CHECK(sink_hits > 0, "diagnostic delivered to sink");
    CHECK(ud_counter > 0, "sink user-data threaded through");

    /* Clearing the sink restores normal routing -- the next diagnostic goes to
     * stderr (the line below is expected output), not the sink. */
    turi_env_set_diag_sink(env, NULL, NULL);
    int hits_before = sink_hits;
    turi_eval(env, "(let [y] y)");
    CHECK(sink_hits == hits_before, "no sink delivery after clear");

    turi_env_free(env);
}

/* --- Gap 4: setter resolves imports relative to the chosen base dir ------ */
static void test_module_base_dir(void) {
    TuriEnv *env = turi_env_new();
    /* Set then re-set to confirm the owned string is freed/replaced cleanly.
     * No import is performed here -- the point is exercising the setter and its
     * free path under ASan (turi_env_free reclaims the owned copy). */
    turi_env_set_module_base_dir(env, "/tmp/does-not-matter");
    turi_env_set_module_base_dir(env, "/tmp/replaced");
    turi_env_set_module_base_dir(env, NULL);
    turi_env_set_module_base_dir(env, "/tmp/final");
    CHECK(1, "module base dir setter exercised (set/replace/clear/set)");
    turi_env_free(env);
}

/* --- Gap 5: native ud finalizer fires exactly once at env free ---------- */
/* Reads through its ud pointer (an int*) so the test can confirm the native
 * closed over the embedder's object. */
static TuriValue native_deref(TuriEnv *env, TuriValue *args, uint32_t n, void *ud) {
    (void)env; (void)args; (void)n;
    return turi_int(ud ? *(int *)ud : -1);
}

static int free_ud_calls = 0;
static void *free_ud_seen = NULL;

static void my_free_ud(void *ud) {
    free_ud_calls++;
    free_ud_seen = ud;
}

static void test_native_finalizer(void) {
    /* A heap-owned "script object" the native closes over; the env owns its
     * lifetime via the finalizer. */
    int *script_obj = (int *)malloc(sizeof(int));
    *script_obj = 99;

    free_ud_calls = 0;
    free_ud_seen  = NULL;

    TuriEnv *env = turi_env_new();
    turi_env_register_native_ex(env, "embed-obj", native_deref, script_obj, my_free_ud);

    /* The native is callable and sees its ud (native_seven returns (int)ud). */
    TuriValue r = turi_eval(env, "(embed-obj)");
    CHECK(r.tag == TURI_INT && r.as_int == 99, "native_ex ud visible to native");
    CHECK(free_ud_calls == 0, "finalizer not fired before env free");

    /* reset keeps natives -- and therefore must NOT fire the finalizer. */
    turi_env_reset(env);
    CHECK(free_ud_calls == 0, "finalizer survives reset (native survives)");

    turi_env_free(env);
    CHECK(free_ud_calls == 1, "finalizer fired exactly once at env free");
    CHECK(free_ud_seen == script_obj, "finalizer received the registered ud");

    free(script_obj);  /* the finalizer freed nothing in this test; we own the buffer */
}

/* --- Gap 6: a closure obtained then re-applied in the SAME env is fine ---- */
static void test_closure_origin_same_env(void) {
    /* The debug-only origin tag must NOT trip for legitimate same-env reuse:
     * obtain a closure value, then call it repeatedly via turi_call. */
    TuriEnv *env = turi_env_new();
    turi_eval(env, "(defn dbl [x :int] :int (* x 2))");
    TuriValue fn = turi_env_get(env, "dbl");
    CHECK(fn.tag == TURI_CLOSURE, "closure value retrieved");

    TuriValue a2 = turi_int(21);
    TuriValue r1 = turi_call(env, fn, &a2, 1);
    TuriValue r2 = turi_call(env, fn, &a2, 1);  /* second apply: tag must match */
    CHECK(r1.tag == TURI_INT && r1.as_int == 42, "closure first apply ok");
    CHECK(r2.tag == TURI_INT && r2.as_int == 42, "closure re-apply in same env ok (origin tag matches)");

    turi_env_free(env);
}

/* --- Gap 7: per-env interpret-mode bit is independent across envs --------- */
static void test_interpret_mode(void) {
    TuriEnv *a = turi_env_new();
    TuriEnv *b = turi_env_new();
    /* Both default to interpret mode -- a registered native resolves at runtime
     * on each, even after the other env was created (which previously could not
     * matter since the flag was a single global; here we just assert the bit is
     * honoured per env). */
    turi_env_register_native(a, "amark", native_seven, (void *)(intptr_t)1);
    turi_env_register_native(b, "bmark", native_seven, (void *)(intptr_t)2);
    TuriValue ra = turi_eval(a, "(amark)");
    TuriValue rb = turi_eval(b, "(bmark)");
    CHECK(ra.tag == TURI_INT && ra.as_int == 1, "env A native resolves under per-env interpret mode");
    CHECK(rb.tag == TURI_INT && rb.as_int == 2, "env B native resolves under per-env interpret mode");

    /* Toggle is a no-op for resolution here but exercises the setter + restore. */
    turi_env_set_interpret_mode(a, true);
    TuriValue ra2 = turi_eval(a, "(amark)");
    CHECK(ra2.tag == TURI_INT && ra2.as_int == 1, "setter does not disturb interpret-mode eval");

    turi_env_free(a);
    turi_env_free(b);
}

/* --- Gap 8: a borrowed (NULL-detach) shared image leaves env free clean --- */
static void test_shared_spice_image(void) {
    /* Without a real loaded image we exercise the borrow flag's free path:
     * detaching with NULL must be a no-op, and a borrowed env must not attempt
     * to free an image it does not own.  (A NULL image just clears the flag.) */
    TuriEnv *env = turi_env_new();
    turi_env_set_shared_spice_image(env, NULL);  /* detach: no-op, flag cleared */
    CHECK(1, "shared spice image setter exercised (NULL detach)");
    turi_env_free(env);  /* must not double-free under ASan */
}

/* --- Typed native registration: curated facade over a non-:int native ----
 * Regression test for
 * docs/archive/history/untyped-native-registration-blocks-curated-facades.md.
 * A native whose runtime result is :float must be wrappable by a defn that
 * declares the honest :float return type -- without the typed registry the
 * wrapper failed elaboration (TUR-E0707) because the call was typed :int. */
static TuriValue native_get_x(TuriEnv *env, TuriValue *args, uint32_t n, void *ud) {
    (void)env; (void)args; (void)n; (void)ud;
    return turi_float(3.5);
}
static TuriValue native_label(TuriEnv *env, TuriValue *args, uint32_t n, void *ud) {
    (void)env; (void)args; (void)n; (void)ud;
    return turi_cstr("hi");
}

static void test_typed_native(void) {
    turi_register_default_native_typed("my-get-x", native_get_x, NULL, TUR_NRT_FLOAT);
    turi_register_default_native_typed("my-label", native_label, NULL, TUR_NRT_CSTR);

    TuriEnv *env = turi_env_new();

    /* Direct call returns the float value. */
    TuriValue dx = turi_eval(env, "(my-get-x)");
    CHECK(dx.tag == TURI_FLOAT && dx.as_float == 3.5,
          "typed float native: direct call returns float");

    /* The curated facade -- a wrapper declaring :float -- now elaborates. */
    TuriValue def = turi_eval(env, "(defn wrap-x [] :float (my-get-x))");
    CHECK(def.tag != TURI_ERROR, "typed float native: :float wrapper elaborates");
    TuriValue wx = turi_eval(env, "(wrap-x)");
    CHECK(wx.tag == TURI_FLOAT && wx.as_float == 3.5,
          "typed float native: wrapper returns float");

    /* A :cstr native wraps cleanly too. */
    TuriValue defc = turi_eval(env, "(defn wrap-label [] :cstr (my-label))");
    CHECK(defc.tag != TURI_ERROR, "typed cstr native: :cstr wrapper elaborates");

    turi_env_free(env);

    /* Per-env typed registration feeds the same process-global registry. */
    TuriEnv *e2 = turi_env_new();
    turi_env_register_native_typed(e2, "env-x", native_get_x, NULL, TUR_NRT_FLOAT);
    TuriValue d2 = turi_eval(e2, "(defn w2 [] :float (env-x))");
    CHECK(d2.tag != TURI_ERROR, "env-scoped typed native: :float wrapper elaborates");
    turi_env_free(e2);

    turi_clear_default_natives();
    tur_native_sig_clear();
}

/* --- Gap 9: an embedder's imports resolve the way `tur run <file>` does -- */
/* The notebook spice evaluates cells for a .tur.md file and could import
 * nothing: the env searched only the process cwd and the stdlib
 * (docs/archive/notebook-eval-no-module-base-dir.md).  Build a workspace in a
 * scratch dir -- a spice `app` with a module of its own, a `:path` dep `dep`,
 * a workspace sibling `sib`, and a module beside the note -- and import all
 * four through one call. */
static void write_file(const char *dir, const char *rel, const char *text) {
    char p[1024];
    snprintf(p, sizeof(p), "%s/%s", dir, rel);
    FILE *f = fopen(p, "w");
    if (!f) { fprintf(stderr, "FAIL [cannot write %s]\n", p); failures++; return; }
    fputs(text, f);
    fclose(f);
}

/* MinGW's mkdir takes no mode, and its CRT does not promise mkdtemp; the
 * Windows job builds this harness but does not run it. */
#if defined(_WIN32)
#include <direct.h>
#define EP_MKDIR(p) _mkdir(p)
static char *ep_mkdtemp(char *tmpl) {
    if (!_mktemp(tmpl)) return NULL;
    return _mkdir(tmpl) == 0 ? tmpl : NULL;
}
#else
#define EP_MKDIR(p) mkdir((p), 0755)
#define ep_mkdtemp mkdtemp
#endif

static void make_dir(const char *dir, const char *rel) {
    char p[1024];
    snprintf(p, sizeof(p), "%s/%s", dir, rel);
    EP_MKDIR(p);
}

static void test_search_path_for(void) {
    char root[512];
#if defined(_WIN32)
    const char *tmp = getenv("TEMP") ? getenv("TEMP") : ".";
#else
    const char *tmp = "/tmp";
#endif
    snprintf(root, sizeof(root), "%s/tur-embed-search-XXXXXX", tmp);
    if (!ep_mkdtemp(root)) { CHECK(0, "mkdtemp for the search-path workspace"); return; }
    const char *dirs[] = { "ws", "ws/app", "ws/app/src", "ws/app/src/app",
                           "ws/app/notes", "ws/dep", "ws/dep/src", "ws/dep/src/dep",
                           "ws/sib", "ws/sib/src", "ws/sib/src/sib", "loose" };
    for (size_t i = 0; i < sizeof(dirs) / sizeof(dirs[0]); i++) make_dir(root, dirs[i]);
    write_file(root, "ws/build.tur",
               "(defpackage ws :name \"ws\" :members [\"app\" \"sib\"])\n");
    write_file(root, "ws/app/build.tur",
               "(defpackage app :name \"app\"\n"
               "  :spices #map{\"dep\" #map{:path \"../dep\"}})\n");
    write_file(root, "ws/dep/build.tur", "(defpackage dep :name \"dep\")\n");
    write_file(root, "ws/sib/build.tur", "(defpackage sib :name \"sib\")\n");
    write_file(root, "ws/app/src/app/core.tur",
               "(defmodule app/core (export core-val) (defn core-val [] : int 30))\n");
    write_file(root, "ws/dep/src/dep/lib.tur",
               "(defmodule dep/lib (export dep-val) (defn dep-val [] : int 7))\n");
    write_file(root, "ws/sib/src/sib/util.tur",
               "(defmodule sib/util (export sib-val) (defn sib-val [] : int 4))\n");
    write_file(root, "ws/app/notes/beside.tur",
               "(defmodule beside (export beside-val) (defn beside-val [] : int 1))\n");

    char note[1024];
    snprintf(note, sizeof(note), "%s/ws/app/notes/walkthrough.tur.md", root);
    const char *prog =
        "(import app/core :refer [core-val])\n"
        "(import dep/lib :refer [dep-val])\n"
        "(import sib/util :refer [sib-val])\n"
        "(import beside :refer [beside-val])\n"
        "(+ (core-val) (+ (dep-val) (+ (sib-val) (beside-val))))\n";

    /* Controls: without the search path nothing outside the cwd and stdlib
     * resolves, and without toplevel imports a bare `(import ...)` is still
     * the defmodule error it is in a compiled file. */
    TuriEnv *bare = turi_env_new();
    turi_env_set_toplevel_imports(bare, true);
    TuriValue vb = turi_eval(bare, "(import dep/lib :refer [dep-val]) (dep-val)");
    CHECK(turi_is_error(vb), "search path: a bare env cannot import a spice module");
    turi_env_free(bare);
    TuriEnv *strict = turi_env_new();
    turi_env_set_search_path_for(strict, note);
    TuriValue vs = turi_eval(strict, "(import dep/lib :refer [dep-val]) (dep-val)");
    CHECK(turi_is_error(vs), "toplevel imports: off by default");
    TuriValue vm = turi_eval(strict,
        "(defmodule probe (import dep/lib :refer [dep-val])"
        " (export probe-val) (defn probe-val [] : int (dep-val)))");
    CHECK(!turi_is_error(vm), "search path: a defmodule's import resolves without them");
    turi_env_free(strict);

    TuriEnv *env = turi_env_new();
    turi_env_set_toplevel_imports(env, true);
    int n = turi_env_set_search_path_for(env, note);
    CHECK(n == 3, "search path: own src/, the :path dep and the workspace sibling");
    TuriValue v = turi_eval(env, prog);
    CHECK(!turi_is_error(v) && v.tag == TURI_INT && v.as_int == 42,
          "search path: all four imports resolve (30+7+4+1)");
    /* The Jupyter model: a referred name and an alias outlive their turn, and
     * re-running an import turn is harmless. */
    TuriValue v1 = turi_eval(env, "(import dep/lib :as d)");
    TuriValue v3 = turi_eval(env, "(+ (d/dep-val) (core-val))");
    CHECK(!turi_is_error(v1) && !turi_is_error(v3) && v3.as_int == 37,
          "toplevel imports: :as and :refer resolve on a later turn");
    TuriValue v4 = turi_eval(env, prog);
    CHECK(!turi_is_error(v4) && v4.as_int == 42, "toplevel imports: a re-run turn");
    TuriValue v5 = turi_eval(env, "(import dep/lib :refer [no-such])");
    CHECK(turi_is_error(v5), "toplevel imports: an unexported :refer is an error");

    /* It survives a reset, and a second call replaces the owned list. */
    turi_env_reset(env);
    TuriValue v2 = turi_eval(env, "(import dep/lib :refer [dep-val]) (dep-val)");
    CHECK(!turi_is_error(v2) && v2.as_int == 7, "search path: survives turi_env_reset");
    n = turi_env_set_search_path_for(env, note);
    CHECK(n == 3, "search path: a second call replaces the list");

    /* Outside any build.tur only the base dir is set. */
    char loose[1024];
    snprintf(loose, sizeof(loose), "%s/loose/x.tur", root);
    n = turi_env_set_search_path_for(env, loose);
    CHECK(n == 0, "search path: no enclosing build.tur adds no dirs");
    n = turi_env_set_search_path_for(env, NULL);
    CHECK(n == 0, "search path: NULL clears it");
    turi_env_free(env);

    /* Leave nothing behind. */
    const char *files[] = { "ws/build.tur", "ws/app/build.tur", "ws/dep/build.tur",
                            "ws/sib/build.tur", "ws/app/src/app/core.tur",
                            "ws/dep/src/dep/lib.tur", "ws/sib/src/sib/util.tur",
                            "ws/app/notes/beside.tur" };
    char p[1024];
    for (size_t i = 0; i < sizeof(files) / sizeof(files[0]); i++) {
        snprintf(p, sizeof(p), "%s/%s", root, files[i]);
        unlink(p);
    }
    for (size_t i = sizeof(dirs) / sizeof(dirs[0]); i-- > 0;) {
        snprintf(p, sizeof(p), "%s/%s", root, dirs[i]);
        rmdir(p);
    }
    rmdir(root);
}

int main(void) {
    turi_init(false);

    test_default_natives();
    test_typed_native();
    test_reset();
    test_diag_sink();
    test_module_base_dir();
    test_native_finalizer();
    test_closure_origin_same_env();
    test_interpret_mode();
    test_shared_spice_image();
    test_search_path_for();

    if (failures == 0) {
        printf("\nAll embed-peripheral tests passed.\n");
        return 0;
    }
    fprintf(stderr, "\n%d test(s) FAILED.\n", failures);
    return 1;
}
